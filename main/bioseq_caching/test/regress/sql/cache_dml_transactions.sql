\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'same';
SET bioseq.query_cache_ttl = '60 min';

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

BEGIN;
INSERT INTO qc_dml VALUES (7, 3, 'own write');
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

TRUNCATE qc_dml;
SELECT count(*) FROM qc_dml;

\o
SELECT 'cache_dml_transactions: ok';
