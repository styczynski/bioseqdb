/**
 * @file
 * @brief Defines per-scan and join runtime state with query-context cleanup.
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

export module bioseq.postgres.table:custom_scan_runtime_support;

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

using namespace bioseq::postgres::support;

export {

    using aligner_options_catalog_row = bioseq::table::composite_rows::bioseq_aligner_options;
    using aligner_options_codec =
        bioseq::postgres::table::sql_row_codecs::bioseq_aligner_options_codec;

    [[nodiscard]] inline bioseq::simd::pipe::pipeline_description
    describe_pipeline_for_pg_alphabet(const bioseq::simd::minimap2_parameters& params,
                                      const std::uint8_t alphabet_code,
                                      const bioseq::simd::pipe::query_sketch_route route) {
        std::optional<bioseq::simd::pipe::pipeline_description> description;
        pgseq_supported_alphabets::dispatch_alphabet_by_code(
            static_cast<pgseq_supported_alphabets::alphabet_code_t>(alphabet_code),
            [&description, &params, alphabet_code, route]<sequence_alphabet alphabet>() {
                description.emplace(bioseq::simd::pipe::describe_pipeline_for<alphabet>(
                    params, alphabet_code, route));
            });
        if (!description.has_value()) {
            throw std::invalid_argument{
                "PostgreSQL alphabet dispatch produced no pipeline description"};
        }
        return std::move(*description);
    }

    /* These module imports provide the table access, row-number conversion, and
     * storage-metadata contracts used by this executor partition. */

    /*
     * Initial reservation hint passed to tuple_dispatch_buffer when a
     * bioseq_scan_runtime is constructed. Sized for the typical small-result
     * scan; the buffer reallocates linearly past this if a query produces
     * more buffered tuples.
     */
    inline constexpr std::size_t k_bioseq_runtime_buffer_reserve = 8192;

    /*
     * Per-scan projection usage flags. Walked from the plan target list once
     * per BeginCustomScan and pushed into minimap2_parameters so the minimap2
     * pipeline can skip expensive stages (divergence estimation, MAPQ, CIGAR
     * base alignment) whose outputs are never projected to the caller.
     *
     * Keeping this in one place also makes EXPLAIN consistent: the same flags
     * drive both what-to-run at execution time and which pipeline-step sections
     * are surfaced in BioseqColumnScan_ExplainCustomScan output.
     */
    struct scan_feature_flags {
        bool uses_cigar = false;      ///< bioseq.cigar(a) in target list
        bool uses_edit = false;       ///< bioseq.edit(a) projection or exact predicate
        bool uses_divergence = false; ///< bioseq.divergence(a) in target list
        bool uses_mapq = false;       ///< bioseq.mapq(a) in target list
        bool has_mmsearch = false;    ///< WHERE mmsearch(seq, 'query'::bioseq.seq) present
        /*
         * True when any placeholder projection that is resolved from a
         * match_result appears in the target list. Distinct from has_mmsearch
         * because a projection like bioseq.cigar(a) without a matching
         * mmsearch() qual is theoretically possible but yields NULLs today.
         */
        bool any_match_projection = false;
    };

    /*
     * Per-scan snapshot of the plan-level minimap2_parameters and feature flags.
     * Stored here (rather than inside buffer) so BioseqColumnScan_ExplainCustomScan
     * can print the *exact* parameters that were fed to the batch processor —
     * including any aligner_options() overrides — without a second plan walk.
     * The minimap2_parameters struct lives in bioseq::simd::active_target.
     */
    struct per_scan_aligner_state {
        bioseq::simd::minimap2_parameters params{};
        scan_feature_flags features{};
        bioseq::indexing::index_read_requirement index_read_requirement =
            bioseq::indexing::index_read_requirement::allow;
        bool read_sequence_index = false;
        bool params_captured = false;

        /*
         * Structured pipeline description captured at BeginCustomScan via
         * `bioseq::simd::pipe::describe_pipeline_for(...)`.  Populated
         * once per scan (cold path) and consumed by
         * `BioseqColumnScan_ExplainCustomScan` to render the per-stage breakdown
         * without hardcoded knowledge of which parameters belong to which stage.
         */
        std::optional<bioseq::simd::pipe::pipeline_description> pipeline_desc{};
    };

    /*
     * Param-driven mmsearch query reconfiguration metadata. When the planner
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

    /*
     * One rewritten `bioseq.symbol_count(Var, 'L')` call site. The Const*
     * replaces the FuncExpr in runtime.newtargetlist; `rank` is resolved once at
     * plan time via seqan3 so the per-row populate path is a single array
     * index into the meta_sidecar counts[]. `letter` + `column_name` are
     * retained for the EXPLAIN-time "Fetched from meta:" string.
     */
    struct symbol_count_call_site {
        Const* placeholder = nullptr;
        int rank = -1;
        char letter = '\0';
        int16_t column_attno = 0; // SEQ column whose symbols are counted
        std::string column_name;  // attname for EXPLAIN output
    };

    /*
     * Placeholder Const nodes injected by VarInjectorMutator into the
     * rewritten target list. Each one stands in for a bioseq.<field>(...)
     * FuncExpr; per-row values are written into constvalue/constisnull
     * before ExecProject runs. All pointers start as nullptr in a fresh
     * bioseq_scan_runtime; CustomExecScan's first-call init allocates them
     * and VarInjectorMutator wires them into runtime.newtargetlist.
     *
     * symbol_count_sites is populated dynamically by VarInjectorMutator as
     * it discovers `symbol_count(Var, 'L')` call sites in the target list.
     * Multiple call sites per scan are supported (different letters or
     * different columns resolve to independent placeholders), and the
     * vector is cleared at the top of every scan init so stale entries
     * from a prior query cannot bleed across.
     */
    struct placeholder_constants {
        Const* dp_score_node = nullptr; // bioseq.score / bioseq.dp_score
        Const* query_len_node = nullptr;
        Const* query_start_node = nullptr;
        Const* query_end_node = nullptr;
        Const* target_len_node = nullptr;
        Const* target_start_node = nullptr;
        Const* target_end_node = nullptr;
        Const* num_matches_node = nullptr;
        Const* block_length_node = nullptr;
        Const* edit_distance_node = nullptr;
        Const* mapq_node = nullptr;
        Const* chain_score_node = nullptr;
        Const* dp_max_score_node = nullptr;
        Const* seed_count_node = nullptr;
        Const* secondary_chain_score_node = nullptr;
        Const* is_reverse_node = nullptr;
        Const* is_primary_node = nullptr;
        Const* has_cigar_node = nullptr;
        Const* divergence_node = nullptr;
        Const* cigar_node = nullptr;
        std::vector<symbol_count_call_site> symbol_count_sites;
    };

    /*
     * Per-scan runtime state, owned via a heap pointer from scan_state
     * (see scan_state.cppm for the void* runtime slot).
     *
     * Owning these fields per-scan-state — rather than as file statics —
     * lets a single backend run two BioseqScan nodes concurrently. The bug
     * before this refactor: a CROSS JOIN of `samples` against itself shares
     * the global `output_position` cursor; once the second scan exhausted
     * its batch the first scan's next ExecCustomScan call would observe
     * `has_finished_writing() == true` against an empty cursor and return
     * EOF, capping the join at one outer row instead of four. The same
     * sharing crashed LEFT JOIN queries that combined a `WHERE mmsearch`
     * filter with a `bioseq.cigar` projection: the inner scan's
     * `clear_mmsearch_context()` wiped the outer scan's mmsearch context.
     *
     * Lives on the C++ heap so std::vector / std::map free their own
     * storage. To handle the mid-scan-error case (any longjmp out of an
     * ereport(ERROR, ...) inside Begin/Exec/End that would otherwise skip
     * EndCustomScan) we register a MemoryContext reset callback on
     * `estate->es_query_cxt` from BeginCustomScan. Postgres invokes the
     * callback when the query context is destroyed — that is, on either
     * normal end-of-query or transaction abort — so the C++ heap memory
     * never outlives a single statement. The `abort_cleanup_done` flag
     * lets EndCustomScan claim the runtime explicitly (so its
     * `clear_slots()` runs while the per-query ResourceOwner is still
     * alive) without racing the callback.
     */
    struct slice_projected_source_binding {
        std::uint32_t producer_id = 0u;
        AttrNumber physical_attno = InvalidAttrNumber;
    };

    enum class slice_project_source_slot_layout : std::uint8_t {
        physical_relation,
        projected_child,
    };

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

    /* Internal planner/executor ABI for the two-child join. */
    inline constexpr int k_bioseq_batch_join_plan_version = 3;
    inline constexpr int k_bioseq_locator_passthrough_magic = 0x424c5031; /* BLP1 */
    inline constexpr int k_bioseq_slice_source_magic = 0x42535331;        /* BSS1 */
    inline constexpr int k_bioseq_slice_source_version = 3;
    inline constexpr int k_bioseq_mmsearch_owner_magic = 0x424d4f31; /* BMO1 */
    inline constexpr int k_bioseq_mmsearch_owner_version = 2;
    inline constexpr int k_bioseq_base_match_owner_plan_magic = 0x424d5031; /* BMP1 */
    inline constexpr int k_bioseq_base_match_owner_plan_version = 2;
    inline constexpr int k_bioseq_base_match_owner_plan_fields = 7;
    inline constexpr int k_bioseq_batch_join_path_private_fields = 13;
    inline constexpr int k_bioseq_batch_join_private_fields = 18;

    /* One-child owner used when either mmsearch operand is a lazy recipe.  The
     * child is a reconstructed core INNER join with the exact mmsearch
     * RestrictInfo removed, wrapped in BioseqSliceProject to compute the two
     * per-row operand Datums. */
    inline constexpr int k_bioseq_per_row_join_plan_version = 4;
    inline constexpr int k_bioseq_per_row_join_path_private_fields = 13;
    inline constexpr int k_bioseq_per_row_join_private_fields = 19;

    inline constexpr int k_bioseq_match_owner_descriptor_magic = 0x424d4231; /* BMB1 */
    inline constexpr int k_bioseq_match_owner_descriptor_version = 1;
    inline constexpr int k_bioseq_match_owner_descriptor_fields = 6;

    enum match_owner_descriptor_index : int {
        k_match_owner_magic = 0,
        k_match_owner_version = 1,
        k_match_owner_stable_id = 2,
        k_match_owner_target_recipe = 3,
        k_match_owner_base_lineage = 4,
        k_match_owner_base_relid = 5,
    };

    enum class match_helper_binding_kind : int {
        unresolved = 0,
        exact_recipe = 1,
        unique_lineage = 2,
    };

    inline constexpr int k_bioseq_match_helper_site_fields = 8;
    enum match_helper_site_index : int {
        k_match_site_resno = 0,
        k_match_site_funcid = 1,
        k_match_site_typmod = 2,
        k_match_site_collation = 3,
        k_match_site_binding_kind = 4,
        k_match_site_canonical_argument = 5,
        k_match_site_phid = 6,
        k_match_site_stable_id = 7,
    };

    enum base_match_owner_plan_index : int {
        k_base_match_plan_magic = 0,
        k_base_match_plan_version = 1,
        k_base_match_plan_descriptor = 2,
        k_base_match_plan_sites = 3,
        k_base_match_plan_relation_oid = 4,
        k_base_match_plan_executor_descriptor = 5,
        k_base_match_plan_executor_sites = 6,
    };

    enum per_row_join_private_index : int {
        k_per_row_version = 0,
        k_per_row_target_relation_oid = 1,
        k_per_row_target_storage_attno = 2,
        k_per_row_target_slot_resno = 3,
        k_per_row_query_slot_resno = 4,
        k_per_row_child_column_count = 5,
        k_per_row_alphabet_code = 6,
        k_per_row_storage_unit_byte_width = 7,
        k_per_row_query_alphabet_code = 8,
        k_per_row_query_storage_unit_byte_width = 9,
        k_per_row_target_typmod_erased = 10,
        k_per_row_query_typmod_erased = 11,
        k_per_row_placeholder_sites = 12,
        k_per_row_mmsearch_expr = 13,
        k_per_row_index_read_requirement = 14,
        k_per_row_honest_startup_cost = 15,
        k_per_row_honest_total_cost = 16,
        k_per_row_incumbent_cost_floor = 17,
        k_per_row_match_owner_descriptor = 18,
    };

    enum batch_join_private_index : int {
        k_batch_join_version = 0,
        k_batch_join_token = 1,
        k_batch_join_target_relation_oid = 2,
        k_batch_join_target_storage_seq_attno = 3,
        k_batch_join_target_slot_seq_resno = 4,
        k_batch_join_target_slot_rowno_resno = 5,
        k_batch_join_query_slot_seq_resno = 6,
        k_batch_join_target_column_count = 7,
        k_batch_join_query_column_count = 8,
        k_batch_join_alphabet_code = 9,
        k_batch_join_storage_unit_byte_width = 10,
        k_batch_join_placeholder_sites = 11,
        k_batch_join_mmsearch_expr = 12,
        k_batch_join_index_read_requirement = 13,
        k_batch_join_honest_startup_cost = 14,
        k_batch_join_honest_total_cost = 15,
        k_batch_join_incumbent_cost_floor = 16,
        k_batch_join_match_owner_descriptor = 17,
    };

    struct batch_join_placeholder_site {
        AttrNumber resno = InvalidAttrNumber;
        Oid funcid = InvalidOid;
        int32 typmod = -1;
        Oid collation = InvalidOid;
        match_helper_binding_kind binding_kind = match_helper_binding_kind::unresolved;
        int stable_match_id = 0;
    };

    struct batch_join_query_tuple {
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

    struct batch_join_state {
        CustomScanState custom_scan_state;
        void* runtime = nullptr;
    };

    static_assert(std::is_standard_layout_v<batch_join_state>);
    static_assert(offsetof(batch_join_state, custom_scan_state) == 0);

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

    struct per_row_join_state {
        CustomScanState custom_scan_state;
        void* runtime = nullptr;
    };

    static_assert(std::is_standard_layout_v<per_row_join_state>);
    static_assert(offsetof(per_row_join_state, custom_scan_state) == 0);

    void promote_slice_project_sources(scan_state* state,
                                       TupleTableSlot* slot,
                                       MemoryContext result_context,
                                       bool allow_locator_passthrough,
                                       slice_project_source_slot_layout layout,
                                       TupleDesc expected_descriptor);

    /*
     * Centralised accessor: each scan_state owns exactly one
     * bioseq_scan_runtime, allocated by BeginCustomScan via
     * register_runtime_cleanup_callback and torn down by the per-query
     * MemoryContext reset callback. The runtime is held as `void*` on
     * scan_state so the public scan-state header stays
     * standard-layout; this file is the only one that knows the concrete
     * bioseq_scan_runtime type and casts back to it.
     *
     * If we somehow get here before BeginCustomScan fired (no current
     * call path does, but future hooks could regress) we lazy-allocate
     * without registering a cleanup callback — that allocation WILL
     * leak its std::vector / std::map storage on context teardown
     * because no callback is wired to call the destructor. The bioseq_log<logging_level::debug>
     * makes the regression loud so a future maintainer notices.
     */
    [[nodiscard]] bioseq_scan_runtime& bioseq_runtime(scan_state* state) {
        if (state->runtime_state == nullptr) {
            bioseq_log<logging_level::debug>(
                "[bioseq] pg: bioseq_runtime() lazy-allocating runtime — BeginCustomScan did not "
                "fire first; this allocation will leak unless a cleanup callback is wired");
            state->runtime_state = static_cast<void*>(new bioseq_scan_runtime{});
        }
        return *static_cast<bioseq_scan_runtime*>(state->runtime_state);
    }

    /*
     * MemoryContext reset callback for bioseq_scan_runtime. Postgres invokes
     * this when `estate->es_query_cxt` is destroyed — both on normal end
     * of query and on transaction abort. The callback is the safety net
     * that prevents leaks when an ereport(ERROR, ...) longjmps over
     * BioseqColumnScan_EndCustomScan.
     *
     * Important lifecycle constraint: the MemoryContextCallback struct
     * itself is embedded INSIDE the bioseq_scan_runtime, so it must not be
     * freed before this callback fires. We therefore do NOT `delete
     * runtime_ptr` in EndCustomScan — that would dangle the callback
     * pointer in the context's reset-cb chain and crash on context
     * teardown. Instead EndCustomScan only releases per-tuple resources
     * (`buffer.clear_slots()`) while the per-query ResourceOwner is still
     * alive, sets `cleanup_done = true`, and lets this callback do the
     * actual delete when the context tears down.
     *
     * On the abort path (EndCustomScan never ran) we get `cleanup_done =
     * false` and must skip touching any Postgres tuple-table machinery
     * because the ResourceOwner is also being torn down. Postgres' own
     * bookkeeping handles the tuple-desc refcount cleanup; just freeing
     * the C++ heap is enough.
     */
    void bioseq_runtime_cleanup_callback(void* arg) {
        auto* state = static_cast<scan_state*>(arg);
        if (state == nullptr || state->runtime_state == nullptr) {
            return;
        }
        auto* runtime_ptr = static_cast<bioseq_scan_runtime*>(state->runtime_state);
        if (runtime_ptr->cleanup_done) {
            bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_runtime_cleanup_callback() "
                                             "normal-path delete after EndCustomScan");
        } else {
            bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_runtime_cleanup_callback() "
                                             "abort-path delete (slots not released)");
        }
        if (runtime_ptr->lazy_source_provider != nullptr) {
            bioseq::storage::close_pg_sequence_source_provider(
                runtime_ptr->lazy_source_provider,
                /*is_commit=*/runtime_ptr->cleanup_done);
            runtime_ptr->lazy_source_provider = nullptr;
        }
        if (runtime_ptr->lazy_trace_attachment != nullptr) {
            release_slice_trace_attachment(runtime_ptr->lazy_trace_attachment);
            runtime_ptr->lazy_trace_attachment = nullptr;
        }
        state->runtime_state = nullptr;
        delete runtime_ptr;
    }

    /*
     * Wire the cleanup callback so Postgres frees the runtime on
     * transaction abort. Idempotent — callable multiple times across
     * Begin / ReScan without effect; the callback list is keyed on the
     * MemoryContextCallback struct address, and we always pass the same
     * embedded address.
     */
    void register_runtime_cleanup_callback(scan_state* state, EState* estate) {
        if (state == nullptr || estate == nullptr) {
            return;
        }
        bioseq_scan_runtime& runtime = bioseq_runtime(state);
        if (runtime.owning_state == state) {
            return; /* already registered */
        }
        runtime.owning_state = state;
        runtime.memcontext_callback.func = bioseq_runtime_cleanup_callback;
        runtime.memcontext_callback.arg = static_cast<void*>(state);
        MemoryContextRegisterResetCallback(estate->es_query_cxt, &runtime.memcontext_callback);
    }

} // export custom-scan partition declarations
