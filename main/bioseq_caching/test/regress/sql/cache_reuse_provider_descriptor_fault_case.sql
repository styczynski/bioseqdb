SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

SELECT count(*)
FROM cache_provider_binding_rows
WHERE nullable_value >= 5;

SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();
SELECT bioseq_cache_test.set_coverage_descriptor_fault(:'descriptor_fault');

SELECT count(*) AS result_count
FROM cache_provider_binding_rows
WHERE nullable_value >= 6
\gset descriptor_result_

SELECT bioseq_cache_test.set_coverage_descriptor_fault('none');

SELECT
    :'descriptor_fault' AS fault,
    :descriptor_result_result_count::bigint AS result_count,
    cache_stats.reuse_hits,
    (provider.stats->>'executor_rows')::bigint AS executor_rows,
    reference_counts.reader_pins,
    reference_counts.waiter_references,
    reference_counts.admission_reservations
FROM bioseq_cache.stats() AS cache_stats
CROSS JOIN LATERAL (
    SELECT bioseq_cache_test.provider_stats() AS stats
) AS provider
CROSS JOIN bioseq_cache_test.planner_reference_counts() AS reference_counts;
