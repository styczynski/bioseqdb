# Ten exact-key waiters share one BUILDING generation and all consume the
# producer's COMPLETE generation without duplicate production.
setup
{
  CREATE TABLE qc_ten_waiters(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_ten_waiters
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
  DROP TABLE qc_ten_waiters;
}

session "controller"
step "ten_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "ten_arm_waiters" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "ten_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "ten_state" {
  SELECT count(*) FILTER (WHERE state = 'BUILDING') AS building,
         max(waiter_count) AS waiter_count
  FROM bioseq_cache_test.entry_states();
}
step "ten_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "ten_release_waiters" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "ten_stats" {
  SELECT produced, completed_hits, inflight_waits
  FROM bioseq_cache.stats();
}

session "producer"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_01"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_01" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_02"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_02" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_03"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_03" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_04"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_04" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_05"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_05" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_06"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_06" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_07"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_07" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_08"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_08" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_09"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_09" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

session "waiter_10"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "ten_wait_10" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_ten_waiters WHERE id <= 10;
}

permutation "ten_arm_producer" "ten_arm_waiters" "ten_produce"
  "ten_producer_paused" "ten_wait_01" "ten_wait_02" "ten_wait_03"
  "ten_wait_04" "ten_wait_05" "ten_wait_06" "ten_wait_07"
  "ten_wait_08" "ten_wait_09" "ten_wait_10" "ten_state"
  "ten_release_producer" "ten_release_waiters" "ten_stats"
