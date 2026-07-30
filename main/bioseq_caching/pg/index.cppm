/*-------------------------------------------------------------------------
 *
 * Thin primary interface for the bioseq_cache PostgreSQL adapter.
 *
 * The primary exports only the typed SQL backend surface. PostgreSQL hook,
 * planner, executor, provider, and GUC ownership remains in named modules.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"
#include "fmgr.h"

PG_MODULE_MAGIC;
}

#include <cstdint>
#include <optional>
#include <string>

export module bioseq.cache.pg;

import bioseq.cache.pg.executor;
import bioseq.cache.pg.guc;
import bioseq.cache.pg.planner;
import bioseq.cache.pg.provider;

export import bioseq.cache.pg.sql.rows;
export import bioseq.pg_sql_boundary;

namespace bioseq::cache::pg::detail {
void initialize_extension();
}

export namespace bioseq::cache::pg::sql_backend {

[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::stats_row>
stats(const bioseq::pg_utils::sql_call_context& call);

[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::cache_entry_row>
entries(const bioseq::pg_utils::sql_call_context& call);

[[nodiscard]] std::int64_t invalidate_all();
[[nodiscard]] std::int64_t invalidate_relation(std::uint32_t relation_oid);
void reset_stats();

} // namespace bioseq::cache::pg::sql_backend

#if defined(BIOSEQ_CACHE_TESTING)
export namespace bioseq::cache::pg::test_sql_backend {

void arm_pause(const std::string& name);
void arm_backend_pause(const std::string& name, std::int32_t target_pid);
void wait_until_paused(const std::string& name);
void wait_until_backend_paused(
    const std::string& name, std::int32_t target_pid);
void release_pause(const std::string& name);
void release_backend_pause(
    const std::string& name, std::int32_t target_pid);
void fail_next_write(const std::string& failure);
void set_force_file_tier(bool enabled);

[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::storage_tier_stats_row>
storage_tier_stats();
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::entry_state_row>
entry_states();
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::runtime_counters_row>
runtime_counters();
[[nodiscard]] bool cache_infrastructure_ready();
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::transition_state_row>
transition_state();
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::relation_transition_state_row>
relation_transition_state(std::uint32_t relation_oid);
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::planner_reference_counts_row>
planner_reference_counts();
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::provider_callback_stats_row>
provider_callback_stats();
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::plan_build_stats_row>
plan_build_stats();

void set_exact_plan_fault(const std::string& name);
void set_exact_recipe_fault(const std::string& name);
void set_coverage_descriptor_fault(const std::string& name);
void set_mapping_fault(const std::string& name);
void clear_mapping_fault();
void set_runtime_digest_fault(const std::string& name);
void clear_runtime_digest_fault();
void set_forced_coverage_domain_digest(
    const bioseq::pg_utils::sql_bytea& value);
void clear_forced_coverage_domain_digest();
[[nodiscard]] std::int64_t inject_complete_coverage(
    const bioseq::pg_utils::sql_bytea& domain_key,
    const bioseq::pg_utils::sql_bytea& predicate_key,
    const bioseq::pg_utils::sql_bytea& provider_security_digest,
    const std::optional<bioseq::pg_utils::sql_bytea>& forced_domain_digest);
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::injected_coverage_row>
lookup_injected_coverage(const bioseq::pg_utils::sql_bytea& domain_key);
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::coverage_bucket_state_row>
coverage_bucket_states();
void mutate_coverage_descriptor(
    std::int64_t generation, const std::string& mutation);
void mark_generation_evictable(std::int64_t generation);
void request_janitor_sweep();
[[nodiscard]] bool wait_for_generation_absent(
    std::int64_t generation,
    const bioseq::pg_utils::sql_interval& timeout);
void direct_heap_insert(
    std::uint32_t relation_oid,
    std::int32_t identifier,
    const std::string& value);

} // namespace bioseq::cache::pg::test_sql_backend
#endif

extern "C" void _PG_init() {
    bioseq::cache::pg::detail::initialize_extension();
}
