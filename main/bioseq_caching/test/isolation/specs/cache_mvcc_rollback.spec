# A rolled-back concurrent write does not advance committed dependency epochs.
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
  CREATE TABLE qc_rollback(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_rollback VALUES (1, 'one'), (2, 'two'), (3, 'three');
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_rollback;
}

session "controller"
step "rollback_arm" { SELECT bioseq_cache_test.arm_pause('before_terminal_publish'); }
step "rollback_paused" { SELECT bioseq_cache_test.wait_until_paused('before_terminal_publish'); }
step "rollback_release" { SELECT bioseq_cache_test.release_pause('before_terminal_publish'); }
step "rollback_stats" {
  SELECT produced, completed_hits FROM bioseq_cache.stats();
}

session "producer"
step "rollback_producer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "rollback_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_rollback;
}

session "writer"
setup { BEGIN; }
step "rollback_write" { INSERT INTO qc_rollback VALUES (4, 'rolled-back'); }
step "rollback_abort" { ROLLBACK; }

session "consumer"
step "rollback_consumer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "rollback_consume" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_rollback;
}

permutation "rollback_arm" "rollback_producer_mode" "rollback_produce"
  "rollback_paused" "rollback_write" "rollback_abort" "rollback_release"
  "rollback_consumer_mode" "rollback_consume" "rollback_stats"
