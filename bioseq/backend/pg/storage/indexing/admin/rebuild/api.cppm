/**
 * @file admin/rebuild/api.cppm
 * @brief Declares the stable PostgreSQL index-rebuild entry points.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.indexing.admin:rebuild_api;

import std;
import bioseq.indexing.type;

export namespace bioseq::storage::indexing {

/** Return whether the PostgreSQL registry contains the logical index name. */
[[nodiscard]] bool name_is_registered(std::string_view name);

/** Return whether inserts may maintain the registered logical index. */
[[nodiscard]] bool index_allows_update_on_insert(std::string_view name);

/** Return the physical layout registered for a logical index name. */
[[nodiscard]] bioseq::indexing::sequence_index_layout layout_of_name(std::string_view name);

/** Rebuild a named sequence index and translate failures to PostgreSQL errors. */
[[nodiscard]] std::int64_t
update_index_pg_checked(Oid rel_oid, const char* name_data, std::size_t name_size);

/** Rebuild the enabled update-on-insert indexes after a streamed table load. */
[[nodiscard]] std::int64_t build_policy_update_on_insert_indexes(Oid main_relid);

} // namespace bioseq::storage::indexing
