/**
 * @file
 * @brief Defines the saved plan settings and mutable state of a base scan.
 */
module;
#include <bioseq/pg_compat.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module bioseq.postgres.table:custom_scan_scan_runtime_state_contracts;

import bioseq.indexing.type;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.pipe;
import bioseq.pipeline.runtime_contract;
import bioseq.postgres.scheduler.dispatch_buffer.runtime_contract;
import bioseq.postgres.storage.scanner.source_provider_contract;
import bioseq.postgres.table.alignment_execution;
import bioseq.postgres.table.scan_state;
import bioseq.postgres.types.compact_sequence;

import :custom_scan_runtime_buffer_contracts;
import :custom_scan_runtime_placeholder_contracts;
import :custom_scan_scan_feature_contracts;

export {
    /**
     * @brief Keeps the alignment plan and EXPLAIN description used by one scan.
     *
     * The scan and dispatch buffer share the same saved execution plan, including
     * SQL option overrides. EXPLAIN reads its parameters to describe the algorithms
     * selected for this scan.
     */
    struct per_scan_aligner_state {
        bioseq::simd::pipe::statement_execution_plan_owner execution_plan{};
        scan_feature_flags features{};
        bioseq::indexing::index_read_requirement index_read_requirement =
            bioseq::indexing::index_read_requirement::allow;
        bool read_sequence_index = false;

        /* Save stage descriptions at BeginCustomScan so EXPLAIN can print
         * each stage's algorithm and settings. */
        std::optional<bioseq::simd::pipe::pipeline_description> pipeline_desc{};
    };

    /**
     * @brief Keeps the query parameter and target metadata needed to rescan mmsearch.
     *
     * A nested-loop inner scan receives a new query sequence for each outer row.
     * PARAM_EXEC is bound on rescan after the parent installs that row's value.
     * PARAM_EXTERN is read at BeginCustomScan and again on rescan. Saving the original
     * call lets each binding use the same parameter without searching the filters.
     */
    struct mmsearch_parameter_rebind {
        bool active = false;
        int paramid = -1;
        int paramkind = -1;
        Oid param_typoid = InvalidOid;
        int32_t param_typmod = -1;
        AttrNumber seq_attno = InvalidAttrNumber;
        AttrNumber primary_key_attno = InvalidAttrNumber;
        uint8_t alphabet_code = 0;
        uint8_t storage_unit_byte_width = 0;
        std::string column_name;
        /* Keep the original call so filter ordering cannot change which query
         * parameter is rebound. */
        FuncExpr* mmsearch_fe = nullptr;
    };

    /** @brief Maps a child output column to its stored sequence column and producer ID. */
    struct slice_projected_source_binding {
        std::uint32_t producer_id = 0u;
        AttrNumber physical_attno = InvalidAttrNumber;
    };

    /** @brief Selects physical table columns or child output columns for source lookup. */
    enum class slice_project_source_slot_layout : std::uint8_t {
        physical_relation,
        projected_child,
    };

    /**
     * @brief Holds mutable execution state for one Bioseq scan.
     *
     * Allocated on the C++ heap and reached through scan_state. The query memory
     * context destroys it through a registered callback. Normal scan cleanup releases
     * PostgreSQL tuple resources while their ResourceOwner still exists.
     */
    struct bioseq_scan_runtime {
        bioseq::scheduler::dispatch_buffer_runtime buffer{k_bioseq_runtime_buffer_reserve};
        bioseq::postgres::table::alignment_execution::alignment_node_registration_handle
            alignment_registration{};
        placeholder_constants placeholder_consts{};
        List* newtargetlist = nullptr;
        per_scan_aligner_state aligner_state{};
        mmsearch_parameter_rebind param_rebind{};
        bool edit_predicate_active = false;
        std::int64_t edit_exclusive_upper_bound = 0;
        AttrNumber edit_sequence_attno = InvalidAttrNumber;
        Index match_sequence_varno = 0;
        AttrNumber match_sequence_attno = InvalidAttrNumber;
        /* These expressions use final executor column references. Direct column
         * operands can be read from the slot. Evaluated operands use ExprStates
         * once per row that passes the filters, with their sequence sources available. */
        Node* match_target_recipe = nullptr;
        Node* match_query_recipe = nullptr;
        List* match_target_lineage = NIL;
        List* match_query_lineage = NIL;
        ExprState* match_target_expr_state = nullptr;
        ExprState* match_query_expr_state = nullptr;
        int32 match_target_typmod = -1;
        int32 match_query_typmod = -1;
        bool match_target_evaluated = false;
        bool match_query_evaluated = false;
        bool match_evaluated_submission = false;
        /* An owned mmsearch has a NULL constant or parameter query. */
        bool mmsearch_null_input = false;
        /* True only after the access-method source itself returned EOF. */
        bool source_exhausted = false;
        bool locator_passthrough = false;
        /* Keep locators until the SliceProject consumer needs readable sequence
         * values, then create them through this scan's source provider. */
        bool lazy_source_projection = false;
        bioseq::storage::pg_sequence_source_provider* lazy_source_provider = nullptr;
        slice_trace_attachment* lazy_trace_attachment = nullptr;
        /* Projected bindings use child output positions. Physical bindings use
         * table attribute numbers. A projection can reorder or omit columns. */
        std::vector<slice_projected_source_binding> lazy_source_projected_bindings{};
        std::vector<std::uint32_t> lazy_source_physical_producer_ids{};
        std::vector<std::uint8_t> lazy_source_physical_bound{};
        /* Mark columns needed before alignment and during match projection
         * separately. Each group is made readable only for that evaluation step,
         * then its original locator values are restored. */
        std::vector<std::uint8_t> mmsearch_pre_eval_source_bound{};
        std::vector<std::uint8_t> mmsearch_output_source_bound{};
        bool mmsearch_pre_eval_sources_present = false;
        int locator_parent_token = 0;
        AttrNumber locator_rowno_resno = InvalidAttrNumber;
        List* match_helper_sites = NIL;
        int stable_match_id = 0;

        /* Registered once in the query memory context. This callback is part
         * of the runtime object, so EndCustomScan must keep the object alive
         * until the callback runs. */
        MemoryContextCallback memcontext_callback{};
        scan_state* owning_state = nullptr;
        bool cleanup_done = false;
    };

    /** @brief Replaces bound sequence locators with lazy values in the result memory context. */
    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       bool allow_locator_passthrough,
                                       slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor);

    /** @brief Returns this scan's runtime, allocating it on first use. */
    [[nodiscard]] bioseq_scan_runtime& bioseq_runtime(scan_state* state);

    /** @brief Destroy scan-owned C++ state when the query memory context resets. */
    void bioseq_runtime_cleanup_callback(void* arg);

    /** @brief Registers the runtime's query-context cleanup callback once. */
    void register_runtime_cleanup_callback(scan_state* state, EState* estate);
}
