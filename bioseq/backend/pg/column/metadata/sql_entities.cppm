/**
 * @file sql_entities.cppm
 * @brief Declare SQL sequences that allocate columnar metadata ids.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.metadata.sql_entities;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::metadata::sql_entities {

/// @brief Allocates identifiers for persisted row-mask records.
///
/// ```sql
/// -- Inspect sequence bounds and the last allocated value without consuming an identifier.
/// SELECT sequencename, start_value, min_value, max_value, last_value
/// FROM pg_catalog.pg_sequences
/// WHERE schemaname = 'bioseq_internal'
///   AND sequencename = 'row_mask_seq';
/// ```
[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
inline constexpr std::int64_t row_mask_seq = 1;

/// @brief Allocate storage ids starting at 10,000,000,000.
///
/// ```sql
/// -- Inspect sequence bounds and the last allocated value without consuming an identifier.
/// SELECT sequencename, start_value, min_value, max_value, last_value
/// FROM pg_catalog.pg_sequences
/// WHERE schemaname = 'bioseq_internal'
///   AND sequencename = 'storageid_seq';
/// ```
[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::if_not_exists,
    .minimum = 10000000000,
})]]
inline constexpr std::int64_t storageid_seq = 10000000000;

} // namespace bioseq::storage::metadata::sql_entities
