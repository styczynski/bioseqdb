/**
 * @file
 * @brief Owns SliceProject DSM, runtime, state, and plan access contracts.
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

export module bioseq.postgres.table:custom_scan_slice_path_runtime_contract;

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

import :custom_scan_slice_path_codec_contract;
import :custom_scan_slice_path_carrier_lineage;

using namespace bioseq::postgres::support;

export {
    enum class slice_dsm_publish_state : std::uint8_t {
        never_launched = 0u,
        attached_running = 1u,
        published_complete = 2u,
        published_incomplete = 3u,
    };

    struct slice_dsm_header {
        std::uint32_t magic;
        std::uint16_t codec_version;
        std::uint16_t header_bytes;
        std::uint32_t recipe_count;
        std::uint32_t slot_count;
        std::uint64_t cycle;
        std::uint64_t slot_stride;
    };

    struct slice_dsm_worker_header {
        std::uint64_t cycle;
        slice_dsm_publish_state publish_state;
        std::uint8_t overflowed;
        std::array<std::byte, 6> reserved;
    };

    static_assert(sizeof(slice_dsm_header) == 32u);
    static_assert(sizeof(slice_dsm_worker_header) == 16u);
    static_assert(std::is_trivially_copyable_v<slice_dsm_header>);
    static_assert(std::is_trivially_copyable_v<slice_dsm_worker_header>);
    static_assert(std::is_standard_layout_v<slice_dsm_header>);
    static_assert(std::is_standard_layout_v<slice_dsm_worker_header>);

    inline constexpr std::uint32_t k_bioseq_slice_dsm_magic = 0x42534432u; /* BSD2 */
    inline constexpr std::uint16_t k_bioseq_slice_dsm_version = 1u;

    struct slice_dsm_detach_state {
        std::uint32_t refcount;
        bool callback_armed;
        std::array<std::byte, 3> reserved;
        ParallelContext* pcxt;
        slice_dsm_header* mapped_header;
        slice_trace_attachment* attachment;
        std::uint64_t expected_cycle;
    };
    static_assert(std::is_standard_layout_v<slice_dsm_detach_state>);

    struct slice_project_output_binding {
        AttrNumber resno;
        std::uint32_t producer_value_id;
    };

    struct slice_project_dsm_merge_audit;

    struct slice_project_runtime {
        std::uint64_t rows_published = 0u;
        std::uint64_t rescans = 0u;
        std::uint64_t planned_symbols = 0u;
        std::uint64_t planned_bytes = 0u;
        std::uint32_t boundary_count = 0u;
        std::uint32_t output_binding_count = 0u;
        slice_project_output_binding* output_bindings = nullptr;
        slice_materialization_boundary* boundaries = nullptr;
        slice_dsm_counter* counters = nullptr;
        slice_materialization_runtime trace_runtime{};
        slice_trace_attachment* attachment = nullptr;
        slice_async_owner_guard owner_guard{};
        slice_async_job_slot* async_jobs = nullptr;
        slice_dsm_counter* async_job_counters = nullptr;
        bioseq::storage::pg_sequence_source_route_set source_routes{};
        bool owner_initialized = false;
        bool source_routes_acquired = false;
        MemoryContextCallback source_routes_callback{};
        bool source_routes_callback_registered = false;
        slice_dsm_header* mapped_header = nullptr;
        slice_dsm_worker_header* mapped_worker = nullptr;
        std::uint64_t mapped_cycle = 0u;
        slice_dsm_detach_state* detach_state = nullptr;
        bool trace_initialization_attempted = false;
        bool collect_actual = false;
        bool timing = false;
#ifdef BIOSEQ_TEST_INDEXES
        bool dsm_test_fixture_applied = false;
        bool dsm_test_fixture_validated = false;
        bool dsm_test_async_validated = false;
        int dsm_test_fixture_mode = k_slice_dsm_test_none;
        std::uint32_t dsm_test_checks = 0u;
        std::uint32_t dsm_test_slot_headers_read = 0u;
        std::uint32_t dsm_test_counter_records_read = 0u;
        std::uint32_t dsm_test_cleanup_callbacks = 0u;
        slice_project_dsm_merge_audit* dsm_test_reinitialize_audit = nullptr;
#endif
    };

    /* slice_async_max_live_jobs is the one normative concurrency bound. Released
     * slots advance their generation and are reused across arbitrarily many
     * batches without multiplying every trace boundary by dispatch capacity. */

    struct slice_project_state {
        CustomScanState custom_scan_state;
        slice_project_runtime runtime;
    };
    static_assert(std::is_standard_layout_v<slice_project_state>);
    static_assert(offsetof(slice_project_state, custom_scan_state) == 0);

    [[nodiscard]] bool
    slice_project_rearm_worker_for_rescan_noexcept(slice_project_state* state) noexcept;

    [[nodiscard]] CustomExecMethods const& bioseq_slice_project_exec_methods() noexcept;
    [[nodiscard]] CustomScanMethods const& bioseq_slice_project_scan_methods() noexcept;
    /* A nested SliceProject must initialize its child's trace before it can bind
     * the parent projector, while child initialization calls back through the
     * parent-aware binding path. This forward declaration closes that genuine
     * initialization cycle without moving either executor stage out of order. */
    void slice_project_ensure_trace_initialized(CustomScanState* custom);
    void slice_project_reinitialize_dsm(CustomScanState* custom,
                                        ParallelContext* pcxt,
                                        void* coordinate);

    [[nodiscard]] List* slice_project_header(const slice_project_ir_builder& builder) {
        List* header = list_make5(makeInteger(k_bioseq_slice_project_magic),
                                  makeInteger(k_bioseq_slice_project_codec_version),
                                  makeInteger(list_length(builder.records)),
                                  makeInteger(list_length(builder.expressions)),
                                  slice_project_int8_const(builder.piece_boundaries));
        header = lappend(header, slice_project_int8_const(builder.source_pieces));
        header = lappend(header, slice_project_int8_const(builder.planned_symbols));
        header = lappend(header, slice_project_int8_const(builder.planned_bytes));
        header = lappend(header, makeInteger(list_length(builder.nodes)));
        header = lappend(header, makeString(pstrdup("none")));
        return header;
    }

    [[nodiscard]] List* slice_project_plan_header(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* header = reinterpret_cast<Node*>(linitial(scan->custom_private));
        return header != nullptr && IsA(header, List) ? reinterpret_cast<List*>(header) : NIL;
    }

    [[nodiscard]] List* slice_project_plan_nodes(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* nodes = reinterpret_cast<Node*>(lsecond(scan->custom_private));
        return nodes != nullptr && IsA(nodes, List) ? reinterpret_cast<List*>(nodes) : NIL;
    }

    [[nodiscard]] List* slice_project_plan_records(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* records = reinterpret_cast<Node*>(lthird(scan->custom_private));
        return records != nullptr && IsA(records, List) ? reinterpret_cast<List*>(records) : NIL;
    }

    [[nodiscard]] List* slice_project_plan_output_bindings(CustomScan* scan) {
        if (scan == nullptr || list_length(scan->custom_private) != 4)
            return NIL;
        Node* bindings = reinterpret_cast<Node*>(lfourth(scan->custom_private));
        return bindings != nullptr && IsA(bindings, List) ? reinterpret_cast<List*>(bindings) : NIL;
    }

} // export custom-scan partition declarations
