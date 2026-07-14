# TTL zero rejects new lookups immediately but preserves a waiter registered
# against the in-flight generation until it converts its reference to a pin.
setup
{
  CREATE TABLE qc_ttl_zero(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_ttl_zero
  SELECT i, md5(i::text) FROM generate_series(1, 20) AS g(i);
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_ttl_zero;
}

session "controller"
step "ttl_arm_producer" { SELECT bioseq_cache_test.arm_pause('before_terminal_publish'); }
step "ttl_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "ttl_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('before_terminal_publish'); }
step "ttl_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "ttl_release_producer" { SELECT bioseq_cache_test.release_pause('before_terminal_publish'); }
step "ttl_sweep" { SELECT bioseq_cache_test.request_janitor_sweep(); }
step "ttl_before_waiter" {
  SELECT state, waiter_count, reader_count
  FROM bioseq_cache_test.entry_states()
  ORDER BY generation;
}
step "ttl_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "ttl_sweep_after" { SELECT bioseq_cache_test.request_janitor_sweep(); }

session "producer"
step "ttl_producer_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
  SET bioseq.query_cache_ttl = 0;
}
step "ttl_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_ttl_zero;
}

session "waiter"
step "ttl_waiter_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
  SET bioseq.query_cache_ttl = 0;
}
step "ttl_wait" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_ttl_zero;
}

session "new_reader"
step "ttl_new_reader_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
  SET bioseq.query_cache_ttl = 0;
}
step "ttl_new_read" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_ttl_zero;
}

permutation "ttl_arm_producer" "ttl_arm_waiter" "ttl_producer_mode"
  "ttl_produce" "ttl_producer_paused" "ttl_waiter_mode" "ttl_wait"
  "ttl_waiter_paused" "ttl_release_producer" "ttl_sweep"
  "ttl_before_waiter" "ttl_new_reader_mode" "ttl_new_read"
  "ttl_release_waiter" "ttl_sweep_after"
