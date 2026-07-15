\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'reuse';
SET bioseq.query_cache_ttl = '60 min';

CREATE TABLE cache_provider_rows(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO cache_provider_rows VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0,    'zero', 3),
    (3, 12, 0, 5,    'first match', 3),
    (4, 12, 1, 5,    'second match', 3),
    (5, 13, 0, 10,   'ten', 3),
    (6, 14, 0, 20,   'twenty', 3);

SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation('cache_provider_rows');
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

DO $$
DECLARE
    plan_line text;
    plan_text text := '';
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF) '
        'SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask '
        'FROM cache_provider_rows WHERE nullable_value <= 10'
    LOOP
        plan_text := plan_text || plan_line || E'\n';
    END LOOP;
    IF position('BioseqCacheGenericTestProvider' in plan_text) = 0 THEN
        RAISE EXCEPTION 'generic provider CustomScan was not chosen: %', plan_text;
    END IF;
    IF plan_text !~
            'BioseqReusableCoverageScan\) on public\.cache_provider_rows' THEN
        RAISE EXCEPTION
            'reusable coverage scan lost its relation identity: %', plan_text;
    END IF;
END
$$;

-- Prove a current reusable generation is produced once and replayed without
-- entering the provider's gated executor on the identical second request.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
DO $$
DECLARE
    plan_line text;
    plan_text text := '';
    plan jsonb;
    cache_node jsonb;
    cache_nodes jsonb;
    exact_node_count bigint;
    reusable_node_count bigint;
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF) '
        'SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask '
        'FROM cache_provider_rows WHERE nullable_value <= 10 ORDER BY row_id'
    LOOP
        plan_text := plan_text || plan_line || E'\n';
    END LOOP;
    IF position('BioseqReusableCoverageScan' in plan_text) = 0
       OR position('Cache Entry Kind: ReusableCoverage' in plan_text) = 0 THEN
        RAISE EXCEPTION 'reusable-current plan proof is absent: %', plan_text;
    END IF;
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask '
        'FROM cache_provider_rows WHERE nullable_value <= 10 ORDER BY row_id'
        INTO plan_text;
    plan := plan_text::jsonb;
    WITH RECURSIVE plan_nodes(node) AS (
        SELECT plan->0->'Plan'
        UNION ALL
        SELECT child.value
        FROM plan_nodes
        CROSS JOIN LATERAL jsonb_array_elements(
            COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
    )
    SELECT node INTO cache_node
    FROM plan_nodes
    WHERE node->>'Cache Mode' = 'reuse'
      AND node->>'Cache Entry Kind' = 'ReusableCoverage'
    LIMIT 1;
    IF cache_node IS NULL
       OR cache_node->>'Planned Cache Action' IS NULL
       OR cache_node ? 'Actual Cache Action'
       OR length(cache_node->>'Cache Entry') <> 16
       OR cache_node->>'Schema' <> 'public'
       OR cache_node->>'Relation Name' <> 'cache_provider_rows'
       OR cache_node->>'Alias' <> 'cache_provider_rows'
       OR cache_node->>'SQL' IS NULL
       OR cache_node->>'State' <> 'planned/provisional'
       OR jsonb_typeof(
            cache_node->'Selected Cache Branches') <> 'number'
       OR jsonb_typeof(cache_node->'Residual Child') <> 'object'
       OR cache_node->'Provider'->>'Generic Provider' IS NULL THEN
        RAISE EXCEPTION
            'structured reusable EXPLAIN contract is incomplete: %', plan;
    END IF;
    WITH RECURSIVE plan_nodes(node, parent_path) AS (
        SELECT plan->0->'Plan', ARRAY[]::text[]
        UNION ALL
        SELECT child.value,
               plan_nodes.parent_path
                   || COALESCE(plan_nodes.node->>'Node Type', '<unknown>')
        FROM plan_nodes
        CROSS JOIN LATERAL jsonb_array_elements(
            COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
    )
    SELECT
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ExactFinalResult'),
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'),
        jsonb_agg(jsonb_build_object(
        'cache_entry_kind', node->>'Cache Entry Kind',
        'node_type', node->>'Node Type',
        'parent_path', to_jsonb(parent_path),
        'child_node_types', COALESCE((
            SELECT jsonb_agg(child.value->>'Node Type')
            FROM jsonb_array_elements(
                COALESCE(node->'Plans', '[]'::jsonb)) AS child
        ), '[]'::jsonb),
        'child_paths', COALESCE((
            SELECT jsonb_agg(to_jsonb(
                parent_path
                    || COALESCE(node->>'Node Type', '<unknown>')
                    || COALESCE(child.value->>'Node Type', '<unknown>')))
            FROM jsonb_array_elements(
                COALESCE(node->'Plans', '[]'::jsonb)) AS child
        ), '[]'::jsonb),
        'cache_force_bypass', node->>'Cache Force Bypass'))
    INTO exact_node_count, reusable_node_count, cache_nodes
    FROM plan_nodes
    WHERE node->>'Cache Entry Kind' IN (
        'ExactFinalResult', 'ReusableCoverage');
    IF exact_node_count <> 0 OR reusable_node_count <> 1
       OR plan->0->'Plan'->>'Cache Entry Kind' <> 'ReusableCoverage' THEN
        RAISE EXCEPTION
            'cold reusable plan cache-kind counts exact=%, reusable=%: %',
            exact_node_count, reusable_node_count, cache_nodes;
    END IF;
END
$$;

-- Planning diagnostics can legitimately bypass exact-result caching. Reset
-- observable counters after proving the plan and before measuring execution.
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

\o
SELECT row_id, base_id, occurrence_no, nullable_value, payload,
       capability_mask
FROM cache_provider_rows
WHERE nullable_value <= 10
ORDER BY row_id;
SELECT jsonb_build_object(
    'produced', cache_stats.produced,
    'completed_hits', cache_stats.completed_hits,
    'reuse_hits', cache_stats.reuse_hits,
    'fallbacks', cache_stats.fallbacks,
    'extract_occurrence',
        (provider.stats->>'extract_occurrence')::bigint,
    'executor_rows', (provider.stats->>'executor_rows')::bigint,
    'gated_expensive_rows',
        (provider.stats->>'gated_expensive_rows')::bigint)
FROM bioseq_cache.stats() AS cache_stats
CROSS JOIN LATERAL (
    SELECT bioseq_cache_test.provider_stats() AS stats
) AS provider;

SELECT row_id, base_id, occurrence_no, nullable_value, payload,
       capability_mask
FROM cache_provider_rows
WHERE nullable_value <= 10
ORDER BY row_id;
SELECT jsonb_build_object(
    'produced', cache_stats.produced,
    'completed_hits', cache_stats.completed_hits,
    'reuse_hits', cache_stats.reuse_hits,
    'fallbacks', cache_stats.fallbacks,
    'extract_occurrence',
        (provider.stats->>'extract_occurrence')::bigint,
    'executor_rows', (provider.stats->>'executor_rows')::bigint,
    'gated_expensive_rows',
        (provider.stats->>'gated_expensive_rows')::bigint)
FROM bioseq_cache.stats() AS cache_stats
CROSS JOIN LATERAL (
    SELECT bioseq_cache_test.provider_stats() AS stats
) AS provider;

-- A selected reusable final path suppresses the outer exact wrapper.  The
-- second request must replay coverage without entering its retained child.
DO $$
DECLARE
    cache_stats record;
    provider_stats jsonb := bioseq_cache_test.provider_stats();
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 0
       OR cache_stats.reuse_hits <> 1
       OR cache_stats.fallbacks <> 0
       OR (provider_stats->>'executor_rows')::bigint <> 4
       OR (provider_stats->>'gated_expensive_rows')::bigint <> 4
       OR (provider_stats->>'extract_occurrence')::bigint <> 4 THEN
        RAISE EXCEPTION
            'reusable replay entered its retained child: %, %',
            row_to_json(cache_stats), provider_stats;
    END IF;
END
$$;

DO $$
DECLARE
    stats jsonb := bioseq_cache_test.provider_stats();
BEGIN
    IF (stats->>'last_boundary_column_count')::bigint <> 6
       OR (stats->>'last_required_boundary_count')::bigint <> 6
       OR (stats->>'last_lineage_column_count')::bigint <> 3
       OR (stats->>'last_required_lineage_count')::bigint <> 3
       OR (stats->>'last_recipe_required_boundary_count')::bigint <> 6 THEN
        RAISE EXCEPTION 'all-column provider contract mismatch: %', stats;
    END IF;
END
$$;
\o /dev/null

-- An ordinary main-tree SubqueryScan owns the coverage lifecycle and remains
-- enabled.  A separate InitPlan in the same statement is permanently bypassed.
DO $$
DECLARE
    plan_text text;
    plan jsonb;
    coverage_node_count bigint;
    enabled_coverage_count bigint;
    bypassed_coverage_count bigint;
    observed_ids bigint[];
BEGIN
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT nested.row_id FROM ('
        '  SELECT row_id FROM ONLY cache_provider_rows '
        '  WHERE nullable_value <= 10 OFFSET 0'
        ') AS nested ORDER BY nested.row_id'
        INTO plan_text;
    plan := plan_text::jsonb;
    WITH RECURSIVE plan_nodes(node) AS (
        SELECT plan->0->'Plan'
        UNION ALL
        SELECT child.value
        FROM plan_nodes
        CROSS JOIN LATERAL jsonb_array_elements(
            COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
    )
    SELECT
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'),
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'
              AND NOT (node->>'Cache Force Bypass')::boolean),
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'
              AND (node->>'Cache Force Bypass')::boolean)
    INTO coverage_node_count, enabled_coverage_count,
         bypassed_coverage_count
    FROM plan_nodes;
    IF coverage_node_count <> 1 OR enabled_coverage_count <> 1
       OR bypassed_coverage_count <> 0
       OR position('Subquery Scan' in plan_text) = 0 THEN
        RAISE EXCEPTION
            'main-tree SubqueryScan coverage marker is wrong: %', plan;
    END IF;
    EXECUTE
        'SELECT array_agg(nested.row_id ORDER BY nested.row_id) FROM ('
        '  SELECT row_id FROM ONLY cache_provider_rows '
        '  WHERE nullable_value <= 10 OFFSET 0'
        ') AS nested'
        INTO observed_ids;
    IF observed_ids IS DISTINCT FROM ARRAY[2, 3, 4, 5]::bigint[] THEN
        RAISE EXCEPTION
            'main-tree SubqueryScan changed rows: %', observed_ids;
    END IF;

    -- The InitPlan may legitimately select its ordinary index path.  The
    -- selected main-tree SubqueryScan coverage node was asserted above.
    EXECUTE
        'SELECT array_agg(nested.row_id ORDER BY nested.row_id) FROM ('
        '  SELECT row_id FROM ONLY cache_provider_rows '
        '  WHERE nullable_value <= 10 OFFSET 0'
        ') AS nested '
        'WHERE nested.row_id <= ('
        '  SELECT max(row_id) FROM ONLY cache_provider_rows '
        '  WHERE nullable_value <= 10)'
        INTO observed_ids;
    IF observed_ids IS DISTINCT FROM ARRAY[2, 3, 4, 5]::bigint[] THEN
        RAISE EXCEPTION
            'mixed main/subplan coverage changed rows: %', observed_ids;
    END IF;
END
$$;

-- A provider reachable only through an InitPlan referenced by a join filter
-- must still contribute one finalized exact recipe.  A bad nested recipe
-- bypasses the exact root; the reusable subplan remains visibly force-bypassed.
CREATE TABLE cache_recipe_left(id integer PRIMARY KEY);
CREATE TABLE cache_recipe_right(id integer PRIMARY KEY);
INSERT INTO cache_recipe_left VALUES (1), (2), (3);
INSERT INTO cache_recipe_right VALUES (1), (2), (3);
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

DO $$
DECLARE
    plan_text text;
    plan jsonb;
    coverage_node_count bigint;
    bypassed_coverage_count bigint;
BEGIN
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT l.id, r.id FROM cache_recipe_left l '
        'LEFT JOIN cache_recipe_right r '
        'ON l.id = r.id AND l.id + r.id <= '
        '(SELECT sum(row_id) FROM cache_provider_rows '
        ' WHERE nullable_value <= 10) ORDER BY l.id'
        INTO plan_text;
    plan := plan_text::jsonb;
    WITH RECURSIVE plan_nodes(node) AS (
        SELECT plan->0->'Plan'
        UNION ALL
        SELECT child.value
        FROM plan_nodes
        CROSS JOIN LATERAL jsonb_array_elements(
            COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
    )
    SELECT
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'),
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'
              AND (node->>'Cache Force Bypass')::boolean)
    INTO coverage_node_count, bypassed_coverage_count
    FROM plan_nodes;
    IF plan->0->'Plan'->>'Cache Entry Kind' <> 'ExactFinalResult'
       OR (plan->0->'Plan'->>'Cache Force Bypass')::boolean
       OR (plan->0->'Plan'->>'Provider Runtime Recipes')::integer <> 1
       OR coverage_node_count <> 1
       OR bypassed_coverage_count <> 1
       OR position('BioseqCacheGenericTestProvider' in plan_text) = 0
       OR position('InitPlan' in plan_text) = 0
       OR position('Join Filter' in plan_text) = 0 THEN
        RAISE EXCEPTION
            'nested provider recipe was not captured from join/initplan: %',
            plan;
    END IF;
END
$$;

-- If top exact dependency discovery declines while coverage exists only in a
-- separate InitPlan, a permanent internal wrapper still finalizes its gate
-- Param IDs.  Both layers must remain registry-free passthroughs.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();
SELECT bioseq_cache_test.set_exact_plan_fault(
    'dependency_finalization_declined');
DO $$
DECLARE
    plan_text text;
    plan jsonb;
    coverage_node_count bigint;
    bypassed_coverage_count bigint;
    first_ids integer[];
    second_ids integer[];
    cache_stats record;
    active_entries bigint;
    nested_query constant text :=
        'SELECT array_agg(COALESCE(joined.right_id, -1) '
        'ORDER BY joined.left_id) FROM ('
        'SELECT l.id AS left_id, r.id AS right_id '
        'FROM cache_recipe_left l LEFT JOIN cache_recipe_right r '
        'ON l.id = r.id AND l.id + r.id <= '
        '(SELECT sum(row_id) FROM cache_provider_rows '
        ' WHERE nullable_value <= 10)) AS joined';
BEGIN
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT l.id FROM cache_recipe_left l '
        'LEFT JOIN cache_recipe_right r '
        'ON l.id = r.id AND l.id + r.id <= '
        '(SELECT sum(row_id) FROM cache_provider_rows '
        ' WHERE nullable_value <= 10) ORDER BY l.id'
        INTO plan_text;
    plan := plan_text::jsonb;
    WITH RECURSIVE plan_nodes(node) AS (
        SELECT plan->0->'Plan'
        UNION ALL
        SELECT child.value
        FROM plan_nodes
        CROSS JOIN LATERAL jsonb_array_elements(
            COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
    )
    SELECT
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'),
        count(*) FILTER (
            WHERE node->>'Cache Entry Kind' = 'ReusableCoverage'
              AND (node->>'Cache Force Bypass')::boolean)
    INTO coverage_node_count, bypassed_coverage_count
    FROM plan_nodes;
    IF plan->0->'Plan'->>'Cache Entry Kind' <> 'ExactFinalResult'
       OR NOT (plan->0->'Plan'->>'Cache Force Bypass')::boolean
       OR plan->0->'Plan'->>'Cache Bypass Reason'
            <> 'internal nested-coverage gate finalizer'
       OR coverage_node_count <> 1
       OR bypassed_coverage_count <> 1
       OR position('InitPlan' in plan_text) = 0 THEN
        RAISE EXCEPTION
            'nested gate finalizer plan is unsafe: %', plan;
    END IF;

    EXECUTE nested_query INTO first_ids;
    EXECUTE nested_query INTO second_ids;
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT count(*) INTO active_entries
    FROM bioseq_cache_test.entry_states()
    WHERE state IN ('CLAIMED', 'BUILDING', 'COMPLETE');
    IF first_ids IS DISTINCT FROM ARRAY[1, 2, 3]
       OR second_ids IS DISTINCT FROM first_ids
       OR cache_stats.produced <> 0
       OR cache_stats.completed_hits <> 0
       OR cache_stats.inflight_waits <> 0
       OR cache_stats.reuse_hits <> 0
       -- Each execution visits both the permanent exact gate-finalizer
       -- wrapper and the permanent InitPlan coverage wrapper.
       OR cache_stats.fallbacks <> 4
       OR active_entries <> 0 THEN
        RAISE EXCEPTION
            'nested gate finalizer touched cache or rows: %, %, %, %, %',
            first_ids, second_ids, row_to_json(cache_stats),
            active_entries, plan;
    END IF;
END
$$;
SELECT bioseq_cache_test.set_exact_plan_fault('none');

SELECT bioseq_cache_test.set_exact_recipe_fault('expected_digest_mismatch');
DO $$
DECLARE
    first_ids integer[];
    second_ids integer[];
    first_provider_rows bigint;
    second_provider_rows bigint;
    cache_stats record;
    nested_query constant text :=
        'SELECT array_agg(COALESCE(joined.right_id, -1) '
        'ORDER BY joined.left_id) FROM ('
        'SELECT l.id AS left_id, r.id AS right_id '
        'FROM cache_recipe_left l LEFT JOIN cache_recipe_right r '
        'ON l.id = r.id AND l.id + r.id <= '
        '(SELECT sum(row_id) FROM cache_provider_rows '
        ' WHERE nullable_value <= 10)) AS joined';
BEGIN
    EXECUTE nested_query INTO first_ids;
    first_provider_rows := (
        bioseq_cache_test.provider_stats()->>'executor_rows')::bigint;
    EXECUTE nested_query INTO second_ids;
    second_provider_rows := (
        bioseq_cache_test.provider_stats()->>'executor_rows')::bigint;
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF first_ids IS DISTINCT FROM ARRAY[1, 2, 3]
       OR second_ids IS DISTINCT FROM first_ids
       OR first_provider_rows <= 0
       OR second_provider_rows <= first_provider_rows
       OR cache_stats.completed_hits <> 0
       OR cache_stats.reuse_hits <> 0
       OR cache_stats.produced <> 0
       -- Four fallbacks carry over from the gate-finalizer proof.  Each of
       -- these two executions then adds one exact recipe bypass and one
       -- permanent InitPlan coverage bypass.
       OR cache_stats.fallbacks <> 8 THEN
        RAISE EXCEPTION
            'nested recipe did not fail open through reusable child: %, %, %, %, %',
            first_ids, second_ids, first_provider_rows,
            second_provider_rows, row_to_json(cache_stats);
    END IF;
END
$$;
SELECT bioseq_cache_test.set_exact_recipe_fault('none');

-- Exact roots may legitimately coexist with reusable coverage in an InitPlan.
-- The complete frozen coverage semantic frame must therefore partition the
-- outer exact key when provider-defined security identity changes.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();
SELECT bioseq_cache_test.provider_set_security_digest(
    decode(repeat('11', 32), 'hex'));
PREPARE cache_nested_security AS
SELECT array_agg(COALESCE(joined.right_id, -1) ORDER BY joined.left_id)
FROM (
    SELECT l.id AS left_id, r.id AS right_id
    FROM cache_recipe_left AS l
    LEFT JOIN cache_recipe_right AS r
      ON l.id = r.id
     AND l.id + r.id <= (
        SELECT sum(row_id)
        FROM cache_provider_rows
        WHERE nullable_value <= 10)
) AS joined;
EXECUTE cache_nested_security;
EXECUTE cache_nested_security;
SELECT bioseq_cache_test.provider_set_security_digest(
    decode(repeat('22', 32), 'hex'));
EXECUTE cache_nested_security;
EXECUTE cache_nested_security;
DO $$
DECLARE
    cache_stats record;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 2
       OR cache_stats.completed_hits <> 2
       OR cache_stats.reuse_hits <> 0
       -- Every prepared execution records one permanent InitPlan coverage
       -- bypass while the exact root produces or replays normally.
       OR cache_stats.fallbacks <> 4 THEN
        RAISE EXCEPTION
            'nested coverage semantics did not partition exact key: %',
            row_to_json(cache_stats);
    END IF;
END
$$;
DEALLOCATE cache_nested_security;
SELECT bioseq_cache_test.provider_clear_security_digest();

-- A retained coverage alternative must not suppress exact caching when the
-- final path instead chooses PostgreSQL's ordered index scan.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();
SET random_page_cost = 100;
SET enable_sort = off;
DO $$
DECLARE
    plan_text text;
    plan jsonb;
    provider_stats jsonb;
BEGIN
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT row_id, base_id, occurrence_no, nullable_value, payload, '
        'capability_mask FROM cache_provider_rows '
        'WHERE row_id > 0 ORDER BY row_id'
        INTO plan_text;
    plan := plan_text::jsonb;
    provider_stats := bioseq_cache_test.provider_stats();
    IF plan->0->'Plan'->>'Cache Entry Kind' <> 'ExactFinalResult'
       OR COALESCE(
            (plan->0->'Plan'->>'Cache Force Bypass')::boolean,
            true)
       OR position('BioseqReusableCoverageScan' in plan_text) <> 0
       OR position('Index Scan' in plan_text) = 0
       OR (provider_stats->>'decorate_path')::bigint = 0 THEN
        RAISE EXCEPTION
            'losing coverage alternative suppressed the exact path: %, %',
            plan, provider_stats;
    END IF;
END
$$;

SELECT row_id, base_id, occurrence_no, nullable_value, payload,
       capability_mask
FROM cache_provider_rows
WHERE row_id > 0
ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload,
       capability_mask
FROM cache_provider_rows
WHERE row_id > 0
ORDER BY row_id;

DO $$
DECLARE
    cache_stats record;
    provider_stats jsonb;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    provider_stats := bioseq_cache_test.provider_stats();
    IF cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 1
       OR cache_stats.reuse_hits <> 0
       OR cache_stats.fallbacks <> 0
       OR (provider_stats->>'executor_rows')::bigint <> 0
       OR (provider_stats->>'extract_occurrence')::bigint <> 0 THEN
        RAISE EXCEPTION
            'chosen exact path did not produce and replay cleanly: %, %',
            row_to_json(cache_stats), provider_stats;
    END IF;
END
$$;
RESET enable_sort;
RESET random_page_cost;

-- Keep the remaining provider-contract cases independent of the hit proof.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.reset_stats();

DO $$
DECLARE
    ids bigint[];
    occurrences integer[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_rows
    WHERE nullable_value <= 10;
    IF ids <> ARRAY[2,3,4,5]::bigint[] THEN
        RAISE EXCEPTION 'nullable truth-table P1 mismatch: %', ids;
    END IF;

    SELECT array_agg(occurrence_no ORDER BY occurrence_no)
    INTO occurrences
    FROM cache_provider_rows
    WHERE base_id = 12 AND nullable_value >= 5;
    IF occurrences <> ARRAY[0,1]::integer[] THEN
        RAISE EXCEPTION 'one-to-many occurrence identity was lost: %', occurrences;
    END IF;
END
$$;

-- Build a P1 generation, then consume the overlapping P2 region.  P2 must
-- return overlap plus residual exactly once under SQL three-valued logic.
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_provider_rows
WHERE nullable_value <= 10
ORDER BY row_id;

DO $$
DECLARE
    ids bigint[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_rows
    WHERE nullable_value >= 5;
    IF ids <> ARRAY[3,4,5,6]::bigint[] THEN
        RAISE EXCEPTION 'overlap/residual bag mismatch: %', ids;
    END IF;
END
$$;

-- Full-key equality must survive a forced digest collision across relations.
CREATE TABLE cache_provider_collision(LIKE cache_provider_rows INCLUDING ALL);
INSERT INTO cache_provider_collision VALUES
    (101, 110, 0, 5, 'other relation', 3);
SELECT bioseq_cache_test.set_forced_coverage_domain_digest(
    decode(repeat('42', 32), 'hex'));
SELECT bioseq_cache_test.provider_force_domain_collision(true);
SELECT bioseq_cache_test.provider_use_relation('cache_provider_collision');
DO $$
DECLARE
    ids bigint[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_collision
    WHERE nullable_value >= 5;
    IF ids <> ARRAY[101]::bigint[] THEN
        RAISE EXCEPTION 'forced digest collision crossed domain keys: %', ids;
    END IF;
END
$$;
SELECT bioseq_cache_test.clear_forced_coverage_domain_digest();
SELECT bioseq_cache_test.provider_force_domain_collision(false);
SELECT bioseq_cache_test.provider_use_relation('cache_provider_rows');

-- Every malformed mapping is fail-open: the retained provider path must
-- still return the disabled-mode bag, including provider ERROR.
DO $$
DECLARE
    fault text;
    ids bigint[];
    faults constant text[] := ARRAY[
        'null_pointer_nonzero_count',
        'short_boundary_map',
        'long_boundary_map',
        'short_lineage_map',
        'long_lineage_map',
        'zero_consumer_attno',
        'zero_source_attno',
        'source_attno_out_of_range',
        'duplicate_consumer_attno',
        'missing_required_attno',
        'wrong_type',
        'wrong_typmod',
        'wrong_collation',
        'wrong_semantic_id',
        'same_typed_column_permutation',
        'illegal_repeated_source_attno',
        'bad_provider_private_version',
        'non_integer_persisted_map_node',
        'unknown_persisted_map_version',
        'decorated_boundary_schema_mismatch',
        'decorated_lineage_schema_mismatch',
        'provider_error'
    ];
BEGIN
    FOREACH fault IN ARRAY faults LOOP
        PERFORM bioseq_cache_test.provider_set_mapping_fault(fault);
        SELECT array_agg(row_id ORDER BY row_id)
        INTO ids
        FROM cache_provider_rows
        WHERE nullable_value >= 5;
        IF ids <> ARRAY[3,4,5,6]::bigint[] THEN
            RAISE EXCEPTION 'mapping fault % changed the result bag: %', fault, ids;
        END IF;
    END LOOP;
    PERFORM bioseq_cache_test.provider_set_mapping_fault('none');
END
$$;

-- Freeze a correct prepared plan, then corrupt each serialized source mapping
-- immediately before BeginCustomScan.  Every malformed List must close the
-- cached-source gate and execute the complete retained provider child.
SET plan_cache_mode = force_generic_plan;
\set mapping_fault persisted_wrong_top_tag
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_short_top
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_long_top
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_bad_magic
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_bad_version
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_count_overflow
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_wrong_entry_tag
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_short_entry
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_long_entry
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_bad_semantic_size
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_bad_fingerprint_size
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_bad_entry_metadata
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_source_fingerprint_mismatch
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_consumer_fingerprint_mismatch
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_decorated_fingerprint_mismatch
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\set mapping_fault persisted_map_fingerprint_mismatch
\ir ../../../bioseq_caching/test/regress/sql/cache_reuse_provider_mapping_fault_case.sql
\unset mapping_fault
RESET plan_cache_mode;

-- Capability-superset rejection and plan/runtime digest mismatches also
-- expand to the complete residual without changing rows.
SELECT bioseq_cache_test.provider_set_capabilities(3);
SELECT bioseq_cache_test.provider_set_projection_order('reverse');
SELECT bioseq_cache_test.provider_set_digest_fault('runtime_mismatch');
DO $$
DECLARE
    ids bigint[];
BEGIN
    SELECT array_agg(row_id ORDER BY row_id)
    INTO ids
    FROM cache_provider_rows
    WHERE nullable_value >= 5;
    IF ids <> ARRAY[3,4,5,6]::bigint[] THEN
        RAISE EXCEPTION 'capability/digest fail-open mismatch: %', ids;
    END IF;
END
$$;
SELECT bioseq_cache_test.provider_set_digest_fault('none');
SELECT bioseq_cache_test.provider_set_projection_order('natural');
SELECT bioseq_cache_test.provider_set_capabilities(1);

DO $$
DECLARE
    stats jsonb := bioseq_cache_test.provider_stats();
BEGIN
    IF NOT (stats->>'registered')::boolean
       OR (stats->>'planner_bridge_calls')::bigint = 0
       OR (stats->>'executor_rows')::bigint = 0 THEN
        RAISE EXCEPTION 'generic provider callbacks did not execute: %', stats;
    END IF;
END
$$;

SELECT bioseq_cache_test.provider_shutdown();

\o
SELECT 'cache_reuse_provider: ok';
