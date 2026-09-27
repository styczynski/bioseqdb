/**
 * @file
 * @brief Declares path construction for mmsearch over joined-row sequence expressions.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_per_row_join_paths_transformed_admission_contracts;

import bioseq.indexing.type;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Adds per-row alignment paths for a transformed mmsearch predicate. */
export [[nodiscard]] bool per_row_add_transformed_mmsearch_paths(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    JoinPathExtraData* extra,
    RestrictInfo* restriction,
    FuncExpr* mmsearch,
    FuncExpr* executable_mmsearch,
    bioseq::indexing::index_read_requirement requirement,
    const bioseq_stub_oids& oids);
