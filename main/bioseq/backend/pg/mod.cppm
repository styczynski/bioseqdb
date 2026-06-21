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


extern "C" {
#include "funcapi.h"
#include "utils/tuplestore.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
}

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.mod;

import bioseq.common.mlogging;
import bioseq.column;
import bioseq.pg_utils;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.storage.manager;                   // is_locator_stub, decode_locator_from_stub
import bioseq.storage.lazy;              // make_lazy_in_memory
import bioseq.indexing.type;             // DIRECTORY_SUB_KEY, index_sub_key_t
import bioseq.indexing.symbol_counts;    // DOC0279 §12.5 — tally_symbol_counts, SymbolCountsIndex
import bioseq.storage.indexing_pg.admin;    // DOC0279 §11.4/§12 — update_index_impl, list helpers, letter_to_rank
import bioseq.storage.indexing_pg.chunked_storage;       // DOC0279 §8.8 — g_last_index_merge_run_count, format_seqidx helpers
import bioseq.scheduler.supervisor_pg;
import bioseq.parallelism.mode;
import bioseq.common.sequan_compat;
import bioseq.column.tam;   // DOC0322 §7.4 — tid_to_row_number, IsBioseqColumnTableAmTable (the leaf replaces the former extern "C++" reaches)
import bioseq.pg_utils.nodes;   // DOC0333 — pg_node_try_as (support-request dispatch), expression walker adapter (find_first_var)

using namespace bioseq::pg_utils;   // DOC0333 — node view/walker helpers live in bioseq::pg_utils

namespace {

int bioseq_pg_elevel_for_level(LoggingLevel level) noexcept
{
	switch (level) {
		case LoggingLevel::DBG: return DEBUG1;
		case LoggingLevel::INF: return LOG;
		case LoggingLevel::WRN: return WARNING;
		case LoggingLevel::ERR: return ERROR;
		case LoggingLevel::OFF: return DEBUG5;
	}
	return ERROR;
}

bool bioseq_pg_should_log_elevel(int elevel) noexcept
{
	return elevel >= ERROR || message_level_is_interesting(elevel);
}

bool bioseq_pg_sink_should_log_level(LoggingLevel level) noexcept
{
	return bioseq_pg_should_log_elevel(bioseq_pg_elevel_for_level(level));
}

void bioseq_pg_sink_log_elevel(
	int elevel,
	BioseqSourceLocation location,
	std::string_view message)
{
	if (!errstart(elevel, TEXTDOMAIN)) {
		return;
	}
	errmsg_internal("%.*s",
					static_cast<int>(message.size()),
					message.data());
	errfinish(location.file_name,
			  static_cast<int>(location.line),
			  location.function_name);
	if (elevel >= ERROR) {
		__builtin_unreachable();
	}
}

void bioseq_pg_sink_log_level(
	LoggingLevel level,
	BioseqSourceLocation location,
	std::string_view message)
{
	bioseq_pg_sink_log_elevel(
		bioseq_pg_elevel_for_level(level),
		location,
		message);
}

void bioseq_pg_sink_report(const BioseqReportView& report)
{
	const int elevel = report.has_raw_pg_elevel
		? report.raw_pg_elevel
		: bioseq_pg_elevel_for_level(report.level);

	if (!errstart(elevel, TEXTDOMAIN)) {
		return;
	}
	if (report.has_sqlerrcode) {
		errcode(report.sqlerrcode);
	}
	if (!report.message.empty()) {
		errmsg_internal("%.*s",
						static_cast<int>(report.message.size()),
						report.message.data());
	}
	if (!report.detail.empty()) {
		errdetail_internal("%.*s",
						   static_cast<int>(report.detail.size()),
						   report.detail.data());
	}
	if (!report.hint.empty()) {
		errhint("%.*s",
				static_cast<int>(report.hint.size()),
				report.hint.data());
	}
	errfinish(report.location.file_name,
			  static_cast<int>(report.location.line),
			  report.location.function_name);
	if (elevel >= ERROR) {
		__builtin_unreachable();
	}
}

} // namespace

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
	static bool initialized = false;
	bioseq_register_postgres_logging_sink({
		.should_log_level = bioseq_pg_sink_should_log_level,
		.log_level = bioseq_pg_sink_log_level,
		.should_log_pg = bioseq_pg_should_log_elevel,
		.log_pg = bioseq_pg_sink_log_elevel,
		.report = bioseq_pg_sink_report,
	});
	if (initialized) {
		bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() skipped duplicate call");
		return;
	}
	initialized = true;
	log_configure();
	bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() entered");
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
	bioseq_log<DBG>("[bioseq] pg: mmsearch() stub invoked, always returning true");
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
	bioseq_log<DBG>("[bioseq] pg: bioseq_score_stub() UNEXPECTED direct call -- custom scan did not rewrite this call site");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_dp_score_stub);
Datum bioseq_dp_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_dp_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_len_stub);
Datum bioseq_query_len_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_query_len_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_start_stub);
Datum bioseq_query_start_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_query_start_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_end_stub);
Datum bioseq_query_end_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_query_end_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_len_stub);
Datum bioseq_target_len_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_target_len_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_start_stub);
Datum bioseq_target_start_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_target_start_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_end_stub);
Datum bioseq_target_end_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_target_end_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_num_matches_stub);
Datum bioseq_num_matches_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_num_matches_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_block_length_stub);
Datum bioseq_block_length_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_block_length_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_mapq_stub);
Datum bioseq_mapq_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_mapq_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_chain_score_stub);
Datum bioseq_chain_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_chain_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_dp_max_score_stub);
Datum bioseq_dp_max_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_dp_max_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_seed_count_stub);
Datum bioseq_seed_count_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_seed_count_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_secondary_chain_score_stub);
Datum bioseq_secondary_chain_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_secondary_chain_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_is_reverse_stub);
Datum bioseq_is_reverse_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_is_reverse_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_is_primary_stub);
Datum bioseq_is_primary_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_is_primary_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_has_cigar_stub);
Datum bioseq_has_cigar_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_has_cigar_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_divergence_stub);
Datum bioseq_divergence_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_divergence_stub() UNEXPECTED direct call");
	PG_RETURN_FLOAT4(-1.0f);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_cigar_stub);
Datum bioseq_cigar_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_cigar_stub() UNEXPECTED direct call");
	PG_RETURN_TEXT_P(cstring_to_text("!UNEXPECTED!"));
}

CXX_PG_FUNCTION_INFO_V1(bioseq_gpu_warmup_shader_cache);
Datum bioseq_gpu_warmup_shader_cache(PG_FUNCTION_ARGS) {
	const std::string report =
		bioseq::parallelism::pg::warmup_backend_local_shader_cache_report();
	PG_RETURN_TEXT_P(cstring_to_text_with_len(
		report.data(),
		static_cast<int>(report.size())));
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
 * DOC0279 §12.5: this scalar entry computes the REAL per-rank tally
 * (tally_symbol_counts, an int64[2^bits_per_char] histogram) over the
 * literal's inline bases — NOT the deleted `49`-stub. The custom-scan
 * fast path (columnar_customscan.cppm) routes the same tally through the
 * stored INLINE SymbolCountsIndex + the recompute fallback, so both paths
 * return identical values. A stored (locator-stub) value cannot be tallied
 * here because the scalar entry has no owning-relation context to stream
 * the sub-table fragments from; such values are handled by the custom scan.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_symbol_count);
Datum bioseq_symbol_count(PG_FUNCTION_ARGS) {
	/* Detoast the bioseq.seq varlena and reinterpret as pgseq_t. */
	pgseq_t* hdr = reinterpret_cast<pgseq_t*>(
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

	if (bioseq::storage::is_locator_stub(hdr)) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq.symbol_count: cannot tally a stored (stub) bioseq.seq value outside a scan"),
				 errhint("query the column through the bioseq columnar scan, or run "
						 "bioseq.update_index(rel, 'symbol_counts') to materialise the counter.")));
	}

	int rank = bioseq::storage::indexing::letter_to_rank(hdr->alphabet_code, sym_char);
	if (rank < 0) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq.symbol_count: letter '%c' is not part of the alphabet (code=%u)",
						sym_char, static_cast<unsigned>(hdr->alphabet_code))));
	}

	int64_t computed_result = 0;
	pgseq_t::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width,
		[&]<class S>() {
			compact_sequence_range<S> range{*hdr};
			auto lazy = bioseq::storage::make_lazy_in_memory(range);
			auto counts = bioseq::indexing::tally_symbol_counts(lazy);  // std::array<int64,N=2^bits_per_char>
			if (static_cast<size_t>(rank) < counts.size()) {
				computed_result = static_cast<int64_t>(counts[rank]);
			}
		});

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
 * expensive" trick in AddBioseqColumnScanPath — see columnar_customscan.cppm's
 * `wehavemagic_cnt` scan which translates every detected cost inflator
 * into a negative custom-scan startup cost.
 */
/*
 * Hand-rolled walker that returns the first Var in a node tree without
 * raising on Aggref/WindowFunc/SubLink/etc. Used by the simplify helper
 * below. We deliberately avoid Postgres' pull_var_clause() because that
 * call is documented to raise from late planning paths and we are
 * invoked from eval_const_expressions() which is reached very early in
 * subquery_planner.
 */
struct FirstVarFinder {
	Var* result = nullptr;
};
struct first_var_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, FirstVarFinder& finder) const noexcept {
		if (finder.result != nullptr) {
			return pg_walk_step::stop;  /* abort the walk */
		}
		if (Var* var = node.try_as<Var>()) {
			finder.result = var;
			return pg_walk_step::stop;
		}
		return pg_walk_step::descend;
	}
};

/*
 * Shared simplify-time PHV wrapping. Used by both bioseq_placeholder_support
 * (attached to the ANYELEMENT projections) and bioseq_symbol_count_support
 * (which also folds bioseq.symbol_count()). Returning nullptr here means
 * "no rewrite" — the caller surfaces that to PostgreSQL as "leave the
 * FuncExpr alone."
 */
static Node* try_wrap_in_placeholder(SupportRequestSimplify* simplify) {
	if (simplify == nullptr || simplify->root == nullptr || simplify->fcall == nullptr) {
		return nullptr;
	}
	PlannerInfo* root = simplify->root;
	const bool has_joins = root->hasJoinRTEs;
	const bool has_aggs = root->parse != nullptr && root->parse->hasAggs;
	if (!has_joins && !has_aggs) {
		return nullptr;
	}
	FirstVarFinder finder;
	first_var_visitor visitor;
	pg_walk_expression_including_root(pg_as_node(simplify->fcall), finder, visitor);
	if (finder.result == nullptr) {
		bioseq_log<DBG>("[bioseq] pg: try_wrap_in_placeholder() no Var found in fcall — skipping");
		return nullptr;
	}
	Var* arg_var = finder.result;
	Relids phrels = bms_make_singleton(static_cast<int>(arg_var->varno));
	Expr* expr_copy = reinterpret_cast<Expr*>(copyObjectImpl(simplify->fcall));
	PlaceHolderVar* phv = make_placeholder_expr(root, expr_copy, phrels);
	phv->phlevelsup    = arg_var->varlevelsup;
	phv->phnullingrels = arg_var->varnullingrels;
	bioseq_log<DBG>("[bioseq] pg: try_wrap_in_placeholder() wrapped funcid={} phid={} phrels={}",
			simplify->fcall->funcid, phv->phid,
			static_cast<int>(arg_var->varno));
	return reinterpret_cast<Node*>(phv);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_symbol_count_support);
Datum bioseq_symbol_count_support(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() entered");
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	if (SupportRequestCost* cost_request = pg_node_try_as<SupportRequestCost>(request)) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() received SupportRequestCost");
		cost_request->startup = 50000;
		cost_request->per_tuple = 0;
		PG_RETURN_POINTER(cost_request);
	}
	if (SupportRequestSimplify* simplify = pg_node_try_as<SupportRequestSimplify>(request)) {
		/*
		 * symbol_count's projection placeholder is rewritten by the
		 * VarInjectorMutator just like the bioseq.cigar/score/...
		 * projections, so the same join-pushdown bug bites it. The
		 * simplify branch here mirrors bioseq_placeholder_support so a
		 * LEFT JOIN with WHERE mmsearch + bioseq.symbol_count(...) on
		 * the projection no longer crashes.
		 */
		Node* wrapped = try_wrap_in_placeholder(simplify);
		if (wrapped != nullptr) {
			PG_RETURN_POINTER(wrapped);
		}
	}
	PG_RETURN_POINTER(nullptr);
}

/*
 * mmsearch_support is the planner-support function attached to mmsearch().
 * PostgreSQL calls it with SupportRequest nodes so we can adjust cost
 * estimates before planning proceeds.
 */
CXX_PG_FUNCTION_INFO_V1(mmsearch_support);
Datum mmsearch_support(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: mmsearch_support() entered");
	Node* response = nullptr;
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	if (SupportRequestCost* cost_request = pg_node_try_as<SupportRequestCost>(request))
	{
		bioseq_log<DBG>("[bioseq] pg: mmsearch_support() received SupportRequestCost");
		PlannerInfo* planner_info = cost_request->root;
		if (planner_info == nullptr) {
			bioseq_log<DBG>("[bioseq] pg: mmsearch_support() planner_info is null");
		} else {
			if (planner_info->non_recursive_path == nullptr) {
				bioseq_log<DBG>("[bioseq] pg: mmsearch_support() non_recursive_path is null");
			} else {
				bioseq_log<DBG>("[bioseq] pg: mmsearch_support() non_recursive_path pathtype={}", static_cast<int>(planner_info->non_recursive_path->pathtype));
			}
		}
		cost_request->startup = 100000;
		cost_request->per_tuple = 0;
		PG_RETURN_POINTER(cost_request);
	}
	PG_RETURN_POINTER(response);
}

/*
 * bioseq_placeholder_support is the planner-support function attached to
 * every bioseq.* projection function (bioseq.cigar, bioseq.score,
 * bioseq.target_start, ...). Mirrors paradedb's `placeholder_support`
 * (see paradedb-experiments/pg_search/.../projections/mod.rs) — when a
 * query contains JOINs or aggregates, we wrap the FuncExpr in a
 * PlaceHolderVar so PostgreSQL evaluates it once at the originating scan
 * level and propagates the value upward through joins/aggregates without
 * re-evaluating it. Without this, the planner pushes copies of the
 * FuncExpr into every join child's targetlist, which crashes the LEFT
 * JOIN + bioseq.cigar + WHERE mmsearch path because the inner-side scan
 * has no MatchResult to back the rewritten placeholder Const.
 *
 * Returns NULL (no rewrite) when the query has no joins or aggregates;
 * the FuncExpr stays bare and the scan-time VarInjectorMutator rewrites
 * it to a placeholder Const directly. Returns NULL likewise when the
 * function call shape cannot be wrapped (zero or multiple Vars,
 * varlevelsup mismatch, …) — the executor still falls back to the stub
 * which logs and returns a sentinel.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_placeholder_support);
Datum bioseq_placeholder_support(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_placeholder_support() entered");
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	SupportRequestSimplify* simplify = pg_node_try_as<SupportRequestSimplify>(request);
	if (simplify == nullptr) {
		PG_RETURN_POINTER(nullptr);
	}
	Node* wrapped = try_wrap_in_placeholder(simplify);
	PG_RETURN_POINTER(wrapped);
}

/*
 * bioseq.list_metadata() — DOC0279 §12.1/§12.2. Set-returning function
 * emitting one tuple per (user bioseq table, bioseq.seq column, registered
 * index) — the FULL cartesian product of {user bioseq relations} x {seq
 * columns} x pg_supported_indexes::list_types(), NOT gated on whether any
 * chunk exists. Columns: (table_name, column_attno, column_name, name,
 * index_type, update_policy, size, age). The index set is the compile-time
 * registry (no runtime MetadataKindRegistry). The heavy lifting is done by
 * one constructed SQL query that joins a VALUES source (built in C++ with
 * the per-column physical shard prefix) against index_policy + the derived
 * seqidx_* shards + metadata_age_log.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_list_metadata);
Datum bioseq_list_metadata(PG_FUNCTION_ARGS) {
	namespace bsi = bioseq::storage::indexing;
	namespace bidx = bioseq::indexing;

	ReturnSetInfo* rsinfo = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
	if (rsinfo == nullptr || !pg_node_view{pg_as_node(rsinfo)}.is<ReturnSetInfo>())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("bioseq.list_metadata() must be called in a set-returning context")));
	if ((rsinfo->allowedModes & SFRM_Materialize) == 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("materialize mode required for bioseq.list_metadata()")));
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("bioseq.list_metadata() return type is not composite")));

	MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	MemoryContext old_ctx = MemoryContextSwitchTo(per_query_ctx);
	Tuplestorestate* tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult  = tupstore;
	rsinfo->setDesc    = tupdesc;
	MemoryContextSwitchTo(old_ctx);

	const auto kinds = bsi::pg_supported_indexes::list_types();
	const auto allows = bsi::pg_supported_indexes::allows_on_insert_flags();

	if (SPI_connect() != SPI_OK_CONNECT)
		return static_cast<Datum>(0);

	/* 1. every user bioseq-AM relation (exclude internal namespaces / names). */
	std::vector<Oid> rels;
	if (SPI_execute(
			"SELECT c.oid FROM pg_class c JOIN pg_am am ON c.relam=am.oid "
			"JOIN pg_namespace n ON c.relnamespace=n.oid "
			"WHERE am.amname='bioseq' AND c.relkind='r' "
			"AND n.nspname NOT IN ('bioseq_internal','pg_catalog','information_schema') "
			"AND n.nspname NOT LIKE 'pg\\_temp%'", true, 0) == SPI_OK_SELECT) {
		for (std::uint64_t r = 0; r < SPI_processed; ++r) {
			auto rel_cell = bioseq::pg_utils::pg_spi_cell::get(
				SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1);
			if (auto rel_oid = rel_cell.optional_oid()) rels.push_back(*rel_oid);
		}
	}

	/* 2. build the VALUES source: (rel, attno, attname, idx_name, idx_type, def_uoi, prefix). */
	std::string values;
	for (Oid rel : rels) {
		const auto rid = bsi::index_relation_identity(rel);
		std::string colsql = fmt::format(
			"SELECT a.attnum, a.attname FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
			"JOIN pg_namespace tn ON t.typnamespace=tn.oid WHERE a.attrelid={}::oid AND t.typname='seq' "
			"AND tn.nspname IN ('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
			static_cast<unsigned>(rel));
		if (SPI_execute(colsql.c_str(), true, 0) != SPI_OK_SELECT) continue;
		std::vector<std::pair<std::int16_t, std::string>> cols;
		for (std::uint64_t c = 0; c < SPI_processed; ++c) {
			auto att_cell = bioseq::pg_utils::pg_spi_cell::get(
				SPI_tuptable->vals[c], SPI_tuptable->tupdesc, 1);
			const std::int16_t a = att_cell.optional<std::int16_t>().value_or(0);
			char* nm = SPI_getvalue(SPI_tuptable->vals[c], SPI_tuptable->tupdesc, 2);
			cols.emplace_back(a, nm ? std::string(nm) : std::string());
		}
		for (auto& [attno, attname] : cols) {
			for (std::size_t i = 0; i < kinds.size(); ++i) {
				const std::string idx_name{kinds[i].first};
				const std::string idx_type{bidx::sequence_index_type_name(kinds[i].second)};
				const bool def_uoi = bidx::default_update_on_insert_contains(kinds[i].first) && allows[i];
				const std::string prefix = fmt::format("seqidx_{}_{:016x}_",
					bsi::short_index_for(kinds[i].first),
					bsi::identity_hash64(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, kinds[i].first));
				if (!values.empty()) values += ",";
				values += fmt::format("({}::oid,{}::int2,{},{},{},{},{})",
					static_cast<unsigned>(rel), static_cast<int>(attno),
					quote_literal_cstr(attname.c_str()),
					quote_literal_cstr(idx_name.c_str()),
					quote_literal_cstr(idx_type.c_str()),
					def_uoi ? "true" : "false",
					quote_literal_cstr(prefix.c_str()));
			}
		}
	}

	if (!values.empty()) {
		std::string query = fmt::format(
			"WITH src(rel,attno,attname,idx_name,idx_type,def_uoi,prefix) AS (VALUES {}) "
			"SELECT src.rel::regclass, src.attno::int2, src.attname, src.idx_name, src.idx_type, "
			"  CASE WHEN p.update_on_insert IS NOT NULL "
			"       THEN (CASE WHEN p.update_on_insert THEN 'UPDATE_ON_INSERT' ELSE 'UPDATE_MANUALLY' END) "
			"       WHEN src.def_uoi THEN 'UPDATE_ON_INSERT' ELSE 'UPDATE_MANUALLY' END, "
			"  pg_size_pretty(COALESCE((SELECT SUM(pg_total_relation_size(ic.oid)) FROM pg_class ic "
			"     JOIN pg_namespace inx ON ic.relnamespace=inx.oid "
			"     WHERE inx.nspname='bioseq_internal' AND ic.relname LIKE src.prefix||'%'),0)::bigint), "
			"  (SELECT now()-m.last_updated FROM bioseq_internal.metadata_age_log m "
			"     WHERE m.user_relid=src.rel AND m.column_attno=src.attno AND m.index_name=src.idx_name LIMIT 1) "
			"FROM src LEFT JOIN bioseq_internal.index_policy p "
			"  ON p.user_relid=src.rel AND p.column_attno=src.attno AND p.index_name=src.idx_name "
			"ORDER BY src.rel, src.attno, src.idx_name", values);
		if (SPI_execute(query.c_str(), true, 0) == SPI_OK_SELECT) {
			for (std::uint64_t r = 0; r < SPI_processed; ++r) {
				bioseq::pg_utils::pg_datum_list<8> row{};
				for (int c = 0; c < 8; ++c) {
					auto cell = bioseq::pg_utils::pg_spi_cell::get(
						SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c + 1);
					if (cell.is_null) {
						row.set_null(c + 1);
					} else {
						row.set_raw(c + 1, cell.value);
					}
				}
				row.put_values(tupstore, tupdesc);
			}
		}
	}

	SPI_finish();
	return static_cast<Datum>(0);
}

/*
 * bioseq.update_index(rel regclass, kind text) — bulk recompute or
 * create the named metadata kind for the given user table. Returns the
 * count of entries written. Forwards into bioseq::metadata_admin.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_update_index);
Datum bioseq_update_index(PG_FUNCTION_ARGS) {
	// DOC0279 §11.4 — route to the chunked-storage reindex driver. The driver
	// validates the name, clears + rebuilds the named index for every SEQ
	// column of every row, and stamps metadata_age_log on success.
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::int64_t n = bioseq::storage::indexing::update_index_impl(rel, name);
	PG_RETURN_INT64(n);
}

/* DOC0279 §12.2 — read the backend-local minimizers_per_sequence merge run
 * counter (test observability). */
CXX_PG_FUNCTION_INFO_V1(bioseq_last_index_merge_run_count);
Datum bioseq_last_index_merge_run_count(PG_FUNCTION_ARGS) {
	PG_RETURN_INT64((std::int64_t)bioseq::storage::indexing::g_last_index_merge_run_count);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_last_mps_build_stats);
Datum bioseq_last_mps_build_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR, (errmsg("bioseq.last_mps_build_stats: composite result type expected")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	const auto st = bioseq::storage::indexing::g_last_mps_build_stats;
	const std::array<std::uint64_t, 18> fields{
		st.backend_pid,
		st.db_oid,
		st.relfilenode,
		st.sequence_rowno,
		st.total_run_count,
		st.resident_runs,
		st.file_runs,
		st.resident_run_bytes,
		st.resident_peak_bytes,
		st.spilled_run_bytes,
		st.postgres_temp_file_bytes,
		st.postgres_temp_file_count,
		st.merge_reader_buffer_bytes,
		st.merge_pass_count,
		st.spill_count_from_pressure,
		st.resident_demotions,
		st.position_spill_bytes,
		st.position_spill_count,
	};
	bioseq::pg_utils::pg_datum_list<18> values{};
	for (std::size_t i = 0; i < fields.size(); ++i) {
		values.set(static_cast<int>(i + 1), static_cast<std::int64_t>(fields[i]));
	}

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(bioseq_scheduler_mps_build_stats);
Datum bioseq_scheduler_mps_build_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR, (errmsg("bioseq.scheduler_mps_build_stats: composite result type expected")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	const auto snap = bioseq::parallelism::pg::async_mps_build_stats_snapshot();
	const auto st = snap.stats;
	const std::array<std::uint64_t, 24> fields{
		st.backend_pid,
		st.db_oid,
		st.relfilenode,
		st.sequence_rowno,
		st.total_run_count,
		st.resident_runs,
		st.file_runs,
		st.resident_run_bytes,
		st.resident_peak_bytes,
		st.spilled_run_bytes,
		st.postgres_temp_file_bytes,
		st.postgres_temp_file_count,
		st.merge_reader_buffer_bytes,
		st.merge_pass_count,
		st.spill_count_from_pressure,
		st.resident_demotions,
		st.position_spill_bytes,
		st.position_spill_count,
		snap.generation,
		snap.worker_build_ns,
		snap.pipeline_mode,
		snap.max_concurrency,
		snap.index_memory_cap_mb,
		snap.lazy_worker_shareable,
	};
	bioseq::pg_utils::pg_datum_list<24> values{};
	for (std::size_t i = 0; i < fields.size(); ++i) {
		values.set(static_cast<int>(i + 1), static_cast<std::int64_t>(fields[i]));
	}

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(bioseq_scheduler_restart);
Datum bioseq_scheduler_restart(PG_FUNCTION_ARGS) {
	bioseq::parallelism::pg::restart_cluster();
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_scheduler_stats);
Datum bioseq_scheduler_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR, (errmsg("bioseq.scheduler_stats: composite result type expected")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	const auto snap = bioseq::parallelism::pg::snapshot();
	const std::string mode_string{bioseq::parallelism::to_string(snap.mode)};
	bioseq::pg_utils::pg_datum_list<16> values{};
	values.set(1, snap.supervisor);
	values.set(2, static_cast<std::int32_t>(snap.config_generation));
	values.set_raw(3, bioseq::pg_utils::pg_datum::from_text_cstring(mode_string.c_str()));
	values.set(4, static_cast<std::int32_t>(snap.min_threads));
	values.set(5, static_cast<std::int32_t>(snap.max_threads));
	values.set(6, static_cast<std::int32_t>(snap.current_threads));
	values.set(7, static_cast<std::int64_t>(snap.batches_scheduled));
	values.set(8, static_cast<std::int64_t>(snap.batches_awaiting));
	values.set(9, static_cast<std::int64_t>(snap.batches_processed));
	values.set(10, static_cast<std::int64_t>(snap.queries_waiting));
	values.set(11, static_cast<std::int64_t>(snap.queries_processed));
	values.set(12, snap.oldest_waiting_batch_age_secs);
	values.set(13, snap.queries_per_sec);
	values.set(14, snap.uptime_secs);
	values.set(15, static_cast<std::int64_t>(snap.async_persist_jobs_waiting));
	values.set(16, static_cast<std::int64_t>(snap.index_memory_reserved_bytes));

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

/* DOC0279 §12.2 — the remaining admin/test SQL helpers. */
CXX_PG_FUNCTION_INFO_V1(bioseq_index_table_row_count);
Datum bioseq_index_table_row_count(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	PG_RETURN_INT64(bioseq::storage::indexing::index_table_row_count_impl(rel, name));
}
CXX_PG_FUNCTION_INFO_V1(bioseq_index_distinct_sub_keys);
Datum bioseq_index_distinct_sub_keys(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t qk = PG_GETARG_INT64(2);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	PG_RETURN_INT64(bioseq::storage::indexing::index_distinct_sub_keys_impl(rel, name, qk));
}
CXX_PG_FUNCTION_INFO_V1(bioseq_drop_all_index_tables);
Datum bioseq_drop_all_index_tables(PG_FUNCTION_ARGS) {
	bioseq::storage::indexing::drop_all_index_tables_impl();
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_set_index_policy);
Datum bioseq_set_index_policy(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	text* pol = PG_GETARG_TEXT_PP(2);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::string policy(VARDATA_ANY(pol), VARSIZE_ANY_EXHDR(pol));
	const bool uoi = (policy == "UPDATE_ON_INSERT");
	const int st = bioseq::storage::indexing::set_index_policy_impl(rel, name, uoi);
	if (st == 1) ereport(ERROR, (errmsg("bioseq.set_index_policy: unknown index '%s'", name.c_str())));
	if (st == 2) ereport(ERROR, (errmsg("bioseq.set_index_policy: index '%s' cannot be UPDATE_ON_INSERT", name.c_str())));
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_set_active_read_index);
Datum bioseq_set_active_read_index(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const int st = bioseq::storage::indexing::set_active_read_index_impl(rel, name);
	if (st == 1) ereport(ERROR, (errmsg("bioseq.set_active_read_index: unknown index '%s'", name.c_str())));
	if (st == 2) ereport(ERROR, (errmsg("bioseq.set_active_read_index: a CHUNKED_PER_TABLE index cannot be the active read index")));
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_column_row_number);
Datum bioseq_column_row_number(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	if (!IsBioseqColumnTableAmTable(rel))
		ereport(ERROR, (errmsg("bioseq.column_row_number: relation is not a USING bioseq table")));
	/* `ctid tid` is a by-reference type; PG_GETARG_DATUM holds a pointer to the
	 * 6-byte ItemPointerData. Copy by value before handing it to the extern
	 * tid_to_row_number (pure ctid arithmetic, access_method.cppm). */
	ItemPointer ctid = reinterpret_cast<ItemPointer>(PG_GETARG_DATUM(1));
	if (ctid == nullptr)
		ereport(ERROR, (errmsg("bioseq.column_row_number: NULL ctid")));
	ItemPointerData tid = *ctid;
	PG_RETURN_INT64(static_cast<std::int64_t>(tid_to_row_number(tid)));
}

/* DOC0279 §12.2/§16.2 — TEST-ONLY controlled-corruption helpers. They resolve
 * the dynamic seqidx_* shard for (rel, name) internally and write / delete a
 * single chunk row, so the integration tests can pin reserved-sentinel
 * filtering, missing-manifest -> recompute, and present-manifest/missing-
 * fragment corruption detection. Harmless in production (explicit admin verbs;
 * never called by the engine). The TestTableCountIndex registry entry IS gated
 * by BIOSEQ_TEST_INDEXES (§14); these byte-level hooks are not, because they
 * change no production behaviour. */
CXX_PG_FUNCTION_INFO_V1(bioseq_test_write_index_chunk);
Datum bioseq_test_write_index_chunk(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::int64_t query_key = PG_GETARG_INT64(2);
	const std::int64_t sub_key   = PG_GETARG_INT64(3);
	const std::int32_t chunk_no  = PG_GETARG_INT32(4);
	Datum payload = bioseq::pg_utils::pg_datum::from_pointer(PG_GETARG_BYTEA_PP(5)).raw();   // detoast before SPI
	bool wrote = false;
	for (const std::string& pfx : bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		auto shards = bioseq::storage::indexing::shard_relnames_with_prefix(pfx);
		if (shards.empty()) continue;
		/* prefer the shard already holding query_key; else the first shard. */
		std::string target = shards.front();
		for (const std::string& sh : shards) {
			if (bioseq::storage::indexing::count_in_shard(sh,
					fmt::format("WHERE query_key={}", static_cast<long long>(query_key))) > 0) {
				target = sh;
				break;
			}
		}
		if (SPI_connect() != SPI_OK_CONNECT) break;
		std::string sql = fmt::format(
			"INSERT INTO bioseq_internal.{} (query_key, sub_key, chunk_no, chunk_bytes, blob) "
			"VALUES ($1,$2,$3,octet_length($4),$4) "
			"ON CONFLICT (query_key, sub_key, chunk_no) DO UPDATE SET chunk_bytes=EXCLUDED.chunk_bytes, blob=EXCLUDED.blob",
			quote_identifier(target.c_str()));
		Oid argt[4] = { INT8OID, INT8OID, INT4OID, BYTEAOID };
		Datum argv[4] = {
			bioseq::pg_utils::pg_datum::from(query_key).raw(),
			bioseq::pg_utils::pg_datum::from(sub_key).raw(),
			bioseq::pg_utils::pg_datum::from(chunk_no).raw(),
			payload
		};
		SPI_execute_with_args(sql.c_str(), 4, argt, argv, nullptr, false, 0);
		SPI_finish();
		wrote = true;
		break;
	}
	if (!wrote)
		ereport(ERROR, (errmsg("bioseq.test_write_index_chunk: no seqidx shard for (%s, %s)",
			get_rel_name(rel), name.c_str())));
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_test_delete_index_manifest);
Datum bioseq_test_delete_index_manifest(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::int64_t query_key = PG_GETARG_INT64(2);
	const std::int64_t dir = static_cast<std::int64_t>(bioseq::indexing::DIRECTORY_SUB_KEY);
	for (const std::string& pfx : bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		for (const std::string& sh : bioseq::storage::indexing::shard_relnames_with_prefix(pfx)) {
			if (SPI_connect() != SPI_OK_CONNECT) continue;
			std::string sql = fmt::format(
				"DELETE FROM bioseq_internal.{} WHERE query_key={} AND sub_key={}",
				quote_identifier(sh.c_str()), static_cast<long long>(query_key), static_cast<long long>(dir));
			SPI_execute(sql.c_str(), false, 0);
			SPI_finish();
		}
	}
	PG_RETURN_VOID();
}
}
