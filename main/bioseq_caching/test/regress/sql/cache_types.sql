\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SET TimeZone = 'UTC';
SET bioseq.query_cache_optimisation_mode = 'disabled';

CREATE TYPE qc_enum AS ENUM ('one', 'two');
CREATE DOMAIN qc_positive AS integer CHECK (VALUE > 0);
CREATE TYPE qc_pair AS (x integer, y text);

CREATE TABLE qc_types(
    id bigint,
    i2 smallint,
    i4 integer,
    i8 bigint,
    b boolean,
    f4 real,
    f8 double precision,
    n numeric,
    t text,
    bytes bytea,
    u uuid,
    d date,
    tm time,
    ts timestamp,
    tstz timestamptz,
    iv interval,
    a integer[],
    j jsonb,
    e qc_enum,
    dom qc_positive,
    pair qc_pair
);

INSERT INTO qc_types VALUES
  (1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (2, -32768, -2147483648, -9223372036854775808, false,
   '-Infinity', 'NaN', 1234567890.00100, '', '\x',
   '00000000-0000-0000-0000-000000000000', '2000-01-01', '00:00:00',
   '2000-01-01 00:00:00', '2000-01-01 00:00:00+00', '0 seconds', '{}',
   '{}'::jsonb, 'one', 1, ROW(NULL, '')::qc_pair),
  (3, 32767, 2147483647, 9223372036854775807, true,
   'Infinity', '-Infinity', -0.0000012300, 'zażółć €',
   decode('004100e282acff', 'hex'),
   'ffffffff-ffff-ffff-ffff-ffffffffffff', '2099-12-31', '23:59:59.999999',
   '2099-12-31 23:59:59.999999', '2099-12-31 23:59:59.999999+00',
   '2 years 3 mons 4 days 05:06:07', ARRAY[1, NULL, 3],
   '{"nested":[1,null,"x"]}'::jsonb, 'two', 42,
   ROW(7, repeat('toast', 3000))::qc_pair),
  (4, 7, 8, 9, true, 1.5, 2.5, 999.9900,
   repeat('large-text-', 5000), decode(repeat('0041', 6000), 'hex'),
   gen_random_uuid(), CURRENT_DATE, LOCALTIME, LOCALTIMESTAMP,
   CURRENT_TIMESTAMP, interval '1 microsecond', ARRAY[]::integer[],
   'null'::jsonb, 'one', 2, ROW(8, NULL)::qc_pair);

CREATE TEMP TABLE qc_types_reference AS TABLE qc_types;
CREATE TEMP TABLE qc_types_digest AS
SELECT md5(string_agg(encode(pg_catalog.record_send(t), 'hex'), '' ORDER BY id))
       AS digest
FROM qc_types AS t;

SET bioseq.query_cache_optimisation_mode = 'same';
SELECT bioseq_cache.reset_stats();

SELECT * FROM qc_types ORDER BY id;
SELECT * FROM qc_types ORDER BY id;

SELECT * FROM qc_types EXCEPT ALL SELECT * FROM qc_types_reference;
SELECT * FROM qc_types_reference EXCEPT ALL SELECT * FROM qc_types;

DO $$
DECLARE
    current_digest text;
BEGIN
    SELECT md5(string_agg(encode(pg_catalog.record_send(t), 'hex'), '' ORDER BY id))
    INTO current_digest
    FROM qc_types AS t;
    IF current_digest IS DISTINCT FROM (SELECT digest FROM qc_types_digest) THEN
        RAISE EXCEPTION 'binary-send digest changed after cache replay';
    END IF;
    IF NOT EXISTS (
        SELECT 1 FROM bioseq_cache.stats()
        WHERE produced >= 1 AND completed_hits >= 1)
    THEN
        RAISE EXCEPTION 'type matrix did not produce and hit';
    END IF;
END
$$;

\o
SELECT 'cache_types: ok';
