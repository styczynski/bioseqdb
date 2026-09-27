/**
 * @file
 * @brief Checks which match helper belongs to a search target.
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

export module bioseq.postgres.table:custom_scan_join_ownership_helper_contracts;

export import :custom_scan_join_ownership_helper_presence_contracts;

import bioseq.postgres.support;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

using namespace bioseq::postgres::support;

export {
    /**
     * @brief Returns a match-helper call with a valid function, argument count, and result type.
     *
     * Its argument is matched to an executor owner later, after standard_planner
     * assigns the final plan_node_id values.
     */
    [[nodiscard]] FuncExpr*
    batch_join_well_formed_match_helper(Node* expression,
                                        const bioseq_stub_oids& oids) noexcept {
        FuncExpr* helper = try_cast_pg_node<FuncExpr>(expression);
        if (helper == nullptr || !is_bioseq_projection_funcid(helper->funcid, oids) ||
            helper->funcretset || OidIsValid(helper->funcvariadic) ||
            get_func_rettype(helper->funcid) != helper->funcresulttype ||
            list_length(helper->args) != 1) {
            return nullptr;
        }
        return helper;
    }

    /** @brief Returns the sole argument of a well-formed match helper. */
    [[nodiscard]] Node* batch_join_match_helper_argument(FuncExpr* helper) noexcept {
        return helper == nullptr || list_length(helper->args) != 1
                   ? nullptr
                   : reinterpret_cast<Node*>(linitial(helper->args));
    }

    /** @brief Returns the target expression's source sequence Vars, deduplicated and in a stable order. */
    [[nodiscard]] List* canonical_match_base_lineage(Node* target_recipe,
                                                     const bioseq_stub_oids& oids);

    /** @brief Stores the target expression and its source sequence Vars for later owner resolution. */
    [[nodiscard]] List* make_match_owner_descriptor(Node* target_recipe,
                                                    Index base_relid,
                                                    const bioseq_stub_oids& oids);

    /** @brief Records where match helpers occur in the base scan's output and checks which target they belong to. */
    [[nodiscard]] List* collect_base_match_helper_sites(List* targetlist,
                                                        Node* target_recipe,
                                                        Index target_relid,
                                                        AttrNumber primary_target_attno,
                                                        const bioseq_stub_oids& oids);

    /** @brief Checks whether the helper argument equals the complete target expression. */
    [[nodiscard]] bool batch_join_match_helper_names_recipe(FuncExpr* helper,
                                                            Node* target_recipe) noexcept {
        Node* argument = batch_join_match_helper_argument(helper);
        return argument != nullptr && target_recipe != nullptr && equal(argument, target_recipe);
    }

    /** @brief Checks whether the helper names a source sequence or another column from a source relation. */
    [[nodiscard]] bool batch_join_match_helper_names_base_lineage(FuncExpr* helper,
                                                                  Node* target_recipe,
                                                                  const bioseq_stub_oids& oids) {
        Var* argument = try_cast_pg_node<Var>(batch_join_match_helper_argument(helper));
        if (argument == nullptr || target_recipe == nullptr ||
            !bioseq_is_concrete_rte_varno(argument->varno) || argument->varattno <= 0 ||
            argument->varlevelsup != 0 || !bms_is_empty(argument->varnullingrels))
            return false;
        for (Var* base : postgres_list<Var>(canonical_match_base_lineage(target_recipe, oids))) {
            if (base == nullptr)
                continue;
            if (argument->vartype == oids.bioseq_seq_type_oid) {
                if (equal(argument, base))
                    return true;
            } else if (argument->varno == base->varno &&
                       argument->varlevelsup == base->varlevelsup) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Returns the match-helper call inside a valid PlaceHolderVar.
     *
     * A concatenated target can draw on several relations, so phrels may contain
     * several entries. The final owner resolver checks the helper argument and
     * its source columns before execution.
     */
    [[nodiscard]] FuncExpr*
    batch_join_match_helper_phv_function(const PlaceHolderVar* placeholder,
                                         const bioseq_stub_oids& oids) noexcept {
        if (placeholder == nullptr || placeholder->phid == 0 || placeholder->phlevelsup != 0 ||
            !bms_is_empty(placeholder->phnullingrels) || bms_is_empty(placeholder->phrels)) {
            return nullptr;
        }
        return batch_join_well_formed_match_helper(reinterpret_cast<Node*>(placeholder->phexpr),
                                                   oids);
    }
}
