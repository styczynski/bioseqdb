# Cache encoding/storage failure is fail-open and never exposes a partial store.
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
  CREATE TABLE qc_failure(id integer PRIMARY KEY, value text NOT NULL);
  INSERT INTO qc_failure
  SELECT i, repeat(md5(i::text), 8) FROM generate_series(1, 50) AS g(i);
  SELECT bioseq_cache.reset_stats();
}

teardown
{
  DROP TABLE qc_failure;
}

session "controller"
step "failure_inject" { SELECT bioseq_cache_test.fail_next_write('encoder_error'); }
step "failure_state" {
  SELECT count(*) FILTER (WHERE state = 'BUILDING') AS building,
         count(*) FILTER (WHERE state = 'FAILED') AS failed
  FROM bioseq_cache_test.entry_states();
}

session "producer"
step "failure_producer_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "failure_produce" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_failure WHERE id <= 25;
}

session "reader"
step "failure_reader_mode" { SET bioseq.query_cache_optimisation_mode = 'same'; }
step "failure_read" {
  SELECT count(*) AS rows, sum(id) AS id_sum
  FROM qc_failure WHERE id <= 25;
}

permutation "failure_inject" "failure_producer_mode" "failure_produce"
  "failure_state" "failure_reader_mode" "failure_read"
