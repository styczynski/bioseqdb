/**
 * @file fragment_storage_lifecycle.cppm
 * @brief Move and release fragment index resources, check stored headers and clear cached data.
 */
module;
#include <bioseq/pg_compat.h>
#include <libassert/assert.hpp>

extern "C" {
#include "utils/builtins.h"
}
export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_lifecycle;
import std;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support; // PostgreSQL calls with C++ error handling.
import bioseq.parallelism.memory_admission;
import bioseq.postgres.storage.indexing.residency;
import bioseq.postgres.storage.indexing.sorted_runs;
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
export import :fragment_storage_contract;
export namespace bioseq::storage::indexing {

/**
 * @brief Reserve memory for the list of segment run stores before growing it.
 * @param[in] required Minimum number of segment entries.
 * @return True if the vector capacity and memory reservation cover the request.
 */
[[nodiscard]] bool
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

/**
 * @brief Move the index identity and resources into this handle.
 *
 * Clear the source handles and identity so destroying the moved-from object
 * cannot release resources now owned by the destination.
 *
 * @param[in,out] o Handle whose state is transferred.
 */
void fragment_index_storage::steal_(fragment_index_storage&& o) noexcept {
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
    active_segment_write_buffer_owner_ = o.active_segment_write_buffer_owner_;
    segment_spill_binding_ = o.segment_spill_binding_;
    segment_spill_pg_poisoned_ = o.segment_spill_pg_poisoned_;

    // Clear the source identity and resource pointers after transferring ownership.
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
    o.active_segment_write_buffer_owner_.reset();
    o.segment_spill_binding_ = {};
    o.segment_spill_pg_poisoned_ = true;
    o.sorted_runs_.reset();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        o.segment_sorted_runs_, o.segment_sorted_runs_metadata_permit_);
}

/**
 * @brief Discard run data kept in memory without calling PostgreSQL.
 *
 * Runs that existed only in memory are lost, so saved segment replay state
 * becomes invalid.
 */
void fragment_index_storage::release_resident_runs_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->release_resident_runs_noexcept();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.release_resident_runs_noexcept();
    }
    // These runs are discarded without being written to temporary files.
    // Invalidate saved replay state because its input has changed.
    record_segment_spill_mutation_();
}

/**
 * @brief Finish destruction after PostgreSQL resources have been closed or abandoned.
 *
 * Call close_checked() on normal paths, or abandon_after_pg_error_noexcept()
 * after a PostgreSQL error, before destroying the handle.
 */
void fragment_index_storage::release_() noexcept {
    DEBUG_ASSERT(own_mctx_ == nullptr,
                 "fragment_index_storage must be close_checked or abandoned before destruction");
    DEBUG_ASSERT(
        !sorted_runs_.has_value() && segment_sorted_runs_.empty(),
        "fragment_index_storage run stores must be close_checked or abandoned before destruction");
    release_runs_noexcept();
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    // The caller has already closed these resources or left them to PostgreSQL
    // error cleanup. Clear the remaining borrowed pointers.
    own_mctx_ = nullptr;
    decoded_ = nullptr;
    scratch_ = nullptr;
}

/**
 * @brief Report corrupt index data as a PostgreSQL error wrapped in a C++ exception.
 * @param[in] message Error text, shortened if its length exceeds the formatter
 * precision limit.
 */
[[noreturn]] void fragment_index_storage::index_error(std::string_view message) const {
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

/**
 * @brief Check that a minimizer directory has a complete header and supported format.
 * @param[in] p Stored directory bytes.
 * @param[in] bytes Available byte count.
 */
void fragment_index_storage::validate_minimizer_directory_or_error(const char* p,
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

/**
 * @brief Check that a stored chunk contains at least the requested number of bytes.
 * @param[in] sk Sub-key identifying the chunk group.
 * @param[in] chunk_no Chunk number, starting at zero.
 * @param[in] min_bytes Minimum required payload size.
 */
void fragment_index_storage::validate_minimizer_chunk_metadata_or_error(
    index_sub_key_t sk,
    std::size_t chunk_no,
    std::size_t min_bytes,
    std::size_t /*alignment*/) const {
    if (chunk_byte_size(sk, chunk_no) < min_bytes)
        index_error("chunk metadata below minimum size");
}

/**
 * @brief Build the fragment index table name from this handle's storage identity.
 * @return Table name in the extension's internal schema.
 */
[[nodiscard]] std::string fragment_index_storage::this_table_name() const {
    return format_seqidx_name(index_name_,
                              db_oid_,
                              tablespace_oid_,
                              main_relfilenode_,
                              column_attno_,
                              bucket_idx_,
                              shard_idx_);
}

/**
 * @brief Invalidate cached chunks and the decoded-buffer identity.
 *
 * Reset the handle cache, then remove entries from the backend index cache
 * using the owning table, column and index name.
 */
void fragment_index_storage::evict_generation_storage_residency_() {
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

/// @brief Return a copy of the current segment spill identity.
[[nodiscard]] direct_minimizer_spill_storage_binding
fragment_index_storage::segment_spill_storage_binding() const noexcept {
    return segment_spill_binding_;
}

} // namespace bioseq::storage::indexing
