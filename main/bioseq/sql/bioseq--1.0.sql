-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION bioseq" to load this file. \quit

SET bioseq.logging = off;

-- bioseq--9.5-1--10.0-1.sql

CREATE SCHEMA IF NOT EXISTS bioseq;
SET search_path TO bioseq;


-- ROW MASK
CREATE SEQUENCE row_mask_seq START WITH 1 INCREMENT BY 1; 

CREATE TABLE row_mask (
	id BIGINT NOT NULL,
	storage_id BIGINT NOT NULL,
	stripe_id BIGINT NOT NULL,
	chunk_id INT NOT NULL,
	start_row_number BIGINT NOT NULL,
	end_row_number BIGINT NOT NULL,
	deleted_rows INT NOT NULL,
	mask BYTEA,
	PRIMARY KEY (id, storage_id, start_row_number, end_row_number)
) WITH (user_catalog_table = true);

ALTER TABLE bioseq.row_mask ADD CONSTRAINT row_mask_stripe_unique
UNIQUE (storage_id, stripe_id, start_row_number);

ALTER TABLE bioseq.row_mask ADD CONSTRAINT row_mask_chunk_unique
UNIQUE (storage_id, stripe_id, chunk_id, start_row_number);

REVOKE SELECT ON bioseq.row_mask FROM PUBLIC;

COMMENT ON TABLE row_mask IS 'Columnar chunk mask metadata';
-- ROW MASK END

CREATE SEQUENCE IF NOT EXISTS storageid_seq MINVALUE 10000000000 NO CYCLE;

CREATE TABLE IF NOT EXISTS options (
    regclass regclass NOT NULL PRIMARY KEY,
    chunk_group_row_limit int NOT NULL,
    stripe_row_limit int NOT NULL,
    compression_level int NOT NULL,
    compression name NOT NULL
) WITH (user_catalog_table = true);

COMMENT ON TABLE options IS 'bioseq table specific options, maintained by alter_bioseq_table_set';

CREATE TABLE IF NOT EXISTS stripe (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    file_offset bigint NOT NULL,
    data_length bigint NOT NULL,
    column_count int NOT NULL,
    chunk_row_count int NOT NULL,
    row_count bigint NOT NULL,
    chunk_group_count int NOT NULL,
    first_row_number bigint NOT NULL,
    PRIMARY KEY (storage_id, stripe_num),
    CONSTRAINT stripe_first_row_number_idx UNIQUE (storage_id, first_row_number)
) WITH (user_catalog_table = true);

COMMENT ON TABLE stripe IS 'Columnar per stripe metadata';

CREATE TABLE IF NOT EXISTS chunk_group (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    chunk_group_num int NOT NULL,
    row_count bigint NOT NULL,
    PRIMARY KEY (storage_id, stripe_num, chunk_group_num)
);

COMMENT ON TABLE chunk_group IS 'Columnar chunk group metadata';

CREATE TABLE IF NOT EXISTS chunk (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    attr_num int NOT NULL,
    chunk_group_num int NOT NULL,
    minimum_value bytea,
    maximum_value bytea,
    value_stream_offset bigint NOT NULL,
    value_stream_length bigint NOT NULL,
    exists_stream_offset bigint NOT NULL,
    exists_stream_length bigint NOT NULL,
    value_compression_type int NOT NULL,
    value_compression_level int NOT NULL,
    value_decompressed_length bigint NOT NULL,
    value_count bigint NOT NULL,
    PRIMARY KEY (storage_id, stripe_num, attr_num, chunk_group_num)
) WITH (user_catalog_table = true);

COMMENT ON TABLE chunk IS 'Columnar per chunk metadata';

DO $proc$
BEGIN

-- from version 12 and up we have support for tableam's if installed on pg11 we can't
-- create the objects here. Instead we rely on citus_finish_pg_upgrade to be called by the
-- user instead to add the missing objects
IF substring(current_Setting('server_version'), '\d+')::int >= 12 THEN
  EXECUTE $$
--#include "udfs/bioseq_handler/10.0-1.sql"
CREATE OR REPLACE FUNCTION bioseq.bioseq_handler(internal)
    RETURNS table_am_handler
    LANGUAGE C
AS 'MODULE_PATHNAME', 'bioseq_handler';
COMMENT ON FUNCTION bioseq.bioseq_handler(internal)
    IS 'internal function returning the handler for bioseq tables';

-- postgres 11.8 does not support the syntax for table am, also it is seemingly trying
-- to parse the upgrade file and erroring on unknown syntax.
-- normally this section would not execute on postgres 11 anyway. To trick it to pass on
-- 11.8 we wrap the statement in a plpgsql block together with an EXECUTE. This is valid
-- syntax on 11.8 and will execute correctly in 12
DO $create_table_am$
BEGIN
  EXECUTE 'CREATE ACCESS METHOD bioseq TYPE TABLE HANDLER bioseq.bioseq_handler';
END $create_table_am$;

--#include "udfs/alter_bioseq_table_set/10.0-1.sql"
CREATE OR REPLACE FUNCTION pg_catalog.alter_bioseq_table_set(
    table_name regclass,
    chunk_group_row_limit int DEFAULT NULL,
    stripe_row_limit int DEFAULT NULL,
    compression name DEFAULT null,
    compression_level int DEFAULT NULL)
    RETURNS void
    LANGUAGE C
AS 'MODULE_PATHNAME', 'alter_bioseq_table_set';

COMMENT ON FUNCTION pg_catalog.alter_bioseq_table_set(
    table_name regclass,
    chunk_group_row_limit int,
    stripe_row_limit int,
    compression name,
    compression_level int)
IS 'set one or more options on a bioseq table, when set to NULL no change is made';


--#include "udfs/alter_bioseq_table_reset/10.0-1.sql"
CREATE OR REPLACE FUNCTION pg_catalog.alter_bioseq_table_reset(
    table_name regclass,
    chunk_group_row_limit bool DEFAULT false,
    stripe_row_limit bool DEFAULT false,
    compression bool DEFAULT false,
    compression_level bool DEFAULT false)
    RETURNS void
    LANGUAGE C
AS 'MODULE_PATHNAME', 'alter_bioseq_table_reset';

COMMENT ON FUNCTION pg_catalog.alter_bioseq_table_reset(
    table_name regclass,
    chunk_group_row_limit bool,
    stripe_row_limit bool,
    compression bool,
    compression_level bool)
IS 'reset on or more options on a bioseq table to the system defaults';

  $$;
END IF;
END$proc$;

-- (this function being dropped in 10.0.3)->#include "udfs/bioseq_ensure_objects_exist/10.0-1.sql"

RESET search_path;

-- bioseq--10.0.-1 --10.0.2
GRANT USAGE ON SCHEMA bioseq TO PUBLIC;
GRANT SELECT ON ALL tables IN SCHEMA bioseq TO PUBLIC ;

-- bioseq--10.0-3--10.1-1.sql

-- Drop foreign keys between bioseq metadata tables.


-- bioseq--10.1-1--10.2-1.sql

-- For a proper mapping between tid & (stripe, row_num), add a new column to
-- bioseq.stripe and define a BTREE index on this column.
-- Also include storage_id column for per-relation scans.


-- Populate first_row_number column of bioseq.stripe table.
--
-- For simplicity, we calculate MAX(row_count) value across all the stripes
-- of all the columanar tables and then use it to populate first_row_number
-- column. This would introduce some gaps however we are okay with that since
-- it's already the case with regular INSERT/COPY's.
DO $$
DECLARE
  max_row_count bigint;
  -- this should be equal to bioseq_storage.h/COLUMNAR_FIRST_ROW_NUMBER
  COLUMNAR_FIRST_ROW_NUMBER constant bigint := 1;
BEGIN
  SELECT MAX(row_count) INTO max_row_count FROM bioseq.stripe;
  UPDATE bioseq.stripe SET first_row_number = COLUMNAR_FIRST_ROW_NUMBER +
                                                (stripe_num - 1) * max_row_count;
END;
$$;

-- bioseq--10.2-1--10.2-2.sql

-- revoke read access for bioseq.chunk from unprivileged
-- user as it contains chunk min/max values
REVOKE SELECT ON bioseq.chunk FROM PUBLIC;


-- bioseq--10.2-2--10.2-3.sql

-- Since stripe_first_row_number_idx is required to scan a bioseq table, we
-- need to make sure that it is created before doing anything with bioseq
-- tables during pg upgrades.
--
-- However, a plain btree index is not a dependency of a table, so pg_upgrade
-- cannot guarantee that stripe_first_row_number_idx gets created when
-- creating bioseq.stripe, unless we make it a unique "constraint".
--
-- To do that, drop stripe_first_row_number_idx and create a unique
-- constraint with the same name to keep the code change at minimum.

-- bioseq--10.2-3--10.2-4.sql


-- bioseq--11.0-2--11.1-1.sql

CREATE OR REPLACE FUNCTION pg_catalog.alter_bioseq_table_set(
    table_name regclass,
    chunk_group_row_limit int DEFAULT NULL,
    stripe_row_limit int DEFAULT NULL,
    compression name DEFAULT null,
    compression_level int DEFAULT NULL)
    RETURNS void
    LANGUAGE plpgsql AS
$alter_bioseq_table_set$
declare
  noop BOOLEAN := true;
  cmd  TEXT    := 'ALTER TABLE ' || table_name::text || ' SET (';
begin
  if (chunk_group_row_limit is not null) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd  || 'bioseq.chunk_group_row_limit=' || chunk_group_row_limit;
    noop := false;
  end if;
  if (stripe_row_limit is not null) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd || 'bioseq.stripe_row_limit=' || stripe_row_limit;
    noop := false;
  end if;
  if (compression is not null) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd || 'bioseq.compression=' || compression;
    noop := false;
  end if;
  if (compression_level is not null) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd || 'bioseq.compression_level=' || compression_level;
    noop := false;
  end if;
  cmd := cmd || ')';
  if (not noop) then
    execute cmd;
  end if;
  return;
end;
$alter_bioseq_table_set$;

COMMENT ON FUNCTION pg_catalog.alter_bioseq_table_set(
    table_name regclass,
    chunk_group_row_limit int,
    stripe_row_limit int,
    compression name,
    compression_level int)
IS 'set one or more options on a bioseq table, when set to NULL no change is made';

CREATE OR REPLACE FUNCTION pg_catalog.alter_bioseq_table_reset(
    table_name regclass,
    chunk_group_row_limit bool DEFAULT false,
    stripe_row_limit bool DEFAULT false,
    compression bool DEFAULT false,
    compression_level bool DEFAULT false)
    RETURNS void
    LANGUAGE plpgsql AS
$alter_bioseq_table_reset$
declare
  noop BOOLEAN := true;
  cmd  TEXT    := 'ALTER TABLE ' || table_name::text || ' RESET (';
begin
  if (chunk_group_row_limit) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd   || 'bioseq.chunk_group_row_limit';
    noop := false;
  end if;
  if (stripe_row_limit) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd || 'bioseq.stripe_row_limit';
    noop := false;
  end if;
  if (compression) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd || 'bioseq.compression';
    noop := false;
  end if;
  if (compression_level) then
    if (not noop) then cmd := cmd || ', '; end if;
    cmd := cmd || 'bioseq.compression_level';
    noop := false;
  end if;
  cmd := cmd || ')';
  if (not noop) then
    execute cmd;
  end if;
  return;
end;
$alter_bioseq_table_reset$;

COMMENT ON FUNCTION pg_catalog.alter_bioseq_table_reset(
    table_name regclass,
    chunk_group_row_limit bool,
    stripe_row_limit bool,
    compression bool,
    compression_level bool)
IS 'reset on or more options on a bioseq table to the system defaults';

-- ============================================================================
-- SIDECAR TABLES (heap). See RESEARCH/SPEC_MINIMIZER_STORAGE.md.
--
--   index_sidecar: serialized TargetIndex blob per (user_relid, rowno).
--   meta_sidecar:  per-sequence metadata (symbol counts). Count is
--                  hardcoded to 49 A's for this POC.
-- ============================================================================
CREATE TABLE IF NOT EXISTS bioseq.index_sidecar (
    user_relid      oid    NOT NULL,
    sequence_rowno  bigint NOT NULL,
    target_index    bytea  NOT NULL,
    PRIMARY KEY (user_relid, sequence_rowno)
);
COMMENT ON TABLE bioseq.index_sidecar IS
    'Serialized minimizer TargetIndex, keyed by (user_relid, rowno).';

CREATE TABLE IF NOT EXISTS bioseq.meta_sidecar (
    user_relid      oid      NOT NULL,
    sequence_rowno  bigint   NOT NULL,
    alphabet_code   smallint NOT NULL,
    symbol_counts   bigint[] NOT NULL,         -- indexed by seqan3 rank
    PRIMARY KEY (user_relid, sequence_rowno)
);
COMMENT ON TABLE bioseq.meta_sidecar IS
    'Per-sequence symbol counts. symbol_counts[rank+1] is the count for '
    'the rank-th letter in the alphabet identified by alphabet_code.';

-- ============================================================================
-- METADATA AGE LOG.
--
-- Tracks the most recent (recompute, INSERT-batch) timestamp per
-- (user_relid, kind). Read by bioseq.list_metadata(); written by
-- bioseq.update_index() and (throttled to once per minute via a
-- backend-local in-memory cache) by sidecar_after_insert. The throttle
-- keeps high-throughput INSERT workloads off this single row.
--
-- Plain heap-AM table on purpose: writes here must NOT trigger the
-- bioseq AM's sidecar_after_insert hook (which would recurse). When a
-- future bioseq-AM table needs an age log, the recursion guard in
-- sidecar_after_insert will need to cover it explicitly.
-- ============================================================================
CREATE TABLE IF NOT EXISTS bioseq.metadata_age_log (
    user_relid    oid          NOT NULL,
    kind          text         NOT NULL,
    last_updated  timestamptz  NOT NULL DEFAULT now(),
    PRIMARY KEY (user_relid, kind)
);
COMMENT ON TABLE bioseq.metadata_age_log IS
    'Last update timestamp per (user_relid, metadata kind). Read by '
    'bioseq.list_metadata(); written by bioseq.update_index() and '
    '(throttled) by sidecar_after_insert.';

-- rename bioseq schema to bioseq_internal and tighten security

REVOKE ALL PRIVILEGES ON ALL TABLES IN SCHEMA bioseq FROM PUBLIC;
ALTER SCHEMA bioseq RENAME TO bioseq_internal;
REVOKE ALL PRIVILEGES ON SCHEMA bioseq_internal FROM PUBLIC;

-- create bioseq schema with public usage privileges

CREATE SCHEMA bioseq;
GRANT USAGE ON SCHEMA bioseq TO PUBLIC;

--#include "udfs/upgrade_bioseq_storage/10.2-1.sql"
CREATE OR REPLACE FUNCTION bioseq_internal.upgrade_bioseq_storage(rel regclass)
  RETURNS VOID
  STRICT
  LANGUAGE c AS 'MODULE_PATHNAME', $$upgrade_bioseq_storage$$;

COMMENT ON FUNCTION bioseq_internal.upgrade_bioseq_storage(regclass)
  IS 'function to upgrade the bioseq storage, if necessary';


--#include "udfs/downgrade_bioseq_storage/10.2-1.sql"

CREATE OR REPLACE FUNCTION bioseq_internal.downgrade_bioseq_storage(rel regclass)
  RETURNS VOID
  STRICT
  LANGUAGE c AS 'MODULE_PATHNAME', $$downgrade_bioseq_storage$$;

COMMENT ON FUNCTION bioseq_internal.downgrade_bioseq_storage(regclass)
  IS 'function to downgrade the bioseq storage, if necessary';

-- update UDF to account for bioseq_internal schema
CREATE OR REPLACE FUNCTION bioseq_internal.bioseq_ensure_am_depends_catalog()
  RETURNS void
  LANGUAGE plpgsql
  SET search_path = pg_catalog
AS $func$
BEGIN
  INSERT INTO pg_depend
  WITH bioseq_schema_members(relid) AS (
    SELECT pg_class.oid AS relid FROM pg_class
      WHERE relnamespace =
            COALESCE(
	       (SELECT pg_namespace.oid FROM pg_namespace WHERE nspname = 'bioseq_internal'),
	       (SELECT pg_namespace.oid FROM pg_namespace WHERE nspname = 'bioseq')
	    )
        AND relname IN ('chunk',
                        'chunk_group',
                        'chunk_group_pkey',
                        'chunk_pkey',
                        'options',
                        'options_pkey',
                        'storageid_seq',
                        'stripe',
                        'stripe_first_row_number_idx',
                        'stripe_pkey')
  )
  SELECT -- Define a dependency edge from "bioseq table access method" ..
         'pg_am'::regclass::oid as classid,
         (select oid from pg_am where amname = 'bioseq') as objid,
         0 as objsubid,
         -- ... to each object that is registered to pg_class and that lives
         -- in "bioseq" schema. That contains catalog tables, indexes
         -- created on them and the sequences created in "bioseq" schema.
         --
         -- Given the possibility of user might have created their own objects
         -- in bioseq schema, we explicitly specify list of objects that we
         -- are interested in.
         'pg_class'::regclass::oid as refclassid,
         bioseq_schema_members.relid as refobjid,
         0 as refobjsubid,
         'n' as deptype
  FROM bioseq_schema_members
  -- Avoid inserting duplicate entries into pg_depend.
  EXCEPT TABLE pg_depend;
END;
$func$;
COMMENT ON FUNCTION bioseq_internal.bioseq_ensure_am_depends_catalog()
  IS 'internal function responsible for creating dependencies from bioseq '
     'table access method to the rel objects in bioseq schema';

SELECT bioseq_internal.bioseq_ensure_am_depends_catalog();

-- add utility function

CREATE FUNCTION bioseq.get_storage_id(regclass) RETURNS bigint
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$bioseq_relation_storageid$$;

-- create views for bioseq table information

CREATE VIEW bioseq.storage WITH (security_barrier) AS
  SELECT c.oid::regclass AS relation,
         bioseq.get_storage_id(c.oid) AS storage_id
    FROM pg_class c, pg_am am
    WHERE c.relam = am.oid AND am.amname = 'bioseq'
      AND pg_has_role(c.relowner, 'USAGE');
COMMENT ON VIEW bioseq.storage IS 'Columnar relation ID to storage ID mapping.';
GRANT SELECT ON bioseq.storage TO PUBLIC;

CREATE VIEW bioseq.options WITH (security_barrier) AS
  SELECT regclass AS relation, chunk_group_row_limit,
         stripe_row_limit, compression, compression_level
    FROM bioseq_internal.options o, pg_class c
    WHERE o.regclass = c.oid
      AND pg_has_role(c.relowner, 'USAGE');
COMMENT ON VIEW bioseq.options
  IS 'Columnar options for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.options TO PUBLIC;

CREATE VIEW bioseq.stripe WITH (security_barrier) AS
  SELECT relation, storage.storage_id, stripe_num, file_offset, data_length,
         column_count, chunk_row_count, row_count, chunk_group_count, first_row_number
    FROM bioseq_internal.stripe stripe, bioseq.storage storage
    WHERE stripe.storage_id = storage.storage_id;
COMMENT ON VIEW bioseq.stripe
  IS 'Columnar stripe information for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.stripe TO PUBLIC;

CREATE VIEW bioseq.chunk_group WITH (security_barrier) AS
  SELECT relation, storage.storage_id, stripe_num, chunk_group_num, row_count
    FROM bioseq_internal.chunk_group cg, bioseq.storage storage
    WHERE cg.storage_id = storage.storage_id;
COMMENT ON VIEW bioseq.chunk_group
  IS 'Columnar chunk group information for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.chunk_group TO PUBLIC;

CREATE VIEW bioseq.chunk WITH (security_barrier) AS
  SELECT relation, storage.storage_id, stripe_num, attr_num, chunk_group_num,
         minimum_value, maximum_value, value_stream_offset, value_stream_length,
         exists_stream_offset, exists_stream_length, value_compression_type,
         value_compression_level, value_decompressed_length, value_count
    FROM bioseq_internal.chunk chunk, bioseq.storage storage
    WHERE chunk.storage_id = storage.storage_id;
COMMENT ON VIEW bioseq.chunk
  IS 'Columnar chunk information for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.chunk TO PUBLIC;

-- bioseq--11.1-8--11.1-9.sql

-- count

CREATE FUNCTION vemptycount(int8) RETURNS int8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE FUNCTION vanycount(int8, "any") RETURNS int8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;

CREATE AGGREGATE vcount(*) (SFUNC = vemptycount, STYPE = int8, INITCOND="0");
CREATE AGGREGATE vcount("any") (SFUNC = vanycount, STYPE = int8, INITCOND="0");

-- int2 / int4

CREATE FUNCTION vint2int4avg(internal) RETURNS numeric AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;

-- int2

CREATE FUNCTION vint2sum(int8, int2) RETURNS int8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vsum(int2) (SFUNC = vint2sum, STYPE = int8, INITCOND="0");

CREATE FUNCTION vint2acc(internal, int2) RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vavg(int2) (SFUNC = vint2acc, STYPE = internal, FINALFUNC = vint2int4avg, INITCOND = '{0,0}');

CREATE FUNCTION vint2larger(int2, int2) RETURNS int2 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmax(int2) (SFUNC = vint2larger, STYPE = int2, INITCOND="-32768");

CREATE FUNCTION vint2smaller(int2, int2) RETURNS int2 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmin(int2) (SFUNC = vint2smaller, STYPE = int2, INITCOND="32767");

-- int4

CREATE FUNCTION vint4sum(int8, int4) RETURNS int8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vsum(int4) (SFUNC = vint4sum, STYPE = int8, INITCOND="0");

CREATE FUNCTION vint4acc(internal, int4) RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vavg(int4) (SFUNC = vint4acc, STYPE = internal, FINALFUNC = vint2int4avg, INITCOND = '{0,0}');

CREATE FUNCTION vint4larger(int4, int4) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmax(int4) (SFUNC = vint4larger, STYPE = int4, INITCOND="-2147483648");

CREATE FUNCTION vint4smaller(int4, int4) RETURNS int4 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmin(int4) (SFUNC = vint4smaller, STYPE = int4, INITCOND="2147483647");


-- int8

CREATE FUNCTION vint8acc(internal, int8) RETURNS internal AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;

CREATE FUNCTION vint8sum(internal) RETURNS numeric AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vsum(int8) (SFUNC = vint8acc, STYPE = internal, FINALFUNC = vint8sum, 
                             SERIALFUNC = int8_avg_serialize, DESERIALFUNC = int8_avg_deserialize);

CREATE FUNCTION vint8avg(internal) RETURNS numeric AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vavg(int8) (SFUNC = vint8acc, STYPE = internal, FINALFUNC = vint8avg, 
                             SERIALFUNC = int8_avg_serialize, DESERIALFUNC = int8_avg_deserialize);

CREATE FUNCTION vint8larger(int8, int8) RETURNS int8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmax(int8) (SFUNC = vint8larger, STYPE = int8, INITCOND="-9223372036854775808");

CREATE FUNCTION vint8smaller(int8, int8) RETURNS int8 AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmin(int8) (SFUNC = vint8smaller, STYPE = int8, INITCOND="9223372036854775807");

-- date

CREATE FUNCTION vdatelarger(date, date) RETURNS date AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmax(date) (SFUNC = vdatelarger, STYPE = date, INITCOND="-infinity");

CREATE FUNCTION vdatesmaller(date, date) RETURNS date AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE;
CREATE AGGREGATE vmin(date) (SFUNC = vdatesmaller, STYPE = date, INITCOND="infinity");


-- char

CREATE FUNCTION vchareq(char, char) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vcharne(char, char) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vchargt(char, char) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vcharlt(char, char) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vcharle(char, char) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vcharge(char, char) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- int2

CREATE FUNCTION vint2eq(int2, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint2ne(int2, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint2gt(int2, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint2lt(int2, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint2le(int2, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint2ge(int2, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vint24eq(int2, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint24ne(int2, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint24gt(int2, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint24lt(int2, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint24le(int2, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint24ge(int2, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vint28eq(int2, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint28ne(int2, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint28gt(int2, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint28lt(int2, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint28le(int2, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint28ge(int2, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- int4

CREATE FUNCTION vint4eq(int4, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint4ne(int4, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint4gt(int4, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint4lt(int4, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint4le(int4, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint4ge(int4, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vint42eq(int4, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint42ne(int4, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint42gt(int4, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint42lt(int4, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint42le(int4, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint42ge(int4, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vint48eq(int4, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint48ne(int4, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint48gt(int4, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint48lt(int4, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint48le(int4, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint48ge(int4, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- int8

CREATE FUNCTION vint8eq(int8, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint8ne(int8, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint8gt(int8, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint8lt(int8, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint8le(int8, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint8ge(int8, int8) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vint82eq(int8, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint82ne(int8, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint82gt(int8, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint82lt(int8, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint82le(int8, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint82ge(int8, int2) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION vint84eq(int8, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint84ne(int8, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint84gt(int8, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint84lt(int8, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint84le(int8, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vint84ge(int8, int4) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- date

CREATE FUNCTION vdate_eq(date, date) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vdate_ne(date, date) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vdate_gt(date, date) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vdate_lt(date, date) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vdate_le(date, date) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vdate_ge(date, date) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- time

CREATE FUNCTION vtime_eq(time, time) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtime_ne(time, time) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtime_gt(time, time) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtime_lt(time, time) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtime_le(time, time) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtime_ge(time, time) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- timestamp

CREATE FUNCTION vtimestamp_eq(timestamp, timestamp) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamp_ne(timestamp, timestamp) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamp_gt(timestamp, timestamp) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamp_lt(timestamp, timestamp) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamp_le(timestamp, timestamp) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamp_ge(timestamp, timestamp) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;

-- timestamptz

CREATE FUNCTION vtimestamptz_eq(timestamptz, timestamptz) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamptz_ne(timestamptz, timestamptz) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamptz_gt(timestamptz, timestamptz) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamptz_lt(timestamptz, timestamptz) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamptz_le(timestamptz, timestamptz) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;
CREATE FUNCTION vtimestamptz_ge(timestamptz, timestamptz) RETURNS bool AS 'MODULE_PATHNAME' LANGUAGE C IMMUTABLE STRICT;


CREATE FUNCTION mmsearch_support(internal)
    RETURNS internal
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE;

-- bioseq_placeholder_support is the SUPPORT function attached to every
-- bioseq.* projection (score, cigar, target_start, ...). It receives a
-- SupportRequestSimplify and, when the planner is processing a query
-- with JOINs or aggregates, wraps the FuncExpr in a PlaceHolderVar so
-- the projection stays anchored to the originating scan level and is
-- not duplicated into sibling join children's targetlists. Without it,
-- a LEFT JOIN with `bioseq.cigar(t1.a)` and `WHERE mmsearch(t1.seq, ...)`
-- crashes the backend because the inner side ends up with a copy of
-- the cigar() FuncExpr but no MatchResult to back the placeholder.
-- Inspired by paradedb pg_search's `placeholder_support` — see
-- main/bioseq/RESEARCH/REPORT_POSTGRES_QUERY_BUGFIXING_1.md §3.
CREATE FUNCTION bioseq_placeholder_support(internal)
    RETURNS internal
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE;

-- mmsearch arg types reference bioseq.seq; see the bottom of this file for its
-- full definition. The function body itself is declared below, after the type
-- registration block, because the SQL parser needs the type to exist to accept
-- the argument types here.

CREATE OR REPLACE FUNCTION bioseq.score(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_score_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.dp_score(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_dp_score_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_len(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_query_len_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_start(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_query_start_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_end(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_query_end_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_len(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_target_len_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_start(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_target_start_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_end(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_target_end_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.num_matches(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_num_matches_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.block_length(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_block_length_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.mapq(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_mapq_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.chain_score(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_chain_score_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.dp_max_score(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_dp_max_score_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.seed_count(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_seed_count_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.secondary_chain_score(el ANYELEMENT)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME', 'bioseq_secondary_chain_score_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.is_reverse(el ANYELEMENT)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'bioseq_is_reverse_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.is_primary(el ANYELEMENT)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'bioseq_is_primary_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.has_cigar(el ANYELEMENT)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'bioseq_has_cigar_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.divergence(el ANYELEMENT)
    RETURNS REAL
    AS 'MODULE_PATHNAME', 'bioseq_divergence_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.cigar(el ANYELEMENT)
    RETURNS TEXT
    AS 'MODULE_PATHNAME', 'bioseq_cigar_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

-- ============================================================================
-- bioseq.seq -- parameterised compact-sequence type
-- (SPEC_HOW_TO_CONNECT_ALIGNER_3.md)
-- ============================================================================

-- 1. Shell type so the I/O functions below can reference it.
CREATE TYPE bioseq.seq;

-- 2. I/O, SEND/RECV, TYPMOD IN/OUT.
CREATE OR REPLACE FUNCTION bioseq.seq_in(cstring, oid, integer)
    RETURNS bioseq.seq
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_in'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_out(bioseq.seq)
    RETURNS cstring
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_out'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_recv(internal, oid, integer)
    RETURNS bioseq.seq
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_recv'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_send(bioseq.seq)
    RETURNS bytea
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_send'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_typmod_in(cstring[])
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_typmod_in'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_typmod_out(integer)
    RETURNS cstring
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_typmod_out'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- 3. Type registration: wires I/O funcs, sets storage + alignment.
CREATE TYPE bioseq.seq (
    INPUT          = bioseq.seq_in,
    OUTPUT         = bioseq.seq_out,
    RECEIVE        = bioseq.seq_recv,
    SEND           = bioseq.seq_send,
    TYPMOD_IN      = bioseq.seq_typmod_in,
    TYPMOD_OUT     = bioseq.seq_typmod_out,
    STORAGE        = extended,
    ALIGNMENT      = double,
    INTERNALLENGTH = VARIABLE
);

-- 3a. Self-cast function to apply typmod coercion after bioseq.seq_in.
CREATE OR REPLACE FUNCTION bioseq.seq_cast(bioseq.seq, integer, boolean)
    RETURNS bioseq.seq
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_cast'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE CAST (bioseq.seq AS bioseq.seq)
    WITH FUNCTION bioseq.seq_cast(bioseq.seq, integer, boolean)
    AS IMPLICIT;

-- 4. Comparison functions + operators.
CREATE OR REPLACE FUNCTION bioseq.seq_cmp(bioseq.seq, bioseq.seq)
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_cmp'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_lt(bioseq.seq, bioseq.seq)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_lt'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_le(bioseq.seq, bioseq.seq)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_le'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_eq(bioseq.seq, bioseq.seq)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_eq'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_ne(bioseq.seq, bioseq.seq)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_ne'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_ge(bioseq.seq, bioseq.seq)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_ge'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.seq_gt(bioseq.seq, bioseq.seq)
    RETURNS boolean
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_gt'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OPERATOR < (
    LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, PROCEDURE = bioseq.seq_lt,
    COMMUTATOR = >, NEGATOR = >=,
    RESTRICT = scalarltsel, JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, PROCEDURE = bioseq.seq_le,
    COMMUTATOR = >=, NEGATOR = >,
    RESTRICT = scalarlesel, JOIN = scalarlejoinsel
);

CREATE OPERATOR = (
    LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, PROCEDURE = bioseq.seq_eq,
    COMMUTATOR = =, NEGATOR = <>,
    RESTRICT = eqsel, JOIN = eqjoinsel,
    HASHES, MERGES
);

CREATE OPERATOR <> (
    LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, PROCEDURE = bioseq.seq_ne,
    COMMUTATOR = <>, NEGATOR = =,
    RESTRICT = neqsel, JOIN = neqjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, PROCEDURE = bioseq.seq_ge,
    COMMUTATOR = <=, NEGATOR = <,
    RESTRICT = scalargesel, JOIN = scalargejoinsel
);

CREATE OPERATOR > (
    LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, PROCEDURE = bioseq.seq_gt,
    COMMUTATOR = <, NEGATOR = <=,
    RESTRICT = scalargtsel, JOIN = scalargtjoinsel
);

-- 5. Btree operator class.
CREATE OPERATOR CLASS bioseq.seq_ops
    DEFAULT FOR TYPE bioseq.seq USING btree AS
    OPERATOR 1 < ,
    OPERATOR 2 <= ,
    OPERATOR 3 = ,
    OPERATOR 4 >= ,
    OPERATOR 5 > ,
    FUNCTION 1 bioseq.seq_cmp(bioseq.seq, bioseq.seq);

-- 6. bp_size / unit_size projection helpers.
CREATE OR REPLACE FUNCTION bioseq.len(bioseq.seq)
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_len'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.unit_len(bioseq.seq)
    RETURNS integer
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_unit_len'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ---------------------------------------------------------------------------
-- bioseq.symbol_count — O(n) scalar fallback + cheap custom-scan fast path.
--
-- The Postgres entry point recomputes count_symbols() on the passed-in
-- varlena, so it works correctly for literals and plain-heap tables. On a
-- columnar scan the VarInjector in the bioseq custom scan rewrites every
-- `symbol_count(Var, 'L')` call into a placeholder Const that is populated
-- per-row from bioseq_internal.meta_sidecar; this is the preferred path
-- and is several orders of magnitude cheaper than re-scanning every
-- sequence. The SUPPORT function inflates the planner cost so any query
-- that *can* go through the custom scan will prefer it.
-- ---------------------------------------------------------------------------
CREATE FUNCTION bioseq_symbol_count_support(internal)
    RETURNS internal
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.symbol_count(el bioseq.seq, sym text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_symbol_count'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE
    SUPPORT bioseq_symbol_count_support;

-- ============================================================================
-- mmsearch — WHERE-qual stub over two bioseq.seq values.
-- Declared here (after bioseq.seq is fully registered) so the parser can
-- resolve both argument types. The columnar custom scan rewrites the qual at
-- plan time; see SPEC_HOW_TO_CONNECT_ALIGNER_4.md for the dataflow.
-- ============================================================================
CREATE OR REPLACE FUNCTION mmsearch(el bioseq.seq, query bioseq.seq)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'mmsearch'
    LANGUAGE C IMMUTABLE STRICT
    SUPPORT mmsearch_support
    PARALLEL SAFE;

-- ============================================================================
-- aligner_options — composite type for per-call overrides of
-- bioseq.aligner.* GUCs. Field order MUST match apply_aligner_options_override
-- in columnar_customscan.cppm; adding/removing a field requires updating
-- both sides simultaneously.
--
-- NULL in any field means "inherit the corresponding bioseq.aligner.* GUC".
-- The aligner_options() helper below builds a row where every field is
-- defaulted to NULL so "aligner_options(max_occ => 69)" overrides only
-- max_occ and leaves every other field at its GUC value.
--
-- For chaining_algorithm / base_alignment_algorithm the user supplies the
-- enum ordinal (matching bioseq.aligner.chaining_algorithm's order:
-- 0 = scalar_mm2, 1 = simd_mm2, 2 = rmq; and
-- 0 = ksw2, 1 = block_aligner for base alignment).
-- ============================================================================
CREATE TYPE bioseq_aligner_options AS (
    kmer_size                 integer,
    window_size               integer,
    max_gap                   integer,
    max_gap_ref               integer,
    bandwidth                 integer,
    bandwidth_long            integer,
    max_chain_skip            integer,
    max_chain_iter            integer,
    min_seed_count            integer,
    min_chain_score           integer,
    chain_gap_scale           double precision,
    chain_skip_scale          double precision,
    mask_level                double precision,
    mask_len                  integer,
    pri_ratio                 double precision,
    best_n                    integer,
    match_score               integer,
    mismatch_pen              integer,
    gap_open1                 integer,
    gap_ext1                  integer,
    gap_open2                 integer,
    gap_ext2                  integer,
    sc_ambi                   integer,
    zdrop                     integer,
    end_bonus                 integer,
    max_occ                   integer,
    mid_occ                   integer,
    block_min_size            integer,
    block_max_size            integer,
    chaining_algorithm        integer,
    base_alignment_algorithm  integer
);

CREATE OR REPLACE FUNCTION aligner_options(
    kmer_size                 integer  DEFAULT NULL,
    window_size               integer  DEFAULT NULL,
    max_gap                   integer  DEFAULT NULL,
    max_gap_ref               integer  DEFAULT NULL,
    bandwidth                 integer  DEFAULT NULL,
    bandwidth_long            integer  DEFAULT NULL,
    max_chain_skip            integer  DEFAULT NULL,
    max_chain_iter            integer  DEFAULT NULL,
    min_seed_count            integer  DEFAULT NULL,
    min_chain_score           integer  DEFAULT NULL,
    chain_gap_scale           double precision DEFAULT NULL,
    chain_skip_scale          double precision DEFAULT NULL,
    mask_level                double precision DEFAULT NULL,
    mask_len                  integer  DEFAULT NULL,
    pri_ratio                 double precision DEFAULT NULL,
    best_n                    integer  DEFAULT NULL,
    match_score               integer  DEFAULT NULL,
    mismatch_pen              integer  DEFAULT NULL,
    gap_open1                 integer  DEFAULT NULL,
    gap_ext1                  integer  DEFAULT NULL,
    gap_open2                 integer  DEFAULT NULL,
    gap_ext2                  integer  DEFAULT NULL,
    sc_ambi                   integer  DEFAULT NULL,
    zdrop                     integer  DEFAULT NULL,
    end_bonus                 integer  DEFAULT NULL,
    max_occ                   integer  DEFAULT NULL,
    mid_occ                   integer  DEFAULT NULL,
    block_min_size            integer  DEFAULT NULL,
    block_max_size            integer  DEFAULT NULL,
    chaining_algorithm        integer  DEFAULT NULL,
    base_alignment_algorithm  integer  DEFAULT NULL
) RETURNS bioseq_aligner_options AS $$
    SELECT ROW(
        kmer_size, window_size, max_gap, max_gap_ref, bandwidth, bandwidth_long,
        max_chain_skip, max_chain_iter, min_seed_count, min_chain_score,
        chain_gap_scale, chain_skip_scale, mask_level, mask_len, pri_ratio,
        best_n, match_score, mismatch_pen, gap_open1, gap_ext1, gap_open2,
        gap_ext2, sc_ambi, zdrop, end_bonus, max_occ, mid_occ,
        block_min_size, block_max_size,
        chaining_algorithm, base_alignment_algorithm
    )::bioseq_aligner_options
$$ LANGUAGE SQL IMMUTABLE;

-- Overload of mmsearch that accepts an aligner_options payload. The third
-- argument is read by ColumnarScan_BeginCustomScan (see
-- apply_aligner_options_override in columnar_customscan.cppm) and overlays
-- the non-NULL fields on top of the bioseq.aligner.* GUC snapshot. The
-- function itself is still a tautology at the scalar level.
CREATE OR REPLACE FUNCTION mmsearch(el bioseq.seq, query bioseq.seq,
                                    opts bioseq_aligner_options)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME', 'mmsearch'
    LANGUAGE C IMMUTABLE
    SUPPORT mmsearch_support
    PARALLEL SAFE;

-- ============================================================================
-- bioseq.list_metadata — set-returning function listing every
-- (user_relid, metadata_kind) pair the extension knows about. The kind
-- set is populated at _PG_init() by the C++ MetadataKindRegistry; today's
-- set is {minimizers, symbol_counts}. Future kinds (e.g. sneaky_snake
-- runtime, bwa_fm_index, hnsw_minimizer_index) appear automatically when
-- their owning module registers them.
-- ============================================================================
CREATE OR REPLACE FUNCTION bioseq.list_metadata()
    RETURNS TABLE (
        table_name      regclass,
        name            text,
        mode            text,        -- 'PER_ROW' or 'PER_TABLE'
        insert_support  boolean,
        size            text,        -- pg_size_pretty output, e.g. '5.1 GB'
        age             interval     -- now() - last_updated; NULL if never updated
    )
    AS 'MODULE_PATHNAME', 'bioseq_list_metadata'
    LANGUAGE C VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION bioseq.list_metadata() IS
    'Enumerate every (user table, metadata kind) pair the extension '
    'manages, with mode (PER_ROW/PER_TABLE), insert-trigger support, '
    'estimated on-disk size, and estimated age since last update.';

-- ============================================================================
-- bioseq.update_index — bulk recompute / create the named metadata kind
-- for a given user table.
--
-- The first argument is regclass; pass the table name as a quoted text
-- literal (`bioseq.update_index('my_table', 'minimizers')`) or with an
-- explicit cast (`bioseq.update_index('my_table'::regclass,
-- 'minimizers')`). PG implicitly converts text → regclass via the
-- regclassin input function in the first form.
--
-- The function:
--   * raises ERRCODE_INVALID_PARAMETER_VALUE if `kind` is not registered;
--   * raises ERRCODE_INSUFFICIENT_PRIVILEGE if the caller cannot SELECT
--     from the user relation or INSERT into the backing sidecar;
--   * deletes any pre-existing entries for this (rel, kind) pair;
--   * scans the user relation and writes new entries;
--   * updates bioseq_internal.metadata_age_log;
--   * returns the count of entries written (PER_ROW: row count,
--     PER_TABLE: 1).
--
-- Not PARALLEL SAFE: writes to sidecars and the age-log.
-- ============================================================================
CREATE OR REPLACE FUNCTION bioseq.update_index(rel regclass, kind text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_update_index'
    LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION bioseq.update_index(regclass, text) IS
    'Recompute (or create) the named metadata kind for the given table. '
    'Returns the number of entries written. Used to backfill metadata '
    'after bulk INSERTs were run with bioseq.update_index_on_insert = off.';

