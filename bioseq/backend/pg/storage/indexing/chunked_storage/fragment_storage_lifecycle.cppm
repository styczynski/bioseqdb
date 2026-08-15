/**
 * @file fragment_storage_lifecycle.cppm
 * @brief Fragment storage resource ownership, validation, and residency transitions.
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
export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_lifecycle;
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
export namespace bioseq::storage::indexing {

[[nodiscard]] inline bool
fragment_index_storage::ensure_segment_run_metadata_capacity_(std::size_t required) {
    const std::uint64_t retained =
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(segment_sorted_runs_.capacity()),
                                       static_cast<std::uint64_t>(sizeof(segment_run_entry_type)));
    if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
        (retained != 0u && (!segment_sorted_runs_metadata_permit_ ||
                            segment_sorted_runs_metadata_permit_.bytes() < retained))) {
        return false;
    }
    if (segment_sorted_runs_.capacity() >= required) {
        return true;
    }
    const std::uint64_t headroom = bioseq::common::saturating_mul(
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(required),
                                       static_cast<std::uint64_t>(sizeof(segment_run_entry_type))),
        bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t peak = bioseq::common::saturating_add(retained, headroom);
    if (headroom == (std::numeric_limits<std::uint64_t>::max)() ||
        peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (segment_sorted_runs_metadata_permit_) {
        if (!segment_sorted_runs_metadata_permit_.grow_to(peak)) {
            return false;
        }
    } else {
        auto permit =
            bioseq::parallelism::memory_admission_controller::instance().try_reserve(peak);
        if (!permit) {
            return false;
        }
        segment_sorted_runs_metadata_permit_ = std::move(*permit);
    }
    segment_sorted_runs_.reserve(required);
    const std::uint64_t exact =
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(segment_sorted_runs_.capacity()),
                                       static_cast<std::uint64_t>(sizeof(segment_run_entry_type)));
    if (exact == (std::numeric_limits<std::uint64_t>::max)() ||
        exact > segment_sorted_runs_metadata_permit_.bytes()) {
        return false;
    }
    segment_sorted_runs_metadata_permit_.shrink_to(exact);
    return true;
}

inline void fragment_index_storage::steal_(fragment_index_storage&& o) noexcept {
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
    decoded_sub_ = o.decoded_sub_;
    decoded_chunk_ = o.decoded_chunk_;
    chunk_cache_ = std::move(o.chunk_cache_);
    chunk_cache_clock_ = o.chunk_cache_clock_;
    chunk_cache_bytes_ = o.chunk_cache_bytes_;
    scratch_ = o.scratch_;
    scratch_units_ = o.scratch_units_;
    run_buffer_override_bytes_ = o.run_buffer_override_bytes_;
    chunk_payload_bytes_ = std::move(o.chunk_payload_bytes_);
    sorted_runs_.swap(o.sorted_runs_);
    segment_sorted_runs_.swap(o.segment_sorted_runs_);
    segment_sorted_runs_metadata_permit_ = std::move(o.segment_sorted_runs_metadata_permit_);
    segment_spill_binding_ = o.segment_spill_binding_;
    segment_spill_pg_poisoned_ = o.segment_spill_pg_poisoned_;

    // A moved-from fragment is not merely destruction-safe: it is fully
    // unbound and cannot still describe or access the transferred entity.
    o.db_oid_ = InvalidOid;
    o.tablespace_oid_ = InvalidOid;
    o.main_relfilenode_ = InvalidOid;
    o.user_relid_ = InvalidOid;
    o.read_caches_ = nullptr;
    o.column_attno_ = 0;
    o.index_name_ = {};
    o.bucket_idx_ = 0;
    o.shard_idx_ = 0;
    o.query_key_ = 0;
    o.own_mctx_ = nullptr;
    o.index_relid_ = InvalidOid;
    o.decoded_ = nullptr;
    o.decoded_units_ = 0;
    o.decoded_bytes_ = 0;
    o.decoded_sub_ = k_directory_sub_key;
    o.decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
    {
        std::vector<cached_chunk> empty;
        o.chunk_cache_.swap(empty);
    }
    o.chunk_cache_clock_ = 0;
    o.chunk_cache_bytes_ = 0;
    o.scratch_ = nullptr;
    o.scratch_units_ = 0;
    o.run_buffer_override_bytes_ = 0;
    o.chunk_payload_bytes_.clear();
    o.segment_spill_binding_ = {};
    o.segment_spill_pg_poisoned_ = true;
    o.sorted_runs_.reset();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        o.segment_sorted_runs_, o.segment_sorted_runs_metadata_permit_);
}

inline void fragment_index_storage::release_resident_runs_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->release_resident_runs_noexcept();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.release_resident_runs_noexcept();
    }
    // sorted_pair_run_store::release_resident_runs_noexcept drops logical
    // memory-only runs; unlike its internal memory-to-BufFile compaction,
    // this is not representation-preserving and must invalidate replay.
    record_segment_spill_mutation_();
}

inline void fragment_index_storage::release_() noexcept {
    DEBUG_ASSERT(own_mctx_ == nullptr,
                 "fragment_index_storage must be close_checked or abandoned before destruction");
    DEBUG_ASSERT(
        !sorted_runs_.has_value() && segment_sorted_runs_.empty(),
        "fragment_index_storage run stores must be close_checked or abandoned before destruction");
    release_runs_noexcept();
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    // Destruction is an unwind-only abandonment boundary. A normal owner
    // must call close_checked(); a failing owner must call
    // abandon_after_pg_error_noexcept(). The parent PostgreSQL context
    // reclaims abandoned storage at statement/transaction abort.
    own_mctx_ = nullptr;
    decoded_ = nullptr;
    scratch_ = nullptr;
}

[[noreturn]] inline void fragment_index_storage::index_error(std::string_view message) const {
    const char* message_data = message.empty() ? "" : message.data();
    const int message_size = static_cast<int>(
        std::min(message.size(), static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    bioseq::postgres::support::invoke_pg_as_cpp([message_data, message_size] {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq index: %.*s", message_size, message_data)));
    });
    pg_unreachable();
}

inline void fragment_index_storage::validate_minimizer_directory_or_error(const char* p,
                                                                          std::size_t bytes) const {
    if (bytes < 40)
        index_error("directory truncated");
    std::uint32_t magic{};
    std::uint32_t format_revision{};
    std::memcpy(&magic, p, 4);
    std::memcpy(&format_revision, p + 4, 4);
    if (magic != 0xB1000002u || format_revision != 2u) {
        index_error("directory magic/format revision");
    }
}

inline void fragment_index_storage::validate_minimizer_chunk_metadata_or_error(
    index_sub_key_t sk,
    std::size_t chunk_no,
    std::size_t min_bytes,
    std::size_t /*alignment*/) const {
    if (chunk_byte_size(sk, chunk_no) < min_bytes)
        index_error("chunk metadata below minimum size");
}

[[nodiscard]] inline std::string fragment_index_storage::this_table_name() const {
    return format_seqidx_name(index_name_,
                              db_oid_,
                              tablespace_oid_,
                              main_relfilenode_,
                              column_attno_,
                              bucket_idx_,
                              shard_idx_);
}

inline void fragment_index_storage::evict_generation_storage_residency_() {
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_cache_clock_ = 0;
    decoded_sub_ = k_directory_sub_key;
    decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
    decoded_bytes_ = 0;
    evict_index_residency(
        db_oid_,
        user_relid_,
        tablespace_oid_,
        main_relfilenode_,
        column_attno_,
        index_name_hash(db_oid_, tablespace_oid_, main_relfilenode_, column_attno_, index_name_));
}

[[nodiscard]] inline direct_minimizer_spill_storage_binding
fragment_index_storage::segment_spill_storage_binding() const noexcept {
    return segment_spill_binding_;
}

} // namespace bioseq::storage::indexing
