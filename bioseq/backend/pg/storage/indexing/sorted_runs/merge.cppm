/**
 * @file merge.cppm
 * @brief Merge sorted runs, reduce the number of temporary files, and release build resources.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/buffile.h"

// Keep the PostgreSQL error implementation in a separate module unit
// so this declaration can be compiled by both supported compilers.
[[noreturn]] void bioseq_sorted_run_error_pg_leaf(unsigned error_code);
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

module bioseq.postgres.storage.indexing.sorted_runs;

import bioseq.common.checked_arithmetic;
import bioseq.postgres.support;
import bioseq.parallelism.memory_admission;

import :sorted_runs_storage;

namespace bioseq::storage::indexing {

/**
 * @brief Reserve memory and prepare readers for one group of files to merge.
 *
 * Create the output file only after this succeeds. If the memory budget refuses
 * the request, the caller  can try fewer input files without undoing a partial write.
 */
std::unique_ptr<admitted_file_merge> sorted_pair_run_store::try_prepare_compaction_merge_(
    std::span<buffer_file_run> runs,
    bioseq::parallelism::memory_admission_failure& terminal_failure,
    sorted_run_merge_admission_site& terminal_site) {
    terminal_failure = {};

    // Reserve the merge queue first. Reader buffers can shrink to fit the
    // remaining budget, while the queue needs one entry per input file
    terminal_site = sorted_run_merge_admission_site::merge_head_queue;
    auto heap_admission = try_make_admitted_merge_head_queue(runs.size());
    if (!heap_admission.admitted.has_value()) {
        terminal_failure = heap_admission.failure;
        return nullptr;
    }

    auto prepared = std::make_unique<admitted_file_merge>();
    prepared->heap.emplace(std::move(*heap_admission.admitted));
    merge_resources& resources = prepared->resources;

    terminal_site = sorted_run_merge_admission_site::compaction_readers;
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::uint64_t preferred_per_reader_bytes =
        std::clamp<std::uint64_t>(reader_budget / std::max<std::size_t>(1u, runs.size()),
                                  minimum_merge_reader_buffer_bytes,
                                  maximum_merge_reader_buffer_bytes);
    auto reader_budget_admission = try_admit_file_reader_budget(
        runs.size(), preferred_per_reader_bytes, &terminal_failure);
    if (!reader_budget_admission) {
        return nullptr;
    }
    const std::uint64_t per_reader_bytes = reader_budget_admission->per_reader_bytes;
    resources.reader_budget_permit = std::move(reader_budget_admission->permit);
    resources.readers.reserve(runs.size());
    for (buffer_file_run& run : runs) {
        file_reader reader{};
        reader.file = run.file;
        reader.remaining = run.pair_count;
        reader.buffer.reserve(static_cast<std::size_t>(
            std::max<std::uint64_t>(1u, per_reader_bytes / sizeof(sorted_pair_wire))));
        resources.readers.push_back(std::move(reader));
    }
    const std::uint64_t exact_reader_bytes = file_reader_owned_capacity_bytes(resources.readers);
    if (exact_reader_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        exact_reader_bytes > resources.reader_budget_permit.bytes()) {
        resources.finish_success();
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    resources.reader_budget_permit.shrink_to(exact_reader_bytes);

    for (buffer_file_run& run : runs) {
        if (seek_buffile_as_cpp(run.file, 0, 0, SEEK_SET) != 0) {
            report_sorted_run_error(sorted_run_error::compacted_run_rewind_failed);
        }
    }
    stats_.merge_reader_buffer_bytes += exact_reader_bytes;
    return prepared;
}

/** Merge the prefared file readers and pass each sorted pair to the writer. */
template <class p_writer>
void sorted_pair_run_store::merge_sorted_file_runs_to_writer_(
    merge_resources& resources,
    admitted_merge_head_queue& admitted_heap,
    p_writer&& writer) {
    merge_head_queue& heap = admitted_heap.queue;
    for (std::size_t idx = 0; idx < resources.readers.size(); ++idx) {
        file_reader& reader = resources.readers[idx];
        if (reader.read_next()) {
            heap.push(merge_head{
                .kmer = reader.current.kmer,
                .encoded = reader.current.encoded,
                .run_index = idx,
                .file = true,
            });
        }
    }
    while (!heap.empty()) {
        merge_head head = heap.top();
        heap.pop();
        writer(sorted_pair_wire{.kmer = head.kmer, .encoded = head.encoded});
        file_reader& reader = resources.readers[head.run_index];
        if (reader.read_next()) {
            heap.push(merge_head{
                .kmer = reader.current.kmer,
                .encoded = reader.current.encoded,
                .run_index = head.run_index,
                .file = true,
            });
        }
    }
}

/**
 * @brief Merge a prepared group of runs into one temporary file.
 * @param[in,out] admitted Readers and merge queue with memory already reserved.
 * @return File run that can be rewound for another merge.
 */
buffer_file_run sorted_pair_run_store::merge_file_run_group_to_new_buffile_(
    admitted_file_merge& admitted) {
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::postgres::support::invoke_pg_as_cpp([] {
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::compact_create_before);
    });
#endif
    BufFile* out_file = create_temp_buffile_as_cpp();
    if (out_file == nullptr) {
        report_sorted_run_error(sorted_run_error::compact_temp_file_create_failed);
    }
    merge_resources& resources = admitted.resources;
    buffer_file_run out{.file = out_file};
    try {
        merge_sorted_file_runs_to_writer_(
            resources,
            *admitted.heap,
            [out_file, &out](sorted_pair_wire pair) {
                write_buffile_as_cpp(out_file, &pair, sizeof(pair));
                ++out.pair_count;
                out.byte_count += sizeof(pair);
            });
        ++stats_.postgres_temp_file_count;
        const int64 size = size_buffile_as_cpp(out.file);
        if (size > 0) {
            stats_.postgres_temp_file_bytes += static_cast<std::uint64_t>(size);
        }
        resources.finish_success();
        out_file = nullptr;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources.abandon_pg_resources_noexcept();
        // out_file remains registered with the current ResourceOwner.
        out_file = nullptr;
        throw;
    } catch (...) {
        // Leave file cleanup to ResourceOwner so a close, failure  cannot replace
        // the C++ exception being propagated.
        resources.abandon_pg_resources_noexcept();
        abandon_buffile_noexcept(out_file);
        throw;
    }
    return out;
}

/**
 * @brief Merge groups of files until at most target_output_runs remain.
 * @param[in] target_output_runs Maximum file count, treated as at least one.
 */
void sorted_pair_run_store::compact_file_runs_to_output_count_(std::size_t target_output_runs) {
    target_output_runs = std::max<std::size_t>(1, target_output_runs);
    while (file_runs_.size() > target_output_runs) {
        const std::size_t input_run_count = file_runs_.size();
        std::size_t group_size = std::max<std::size_t>(
            2, bioseq::common::ceil_div(input_run_count, target_output_runs));
        admitted_owned_vector<buffer_file_run> admitted_next;
        std::unique_ptr<admitted_file_merge> first_group;
        bioseq::parallelism::memory_admission_failure admission_failure{};
        sorted_run_merge_admission_site admission_site =
            sorted_run_merge_admission_site::compaction_readers;
        for (;;) {
            const std::size_t next_output_runs =
                bioseq::common::ceil_div(input_run_count, group_size);
            if (!try_ensure_admitted_vector_capacity(admitted_next, next_output_runs)) {
                report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
            }
            const std::size_t first_hi = std::min(input_run_count, group_size);
            first_group = try_prepare_compaction_merge_(
                std::span<buffer_file_run>(file_runs_.data(), first_hi),
                admission_failure,
                admission_site);
            if (first_group) {
                break;
            }
            const bool can_reduce_fan_in =
                admission_failure.reason ==
                    bioseq::parallelism::memory_admission_failure_reason::budget_exhausted &&
                group_size > 2u;
            if (!can_reduce_fan_in) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                if (admission_failure.has_failure()) {
                    emit_terminal_merge_admission_failure(admission_site, admission_failure);
                }
#endif
                report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
            }
            group_size = std::max<std::size_t>(
                2, bioseq::common::ceil_div(group_size, std::size_t{2}));
        }
        std::vector<buffer_file_run>& next = admitted_next.values;
        try {
            for (std::size_t lo = 0; lo < input_run_count;) {
                const std::size_t remaining = input_run_count - lo;
                const std::size_t hi = remaining <= group_size
                    ? input_run_count
                    : lo + group_size;
                std::unique_ptr<admitted_file_merge> prepared;
                if (lo == 0u) {
                    prepared = std::move(first_group);
                } else {
                    prepared = try_prepare_compaction_merge_(
                        std::span<buffer_file_run>(file_runs_.data() + lo, hi - lo),
                        admission_failure,
                        admission_site);
                }
                if (!prepared) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                    if (admission_failure.has_failure()) {
                        emit_terminal_merge_admission_failure(admission_site, admission_failure);
                    }
#endif
                    report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
                }
                next.push_back(merge_file_run_group_to_new_buffile_(*prepared));
                for (std::size_t i = lo; i < hi; ++i) {
                    close_buffile_checked_as_cpp(file_runs_[i].file);
                }
                lo = hi;
            }
            bioseq::parallelism::release_admitted_vector_capacity_before_permit(
                file_runs_, file_runs_metadata_permit_);
            // Transfer the new run list while its memory reservation is still held.
            // Swapping with the freed vector leaves next empty before moving its permit.
            file_runs_.swap(next);
            file_runs_metadata_permit_ = std::move(admitted_next.permit);
            ++stats_.merge_pass_count;
        } catch (const bioseq::postgres::support::postgres_error&) {
            // Earlier groups may already have been replaced and closed. Mark the
            // store unusable so callers cannot  retry with an incomplete input set.
            poisoned_ = true;
            for (buffer_file_run& run : next) {
                // Leave these files to ResourceOwner cleanup after the PostgreSQL error.
                run.file = nullptr;
            }
            throw;
        } catch (...) {
            poisoned_ = true;
            for (buffer_file_run& run : next) {
                abandon_buffile_noexcept(run.file);
            }
            throw;
        }
    }
}

/**
 * @brief Reduce the file count so minimum-sized reader buffers fit the merge budgeh.
 */
void sorted_pair_run_store::compact_file_runs_until_bounded_() {
    if (file_runs_.empty()) {
        return;
    }
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::size_t max_output_runs =
        std::max<std::size_t>(1, reader_budget / minimum_merge_reader_buffer_bytes);
    compact_file_runs_to_output_count_(max_output_runs);
}

/**
 * @brief Merge runs in hash order and remove duplicate positions for each hash.
 * @param[in] sink Callback receiving a hash and its temporary position cursor.
 * @param[in,out] resources File readers and position buffers used during the merge
 */
void sorted_pair_run_store::merge_runs_erased_impl_(sorted_pair_kmer_sink sink,
                                                    merge_resources& resources) {
    compact_file_runs_until_bounded_();
    auto admitted_heap = open_file_readers_(resources);
    merge_head_queue& heap = admitted_heap.queue;
    for (std::size_t run_index = 0; run_index < memory_runs_.size(); ++run_index) {
        const auto& pairs = memory_runs_[run_index].pairs;
        if (!pairs.empty()) {
            heap.push(merge_head{
                .kmer = pairs.front().first,
                .encoded = pairs.front().second,
                .run_index = run_index,
                .pair_index = 0,
                .file = false,
            });
        }
    }
    for (std::size_t run_index = 0; run_index < resources.readers.size(); ++run_index) {
        file_reader& reader = resources.readers[run_index];
        if (reader.read_next()) {
            heap.push(merge_head{
                .kmer = reader.current.kmer,
                .encoded = reader.current.encoded,
                .run_index = run_index,
                .pair_index = 0,
                .file = true,
            });
        }
    }

    while (!heap.empty()) {
        const std::uint64_t kmer = heap.top().kmer;
        resources.positions.reset_for_kmer();
        std::uint64_t last_encoded = 0;
        bool have_last = false;
        while (!heap.empty() && heap.top().kmer == kmer) {
            const merge_head head = heap.top();
            heap.pop();
            if (!have_last || head.encoded != last_encoded) {
                resources.positions.push_unique_encoded(head.encoded);
                last_encoded = head.encoded;
                have_last = true;
            }
            if (head.file) {
                file_reader& reader = resources.readers[head.run_index];
                if (reader.read_next()) {
                    heap.push(merge_head{
                        .kmer = reader.current.kmer,
                        .encoded = reader.current.encoded,
                        .run_index = head.run_index,
                        .file = true,
                    });
                }
            } else {
                const std::size_t next_pair = head.pair_index + 1;
                const auto& pairs = memory_runs_[head.run_index].pairs;
                if (next_pair < pairs.size()) {
                    heap.push(merge_head{
                        .kmer = pairs[next_pair].first,
                        .encoded = pairs[next_pair].second,
                        .run_index = head.run_index,
                        .pair_index = next_pair,
                        .file = false,
                    });
                }
            }
        }
        resources.positions.emit(kmer, sink);
        if (resources.positions.spill != nullptr) {
            ++stats_.position_spill_count;
            stats_.position_spill_bytes += resources.positions.spilled_bytes;
        }
    }
}

/**
 * @brief Free the reusable write buffer after a completed spill.
 *
 * Keep the stored runs. A later spill can allocate another buffer. Reject  a
 * store left unfinished by an error or already marked as finalized.
 */
void sorted_pair_run_store::release_completed_write_buffer() {
    if (poisoned_) {
        report_sorted_run_error(sorted_run_error::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(sorted_run_error::finalized_store_reused);
    }
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        write_buffer_, write_buffer_permit_);
}

/**
 * @brief Move the reusable write buffer and its memory reservation to another store.
 *
 * Check both stores before changing them. An empty source succeeds, and the
 * destination can allocate a buffer on its next spill. Moving a store to itself
 * also succeeds.
 *
 * @param[in,out] destination Active store with no write buffer.
 * @return False if a store is unusable or its buffer ownership is invalid.
 */
bool sorted_pair_run_store::try_transfer_completed_write_buffer_to(
    sorted_pair_run_store& destination) noexcept {
    const auto writer_capacity_bytes = [](const sorted_pair_run_store& store) noexcept {
        return bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(store.write_buffer_.capacity()),
            static_cast<std::uint64_t>(sizeof(sorted_pair_wire)));
    };
    const auto writer_ownership_is_valid = [&writer_capacity_bytes](
                                               const sorted_pair_run_store& store) noexcept {
        const std::uint64_t capacity_bytes = writer_capacity_bytes(store);
        if (capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return false;
        }
        const bool has_capacity = store.write_buffer_.capacity() != 0u;
        const bool has_permit = static_cast<bool>(store.write_buffer_permit_);
        return store.write_buffer_.size() <= store.write_buffer_.capacity() &&
               has_capacity == has_permit &&
               (!has_permit || store.write_buffer_permit_.bytes() == capacity_bytes);
    };

    if (poisoned_ || finalized_ || destination.poisoned_ || destination.finalized_ ||
        !writer_ownership_is_valid(*this) || !writer_ownership_is_valid(destination)) {
        return false;
    }
    if (this == std::addressof(destination)) {
        return true;
    }
    if (destination.write_buffer_.capacity() != 0u || destination.write_buffer_permit_) {
        return false;
    }
    if (!write_buffer_permit_) {
        return true;
    }

    write_buffer_.clear();
    destination.write_buffer_.swap(write_buffer_);
    destination.write_buffer_permit_ = std::move(write_buffer_permit_);
    return true;
}

/**
 * @brief Merge runs and call the sink once per hash with its  unique positions.
 *
 * Successful runs remain available for another merge. A PostgreSQL error or
 * callback exception marks the, store unusable until its owne abandons it.
 *
 * @param[in] sink Callback whose position cursor is valid only during the call.
 */
void sorted_pair_run_store::merge_runs_erased(sorted_pair_kmer_sink sink) {
    // Free the spill buffer before reserving merge readers. Builds can own
    // one store per segment, and keeping every idle writer would consume the
    // memory needed to read their runs. A later spill can allocate it again.
    release_completed_write_buffer();
    poisoned_ = true;
    auto resources = std::make_unique<merge_resources>();
    try {
        merge_runs_erased_impl_(sink, *resources);
        resources->finish_success();
        // Later build stages read  these runs again Keep their files open until
        // the owner calls release_runs.
        poisoned_ = false;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources->abandon_pg_resources_noexcept();
        throw;
    } catch (...) {
        // A callback can throw a C++ exception. Leave file cleanup to
        // ResourceOwner so a close failure cannot replace that exception.
        resources->abandon_pg_resources_noexcept();
        throw;
    }
}

/// Close every temporary run file after successful use.
void sorted_pair_run_store::finalize_file_handles_success_() {
    for (buffer_file_run& run : file_runs_) {
        close_buffile_checked_as_cpp(run.file);
    }
}

/**
 * @brief Discard runs held in memory and release their reservations without PostgreSQL calls.
 */
void sorted_pair_run_store::release_resident_runs_noexcept() noexcept {
    for (memory_run& run : memory_runs_) {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(run.pairs, run.permit);
    }
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        memory_runs_, memory_runs_metadata_permit_);
    memory_run_bytes_ = 0;
    memory_run_peak_bytes_ = 0;
}

/**
 * @brief Release run memory and forget temporary file handles during error cleanup.
 *
 * PostgreSQL ResourceOwner cleanup will close the temporary files. This function
 * makes no PostgreSQL calls.
 */
void sorted_pair_run_store::release_runs_noexcept() noexcept {
    release_resident_runs_noexcept();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(write_buffer_,
                                                                        write_buffer_permit_);
    for (buffer_file_run& run : file_runs_) {
        // Destruction  and error cleanup leave temporary files to ResourceOwner.
        abandon_buffile_noexcept(run.file);
    }
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(file_runs_,
                                                                        file_runs_metadata_permit_);
    spilled_run_bytes_ = 0;
    stats_ = {};
    finalized_ = false;
}

/**
 * @brief Close temporary files and free run memory after successful use.
 *
 * Reject an unusable stzre. Reset the counters and allow reuse only after all
 * checked cleanup operations have succeeded.
 */
void sorted_pair_run_store::release_runs() {
    if (poisoned_) {
        report_sorted_run_error(sorted_run_error::poisoned_store_reused);
    }
    poisoned_ = true;
    finalize_file_handles_success_();
    release_resident_runs_noexcept();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(write_buffer_,
                                                                        write_buffer_permit_);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(file_runs_,
                                                                        file_runs_metadata_permit_);
    spilled_run_bytes_ = 0;
    stats_ = {};
    finalized_ = false;
    poisoned_ = false;
}

} // namespace bioseq::storage::indexing
