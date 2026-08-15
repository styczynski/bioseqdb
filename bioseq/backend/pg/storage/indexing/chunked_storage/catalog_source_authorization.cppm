/**
 * @file catalog_source_authorization.cppm
 * @brief Defines generation-row validation and snapshot-bound source authorization.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
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

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
export module bioseq.postgres.storage.indexing.chunked_storage:catalog_source_authorization;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.postgres.support; // bioseq::postgres::support::try_open_postgres_relation, bioseq::postgres::support::find_primary_key_oid
import bioseq.postgres.support.datums;
import bioseq.postgres.sql_boundary;
import bioseq.postgres.storage.manager; // seq_caches, sequence_locator, format helpers
import bioseq.storage.layout;           // sequence_locator
import bioseq.storage.size;             // sequence_size_t
import bioseq.indexing.type; // sequence_index_layout, index_sub_key_t, k_max_index_chunk_size_bytes, sentinels
import bioseq.indexing.storage;           // the concepts
import bioseq.indexing.minimizer_hashmap; // encoded header/bucket validation
import bioseq.indexing.minimizer_fragment_image; // explicit fixed-endian minimizer fragment image image ABI
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.minimizer_fragment_compact_hash32_adapter;
import bioseq.postgres.table.io; // begin_column_read/read_next_column_row/end_column_read/column_read_state
import bioseq.postgres.storage.scanner;        // PostgreSQL-backed lazy sequence access
import bioseq.storage.lazy_sequence;           // lazy_sequence
import bioseq.postgres.types.compact_sequence; // bioseq::pg_types::seq, dispatch_instance
import bioseq.sequences;                       // compact_sequence_range
import bioseq.sequences.simd;                  // canonical bounded base decoding
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

import :catalog_contracts;
import :catalog_metadata;
export namespace bioseq::storage::indexing {
/// Validate one current-generation address and its row checksum before a PostgreSQL
/// tuple is formed. The active-head row is never accepted by the immutable
/// generation sink and has a separate atomic publication helper below.
struct minimizer_generation_row_validator;

/// Move-only authority for a row validated against the frozen wire contract.
class validated_minimizer_generation_row {
  private:
    minimizer_generation_row_view row_{};

    explicit validated_minimizer_generation_row(const minimizer_generation_row_view& row) noexcept
        : row_(row) {}

    friend struct minimizer_generation_row_validator;

  public:
    validated_minimizer_generation_row() = delete;

    [[nodiscard]] const minimizer_generation_row_view& row() const noexcept {
        return row_;
    }
};

/// Sole validation and construction authority. Defining this after the token is
/// complete avoids instantiating std::expected<validated_minimizer_generation_row, ...>
/// while preserving the invariant that no caller can construct an unchecked
/// token.
struct minimizer_generation_row_validator {
  private:
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    validate_shape(std::uint64_t expected_generation_id,
                   const index_chunk_address& address,
                   std::span<const std::byte> bytes) noexcept {
        if (expected_generation_id == 0 ||
            !bioseq::indexing::sql_bigint_can_represent(expected_generation_id) ||
            address.generation_id != expected_generation_id) {
            return std::unexpected(minimizer_generation_encoding_error::generation_id);
        }
        if (bytes.empty() ||
            bytes.size() > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) ||
            !bioseq::indexing::sql_integer_can_represent(address.chunk_no)) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        if (address.position_segment == -1) {
            if (address.sub_key != k_directory_sub_key) {
                return std::unexpected(minimizer_generation_encoding_error::sub_key);
            }
            if ((address.chunk_no == 0 &&
                 bytes.size() != minimizer_generation_manifest::encoded_bytes &&
                 bytes.size() != minimizer_generation_manifest::encoded_bytes) ||
                (address.chunk_no != 0 &&
                 bytes.size() > bioseq::indexing::k_grouped_soa_extent_payload_bytes)) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }
        } else if (address.position_segment < 0) {
            return std::unexpected(minimizer_generation_encoding_error::position_segment);
        } else if (address.sub_key < 0 || address.sub_key >= k_temporary_run_sub_key_base ||
                   bytes.size() > bioseq::indexing::k_grouped_soa_extent_payload_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::sub_key);
        }
        return {};
    }

  public:
    [[nodiscard]] static std::expected<validated_minimizer_generation_row,
                                       minimizer_generation_encoding_error>
    validate(std::uint64_t expected_generation_id,
             const minimizer_generation_row_view& row) noexcept {
        if (auto shape = validate_shape(expected_generation_id, row.address, row.bytes); !shape) {
            return std::unexpected(shape.error());
        }
        const std::uint64_t expected_checksum =
            bioseq::indexing::crc64_index_chunk_row(row.address.generation_id,
                                                    row.address.position_segment,
                                                    row.address.sub_key,
                                                    row.address.chunk_no,
                                                    static_cast<std::uint32_t>(row.bytes.size()),
                                                    row.bytes);
        if (row.row_checksum != expected_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        return validated_minimizer_generation_row{row};
    }

    /// Compute and bind the sole grouped-SoA payload-row CRC. The caller does
    /// not provide a checksum that would require a second verification pass.
    [[nodiscard]] static std::expected<validated_minimizer_generation_row,
                                       minimizer_generation_encoding_error>
    validate_and_checksum(std::uint64_t expected_generation_id,
                          const unchecked_minimizer_generation_row_view& row) noexcept {
        if (row.address.position_segment < 0) {
            return std::unexpected(minimizer_generation_encoding_error::row_order);
        }
        if (auto shape = validate_shape(expected_generation_id, row.address, row.bytes); !shape) {
            return std::unexpected(shape.error());
        }
        const std::uint64_t checksum =
            bioseq::indexing::crc64_index_chunk_row(row.address.generation_id,
                                                    row.address.position_segment,
                                                    row.address.sub_key,
                                                    row.address.chunk_no,
                                                    static_cast<std::uint32_t>(row.bytes.size()),
                                                    row.bytes);
        return validated_minimizer_generation_row{minimizer_generation_row_view{
            .address = row.address,
            .bytes = row.bytes,
            .row_checksum = checksum,
        }};
    }
};

/// Validate one row before forming a PostgreSQL tuple.
[[nodiscard]] inline std::expected<validated_minimizer_generation_row,
                                   minimizer_generation_encoding_error>
validate_minimizer_generation_row_for_write(std::uint64_t expected_generation_id,
                                            const minimizer_generation_row_view& row) noexcept {
    return minimizer_generation_row_validator::validate(expected_generation_id, row);
}

/// Catalog identity shared by generation writers, readers, and administrators.
struct minimizer_generation_administration_identity {
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    Oid user_relid = InvalidOid;
    std::int16_t column_attno = 0;
    std::string index_name;
    std::string table_name;

    void release_owned_strings() noexcept {
        std::string{}.swap(index_name);
        std::string{}.swap(table_name);
    }
};

/// Immutable identity of the exact visible source row used by the current generation.
/// `storage_contract_checksum` is filled only after the independently decoded
/// manifest has been checked against the source row and active storage-contract
/// axes. No head/manifest checksum is copied into this object.
struct minimizer_generation_source_locator {
    std::uint64_t sequence_rowno = 0;
    sequence_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t padding_letters = 0;
    std::uint64_t storage_contract_checksum = 0;

    [[nodiscard]] bool operator==(const minimizer_generation_source_locator& other) const noexcept {
        return sequence_rowno == other.sequence_rowno &&
               locator.payload_version == other.locator.payload_version &&
               locator.bucket_index == other.locator.bucket_index &&
               locator.reserved_16 == other.locator.reserved_16 &&
               locator.shard_index == other.locator.shard_index &&
               locator.fragment_count == other.locator.fragment_count &&
               locator.reserved_32 == other.locator.reserved_32 &&
               locator.logical_length == other.locator.logical_length &&
               locator.first_fragment_row == other.locator.first_fragment_row &&
               locator.source_epoch == other.locator.source_epoch &&
               alphabet_code == other.alphabet_code &&
               storage_unit_byte_width == other.storage_unit_byte_width &&
               padding_letters == other.padding_letters &&
               storage_contract_checksum == other.storage_contract_checksum;
    }
};

[[nodiscard]] constexpr bool
same_minimizer_generation_sequence_locator(const sequence_locator& left,
                                           const sequence_locator& right) noexcept {
    return left.payload_version == right.payload_version &&
           left.bucket_index == right.bucket_index && left.reserved_16 == right.reserved_16 &&
           left.shard_index == right.shard_index && left.fragment_count == right.fragment_count &&
           left.reserved_32 == right.reserved_32 && left.logical_length == right.logical_length &&
           left.first_fragment_row == right.first_fragment_row &&
           left.source_epoch == right.source_epoch;
}

/// Canonical locator-derived identity. The independently streamed content
/// checksum prevents a same-length mutation from being hidden by a
/// self-consistent rewrite of the head and manifest.
[[nodiscard]] constexpr std::uint64_t
minimizer_generation_source_identity(Oid /*main_relfilenode*/,
                                     const minimizer_generation_source_locator& source) noexcept {
    return source.locator.source_epoch;
}

class index_storage_fragment_generation_row_sink;
class index_storage_fragment_generation_read_session;
class index_storage_hash_major_minimizer_writer;
class index_storage_hash_major_minimizer_read_session;

/// Unforgeable PostgreSQL-owned authorization derived from the exact visible main-row
/// datum and, for a locator stub, every referenced fragment row under one
/// registered MVCC snapshot. Only the authorization helper can construct it. The
/// relation/query/snapshot binding prevents replay into another read session.
/// consumption is move-only and invalidates the token.
class minimizer_generation_source_authorization {
  private:
    minimizer_generation_source_locator source_{};
    std::uint64_t source_content_checksum_ = 0;
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    TransactionId transaction_id_ = InvalidTransactionId;
    bool valid_ = false;

    minimizer_generation_source_authorization() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        transaction_id_ = InvalidTransactionId;
        valid_ = false;
    }

    friend minimizer_generation_source_authorization authorize_minimizer_generation_source_pg(
        const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);
    friend class index_storage_fragment_generation_row_sink;
    friend class index_storage_fragment_generation_read_session;
    friend class index_storage_hash_major_minimizer_writer;
    friend class index_storage_hash_major_minimizer_read_session;

  public:
    minimizer_generation_source_authorization(const minimizer_generation_source_authorization&) =
        delete;
    minimizer_generation_source_authorization&
    operator=(const minimizer_generation_source_authorization&) = delete;
    minimizer_generation_source_authorization(
        minimizer_generation_source_authorization&& other) noexcept
        : source_(other.source_), source_content_checksum_(other.source_content_checksum_),
          database_oid_(other.database_oid_), tablespace_oid_(other.tablespace_oid_),
          main_relfilenode_(other.main_relfilenode_), user_relid_(other.user_relid_),
          column_attno_(other.column_attno_), index_name_(std::move(other.index_name_)),
          table_name_(std::move(other.table_name_)), snapshot_(other.snapshot_),
          transaction_id_(other.transaction_id_), valid_(other.valid_) {
        other.invalidate_();
    }
    minimizer_generation_source_authorization&
    operator=(minimizer_generation_source_authorization&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            transaction_id_ = other.transaction_id_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~minimizer_generation_source_authorization() = default;

    [[nodiscard]] const minimizer_generation_source_locator&
    source_for_diagnostics() const noexcept {
        return source_;
    }

    [[nodiscard]] std::uint64_t source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
};

static_assert(!std::is_default_constructible_v<minimizer_generation_source_authorization>);
static_assert(!std::is_copy_constructible_v<minimizer_generation_source_authorization>);
static_assert(std::is_nothrow_move_constructible_v<minimizer_generation_source_authorization>);

[[nodiscard]] inline minimizer_generation_source_authorization
authorize_minimizer_generation_source_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t sequence_rowno,
    Snapshot registered_snapshot) {
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct source_read_state {
        Relation source_relation = nullptr;
        column_read_state* source_read = nullptr;
        column_read_state* fragment_read = nullptr;
        seq_caches* fragment_caches = nullptr;
        MemoryContext work_context = nullptr;
        MemoryContext fragment_context = nullptr;
    };
    auto* state = new source_read_state{};
    minimizer_generation_source_authorization authorization{};
    try {
        if (identity.database_oid != MyDatabaseId || !OidIsValid(identity.user_relid) ||
            !OidIsValid(identity.tablespace_oid) || !OidIsValid(identity.main_relfilenode) ||
            identity.column_attno <= 0 || identity.index_name.empty() ||
            !bioseq::indexing::sql_bigint_can_represent(sequence_rowno) ||
            registered_snapshot == nullptr || !IsMVCCSnapshot(registered_snapshot) ||
            registered_snapshot->regd_count == 0) {
            minimizer_generation_storage_error(
                "authoritative source authorization input is invalid");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq current generation authoritative source authorization",
                ALLOCSET_DEFAULT_SIZES);
        });
        state->fragment_context = bioseq::postgres::support::invoke_pg_as_cpp([state] {
            return AllocSetContextCreate(state->work_context,
                                         "bioseq current generation source authorization fragment",
                                         ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation =
            bioseq::postgres::support::invoke_pg_as_cpp([user_relid = identity.user_relid] {
                return bioseq::postgres::support::try_open_postgres_relation(user_relid,
                                                                             AccessShareLock);
            });
        if (state->source_relation == nullptr ||
            RelationGetRelid(state->source_relation) != identity.user_relid ||
            state->source_relation->rd_locator.dbOid != identity.database_oid ||
            static_cast<Oid>(state->source_relation->rd_locator.relNumber) !=
                identity.main_relfilenode) {
            minimizer_generation_storage_error(
                "authoritative source relation is missing or changed");
        }
        const Oid source_tablespace = OidIsValid(state->source_relation->rd_rel->reltablespace)
                                          ? state->source_relation->rd_rel->reltablespace
                                          : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid ||
            identity.column_attno > source_desc->natts ||
            TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            minimizer_generation_storage_error(
                "authoritative source relation/column identity changed");
        }

        MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [column_attno = identity.column_attno] { return list_make1_int(column_attno); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<Datum*>(
                palloc0(sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<bool*>(
                palloc0(sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, source_desc, projected, registered_snapshot] {
                return begin_column_read(state->source_relation,
                                         source_desc,
                                         projected,
                                         NIL,
                                         state->work_context,
                                         registered_snapshot,
                                         /*randomAccess=*/true,
                                         nullptr);
            });
        const bool row_visible =
            bioseq::postgres::support::invoke_pg_as_cpp([state, sequence_rowno, values, nulls] {
                return read_column_row_by_number(state->source_read, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[identity.column_attno - 1]) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source row is not visible");
        }
        bioseq::pg_types::seq* source_header = bioseq::postgres::support::invoke_pg_as_cpp(
            [values, column_attno = identity.column_attno] {
                return reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(values[column_attno - 1]));
            });
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(source_header->alphabet_code);
        const std::uint8_t storage_width = logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (storage_width == 0) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source storage width is zero");
        }

        if (!is_locator_stub(source_header)) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative current-generation source is not an epoch-bearing locator stub");
        }
        const sequence_locator locator = decode_locator_from_stub(source_header);
        if (locator.payload_version != sequence_locator_payload_version ||
            locator.reserved_16 != 0 || locator.reserved_32 != 0 || locator.source_epoch == 0) {
            switch_context(previous_context);
            minimizer_generation_storage_error("authoritative source locator is invalid");
        }
        authorization.source_ = minimizer_generation_source_locator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .storage_contract_checksum = 0,
        };

        const auto source_catalog = bioseq::postgres::support::invoke_pg_as_cpp(
            [source_epoch = locator.source_epoch, registered_snapshot] {
                return sequence_source_version_manager::lookup(source_epoch, registered_snapshot);
            });
        const std::uint64_t main_storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return column_storage_id(state->source_relation, false); });
        if (!source_catalog || source_catalog->source_epoch != locator.source_epoch ||
            source_catalog->user_relid != identity.user_relid ||
            source_catalog->database_oid != identity.database_oid ||
            source_catalog->tablespace_oid != identity.tablespace_oid ||
            source_catalog->main_relfilenode != identity.main_relfilenode ||
            source_catalog->main_storage_id != main_storage_id ||
            source_catalog->column_attno != identity.column_attno ||
            !same_minimizer_generation_sequence_locator(source_catalog->locator, locator) ||
            source_catalog->alphabet_code != alphabet_code ||
            source_catalog->storage_unit_byte_width != storage_width ||
            source_catalog->padding_letters != padding_letters) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative source epoch/catalog binding disagrees");
        }

        crc64_ecma182_accumulator content_checksum{};
        std::uint64_t decoded_bp = 0;
        const auto consume_visible_fragment =
            [&content_checksum,
             &decoded_bp,
             &minimizer_generation_storage_error,
             alphabet_code,
             storage_width](const bioseq::pg_types::seq* fragment) {
                if (fragment == nullptr || is_locator_stub(fragment) ||
                    static_cast<std::uint8_t>(fragment->alphabet_code) != alphabet_code ||
                    logical_storage_unit_byte_width(fragment) != storage_width) {
                    minimizer_generation_storage_error(
                        "authoritative source fragment header disagrees");
                }
                fragment->convert_to([&content_checksum,
                                      &decoded_bp,
                                      &minimizer_generation_storage_error]<class p_sequence_type>(
                                         const compact_sequence_range<p_sequence_type>& range) {
                    const std::string decoded =
                        ::bioseq::simd::active_target::decode_sequence_simd<p_sequence_type>(range);
                    if (decoded.size() != range.bp_size() ||
                        decoded.size() > (std::numeric_limits<std::uint64_t>::max)() - decoded_bp) {
                        minimizer_generation_storage_error(
                            "authoritative source decoded length is invalid");
                    }
                    content_checksum.update(
                        std::as_bytes(std::span<const char>{decoded.data(), decoded.size()}));
                    decoded_bp += decoded.size();
                });
            };

        if (!is_locator_stub(source_header)) {
            consume_visible_fragment(source_header);
        } else if (locator.fragment_count != 0) {
            state->fragment_caches = bioseq::postgres::support::invoke_pg_as_cpp(
                [state] { return alloc_seq_caches(state->work_context); });
            state->fragment_caches->side = seq_caches::access_side::read;
            state->fragment_caches->lock_mode = AccessShareLock;
            state->fragment_caches->read_state_mctx = state->work_context;
            state->fragment_caches->main_database_oid = identity.database_oid;
            state->fragment_caches->main_tablespace_oid = identity.tablespace_oid;
            state->fragment_caches->main_relfilenode = identity.main_relfilenode;
            Relation fragment_relation =
                bioseq::postgres::support::invoke_pg_as_cpp([state, &identity, locator] {
                    return open_sub_table_for_locator(identity.database_oid,
                                                      identity.tablespace_oid,
                                                      identity.main_relfilenode,
                                                      identity.column_attno,
                                                      locator,
                                                      AccessShareLock,
                                                      state->fragment_caches);
                });
            if (RelationGetRelid(fragment_relation) != source_catalog->fragment_relid ||
                bioseq::postgres::support::invoke_pg_as_cpp([fragment_relation] {
                    return column_storage_id(fragment_relation, false);
                }) != source_catalog->fragment_storage_id) {
                switch_context(previous_context);
                minimizer_generation_storage_error(
                    "authoritative source fragment storage incarnation changed");
            }
            TupleDesc fragment_desc = RelationGetDescr(fragment_relation);
            if (fragment_desc->natts != 1 || TupleDescAttr(fragment_desc, 0)->attisdropped) {
                switch_context(previous_context);
                minimizer_generation_storage_error(
                    "authoritative source fragment schema disagrees");
            }
            List* fragment_projected =
                bioseq::postgres::support::invoke_pg_as_cpp([] { return list_make1_int(1); });
            state->fragment_read = bioseq::postgres::support::invoke_pg_as_cpp(
                [state, fragment_relation, fragment_desc, fragment_projected, registered_snapshot] {
                    return begin_column_read(fragment_relation,
                                             fragment_desc,
                                             fragment_projected,
                                             NIL,
                                             state->work_context,
                                             registered_snapshot,
                                             /*randomAccess=*/true,
                                             nullptr);
                });
            for (std::uint32_t fragment_no = 0; fragment_no < locator.fragment_count;
                 ++fragment_no) {
                bioseq::postgres::support::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
                bioseq::postgres::support::invoke_pg_as_cpp(
                    [state] { MemoryContextReset(state->fragment_context); });
                Datum fragment_values[1]{0};
                bool fragment_nulls[1]{true};
                const std::uint64_t fragment_rowno = locator.first_fragment_row + fragment_no;
                if (fragment_rowno < locator.first_fragment_row ||
                    !bioseq::postgres::support::invoke_pg_as_cpp(
                        [state, fragment_rowno, &fragment_values, &fragment_nulls] {
                            return read_column_row_by_number(state->fragment_read,
                                                             fragment_rowno,
                                                             fragment_values,
                                                             fragment_nulls);
                        }) ||
                    fragment_nulls[0]) {
                    switch_context(previous_context);
                    minimizer_generation_storage_error(
                        "authoritative source fragment row is missing");
                }
                switch_context(state->fragment_context);
                bioseq::pg_types::seq* fragment = bioseq::postgres::support::invoke_pg_as_cpp(
                    [fragment_datum = fragment_values[0]] {
                        return reinterpret_cast<bioseq::pg_types::seq*>(
                            PG_DETOAST_DATUM(fragment_datum));
                    });
                if ((fragment_no + 1u != locator.fragment_count &&
                     fragment->padding_letters != 0) ||
                    (fragment_no + 1u == locator.fragment_count &&
                     fragment->padding_letters != padding_letters)) {
                    switch_context(previous_context);
                    minimizer_generation_storage_error(
                        "authoritative source fragment padding disagrees");
                }
                consume_visible_fragment(fragment);
                switch_context(state->work_context);
            }
        }
        if (decoded_bp != locator.logical_length) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative source decoded length disagrees with locator");
        }
        if (content_checksum.checksum() != source_catalog->source_content_checksum) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "authoritative source decoded checksum disagrees with its epoch catalog");
        }
        authorization.source_content_checksum_ = source_catalog->source_content_checksum;
        authorization.database_oid_ = identity.database_oid;
        authorization.tablespace_oid_ = identity.tablespace_oid;
        authorization.main_relfilenode_ = identity.main_relfilenode;
        authorization.user_relid_ = identity.user_relid;
        authorization.column_attno_ = identity.column_attno;
        authorization.index_name_ = identity.index_name;
        authorization.table_name_ = format_seqidx_name(identity.index_name,
                                                       identity.database_oid,
                                                       identity.tablespace_oid,
                                                       identity.main_relfilenode,
                                                       identity.column_attno,
                                                       authorization.source_.locator.bucket_index,
                                                       authorization.source_.locator.shard_index);
        authorization.snapshot_ = registered_snapshot;
        authorization.transaction_id_ =
            bioseq::postgres::support::invoke_pg_as_cpp([] { return GetCurrentTransactionId(); });
        authorization.valid_ = true;

        if (state->fragment_read != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [state] { end_column_read(state->fragment_read); });
            state->fragment_read = nullptr;
        }
        if (state->fragment_caches != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp([state] {
                close_and_destroy_seq_caches(state->fragment_caches, /*commit=*/false);
            });
            state->fragment_caches = nullptr;
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        Relation finished_source = std::exchange(state->source_relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_source] { relation_close(finished_source, AccessShareLock); });
        MemoryContext finished_context = std::exchange(state->work_context, nullptr);
        state->fragment_context = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
    } catch (...) {
        state->fragment_read = nullptr;
        state->fragment_caches = nullptr;
        state->source_read = nullptr;
        state->source_relation = nullptr;
        state->work_context = nullptr;
        state->fragment_context = nullptr;
        authorization.invalidate_();
        delete state;
        throw;
    }
    delete state;
    return authorization;
}

/// Fixed work equation for cold, timed source admission. These counters travel
/// inside the move-only authorization and are rechecked by the read session, so a later
/// accidental source scan cannot be benchmark-accepted as an O(1) load.
struct minimizer_generation_catalog_source_stats {
    std::uint64_t parent_rows_read = 0;
    std::uint64_t catalog_rows_read = 0;
    std::uint64_t catalog_index_scans = 0;
    std::uint64_t fragment_rows_read = 0;
    std::uint64_t decoded_bp = 0;
    std::uint64_t storage_incarnations_checked = 0;

    [[nodiscard]] bool timed_equation_holds() const noexcept {
        return parent_rows_read == 1 && catalog_rows_read == 1 && catalog_index_scans == 1 &&
               fragment_rows_read == 0 && decoded_bp == 0 && storage_incarnations_checked >= 1 &&
               storage_incarnations_checked <= 2;
    }
};

/// O(1), PostgreSQL-owned source authorization for a cold reader. It validates one
/// visible parent stub and one source-epoch catalog tuple, plus constant-size
/// relation storage-incarnation metadata. It never opens a source fragment
/// read state and never decodes bases.
class minimizer_generation_catalog_source_authorization {
  private:
    minimizer_generation_source_locator source_{};
    std::uint64_t source_content_checksum_ = 0;
    minimizer_generation_catalog_source_stats stats_{};
    cold_index_preparation_stats cold_stats_{};
    Oid database_oid_ = InvalidOid;
    Oid tablespace_oid_ = InvalidOid;
    Oid main_relfilenode_ = InvalidOid;
    Oid user_relid_ = InvalidOid;
    std::int16_t column_attno_ = 0;
    std::string index_name_{};
    std::string table_name_{};
    Snapshot snapshot_ = nullptr;
    bool valid_ = false;

    minimizer_generation_catalog_source_authorization() = default;

    void invalidate_() noexcept {
        source_ = {};
        source_content_checksum_ = 0;
        stats_ = {};
        cold_stats_ = {};
        database_oid_ = InvalidOid;
        tablespace_oid_ = InvalidOid;
        main_relfilenode_ = InvalidOid;
        user_relid_ = InvalidOid;
        column_attno_ = 0;
        std::string{}.swap(index_name_);
        std::string{}.swap(table_name_);
        snapshot_ = nullptr;
        valid_ = false;
    }

    friend minimizer_generation_catalog_source_authorization
    authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity&,
        std::uint64_t,
        const borrowed_executor_read_context&);
    friend minimizer_generation_catalog_source_authorization
    authorize_minimizer_generation_catalog_source_pg(
        const minimizer_generation_administration_identity&, std::uint64_t, Snapshot);
    friend class index_storage_fragment_generation_read_session;
    friend class index_storage_hash_major_minimizer_read_session;

  public:
    minimizer_generation_catalog_source_authorization(
        const minimizer_generation_catalog_source_authorization&) = delete;
    minimizer_generation_catalog_source_authorization&
    operator=(const minimizer_generation_catalog_source_authorization&) = delete;
    minimizer_generation_catalog_source_authorization(
        minimizer_generation_catalog_source_authorization&& other) noexcept
        : source_(other.source_), source_content_checksum_(other.source_content_checksum_),
          stats_(other.stats_), cold_stats_(other.cold_stats_), database_oid_(other.database_oid_),
          tablespace_oid_(other.tablespace_oid_), main_relfilenode_(other.main_relfilenode_),
          user_relid_(other.user_relid_), column_attno_(other.column_attno_),
          index_name_(std::move(other.index_name_)), table_name_(std::move(other.table_name_)),
          snapshot_(other.snapshot_), valid_(other.valid_) {
        other.invalidate_();
    }
    minimizer_generation_catalog_source_authorization&
    operator=(minimizer_generation_catalog_source_authorization&& other) noexcept {
        if (this != &other) {
            invalidate_();
            source_ = other.source_;
            source_content_checksum_ = other.source_content_checksum_;
            stats_ = other.stats_;
            cold_stats_ = other.cold_stats_;
            database_oid_ = other.database_oid_;
            tablespace_oid_ = other.tablespace_oid_;
            main_relfilenode_ = other.main_relfilenode_;
            user_relid_ = other.user_relid_;
            column_attno_ = other.column_attno_;
            index_name_ = std::move(other.index_name_);
            table_name_ = std::move(other.table_name_);
            snapshot_ = other.snapshot_;
            valid_ = other.valid_;
            other.invalidate_();
        }
        return *this;
    }
    ~minimizer_generation_catalog_source_authorization() = default;

    [[nodiscard]] const minimizer_generation_source_locator&
    source_for_diagnostics() const noexcept {
        return source_;
    }
    [[nodiscard]] std::uint64_t source_content_checksum_for_diagnostics() const noexcept {
        return source_content_checksum_;
    }
    [[nodiscard]] const minimizer_generation_catalog_source_stats&
    stats_for_diagnostics() const noexcept {
        return stats_;
    }
    [[nodiscard]] std::string_view table_name_for_diagnostics() const noexcept {
        return table_name_;
    }
};

static_assert(!std::is_default_constructible_v<minimizer_generation_catalog_source_authorization>);
static_assert(!std::is_copy_constructible_v<minimizer_generation_catalog_source_authorization>);
static_assert(
    std::is_nothrow_move_constructible_v<minimizer_generation_catalog_source_authorization>);

[[nodiscard]] inline minimizer_generation_catalog_source_authorization
authorize_minimizer_generation_catalog_source_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t sequence_rowno,
    const borrowed_executor_read_context& executor_read) {
    const std::uint64_t source_begin_ns = cold_index_now_ns();
    const BufferUsage source_buffer_begin = pgBufferUsage;
    const auto minimizer_generation_storage_error = [](std::string_view message) {
        minimizer_generation_storage_error_as_cpp(message);
    };
    struct source_read_state {
        Relation source_relation = nullptr;
        Relation fragment_relation = nullptr;
        column_read_state* source_read = nullptr;
        MemoryContext work_context = nullptr;
    };
    auto* state = new source_read_state{};
    minimizer_generation_catalog_source_authorization authorization{};
    try {
        Snapshot registered_snapshot = executor_read.snapshot();
        if (identity.database_oid != MyDatabaseId || !OidIsValid(identity.user_relid) ||
            !OidIsValid(identity.tablespace_oid) || !OidIsValid(identity.main_relfilenode) ||
            identity.column_attno <= 0 || identity.index_name.empty() ||
            !bioseq::indexing::sql_bigint_can_represent(sequence_rowno) ||
            !executor_read.matches(identity.database_oid,
                                   identity.tablespace_oid,
                                   identity.main_relfilenode,
                                   identity.user_relid)) {
            minimizer_generation_storage_error("catalog source authorization input is invalid");
        }
        state->work_context = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(CurrentMemoryContext,
                                         "bioseq current generation catalog source authorization",
                                         ALLOCSET_DEFAULT_SIZES);
        });
        state->source_relation = executor_read.relation();
        if (state->source_relation == nullptr ||
            RelationGetRelid(state->source_relation) != identity.user_relid ||
            state->source_relation->rd_locator.dbOid != identity.database_oid ||
            static_cast<Oid>(state->source_relation->rd_locator.relNumber) !=
                identity.main_relfilenode) {
            minimizer_generation_storage_error("catalog source relation is missing or changed");
        }
        const Oid source_tablespace = OidIsValid(state->source_relation->rd_rel->reltablespace)
                                          ? state->source_relation->rd_rel->reltablespace
                                          : MyDatabaseTableSpace;
        TupleDesc source_desc = RelationGetDescr(state->source_relation);
        if (source_tablespace != identity.tablespace_oid ||
            identity.column_attno > source_desc->natts ||
            TupleDescAttr(source_desc, identity.column_attno - 1)->attisdropped) {
            minimizer_generation_storage_error("catalog source relation/column identity changed");
        }

        MemoryContext previous_context = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return MemoryContextSwitchTo(state->work_context); });
        const auto switch_context = [](MemoryContext context) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [context] { MemoryContextSwitchTo(context); });
        };
        List* projected = bioseq::postgres::support::invoke_pg_as_cpp(
            [column_attno = identity.column_attno] { return list_make1_int(column_attno); });
        Datum* values = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<Datum*>(
                palloc0(sizeof(Datum) * static_cast<std::size_t>(source_desc->natts)));
        });
        bool* nulls = bioseq::postgres::support::invoke_pg_as_cpp([source_desc] {
            return static_cast<bool*>(
                palloc0(sizeof(bool) * static_cast<std::size_t>(source_desc->natts)));
        });
        state->source_read = bioseq::postgres::support::invoke_pg_as_cpp(
            [state, source_desc, projected, registered_snapshot] {
                return begin_column_read(state->source_relation,
                                         source_desc,
                                         projected,
                                         NIL,
                                         state->work_context,
                                         registered_snapshot,
                                         /*randomAccess=*/true,
                                         nullptr);
            });
        const bool row_visible =
            bioseq::postgres::support::invoke_pg_as_cpp([state, sequence_rowno, values, nulls] {
                return read_column_row_by_number(state->source_read, sequence_rowno, values, nulls);
            });
        if (!row_visible || nulls[identity.column_attno - 1]) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source row is not visible");
        }
        ++authorization.stats_.parent_rows_read;
        authorization.cold_stats_.parent_random_reads = 1;
        bioseq::pg_types::seq* source_header = bioseq::postgres::support::invoke_pg_as_cpp(
            [values, column_attno = identity.column_attno] {
                return reinterpret_cast<bioseq::pg_types::seq*>(
                    PG_DETOAST_DATUM(values[column_attno - 1]));
            });
        if (!is_locator_stub(source_header)) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "catalog source is not an epoch-bearing locator stub");
        }
        const sequence_locator locator = decode_locator_from_stub(source_header);
        const std::uint8_t alphabet_code = static_cast<std::uint8_t>(source_header->alphabet_code);
        const std::uint8_t storage_width = logical_storage_unit_byte_width(source_header);
        const std::uint16_t padding_letters = source_header->padding_letters;
        if (locator.payload_version != sequence_locator_payload_version ||
            locator.source_epoch == 0 || locator.reserved_16 != 0 || locator.reserved_32 != 0 ||
            storage_width == 0) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source locator is invalid");
        }

        bool exact_primary_index_scan = false;
        sequence_source_lookup_telemetry source_lookup_telemetry{};
        const auto catalog =
            bioseq::postgres::support::invoke_pg_as_cpp([source_epoch = locator.source_epoch,
                                                         registered_snapshot,
                                                         &exact_primary_index_scan,
                                                         &source_lookup_telemetry] {
                return sequence_source_version_manager::lookup(source_epoch,
                                                               registered_snapshot,
                                                               /*require_ready=*/true,
                                                               &exact_primary_index_scan,
                                                               &source_lookup_telemetry);
            });
        if (exact_primary_index_scan) {
            ++authorization.stats_.catalog_index_scans;
            authorization.cold_stats_.add(authorization.cold_stats_.source_heap_opens,
                                          source_lookup_telemetry.heap_opens);
            authorization.cold_stats_.add(authorization.cold_stats_.source_index_opens,
                                          source_lookup_telemetry.index_opens);
            authorization.cold_stats_.add(authorization.cold_stats_.source_schema_validations,
                                          source_lookup_telemetry.schema_validations);
            authorization.cold_stats_.add(authorization.cold_stats_.lock_acquires,
                                          source_lookup_telemetry.lock_acquires);
            authorization.cold_stats_.add(authorization.cold_stats_.lock_releases,
                                          source_lookup_telemetry.lock_releases);
        }
        if (catalog)
            ++authorization.stats_.catalog_rows_read;
        const std::uint64_t main_storage_id = bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { return column_storage_id(state->source_relation, false); });
        ++authorization.stats_.storage_incarnations_checked;
        if (!catalog || catalog->source_epoch != locator.source_epoch ||
            catalog->user_relid != identity.user_relid ||
            catalog->database_oid != identity.database_oid ||
            catalog->tablespace_oid != identity.tablespace_oid ||
            catalog->main_relfilenode != identity.main_relfilenode ||
            catalog->main_storage_id != main_storage_id ||
            catalog->column_attno != identity.column_attno ||
            !same_minimizer_generation_sequence_locator(catalog->locator, locator) ||
            catalog->alphabet_code != alphabet_code ||
            catalog->storage_unit_byte_width != storage_width ||
            catalog->padding_letters != padding_letters) {
            switch_context(previous_context);
            minimizer_generation_storage_error("catalog source epoch binding disagrees");
        }
        if (locator.fragment_count == 0) {
            if (OidIsValid(catalog->fragment_relid) || catalog->fragment_storage_id != 0 ||
                locator.first_fragment_row != 0) {
                switch_context(previous_context);
                minimizer_generation_storage_error("empty catalog source sentinel disagrees");
            }
        } else {
            state->fragment_relation = bioseq::postgres::support::invoke_pg_as_cpp(
                [fragment_relid = catalog->fragment_relid] {
                    return bioseq::postgres::support::try_open_postgres_relation(fragment_relid,
                                                                                 AccessShareLock);
                });
            authorization.cold_stats_.fragment_heap_opens = 1;
            authorization.cold_stats_.add(authorization.cold_stats_.lock_acquires, 1);
            const std::string expected_fragment_name =
                format_sub_table_name(identity.database_oid,
                                      identity.tablespace_oid,
                                      identity.main_relfilenode,
                                      identity.column_attno,
                                      static_cast<std::int16_t>(locator.bucket_index),
                                      static_cast<std::int32_t>(locator.shard_index));
            if (state->fragment_relation == nullptr ||
                RelationGetNamespace(state->fragment_relation) != bioseq_internal_ns() ||
                expected_fragment_name != RelationGetRelationName(state->fragment_relation) ||
                bioseq::postgres::support::invoke_pg_as_cpp([state] {
                    return column_storage_id(state->fragment_relation, false);
                }) != catalog->fragment_storage_id) {
                switch_context(previous_context);
                minimizer_generation_storage_error(
                    "catalog source fragment storage incarnation changed");
            }
            ++authorization.stats_.storage_incarnations_checked;
        }

        authorization.source_ = minimizer_generation_source_locator{
            .sequence_rowno = sequence_rowno,
            .locator = locator,
            .alphabet_code = alphabet_code,
            .storage_unit_byte_width = storage_width,
            .padding_letters = padding_letters,
            .storage_contract_checksum = 0,
        };
        authorization.source_content_checksum_ = catalog->source_content_checksum;
        authorization.database_oid_ = identity.database_oid;
        authorization.tablespace_oid_ = identity.tablespace_oid;
        authorization.main_relfilenode_ = identity.main_relfilenode;
        authorization.user_relid_ = identity.user_relid;
        authorization.column_attno_ = identity.column_attno;
        authorization.index_name_ = identity.index_name;
        authorization.table_name_ = format_seqidx_name(identity.index_name,
                                                       identity.database_oid,
                                                       identity.tablespace_oid,
                                                       identity.main_relfilenode,
                                                       identity.column_attno,
                                                       locator.bucket_index,
                                                       locator.shard_index);
        authorization.snapshot_ = registered_snapshot;
        authorization.valid_ = authorization.stats_.timed_equation_holds();
        if (!authorization.valid_) {
            switch_context(previous_context);
            minimizer_generation_storage_error(
                "catalog source authorization work equation disagrees");
        }

        if (state->fragment_relation != nullptr) {
            Relation finished = std::exchange(state->fragment_relation, nullptr);
            bioseq::postgres::support::invoke_pg_as_cpp(
                [finished] { relation_close(finished, AccessShareLock); });
            authorization.cold_stats_.add(authorization.cold_stats_.lock_releases, 1);
        }
        bioseq::postgres::support::invoke_pg_as_cpp(
            [state] { end_column_read(state->source_read); });
        state->source_read = nullptr;
        switch_context(previous_context);
        // The executor owns this Relation and its AccessShareLock. The authorization
        // closes only its column read state and never closes the borrowed
        // descriptor.
        state->source_relation = nullptr;
        MemoryContext finished_context = std::exchange(state->work_context, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
        add_cold_index_elapsed(
            authorization.cold_stats_, authorization.cold_stats_.source_ns, source_begin_ns);
        add_cold_index_buffer_usage(authorization.cold_stats_, source_buffer_begin);
    } catch (...) {
        state->source_read = nullptr;
        state->fragment_relation = nullptr;
        state->source_relation = nullptr;
        state->work_context = nullptr;
        authorization.invalidate_();
        delete state;
        throw;
    }
    delete state;
    return authorization;
}

/// Administrative overload. Admin callers do not own an
/// executor Relation descriptor, so they retain the explicit open. The timed
/// BatchJoin production path above never enters this overload.
[[nodiscard]] inline minimizer_generation_catalog_source_authorization
authorize_minimizer_generation_catalog_source_pg(
    const minimizer_generation_administration_identity& identity,
    std::uint64_t sequence_rowno,
    Snapshot registered_snapshot) {
    Relation relation = nullptr;
    try {
        relation = bioseq::postgres::support::invoke_pg_as_cpp([&identity] {
            return bioseq::postgres::support::try_open_postgres_relation(identity.user_relid,
                                                                         AccessShareLock);
        });
        if (relation == nullptr) {
            minimizer_generation_storage_error_as_cpp(
                "catalog source relation is missing or changed");
        }
        borrowed_executor_read_context borrowed{relation, registered_snapshot};
        auto authorization =
            authorize_minimizer_generation_catalog_source_pg(identity, sequence_rowno, borrowed);
        Relation finished = std::exchange(relation, nullptr);
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished] { relation_close(finished, AccessShareLock); });
        authorization.cold_stats_.target_relation_extra_opens = 1;
        authorization.cold_stats_.add(authorization.cold_stats_.lock_acquires, 1);
        authorization.cold_stats_.add(authorization.cold_stats_.lock_releases, 1);
        return authorization;
    } catch (const bioseq::postgres::support::postgres_error&) {
        relation = nullptr;
        throw;
    } catch (...) {
        if (relation != nullptr) {
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation] { relation_close(relation, AccessShareLock); });
        }
        throw;
    }
}

} // namespace bioseq::storage::indexing
