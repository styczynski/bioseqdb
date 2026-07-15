\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'reuse';
SET bioseq.query_cache_ttl = '60 min';

CREATE TABLE cache_security_rows(
    row_id bigint PRIMARY KEY,
    base_id bigint NOT NULL,
    occurrence_no integer NOT NULL,
    nullable_value integer,
    payload text NOT NULL,
    capability_mask bigint NOT NULL,
    UNIQUE (base_id, occurrence_no)
);
INSERT INTO cache_security_rows VALUES
    (1, 10, 0, NULL, 'null', 3),
    (2, 11, 0, 0,    'zero', 3),
    (3, 12, 0, 5,    'five', 3),
    (4, 13, 0, 10,   'ten', 3);

CREATE ROLE cache_security_alice;
CREATE ROLE cache_security_bob;
CREATE ROLE cache_security_monitor;
GRANT SELECT ON cache_security_rows
TO cache_security_alice, cache_security_bob;
GRANT USAGE ON SCHEMA bioseq_cache
TO cache_security_alice, cache_security_monitor;
GRANT EXECUTE ON FUNCTION bioseq_cache.entries()
TO cache_security_alice, cache_security_monitor;

SELECT bioseq_cache_test.provider_init();
SELECT bioseq_cache_test.provider_use_relation('cache_security_rows');
SELECT bioseq_cache_test.provider_reset();

-- A SET ROLE change must create an independent current generation.  Each
-- role's second execution is nevertheless a hit in its own namespace.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache.reset_stats();
SET ROLE cache_security_alice;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
RESET ROLE;
SET ROLE cache_security_bob;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
RESET ROLE;
WITH observed AS (
    SELECT * FROM bioseq_cache.stats()
)
SELECT 1 / CASE
    WHEN produced = 2 AND reuse_hits = 2 AND fallbacks = 0 THEN 1
    ELSE 0
END
FROM observed;

-- C-side row filtering applies before reusable SQL and Plan text are copied.
SET ROLE cache_security_alice;
WITH observed AS (
    SELECT
        count(*) FILTER (
            WHERE kind = 'ReusableCoverage'
              AND state = 'COMPLETE'
              AND owner_role = 'cache_security_alice'::regrole
              AND producer_pid IS NOT NULL
              AND sql IS NOT NULL
              AND producer_plan IS NOT NULL
              AND dependencies->>'canonical_plan_status' = 'Planned'
              AND dependencies->>'producer_plan_status' = 'Planned'
        ) AS own_rows,
        count(*) FILTER (
            WHERE kind = 'ReusableCoverage'
              AND state = 'COMPLETE'
              AND entry_key IS NOT NULL
              AND age IS NOT NULL
              AND rows IS NOT NULL
              AND logical_bytes IS NOT NULL
              AND stored_bytes IS NOT NULL
        ) AS scalar_rows,
        count(*) FILTER (
            WHERE kind = 'ReusableCoverage'
              AND state = 'COMPLETE'
              AND owner_role IS NULL
              AND producer_pid IS NULL
              AND sql IS NULL
              AND producer_plan IS NULL
              AND dependencies IS NULL
        ) AS foreign_rows,
        count(*) FILTER (
            WHERE kind = 'ReusableCoverage'
              AND state = 'COMPLETE'
              AND owner_role IS NULL
              AND (producer_pid IS NOT NULL OR sql IS NOT NULL
                   OR producer_plan IS NOT NULL OR dependencies IS NOT NULL)
        ) AS leaked_rows
    FROM bioseq_cache.entries()
)
SELECT 1 / CASE
    WHEN own_rows = 1 AND scalar_rows = 2 AND foreign_rows = 1
         AND leaked_rows = 0 THEN 1
    ELSE 0
END
FROM observed;
RESET ROLE;

SET ROLE cache_security_monitor;
WITH observed AS (
    SELECT
        count(*) FILTER (
            WHERE kind = 'ReusableCoverage'
              AND state = 'COMPLETE'
              AND entry_key IS NOT NULL
              AND age IS NOT NULL
              AND rows IS NOT NULL
              AND logical_bytes IS NOT NULL
              AND stored_bytes IS NOT NULL
        ) AS scalar_rows,
        count(*) FILTER (
            WHERE kind = 'ReusableCoverage'
              AND state = 'COMPLETE'
              AND (owner_role IS NOT NULL OR producer_pid IS NOT NULL
                   OR sql IS NOT NULL OR producer_plan IS NOT NULL
                   OR dependencies IS NOT NULL)
        ) AS leaked_rows
    FROM bioseq_cache.entries()
)
SELECT 1 / CASE
    WHEN scalar_rows = 2 AND leaked_rows = 0 THEN 1
    ELSE 0
END
FROM observed;
RESET ROLE;

SET ROLE pg_read_all_stats;
WITH observed AS (
    SELECT count(*) AS full_rows
    FROM bioseq_cache.entries()
    WHERE kind = 'ReusableCoverage'
      AND state = 'COMPLETE'
      AND owner_role IS NOT NULL
      AND producer_pid IS NOT NULL
      AND sql IS NOT NULL
      AND producer_plan IS NOT NULL
      AND dependencies IS NOT NULL
)
SELECT 1 / CASE WHEN full_rows = 2 THEN 1 ELSE 0 END
FROM observed;
RESET ROLE;

REVOKE EXECUTE ON FUNCTION bioseq_cache.entries()
FROM cache_security_alice, cache_security_monitor;
REVOKE USAGE ON SCHEMA bioseq_cache
FROM cache_security_alice, cache_security_monitor;
DROP ROLE cache_security_monitor;

-- row_security is an explicit namespace field even for a non-RLS fixture.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache.reset_stats();
SET row_security = on;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SET row_security = off;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SET row_security = on;
WITH observed AS (
    SELECT * FROM bioseq_cache.stats()
)
SELECT 1 / CASE
    WHEN produced = 2 AND reuse_hits = 2 AND fallbacks = 0 THEN 1
    ELSE 0
END
FROM observed;

-- The provider-defined 32-byte namespace is mandatory key material.  The
-- setter resets cached plans so each Plan captures exactly one frozen value.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache_test.request_janitor_sweep();
SELECT bioseq_cache.reset_stats();
SELECT bioseq_cache_test.provider_set_security_digest(
    decode(repeat('11', 32), 'hex'));
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT bioseq_cache_test.provider_set_security_digest(
    decode(repeat('22', 32), 'hex'));
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
SELECT row_id, base_id, occurrence_no, nullable_value, payload, capability_mask
FROM cache_security_rows WHERE nullable_value <= 10 ORDER BY row_id;
WITH observed AS (
    SELECT * FROM bioseq_cache.stats()
)
SELECT 1 / CASE
    WHEN produced = 2 AND reuse_hits = 2 AND fallbacks = 0 THEN 1
    ELSE 0
END
FROM observed;

SELECT bioseq_cache_test.provider_clear_security_digest();
SELECT bioseq_cache_test.provider_shutdown();
REVOKE SELECT ON cache_security_rows
FROM cache_security_alice, cache_security_bob;
DROP ROLE cache_security_alice;
DROP ROLE cache_security_bob;

\o
SELECT 'cache_reuse_security: ok';
