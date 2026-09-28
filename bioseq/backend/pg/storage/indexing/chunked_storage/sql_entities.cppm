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
 */
inline constexpr std::int64_t minimizer_generation_id_seq = 1;

} // namespace bioseq::storage::indexing::sql_entities
