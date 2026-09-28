/**
 * @file
 * @brief Declares symbol_count result lookup for a stored row.
 */
module;
#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.table:custom_scan_scan_runtime_symbol_count_materialization_contracts;

import :custom_scan_runtime_placeholder_contracts;

/** @brief Fills symbol_count results from stored counts or by reading the sequence. */
export void populate_symbol_count_placeholders(placeholder_constants& consts,
                                               Relation main,
                                               std::int64_t rowno,
                                               Snapshot registered_snapshot,
                                               bool read_sequence_index);
