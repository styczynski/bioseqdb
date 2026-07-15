# DOC0371 42.14.  A frozen two-leaf plan consumes reusable physical fragments
# A and B.  After its A reader is pinned but before B begins, a precise
# committed write invalidates B only.  The A reader must continue and the
# frozen query must complete without a database-wide/coarse fallback.
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
  CREATE TABLE cache_provider_iso_partitioned(
    row_id bigint NOT NULL,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    part integer NOT NULL
  ) PARTITION BY LIST (part);
  CREATE TABLE cache_provider_iso_part_a
    PARTITION OF cache_provider_iso_partitioned FOR VALUES IN (1);
  CREATE TABLE cache_provider_iso_part_b
    PARTITION OF cache_provider_iso_partitioned FOR VALUES IN (2);
  INSERT INTO cache_provider_iso_partitioned VALUES
    (1, 10, 0, 5, 'a-one', 3, 1),
    (2, 11, 0, 10, 'a-two', 3, 1),
    (3, 12, 0, 5, 'b-one', 3, 2);
  SELECT bioseq_cache.invalidate_all();
  SELECT bioseq_cache_test.request_janitor_sweep();
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE cache_provider_iso_partitioned;
}

session "partition_controller"
step "partition_arm" {
  SELECT bioseq_cache_test.arm_pause('after_coverage_reader_pinned');
}
step "partition_paused" {
  SELECT bioseq_cache_test.wait_until_paused(
    'after_coverage_reader_pinned');
}
step "partition_pinned_state" {
  SELECT
    count(*) FILTER (
      WHERE kind = 'ReusableCoverage'
        AND state = 'COMPLETE'
        AND readers = 1
        AND sql LIKE '%cache_provider_partition_source_a%') = 1
      AS a_reader_pinned,
    count(*) FILTER (
      WHERE kind = 'ReusableCoverage'
        AND state = 'COMPLETE'
        AND readers = 0
        AND sql LIKE '%cache_provider_partition_source_b%') = 1
      AS b_not_yet_pinned
  FROM bioseq_cache.entries();
}
step "partition_after_b_write" {
  SELECT
    count(*) FILTER (
      WHERE kind = 'ReusableCoverage'
        AND state = 'COMPLETE'
        AND readers = 1
        AND sql LIKE '%cache_provider_partition_source_a%') = 1
      AS a_remains_complete,
    count(*) FILTER (
      WHERE kind = 'ReusableCoverage'
        AND state = 'COMPLETE'
        AND readers = 0
        AND sql LIKE '%cache_provider_partition_source_b%') = 1
      AS b_non_current,
    count(*) FILTER (
      WHERE kind = 'ReusableCoverage'
        AND state = 'INVALIDATED'
        AND sql LIKE '%cache_provider_partition_source_a%') = 0
      AS no_global_reusable_purge
  FROM bioseq_cache.entries();
}
step "partition_release" {
  SELECT bioseq_cache_test.release_pause(
    'after_coverage_reader_pinned');
}

session "partition_reader"
step "partition_init" { SELECT bioseq_cache_test.provider_init(); }
step "partition_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "partition_generic" { SET plan_cache_mode = force_generic_plan; }
step "partition_relation_a" {
  SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_part_a');
}
step "partition_prepare_source_a" {
  PREPARE cache_provider_partition_source_a AS
  SELECT row_id
  FROM ONLY cache_provider_iso_part_a
  WHERE nullable_value <= 10
  ORDER BY row_id;
}
step "partition_freeze_source_a" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_partition_source_a', 1, 0);
}
step "partition_source_a" {
  EXECUTE cache_provider_partition_source_a;
}
step "partition_relation_b" {
  SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_part_b');
}
step "partition_prepare_source_b" {
  PREPARE cache_provider_partition_source_b AS
  SELECT row_id
  FROM ONLY cache_provider_iso_part_b
  WHERE nullable_value <= 10
  ORDER BY row_id;
}
step "partition_freeze_source_b" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_partition_source_b', 1, 0);
}
step "partition_source_b" {
  EXECUTE cache_provider_partition_source_b;
}
step "partition_source_state" {
  SELECT count(*) = 2 AS two_complete_sources
  FROM bioseq_cache.entries()
  WHERE kind = 'ReusableCoverage' AND state = 'COMPLETE';
}
step "partition_relations_all" {
  SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_part_a');
  SELECT bioseq_cache_test.provider_add_relation('cache_provider_iso_part_b');
}
step "partition_prepare_consumer" {
  PREPARE cache_provider_partition_consumer AS
  SELECT a.row_id AS a_row_id, b.row_id AS b_row_id
  FROM ONLY cache_provider_iso_part_a AS a
  CROSS JOIN ONLY cache_provider_iso_part_b AS b
  WHERE a.nullable_value >= 5
    AND b.nullable_value >= 5
  ORDER BY a.row_id, b.row_id;
}
step "partition_freeze_consumer" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_partition_consumer', 2, 2);
}
step "partition_reset_stats" { SELECT bioseq_cache.reset_stats(); }
step "partition_consume" {
  EXECUTE cache_provider_partition_consumer;
}
step "partition_consume_stats" {
  SELECT produced = 1 AS only_a_current_generation,
         reuse_hits = 1 AS only_a_reused,
         fallbacks = 1 AS b_precise_fallback
  FROM bioseq_cache.stats();
}
step "partition_future_relation_a" {
  SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_part_a');
}
step "partition_prepare_future_a" {
  PREPARE cache_provider_partition_future_a AS
  SELECT row_id
  FROM ONLY cache_provider_iso_part_a
  WHERE nullable_value = 10
  ORDER BY row_id;
}
step "partition_freeze_future_a" {
  SELECT bioseq_cache_test.assert_prepared_reusable_plan(
    'cache_provider_partition_future_a', 1, 2);
}
step "partition_future_a" {
  EXECUTE cache_provider_partition_future_a;
}
step "partition_final_stats" {
  SELECT produced = 2 AS two_current_generations,
         reuse_hits = 2 AS future_a_reused,
         fallbacks = 1 AS only_b_fell_back
  FROM bioseq_cache.stats();
}
step "partition_deallocate" {
  DEALLOCATE cache_provider_partition_source_a;
  DEALLOCATE cache_provider_partition_source_b;
  DEALLOCATE cache_provider_partition_consumer;
  DEALLOCATE cache_provider_partition_future_a;
}

session "partition_writer"
step "partition_begin" { BEGIN; }
step "partition_write_b" {
  UPDATE ONLY cache_provider_iso_part_b
  SET payload = 'b-updated' WHERE row_id = 3;
  SELECT bioseq_cache_test.provider_record_write(
    'cache_provider_iso_part_b') IS NOT NULL AS x;
}
step "partition_commit" { COMMIT; }
step "partition_transition_drained" {
  SELECT database_transitions = 0 AS no_database_transition,
         coarse_transitions = 0 AS no_coarse_transition
  FROM bioseq_cache_test.transition_state();
}

permutation "partition_init" "partition_mode" "partition_generic"
  "partition_relation_a" "partition_prepare_source_a"
  "partition_freeze_source_a" "partition_source_a"
  "partition_relation_b" "partition_prepare_source_b"
  "partition_freeze_source_b" "partition_source_b"
  "partition_source_state" "partition_relations_all"
  "partition_prepare_consumer" "partition_freeze_consumer"
  "partition_reset_stats" "partition_arm" "partition_consume"
  "partition_paused" "partition_pinned_state"
  "partition_begin" "partition_write_b"
  "partition_commit" "partition_transition_drained"
  "partition_after_b_write" "partition_release" "partition_consume_stats"
  "partition_future_relation_a" "partition_prepare_future_a"
  "partition_freeze_future_a" "partition_future_a"
  "partition_final_stats" "partition_deallocate"
