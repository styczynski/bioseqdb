/**
 * @file
 * @brief Declares selection of the mmsearch clauses a scan will  execute.
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

/**
 * @brief Checks whether two calls can share the supported fallback alignment settings.
 *
 * The primary must explicitly require execution without an index. Its other
 * options must be unspecified, and the extra call must use the two-argument
 * form with default options.
 */
export [[nodiscard]] bool bounded_batch_fallback_options_are_supported(
    FuncExpr* primary,
    FuncExpr* extra,
    const bioseq_stub_oids& oids);

/** @brief Selects one mmsearch call or the supported joiu-parameter and constant-filter pair. */
export [[nodiscard]] owned_mmsearch_restriction choose_owned_mmsearch_restriction(
    PlannerInfo* root,
    RelOptInfo* rel,
    RangeTblEntry* rte,
    List* restrictions,
    List* ppi_restrictions,
    Relids param_relids,
    const bioseq_stub_oids& oids);

/** @brief Builds a restriction list excluding clauses assigned to the mmsearch executor. */
export [[nodiscard]] List* restrictions_without_owned_mmsearch(
    List* restrictions,
    List* owned,
    bool restrictions_are_private_copies);
