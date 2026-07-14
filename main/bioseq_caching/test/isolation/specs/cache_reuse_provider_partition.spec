# Generic provider fixture for DOC0371 42.14.  A write held open against leaf
# B must neither block nor invalidate the reusable generation for leaf A.
setup
{
  CREATE TABLE cache_provider_iso_partitioned(
    row_id bigint NOT NULL,
    part integer NOT NULL,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL
  ) PARTITION BY LIST (part);
  CREATE TABLE cache_provider_iso_part_a
    PARTITION OF cache_provider_iso_partitioned FOR VALUES IN (1);
  CREATE TABLE cache_provider_iso_part_b
    PARTITION OF cache_provider_iso_partitioned FOR VALUES IN (2);
  INSERT INTO cache_provider_iso_partitioned VALUES
    (1, 1, 10, 0, 5, 'a-one', 3),
    (2, 1, 11, 0, 10, 'a-two', 3),
    (3, 2, 12, 0, 5, 'b-one', 3);
}

teardown
{
  DROP TABLE cache_provider_iso_partitioned;
}

session "partition_reader"
step "partition_init" { SELECT bioseq_cache_test.provider_init(); }
step "partition_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_part_a'); }
step "partition_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "partition_source" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_part_a WHERE nullable_value <= 10;
}
step "partition_during_write" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_part_a WHERE nullable_value >= 5;
}
step "partition_after_write" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_part_a WHERE nullable_value >= 5;
}

session "partition_writer"
step "partition_begin" { BEGIN; }
step "partition_write_b" {
  UPDATE cache_provider_iso_part_b SET payload = 'b-updated' WHERE row_id = 3;
}
step "partition_commit" { COMMIT; }

permutation "partition_init" "partition_relation" "partition_mode"
  "partition_source" "partition_begin" "partition_write_b"
  "partition_during_write" "partition_commit" "partition_after_write"
