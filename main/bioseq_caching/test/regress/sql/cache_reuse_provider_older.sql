\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'reuse';
SET bioseq.query_cache_ttl = '60 min';

CREATE TABLE cache_provider_older_rows(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO cache_provider_older_rows VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0,    'zero', 3),
    (3, 12, 0, 5,    'first match', 3),
    (4, 12, 1, 5,    'second match', 3),
    (5, 13, 0, 10,   'ten', 3),
    (6, 14, 0, 20,   'twenty', 3);

SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation('cache_provider_older_rows');
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();

-- Create the plan-capture helper before producing any reusable source.  Calling
-- it from SELECT keeps the analyzed statement outside a write-capable DO
-- utility boundary.
CREATE FUNCTION pg_temp.cache_provider_older_plan()
RETURNS jsonb
LANGUAGE plpgsql
AS $$
DECLARE
    plan_text text;
BEGIN
    EXECUTE
        'EXPLAIN (ANALYZE, FORMAT JSON, VERBOSE, COSTS OFF, '
        'TIMING OFF, SUMMARY OFF) '
        'SELECT row_id FROM cache_provider_older_rows '
        'WHERE nullable_value IS NULL '
        'OR nullable_value BETWEEN 5 AND 20 ORDER BY row_id'
        INTO plan_text;
    RETURN plan_text::jsonb;
END
$$;

-- Source A stores row_id plus occurrence lineage, but nullable_value is used
-- only by its predicate and must remain absent from the stored boundary.
SELECT row_id
FROM cache_provider_older_rows
WHERE nullable_value <= 10
ORDER BY row_id;

-- Force source B to be independently produced instead of consuming A.  The
-- completed A generation remains in the same reusable coverage directory.
SELECT bioseq_cache_test.provider_set_mapping_fault('provider_error');
SELECT row_id
FROM cache_provider_older_rows
WHERE nullable_value >= 5
ORDER BY row_id;
SELECT bioseq_cache_test.provider_set_mapping_fault('none');

-- Preserve both completed sources while resetting the consumer's observable
-- counters and forcing a fresh plan that maps both older generations.
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

-- Execute the older-source merge under structured EXPLAIN ANALYZE so the
-- runtime branch count, residual gate, and provider work are proved rather
-- than inferred from planner metadata.
SELECT pg_temp.cache_provider_older_plan() AS plan
\gset older_runtime_

WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'older_runtime_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child.value
    FROM plan_nodes
    CROSS JOIN LATERAL jsonb_array_elements(
        COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
), proof AS (
    SELECT
        (SELECT node
         FROM plan_nodes
         WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'
         LIMIT 1) AS cache_node,
        (SELECT node
         FROM plan_nodes
         WHERE node ? 'Test Provider Rows'
         LIMIT 1) AS provider_node
)
SELECT ('structured older reusable runtime proof failed: '
        || row_to_json(proof)::text)::bigint
FROM proof
WHERE cache_node IS NULL
   OR cache_node->>'Actual Cache Action'
        <> '[Reuse] Reused selected branches and produced residual'
   OR (cache_node->>'Reusable Sources')::bigint <> 3
   OR (cache_node->>'Selected Cache Branches')::bigint <> 2
   OR (cache_node->>'Provider Runtime Recipe Valid')::boolean IS NOT TRUE
   OR (cache_node->>'Actual Rows')::bigint <> 5
   OR cache_node->>'State' <> 'COMPLETE'
   OR jsonb_typeof(cache_node->'Cache Branch 1') <> 'object'
   OR jsonb_typeof(cache_node->'Cache Branch 2') <> 'object'
   OR jsonb_typeof(cache_node->'Residual Child') <> 'object'
   OR provider_node IS NULL
   OR (provider_node->>'Test Provider Rows')::bigint <> 1
   OR (provider_node->>'Test Provider Gated Work')::bigint <> 1
   OR (provider_node->>'Actual Rows')::bigint <> 1;

-- Replay the generation produced by the analyzed merge and prove its exact
-- values.  No provider row may execute during this current-generation hit.
\o
SELECT row_id
FROM cache_provider_older_rows
WHERE nullable_value IS NULL
   OR nullable_value BETWEEN 5 AND 20
ORDER BY row_id;
\o /dev/null

SELECT
    cache_stats.produced,
    cache_stats.completed_hits,
    cache_stats.reuse_hits,
    cache_stats.fallbacks,
    (provider.stats->>'extract_occurrence')::bigint
        AS extract_occurrence,
    (provider.stats->>'late_fetch_batch')::bigint AS late_fetch_batch,
    (provider.stats->>'late_fetch_references')::bigint
        AS late_fetch_references,
    (provider.stats->>'late_fetch_values')::bigint AS late_fetch_values,
    (provider.stats->>'executor_rows')::bigint AS executor_rows,
    (provider.stats->>'gated_expensive_rows')::bigint
        AS gated_expensive_rows,
    (provider.stats->>'last_boundary_column_count')::bigint
        AS boundary_column_count,
    (provider.stats->>'last_required_boundary_count')::bigint
        AS required_boundary_count,
    (provider.stats->>'last_lineage_column_count')::bigint
        AS lineage_column_count,
    (provider.stats->>'last_required_lineage_count')::bigint
        AS required_lineage_count,
    (provider.stats->>'last_recipe_required_boundary_count')::bigint
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
\gset older_stats_

WITH failed AS (
    SELECT
        :older_stats_produced::bigint AS produced,
        :older_stats_reuse_hits::bigint AS reuse_hits,
        :older_stats_fallbacks::bigint AS fallbacks,
        :older_stats_extract_occurrence::bigint AS extract_occurrence,
        :older_stats_late_fetch_batch::bigint AS late_fetch_batch,
        :older_stats_late_fetch_references::bigint AS late_fetch_references,
        :older_stats_late_fetch_values::bigint AS late_fetch_values,
        :older_stats_executor_rows::bigint AS executor_rows,
        :older_stats_gated_expensive_rows::bigint AS gated_expensive_rows
    WHERE :older_stats_produced::bigint <> 1
       OR :older_stats_completed_hits::bigint <> 0
       OR :older_stats_reuse_hits::bigint <> 2
       OR :older_stats_fallbacks::bigint <> 0
       OR :older_stats_extract_occurrence::bigint <> 1
       OR :older_stats_late_fetch_batch::bigint <> 2
       OR :older_stats_late_fetch_references::bigint <> 8
       OR :older_stats_late_fetch_values::bigint <> 8
       OR :older_stats_executor_rows::bigint <> 1
       OR :older_stats_gated_expensive_rows::bigint <> 1
       OR :older_stats_boundary_column_count::bigint <> 6
       OR :older_stats_required_boundary_count::bigint <> 1
       OR :older_stats_lineage_column_count::bigint <> 3
       OR :older_stats_required_lineage_count::bigint <> 3
       OR :older_stats_recipe_required_boundary_count::bigint <> 1
       OR :older_stats_reader_pins::bigint <> 0
       OR :older_stats_waiter_references::bigint <> 0
       OR :older_stats_admission_reservations::bigint <> 0
)
SELECT ('older reusable two-source mismatch: '
        || row_to_json(failed)::text)::bigint
FROM failed;

DROP TABLE cache_provider_older_rows;

\o
SELECT 'cache_reuse_provider_older: ok';
