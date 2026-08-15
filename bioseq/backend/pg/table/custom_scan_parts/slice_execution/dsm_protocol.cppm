/**
 * @file
 * @brief Owns SliceProject DSM layout, merge validation, and diagnostic fixtures.
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

export module bioseq.postgres.table:custom_scan_slice_execution_dsm_protocol;

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

} // export custom-scan partition declarations
