/**
 * @file
 * @brief Declares exact per-row symbol-count placeholder materialization.
 */
module;
#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.table:custom_scan_scan_runtime_symbol_count_materialization_contracts;

import :custom_scan_runtime_placeholder_contracts;

/** @brief Populates symbol-count placeholders for one columnar row. */
export void populate_symbol_count_placeholders(placeholder_constants& consts,
                                               Relation main,
                                               std::int64_t rowno,
                                               Snapshot registered_snapshot,
                                               bool read_sequence_index);
