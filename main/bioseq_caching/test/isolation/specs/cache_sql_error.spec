# A SQL execution error is propagated to the producer and retrying waiter;
# unlike an internal cache-write failure, it is never converted to success.
setup
{
  CREATE TABLE qc_sql_error(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_sql_error
  SELECT i, md5(i::text) FROM generate_series(1, 50) AS g(i);
  DO $$
  BEGIN
    PERFORM bioseq_cache.invalidate_all();
    PERFORM bioseq_cache.reset_stats();
  END
  $$;
}

teardown
{
  DROP TABLE qc_sql_error;
}

session "controller"
step "sqlerr_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "sqlerr_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "sqlerr_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "sqlerr_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "sqlerr_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "sqlerr_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "sqlerr_state" {
  SELECT state, waiter_count, reader_count
  FROM bioseq_cache_test.entry_states()
  ORDER BY generation;
}

session "producer"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "sqlerr_produce" {
  SELECT sum(100 / (id - 25)) AS result
  FROM qc_sql_error;
}

session "waiter"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "sqlerr_wait" {
  SELECT sum(100 / (id - 25)) AS result
  FROM qc_sql_error;
}

session "recovery"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "sqlerr_recover" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_sql_error WHERE id <= 10;
}

permutation "sqlerr_arm_producer" "sqlerr_arm_waiter" "sqlerr_produce"
  "sqlerr_producer_paused" "sqlerr_wait" "sqlerr_waiter_paused"
  "sqlerr_release_producer" "sqlerr_release_waiter" "sqlerr_recover"
  "sqlerr_state"
