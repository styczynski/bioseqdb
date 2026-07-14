# Canceling the elected producer fails its BUILDING generation, wakes the
# waiter to a source fallback, and leaves the key available for a fresh claim.
setup
{
  CREATE TABLE qc_producer_cancel(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_producer_cancel
  SELECT i, md5(i::text) FROM generate_series(1, 100) AS g(i);
  DO $$
  BEGIN
    PERFORM bioseq_cache.invalidate_all();
    PERFORM bioseq_cache.reset_stats();
  END
  $$;
}

teardown
{
  DROP TABLE qc_producer_cancel;
}

session "controller"
step "pcancel_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "pcancel_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "pcancel_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "pcancel_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "pcancel_backend" {
  SELECT pg_cancel_backend(producer_pid)
  FROM bioseq_cache_test.entry_states()
  WHERE state = 'BUILDING';
}
step "pcancel_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "pcancel_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "pcancel_state" {
  SELECT state, waiter_count, reader_count
  FROM bioseq_cache_test.entry_states()
  WHERE state <> 'RETIRED';
}
step "pcancel_stats" {
  SELECT produced, completed_hits, inflight_waits, fallbacks
  FROM bioseq_cache.stats();
}

session "producer"
setup
{
  SET application_name = 'bioseq_cache_cancel_producer';
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "pcancel_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_producer_cancel WHERE id <= 10;
}

session "waiter"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "pcancel_wait" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_producer_cancel WHERE id <= 10;
}

session "retry"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "pcancel_retry" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_producer_cancel WHERE id <= 10;
}

permutation "pcancel_arm_producer" "pcancel_arm_waiter" "pcancel_produce"
  "pcancel_producer_paused" "pcancel_wait" "pcancel_waiter_paused"
  "pcancel_backend" "pcancel_release_producer" "pcancel_release_waiter"
  "pcancel_retry" "pcancel_state" "pcancel_stats"
