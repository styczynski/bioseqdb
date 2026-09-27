/**
 * @file
 * @brief Declares base-scan EXPLAIN helpers without unrelated callback contracts.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_semantic_payload_explain_callback_contracts;

import bioseq.postgres.table.scan_state;

/** @brief Deparse pushed-down clauses into query-context-owned EXPLAIN text. */
export const char* bioseq_column_pushdown_clauses_str(List* context, List* clauses);
/** @brief Deparse projected columns into query-context-owned EXPLAIN text. */
export const char* bioseq_column_projected_columns_str(List* context, List* projected_columns);
/** @brief Build a deparse context rooted at an executor plan state. */
export List* set_deparse_context_planstate(List* dpcontext, Node* node, List* ancestors);
/** @brief Return the physical attribute bitmap required by a Bioseq scan. */
export List* bioseq_column_var_needed(scan_state* bioseq_scan_state);
