/**
 * @file
 * @brief Defines lightweight row-identity and target-list contracts for per-row owners.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "catalog/namespace.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_proc.h"
#include "commands/extension.h"
#include "optimizer/appendinfo.h"
#include "optimizer/planmain.h"
#include "storage/dsm.h"
#include "storage/shmem.h"
#include "utils/inval.h"
#include "utils/syscache.h"

#include "rewrite/rewriteManip.h"
}

export module bioseq.postgres.table:custom_scan_join_ownership_target_contracts;

import std;
import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

using namespace bioseq::postgres::support;

export {

    /** Returns true when @p var is an exact ctid or tableoid row-identity Var. */
    [[nodiscard]] bool per_row_is_exact_row_identity_var(const Var* var) noexcept {
        if (var == nullptr || !bioseq_is_concrete_rte_varno(var->varno) || var->varlevelsup != 0 ||
            !bms_is_empty(var->varnullingrels))
            return false;
        return (var->varattno == SelfItemPointerAttributeNumber && var->vartype == TIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid)) ||
               (var->varattno == TableOidAttributeNumber && var->vartype == OIDOID &&
                var->vartypmod == -1 && !OidIsValid(var->varcollid));
    }

    /** Returns true when @p expressions contains an expression equal to @p node. */
    [[nodiscard]] bool per_row_expression_list_contains(List* expressions, Node* node) noexcept {
        if (node == nullptr)
            return false;
        for (Node* existing : postgres_list<Node>(expressions)) {
            if (existing != nullptr && equal(existing, node))
                return true;
        }
        return false;
    }

    /** Resolves authoritative sort and group references for @p expressions. */
    [[nodiscard]] bool per_row_authoritative_sortgrouprefs(PlannerInfo* root,
                                                           List* expressions,
                                                           std::vector<Index>& results);

} // export per-row target contracts
