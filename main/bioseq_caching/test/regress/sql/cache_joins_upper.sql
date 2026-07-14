\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET bioseq.query_cache_optimisation_mode = 'same';

CREATE TABLE qc_left(id integer, k integer, payload text);
CREATE TABLE qc_right(id integer, k integer, payload text);
INSERT INTO qc_left VALUES
    (1, 1, 'l1'), (2, 1, 'l2'), (3, 2, 'l3'), (4, NULL, 'ln');
INSERT INTO qc_right VALUES
    (10, 1, 'r1'), (11, 1, 'r2'), (12, 3, 'r3'), (13, NULL, 'rn');

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

SELECT l.id, r.id
FROM qc_left l JOIN qc_right r ON l.k = r.k
ORDER BY l.id, r.id;
SELECT l.id, r.id
FROM qc_left l JOIN qc_right r ON l.k = r.k
ORDER BY l.id, r.id;

SELECT l.id, r.id
FROM qc_left l CROSS JOIN qc_right r
WHERE l.id <= 2 AND r.id <= 11
ORDER BY l.id, r.id;
SELECT l.id, r.id
FROM qc_left l CROSS JOIN qc_right r
WHERE l.id <= 2 AND r.id <= 11
ORDER BY l.id, r.id;

SELECT l.id, r.id
FROM qc_left l LEFT JOIN qc_right r ON l.k = r.k
ORDER BY l.id, r.id NULLS FIRST;
SELECT l.id, r.id
FROM qc_left l LEFT JOIN qc_right r ON l.k = r.k
ORDER BY l.id, r.id NULLS FIRST;

SELECT l.id, r.id
FROM qc_left l RIGHT JOIN qc_right r ON l.k = r.k
ORDER BY r.id, l.id NULLS FIRST;
SELECT l.id, r.id
FROM qc_left l RIGHT JOIN qc_right r ON l.k = r.k
ORDER BY r.id, l.id NULLS FIRST;

SELECT l.id, r.id
FROM qc_left l FULL JOIN qc_right r ON l.k = r.k
ORDER BY l.id NULLS FIRST, r.id NULLS FIRST;
SELECT l.id, r.id
FROM qc_left l FULL JOIN qc_right r ON l.k = r.k
ORDER BY l.id NULLS FIRST, r.id NULLS FIRST;

SELECT l.id
FROM qc_left l
WHERE EXISTS (SELECT 1 FROM qc_right r WHERE r.k = l.k)
ORDER BY l.id;
SELECT l.id
FROM qc_left l
WHERE EXISTS (SELECT 1 FROM qc_right r WHERE r.k = l.k)
ORDER BY l.id;

SELECT l.id
FROM qc_left l
WHERE NOT EXISTS (SELECT 1 FROM qc_right r WHERE r.k = l.k)
ORDER BY l.id;
SELECT l.id
FROM qc_left l
WHERE NOT EXISTS (SELECT 1 FROM qc_right r WHERE r.k = l.k)
ORDER BY l.id;

DO $$
DECLARE
    cache_stats record;
    semi_ids integer[];
    anti_ids integer[];
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT array_agg(id ORDER BY id) INTO semi_ids
    FROM (
        SELECT l.id
        FROM qc_left l
        WHERE EXISTS (SELECT 1 FROM qc_right r WHERE r.k = l.k)
    ) AS matched;
    SELECT array_agg(id ORDER BY id) INTO anti_ids
    FROM (
        SELECT l.id
        FROM qc_left l
        WHERE NOT EXISTS (SELECT 1 FROM qc_right r WHERE r.k = l.k)
    ) AS unmatched;
    IF semi_ids IS DISTINCT FROM ARRAY[1, 2]
       OR anti_ids IS DISTINCT FROM ARRAY[3, 4]
       OR cache_stats.produced <> 7
       OR cache_stats.completed_hits <> 7
       OR cache_stats.fallbacks <> 0 THEN
        RAISE EXCEPTION 'join matrix mismatch: semi %, anti %, stats %',
            semi_ids, anti_ids, row_to_json(cache_stats);
    END IF;
END
$$;

SELECT k, count(*), min(id), max(id)
FROM qc_left
GROUP BY k
HAVING count(*) >= 1
ORDER BY k NULLS FIRST;
SELECT k, count(*), min(id), max(id)
FROM qc_left
GROUP BY k
HAVING count(*) >= 1
ORDER BY k NULLS FIRST;

SELECT DISTINCT k FROM qc_left ORDER BY k NULLS FIRST;
SELECT DISTINCT k FROM qc_left ORDER BY k NULLS FIRST;

SELECT id, row_number() OVER (PARTITION BY k ORDER BY id)
FROM qc_left
ORDER BY id;
SELECT id, row_number() OVER (PARTITION BY k ORDER BY id)
FROM qc_left
ORDER BY id;

SELECT id FROM qc_left
UNION ALL
SELECT id FROM qc_left
ORDER BY id
LIMIT 6 OFFSET 1;
SELECT id FROM qc_left
UNION ALL
SELECT id FROM qc_left
ORDER BY id
LIMIT 6 OFFSET 1;

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();

\o
SELECT k, count(*), min(id), max(id)
FROM qc_left
GROUP BY k
HAVING count(*) >= 1
ORDER BY k NULLS FIRST;
SELECT k, count(*), min(id), max(id)
FROM qc_left
GROUP BY k
HAVING count(*) >= 1
ORDER BY k NULLS FIRST;

SELECT DISTINCT k FROM qc_left ORDER BY k NULLS FIRST;
SELECT DISTINCT k FROM qc_left ORDER BY k NULLS FIRST;

SELECT id, row_number() OVER (PARTITION BY k ORDER BY id)
FROM qc_left
ORDER BY id;
SELECT id, row_number() OVER (PARTITION BY k ORDER BY id)
FROM qc_left
ORDER BY id;

SELECT id FROM qc_left
UNION ALL
SELECT id FROM qc_left
ORDER BY id
LIMIT 6 OFFSET 1;
SELECT id FROM qc_left
UNION ALL
SELECT id FROM qc_left
ORDER BY id
LIMIT 6 OFFSET 1;

SELECT produced, completed_hits, fallbacks
FROM bioseq_cache.stats();
\o /dev/null

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 4
          AND completed_hits = 4
          AND fallbacks = 0)
    THEN
        RAISE EXCEPTION 'immutable upper operations did not produce and replay';
    END IF;
END
$$;

-- INTERSECT and EXCEPT are immutable exact upper operations as well.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT k FROM qc_left WHERE k IS NOT NULL
INTERSECT
SELECT k FROM qc_right WHERE k IS NOT NULL
ORDER BY k;
SELECT k FROM qc_left WHERE k IS NOT NULL
INTERSECT
SELECT k FROM qc_right WHERE k IS NOT NULL
ORDER BY k;
SELECT k FROM qc_left WHERE k IS NOT NULL
EXCEPT
SELECT k FROM qc_right WHERE k IS NOT NULL
ORDER BY k;
SELECT k FROM qc_left WHERE k IS NOT NULL
EXCEPT
SELECT k FROM qc_right WHERE k IS NOT NULL
ORDER BY k;

DO $$
DECLARE
    cache_stats record;
    intersect_values integer[];
    except_values integer[];
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT array_agg(k ORDER BY k) INTO intersect_values
    FROM (
        SELECT k FROM qc_left WHERE k IS NOT NULL
        INTERSECT
        SELECT k FROM qc_right WHERE k IS NOT NULL
    ) AS intersection;
    SELECT array_agg(k ORDER BY k) INTO except_values
    FROM (
        SELECT k FROM qc_left WHERE k IS NOT NULL
        EXCEPT
        SELECT k FROM qc_right WHERE k IS NOT NULL
    ) AS difference;
    IF intersect_values IS DISTINCT FROM ARRAY[1]
       OR except_values IS DISTINCT FROM ARRAY[2]
       OR cache_stats.produced <> 2
       OR cache_stats.completed_hits <> 2
       OR cache_stats.fallbacks <> 0 THEN
        RAISE EXCEPTION 'set-operation mismatch: intersect %, except %, stats %',
            intersect_values, except_values, row_to_json(cache_stats);
    END IF;
END
$$;

-- bigint || text selects the polymorphic STABLE anytextcat function.  The
-- aggregates remain immutable, but DOC0371 deliberately rejects this call.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
\o
SELECT count(*) || '|' || sum(id) FROM qc_left;
SELECT count(*) || '|' || sum(id) FROM qc_left;
SELECT produced, completed_hits, fallbacks
FROM bioseq_cache.stats();
\o /dev/null

DO $$
BEGIN
    IF EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced <> 0
           OR completed_hits <> 0
           OR fallbacks <> 0)
    THEN
        RAISE EXCEPTION 'STABLE implicit text conversion entered exact cache';
    END IF;
END
$$;

-- Explicit immutable integer-to-text casts select immutable textcat.
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
\o
SELECT count(*)::text || '|' || sum(id)::text FROM qc_left;
SELECT count(*)::text || '|' || sum(id)::text FROM qc_left;
SELECT produced, completed_hits, fallbacks
FROM bioseq_cache.stats();
\o /dev/null

DO $$
BEGIN
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced = 1
          AND completed_hits = 1
          AND fallbacks = 0)
    THEN
        RAISE EXCEPTION 'immutable scalar aggregate did not produce and replay';
    END IF;
END
$$;

-- Replacing an immutable aggregate support function invalidates exact state.
CREATE FUNCTION qc_replace_sum_state(integer, integer)
RETURNS integer
LANGUAGE SQL IMMUTABLE STRICT
AS 'SELECT $1 + $2';
CREATE AGGREGATE qc_replace_sum(integer) (
    SFUNC = qc_replace_sum_state,
    STYPE = integer,
    INITCOND = '0'
);

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT qc_replace_sum(id) FROM qc_left;
SELECT qc_replace_sum(id) FROM qc_left;

DO $$
DECLARE
    cache_stats record;
    observed integer;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT qc_replace_sum(id) INTO observed FROM qc_left;
    IF observed <> 10
       OR cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 1
       OR cache_stats.fallbacks <> 0 THEN
        RAISE EXCEPTION 'immutable aggregate baseline mismatch: %, %',
            observed, row_to_json(cache_stats);
    END IF;
END
$$;

CREATE OR REPLACE FUNCTION qc_replace_sum_state(integer, integer)
RETURNS integer
LANGUAGE SQL IMMUTABLE STRICT
AS 'SELECT $1 + $2 + 100';

DO $$
DECLARE
    cache_stats record;
    live_entries integer;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT count(*) INTO live_entries
    FROM bioseq_cache.entries()
    WHERE state = 'COMPLETE'
      AND sql LIKE '%qc_replace_sum(id)%';
    IF cache_stats.invalidations = 0 OR live_entries <> 0 THEN
        RAISE EXCEPTION
            'aggregate support replacement invalidation mismatch: invalidations %, live %',
            cache_stats.invalidations, live_entries;
    END IF;
END
$$;

SELECT bioseq_cache.reset_stats();
SELECT qc_replace_sum(id) FROM qc_left;
SELECT qc_replace_sum(id) FROM qc_left;

DO $$
DECLARE
    cache_stats record;
    observed integer;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT qc_replace_sum(id) INTO observed FROM qc_left;
    IF observed <> 410
       OR cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 1
       OR cache_stats.fallbacks <> 0 THEN
        RAISE EXCEPTION 'aggregate support replacement was stale: %, %',
            observed, row_to_json(cache_stats);
    END IF;
END
$$;

-- Repeat replacement invalidation when the aggregate executes as a window.
CREATE FUNCTION qc_replace_window_state(integer, integer)
RETURNS integer
LANGUAGE SQL IMMUTABLE STRICT
AS 'SELECT $1 + $2';
CREATE AGGREGATE qc_replace_window_sum(integer) (
    SFUNC = qc_replace_window_state,
    STYPE = integer,
    INITCOND = '0'
);

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT id, qc_replace_window_sum(id) OVER (
    ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
FROM qc_left ORDER BY id;
SELECT id, qc_replace_window_sum(id) OVER (
    ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
FROM qc_left ORDER BY id;

DO $$
DECLARE
    cache_stats record;
    observed integer[];
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT array_agg(running_sum ORDER BY id) INTO observed
    FROM (
        SELECT id, qc_replace_window_sum(id) OVER (
            ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
            AS running_sum
        FROM qc_left
    ) AS values_before;
    IF observed IS DISTINCT FROM ARRAY[1, 3, 6, 10]
       OR cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 1
       OR cache_stats.fallbacks <> 0 THEN
        RAISE EXCEPTION 'aggregate window baseline mismatch: %, %',
            observed, row_to_json(cache_stats);
    END IF;
END
$$;

CREATE OR REPLACE FUNCTION qc_replace_window_state(integer, integer)
RETURNS integer
LANGUAGE SQL IMMUTABLE STRICT
AS 'SELECT $1 + $2 + 1000';

DO $$
DECLARE
    cache_stats record;
    live_entries integer;
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT count(*) INTO live_entries
    FROM bioseq_cache.entries()
    WHERE state = 'COMPLETE'
      AND sql LIKE '%qc_replace_window_sum(id)%';
    IF cache_stats.invalidations = 0 OR live_entries <> 0 THEN
        RAISE EXCEPTION
            'aggregate-window replacement invalidation mismatch: invalidations %, live %',
            cache_stats.invalidations, live_entries;
    END IF;
END
$$;

SELECT bioseq_cache.reset_stats();
SELECT id, qc_replace_window_sum(id) OVER (
    ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
FROM qc_left ORDER BY id;
SELECT id, qc_replace_window_sum(id) OVER (
    ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
FROM qc_left ORDER BY id;

DO $$
DECLARE
    cache_stats record;
    observed integer[];
BEGIN
    SELECT * INTO cache_stats FROM bioseq_cache.stats();
    SELECT array_agg(running_sum ORDER BY id) INTO observed
    FROM (
        SELECT id, qc_replace_window_sum(id) OVER (
            ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)
            AS running_sum
        FROM qc_left
    ) AS values_after;
    IF observed IS DISTINCT FROM ARRAY[1001, 2003, 3006, 4010]
       OR cache_stats.produced <> 1
       OR cache_stats.completed_hits <> 1
       OR cache_stats.fallbacks <> 0 THEN
        RAISE EXCEPTION 'aggregate-window replacement was stale: %, %',
            observed, row_to_json(cache_stats);
    END IF;
END
$$;

CREATE FUNCTION qc_volatile_state(integer, integer)
RETURNS integer
LANGUAGE SQL VOLATILE
AS 'SELECT $1 + $2';
CREATE AGGREGATE qc_volatile_sum(integer) (
    SFUNC = qc_volatile_state,
    STYPE = integer,
    INITCOND = '0'
);

SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
SELECT qc_volatile_sum(id) FROM qc_left;

DO $$
BEGIN
    IF (SELECT count(*) FROM qc_left l JOIN qc_right r ON l.k = r.k) <> 4 THEN
        RAISE EXCEPTION 'inner join multiplicity is wrong';
    END IF;
    IF (SELECT count(*) FROM qc_left l FULL JOIN qc_right r ON l.k = r.k) <> 8 THEN
        RAISE EXCEPTION 'full join multiplicity is wrong';
    END IF;
    IF EXISTS (SELECT 1 FROM bioseq_cache.stats() WHERE produced <> 0) THEN
        RAISE EXCEPTION 'volatile aggregate query entered cache';
    END IF;
END
$$;

\o
SELECT 'cache_joins_upper: ok';
