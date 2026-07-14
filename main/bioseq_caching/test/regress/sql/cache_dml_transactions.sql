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

DO $$
BEGIN
    IF (SELECT array_agg(id ORDER BY id) FROM qc_copy_routing)
       IS DISTINCT FROM ARRAY[1,101]
       OR NOT EXISTS (
           SELECT 1 FROM bioseq_cache.stats()
           WHERE produced = 1 AND completed_hits = 2)
    THEN
        RAISE EXCEPTION 'rolled-back routed COPY invalidated or leaked rows';
    END IF;
END
$$;

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

DO $$
DECLARE
    dependency_limit integer;
    leaf_count integer;
    query_sql text;
    plan_text text;
    plan jsonb;
    cache_node jsonb;
    actual_dependency_count integer;
    planned_leaf_count integer;
    first_rows bigint;
    first_total bigint;
    second_rows bigint;
    second_total bigint;
    expected_total bigint;
    cache_stats record;
BEGIN
    FOREACH dependency_limit IN ARRAY ARRAY[1023, 1024, 1025] LOOP
        leaf_count := dependency_limit - 1;
        expected_total := leaf_count::bigint * (leaf_count + 1)::bigint / 2;
        query_sql := format(
            'SELECT count(*)::bigint, sum(partition_key)::bigint '
            'FROM qc_dependency_partitioned WHERE partition_key <= %s',
            leaf_count);

        PERFORM set_config(
            'bioseq.query_cache_optimisation_mode', 'disabled', true);
        EXECUTE 'EXPLAIN (FORMAT JSON, COSTS OFF) ' || query_sql
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
        SELECT count(*) INTO planned_leaf_count
        FROM plan_nodes
        WHERE node->>'Relation Name' LIKE 'qc_dependency_leaf_%';
        IF planned_leaf_count <> leaf_count THEN
            RAISE EXCEPTION
                'dependency fixture planned % leaves, expected % for limit %',
                planned_leaf_count, leaf_count, dependency_limit;
        END IF;

        PERFORM set_config(
            'bioseq.query_cache_optimisation_mode', 'same', true);
        PERFORM bioseq_cache.invalidate_all();
        PERFORM bioseq_cache.reset_stats();
        EXECUTE 'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) ' || query_sql
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
        SELECT node INTO cache_node
        FROM plan_nodes
        WHERE node->>'Node Type' = 'Custom Scan'
          AND node->>'Custom Plan Provider' = 'BioseqCacheScan'
        LIMIT 1;
        actual_dependency_count := CASE
            WHEN cache_node IS NULL THEN NULL
            ELSE (cache_node->>'Cache Dependency Count')::integer
        END;

        IF dependency_limit <= 1024 THEN
            IF cache_node IS NULL
               OR actual_dependency_count IS DISTINCT FROM dependency_limit THEN
                RAISE EXCEPTION
                    'bounded dependency metadata mismatch: expected %, actual %, wrapped %',
                    dependency_limit,
                    actual_dependency_count,
                    cache_node IS NOT NULL;
            END IF;
        ELSIF cache_node IS NOT NULL THEN
            RAISE EXCEPTION
                '1025-dependency plan was wrapped with % dependencies',
                actual_dependency_count;
        END IF;

        EXECUTE query_sql INTO first_rows, first_total;
        EXECUTE query_sql INTO second_rows, second_total;
        SELECT * INTO cache_stats FROM bioseq_cache.stats();
        IF first_rows <> leaf_count
           OR second_rows <> leaf_count
           OR first_total <> expected_total
           OR second_total <> expected_total THEN
            RAISE EXCEPTION
                'dependency boundary result mismatch at %: %, %, %, %',
                dependency_limit,
                first_rows, first_total, second_rows, second_total;
        END IF;

        IF dependency_limit <= 1024 THEN
            IF cache_stats.produced <> 1
               OR cache_stats.completed_hits <> 1
               OR cache_stats.inflight_waits <> 0
               OR cache_stats.fallbacks <> 0 THEN
                RAISE EXCEPTION
                    'bounded dependency execution mismatch at %: %',
                    dependency_limit, row_to_json(cache_stats);
            END IF;
        ELSIF cache_stats.produced <> 0
              OR cache_stats.completed_hits <> 0
              OR cache_stats.inflight_waits <> 0
              OR cache_stats.fallbacks <> 0
              OR EXISTS (
                    SELECT 1 FROM bioseq_cache_test.entry_states()
                    WHERE state IN ('BUILDING', 'COMPLETE')) THEN
            RAISE EXCEPTION
                '1025-dependency bypass touched shared state: %',
                row_to_json(cache_stats);
        END IF;
    END LOOP;
END
$$;

RESET max_parallel_workers_per_gather;
DROP TABLE qc_dependency_partitioned;

TRUNCATE qc_dml;
SELECT count(*) FROM qc_dml;

\o
SELECT 'cache_dml_transactions: ok';
