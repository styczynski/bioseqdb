/**
 * @file
 * @brief Declares mmsearch planner-cost contracts without importing SliceProject codecs.
 */
module;
#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.table:custom_scan_slice_path_cost_contracts;

import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Startup and per-input-tuple planner costs for an owning mmsearch path. */
export struct owned_mmsearch_cost {
    Cost startup = 0;
    Cost per_input_tuple = 0;
};

/** @brief Return only the executable operands of a validated mmsearch expression. */
export [[nodiscard]] List* mmsearch_operand_cost_exprs(
    const FuncExpr* expression,
    const bioseq_stub_oids& oids);

/** @brief Estimate symbol count for one mmsearch operand expression. */
export [[nodiscard]] std::uint64_t estimate_mmsearch_operand_symbols(
    Node* expression,
    const bioseq_stub_oids& oids);

/** @brief Compute finite startup and per-tuple costs from workload estimates. */
export [[nodiscard]] owned_mmsearch_cost estimate_owned_mmsearch_cost(
    std::uint64_t target_length_estimate,
    std::uint64_t query_length_estimate,
    double input_rows,
    bool index_read_requirement,
    std::uint32_t planned_batch_size,
    std::uint64_t planned_materialized_bytes) noexcept;
