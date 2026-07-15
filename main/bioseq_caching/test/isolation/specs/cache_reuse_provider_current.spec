# Generic reusable-current coalescing proof.  The reusable executor must invoke
# the existing after_building_publish pause immediately after its current
# generation enters BUILDING; keep this spec unscheduled until that hook and
# reusable generation diagnostics are live.
setup
{
  DO $$
  BEGIN
    PERFORM bioseq_cache.invalidate_all();
    PERFORM bioseq_cache_test.request_janitor_sweep();
    IF EXISTS (SELECT 1 FROM bioseq_cache.entries()) THEN
      RAISE EXCEPTION 'bioseq_cache unified registry did not drain';
    END IF;
  END
  $$;
  CREATE TABLE cache_provider_iso_current(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
  );
  INSERT INTO cache_provider_iso_current VALUES
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
  DROP TABLE cache_provider_iso_current;
}

session "controller"
step "current_arm" {
  SELECT bioseq_cache_test.arm_pause('after_building_publish');
}
step "current_arm_waiter" {
  SELECT bioseq_cache_test.arm_pause('after_waiter_registered');
}
step "current_producer_paused" {
  SELECT bioseq_cache_test.wait_until_paused('after_building_publish');
}
step "current_waiter_paused" {
  SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered');
}
step "current_building" {
  SELECT count(*) AS reusable_building, count(*) = 1 AS exactly_one
  FROM bioseq_cache.entries()
  WHERE kind = 'ReusableCoverage' AND state = 'BUILDING';
}
step "current_release_producer" {
  SELECT bioseq_cache_test.release_pause('after_building_publish');
}
step "current_release_waiter" {
  SELECT bioseq_cache_test.release_pause('after_waiter_registered');
}
step "current_stats" {
  SELECT produced, reuse_hits, fallbacks,
         produced = 1 AND reuse_hits = 1 AND fallbacks = 0 AS exact_counts
  FROM bioseq_cache.stats();
}

session "producer"
step "current_producer_init" {
  SELECT bioseq_cache_test.provider_init();
  SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_current');
  SELECT bioseq_cache_test.provider_reset();
}
step "current_producer_mode" {
  SET bioseq.query_cache_optimisation_mode = 'reuse';
  SET bioseq.query_cache_ttl = '60 min';
}
step "current_produce" {
  SELECT row_id, base_id, occurrence_no, nullable_value, payload,
         capability_mask
  FROM cache_provider_iso_current
  WHERE nullable_value <= 10
  ORDER BY row_id;
}

session "waiter"
step "current_waiter_init" {
  SELECT bioseq_cache_test.provider_init();
  SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_current');
  SELECT bioseq_cache_test.provider_reset();
}
step "current_waiter_mode" {
  SET bioseq.query_cache_optimisation_mode = 'reuse';
  SET bioseq.query_cache_ttl = '60 min';
}
step "current_wait" {
  SELECT row_id, base_id, occurrence_no, nullable_value, payload,
         capability_mask
  FROM cache_provider_iso_current
  WHERE nullable_value <= 10
  ORDER BY row_id;
}

permutation "current_arm" "current_arm_waiter"
  "current_producer_init" "current_producer_mode" "current_produce"
  "current_producer_paused"
  "current_waiter_init" "current_waiter_mode" "current_wait"
  "current_waiter_paused" "current_building"
  "current_release_producer" "current_release_waiter" "current_stats"
