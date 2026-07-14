\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'same';

CREATE TABLE qc_elig(id integer PRIMARY KEY, value text);
INSERT INTO qc_elig VALUES (1, 'one'), (2, 'two');
CREATE SEQUENCE qc_sequence;

SELECT bioseq_cache.reset_stats();
SELECT random();
SELECT nextval('qc_sequence');
SELECT clock_timestamp();
SELECT CURRENT_TIMESTAMP;
SELECT CURRENT_DATE;
SELECT LOCALTIME;
SELECT CURRENT_USER;
SELECT current_setting('TimeZone');
SELECT * FROM qc_elig FOR UPDATE;
SELECT * FROM qc_elig FOR SHARE SKIP LOCKED;
SELECT * FROM qc_elig TABLESAMPLE SYSTEM (10);
SELECT tableoid, xmin, xmax, ctid FROM qc_elig ORDER BY id;
SELECT last_value FROM qc_sequence;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM bioseq_cache.stats() WHERE produced <> 0) THEN
        RAISE EXCEPTION 'volatile/stable/locking/system-column query entered cache';
    END IF;
END
$$;

-- Relation-free Result and VALUES plans are positively verified and reusable.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT 40 + 2 AS answer;
SELECT 40 + 2 AS answer;
SELECT *
FROM (VALUES
    (1::bigint, 'alpha'::text, NULL::integer),
    (2::bigint, 'beta'::text, 7::integer))
    AS value_rows(id, label, nullable_value)
ORDER BY id;
SELECT *
FROM (VALUES
    (1::bigint, 'alpha'::text, NULL::integer),
    (2::bigint, 'beta'::text, 7::integer))
    AS value_rows(id, label, nullable_value)
ORDER BY id;

DO $$
DECLARE
    cache_stats record;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 2
       OR cache_stats.completed_hits <> 2
       OR cache_stats.inflight_waits <> 0 THEN
        RAISE EXCEPTION 'relation-free exact reuse counters mismatch: %',
            row_to_json(cache_stats);
    END IF;
    IF (SELECT count(*) FROM bioseq_cache.entries()
        WHERE state = 'COMPLETE') <> 2
       OR EXISTS (
            SELECT 1
            FROM bioseq_cache.entries()
            WHERE state = 'COMPLETE'
              AND (jsonb_array_length(dependencies->'relation_oids') <> 0
                   OR producer_plan IS NOT NULL)) THEN
        RAISE EXCEPTION 'relation-free dependency diagnostics are invalid';
    END IF;
END
$$;

-- Finalized table/index AM rejection must run the retained child with no
-- registry claim, wait, publication, or hit.
SET enable_indexscan = off;
SET enable_indexonlyscan = off;
SET enable_bitmapscan = off;
SELECT bioseq_cache_test.set_exact_plan_fault('unknown_table_am');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
DO $$
DECLARE
    attempt integer;
    observed bigint;
    cache_stats record;
    plan_text text;
    plan jsonb;
BEGIN
    FOR attempt IN 1..2 LOOP
        EXECUTE 'SELECT sum(id)::bigint FROM qc_elig' INTO observed;
        IF observed <> 3 THEN
            RAISE EXCEPTION 'table-AM bypass changed retained-child result: %',
                observed;
        END IF;
    END LOOP;
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT sum(id)::bigint FROM qc_elig'
        INTO plan_text;
    plan := plan_text::jsonb;
    IF COALESCE(
            (plan->0->'Plan'->>'Cache Force Bypass')::boolean,
            false) IS NOT TRUE
       OR plan->0->'Plan'->>'Cache Bypass Reason'
            <> 'unsafe finalized source path' THEN
        RAISE EXCEPTION 'table-AM bypass is absent from EXPLAIN: %', plan;
    END IF;
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 0
       OR cache_stats.completed_hits <> 0
       OR cache_stats.inflight_waits <> 0
       OR cache_stats.fallbacks <> 2
       OR EXISTS (
            SELECT 1 FROM bioseq_cache_test.entry_states()
            WHERE state IN ('BUILDING', 'COMPLETE')) THEN
        RAISE EXCEPTION 'table-AM verifier touched the registry: %',
            row_to_json(cache_stats);
    END IF;
END
$$;

SET enable_indexscan = on;
SET enable_indexonlyscan = on;
SET enable_seqscan = off;
SELECT bioseq_cache_test.set_exact_plan_fault('unknown_index_am');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
DO $$
DECLARE
    attempt integer;
    observed text;
    cache_stats record;
    plan_text text;
    plan jsonb;
BEGIN
    FOR attempt IN 1..2 LOOP
        EXECUTE 'SELECT value FROM qc_elig WHERE id = 1' INTO observed;
        IF observed <> 'one' THEN
            RAISE EXCEPTION 'index-AM bypass changed retained-child result: %',
                observed;
        END IF;
    END LOOP;
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT value FROM qc_elig WHERE id = 1'
        INTO plan_text;
    plan := plan_text::jsonb;
    IF COALESCE(
            (plan->0->'Plan'->>'Cache Force Bypass')::boolean,
            false) IS NOT TRUE
       OR plan->0->'Plan'->>'Cache Bypass Reason'
            <> 'unsafe finalized source path' THEN
        RAISE EXCEPTION 'index-AM bypass is absent from EXPLAIN: %', plan;
    END IF;
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 0
       OR cache_stats.completed_hits <> 0
       OR cache_stats.inflight_waits <> 0
       OR cache_stats.fallbacks <> 2
       OR EXISTS (
            SELECT 1 FROM bioseq_cache_test.entry_states()
            WHERE state IN ('BUILDING', 'COMPLETE')) THEN
        RAISE EXCEPTION 'index-AM verifier touched the registry: %',
            row_to_json(cache_stats);
    END IF;
END
$$;
SELECT bioseq_cache_test.set_exact_plan_fault('none');
RESET enable_seqscan;
RESET enable_indexscan;
RESET enable_indexonlyscan;
RESET enable_bitmapscan;

-- A finalized unknown-CustomScan classification is likewise fail-open.
SELECT bioseq_cache_test.set_exact_plan_fault('unknown_custom_scan');
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
DO $$
DECLARE
    attempt integer;
    observed text;
    cache_stats record;
    plan_text text;
    plan jsonb;
BEGIN
    FOR attempt IN 1..2 LOOP
        EXECUTE 'SELECT value FROM qc_elig WHERE id = 2' INTO observed;
        IF observed <> 'two' THEN
            RAISE EXCEPTION 'CustomScan-class bypass changed child result: %',
                observed;
        END IF;
    END LOOP;
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT value FROM qc_elig WHERE id = 2'
        INTO plan_text;
    plan := plan_text::jsonb;
    IF COALESCE(
            (plan->0->'Plan'->>'Cache Force Bypass')::boolean,
            false) IS NOT TRUE THEN
        RAISE EXCEPTION 'unknown-CustomScan bypass proof is absent: %', plan;
    END IF;
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    IF cache_stats.produced <> 0
       OR cache_stats.completed_hits <> 0
       OR cache_stats.inflight_waits <> 0
       OR cache_stats.fallbacks <> 2
       OR EXISTS (
            SELECT 1 FROM bioseq_cache_test.entry_states()
            WHERE state IN ('BUILDING', 'COMPLETE')) THEN
        RAISE EXCEPTION 'CustomScan classifier touched the registry: %',
            row_to_json(cache_stats);
    END IF;
END
$$;
SELECT bioseq_cache_test.set_exact_plan_fault('none');

CREATE ROLE qc_alice;
CREATE ROLE qc_bob;
CREATE TABLE qc_rls(owner_name name, value text);
INSERT INTO qc_rls VALUES ('qc_alice', 'alice-secret'), ('qc_bob', 'bob-secret');
ALTER TABLE qc_rls ENABLE ROW LEVEL SECURITY;
CREATE POLICY qc_owner_policy ON qc_rls
USING (owner_name = CURRENT_USER);
GRANT SELECT ON qc_rls TO qc_alice, qc_bob;

SELECT bioseq_cache.reset_stats();
SET ROLE qc_alice;
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT value FROM qc_rls ORDER BY value;
RESET ROLE;
SET ROLE qc_bob;
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT value FROM qc_rls ORDER BY value;
RESET ROLE;

REVOKE SELECT ON qc_rls FROM qc_bob;

DO $$
BEGIN
    IF EXISTS (SELECT 1 FROM bioseq_cache.stats() WHERE produced <> 0) THEN
        RAISE EXCEPTION 'RLS result entered cross-backend shared cache';
    END IF;
    IF has_table_privilege('qc_bob', 'qc_rls', 'SELECT') THEN
        RAISE EXCEPTION 'permission revoke was not applied';
    END IF;
    IF has_function_privilege('qc_alice', 'bioseq_cache.entries()', 'EXECUTE') THEN
        RAISE EXCEPTION 'ordinary role can inspect sensitive cache entries';
    END IF;
END
$$;

-- SQL ACLs authorize monitoring calls; C-side row disclosure remains scoped
-- to the producer, superusers, and the policy monitoring role.
CREATE ROLE qc_cache_producer;
CREATE ROLE qc_cache_monitor;
CREATE ROLE qc_cache_stats_monitor;
GRANT pg_read_all_stats TO qc_cache_stats_monitor;
GRANT SELECT ON qc_elig TO qc_cache_producer;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SET ROLE qc_cache_producer;
SET bioseq.query_cache_optimisation_mode = 'same';
SELECT id, value, 'qc-cache-private-literal-7f13'::text AS private_marker
FROM qc_elig
WHERE id = 1;
RESET ROLE;

GRANT USAGE ON SCHEMA bioseq_cache
TO qc_cache_producer, qc_cache_monitor;
GRANT EXECUTE ON FUNCTION bioseq_cache.entries(), bioseq_cache.stats()
TO qc_cache_producer, qc_cache_monitor;

SET ROLE qc_cache_monitor;
DO $$
BEGIN
    IF (SELECT count(*) FROM bioseq_cache.entries()) <> 1
       OR EXISTS (
            SELECT 1
            FROM bioseq_cache.entries()
            WHERE kind = 'ExactFinalResult'
              AND (owner_role IS NOT NULL
                   OR producer_pid IS NOT NULL
                   OR sql IS NOT NULL
                   OR producer_plan IS NOT NULL
                   OR dependencies IS NOT NULL))
       OR NOT EXISTS (
            SELECT 1
            FROM bioseq_cache.entries()
            WHERE kind = 'ExactFinalResult'
              AND state = 'COMPLETE'
              AND rows = 1
              AND logical_bytes > 0
              AND stored_bytes > 0)
       OR (SELECT count(*) FROM bioseq_cache.stats()) <> 1 THEN
        RAISE EXCEPTION 'explicit monitor did not receive a redacted scalar summary';
    END IF;
END
$$;
RESET ROLE;

SET ROLE qc_cache_producer;
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM bioseq_cache.entries()
        WHERE owner_role = 'qc_cache_producer'::regrole
          AND producer_pid IS NOT NULL
          AND sql LIKE '%qc-cache-private-literal-7f13%'
          AND dependencies IS NOT NULL)
    THEN
        RAISE EXCEPTION 'producer cannot inspect its authorized cache text';
    END IF;
END
$$;
RESET ROLE;

SET ROLE qc_cache_stats_monitor;
DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1
        FROM bioseq_cache.entries()
        WHERE owner_role = 'qc_cache_producer'::regrole
          AND producer_pid IS NOT NULL
          AND sql LIKE '%qc-cache-private-literal-7f13%'
          AND dependencies IS NOT NULL)
    THEN
        RAISE EXCEPTION 'pg_read_all_stats policy monitor cannot inspect cache text';
    END IF;
END
$$;
RESET ROLE;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, value FROM qc_elig ORDER BY id;
SELECT id, value FROM qc_elig ORDER BY id;

CREATE TEMP TABLE qc_explain_json(plan jsonb);
DO $$
DECLARE
    plan_line text;
BEGIN
    FOR plan_line IN EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT id, value FROM qc_elig ORDER BY id'
    LOOP
        INSERT INTO qc_explain_json VALUES (plan_line::jsonb);
    END LOOP;
END
$$;

EXPLAIN (VERBOSE, COSTS OFF)
SELECT id, value FROM qc_elig ORDER BY id;
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, BUFFERS)
SELECT id, value FROM qc_elig ORDER BY id;
EXPLAIN (FORMAT YAML, VERBOSE, COSTS OFF)
SELECT id, value FROM qc_elig ORDER BY id;
EXPLAIN (FORMAT XML, VERBOSE, COSTS OFF)
SELECT id, value FROM qc_elig ORDER BY id;

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM qc_explain_json
        WHERE jsonb_typeof(plan) <> 'array')
    THEN
        RAISE EXCEPTION 'structured EXPLAIN is not a JSON array';
    END IF;
    IF NOT EXISTS (
        SELECT 1
        FROM qc_explain_json,
             jsonb_array_elements(plan) root
        WHERE root ? 'Plan')
    THEN
        RAISE EXCEPTION 'structured EXPLAIN lacks typed Plan field';
    END IF;
END
$$;

\o
SELECT 'cache_eligibility_security_explain: ok';
