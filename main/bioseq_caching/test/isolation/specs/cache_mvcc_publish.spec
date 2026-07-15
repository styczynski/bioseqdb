# A committed dependency write between source execution and publication rejects
# the old-snapshot generation; the later consumer sees the committed row.
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
  CREATE TABLE qc_mvcc(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_mvcc VALUES (1, 'one'), (2, 'two'), (3, 'three');
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_mvcc;
}

session "controller"
step "mvcc_arm" { SELECT bioseq_cache_test.arm_pause('before_terminal_publish'); }
step "mvcc_paused" { SELECT bioseq_cache_test.wait_until_paused('before_terminal_publish'); }
step "mvcc_release" { SELECT bioseq_cache_test.release_pause('before_terminal_publish'); }
step "mvcc_state" {
  SELECT count(*) FILTER (WHERE state = 'FAILED') AS failed,
         count(*) FILTER (WHERE state = 'COMPLETE') AS complete
  FROM bioseq_cache_test.entry_states();
}

session "producer"
step "mvcc_producer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "mvcc_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_mvcc;
}

session "writer"
step "mvcc_write" { INSERT INTO qc_mvcc VALUES (4, 'committed'); }

session "consumer"
step "mvcc_consumer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "mvcc_consume" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_mvcc;
}

permutation "mvcc_arm" "mvcc_producer_mode" "mvcc_produce" "mvcc_paused"
  "mvcc_write" "mvcc_release" "mvcc_consumer_mode" "mvcc_consume"
  "mvcc_state"
