# One exact key elects one producer and a registered waiter consumes COMPLETE.
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
  CREATE TABLE qc_wait(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_wait
  SELECT i, md5(i::text) FROM generate_series(1, 100) AS g(i);
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_wait;
}

session "controller"
step "same_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "same_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "same_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "same_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "same_state" {
  SELECT state, waiters AS waiter_count
  FROM bioseq_cache.entries()
  WHERE kind = 'ExactFinalResult'
  ORDER BY generation;
}
step "same_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "same_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "same_stats" {
  SELECT produced, completed_hits, inflight_waits
  FROM bioseq_cache.stats();
}

session "producer"
step "same_producer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "same_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_wait WHERE id <= 10;
}

session "waiter"
step "same_waiter_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "same_wait" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_wait WHERE id <= 10;
}

permutation "same_arm_producer" "same_arm_waiter"
  "same_producer_mode" "same_produce" "same_producer_paused"
  "same_waiter_mode" "same_wait" "same_waiter_paused" "same_state"
  "same_release_producer" "same_release_waiter" "same_stats"
