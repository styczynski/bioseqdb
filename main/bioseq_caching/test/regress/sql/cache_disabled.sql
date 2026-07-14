\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'disabled';
SELECT bioseq_cache.reset_stats();

CREATE TABLE qc_disabled(id integer PRIMARY KEY, value text NOT NULL);
INSERT INTO qc_disabled VALUES (1, 'a'), (2, 'b');

SELECT * FROM qc_disabled ORDER BY id;

CREATE TEMP TABLE qc_disabled_explain(line text);
DO $$
DECLARE
    plan_line text;
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM qc_disabled ORDER BY id'
    LOOP
        INSERT INTO qc_disabled_explain VALUES (plan_line);
    END LOOP;
END
$$;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM qc_disabled_explain
        WHERE line LIKE '%BioseqCacheScan%')
    THEN
        RAISE EXCEPTION 'disabled plan unexpectedly contains BioseqCacheScan';
    END IF;
    IF (SELECT array_agg(id ORDER BY id) FROM qc_disabled)
       IS DISTINCT FROM ARRAY[1, 2]
    THEN
        RAISE EXCEPTION 'disabled query returned wrong rows';
    END IF;
    IF EXISTS (
        SELECT 1
        FROM bioseq_cache.stats()
        WHERE produced <> 0
           OR completed_hits <> 0
           OR inflight_waits <> 0
           OR reuse_hits <> 0)
    THEN
        RAISE EXCEPTION 'disabled query changed cache counters';
    END IF;
    IF has_schema_privilege('public', 'bioseq_cache', 'CREATE') THEN
        RAISE EXCEPTION 'PUBLIC can create objects in fixed cache schema';
    END IF;
    IF EXISTS (
        SELECT 1
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE n.nspname = 'bioseq_cache'
          AND c.relkind IN ('r', 'p'))
    THEN
        RAISE EXCEPTION 'production cache schema contains a metadata table';
    END IF;
END
$$;

\o
SELECT 'cache_disabled: ok';
