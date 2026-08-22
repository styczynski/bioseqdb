/**
 * @file index_maintenance.cppm
 * @brief Provides the narrow index-maintenance boundary used by table loads.
 */
module;

#include <bioseq/pg_compat.h>
#include <cstdint>

export module bioseq.postgres.storage.io.index_maintenance;

export namespace bioseq::postgres::storage::io {

/** Rebuild indexes configured for maintenance after a streamed table load. */
[[nodiscard]] std::int64_t maintain_insert_policy_indexes(Oid main_relation_oid);

} // namespace bioseq::postgres::storage::io
