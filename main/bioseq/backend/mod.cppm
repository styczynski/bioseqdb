/*-------------------------------------------------------------------------
 *
 * mod.cppm
 *
 * Extension entry points: PG_MODULE_MAGIC, _PG_init and the SQL-level C
 * functions exported to the bioseq extension script (mmsearch,
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

import mlogging;
import bioseq_columnar;
import pg_utils;
import bioseq_types.pg_compact_sequence;
import bioseq_sequences;
import bioseq_sidecar;
import sequan_compat;

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
 * mmsearch is a BOOLEAN stub used as a WHERE-clause predicate. Always
 * returns true so every scanned row passes the qual; the custom scan
 * only cares that the FuncExpr exists in the plan so we can walk its
 * args at BeginCustomScan time to discover the column and the query
 * literal.
 */
CXX_PG_FUNCTION_INFO_V1(mmsearch);
Datum mmsearch(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: mmsearch() stub invoked, always returning true");
	PG_RETURN_BOOL(true);
}

/*
 * bioseq_score_stub and its siblings back the SQL projection functions
 * bioseq.score / bioseq.dp_score / bioseq.cigar / bioseq.query_start / ...
 * None of them must ever execute in the hot path: the columnar custom
 * scan rewrites the target list to replace every FuncExpr whose funcid
 * matches one of these stubs with a Const we own. If any stub fires
 * here, the custom path was not chosen for the query and the operator
 * sees the sentinel value; we log loudly so the miss is obvious in
 * main/loglog.log.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_score_stub);
Datum bioseq_score_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_score_stub() UNEXPECTED direct call -- custom scan did not rewrite this call site");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_dp_score_stub);
Datum bioseq_dp_score_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_dp_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_len_stub);
Datum bioseq_query_len_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_query_len_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_start_stub);
Datum bioseq_query_start_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_query_start_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_end_stub);
Datum bioseq_query_end_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_query_end_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_len_stub);
Datum bioseq_target_len_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_target_len_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_start_stub);
Datum bioseq_target_start_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_target_start_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_end_stub);
Datum bioseq_target_end_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_target_end_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_num_matches_stub);
Datum bioseq_num_matches_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_num_matches_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_block_length_stub);
Datum bioseq_block_length_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_block_length_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_mapq_stub);
Datum bioseq_mapq_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_mapq_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_chain_score_stub);
Datum bioseq_chain_score_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_chain_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_dp_max_score_stub);
Datum bioseq_dp_max_score_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_dp_max_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_seed_count_stub);
Datum bioseq_seed_count_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_seed_count_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_secondary_chain_score_stub);
Datum bioseq_secondary_chain_score_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_secondary_chain_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_is_reverse_stub);
Datum bioseq_is_reverse_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_is_reverse_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_is_primary_stub);
Datum bioseq_is_primary_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_is_primary_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_has_cigar_stub);
Datum bioseq_has_cigar_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_has_cigar_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_divergence_stub);
Datum bioseq_divergence_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_divergence_stub() UNEXPECTED direct call");
	PG_RETURN_FLOAT4(-1.0f);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_cigar_stub);
Datum bioseq_cigar_stub(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_cigar_stub() UNEXPECTED direct call");
	PG_RETURN_TEXT_P(cstring_to_text("!UNEXPECTED!"));
}

/*
 * bioseq.symbol_count(el bioseq.seq, sym text) -> bigint.
 *
 * The Postgres entry point computes the result directly from the passed-in
 * varlena (so it works on literals and outside a scan context). This path is
 * deliberately expensive (O(n) over the sequence): the planner-support
 * function bioseq_symbol_count_support flags it as such so any query that
 * routes through the bioseq columnar custom scan will prefer the custom
 * scan's cheap path, which resolves the value from bioseq_internal.meta_sidecar
 * without re-examining the varlena. See columnar_customscan.cppm —
 * VarInjectorMutator rewrites every `symbol_count(Var, 'L')` call site in
 * the target list into a placeholder Const that is populated per-row from
 * the sidecar; this function is never invoked inside the custom scan for a
 * column reference.
 *
 * Both the scalar entry point here and the custom-scan fast path route
 * through count_symbols() at some point (compute-at-call for literals vs.
 * compute-at-INSERT for columns), so they return bit-for-bit identical
 * values by construction. Test 13 exercises the parity with vectorization
 * both on and off.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_symbol_count);
Datum bioseq_symbol_count(PG_FUNCTION_ARGS) {
	/* Module macros like PG_GETARG_PGCOMPACTSEQUENCE_P are not visible across
	 * a C++20 `import` boundary (modules don't export preprocessor macros).
	 * The varlena detoast is done manually. */
	PgCompactSequenceHdr* hdr = reinterpret_cast<PgCompactSequenceHdr*>(
		PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	text* sym_text = PG_GETARG_TEXT_PP(1);
	const int32 sym_size = VARSIZE_ANY_EXHDR(sym_text);
	if (sym_size != 1) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq.symbol_count: symbol must be exactly one byte, got %d",
						sym_size)));
	}
	const char sym_char = *VARDATA_ANY(sym_text);

	/* Resolve letter→rank through the seqan3 dispatch so the scalar path
	 * and the INSERT-time meta_sidecar computation share one source of
	 * truth. On an unknown letter the caller raises ERRCODE_INVALID_PARAMETER_VALUE
	 * below. */
	int rank = -1;
	int64_t computed_result = 0;
	dispatch_pgseq(hdr->alphabet_code, hdr->width_code,
		[&]<class Dst>() {
			using alphabet_t = typename compact_sequence_range<Dst>::traits::alphabet_type;
			if (!seqan3::char_is_valid_for<alphabet_t>(sym_char)) {
				return;
			}
			alphabet_t letter{};
			letter.assign_char(sym_char);
			rank = static_cast<int>(letter.to_rank());

			compact_sequence_range<Dst> seq = make_seq_range_from_pg<Dst>(hdr);
			auto tally = seq.count_symbols();
			if (static_cast<size_t>(rank) < tally.size()) {
				computed_result = static_cast<int64_t>(tally[rank].first);
			}
		});

	if (rank < 0) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq.symbol_count: letter '%c' is not part of the alphabet (code=%u)",
						sym_char, static_cast<unsigned>(hdr->alphabet_code))));
	}

	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT64(computed_result);
}

/*
 * bioseq_symbol_count_support is the planner-support function attached to
 * bioseq.symbol_count(). We return a high per-call cost so any query with a
 * cheaper alternative (the bioseq columnar custom scan) is preferred by the
 * optimiser. The cost is lower than mmsearch_support's (100000) because the
 * scalar symbol_count implementation is O(n) where n is the sequence
 * length, while mmsearch's scalar path is ~O(n^2). The ordering matters
 * because both paths use the same "inflate to make plain scan look
 * expensive" trick in AddColumnarScanPath — see columnar_customscan.cppm's
 * `wehavemagic_cnt` scan which translates every detected cost inflator
 * into a negative custom-scan startup cost.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_symbol_count_support);
Datum bioseq_symbol_count_support(PG_FUNCTION_ARGS) {
	JustLog("[bioseq] pg: bioseq_symbol_count_support() entered");
	Node* response = nullptr;
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	if (IsA(request, SupportRequestCost)) {
		SupportRequestCost* cost_request = reinterpret_cast<SupportRequestCost*>(request);
		JustLog("[bioseq] pg: bioseq_symbol_count_support() received SupportRequestCost");
		cost_request->startup = 50000;
		cost_request->per_tuple = 0;
		PG_RETURN_POINTER(cost_request);
	}
	PG_RETURN_POINTER(response);
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
