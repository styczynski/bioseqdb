/**
 * @file admin/policy_commands.cppm
 * @brief Applies user-facing PostgreSQL index policy changes.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.indexing.admin:policy_commands;

import std;
import :rebuild_api;
import bioseq.common.logging;
import bioseq.postgres.storage.indexing.index_relation_identity;

export namespace bioseq::storage::indexing {

/**
 * Enable a registered index and select whether inserts maintain it.
 *
 * @return Zero on success, one for an unknown name, or two when the index
 * cannot be maintained during insertion.
 */
[[nodiscard]] int
set_index_policy_impl(Oid rel_oid, std::string_view name, bool update_on_insert) {
    if (!name_is_registered(name))
        return 1;
    if (update_on_insert && !index_allows_update_on_insert(name))
        return 2;
    const relation_identity rid = index_relation_identity(rel_oid);
    if (SPI_connect() != SPI_OK_CONNECT)
        return 0;
    std::string sql =
        fmt::format("INSERT INTO bioseq_internal.index_policy (user_relid, main_relfilenode, "
                    "column_attno, index_name, enabled, update_on_insert) "
                    "SELECT {}::oid, {}::oid, a.attnum, {}, true, {} FROM pg_attribute a JOIN "
                    "pg_type t ON a.atttypid=t.oid "
                    "JOIN pg_namespace n ON t.typnamespace=n.oid WHERE a.attrelid={}::oid AND "
                    "t.typname='seq' AND n.nspname IN ('bioseq','bioseq_internal') "
                    "AND a.attnum>0 AND NOT a.attisdropped "
                    "ON CONFLICT (user_relid, main_relfilenode, column_attno, index_name) DO "
                    "UPDATE SET update_on_insert=EXCLUDED.update_on_insert, enabled=true",
                    static_cast<unsigned>(rel_oid),
                    static_cast<unsigned>(rid.relfilenode),
                    quote_literal_cstr(std::string(name).c_str()),
                    update_on_insert ? "true" : "false",
                    static_cast<unsigned>(rel_oid));
    SPI_execute(sql.c_str(), false, 0);
    SPI_finish();
    return 0;
}

} // namespace bioseq::storage::indexing
