/**
 * @file
 * @brief Defines BatchJoin runtime state without importing unrelated scan runtimes.
 */
module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

#include <bioseq/pg_compat.h>
#include <hwy/highway.h>

export module bioseq.postgres.table:custom_scan_batch_join_execution_runtime_state_contracts;

import bioseq.indexing.type;
import bioseq.parallelism.memory_admission;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.runtime_contract;
import bioseq.pipeline.stats;
import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_runtime_buffer_contracts;
import :custom_scan_runtime_match_contracts;

export {
    /** @brief Owns a materialized query row and any retained expanded sequence wrappers. */
    struct batch_join_query_tuple {
        /** @brief Retains an expanded sequence attribute in query-tuple memory. */
        struct retained_expanded_attribute {
            AttrNumber attno = InvalidAttrNumber;
            Datum datum = Datum{0};
            pg_sequence_descriptor descriptor{};
        };
        MinimalTuple tuple = nullptr;
        TupleTableSlot* slot = nullptr;
        std::uint64_t ordinal = 0;
        Datum recipe_datum = Datum{0};
        pg_sequence_descriptor recipe_descriptor{};
        std::vector<retained_expanded_attribute> expanded_sequence_attributes{};
    };

    /* Retaining an expanded sequence copies one EOH wrapper into its own
     * ALLOCSET_SMALL child context and retains the existing tree edge; it never
     * copies piece nodes or packed payload.  Admit both the child context's first
     * block and a conservative parent/context-header block, plus the wrapper
     * itself, without making the allowance depend on logical sequence length. */
    inline constexpr std::uint64_t k_batch_join_retained_recipe_admission_bytes =
        static_cast<std::uint64_t>(2u * ALLOCSET_SMALL_INITSIZE) +
        static_cast<std::uint64_t>(MAXALIGN(sizeof(pg_sequence_expanded)));

    /**
     * @brief Owns child plans, query materialization, buffering, and cleanup for one BatchJoin.
     *
     * PostgreSQL tuple storage belongs to `query_tuple_context`. The embedded reset callback
     * guarantees heap cleanup when executor callbacks are bypassed by PostgreSQL ERROR.
     */
    struct batch_join_runtime {
        bioseq::scheduler::dispatch_buffer_runtime buffer{k_bioseq_runtime_buffer_reserve};
        bioseq::postgres::table::alignment_execution::alignment_node_registration_handle
            alignment_registration{};
        PlanState* target_child = nullptr;
        PlanState* query_child = nullptr;
        bool target_child_initialized = false;
        bool query_child_initialized = false;
        bool target_child_ended = false;
        bool query_child_ended = false;
        MemoryContext query_tuple_context = nullptr;
        TupleTableSlot* query_staging_slot = nullptr;
        std::vector<bioseq::parallelism::memory_permit> query_permits{};
        std::vector<batch_join_query_tuple> query_tuples{};
        std::vector<bioseq::scheduler::dispatch_join_query_ref> all_query_refs{};
        std::vector<batch_join_placeholder_site> placeholder_sites{};
        std::size_t next_query_batch = 0;
        std::size_t active_query_batch_end = 0;
        bool queries_materialized = false;
        bool target_active = false;
        bool target_eof = false;
        bool is_current_pass_fully_drained = false;
        TupleTableSlot* current_target_slot = nullptr;
        Oid target_relation_oid = InvalidOid;
        AttrNumber target_storage_seq_attno = InvalidAttrNumber;
        AttrNumber target_slot_seq_resno = InvalidAttrNumber;
        AttrNumber target_slot_rowno_resno = InvalidAttrNumber;
        AttrNumber query_slot_seq_resno = InvalidAttrNumber;
        int target_column_count = 0;
        int query_column_count = 0;
        std::uint8_t alphabet_code = 0;
        std::uint8_t storage_unit_byte_width = 0;
        std::uint8_t query_alphabet_code = 0;
        std::uint8_t query_storage_unit_byte_width = 0;
        std::uint64_t distinct_targets = 0;
        std::uint64_t query_batch_count = 0;
        std::uint64_t query_planner_admission_bytes = 0;
        std::uint64_t query_reserved_admission_bytes = 0;
        std::uint64_t query_actual_materialized_bytes = 0;
        double query_planner_rows = 0.0;
        int query_planner_width = 0;
        Cost honest_startup_cost = 0.0;
        Cost honest_total_cost = 0.0;
        Cost incumbent_cost_floor = 0.0;
        bioseq::indexing::index_read_requirement index_read_requirement =
            bioseq::indexing::index_read_requirement::allow;
        bioseq::simd::minimap2_parameters aligner_params{};
        bool aligner_params_captured = false;
        std::optional<bioseq::simd::pipe::pipeline_description> pipeline_desc{};
        bool explain_only = false;
        bioseq::pipeline_stats::pipeline_execution_stats aggregate_stats{};
        MemoryContextCallback cleanup_callback{};
        bool cleanup_done = false;
    };

    /** @brief Standard-layout PostgreSQL custom-scan state for a batched join. */
    struct batch_join_state {
        CustomScanState custom_scan_state;
        void* runtime = nullptr;
    };

    static_assert(std::is_standard_layout_v<batch_join_state>);
    static_assert(offsetof(batch_join_state, custom_scan_state) == 0);
}
