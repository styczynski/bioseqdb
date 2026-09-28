/**
 * @file fragment_storage_spill.cppm
 * @brief Store sorted minimizer runs, regroup them by sequence position and reletse their resources.
 */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "utils/memutils.h"

#include "miscadmin.h"
}

// With Clang 22, include these libc++ declarations before importing modules
// that use them. Importing std first can leave container allocation with two
// conflicting declarations of aligned operator new
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.postgres.storage.indexing.chunked_storage:fragment_storage_spill;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.support; // PostgreSQL calls with C++ error handling.
import bioseq.parallelism.memory_admission;
import bioseq.postgres.storage.indexing.sorted_runs;
import :catalog;
import :fragment_image;
import :generation_writer;
import :generation_session;
import :hash_major_writer;
export import :fragment_storage_contract;
export namespace bioseq::storage::indexing {

/**
 * @brief Add a borrowed run to the index build's sorted-run store.
 * @param[in] sorted Pairs sorted by minimizer hash and encoded position.
 * @return Run  number, starting at zero.
 */
std::size_t fragment_index_storage::spill_sorted_run(
    std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run(sorted);
}

/**
 * @brief Give a sorted run to the store, keeping it in memory when the budget permits.
 * @param[in] sorted Owned pairs sorted by hash and encoded position.
 * @return Run number, starting at zero.
 */
std::size_t fragment_index_storage::spill_sorted_run_owned(
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted) {
    return sorted_runs_ensure_().spill_sorted_run_owned(std::move(sorted));
}

/**
 * @brief Transfer a sorted run together with its existing memory, reservation.
 * @param[in] sorted Owned pairs sorted by hash and encoded position.
 * @param[in] admitted_bytes Reserved bytes covering the vector capacity.
 * @param[in] permit Memory reservation transferred with the vector.
 * @return Run number, starting at zero.
 */
std::size_t fragment_index_storage::spill_sorted_run_owned_admitted(
    std::vector<std::pair<std::uint64_t, std::uint64_t>> sorted,
    std::uint64_t admitted_bytes,
    bioseq::parallelism::memory_permit permit) {
    return sorted_runs_ensure_().spill_sorted_run_owned_admitted(
        std::move(sorted), admitted_bytes, std::move(permit));
}

/**
 * @brief Move runs to temporary files to make room for the next  allocation.
 * @param[in] bytes_needed Additional bytes the caller wants to reserve.
 */
void fragment_index_storage::demote_resident_runs_until_available(std::uint64_t bytes_needed) {
    if (sorted_runs_.has_value()) {
        sorted_runs_->demote_resident_runs_until_available(bytes_needed);
    }
}

/**
 * @brief Start a new set of sorted runs grouped by target position.
 *
 * Close existing segment runs and assign a new token so replay data from an
 * earlier build cannot be reused. Exhausting the token counter returns an
 * invalid identity.
 *
 * @return New generation token with its mutation revisiow set to zero.
 */
direct_minimizer_spill_storage_binding fragment_index_storage::begin_segment_spill_generation() {
    if (segment_spill_pg_poisoned_) {
        minimizer_generation_storage_error_as_cpp(
            "poisoned minimizer-sequence spill storage reused after PostgreSQL error");
    }
    try {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_sorted_runs_.clear();
        active_segment_write_buffer_owner_.reset();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
    segment_sorted_runs_.clear();
    active_segment_write_buffer_owner_.reset();
    std::uint64_t token = next_segment_spill_generation_token_.load(std::memory_order_relaxed);
    for (;;) {
        if (token == 0) {
            segment_spill_binding_ = {};
            return segment_spill_binding_;
        }
        const std::uint64_t next =
            token == (std::numeric_limits<std::uint64_t>::max)() ? 0u : token + 1u;
        if (next_segment_spill_generation_token_.compare_exchange_weak(
                token, next, std::memory_order_relaxed, std::memory_order_relaxed)) {
            break;
        }
    }
    segment_spill_binding_ = direct_minimizer_spill_storage_binding{
        .generation_token = token,
        .mutation_revision = 0,
    };
    return segment_spill_binding_;
}

/// @brief Record changed spill contents, invalidating the identity if the revision overflows.
void fragment_index_storage::record_segment_spill_mutation_() noexcept {
    if (!segment_spill_binding_.valid()) {
        return;
    }
    if (segment_spill_binding_.mutation_revision == (std::numeric_limits<std::uint64_t>::max)()) {
        segment_spill_binding_ = {};
        return;
    }
    ++segment_spill_binding_.mutation_revision;
}

/**
 * @brief Add a sorted run to the store for one  target-position segment
 * @param[in] position_segment Target segment number, starting at zero.
 * @param[in] sorted Pairs sorted by minimizer hash and encoded position.
 * @return Run number within this segment, starting at zero.
 */
std::size_t fragment_index_storage::spill_sorted_segment_run(
    std::uint32_t position_segment,
    std::span<const std::pair<std::uint64_t, std::uint64_t>> sorted) {
    if (segment_spill_pg_poisoned_) {
        minimizer_generation_storage_error_as_cpp(
            "poisoned minimizer-sequence spill storage reused after PostgreSQL error");
    }
    if (!segment_spill_binding_.valid()) {
        (void)begin_segment_spill_generation();
    }
    auto iterator = std::lower_bound(segment_sorted_runs_.begin(),
                                     segment_sorted_runs_.end(),
                                     position_segment,
                                     [](const segment_run_entry_type& entry,
                                        std::uint32_t segment) { return entry.first < segment; });
    if (iterator == segment_sorted_runs_.end() || iterator->first != position_segment) {
        const std::size_t offset =
            static_cast<std::size_t>(iterator - segment_sorted_runs_.begin());
        if (!ensure_segment_run_metadata_capacity_(segment_sorted_runs_.size() + 1u)) {
            minimizer_generation_storage_error_as_cpp(
                "minimizer-sequence spill segment metadata admission failed");
        }
        iterator = segment_sorted_runs_.insert(
            segment_sorted_runs_.begin() + static_cast<std::ptrdiff_t>(offset),
            segment_run_entry_type{position_segment, sorted_pair_run_store{}});
    }
    try {
        if (!active_segment_write_buffer_owner_.has_value()) {
            // Check the destination writer state. The first spill allocates its buffer
            // when needed.
            if (!iterator->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer-sequence spill writer ownership is invalid");
            }
            active_segment_write_buffer_owner_ = position_segment;
        } else if (*active_segment_write_buffer_owner_ != position_segment) {
            // Inserting the destination may move vector entries. Find the source again
            // by segment number before transferring its write buffer.
            const auto source = std::lower_bound(
                segment_sorted_runs_.begin(),
                segment_sorted_runs_.end(),
                *active_segment_write_buffer_owner_,
                [](const segment_run_entry_type& entry, std::uint32_t segment) {
                    return entry.first < segment;
                });
            if (source == segment_sorted_runs_.end() ||
                source->first != *active_segment_write_buffer_owner_ ||
                !source->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer-sequence spill writer transfer is invalid");
            }
            active_segment_write_buffer_owner_ = position_segment;
        }
        // Adding this run changes the replay input, so advance  its revision. Moving
        // existing runs between memory and temporary files preserves their contents.
        const std::size_t run = iterator->second.spill_sorted_run(sorted);
        inject_minimizer_generation_failure_as_cpp(
            minimizer_generation_failure_injection::minimizer_sequence_spill,
            "minimizer_sequence_spill");
        record_segment_spill_mutation_();
        return run;
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        active_segment_write_buffer_owner_.reset();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

/**
 * @brief Regroup stored minimizers after the target segment size changes.
 *
 * Merge the old runs in batches, check, each position and send it to its new
 * segment. Replace the old stores after the vull pass succeeds. Temporary
 * batches and segment metadata are covered by memory reservations.
 *
 * @param[in] old_span Previous segment width in bases.
 * @param[in] new_span New segment width in bases.
 * @param[in] target_length Target length in bases.
 * @return True when regrouping succeeds, or false for invalid sizes, identity or positions.
 */
bool fragment_index_storage::reroute_segment_runs(std::uint64_t old_span,
                                                  std::uint64_t new_span,
                                                  std::uint64_t target_length) {
    if (!segment_spill_binding_.valid() || old_span == 0 || new_span == 0) {
        if (segment_spill_pg_poisoned_) {
            minimizer_generation_storage_error_as_cpp(
                "poisoned minimizer-sequence reroute storage reused after PostgreSQL error");
        }
        return false;
    }
    // Keep  the replacement segment list with the memory reservation for its capacity.
    struct admitted_segment_runs {
        bioseq::parallelism::memory_permit permit;
        std::vector<segment_run_entry_type> values;
    } rerouted;
    std::optional<std::uint32_t> rerouted_active_write_buffer_owner;
    // Reuse one reserved batch while copying pairs into their new segments.
    struct admitted_pair_batch {
        bioseq::parallelism::memory_permit permit;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> values;
    } routed_batch;
    // Reserve space for both the old and replacement allocations before growing a vector
    const auto ensure_capacity = []<class p_value_type>(bioseq::parallelism::memory_permit& permit,
                                                        std::vector<p_value_type>& values,
                                                        std::size_t required) -> bool {
        const std::uint64_t retained =
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(values.capacity()),
                                           static_cast<std::uint64_t>(sizeof(p_value_type)));
        if (retained == (std::numeric_limits<std::uint64_t>::max)() ||
            (retained != 0u && (!permit || permit.bytes() < retained))) {
            return false;
        }
        if (values.capacity() >= required) {
            return true;
        }
        const std::uint64_t headroom = bioseq::common::saturating_mul(
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(required),
                                           static_cast<std::uint64_t>(sizeof(p_value_type))),
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = bioseq::common::saturating_add(retained, headroom);
        if (headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        if (permit) {
            if (!permit.grow_to(peak)) {
                return false;
            }
        } else {
            auto acquired =
                bioseq::parallelism::memory_admission_controller::instance().try_reserve(peak);
            if (!acquired) {
                return false;
            }
            permit = std::move(*acquired);
        }
        values.reserve(required);
        const std::uint64_t exact =
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(values.capacity()),
                                           static_cast<std::uint64_t>(sizeof(p_value_type)));
        if (exact == (std::numeric_limits<std::uint64_t>::max)() || exact > permit.bytes()) {
            return false;
        }
        permit.shrink_to(exact);
        return true;
    };
    // Find or create the run store for the new segment.
    const auto rerouted_store =
        [&rerouted,
         &rerouted_active_write_buffer_owner,
         &ensure_capacity](std::uint32_t segment) -> sorted_pair_run_store& {
        auto iterator = std::lower_bound(rerouted.values.begin(),
                                         rerouted.values.end(),
                                         segment,
                                         [](const segment_run_entry_type& entry,
                                            std::uint32_t value) { return entry.first < value; });
        if (iterator == rerouted.values.end() || iterator->first != segment) {
            const std::size_t offset = static_cast<std::size_t>(iterator - rerouted.values.begin());
            if (!ensure_capacity(rerouted.permit, rerouted.values, rerouted.values.size() + 1u)) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer-sequence reroute segment metadata admission failed");
            }
            iterator = rerouted.values.insert(
                rerouted.values.begin() + static_cast<std::ptrdiff_t>(offset),
                segment_run_entry_type{segment, sorted_pair_run_store{}});
        }
        if (!rerouted_active_write_buffer_owner.has_value()) {
            if (!iterator->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer-sequence reroute writer ownership is invalid");
            }
            rerouted_active_write_buffer_owner = segment;
        } else if (*rerouted_active_write_buffer_owner != segment) {
            // Insertion may have moved the source entry. Find it again by segment number
            // before transferring the write buffer.
            const auto source = std::lower_bound(
                rerouted.values.begin(),
                rerouted.values.end(),
                *rerouted_active_write_buffer_owner,
                [](const segment_run_entry_type& entry, std::uint32_t value) {
                    return entry.first < value;
                });
            if (source == rerouted.values.end() ||
                source->first != *rerouted_active_write_buffer_owner ||
                !source->second.try_transfer_completed_write_buffer_to(iterator->second)) {
                minimizer_generation_storage_error_as_cpp(
                    "minimizer-sequence reroute writer transfer is invalid");
            }
            rerouted_active_write_buffer_owner = segment;
        }
        return iterator->second;
    };
    bool valid = true;
    try {
        // Release completed write buffers before reserving memory  for readers and new runs.
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_completed_write_buffer();
        }
        active_segment_write_buffer_owner_.reset();
        for (auto& [old_segment, runs] : segment_sorted_runs_) {
            runs.merge_runs([&valid,
                             new_span,
                             target_length,
                             old_span,
                             old_segment,
                             &routed_batch,
                             &ensure_capacity,
                             &rerouted_store](std::uint64_t hash, auto&& positions) {
                positions.for_each_batch([&valid,
                                          new_span,
                                          target_length,
                                          old_span,
                                          old_segment,
                                          hash,
                                          &routed_batch,
                                          &ensure_capacity,
                                          &rerouted_store](std::span<const std::uint64_t> batch) {
                    if (!valid) {
                        return;
                    }
                    std::size_t offset = 0;
                    while (offset < batch.size()) {
                        const std::uint64_t first_encoded = batch[offset];
                        const std::uint64_t first_position = first_encoded >> 1u;
                        const std::uint64_t first_segment = first_position / new_span;
                        if (first_position >= target_length ||
                            first_position / old_span != old_segment ||
                            first_segment > (std::numeric_limits<std::uint32_t>::max)()) {
                            valid = false;
                            return;
                        }
                        std::size_t end = offset + 1u;
                        for (; end < batch.size(); ++end) {
                            const std::uint64_t position = batch[end] >> 1u;
                            const std::uint64_t new_segment = position / new_span;
                            if (position >= target_length || position / old_span != old_segment ||
                                new_segment > (std::numeric_limits<std::uint32_t>::max)()) {
                                valid = false;
                                return;
                            }
                            if (new_segment != first_segment) {
                                break;
                            }
                        }
                        const std::size_t count = end - offset;
                        if (!ensure_capacity(routed_batch.permit, routed_batch.values, count)) {
                            minimizer_generation_storage_error_as_cpp(
                                "minimizer-sequence reroute pair batch admission failed");
                        }
                        routed_batch.values.clear();
                        for (std::size_t i = offset; i < end; ++i) {
                            const std::uint64_t encoded_position = batch[i];
                            routed_batch.values.emplace_back(hash, encoded_position);
                        }
                        rerouted_store(static_cast<std::uint32_t>(first_segment))
                            .spill_sorted_run(
                                std::span<const std::pair<std::uint64_t, std::uint64_t>>{
                                    routed_batch.values});
                        offset = end;
                    }
                    // Each batch keeps the original hash and position order.
                });
            });
            if (!valid) {
                active_segment_write_buffer_owner_.reset();
                segment_spill_binding_ = {};
                segment_spill_pg_poisoned_ = true;
                return false;
            }
        }
        // All old runs have been copied. Close their files before replacing the stores,
        // and keep the new stores open for the remaining build steps.
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
    } catch (const bioseq::postgres::support::postgres_error&) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        for (auto& [segment, runs] : rerouted.values) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        active_segment_write_buffer_owner_.reset();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    } catch (...) {
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs_noexcept();
        }
        for (auto& [segment, runs] : rerouted.values) {
            (void)segment;
            runs.release_runs_noexcept();
        }
        active_segment_write_buffer_owner_.reset();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
    segment_sorted_runs_.swap(rerouted.values);
    std::swap(segment_sorted_runs_metadata_permit_, rerouted.permit);
    active_segment_write_buffer_owner_ = rerouted_active_write_buffer_owner;
    record_segment_spill_mutation_();
    return true;
}

/// @brief Return statistics for the general run store, or zero counters if it is absent.
sorted_run_store_stats fragment_index_storage::sorted_run_stats() const noexcept {
    return sorted_runs_.has_value() ? sorted_runs_->stats() : sorted_run_store_stats{};
}

/**
 * @brief Save general-run statistis for SQL diagnostics before releasing the store.
 *
 * Record the sequence identity along with memory, temporary-file and  merge
 * counts while the completed build's resources are still available.
 */
void fragment_index_storage::snapshot_sorted_run_stats_before_release() {
    if (!sorted_runs_.has_value()) {
        return;
    }
    const sorted_run_store_stats st = sorted_runs_->stats();
    const std::uint64_t total_runs = sorted_runs_->run_count();
    g_last_index_merge_run_count = total_runs;
    g_last_minimizer_sequence_build_stats = last_minimizer_sequence_build_stats{
        .backend_pid = static_cast<std::uint64_t>(MyProcPid),
        .db_oid = static_cast<std::uint64_t>(db_oid_),
        .relfilenode = static_cast<std::uint64_t>(main_relfilenode_),
        .sequence_rowno = static_cast<std::uint64_t>(query_key_),
        .total_run_count = total_runs,
        .resident_runs = st.resident_runs,
        .file_runs = st.file_runs,
        .resident_run_bytes = st.resident_bytes,
        .resident_peak_bytes = st.resident_peak_bytes,
        .spilled_run_bytes = st.spilled_bytes,
        .postgres_temp_file_bytes = st.postgres_temp_file_bytes,
        .postgres_temp_file_count = st.postgres_temp_file_count,
        .merge_reader_buffer_bytes = st.merge_reader_buffer_bytes,
        .merge_pass_count = st.merge_pass_count,
        .spill_count_from_pressure = st.spill_count_from_pressure,
        .resident_demotions = st.resident_demotions,
        .position_spill_bytes = st.position_spill_bytes,
        .position_spill_count = st.position_spill_count,
    };
    bioseq_log<logging_level::debug>(
        "[bioseq.index.minimizer_sequence_build.row] backend_pid={} db_oid={} relfilenode={} "
        "sequence_rowno={} "
        "resident_runs={} file_runs={} resident_run_bytes={} resident_peak_bytes={} "
        "spilled_run_bytes={} postgres_temp_file_bytes={} postgres_temp_file_count={} "
        "merge_reader_buffer_bytes={} merge_pass_count={} spill_count_from_pressure={} "
        "resident_demotions={} position_spill_bytes={} position_spill_count={}",
        MyProcPid,
        static_cast<unsigned long long>(db_oid_),
        static_cast<unsigned long long>(main_relfilenode_),
        static_cast<unsigned long long>(query_key_),
        static_cast<unsigned long long>(st.resident_runs),
        static_cast<unsigned long long>(st.file_runs),
        static_cast<unsigned long long>(st.resident_bytes),
        static_cast<unsigned long long>(st.resident_peak_bytes),
        static_cast<unsigned long long>(st.spilled_bytes),
        static_cast<unsigned long long>(st.postgres_temp_file_bytes),
        static_cast<unsigned long long>(st.postgres_temp_file_count),
        static_cast<unsigned long long>(st.merge_reader_buffer_bytes),
        static_cast<unsigned long long>(st.merge_pass_count),
        static_cast<unsigned long long>(st.spill_count_from_pressure),
        static_cast<unsigned long long>(st.resident_demotions),
        static_cast<unsigned long long>(st.position_spill_bytes),
        static_cast<unsigned long long>(st.position_spill_count));
}

/// @brief Drop, run handles without PostgreSQL calls and invalidate saved replay state.
void fragment_index_storage::release_runs_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->release_runs_noexcept();
        sorted_runs_.reset();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.release_runs_noexcept();
    }
    segment_sorted_runs_.clear();
    active_segment_write_buffer_owner_.reset();
    record_segment_spill_mutation_();
}

/**
 * @brief Close the general and segment run stores.
 *
 * If PostgreSQL reports an error, mark the spill state unusable and leave its
 * remaining PostgreSQL resources to transaction cleanup.
 */
void fragment_index_storage::release_runs() {
    try {
        if (sorted_runs_.has_value()) {
            sorted_runs_->release_runs();
            sorted_runs_.reset();
        }
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.release_runs();
        }
        segment_sorted_runs_.clear();
        active_segment_write_buffer_owner_.reset();
        record_segment_spill_mutation_();
    } catch (const bioseq::postgres::support::postgres_error&) {
        if (sorted_runs_.has_value()) {
            sorted_runs_->abandon_after_pg_error_noexcept();
            sorted_runs_.reset();
        }
        for (auto& [segment, runs] : segment_sorted_runs_) {
            (void)segment;
            runs.abandon_after_pg_error_noexcept();
        }
        segment_sorted_runs_.clear();
        active_segment_write_buffer_owner_.reset();
        segment_spill_binding_ = {};
        segment_spill_pg_poisoned_ = true;
        throw;
    }
}

/**
 * @brief Leave PostgreSQL resources to transaction cleanup after an error.
 *
 * Clear borrowed handles and release C++ buffers. PostgreSQL will reclaim its
 * memory contexts and temporary files when the transaction aborts.
 */
void fragment_index_storage::abandon_after_pg_error_noexcept() noexcept {
    if (sorted_runs_.has_value()) {
        sorted_runs_->abandon_after_pg_error_noexcept();
        sorted_runs_.reset();
    }
    for (auto& [segment, runs] : segment_sorted_runs_) {
        (void)segment;
        runs.abandon_after_pg_error_noexcept();
    }
    segment_sorted_runs_.clear();
    active_segment_write_buffer_owner_.reset();
    segment_spill_binding_ = {};
    segment_spill_pg_poisoned_ = true;

    // PostgreSQL error cleanup owns these contexts and buffers now Clear our
    // pointers so later  C++ destruction cannot try to use them.
    own_mctx_ = nullptr;
    index_relid_ = InvalidOid;
    decoded_ = nullptr;
    decoded_units_ = 0;
    decoded_bytes_ = 0;
    decoded_sub_ = k_directory_sub_key;
    decoded_chunk_ = (std::numeric_limits<std::size_t>::max)();
    scratch_ = nullptr;
    scratch_units_ = 0;
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_payload_bytes_.clear();
}

/// @brief Closes run stores and deletes the handle-owned memory context.
void fragment_index_storage::close_checked() {
    release_runs();
    chunk_cache_.clear();
    chunk_cache_bytes_ = 0;
    chunk_payload_bytes_.clear();
    if (own_mctx_ != nullptr) {
        MemoryContext finished_context = std::exchange(own_mctx_, nullptr);
        decoded_ = nullptr;
        scratch_ = nullptr;
        bioseq::postgres::support::invoke_pg_as_cpp(
            [finished_context] { MemoryContextDelete(finished_context); });
    }
}

/**
 * @brief Write an encoded index chunk using a temporary storage handle.
 *
 * Close the handle after a successful write, and release C++ objects before
 * rethrowing a PostgreSQL error so transaction cleanup can reclaim its resources.
 *
 * @param[in] user_relid User table OID.
 * @param[in] db_oid Database OID.
 * @param[in] tablespace_oid Tablespace OID.
 * @param[in] main_relfilenode Main table file number.
 * @param[in] column_attno Sequence column number, starting at one.
 * @param[in] index_name Index name borrowed for tdis call.
 * @param[in] bucket_idx Storage bucket.
 * @param[in] shard_idx Storage shard.
 * @param[in] sequence_rowno Sequence row number.
 * @param[in] sub_key Chunk group within the sequence index.
 * @param[in] chunk_no Chunk number, starting  at zero.
 * @param[in] bytes Encoded payload copied into PostgreSQL storage.
 */
inline void write_serialized_index_chunk_pg(Oid user_relid,
                                            Oid db_oid,
                                            Oid tablespace_oid,
                                            Oid main_relfilenode,
                                            std::int16_t column_attno,
                                            std::string_view index_name,
                                            std::uint8_t bucket_idx,
                                            std::uint32_t shard_idx,
                                            std::uint64_t sequence_rowno,
                                            index_sub_key_t sub_key,
                                            std::uint32_t chunk_no,
                                            std::span<const char> bytes) {
    ErrorData* postgres_error = nullptr;
    {
        fragment_index_storage storage;
        storage.db_oid_ = db_oid;
        storage.tablespace_oid_ = tablespace_oid;
        storage.main_relfilenode_ = main_relfilenode;
        storage.user_relid_ = user_relid;
        storage.column_attno_ = column_attno;
        storage.index_name_ = index_name;
        storage.bucket_idx_ = bucket_idx;
        storage.shard_idx_ = shard_idx;
        storage.query_key_ = sequence_rowno;
        storage.own_mctx_ = bioseq::postgres::support::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext, "bioseq_async_index_persist", ALLOCSET_SMALL_SIZES);
        });
        try {
            storage.write_serialized_chunk_checked(sequence_rowno, sub_key, chunk_no, bytes, false);
            storage.close_checked();
        } catch (bioseq::postgres::support::postgres_error& error) {
            storage.abandon_after_pg_error_noexcept();
            postgres_error = error.release_error_data();
        } catch (...) {
            try {
                storage.close_checked();
            } catch (...) {
                storage.abandon_after_pg_error_noexcept();
            }
            throw;
        }
    }
    if (postgres_error != nullptr) {
        ReThrowError(postgres_error);
    }
}

void mark_serialized_index_write_successful_pg(Oid user_relid,
                                               Oid main_relfilenode,
                                               std::int16_t column_attno,
                                               std::string_view index_name) {
    static_cast<void>(
        upsert_metadata_age_log_pg(user_relid, main_relfilenode, column_attno, index_name));
}

} // namespace bioseq::storage::indexing
