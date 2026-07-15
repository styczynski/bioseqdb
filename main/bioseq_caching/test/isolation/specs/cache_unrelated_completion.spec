# A COMPLETE exact key remains readable while another key transitions through
# BUILDING with a waiter; registry state changes stay isolated by full key.
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
  CREATE TABLE qc_unrelated_completion(
    id integer PRIMARY KEY,
    bucket integer NOT NULL,
    value text NOT NULL
  );
  INSERT INTO qc_unrelated_completion
  SELECT i, CASE WHEN i <= 10 THEN 1 ELSE 2 END, md5(i::text)
  FROM generate_series(1, 20) AS g(i);
  DO $$
  BEGIN
    PERFORM bioseq_cache.invalidate_all();
    PERFORM bioseq_cache.reset_stats();
  END
  $$;
}

teardown
{
  DROP TABLE qc_unrelated_completion;
}

session "controller"
step "transition_arm_producer" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "transition_arm_waiter" { SELECT bioseq_cache_test.arm_pause('after_waiter_registered'); }
step "transition_producer_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "transition_waiter_paused" { SELECT bioseq_cache_test.wait_until_paused('after_waiter_registered'); }
step "transition_state" {
  SELECT count(*) FILTER (WHERE state = 'BUILDING') AS building,
         count(*) FILTER (WHERE state = 'COMPLETE') AS complete,
         sum(waiter_count) AS waiters
  FROM bioseq_cache_test.entry_states();
}
step "transition_release_producer" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }
step "transition_release_waiter" { SELECT bioseq_cache_test.release_pause('after_waiter_registered'); }
step "transition_stats" {
  SELECT produced, completed_hits, inflight_waits
  FROM bioseq_cache.stats();
}

session "unrelated"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "transition_seed" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_unrelated_completion WHERE bucket = 2;
}
step "transition_hit" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_unrelated_completion WHERE bucket = 2;
}

session "producer"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "transition_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_unrelated_completion WHERE bucket = 1;
}

session "waiter"
setup { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "transition_wait" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_unrelated_completion WHERE bucket = 1;
}

permutation "transition_seed" "transition_arm_producer"
  "transition_arm_waiter" "transition_produce" "transition_producer_paused"
  "transition_wait" "transition_waiter_paused" "transition_hit"
  "transition_state" "transition_release_producer"
  "transition_release_waiter" "transition_stats"
