/**
 * @file index_maintenance.cppm
 * @brief Update configured indexes after a streamed table load.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.storage.io.index_maintenance;

export namespace bioseq::postgres::storage::io {

/** Rebuild enabled indexes configured for update on insert, and return the total rows reported. */
[[nodiscard]] std::int64_t maintain_insert_policy_indexes(Oid main_relation_oid);

} // namespace bioseq::postgres::storage::io
