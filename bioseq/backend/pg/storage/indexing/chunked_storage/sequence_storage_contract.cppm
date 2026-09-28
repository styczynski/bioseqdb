/**
 * @file sequence_storage_contract.cppm
 * @brief Store a sequence index in one row or a table index in numbered chunks.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h" // RelnameGetRelid
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_opclass_d.h"
#include "catalog/pg_sequence.h"
#include "catalog/pg_type_d.h"
#include "commands/sequence.h"
#include "commands/tablespace.h"
#include "executor/instrument.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "storage/buffile.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/procarray.h"
#include "storage/relfilelocator.h"
#include "storage/shm_toc.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include "common/relpath.h"
#include "miscadmin.h"
#include "port.h"
#include "port/atomics.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#ifdef NDEBUG
#include <libassert/assert-macros.hpp>
#else
#include <libassert/assert.hpp>
#endif

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <new>

export module bioseq.postgres.storage.indexing.chunked_storage:sequence_storage_contract;
import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.postgres.storage.scanner.lazy_binding; // Read sequence fragments through a PostgreSQL snapshot.
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // Storage adapter requirements.
import bioseq.indexing.minimizer_hashmap; // Minimizer index header and bucket format.
import bioseq.indexing.minimizer_fragment_image; // Minimizer fragment format with a fixed byte order.
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // Decode sequence bases with SIMD.
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import bioseq.instrumentation.clock;
import bioseq.postgres.column.table_access;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.storage.physical;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
export namespace bioseq::storage::indexing {

/// Borrow a vector of positions and present it as one batch.
struct position_batch_cursor {
    const std::vector<std::uint64_t>* v = nullptr;
    /// Return  the number of positions in the borrowed vector.
    [[nodiscard]] std::uint64_t total_count() const {
        return v->size();
    }
    /// Call the callback once with all positions, or skip it when the vector is empty.
    template <class p_callback> void for_each_batch(p_callback&& f) const {
        if (!v->empty())
            f(std::span<const std::uint64_t>(v->data(), v->size()));
    }
};

/// Store one sequence index as a single PostgreSQL row.
struct sequence_index_storage {
    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid,
        user_relid_ = InvalidOid;
    seq_caches* read_caches_ = nullptr;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    std::uint8_t bucket_idx_ = 0;
    std::uint32_t shard_idx_ = 0;
    std::uint64_t query_key_ = 0;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t decoded_units_ = 0;
    mutable std::size_t decoded_bytes_ = 0;
    mutable bool decoded_valid_ = false;
    std::size_t planned_bytes_ = std::numeric_limits<std::size_t>::max();

    sequence_index_storage() = default;
    sequence_index_storage(const sequence_index_storage&) = delete;
    sequence_index_storage& operator=(const sequence_index_storage&) = delete;
    sequence_index_storage(sequence_index_storage&& o) noexcept {
        steal_(std::move(o));
    }
    sequence_index_storage& operator=(sequence_index_storage&&) = delete;
    ~sequence_index_storage() {
        release_();
    }
    /// Move the storage identity and take ownership of the buffer fontext.
    /// Clear the source's owning pointers so it can be destroyed.
    void steal_(sequence_index_storage&& o) noexcept {
        db_oid_ = o.db_oid_;
        tablespace_oid_ = o.tablespace_oid_;
        main_relfilenode_ = o.main_relfilenode_;
        user_relid_ = o.user_relid_;
        read_caches_ = o.read_caches_;
        column_attno_ = o.column_attno_;
        index_name_ = o.index_name_;
        bucket_idx_ = o.bucket_idx_;
        shard_idx_ = o.shard_idx_;
        query_key_ = o.query_key_;
        own_mctx_ = o.own_mctx_;
        index_relid_ = o.index_relid_;
        decoded_ = o.decoded_;
        decoded_units_ = o.decoded_units_;
        decoded_bytes_ = o.decoded_bytes_;
        decoded_valid_ = o.decoded_valid_;
        planned_bytes_ = o.planned_bytes_;
        o.own_mctx_ = nullptr;
        o.decoded_ = nullptr;
        o.decoded_units_ = 0;
        o.index_relid_ = InvalidOid;
        o.decoded_valid_ = false;
    }
    /// Assert that close or error handling has cleared the context pointer.
    void release_() noexcept {
        DEBUG_ASSERT(
            own_mctx_ == nullptr,
            "sequence_index_storage must be close_checked or abandoned before destruction");
        own_mctx_ = nullptr;
        decoded_ = nullptr;
    }
    /// Delete the buffer memory context, converting PostgreSQL errors to C++ exceptions.
    void close_checked() {
        if (own_mctx_ != nullptr) {
            MemoryContext finished_context = std::exchange(own_mctx_, nullptr);
            decoded_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_context] { MemoryContextDelete(finished_context); });
        }
    }
    /// Clear handles after a PostgreSQL error and leave memory release  to, transaction cleanup
    void abandon_after_pg_error_noexcept() noexcept {
        own_mctx_ = nullptr;
        decoded_ = nullptr;
        decoded_units_ = 0;
        decoded_bytes_ = 0;
        decoded_valid_ = false;
        index_relid_ = InvalidOid;
    }

    /// Report corrupt index data as a PostgreSQL error wrapped in a C++ exception.
    [[noreturn]] void index_error(std::string_view msg) const {
        bioseq::postgres::support::invoke_pg_as_cpp([msg] {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq index: %.*s", static_cast<int>(msg.size()), msg.data())));
        });
        pg_unreachable();
    }

    /// Build the index table name from the parent table, column, index and shard.
    [[nodiscard]] std::string this_table_name() const {
        return format_seqidx_name(index_name_,
                                  db_oid_,
                                  tablespace_oid_,
                                  main_relfilenode_,
                                  column_attno_,
                                  bucket_idx_,
                                  shard_idx_);
    }
    /// Find the index table, creating it when requested.
    [[nodiscard]] Oid resolve_relid_(bool create_if_missing) const {
        if (OidIsValid(index_relid_))
            return index_relid_;
        const std::string nm = this_table_name();
        const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create_if_missing) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "query_key BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (query_key)) USING heap;",
                pk,
                quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(
                fmt::format(
                    "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                    quote_identifier(nm.c_str()))
                    .c_str());
            rid = get_relname_relid(nm.c_str(), ns);
            if (!OidIsValid(rid))
                index_error("failed to create inline seqidx table");
        }
        index_relid_ = rid;
        return rid;
    }
    /// Open the index table with the requested lock, or return null if it is absent.
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create);
        if (!OidIsValid(rid))
            return nullptr;
        return bioseq::postgres::support::try_open_postgres_relation(rid, lock);
    }
    /// Find the index table while converting PostgreSQL errors to C++ exceptions.
    [[nodiscard]] Oid resolve_relid_for_read_checked_() const {
        if (OidIsValid(index_relid_)) {
            return index_relid_;
        }
        // Keep the string outside PG_TRY so a PostgreSQL error can be converted
        // to a C++ exception before the string is destroyed.
        const std::string table_name = this_table_name();
        const Oid namespace_oid =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return bioseq_internal_ns(); });
        const Oid relation_oid =
            bioseq::postgres::support::invoke_pg_as_cpp([&table_name, namespace_oid] {
                inject_inline_sequence_pg_leaf_failure(
                    inline_sequence_pg_leaf_failure::relation_lookup);
                return get_relname_relid(table_name.c_str(), namespace_oid);
            });
        index_relid_ = relation_oid;
        return relation_oid;
    }
    /// Open  the index table for reading, converting PostgreSQL errors to C++ exceptions.
    [[nodiscard]] Relation open_for_read_checked_() const {
        const Oid relation_oid = resolve_relid_for_read_checked_();
        if (!OidIsValid(relation_oid)) {
            return nullptr;
        }
        return bioseq::postgres::support::invoke_pg_as_cpp([relation_oid] {
            return bioseq::postgres::support::try_open_postgres_relation(relation_oid,
                                                                         AccessShareLock);
        });
    }
    /// Grow the read buffer in the handle's PostgreSQL memory conext.
    void ensure_buffer_checked_(const std::size_t units) const {
        if (decoded_units_ >= units && decoded_ != nullptr) {
            return;
        }
        MemoryContext owner = own_mctx_;
        std::int64_t* old_buffer = decoded_;
        std::int64_t* replacement =
            bioseq::postgres::support::invoke_pg_as_cpp([owner, old_buffer, units] {
                MemoryContext previous = MemoryContextSwitchTo(owner);
                auto* result =
                    old_buffer == nullptr
                        ? static_cast<std::int64_t*>(palloc(units * sizeof(std::int64_t)))
                        : static_cast<std::int64_t*>(
                              repalloc(old_buffer, units * sizeof(std::int64_t)));
                MemoryContextSwitchTo(previous);
                return result;
            });
        decoded_ = replacement;
        decoded_units_ = units;
    }
    /// Copy this sequence's stored index bytes into the reusable read buffer.
    void load_() const {
        Relation rel = open_for_read_checked_();
        if (rel == nullptr) {
            decoded_bytes_ = 0;
            decoded_valid_ = true;
            return;
        }
        const Oid pk = bioseq::postgres::support::invoke_pg_as_cpp(
            [rel] { return bioseq::postgres::support::find_primary_key_oid(rel); });
        ScanKeyData key{};
        const Datum query_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw();
        bioseq::postgres::support::invoke_pg_as_cpp([&key, query_key] {
            ScanKeyInit(&key, 1, BTEqualStrategyNumber, F_INT8EQ, query_key);
        });
        SysScanDesc scan = bioseq::postgres::support::invoke_pg_as_cpp([rel, pk, &key] {
            return systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, &key);
        });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp([scan] {
            inject_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure::tuple_read);
            return systable_getnext(scan);
        });
        if (HeapTupleIsValid(tuple)) {
            bool is_null = false;
            TupleDesc tuple_desc = RelationGetDescr(rel);
            Datum datum =
                bioseq::postgres::support::invoke_pg_as_cpp([tuple, tuple_desc, &is_null] {
                    return heap_getattr(tuple, 3, tuple_desc, &is_null);
                });
            if (is_null) {
                index_error("inline sequence index blob is NULL");
            }
            auto raw_detoasted = bioseq::postgres::support::invoke_pg_as_cpp([datum] {
                inject_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure::detoast);
                return bioseq::postgres::support::detoast_packed_as<bytea>(
                    bioseq::postgres::support::pg_datum::from_raw(datum));
            });
            bytea* raw = raw_detoasted.ptr;
            const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
            const std::size_t units = (payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
            ensure_buffer_checked_(units == 0 ? 1 : units);
            if (payload > 0)
                std::memcpy(decoded_, VARDATA_ANY(raw), payload);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [&raw_detoasted] { raw_detoasted.free_if_copy(); });
            decoded_bytes_ = payload;
        } else
            decoded_bytes_ = 0;
        bioseq::postgres::support::invoke_pg_as_cpp([scan] { systable_endscan(scan); });
        bioseq::postgres::support::invoke_pg_as_cpp([rel] {
            inject_inline_sequence_pg_leaf_failure(inline_sequence_pg_leaf_failure::close);
            relation_close(rel, AccessShareLock);
        });
        decoded_valid_ = true;
    }
    /// Return one for a nonempty index payload, or zero otherwise.
    [[nodiscard]] std::size_t number_of_chunks() const {
        return chunk_byte_size() == 0 ? 0 : 1;
    }
    /// Return the stored payload length, reading the row on first use.
    [[nodiscard]] std::size_t chunk_byte_size() const {
        if (!decoded_valid_)
            load_();
        return decoded_bytes_;
    }
    /// Read the index bytes through the handle's reusable buffer.
    /// The pointer remains valid until the buffer is reused or the handle closes.
    template <class p_value_type> [[nodiscard]] const p_value_type* read_chunk() {
        if (!decoded_valid_)
            load_();
        return reinterpret_cast<const p_value_type*>(decoded_);
    }
    /// Record the byte length expected from the next writer callback.
    void resize(std::size_t bytes) {
        if (bytes > k_max_index_chunk_size_bytes)
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq index inline: bytes exceed single-chunk MAX");
        planned_bytes_ = bytes;
    }
    /// No preparation is needed before reading this single-row index.
    void prefetch_chunk() {}
    /// Fill a temporary buffer and replace  this sequence's index row.
    /// Call resize first The callback receives chunk zero and must return zero.
    template <class p_value_type, class p_callback> void write_to_each(p_callback&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<p_callback, p_value_type>,
                      "inline write_to_each writer shape");
        if (planned_bytes_ == std::numeric_limits<std::size_t>::max())
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq index inline: write_to_each without resize");
        const std::size_t payload = planned_bytes_;
        if (payload % sizeof(p_value_type) != 0)
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq index inline: payload is not a whole number of elements");
        std::vector<char> buf(payload, 0);
        const std::size_t next =
            writer(0, reinterpret_cast<p_value_type*>(buf.data()), payload / sizeof(p_value_type));
        if (next != 0)
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq index inline: writer emitted >1 chunk");
        Relation rel = open_(true, RowExclusiveLock);
        // Replace the old row when rebuilding this sequence's index.
        {
            const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
            ScanKeyData k[1];
            ScanKeyInit(
                &k[0],
                1,
                BTEqualStrategyNumber,
                F_INT8EQ,
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_))
                    .raw());
            SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
            HeapTuple old_t = systable_getnext(s);
            if (HeapTupleIsValid(old_t))
                simple_heap_delete(rel, &old_t->t_self);
            systable_endscan(s);
        }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload));
        SET_VARSIZE(blob, VARHDRSZ + payload);
        if (payload > 0)
            std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::postgres::support::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(query_key_));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::postgres::support::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup);
        heap_freetuple(tup);
        pfree(blob);
        relation_close(rel, RowExclusiveLock);
        decoded_valid_ = false;
        planned_bytes_ = std::numeric_limits<std::size_t>::max();
    }
    /// Delete the index row for this sequence.
    void clear_entity() {
        const Oid rid = resolve_relid_(false);
        if (!OidIsValid(rid))
            return;
        std::string sql = fmt::format("DELETE FROM bioseq_internal.{} WHERE query_key = {}",
                                      quote_identifier(this_table_name().c_str()),
                                      (long long)query_key_);
        if (SPI_connect() == SPI_OK_CONNECT) {
            SPI_execute(sql.c_str(), false, 0);
            SPI_finish();
        }
        decoded_valid_ = false;
    }
    /// Drop the tables whose names identify shards of the named index.
    void clear(std::string_view name);
    /// Rebind this handle to one sequence row without reallocating its context.
    void reset_entity(const sequence_locator& loc, std::uint64_t sequence_rowno) {
        bucket_idx_ = static_cast<std::uint8_t>(loc.bucket_index);
        shard_idx_ = loc.shard_index;
        query_key_ = sequence_rowno;
        index_relid_ = InvalidOid;
        decoded_valid_ = false;
        decoded_bytes_ = 0;
    }
};

/// Store a table index as numbered PostgreSQL rows, one payload per chunk.
struct table_index_storage {
    Oid db_oid_ = InvalidOid, tablespace_oid_ = InvalidOid, main_relfilenode_ = InvalidOid,
        user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string_view index_name_{};
    seq_caches* read_caches_ = nullptr;
    MemoryContext own_mctx_ = nullptr;
    mutable Oid index_relid_ = InvalidOid;
    mutable std::int64_t* decoded_ = nullptr;
    mutable std::size_t decoded_units_ = 0, decoded_bytes_ = 0;
    mutable std::int64_t decoded_chunk_ = -1;
    std::size_t planned_bytes_for_chunk_ = 0;

    table_index_storage() = default;
    table_index_storage(const table_index_storage&) = delete;
    table_index_storage& operator=(const table_index_storage&) = delete;
    table_index_storage(table_index_storage&& o) noexcept {
        steal_(std::move(o));
    }
    table_index_storage& operator=(table_index_storage&&) = delete;
    ~table_index_storage() {
        release_();
    }
    /// Move the storage identity and take ownership of the buffer context.
    /// Clear the source's owning pointers so it can be destroyed.
    void steal_(table_index_storage&& o) noexcept {
        db_oid_ = o.db_oid_;
        tablespace_oid_ = o.tablespace_oid_;
        main_relfilenode_ = o.main_relfilenode_;
        user_relid_ = o.user_relid_;
        column_attno_ = o.column_attno_;
        index_name_ = o.index_name_;
        read_caches_ = o.read_caches_;
        own_mctx_ = o.own_mctx_;
        index_relid_ = o.index_relid_;
        decoded_ = o.decoded_;
        decoded_units_ = o.decoded_units_;
        decoded_bytes_ = o.decoded_bytes_;
        decoded_chunk_ = o.decoded_chunk_;
        planned_bytes_for_chunk_ = o.planned_bytes_for_chunk_;
        o.own_mctx_ = nullptr;
        o.decoded_ = nullptr;
        o.index_relid_ = InvalidOid;
        o.decoded_units_ = 0;
    }
    /// Assert that close or error handling has cleared the context pointer.
    void release_() noexcept {
        DEBUG_ASSERT(own_mctx_ == nullptr,
                     "table_index_storage must be close_checked or abandoned before destruction");
        own_mctx_ = nullptr;
        decoded_ = nullptr;
    }
    void close_checked() {
        if (own_mctx_ != nullptr) {
            MemoryContext finished_context = std::exchange(own_mctx_, nullptr);
            decoded_ = nullptr;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished_context] { MemoryContextDelete(finished_context); });
        }
    }
    void abandon_after_pg_error_noexcept() noexcept {
        own_mctx_ = nullptr;
        decoded_ = nullptr;
        decoded_units_ = 0;
        decoded_bytes_ = 0;
        decoded_chunk_ = -1;
        index_relid_ = InvalidOid;
    }
    /// Report a  tabe index, error and stop execution.
    [[noreturn]] void index_error(std::string_view msg) const {
        bioseq_log<logging_level::error, logging_target::postgres>("bioseq index: {}", msg);
        abort();
    }

    /// Build the index table name for one shard.
    [[nodiscard]] std::string this_table_name(std::int32_t shard = 0) const {
        return format_seqidxtab_name(
            index_name_, db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, shard);
    }
    /// Find the chunk table, creating it when requested.
    [[nodiscard]] Oid resolve_relid_(bool create) const {
        if (OidIsValid(index_relid_))
            return index_relid_;
        const std::string nm = this_table_name(0);
        const Oid ns = bioseq_internal_ns();
        Oid rid = get_relname_relid(nm.c_str(), ns);
        if (!OidIsValid(rid) && create) {
            const char* pk = persistence_kw_for(user_relid_);
            std::string ddl = fmt::format(
                "CREATE {} TABLE IF NOT EXISTS bioseq_internal.{} ("
                "chunk_no BIGINT NOT NULL, chunk_bytes INT NOT NULL, blob BYTEA NOT NULL, "
                "PRIMARY KEY (chunk_no)) USING heap;",
                pk,
                quote_identifier(nm.c_str()));
            run_internal_ddl(ddl.c_str());
            run_internal_ddl(
                fmt::format(
                    "ALTER TABLE bioseq_internal.{} ALTER COLUMN blob SET STORAGE EXTERNAL;",
                    quote_identifier(nm.c_str()))
                    .c_str());
            rid = get_relname_relid(nm.c_str(), ns);
        }
        index_relid_ = rid;
        return rid;
    }
    /// Open the chunk table with the requested lock, or return null if it is absent.
    [[nodiscard]] Relation open_(bool create, LOCKMODE lock) const {
        const Oid rid = resolve_relid_(create);
        if (!OidIsValid(rid))
            return nullptr;
        return bioseq::postgres::support::try_open_postgres_relation(rid, lock);
    }
    /// Grow the read buffer in the handle's PostgreSQL memory context.
    void ensure_buffer_(std::size_t units) const {
        if (decoded_units_ >= units && decoded_ != nullptr)
            return;
        MemoryContext previous_context = MemoryContextSwitchTo(own_mctx_);
        decoded_ = (decoded_ == nullptr)
                       ? (std::int64_t*)palloc(units * sizeof(std::int64_t))
                       : (std::int64_t*)repalloc(decoded_, units * sizeof(std::int64_t));
        decoded_units_ = units;
        MemoryContextSwitchTo(previous_context);
    }
    /// Count the stored chunk rows.
    [[nodiscard]] std::size_t number_of_chunks() const {
        Relation rel = open_(false, AccessShareLock);
        if (rel == nullptr)
            return 0;
        SysScanDesc s = systable_beginscan(rel, InvalidOid, false, nullptr, 0, nullptr);
        std::size_t n = 0;
        for (; systable_getnext(s) != nullptr;)
            ++n;
        systable_endscan(s);
        relation_close(rel, AccessShareLock);
        return n;
    }
    /// Return the stored byte length of one table chunk.
    [[nodiscard]] std::size_t chunk_byte_size(std::size_t chunk_no) const {
        Relation rel = open_(false, AccessShareLock);
        if (rel == nullptr)
            return 0;
        const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
        ScanKeyData k[1];
        ScanKeyInit(
            &k[0],
            1,
            BTEqualStrategyNumber,
            F_INT8EQ,
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        std::size_t out = 0;
        HeapTuple t = systable_getnext(s);
        if (HeapTupleIsValid(t)) {
            bool isnull = false;
            out = static_cast<std::size_t>(bioseq::postgres::support::pg_datum::to<std::int32_t>(
                bioseq::postgres::support::pg_datum::from_raw(
                    heap_getattr(t, 2, RelationGetDescr(rel), &isnull))));
        }
        systable_endscan(s);
        relation_close(rel, AccessShareLock);
        return out;
    }
    /// Copy a chunk into the reusable read buffer.
    /// The returned pointer remains valid until another chunk  is read or the handle closes.
    template <class p_value_type>
    [[nodiscard]] const p_value_type* read_chunk(std::size_t chunk_no) {
        if (decoded_ != nullptr && decoded_chunk_ == static_cast<std::int64_t>(chunk_no)) {
            return reinterpret_cast<const p_value_type*>(decoded_);
        }
        Relation rel = open_(false, AccessShareLock);
        if (rel == nullptr)
            index_error("table index relation missing");
        const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
        ScanKeyData k[1];
        ScanKeyInit(
            &k[0],
            1,
            BTEqualStrategyNumber,
            F_INT8EQ,
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(chunk_no)).raw());
        SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
        HeapTuple t = systable_getnext(s);
        if (!HeapTupleIsValid(t)) {
            systable_endscan(s);
            relation_close(rel, AccessShareLock);
            index_error("table chunk missing");
        }
        bool isnull = false;
        Datum d = heap_getattr(t, 3, RelationGetDescr(rel), &isnull);
        auto raw_detoasted = bioseq::postgres::support::detoast_packed_as<bytea>(
            bioseq::postgres::support::pg_datum::from_raw(d));
        bytea* raw = raw_detoasted.ptr;
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units = (payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        ensure_buffer_(units == 0 ? 1 : units);
        if (payload > 0)
            std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        raw_detoasted.free_if_copy();
        decoded_chunk_ = static_cast<std::int64_t>(chunk_no);
        decoded_bytes_ = payload;
        systable_endscan(s);
        relation_close(rel, AccessShareLock);
        return reinterpret_cast<const p_value_type*>(decoded_);
    }
    /// Record the byte length expected for the next table chunk.
    void resize(std::size_t bytes) {
        planned_bytes_for_chunk_ = bytes;
    }
    /// No preparation is needed before reading a table chunk.
    void prefetch_chunk(std::size_t) {}
    /// Write chunk zero using the size set by resize.
    template <class p_value_type, class p_callback> void write_to_each(p_callback&& writer) {
        write_to<p_value_type>(0, std::forward<p_callback>(writer));
    }
    /// Fill a temporary buffer and replace the requested chunk row
    template <class p_value_type, class p_callback>
    void write_to(std::size_t chunk_no, p_callback&& writer) {
        static_assert(bioseq::indexing::index_chunk_writer_fn<p_callback, p_value_type>,
                      "table write_to writer shape");
        const std::size_t payload = planned_bytes_for_chunk_;
        if (payload % sizeof(p_value_type) != 0)
            bioseq_log<logging_level::error, logging_target::postgres>(
                "bioseq index table: payload is not a whole number of elements");
        std::vector<char> buf(payload, 0);
        writer(
            chunk_no, reinterpret_cast<p_value_type*>(buf.data()), payload / sizeof(p_value_type));
        Relation rel = open_(true, RowExclusiveLock);
        {
            const Oid pk = bioseq::postgres::support::find_primary_key_oid(rel);
            ScanKeyData k[1];
            ScanKeyInit(
                &k[0],
                1,
                BTEqualStrategyNumber,
                F_INT8EQ,
                bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(chunk_no))
                    .raw());
            SysScanDesc s = systable_beginscan(rel, pk, OidIsValid(pk), nullptr, 1, k);
            HeapTuple old_t = systable_getnext(s);
            if (HeapTupleIsValid(old_t))
                simple_heap_delete(rel, &old_t->t_self);
            systable_endscan(s);
        }
        bytea* blob = reinterpret_cast<bytea*>(palloc(VARHDRSZ + payload));
        SET_VARSIZE(blob, VARHDRSZ + payload);
        if (payload > 0)
            std::memcpy(VARDATA(blob), buf.data(), payload);
        bioseq::postgres::support::pg_datum_list<3> values{};
        values.set(1, static_cast<std::int64_t>(chunk_no));
        values.set(2, static_cast<std::int32_t>(payload));
        values.set_raw(3, bioseq::postgres::support::pg_datum::from_pointer(blob));
        HeapTuple tup = values.form_tuple(RelationGetDescr(rel));
        CatalogTupleInsert(rel, tup);
        heap_freetuple(tup);
        pfree(blob);
        relation_close(rel, RowExclusiveLock);
        decoded_chunk_ = -1;
    }
    /// Visit each non-null sequence using a registered transaction snapshot.
    template <class p_callback> void for_each_sequence(p_callback&& loader);
    /// Drop the tables whose names identify shards of the named index.
    void clear(std::string_view name);
};

// Shared index table cleanup.
/// Report a failed cleanup operaion as a PostgreSQL error wrapped in a C++ exception.
[[noreturn]] inline void index_clear_spi_error_as_cpp(const char* stage, int status) {
    bioseq::postgres::support::invoke_pg_as_cpp([stage, status] {
        ereport(
            ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("bioseq index clear failed"),
             errdetail("stage=%s status=%d", stage, status),
             errhint("The transaction was aborted so stale index relations cannot be accepted.")));
    });
    pg_unreachable();
}

/// Select all index  tables for best-effort cleanup, or a prefix whose cleanup must succeed.
struct drop_index_relation_selector {
    enum class kind : std::uint8_t { all_index_tables_best_effort, prefix_strict };

    kind selected_kind;
    std::string_view prefix;

    /// Select best-effort cleanup of every recognized internal index table.
    [[nodiscard]] static constexpr drop_index_relation_selector all_index_tables() noexcept {
        return {.selected_kind = kind::all_index_tables_best_effort, .prefix = {}};
    }

    /// Require cleanup of index tables whose names start with this prefix.
    [[nodiscard]] static constexpr drop_index_relation_selector
    with_prefix(std::string_view selected_prefix) noexcept {
        return {.selected_kind = kind::prefix_strict, .prefix = selected_prefix};
    }
};

/// Drop the selected internal index tables and finish the SPI session.
void drop_index_relations(drop_index_relation_selector selector);

/**
 * @brief Visit each non-null sequence in the table column.
 *
 * Hold AccessShareLock on the parent table and register a transaction snapshot
 * while reading. Each callback borrows its sequence view for that call  only.
 * Locator-backed values read their fragments using the column reader's current snapshot.
 *
 * @tparam p_callback Callable accepting (sequence_view, row_number, total_rows).
 * @param[in] loader Callback for each inline or locator-backed sequence.
 */
template <class p_callback>
inline void table_index_storage::for_each_sequence(p_callback&& loader) {
    Relation main = table_open(user_relid_, AccessShareLock);
    TupleDesc td = RelationGetDescr(main);
    Snapshot snap = RegisterSnapshot(GetTransactionSnapshot());
    const std::size_t total = static_cast<std::size_t>(column_table_row_count(main));
    List* projected = list_make1_int(column_attno_);
    column_read_state* rs = begin_column_read(
        main, td, projected, NIL, own_mctx_, snap, /*randomAccess=*/false, nullptr);
    MemoryContext inner =
        AllocSetContextCreate(own_mctx_, "seqidx_table_inner_seq", ALLOCSET_DEFAULT_SIZES);
    std::vector<Datum> vals(td->natts);
    std::vector<bool> nullsv(td->natts);
    Datum* valp = vals.data();
    std::vector<char> nullbuf(td->natts);
    bool* nulls = reinterpret_cast<bool*>(nullbuf.data());
    std::uint64_t rowno = 0;
    while (read_next_column_row(rs, valp, nulls, &rowno)) {
        if (nulls[column_attno_ - 1])
            continue;
        MemoryContextReset(inner);
        MemoryContext previous_context = MemoryContextSwitchTo(inner);
        auto hdr_detoasted = bioseq::postgres::support::detoast_as<bioseq::pg_types::seq>(
            bioseq::postgres::support::pg_datum::from_raw(valp[column_attno_ - 1]));
        bioseq::pg_types::seq* hdr = hdr_detoasted.ptr;
        MemoryContextSwitchTo(previous_context);
        const std::uint8_t alpha = hdr->alphabet_code;
        const std::uint8_t bw = logical_storage_unit_byte_width(hdr);
        if (!is_locator_stub(hdr)) {
            bioseq::pg_types::seq::dispatch_instance(
                alpha, bw, [hdr, &loader, rowno, total]<class p_sequence_type>() {
                    compact_sequence_range<p_sequence_type> rng{*hdr};
                    const auto view = bioseq::storage::make_lazy_sequence_view(rng);
                    loader(view, static_cast<std::size_t>(rowno), total);
                });
        } else {
            const sequence_locator loc = decode_locator_from_stub(hdr);
            bioseq::pg_types::seq::dispatch_instance(
                alpha, bw, [this, &loader, loc, rowno, total, rs, inner]<class p_sequence_type>() {
                    // Read locator fragments with this traversal's snapshot and row memory context.
                    using source_binding =
                        bioseq::storage::postgres_lazy_sequence_binding<p_sequence_type>;
                    const source_binding bound_source{
                        db_oid_,
                        tablespace_oid_,
                        main_relfilenode_,
                        column_attno_,
                        loc,
                        source_binding::expected_last_fragment_padding(loc),
                        rs->snapshot,
                        read_caches_,
                        inner};
                    bioseq::storage::with_postgres_lazy_sequence<p_sequence_type>(
                        bound_source, [&loader, rowno, total](auto view) {
                            loader(view, static_cast<std::size_t>(rowno), total);
                        });
                });
        }
        hdr_detoasted.free_if_copy();
    }
    // Cached fragment readers borrow rs->snapshot. Close them before
    // end_column_read unregisters any snapshot it refreshed during the scan.
    if (read_caches_ != nullptr) {
        read_caches_->close_all(/*commit=*/false);
    }
    end_column_read(rs);
    UnregisterSnapshot(snap);
    MemoryContextDelete(inner);
    table_close(main, AccessShareLock);
}

static_assert(!std::is_move_assignable_v<sequence_index_storage>);
static_assert(!std::is_move_assignable_v<table_index_storage>);
static_assert(
    bioseq::indexing::is_index_chunked_storage_for<sequence_index_storage,
                                                   sequence_index_layout::sequence_inline>,
    "sequence_index_storage must satisfy INLINE_PER_SEQUENCE");
static_assert(bioseq::indexing::is_index_chunked_storage_for<table_index_storage,
                                                             sequence_index_layout::table_chunks>,
              "table_index_storage must satisfy CHUNKED_PER_TABLE");

} // namespace bioseq::storage::indexing
