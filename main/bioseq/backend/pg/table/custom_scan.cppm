/*-------------------------------------------------------------------------
 *
 * table/columnar_customscan.cppm
 *
 * Bioseq columnar CustomScan node. The planner_hook injects a CustomPath
 * pointing at this node ahead of postgres' default seq/index path; at
 * plan time it materializes a CustomScan, and at execution time we drive
 * the bioseq columnar reader through the CustomScanState helpers
 * defined here. The state object is BioseqScanState (defined in
 * table/bioseq_scan_state.cppm), which embeds CustomScanState as its
 * first field so PostgreSQL's reinterpret_cast across the C ABI stays
 * sound.
 *
 *-------------------------------------------------------------------------
 */
module;
// DOC0192 — pull <hwy/highway.h> into THIS TU's global module fragment so that
// when columnar_customscan imports multiple HWY-wrapped module BMIs
// (bioseq_sequences, bioseq_sequences.simd, bioseq_sequences.minimap_port,
// bioseq_sequences.pipe, bioseq_sidecar, bioseq_types.pg_compact_sequence),
// clang's loader has a TU-local "anchor"
// copy of the current `hwy::HWY_NAMESPACE::*` to collapse the per-BMI copies against. Without
// this anchor the loader can't merge them and emits
// `definition with same mangled name '_ZN3hwy...BitCast...' as another
// definition`. Mirrors the fix applied to bioseq_io_bench/bench_test.cpp
// (DOC0192 §6). common_simd.h forwards #include <hwy/highway.h>.
#include "common_simd.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>


extern "C" {
#include "commands/extension.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
}

export module bioseq.table:custom_scan;

import bioseq.table.io;
import :access_method;             // DOC0322 §7.5 — GetBioseqColumnTableAmRoutine, BioseqColumnScanChunkGroupsFiltered, bioseq_index_fetch_begin_extended, bioseq_beginscan_*_c
import :access_common;             // DOC0324 — bioseq_hydrate_scan_slot
import :utils;                     // DOC0322 §7.5 — BioseqColumnAttrNeeded
import bioseq.column.tam;          // DOC0322 §7.4 — tid_to_row_number, row_number_to_tid, IsBioseqColumnTableAmTable
import bioseq.config;                     // DOC0322 §9.3 — bioseq_aligner_*, bioseq_index_scan, bioseq_enable_vectorization, bioseq_read_sequence_index, bioseq_enable_parallel_execution, bioseq_min_parallel_processes, bioseq_enable_page_cache GUCs
import bioseq.vector.types;        // VectorColumn, VectorTupleTableSlot, CreateVectorTupleTableSlot, ...
import bioseq.vector.execution;    // CreateVectorizedExprList, ConstructVectorizedQualList, ExecuteVectorizedQual
import bioseq.common.mlogging;
import bioseq.pg.external.path_builder_slot;  // DOC0372 — TryAddBioseqExternalScanPaths (always-owned slot)
import bioseq.storage.metadata;
import bioseq.table.types;
import bioseq.table.scan_state;
import bioseq.pg_utils;
import bioseq.scheduler.dispatch_buffer_api;
import bioseq.scheduler.supervisor_pg;
import bioseq.parallelism.mode;
import bioseq.parallelism.memory_admission;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences;                  // compact_sequence_stdc_type_params, sequence_custom_alphabet
import bioseq.sequences.simd;             // stdc_recode_to_type_params
import bioseq.sequences.minimap_port;  // for MatchResult
import bioseq.sequences.pipe;            // for PipelineDescription / describe_pipeline_for
import bioseq.sequences.cigar;                    // for format_cigar
import bioseq.types.pg_compact_sequence; // bioseq.seq varlena layout + typmod helpers
import bioseq.storage.manager;                   // DOC0279 §12.5 — seq_locator, is_locator_stub, decode_locator_from_stub, seq_caches
import bioseq.storage.bucket_math;               // bucket_index_for_size (inline-value placement)
import bioseq.storage.lazy;              // make_lazy_in_memory (inline-value tally)
import bioseq.storage.scanner_pg;        // with_lazy_postgres (stub-value tally recompute)
import bioseq.indexing.type;             // DOC0279 §11.5 — SequenceIndexType
import bioseq.indexing.symbol_counts;    // DOC0279 §12.5 — tally_symbol_counts, SymbolCountsIndex
import bioseq.storage.indexing_pg.chunked_storage;       // IndexStorageSequencePg
import bioseq.storage.indexing_pg.admin;    // estimate_index_presence, open_sequence_storage, letter_to_rank
import bioseq.instr;                      // runtime profiler — summary at scan end
import bioseq.common.sequan_compat;                     // seq:: alphabet alias used by sequence_custom_alphabet
import bioseq.pg_utils.nodes;             // DOC0333 — pg_node_view/try_as, expression walker+mutator adapters, arg-shape helpers
import bioseq.table.batch_sort_scan;      // the one chained create_upper_paths dispatcher

using namespace bioseq::pg_utils;         // DOC0333 — node view/walker/mutator helpers live in bioseq::pg_utils

/*
 * DOC0322 §7.5 — the former `extern "C++"` cross-partition reaches are replaced
 * by real module imports:
 *   - GetBioseqColumnTableAmRoutine, BioseqColumnScanChunkGroupsFiltered,
 *     bioseq_index_fetch_begin_extended, and the two extern "C" bioseq_beginscan_*_c
 *     now come from `import :access_method`; bioseq_hydrate_scan_slot comes
 *     from the narrower `import :access_common`.
 *     :custom_scan -> :access_method is one-directional (the former reverse edge,
 *     :access_method calling bioseq_customscan_init, was inverted: the call now
 *     runs from bioseq_init).
 *   - tid_to_row_number / row_number_to_tid / IsBioseqColumnTableAmTable come from
 *     `import bioseq_column_tam` (the §7.4 leaf).
 *   - BioseqColumnTableRowCount comes from `import storage_metadata` (above; its
 *     definition moved to storage_metadata:core, §7.4).
 *   - BioseqColumnAttrNeeded comes from the sibling `import :utils`.
 */


/*
 * Initial reservation hint passed to TupleDispatchBuffer when a
 * BioseqScanRuntime is constructed. Sized for the typical small-result
 * scan; the buffer reallocates linearly past this if a query produces
 * more buffered tuples.
 */
constexpr std::size_t kBioseqRuntimeBufferReserve = 8192;

/*
 * Per-scan projection usage flags. Walked from the plan target list once
 * per BeginCustomScan and pushed into Minimap2Params so the minimap2
 * pipeline can skip expensive stages (divergence estimation, MAPQ, CIGAR
 * base alignment) whose outputs are never projected to the caller.
 *
 * Keeping this in one place also makes EXPLAIN consistent: the same flags
 * drive both what-to-run at execution time and which pipeline-step sections
 * are surfaced in BioseqColumnScan_ExplainCustomScan output.
 */
struct BioseqScanFeatureFlags {
	bool uses_cigar      = false;  ///< bioseq.cigar(a) in target list
	bool uses_edit       = false;  ///< bioseq.edit(a) projection or exact predicate
	bool uses_divergence = false;  ///< bioseq.divergence(a) in target list
	bool uses_mapq       = false;  ///< bioseq.mapq(a) in target list
	bool has_mmsearch    = false;  ///< WHERE mmsearch(seq, 'query'::bioseq.seq) present
	/*
	 * True when any placeholder projection that is resolved from a
	 * MatchResult appears in the target list. Distinct from has_mmsearch
	 * because a projection like bioseq.cigar(a) without a matching
	 * mmsearch() qual is theoretically possible but yields NULLs today.
	 */
	bool any_match_projection = false;
};

/*
 * Per-scan snapshot of the plan-level Minimap2Params and feature flags.
 * Stored here (rather than inside buffer) so BioseqColumnScan_ExplainCustomScan
 * can print the *exact* parameters that were fed to the batch processor —
 * including any aligner_options() overrides — without a second plan walk.
 * The Minimap2Params struct lives in bioseq::simd::active_target.
 */
struct PerScanAlignerState {
	bioseq::simd::active_target::Minimap2Params params{};
	BioseqScanFeatureFlags              features{};
	bool                                params_captured = false;

	/*
	 * Structured pipeline description captured at BeginCustomScan via
	 * `bioseq::simd::active_target::pipe::describe_pipeline_for(...)`.  Populated
	 * once per scan (cold path) and consumed by
	 * `BioseqColumnScan_ExplainCustomScan` to render the per-stage breakdown
	 * without hardcoded knowledge of which parameters belong to which
	 * stage (DOC0249 §11, §12.2).
	 */
	std::optional<bioseq::simd::active_target::pipe::PipelineDescription> pipeline_desc{};
};

/*
 * Param-driven mmsearch query reconfiguration metadata. When the planner
 * rewrites the second mmsearch argument into a Param (the typical shape
 * for parameterized inner-side scans of a Nested Loop join — e.g.
 * `WHERE mmsearch(t2.seq, t1.seq)` becomes `mmsearch(t2.seq, $0)` on the
 * inner side), BeginCustomScan stashes the Param coordinates here. Every
 * ReScanCustomScan invocation then resolves the Param through
 * EvalParamsMutator, detoasts the resulting bioseq.seq varlena and pushes
 * the new query into TupleDispatchBuffer::configure_mmsearch — turning a
 * single-literal scan into a many-to-many alignment under NL.
 *
 * `paramid` and `paramkind` preserve PostgreSQL's native Param coordinates.
 * PARAM_EXEC is resolved on ReScan after a Nested Loop installs its outer
 * value; PARAM_EXTERN is resolved eagerly at Begin and again on ReScan from
 * the executor's ParamListInfo.
 *
 * `seq_attno` / `column_name` / `alphabet_code` / `storage_unit_byte_width` /
 * `primary_key_attno` mirror the equivalent MmsearchContext fields so the
 * ReScan path can call configure_mmsearch with a fresh query without
 * re-walking the plan.
 */
struct MmsearchParamRebind {
	bool       active             = false;
	int        paramid            = -1;
	int        paramkind          = -1;
	Oid        param_typoid       = InvalidOid;
	int32_t    param_typmod       = -1;
	AttrNumber seq_attno          = InvalidAttrNumber;
	AttrNumber primary_key_attno  = InvalidAttrNumber;
	uint8_t    alphabet_code      = 0;
	uint8_t    storage_unit_byte_width         = 0;
	std::string column_name;
	/*
	 * Stash the exact privately owned FuncExpr so ReScan resolves the same Param
	 * without consulting or depending on executable qual order.
	 */
	FuncExpr*  mmsearch_fe        = nullptr;
};

/*
 * One rewritten `bioseq.symbol_count(Var, 'L')` call site. The Const*
 * replaces the FuncExpr in runtime.newtargetlist; `rank` is resolved once at
 * plan time via seqan3 so the per-row populate path is a single array
 * index into the meta_sidecar counts[]. `letter` + `column_name` are
 * retained for the EXPLAIN-time "Fetched from meta:" string.
 */
struct SymbolCountCallSite {
	Const*      placeholder   = nullptr;
	int         rank          = -1;
	char        letter        = '\0';
	int16_t     column_attno  = 0;   // DOC0260 §11.1 — which SEQ column this counts
	std::string column_name;    // attname for EXPLAIN output
};

/*
 * Placeholder Const nodes injected by VarInjectorMutator into the
 * rewritten target list. Each one stands in for a bioseq.<field>(...)
 * FuncExpr; per-row values are written into constvalue/constisnull
 * before ExecProject runs. All pointers start as nullptr in a fresh
 * BioseqScanRuntime; CustomExecScan's first-call init allocates them
 * and VarInjectorMutator wires them into runtime.newtargetlist.
 *
 * symbol_count_sites is populated dynamically by VarInjectorMutator as
 * it discovers `symbol_count(Var, 'L')` call sites in the target list.
 * Multiple call sites per scan are supported (different letters or
 * different columns resolve to independent placeholders), and the
 * vector is cleared at the top of every scan init so stale entries
 * from a prior query cannot bleed across.
 */
struct BioseqPlaceholderConsts {
	Const* dp_score_node              = nullptr;  // bioseq.score / bioseq.dp_score
	Const* query_len_node             = nullptr;
	Const* query_start_node           = nullptr;
	Const* query_end_node             = nullptr;
	Const* target_len_node            = nullptr;
	Const* target_start_node          = nullptr;
	Const* target_end_node            = nullptr;
	Const* num_matches_node           = nullptr;
	Const* block_length_node          = nullptr;
	Const* edit_distance_node         = nullptr;
	Const* mapq_node                  = nullptr;
	Const* chain_score_node           = nullptr;
	Const* dp_max_score_node          = nullptr;
	Const* seed_count_node            = nullptr;
	Const* secondary_chain_score_node = nullptr;
	Const* is_reverse_node            = nullptr;
	Const* is_primary_node            = nullptr;
	Const* has_cigar_node             = nullptr;
	Const* divergence_node            = nullptr;
	Const* cigar_node                 = nullptr;
	std::vector<SymbolCountCallSite> symbol_count_sites;
};

/*
 * Per-scan runtime state, owned via a heap pointer from BioseqScanState
 * (see bioseq_scan_state.cppm for the void* runtime slot).
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
struct BioseqScanRuntime {
	bioseq::scheduler::DispatchBufferRuntime buffer{kBioseqRuntimeBufferReserve};
	bioseq::pg::alignment_execution::AlignmentNodeRegistrationHandle
		alignment_registration{};
	BioseqPlaceholderConsts                   placeholder_consts{};
	List*                                     newtargetlist = nullptr;
	PerScanAlignerState                       aligner_state{};
	MmsearchParamRebind                       param_rebind{};
	bool                                      edit_predicate_active = false;
	std::int64_t                              edit_exclusive_upper_bound = 0;
	AttrNumber                                edit_sequence_attno = InvalidAttrNumber;
	Index                                     match_sequence_varno = 0;
	AttrNumber                                match_sequence_attno = InvalidAttrNumber;
	/* SQL strictness for an owned mmsearch whose Const/Param query is NULL. */
	bool                                      mmsearch_null_input = false;
	/* True only after the access-method source itself returned EOF. */
	bool                                      source_exhausted = false;
	bool                                      locator_passthrough = false;
	/* A SliceProject parent consumes source-backed expanded values.  The base
	 * scan keeps locator stubs deferred, then promotes them through the one
	 * storage-owned source provider immediately before projection. */
	bool                                      lazy_source_projection = false;
	bioseq::storage::pg_sequence_source_provider*
	                                          lazy_source_provider = nullptr;
	slice_trace_attachment*                   lazy_trace_attachment = nullptr;
	std::vector<std::uint32_t>                lazy_source_producer_ids{};
	std::vector<AttrNumber>                   lazy_source_column_attnos{};
	int                                       locator_parent_token = 0;
	AttrNumber                                locator_rowno_resno = InvalidAttrNumber;

	/*
	 * Cleanup hook registered against the query MemoryContext. Set up
	 * once in BeginCustomScan (see register_runtime_cleanup_callback).
	 * Postgres' MemoryContextCallback is a single-linked-list node
	 * embedded directly in the runtime struct — its `next` pointer is
	 * threaded into the context's chain at registration time. Each
	 * BioseqScanRuntime therefore needs exactly one such node.
	 */
	MemoryContextCallback                     memcontext_callback{};
	BioseqScanState*                          owning_state = nullptr;
	bool                                      cleanup_done = false;
};

/* Internal planner/executor ABI for the two-child join. */
constexpr int kBioseqBatchJoinPlanVersion = 2;
constexpr int kBioseqLocatorPassthroughMagic = 0x424c5031; /* BLP1 */
constexpr int kBioseqSliceSourceMagic = 0x42535331; /* BSS1 */
constexpr int kBioseqSliceSourceVersion = 3;
constexpr int kBioseqMmsearchOwnerMagic = 0x424d4f31; /* BMO1 */
constexpr int kBioseqMmsearchOwnerVersion = 1;
constexpr int kBioseqBatchJoinPathPrivateFields = 13;
constexpr int kBioseqBatchJoinPrivateFields = 17;

/* One-child owner used when either mmsearch operand is a lazy recipe.  The
 * child is a reconstructed core INNER join with the exact mmsearch
 * RestrictInfo removed, wrapped in BioseqSliceProject to compute the two
 * per-row operand Datums. */
constexpr int kBioseqPerRowJoinPlanVersion = 1;
constexpr int kBioseqPerRowJoinPathPrivateFields = 13;
constexpr int kBioseqPerRowJoinPrivateFields = 14;

enum BioseqPerRowJoinPrivateIndex : int {
	kPerRowVersion = 0,
	kPerRowTargetRelationOid = 1,
	kPerRowTargetStorageAttno = 2,
	kPerRowTargetSlotResno = 3,
	kPerRowQuerySlotResno = 4,
	kPerRowChildColumnCount = 5,
	kPerRowAlphabetCode = 6,
	kPerRowStorageUnitByteWidth = 7,
	kPerRowPlaceholderSites = 8,
	kPerRowMmsearchExpr = 9,
	kPerRowIndexReadRequirement = 10,
	kPerRowHonestStartupCost = 11,
	kPerRowHonestTotalCost = 12,
	kPerRowIncumbentCostFloor = 13,
};

enum BioseqBatchJoinPrivateIndex : int {
	kBatchJoinVersion = 0,
	kBatchJoinToken = 1,
	kBatchJoinTargetRelationOid = 2,
	kBatchJoinTargetStorageSeqAttno = 3,
	kBatchJoinTargetSlotSeqResno = 4,
	kBatchJoinTargetSlotRownoResno = 5,
	kBatchJoinQuerySlotSeqResno = 6,
	kBatchJoinTargetColumnCount = 7,
	kBatchJoinQueryColumnCount = 8,
	kBatchJoinAlphabetCode = 9,
	kBatchJoinStorageUnitByteWidth = 10,
	kBatchJoinPlaceholderSites = 11,
	kBatchJoinMmsearchExpr = 12,
	kBatchJoinIndexReadRequirement = 13,
	kBatchJoinHonestStartupCost = 14,
	kBatchJoinHonestTotalCost = 15,
	kBatchJoinIncumbentCostFloor = 16,
};

struct BioseqBatchJoinPlaceholderSite {
	AttrNumber resno = InvalidAttrNumber;
	Oid funcid = InvalidOid;
	int32 typmod = -1;
	Oid collation = InvalidOid;
};

struct BioseqBatchJoinQueryTuple {
	MinimalTuple tuple = nullptr;
	TupleTableSlot* slot = nullptr;
	std::uint64_t ordinal = 0;
};

struct BioseqBatchJoinRuntime {
	bioseq::scheduler::DispatchBufferRuntime buffer{kBioseqRuntimeBufferReserve};
	bioseq::pg::alignment_execution::AlignmentNodeRegistrationHandle
		alignment_registration{};
	PlanState* target_child = nullptr;
	PlanState* query_child = nullptr;
	bool target_child_initialized = false;
	bool query_child_initialized = false;
	bool target_child_ended = false;
	bool query_child_ended = false;
	MemoryContext query_tuple_context = nullptr;
	std::vector<bioseq::parallelism::MemoryPermit> query_permits{};
	std::vector<BioseqBatchJoinQueryTuple> query_tuples{};
	std::vector<bioseq::scheduler::DispatchJoinQueryRef> all_query_refs{};
	std::vector<BioseqBatchJoinPlaceholderSite> placeholder_sites{};
	std::size_t next_query_batch = 0;
	std::size_t active_query_batch_end = 0;
	bool queries_materialized = false;
	bool target_active = false;
	bool target_eof = false;
	bool current_pass_fully_drained = false;
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
	bioseq::indexing::IndexReadRequirement index_read_requirement =
		bioseq::indexing::IndexReadRequirement::allow;
	bool explain_only = false;
	bioseq::pipeline_stats::PipelineExecutionStats aggregate_stats{};
	MemoryContextCallback cleanup_callback{};
	bool cleanup_done = false;
};

struct BioseqBatchJoinState {
	CustomScanState custom_scanstate;
	void* runtime = nullptr;
};

static_assert(std::is_standard_layout_v<BioseqBatchJoinState>);
static_assert(offsetof(BioseqBatchJoinState, custom_scanstate) == 0);

struct BioseqPerRowJoinRuntime {
	bioseq::scheduler::DispatchBufferRuntime buffer{kBioseqRuntimeBufferReserve};
	bioseq::pg::alignment_execution::AlignmentNodeRegistrationHandle
		alignment_registration{};
	PlanState* child = nullptr;
	Relation target_relation = nullptr;
	std::vector<BioseqBatchJoinPlaceholderSite> placeholder_sites{};
	AttrNumber target_slot_resno = InvalidAttrNumber;
	AttrNumber query_slot_resno = InvalidAttrNumber;
	int child_column_count = 0;
	std::uint8_t alphabet_code = 0;
	std::uint8_t storage_unit_byte_width = 0;
	bool source_exhausted = false;
	bool pass_fully_drained = false;
	bool child_initialized = false;
	bool child_ended = false;
	bool cleanup_done = false;
	Cost honest_startup_cost = 0;
	Cost honest_total_cost = 0;
	Cost incumbent_cost_floor = 0;
	bioseq::pipeline_stats::PipelineExecutionStats aggregate_stats{};
	MemoryContextCallback cleanup_callback{};
	bool cleanup_callback_registered = false;
};

struct BioseqPerRowJoinState {
	CustomScanState custom_scanstate;
	void* runtime = nullptr;
};

static_assert(std::is_standard_layout_v<BioseqPerRowJoinState>);
static_assert(offsetof(BioseqPerRowJoinState, custom_scanstate) == 0);

static void promote_slice_project_sources(
	BioseqScanState* state, TupleTableSlot* slot,
	MemoryContext result_context, bool allow_locator_passthrough);

/*
 * Centralised accessor: each BioseqScanState owns exactly one
 * BioseqScanRuntime, allocated by BeginCustomScan via
 * register_runtime_cleanup_callback and torn down by the per-query
 * MemoryContext reset callback. The runtime is held as `void*` on
 * BioseqScanState so the public scan-state header stays
 * standard-layout; this file is the only one that knows the concrete
 * BioseqScanRuntime type and casts back to it.
 *
 * If we somehow get here before BeginCustomScan fired (no current
 * call path does, but future hooks could regress) we lazy-allocate
 * without registering a cleanup callback — that allocation WILL
 * leak its std::vector / std::map storage on context teardown
 * because no callback is wired to call the destructor. The bioseq_log<DBG>
 * makes the regression loud so a future maintainer notices.
 */
[[nodiscard]] static BioseqScanRuntime& bioseq_runtime(BioseqScanState* state) {
	if (state->runtime == nullptr) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_runtime() lazy-allocating runtime — BeginCustomScan did not fire first; this allocation will leak unless a cleanup callback is wired");
		state->runtime = static_cast<void*>(new BioseqScanRuntime{});
	}
	return *static_cast<BioseqScanRuntime*>(state->runtime);
}

/*
 * MemoryContext reset callback for BioseqScanRuntime. Postgres invokes
 * this when `estate->es_query_cxt` is destroyed — both on normal end
 * of query and on transaction abort. The callback is the safety net
 * that prevents leaks when an ereport(ERROR, ...) longjmps over
 * BioseqColumnScan_EndCustomScan.
 *
 * Important lifecycle constraint: the MemoryContextCallback struct
 * itself is embedded INSIDE the BioseqScanRuntime, so it must not be
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
static void bioseq_runtime_cleanup_callback(void* arg) {
	auto* state = static_cast<BioseqScanState*>(arg);
	if (state == nullptr || state->runtime == nullptr) {
		return;
	}
	auto* runtime_ptr = static_cast<BioseqScanRuntime*>(state->runtime);
	if (runtime_ptr->cleanup_done) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_runtime_cleanup_callback() normal-path delete after EndCustomScan");
	} else {
		bioseq_log<DBG>("[bioseq] pg: bioseq_runtime_cleanup_callback() abort-path delete (slots not released)");
	}
	if (runtime_ptr->lazy_source_provider != nullptr) {
		bioseq::storage::close_pg_sequence_source_provider(
			runtime_ptr->lazy_source_provider,
			/*is_commit=*/runtime_ptr->cleanup_done);
		runtime_ptr->lazy_source_provider = nullptr;
	}
	if (runtime_ptr->lazy_trace_attachment != nullptr) {
		release_slice_trace_attachment(
			runtime_ptr->lazy_trace_attachment);
		runtime_ptr->lazy_trace_attachment = nullptr;
	}
	state->runtime = nullptr;
	delete runtime_ptr;
}

/*
 * Wire the cleanup callback so Postgres frees the runtime on
 * transaction abort. Idempotent — callable multiple times across
 * Begin / ReScan without effect; the callback list is keyed on the
 * MemoryContextCallback struct address, and we always pass the same
 * embedded address.
 */
static void register_runtime_cleanup_callback(BioseqScanState* state,
											  EState* estate)
{
	if (state == nullptr || estate == nullptr) {
		return;
	}
	BioseqScanRuntime& runtime = bioseq_runtime(state);
	if (runtime.owning_state == state) {
		return;  /* already registered */
	}
	runtime.owning_state             = state;
	runtime.memcontext_callback.func = bioseq_runtime_cleanup_callback;
	runtime.memcontext_callback.arg  = static_cast<void*>(state);
	MemoryContextRegisterResetCallback(estate->es_query_cxt,
									   &runtime.memcontext_callback);
}

/*
 * OIDs looked up from pg_proc for the bioseq extension's SQL stubs.
 * Resolved on demand via LookupStubOids() and cached by
 * get_cached_stub_oids() for the lifetime of a backend. The cache is
 * flushed on any relcache invalidation (which covers DROP/CREATE
 * EXTENSION re-registering the pg_proc rows with new OIDs).
 */
struct BioseqStubOids {
	Oid mmsearch_oid;
	Oid mmsearch_with_opts_oid;       // mmsearch(bioseq.seq, bioseq.seq, bioseq_aligner_options)
	Oid bioseq_seq_type_oid;          // resolves to bioseq.seq pg_type row
	Oid bioseq_aligner_options_type_oid;

	Oid score_oid;                    // bioseq.score (alias for dp_score)
	Oid dp_score_oid;
	Oid query_len_oid;
	Oid query_start_oid;
	Oid query_end_oid;
	Oid target_len_oid;
	Oid target_start_oid;
	Oid target_end_oid;
	Oid num_matches_oid;
	Oid block_length_oid;
	Oid edit_oid;
	Oid mapq_oid;
	Oid chain_score_oid;
	Oid dp_max_score_oid;
	Oid seed_count_oid;
	Oid secondary_chain_score_oid;
	Oid is_reverse_oid;
	Oid is_primary_oid;
	Oid has_cigar_oid;
	Oid divergence_oid;
	Oid cigar_oid;
	Oid symbol_count_oid;
	/* DOC0377 lazy sequence projection surface.  These are resolved by exact
	 * signatures; the planner never classifies an operation by its name. */
	Oid seq_concat_oid;
	Oid seq_concat_text_oid;
	Oid text_concat_seq_oid;
	Oid seq_concat_varchar_oid;
	Oid varchar_concat_seq_oid;
	Oid seq_concat_text_array_oid;
	Oid text_array_concat_seq_oid;
	Oid seq_concat_letter_array_oid;
	Oid letter_array_concat_seq_oid;
	Oid seq_reverse_oid;
	Oid seq_reverse_complement_oid;
	Oid seq_array_cat_oid;
	Oid seq_array_length_oid;
	Oid seq_cardinality_oid;
	Oid seq_to_text_oid;
	Oid seq_to_varchar_oid;
	Oid seq_to_array_oid;
	Oid seq_from_letter_array_infer_oid;
	Oid seq_from_letter_array_named_oid;
	Oid seq_len_oid;
};

/*
 * The create_upper_paths hook is live while CREATE EXTENSION is still
 * executing.  Its catalog surface is therefore allowed to be absent or only
 * partially installed.  These missing-ok probes are deliberately separate
 * from LookupStubOids(): the executor/planner hot paths retain the exact,
 * throwing lookup once the complete surface has been proven ready.
 */
[[nodiscard]] static Oid lookup_defined_type_missing_ok(
		const char* schema_name, const char* type_name) {
	const Oid namespace_oid = get_namespace_oid(
		schema_name, /*missing_ok=*/true);
	if (!OidIsValid(namespace_oid)) return InvalidOid;
	const Oid type_oid = GetSysCacheOid2(
		TYPENAMENSP, Anum_pg_type_oid,
		PointerGetDatum(type_name), ObjectIdGetDatum(namespace_oid));
	if (!OidIsValid(type_oid)) return InvalidOid;
	HeapTuple tuple = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
	if (!HeapTupleIsValid(tuple)) return InvalidOid;
	const bool is_defined =
		reinterpret_cast<Form_pg_type>(GETSTRUCT(tuple))->typisdefined;
	ReleaseSysCache(tuple);
	return is_defined ? type_oid : InvalidOid;
}

[[nodiscard]] static bool exact_function_exists_missing_ok(
		const char* schema_name, const char* function_name,
		std::initializer_list<Oid> argument_types) {
	for (const Oid argument_type : argument_types) {
		if (!OidIsValid(argument_type)) return false;
	}
	List* qualified_name = list_make2(
		makeString(pstrdup(schema_name)),
		makeString(pstrdup(function_name)));
	const Oid function_oid = LookupFuncName(
		qualified_name, static_cast<int>(argument_types.size()),
		argument_types.begin(), /*missing_ok=*/true);
	list_free_deep(qualified_name);
	return OidIsValid(function_oid);
}

[[nodiscard]] static bool bioseq_stub_oid_surface_ready() {
	const Oid seq_oid = lookup_defined_type_missing_ok("bioseq", "seq");
	const Oid letter_oid = lookup_defined_type_missing_ok(
		"bioseq", "letter");
	const Oid options_oid = lookup_defined_type_missing_ok(
		"public", "bioseq_aligner_options");
	const Oid letter_array_oid = OidIsValid(letter_oid)
		? get_array_type(letter_oid) : InvalidOid;
	if (!OidIsValid(seq_oid) || !OidIsValid(letter_oid)
		|| !OidIsValid(options_oid) || !OidIsValid(letter_array_oid)) {
		return false;
	}

	if (!exact_function_exists_missing_ok(
			"public", "mmsearch", {seq_oid, seq_oid})
		|| !exact_function_exists_missing_ok(
			"public", "mmsearch", {seq_oid, seq_oid, options_oid})
		|| !exact_function_exists_missing_ok(
			"bioseq", "symbol_count", {seq_oid, TEXTOID})) {
		return false;
	}

	constexpr std::array<const char*, 21> projection_functions{
		"score", "dp_score", "query_len", "query_start", "query_end",
		"target_len", "target_start", "target_end", "num_matches",
		"block_length", "edit", "mapq", "chain_score", "dp_max_score",
		"seed_count", "secondary_chain_score", "is_reverse", "is_primary",
		"has_cigar", "divergence", "cigar"
	};
	for (const char* function_name : projection_functions) {
		if (!exact_function_exists_missing_ok(
				"bioseq", function_name, {ANYELEMENTOID})) {
			return false;
		}
	}

	return exact_function_exists_missing_ok(
			"bioseq", "seq_concat", {seq_oid, seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "seq_concat_text", {seq_oid, TEXTOID})
		&& exact_function_exists_missing_ok(
			"bioseq", "text_concat_seq", {TEXTOID, seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "seq_concat_varchar", {seq_oid, VARCHAROID})
		&& exact_function_exists_missing_ok(
			"bioseq", "varchar_concat_seq", {VARCHAROID, seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "seq_concat_text_array", {seq_oid, TEXTARRAYOID})
		&& exact_function_exists_missing_ok(
			"bioseq", "text_array_concat_seq", {TEXTARRAYOID, seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "seq_concat_letter_array", {seq_oid, letter_array_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "letter_array_concat_seq", {letter_array_oid, seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "reverse", {seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "reverse_complement", {seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "array_cat", {seq_oid, seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "array_length", {seq_oid, INT4OID})
		&& exact_function_exists_missing_ok(
			"bioseq", "cardinality", {seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "to_text", {seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "to_varchar", {seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "to_array", {seq_oid})
		&& exact_function_exists_missing_ok(
			"bioseq", "from_array", {letter_array_oid, INT4OID})
		&& exact_function_exists_missing_ok(
			"bioseq", "from_array", {letter_array_oid, TEXTOID, INT4OID})
		&& exact_function_exists_missing_ok(
			"bioseq", "len", {seq_oid});
}

/*
 * LookupStubOids resolves all bioseq stub OIDs via regprocedurein,
 * which raises ereport(ERROR, ...) if any function is missing. We
 * assume CREATE EXTENSION bioseq has already succeeded before any
 * path below runs. Prefer get_cached_stub_oids() on the hot path —
 * this builder is kept public so the cache can repopulate on
 * relcache invalidation.
 */
[[nodiscard]] static BioseqStubOids LookupStubOids() {
	BioseqStubOids result{};
	auto resolve = [](const char* signature) {
		return DatumGetObjectId(DirectFunctionCall1(regprocedurein,
			CStringGetDatum(signature)));
	};
	auto resolve_type = [](const char* type_name) {
		return DatumGetObjectId(DirectFunctionCall1(regtypein,
			CStringGetDatum(type_name)));
	};
	result.mmsearch_oid              = resolve("public.mmsearch(bioseq.seq, bioseq.seq)");
	result.mmsearch_with_opts_oid    = resolve(
		"public.mmsearch(bioseq.seq, bioseq.seq, public.bioseq_aligner_options)");
	result.bioseq_seq_type_oid       = resolve_type("bioseq.seq");
	result.bioseq_aligner_options_type_oid =
		resolve_type("public.bioseq_aligner_options");
	result.score_oid                 = resolve("bioseq.score(anyelement)");
	result.dp_score_oid              = resolve("bioseq.dp_score(anyelement)");
	result.query_len_oid             = resolve("bioseq.query_len(anyelement)");
	result.query_start_oid           = resolve("bioseq.query_start(anyelement)");
	result.query_end_oid             = resolve("bioseq.query_end(anyelement)");
	result.target_len_oid            = resolve("bioseq.target_len(anyelement)");
	result.target_start_oid          = resolve("bioseq.target_start(anyelement)");
	result.target_end_oid            = resolve("bioseq.target_end(anyelement)");
	result.num_matches_oid           = resolve("bioseq.num_matches(anyelement)");
	result.block_length_oid          = resolve("bioseq.block_length(anyelement)");
	result.edit_oid                  = resolve("bioseq.edit(anyelement)");
	result.mapq_oid                  = resolve("bioseq.mapq(anyelement)");
	result.chain_score_oid           = resolve("bioseq.chain_score(anyelement)");
	result.dp_max_score_oid          = resolve("bioseq.dp_max_score(anyelement)");
	result.seed_count_oid            = resolve("bioseq.seed_count(anyelement)");
	result.secondary_chain_score_oid = resolve("bioseq.secondary_chain_score(anyelement)");
	result.is_reverse_oid            = resolve("bioseq.is_reverse(anyelement)");
	result.is_primary_oid            = resolve("bioseq.is_primary(anyelement)");
	result.has_cigar_oid             = resolve("bioseq.has_cigar(anyelement)");
	result.divergence_oid            = resolve("bioseq.divergence(anyelement)");
	result.cigar_oid                 = resolve("bioseq.cigar(anyelement)");
	result.symbol_count_oid          = resolve("bioseq.symbol_count(bioseq.seq, text)");
	result.seq_concat_oid = resolve(
		"bioseq.seq_concat(bioseq.seq,bioseq.seq)");
	result.seq_concat_text_oid = resolve(
		"bioseq.seq_concat_text(bioseq.seq,text)");
	result.text_concat_seq_oid = resolve(
		"bioseq.text_concat_seq(text,bioseq.seq)");
	result.seq_concat_varchar_oid = resolve(
		"bioseq.seq_concat_varchar(bioseq.seq,character varying)");
	result.varchar_concat_seq_oid = resolve(
		"bioseq.varchar_concat_seq(character varying,bioseq.seq)");
	result.seq_concat_text_array_oid = resolve(
		"bioseq.seq_concat_text_array(bioseq.seq,text[])");
	result.text_array_concat_seq_oid = resolve(
		"bioseq.text_array_concat_seq(text[],bioseq.seq)");
	result.seq_concat_letter_array_oid = resolve(
		"bioseq.seq_concat_letter_array(bioseq.seq,bioseq.letter[])");
	result.letter_array_concat_seq_oid = resolve(
		"bioseq.letter_array_concat_seq(bioseq.letter[],bioseq.seq)");
	result.seq_reverse_oid = resolve("bioseq.reverse(bioseq.seq)");
	result.seq_reverse_complement_oid = resolve(
		"bioseq.reverse_complement(bioseq.seq)");
	result.seq_array_cat_oid = resolve(
		"bioseq.array_cat(bioseq.seq,bioseq.seq)");
	result.seq_array_length_oid = resolve(
		"bioseq.array_length(bioseq.seq,integer)");
	result.seq_cardinality_oid = resolve(
		"bioseq.cardinality(bioseq.seq)");
	result.seq_to_text_oid = resolve("bioseq.to_text(bioseq.seq)");
	result.seq_to_varchar_oid = resolve("bioseq.to_varchar(bioseq.seq)");
	result.seq_to_array_oid = resolve("bioseq.to_array(bioseq.seq)");
	result.seq_from_letter_array_infer_oid = resolve(
		"bioseq.from_array(bioseq.letter[],integer)");
	result.seq_from_letter_array_named_oid = resolve(
		"bioseq.from_array(bioseq.letter[],text,integer)");
	result.seq_len_oid = resolve("bioseq.len(bioseq.seq)");
	return result;
}

/*
 * Per-backend cache for BioseqStubOids. The prior implementation called
 * LookupStubOids() from every planner path entry (AddBioseqColumnScanPath,
 * CustomExecScan first-call init, ExplainCustomScan) — that is ~22
 * regprocedurein syscache hits per query. The cache is backed by pg_proc and
 * pg_type syscache invalidation, plus relcache invalidation for relation-driven
 * replans, so DROP/CREATE EXTENSION in one backend flushes the resolved OIDs
 * before the next lookup.
 */
struct CachedStubOids {
	BioseqStubOids oids{};
	bool           initialised = false;
};
static CachedStubOids g_cached_stub_oids{};

static void invalidate_cached_stub_oids(Datum /*arg*/, Oid /*relid*/) {
	g_cached_stub_oids.initialised = false;
}

static void invalidate_cached_stub_oids_syscache(Datum /*arg*/, int /*cacheid*/,
												 uint32 /*hashvalue*/) {
	g_cached_stub_oids.initialised = false;
}

[[nodiscard]] static bool oid_exists_in_syscache(int cache_id, Oid oid) {
	if (!OidIsValid(oid)) {
		return false;
	}
	HeapTuple tuple = SearchSysCache1(cache_id, bioseq::pg_utils::pg_datum::from_oid(oid).raw());
	if (!HeapTupleIsValid(tuple)) {
		return false;
	}
	ReleaseSysCache(tuple);
	return true;
}

[[nodiscard]] static bool cached_stub_oids_are_valid(const BioseqStubOids& o) {
	const Oid func_oids[] = {
		o.mmsearch_oid,
		o.mmsearch_with_opts_oid,
		o.score_oid,
		o.dp_score_oid,
		o.query_len_oid,
		o.query_start_oid,
		o.query_end_oid,
		o.target_len_oid,
		o.target_start_oid,
		o.target_end_oid,
		o.num_matches_oid,
		o.block_length_oid,
		o.edit_oid,
		o.mapq_oid,
		o.chain_score_oid,
		o.dp_max_score_oid,
		o.seed_count_oid,
		o.secondary_chain_score_oid,
		o.is_reverse_oid,
		o.is_primary_oid,
		o.has_cigar_oid,
		o.divergence_oid,
		o.cigar_oid,
		o.symbol_count_oid,
		o.seq_concat_oid,
		o.seq_concat_text_oid,
		o.text_concat_seq_oid,
		o.seq_concat_varchar_oid,
		o.varchar_concat_seq_oid,
		o.seq_concat_text_array_oid,
		o.text_array_concat_seq_oid,
		o.seq_concat_letter_array_oid,
		o.letter_array_concat_seq_oid,
		o.seq_reverse_oid,
		o.seq_reverse_complement_oid,
		o.seq_array_cat_oid,
		o.seq_array_length_oid,
		o.seq_cardinality_oid,
		o.seq_to_text_oid,
		o.seq_to_varchar_oid,
		o.seq_to_array_oid,
		o.seq_from_letter_array_infer_oid,
		o.seq_from_letter_array_named_oid,
		o.seq_len_oid,
	};
	for (const Oid oid : func_oids) {
		if (!oid_exists_in_syscache(PROCOID, oid)) {
			return false;
		}
	}
	return oid_exists_in_syscache(TYPEOID, o.bioseq_seq_type_oid)
		&& oid_exists_in_syscache(
			TYPEOID, o.bioseq_aligner_options_type_oid);
}

[[nodiscard]] static const BioseqStubOids& get_cached_stub_oids() {
	if (g_cached_stub_oids.initialised
		&& cached_stub_oids_are_valid(g_cached_stub_oids.oids)) {
		return g_cached_stub_oids.oids;
	}
	static bool invalidation_registered = false;
	if (!invalidation_registered) {
		CacheRegisterRelcacheCallback(invalidate_cached_stub_oids,
									  PointerGetDatum(NULL));
		CacheRegisterSyscacheCallback(PROCOID,
									  invalidate_cached_stub_oids_syscache,
									  PointerGetDatum(NULL));
		CacheRegisterSyscacheCallback(TYPEOID,
									  invalidate_cached_stub_oids_syscache,
									  PointerGetDatum(NULL));
		invalidation_registered = true;
	}
	g_cached_stub_oids.oids        = LookupStubOids();
	g_cached_stub_oids.initialised = true;
	return g_cached_stub_oids.oids;
}

/* Planner hooks use this entrypoint while CREATE/DROP EXTENSION can expose a
 * partial catalog surface. A partial probe neither calls the throwing lookup
 * nor marks the cache initialized. */
[[nodiscard]] static const BioseqStubOids*
try_get_cached_stub_oids() {
	if (g_cached_stub_oids.initialised
		&& cached_stub_oids_are_valid(g_cached_stub_oids.oids)) {
		return &g_cached_stub_oids.oids;
	}
	g_cached_stub_oids.initialised = false;
	if (!bioseq_stub_oid_surface_ready()) return nullptr;
	return &get_cached_stub_oids();
}

/*
 * Classifier used by planner ownership and BeginCustomScan discovery. Returns
 * true if funcid
 * matches any of the bioseq projection stubs (i.e. anything that
 * VarInjectorMutator will rewrite into a placeholder Const).
 */
/*
 * Accepts either the 2-arg mmsearch(seq, seq) or the 3-arg
 * mmsearch(seq, seq, bioseq_aligner_options). Call sites care about
 * "is this a match qual" rather than the arity, so funnelling both
 * overloads here avoids scattering the same disjunction everywhere.
 */
[[nodiscard]] static inline bool is_mmsearch_funcid(Oid funcid, const BioseqStubOids& o) {
	return funcid == o.mmsearch_oid || funcid == o.mmsearch_with_opts_oid;
}

struct MmsearchQualifierWalker {
	const BioseqStubOids* oids = nullptr;
};

struct mmsearch_qualifier_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, MmsearchQualifierWalker& data) const {
		if (FuncExpr* fe = node.try_as<FuncExpr>()) {
			if (is_mmsearch_funcid(fe->funcid, *data.oids)) {
				return pg_walk_step::stop;
			}
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static bool
contains_mmsearch_qualifier(Node* node, const BioseqStubOids& oids)
{
	MmsearchQualifierWalker data{ &oids };
	mmsearch_qualifier_visitor visitor;
	return pg_walk_expression_including_root(node, data, visitor);
}

struct MmsearchOccurrenceCounter {
	const BioseqStubOids* oids = nullptr;
	std::size_t count = 0;
};

struct mmsearch_occurrence_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node,
			   MmsearchOccurrenceCounter& data) const {
		if (FuncExpr* fe = node.try_as<FuncExpr>();
			fe != nullptr && is_mmsearch_funcid(fe->funcid, *data.oids)) {
			++data.count;
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static std::size_t
count_mmsearch_occurrences(Node* node, const BioseqStubOids& oids)
{
	MmsearchOccurrenceCounter data{ &oids, 0 };
	mmsearch_occurrence_visitor visitor;
	pg_walk_expression_including_root(node, data, visitor);
	return data.count;
}

struct OwnedMmsearchRestriction {
	RestrictInfo* restriction = nullptr;
	FuncExpr* normalized_expr = nullptr;
};

/*
 * Choose the one RestrictInfo the custom scan is allowed to execute.  The
 * decision is deliberately made against the original planner nodes: their
 * pointer identity is the proof later used to remove only this restriction
 * from executable quals and to prune competing paths that would execute it.
 */
[[nodiscard]] static OwnedMmsearchRestriction
choose_owned_mmsearch_restriction(PlannerInfo* root, RelOptInfo* rel,
								 RangeTblEntry* rte, List* restrictions,
								 Relids param_relids,
								 const BioseqStubOids& oids)
{
	std::size_t semantic_calls = 0;
	RestrictInfo* semantic_restriction = nullptr;
	for (RestrictInfo* rinfo : PList<RestrictInfo>(restrictions)) {
		const std::size_t in_clause = count_mmsearch_occurrences(
			reinterpret_cast<Node*>(rinfo->clause), oids);
		semantic_calls += in_clause;
		if (in_clause != 0) {
			semantic_restriction = rinfo;
		}
	}
	if (semantic_calls > 1) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("stacked mmsearch qualifiers are unsupported"),
			 errdetail("A Bioseq scan can own exactly one semantic mmsearch call."),
			 errhint("Split the alignment predicates into separate query levels.")));
	}
	if (semantic_calls == 0 || semantic_restriction == nullptr) {
		return {};
	}

	/* Nested Boolean/CASE/NOT shapes are not equivalent to a consumed, strict
	 * top-level predicate and therefore remain executable SQL quals. */
	FuncExpr* original = pg_node_try_as<FuncExpr>(
		reinterpret_cast<Node*>(semantic_restriction->clause));
	if (original == nullptr || !is_mmsearch_funcid(original->funcid, oids)) {
		return {};
	}
	const int arity = list_length(original->args);
	if ((original->funcid == oids.mmsearch_oid && arity != 2)
		|| (original->funcid == oids.mmsearch_with_opts_oid && arity != 3)) {
		return {};
	}

	/* Fail closed under row-level security, security-barrier quals, leaky
	 * ordering, pseudoconstants, or volatile siblings. */
	if (root == nullptr || root->parse == nullptr || root->parse->hasRowSecurity
		|| rte == nullptr || rte->securityQuals != NIL
		|| semantic_restriction->pseudoconstant
		|| semantic_restriction->security_level != 0
		|| !restriction_is_securely_promotable(semantic_restriction, rel)) {
		return {};
	}
	for (RestrictInfo* sibling : PList<RestrictInfo>(restrictions)) {
		if (sibling != semantic_restriction
			&& contain_volatile_functions(
				reinterpret_cast<Node*>(sibling->clause))) {
			return {};
		}
	}

	pg_first_two_node_args args = pg_first_two_args_if_arity_between(*original, 2, 3);
	if (!args.present()) {
		return {};
	}
	Var* target = pg_node_try_as<Var>(args.first);
	if (target == nullptr || target->varlevelsup != 0
		|| target->varno != rel->relid || target->varattno <= 0
		|| target->vartype != oids.bioseq_seq_type_oid
		|| target->vartypmod < 0) {
		return {};
	}

	Node* query = args.second;
	if (Const* constant = pg_node_try_as<Const>(query)) {
		if (constant->consttype != oids.bioseq_seq_type_oid
			|| constant->consttypmod < 0) {
			return {};
		}
	} else if (Param* param = pg_node_try_as<Param>(query)) {
		if (param->paramtype != oids.bioseq_seq_type_oid
			|| (param->paramkind != PARAM_EXEC
				&& param->paramkind != PARAM_EXTERN)
			|| (param->paramtypmod < 0 && param->paramkind != PARAM_EXTERN)) {
			return {};
		}
	} else if (Var* query_var = pg_node_try_as<Var>(query)) {
		const bool same_relation = query_var->varno == rel->relid;
		const bool available_outer = bms_is_member(query_var->varno, param_relids);
		if (query_var->varlevelsup != 0 || query_var->varattno <= 0
			|| query_var->vartype != oids.bioseq_seq_type_oid
			|| query_var->vartypmod < 0
			|| (!same_relation && !available_outer)) {
			return {};
		}
	} else {
		return {};
	}

	FuncExpr* normalized = copyObject(original);
	if (arity == 3) {
		Node* option = reinterpret_cast<Node*>(list_nth(normalized->args, 2));
		if (contain_volatile_functions(option)) {
			return {};
		}
		Node* folded = eval_const_expressions(root, option);
		Const* option_const = pg_node_try_as<Const>(folded);
		if (option_const == nullptr
			|| option_const->consttype != oids.bioseq_aligner_options_type_oid) {
			return {};
		}
		lfirst(list_nth_cell(normalized->args, 2)) = folded;
	}

	return { semantic_restriction, normalized };
}

[[nodiscard]] static List*
restrictions_without_owned_mmsearch(List* restrictions,
							 RestrictInfo* owned,
							 Expr* owned_clause,
							 bool restrictions_are_private_copies)
{
	PList<RestrictInfo> result;
	for (RestrictInfo* rinfo : PList<RestrictInfo>(restrictions)) {
		if (owned != nullptr
			&& (rinfo == owned
				|| (restrictions_are_private_copies
					&& equal(rinfo->clause, owned_clause)))) {
			continue;
		}
		result.append(rinfo);
	}
	return result.release();
}

/*
 * The owned mmsearch is removed before these storage quals are built. Keep a
 * defensive filter at the reader boundary so an unowned/nested rendezvous
 * expression can never be mistaken for a min/max pruning predicate.
 */
[[nodiscard]] static List*
storage_prunable_quals_without_mmsearch(List* quals, const BioseqStubOids& oids)
{
	PList<Node> filtered;
	for (auto qual : PList<Node>(quals)) {
		if (qual == nullptr) {
			continue;
		}
		if (contains_mmsearch_qualifier(qual, oids)) {
			continue;
		}
		filtered.append(qual);
	}
	return filtered.release();
}

[[nodiscard]] static inline bool is_bioseq_projection_funcid(Oid funcid, const BioseqStubOids& o) {
	return funcid == o.score_oid
		|| funcid == o.dp_score_oid
		|| funcid == o.query_len_oid
		|| funcid == o.query_start_oid
		|| funcid == o.query_end_oid
		|| funcid == o.target_len_oid
		|| funcid == o.target_start_oid
		|| funcid == o.target_end_oid
		|| funcid == o.num_matches_oid
		|| funcid == o.block_length_oid
		|| funcid == o.edit_oid
		|| funcid == o.mapq_oid
		|| funcid == o.chain_score_oid
		|| funcid == o.dp_max_score_oid
		|| funcid == o.seed_count_oid
		|| funcid == o.secondary_chain_score_oid
		|| funcid == o.is_reverse_oid
		|| funcid == o.is_primary_oid
		|| funcid == o.has_cigar_oid
		|| funcid == o.divergence_oid
		|| funcid == o.cigar_oid;
}

/* `anyelement` match helpers traditionally accept a target-owned row marker
 * such as the primary key.  When bioseq.edit is instead given a bioseq.seq
 * Var, that Var carries biological meaning and must be the exact sequence
 * column aligned by mmsearch; silently substituting another column's helper
 * with the shared MatchResult edit count would return a plausible wrong value. */
[[nodiscard]] static bool edit_sequence_argument_matches_target(
		const FuncExpr* function,
		Index target_relid,
		AttrNumber target_sequence_attno,
		const BioseqStubOids& oids) noexcept {
	if (function == nullptr || function->funcid != oids.edit_oid) return true;
	if (list_length(function->args) != 1) return false;
	Node* argument_node = reinterpret_cast<Node*>(linitial(function->args));
	if (argument_node == nullptr
		|| exprType(argument_node) != oids.bioseq_seq_type_oid) {
		/* Preserve the established target-owned row-marker helper form. */
		return true;
	}
	Var* argument = pg_node_try_as<Var>(argument_node);
	if (argument == nullptr) {
		/* A sequence-valued wrapper (COALESCE, cast, PHV expression, etc.)
		 * has biological identity that the shared MatchResult cannot safely
		 * infer.  Only the exact mmsearch target Var is supported. */
		return false;
	}
	return target_relid != 0
		&& target_sequence_attno > 0
		&& argument->varno == target_relid
		&& argument->varattno == target_sequence_attno
		&& argument->varlevelsup == 0
		&& bms_is_empty(argument->varnullingrels);
}

/*
 * Walk a target list for bioseq.* function calls and set the matching
 * feature-flags. We also recurse into arg subtrees so "bioseq.cigar(a) + 1"
 * is still counted. The shape used for detection matches the shape used by
 * VarInjectorMutator so the flags agree with which placeholders actually
 * populate at execution time.
 */
struct BioseqFeatureDetector {
	const BioseqStubOids* oids = nullptr;
	BioseqScanFeatureFlags flags;
};

struct feature_flag_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, BioseqFeatureDetector& detector) const {
		if (FuncExpr* fe = node.try_as<FuncExpr>()) {
			const BioseqStubOids* o = detector.oids;
			if (is_mmsearch_funcid(fe->funcid, *o)) {
				detector.flags.has_mmsearch = true;
			}
			if (fe->funcid == o->cigar_oid || fe->funcid == o->has_cigar_oid) {
				detector.flags.uses_cigar = true;
			}
			if (fe->funcid == o->edit_oid) {
				detector.flags.uses_edit = true;
			}
			if (fe->funcid == o->divergence_oid) {
				detector.flags.uses_divergence = true;
			}
			if (fe->funcid == o->mapq_oid) {
				detector.flags.uses_mapq = true;
			}
			if (is_bioseq_projection_funcid(fe->funcid, *o)) {
				detector.flags.any_match_projection = true;
			}
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static BioseqScanFeatureFlags
detect_feature_flags(List* targetlist, List* qual, const BioseqStubOids& oids)
{
	BioseqFeatureDetector detector{ &oids, BioseqScanFeatureFlags{} };
	feature_flag_visitor visitor;
	for (auto tgt : PList<TargetEntry>(targetlist)) {
		if (tgt == nullptr) {
			continue;
		}
		pg_walk_expression_including_root(pg_as_node(tgt->expr), detector, visitor);
	}
	for (auto q : PList<Node>(qual)) {
		if (q == nullptr) {
			continue;
		}
		pg_walk_expression_including_root(q, detector, visitor);
	}
	return detector.flags;
}

constexpr int kBioseqEditPredicateMagic = 0x42534531; /* BSE1 */
constexpr int kBioseqEditPredicateVersion = 1;

struct StrictEditPredicate {
	std::int64_t exclusive_upper_bound = 0;
	AttrNumber sequence_attno = InvalidAttrNumber;
};

struct EditFunctionProbe {
	const BioseqStubOids* oids = nullptr;
	bool found = false;
};

struct edit_function_probe_visitor {
	[[nodiscard]] pg_walk_step operator()(
		pg_expression_walk_cursor, pg_node_view node,
		EditFunctionProbe& probe) const {
		if (FuncExpr* function = node.try_as<FuncExpr>();
			function != nullptr && function->funcid == probe.oids->edit_oid) {
			probe.found = true;
			return pg_walk_step::stop;
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static bool contains_bioseq_edit(
		Node* expression, const BioseqStubOids& oids) {
	if (expression == nullptr) return false;
	EditFunctionProbe probe{&oids, false};
	edit_function_probe_visitor visitor;
	pg_walk_expression_including_root(expression, probe, visitor);
	return probe.found;
}

/* Supported SQL contract: one top-level `bioseq.edit(Var) < integer Const`.
 * The predicate is removed from the pre-alignment qual and enforced against
 * MatchResult::edit_distance while draining aligned output. */
[[nodiscard]] static Node* strict_edit_operand(
		Node* operand, Index scan_relid) noexcept {
	PlaceHolderVar* placeholder = pg_node_try_as<PlaceHolderVar>(operand);
	if (placeholder == nullptr) return operand;
	/* bioseq_placeholder_support introduces this exact wrapper for aggregate
	 * and join plans.  Accept only a scan-owned, non-nulled PHV; unwrapping a
	 * generic or outer-join-nullable PHV would discard planner semantics. */
	if (placeholder->phid == 0 || placeholder->phlevelsup != 0
		|| !bms_is_empty(placeholder->phnullingrels)
		|| bms_num_members(placeholder->phrels) != 1
		|| !bms_is_member(static_cast<int>(scan_relid), placeholder->phrels)) {
		return nullptr;
	}
	return reinterpret_cast<Node*>(placeholder->phexpr);
}

[[nodiscard]] static std::optional<StrictEditPredicate>
decode_strict_edit_upper_bound(
		Node* expression, Index scan_relid, const BioseqStubOids& oids) {
	OpExpr* comparison = pg_node_try_as<OpExpr>(expression);
	if (comparison == nullptr || comparison->opresulttype != BOOLOID
		|| list_length(comparison->args) != 2) {
		return std::nullopt;
	}
	PList<Node> arguments(comparison->args);
	FuncExpr* edit = pg_node_try_as<FuncExpr>(
		strict_edit_operand(arguments.first(), scan_relid));
	Const* upper = pg_node_try_as<Const>(arguments.second());
	if (edit == nullptr || edit->funcid != oids.edit_oid
		|| edit->funcresulttype != INT8OID || list_length(edit->args) != 1
		|| upper == nullptr || upper->constisnull) {
		return std::nullopt;
	}
	/* Operator names are schema-overloadable.  A bare SQL literal such as 10
	 * is int4, so PostgreSQL resolves BIGINT < 10 to its built-in int84lt
	 * implementation rather than inserting an int8 cast.  Accept only the
	 * three exact built-in RHS integer widths and normalize their Const datum;
	 * an arbitrary operator merely named `<` must remain fail-closed. */
	const Oid opcode = get_opcode(comparison->opno);
	std::int64_t exclusive_upper_bound = 0;
	switch (upper->consttype) {
	case INT8OID:
		if (opcode != F_INT8LT) return std::nullopt;
		exclusive_upper_bound = DatumGetInt64(upper->constvalue);
		break;
	case INT4OID:
		if (opcode != F_INT84LT) return std::nullopt;
		exclusive_upper_bound = static_cast<std::int64_t>(
			DatumGetInt32(upper->constvalue));
		break;
	case INT2OID:
		if (opcode != F_INT82LT) return std::nullopt;
		exclusive_upper_bound = static_cast<std::int64_t>(
			DatumGetInt16(upper->constvalue));
		break;
	default:
		return std::nullopt;
	}
	Var* sequence = pg_node_try_as<Var>(
		reinterpret_cast<Node*>(linitial(edit->args)));
	if (sequence == nullptr || sequence->varno != scan_relid
		|| sequence->varattno <= 0 || sequence->varlevelsup != 0
		|| !bms_is_empty(sequence->varnullingrels)
		|| sequence->vartype != oids.bioseq_seq_type_oid) {
		return std::nullopt;
	}
	return StrictEditPredicate{
		.exclusive_upper_bound = exclusive_upper_bound,
		.sequence_attno = sequence->varattno,
	};
}

[[nodiscard]] static List* quals_without_bioseq_edit(
		List* quals, const BioseqStubOids& oids) {
	PList<Node> filtered;
	for (Node* qual : PList<Node>(quals)) {
		if (qual != nullptr && !contains_bioseq_edit(qual, oids)) {
			filtered.append(qual);
		}
	}
	return filtered.release();
}

/* Planner baserestrictinfo is a List<RestrictInfo>, not an expression list.
 * Expression walkers deliberately do not know how to descend RestrictInfo
 * (PostgreSQL reports "unrecognized node type"), so unwrap only the exact
 * clause field and fail closed if a future planner shape violates that ABI. */
[[nodiscard]] static std::optional<BioseqScanFeatureFlags>
detect_restrictinfo_feature_flags(
		List* restrictions,
		const BioseqStubOids& oids) {
	BioseqFeatureDetector detector{&oids, BioseqScanFeatureFlags{}};
	feature_flag_visitor visitor;
	for (Node* node : PList<Node>(restrictions)) {
		RestrictInfo* restriction = pg_node_try_as<RestrictInfo>(node);
		if (restriction == nullptr || restriction->clause == nullptr) {
			return std::nullopt;
		}
		pg_walk_expression_including_root(
			reinterpret_cast<Node*>(restriction->clause), detector, visitor);
	}
	return detector.flags;
}

/*
 * DOC0279 — plan-time detection of an mmsearch typmod-coalesce (same alphabet,
 * different storage width between the bioseq.seq column and the query literal).
 *
 * BioseqColumnScan_ExplainCustomScan needs this because the "coalesce to
 * seq('<alpha>', <bits>)" hint used to be surfaced from the runtime buffer
 * (`coalesce_active()`), which is ONLY populated when BeginCustomScan ran —
 * i.e. NOT for `EXPLAIN` without `ANALYZE`. The coalesce decision is, however,
 * fully determined at plan time by the column Var's typmod vs the literal's
 * typmod, so we re-derive it from the plan qual here. Mirrors the BeginCustomScan
 * typmod logic (same `compact_sequence_stdc_type_params::decode`), reading no
 * runtime state. A mismatch we cannot classify simply yields `active=false`
 * (no hint, never a crash).
 */
struct ExplainTypmodCoalesce { bool active = false; uint8_t col_alpha = 0; uint8_t col_byte_width = 0; };
struct ExplainCoalesceWalkCtx { const BioseqStubOids* oids; ExplainTypmodCoalesce result; };

struct typmod_coalesce_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, ExplainCoalesceWalkCtx& ctx) const {
		if (!ctx.result.active) {
			if (FuncExpr* fe = node.try_as<FuncExpr>()) {
				if (is_mmsearch_funcid(fe->funcid, *ctx.oids) && list_length(fe->args) >= 2) {
					Node* arg0 = reinterpret_cast<Node*>(linitial(fe->args));   // bioseq.seq column
					Node* arg1 = reinterpret_cast<Node*>(lsecond(fe->args));    // query literal / param / var
					if (arg0 != nullptr && arg1 != nullptr && pg_node_view{arg0}.is<Var>()) {
						const int32 col_typmod = pg_node_expect<Var>(arg0)->vartypmod;
						int32 arg1_typmod = -1;
						if (Const* arg1_const = pg_node_try_as<Const>(arg1)) {
							arg1_typmod = arg1_const->consttypmod;
						} else if (Param* arg1_param = pg_node_try_as<Param>(arg1)) {
							arg1_typmod = arg1_param->paramtypmod;
						} else if (Var* arg1_var = pg_node_try_as<Var>(arg1)) {
							arg1_typmod = arg1_var->vartypmod;
						}
						if (col_typmod >= 0 && arg1_typmod >= 0) {
							const auto col = compact_sequence_stdc_type_params::decode(col_typmod);
							const auto qry = compact_sequence_stdc_type_params::decode(arg1_typmod);
							if (col.alphabet_code == qry.alphabet_code
								&& col.storage_unit_byte_width != qry.storage_unit_byte_width) {
								ctx.result.active = true;
								ctx.result.col_alpha = col.alphabet_code;
								ctx.result.col_byte_width = col.storage_unit_byte_width;
							}
						}
					}
				}
			}
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static ExplainTypmodCoalesce
explain_detect_typmod_coalesce(List* qual, const BioseqStubOids& oids)
{
	ExplainCoalesceWalkCtx ctx{ &oids, ExplainTypmodCoalesce{} };
	typmod_coalesce_visitor visitor;
	for (auto q : PList<Node>(qual)) {
		if (q != nullptr) {
			pg_walk_expression_including_root(q, ctx, visitor);
		}
	}
	return ctx.result;
}

/*
 * Snapshot the bioseq.aligner.* GUCs into a Minimap2Params struct.
 * A subsequent pass (parse_aligner_options_override) may overlay per-call
 * fields that the user supplied via aligner_options() in the mmsearch
 * FuncExpr. Keeping the two passes separate makes the precedence order
 * obvious: GUC → aligner_options() override → size-heuristic → pipeline.
 */
[[nodiscard]] static bioseq::simd::active_target::Minimap2Params
build_params_from_gucs()
{
	bioseq::simd::active_target::Minimap2Params params;
	params.kmer_size        = bioseq_aligner_kmer_size;
	params.window_size      = bioseq_aligner_window_size;
	params.max_gap          = bioseq_aligner_max_gap;
	params.max_gap_ref      = bioseq_aligner_max_gap_ref;
	params.bandwidth        = bioseq_aligner_bandwidth;
	params.max_chain_skip   = bioseq_aligner_max_chain_skip;
	params.max_chain_iter   = bioseq_aligner_max_chain_iter;
	params.min_seed_count   = bioseq_aligner_min_seed_count;
	params.min_chain_score  = bioseq_aligner_min_chain_score;
	params.chain_gap_scale  = static_cast<float>(bioseq_aligner_chain_gap_scale);
	params.chain_skip_scale = static_cast<float>(bioseq_aligner_chain_skip_scale);
	params.mask_level       = static_cast<float>(bioseq_aligner_mask_level);
	params.mask_len         = bioseq_aligner_mask_len;
	params.pri_ratio        = static_cast<float>(bioseq_aligner_pri_ratio);
	params.best_n           = bioseq_aligner_best_n;
	params.match_score      = bioseq_aligner_match_score;
	params.mismatch_pen     = bioseq_aligner_mismatch_pen;
	params.gap_open1        = bioseq_aligner_gap_open1;
	params.gap_ext1         = bioseq_aligner_gap_ext1;
	params.gap_open2        = bioseq_aligner_gap_open2;
	params.gap_ext2         = bioseq_aligner_gap_ext2;
	params.sc_ambi          = bioseq_aligner_sc_ambi;
	params.zdrop            = bioseq_aligner_zdrop;
	params.end_bonus        = bioseq_aligner_end_bonus;
	params.max_occ          = bioseq_aligner_max_occ;
	params.mid_occ          = bioseq_aligner_mid_occ;
	params.block_min_size   = static_cast<size_t>(bioseq_aligner_block_min_size);
	params.block_max_size   = static_cast<size_t>(bioseq_aligner_block_max_size);
	params.bandwidth_long   = bioseq_aligner_bandwidth_long;
	params.chaining_algorithm =
		static_cast<bioseq::simd::active_target::ChainingAlgorithmType>(
			bioseq_aligner_chaining_algorithm);
	params.base_alignment_algorithm =
		static_cast<bioseq::simd::active_target::BaseAlignmentAlgorithmType>(
			bioseq_aligner_base_alignment_algorithm);
	params.minimizer_algorithm =
		static_cast<bioseq::simd::active_target::MinimizerAlgorithmType>(
			bioseq_aligner_minimizer_algorithm);
	params.enable_profiling = bioseq_aligner_enable_profiling;
	params.vulkan_enabled = bioseq_aligner_vulkan_enabled;
	params.vulkan_seed_hits_enabled = bioseq_aligner_vulkan_seed_hits_enabled;
	params.vulkan_index_cache_enabled = bioseq_aligner_vulkan_index_cache_enabled;
	/* DOC0310 Phase C — resident two-submit frontend + GPU index build. */
	params.vulkan_fused_frontend_enabled = bioseq_aligner_vulkan_fused_frontend_enabled;
	params.vulkan_gpu_index_build_enabled = bioseq_aligner_vulkan_gpu_index_build_enabled;
	params.vulkan_extend_enabled = bioseq_aligner_vulkan_extend_enabled;
	params.vulkan_phase_c_tile_bp =
		static_cast<uint32_t>(bioseq_aligner_vulkan_phase_c_tile_bp);
	params.vulkan_phase_c_radix_bits =
		static_cast<uint32_t>(bioseq_aligner_vulkan_phase_c_radix_bits);
	return params;
}

[[nodiscard]] static Const*
require_aligner_options_const(FuncExpr* mmsearch_fe, const BioseqStubOids& oids)
{
	if (mmsearch_fe == nullptr) {
		return nullptr;
	}
	const int arity = list_length(mmsearch_fe->args);
	if (mmsearch_fe->funcid == oids.mmsearch_oid && arity == 2) {
		return nullptr;
	}
	if (arity != 3 || mmsearch_fe->funcid != oids.mmsearch_with_opts_oid) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("mmsearch aligner-options call has an invalid function shape"),
			 errdetail("arity=%d funcid=%u expected_funcid=%u",
				arity,
				mmsearch_fe->funcid,
				oids.mmsearch_with_opts_oid)));
	}
	PList<Node> args(mmsearch_fe->args);
	Node* opt_node = args.third();
	Const* opt_const = pg_node_try_as<Const>(opt_node);
	if (opt_const == nullptr) {
		const std::string_view node_tag_name =
			pg_node_tag_name(pg_node_tag_or_invalid(opt_node));
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("mmsearch aligner_options must be a plan-time constant"),
			 errdetail("third argument node tag is %.*s",
				static_cast<int>(node_tag_name.size()),
				node_tag_name.data()),
			 errhint("Pass aligner_options(...) directly; parameterized or column-valued option records are not supported.")));
	}
	if (opt_const->consttype != oids.bioseq_aligner_options_type_oid) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("mmsearch aligner_options constant has the wrong type"),
			 errdetail("consttype=%u expected=%u",
				opt_const->consttype,
				oids.bioseq_aligner_options_type_oid)));
	}
	return opt_const;
}

/*
 * If the third argument of a mmsearch FuncExpr is a row-typed Const
 * representing aligner_options(), decode its fields and overlay any
 * non-NULL values on top of `params`. NULL field values signal "inherit
 * the GUC" so they are skipped here.
 *
 * Field order must match the CREATE TYPE bioseq_aligner_options DDL in
 * bioseq--1.0.sql. Each field is kept INT4 (or FLOAT8) so the decoder
 * just pulls a Datum per non-null slot.
 */
[[nodiscard]] static bool
apply_aligner_options_override(FuncExpr* mmsearch_fe,
							   bioseq::simd::active_target::Minimap2Params& params,
							   bioseq::indexing::IndexReadRequirement& index_read_requirement,
							   const BioseqStubOids& oids,
							   bool* vulkan_enabled_explicit = nullptr)
{
	if (vulkan_enabled_explicit != nullptr) {
		*vulkan_enabled_explicit = false;
	}
	Const* opt_const = require_aligner_options_const(mmsearch_fe, oids);
	if (opt_const == nullptr) {
		return false;
	}
	if (opt_const->constisnull) {
		return false;
	}
	/*
	 * The composite comes through as a HeapTupleHeader varlena. Deconstruct
	 * it via heap_deform_tuple against the pg_type tupdesc.
	 */
	HeapTupleHeader header = reinterpret_cast<HeapTupleHeader>(
		PG_DETOAST_DATUM(opt_const->constvalue));
	Oid row_typeoid    = HeapTupleHeaderGetTypeId(header);
	int32 row_typmod   = HeapTupleHeaderGetTypMod(header);
	auto free_detoasted_header = [&] {
		if (reinterpret_cast<void*>(header)
			!= reinterpret_cast<void*>(DatumGetPointer(opt_const->constvalue))) {
			pfree(header);
		}
	};
	if (row_typeoid != oids.bioseq_aligner_options_type_oid) {
		const Oid actual_row_typeoid = row_typeoid;
		free_detoasted_header();
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("mmsearch aligner_options payload has the wrong row type"),
			 errdetail("header_typeoid=%u expected=%u",
				actual_row_typeoid,
				oids.bioseq_aligner_options_type_oid)));
	}
	TupleDesc tupdesc  = lookup_rowtype_tupdesc(row_typeoid, row_typmod);

	HeapTupleData heap_tuple{};
	heap_tuple.t_len  = HeapTupleHeaderGetDatumLength(header);
	heap_tuple.t_data = header;
	ItemPointerSetInvalid(&heap_tuple.t_self);
	heap_tuple.t_tableOid = InvalidOid;

	const int n_attrs = tupdesc->natts;
	/*
	 * Stack-sized buffers sized above the field count of the
	 * bioseq_aligner_options composite (36 fields including the appended
	 * index_read_requirement; 40 leaves headroom). Avoids the per-call heap
	 * allocation that std::vector<Datum> would add and keeps us clear of
	 * std::vector<bool>'s proxy storage (no .data()).
	 */
	constexpr int kExpectedOptionAttrs = 36;
	constexpr int kMaxOptionAttrs = 40;
	if (n_attrs != kExpectedOptionAttrs) {
		ReleaseTupleDesc(tupdesc);
		free_detoasted_header();
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq_aligner_options has %d fields, expected exactly %d",
					n_attrs, kExpectedOptionAttrs)));
	}
	struct ExpectedAttribute {
		const char* name;
		Oid type_oid;
	};
	static constexpr std::array<ExpectedAttribute, kExpectedOptionAttrs>
		expected_attributes{{
			{"kmer_size", INT4OID},
			{"window_size", INT4OID},
			{"max_gap", INT4OID},
			{"max_gap_ref", INT4OID},
			{"bandwidth", INT4OID},
			{"bandwidth_long", INT4OID},
			{"max_chain_skip", INT4OID},
			{"max_chain_iter", INT4OID},
			{"min_seed_count", INT4OID},
			{"min_chain_score", INT4OID},
			{"chain_gap_scale", FLOAT8OID},
			{"chain_skip_scale", FLOAT8OID},
			{"mask_level", FLOAT8OID},
			{"mask_len", INT4OID},
			{"pri_ratio", FLOAT8OID},
			{"best_n", INT4OID},
			{"match_score", INT4OID},
			{"mismatch_pen", INT4OID},
			{"gap_open1", INT4OID},
			{"gap_ext1", INT4OID},
			{"gap_open2", INT4OID},
			{"gap_ext2", INT4OID},
			{"sc_ambi", INT4OID},
			{"zdrop", INT4OID},
			{"end_bonus", INT4OID},
			{"max_occ", INT4OID},
			{"mid_occ", INT4OID},
			{"block_min_size", INT4OID},
			{"block_max_size", INT4OID},
			{"chaining_algorithm", INT4OID},
			{"base_alignment_algorithm", INT4OID},
			{"minimizer_algorithm", INT4OID},
			{"vulkan_enabled", BOOLOID},
			{"vulkan_seed_hits_enabled", BOOLOID},
			{"vulkan_index_cache_enabled", BOOLOID},
			{"index_read_requirement", INT4OID},
		}};
	for (int attr_index = 0; attr_index < kExpectedOptionAttrs; ++attr_index) {
		Form_pg_attribute attribute = TupleDescAttr(tupdesc, attr_index);
		const ExpectedAttribute& expected = expected_attributes[attr_index];
		if (attribute->attisdropped
			|| attribute->attnum != attr_index + 1
			|| attribute->atttypid != expected.type_oid
			|| std::strcmp(NameStr(attribute->attname), expected.name) != 0) {
			char actual_name[NAMEDATALEN]{};
			const std::size_t actual_name_size = std::min(
				std::strlen(NameStr(attribute->attname)),
				sizeof(actual_name) - 1);
			std::memcpy(
				actual_name,
				NameStr(attribute->attname),
				actual_name_size);
			const AttrNumber actual_attnum = attribute->attnum;
			const Oid actual_type_oid = attribute->atttypid;
			const bool actual_dropped = attribute->attisdropped;
			ReleaseTupleDesc(tupdesc);
			free_detoasted_header();
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq_aligner_options catalog layout is incompatible"),
				 errdetail("attribute %d: actual name=%s attnum=%d type=%u dropped=%s; expected name=%s attnum=%d type=%u",
					attr_index + 1,
					actual_name,
					static_cast<int>(actual_attnum),
					actual_type_oid,
					actual_dropped ? "true" : "false",
					expected.name,
					attr_index + 1,
					expected.type_oid)));
		}
	}
	std::array<Datum, kMaxOptionAttrs> values{};
	std::array<bool,  kMaxOptionAttrs> nulls{};
	heap_deform_tuple(&heap_tuple, tupdesc, values.data(), nulls.data());

	/*
	 * Positional decoder — the ordinal must match the CREATE TYPE field
	 * order in bioseq--1.0.sql. Keeping this logic here (rather than a
	 * generic name-lookup) means we pay zero catalog hits per-call, which
	 * matters because BeginCustomScan runs once per query invocation.
	 */
	auto get_int = [&](int idx, int32_t& out) {
		if (idx < n_attrs && !nulls[idx]) {
			out = DatumGetInt32(values[idx]);
		}
	};
	auto get_float = [&](int idx, float& out) {
		if (idx < n_attrs && !nulls[idx]) {
			out = DatumGetFloat8(values[idx]);
		}
	};
	int idx = 0;
	bool index_read_requirement_present = false;
	get_int  (idx++, params.kmer_size);
	get_int  (idx++, params.window_size);
	get_int  (idx++, params.max_gap);
	get_int  (idx++, params.max_gap_ref);
	get_int  (idx++, params.bandwidth);
	get_int  (idx++, params.bandwidth_long);
	get_int  (idx++, params.max_chain_skip);
	get_int  (idx++, params.max_chain_iter);
	get_int  (idx++, params.min_seed_count);
	get_int  (idx++, params.min_chain_score);
	get_float(idx++, params.chain_gap_scale);
	get_float(idx++, params.chain_skip_scale);
	get_float(idx++, params.mask_level);
	get_int  (idx++, params.mask_len);
	get_float(idx++, params.pri_ratio);
	get_int  (idx++, params.best_n);
	get_int  (idx++, params.match_score);
	get_int  (idx++, params.mismatch_pen);
	get_int  (idx++, params.gap_open1);
	get_int  (idx++, params.gap_ext1);
	get_int  (idx++, params.gap_open2);
	get_int  (idx++, params.gap_ext2);
	get_int  (idx++, params.sc_ambi);
	get_int  (idx++, params.zdrop);
	get_int  (idx++, params.end_bonus);
	get_int  (idx++, params.max_occ);
	get_int  (idx++, params.mid_occ);
	if (idx < n_attrs && !nulls[idx]) {
		params.block_min_size = static_cast<size_t>(DatumGetInt32(values[idx]));
	}
	++idx;
	if (idx < n_attrs && !nulls[idx]) {
		params.block_max_size = static_cast<size_t>(DatumGetInt32(values[idx]));
	}
	++idx;
	if (idx < n_attrs && !nulls[idx]) {
		/*
		 * Range-check before the enum cast — otherwise a crafted caller
		 * (aligner_options(chaining_algorithm => 42)) injects an invalid
		 * enum value into params.chaining_algorithm, which produces UB
		 * via the constexpr switch in to_chaining_params(). Known-good
		 * ordinals: 0=SCALAR_MM2, 1=SIMD_MM2, 2=RMQ, 3=VULKAN_TILED_DP.
		 */
		const int32_t raw = DatumGetInt32(values[idx]);
		if (raw >= 0 && raw <= 3) {
			params.chaining_algorithm =
				static_cast<bioseq::simd::active_target::ChainingAlgorithmType>(raw);
		} else {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("aligner_options.chaining_algorithm out of range: %d (expected 0..3)", raw)));
		}
	}
	++idx;
	if (idx < n_attrs && !nulls[idx]) {
		const int32_t raw = DatumGetInt32(values[idx]);
		if (raw >= 0 && raw <= 1) {
			params.base_alignment_algorithm =
				static_cast<bioseq::simd::active_target::BaseAlignmentAlgorithmType>(raw);
		} else {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("aligner_options.base_alignment_algorithm out of range: %d (expected 0..1)", raw)));
		}
	}
	++idx;
	/*
	 * DOC0307 Phase B — minimizer_algorithm. Known-good ordinals:
	 * 0=SCALAR, 1=SIMD_MINIMAP2_SLOW, 2=TWO_STACK_NTHASH,
	 * 3=VULKAN_MINIMAP_INT, 4=PAPER_NTHASH_SIMD.
	 */
	if (idx < n_attrs && !nulls[idx]) {
		const int32_t raw = DatumGetInt32(values[idx]);
		if (raw >= 0 && raw <= 4) {
			params.minimizer_algorithm =
				static_cast<bioseq::simd::active_target::MinimizerAlgorithmType>(raw);
		} else {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("aligner_options.minimizer_algorithm out of range: %d (expected 0..4)", raw)));
		}
	}
	++idx;
	/*
	 * vulkan_enabled (DOC0304 §7.4) — decoded as a boolean; a NULL leaves the
	 * GUC-derived default in place.
	 */
	if (idx < n_attrs && !nulls[idx]) {
		params.vulkan_enabled = DatumGetBool(values[idx]);
		if (vulkan_enabled_explicit != nullptr) {
			*vulkan_enabled_explicit = true;
		}
	}
	++idx;
	/* DOC0307 Phase B — vulkan_seed_hits_enabled. */
	if (idx < n_attrs && !nulls[idx]) {
		params.vulkan_seed_hits_enabled = DatumGetBool(values[idx]);
	}
	++idx;
	/* DOC0307 Phase B — vulkan_index_cache_enabled. */
	if (idx < n_attrs && !nulls[idx]) {
		params.vulkan_index_cache_enabled = DatumGetBool(values[idx]);
	}
	++idx;
	if (idx < n_attrs && !nulls[idx]) {
		const int32_t raw = DatumGetInt32(values[idx]);
		if (!bioseq::indexing::index_read_requirement_ordinal_valid(raw)) {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("aligner_options.index_read_requirement out of range: %d (expected 0..2)", raw)));
		}
		index_read_requirement =
			static_cast<bioseq::indexing::IndexReadRequirement>(raw);
		index_read_requirement_present = true;
	}
	ReleaseTupleDesc(tupdesc);
	/* Free the detoasted copy if PG_DETOAST_DATUM actually copied. */
	free_detoasted_header();
	return index_read_requirement_present;
}

/*
 * Resolve the SIMD chaining policy only after the final per-call override.
 * The C++ API deliberately defaults to exact_visited.  PostgreSQL promotes
 * the explicitly selected SIMD_MM2 route to the named bounded candidate so
 * integration and performance gates exercise the real production choice.
 * Every non-SIMD algorithm is reset to exact: a stale bounded value must not
 * leak through a copied parameter object, and no unsupported request may be
 * translated into a different biological algorithm.
 */
static void
finalize_simd_chaining_policy(
	bioseq::simd::active_target::Minimap2Params& params)
{
	using bioseq::simd::active_target::ChainingAlgorithmType;
	using bioseq::simd::active_target::SimdChainingPolicy;
	switch (params.chaining_algorithm) {
		case ChainingAlgorithmType::SIMD_MM2:
			params.simd_chaining_policy =
				SimdChainingPolicy::bounded_batch_approximate;
			return;
		case ChainingAlgorithmType::SCALAR_MM2:
		case ChainingAlgorithmType::RMQ:
		case ChainingAlgorithmType::VULKAN_TILED_DP:
			params.simd_chaining_policy = SimdChainingPolicy::exact_visited;
			return;
	}
	ereport(ERROR,
		(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		 errmsg("final SIMD chaining policy cannot be selected"),
		 errdetail("chaining_algorithm has unknown ordinal %u",
			 static_cast<unsigned>(params.chaining_algorithm)),
		 errhint("Select scalar_mm2, simd_mm2, rmq, or vulkan_tiled_dp explicitly; no fallback is performed.")));
}

/*
 * Context passed to VarInjectorMutator through the void* context arg.
 * Bundles the allocated placeholder Consts with the resolved OIDs so
 * the mutator can dispatch on funcid -> Const* without a global.
 *
 * `current_relation` is used exclusively when rewriting
 * `symbol_count(Var, 'L')` call sites to resolve the column name
 * (attname) referenced by the Var — so the EXPLAIN output can say
 * "Fetched from meta: seq.symbol_count('A') AS ...".
 */
struct VarInjectorCtx {
	BioseqPlaceholderConsts* consts;
	const BioseqStubOids*    oids;
	Relation                 current_relation;
	Index                    match_sequence_varno;
	AttrNumber               match_sequence_attno;
};

/* functions to cost paths in-place */
static void CostBioseqColumnPaths(PlannerInfo *root, RelOptInfo *rel, Oid relationId);
static void CostBioseqColumnIndexPath(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
								  IndexPath *indexPath);
static void CostBioseqColumnSeqPath(RelOptInfo *rel, Oid relationId, Path *path);
static void AdjustBioseqColumnParallelScanCost(Path *path);
static void CostBioseqColumnScan(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
							 CustomPath *cpath, int numberOfColumnsRead,
							 int nClauses);

/* functions to add new paths */
static void AddBioseqColumnScanPaths(PlannerInfo *root, RelOptInfo *rel,
								 RangeTblEntry *rte);
static Path * AddBioseqColumnScanPath(PlannerInfo *root, RelOptInfo *rel,
								  RangeTblEntry *rte, Relids required_relids);

/* helper functions to be used when costing paths or altering them */
static void RemovePathsByPredicate(RelOptInfo *rel, PathPredicate removePathPredicate);
static bool IsNotIndexPath(Path *path);
static Cost BioseqColumnIndexScanAdditionalCost(PlannerInfo *root, RelOptInfo *rel,
											Oid relationId, IndexPath *indexPath);
static int RelationIdGetNumberOfAttributes(Oid relationId);
static Cost BioseqColumnPerStripeScanCost(RelOptInfo *rel, Oid relationId,
									  int numberOfColumnsRead);
static uint64_t BioseqColumnTableStripeCount(Oid relationId);
static Path * CreateBioseqColumnSeqScanPath(PlannerInfo *root, RelOptInfo *rel,
										Oid relationId);
static void AddBioseqColumnScanPathsRec(PlannerInfo *root, RelOptInfo *rel,
									RangeTblEntry *rte, Relids paramRelids,
									Relids candidateRelids,
									int depthLimit);

/* hooks and callbacks */
static void BioseqColumnSetRelPathlistHook(PlannerInfo *root, RelOptInfo *rel, Index rti,
									   RangeTblEntry *rte);
static void BioseqColumnGetRelationInfoHook(PlannerInfo *root, Oid relationObjectId,
											bool inhparent, RelOptInfo *rel);
static void BioseqBatchJoinSetJoinPathlistHook(
	PlannerInfo* root, RelOptInfo* joinrel, RelOptInfo* outerrel,
	RelOptInfo* innerrel, JoinType jointype, JoinPathExtraData* extra);

static Plan * BioseqColumnScanPath_PlanCustomPath(PlannerInfo *root,
											  RelOptInfo *rel,
											  CustomPath *best_path,
											  List *tlist,
											  List *clauses,
											  List *custom_plans);
static List * BioseqColumnScanPath_ReparameterizeCustomPathByChild(PlannerInfo *root,
															   List *custom_private,
															   RelOptInfo *child_rel);
static Node * BioseqColumnScan_CreateCustomScanState(CustomScan *cscan);
static Plan* BioseqBatchJoin_PlanCustomPath(
	PlannerInfo* root, RelOptInfo* rel, CustomPath* best_path,
	List* tlist, List* clauses, List* custom_plans);
static Node* BioseqBatchJoin_CreateCustomScanState(CustomScan* cscan);
static Plan* BioseqPerRowJoin_PlanCustomPath(
	PlannerInfo* root, RelOptInfo* rel, CustomPath* best_path,
	List* tlist, List* clauses, List* custom_plans);

static void BioseqColumnScan_BeginCustomScan(CustomScanState *node, EState *estate,
										 int eflags);

static TupleTableSlot * CustomExecScan(BioseqScanState *node,
									   ExecScanAccessMtd accessMtd,
									   ExecScanRecheckMtd recheckMtd);
static TupleTableSlot * BioseqColumnScan_ExecCustomScan(CustomScanState *node);
static void BioseqColumnScan_EndCustomScan(CustomScanState *node);
static void BioseqColumnScan_ReScanCustomScan(CustomScanState *node);
static void BioseqColumnScan_ExplainCustomScan(CustomScanState *node, List *ancestors,
										   ExplainState *es);
static Size BioseqColumn_EstimateDSMCustomScan(CustomScanState *node,
										   ParallelContext *pcxt);
static void BioseqColumn_InitializeDSMCustomScan(CustomScanState *node,
											ParallelContext *pcxt,
											void *coordinate);
static void BioseqColumn_ReinitializeDSMCustomScan(CustomScanState *node,
											   ParallelContext *pcxt,
											   void *coordinate);
static void BioseqColumn_InitializeWorkerCustomScan(CustomScanState *node,
												shm_toc *toc,
											void *coordinate);
static void BioseqBatchJoin_BeginCustomScan(
	CustomScanState* node, EState* estate, int eflags);
static TupleTableSlot* BioseqBatchJoin_ExecCustomScan(CustomScanState* node);
static void BioseqBatchJoin_EndCustomScan(CustomScanState* node);
static void BioseqBatchJoin_ReScanCustomScan(CustomScanState* node);
static void BioseqBatchJoin_ExplainCustomScan(
	CustomScanState* node, List* ancestors, ExplainState* es);
static void BioseqPerRowJoin_BeginCustomScan(
	CustomScanState* node, EState* estate, int eflags);
static TupleTableSlot* BioseqPerRowJoin_ExecCustomScan(CustomScanState* node);
static void BioseqPerRowJoin_EndCustomScan(CustomScanState* node);
static void BioseqPerRowJoin_ReScanCustomScan(CustomScanState* node);
static void BioseqPerRowJoin_ExplainCustomScan(
	CustomScanState* node, List* ancestors, ExplainState* es);
static void ExplainPipelineExecutionStats(
	const bioseq::pipeline_stats::PipelineExecutionStats& stats,
	ExplainState* es);

/* helper functions to build strings for EXPLAIN */
static const char * BioseqColumnPushdownClausesStr(List *context, List *clauses);
static const char * BioseqColumnProjectedColumnsStr(List *context,
												List *projectedColumns);
static List * set_deparse_context_planstate(List *dpcontext, Node *node,
											List *ancestors);

/* other helpers */
static List * BioseqColumnVarNeeded(BioseqScanState *bioseqScanState);

/* saved hook value in case of unload */
static set_rel_pathlist_hook_type PreviousSetRelPathlistHook = NULL;
static set_join_pathlist_hook_type PreviousSetJoinPathlistHook = NULL;
static get_relation_info_hook_type PreviousGetRelationInfoHook = NULL;

static bool EnableBioseqColumnCustomScan = true;
static bool EnableBioseqColumnQualPushdown = true;
static double BioseqColumnQualPushdownCorrelationThreshold = 0.4;
static int BioseqColumnMaxCustomScanPaths = 64;
static int BioseqColumnPlannerDebugLevel = DEBUG3;

#ifdef BIOSEQ_TEST_INDEXES
enum SliceProjectDsmTestFixture : int {
	kSliceDsmTestNone = 0,
	kSliceDsmTestIncomplete = 1,
	kSliceDsmTestMalformed = 2,
};

static int BioseqSliceDsmTestFixture = kSliceDsmTestNone;

static const config_enum_entry slice_dsm_test_fixture_options[] = {
	{"none", kSliceDsmTestNone, false},
	{"incomplete", kSliceDsmTestIncomplete, false},
	{"malformed", kSliceDsmTestMalformed, false},
	{nullptr, 0, false},
};
#endif


const CustomPathMethods BioseqColumnScanPathMethods = {
	.CustomName = "BioseqScan",
	.PlanCustomPath = BioseqColumnScanPath_PlanCustomPath,
	.ReparameterizeCustomPathByChild = BioseqColumnScanPath_ReparameterizeCustomPathByChild,
};

const CustomScanMethods BioseqColumnScanScanMethods = {
	.CustomName = "BioseqScan",
	.CreateCustomScanState = BioseqColumnScan_CreateCustomScanState,
};

const CustomPathMethods BioseqBatchJoinPathMethods = {
	.CustomName = "BioseqBatchJoin",
	.PlanCustomPath = BioseqBatchJoin_PlanCustomPath,
};

const CustomScanMethods BioseqBatchJoinScanMethods = {
	.CustomName = "BioseqBatchJoin",
	.CreateCustomScanState = BioseqBatchJoin_CreateCustomScanState,
};

const CustomExecMethods BioseqBatchJoinExecuteMethods = {
	.CustomName = "BioseqBatchJoin",
	.BeginCustomScan = BioseqBatchJoin_BeginCustomScan,
	.ExecCustomScan = BioseqBatchJoin_ExecCustomScan,
	.EndCustomScan = BioseqBatchJoin_EndCustomScan,
	.ReScanCustomScan = BioseqBatchJoin_ReScanCustomScan,
	.ExplainCustomScan = BioseqBatchJoin_ExplainCustomScan,
};

const CustomPathMethods BioseqPerRowJoinPathMethods = {
	.CustomName = "BioseqBatchJoin",
	.PlanCustomPath = BioseqPerRowJoin_PlanCustomPath,
};

const CustomExecMethods BioseqColumnScanExecuteMethods = {
	.CustomName = "BioseqScan",

	.BeginCustomScan = BioseqColumnScan_BeginCustomScan,
	.ExecCustomScan = BioseqColumnScan_ExecCustomScan,
	.EndCustomScan = BioseqColumnScan_EndCustomScan,
	.ReScanCustomScan = BioseqColumnScan_ReScanCustomScan,

	.EstimateDSMCustomScan = BioseqColumn_EstimateDSMCustomScan,
	.InitializeDSMCustomScan = BioseqColumn_InitializeDSMCustomScan,
	.ReInitializeDSMCustomScan = BioseqColumn_ReinitializeDSMCustomScan,
	.InitializeWorkerCustomScan = BioseqColumn_InitializeWorkerCustomScan,

	.ExplainCustomScan = BioseqColumnScan_ExplainCustomScan
};

static const config_enum_entry debug_level_options[] = {
	{ "debug5", DEBUG5, false },
	{ "debug4", DEBUG4, false },
	{ "debug3", DEBUG3, false },
	{ "debug2", DEBUG2, false },
	{ "debug1", DEBUG1, false },
	{ "debug", DEBUG2, true },
	{ "info", INFO, false },
	{ "notice", NOTICE, false },
	{ "warning", WARNING, false },
	{ "log", LOG, false },
	{ NULL, 0, false }
};


export const CustomScanMethods *
bioseq_customscan_methods(void)
{
	return &BioseqColumnScanScanMethods;
}

/*
 * Walker context for bioseq_node_has_projection_or_mmsearch.
 */
struct BioseqProjectionPresenceProbe {
	const BioseqStubOids* stub_oids;
	bool found;
};

struct projection_presence_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, BioseqProjectionPresenceProbe& data) const {
		if (data.found) return pg_walk_step::stop;
		if (FuncExpr* fe = node.try_as<FuncExpr>()) {
			if (is_bioseq_projection_funcid(fe->funcid, *data.stub_oids)
				|| is_mmsearch_funcid(fe->funcid, *data.stub_oids)
				|| fe->funcid == data.stub_oids->symbol_count_oid) {
				data.found = true;
				return pg_walk_step::stop;
			}
		}
		return pg_walk_step::descend;
	}
};

/*
 * True if any node in the expression tree references a bioseq.<...>
 * projection FuncExpr (cigar / score / target_*, or symbol_count) or
 * an mmsearch FuncExpr. The vectorised aggregator rewrite path uses
 * this to skip its rewrite when the agg input depends on any of
 * these — the projection FuncExprs are scan-time-rewritten into
 * placeholder Consts that the regular Postgres Aggregate node can
 * evaluate, but the vectorised aggregator expects vector-typed
 * scan output and crashes with a SIGSEGV when fed a regular slot
 * carrying our placeholder Consts.
 */
export bool
bioseq_expr_uses_bioseq_projection(Node* node)
{
	if (node == nullptr) {
		return false;
	}
	const BioseqStubOids* stub_oids = try_get_cached_stub_oids();
	if (stub_oids == nullptr) return false;
	BioseqProjectionPresenceProbe probe{ stub_oids, false };
	projection_presence_visitor visitor;
	pg_walk_expression_including_root(node, probe, visitor);
	return probe.found;
}

/* ========================================================================
 * DOC0377 BioseqSliceProject
 *
 * The executable expression remains PostgreSQL's targetlist expression.  The
 * codec below is deliberately symbolic: it records the lazy sequence algebra
 * for EXPLAIN and validation while core ExprState/ProjectionInfo preserves
 * CASE, NULL, STRICT, volatility and error semantics.  Every serialized value
 * is a PostgreSQL Node, so copy/equal/out/read and setrefs remain core-owned.
 * ====================================================================== */

enum class SliceProjectOpcode : std::uint8_t {
	source_expr = 1,
	literal_expr = 2,
	element = 3,
	slice = 4,
	concat = 5,
	reverse = 6,
	reverse_complement = 7,
	length = 8,
	cardinality = 9,
	bridge = 10,
	materialize = 11,
};

constexpr int kBioseqSliceProjectMagic = 0x42535031; /* BSP1 */
constexpr int kBioseqSliceProjectCodecVersion = 3;
constexpr int kBioseqSliceProjectHeaderFields = 10;
constexpr int kBioseqSliceProjectNodeFields = 11;
constexpr int kBioseqSliceProjectRecordFields = 9;

enum SliceProjectHeaderIndex : int {
	kSliceHeaderMagic = 0,
	kSliceHeaderVersion = 1,
	kSliceHeaderBoundaryCount = 2,
	kSliceHeaderExpressionCount = 3,
	kSliceHeaderPieceBoundaries = 4,
	kSliceHeaderSourcePieces = 5,
	kSliceHeaderPlannedSymbols = 6,
	kSliceHeaderPlannedBytes = 7,
	kSliceHeaderProducerCount = 8,
	kSliceHeaderFallbackReason = 9,
};

enum SliceProjectRecordIndex : int {
	kSliceRecordBoundaryId = 0,
	kSliceRecordProducerId = 1,
	kSliceRecordExpressionIndex = 2,
	kSliceRecordDependencies = 3,
	kSliceRecordMechanism = 4,
	kSliceRecordReason = 5,
	kSliceRecordEstimatedSymbols = 6,
	kSliceRecordEncoding = 7,
	kSliceRecordCacheScope = 8,
};

enum SliceProjectNodeIndex : int {
	kSliceNodeStableId = 0,
	kSliceNodeOpcode = 1,
	kSliceNodeDependencies = 2,
	kSliceNodeExpressionIndex = 3,
	kSliceNodeLowerExpressionIndex = 4,
	kSliceNodeUpperExpressionIndex = 5,
	kSliceNodeOptionsExpressionIndex = 6,
	kSliceNodeTransform = 7,
	kSliceNodeMechanism = 8,
	kSliceNodeReason = 9,
	kSliceNodeFlags = 10,
};

enum SliceProjectNodeFlags : int {
	kSliceNodeVolatile = 1 << 0,
	kSliceNodeOpaque = 1 << 1,
};

[[nodiscard]] static Const* slice_project_int8_const(std::uint64_t value) {
	const std::uint64_t clamped = std::min<std::uint64_t>(
		value, static_cast<std::uint64_t>(INT64_MAX));
	return makeConst(INT8OID, -1, InvalidOid, sizeof(std::int64_t),
		Int64GetDatum(static_cast<std::int64_t>(clamped)), false, true);
}

[[nodiscard]] static std::uint64_t slice_project_int8_value(Node* node) {
	Const* value = pg_node_try_as<Const>(node);
	if (value == nullptr || value->constisnull || value->consttype != INT8OID) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject uint64 codec value is invalid")));
	}
	const std::int64_t decoded = DatumGetInt64(value->constvalue);
	if (decoded < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject uint64 codec value is negative")));
	}
	return static_cast<std::uint64_t>(decoded);
}

[[nodiscard]] static const char* slice_project_mechanism_name(
		materialization_mechanism mechanism) noexcept {
	switch (mechanism) {
		case materialization_mechanism::backing_copy: return "backing copy";
		case materialization_mechanism::packed_full: return "packed full";
		case materialization_mechanism::packed_region: return "packed region";
		case materialization_mechanism::text: return "text";
		case materialization_mechanism::binary: return "binary";
		case materialization_mechanism::letter_array: return "letter array";
	}
	return "invalid";
}

[[nodiscard]] static const char* slice_project_reason_name(
		materialization_reason reason) noexcept {
	switch (reason) {
		case materialization_reason::executor_copy: return "executor copy";
		case materialization_reason::heap_tuple: return "heap tuple";
		case materialization_reason::index_tuple: return "index tuple";
		case materialization_reason::mmsearch_target: return "mmsearch target";
		case materialization_reason::mmsearch_query: return "mmsearch query";
		case materialization_reason::resident_algorithm:
			return "resident algorithm";
		case materialization_reason::sql_conversion: return "SQL conversion";
		case materialization_reason::client_output: return "client output";
	}
	return "invalid";
}

[[nodiscard]] static const char* slice_project_encoding_name(
		materialization_mechanism mechanism) noexcept {
	switch (mechanism) {
		case materialization_mechanism::packed_full:
		case materialization_mechanism::packed_region:
			return "alphabet packed";
		case materialization_mechanism::text:
			return "database encoding text";
		case materialization_mechanism::binary:
			return "bioseq binary";
		case materialization_mechanism::letter_array:
			return "bioseq.letter[]";
		case materialization_mechanism::backing_copy:
			return "source encoding";
	}
	return "unknown";
}

[[nodiscard]] static bool slice_project_concat_oid(
		Oid oid, const BioseqStubOids& oids) noexcept {
	return oid == oids.seq_concat_oid
		|| oid == oids.seq_array_cat_oid
		|| oid == oids.seq_concat_text_oid
		|| oid == oids.text_concat_seq_oid
		|| oid == oids.seq_concat_varchar_oid
		|| oid == oids.varchar_concat_seq_oid
		|| oid == oids.seq_concat_text_array_oid
		|| oid == oids.text_array_concat_seq_oid
		|| oid == oids.seq_concat_letter_array_oid
		|| oid == oids.letter_array_concat_seq_oid;
}

[[nodiscard]] static std::optional<SliceProjectOpcode>
slice_project_function_opcode(FuncExpr* function,
		const BioseqStubOids& oids) noexcept {
	if (function == nullptr) return std::nullopt;
	if (slice_project_concat_oid(function->funcid, oids)) {
		return SliceProjectOpcode::concat;
	}
	if (function->funcid == oids.seq_reverse_oid) {
		return SliceProjectOpcode::reverse;
	}
	if (function->funcid == oids.seq_reverse_complement_oid) {
		return SliceProjectOpcode::reverse_complement;
	}
	if (function->funcid == oids.seq_len_oid
		|| function->funcid == oids.seq_array_length_oid) {
		return SliceProjectOpcode::length;
	}
	if (function->funcid == oids.seq_cardinality_oid) {
		return SliceProjectOpcode::cardinality;
	}
	if (function->funcid == oids.seq_to_text_oid
		|| function->funcid == oids.seq_to_varchar_oid
		|| function->funcid == oids.seq_to_array_oid) {
		return SliceProjectOpcode::materialize;
	}
	if (function->funcid == oids.seq_from_letter_array_infer_oid
		|| function->funcid == oids.seq_from_letter_array_named_oid) {
		return SliceProjectOpcode::bridge;
	}
	return std::nullopt;
}

[[nodiscard]] static std::optional<SliceProjectOpcode>
slice_project_node_opcode(Node* node, const BioseqStubOids& oids) noexcept {
	if (node == nullptr) return std::nullopt;
	if (IsA(node, SubscriptingRef)) {
		auto* subscript = reinterpret_cast<SubscriptingRef*>(node);
		if (subscript->refcontainertype != oids.bioseq_seq_type_oid) {
			return std::nullopt;
		}
		return subscript->reflowerindexpr == NIL
			? SliceProjectOpcode::element : SliceProjectOpcode::slice;
	}
	if (FuncExpr* function = pg_node_try_as<FuncExpr>(node)) {
		return slice_project_function_opcode(function, oids);
	}
	return std::nullopt;
}

[[nodiscard]] static std::uint64_t slice_project_estimated_symbols(
		Node* node, const BioseqStubOids& oids) {
	if (node == nullptr) return 0u;
	if (IsA(node, SubscriptingRef)) {
		auto* subscript = reinterpret_cast<SubscriptingRef*>(node);
		if (subscript->reflowerindexpr == NIL) return 1u;
		Node* lower = subscript->reflowerindexpr == NIL
			? nullptr : reinterpret_cast<Node*>(linitial(subscript->reflowerindexpr));
		Node* upper = subscript->refupperindexpr == NIL
			? nullptr : reinterpret_cast<Node*>(linitial(subscript->refupperindexpr));
		auto constant_bound = [](Node* expression)
				-> std::optional<std::int64_t> {
			Const* constant = pg_node_try_as<Const>(expression);
			if (constant == nullptr || constant->constisnull
				|| constant->consttype != INT8OID) return std::nullopt;
			return DatumGetInt64(constant->constvalue);
		};
		const auto lo = lower == nullptr
			? std::optional<std::int64_t>{0} : constant_bound(lower);
		const auto hi = constant_bound(upper);
		if (lo.has_value() && hi.has_value() && *lo >= 0 && *hi >= *lo) {
			return static_cast<std::uint64_t>(*hi - *lo);
		}
		return 4096u;
	}
	if (FuncExpr* function = pg_node_try_as<FuncExpr>(node)) {
		const auto opcode = slice_project_function_opcode(function, oids);
		if (!opcode.has_value()) return 4096u;
		if (*opcode == SliceProjectOpcode::concat) {
			std::uint64_t total = 0u;
			for (Node* argument : PList<Node>(function->args)) {
				total = bioseq::parallelism::saturating_add(
					total, slice_project_estimated_symbols(argument, oids));
			}
			return total;
		}
		if (*opcode == SliceProjectOpcode::length
			|| *opcode == SliceProjectOpcode::cardinality) return 1u;
		if (function->args != NIL) {
			return slice_project_estimated_symbols(
				reinterpret_cast<Node*>(linitial(function->args)), oids);
		}
	}
	return 4096u;
}

struct owned_mmsearch_cost {
	Cost startup = 0;
	Cost per_input_tuple = 0;
};

/* Cost only executable operands, in SQL argument order.  In particular the
 * rendezvous FuncExpr itself (whose support function deliberately costs
 * 100000) must never enter an owning path's cost tree. */
[[nodiscard]] static List* mmsearch_operand_cost_exprs(
		FuncExpr const* expression, BioseqStubOids const& oids) {
	if (expression == nullptr || !is_mmsearch_funcid(expression->funcid, oids)
		|| (expression->funcid == oids.mmsearch_oid
			&& list_length(expression->args) != 2)
		|| (expression->funcid == oids.mmsearch_with_opts_oid
			&& list_length(expression->args) != 3)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("owned mmsearch operand cost shape is invalid")));
	}
	List* operands = NIL;
	for (Node* argument : PList<Node>(expression->args)) {
		operands = lappend(operands, argument);
	}
	return operands;
}

[[nodiscard]] static std::uint64_t estimate_mmsearch_operand_symbols(
		Node* expression, BioseqStubOids const& oids) {
	if (expression == nullptr) return 4096u;
	if (Const* constant = pg_node_try_as<Const>(expression)) {
		if (constant->constisnull) return 0u;
		if (constant->consttype == oids.bioseq_seq_type_oid) {
			return classify_and_describe_sequence_header(
				constant->constvalue).bp_length;
		}
	}
	const auto opcode = slice_project_node_opcode(expression, oids);
	if (opcode.has_value()) {
		return slice_project_estimated_symbols(expression, oids);
	}
	return 4096u;
}

[[nodiscard]] static owned_mmsearch_cost estimate_owned_mmsearch_cost(
		std::uint64_t target_length_estimate,
		std::uint64_t query_length_estimate,
		double input_rows,
		bool index_read_requirement,
		std::uint32_t planned_batch_size,
		std::uint64_t planned_materialized_bytes) noexcept {
	constexpr Cost maximum = std::numeric_limits<Cost>::max() / 32.0;
	auto finite_cost = [](long double value) noexcept -> Cost {
		constexpr long double limit = static_cast<long double>(
			std::numeric_limits<Cost>::max() / 32.0);
		if (!(value >= 0.0L)) return 0;
		if (value >= limit) return static_cast<Cost>(limit);
		return static_cast<Cost>(value);
	};
	const std::uint32_t batch_size = std::max<std::uint32_t>(
		1u, planned_batch_size);
	const long double target_units = static_cast<long double>(
		target_length_estimate / 64u
		+ (target_length_estimate % 64u != 0u));
	const long double query_units = static_cast<long double>(
		query_length_estimate / 64u
		+ (query_length_estimate % 64u != 0u));
	const long double byte_pages = static_cast<long double>(
		planned_materialized_bytes / BLCKSZ)
		+ (planned_materialized_bytes % BLCKSZ != 0u);
	const long double rows = std::isfinite(input_rows) && input_rows > 0.0
		? static_cast<long double>(input_rows) : 0.0L;
	const long double batch_count = rows == 0.0L ? 1.0L
		: std::ceil(rows / static_cast<long double>(batch_size));
	const long double startup = static_cast<long double>(cpu_operator_cost)
		* (8.0L + query_units + batch_count)
		+ (index_read_requirement
			? static_cast<long double>(random_page_cost) : 0.0L);
	const long double per_input = static_cast<long double>(cpu_tuple_cost)
		+ static_cast<long double>(cpu_operator_cost)
			* (8.0L + target_units + query_units)
		+ static_cast<long double>(seq_page_cost) * byte_pages
			/ static_cast<long double>(batch_size);
	owned_mmsearch_cost result{
		finite_cost(startup), finite_cost(per_input)};
	if (!std::isfinite(result.startup) || result.startup < 0) {
		result.startup = maximum;
	}
	if (!std::isfinite(result.per_input_tuple)
		|| result.per_input_tuple < 0) {
		result.per_input_tuple = maximum;
	}
	return result;
}

struct SliceProjectIrBuilder {
	const BioseqStubOids* oids = nullptr;
	CmdType command_type = CMD_UNKNOWN;
	List* expressions = NIL;
	List* nodes = NIL;
	List* records = NIL;
	List* output_bindings = NIL;
	std::vector<Node*> producer_expressions{};
	std::vector<std::vector<std::uint32_t>> producer_dependencies{};
	std::vector<std::pair<Node*, std::uint32_t>> source_bindings{};
	bool source_bindings_only = false;
	std::uint32_t next_boundary_id = 1u;
	std::uint64_t piece_boundaries = 0u;
	std::uint64_t source_pieces = 0u;
	std::uint64_t planned_symbols = 0u;
	std::uint64_t planned_bytes = 0u;
	bool found = false;
};

[[nodiscard]] static int slice_project_add_expression(
		SliceProjectIrBuilder& builder, Node* expression) {
	int index = 0;
	for (Node* candidate : PList<Node>(builder.expressions)) {
		if (equal(candidate, expression)) return index;
		++index;
	}
	index = list_length(builder.expressions);
	builder.expressions = lappend(builder.expressions, copyObject(expression));
	return index;
}

[[nodiscard]] static std::uint32_t slice_project_add_producer(
		SliceProjectIrBuilder& builder, Node* expression,
		bool distinct_consumer = false, bool force_value = false) {
	check_stack_depth();
	if (expression == nullptr) return 0u;
	const auto operation = slice_project_node_opcode(expression, *builder.oids);
	if (exprType(expression) != builder.oids->bioseq_seq_type_oid
		&& !operation.has_value() && !force_value) return 0u;
	/* CSE is descriptive only, but even descriptive CSE would hide distinct
	 * occurrences of a volatile expression from EXPLAIN. */
	if (!distinct_consumer && !contain_volatile_functions(expression)) {
		for (std::size_t index = 0u;
			 index < builder.producer_expressions.size(); ++index) {
			if (equal(builder.producer_expressions[index], expression)) {
				return static_cast<std::uint32_t>(index + 1u);
			}
		}
	}
	/* Assign children first so stable ids are deterministic and topological. */
	std::vector<std::uint32_t> dependencies;
	auto add_dependency = [&builder, &dependencies](
			Node* child, bool force_child) {
		const std::uint32_t dependency = slice_project_add_producer(
			builder, child, false, force_child);
		if (dependency == 0u
			|| std::find(dependencies.begin(), dependencies.end(), dependency)
				!= dependencies.end()) return;
		dependencies.push_back(dependency);
	};
	if (IsA(expression, SubscriptingRef)) {
		auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
		add_dependency(reinterpret_cast<Node*>(subscript->refexpr), false);
	} else if (FuncExpr* function = pg_node_try_as<FuncExpr>(expression)) {
		const bool all_arguments_are_values = operation.has_value()
			&& (*operation == SliceProjectOpcode::concat
				|| *operation == SliceProjectOpcode::bridge);
		for (Node* argument : PList<Node>(function->args)) {
			if (exprType(argument) == builder.oids->bioseq_seq_type_oid
				|| slice_project_node_opcode(argument, *builder.oids).has_value()
				|| all_arguments_are_values) {
				add_dependency(argument, all_arguments_are_values);
			}
		}
	}
	if (builder.producer_expressions.size()
		>= static_cast<std::size_t>(INT_MAX - 1)) {
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("BioseqSliceProject producer graph is too large")));
	}
	const std::uint32_t stable_id = static_cast<std::uint32_t>(
		builder.producer_expressions.size() + 1u);
	const int expression_index = slice_project_add_expression(
		builder, expression);
	const SliceProjectOpcode opcode = operation.has_value() ? *operation
		: (IsA(expression, Const) ? SliceProjectOpcode::literal_expr
			: SliceProjectOpcode::source_expr);
	int lower_expression_index = -1;
	int upper_expression_index = -1;
	if (IsA(expression, SubscriptingRef)) {
		auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
		if (subscript->reflowerindexpr != NIL) {
			lower_expression_index = slice_project_add_expression(builder,
				reinterpret_cast<Node*>(linitial(subscript->reflowerindexpr)));
		}
		if (subscript->refupperindexpr != NIL) {
			upper_expression_index = slice_project_add_expression(builder,
				reinterpret_cast<Node*>(linitial(subscript->refupperindexpr)));
		}
	}
	int transform = 0;
	if (opcode == SliceProjectOpcode::reverse) transform = 1;
	if (opcode == SliceProjectOpcode::reverse_complement) transform = 3;
	int mechanism = -1;
	int reason = -1;
	if (opcode == SliceProjectOpcode::materialize) {
		auto* function = reinterpret_cast<FuncExpr*>(expression);
		mechanism = static_cast<int>(function->funcid
			== builder.oids->seq_to_array_oid
			? materialization_mechanism::letter_array
			: materialization_mechanism::text);
		reason = static_cast<int>(materialization_reason::sql_conversion);
	}
	int flags = 0;
	if (contain_volatile_functions(expression)) flags |= kSliceNodeVolatile;
	if (!operation.has_value() && !IsA(expression, Var)
		&& !IsA(expression, Const)) flags |= kSliceNodeOpaque;
	List* dependency_codec = NIL;
	for (const std::uint32_t dependency : dependencies) {
		dependency_codec = lappend(dependency_codec,
			makeInteger(static_cast<int>(dependency)));
	}
	List* node = list_make5(
		makeInteger(static_cast<int>(stable_id)),
		makeInteger(static_cast<int>(opcode)), dependency_codec,
		makeInteger(expression_index), makeInteger(lower_expression_index));
	node = lappend(node, makeInteger(upper_expression_index));
	node = lappend(node, makeInteger(-1)); /* optional mmsearch options Expr */
	node = lappend(node, makeInteger(transform));
	node = lappend(node, makeInteger(mechanism));
	node = lappend(node, makeInteger(reason));
	node = lappend(node, makeInteger(flags));
	builder.nodes = lappend(builder.nodes, node);
	if (opcode == SliceProjectOpcode::source_expr
		|| opcode == SliceProjectOpcode::literal_expr) {
		builder.source_pieces = bioseq::parallelism::saturating_add(
			builder.source_pieces, 1u);
	}
	builder.producer_expressions.push_back(
		reinterpret_cast<Node*>(copyObject(expression)));
	builder.producer_dependencies.push_back(std::move(dependencies));
	return stable_id;
}

static void slice_project_note_source_binding(
		SliceProjectIrBuilder& builder, Node* sequence_expression,
		std::uint32_t producer_id) {
	if (sequence_expression == nullptr || producer_id == 0u) return;
	constexpr int pull_flags = PVC_RECURSE_AGGREGATES
		| PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	List* sources = pull_var_clause(sequence_expression, pull_flags);
	for (Node* source : PList<Node>(sources)) {
		Var* var = pg_node_try_as<Var>(source);
		if (var == nullptr || var->vartype != builder.oids->bioseq_seq_type_oid) {
			continue;
		}
		bool seen = false;
		for (auto& binding : builder.source_bindings) {
			if (!equal(binding.first, source)) continue;
			/* Zero is the deliberate ambiguity marker.  A source Datum cannot
			 * truthfully carry two simultaneous pre-projection producer ids. */
			if (binding.second != producer_id) binding.second = 0u;
			seen = true;
			break;
		}
		if (!seen) {
			builder.source_bindings.emplace_back(
				reinterpret_cast<Node*>(copyObject(source)), producer_id);
		}
	}
}

[[nodiscard]] static bool slice_project_producer_depends_on(
		SliceProjectIrBuilder const& builder, std::uint32_t producer_id,
		std::uint32_t possible_ancestor) {
	if (producer_id == 0u || possible_ancestor == 0u
		|| producer_id > builder.producer_dependencies.size()
		|| possible_ancestor >= producer_id) return false;
	std::vector<std::uint8_t> visited(
		builder.producer_dependencies.size() + 1u, 0u);
	std::vector<std::uint32_t> pending(
		builder.producer_dependencies[producer_id - 1u].begin(),
		builder.producer_dependencies[producer_id - 1u].end());
	while (!pending.empty()) {
		const std::uint32_t candidate = pending.back();
		pending.pop_back();
		if (candidate == possible_ancestor) return true;
		if (candidate == 0u
			|| candidate > builder.producer_dependencies.size()
			|| visited[candidate] != 0u) continue;
		visited[candidate] = 1u;
		auto const& dependencies = builder.producer_dependencies[candidate - 1u];
		pending.insert(pending.end(), dependencies.begin(), dependencies.end());
	}
	return false;
}

static void slice_project_add_boundary(
		SliceProjectIrBuilder& builder, Node* sequence_expression,
		std::uint32_t producer_id, materialization_mechanism mechanism,
		materialization_reason reason, bool preprojection) {
	if (sequence_expression == nullptr || producer_id == 0u) return;
	const int expression_index = slice_project_add_expression(
		builder, sequence_expression);
	for (Node* raw : PList<Node>(builder.records)) {
		List* record = reinterpret_cast<List*>(raw);
		if (intVal(list_nth(record, kSliceRecordProducerId))
				== static_cast<int>(producer_id)
			&& intVal(list_nth(record, kSliceRecordExpressionIndex))
				== expression_index
			&& intVal(list_nth(record, kSliceRecordMechanism))
				== static_cast<int>(mechanism)
			&& intVal(list_nth(record, kSliceRecordReason))
				== static_cast<int>(reason)) return;
	}
	if (builder.next_boundary_id == UINT32_MAX) {
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("BioseqSliceProject materialization DAG is too large")));
	}
	const std::uint64_t estimated_symbols = slice_project_estimated_symbols(
		sequence_expression, *builder.oids);
	List* dependencies = NIL;
	for (Node* raw : PList<Node>(builder.records)) {
		List* previous = reinterpret_cast<List*>(raw);
		const auto previous_producer = static_cast<std::uint32_t>(intVal(
			list_nth(previous, kSliceRecordProducerId)));
		if (!slice_project_producer_depends_on(
				builder, producer_id, previous_producer)) continue;
		dependencies = lappend(dependencies, makeInteger(intVal(
			list_nth(previous, kSliceRecordBoundaryId))));
	}
	List* record = list_make5(
		makeInteger(static_cast<int>(builder.next_boundary_id++)),
		makeInteger(static_cast<int>(producer_id)),
		makeInteger(expression_index), dependencies,
		makeInteger(static_cast<int>(mechanism)));
	record = lappend(record, makeInteger(static_cast<int>(reason)));
	record = lappend(record, slice_project_int8_const(estimated_symbols));
	record = lappend(record, makeString(pstrdup(
		slice_project_encoding_name(mechanism))));
	record = lappend(record, makeString(pstrdup(
		preprojection ? "projection row" : "result datum")));
	builder.records = lappend(builder.records, record);
	builder.planned_symbols = bioseq::parallelism::saturating_add(
		builder.planned_symbols, estimated_symbols);
	builder.planned_bytes = bioseq::parallelism::saturating_add(
		builder.planned_bytes, estimated_symbols);
	if (preprojection) {
		slice_project_note_source_binding(
			builder, sequence_expression, producer_id);
	}
}

[[nodiscard]] static std::uint32_t slice_project_build_ir_node(
		SliceProjectIrBuilder& builder, Node* expression) {
	check_stack_depth();
	if (expression == nullptr) return 0u;
	const auto operation = slice_project_node_opcode(expression, *builder.oids);
	if (!operation.has_value()) {
		return slice_project_add_producer(builder, expression);
	}
	builder.found = true;
	const SliceProjectOpcode opcode = *operation;
	if (opcode == SliceProjectOpcode::slice
		|| opcode == SliceProjectOpcode::concat
		|| opcode == SliceProjectOpcode::reverse
		|| opcode == SliceProjectOpcode::reverse_complement) {
		builder.piece_boundaries = bioseq::parallelism::saturating_add(
			builder.piece_boundaries, 1u);
	}
	if (FuncExpr* function = pg_node_try_as<FuncExpr>(expression)) {
		for (Node* argument : PList<Node>(function->args)) {
			if (slice_project_node_opcode(argument, *builder.oids).has_value()) {
				(void)slice_project_build_ir_node(builder, argument);
			} else if (exprType(argument) == builder.oids->bioseq_seq_type_oid) {
				(void)slice_project_add_producer(builder, argument);
			}
		}
		if (opcode == SliceProjectOpcode::materialize
			&& function->args != NIL) {
			Node* source = reinterpret_cast<Node*>(linitial(function->args));
			const std::uint32_t producer_id = slice_project_add_producer(
				builder, source);
			const bool as_array = function->funcid
				== builder.oids->seq_to_array_oid;
			slice_project_add_boundary(builder, source, producer_id,
				as_array ? materialization_mechanism::letter_array
					: materialization_mechanism::text,
				materialization_reason::sql_conversion, true);
		}
	} else if (IsA(expression, SubscriptingRef)) {
		auto* subscript = reinterpret_cast<SubscriptingRef*>(expression);
		Node* source = reinterpret_cast<Node*>(subscript->refexpr);
		if (slice_project_node_opcode(source, *builder.oids).has_value()) {
			(void)slice_project_build_ir_node(builder, source);
		} else {
			(void)slice_project_add_producer(builder, source);
		}
	}
	return slice_project_add_producer(builder, expression);
}

struct SliceProjectDiscovery {
	SliceProjectIrBuilder* builder = nullptr;
	bool found = false;
};

struct slice_project_discovery_visitor {
	[[nodiscard]] pg_walk_step operator()(
		pg_expression_walk_cursor, pg_node_view node,
		SliceProjectDiscovery& discovery) const {
		if (slice_project_node_opcode(
				node.raw(), *discovery.builder->oids).has_value()) {
			if (contain_volatile_functions(node.raw())) {
				return pg_walk_step::skip_subtree;
			}
			(void)slice_project_build_ir_node(
				*discovery.builder, node.raw());
			discovery.found = true;
			return pg_walk_step::skip_subtree;
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static bool slice_project_discover_target(
		PathTarget* target, SliceProjectIrBuilder& builder) {
	if (target == nullptr) return false;
	slice_project_discovery_visitor visitor;
	int resno = 1;
	for (Node* expression : PList<Node>(target->exprs)) {
		SliceProjectDiscovery discovery{&builder};
		pg_walk_expression_including_root(expression, discovery, visitor);
		if (discovery.found
			&& exprType(expression) == builder.oids->bioseq_seq_type_oid) {
			const std::uint32_t producer_id = slice_project_add_producer(
				builder, expression);
			builder.output_bindings = lappend(builder.output_bindings,
				list_make2(makeInteger(resno),
					makeInteger(static_cast<int>(producer_id))));
			if (builder.command_type == CMD_INSERT) {
				slice_project_add_boundary(builder, expression, producer_id,
					materialization_mechanism::packed_full,
					materialization_reason::heap_tuple, false);
			} else if (builder.command_type == CMD_SELECT) {
				slice_project_add_boundary(builder, expression, producer_id,
					materialization_mechanism::text,
					materialization_reason::client_output, false);
				slice_project_add_boundary(builder, expression, producer_id,
					materialization_mechanism::binary,
					materialization_reason::client_output, false);
			}
		}
		++resno;
	}
	return builder.found;
}

export bool bioseq_expr_uses_lazy_sequence_projection(Node* node) {
	if (node == nullptr) return false;
	const BioseqStubOids* oids = try_get_cached_stub_oids();
	if (oids == nullptr) return false;
	SliceProjectIrBuilder builder{.oids = oids};
	SliceProjectDiscovery discovery{&builder};
	slice_project_discovery_visitor visitor;
	pg_walk_expression_including_root(node, discovery, visitor);
	return discovery.found;
}

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

constexpr std::uint32_t kBioseqSliceDsmMagic = 0x42534432u; /* BSD2 */
constexpr std::uint16_t kBioseqSliceDsmVersion = 1u;

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

struct SliceProjectOutputBinding {
	AttrNumber resno;
	std::uint32_t producer_value_id;
};

struct SliceProjectRuntime {
	std::uint64_t rows_published = 0u;
	std::uint64_t rescans = 0u;
	std::uint64_t planned_symbols = 0u;
	std::uint64_t planned_bytes = 0u;
	std::uint32_t boundary_count = 0u;
	std::uint32_t output_binding_count = 0u;
	SliceProjectOutputBinding* output_bindings = nullptr;
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
	int dsm_test_fixture_mode = kSliceDsmTestNone;
	std::uint32_t dsm_test_checks = 0u;
	std::uint32_t dsm_test_slot_headers_read = 0u;
	std::uint32_t dsm_test_counter_records_read = 0u;
	std::uint32_t dsm_test_cleanup_callbacks = 0u;
#endif
};

/* slice_async_max_live_jobs is the one normative concurrency bound. Released
 * slots advance their generation and are reused across arbitrarily many
 * batches without multiplying every trace boundary by dispatch capacity. */

struct BioseqSliceProjectState {
	CustomScanState custom_scanstate;
	SliceProjectRuntime runtime;
};
static_assert(std::is_standard_layout_v<BioseqSliceProjectState>);
static_assert(offsetof(BioseqSliceProjectState, custom_scanstate) == 0);

[[nodiscard]] static CustomExecMethods const&
bioseq_slice_project_exec_methods() noexcept;
[[nodiscard]] static CustomScanMethods const&
bioseq_slice_project_scan_methods() noexcept;

[[nodiscard]] static List* slice_project_header(
		const SliceProjectIrBuilder& builder) {
	List* header = list_make5(
		makeInteger(kBioseqSliceProjectMagic),
		makeInteger(kBioseqSliceProjectCodecVersion),
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

[[nodiscard]] static List* slice_project_plan_header(CustomScan* scan) {
	if (scan == nullptr || list_length(scan->custom_private) != 4) return NIL;
	Node* header = reinterpret_cast<Node*>(linitial(scan->custom_private));
	return header != nullptr && IsA(header, List)
		? reinterpret_cast<List*>(header) : NIL;
}

[[nodiscard]] static List* slice_project_plan_nodes(CustomScan* scan) {
	if (scan == nullptr || list_length(scan->custom_private) != 4) return NIL;
	Node* nodes = reinterpret_cast<Node*>(lsecond(scan->custom_private));
	return nodes != nullptr && IsA(nodes, List)
		? reinterpret_cast<List*>(nodes) : NIL;
}

[[nodiscard]] static List* slice_project_plan_records(CustomScan* scan) {
	if (scan == nullptr || list_length(scan->custom_private) != 4) return NIL;
	Node* records = reinterpret_cast<Node*>(lthird(scan->custom_private));
	return records != nullptr && IsA(records, List)
		? reinterpret_cast<List*>(records) : NIL;
}

[[nodiscard]] static List* slice_project_plan_output_bindings(
		CustomScan* scan) {
	if (scan == nullptr || list_length(scan->custom_private) != 4) return NIL;
	Node* bindings = reinterpret_cast<Node*>(lfourth(scan->custom_private));
	return bindings != nullptr && IsA(bindings, List)
		? reinterpret_cast<List*>(bindings) : NIL;
}

static void slice_project_validate_codec(CustomScan* scan) {
	List* header = slice_project_plan_header(scan);
	List* nodes = slice_project_plan_nodes(scan);
	List* records = slice_project_plan_records(scan);
	List* output_bindings = slice_project_plan_output_bindings(scan);
	if (header == NIL || list_length(header) != kBioseqSliceProjectHeaderFields
		|| !IsA(reinterpret_cast<Node*>(list_nth(header, kSliceHeaderMagic)), Integer)
		|| !IsA(reinterpret_cast<Node*>(list_nth(header, kSliceHeaderVersion)), Integer)
		|| !IsA(reinterpret_cast<Node*>(list_nth(
			header, kSliceHeaderBoundaryCount)), Integer)
		|| !IsA(reinterpret_cast<Node*>(list_nth(
			header, kSliceHeaderExpressionCount)), Integer)
		|| !IsA(reinterpret_cast<Node*>(list_nth(
			header, kSliceHeaderProducerCount)), Integer)
		|| !IsA(reinterpret_cast<Node*>(list_nth(
			header, kSliceHeaderFallbackReason)), String)
		|| intVal(list_nth(header, kSliceHeaderMagic))
			!= kBioseqSliceProjectMagic
		|| intVal(list_nth(header, kSliceHeaderVersion))
			!= kBioseqSliceProjectCodecVersion) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject plan codec header is invalid"),
			 errdetail("header_fields=%d expected_fields=%d",
				 list_length(header), kBioseqSliceProjectHeaderFields)));
	}
	const int boundary_count = intVal(
		list_nth(header, kSliceHeaderBoundaryCount));
	const int expression_count = intVal(
		list_nth(header, kSliceHeaderExpressionCount));
	const int producer_count = intVal(
		list_nth(header, kSliceHeaderProducerCount));
	if (boundary_count < 0 || boundary_count != list_length(records)
		|| expression_count < 0
		|| expression_count != list_length(scan->custom_exprs)
		|| producer_count < 1 || producer_count != list_length(nodes)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject plan codec cardinality is invalid"),
			 errdetail("boundary_count=%d record_count=%d "
				 "expression_count=%d custom_expression_count=%d "
				 "producer_count=%d node_count=%d",
				 boundary_count, list_length(records), expression_count,
				 list_length(scan->custom_exprs), producer_count,
				 list_length(nodes))));
	}
	(void)slice_project_int8_value(reinterpret_cast<Node*>(
		list_nth(header, kSliceHeaderPieceBoundaries)));
	(void)slice_project_int8_value(reinterpret_cast<Node*>(
		list_nth(header, kSliceHeaderSourcePieces)));
	(void)slice_project_int8_value(reinterpret_cast<Node*>(
		list_nth(header, kSliceHeaderPlannedSymbols)));
	(void)slice_project_int8_value(reinterpret_cast<Node*>(
		list_nth(header, kSliceHeaderPlannedBytes)));

	int expected_stable_id = 1;
	for (Node* raw_node : PList<Node>(nodes)) {
		List* node = raw_node != nullptr && IsA(raw_node, List)
			? reinterpret_cast<List*>(raw_node) : NIL;
		if (node == NIL || list_length(node) != kBioseqSliceProjectNodeFields) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject IR node is invalid"),
				 errdetail("stable_id=%d node_fields=%d expected_fields=%d",
					 expected_stable_id, list_length(node),
					 kBioseqSliceProjectNodeFields)));
		}
		constexpr std::array<int, 9> integer_fields{
			kSliceNodeStableId, kSliceNodeOpcode,
			kSliceNodeExpressionIndex, kSliceNodeLowerExpressionIndex,
			kSliceNodeUpperExpressionIndex, kSliceNodeOptionsExpressionIndex,
			kSliceNodeTransform, kSliceNodeMechanism, kSliceNodeReason};
		for (const int field : integer_fields) {
			Node* value = reinterpret_cast<Node*>(list_nth(node, field));
			if (value == nullptr || !IsA(value, Integer)) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqSliceProject IR integer field is invalid"),
					 errdetail("stable_id=%d field=%d node_tag=%d",
						 expected_stable_id, field, value == nullptr ? 0
							 : static_cast<int>(nodeTag(value)))));
			}
		}
		Node* flags_node = reinterpret_cast<Node*>(
			list_nth(node, kSliceNodeFlags));
		Node* dependencies_node = reinterpret_cast<Node*>(
			list_nth(node, kSliceNodeDependencies));
		if (flags_node == nullptr || !IsA(flags_node, Integer)
			|| (dependencies_node != nullptr
				&& !IsA(dependencies_node, List))) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject IR topology field is invalid"),
				 errdetail("stable_id=%d flags_tag=%d dependencies_tag=%d",
					 expected_stable_id,
					 flags_node == nullptr ? 0
						 : static_cast<int>(nodeTag(flags_node)),
					 dependencies_node == nullptr ? 0
						 : static_cast<int>(nodeTag(dependencies_node)))));
		}
		List* dependencies = dependencies_node == nullptr ? NIL
			: reinterpret_cast<List*>(dependencies_node);
		const int stable_id = intVal(list_nth(node, kSliceNodeStableId));
		const int opcode = intVal(list_nth(node, kSliceNodeOpcode));
		const int expression_index = intVal(
			list_nth(node, kSliceNodeExpressionIndex));
		const int lower_expression_index = intVal(
			list_nth(node, kSliceNodeLowerExpressionIndex));
		const int upper_expression_index = intVal(
			list_nth(node, kSliceNodeUpperExpressionIndex));
		const int options_expression_index = intVal(
			list_nth(node, kSliceNodeOptionsExpressionIndex));
		const int transform = intVal(list_nth(node, kSliceNodeTransform));
		const int mechanism = intVal(list_nth(node, kSliceNodeMechanism));
		const int reason = intVal(list_nth(node, kSliceNodeReason));
		const int flags = intVal(flags_node);
		auto valid_optional_expression_index = [expression_count](int index) {
			return index == -1 || (index >= 0 && index < expression_count);
		};
		if (stable_id != expected_stable_id
			|| opcode < static_cast<int>(SliceProjectOpcode::source_expr)
			|| opcode > static_cast<int>(SliceProjectOpcode::materialize)
			|| expression_index < 0 || expression_index >= expression_count
			|| !valid_optional_expression_index(lower_expression_index)
			|| !valid_optional_expression_index(upper_expression_index)
			|| !valid_optional_expression_index(options_expression_index)
			|| transform < 0 || transform > 3
			|| flags < 0
			|| (flags & ~(kSliceNodeVolatile | kSliceNodeOpaque)) != 0) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject IR node metadata is invalid"),
				 errdetail("stable_id=%d expected_id=%d opcode=%d "
					 "expression=%d lower=%d upper=%d options=%d "
					 "transform=%d flags=%d",
					 stable_id, expected_stable_id, opcode, expression_index,
					 lower_expression_index, upper_expression_index,
					 options_expression_index, transform, flags)));
		}
		Bitmapset* seen_dependencies = nullptr;
		for (Node* dependency : PList<Node>(dependencies)) {
			if (dependency == nullptr || !IsA(dependency, Integer)
				|| intVal(dependency) <= 0 || intVal(dependency) >= stable_id
				|| bms_is_member(intVal(dependency), seen_dependencies)) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqSliceProject IR dependency is not topological"),
					 errdetail("stable_id=%d dependency_id=%d dependency_tag=%d",
						 stable_id,
						 dependency != nullptr && IsA(dependency, Integer)
							 ? intVal(dependency) : 0,
						 dependency == nullptr ? 0
							 : static_cast<int>(nodeTag(dependency)))));
			}
			seen_dependencies = bms_add_member(
				seen_dependencies, intVal(dependency));
		}
		if (seen_dependencies != nullptr) bms_free(seen_dependencies);
		const auto typed_opcode = static_cast<SliceProjectOpcode>(opcode);
		const int dependency_count = list_length(dependencies);
		const bool leaf = typed_opcode == SliceProjectOpcode::source_expr
			|| typed_opcode == SliceProjectOpcode::literal_expr;
		const bool unary = typed_opcode == SliceProjectOpcode::element
			|| typed_opcode == SliceProjectOpcode::slice
			|| typed_opcode == SliceProjectOpcode::reverse
			|| typed_opcode == SliceProjectOpcode::reverse_complement
			|| typed_opcode == SliceProjectOpcode::length
			|| typed_opcode == SliceProjectOpcode::cardinality
			|| typed_opcode == SliceProjectOpcode::materialize;
		const bool materialize = typed_opcode
			== SliceProjectOpcode::materialize;
		const bool bounds_valid = (typed_opcode == SliceProjectOpcode::slice
			|| typed_opcode == SliceProjectOpcode::element)
			? (typed_opcode == SliceProjectOpcode::element
				? lower_expression_index == -1 && upper_expression_index >= 0
				: true)
			: lower_expression_index == -1 && upper_expression_index == -1;
		const int expected_transform = typed_opcode == SliceProjectOpcode::reverse
			? 1 : (typed_opcode == SliceProjectOpcode::reverse_complement ? 3 : 0);
		const bool boundary_metadata_valid = materialize
			? mechanism >= static_cast<int>(materialization_mechanism::backing_copy)
				&& mechanism <= static_cast<int>(materialization_mechanism::letter_array)
				&& reason == static_cast<int>(materialization_reason::sql_conversion)
			: mechanism == -1 && reason == -1;
		if ((leaf && dependency_count != 0)
			|| (unary && dependency_count != 1)
			|| (typed_opcode == SliceProjectOpcode::concat
				&& dependency_count < 1)
			|| (typed_opcode == SliceProjectOpcode::bridge
				&& dependency_count < 1)
			|| !bounds_valid || transform != expected_transform
			|| !boundary_metadata_valid || options_expression_index != -1) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject IR opcode contract is invalid"),
				 errdetail("stable_id=%d opcode=%d dependencies=%d "
					 "lower=%d upper=%d options=%d transform=%d "
					 "mechanism=%d reason=%d",
					 stable_id, opcode, dependency_count,
					 lower_expression_index, upper_expression_index,
					 options_expression_index, transform, mechanism, reason)));
		}
		++expected_stable_id;
	}

	std::uint32_t expected_boundary_id = 1u;
	int record_index = 0;
	for (Node* raw_record : PList<Node>(records)) {
		List* record = raw_record != nullptr && IsA(raw_record, List)
			? reinterpret_cast<List*>(raw_record) : NIL;
		if (record == NIL
			|| list_length(record) != kBioseqSliceProjectRecordFields) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject plan codec record is invalid"),
				 errdetail("record_index=%d record_node_tag=%d "
					 "record_fields=%d expected_fields=%d",
					 record_index,
					 raw_record == nullptr ? 0
						 : static_cast<int>(nodeTag(raw_record)),
					 list_length(record),
					 kBioseqSliceProjectRecordFields)));
		}
		Node* boundary_id_node = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordBoundaryId));
		Node* producer_id_node = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordProducerId));
		Node* expression_index_node = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordExpressionIndex));
		Node* mechanism_node = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordMechanism));
		Node* reason_node = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordReason));
		if (boundary_id_node == nullptr || !IsA(boundary_id_node, Integer)
			|| producer_id_node == nullptr || !IsA(producer_id_node, Integer)
			|| expression_index_node == nullptr
			|| !IsA(expression_index_node, Integer)
			|| mechanism_node == nullptr || !IsA(mechanism_node, Integer)
			|| reason_node == nullptr || !IsA(reason_node, Integer)) {
			auto node_tag_or_zero = [](Node* node) noexcept {
				return node == nullptr ? 0 : static_cast<int>(nodeTag(node));
			};
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject plan codec topology is invalid"),
				 errdetail("record_index=%d integer_field_tags: "
					 "boundary_id=%d producer_id=%d expression_index=%d "
					 "mechanism=%d reason=%d",
					 record_index, node_tag_or_zero(boundary_id_node),
					 node_tag_or_zero(producer_id_node),
					 node_tag_or_zero(expression_index_node),
					 node_tag_or_zero(mechanism_node),
					 node_tag_or_zero(reason_node))));
		}
		const int boundary_id = intVal(boundary_id_node);
		const int producer_id = intVal(producer_id_node);
		const int expression_index = intVal(
			expression_index_node);
		const int mechanism = intVal(mechanism_node);
		const int reason = intVal(reason_node);
		Node* dependency_node = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordDependencies));
		const bool dependency_shape_valid = dependency_node == nullptr
			|| IsA(dependency_node, List);
		List* dependencies = dependency_node == nullptr ? NIL
			: (dependency_shape_valid
				? reinterpret_cast<List*>(dependency_node) : NIL);
		if (boundary_id != static_cast<int>(expected_boundary_id)
			|| producer_id <= 0 || producer_id > producer_count
			|| expression_index < 0 || expression_index >= expression_count
			|| mechanism < static_cast<int>(
				materialization_mechanism::backing_copy)
			|| mechanism > static_cast<int>(
				materialization_mechanism::letter_array)
			|| reason < static_cast<int>(materialization_reason::executor_copy)
			|| reason > static_cast<int>(materialization_reason::client_output)
			|| !dependency_shape_valid) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject plan codec topology is invalid"),
				 errdetail("record_index=%d boundary_id=%d "
					 "expected_boundary_id=%u producer_id=%d "
					 "producer_count=%d mechanism=%d reason=%d "
					 "expression_index=%d expression_count=%d "
					 "dependency_node_tag=%d",
					 record_index, boundary_id, expected_boundary_id,
					 producer_id, producer_count, mechanism, reason,
					 expression_index, expression_count,
					 dependency_node == nullptr ? 0
						 : static_cast<int>(nodeTag(dependency_node)))));
		}
		int dependency_index = 0;
		for (Node* dependency : PList<Node>(dependencies)) {
			if (dependency == nullptr || !IsA(dependency, Integer)
				|| intVal(dependency) <= 0
				|| intVal(dependency) >= boundary_id) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("BioseqSliceProject dependency is not topological"),
					 errdetail("record_index=%d dependency_index=%d "
						 "boundary_id=%d dependency_id=%d dependency_node_tag=%d",
						 record_index, dependency_index, boundary_id,
						 dependency != nullptr && IsA(dependency, Integer)
							 ? intVal(dependency) : 0,
						 dependency == nullptr ? 0
							 : static_cast<int>(nodeTag(dependency)))));
			}
			++dependency_index;
		}
		(void)slice_project_int8_value(reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordEstimatedSymbols)));
		Node* encoding = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordEncoding));
		Node* cache_scope = reinterpret_cast<Node*>(
			list_nth(record, kSliceRecordCacheScope));
		if (encoding == nullptr || !IsA(encoding, String)
			|| cache_scope == nullptr || !IsA(cache_scope, String)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject boundary metadata is invalid"),
				 errdetail("record_index=%d encoding_node_tag=%d "
					 "cache_scope_node_tag=%d",
					 record_index,
					 encoding == nullptr ? 0
						 : static_cast<int>(nodeTag(encoding)),
					 cache_scope == nullptr ? 0
						 : static_cast<int>(nodeTag(cache_scope)))));
		}
		++expected_boundary_id;
		++record_index;
	}
	Bitmapset* seen_resnos = nullptr;
	int binding_index = 0;
	for (Node* raw_binding : PList<Node>(output_bindings)) {
		List* binding = raw_binding != nullptr && IsA(raw_binding, List)
			? reinterpret_cast<List*>(raw_binding) : NIL;
		if (list_length(binding) != 2
			|| !IsA(reinterpret_cast<Node*>(linitial(binding)), Integer)
			|| !IsA(reinterpret_cast<Node*>(lsecond(binding)), Integer)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject output binding is invalid"),
				 errdetail("binding_index=%d", binding_index)));
		}
		const int resno = intVal(linitial(binding));
		const int producer_id = intVal(lsecond(binding));
		if (resno <= 0 || resno > list_length(scan->scan.plan.targetlist)
			|| producer_id <= 0 || producer_id > producer_count
			|| bms_is_member(resno, seen_resnos)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject output binding is out of range"),
				 errdetail("binding_index=%d resno=%d producer_id=%d",
					 binding_index, resno, producer_id)));
		}
		seen_resnos = bms_add_member(seen_resnos, resno);
		++binding_index;
	}
	if (seen_resnos != nullptr) bms_free(seen_resnos);
}

[[nodiscard]] static Plan* BioseqSliceProject_PlanCustomPath(
		PlannerInfo*, RelOptInfo* rel, CustomPath* best_path,
		List* tlist, List* clauses, List* custom_plans) {
	if (rel == nullptr || best_path == nullptr
		|| list_length(custom_plans) != 1
		|| list_length(best_path->custom_private) != 5) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject path shape is invalid")));
	}
	List* header = reinterpret_cast<List*>(
		list_nth(best_path->custom_private, 0));
	List* expressions = reinterpret_cast<List*>(
		list_nth(best_path->custom_private, 1));
	List* nodes = reinterpret_cast<List*>(
		list_nth(best_path->custom_private, 2));
	List* records = reinterpret_cast<List*>(
		list_nth(best_path->custom_private, 3));
	List* output_bindings = reinterpret_cast<List*>(
		list_nth(best_path->custom_private, 4));
	if (header == nullptr || !IsA(header, List)
		|| (expressions != NIL && !IsA(expressions, List))
		|| (nodes != NIL && !IsA(nodes, List))
		|| (records != NIL && !IsA(records, List))
		|| (output_bindings != NIL && !IsA(output_bindings, List))) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject path codec is invalid")));
	}
	auto* scan = makeNode(CustomScan);
	scan->methods = &bioseq_slice_project_scan_methods();
	scan->scan.scanrelid = 0;
	scan->scan.plan.targetlist = tlist;
	/* SliceProject is a projection owner, never a restriction owner.  This is
	 * essential when the lowest valid placement is directly above a base
	 * BioseqScan: create_plan has already given the child the base clauses, and
	 * copying them onto this scanrelid-zero wrapper would both execute them
	 * twice and ask setrefs to resolve base Vars against the projected output. */
	(void)clauses;
	scan->scan.plan.qual = NIL;
	scan->custom_plans = custom_plans;
	auto* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
	scan->custom_scan_tlist = static_cast<List*>(
		copyObject(child_plan->targetlist));
	scan->custom_exprs = static_cast<List*>(copyObject(expressions));
	scan->custom_private = list_make4(
		copyObject(header), copyObject(nodes), copyObject(records),
		copyObject(output_bindings));
	scan->custom_relids = bms_copy(rel->relids);
	scan->flags = best_path->flags;
	return reinterpret_cast<Plan*>(scan);
}

[[nodiscard]] static List*
BioseqSliceProject_ReparameterizeCustomPathByChild(
		PlannerInfo*, List* custom_private, RelOptInfo*) {
	return static_cast<List*>(copyObject(custom_private));
}

[[nodiscard]] static Node* BioseqSliceProject_CreateCustomScanState(
		CustomScan*) {
	auto* state = static_cast<BioseqSliceProjectState*>(
		palloc0(sizeof(BioseqSliceProjectState)));
	NodeSetTag(&state->custom_scanstate, T_CustomScanState);
	state->custom_scanstate.methods = &bioseq_slice_project_exec_methods();
	return reinterpret_cast<Node*>(state);
}

[[nodiscard]] static CustomScanMethods const&
bioseq_slice_project_scan_methods() noexcept {
	static const CustomScanMethods methods{
		.CustomName = "BioseqSliceProject",
		.CreateCustomScanState = BioseqSliceProject_CreateCustomScanState,
	};
	return methods;
}

[[nodiscard]] static CustomPathMethods const&
bioseq_slice_project_path_methods() noexcept {
	static const CustomPathMethods methods{
		.CustomName = "BioseqSliceProject",
		.PlanCustomPath = BioseqSliceProject_PlanCustomPath,
		.ReparameterizeCustomPathByChild =
			BioseqSliceProject_ReparameterizeCustomPathByChild,
	};
	return methods;
}

struct SliceProjectDsmLayout {
	Size header_bytes;
	Size slot_stride;
	Size total_bytes;
};

struct SliceProjectDsmMergeAudit {
	std::uint32_t slot_headers_read = 0u;
	std::uint32_t counter_records_read = 0u;
	std::uint32_t merged_slots = 0u;
	std::uint32_t incomplete_slots = 0u;
	bool header_valid = false;
	bool mapping_valid = false;
	bool merged = false;
};

[[nodiscard]] static SliceProjectDsmLayout slice_project_dsm_layout(
		std::uint32_t recipe_count, std::uint32_t slot_count) {
	const Size header_bytes = MAXALIGN(sizeof(slice_dsm_header));
	const Size counter_bytes = mul_size(
		static_cast<Size>(recipe_count), sizeof(slice_dsm_counter));
	const Size slot_stride = MAXALIGN(add_size(
		sizeof(slice_dsm_worker_header), counter_bytes));
	return {
		.header_bytes = header_bytes,
		.slot_stride = slot_stride,
		.total_bytes = add_size(header_bytes,
			mul_size(static_cast<Size>(slot_count), slot_stride)),
	};
}

[[nodiscard]] static bool slice_project_checked_add_noexcept(
		Size left, Size right, Size* result) noexcept {
	if (result == nullptr || left > MaxAllocSize
		|| right > MaxAllocSize - left) return false;
	*result = left + right;
	return true;
}

[[nodiscard]] static bool slice_project_checked_mul_noexcept(
		Size left, Size right, Size* result) noexcept {
	if (result == nullptr || (left != 0u && right > MaxAllocSize / left)) {
		return false;
	}
	*result = left * right;
	return true;
}

[[nodiscard]] static bool slice_project_dsm_layout_noexcept(
		std::uint32_t recipe_count, std::uint32_t slot_count,
		SliceProjectDsmLayout* result) noexcept {
	if (result == nullptr) return false;
	Size counter_bytes = 0u;
	Size raw_stride = 0u;
	Size slots_bytes = 0u;
	const Size header_bytes = MAXALIGN(sizeof(slice_dsm_header));
	if (!slice_project_checked_mul_noexcept(
			static_cast<Size>(recipe_count), sizeof(slice_dsm_counter),
			&counter_bytes)
		|| !slice_project_checked_add_noexcept(
			sizeof(slice_dsm_worker_header), counter_bytes, &raw_stride)) {
		return false;
	}
	const Size slot_stride = MAXALIGN(raw_stride);
	if (slot_stride < raw_stride
		|| !slice_project_checked_mul_noexcept(
			static_cast<Size>(slot_count), slot_stride, &slots_bytes)
		|| !slice_project_checked_add_noexcept(
			header_bytes, slots_bytes, &result->total_bytes)) return false;
	result->header_bytes = header_bytes;
	result->slot_stride = slot_stride;
	return true;
}

[[nodiscard]] static slice_dsm_worker_header* slice_project_worker_slot(
		slice_dsm_header* header, std::uint32_t slot_index) noexcept {
	return reinterpret_cast<slice_dsm_worker_header*>(
		reinterpret_cast<char*>(header) + header->header_bytes
		+ static_cast<Size>(slot_index) * header->slot_stride);
}

[[nodiscard]] static slice_dsm_counter* slice_project_worker_counters(
		slice_dsm_worker_header* worker) noexcept {
	return reinterpret_cast<slice_dsm_counter*>(
		reinterpret_cast<char*>(worker) + sizeof(slice_dsm_worker_header));
}

static void slice_project_mark_trace_invalid_noexcept(
		slice_trace_attachment* attachment) noexcept {
	if (!slice_trace_attachment_live(attachment)) return;
	attachment->runtime->invalid = true;
	attachment->runtime->overflowed = true;
}

[[nodiscard]] static bool slice_project_validate_dsm_header_noexcept(
		slice_dsm_header const* header, std::uint32_t recipe_count,
		std::uint32_t slot_count, std::uint64_t cycle,
		SliceProjectDsmLayout* layout) noexcept {
	if (header == nullptr
		|| !slice_project_dsm_layout_noexcept(
			recipe_count, slot_count, layout)) return false;
	return header->magic == kBioseqSliceDsmMagic
		&& header->codec_version == kBioseqSliceDsmVersion
		&& header->header_bytes == layout->header_bytes
		&& header->recipe_count == recipe_count
		&& header->slot_count == slot_count
		&& header->cycle == cycle
		&& header->slot_stride == layout->slot_stride;
}

static void slice_project_merge_dsm_cycle_noexcept(
		slice_dsm_header* header, std::uintptr_t map_base, Size map_length,
		std::uint32_t slot_count, slice_trace_attachment* attachment,
		std::uint64_t expected_cycle,
		SliceProjectDsmMergeAudit* audit = nullptr) noexcept {
	if (audit != nullptr) *audit = {};
	if (!slice_trace_attachment_live(attachment) || header == nullptr) return;
	auto& trace = *attachment->runtime;
	if (trace.last_merged_cycle >= expected_cycle) return;
	const auto coordinate = reinterpret_cast<std::uintptr_t>(header);
	if (coordinate < map_base || coordinate - map_base > map_length
		|| sizeof(slice_dsm_header)
			> map_length - (coordinate - map_base)) {
		slice_project_mark_trace_invalid_noexcept(attachment);
		return;
	}
	SliceProjectDsmLayout layout{};
	if (!slice_project_validate_dsm_header_noexcept(
			header, trace.boundary_count, slot_count,
			expected_cycle, &layout)) {
		slice_project_mark_trace_invalid_noexcept(attachment);
		return;
	}
	if (audit != nullptr) audit->header_valid = true;
	if (layout.total_bytes > map_length - (coordinate - map_base)) {
		slice_project_mark_trace_invalid_noexcept(attachment);
		return;
	}
	if (audit != nullptr) audit->mapping_valid = true;
	/* First pass validates the complete waited cycle before a single counter is
	 * merged.  A malformed slot therefore cannot create a plausible prefix. */
	for (std::uint32_t slot_index = 0u;
		 slot_index < slot_count; ++slot_index) {
		auto* worker = slice_project_worker_slot(header, slot_index);
		if (audit != nullptr) ++audit->slot_headers_read;
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
		if (worker->cycle != expected_cycle || worker->overflowed > 1u
			|| std::any_of(worker->reserved.begin(), worker->reserved.end(),
				[](std::byte byte) { return byte != std::byte{0}; })) {
			slice_project_mark_trace_invalid_noexcept(attachment);
			return;
		}
		auto* counters = slice_project_worker_counters(worker);
		for (std::uint32_t counter_index = 0u;
			 counter_index < trace.boundary_count; ++counter_index) {
			if (audit != nullptr) ++audit->counter_records_read;
			if (counters[counter_index].overflowed > 1u
				|| !slice_dsm_counter_reserved_valid(
					counters[counter_index])) {
				slice_project_mark_trace_invalid_noexcept(attachment);
				return;
			}
		}
	}
	std::uint32_t incomplete = 0u;
	for (std::uint32_t slot_index = 0u;
		 slot_index < slot_count; ++slot_index) {
		auto* worker = slice_project_worker_slot(header, slot_index);
		const auto state = worker->publish_state;
		if (state == slice_dsm_publish_state::attached_running
			|| state == slice_dsm_publish_state::published_incomplete) {
			if (incomplete == UINT32_MAX) {
				trace.overflowed = true;
			} else {
				++incomplete;
			}
		}
		if (state == slice_dsm_publish_state::never_launched) continue;
		if (audit != nullptr) ++audit->merged_slots;
		auto* counters = slice_project_worker_counters(worker);
		for (std::uint32_t counter_index = 0u;
			 counter_index < trace.boundary_count; ++counter_index) {
			merge_slice_dsm_counter_noexcept(
				trace.cumulative_sink.entries[counter_index],
				counters[counter_index]);
			trace.overflowed = trace.overflowed
				|| counters[counter_index].overflowed != 0u
				|| trace.cumulative_sink.entries[counter_index].overflowed != 0u;
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

static void slice_project_merge_dsm_cycle_noexcept(
		slice_dsm_header* header, ParallelContext* pcxt,
		slice_trace_attachment* attachment,
		std::uint64_t expected_cycle) noexcept {
	if (!slice_trace_attachment_live(attachment)
		|| header == nullptr || pcxt == nullptr || pcxt->seg == nullptr
		|| pcxt->nworkers < 0) return;
	slice_project_merge_dsm_cycle_noexcept(
		header,
		reinterpret_cast<std::uintptr_t>(dsm_segment_address(pcxt->seg)),
		dsm_segment_map_length(pcxt->seg),
		static_cast<std::uint32_t>(pcxt->nworkers), attachment,
		expected_cycle, nullptr);
}

#ifdef BIOSEQ_TEST_INDEXES
enum class SliceProjectDsmCorruption : std::uint8_t {
	version = 0u,
	recipe_count,
	header_offset,
	truncated_mapping,
	lifecycle,
	stale_cycle,
};

struct SliceProjectDsmTestCaseResult {
	bool validated = false;
	SliceProjectDsmMergeAudit audit{};
};

static void slice_project_initialize_test_dsm(
		slice_dsm_header* header, SliceProjectDsmLayout const& layout,
		std::uint32_t recipe_count, std::uint32_t slot_count,
		std::uint64_t cycle) noexcept {
	header->magic = kBioseqSliceDsmMagic;
	header->codec_version = kBioseqSliceDsmVersion;
	header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
	header->recipe_count = recipe_count;
	header->slot_count = slot_count;
	header->cycle = cycle;
	header->slot_stride = layout.slot_stride;
}

[[nodiscard]] static SliceProjectDsmTestCaseResult
slice_project_run_corrupt_dsm_test(
		SliceProjectRuntime const& source,
		SliceProjectDsmCorruption corruption) {
	SliceProjectDsmTestCaseResult result{};
	if (source.boundary_count == 0u || source.boundaries == nullptr) return result;
	constexpr std::uint32_t slot_count = 2u;
	constexpr std::uint64_t cycle = 17u;
	const SliceProjectDsmLayout layout = slice_project_dsm_layout(
		source.boundary_count, slot_count);
	auto* bytes = static_cast<std::byte*>(palloc0(layout.total_bytes));
	auto* counters = static_cast<slice_dsm_counter*>(palloc0(
		mul_size(source.boundary_count, sizeof(slice_dsm_counter))));
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
	slice_project_initialize_test_dsm(
		header, layout, source.boundary_count, slot_count, cycle);
	auto* worker = slice_project_worker_slot(header, 0u);
	worker->cycle = cycle;
	worker->publish_state = slice_dsm_publish_state::published_complete;
	auto* worker_counters = slice_project_worker_counters(worker);
	worker_counters[0].calls = 99u;
	Size mapped_length = layout.total_bytes;
	switch (corruption) {
		case SliceProjectDsmCorruption::version:
			++header->codec_version;
			break;
		case SliceProjectDsmCorruption::recipe_count:
			++header->recipe_count;
			break;
		case SliceProjectDsmCorruption::header_offset:
			header->header_bytes = static_cast<std::uint16_t>(
				header->header_bytes + MAXIMUM_ALIGNOF);
			break;
		case SliceProjectDsmCorruption::truncated_mapping:
			--mapped_length;
			break;
		case SliceProjectDsmCorruption::lifecycle:
			worker->publish_state = static_cast<slice_dsm_publish_state>(255u);
			break;
		case SliceProjectDsmCorruption::stale_cycle:
			worker->cycle = cycle - 1u;
			break;
	}
	slice_project_merge_dsm_cycle_noexcept(
		header, reinterpret_cast<std::uintptr_t>(bytes), mapped_length,
		slot_count, &attachment, cycle, &result.audit);
	const bool zero_payload_reads = result.audit.counter_records_read == 0u;
	const bool no_guessed_workers = trace.incomplete_workers == 0u;
	const bool no_prefix_merge = trace.last_merged_cycle == 0u
		&& counters[0].calls == 0u;
	const bool rejected_before_slot =
		corruption == SliceProjectDsmCorruption::version
		|| corruption == SliceProjectDsmCorruption::recipe_count
		|| corruption == SliceProjectDsmCorruption::header_offset
		|| corruption == SliceProjectDsmCorruption::truncated_mapping;
	result.validated = trace.invalid && trace.overflowed
		&& zero_payload_reads && no_guessed_workers && no_prefix_merge
		&& (!rejected_before_slot || result.audit.slot_headers_read == 0u);
	pfree(counters);
	pfree(bytes);
	return result;
}

struct SliceProjectAsyncTestCallbacks {
	std::uint32_t cancelled = 0u;
	std::uint32_t joined = 0u;
	std::uint32_t backings_released = 0u;
};

static void slice_project_test_cancel(void* argument) noexcept {
	++static_cast<SliceProjectAsyncTestCallbacks*>(argument)->cancelled;
}

static void slice_project_test_join(void* argument) noexcept {
	++static_cast<SliceProjectAsyncTestCallbacks*>(argument)->joined;
}

static void slice_project_test_release_backing(void* argument) noexcept {
	++static_cast<SliceProjectAsyncTestCallbacks*>(argument)->backings_released;
}

[[nodiscard]] static bool slice_project_run_incomplete_async_test(
		SliceProjectRuntime const& source,
		std::uint32_t* cleanup_callbacks) {
	if (cleanup_callbacks != nullptr) *cleanup_callbacks = 0u;
	if (source.boundary_count == 0u || source.boundaries == nullptr) return false;
	slice_materialization_boundary boundary = source.boundaries[0];
	boundary.counter_index = 0u;
	slice_dsm_counter cumulative{};
	slice_materialization_runtime trace{};
	if (!initialize_slice_materialization_runtime_noexcept(
			&trace, &boundary, 1u, &cumulative, 1u, true, false)) return false;
	auto* attachment = make_slice_trace_attachment(&trace);
	slice_async_owner_guard guard{};
	slice_async_job_slot jobs[slice_async_max_live_jobs]{};
	slice_dsm_counter job_counters[slice_async_max_live_jobs]{};
	if (!initialize_slice_async_owner_noexcept(
			&guard, &trace, attachment, jobs, slice_async_max_live_jobs,
			job_counters, 1u)) {
		invalidate_slice_trace_attachment_noexcept(attachment);
		release_slice_trace_attachment(attachment);
		return false;
	}
	SliceProjectAsyncTestCallbacks callbacks{};
	const slice_async_job_resources resources{
		.cancel = {&callbacks, &slice_project_test_cancel},
		.join = {&callbacks, &slice_project_test_join},
		.backings = {&callbacks, &slice_project_test_release_backing},
	};
	slice_async_completion_handle handle{};
	bool incomplete = false;
	const bool enrolled = enroll_slice_async_job_noexcept(
		&guard, resources, &handle);
	const bool scheduled = enrolled
		&& mark_slice_async_job_scheduled_noexcept(&guard, handle);
	if (scheduled) {
		update_slice_dsm_counter_noexcept(
			guard.jobs[handle.slot_index].counters[0],
			1u, 1u, 8u, false, 8u, 0u);
	}
	const bool drained = scheduled && drain_slice_async_job_noexcept(
		&guard, handle, &incomplete);
	const bool stale_rejected = !mark_slice_async_job_finished_noexcept(
		&guard, handle);
	const bool lifecycle_valid = drained && incomplete && stale_rejected
		&& guard.enrolled_jobs == 0u
		&& jobs[handle.slot_index].state == slice_async_job_state::unused
		&& jobs[handle.slot_index].generation == handle.generation + 1u
		&& cumulative.calls == 1u
		&& callbacks.cancelled == 1u && callbacks.joined == 1u
		&& callbacks.backings_released == 1u
		&& attachment->refcount == 1u;
	if (cleanup_callbacks != nullptr) {
		*cleanup_callbacks = callbacks.cancelled + callbacks.joined
			+ callbacks.backings_released;
	}
	finish_slice_async_owner_noexcept(
		&guard, slice_shutdown_disposition::incomplete);
	return lifecycle_valid
		&& guard.phase == slice_async_owner_phase::terminal
		&& trace.boundaries == nullptr && trace.cumulative_sink.entries == nullptr;
}

static void slice_project_apply_dsm_test_fixture(
		BioseqSliceProjectState* state) {
	if (state == nullptr || BioseqSliceDsmTestFixture == kSliceDsmTestNone) return;
	auto& runtime = state->runtime;
	if (runtime.dsm_test_fixture_applied) return;
	runtime.dsm_test_fixture_applied = true;
	runtime.dsm_test_fixture_mode = BioseqSliceDsmTestFixture;
	runtime.dsm_test_async_validated = slice_project_run_incomplete_async_test(
		runtime, &runtime.dsm_test_cleanup_callbacks);
	if (BioseqSliceDsmTestFixture == kSliceDsmTestMalformed) {
		constexpr std::array corruptions{
			SliceProjectDsmCorruption::version,
			SliceProjectDsmCorruption::recipe_count,
			SliceProjectDsmCorruption::header_offset,
			SliceProjectDsmCorruption::truncated_mapping,
			SliceProjectDsmCorruption::lifecycle,
			SliceProjectDsmCorruption::stale_cycle,
		};
		bool valid = true;
		for (const auto corruption : corruptions) {
			const auto result = slice_project_run_corrupt_dsm_test(
				runtime, corruption);
			valid = valid && result.validated;
			++runtime.dsm_test_checks;
			runtime.dsm_test_slot_headers_read +=
				result.audit.slot_headers_read;
			runtime.dsm_test_counter_records_read +=
				result.audit.counter_records_read;
		}
		runtime.dsm_test_fixture_validated = valid
			&& runtime.dsm_test_async_validated;
		runtime.trace_runtime.invalid = true;
		runtime.trace_runtime.overflowed = true;
		return;
	}

	constexpr std::uint32_t slot_count = 3u;
	constexpr std::uint64_t cycle = 23u;
	const SliceProjectDsmLayout layout = slice_project_dsm_layout(
		runtime.boundary_count, slot_count);
	auto* bytes = static_cast<std::byte*>(palloc0(layout.total_bytes));
	auto* local_counters = static_cast<slice_dsm_counter*>(palloc0(
		mul_size(runtime.boundary_count, sizeof(slice_dsm_counter))));
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
	slice_trace_attachment attachment{
		.refcount = 1u, .generation = 1u, .runtime = &trace,
		.async_owner = nullptr, .async_owner_generation = 0u, .valid = true};
	auto* header = reinterpret_cast<slice_dsm_header*>(bytes);
	slice_project_initialize_test_dsm(
		header, layout, runtime.boundary_count, slot_count, cycle);
	for (std::uint32_t slot_index = 0u; slot_index < 2u; ++slot_index) {
		auto* worker = slice_project_worker_slot(header, slot_index);
		worker->cycle = cycle;
		worker->publish_state = slot_index == 0u
			? slice_dsm_publish_state::published_complete
			: slice_dsm_publish_state::attached_running;
		auto* counters = slice_project_worker_counters(worker);
		for (std::uint32_t index = 0u;
			 index < runtime.boundary_count; ++index) {
			counters[index].calls = slot_index == 0u ? 3u : 2u;
			counters[index].bytes = slot_index == 0u ? 30u : 20u;
		}
	}
	SliceProjectDsmMergeAudit audit{};
	slice_project_merge_dsm_cycle_noexcept(
		header, reinterpret_cast<std::uintptr_t>(bytes), layout.total_bytes,
		slot_count, &attachment, cycle, &audit);
	bool valid = audit.header_valid && audit.mapping_valid && audit.merged
		&& audit.merged_slots == 2u && audit.incomplete_slots == 1u
		&& trace.last_merged_cycle == cycle
		&& trace.incomplete_workers == 1u && !trace.invalid
		&& !trace.overflowed;
	for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
		valid = valid && local_counters[index].calls == 5u
			&& local_counters[index].bytes == 50u;
		if (runtime.counters != nullptr) {
			merge_slice_dsm_counter_noexcept(
				runtime.counters[index], local_counters[index]);
		}
	}
	if (runtime.trace_runtime.incomplete_workers == UINT32_MAX) {
		runtime.trace_runtime.overflowed = true;
	} else {
		++runtime.trace_runtime.incomplete_workers;
	}
	runtime.dsm_test_checks = 1u;
	runtime.dsm_test_slot_headers_read = audit.slot_headers_read;
	runtime.dsm_test_counter_records_read = audit.counter_records_read;
	runtime.dsm_test_fixture_validated = valid
		&& runtime.dsm_test_async_validated;
	pfree(local_counters);
	pfree(bytes);
}
#endif

static void slice_project_release_detach_state(
		slice_dsm_detach_state* state) noexcept {
	if (state != nullptr && state->refcount != 0u
		&& state->refcount != UINT32_MAX && --state->refcount == 0u) pfree(state);
}

static void slice_project_owner_reset(void* argument) noexcept {
	auto* state = static_cast<BioseqSliceProjectState*>(argument);
	if (state == nullptr) return;
	auto& runtime = state->runtime;
	if (runtime.owner_initialized) {
		finish_slice_async_owner_noexcept(
			&runtime.owner_guard, slice_shutdown_disposition::incomplete);
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
static void slice_project_source_routes_reset(void* argument) noexcept {
	auto* state = static_cast<BioseqSliceProjectState*>(argument);
	if (state == nullptr) return;
	auto& runtime = state->runtime;
	if (runtime.source_routes_acquired) {
		bioseq::storage::release_pg_sequence_source_routes_noexcept(
			runtime.source_routes);
		runtime.source_routes_acquired = false;
	}
}

static void slice_project_dsm_on_detach(
		dsm_segment*, Datum argument) noexcept {
	auto* state = reinterpret_cast<slice_dsm_detach_state*>(
		DatumGetPointer(argument));
	if (state == nullptr || !state->callback_armed) return;
	state->callback_armed = false;
	slice_project_merge_dsm_cycle_noexcept(
		state->mapped_header, state->pcxt, state->attachment,
		state->expected_cycle);
	auto* attachment = state->attachment;
	state->attachment = nullptr;
	state->pcxt = nullptr;
	state->mapped_header = nullptr;
	release_slice_trace_attachment(attachment);
	slice_project_release_detach_state(state);
}

static void slice_project_publish_worker_noexcept(
		BioseqSliceProjectState* state,
		slice_dsm_publish_state final_state) noexcept {
	if (state == nullptr) return;
	auto& runtime = state->runtime;
	auto* worker = runtime.mapped_worker;
	if (worker == nullptr || runtime.mapped_cycle == 0u) return;
	if (worker->publish_state != slice_dsm_publish_state::attached_running
		|| worker->cycle != runtime.mapped_cycle) {
		slice_project_mark_trace_invalid_noexcept(runtime.attachment);
		return;
	}
	auto* destination = slice_project_worker_counters(worker);
	bool overflowed = runtime.trace_runtime.invalid
		|| runtime.trace_runtime.overflowed || worker->overflowed != 0u;
	for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
		if (!slice_dsm_counter_reserved_valid(destination[index])
			|| !slice_dsm_counter_reserved_valid(runtime.counters[index])) {
			overflowed = true;
		} else {
			/* The shared slot is cumulative for one parallel cycle, whereas the
			 * backend-local sink is a delta since the preceding publish/rescan. */
			merge_slice_dsm_counter_noexcept(
				destination[index], runtime.counters[index]);
		}
		overflowed = overflowed || destination[index].overflowed != 0u
			|| !slice_dsm_counter_reserved_valid(destination[index]);
	}
	worker->overflowed = overflowed ? 1u : 0u;
	pg_write_barrier();
	worker->publish_state = overflowed
		? slice_dsm_publish_state::published_incomplete : final_state;
	/* A worker publishes one cycle delta.  Only after the shared slot is fully
	 * visible may its backend-local sink be cleared for a subsequent rescan;
	 * otherwise ReInitializeDSM would merge cumulative prefixes twice. */
	for (std::uint32_t index = 0u; index < runtime.boundary_count; ++index) {
		reset_slice_dsm_counter_noexcept(runtime.counters[index]);
	}
}

[[nodiscard]] static bool slice_project_rearm_worker_for_rescan_noexcept(
		BioseqSliceProjectState* state) noexcept {
	if (state == nullptr) return false;
	auto& runtime = state->runtime;
	auto* worker = runtime.mapped_worker;
	if (worker == nullptr) return true;
	if (runtime.mapped_cycle == 0u || worker->cycle != runtime.mapped_cycle
		|| (worker->publish_state
			!= slice_dsm_publish_state::published_complete
			&& worker->publish_state
				!= slice_dsm_publish_state::published_incomplete)) {
		slice_project_mark_trace_invalid_noexcept(runtime.attachment);
		return false;
	}
	/* The preceding publish made every cumulative counter visible.  Retain
	 * those values and reopen only the state byte; the next publish merges a
	 * fresh backend-local delta instead of overwriting earlier rescans. */
	pg_write_barrier();
	worker->publish_state = slice_dsm_publish_state::attached_running;
	return true;
}

static void slice_project_publish_complete_noexcept(void* argument) noexcept {
	slice_project_publish_worker_noexcept(
		static_cast<BioseqSliceProjectState*>(argument),
		slice_dsm_publish_state::published_complete);
}

static void slice_project_publish_incomplete_noexcept(void* argument) noexcept {
	slice_project_publish_worker_noexcept(
		static_cast<BioseqSliceProjectState*>(argument),
		slice_dsm_publish_state::published_incomplete);
}

static void slice_project_attach_sources_from_plan_state(
		PlanState* child, BioseqSliceProjectState* state,
		std::uint32_t& tagged_sources) {
	if (child == nullptr || state == nullptr) return;
	if (IsA(child, CustomScanState)) {
		auto* child_custom = reinterpret_cast<CustomScanState*>(child);
		if (child_custom->methods == &BioseqColumnScanExecuteMethods) {
			auto* bioseq_state =
				BioseqScanState::from_custom_scan_state(child_custom);
			BioseqScanRuntime& child_runtime = bioseq_runtime(bioseq_state);
			if (!child_runtime.lazy_source_projection) return;
			++tagged_sources;
			if (child_runtime.lazy_source_provider == nullptr
				|| !bioseq::storage::add_pg_sequence_source_route(
					state->runtime.source_routes,
					child_runtime.lazy_source_provider)) {
				ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("BioseqSliceProject source route is unavailable")));
			}
			state->runtime.source_routes_acquired = true;
			if (child_runtime.lazy_trace_attachment != nullptr) {
				release_slice_trace_attachment(
					child_runtime.lazy_trace_attachment);
				child_runtime.lazy_trace_attachment = nullptr;
			}
			if (slice_trace_attachment_live(state->runtime.attachment)) {
				child_runtime.lazy_trace_attachment = state->runtime.attachment;
				retain_slice_trace_attachment(
					child_runtime.lazy_trace_attachment);
				child_runtime.buffer.bind_slice_async_owner(
					&state->runtime.owner_guard);
			}
			return;
		}
		for (PlanState* custom_child : PList<PlanState>(
				child_custom->custom_ps)) {
			slice_project_attach_sources_from_plan_state(
				custom_child, state, tagged_sources);
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
		slice_project_attach_sources_from_plan_state(
			outer, state, tagged_sources);
	}
	if (inner != nullptr && inner != outer) {
		slice_project_attach_sources_from_plan_state(
			inner, state, tagged_sources);
	}
}

static void slice_project_attach_trace_to_child(
		CustomScanState* custom, BioseqSliceProjectState* state) {
	if (custom == nullptr || custom->custom_ps == NIL || state == nullptr) return;
	std::uint32_t tagged_sources = 0u;
	for (PlanState* child : PList<PlanState>(custom->custom_ps)) {
		slice_project_attach_sources_from_plan_state(
			child, state, tagged_sources);
	}
	if (tagged_sources == 0u) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject child did not accept source ownership")));
	}
}

static void slice_project_initialize_trace(
		BioseqSliceProjectState* state, CustomScan* scan, EState* estate) {
	auto& runtime = state->runtime;
	List* records = slice_project_plan_records(scan);
	List* output_bindings = slice_project_plan_output_bindings(scan);
	runtime.boundary_count = static_cast<std::uint32_t>(list_length(records));
	runtime.output_binding_count = static_cast<std::uint32_t>(
		list_length(output_bindings));
	runtime.collect_actual = runtime.boundary_count != 0u
		&& state->custom_scanstate.ss.ps.instrument != nullptr;
	runtime.timing = runtime.collect_actual
		&& state->custom_scanstate.ss.ps.instrument->need_timer;
	if (runtime.boundary_count != 0u) {
		runtime.boundaries = static_cast<slice_materialization_boundary*>(
			MemoryContextAllocZero(estate->es_query_cxt,
				mul_size(runtime.boundary_count,
					sizeof(slice_materialization_boundary))));
		runtime.counters = static_cast<slice_dsm_counter*>(
			MemoryContextAllocZero(estate->es_query_cxt,
				mul_size(runtime.boundary_count, sizeof(slice_dsm_counter))));
	}
	std::uint32_t index = 0u;
	for (Node* raw_record : PList<Node>(records)) {
		List* record = reinterpret_cast<List*>(raw_record);
		runtime.boundaries[index] = {
			.boundary_id = static_cast<std::uint32_t>(intVal(
				list_nth(record, kSliceRecordBoundaryId))),
			.producer_value_id = static_cast<std::uint32_t>(intVal(
				list_nth(record, kSliceRecordProducerId))),
			.counter_index = index,
			.mechanism = static_cast<materialization_mechanism>(intVal(
				list_nth(record, kSliceRecordMechanism))),
			.reason = static_cast<materialization_reason>(intVal(
				list_nth(record, kSliceRecordReason))),
			.reserved = {},
		};
		++index;
	}
	if (!initialize_slice_materialization_runtime_noexcept(
			&runtime.trace_runtime, runtime.boundaries,
			runtime.boundary_count, runtime.counters,
			runtime.boundary_count, runtime.collect_actual, runtime.timing)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject trace runtime is invalid")));
	}
	runtime.attachment = make_slice_trace_attachment(&runtime.trace_runtime);
	runtime.async_jobs = static_cast<slice_async_job_slot*>(
		MemoryContextAllocZero(estate->es_query_cxt,
			mul_size(slice_async_max_live_jobs,
				sizeof(slice_async_job_slot))));
	if (runtime.boundary_count != 0u) {
		const Size async_counter_count = mul_size(
			static_cast<Size>(slice_async_max_live_jobs),
			static_cast<Size>(runtime.boundary_count));
		runtime.async_job_counters = static_cast<slice_dsm_counter*>(
			MemoryContextAllocZero(estate->es_query_cxt,
				mul_size(async_counter_count,
					sizeof(slice_dsm_counter))));
	}
	if (!initialize_slice_async_owner_noexcept(
			&runtime.owner_guard, &runtime.trace_runtime,
			runtime.attachment, runtime.async_jobs,
			slice_async_max_live_jobs, runtime.async_job_counters,
			runtime.boundary_count)) {
		invalidate_slice_trace_attachment_noexcept(runtime.attachment);
		release_slice_trace_attachment(runtime.attachment);
		runtime.attachment = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqSliceProject owner initialization failed")));
	}
	runtime.owner_guard.publish_complete = {
		.argument = state, .invoke = slice_project_publish_complete_noexcept};
	runtime.owner_guard.publish_incomplete = {
		.argument = state, .invoke = slice_project_publish_incomplete_noexcept};
	runtime.owner_initialized = true;
	runtime.owner_guard.callback.func = slice_project_owner_reset;
	runtime.owner_guard.callback.arg = state;
	MemoryContextRegisterResetCallback(
		estate->es_query_cxt, &runtime.owner_guard.callback);
	if (runtime.output_binding_count != 0u) {
		runtime.output_bindings = static_cast<SliceProjectOutputBinding*>(
			MemoryContextAllocZero(estate->es_query_cxt,
				mul_size(runtime.output_binding_count,
					sizeof(SliceProjectOutputBinding))));
		index = 0u;
		for (Node* raw_binding : PList<Node>(output_bindings)) {
			List* binding = reinterpret_cast<List*>(raw_binding);
			runtime.output_bindings[index++] = {
				.resno = static_cast<AttrNumber>(intVal(linitial(binding))),
				.producer_value_id = static_cast<std::uint32_t>(
					intVal(lsecond(binding))),
			};
		}
	}
}

static void slice_project_ensure_trace_initialized(CustomScanState* custom) {
	if (custom == nullptr) return;
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	auto& runtime = state->runtime;
	if (runtime.trace_initialization_attempted) return;
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

static Size SliceProjectEstimateDSM(
		CustomScanState* custom, ParallelContext* pcxt) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	slice_project_ensure_trace_initialized(custom);
	if (!state->runtime.collect_actual) return 0u;
	if (pcxt == nullptr || pcxt->nworkers < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject parallel context is invalid")));
	}
	return slice_project_dsm_layout(
		state->runtime.boundary_count,
		static_cast<std::uint32_t>(pcxt->nworkers)).total_bytes;
}

static void SliceProjectInitializeDSM(
		CustomScanState* custom, ParallelContext* pcxt, void* coordinate) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	slice_project_ensure_trace_initialized(custom);
	auto& runtime = state->runtime;
	if (!runtime.collect_actual) return;
	if (coordinate == nullptr || pcxt == nullptr || pcxt->seg == nullptr
		|| pcxt->nworkers < 0 || runtime.detach_state != nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject DSM initialization is invalid")));
	}
	const auto slot_count = static_cast<std::uint32_t>(pcxt->nworkers);
	const SliceProjectDsmLayout layout = slice_project_dsm_layout(
		runtime.boundary_count, slot_count);
	std::memset(coordinate, 0, layout.total_bytes);
	auto* header = static_cast<slice_dsm_header*>(coordinate);
	header->magic = kBioseqSliceDsmMagic;
	header->codec_version = kBioseqSliceDsmVersion;
	header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
	header->recipe_count = runtime.boundary_count;
	header->slot_count = slot_count;
	header->cycle = 1u;
	header->slot_stride = layout.slot_stride;
	runtime.mapped_header = header;
	runtime.mapped_cycle = 1u;
	auto* detach = static_cast<slice_dsm_detach_state*>(
		MemoryContextAllocZero(TopMemoryContext,
			sizeof(slice_dsm_detach_state)));
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
	on_dsm_detach(pcxt->seg, slice_project_dsm_on_detach,
		PointerGetDatum(detach));
	retain_slice_trace_attachment(detach->attachment);
	detach->refcount = 2u;
	detach->callback_armed = true;
}

static void SliceProjectReInitializeDSM(
		CustomScanState* custom, ParallelContext* pcxt, void* coordinate) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	slice_project_ensure_trace_initialized(custom);
	auto& runtime = state->runtime;
	if (!runtime.collect_actual) return;
	if (coordinate == nullptr || pcxt == nullptr || pcxt->seg == nullptr
		|| pcxt->nworkers < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject DSM reinitialization is invalid")));
	}
	auto* header = static_cast<slice_dsm_header*>(coordinate);
	const std::uint64_t old_cycle = header->cycle;
	slice_project_merge_dsm_cycle_noexcept(
		header, pcxt, runtime.attachment, old_cycle);
	if (old_cycle == UINT64_MAX) {
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("BioseqSliceProject parallel cycle exhausted")));
	}
	const auto slot_count = static_cast<std::uint32_t>(pcxt->nworkers);
	const SliceProjectDsmLayout layout = slice_project_dsm_layout(
		runtime.boundary_count, slot_count);
	std::memset(reinterpret_cast<char*>(header) + layout.header_bytes, 0,
		layout.total_bytes - layout.header_bytes);
	header->magic = kBioseqSliceDsmMagic;
	header->codec_version = kBioseqSliceDsmVersion;
	header->header_bytes = static_cast<std::uint16_t>(layout.header_bytes);
	header->recipe_count = runtime.boundary_count;
	header->slot_count = slot_count;
	header->slot_stride = layout.slot_stride;
	pg_write_barrier();
	header->cycle = old_cycle + 1u;
	runtime.mapped_header = header;
	runtime.mapped_cycle = header->cycle;
	if (runtime.detach_state != nullptr) {
		runtime.detach_state->mapped_header = header;
		pg_write_barrier();
		runtime.detach_state->expected_cycle = header->cycle;
	}
	if (runtime.owner_initialized
		&& runtime.owner_guard.phase == slice_async_owner_phase::quiesced) {
		(void)rearm_slice_async_owner_for_rescan_noexcept(
			&runtime.owner_guard);
	}
}

static void SliceProjectInitializeWorker(
		CustomScanState* custom, shm_toc*, void* coordinate) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	slice_project_ensure_trace_initialized(custom);
	auto& runtime = state->runtime;
	if (!runtime.collect_actual) return;
	if (coordinate == nullptr || ParallelWorkerNumber < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject worker coordinate is invalid")));
	}
	auto* header = static_cast<slice_dsm_header*>(coordinate);
	if (header->slot_count > static_cast<std::uint32_t>(INT_MAX)
		|| ParallelWorkerNumber >= static_cast<int>(header->slot_count)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject worker slot is out of range")));
	}
	SliceProjectDsmLayout layout{};
	if (!slice_project_validate_dsm_header_noexcept(
			header, runtime.boundary_count, header->slot_count,
			header->cycle, &layout)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject worker DSM header is invalid")));
	}
	auto* worker = slice_project_worker_slot(
		header, static_cast<std::uint32_t>(ParallelWorkerNumber));
	if (worker->publish_state != slice_dsm_publish_state::never_launched
		|| worker->cycle != 0u
		|| std::any_of(worker->reserved.begin(), worker->reserved.end(),
			[](std::byte byte) { return byte != std::byte{0}; })) {
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

[[nodiscard]] static TupleTableSlot* slice_project_publish_child(
		CustomScanState* custom, bool count_row) {
	if (custom->custom_ps == NIL || list_length(custom->custom_ps) != 1) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject executor child is missing")));
	}
	auto* child = reinterpret_cast<PlanState*>(linitial(custom->custom_ps));
	TupleTableSlot* child_slot = ExecProcNode(child);
	TupleTableSlot* scan_slot = custom->ss.ss_ScanTupleSlot;
	if (TupIsNull(child_slot)) return ExecClearTuple(scan_slot);
	slot_getallattrs(child_slot);
	const int natts = child_slot->tts_tupleDescriptor->natts;
	if (natts != scan_slot->tts_tupleDescriptor->natts) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqSliceProject child descriptor changed")));
	}
	ExecClearTuple(scan_slot);
	std::memcpy(scan_slot->tts_values, child_slot->tts_values,
		static_cast<std::size_t>(natts) * sizeof(Datum));
	std::memcpy(scan_slot->tts_isnull, child_slot->tts_isnull,
		static_cast<std::size_t>(natts) * sizeof(bool));
	scan_slot->tts_nvalid = natts;
	TupleDesc descriptor = scan_slot->tts_tupleDescriptor;
	for (int index = 0; index < natts; ++index) {
		if (!scan_slot->tts_isnull[index]) {
			scan_slot->tts_values[index] = MakeExpandedObjectReadOnly(
				scan_slot->tts_values[index], false,
				TupleDescAttr(descriptor, index)->attlen);
		}
	}
	ExecStoreVirtualTuple(scan_slot);
	if (count_row) {
		auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
		state->runtime.rows_published = bioseq::parallelism::saturating_add(
			state->runtime.rows_published, 1u);
	}
	return scan_slot;
}

[[nodiscard]] static TupleTableSlot* SliceProjectAccess(ScanState* scan) {
	return slice_project_publish_child(
		reinterpret_cast<CustomScanState*>(scan), true);
}

[[nodiscard]] static bool SliceProjectRecheck(
		ScanState* scan, TupleTableSlot*) {
	CustomScanState* custom = reinterpret_cast<CustomScanState*>(scan);
	if (custom->ss.ps.state == nullptr
		|| custom->ss.ps.state->es_epq_active == nullptr) return true;
	return !TupIsNull(slice_project_publish_child(custom, false));
}

static void SliceProjectBegin(
		CustomScanState* custom, EState* estate, int eflags) {
	auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
	scan->methods = &bioseq_slice_project_scan_methods();
	slice_project_validate_codec(scan);
	if (list_length(scan->custom_plans) != 1) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqSliceProject requires exactly one child plan")));
	}
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	List* header = slice_project_plan_header(scan);
	state->runtime.planned_symbols = slice_project_int8_value(
		reinterpret_cast<Node*>(list_nth(header, kSliceHeaderPlannedSymbols)));
	state->runtime.planned_bytes = slice_project_int8_value(
		reinterpret_cast<Node*>(list_nth(header, kSliceHeaderPlannedBytes)));
	/* Register the upper-lease reset callback before ExecInitNode registers
	 * any child/provider callbacks.  Their reverse-order teardown then closes
	 * providers before this callback drops the independently retained routes. */
	state->runtime.source_routes_callback.func =
		slice_project_source_routes_reset;
	state->runtime.source_routes_callback.arg = state;
	MemoryContextRegisterResetCallback(
		estate->es_query_cxt,
		&state->runtime.source_routes_callback);
	state->runtime.source_routes_callback_registered = true;
	Plan* child_plan = reinterpret_cast<Plan*>(linitial(scan->custom_plans));
	custom->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
}

[[nodiscard]] static TupleTableSlot* SliceProjectExec(
		CustomScanState* custom) {
	slice_project_ensure_trace_initialized(custom);
	TupleTableSlot* result = ExecScan(
		&custom->ss, SliceProjectAccess, SliceProjectRecheck);
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	auto& runtime = state->runtime;
	if (TupIsNull(result) || !slice_trace_attachment_live(runtime.attachment)
		|| runtime.output_binding_count == 0u) return result;
	slot_getallattrs(result);
	const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
	for (std::uint32_t index = 0u;
		 index < runtime.output_binding_count; ++index) {
		auto const& binding = runtime.output_bindings[index];
		if (binding.resno <= 0
			|| binding.resno > result->tts_tupleDescriptor->natts
			|| TupleDescAttr(result->tts_tupleDescriptor,
				binding.resno - 1)->atttypid != seq_oid) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject result binding changed")));
		}
		if (result->tts_isnull[binding.resno - 1]) continue;
		/* CASE/COALESCE may select a flat or literal arm even though another
		 * arm caused this symbolic recipe to be planned.  Only an expanded
		 * result can carry provenance; a skipped lazy arm is not an error and
		 * must retain zero actual boundary calls. */
		if (!VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(
				result->tts_values[binding.resno - 1]))) continue;
		if (!attach_slice_trace_provenance(
				result->tts_values[binding.resno - 1], runtime.attachment,
				binding.producer_value_id)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject result lost expanded provenance")));
		}
	}
	return result;
}

static void SliceProjectEnd(CustomScanState* custom) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	/* No source-backed task may outlive the owning upper executor. */
	if (state->runtime.owner_initialized) {
		finish_slice_async_owner_noexcept(
			&state->runtime.owner_guard,
			slice_shutdown_disposition::complete);
		state->runtime.attachment = nullptr;
	}
	if (custom->custom_ps != NIL) {
		ExecEndNode(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
		custom->custom_ps = NIL;
	}
	ExecClearTuple(custom->ss.ss_ScanTupleSlot);
	/* Child End closes every provider first.  The independent upper leases
	 * keep registries alive until this final release/sweep. */
	if (state->runtime.source_routes_acquired) {
		bioseq::storage::release_pg_sequence_source_routes(
			state->runtime.source_routes, /*is_commit=*/true);
		state->runtime.source_routes_acquired = false;
	}
	if (state->runtime.detach_state != nullptr) {
		slice_project_release_detach_state(state->runtime.detach_state);
		state->runtime.detach_state = nullptr;
	}
	state->runtime.mapped_header = nullptr;
	state->runtime.mapped_worker = nullptr;
}

static void SliceProjectReScan(CustomScanState* custom) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	slice_project_ensure_trace_initialized(custom);
	if (state->runtime.owner_initialized) {
		quiesce_slice_async_owner_noexcept(
			&state->runtime.owner_guard,
			slice_shutdown_disposition::complete);
	}
	if (custom->custom_ps != NIL) {
		ExecReScan(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
	}
	if (state->runtime.source_routes_acquired) {
		for (std::uint8_t index = 0u;
			 index < state->runtime.source_routes.count; ++index) {
			bioseq::storage::sweep_reclaimable_pg_sequence_source_domains(
				state->runtime.source_routes.leases[index],
				bioseq::storage::pg_sequence_source_sweep_boundary::rescan,
				/*is_commit=*/true);
		}
	}
	ExecClearTuple(custom->ss.ss_ScanTupleSlot);
	state->runtime.rescans = bioseq::parallelism::saturating_add(
		state->runtime.rescans, 1u);
	const bool worker_rearmed =
		slice_project_rearm_worker_for_rescan_noexcept(state);
	if (state->runtime.owner_initialized
		&& state->runtime.owner_guard.phase
			== slice_async_owner_phase::quiesced
		&& worker_rearmed) {
		(void)rearm_slice_async_owner_for_rescan_noexcept(
			&state->runtime.owner_guard);
	}
}

static void SliceProjectMarkPos(CustomScanState* custom) {
	if (custom->custom_ps != NIL) {
		ExecMarkPos(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
	}
}

static void SliceProjectRestrPos(CustomScanState* custom) {
	if (custom->custom_ps != NIL) {
		ExecRestrPos(reinterpret_cast<PlanState*>(linitial(custom->custom_ps)));
	}
}

[[nodiscard]] static char* slice_project_deparse_expression(
		CustomScan* scan, int expression_index, ExplainState* es) {
	if (expression_index < 0
		|| expression_index >= list_length(scan->custom_exprs)) {
		return pstrdup("<omitted>");
	}
	Node* expression = reinterpret_cast<Node*>(
		list_nth(scan->custom_exprs, expression_index));
	return deparse_expression(expression, es->deparse_cxt, true, false);
}

[[nodiscard]] static const char* slice_project_opcode_name(
		SliceProjectOpcode opcode) noexcept {
	switch (opcode) {
		case SliceProjectOpcode::source_expr: return "source";
		case SliceProjectOpcode::literal_expr: return "literal";
		case SliceProjectOpcode::element: return "element";
		case SliceProjectOpcode::slice: return "slice";
		case SliceProjectOpcode::concat: return "concat";
		case SliceProjectOpcode::reverse: return "reverse";
		case SliceProjectOpcode::reverse_complement:
			return "reverse complement";
		case SliceProjectOpcode::length: return "length";
		case SliceProjectOpcode::cardinality: return "cardinality";
		case SliceProjectOpcode::bridge: return "literal bridge";
		case SliceProjectOpcode::materialize: return "materialize";
	}
	return "invalid";
}

[[nodiscard]] static List* slice_project_node_dependency_names(List* node) {
	List* names = NIL;
	Node* dependency_node = reinterpret_cast<Node*>(
		list_nth(node, kSliceNodeDependencies));
	if (dependency_node == nullptr) return names;
	for (Node* dependency : PList<Node>(
			reinterpret_cast<List*>(dependency_node))) {
		const std::string value = fmt::format(
			"value_{}", intVal(dependency));
		names = lappend(names, pstrdup(value.c_str()));
	}
	return names;
}

[[nodiscard]] static List* slice_project_dependency_names(List* record) {
	List* names = NIL;
	Node* dependency_node = reinterpret_cast<Node*>(
		list_nth(record, kSliceRecordDependencies));
	if (dependency_node == nullptr) return names;
	for (Node* dependency : PList<Node>(
			reinterpret_cast<List*>(dependency_node))) {
		char variable[32]{};
		snprintf(variable, sizeof(variable), "var_%d", intVal(dependency));
		names = lappend(names, pstrdup(variable));
	}
	return names;
}

#ifdef BIOSEQ_TEST_INDEXES
[[nodiscard]] static const char* slice_project_dsm_test_fixture_name(
		int mode) noexcept {
	switch (mode) {
		case kSliceDsmTestIncomplete: return "incomplete";
		case kSliceDsmTestMalformed: return "malformed";
		default: return "none";
	}
}

static void slice_project_explain_dsm_test_fixture(
		BioseqSliceProjectState const* state, ExplainState* es) {
	if (state == nullptr || es == nullptr
		|| !state->runtime.dsm_test_fixture_applied) return;
	auto const& runtime = state->runtime;
	const char* mode = slice_project_dsm_test_fixture_name(
		runtime.dsm_test_fixture_mode);
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
		const auto integer_property = [es](
				const char* name, std::uint32_t value) {
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "%s: %u\n", name, value);
		};
		integer_property("DSM Fixture Checks", runtime.dsm_test_checks);
		integer_property(
			"DSM Fixture Slot Headers Read",
			runtime.dsm_test_slot_headers_read);
		integer_property(
			"DSM Fixture Counter Records Read",
			runtime.dsm_test_counter_records_read);
		integer_property(
			"DSM Async Cleanup Callbacks",
			runtime.dsm_test_cleanup_callbacks);
		return;
	}
	ExplainPropertyText("DSM Test Fixture", mode, es);
	ExplainPropertyBool(
		"DSM Fixture Validated", runtime.dsm_test_fixture_validated, es);
	ExplainPropertyBool(
		"DSM Async Lifecycle Validated",
		runtime.dsm_test_async_validated, es);
	ExplainPropertyUInteger(
		"DSM Fixture Checks", nullptr, runtime.dsm_test_checks, es);
	ExplainPropertyUInteger(
		"DSM Fixture Slot Headers Read", nullptr,
		runtime.dsm_test_slot_headers_read, es);
	ExplainPropertyUInteger(
		"DSM Fixture Counter Records Read", nullptr,
		runtime.dsm_test_counter_records_read, es);
	ExplainPropertyUInteger(
		"DSM Async Cleanup Callbacks", nullptr,
		runtime.dsm_test_cleanup_callbacks, es);
}
#endif

[[nodiscard]] static slice_dsm_counter const& slice_project_explain_counter(
		BioseqSliceProjectState const* state, std::uint32_t index) noexcept {
	static const slice_dsm_counter zero{};
	if (state == nullptr || !state->runtime.collect_actual
		|| state->runtime.counters == nullptr
		|| index >= state->runtime.boundary_count) return zero;
	return state->runtime.counters[index];
}

static void slice_project_explain_text(
		BioseqSliceProjectState* state, CustomScan* scan,
		List* header, List* nodes, List* records, ExplainState* es) {
	appendStringInfoSpaces(es->str, es->indent * 2);
	appendStringInfoString(es->str, "Slice Recipe:\n");
	for (Node* raw_node : PList<Node>(nodes)) {
		List* node = reinterpret_cast<List*>(raw_node);
		const int stable_id = intVal(list_nth(node, kSliceNodeStableId));
		const auto opcode = static_cast<SliceProjectOpcode>(intVal(
			list_nth(node, kSliceNodeOpcode)));
		const int expression_index = intVal(
			list_nth(node, kSliceNodeExpressionIndex));
		appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
		appendStringInfo(es->str, "value_%d [%s] = %s\n", stable_id,
			slice_project_opcode_name(opcode),
			slice_project_deparse_expression(scan, expression_index, es));
		List* dependencies = slice_project_node_dependency_names(node);
		appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
		appendStringInfoString(es->str, "Depends On: ");
		if (dependencies == NIL) {
			appendStringInfoString(es->str, "none");
		} else {
			bool first = true;
			for (char* name : PList<char>(dependencies)) {
				if (!first) appendStringInfoString(es->str, ", ");
				appendStringInfoString(es->str, name);
				first = false;
			}
		}
		appendStringInfoChar(es->str, '\n');
		if (opcode == SliceProjectOpcode::slice
			|| opcode == SliceProjectOpcode::element) {
			const int lower = intVal(
				list_nth(node, kSliceNodeLowerExpressionIndex));
			const int upper = intVal(
				list_nth(node, kSliceNodeUpperExpressionIndex));
			appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
			appendStringInfo(es->str, "Bounds: lower=%s upper=%s\n",
				lower < 0 ? "omitted"
					: slice_project_deparse_expression(scan, lower, es),
				upper < 0 ? "omitted"
					: slice_project_deparse_expression(scan, upper, es));
		}
	}
	appendStringInfoSpaces(es->str, es->indent * 2);
	appendStringInfoString(es->str, "Slice Materialization:\n");
	if (es->analyze && state != nullptr
		&& state->runtime.trace_runtime.incomplete_workers != 0u) {
		appendStringInfoSpaces(es->str, (es->indent + 1) * 2);
		appendStringInfo(es->str, "Incomplete Workers: %u\n",
			state->runtime.trace_runtime.incomplete_workers);
	}
	std::uint32_t counter_index = 0u;
	for (Node* raw_record : PList<Node>(records)) {
		List* record = reinterpret_cast<List*>(raw_record);
		const int boundary_id = intVal(
			list_nth(record, kSliceRecordBoundaryId));
		const int producer_id = intVal(
			list_nth(record, kSliceRecordProducerId));
		const int expression_index = intVal(
			list_nth(record, kSliceRecordExpressionIndex));
		const auto mechanism = static_cast<materialization_mechanism>(intVal(
			list_nth(record, kSliceRecordMechanism)));
		const auto reason = static_cast<materialization_reason>(intVal(
			list_nth(record, kSliceRecordReason)));
		char* expression = slice_project_deparse_expression(
			scan, expression_index, es);
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
			for (char* name : PList<char>(dependencies)) {
				if (!first) appendStringInfoString(&dependency_text, ", ");
				appendStringInfoString(&dependency_text, name);
				first = false;
			}
		}
		property("Depends On", dependency_text.data);
		property("Encoding", strVal(list_nth(record, kSliceRecordEncoding)));
		property("Cache Scope", strVal(
			list_nth(record, kSliceRecordCacheScope)));
		if (es->verbose) {
			char producer[32]{};
			snprintf(producer, sizeof(producer), "%d", producer_id);
			property("Producer Value", producer);
			property("Source Provenance", expression);
		}
		if (es->costs) {
			const auto estimate = slice_project_int8_value(
				reinterpret_cast<Node*>(list_nth(
					record, kSliceRecordEstimatedSymbols)));
			appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
			appendStringInfo(es->str,
				"Estimated Symbols: " UINT64_FORMAT "\n", estimate);
			appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
			appendStringInfo(es->str,
				"Estimated Bytes: " UINT64_FORMAT " bytes\n", estimate);
		}
		if (es->analyze) {
			auto const& counter = slice_project_explain_counter(
				state, counter_index);
			auto actual = [es](const char* name, std::uint64_t value,
					const char* unit = nullptr) {
				appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
				appendStringInfo(es->str, "%s: " UINT64_FORMAT "%s%s\n",
					name, value, unit == nullptr ? "" : " ",
					unit == nullptr ? "" : unit);
			};
			actual("Actual Calls", counter.calls);
			actual("Actual Input Symbols", counter.input_symbols);
			actual("Actual Output Symbols", counter.output_symbols);
			actual("Actual Bytes", counter.bytes, "bytes");
			actual("Reused Calls", counter.reused_calls);
			actual("Peak Retained Bytes", counter.peak_retained_bytes, "bytes");
			const bool overflow = counter.overflowed != 0u
				|| (state != nullptr
					&& (state->runtime.trace_runtime.invalid
						|| state->runtime.trace_runtime.overflowed));
			property("Counter Overflow", overflow ? "true" : "false");
			if (es->timing && state != nullptr && state->runtime.timing) {
				appendStringInfoSpaces(es->str, (es->indent + 2) * 2);
				appendStringInfo(es->str, "Actual Time: %.3f ms\n",
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
				list_nth(header, kSliceHeaderPlannedSymbols))));
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str,
			"Planned Materialized Bytes: " UINT64_FORMAT " bytes\n",
			slice_project_int8_value(reinterpret_cast<Node*>(
				list_nth(header, kSliceHeaderPlannedBytes))));
	}
}

static void slice_project_explain_structured(
		BioseqSliceProjectState* state, CustomScan* scan,
		List* nodes, List* records, ExplainState* es) {
	ExplainOpenGroup("Slice Recipe", "Slice Recipe", false, es);
	for (Node* raw_node : PList<Node>(nodes)) {
		List* node = reinterpret_cast<List*>(raw_node);
		const int stable_id = intVal(list_nth(node, kSliceNodeStableId));
		const auto opcode = static_cast<SliceProjectOpcode>(intVal(
			list_nth(node, kSliceNodeOpcode)));
		const int expression_index = intVal(
			list_nth(node, kSliceNodeExpressionIndex));
		ExplainOpenGroup("Recipe Node", nullptr, true, es);
		ExplainPropertyUInteger("Stable ID", nullptr,
			static_cast<std::uint64_t>(stable_id), es);
		ExplainPropertyText("Opcode", slice_project_opcode_name(opcode), es);
		ExplainPropertyText("Expression", slice_project_deparse_expression(
			scan, expression_index, es), es);
		ExplainPropertyList("Depends On",
			slice_project_node_dependency_names(node), es);
		const int lower = intVal(
			list_nth(node, kSliceNodeLowerExpressionIndex));
		const int upper = intVal(
			list_nth(node, kSliceNodeUpperExpressionIndex));
		if (lower >= 0) {
			ExplainPropertyText("Lower Bound", slice_project_deparse_expression(
				scan, lower, es), es);
		}
		if (upper >= 0) {
			ExplainPropertyText("Upper Bound", slice_project_deparse_expression(
				scan, upper, es), es);
		}
		ExplainCloseGroup("Recipe Node", nullptr, true, es);
	}
	ExplainCloseGroup("Slice Recipe", "Slice Recipe", false, es);
	ExplainOpenGroup(
		"Slice Materialization", "Slice Materialization", false, es);
	if (es->analyze && state != nullptr
		&& state->runtime.trace_runtime.incomplete_workers != 0u) {
		ExplainPropertyUInteger("Incomplete Workers", nullptr,
			state->runtime.trace_runtime.incomplete_workers, es);
	}
	std::uint32_t counter_index = 0u;
	for (Node* raw_record : PList<Node>(records)) {
		List* record = reinterpret_cast<List*>(raw_record);
		const int boundary_id = intVal(
			list_nth(record, kSliceRecordBoundaryId));
		const int producer_id = intVal(
			list_nth(record, kSliceRecordProducerId));
		const int expression_index = intVal(
			list_nth(record, kSliceRecordExpressionIndex));
		const auto mechanism = static_cast<materialization_mechanism>(intVal(
			list_nth(record, kSliceRecordMechanism)));
		const auto reason = static_cast<materialization_reason>(intVal(
			list_nth(record, kSliceRecordReason)));
		char variable[32]{};
		snprintf(variable, sizeof(variable), "var_%d", boundary_id);
		ExplainOpenGroup("Materialization", nullptr, true, es);
		ExplainPropertyText("Variable", variable, es);
		ExplainPropertyText("Expression", slice_project_deparse_expression(
			scan, expression_index, es), es);
		ExplainPropertyText("Mechanism",
			slice_project_mechanism_name(mechanism), es);
		ExplainPropertyText("Reason", slice_project_reason_name(reason), es);
		ExplainPropertyList("Depends On",
			slice_project_dependency_names(record), es);
		ExplainPropertyText("Encoding",
			strVal(list_nth(record, kSliceRecordEncoding)), es);
		ExplainPropertyText("Cache Scope",
			strVal(list_nth(record, kSliceRecordCacheScope)), es);
		if (es->verbose) {
			ExplainPropertyUInteger("Producer Value", nullptr,
				static_cast<std::uint64_t>(producer_id), es);
			ExplainPropertyText("Source Provenance",
				slice_project_deparse_expression(scan, expression_index, es), es);
		}
		if (es->costs) {
			const auto estimate = slice_project_int8_value(
				reinterpret_cast<Node*>(list_nth(
					record, kSliceRecordEstimatedSymbols)));
			ExplainPropertyUInteger(
				"Estimated Symbols", nullptr, estimate, es);
			ExplainPropertyUInteger(
				"Estimated Bytes", "bytes", estimate, es);
		}
		if (es->analyze) {
			auto const& counter = slice_project_explain_counter(
				state, counter_index);
			ExplainPropertyUInteger("Actual Calls", nullptr, counter.calls, es);
			ExplainPropertyUInteger(
				"Actual Input Symbols", nullptr, counter.input_symbols, es);
			ExplainPropertyUInteger(
				"Actual Output Symbols", nullptr, counter.output_symbols, es);
			ExplainPropertyUInteger(
				"Actual Bytes", "bytes", counter.bytes, es);
			ExplainPropertyUInteger(
				"Reused Calls", nullptr, counter.reused_calls, es);
			ExplainPropertyUInteger("Peak Retained Bytes", "bytes",
				counter.peak_retained_bytes, es);
			ExplainPropertyBool("Counter Overflow",
				counter.overflowed != 0u || (state != nullptr
					&& (state->runtime.trace_runtime.invalid
						|| state->runtime.trace_runtime.overflowed)), es);
			if (es->timing && state != nullptr && state->runtime.timing) {
				ExplainPropertyFloat("Actual Time", "ms",
					static_cast<double>(counter.elapsed_ns) / 1.0e6,
					3, es);
			}
		}
		ExplainCloseGroup("Materialization", nullptr, true, es);
		++counter_index;
	}
	ExplainCloseGroup(
		"Slice Materialization", "Slice Materialization", false, es);
}

static void SliceProjectExplain(
		CustomScanState* custom, List*, ExplainState* es) {
	if (es->analyze) slice_project_ensure_trace_initialized(custom);
#ifdef BIOSEQ_TEST_INDEXES
	if (es->analyze) {
		slice_project_apply_dsm_test_fixture(
			reinterpret_cast<BioseqSliceProjectState*>(custom));
	}
#endif
	auto* scan = reinterpret_cast<CustomScan*>(custom->ss.ps.plan);
	List* header = slice_project_plan_header(scan);
	List* nodes = slice_project_plan_nodes(scan);
	List* records = slice_project_plan_records(scan);
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
#ifdef BIOSEQ_TEST_INDEXES
	slice_project_explain_dsm_test_fixture(state, es);
#endif
	if (es->format == EXPLAIN_FORMAT_TEXT) {
		slice_project_explain_text(state, scan, header, nodes, records, es);
		return;
	}
	ExplainPropertyText("Projection Mode", "source-backed lazy", es);
	ExplainPropertyUInteger("Source Pieces", nullptr,
		slice_project_int8_value(reinterpret_cast<Node*>(
			list_nth(header, kSliceHeaderSourcePieces))), es);
	ExplainPropertyUInteger("Estimated Piece Boundaries", nullptr,
		slice_project_int8_value(reinterpret_cast<Node*>(
			list_nth(header, kSliceHeaderPieceBoundaries))), es);
	ExplainPropertyText("Fallback Reason",
		strVal(list_nth(header, kSliceHeaderFallbackReason)), es);
	slice_project_explain_structured(state, scan, nodes, records, es);
}

static void SliceProjectShutdown(CustomScanState* custom) {
	auto* state = reinterpret_cast<BioseqSliceProjectState*>(custom);
	if (state->runtime.owner_initialized) {
		quiesce_slice_async_owner_noexcept(
			&state->runtime.owner_guard,
			slice_shutdown_disposition::complete);
	}
}

[[nodiscard]] static CustomExecMethods const&
bioseq_slice_project_exec_methods() noexcept {
	static_assert(std::is_same_v<decltype(&SliceProjectEstimateDSM),
		Size (*)(CustomScanState*, ParallelContext*)>);
	static_assert(std::is_same_v<decltype(&SliceProjectInitializeDSM),
		void (*)(CustomScanState*, ParallelContext*, void*)>);
	static_assert(std::is_same_v<decltype(&SliceProjectReInitializeDSM),
		void (*)(CustomScanState*, ParallelContext*, void*)>);
	static_assert(std::is_same_v<decltype(&SliceProjectInitializeWorker),
		void (*)(CustomScanState*, shm_toc*, void*)>);
	static_assert(std::is_same_v<decltype(&SliceProjectShutdown),
		void (*)(CustomScanState*)>);
	static_assert(std::is_same_v<decltype(&SliceProjectExplain),
		void (*)(CustomScanState*, List*, ExplainState*)>);
	static const CustomExecMethods methods{
		.CustomName = "BioseqSliceProject",
		.BeginCustomScan = SliceProjectBegin,
		.ExecCustomScan = SliceProjectExec,
		.EndCustomScan = SliceProjectEnd,
		.ReScanCustomScan = SliceProjectReScan,
		.MarkPosCustomScan = SliceProjectMarkPos,
		.RestrPosCustomScan = SliceProjectRestrPos,
		.EstimateDSMCustomScan = SliceProjectEstimateDSM,
		.InitializeDSMCustomScan = SliceProjectInitializeDSM,
		.ReInitializeDSMCustomScan = SliceProjectReInitializeDSM,
		.InitializeWorkerCustomScan = SliceProjectInitializeWorker,
		.ShutdownCustomScan = SliceProjectShutdown,
		.ExplainCustomScan = SliceProjectExplain,
	};
	return methods;
}

[[nodiscard]] static bool slice_project_is_bioseq_scan_path(Path* path) {
	if (path == nullptr || !IsA(path, CustomPath)) return false;
	auto* custom = reinterpret_cast<CustomPath*>(path);
	return custom->methods != nullptr
		&& custom->methods->CustomName != nullptr
		&& strcmp(custom->methods->CustomName, "BioseqScan") == 0;
}

[[nodiscard]] static bool slice_project_is_owner_path(Path* path) {
	if (path == nullptr || !IsA(path, CustomPath)) return false;
	auto* custom = reinterpret_cast<CustomPath*>(path);
	return custom->methods == &bioseq_slice_project_path_methods();
}

[[nodiscard]] static Path* slice_project_tag_source_child(
		Path* path, PathTarget* target,
		SliceProjectIrBuilder const& builder) {
	if (!slice_project_is_bioseq_scan_path(path)) return path;
	auto* original = reinterpret_cast<CustomPath*>(path);
	auto* copy = makeNode(CustomPath);
	*copy = *original;
	copy->custom_private = NIL;
	for (Node* node : PList<Node>(original->custom_private)) {
		if (node != nullptr && IsA(node, List)) {
			List* tag = reinterpret_cast<List*>(node);
			if (list_length(tag) >= 1
				&& IsA(reinterpret_cast<Node*>(linitial(tag)), Integer)
				&& intVal(linitial(tag)) == kBioseqSliceSourceMagic) {
				continue;
			}
		}
		copy->custom_private = lappend(
			copy->custom_private, copyObject(node));
	}
	List* bindings = NIL;
	Bitmapset* seen_column_attnos = nullptr;
	int resno = 1;
	/* BSS3 bindings are [child resno, producer id, physical varattno].  The
	 * child resno validates plan/setrefs identity; promotion indexes the raw
	 * relation scan slot by physical varattno. */
	for (Node* expression : PList<Node>(target->exprs)) {
		if (exprType(expression) == builder.oids->bioseq_seq_type_oid) {
			Var* var = pg_node_try_as<Var>(expression);
			if (var == nullptr || var->varattno <= 0
				|| bms_is_member(var->varattno, seen_column_attnos)) {
				++resno;
				continue;
			}
			std::uint32_t producer_id = 0u;
			bool binding_found = false;
			for (auto const& binding : builder.source_bindings) {
				if (equal(binding.first, expression)) {
					producer_id = binding.second;
					binding_found = true;
					break;
				}
			}
			if (builder.source_bindings_only && !binding_found) {
				++resno;
				continue;
			}
			bindings = lappend(bindings,
				list_make3(makeInteger(resno),
					makeInteger(static_cast<int>(producer_id)),
					makeInteger(static_cast<int>(var->varattno))));
			seen_column_attnos = bms_add_member(
				seen_column_attnos, var->varattno);
		}
		++resno;
	}
	if (seen_column_attnos != nullptr) bms_free(seen_column_attnos);
	copy->custom_private = lappend(copy->custom_private,
		list_make3(makeInteger(kBioseqSliceSourceMagic),
			makeInteger(kBioseqSliceSourceVersion), bindings));
	copy->path.pathtarget = target;
	return reinterpret_cast<Path*>(copy);
}

/* `path` is already a private copy.  Descend only core path shapes whose
 * child ownership is part of PostgreSQL's public Path ABI; unknown custom and
 * foreign providers remain unmodified unless they expose their own contract. */
[[nodiscard]] static Path* slice_project_tag_owned_path_tree(
		Path* path, SliceProjectIrBuilder const& builder,
		bool& tagged_source) {
	if (path == nullptr) return nullptr;
	if (slice_project_is_bioseq_scan_path(path)) {
		tagged_source = true;
		return slice_project_tag_source_child(
			path, copy_pathtarget(path->pathtarget), builder);
	}
	if (IsA(path, NestPath)) {
		auto* join = &reinterpret_cast<NestPath*>(path)->jpath;
		join->outerjoinpath = slice_project_tag_owned_path_tree(
			join->outerjoinpath, builder, tagged_source);
		join->innerjoinpath = slice_project_tag_owned_path_tree(
			join->innerjoinpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, MergePath)) {
		auto* join = &reinterpret_cast<MergePath*>(path)->jpath;
		join->outerjoinpath = slice_project_tag_owned_path_tree(
			join->outerjoinpath, builder, tagged_source);
		join->innerjoinpath = slice_project_tag_owned_path_tree(
			join->innerjoinpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, HashPath)) {
		auto* join = &reinterpret_cast<HashPath*>(path)->jpath;
		join->outerjoinpath = slice_project_tag_owned_path_tree(
			join->outerjoinpath, builder, tagged_source);
		join->innerjoinpath = slice_project_tag_owned_path_tree(
			join->innerjoinpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, CustomPath)) {
		auto* custom = reinterpret_cast<CustomPath*>(path);
		for (ListCell* cell = list_head(custom->custom_paths);
			 cell != nullptr; cell = lnext(custom->custom_paths, cell)) {
			lfirst(cell) = slice_project_tag_owned_path_tree(
				reinterpret_cast<Path*>(lfirst(cell)), builder, tagged_source);
		}
		return path;
	}
	if (IsA(path, ProjectionPath)) {
		auto* projection = reinterpret_cast<ProjectionPath*>(path);
		projection->subpath = slice_project_tag_owned_path_tree(
			projection->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, ProjectSetPath)) {
		auto* projection = reinterpret_cast<ProjectSetPath*>(path);
		projection->subpath = slice_project_tag_owned_path_tree(
			projection->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, SortPath)) {
		auto* sort = reinterpret_cast<SortPath*>(path);
		sort->subpath = slice_project_tag_owned_path_tree(
			sort->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, IncrementalSortPath)) {
		auto* sort = reinterpret_cast<IncrementalSortPath*>(path);
		sort->spath.subpath = slice_project_tag_owned_path_tree(
			sort->spath.subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, MaterialPath)) {
		auto* material = reinterpret_cast<MaterialPath*>(path);
		material->subpath = slice_project_tag_owned_path_tree(
			material->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, MemoizePath)) {
		auto* memoize = reinterpret_cast<MemoizePath*>(path);
		memoize->subpath = slice_project_tag_owned_path_tree(
			memoize->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, UniquePath)) {
		auto* unique = reinterpret_cast<UniquePath*>(path);
		unique->subpath = slice_project_tag_owned_path_tree(
			unique->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, GatherPath)) {
		auto* gather = reinterpret_cast<GatherPath*>(path);
		gather->subpath = slice_project_tag_owned_path_tree(
			gather->subpath, builder, tagged_source);
		return path;
	}
	if (IsA(path, GatherMergePath)) {
		auto* gather = reinterpret_cast<GatherMergePath*>(path);
		gather->subpath = slice_project_tag_owned_path_tree(
			gather->subpath, builder, tagged_source);
		return path;
	}
	auto tag_path_list = [&builder, &tagged_source](List* subpaths) {
		for (ListCell* cell = list_head(subpaths);
			 cell != nullptr; cell = lnext(subpaths, cell)) {
			lfirst(cell) = slice_project_tag_owned_path_tree(
				reinterpret_cast<Path*>(lfirst(cell)), builder, tagged_source);
		}
	};
	if (IsA(path, AppendPath)) {
		tag_path_list(reinterpret_cast<AppendPath*>(path)->subpaths);
	} else if (IsA(path, MergeAppendPath)) {
		tag_path_list(reinterpret_cast<MergeAppendPath*>(path)->subpaths);
	}
	return path;
}

/* BatchSort is an upper owner rather than a SliceProject node, but it needs
 * the same exact base-scan source contract: copy the owned input tree, attach
 * BSS3 only to BioseqScan leaves, and leave unknown providers untouched. */
[[nodiscard]] static Path* batch_sort_tag_source_path(
		Path* path, Node* sort_expression, bool* tagged_out) {
	if (tagged_out == nullptr) return nullptr;
	*tagged_out = false;
	if (path == nullptr || sort_expression == nullptr) return path;
	const BioseqStubOids* oids = try_get_cached_stub_oids();
	if (oids == nullptr
		|| exprType(sort_expression) != oids->bioseq_seq_type_oid) return path;
	SliceProjectIrBuilder builder{
		.oids = oids,
		.command_type = CMD_SELECT,
		.source_bindings_only = true,
	};
	/* BatchSort has no SliceProject producer graph.  Producer zero therefore
	 * truthfully means source ownership without boundary attribution. */
	builder.source_bindings.emplace_back(
		reinterpret_cast<Node*>(copyObject(sort_expression)), 0u);
	Path* private_path = reinterpret_cast<Path*>(copyObject(path));
	bool tagged = false;
	private_path = slice_project_tag_owned_path_tree(
		private_path, builder, tagged);
	*tagged_out = tagged;
	return tagged ? private_path : path;
}

static void batch_sort_collect_source_routes(
		PlanState* plan, bioseq::storage::pg_sequence_source_route_set& routes,
		std::uint32_t& provider_count) {
	if (plan == nullptr) return;
	if (IsA(plan, CustomScanState)) {
		auto* custom = reinterpret_cast<CustomScanState*>(plan);
		if (custom->methods == &BioseqColumnScanExecuteMethods) {
			auto* scan = BioseqScanState::from_custom_scan_state(custom);
			BioseqScanRuntime& runtime = bioseq_runtime(scan);
			if (!runtime.lazy_source_projection) return;
			if (runtime.lazy_source_provider == nullptr
				|| !bioseq::storage::add_pg_sequence_source_route(
					routes, runtime.lazy_source_provider)) {
				ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("BioseqBatchSort source route is unavailable")));
			}
			provider_count = bioseq::parallelism::saturating_add(
				provider_count, 1u);
			return;
		}
		for (PlanState* child : PList<PlanState>(custom->custom_ps)) {
			batch_sort_collect_source_routes(child, routes, provider_count);
		}
		return;
	}
	if (IsA(plan, AppendState)) {
		auto* append = reinterpret_cast<AppendState*>(plan);
		for (int index = 0; index < append->as_nplans; ++index) {
			batch_sort_collect_source_routes(
				append->appendplans[index], routes, provider_count);
		}
		return;
	}
	if (IsA(plan, MergeAppendState)) {
		auto* append = reinterpret_cast<MergeAppendState*>(plan);
		for (int index = 0; index < append->ms_nplans; ++index) {
			batch_sort_collect_source_routes(
				append->mergeplans[index], routes, provider_count);
		}
		return;
	}
	PlanState* outer = outerPlanState(plan);
	PlanState* inner = innerPlanState(plan);
	batch_sort_collect_source_routes(outer, routes, provider_count);
	if (inner != outer) {
		batch_sort_collect_source_routes(inner, routes, provider_count);
	}
}

[[nodiscard]] static bool batch_sort_attach_source_routes(
		PlanState* child,
		bioseq::storage::pg_sequence_source_route_set& routes) {
	std::uint32_t provider_count = 0u;
	batch_sort_collect_source_routes(child, routes, provider_count);
	return provider_count != 0u;
}

struct SliceProjectBaseTargetProof {
	Index relid = 0;
	bool saw_base_var = false;
	bool invalid = false;
};

struct slice_project_base_target_visitor {
	[[nodiscard]] pg_walk_step operator()(
			pg_expression_walk_cursor, pg_node_view node,
			SliceProjectBaseTargetProof& proof) const noexcept {
		if (node.try_as<PlaceHolderVar>() != nullptr) {
			/* A PHV belongs to a planner-selected evaluation/nulling frontier.
			 * A base SliceProject must not move it below that frontier. */
			proof.invalid = true;
			return pg_walk_step::stop;
		}
		if (Var* var = node.try_as<Var>()) {
			if (var->varno != proof.relid || var->varattno <= 0
				|| var->varlevelsup != 0
				|| !bms_is_empty(var->varnullingrels)) {
				proof.invalid = true;
				return pg_walk_step::stop;
			}
			proof.saw_base_var = true;
			return pg_walk_step::skip_subtree;
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static bool slice_project_target_has_semantic_owner(
		PlannerInfo* root, PathTarget* target) {
	if (root == nullptr || root->parse == nullptr || target == nullptr) {
		return true;
	}
	Node* expressions = reinterpret_cast<Node*>(target->exprs);
	return root->parse->hasAggs || root->parse->hasWindowFuncs
		|| root->parse->hasTargetSRFs
		|| root->parse->groupClause != NIL
		|| root->parse->groupingSets != NIL
		|| root->parse->havingQual != nullptr
		|| root->parse->setOperations != nullptr
		|| contain_agg_clause(expressions)
		|| contain_window_function(expressions)
		|| contain_volatile_functions(expressions)
		|| contain_subplans(expressions)
		|| expression_returns_set(expressions);
}

/* Prove the final processed projection can be evaluated immediately above
 * this one base relation.  The single-baserel restriction is intentionally
 * strict: a wider query needs a join/upper owner which preserves its own
 * nulling and evaluation frontier. */
[[nodiscard]] static bool slice_project_base_target_is_safe(
		PlannerInfo* root, RelOptInfo* rel, PathTarget* target) {
	if (root == nullptr || rel == nullptr || root->parse == nullptr
		|| target == nullptr || target->exprs == NIL
		|| root->parse->hasRowSecurity
		|| !(root->parse->commandType == CMD_SELECT
			|| root->parse->commandType == CMD_INSERT)
		|| rel->relid == 0 || rel->reloptkind != RELOPT_BASEREL
		|| bms_num_members(root->all_baserels) != 1
		|| !bms_is_member(static_cast<int>(rel->relid), root->all_baserels)
		|| !bms_is_empty(rel->lateral_relids)
		|| slice_project_target_has_semantic_owner(root, target)) {
		return false;
	}
	SliceProjectBaseTargetProof proof{.relid = rel->relid};
	slice_project_base_target_visitor visitor;
	pg_walk_expression_including_root(
		reinterpret_cast<Node*>(target->exprs), proof, visitor);
	return proof.saw_base_var && !proof.invalid;
}

/* Install the lowest valid owner for a base-local recipe.  Every surviving
 * alternative is a real SliceProject -> BioseqScan pair; incompatible index,
 * heap, and untagged paths are removed only after at least one complete owner
 * has been constructed.  Costs remain ordinary nonnegative PostgreSQL costs. */
static void slice_project_try_install_base_owner(
		PlannerInfo* root, RelOptInfo* rel) {
	if (!EnableBioseqColumnCustomScan || root == nullptr || rel == nullptr
		|| root->processed_tlist == NIL) return;
	PathTarget* projection_target = make_pathtarget_from_tlist(
		root->processed_tlist);
	set_pathtarget_cost_width(root, projection_target);
	if (!slice_project_base_target_is_safe(root, rel, projection_target)) return;
	if (!is_parallel_safe(
			root, reinterpret_cast<Node*>(projection_target->exprs))) return;
	const BioseqStubOids* oids = try_get_cached_stub_oids();
	if (oids == nullptr) return;
	SliceProjectIrBuilder builder{
		.oids = oids, .command_type = root->parse->commandType};
	if (!slice_project_discover_target(projection_target, builder)) return;

	List* required = NIL;
	constexpr int pull_flags = PVC_RECURSE_AGGREGATES
		| PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	for (Node* expression : PList<Node>(projection_target->exprs)) {
		required = list_concat(required,
			pull_var_clause(expression, pull_flags));
	}
	QualCost expression_cost{};
	cost_qual_eval(&expression_cost, projection_target->exprs, root);
	const double metadata_per_row = cpu_operator_cost * static_cast<double>(
		std::min<std::uint64_t>(builder.piece_boundaries, UINT32_MAX));
	const double boundary_per_row = cpu_operator_cost * static_cast<double>(
		std::min<std::uint64_t>(builder.planned_symbols, UINT32_MAX));

	PList<Path> owners;
	PList<Path> partial_owners;
	auto build_owner = [root, rel, projection_target, required, &builder,
		&expression_cost, metadata_per_row,
		boundary_per_row](Path* source) -> Path* {
		PathTarget* child_target = copy_pathtarget(source->pathtarget);
		add_new_columns_to_pathtarget(child_target, required);
		set_pathtarget_cost_width(root, child_target);
		Path* child = slice_project_tag_source_child(
			source, child_target, builder);

		auto* owner = makeNode(CustomPath);
		owner->path.pathtype = T_CustomScan;
		owner->path.parent = rel;
		owner->path.pathtarget = copy_pathtarget(projection_target);
		owner->path.param_info = child->param_info;
		owner->path.parallel_aware = child->parallel_aware;
		owner->path.parallel_safe = child->parallel_safe;
		owner->path.parallel_workers = child->parallel_workers;
		owner->path.rows = child->rows;
		owner->path.disabled_nodes = child->disabled_nodes;
		owner->path.startup_cost = child->startup_cost
			+ expression_cost.startup;
		const double rows = std::max(0.0, child->rows);
		owner->path.total_cost = child->total_cost + expression_cost.startup
			+ rows * (expression_cost.per_tuple
				+ metadata_per_row + boundary_per_row);
		owner->path.pathkeys = child->pathkeys;
		owner->flags = CUSTOMPATH_SUPPORT_PROJECTION;
		owner->custom_paths = list_make1(child);
		owner->custom_private = list_make5(
			slice_project_header(builder), copyObject(builder.expressions),
			copyObject(builder.nodes), copyObject(builder.records),
			copyObject(builder.output_bindings));
		owner->custom_restrictinfo = NIL;
		owner->methods = &bioseq_slice_project_path_methods();
		return reinterpret_cast<Path*>(owner);
	};
	for (Path* source : PList<Path>(rel->pathlist)) {
		if (!slice_project_is_bioseq_scan_path(source)) continue;
		owners.append(build_owner(source));
	}
	for (Path* source : PList<Path>(rel->partial_pathlist)) {
		if (!slice_project_is_bioseq_scan_path(source)) continue;
		partial_owners.append(build_owner(source));
	}
	if (owners.empty()) return;

	rel->pathlist = NIL;
	rel->partial_pathlist = NIL;
	rel->cheapest_startup_path = nullptr;
	rel->cheapest_total_path = nullptr;
	rel->cheapest_unique_path = nullptr;
	rel->cheapest_parameterized_paths = NIL;
	for (Path* owner : owners) add_path(rel, owner);
	for (Path* owner : partial_owners) add_partial_path(rel, owner);
}

static void BioseqSliceProject_CreateUpperPaths(
		PlannerInfo* root, UpperRelationKind stage,
		RelOptInfo* input_rel, RelOptInfo* output_rel, void*) {
	if (!EnableBioseqColumnCustomScan || root == nullptr
		|| input_rel == nullptr || output_rel == nullptr
		|| output_rel->reltarget == nullptr
		|| root->parse == nullptr || root->parse->hasRowSecurity
		|| root->parse->commandType != CMD_SELECT) return;
	if (!(stage == UPPERREL_GROUP_AGG || stage == UPPERREL_WINDOW
		|| stage == UPPERREL_DISTINCT || stage == UPPERREL_ORDERED
		|| stage == UPPERREL_FINAL)) return;
	/*
	 * PostgreSQL can defer the final SELECT projection until after the
	 * UPPERREL_FINAL hook has run.  In that shape output_rel->reltarget still
	 * describes the pass-through input (and a later core ProjectionPath becomes
	 * the visible Result), while grouping_planner has already published the
	 * stage's authoritative projection in root->upper_targets.  Discovering
	 * recipes only in output_rel->reltarget therefore misses the common
	 *
	 *     Result (slice expression)
	 *       -> BioseqScan (raw sequence Var)
	 *
	 * plan.  Build the owner against the same stage target core will apply
	 * after this hook.  Falling back to the relation target preserves stages
	 * for which core has no separate upper target.
	 */
	PathTarget* projection_target = root->upper_targets[stage];
	if (projection_target == nullptr) {
		projection_target = output_rel->reltarget;
	}
	if (projection_target == nullptr
		|| slice_project_target_has_semantic_owner(root, projection_target)) return;
	const BioseqStubOids* oids = try_get_cached_stub_oids();
	if (oids == nullptr) return;
	if (!is_parallel_safe(
			root, reinterpret_cast<Node*>(projection_target->exprs))) return;
	SliceProjectIrBuilder builder{
		.oids = oids, .command_type = root->parse->commandType};
	if (!slice_project_discover_target(projection_target, builder)) return;
	for (Node* expression : PList<Node>(projection_target->exprs)) {
		if (bioseq_expr_uses_lazy_sequence_projection(expression)
			&& contain_volatile_functions(expression)) return;
	}
	Path* child = input_rel->cheapest_total_path;
	if (child == nullptr && input_rel->pathlist != NIL) {
		child = reinterpret_cast<Path*>(linitial(input_rel->pathlist));
	}
	if (child == nullptr) return;
	while (IsA(child, ProjectionPath)
		&& bioseq_expr_uses_lazy_sequence_projection(
			reinterpret_cast<Node*>(child->pathtarget->exprs))) {
		child = reinterpret_cast<ProjectionPath*>(child)->subpath;
	}
	if (slice_project_is_owner_path(child)) return;

	List* required = NIL;
	constexpr int pull_flags = PVC_RECURSE_AGGREGATES
		| PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	for (Node* expression : PList<Node>(projection_target->exprs)) {
		required = list_concat(required,
			pull_var_clause(expression, pull_flags));
	}
	PathTarget* child_target = copy_pathtarget(child->pathtarget);
	add_new_columns_to_pathtarget(child_target, required);
	set_pathtarget_cost_width(root, child_target);
	if (slice_project_is_bioseq_scan_path(child)) {
		child = slice_project_tag_source_child(child, child_target, builder);
	} else {
		/* A join/upper child is immutable and may be shared by many candidates.
		 * Deep-copy supported core path shapes and tag every BioseqScan source
		 * below them before installing hidden leaf outputs through core's
		 * projection constructor.  The final target remains wrapper-only. */
		bool tagged_source = false;
		Path* private_child = reinterpret_cast<Path*>(copyObject(child));
		private_child = slice_project_tag_owned_path_tree(
			private_child, builder, tagged_source);
		if (!tagged_source) return;
		child = reinterpret_cast<Path*>(create_projection_path(
			root, private_child->parent, private_child, child_target));
	}

	QualCost expression_cost{};
	cost_qual_eval(&expression_cost, projection_target->exprs, root);
	const double rows = std::max(0.0, child->rows);
	const double metadata_per_row = cpu_operator_cost * static_cast<double>(
		std::min<std::uint64_t>(builder.piece_boundaries, UINT32_MAX));
	const double boundary_per_row = cpu_operator_cost * static_cast<double>(
		std::min<std::uint64_t>(builder.planned_symbols, UINT32_MAX));

	auto* path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = output_rel;
	path->path.pathtarget = copy_pathtarget(projection_target);
	path->path.param_info = child->param_info;
	path->path.parallel_aware = child->parallel_aware;
	path->path.parallel_safe = child->parallel_safe;
	path->path.parallel_workers = child->parallel_workers;
	path->path.rows = child->rows;
	path->path.disabled_nodes = child->disabled_nodes;
	path->path.startup_cost = child->startup_cost + expression_cost.startup;
	path->path.total_cost = child->total_cost + expression_cost.startup
		+ rows * (expression_cost.per_tuple
			+ metadata_per_row + boundary_per_row);
	path->path.pathkeys = child->pathkeys;
	path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
	path->custom_paths = list_make1(child);
	path->custom_private = list_make5(
		slice_project_header(builder), builder.expressions, builder.nodes,
		builder.records, builder.output_bindings);
	path->custom_restrictinfo = NIL;
	path->methods = &bioseq_slice_project_path_methods();

	/* The core ProjectionPath remains fractionally cheaper because PostgreSQL's
	 * generic cost model cannot represent avoided full-sequence hydration.  Do
	 * not falsify either cost.  Once a complete source-tagged wrapper exists,
	 * remove the incompatible unowned alternatives so selection is structural.
	 * Every early/fallback return above leaves the original lists untouched. */
	output_rel->pathlist = NIL;
	output_rel->partial_pathlist = NIL;
	output_rel->cheapest_startup_path = nullptr;
	output_rel->cheapest_total_path = nullptr;
	output_rel->cheapest_unique_path = nullptr;
	output_rel->cheapest_parameterized_paths = NIL;
	add_path(output_rel, reinterpret_cast<Path*>(path));
	set_cheapest(output_rel);
}


/*
 * bioseq_customscan_init installs the hook required to intercept the postgres planner and
 * provide extra paths for columnar tables
 */
export void
bioseq_customscan_init()
{
	PreviousSetRelPathlistHook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = BioseqColumnSetRelPathlistHook;
	PreviousSetJoinPathlistHook = set_join_pathlist_hook;
	set_join_pathlist_hook = BioseqBatchJoinSetJoinPathlistHook;

	PreviousGetRelationInfoHook = get_relation_info_hook;
	get_relation_info_hook = BioseqColumnGetRelationInfoHook;

	/* register customscan specific GUC's */
	DefineCustomBoolVariable(
		"bioseq.enable_custom_scan",
		"Enables the use of a custom scan to push projections and quals "
					 "into the storage layer.",
		NULL,
		&EnableBioseqColumnCustomScan,
		true,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomBoolVariable(
		"bioseq.enable_qual_pushdown",
		"Enables qual pushdown into bioseq. This has no effect unless "
					 "bioseq.enable_custom_scan is true.",
		NULL,
		&EnableBioseqColumnQualPushdown,
		true,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomRealVariable(
		"bioseq.qual_pushdown_correlation_threshold",
		"Correlation threshold to attempt to push a qual "
					 "referencing the given column. A value of 0 means "
					 "attempt to push down all quals, even if the column "
					 "is uncorrelated.",
		NULL,
		&BioseqColumnQualPushdownCorrelationThreshold,
		0.4,
		0.0,
		1.0,
		PGC_USERSET,
		GUC_NO_SHOW_ALL |GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomIntVariable(
		"bioseq.max_custom_scan_paths",
		"Maximum number of custom scan paths to generate "
					 "for a columnar table when planning.",
		NULL,
		&BioseqColumnMaxCustomScanPaths,
		64,
		1,
		1024,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL, NULL, NULL);
	DefineCustomEnumVariable(
		"bioseq.planner_debug_level",
		"Message level for columnar planning information.",
		NULL,
		&BioseqColumnPlannerDebugLevel,
		DEBUG3,
		debug_level_options,
		PGC_USERSET,
		0,
		NULL,
		NULL,
		NULL);
#ifdef BIOSEQ_TEST_INDEXES
	DefineCustomEnumVariable(
		"bioseq.test_slice_dsm_fixture",
		"Injects an isolated SliceProject DSM lifecycle fixture for tests.",
		NULL,
		&BioseqSliceDsmTestFixture,
		kSliceDsmTestNone,
		slice_dsm_test_fixture_options,
		PGC_USERSET,
		GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
		NULL,
		NULL,
		NULL);
#endif

	RegisterCustomScanMethods(&BioseqColumnScanScanMethods);
	RegisterCustomScanMethods(&BioseqBatchJoinScanMethods);
	RegisterCustomScanMethods(&bioseq_slice_project_scan_methods());
	bioseq_register_slice_project_upper_path_provider(
		BioseqSliceProject_CreateUpperPaths);
	bioseq_register_batch_sort_source_providers(
		batch_sort_tag_source_path, batch_sort_attach_source_routes);
}

static void
BioseqColumnSetRelPathlistHook(PlannerInfo *root, RelOptInfo *rel, Index rti,
						   RangeTblEntry *rte)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnSetRelPathlistHook() entered");
	/* call into previous hook if assigned */
	if (PreviousSetRelPathlistHook)
	{
		PreviousSetRelPathlistHook(root, rel, rti, rte);
	}

	if (!OidIsValid(rte->relid) || rte->rtekind != RTE_RELATION || rte->inh)
	{
		/* some calls to the pathlist hook don't have a valid relation set. Do nothing */
		return;
	}

	/*
	 * Here we want to inspect if this relation pathlist hook is accessing a columnar table.
	 * If that is the case we want to insert an extra path that pushes down the projection
	 * into the scan of the table to minimize the data read.
	 */
	Relation relation = RelationIdGetRelation(rte->relid);
	if (relation->rd_tableam == GetBioseqColumnTableAmRoutine())
	{
		if (rte->tablesample != NULL)
		{
			bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
							.msg("sample scans not supported on columnar tables");
		}

		/*
		 * There are cases where IndexPath is normally more preferrable over
		 * SeqPath for heapAM but not for columnarAM. In such cases, an
		 * IndexPath could wrongly dominate a SeqPath based on the costs
		 * estimated by postgres earlier. For this reason, here we manually
		 * create a SeqPath, estimate the cost based on columnarAM and append
		 * to pathlist.
		 *
		 * Before doing that, we first re-cost all the existing paths so that
		 * add_path makes correct cost comparisons when appending our SeqPath.
		 */
		CostBioseqColumnPaths(root, rel, rte->relid);

		Path *seqPath = CreateBioseqColumnSeqScanPath(root, rel, rte->relid);
		add_path(rel, seqPath);

		if (EnableBioseqColumnCustomScan)
		{
			/*
			 * When columnar custom scan is enabled (bioseq.enable_custom_scan),
			 * we only consider BioseqColumnScanPath's & IndexPath's. For this reason,
			 * we remove other paths and re-estimate IndexPath costs to make accurate
			 * comparisons between them.
			 *
			 * Even more, we might calculate an equal cost for a
			 * BioseqColumnCustomScan and a SeqPath if we are reading all columns
			 * of given table since we don't consider chunk group filtering
			 * when costing BioseqColumnCustomScan.
			 * In that case, if we don't remove SeqPath's, we might wrongly choose
			 * SeqPath thinking that its cost would be equal to BioseqColumnCustomScan.
			 */
			RemovePathsByPredicate(rel, IsNotIndexPath);
			AddBioseqColumnScanPaths(root, rel, rte);
			slice_project_try_install_base_owner(root, rel);
		}
	}
	else
	{
		/* DOC0372 — external logical relations use a distinct table AM. The
		 * always-owned slot returns false (this columnar hook simply falls
		 * through) unless the external provider stack is compiled in and has
		 * registered a path builder, in which case the builder replaces the
		 * physical scan with the external CustomScan. */
		(void) TryAddBioseqExternalScanPaths(root, rel, rte, relation);
	}
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnSetRelPathlistHook() relation_close");
	RelationClose(relation);
}


static void
BioseqColumnGetRelationInfoHook(PlannerInfo *root, Oid relationObjectId,
							bool inhparent, RelOptInfo *rel)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnGetRelationInfoHook() entered");
	if (PreviousGetRelationInfoHook)
	{
		PreviousGetRelationInfoHook(root, relationObjectId, inhparent, rel);
	}

	if (IsBioseqColumnTableAmTable(relationObjectId))
	{
		/* disable parallel query */
		rel->rel_parallel_workers = 0;

		/* disable index-only scan */
		for(auto indexOptInfo : PList<IndexOptInfo>(rel->indexlist)) {
			memset(indexOptInfo->canreturn, false, indexOptInfo->ncolumns * sizeof(bool));
		}
	}
}

struct BatchJoinTargetProjectionProbe {
	Index target_varno = 0;
	AttrNumber target_seq_attno = InvalidAttrNumber;
	const BioseqStubOids* oids = nullptr;
	bool allow_mmsearch_qualifier = false;
	bool forbidden = false;
};

struct batch_join_target_projection_visitor {
	[[nodiscard]] pg_walk_step operator()(
		pg_expression_walk_cursor,
		pg_node_view node,
		BatchJoinTargetProjectionProbe& probe) const noexcept {
		/* A validated mmsearch restriction consumes the locator inside the
		 * BatchJoin executor, while match-result helpers are evaluated by its
		 * parent from the current MatchResult. Neither argument is a request to
		 * let the locator-backed datum escape into ordinary SQL evaluation. A
		 * projected mmsearch call remains forbidden. */
		if (FuncExpr* function = node.try_as<FuncExpr>()) {
			if (probe.oids != nullptr
				&& (is_bioseq_projection_funcid(function->funcid, *probe.oids)
					|| (probe.allow_mmsearch_qualifier
						&& is_mmsearch_funcid(
							function->funcid, *probe.oids)))) {
				return pg_walk_step::skip_subtree;
			}
		}
		if (Var* var = node.try_as<Var>()) {
			if (var->varno == probe.target_varno
				&& (var->varattno == 0
					|| var->varattno == probe.target_seq_attno)) {
				probe.forbidden = true;
				return pg_walk_step::stop;
			}
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static bool batch_join_target_expression_is_forbidden(
		Node* expression,
		const Var* target_var,
		const bool allow_mmsearch_qualifier) {
	if (expression == nullptr || target_var == nullptr) {
		return false;
	}
	BatchJoinTargetProjectionProbe probe{
		target_var->varno, target_var->varattno,
		&get_cached_stub_oids(), allow_mmsearch_qualifier, false};
	batch_join_target_projection_visitor visitor;
	pg_walk_expression_including_root(
		expression, probe, visitor);
	return probe.forbidden;
}

[[nodiscard]] static bool batch_join_target_restrictions_are_forbidden(
		List* restrictions,
		const Var* target_var,
		const bool allow_mmsearch_qualifier) {
	for (Node* node : PList<Node>(restrictions)) {
		RestrictInfo* restriction = pg_node_try_as<RestrictInfo>(node);
		if (restriction == nullptr || restriction->clause == nullptr
			|| batch_join_target_expression_is_forbidden(
				reinterpret_cast<Node*>(restriction->clause),
				target_var,
				allow_mmsearch_qualifier)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] static bool batch_join_typmod_is_supported(
		const int32 typmod) noexcept {
	if (typmod < 0) {
		return false;
	}
	const auto decoded = compact_sequence_stdc_type_params::decode(typmod);
	const bool alphabet_supported =
		decoded.alphabet_code < pgseq_supported_alphabets::alphabet_count;
	const bool width_supported =
		pgseq_supported_storage_unit_byte_widths::contains_runtime(
			decoded.storage_unit_byte_width);
	return alphabet_supported && width_supported
		&& decoded.encode() == typmod;
}

/* Reject a Gather hidden below the common unary planner wrappers.  Looking
 * only at cheapest_total_path->pathtype misses Projection/Material wrappers
 * and lets a parallel child enter an executor that owns one backend-local
 * query materialization. */
[[nodiscard]] static bool batch_join_path_contains_parallel_execution(
		const Path* path) noexcept {
	if (path == nullptr) {
		return false;
	}
	if (path->pathtype == T_Gather || path->pathtype == T_GatherMerge
		|| path->parallel_aware || path->parallel_workers > 0) {
		return true;
	}
	switch (path->pathtype) {
		case T_Result:
			return batch_join_path_contains_parallel_execution(
				reinterpret_cast<const ProjectionPath*>(path)->subpath);
		case T_Material:
			return batch_join_path_contains_parallel_execution(
				reinterpret_cast<const MaterialPath*>(path)->subpath);
		case T_Memoize:
			return batch_join_path_contains_parallel_execution(
				reinterpret_cast<const MemoizePath*>(path)->subpath);
		case T_Sort:
			return batch_join_path_contains_parallel_execution(
				reinterpret_cast<const SortPath*>(path)->subpath);
	case T_IncrementalSort:
			return batch_join_path_contains_parallel_execution(
				reinterpret_cast<const IncrementalSortPath*>(path)->spath.subpath);
		case T_NestLoop:
		case T_MergeJoin:
		case T_HashJoin: {
			auto const* join = reinterpret_cast<JoinPath const*>(path);
			return batch_join_path_contains_parallel_execution(
					join->outerjoinpath)
				|| batch_join_path_contains_parallel_execution(
					join->innerjoinpath);
		}
		case T_CustomScan: {
			auto const* custom = reinterpret_cast<CustomPath const*>(path);
			for (Path* child : PList<Path>(custom->custom_paths)) {
				if (batch_join_path_contains_parallel_execution(child)) {
					return true;
				}
			}
			return false;
		}
		case T_Append: {
			auto const* append = reinterpret_cast<AppendPath const*>(path);
			for (Path* child : PList<Path>(append->subpaths)) {
				if (batch_join_path_contains_parallel_execution(child)) {
					return true;
				}
			}
			for (Path* child : PList<Path>(append->partial_subpaths)) {
				if (batch_join_path_contains_parallel_execution(child)) {
					return true;
				}
			}
			return false;
		}
		case T_MergeAppend: {
			auto const* append = reinterpret_cast<MergeAppendPath const*>(path);
			for (Path* child : PList<Path>(append->subpaths)) {
				if (batch_join_path_contains_parallel_execution(child)) {
					return true;
				}
			}
			return false;
		}
		default:
			return false;
	}
}

struct BatchJoinBioseqPathCandidate {
	CustomPath* scan = nullptr;
	Path* cost_owner = nullptr;
	const PathTarget* locator_target = nullptr;
};

[[nodiscard]] static bool path_target_has_equal_expression(
		const PathTarget* target, const Node* expression) noexcept {
	if (target == nullptr || expression == nullptr) {
		return false;
	}
	for (Node* existing : PList<Node>(target->exprs)) {
		if (equal(existing, expression)) {
			return true;
		}
	}
	return false;
}

[[nodiscard]] static FuncExpr* batch_join_exact_target_helper(
		Node* expression,
		const Index target_relid) noexcept {
	FuncExpr* helper = pg_node_try_as<FuncExpr>(expression);
	if (helper == nullptr
		|| !is_bioseq_projection_funcid(
			helper->funcid, get_cached_stub_oids())
		|| helper->funcretset
		|| OidIsValid(helper->funcvariadic)
		|| get_func_rettype(helper->funcid) != helper->funcresulttype
		|| list_length(helper->args) != 1) {
		return nullptr;
	}
	Var* argument = pg_node_try_as<Var>(
		reinterpret_cast<Node*>(linitial(helper->args)));
	return argument != nullptr
		&& argument->varno == target_relid
		&& argument->varattno > 0
		&& argument->varlevelsup == 0
		&& bms_is_empty(argument->varnullingrels)
		? helper : nullptr;
}

/* Exact shape emitted by try_wrap_in_placeholder() for a direct helper call.
 * Do not accept a generic PHV merely because a helper occurs somewhere below
 * it: its evaluation level/nulling semantics may belong to unrelated SQL. */
[[nodiscard]] static FuncExpr* batch_join_exact_helper_phv_function(
		const PlaceHolderVar* placeholder,
		const Index target_relid) noexcept {
	if (placeholder == nullptr
		|| placeholder->phid == 0
		|| placeholder->phlevelsup != 0
		|| !bms_is_empty(placeholder->phnullingrels)
		|| bms_num_members(placeholder->phrels) != 1
		|| !bms_is_member(static_cast<int>(target_relid), placeholder->phrels)) {
		return nullptr;
	}
	return batch_join_exact_target_helper(
		reinterpret_cast<Node*>(placeholder->phexpr), target_relid);
}

[[nodiscard]] static bool batch_join_is_exact_helper_phv(
		const PlaceHolderVar* placeholder,
		const Index target_relid) noexcept {
	return batch_join_exact_helper_phv_function(
		placeholder, target_relid) != nullptr;
}

struct BatchJoinProjectionDeltaProof {
	const PathTarget* child_target = nullptr;
	Index target_relid = 0;
	bool saw_exact_helper_phv = false;
	bool invalid = false;
};

struct batch_join_projection_delta_visitor {
	[[nodiscard]] pg_walk_step operator()(
		pg_expression_walk_cursor,
		pg_node_view node,
		BatchJoinProjectionDeltaProof& proof) const noexcept {
		if (PlaceHolderVar* placeholder = node.try_as<PlaceHolderVar>()) {
			if (!batch_join_is_exact_helper_phv(
					placeholder, proof.target_relid)) {
				proof.invalid = true;
				return pg_walk_step::stop;
			}
			proof.saw_exact_helper_phv = true;
			return pg_walk_step::skip_subtree;
		}
		if (Var* var = node.try_as<Var>()) {
			if (!path_target_has_equal_expression(
					proof.child_target, reinterpret_cast<Node*>(var))) {
				proof.invalid = true;
				return pg_walk_step::stop;
			}
		}
		return pg_walk_step::descend;
	}
};

/* Prove that the complete wrapper-only expression can be evaluated by the
 * BatchJoin parent without changing semantics: it is nonvolatile, every Var
 * is already produced by the child, every PHV is the exact helper-support
 * shape, and at least one such helper PHV explains the wrapper delta. */
[[nodiscard]] static bool batch_join_projection_delta_is_parent_safe(
		Node* expression,
		const PathTarget* child_target,
		const Index target_relid) noexcept {
	if (expression == nullptr || child_target == nullptr
		|| contain_volatile_functions(expression)) {
		return false;
	}
	BatchJoinProjectionDeltaProof proof{
		child_target, target_relid, false, false};
	batch_join_projection_delta_visitor visitor;
	pg_walk_expression_including_root(expression, proof, visitor);
	return !proof.invalid && proof.saw_exact_helper_phv;
}

/* A wrapper may also expose an ordinary target attribute needed above the
 * join (for example t.pk).  Carrying that base Var through the locator child
 * is exact; carrying the sequence datum, a whole row, a system attribute, a
 * nulled outer-join Var, or an expression assembled at this level is not. */
[[nodiscard]] static bool batch_join_is_safe_plain_target_var(
		Node* expression,
		const Index target_relid,
		const AttrNumber target_seq_attno) noexcept {
	Var* var = pg_node_try_as<Var>(expression);
	return var != nullptr
		&& var->varno == target_relid
		&& var->varattno > 0
		&& var->varattno != target_seq_attno
		&& var->varlevelsup == 0
		&& bms_is_empty(var->varnullingrels);
}

/* A helper support function can add a ProjectionPath at the base relation.
 * BioseqBatchJoin may look through it only when every wrapper-only expression
 * is either an exact helper expression evaluated by the parent or one plain,
 * non-sequence target Var copied into the locator child. Any other expression
 * is a real evaluation boundary and disqualifies this candidate. */
[[nodiscard]] static bool batch_join_projection_is_parent_safe(
		const ProjectionPath* projection,
		const AttrNumber target_seq_attno) noexcept {
	if (projection == nullptr || projection->subpath == nullptr
		|| projection->path.pathtype != T_Result
		|| projection->path.parent != projection->subpath->parent
		|| projection->path.parent == nullptr
		|| projection->path.parent->reloptkind != RELOPT_BASEREL
		|| projection->path.parent->relid == 0
		|| projection->path.param_info != nullptr
		|| projection->subpath->param_info != nullptr
		|| !equal(projection->path.pathkeys, projection->subpath->pathkeys)
		|| projection->path.pathtarget == nullptr
		|| projection->subpath->pathtarget == nullptr) {
		return false;
	}
	bool saw_safe_delta = false;
	for (Node* expression : PList<Node>(projection->path.pathtarget->exprs)) {
		if (path_target_has_equal_expression(
				projection->subpath->pathtarget, expression)) {
			continue;
		}
		if (batch_join_is_safe_plain_target_var(
				expression,
				projection->path.parent->relid,
				target_seq_attno)) {
			saw_safe_delta = true;
			continue;
		}
		if (!batch_join_projection_delta_is_parent_safe(
				expression,
				projection->subpath->pathtarget,
				projection->path.parent->relid)) {
			return false;
		}
		saw_safe_delta = true;
	}
	return saw_safe_delta;
}

[[nodiscard]] static BatchJoinBioseqPathCandidate
find_unparameterized_bioseq_custom_path(
		RelOptInfo* rel,
		const AttrNumber target_seq_attno) noexcept {
	BatchJoinBioseqPathCandidate best{};
	for (Path* path : PList<Path>(rel->pathlist)) {
		if (path == nullptr || path->param_info != nullptr) {
			continue;
		}
		Path* cost_owner = path;
		const PathTarget* locator_target = path->pathtarget;
		/* Planner support can expose safe base attributes and wraps match-result helpers in
		 * PlaceHolderVars at their originating base relation.  PostgreSQL can
		 * consequently expose the otherwise eligible BioseqScan as the child
		 * of a projection path.  The projection must not run in the locator
		 * child: BioseqBatchJoin rewrites those helpers to parent-owned result
		 * placeholders after the MatchResult exists.  Look through exactly
		 * that narrowly proven parent-safe wrapper and retain the underlying scan. */
		if (IsA(path, ProjectionPath)) {
			auto* projection = reinterpret_cast<ProjectionPath*>(path);
			if (!batch_join_projection_is_parent_safe(
					projection, target_seq_attno)
				|| batch_join_path_contains_parallel_execution(&projection->path)) {
				continue;
			}
			locator_target = projection->path.pathtarget;
			path = projection->subpath;
		}
		if (path == nullptr || path->pathtype != T_CustomScan
			|| path->param_info != nullptr
			|| batch_join_path_contains_parallel_execution(path)) {
			continue;
		}
		auto* custom = reinterpret_cast<CustomPath*>(path);
		if (custom->methods != &BioseqColumnScanPathMethods) {
			continue;
		}
		if (best.scan == nullptr
			|| compare_path_costs(
				cost_owner, best.cost_owner, TOTAL_COST) < 0) {
			best = BatchJoinBioseqPathCandidate{
				custom, cost_owner, locator_target};
		}
	}
	return best;
}

/*
 * Integration-only proof seam for the exact production eligibility guard.
 * SQL cannot reliably force an unrelated expression into a base-relation
 * ProjectionPath: PostgreSQL will normally retain it in an upper projection.
 * Construct the dangerous planner shapes directly and pass them through
 * find_unparameterized_bioseq_custom_path(), which is the production entry
 * point used by BioseqBatchJoinSetJoinPathlistHook().
 */
struct BatchJoinProjectionGuardFixture {
	RelOptInfo* rel = nullptr;
	CustomPath* scan = nullptr;
	ProjectionPath* projection = nullptr;
};

[[nodiscard]] static PlaceHolderVar*
make_batch_join_projection_guard_phv(
		const Oid helper_oid,
		const Oid result_type,
		const Index relid,
		const Index phid) {
	auto* argument = makeVar(
		relid, 1, INT4OID, -1, InvalidOid, 0);
	auto* helper = makeFuncExpr(
		helper_oid,
		result_type,
		list_make1(argument),
		InvalidOid,
		InvalidOid,
		COERCE_EXPLICIT_CALL);
	auto* placeholder = makeNode(PlaceHolderVar);
	placeholder->phexpr = reinterpret_cast<Expr*>(helper);
	placeholder->phrels = bms_make_singleton(static_cast<int>(relid));
	placeholder->phnullingrels = nullptr;
	placeholder->phid = phid;
	placeholder->phlevelsup = 0;
	return placeholder;
}

[[nodiscard]] static BatchJoinProjectionGuardFixture
make_batch_join_projection_guard_fixture(
		const Index relid,
		List* wrapper_delta,
		const Cost wrapper_startup,
		const Cost wrapper_total,
		const Cardinality wrapper_rows) {
	auto* rel = makeNode(RelOptInfo);
	rel->reloptkind = RELOPT_BASEREL;
	rel->relid = relid;

	auto* child_target = create_empty_pathtarget();
	add_column_to_pathtarget(
		child_target,
		reinterpret_cast<Expr*>(
			makeVar(relid, 1, INT4OID, -1, InvalidOid, 0)),
		0);
	auto* wrapper_target = copy_pathtarget(child_target);
	for (Node* expression : PList<Node>(wrapper_delta)) {
		add_column_to_pathtarget(
			wrapper_target,
			reinterpret_cast<Expr*>(expression),
			0);
	}

	auto* scan = makeNode(CustomPath);
	scan->path.pathtype = T_CustomScan;
	scan->path.parent = rel;
	scan->path.pathtarget = child_target;
	scan->path.param_info = nullptr;
	scan->path.parallel_aware = false;
	scan->path.parallel_safe = false;
	scan->path.parallel_workers = 0;
	scan->path.rows = wrapper_rows;
	scan->path.startup_cost = 1.0;
	scan->path.total_cost = 2.0;
	scan->path.pathkeys = NIL;
	scan->methods = &BioseqColumnScanPathMethods;

	auto* projection = makeNode(ProjectionPath);
	projection->path.pathtype = T_Result;
	projection->path.parent = rel;
	projection->path.pathtarget = wrapper_target;
	projection->path.param_info = nullptr;
	projection->path.parallel_aware = false;
	projection->path.parallel_safe = false;
	projection->path.parallel_workers = 0;
	projection->path.rows = wrapper_rows;
	projection->path.startup_cost = wrapper_startup;
	projection->path.total_cost = wrapper_total;
	projection->path.pathkeys = NIL;
	projection->subpath = &scan->path;
	projection->dummypp = false;
	rel->pathlist = list_make1(projection);
	return BatchJoinProjectionGuardFixture{rel, scan, projection};
}

POSTGRES_FN(bioseq_internal_batch_join_projection_guard_probe)
{
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq BatchJoin projection guard probe requires a composite result")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	constexpr Index target_relid = 1;
	const BioseqStubOids& oids = get_cached_stub_oids();
	auto* exact_helper = make_batch_join_projection_guard_phv(
		oids.score_oid, INT4OID, target_relid, 1);
	const BatchJoinProjectionGuardFixture exact =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(exact_helper),
			13.0,
			37.0,
			5.0);
	const BatchJoinBioseqPathCandidate exact_candidate =
		find_unparameterized_bioseq_custom_path(exact.rel, 2);
	const bool accepts_exact_helper = exact_candidate.scan == exact.scan;
	const bool preserves_wrapper_cost_owner =
		exact_candidate.cost_owner == &exact.projection->path
		&& exact_candidate.cost_owner->startup_cost == 13.0
		&& exact_candidate.cost_owner->total_cost == 37.0
		&& exact_candidate.cost_owner->rows == 5.0;

	auto* unrelated_placeholder = makeNode(PlaceHolderVar);
	unrelated_placeholder->phexpr = reinterpret_cast<Expr*>(
		makeVar(target_relid, 1, INT4OID, -1, InvalidOid, 0));
	unrelated_placeholder->phrels = bms_make_singleton(
		static_cast<int>(target_relid));
	unrelated_placeholder->phnullingrels = nullptr;
	unrelated_placeholder->phid = 2;
	unrelated_placeholder->phlevelsup = 0;
	const BatchJoinProjectionGuardFixture unrelated =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make2(
				make_batch_join_projection_guard_phv(
					oids.score_oid, INT4OID, target_relid, 3),
				unrelated_placeholder),
			13.0,
			37.0,
			5.0);
	const bool rejects_unrelated_phv =
		find_unparameterized_bioseq_custom_path(unrelated.rel, 2).scan == nullptr;

	const Oid nextval_oid = DatumGetObjectId(DirectFunctionCall1(
		regprocedurein, CStringGetDatum("pg_catalog.nextval(regclass)")));
	auto* nextval = makeFuncExpr(
		nextval_oid,
		INT8OID,
		list_make1(makeConst(
			REGCLASSOID,
			-1,
			InvalidOid,
			static_cast<int16>(sizeof(Oid)),
			ObjectIdGetDatum(InvalidOid),
			false,
			true)),
		InvalidOid,
		InvalidOid,
		COERCE_EXPLICIT_CALL);
	auto* volatile_mixed = makeNode(CoalesceExpr);
	volatile_mixed->coalescetype = INT8OID;
	volatile_mixed->coalescecollid = InvalidOid;
	volatile_mixed->args = list_make2(
		make_batch_join_projection_guard_phv(
			oids.query_len_oid, INT8OID, target_relid, 4),
		nextval);
	volatile_mixed->location = -1;
	const BatchJoinProjectionGuardFixture volatile_fixture =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(volatile_mixed),
			13.0,
			37.0,
			5.0);
	const bool rejects_volatile_mixed_expression =
		find_unparameterized_bioseq_custom_path(volatile_fixture.rel, 2).scan
			== nullptr;

	const BatchJoinProjectionGuardFixture plain_target_var =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(makeVar(
				target_relid, 2, INT4OID, -1, InvalidOid, 0)),
			13.0,
			37.0,
			5.0);
	const BatchJoinBioseqPathCandidate plain_candidate =
		find_unparameterized_bioseq_custom_path(
			plain_target_var.rel, /*target_seq_attno=*/3);
	const bool accepts_plain_target_var =
		plain_candidate.scan == plain_target_var.scan
		&& plain_candidate.locator_target
			== plain_target_var.projection->path.pathtarget;

	const BatchJoinProjectionGuardFixture plain_target_sequence_var =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(makeVar(
				target_relid, 3, INT4OID, -1, InvalidOid, 0)),
			13.0,
			37.0,
			5.0);
	const bool rejects_plain_target_sequence_var =
		find_unparameterized_bioseq_custom_path(
			plain_target_sequence_var.rel, /*target_seq_attno=*/3).scan
			== nullptr;

	const BatchJoinProjectionGuardFixture whole_row_var =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(makeVar(
				target_relid, InvalidAttrNumber, RECORDOID,
				-1, InvalidOid, 0)),
			13.0,
			37.0,
			5.0);
	const bool rejects_whole_row_var =
		find_unparameterized_bioseq_custom_path(
			whole_row_var.rel, /*target_seq_attno=*/3).scan == nullptr;

	const BatchJoinProjectionGuardFixture system_var =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(makeVar(
				target_relid, SelfItemPointerAttributeNumber, TIDOID,
				-1, InvalidOid, 0)),
			13.0,
			37.0,
			5.0);
	const bool rejects_system_var =
		find_unparameterized_bioseq_custom_path(
			system_var.rel, /*target_seq_attno=*/3).scan == nullptr;

	Var* nulled_var = makeVar(
		target_relid, 2, INT4OID, -1, InvalidOid, 0);
	nulled_var->varnullingrels = bms_make_singleton(
		static_cast<int>(target_relid));
	const BatchJoinProjectionGuardFixture nulling_var =
		make_batch_join_projection_guard_fixture(
			target_relid,
			list_make1(nulled_var),
			13.0,
			37.0,
			5.0);
	const bool rejects_nulling_var =
		find_unparameterized_bioseq_custom_path(
			nulling_var.rel, /*target_seq_attno=*/3).scan == nullptr;

	Datum values[9] = {
		BoolGetDatum(accepts_exact_helper),
		BoolGetDatum(rejects_unrelated_phv),
		BoolGetDatum(rejects_volatile_mixed_expression),
		BoolGetDatum(preserves_wrapper_cost_owner),
		BoolGetDatum(accepts_plain_target_var),
		BoolGetDatum(rejects_plain_target_sequence_var),
		BoolGetDatum(rejects_whole_row_var),
		BoolGetDatum(rejects_system_var),
		BoolGetDatum(rejects_nulling_var),
	};
	bool nulls[9]{};
	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

[[nodiscard]] static bool path_target_contains_expression(
		const PathTarget* target, const Expr* expression) noexcept {
	if (target == nullptr || expression == nullptr) {
		return false;
	}
	for (Node* existing : PList<Node>(target->exprs)) {
		if (equal(existing, expression)) {
			return true;
		}
	}
	return false;
}

struct BatchJoinUnderlyingTargetVarCollector {
	PathTarget* destination = nullptr;
	Index target_relid = 0;
	AttrNumber target_seq_attno = InvalidAttrNumber;
	bool invalid = false;
};

struct batch_join_underlying_target_var_visitor {
	[[nodiscard]] pg_walk_step operator()(
			pg_expression_walk_cursor,
			pg_node_view node,
			BatchJoinUnderlyingTargetVarCollector& collector) const {
		Var* var = node.try_as<Var>();
		if (var == nullptr) {
			return pg_walk_step::descend;
		}
		if (var->varno != collector.target_relid) {
			return pg_walk_step::skip_subtree;
		}
		const bool exact_sequence_var =
			var->varattno == collector.target_seq_attno
			&& var->varattno > 0
			&& var->varlevelsup == 0
			&& bms_is_empty(var->varnullingrels);
		if (!exact_sequence_var
			&& !batch_join_is_safe_plain_target_var(
				reinterpret_cast<Node*>(var),
				collector.target_relid,
				collector.target_seq_attno)) {
			collector.invalid = true;
			return pg_walk_step::stop;
		}
		/* The sequence Var is installed explicitly below.  Preserve every other
		 * safe target Var, including a helper's load-bearing argument (normally
		 * t.pk) hidden below a PlaceHolderVar/compound wrapper.  Skipping the
		 * whole wrapper because it contains a bioseq helper must not erase that
		 * base-column dependency from the locator child and combined scan tlist. */
		if (!exact_sequence_var
			&& collector.destination != nullptr
			&& !path_target_contains_expression(
				collector.destination,
				reinterpret_cast<Expr*>(var))) {
			add_column_to_pathtarget(
				collector.destination,
				reinterpret_cast<Expr*>(copyObject(var)), 0);
		}
		return pg_walk_step::skip_subtree;
	}
};

[[nodiscard]] static bool batch_join_append_underlying_target_vars(
		PathTarget* destination,
		const PathTarget* source,
		const Index target_relid,
		const AttrNumber target_seq_attno) {
	if (destination == nullptr || source == nullptr || target_relid == 0
		|| target_seq_attno <= 0) {
		return false;
	}
	BatchJoinUnderlyingTargetVarCollector collector{
		destination, target_relid, target_seq_attno, false};
	batch_join_underlying_target_var_visitor visitor;
	for (Node* expression : PList<Node>(source->exprs)) {
		if (expression == nullptr) {
			continue;
		}
		pg_walk_expression_including_root(expression, collector, visitor);
		if (collector.invalid) {
			return false;
		}
	}
	return true;
}

static int next_batch_join_plan_token = 1;

[[nodiscard]] static Const* make_batch_join_cost_const(const Cost value) {
	if (!std::isfinite(value)) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin planner cost is not finite")));
	}
	return makeConst(
		FLOAT8OID,
		-1,
		InvalidOid,
		static_cast<int16>(sizeof(float8)),
		Float8GetDatum(value),
		false,
		FLOAT8PASSBYVAL);
}

[[nodiscard]] static Cost batch_join_cost_from_node(
		Node* node,
		const char* field) {
	Const* value = pg_node_try_as<Const>(node);
	if (value == nullptr || value->consttype != FLOAT8OID
		|| value->constisnull
		|| value->constlen != static_cast<int16>(sizeof(float8))
		|| value->constbyval != FLOAT8PASSBYVAL) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin cost ABI is invalid"),
			 errdetail("field=%s", field)));
	}
	const Cost decoded = DatumGetFloat8(value->constvalue);
	if (!std::isfinite(decoded)) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin cost ABI is not finite"),
			 errdetail("field=%s", field)));
	}
	return decoded;
}

struct PerRowOperandSourceProof {
	RelOptInfo* joinrel = nullptr;
	List* sequence_vars = NIL;
	bool invalid = false;
};

struct per_row_operand_source_visitor {
	[[nodiscard]] pg_walk_step operator()(
			pg_expression_walk_cursor, pg_node_view node,
			PerRowOperandSourceProof& proof) const {
		if (PlaceHolderVar* placeholder = node.try_as<PlaceHolderVar>()) {
			if (placeholder->phlevelsup != 0
				|| !bms_is_empty(placeholder->phnullingrels)) {
				proof.invalid = true;
				return pg_walk_step::stop;
			}
			return pg_walk_step::descend;
		}
		Var* var = node.try_as<Var>();
		if (var == nullptr) return pg_walk_step::descend;
		if (proof.joinrel == nullptr || var->varno == 0
			|| var->varattno <= 0 || var->varlevelsup != 0
			|| !bms_is_empty(var->varnullingrels)
			|| !bms_is_member(static_cast<int>(var->varno),
				proof.joinrel->relids)) {
			proof.invalid = true;
			return pg_walk_step::stop;
		}
		if (var->vartype == get_cached_stub_oids().bioseq_seq_type_oid) {
			bool seen = false;
			for (Node* existing : PList<Node>(proof.sequence_vars)) {
				if (equal(existing, var)) {
					seen = true;
					break;
				}
			}
			if (!seen) proof.sequence_vars = lappend(
				proof.sequence_vars, var);
		}
		return pg_walk_step::skip_subtree;
	}
};

[[nodiscard]] static bool per_row_collect_operand_sources(
		Node* expression, RelOptInfo* joinrel,
		PerRowOperandSourceProof& proof) {
	if (expression == nullptr || joinrel == nullptr
		|| contain_volatile_functions(expression)) return false;
	proof.joinrel = joinrel;
	per_row_operand_source_visitor visitor;
	pg_walk_expression_including_root(expression, proof, visitor);
	return !proof.invalid;
}

[[nodiscard]] static List* per_row_without_restriction(
		List* restrictions, RestrictInfo* owned) {
	List* filtered = NIL;
	for (RestrictInfo* candidate : PList<RestrictInfo>(restrictions)) {
		if (candidate != owned) filtered = lappend(filtered, candidate);
	}
	return filtered;
}

/* A RestrictInfo can migrate below a NestLoop through PathParamInfo (or sit
 * in another custom/join child) while still appearing in the incumbent's
 * joinrestrictinfo.  Removing only the top copy would leave a second live
 * mmsearch rendezvous.  The post-join owner therefore accepts an incumbent
 * only when the exact pointer is absent from every retained descendant. */
[[nodiscard]] static bool per_row_descendant_owns_restriction(
		Path const* path, RestrictInfo const* owned) noexcept {
	if (path == nullptr || owned == nullptr) return false;
	if (path->param_info != nullptr
		&& list_member_ptr(path->param_info->ppi_clauses, owned)) return true;
	if (IsA(path, CustomPath)) {
		auto const* custom = reinterpret_cast<CustomPath const*>(path);
		if (list_member_ptr(custom->custom_restrictinfo, owned)) return true;
		for (Path* child : PList<Path>(custom->custom_paths)) {
			if (per_row_descendant_owns_restriction(child, owned)) return true;
		}
		return false;
	}
	if (IsA(path, NestPath) || IsA(path, MergePath) || IsA(path, HashPath)) {
		auto const* join = reinterpret_cast<JoinPath const*>(path);
		if (list_member_ptr(join->joinrestrictinfo, owned)) return true;
		return per_row_descendant_owns_restriction(
				join->outerjoinpath, owned)
			|| per_row_descendant_owns_restriction(
				join->innerjoinpath, owned);
	}
	switch (path->pathtype) {
		case T_Result:
			return per_row_descendant_owns_restriction(
				reinterpret_cast<ProjectionPath const*>(path)->subpath, owned);
		case T_Material:
			return per_row_descendant_owns_restriction(
				reinterpret_cast<MaterialPath const*>(path)->subpath, owned);
		case T_Memoize:
			return per_row_descendant_owns_restriction(
				reinterpret_cast<MemoizePath const*>(path)->subpath, owned);
		case T_Sort:
			return per_row_descendant_owns_restriction(
				reinterpret_cast<SortPath const*>(path)->subpath, owned);
		case T_IncrementalSort:
			return per_row_descendant_owns_restriction(
				reinterpret_cast<IncrementalSortPath const*>(path)->spath.subpath,
				owned);
		case T_Append: {
			auto const* append = reinterpret_cast<AppendPath const*>(path);
			for (Path* child : PList<Path>(append->subpaths)) {
				if (per_row_descendant_owns_restriction(child, owned)) return true;
			}
			for (Path* child : PList<Path>(append->partial_subpaths)) {
				if (per_row_descendant_owns_restriction(child, owned)) return true;
			}
			return false;
		}
		case T_MergeAppend: {
			auto const* append = reinterpret_cast<MergeAppendPath const*>(path);
			for (Path* child : PList<Path>(append->subpaths)) {
				if (per_row_descendant_owns_restriction(child, owned)) return true;
			}
			return false;
		}
		default:
			return false;
	}
}

[[nodiscard]] static Path* per_row_reconstruct_inner_join_path(
		PlannerInfo* root, RelOptInfo* joinrel, Path* incumbent,
		RestrictInfo* owned, JoinPathExtraData const& original_extra) {
	if (root == nullptr || joinrel == nullptr || incumbent == nullptr
		|| owned == nullptr) return nullptr;
	JoinPath* join = nullptr;
	if (IsA(incumbent, NestPath)) {
		join = &reinterpret_cast<NestPath*>(incumbent)->jpath;
	} else if (IsA(incumbent, MergePath)) {
		join = &reinterpret_cast<MergePath*>(incumbent)->jpath;
	} else if (IsA(incumbent, HashPath)) {
		join = &reinterpret_cast<HashPath*>(incumbent)->jpath;
	} else {
		return nullptr;
	}
	if (join->jointype != JOIN_INNER
		|| !list_member_ptr(join->joinrestrictinfo, owned)
		|| join->outerjoinpath == nullptr || join->innerjoinpath == nullptr
		|| per_row_descendant_owns_restriction(join->outerjoinpath, owned)
		|| per_row_descendant_owns_restriction(join->innerjoinpath, owned)
		|| batch_join_path_contains_parallel_execution(incumbent)) {
		return nullptr;
	}
	List* filtered_restrictions = per_row_without_restriction(
		join->joinrestrictinfo, owned);
	JoinPathExtraData filtered_extra = original_extra;
	filtered_extra.restrictlist = per_row_without_restriction(
		original_extra.restrictlist, owned);
	filtered_extra.mergeclause_list = per_row_without_restriction(
		original_extra.mergeclause_list, owned);
	Relids required_outer = PATH_REQ_OUTER(&join->path);
	JoinCostWorkspace workspace{};
	if (IsA(incumbent, NestPath)) {
		initial_cost_nestloop(root, &workspace, JOIN_INNER,
			join->outerjoinpath, join->innerjoinpath, &filtered_extra);
		return reinterpret_cast<Path*>(create_nestloop_path(
			root, joinrel, JOIN_INNER, &workspace, &filtered_extra,
			join->outerjoinpath, join->innerjoinpath,
			filtered_restrictions, join->path.pathkeys, required_outer));
	}
	if (IsA(incumbent, MergePath)) {
		auto* merge = reinterpret_cast<MergePath*>(incumbent);
		List* mergeclauses = per_row_without_restriction(
			merge->path_mergeclauses, owned);
		if (mergeclauses == NIL) return nullptr;
		initial_cost_mergejoin(root, &workspace, JOIN_INNER,
			mergeclauses, join->outerjoinpath, join->innerjoinpath,
			merge->outersortkeys, merge->innersortkeys, &filtered_extra);
		return reinterpret_cast<Path*>(create_mergejoin_path(
			root, joinrel, JOIN_INNER, &workspace, &filtered_extra,
			join->outerjoinpath, join->innerjoinpath,
			filtered_restrictions, join->path.pathkeys, required_outer,
			mergeclauses, merge->outersortkeys, merge->innersortkeys));
	}
	auto* hash = reinterpret_cast<HashPath*>(incumbent);
	List* hashclauses = per_row_without_restriction(
		hash->path_hashclauses, owned);
	if (hashclauses == NIL) return nullptr;
	const bool parallel_hash = join->innerjoinpath->parallel_aware;
	initial_cost_hashjoin(root, &workspace, JOIN_INNER,
		hashclauses, join->outerjoinpath, join->innerjoinpath,
		&filtered_extra, parallel_hash);
	return reinterpret_cast<Path*>(create_hashjoin_path(
		root, joinrel, JOIN_INNER, &workspace, &filtered_extra,
		join->outerjoinpath, join->innerjoinpath, parallel_hash,
		filtered_restrictions, required_outer, hashclauses));
}

[[nodiscard]] static bool per_row_add_transformed_mmsearch_paths(
		PlannerInfo* root, RelOptInfo* joinrel, JoinPathExtraData* extra,
		RestrictInfo* restriction, FuncExpr* mmsearch,
		FuncExpr* executable_mmsearch,
		bioseq::indexing::IndexReadRequirement requirement,
		BioseqStubOids const& oids) {
	if (root == nullptr || joinrel == nullptr || extra == nullptr
		|| restriction == nullptr || mmsearch == nullptr
		|| executable_mmsearch == nullptr) return false;
	PList<Node> operands(mmsearch->args);
	Node* target_expression = operands.first();
	Node* query_expression = operands.second();
	const bool transformed_target =
		bioseq_expr_uses_lazy_sequence_projection(target_expression);
	const bool transformed_query =
		bioseq_expr_uses_lazy_sequence_projection(query_expression);
	if (!transformed_target && !transformed_query) return false;
	if (exprType(target_expression) != oids.bioseq_seq_type_oid
		|| exprType(query_expression) != oids.bioseq_seq_type_oid
		|| !batch_join_typmod_is_supported(exprTypmod(target_expression))
		|| exprTypmod(target_expression) != exprTypmod(query_expression)
		|| exprCollation(target_expression) != exprCollation(query_expression)
		|| requirement
			== bioseq::indexing::IndexReadRequirement::require_compatible_persisted) {
		return false;
	}
	PerRowOperandSourceProof target_sources{};
	PerRowOperandSourceProof query_sources{};
	if (!per_row_collect_operand_sources(
			target_expression, joinrel, target_sources)
		|| !per_row_collect_operand_sources(
			query_expression, joinrel, query_sources)) return false;
	Var* target_source = nullptr;
	RangeTblEntry* target_rte = nullptr;
	for (Var* candidate : PList<Var>(target_sources.sequence_vars)) {
		if (candidate->varno > static_cast<Index>(
				list_length(root->parse->rtable))) continue;
		RangeTblEntry* rte = rt_fetch(candidate->varno, root->parse->rtable);
		if (rte != nullptr && rte->rtekind == RTE_RELATION
			&& rte->securityQuals == NIL && OidIsValid(rte->relid)
			&& IsBioseqColumnTableAmTable(rte->relid)) {
			target_source = candidate;
			target_rte = rte;
			break;
		}
	}
	if (target_source == nullptr || target_rte == nullptr) return false;
	for (Var* source : PList<Var>(query_sources.sequence_vars)) {
		if (source->varno > static_cast<Index>(
				list_length(root->parse->rtable))) return false;
		RangeTblEntry* rte = rt_fetch(source->varno, root->parse->rtable);
		if (rte == nullptr || rte->rtekind != RTE_RELATION
			|| rte->securityQuals != NIL) return false;
	}

	PathTarget* required_target = create_empty_pathtarget();
	List* required_vars = NIL;
	constexpr int pull_flags = PVC_RECURSE_AGGREGATES
		| PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	for (Node* expression : PList<Node>(joinrel->reltarget->exprs)) {
		required_vars = list_concat(required_vars,
			pull_var_clause(expression, pull_flags));
	}
	required_vars = list_concat(required_vars,
		pull_var_clause(target_expression, pull_flags));
	required_vars = list_concat(required_vars,
		pull_var_clause(query_expression, pull_flags));
	add_new_columns_to_pathtarget(required_target, required_vars);
	set_pathtarget_cost_width(root, required_target);

	PathTarget* recipe_target = copy_pathtarget(required_target);
	if (!path_target_contains_expression(
			recipe_target, reinterpret_cast<Expr*>(target_expression))) {
		add_column_to_pathtarget(recipe_target,
			reinterpret_cast<Expr*>(copyObject(target_expression)), 0);
	}
	if (!path_target_contains_expression(
			recipe_target, reinterpret_cast<Expr*>(query_expression))) {
		add_column_to_pathtarget(recipe_target,
			reinterpret_cast<Expr*>(copyObject(query_expression)), 0);
	}
	set_pathtarget_cost_width(root, recipe_target);
	SliceProjectIrBuilder builder{
		.oids = &oids, .command_type = CMD_UNKNOWN};
	if (!slice_project_discover_target(recipe_target, builder)) return false;
	const std::uint32_t target_producer = slice_project_add_producer(
		builder, target_expression);
	const std::uint32_t query_producer = slice_project_add_producer(
		builder, query_expression);
	slice_project_add_boundary(builder, target_expression, target_producer,
		materialization_mechanism::packed_full,
		materialization_reason::mmsearch_target, true);
	slice_project_add_boundary(builder, query_expression, query_producer,
		materialization_mechanism::packed_full,
		materialization_reason::mmsearch_query, true);

	PList<Path> owners;
	List* incumbent_paths = list_copy(joinrel->pathlist);
	Cost incumbent_floor = std::numeric_limits<Cost>::max();
	for (Path* incumbent : PList<Path>(incumbent_paths)) {
		if (incumbent != nullptr && std::isfinite(incumbent->total_cost)
			&& incumbent->total_cost >= 0) {
			incumbent_floor = std::min(
				incumbent_floor, incumbent->total_cost);
		}
	}
	for (Path* incumbent : PList<Path>(incumbent_paths)) {
		Path* reconstructed = per_row_reconstruct_inner_join_path(
			root, joinrel, incumbent, restriction, *extra);
		if (reconstructed == nullptr) continue;
		Path* private_join = reinterpret_cast<Path*>(copyObject(reconstructed));
		private_join->pathtarget = copy_pathtarget(required_target);
		bool tagged_source = false;
		private_join = slice_project_tag_owned_path_tree(
			private_join, builder, tagged_source);
		if (!tagged_source) continue;
		Path* source_path = reinterpret_cast<Path*>(create_projection_path(
			root, joinrel, private_join, required_target));

		auto* recipe_path = makeNode(CustomPath);
		recipe_path->path.pathtype = T_CustomScan;
		recipe_path->path.parent = joinrel;
		recipe_path->path.pathtarget = copy_pathtarget(recipe_target);
		recipe_path->path.param_info = source_path->param_info;
		recipe_path->path.parallel_aware = false;
		recipe_path->path.parallel_safe = false;
		recipe_path->path.parallel_workers = 0;
		recipe_path->path.rows = source_path->rows;
		recipe_path->path.disabled_nodes = source_path->disabled_nodes;
		QualCost recipe_cost{};
		cost_qual_eval(&recipe_cost, recipe_target->exprs, root);
		recipe_path->path.startup_cost = source_path->startup_cost
			+ recipe_cost.startup;
		recipe_path->path.total_cost = source_path->total_cost
			+ recipe_cost.startup + std::max(0.0, source_path->rows)
				* recipe_cost.per_tuple;
		recipe_path->path.pathkeys = source_path->pathkeys;
		recipe_path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
		recipe_path->custom_paths = list_make1(source_path);
		recipe_path->custom_private = list_make5(
			slice_project_header(builder), copyObject(builder.expressions),
			copyObject(builder.nodes), copyObject(builder.records),
			copyObject(builder.output_bindings));
		recipe_path->custom_restrictinfo = NIL;
		recipe_path->methods = &bioseq_slice_project_path_methods();

		QualCost operand_cost{};
		cost_qual_eval(&operand_cost,
			mmsearch_operand_cost_exprs(executable_mmsearch, oids), root);
		const std::uint64_t target_symbols =
			estimate_mmsearch_operand_symbols(target_expression, oids);
		const std::uint64_t query_symbols =
			estimate_mmsearch_operand_symbols(query_expression, oids);
		const owned_mmsearch_cost alignment_cost = estimate_owned_mmsearch_cost(
			target_symbols, query_symbols, recipe_path->path.rows,
			/*index_read_requirement=*/false,
			static_cast<std::uint32_t>(kBioseqRuntimeBufferReserve),
			bioseq::parallelism::saturating_add(
				target_symbols, query_symbols));
		const Cost startup = std::max<Cost>(0,
			recipe_path->path.startup_cost + operand_cost.startup
				+ alignment_cost.startup);
		const Cost total = std::max(startup,
			recipe_path->path.total_cost + operand_cost.startup
				+ alignment_cost.startup
				+ std::max(0.0, recipe_path->path.rows)
					* (operand_cost.per_tuple
						+ alignment_cost.per_input_tuple + cpu_tuple_cost));
		if (!std::isfinite(startup) || !std::isfinite(total)) continue;

		auto* owner = makeNode(CustomPath);
		owner->path.pathtype = T_CustomScan;
		owner->path.parent = joinrel;
		owner->path.pathtarget = copy_pathtarget(joinrel->reltarget);
		owner->path.param_info = recipe_path->path.param_info;
		owner->path.parallel_aware = false;
		owner->path.parallel_safe = false;
		owner->path.parallel_workers = 0;
		owner->path.rows = recipe_path->path.rows;
		owner->path.disabled_nodes = recipe_path->path.disabled_nodes;
		owner->path.startup_cost = startup;
		owner->path.total_cost = total;
		owner->path.pathkeys = recipe_path->path.pathkeys;
		owner->flags = CUSTOMPATH_SUPPORT_PROJECTION;
		owner->custom_paths = list_make1(recipe_path);
		owner->custom_restrictinfo = list_make1(restriction);
		const auto typmod = compact_sequence_stdc_type_params::decode(
			exprTypmod(target_expression));
		owner->custom_private = list_make5(
			makeInteger(kBioseqPerRowJoinPlanVersion),
			makeInteger(static_cast<int>(target_rte->relid)),
			makeInteger(static_cast<int>(target_source->varno)),
			makeInteger(static_cast<int>(target_source->varattno)),
			copyObject(target_expression));
		owner->custom_private = lappend(
			owner->custom_private, copyObject(query_expression));
		owner->custom_private = lappend(
			owner->custom_private, copyObject(executable_mmsearch));
		owner->custom_private = lappend(owner->custom_private,
			makeInteger(typmod.alphabet_code));
		owner->custom_private = lappend(owner->custom_private,
			makeInteger(typmod.storage_unit_byte_width));
		owner->custom_private = lappend(owner->custom_private,
			makeInteger(static_cast<int>(
				bioseq::indexing::IndexReadRequirement::require_absent)));
		owner->custom_private = lappend(owner->custom_private,
			make_batch_join_cost_const(startup));
		owner->custom_private = lappend(owner->custom_private,
			make_batch_join_cost_const(total));
		owner->custom_private = lappend(owner->custom_private,
			make_batch_join_cost_const(std::isfinite(incumbent_floor)
				? incumbent_floor : total));
		Assert(list_length(owner->custom_private)
			== kBioseqPerRowJoinPathPrivateFields);
		owner->methods = &BioseqPerRowJoinPathMethods;
		owners.append(reinterpret_cast<Path*>(owner));
	}
	if (owners.empty()) return false;
	PList<Path> retained;
	for (Path* incumbent : PList<Path>(joinrel->pathlist)) {
		if (incumbent != nullptr && IsA(incumbent, CustomPath)
			&& list_member_ptr(
				reinterpret_cast<CustomPath*>(incumbent)->custom_restrictinfo,
				restriction)) retained.append(incumbent);
	}
	joinrel->pathlist = retained.release();
	joinrel->partial_pathlist = NIL;
	for (Path* owner : owners) add_path(joinrel, owner);
	return true;
}

static void
BioseqBatchJoinSetJoinPathlistHook(
		PlannerInfo* root,
		RelOptInfo* joinrel,
		RelOptInfo* outerrel,
		RelOptInfo* innerrel,
		JoinType jointype,
		JoinPathExtraData* extra) {
	if (PreviousSetJoinPathlistHook != nullptr) {
		PreviousSetJoinPathlistHook(
			root, joinrel, outerrel, innerrel, jointype, extra);
	}
	if (root == nullptr || joinrel == nullptr || outerrel == nullptr
		|| innerrel == nullptr || extra == nullptr || jointype != JOIN_INNER
		|| outerrel->reloptkind != RELOPT_BASEREL
		|| innerrel->reloptkind != RELOPT_BASEREL) {
		return;
	}
	// shared_preload_libraries installs this hook cluster-wide. Databases that
	// have not installed Bioseq must retain ordinary PostgreSQL join planning.
	if (!OidIsValid(get_extension_oid("bioseq", /*missing_ok=*/true))) {
		return;
	}

	const BioseqStubOids& oids = get_cached_stub_oids();
	std::size_t semantic_calls = 0;
	RestrictInfo* restriction = nullptr;
	for (RestrictInfo* candidate : PList<RestrictInfo>(extra->restrictlist)) {
		const std::size_t calls = count_mmsearch_occurrences(
			reinterpret_cast<Node*>(candidate->clause), oids);
		semantic_calls += calls;
		if (calls != 0) restriction = candidate;
	}
	if (semantic_calls > 1) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("stacked mmsearch join qualifiers are unsupported"),
			 errhint("Split the alignment predicates into separate query levels.")));
	}
	if (semantic_calls != 1 || restriction == nullptr) {
		return;
	}
	FuncExpr* mmsearch = restriction == nullptr
		? nullptr
		: pg_node_try_as<FuncExpr>(
			reinterpret_cast<Node*>(restriction->clause));
	if (mmsearch == nullptr || !is_mmsearch_funcid(mmsearch->funcid, oids)
		|| (mmsearch->funcid == oids.mmsearch_oid
			&& list_length(mmsearch->args) != 2)
		|| (mmsearch->funcid == oids.mmsearch_with_opts_oid
			&& list_length(mmsearch->args) != 3)
		|| root->parse == nullptr || root->parse->hasRowSecurity
		|| restriction->pseudoconstant || restriction->security_level != 0
		|| !restriction_is_securely_promotable(restriction, joinrel)) {
		return;
	}
	for (RestrictInfo* sibling : PList<RestrictInfo>(extra->restrictlist)) {
		if (sibling != restriction && contain_volatile_functions(
				reinterpret_cast<Node*>(sibling->clause))) {
			/* The first release does not claim an evaluation-order-preserving
			 * split around an effectful sibling. */
			return;
		}
	}
	List* residual_join_restrictions = NIL;
	for (RestrictInfo* candidate : PList<RestrictInfo>(extra->restrictlist)) {
		if (candidate != restriction) {
			residual_join_restrictions = lappend(
				residual_join_restrictions, candidate);
		}
	}
	PList<Node> arguments(mmsearch->args);
	Var* target_var = pg_node_try_as<Var>(arguments.first());
	Var* query_var = pg_node_try_as<Var>(arguments.second());
	/* Eligibility is non-throwing: fold only the option expression on a
	 * private copy. A typed NULL Const means default options. */
	auto* batch_mmsearch = static_cast<FuncExpr*>(copyObject(mmsearch));
	if (list_length(mmsearch->args) == 3) {
		Node* folded_options = eval_const_expressions(
			root, reinterpret_cast<Node*>(copyObject(arguments.third())));
		Const* options = pg_node_try_as<Const>(folded_options);
		if (options == nullptr
			|| options->consttype != oids.bioseq_aligner_options_type_oid) {
			return;
		}
		lfirst(list_nth_cell(batch_mmsearch->args, 2)) = folded_options;
	}
	/* Decode options before choosing the raw two-child or transformed
	 * one-child ownership shape. */
	bioseq::simd::active_target::Minimap2Params planned_params =
		build_params_from_gucs();
	bioseq::indexing::IndexReadRequirement planned_requirement =
		bioseq::indexing::IndexReadRequirement::allow;
	(void)apply_aligner_options_override(
		batch_mmsearch, planned_params, planned_requirement, oids);
	finalize_simd_chaining_policy(planned_params);
	const bool transformed_operands =
		bioseq_expr_uses_lazy_sequence_projection(arguments.first())
		|| bioseq_expr_uses_lazy_sequence_projection(arguments.second());
	if (transformed_operands) {
		(void)per_row_add_transformed_mmsearch_paths(
			root, joinrel, extra, restriction, mmsearch,
			batch_mmsearch, planned_requirement, oids);
		return;
	}
	if (target_var == nullptr || query_var == nullptr
		|| target_var->varattno <= 0 || query_var->varattno <= 0
		|| target_var->varlevelsup != 0 || query_var->varlevelsup != 0
		|| !bms_is_empty(target_var->varnullingrels)
		|| !bms_is_empty(query_var->varnullingrels)
		|| target_var->vartype != oids.bioseq_seq_type_oid
		|| query_var->vartype != oids.bioseq_seq_type_oid
		|| !batch_join_typmod_is_supported(target_var->vartypmod)
		|| target_var->vartypmod != query_var->vartypmod
		|| target_var->varcollid != query_var->varcollid) {
		return;
	}
	RelOptInfo* target_rel = nullptr;
	RelOptInfo* query_rel = nullptr;
	if (target_var->varno == outerrel->relid
		&& query_var->varno == innerrel->relid) {
		target_rel = outerrel;
		query_rel = innerrel;
	} else if (target_var->varno == innerrel->relid
			&& query_var->varno == outerrel->relid) {
		target_rel = innerrel;
		query_rel = outerrel;
	} else {
		return;
	}
	List* target_residual_vars = NIL;
	List* query_residual_vars = NIL;
	bool unsupported_residual_var = false;
	constexpr int residual_pull_flags = PVC_RECURSE_AGGREGATES
		| PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS;
	for (RestrictInfo* residual : PList<RestrictInfo>(
			residual_join_restrictions)) {
		List* variables = pull_var_clause(
			reinterpret_cast<Node*>(residual->clause), residual_pull_flags);
		for (Node* variable : PList<Node>(variables)) {
			Var* var = pg_node_try_as<Var>(variable);
			if (var == nullptr || var->varlevelsup != 0
				|| var->varattno <= 0 || !bms_is_empty(var->varnullingrels)) {
				unsupported_residual_var = true;
				break;
			}
			List** destination = var->varno == target_rel->relid
				? &target_residual_vars
				: (var->varno == query_rel->relid
					? &query_residual_vars : nullptr);
			if (destination == nullptr) {
				unsupported_residual_var = true;
				break;
			}
			bool seen = false;
			for (Node* existing : PList<Node>(*destination)) {
				if (equal(existing, variable)) {
					seen = true;
					break;
				}
			}
			if (!seen) *destination = lappend(
				*destination, copyObject(variable));
		}
		if (unsupported_residual_var) break;
	}
	if (unsupported_residual_var) return;
	RangeTblEntry* target_rte = rt_fetch(target_var->varno, root->parse->rtable);
	RangeTblEntry* query_rte = rt_fetch(query_var->varno, root->parse->rtable);
	if (target_rte == nullptr || target_rte->rtekind != RTE_RELATION
		|| query_rte == nullptr || query_rte->rtekind != RTE_RELATION
		|| target_rte->securityQuals != NIL || query_rte->securityQuals != NIL
		|| !OidIsValid(target_rte->relid)
		|| !IsBioseqColumnTableAmTable(target_rte->relid)
		|| batch_join_target_expression_is_forbidden(
			reinterpret_cast<Node*>(root->processed_tlist), target_var,
			/*allow_mmsearch_qualifier=*/false)
		|| batch_join_target_restrictions_are_forbidden(
			target_rel->baserestrictinfo, target_var,
			/*allow_mmsearch_qualifier=*/true)) {
		return;
	}
	const auto target_qual_features = detect_restrictinfo_feature_flags(
		target_rel->baserestrictinfo, oids);
	const auto query_qual_features = detect_restrictinfo_feature_flags(
		query_rel->baserestrictinfo, oids);
	/* The BatchJoin locator child deliberately does not execute its target
	 * BioseqScan's alignment runtime.  A target-side base mmsearch therefore
	 * has no safe owner after locator conversion; leave this shape to the
	 * ordinary parameterized scan, which can consume both the join Param and
	 * its base qual.  Query-side base mmsearch remains child-owned. */
	if (!target_qual_features.has_value()
		|| !query_qual_features.has_value()
		|| target_qual_features->has_mmsearch
		|| target_qual_features->any_match_projection
		|| query_qual_features->any_match_projection) {
		return;
	}
	const BatchJoinBioseqPathCandidate target_candidate =
		find_unparameterized_bioseq_custom_path(
			target_rel, target_var->varattno);
	CustomPath* target_path = target_candidate.scan;
	Path* query_path = query_rel->cheapest_total_path;
	if (target_path == nullptr || target_candidate.locator_target == nullptr
		|| query_path == nullptr
		|| query_path->param_info != nullptr
		|| batch_join_path_contains_parallel_execution(query_path)) {
		return;
	}
	if (query_residual_vars != NIL) {
		PathTarget* query_target = copy_pathtarget(query_path->pathtarget);
		add_new_columns_to_pathtarget(query_target, query_residual_vars);
		set_pathtarget_cost_width(root, query_target);
		query_path = reinterpret_cast<Path*>(create_projection_path(
			root, query_path->parent, query_path, query_target));
	}

	auto* locator_path = makeNode(CustomPath);
	*locator_path = *target_path;
	/* Preserve the wrapper's conservative projection cost when a helper-only
	 * ProjectionPath was unwrapped. The locator child does not execute those
	 * helpers, but underpricing the custom join is worse than retaining their
	 * planner estimate. */
	locator_path->path.startup_cost = target_candidate.cost_owner->startup_cost;
	locator_path->path.total_cost = target_candidate.cost_owner->total_cost;
	locator_path->path.rows = target_candidate.cost_owner->rows;
	locator_path->path.pathtarget = create_empty_pathtarget();
	auto append_locator_target = [locator_path](const PathTarget* source) {
		if (source == nullptr) return;
		for (Node* expression : PList<Node>(source->exprs)) {
			if (bioseq_expr_uses_bioseq_projection(expression)
				|| path_target_contains_expression(
					locator_path->path.pathtarget,
					reinterpret_cast<Expr*>(expression))) {
				continue;
			}
			add_column_to_pathtarget(
				locator_path->path.pathtarget,
				reinterpret_cast<Expr*>(copyObject(expression)), 0);
		}
	};
	/* Preserve the scan's ordinary expressions and safe plain expressions
	 * introduced by the wrapper projection. Helper-bearing expressions are
	 * parent-owned and handled by the recursive Var pass below. */
	append_locator_target(target_path->path.pathtarget);
	append_locator_target(target_candidate.locator_target);
	add_new_columns_to_pathtarget(
		locator_path->path.pathtarget, target_residual_vars);
	/* A helper-bearing expression is intentionally not evaluated by the
	 * locator child, but PostgreSQL setrefs still requires its underlying base
	 * Vars to exist in the child/combined tlist.  The wrapper PathTarget is the
	 * usual owner; joinrel->reltarget is the authoritative fallback for an
	 * upper compound projection. */
	if (!batch_join_append_underlying_target_vars(
			locator_path->path.pathtarget,
			target_candidate.locator_target,
			target_rel->relid,
			target_var->varattno)
		|| !batch_join_append_underlying_target_vars(
			locator_path->path.pathtarget,
			joinrel->reltarget,
			target_rel->relid,
			target_var->varattno)) {
		return;
	}
	locator_path->custom_private = static_cast<List*>(
		copyObject(target_path->custom_private));
	locator_path->custom_paths = static_cast<List*>(
		copyObject(target_path->custom_paths));
	locator_path->custom_restrictinfo = NIL;
	if (!path_target_contains_expression(
			locator_path->path.pathtarget,
			reinterpret_cast<Expr*>(target_var))) {
		add_column_to_pathtarget(
			locator_path->path.pathtarget,
			reinterpret_cast<Expr*>(copyObject(target_var)), 0);
	}
	Const* hidden_rowno = makeConst(
		INT8OID, -1, InvalidOid, 8, Int64GetDatum(0), false, true);
	add_column_to_pathtarget(
		locator_path->path.pathtarget,
		reinterpret_cast<Expr*>(hidden_rowno), 0);
	set_pathtarget_cost_width(root, locator_path->path.pathtarget);
	if (next_batch_join_plan_token <= 0
		|| next_batch_join_plan_token == (std::numeric_limits<int>::max)()) {
		next_batch_join_plan_token = 1;
	}
	const int token = next_batch_join_plan_token++;
	List* locator_tag = list_make4(
		makeInteger(kBioseqLocatorPassthroughMagic),
		makeInteger(kBioseqBatchJoinPlanVersion),
		makeInteger(token),
		makeInteger(0));
	locator_path->custom_private = lappend(
		locator_path->custom_private, locator_tag);
	locator_path->path.parallel_safe = false;
	locator_path->path.parallel_aware = false;
	locator_path->path.parallel_workers = 0;

	const auto typmod = compact_sequence_stdc_type_params::decode(
		target_var->vartypmod);
	auto* join_path = makeNode(CustomPath);
	join_path->path.pathtype = T_CustomScan;
	join_path->path.parent = joinrel;
	join_path->path.pathtarget = copy_pathtarget(joinrel->reltarget);
	join_path->path.param_info = nullptr;
	join_path->path.parallel_aware = false;
	join_path->path.parallel_safe = false;
	join_path->path.parallel_workers = 0;
	join_path->path.pathkeys = NIL;
	join_path->path.rows = std::max(1.0, joinrel->rows);
	/* The query child is fully materialized before the first output, so its
	 * complete cost is startup.  The target child is read exactly once; never
	 * underprice this as startup-only and force an unadmittable custom join. */
	const double planned_query_rows = std::max(1.0, query_path->rows);
	const double planned_query_width = std::max(
		1, query_path->pathtarget == nullptr
			? 1 : query_path->pathtarget->width);
	const Cost query_materialization_cost = planned_query_rows
		* (cpu_tuple_cost
			+ cpu_operator_cost * (1.0 + planned_query_width / 64.0));
	const Cost output_cost = join_path->path.rows * cpu_tuple_cost;
	QualCost residual_join_cost{};
	if (residual_join_restrictions != NIL) {
		List* residual_expressions = NIL;
		for (RestrictInfo* residual : PList<RestrictInfo>(
				residual_join_restrictions)) {
			residual_expressions = lappend(
				residual_expressions, residual->clause);
		}
		cost_qual_eval(&residual_join_cost, residual_expressions, root);
	}
	QualCost operand_cost{};
	cost_qual_eval(&operand_cost,
		mmsearch_operand_cost_exprs(batch_mmsearch, oids), root);
	PList<Node> owned_operands(batch_mmsearch->args);
	const std::uint64_t target_symbols = estimate_mmsearch_operand_symbols(
		owned_operands.first(), oids);
	const std::uint64_t query_symbols = estimate_mmsearch_operand_symbols(
		owned_operands.second(), oids);
	const std::uint64_t admitted_bytes =
		bioseq::parallelism::saturating_add(target_symbols, query_symbols);
	const owned_mmsearch_cost alignment_cost = estimate_owned_mmsearch_cost(
		target_symbols, query_symbols, locator_path->path.rows,
		planned_requirement
			!= bioseq::indexing::IndexReadRequirement::require_absent,
		static_cast<std::uint32_t>(std::min<std::size_t>(
			kBioseqRuntimeBufferReserve,
			(std::numeric_limits<std::uint32_t>::max)())),
		admitted_bytes);
	const Cost honest_startup_cost = std::max<Cost>(0,
		query_path->total_cost
			+ locator_path->path.startup_cost
			+ query_materialization_cost + operand_cost.startup
			+ alignment_cost.startup + residual_join_cost.startup);
	const Cost honest_total_cost = std::max(
		honest_startup_cost,
		std::max<Cost>(0, query_path->total_cost
			+ locator_path->path.total_cost
			+ query_materialization_cost
			+ operand_cost.startup + alignment_cost.startup
			+ residual_join_cost.startup
			+ output_cost + std::max(0.0, locator_path->path.rows)
				* (operand_cost.per_tuple
					+ alignment_cost.per_input_tuple)
			+ std::max(0.0, join_path->path.rows)
				* residual_join_cost.per_tuple));
	if (!std::isfinite(honest_startup_cost)
		|| !std::isfinite(honest_total_cost)) {
		return;
	}

	/* Retain the best incumbent as EXPLAIN evidence only.  Path selection is
	 * structural once ownership is established, never a negative sentinel. */
	Cost incumbent_cost_floor = honest_total_cost;
	for (Path* incumbent : PList<Path>(joinrel->pathlist)) {
		if (incumbent == nullptr) continue;
		if (std::isfinite(incumbent->total_cost)
			&& incumbent->total_cost >= 0) {
			incumbent_cost_floor = std::min(
				incumbent_cost_floor, incumbent->total_cost);
		}
	}
	join_path->path.startup_cost = std::max<Cost>(0, honest_startup_cost);
	join_path->path.total_cost = std::max(
		join_path->path.startup_cost, std::max<Cost>(0, honest_total_cost));
	join_path->flags = 0;
	join_path->custom_restrictinfo = list_copy(extra->restrictlist);
	join_path->custom_paths = list_make2(locator_path, query_path);
	join_path->custom_private = list_make5(
		makeInteger(kBioseqBatchJoinPlanVersion),
		makeInteger(token),
		makeInteger(static_cast<int>(target_rte->relid)),
		makeInteger(target_var->varattno),
		copyObject(target_var));
	join_path->custom_private = lappend(
		join_path->custom_private, copyObject(query_var));
	join_path->custom_private = lappend(
		join_path->custom_private, makeInteger(typmod.alphabet_code));
	join_path->custom_private = lappend(
		join_path->custom_private,
		makeInteger(typmod.storage_unit_byte_width));
	join_path->custom_private = lappend(
		join_path->custom_private, batch_mmsearch);
	join_path->custom_private = lappend(
		join_path->custom_private,
		makeInteger(static_cast<int>(planned_requirement)));
	join_path->custom_private = lappend(
		join_path->custom_private,
		make_batch_join_cost_const(honest_startup_cost));
	join_path->custom_private = lappend(
		join_path->custom_private,
		make_batch_join_cost_const(honest_total_cost));
	join_path->custom_private = lappend(
		join_path->custom_private,
		make_batch_join_cost_const(incumbent_cost_floor));
	Assert(list_length(join_path->custom_private)
		== kBioseqBatchJoinPathPrivateFields);
	join_path->methods = &BioseqBatchJoinPathMethods;
	auto retain_existing_owner = [restriction](List* paths) {
		PList<Path> survivors;
		for (Path* incumbent : PList<Path>(paths)) {
			if (incumbent != nullptr && IsA(incumbent, CustomPath)
				&& list_member_ptr(
					reinterpret_cast<CustomPath*>(incumbent)->custom_restrictinfo,
					restriction)) {
				survivors.append(incumbent);
			}
		}
		return survivors.release();
	};
	joinrel->pathlist = retain_existing_owner(joinrel->pathlist);
	joinrel->partial_pathlist = retain_existing_owner(joinrel->partial_pathlist);
	add_path(joinrel, &join_path->path);
}


/*
 * RemovePathsByPredicate removes the paths that removePathPredicate
 * evaluates to true from pathlist of given rel.
 */
static void
RemovePathsByPredicate(RelOptInfo *rel, PathPredicate removePathPredicate)
{
	bioseq_log<DBG>("[bioseq] pg: RemovePathsByPredicate() entered");
	PList<Path> filteredPathList;
	PList<Path> filteredPartialPathList;

	for(auto path : PList<Path>(rel->pathlist))
	{
		if (!removePathPredicate(path))
		{
			filteredPathList.append(path);
		}
	}

	rel->pathlist = filteredPathList.release();

	for(auto path : PList<Path>(rel->partial_pathlist))
	{
		if (!removePathPredicate(path))
		{
			filteredPartialPathList.append(path);
		}
	}

	rel->partial_pathlist = filteredPartialPathList.release();
}


/*
 * IsNotIndexPath returns true if given path is not an IndexPath.
 */
static bool
IsNotIndexPath(Path *path)
{
	return !pg_node_view{pg_as_node(path)}.is<IndexPath>();
}


/*
 * CreateBioseqColumnSeqScanPath returns Path for sequential scan on columnar
 * table with relationId.
 */
static Path *
CreateBioseqColumnSeqScanPath(PlannerInfo *root, RelOptInfo *rel, Oid relationId)
{
	bioseq_log<DBG>("[bioseq] pg: CreateBioseqColumnSeqScanPath() entered");
	/* columnar doesn't support parallel scan */
	int parallelWorkers = 0;

	Relids requiredOuter = rel->lateral_relids;
	Path *path = create_seqscan_path(root, rel, requiredOuter, parallelWorkers);
	CostBioseqColumnSeqPath(rel, relationId, path);
	return path;
}


/*
 * CostBioseqColumnPaths re-costs paths of given RelOptInfo for
 * columnar table with relationId.
 */
static void
CostBioseqColumnPaths(PlannerInfo *root, RelOptInfo *rel, Oid relationId)
{
	bioseq_log<DBG>("[bioseq] pg: CostBioseqColumnPaths() entered");
	for(auto path : PList<Path>(rel->pathlist))
	{
		if (pg_node_view{pg_as_node(path)}.is<IndexPath>())
		{
			/*
			 * Since we don't provide implementations for scan_bitmap_next_block
			 * & scan_bitmap_next_tuple, postgres doesn't generate bitmap index
			 * scan paths for columnar tables already (see related comments in
			 * TableAmRoutine). For this reason, we only consider IndexPath's
			 * here.
			 */
			CostBioseqColumnIndexPath(root, rel, relationId, reinterpret_cast<IndexPath *>(path));
		}
		else if (path->pathtype == T_SeqScan)
		{
			CostBioseqColumnSeqPath(rel, relationId, path);
		}
	}
}


/*
 * CostBioseqColumnIndexPath re-costs given index path for columnar table with
 * relationId.
 */
static void
CostBioseqColumnIndexPath(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
					  IndexPath *indexPath)
{
	if (!enable_indexscan)
	{
		bioseq_log<DBG>("[bioseq] pg: CostBioseqColumnIndexPath() skipped (enable_indexscan=false)");
		/* costs are already set to disable_cost, don't adjust them */
		return;
	}

	bioseq_log_pg(DEBUG4, "columnar table index scan costs estimated by "
				   "indexAM: startup cost = {:.10f}, total cost = "
				   "{:.10f}", indexPath->path.startup_cost,
				   indexPath->path.total_cost);

	/*
	 * We estimate the cost for columnar table read during index scan. Also,
	 * instead of overwriting total cost, we "add" ours to the cost estimated
	 * by indexAM since we should consider index traversal related costs too.
	 */

	if (!bioseq_index_scan)
	{
		bioseq_log<DBG>("[bioseq] pg: CostBioseqColumnIndexPath() bioseq_index_scan disabled, adding columnar cost");
		Cost bioseqColumnIndexScanCost = BioseqColumnIndexScanAdditionalCost(root, rel, relationId,
																	indexPath);
		indexPath->path.total_cost += bioseqColumnIndexScanCost;
	}

	bioseq_log<DBG>("[bioseq] pg: CostBioseqColumnIndexPath() cost computation complete");

	bioseq_log_pg(DEBUG4, "columnar table index scan costs re-estimated "
				   "by columnarAM (including indexAM costs): "
				   "startup cost = {:.10f}, total cost = {:.10f}",
				   indexPath->path.startup_cost,
				   indexPath->path.total_cost);
}


/*
 * BioseqColumnIndexScanAdditionalCost returns additional cost estimated for
 * index scan described by IndexPath for columnar table with relationId.
 */
static Cost
BioseqColumnIndexScanAdditionalCost(PlannerInfo *root, RelOptInfo *rel,
								Oid relationId, IndexPath *indexPath)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnIndexScanAdditionalCost() entered");
	int numberOfColumnsRead = RelationIdGetNumberOfAttributes(relationId);
	Cost perStripeCost = BioseqColumnPerStripeScanCost(rel, relationId, numberOfColumnsRead);

	/*
	 * We don't need to pass correct loop count to amcostestimate since we
	 * will only use index correlation & index selectivity, and loop count
	 * doesn't have any effect on those two.
	 */
	double fakeLoopCount = 1;
	Cost fakeIndexStartupCost;
	Cost fakeIndexTotalCost;
	double fakeIndexPages;
	Selectivity indexSelectivity;
	double indexCorrelation;
	amcostestimate_function amcostestimate = indexPath->indexinfo->amcostestimate;
	amcostestimate(root, indexPath, fakeLoopCount, &fakeIndexStartupCost,
				   &fakeIndexTotalCost, &indexSelectivity,
				   &indexCorrelation, &fakeIndexPages);

	Relation relation = RelationIdGetRelation(relationId);
	uint64_t rowCount = BioseqColumnTableRowCount(relation);
	RelationClose(relation);
	double estimatedRows = rowCount * indexSelectivity;

	/*
	 * In the worst case (i.e no correlation between the column & the index),
	 * we need to read a different stripe for each row.
	 */
	double maxStripeReadCount = estimatedRows;

	/*
	 * In the best case (i.e the column is fully correlated with the index),
	 * we wouldn't read the same stripe again and again thanks
	 * to locality.
	 */
	double avgStripeRowCount =
		rowCount / static_cast<double>(BioseqColumnTableStripeCount(relationId));
	double minStripeReadCount = estimatedRows / avgStripeRowCount;

	/*
	 * While being close to 0 means low correlation, being close to -1 or +1
	 * means high correlation. For index scans on columnar tables, it doesn't
	 * matter if the column and the index are "correlated" (+1) or
	 * "anti-correlated" (-1) since both help us avoiding from reading the
	 * same stripe again and again.
	 */
	double absIndexCorrelation = std::abs(indexCorrelation);

	/*
	 * To estimate the number of stripes that we need to read, we do linear
	 * interpolation between minStripeReadCount & maxStripeReadCount. To do
	 * that, we use complement to 1 of absolute correlation, where being
	 * close to 0 means high correlation and being close to 1 means low
	 * correlation.
	 * In practice, we only want to do an index scan when absIndexCorrelation
	 * is 1 (or extremely close to it), or when the absolute number of tuples
	 * returned is very small. Other cases will have a prohibitive cost.
	 */
	double complementIndexCorrelation = 1 - absIndexCorrelation;
	double estimatedStripeReadCount =
		minStripeReadCount + complementIndexCorrelation * (maxStripeReadCount -
														   minStripeReadCount);

	/* even in the best case, we will read a single stripe */
	estimatedStripeReadCount = std::max(estimatedStripeReadCount, 1.0);

	Cost scanCost = perStripeCost * estimatedStripeReadCount;

	bioseq_log_pg(DEBUG4, "re-costing index scan for columnar table: "
				   "selectivity = {:.10f}, complement abs "
				   "correlation = {:.10f}, per stripe cost = {:.10f}, "
				   "estimated stripe read count = {:.10f}, "
				   "total additional cost = {:.10f}",
				   indexSelectivity, complementIndexCorrelation,
				   perStripeCost, estimatedStripeReadCount,
				   scanCost);

	return scanCost;
}


/*
 * CostBioseqColumnSeqPath sets costs given seq path for columnar table with
 * relationId.
 */
static void
CostBioseqColumnSeqPath(RelOptInfo *rel, Oid relationId, Path *path)
{
	if (!enable_seqscan)
	{
		/* costs are already set to disable_cost, don't adjust them */
		return;
	}

	/*
	 * Seq scan doesn't support projection or qual pushdown, so we will read
	 * all the stripes and all the columns.
	 */
	double stripesToRead = BioseqColumnTableStripeCount(relationId);
	int numberOfColumnsRead = RelationIdGetNumberOfAttributes(relationId);

	path->rows = rel->tuples;
	path->startup_cost = 0;
	path->total_cost = stripesToRead *
					   BioseqColumnPerStripeScanCost(rel, relationId, numberOfColumnsRead);
}


/*
 * RelationIdGetNumberOfAttributes returns number of attributes that relation
 * with relationId has.
 */
static int
RelationIdGetNumberOfAttributes(Oid relationId)
{
	Relation relation = RelationIdGetRelation(relationId);
	int nattrs = relation->rd_att->natts;
	RelationClose(relation);
	return nattrs;
}


/*
 * CheckVarStats() checks whether a qual involving this Var is likely to be
 * useful based on the correlation stats. If so, or if stats are unavailable,
 * return true; otherwise return false and sets absVarCorrelation in case
 * caller wants to use for logging purposes.
 */
static bool
CheckVarStats(PlannerInfo *root, Var *var, Oid sortop, float *absVarCorrelation)
{
	bioseq_log<DBG>("[bioseq] pg: CheckVarStats() entered");
	/*
	 * Collect isunique, ndistinct, and varCorrelation.
	 */
	VariableStatData varStatData;
	examine_variable(root, reinterpret_cast<Node *>(var), var->varno, &varStatData);
	if (varStatData.rel == NULL ||
		!HeapTupleIsValid(varStatData.statsTuple))
	{
		return true;
	}

	AttStatsSlot sslot;
	if (!get_attstatsslot(&sslot, varStatData.statsTuple,
						  STATISTIC_KIND_CORRELATION, sortop,
						  ATTSTATSSLOT_NUMBERS))
	{
		ReleaseVariableStats(varStatData);
		return true;
	}

	Assert(sslot.nnumbers == 1);

	float varCorrelation = sslot.numbers[0];

	ReleaseVariableStats(varStatData);

	/*
	 * If the Var is not highly correlated, then the chunk's min/max bounds
	 * will be nearly useless.
	 */
	if (std::abs(varCorrelation) < BioseqColumnQualPushdownCorrelationThreshold)
	{
		if (absVarCorrelation)
		{
			/*
			 * Report absVarCorrelation if caller wants to know why given
			 * var is rejected.
			 */
			*absVarCorrelation = std::abs(varCorrelation);
		}
		return false;
	}

	return true;
}


/*
 * ExprReferencesRelid returns true if any of the Expr's Vars refer to the
 * given relid; false otherwise.
 */
static bool
ExprReferencesRelid(Expr *expr, Index relid)
{
	List *exprVars = pull_var_clause(
		reinterpret_cast<Node *>(expr), PVC_RECURSE_AGGREGATES |
		PVC_RECURSE_WINDOWFUNCS | PVC_RECURSE_PLACEHOLDERS);
	for (auto var : PList<Var>(exprVars))
	{
		if (var->varno == relid)
		{
			return true;
		}
	}
	return false;
}

/*
 * DOC0322 §7.4 — BioseqColumnTableRowCount moved to storage_metadata:core (reached
 * here via `import storage_metadata;`, already imported).
 */


/*
 * ExtractPushdownClause extracts an Expr node from given clause for pushing down
 * into the given rel (including join clauses). This test may not be exact in
 * all cases; it's used to reduce the search space for parameterization.
 *
 * Note that we don't try to handle cases like "Var + ExtParam = 3". That
 * would require going through eval_const_expression after parameter binding,
 * and that doesn't seem worth the effort. Here we just look for "Var op Expr"
 * or "Expr op Var", where Var references rel and Expr references other rels
 * (or no rels at all).
 *
 * Moreover, this function also looks into BoolExpr's to recursively extract
 * pushdownable OpExpr's of them:
 * i)   AND_EXPR:
 *      Take pushdownable args of AND expressions by ignoring the other args.
 * ii)  OR_EXPR:
 *      Ignore the whole OR expression if we cannot extract a pushdownable Expr
 *      from one of its args.
 * iii) NOT_EXPR:
 *      Simply ignore NOT expressions since we don't expect to see them before
 *      an expression that we can pushdown, see the comment in function.
 *
 * The reasoning for those three rules could also be summarized as such;
 * for any expression that we cannot push-down, we must assume that it
 * evaluates to true.
 *
 * For example, given following WHERE clause:
 * (
 *     (a > random() OR a < 30)
 *     AND
 *     a < 200
 * ) OR
 * (
 *     a = 300
 *     OR
 *     a > 400
 * );
 * Even if we can pushdown (a < 30), we cannot pushdown (a > random() OR a < 30)
 * due to (a > random()). However, we can pushdown (a < 200), so we extract
 * (a < 200) from the lhs of the top level OR expression.
 *
 * For the rhs of the top level OR expression, since we can pushdown both (a = 300)
 * and (a > 400), we take this part as is.
 *
 * Finally, since both sides of the top level OR expression yielded pushdownable
 * expressions, we will pushdown the following:
 *  (a < 200) OR ((a = 300) OR (a > 400))
 */
/*
 * mmsearch() FuncExprs are recognised here as pushdownable scan
 * quals even though they have no btree opclass and no scan-key
 * shape: the actual filtering happens at execution time inside
 * BatchProcessor::process_sequences. The reason we still register
 * them as "pushdownable" is to make the planner consider
 * parameterised paths for cross-table mmsearches like
 *   WHERE mmsearch(t1.seq, t2.seq2)
 * Without this recognition `FilterPushdownClauses` drops the
 * FuncExpr from `joinClauses`, `FindCandidateRelids` returns no
 * outer rels for it, and `AddBioseqColumnScanPathsRec` never asks
 * `get_baserel_parampathinfo` to bring the qual into the inner
 * scan's `ppi_clauses`. The result: mmsearch ends up as a Nested
 * Loop join filter (the C stub), which always returns TRUE, and
 * the inner scan never runs the aligner — every cross-product row
 * comes back with NULL bioseq.* projections.
 *
 * Returning the unchanged FuncExpr from this helper tells
 * FilterPushdownClauses "this clause references this rel and is
 * safe to attach to its quals." Postgres (via
 * `replace_nestloop_params`) will rewrite the cross-rel Var (e.g.
 * t2.seq2) into a PARAM_EXEC Param at plan-finalization time, and
 * BeginCustomScan / ReScanCustomScan already handle Params for the
 * mmsearch second argument.
 */
[[nodiscard]] static bool
IsMmsearchPushdownable(RelOptInfo* rel, FuncExpr* fe,
					   const BioseqStubOids& stub_oids)
{
	if (!is_mmsearch_funcid(fe->funcid, stub_oids)) {
		return false;
	}
	pg_first_two_node_args two_args = pg_first_two_args_if_arity_between(*fe, 2, 3);
	if (!two_args.present()) {
		return false;
	}
	Node* arg1 = two_args.second;
	Var* col_var = pg_node_try_as<Var>(two_args.first);
	if (col_var == nullptr) {
		return false;
	}
	/*
	 * The first argument is the column to align — it MUST belong to
	 * `rel`. Cross-table mmsearch with the column on the OTHER side
	 * (e.g. mmsearch(t1.seq, t2.seq) when we are processing t2)
	 * doesn't push to this rel; the mirror call from t1's side
	 * (mmsearch(t1.seq, t2.seq) processing t1) does.
	 */
	if (col_var->varno != rel->relid) {
		return false;
	}
	if (col_var->varattno <= 0) {
		return false;
	}
	/*
	 * Constrain the second argument to shapes BioseqColumnScan_BeginCustomScan
	 * actually handles at execution time (Const literal, Param-driven NL
	 * inner-side rewrite, or another base-relation Var with a varno that
	 * Postgres can later turn into a PARAM_EXEC on the parameterised path).
	 * Without this filter the planner would generate a parameterised path
	 * carrying a complex-expression mmsearch which BeginCustomScan then
	 * silently rejects, leaving the buffer inactive and the bioseq.*
	 * projections back to NULL — the very bug §8 of
	 * REPORT_POSTGRES_QUERY_BUGFIXING_1.md is meant to fix.
	 */
	if (!pg_node_view{arg1}.is<Const>() && !pg_node_view{arg1}.is<Var>() && !pg_node_view{arg1}.is<Param>()) {
		return false;
	}
	/*
	 * Reject volatile second arguments. The OpExpr branch already
	 * does this (search for `contain_volatile_functions` below); the
	 * mmsearch branch needs the same protection so a query like
	 *   WHERE mmsearch(t.seq, gen_random_seq())
	 * does not get re-evaluated arbitrarily often on rescan.
	 */
	if (contain_volatile_functions(arg1)) {
		return false;
	}
	/*
	 * Typmod compatibility check. We accept any bioseq.seq arg1
	 * here and let BeginCustomScan decide whether (and how) to
	 * coalesce / cross-alphabet-recompute. The classification at
	 * runtime:
	 *   - same (alphabet, width)    → direct dispatch
	 *   - same alphabet, diff width → coalesce (re-encode arg1 at
	 *                                  column's width) + index OK
	 *   - diff alphabet, same width → coalesce (re-encode arg1 at
	 *                                  column's alphabet) + index
	 *                                  recompute (sidecar minimizers
	 *                                  no longer match)
	 *   - diff alphabet, diff width → coalesce both and recompute
	 *
	 * The previous version of this helper rejected any mismatch up
	 * here, which resulted in the parameterised path never being
	 * generated for typmod-divergent queries — and BeginCustomScan
	 * would silently leave bioseq.* projections NULL. Letting the
	 * planner generate the path and handling coalesce in
	 * BeginCustomScan is what gives the user useful results.
	 */
	if (col_var->vartype != stub_oids.bioseq_seq_type_oid
		|| col_var->vartypmod < 0) {
		return false;
	}
	int32 arg1_typmod = -1;
	Oid   arg1_typoid = InvalidOid;
	if (Const* c = pg_node_try_as<Const>(arg1)) {
		arg1_typmod = c->consttypmod;
		arg1_typoid = c->consttype;
	} else if (Var* v = pg_node_try_as<Var>(arg1)) {
		arg1_typmod = v->vartypmod;
		arg1_typoid = v->vartype;
	} else if (Param* p = pg_node_try_as<Param>(arg1)) {
		arg1_typmod = p->paramtypmod;
		arg1_typoid = p->paramtype;
	}
	if (arg1_typoid != stub_oids.bioseq_seq_type_oid
		|| arg1_typmod < 0) {
		return false;
	}
	/*
	 * Width must always be in the supported set (8 / 16 / 32 / 64).
	 * Alphabet must be one we know how to dispatch on (dna4 / dna5).
	 * The pack/unpack helper will raise on truly invalid typmods, so
	 * we keep this gate loose: anything BeginCustomScan can decode
	 * is allowed through. Cross-typmod is a runtime concern, not a
	 * planner-time rejection.
	 */
	return true;
}

static Expr *
ExtractPushdownClause(PlannerInfo *root, RelOptInfo *rel, Node *node)
{
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	if (node == NULL)
	{
		return NULL;
	}

	/*
	 * mmsearch FuncExpr accepted by IsMmsearchPushdownable above.
	 * When the qual is a parameter-driven cross-table join
	 * mmsearch(t1.seq, t2.seq2), letting it through here is what
	 * makes Postgres generate the parameterised inner-side path
	 * that drives MmsearchParamRebind.
	 */
	if (FuncExpr* fe = pg_node_try_as<FuncExpr>(node)) {
		const BioseqStubOids& stub_oids = get_cached_stub_oids();
		if (IsMmsearchPushdownable(rel, fe, stub_oids)) {
			bioseq_log<DBG>("[bioseq] pg: ExtractPushdownClause() accepting mmsearch funcexpr funcid={} for rel relid={}",
					fe->funcid, static_cast<int>(rel->relid));
			return reinterpret_cast<Expr*>(node);
		}
	}

	if (BoolExpr *boolExpr = pg_node_try_as<BoolExpr>(node))
	{
		if (boolExpr->boolop == NOT_EXPR)
		{
			/*
			 * Standard planner should have already applied de-morgan rule to
			 * simple NOT expressions. If we encounter with such an expression
			 * here, then it can't be a pushdownable one, such as:
			 *   WHERE id NOT IN (SELECT id FROM something).
			 */
			bioseq_log_pg(BioseqColumnPlannerDebugLevel,
				   "columnar planner: cannot push down clause: "
				   "must not contain a subplan");
			return NULL;
		}

		PList<Expr> pushdownableArgs;

		for(auto boolExprArg : PList<Node>(boolExpr->args))
		{
			Expr *pushdownableArg = ExtractPushdownClause(root, rel,
														  reinterpret_cast<Node *>(boolExprArg));
			if (pushdownableArg)
			{
				pushdownableArgs.append(pushdownableArg);
			}
			else if (boolExpr->boolop == OR_EXPR)
			{
				bioseq_log_pg(BioseqColumnPlannerDebugLevel,
					   "columnar planner: cannot push down clause: "
					   "all arguments of an OR expression must be "
					   "pushdownable but one of them was not, due "
					   "to the reason given above");
				return NULL;
			}

			/* simply skip AND args that we cannot pushdown */
		}

		int npushdownableArgs = pushdownableArgs.length();
		if (npushdownableArgs == 0)
		{
			bioseq_log_pg(BioseqColumnPlannerDebugLevel,
				   "columnar planner: cannot push down clause: "
				   "none of the arguments were pushdownable, "
				   "due to the reason(s) given above");
			return NULL;
		}
		else if (npushdownableArgs == 1)
		{
			return pushdownableArgs.first();
		}

		if (boolExpr->boolop == AND_EXPR)
		{
			return make_andclause(pushdownableArgs);
		}
		else if (boolExpr->boolop == OR_EXPR)
		{
			return make_orclause(pushdownableArgs);
		}
		else
		{
			/* already discarded NOT expr, so should not be reachable */
			return NULL;
		}
	}

	OpExpr *opExpr = pg_node_try_as<OpExpr>(node);
	pg_binary_node_args op_args = opExpr != nullptr ? pg_binary_args(*opExpr) : pg_binary_node_args{};
	if (!op_args.present())
	{
		bioseq_log_pg(BioseqColumnPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "must be binary operator expression");
		return NULL;
	}

	Expr *lhs = reinterpret_cast<Expr *>(op_args.first);
	Expr *rhs = reinterpret_cast<Expr *>(op_args.second);

	Var *varSide;
	Expr *exprSide;

	Var *lhs_var = pg_node_try_as<Var>(pg_as_node(lhs));
	Var *rhs_var = pg_node_try_as<Var>(pg_as_node(rhs));

	if (lhs_var != nullptr && lhs_var->varno == rel->relid &&
		!ExprReferencesRelid(rhs, rel->relid))
	{
		varSide = lhs_var;
		exprSide = rhs;
	}
	else if (rhs_var != nullptr && rhs_var->varno == rel->relid &&
			 !ExprReferencesRelid(lhs, rel->relid))
	{
		varSide = rhs_var;
		exprSide = lhs;
	}
	else
	{
		bioseq_report_pg(BioseqColumnPlannerDebugLevel)
				.msg("columnar planner: cannot push down clause: "
					 "must match 'Var <op> Expr' or 'Expr <op> Var'")
				.hint("Var must only reference this rel, "
					  "and Expr must not reference this rel");
		return NULL;
	}

	if (varSide->varattno <= 0)
	{
		bioseq_log_pg(BioseqColumnPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "var is whole-row reference or system column");
		return NULL;
	}

	if (contain_volatile_functions(reinterpret_cast<Node *>(exprSide)))
	{
		bioseq_log_pg(BioseqColumnPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "expr contains volatile functions");
		return NULL;
	}

	/* only the default opclass is used for qual pushdown. */
	Oid varOpClass = GetDefaultOpClass(varSide->vartype, BTREE_AM_OID);
	Oid varOpFamily;
	Oid varOpcInType;

	if (!OidIsValid(varOpClass) ||
		!get_opclass_opfamily_and_input_type(varOpClass, &varOpFamily,
											 &varOpcInType))
	{
		bioseq_log_pg(BioseqColumnPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "cannot find default btree opclass and opfamily for type: {}",
			   format_type_be(varSide->vartype));
		return NULL;
	}

	if (!op_in_opfamily(opExpr->opno, varOpFamily))
	{
		bioseq_log_pg(BioseqColumnPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "operator {} not a member of opfamily {}",
			   opExpr->opno, varOpFamily);
		return NULL;
	}

	Oid sortop = get_opfamily_member(varOpFamily, varOpcInType,
									 varOpcInType, BTLessStrategyNumber);
	Assert(OidIsValid(sortop));

	/*
	 * Check that statistics on the Var support the utility of this
	 * clause.
	 */
	float absVarCorrelation = 0;
	if (!CheckVarStats(root, varSide, sortop, &absVarCorrelation))
	{
		bioseq_log_pg(BioseqColumnPlannerDebugLevel,
			   "columnar planner: cannot push down clause: "
			   "absolute correlation ({:.3f}) of var attribute {} is "
			   "smaller than the value configured in "
			   "\"bioseq.qual_pushdown_correlation_threshold\" "
			   "({:.3f})", absVarCorrelation, varSide->varattno,
			   BioseqColumnQualPushdownCorrelationThreshold);
		return NULL;
	}

	return reinterpret_cast<Expr *>(node);
}


/*
 * FilterPushdownClauses filters for clauses that are candidates for pushing
 * down into rel.
 */
static List *
FilterPushdownClauses(PlannerInfo *root, RelOptInfo *rel, List *inputClauses)
{
	PList<RestrictInfo> filteredClauses;
	for (auto rinfo : PList<RestrictInfo>(inputClauses))
	{
		/*
		 * Ignore clauses that don't refer to this rel, and pseudoconstants.
		 *
		 * XXX: A pseudoconstant may be of use, but it doesn't make sense to
		 * push it down because it doesn't contain any Vars. Look into if
		 * there's something we should do with pseudoconstants here.
		 */
		if (rinfo->pseudoconstant ||
			!bms_is_member(rel->relid, rinfo->required_relids))
		{
			continue;
		}

		Expr *pushdownableExpr = ExtractPushdownClause(root, rel, reinterpret_cast<Node *>(rinfo->clause));
		if (!pushdownableExpr)
		{
			continue;
		}

		rinfo = copyObject(rinfo);
		rinfo->clause = pushdownableExpr;
		filteredClauses.append(rinfo);
	}

	return filteredClauses.release();
}


/*
 * PushdownJoinClauseMatches is a callback that returns true, indicating that
 * we want all of the clauses from generate_implied_equalities_for_column().
 */
static bool
PushdownJoinClauseMatches(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg)
{
	return true;
}


/*
 * FindPushdownJoinClauses finds join clauses, including those implied by ECs,
 * that may be pushed down.
 */
static List *
FindPushdownJoinClauses(PlannerInfo *root, RelOptInfo *rel)
{
	PList<RestrictInfo> joinClauses = copyObject(rel->joininfo);

	/*
	 * Here we are generating the clauses just so we can later extract the
	 * interesting relids. This is somewhat wasteful, but it allows us to
	 * filter out joinclauses, reducing the number of relids we need to
	 * consider.
	 *
	 * XXX: also find additional clauses for joininfo that are implied by ECs?
	 */
	List *ecClauses = generate_implied_equalities_for_column(
		root, rel, PushdownJoinClauseMatches, NULL,
		rel->lateral_referencers);
	joinClauses.concat(ecClauses);

	return FilterPushdownClauses(root, rel, joinClauses.release());
}


/*
 * FindCandidateRelids identifies candidate rels for parameterization from the
 * list of join clauses.
 *
 * Some rels cannot be considered for parameterization, such as a partitioned
 * parent of the given rel. Other rels are just not useful because they don't
 * appear in a join clause that could be pushed down.
 */
static Relids
FindCandidateRelids(PlannerInfo *root, RelOptInfo *rel, List *joinClauses)
{
	Relids candidateRelids = NULL;
	for (auto rinfo : PList<RestrictInfo>(joinClauses))
	{
		candidateRelids = bms_add_members(candidateRelids,
										  rinfo->required_relids);
	}

	candidateRelids = bms_del_members(candidateRelids, rel->relids);
	candidateRelids = bms_del_members(candidateRelids, rel->lateral_relids);
	candidateRelids = bms_del_members(candidateRelids, root->outer_join_rels);
	return candidateRelids;
}


/*
 * Combinations() calculates the number of combinations of n things taken k at
 * a time. When the correct result is large, the calculation may produce a
 * non-integer result, or overflow to inf, which caller should handle
 * appropriately.
 *
 * Use the following two formulae from Knuth TAoCP, 1.2.6:
 *    (2) Combinations(n, k) = (n*(n-1)..(n-k+1)) / (k*(k-1)..1)
 *    (5) Combinations(n, k) = Combinations(n, n-k)
 */
static double
Combinations(int n, int k)
{
	double v = 1;

	/*
	 * If k is close to n, then both the numerator and the denominator are
	 * close to n!, and we may overflow even if the input is reasonable
	 * (e.g. Combinations(500, 500)). Use formula (5) to choose the smaller,
	 * but equivalent, k.
	 */
	k = std::min(k, n - k);

	/* calculate numerator of formula (2) first */
	for (int i = n; i >= n - k + 1; i--)
	{
		v *= i;
	}

	/*
	 * Divide by each factor in the denominator of formula (2), skipping
	 * division by 1.
	 */
	for (int i = k; i >= 2; i--)
	{
		v /= i;
	}

	return v;
}


/*
 * ChooseDepthLimit() calculates the depth limit for the parameterization
 * search, given the number of candidate relations.
 *
 * The maximum number of paths generated for a given depthLimit is:
 *
 *    Combinations(nCandidates, 0) + Combinations(nCandidates, 1) + ... +
 *    Combinations(nCandidates, depthLimit)
 *
 * There's no closed formula for a partial sum of combinations, so just keep
 * increasing the depth until the number of combinations exceeds the limit.
 */
static int
ChooseDepthLimit(int nCandidates)
{
	if (!EnableBioseqColumnQualPushdown)
	{
		return 0;
	}

	int depth = 0;
	double numPaths = 1;

	while (depth < nCandidates)
	{
		numPaths += Combinations(nCandidates, depth + 1);

		if (numPaths > static_cast<double>(BioseqColumnMaxCustomScanPaths))
		{
			break;
		}

		depth++;
	}

	return depth;
}


/*
 * AddBioseqColumnScanPaths is the entry point for recursively generating
 * parameterized paths. See AddBioseqColumnScanPathsRec() for discussion.
 */
static void
AddBioseqColumnScanPaths(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte)
{
	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPaths() entered");
	List *joinClauses = FindPushdownJoinClauses(root, rel);
	Relids candidateRelids = FindCandidateRelids(root, rel, joinClauses);

	int depthLimit = ChooseDepthLimit(bms_num_members(candidateRelids));

	/* must always parameterize by lateral refs */
	Relids paramRelids = bms_copy(rel->lateral_relids);

	AddBioseqColumnScanPathsRec(root, rel, rte, paramRelids, candidateRelids,
							depthLimit);

	/* Once a CustomPath claims a restriction, remove only alternatives that
	 * would execute that same RestrictInfo through the SQL function.  This is
	 * structural selection; costs remain honest and nonnegative. */
	PList<RestrictInfo> owners;
	for (Path* candidate : PList<Path>(rel->pathlist)) {
		if (!IsA(candidate, CustomPath)) continue;
		CustomPath* custom = reinterpret_cast<CustomPath*>(candidate);
		bool is_mmsearch_owner = false;
		for (Node* private_node : PList<Node>(custom->custom_private)) {
			if (private_node == nullptr || !IsA(private_node, List)) continue;
			List* tag = reinterpret_cast<List*>(private_node);
			if (list_length(tag) == 3
				&& IsA(reinterpret_cast<Node*>(linitial(tag)), Integer)
				&& intVal(linitial(tag)) == kBioseqMmsearchOwnerMagic) {
				is_mmsearch_owner = true;
				break;
			}
		}
		if (!is_mmsearch_owner) continue;
		for (RestrictInfo* owner : PList<RestrictInfo>(custom->custom_restrictinfo)) {
			if (!list_member_ptr(owners.ptr(), owner)) owners.append(owner);
		}
	}
	auto prune_owned = [rel, &owners](List* paths) {
		PList<Path> survivors;
		for (Path* candidate : PList<Path>(paths)) {
			bool remove = false;
			for (RestrictInfo* owner : owners) {
				const bool executes_owner =
					list_member_ptr(rel->baserestrictinfo, owner)
					|| (candidate->param_info != nullptr
						&& list_member_ptr(candidate->param_info->ppi_clauses, owner));
				const bool owns_restriction = IsA(candidate, CustomPath)
					&& list_member_ptr(
						reinterpret_cast<CustomPath*>(candidate)->custom_restrictinfo,
						owner);
				if (executes_owner && !owns_restriction) {
					remove = true;
					break;
				}
			}
			if (!remove) survivors.append(candidate);
		}
		return survivors.release();
	};
	if (!owners.empty()) {
		rel->pathlist = prune_owned(rel->pathlist);
		rel->partial_pathlist = prune_owned(rel->partial_pathlist);
	}

	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPaths() returning");
}


/*
 * AddBioseqColumnScanPathsRec is a recursive function to search the
 * parameterization space and add CustomPaths for columnar scans.
 *
 * The set paramRelids is the parameterization at the current level, and
 * candidateRelids is the set from which we draw to generate paths with
 * greater parameterization.
 *
 * BioseqColumn tables resemble indexes because of the ability to push down
 * quals. Ordinary quals, such as x = 7, can be pushed down easily. But join
 * quals of the form "x = y" (where "y" comes from another rel) require the
 * proper parameterization.
 *
 * Paths that require more outer rels can push down more join clauses that
 * depend on those outer rels. But requiring more outer rels gives the planner
 * fewer options for the shape of the plan. That means there is a trade-off,
 * and we should generate plans of various parameterizations, then let the
 * planner choose. We always need to generate one minimally-parameterized path
 * (parameterized only by lateral refs, if present) to make sure that at least
 * one path can be chosen. Then, we generate as many parameterized paths as we
 * reasonably can.
 *
 * The set of all possible parameterizations is the power set of
 * candidateRelids. The power set has cardinality 2^N, where N is the
 * cardinality of candidateRelids. To avoid creating a huge number of paths,
 * limit the depth of the search; the depthLimit is equivalent to the maximum
 * number of required outer rels (beyond the minimal parameterization) for the
 * path. A depthLimit of zero means that only the minimally-parameterized path
 * will be generated.
 */
static void
AddBioseqColumnScanPathsRec(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
						Relids paramRelids, Relids candidateRelids,
						int depthLimit)
{
	CHECK_FOR_INTERRUPTS();
	check_stack_depth();

	Assert(!bms_overlap(paramRelids, candidateRelids));

	Path *bioseqColumnScanPath = AddBioseqColumnScanPath(root, rel, rte, paramRelids);
	CustomPath* owner_path = IsA(bioseqColumnScanPath, CustomPath)
		? reinterpret_cast<CustomPath*>(bioseqColumnScanPath) : nullptr;
	if (owner_path != nullptr
		&& list_length(owner_path->custom_restrictinfo) == 1) {
		RestrictInfo* owner = reinterpret_cast<RestrictInfo*>(
			linitial(owner_path->custom_restrictinfo));
		auto prune_competing_owner_execution = [rel, owner](List* paths) {
			PList<Path> survivors;
			for (Path* candidate : PList<Path>(paths)) {
				const bool executes_owner =
					list_member_ptr(rel->baserestrictinfo, owner)
					|| (candidate->param_info != nullptr
						&& list_member_ptr(candidate->param_info->ppi_clauses, owner));
				const bool owns_restriction = IsA(candidate, CustomPath)
					&& list_member_ptr(
						reinterpret_cast<CustomPath*>(candidate)->custom_restrictinfo,
						owner);
				if (!executes_owner || owns_restriction) survivors.append(candidate);
			}
			return survivors.release();
		};
		rel->pathlist = prune_competing_owner_execution(rel->pathlist);
		rel->partial_pathlist =
			prune_competing_owner_execution(rel->partial_pathlist);
	}
	if (bioseq_enable_parallel_execution)
		bioseqColumnScanPath->total_cost += bioseqColumnScanPath->rows * 0.1;
	add_path(rel, bioseqColumnScanPath);

	/* For columnar custom scan we should also do planning for parallel exeuction
	 * if it is enabled with GUC.
	 */
	if (bioseq_enable_parallel_execution)
	{
		int bioseq_min_parallel_process_running = bioseq_min_parallel_processes;

		if (bioseq_min_parallel_processes > max_parallel_workers)
		{
			bioseq_log_pg(DEBUG1, "bioseq.min_parallel_proceses is set higher than max_parallel_workers.");
			bioseq_log_pg(DEBUG1, "Using max_parallel_workers instead for parallel columnar scan.");
			bioseq_min_parallel_process_running = 
				std::min(max_parallel_workers, bioseq_min_parallel_processes);
		}

		if (parallel_leader_participation)
			bioseq_min_parallel_process_running--;

		/*
		 * We know how many workers are we going to run. Since workers are using single stripe
		 * probably it doesn't have much sense to spawn more workers than stripes in table.
		 */
		bioseq_min_parallel_process_running = std::min<int64_t>(bioseq_min_parallel_process_running,
													parallel_leader_participation ?
													BioseqColumnTableStripeCount(rte->relid) - 1 :
													BioseqColumnTableStripeCount(rte->relid));

		// FIXME: Fix this!!!
		const bool force_parallel = true;
		if (force_parallel) {
			bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPathsRec() force_parallel override active");
			bioseq_min_parallel_process_running = bioseq_min_parallel_processes;
		}

		if (force_parallel || (rel->consider_parallel && rel->lateral_relids == NULL && 
			bioseq_min_parallel_process_running > 0))
		{
			bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPathsRec() add_partial_path for parallel scan");
			/*
			 * Passing NULL for JOIN quals in parallel execution.
			 */
			Path *parallelBioseqColumnScanPath = 
				AddBioseqColumnScanPath(root, rel, rte, NULL);

			if (parallelBioseqColumnScanPath->parallel_safe) {
				parallelBioseqColumnScanPath->parallel_workers =
					bioseq_min_parallel_process_running;
				parallelBioseqColumnScanPath->parallel_aware = true;
				AdjustBioseqColumnParallelScanCost(
					parallelBioseqColumnScanPath);
				add_partial_path(rel, parallelBioseqColumnScanPath);
			} else {
				bioseq_log<DBG>("[bioseq] pg: suppressing parallel partial path for alignment-owning custom scan");
			}
		}
	}

	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPathsRec() recursing into candidate relids");

	/* recurse for all candidateRelids, unless we hit the depth limit */
	Assert(depthLimit >= 0);
	if (depthLimit-- == 0)
	{
		return;
	}

	/*
	 * Iterate through parameter combinations depth-first. Deeper levels
	 * generate paths of greater parameterization (and hopefully lower
	 * cost).
	 */
	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPathsRec() iterating parameter combinations depth-first");
	Relids tmpCandidateRelids = bms_copy(candidateRelids);
	int relid = -1;
	while ((relid = bms_next_member(candidateRelids, relid)) >= 0)
	{
		Relids tmpParamRelids = bms_add_member(
			bms_copy(paramRelids), relid);

		/*
		 * Because we are generating combinations (not permutations), remove
		 * the relid from the set of candidates at this level as we descend to
		 * the next.
		 */
		tmpCandidateRelids = bms_del_member(tmpCandidateRelids, relid);

		AddBioseqColumnScanPathsRec(root, rel, rte, tmpParamRelids,
								tmpCandidateRelids, depthLimit);
	}

	bms_free(tmpCandidateRelids);
}


/*
 * ParameterizationAsString returns the string representation of the set of
 * rels given in paramRelids.
 *
 * Takes a StringInfo so that it doesn't return palloc'd memory. This makes it
 * easy to call this function as an argument to ereport(), such that it won't
 * be evaluated unless the message is going to be output somewhere.
 */
static char *
ParameterizationAsString(PlannerInfo *root, Relids paramRelids, StringInfo buf)
{
	PBitmapset paramRelidsView{ paramRelids };

	if (paramRelidsView.empty())
	{
		return "unparameterized";
	}

	bool firstTime = true;
	appendStringInfoString(buf, "parameterized by rels {");
	for (int relid : paramRelidsView)
	{
		RangeTblEntry *rte = root->simple_rte_array[relid];
		const char *relname = quote_identifier(rte->eref->aliasname);

		appendStringInfo(buf, "%s%s", firstTime ? "" : ", ", relname);

		if (relname != rte->eref->aliasname)
		{
			pfree(const_cast<char *>(relname));
		}

		firstTime = false;
	}
	appendStringInfoString(buf, "}");
	return buf->data;
}


/*
 * ContainsExecParams tests whether the node contains any exec params. The
 * signature accepts an extra argument for use with expression_tree_walker.
 */
struct exec_params_probe_state {};

struct exec_params_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, exec_params_probe_state&) const {
		if (Param *param = node.try_as<Param>())
		{
			if (param->paramkind == PARAM_EXEC)
			{
				return pg_walk_step::stop;
			}
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static bool
ContainsExecParams(Node *node)
{
	exec_params_probe_state state;
	exec_params_visitor visitor;
	return pg_walk_expression_including_root(node, state, visitor);
}

/*
 * fixup_inherited_columns
 *
 * Exact function Copied from PG16 as it's static.
 *
 * When user is querying on a table with children, it implicitly accesses
 * child tables also. So, we also need to check security label of child
 * tables and columns, but there is no guarantee attribute numbers are
 * same between the parent and children.
 * It returns a bitmapset which contains attribute number of the child
 * table based on the given bitmapset of the parent.
 */
static Bitmapset *
fixup_inherited_columns(Oid parentId, Oid childId, Bitmapset *columns)
{
	PBitmapset result;

	/*
	 * obviously, no need to do anything here
	 */
	if (parentId == childId)
	{
		return columns;
	}

	for (int index : PBitmapset(columns))
	{
		/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
		AttrNumber attno = index + FirstLowInvalidHeapAttributeNumber;

		/*
		 * whole-row-reference shall be fixed-up later
		 */
		if (attno == InvalidAttrNumber)
		{
			result.add(index);
			continue;
		}

		char *attname = get_attname(parentId, attno, false);
		attno = get_attnum(childId, attname);
		if (attno == InvalidAttrNumber)
		{
			bioseq_log<ERR, PG>( "cache lookup failed for attribute {} of relation {}",
				   attname, childId);
		}

		result.add(attno - FirstLowInvalidHeapAttributeNumber);

		pfree(attname);
	}

	return result.release();
}

struct cost_test_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, Query& parse) const {
		bioseq_log<DBG>("[bioseq] pg: cost_test_visitor() entered");
		if (Var* var_node = node.try_as<Var>()) {
			const auto var_no = var_node->varno;
			List* range_table = parse.rtable;
			if (range_table == nullptr) {
				bioseq_log<DBG>("[bioseq] pg: cost_test_visitor() var rtable is null");
			} else if (var_no == 0 || var_no > list_length(range_table)) {
				bioseq_log<DBG>("[bioseq] pg: cost_test_visitor() var varno out of bounds varno={} rtable_len={}", var_no, list_length(range_table));
			} else {
				RangeTblEntry* rte = rt_fetch(var_no, range_table);
				bioseq_log<DBG>("[bioseq] pg: cost_test_visitor() rte_kind={} relid={}", static_cast<int>(rte->rtekind), static_cast<int>(rte->relid));
			}
		}

		return pg_walk_step::descend;
	}
};

/*
 * Create and add a path with the given parameterization paramRelids.
 *
 * XXX: Consider refactoring to be more like postgresGetForeignPaths(). The
 * only differences are param_info and custom_private.
 */
static Path *
AddBioseqColumnScanPath(PlannerInfo *root, RelOptInfo *rel, RangeTblEntry *rte,
					Relids paramRelids)
{
	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() entered");
	/*
	 * Must return a CustomPath, not a larger structure containing a
	 * CustomPath as the first field. Otherwise, nodeToString() will fail to
	 * output the additional fields.
	 */
	CustomPath *cpath = makeNode(CustomPath);

	cpath->methods = &BioseqColumnScanPathMethods;

	/*
	 * populate generic path information
	 */
	Path *path = &cpath->path;
	path->pathtype = T_CustomScan;
	path->parent = rel;
	path->pathtarget = copy_pathtarget(rel->reltarget);

	const BioseqStubOids& stub_oids = get_cached_stub_oids();

	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() mmsearch_oid={} score_oid={} cigar_oid={} query_start_oid={} symbol_count_oid={}",
		stub_oids.mmsearch_oid, stub_oids.score_oid, stub_oids.cigar_oid, stub_oids.query_start_oid,
		stub_oids.symbol_count_oid);

	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() walking pathtarget expressions");
	cost_test_visitor cost_visitor;
	for (auto expr : PList<Node>(path->pathtarget->exprs)) {
		bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() pathtarget expression");
		pg_walk_expression_including_root(expr, *root->parse, cost_visitor);
	}
	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() pathtarget walk complete");

	/* columnar scans are not parallel-aware, but they are parallel-safe */
	path->parallel_safe = rel->consider_parallel;

	path->param_info = get_baserel_parampathinfo(root, rel, paramRelids);

	/*
	 * Usable clauses for this parameterization exist in baserestrictinfo and
	 * ppi_clauses.
	 */
	PList<RestrictInfo> clauseReferences;
	for (RestrictInfo* rinfo : PList<RestrictInfo>(rel->baserestrictinfo)) {
		if (!list_member_ptr(clauseReferences.ptr(), rinfo)) {
			clauseReferences.append(rinfo);
		}
	}
	if (path->param_info != NULL) {
		for (RestrictInfo* rinfo : PList<RestrictInfo>(path->param_info->ppi_clauses)) {
			if (!list_member_ptr(clauseReferences.ptr(), rinfo)) {
				clauseReferences.append(rinfo);
			}
		}
	}
	const OwnedMmsearchRestriction owned_mmsearch =
		choose_owned_mmsearch_restriction(
			root, rel, rte, clauseReferences.ptr(), paramRelids, stub_oids);

	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() iterating allClauses (no-op pass for tracing)");
	for (auto restrict_info : clauseReferences)
	{
		(void) restrict_info;
	}

	PList<RestrictInfo> allClauses(FilterPushdownClauses(
		root, rel, list_copy(clauseReferences.ptr())));
	if (owned_mmsearch.restriction != nullptr) {
		allClauses = restrictions_without_owned_mmsearch(
			allClauses.release(), owned_mmsearch.restriction,
			owned_mmsearch.restriction->clause,
			/*restrictions_are_private_copies=*/true);
		/*
		 * DOC0371 \u00a75.2: the POC publishes backend-local proof and has no
		 * DSM aggregation.  A worker-local snapshot must never masquerade as
		 * leader proof, so every alignment-owning path is parallel unsafe.
		 */
		path->parallel_safe = false;
	}

	/*
	 * Plain clauses may contain extern params, but not exec params, and can
	 * be evaluated at init time or rescan time. Track them in another list
	 * that is a subset of allClauses.
	 *
	 * Note: although typically baserestrictinfo contains plain clauses,
	 * that's not always true. It can also contain a qual referencing a Var at
	 * a higher query level, which can be turned into an exec param, and
	 * therefore it won't be a plain clause.
	 */
	PList<RestrictInfo> plainClauses;
	for (auto rinfo : allClauses)
	{
		if (bms_is_subset(rinfo->required_relids, rel->relids) &&
			!ContainsExecParams(reinterpret_cast<Node *>(rinfo->clause)))
		{
			plainClauses.append(rinfo);
		}
	}

	/*
	 * We can't make our own CustomPath structure, so we need to put
	 * everything in the custom_private list. To keep the two lists separate,
	 * we make them sublists in a 2-element list.
	 */
	if (EnableBioseqColumnQualPushdown)
	{
		cpath->custom_private = list_make2(copyObject(plainClauses.ptr()),
										   copyObject(allClauses.ptr()));
	}
	else
	{
		cpath->custom_private = list_make2(NIL, NIL);
	}
	if (owned_mmsearch.restriction != nullptr) {
		cpath->custom_restrictinfo = list_make1(owned_mmsearch.restriction);
		cpath->custom_private = lappend(cpath->custom_private,
			list_make3(makeInteger(kBioseqMmsearchOwnerMagic),
				makeInteger(kBioseqMmsearchOwnerVersion),
				copyObject(owned_mmsearch.normalized_expr)));
	} else {
		cpath->custom_restrictinfo = NIL;
	}

	int numberOfColumnsRead = 0;
	if (rte->perminfoindex > 0)
	{
		/*
		 * If perminfoindex > 0, that means that this relation's permission info
		 * is directly found in the list of rteperminfos of the Query(root->parse)
		 * So, all we have to do here is retrieve that info.
		 */
		RTEPermissionInfo *perminfo = getRTEPermissionInfo(root->parse->rteperminfos,
														   rte);
		numberOfColumnsRead = bms_num_members(perminfo->selectedCols);
	}
	else
	{
		/*
		 * If perminfoindex = 0, that means we are skipping the check for permission info
		 * for this relation, which means that it's either a partition or an inheritance child.
		 * In these cases, we need to access the permission info of the top parent of this relation.
		 * After thorough checking, we found that the index of the top parent pointing to the correct
		 * range table entry in Query's range tables (root->parse->rtable) is found under
		 * RelOptInfo rel->top_parent->relid.
		 * For reference, check expand_partitioned_rtentry and expand_inherited_rtentry PG functions
		 */
		Assert(rel->top_parent);
		RangeTblEntry *parent_rte = rt_fetch(rel->top_parent->relid, root->parse->rtable);
		RTEPermissionInfo *perminfo = getRTEPermissionInfo(root->parse->rteperminfos,
														   parent_rte);
		numberOfColumnsRead = bms_num_members(fixup_inherited_columns(perminfo->relid,
																	  rte->relid,
																	  perminfo->
																	  selectedCols));
	}
	int numberOfClausesPushed = allClauses.length();

	/* Queries that contain only aggregate with STAR doesn't have any
	 * selectedCols so we should consider at least one column that needs
	 * read (for better cost calculation).
	 */
	if (numberOfColumnsRead == 0)
		numberOfColumnsRead = 1;

	CostBioseqColumnScan(root, rel, rte->relid, cpath, numberOfColumnsRead,
					 numberOfClausesPushed);

	if (owned_mmsearch.normalized_expr != nullptr) {
		QualCost operand_cost{};
		List* operand_expressions = mmsearch_operand_cost_exprs(
			owned_mmsearch.normalized_expr, stub_oids);
		cost_qual_eval(&operand_cost, operand_expressions, root);
		PList<Node> operands(owned_mmsearch.normalized_expr->args);
		const std::uint64_t target_symbols =
			estimate_mmsearch_operand_symbols(operands.first(), stub_oids);
		const std::uint64_t query_symbols =
			estimate_mmsearch_operand_symbols(operands.second(), stub_oids);
		const std::uint64_t admitted_bytes =
			bioseq::parallelism::saturating_add(
				target_symbols, query_symbols);
		bioseq::simd::active_target::Minimap2Params planned_params =
			build_params_from_gucs();
		bioseq::indexing::IndexReadRequirement planned_requirement =
			bioseq::indexing::IndexReadRequirement::allow;
		(void)apply_aligner_options_override(
			owned_mmsearch.normalized_expr, planned_params,
			planned_requirement, stub_oids);
		const owned_mmsearch_cost alignment = estimate_owned_mmsearch_cost(
			target_symbols, query_symbols, path->rows,
			planned_requirement
				!= bioseq::indexing::IndexReadRequirement::require_absent,
			static_cast<std::uint32_t>(std::min<std::size_t>(
				kBioseqRuntimeBufferReserve,
				(std::numeric_limits<std::uint32_t>::max)())),
			admitted_bytes);
		const Cost owner_startup = std::max<Cost>(0,
			operand_cost.startup + alignment.startup);
		const Cost owner_per_row = std::max<Cost>(0,
			operand_cost.per_tuple + alignment.per_input_tuple);
		path->startup_cost += owner_startup;
		path->total_cost += owner_startup
			+ owner_per_row * std::max(0.0, path->rows);
	}
	if (!std::isfinite(path->startup_cost) || path->startup_cost < 0) {
		path->startup_cost = 0;
	}
	if (!std::isfinite(path->total_cost)) {
		path->total_cost = std::numeric_limits<Cost>::max() / 4.0;
	}
	path->total_cost = std::max(path->startup_cost, path->total_cost);


	bioseq_report_pg_lazy(BioseqColumnPlannerDebugLevel, [&] {
		StringInfoData buf;
		initStringInfo(&buf);
		bioseq_report_pg(BioseqColumnPlannerDebugLevel)
				.msg("columnar planner: adding CustomScan path for {}",
					 rte->eref->aliasname)
				.detail("{}; {} clauses pushed down",
						ParameterizationAsString(root, paramRelids, &buf),
						numberOfClausesPushed);
	});

	return path;
}


/*
 * AdjustBioseqColumnParallelScanCost calculates path cost based on
 * number of parallel workers (based on postgres code).
 */
static void
AdjustBioseqColumnParallelScanCost(Path *path) 
{
	/* Adjust costing for parallelism, if used. */
	if (path->parallel_workers > 0)
	{
		double parallel_divisor = path->parallel_workers;

		if (parallel_leader_participation)
		{
			double leader_contribution;

			leader_contribution = 1.0 - (0.3 * path->parallel_workers);
			if (leader_contribution > 0)
				parallel_divisor += leader_contribution;
		}

		/* The CPU cost is divided among all the workers. */
		path->total_cost /= parallel_divisor;

		path->rows = clamp_row_est(path->rows / parallel_divisor);
	}
}


/*
 * CostBioseqColumnScan calculates the cost of scanning the columnar table. The
 * cost is estimated by using all stripe metadata to estimate based on the
 * columns to read how many pages need to be read.
 */
static void
CostBioseqColumnScan(PlannerInfo *root, RelOptInfo *rel, Oid relationId,
				 CustomPath *cpath, int numberOfColumnsRead, int nClauses)
{
	Path *path = &cpath->path;

	List *allClauses = PList<List>(cpath->custom_private).second();
	Selectivity clauseSel = clauselist_selectivity(
		root, allClauses, rel->relid, JOIN_INNER, NULL);

	/*
	 * We already filtered out clauses where the overall selectivity would be
	 * misleading, such as inequalities involving an uncorrelated column. So
	 * we can apply the selectivity directly to the number of stripes.
	 */
	double stripesToRead = clauseSel * BioseqColumnTableStripeCount(relationId);
	stripesToRead = std::max(stripesToRead, 1.0);

	path->rows = rel->tuples;
	path->startup_cost = 0;
	path->total_cost = stripesToRead *
					   BioseqColumnPerStripeScanCost(rel, relationId, numberOfColumnsRead);
}


/*
 * BioseqColumnPerStripeScanCost calculates the cost to scan a single stripe
 * of given columnar table based on number of columns that needs to be
 * read during scan operation.
 */
static Cost
BioseqColumnPerStripeScanCost(RelOptInfo *rel, Oid relationId, int numberOfColumnsRead)
{
	Relation relation = RelationIdGetRelation(relationId);
	List *stripeList = StripesForRelfilenode(RelationPhysicalIdentifier_compat(relation), ForwardScanDirection);
	RelationClose(relation);

	uint32_t maxColumnCount = 0;
	uint64_t totalStripeSize = 0;
	for(auto stripeMetadata : PList<StripeMetadata>(stripeList))
	{
		totalStripeSize += stripeMetadata->dataLength;
		maxColumnCount = std::max(maxColumnCount, stripeMetadata->columnCount);
	}

	/*
	 * When no stripes are in the table we don't have a count in maxColumnCount. To
	 * prevent a division by zero turning into a NaN we keep the ratio on zero.
	 * This will result in a cost of 0 for scanning the table which is a reasonable
	 * cost on an empty table.
	 */
	if (maxColumnCount == 0)
	{
		return 0;
	}

	double columnSelectionRatio = numberOfColumnsRead / static_cast<double>(maxColumnCount);
	Cost tableScanCost = static_cast<double>(totalStripeSize) / BLCKSZ * columnSelectionRatio;
	Cost perStripeScanCost = tableScanCost / list_length(stripeList);

	/*
	 * Finally, multiply the cost of reading a single stripe by seq page read
	 * cost to make our estimation scale compatible with postgres.
	 * Since we are calculating the cost for a single stripe here, we use seq
	 * page cost instead of random page cost. This is because, random page
	 * access only happens when switching between columns, which is pretty
	 * much neglactable.
	 */
	double relSpaceSeqPageCost;
	get_tablespace_page_costs(rel->reltablespace,
							  NULL, &relSpaceSeqPageCost);
	perStripeScanCost = perStripeScanCost * relSpaceSeqPageCost;

	return perStripeScanCost;
}


/*
 * BioseqColumnTableStripeCount returns the number of stripes that columnar
 * table with relationId has by using stripe metadata.
 */
static uint64_t
BioseqColumnTableStripeCount(Oid relationId)
{
	Relation relation = RelationIdGetRelation(relationId);
	List *stripeList = StripesForRelfilenode(RelationPhysicalIdentifier_compat(relation), ForwardScanDirection);
	int stripeCount = list_length(stripeList);
	RelationClose(relation);

	return stripeCount;
}


static Plan *
BioseqColumnScanPath_PlanCustomPath(PlannerInfo *root,
								RelOptInfo *rel,
								CustomPath *best_path,
								List *tlist,
								List *clauses,
								List *custom_plans)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() entered");
	// PAPIESZ: ADD HERE
	/*
	 * Must return a CustomScan, not a larger structure containing a
	 * CustomScan as the first field. Otherwise, copyObject() will fail to
	 * copy the additional fields.
	 */
	CustomScan *cscan = makeNode(CustomScan);

	cscan->methods = &BioseqColumnScanScanMethods;
	cscan->custom_private = NIL;
	bool locator_passthrough_requested = false;
	FuncExpr* owned_mmsearch_expr = nullptr;
	for (Node* possible_tag : PList<Node>(best_path->custom_private)) {
		if (possible_tag == nullptr || !IsA(possible_tag, List)) continue;
		List* tag = reinterpret_cast<List*>(possible_tag);
		if (tag == NIL
			|| !IsA(reinterpret_cast<Node*>(linitial(tag)), Integer)) continue;
		if (list_length(tag) == 4
			&& intVal(list_nth(tag, 0)) == kBioseqLocatorPassthroughMagic) {
			locator_passthrough_requested = true;
		}
		if (list_length(tag) == 3
			&& intVal(list_nth(tag, 0)) == kBioseqMmsearchOwnerMagic) {
			if (intVal(list_nth(tag, 1)) != kBioseqMmsearchOwnerVersion
				|| owned_mmsearch_expr != nullptr
				|| (owned_mmsearch_expr = pg_node_try_as<FuncExpr>(
					reinterpret_cast<Node*>(list_nth(tag, 2)))) == nullptr) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Bioseq mmsearch owner path tag is invalid")));
			}
		}
	}
	RestrictInfo* owned_mmsearch_restriction =
		list_length(best_path->custom_restrictinfo) == 1
			? reinterpret_cast<RestrictInfo*>(
				linitial(best_path->custom_restrictinfo))
			: nullptr;
	if ((owned_mmsearch_expr == nullptr)
		!= (owned_mmsearch_restriction == nullptr)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("Bioseq mmsearch ownership metadata is inconsistent")));
	}

	/* XXX: also need to store projected column list for EXPLAIN */

	if (EnableBioseqColumnQualPushdown)
	{
		/*
		 * Lists of pushed-down clauses. The Vars in custom_exprs referencing
		 * other relations will be changed into exec Params by
		 * create_customscan_plan().
		 *
		 * Like CustomPath->custom_private, keep a list of plain clauses
		 * separate from the list of all clauses by making them sublists of a
		 * 2-element list.
		 *
		 * XXX: custom_exprs are the quals that will be pushed into the
		 * columnar reader code; some of these may not be usable. We should
		 * fix this by processing the quals more completely and using
		 * ScanKeys.
		 */
		PList<List> private_lists(best_path->custom_private);
		List *plainClauses = extract_actual_clauses(
			private_lists.first(), false /* no pseudoconstants */);
		List *allClauses = extract_actual_clauses(
			private_lists.second(), false /* no pseudoconstants */);
		cscan->custom_exprs = copyObject(list_make2(plainClauses, allClauses));
	}
	else
	{
		cscan->custom_exprs = list_make2(NIL, NIL);
	}

	List* executable_clauses = restrictions_without_owned_mmsearch(
		clauses, owned_mmsearch_restriction,
		owned_mmsearch_restriction != nullptr
			? owned_mmsearch_restriction->clause : nullptr,
		/*restrictions_are_private_copies=*/false);
	cscan->scan.plan.qual = extract_actual_clauses(
		executable_clauses, false /* no pseudoconstants */);
	if (owned_mmsearch_expr != nullptr) {
		for (Node* qual : PList<Node>(cscan->scan.plan.qual)) {
			if (contains_mmsearch_qualifier(qual, get_cached_stub_oids())) {
				ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("owned mmsearch remained in executable scan quals")));
			}
		}
	}

	/* Split the exact edit predicate out before ExecInitQual sees it.  Its
	 * placeholder has no value until MatchResults are drained, whereas every
	 * remaining scan qual is valid in the ordinary pre-alignment phase. */
	std::optional<StrictEditPredicate> strict_edit_predicate;
	{
		const BioseqStubOids& edit_oids = get_cached_stub_oids();
		PList<Node> pre_alignment_quals;
		for (Node* qual : PList<Node>(cscan->scan.plan.qual)) {
			if (!contains_bioseq_edit(qual, edit_oids)) {
				pre_alignment_quals.append(qual);
				continue;
			}
			const auto decoded = decode_strict_edit_upper_bound(
				qual, best_path->path.parent->relid, edit_oids);
			if (!decoded.has_value() || strict_edit_predicate.has_value()) {
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported bioseq.edit predicate"),
					 errdetail("Use exactly one top-level bioseq.edit(sequence_column) < integer constant predicate.")));
			}
			strict_edit_predicate = *decoded;
		}
		cscan->scan.plan.qual = pre_alignment_quals.release();

		if (strict_edit_predicate.has_value()) {
			PList<List> edit_expr_lists(cscan->custom_exprs);
			cscan->custom_exprs = list_make2(
				quals_without_bioseq_edit(edit_expr_lists.first(), edit_oids),
				quals_without_bioseq_edit(edit_expr_lists.second(), edit_oids));
			Const* threshold = makeConst(
				INT8OID, -1, InvalidOid, 8,
				Int64GetDatum(strict_edit_predicate->exclusive_upper_bound),
				false, true);
			List* tag = list_make4(
				makeInteger(kBioseqEditPredicateMagic),
				makeInteger(kBioseqEditPredicateVersion),
				threshold,
				makeInteger(strict_edit_predicate->sequence_attno));
			cscan->custom_private = lappend(cscan->custom_private, tag);
		}
	}

			const BioseqStubOids& plan_stub_oids = get_cached_stub_oids();
			if (!locator_passthrough_requested) for(auto n : PList<TargetEntry>(root->processed_tlist)) {
			if (FuncExpr* fe = pg_node_try_as<FuncExpr>(pg_as_node(n->expr))) {
				if (is_bioseq_projection_funcid(fe->funcid, plan_stub_oids)
					|| fe->funcid == plan_stub_oids.symbol_count_oid) {
					TargetEntry* projected = copyObject(n);
					projected->resno = static_cast<AttrNumber>(list_length(tlist) + 1);
					tlist = lappend(tlist, projected);
				}
			}
		}
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() tlist_is_null={}", tlist == nullptr);

	cscan->scan.plan.targetlist = list_copy(tlist);
	cscan->scan.scanrelid = best_path->path.parent->relid;
	for (Node* tag_node : PList<Node>(best_path->custom_private)) {
		if (tag_node != nullptr && IsA(tag_node, List)) {
			List* source_tag = reinterpret_cast<List*>(tag_node);
			if (source_tag == NIL
				|| !IsA(reinterpret_cast<Node*>(linitial(source_tag)), Integer)) {
				continue;
			}
			List* tag = reinterpret_cast<List*>(copyObject(source_tag));
			if (list_length(tag) == 4
				&& intVal(list_nth(tag, 0)) == kBioseqLocatorPassthroughMagic
				&& intVal(list_nth(tag, 1)) == kBioseqBatchJoinPlanVersion) {
				TargetEntry* rowno_entry = reinterpret_cast<TargetEntry*>(
					llast(cscan->scan.plan.targetlist));
				if (rowno_entry == nullptr
					|| exprType(reinterpret_cast<Node*>(rowno_entry->expr))
						!= INT8OID) {
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("Bioseq locator passthrough row-number column is missing")));
				}
				rowno_entry->resjunk = true;
				lfirst(list_nth_cell(tag, 3)) =
					makeInteger(rowno_entry->resno);
				cscan->custom_private = lappend(cscan->custom_private, tag);
				} else if (list_length(tag) == 3
					&& IsA(reinterpret_cast<Node*>(list_nth(tag, 1)), Integer)
					&& intVal(list_nth(tag, 0)) == kBioseqSliceSourceMagic
					&& intVal(list_nth(tag, 1)) == kBioseqSliceSourceVersion
				&& (list_nth(tag, 2) == nullptr
					|| IsA(reinterpret_cast<Node*>(list_nth(tag, 2)), List))) {
				cscan->custom_private = lappend(cscan->custom_private, tag);
			}
		}
	}

	/*
	 * Vectorized QUAL execution. Split QUAL list into vectorized list 
	 * stored in third position of custom_exprs list and rest of
	 * qual list (which can't be vectorized) in scan.plan.qual.
	 */
	if (bioseq_enable_vectorization)
	{
		List *candidateQualList = CreateVectorizedExprList(cscan->scan.plan.qual);
		List *listDifference = list_difference(candidateQualList, cscan->scan.plan.qual);

		bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() splitting qual list for vectorization");

		cscan->custom_exprs = lappend(cscan->custom_exprs, listDifference);

		if (listDifference != NULL)
			cscan->scan.plan.qual = list_intersection(cscan->scan.plan.qual,
													  candidateQualList);
	}
	else
	{
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() vectorization disabled, qual not split");
		/* We still need to create third entry in custom_exprs */
		cscan->custom_exprs = lappend(cscan->custom_exprs, NIL);
	}
	/* The fourth expression list is executor-only ownership state.  Keeping it
	 * in custom_exprs lets PostgreSQL replace outer Vars with PARAM_EXEC while
	 * ensuring ExecInitQual never sees the rendezvous function. */
	cscan->custom_exprs = lappend(cscan->custom_exprs,
		owned_mmsearch_expr != nullptr
			? list_make1(copyObject(owned_mmsearch_expr)) : NIL);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() returning");
	return reinterpret_cast<Plan *>(cscan);
}

struct BatchJoinPlaceholderRegistration {
	const BioseqStubOids* oids = nullptr;
	Index target_relid = 0;
	AttrNumber target_sequence_attno = InvalidAttrNumber;
	List** combined_tlist = nullptr;
	List** encoded_sites = nullptr;
	bool invalid = false;
	bool edit_sequence_mismatch = false;
};

[[nodiscard]] static bool batch_join_expression_has_match_helper(
		Node* expression,
		const BioseqStubOids& oids) {
	if (expression == nullptr) {
		return false;
	}
	BioseqFeatureDetector detector{&oids, BioseqScanFeatureFlags{}};
	feature_flag_visitor visitor;
	pg_walk_expression_including_root(expression, detector, visitor);
	return detector.flags.any_match_projection;
}

static void batch_join_register_placeholder_site(
		Node* identity,
		FuncExpr* function,
		BatchJoinPlaceholderRegistration& registration) {
	if (identity == nullptr || function == nullptr
		|| registration.combined_tlist == nullptr
		|| registration.encoded_sites == nullptr) {
		registration.invalid = true;
		return;
	}
	const Oid type_oid = exprType(identity);
	const int32 typmod = exprTypmod(identity);
	const Oid collation = exprCollation(identity);
	if (!OidIsValid(type_oid)
		|| type_oid != function->funcresulttype
		|| type_oid != get_func_rettype(function->funcid)) {
		registration.invalid = true;
		return;
	}
	const AttrNumber resno = static_cast<AttrNumber>(
		list_length(*registration.combined_tlist) + 1);
	/* PostgreSQL fixes a parent Result/Projection before recursing into this
	 * child.  Therefore plan.targetlist must retain the original helper/PHV
	 * tree long enough for the parent to match the child's output identity.
	 * The same exact helper owner is a descriptor-only custom_scan_tlist
	 * column: this CustomScan's later setrefs pass maps it to INDEX_VAR, and
	 * BioseqBatchJoin fills the slot from MatchResult.  No executor evaluates
	 * the function stub.  Distinct PHV ids or FuncExpr identities receive
	 * distinct resnos even when their SQL result types are the same. */
	TargetEntry* scan_entry = makeTargetEntry(
		reinterpret_cast<Expr*>(copyObject(identity)),
		resno,
		nullptr,
		false);
	*registration.combined_tlist = lappend(
		*registration.combined_tlist, scan_entry);
	*registration.encoded_sites = lappend(
		*registration.encoded_sites,
		list_make4(
			makeInteger(resno),
			makeInteger(static_cast<int>(function->funcid)),
			makeInteger(typmod),
			makeInteger(static_cast<int>(collation))));
}

struct batch_join_placeholder_registration_visitor {
	[[nodiscard]] pg_walk_step operator()(
		pg_expression_walk_cursor,
		pg_node_view node,
		BatchJoinPlaceholderRegistration& registration) const {
		if (PlaceHolderVar* placeholder = node.try_as<PlaceHolderVar>()) {
			FuncExpr* helper = batch_join_exact_helper_phv_function(
				placeholder, registration.target_relid);
			if (helper != nullptr) {
				if (!edit_sequence_argument_matches_target(
						helper,
						registration.target_relid,
						registration.target_sequence_attno,
						*registration.oids)) {
					registration.invalid = true;
					registration.edit_sequence_mismatch = true;
					return pg_walk_step::stop;
				}
				batch_join_register_placeholder_site(
					reinterpret_cast<Node*>(placeholder), helper, registration);
				return registration.invalid
					? pg_walk_step::stop : pg_walk_step::skip_subtree;
			}
			FuncExpr* wrapped_helper = pg_node_try_as<FuncExpr>(
				reinterpret_cast<Node*>(placeholder->phexpr));
			if (registration.oids != nullptr
				&& wrapped_helper != nullptr
				&& !edit_sequence_argument_matches_target(
					wrapped_helper,
					registration.target_relid,
					registration.target_sequence_attno,
					*registration.oids)) {
				registration.invalid = true;
				registration.edit_sequence_mismatch = true;
				return pg_walk_step::stop;
			}
			/* A generic PHV containing a helper has evaluation/nulling semantics
			 * that this join cannot reproduce.  Do not descend and accidentally
			 * register only the inner FuncExpr. */
			if (registration.oids != nullptr
				&& batch_join_expression_has_match_helper(
					reinterpret_cast<Node*>(placeholder->phexpr),
					*registration.oids)) {
				registration.invalid = true;
				return pg_walk_step::stop;
			}
			return pg_walk_step::descend;
		}
		if (FuncExpr* function = node.try_as<FuncExpr>()) {
			if (registration.oids != nullptr
				&& is_bioseq_projection_funcid(
					function->funcid, *registration.oids)) {
				if (!edit_sequence_argument_matches_target(
						function,
						registration.target_relid,
						registration.target_sequence_attno,
						*registration.oids)) {
					registration.invalid = true;
					registration.edit_sequence_mismatch = true;
					return pg_walk_step::stop;
				}
				FuncExpr* exact = batch_join_exact_target_helper(
					reinterpret_cast<Node*>(function), registration.target_relid);
				if (exact == nullptr) {
					registration.invalid = true;
					return pg_walk_step::stop;
				}
				batch_join_register_placeholder_site(
					reinterpret_cast<Node*>(function), exact, registration);
				return registration.invalid
					? pg_walk_step::stop : pg_walk_step::skip_subtree;
			}
		}
		return pg_walk_step::descend;
	}
};

[[nodiscard]] static AttrNumber find_plan_tlist_expression_resno(
		List* targetlist, const void* expression) noexcept {
	for (TargetEntry* entry : PList<TargetEntry>(targetlist)) {
		if (entry != nullptr && equal(entry->expr, expression)) {
			return entry->resno;
		}
	}
	return InvalidAttrNumber;
}

struct BatchJoinSetrefsCoverage {
	List* scan_tlist = NIL;
	const BioseqStubOids* oids = nullptr;
	Var* missing_var = nullptr;
	Node* missing_helper_identity = nullptr;
};

/* Mirror the relevant fix_upper_expr() lookup order before PostgreSQL's final
 * setrefs pass.  A CustomScan with scanrelid == 0 must make every output Var
 * discoverable in custom_scan_tlist; diagnosing that invariant here preserves
 * the exact offending varno/attno instead of reducing it to PostgreSQL's
 * generic "variable not found in subplan target list" error. */
struct batch_join_setrefs_coverage_visitor {
	[[nodiscard]] pg_walk_step operator()(
			pg_expression_walk_cursor,
			pg_node_view node,
			BatchJoinSetrefsCoverage& coverage) const {
		if (Var* var = node.try_as<Var>()) {
			for (TargetEntry* entry : PList<TargetEntry>(coverage.scan_tlist)) {
				Var* supplied = entry == nullptr
					? nullptr
					: pg_node_try_as<Var>(
						reinterpret_cast<Node*>(entry->expr));
				if (supplied != nullptr
					&& supplied->varno == var->varno
					&& supplied->varattno == var->varattno
					&& bms_equal(
						supplied->varnullingrels, var->varnullingrels)) {
					return pg_walk_step::skip_subtree;
				}
			}
			coverage.missing_var = var;
			return pg_walk_step::stop;
		}
		if (PlaceHolderVar* placeholder = node.try_as<PlaceHolderVar>()) {
			for (TargetEntry* entry : PList<TargetEntry>(coverage.scan_tlist)) {
				if (entry != nullptr && IsA(entry->expr, PlaceHolderVar)
					&& equal(entry->expr, placeholder)) {
					return pg_walk_step::skip_subtree;
				}
			}
			if (coverage.oids != nullptr
				&& batch_join_expression_has_match_helper(
					reinterpret_cast<Node*>(placeholder->phexpr),
					*coverage.oids)) {
				coverage.missing_helper_identity =
					reinterpret_cast<Node*>(placeholder);
				return pg_walk_step::stop;
			}
			return pg_walk_step::descend;
		}
		for (TargetEntry* entry : PList<TargetEntry>(coverage.scan_tlist)) {
			if (entry != nullptr && entry->expr != nullptr
				&& !IsA(entry->expr, Var)
				&& !IsA(entry->expr, PlaceHolderVar)
				&& equal(entry->expr, node.raw())) {
				return pg_walk_step::skip_subtree;
			}
		}
		if (FuncExpr* function = node.try_as<FuncExpr>();
			coverage.oids != nullptr && function != nullptr
			&& is_bioseq_projection_funcid(
				function->funcid, *coverage.oids)) {
			coverage.missing_helper_identity =
				reinterpret_cast<Node*>(function);
			return pg_walk_step::stop;
		}
		return pg_walk_step::descend;
	}
};

static void validate_batch_join_setrefs_coverage(
		List* output_tlist, List* scan_tlist) {
	const BioseqStubOids& oids = get_cached_stub_oids();
	BatchJoinSetrefsCoverage coverage{
		scan_tlist, &oids, nullptr, nullptr};
	batch_join_setrefs_coverage_visitor visitor;
	for (TargetEntry* output : PList<TargetEntry>(output_tlist)) {
		if (output == nullptr || output->expr == nullptr) {
			continue;
		}
		pg_walk_expression_including_root(
			reinterpret_cast<Node*>(output->expr), coverage, visitor);
		if (coverage.missing_helper_identity != nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin helper identity is absent from scan tuple"),
				 errdetail(
					"node_type=%d scan_columns=%d; executing the SQL helper stub is forbidden",
					static_cast<int>(nodeTag(
						coverage.missing_helper_identity)),
					list_length(scan_tlist))));
		}
		if (coverage.missing_var != nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin output variable is absent from scan tuple"),
				 errdetail(
					"varno=%u attno=%d type=%u typmod=%d scan_columns=%d",
					coverage.missing_var->varno,
					coverage.missing_var->varattno,
					coverage.missing_var->vartype,
					coverage.missing_var->vartypmod,
					list_length(scan_tlist))));
		}
	}
}

static Plan*
BioseqBatchJoin_PlanCustomPath(
		PlannerInfo* root,
		RelOptInfo* rel,
		CustomPath* best_path,
		List* tlist,
		List* clauses,
		List* custom_plans) {
	if (root == nullptr || rel == nullptr || best_path == nullptr
		|| list_length(custom_plans) != 2
		|| list_length(best_path->custom_private)
			!= kBioseqBatchJoinPathPrivateFields) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin planner state is incomplete")));
	}
	const int version = intVal(list_nth(best_path->custom_private, 0));
	const int token = intVal(list_nth(best_path->custom_private, 1));
	const Oid target_relation_oid = static_cast<Oid>(
		intVal(list_nth(best_path->custom_private, 2)));
	const AttrNumber target_storage_seq_attno = static_cast<AttrNumber>(
		intVal(list_nth(best_path->custom_private, 3)));
	Var* target_var = pg_node_try_as<Var>(reinterpret_cast<Node*>(
		list_nth(best_path->custom_private, 4)));
	Var* query_var = pg_node_try_as<Var>(reinterpret_cast<Node*>(
		list_nth(best_path->custom_private, 5)));
	const int alphabet_code = intVal(list_nth(best_path->custom_private, 6));
	const int storage_unit_byte_width =
		intVal(list_nth(best_path->custom_private, 7));
	Node* mmsearch_node = reinterpret_cast<Node*>(
		list_nth(best_path->custom_private, 8));
	FuncExpr* mmsearch = pg_node_try_as<FuncExpr>(mmsearch_node);
	const int index_read_requirement = intVal(
		list_nth(best_path->custom_private, 9));
	const Cost honest_startup_cost = batch_join_cost_from_node(
		reinterpret_cast<Node*>(
			list_nth(best_path->custom_private, 10)),
		"honest_startup_cost");
	const Cost honest_total_cost = batch_join_cost_from_node(
		reinterpret_cast<Node*>(
			list_nth(best_path->custom_private, 11)),
		"honest_total_cost");
	const Cost incumbent_cost_floor = batch_join_cost_from_node(
		reinterpret_cast<Node*>(
			list_nth(best_path->custom_private, 12)),
		"incumbent_cost_floor");
	if (version != kBioseqBatchJoinPlanVersion || token <= 0
		|| target_var == nullptr || query_var == nullptr || mmsearch == nullptr
		|| !bioseq::indexing::index_read_requirement_ordinal_valid(
			index_read_requirement)
		|| !batch_join_typmod_is_supported(target_var->vartypmod)
		|| target_var->vartypmod != query_var->vartypmod
		|| target_var->varcollid != query_var->varcollid) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin private plan ABI is invalid")));
	}
	Plan* target_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
	Plan* query_plan = reinterpret_cast<Plan*>(lsecond(custom_plans));
	if (target_plan == nullptr || query_plan == nullptr
		|| !IsA(target_plan, CustomScan)) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin target child is not a custom scan")));
	}
	auto* target_custom = reinterpret_cast<CustomScan*>(target_plan);
	if (target_custom->methods != &BioseqColumnScanScanMethods
		|| target_custom->scan.scanrelid != target_var->varno) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin target child lacks locator ownership")));
	}
	List* locator_tag = nullptr;
	for (Node* raw_tag : PList<Node>(target_custom->custom_private)) {
		if (raw_tag == nullptr || !IsA(raw_tag, List)) continue;
		List* candidate = reinterpret_cast<List*>(raw_tag);
		if (list_length(candidate) != 4
			|| !IsA(reinterpret_cast<Node*>(linitial(candidate)), Integer)
			|| intVal(linitial(candidate))
				!= kBioseqLocatorPassthroughMagic) continue;
		if (locator_tag != nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin target has duplicate locator ownership")));
		}
		locator_tag = candidate;
	}
	if (locator_tag == nullptr || !IsA(locator_tag, List)
		|| list_length(locator_tag) != 4
		|| intVal(list_nth(locator_tag, 0))
			!= kBioseqLocatorPassthroughMagic
		|| intVal(list_nth(locator_tag, 1))
			!= kBioseqBatchJoinPlanVersion
		|| intVal(list_nth(locator_tag, 2)) != token) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin locator tag/token validation failed")));
	}

	const AttrNumber target_slot_seq_resno =
		find_plan_tlist_expression_resno(target_plan->targetlist, target_var);
	const AttrNumber query_slot_seq_resno =
		find_plan_tlist_expression_resno(query_plan->targetlist, query_var);
	const AttrNumber target_slot_rowno_resno = static_cast<AttrNumber>(
		intVal(list_nth(locator_tag, 3)));
	if (target_slot_seq_resno <= 0 || query_slot_seq_resno <= 0
		|| target_slot_rowno_resno <= 0
		|| target_slot_rowno_resno > list_length(target_plan->targetlist)) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin child projection resnos are invalid")));
	}
	TargetEntry* target_seq_entry = reinterpret_cast<TargetEntry*>(
		list_nth(target_plan->targetlist, target_slot_seq_resno - 1));
	TargetEntry* query_seq_entry = reinterpret_cast<TargetEntry*>(
		list_nth(query_plan->targetlist, query_slot_seq_resno - 1));
	TargetEntry* rowno_entry = reinterpret_cast<TargetEntry*>(
		list_nth(target_plan->targetlist, target_slot_rowno_resno - 1));
	if (target_seq_entry == nullptr || query_seq_entry == nullptr
		|| rowno_entry == nullptr || !rowno_entry->resjunk
		|| target_seq_entry->resno != target_slot_seq_resno
		|| query_seq_entry->resno != query_slot_seq_resno
		|| rowno_entry->resno != target_slot_rowno_resno
		|| exprType(reinterpret_cast<Node*>(rowno_entry->expr)) != INT8OID
		|| exprTypmod(reinterpret_cast<Node*>(rowno_entry->expr)) != -1
		|| OidIsValid(exprCollation(reinterpret_cast<Node*>(rowno_entry->expr)))
		|| exprType(reinterpret_cast<Node*>(target_seq_entry->expr))
			!= get_cached_stub_oids().bioseq_seq_type_oid
		|| exprType(reinterpret_cast<Node*>(query_seq_entry->expr))
			!= get_cached_stub_oids().bioseq_seq_type_oid
		|| exprTypmod(reinterpret_cast<Node*>(target_seq_entry->expr))
			!= target_var->vartypmod
		|| exprTypmod(reinterpret_cast<Node*>(query_seq_entry->expr))
			!= query_var->vartypmod
		|| exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr))
			!= target_var->varcollid
		|| exprCollation(reinterpret_cast<Node*>(query_seq_entry->expr))
			!= query_var->varcollid) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqBatchJoin child sequence/row-number ABI is invalid")));
	}

	List* combined_tlist = NIL;
	for (TargetEntry* source : PList<TargetEntry>(target_plan->targetlist)) {
		TargetEntry* copied = static_cast<TargetEntry*>(copyObject(source));
		copied->resno = static_cast<AttrNumber>(list_length(combined_tlist) + 1);
		combined_tlist = lappend(combined_tlist, copied);
	}
	const int target_column_count = list_length(combined_tlist);
	for (TargetEntry* source : PList<TargetEntry>(query_plan->targetlist)) {
		TargetEntry* copied = static_cast<TargetEntry*>(copyObject(source));
		copied->resno = static_cast<AttrNumber>(list_length(combined_tlist) + 1);
		combined_tlist = lappend(combined_tlist, copied);
	}
	const int query_column_count =
		list_length(combined_tlist) - target_column_count;

	List* placeholder_sites = NIL;
	const BioseqStubOids& oids = get_cached_stub_oids();
	List* output_tlist = NIL;
	for (TargetEntry* output : PList<TargetEntry>(tlist)) {
		if (output == nullptr) {
			continue;
		}
		TargetEntry* copied = static_cast<TargetEntry*>(copyObject(output));
		BatchJoinPlaceholderRegistration registration{
			&oids,
			target_var->varno,
			target_storage_seq_attno,
			&combined_tlist,
			&placeholder_sites,
			false,
			false};
		batch_join_placeholder_registration_visitor visitor;
		pg_walk_expression_including_root(
			reinterpret_cast<Node*>(copied->expr), registration, visitor);
		if (registration.edit_sequence_mismatch) {
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq.edit projection must reference the mmsearch target column")));
		}
		if (registration.invalid) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin match helper identity is invalid"),
				 errdetail(
					"only exact target-owned FuncExpr or PlaceHolderVar helper sites are supported")));
		}
		output_tlist = lappend(output_tlist, copied);
	}
	validate_batch_join_setrefs_coverage(
		output_tlist, combined_tlist);

	auto* cscan = makeNode(CustomScan);
	cscan->methods = &BioseqBatchJoinScanMethods;
	cscan->flags = best_path->flags;
	cscan->scan.scanrelid = 0;
	/* Keep the pre-setrefs output expression identities intact.  PostgreSQL
	 * fixes any upper Result/Projection against this child targetlist before
	 * it recurses and maps this CustomScan against custom_scan_tlist. */
	cscan->scan.plan.targetlist = output_tlist;
	PList<RestrictInfo> executable_join_restrictions;
	std::size_t removed_mmsearch_restrictions = 0u;
	for (RestrictInfo* candidate : PList<RestrictInfo>(clauses)) {
		if (candidate != nullptr && count_mmsearch_occurrences(
				reinterpret_cast<Node*>(candidate->clause), oids) != 0u) {
			++removed_mmsearch_restrictions;
			continue;
		}
		executable_join_restrictions.append(candidate);
	}
	if (removed_mmsearch_restrictions != 1u) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqBatchJoin owned restriction identity is invalid"),
			 errdetail("removed_mmsearch_restrictions=%zu",
				 removed_mmsearch_restrictions)));
	}
	cscan->scan.plan.qual = extract_actual_clauses(
		executable_join_restrictions.ptr(), false);
	for (Node* executable : PList<Node>(cscan->scan.plan.qual)) {
		if (contains_mmsearch_qualifier(executable, oids)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("owned mmsearch remained in BioseqBatchJoin quals")));
		}
	}
	cscan->custom_exprs = NIL;
	cscan->custom_plans = custom_plans;
	cscan->custom_scan_tlist = combined_tlist;
	cscan->custom_relids = bms_copy(rel->relids);
	cscan->custom_private = list_make5(
		makeInteger(kBioseqBatchJoinPlanVersion),
		makeInteger(token),
		makeInteger(static_cast<int>(target_relation_oid)),
		makeInteger(target_storage_seq_attno),
		makeInteger(target_slot_seq_resno));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(target_slot_rowno_resno));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(query_slot_seq_resno));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(target_column_count));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(query_column_count));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(alphabet_code));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(storage_unit_byte_width));
	cscan->custom_private = lappend(cscan->custom_private, placeholder_sites);
	cscan->custom_private = lappend(
		cscan->custom_private, copyObject(mmsearch));
	cscan->custom_private = lappend(
		cscan->custom_private, makeInteger(index_read_requirement));
	cscan->custom_private = lappend(
		cscan->custom_private,
		make_batch_join_cost_const(honest_startup_cost));
	cscan->custom_private = lappend(
		cscan->custom_private,
		make_batch_join_cost_const(honest_total_cost));
	cscan->custom_private = lappend(
		cscan->custom_private,
		make_batch_join_cost_const(incumbent_cost_floor));
	Assert(list_length(cscan->custom_private) == kBioseqBatchJoinPrivateFields);
	return reinterpret_cast<Plan*>(cscan);
}

static Plan* BioseqPerRowJoin_PlanCustomPath(
		PlannerInfo* root, RelOptInfo* rel, CustomPath* best_path,
		List* tlist, List* clauses, List* custom_plans) {
	if (root == nullptr || rel == nullptr || best_path == nullptr
		|| list_length(custom_plans) != 1
		|| list_length(best_path->custom_private)
			!= kBioseqPerRowJoinPathPrivateFields) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch planner state is incomplete")));
	}
	auto private_integer = [best_path](const int index) -> int {
		Node* node = reinterpret_cast<Node*>(
			list_nth(best_path->custom_private, index));
		if (node == nullptr || !IsA(node, Integer)) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqPerRowMmsearch private integer is invalid"),
				 errdetail("field=%d", index)));
		}
		return intVal(node);
	};
	const int version = private_integer(0);
	const Oid target_relation_oid = static_cast<Oid>(private_integer(1));
	const Index target_varno = static_cast<Index>(private_integer(2));
	const AttrNumber target_storage_attno = static_cast<AttrNumber>(
		private_integer(3));
	Node* target_expression = reinterpret_cast<Node*>(
		list_nth(best_path->custom_private, 4));
	Node* query_expression = reinterpret_cast<Node*>(
		list_nth(best_path->custom_private, 5));
	FuncExpr* mmsearch = pg_node_try_as<FuncExpr>(reinterpret_cast<Node*>(
		list_nth(best_path->custom_private, 6)));
	const int alphabet_code = private_integer(7);
	const int storage_unit_byte_width = private_integer(8);
	const int index_read_requirement = private_integer(9);
	const Cost honest_startup_cost = batch_join_cost_from_node(
		reinterpret_cast<Node*>(list_nth(best_path->custom_private, 10)),
		"honest_startup_cost");
	const Cost honest_total_cost = batch_join_cost_from_node(
		reinterpret_cast<Node*>(list_nth(best_path->custom_private, 11)),
		"honest_total_cost");
	const Cost incumbent_cost_floor = batch_join_cost_from_node(
		reinterpret_cast<Node*>(list_nth(best_path->custom_private, 12)),
		"incumbent_cost_floor");
	const BioseqStubOids& oids = get_cached_stub_oids();
	if (version != kBioseqPerRowJoinPlanVersion
		|| !OidIsValid(target_relation_oid) || target_varno == 0
		|| target_storage_attno <= 0 || target_expression == nullptr
		|| query_expression == nullptr || mmsearch == nullptr
		|| !is_mmsearch_funcid(mmsearch->funcid, oids)
		|| exprType(target_expression) != oids.bioseq_seq_type_oid
		|| exprType(query_expression) != oids.bioseq_seq_type_oid
		|| exprTypmod(target_expression) != exprTypmod(query_expression)
		|| alphabet_code < 0 || alphabet_code > UINT8_MAX
		|| (storage_unit_byte_width != 1 && storage_unit_byte_width != 2
			&& storage_unit_byte_width != 4
			&& storage_unit_byte_width != 8)
		|| index_read_requirement != static_cast<int>(
			bioseq::indexing::IndexReadRequirement::require_absent)) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch private path ABI is invalid")));
	}
	Plan* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
	if (child_plan == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch child plan is missing")));
	}
	const AttrNumber target_resno = find_plan_tlist_expression_resno(
		child_plan->targetlist, target_expression);
	const AttrNumber query_resno = find_plan_tlist_expression_resno(
		child_plan->targetlist, query_expression);
	if (target_resno <= 0 || query_resno <= 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch operand projection is missing"),
			 errdetail("target_resno=%d query_resno=%d",
				static_cast<int>(target_resno),
				static_cast<int>(query_resno))));
	}
	List* combined_tlist = NIL;
	for (TargetEntry* source : PList<TargetEntry>(child_plan->targetlist)) {
		TargetEntry* copied = static_cast<TargetEntry*>(copyObject(source));
		copied->resno = static_cast<AttrNumber>(list_length(combined_tlist) + 1);
		combined_tlist = lappend(combined_tlist, copied);
	}
	const int child_column_count = list_length(combined_tlist);
	List* placeholder_sites = NIL;
	List* output_tlist = NIL;
	for (TargetEntry* output : PList<TargetEntry>(tlist)) {
		if (output == nullptr) continue;
		TargetEntry* copied = static_cast<TargetEntry*>(copyObject(output));
		BatchJoinPlaceholderRegistration registration{
			&oids, target_varno, target_storage_attno,
			&combined_tlist, &placeholder_sites, false, false};
		batch_join_placeholder_registration_visitor visitor;
		pg_walk_expression_including_root(
			reinterpret_cast<Node*>(copied->expr), registration, visitor);
		if (registration.edit_sequence_mismatch) {
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq.edit projection must reference the mmsearch target source")));
		}
		if (registration.invalid) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqPerRowMmsearch match helper identity is invalid")));
		}
		output_tlist = lappend(output_tlist, copied);
	}
	validate_batch_join_setrefs_coverage(output_tlist, combined_tlist);
	std::size_t owned_restrictions = 0u;
	for (RestrictInfo* candidate : PList<RestrictInfo>(clauses)) {
		if (candidate != nullptr && count_mmsearch_occurrences(
				reinterpret_cast<Node*>(candidate->clause), oids) != 0u) {
			++owned_restrictions;
		}
	}
	if (owned_restrictions != 1u) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqPerRowMmsearch owned restriction is invalid"),
			 errdetail("owned_restrictions=%zu", owned_restrictions)));
	}
	auto* cscan = makeNode(CustomScan);
	/* Post-join-recipe mode is a versioned BioseqBatchJoin shape.  Reuse the
	 * registered scan/executor method set so copied, cached, and deserialized
	 * plans retain the same public owner identity as the raw-Var mode. */
	cscan->methods = &BioseqBatchJoinScanMethods;
	cscan->flags = best_path->flags;
	cscan->scan.scanrelid = 0;
	cscan->scan.plan.targetlist = output_tlist;
	cscan->scan.plan.qual = NIL;
	cscan->custom_exprs = NIL;
	cscan->custom_plans = custom_plans;
	cscan->custom_scan_tlist = combined_tlist;
	cscan->custom_relids = bms_copy(rel->relids);
	cscan->custom_private = list_make5(
		makeInteger(kBioseqPerRowJoinPlanVersion),
		makeInteger(static_cast<int>(target_relation_oid)),
		makeInteger(static_cast<int>(target_storage_attno)),
		makeInteger(static_cast<int>(target_resno)),
		makeInteger(static_cast<int>(query_resno)));
	cscan->custom_private = lappend(cscan->custom_private,
		makeInteger(child_column_count));
	cscan->custom_private = lappend(cscan->custom_private,
		makeInteger(alphabet_code));
	cscan->custom_private = lappend(cscan->custom_private,
		makeInteger(storage_unit_byte_width));
	cscan->custom_private = lappend(cscan->custom_private, placeholder_sites);
	cscan->custom_private = lappend(
		cscan->custom_private, copyObject(mmsearch));
	cscan->custom_private = lappend(cscan->custom_private,
		makeInteger(index_read_requirement));
	cscan->custom_private = lappend(cscan->custom_private,
		make_batch_join_cost_const(honest_startup_cost));
	cscan->custom_private = lappend(cscan->custom_private,
		make_batch_join_cost_const(honest_total_cost));
	cscan->custom_private = lappend(cscan->custom_private,
		make_batch_join_cost_const(incumbent_cost_floor));
	Assert(list_length(cscan->custom_private)
		== kBioseqPerRowJoinPrivateFields);
	return reinterpret_cast<Plan*>(cscan);
}


/*
 * reparameterize_mutator changes all varnos referencing the topmost parent
 * of child_rel to instead reference child_rel directly. Both the Var and the
 * RestrictInfo are deep copied before being written (copy-before-write), and
 * a RestrictInfo's clause is recursively rewritten via the cursor.
 */
struct reparameterize_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor cursor, pg_node_view node, RelOptInfo &child_rel) const {
		if (Var *var = node.try_as<Var>())
		{
			if (bms_is_member(var->varno, child_rel.top_parent_relids))
			{
				var = pg_copy_node(var);
				var->varno = child_rel.relid;
			}
			return pg_node_mutation::with(pg_as_node(var));
		}

		if (RestrictInfo *rinfo = node.try_as<RestrictInfo>())
		{
			rinfo = pg_copy_node(rinfo);
			rinfo->clause = reinterpret_cast<Expr *>(cursor.descend(
				reinterpret_cast<Node *>(rinfo->clause)));
			return pg_node_mutation::with(pg_as_node(rinfo));
		}

		return pg_node_mutation::descend();
	}
};


/*
 * BioseqColumnScanPath_ReparameterizeCustomPathByChild is a method called when a
 * path is reparameterized directly to a child relation, rather than the
 * top-level parent.
 *
 * For instance, let there be a join of two partitioned columnar relations PX
 * and PY. A path for a BioseqColumnScan of PY3 might be parameterized by PX so
 * that the join qual "PY3.a = PX.a" (referencing the parent PX) can be pushed
 * down. But if the planner decides on a partition-wise join, then the path
 * will be reparameterized on the child table PX3 directly.
 *
 * When that happens, we need to update all Vars in the pushed-down quals to
 * reference PX3, not PX, to match the new parameterization. This method
 * notifies us that it needs to be done, and allows us to update the
 * information in custom_private.
 */
static List *
BioseqColumnScanPath_ReparameterizeCustomPathByChild(PlannerInfo *root,
												 List *custom_private,
												 RelOptInfo *child_rel)
{
	reparameterize_mutator mutator;
	return reinterpret_cast<List *>(pg_mutate_expression(reinterpret_cast<Node *>(custom_private), *child_rel, mutator));
}


static Node *
BioseqColumnScan_CreateCustomScanState(CustomScan *cscan)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_CreateCustomScanState() entered");
	BioseqScanState* bioseq_scan_state = BioseqScanState::allocate();
	bioseq_scan_state->as_custom_scan_state()->methods = &BioseqColumnScanExecuteMethods;
	bioseq_scan_state->as_custom_scan_state()->ss.ss_currentScanDesc = NULL;
	/*
	 * Defer runtime allocation to BeginCustomScan: that's where we
	 * have the EState (and therefore the per-query MemoryContext we
	 * register the cleanup callback against). A scan that never enters
	 * Begin (e.g. EXPLAIN without ANALYZE) therefore never allocates
	 * a runtime — bioseq_runtime() in the Explain hook is the only
	 * other caller that would lazily allocate, and that case is
	 * benign because EXPLAIN doesn't run the executor body. The
	 * `void*` storage on BioseqScanState lets the public scan-state
	 * header stay standard-layout; we re-cast in bioseq_runtime().
	 */
	bioseq_scan_state->runtime = nullptr;
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_CreateCustomScanState() returning");
	return bioseq_scan_state->as_node();
}


/*
 * eval_params_mutator evaluates Params in the expression and replaces them
 * with Consts, preserving the existing ExecInitExprWithParams / ExecEvalExpr
 * folding path for every Param (not a PARAM_EXEC-only direct lookup).
 */
struct eval_params_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor, pg_node_view node, ExprContext &econtext) const {
		if (Param *param = node.try_as<Param>())
		{
			int16_t typLen;
			bool typByVal;
			bool isnull;

			get_typlenbyval(param->paramtype, &typLen, &typByVal);

			/* XXX: should save ExprState for efficiency */
			ExprState *exprState = ExecInitExprWithParams(pg_as_expr(node.raw()),
														  econtext.ecxt_param_list_info);
			Datum pval = ExecEvalExpr(exprState, &econtext, &isnull);

			return pg_node_mutation::with(reinterpret_cast<Node *>(makeConst(param->paramtype,
									  param->paramtypmod,
									  param->paramcollid,
									  static_cast<int>(typLen),
									  pval,
									  isnull,
									  typByVal)));
		}

		return pg_node_mutation::descend();
	}
};

/*
 * Thin wrapper for the direct callers (qual folding at Begin/ReScan and the
 * per-Param rebind fold) so they keep the (node, econtext) call shape.
 */
[[nodiscard]] static Node *
eval_params(Node *node, ExprContext *econtext)
{
	eval_params_mutator mutator;
	return pg_mutate_expression(node, *econtext, mutator);
}

enum class MmsearchParamResolution : std::uint8_t {
	configured,
	null_value,
};

/* Resolve the exact Param that registered the primary mmsearch query.  Both
 * supported Param kinds use PostgreSQL's expression machinery so PARAM_EXEC
 * and PARAM_EXTERN read from their correct executor-owned storage. */
[[nodiscard]] static MmsearchParamResolution resolve_mmsearch_param_query(
		BioseqScanRuntime& runtime,
		ExprContext* econtext,
		const char* phase) {
	if (!runtime.param_rebind.active || econtext == nullptr
		|| (runtime.param_rebind.paramkind != PARAM_EXEC
			&& runtime.param_rebind.paramkind != PARAM_EXTERN)
		|| runtime.param_rebind.mmsearch_fe == nullptr
		|| list_length(runtime.param_rebind.mmsearch_fe->args) < 2) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq mmsearch Param rebind metadata is invalid"),
			 errdetail("phase=%s paramid=%d paramkind=%d",
				phase,
				runtime.param_rebind.paramid,
				runtime.param_rebind.paramkind)));
	}

	PList<Node> arguments(runtime.param_rebind.mmsearch_fe->args);
	Param* param = pg_node_try_as<Param>(arguments.second());
	if (param == nullptr
		|| static_cast<int>(param->paramkind)
			!= runtime.param_rebind.paramkind
		|| param->paramid != runtime.param_rebind.paramid
		|| param->paramtype != runtime.param_rebind.param_typoid
		|| param->paramtypmod != runtime.param_rebind.param_typmod) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("bioseq mmsearch Param changed after registration"),
			 errdetail("phase=%s paramid=%d paramkind=%d",
				phase,
				runtime.param_rebind.paramid,
				runtime.param_rebind.paramkind)));
	}

	Node* folded = eval_params(reinterpret_cast<Node*>(param), econtext);
	Const* query_const = pg_node_try_as<Const>(folded);
	if (query_const == nullptr
		|| query_const->consttype != runtime.param_rebind.param_typoid
		|| query_const->consttypmod != runtime.param_rebind.param_typmod) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq mmsearch Param resolved to an invalid value"),
			 errdetail("phase=%s paramid=%d paramkind=%d",
				phase,
				runtime.param_rebind.paramid,
				runtime.param_rebind.paramkind)));
	}
	if (query_const->constisnull) {
		runtime.buffer.clear_mmsearch_context();
		runtime.mmsearch_null_input = true;
		bioseq_log<DBG>(
			"[bioseq] pg: {} mmsearch Param paramid={} kind={} resolved "
			"to NULL; strict qual matches no rows",
			phase,
			runtime.param_rebind.paramid,
			runtime.param_rebind.paramkind);
		return MmsearchParamResolution::null_value;
	}

	pgseq_t* query_hdr = reinterpret_cast<pgseq_t*>(
		PG_DETOAST_DATUM(query_const->constvalue));
	const auto query_type_params = query_hdr->get_type_params();
	if (query_type_params.alphabet_code
			!= runtime.param_rebind.alphabet_code
		|| query_type_params.storage_unit_byte_width
			!= runtime.param_rebind.storage_unit_byte_width) {
		const unsigned actual_alphabet = query_type_params.alphabet_code;
		const unsigned actual_width =
			query_type_params.storage_unit_byte_width;
		if (reinterpret_cast<void*>(query_hdr)
			!= reinterpret_cast<void*>(
				DatumGetPointer(query_const->constvalue))) {
			pfree(query_hdr);
		}
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq mmsearch Param payload typmod is incompatible"),
			 errdetail(
				"phase=%s paramid=%d paramkind=%d "
				"expected_alphabet=%u expected_width=%u "
				"actual_alphabet=%u actual_width=%u",
				phase,
				runtime.param_rebind.paramid,
				runtime.param_rebind.paramkind,
				static_cast<unsigned>(
					runtime.param_rebind.alphabet_code),
				static_cast<unsigned>(
					runtime.param_rebind.storage_unit_byte_width),
				actual_alphabet,
				actual_width)));
	}
	const std::size_t query_total_bytes =
		static_cast<std::size_t>(VARSIZE(query_hdr));
	std::vector<char> query_varlena_bytes(query_total_bytes);
	std::memcpy(
		query_varlena_bytes.data(), query_hdr, query_total_bytes);
	const std::uint32_t bp_size = query_hdr->bp_size;
	if (reinterpret_cast<void*>(query_hdr)
		!= reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
		pfree(query_hdr);
	}
	runtime.buffer.configure_mmsearch(
		runtime.param_rebind.seq_attno,
		runtime.param_rebind.column_name,
		std::move(query_varlena_bytes),
		runtime.param_rebind.alphabet_code,
		runtime.param_rebind.storage_unit_byte_width,
		runtime.param_rebind.primary_key_attno);
	runtime.mmsearch_null_input = false;
	bioseq_log<DBG>(
		"[bioseq] pg: {} rebound mmsearch query bp_size={} extras_count={} "
		"from Param paramid={} kind={}",
		phase,
		bp_size,
		runtime.buffer.extra_query_count(),
		runtime.param_rebind.paramid,
		runtime.param_rebind.paramkind);
	return MmsearchParamResolution::configured;
}

[[noreturn]] static void raise_column_scan_vulkan_error_as_cpp(
		std::string_view detail,
		std::string_view hint) {
	const std::string owned_detail{detail};
	const std::string owned_hint{hint};
	const char* detail_cstr = owned_detail.c_str();
	const char* hint_cstr = owned_hint.c_str();
	bioseq::pg_utils::invoke_pg_as_cpp([detail_cstr, hint_cstr] {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg_internal("%s", "Vulkan aligner execution failed"),
			 errdetail_internal("%s", detail_cstr),
			 errhint("%s", hint_cstr)));
	});
	pg_unreachable();
}


static void
BioseqColumnScan_BeginCustomScanImpl(CustomScanState *cscanstate, EState *estate, int eflags)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() entered");
	CustomScan *cscan = reinterpret_cast<CustomScan *>(cscanstate->ss.ps.plan);
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(cscanstate);
	ExprContext *stdecontext = cscanstate->ss.ps.ps_ExprContext;
	cscanstate->ss.ss_currentScanDesc = NULL;
	register_runtime_cleanup_callback(bioseqScanState, estate);
	BioseqScanRuntime& runtime = bioseq_runtime(bioseqScanState);
	runtime.locator_passthrough = false;
	runtime.lazy_source_projection = false;
	runtime.lazy_source_producer_ids.clear();
	runtime.lazy_source_column_attnos.clear();
	runtime.locator_parent_token = 0;
	runtime.locator_rowno_resno = InvalidAttrNumber;
	runtime.edit_predicate_active = false;
	runtime.edit_exclusive_upper_bound = 0;
	runtime.edit_sequence_attno = InvalidAttrNumber;
	runtime.match_sequence_varno = 0;
	runtime.match_sequence_attno = InvalidAttrNumber;
	runtime.mmsearch_null_input = false;
	for (Node* private_node : PList<Node>(cscan->custom_private)) {
		if (private_node == nullptr || !IsA(private_node, List)) continue;
		List* tag = reinterpret_cast<List*>(private_node);
		if (list_length(tag) == 0
			|| !IsA(reinterpret_cast<Node*>(linitial(tag)), Integer)) continue;
		const int magic = intVal(linitial(tag));
		if (magic == kBioseqLocatorPassthroughMagic) {
			if (list_length(tag) != 4
				|| intVal(list_nth(tag, 1)) != kBioseqBatchJoinPlanVersion) {
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Bioseq locator passthrough plan tag is invalid")));
			}
			runtime.locator_passthrough = true;
			runtime.locator_parent_token = intVal(list_nth(tag, 2));
			runtime.locator_rowno_resno = static_cast<AttrNumber>(
				intVal(list_nth(tag, 3)));
			} else if (magic == kBioseqSliceSourceMagic) {
				Node* version_node = list_length(tag) >= 2
					? reinterpret_cast<Node*>(list_nth(tag, 1)) : nullptr;
				Node* bindings_node = list_length(tag) == 3
					? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
				if (list_length(tag) != 3
					|| version_node == nullptr || !IsA(version_node, Integer)
					|| intVal(version_node) != kBioseqSliceSourceVersion
					|| (bindings_node != nullptr && !IsA(bindings_node, List))) {
					ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Bioseq SliceProject source tag is invalid")));
				}
				runtime.lazy_source_projection = true;
				const int relation_natts = RelationGetDescr(
					cscanstate->ss.ss_currentRelation)->natts;
				runtime.lazy_source_producer_ids.assign(
					static_cast<std::size_t>(
						list_length(cscan->scan.plan.targetlist) + 1), 0u);
				runtime.lazy_source_column_attnos.assign(
					static_cast<std::size_t>(
						list_length(cscan->scan.plan.targetlist) + 1),
					InvalidAttrNumber);
				List* bindings = bindings_node == nullptr ? NIL
					: reinterpret_cast<List*>(bindings_node);
				for (Node* raw_binding : PList<Node>(bindings)) {
					List* binding = raw_binding != nullptr && IsA(raw_binding, List)
						? reinterpret_cast<List*>(raw_binding) : NIL;
					if (list_length(binding) != 3
						|| !IsA(reinterpret_cast<Node*>(linitial(binding)), Integer)
						|| !IsA(reinterpret_cast<Node*>(lsecond(binding)), Integer)
						|| !IsA(reinterpret_cast<Node*>(lthird(binding)), Integer)) {
						ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("Bioseq SliceProject source binding is invalid")));
					}
					const int resno = intVal(linitial(binding));
					const int producer_id = intVal(lsecond(binding));
					const int column_attno = intVal(lthird(binding));
					if (resno <= 0
						|| resno > list_length(cscan->scan.plan.targetlist)
						|| producer_id < 0
						|| column_attno <= 0
						|| column_attno > relation_natts
						|| runtime.lazy_source_column_attnos[resno]
							!= InvalidAttrNumber) {
						ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("Bioseq SliceProject source binding is out of range")));
					}
					runtime.lazy_source_producer_ids[resno] =
						static_cast<std::uint32_t>(producer_id);
					runtime.lazy_source_column_attnos[resno] =
						static_cast<AttrNumber>(column_attno);
				}
		} else if (magic == kBioseqEditPredicateMagic) {
			Const* threshold = list_length(tag) == 4
				? pg_node_try_as<Const>(reinterpret_cast<Node*>(list_nth(tag, 2)))
				: nullptr;
			Node* sequence_attno_node = list_length(tag) == 4
				? reinterpret_cast<Node*>(list_nth(tag, 3))
				: nullptr;
			if (list_length(tag) != 4
				|| intVal(list_nth(tag, 1)) != kBioseqEditPredicateVersion
				|| threshold == nullptr || threshold->constisnull
				|| threshold->consttype != INT8OID
				|| sequence_attno_node == nullptr
				|| !IsA(sequence_attno_node, Integer)
				|| intVal(sequence_attno_node) <= 0
				|| intVal(sequence_attno_node) > MaxAttrNumber) {
				ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Bioseq edit-predicate plan tag is invalid")));
			}
			runtime.edit_predicate_active = true;
			runtime.edit_exclusive_upper_bound =
				DatumGetInt64(threshold->constvalue);
			runtime.edit_sequence_attno = static_cast<AttrNumber>(
				intVal(sequence_attno_node));
		}
	}

	/*
	 * Make a new ExprContext just like the existing one, except that we don't
	 * reset it every tuple.
	 */
	ExecAssignExprContext(estate, &cscanstate->ss.ps);
	bioseqScanState->css_RuntimeContext = cscanstate->ss.ps.ps_ExprContext;
	cscanstate->ss.ps.ps_ExprContext = stdecontext;

	ResetExprContext(bioseqScanState->css_RuntimeContext);
	PList<List> custom_exprs(cscan->custom_exprs);
	List *plainClauses = custom_exprs.first();
	List* owned_mmsearch_quals = list_length(cscan->custom_exprs) >= 4
		? reinterpret_cast<List*>(list_nth(cscan->custom_exprs, 3))
		: NIL;
	if (list_length(owned_mmsearch_quals) > 1) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("Bioseq scan plan owns more than one mmsearch qualifier")));
	}
	List *storagePlainClauses = storage_prunable_quals_without_mmsearch(
		plainClauses, get_cached_stub_oids());
	bioseqScanState->qual = reinterpret_cast<List *>(eval_params(
		reinterpret_cast<Node *>(storagePlainClauses), bioseqScanState->css_RuntimeContext));

	/*
	 * Third list in custom_exprs should contain vectorized qual list if not
	 * NULL.
	 */
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() examining custom_exprs for vectorized qual");
	List* vectorized_qual_list = custom_exprs.third();
	if (vectorized_qual_list != NIL) {
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() vectorized qual list present");
		bioseqScanState->vectorization.vectorizedQualList = vectorized_qual_list;
	}

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() iterating custom_private");
	for (auto private_node : PList<Node>(cscan->custom_private))
	{
		Const* private_const_node = pg_node_try_as<Const>(private_node);
		if (private_const_node == nullptr) {
			continue;
		}
		if (private_const_node->consttype == CUSTOM_SCAN_VECTORIZED_AGGREGATE)
		{
			bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() setting vectorizationAggregate");
			bioseqScanState->vectorization.vectorizationAggregate = private_const_node->constvalue;
		}
	}
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() custom_private iteration complete");

	/*
	 * Vectorization is enabled if global variable is set and there is at least one
	 * filter which can be vectorized.
	 */
	bioseqScanState->vectorization.vectorizationEnabled =
		bioseq_enable_vectorization && !runtime.locator_passthrough
		&& !runtime.lazy_source_projection &&
		(bioseqScanState->vectorization.vectorizedQualList != NULL ||
		 bioseqScanState->vectorization.vectorizationAggregate);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() guc_vec={} has_vec_qual={} vec_aggregate={}",
		bioseq_enable_vectorization,
		bioseqScanState->vectorization.vectorizedQualList != NULL,
		bioseqScanState->vectorization.vectorizationAggregate);
	if (bioseqScanState->vectorization.vectorizationAggregate)
	{
		ScanState *node = reinterpret_cast<ScanState *>(&bioseqScanState->custom_scanstate.ss);

		if (node->ps.ps_ProjInfo)
		{
			bioseqScanState->vectorization.resultVectorSlot =
				CreateVectorTupleTableSlot(node->ps.ps_ProjInfo->pi_state.resultslot->tts_tupleDescriptor);
		}
		else
		{
			bioseqScanState->vectorization.resultVectorSlot =
				CreateVectorTupleTableSlot(node->ps.ps_ResultTupleDesc);
		}
	}

	if (bioseqScanState->vectorization.vectorizationEnabled)
	{
		bioseqScanState->vectorization.scanVectorSlot =
			CreateVectorTupleTableSlot(cscanstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	}

	bioseqScanState->attrNeeded =
		BioseqColumnAttrNeeded(&cscanstate->ss, bioseqScanState->vectorization.vectorizedQualList);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() iterating attrNeeded bitmap members");
	for (int attrIndex : bioseqScanState->attrNeeded)
	{
		bioseqScanState->vectorization.attrNeededList.append(attrIndex);
	}

	/*
	 * If we have pending changes that need to be flushed (row_mask after update/delete)
	 * or new stripe we need to to them here because sequential columnar scan 
	 * can have parallel execution and updated are not allowed in parallel mode.
	 */
	bioseqScanState->snapshot = estate->es_snapshot;
	bioseqScanState->snapshotRegisteredByUs = false;
	Oid relationOid = cscanstate->ss.ss_currentRelation->rd_locator.relNumber;

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() checking parallel mode");
	if(!IsInParallelMode())
	{
		RowMaskFlushWriteStateForRelfilenode(relationOid, GetCurrentSubTransactionId());

		FlushWriteStateWithNewSnapshot(relationOid, &bioseqScanState->snapshot,
									   &bioseqScanState->snapshotRegisteredByUs);
	}
	if (runtime.lazy_source_projection || runtime.locator_passthrough) {
		runtime.lazy_source_provider =
			bioseq::storage::create_pg_sequence_source_provider(
				cscanstate->ss.ss_currentRelation, bioseqScanState->snapshot);
		if (runtime.lazy_source_provider == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("Bioseq source provider is unavailable")));
		}
	}

	/*
	 * Discover the primary-key column by scanning the unmutated
	 * targetlist for the first bioseq.<field>(Var) FuncExpr and using
	 * its Var argument's varattno. BeginCustomScan runs before
	 * CustomExecScan, so at this point cscan->scan.plan.targetlist
	 * still contains the FuncExprs — they haven't been rewritten to
	 * Const* yet (the rewrite target is newtargetlist, not
	 * scan.plan.targetlist).
	 */
	const BioseqStubOids& scan_stub_oids = get_cached_stub_oids();
	for (Node* executable_qual : PList<Node>(cscan->scan.plan.qual)) {
		if (contains_mmsearch_qualifier(executable_qual, scan_stub_oids)) {
			ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("mmsearch qualifier is not owned by a Bioseq executor"),
				 errdetail("Nested, stacked, volatile, security-sensitive, or otherwise unsupported mmsearch shapes cannot execute through the SQL function."),
				 errhint("Use exactly one top-level mmsearch predicate with supported operands, or split the query into separate levels.")));
		}
	}
	AttrNumber primary_key_attno = InvalidAttrNumber;
	for (auto tgt : PList<TargetEntry>(cscan->scan.plan.targetlist))
	{
		if (tgt == nullptr) continue;
		FuncExpr* fe = pg_node_try_as<FuncExpr>(pg_as_node(tgt->expr));
		if (fe == nullptr) continue;
		if (!is_bioseq_projection_funcid(fe->funcid, scan_stub_oids)) continue;
		if (list_length(fe->args) < 1) continue;
		PList<Node> fe_args(fe->args);
		Node* arg0 = fe_args.first();
		Var* pk_var = pg_node_try_as<Var>(arg0);
		if (pk_var == nullptr) continue;
		if (pk_var->varattno <= 0) continue;
		/* Projection helpers are semantically tied to the current MatchResult,
		 * not to an int key argument.  The direct-query surface therefore
		 * permits bioseq.edit(seq) / bioseq.cigar(seq).  Retain the historical
		 * optional PK capture only when the supplied Var is actually INT4. */
		if (pk_var->vartype != INT4OID) continue;
		primary_key_attno = pk_var->varattno;
		bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() detected primary_key varattno={} from bioseq projection funcid={}",
				static_cast<int>(primary_key_attno), fe->funcid);
		break;
	}

	/*
	 * Register the per-query MemoryContext callback BEFORE touching
	 * the runtime so an ereport(ERROR) further down still triggers
	 * cleanup on transaction abort. Idempotent across rescans.
	 */
	register_runtime_cleanup_callback(bioseqScanState, estate);

	/*
	 * Decode the one planner-owned expression from custom_exprs and configure
	 * the dispatch buffer. The executable qual list is intentionally ignored.
	 */
	/* Begin is the only place that should drop extras as part of the
	 * primary reset — every Begin call repopulates them from scratch
	 * via the qual loop. ReScan / failure paths must NOT drop extras
	 * (see clear_mmsearch_context's comment for the §9 review fix). */
	runtime.buffer.clear_mmsearch_context(/*clear_extras=*/true);
	runtime.param_rebind = MmsearchParamRebind{};

	Relation relation_for_attname = cscanstate->ss.ss_currentRelation;
	const Oid relation_oid_for_attname = RelationGetRelid(relation_for_attname);

	/*
	 * State retained by the legacy classifier/configuration branches. Planner
	 * ownership guarantees this loop contains at most one expression.
	 *
	 * `primary_configured` flips once the sole owned expression is installed
	 * through the Const, Param-rebind, or same-relation Var path.
	 */
	bool primary_configured = false;
	AttrNumber primary_seq_attno = InvalidAttrNumber;
	FuncExpr* primary_mmsearch_fe = nullptr;

	/*
	 * Process only the private expression selected by the planner. Executable
	 * quals never participate in alignment ownership, and stacked calls are
	 * rejected before a CustomPath is built.
	 *
	 * Solution: classify quals first, then pick the primary by
	 * priority:
	 *   - Var-same-rel  > Param  > Const
	 * Var-same-rel and Param NEED to be the primary (their machinery
	 * lives in the buffer's mmsearch_context / param_rebind slots
	 * which only exist once per scan). Const can be either primary
	 * OR an extra. So putting Param first guarantees the AND-stack
	 * works regardless of planner qual order.
	 *
	 * If we discover a Var-same-rel and a Param in the same plan
	 * we reject the Param (we only have one primary slot); same for
	 * multi-Param or multi-Var-same-rel. Every top-level mmsearch qual must
	 * be accounted for below: unsupported combinations fail closed rather
	 * rather than reaching the fail-closed SQL rendezvous stub.
	 */
	struct ClassifiedMmsearchQual {
		FuncExpr* fe;
		Node* arg0;
		Node* arg1;
		bool is_const;
		bool is_param;
		bool is_var_same_rel;
	};
	std::vector<ClassifiedMmsearchQual> classified_quals;
	std::size_t top_level_mmsearch_qual_count = 0;
	for (auto qual_node : PList<Node>(owned_mmsearch_quals))
	{
		FuncExpr* fe_classify = pg_node_try_as<FuncExpr>(qual_node);
		if (fe_classify == nullptr
			|| !is_mmsearch_funcid(fe_classify->funcid, scan_stub_oids)) {
			/* The storage-pruning copy deliberately drops every clause whose
			 * subtree contains mmsearch(), but the alignment runtime currently
			 * consumes only a top-level mmsearch FuncExpr. Letting a BooleanTest,
			 * NOT, CASE, COALESCE, or other wrapper proceed would leave the
			 * runtime unconfigured and evaluate the immutable SQL stub instead.
			 * That stub is only a planner/executor rendezvous marker and must never
			 * become the query's alignment result. */
			if (contains_mmsearch_qualifier(qual_node, scan_stub_oids)) {
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("nested mmsearch qualifier shape is unsupported"),
					 errdetail("mmsearch must be consumed by the alignment runtime; executing its SQL stub without alignment is forbidden"),
					 errhint("Place mmsearch(...) directly in a top-level AND conjunct.")));
			}
			continue;
		}
		++top_level_mmsearch_qual_count;
		/* Reject a parameterized/column-valued third argument even when this
		 * qualifier is later rejected as an unsupported primary/extra shape. */
		(void) require_aligner_options_const(fe_classify, scan_stub_oids);
		pg_first_two_node_args classify_args = pg_first_two_args_if_arity_between(*fe_classify, 2, 3);
		if (!classify_args.present()) {
			continue;
		}
		Node* a0 = classify_args.first;
		Node* a1 = classify_args.second;
		Var* v0 = pg_node_try_as<Var>(a0);
		if (v0 == nullptr) {
			continue;
		}
		const bool is_c = pg_node_view{a1}.is<Const>();
		const bool is_p = pg_node_view{a1}.is<Param>();
		Var* a1_var = pg_node_try_as<Var>(a1);
		const bool is_v = a1_var != nullptr
			&& a1_var->varno == v0->varno
			&& a1_var->varattno > 0;
		if (!is_c && !is_p && !is_v) {
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() classifier skipping mmsearch (arg1 tag={} is not Const/Param/same-rel-Var)",
					pg_node_tag_name(pg_node_tag_or_invalid(a1)));
			continue;
		}
		classified_quals.push_back({fe_classify, a0, a1, is_c, is_p, is_v});
	}
	/*
	 * Sort by priority (Var-same-rel > Param > Const) using stable
	 * partition so quals of equal priority keep planner order. The
	 * Var-same-rel and Param branches can each fire AT MOST ONCE per
	 * scan (they consume the single primary slot); unsupported subsequent
	 * quals fail closed below.
	 */
	std::stable_sort(classified_quals.begin(), classified_quals.end(),
		[](const ClassifiedMmsearchQual& lhs, const ClassifiedMmsearchQual& rhs) {
			auto pri = [](const ClassifiedMmsearchQual& q) {
				if (q.is_var_same_rel) return 0;
				if (q.is_param)        return 1;
				return 2;  /* Const */
			};
			return pri(lhs) < pri(rhs);
		});

	std::size_t consumed_mmsearch_qual_count = 0;
	for (auto& classified : classified_quals)
	{
		FuncExpr* fe                 = classified.fe;
		Node* arg0                   = classified.arg0;
		Node* arg1                   = classified.arg1;
		const bool arg1_is_const     = classified.is_const;
		const bool arg1_is_param     = classified.is_param;
		const bool arg1_is_var_same_rel = classified.is_var_same_rel;
		Var* col_var                 = reinterpret_cast<Var*>(arg0);

		if (arg1_is_const)
		{
			Const* query_const = reinterpret_cast<Const*>(arg1);
			if (query_const->consttype != scan_stub_oids.bioseq_seq_type_oid)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 is not a bioseq.seq Const (type={}, null={})",
						query_const->consttype, query_const->constisnull);
				continue;
			}
		}
		else if (arg1_is_param)
		{
			Param* query_param = reinterpret_cast<Param*>(arg1);
			if (query_param->paramtype != scan_stub_oids.bioseq_seq_type_oid)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 Param has paramtype={}, expected bioseq.seq ({})",
						query_param->paramtype, scan_stub_oids.bioseq_seq_type_oid);
				continue;
			}
			/* PARAM_EXEC is installed by a parameterized Nested Loop;
			 * PARAM_EXTERN is supplied by PREPARE/client parameters. The
			 * shared evaluator below routes each kind through PostgreSQL's
			 * own ExprContext storage. */
			if (query_param->paramkind != PARAM_EXEC
				&& query_param->paramkind != PARAM_EXTERN)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 Param has unsupported paramkind={}",
						static_cast<int>(query_param->paramkind));
				continue;
			}
		}
		else /* arg1_is_var_same_rel */
		{
			Var* query_var = reinterpret_cast<Var*>(arg1);
			if (query_var->vartype != scan_stub_oids.bioseq_seq_type_oid)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 Var vartype={} not bioseq.seq",
						query_var->vartype);
				continue;
			}
		}

		TupleDesc tuple_descriptor = RelationGetDescr(relation_for_attname);
		if (col_var->varattno <= 0 || col_var->varattno > tuple_descriptor->natts)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg0 varattno={} out of range natts={}",
					static_cast<int>(col_var->varattno), tuple_descriptor->natts);
			continue;
		}
		Form_pg_attribute attribute_form = TupleDescAttr(tuple_descriptor, col_var->varattno - 1);
		if (attribute_form->atttypid != scan_stub_oids.bioseq_seq_type_oid)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg0 column type is {}, expected bioseq.seq ({})",
					attribute_form->atttypid, scan_stub_oids.bioseq_seq_type_oid);
			continue;
		}

		/*
		 * Resolve arg1's typmod and decide if a coalesce or
		 * cross-alphabet recompute is required. Three classification
		 * outcomes:
		 *   - same (alphabet, width): direct dispatch, no extras.
		 *   - same alphabet, different width: coalesce (cast arg1 to
		 *     column's typmod). Index reuse is safe because the
		 *     sidecar's minimizers were sketched against the
		 *     column's alphabet — width is irrelevant for the
		 *     dna4/dna5 minimizer hash.
		 *   - different alphabet: coalesce AND mark for recompute.
		 *     The sidecar minimizers are alphabet-specific (bp_size
		 *     of dna4 = 2 bits/base, dna5 = 3 bits/base), so reusing
		 *     them would silently produce zero seed hits.
		 */
		if (col_var->vartypmod < 0)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg0 has unconstrained typmod; bioseq.seq columns require an explicit (alphabet,width)");
			continue;
		}
		int32 arg1_typmod = -1;
		if (arg1_is_const) {
			arg1_typmod = reinterpret_cast<Const*>(arg1)->consttypmod;
		} else if (arg1_is_param) {
			arg1_typmod = reinterpret_cast<Param*>(arg1)->paramtypmod;
		} else /* arg1_is_var_same_rel */ {
			arg1_typmod = reinterpret_cast<Var*>(arg1)->vartypmod;
		}
		const bool runtime_typed_external_param =
			arg1_is_param && arg1_typmod < 0
			&& reinterpret_cast<Param*>(arg1)->paramkind == PARAM_EXTERN;
		if (arg1_typmod < 0 && !runtime_typed_external_param)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 has unconstrained typmod; add ::bioseq.seq('dna4', N) to the literal/param");
			continue;
		}
		const auto unpacked_typmod      = compact_sequence_stdc_type_params::decode(col_var->vartypmod);
		/* PostgreSQL's generic PREPARE Param retains only the base type OID and
		 * commonly reports paramtypmod=-1 even when the declaration spells a
		 * bioseq typmod.  Its varlena header remains authoritative and is checked
		 * on every Begin/ReScan by resolve_mmsearch_param_query(). */
		const auto unpacked_arg1_typmod = runtime_typed_external_param
			? unpacked_typmod
			: compact_sequence_stdc_type_params::decode(arg1_typmod);
		const bool typmod_match =
			(unpacked_typmod.alphabet_code == unpacked_arg1_typmod.alphabet_code)
			&& (unpacked_typmod.storage_unit_byte_width == unpacked_arg1_typmod.storage_unit_byte_width);
		const bool same_alphabet =
			unpacked_typmod.alphabet_code == unpacked_arg1_typmod.alphabet_code;
		/*
		 * PARAM_EXEC and any PARAM_EXTERN carrying a planner typmod require an
		 * exact match; the rebind path has no implicit re-encode. A generic
		 * PARAM_EXTERN with paramtypmod=-1 is checked against the same target
		 * shape from its physical pgseq header at runtime instead.
		 */
		if (arg1_is_param && !runtime_typed_external_param && !typmod_match)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch typmod mismatch (Param) col=({},{}) arg1=({},{}) — rejecting (Param coalesce not yet supported)",
					static_cast<unsigned>(unpacked_typmod.alphabet_code),
					static_cast<unsigned>(unpacked_typmod.storage_unit_byte_width),
					static_cast<unsigned>(unpacked_arg1_typmod.alphabet_code),
					static_cast<unsigned>(unpacked_arg1_typmod.storage_unit_byte_width));
			continue;
		}
		/*
		 * Var-same-rel ALWAYS shares the column's typmod (every Var
		 * of column type bioseq.seq inherits the column's vartypmod
		 * during planning), so we don't have to coalesce here. Log a
		 * defensive mismatch in case the planner ever surprises us.
		 */
		if (arg1_is_var_same_rel && !typmod_match)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() unexpected same-rel Var typmod mismatch col=({},{}) arg1=({},{}) — rejecting",
					static_cast<unsigned>(unpacked_typmod.alphabet_code),
					static_cast<unsigned>(unpacked_typmod.storage_unit_byte_width),
					static_cast<unsigned>(unpacked_arg1_typmod.alphabet_code),
					static_cast<unsigned>(unpacked_arg1_typmod.storage_unit_byte_width));
			continue;
		}

		/*
		 * Sanity-check the primary key column: must be INT4 for the POC.
		 * If it is not INT4 or InvalidAttrNumber we still configure — the
		 * batch processor will treat the column as null-PK and filter
		 * those rows out (see TupleDispatchBuffer::run_batch_and_build_cursors).
		 */
		if (primary_key_attno != InvalidAttrNumber
			&& primary_key_attno <= tuple_descriptor->natts)
		{
			Form_pg_attribute pk_attr_form = TupleDescAttr(tuple_descriptor, primary_key_attno - 1);
			if (pk_attr_form->atttypid != INT4OID)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() primary key column type is {} not INT4OID; rows will be mis-keyed",
						pk_attr_form->atttypid);
			}
		}

		char* attname_cstr = get_attname(relation_oid_for_attname, col_var->varattno, /*missing_ok=*/true);
		if (attname_cstr == nullptr)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() get_attname returned NULL for attno={}",
					static_cast<int>(col_var->varattno));
			continue;
		}

		if (primary_configured)
		{
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Bioseq scan reached a second owned mmsearch expression")));
		}

		if (arg1_is_const)
		{
			/*
			 * Literal-query case. Detoast the query bioseq.seq varlena once;
			 * submit_scan reuses the bytes across every row so the TOAST read
			 * never happens on the hot path. The buffer owns a std::vector<char>
			 * copy so the bytes outlive the planner's per-query memory context.
			 */
			Const* query_const = reinterpret_cast<Const*>(arg1);
			if (query_const->constisnull) {
				runtime.mmsearch_null_input = true;
			} else {
			pgseq_t* query_hdr = reinterpret_cast<pgseq_t*>(
				PG_DETOAST_DATUM(query_const->constvalue));
			const size_t query_total_bytes = static_cast<size_t>(VARSIZE(query_hdr));
			std::vector<char> query_varlena_bytes(query_total_bytes);
			std::memcpy(query_varlena_bytes.data(), query_hdr, query_total_bytes);
			if (reinterpret_cast<void*>(query_hdr)
				!= reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue)))
			{
				pfree(query_hdr);
			}

			runtime.buffer.configure_mmsearch(col_var->varattno,
										  std::string(attname_cstr),
										  std::move(query_varlena_bytes),
										  unpacked_typmod.alphabet_code,
										  unpacked_typmod.storage_unit_byte_width,
										  primary_key_attno);
			/*
			 * Typmod-coalesce path: when the column and literal
			 * share the alphabet but differ in width — e.g. column
			 * is dna4/32, literal is dna4/64 — re-encode the
			 * literal at the column's width so the static dispatch
			 * agrees. The sidecar's minimizers stay valid because
			 * width does not affect dna4 minimizer hashes (the
			 * encoder collapses storage_unit at sketch time).
			 *
			 * Cross-alphabet path: when alphabets differ — e.g.
			 * column is dna4, literal is dna5 — we still re-encode
			 * the literal at the column's typmod, but ALSO mark
			 * the buffer for minimizer recompute. The sidecar
			 * minimizers were sketched at dna4; a dna5 query
			 * routes through a different bits-per-letter encoding
			 * and the hashes are incompatible.
			 */
			if (!typmod_match) {
				/*
				 * Re-encode the literal at the column's typmod. We
				 * have to re-detoast here because the previous
				 * detoast scope above already pfree'd the local
				 * copy. apply_typmod_coalesce stores the result
				 * inside the buffer's mmsearch_context.
				 */
				pgseq_t* src_hdr2 =
					reinterpret_cast<pgseq_t*>(
						PG_DETOAST_DATUM(query_const->constvalue));
				pgseq_t* recoded =
					(src_hdr2->get_type_params() == unpacked_typmod)
						? nullptr
						: ::bioseq::simd::active_target::stdc_recode_to_type_params(
							*src_hdr2, unpacked_typmod, pg_alloc);
				if (reinterpret_cast<void*>(src_hdr2)
					!= reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue))) {
					pfree(src_hdr2);
				}
				if (recoded != nullptr) {
					const size_t recoded_bytes = static_cast<size_t>(VARSIZE(recoded));
					std::vector<char> coalesced_bytes(recoded_bytes);
					std::memcpy(coalesced_bytes.data(), recoded, recoded_bytes);
					pfree(recoded);
					runtime.buffer.apply_typmod_coalesce(std::move(coalesced_bytes));
					if (!same_alphabet) {
						runtime.buffer.mark_alphabet_mismatch_recompute();
						bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() cross-alphabet recompute set; col_alphabet={} arg1_alphabet={}",
								static_cast<unsigned>(unpacked_typmod.alphabet_code),
								static_cast<unsigned>(unpacked_arg1_typmod.alphabet_code));
					}
				}
			}
			}
		}
		else if (arg1_is_param)
		{
			/*
		 * Parameterized-query case. Stash the Param coordinates for the
		 * shared evaluator. PARAM_EXTERN is available and installed now;
		 * PARAM_EXEC remains inactive until its Nested Loop parent installs
		 * the value and calls ReScan.
			 */
			Param* query_param = reinterpret_cast<Param*>(arg1);
			runtime.param_rebind.active            = true;
			runtime.param_rebind.paramid           = query_param->paramid;
			runtime.param_rebind.paramkind         = static_cast<int>(query_param->paramkind);
			runtime.param_rebind.param_typoid      = query_param->paramtype;
			runtime.param_rebind.param_typmod      = query_param->paramtypmod;
			runtime.param_rebind.seq_attno         = col_var->varattno;
			runtime.param_rebind.column_name       = std::string(attname_cstr);
			runtime.param_rebind.alphabet_code     = unpacked_typmod.alphabet_code;
			runtime.param_rebind.storage_unit_byte_width        = unpacked_typmod.storage_unit_byte_width;
			runtime.param_rebind.primary_key_attno = primary_key_attno;
			runtime.param_rebind.mmsearch_fe       = fe;
			if (query_param->paramkind == PARAM_EXTERN) {
				(void)resolve_mmsearch_param_query(
					runtime, cscanstate->ss.ps.ps_ExprContext,
					"BeginCustomScan");
			}
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() registering param-driven mmsearch rebind paramid={} paramkind={} typmod=({},{})",
					query_param->paramid, static_cast<int>(query_param->paramkind),
					static_cast<unsigned>(unpacked_typmod.alphabet_code),
					static_cast<unsigned>(unpacked_typmod.storage_unit_byte_width));
		}
		else /* arg1_is_var_same_rel */
		{
			/*
			 * Per-row alignment — each row contributes its own
			 * arg1 column value. Same-Var (varattno equal) is the
			 * `mmsearch(t.seq, t.seq)` self-alignment shape;
			 * different varattno is `mmsearch(t.seq, t.seq2)`
			 * sibling-column shape. Both go through the same
			 * dispatch path.
			 */
			Var* query_var = reinterpret_cast<Var*>(arg1);
			const bool self_alignment =
				query_var->varattno == col_var->varattno;
			runtime.buffer.configure_mmsearch_per_row(
				col_var->varattno,
				std::string(attname_cstr),
				query_var->varattno,
				self_alignment,
				unpacked_typmod.alphabet_code,
				unpacked_typmod.storage_unit_byte_width,
				primary_key_attno);
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() registering per-row mmsearch col_attno={} arg1_attno={} self_alignment={}",
					static_cast<int>(col_var->varattno),
					static_cast<int>(query_var->varattno),
					self_alignment);
		}

		primary_configured = true;
		++consumed_mmsearch_qual_count;
		primary_mmsearch_fe = fe;
		primary_seq_attno = col_var->varattno;
		runtime.match_sequence_varno = col_var->varno;
		runtime.match_sequence_attno = col_var->varattno;

		pfree(attname_cstr);
		/*
		 * Don't break — subsequent loop iterations that also match
		 * an mmsearch FuncExpr will fall into the
		 * `primary_configured` branch above and stack as additional
		 * AND filters.
		 */
	}
	if (consumed_mmsearch_qual_count != top_level_mmsearch_qual_count
		&& (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("mmsearch qualifier shape is unsupported"),
			 errdetail("the alignment runtime consumed %llu of %llu top-level mmsearch qualifiers; executing any remainder through the SQL stub is forbidden",
				static_cast<unsigned long long>(consumed_mmsearch_qual_count),
				static_cast<unsigned long long>(top_level_mmsearch_qual_count)),
				 errhint("Use a bioseq.seq column as the first argument and a non-null Const, PARAM_EXEC, PARAM_EXTERN, or same-relation bioseq.seq Var as the second argument.")));
	}
	if (runtime.edit_predicate_active
		&& (primary_seq_attno == InvalidAttrNumber
			|| runtime.edit_sequence_attno != primary_seq_attno)) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq.edit predicate must reference the mmsearch target column"),
			 errdetail("bioseq.edit attribute %d does not match mmsearch target attribute %d",
				 static_cast<int>(runtime.edit_sequence_attno),
				 static_cast<int>(primary_seq_attno))));
	}

	/*
	 * Build the per-scan Minimap2Params: start from a GUC snapshot, overlay
	 * any aligner_options() overrides the user passed to mmsearch, then
	 * flip conditional-pipeline flags based on which bioseq.* projections
	 * the target list actually references.
	 *
	 * The resulting struct is pushed into the TupleDispatchBuffer so every
	 * batch row inherits the same parameters, and copied into the per-scan
	 * runtime.aligner_state so EXPLAIN can reproduce the user-visible
	 * decision tree without re-walking the plan.
	 */
	BioseqScanFeatureFlags features = detect_feature_flags(
		cscan->scan.plan.targetlist, cscan->scan.plan.qual, scan_stub_oids);
	features.has_mmsearch = owned_mmsearch_quals != NIL;
	features.uses_edit = features.uses_edit || runtime.edit_predicate_active;
	features.any_match_projection =
		features.any_match_projection || runtime.edit_predicate_active;
	if (features.uses_edit && !features.has_mmsearch) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq.edit requires mmsearch in the same scan")));
	}
	if (features.has_mmsearch && IsParallelWorker()) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq alignment custom scans are parallel unsafe"),
			 errdetail("statement execution attestation is backend-local and DSM aggregation is not implemented")));
	}
	bioseq::simd::active_target::Minimap2Params aligner_params = build_params_from_gucs();
	bioseq::indexing::IndexReadRequirement primary_requirement =
		bioseq::indexing::IndexReadRequirement::allow;
	bool primary_vulkan_enabled_explicit = false;
	(void)apply_aligner_options_override(
		primary_mmsearch_fe,
		aligner_params,
		primary_requirement,
		scan_stub_oids,
		&primary_vulkan_enabled_explicit);
	finalize_simd_chaining_policy(aligner_params);
	const bioseq::indexing::IndexReadRequirement index_read_requirement =
		primary_requirement;
	aligner_params.do_cigar           = features.uses_cigar || features.uses_edit;
	aligner_params.compute_divergence = features.uses_divergence;
	aligner_params.compute_mapq       = features.uses_mapq;
	aligner_params.compute_edit_distance = features.uses_edit;
	aligner_params.exact_edit_filter_enabled =
		runtime.edit_predicate_active;
	aligner_params.exact_edit_filter_exclusive_upper_bound =
		runtime.edit_exclusive_upper_bound;
	if (runtime.edit_predicate_active
		&& runtime.edit_exclusive_upper_bound > 0
		&& static_cast<std::uint64_t>(runtime.edit_exclusive_upper_bound - 1)
			<= (std::numeric_limits<std::uint32_t>::max)()) {
		aligner_params.prefilter_enabled = true;
		aligner_params.prefilter_max_edits = static_cast<std::uint32_t>(
			runtime.edit_exclusive_upper_bound - 1);
		const bool existing_vulkan_route =
			aligner_params.chaining_algorithm
				== bioseq::simd::active_target::ChainingAlgorithmType::VULKAN_TILED_DP
			|| aligner_params.minimizer_algorithm
				== bioseq::simd::active_target::MinimizerAlgorithmType::VULKAN_MINIMAP_INT
			|| aligner_params.vulkan_seed_hits_enabled
			|| aligner_params.vulkan_fused_frontend_enabled
			|| aligner_params.vulkan_gpu_index_build_enabled
			|| aligner_params.vulkan_extend_enabled;
		const bool standalone_vulkan_prefilter_requested =
			primary_vulkan_enabled_explicit && aligner_params.vulkan_enabled;
		aligner_params.prefilter_backend =
			(existing_vulkan_route || standalone_vulkan_prefilter_requested)
			? bioseq::prefilter_stats::PrefilterBackendType::VULKAN
			: (bioseq_enable_vectorization
				? bioseq::prefilter_stats::PrefilterBackendType::SIMD
				: bioseq::prefilter_stats::PrefilterBackendType::SCALAR);
	}
	const bool selected_vulkan =
		bioseq::simd::active_target::pipe::vulkan_components_requested(
			aligner_params);
	const bool selected_vulkan_accelerator =
		bioseq::simd::active_target::pipe::vulkan_accelerator_requested(
			aligner_params);
	const bool executable_scan =
		(eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0;
	/*
	 * C1 is deliberately fail-closed: the only currently readable persisted
	 * MPF/MPS layouts predate the revision-3 READY head+manifest identity.
	 * They cannot satisfy ordinal 2, and rebuilding pairs or trusting a warm
	 * assembled/GPU cache would counterfeit a persisted-compatible read.
	 * Reject executable scans here before Vulkan initialization and retain the
	 * typed-processor check as defence in depth for non-standard callers. Plain
	 * EXPLAIN keeps the ordinal visible without initializing either payload.
	 */
	if (index_read_requirement
		== bioseq::indexing::IndexReadRequirement::
			require_compatible_persisted
		&& executable_scan) {
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("required compatible persisted minimizer index unavailable"),
			 errdetail("ordinal 2 requires a revision-3 READY generation retained by the BatchJoin prepared-reader lifecycle; ordinary column scans have no compatible persisted reader"),
			 errhint("Use BioseqBatchJoin with a compatible READY generation, or choose index_read_requirement=allow/require_absent; recompute and cache fallback are forbidden.")));
	}
	/* Plain EXPLAIN must describe the selected pipeline without touching the
	 * process-owned Vulkan runtime or its immutable Metal policy.  PostgreSQL
	 * clears EXEC_FLAG_EXPLAIN_ONLY for SELECT and EXPLAIN ANALYZE, so every
	 * executable path retains the fail-closed enablement, cache and device
	 * checks below. */
	const bool executable_vulkan =
		features.has_mmsearch
		&& selected_vulkan_accelerator
		&& executable_scan;
	if (executable_vulkan && !aligner_params.vulkan_enabled) {
		raise_column_scan_vulkan_error_as_cpp(
			"selected Vulkan execution requires "
			"bioseq.aligner.vulkan_enabled=on",
			"Set bioseq.aligner.vulkan_enabled=on, remove the per-call "
			"standalone Vulkan prefilter request, or select non-Vulkan "
			"minimizer/chaining algorithms before dispatch.");
	}
	if (executable_vulkan && aligner_params.vulkan_enabled) {
		auto& vulkan_runtime =
			bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
		if (!vulkan_runtime.usable()) {
			const std::string diagnostic = fmt::format(
				"selected Vulkan execution requires a usable Vulkan runtime; "
				"initialization_error={}",
				vulkan_runtime.initialization_error());
			raise_column_scan_vulkan_error_as_cpp(
				diagnostic,
				"Install a working Vulkan ICD, remove or set false the per-call "
				"aligner_options(vulkan_enabled) standalone prefilter request, "
				"or select non-Vulkan minimizer/chaining algorithms before dispatch.");
		}
		if (vulkan_runtime.features().software_device) {
			const std::string diagnostic = fmt::format(
				"selected Vulkan execution requires a hardware GPU; active "
				"Vulkan device '{}' is a software/CPU Vulkan device "
				"(VkPhysicalDeviceType={})",
				vulkan_runtime.features().device_name.data(),
				vulkan_runtime.features().device_type);
			raise_column_scan_vulkan_error_as_cpp(
				diagnostic,
				"Use a hardware Vulkan ICD, remove or set false the per-call "
				"aligner_options(vulkan_enabled) standalone prefilter request, "
				"or select non-Vulkan minimizer/chaining algorithms before dispatch.");
		}
	}

	runtime.aligner_state.params          = aligner_params;
	runtime.aligner_state.features        = features;
	runtime.aligner_state.params_captured = true;

	/*
	 * Capture the structured pipeline description for EXPLAIN.  Walks the
	 * same dispatch ladder `dispatch_pipeline` would but invokes each stage's
	 * `describe()` instead of `on_batch(...)`.  The proto query / target
	 * sequence ranges are empty placeholders — every stage's `describe()`
	 * consults only by-value config fields (DOC0249 §6.1 invariant) so the
	 * empty input is safe.
	 */
	if (features.has_mmsearch) {
		using QR_default = sequence_custom_alphabet<seq::dna4, uint64_t>;
		using TR_default = sequence_custom_alphabet<seq::dna4, uint64_t>;
		compact_sequence_range<QR_default> empty_query_proto{};
		compact_sequence_range<TR_default> empty_target_proto{};
		auto empty_query_owner =
			bioseq::storage::make_lazy_in_memory(empty_query_proto);
		auto empty_target_owner =
			bioseq::storage::make_lazy_in_memory(empty_target_proto);
		const auto empty_query_view =
			bioseq::storage::as_lazy_sequence_view(empty_query_owner);
		const auto empty_target_view =
			bioseq::storage::as_lazy_sequence_view(empty_target_owner);
		runtime.aligner_state.pipeline_desc =
			bioseq::simd::active_target::pipe::describe_pipeline_for(
				aligner_params, empty_query_view, empty_target_view);
	}

	/*
	 * Only consult the sidecar when mmsearch is actually present. Without
	 * mmsearch there is no BatchProcessor call so opening the sidecar
	 * relation would be wasted work (and would surface a confusing
	 * "Recomputed" line in EXPLAIN on a query that doesn't use any index).
	 */
	const Oid resolved_user_relid =
		cscanstate->ss.ss_currentRelation != nullptr
			? RelationGetRelid(cscanstate->ss.ss_currentRelation)
			: InvalidOid;
	/*
	 * DOC0260 §9.4 — pass the parent's three-part physical identity
	 * (database, tablespace, relfilenode) so the dispatch buffer can route
	 * its per-bucket SM::fetch_payload_bulk calls to the right sub-tables.
	 */
	Oid scan_db_oid  = InvalidOid;
	Oid scan_spc_oid = InvalidOid;
	Oid scan_relfile = InvalidOid;
	if (cscanstate->ss.ss_currentRelation != nullptr) {
		scan_db_oid  = cscanstate->ss.ss_currentRelation->rd_locator.dbOid;
		scan_spc_oid = cscanstate->ss.ss_currentRelation->rd_locator.spcOid;
		scan_relfile = cscanstate->ss.ss_currentRelation->rd_locator.relNumber;
	}
	runtime.buffer.configure_params(aligner_params,
							index_read_requirement,
							features.has_mmsearch && bioseq_read_sequence_index,
							resolved_user_relid,
							scan_db_oid, scan_spc_oid, scan_relfile,
							cscanstate->ss.ss_currentRelation,
							bioseqScanState->snapshot);
	if (features.has_mmsearch) {
		const int plan_node_id = cscan->scan.plan.plan_node_id;
		if (plan_node_id < 0) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq alignment scan has an invalid plan node id")));
		}
		runtime.alignment_registration =
			bioseq::pg::alignment_execution::bind_alignment_node(
				estate, static_cast<std::uint32_t>(plan_node_id));
		runtime.buffer.bind_persisted_gpu_operation_evidence_sink(
			*bioseq::pg::alignment_execution::
				borrow_persisted_gpu_operation_evidence_sink(
					runtime.alignment_registration));
	}

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() scan initialised successfully");
	/* scan slot is already initialized */
}

static void
BioseqColumnScan_BeginCustomScan(
		CustomScanState* cscanstate,
		EState* estate,
		int eflags)
{
	ErrorData* exact_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char detail[512] = "std::exception without detail";
	try {
		BioseqColumnScan_BeginCustomScanImpl(cscanstate, estate, eflags);
		return;
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			detail,
			error.what() == nullptr ? "std::exception without detail" : error.what(),
			sizeof(detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}

	BioseqScanState* state =
		BioseqScanState::from_custom_scan_state(cscanstate);
	if (state != nullptr && state->runtime != nullptr) {
		auto* runtime = static_cast<BioseqScanRuntime*>(state->runtime);
		runtime->buffer.abandon_join_runtime_after_pg_error_noexcept();
	}
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqColumnScan Begin raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqColumnScan Begin raised an unknown C++ exception")));
	}
}

/*
 * ExecScanFetch -- check interrupts & fetch next potential tuple
 *
 * This routine is concerned with substituting a test tuple if we are
 * inside an EvalPlanQual recheck.  If we aren't, just execute
 * the access method's next-tuple routine.
 */
static inline TupleTableSlot *
ExecScanFetch(ScanState *node,
			  ExecScanAccessMtd accessMtd,
			  ExecScanRecheckMtd recheckMtd)
{
	bioseq_log<DBG>("[bioseq] pg: ExecScanFetch() entered");
	EState	   *estate = node->ps.state;

	CHECK_FOR_INTERRUPTS();

	if (estate->es_epq_active != NULL)
	{
		EPQState   *epqstate = estate->es_epq_active;

		/*
		 * We are inside an EvalPlanQual recheck.  Return the test tuple if
		 * one is available, after rechecking any access-method-specific
		 * conditions.
		 */
		Index		scanrelid = reinterpret_cast<Scan *>(node->ps.plan)->scanrelid;

		if (scanrelid == 0)
		{
			/*
			 * This is a ForeignScan or CustomScan which has pushed down a
			 * join to the remote side.  The recheck method is responsible not
			 * only for rechecking the scan/join quals but also for storing
			 * the correct tuple in the slot.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			if (!(*recheckMtd) (node, slot))
				ExecClearTuple(slot);	/* would not be returned by scan */
			return slot;
		}
		else if (epqstate->relsubs_done[scanrelid - 1])
		{
			/*
			 * Return empty slot, as we already performed an EPQ substitution
			 * for this relation.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Return empty slot, as we already returned a tuple */
			return ExecClearTuple(slot);
		}
		else if (epqstate->relsubs_slot[scanrelid - 1] != NULL)
		{
			/*
			 * Return replacement tuple provided by the EPQ caller.
			 */

			TupleTableSlot *slot = epqstate->relsubs_slot[scanrelid - 1];

			Assert(epqstate->relsubs_rowmark[scanrelid - 1] == NULL);

			/* Mark to remember that we shouldn't return more */
			epqstate->relsubs_done[scanrelid - 1] = true;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
		else if (epqstate->relsubs_rowmark[scanrelid - 1] != NULL)
		{
			/*
			 * Fetch and return replacement tuple using a non-locking rowmark.
			 */

			TupleTableSlot *slot = node->ss_ScanTupleSlot;

			/* Mark to remember that we shouldn't return more */
			epqstate->relsubs_done[scanrelid - 1] = true;

			if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
				return NULL;

			/* Return empty slot if we haven't got a test tuple */
			if (TupIsNull(slot))
				return NULL;

			/* Check if it meets the access-method conditions */
			if (!(*recheckMtd) (node, slot))
				return ExecClearTuple(slot);	/* would not be returned by
												 * scan */
			return slot;
		}
	}

	/*
	 * Run the node-type-specific access method function to get the next tuple
	 */
	return (*accessMtd) (node);
}

/*
 * Dispatch table: given a matched funcid, which placeholder Const do we
 * substitute in? bioseq.score is an alias for bioseq.dp_score so both
 * resolve to the same Const* (paradedb's score/pdb.score alias pattern).
 */
static Const*
lookup_bioseq_placeholder_const(Oid funcid, const VarInjectorCtx* ctx)
{
	const BioseqStubOids* o = ctx->oids;
	const BioseqPlaceholderConsts* c = ctx->consts;
	if (funcid == o->score_oid || funcid == o->dp_score_oid)     return c->dp_score_node;
	if (funcid == o->query_len_oid)              return c->query_len_node;
	if (funcid == o->query_start_oid)            return c->query_start_node;
	if (funcid == o->query_end_oid)              return c->query_end_node;
	if (funcid == o->target_len_oid)             return c->target_len_node;
	if (funcid == o->target_start_oid)           return c->target_start_node;
	if (funcid == o->target_end_oid)             return c->target_end_node;
	if (funcid == o->num_matches_oid)            return c->num_matches_node;
	if (funcid == o->block_length_oid)           return c->block_length_node;
	if (funcid == o->edit_oid)                   return c->edit_distance_node;
	if (funcid == o->mapq_oid)                   return c->mapq_node;
	if (funcid == o->chain_score_oid)            return c->chain_score_node;
	if (funcid == o->dp_max_score_oid)           return c->dp_max_score_node;
	if (funcid == o->seed_count_oid)             return c->seed_count_node;
	if (funcid == o->secondary_chain_score_oid)  return c->secondary_chain_score_node;
	if (funcid == o->is_reverse_oid)             return c->is_reverse_node;
	if (funcid == o->is_primary_oid)             return c->is_primary_node;
	if (funcid == o->has_cigar_oid)              return c->has_cigar_node;
	if (funcid == o->divergence_oid)             return c->divergence_node;
	if (funcid == o->cigar_oid)                  return c->cigar_node;
	return nullptr;
}

/*
 * Rewrite a single `bioseq.symbol_count(Var, 'L')` call site into a
 * placeholder Const that the executor populates per-row from meta_sidecar.
 * Returns the replacement Const*, or nullptr when the FuncExpr does not
 * match the supported shape (in which case the caller must leave the
 * original FuncExpr in place so the scalar O(n) path runs).
 *
 * Matching is deliberately strict: arg0 must be a Var (a column reference
 * with a concrete bioseq.seq typmod), and arg1 must be a single-byte
 * non-null text Const whose letter is in the column's alphabet. Any call
 * outside this shape — e.g. symbol_count('ACGT'::bioseq.seq, 'A') — keeps
 * its original FuncExpr and runs through the scalar entry point in
 * mod.cppm, which is cost-inflated by bioseq_symbol_count_support.
 */
static Const*
try_rewrite_symbol_count(FuncExpr* fe, VarInjectorCtx* ctx)
{
	if (ctx == nullptr || fe == nullptr || ctx->consts == nullptr) {
		return nullptr;
	}
	if (list_length(fe->args) != 2) {
		return nullptr;
	}
	PList<Node> fe_args(fe->args);
	Node* arg0 = fe_args.first();
	Node* arg1 = fe_args.second();
	if (arg0 == nullptr || arg1 == nullptr) {
		return nullptr;
	}
	Var* col_var = pg_node_try_as<Var>(arg0);
	Const* letter_const = pg_node_try_as<Const>(arg1);
	if (col_var == nullptr || letter_const == nullptr) {
		bioseq_log<DBG>("[bioseq] pg: var_injector_mutator() leaving symbol_count(...) alone; arg0_tag={} arg1_tag={}",
				static_cast<int>(nodeTag(arg0)),
				static_cast<int>(nodeTag(arg1)));
		return nullptr;
	}
	if (col_var->vartypmod < 0 || col_var->vartype != ctx->oids->bioseq_seq_type_oid) {
		bioseq_log<DBG>("[bioseq] pg: VarInjectorMutator() symbol_count arg0 has wrong type/typmod (type={}, typmod={})",
				col_var->vartype, col_var->vartypmod);
		return nullptr;
	}
	if (letter_const->constisnull) {
		bioseq_log<DBG>("[bioseq] pg: VarInjectorMutator() symbol_count arg1 is NULL; skipping");
		return nullptr;
	}

	text* letter_text = reinterpret_cast<text*>(DatumGetPointer(letter_const->constvalue));
	if (letter_text == nullptr) {
		return nullptr;
	}
	text* letter_detoasted = reinterpret_cast<text*>(PG_DETOAST_DATUM(letter_const->constvalue));
	const int32 letter_size = VARSIZE_ANY_EXHDR(letter_detoasted);
	if (letter_size != 1) {
		bioseq_log<DBG>("[bioseq] pg: VarInjectorMutator() symbol_count arg1 size={} (expected 1)", letter_size);
		if (reinterpret_cast<void*>(letter_detoasted)
			!= reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
			pfree(letter_detoasted);
		}
		return nullptr;
	}
	const char letter_char = *VARDATA_ANY(letter_detoasted);
	if (reinterpret_cast<void*>(letter_detoasted)
		!= reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
		pfree(letter_detoasted);
	}

	const compact_sequence_stdc_type_params typmod = compact_sequence_stdc_type_params::decode(col_var->vartypmod);
	const int rank = bioseq::storage::indexing::letter_to_rank(typmod.alphabet_code, letter_char);
	if (rank < 0) {
		bioseq_log<DBG>("[bioseq] pg: VarInjectorMutator() symbol_count letter '{}' not in alphabet_code={}; keeping scalar call",
				letter_char, static_cast<unsigned>(typmod.alphabet_code));
		return nullptr;
	}

	/*
	 * Build the placeholder Const. INT8 is byval on LP64 (pg_type.typbyval
	 * for bigint is 't'), so constbyval=true + constlen=8 is correct. The
	 * initial value is NULL until the executor populates it per-row.
	 */
		Const* placeholder = makeConst(
			INT8OID,
			-1,
			InvalidOid,
			8,
			bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(0)).raw(),
			/*constisnull=*/ true,
			/*constbyval=*/ true);

	SymbolCountCallSite site;
	site.placeholder = placeholder;
	site.rank        = rank;
	site.letter      = letter_char;
	site.column_attno = static_cast<int16_t>(col_var->varattno);  // DOC0260 §11.1
	if (ctx->current_relation != nullptr
		&& col_var->varattno >= 1) {
		char* attname_cstr = get_attname(
			RelationGetRelid(ctx->current_relation), col_var->varattno, /*missing_ok=*/true);
		if (attname_cstr != nullptr) {
			site.column_name = std::string(attname_cstr);
			pfree(attname_cstr);
		}
	}
	bioseq_log<DBG>("[bioseq] pg: VarInjectorMutator() installing symbol_count placeholder column='{}' letter='{}' rank={}",
			site.column_name, letter_char, rank);

	ctx->consts->symbol_count_sites.push_back(std::move(site));
	return placeholder;
}

struct var_injector_mutator {
	[[nodiscard]] pg_node_mutation
	operator()(pg_expression_mutation_cursor, pg_node_view node, VarInjectorCtx &injector_context) const {
		if (FuncExpr* funcExpr = node.try_as<FuncExpr>()) {
			if (!edit_sequence_argument_matches_target(
					funcExpr,
					injector_context.match_sequence_varno,
					injector_context.match_sequence_attno,
					*injector_context.oids)) {
				ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("bioseq.edit projection must reference the mmsearch target column")));
			}
			Const* replacement = lookup_bioseq_placeholder_const(funcExpr->funcid, &injector_context);
			if (replacement != nullptr) {
				bioseq_log<DBG>("[bioseq] pg: var_injector_mutator() replacing bioseq funcid={} with placeholder Const",
						funcExpr->funcid);
				return pg_node_mutation::with(pg_as_node(replacement));
			}
			if (funcExpr->funcid == injector_context.oids->symbol_count_oid) {
				Const* symbol_placeholder =
					try_rewrite_symbol_count(funcExpr, &injector_context);
				if (symbol_placeholder != nullptr) {
					return pg_node_mutation::with(pg_as_node(symbol_placeholder));
				}
				/* Call shape not supported (literal arg, non-Const letter,
				 * letter outside alphabet, …) — let the scalar entry point
				 * in mod.cppm handle it in place. */
			}
			/* Fall through to recurse into arg subtrees of unrelated FuncExprs. */
		}
		/*
		 * PlaceHolderVar handling. By Postgres convention the planner
		 * resolves PHVs to Vars / Params by execution time (pathnodes.h:
		 * "it is not recognized by the parser or executor"), so seeing
		 * one here is a planner bug. We log loudly and pass the node
		 * through — unwrapping would silently drop the PHV's
		 * phnullingrels metadata and could produce wrong results in
		 * outer-join nullable positions (a plain Const has no nulling
		 * semantics). Returning descend() does the right thing because
		 * the cursor recurses into phexpr, giving var_injector_mutator a
		 * chance to rewrite our FuncExpr in place; the PHV node itself is
		 * preserved.
		 */
		if (PlaceHolderVar* phv = node.try_as<PlaceHolderVar>()) {
			bioseq_log<DBG>("[bioseq] pg: var_injector_mutator() encountered PlaceHolderVar phid={} at exec time — passing through",
					phv->phid);
			/* fall through to descend */
		}
		return pg_node_mutation::descend();
	}
};

/*
 * Thin wrapper for the direct callers (execution-time targetlist rewrite)
 * so they keep the (node, ctx) call shape.
 */
[[nodiscard]] static Node *
var_injector(Node *node, VarInjectorCtx *injector_context)
{
	var_injector_mutator mutator;
	return pg_mutate_expression(node, *injector_context, mutator);
}

/*
 * Per-tuple update of every placeholder Const* from a MatchResult's
 * fields. Fields we don't have a Const for (because the query didn't
 * project them) are skipped thanks to the nullptr guard. The cigar text
 * Datum is allocated via CStringGetTextDatum in CurrentMemoryContext;
 * for the POC we accept the per-query leak (see SPEC_HOW_TO_CONNECT_ALIGNER_2.md §6.2).
 *
 * MatchResult and format_cigar live inside bioseq::simd::HWY_NAMESPACE
 * (expanded to the current Highway target namespace); this file does not use
 * HWY_BEFORE_NAMESPACE() so we qualify with the full path.
 */
static void
populate_placeholder_consts(BioseqPlaceholderConsts& consts,
							const bioseq::simd::active_target::MatchResult& match)
{
	using bioseq::simd::active_target::format_cigar;
	// DOC0274 §1.8 — MatchResult absolute coordinates / lengths are
	// `sequence_size_t` (uint64; minimap_port.cppm:334). They are projected
	// through BIGINT (int8) placeholders so positions / lengths in sequences
	// longer than `INT32_MAX` (~2.15 Gbp) are NOT truncated. `set_bigint`
	// fills those eight (query_len … block_length) via `Int64GetDatum`. The
	// remaining MatchResult fields (dp_score, mapq, chain_score, seed_count,
	// …) are genuinely `int32_t` in the struct, so they keep INT4 placeholders
	// and `set_int` — widening them at this boundary cannot recover precision
	// the source type never held.
	//
	// `set_bigint`'s Assert is a debug-time witness that no real coordinate
	// reaches the int64 sign boundary: the project's pessimistic ceiling is
	// 1 Tbp (~2^40), ~23 bits below INT64_MAX, so the uint64→int64 reinterpret
	// is always value-preserving.
	auto set_int = [](Const* node, int64_t value) {
			if (node == nullptr) return;
			Assert(value >= INT32_MIN && value <= INT32_MAX);
			node->constvalue = bioseq::pg_utils::pg_datum::from(static_cast<std::int32_t>(value)).raw();
			node->constisnull = false;
		};
	// Parameter is the underlying type of `sequence_size_t` (std::uint64_t)
	// spelled directly — this TU does not import bioseq_storage_size, so the
	// `sequence_size_t` alias is not in scope here.
	auto set_bigint = [](Const* node, std::uint64_t value) {
			if (node == nullptr) return;
			Assert(value <= static_cast<std::uint64_t>(INT64_MAX));
			node->constvalue = bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(value)).raw();
			node->constisnull = false;
		};
		auto set_bool = [](Const* node, bool value) {
			if (node == nullptr) return;
			node->constvalue = bioseq::pg_utils::pg_datum::from(value).raw();
			node->constisnull = false;
		};
		auto set_float = [](Const* node, float value) {
			if (node == nullptr) return;
			node->constvalue = bioseq::pg_utils::pg_datum::from(value).raw();
			node->constisnull = false;
		};

	set_int(consts.dp_score_node,              match.dp_score);
	// The eight sequence_size_t (uint64) coordinates/lengths → BIGINT, no narrow.
	set_bigint(consts.query_len_node,          match.query_len);
	set_bigint(consts.query_start_node,        match.query_start);
	set_bigint(consts.query_end_node,          match.query_end);
	set_bigint(consts.target_len_node,         match.target_len);
	set_bigint(consts.target_start_node,       match.target_start);
	set_bigint(consts.target_end_node,         match.target_end);
	set_bigint(consts.num_matches_node,        match.num_matches);
	set_bigint(consts.block_length_node,       match.block_length);
	set_bigint(consts.edit_distance_node,      match.edit_distance);
	set_int(consts.mapq_node,                  match.mapq);
	set_int(consts.chain_score_node,           match.chain_score);
	set_int(consts.dp_max_score_node,          match.dp_max_score);
	set_int(consts.seed_count_node,            match.seed_count);
	set_int(consts.secondary_chain_score_node, match.secondary_chain_score);
	set_bool(consts.is_reverse_node,           match.is_reverse);
	set_bool(consts.is_primary_node,           match.is_primary);
	set_bool(consts.has_cigar_node,            match.has_cigar);
	set_float(consts.divergence_node,          match.divergence);

	if (consts.cigar_node != nullptr)
		{
			std::string cigar_str = format_cigar(match.cigar);
			consts.cigar_node->constvalue =
				bioseq::pg_utils::pg_datum::from_text_cstring(cigar_str.c_str()).raw();
			consts.cigar_node->constisnull = false;
			}
}

[[nodiscard]] static BioseqBatchJoinRuntime* batch_join_runtime(
		CustomScanState* node) noexcept {
	auto* state = reinterpret_cast<BioseqBatchJoinState*>(node);
	return state == nullptr
		? nullptr
		: static_cast<BioseqBatchJoinRuntime*>(state->runtime);
}

static void batch_join_release_executor_resources_checked(
	CustomScanState* node,
	BioseqBatchJoinRuntime& runtime,
	bool delete_query_context);

static void batch_join_abandon_executor_resources_after_error(
	CustomScanState* node,
	BioseqBatchJoinRuntime& runtime) noexcept;

static void batch_join_runtime_cleanup(void* argument) {
	auto* state = static_cast<BioseqBatchJoinState*>(argument);
	if (state == nullptr || state->runtime == nullptr) {
		return;
	}
	auto* runtime = static_cast<BioseqBatchJoinRuntime*>(state->runtime);
	batch_join_abandon_executor_resources_after_error(
		&state->custom_scanstate, *runtime);
	state->runtime = nullptr;
	std::destroy_at(runtime);
}

static void batch_join_clear_query_tuples(
		BioseqBatchJoinRuntime& runtime,
		const bool drop_slots) {
	if (drop_slots) {
		for (BioseqBatchJoinQueryTuple& query : runtime.query_tuples) {
			if (query.slot != nullptr) {
				ExecDropSingleTupleTableSlot(query.slot);
				query.slot = nullptr;
			}
		}
	}
	runtime.query_tuples.clear();
	runtime.all_query_refs.clear();
	if (runtime.query_tuple_context != nullptr) {
		MemoryContextReset(runtime.query_tuple_context);
	}
	/* The permit covers allocations owned by query_tuple_context (plus the
	 * MinimalTuple slots above).  Release only after those owners are gone. */
	runtime.query_permits.clear();
	runtime.query_reserved_admission_bytes = 0;
	runtime.query_actual_materialized_bytes = 0;
	runtime.queries_materialized = false;
}

/* A child PlanState may legally return a pass-through scan/child slot rather
 * than its ps_ResultTupleSlot.  Validate the slot that ExecProcNode actually
 * returned before reading or copying it; the post-setrefs Plan targetlist is
 * the planning ABI, while this is the independent executor ABI. */
static void batch_join_validate_returned_child_slot(
		TupleTableSlot* slot,
		const int expected_columns,
		const AttrNumber sequence_resno,
		const AttrNumber rowno_resno,
		const std::uint8_t alphabet_code,
		const std::uint8_t storage_unit_byte_width,
		const char* role) {
	TupleDesc descriptor = slot == nullptr
		? nullptr : slot->tts_tupleDescriptor;
	const bool rowno_required = rowno_resno != InvalidAttrNumber;
	if (descriptor == nullptr || expected_columns <= 0
		|| descriptor->natts != expected_columns
		|| sequence_resno <= 0 || sequence_resno > expected_columns
		|| (rowno_required
			&& (rowno_resno <= 0 || rowno_resno > expected_columns))) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin returned %s slot layout is invalid", role),
			 errdetail(
				"expected_columns=%d actual_columns=%d sequence_resno=%d rowno_resno=%d",
				expected_columns,
				descriptor == nullptr ? -1 : descriptor->natts,
				sequence_resno,
				rowno_resno)));
	}
	const Oid sequence_type = get_cached_stub_oids().bioseq_seq_type_oid;
	const int32 expected_typmod = compact_sequence_stdc_type_params{
		alphabet_code, storage_unit_byte_width}.encode();
	Form_pg_attribute sequence_attribute = TupleDescAttr(
		descriptor, sequence_resno - 1);
	Form_pg_attribute rowno_attribute = rowno_required
		? TupleDescAttr(descriptor, rowno_resno - 1) : nullptr;
	if (sequence_attribute->atttypid != sequence_type
		|| sequence_attribute->atttypmod != expected_typmod
		|| sequence_attribute->attcollation != get_typcollation(sequence_type)
		|| (rowno_required
			&& (rowno_attribute->atttypid != INT8OID
				|| rowno_attribute->atttypmod != -1
				|| OidIsValid(rowno_attribute->attcollation)))) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqBatchJoin returned %s slot types are invalid", role),
			 errdetail(
				"sequence_type=%u sequence_typmod=%d sequence_collation=%u rowno_type=%u rowno_typmod=%d rowno_collation=%u",
				sequence_attribute->atttypid,
				sequence_attribute->atttypmod,
				sequence_attribute->attcollation,
				rowno_attribute == nullptr
					? InvalidOid : rowno_attribute->atttypid,
				rowno_attribute == nullptr
					? -1 : rowno_attribute->atttypmod,
				rowno_attribute == nullptr
					? InvalidOid : rowno_attribute->attcollation)));
	}
}

static void batch_join_materialize_queries(
		BioseqBatchJoinRuntime& runtime) {
	if (runtime.queries_materialized) {
		return;
	}
	if (runtime.query_child == nullptr || runtime.query_tuple_context == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin query child is not initialized")));
	}
	if (!std::isfinite(runtime.query_planner_rows)
		|| runtime.query_planner_rows < 0.0
		|| runtime.query_planner_width < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqBatchJoin query planner estimate is invalid")));
	}
	/* Reserve the planner's rows*width envelope before asking the child for
	 * its first tuple.  Actual whole-MinimalTuple upper bounds below extend
	 * this reservation before detoast/copy whenever the estimate is low. */
	const double bounded_rows = std::min(
		runtime.query_planner_rows,
		static_cast<double>((std::numeric_limits<std::uint64_t>::max)()));
	const std::uint64_t planner_rows = static_cast<std::uint64_t>(
		std::ceil(bounded_rows));
	const std::uint64_t planner_tuple_bytes =
		bioseq::parallelism::saturating_add(
			static_cast<std::uint64_t>(runtime.query_planner_width),
			static_cast<std::uint64_t>(
				MAXALIGN(MINIMAL_TUPLE_DATA_OFFSET)
				+ sizeof(BioseqBatchJoinQueryTuple)
				+ sizeof(bioseq::scheduler::DispatchJoinQueryRef)
				+ 256u));
	runtime.query_planner_admission_bytes =
		bioseq::parallelism::saturating_mul(
			planner_rows, planner_tuple_bytes);
	auto planner_permit =
		bioseq::parallelism::MemoryAdmissionController::instance()
			.try_reserve(runtime.query_planner_admission_bytes);
	if (!planner_permit.has_value()) {
		ereport(ERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("BioseqBatchJoin planned query relation cannot be admitted"),
			 errdetail("planned_rows=%.0f planned_width=%d required_bytes=%llu",
				runtime.query_planner_rows,
				runtime.query_planner_width,
				static_cast<unsigned long long>(
					runtime.query_planner_admission_bytes))));
	}
	runtime.query_reserved_admission_bytes =
		runtime.query_planner_admission_bytes;
	runtime.query_permits.push_back(std::move(*planner_permit));
	for (;;) {
		TupleTableSlot* source = ExecProcNode(runtime.query_child);
		if (TupIsNull(source)) {
			break;
		}
		batch_join_validate_returned_child_slot(
			source,
			runtime.query_column_count,
			runtime.query_slot_seq_resno,
			InvalidAttrNumber,
			runtime.alphabet_code,
			runtime.storage_unit_byte_width,
			"query");
		slot_getallattrs(source);
		bool query_is_null = source->tts_isnull[
			runtime.query_slot_seq_resno - 1];
		if (query_is_null) {
			/* Strict INNER predicate: a NULL query cannot produce a match. */
			continue;
		}
		const Datum original = source->tts_values[
			runtime.query_slot_seq_resno - 1];
		const std::uint64_t raw_query_bytes = static_cast<std::uint64_t>(
			toast_raw_datum_size(original));
		const Size source_tuple_data = heap_compute_data_size(
			source->tts_tupleDescriptor,
			source->tts_values,
			source->tts_isnull);
		std::uint64_t tuple_upper_bound =
			bioseq::parallelism::saturating_add(
				static_cast<std::uint64_t>(
					MAXALIGN(MINIMAL_TUPLE_DATA_OFFSET)),
				static_cast<std::uint64_t>(source_tuple_data));
		/* source_tuple_data includes the original (possibly external TOAST)
		 * sequence datum. Adding the raw size rather than subtracting that datum
		 * is deliberately conservative and covers the flat replacement without
		 * inspecting or allocating any pass-by-reference value. */
		tuple_upper_bound = bioseq::parallelism::saturating_add(
			tuple_upper_bound, raw_query_bytes);
		tuple_upper_bound = bioseq::parallelism::saturating_add(
			tuple_upper_bound,
			static_cast<std::uint64_t>(
				sizeof(BioseqBatchJoinQueryTuple)
				+ sizeof(bioseq::scheduler::DispatchJoinQueryRef)
				+ sizeof(TupleTableSlot)
				+ static_cast<std::size_t>(
					source->tts_tupleDescriptor->natts)
					* (sizeof(Datum) + sizeof(bool))));
		const std::uint64_t required_after_row =
			bioseq::parallelism::saturating_add(
				runtime.query_actual_materialized_bytes,
				tuple_upper_bound);
		if (required_after_row
			== (std::numeric_limits<std::uint64_t>::max)()) {
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BioseqBatchJoin query admission size overflow")));
		}
		if (required_after_row > runtime.query_reserved_admission_bytes) {
			const std::uint64_t additional_bytes = required_after_row
				- runtime.query_reserved_admission_bytes;
			auto additional_permit =
				bioseq::parallelism::MemoryAdmissionController::instance()
					.try_reserve(additional_bytes);
			if (!additional_permit.has_value()) {
				ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("BioseqBatchJoin query child cannot be admitted"),
					 errdetail("query_ordinal=%zu required_additional_bytes=%llu whole_tuple_upper_bound=%llu",
						runtime.query_tuples.size(),
						static_cast<unsigned long long>(additional_bytes),
						static_cast<unsigned long long>(tuple_upper_bound))));
			}
			runtime.query_permits.push_back(std::move(*additional_permit));
			runtime.query_reserved_admission_bytes = required_after_row;
		}
		/* Every following operation may allocate; the complete tuple upper
		 * bound is owned before the first detoast/copy. */
		pgseq_t* flat = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(original));
		if (bioseq::storage::is_locator_stub(flat)) {
			if (reinterpret_cast<void*>(flat)
				!= reinterpret_cast<void*>(DatumGetPointer(original))) {
				pfree(flat);
			}
				ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("BioseqBatchJoin query child did not materialize its sequence")));
		}
		if (flat->alphabet_code != runtime.alphabet_code
			|| flat->storage_unit_byte_width
				!= runtime.storage_unit_byte_width) {
			if (reinterpret_cast<void*>(flat)
				!= reinterpret_cast<void*>(DatumGetPointer(original))) {
				pfree(flat);
			}
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqBatchJoin query typmod changed after planning")));
		}
		source->tts_values[runtime.query_slot_seq_resno - 1] =
			PointerGetDatum(flat);
		MemoryContext old = MemoryContextSwitchTo(
			runtime.query_tuple_context);
		MinimalTuple copy = ExecCopySlotMinimalTuple(source);
		MemoryContextSwitchTo(old);
		if (reinterpret_cast<void*>(flat)
			!= reinterpret_cast<void*>(DatumGetPointer(original))) {
			pfree(flat);
			source->tts_values[runtime.query_slot_seq_resno - 1] = original;
		}
		if (copy == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("BioseqBatchJoin failed to copy a query tuple")));
		}
		const std::uint64_t actual_tuple_bytes =
			bioseq::parallelism::saturating_add(
				static_cast<std::uint64_t>(copy->t_len),
				static_cast<std::uint64_t>(
					sizeof(BioseqBatchJoinQueryTuple)
					+ sizeof(bioseq::scheduler::DispatchJoinQueryRef)
					+ sizeof(TupleTableSlot)));
		runtime.query_actual_materialized_bytes =
			bioseq::parallelism::saturating_add(
				runtime.query_actual_materialized_bytes,
				actual_tuple_bytes);
		if (runtime.query_actual_materialized_bytes
			> runtime.query_reserved_admission_bytes) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin whole-tuple admission under-estimated its allocation")));
		}
		const std::uint64_t ordinal = runtime.query_tuples.size();
		if (ordinal
			> static_cast<std::uint64_t>(
				(std::numeric_limits<std::int32_t>::max)())) {
			ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("BioseqBatchJoin query ordinal exceeds int32 dispatch ABI")));
		}
		TupleTableSlot* stable_slot = MakeSingleTupleTableSlot(
			source->tts_tupleDescriptor, &TTSOpsMinimalTuple);
		ExecStoreMinimalTuple(copy, stable_slot, /*shouldFree=*/false);
		slot_getallattrs(stable_slot);
		runtime.query_tuples.push_back(BioseqBatchJoinQueryTuple{
			.tuple = copy,
			.slot = stable_slot,
			.ordinal = ordinal,
		});
	}
	runtime.all_query_refs.reserve(runtime.query_tuples.size());
	for (BioseqBatchJoinQueryTuple& query : runtime.query_tuples) {
		runtime.all_query_refs.push_back(
			bioseq::scheduler::DispatchJoinQueryRef{
				.ordinal = query.ordinal,
				.query_slot = query.slot,
			});
	}
	runtime.queries_materialized = true;
}

static void batch_join_set_placeholder_value(
		TupleTableSlot* slot,
		const BioseqBatchJoinPlaceholderSite& site,
		const bioseq::simd::active_target::MatchResult& match,
		const BioseqStubOids& oids,
		MemoryContext output_context) {
	if (slot == nullptr || site.resno <= 0
		|| site.resno > slot->tts_tupleDescriptor->natts) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin placeholder resno is invalid")));
	}
	Datum value = static_cast<Datum>(0);
	bool is_null = false;
	auto bigint = [&value](const std::uint64_t input) {
		if (input > static_cast<std::uint64_t>(INT64_MAX)) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("BioseqBatchJoin match coordinate exceeds bigint")));
		}
		value = Int64GetDatum(static_cast<std::int64_t>(input));
	};
	if (site.funcid == oids.score_oid || site.funcid == oids.dp_score_oid) {
		value = Int32GetDatum(match.dp_score);
	} else if (site.funcid == oids.query_len_oid) {
		bigint(match.query_len);
	} else if (site.funcid == oids.query_start_oid) {
		bigint(match.query_start);
	} else if (site.funcid == oids.query_end_oid) {
		bigint(match.query_end);
	} else if (site.funcid == oids.target_len_oid) {
		bigint(match.target_len);
	} else if (site.funcid == oids.target_start_oid) {
		bigint(match.target_start);
	} else if (site.funcid == oids.target_end_oid) {
		bigint(match.target_end);
	} else if (site.funcid == oids.num_matches_oid) {
		bigint(match.num_matches);
	} else if (site.funcid == oids.block_length_oid) {
		bigint(match.block_length);
	} else if (site.funcid == oids.edit_oid) {
		bigint(match.edit_distance);
	} else if (site.funcid == oids.mapq_oid) {
		value = Int32GetDatum(match.mapq);
	} else if (site.funcid == oids.chain_score_oid) {
		value = Int32GetDatum(match.chain_score);
	} else if (site.funcid == oids.dp_max_score_oid) {
		value = Int32GetDatum(match.dp_max_score);
	} else if (site.funcid == oids.seed_count_oid) {
		value = Int32GetDatum(match.seed_count);
	} else if (site.funcid == oids.secondary_chain_score_oid) {
		value = Int32GetDatum(match.secondary_chain_score);
	} else if (site.funcid == oids.is_reverse_oid) {
		value = BoolGetDatum(match.is_reverse);
	} else if (site.funcid == oids.is_primary_oid) {
		value = BoolGetDatum(match.is_primary);
	} else if (site.funcid == oids.has_cigar_oid) {
		value = BoolGetDatum(match.has_cigar);
	} else if (site.funcid == oids.divergence_oid) {
		value = Float4GetDatum(match.divergence);
	} else if (site.funcid == oids.cigar_oid) {
		const std::string cigar =
			bioseq::simd::active_target::format_cigar(match.cigar);
		MemoryContext previous = MemoryContextSwitchTo(output_context);
		value = CStringGetTextDatum(cigar.c_str());
		MemoryContextSwitchTo(previous);
	} else {
		is_null = true;
	}
	slot->tts_values[site.resno - 1] = value;
	slot->tts_isnull[site.resno - 1] = is_null;
}

static TupleTableSlot* batch_join_project_output(
		CustomScanState* node,
		BioseqBatchJoinRuntime& runtime,
		const bioseq::scheduler::DispatchNextOutput& output) {
	if (output.scan_slot_copy == nullptr || output.match == nullptr
		|| output.query_ordinal >= runtime.query_tuples.size()) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqBatchJoin output references invalid inputs")));
	}
	ExprContext* expression_context = node->ss.ps.ps_ExprContext;
	if (expression_context == nullptr
		|| expression_context->ecxt_per_tuple_memory == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin expression context is missing")));
	}
	/* We bypass ExecScan, so own the per-output reset it would normally do. */
	ResetExprContext(expression_context);
	TupleTableSlot* query_slot = runtime.query_tuples[
		output.query_ordinal].slot;
	TupleTableSlot* combined = node->ss.ss_ScanTupleSlot;
	ExecClearTuple(combined);
	slot_getallattrs(output.scan_slot_copy);
	slot_getallattrs(query_slot);
	/* A locator target remains deferred while BatchJoin aligns it.  Once the
	 * match has been produced, an enclosing SliceProject may consume that same
	 * target through a source-backed recipe; promote the output copy only now,
	 * into this output's per-tuple context. */
	if (runtime.target_child != nullptr
		&& IsA(runtime.target_child, CustomScanState)) {
		auto* target_custom =
			reinterpret_cast<CustomScanState*>(runtime.target_child);
		if (target_custom->methods == &BioseqColumnScanExecuteMethods) {
			auto* target_state =
				BioseqScanState::from_custom_scan_state(target_custom);
			BioseqScanRuntime& target_runtime = bioseq_runtime(target_state);
			if (target_runtime.lazy_source_projection) {
				promote_slice_project_sources(
					target_state, output.scan_slot_copy,
					expression_context->ecxt_per_tuple_memory,
					/*allow_locator_passthrough=*/true);
				slot_getallattrs(output.scan_slot_copy);
			}
		}
	}
	if (runtime.target_column_count
		> output.scan_slot_copy->tts_tupleDescriptor->natts
		|| runtime.query_column_count
		> query_slot->tts_tupleDescriptor->natts
		|| runtime.target_column_count + runtime.query_column_count
			> combined->tts_tupleDescriptor->natts) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin combined tuple layout is invalid")));
	}
	for (int i = 0; i < runtime.target_column_count; ++i) {
		combined->tts_values[i] = output.scan_slot_copy->tts_values[i];
		combined->tts_isnull[i] = output.scan_slot_copy->tts_isnull[i];
	}
	for (int i = 0; i < runtime.query_column_count; ++i) {
		const int destination = runtime.target_column_count + i;
		combined->tts_values[destination] = query_slot->tts_values[i];
		combined->tts_isnull[destination] = query_slot->tts_isnull[i];
	}
	const BioseqStubOids& oids = get_cached_stub_oids();
	for (const BioseqBatchJoinPlaceholderSite& site : runtime.placeholder_sites) {
		batch_join_set_placeholder_value(
			combined, site, *output.match, oids,
			expression_context->ecxt_per_tuple_memory);
	}
	ExecStoreVirtualTuple(combined);
	expression_context->ecxt_scantuple = combined;
	if (node->ss.ps.qual != nullptr
		&& !ExecQual(node->ss.ps.qual, expression_context)) {
		InstrCountFiltered1(&node->ss, 1);
		return nullptr;
	}
	return node->ss.ps.ps_ProjInfo != nullptr
		? ExecProject(node->ss.ps.ps_ProjInfo)
		: combined;
}

static Node* BioseqBatchJoin_CreateCustomScanState(CustomScan* cscan) {
	(void)cscan;
	auto* state = reinterpret_cast<BioseqBatchJoinState*>(
		newNode(sizeof(BioseqBatchJoinState), T_CustomScanState));
	state->custom_scanstate.methods = &BioseqBatchJoinExecuteMethods;
	state->runtime = nullptr;
	return reinterpret_cast<Node*>(&state->custom_scanstate);
}

[[nodiscard]] static bool bioseq_batch_join_is_post_join_recipe(
		CustomScanState const* node) noexcept {
	if (node == nullptr || node->ss.ps.plan == nullptr
		|| !IsA(node->ss.ps.plan, CustomScan)) return false;
	auto const* cscan = reinterpret_cast<CustomScan const*>(node->ss.ps.plan);
	if (list_length(cscan->custom_plans) != 1
		|| list_length(cscan->custom_private)
			!= kBioseqPerRowJoinPrivateFields) return false;
	Node* version = reinterpret_cast<Node*>(
		linitial(cscan->custom_private));
	return version != nullptr && IsA(version, Integer)
		&& intVal(version) == kBioseqPerRowJoinPlanVersion;
}

static void BioseqBatchJoin_BeginCustomScan(
		CustomScanState* node, EState* estate, int eflags) {
	if (bioseq_batch_join_is_post_join_recipe(node)) {
		BioseqPerRowJoin_BeginCustomScan(node, estate, eflags);
		return;
	}
	auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
	if (IsParallelWorker() || cscan == nullptr
		|| cscan->scan.scanrelid != 0
		|| list_length(cscan->custom_plans) != 2
		|| cscan->custom_exprs != NIL
		|| node->custom_ps != NIL
		|| list_length(cscan->custom_private)
			!= kBioseqBatchJoinPrivateFields) {
			ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("BioseqBatchJoin executor shape is invalid or parallel")));
	}
	/* custom_scan_tlist deliberately retains original helper/PHV identities
	 * as descriptor-only columns, but PostgreSQL setrefs must have removed
	 * every helper from the executable plan targetlist.  If that contract ever
	 * changes, fail before child initialization instead of silently executing a
	 * rendezvous stub (which would fabricate a CPU-looking success). */
	for (TargetEntry* output : PList<TargetEntry>(
			cscan->scan.plan.targetlist)) {
		if (output != nullptr && output->expr != nullptr
			&& batch_join_expression_has_match_helper(
				reinterpret_cast<Node*>(output->expr),
				get_cached_stub_oids())) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin executable projection retained a match helper"),
				 errdetail(
					"setrefs did not map every helper identity to an executor-owned scan slot; no child was initialized")));
		}
	}
	auto* state = reinterpret_cast<BioseqBatchJoinState*>(node);
	state->runtime = nullptr;
	BioseqBatchJoinRuntime* volatile runtime = nullptr;
	bool volatile cleanup_registered = false;
	MemoryContext saved_context = CurrentMemoryContext;
	ErrorData* volatile original_error = nullptr;
	ErrorData* volatile converted_pg_error = nullptr;
	std::exception_ptr cpp_error;
	PG_TRY();
	{
	  try {
		runtime = std::construct_at(static_cast<BioseqBatchJoinRuntime*>(
			MemoryContextAlloc(
				estate->es_query_cxt, sizeof(BioseqBatchJoinRuntime))));
		state->runtime = runtime;
		runtime->cleanup_callback.func = batch_join_runtime_cleanup;
		runtime->cleanup_callback.arg = state;
		MemoryContextRegisterResetCallback(
			estate->es_query_cxt, &runtime->cleanup_callback);
		cleanup_registered = true;
		auto private_integer = [cscan](const int index) -> int {
			Node* encoded = reinterpret_cast<Node*>(
				list_nth(cscan->custom_private, index));
			if (encoded == nullptr || !IsA(encoded, Integer)) {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("BioseqBatchJoin private integer ABI is invalid"),
					 errdetail("field=%d", index)));
			}
			return intVal(encoded);
		};
		const int version = private_integer(kBatchJoinVersion);
		const int token = private_integer(kBatchJoinToken);
		runtime->target_relation_oid = static_cast<Oid>(
			private_integer(kBatchJoinTargetRelationOid));
		runtime->target_storage_seq_attno = static_cast<AttrNumber>(
			private_integer(kBatchJoinTargetStorageSeqAttno));
		runtime->target_slot_seq_resno = static_cast<AttrNumber>(
			private_integer(kBatchJoinTargetSlotSeqResno));
		runtime->target_slot_rowno_resno = static_cast<AttrNumber>(
			private_integer(kBatchJoinTargetSlotRownoResno));
		runtime->query_slot_seq_resno = static_cast<AttrNumber>(
			private_integer(kBatchJoinQuerySlotSeqResno));
		runtime->target_column_count =
			private_integer(kBatchJoinTargetColumnCount);
		runtime->query_column_count =
			private_integer(kBatchJoinQueryColumnCount);
		const int raw_alphabet_code = private_integer(kBatchJoinAlphabetCode);
		const int raw_storage_width =
			private_integer(kBatchJoinStorageUnitByteWidth);
		const int raw_requirement =
			private_integer(kBatchJoinIndexReadRequirement);
		runtime->honest_startup_cost = batch_join_cost_from_node(
			reinterpret_cast<Node*>(list_nth(
				cscan->custom_private, kBatchJoinHonestStartupCost)),
			"honest_startup_cost");
		runtime->honest_total_cost = batch_join_cost_from_node(
			reinterpret_cast<Node*>(list_nth(
				cscan->custom_private, kBatchJoinHonestTotalCost)),
			"honest_total_cost");
		runtime->incumbent_cost_floor = batch_join_cost_from_node(
			reinterpret_cast<Node*>(list_nth(
				cscan->custom_private, kBatchJoinIncumbentCostFloor)),
			"incumbent_cost_floor");
		Node* sites_node = reinterpret_cast<Node*>(
			list_nth(cscan->custom_private, kBatchJoinPlaceholderSites));
		/* PostgreSQL's canonical empty List is NIL, i.e. a null pointer.  Keep
		 * shape validity separate from the decoded pointer so an empty helper
		 * set is valid while every non-List node remains fail-closed. */
		const bool sites_shape_valid =
			sites_node == nullptr || IsA(sites_node, List);
		List* sites = sites_node == nullptr
			? NIL : reinterpret_cast<List*>(sites_node);
		FuncExpr* mmsearch = pg_node_try_as<FuncExpr>(
			reinterpret_cast<Node*>(
				list_nth(cscan->custom_private, kBatchJoinMmsearchExpr)));
		if (version != kBioseqBatchJoinPlanVersion || token <= 0
			|| !OidIsValid(runtime->target_relation_oid)
			|| runtime->target_storage_seq_attno <= 0
			|| runtime->target_slot_seq_resno <= 0
			|| runtime->target_slot_rowno_resno <= 0
			|| runtime->query_slot_seq_resno <= 0
			|| runtime->target_column_count <= 0
			|| runtime->query_column_count <= 0
			|| raw_alphabet_code < 0 || raw_alphabet_code > UINT8_MAX
			|| (raw_storage_width != 1 && raw_storage_width != 2
				&& raw_storage_width != 4 && raw_storage_width != 8)
			|| !bioseq::indexing::index_read_requirement_ordinal_valid(
				raw_requirement)
			|| !sites_shape_valid || mmsearch == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin executor private ABI is invalid")));
		}
		runtime->alphabet_code = static_cast<std::uint8_t>(raw_alphabet_code);
		runtime->storage_unit_byte_width =
			static_cast<std::uint8_t>(raw_storage_width);
		const int32 expected_typmod = compact_sequence_stdc_type_params{
			runtime->alphabet_code,
			runtime->storage_unit_byte_width}.encode();
		if (!batch_join_typmod_is_supported(expected_typmod)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqBatchJoin encoded typmod is unsupported")));
		}
		runtime->index_read_requirement =
			static_cast<bioseq::indexing::IndexReadRequirement>(
				raw_requirement);
		runtime->explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;

		/* Re-decode the catalog composite before any child initialization.  This
		 * catches catalog drift and proves the ordinal marker was not altered
		 * between planning and execution. */
		bioseq::simd::active_target::Minimap2Params prevalidated_params =
			build_params_from_gucs();
		bioseq::indexing::IndexReadRequirement decoded_requirement =
			bioseq::indexing::IndexReadRequirement::allow;
		(void)apply_aligner_options_override(
			mmsearch, prevalidated_params, decoded_requirement,
			get_cached_stub_oids());
		finalize_simd_chaining_policy(prevalidated_params);
		if (decoded_requirement != runtime->index_read_requirement) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin index requirement marker disagrees with options")));
		}
		/* A selected Vulkan BatchJoin is allowed to initialize its children and
		 * reach TupleDispatchBuffer's production Format-5 preparation path.  That
		 * path opens the exact PostgreSQL-owned MPS generation, validates READY,
		 * revision/ABI/source/device identity, and retains its three resident GPU
		 * buffers.  Missing or incompatible data still fails there before typed
		 * dispatch; the typed processor independently requires that exact GPU
		 * prepared target and has no CPU/SIMD/recompute fallback.  The older
		 * column-scan path remains rejected separately because it has no such
		 * prepared-target owner. */

		TupleDesc scan_desc = node->ss.ss_ScanTupleSlot == nullptr
			? nullptr : node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
		const std::int64_t child_column_count =
			static_cast<std::int64_t>(runtime->target_column_count)
			+ static_cast<std::int64_t>(runtime->query_column_count);
		if (scan_desc == nullptr || child_column_count <= 0
			|| child_column_count > scan_desc->natts
			|| list_length(sites)
				!= scan_desc->natts - child_column_count) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin scan tuple/private layout is invalid"),
				 errdetail(
					"target_columns=%d query_columns=%d scan_columns=%d placeholder_sites=%d",
					runtime->target_column_count,
					runtime->query_column_count,
					scan_desc == nullptr ? -1 : scan_desc->natts,
					list_length(sites))));
		}
		Bitmapset* placeholder_resnos = nullptr;
		for (List* encoded : PList<List>(sites)) {
			Node* encoded_node = reinterpret_cast<Node*>(encoded);
			if (encoded_node == nullptr || !IsA(encoded_node, List)
				|| list_length(encoded) != 4) {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("BioseqBatchJoin placeholder ABI is invalid")));
			}
			for (int index = 0; index < 4; ++index) {
				Node* value = reinterpret_cast<Node*>(list_nth(encoded, index));
				if (value == nullptr || !IsA(value, Integer)) {
					ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("BioseqBatchJoin placeholder scalar ABI is invalid")));
				}
			}
			const int raw_resno = intVal(list_nth(encoded, 0));
			const bool duplicate_resno = raw_resno > 0
				&& bms_is_member(raw_resno, placeholder_resnos);
			if (raw_resno <= child_column_count
				|| raw_resno > scan_desc->natts
				|| duplicate_resno) {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("BioseqBatchJoin placeholder resno ABI is invalid"),
					 errdetail(
						"resno=%d child_columns=%lld scan_columns=%d duplicate=%s",
						raw_resno,
						static_cast<long long>(child_column_count),
						scan_desc->natts,
						duplicate_resno ? "true" : "false")));
			}
			placeholder_resnos = bms_add_member(
				placeholder_resnos, raw_resno);
			const BioseqBatchJoinPlaceholderSite site{
				.resno = static_cast<AttrNumber>(raw_resno),
				.funcid = static_cast<Oid>(intVal(list_nth(encoded, 1))),
				.typmod = intVal(list_nth(encoded, 2)),
				.collation = static_cast<Oid>(intVal(list_nth(encoded, 3))),
			};
			Form_pg_attribute attribute = scan_desc == nullptr
				|| site.resno <= 0 || site.resno > scan_desc->natts
				? nullptr : TupleDescAttr(scan_desc, site.resno - 1);
			if (attribute == nullptr
				|| !is_bioseq_projection_funcid(
					site.funcid, get_cached_stub_oids())
				|| attribute->atttypid != get_func_rettype(site.funcid)
				|| attribute->atttypmod != site.typmod
				|| attribute->attcollation != site.collation) {
				ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("BioseqBatchJoin placeholder type/resno ABI is invalid")));
			}
			runtime->placeholder_sites.push_back(site);
		}
		bms_free(placeholder_resnos);

		Plan* target_plan = reinterpret_cast<Plan*>(linitial(cscan->custom_plans));
		Plan* query_plan = reinterpret_cast<Plan*>(lsecond(cscan->custom_plans));
		if (target_plan == nullptr || query_plan == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin child plan ABI is invalid")));
		}
		runtime->query_planner_rows = std::max(0.0, query_plan->plan_rows);
		runtime->query_planner_width = std::max(0, query_plan->plan_width);
		runtime->target_child = ExecInitNode(target_plan, estate, eflags);
		runtime->target_child_initialized = true;
		node->custom_ps = lappend(node->custom_ps, runtime->target_child);
		runtime->query_child = ExecInitNode(query_plan, estate, eflags);
		runtime->query_child_initialized = true;
		node->custom_ps = lappend(node->custom_ps, runtime->query_child);
		auto* target_custom = reinterpret_cast<CustomScan*>(target_plan);
		if (!IsA(target_plan, CustomScan)
			|| target_custom->methods != &BioseqColumnScanScanMethods) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin target provider is invalid")));
		}
		CustomScanState* target_custom_state =
			pg_node_try_as<CustomScanState>(
				reinterpret_cast<Node*>(runtime->target_child));
		if (target_custom_state == nullptr
			|| target_custom_state->methods != &BioseqColumnScanExecuteMethods
			|| target_custom_state->ss.ps.plan != target_plan
			|| target_custom_state->ss.ss_currentRelation == nullptr
			|| RelationGetRelid(target_custom_state->ss.ss_currentRelation)
				!= runtime->target_relation_oid) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin initialized target provider state is invalid")));
		}
		BioseqScanState* target_bioseq_state =
			BioseqScanState::from_custom_scan_state(target_custom_state);
		Snapshot target_snapshot = target_bioseq_state->snapshot;
		if (target_snapshot == nullptr
			|| !IsMVCCSnapshot(target_snapshot)
			|| target_snapshot->regd_count == 0) {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("BioseqBatchJoin target provider snapshot is not registered MVCC state")));
		}
		RangeTblEntry* target_rte = exec_rt_fetch(
			target_custom->scan.scanrelid, estate);
		if (target_rte == nullptr || target_rte->rtekind != RTE_RELATION
			|| target_rte->relid != runtime->target_relation_oid) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin target provider relation is invalid")));
		}
		List* locator_tag = nullptr;
		for (Node* raw_tag : PList<Node>(target_custom->custom_private)) {
			if (raw_tag == nullptr || !IsA(raw_tag, List)) continue;
			List* candidate = reinterpret_cast<List*>(raw_tag);
			if (list_length(candidate) != 4
				|| !IsA(reinterpret_cast<Node*>(linitial(candidate)), Integer)
				|| intVal(linitial(candidate))
					!= kBioseqLocatorPassthroughMagic) continue;
			if (locator_tag != nullptr) {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("BioseqBatchJoin target has duplicate locator ownership")));
			}
			locator_tag = candidate;
		}
		if (locator_tag == nullptr || list_length(locator_tag) != 4
			|| intVal(list_nth(locator_tag, 0))
				!= kBioseqLocatorPassthroughMagic
			|| intVal(list_nth(locator_tag, 1))
				!= kBioseqBatchJoinPlanVersion
			|| intVal(list_nth(locator_tag, 2)) != token
			|| intVal(list_nth(locator_tag, 3))
				!= runtime->target_slot_rowno_resno) {
				ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin target locator token mismatch")));
		}
		/* Validate against the post-setrefs Plan targetlists.  They are stable
		 * planner ABI; ps_ResultTupleSlot is not, because projection-free and
		 * pass-through PlanStates may return a scan/child slot instead.  The
		 * actual slots are validated independently at each ExecProcNode edge. */
		if (runtime->target_column_count
				!= list_length(target_plan->targetlist)
			|| runtime->query_column_count
				!= list_length(query_plan->targetlist)
			|| runtime->target_slot_seq_resno <= 0
			|| runtime->target_slot_seq_resno
				> runtime->target_column_count
			|| runtime->target_slot_rowno_resno <= 0
			|| runtime->target_slot_rowno_resno
				> runtime->target_column_count
			|| runtime->query_slot_seq_resno <= 0
			|| runtime->query_slot_seq_resno
				> runtime->query_column_count) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin post-setrefs child targetlists are invalid"),
				 errdetail(
					"target_private_columns=%d target_plan_columns=%d query_private_columns=%d query_plan_columns=%d target_sequence_resno=%d target_rowno_resno=%d query_sequence_resno=%d",
					runtime->target_column_count,
					list_length(target_plan->targetlist),
					runtime->query_column_count,
					list_length(query_plan->targetlist),
					runtime->target_slot_seq_resno,
					runtime->target_slot_rowno_resno,
					runtime->query_slot_seq_resno)));
		}
		TargetEntry* target_seq_entry = reinterpret_cast<TargetEntry*>(
			list_nth(
				target_plan->targetlist,
				runtime->target_slot_seq_resno - 1));
		TargetEntry* target_rowno_entry = reinterpret_cast<TargetEntry*>(
			list_nth(
				target_plan->targetlist,
				runtime->target_slot_rowno_resno - 1));
		TargetEntry* query_seq_entry = reinterpret_cast<TargetEntry*>(
			list_nth(
				query_plan->targetlist,
				runtime->query_slot_seq_resno - 1));
		const int32 expected_sequence_typmod =
			compact_sequence_stdc_type_params{
				runtime->alphabet_code,
				runtime->storage_unit_byte_width}.encode();
		const Oid sequence_type =
			get_cached_stub_oids().bioseq_seq_type_oid;
		if (target_seq_entry == nullptr || target_rowno_entry == nullptr
			|| query_seq_entry == nullptr
			|| target_seq_entry->resno != runtime->target_slot_seq_resno
			|| target_rowno_entry->resno != runtime->target_slot_rowno_resno
			|| query_seq_entry->resno != runtime->query_slot_seq_resno
			|| !target_rowno_entry->resjunk
			|| exprType(reinterpret_cast<Node*>(target_seq_entry->expr))
				!= get_cached_stub_oids().bioseq_seq_type_oid
			|| exprType(reinterpret_cast<Node*>(query_seq_entry->expr))
				!= get_cached_stub_oids().bioseq_seq_type_oid
			|| exprTypmod(reinterpret_cast<Node*>(target_seq_entry->expr))
				!= expected_sequence_typmod
			|| exprTypmod(reinterpret_cast<Node*>(query_seq_entry->expr))
				!= expected_sequence_typmod
			|| exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr))
				!= exprCollation(reinterpret_cast<Node*>(query_seq_entry->expr))
			|| exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr))
				!= get_typcollation(sequence_type)
			|| exprType(reinterpret_cast<Node*>(target_rowno_entry->expr))
				!= INT8OID
			|| exprTypmod(reinterpret_cast<Node*>(target_rowno_entry->expr)) != -1
			|| OidIsValid(exprCollation(
				reinterpret_cast<Node*>(target_rowno_entry->expr)))) {
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqBatchJoin post-setrefs child targetlist types are invalid"),
				 errdetail(
					"target_sequence_type=%u target_sequence_typmod=%d target_sequence_collation=%u query_sequence_type=%u query_sequence_typmod=%d query_sequence_collation=%u rowno_type=%u rowno_typmod=%d rowno_collation=%u rowno_resjunk=%s",
					target_seq_entry == nullptr ? InvalidOid
						: exprType(reinterpret_cast<Node*>(target_seq_entry->expr)),
					target_seq_entry == nullptr ? -1
						: exprTypmod(reinterpret_cast<Node*>(target_seq_entry->expr)),
					target_seq_entry == nullptr ? InvalidOid
						: exprCollation(reinterpret_cast<Node*>(target_seq_entry->expr)),
					query_seq_entry == nullptr ? InvalidOid
						: exprType(reinterpret_cast<Node*>(query_seq_entry->expr)),
					query_seq_entry == nullptr ? -1
						: exprTypmod(reinterpret_cast<Node*>(query_seq_entry->expr)),
					query_seq_entry == nullptr ? InvalidOid
						: exprCollation(reinterpret_cast<Node*>(query_seq_entry->expr)),
					target_rowno_entry == nullptr ? InvalidOid
						: exprType(reinterpret_cast<Node*>(target_rowno_entry->expr)),
					target_rowno_entry == nullptr ? -1
						: exprTypmod(reinterpret_cast<Node*>(target_rowno_entry->expr)),
					target_rowno_entry == nullptr ? InvalidOid
						: exprCollation(reinterpret_cast<Node*>(target_rowno_entry->expr)),
					target_rowno_entry != nullptr && target_rowno_entry->resjunk
						? "true" : "false")));
		}

		bioseq::simd::active_target::Minimap2Params params =
			build_params_from_gucs();
		bioseq::indexing::IndexReadRequirement requirement =
			bioseq::indexing::IndexReadRequirement::allow;
		(void)apply_aligner_options_override(
			mmsearch, params, requirement, get_cached_stub_oids());
		finalize_simd_chaining_policy(params);
		if (requirement != runtime->index_read_requirement) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqBatchJoin requirement changed after prevalidation")));
		}
		/* Plan setrefs has deliberately replaced every match-result helper
		 * with an INDEX_VAR into the combined scan tuple.  Derive expensive
		 * stage requirements from the validated private placeholder kinds,
		 * not by re-walking an executor targetlist in which helpers no longer
		 * exist. */
		params.do_cigar = false;
		params.compute_divergence = false;
		params.compute_mapq = false;
		params.compute_edit_distance = false;
		const BioseqStubOids& batch_oids = get_cached_stub_oids();
		for (const BioseqBatchJoinPlaceholderSite& site :
			 runtime->placeholder_sites) {
			params.do_cigar = params.do_cigar
				|| site.funcid == batch_oids.cigar_oid
				|| site.funcid == batch_oids.has_cigar_oid
				|| site.funcid == batch_oids.edit_oid;
			params.compute_edit_distance = params.compute_edit_distance
				|| site.funcid == batch_oids.edit_oid;
			params.compute_divergence = params.compute_divergence
				|| site.funcid == batch_oids.divergence_oid;
			params.compute_mapq = params.compute_mapq
				|| site.funcid == batch_oids.mapq_oid;
		}

		/* Reuse the target provider's executor-owned Relation. Its lock lives
		 * through ExecEndNode; this BatchJoin neither reopens nor closes it. */
		Relation target_relation =
			target_custom_state->ss.ss_currentRelation;
		if (target_relation == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("BioseqBatchJoin target executor relation is unavailable")));
		}
		if (target_relation->rd_tableam != GetBioseqColumnTableAmRoutine()) {
			ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("BioseqBatchJoin target relation changed table AM")));
		}
		TupleDesc relation_desc = RelationGetDescr(target_relation);
		Form_pg_attribute storage_sequence_attribute =
			runtime->target_storage_seq_attno <= 0
			|| runtime->target_storage_seq_attno > relation_desc->natts
			? nullptr
			: TupleDescAttr(
				relation_desc, runtime->target_storage_seq_attno - 1);
		if (storage_sequence_attribute == nullptr
			|| storage_sequence_attribute->attisdropped
			|| storage_sequence_attribute->atttypid
				!= get_cached_stub_oids().bioseq_seq_type_oid
			|| storage_sequence_attribute->atttypmod
				!= expected_sequence_typmod
			|| storage_sequence_attribute->attcollation
				!= get_typcollation(sequence_type)) {
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqBatchJoin storage-column ABI changed after planning")));
		}
		char* target_column_name = get_attname(
			runtime->target_relation_oid,
			runtime->target_storage_seq_attno,
			/*missing_ok=*/false);
		/* The target child may have replaced the executor snapshot while
		 * flushing its columnar write state. Borrow that exact registered
		 * snapshot. batch_join_release_executor_resources_checked drains the
		 * buffer (and its fragment readers) before ExecEndNode(target_child)
		 * releases the child's registration. */
		runtime->buffer.configure_params(
			params,
			requirement,
			bioseq_read_sequence_index,
			runtime->target_relation_oid,
			target_relation->rd_locator.dbOid,
			target_relation->rd_locator.spcOid,
			target_relation->rd_locator.relNumber,
			target_relation,
			target_snapshot);
		runtime->buffer.configure_mmsearch_join(
			runtime->target_relation_oid,
			runtime->target_storage_seq_attno,
			runtime->target_slot_seq_resno,
			runtime->target_slot_rowno_resno,
			runtime->query_slot_seq_resno,
			std::string{target_column_name},
			runtime->alphabet_code,
			runtime->storage_unit_byte_width);
		pfree(target_column_name);

		runtime->query_tuple_context = AllocSetContextCreate(
			estate->es_query_cxt,
			"BioseqBatchJoin query tuples",
			ALLOCSET_DEFAULT_SIZES);
		const int plan_node_id = cscan->scan.plan.plan_node_id;
		if (plan_node_id < 0) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqBatchJoin plan node id is invalid")));
		}
		runtime->alignment_registration =
			bioseq::pg::alignment_execution::bind_alignment_node(
				estate, static_cast<std::uint32_t>(plan_node_id));
		runtime->buffer.bind_persisted_gpu_operation_evidence_sink(
			*bioseq::pg::alignment_execution::
				borrow_persisted_gpu_operation_evidence_sink(
					runtime->alignment_registration));
		  } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
			converted_pg_error = error.release_error_data();
		  } catch (...) {
			cpp_error = std::current_exception();
	  }
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		original_error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	BioseqBatchJoinRuntime* stable_runtime = runtime;
	const bool stable_cleanup_registered = cleanup_registered;
	auto abandon = [node, state, stable_runtime,
		stable_cleanup_registered]() noexcept {
		if (stable_runtime == nullptr) return;
		batch_join_abandon_executor_resources_after_error(
			node, *stable_runtime);
		if (!stable_cleanup_registered) {
			state->runtime = nullptr;
			std::destroy_at(stable_runtime);
		}
	};
	if (original_error != nullptr) {
		abandon();
		ErrorData* exact_error = original_error;
		ReThrowError(exact_error);
	}
	if (converted_pg_error != nullptr) {
		abandon();
		ErrorData* exact_error = converted_pg_error;
		ReThrowError(exact_error);
	}
	if (cpp_error != nullptr) {
		abandon();
		char detail[512] = "unknown C++ exception";
		try {
			std::rethrow_exception(cpp_error);
		} catch (const std::exception& error) {
			strlcpy(detail,
				error.what() == nullptr ? "std::exception without detail"
					: error.what(),
				sizeof(detail));
		} catch (...) {
		}
		cpp_error = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin Begin raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
}

static void batch_join_release_executor_resources_checked(
		CustomScanState* node,
		BioseqBatchJoinRuntime& runtime,
		const bool delete_query_context) {
	if (runtime.cleanup_done) {
		return;
	}
	if (runtime.target_active) {
		bioseq::pg_utils::invoke_pg_as_cpp([&] {
			runtime.buffer.finish_join_target();
		});
		runtime.target_active = false;
	}
	bioseq::pg_utils::invoke_pg_as_cpp([&] {
		runtime.buffer.reset_join_runtime_for_rescan();
	});
	for (BioseqBatchJoinQueryTuple& query : runtime.query_tuples) {
		if (query.slot != nullptr) {
			TupleTableSlot* finished = query.slot;
			bioseq::pg_utils::invoke_pg_as_cpp(
				[finished] { ExecDropSingleTupleTableSlot(finished); });
			query.slot = nullptr;
		}
	}
	runtime.query_tuples.clear();
	runtime.all_query_refs.clear();
	if (runtime.query_tuple_context != nullptr) {
		MemoryContext context = runtime.query_tuple_context;
		bioseq::pg_utils::invoke_pg_as_cpp([&] {
			if (delete_query_context) {
				MemoryContextDelete(context);
			} else {
				MemoryContextReset(context);
			}
		});
		runtime.query_tuple_context = nullptr;
	}
	/* Release admission only after the owning tuple context was reset/deleted. */
	runtime.query_permits.clear();
	runtime.query_reserved_admission_bytes = 0;
	runtime.query_actual_materialized_bytes = 0;
	runtime.queries_materialized = false;
	if (runtime.query_child_initialized && !runtime.query_child_ended) {
		PlanState* finished = runtime.query_child;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[finished] { ExecEndNode(finished); });
		runtime.query_child_ended = true;
	}
	if (runtime.target_child_initialized && !runtime.target_child_ended) {
		PlanState* finished = runtime.target_child;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[finished] { ExecEndNode(finished); });
		runtime.target_child_ended = true;
	}
	if (node != nullptr) {
		node->custom_ps = NIL;
	}
	runtime.cleanup_done = true;
}

static void batch_join_abandon_executor_resources_after_error(
		CustomScanState* node,
		BioseqBatchJoinRuntime& runtime) noexcept {
	if (runtime.cleanup_done) {
		return;
	}
	runtime.buffer.abandon_join_runtime_after_pg_error_noexcept();
	for (BioseqBatchJoinQueryTuple& query : runtime.query_tuples) {
		query.slot = nullptr;
		query.tuple = nullptr;
	}
	runtime.query_tuples.clear();
	runtime.all_query_refs.clear();
	runtime.placeholder_sites.clear();
	runtime.query_tuple_context = nullptr;
	runtime.query_permits.clear();
	runtime.query_reserved_admission_bytes = 0;
	runtime.query_actual_materialized_bytes = 0;
	runtime.queries_materialized = false;
	runtime.target_active = false;
	runtime.current_target_slot = nullptr;
	runtime.query_child = nullptr;
	runtime.target_child = nullptr;
	runtime.query_child_ended = true;
	runtime.target_child_ended = true;
	runtime.query_child_initialized = false;
	runtime.target_child_initialized = false;
	if (node != nullptr) {
		node->custom_ps = NIL;
	}
	runtime.cleanup_done = true;
}

static TupleTableSlot* BioseqBatchJoin_ExecCustomScanImpl(
		CustomScanState* node) {
	BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
	if (runtime == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin runtime is missing")));
	}
	bioseq::pg::alignment_execution::ensure_alignment_node_registered(
		runtime->alignment_registration);
	batch_join_materialize_queries(*runtime);

	for (;;) {
		auto output = runtime->buffer.read_next_output();
		if (output.has_value()) {
			TupleTableSlot* projected = batch_join_project_output(
				node, *runtime, *output);
			if (projected != nullptr) return projected;
			continue;
		}
		if (runtime->target_active
			&& runtime->next_query_batch < runtime->all_query_refs.size()) {
			const std::size_t batch_end = std::min(
				runtime->all_query_refs.size(),
				runtime->next_query_batch + kBioseqRuntimeBufferReserve);
			const std::span<const bioseq::scheduler::DispatchJoinQueryRef> batch{
				runtime->all_query_refs.data() + runtime->next_query_batch,
				batch_end - runtime->next_query_batch};
			runtime->buffer.set_join_query_batch(batch);
			if (runtime->buffer.run_join_query_batch_and_build_cursors()) {
				const auto delta = runtime->buffer.execution_stats();
				runtime->aggregate_stats.merge(delta);
				bioseq::pg::alignment_execution::merge_alignment_batch(
					runtime->alignment_registration,
					delta);
			}
			runtime->next_query_batch = batch_end;
			++runtime->query_batch_count;
			continue;
		}
		if (runtime->target_active) {
			runtime->buffer.finish_join_target();
			runtime->target_active = false;
			runtime->current_target_slot = nullptr;
			runtime->next_query_batch = 0;
		}
		if (runtime->target_eof) {
			bioseq::pg::alignment_execution::mark_alignment_pass_fully_drained(
				runtime->alignment_registration);
			runtime->current_pass_fully_drained = true;
			return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
		}
		TupleTableSlot* target = ExecProcNode(runtime->target_child);
		if (TupIsNull(target)) {
			runtime->target_eof = true;
			continue;
		}
		batch_join_validate_returned_child_slot(
			target,
			runtime->target_column_count,
			runtime->target_slot_seq_resno,
			runtime->target_slot_rowno_resno,
			runtime->alphabet_code,
			runtime->storage_unit_byte_width,
			"target");
		slot_getallattrs(target);
		if (target->tts_isnull[runtime->target_slot_seq_resno - 1]) {
			/* Strict INNER predicate: do not hand a NULL target to dispatch. */
			continue;
		}
		runtime->current_target_slot = target;
		runtime->buffer.begin_join_target(target);
		runtime->buffer.prepare_join_target_queries(
			std::span<const bioseq::scheduler::DispatchJoinQueryRef>{
				runtime->all_query_refs});
		runtime->target_active = true;
		++runtime->distinct_targets;
	}
}

static TupleTableSlot* BioseqBatchJoin_ExecCustomScan(
		CustomScanState* node) {
	if (bioseq_batch_join_is_post_join_recipe(node)) {
		return BioseqPerRowJoin_ExecCustomScan(node);
	}
	TupleTableSlot* result = nullptr;
	MemoryContext saved_context = CurrentMemoryContext;
	ErrorData* volatile original_error = nullptr;
	ErrorData* volatile converted_pg_error = nullptr;
	std::exception_ptr cpp_error;
	PG_TRY();
	{
		try {
			result = BioseqBatchJoin_ExecCustomScanImpl(node);
			} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
				converted_pg_error = error.release_error_data();
			} catch (...) {
				cpp_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		original_error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	if (original_error != nullptr) {
		if (BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
			runtime != nullptr) {
			batch_join_abandon_executor_resources_after_error(
				node, *runtime);
		}
		ErrorData* exact_error = original_error;
		ReThrowError(exact_error);
	}
	if (converted_pg_error != nullptr) {
		if (BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
			runtime != nullptr) {
			batch_join_abandon_executor_resources_after_error(
				node, *runtime);
		}
		ErrorData* exact_error = converted_pg_error;
		ReThrowError(exact_error);
	}
	if (cpp_error != nullptr) {
		if (BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
			runtime != nullptr) {
			batch_join_abandon_executor_resources_after_error(
				node, *runtime);
		}
		char detail[512] = "unknown C++ exception";
		try {
			std::rethrow_exception(cpp_error);
		} catch (const std::exception& error) {
			strlcpy(detail,
				error.what() == nullptr ? "std::exception without detail"
					: error.what(),
				sizeof(detail));
		} catch (...) {
		}
		cpp_error = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin Exec raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	return result;
}

static void BioseqBatchJoin_EndCustomScan(CustomScanState* node) {
	if (bioseq_batch_join_is_post_join_recipe(node)) {
		BioseqPerRowJoin_EndCustomScan(node);
		return;
	}
	BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
	if (runtime == nullptr || runtime->cleanup_done) {
		return;
	}
	ErrorData* exact_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char detail[512] = "std::exception without detail";
	try {
		batch_join_release_executor_resources_checked(
			node, *runtime, /*delete_query_context=*/false);
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(detail,
			error.what() == nullptr ? "std::exception without detail"
				: error.what(),
			sizeof(detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	if (exact_error != nullptr || cpp_error || unknown_cpp_error) {
		batch_join_abandon_executor_resources_after_error(node, *runtime);
	}
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin checked End raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin checked End raised an unknown C++ exception")));
	}
}

static void BioseqBatchJoin_ReScanCustomScan(CustomScanState* node) {
	if (bioseq_batch_join_is_post_join_recipe(node)) {
		BioseqPerRowJoin_ReScanCustomScan(node);
		return;
	}
	BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
	if (runtime == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin rescan runtime is missing")));
	}
	MemoryContext saved_context = CurrentMemoryContext;
	ErrorData* volatile original_error = nullptr;
	ErrorData* volatile converted_pg_error = nullptr;
	std::exception_ptr cpp_error;
	PG_TRY();
	{
		try {
			bioseq::pg::alignment_execution::mark_alignment_rescan(
				runtime->alignment_registration);
			if (runtime->target_active) {
				runtime->buffer.finish_join_target();
			}
			runtime->buffer.reset_join_runtime_for_rescan();
			batch_join_clear_query_tuples(*runtime, /*drop_slots=*/true);
			runtime->target_active = false;
			runtime->target_eof = false;
			runtime->current_pass_fully_drained = false;
			runtime->current_target_slot = nullptr;
			runtime->next_query_batch = 0;
			runtime->active_query_batch_end = 0;
			ExecReScan(runtime->target_child);
			ExecReScan(runtime->query_child);
			} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
				converted_pg_error = error.release_error_data();
			} catch (...) {
				cpp_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		original_error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	if (original_error != nullptr) {
		batch_join_abandon_executor_resources_after_error(node, *runtime);
		ErrorData* exact_error = original_error;
		ReThrowError(exact_error);
	}
	if (converted_pg_error != nullptr) {
		batch_join_abandon_executor_resources_after_error(node, *runtime);
		ErrorData* exact_error = converted_pg_error;
		ReThrowError(exact_error);
	}
	if (cpp_error != nullptr) {
		batch_join_abandon_executor_resources_after_error(node, *runtime);
		char detail[512] = "unknown C++ exception";
		try {
			std::rethrow_exception(cpp_error);
		} catch (const std::exception& error) {
			strlcpy(detail,
				error.what() == nullptr ? "std::exception without detail"
					: error.what(),
				sizeof(detail));
		} catch (...) {
		}
		cpp_error = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin ReScan raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
}

static void BioseqBatchJoin_ExplainCustomScan(
		CustomScanState* node, List* ancestors, ExplainState* es) {
	if (bioseq_batch_join_is_post_join_recipe(node)) {
		BioseqPerRowJoin_ExplainCustomScan(node, ancestors, es);
		return;
	}
	(void)ancestors;
	BioseqBatchJoinRuntime* runtime = batch_join_runtime(node);
	ExplainPropertyInteger(
		"Bioseq Query Batch Capacity", nullptr,
		static_cast<std::int64_t>(kBioseqRuntimeBufferReserve), es);
	ExplainPropertyText(
		"Bioseq Residual Mmsearch Qual", "absent", es);
	ExplainPropertyInteger(
		"Bioseq Batch ABI Version", nullptr,
		static_cast<std::int64_t>(kBioseqBatchJoinPlanVersion), es);
	if (runtime == nullptr) {
		return;
	}
	ExplainPropertyFloat(
		"Bioseq Honest Startup Cost", nullptr,
		runtime->honest_startup_cost, 2, es);
	ExplainPropertyFloat(
		"Bioseq Honest Total Cost", nullptr,
		runtime->honest_total_cost, 2, es);
	ExplainPropertyFloat(
		"Bioseq Incumbent Cost Floor", nullptr,
		runtime->incumbent_cost_floor, 2, es);
	ExplainPropertyInteger(
		"Bioseq Match Placeholder Sites", nullptr,
		static_cast<std::int64_t>(runtime->placeholder_sites.size()), es);
	ExplainPropertyText(
		"Bioseq Index Read Requirement",
		bioseq::indexing::index_read_requirement_name(
			runtime->index_read_requirement).data(), es);
	ExplainPropertyBool(
		"Bioseq Requirement Executable",
		true,
		es);
	ExplainPropertyText(
		"Bioseq CPU Prepared Target",
		runtime->index_read_requirement
			== bioseq::indexing::IndexReadRequirement::require_absent
				? "disabled: ExactSimd target-sketch backend unavailable"
				: runtime->index_read_requirement
						== bioseq::indexing::IndexReadRequirement::
							require_compatible_persisted
					? "revision-3 grouped32 direct reader retained across query waves"
					: "persisted-only query-hash union; one target index per parameter group",
		es);
	ExplainPropertyFloat(
		"Bioseq Query Planner Rows", nullptr,
		runtime->query_planner_rows, 0, es);
	ExplainPropertyInteger(
		"Bioseq Query Planner Width", "bytes",
		static_cast<std::int64_t>(runtime->query_planner_width), es);
	ExplainPropertyInteger(
		"Bioseq Query Planner Admission", "bytes",
		static_cast<std::int64_t>(std::min<std::uint64_t>(
			runtime->query_planner_admission_bytes,
			static_cast<std::uint64_t>(INT64_MAX))), es);
	ExplainPropertyInteger(
		"Bioseq Query Actual Materialization", "bytes",
		static_cast<std::int64_t>(std::min<std::uint64_t>(
			runtime->query_actual_materialized_bytes,
			static_cast<std::uint64_t>(INT64_MAX))), es);
	ExplainPropertyInteger(
		"Bioseq Distinct Targets", nullptr,
		static_cast<std::int64_t>(runtime->distinct_targets), es);
	ExplainPropertyInteger(
		"Bioseq Query Batches", nullptr,
		static_cast<std::int64_t>(runtime->query_batch_count), es);
	ExplainPipelineExecutionStats(runtime->aggregate_stats, es);
}

[[nodiscard]] static BioseqPerRowJoinRuntime* per_row_join_runtime(
		CustomScanState* node) noexcept {
	auto* state = reinterpret_cast<BioseqPerRowJoinState*>(node);
	return state == nullptr
		? nullptr : static_cast<BioseqPerRowJoinRuntime*>(state->runtime);
}

static void per_row_join_abandon_after_error(
		CustomScanState* node, BioseqPerRowJoinRuntime& runtime) noexcept {
	if (runtime.cleanup_done) return;
	runtime.buffer.abandon_join_runtime_after_pg_error_noexcept();
	runtime.child = nullptr;
	runtime.target_relation = nullptr;
	runtime.child_initialized = false;
	runtime.child_ended = true;
	if (node != nullptr) node->custom_ps = NIL;
	runtime.cleanup_done = true;
}

static void per_row_join_runtime_cleanup(void* argument) {
	auto* state = static_cast<BioseqPerRowJoinState*>(argument);
	if (state == nullptr || state->runtime == nullptr) return;
	auto* runtime = static_cast<BioseqPerRowJoinRuntime*>(state->runtime);
	per_row_join_abandon_after_error(&state->custom_scanstate, *runtime);
	state->runtime = nullptr;
	std::destroy_at(runtime);
}

static void per_row_join_release_checked(
		CustomScanState* node, BioseqPerRowJoinRuntime& runtime) {
	if (runtime.cleanup_done) return;
	runtime.buffer.clear_slots();
	if (runtime.child_initialized && !runtime.child_ended
		&& runtime.child != nullptr) {
		ExecEndNode(runtime.child);
		runtime.child_ended = true;
	}
	if (runtime.target_relation != nullptr) {
		table_close(runtime.target_relation, NoLock);
		runtime.target_relation = nullptr;
	}
	runtime.child = nullptr;
	if (node != nullptr) node->custom_ps = NIL;
	runtime.cleanup_done = true;
}

static void BioseqPerRowJoin_BeginCustomScanImpl(
		CustomScanState* node, EState* estate, int eflags) {
	auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
	if (IsParallelWorker() || cscan == nullptr
		|| cscan->scan.scanrelid != 0 || cscan->custom_exprs != NIL
		|| list_length(cscan->custom_plans) != 1
		|| list_length(cscan->custom_private)
			!= kBioseqPerRowJoinPrivateFields
		|| node->custom_ps != NIL) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("BioseqPerRowMmsearch executor shape is invalid or parallel")));
	}
	for (TargetEntry* output : PList<TargetEntry>(
			cscan->scan.plan.targetlist)) {
		if (output != nullptr && output->expr != nullptr
			&& batch_join_expression_has_match_helper(
				reinterpret_cast<Node*>(output->expr),
				get_cached_stub_oids())) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqPerRowMmsearch executable projection retained a match helper")));
		}
	}
	auto* state = reinterpret_cast<BioseqPerRowJoinState*>(node);
	auto* runtime = std::construct_at(static_cast<BioseqPerRowJoinRuntime*>(
		MemoryContextAlloc(
			estate->es_query_cxt, sizeof(BioseqPerRowJoinRuntime))));
	state->runtime = runtime;
	runtime->cleanup_callback.func = per_row_join_runtime_cleanup;
	runtime->cleanup_callback.arg = state;
	MemoryContextRegisterResetCallback(
		estate->es_query_cxt, &runtime->cleanup_callback);
	runtime->cleanup_callback_registered = true;
	auto private_integer = [cscan](const int index) -> int {
		Node* value = reinterpret_cast<Node*>(
			list_nth(cscan->custom_private, index));
		if (value == nullptr || !IsA(value, Integer)) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqPerRowMmsearch executor integer ABI is invalid"),
				 errdetail("field=%d", index)));
		}
		return intVal(value);
	};
	const int version = private_integer(kPerRowVersion);
	const Oid target_relation_oid = static_cast<Oid>(
		private_integer(kPerRowTargetRelationOid));
	const AttrNumber target_storage_attno = static_cast<AttrNumber>(
		private_integer(kPerRowTargetStorageAttno));
	runtime->target_slot_resno = static_cast<AttrNumber>(
		private_integer(kPerRowTargetSlotResno));
	runtime->query_slot_resno = static_cast<AttrNumber>(
		private_integer(kPerRowQuerySlotResno));
	runtime->child_column_count = private_integer(kPerRowChildColumnCount);
	const int raw_alphabet = private_integer(kPerRowAlphabetCode);
	const int raw_width = private_integer(kPerRowStorageUnitByteWidth);
	const int raw_requirement = private_integer(kPerRowIndexReadRequirement);
	runtime->honest_startup_cost = batch_join_cost_from_node(
		reinterpret_cast<Node*>(list_nth(cscan->custom_private,
			kPerRowHonestStartupCost)), "honest_startup_cost");
	runtime->honest_total_cost = batch_join_cost_from_node(
		reinterpret_cast<Node*>(list_nth(cscan->custom_private,
			kPerRowHonestTotalCost)), "honest_total_cost");
	runtime->incumbent_cost_floor = batch_join_cost_from_node(
		reinterpret_cast<Node*>(list_nth(cscan->custom_private,
			kPerRowIncumbentCostFloor)), "incumbent_cost_floor");
	Node* sites_node = reinterpret_cast<Node*>(list_nth(
		cscan->custom_private, kPerRowPlaceholderSites));
	const bool sites_shape_valid = sites_node == nullptr || IsA(sites_node, List);
	List* sites = sites_node == nullptr
		? NIL : reinterpret_cast<List*>(sites_node);
	FuncExpr* mmsearch = pg_node_try_as<FuncExpr>(reinterpret_cast<Node*>(
		list_nth(cscan->custom_private, kPerRowMmsearchExpr)));
	if (version != kBioseqPerRowJoinPlanVersion
		|| !OidIsValid(target_relation_oid) || target_storage_attno <= 0
		|| runtime->target_slot_resno <= 0 || runtime->query_slot_resno <= 0
		|| runtime->child_column_count <= 0
		|| raw_alphabet < 0 || raw_alphabet > UINT8_MAX
		|| (raw_width != 1 && raw_width != 2
			&& raw_width != 4 && raw_width != 8)
		|| raw_requirement != static_cast<int>(
			bioseq::indexing::IndexReadRequirement::require_absent)
		|| !sites_shape_valid || mmsearch == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch executor private ABI is invalid")));
	}
	runtime->alphabet_code = static_cast<std::uint8_t>(raw_alphabet);
	runtime->storage_unit_byte_width = static_cast<std::uint8_t>(raw_width);
	TupleDesc scan_desc = node->ss.ss_ScanTupleSlot == nullptr
		? nullptr : node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	if (scan_desc == nullptr
		|| runtime->child_column_count > scan_desc->natts
		|| list_length(sites)
			!= scan_desc->natts - runtime->child_column_count
		|| runtime->target_slot_resno > runtime->child_column_count
		|| runtime->query_slot_resno > runtime->child_column_count) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch scan tuple layout is invalid")));
	}
	const int32 expected_typmod = compact_sequence_stdc_type_params{
		runtime->alphabet_code, runtime->storage_unit_byte_width}.encode();
	for (AttrNumber resno : {runtime->target_slot_resno,
			runtime->query_slot_resno}) {
		Form_pg_attribute attribute = TupleDescAttr(scan_desc, resno - 1);
		if (attribute->atttypid
				!= get_cached_stub_oids().bioseq_seq_type_oid
			|| attribute->atttypmod != expected_typmod) {
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqPerRowMmsearch operand type is invalid")));
		}
	}
	Bitmapset* placeholder_resnos = nullptr;
	for (List* encoded : PList<List>(sites)) {
		if (encoded == nullptr || !IsA(encoded, List)
			|| list_length(encoded) != 4) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqPerRowMmsearch placeholder ABI is invalid")));
		}
		for (int index = 0; index < 4; ++index) {
			Node* value = reinterpret_cast<Node*>(list_nth(encoded, index));
			if (value == nullptr || !IsA(value, Integer)) {
				ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("BioseqPerRowMmsearch placeholder scalar is invalid")));
			}
		}
		const int raw_resno = intVal(linitial(encoded));
		if (raw_resno <= runtime->child_column_count
			|| raw_resno > scan_desc->natts
			|| bms_is_member(raw_resno, placeholder_resnos)) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("BioseqPerRowMmsearch placeholder resno is invalid")));
		}
		placeholder_resnos = bms_add_member(placeholder_resnos, raw_resno);
		const BioseqBatchJoinPlaceholderSite site{
			.resno = static_cast<AttrNumber>(raw_resno),
			.funcid = static_cast<Oid>(intVal(lsecond(encoded))),
			.typmod = intVal(lthird(encoded)),
			.collation = static_cast<Oid>(intVal(lfourth(encoded))),
		};
		Form_pg_attribute attribute = TupleDescAttr(
			scan_desc, site.resno - 1);
		if (!is_bioseq_projection_funcid(
				site.funcid, get_cached_stub_oids())
			|| attribute->atttypid != get_func_rettype(site.funcid)
			|| attribute->atttypmod != site.typmod
			|| attribute->attcollation != site.collation) {
			ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("BioseqBatchJoin post-join recipe placeholder type is invalid")));
		}
		runtime->placeholder_sites.push_back(site);
	}
	bms_free(placeholder_resnos);

	bioseq::simd::active_target::Minimap2Params params =
		build_params_from_gucs();
	bioseq::indexing::IndexReadRequirement decoded_requirement =
		bioseq::indexing::IndexReadRequirement::allow;
	(void)apply_aligner_options_override(
		mmsearch, params, decoded_requirement, get_cached_stub_oids());
	finalize_simd_chaining_policy(params);
	if (decoded_requirement
			== bioseq::indexing::IndexReadRequirement::require_compatible_persisted) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("transformed mmsearch target cannot require a persisted index")));
	}
	params.do_cigar = false;
	params.compute_divergence = false;
	params.compute_mapq = false;
	params.compute_edit_distance = false;
	const BioseqStubOids& placeholder_oids = get_cached_stub_oids();
	for (auto const& site : runtime->placeholder_sites) {
		params.do_cigar = params.do_cigar
			|| site.funcid == placeholder_oids.cigar_oid
			|| site.funcid == placeholder_oids.has_cigar_oid
			|| site.funcid == placeholder_oids.edit_oid;
		params.compute_edit_distance = params.compute_edit_distance
			|| site.funcid == placeholder_oids.edit_oid;
		params.compute_divergence = params.compute_divergence
			|| site.funcid == placeholder_oids.divergence_oid;
		params.compute_mapq = params.compute_mapq
			|| site.funcid == placeholder_oids.mapq_oid;
	}
	Plan* child_plan = reinterpret_cast<Plan*>(linitial(cscan->custom_plans));
	runtime->child = ExecInitNode(child_plan, estate, eflags);
	runtime->child_initialized = true;
	node->custom_ps = list_make1(runtime->child);
	runtime->target_relation = table_open(target_relation_oid, NoLock);
	if (runtime->target_relation == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_TABLE),
			 errmsg("BioseqPerRowMmsearch target relation is unavailable")));
	}
	if (runtime->target_relation->rd_tableam
			!= GetBioseqColumnTableAmRoutine()) {
		ereport(ERROR,
			(errcode(ERRCODE_WRONG_OBJECT_TYPE),
			 errmsg("BioseqPerRowMmsearch target relation changed table AM")));
	}
	TupleDesc relation_desc = RelationGetDescr(runtime->target_relation);
	Form_pg_attribute storage_attribute =
		target_storage_attno > relation_desc->natts
		? nullptr : TupleDescAttr(relation_desc, target_storage_attno - 1);
	if (storage_attribute == nullptr || storage_attribute->attisdropped
		|| storage_attribute->atttypid
			!= get_cached_stub_oids().bioseq_seq_type_oid
		|| storage_attribute->atttypmod != expected_typmod) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("BioseqPerRowMmsearch target storage column changed")));
	}
	runtime->buffer.configure_params(
		params, bioseq::indexing::IndexReadRequirement::require_absent,
		/*read_sequence_index=*/false, target_relation_oid,
		runtime->target_relation->rd_locator.dbOid,
		runtime->target_relation->rd_locator.spcOid,
		runtime->target_relation->rd_locator.relNumber,
		runtime->target_relation, estate->es_snapshot);
	char* column_name = get_attname(
		target_relation_oid, target_storage_attno, /*missing_ok=*/false);
	runtime->buffer.configure_mmsearch_per_row(
		runtime->target_slot_resno, std::string{column_name},
		runtime->query_slot_resno,
		runtime->target_slot_resno == runtime->query_slot_resno,
		runtime->alphabet_code, runtime->storage_unit_byte_width,
		InvalidAttrNumber);
	pfree(column_name);
	const int plan_node_id = cscan->scan.plan.plan_node_id;
	if (plan_node_id < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch plan node id is invalid")));
	}
	runtime->alignment_registration =
		bioseq::pg::alignment_execution::bind_alignment_node(
			estate, static_cast<std::uint32_t>(plan_node_id));
	runtime->buffer.bind_persisted_gpu_operation_evidence_sink(
		*bioseq::pg::alignment_execution::
			borrow_persisted_gpu_operation_evidence_sink(
				runtime->alignment_registration));
}

static void BioseqPerRowJoin_BeginCustomScan(
		CustomScanState* node, EState* estate, int eflags) {
	MemoryContext saved_context = CurrentMemoryContext;
	ErrorData* volatile original_error = nullptr;
	ErrorData* volatile converted_pg_error = nullptr;
	std::exception_ptr cpp_error;
	PG_TRY();
	{
		try {
			BioseqPerRowJoin_BeginCustomScanImpl(node, estate, eflags);
		} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
			converted_pg_error = error.release_error_data();
		} catch (...) {
			cpp_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		original_error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	auto abandon = [node]() noexcept {
		if (BioseqPerRowJoinRuntime* runtime = per_row_join_runtime(node);
			runtime != nullptr) {
			per_row_join_abandon_after_error(node, *runtime);
			if (!runtime->cleanup_callback_registered) {
				auto* state = reinterpret_cast<BioseqPerRowJoinState*>(node);
				state->runtime = nullptr;
				std::destroy_at(runtime);
			}
		}
	};
	if (original_error != nullptr) {
		abandon();
		ErrorData* exact = original_error;
		ReThrowError(exact);
	}
	if (converted_pg_error != nullptr) {
		abandon();
		ErrorData* exact = converted_pg_error;
		ReThrowError(exact);
	}
	if (cpp_error != nullptr) {
		abandon();
		char detail[512] = "unknown C++ exception";
		try {
			std::rethrow_exception(cpp_error);
		} catch (const std::exception& error) {
			strlcpy(detail,
				error.what() == nullptr ? "std::exception without detail"
					: error.what(),
				sizeof(detail));
		} catch (...) {
		}
		cpp_error = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin post-join recipe Begin raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
}

[[nodiscard]] static TupleTableSlot* per_row_join_project_output(
		CustomScanState* node, BioseqPerRowJoinRuntime& runtime,
		bioseq::scheduler::DispatchNextOutput const& output) {
	if (output.scan_slot_copy == nullptr || output.match == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqPerRowMmsearch output references invalid input")));
	}
	ExprContext* context = node->ss.ps.ps_ExprContext;
	if (context == nullptr || context->ecxt_per_tuple_memory == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin post-join recipe expression context is missing")));
	}
	ResetExprContext(context);
	TupleTableSlot* combined = node->ss.ss_ScanTupleSlot;
	ExecClearTuple(combined);
	slot_getallattrs(output.scan_slot_copy);
	if (runtime.child_column_count
			> output.scan_slot_copy->tts_tupleDescriptor->natts
		|| runtime.child_column_count
			> combined->tts_tupleDescriptor->natts) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch output tuple layout changed")));
	}
	for (int index = 0; index < runtime.child_column_count; ++index) {
		combined->tts_values[index] = output.scan_slot_copy->tts_values[index];
		combined->tts_isnull[index] = output.scan_slot_copy->tts_isnull[index];
	}
	const BioseqStubOids& oids = get_cached_stub_oids();
	for (auto const& site : runtime.placeholder_sites) {
		batch_join_set_placeholder_value(
			combined, site, *output.match, oids,
			context->ecxt_per_tuple_memory);
	}
	ExecStoreVirtualTuple(combined);
	context->ecxt_scantuple = combined;
	return node->ss.ps.ps_ProjInfo != nullptr
		? ExecProject(node->ss.ps.ps_ProjInfo) : combined;
}

static TupleTableSlot* BioseqPerRowJoin_ExecCustomScanImpl(
		CustomScanState* node) {
	BioseqPerRowJoinRuntime* runtime = per_row_join_runtime(node);
	if (runtime == nullptr || runtime->child == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch runtime is missing")));
	}
	bioseq::pg::alignment_execution::ensure_alignment_node_registered(
		runtime->alignment_registration);
	for (;;) {
		if (runtime->buffer.has_finished_writing()) {
			auto output = runtime->buffer.read_next_output();
			if (output.has_value()) {
				return per_row_join_project_output(node, *runtime, *output);
			}
			if (runtime->source_exhausted) {
				if (!runtime->pass_fully_drained) {
					bioseq::pg::alignment_execution::
						mark_alignment_pass_fully_drained(
							runtime->alignment_registration);
					runtime->pass_fully_drained = true;
				}
				return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
			}
			runtime->buffer.clear_slots();
			continue;
		}
		TupleTableSlot* child_slot = ExecProcNode(runtime->child);
		if (TupIsNull(child_slot)) {
			runtime->source_exhausted = true;
			if (runtime->buffer.run_batch_and_build_cursors()) {
				auto delta = runtime->buffer.execution_stats();
				runtime->aggregate_stats.merge(delta);
				bioseq::pg::alignment_execution::merge_alignment_batch(
					runtime->alignment_registration, delta);
			}
			continue;
		}
		bool target_is_null = false;
		bool query_is_null = false;
		const Datum target = slot_getattr(child_slot,
			runtime->target_slot_resno, &target_is_null);
		const Datum query = slot_getattr(child_slot,
			runtime->query_slot_resno, &query_is_null);
		if (target_is_null || query_is_null) continue;
		slice_async_owner_guard* async_owner = nullptr;
		for (const Datum operand : std::array{target, query}) {
			auto* attachment = pg_sequence_trace_attachment_for_datum(operand);
			auto* candidate = borrow_slice_async_owner_noexcept(attachment);
			if (candidate == nullptr) continue;
			if (async_owner != nullptr && async_owner != candidate) {
				ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("BioseqPerRowMmsearch operands have distinct async owners")));
			}
			async_owner = candidate;
		}
		if (async_owner != nullptr) {
			runtime->buffer.bind_slice_async_owner(async_owner);
		}
		runtime->buffer.submit_scan(child_slot);
		if (runtime->buffer.ready_to_dispatch()) {
			if (runtime->buffer.run_batch_and_build_cursors()) {
				auto delta = runtime->buffer.execution_stats();
				runtime->aggregate_stats.merge(delta);
				bioseq::pg::alignment_execution::merge_alignment_batch(
					runtime->alignment_registration, delta);
			}
		}
	}
}

static TupleTableSlot* BioseqPerRowJoin_ExecCustomScan(
		CustomScanState* node) {
	TupleTableSlot* result = nullptr;
	MemoryContext saved_context = CurrentMemoryContext;
	ErrorData* volatile original_error = nullptr;
	ErrorData* volatile converted_pg_error = nullptr;
	std::exception_ptr cpp_error;
	PG_TRY();
	{
		try {
			result = BioseqPerRowJoin_ExecCustomScanImpl(node);
		} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
			converted_pg_error = error.release_error_data();
		} catch (...) {
			cpp_error = std::current_exception();
		}
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		original_error = CopyErrorData();
		FlushErrorState();
	}
	PG_END_TRY();
	auto abandon = [node]() noexcept {
		if (BioseqPerRowJoinRuntime* runtime = per_row_join_runtime(node);
			runtime != nullptr) {
			per_row_join_abandon_after_error(node, *runtime);
		}
	};
	if (original_error != nullptr) {
		abandon();
		ErrorData* exact = original_error;
		ReThrowError(exact);
	}
	if (converted_pg_error != nullptr) {
		abandon();
		ErrorData* exact = converted_pg_error;
		ReThrowError(exact);
	}
	if (cpp_error != nullptr) {
		abandon();
		char detail[512] = "unknown C++ exception";
		try {
			std::rethrow_exception(cpp_error);
		} catch (const std::exception& error) {
			strlcpy(detail,
				error.what() == nullptr ? "std::exception without detail"
					: error.what(),
				sizeof(detail));
		} catch (...) {
		}
		cpp_error = nullptr;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchJoin post-join recipe Exec raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	return result;
}

static void BioseqPerRowJoin_EndCustomScan(CustomScanState* node) {
	BioseqPerRowJoinRuntime* runtime = per_row_join_runtime(node);
	if (runtime == nullptr || runtime->cleanup_done) return;
	try {
		per_row_join_release_checked(node, *runtime);
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		per_row_join_abandon_after_error(node, *runtime);
		ErrorData* exact = error.release_error_data();
		ReThrowError(exact);
	} catch (const std::exception& error) {
		per_row_join_abandon_after_error(node, *runtime);
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch End raised a C++ exception"),
			 errdetail_internal("%s", error.what())));
	}
}

static void BioseqPerRowJoin_ReScanCustomScan(CustomScanState* node) {
	BioseqPerRowJoinRuntime* runtime = per_row_join_runtime(node);
	if (runtime == nullptr || runtime->child == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqPerRowMmsearch rescan runtime is missing")));
	}
	bioseq::pg::alignment_execution::mark_alignment_rescan(
		runtime->alignment_registration);
	runtime->buffer.clear_slots();
	runtime->source_exhausted = false;
	runtime->pass_fully_drained = false;
	ExecReScan(runtime->child);
}

static void BioseqPerRowJoin_ExplainCustomScan(
		CustomScanState* node, List*, ExplainState* es) {
	BioseqPerRowJoinRuntime* runtime = per_row_join_runtime(node);
	ExplainPropertyText("Bioseq Mmsearch Ownership", "per-row recipe", es);
	ExplainPropertyText("Bioseq Reconstructed Join", "child plan", es);
	ExplainPropertyInteger("Bioseq Batch Capacity", nullptr,
		static_cast<std::int64_t>(kBioseqRuntimeBufferReserve), es);
	ExplainPropertyText("Bioseq Index Read Requirement",
		"require_absent (transformed target)", es);
	if (runtime == nullptr) return;
	ExplainPropertyFloat("Bioseq Honest Startup Cost", nullptr,
		runtime->honest_startup_cost, 2, es);
	ExplainPropertyFloat("Bioseq Honest Total Cost", nullptr,
		runtime->honest_total_cost, 2, es);
	ExplainPropertyFloat("Bioseq Incumbent Cost Floor", nullptr,
		runtime->incumbent_cost_floor, 2, es);
	ExplainPropertyInteger("Bioseq Match Placeholder Sites", nullptr,
		static_cast<std::int64_t>(runtime->placeholder_sites.size()), es);
	ExplainPipelineExecutionStats(runtime->aggregate_stats, es);
}

/*
 * DOC0279 §12.5 — per-tuple populate for every `bioseq.symbol_count(Var, 'L')`
 * placeholder. For the current output row (resolved by columnar rowno) it
 * random-access reads the SEQ column header, then for each site:
 *   (2) if bioseq.read_sequence_index = on, tries the stored INLINE
 *       SymbolCountsIndex chunk (one heap PK lookup, the only thing the GUC
 *       gates here);
 *   (3) otherwise UNCONDITIONALLY recomputes tally_symbol_counts over the row
 *       (scanner_pg-streamed for a stored stub, in-memory for an inline value).
 * The recompute guarantees a non-NULL answer for every present row regardless
 * of the GUC — the GUC only chooses whether the stored chunk is preferred.
 * Replaces the deleted meta_sidecar / 49-stub path.
 */
struct SymbolCountReadResources {
	MemoryContext caller_context = nullptr;
	MemoryContext scratch = nullptr;
	bioseq::storage::seq_caches* caches = nullptr;
	BioseqColumnReadState* row_read_state = nullptr;
};

static void
release_symbol_count_read_resources_checked(
	SymbolCountReadResources* resources)
{
	if (resources == nullptr) {
		return;
	}
	if (resources->caller_context != nullptr
		&& CurrentMemoryContext != resources->caller_context) {
		bioseq::pg_utils::invoke_pg_as_cpp([resources] {
			MemoryContextSwitchTo(resources->caller_context);
		});
	}
	if (resources->row_read_state != nullptr) {
		BioseqColumnReadState* finished = resources->row_read_state;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[finished] { BioseqColumnEndRead(finished); });
		resources->row_read_state = nullptr;
	}
	if (resources->caches != nullptr) {
		bioseq::storage::seq_caches* finished = resources->caches;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[finished] { finished->close_all(/*commit=*/false); });
		bioseq::pg_utils::invoke_pg_as_cpp([finished] {
			bioseq::storage::destroy_seq_caches(finished);
		});
		resources->caches = nullptr;
	}
	if (resources->scratch != nullptr) {
		MemoryContext finished = resources->scratch;
		bioseq::pg_utils::invoke_pg_as_cpp(
			[finished] { MemoryContextDelete(finished); });
		resources->scratch = nullptr;
	}
}

static void
abandon_symbol_count_read_resources_after_error(
	SymbolCountReadResources* resources) noexcept
{
	if (resources == nullptr) {
		return;
	}
	// PostgreSQL's transaction/ResourceOwner will release readers, relations,
	// and palloc contexts.  Do not issue any PG call while preserving the
	// original ErrorData; only discharge ordinary C++ heap ownership.
	resources->row_read_state = nullptr;
	if (resources->caches != nullptr) {
		bioseq::storage::seq_caches* abandoned = std::exchange(
			resources->caches, nullptr);
		for (auto& [key, entry] : abandoned->entries) {
			(void)key;
			entry.rel = nullptr;
			entry.write_state = nullptr;
			entry.read_state = nullptr;
			entry.read_snapshot_source = nullptr;
			entry.pending_bytes_delta = 0;
			entry.pending_total_payload_delta = 0;
		}
		abandoned->entries.clear();
		abandoned->~seq_caches();
	}
	resources->scratch = nullptr;
}

[[noreturn]] static void symbol_count_error_as_cpp(
		int sqlstate,
		std::string_view message) {
	bioseq::pg_utils::invoke_pg_as_cpp([sqlstate, message] {
		ereport(ERROR,
			(errcode(sqlstate),
			 errmsg("%.*s", static_cast<int>(message.size()),
				message.data())));
	});
	pg_unreachable();
}

static void
populate_symbol_count_placeholders(BioseqPlaceholderConsts& consts,
								   Relation main, int64_t rowno,
								   Snapshot registered_snapshot)
{
	if (consts.symbol_count_sites.empty()) {
		return;
	}
	SymbolCountReadResources* resources = nullptr;
	ErrorData* exact_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char detail[512] = "std::exception without detail";
	try {
	if (registered_snapshot == nullptr
		|| !IsMVCCSnapshot(registered_snapshot)
		|| registered_snapshot->regd_count == 0) {
		symbol_count_error_as_cpp(
			ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
			"bioseq symbol-count read requires a registered MVCC executor snapshot");
	}
	auto set_all_null = [&]() {
		for (auto& s : consts.symbol_count_sites)
			if (s.placeholder != nullptr) s.placeholder->constisnull = true;
	};
	if (rowno < 0 || main == nullptr) { set_all_null(); return; }

	namespace bsi = bioseq::storage::indexing;
	const Oid relid = RelationGetRelid(main);
	const auto rid = bsi::index_relation_identity(relid);
	TupleDesc td   = RelationGetDescr(main);

	/* Mutable ownership lives in this record so ordinary C++ catches can either
	 * checked-close it on success or abandon PG handles without replacing the
	 * original ErrorData. */
	resources = bioseq::pg_utils::invoke_pg_as_cpp([] {
		return static_cast<SymbolCountReadResources*>(
			palloc0(sizeof(SymbolCountReadResources)));
	});
	resources->caller_context = CurrentMemoryContext;
	resources->scratch = bioseq::pg_utils::invoke_pg_as_cpp([] {
		return AllocSetContextCreate(CurrentMemoryContext,
			"bioseq_symcount", ALLOCSET_SMALL_SIZES);
	});
	resources->caches = bioseq::pg_utils::invoke_pg_as_cpp(
		[resources] {
			return bioseq::storage::alloc_seq_caches(resources->scratch);
		});
	resources->caches->side = bioseq::storage::seq_caches::Side::Read;
	resources->caches->lock_mode = AccessShareLock;
	resources->caches->read_state_mctx = resources->scratch;
	resources->caches->main_database_oid = rid.db_oid;
	resources->caches->main_tablespace_oid = rid.tablespace_oid;
	resources->caches->main_relfilenode = rid.relfilenode;

	auto* vals = bioseq::pg_utils::invoke_pg_as_cpp([resources, td] {
		return static_cast<Datum*>(MemoryContextAllocZero(
			resources->scratch,
			sizeof(Datum) * static_cast<std::size_t>(td->natts)));
	});
	auto* nulls = bioseq::pg_utils::invoke_pg_as_cpp([resources, td] {
		return static_cast<bool*>(MemoryContextAllocZero(
			resources->scratch,
			sizeof(bool) * static_cast<std::size_t>(td->natts)));
	});
		/* Project every distinct site column and keep the transient List plus
		 * direct read state inside scratch. Catch restores caller_context if an
		 * ereport jumps over this explicit context switch. */
		MemoryContext previous_context =
			bioseq::pg_utils::invoke_pg_as_cpp([resources] {
				return MemoryContextSwitchTo(resources->scratch);
			});
		List* projected = bioseq::pg_utils::invoke_pg_as_cpp([&consts] {
			List* result = NIL;
			for (auto& site : consts.symbol_count_sites) {
				if (site.placeholder != nullptr
					&& !list_member_int(result, site.column_attno)) {
					result = lappend_int(result, site.column_attno);
				}
			}
			return result;
		});
		resources->row_read_state = bioseq::pg_utils::invoke_pg_as_cpp(
			[&] {
				return BioseqColumnBeginRead(
					main, td, projected, NIL, resources->scratch,
					registered_snapshot,
					/*randomAccess=*/true, nullptr);
			});
		bioseq::pg_utils::invoke_pg_as_cpp([previous_context] {
			MemoryContextSwitchTo(previous_context);
		});
		const bool row_ok = bioseq::pg_utils::invoke_pg_as_cpp([&] {
			return BioseqColumnReadRowByRowNumber(
				resources->row_read_state, static_cast<uint64_t>(rowno),
				vals, nulls);
		});
		/* vals contains by-reference Datums owned by row_read_state's
		 * stripeReadContext.  Keep the reader alive until every symbol-count
		 * site has detoasted/consumed those values; the unified release below
		 * ends it on both success and ERROR. */

		for (auto& site : consts.symbol_count_sites) {
			if (site.placeholder == nullptr) continue;
			if (!row_ok || site.rank < 0 || nulls[site.column_attno - 1]) {
				site.placeholder->constisnull = true;
				continue;
			}
			MemoryContext old = bioseq::pg_utils::invoke_pg_as_cpp(
				[resources] {
					return MemoryContextSwitchTo(resources->scratch);
				});
			pgseq_t* hdr = bioseq::pg_utils::invoke_pg_as_cpp([&] {
				return reinterpret_cast<pgseq_t*>(
					PG_DETOAST_DATUM(vals[site.column_attno - 1]));
			});
			bioseq::pg_utils::invoke_pg_as_cpp(
				[old] { MemoryContextSwitchTo(old); });
			const std::uint8_t alpha = hdr->alphabet_code;
			const bool stub = bioseq::storage::is_locator_stub(hdr);
			const std::uint8_t bw = stub
				? bioseq::storage::logical_storage_unit_byte_width(hdr)
				: hdr->storage_unit_byte_width;
			bioseq::storage::seq_locator loc{};
			if (stub) loc = bioseq::storage::decode_locator_from_stub(hdr);

			/* (2) stored read — the ONLY thing read_sequence_index gates here. */
			std::optional<std::int64_t> stored;
			if (bioseq_read_sequence_index && stub) {
				auto store = bsi::open_sequence_storage(
						relid, rid.db_oid, rid.tablespace_oid,
						rid.relfilenode, site.column_attno,
						bioseq::indexing::SymbolCountsIndex<
							bsi::IndexStorageSequencePg>::index_name,
						loc, static_cast<uint64_t>(rowno),
						resources->caches, resources->scratch);
				try {
					stored = bioseq::indexing::SymbolCountsIndex<
						bsi::IndexStorageSequencePg>::read_count(
							store, static_cast<std::size_t>(site.rank));
					store.close_checked();
				} catch (...) {
					store.abandon_after_pg_error_noexcept();
					throw;
				}
			}
			if (stored.has_value()) {
				site.placeholder->constvalue =
					bioseq::pg_utils::pg_datum::from(
						static_cast<std::int64_t>(*stored)).raw();
				site.placeholder->constisnull = false;
				continue;
			}

			/* (3) unconditional recompute fallback (never gated by the GUC). */
			std::int64_t cnt = 0;
			pgseq_t::dispatch_instance(alpha, bw, [&]<class S>() {
				if (stub) {
					using SourceBinding =
						bioseq::storage::postgres_lazy_sequence_binding<S>;
					const SourceBinding source_binding{
						rid.db_oid, rid.tablespace_oid, rid.relfilenode,
						site.column_attno, loc,
						SourceBinding::expected_last_fragment_padding(loc),
						registered_snapshot, resources->caches,
						resources->scratch};
					bioseq::storage::with_lazy_postgres<S>(
						source_binding, [&site, &cnt](auto lazy_view) {
							auto counts =
								bioseq::indexing::tally_symbol_counts(lazy_view);
							if (static_cast<std::size_t>(site.rank)
								< counts.size())
								cnt = counts[site.rank];
						});
				} else {
					compact_sequence_range<S> range{*hdr};
					auto lazy = bioseq::storage::make_lazy_in_memory(range);
					const auto lazy_view =
						bioseq::storage::as_lazy_sequence_view(lazy);
					auto counts =
						bioseq::indexing::tally_symbol_counts(lazy_view);
					if (static_cast<std::size_t>(site.rank) < counts.size())
						cnt = counts[site.rank];
				}
			});
			site.placeholder->constvalue =
				bioseq::pg_utils::pg_datum::from(cnt).raw();
			site.placeholder->constisnull = false;
		}
		release_symbol_count_read_resources_checked(resources);
		bioseq::pg_utils::invoke_pg_as_cpp(
			[resources] { pfree(resources); });
		resources = nullptr;
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(detail,
			error.what() == nullptr ? "std::exception without detail"
				: error.what(),
			sizeof(detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	if (exact_error != nullptr || cpp_error || unknown_cpp_error) {
		abandon_symbol_count_read_resources_after_error(resources);
	}
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq symbol-count read raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq symbol-count read raised an unknown C++ exception")));
	}
}


static void promote_slice_project_sources(
		BioseqScanState* state, TupleTableSlot* slot,
		MemoryContext result_context,
		const bool allow_locator_passthrough) {
	BioseqScanRuntime& runtime = bioseq_runtime(state);
	if (!runtime.lazy_source_projection
		|| (runtime.locator_passthrough && !allow_locator_passthrough)) return;
	if (runtime.lazy_source_provider == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("BioseqSliceProject source provider is not initialized")));
	}
	slot_getallattrs(slot);
	const Oid seq_oid = get_cached_stub_oids().bioseq_seq_type_oid;
	if (result_context == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
			 errmsg("BioseqSliceProject result context is unavailable")));
	}
	for (int index = 0; index < slot->tts_tupleDescriptor->natts; ++index) {
		if (slot->tts_isnull[index]
			|| TupleDescAttr(slot->tts_tupleDescriptor, index)->atttypid
				!= seq_oid) continue;
		auto* value = reinterpret_cast<const pgseq_t*>(
			DatumGetPointer(slot->tts_values[index]));
		if (!bioseq::storage::is_locator_stub(value)) continue;
		const AttrNumber resno = static_cast<AttrNumber>(index + 1);
		const std::uint32_t producer_id =
			static_cast<std::size_t>(resno)
					< runtime.lazy_source_producer_ids.size()
				? runtime.lazy_source_producer_ids[resno] : 0u;
		const AttrNumber column_attno =
			static_cast<std::size_t>(resno)
					< runtime.lazy_source_column_attnos.size()
				? runtime.lazy_source_column_attnos[resno] : InvalidAttrNumber;
		if (column_attno <= 0) {
			ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("BioseqSliceProject locator lost its physical column"),
				 errdetail("result_attribute=%d", static_cast<int>(resno))));
		}
		slot->tts_values[index] =
			bioseq::storage::promote_pg_sequence_locator(
				runtime.lazy_source_provider,
				column_attno, slot->tts_values[index], result_context,
				runtime.lazy_trace_attachment, producer_id);
	}
}

static TupleTableSlot *
Inner_CustomExecScan(BioseqScanState *bioseqScanState,
			   ExecScanAccessMtd accessMtd,
			   ExecScanRecheckMtd recheckMtd)
{
	bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() entered");
	ExprContext *econtext;
	ExprState  *qual;

	ScanState *node = reinterpret_cast<ScanState *>(&bioseqScanState->custom_scanstate.ss);
	BioseqScanRuntime& runtime = bioseq_runtime(bioseqScanState);

	qual     = node->ps.qual;
	econtext = node->ps.ps_ExprContext;

	if (bioseqScanState->vectorization.vectorizationAggregate)
	{
		ExecClearTuple(bioseqScanState->vectorization.resultVectorSlot);
		CleanupVectorSlot(reinterpret_cast<VectorTupleTableSlot *>(
							bioseqScanState->vectorization.resultVectorSlot));
	}

	/*
	 * Two-phase state machine:
	 *
	 *   Phase 1 (writing): pull tuples from the access method, run qual,
	 *                      buffer passing tuples via buffer.submit_scan.
	 *                      When source is exhausted, run batch processor
	 *                      and transition.
	 *   Phase 2 (reading): drain buffered output entries, setting
	 *                      placeholder const nodes from MatchResults and
	 *                      projecting via ExecProject.
	 *
	 * buffer.has_finished_writing() flips from false → true at the
	 * transition.
	 */
	for (;;)
	{
		/* ---- Phase 2: drain buffered output ---- */
		if (runtime.buffer.has_finished_writing())
		{
			bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() draining output cursor");
				auto next = runtime.buffer.read_next_output();
				if (!next)
				{
					if (runtime.source_exhausted) {
						bioseq::pg::alignment_execution::
							mark_alignment_pass_fully_drained(
								runtime.alignment_registration);
						bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() final output cursor drained; returning empty slot (EOF)");
						return ExecClearTuple(node->ps.ps_ResultTupleSlot);
					}
					/* A capacity rollover completed. Drop only that physical
					 * batch and resume the still-live access-method scan. */
					runtime.buffer.clear_slots();
					continue;
				}

			if (runtime.edit_predicate_active
				&& (runtime.edit_exclusive_upper_bound <= 0
					|| next->match->edit_distance
						>= static_cast<std::uint64_t>(
							runtime.edit_exclusive_upper_bound))) {
				/* SneakySnake is only a lower-bound gate.  Every survivor is
				 * aligned, and this exact comparison is the authoritative SQL
				 * predicate (strict `<`, including the boundary case). */
				InstrCountFiltered1(node, 1);
				continue;
			}

			/*
			 * Update every placeholder Const from the MatchResult. The
			 * subsequent ExecProject reads the current constvalue of
			 * every Const node referenced in newtargetlist.
			 */
			populate_placeholder_consts(runtime.placeholder_consts, *next->match);

			/*
			 * symbol_count placeholders are independent of MatchResult;
			 * they are resolved from meta_sidecar by (relid, rowno) so
			 * we populate them alongside the per-match fields.
			 */
			populate_symbol_count_placeholders(runtime.placeholder_consts,
											   node->ss_currentRelation,
											   next->sequence_rowno,
											   bioseqScanState->snapshot);

			/*
			 * Re-install the buffered scan slot as the current scan
			 * tuple so Var references (e.g. `a`, `c`) resolve off the
			 * right row.
			 */
			econtext->ecxt_scantuple = next->scan_slot_copy;

			ProjectionInfo* proj = ExecBuildProjectionInfo(
				runtime.newtargetlist,
				econtext,
				node->ps.ps_ResultTupleSlot,
				&(node->ps),
				next->scan_slot_copy->tts_tupleDescriptor);

			return ExecProject(proj);
		}

		/* ---- Phase 1: writing ---- */
		TupleTableSlot *slot = NULL;

		/*
		 * Scalar fetch path: pull one scan tuple and run qual on it.
		 */
		if (!bioseqScanState->vectorization.vectorizationEnabled)
		{
			bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() scalar fetch via ExecScanFetch");
			slot = ExecScanFetch(node, accessMtd, recheckMtd);

				if (TupIsNull(slot))
				{
					runtime.source_exhausted = true;
					bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() source exhausted; running batch processor");
				if (runtime.buffer.run_batch_and_build_cursors()) {
					bioseq::pg::alignment_execution::merge_alignment_batch(
						runtime.alignment_registration,
						runtime.buffer.execution_stats());
				}
				continue; /* Next iteration enters Phase 2. */
			}
		}
		/*
		 * Vectorized fetch path: pull a vector slot, apply vectorized
		 * qual mask, then extract a single scalar tuple per iteration
		 * until the vector is exhausted.
		 */
		else
		{
			/*
			 * First vector pull or after previous vector drained.
			 */
			if (bioseqScanState->vectorization.vectorPendingRowNumber == 0)
			{
				bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() vectorized fetch (initial vector pull)");
				slot = ExecScanFetch(node, accessMtd, recheckMtd);

					if (TupIsNull(slot))
					{
						runtime.source_exhausted = true;
						bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() vectorized source exhausted; running batch processor");
					if (runtime.buffer.run_batch_and_build_cursors()) {
						bioseq::pg::alignment_execution::merge_alignment_batch(
							runtime.alignment_registration,
							runtime.buffer.execution_stats());
					}
					continue;
				}

				if (bioseqScanState->vectorization.vectorizedQualList != NULL)
				{
					if (bioseqScanState->vectorization.constructedVectorizedQualList == NULL)
					{
						bioseqScanState->vectorization.constructedVectorizedQualList =
							ConstructVectorizedQualList(slot, bioseqScanState->vectorization.vectorizedQualList);
					}

					VectorTupleTableSlot *vectorSlot = reinterpret_cast<VectorTupleTableSlot *>(slot);
					bool *resultQual = ExecuteVectorizedQual(
						slot,
						bioseqScanState->vectorization.constructedVectorizedQualList,
						AND_EXPR, econtext);
					memcpy(vectorSlot->keep, resultQual, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);
				}
			}

			/*
			 * Extract one kept row from the vector slot into the scalar
			 * scan slot. Skip entries where keep[i] is false.
			 */
			bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() vectorized extraction");
			VectorTupleTableSlot *vectorSlot = reinterpret_cast<VectorTupleTableSlot *>(
				bioseqScanState->vectorization.scanVectorSlot);

			bool rowFound = false;
			while (bioseqScanState->vectorization.vectorPendingRowNumber != 0 && !rowFound)
			{
				if (vectorSlot->keep[bioseqScanState->vectorization.vectorRowIndex])
				{
					slot = bioseqScanState->custom_scanstate.ss.ss_ScanTupleSlot;
					ExecClearTuple(slot);
					ExtractTupleFromVectorSlot(slot,
											   vectorSlot,
											   bioseqScanState->vectorization.vectorRowIndex,
											   bioseqScanState->vectorization.attrNeededList);

					uint64_t rowNumber = vectorSlot->rowNumber[bioseqScanState->vectorization.vectorRowIndex];
					slot->tts_tid = row_number_to_tid(rowNumber);
					rowFound = true;
				}
				bioseqScanState->vectorization.vectorPendingRowNumber--;
				bioseqScanState->vectorization.vectorRowIndex++;
			}

			/*
			 * Vector exhausted without finding a kept row: go back to
			 * pull another vector or finalize.
			 */
			if (!rowFound)
				continue;
		}

		/*
		 * DOC0260 §9 — the scan runs with defer_seq_hydration=true, so `slot`
		 * carries 40-byte locator stubs. When there is an mmsearch qual the
		 * row is buffered (submit_scan captures the locator; the batch path
		 * bulk-hydrates per bucket), so we leave the stub in place. Without an
		 * mmsearch qual the row is projected immediately below, so hydrate the
		 * SEQ columns now (force=true overrides the defer flag) — this covers
		 * both the scalar and vectorized convergence and is the analogue of
		 * the inline hydration bioseq_getnextslot performs on non-deferred
		 * scan paths.
		 */
			if (!runtime.buffer.mmsearch_active()
				&& !runtime.locator_passthrough
				&& !runtime.lazy_source_projection) {
				bioseq_hydrate_scan_slot(node->ss_currentScanDesc, slot, /*force=*/true);
			}
			if (!runtime.buffer.mmsearch_active()
				&& runtime.lazy_source_projection
				&& !runtime.locator_passthrough) {
				promote_slice_project_sources(
					bioseqScanState, slot,
					bioseqScanState->custom_scanstate.ss.ps.
						ps_ExprContext->ecxt_per_tuple_memory,
					/*allow_locator_passthrough=*/false);
			}

		/*
		 * Place the current tuple into the expr context and run the
		 * non-vectorized qual (e.g. mmsearch()). Both scalar and
		 * vectorized paths converge here.
		 */
		econtext->ecxt_scantuple = slot;

		bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() applying qual filter");
		if (qual == NULL || ExecQual(qual, econtext))
		{
			if (runtime.mmsearch_null_input) {
				/* mmsearch is strict: a NULL owned query yields no rows, while
				 * ordinary sibling quals above retain their normal evaluation. */
				continue;
			}
				if (runtime.buffer.mmsearch_active()) {
					bool target_is_null = false;
					(void)slot_getattr(
						econtext->ecxt_scantuple,
						runtime.match_sequence_attno,
						&target_is_null);
					if (target_is_null) {
						continue;
					}
					bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() qual passed; buffering scan tuple");
					runtime.buffer.submit_scan(econtext->ecxt_scantuple);
					if (runtime.buffer.ready_to_dispatch()) {
						if (runtime.buffer.run_batch_and_build_cursors()) {
							bioseq::pg::alignment_execution::merge_alignment_batch(
								runtime.alignment_registration,
								runtime.buffer.execution_stats());
						}
					}
				}
			else {
				/*
				 * No WHERE mmsearch(...) qual installed -> there is nothing
				 * to batch. Project the scan tuple to the CustomScan target
				 * list so the output slot layout matches what an outer Result
				 * node expects. PlanCustomPath filters this target list to
				 * plain Vars plus bioseq-owned placeholders; ordinary
				 * expressions such as seq::text and bioseq.len(seq) remain in
				 * the outer Result and evaluate against this projected tuple.
				 */
				bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() qual passed; no mmsearch qual");

				/*
				 * symbol_count fast-path requires the row number; the
				 * access method set it on slot->tts_tid above. Convert
				 * once per projected row rather than stashing a
				 * (relid, rowno) in a shared struct (which was the
				 * anti-pattern this module used to carry).
				 */
				{
					int64_t row_number = -1;
					if (ItemPointerIsValid(&slot->tts_tid)) {
						row_number = static_cast<int64_t>(tid_to_row_number(slot->tts_tid));
					}
					populate_symbol_count_placeholders(runtime.placeholder_consts,
											   node->ss_currentRelation, row_number,
											   bioseqScanState->snapshot);
				}

				ProjectionInfo* proj = ExecBuildProjectionInfo(
					runtime.newtargetlist,
					econtext,
					node->ps.ps_ResultTupleSlot,
					&(node->ps),
					slot->tts_tupleDescriptor);
					TupleTableSlot* projected = ExecProject(proj);
					if (runtime.locator_passthrough) {
						if (!ItemPointerIsValid(&slot->tts_tid)
							|| runtime.locator_rowno_resno <= 0
							|| runtime.locator_rowno_resno
								> projected->tts_tupleDescriptor->natts) {
							ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("Bioseq locator passthrough row-number projection is invalid")));
						}
						Form_pg_attribute rowno_attribute = TupleDescAttr(
							projected->tts_tupleDescriptor,
							runtime.locator_rowno_resno - 1);
						if (rowno_attribute->atttypid != INT8OID) {
							ereport(ERROR,
								(errcode(ERRCODE_DATATYPE_MISMATCH),
								 errmsg("Bioseq locator passthrough row-number column is not bigint")));
						}
						slot_getallattrs(projected);
						projected->tts_values[
							runtime.locator_rowno_resno - 1] =
							Int64GetDatum(static_cast<std::int64_t>(
								tid_to_row_number(slot->tts_tid)));
						projected->tts_isnull[
							runtime.locator_rowno_resno - 1] = false;
					}
					return projected;
			}
		}
		else
		{
			InstrCountFiltered1(node, 1);
		}
	}
}


static TupleTableSlot *
CustomExecScan(BioseqScanState *bioseqScanState,
			   ExecScanAccessMtd accessMtd,
			   ExecScanRecheckMtd recheckMtd) {

	// Testing custom exec scan
	bioseq_log<DBG>("[bioseq] pg: CustomExecScan() entered");

	ScanState *node = reinterpret_cast<ScanState *>(&bioseqScanState->custom_scanstate.ss);
	PlanState ps = node->ps;
	auto context = ps.ps_ExprContext;
	BioseqScanRuntime& runtime = bioseq_runtime(bioseqScanState);

	bioseq_log<DBG>("[bioseq] pg: CustomExecScan() reading plan targetlist");
	auto targetlist = ps.plan->targetlist;
	bioseq_log<DBG>("[bioseq] pg: CustomExecScan() targetlist_is_null={}", targetlist == nullptr);
	bioseq_log<DBG>("[bioseq] pg: CustomExecScan() targetlist iteration complete");

	if (runtime.placeholder_consts.dp_score_node == nullptr) {
		bioseq_log<DBG>("[bioseq] pg: CustomExecScan() allocating placeholder const nodes");

		/* INT4: byval, len=4, no collation. Used for the int32_t MatchResult
		 * fields (dp_score, mapq, chain_score, dp_max_score, seed_count,
		 * secondary_chain_score). */
		const auto int_placeholder = []() {
			return makeConst(
				INT4OID,
				-1,
				InvalidOid,
				4,
				static_cast<Datum>(static_cast<int32_t>(0)),
				/*constisnull=*/ true,
				/*constbyval=*/ true
			);
		};
		/* INT8: byval on LP64 (pg_type.typbyval for bigint is 't'), len=8.
		 * Used for the eight sequence_size_t (uint64) coordinate/length
		 * fields (query_len … block_length) so positions in sequences longer
		 * than INT32_MAX are not truncated — DOC0274 §1.8. Matches the SQL
		 * DDL where those eight projections RETURN BIGINT. */
		const auto bigint_placeholder = []() {
			return makeConst(
					INT8OID,
					-1,
					InvalidOid,
					8,
					bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(0)).raw(),
					/*constisnull=*/ true,
					/*constbyval=*/ true
				);
		};
		/* BOOL: byval, len=1, no collation. */
		const auto bool_placeholder = []() {
			return makeConst(
					BOOLOID,
					-1,
					InvalidOid,
					1,
					bioseq::pg_utils::pg_datum::from(false).raw(),
					/*constisnull=*/ true,
					/*constbyval=*/ true
				);
		};
		/* FLOAT4: byval, len=4, no collation. */
		const auto float_placeholder = []() {
			return makeConst(
					FLOAT4OID,
					-1,
					InvalidOid,
					4,
					bioseq::pg_utils::pg_datum::from(0.0F).raw(),
					/*constisnull=*/ true,
					/*constbyval=*/ true
				);
		};
		/* TEXT: byref, len=-1 (varlena), default collation. */
		const auto text_placeholder = []() {
			return makeConst(
					TEXTOID,
					-1,
					DEFAULT_COLLATION_OID,
					-1,
					bioseq::pg_utils::pg_datum::from_pointer(nullptr).raw(),
					/*constisnull=*/ true,
					/*constbyval=*/ false
				);
		};

		runtime.placeholder_consts.symbol_count_sites.clear();
		runtime.placeholder_consts.dp_score_node              = int_placeholder();
		runtime.placeholder_consts.query_len_node             = bigint_placeholder();
		runtime.placeholder_consts.query_start_node           = bigint_placeholder();
		runtime.placeholder_consts.query_end_node             = bigint_placeholder();
		runtime.placeholder_consts.target_len_node            = bigint_placeholder();
		runtime.placeholder_consts.target_start_node          = bigint_placeholder();
		runtime.placeholder_consts.target_end_node            = bigint_placeholder();
		runtime.placeholder_consts.num_matches_node           = bigint_placeholder();
		runtime.placeholder_consts.block_length_node          = bigint_placeholder();
		runtime.placeholder_consts.edit_distance_node         = bigint_placeholder();
		runtime.placeholder_consts.mapq_node                  = int_placeholder();
		runtime.placeholder_consts.chain_score_node           = int_placeholder();
		runtime.placeholder_consts.dp_max_score_node          = int_placeholder();
		runtime.placeholder_consts.seed_count_node            = int_placeholder();
		runtime.placeholder_consts.secondary_chain_score_node = int_placeholder();
		runtime.placeholder_consts.is_reverse_node            = bool_placeholder();
		runtime.placeholder_consts.is_primary_node            = bool_placeholder();
		runtime.placeholder_consts.has_cigar_node             = bool_placeholder();
		runtime.placeholder_consts.divergence_node            = float_placeholder();
		runtime.placeholder_consts.cigar_node                 = text_placeholder();

		/*
		 * Reset only the slot buffer; keep the mmsearch_context that
		 * BeginCustomScan configured earlier for this scan.
		 */
		runtime.buffer.clear_slots();

		const BioseqStubOids& oids_snapshot = get_cached_stub_oids();
		VarInjectorCtx injector_ctx;
		injector_ctx.consts           = &runtime.placeholder_consts;
		injector_ctx.oids             = &oids_snapshot;
		injector_ctx.current_relation = bioseqScanState->custom_scanstate.ss.ss_currentRelation;
		injector_ctx.match_sequence_varno = runtime.match_sequence_varno;
		injector_ctx.match_sequence_attno = runtime.match_sequence_attno;

		runtime.newtargetlist = reinterpret_cast<List *>(var_injector(
			reinterpret_cast<Node *>(targetlist),
			&injector_ctx));
	}


	bioseq_log<DBG>("[bioseq] pg: CustomExecScan() dispatching to Inner_CustomExecScan");

	auto tupleSlot = Inner_CustomExecScan(bioseqScanState, accessMtd, recheckMtd);



	//MemoryContextReset(econtext->ecxt_per_tuple_memory);

	// (*(*state.projection_info()).pi_exprContext).ecxt_per_tuple_memory,
	//ExecProject(newProj);


	return tupleSlot;
}


static TupleTableSlot *
BioseqColumnScanNext(BioseqScanState *bioseqScanState)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanNext() entered");
	CustomScanState *node = bioseqScanState->as_custom_scan_state();

	/*
	 * get information from the estate and scan state
	 */
	TableScanDesc scandesc = node->ss.ss_currentScanDesc;
	EState *estate = node->ss.ps.state;
	ScanDirection direction = estate->es_direction;
	bool vectorizationEnabled = bioseqScanState->vectorization.vectorizationEnabled;

	TupleTableSlot *slot = nullptr;

	if (vectorizationEnabled)
		slot = bioseqScanState->vectorization.scanVectorSlot;
	else
		slot = node->ss.ss_ScanTupleSlot;

	if (scandesc != NULL && scandesc->rs_rd == NULL)
	{
		bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanNext() found stale scan descriptor with null relation; reopening");
		node->ss.ss_currentScanDesc = NULL;
		scandesc = NULL;
	}

	if (scandesc == NULL)
	{
		/* the columnar access method does not use the flags, they are specific to heap */
		uint32_t flags = 0;

		/*
		 * We reach here if the scan is not parallel, or if we're serially
		 * executing a scan that was planned to be parallel.
		 */
			BioseqScanRuntime& runtime = bioseq_runtime(bioseqScanState);
			const bool needs_deferred_hydration =
				(runtime.locator_passthrough
				 || runtime.buffer.mmsearch_active() || runtime.param_rebind.active)
				&& !runtime.buffer.per_row_self_alignment();

		/*
		 * DOC0260 §8/§9 — deferred hydration is only valid for the batched
		 * mmsearch path, where the dispatch buffer consumes locator stubs and
		 * bulk-hydrates them per bucket. Plain scans may be projected by an
		 * upper Result node, so they must use the ordinary table-AM scan and
		 * return fully hydrated bioseq.seq varlenas. Same-column self-alignment
		 * (`mmsearch(seq, seq)`) also stays non-deferred: submit_scan can copy
		 * the hydrated value once as both target and per-row query, avoiding the
		 * cold post-load descriptor path while preserving deferred hydration for
		 * real target/query scans.
		 */
		if (needs_deferred_hydration) {
			scandesc = bioseq_beginscan_deferred_c(node->ss.ss_currentRelation,
											 bioseqScanState->snapshot,
											 0, NULL, NULL, flags,
											 bioseqScanState->attrNeeded,
											 bioseqScanState->qual,
											 bioseqScanState->parallelBioseqColumnScan,
												 vectorizationEnabled);
		} else {
			scandesc = bioseq_beginscan_extended_c(node->ss.ss_currentRelation,
											 bioseqScanState->snapshot,
											 0, NULL, NULL, flags,
											 bioseqScanState->attrNeeded,
											 bioseqScanState->qual,
											 bioseqScanState->parallelBioseqColumnScan,
												 vectorizationEnabled);
		}

		node->ss.ss_currentScanDesc = scandesc;
	}

	if (scandesc == NULL || scandesc->rs_rd == NULL)
	{
		ereport(ERROR,
				(errmsg("bioseq: failed to initialize columnar scan descriptor "
						"for relation %u",
						node->ss.ss_currentRelation == nullptr
							? InvalidOid
							: RelationGetRelid(node->ss.ss_currentRelation))));
	}

	/* 
	* Cleanup vector tuple table slot when reading next chunk
	 */
	if (vectorizationEnabled)
	{
		VectorTupleTableSlot *vectorSlot = reinterpret_cast<VectorTupleTableSlot *>(slot);
		for (int attrIndex : PIntList(bioseqScanState->vectorization.attrNeededList))
		{
			VectorColumn *column = reinterpret_cast<VectorColumn *>(vectorSlot->tts.tts_values[attrIndex]);
			memset(column->isnull, true, BIOSEQ_COLUMN_VECTOR_COLUMN_SIZE);
			column->dimension = 0;
		}
		vectorSlot->dimension = 0;
	}

	if (table_scan_getnextslot(scandesc, direction, slot))
	{
		/*
		* Setup custom scan state for reading tuples directly from stored
		* vector tuple table slot
		*/
		if (vectorizationEnabled)
		{
			bioseqScanState->vectorization.vectorPendingRowNumber =
				reinterpret_cast<VectorTupleTableSlot *>(slot)->dimension;
			bioseqScanState->vectorization.vectorRowIndex = 0;
		}
		return slot;
	}

	return NULL;
}


/*
 * SeqRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
BioseqColumnScanRecheck(BioseqScanState *node, TupleTableSlot *slot)
{
	return true;
}


static TupleTableSlot *
BioseqColumnScan_ExecCustomScan(CustomScanState *node)
{
	TupleTableSlot* result = nullptr;
	ErrorData* exact_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char detail[512] = "std::exception without detail";
	try {
		BioseqScanState* state = BioseqScanState::from_custom_scan_state(node);
		BioseqScanRuntime& runtime = bioseq_runtime(state);
		bioseq::pg::alignment_execution::ensure_alignment_node_registered(
			runtime.alignment_registration);
		result = CustomExecScan(
			state,
			reinterpret_cast<ExecScanAccessMtd>(BioseqColumnScanNext),
			reinterpret_cast<ExecScanRecheckMtd>(BioseqColumnScanRecheck));
		return result;
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			detail,
			error.what() == nullptr ? "std::exception without detail" : error.what(),
			sizeof(detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}

	BioseqScanState* state = BioseqScanState::from_custom_scan_state(node);
	if (state != nullptr && state->runtime != nullptr) {
		auto* runtime = static_cast<BioseqScanRuntime*>(state->runtime);
		runtime->buffer.abandon_join_runtime_after_pg_error_noexcept();
	}
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqColumnScan Exec raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqColumnScan Exec raised an unknown C++ exception")));
	}
	return result;
}


static void
BioseqColumnScan_EndCustomScan(CustomScanState *node)
{
	TableScanDesc scanDesc = node->ss.ss_currentScanDesc;
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);

	/*
	 * Profiler summary lives here (not inside the per-batch dispatch
	 * fork) so multi-batch scans emit exactly one block of summary
	 * lines. After dumping, reset_all() clears the persistent
	 * per-process aggregate so the next scan starts with fresh counts.
	 * Part 48.5 / 48.8 of NEW_PROFILING_RESEARCH.md.
	 */
	if (bioseq_aligner_enable_profiling) {
		bioseq::instr::access::write_summary_to_justlog();
		bioseq::instr::access::reset_all();
	}

	/*
	 * Free buffered scan slots HERE, while the per-query ResourceOwner
	 * that pinned their TupleDesc refs is still active. Deferring this
	 * to the next query's init would leak TupleDesc refcounts onto a
	 * dead ResourceOwner and produce
	 *   "WARNING: resource was not closed: TupleDesc ..."
	 * followed by
	 *   "ERROR: tupdesc reference ... is not owned by resource owner Portal"
	 * when the next query tries to release them.
	 *
	 * After clear_slots, free the BioseqScanRuntime itself. Owning the
	 * std::vector / std::map storage on the C++ heap (rather than in a
	 * Postgres MemoryContext) means we have to reach the destructor
	 * here — Postgres' MemoryContext teardown wouldn't run them.
	 */
	if (bioseqScanState->runtime != nullptr) {
		BioseqScanRuntime* runtime_ptr =
			static_cast<BioseqScanRuntime*>(bioseqScanState->runtime);
		if (runtime_ptr->lazy_source_provider != nullptr) {
			bioseq::storage::close_pg_sequence_source_provider(
				runtime_ptr->lazy_source_provider, /*is_commit=*/true);
			runtime_ptr->lazy_source_provider = nullptr;
		}
		if (runtime_ptr->lazy_trace_attachment != nullptr) {
			release_slice_trace_attachment(
				runtime_ptr->lazy_trace_attachment);
			runtime_ptr->lazy_trace_attachment = nullptr;
		}
		/*
		 * Clear MinimalTuple slots while the per-query ResourceOwner
		 * is still alive — Postgres tuple-desc refcounts are tracked
		 * against it. The MemoryContext reset callback fires later
		 * (during es_query_cxt teardown) and is forbidden from
		 * touching slots, so we MUST do this here.
		 *
		 * We deliberately do NOT delete the runtime here. The
		 * MemoryContextCallback struct that bioseq_runtime_cleanup_callback
		 * uses lives inside the runtime; freeing the runtime now would
		 * dangle the callback pointer in es_query_cxt's reset chain
		 * and SIGSEGV on context teardown. The callback handles the
		 * delete safely once the context begins to tear down.
		 */
		runtime_ptr->buffer.clear_slots();
		runtime_ptr->cleanup_done = true;
	}

	/*
	 * Cleanup BMS of selected scan attributes
	 */
	bioseqScanState->attrNeeded.free();

	/*
	 * clean out the tuple table
	 */
	if (node->ss.ps.ps_ResultTupleSlot)
	{
		ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
	}
	ExecClearTuple(node->ss.ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	if (scanDesc != NULL)
	{
		table_endscan(scanDesc);
		node->ss.ss_currentScanDesc = NULL;
	}

	/* Unregister snapshot */
	if (bioseqScanState->snapshotRegisteredByUs)
	{
		UnregisterSnapshot(bioseqScanState->snapshot);
	}
}


static void
BioseqColumnScan_ReScanCustomScan(CustomScanState *node)
{
	CustomScan *cscan = reinterpret_cast<CustomScan *>(node->ss.ps.plan);
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);
	BioseqScanRuntime& runtime = bioseq_runtime(bioseqScanState);
	bioseq::pg::alignment_execution::mark_alignment_rescan(
		runtime.alignment_registration);

	ResetExprContext(bioseqScanState->css_RuntimeContext);
	List *allClauses = PList<List>(cscan->custom_exprs).second();
	List *storageClauses = storage_prunable_quals_without_mmsearch(
		allClauses, get_cached_stub_oids());
	bioseqScanState->qual = reinterpret_cast<List *>(eval_params(
		reinterpret_cast<Node *>(storageClauses), bioseqScanState->css_RuntimeContext));

	/*
	 * Drop every buffered tuple, output cursor and match map left over
	 * from the previous scan iteration. Without this, an inner-side
	 * scan that drained its cursor on iteration N would re-enter Phase
	 * 2 immediately on iteration N+1 (because output_position stayed
	 * pointing past the cursor end) and return EOF without ever pulling
	 * a tuple from the access method. ReScan is the natural reset
	 * point: BeginCustomScan only fires once per query, so we cannot
	 * defer this to there.
	 */
	runtime.buffer.clear_slots();
	runtime.source_exhausted = false;

	/*
	 * Param-driven mmsearch rebind: when the planner rewrote the
	 * second mmsearch argument as a Param (parameterized inner-side
	 * scan in a Nested Loop), each rescan needs to install the current
	 * Param value as the new query. The Param lives in the scan
	 * node's PlanState ExprContext (filled by the NL parent before
	 * triggering rescan), so we resolve it via EvalParamsMutator
	 * against a copy of the original FuncExpr argument.
	 *
	 * MmsearchParamRebind retains the exact plan FuncExpr because the copied
	 * storage qual has already folded Params and planner order may put a Const
	 * extra before the Param-driven primary.
	 */
	if (runtime.param_rebind.active)
	{
		/* The stashed FuncExpr identifies the exact primary Param even when
		 * planner qual ordering puts a Const extra first.  NULL clears only
		 * the primary context; the strict plan qual rejects every row while
		 * registered Const extras survive for a later non-NULL rescan. */
		(void)resolve_mmsearch_param_query(
			runtime, node->ss.ps.ps_ExprContext,
			"ReScanCustomScan");
	}

	TableScanDesc scanDesc = node->ss.ss_currentScanDesc;

	if (scanDesc != NULL)
	{
		/* XXX: hack to pass quals as scan keys */
		ScanKey scanKeys = reinterpret_cast<ScanKey>(bioseqScanState->qual.ptr());
		table_rescan(node->ss.ss_currentScanDesc,
					 scanKeys);
	}
}


static void
ExplainPrefilterExecutionProof(
	const bioseq::pipeline_stats::PipelineOperationalStats& operational,
	ExplainState* es)
{
	if (!operational.prefilter_execution_proof_present) return;
	const auto& proof = operational.prefilter_execution_proof;
	ExplainPropertyBool("Prefilter Proof Present", true, es);
	ExplainPropertyText(
		"Prefilter Resolved Algorithm", proof.resolved_algorithm_name(), es);
	ExplainPropertyText(
		"Prefilter Resolved Backend", proof.resolved_backend_name(), es);
	ExplainPropertyUInteger(
		"Prefilter Dispatches", nullptr, proof.dispatches, es);
	ExplainPropertyUInteger(
		"Prefilter SneakySnake Dispatches", nullptr,
		proof.sneaky_snake_dispatches, es);
	ExplainPropertyUInteger(
		"Prefilter GateKeeper Dispatches", nullptr,
		proof.gatekeeper_dispatches, es);
	ExplainPropertyUInteger(
		"Prefilter Scalar Dispatches", nullptr, proof.scalar_dispatches, es);
	ExplainPropertyUInteger(
		"Prefilter SIMD Dispatches", nullptr, proof.simd_dispatches, es);
	ExplainPropertyUInteger(
		"Prefilter Vulkan Dispatches", nullptr, proof.vulkan_dispatches, es);
	ExplainPropertyUInteger("Prefilter Jobs", nullptr, proof.jobs, es);
	ExplainPropertyUInteger(
		"Prefilter Evaluated Jobs", nullptr, proof.evaluated_jobs, es);
	ExplainPropertyUInteger(
		"Prefilter Length 64 Jobs", nullptr, proof.length_64_jobs, es);
	ExplainPropertyUInteger(
		"Prefilter Unequal-Length Jobs", nullptr,
		proof.length_difference_jobs, es);
	ExplainPropertyUInteger(
		"Prefilter Unsupported Jobs", nullptr, proof.unsupported_jobs, es);
	ExplainPropertyUInteger(
		"Prefilter Rejected Jobs", nullptr, proof.rejected_jobs, es);
	ExplainPropertyUInteger(
		"Prefilter Rejected Regions", nullptr, proof.rejected_regions, es);
	ExplainPropertyUInteger(
		"Prefilter Surviving Regions", nullptr, proof.surviving_regions, es);
	ExplainPropertyUInteger(
		"Prefilter Mask Rejection Candidates", nullptr,
		proof.mask_rejection_candidates, es);
	ExplainPropertyUInteger(
		"Prefilter Exact Guard Activations", nullptr,
		proof.exact_guard_activations, es);
	ExplainPropertyUInteger(
		"Prefilter Guard Confirmed Rejections", nullptr,
		proof.guard_confirmed_rejections, es);
	ExplainPropertyUInteger(
		"Prefilter Hamming Exact Rejections", nullptr,
		proof.hamming_exact_rejections, es);
	ExplainPropertyBool(
		"Prefilter Proof Saturated", proof.saturated, es);
	ExplainPropertyBool("Prefilter Proof Valid", proof.valid(), es);
}

static void
ExplainPipelineExecutionStats(
	const bioseq::pipeline_stats::PipelineExecutionStats& stats,
	ExplainState* es)
{
	const auto requested_backend =
		bioseq::pipeline_stats::executed_backend_name(stats.requested_backend);
	const auto actual_backend =
		bioseq::pipeline_stats::executed_backend_name(stats.actual_backend);
	const auto requested_source =
		bioseq::pipeline_stats::target_index_source_name(
			stats.requested_target_index_source);
	const auto actual_source =
		bioseq::pipeline_stats::target_index_source_name(
			stats.actual_target_index_source);
	const auto& operational = stats.operational;
	const auto& query_sketch_timing = operational.timing[
		bioseq::pipeline_stats::stage_index(
			bioseq::pipeline_stats::PipelineStage::query_sketch)];
	const auto& target_index_build_timing = operational.timing[
		bioseq::pipeline_stats::stage_index(
			bioseq::pipeline_stats::PipelineStage::target_index_build)];

	if (es->format == EXPLAIN_FORMAT_TEXT) {
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str,
			"* Execution proof [requested_backend=%.*s actual_backend=%.*s "
			"requested_index_source=%.*s actual_index_source=%.*s "
			"attempted=%llu fully_attested=%llu unattested=%llu "
			"environment_mixed=%s persisted_identity_mixed=%s "
			"identity_conflict=%s saturated=%s]\n",
			static_cast<int>(requested_backend.size()), requested_backend.data(),
			static_cast<int>(actual_backend.size()), actual_backend.data(),
			static_cast<int>(requested_source.size()), requested_source.data(),
			static_cast<int>(actual_source.size()), actual_source.data(),
			static_cast<unsigned long long>(stats.attempted_alignment_jobs),
			static_cast<unsigned long long>(stats.fully_attested_alignment_jobs),
			static_cast<unsigned long long>(stats.unattested_alignment_jobs),
			stats.environment.mixed_invalid ? "true" : "false",
			stats.mixed_persisted_identity ? "true" : "false",
			operational.operational_identity_conflict ? "true" : "false",
			operational.stats_saturated ? "true" : "false");
		if (stats.unattested_alignment_jobs != 0) {
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str,
				"* First unattested [target_row=%llu query_ordinal=%llu]\n",
				static_cast<unsigned long long>(
					stats.first_unattested_target_row),
				static_cast<unsigned long long>(
					stats.first_unattested_query_ordinal));
		}
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str,
			"* Index outcomes [targets=%llu persisted=%llu missing_persisted=%llu "
			"recomputed=%llu]\n",
			static_cast<unsigned long long>(operational.target_rows),
			static_cast<unsigned long long>(operational.persisted_index_rows),
			static_cast<unsigned long long>(
				operational.missing_persisted_index_rows),
			static_cast<unsigned long long>(
				operational.recomputed_target_index_rows));
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str,
			"* GPU dispatch [seed=%llu chain=%llu chain_backtrack=%llu "
				"coordinate_vote=%llu extend=%llu backend_declines=%llu "
				"backend_fallbacks=%llu]\n",
				static_cast<unsigned long long>(operational.gpu_seed_dispatches),
				static_cast<unsigned long long>(operational.gpu_chain_dispatches),
				static_cast<unsigned long long>(
					operational.gpu_chain_backtrack_dispatches),
				static_cast<unsigned long long>(
					operational.gpu_coordinate_vote_dispatches),
				static_cast<unsigned long long>(operational.gpu_extend_dispatches),
			static_cast<unsigned long long>(operational.backend_declines),
			static_cast<unsigned long long>(operational.backend_fallbacks));
		/*
		 * Keep the compact headline above for human scans, and also expose the
		 * algorithm-identity counters as ordinary textual EXPLAIN properties.
		 * These values come from the same completed PipelineExecutionStats
		 * aggregate as the headline; selection flags never populate them.
		 */
		ExplainPropertyUInteger("GPU Chain Dispatches", nullptr,
			operational.gpu_chain_dispatches, es);
		ExplainPropertyUInteger("GPU Chain Backtrack Dispatches", nullptr,
			operational.gpu_chain_backtrack_dispatches, es);
		ExplainPropertyUInteger("GPU Coordinate Vote Dispatches", nullptr,
			operational.gpu_coordinate_vote_dispatches, es);
		appendStringInfoSpaces(es->str, es->indent * 2);
		appendStringInfo(es->str,
			"* Host evidence [query_sketch_ns=%llu target_index_build_ns=%llu "
			"query_hash_readbacks=%llu query_symbol_packing=%llu "
			"target_symbol_packing=%llu alignment_classification=%llu "
			"cigar_construction=%llu]\n",
			static_cast<unsigned long long>(query_sketch_timing.host_wall_ns),
			static_cast<unsigned long long>(
				target_index_build_timing.host_wall_ns),
			static_cast<unsigned long long>(
				operational.host_query_hash_readbacks),
			static_cast<unsigned long long>(
				operational.host_query_symbol_packing_calls),
			static_cast<unsigned long long>(
				operational.host_target_symbol_packing_calls),
			static_cast<unsigned long long>(
				operational.host_alignment_job_classification_calls),
			static_cast<unsigned long long>(
				operational.host_cigar_construction_calls));
		ExplainPrefilterExecutionProof(operational, es);
		return;
	}

	ExplainOpenGroup(
		"Bioseq Execution Proof", "Bioseq Execution Proof", true, es);
	ExplainPropertyText(
		"Requested Backend", requested_backend.data(), es);
	ExplainPropertyText(
		"Actual Backend", actual_backend.data(), es);
	ExplainPropertyText(
		"Requested Target Index Source", requested_source.data(), es);
	ExplainPropertyText(
		"Actual Target Index Source", actual_source.data(), es);
	ExplainPropertyUInteger("Attempted Alignment Jobs", nullptr,
		stats.attempted_alignment_jobs, es);
	ExplainPropertyUInteger("Fully Attested Alignment Jobs", nullptr,
		stats.fully_attested_alignment_jobs, es);
	ExplainPropertyUInteger("Unattested Alignment Jobs", nullptr,
		stats.unattested_alignment_jobs, es);
	if (stats.unattested_alignment_jobs != 0) {
		ExplainPropertyUInteger("First Unattested Target Row", nullptr,
			stats.first_unattested_target_row, es);
		ExplainPropertyUInteger("First Unattested Query Ordinal", nullptr,
			stats.first_unattested_query_ordinal, es);
	}
	ExplainPropertyBool(
		"Environment Mixed Invalid", stats.environment.mixed_invalid, es);
	ExplainPropertyBool(
		"Mixed Persisted Identity", stats.mixed_persisted_identity, es);
	ExplainPropertyBool("Operational Identity Conflict",
		operational.operational_identity_conflict, es);
	ExplainPropertyBool(
		"Stats Saturated", operational.stats_saturated, es);
	ExplainPropertyUInteger("Target Rows", nullptr,
		operational.target_rows, es);
	ExplainPropertyUInteger("Persisted Index Rows", nullptr,
		operational.persisted_index_rows, es);
	ExplainPropertyUInteger("Missing Persisted Index Rows", nullptr,
		operational.missing_persisted_index_rows, es);
	ExplainPropertyUInteger("Recomputed Target Index Rows", nullptr,
		operational.recomputed_target_index_rows, es);
	ExplainPropertyUInteger("GPU Seed Dispatches", nullptr,
		operational.gpu_seed_dispatches, es);
	ExplainPropertyUInteger("GPU Chain Dispatches", nullptr,
		operational.gpu_chain_dispatches, es);
	ExplainPropertyUInteger("GPU Chain Backtrack Dispatches", nullptr,
		operational.gpu_chain_backtrack_dispatches, es);
	ExplainPropertyUInteger("GPU Coordinate Vote Dispatches", nullptr,
		operational.gpu_coordinate_vote_dispatches, es);
	ExplainPropertyUInteger("GPU Extend Dispatches", nullptr,
		operational.gpu_extend_dispatches, es);
	ExplainPropertyUInteger("Backend Declines", nullptr,
		operational.backend_declines, es);
	ExplainPropertyUInteger("Backend Fallbacks", nullptr,
		operational.backend_fallbacks, es);
	ExplainPropertyUInteger("Query Sketch Host Wall Ns", nullptr,
		query_sketch_timing.host_wall_ns, es);
	ExplainPropertyUInteger("Target Index Build Host Wall Ns", nullptr,
		target_index_build_timing.host_wall_ns, es);
	ExplainPropertyUInteger("Host Query Hash Readbacks", nullptr,
		operational.host_query_hash_readbacks, es);
	ExplainPropertyUInteger("Host Query Symbol Packing Calls", nullptr,
		operational.host_query_symbol_packing_calls, es);
	ExplainPropertyUInteger("Host Target Symbol Packing Calls", nullptr,
		operational.host_target_symbol_packing_calls, es);
	ExplainPropertyUInteger("Host Alignment Job Classification Calls", nullptr,
		operational.host_alignment_job_classification_calls, es);
	ExplainPropertyUInteger("Host CIGAR Construction Calls", nullptr,
		operational.host_cigar_construction_calls, es);
	ExplainPrefilterExecutionProof(operational, es);
	ExplainCloseGroup(
		"Bioseq Execution Proof", "Bioseq Execution Proof", true, es);
}

static void
BioseqColumnScan_ExplainCustomScan(CustomScanState *node, List *ancestors,
							   ExplainState *es)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_ExplainCustomScan() entered");
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);

	List *context = set_deparse_context_planstate(
		es->deparse_cxt, reinterpret_cast<Node *>(&node->ss.ps), ancestors);

	List *projectedColumns = BioseqColumnVarNeeded(bioseqScanState);
	const char *projectedColumnsStr = BioseqColumnProjectedColumnsStr(
		context, projectedColumns);
	ExplainPropertyText("Bioseq Projected Columns",
						projectedColumnsStr, es);

	CustomScan *cscan = pg_node_expect<CustomScan>(pg_as_node(node->ss.ps.plan));
	List *chunkGroupFilter = storage_prunable_quals_without_mmsearch(
		PList<List>(cscan->custom_exprs).second(), get_cached_stub_oids());
	if (chunkGroupFilter != NULL)
	{
		const char *pushdownClausesStr = BioseqColumnPushdownClausesStr(
			context, chunkGroupFilter);
		ExplainPropertyText("Bioseq Chunk Group Filters",
							pushdownClausesStr, es);

		BioseqColumnScanDesc bioseqColumnScanDesc =
			reinterpret_cast<BioseqColumnScanDesc>(node->ss.ss_currentScanDesc);
		if (bioseqColumnScanDesc != NULL)
		{
			ExplainPropertyInteger(
				"Bioseq Chunk Groups Removed by Filter",
				NULL, BioseqColumnScanChunkGroupsFiltered(bioseqColumnScanDesc), es);
		}
	}

	if (bioseqScanState->vectorization.vectorizationEnabled &&
		bioseqScanState->vectorization.vectorizedQualList != NULL)
	{
		const char *vectorizedWhereClauses = BioseqColumnPushdownClausesStr(
					context, bioseqScanState->vectorization.vectorizedQualList);
		ExplainPropertyText("Bioseq Vectorized Filter",
							vectorizedWhereClauses, es);
	}

	/*
	 * List every per-column metadata lookup the scan resolves without
	 * touching the base relation's varlena. Today this is just the
	 * symbol_count fast path — each rewritten call-site shows up as
	 * "<column>.symbol_count('<letter>', rank=<n>)". The list is
	 * recomputed here by walking the plan's target list instead of
	 * inspecting `placeholder_consts.symbol_count_sites` directly: the
	 * latter is populated lazily on first ExecScan, but EXPLAIN (without
	 * ANALYZE) never runs ExecScan and would therefore show an empty
	 * line. Re-deriving from the plan keeps EXPLAIN output useful for
	 * planners inspecting queries that have not yet executed. When
	 * future metadata is surfaced via the same pattern (min_qual,
	 * gc_ratio, …) they should extend this walker accordingly.
	 */
	{
		StringInfoData meta_buf;
		initStringInfo(&meta_buf);
		bool first = true;
		const BioseqStubOids& oids_snapshot = get_cached_stub_oids();
		Relation scan_rel = node->ss.ss_currentRelation;
		for (auto tgt : PList<TargetEntry>(node->ss.ps.plan->targetlist)) {
			if (tgt == nullptr) {
				continue;
			}
			FuncExpr* fe = pg_node_try_as<FuncExpr>(pg_as_node(tgt->expr));
			if (fe == nullptr
				|| fe->funcid != oids_snapshot.symbol_count_oid
				|| list_length(fe->args) != 2) {
				continue;
			}
			PList<Node> args(fe->args);
			Var* col_var = pg_node_try_as<Var>(args.first());
			Const* letter_const = pg_node_try_as<Const>(args.second());
			if (col_var == nullptr || letter_const == nullptr) {
				continue;
			}
			if (letter_const->constisnull || col_var->vartypmod < 0) {
				continue;
			}
			text* letter_detoasted = reinterpret_cast<text*>(
				PG_DETOAST_DATUM(letter_const->constvalue));
			if (VARSIZE_ANY_EXHDR(letter_detoasted) != 1) {
				if (reinterpret_cast<void*>(letter_detoasted)
					!= reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
					pfree(letter_detoasted);
				}
				continue;
			}
			const char letter_char = *VARDATA_ANY(letter_detoasted);
			if (reinterpret_cast<void*>(letter_detoasted)
				!= reinterpret_cast<void*>(DatumGetPointer(letter_const->constvalue))) {
				pfree(letter_detoasted);
			}
			const compact_sequence_stdc_type_params typmod = compact_sequence_stdc_type_params::decode(col_var->vartypmod);
			const int rank = bioseq::storage::indexing::letter_to_rank(typmod.alphabet_code, letter_char);
			if (rank < 0) {
				continue;
			}
			std::string col_name;
			if (scan_rel != nullptr && col_var->varattno >= 1) {
				char* attname_cstr = get_attname(
					RelationGetRelid(scan_rel), col_var->varattno, /*missing_ok=*/true);
				if (attname_cstr != nullptr) {
					col_name.assign(attname_cstr);
					pfree(attname_cstr);
				}
			}
			if (!first) {
				appendStringInfoString(&meta_buf, ", ");
			}
			first = false;
			appendStringInfo(&meta_buf,
							 "%s.symbol_count('%c', rank=%d)",
							 col_name.empty() ? "<unknown>" : col_name.c_str(),
							 letter_char, rank);
		}
		if (!first) {
			ExplainPropertyText("Bioseq Fetched From Meta", meta_buf.data, es);
		}
		pfree(meta_buf.data);
	}

	/*
	 * Two-tier EXPLAIN:
	 *   1. A "Bioseq Fetched From Sequence Index" / "Bioseq Recomputed"
	 *      summary line — surfaces only when the plan actually runs the
	 *      minimap2 pipeline (i.e. mmsearch is part of the plan).
	 *   2. A full One-to-many alignment breakdown showing each pipeline
	 *      stage and the Minimap2Params values that drive it. The stage
	 *      list is gated by params.do_cigar / compute_divergence /
	 *      compute_mapq so users see exactly what will (or will not) run.
	 *
	 * The distinction from the previous implementation is that the
	 * sidecar/recomputed line never prints for a pass-through scan. Plans
	 * that only use symbol_count/meta projections still show the "Fetched
	 * From Meta" line above but never a minimizer-index line — because
	 * they never consult index_sidecar.
	 */
	/*
	 * Per-scan aligner snapshot captured at BeginCustomScan. Hoisted to
	 * the outer scope so the pipeline-step breakdown block below can
	 * reuse it without a second runtime lookup.
	 */
	BioseqScanState* explainScanState = BioseqScanState::from_custom_scan_state(node);
	const BioseqScanRuntime* explain_runtime =
		(explainScanState != nullptr && explainScanState->runtime != nullptr)
			? static_cast<const BioseqScanRuntime*>(explainScanState->runtime)
			: nullptr;
	const PerScanAlignerState* aligner_snapshot =
		(explain_runtime != nullptr) ? &explain_runtime->aligner_state : nullptr;

	{
		/*
		 * Use the feature flags captured on BeginCustomScan when
		 * available (executor ran). For EXPLAIN without ANALYZE the
		 * BeginCustomScan may not have fired — recompute locally using
		 * the plan targetlist/qual so the printed output is still
		 * accurate.
		 */
		const BioseqStubOids& explain_oids = get_cached_stub_oids();
		List* explain_owned_mmsearch = cscan != nullptr
			&& list_length(cscan->custom_exprs) >= 4
			? reinterpret_cast<List*>(list_nth(cscan->custom_exprs, 3))
			: NIL;
		BioseqScanFeatureFlags features =
			(aligner_snapshot != nullptr && aligner_snapshot->params_captured)
				? aligner_snapshot->features
				: detect_feature_flags(node->ss.ps.plan->targetlist,
									   node->ss.ps.plan->qual, explain_oids);
		features.has_mmsearch = features.has_mmsearch
			|| explain_owned_mmsearch != NIL;

		/*
		 * Only the WHERE mmsearch(...) pipeline consults the minimizer
		 * index. A bare SELECT with only symbol_count/metadata projections
		 * stays on the meta-sidecar side and should not see a minimizer
		 * line at all (the previous behaviour printed one unconditionally
		 * and was misleading).
		 */
		if (features.has_mmsearch)
		{
			/*
			 * DOC0279 — plan-time typmod-coalesce hint. `EXPLAIN` without
			 * `ANALYZE` never populates the runtime coalesce buffer, so the
			 * runtime-gated "...coalesce to..." sub-clause further below cannot
			 * fire for a plain `EXPLAIN`. Re-derive the decision from the plan
			 * qual here so the coalesce is always visible in the plan (restores
			 * the pre-DOC0279 plain-EXPLAIN behaviour the §11.5 rewrite dropped).
			 */
			{
				const ExplainTypmodCoalesce coalesce_hint =
					explain_detect_typmod_coalesce(
						explain_owned_mmsearch != NIL
							? explain_owned_mmsearch : node->ss.ps.plan->qual,
						explain_oids);
				if (coalesce_hint.active) {
					const auto coa_name =
						pgseq_supported_alphabets::name_of_alphabet_code(coalesce_hint.col_alpha);
					StringInfoData coa_buf;
					initStringInfo(&coa_buf);
					appendStringInfo(&coa_buf, "coalesce to seq('%.*s', %u)",
						static_cast<int>(coa_name.size()), coa_name.data(),
						static_cast<unsigned>(coalesce_hint.col_byte_width) * 8u);
					ExplainPropertyText("Bioseq Typmod Coalesce", coa_buf.data, es);
					pfree(coa_buf.data);
				}
			}
			Oid user_relid = InvalidOid;
			double rel_tuples = -1.0;
			if (node->ss.ss_currentRelation != nullptr) {
				user_relid = RelationGetRelid(node->ss.ss_currentRelation);
				if (node->ss.ss_currentRelation->rd_rel != nullptr) {
					rel_tuples = static_cast<double>(
						node->ss.ss_currentRelation->rd_rel->reltuples);
				}
			} else if (cscan != nullptr && node->ss.ps.state != nullptr) {
				RangeTblEntry* rte = rt_fetch(cscan->scan.scanrelid,
											  node->ss.ps.state->es_range_table);
				if (rte != nullptr) {
					user_relid = rte->relid;
				}
			}

				if (user_relid != InvalidOid) {
					if (rel_tuples < 0.0) {
						HeapTuple cls_tup = SearchSysCache1(RELOID,
															bioseq::pg_utils::pg_datum::from_oid(user_relid).raw());
					if (HeapTupleIsValid(cls_tup)) {
						Form_pg_class cls = reinterpret_cast<Form_pg_class>(GETSTRUCT(cls_tup));
						rel_tuples = static_cast<double>(cls->reltuples);
						ReleaseSysCache(cls_tup);
					}
				}
				/*
				 * Index-presence estimate is the plan-time view of how
				 * many rows have a prebuilt sidecar blob. With
				 * read_sequence_index off we always recompute — surface
				 * that explicitly. With it on we display both the
				 * "fetched from sidecar" and "recomputed" halves so the
				 * user can see exactly what the pipeline did (or will
				 * do): fetched = min(presence, 1), recomputed = 1 -
				 * fetched. The two lines sum to 100%.
				 *
				 * The cross-alphabet recompute path overrides everything
				 * here: a query against a different alphabet than the
				 * column's makes the sidecar's minimizers structurally
				 * incompatible (different bits-per-letter encoding), so
				 * the buffer's `recompute_due_to_alphabet_mismatch` flag
				 * forces a 100% recompute regardless of how full the
				 * sidecar actually is. We surface this with an explicit
				 * "Performance reduced" hint so users understand why a
				 * sidecar-backed scan suddenly gets slower.
				 */
				const float index_presence =
					bioseq::storage::indexing::estimate_index_presence(
						user_relid, rel_tuples,
						bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX);
				float fetched_pct    = 0.0f;
				float recomputed_pct = 100.0f;
				const bool runtime_alphabet_recompute =
					(explain_runtime != nullptr)
					&& explain_runtime->buffer.recompute_due_to_alphabet_mismatch();
				const bool runtime_coalesce_active =
					(explain_runtime != nullptr)
					&& explain_runtime->buffer.coalesce_active();
				if (runtime_alphabet_recompute) {
					/*
					 * Cross-alphabet path: always 100% recompute. Print
					 * the explanatory note next to the line so the user
					 * sees WHY the sidecar didn't apply. We accept that
					 * the message reaches into runtime state (raw vs
					 * coalesced typmod) which EXPLAIN-without-ANALYZE
					 * may not have populated yet.
					 */
					StringInfoData recompute_buf;
					initStringInfo(&recompute_buf);
					appendStringInfo(&recompute_buf, "minimizers 100%%");
					ExplainPropertyText("Bioseq Recomputed", recompute_buf.data, es);
					pfree(recompute_buf.data);

					if (explain_runtime != nullptr) {
						const auto raw_alpha = explain_runtime->buffer.raw_query_alphabet_code();
						const auto raw_width = explain_runtime->buffer.raw_query_storage_unit_byte_width();
						const auto col_alpha = explain_runtime->buffer.mmsearch_alphabet_code();
						const auto col_width = explain_runtime->buffer.mmsearch_storage_unit_byte_width();
						const auto width_to_bits = [](uint8_t code) {
							return static_cast<unsigned>(8u << code);
						};
						StringInfoData hint_buf;
						initStringInfo(&hint_buf);
						const auto col_name =
							pgseq_supported_alphabets::name_of_alphabet_code(col_alpha);
						const auto raw_name =
							pgseq_supported_alphabets::name_of_alphabet_code(raw_alpha);
						appendStringInfo(&hint_buf,
							"column is seq('%.*s', %u), but query is seq('%.*s', %u), so index needs to be recomputed",
							static_cast<int>(col_name.size()), col_name.data(),
							width_to_bits(col_width),
							static_cast<int>(raw_name.size()), raw_name.data(),
							width_to_bits(raw_width));
						ExplainPropertyText("Performance reduced", hint_buf.data, es);
						pfree(hint_buf.data);
					}
				} else if (bioseq_read_sequence_index) {
					fetched_pct    = index_presence * 100.0f;
					recomputed_pct = (1.0f - index_presence) * 100.0f;
					if (fetched_pct > 0.0f) {
						StringInfoData presence_buf;
						initStringInfo(&presence_buf);
						appendStringInfo(&presence_buf,
										 "minimizers (index presence: %.0f%%",
										 static_cast<double>(fetched_pct));
						if (runtime_coalesce_active && explain_runtime != nullptr) {
							/*
							 * Surface the typmod-coalesce decision next to
							 * the index-presence percentage so users can
							 * see "we re-encoded your literal at the
							 * column's typmod, then used the prebuilt
							 * minimizer index". Without this line the user
							 * sees a successful scan and has no idea their
							 * literal was silently re-encoded.
							 */
							const auto col_alpha = explain_runtime->buffer.mmsearch_alphabet_code();
							const auto col_width = explain_runtime->buffer.mmsearch_storage_unit_byte_width();
							// mmsearch_storage_unit_byte_width() returns the
							// storage unit width in BYTES (1/2/4/8), so the
							// typmod width in bits is byte_width * 8 (e.g. 4
							// bytes -> seq('dna4', 32)). The previous
							// `8u << byte_width` formula treated the byte count
							// as a log2 code and rendered 4 bytes as 128.
							const auto width_to_bits = [](uint8_t byte_width) {
								return static_cast<unsigned>(byte_width) * 8u;
							};
							const auto col_name =
								pgseq_supported_alphabets::name_of_alphabet_code(col_alpha);
							appendStringInfo(&presence_buf,
								", coalesce to seq('%.*s', %u)",
								static_cast<int>(col_name.size()), col_name.data(),
								width_to_bits(col_width));
						}
						appendStringInfoString(&presence_buf, ")");
						ExplainPropertyText("Bioseq Fetched From Sequence Index",
											presence_buf.data, es);
						pfree(presence_buf.data);
					}
					if (recomputed_pct > 0.0f) {
						StringInfoData recompute_buf;
						initStringInfo(&recompute_buf);
						appendStringInfo(&recompute_buf,
										 "minimizers %.0f%%",
										 static_cast<double>(recomputed_pct));
						ExplainPropertyText("Bioseq Recomputed",
											recompute_buf.data, es);
						pfree(recompute_buf.data);
					}
				} else {
					/* 100% recomputed because the GUC is off. */
					ExplainPropertyText("Bioseq Recomputed",
										"minimizers 100% (read_sequence_index disabled)",
										es);
				}
			}

			{
				const auto scheduler =
					bioseq::parallelism::pg::snapshot();
				const auto scheduler_mode =
					bioseq::parallelism::to_string(scheduler.mode);
				StringInfoData scheduler_buf;
				initStringInfo(&scheduler_buf);
				appendStringInfo(&scheduler_buf,
					"supervisor=%s mode=%.*s workers=%u/%u batches=%llu/%llu awaiting=%llu queries=%llu/%llu qps=%.3f async_persist_waiting=%llu index_memory_reserved=%llu",
					scheduler.supervisor ? "on" : "local",
					static_cast<int>(scheduler_mode.size()),
					scheduler_mode.data(),
					static_cast<unsigned>(scheduler.current_threads),
					static_cast<unsigned>(scheduler.max_threads),
					static_cast<unsigned long long>(scheduler.batches_processed),
					static_cast<unsigned long long>(scheduler.batches_scheduled),
					static_cast<unsigned long long>(scheduler.batches_awaiting),
					static_cast<unsigned long long>(scheduler.queries_processed),
					static_cast<unsigned long long>(scheduler.queries_waiting),
					scheduler.queries_per_sec,
					static_cast<unsigned long long>(scheduler.async_persist_jobs_waiting),
					static_cast<unsigned long long>(scheduler.index_memory_reserved_bytes));
				ExplainPropertyText("Bioseq Scheduler", scheduler_buf.data, es);
				pfree(scheduler_buf.data);
			}
		}
	}

	/*
	 * Rich pipeline-step breakdown. Only shown when mmsearch is present
	 * (without mmsearch the minimap2 pipeline never runs). Uses the
	 * per-scan params captured at BeginCustomScan so every printed value
	 * is the one actually fed to the batch processor — not a recomputation
	 * off GUCs that might have changed since.
	 */
	if (aligner_snapshot != nullptr
		&& aligner_snapshot->params_captured
		&& aligner_snapshot->features.has_mmsearch)
	{
		using bioseq::simd::active_target::ChainingAlgorithmType;
		using bioseq::simd::active_target::BaseAlignmentAlgorithmType;
		using bioseq::simd::active_target::chaining_algorithm_name;
		using bioseq::simd::active_target::base_alignment_algorithm_name;
		const auto& params = aligner_snapshot->params;
		const auto& features = aligner_snapshot->features;

		/*
		 * Renderer driven by the structured `PipelineDescription` populated at
		 * `BeginCustomScan` (DOC0249 §12.3).  The renderer no longer hardcodes
		 * which parameters belong to which stage — adding a new stage / new
		 * algorithm trait surfaces automatically once it appears in
		 * `PipelineDescription::stages`.
		 *
		 * When the describe path did not populate a description (e.g. a
		 * cold-path branch that bypassed BeginCustomScan), `pipeline_desc` is
		 * `std::nullopt`; in that case we emit a minimal placeholder line so
		 * EXPLAIN still surfaces the algorithm choice the executor will run.
		 */
			const bool has_pipeline_desc =
				aligner_snapshot->pipeline_desc.has_value();
			const auto pipeline_scheduler =
				bioseq::parallelism::pg::snapshot();
			const auto pipeline_mode =
				bioseq::parallelism::to_string(pipeline_scheduler.mode);
			const std::string pipeline_mode_text{pipeline_mode};
			const char* pipeline_label =
				pipeline_scheduler.mode
					== bioseq::parallelism::PipelineConcurrencyMode::SINGLE
				? "Single thread"
				: pipeline_scheduler.mode
					== bioseq::parallelism::PipelineConcurrencyMode::PARALLEL
				? "Parallel dispatch"
				: "Task-parallel";

			if (es->format == EXPLAIN_FORMAT_TEXT) {
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfoString(es->str,
					"One-to-many alignment:\n");
				++es->indent;
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfo(es->str,
					"%s [mode=%.*s, workers=%u, index_memory_reserved=%llu bytes]:\n",
					pipeline_label,
					static_cast<int>(pipeline_mode.size()),
					pipeline_mode.data(),
					static_cast<unsigned>(pipeline_scheduler.max_threads),
					static_cast<unsigned long long>(
						pipeline_scheduler.index_memory_reserved_bytes));
				++es->indent;

			/*
			 * Sidecar-divergence warning: when the prebuilt-index path is
			 * active for some fraction of the batch, those rows pin
			 * k=15/w=10 regardless of the user's GUC (see
			 * dispatch_buffer.cppm).  Surfaced as a header line above the
			 * pipeline tree so it is not visually attached to any one stage.
			 */
			const bool sidecar_path_active = bioseq_read_sequence_index;
			const bool user_kw_differs =
				sidecar_path_active &&
				(params.kmer_size != 15 || params.window_size != 10);
			if (user_kw_differs) {
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfoString(es->str,
					"NOTE: sidecar rows pin kmer_size=15 window_size=10\n");
			}

			if (has_pipeline_desc) {
				for (const auto& stage :
					aligner_snapshot->pipeline_desc->stages)
				{
					/*
					 * Skip stages whose `algorithm` is std::nullopt AND
					 * carry no `extra_description` — those are stages
					 * disabled by their `enabled` flag (e.g. DivergenceStage
					 * when compute_divergence=false).  DOC0249 §11.4.
					 */
					if (!stage.algorithm.has_value()
						&& !stage.extra_description.has_value())
					{
						continue;
					}

					appendStringInfoSpaces(es->str, es->indent * 2);
					if (stage.algorithm.has_value()) {
						appendStringInfo(es->str, "| %s: %s\n",
							stage.stage_name.c_str(),
							stage.algorithm->name.c_str());
						for (const auto& p : stage.algorithm->parameters) {
							appendStringInfoSpaces(es->str,
								(es->indent + 1) * 2);
							appendStringInfo(es->str, "%s: %s\n",
								p.name.c_str(), p.value.c_str());
						}
						for (const auto& p : stage.algorithm->computed_properties) {
							appendStringInfoSpaces(es->str,
								(es->indent + 1) * 2);
							appendStringInfo(es->str, "[%s: %s]\n",
								p.name.c_str(), p.value.c_str());
						}
					} else {
						appendStringInfo(es->str, "| %s — %s\n",
							stage.stage_name.c_str(),
							stage.extra_description->c_str());
					}
					if (stage.estimated_batch_size.has_value()) {
						appendStringInfoSpaces(es->str,
							(es->indent + 1) * 2);
						appendStringInfo(es->str, "[reserved: %zu]\n",
							*stage.estimated_batch_size);
					}
				}
			} else {
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfo(es->str,
					"| Pipeline (chaining=%s, base_alignment=%s)\n",
					chaining_algorithm_name(params.chaining_algorithm),
					base_alignment_algorithm_name(params.base_alignment_algorithm));
			}

			/*
			 * Surface canonical pipeline stats. Only meaningful after ANALYZE has
			 * actually run the scan (target_rows is zero before the first
			 * completed batch).
			 */
			auto print_submit = [&](const char* label) {
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfo(es->str, "* %s\n", label);
			};
			if (explain_runtime != nullptr) {
				const auto& stats = explain_runtime->buffer.execution_stats();
				const auto& operational = stats.operational;
				if (operational.target_rows > 0) {
					StringInfoData submit_label;
					initStringInfo(&submit_label);
					appendStringInfo(&submit_label,
						"Submit results [targets=%llu persisted_index=%llu recomputed=%llu]",
						static_cast<unsigned long long>(
							operational.target_rows),
						static_cast<unsigned long long>(
							operational.persisted_index_rows),
						static_cast<unsigned long long>(
							operational.recomputed_target_index_rows));
					print_submit(submit_label.data);
					pfree(submit_label.data);
				} else {
					print_submit("Submit results");
				}
				ExplainPipelineExecutionStats(stats, es);
			} else {
				print_submit("Submit results");
			}

			if (params.enable_profiling) {
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfoString(es->str, "Runtime:\n");
				++es->indent;
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfo(es->str,
					"profiling: enabled (see bioseq_log<DBG> for per-stage μs/ns counters)\n");
				appendStringInfoSpaces(es->str, es->indent * 2);
				if (explain_runtime != nullptr) {
					const auto& stats = explain_runtime->buffer.execution_stats();
					const auto& operational = stats.operational;
					appendStringInfo(es->str,
						"batch summary: target_rows=%llu, persisted_index=%llu, "
						"recomputed=%llu, attempted_jobs=%llu, unattested_jobs=%llu\n",
						static_cast<unsigned long long>(
							operational.target_rows),
						static_cast<unsigned long long>(
							operational.persisted_index_rows),
						static_cast<unsigned long long>(
							operational.recomputed_target_index_rows),
						static_cast<unsigned long long>(
							stats.attempted_alignment_jobs),
						static_cast<unsigned long long>(
							stats.unattested_alignment_jobs));
				}
				--es->indent;
			}

			es->indent -= 2;
			(void) features;  // currently only has_mmsearch is load-bearing
		} else {
			/*
			 * Structured formats: emit the same per-stage information via
			 * `ExplainOpenGroup` + `ExplainPropertyText` so EXPLAIN (FORMAT
			 * JSON / YAML / XML) consumes the same metadata as the TEXT
			 * renderer (DOC0249 §12.4).
			 */
			ExplainOpenGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
			ExplainPropertyText("Chaining Algorithm",
								chaining_algorithm_name(params.chaining_algorithm), es);
			ExplainPropertyText("Base Alignment Algorithm",
								base_alignment_algorithm_name(params.base_alignment_algorithm), es);
			ExplainPropertyText("Pipeline Concurrency Mode",
				pipeline_mode_text.c_str(), es);
			ExplainPropertyUInteger("Max Concurrency", NULL,
				static_cast<uint64_t>(pipeline_scheduler.max_threads), es);
			ExplainPropertyUInteger("Index Memory Reserved Bytes", NULL,
				static_cast<uint64_t>(
					pipeline_scheduler.index_memory_reserved_bytes), es);
			if (has_pipeline_desc) {
				ExplainOpenGroup("Pipeline Stages", "Pipeline Stages", false, es);
				for (const auto& stage :
					aligner_snapshot->pipeline_desc->stages)
				{
					if (!stage.algorithm.has_value()
						&& !stage.extra_description.has_value())
					{
						continue;
					}
					// Each stage is an unlabeled object inside the "Pipeline
					// Stages" array: a labeled group here would emit
					// `[ "IndexStage": { ... } ]`, which is not valid JSON. The
					// stage name is preserved by the "Stage Name" property below.
					ExplainOpenGroup("Stage", nullptr, true, es);
					ExplainPropertyText("Stage Name",
						stage.stage_name.c_str(), es);
					if (stage.algorithm.has_value()) {
						ExplainPropertyText("Algorithm",
							stage.algorithm->name.c_str(), es);
						for (const auto& p : stage.algorithm->parameters) {
							ExplainPropertyText(p.name.c_str(),
								p.value.c_str(), es);
						}
						for (const auto& p : stage.algorithm->computed_properties) {
							ExplainPropertyText(p.name.c_str(),
								p.value.c_str(), es);
						}
					} else if (stage.extra_description.has_value()) {
						ExplainPropertyText("Description",
							stage.extra_description->c_str(), es);
					}
					if (stage.estimated_batch_size.has_value()) {
						ExplainPropertyUInteger("Estimated Batch Size",
							NULL,
							static_cast<uint64_t>(*stage.estimated_batch_size),
							es);
					}
					ExplainCloseGroup("Stage", nullptr, true, es);
				}
				ExplainCloseGroup("Pipeline Stages", "Pipeline Stages", false, es);
			}
			ExplainCloseGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
			if (explain_runtime != nullptr) {
				const auto& stats = explain_runtime->buffer.execution_stats();
				ExplainPipelineExecutionStats(stats, es);
			}
		}
	}

	if (bioseq_enable_page_cache)
	{
		ExplainOpenGroup("Cache", "Cache", true, es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			appendStringInfoSpaces(es->str, es->indent * 2);
			appendStringInfo(es->str, "->  Cache\n");
			es->indent += 3;
		}

		BioseqColumnCacheStatistics *statistics = BioseqColumnGetCacheStatistics();

		ExplainPropertyUInteger(
			"Cache Hits",
			NULL,
			statistics->hits,
			es);

		ExplainPropertyUInteger(
			"Cache Misses",
			NULL,
			statistics->misses,
			es);

		ExplainPropertyUInteger(
			"Cache Evictions",
			NULL,
			statistics->evictions,
			es);

		ExplainPropertyUInteger(
			"Cache Writes",
			NULL,
			statistics->writes,
			es);

		ExplainPropertyUInteger(
			"Cache Maximum Size",
			NULL,
			statistics->maximumCacheSize,
			es);

		ExplainPropertyUInteger(
			"Cache Ending Size", 
			NULL, 
			statistics->endingCacheSize,
			es);

		ExplainPropertyUInteger(
			"Total Cache Entries",
			NULL,
			statistics->entries,
			es
		);
		ExplainCloseGroup("Cache", "Cache", true, es);
		if (es->format == EXPLAIN_FORMAT_TEXT)
		{
			es->indent -= 3;
		}
	}
}


/* Parallel Execution */

constexpr std::uint32_t kParallelBioseqColumnScanVersion = 2u;

[[nodiscard]] static slice_dsm_worker_header*
BioseqColumnParallelCounterSlot(
		ParallelBioseqColumnScan pscan, std::uint32_t slot_index,
		std::uint32_t expected_entry_count,
		std::uint32_t expected_slot_count) noexcept {
	if (pscan == nullptr
		|| pscan->version != kParallelBioseqColumnScanVersion
		|| pscan->entry_count != expected_entry_count
		|| pscan->slot_count != expected_slot_count
		|| slot_index >= pscan->slot_count
		|| pscan->counters_offset
			< MAXALIGN(offsetof(ParallelBioseqColumnScanData, snapshotData))) {
		return nullptr;
	}
	SliceProjectDsmLayout layout{};
	if (!slice_project_dsm_layout_noexcept(
			pscan->entry_count, pscan->slot_count, &layout)
		|| pscan->slot_stride != layout.slot_stride) return nullptr;
	return reinterpret_cast<slice_dsm_worker_header*>(
		reinterpret_cast<char*>(pscan) + pscan->counters_offset
		+ static_cast<Size>(slot_index) * pscan->slot_stride);
}

static Size 
BioseqColumn_EstimateDSMCustomScan(CustomScanState *node,
								   ParallelContext *pcxt)
{
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);
	if (pcxt == nullptr || pcxt->nworkers < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqScan parallel context is invalid")));
	}
	const Size snapshot_end = add_size(
		offsetof(ParallelBioseqColumnScanData, snapshotData),
		EstimateSnapshotSpace(bioseqScanState->snapshot));
	const Size counters_offset = MAXALIGN(snapshot_end);
	const Size slot_count = add_size(
		static_cast<Size>(pcxt->nworkers), 1u);
	constexpr std::uint32_t entry_count = 0u;
	const Size slot_stride = MAXALIGN(add_size(
		sizeof(slice_dsm_worker_header),
		mul_size(entry_count, sizeof(slice_dsm_counter))));
	return add_size(counters_offset, mul_size(slot_count, slot_stride));
}


static void 
BioseqColumn_InitializeDSMCustomScan(CustomScanState *node,
								 ParallelContext *pcxt,
								 void *coordinate)
{
	ParallelBioseqColumnScan pscan = reinterpret_cast<ParallelBioseqColumnScan>(coordinate);
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);
	if (pcxt == nullptr || pcxt->nworkers < 0 || pscan == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqScan DSM initialization is invalid")));
	}
	const Size snapshot_end = add_size(
		offsetof(ParallelBioseqColumnScanData, snapshotData),
		EstimateSnapshotSpace(bioseqScanState->snapshot));
	const Size counters_offset = MAXALIGN(snapshot_end);
	const Size slot_count = add_size(
		static_cast<Size>(pcxt->nworkers), 1u);
	constexpr std::uint32_t entry_count = 0u;
	const Size slot_stride = MAXALIGN(add_size(
		sizeof(slice_dsm_worker_header),
		mul_size(entry_count, sizeof(slice_dsm_counter))));
	pscan->version = kParallelBioseqColumnScanVersion;
	pscan->counters_offset = counters_offset;
	pscan->entry_count = entry_count;
	pscan->slot_count = static_cast<std::uint32_t>(slot_count);
	pscan->slot_stride = slot_stride;
	std::memset(reinterpret_cast<char*>(pscan) + counters_offset, 0,
		mul_size(slot_count, slot_stride));
	
	/* 
	 * Serialize scan snapshot for workers so they see changes
	 * if we have flushed stripe / row_mask during this scan.
	 */
	SerializeSnapshot(bioseqScanState->snapshot, pscan->snapshotData);

	/* Initialize parallel scan mutex */
	SpinLockInit(&pscan->mutex);

	/* Stripe numbers are starting from index 1 */
	pg_atomic_init_u64(&pscan->nextStripeId, 1);

	if(parallel_leader_participation)
		bioseqScanState->parallelBioseqColumnScan = pscan;
	else
		bioseqScanState->parallelBioseqColumnScan = NULL;
}


static void 
BioseqColumn_ReinitializeDSMCustomScan(CustomScanState *node,
								   ParallelContext *pcxt,
								   void *coordinate)
{
	ParallelBioseqColumnScan pscan = reinterpret_cast<ParallelBioseqColumnScan>(coordinate);
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);
	if (pcxt == nullptr || pcxt->nworkers < 0 || pscan == nullptr
		|| pscan->slot_count
			!= static_cast<std::uint32_t>(pcxt->nworkers + 1)
		|| BioseqColumnParallelCounterSlot(
			pscan, 0u, pscan->entry_count, pscan->slot_count) == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqScan DSM reinitialization is invalid")));
	}

	/* Reset atomic nextStripeId to initial value */
	pg_atomic_init_u64(&pscan->nextStripeId, 1);
	std::memset(reinterpret_cast<char*>(pscan) + pscan->counters_offset, 0,
		mul_size(pscan->slot_count, pscan->slot_stride));

	if(parallel_leader_participation)
		bioseqScanState->parallelBioseqColumnScan = pscan;
	else
		bioseqScanState->parallelBioseqColumnScan = NULL;
}


static void 
BioseqColumn_InitializeWorkerCustomScan(CustomScanState *node,
									shm_toc *toc,
									void *coordinate)
{
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);
	ParallelBioseqColumnScan pscan = reinterpret_cast<ParallelBioseqColumnScan>(coordinate);
	if (pscan == nullptr || pscan->slot_count == 0u
		|| ParallelWorkerNumber < 0
		|| static_cast<std::uint32_t>(ParallelWorkerNumber)
			>= pscan->slot_count - 1u
		|| BioseqColumnParallelCounterSlot(
			pscan, 1u + static_cast<std::uint32_t>(ParallelWorkerNumber),
			pscan->entry_count, pscan->slot_count) == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("BioseqScan worker counter coordinate is invalid")));
	}
	bioseqScanState->parallelBioseqColumnScan = pscan;
	if (bioseqScanState->snapshotRegisteredByUs) {
		UnregisterSnapshot(bioseqScanState->snapshot);
		bioseqScanState->snapshotRegisteredByUs = false;
	}
	bioseqScanState->snapshot = RegisterSnapshot(
		RestoreSnapshot(pscan->snapshotData));
	bioseqScanState->snapshotRegisteredByUs = true;
	bioseq_runtime(bioseqScanState).buffer.replace_executor_snapshot(
		bioseqScanState->snapshot);
}


/*
 * BioseqColumnPushdownClausesStr represents the clauses to push down as a string.
 */
static const char *
BioseqColumnPushdownClausesStr(List *context, List *clauses)
{
	Expr *conjunction;

	PList<Expr> clauseList{ clauses };
	Assert(clauseList.length() > 0);

	if (clauseList.length() == 1)
	{
		conjunction = clauseList.first();
	}
	else
	{
		conjunction = make_andclause(clauses);
	}

	bool useTableNamePrefix = false;
	bool showImplicitCast = false;
	return deparse_expression(reinterpret_cast<Node *>(conjunction), context,
							  useTableNamePrefix, showImplicitCast);
}


/*
 * BioseqColumnProjectedColumnsStr generates projected column string for
 * explain output.
 */
static const char *
BioseqColumnProjectedColumnsStr(List *context, List *projectedColumns)
{
	if (list_length(projectedColumns) == 0)
	{
		return "<columnar optimized out all columns>";
	}

	bool useTableNamePrefix = false;
	bool showImplicitCast = false;
	return deparse_expression(reinterpret_cast<Node *>(projectedColumns), context,
							  useTableNamePrefix, showImplicitCast);
}


/*
 * BioseqColumnVarNeeded returns a list of Var objects for the ones that are
 * needed during columnar custom scan.
 * Throws an error if finds a Var referencing to an attribute not supported
 * by BioseqColumnScan.
 */
static List *
BioseqColumnVarNeeded(BioseqScanState *bioseqScanState)
{
	ScanState *scanState = &bioseqScanState->custom_scanstate.ss;

	PList<Var> varList;

	PBitmapset neededAttrSet =
		BioseqColumnAttrNeeded(scanState, bioseqScanState->vectorization.vectorizedQualList);

	for (int bmsMember : neededAttrSet)
	{
		Relation bioseqColumnRelation = scanState->ss_currentRelation;

		/* neededAttrSet already represents 0-indexed attribute numbers */
		Form_pg_attribute columnForm =
			TupleDescAttr(RelationGetDescr(bioseqColumnRelation), bmsMember);
		if (columnForm->attisdropped)
		{
			bioseq_report<ERR, PG>().code(ERRCODE_UNDEFINED_COLUMN)
							.msg("cannot explain column with attrNum={} "
								 "of columnar table {} since it is dropped",
								 bmsMember + 1,
								 RelationGetRelationName(bioseqColumnRelation));
		}
		else if (columnForm->attnum <= 0)
		{
			/*
			 * BioseqColumnAttrNeeded should have already thrown an error for
			 * system columns. Similarly, it should have already expanded
			 * whole-row references to individual attributes.
			 */
			bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
							.msg("cannot explain column with attrNum={} "
								 "of columnar table {} since it is either "
								 "a system column or a whole-row "
								 "reference", columnForm->attnum,
								 RelationGetRelationName(bioseqColumnRelation));
		}


		/*
		 * varlevelsup is used to figure out the (query) level of the Var
		 * that we are investigating. Since we are dealing with a particular
		 * relation, it is useless here.
		 */
		Index varlevelsup = 0;

		CustomScan *customScan = reinterpret_cast<CustomScan *>(bioseqScanState->plan_state()->plan);
		Index scanrelid = customScan->scan.scanrelid;
		Var *var = makeVar(scanrelid, columnForm->attnum, columnForm->atttypid,
						   columnForm->atttypmod, columnForm->attcollation,
						   varlevelsup);
		varList.append(var);
	}

	return varList.release();
}


/*
 * set_deparse_context_planstate forwards to set_deparse_context_plan, going
 * through the PlanState* indirection that the EXPLAIN code already holds.
 */
static List *
set_deparse_context_planstate(List *dpcontext, Node *node, List *ancestors)
{
	PlanState *ps = reinterpret_cast<PlanState *>(node);
	return set_deparse_context_plan(dpcontext, ps->plan, ancestors);
}
