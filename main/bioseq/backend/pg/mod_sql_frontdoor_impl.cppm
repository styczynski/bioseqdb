/*-------------------------------------------------------------------------
 *
 * mod_sql_frontdoor_impl.cppm
 *
 * Ordinary implementation unit for SQL rendezvous, projection, runtime
 * status, symbol-count, and planner-support routines declared by bioseq.mod.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

extern "C" {
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "catalog/pg_collation_d.h"
#include "executor/executor.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
}

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module bioseq.mod;

import bioseq.common.mlogging;
import bioseq.column;
import bioseq.config;
import bioseq.mod.alignment.sql_rows;
import bioseq.mod.sql_rows;
import bioseq.pg_utils;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.pg_utils.row_codec_runtime_test_support;
#endif
import bioseq.pg_utils.runtime_test_counters;
import bioseq.pg_utils.sql_catalog_entities;
import bioseq.pg.cache_provider;
import bioseq.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import bioseq.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
import bioseq.sql.generated.core.storage_metadata_core.entity_handles;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.sql.generated.test_indexes.guc_registry;
#else
import bioseq.sql.generated.production.guc_registry;
#endif
import bioseq.types.pg_compact_sequence;
import bioseq.vector.execution;
import bioseq.vector.types;
import bioseq.vector.types.aggregates;
import bioseq.sequences;
import bioseq.storage.manager;
import bioseq.storage.scanner_pg;
import bioseq.storage.lazy;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.storage.indexing_pg.admin;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;
import bioseq.scheduler.supervisor_pg;
import bioseq.parallelism.mode;
import bioseq.common.sequan_compat;
import bioseq.column.tam;
import bioseq.pg_utils.nodes;
import bioseq.table.scan_state;
import bioseq.table;
import bioseq.table.sql_rows;

using namespace bioseq::pg_utils;

namespace bioseq::pg::sql_routines {

/**
 * @brief Requests similarity search with default aligner options.
 * @ingroup bioseq_sql_public_api
 */
bool mmsearch(
        Datum el,
        Datum query) {
    static_cast<void>(el);
    static_cast<void>(query);
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message =
                "mmsearch qualifier has no Bioseq executor owner",
            .detail =
                "Executing the SQL rendezvous function as a Boolean "
                "predicate is forbidden.",
            .hint =
                "Use exactly one top-level mmsearch predicate with "
                "supported operands, or split the query into separate "
                "levels.",
        });
}

/**
 * @brief Similarity-search predicate with explicit per-call aligner options.
 * @ingroup bioseq_sql_public_api
 */
std::optional<bool> mmsearch(
        std::optional<Datum> el,
        std::optional<Datum> query,
        std::optional<Datum> opts) {
    static_cast<void>(opts);
    if (!el.has_value() || !query.has_value()) {
        return std::nullopt;
    }
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message =
                "mmsearch qualifier has no Bioseq executor owner",
            .detail =
                "Executing the SQL rendezvous function as a Boolean "
                "predicate is forbidden.",
            .hint =
                "Use exactly one top-level mmsearch predicate with "
                "supported operands, or split the query into separate "
                "levels.",
        });
}

std::int32_t score(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>(
        "[bioseq] pg: score() UNEXPECTED direct call -- custom scan did "
        "not rewrite this call site");
    return -1;
}

std::int32_t dp_score(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: dp_score() UNEXPECTED direct call");
    return -1;
}

std::int64_t query_len(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: query_len() UNEXPECTED direct call");
    return -1;
}

/**
 * @brief Start offset of the current mmsearch alignment on the query sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t query_start(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: query_start() UNEXPECTED direct call");
    return -1;
}

/**
 * @brief End offset of the current mmsearch alignment on the query sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t query_end(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: query_end() UNEXPECTED direct call");
    return -1;
}

std::int64_t target_len(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: target_len() UNEXPECTED direct call");
    return -1;
}

/**
 * @brief Start offset of the current mmsearch alignment on the stored target sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t target_start(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: target_start() UNEXPECTED direct call");
    return -1;
}

/**
 * @brief End offset of the current mmsearch alignment on the stored target sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t target_end(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: target_end() UNEXPECTED direct call");
    return -1;
}

std::int64_t num_matches(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: num_matches() UNEXPECTED direct call");
    return -1;
}

std::int64_t block_length(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: block_length() UNEXPECTED direct call");
    return -1;
}

std::int64_t edit(
        Datum el) {
    static_cast<void>(el);
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message =
                "bioseq.edit requires a bioseq alignment custom scan",
            .hint =
                "Use bioseq.edit(...) in a query whose WHERE clause "
                "contains mmsearch(...).",
        });
}

std::int32_t mapq(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: mapq() UNEXPECTED direct call");
    return -1;
}

std::int32_t chain_score(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: chain_score() UNEXPECTED direct call");
    return -1;
}

std::int32_t dp_max_score(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: dp_max_score() UNEXPECTED direct call");
    return -1;
}

std::int32_t seed_count(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: seed_count() UNEXPECTED direct call");
    return -1;
}

std::int32_t secondary_chain_score(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>(
        "[bioseq] pg: secondary_chain_score() UNEXPECTED direct call");
    return -1;
}

bool is_reverse(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: is_reverse() UNEXPECTED direct call");
    return false;
}

bool is_primary(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: is_primary() UNEXPECTED direct call");
    return false;
}

bool has_cigar(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: has_cigar() UNEXPECTED direct call");
    return false;
}

float divergence(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: divergence() UNEXPECTED direct call");
    return -1.0F;
}

/**
 * @brief CIGAR string describing the current mmsearch alignment.
 * @ingroup bioseq_sql_public_api
 */
std::string cigar(
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: cigar() UNEXPECTED direct call");
    return "!UNEXPECTED!";
}

} // namespace bioseq::pg::sql_routines

namespace bioseq::pg::sql_routines {

std::string gpu_warmup_shader_cache() {
    return bioseq::parallelism::pg::
        warmup_backend_local_shader_cache_report();
}

bioseq::pg_utils::named_out<
    bioseq::pg::sql_rows::vulkan_runtime_status_row>
vulkan_runtime_status() {
    const auto status =
        bioseq::parallelism::pg::vulkan_runtime_status_snapshot();
    return bioseq::pg_utils::named_out{
        bioseq::pg::sql_rows::vulkan_runtime_status_row{
            .available = status.available,
            .device_name = status.device_name,
            .max_workgroup_count_x = status.max_workgroup_count_x,
            .scalar_block_layout = status.scalar_block_layout,
            .initialization_error = status.initialization_error,
            .device_local_bytes = status.device_local_bytes,
            .software_device = status.software_device,
        }};
}

/**
 * @brief Number of occurrences of a given symbol within a bioseq.seq value.
 *
 * Stored locator values are resolved by the Bioseq scan path, which can
 * access their owning relation and persisted symbol-count metadata.
 *
 * @ingroup bioseq_sql_public_api
 */
std::int64_t symbol_count(
        Datum el,
        std::string sym) {
	/* Detoast the bioseq.seq varlena and reinterpret as bioseq::pg_types::seq. */
	bioseq::pg_types::seq* hdr = bioseq::pg_utils::invoke_pg_as_cpp(
		[el] {
			return reinterpret_cast<bioseq::pg_types::seq*>(
				PG_DETOAST_DATUM(el));
		});
	if (sym.size() != 1) {
        bioseq::pg_utils::pfree_if_distinct_from(
            bioseq::pg_utils::pg_datum::from_raw(el), hdr);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.symbol_count: symbol must be exactly one byte",
            });
	}
	const char sym_char = sym.front();

	if (bioseq::storage::is_locator_stub(hdr)) {
        bioseq::pg_utils::pfree_if_distinct_from(
            bioseq::pg_utils::pg_datum::from_raw(el), hdr);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            {
                .message =
                    "bioseq.symbol_count cannot tally a stored sequence "
                    "outside a scan",
                .hint =
                    "Query the column through the Bioseq scan or materialize "
                    "the symbol-count index with bioseq.update_index.",
            });
	}

	int rank = bioseq::storage::indexing::letter_to_rank(hdr->alphabet_code, sym_char);
	if (rank < 0) {
        bioseq::pg_utils::pfree_if_distinct_from(
            bioseq::pg_utils::pg_datum::from_raw(el), hdr);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.symbol_count: symbol is not part of the "
                    "sequence alphabet",
            });
	}

	int64_t computed_result = 0;
	bioseq::pg_types::seq::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width,
		[hdr, rank, &computed_result]<class S>() {
			compact_sequence_range<S> range{*hdr};
			auto lazy = bioseq::storage::make_lazy_in_memory(range);
			const auto lazy_view =
				bioseq::storage::as_lazy_sequence_view(lazy);
			auto counts = bioseq::indexing::tally_symbol_counts(lazy_view);  // std::array<int64,N=2^bits_per_char>
			if (static_cast<size_t>(rank) < counts.size()) {
				computed_result = static_cast<int64_t>(counts[rank]);
			}
		});

    bioseq::pg_utils::pfree_if_distinct_from(
        bioseq::pg_utils::pg_datum::from_raw(el), hdr);
	return computed_result;
}

/*
 * bioseq_symbol_count_support is the planner-support function attached to
 * bioseq.symbol_count(). We return a high per-call cost so any query with a
 * cheaper alternative (the bioseq columnar custom scan) is preferred by the
 * optimiser. The cost is lower than mmsearch_support's (100000) because the
 * scalar symbol_count implementation is O(n) where n is the sequence
 * length, while alignment is substantially more expensive. Custom-scan
 * ownership is selected structurally; these support costs describe only the
 * ordinary scalar alternative.
 */
/*
 * Hand-rolled walker that returns the first Var in a node tree without
 * raising on Aggref/WindowFunc/SubLink/etc. Used by the simplify helper
 * below. We deliberately avoid Postgres' pull_var_clause() because that
 * call is documented to raise from late planning paths and we are
 * invoked from eval_const_expressions() which is reached very early in
 * subquery_planner.
 */
} // namespace bioseq::pg::sql_routines

namespace {

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

} // namespace

namespace bioseq::pg::sql_routines {


Datum bioseq_symbol_count_support(
        Datum request_datum) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() entered");
	Node* request = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	if (SupportRequestCost* cost_request = pg_node_try_as<SupportRequestCost>(request)) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() received SupportRequestCost");
		cost_request->startup = 50000;
		cost_request->per_tuple = 0;
		return PointerGetDatum(cost_request);
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
			return PointerGetDatum(wrapped);
		}
	}
	return PointerGetDatum(nullptr);
}

/*
 * mmsearch_support is the planner-support function attached to mmsearch().
 * PostgreSQL calls it with SupportRequest nodes so we can adjust cost
 * estimates before planning proceeds.
 */

Datum mmsearch_support(
        Datum request_datum) {
	bioseq_log<DBG>("[bioseq] pg: mmsearch_support() entered");
	Node* response = nullptr;
	Node* request = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
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
		return PointerGetDatum(cost_request);
	}
	return PointerGetDatum(response);
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

Datum bioseq_placeholder_support(
        Datum request_datum) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_placeholder_support() entered");
	Node* request = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	SupportRequestSimplify* simplify = pg_node_try_as<SupportRequestSimplify>(request);
	if (simplify == nullptr) {
		return PointerGetDatum(nullptr);
	}
	Node* wrapped = try_wrap_in_placeholder(simplify);
	return PointerGetDatum(wrapped);
}

} // namespace bioseq::pg::sql_routines
