/*-------------------------------------------------------------------------
 *
 * bioseq_cache generated SQL result rows.
 *
 * These owning rows are shared by the cache PostgreSQL adapter and the
 * annotation-owned SQL catalog.  PostgreSQL-specific spellings which cannot
 * be inferred from the C++ representation are declared on the fields.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

export module bioseq.cache.pg.sql.rows;

import bioseq.codegen.sql.annotation_model;
import bioseq.pg_sql_boundary;

export namespace bioseq::cache::pg::sql_rows {

/** One row returned by bioseq_cache.stats(). */
struct stats_row final {
    std::int64_t produced;
    std::int64_t completed_hits;
    std::int64_t inflight_waits;
    std::int64_t reuse_hits;
    std::int64_t fallbacks;
    std::int64_t invalidations;
    std::int64_t evictions;
    std::int64_t logical_bytes;
    std::int64_t stored_bytes;
};

/** One exact or reusable generation returned by bioseq_cache.entries(). */
struct cache_entry_row final {
    std::string entry_key;
    std::int64_t generation;
    std::string kind;
    std::string state;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    std::optional<std::uint32_t> database_oid;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    std::optional<std::uint32_t> owner_role;

    std::optional<std::int32_t> producer_pid;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::optional<bioseq::pg_utils::sql_timestamptz> created_at;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::optional<bioseq::pg_utils::sql_timestamptz> completed_at;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::interval,
    })]]
    bioseq::pg_utils::sql_interval age;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::interval,
    })]]
    bioseq::pg_utils::sql_interval ttl;

    std::int64_t rows;
    std::int64_t logical_bytes;
    std::int64_t stored_bytes;
    std::int32_t waiters;
    std::int32_t readers;
    std::int64_t hits;
    std::optional<std::string> sql;
    std::optional<std::string> producer_plan;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<bioseq::pg_utils::sql_jsonb> dependencies;
};

static_assert(std::is_aggregate_v<stats_row>);
static_assert(std::is_aggregate_v<cache_entry_row>);

#if defined(BIOSEQ_CACHE_TESTING)

/** Storage-ledger counters exposed only by the test-support profile. */
struct storage_tier_stats_row final {
    bool force_file_tier;
    std::int64_t dsa_blocks;
    std::int64_t file_blocks;
    std::int64_t reserved_hot_bytes;
    std::int64_t reserved_file_bytes;
    bool coherent;
    std::int64_t occupied_blocks;
    std::int64_t reserved_blocks;
    std::int64_t stored_blocks;
    std::int64_t referenced_blocks;
    std::int64_t retiring_blocks;
    std::int64_t release_committed_blocks;
    std::int64_t reader_pins;
};

/** Exact-result generation state exposed only by test support. */
struct entry_state_row final {
    std::string entry_key;
    std::int64_t generation;
    std::string state;
    std::int32_t waiter_count;
    std::int32_t reader_count;
    std::int32_t producer_pid;
    std::int64_t claim_sequence;
};

/** Runtime child-execution counter exposed only by test support. */
struct runtime_counters_row final {
    std::int64_t exact_child_executions;
};

/** Transaction-transition state exposed only by test support. */
struct transition_state_row final {
    std::int64_t database_transitions;
    std::int64_t coarse_transitions;
    bool prepared_xacts;
    std::int64_t snapshot_completion;
    std::int64_t global_completion;
};

/** One relation's precise commit-transition state exposed by test support. */
struct relation_transition_state_row final {
    std::int64_t relation_transitions;
    std::int64_t relation_epoch;
};

/** Planner-held reusable references exposed only by test support. */
struct planner_reference_counts_row final {
    std::int64_t reader_pins;
    std::int64_t waiter_references;
    std::int64_t admission_reservations;
};

/** Per-callback provider counters exposed only by test support. */
struct provider_callback_stats_row final {
    std::string callback_name;
    std::int64_t calls;
    std::int64_t errors;
    std::int64_t unsupported;
};

/** Cache-plan construction counters exposed only by test support. */
struct plan_build_stats_row final {
    std::int64_t planning_generation;
    std::int64_t disabled_plans;
    std::int64_t same_exact_wrappers;
    std::int64_t reuse_exact_wrappers;
    std::int64_t reuse_coverage_nodes;
};

/** One injected coverage generation exposed only by test support. */
struct injected_coverage_row final {
    std::int64_t generation;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    bioseq::pg_utils::sql_bytea predicate_key;

    std::string state;
};

/** One injected security/domain coverage bucket exposed by test support. */
struct coverage_bucket_state_row final {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    std::uint32_t database_oid;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    std::uint32_t current_role_oid;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    std::uint32_t session_user_oid;

    std::int32_t security_context_flags;
    std::int32_t row_security_mode;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    bioseq::pg_utils::sql_bytea provider_security_digest;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    bioseq::pg_utils::sql_bytea coverage_domain_digest;

    std::int32_t generation_count;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bigint[]">,
    })]]
    bioseq::pg_utils::sql_int64_array generation_ids;
};

static_assert(std::is_aggregate_v<storage_tier_stats_row>);
static_assert(std::is_aggregate_v<entry_state_row>);
static_assert(std::is_aggregate_v<runtime_counters_row>);
static_assert(std::is_aggregate_v<transition_state_row>);
static_assert(std::is_aggregate_v<relation_transition_state_row>);
static_assert(std::is_aggregate_v<planner_reference_counts_row>);
static_assert(std::is_aggregate_v<provider_callback_stats_row>);
static_assert(std::is_aggregate_v<plan_build_stats_row>);
static_assert(std::is_aggregate_v<injected_coverage_row>);
static_assert(std::is_aggregate_v<coverage_bucket_state_row>);

#endif  // defined(BIOSEQ_CACHE_TESTING)

}  // namespace bioseq::cache::pg::sql_rows
