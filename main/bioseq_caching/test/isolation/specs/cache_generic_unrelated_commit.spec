# DOC0371 42.10.3 generic half.  Exact caching observes database-wide XID
# completion conservatively: an unrelated commit after source execution but
# before terminal publication rejects that generation.  A later execution may
# publish only from a fresh post-commit visibility ticket.
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
  CREATE TABLE qc_generic_commit_target(
    id integer PRIMARY KEY,
    value text NOT NULL
  );
  CREATE TABLE qc_generic_commit_unrelated(id integer PRIMARY KEY);
  INSERT INTO qc_generic_commit_target VALUES
    (1, 'one'), (2, 'two'), (3, 'three');
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_generic_commit_unrelated;
  DROP TABLE qc_generic_commit_target;
}

session "controller"
step "generic_commit_arm" {
  SELECT bioseq_cache_test.arm_pause('before_terminal_publish');
}
step "generic_commit_paused" {
  SELECT bioseq_cache_test.wait_until_paused('before_terminal_publish');
}
step "generic_commit_release" {
  SELECT bioseq_cache_test.release_pause('before_terminal_publish');
}
step "generic_commit_rejected" {
  SELECT (SELECT count(*) FILTER (WHERE state = 'COMPLETE')
          FROM bioseq_cache_test.entry_states()) AS complete,
         produced, fallbacks
  FROM bioseq_cache.stats();
}
step "generic_commit_final" {
  SELECT (SELECT count(*) FILTER (WHERE state = 'COMPLETE')
          FROM bioseq_cache_test.entry_states()) AS complete,
         produced, completed_hits, fallbacks
  FROM bioseq_cache.stats();
}

session "producer"
step "generic_commit_producer_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "generic_commit_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_generic_commit_target;
}

session "writer"
step "generic_commit_unrelated_write" {
  INSERT INTO qc_generic_commit_unrelated VALUES (1);
}

session "consumer"
step "generic_commit_consumer_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "generic_commit_consume" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_generic_commit_target;
}
step "generic_commit_hit" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_generic_commit_target;
}

permutation "generic_commit_arm" "generic_commit_producer_mode"
  "generic_commit_produce" "generic_commit_paused"
  "generic_commit_unrelated_write" "generic_commit_release"
  "generic_commit_rejected" "generic_commit_consumer_mode"
  "generic_commit_consume" "generic_commit_hit" "generic_commit_final"
