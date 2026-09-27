/**
 * @file
 * @brief Resource cleanup and row validation for batched sequence joins.
 */
module;
struct CustomScanState;
struct TupleTableSlot;

export module bioseq.postgres.table:custom_scan_batch_join_execution_resource_lifecycle_contracts;

import std;
import :custom_scan_batch_join_execution_runtime_state_contracts;
export import :custom_scan_batch_join_execution_parent_tuple_contracts;

/** Return the join runtime, or null when the node has none. */
export [[nodiscard]] batch_join_runtime*
try_get_batch_join_runtime(CustomScanState* node) noexcept;

/** Clear output references and forget the current target slot. */
export void batch_join_clear_parent_tuple_references(CustomScanState* node,
                                                      batch_join_runtime& runtime);

/** Drain join work before releasing tuples, reserved memory, and child plans. */
export void batch_join_release_executor_resources_checked(CustomScanState* node,
                                                           batch_join_runtime& runtime,
                                                           bool delete_query_context);

/** Forget PostgreSQL resources after an error and release the remaining C++ state. */
export void batch_join_abandon_executor_resources_after_error(
    CustomScanState* node,
    batch_join_runtime& runtime) noexcept;

/** @brief Destroys the C++ runtime from its query-memory-context reset callback. */
export void batch_join_runtime_cleanup(void* argument);

/** Clear saved query rows and reset their memory context. */
export void batch_join_clear_query_tuples(batch_join_runtime& runtime, bool drop_slots);

/** Check the layout and sequence type of the slot returned by a join child. */
export void batch_join_validate_returned_child_slot(
    TupleTableSlot* slot,
    int expected_columns,
    std::int16_t sequence_resno,
    std::int16_t rowno_resno,
    std::uint8_t alphabet_code,
    std::uint8_t storage_unit_byte_width,
    const char* role);

/** Raise a materialization error as a C++ exception so local objects are cleaned up. */
export [[noreturn]] void batch_join_raise_materialization_as_cpp(
    int sqlstate,
    const char* message,
    std::string detail = {});
