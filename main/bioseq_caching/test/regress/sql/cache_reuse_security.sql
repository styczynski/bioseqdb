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
GRANT SELECT ON cache_security_rows
TO cache_security_alice, cache_security_bob;

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
DO $$
DECLARE
    observed record;
BEGIN
    SELECT * INTO observed FROM bioseq_cache.stats();
    IF observed.produced <> 2
       OR observed.reuse_hits <> 2
       OR observed.fallbacks <> 0 THEN
        RAISE EXCEPTION 'role namespaces coalesced: %', row_to_json(observed);
    END IF;
END
$$;

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
DO $$
DECLARE
    observed record;
BEGIN
    SELECT * INTO observed FROM bioseq_cache.stats();
    IF observed.produced <> 2
       OR observed.reuse_hits <> 2
       OR observed.fallbacks <> 0 THEN
        RAISE EXCEPTION 'row-security namespaces coalesced: %',
            row_to_json(observed);
    END IF;
END
$$;

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
DO $$
DECLARE
    observed record;
BEGIN
    SELECT * INTO observed FROM bioseq_cache.stats();
    IF observed.produced <> 2
       OR observed.reuse_hits <> 2
       OR observed.fallbacks <> 0 THEN
        RAISE EXCEPTION 'provider-security namespaces coalesced: %',
            row_to_json(observed);
    END IF;
END
$$;

SELECT bioseq_cache_test.provider_clear_security_digest();
SELECT bioseq_cache_test.provider_shutdown();
REVOKE SELECT ON cache_security_rows
FROM cache_security_alice, cache_security_bob;
DROP ROLE cache_security_alice;
DROP ROLE cache_security_bob;

\o
SELECT 'cache_reuse_security: ok';
