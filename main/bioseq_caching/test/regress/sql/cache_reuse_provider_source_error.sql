\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

CREATE TABLE cache_provider_source_error_rows(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO cache_provider_source_error_rows
SELECT value,
       value,
       0,
       value::integer,
       'row-' || value::text,
       3
FROM generate_series(1, 100) AS value;
ANALYZE cache_provider_source_error_rows;

-- The loop's SPI statement is the direct single-provider SELECT under test;
-- the volatile outer call only collects its bag into one regression value.
CREATE FUNCTION pg_temp.cache_provider_source_error_bag()
RETURNS bigint[]
VOLATILE
LANGUAGE plpgsql
AS $$
DECLARE
    result bigint[] := ARRAY[]::bigint[];
    value bigint;
BEGIN
    FOR value IN
        SELECT row_id
        FROM cache_provider_source_error_rows
        WHERE row_id BETWEEN 1 AND 100
        ORDER BY row_id
    LOOP
        result := array_append(result, value);
    END LOOP;
    RETURN result;
END
$$;

SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation(
    'cache_provider_source_error_rows');
SELECT bioseq_cache_test.provider_reset();

SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT pg_temp.cache_provider_source_error_bag()::text AS ids
\gset disabled_

SET bioseq.query_cache_optimisation_mode = 'reuse';
SET bioseq.query_cache_ttl = '60 min';
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();

-- Source A is intentionally the broadest candidate and needs no late fetch.
SELECT row_id
FROM cache_provider_source_error_rows
WHERE row_id <= 70
ORDER BY row_id;

-- Produce B and C independently so the later consumer sees three sources.
SELECT bioseq_cache_test.provider_set_mapping_fault('provider_status_error');
SELECT row_id
FROM cache_provider_source_error_rows
WHERE nullable_value BETWEEN 71 AND 90
ORDER BY row_id;
SELECT bioseq_cache_test.provider_set_mapping_fault('none');

SELECT bioseq_cache_test.provider_set_mapping_fault('provider_status_error');
SELECT row_id
FROM cache_provider_source_error_rows
WHERE nullable_value >= 91
ORDER BY row_id;
SELECT bioseq_cache_test.provider_set_mapping_fault('none');

SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();
DISCARD PLANS;

-- A performs no late fetch.  The one-shot status error therefore rejects
-- middle B; C must still late-fetch successfully and the residual must replace
-- only B.  The first staged B value is intentionally discarded.
SELECT bioseq_cache_test.provider_set_mapping_fault(
    'late_fetch_partial_status_error_once');
SELECT pg_temp.cache_provider_source_error_bag()::text AS ids
\gset reused_
SELECT bioseq_cache_test.provider_set_mapping_fault('none');

WITH observed AS (
    SELECT
        :'disabled_ids'::bigint[] AS disabled_ids,
        :'reused_ids'::bigint[] AS reused_ids,
        cache_stats.produced,
        cache_stats.completed_hits,
        cache_stats.reuse_hits,
        cache_stats.fallbacks,
        (provider.stats->>'extract_occurrence')::bigint
            AS extract_occurrence,
        (provider.stats->>'late_fetch_batch')::bigint
            AS late_fetch_batch,
        (provider.stats->>'late_fetch_references')::bigint
            AS late_fetch_references,
        (provider.stats->>'late_fetch_values')::bigint
            AS late_fetch_values,
        (provider.stats->>'late_fetch_errors')::bigint
            AS late_fetch_errors,
        (provider.stats->>'executor_rows')::bigint
            AS executor_rows,
        (provider.stats->>'gated_expensive_rows')::bigint
            AS gated_expensive_rows,
        reference_counts.reader_pins,
        reference_counts.waiter_references,
        reference_counts.admission_reservations
    FROM bioseq_cache.stats() AS cache_stats
    CROSS JOIN LATERAL (
        SELECT bioseq_cache_test.provider_stats() AS stats
    ) AS provider
    CROSS JOIN bioseq_cache_test.planner_reference_counts()
        AS reference_counts
), failed AS (
    SELECT *
    FROM observed
    WHERE disabled_ids IS DISTINCT FROM reused_ids
       OR produced <> 1
       OR completed_hits <> 0
       OR reuse_hits <> 1
       OR fallbacks <> 0
       OR extract_occurrence <> 20
       OR late_fetch_batch <> 2
       OR late_fetch_references <> 30
       OR late_fetch_values <> 11
       OR late_fetch_errors <> 1
       OR executor_rows <> 20
       OR gated_expensive_rows <> 20
       OR reader_pins <> 0
       OR waiter_references <> 0
       OR admission_reservations <> 0
)
SELECT ('per-source reusable ERROR isolation mismatch: '
        || row_to_json(failed)::text)::bigint
FROM failed;

DROP TABLE cache_provider_source_error_rows;
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

\o
SELECT 'cache_reuse_provider_source_error: ok';
