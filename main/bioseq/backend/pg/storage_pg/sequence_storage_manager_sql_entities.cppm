module;

#include <cstdint>

export module bioseq.storage_manager.sequence_storage_manager;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::sql_entities {

[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Extension-owned noncycling allocator for immutable sequence-source epochs.
 */
inline constexpr std::int64_t sequence_source_epoch_seq = 1;

} // namespace bioseq::storage::sql_entities
