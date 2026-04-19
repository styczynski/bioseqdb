/*-------------------------------------------------------------------------
 *
 * mod.cppm
 *
 * Extension entry points: PG_MODULE_MAGIC, _PG_init and the SQL-level C
 * functions exported to the bioseq extension script (testtest, mmsearch,
 * mmsearch_support). The C functions are defined with extern "C" linkage
 * so PostgreSQL's fmgr can call them directly.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include "columnar/columnar.h"
#include "columnar/columnar_tableam.h"

export module bioseq_mod;

import alignment;
import mlogging;
import bioseq_columnar;
import pg_utils;

extern "C" {
PG_MODULE_MAGIC;

void _PG_init();

/*
 * _PG_init is PostgreSQL's extension entry point. It runs when the shared
 * library is loaded into a backend, before any SQL function in the extension
 * is callable.
 */
void
_PG_init()
{
	log_configure();
	JustLog("[bioseq] pg: _PG_init() entered");
	bioseq_init();
}

/*
 * testtest smoke-tests the ksw2 aligner entry point by invoking it with
 * empty arguments. Useful for verifying that the module actually loads and
 * linked against the alignment library.
 */
CXX_PG_FUNCTION_INFO_V1(testtest);
Datum testtest(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: testtest() entered");
	auto align_result = make0<ksw_extz_t>();
	Aligner<ALIGN_ALGORITHM_KSW2>::z2(nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0, 0, 0, 0, 0, align_result);
	JustLog("[bioseq] pg: testtest() align complete");
	return BoolGetDatum(true);
}

/*
 * mmsearch is a stub SQL function used by the demo setup. Returns a
 * synthetic integer derived from the argument so the custom scan plan can
 * be exercised with observable output.
 */
CXX_PG_FUNCTION_INFO_V1(mmsearch);
Datum mmsearch(PG_FUNCTION_ARGS) {
	Oid argument_type = get_fn_expr_argtype(fcinfo->flinfo, 0);
	JustLog("[bioseq] pg: mmsearch() argument_type={}", argument_type);
	if (argument_type == INT4OID) {
		return Int32GetDatum((static_cast<int32_t>(PG_GETARG_INT32(0)) + 10000) * 100);
	}
	return Int32GetDatum(static_cast<int32_t>(999999));
}

/*
 * mmsearch_support is the planner-support function attached to mmsearch().
 * PostgreSQL calls it with SupportRequest nodes so we can adjust cost
 * estimates before planning proceeds.
 */
CXX_PG_FUNCTION_INFO_V1(mmsearch_support);
Datum mmsearch_support(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: mmsearch_support() entered");
	Node* response = nullptr;
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	if (IsA(request, SupportRequestCost))
	{
		SupportRequestCost* cost_request = reinterpret_cast<SupportRequestCost*>(request);
		JustLog("[bioseq] pg: mmsearch_support() received SupportRequestCost");
		PlannerInfo* planner_info = cost_request->root;
		if (planner_info == nullptr) {
			JustLog("[bioseq] pg: mmsearch_support() planner_info is null");
		} else {
			JustLog("[bioseq] pg: mmsearch_support() invoked from node=[{}]", nodeStr(cost_request->node));
			if (planner_info->non_recursive_path == nullptr) {
				JustLog("[bioseq] pg: mmsearch_support() non_recursive_path is null");
			} else {
				JustLog("[bioseq] pg: mmsearch_support() non_recursive_path pathtype={}", static_cast<int>(planner_info->non_recursive_path->pathtype));
			}
		}
		cost_request->startup = 100000;
		cost_request->per_tuple = 0;
		PG_RETURN_POINTER(cost_request);
	}
	PG_RETURN_POINTER(response);
}
}
