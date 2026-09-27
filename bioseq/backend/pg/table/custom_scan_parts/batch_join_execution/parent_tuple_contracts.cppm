/**
 * @file
 * @brief Cleanup for output slots that borrow lazy sequence values.
 */
module;
struct CustomScanState;

export module bioseq.postgres.table:custom_scan_batch_join_execution_parent_tuple_contracts;

/** Clear output slots and  expression references before releasing their sequence storage. */
export void clear_recipe_scan_parent_tuple_references(CustomScanState* node);
