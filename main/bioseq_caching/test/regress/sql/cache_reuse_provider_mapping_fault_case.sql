SELECT bioseq_cache_test.provider_set_mapping_fault('none');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

SELECT array_agg(row_id ORDER BY row_id)
FROM cache_provider_rows
WHERE nullable_value <= 10;

PREPARE bioseq_mapping_probe AS
SELECT array_agg(row_id ORDER BY row_id)::bigint[] AS ids
FROM cache_provider_rows
WHERE nullable_value >= 5;

-- Force and cache the correct generic plan without running BeginCustomScan.
EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF)
EXECUTE bioseq_mapping_probe;

SELECT
    (stats->>'extract_occurrence')::bigint AS extract_occurrence,
    (stats->>'late_fetch_batch')::bigint AS late_fetch_batch,
    (stats->>'late_fetch_references')::bigint AS late_fetch_references,
    (stats->>'executor_rows')::bigint AS executor_rows,
    (stats->>'gated_expensive_rows')::bigint AS gated_expensive_rows,
    (stats->>'last_boundary_column_count')::bigint
        AS boundary_column_count,
    (stats->>'last_required_boundary_count')::bigint
        AS required_boundary_count,
    (stats->>'last_lineage_column_count')::bigint AS lineage_column_count,
    (stats->>'last_required_lineage_count')::bigint
        AS required_lineage_count,
    (stats->>'last_recipe_required_boundary_count')::bigint
        AS recipe_required_boundary_count
FROM (SELECT bioseq_cache_test.provider_stats() AS stats) AS provider
\gset mapping_before_

SELECT bioseq_cache.reset_stats();
SELECT bioseq_cache_test.provider_set_mapping_fault(:'mapping_fault');

EXECUTE bioseq_mapping_probe
\gset mapping_result_

WITH observed AS (
    SELECT
        :'mapping_fault'::text AS fault,
        :'mapping_result_ids'::bigint[] AS ids,
        cache_stats.produced,
        cache_stats.completed_hits,
        cache_stats.reuse_hits,
        cache_stats.fallbacks,
        (provider.stats->>'extract_occurrence')::bigint
            - :mapping_before_extract_occurrence::bigint
            AS extract_occurrence,
        (provider.stats->>'late_fetch_batch')::bigint
            - :mapping_before_late_fetch_batch::bigint AS late_fetch_batch,
        (provider.stats->>'late_fetch_references')::bigint
            - :mapping_before_late_fetch_references::bigint
            AS late_fetch_references,
        (provider.stats->>'executor_rows')::bigint
            - :mapping_before_executor_rows::bigint AS executor_rows,
        (provider.stats->>'gated_expensive_rows')::bigint
            - :mapping_before_gated_expensive_rows::bigint
            AS gated_expensive_rows,
        :mapping_before_boundary_column_count::bigint
            AS boundary_column_count,
        :mapping_before_required_boundary_count::bigint
            AS required_boundary_count,
        :mapping_before_lineage_column_count::bigint
            AS lineage_column_count,
        :mapping_before_required_lineage_count::bigint
            AS required_lineage_count,
        :mapping_before_recipe_required_boundary_count::bigint
            AS recipe_required_boundary_count,
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
    WHERE ids IS DISTINCT FROM ARRAY[3,4,5,6]::bigint[]
       OR produced <> 1
       OR completed_hits <> 0
       OR reuse_hits <> 0
       OR fallbacks <> 0
       OR extract_occurrence <> 4
       OR late_fetch_batch <> 0
       OR late_fetch_references <> 0
       OR executor_rows <> 4
       OR gated_expensive_rows <> 4
       OR boundary_column_count <> 6
       OR required_boundary_count <> 1
       OR lineage_column_count <> 3
       OR required_lineage_count <> 3
       OR recipe_required_boundary_count <> 1
       OR reader_pins <> 0
       OR waiter_references <> 0
       OR admission_reservations <> 0
)
SELECT ('frozen mapping fail-open mismatch: '
        || row_to_json(failed)::text)::bigint
FROM failed;

SELECT bioseq_cache_test.provider_set_mapping_fault('none');
DEALLOCATE bioseq_mapping_probe;
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();
