/**
 * @file
 * @brief Declares expression and column-list formatting for base-scan EXPLAIN.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_semantic_payload_explain_callback_contracts;

import bioseq.postgres.table.scan_state;

/** @brief Formats pushed-down filters as an SQL expression for EXPLAIN. */
export const char* bioseq_column_pushdown_clauses_str(List* context, List* clauses);
/** @brief Formats projected columns for EXPLAIN, with a message when no columns are read. */
export const char* bioseq_column_projected_columns_str(List* context, List* projected_columns);
/** @brief Builds the expression-printing context from the plan inside a PlanState. */
export List* set_deparse_context_planstate(List* dpcontext, Node* node, List* ancestors);
/** @brief Returns typed Vars for the physical columns needed by a Bioseq scan. */
export List* bioseq_column_var_needed(scan_state* bioseq_scan_state);
