/**
 * @file admin/audit_api.cppm
 * @brief Declares the public PostgreSQL index audit and policy commands.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.indexing.admin:audit_api;

import std;
export import :hash_major;

export namespace bioseq::storage::indexing {

/** Inspect and validate one published minimizer generation. */
[[nodiscard]] ready_minimizer_generation_inspection
inspect_ready_minimizer_generation_impl(Oid rel_oid,
                                        std::string_view index_name,
                                        std::uint64_t sequence_rowno);

/** Count persisted rows across the physical shards of one logical index. */
[[nodiscard]] std::int64_t index_table_row_count_impl(Oid rel_oid, std::string_view name);

/** Validate that a no-index policy has no persisted payload. */
[[nodiscard]] cold_no_index_absence_audit
cold_no_index_absence_audit_impl(Oid rel_oid, std::string_view name);

/** Drop every physical index relation through the shared cleanup boundary. */
void drop_all_index_tables_impl();

/** Enable a registered index and configure insertion-time maintenance. */
[[nodiscard]] int
set_index_policy_impl(Oid rel_oid, std::string_view name, bool update_on_insert);

/** Select the active read index for every sequence column in a relation. */
[[nodiscard]] int set_active_read_index_impl(Oid rel_oid, std::string_view name);

} // namespace bioseq::storage::indexing
