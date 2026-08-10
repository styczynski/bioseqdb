module;

#include <cstdint>

export module bioseq.postgres.storage.metadata.sql_entities;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::metadata::sql_entities {

[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
inline constexpr std::int64_t row_mask_seq = 1;

[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::if_not_exists,
    .minimum = 10000000000,
})]]
inline constexpr std::int64_t storageid_seq = 10000000000;

} // namespace bioseq::storage::metadata::sql_entities
