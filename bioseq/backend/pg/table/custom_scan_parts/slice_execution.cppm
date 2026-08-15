/**
 * @file
 * @brief Executes SliceProject materialization, DSM accounting, and cleanup.
 */
module;
// Anchor Highway in this global module fragment so target-specific definitions
// imported through multiple module graphs share one translation-unit context.
#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_slice_execution;

import std;
import bioseq.postgres.table.io;
import :access_method;
import :access_common;
import :utils;
import bioseq.postgres.column.table_access;
import bioseq.postgres.configuration.aligner;
import bioseq.postgres.configuration.core;
import bioseq.postgres.vectorization.types;
import bioseq.postgres.vectorization.execution;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.highway.dispatch_contract;
import bioseq.postgres.external.path_builder_slot;
import bioseq.postgres.storage.metadata;
import bioseq.postgres.table.types;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.table.sql_row_codecs;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.scheduler.dispatch_buffer.api;
import bioseq.postgres.scheduler.supervisor;
import bioseq.postgres.scheduler.supervisor.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.pipeline.stats;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.pipe;
import bioseq.sequences.cigar;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.storage.manager;
import bioseq.storage.layout;
import bioseq.storage.size;
import bioseq.storage.coordinates;
import bioseq.storage.lazy_sequence;
import bioseq.postgres.storage.scanner;
import bioseq.indexing.type;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.admin;
import bioseq.instrumentation.summary_writer;
import bioseq.common.seqan_adapter;
import bioseq.postgres.table.batch_sort_scan;

import :custom_scan_support;

import :custom_scan_slice_path;

using namespace bioseq::postgres::support;

export {

    struct slice_project_dsm_layout {
        Size header_bytes;
        Size slot_stride;
        Size total_bytes;
    };

    struct slice_project_dsm_merge_audit {
        std::uint32_t slot_headers_read = 0u;
        std::uint32_t counter_records_read = 0u;
        std::uint32_t merged_slots = 0u;
        std::uint32_t incomplete_slots = 0u;
        bool header_valid = false;
        bool mapping_valid = false;
        bool merged = false;
    };

    [[nodiscard]] slice_project_dsm_layout make_slice_project_dsm_layout(std::uint32_t recipe_count,
                                                                         std::uint32_t slot_count) {
        const Size header_bytes = MAXALIGN(sizeof(slice_dsm_header));
        const Size counter_bytes =
            mul_size(static_cast<Size>(recipe_count), sizeof(slice_dsm_counter));
        const Size slot_stride = MAXALIGN(add_size(sizeof(slice_dsm_worker_header), counter_bytes));
        return {
            .header_bytes = header_bytes,
            .slot_stride = slot_stride,
            .total_bytes =
                add_size(header_bytes, mul_size(static_cast<Size>(slot_count), slot_stride)),
        };
    }

    [[nodiscard]] bool
    slice_project_checked_add_noexcept(Size left, Size right, Size* result) noexcept {
        if (result == nullptr || left > MaxAllocSize || right > MaxAllocSize - left)
            return false;
        *result = left + right;
        return true;
    }

    [[nodiscard]] bool
    slice_project_checked_mul_noexcept(Size left, Size right, Size* result) noexcept {
        if (result == nullptr || (left != 0u && right > MaxAllocSize / left)) {
            return false;
        }
        *result = left * right;
        return true;
    }

    [[nodiscard]] bool
    slice_project_dsm_layout_noexcept(std::uint32_t recipe_count,
                                      std::uint32_t slot_count,
                                      slice_project_dsm_layout* result) noexcept {
        if (result == nullptr)
            return false;
        Size counter_bytes = 0u;
        Size raw_stride = 0u;
        Size slots_bytes = 0u;
        const Size header_bytes = MAXALIGN(sizeof(slice_dsm_header));
        if (!slice_project_checked_mul_noexcept(
                static_cast<Size>(recipe_count), sizeof(slice_dsm_counter), &counter_bytes) ||
            !slice_project_checked_add_noexcept(
                sizeof(slice_dsm_worker_header), counter_bytes, &raw_stride)) {
            return false;
        }
        const Size slot_stride = MAXALIGN(raw_stride);
        if (slot_stride < raw_stride ||
            !slice_project_checked_mul_noexcept(
                static_cast<Size>(slot_count), slot_stride, &slots_bytes) ||
            !slice_project_checked_add_noexcept(header_bytes, slots_bytes, &result->total_bytes))
            return false;
        result->header_bytes = header_bytes;
        result->slot_stride = slot_stride;
        return true;
    }

    [[nodiscard]] slice_dsm_worker_header*
    slice_project_worker_slot(slice_dsm_header* header, std::uint32_t slot_index) noexcept {
        return reinterpret_cast<slice_dsm_worker_header*>(
            reinterpret_cast<char*>(header) + header->header_bytes +
            static_cast<Size>(slot_index) * header->slot_stride);
    }

    [[nodiscard]] slice_dsm_counter*
    slice_project_worker_counters(slice_dsm_worker_header* worker) noexcept {
        return reinterpret_cast<slice_dsm_counter*>(reinterpret_cast<char*>(worker) +
                                                    sizeof(slice_dsm_worker_header));
    }

    void slice_project_mark_trace_invalid_noexcept(slice_trace_attachment* attachment) noexcept {
        if (!slice_trace_attachment_live(attachment))
            return;
        attachment->runtime->invalid = true;
        attachment->runtime->overflowed = true;
    }

    [[nodiscard]] bool
    slice_project_validate_dsm_header_noexcept(slice_dsm_header const* header,
                                               std::uint32_t recipe_count,
                                               std::uint32_t slot_count,
                                               std::uint64_t cycle,
                                               slice_project_dsm_layout* layout) noexcept {
        if (header == nullptr ||
            !slice_project_dsm_layout_noexcept(recipe_count, slot_count, layout))
            return false;
        return header->magic == k_bioseq_slice_dsm_magic &&
               header->codec_version == k_bioseq_slice_dsm_version &&
               header->header_bytes == layout->header_bytes &&
               header->recipe_count == recipe_count && header->slot_count == slot_count &&
               header->cycle == cycle && header->slot_stride == layout->slot_stride;
    }

    void slice_project_merge_dsm_cycle_noexcept(
        slice_dsm_header* header,
        std::uintptr_t map_base,
        Size map_length,
        std::uint32_t slot_count,
        slice_trace_attachment* attachment,
        std::uint64_t expected_cycle,
        slice_project_dsm_merge_audit* audit = nullptr) noexcept {
        if (audit != nullptr)
            *audit = {};
        if (!slice_trace_attachment_live(attachment) || header == nullptr)
            return;
        auto& trace = *attachment->runtime;
        if (trace.last_merged_cycle >= expected_cycle)
            return;
        const auto coordinate = reinterpret_cast<std::uintptr_t>(header);
        if (coordinate < map_base || coordinate - map_base > map_length ||
            sizeof(slice_dsm_header) > map_length - (coordinate - map_base)) {
            slice_project_mark_trace_invalid_noexcept(attachment);
            return;
        }
        slice_project_dsm_layout layout{};
        if (!slice_project_validate_dsm_header_noexcept(
                header, trace.boundary_count, slot_count, expected_cycle, &layout)) {
            slice_project_mark_trace_invalid_noexcept(attachment);
            return;
        }
        if (audit != nullptr)
            audit->header_valid = true;
        if (layout.total_bytes > map_length - (coordinate - map_base)) {
            slice_project_mark_trace_invalid_noexcept(attachment);
            return;
        }
        if (audit != nullptr)
            audit->mapping_valid = true;
        /* First pass validates every published-complete slot before a single
         * counter is merged.  Running and incomplete slots own tentative deltas:
         * never dereference those payloads, because a valid-looking prefix is not a
         * completed materialization. */
        for (std::uint32_t slot_index = 0u; slot_index < slot_count; ++slot_index) {
            auto* worker = slice_project_worker_slot(header, slot_index);
            if (audit != nullptr)
                ++audit->slot_headers_read;
            const auto state = worker->publish_state;
            if (state > slice_dsm_publish_state::published_incomplete) {
                slice_project_mark_trace_invalid_noexcept(attachment);
                return;
            }
            if (state == slice_dsm_publish_state::never_launched) {
                if (worker->cycle != 0u) {
                    slice_project_mark_trace_invalid_noexcept(attachment);
                    return;
                }
                continue;
            }
            pg_read_barrier();
            if (worker->cycle != expected_cycle || worker->overflowed > 1u ||
                std::any_of(worker->reserved.begin(), worker->reserved.end(), [](std::byte byte) {
                    return byte != std::byte{0};
                })) {
                slice_project_mark_trace_invalid_noexcept(attachment);
                return;
            }
            /* Running slots still belong to one exact generation.  Their tentative
             * counter payload remains unread, but accepting a stale slot header would
             * count an earlier cycle as incomplete work in this one. */
            if (state == slice_dsm_publish_state::attached_running)
                continue;
            if (state == slice_dsm_publish_state::published_incomplete)
                continue;
            auto* counters = slice_project_worker_counters(worker);
            for (std::uint32_t counter_index = 0u; counter_index < trace.boundary_count;
                 ++counter_index) {
                if (audit != nullptr)
                    ++audit->counter_records_read;
                if (counters[counter_index].overflowed > 1u ||
                    !slice_dsm_counter_reserved_valid(counters[counter_index])) {
                    slice_project_mark_trace_invalid_noexcept(attachment);
                    return;
                }
            }
        }
        std::uint32_t incomplete = 0u;
        for (std::uint32_t slot_index = 0u; slot_index < slot_count; ++slot_index) {
            auto* worker = slice_project_worker_slot(header, slot_index);
            const auto state = worker->publish_state;
            if (state == slice_dsm_publish_state::attached_running ||
                state == slice_dsm_publish_state::published_incomplete) {
                if (incomplete == UINT32_MAX) {
                    trace.overflowed = true;
                } else {
                    ++incomplete;
                }
            }
            if (state != slice_dsm_publish_state::published_complete)
                continue;
            if (audit != nullptr)
                ++audit->merged_slots;
            auto* counters = slice_project_worker_counters(worker);
            for (std::uint32_t counter_index = 0u; counter_index < trace.boundary_count;
                 ++counter_index) {
                merge_slice_dsm_counter_noexcept(trace.cumulative_sink.entries[counter_index],
                                                 counters[counter_index]);
                trace.overflowed = trace.overflowed || counters[counter_index].overflowed != 0u ||
                                   trace.cumulative_sink.entries[counter_index].overflowed != 0u;
            }
            trace.overflowed = trace.overflowed || worker->overflowed != 0u;
        }
        if (incomplete > UINT32_MAX - trace.incomplete_workers) {
            trace.incomplete_workers = UINT32_MAX;
            trace.overflowed = true;
        } else {
            trace.incomplete_workers += incomplete;
        }
        trace.last_merged_cycle = expected_cycle;
        if (audit != nullptr) {
            audit->incomplete_slots = incomplete;
            audit->merged = true;
        }
    }

    void slice_project_merge_dsm_cycle_noexcept(
        slice_dsm_header* header,
        ParallelContext* pcxt,
        slice_trace_attachment* attachment,
        std::uint64_t expected_cycle,
        slice_project_dsm_merge_audit* audit = nullptr) noexcept {
        if (!slice_trace_attachment_live(attachment) || header == nullptr || pcxt == nullptr ||
            pcxt->seg == nullptr || pcxt->nworkers < 0)
            return;
        slice_project_merge_dsm_cycle_noexcept(
            header,
            reinterpret_cast<std::uintptr_t>(dsm_segment_address(pcxt->seg)),
            dsm_segment_map_length(pcxt->seg),
            static_cast<std::uint32_t>(pcxt->nworkers),
            attachment,
            expected_cycle,
            audit);
    }

#ifdef BIOSEQ_TEST_INDEXES
    enum class slice_project_dsm_corruption : std::uint8_t {
        version = 0u,
        recipe_count,
        header_offset,
        truncated_mapping,
        lifecycle,
        stale_cycle,
    };

    struct slice_project_dsm_test_case_result {
        bool validated = false;
        slice_project_dsm_merge_audit audit{};
    };

    void slice_project_initialize_test_dsm(slice_dsm_header* header,
                                           slice_project_dsm_layout const& layout,
                                           std::uint32_t recipe_count,
                                           std::uint32_t slot_count,
                                           std::uint64_t cycle) noexcept {
        header->magic = k_bioseq_slice_dsm_magic;
        header->codec_version = k_bioseq_slice_dsm_version;
        header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
        header->recipe_count = recipe_count;
        header->slot_count = slot_count;
        header->cycle = cycle;
        header->slot_stride = layout.slot_stride;
    }

    [[nodiscard]] bool
    slice_project_test_dsm_payload_is_reset(slice_dsm_header* header,
                                            slice_project_dsm_layout const& layout,
                                            std::uint32_t recipe_count,
                                            std::uint32_t slot_count,
                                            std::uint64_t cycle) noexcept {
        if (header == nullptr || header->magic != k_bioseq_slice_dsm_magic ||
            header->codec_version != k_bioseq_slice_dsm_version ||
            header->header_bytes != layout.header_bytes || header->recipe_count != recipe_count ||
            header->slot_count != slot_count || header->cycle != cycle ||
            header->slot_stride != layout.slot_stride)
            return false;
        for (std::uint32_t slot_index = 0u; slot_index < slot_count; ++slot_index) {
            auto* worker = slice_project_worker_slot(header, slot_index);
            if (worker->cycle != 0u ||
                worker->publish_state != slice_dsm_publish_state::never_launched ||
                worker->overflowed != 0u ||
                std::any_of(worker->reserved.begin(), worker->reserved.end(), [](std::byte byte) {
                    return byte != std::byte{0};
                }))
                return false;
            auto* counters = slice_project_worker_counters(worker);
            for (std::uint32_t index = 0u; index < recipe_count; ++index) {
                if (counters[index].calls != 0u || counters[index].bytes != 0u ||
                    counters[index].overflowed != 0u ||
                    !slice_dsm_counter_reserved_valid(counters[index]))
                    return false;
            }
        }
        return true;
    }

    [[nodiscard]] slice_project_dsm_test_case_result
    slice_project_run_corrupt_dsm_test(slice_project_runtime const& source,
                                       slice_project_dsm_corruption corruption) {
        slice_project_dsm_test_case_result result{};
        if (source.boundary_count == 0u || source.boundaries == nullptr)
            return result;
        constexpr std::uint32_t slot_count = 2u;
        constexpr std::uint64_t cycle = 17u;
        const slice_project_dsm_layout layout =
            make_slice_project_dsm_layout(source.boundary_count, slot_count);
        auto* bytes = static_cast<std::byte*>(palloc0(layout.total_bytes));
        auto* counters = static_cast<slice_dsm_counter*>(
            palloc0(mul_size(source.boundary_count, sizeof(slice_dsm_counter))));
        slice_materialization_runtime trace{
            .boundaries = source.boundaries,
            .boundary_count = source.boundary_count,
            .cumulative_sink = {counters, source.boundary_count},
            .last_merged_cycle = 0u,
            .incomplete_workers = 0u,
            .collection_enabled = true,
            .timing_enabled = false,
            .invalid = false,
            .overflowed = false,
        };
        slice_trace_attachment attachment{
            .refcount = 1u,
            .generation = 1u,
            .runtime = &trace,
            .async_owner = nullptr,
            .async_owner_generation = 0u,
            .valid = true,
        };
        auto* header = reinterpret_cast<slice_dsm_header*>(bytes);
        slice_project_initialize_test_dsm(header, layout, source.boundary_count, slot_count, cycle);
        auto* worker = slice_project_worker_slot(header, 0u);
        worker->cycle = cycle;
        worker->publish_state = slice_dsm_publish_state::published_complete;
        auto* worker_counters = slice_project_worker_counters(worker);
        worker_counters[0].calls = 99u;
        Size mapped_length = layout.total_bytes;
        switch (corruption) {
        case slice_project_dsm_corruption::version:
            ++header->codec_version;
            break;
        case slice_project_dsm_corruption::recipe_count:
            ++header->recipe_count;
            break;
        case slice_project_dsm_corruption::header_offset:
            header->header_bytes =
                static_cast<std::uint16_t>(header->header_bytes + MAXIMUM_ALIGNOF);
            break;
        case slice_project_dsm_corruption::truncated_mapping:
            --mapped_length;
            break;
        case slice_project_dsm_corruption::lifecycle:
            worker->publish_state = static_cast<slice_dsm_publish_state>(255u);
            break;
        case slice_project_dsm_corruption::stale_cycle:
            worker->cycle = cycle - 1u;
            break;
        }
        slice_project_merge_dsm_cycle_noexcept(header,
                                               reinterpret_cast<std::uintptr_t>(bytes),
                                               mapped_length,
                                               slot_count,
                                               &attachment,
                                               cycle,
                                               &result.audit);
        const bool zero_payload_reads = result.audit.counter_records_read == 0u;
        const bool no_guessed_workers = trace.incomplete_workers == 0u;
        const bool no_prefix_merge = trace.last_merged_cycle == 0u && counters[0].calls == 0u;
        const bool rejected_before_slot =
            corruption == slice_project_dsm_corruption::version ||
            corruption == slice_project_dsm_corruption::recipe_count ||
            corruption == slice_project_dsm_corruption::header_offset ||
            corruption == slice_project_dsm_corruption::truncated_mapping;
        result.validated = trace.invalid && trace.overflowed && zero_payload_reads &&
                           no_guessed_workers && no_prefix_merge &&
                           (!rejected_before_slot || result.audit.slot_headers_read == 0u);
        pfree(counters);
        pfree(bytes);
        return result;
    }

    struct slice_project_async_test_callbacks {
        std::uint32_t cancelled = 0u;
        std::uint32_t joined = 0u;
        std::uint32_t backings_released = 0u;
        std::uint32_t complete_published = 0u;
        std::uint32_t incomplete_published = 0u;
    };

    void slice_project_test_cancel(void* argument) noexcept {
        ++static_cast<slice_project_async_test_callbacks*>(argument)->cancelled;
    }

    void slice_project_test_join(void* argument) noexcept {
        ++static_cast<slice_project_async_test_callbacks*>(argument)->joined;
    }

    void slice_project_test_release_backing(void* argument) noexcept {
        ++static_cast<slice_project_async_test_callbacks*>(argument)->backings_released;
    }

    void slice_project_test_publish_complete(void* argument) noexcept {
        ++static_cast<slice_project_async_test_callbacks*>(argument)->complete_published;
    }

    void slice_project_test_publish_incomplete(void* argument) noexcept {
        ++static_cast<slice_project_async_test_callbacks*>(argument)->incomplete_published;
    }

    [[nodiscard]] bool slice_project_run_incomplete_async_test(slice_project_runtime const& source,
                                                               std::uint32_t* cleanup_callbacks) {
        if (cleanup_callbacks != nullptr)
            *cleanup_callbacks = 0u;
        if (source.boundary_count == 0u || source.boundaries == nullptr)
            return false;
        slice_materialization_boundary boundary = source.boundaries[0];
        boundary.counter_index = 0u;
        slice_dsm_counter cumulative{};
        slice_materialization_runtime trace{};
        if (!initialize_slice_materialization_runtime_noexcept(
                &trace, &boundary, 1u, &cumulative, 1u, true, false))
            return false;
        auto* attachment = make_slice_trace_attachment(&trace);
        slice_async_owner_guard guard{};
        std::array<slice_async_job_slot, slice_async_max_live_jobs> jobs{};
        std::array<slice_dsm_counter, slice_async_max_live_jobs> job_counters{};
        if (!initialize_slice_async_owner_noexcept(&guard,
                                                   &trace,
                                                   attachment,
                                                   jobs.data(),
                                                   slice_async_max_live_jobs,
                                                   job_counters.data(),
                                                   1u)) {
            invalidate_slice_trace_attachment_noexcept(attachment);
            release_slice_trace_attachment(attachment);
            return false;
        }
        slice_project_async_test_callbacks callbacks{};
        const slice_async_job_resources resources{
            .cancel = {&callbacks, &slice_project_test_cancel},
            .join = {&callbacks, &slice_project_test_join},
            .backings = {&callbacks, &slice_project_test_release_backing},
        };
        guard.publish_complete = {&callbacks, &slice_project_test_publish_complete};
        guard.publish_incomplete = {&callbacks, &slice_project_test_publish_incomplete};
        slice_async_completion_handle handle{};
        const bool enrolled = enroll_slice_async_job_noexcept(&guard, resources, &handle);
        const bool scheduled = enrolled && mark_slice_async_job_scheduled_noexcept(&guard, handle);
        if (scheduled) {
            update_slice_dsm_counter_noexcept(
                guard.jobs[handle.slot_index].counters[0], 1u, 1u, 8u, false, 8u, 0u);
        }
        /* Exact executor teardown order: Shutdown drains an unpublished scheduled
         * job, End makes the owner terminal, and a later context reset is a no-op. */
        quiesce_slice_async_owner_noexcept(&guard, slice_shutdown_disposition::complete);
        const bool stale_rejected = !mark_slice_async_job_finished_noexcept(&guard, handle);
        const bool lifecycle_valid =
            enrolled && scheduled && stale_rejected &&
            guard.phase == slice_async_owner_phase::quiesced && guard.enrolled_jobs == 0u &&
            jobs[handle.slot_index].state == slice_async_job_state::unused &&
            jobs[handle.slot_index].generation == handle.generation + 1u &&
            cumulative.calls == 0u && cumulative.bytes == 0u && callbacks.cancelled == 1u &&
            callbacks.joined == 1u && callbacks.backings_released == 1u &&
            callbacks.complete_published == 0u && callbacks.incomplete_published == 1u &&
            attachment->refcount == 1u;
        if (cleanup_callbacks != nullptr) {
            *cleanup_callbacks =
                callbacks.cancelled + callbacks.joined + callbacks.backings_released;
        }
        finish_slice_async_owner_noexcept(&guard, slice_shutdown_disposition::complete);
        const bool end_valid =
            lifecycle_valid && guard.phase == slice_async_owner_phase::terminal &&
            trace.boundaries == nullptr && trace.cumulative_sink.entries == nullptr &&
            callbacks.complete_published == 0u && callbacks.incomplete_published == 1u;
        slice_async_owner_reset(&guard);
        return end_valid && guard.phase == slice_async_owner_phase::terminal &&
               callbacks.complete_published == 0u && callbacks.incomplete_published == 1u;
    }

    [[nodiscard]] bool slice_project_reinitialize_rejects_test_cycle(slice_project_state* state,
                                                                     ParallelContext* pcxt,
                                                                     slice_dsm_header* header) {
        MemoryContext saved_context = CurrentMemoryContext;
        bool volatile rejected = false;
        PG_TRY();
        {
            slice_project_reinitialize_dsm(&state->custom_scan_state, pcxt, header);
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(saved_context);
            ErrorData* error = CopyErrorData();
            rejected = error != nullptr && error->sqlerrcode == ERRCODE_DATA_CORRUPTED;
            FlushErrorState();
            if (error != nullptr)
                FreeErrorData(error);
        }
        PG_END_TRY();
        MemoryContextSwitchTo(saved_context);
        return rejected;
    }

    void slice_project_apply_dsm_test_fixture(slice_project_state* state) {
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        if (runtime.dsm_test_fixture_applied)
            return;
        const int fixture_mode = runtime.dsm_test_fixture_mode;
        if (fixture_mode == k_slice_dsm_test_none)
            return;
        runtime.dsm_test_fixture_applied = true;
        if (runtime.boundary_count == 0u || runtime.boundaries == nullptr) {
            runtime.dsm_test_fixture_validated = false;
            return;
        }
        runtime.dsm_test_async_validated =
            slice_project_run_incomplete_async_test(runtime, &runtime.dsm_test_cleanup_callbacks);
        if (fixture_mode == k_slice_dsm_test_malformed) {
            constexpr std::array corruptions{
                slice_project_dsm_corruption::version,
                slice_project_dsm_corruption::recipe_count,
                slice_project_dsm_corruption::header_offset,
                slice_project_dsm_corruption::truncated_mapping,
                slice_project_dsm_corruption::lifecycle,
                slice_project_dsm_corruption::stale_cycle,
            };
            bool valid = true;
            for (const auto corruption : corruptions) {
                const auto result = slice_project_run_corrupt_dsm_test(runtime, corruption);
                valid = valid && result.validated;
                ++runtime.dsm_test_checks;
                runtime.dsm_test_slot_headers_read += result.audit.slot_headers_read;
                runtime.dsm_test_counter_records_read += result.audit.counter_records_read;
            }
            runtime.dsm_test_fixture_validated = valid && runtime.dsm_test_async_validated;
            runtime.trace_runtime.invalid = true;
            runtime.trace_runtime.overflowed = true;
            return;
        }

        /* Exercise the real leader ReInitializeDSM callback twice over a real DSM
         * mapping.  The first cycle contains complete, attached-running,
         * published-incomplete, and never-launched slots.  Only the complete delta
         * may merge; both incomplete lifecycle states count exactly once. */
        constexpr std::uint32_t slot_count = 4u;
        constexpr std::uint64_t first_cycle = 23u;
        constexpr std::uint64_t second_cycle = first_cycle + 1u;
        constexpr std::uint64_t reset_cycle = second_cycle + 1u;
        const slice_project_dsm_layout layout =
            make_slice_project_dsm_layout(runtime.boundary_count, slot_count);
        auto* local_counters = static_cast<slice_dsm_counter*>(
            palloc0(mul_size(runtime.boundary_count, sizeof(slice_dsm_counter))));
        dsm_segment* segment = dsm_create(layout.total_bytes, DSM_CREATE_NULL_IF_MAXSEGMENTS);
        if (segment == nullptr) {
            runtime.dsm_test_fixture_validated = false;
            pfree(local_counters);
            return;
        }
        auto* bytes = static_cast<std::byte*>(dsm_segment_address(segment));
        std::memset(bytes, 0, layout.total_bytes);
        slice_materialization_runtime trace{
            .boundaries = runtime.boundaries,
            .boundary_count = runtime.boundary_count,
            .cumulative_sink = {local_counters, runtime.boundary_count},
            .last_merged_cycle = 0u,
            .incomplete_workers = 0u,
            .collection_enabled = true,
            .timing_enabled = false,
            .invalid = false,
            .overflowed = false,
        };
        slice_trace_attachment attachment{.refcount = 1u,
                                          .generation = 1u,
                                          .runtime = &trace,
                                          .async_owner = nullptr,
                                          .async_owner_generation = 0u,
                                          .valid = true};
        slice_project_state reinitialize_state{};
        auto& reinitialize_runtime = reinitialize_state.runtime;
        reinitialize_runtime.boundary_count = runtime.boundary_count;
        reinitialize_runtime.trace_initialization_attempted = true;
        reinitialize_runtime.collect_actual = true;
        reinitialize_runtime.attachment = &attachment;
        ParallelContext parallel_context{};
        parallel_context.seg = segment;
        parallel_context.nworkers = static_cast<int>(slot_count);
        auto* header = reinterpret_cast<slice_dsm_header*>(bytes);
        const auto reset_reinitialize_case = [bytes,
                                              layout,
                                              header,
                                              local_counters,
                                              &runtime,
                                              &trace,
                                              &attachment,
                                              &reinitialize_runtime](std::uint64_t cycle) {
            std::memset(bytes, 0, layout.total_bytes);
            slice_project_initialize_test_dsm(
                header, layout, runtime.boundary_count, slot_count, cycle);
            for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
                reset_slice_dsm_counter_noexcept(local_counters[index]);
            }
            trace.last_merged_cycle = 0u;
            trace.incomplete_workers = 0u;
            trace.collection_enabled = true;
            trace.invalid = false;
            trace.overflowed = false;
            attachment.runtime = &trace;
            attachment.valid = true;
            reinitialize_runtime.mapped_header = header;
            reinitialize_runtime.mapped_cycle = cycle;
            reinitialize_runtime.dsm_test_reinitialize_audit = nullptr;
        };

        /* The shared header may not nominate its own generation.  Exercise both
         * directions of header drift and a running slot from an earlier cycle
         * through the production callback before the two successful rescans. */
        reset_reinitialize_case(first_cycle);
        header->cycle = first_cycle - 1u;
        const bool stale_header_rejected = slice_project_reinitialize_rejects_test_cycle(
            &reinitialize_state, &parallel_context, header);
        reset_reinitialize_case(first_cycle);
        header->cycle = first_cycle + 1u;
        const bool forward_header_rejected = slice_project_reinitialize_rejects_test_cycle(
            &reinitialize_state, &parallel_context, header);
        reset_reinitialize_case(first_cycle);
        auto* wrong_generation_worker = slice_project_worker_slot(header, 0u);
        wrong_generation_worker->cycle = first_cycle - 1u;
        wrong_generation_worker->publish_state = slice_dsm_publish_state::attached_running;
        slice_project_dsm_merge_audit wrong_generation_audit{};
        reinitialize_runtime.dsm_test_reinitialize_audit = &wrong_generation_audit;
        const bool attached_generation_rejected = slice_project_reinitialize_rejects_test_cycle(
            &reinitialize_state, &parallel_context, header);
        const bool rejection_valid =
            stale_header_rejected && forward_header_rejected && attached_generation_rejected &&
            wrong_generation_audit.header_valid && wrong_generation_audit.mapping_valid &&
            !wrong_generation_audit.merged && wrong_generation_audit.slot_headers_read == 1u &&
            wrong_generation_audit.counter_records_read == 0u && local_counters[0].calls == 0u;

        reset_reinitialize_case(first_cycle);
        auto* first_complete = slice_project_worker_slot(header, 0u);
        first_complete->cycle = first_cycle;
        first_complete->publish_state = slice_dsm_publish_state::published_complete;
        auto* first_complete_counters = slice_project_worker_counters(first_complete);
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            first_complete_counters[index].calls = 3u;
            first_complete_counters[index].bytes = 30u;
        }
        auto* old_cycle_attached = slice_project_worker_slot(header, 1u);
        old_cycle_attached->cycle = first_cycle;
        old_cycle_attached->publish_state = slice_dsm_publish_state::attached_running;
        auto* old_cycle_tentative = slice_project_worker_counters(old_cycle_attached);
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            /* Poison a valid-looking tentative prefix.  Attached payloads must never
             * be inspected or merged, even when their bytes are malformed. */
            old_cycle_tentative[index].calls = 99u;
            old_cycle_tentative[index].bytes = 990u;
            old_cycle_tentative[index].overflowed = 2u;
            old_cycle_tentative[index].reserved[0] = std::byte{1};
        }
        auto* first_incomplete = slice_project_worker_slot(header, 2u);
        first_incomplete->cycle = first_cycle;
        first_incomplete->publish_state = slice_dsm_publish_state::published_incomplete;
        first_incomplete->overflowed = 1u;
        auto* first_incomplete_counters = slice_project_worker_counters(first_incomplete);
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            /* Published-incomplete payloads are tentative too.  Poisoning their
             * records distinguishes that branch from published-complete validation. */
            first_incomplete_counters[index].calls = 101u;
            first_incomplete_counters[index].bytes = 1010u;
            first_incomplete_counters[index].overflowed = 2u;
            first_incomplete_counters[index].reserved[0] = std::byte{1};
        }

        slice_project_dsm_merge_audit first_audit{};
        reinitialize_runtime.dsm_test_reinitialize_audit = &first_audit;
        slice_project_reinitialize_dsm(
            &reinitialize_state.custom_scan_state, &parallel_context, header);
        bool valid = rejection_valid && first_audit.header_valid && first_audit.mapping_valid &&
                     first_audit.merged && first_audit.slot_headers_read == slot_count &&
                     first_audit.counter_records_read == runtime.boundary_count &&
                     first_audit.merged_slots == 1u && first_audit.incomplete_slots == 2u &&
                     trace.last_merged_cycle == first_cycle && trace.incomplete_workers == 2u &&
                     !trace.invalid && !trace.overflowed;
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            valid =
                valid && local_counters[index].calls == 3u && local_counters[index].bytes == 30u;
        }

        valid = valid && reinitialize_runtime.mapped_header == header &&
                reinitialize_runtime.mapped_cycle == second_cycle &&
                slice_project_test_dsm_payload_is_reset(
                    header, layout, runtime.boundary_count, slot_count, second_cycle);

        constexpr std::array<std::uint64_t, 2u> second_calls{5u, 7u};
        constexpr std::array<std::uint64_t, 2u> second_bytes{50u, 70u};
        for (std::uint32_t slot_index = 0u; slot_index < 2u; ++slot_index) {
            auto* worker = slice_project_worker_slot(header, slot_index);
            worker->cycle = second_cycle;
            worker->publish_state = slice_dsm_publish_state::published_complete;
            auto* counters = slice_project_worker_counters(worker);
            for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
                counters[index].calls = second_calls[slot_index];
                counters[index].bytes = second_bytes[slot_index];
            }
        }
        slice_project_dsm_merge_audit second_audit{};
        reinitialize_runtime.dsm_test_reinitialize_audit = &second_audit;
        slice_project_reinitialize_dsm(
            &reinitialize_state.custom_scan_state, &parallel_context, header);
        valid = valid && second_audit.header_valid && second_audit.mapping_valid &&
                second_audit.merged && second_audit.slot_headers_read == slot_count &&
                second_audit.counter_records_read == runtime.boundary_count * 2u &&
                second_audit.merged_slots == 2u && second_audit.incomplete_slots == 0u &&
                trace.last_merged_cycle == second_cycle && trace.incomplete_workers == 2u &&
                !trace.invalid && !trace.overflowed;
        valid = valid && reinitialize_runtime.mapped_header == header &&
                reinitialize_runtime.mapped_cycle == reset_cycle &&
                slice_project_test_dsm_payload_is_reset(
                    header, layout, runtime.boundary_count, slot_count, reset_cycle);

        /* Worker ExecReScan is a separate production transition from leader DSM
         * reinitialization.  Complete cumulative records survive rearm, whereas an
         * incomplete tentative record is cleared before its stable slot is reused. */
        auto* complete_rearm = slice_project_worker_slot(header, 0u);
        complete_rearm->cycle = reset_cycle;
        complete_rearm->publish_state = slice_dsm_publish_state::published_complete;
        auto* complete_rearm_counters = slice_project_worker_counters(complete_rearm);
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            complete_rearm_counters[index].calls = 3u;
            complete_rearm_counters[index].bytes = 30u;
        }
        reinitialize_runtime.mapped_worker = complete_rearm;
        const bool complete_rearmed =
            slice_project_rearm_worker_for_rescan_noexcept(&reinitialize_state);
        valid = valid && complete_rearmed &&
                complete_rearm->publish_state == slice_dsm_publish_state::attached_running;
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            valid = valid && complete_rearm_counters[index].calls == 3u &&
                    complete_rearm_counters[index].bytes == 30u;
        }

        auto* incomplete_rearm = slice_project_worker_slot(header, 1u);
        incomplete_rearm->cycle = reset_cycle;
        incomplete_rearm->publish_state = slice_dsm_publish_state::published_incomplete;
        incomplete_rearm->overflowed = 1u;
        auto* incomplete_rearm_counters = slice_project_worker_counters(incomplete_rearm);
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            incomplete_rearm_counters[index].calls = 4u;
            incomplete_rearm_counters[index].bytes = 40u;
        }
        reinitialize_runtime.mapped_worker = incomplete_rearm;
        const bool incomplete_rearmed =
            slice_project_rearm_worker_for_rescan_noexcept(&reinitialize_state);
        valid = valid && incomplete_rearmed &&
                incomplete_rearm->publish_state == slice_dsm_publish_state::attached_running &&
                incomplete_rearm->overflowed == 0u;
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            valid = valid && incomplete_rearm_counters[index].calls == 0u &&
                    incomplete_rearm_counters[index].bytes == 0u &&
                    incomplete_rearm_counters[index].overflowed == 0u;
        }
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            valid =
                valid && local_counters[index].calls == 15u && local_counters[index].bytes == 150u;
            if (runtime.counters != nullptr) {
                merge_slice_dsm_counter_noexcept(runtime.counters[index], local_counters[index]);
            }
        }
        if (first_audit.incomplete_slots > UINT32_MAX - runtime.trace_runtime.incomplete_workers) {
            runtime.trace_runtime.incomplete_workers = UINT32_MAX;
            runtime.trace_runtime.overflowed = true;
        } else {
            runtime.trace_runtime.incomplete_workers += first_audit.incomplete_slots;
        }
        runtime.dsm_test_checks = 5u;
        runtime.dsm_test_slot_headers_read = wrong_generation_audit.slot_headers_read +
                                             first_audit.slot_headers_read +
                                             second_audit.slot_headers_read;
        runtime.dsm_test_counter_records_read = wrong_generation_audit.counter_records_read +
                                                first_audit.counter_records_read +
                                                second_audit.counter_records_read;
        runtime.dsm_test_fixture_validated = valid && runtime.dsm_test_async_validated;
        dsm_detach(segment);
        pfree(local_counters);
    }
#endif

    void slice_project_release_detach_state(slice_dsm_detach_state* state) noexcept {
        if (state != nullptr && state->refcount != 0u && state->refcount != UINT32_MAX &&
            --state->refcount == 0u)
            pfree(state);
    }

    void slice_project_owner_reset(void* argument) noexcept {
        auto* state = static_cast<slice_project_state*>(argument);
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        if (runtime.owner_initialized) {
            finish_slice_async_owner_noexcept(&runtime.owner_guard,
                                              slice_shutdown_disposition::incomplete);
            runtime.attachment = nullptr;
        }
        /* End may be skipped by ERROR.  Drop exactly the query-owner token here;
         * the independently retained DSM callback token remains until detach. */
        if (runtime.detach_state != nullptr) {
            slice_project_release_detach_state(runtime.detach_state);
            runtime.detach_state = nullptr;
        }
        runtime.mapped_header = nullptr;
        runtime.mapped_worker = nullptr;
    }

    /* Registered before child initialization.  PostgreSQL invokes reset callbacks
     * in reverse registration order, so child scan/provider callbacks run after
     * the async-owner callback but before this upper-lease callback. */
    void slice_project_source_routes_reset(void* argument) noexcept {
        auto* state = static_cast<slice_project_state*>(argument);
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        /* Attachment can ERROR after retaining an early route but before publishing
         * source_routes_acquired.  The container count is the unwind authority; an
         * empty or already-released set is intentionally a no-op. */
        bioseq::storage::release_pg_sequence_source_routes_noexcept(runtime.source_routes);
        runtime.source_routes_acquired = false;
    }

    void slice_project_dsm_on_detach(dsm_segment*, Datum argument) noexcept {
        auto* state = reinterpret_cast<slice_dsm_detach_state*>(DatumGetPointer(argument));
        if (state == nullptr || !state->callback_armed)
            return;
        state->callback_armed = false;
        slice_project_merge_dsm_cycle_noexcept(
            state->mapped_header, state->pcxt, state->attachment, state->expected_cycle);
        auto* attachment = state->attachment;
        state->attachment = nullptr;
        state->pcxt = nullptr;
        state->mapped_header = nullptr;
        release_slice_trace_attachment(attachment);
        slice_project_release_detach_state(state);
    }

    void slice_project_publish_worker_noexcept(slice_project_state* state,
                                               slice_dsm_publish_state final_state) noexcept {
        if (state == nullptr)
            return;
        auto& runtime = state->runtime;
        auto* worker = runtime.mapped_worker;
        if (worker == nullptr || runtime.mapped_cycle == 0u)
            return;
        if (worker->publish_state != slice_dsm_publish_state::attached_running ||
            worker->cycle != runtime.mapped_cycle) {
            slice_project_mark_trace_invalid_noexcept(runtime.attachment);
            return;
        }
        auto* destination = slice_project_worker_counters(worker);
        bool overflowed = runtime.trace_runtime.invalid || runtime.trace_runtime.overflowed ||
                          worker->overflowed != 0u;
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            if (!slice_dsm_counter_reserved_valid(destination[index]) ||
                !slice_dsm_counter_reserved_valid(runtime.counters[index])) {
                overflowed = true;
            } else {
                /* The shared slot is cumulative for one parallel cycle, whereas the
                 * backend-local sink is a delta since the preceding publish/rescan. */
                merge_slice_dsm_counter_noexcept(destination[index], runtime.counters[index]);
            }
            overflowed = overflowed || destination[index].overflowed != 0u ||
                         !slice_dsm_counter_reserved_valid(destination[index]);
        }
        worker->overflowed = overflowed ? 1u : 0u;
        pg_write_barrier();
        worker->publish_state =
            overflowed ? slice_dsm_publish_state::published_incomplete : final_state;
        /* A worker publishes one cycle delta.  Only after the shared slot is fully
         * visible may its backend-local sink be cleared for a subsequent rescan;
         * otherwise ReInitializeDSM would merge cumulative prefixes twice. */
        for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
            reset_slice_dsm_counter_noexcept(runtime.counters[index]);
        }
    }

    [[nodiscard]] bool
    slice_project_rearm_worker_for_rescan_noexcept(slice_project_state* state) noexcept {
        if (state == nullptr)
            return false;
        auto& runtime = state->runtime;
        auto* worker = runtime.mapped_worker;
        if (worker == nullptr)
            return true;
        if (runtime.mapped_cycle == 0u || worker->cycle != runtime.mapped_cycle ||
            (worker->publish_state != slice_dsm_publish_state::published_complete &&
             worker->publish_state != slice_dsm_publish_state::published_incomplete)) {
            slice_project_mark_trace_invalid_noexcept(runtime.attachment);
            return false;
        }
        /* Completed slots are cumulative across rescans.  An incomplete slot is a
         * tentative delta and must be discarded before a later rescan can publish
         * the same stable worker slot as complete. */
        if (worker->publish_state == slice_dsm_publish_state::published_incomplete) {
            auto* counters = slice_project_worker_counters(worker);
            for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
                reset_slice_dsm_counter_noexcept(counters[index]);
            }
            worker->overflowed = 0u;
        }
        pg_write_barrier();
        worker->publish_state = slice_dsm_publish_state::attached_running;
        return true;
    }

    void slice_project_publish_complete_noexcept(void* argument) noexcept {
        slice_project_publish_worker_noexcept(static_cast<slice_project_state*>(argument),
                                              slice_dsm_publish_state::published_complete);
    }

    void slice_project_publish_incomplete_noexcept(void* argument) noexcept {
        slice_project_publish_worker_noexcept(static_cast<slice_project_state*>(argument),
                                              slice_dsm_publish_state::published_incomplete);
    }

    void slice_project_attach_sources_from_plan_state(PlanState* child,
                                                      slice_project_state* state,
                                                      std::uint32_t& tagged_sources) {
        if (child == nullptr || state == nullptr)
            return;
        if (IsA(child, CustomScanState)) {
            auto* child_custom = reinterpret_cast<CustomScanState*>(child);
            if (child_custom->methods == &bioseq_slice_project_exec_methods()) {
                /* Nested projectors form an async-owner boundary.  Initialize the
                 * lower projector first, then retain its exact source routes instead
                 * of descending through it and rebinding the same leaf buffers to a
                 * second owner.  The lower projector owns work below the boundary;
                 * this projector owns only provenance it publishes above it. */
                slice_project_ensure_trace_initialized(child_custom);
                auto* child_state = reinterpret_cast<slice_project_state*>(child_custom);
                if (!child_state->runtime.source_routes_acquired ||
                    child_state->runtime.source_routes.count == 0u ||
                    !bioseq::storage::merge_pg_sequence_source_routes(
                        state->runtime.source_routes, child_state->runtime.source_routes)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqSliceProject nested source route is unavailable")));
                }
                state->runtime.source_routes_acquired = true;
                ++tagged_sources;
                return;
            }
            if (child_custom->methods == bioseq_column_scan_execute_methods()) {
                auto* bioseq_state = scan_state::from_custom_scan_state(child_custom);
                bioseq_scan_runtime& child_runtime = bioseq_runtime(bioseq_state);
                if (!child_runtime.lazy_source_projection)
                    return;
                ++tagged_sources;
                /* The upper set retains child lifetimes only.  Locator stubs are
                 * promoted inside each BioseqScan through that leaf's exact provider
                 * before its tuple reaches this projection, so self-join aliases never
                 * require an ambiguous physical-relation lookup here. */
                if (child_runtime.lazy_source_provider == nullptr ||
                    !bioseq::storage::add_pg_sequence_source_route(
                        state->runtime.source_routes, child_runtime.lazy_source_provider)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqSliceProject source route is unavailable")));
                }
                state->runtime.source_routes_acquired = true;
                if (child_runtime.lazy_trace_attachment != nullptr) {
                    release_slice_trace_attachment(child_runtime.lazy_trace_attachment);
                    child_runtime.lazy_trace_attachment = nullptr;
                }
                if (slice_trace_attachment_live(state->runtime.attachment)) {
                    child_runtime.lazy_trace_attachment = state->runtime.attachment;
                    retain_slice_trace_attachment(child_runtime.lazy_trace_attachment);
                    child_runtime.buffer.bind_slice_async_owner(&state->runtime.owner_guard);
                }
                return;
            }
            for (PlanState* custom_child : postgres_list<PlanState>(child_custom->custom_ps)) {
                slice_project_attach_sources_from_plan_state(custom_child, state, tagged_sources);
            }
            return;
        }
        if (IsA(child, AppendState)) {
            auto* append = reinterpret_cast<AppendState*>(child);
            for (int index = 0; index < append->as_nplans; ++index) {
                slice_project_attach_sources_from_plan_state(
                    append->appendplans[index], state, tagged_sources);
            }
            return;
        }
        if (IsA(child, MergeAppendState)) {
            auto* append = reinterpret_cast<MergeAppendState*>(child);
            for (int index = 0; index < append->ms_nplans; ++index) {
                slice_project_attach_sources_from_plan_state(
                    append->mergeplans[index], state, tagged_sources);
            }
            return;
        }
        PlanState* outer = outerPlanState(child);
        PlanState* inner = innerPlanState(child);
        if (outer != nullptr) {
            slice_project_attach_sources_from_plan_state(outer, state, tagged_sources);
        }
        if (inner != nullptr && inner != outer) {
            slice_project_attach_sources_from_plan_state(inner, state, tagged_sources);
        }
    }

    void slice_project_attach_trace_to_child(CustomScanState* custom, slice_project_state* state) {
        if (custom == nullptr || custom->custom_ps == NIL || state == nullptr)
            return;
        std::uint32_t tagged_sources = 0u;
        for (PlanState* child : postgres_list<PlanState>(custom->custom_ps)) {
            slice_project_attach_sources_from_plan_state(child, state, tagged_sources);
        }
        if (tagged_sources == 0u) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject child did not accept source ownership")));
        }
    }

    void
    slice_project_initialize_trace(slice_project_state* state, CustomScan* scan, EState* estate) {
        auto& runtime = state->runtime;
        List* records = slice_project_plan_records(scan);
        List* output_bindings = slice_project_plan_output_bindings(scan);
        runtime.boundary_count = static_cast<std::uint32_t>(list_length(records));
        runtime.output_binding_count = static_cast<std::uint32_t>(list_length(output_bindings));
        runtime.collect_actual =
            runtime.boundary_count != 0u && state->custom_scan_state.ss.ps.instrument != nullptr;
        runtime.timing =
            runtime.collect_actual && state->custom_scan_state.ss.ps.instrument->need_timer;
        if (runtime.boundary_count != 0u) {
            runtime.boundaries =
                static_cast<slice_materialization_boundary*>(MemoryContextAllocZero(
                    estate->es_query_cxt,
                    mul_size(runtime.boundary_count, sizeof(slice_materialization_boundary))));
            runtime.counters = static_cast<slice_dsm_counter*>(MemoryContextAllocZero(
                estate->es_query_cxt, mul_size(runtime.boundary_count, sizeof(slice_dsm_counter))));
        }
        std::uint32_t index = 0u;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = reinterpret_cast<List*>(raw_record);
            runtime.boundaries[index] = {
                .boundary_id = static_cast<std::uint32_t>(
                    intVal(list_nth(record, k_slice_record_boundary_id))),
                .producer_value_id = static_cast<std::uint32_t>(
                    intVal(list_nth(record, k_slice_record_producer_id))),
                .counter_index = index,
                .mechanism = static_cast<materialization_mechanism>(
                    intVal(list_nth(record, k_slice_record_mechanism))),
                .reason = static_cast<materialization_reason>(
                    intVal(list_nth(record, k_slice_record_reason))),
                .reserved = {},
            };
            ++index;
        }
        if (!initialize_slice_materialization_runtime_noexcept(&runtime.trace_runtime,
                                                               runtime.boundaries,
                                                               runtime.boundary_count,
                                                               runtime.counters,
                                                               runtime.boundary_count,
                                                               runtime.collect_actual,
                                                               runtime.timing)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject trace runtime is invalid")));
        }
        runtime.attachment = make_slice_trace_attachment(&runtime.trace_runtime);
        runtime.async_jobs = static_cast<slice_async_job_slot*>(MemoryContextAllocZero(
            estate->es_query_cxt,
            mul_size(slice_async_max_live_jobs, sizeof(slice_async_job_slot))));
        if (runtime.boundary_count != 0u) {
            const Size async_counter_count = mul_size(static_cast<Size>(slice_async_max_live_jobs),
                                                      static_cast<Size>(runtime.boundary_count));
            runtime.async_job_counters = static_cast<slice_dsm_counter*>(MemoryContextAllocZero(
                estate->es_query_cxt, mul_size(async_counter_count, sizeof(slice_dsm_counter))));
        }
        if (!initialize_slice_async_owner_noexcept(&runtime.owner_guard,
                                                   &runtime.trace_runtime,
                                                   runtime.attachment,
                                                   runtime.async_jobs,
                                                   slice_async_max_live_jobs,
                                                   runtime.async_job_counters,
                                                   runtime.boundary_count)) {
            invalidate_slice_trace_attachment_noexcept(runtime.attachment);
            release_slice_trace_attachment(runtime.attachment);
            runtime.attachment = nullptr;
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                     errmsg("BioseqSliceProject owner initialization failed")));
        }
        runtime.owner_guard.publish_complete = {.argument = state,
                                                .invoke = slice_project_publish_complete_noexcept};
        runtime.owner_guard.publish_incomplete = {
            .argument = state, .invoke = slice_project_publish_incomplete_noexcept};
        runtime.owner_initialized = true;
        runtime.owner_guard.callback.func = slice_project_owner_reset;
        runtime.owner_guard.callback.arg = state;
        MemoryContextRegisterResetCallback(estate->es_query_cxt, &runtime.owner_guard.callback);
        if (runtime.output_binding_count != 0u) {
            runtime.output_bindings =
                static_cast<slice_project_output_binding*>(MemoryContextAllocZero(
                    estate->es_query_cxt,
                    mul_size(runtime.output_binding_count, sizeof(slice_project_output_binding))));
            index = 0u;
            for (Node* raw_binding : postgres_list<Node>(output_bindings)) {
                List* binding = reinterpret_cast<List*>(raw_binding);
                runtime.output_bindings[index++] = {
                    .resno = static_cast<AttrNumber>(intVal(linitial(binding))),
                    .producer_value_id = static_cast<std::uint32_t>(intVal(lsecond(binding))),
                };
            }
        }
    }

    void slice_project_ensure_trace_initialized(CustomScanState* custom) {
        if (custom == nullptr)
            return;
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        auto& runtime = state->runtime;
        if (runtime.trace_initialization_attempted)
            return;
        runtime.trace_initialization_attempted = true;
        EState* estate = custom->ss.ps.state;
        auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
        if (estate == nullptr || scan == nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject trace initialization state is invalid")));
        }
        slice_project_initialize_trace(state, scan, estate);
        slice_project_attach_trace_to_child(custom, state);
    }

    Size slice_project_estimate_dsm(CustomScanState* custom, ParallelContext* pcxt) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        if (!state->runtime.collect_actual)
            return 0u;
        if (pcxt == nullptr || pcxt->nworkers < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject parallel context is invalid")));
        }
        return make_slice_project_dsm_layout(state->runtime.boundary_count,
                                             static_cast<std::uint32_t>(pcxt->nworkers))
            .total_bytes;
    }

    void
    slice_project_initialize_dsm(CustomScanState* custom, ParallelContext* pcxt, void* coordinate) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        auto& runtime = state->runtime;
        if (!runtime.collect_actual)
            return;
        if (coordinate == nullptr || pcxt == nullptr || pcxt->seg == nullptr ||
            pcxt->nworkers < 0 || runtime.detach_state != nullptr) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject DSM initialization is invalid")));
        }
        const auto slot_count = static_cast<std::uint32_t>(pcxt->nworkers);
        const slice_project_dsm_layout layout =
            make_slice_project_dsm_layout(runtime.boundary_count, slot_count);
        std::memset(coordinate, 0, layout.total_bytes);
        auto* header = static_cast<slice_dsm_header*>(coordinate);
        header->magic = k_bioseq_slice_dsm_magic;
        header->codec_version = k_bioseq_slice_dsm_version;
        header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
        header->recipe_count = runtime.boundary_count;
        header->slot_count = slot_count;
        header->cycle = 1u;
        header->slot_stride = layout.slot_stride;
        runtime.mapped_header = header;
        runtime.mapped_cycle = 1u;
        auto* detach = static_cast<slice_dsm_detach_state*>(
            MemoryContextAllocZero(TopMemoryContext, sizeof(slice_dsm_detach_state)));
        /* The query-owner token exists before registration so an ERROR from
         * on_dsm_detach is reclaimed by the already-installed query callback.
         * Acquire the independent callback token/attachment retain only after
         * registration succeeds; otherwise neither can become a phantom leak. */
        detach->refcount = 1u;
        detach->callback_armed = false;
        detach->pcxt = pcxt;
        detach->mapped_header = header;
        detach->attachment = runtime.attachment;
        detach->expected_cycle = 1u;
        runtime.detach_state = detach;
        on_dsm_detach(pcxt->seg, slice_project_dsm_on_detach, PointerGetDatum(detach));
        retain_slice_trace_attachment(detach->attachment);
        detach->refcount = 2u;
        detach->callback_armed = true;
    }

    void slice_project_reinitialize_dsm(CustomScanState* custom,
                                        ParallelContext* pcxt,
                                        void* coordinate) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        auto& runtime = state->runtime;
        if (!runtime.collect_actual)
            return;
        if (coordinate == nullptr || pcxt == nullptr || pcxt->seg == nullptr ||
            pcxt->nworkers < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject DSM reinitialization is invalid")));
        }
        auto* header = static_cast<slice_dsm_header*>(coordinate);
        const std::uint64_t expected_cycle = runtime.mapped_cycle;
        if (runtime.mapped_header != header || expected_cycle == 0u ||
            header->cycle != expected_cycle ||
            (runtime.detach_state != nullptr &&
             (runtime.detach_state->mapped_header != header ||
              runtime.detach_state->expected_cycle != expected_cycle))) {
            slice_project_mark_trace_invalid_noexcept(runtime.attachment);
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject DSM cycle identity changed"),
                     errdetail("expected_cycle=%llu shared_cycle=%llu",
                               static_cast<unsigned long long>(expected_cycle),
                               static_cast<unsigned long long>(header->cycle))));
        }
        slice_project_dsm_merge_audit* merge_audit = nullptr;
#ifdef BIOSEQ_TEST_INDEXES
        merge_audit = runtime.dsm_test_reinitialize_audit;
        runtime.dsm_test_reinitialize_audit = nullptr;
#endif
        auto* trace_runtime = runtime.attachment == nullptr ? nullptr : runtime.attachment->runtime;
        slice_project_merge_dsm_cycle_noexcept(
            header, pcxt, runtime.attachment, expected_cycle, merge_audit);
        if (trace_runtime == nullptr || trace_runtime->invalid ||
            trace_runtime->last_merged_cycle != expected_cycle) {
            ereport(
                ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqSliceProject DSM cycle merge was rejected"),
                 errdetail("expected_cycle=%llu merged_cycle=%llu",
                           static_cast<unsigned long long>(expected_cycle),
                           static_cast<unsigned long long>(
                               trace_runtime == nullptr ? 0u : trace_runtime->last_merged_cycle))));
        }
        if (expected_cycle == UINT64_MAX) {
            ereport(ERROR,
                    (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                     errmsg("BioseqSliceProject parallel cycle exhausted")));
        }
        const auto slot_count = static_cast<std::uint32_t>(pcxt->nworkers);
        const slice_project_dsm_layout layout =
            make_slice_project_dsm_layout(runtime.boundary_count, slot_count);
        std::memset(reinterpret_cast<char*>(header) + layout.header_bytes,
                    0,
                    layout.total_bytes - layout.header_bytes);
        header->magic = k_bioseq_slice_dsm_magic;
        header->codec_version = k_bioseq_slice_dsm_version;
        header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
        header->recipe_count = runtime.boundary_count;
        header->slot_count = slot_count;
        header->slot_stride = layout.slot_stride;
        pg_write_barrier();
        header->cycle = expected_cycle + 1u;
        runtime.mapped_header = header;
        runtime.mapped_cycle = header->cycle;
        if (runtime.detach_state != nullptr) {
            runtime.detach_state->mapped_header = header;
            pg_write_barrier();
            runtime.detach_state->expected_cycle = header->cycle;
        }
        if (runtime.owner_initialized &&
            runtime.owner_guard.phase == slice_async_owner_phase::quiesced) {
            (void)rearm_slice_async_owner_for_rescan_noexcept(&runtime.owner_guard);
        }
    }

    void slice_project_initialize_worker(CustomScanState* custom, shm_toc*, void* coordinate) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        auto& runtime = state->runtime;
        if (!runtime.collect_actual)
            return;
        if (coordinate == nullptr || ParallelWorkerNumber < 0) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker coordinate is invalid")));
        }
        auto* header = static_cast<slice_dsm_header*>(coordinate);
        if (header->slot_count > static_cast<std::uint32_t>(INT_MAX) ||
            ParallelWorkerNumber >= static_cast<int>(header->slot_count)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker slot is out of range")));
        }
        slice_project_dsm_layout layout{};
        if (!slice_project_validate_dsm_header_noexcept(
                header, runtime.boundary_count, header->slot_count, header->cycle, &layout)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker DSM header is invalid")));
        }
        auto* worker =
            slice_project_worker_slot(header, static_cast<std::uint32_t>(ParallelWorkerNumber));
        if (worker->publish_state != slice_dsm_publish_state::never_launched ||
            worker->cycle != 0u ||
            std::any_of(worker->reserved.begin(), worker->reserved.end(), [](std::byte byte) {
                return byte != std::byte{0};
            })) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject worker DSM slot is invalid")));
        }
        runtime.mapped_header = header;
        runtime.mapped_worker = worker;
        runtime.mapped_cycle = header->cycle;
        worker->cycle = header->cycle;
        pg_write_barrier();
        worker->publish_state = slice_dsm_publish_state::attached_running;
    }

    [[nodiscard]] TupleTableSlot* slice_project_publish_child(CustomScanState* custom,
                                                              bool count_row) {
        if (custom->custom_ps == NIL || list_length(custom->custom_ps) != 1) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject executor child is missing")));
        }
        auto* child = reinterpret_cast<PlanState*>(linitial(custom->custom_ps));
        TupleTableSlot* child_slot = ExecProcNode(child);
        TupleTableSlot* scan_slot = custom->ss.ss_ScanTupleSlot;
        if (TupIsNull(child_slot))
            return ExecClearTuple(scan_slot);
#ifdef BIOSEQ_TEST_INDEXES
        const auto publication_before = capture_slice_payload_allocation_test_snapshot_noexcept();
#endif
        slot_getallattrs(child_slot);
        const int natts = child_slot->tts_tupleDescriptor->natts;
        if (natts != scan_slot->tts_tupleDescriptor->natts) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATATYPE_MISMATCH),
                     errmsg("BioseqSliceProject child descriptor changed")));
        }
        ExecClearTuple(scan_slot);
        std::memcpy(scan_slot->tts_values,
                    child_slot->tts_values,
                    static_cast<std::size_t>(natts) * sizeof(Datum));
        std::memcpy(scan_slot->tts_isnull,
                    child_slot->tts_isnull,
                    static_cast<std::size_t>(natts) * sizeof(bool));
        scan_slot->tts_nvalid = natts;
        TupleDesc descriptor = scan_slot->tts_tupleDescriptor;
        for (int index = 0; index < natts; ++index) {
            if (!scan_slot->tts_isnull[index]) {
                scan_slot->tts_values[index] = MakeExpandedObjectReadOnly(
                    scan_slot->tts_values[index], false, TupleDescAttr(descriptor, index)->attlen);
            }
        }
        ExecStoreVirtualTuple(scan_slot);
#ifdef BIOSEQ_TEST_INDEXES
        record_slice_project_publication_allocation_delta_noexcept(
            publication_before, capture_slice_payload_allocation_test_snapshot_noexcept());
#endif
        if (count_row) {
            auto* state = reinterpret_cast<slice_project_state*>(custom);
            state->runtime.rows_published =
                bioseq::common::saturating_add(state->runtime.rows_published, 1u);
        }
        return scan_slot;
    }

    [[nodiscard]] TupleTableSlot* slice_project_access(ScanState* scan) {
        return slice_project_publish_child(reinterpret_cast<CustomScanState*>(scan), true);
    }

    [[nodiscard]] bool slice_project_recheck(ScanState* scan, TupleTableSlot*) {
        CustomScanState* custom = reinterpret_cast<CustomScanState*>(scan);
        if (custom->ss.ps.state == nullptr || custom->ss.ps.state->es_epq_active == nullptr)
            return true;
        return !TupIsNull(slice_project_publish_child(custom, false));
    }

    void slice_project_begin(CustomScanState* custom, EState* estate, int eflags) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
#ifdef BIOSEQ_TEST_INDEXES
        /* Claim the backend-local one-shot before any plan validation or child
         * initialization can ERROR.  The claimed mode stays on this executor node
         * for post-execution EXPLAIN diagnostics; an aborted statement destroys it
         * instead of leaking the injection to a later query. */
        state->runtime.dsm_test_fixture_mode =
            std::exchange(pending_slice_dsm_test_fixture, k_slice_dsm_test_none);
#endif
        auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
        scan->methods = &bioseq_slice_project_scan_methods();
        slice_project_validate_codec(scan);
        if (list_length(scan->custom_plans) != 1) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqSliceProject requires exactly one child plan")));
        }
        List* header = slice_project_plan_header(scan);
        state->runtime.planned_symbols = slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_symbols)));
        state->runtime.planned_bytes = slice_project_int8_value(
            reinterpret_cast<Node*>(list_nth(header, k_slice_header_planned_bytes)));
        /* Register the upper-lease reset callback before ExecInitNode registers
         * any child/provider callbacks.  Their reverse-order teardown then closes
         * providers before this callback drops the independently retained routes. */
        state->runtime.source_routes_callback.func = slice_project_source_routes_reset;
        state->runtime.source_routes_callback.arg = state;
        bioseq::storage::initialize_pg_sequence_source_routes(state->runtime.source_routes,
                                                              estate->es_query_cxt);
        MemoryContextRegisterResetCallback(estate->es_query_cxt,
                                           &state->runtime.source_routes_callback);
        state->runtime.source_routes_callback_registered = true;
        Plan* child_plan = reinterpret_cast<Plan*>(linitial(scan->custom_plans));
        custom->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
    }

    [[nodiscard]] TupleTableSlot* slice_project_exec(CustomScanState* custom) {
        slice_project_ensure_trace_initialized(custom);
        TupleTableSlot* result = ExecScan(&custom->ss, slice_project_access, slice_project_recheck);
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        auto& runtime = state->runtime;
        if (TupIsNull(result) || !slice_trace_attachment_live(runtime.attachment) ||
            runtime.output_binding_count == 0u)
            return result;
        slot_getallattrs(result);
        const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
        for (std::uint32_t index = 0u; index < runtime.output_binding_count; ++index) {
            auto const& binding = runtime.output_bindings[index];
            if (binding.resno <= 0 || binding.resno > result->tts_tupleDescriptor->natts ||
                TupleDescAttr(result->tts_tupleDescriptor, binding.resno - 1)->atttypid !=
                    seq_oid) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject result binding changed")));
            }
            if (result->tts_isnull[binding.resno - 1])
                continue;
            /* CASE/COALESCE may select a flat or literal arm even though another
             * arm caused this symbolic recipe to be planned.  Only an expanded
             * result can carry provenance; a skipped lazy arm is not an error and
             * must retain zero actual boundary calls. */
            if (!VARATT_IS_EXTERNAL_EXPANDED(
                    DatumGetPointer(result->tts_values[binding.resno - 1])))
                continue;
            if (!attach_slice_trace_provenance(result->tts_values[binding.resno - 1],
                                               runtime.attachment,
                                               binding.producer_value_id)) {
                ereport(ERROR,
                        (errcode(ERRCODE_DATA_CORRUPTED),
                         errmsg("BioseqSliceProject result lost expanded provenance")));
            }
        }
        return result;
    }

    void slice_project_end(CustomScanState* custom) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        /* No source-backed task may outlive the owning upper executor. */
        if (state->runtime.owner_initialized) {
            finish_slice_async_owner_noexcept(&state->runtime.owner_guard,
                                              slice_shutdown_disposition::complete);
            state->runtime.attachment = nullptr;
        }
        /* Core frees this node's ExprContext only after EndCustomScan returns.
         * Drain the last projected EOH while both the child provider and our upper
         * route are still live; otherwise provider_end and upper_lease_end can both
         * sweep before that EOH drops its source-domain reference, leaving only the
         * registry token after a cursor CLOSE with no later statement boundary. */
        if (custom->ss.ps.ps_ResultTupleSlot != nullptr) {
            ExecClearTuple(custom->ss.ps.ps_ResultTupleSlot);
        }
        if (custom->ss.ps.ps_ExprContext != nullptr) {
            ReScanExprContext(custom->ss.ps.ps_ExprContext);
        }
        if (custom->custom_ps != NIL) {
            ExecEndNode(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
            custom->custom_ps = NIL;
        }
        ExecClearTuple(custom->ss.ss_ScanTupleSlot);
        /* Child End closes every provider first.  The independent upper leases
         * keep registries alive until this final release/sweep. */
        if (state->runtime.source_routes_acquired) {
            bioseq::storage::release_pg_sequence_source_routes(state->runtime.source_routes,
                                                               /*is_commit=*/true);
            state->runtime.source_routes_acquired = false;
        }
        if (state->runtime.detach_state != nullptr) {
            slice_project_release_detach_state(state->runtime.detach_state);
            state->runtime.detach_state = nullptr;
        }
        state->runtime.mapped_header = nullptr;
        state->runtime.mapped_worker = nullptr;
    }

    void slice_project_rescan(CustomScanState* custom) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        slice_project_ensure_trace_initialized(custom);
        if (state->runtime.owner_initialized) {
            quiesce_slice_async_owner_noexcept(&state->runtime.owner_guard,
                                               slice_shutdown_disposition::complete);
        }
        /* Reset the ScanState projection/result machinery before the parameterized
         * child is rescanned.  Clearing only ss_ScanTupleSlot leaves ExecScan's
         * prior projected datum live, so a NULL source on the next PARAM_EXEC pass
         * can inherit the preceding non-NULL slice. */
        ExecScanReScan(&custom->ss);
        /* ExecScanReScan resets scan input and expression state but does not
         * guarantee that a previously projected virtual result is invalidated.
         * Nested-loop PARAM_EXEC rescans must not return that stale expanded slice
         * when the next child row projects NULL. */
        if (custom->ss.ps.ps_ResultTupleSlot != nullptr) {
            ExecClearTuple(custom->ss.ps.ps_ResultTupleSlot);
        }
        if (custom->custom_ps != NIL) {
            auto* child = reinterpret_cast<PlanState*>(linitial(custom->custom_ps));
            /* CustomScan children live in custom_ps rather than lefttree/righttree,
             * so core ExecReScan cannot propagate the parent's changed PARAM_EXEC
             * set for us.  Preserve that invalidation before rescanning the
             * parameterized join/Append subtree. */
            if (custom->ss.ps.chgParam != nullptr) {
                UpdateChangedParamSet(child, custom->ss.ps.chgParam);
            }
            ExecReScan(child);
        }
        if (state->runtime.source_routes_acquired) {
            for (std::size_t index = 0u; index < state->runtime.source_routes.count; ++index) {
                bioseq::storage::sweep_reclaimable_pg_sequence_source_domains(
                    bioseq::storage::pg_sequence_source_route_lease(state->runtime.source_routes,
                                                                    index),
                    bioseq::storage::pg_sequence_source_sweep_boundary::rescan,
                    /*is_commit=*/true);
            }
        }
        state->runtime.rescans = bioseq::common::saturating_add(state->runtime.rescans, 1u);
        const bool worker_rearmed = slice_project_rearm_worker_for_rescan_noexcept(state);
        if (state->runtime.owner_initialized &&
            state->runtime.owner_guard.phase == slice_async_owner_phase::quiesced &&
            worker_rearmed) {
            (void)rearm_slice_async_owner_for_rescan_noexcept(&state->runtime.owner_guard);
        }
    }

    void slice_project_mark_pos(CustomScanState* custom) {
        if (custom->custom_ps != NIL) {
            ExecMarkPos(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
        }
    }

    void slice_project_restr_pos(CustomScanState* custom) {
        if (custom->custom_ps != NIL) {
            ExecRestrPos(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
        }
    }

    [[nodiscard]] char*
    slice_project_deparse_expression(CustomScan* scan, int expression_index, ExplainState* es) {
        if (expression_index < 0 || expression_index >= list_length(scan->custom_exprs)) {
            return pstrdup("<omitted>");
        }
        Node* expression = reinterpret_cast<Node*>(list_nth(scan->custom_exprs, expression_index));
        return deparse_expression(expression, es->deparse_cxt, true, false);
    }

    [[nodiscard]] const char* slice_project_opcode_name(slice_project_opcode opcode) noexcept {
        switch (opcode) {
        case slice_project_opcode::source_expr:
            return "source";
        case slice_project_opcode::literal_expr:
            return "literal";
        case slice_project_opcode::element:
            return "element";
        case slice_project_opcode::slice:
            return "slice";
        case slice_project_opcode::concat:
            return "concat";
        case slice_project_opcode::reverse:
            return "reverse";
        case slice_project_opcode::reverse_complement:
            return "reverse complement";
        case slice_project_opcode::length:
            return "length";
        case slice_project_opcode::cardinality:
            return "cardinality";
        case slice_project_opcode::bridge:
            return "literal bridge";
        case slice_project_opcode::materialize:
            return "materialize";
        }
        return "invalid";
    }

    [[nodiscard]] List* slice_project_node_dependency_names(List* node) {
        List* names = NIL;
        Node* dependency_node = reinterpret_cast<Node*>(list_nth(node, k_slice_node_dependencies));
        if (dependency_node == nullptr)
            return names;
        for (Node* dependency : postgres_list<Node>(reinterpret_cast<List*>(dependency_node))) {
            const std::string value = fmt::format("value_{}", intVal(dependency));
            names = lappend(names, pstrdup(value.c_str()));
        }
        return names;
    }

    [[nodiscard]] List* slice_project_dependency_names(List* record) {
        List* names = NIL;
        Node* dependency_node =
            reinterpret_cast<Node*>(list_nth(record, k_slice_record_dependencies));
        if (dependency_node == nullptr)
            return names;
        for (Node* dependency : postgres_list<Node>(reinterpret_cast<List*>(dependency_node))) {
            std::array<char, 32> variable{};
            snprintf(variable.data(), variable.size(), "var_%d", intVal(dependency));
            names = lappend(names, pstrdup(variable.data()));
        }
        return names;
    }

#ifdef BIOSEQ_TEST_INDEXES
    [[nodiscard]] const char* slice_project_dsm_test_fixture_name(int mode) noexcept {
        switch (mode) {
        case k_slice_dsm_test_incomplete:
            return "incomplete";
        case k_slice_dsm_test_malformed:
            return "malformed";
        default:
            return "none";
        }
    }

    void slice_project_explain_dsm_test_fixture(slice_project_state const* state,
                                                ExplainState* es) {
        if (state == nullptr || es == nullptr || !state->runtime.dsm_test_fixture_applied)
            return;
        auto const& runtime = state->runtime;
        const char* mode = slice_project_dsm_test_fixture_name(runtime.dsm_test_fixture_mode);
        if (es->format == EXPLAIN_FORMAT_TEXT) {
            auto property = [es](const char* name, const char* value) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str, "%s: %s\n", name, value);
            };
            property("DSM Test Fixture", mode);
            property("DSM Fixture Validated",
                     runtime.dsm_test_fixture_validated ? "true" : "false");
            property("DSM Async Lifecycle Validated",
                     runtime.dsm_test_async_validated ? "true" : "false");
            const auto integer_property = [es](const char* name, std::uint32_t value) {
                appendStringInfoSpaces(es->str, es->indent * 2);
                appendStringInfo(es->str, "%s: %u\n", name, value);
            };
            integer_property("DSM Fixture Checks", runtime.dsm_test_checks);
            integer_property("DSM Fixture Slot Headers Read", runtime.dsm_test_slot_headers_read);
            integer_property("DSM Fixture Counter Records Read",
                             runtime.dsm_test_counter_records_read);
            integer_property("DSM Async Cleanup Callbacks", runtime.dsm_test_cleanup_callbacks);
            return;
        }
        ExplainPropertyText("DSM Test Fixture", mode, es);
        ExplainPropertyBool("DSM Fixture Validated", runtime.dsm_test_fixture_validated, es);
        ExplainPropertyBool("DSM Async Lifecycle Validated", runtime.dsm_test_async_validated, es);
        ExplainPropertyUInteger("DSM Fixture Checks", nullptr, runtime.dsm_test_checks, es);
        ExplainPropertyUInteger(
            "DSM Fixture Slot Headers Read", nullptr, runtime.dsm_test_slot_headers_read, es);
        ExplainPropertyUInteger(
            "DSM Fixture Counter Records Read", nullptr, runtime.dsm_test_counter_records_read, es);
        ExplainPropertyUInteger(
            "DSM Async Cleanup Callbacks", nullptr, runtime.dsm_test_cleanup_callbacks, es);
    }
#endif

    [[nodiscard]] slice_dsm_counter const&
    slice_project_explain_counter(slice_project_state const* state, std::uint32_t index) noexcept {
        static const slice_dsm_counter zero{};
        if (state == nullptr || !state->runtime.collect_actual ||
            state->runtime.counters == nullptr || index >= state->runtime.boundary_count)
            return zero;
        return state->runtime.counters[index];
    }

    void slice_project_explain_text(slice_project_state* state,
                                    CustomScan* scan,
                                    List* header,
                                    List* nodes,
                                    List* records,
                                    ExplainState* es) {
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfoString(es->str, "Slice Recipe:\n");
        for (Node* raw_node : postgres_list<Node>(nodes)) {
            List* node = reinterpret_cast<List*>(raw_node);
            const int stable_id = intVal(list_nth(node, k_slice_node_stable_id));
            const auto opcode =
                static_cast<slice_project_opcode>(intVal(list_nth(node, k_slice_node_opcode)));
            const int expression_index = intVal(list_nth(node, k_slice_node_expression_index));
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfo(es->str,
                             "value_%d [%s] = %s\n",
                             stable_id,
                             slice_project_opcode_name(opcode),
                             slice_project_deparse_expression(scan, expression_index, es));
            List* dependencies = slice_project_node_dependency_names(node);
            appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
            appendStringInfoString(es->str, "Depends On: ");
            if (dependencies == NIL) {
                appendStringInfoString(es->str, "none");
            } else {
                bool first = true;
                for (char* name : postgres_list<char>(dependencies)) {
                    if (!first)
                        appendStringInfoString(es->str, ", ");
                    appendStringInfoString(es->str, name);
                    first = false;
                }
            }
            appendStringInfoChar(es->str, '\n');
            if (opcode == slice_project_opcode::slice || opcode == slice_project_opcode::element) {
                const int lower = intVal(list_nth(node, k_slice_node_lower_expression_index));
                const int upper = intVal(list_nth(node, k_slice_node_upper_expression_index));
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(
                    es->str,
                    "Bounds: lower=%s upper=%s\n",
                    lower < 0 ? "omitted" : slice_project_deparse_expression(scan, lower, es),
                    upper < 0 ? "omitted" : slice_project_deparse_expression(scan, upper, es));
            }
        }
        appendStringInfoSpaces(es->str, es->indent * 2);
        appendStringInfoString(es->str, "Slice Materialization:\n");
        if (es->analyze && state != nullptr &&
            state->runtime.trace_runtime.incomplete_workers != 0u) {
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfo(es->str,
                             "Incomplete Workers: %u\n",
                             state->runtime.trace_runtime.incomplete_workers);
        }
        std::uint32_t counter_index = 0u;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = reinterpret_cast<List*>(raw_record);
            const int boundary_id = intVal(list_nth(record, k_slice_record_boundary_id));
            const int producer_id = intVal(list_nth(record, k_slice_record_producer_id));
            const int expression_index = intVal(list_nth(record, k_slice_record_expression_index));
            const auto mechanism = static_cast<materialization_mechanism>(
                intVal(list_nth(record, k_slice_record_mechanism)));
            const auto reason = static_cast<materialization_reason>(
                intVal(list_nth(record, k_slice_record_reason)));
            char* expression = slice_project_deparse_expression(scan, expression_index, es);
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfo(es->str, "var_%d = %s\n", boundary_id, expression);
            auto property = [es](const char* name, const char* value) {
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(es->str, "%s: %s\n", name, value);
            };
            property("Mechanism", slice_project_mechanism_name(mechanism));
            property("Reason", slice_project_reason_name(reason));
            List* dependencies = slice_project_dependency_names(record);
            StringInfoData dependency_text;
            initStringInfo(&dependency_text);
            if (dependencies == NIL) {
                appendStringInfoString(&dependency_text, "none");
            } else {
                bool first = true;
                for (char* name : postgres_list<char>(dependencies)) {
                    if (!first)
                        appendStringInfoString(&dependency_text, ", ");
                    appendStringInfoString(&dependency_text, name);
                    first = false;
                }
            }
            property("Depends On", dependency_text.data);
            property("Encoding", strVal(list_nth(record, k_slice_record_encoding)));
            property("Cache Scope", strVal(list_nth(record, k_slice_record_cache_scope)));
            if (es->verbose) {
                std::array<char, 32> producer{};
                snprintf(producer.data(), producer.size(), "%d", producer_id);
                property("Producer Value", producer.data());
                property("Source Provenance", expression);
            }
            if (es->costs) {
                const auto estimate = slice_project_int8_value(
                    reinterpret_cast<Node*>(list_nth(record, k_slice_record_estimated_symbols)));
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(es->str, "Estimated Symbols: " UINT64_FORMAT "\n", estimate);
                appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                appendStringInfo(es->str, "Estimated Bytes: " UINT64_FORMAT " bytes\n", estimate);
            }
            if (es->analyze) {
                auto const& counter = slice_project_explain_counter(state, counter_index);
                auto actual =
                    [es](const char* name, std::uint64_t value, const char* unit = nullptr) {
                        appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                        appendStringInfo(es->str,
                                         "%s: " UINT64_FORMAT "%s%s\n",
                                         name,
                                         value,
                                         unit == nullptr ? "" : " ",
                                         unit == nullptr ? "" : unit);
                    };
                actual("Actual Calls", counter.calls);
                actual("Actual Input Symbols", counter.input_symbols);
                actual("Actual Output Symbols", counter.output_symbols);
                actual("Actual Bytes", counter.bytes, "bytes");
                actual("Reused Calls", counter.reused_calls);
                actual("Peak Retained Bytes", counter.peak_retained_bytes, "bytes");
                const bool overflow =
                    counter.overflowed != 0u ||
                    (state != nullptr && (state->runtime.trace_runtime.invalid ||
                                          state->runtime.trace_runtime.overflowed));
                property("Counter Overflow", overflow ? "true" : "false");
                if (es->timing && state != nullptr && state->runtime.timing) {
                    appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
                    appendStringInfo(es->str,
                                     "Actual Time: %.3f ms\n",
                                     static_cast<double>(counter.elapsed_ns) / 1.0e6);
                }
            }
            ++counter_index;
        }
        if (records == NIL) {
            appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
            appendStringInfoString(es->str, "none\n");
        }
        if (es->costs) {
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(es->str,
                             "Planned Materialized Symbols: " UINT64_FORMAT "\n",
                             slice_project_int8_value(reinterpret_cast<Node*>(
                                 list_nth(header, k_slice_header_planned_symbols))));
            appendStringInfoSpaces(es->str, es->indent * 2);
            appendStringInfo(es->str,
                             "Planned Materialized Bytes: " UINT64_FORMAT " bytes\n",
                             slice_project_int8_value(reinterpret_cast<Node*>(
                                 list_nth(header, k_slice_header_planned_bytes))));
        }
    }

    void slice_project_explain_structured(slice_project_state* state,
                                          CustomScan* scan,
                                          List* nodes,
                                          List* records,
                                          ExplainState* es) {
        ExplainOpenGroup("Slice Recipe", "Slice Recipe", false, es);
        for (Node* raw_node : postgres_list<Node>(nodes)) {
            List* node = reinterpret_cast<List*>(raw_node);
            const int stable_id = intVal(list_nth(node, k_slice_node_stable_id));
            const auto opcode =
                static_cast<slice_project_opcode>(intVal(list_nth(node, k_slice_node_opcode)));
            const int expression_index = intVal(list_nth(node, k_slice_node_expression_index));
            ExplainOpenGroup("Recipe Node", nullptr, true, es);
            ExplainPropertyUInteger(
                "Stable ID", nullptr, static_cast<std::uint64_t>(stable_id), es);
            ExplainPropertyText("Opcode", slice_project_opcode_name(opcode), es);
            ExplainPropertyText(
                "Expression", slice_project_deparse_expression(scan, expression_index, es), es);
            ExplainPropertyList("Depends On", slice_project_node_dependency_names(node), es);
            const int lower = intVal(list_nth(node, k_slice_node_lower_expression_index));
            const int upper = intVal(list_nth(node, k_slice_node_upper_expression_index));
            if (lower >= 0) {
                ExplainPropertyText(
                    "Lower Bound", slice_project_deparse_expression(scan, lower, es), es);
            }
            if (upper >= 0) {
                ExplainPropertyText(
                    "Upper Bound", slice_project_deparse_expression(scan, upper, es), es);
            }
            ExplainCloseGroup("Recipe Node", nullptr, true, es);
        }
        ExplainCloseGroup("Slice Recipe", "Slice Recipe", false, es);
        ExplainOpenGroup("Slice Materialization", "Slice Materialization", false, es);
        if (es->analyze && state != nullptr &&
            state->runtime.trace_runtime.incomplete_workers != 0u) {
            ExplainPropertyUInteger(
                "Incomplete Workers", nullptr, state->runtime.trace_runtime.incomplete_workers, es);
        }
        std::uint32_t counter_index = 0u;
        for (Node* raw_record : postgres_list<Node>(records)) {
            List* record = reinterpret_cast<List*>(raw_record);
            const int boundary_id = intVal(list_nth(record, k_slice_record_boundary_id));
            const int producer_id = intVal(list_nth(record, k_slice_record_producer_id));
            const int expression_index = intVal(list_nth(record, k_slice_record_expression_index));
            const auto mechanism = static_cast<materialization_mechanism>(
                intVal(list_nth(record, k_slice_record_mechanism)));
            const auto reason = static_cast<materialization_reason>(
                intVal(list_nth(record, k_slice_record_reason)));
            std::array<char, 32> variable{};
            snprintf(variable.data(), variable.size(), "var_%d", boundary_id);
            ExplainOpenGroup("Materialization", nullptr, true, es);
            ExplainPropertyText("Variable", variable.data(), es);
            ExplainPropertyText(
                "Expression", slice_project_deparse_expression(scan, expression_index, es), es);
            ExplainPropertyText("Mechanism", slice_project_mechanism_name(mechanism), es);
            ExplainPropertyText("Reason", slice_project_reason_name(reason), es);
            ExplainPropertyList("Depends On", slice_project_dependency_names(record), es);
            ExplainPropertyText("Encoding", strVal(list_nth(record, k_slice_record_encoding)), es);
            ExplainPropertyText(
                "Cache Scope", strVal(list_nth(record, k_slice_record_cache_scope)), es);
            if (es->verbose) {
                ExplainPropertyUInteger(
                    "Producer Value", nullptr, static_cast<std::uint64_t>(producer_id), es);
                ExplainPropertyText("Source Provenance",
                                    slice_project_deparse_expression(scan, expression_index, es),
                                    es);
            }
            if (es->costs) {
                const auto estimate = slice_project_int8_value(
                    reinterpret_cast<Node*>(list_nth(record, k_slice_record_estimated_symbols)));
                ExplainPropertyUInteger("Estimated Symbols", nullptr, estimate, es);
                ExplainPropertyUInteger("Estimated Bytes", "bytes", estimate, es);
            }
            if (es->analyze) {
                auto const& counter = slice_project_explain_counter(state, counter_index);
                ExplainPropertyUInteger("Actual Calls", nullptr, counter.calls, es);
                ExplainPropertyUInteger("Actual Input Symbols", nullptr, counter.input_symbols, es);
                ExplainPropertyUInteger(
                    "Actual Output Symbols", nullptr, counter.output_symbols, es);
                ExplainPropertyUInteger("Actual Bytes", "bytes", counter.bytes, es);
                ExplainPropertyUInteger("Reused Calls", nullptr, counter.reused_calls, es);
                ExplainPropertyUInteger(
                    "Peak Retained Bytes", "bytes", counter.peak_retained_bytes, es);
                ExplainPropertyBool(
                    "Counter Overflow",
                    counter.overflowed != 0u ||
                        (state != nullptr && (state->runtime.trace_runtime.invalid ||
                                              state->runtime.trace_runtime.overflowed)),
                    es);
                if (es->timing && state != nullptr && state->runtime.timing) {
                    ExplainPropertyFloat("Actual Time",
                                         "ms",
                                         static_cast<double>(counter.elapsed_ns) / 1.0e6,
                                         3,
                                         es);
                }
            }
            ExplainCloseGroup("Materialization", nullptr, true, es);
            ++counter_index;
        }
        ExplainCloseGroup("Slice Materialization", "Slice Materialization", false, es);
    }

    void slice_project_explain(CustomScanState* custom, List*, ExplainState* es) {
        if (es->analyze)
            slice_project_ensure_trace_initialized(custom);
#ifdef BIOSEQ_TEST_INDEXES
        if (es->analyze) {
            slice_project_apply_dsm_test_fixture(reinterpret_cast<slice_project_state*>(custom));
        }
#endif
        auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
        List* header = slice_project_plan_header(scan);
        List* nodes = slice_project_plan_nodes(scan);
        List* records = slice_project_plan_records(scan);
        auto* state = reinterpret_cast<slice_project_state*>(custom);
#ifdef BIOSEQ_TEST_INDEXES
        slice_project_explain_dsm_test_fixture(state, es);
#endif
        if (es->format == EXPLAIN_FORMAT_TEXT) {
            slice_project_explain_text(state, scan, header, nodes, records, es);
            return;
        }
        ExplainPropertyText("Projection Mode", "source-backed lazy", es);
        ExplainPropertyUInteger("Source Pieces",
                                nullptr,
                                slice_project_int8_value(reinterpret_cast<Node*>(
                                    list_nth(header, k_slice_header_source_pieces))),
                                es);
        ExplainPropertyUInteger("Estimated Piece Boundaries",
                                nullptr,
                                slice_project_int8_value(reinterpret_cast<Node*>(
                                    list_nth(header, k_slice_header_piece_boundaries))),
                                es);
        ExplainPropertyText(
            "Fallback Reason", strVal(list_nth(header, k_slice_header_fallback_reason)), es);
        slice_project_explain_structured(state, scan, nodes, records, es);
    }

    void slice_project_shutdown(CustomScanState* custom) {
        auto* state = reinterpret_cast<slice_project_state*>(custom);
        if (state->runtime.owner_initialized) {
            quiesce_slice_async_owner_noexcept(&state->runtime.owner_guard,
                                               slice_shutdown_disposition::complete);
        }
    }

    [[nodiscard]] CustomExecMethods const& bioseq_slice_project_exec_methods() noexcept {
        static_assert(std::is_same_v<decltype(&slice_project_estimate_dsm),
                                     Size (*)(CustomScanState*, ParallelContext*)>);
        static_assert(std::is_same_v<decltype(&slice_project_initialize_dsm),
                                     void (*)(CustomScanState*, ParallelContext*, void*)>);
        static_assert(std::is_same_v<decltype(&slice_project_reinitialize_dsm),
                                     void (*)(CustomScanState*, ParallelContext*, void*)>);
        static_assert(std::is_same_v<decltype(&slice_project_initialize_worker),
                                     void (*)(CustomScanState*, shm_toc*, void*)>);
        static_assert(
            std::is_same_v<decltype(&slice_project_shutdown), void (*)(CustomScanState*)>);
        static_assert(std::is_same_v<decltype(&slice_project_explain),
                                     void (*)(CustomScanState*, List*, ExplainState*)>);
        static const CustomExecMethods methods{
            .CustomName = "BioseqSliceProject",
            .BeginCustomScan = slice_project_begin,
            .ExecCustomScan = slice_project_exec,
            .EndCustomScan = slice_project_end,
            .ReScanCustomScan = slice_project_rescan,
            .MarkPosCustomScan = slice_project_mark_pos,
            .RestrPosCustomScan = slice_project_restr_pos,
            .EstimateDSMCustomScan = slice_project_estimate_dsm,
            .InitializeDSMCustomScan = slice_project_initialize_dsm,
            .ReInitializeDSMCustomScan = slice_project_reinitialize_dsm,
            .InitializeWorkerCustomScan = slice_project_initialize_worker,
            .ShutdownCustomScan = slice_project_shutdown,
            .ExplainCustomScan = slice_project_explain,
        };
        return methods;
    }

} // export custom-scan partition declarations
