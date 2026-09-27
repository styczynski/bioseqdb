/**
 * @file
 * @brief Declares cost estimates for custom scans that run mmsearch.
 */
module;
#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.table:custom_scan_slice_path_cost_contracts;

import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Startup and per-input-row costs of running mmsearch in a custom scan. */
export struct owned_mmsearch_cost {
    Cost startup = 0;
    Cost per_input_tuple = 0;
};

/**
 * @brief Return the arguments whose evaluation the mmsearch scan must pay for.
 *
 * The mmsearch SQL function has a large cost to discourage plans that call its
 * stub. Costing its arguments separately avoids charging that penalty to the
 * custom scan that performs the  search.
 */
export [[nodiscard]] List* mmsearch_operand_cost_exprs(
    const FuncExpr* expression,
    const bioseq_stub_oids& oids);

/** @brief Estimate an operand length for mmsearch planning. */
export [[nodiscard]] std::uint64_t estimate_mmsearch_operand_symbols(
    Node* expression,
    const bioseq_stub_oids& oids);

/** @brief Estimate mmsearch work from sequence lengths, input rows, and batch settings. */
export [[nodiscard]] owned_mmsearch_cost estimate_owned_mmsearch_cost(
    std::uint64_t target_length_estimate,
    std::uint64_t query_length_estimate,
    double input_rows,
    bool index_read_requirement,
    std::uint32_t planned_batch_size,
    std::uint64_t planned_materialized_bytes) noexcept;
