# Generic provider fixture for DOC0371 42.11 and 42.13.  Two projected-only
# sources overlap in one reusable domain while terminal publication is paused.
# The consumer must preserve NULL/UNKNOWN semantics, deduplicate overlap, late
# fetch predicate-only attributes, and finish without PostgreSQL deadlock.
setup
{
  CREATE TABLE cache_provider_iso_sources(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL
  );
  INSERT INTO cache_provider_iso_sources VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0, 'zero', 3),
    (3, 12, 0, 5, 'match-a', 3),
    (4, 12, 1, 5, 'match-b', 3),
    (5, 13, 0, 10, 'ten', 3),
    (6, 14, 0, 20, 'twenty', 3);
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache_test.request_janitor_sweep();
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE cache_provider_iso_sources;
}

session "controller"
step "sources_arm" {
  SELECT bioseq_cache_test.arm_pause('after_building_publish');
  SELECT bioseq_cache_test.arm_pause('after_waiter_registered');
}
step "sources_a_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "sources_c_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "sources_state" {
  SELECT count(*) AS building, count(*) = 2 AS exactly_two
  FROM bioseq_cache.entries()
  WHERE kind = 'ReusableCoverage' AND state = 'BUILDING';
}
step "sources_release_builders" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "sources_release_consumer" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }

session "source_a"
step "sources_a_init" {
  SELECT bioseq_cache_test.provider_init();
  SELECT bioseq_cache_test.provider_reset();
}
step "sources_a_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_sources'); }
step "sources_a_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "sources_a_prepare" {
  SET plan_cache_mode = force_generic_plan;
  PREPARE cache_provider_sources_a AS
  SELECT row_id
  FROM cache_provider_iso_sources
  WHERE nullable_value <= 10
  ORDER BY row_id;
}
step "sources_a_freeze" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_sources_a', 1, 0);
}
step "sources_a_query" {
  EXECUTE cache_provider_sources_a;
}

session "source_b"
step "sources_b_init" {
  SELECT bioseq_cache_test.provider_init();
  SELECT bioseq_cache_test.provider_reset();
}
step "sources_b_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_sources'); }
step "sources_b_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "sources_b_prepare" {
  SET plan_cache_mode = force_generic_plan;
  PREPARE cache_provider_sources_b AS
  SELECT row_id
  FROM cache_provider_iso_sources
  WHERE nullable_value >= 5
  ORDER BY row_id;
}
step "sources_b_freeze" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_sources_b', 1, 0);
}
step "sources_b_query" {
  EXECUTE cache_provider_sources_b;
}

session "consumer"
step "sources_c_init" {
  SELECT bioseq_cache_test.provider_init();
  SELECT bioseq_cache_test.provider_reset();
}
step "sources_c_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_sources'); }
step "sources_c_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "sources_c_query" {
  SELECT row_id
  FROM cache_provider_iso_sources
  WHERE nullable_value IS NULL OR nullable_value BETWEEN 5 AND 20
  ORDER BY row_id;
}
step "sources_c_stats" {
  SELECT
    cache_stats.produced = 3 AS three_completed_producers,
    cache_stats.reuse_hits = 1 AS one_two_source_reuse,
    cache_stats.inflight_waits = 2 AS waited_for_both_older_claims,
    cache_stats.fallbacks = 0 AS no_fallback,
    (stats->>'late_fetch_batch')::bigint = 2 AS two_late_fetch_batches,
    (stats->>'late_fetch_references')::bigint = 8 AS eight_late_fetch_references,
    (stats->>'late_fetch_values')::bigint = 8 AS eight_late_fetch_values,
    (stats->>'executor_rows')::bigint = 1 AS one_residual_row,
    (stats->>'last_required_boundary_count')::bigint = 1
      AS one_required_boundary_column
  FROM (SELECT bioseq_cache_test.provider_stats() AS stats) AS provider
  CROSS JOIN bioseq_cache.stats() AS cache_stats;
}

permutation "sources_arm"
  "sources_a_init" "sources_a_relation" "sources_a_mode"
  "sources_a_prepare" "sources_a_freeze"
  "sources_b_init" "sources_b_relation" "sources_b_mode"
  "sources_b_prepare" "sources_b_freeze"
  "sources_c_init" "sources_c_relation" "sources_c_mode"
  "sources_a_query" "sources_a_paused"
  "sources_b_query" "sources_c_query" "sources_c_paused"
  "sources_state" "sources_release_builders"
  "sources_release_consumer" "sources_c_stats"
