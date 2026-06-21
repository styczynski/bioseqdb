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
// bioseq_sequences.pipe, bioseq_sidecar, bioseq_scheduler.dispatch_buffer,
// bioseq_types.pg_compact_sequence), clang's loader has a TU-local "anchor"
// copy of `hwy::N_NEON::*` to collapse the per-BMI copies against. Without
// this anchor the loader can't merge them and emits
// `definition with same mangled name '_ZN3hwy6N_NEONL7BitCast...' as another
// definition`. Mirrors the fix applied to bioseq_io_bench/bench_test.cpp
// (DOC0192 §6). common_simd.h forwards #include <hwy/highway.h>.
#include "common_simd.h"
#include "common.h"

#include <algorithm>
#include <cmath>


extern "C" {
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
import bioseq.storage.metadata;
import bioseq.table.types;
import bioseq.table.scan_state;
import bioseq.pg_utils;
import bioseq.scheduler.dispatch_buffer;
import bioseq.scheduler.supervisor_pg;
import bioseq.parallelism.mode;
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
constexpr std::size_t kBioseqRuntimeBufferReserve = 1000;

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
 * The Minimap2Params struct lives in bioseq::simd::N_NEON.
 */
struct PerScanAlignerState {
	bioseq::simd::N_NEON::Minimap2Params params{};
	BioseqScanFeatureFlags              features{};
	bool                                params_captured = false;

	/*
	 * Structured pipeline description captured at BeginCustomScan via
	 * `bioseq::simd::N_NEON::pipe::describe_pipeline_for(...)`.  Populated
	 * once per scan (cold path) and consumed by
	 * `BioseqColumnScan_ExplainCustomScan` to render the per-stage breakdown
	 * without hardcoded knowledge of which parameters belong to which
	 * stage (DOC0249 §11, §12.2).
	 */
	std::optional<bioseq::simd::N_NEON::pipe::PipelineDescription> pipeline_desc{};
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
 * `paramid` follows pgsql's PARAM_EXEC ID space (matches Param->paramid).
 * `paramkind` is captured so we can validate it really is PARAM_EXEC at
 * ReScan (PARAM_EXTERN would route through a different ParamListInfo
 * member and is not currently supported).
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
	 * Stash a pointer to the SPECIFIC mmsearch FuncExpr that
	 * registered this rebind. Without this, ReScanCustomScan would
	 * use find_mmsearch_funcexpr() which returns the FIRST
	 * mmsearch in plan.qual order — and the planner is free to
	 * order quals differently from our priority-classified order.
	 * For Param + Const-extra stacking the planner often places
	 * the Const first, so find_mmsearch_funcexpr would return the
	 * Const FuncExpr (whose arg1 is a Const) and the Param-rebind
	 * loop would silently fall through, leaving the buffer's
	 * primary mmsearch with stale bytes. Stashing the
	 * Param-driving FuncExpr at register time lets ReScan resolve
	 * the correct Param every rescan.
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
	bioseq::simd::N_NEON::TupleDispatchBuffer buffer{kBioseqRuntimeBufferReserve};
	BioseqPlaceholderConsts                   placeholder_consts{};
	List*                                     newtargetlist = nullptr;
	PerScanAlignerState                       aligner_state{};
	MmsearchParamRebind                       param_rebind{};

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
};

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
	result.mmsearch_oid              = resolve("mmsearch(bioseq.seq, bioseq.seq)");
	result.mmsearch_with_opts_oid    = resolve("mmsearch(bioseq.seq, bioseq.seq, bioseq_aligner_options)");
	result.bioseq_seq_type_oid       = resolve_type("bioseq.seq");
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
	return result;
}

/*
 * Per-backend cache for BioseqStubOids. The prior implementation called
 * LookupStubOids() from every planner path entry (AddBioseqColumnScanPath,
 * CustomExecScan first-call init, ExplainCustomScan) — that is ~22
 * regprocedurein syscache hits per query. The cache is backed by syscache
 * invalidation for pg_proc OIDs, plus relcache invalidation for relation-driven
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
	};
	for (const Oid oid : func_oids) {
		if (!oid_exists_in_syscache(PROCOID, oid)) {
			return false;
		}
	}
	return oid_exists_in_syscache(TYPEOID, o.bioseq_seq_type_oid);
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
		invalidation_registered = true;
	}
	g_cached_stub_oids.oids        = LookupStubOids();
	g_cached_stub_oids.initialised = true;
	return g_cached_stub_oids.oids;
}

/*
 * Classifier used by AddBioseqColumnScanPath's wehavemagic sweep and by
 * BeginCustomScan's primary-key discovery. Returns true if funcid
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

/*
 * mmsearch() must remain in scan.plan.qual so ExecQual and the custom
 * mmsearch runtime see it, but the columnar reader's qual list is only used
 * for min/max chunk pruning. The reader cannot prune on mmsearch and deep
 * copying that FuncExpr during table_beginscan has crashed on same-row
 * mmsearch(seq, seq) plans, so strip those clauses at the storage boundary.
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
[[nodiscard]] static bioseq::simd::N_NEON::Minimap2Params
build_params_from_gucs()
{
	bioseq::simd::N_NEON::Minimap2Params params;
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
		static_cast<bioseq::simd::N_NEON::ChainingAlgorithmType>(
			bioseq_aligner_chaining_algorithm);
	params.base_alignment_algorithm =
		static_cast<bioseq::simd::N_NEON::BaseAlignmentAlgorithmType>(
			bioseq_aligner_base_alignment_algorithm);
	params.minimizer_algorithm =
		static_cast<bioseq::simd::N_NEON::MinimizerAlgorithmType>(
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
static void
apply_aligner_options_override(FuncExpr* mmsearch_fe,
							   bioseq::simd::N_NEON::Minimap2Params& params,
							   const BioseqStubOids& oids)
{
	(void) oids; // composite decoding is self-contained
	if (mmsearch_fe == nullptr) {
		return;
	}
	if (list_length(mmsearch_fe->args) < 3) {
		return;
	}
	PList<Node> args(mmsearch_fe->args);
	Node* opt_node = args.third();
	Const* opt_const = pg_node_try_as<Const>(opt_node);
	if (opt_const == nullptr) {
		return;
	}
	if (opt_const->constisnull) {
		return;
	}
	/*
	 * The composite comes through as a HeapTupleHeader varlena. Deconstruct
	 * it via heap_deform_tuple against the pg_type tupdesc.
	 */
	HeapTupleHeader header = reinterpret_cast<HeapTupleHeader>(
		PG_DETOAST_DATUM(opt_const->constvalue));
	Oid row_typeoid    = HeapTupleHeaderGetTypeId(header);
	int32 row_typmod   = HeapTupleHeaderGetTypMod(header);
	TupleDesc tupdesc  = lookup_rowtype_tupdesc(row_typeoid, row_typmod);

	HeapTupleData heap_tuple{};
	heap_tuple.t_len  = HeapTupleHeaderGetDatumLength(header);
	heap_tuple.t_data = header;
	ItemPointerSetInvalid(&heap_tuple.t_self);
	heap_tuple.t_tableOid = InvalidOid;

	const int n_attrs = tupdesc->natts;
	/*
	 * Stack-sized buffers sized above the field count of the
	 * bioseq_aligner_options composite (today's count is 32 after the DOC0304
	 * vulkan_enabled field; 40 leaves headroom). Avoids the per-call heap
	 * allocation that std::vector<Datum> would add and keeps us clear of
	 * std::vector<bool>'s proxy storage (no .data()).
	 */
	constexpr int kMaxOptionAttrs = 40; // current bioseq_aligner_options uses 32 fields
	if (n_attrs > kMaxOptionAttrs) {
		ReleaseTupleDesc(tupdesc);
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq_aligner_options has %d fields, max supported is %d",
					n_attrs, kMaxOptionAttrs)));
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
				static_cast<bioseq::simd::N_NEON::ChainingAlgorithmType>(raw);
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
				static_cast<bioseq::simd::N_NEON::BaseAlignmentAlgorithmType>(raw);
		} else {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("aligner_options.base_alignment_algorithm out of range: %d (expected 0..1)", raw)));
		}
	}
	++idx;
	/*
	 * DOC0307 Phase B — minimizer_algorithm. Known-good ordinals:
	 * 0=SCALAR, 1=SIMD_MINIMAP2_SLOW, 2=TWO_STACK_NTHASH, 3=VULKAN_MINIMAP_INT.
	 */
	if (idx < n_attrs && !nulls[idx]) {
		const int32_t raw = DatumGetInt32(values[idx]);
		if (raw >= 0 && raw <= 3) {
			params.minimizer_algorithm =
				static_cast<bioseq::simd::N_NEON::MinimizerAlgorithmType>(raw);
		} else {
			ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("aligner_options.minimizer_algorithm out of range: %d (expected 0..3)", raw)));
		}
	}
	++idx;
	/*
	 * vulkan_enabled (DOC0304 §7.4) — decoded as a boolean; a NULL leaves the
	 * GUC-derived default in place.
	 */
	if (idx < n_attrs && !nulls[idx]) {
		params.vulkan_enabled = DatumGetBool(values[idx]);
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
	ReleaseTupleDesc(tupdesc);
	/* Free the detoasted copy if PG_DETOAST_DATUM actually copied. */
	if (reinterpret_cast<void*>(header)
		!= reinterpret_cast<void*>(DatumGetPointer(opt_const->constvalue))) {
		pfree(header);
	}
}

/*
 * Resolve the first mmsearch FuncExpr in `qual` (or nullptr when the plan
 * has no mmsearch filter). Shared between the params-override pass and the
 * BeginCustomScan buffer configure call site.
 */
[[nodiscard]] static FuncExpr*
find_mmsearch_funcexpr(List* qual, const BioseqStubOids& oids)
{
	for (auto q : PList<Node>(qual)) {
		FuncExpr* fe = pg_node_try_as<FuncExpr>(q);
		if (fe == nullptr) {
			continue;
		}
		if (is_mmsearch_funcid(fe->funcid, oids)) {
			return fe;
		}
	}
	return nullptr;
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
static get_relation_info_hook_type PreviousGetRelationInfoHook = NULL;

static bool EnableBioseqColumnCustomScan = true;
static bool EnableBioseqColumnQualPushdown = true;
static double BioseqColumnQualPushdownCorrelationThreshold = 0.4;
static int BioseqColumnMaxCustomScanPaths = 64;
static int BioseqColumnPlannerDebugLevel = DEBUG3;


const CustomPathMethods BioseqColumnScanPathMethods = {
	.CustomName = "BioseqScan",
	.PlanCustomPath = BioseqColumnScanPath_PlanCustomPath,
	.ReparameterizeCustomPathByChild = BioseqColumnScanPath_ReparameterizeCustomPathByChild,
};

const CustomScanMethods BioseqColumnScanScanMethods = {
	.CustomName = "BioseqScan",
	.CreateCustomScanState = BioseqColumnScan_CreateCustomScanState,
};

const CustomExecMethods BioseqColumnScanExecuteMethods = {
	.CustomName = "BioseqScan",

	.BeginCustomScan = BioseqColumnScan_BeginCustomScan,
	.ExecCustomScan = BioseqColumnScan_ExecCustomScan,
	.EndCustomScan = BioseqColumnScan_EndCustomScan,
	.ReScanCustomScan = BioseqColumnScan_ReScanCustomScan,

	.ExplainCustomScan = BioseqColumnScan_ExplainCustomScan,

	.EstimateDSMCustomScan = BioseqColumn_EstimateDSMCustomScan,
	.InitializeDSMCustomScan = BioseqColumn_InitializeDSMCustomScan,
	.ReInitializeDSMCustomScan = BioseqColumn_ReinitializeDSMCustomScan,
	.InitializeWorkerCustomScan = BioseqColumn_InitializeWorkerCustomScan
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
	const BioseqStubOids& stub_oids = get_cached_stub_oids();
	BioseqProjectionPresenceProbe probe{ &stub_oids, false };
	projection_presence_visitor visitor;
	pg_walk_expression_including_root(node, probe, visitor);
	return probe.found;
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

	RegisterCustomScanMethods(&BioseqColumnScanScanMethods);
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
		}
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
	add_path(rel, bioseqColumnScanPath);
	if (bioseq_enable_parallel_execution)
		bioseqColumnScanPath->total_cost += bioseqColumnScanPath->rows * 0.1;

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

			parallelBioseqColumnScanPath->parallel_workers = bioseq_min_parallel_process_running;
			parallelBioseqColumnScanPath->parallel_aware = true;
			AdjustBioseqColumnParallelScanCost(parallelBioseqColumnScanPath);

			add_partial_path(rel, parallelBioseqColumnScanPath);
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
 * Walker context for bioseq_projection_walker — pairs a stub-OID
 * snapshot with an integer counter incremented for each detected
 * bioseq.<...>(...) call site. The walker handles FuncExprs at any
 * depth, including children of PlaceHolderVar wrappers introduced by
 * bioseq_placeholder_support when the query has joins or aggregates.
 */
struct BioseqProjectionCounter {
	const BioseqStubOids* stub_oids;
	int* count;
};

struct projection_counter_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, BioseqProjectionCounter& data) const {
		if (FuncExpr* fe = node.try_as<FuncExpr>()) {
			if (is_mmsearch_funcid(fe->funcid, *data.stub_oids)) {
				bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() detected mmsearch funcexpr (any depth)");
				++(*data.count);
			} else if (is_bioseq_projection_funcid(fe->funcid, *data.stub_oids)) {
				bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() detected bioseq projection funcid={}",
						fe->funcid);
				++(*data.count);
			} else if (fe->funcid == data.stub_oids->symbol_count_oid
					   && list_length(fe->args) == 2) {
				/*
				 * symbol_count(Var, Const) is a cost inflator: the scalar
				 * Postgres-level function path is O(n) over the varlena,
				 * but the custom scan resolves it via a single O(1) array
				 * index into meta_sidecar.counts[]. We count every call
				 * site so the negative-cost adjustment scales with the
				 * number of rewrites VarInjectorMutator will perform.
				 */
				bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() detected bioseq.symbol_count funcexpr");
				++(*data.count);
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

	bool wehavemagic = false;
	int wehavemagic_cnt = 0;
	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() mmsearch_oid={} score_oid={} cigar_oid={} query_start_oid={} symbol_count_oid={}",
		stub_oids.mmsearch_oid, stub_oids.score_oid, stub_oids.cigar_oid, stub_oids.query_start_oid,
		stub_oids.symbol_count_oid);
	/*
	 * Tree-walk the parse tlist (and its expression children) to count
	 * every bioseq.* FuncExpr. A flat top-level scan misses three real
	 * shapes that need the custom scan to win:
	 *   (a) Targetlist entries holding a TargetEntry whose expr is a
	 *       PlaceHolderVar — this is the post-planner-support shape for
	 *       any bioseq.* projection in a query with JOINs or aggs.
	 *   (b) Composite expressions like `bioseq.cigar(t1.pk) || '!'`
	 *       where the FuncExpr is a child of a higher operator.
	 *   (c) `symbol_count(Var, 'L')` calls nested similarly.
	 * Without the walker, picking PHV-wrapping in
	 * bioseq_placeholder_support would silently lose the cost reduction
	 * and the planner would prefer Index Scan / Seq Scan over our
	 * custom scan — observed for the LEFT JOIN regression test.
	 */
	BioseqProjectionCounter walker_data{ &stub_oids, &wehavemagic_cnt };
	projection_counter_visitor projection_visitor;
	pg_walk_expression_including_root(
		reinterpret_cast<Node*>(root->parse->targetList), walker_data, projection_visitor);
	wehavemagic = wehavemagic_cnt > 0;

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
	PList<RestrictInfo> allClauses = copyObject(rel->baserestrictinfo);
	if (path->param_info != NULL)
	{
		allClauses.concat(path->param_info->ppi_clauses);
	}

	bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() iterating allClauses (no-op pass for tracing)");
	for (auto restrict_info : allClauses)
	{
		(void) restrict_info;
	}

	allClauses = FilterPushdownClauses(root, rel, allClauses);

	/*
	 * Count mmsearch FuncExprs that landed in this path's scan-quals
	 * (allClauses already contains both baserestrictinfo and the
	 * `ppi_clauses` Postgres added because `paramRelids` brings them
	 * into scope). For an unparameterised path, a cross-table
	 * mmsearch like `mmsearch(t1.seq, t2.seq2)` lives in the join
	 * filter and is NOT in allClauses; for the matching
	 * parameterised path, it IS — and we want the planner to pick
	 * that one. Each such qual contributes another unit of
	 * wehavemagic so the parameterised path's negative cost is
	 * strictly more negative than the unparameterised competitor.
	 *
	 * Without this boost the unparameterised path's cost matches the
	 * parameterised one (both have wehavemagic_cnt = 1 from the
	 * targetlist's bioseq.cigar projection). The planner then picks
	 * the simpler unparameterised path + Nested-Loop-with-join-filter
	 * shape, which silently runs the mmsearch C stub (always TRUE)
	 * and emits every cross-product row with NULL bioseq.* columns —
	 * the regression the user reported.
	 */
	{
		const BioseqStubOids& mmsearch_oids = get_cached_stub_oids();
		for (auto rinfo : allClauses) {
			Node* clause_node = reinterpret_cast<Node*>(rinfo->clause);
			if (FuncExpr* fe = pg_node_try_as<FuncExpr>(clause_node)) {
				if (is_mmsearch_funcid(fe->funcid, mmsearch_oids)) {
					wehavemagic = true;
					++wehavemagic_cnt;
					bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() mmsearch in scan quals adds wehavemagic; new cnt={}",
							wehavemagic_cnt);
				}
			}
		}
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

	if (wehavemagic) {
		//(&cpath->path)->total_cost -= 90000;
		PathTarget* pt = (&cpath->path)->pathtarget;
		//auto curcost = pt->cost.per_tuple;

		/*
		 * Estimate what fraction of the scanned relation's rows has a
		 * prebuilt minimizer index in bioseq_internal.index_sidecar and
		 * scale an additional cost reduction by that factor. A table with
		 * 100% sidecar coverage (bioseq.update_index_on_insert = on for
		 * every INSERT) wins by a fuller margin than one that was bulk-
		 * loaded with the GUC off. See SPEC_MINIMIZER_STORAGE.md §8 for
		 * how the GUC toggling interacts with scan-time backfill.
		 *
		 * estimate_index_presence() clamps to [0, 1]; the resulting
		 * delta is in [0, 10000]. wehavemagic_cnt's 100000-per-call
		 * reduction still dominates so the custom scan always wins when
		 * a rewrite is possible — the sidecar coverage just adjusts the
		 * *margin*, not the *direction*, of the preference.
			 */
		const float index_presence =
			bioseq::storage::indexing::estimate_index_presence(
				rte->relid, rel->tuples,
				bioseq::indexing::DEFAULT_ACTIVE_READ_INDEX);
		const double sidecar_bonus = 10000.0 * static_cast<double>(index_presence);

		bioseq_log<DBG>("[bioseq] pg: AddBioseqColumnScanPath() reducing mmsearch cost path_total={} path_startup={} pertuple={} pt_startup={} cpath_startup={} cpath_total={} magic_cnt={} index_presence={:.4f}",
			path->total_cost, path->startup_cost, pt->cost.per_tuple, pt->cost.startup,
			(&cpath->path)->startup_cost, (&cpath->path)->total_cost,
			wehavemagic_cnt, static_cast<double>(index_presence));
		/* Apply the sidecar bonus to BOTH startup and total cost so the
		 * Postgres cost-model invariant (startup_cost <= total_cost)
		 * stays satisfied. Without the double-subtract the two costs
		 * diverge when the bonus is positive and some planner paths
		 * (parallel sort, partition pruning) rely on the invariant
		 * when ranking candidate plans. */
		cpath->path.total_cost   = -100000.0 * wehavemagic_cnt - sidecar_bonus;
		cpath->path.startup_cost = -100000.0 * wehavemagic_cnt - sidecar_bonus;
	}


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

	cscan->scan.plan.qual = extract_actual_clauses(
		clauses, false /* no pseudoconstants */);

		const BioseqStubOids& plan_stub_oids = get_cached_stub_oids();
		for(auto n : PList<TargetEntry>(root->processed_tlist)) {
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

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScanPath_PlanCustomPath() returning");
	return reinterpret_cast<Plan *>(cscan);
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


static void
BioseqColumnScan_BeginCustomScan(CustomScanState *cscanstate, EState *estate, int eflags)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() entered");
	CustomScan *cscan = reinterpret_cast<CustomScan *>(cscanstate->ss.ps.plan);
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(cscanstate);
	ExprContext *stdecontext = cscanstate->ss.ps.ps_ExprContext;
	cscanstate->ss.ss_currentScanDesc = NULL;

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
	for (auto private_const_node : PList<Const>(cscan->custom_private))
	{
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
		bioseq_enable_vectorization &&
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
	 * Walk cscan->scan.plan.qual to discover the WHERE mmsearch(...) call,
	 * pull out (varattno of the TEXT column, query literal) and configure
	 * the dispatch buffer so submit_scan has enough context to emit the
	 * per-tuple log line and buffer the right column values.
	 */
	BioseqScanRuntime& runtime = bioseq_runtime(bioseqScanState);
	/* Begin is the only place that should drop extras as part of the
	 * primary reset — every Begin call repopulates them from scratch
	 * via the qual loop. ReScan / failure paths must NOT drop extras
	 * (see clear_mmsearch_context's comment for the §9 review fix). */
	runtime.buffer.clear_mmsearch_context(/*clear_extras=*/true);
	runtime.param_rebind = MmsearchParamRebind{};

	Relation relation_for_attname = cscanstate->ss.ss_currentRelation;
	const Oid relation_oid_for_attname = RelationGetRelid(relation_for_attname);

	/*
	 * State threaded across the qual loop so subsequent mmsearch
	 * quals can be registered as additional Const-driven filters.
	 *
	 * `primary_configured` flips to true once the first acceptable
	 * mmsearch qual is installed (either via configure_mmsearch for
	 * the literal-query case, configure_mmsearch_per_row for the
	 * same-rel Var case, or via param_rebind for the
	 * parametrised-NL case). Every subsequent mmsearch qual we see
	 * is treated as an additional AND-filter — accepted when:
	 *   - the new qual is Const-driven (Param/Var-same-rel extras
	 *     would need their own per-rescan resolve machinery and
	 *     are still rejected with NOTICE),
	 *   - the new qual's first Var is the SAME column as the
	 *     primary's,
	 *   - and the new qual's typmod matches the primary's.
	 * Param-primary + Const-extra IS supported as of §9.4 — the
	 * primary rebinds per-rescan and the Const extras are added
	 * once during BeginCustomScan and intersected via match_map
	 * filter inside `run_batch_and_build_cursors`.
	 *
	 * `primary_seq_attno` / `primary_typmod_packed` carry the
	 * primary's column and typmod for these comparisons.
	 */
	bool primary_configured = false;
	bool primary_is_const = false;
	AttrNumber primary_seq_attno = InvalidAttrNumber;
	int32 primary_typmod_packed = -1;

	/*
	 * Two-pass qual processing. The previous single-pass loop
	 * processed mmsearch quals in `cscan->scan.plan.qual` order,
	 * which is not stable across plans (the planner is free to
	 * reorder). For Param + Const stacking that bit us: when the
	 * planner placed the Const first, the Const became the
	 * primary and the Param-driven qual was rejected as an extra
	 * (Param can't be a Const-extra). The Param qual then evaluated
	 * as the always-true C stub and silently dropped from the AND.
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
	 * multi-Param or multi-Var-same-rel. The user gets a NOTICE
	 * line and the qual still evaluates as the C stub for those
	 * dropped cases — same behaviour as before, just made explicit.
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
	for (auto qual_node : PList<Node>(cscan->scan.plan.qual))
	{
		FuncExpr* fe_classify = pg_node_try_as<FuncExpr>(qual_node);
		if (fe_classify == nullptr) {
			continue;
		}
		if (!is_mmsearch_funcid(fe_classify->funcid, scan_stub_oids)) {
			continue;
		}
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
	 * scan (they consume the single primary slot); subsequent quals
	 * of the same priority are rejected with a NOTICE.
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
			if (query_const->consttype != scan_stub_oids.bioseq_seq_type_oid
				|| query_const->constisnull)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 is not a non-null bioseq.seq Const (type={}, null={})",
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
			/*
			 * Only PARAM_EXEC is the planner-internal kind that
			 * appears for parameterized inner-side NL scans (the
			 * scenario we want to support — outer Var pushed into
			 * the inner side as $0). PARAM_EXTERN comes from the
			 * client protocol and would route through
			 * `econtext->ecxt_param_list_info` rather than
			 * `estate->es_param_exec_vals`; treating it the same
			 * way would silently miss the rebind. Reject other
			 * kinds for now so the failure is loud rather than
			 * "rows but cigar empty".
			 */
			if (query_param->paramkind != PARAM_EXEC)
			{
				bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 Param has paramkind={}, only PARAM_EXEC supported — bioseq.* projections will be NULL",
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
		if (arg1_typmod < 0)
		{
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() mmsearch arg1 has unconstrained typmod; add ::bioseq.seq('dna4', N) to the literal/param");
			continue;
		}
		const auto unpacked_typmod      = compact_sequence_stdc_type_params::decode(col_var->vartypmod);
		const auto unpacked_arg1_typmod = compact_sequence_stdc_type_params::decode(arg1_typmod);
		const bool typmod_match =
			(unpacked_typmod.alphabet_code == unpacked_arg1_typmod.alphabet_code)
			&& (unpacked_typmod.storage_unit_byte_width == unpacked_arg1_typmod.storage_unit_byte_width);
		const bool same_alphabet =
			unpacked_typmod.alphabet_code == unpacked_arg1_typmod.alphabet_code;
		/*
		 * For Param shapes (cross-table mmsearch) we historically
		 * required exact typmod match; the runtime rebind path does
		 * NOT have a re-encode hook today (the Param value is only
		 * resolved during ReScan). Keep that invariant: reject
		 * Param mismatches so users get a clear log line instead of
		 * silent NULLs.
		 */
		if (arg1_is_param && !typmod_match)
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

		/*
		 * Subsequent mmsearch quals get treated as additional AND
		 * filters. We require: (a) the primary was Const-driven,
		 * (b) the extra is also Const-driven, (c) same seq column,
		 * (d) matching typmod. Anything else is rejected: the qual
		 * stays in `cscan->scan.plan.qual` and ExecQual evaluates
		 * mmsearch via the always-true C stub, which silently makes
		 * the AND-filter behaviour incorrect. We surface this via
		 * ereport(NOTICE, ...) so the user sees the rejection
		 * (default client_min_messages includes NOTICE) instead of
		 * having to enable bioseq.logging.
		 */
		if (primary_configured)
		{
			/*
			 * Multi-mmsearch stacking rules:
			 *   - Primary Const + Extra Const   ✓  AND-filter (already worked)
			 *   - Primary Param + Extra Const   ✓  AND-filter — the
			 *     primary query rebinds per-rescan via the Param
			 *     path; the Const extras stay fixed and ride along
			 *     as additional row filters. This was historically
			 *     rejected with NOTICE but does not actually
			 *     conflict with the Param-rebind path: the const
			 *     extras only contribute to match_map filtering
			 *     after the primary batch ran.
			 *   - Primary Const + Extra Param   ✗  rejected (Param
			 *     extras would need their own per-rescan resolve;
			 *     the buffer's `extra_query_varlenas` is bytes-only
			 *     today, no Param-coords slot exists).
			 *   - Primary Param + Extra Param   ✗  rejected (would
			 *     need multi-Param rebind machinery — see §9.7 of
			 *     REPORT_POSTGRES_QUERY_BUGFIXING_1.md for the
			 *     follow-up plan).
			 */
			if (!arg1_is_const) {
				ereport(NOTICE,
					(errmsg("bioseq: additional mmsearch qual ignored: only Const-typed extras are supported beyond the primary qual (extra_is_const=%d)",
							arg1_is_const),
					 errhint("Combine quals with the same column and a literal query to AND-filter rows.")));
				pfree(attname_cstr);
				continue;
			}
			if (col_var->varattno != primary_seq_attno) {
				ereport(NOTICE,
					(errmsg("bioseq: additional mmsearch qual ignored: column attno=%d differs from primary attno=%d",
							static_cast<int>(col_var->varattno),
							static_cast<int>(primary_seq_attno))));
				pfree(attname_cstr);
				continue;
			}
			if (col_var->vartypmod != primary_typmod_packed) {
				ereport(NOTICE,
					(errmsg("bioseq: additional mmsearch qual ignored: typmod=%d differs from primary=%d",
							col_var->vartypmod, primary_typmod_packed)));
				pfree(attname_cstr);
				continue;
			}
			Const* query_const = reinterpret_cast<Const*>(arg1);
			pgseq_t* extra_query_hdr = reinterpret_cast<pgseq_t*>(
				PG_DETOAST_DATUM(query_const->constvalue));
			const size_t extra_total_bytes = static_cast<size_t>(VARSIZE(extra_query_hdr));
			std::vector<char> extra_varlena_bytes(extra_total_bytes);
			std::memcpy(extra_varlena_bytes.data(), extra_query_hdr, extra_total_bytes);
			if (reinterpret_cast<void*>(extra_query_hdr)
				!= reinterpret_cast<void*>(DatumGetPointer(query_const->constvalue)))
			{
				pfree(extra_query_hdr);
			}
			runtime.buffer.add_extra_const_query(std::move(extra_varlena_bytes));
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() registered additional mmsearch (extra count now {})",
					runtime.buffer.extra_query_count());
			pfree(attname_cstr);
			continue;
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
						: ::bioseq::simd::N_NEON::stdc_recode_to_type_params(
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
			primary_is_const = true;
		}
		else if (arg1_is_param)
		{
			/*
			 * Parameterized-query case. Stash the Param coordinates so
			 * BioseqColumnScan_ReScanCustomScan can resolve them per-rescan
			 * via EvalParamsMutator. The buffer's mmsearch_context stays
			 * inactive at Begin; ReScan installs it (or skips this scan
			 * iteration when the Param resolves to NULL).
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
			primary_is_const = false;
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
			primary_is_const = false;
			bioseq_log<DBG>("[bioseq] pg: BeginCustomScan() registering per-row mmsearch col_attno={} arg1_attno={} self_alignment={}",
					static_cast<int>(col_var->varattno),
					static_cast<int>(query_var->varattno),
					self_alignment);
		}

		primary_configured = true;
		primary_seq_attno = col_var->varattno;
		primary_typmod_packed = col_var->vartypmod;

		pfree(attname_cstr);
		/*
		 * Don't break — subsequent loop iterations that also match
		 * an mmsearch FuncExpr will fall into the
		 * `primary_configured` branch above and stack as additional
		 * AND filters.
		 */
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
	const BioseqScanFeatureFlags features = detect_feature_flags(
		cscan->scan.plan.targetlist, cscan->scan.plan.qual, scan_stub_oids);
	bioseq::simd::N_NEON::Minimap2Params aligner_params = build_params_from_gucs();
	FuncExpr* mmsearch_fe = find_mmsearch_funcexpr(
		cscan->scan.plan.qual, scan_stub_oids);
	apply_aligner_options_override(mmsearch_fe, aligner_params, scan_stub_oids);
	aligner_params.do_cigar           = features.uses_cigar;
	aligner_params.compute_divergence = features.uses_divergence;
	aligner_params.compute_mapq       = features.uses_mapq;
	const bool selected_vulkan =
		aligner_params.vulkan_seed_hits_enabled
		|| aligner_params.vulkan_fused_frontend_enabled
		|| aligner_params.vulkan_gpu_index_build_enabled
		|| aligner_params.vulkan_extend_enabled
		|| aligner_params.minimizer_algorithm
			== bioseq::simd::N_NEON::MinimizerAlgorithmType::VULKAN_MINIMAP_INT
		|| aligner_params.chaining_algorithm
			== bioseq::simd::N_NEON::ChainingAlgorithmType::VULKAN_TILED_DP;
	if (features.has_mmsearch && selected_vulkan && !aligner_params.vulkan_enabled) {
		bioseq_report<ERR, PG>()
			.code(ERRCODE_INTERNAL_ERROR)
			.msg("Vulkan aligner execution failed")
			.detail("selected Vulkan execution requires bioseq.aligner.vulkan_enabled=on")
			.hint("Set bioseq.aligner.vulkan_enabled=on, or select non-Vulkan minimizer/chaining algorithms before dispatch.");
	}
	if (features.has_mmsearch && selected_vulkan && aligner_params.vulkan_enabled) {
		bioseq::parallelism::pg::install_pg_shader_provider_if_available();
		bioseq::parallelism::pg::wait_for_runtime_ready();
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
		runtime.aligner_state.pipeline_desc =
			bioseq::simd::N_NEON::pipe::describe_pipeline_for(
				aligner_params, empty_query_proto, empty_target_proto);
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
							features.has_mmsearch && bioseq_read_sequence_index,
							resolved_user_relid,
							scan_db_oid, scan_spc_oid, scan_relfile);

	bioseq_log<DBG>("[bioseq] pg: BioseqColumnScan_BeginCustomScan() scan initialised successfully");
	/* scan slot is already initialized */
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
 * (expanded to N_NEON on this arm64 target); this file does not use
 * HWY_BEFORE_NAMESPACE() so we qualify with the full path.
 */
static void
populate_placeholder_consts(BioseqPlaceholderConsts& consts,
							const bioseq::simd::N_NEON::MatchResult& match)
{
	using bioseq::simd::N_NEON::format_cigar;
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
static void
populate_symbol_count_placeholders(BioseqPlaceholderConsts& consts,
								   Relation main, int64_t rowno)
{
	if (consts.symbol_count_sites.empty()) {
		return;
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
	Snapshot snap  = GetTransactionSnapshot();

	MemoryContext scratch = AllocSetContextCreate(CurrentMemoryContext,
		"bioseq_symcount", ALLOCSET_SMALL_SIZES);
	bioseq::storage::seq_caches* caches = bioseq::storage::alloc_seq_caches(scratch);
	caches->side                = bioseq::storage::seq_caches::Side::Read;
	caches->lock_mode           = AccessShareLock;
	caches->read_state_mctx     = scratch;
	caches->main_database_oid   = rid.db_oid;
	caches->main_tablespace_oid = rid.tablespace_oid;
	caches->main_relfilenode    = rid.relfilenode;

	/* Project every distinct site column and random-access read the row once. */
	List* projected = NIL;
	for (auto& s : consts.symbol_count_sites)
		if (s.placeholder != nullptr && !list_member_int(projected, s.column_attno))
			projected = lappend_int(projected, s.column_attno);

	std::vector<Datum> vals(td->natts);
	auto nulls_owner = std::make_unique<bool[]>(td->natts);
	bool* nulls = nulls_owner.get();
	BioseqColumnReadState* rs = BioseqColumnBeginRead(main, td, projected, NIL, scratch, snap,
											  /*randomAccess=*/true, nullptr);
	const bool row_ok = BioseqColumnReadRowByRowNumber(
		rs, static_cast<uint64_t>(rowno), vals.data(), nulls);
	BioseqColumnEndRead(rs);

	for (auto& site : consts.symbol_count_sites) {
		if (site.placeholder == nullptr) continue;
		if (!row_ok || site.rank < 0 || nulls[site.column_attno - 1]) {
			site.placeholder->constisnull = true;
			continue;
		}
		MemoryContext old = MemoryContextSwitchTo(scratch);
		pgseq_t* hdr = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(vals[site.column_attno - 1]));
		MemoryContextSwitchTo(old);
		const std::uint8_t alpha = hdr->alphabet_code;
		const bool stub          = bioseq::storage::is_locator_stub(hdr);
		const std::uint8_t bw    = stub ? bioseq::storage::logical_storage_unit_byte_width(hdr)
										: hdr->storage_unit_byte_width;
		bioseq::storage::seq_locator loc{};
		if (stub) loc = bioseq::storage::decode_locator_from_stub(hdr);

		/* (2) stored read — the ONLY thing read_sequence_index gates here. */
		std::optional<std::int64_t> stored;
		if (bioseq_read_sequence_index && stub) {
			auto store = bsi::open_sequence_storage(relid, rid.db_oid, rid.tablespace_oid,
				rid.relfilenode, site.column_attno,
				bioseq::indexing::SymbolCountsIndex<bsi::IndexStorageSequencePg>::index_name, loc,
				static_cast<uint64_t>(rowno), caches, scratch);
			stored = bioseq::indexing::SymbolCountsIndex<bsi::IndexStorageSequencePg>::read_count(
				store, static_cast<std::size_t>(site.rank));
		}
		if (stored.has_value()) {
			site.placeholder->constvalue =
				bioseq::pg_utils::pg_datum::from(static_cast<std::int64_t>(*stored)).raw();
			site.placeholder->constisnull = false;
			continue;
		}

		/* (3) unconditional recompute fallback (never gated by the GUC). */
		std::int64_t cnt = 0;
		pgseq_t::dispatch_instance(alpha, bw, [&]<class S>() {
			if (stub) {
				constexpr std::size_t lpu = compact_sequence_range<S>::traits::letters_per_unit;
				const std::size_t pad = (loc.total_bp_size % lpu) == 0 ? 0
					: static_cast<std::size_t>(lpu - loc.total_bp_size % lpu);
				bioseq::storage::with_lazy_postgres<S>(rid.db_oid, rid.tablespace_oid,
					rid.relfilenode, site.column_attno, loc, pad, caches, scratch,
					[&](auto& lazy) {
						auto counts = bioseq::indexing::tally_symbol_counts(lazy);
						if (static_cast<std::size_t>(site.rank) < counts.size())
							cnt = counts[site.rank];
					});
			} else {
				compact_sequence_range<S> range{*hdr};
				auto lazy = bioseq::storage::make_lazy_in_memory(range);
				auto counts = bioseq::indexing::tally_symbol_counts(lazy);
				if (static_cast<std::size_t>(site.rank) < counts.size())
					cnt = counts[site.rank];
			}
		});
		site.placeholder->constvalue  = bioseq::pg_utils::pg_datum::from(cnt).raw();
		site.placeholder->constisnull = false;
	}
	MemoryContextDelete(scratch);
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
				bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() output cursor drained; returning empty slot (EOF)");
				return ExecClearTuple(node->ps.ps_ResultTupleSlot);
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
											   next->entry->sequence_rowno);

			/*
			 * Re-install the buffered scan slot as the current scan
			 * tuple so Var references (e.g. `a`, `c`) resolve off the
			 * right row.
			 */
			econtext->ecxt_scantuple = next->entry->scan_slot_copy;

			ProjectionInfo* proj = ExecBuildProjectionInfo(
				runtime.newtargetlist,
				econtext,
				node->ps.ps_ResultTupleSlot,
				&(node->ps),
				next->entry->scan_slot_copy->tts_tupleDescriptor);

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
				bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() source exhausted; running batch processor");
				runtime.buffer.run_batch_and_build_cursors();
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
					bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() vectorized source exhausted; running batch processor");
					runtime.buffer.run_batch_and_build_cursors();
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
		if (!runtime.buffer.mmsearch_active()) {
			bioseq_hydrate_scan_slot(node->ss_currentScanDesc, slot, /*force=*/true);
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
			if (runtime.buffer.mmsearch_active()) {
				bioseq_log<DBG>("[bioseq] pg: Inner_CustomExecScan() qual passed; buffering scan tuple");
				runtime.buffer.submit_scan(econtext->ecxt_scantuple);
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
													   node->ss_currentRelation, row_number);
				}

				ProjectionInfo* proj = ExecBuildProjectionInfo(
					runtime.newtargetlist,
					econtext,
					node->ps.ps_ResultTupleSlot,
					&(node->ps),
					slot->tts_tupleDescriptor);
				return ExecProject(proj);
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
			(runtime.buffer.mmsearch_active() || runtime.param_rebind.active)
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
	return CustomExecScan(BioseqScanState::from_custom_scan_state(node),
						  reinterpret_cast<ExecScanAccessMtd>(BioseqColumnScanNext),
						  reinterpret_cast<ExecScanRecheckMtd>(BioseqColumnScanRecheck));
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

	/*
	 * Param-driven mmsearch rebind: when the planner rewrote the
	 * second mmsearch argument as a Param (parameterized inner-side
	 * scan in a Nested Loop), each rescan needs to install the current
	 * Param value as the new query. The Param lives in the scan
	 * node's PlanState ExprContext (filled by the NL parent before
	 * triggering rescan), so we resolve it via EvalParamsMutator
	 * against a copy of the original FuncExpr argument.
	 *
	 * We re-walk plan.qual rather than caching the original Param
	 * pointer because EvalParamsMutator (above) already replaced
	 * Params in `bioseqScanState->qual` with Consts; the originals
	 * still live in cscan->scan.plan.qual and are stable across
	 * rescans.
	 */
	if (runtime.param_rebind.active)
	{
		/*
		 * Defence in depth: BeginCustomScan should already have
		 * accepted only PARAM_EXEC, but if a future change relaxes
		 * that gate the rebind path must not silently route through
		 * the wrong ParamListInfo. Refuse to rebind if the kind
		 * doesn't match what the rebind path supports.
		 */
		if (runtime.param_rebind.paramkind != PARAM_EXEC) {
			bioseq_log<DBG>("[bioseq] pg: ReScanCustomScan() refusing rebind for non-PARAM_EXEC kind={}",
					runtime.param_rebind.paramkind);
			runtime.buffer.clear_mmsearch_context();
		} else {
		const BioseqStubOids& scan_stub_oids = get_cached_stub_oids();
		(void)scan_stub_oids;
		/*
		 * Use the stashed Param FuncExpr from BeginCustomScan rather
		 * than re-walking plan.qual. With multi-mmsearch stacking
		 * (Param primary + Const extra), `find_mmsearch_funcexpr`
		 * may return the wrong FuncExpr (the planner is free to put
		 * the Const first), and that would silently leave the
		 * Param-driven primary unresolved on rescan.
		 */
		FuncExpr* mmsearch_fe = runtime.param_rebind.mmsearch_fe;
		ExprContext* econtext = node->ss.ps.ps_ExprContext;
		bool reconfigured = false;
		if (mmsearch_fe != nullptr && list_length(mmsearch_fe->args) >= 2)
		{
			PList<Node> fe_args(mmsearch_fe->args);
			Node* arg1 = fe_args.second();
			if (pg_node_try_as<Param>(arg1) != nullptr)
			{
				/*
				 * EvalParamsMutator is the same helper used to fold
				 * Params in plain qualifiers — it returns a Const node
				 * holding the current Param value (or constisnull=true
				 * if the Param resolved to NULL).
				 */
				Node* folded = eval_params(arg1, econtext);
				if (Const* query_const = pg_node_try_as<Const>(folded))
				{
					if (!query_const->constisnull
						&& query_const->consttype == scan_stub_oids.bioseq_seq_type_oid)
					{
						pgseq_t* query_hdr =
							reinterpret_cast<pgseq_t*>(
								PG_DETOAST_DATUM(query_const->constvalue));
						const std::size_t query_total_bytes =
							static_cast<std::size_t>(VARSIZE(query_hdr));
						std::vector<char> query_varlena_bytes(query_total_bytes);
						std::memcpy(query_varlena_bytes.data(), query_hdr,
									query_total_bytes);
						const uint32_t bp_size = query_hdr->bp_size;
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
						reconfigured = true;
						bioseq_log<DBG>("[bioseq] pg: ReScanCustomScan() rebound mmsearch query bp_size={} extras_count={} from Param paramid={}",
								bp_size,
								runtime.buffer.extra_query_count(),
								runtime.param_rebind.paramid);
					}
					else if (query_const->constisnull)
					{
						bioseq_log<DBG>("[bioseq] pg: ReScanCustomScan() Param paramid={} resolved to NULL; mmsearch will match no rows",
								runtime.param_rebind.paramid);
					}
				}
			}
		}
		if (!reconfigured)
		{
			/*
			 * Could not resolve the Param. Mark the buffer's mmsearch
			 * context inactive so this rescan iteration skips batch
			 * processing entirely and the qual evaluates as the
			 * tautology stub (which still rejects via NULL semantics
			 * when a param was NULL, since mmsearch is STRICT).
			 */
			runtime.buffer.clear_mmsearch_context();
		}
		}  /* end of else (paramkind == PARAM_EXEC) */
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
		BioseqScanFeatureFlags features =
			(aligner_snapshot != nullptr && aligner_snapshot->params_captured)
				? aligner_snapshot->features
				: detect_feature_flags(node->ss.ps.plan->targetlist,
									   node->ss.ps.plan->qual, explain_oids);

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
					explain_detect_typmod_coalesce(node->ss.ps.plan->qual, explain_oids);
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
		using bioseq::simd::N_NEON::ChainingAlgorithmType;
		using bioseq::simd::N_NEON::BaseAlignmentAlgorithmType;
		using bioseq::simd::N_NEON::chaining_algorithm_name;
		using bioseq::simd::N_NEON::base_alignment_algorithm_name;
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
			 * Surface the per-batch execution counters from the
			 * BatchProcessor stats. Only meaningful after ANALYZE has
			 * actually run the scan (`last_stats.targets_considered`
			 * is zero before the first batch).
			 */
			auto print_submit = [&](const char* label) {
				appendStringInfoSpaces(es->str, es->indent * 2);
				appendStringInfo(es->str, "* %s\n", label);
			};
			if (explain_runtime != nullptr) {
				const auto& stats = explain_runtime->buffer.execution_stats();
				if (stats.targets_considered > 0) {
					StringInfoData submit_label;
					initStringInfo(&submit_label);
					appendStringInfo(&submit_label,
						"Submit results [targets=%zu prebuilt_index=%zu recomputed=%zu]",
						stats.targets_considered,
						stats.targets_with_prebuilt_index,
						stats.targets_recomputed_minimizers);
					print_submit(submit_label.data);
					pfree(submit_label.data);
				} else {
					print_submit("Submit results");
				}
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
					appendStringInfo(es->str,
						"batch summary: targets_considered=%zu, prebuilt_index=%zu, recomputed=%zu\n",
						stats.targets_considered,
						stats.targets_with_prebuilt_index,
						stats.targets_recomputed_minimizers);
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
					ExplainOpenGroup("Stage", stage.stage_name.c_str(), true, es);
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
					ExplainCloseGroup("Stage", stage.stage_name.c_str(), true, es);
				}
				ExplainCloseGroup("Pipeline Stages", "Pipeline Stages", false, es);
			}
			ExplainCloseGroup("Bioseq Pipeline", "Bioseq Pipeline", true, es);
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

static Size 
BioseqColumn_EstimateDSMCustomScan(CustomScanState *node,
							   ParallelContext *pcxt)
{
	Size nbytes;

	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);

	nbytes = offsetof(ParallelBioseqColumnScanData, snapshotData);
	nbytes = add_size(nbytes, EstimateSnapshotSpace(bioseqScanState->snapshot));
	nbytes = MAXALIGN(nbytes);

	return nbytes;
}


static void 
BioseqColumn_InitializeDSMCustomScan(CustomScanState *node,
								 ParallelContext *pcxt,
								 void *coordinate)
{
	ParallelBioseqColumnScan pscan = reinterpret_cast<ParallelBioseqColumnScan>(coordinate);
	BioseqScanState* bioseqScanState = BioseqScanState::from_custom_scan_state(node);
	
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

	/* Reset atomic nextStripeId to initial value */
	pg_atomic_init_u64(&pscan->nextStripeId, 1);

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
	bioseqScanState->parallelBioseqColumnScan = pscan;
	bioseqScanState->snapshot = RestoreSnapshot(pscan->snapshotData);
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
