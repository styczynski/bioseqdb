/**
 * @file
 * @brief Declares symbol_count result lookup for a stored row.
 */
module;
#include <cstdint>

// Relation and Snapshot are PostgreSQL aliases for pointers to these global tags.
struct RelationData;
struct SnapshotData;

export module bioseq.postgres.table:custom_scan_scan_runtime_symbol_count_materialization_contracts;

import :custom_scan_runtime_placeholder_contracts;

/** @brief Fills symbol_count results from stored counts or by reading the sequence. */
export void populate_symbol_count_placeholders(placeholder_constants& consts,
                                               RelationData* main,
                                               std::int64_t rowno,
                                               SnapshotData* registered_snapshot,
                                               bool read_sequence_index);
