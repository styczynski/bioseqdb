/**
 * @file
 * @brief Declares exact mmsearch restriction ownership without importing planner walks.
 */
module;
extern "C" {
#include "postgres.h"
#include "nodes/bitmapset.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.table:custom_scan_oid_expression_support_restriction_ownership_contracts;

import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_oid_expression_support_expression_analysis_contracts;
export import :custom_scan_oid_expression_support_storage_pruning_contracts;

/** @brief Test whether a primary and fallback mmsearch use the admitted option pairing. */
export [[nodiscard]] bool bounded_batch_fallback_options_are_supported(
    FuncExpr* primary,
    FuncExpr* extra,
    const bioseq_stub_oids& oids);

/** @brief Select the bounded mmsearch restriction set owned by one custom scan. */
export [[nodiscard]] owned_mmsearch_restriction choose_owned_mmsearch_restriction(
    PlannerInfo* root,
    RelOptInfo* rel,
    RangeTblEntry* rte,
    List* restrictions,
    List* ppi_restrictions,
    Relids param_relids,
    const bioseq_stub_oids& oids);

/** @brief Copy restrictions while excluding the exact mmsearch-owned clauses. */
export [[nodiscard]] List* restrictions_without_owned_mmsearch(
    List* restrictions,
    List* owned,
    bool restrictions_are_private_copies);
