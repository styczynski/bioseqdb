# DOC0371 42.10.  Requires the BIOSEQ_CACHE_TESTING-only
# before_visibility_ticket pause in cache_begin_custom_scan after PostgreSQL has
# installed the statement snapshot and immediately before mint_visibility_ticket.
# A stale COMPLETE entry exists when the consumer takes snapshot S.  The target
# write commits before lookup; the consumer must bypass the invalid entry and
# retained-child execution must still return rows visible to S.
setup
{
  CREATE TABLE qc_snapshot_lookup(
    id integer PRIMARY KEY,
    value text NOT NULL
  );
  INSERT INTO qc_snapshot_lookup VALUES
    (1, 'one'), (2, 'two'), (3, 'three');
  SELECT bioseq_cache.invalidate_all();
}

teardown
{
  DROP TABLE qc_snapshot_lookup;
}

session "controller"
step "snapshot_lookup_arm" {
  SELECT bioseq_cache_test.arm_pause('before_visibility_ticket');
}
step "snapshot_lookup_paused" {
  SELECT bioseq_cache_test.wait_until_paused('before_visibility_ticket');
}
step "snapshot_lookup_release" {
  SELECT bioseq_cache_test.release_pause('before_visibility_ticket');
}
step "snapshot_lookup_rejected" {
  SELECT (SELECT count(*) FILTER (WHERE state = 'COMPLETE')
          FROM bioseq_cache_test.entry_states()) AS complete,
         produced, completed_hits, fallbacks
  FROM bioseq_cache.stats();
}
step "snapshot_lookup_final" {
  SELECT produced, completed_hits, fallbacks
  FROM bioseq_cache.stats();
}

session "consumer"
step "snapshot_lookup_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "snapshot_lookup_seed" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_lookup;
}
step "snapshot_lookup_reset" { SELECT bioseq_cache.reset_stats(); }
step "snapshot_lookup_old_snapshot" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_lookup;
}
step "snapshot_lookup_fresh" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_lookup;
}
step "snapshot_lookup_hit" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_lookup;
}

session "writer"
step "snapshot_lookup_write" {
  INSERT INTO qc_snapshot_lookup VALUES (4, 'committed');
}

permutation "snapshot_lookup_mode" "snapshot_lookup_seed"
  "snapshot_lookup_reset" "snapshot_lookup_arm"
  "snapshot_lookup_old_snapshot" "snapshot_lookup_paused"
  "snapshot_lookup_write" "snapshot_lookup_release"
  "snapshot_lookup_rejected" "snapshot_lookup_fresh"
  "snapshot_lookup_hit" "snapshot_lookup_final"
