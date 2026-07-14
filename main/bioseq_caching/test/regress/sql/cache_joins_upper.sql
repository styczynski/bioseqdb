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
