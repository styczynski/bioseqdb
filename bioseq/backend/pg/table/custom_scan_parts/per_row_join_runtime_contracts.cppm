/**
 * @file
 * @brief Defines per-row join runtime state without importing executor implementations.
 */
module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

export module bioseq.postgres.table:custom_scan_per_row_join_runtime_contracts;

import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.runtime_contract;
import bioseq.pipeline.stats;
import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
import bioseq.postgres.table.alignment_execution;

import :custom_scan_runtime_buffer_contracts;
import :custom_scan_runtime_match_contracts;

export {
    /**
     * @brief Owns the child plan and buffered alignment state for one per-row join execution.
     *
     * The query-context callback releases C++ state if PostgreSQL ERROR skips normal shutdown.
     */
    struct per_row_join_runtime {
        bioseq::scheduler::dispatch_buffer_runtime buffer{k_bioseq_runtime_buffer_reserve};
        bioseq::postgres::table::alignment_execution::alignment_node_registration_handle
            alignment_registration{};
        PlanState* child = nullptr;
        Relation target_relation = nullptr;
        std::vector<batch_join_placeholder_site> placeholder_sites{};
        AttrNumber target_slot_resno = InvalidAttrNumber;
        AttrNumber query_slot_resno = InvalidAttrNumber;
        int child_column_count = 0;
        std::uint8_t alphabet_code = 0;
        std::uint8_t storage_unit_byte_width = 0;
        std::uint8_t query_alphabet_code = 0;
        std::uint8_t query_storage_unit_byte_width = 0;
        bool source_exhausted = false;
        bool pass_fully_drained = false;
        bool child_initialized = false;
        bool child_ended = false;
        bool cleanup_done = false;
        Cost honest_startup_cost = 0;
        Cost honest_total_cost = 0;
        Cost incumbent_cost_floor = 0;
        bioseq::simd::minimap2_parameters aligner_params{};
        bool aligner_params_captured = false;
        std::optional<bioseq::simd::pipe::pipeline_description> pipeline_desc{};
        bioseq::pipeline_stats::pipeline_execution_stats aggregate_stats{};
        MemoryContextCallback cleanup_callback{};
        bool cleanup_callback_registered = false;
    };

    /** @brief Standard-layout PostgreSQL custom-scan state for a per-row join. */
    struct per_row_join_state {
        CustomScanState custom_scan_state;
        void* runtime = nullptr;
    };

    static_assert(std::is_standard_layout_v<per_row_join_state>);
    static_assert(offsetof(per_row_join_state, custom_scan_state) == 0);
}
