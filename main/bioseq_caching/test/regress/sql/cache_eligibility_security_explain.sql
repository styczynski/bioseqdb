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
