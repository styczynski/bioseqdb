# DOC0371 42.10.1 cache-only generic analogue.  Requires the
# BIOSEQ_CACHE_TESTING-only before_visibility_ticket pause after the statement
# snapshot is active.  With no existing entry, a target write commits after
# snapshot S but before the cache handshake.  The request must neither claim nor
# publish a generation, and retained-child execution must return S-visible rows.
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
  CREATE TABLE qc_snapshot_claim(
    id integer PRIMARY KEY,
    value text NOT NULL
  );
  INSERT INTO qc_snapshot_claim VALUES
    (1, 'one'), (2, 'two'), (3, 'three');
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_snapshot_claim;
}

session "controller"
step "snapshot_claim_arm" {
  SELECT bioseq_cache_test.arm_pause('before_visibility_ticket');
}
step "snapshot_claim_paused" {
  SELECT bioseq_cache_test.wait_until_paused('before_visibility_ticket');
}
step "snapshot_claim_release" {
  SELECT bioseq_cache_test.release_pause('before_visibility_ticket');
}
step "snapshot_claim_rejected" {
  SELECT (SELECT count(*) FROM bioseq_cache_test.entry_states()) AS entries,
         produced, completed_hits, fallbacks
  FROM bioseq_cache.stats();
}
step "snapshot_claim_final" {
  SELECT produced, completed_hits, fallbacks
  FROM bioseq_cache.stats();
}

session "consumer"
step "snapshot_claim_mode" {
  SET bioseq.query_cache_optimisation_mode = 'same';
}
step "snapshot_claim_old_snapshot" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_claim;
}
step "snapshot_claim_fresh" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_claim;
}
step "snapshot_claim_hit" {
  SELECT count(*) AS rows, sum(id) AS id_sum FROM qc_snapshot_claim;
}

session "writer"
step "snapshot_claim_write" {
  INSERT INTO qc_snapshot_claim VALUES (4, 'committed');
}

permutation "snapshot_claim_mode" "snapshot_claim_arm"
  "snapshot_claim_old_snapshot" "snapshot_claim_paused"
  "snapshot_claim_write" "snapshot_claim_release"
  "snapshot_claim_rejected" "snapshot_claim_fresh"
  "snapshot_claim_hit" "snapshot_claim_final"
