/**
 * @file
 * @brief Declares scan-state creation and parameter binding without importing implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_runtime_parameter_binding_contracts;

import std;

/** @brief Mutable base-scan runtime state defined by the scan-runtime contract. */
export struct bioseq_scan_runtime;

/** @brief Allocate a base-scan state while deferring query-owned runtime construction. */
export Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan);

/** @brief Evaluate every Param in an expression against the supplied executor context. */
export [[nodiscard]] Node* eval_params(Node* node, ExprContext* econtext);

/** @brief Distinguishes a configured query from SQL NULL during parameter rebinding. */
export enum class mmsearch_parameter_resolution : std::uint8_t {
    configured,
    null_value,
};

/** @brief Resolve and validate the Param registered as the primary mmsearch query. */
export [[nodiscard]] mmsearch_parameter_resolution resolve_mmsearch_param_query(
    bioseq_scan_runtime& runtime,
    ExprContext* econtext,
    const char* stage);

/** @brief Raise a Vulkan execution failure through the PostgreSQL-to-C++ bridge. */
export [[noreturn]] void raise_column_scan_vulkan_error_as_cpp(std::string_view detail,
                                                               std::string_view hint);
