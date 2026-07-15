# Generic provider fixture for DOC0371 42.12/42.12.1.  A frozen generic plan
# is executed after the provider runtime digest changes and after provider
# unregistration.  Both permutations must leave gates false and execute the
# retained provider child exactly once.
setup
{
  CREATE TABLE cache_provider_iso_validity(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL
  );
  INSERT INTO cache_provider_iso_validity VALUES
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
  DROP TABLE cache_provider_iso_validity;
}

session "validity"
step "validity_init" { SELECT bioseq_cache_test.provider_init(); }
step "validity_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_validity'); }
step "validity_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "validity_generic" { SET plan_cache_mode = force_generic_plan; }
step "validity_prepare_source" {
  PREPARE cache_provider_validity_source AS
  SELECT row_id
  FROM cache_provider_iso_validity
  WHERE nullable_value <= 10
  ORDER BY row_id;
}
step "validity_freeze_source" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_validity_source', 1, 0);
}
step "validity_source" {
  EXECUTE cache_provider_validity_source;
}
step "validity_source_state" {
  SELECT count(*) = 1 AS one_complete_source
  FROM bioseq_cache.entries()
  WHERE kind = 'ReusableCoverage' AND state = 'COMPLETE';
}
step "validity_reset_counters" {
  SELECT bioseq_cache_test.provider_reset();
  SELECT bioseq_cache.reset_stats();
}
step "validity_prepare" {
  PREPARE cache_provider_validity AS
  SELECT row_id
  FROM cache_provider_iso_validity
  WHERE nullable_value >= 5
  ORDER BY row_id;
}
step "validity_freeze" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_validity', 1, 1);
}
step "validity_digest_fault" { SELECT bioseq_cache_test.provider_set_digest_fault('runtime_mismatch'); }
step "validity_digest_execute" { EXECUTE cache_provider_validity; }
step "validity_digest_stats" {
  SELECT
    (stats->>'executor_rows')::bigint = 4 AS retained_child_once,
    (stats->>'late_fetch_batch')::bigint = 0 AS no_cached_branch,
    (stats->>'late_fetch_references')::bigint = 0 AS no_cached_references,
    cache_stats.fallbacks = 1 AS one_fail_open_fallback
  FROM (SELECT bioseq_cache_test.provider_stats() AS stats) AS provider
  CROSS JOIN bioseq_cache.stats() AS cache_stats;
}
step "validity_digest_references" {
  SELECT coalesce(sum(waiters), 0) = 0 AS no_waiters,
         coalesce(sum(readers), 0) = 0 AS no_readers,
         coalesce(sum((dependencies->>'planner_pins')::bigint), 0) = 0
           AS no_planner_pins
  FROM bioseq_cache.entries()
  WHERE kind = 'ReusableCoverage';
}
step "validity_digest_clear" { SELECT bioseq_cache_test.provider_set_digest_fault('none'); }
step "validity_shutdown" { SELECT bioseq_cache_test.provider_shutdown(); }
step "validity_unregistered_execute" { EXECUTE cache_provider_validity; }
step "validity_unregistered_stats" {
  SELECT
    (stats->>'executor_rows')::bigint = 4 AS retained_child_once,
    (stats->>'late_fetch_batch')::bigint = 0 AS no_cached_branch,
    (stats->>'late_fetch_references')::bigint = 0 AS no_cached_references,
    cache_stats.fallbacks = 1 AS one_fail_open_fallback
  FROM (SELECT bioseq_cache_test.provider_stats() AS stats) AS provider
  CROSS JOIN bioseq_cache.stats() AS cache_stats;
}
step "validity_unregistered_references" {
  SELECT coalesce(sum(waiters), 0) = 0 AS no_waiters,
         coalesce(sum(readers), 0) = 0 AS no_readers,
         coalesce(sum((dependencies->>'planner_pins')::bigint), 0) = 0
           AS no_planner_pins
  FROM bioseq_cache.entries()
  WHERE kind = 'ReusableCoverage';
}
step "validity_deallocate" {
  DEALLOCATE cache_provider_validity_source;
  DEALLOCATE cache_provider_validity;
}

permutation "validity_init" "validity_relation" "validity_mode"
  "validity_generic" "validity_prepare_source" "validity_freeze_source"
  "validity_source" "validity_source_state" "validity_reset_counters"
  "validity_prepare"
  "validity_freeze" "validity_digest_fault" "validity_digest_execute"
  "validity_digest_stats" "validity_digest_references"
  "validity_digest_clear" "validity_deallocate"

permutation "validity_init" "validity_relation" "validity_mode"
  "validity_generic" "validity_prepare_source" "validity_freeze_source"
  "validity_source" "validity_source_state" "validity_reset_counters"
  "validity_prepare"
  "validity_freeze" "validity_shutdown"
  "validity_unregistered_execute" "validity_unregistered_stats"
  "validity_unregistered_references"
  "validity_deallocate"
