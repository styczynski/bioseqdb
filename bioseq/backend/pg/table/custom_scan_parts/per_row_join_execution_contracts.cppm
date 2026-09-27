/**
 * @file
 * @brief Declares execution and cleanup for mmsearch over joined-row expressions.
 */
module;
struct CustomScanState;
struct EState;
struct TupleTableSlot;
struct ScanState;
struct List;
struct ExplainState;

export module bioseq.postgres.table:custom_scan_per_row_join_execution_contracts;

export import :custom_scan_execution_method_contracts;

/** @brief Holds the child plan and buffered alignment state of a per-row join. */
export struct per_row_join_runtime;

/** @brief Returns the per-row C++ runtime stored behind a custom scan state. */
export [[nodiscard]] per_row_join_runtime* try_get_per_row_join_runtime(
    CustomScanState* node) noexcept;

/** @brief Abandons buffered work and forgets PostgreSQL resources after an error. */
export void per_row_join_abandon_after_error(
    CustomScanState* node,
    per_row_join_runtime& runtime) noexcept;

/** @brief Destroys the per-row C++ runtime when its query memory context is reset. */
export void per_row_join_runtime_cleanup(void* argument);

/** @brief Releases buffered sequence references before ending the child and closing the relation. */
export void per_row_join_release_checked(
    CustomScanState* node,
    per_row_join_runtime& runtime);

/** @brief Checks the saved plan and initializes a per-row alignment buffer and its child. */
export void bioseq_per_row_join_begin_custom_scan_impl(
    CustomScanState* node,
    EState* estate,
    int eflags);

/** @brief Feeds joined rows into alignment and returns the next match tuple. */
export TupleTableSlot* bioseq_per_row_join_exec_custom_scan_impl(CustomScanState* node);

/** @brief Supplies the next aligned row to PostgreSQL's ExecScan. */
export [[nodiscard]] TupleTableSlot* bioseq_per_row_join_access(ScanState* scan);

/** @brief Aligns the child's replacement row during an EvalPlanQual recheck. */
export [[nodiscard]] bool bioseq_per_row_join_recheck(
    ScanState* scan,
    TupleTableSlot* slot);
