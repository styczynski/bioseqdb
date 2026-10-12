\set ON_ERROR_STOP on
CREATE EXTENSION traceheap;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE specimens (
  id integer NOT NULL,
  label text NOT NULL,
  read_count integer NOT NULL
) USING traceheap;
INSERT INTO specimens VALUES
  (1, 'soil', 120), (2, 'river', 900),
  (3, 'leaf', 480), (4, 'air', 60);

SELECT traceheap.reset();
SELECT id, label FROM specimens WHERE read_count >= 400 ORDER BY id;
DO $$ BEGIN
  IF (traceheap.stats()->>'scan_rows')::integer <> 4 THEN
    RAISE EXCEPTION 'the sequential scan should visit all four rows';
  END IF;
END $$;

SELECT traceheap.reset();
UPDATE specimens SET read_count = read_count + 1 WHERE id = 1;
DELETE FROM specimens WHERE id = 4;
DO $$ BEGIN
  IF (traceheap.stats()->>'updates')::integer <> 1
     OR (traceheap.stats()->>'deletes')::integer <> 1 THEN
    RAISE EXCEPTION 'update/delete callbacks did not run';
  END IF;
END $$;

SELECT traceheap.reset();
BEGIN;
INSERT INTO specimens VALUES (99, 'rolled back', 0);
ROLLBACK;
DO $$ BEGIN
  IF EXISTS (SELECT FROM specimens WHERE id = 99)
     OR (traceheap.stats()->>'inserts')::integer <> 1 THEN
    RAISE EXCEPTION 'rollback or nontransactional counter contract failed';
  END IF;
END $$;

SELECT traceheap.reset();
COPY specimens FROM STDIN;
5	pond	50
6	root	60
7	stem	70
\.
DO $$ BEGIN
  IF (traceheap.stats()->>'bulk_rows')::integer <> 3 THEN
    RAISE EXCEPTION 'COPY did not use multi_insert';
  END IF;
END $$;

-- Index creation is deliberately rejected, including implicit constraint indexes.
DO $$ BEGIN
  BEGIN
    CREATE INDEX specimens_reads ON specimens(read_count);
    RAISE EXCEPTION 'index creation unexpectedly succeeded';
  EXCEPTION WHEN feature_not_supported THEN NULL;
  END;
  BEGIN
    ALTER TABLE specimens ADD PRIMARY KEY (id);
    RAISE EXCEPTION 'primary key creation unexpectedly succeeded';
  EXCEPTION WHEN feature_not_supported THEN NULL;
  END;
END $$;

-- These take delegated paths: keep them covered rather than assuming they work.
ANALYZE specimens;
VACUUM specimens;
DO $$ BEGIN
  IF (traceheap.stats()->>'vacuums')::integer <> 1 THEN
    RAISE EXCEPTION 'ordinary VACUUM did not reach its callback';
  END IF;
END $$;

CREATE TABLE large_notes (id integer, note text) USING traceheap;
INSERT INTO large_notes SELECT 1, string_agg(md5(g::text), '' ORDER BY g)
FROM generate_series(1, 1000) AS g;
DO $$ BEGIN
  IF (SELECT note FROM large_notes) IS DISTINCT FROM
     (SELECT string_agg(md5(g::text), '' ORDER BY g) FROM generate_series(1, 1000) AS g) THEN
    RAISE EXCEPTION 'TOAST round trip failed';
  END IF;
END $$;
TRUNCATE large_notes;
DROP TABLE large_notes;

-- Leave specimens for the two-session snapshot and restart checks.
SELECT id, label, read_count FROM specimens ORDER BY id;
SELECT 'traceheap smoke checks passed' AS result;
