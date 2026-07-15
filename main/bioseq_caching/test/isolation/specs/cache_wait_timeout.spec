# A waiter statement timeout drains its reference without canceling the
# producer; a later reader consumes the producer's eventual COMPLETE result.
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
  CREATE TABLE qc_wait_timeout(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_wait_timeout
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
  DROP TABLE qc_wait_timeout;
}

session "controller"
step "timeout_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "timeout_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "timeout_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "timeout_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "timeout_expire" { SELECT pg_sleep(1.2); }
step "timeout_waiter_gone" {
  SELECT state, waiter_count
  FROM bioseq_cache_test.entry_states()
  WHERE state <> 'RETIRED';
}
step "timeout_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "timeout_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "timeout_stats" {
  SELECT produced, completed_hits, inflight_waits
  FROM bioseq_cache.stats();
}

session "producer"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "timeout_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_wait_timeout WHERE id <= 10;
}

session "waiter"
setup
{
  SET statement_timeout = '1 s';
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "timeout_wait" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_wait_timeout WHERE id <= 10;
}

session "reader"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "timeout_read" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_wait_timeout WHERE id <= 10;
}

permutation "timeout_arm_producer" "timeout_arm_waiter" "timeout_produce"
  "timeout_producer_paused" "timeout_wait" "timeout_waiter_paused"
  "timeout_expire" "timeout_waiter_gone" "timeout_release_waiter"
  "timeout_release_producer" "timeout_read" "timeout_stats"
