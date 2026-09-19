/**
 * @file
 * @brief Declares batched-join resource lifecycle without importing its implementation.
 */
module;
struct CustomScanState;
struct TupleTableSlot;

export module bioseq.postgres.table:custom_scan_batch_join_execution_resource_lifecycle_contracts;

import std;
import :custom_scan_batch_join_execution_runtime_state_contracts;
export import :custom_scan_batch_join_execution_parent_tuple_contracts;

/** @brief Returns the C++ BatchJoin runtime stored behind a custom scan state. */
export [[nodiscard]] batch_join_runtime*
try_get_batch_join_runtime(CustomScanState* node) noexcept;

/** @brief Clears parent-visible tuple references and forgets the active target slot. */
export void batch_join_clear_parent_tuple_references(CustomScanState* node,
                                                      batch_join_runtime& runtime);

/** @brief Performs ordered BatchJoin executor cleanup. */
export void batch_join_release_executor_resources_checked(CustomScanState* node,
                                                           batch_join_runtime& runtime,
                                                           bool delete_query_context);

/** @brief Marks executor resources abandoned after PostgreSQL error unwinding. */
export void batch_join_abandon_executor_resources_after_error(
    CustomScanState* node,
    batch_join_runtime& runtime) noexcept;

/** @brief Destroys the C++ runtime from its query-memory-context reset callback. */
export void batch_join_runtime_cleanup(void* argument);

/** @brief Releases materialized query rows before resetting their memory context. */
export void batch_join_clear_query_tuples(batch_join_runtime& runtime, bool drop_slots);

/** @brief Validates the physical slot returned by a BatchJoin child plan. */
export void batch_join_validate_returned_child_slot(
    TupleTableSlot* slot,
    int expected_columns,
    std::int16_t sequence_resno,
    std::int16_t rowno_resno,
    std::uint8_t alphabet_code,
    std::uint8_t storage_unit_byte_width,
    const char* role);

/** @brief Raises a materialization failure through the PostgreSQL-to-C++ bridge. */
export [[noreturn]] void batch_join_raise_materialization_as_cpp(
    int sqlstate,
    const char* message,
    std::string detail = {});
