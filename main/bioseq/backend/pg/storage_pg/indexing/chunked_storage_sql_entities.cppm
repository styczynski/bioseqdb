module;

#include <cstdint>

export module bioseq.storage_indexing_pg.chunked_storage;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::indexing::sql_entities {

[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Noncycling allocator for immutable minimizer generation identifiers.
 */
inline constexpr std::int64_t minimizer_generation_id_seq = 1;

} // namespace bioseq::storage::indexing::sql_entities
