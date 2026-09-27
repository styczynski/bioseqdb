/**
 * @file
 * @brief Declares mmsearch detection and the first checks on its options and operands.
 */
module;
extern "C" {
#include "postgres.h"
#include "nodes/parsenodes.h"
#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>

export module bioseq.postgres.table:custom_scan_oid_expression_support_expression_analysis_contracts;

import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Tests whether an OID names either supported mmsearch overload. */
export [[nodiscard]] inline bool is_mmsearch_funcid(Oid funcid, const bioseq_stub_oids& o) {
    return funcid == o.mmsearch_oid || funcid == o.mmsearch_with_opts_oid;
}

/** @brief Counts mmsearch calls at the root and inside the expression tree. */
export [[nodiscard]] std::size_t count_mmsearch_occurrences(
    Node* node,
    const bioseq_stub_oids& oids);

/** @brief Holds the selected mmsearch clause and any extra clauses the scan will execute. */
export struct owned_mmsearch_restriction {
    RestrictInfo* restriction = nullptr;
    FuncExpr* normalized_expr = nullptr;
    List* extra_restrictions = NIL;
    List* extra_normalized_exprs = NIL;
};

/** @brief Rejects volatile SQL calls in mmsearch options before planner simplification. */
export void bioseq_validate_raw_mmsearch_options(Query* query);

/** @brief Checks whether an expression tree contains an mmsearch call. */
export [[nodiscard]] bool contains_mmsearch_qualifier(
    Node* node,
    const bioseq_stub_oids& oids);

/** @brief Checks the operand's type and relation references before full base-scan validation. */
export [[nodiscard]] bool coarse_base_mmsearch_operand_candidate(
    Node* expression,
    RelOptInfo* rel,
    const bioseq_stub_oids& oids,
    bool require_sequence_var);
