/**
 * @file merge.cppm
 * @brief Sorted-run compaction, merge, and terminal cleanup.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/buffile.h"

// The PostgreSQL ERROR bridge is owned by the private named implementation
// unit so this interface remains usable by both supported compilers.
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
 * @brief Merges sorted file runs into an arbitrary pair writer.
 *
 * Reader buffers and the merge heap are memory-admitted before use. Every
 * input `BufFile` is rewound and remains owned by its run after the merge.
 *
 * @tparam p_writer Callable accepting one `sorted_pair_wire` value.
 * @param[in,out] runs File runs to read in canonical pair order.
 * @param[in,out] resources Checked readers and admission state for this merge.
 * @param[in] writer Destination invoked once for each merged pair.
 */
template <class p_writer>
void sorted_pair_run_store::merge_sorted_file_runs_to_writer_(std::span<buffer_file_run> runs,
                                                              merge_resources& resources,
                                                              p_writer&& writer) {
    resources.finish_success();
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::uint64_t per_reader_bytes =
        std::clamp<std::uint64_t>(reader_budget / std::max<std::size_t>(1u, runs.size()),
                                  std::uint64_t{32} << 10,
                                  std::uint64_t{4} << 20);
    const std::uint64_t logical_reader_bytes = bioseq::common::saturating_add(
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(runs.size()), per_reader_bytes),
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(runs.size()),
                                       static_cast<std::uint64_t>(sizeof(file_reader))));
    const std::uint64_t reader_headroom = bioseq::common::saturating_mul(
        logical_reader_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (logical_reader_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        reader_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    auto reader_permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(reader_headroom);
    if (!reader_permit) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    resources.reader_budget_permit = std::move(*reader_permit);
    resources.readers.reserve(runs.size());
    for (buffer_file_run& run : runs) {
        if (seek_buffile_as_cpp(run.file, 0, 0, SEEK_SET) != 0) {
            report_sorted_run_error(sorted_run_error::compacted_run_rewind_failed);
        }
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
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    resources.reader_budget_permit.shrink_to(exact_reader_bytes);
    stats_.merge_reader_buffer_bytes += exact_reader_bytes;

    auto admitted_heap = make_admitted_merge_head_queue(runs.size());
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
 * @brief Compacts a half-open group of file runs into one PostgreSQL temporary file.
 * @param[in] lo First input run index.
 * @param[in] hi One-past-last input run index.
 * @return New rewindable file run containing the merged pair stream.
 */
buffer_file_run sorted_pair_run_store::merge_file_run_group_to_new_buffile_(std::size_t lo,
                                                                            std::size_t hi) {
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::postgres::support::invoke_pg_as_cpp([] {
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::compact_create_before);
    });
#endif
    BufFile* out_file = create_temp_buffile_as_cpp();
    if (out_file == nullptr) {
        report_sorted_run_error(sorted_run_error::compact_temp_file_create_failed);
    }
    auto resources = std::make_unique<merge_resources>();
    buffer_file_run out{.file = out_file};
    try {
        merge_sorted_file_runs_to_writer_(
            std::span<buffer_file_run>(file_runs_.data() + lo, hi - lo),
            *resources,
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
        resources->finish_success();
        out_file = nullptr;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources->abandon_pg_resources_noexcept();
        // out_file remains registered with the current ResourceOwner.
        out_file = nullptr;
        throw;
    } catch (...) {
        // The caller will turn the C++ failure into a PostgreSQL ERROR.  Do
        // not call BufFileClose while preserving that original exception.
        resources->abandon_pg_resources_noexcept();
        abandon_buffile_noexcept(out_file);
        throw;
    }
    return out;
}

/**
 * @brief Repeatedly compacts file runs to a bounded output count.
 * @param[in] target_output_runs Maximum number of file runs to retain.
 */
void sorted_pair_run_store::compact_file_runs_to_output_count_(std::size_t target_output_runs) {
    target_output_runs = std::max<std::size_t>(1, target_output_runs);
    while (file_runs_.size() > target_output_runs) {
        const std::size_t group_size = std::max<std::size_t>(
            2, (file_runs_.size() + target_output_runs - 1) / target_output_runs);
        admitted_owned_vector<buffer_file_run> admitted_next;
        if (!try_ensure_admitted_vector_capacity(admitted_next, target_output_runs)) {
            report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
        }
        std::vector<buffer_file_run>& next = admitted_next.values;
        try {
            for (std::size_t lo = 0; lo < file_runs_.size(); lo += group_size) {
                const std::size_t hi = std::min(file_runs_.size(), lo + group_size);
                next.push_back(merge_file_run_group_to_new_buffile_(lo, hi));
                for (std::size_t i = lo; i < hi; ++i) {
                    close_buffile_checked_as_cpp(file_runs_[i].file);
                }
            }
            bioseq::parallelism::release_admitted_vector_capacity_before_permit(
                file_runs_, file_runs_metadata_permit_);
            // `file_runs_` is now capacity-free.  Swap transfers the next
            // allocation while its permit remains live and proves `next` is
            // empty before that permit moves into the store.
            file_runs_.swap(next);
            file_runs_metadata_permit_ = std::move(admitted_next.permit);
            ++stats_.merge_pass_count;
        } catch (const bioseq::postgres::support::postgres_error&) {
            // Some earlier input groups may already be closed.  The original
            // run set is no longer retryable even if this helper is ever
            // reached from a new caller that did not pre-poison the store.
            poisoned_ = true;
            for (buffer_file_run& run : next) {
                // ResourceOwner cleanup owns files touched by the failed PG
                // operation; do not issue more PostgreSQL calls here.
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
 * @brief Compacts file runs until minimum reader buffers fit the merge budget.
 */
void sorted_pair_run_store::compact_file_runs_until_bounded_() {
    if (file_runs_.empty()) {
        return;
    }
    constexpr std::uint64_t k_minimum_reader_bytes = std::uint64_t{32} << 10;
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::size_t max_output_runs =
        std::max<std::size_t>(1, reader_budget / k_minimum_reader_bytes);
    compact_file_runs_to_output_count_(max_output_runs);
}

/**
 * @brief Merges memory and file runs into unique positions grouped by hash.
 * @param[in] sink Type-erased callback receiving each hash and its position cursor.
 * @param[in,out] resources Readers, heap state, and position accumulation owned by the merge.
 */
void sorted_pair_run_store::merge_runs_erased_impl_(sorted_pair_kmer_sink sink,
                                                    merge_resources& resources) {
    compact_file_runs_until_bounded_();
    open_file_readers_(resources);

    auto admitted_heap =
        make_admitted_merge_head_queue(memory_runs_.size() + resources.readers.size());
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
 * @brief Executes a rewindable merge through a type-erased hash callback.
 *
 * A successful merge leaves immutable file handles open for later replay.
 * PostgreSQL or callback failure poisons the store until its owner abandons it.
 *
 * @param[in] sink Callback whose position cursor is valid only during the call.
 */
void sorted_pair_run_store::merge_runs_erased(sorted_pair_kmer_sink sink) {
    if (poisoned_) {
        report_sorted_run_error(sorted_run_error::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(sorted_run_error::finalized_store_reused);
    }
    poisoned_ = true;
    auto resources = std::make_unique<merge_resources>();
    try {
        merge_runs_erased_impl_(sink, *resources);
        resources->finish_success();
        // Minimizer-sequence qualification and generation encoding replay the
        // same immutable runs. Keep successful handles rewindable until the
        // owning storage calls the checked `release_runs()` boundary.
        poisoned_ = false;
    } catch (const bioseq::postgres::support::postgres_error&) {
        resources->abandon_pg_resources_noexcept();
        throw;
    } catch (...) {
        // The callback may throw an ordinary C++ exception.  Abandon all
        // ResourceOwner-backed handles; closing here could mask that error.
        resources->abandon_pg_resources_noexcept();
        throw;
    }
}

/// @brief Closes every temporary file after successful terminal use.
void sorted_pair_run_store::finalize_file_handles_success_() {
    for (buffer_file_run& run : file_runs_) {
        close_buffile_checked_as_cpp(run.file);
    }
}

/**
 * @brief Releases memory-run capacity and admission permits without PostgreSQL calls.
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
 * @brief Abandons temporary files and releases all admitted memory without PostgreSQL calls.
 *
 * The active PostgreSQL ResourceOwner remains responsible for temporary-file cleanup.
 */
void sorted_pair_run_store::release_runs_noexcept() noexcept {
    release_resident_runs_noexcept();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(write_buffer_,
                                                                        write_buffer_permit_);
    for (buffer_file_run& run : file_runs_) {
        // Destructors, move assignment and explicit error cleanup never enter
        // PostgreSQL.  Temp files remain registered with ResourceOwner.
        abandon_buffile_noexcept(run.file);
    }
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(file_runs_,
                                                                        file_runs_metadata_permit_);
    spilled_run_bytes_ = 0;
    stats_ = {};
    finalized_ = false;
}

/**
 * @brief Closes temporary files and releases all admitted run memory.
 *
 * The operation rejects a poisoned store and performs checked PostgreSQL
 * cleanup before resetting counters and reusable state.
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
