/**
 * @file
 * @brief Owns SliceProject DSM, runtime, state, and plan access contracts.
 */
module;
#include <bioseq/pg_compat.h>

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
import bioseq.postgres.storage.scanner;
import bioseq.postgres.types.compact_sequence;
export import :custom_scan_slice_path_diagnostic_fixture_contracts;
export import :custom_scan_planner_method_contracts;
export import :custom_scan_slice_execution_method_contracts;

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

    /** @brief Selects a one-shot malformed DSM condition for integration diagnostics. */
    enum slice_project_dsm_test_fixture : int {
        k_slice_dsm_test_none = 0,
        k_slice_dsm_test_incomplete = 1,
        k_slice_dsm_test_malformed = 2,
    };

    /* Backend-local, one-shot diagnostic state. The next analyzed SliceProject
     * consumes the selected mode before any fixture code can raise an error. */
    int pending_slice_dsm_test_fixture = k_slice_dsm_test_none;

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

    /* A nested SliceProject must initialize its child's trace before it can bind
     * the parent projector, while child initialization calls back through the
     * parent-aware binding path. This forward declaration closes that genuine
     * initialization cycle without moving either executor stage out of order. */
    void slice_project_ensure_trace_initialized(CustomScanState* custom);
    void slice_project_reinitialize_dsm(CustomScanState* custom,
                                        ParallelContext* pcxt,
                                        void* coordinate);
    void slice_project_release_detach_state(slice_dsm_detach_state* state) noexcept;
    void slice_project_source_routes_reset(void* argument) noexcept;
    [[nodiscard]] TupleTableSlot* slice_project_publish_child(CustomScanState* custom,
                                                              bool project);
    void slice_project_apply_dsm_test_fixture(slice_project_state* state);
    void slice_project_begin(CustomScanState* custom, EState* estate, int eflags);
    [[nodiscard]] TupleTableSlot* slice_project_exec(CustomScanState* custom);
    void slice_project_end(CustomScanState* custom);
    void slice_project_rescan(CustomScanState* custom);
    void slice_project_mark_pos(CustomScanState* custom);
    void slice_project_restr_pos(CustomScanState* custom);
    [[nodiscard]] Size slice_project_estimate_dsm(CustomScanState* custom,
                                                  ParallelContext* pcxt);
    void slice_project_initialize_dsm(CustomScanState* custom,
                                      ParallelContext* pcxt,
                                      void* coordinate);
    void slice_project_initialize_worker(CustomScanState* custom,
                                         shm_toc* toc,
                                         void* coordinate);

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
