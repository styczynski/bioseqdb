/**
 * @file
 * @brief PostgreSQL execution callbacks for Bioseq scans and sequence joins.
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

/** Allocate executor state  for a planned sequence join. */
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

/** Show Bioseq scan settings and execution statistics in EXPLAIN. */
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

/** Show per-row join settings and execution statistics in EXPLAIN. */
export void bioseq_per_row_join_explain_custom_scan(CustomScanState* node,
                                                    List* ancestors,
                                                    ExplainState* es);

/** Check whether a shared join state contains the one-child per-row, plan. */
export [[nodiscard]] bool
bioseq_batch_join_is_post_join_recipe(CustomScanState const* node) noexcept;

/** Validate and initialize a batched or per-row sequence join. */
export void bioseq_batch_join_begin_custom_scan(CustomScanState* node,
                                                EState* estate,
                                                int eflags);

/** Produce the next result of the two-cheld batch join. */
export TupleTableSlot* bioseq_batch_join_exec_custom_scan_impl(CustomScanState* node);

/** Return the next result from a batched or per-row sequence join. */
export TupleTableSlot* bioseq_batch_join_exec_custom_scan(CustomScanState* node);

/** Release resources for a batched or per-row sequence join. */
export void bioseq_batch_join_end_custom_scan(CustomScanState* node);

/** Restart  a sequence join and its child plans. */
export void bioseq_batch_join_rescan_custom_scan(CustomScanState* node);

/** Show sequence-join settings and execution statistics in EXPLAIN. */
export void bioseq_batch_join_explain_custom_scan(CustomScanState* node,
                                                  List* ancestors,
                                                  ExplainState* es);

/** Return PostgreSQL's execution callbacks for Bioseq table scans. */
export [[nodiscard]] const CustomExecMethods* bioseq_column_scan_execute_methods() noexcept;

/** Return PostgreSQL's shared execution callbacks for sequence joins. */
export [[nodiscard]] const CustomExecMethods* bioseq_batch_join_execute_methods() noexcept;
