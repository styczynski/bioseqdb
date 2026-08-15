/**
 * @file fragment_storage_io.cppm
 * @brief Fragment chunk buffering, cache admission, tuple I/O, and deletion operations.
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
export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_io;
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
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
import :hash_major_session;
export import :storage_adapter_contract;
export import :fragment_storage_lifecycle;
export import :fragment_storage_catalog;
export namespace bioseq::storage::indexing {

inline void fragment_index_storage::ensure_buffer_(std::size_t units) const {
    if (decoded_units_ >= units && decoded_ != nullptr)
        return;
    if (own_mctx_ == nullptr) {
        index_error("fragment read buffer has no owning memory context");
    }
    if (units > MaxAllocSize / sizeof(std::int64_t)) {
        index_error("fragment read buffer exceeds PostgreSQL allocation limit");
    }
    const std::size_t bytes = units * sizeof(std::int64_t);
    MemoryContext owner = own_mctx_;
    std::int64_t* old_buffer = decoded_;
    std::int64_t* replacement =
        bioseq::postgres::support::invoke_pg_as_cpp([owner, old_buffer, bytes] {
            MemoryContext previous = MemoryContextSwitchTo(owner);
            auto* result = old_buffer == nullptr
                               ? static_cast<std::int64_t*>(palloc(bytes))
                               : static_cast<std::int64_t*>(repalloc(old_buffer, bytes));
            MemoryContextSwitchTo(previous);
            return result;
        });
    decoded_ = replacement;
    decoded_units_ = units;
}

[[nodiscard]] inline std::int64_t* fragment_index_storage::scratch_buffer_(std::size_t units) {
    const std::size_t required_units = std::max<std::size_t>(units, 1u);
    if (scratch_units_ >= required_units && scratch_ != nullptr) {
        return scratch_;
    }
    if (own_mctx_ == nullptr) {
        index_error("fragment write scratch has no owning memory context");
    }
    if (required_units > MaxAllocSize / sizeof(std::int64_t)) {
        index_error("fragment write scratch exceeds PostgreSQL allocation limit");
    }
    const std::size_t bytes = required_units * sizeof(std::int64_t);
    MemoryContext owner = own_mctx_;
    std::int64_t* old_buffer = scratch_;
    std::int64_t* replacement =
        bioseq::postgres::support::invoke_pg_as_cpp([owner, old_buffer, bytes] {
            MemoryContext previous = MemoryContextSwitchTo(owner);
            auto* result = old_buffer == nullptr
                               ? static_cast<std::int64_t*>(palloc(bytes))
                               : static_cast<std::int64_t*>(repalloc(old_buffer, bytes));
            MemoryContextSwitchTo(previous);
            return result;
        });
    scratch_ = replacement;
    scratch_units_ = required_units;
    return scratch_;
}

[[nodiscard]] inline std::size_t fragment_index_storage::scratch_buffer_bytes_() const {
    return scratch_units_ * sizeof(std::int64_t);
}

[[nodiscard]] inline index_chunk_key
fragment_index_storage::chunk_cache_key_(index_sub_key_t sub_key,
                                         std::size_t chunk_no) const noexcept {
    return index_chunk_key{
        .db_oid = db_oid_,
        .user_relid = user_relid_,
        .tablespace_oid = tablespace_oid_,
        .main_relfilenode = main_relfilenode_,
        .column_attno = column_attno_,
        .index_name_hash = index_name_hash(
            db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_),
        .bucket_idx = bucket_idx_,
        .shard_idx = shard_idx_,
        .query_key = query_key_,
        .sub_key = sub_key,
        .chunk_no = static_cast<std::uint32_t>(chunk_no),
    };
}

inline void fragment_index_storage::admit_cached_chunk_(index_sub_key_t sub_key,
                                                        std::size_t chunk_no,
                                                        std::span<const char> payload,
                                                        bool written_in_current_xact) const {
    (void)store_cached_chunk(chunk_cache_key_(sub_key, chunk_no), payload, written_in_current_xact);
}

inline void fragment_index_storage::load_chunk_into_buffer_(index_sub_key_t sub_key,
                                                            std::size_t chunk_no) const {
    const index_chunk_key key = chunk_cache_key_(sub_key, chunk_no);
    if (auto cached = lookup_cached_chunk(key)) {
        const std::size_t units = (cached->size + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        ensure_buffer_(units == 0 ? 1 : units);
        if (cached->size > 0) {
            std::memcpy(decoded_, cached->data, cached->size);
        }
        decoded_sub_ = sub_key;
        decoded_chunk_ = chunk_no;
        decoded_bytes_ = cached->size;
        return;
    }
    if (chunk_no > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
        index_error("fragment chunk number exceeds PostgreSQL INT range");
    }

    fragment_read_resources resources{AccessShareLock};
    try {
        resources.relation = open_index_relation_(false, AccessShareLock);
        if (resources.relation == nullptr) {
            const std::string error =
                fmt::format("chunk ({},{},{}) relation missing", query_key_, sub_key, chunk_no);
            index_error(error);
        }

        const Oid primary_index =
            bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation] {
                return bioseq::postgres::support::find_primary_key_oid(relation);
            });
        std::array<ScanKeyData, 3> keys{};
        const Datum query_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw();
        const Datum stored_sub_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw();
        const Datum stored_chunk_no =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(chunk_no)).raw();
        ScanKeyData* key_data = keys.data();
        bioseq::postgres::support::invoke_pg_as_cpp([key_data,
                                                     query_key,
                                                     stored_sub_key,
                                                     stored_chunk_no] {
            ScanKeyInit(
                &key_data[0], k_attribute_query_key, BTEqualStrategyNumber, F_INT8EQ, query_key);
            ScanKeyInit(
                &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber, F_INT8EQ, stored_sub_key);
            ScanKeyInit(&key_data[2],
                        k_attribute_chunk_number,
                        BTEqualStrategyNumber,
                        F_INT4EQ,
                        stored_chunk_no);
        });
        resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation = resources.relation, primary_index, key_data] {
                return systable_beginscan(
                    relation, primary_index, OidIsValid(primary_index), nullptr, 3, key_data);
            });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan = resources.scan] { return systable_getnext(scan); });
        if (!HeapTupleIsValid(tuple)) {
            const std::string error =
                fmt::format("chunk ({},{},{}) is missing", query_key_, sub_key, chunk_no);
            index_error(error);
        }

        bool is_null = false;
        const Datum blob = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation = resources.relation, tuple, &is_null] {
                return heap_getattr(tuple, k_attribute_blob, RelationGetDescr(relation), &is_null);
            });
        if (is_null) {
            index_error("fragment chunk blob is NULL");
        }
        auto detoasted = bioseq::postgres::support::invoke_pg_as_cpp([blob] {
            return bioseq::postgres::support::detoast_packed_as<bytea>(
                bioseq::postgres::support::pg_datum::from_raw(blob));
        });
        resources.detoasted = detoasted.ptr;
        resources.detoasted_is_copy = detoasted.is_copy;
        detoasted.ptr = nullptr;
        detoasted.is_copy = false;

        bytea* raw = resources.detoasted;
        const std::size_t payload = VARSIZE_ANY_EXHDR(raw);
        const std::size_t units = (payload + sizeof(std::int64_t) - 1) / sizeof(std::int64_t);
        ensure_buffer_(units == 0 ? 1 : units);
        if (payload > 0) {
            std::memcpy(decoded_, VARDATA_ANY(raw), payload);
        }
        admit_cached_chunk_(
            sub_key,
            chunk_no,
            std::span<const char>(reinterpret_cast<const char*>(VARDATA_ANY(raw)), payload),
            false);

        resources.close_checked();
        decoded_sub_ = sub_key;
        decoded_chunk_ = chunk_no;
        decoded_bytes_ = payload;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        const std::exception_ptr error = std::current_exception();
        try {
            resources.close_checked();
        } catch (...) {
            resources.abandon_after_pg_error_noexcept();
            throw;
        }
        std::rethrow_exception(error);
    }
}

[[nodiscard]] inline std::size_t fragment_index_storage::number_of_sub_keys() const {
    fragment_read_resources resources{AccessShareLock};
    try {
        resources.relation = open_index_relation_(false, AccessShareLock);
        if (resources.relation == nullptr)
            return 0;
        const Oid primary_index =
            bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation] {
                return bioseq::postgres::support::find_primary_key_oid(relation);
            });
        std::array<ScanKeyData, 2> keys{};
        ScanKeyData* key_data = keys.data();
        const Datum query_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw();
        const Datum data_sub_key_limit =
            bioseq::postgres::support::pg_datum::from(
                static_cast<std::int64_t>(k_temporary_run_sub_key_base))
                .raw();
        bioseq::postgres::support::invoke_pg_as_cpp([key_data, query_key, data_sub_key_limit] {
            ScanKeyInit(
                &key_data[0], k_attribute_query_key, BTEqualStrategyNumber, F_INT8EQ, query_key);
            ScanKeyInit(&key_data[1],
                        k_attribute_sub_key,
                        BTLessStrategyNumber,
                        F_INT8LT,
                        data_sub_key_limit);
        });
        resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation = resources.relation, primary_index, key_data] {
                return systable_beginscan(
                    relation, primary_index, OidIsValid(primary_index), nullptr, 2, key_data);
            });
        std::int64_t last = (std::numeric_limits<std::int64_t>::min)();
        bool have_last = false;
        std::size_t count = 0;
        while (true) {
            HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                [scan = resources.scan] { return systable_getnext(scan); });
            if (!HeapTupleIsValid(tuple))
                break;
            bool is_null = false;
            const Datum sub_key_datum = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, tuple, &is_null] {
                    return heap_getattr(
                        tuple, k_attribute_sub_key, RelationGetDescr(relation), &is_null);
                });
            if (is_null)
                index_error("fragment sub-key is NULL");
            const std::int64_t sub_key = bioseq::postgres::support::pg_datum::to<std::int64_t>(
                bioseq::postgres::support::pg_datum::from_raw(sub_key_datum));
            if (!have_last || sub_key != last) {
                ++count;
                last = sub_key;
                have_last = true;
            }
        }
        resources.close_checked();
        return count;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        resources.close_then_rethrow_cpp(std::current_exception());
    }
}

[[nodiscard]] inline std::size_t
fragment_index_storage::number_of_chunks(index_sub_key_t sub_key) const {
    fragment_read_resources resources{AccessShareLock};
    try {
        resources.relation = open_index_relation_(false, AccessShareLock);
        if (resources.relation == nullptr)
            return 0;
        const Oid primary_index =
            bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation] {
                return bioseq::postgres::support::find_primary_key_oid(relation);
            });
        std::array<ScanKeyData, 2> keys{};
        ScanKeyData* key_data = keys.data();
        const Datum query_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw();
        const Datum stored_sub_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw();
        bioseq::postgres::support::invoke_pg_as_cpp([key_data, query_key, stored_sub_key] {
            ScanKeyInit(
                &key_data[0], k_attribute_query_key, BTEqualStrategyNumber, F_INT8EQ, query_key);
            ScanKeyInit(
                &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber, F_INT8EQ, stored_sub_key);
        });
        resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation = resources.relation, primary_index, key_data] {
                return systable_beginscan(
                    relation, primary_index, OidIsValid(primary_index), nullptr, 2, key_data);
            });
        std::size_t count = 0;
        while (bioseq::postgres::support::invoke_pg_as_cpp(
                   [scan = resources.scan] { return systable_getnext(scan); }) != nullptr) {
            ++count;
        }
        resources.close_checked();
        return count;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        resources.close_then_rethrow_cpp(std::current_exception());
    }
}

[[nodiscard]] inline std::size_t
fragment_index_storage::chunk_byte_size(index_sub_key_t sub_key, std::size_t chunk_no) const {
    if (chunk_no > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
        index_error("fragment chunk number exceeds PostgreSQL INT range");
    }
    fragment_read_resources resources{AccessShareLock};
    try {
        resources.relation = open_index_relation_(false, AccessShareLock);
        if (resources.relation == nullptr)
            return 0;
        const Oid primary_index =
            bioseq::postgres::support::invoke_pg_as_cpp([relation = resources.relation] {
                return bioseq::postgres::support::find_primary_key_oid(relation);
            });
        std::array<ScanKeyData, 3> keys{};
        ScanKeyData* key_data = keys.data();
        const Datum query_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw();
        const Datum stored_sub_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw();
        const Datum stored_chunk_no =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(chunk_no)).raw();
        bioseq::postgres::support::invoke_pg_as_cpp([key_data,
                                                     query_key,
                                                     stored_sub_key,
                                                     stored_chunk_no] {
            ScanKeyInit(
                &key_data[0], k_attribute_query_key, BTEqualStrategyNumber, F_INT8EQ, query_key);
            ScanKeyInit(
                &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber, F_INT8EQ, stored_sub_key);
            ScanKeyInit(&key_data[2],
                        k_attribute_chunk_number,
                        BTEqualStrategyNumber,
                        F_INT4EQ,
                        stored_chunk_no);
        });
        resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation = resources.relation, primary_index, key_data] {
                return systable_beginscan(
                    relation, primary_index, OidIsValid(primary_index), nullptr, 3, key_data);
            });
        HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
            [scan = resources.scan] { return systable_getnext(scan); });
        std::size_t result = 0;
        if (HeapTupleIsValid(tuple)) {
            bool is_null = false;
            const Datum chunk_bytes_datum = bioseq::postgres::support::invoke_pg_as_cpp(
                [relation = resources.relation, tuple, &is_null] {
                    return heap_getattr(
                        tuple, k_attribute_chunk_bytes, RelationGetDescr(relation), &is_null);
                });
            if (is_null)
                index_error("fragment chunk byte count is NULL");
            const std::int32_t signed_result =
                bioseq::postgres::support::pg_datum::to<std::int32_t>(
                    bioseq::postgres::support::pg_datum::from_raw(chunk_bytes_datum));
            if (signed_result < 0) {
                index_error("fragment chunk byte count is negative");
            }
            result = static_cast<std::size_t>(signed_result);
        }
        resources.close_checked();
        return result;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        resources.close_then_rethrow_cpp(std::current_exception());
    }
}

inline void fragment_index_storage::resize(index_sub_key_t sub_key, std::size_t bytes_count) {
    const std::size_t n =
        bytes_count == 0 ? 1 : 1 + (bytes_count - 1) / k_max_index_chunk_size_bytes;
    std::vector<std::size_t> plan(n);
    std::size_t rem = bytes_count;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t c = std::min(rem, k_max_index_chunk_size_bytes);
        plan[i] = c;
        rem -= c;
    }
    chunk_payload_bytes_[sub_key] = std::move(plan);
}

inline void fragment_index_storage::resize_chunks(index_sub_key_t sub_key,
                                                  std::span<const std::size_t> per_chunk_bytes) {
    std::vector<std::size_t> plan(per_chunk_bytes.begin(), per_chunk_bytes.end());
    for (std::size_t bytes : plan) {
        if (bytes > k_max_index_chunk_size_bytes) {
            index_error("fragment write chunk payload exceeds MAX");
        }
    }
    chunk_payload_bytes_[sub_key] = std::move(plan);
}

inline void fragment_index_storage::prefetch_chunk(index_sub_key_t sub_key, std::size_t chunk_no) {
    if (decoded_sub_ == sub_key && decoded_chunk_ == chunk_no) {
        return;
    }
    load_chunk_into_buffer_(sub_key, chunk_no);
}

inline void fragment_index_storage::upsert_chunk_row_(fragment_write_resources& resources,
                                                      std::uint64_t query_key,
                                                      index_sub_key_t sub_key,
                                                      std::size_t chunk_no,
                                                      std::size_t chunk_bytes,
                                                      std::span<const char> payload) {
    if (resources.relation == nullptr) {
        index_error("fragment write relation is missing");
    }
    if (!bioseq::indexing::sql_bigint_can_represent(query_key)) {
        index_error("fragment write query key exceeds PostgreSQL BIGINT");
    }
    if (chunk_no > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
        index_error("fragment write chunk number exceeds PostgreSQL INT");
    }
    if (chunk_bytes > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)()) ||
        chunk_bytes != payload.size()) {
        index_error("fragment write chunk byte count is invalid");
    }
    if (payload.size() > MaxAllocSize - VARHDRSZ) {
        index_error("fragment write payload exceeds PostgreSQL allocation limit");
    }

    Relation relation = resources.relation;
    const Oid primary_index = bioseq::postgres::support::invoke_pg_as_cpp(
        [relation] { return bioseq::postgres::support::find_primary_key_oid(relation); });
    if (!OidIsValid(primary_index)) {
        index_error("fragment write relation has no primary index");
    }

    std::array<ScanKeyData, 3> keys{};
    ScanKeyData* key_data = keys.data();
    const Datum stored_query_key =
        bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key)).raw();
    const Datum stored_sub_key =
        bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(sub_key)).raw();
    const Datum stored_chunk_no =
        bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(chunk_no)).raw();
    bioseq::postgres::support::invoke_pg_as_cpp(
        [key_data, stored_query_key, stored_sub_key, stored_chunk_no] {
            ScanKeyInit(&key_data[0],
                        k_attribute_query_key,
                        BTEqualStrategyNumber,
                        F_INT8EQ,
                        stored_query_key);
            ScanKeyInit(
                &key_data[1], k_attribute_sub_key, BTEqualStrategyNumber, F_INT8EQ, stored_sub_key);
            ScanKeyInit(&key_data[2],
                        k_attribute_chunk_number,
                        BTEqualStrategyNumber,
                        F_INT4EQ,
                        stored_chunk_no);
        });
    resources.scan =
        bioseq::postgres::support::invoke_pg_as_cpp([relation, primary_index, key_data] {
            return systable_beginscan(relation, primary_index, true, nullptr, 3, key_data);
        });
    HeapTuple previous = bioseq::postgres::support::invoke_pg_as_cpp(
        [scan = resources.scan] { return systable_getnext(scan); });
    if (HeapTupleIsValid(previous)) {
        ItemPointerData previous_tid = previous->t_self;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, previous_tid]() mutable { simple_heap_delete(relation, &previous_tid); });
    }
    resources.finish_scan_checked();

    const std::size_t allocation_bytes = VARHDRSZ + payload.size();
    resources.blob = bioseq::postgres::support::invoke_pg_as_cpp(
        [allocation_bytes] { return reinterpret_cast<bytea*>(palloc(allocation_bytes)); });
    SET_VARSIZE(resources.blob, allocation_bytes);
    if (!payload.empty()) {
        std::memcpy(VARDATA(resources.blob), payload.data(), payload.size());
    }

    std::array<Datum, 5> values{
        stored_query_key,
        stored_sub_key,
        stored_chunk_no,
        bioseq::postgres::support::pg_datum::from(static_cast<std::int32_t>(chunk_bytes)).raw(),
        bioseq::postgres::support::pg_datum::from_pointer(resources.blob).raw(),
    };
    std::array<bool, 5> nulls{};
    Datum* value_data = values.data();
    bool* null_data = nulls.data();
    resources.tuple =
        bioseq::postgres::support::invoke_pg_as_cpp([relation, value_data, null_data] {
            return heap_form_tuple(RelationGetDescr(relation), value_data, null_data);
        });
    bioseq::postgres::support::invoke_pg_as_cpp(
        [relation, tuple = resources.tuple] { CatalogTupleInsert(relation, tuple); });
    resources.finish_row_checked();
}

inline void fragment_index_storage::write_serialized_chunk_checked(std::uint64_t query_key,
                                                                   index_sub_key_t sub_key,
                                                                   std::size_t chunk_no,
                                                                   std::span<const char> payload,
                                                                   bool written_in_current_xact) {
    fragment_write_resources resources{RowExclusiveLock};
    try {
        resources.relation = open_index_relation_(true, RowExclusiveLock);
        if (resources.relation == nullptr) {
            index_error("serialized fragment write relation is missing");
        }
        upsert_chunk_row_(resources, query_key, sub_key, chunk_no, payload.size(), payload);
        admit_cached_chunk_(sub_key, chunk_no, payload, written_in_current_xact);
        resources.close_checked();
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        resources.close_then_rethrow_cpp(std::current_exception());
    }
}

inline void fragment_index_storage::delete_where_(
    std::optional<std::pair<index_sub_key_t, index_sub_key_t>> inclusive_sub_key_range) {
    if (inclusive_sub_key_range &&
        inclusive_sub_key_range->second < inclusive_sub_key_range->first) {
        return;
    }
    if (!bioseq::indexing::sql_bigint_can_represent(query_key_)) {
        index_error("fragment delete query key exceeds PostgreSQL BIGINT");
    }
    // Opening through the five-column validator is intentional: a generation
    // generation table has query_key/sub_key columns too, so name-based
    // DELETE SQL could otherwise erase immutable generations while
    // bypassing every direct-row attribute check.
    fragment_write_resources resources{RowExclusiveLock};
    try {
        resources.relation = open_index_relation_(false, RowExclusiveLock);
        if (resources.relation == nullptr)
            return;
        Relation relation = resources.relation;
        const Oid primary_index = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation] { return bioseq::postgres::support::find_primary_key_oid(relation); });
        if (!OidIsValid(primary_index)) {
            resources.close_checked();
            index_error("fragment delete requires a primary index");
        }

        std::array<ScanKeyData, 3> keys{};
        ScanKeyData* key_data = keys.data();
        const Datum query_key =
            bioseq::postgres::support::pg_datum::from(static_cast<std::int64_t>(query_key_)).raw();
        const bool bounded = inclusive_sub_key_range.has_value();
        const Datum first_sub_key =
            bounded ? bioseq::postgres::support::pg_datum::from(
                          static_cast<std::int64_t>(inclusive_sub_key_range->first))
                          .raw()
                    : Datum{0};
        const Datum last_sub_key =
            bounded ? bioseq::postgres::support::pg_datum::from(
                          static_cast<std::int64_t>(inclusive_sub_key_range->second))
                          .raw()
                    : Datum{0};
        bioseq::postgres::support::invoke_pg_as_cpp(
            [key_data, query_key, bounded, first_sub_key, last_sub_key] {
                ScanKeyInit(&key_data[0],
                            k_attribute_query_key,
                            BTEqualStrategyNumber,
                            F_INT8EQ,
                            query_key);
                if (bounded) {
                    ScanKeyInit(&key_data[1],
                                k_attribute_sub_key,
                                BTGreaterEqualStrategyNumber,
                                F_INT8GE,
                                first_sub_key);
                    ScanKeyInit(&key_data[2],
                                k_attribute_sub_key,
                                BTLessEqualStrategyNumber,
                                F_INT8LE,
                                last_sub_key);
                }
            });
        const int key_count = bounded ? 3 : 1;
        resources.scan = bioseq::postgres::support::invoke_pg_as_cpp(
            [relation, primary_index, key_count, key_data] {
                return systable_beginscan(
                    relation, primary_index, true, nullptr, key_count, key_data);
            });
        while (true) {
            HeapTuple tuple = bioseq::postgres::support::invoke_pg_as_cpp(
                [scan = resources.scan] { return systable_getnext(scan); });
            if (!HeapTupleIsValid(tuple))
                break;
            ItemPointerData tuple_id = tuple->t_self;
            bioseq::postgres::support::invoke_pg_as_cpp(
                [relation, tuple_id]() mutable { simple_heap_delete(relation, &tuple_id); });
        }
        resources.close_checked();
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_after_pg_error_noexcept();
        throw;
    } catch (...) {
        resources.close_then_rethrow_cpp(std::current_exception());
    }
    bioseq::postgres::support::invoke_pg_as_cpp([] { CommandCounterIncrement(); });
    evict_generation_storage_residency_();
}

inline void fragment_index_storage::clear_entity() {
    delete_where_(std::nullopt);
    decoded_sub_ = k_directory_sub_key;
    decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
    decoded_bytes_ = 0;
}

inline void fragment_index_storage::discard_sub_keys(index_sub_key_t lo, index_sub_key_t hi) {
    delete_where_(std::pair{lo, hi});
}

[[nodiscard]] inline std::size_t fragment_index_storage::run_buffer_bytes() const {
    if (run_buffer_override_bytes_ > 0)
        return run_buffer_override_bytes_;
    constexpr std::uint64_t k_floor = std::uint64_t{64} << 20;
    constexpr std::uint64_t k_ceiling = std::uint64_t{8} << 30;
    const std::uint64_t desired = std::uint64_t{2} << 30;
    std::uint64_t capped =
        bioseq::common::memory::default_budget().clamp_working_set(desired, k_floor, k_ceiling);
    const std::uint64_t index_cap =
        bioseq::parallelism::memory_admission_controller::instance().index_memory_cap_bytes();
    if (index_cap != 0) {
        capped = std::min(capped, index_cap);
    }
    return static_cast<std::size_t>(capped);
}

[[nodiscard]] inline sorted_pair_run_store& fragment_index_storage::sorted_runs_ensure_() {
    if (!sorted_runs_.has_value()) {
        sorted_runs_.emplace();
    }
    return *sorted_runs_;
}

} // namespace bioseq::storage::indexing
