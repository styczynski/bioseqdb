/**
 * @file
 * @brief Declares stable CustomScan executor method tables without importing implementations.
 */
module;
struct Node;
struct CustomScan;
struct CustomScanState;
struct EState;
struct TupleTableSlot;
struct List;
struct ExplainState;
struct CustomExecMethods;

export module bioseq.postgres.table:custom_scan_execution_method_contracts;

/** @brief Allocate executor state for a planned Bioseq base scan. */
export Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan);

/** @brief Allocate executor state for a planned batched join. */
export Node* bioseq_batch_join_create_custom_scan_state(CustomScan* cscan);

/** @brief Initialize a Bioseq scan within the executor query context. */
export void bioseq_column_scan_begin_custom_scan(CustomScanState* node,
                                                 EState* estate,
                                                 int eflags);

/** @brief Produce the next tuple from a Bioseq base scan. */
export TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node);

/** @brief Release Bioseq scan resources before query-context teardown. */
export void bioseq_column_scan_end_custom_scan(CustomScanState* node);

/** @brief Reset a Bioseq scan for a new executor pass. */
export void bioseq_column_scan_rescan_custom_scan(CustomScanState* node);

/** @brief Render Bioseq scan planning and runtime properties for EXPLAIN. */
export void bioseq_column_scan_explain_custom_scan(CustomScanState* node,
                                                   List* ancestors,
                                                   ExplainState* es);

/** @brief Initialize a per-row join and its child executor state. */
export void bioseq_per_row_join_begin_custom_scan(CustomScanState* node,
                                                  EState* estate,
                                                  int eflags);

/** @brief Produce the next projected match from a per-row join. */
export TupleTableSlot* bioseq_per_row_join_exec_custom_scan(CustomScanState* node);

/** @brief Release per-row join resources before query-context teardown. */
export void bioseq_per_row_join_end_custom_scan(CustomScanState* node);

/** @brief Reset a per-row join and its child for another executor pass. */
export void bioseq_per_row_join_rescan_custom_scan(CustomScanState* node);

/** @brief Render per-row join planning and execution properties for EXPLAIN. */
export void bioseq_per_row_join_explain_custom_scan(CustomScanState* node,
                                                    List* ancestors,
                                                    ExplainState* es);

/** @brief Detect whether a shared BatchJoin state carries the per-row recipe. */
export [[nodiscard]] bool
bioseq_batch_join_is_post_join_recipe(CustomScanState const* node) noexcept;

/** @brief Initialize a batched join and both of its executor children. */
export void bioseq_batch_join_begin_custom_scan(CustomScanState* node,
                                                EState* estate,
                                                int eflags);

/** @brief Produce the next batched-join tuple without the per-row dispatch shim. */
export TupleTableSlot* bioseq_batch_join_exec_custom_scan_impl(CustomScanState* node);

/** @brief Produce the next tuple from either shared BatchJoin execution recipe. */
export TupleTableSlot* bioseq_batch_join_exec_custom_scan(CustomScanState* node);

/** @brief Release a batched join's executor resources. */
export void bioseq_batch_join_end_custom_scan(CustomScanState* node);

/** @brief Reset a batched join and its children for another executor pass. */
export void bioseq_batch_join_rescan_custom_scan(CustomScanState* node);

/** @brief Render batched-join planning and runtime properties for EXPLAIN. */
export void bioseq_batch_join_explain_custom_scan(CustomScanState* node,
                                                  List* ancestors,
                                                  ExplainState* es);

/** @brief Returns the stable executor method table for base Bioseq scans. */
export [[nodiscard]] const CustomExecMethods* bioseq_column_scan_execute_methods() noexcept;

/** @brief Returns the stable executor method table for batched Bioseq joins. */
export [[nodiscard]] const CustomExecMethods* bioseq_batch_join_execute_methods() noexcept;
