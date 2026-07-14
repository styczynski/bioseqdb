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

-- An unregistered CustomScan is likewise bypassed while its executor still
-- produces the complete disabled-mode bag.
CREATE TABLE qc_unsafe_provider(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO qc_unsafe_provider VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0, 'zero', 3),
    (3, 12, 0, 5, 'first', 3),
    (4, 12, 1, 5, 'second', 3),
    (5, 13, 0, 20, 'outside', 3);
SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation('qc_unsafe_provider');
SELECT bioseq_cache_test.provider_reset();
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
DO $$
DECLARE
    attempt integer;
    observed bigint[];
    observed_row record;
    cache_stats record;
    provider_stats jsonb;
    plan_text text;
    plan jsonb;
BEGIN
    FOR attempt IN 1..2 LOOP
        observed := ARRAY[]::bigint[];
        FOR observed_row IN EXECUTE
            'SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask '
            'FROM qc_unsafe_provider WHERE nullable_value <= 10'
        LOOP
            observed := array_append(observed, observed_row.row_id);
        END LOOP;
        SELECT array_agg(value ORDER BY value)
        INTO observed
        FROM unnest(observed) AS sorted_values(value);
        IF observed <> ARRAY[2,3,4]::bigint[] THEN
            RAISE EXCEPTION 'CustomScan bypass changed retained-child bag: %',
                observed;
        END IF;
    END LOOP;
    EXECUTE
        'EXPLAIN (FORMAT JSON, VERBOSE, COSTS OFF) '
        'SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask '
        'FROM qc_unsafe_provider WHERE nullable_value <= 10'
        INTO plan_text;
    plan := plan_text::jsonb;
    IF COALESCE(
            (plan->0->'Plan'->>'Cache Force Bypass')::boolean,
            false) IS NOT TRUE
       OR position('BioseqCacheGenericTestProvider' in plan::text) = 0 THEN
        RAISE EXCEPTION 'unknown CustomScan bypass proof is absent: %', plan;
    END IF;
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    provider_stats := bioseq_cache_test.provider_stats();
    IF cache_stats.produced <> 0
       OR cache_stats.completed_hits <> 0
       OR cache_stats.inflight_waits <> 0
       OR cache_stats.fallbacks <> 2
       OR (provider_stats->>'executor_rows')::bigint = 0
       OR EXISTS (
            SELECT 1 FROM bioseq_cache_test.entry_states()
            WHERE state IN ('BUILDING', 'COMPLETE')) THEN
        RAISE EXCEPTION 'CustomScan verifier failed open incorrectly: %, %',
            row_to_json(cache_stats), provider_stats;
    END IF;
END
$$;
SELECT bioseq_cache_test.provider_shutdown();

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
