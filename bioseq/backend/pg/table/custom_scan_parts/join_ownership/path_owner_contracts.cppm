/**
 * @file
 * @brief Reads the target expressions owned by Bioseq custom paths.
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

export module bioseq.postgres.table:custom_scan_join_ownership_path_owner_contracts;

import std;
import bioseq.postgres.support;
import :custom_scan_plan_abi_constants;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_oid_expression_support_expression_analysis_contracts;
import :custom_scan_planner_method_contracts;

using namespace bioseq::postgres::support;

export {
    /** @brief Reads a BatchJoin or PerRow target expression and checks the stored version, fields, and child count. */
    [[nodiscard]] Node* per_row_known_path_owner_recipe(CustomPath* custom, bool& invalid) {
        if (custom == nullptr)
            return nullptr;
        const bool raw_owner = custom->methods == bioseq_batch_join_path_methods();
        const bool transformed_owner = custom->methods == bioseq_per_row_join_path_methods();
        if (!raw_owner && !transformed_owner)
            return nullptr;
        const int expected_version =
            raw_owner ? k_bioseq_batch_join_plan_version : k_bioseq_per_row_join_plan_version;
        const int expected_children = raw_owner ? 2 : 1;
        const int expected_fields = raw_owner ? k_bioseq_batch_join_path_private_fields
                                              : k_bioseq_per_row_join_path_private_fields;
        Node* version = list_length(custom->custom_private) == expected_fields
                            ? reinterpret_cast<Node*>(linitial(custom->custom_private))
                            : nullptr;
        Node* recipe = list_length(custom->custom_private) == expected_fields
                           ? reinterpret_cast<Node*>(list_nth(custom->custom_private, 4))
                           : nullptr;
        if (version == nullptr || !IsA(version, Integer) || intVal(version) != expected_version ||
            list_length(custom->custom_paths) != expected_children || recipe == nullptr ||
            exprType(recipe) != get_cached_stub_oids().bioseq_seq_type_oid) {
            invalid = true;
            return nullptr;
        }
        return recipe;
    }

    /** @brief Reads the primary mmsearch target from a BioseqScan and flags malformed or duplicate owner tags. */
    [[nodiscard]] Node* per_row_base_scan_owner_recipe(CustomPath* custom, bool& invalid) {
        if (custom == nullptr || custom->methods != bioseq_column_scan_path_methods()) {
            return nullptr;
        }
        Node* recipe = nullptr;
        for (Node* private_node : postgres_list<Node>(custom->custom_private)) {
            if (private_node == nullptr || !IsA(private_node, List))
                continue;
            List* tag = reinterpret_cast<List*>(private_node);
            Node* magic = list_length(tag) >= 1 ? reinterpret_cast<Node*>(linitial(tag)) : nullptr;
            if (magic == nullptr || !IsA(magic, Integer) ||
                intVal(magic) != k_bioseq_mmsearch_owner_magic)
                continue;
            Node* version =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 1)) : nullptr;
            Node* expressions_node =
                list_length(tag) == 3 ? reinterpret_cast<Node*>(list_nth(tag, 2)) : nullptr;
            List* expressions = expressions_node != nullptr && IsA(expressions_node, List)
                                    ? reinterpret_cast<List*>(expressions_node)
                                    : NIL;
            FuncExpr* mmsearch =
                expressions == NIL
                    ? nullptr
                    : try_cast_pg_node<FuncExpr>(reinterpret_cast<Node*>(linitial(expressions)));
            if (recipe != nullptr || version == nullptr || !IsA(version, Integer) ||
                intVal(version) != k_bioseq_mmsearch_owner_version || mmsearch == nullptr ||
                list_length(expressions) > 2 ||
                !is_mmsearch_funcid(mmsearch->funcid, get_cached_stub_oids()) ||
                (list_length(mmsearch->args) != 2 && list_length(mmsearch->args) != 3) ||
                list_length(custom->custom_restrictinfo) != list_length(expressions)) {
                invalid = true;
                return nullptr;
            }
            for (Node* expression : postgres_list<Node>(expressions)) {
                FuncExpr* owned = try_cast_pg_node<FuncExpr>(expression);
                if (owned == nullptr ||
                    !is_mmsearch_funcid(owned->funcid, get_cached_stub_oids()) ||
                    (list_length(owned->args) != 2 && list_length(owned->args) != 3)) {
                    invalid = true;
                    return nullptr;
                }
            }
            recipe = reinterpret_cast<Node*>(linitial(mmsearch->args));
            if (recipe == nullptr ||
                exprType(recipe) != get_cached_stub_oids().bioseq_seq_type_oid) {
                invalid = true;
                return nullptr;
            }
        }
        return recipe;
    }
}
