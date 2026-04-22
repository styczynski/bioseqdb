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
 * Resolution order per the spec (§7):
 *   1. If we are inside an active sidecar scan-row context
 *      (bioseq::sidecar::g_scan_row_ctx.active), read symbol_counts[] from
 *      bioseq_internal.meta_sidecar by (user_relid, rowno) and index the
 *      requested letter's rank into that array. This matches what would
 *      have been computed at INSERT time, bit-for-bit, at the cost of one
 *      index probe.
 *   2. If the sidecar is missing (row absent — GUC was off at INSERT, or
 *      the row predates the feature), fall back to recomputing
 *      count_symbols() on the varlena and (if we were in a scan context)
 *      backfill the meta_sidecar row.
 *   3. If we are NOT in a scan context (literal call, non-custom scan),
 *      compute directly on the varlena via count_symbols().
 *
 * All three paths produce identical output because meta_sidecar rows are
 * themselves produced by calling count_symbols() on insert. The scalar and
 * vectorized planner paths therefore return EXACTLY the same value for any
 * input; see setup.sql Test 13 for the parity check.
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

	/* Resolve the letter-to-rank mapping *through* dispatch_pgseq so seqan3
	 * is the single source of truth — avoids the hand-maintained
	 * letter_to_rank_for_alphabet switch falling out of sync when an
	 * alphabet is added.
	 *
	 * Memory-context note: dispatch_pgseq's default arm itself ereports
	 * with ERRCODE_FEATURE_NOT_SUPPORTED on an unknown (alphabet_code,
	 * width_code) pair. That longjmps past our PG_FREE_IF_COPY below, but
	 * since every palloc in this function lives in the per-tuple context
	 * (cleared on ereport unwinding), the "leak" is a convention gap, not
	 * a real memory retention. The caller surfaces the ereport without
	 * swallowing it. */
	int rank = -1;
	int64_t computed_result = 0;
	dispatch_pgseq(hdr->alphabet_code, hdr->width_code,
		[&]<class Dst>() {
			using alphabet_t = typename compact_sequence_range<Dst>::traits::alphabet_type;
			if (!seqan3::char_is_valid_for<alphabet_t>(sym_char)) {
				return;          /* rank stays -1; caller below errors out */
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

	int64_t result = computed_result;

	/* Tier 1 — fetch from meta_sidecar if we are inside an active scan-row
	 * context. The sidecar row was populated on INSERT by the same
	 * count_symbols() call path above, so tier-1 and tier-3 return bit-
	 * identical values by construction. We intentionally do NOT perform a
	 * tier-2 backfill from a SELECT: backfilling would (a) escalate the
	 * transaction to read-write, which fails on hot-standbys, and (b) race
	 * on the meta_sidecar PK against concurrent scanners. Missing rows
	 * simply fall through to the already-computed value.
	 *
	 * TODO(future): if a user is able to alter the alphabet typmod of an
	 * existing bioseq.seq column (currently blocked by the type system —
	 * `ALTER TABLE ... ALTER COLUMN seq TYPE bioseq.seq(...)` rewrites the
	 * column and repopulates meta_sidecar through bioseq_tuple_insert), we
	 * could still see a stale sidecar row for a row whose seq was updated
	 * in-place without passing through the insert path. A version-stamp
	 * column on meta_sidecar + relcache-invalidation hook would catch this.
	 * Today all write paths go through sidecar_after_insert, so the sidecar
	 * is always current. */
	if (bioseq::sidecar::g_scan_row_ctx.active) {
		auto counts = bioseq::sidecar::sidecar_read_meta_counts(
			bioseq::sidecar::g_scan_row_ctx.user_relid,
			bioseq::sidecar::g_scan_row_ctx.sequence_rowno);
		if (counts.has_value() && static_cast<size_t>(rank) < counts->size()) {
			result = (*counts)[rank];
			JustLog("[bioseq] pg: bioseq_symbol_count('{}') -> {} from meta_sidecar (relid={}, rowno={})",
					sym_char, result,
					static_cast<unsigned>(bioseq::sidecar::g_scan_row_ctx.user_relid),
					bioseq::sidecar::g_scan_row_ctx.sequence_rowno);
		}
	}

	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT64(result);
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
