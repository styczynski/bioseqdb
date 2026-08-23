/**
 * @file
 * @brief Declares transformed per-row descendant-plan auditing without recursive implementations.
 */
module;
struct Plan;
struct FuncExpr;
struct Node;

export module bioseq.postgres.table:custom_scan_join_ownership_descendant_neutralization_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Counts executable mmsearch copies remaining below a transformed owner. */
export struct per_row_mmsearch_plan_audit {
    std::size_t executable_qual_copies = 0u;
    std::size_t vector_qual_copies = 0u;
    std::size_t storage_qual_copies = 0u;
    std::size_t descendant_owner_copies = 0u;

    /** @brief Reports whether no residual or descendant owner copy remains. */
    [[nodiscard]] bool clean() const noexcept {
        return executable_qual_copies == 0u && vector_qual_copies == 0u &&
               storage_qual_copies == 0u && descendant_owner_copies == 0u;
    }
};

/** @brief Adds all mmsearch occurrences in one expression to an audit counter. */
export void per_row_audit_mmsearch_expression(
    Node* expression,
    std::size_t& destination,
    const bioseq_stub_oids& oids);

/** @brief Audits every executable qual store below a transformed per-row owner. */
export void per_row_audit_mmsearch_plan(
    Plan* plan,
    per_row_mmsearch_plan_audit& audit,
    const bioseq_stub_oids& oids,
    FuncExpr* exact_owner_candidate = nullptr);
