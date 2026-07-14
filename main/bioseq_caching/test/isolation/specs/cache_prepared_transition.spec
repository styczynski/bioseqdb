# A producer bound to an older snapshot cannot publish across PREPARE, an
# unrelated commit, and COMMIT PREPARED.  The dynamic 2PC gate blocks reuse
# while the GID exists and reopens only after coarse resolution invalidation.
setup
{
  CREATE TABLE qc_prepared_transition(
    id integer PRIMARY KEY,
    value text NOT NULL
  );
  CREATE TABLE qc_prepared_unrelated(id integer PRIMARY KEY);
  INSERT INTO qc_prepared_transition VALUES (1, 'one'), (2, 'two');
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_prepared_unrelated;
  DROP TABLE qc_prepared_transition;
}

session "controller"
step "prepared_arm" {
  SELECT bioseq_cache_test.arm_pause('before_terminal_publish');
}
step "prepared_paused" {
  SELECT bioseq_cache_test.wait_until_paused('before_terminal_publish');
}
step "prepared_release" {
  SELECT bioseq_cache_test.release_pause('before_terminal_publish');
}
step "prepared_stats" {
  SELECT produced, completed_hits FROM bioseq_cache.stats();
}
step "prepared_remaining" {
  SELECT count(*) AS prepared
  FROM pg_prepared_xacts WHERE database = current_database();
}

session "producer"
step "prepared_producer_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "prepared_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_prepared_transition;
}

session "writer"
step "prepared_begin" { BEGIN; }
step "prepared_write" {
  INSERT INTO qc_prepared_transition VALUES (3, 'prepared');
}
step "prepared_prepare" {
  PREPARE TRANSACTION 'cache_iso_prepared_transition';
}
step "prepared_unrelated" {
  INSERT INTO qc_prepared_unrelated VALUES (1);
}
step "prepared_commit" {
  COMMIT PREPARED 'cache_iso_prepared_transition';
}

session "consumer"
step "prepared_consumer_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "prepared_before_commit" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_prepared_transition;
}
step "prepared_after_commit" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_prepared_transition;
}
step "prepared_hit" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_prepared_transition;
}

permutation "prepared_arm" "prepared_producer_mode" "prepared_produce"
  "prepared_paused" "prepared_begin" "prepared_write" "prepared_prepare"
  "prepared_unrelated" "prepared_release" "prepared_consumer_mode"
  "prepared_before_commit" "prepared_commit" "prepared_after_commit"
  "prepared_hit" "prepared_stats" "prepared_remaining"
