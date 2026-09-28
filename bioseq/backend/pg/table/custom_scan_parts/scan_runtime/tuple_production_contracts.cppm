/**
 * @file
 * @brief Declares row filtering and mmsearch operand evaluation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_runtime_tuple_production_contracts;

import bioseq.postgres.table.scan_state;
import :custom_scan_scan_runtime_source_lifecycle_contracts;

/** @brief Checks a row's filters, then evaluates its configured mmsearch operands. */
export [[nodiscard]] mmsearch_row_evaluation
evaluate_mmsearch_input_row(bioseq_scan_runtime& runtime,
                            TupleTableSlot* slot,
                            ExprState* qual,
                            ExprContext* expression_context);
