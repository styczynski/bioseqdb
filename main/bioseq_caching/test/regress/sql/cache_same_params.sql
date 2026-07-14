\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'same';
SET bioseq.query_cache_ttl = '60 min';

CREATE TABLE qc_same(
    id bigint PRIMARY KEY,
    meta integer,
    payload text NOT NULL
);
INSERT INTO qc_same
SELECT i, i % 7, repeat(md5(i::text), 4)
FROM generate_series(1, 100) AS g(i);

SELECT bioseq_cache.reset_stats();

SELECT id, payload
FROM qc_same
WHERE meta = 3
ORDER BY id;

SELECT id, payload
FROM qc_same
WHERE meta = 3
ORDER BY id;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1
          AND completed_hits = 1
          AND inflight_waits = 0)
    THEN
        RAISE EXCEPTION 'same-query producer/hit counters are wrong';
    END IF;
END
$$;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SET plan_cache_mode = force_generic_plan;

PREPARE qc_param(integer) AS
SELECT id FROM qc_same WHERE meta = $1 ORDER BY id;

EXECUTE qc_param(3);
EXECUTE qc_param(3);
EXECUTE qc_param(4);
EXECUTE qc_param(NULL);
EXECUTE qc_param(NULL);

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 3
          AND completed_hits = 2)
    THEN
        RAISE EXCEPTION 'runtime Param values or NULL did not separate keys';
    END IF;
END
$$;

DEALLOCATE qc_param;
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

-- PARAM_EXTERN inventory is derived from expressions, not ParamListInfo size.
-- Changing an unused second client parameter must retain the first key's hit.
PREPARE qc_unused_param(integer, text) AS
SELECT id FROM qc_same WHERE meta = $1 ORDER BY id;

EXECUTE qc_unused_param(3, 'first');
EXECUTE qc_unused_param(3, 'second');
EXECUTE qc_unused_param(4, NULL);
EXECUTE qc_unused_param(4, 'ignored');

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 2
          AND completed_hits = 2
          AND inflight_waits = 0)
    THEN
        RAISE EXCEPTION 'unused second PARAM_EXTERN changed the exact key';
    END IF;
END
$$;

DEALLOCATE qc_unused_param;
RESET plan_cache_mode;
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

SELECT id FROM qc_same WHERE meta = 3;
SELECT  id  FROM qc_same /* comment */ WHERE meta = 3;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'equal analyzed trees did not share';
    END IF;
END
$$;

-- Closing a cursor before child exhaustion must fail its partial producer.
CREATE TABLE qc_cursor_early(id integer PRIMARY KEY, payload text NOT NULL);
INSERT INTO qc_cursor_early VALUES
    (1, 'one'), (2, 'two'), (3, 'three'), (4, 'four');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

BEGIN;
DECLARE qc_early_close NO SCROLL CURSOR FOR
SELECT id, payload FROM qc_cursor_early ORDER BY id;
FETCH 1 FROM qc_early_close;
CLOSE qc_early_close;
COMMIT;

DO $$
DECLARE
    cache_stats record;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 0
       OR cache_stats.completed_hits <> 0
       OR EXISTS (
            SELECT 1 FROM bioseq_cache_test.entry_states()
            WHERE state IN ('BUILDING', 'COMPLETE')) THEN
        RAISE EXCEPTION 'early cursor close published partial state: %',
            row_to_json(cache_stats);
    END IF;
END
$$;

SELECT id, payload FROM qc_cursor_early ORDER BY id;
SELECT id, payload FROM qc_cursor_early ORDER BY id;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1
          AND completed_hits = 1
          AND inflight_waits = 0)
       OR (SELECT count(*) FROM qc_cursor_early) <> 4 THEN
        RAISE EXCEPTION 'early-close retry did not produce and replay fully';
    END IF;
END
$$;

-- The serial exact wrapper must remain above a deterministic Gather child.
CREATE TABLE qc_parallel AS
SELECT value::bigint AS value
FROM generate_series(1, 100000) AS generated(value);
ALTER TABLE qc_parallel SET (parallel_workers = 2);
ANALYZE qc_parallel;
SET max_parallel_workers_per_gather = 2;
SET min_parallel_table_scan_size = 0;
SET parallel_setup_cost = 0;
SET parallel_tuple_cost = 0;
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

SELECT sum(value)::bigint FROM qc_parallel;
SELECT sum(value)::bigint FROM qc_parallel;

DO $$
DECLARE
    cache_stats record;
    observed bigint;
    plan_text text;
    plan jsonb;
    cache_node jsonb;
    gather_count integer;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT sum(value)::bigint FROM qc_parallel'
        INTO plan_text;
    plan := plan_text::jsonb;

    WITH RECURSIVE plan_nodes(node) AS (
        SELECT plan->0->'Plan'
      UNION ALL
        SELECT child
        FROM plan_nodes,
             LATERAL jsonb_array_elements(
                 COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
    )
    SELECT
        (min(node::text) FILTER (
            WHERE node->>'Node Type' = 'Custom Scan'
              AND node->>'Custom Plan Provider' = 'BioseqCacheScan'))::jsonb,
        count(*) FILTER (WHERE node->>'Node Type' = 'Gather')
    INTO cache_node, gather_count
    FROM plan_nodes;

    SELECT sum(value)::bigint INTO observed FROM qc_parallel;
    IF cache_node IS NULL
       OR cache_node IS DISTINCT FROM plan->0->'Plan'
       OR gather_count < 1
       OR observed <> 5000050000
       OR cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 1
       OR cache_stats.inflight_waits <> 0 THEN
        RAISE EXCEPTION
            'serial-wrapper/Gather proof failed: plan %, stats %, sum %',
            plan, row_to_json(cache_stats), observed;
    END IF;
END
$$;

RESET max_parallel_workers_per_gather;
RESET min_parallel_table_scan_size;
RESET parallel_setup_cost;
RESET parallel_tuple_cost;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

SELECT id FROM qc_same WHERE meta = 3 AND id > 0;
SELECT id FROM qc_same WHERE id > 0 AND meta = 3;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 2 AND completed_hits = 0)
    THEN
        RAISE EXCEPTION 'distinct analyzed trees incorrectly shared';
    END IF;
END
$$;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
CREATE TABLE qc_bag(id integer, value integer);
INSERT INTO qc_bag VALUES (1, 9), (2, 9), (3, 10);

SELECT value FROM qc_bag WHERE value = 99;
SELECT value FROM qc_bag WHERE value = 99;
SELECT value FROM qc_bag WHERE value = 9 ORDER BY id;
SELECT value FROM qc_bag WHERE value = 9 ORDER BY id;

DO $$
BEGIN
    IF (SELECT count(*) FROM qc_bag WHERE value = 9) <> 2 THEN
        RAISE EXCEPTION 'duplicate result fixture lost bag multiplicity';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 2 AND completed_hits = 2)
    THEN
        RAISE EXCEPTION 'empty or duplicate completed result did not hit';
    END IF;
END
$$;

\o
SELECT 'cache_same_params: ok';
