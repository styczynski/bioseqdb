/**
 * @file
 * @brief Checks function and operator expressions against  their PostgreSQL catalog signatures.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_expression_signature_contract;

import bioseq.postgres.support;

using namespace bioseq::postgres::support;

export {
    /** @brief Checks that a scalar function call has the argument and result types declared in the catalog. */
    [[nodiscard]] bool per_row_function_signature_is_exact(FuncExpr* function) {
        if (function == nullptr || function->funcretset || OidIsValid(function->funcvariadic))
            return false;
        Oid* catalog_argument_types = nullptr;
        int catalog_argument_count = 0;
        const Oid catalog_result_type =
            get_func_signature(function->funcid, &catalog_argument_types, &catalog_argument_count);
        if (catalog_result_type != function->funcresulttype ||
            catalog_argument_count != list_length(function->args))
            return false;
        int argument_index = 0;
        for (Node* argument : postgres_list<Node>(function->args)) {
            if (argument == nullptr ||
                exprType(argument) != catalog_argument_types[argument_index++])
                return false;
        }
        return true;
    }

    /** @brief Checks the operator's implementation function and its argument and result types against the catalog. */
    [[nodiscard]] bool per_row_operator_signature_is_exact(OpExpr* operation) {
        if (operation == nullptr || !OidIsValid(operation->opno) ||
            !OidIsValid(operation->opfuncid) || operation->opretset ||
            get_opcode(operation->opno) != operation->opfuncid)
            return false;
        Oid* catalog_argument_types = nullptr;
        int catalog_argument_count = 0;
        const Oid catalog_result_type = get_func_signature(
            operation->opfuncid, &catalog_argument_types, &catalog_argument_count);
        if (catalog_result_type != operation->opresulttype ||
            catalog_argument_count != list_length(operation->args))
            return false;
        int argument_index = 0;
        for (Node* argument : postgres_list<Node>(operation->args)) {
            if (argument == nullptr ||
                exprType(argument) != catalog_argument_types[argument_index++])
                return false;
        }
        return true;
    }
}
