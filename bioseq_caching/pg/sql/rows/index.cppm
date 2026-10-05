/*
 * C++ rows used by the cache backend and generated SQL result converters.
 * Each row owns its strings and values. Field annotations select SQL types
 * such as OID, JSONB or INTERVAL when the C++ type alone is insufficient.
 */
module;

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

export module bioseq.cache.pg.sql.rows;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.sql_boundary;

export namespace bioseq::cache::pg::sql_rows {

/** One row returned by bioseq_cache.stats(). */
struct stats_row final {
    /// @brief Number of cache generations successfully published since counters were reset.
    std::int64_t produced;
    /// @brief Number of hits on completed exact-result generations.
    std::int64_t completed_hits;
    /// @brief Number of waits for a cache producer that was still running.
    std::int64_t inflight_waits;
    /// @brief Number of reusable-coverage cache hits.
    std::int64_t reuse_hits;
    /// @brief Number of cache fallbacks to ordinary execution.
    std::int64_t fallbacks;
    /// @brief Number of cache invalidations recorded since counters were reset.
    std::int64_t invalidations;
    /// @brief Number of cache evictions recorded since counters were reset.
    std::int64_t evictions;
    /// @brief Cumulative logical payload bytes recorded by cache producers.
    std::int64_t logical_bytes;
    /// @brief Cumulative stored payload bytes recorded by cache producers.
    std::int64_t stored_bytes;
};

/** One exact or reusable generation returned by bioseq_cache.entries(). */
struct cache_entry_row final {
    /// @brief Printable key identifying the cached result or reusable coverage.
    std::string entry_key;
    /// @brief Generation number associated with the cache key.
    std::int64_t generation;
    /// @brief Cache entry kind: ExactFinalResult or ReusableCoverage.
    std::string kind;
    /// @brief Current lifecycle state of the cache generation.
    std::string state;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    /// @brief Database containing the cached work, or NULL when unavailable.
    std::optional<std::uint32_t> database_oid;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::oid,
    })]]
    /// @brief Role that owns the generation; hidden when the caller lacks full disclosure.
    std::optional<std::uint32_t> owner_role;

    /// @brief Producer backend PID; hidden when unavailable or disclosure is restricted.
    std::optional<std::int32_t> producer_pid;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    /// @brief Time the generation started, or NULL when unavailable.
    std::optional<bioseq::postgres::support::timestamp_with_time_zone> created_at;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    /// @brief Time production completed, or NULL while incomplete or unavailable.
    std::optional<bioseq::postgres::support::timestamp_with_time_zone> completed_at;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::interval,
    })]]
    /// @brief Elapsed time since the generation started.
    bioseq::postgres::support::interval_value age;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::interval,
    })]]
    /// @brief Configured lifetime of the generation after completion.
    bioseq::postgres::support::interval_value ttl;

    /// @brief Number of rows stored in the generation.
    std::int64_t rows;
    /// @brief Logical uncompressed payload size of the generation, in bytes.
    std::int64_t logical_bytes;
    /// @brief Physical payload size recorded for the generation, in bytes.
    std::int64_t stored_bytes;
    /// @brief Number of callers waiting for the generation.
    std::int32_t waiters;
    /// @brief Number of active reader pins on the generation.
    std::int32_t readers;
    /// @brief Recorded exact-result hit count; reusable-coverage rows currently report 0.
    std::int64_t hits;
    /// @brief Producer SQL text, or NULL when unavailable or disclosure is restricted.
    std::optional<std::string> sql;
    /// @brief Producer plan text, or NULL when unavailable or disclosure is restricted.
    std::optional<std::string> producer_plan;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    /// @brief JSONB dependency information, or NULL when disclosure is restricted.
    std::optional<bioseq::postgres::support::jsonb_value> dependencies;
};

static_assert(std::is_aggregate_v<stats_row>);
static_assert(std::is_aggregate_v<cache_entry_row>);

#if defined(BIOSEQ_CACHE_TESTING)

/** Test-only counts of cache  storage blocks, reserved bytes and reader pins. */
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

/** Test-only counts and mompletion markers for, transaction changes. */
struct transition_state_row final {
    std::int64_t database_transitions;
    std::int64_t coarse_transitions;
    bool prepared_xacts;
    std::int64_t snapshot_completion;
    std::int64_t global_completion;
};

/** Test-only change count and invalidation epoch for one relation. */
struct relation_transition_state_row final {
    std::int64_t relation_transitions;
    std::int64_t relation_epoch;
};

/** Test-only counts of reader pins, waiters and reservations held during planning. */
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
    bioseq::postgres::support::bytea_value predicate_key;

    std::string state;
};

/** Test-only generations grouped by database, role, security settings and coverage domain. */
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
    bioseq::postgres::support::bytea_value provider_security_digest;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    bioseq::postgres::support::bytea_value coverage_domain_digest;

    std::int32_t generation_count;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bigint[]">,
    })]]
    bioseq::postgres::support::bigint_array generation_ids;
};

static_assert(std::is_aggregate_v<storage_tier_stats_row>);
static_assert(std::is_aggregate_v<entry_state_row>);
static_assert(std::is_aggregate_v<transition_state_row>);
static_assert(std::is_aggregate_v<relation_transition_state_row>);
static_assert(std::is_aggregate_v<planner_reference_counts_row>);
static_assert(std::is_aggregate_v<provider_callback_stats_row>);
static_assert(std::is_aggregate_v<plan_build_stats_row>);
static_assert(std::is_aggregate_v<injected_coverage_row>);
static_assert(std::is_aggregate_v<coverage_bucket_state_row>);

#endif  // defined(BIOSEQ_CACHE_TESTING)

}  // namespace bioseq::cache::pg::sql_rows
