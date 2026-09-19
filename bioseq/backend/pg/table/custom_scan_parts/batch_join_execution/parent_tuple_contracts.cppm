/**
 * @file
 * @brief Declares parent-visible tuple cleanup without importing join runtime state.
 */
module;
struct CustomScanState;

export module bioseq.postgres.table:custom_scan_batch_join_execution_parent_tuple_contracts;

/** @brief Clear parent-visible recipe tuple references before retained storage is reset. */
export void clear_recipe_scan_parent_tuple_references(CustomScanState* node);
