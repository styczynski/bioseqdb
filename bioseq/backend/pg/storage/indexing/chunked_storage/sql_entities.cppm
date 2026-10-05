/**
 * @file sql_entities.cppm
 * @brief SQL sequence used to assign minimizer index, geneation IDs.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.indexing.chunked_storage.sql_entities;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::indexing::sql_entities {

[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]] [[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Assign a new ID when a minimizer index generation is built.
 *
 * The sequence stops at its maximum value
 *
 * ```sql
 * -- Inspect sequence bounds and the last allocated value without consuming an identifier.
 * SELECT sequencename, start_value, min_value, max_value, last_value
 * FROM pg_catalog.pg_sequences
 * WHERE schemaname = 'bioseq_internal'
 *   AND sequencename = 'minimizer_generation_id_seq';
 * ```
 */
inline constexpr std::int64_t minimizer_generation_id_seq = 1;

} // namespace bioseq::storage::indexing::sql_entities
