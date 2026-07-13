/*-------------------------------------------------------------------------
 *
 * table/batch_sort_scan.cppm
 *
 * BioseqBatchSortScan — an OUTER custom scan that sorts sequences.
 *
 * When a query has `ORDER BY <bioseq.seq column>`, the planner would normally
 * stack a core `Sort` node on top of the input scan (bioseq paths never set
 * pathkeys, so PostgreSQL always sorts sequences itself). This module installs
 * a `create_upper_paths_hook` on the UPPERREL_ORDERED stage that, whenever the
 * sort key is of type `bioseq.seq`, REPLACES the core Sort path(s) with a
 * CustomPath for this node. The node:
 *
 *   1. drains its child (e.g. the inner `BioseqScan` running the mmsearch
 *      aligner, or a plain SeqScan), buffering every output row;
 *   2. wraps each row's sequence Datum as a `lazy_compact_sequence` and gathers
 *      them into a `std::span` batch;
 *   3. calls `lazy_compact_sequence_batch_processor::lazy_compact_sequence_batch_sort`
 *      to obtain an output ordering (a permutation);
 *   4. re-emits the buffered rows in that order.
 *
 * The batch processor orders with the GPU seq-sort path when Vulkan is enabled.
 * When `bioseq.aligner_vulkan_enabled` is disabled, this executor sorts the
 * buffered compact_sequence_stdc headers directly with `stdc_compare`, which is
 * the scalar comparator used by the SQL surface.
 *
 * Layout invariant: BioseqBatchSortState keeps CustomScanState as its first
 * field (standard-layout) so PostgreSQL's CustomScanState* <-> our state*
 * reinterpret_cast across the C ABI round-trips (same pattern as
 * BioseqScanState in table/bioseq_scan_state.cppm).
 *
 *-------------------------------------------------------------------------
 */
module;

// DOC0192 — anchor <hwy/highway.h> in this TU's global module fragment before
// importing the HWY-wrapped sequence/storage BMIs (mirrors custom_scan.cppm).
#include "common_simd.h"
#include "common.h"

#include <chrono>
#include <compare>
#include <cstddef>
#include <exception>
#include <numeric>
#include <span>
#include <vector>

export module bioseq.table.batch_sort_scan;

import bioseq.common.mlogging;          // bioseq_log
import bioseq.storage.batch_processor;  // lazy_compact_sequence_batch_processor, batch_sort_spec
import bioseq.storage.lazy;             // lazy_compact_sequence, make_lazy_in_memory
import bioseq.storage.scanner;          // sequence_scanner_none
import bioseq.storage.manager;          // is_locator_stub
import bioseq.sequences;                // compact_sequence_range
import bioseq.sequences.simd;           // active_target::stdc_compare
import bioseq.types.pg_compact_sequence;// pgseq_t, dispatch_instance
import bioseq.config;                   // DOC0346 — bioseq_aligner_vulkan_enabled
import bioseq.parallelism.pool;         // GlobalPool::parallel_sort
import bioseq.gpu.vulkan.runtime;       // Runtime counters for EXPLAIN proof
import bioseq.scheduler.supervisor_pg;  // readiness-first PG Runtime entry
import bioseq.pg_utils;                 // PostgresErrorAsCpp at the PG callback seam

namespace {

/* GUC backing var: opt-in switch for the prototype sort node. Default OFF so
 * PostgreSQL keeps sorting sequences itself (existing ORDER BY seq behavior +
 * tests are preserved); experiments SET it on. Registered in init below. */
bool bioseq_enable_sequence_sort_scan = false;

/* ------------------------------------------------------------------ */
/* Per-execution runtime (C++-heap state behind the standard-layout    */
/* BioseqBatchSortState::runtime void*; mirrors BioseqScanRuntime).     */
/* ------------------------------------------------------------------ */
struct BioseqBatchSortRuntime {
    bool                            sorted    = false;   //< child drained + ordered?
    std::size_t                     emit_idx  = 0;       //< next index into `order`
    AttrNumber                      seq_attno = 0;       //< 1-based seq column in scan tuple
    bioseq::storage::batch_sort_spec spec{};            //< ORDER BY direction + GPU enablement
	    MemoryContext                   tuple_cxt = nullptr; //< owns the buffered MinimalTuples
	    std::vector<MinimalTuple>       tuples;              //< all child output rows (scan order)
	    std::vector<std::size_t>        order;               //< emit permutation from the processor
	    TupleTableSlot*                 sortslot  = nullptr; //< MinimalTuple slot used for emit/deform
	    const char*                     accelerator = "not executed";
	    std::size_t                     sorted_rows = 0;
	    std::uint64_t                   gpu_dispatch_delta = 0;
	    std::uint64_t                   gpu_global_compare_dispatch_delta = 0;
	    std::uint64_t                   gpu_dense_scatter_dispatch_delta = 0;
	    std::uint64_t                   gpu_uploaded_bytes_delta = 0;
	    std::uint64_t                   sort_runtime_ns = 0;
	};

/* State object passed across the C ABI. CustomScanState MUST be first. */
struct BioseqBatchSortState {
    CustomScanState css;
    void*           runtime;   //< BioseqBatchSortRuntime*, owned; freed in End + reset callback
};
static_assert(std::is_standard_layout_v<BioseqBatchSortState>,
              "BioseqBatchSortState must remain standard-layout (C ABI reinterpret_cast safety)");
static_assert(offsetof(BioseqBatchSortState, css) == 0,
              "BioseqBatchSortState::css must be the first field");

[[nodiscard]] inline BioseqBatchSortRuntime* runtime_of(CustomScanState* node) {
    return static_cast<BioseqBatchSortRuntime*>(
        reinterpret_cast<BioseqBatchSortState*>(node)->runtime);
}

inline void release_batch_sort_scratch_as_cpp(
		MemoryContext old_context,
		MemoryContext scratch) {
	bioseq::pg_utils::invoke_pg_as_cpp(
		[old_context] { MemoryContextSwitchTo(old_context); });
	bioseq::pg_utils::invoke_pg_as_cpp(
		[scratch] { MemoryContextDelete(scratch); });
}

/* MemoryContext reset callback — frees the C++ runtime even on error paths so
 * the std::vector control blocks don't leak the backend's heap. The buffered
 * MinimalTuples live in tuple_cxt (a child of es_query_cxt) and are reclaimed
 * by the context teardown itself, so we only delete the C++ object here. */
void BioseqBatchSort_RuntimeCleanup(void* arg) {
    auto* state = static_cast<BioseqBatchSortState*>(arg);
    if (state->runtime != nullptr) {
        delete static_cast<BioseqBatchSortRuntime*>(state->runtime);
        state->runtime = nullptr;
    }
}

/* ------------------------------------------------------------------ */
/* Resolve the bioseq.seq pg_type OID, or InvalidOid if absent.        */
/*                                                                      */
/* This MUST NOT raise: the create_upper_paths_hook is installed        */
/* process-wide (the library is in shared_preload_libraries), so it     */
/* fires for ORDER BY in every database — including ones where the      */
/* bioseq extension was never created. `regtypein` would ereport(ERROR) */
/* there; a syscache lookup quietly returns InvalidOid instead.         */
/* Re-resolved on demand (never cached process-wide): integration tests */
/* DROP/CREATE the extension in one backend, reassigning the type OID   */
/* (same rationale as bioseq_am_cache::current_bioseq_seq_type_oid).    */
/* ------------------------------------------------------------------ */
[[nodiscard]] Oid bioseq_seq_type_oid() {
    const Oid nsp_oid = get_namespace_oid("bioseq", /*missing_ok=*/true);
    if (!OidIsValid(nsp_oid)) {
        return InvalidOid;
    }
    return GetSysCacheOid2(TYPENAMENSP, Anum_pg_type_oid,
                           PointerGetDatum("seq"), ObjectIdGetDatum(nsp_oid));
}

/* ------------------------------------------------------------------ */
/* Build the lazy batch from the buffered rows and run the (placeholder)*/
/* ordering. Returns the emit permutation over rt->tuples.              */
/* ------------------------------------------------------------------ */
	[[nodiscard]] std::vector<std::size_t>
	build_batch_order(CustomScanState* node, BioseqBatchSortRuntime* rt) {
	    const std::size_t row_count = rt->tuples.size();
	    rt->sorted_rows = row_count;
	    if (row_count == 0) {
	        return {};
	    }

    /* All rows in one column share an alphabet/width; dispatch once on row 0.
     * Detoast/headers/ranges/lazies are transient — they only need to live for
     * the synchronous sort call below, so they go in a scratch context that is
     * dropped immediately afterwards. The returned permutation is C++-heap. */
    MemoryContext scratch = AllocSetContextCreate(CurrentMemoryContext,
                                                  "BioseqBatchSort scratch",
                                                  ALLOCSET_DEFAULT_SIZES);
    MemoryContext old_cxt = MemoryContextSwitchTo(scratch);

    ExecStoreMinimalTuple(rt->tuples[0], rt->sortslot, false);
    bool first_isnull = false;
    Datum first_datum = slot_getattr(rt->sortslot, rt->seq_attno, &first_isnull);
    if (first_isnull) {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(scratch);
        ereport(ERROR, (errmsg("BioseqBatchSortScan: NULL sequence values are not "
                               "supported by the prototype sort node")));
    }
    auto* first_hdr = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(first_datum));
    if (bioseq::storage::is_locator_stub(first_hdr)) {
        MemoryContextSwitchTo(old_cxt);
        MemoryContextDelete(scratch);
        ereport(ERROR, (errmsg("BioseqBatchSortScan: out-of-core (locator-stub) "
                               "sequences require the fragmented scanner path, "
                               "which is out of scope for this prototype")));
    }
    const std::uint8_t alphabet_code = first_hdr->alphabet_code;
    const std::uint8_t byte_width    = first_hdr->storage_unit_byte_width;

	std::vector<std::size_t> order;
	try {
	pgseq_t::dispatch_instance(alphabet_code, byte_width, [&]<class S>() {
        using unit_t = typename compact_sequence_range<S>::traits::storage_unit;
        using scanner_t = bioseq::storage::sequence_scanner_none<unit_t>;
        using lazy_t = bioseq::storage::lazy_compact_sequence<S, scanner_t>;

        /* Detoast every row's sequence header. For inline (non-stub) values
         * PG_DETOAST_DATUM returns a pointer into the buffered MinimalTuple,
         * which lives in tuple_cxt and so stays valid for the whole batch. */
        std::vector<pgseq_t*> headers;
        headers.reserve(row_count);
        for (std::size_t i = 0; i < row_count; ++i) {
            ExecStoreMinimalTuple(rt->tuples[i], rt->sortslot, false);
            bool isnull = false;
            Datum datum = slot_getattr(rt->sortslot, rt->seq_attno, &isnull);
            if (isnull) {
                ereport(ERROR, (errmsg("BioseqBatchSortScan: NULL sequence values "
                                       "are not supported by the prototype sort node")));
            }
            auto* hdr = reinterpret_cast<pgseq_t*>(PG_DETOAST_DATUM(datum));
            if (bioseq::storage::is_locator_stub(hdr)) {
                ereport(ERROR, (errmsg("BioseqBatchSortScan: out-of-core "
                                       "(locator-stub) sequences are out of scope "
                                       "for this prototype")));
            }
	            headers.push_back(hdr);
	        }

	        if (!rt->spec.vulkan_enabled) {
	            std::vector<std::size_t> stdc_order(row_count);
	            auto* order_begin = stdc_order.data();
	            auto* order_end = order_begin + stdc_order.size();
	            std::iota(order_begin, order_end, std::size_t{0});
	            const bool descending = rt->spec.descending;
	            auto less = [&headers, descending](std::size_t i, std::size_t j) {
	                const std::strong_ordering c =
	                    bioseq::simd::active_target::stdc_compare(*headers[i], *headers[j]);
	                if (c != 0) return descending ? c > 0 : c < 0;
	                return descending ? i > j : i < j;
	            };
	            const auto sort_start = std::chrono::steady_clock::now();
	            bioseq::parallelism::GlobalPool::instance().parallel_sort(
	                order_begin, order_end, less);
	            const auto sort_end = std::chrono::steady_clock::now();
	            rt->sort_runtime_ns = static_cast<std::uint64_t>(
	                std::chrono::duration_cast<std::chrono::nanoseconds>(
	                    sort_end - sort_start).count());
	            order = std::move(stdc_order);
	            rt->accelerator = "stdc_compare";
	            return;
	        }

	        /* Ranges view the headers; lazies reference the ranges (pointer-sized,
	         * copyable for the no-fragment scanner). Both are reserved up front so
         * neither vector reallocates and invalidates the back-references. */
        std::vector<compact_sequence_range<S>> ranges;
        ranges.reserve(row_count);
        for (std::size_t i = 0; i < row_count; ++i) {
            ranges.push_back(compact_sequence_range<S>{*headers[i]});
        }
        std::vector<lazy_t> lazies;
        lazies.reserve(row_count);
        for (std::size_t i = 0; i < row_count; ++i) {
            lazies.push_back(bioseq::storage::make_lazy_in_memory(ranges[i]));
	        }

	        std::span<lazy_t> batch(lazies.data(), lazies.size());
	        auto& gpu_rt =
	            bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
	        const std::uint64_t dispatch_before = gpu_rt.seq_sort_dispatch_count();
	        const std::uint64_t global_before = gpu_rt.seq_sort_global_compare_dispatch_count();
	        const std::uint64_t dense_before = gpu_rt.seq_sort_dense_scatter_dispatch_count();
	        const std::uint64_t uploaded_before = gpu_rt.seq_sort_uploaded_bytes_count();
	        const auto sort_start = std::chrono::steady_clock::now();
	        order = bioseq::storage::lazy_compact_sequence_batch_processor<S, scanner_t>::
	                    lazy_compact_sequence_batch_sort(batch, rt->spec);
	        const auto sort_end = std::chrono::steady_clock::now();
	        const std::uint64_t dispatch_after = gpu_rt.seq_sort_dispatch_count();
	        const std::uint64_t global_after = gpu_rt.seq_sort_global_compare_dispatch_count();
	        const std::uint64_t dense_after = gpu_rt.seq_sort_dense_scatter_dispatch_count();
	        const std::uint64_t uploaded_after = gpu_rt.seq_sort_uploaded_bytes_count();
	        rt->gpu_dispatch_delta = dispatch_after - dispatch_before;
	        rt->gpu_global_compare_dispatch_delta = global_after - global_before;
	        rt->gpu_dense_scatter_dispatch_delta = dense_after - dense_before;
	        rt->gpu_uploaded_bytes_delta = uploaded_after - uploaded_before;
	        rt->sort_runtime_ns = static_cast<std::uint64_t>(
	            std::chrono::duration_cast<std::chrono::nanoseconds>(
	                sort_end - sort_start).count());
	        rt->accelerator = rt->gpu_dispatch_delta == 0 ? "compare_sequences_scalar" : "gpu";
	    });
	} catch (...) {
		// Readiness errors are C++ exceptions only after their inner PG_END_TRY.
		// Restore/delete the PG scratch context after every C++ owner in the typed
		// dispatch has unwound, then let the callback boundary rethrow exactly.
		release_batch_sort_scratch_as_cpp(old_cxt, scratch);
		throw;
	}

	release_batch_sort_scratch_as_cpp(old_cxt, scratch);
	return order;
}

/* ================================================================== */
/* Executor callbacks                                                  */
/* ================================================================== */

void
BioseqBatchSort_BeginCustomScan(CustomScanState* node, EState* estate, int eflags)
{
    bioseq_log<DBG>("[bioseq] pg: BioseqBatchSort_BeginCustomScan() entered");
    auto* cscan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);

    /* Initialise the single child plan as our outer subplan. Setting
     * outerPlanState makes the child show up under this node in EXPLAIN and
     * lets End/ReScan drive it generically. */
    auto* child_plan = reinterpret_cast<Plan*>(linitial(cscan->custom_plans));
    outerPlanState(node) = ExecInitNode(child_plan, estate, eflags);

    auto* rt = new BioseqBatchSortRuntime();
    reinterpret_cast<BioseqBatchSortState*>(node)->runtime = rt;

    /* DOC0346 — ORDER BY direction (planner) + GPU enablement (GUC) into the
     * spec the batch processor consumes. custom_private = [descending, nulls_first]. */
    if (list_length(cscan->custom_private) == 2) {
        rt->spec.descending  = (intVal(list_nth(cscan->custom_private, 0)) != 0);
        rt->spec.nulls_first = (intVal(list_nth(cscan->custom_private, 1)) != 0);
    }
    rt->spec.vulkan_enabled = bioseq_aligner_vulkan_enabled;

    /* Free the C++ runtime even if the executor errors out before End. */
    auto* cb = static_cast<MemoryContextCallback*>(
        MemoryContextAlloc(estate->es_query_cxt, sizeof(MemoryContextCallback)));
    cb->func = &BioseqBatchSort_RuntimeCleanup;
    cb->arg  = node;   /* void*; the callback casts it back to our state */
    MemoryContextRegisterResetCallback(estate->es_query_cxt, cb);

    /* Buffered rows outlive per-tuple resets, so they get their own context. */
    rt->tuple_cxt = AllocSetContextCreate(estate->es_query_cxt,
                                          "BioseqBatchSort tuples",
                                          ALLOCSET_DEFAULT_SIZES);

    /* MinimalTuple slot (the scan tuple type) for buffering/emit. ss_ScanTupleSlot
     * was built by ExecInitCustomScan from custom_scan_tlist. */
    rt->sortslot = ExecInitExtraTupleSlot(
        estate, node->ss.ss_ScanTupleSlot->tts_tupleDescriptor, &TTSOpsMinimalTuple);
}

TupleTableSlot*
BioseqBatchSort_ExecCustomScanImpl(CustomScanState* node)
{
    auto* rt = runtime_of(node);
    ExprContext* econtext = node->ss.ps.ps_ExprContext;

    if (!rt->sorted) {
        /* ---- drain the child, buffering every row (scan order) ---- */
        PlanState* child = outerPlanState(node);
        for (;;) {
            TupleTableSlot* slot = ExecProcNode(child);
            if (TupIsNull(slot)) {
                break;
            }
            MemoryContext old_cxt = MemoryContextSwitchTo(rt->tuple_cxt);
            rt->tuples.push_back(ExecCopySlotMinimalTuple(slot));
            MemoryContextSwitchTo(old_cxt);
        }

        /* ---- locate the bioseq.seq column in the scan tuple ---- */
        TupleDesc scan_desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
        const Oid seq_oid = bioseq_seq_type_oid();
        rt->seq_attno = 0;
        for (int i = 0; i < scan_desc->natts; ++i) {
            if (TupleDescAttr(scan_desc, i)->atttypid == seq_oid) {
                rt->seq_attno = static_cast<AttrNumber>(i + 1);
                break;
            }
        }
        if (rt->seq_attno == 0 && !rt->tuples.empty()) {
            ereport(ERROR, (errmsg("BioseqBatchSortScan: no bioseq.seq column found "
                                   "in the sort input tuple")));
        }

        /* ---- batch the lazy sequences and obtain the emit order ---- */
        rt->order = build_batch_order(node, rt);
        rt->sorted = true;
        rt->emit_idx = 0;
        bioseq_log<DBG>("[bioseq] pg: BioseqBatchSort buffered {} rows, ordered",
                        rt->tuples.size());
    }

    /* ---- emit buffered rows in the computed order ---- */
    if (rt->emit_idx >= rt->order.size()) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    const std::size_t row_index = rt->order[rt->emit_idx++];
    ExecStoreMinimalTuple(rt->tuples[row_index], rt->sortslot, false);
    econtext->ecxt_scantuple = rt->sortslot;
    if (node->ss.ps.ps_ProjInfo != nullptr) {
        return ExecProject(node->ss.ps.ps_ProjInfo);
    }
	return rt->sortslot;
}

TupleTableSlot*
BioseqBatchSort_ExecCustomScan(CustomScanState* node)
{
	ErrorData* exact_error = nullptr;
	TupleTableSlot* result = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char detail[512] = "std::exception without detail";
	try {
		result = BioseqBatchSort_ExecCustomScanImpl(node);
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
	if (exact_error != nullptr || cpp_error || unknown_cpp_error) {
		MemoryContext recovery_context =
			node != nullptr && node->ss.ps.state != nullptr
				? node->ss.ps.state->es_query_cxt
				: TopMemoryContext;
		MemoryContextSwitchTo(recovery_context);
		BioseqBatchSort_RuntimeCleanup(
			reinterpret_cast<BioseqBatchSortState*>(node));
	}
	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchSort Exec raised a C++ exception"),
			 errdetail_internal("%s", detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("BioseqBatchSort Exec raised an unknown C++ exception")));
	}
	return result;
}

void
BioseqBatchSort_EndCustomScan(CustomScanState* node)
{
    if (PlanState* child = outerPlanState(node)) {
        ExecEndNode(child);
    }
    auto* state = reinterpret_cast<BioseqBatchSortState*>(node);
    if (state->runtime != nullptr) {
        /* tuple_cxt is a child of es_query_cxt and is freed by the executor's
         * context teardown; we only release the C++ object here. */
        delete static_cast<BioseqBatchSortRuntime*>(state->runtime);
        state->runtime = nullptr;
    }
}

void
BioseqBatchSort_ReScanCustomScan(CustomScanState* node)
{
    auto* rt = runtime_of(node);
    rt->sorted = false;
    rt->emit_idx = 0;
    rt->tuples.clear();
    rt->order.clear();
    rt->accelerator = "not executed";
    rt->sorted_rows = 0;
    rt->gpu_dispatch_delta = 0;
    rt->gpu_global_compare_dispatch_delta = 0;
    rt->gpu_dense_scatter_dispatch_delta = 0;
    rt->sort_runtime_ns = 0;
    if (rt->tuple_cxt != nullptr) {
        MemoryContextReset(rt->tuple_cxt);
    }
    if (PlanState* child = outerPlanState(node)) {
        ExecReScan(child);
    }
}

void
BioseqBatchSort_ExplainCustomScan(CustomScanState* node, List* ancestors, ExplainState* es)
{
    (void)ancestors;
    auto* rt = runtime_of(node);
    if (rt == nullptr) {
        ExplainPropertyText("Bioseq Sort Accelerator", "not initialized", es);
        return;
    }
    ExplainPropertyText("Bioseq Sort Accelerator", rt->accelerator, es);
    ExplainPropertyInteger("Bioseq Sort Rows", nullptr,
                           static_cast<int64>(rt->sorted_rows), es);
    ExplainPropertyInteger("Bioseq Sort Runtime Ns", nullptr,
                           static_cast<int64>(rt->sort_runtime_ns), es);
    ExplainPropertyInteger("Bioseq GPU Sort Dispatches", nullptr,
                           static_cast<int64>(rt->gpu_dispatch_delta), es);
    ExplainPropertyInteger("Bioseq GPU Global Compare Dispatches", nullptr,
                           static_cast<int64>(rt->gpu_global_compare_dispatch_delta), es);
    ExplainPropertyInteger("Bioseq GPU Dense Scatter Dispatches", nullptr,
                           static_cast<int64>(rt->gpu_dense_scatter_dispatch_delta), es);
    ExplainPropertyInteger("Bioseq GPU Sort Uploaded Bytes", nullptr,
                           static_cast<int64>(rt->gpu_uploaded_bytes_delta), es);
}

/* Method tables are defined AFTER their callbacks (DOC0326 — no forward
 * declarations). CreateCustomScanState needs the exec table, the scan table
 * needs CreateCustomScanState, etc., so they are ordered accordingly. */
const CustomExecMethods BioseqBatchSortExecMethods = {
    .CustomName        = "BioseqBatchSortScan",
    .BeginCustomScan   = BioseqBatchSort_BeginCustomScan,
    .ExecCustomScan    = BioseqBatchSort_ExecCustomScan,
    .EndCustomScan     = BioseqBatchSort_EndCustomScan,
    .ReScanCustomScan  = BioseqBatchSort_ReScanCustomScan,
    .ExplainCustomScan = BioseqBatchSort_ExplainCustomScan,
};

Node*
BioseqBatchSort_CreateCustomScanState(CustomScan* cscan)
{
    auto* state = reinterpret_cast<BioseqBatchSortState*>(
        newNode(sizeof(BioseqBatchSortState), T_CustomScanState));
    state->css.methods = &BioseqBatchSortExecMethods;
    state->runtime = nullptr;   /* allocated in BeginCustomScan */
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods BioseqBatchSortScanMethods = {
    .CustomName            = "BioseqBatchSortScan",
    .CreateCustomScanState = BioseqBatchSort_CreateCustomScanState,
};

Plan*
BioseqBatchSort_PlanCustomPath(PlannerInfo* root, RelOptInfo* rel, CustomPath* best_path,
                               List* tlist, List* clauses, List* custom_plans)
{
    bioseq_log<DBG>("[bioseq] pg: BioseqBatchSort_PlanCustomPath() entered");
    CustomScan* cscan = makeNode(CustomScan);
    cscan->methods = &BioseqBatchSortScanMethods;
    cscan->flags = best_path->flags;

    /* Upper node (not a base relation scan). */
    cscan->scan.scanrelid = 0;
    cscan->scan.plan.qual = NIL;
    cscan->custom_exprs = NIL;
    cscan->custom_private = best_path->custom_private;
    cscan->custom_plans = custom_plans;

    /* Passthrough output: our path's pathtarget equals the child's, so `tlist`
     * matches the child's targetlist column-for-column. custom_scan_tlist
     * describes the scan tuple (= child output); setrefs rewrites the output
     * targetlist into INDEX_VAR references over it. */
    cscan->scan.plan.targetlist = tlist;
    auto* child_plan = reinterpret_cast<Plan*>(linitial(custom_plans));
    cscan->custom_scan_tlist = static_cast<List*>(copyObject(child_plan->targetlist));
    cscan->custom_relids = bms_copy(rel->relids);

    return reinterpret_cast<Plan*>(cscan);
}

const CustomPathMethods BioseqBatchSortPathMethods = {
    .CustomName = "BioseqBatchSortScan",
    .PlanCustomPath = BioseqBatchSort_PlanCustomPath,
};

/* ================================================================== */
/* Planner hook — install our ordered path for sequence ORDER BY       */
/* ================================================================== */

create_upper_paths_hook_type PreviousCreateUpperPathsHook = nullptr;

/* The first ORDER BY pathkey whose key is of type bioseq.seq, or nullptr. */
[[nodiscard]] PathKey* sort_sequence_pathkey(PlannerInfo* root, Oid seq_oid)
{
    ListCell* pk_cell;
    foreach(pk_cell, root->sort_pathkeys) {
        auto* pathkey = reinterpret_cast<PathKey*>(lfirst(pk_cell));
        EquivalenceClass* ec = pathkey->pk_eclass;
        ListCell* em_cell;
        foreach(em_cell, ec->ec_members) {
            auto* member = reinterpret_cast<EquivalenceMember*>(lfirst(em_cell));
            if (member->em_is_child) {
                continue;
            }
            if (exprType(reinterpret_cast<Node*>(member->em_expr)) == seq_oid) {
                return pathkey;
            }
        }
    }
    return nullptr;
}

[[nodiscard]] bool pathkey_descending(PathKey* pathkey)
{
#if POSTGRESQL_VERSION_MAJOR >= 18
    return pathkey->pk_cmptype == COMPARE_GT;
#else
    return pathkey->pk_strategy == BTGreaterStrategyNumber;
#endif
}

void
BioseqBatchSort_CreateUpperPathsHook(PlannerInfo* root, UpperRelationKind stage,
                                     RelOptInfo* input_rel, RelOptInfo* output_rel,
                                     void* extra)
{
    if (PreviousCreateUpperPathsHook != nullptr) {
        PreviousCreateUpperPathsHook(root, stage, input_rel, output_rel, extra);
    }

    if (stage != UPPERREL_ORDERED || root->sort_pathkeys == NIL) {
        return;
    }
    /* Opt-in (default OFF): leave PostgreSQL's own Sort in place unless the
     * operator turns this prototype on. Doubles as the A/B toggle (off => core
     * Sort sorts the sequence; on => this custom batch sort node). */
    if (!bioseq_enable_sequence_sort_scan) {
        return;
    }
    Path* input_path = input_rel->cheapest_total_path;
    if (input_path == nullptr) {
        return;
    }
    const Oid seq_oid = bioseq_seq_type_oid();
    PathKey* seq_pk = OidIsValid(seq_oid) ? sort_sequence_pathkey(root, seq_oid) : nullptr;
    if (seq_pk == nullptr) {
        return;   /* not sorting sequences — leave PostgreSQL's plan untouched */
    }

    /* DOC0346 — carry the sort direction to the executor. Stored as a 2-int List
     * in custom_private and read back in BeginCustomScan. */
    const bool descending  = pathkey_descending(seq_pk);
    const bool nulls_first = seq_pk->pk_nulls_first;

    bioseq_log<DBG>("[bioseq] pg: BioseqBatchSort engaging for sequence ORDER BY "
                    "(descending={}, nulls_first={})", descending, nulls_first);

    CustomPath* cpath = makeNode(CustomPath);
    cpath->path.pathtype = T_CustomScan;
    cpath->path.parent = output_rel;
    cpath->path.pathtarget = input_path->pathtarget;   /* passthrough */
    cpath->path.param_info = nullptr;
    cpath->path.parallel_aware = false;
    cpath->path.parallel_safe = false;                 /* blocking sort: serial */
    cpath->path.parallel_workers = 0;
    cpath->path.rows = input_path->rows;
    /* Blocking operator: all input is consumed before the first output row, so
     * startup == input's total. total_cost is deliberately set to the input's
     * total (a "free" sort) so this path dominates and replaces any core Sort. */
    cpath->path.startup_cost = input_path->total_cost;
    cpath->path.total_cost = input_path->total_cost;
    cpath->path.pathkeys = root->sort_pathkeys;         /* we satisfy the ordering */
    cpath->flags = 0;
    cpath->custom_paths = list_make1(input_path);
    /* [0] = descending, [1] = nulls_first (read in BeginCustomScan). */
    cpath->custom_private = list_make2(makeInteger(descending ? 1 : 0),
                                       makeInteger(nulls_first ? 1 : 0));
    cpath->custom_restrictinfo = NIL;
    cpath->methods = &BioseqBatchSortPathMethods;

    /* Deterministically REPLACE the ordered rel's paths so the planner never
     * sorts sequences itself (requirement: always select our outer scan). */
    output_rel->pathlist = NIL;
    output_rel->partial_pathlist = NIL;
    output_rel->cheapest_startup_path = nullptr;
    output_rel->cheapest_total_path = nullptr;
    output_rel->cheapest_unique_path = nullptr;
    output_rel->cheapest_parameterized_paths = NIL;
    add_path(output_rel, reinterpret_cast<Path*>(cpath));
    set_cheapest(output_rel);
}

}  // anonymous namespace

/* ------------------------------------------------------------------ */
/* Public init — install the hook + register the scan methods. Called   */
/* from bioseq_init() (column/column.cppm) alongside the other inits.    */
/* ------------------------------------------------------------------ */
export void
bioseq_batch_sort_init()
{
    DefineCustomBoolVariable(
        "bioseq.enable_sequence_sort_scan",
        "Route ORDER BY on a bioseq.seq column through the BioseqBatchSortScan "
        "outer custom scan instead of a core Sort. Prototype: OFF by default.",
        NULL,
        &bioseq_enable_sequence_sort_scan,
        false,
        PGC_USERSET,
        GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
        NULL, NULL, NULL);
    PreviousCreateUpperPathsHook = create_upper_paths_hook;
    create_upper_paths_hook = BioseqBatchSort_CreateUpperPathsHook;
    RegisterCustomScanMethods(&BioseqBatchSortScanMethods);
}
