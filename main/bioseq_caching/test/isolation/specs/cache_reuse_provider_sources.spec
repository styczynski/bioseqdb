# Generic provider fixture for DOC0371 42.11 and 42.13.  Three distinct
# exact keys overlap in one reusable domain while terminal publication is
# paused.  The consumer may wait only on older claims and must finish without
# PostgreSQL's deadlock detector.
setup
{
  CREATE TABLE cache_provider_iso_sources(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL
  );
  INSERT INTO cache_provider_iso_sources VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0, 'zero', 3),
    (3, 12, 0, 5, 'match-a', 3),
    (4, 12, 1, 5, 'match-b', 3),
    (5, 13, 0, 10, 'ten', 3),
    (6, 14, 0, 20, 'twenty', 3);
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE cache_provider_iso_sources;
}

session "controller"
step "sources_arm" { SELECT bioseq_cache_test.arm_pause('after_building_publish'); }
step "sources_a_paused" { SELECT bioseq_cache_test.wait_until_paused('after_building_publish'); }
step "sources_state" {
  SELECT count(*) AS building
  FROM bioseq_cache_test.entry_states()
  WHERE state = 'BUILDING';
}
step "sources_release" { SELECT bioseq_cache_test.release_pause('after_building_publish'); }

session "source_a"
step "sources_a_init" { SELECT bioseq_cache_test.provider_init(); }
step "sources_a_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_sources'); }
step "sources_a_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "sources_a_query" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_sources WHERE nullable_value <= 10;
}

session "source_b"
step "sources_b_init" { SELECT bioseq_cache_test.provider_init(); }
step "sources_b_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_sources'); }
step "sources_b_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "sources_b_query" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_sources WHERE nullable_value >= 5;
}

session "consumer"
step "sources_c_init" { SELECT bioseq_cache_test.provider_init(); }
step "sources_c_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_sources'); }
step "sources_c_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "sources_c_query" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_sources
  WHERE nullable_value BETWEEN 5 AND 20;
}

permutation "sources_arm"
  "sources_a_init" "sources_a_relation" "sources_a_mode"
  "sources_b_init" "sources_b_relation" "sources_b_mode"
  "sources_c_init" "sources_c_relation" "sources_c_mode"
  "sources_a_query" "sources_a_paused"
  "sources_b_query" "sources_c_query" "sources_state"
  "sources_release"
