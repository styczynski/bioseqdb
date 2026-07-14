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
