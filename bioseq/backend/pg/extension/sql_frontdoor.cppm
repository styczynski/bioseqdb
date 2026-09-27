/**
 * @file sql_frontdoor.cppm
 * @brief SQL search markers, alignment-result helpers and planner support.
 *
 * Bioseq custom scans replace search and result calls during planning.
 * This file also handles direct symbol counts and GPU status queries.
 */
module;

#include <bioseq/pg_compat.h>

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

module bioseq.postgres.extension;

import bioseq.common.logging;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.support.datums;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences;
import bioseq.postgres.storage.manager;
import bioseq.storage.lazy_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.postgres.storage.indexing.admin.policy_catalog;
import bioseq.postgres.scheduler.supervisor;

using namespace bioseq::postgres::support;

namespace {

[[noreturn]] void raise_unowned_mmsearch()
{
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message = "mmsearch qualifier has no Bioseq executor owner",
			.detail =
				"Executing the SQL rendezvous function as a Boolean "
				"predicate is forbidden.",
			.hint =
				"Use exactly one top-level mmsearch predicate with "
				"supported operands, or split the query into separate "
				"levels.",
		});
}

template <class p_result>
[[nodiscard]] auto unowned_projection_result(const std::string_view routine_name, p_result fallback)
    -> p_result {
    bioseq_log<logging_level::debug>(
		"[bioseq] pg: {}() direct call without a custom-scan rewrite",
		routine_name);
	return fallback;
}

} // namespace

namespace bioseq::pg::sql_routines {

/**
 * @brief Mark a similarity search for the planner, using default options.
 * A Bioseq custom scan must execute the search. Calling this function as
 * an ordinary Boolean predicate raises an error.
 * @ingroup bioseq_sql_public_api
 */
bool mmsearch(
        Datum el,
        Datum query) {
    static_cast<void>(el);
    static_cast<void>(query);
	raise_unowned_mmsearch();
}

/**
 * @brief Mark a similarity search with per-call aligner options.
 * Return SQL NULL for a null target or query. Otherwise require the planner
 * to replace this call with a Bioseq custom scan.
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
	raise_unowned_mmsearch();
}

std::int32_t score(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("score", std::int32_t{-1});
}

std::int32_t dp_score(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("dp_score", std::int32_t{-1});
}

std::int64_t query_len(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("query_len", std::int64_t{-1});
}

/**
 * @brief Query start offset supplied by the owning mmsearch scan.
 * Return -1 if the planner leaves this placeholder as a direct call.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t query_start(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("query_start", std::int64_t{-1});
}

/**
 * @brief Query end offset supplied by the owning mmsearch scan.
 * Return -1 if this placeholder is called directly.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t query_end(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("query_end", std::int64_t{-1});
}

std::int64_t target_len(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("target_len", std::int64_t{-1});
}

/**
 * @brief Target start offset supplied by the owning mmsearch scan.
 * Return -1 if this placeholder is called directly.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t target_start(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("target_start", std::int64_t{-1});
}

/**
 * @brief Target end offset supplied by the owning mmsearch scan.
 * Return -1 if this placeholder is called directly.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t target_end(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("target_end", std::int64_t{-1});
}

std::int64_t num_matches(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("num_matches", std::int64_t{-1});
}

std::int64_t block_length(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("block_length", std::int64_t{-1});
}

std::int64_t edit(
        Datum el) {
    static_cast<void>(el);
    bioseq::postgres::support::raise_sql_error(
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
    return unowned_projection_result("mapq", std::int32_t{-1});
}

std::int32_t chain_score(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("chain_score", std::int32_t{-1});
}

std::int32_t dp_max_score(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("dp_max_score", std::int32_t{-1});
}

std::int32_t seed_count(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("seed_count", std::int32_t{-1});
}

std::int32_t secondary_chain_score(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result(
        "secondary_chain_score", std::int32_t{-1});
}

bool is_reverse(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("is_reverse", false);
}

bool is_primary(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("is_primary", false);
}

bool has_cigar(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("has_cigar", false);
}

float divergence(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("divergence", -1.0F);
}

/**
 * @brief CIGAR operations supplied by the owning mmsearch scan.
 * A direct call returns !UNEXPECTED! to expose a missing planner rewrite.
 * @ingroup bioseq_sql_public_api
 */
std::string cigar(
        Datum el) {
    static_cast<void>(el);
    return unowned_projection_result("cigar", std::string{"!UNEXPECTED!"});
}

std::string gpu_warmup_shader_cache() {
    return bioseq::postgres::scheduler::
        warmup_backend_local_shader_cache_report();
}

bioseq::postgres::support::named_output<
    bioseq::postgres::sql_rows::vulkan_runtime_status_row>
vulkan_runtime_status() {
    const auto runtime_status =
        bioseq::postgres::scheduler::snapshot_vulkan_runtime_status();
    return bioseq::postgres::support::named_output{
        bioseq::postgres::sql_rows::vulkan_runtime_status_row{
            .is_available = runtime_status.available,
            .device_name = runtime_status.device_name,
            .max_workgroup_count_x = runtime_status.max_workgroup_count_x,
            .has_scalar_block_layout = runtime_status.scalar_block_layout,
            .initialization_error = runtime_status.initialization_error,
            .device_local_bytes = runtime_status.device_local_bytes,
            .is_software_device = runtime_status.software_device,
        }};
}

/**
 * @brief Count occurrences of one alphabet symbol in a sequence.
 *
 * The symbol must be one byte and belong to the sequence alphabet. This
 * direct call handles in-memory values. Stored locators need a Bioseq scan,
 * which can read the sequence or use its stored symbol-count index.
 *
 * @ingroup bioseq_sql_public_api
 */
std::int64_t symbol_count(
        Datum el,
        std::string sym) {
    auto* const sequence_header = bioseq::postgres::support::invoke_pg_as_cpp(
        [el] { return reinterpret_cast<bioseq::pg_types::seq*>(PG_DETOAST_DATUM(el)); });
    if (sym.size() != 1) {
        bioseq::postgres::support::pfree_if_distinct_from(
            bioseq::postgres::support::pg_datum::from_raw(el),
			sequence_header);
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.symbol_count: symbol must be exactly one byte",
            });
	}
	const char symbol = sym.front();

	if (bioseq::storage::is_locator_stub(sequence_header)) {
        bioseq::postgres::support::pfree_if_distinct_from(
            bioseq::postgres::support::pg_datum::from_raw(el),
			sequence_header);
        bioseq::postgres::support::raise_sql_error(
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

	const int symbol_rank = bioseq::storage::indexing::letter_to_rank(
		sequence_header->alphabet_code,
		symbol);
	if (symbol_rank < 0) {
        bioseq::postgres::support::pfree_if_distinct_from(
            bioseq::postgres::support::pg_datum::from_raw(el),
			sequence_header);
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.symbol_count: symbol is not part of the "
                    "sequence alphabet",
            });
	}

	std::int64_t symbol_count_result = 0;
	bioseq::pg_types::seq::dispatch_instance(
		sequence_header->alphabet_code,
		sequence_header->storage_unit_byte_width,
		[sequence_header, symbol_rank, &symbol_count_result]
		<class sequence_type>() {
			compact_sequence_range<sequence_type> sequence_range{
				*sequence_header};
			auto resident_sequence =
				bioseq::storage::make_resident_lazy_sequence(sequence_range);
			const auto sequence_view =
				bioseq::storage::make_lazy_sequence_view(resident_sequence);
			const auto symbol_counts =
				bioseq::indexing::tally_symbol_counts(sequence_view);
			if (static_cast<std::size_t>(symbol_rank) < symbol_counts.size()) {
				symbol_count_result = static_cast<std::int64_t>(
					symbol_counts[static_cast<std::size_t>(symbol_rank)]);
			}
		});

    bioseq::postgres::support::pfree_if_distinct_from(
        bioseq::postgres::support::pg_datum::from_raw(el), sequence_header);
	return symbol_count_result;
}

} // namespace bioseq::pg::sql_routines

namespace {

/** Store the first column reference found while walking an expression. */
struct first_var_finder final {
	Var* found_var = nullptr;
};

struct first_var_visitor final {
    [[nodiscard]] auto operator()(pg_expression_walk_cursor,
                                  const pg_node_view node,
                                  first_var_finder& finder) const noexcept -> pg_walk_step {
        if (finder.found_var != nullptr) {
			return pg_walk_step::stop;
		}
        if (auto* const var = node.try_cast<Var>()) {
            finder.found_var = var;
			return pg_walk_step::stop;
        }
        return pg_walk_step::descend;
    }
};

/**
 * Keep a projection beside its source relation when a query has joins or
 * aggregates. Use the first column reference to build a PlaceHolderVar.
 * Return nullptr if the request cannot be rewritten or has no column reference.
 */
[[nodiscard]] Node* try_wrap_in_placeholder(
		const SupportRequestSimplify* simplify_request)
{
	if (simplify_request == nullptr || simplify_request->root == nullptr
			|| simplify_request->fcall == nullptr) {
		return nullptr;
	}
	PlannerInfo* const planner_root = simplify_request->root;
	const bool has_joins = planner_root->hasJoinRTEs;
	const bool has_aggregates =
		planner_root->parse != nullptr && planner_root->parse->hasAggs;
	if (!has_joins && !has_aggregates) {
		return nullptr;
	}
	first_var_finder finder{};
	const first_var_visitor visitor{};
	static_cast<void>(walk_pg_expression_including_root(
		to_pg_node(simplify_request->fcall), finder, visitor));
	if (finder.found_var == nullptr) {
		bioseq_log<logging_level::debug>(
			"[bioseq] pg: try_wrap_in_placeholder() no Var found in "
			"fcall — skipping");
		return nullptr;
	}
	Var* const argument_var = finder.found_var;
	const Relids placeholder_relations =
		bms_make_singleton(static_cast<int>(argument_var->varno));
	Expr* const expression_copy = reinterpret_cast<Expr*>(
		copyObjectImpl(simplify_request->fcall));
	PlaceHolderVar* const placeholder = make_placeholder_expr(
		planner_root, expression_copy, placeholder_relations);
	placeholder->phlevelsup = argument_var->varlevelsup;
	placeholder->phnullingrels = argument_var->varnullingrels;
	bioseq_log<logging_level::debug>(
			"[bioseq] pg: try_wrap_in_placeholder() wrapped funcid={} "
			"phid={} phrels={}",
			simplify_request->fcall->funcid,
			placeholder->phid,
			static_cast<int>(argument_var->varno));
	return reinterpret_cast<Node*>(placeholder);
}

} // namespace

namespace bioseq::pg::sql_routines {

/**
 * Give direct symbol counting a high planner cost and keep its projection
 * at the source relation in queries with joins or aggregates.
 */
Datum bioseq_symbol_count_support(
        Datum request_datum) {
	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_symbol_count_support() entered");
	Node* const request_node = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	if (SupportRequestCost* cost_request =
			try_cast_pg_node<SupportRequestCost>(request_node)) {
		bioseq_log<logging_level::debug>(
			"[bioseq] pg: bioseq_symbol_count_support() received "
			"SupportRequestCost");
		cost_request->startup = 50'000.0;
		cost_request->per_tuple = 0.0;
		return PointerGetDatum(cost_request);
	}
	if (SupportRequestSimplify* simplify_request =
			try_cast_pg_node<SupportRequestSimplify>(request_node)) {
		/* Evaluate symbol_count at its source relation so joins or aggregates
		 * do not duplicate the projection work. */
		Node* const placeholder = try_wrap_in_placeholder(simplify_request);
		if (placeholder != nullptr) {
			return PointerGetDatum(placeholder);
		}
	}
	return PointerGetDatum(nullptr);
}

/**
 * Give an ordinary mmsearch call a high planner cost so PostgreSQL prefers
 * a Bioseq custom scan that performs the search. Direct execution of the
 * SQL marker raises an error.
 */
Datum mmsearch_support(
        Datum request_datum) {
	bioseq_log<logging_level::debug>("[bioseq] pg: mmsearch_support() entered");
	Node* const request_node = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	if (SupportRequestCost* cost_request =
			try_cast_pg_node<SupportRequestCost>(request_node))
	{
		bioseq_log<logging_level::debug>(
			"[bioseq] pg: mmsearch_support() received SupportRequestCost");
		PlannerInfo* const planner_root = cost_request->root;
		if (planner_root == nullptr) {
			bioseq_log<logging_level::debug>(
				"[bioseq] pg: mmsearch_support() planner root is null");
		} else if (planner_root->non_recursive_path == nullptr) {
			bioseq_log<logging_level::debug>(
				"[bioseq] pg: mmsearch_support() non_recursive_path is null");
		} else {
			bioseq_log<logging_level::debug>(
				"[bioseq] pg: mmsearch_support() non_recursive_path "
				"pathtype={}",
				static_cast<int>(
					planner_root->non_recursive_path->pathtype));
		}
		cost_request->startup = 100'000.0;
		cost_request->per_tuple = 0.0;
		return PointerGetDatum(cost_request);
	}
	return PointerGetDatum(nullptr);
}

/**
 * Keep alignment-result projections at the scan that supplies their values
 * when the query contains joins or aggregates. Return NULL if PostgreSQL
 * should keep the original function expression.
 */
Datum bioseq_placeholder_support(
        Datum request_datum) {
	bioseq_log<logging_level::debug>("[bioseq] pg: bioseq_placeholder_support() entered");
	Node* const request_node = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	SupportRequestSimplify* const simplify_request =
		try_cast_pg_node<SupportRequestSimplify>(request_node);
	if (simplify_request == nullptr) {
		return PointerGetDatum(nullptr);
	}
	Node* const placeholder = try_wrap_in_placeholder(simplify_request);
	return PointerGetDatum(placeholder);
}

} // namespace bioseq::pg::sql_routines
