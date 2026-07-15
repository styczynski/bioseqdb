# Canceling one waiter drains its reference without disturbing the producer.
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
  CREATE TABLE qc_cancel(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_cancel
  SELECT i, md5(i::text) FROM generate_series(1, 100) AS g(i);
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_cancel;
}

session "controller"
step "cancel_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "cancel_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "cancel_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "cancel_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "cancel_waiter_backend" {
  SELECT pg_cancel_backend(pid)
  FROM pg_stat_activity
  WHERE application_name = 'bioseq_cache_cancel_waiter';
}
step "cancel_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "cancel_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "cancel_state" {
  SELECT state, waiter_count, reader_count
  FROM bioseq_cache_test.entry_states()
  ORDER BY generation;
}

session "producer"
step "cancel_producer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "cancel_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_cancel WHERE id <= 10;
}

session "waiter"
step "cancel_waiter_mode" {
  SET application_name = 'bioseq_cache_cancel_waiter';
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "cancel_wait" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_cancel WHERE id <= 10;
}

permutation "cancel_arm_producer" "cancel_arm_waiter"
  "cancel_producer_mode" "cancel_produce" "cancel_producer_paused"
  "cancel_waiter_mode" "cancel_wait" "cancel_waiter_paused"
  "cancel_waiter_backend" "cancel_release_waiter" "cancel_release_producer"
  "cancel_state"
