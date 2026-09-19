/**
 * @file
 * @brief Declares transformed per-row join execution without importing its state machine.
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

/** @brief Per-row runtime state defined by the dedicated runtime contract. */
export struct per_row_join_runtime;

/** @brief Returns the per-row C++ runtime stored behind a custom scan state. */
export [[nodiscard]] per_row_join_runtime* try_get_per_row_join_runtime(
    CustomScanState* node) noexcept;

/** @brief Marks per-row resources abandoned after PostgreSQL error unwinding. */
export void per_row_join_abandon_after_error(
    CustomScanState* node,
    per_row_join_runtime& runtime) noexcept;

/** @brief Destroys the per-row runtime from its query-memory-context callback. */
export void per_row_join_runtime_cleanup(void* argument);

/** @brief Drains asynchronous ownership and releases child and relation resources in order. */
export void per_row_join_release_checked(
    CustomScanState* node,
    per_row_join_runtime& runtime);

/** @brief Validates the serialized per-row recipe and constructs its query-owned runtime. */
export void bioseq_per_row_join_begin_custom_scan_impl(
    CustomScanState* node,
    EState* estate,
    int eflags);

/** @brief Produce the next tuple from the initialized per-row state machine. */
export TupleTableSlot* bioseq_per_row_join_exec_custom_scan_impl(CustomScanState* node);

/** @brief Fetch callback used by PostgreSQL's ExecScan adapter. */
export [[nodiscard]] TupleTableSlot* bioseq_per_row_join_access(ScanState* scan);

/** @brief Recheck callback for already-produced per-row tuples. */
export [[nodiscard]] bool bioseq_per_row_join_recheck(
    ScanState* scan,
    TupleTableSlot* slot);
