# DOC0371 42.10.2.  This draft intentionally remains unscheduled until the
# BIOSEQ_CACHE_TESTING-only backend-targeted after_transition_publish pause and
# transition_state(regclass) diagnostic are installed.  Both writers must
# publish the same relation transition concurrently; releasing A must leave B's
# count at one and keep cache admission closed, then releasing B must advance the
# relation epoch a second time and reopen admission.
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
  CREATE TABLE qc_overlapping_transitions(
    id integer PRIMARY KEY,
    value text NOT NULL
  );
  CREATE TABLE qc_transition_backends(
    writer text PRIMARY KEY,
    pid integer NOT NULL
  );
  CREATE TABLE qc_transition_baseline(epoch bigint NOT NULL);
  INSERT INTO qc_overlapping_transitions VALUES (1, 'one'), (2, 'two');
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_transition_baseline;
  DROP TABLE qc_transition_backends;
  DROP TABLE qc_overlapping_transitions;
}

session "controller"
step "overlap_capture_baseline" {
  INSERT INTO qc_transition_baseline
  SELECT relation_epoch
  FROM bioseq_cache_test.transition_state(
    'qc_overlapping_transitions'::regclass);
}
step "overlap_arm_a" {
  SELECT bioseq_cache_test.arm_backend_pause(
    'after_transition_publish',
    (SELECT pid FROM qc_transition_backends WHERE writer = 'a'));
}
step "overlap_arm_b" {
  SELECT bioseq_cache_test.arm_backend_pause(
    'after_transition_publish',
    (SELECT pid FROM qc_transition_backends WHERE writer = 'b'));
}
step "overlap_wait_a" {
  SELECT bioseq_cache_test.wait_until_backend_paused(
    'after_transition_publish',
    (SELECT pid FROM qc_transition_backends WHERE writer = 'a'));
}
step "overlap_wait_b" {
  SELECT bioseq_cache_test.wait_until_backend_paused(
    'after_transition_publish',
    (SELECT pid FROM qc_transition_backends WHERE writer = 'b'));
}
step "overlap_both_published" {
  SELECT relation_transitions = 2 AS two_transitions,
         relation_epoch = baseline.epoch AS epoch_unchanged
  FROM bioseq_cache_test.transition_state(
         'qc_overlapping_transitions'::regclass)
  CROSS JOIN qc_transition_baseline AS baseline;
}
step "overlap_release_a" {
  SELECT bioseq_cache_test.release_backend_pause(
    'after_transition_publish',
    (SELECT pid FROM qc_transition_backends WHERE writer = 'a'));
}
step "overlap_one_published" {
  SELECT relation_transitions = 1 AS one_transition,
         relation_epoch = baseline.epoch + 1 AS epoch_advanced_once
  FROM bioseq_cache_test.transition_state(
         'qc_overlapping_transitions'::regclass)
  CROSS JOIN qc_transition_baseline AS baseline;
}
step "overlap_no_admission" {
  SELECT (SELECT count(*) FROM bioseq_cache_test.entry_states()) AS entries,
         produced, fallbacks
  FROM bioseq_cache.stats();
}
step "overlap_release_b" {
  SELECT bioseq_cache_test.release_backend_pause(
    'after_transition_publish',
    (SELECT pid FROM qc_transition_backends WHERE writer = 'b'));
}
step "overlap_finished" {
  SELECT relation_transitions = 0 AS no_transitions,
         relation_epoch = baseline.epoch + 2 AS epoch_advanced_twice
  FROM bioseq_cache_test.transition_state(
         'qc_overlapping_transitions'::regclass)
  CROSS JOIN qc_transition_baseline AS baseline;
}
step "overlap_final_stats" {
  SELECT produced, completed_hits, fallbacks
  FROM bioseq_cache.stats();
}

session "writer_a"
step "overlap_register_a" {
  INSERT INTO qc_transition_backends VALUES ('a', pg_backend_pid());
}
step "overlap_begin_a" { BEGIN; }
step "overlap_write_a" {
  UPDATE qc_overlapping_transitions SET value = 'a' WHERE id = 1;
}
step "overlap_commit_a" { COMMIT; }

session "writer_b"
step "overlap_register_b" {
  INSERT INTO qc_transition_backends VALUES ('b', pg_backend_pid());
}
step "overlap_begin_b" { BEGIN; }
step "overlap_write_b" {
  UPDATE qc_overlapping_transitions SET value = 'b' WHERE id = 2;
}
step "overlap_commit_b" { COMMIT; }

session "reader"
step "overlap_prime" {
  SET bioseq.query_cache_optimisation_mode = 'same';
  SELECT array_agg(value ORDER BY id) AS values
  FROM qc_overlapping_transitions;
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache.reset_stats();
}
step "overlap_during_transition" {
  SELECT array_agg(value ORDER BY id) AS values
  FROM qc_overlapping_transitions;
}
step "overlap_after_transitions" {
  SELECT array_agg(value ORDER BY id) AS values
  FROM qc_overlapping_transitions;
}
step "overlap_hit" {
  SELECT array_agg(value ORDER BY id) AS values
  FROM qc_overlapping_transitions;
}

permutation "overlap_prime" "overlap_register_a" "overlap_register_b"
  "overlap_capture_baseline" "overlap_arm_a" "overlap_arm_b"
  "overlap_begin_a" "overlap_write_a" "overlap_begin_b"
  "overlap_write_b" "overlap_commit_a" "overlap_wait_a"
  "overlap_commit_b" "overlap_wait_b" "overlap_both_published"
  "overlap_release_a" "overlap_one_published"
  "overlap_during_transition" "overlap_no_admission"
  "overlap_release_b" "overlap_finished" "overlap_after_transitions"
  "overlap_hit" "overlap_final_stats"
