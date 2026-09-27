/**
 * @file
 * @brief Declares lazy-sequence expression detection without importing IR construction.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_slice_path_expression_detection_contracts;

/** @brief Report whether an expression contains a supported lazy sequence projection. */
export bool bioseq_expr_uses_lazy_sequence_projection(Node* node);
