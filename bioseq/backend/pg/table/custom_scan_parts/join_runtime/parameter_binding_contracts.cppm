/**
 * @file
 * @brief Declares BioseqScan allocation and parameter binding.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_runtime_parameter_binding_contracts;

import std;

/** @brief The C++ state used while a BioseqScan runs. */
export struct bioseq_scan_runtime;

/** @brief Allocates the BioseqScan node and leaves its C++ runtime for BeginCustomScan. */
export Node* bioseq_column_scan_create_custom_scan_state(CustomScan* cscan);

/** @brief Evaluate every Param in an expression against the supplied executor context. */
export [[nodiscard]] Node* eval_params(Node* node, ExprContext* econtext);

/** @brief Whether the query parameter configured a search or resolved to SQL NULL. */
export enum class mmsearch_parameter_resolution : std::uint8_t {
    configured,
    null_value,
};

/** @brief Reads and checks the current query parameter, then configures or clears the search. */
export [[nodiscard]] mmsearch_parameter_resolution resolve_mmsearch_param_query(
    bioseq_scan_runtime& runtime,
    ExprContext* econtext,
    const char* stage);

/** @brief Reports a Vulkan failure as a PostgreSQL error carried through the C++ exception bridge. */
export [[noreturn]] void raise_column_scan_vulkan_error_as_cpp(std::string_view detail,
                                                               std::string_view hint);
