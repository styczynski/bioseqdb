# Generic provider fixture for DOC0371 42.12/42.12.1.  A frozen generic plan
# is executed after the provider runtime digest changes and after provider
# unregistration.  Both permutations must leave gates false and execute the
# retained provider child exactly once.
setup
{
  CREATE TABLE cache_provider_iso_validity(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL
  );
  INSERT INTO cache_provider_iso_validity VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0, 'zero', 3),
    (3, 12, 0, 5, 'match-a', 3),
    (4, 12, 1, 5, 'match-b', 3),
    (5, 13, 0, 10, 'ten', 3),
    (6, 14, 0, 20, 'twenty', 3);
}

teardown
{
  DROP TABLE cache_provider_iso_validity;
}

session "validity"
step "validity_init" { SELECT bioseq_cache_test.provider_init(); }
step "validity_relation" { SELECT bioseq_cache_test.provider_use_relation('cache_provider_iso_validity'); }
step "validity_mode" { SET bioseq.query_cache_optimisation_mode = 'reuse'; }
step "validity_generic" { SET plan_cache_mode = force_generic_plan; }
step "validity_source" {
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_validity WHERE nullable_value <= 10;
}
step "validity_prepare" {
  PREPARE cache_provider_validity(integer) AS
  SELECT count(*) AS rows, sum(row_id) AS row_sum
  FROM cache_provider_iso_validity WHERE nullable_value >= $1;
}
step "validity_freeze" { EXPLAIN (COSTS OFF) EXECUTE cache_provider_validity(5); }
step "validity_digest_fault" { SELECT bioseq_cache_test.provider_set_digest_fault('runtime_mismatch'); }
step "validity_digest_execute" { EXECUTE cache_provider_validity(5); }
step "validity_digest_clear" { SELECT bioseq_cache_test.provider_set_digest_fault('none'); }
step "validity_shutdown" { SELECT bioseq_cache_test.provider_shutdown(); }
step "validity_unregistered_execute" { EXECUTE cache_provider_validity(5); }
step "validity_deallocate" { DEALLOCATE cache_provider_validity; }

permutation "validity_init" "validity_relation" "validity_mode"
  "validity_generic" "validity_source" "validity_prepare"
  "validity_freeze" "validity_digest_fault" "validity_digest_execute"
  "validity_digest_clear" "validity_shutdown"
  "validity_unregistered_execute" "validity_deallocate"
