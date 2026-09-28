/**
 * @file admin/audit_api.cppm
 * @brief Commands for inspecting stored indexes and changing index policies.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>
#include <string_view>

export module bioseq.postgres.storage.indexing.admin:audit_api;

export import :hash_major;

export namespace bioseq::storage::indexing {

/** Check a published grouped-SoA generation and return its source and metadata. */
[[nodiscard]] ready_minimizer_generation_inspection
inspect_ready_minimizer_generation_impl(Oid rel_oid,
                                        std::string_view index_name,
                                        std::uint64_t sequence_rowno);

/** Count persisted rows across the physical shards of one logical index. */
[[nodiscard]] std::int64_t index_table_row_count_impl(Oid rel_oid, std::string_view name);

/** Return source and index counts so a caller can check that no index data remains. */
[[nodiscard]] cold_no_index_absence_audit
cold_no_index_absence_audit_impl(Oid rel_oid, std::string_view name);

/** Drop all physical index tables, reporting failures after C++ cleanup. */
void drop_all_index_tables_impl();

/** Enable a registered index and configure insertion-time maintenance. */
[[nodiscard]] int
set_index_policy_impl(Oid rel_oid, std::string_view name, bool update_on_insert);

/** Select the active read index for every sequence column in a relation. */
[[nodiscard]] int set_active_read_index_impl(Oid rel_oid, std::string_view name);

} // namespace bioseq::storage::indexing
