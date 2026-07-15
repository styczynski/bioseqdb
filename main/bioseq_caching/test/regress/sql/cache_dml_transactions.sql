\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'same';
SET bioseq.query_cache_ttl = '60 min';
SET client_min_messages = warning;

CREATE TABLE qc_dml(
    id integer PRIMARY KEY,
    meta integer NOT NULL,
    payload text NOT NULL
);
INSERT INTO qc_dml VALUES
    (1, 3, 'one'), (2, 3, 'two'), (3, 1, 'three');

SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

INSERT INTO qc_dml VALUES (4, 3, 'inserted');
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

UPDATE qc_dml SET payload = 'updated' WHERE id = 1;
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

DELETE FROM qc_dml WHERE id = 2;
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

INSERT INTO qc_dml VALUES (4, 3, 'upsert')
ON CONFLICT (id) DO UPDATE SET payload = EXCLUDED.payload;
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

MERGE INTO qc_dml AS target
USING (VALUES (5, 3, 'merge')) AS source(id, meta, payload)
ON target.id = source.id
WHEN NOT MATCHED THEN
  INSERT VALUES (source.id, source.meta, source.payload);
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

COPY qc_dml FROM STDIN WITH (FORMAT csv);
6,3,copied
\.
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

-- EXPLAIN without ANALYZE is read-only even for a DML statement.  ANALYZE
-- executes that statement, so its ResultRelInfo must still enter the normal
-- write-observation path instead of relying on utility classification.
CREATE TABLE qc_explain_dml(id integer PRIMARY KEY, payload text NOT NULL);
INSERT INTO qc_explain_dml VALUES (1, 'before');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_explain_dml ORDER BY id;
SELECT id, payload FROM qc_explain_dml ORDER BY id;
EXPLAIN (FORMAT JSON, COSTS OFF)
INSERT INTO qc_explain_dml VALUES (2, 'planned only');
SELECT invalidations = 0 AS explain_plan_preserved_cache
FROM bioseq_cache.stats() \gset
SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT invalidations = 0 AS explain_disable_preserved_cache
FROM bioseq_cache.stats() \gset
SELECT count(*) = 1 AS explain_was_read_only FROM qc_explain_dml \gset
SELECT invalidations = 0 AS explain_probe_preserved_cache
FROM bioseq_cache.stats() \gset
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT produced = 1
       AND completed_hits = 1
       AND invalidations = 0 AS explain_preserved_cache
FROM bioseq_cache.stats() \gset
\if :explain_was_read_only
\else
DO $$ BEGIN RAISE EXCEPTION 'plain EXPLAIN executed its DML'; END $$;
\endif
\if :explain_plan_preserved_cache
\else
DO $$ BEGIN RAISE EXCEPTION 'plain EXPLAIN marked its plan dirty'; END $$;
\endif
\if :explain_disable_preserved_cache
\else
DO $$ BEGIN RAISE EXCEPTION 'disabling cache invalidated reusable state'; END $$;
\endif
\if :explain_probe_preserved_cache
\else
DO $$ BEGIN RAISE EXCEPTION 'disabled read probe invalidated reusable state'; END $$;
\endif
\if :explain_preserved_cache
\else
DO $$
DECLARE cache_stats record;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    RAISE EXCEPTION 'plain EXPLAIN invalidated reusable state: %',
        row_to_json(cache_stats);
END $$;
\endif
EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF)
INSERT INTO qc_explain_dml VALUES (2, 'analyzed');
SELECT id, payload FROM qc_explain_dml ORDER BY id;
SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT count(*) = 2 AS explain_analyze_wrote FROM qc_explain_dml \gset
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT produced = 2
       AND completed_hits = 1
       AND invalidations > 0 AS explain_analyze_invalidated
FROM bioseq_cache.stats() \gset
\if :explain_analyze_wrote
\else
DO $$ BEGIN RAISE EXCEPTION 'EXPLAIN ANALYZE did not execute its DML'; END $$;
\endif
\if :explain_analyze_invalidated
\else
DO $$
DECLARE cache_stats record;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    RAISE EXCEPTION 'EXPLAIN ANALYZE DML did not invalidate: %',
        row_to_json(cache_stats);
END $$;
\endif

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

BEGIN;
UPDATE qc_dml SET payload = 'rolled back' WHERE id = 1;
ROLLBACK;

SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'top-level rollback invalidated completed entry';
    END IF;
END
$$;

-- A committed target write selects a new dependency generation; the first
-- post-change execution produces current rows and the second reuses them.
CREATE TABLE qc_generation_rollover(id integer PRIMARY KEY, value text NOT NULL);
INSERT INTO qc_generation_rollover VALUES (1, 'old');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT array_agg(value ORDER BY id) AS values
FROM qc_generation_rollover \gset rollover_before_first_
SELECT array_agg(value ORDER BY id) AS values
FROM qc_generation_rollover \gset rollover_before_second_
UPDATE qc_generation_rollover SET value = 'new' WHERE id = 1;
SELECT array_agg(value ORDER BY id) AS values
FROM qc_generation_rollover \gset rollover_after_first_
SELECT array_agg(value ORDER BY id) AS values
FROM qc_generation_rollover \gset rollover_after_second_
SELECT :'rollover_before_first_values'::text[] = ARRAY['old']
       AND :'rollover_before_second_values'::text[] = ARRAY['old']
       AND :'rollover_after_first_values'::text[] = ARRAY['new']
       AND :'rollover_after_second_values'::text[] = ARRAY['new']
       AND produced = 2 AND completed_hits = 2
       AND fallbacks = 0 AS committed_rollover_ok
FROM bioseq_cache.stats() \gset
\if :committed_rollover_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'committed write generation rollover failed'; END $$;
\endif
DROP TABLE qc_generation_rollover;

-- A rolled-back write to an unrelated relation leaves the COMPLETE target
-- generation usable; this is a hit oracle, not an eager-retirement oracle.
CREATE TABLE qc_rollback_target(id integer PRIMARY KEY, value text NOT NULL);
CREATE TABLE qc_rollback_unrelated(id integer PRIMARY KEY, value text NOT NULL);
INSERT INTO qc_rollback_target VALUES (1, 'stable');
INSERT INTO qc_rollback_unrelated VALUES (1, 'before');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT array_agg(value ORDER BY id) AS values
FROM qc_rollback_target \gset unrelated_before_
BEGIN;
UPDATE qc_rollback_unrelated SET value = 'rolled back' WHERE id = 1;
ROLLBACK;
SELECT array_agg(value ORDER BY id) AS values
FROM qc_rollback_target \gset unrelated_after_
SELECT :'unrelated_before_values'::text[] = ARRAY['stable']
       AND :'unrelated_after_values'::text[] = ARRAY['stable']
       AND produced = 1 AND completed_hits = 1
       AND invalidations = 0 AND fallbacks = 0
       AS unrelated_rollback_hit_ok
FROM bioseq_cache.stats() \gset
\if :unrelated_rollback_hit_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'unrelated rollback did not preserve COMPLETE hit'; END $$;
\endif
DROP TABLE qc_rollback_unrelated;
DROP TABLE qc_rollback_target;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

BEGIN;
SAVEPOINT s1;
UPDATE qc_dml SET payload = 'subxact rollback' WHERE id = 1;
ROLLBACK TO s1;
COMMIT;

SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'savepoint rollback invalidated completed entry';
    END IF;
END
$$;

BEGIN;
SAVEPOINT s2;
UPDATE qc_dml SET payload = 'subxact committed' WHERE id = 1;
RELEASE SAVEPOINT s2;
COMMIT;

SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;

-- A completed entry must not mask writes made by the current transaction.
SELECT count(*) = 5 AS own_write_visible
FROM qc_dml WHERE meta = 3;
SELECT count(*) = 5 AS own_write_visible
FROM qc_dml WHERE meta = 3;

BEGIN;
INSERT INTO qc_dml VALUES (7, 3, 'own write');
SELECT count(*) = 5 AS own_write_visible
FROM qc_dml WHERE meta = 3 \gset
\if :own_write_visible
\else
DO $$
BEGIN
    RAISE EXCEPTION 'completed entry masked the current transaction write';
END
$$;
\endif
SELECT id, payload FROM qc_dml WHERE meta = 3 ORDER BY id;
DO $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM qc_dml WHERE id = 7 AND payload = 'own write') THEN
        RAISE EXCEPTION 'own-write query did not see its row';
    END IF;
END
$$;
ROLLBACK;

CREATE TABLE qc_direct(id integer PRIMARY KEY, value text NOT NULL);
INSERT INTO qc_direct VALUES (1, 'old');
SELECT id, value FROM qc_direct ORDER BY id;
BEGIN;
SELECT bioseq_cache_test.direct_heap_insert('qc_direct', 2, 'direct');
SELECT id, value FROM qc_direct ORDER BY id;
ROLLBACK;

DO $$
BEGIN
    IF (SELECT payload FROM qc_dml WHERE id = 1)
       IS DISTINCT FROM 'subxact committed'
    THEN
        RAISE EXCEPTION 'released savepoint write was not committed';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats() WHERE invalidations > 0)
    THEN
        RAISE EXCEPTION 'committed DML did not invalidate';
    END IF;
END
$$;

CREATE TABLE qc_partitioned(
    id integer NOT NULL,
    payload text NOT NULL
) PARTITION BY RANGE (id);
CREATE TABLE qc_partitioned_lo PARTITION OF qc_partitioned
    FOR VALUES FROM (0) TO (100);
INSERT INTO qc_partitioned VALUES (1, 'parent scan');

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_partitioned ORDER BY id;
SELECT id, payload FROM qc_partitioned ORDER BY id;

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'partition-parent scan did not reuse its result';
    END IF;
END
$$;

INSERT INTO qc_partitioned_lo VALUES (2, 'direct child write');
SELECT id, payload FROM qc_partitioned ORDER BY id;

DO $$
BEGIN
    IF (SELECT count(*) FROM qc_partitioned) <> 2 THEN
        RAISE EXCEPTION 'partition-parent scan missed direct child write';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'direct child write did not invalidate parent result';
    END IF;
END
$$;

-- Tuple routing must observe both the partition parent and the moved-to leaf.
CREATE TABLE qc_row_move(
    id integer NOT NULL,
    payload text NOT NULL
) PARTITION BY RANGE (id);
CREATE TABLE qc_row_move_lo PARTITION OF qc_row_move
    FOR VALUES FROM (0) TO (100);
CREATE TABLE qc_row_move_hi PARTITION OF qc_row_move
    FOR VALUES FROM (100) TO (200);
INSERT INTO qc_row_move VALUES (1, 'before move');

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_row_move ORDER BY id;
SELECT id, payload FROM qc_row_move ORDER BY id;
UPDATE qc_row_move SET id = 101, payload = 'after move' WHERE id = 1;
SELECT id, payload FROM qc_row_move ORDER BY id;

DO $$
BEGIN
    IF (SELECT array_agg(id ORDER BY id) FROM qc_row_move)
       IS DISTINCT FROM ARRAY[101]
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'partition row move left a reusable parent result stale';
    END IF;
END
$$;

-- Trigger-only secondary targets are discovered from live ResultRelInfo state.
CREATE TABLE qc_trigger_source(id integer PRIMARY KEY);
CREATE TABLE qc_trigger_sink(id integer PRIMARY KEY, payload text NOT NULL);
CREATE FUNCTION qc_trigger_copy() RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO qc_trigger_sink VALUES (NEW.id, 'triggered');
    RETURN NEW;
END
$$;
CREATE TRIGGER qc_trigger_copy_after
AFTER INSERT ON qc_trigger_source
FOR EACH ROW EXECUTE FUNCTION qc_trigger_copy();

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_trigger_sink ORDER BY id;
SELECT id, payload FROM qc_trigger_sink ORDER BY id;
INSERT INTO qc_trigger_source VALUES (1);
SELECT id, payload FROM qc_trigger_sink ORDER BY id;

DO $$
BEGIN
    IF (SELECT count(*) FROM qc_trigger_sink) <> 1
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'trigger secondary target was not invalidated';
    END IF;
END
$$;

-- A caught PL/pgSQL exception aborts its SPI subtransaction observation.
CREATE TABLE qc_caught_spi(id integer PRIMARY KEY, payload text NOT NULL);
CREATE FUNCTION qc_caught_spi_write() RETURNS void
LANGUAGE plpgsql VOLATILE AS $$
BEGIN
    BEGIN
        INSERT INTO qc_caught_spi VALUES (1, 'must roll back');
        RAISE EXCEPTION 'caught write';
    EXCEPTION WHEN OTHERS THEN
        NULL;
    END;
END
$$;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_caught_spi ORDER BY id;
SELECT id, payload FROM qc_caught_spi ORDER BY id;
SELECT qc_caught_spi_write();
SELECT id, payload FROM qc_caught_spi ORDER BY id;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM qc_caught_spi)
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 1 AND completed_hits = 2)
    THEN
        RAISE EXCEPTION 'caught SPI subtransaction invalidated committed state';
    END IF;
END
$$;

-- COPY FROM through a partition parent commits and rolls back precisely.
CREATE TABLE qc_copy_routing(
    id integer NOT NULL,
    payload text NOT NULL
) PARTITION BY RANGE (id);
CREATE TABLE qc_copy_routing_lo PARTITION OF qc_copy_routing
    FOR VALUES FROM (0) TO (100);
CREATE TABLE qc_copy_routing_hi PARTITION OF qc_copy_routing
    FOR VALUES FROM (100) TO (200);

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_copy_routing ORDER BY id;
SELECT id, payload FROM qc_copy_routing ORDER BY id;
COPY qc_copy_routing FROM STDIN WITH (FORMAT csv);
1,low
101,high
\.
SELECT id, payload FROM qc_copy_routing ORDER BY id;

DO $$
BEGIN
    IF (SELECT array_agg(id ORDER BY id) FROM qc_copy_routing)
       IS DISTINCT FROM ARRAY[1,101]
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'committed routed COPY did not invalidate';
    END IF;
END
$$;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, payload FROM qc_copy_routing ORDER BY id;
SELECT id, payload FROM qc_copy_routing ORDER BY id;
BEGIN;
COPY qc_copy_routing FROM STDIN WITH (FORMAT csv);
2,rolled back
102,rolled back
\.
ROLLBACK;
SELECT id, payload FROM qc_copy_routing ORDER BY id;
SELECT produced = 1
       AND completed_hits = 2
       AND invalidations = 0 AS copy_rollback_preserved_cache
FROM bioseq_cache.stats() \gset
SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT array_agg(id ORDER BY id) = ARRAY[1,101] AS copy_rollback_rows_ok
FROM qc_copy_routing \gset
SET bioseq.query_cache_optimisation_mode = 'same';
\if :copy_rollback_preserved_cache
\else
DO $$
DECLARE cache_stats record;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    RAISE EXCEPTION
        'rolled-back routed COPY invalidated reusable state: %',
        row_to_json(cache_stats);
END
$$;
\endif
\if :copy_rollback_rows_ok
\else
DO $$ BEGIN
    RAISE EXCEPTION 'rolled-back routed COPY leaked rows';
END $$;
\endif

-- A rolled-back TRUNCATE retains the completed generation.
CREATE TABLE qc_truncate_rollback(id integer PRIMARY KEY);
INSERT INTO qc_truncate_rollback VALUES (1), (2);
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id FROM qc_truncate_rollback ORDER BY id;
SELECT id FROM qc_truncate_rollback ORDER BY id;
BEGIN;
TRUNCATE qc_truncate_rollback;
ROLLBACK;
SELECT id FROM qc_truncate_rollback ORDER BY id;

DO $$
BEGIN
    IF (SELECT count(*) FROM qc_truncate_rollback) <> 2
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 1 AND completed_hits = 2)
    THEN
        RAISE EXCEPTION 'rolled-back TRUNCATE invalidated or leaked state';
    END IF;
END
$$;

-- Inheritance expansion is coarse because the utility node lacks its closure.
CREATE TABLE qc_truncate_parent(id integer PRIMARY KEY);
CREATE TABLE qc_truncate_child() INHERITS (qc_truncate_parent);
INSERT INTO qc_truncate_child VALUES (1);
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id FROM qc_truncate_parent ORDER BY id;
SELECT id FROM qc_truncate_parent ORDER BY id;
TRUNCATE qc_truncate_parent;
SELECT id FROM qc_truncate_parent ORDER BY id;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM qc_truncate_parent)
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'inherited TRUNCATE left a child result stale';
    END IF;
END
$$;

-- CASCADE must invalidate targets not named in the original utility tree.
CREATE TABLE qc_truncate_primary(id integer PRIMARY KEY);
CREATE TABLE qc_truncate_referencing(
    id integer PRIMARY KEY,
    primary_id integer REFERENCES qc_truncate_primary(id)
);
INSERT INTO qc_truncate_primary VALUES (1);
INSERT INTO qc_truncate_referencing VALUES (1, 1);
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, primary_id FROM qc_truncate_referencing ORDER BY id;
SELECT id, primary_id FROM qc_truncate_referencing ORDER BY id;
TRUNCATE qc_truncate_primary CASCADE;
SELECT id, primary_id FROM qc_truncate_referencing ORDER BY id;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM qc_truncate_referencing)
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'TRUNCATE CASCADE left an unnamed target stale';
    END IF;
END
$$;

-- More than the fixed precise-key capacity must switch to coarse invalidation.
DO $$
BEGIN
    FOR relation_index IN 1..258 LOOP
        EXECUTE format(
            'CREATE TABLE qc_pressure_%s(id integer PRIMARY KEY)',
            relation_index);
    END LOOP;
END
$$;
CREATE FUNCTION qc_pressure_fill() RETURNS void
LANGUAGE plpgsql VOLATILE AS $$
BEGIN
    FOR relation_index IN 1..258 LOOP
        EXECUTE format(
            'INSERT INTO qc_pressure_%s VALUES (1)', relation_index);
    END LOOP;
END
$$;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id FROM qc_pressure_258 ORDER BY id;
SELECT id FROM qc_pressure_258 ORDER BY id;
SELECT qc_pressure_fill();
SELECT id FROM qc_pressure_258 ORDER BY id;

DO $$
BEGIN
    IF (SELECT count(*) FROM qc_pressure_258) <> 1
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 2 AND completed_hits = 1)
    THEN
        RAISE EXCEPTION 'write-observation pressure failed coarse invalidation';
    END IF;
END
$$;

-- A partition parent contributes one topology dependency in addition to each
-- selected physical leaf.  The 1025-dependency plan must be rejected before
-- claim/publication instead of truncating its dependency vector.
CREATE TABLE qc_dependency_partitioned(
    partition_key integer NOT NULL,
    payload integer NOT NULL
) PARTITION BY RANGE (partition_key);

DO $$
BEGIN
    FOR leaf_index IN 1..1024 LOOP
        EXECUTE format(
            'CREATE TABLE %I PARTITION OF qc_dependency_partitioned '
            'FOR VALUES FROM (%s) TO (%s)',
            'qc_dependency_leaf_' || leaf_index,
            leaf_index,
            leaf_index + 1);
    END LOOP;
END
$$;

INSERT INTO qc_dependency_partitioned
SELECT value, value * 10
FROM generate_series(1, 1024) AS generated(value);
SET max_parallel_workers_per_gather = 0;

CREATE FUNCTION pg_temp.qc_dependency_plan(query_text text) RETURNS jsonb
LANGUAGE plpgsql VOLATILE AS $$
DECLARE
    plan_text text;
BEGIN
    EXECUTE 'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) ' || query_text
        INTO plan_text;
    RETURN plan_text::jsonb;
END
$$;

\set dep_query_1023 'SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total FROM qc_dependency_partitioned WHERE partition_key <= 1022'
\set dep_query_1024 'SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total FROM qc_dependency_partitioned WHERE partition_key <= 1023'
\set dep_query_1025 'SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total FROM qc_dependency_partitioned WHERE partition_key <= 1024'

SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT pg_temp.qc_dependency_plan(:'dep_query_1023')::text AS plan
\gset dep1023_base_
WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'dep1023_base_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child
    FROM plan_nodes,
         LATERAL jsonb_array_elements(
             COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
)
SELECT count(*) = 1022 AS dep1023_leaf_plan_ok
FROM plan_nodes
WHERE node->>'Relation Name' LIKE 'qc_dependency_leaf_%' \gset
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT pg_temp.qc_dependency_plan(:'dep_query_1023')::text AS plan
\gset dep1023_cache_
SET bioseq.query_cache_optimisation_mode = 'disabled';
WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'dep1023_cache_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child
    FROM plan_nodes,
         LATERAL jsonb_array_elements(
             COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
)
SELECT count(*) = 1
       AND COALESCE(max((node->>'Cache Dependency Count')::integer), -1)
           = 1023 AS dep1023_cache_plan_ok
FROM plan_nodes
WHERE node->>'Node Type' = 'Custom Scan'
  AND node->>'Custom Plan Provider' = 'BioseqCacheScan' \gset
\if :dep1023_leaf_plan_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'dependency fixture did not plan 1022 leaves'; END $$;
\endif
\if :dep1023_cache_plan_ok
\else
DO $$ BEGIN RAISE EXCEPTION '1023-dependency plan metadata mismatch'; END $$;
\endif
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total
FROM qc_dependency_partitioned WHERE partition_key <= 1022 \gset dep1023_first_
SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total
FROM qc_dependency_partitioned WHERE partition_key <= 1022 \gset dep1023_second_
SELECT :dep1023_first_row_count::bigint = 1022
       AND :dep1023_second_row_count::bigint = 1022
       AND :dep1023_first_total::bigint = 522753
       AND :dep1023_second_total::bigint = 522753
       AND produced = 1 AND completed_hits = 1
       AND inflight_waits = 0 AND fallbacks = 0 AS dep1023_execution_ok
FROM bioseq_cache.stats() \gset
\if :dep1023_execution_ok
\else
DO $$ BEGIN RAISE EXCEPTION '1023-dependency execution mismatch'; END $$;
\endif

SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT pg_temp.qc_dependency_plan(:'dep_query_1024')::text AS plan
\gset dep1024_base_
WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'dep1024_base_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child
    FROM plan_nodes,
         LATERAL jsonb_array_elements(
             COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
)
SELECT count(*) = 1023 AS dep1024_leaf_plan_ok
FROM plan_nodes
WHERE node->>'Relation Name' LIKE 'qc_dependency_leaf_%' \gset
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT pg_temp.qc_dependency_plan(:'dep_query_1024')::text AS plan
\gset dep1024_cache_
SET bioseq.query_cache_optimisation_mode = 'disabled';
WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'dep1024_cache_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child
    FROM plan_nodes,
         LATERAL jsonb_array_elements(
             COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
)
SELECT count(*) = 1
       AND COALESCE(max((node->>'Cache Dependency Count')::integer), -1)
           = 1024 AS dep1024_cache_plan_ok
FROM plan_nodes
WHERE node->>'Node Type' = 'Custom Scan'
  AND node->>'Custom Plan Provider' = 'BioseqCacheScan' \gset
\if :dep1024_leaf_plan_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'dependency fixture did not plan 1023 leaves'; END $$;
\endif
\if :dep1024_cache_plan_ok
\else
DO $$ BEGIN RAISE EXCEPTION '1024-dependency plan metadata mismatch'; END $$;
\endif
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total
FROM qc_dependency_partitioned WHERE partition_key <= 1023 \gset dep1024_first_
SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total
FROM qc_dependency_partitioned WHERE partition_key <= 1023 \gset dep1024_second_
SELECT :dep1024_first_row_count::bigint = 1023
       AND :dep1024_second_row_count::bigint = 1023
       AND :dep1024_first_total::bigint = 523776
       AND :dep1024_second_total::bigint = 523776
       AND produced = 1 AND completed_hits = 1
       AND inflight_waits = 0 AND fallbacks = 0 AS dep1024_execution_ok
FROM bioseq_cache.stats() \gset
\if :dep1024_execution_ok
\else
DO $$ BEGIN RAISE EXCEPTION '1024-dependency execution mismatch'; END $$;
\endif

SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT pg_temp.qc_dependency_plan(:'dep_query_1025')::text AS plan
\gset dep1025_base_
WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'dep1025_base_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child
    FROM plan_nodes,
         LATERAL jsonb_array_elements(
             COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
)
SELECT count(*) = 1024 AS dep1025_leaf_plan_ok
FROM plan_nodes
WHERE node->>'Relation Name' LIKE 'qc_dependency_leaf_%' \gset
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT pg_temp.qc_dependency_plan(:'dep_query_1025')::text AS plan
\gset dep1025_cache_
SET bioseq.query_cache_optimisation_mode = 'disabled';
WITH RECURSIVE plan_nodes(node) AS (
    SELECT :'dep1025_cache_plan'::jsonb->0->'Plan'
  UNION ALL
    SELECT child
    FROM plan_nodes,
         LATERAL jsonb_array_elements(
             COALESCE(plan_nodes.node->'Plans', '[]'::jsonb)) AS child
)
SELECT count(*) = 0 AS dep1025_cache_plan_ok
FROM plan_nodes
WHERE node->>'Node Type' = 'Custom Scan'
  AND node->>'Custom Plan Provider' = 'BioseqCacheScan' \gset
\if :dep1025_leaf_plan_ok
\else
DO $$ BEGIN RAISE EXCEPTION 'dependency fixture did not plan 1024 leaves'; END $$;
\endif
\if :dep1025_cache_plan_ok
\else
DO $$ BEGIN RAISE EXCEPTION '1025-dependency plan entered the registry'; END $$;
\endif
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total
FROM qc_dependency_partitioned WHERE partition_key <= 1024 \gset dep1025_first_
SELECT count(*)::bigint AS row_count, sum(partition_key)::bigint AS total
FROM qc_dependency_partitioned WHERE partition_key <= 1024 \gset dep1025_second_
SELECT :dep1025_first_row_count::bigint = 1024
       AND :dep1025_second_row_count::bigint = 1024
       AND :dep1025_first_total::bigint = 524800
       AND :dep1025_second_total::bigint = 524800
       AND produced = 0 AND completed_hits = 0
       AND inflight_waits = 0 AND fallbacks = 0
       AND NOT EXISTS (
           SELECT 1 FROM bioseq_cache_test.entry_states()
           WHERE state IN ('BUILDING', 'COMPLETE')) AS dep1025_execution_ok
FROM bioseq_cache.stats() \gset
\if :dep1025_execution_ok
\else
DO $$ BEGIN RAISE EXCEPTION '1025-dependency bypass touched shared state'; END $$;
\endif

RESET max_parallel_workers_per_gather;
DROP TABLE qc_dependency_partitioned;

TRUNCATE qc_dml;
SELECT count(*) FROM qc_dml;

\o
SELECT 'cache_dml_transactions: ok';
