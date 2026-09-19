/**
 * @file
 * @brief Defines base-scan runtime state without importing executor callback implementations.
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
     * @brief Retains the exact alignment policy and pipeline description used by one scan.
     *
     * Shares the executor's immutable plan with the dispatch buffer. EXPLAIN
     * reads that same control block, including all aligner_options overrides;
     * it neither copies parameters nor takes a second executor snapshot.
     */
    struct per_scan_aligner_state {
        bioseq::simd::pipe::statement_execution_plan_owner execution_plan{};
        scan_feature_flags features{};
        bioseq::indexing::index_read_requirement index_read_requirement =
            bioseq::indexing::index_read_requirement::allow;
        bool read_sequence_index = false;

        /*
         * Structured pipeline description captured at BeginCustomScan via
         * `bioseq::simd::pipe::describe_pipeline_for(...)`.  Populated
         * once per scan (cold path) and consumed by
         * `BioseqColumnScan_ExplainCustomScan` to render the per-stage breakdown
         * without hardcoded knowledge of which parameters belong to which stage.
         */
        std::optional<bioseq::simd::pipe::pipeline_description> pipeline_desc{};
    };

    /**
     * @brief Retains a parameterized mmsearch query recipe across executor rescans.
     *
     * When the planner
     * rewrites the second mmsearch argument into a Param (the typical shape
     * for parameterized inner-side scans of a Nested Loop join — e.g.
     * `WHERE mmsearch(t2.seq, t1.seq)` becomes `mmsearch(t2.seq, $0)` on the
     * inner side), BeginCustomScan stashes the Param coordinates here. Every
     * ReScanCustomScan invocation then resolves the Param through
     * EvalParamsMutator, detoasts the resulting bioseq.seq varlena and pushes
     * the new query into tuple_dispatch_buffer::configure_mmsearch — turning a
     * single-literal scan into a many-to-many alignment under NL.
     *
     * `paramid` and `paramkind` preserve PostgreSQL's native Param coordinates.
     * PARAM_EXEC is resolved on ReScan after a Nested Loop installs its outer
     * value; PARAM_EXTERN is resolved eagerly at Begin and again on ReScan from
     * the executor's ParamListInfo.
     *
     * `seq_attno` / `column_name` / `alphabet_code` / `storage_unit_byte_width` /
     * `primary_key_attno` mirror the equivalent mmsearch_context fields so the
     * ReScan path can call configure_mmsearch with a fresh query without
     * re-walking the plan.
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
        /*
         * Stash the exact privately owned FuncExpr so ReScan resolves the same Param
         * without consulting or depending on executable qual order.
         */
        FuncExpr* mmsearch_fe = nullptr;
    };

    /** @brief Maps one projected child column to its physical sequence producer. */
    struct slice_projected_source_binding {
        std::uint32_t producer_id = 0u;
        AttrNumber physical_attno = InvalidAttrNumber;
    };

    /** @brief Selects the attribute-number namespace used to locate deferred sources. */
    enum class slice_project_source_slot_layout : std::uint8_t {
        physical_relation,
        projected_child,
    };

    /**
     * @brief Owns mutable state for one Bioseq custom-scan execution.
     *
     * The object lives on the C++ heap and is reached through the standard-layout
     * `scan_state`. A reset callback registered on the query memory context destroys it
     * after normal completion or PostgreSQL ERROR unwinding. PostgreSQL-owned tuple
     * resources are released only while their query ResourceOwner remains live.
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
        /* The finalized post-setrefs operand recipes are executor identities, not
         * deparse-only metadata.  Raw-Var owners use the direct attribute fast
         * path; every other base-local target/query is evaluated exactly once per
         * admitted row by these ExprStates after its BSS3 leaves are promoted. */
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
        /* SQL strictness for an owned mmsearch whose Const/Param query is NULL. */
        bool mmsearch_null_input = false;
        /* True only after the access-method source itself returned EOF. */
        bool source_exhausted = false;
        bool locator_passthrough = false;
        /* A SliceProject parent consumes source-backed expanded values.  The base
         * scan keeps locator stubs deferred, then promotes them through the one
         * storage-owned source provider immediately before projection. */
        bool lazy_source_projection = false;
        bioseq::storage::pg_sequence_source_provider* lazy_source_provider = nullptr;
        slice_trace_attachment* lazy_trace_attachment = nullptr;
        /* BSS3 has two distinct attribute-number namespaces.  The projected map
         * is indexed by child targetlist resno; the physical maps are indexed by
         * relation varattno.  Never infer one layout from the other. */
        std::vector<slice_projected_source_binding> lazy_source_projected_bindings{};
        std::vector<std::uint32_t> lazy_source_physical_producer_ids{};
        std::vector<std::uint8_t> lazy_source_physical_bound{};
        /* Per-physical-column lifetime roles derived after final executor
         * injection.  Residual-qual/operand sources are promoted only across their
         * authoritative ExprStates; output sources stay as locators until a
         * particular match_result is projected. */
        std::vector<std::uint8_t> mmsearch_pre_eval_source_bound{};
        std::vector<std::uint8_t> mmsearch_output_source_bound{};
        bool mmsearch_pre_eval_sources_present = false;
        int locator_parent_token = 0;
        AttrNumber locator_rowno_resno = InvalidAttrNumber;
        List* match_helper_sites = NIL;
        int stable_match_id = 0;

        /*
         * Cleanup hook registered against the query MemoryContext. Set up
         * once in BeginCustomScan (see register_runtime_cleanup_callback).
         * Postgres' MemoryContextCallback is a single-linked-list node
         * embedded directly in the runtime struct — its `next` pointer is
         * threaded into the context's chain at registration time. Each
         * bioseq_scan_runtime therefore needs exactly one such node.
         */
        MemoryContextCallback memcontext_callback{};
        scan_state* owning_state = nullptr;
        bool cleanup_done = false;
    };

    /** @brief Promote deferred sequence sources into the result context before projection. */
    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       bool allow_locator_passthrough,
                                       slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor);

    /** @brief Return the sole runtime owned by a scan state. */
    [[nodiscard]] bioseq_scan_runtime& bioseq_runtime(scan_state* state);

    /** @brief Destroy scan-owned C++ state when the query memory context resets. */
    void bioseq_runtime_cleanup_callback(void* arg);

    /** @brief Register idempotent scan-runtime cleanup in the query context. */
    void register_runtime_cleanup_callback(scan_state* state, EState* estate);
}
