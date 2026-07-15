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

COMMENT ON TABLE row_mask IS 'BioseqColumn chunk mask metadata';
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

COMMENT ON TABLE stripe IS 'BioseqColumn per stripe metadata';

CREATE TABLE IF NOT EXISTS chunk_group (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    chunk_group_num int NOT NULL,
    row_count bigint NOT NULL,
    -- DOC0275: the C metadata layer (config.h Natts_bioseq_chunkgroup = 5,
    -- Anum_bioseq_chunkgroup_deleted_rows = 5) has always read, written and
    -- updated a 5th `deleted_rows` column: SaveChunkGroups writes it (0 at
    -- creation), UpdateChunkGroupDeletedRows (write_state_row_mask.cppm) bumps
    -- it on DML deletes, and ReadChunkGroupRowCounts reads it to gate the
    -- row-mask read in ReadStripeRowByRowNumber. The DDL was a 4-column
    -- partial port, so heap_deform_tuple only filled attrs 0..3 and
    -- ReadChunkGroupRowCounts read datumArray[4] = UNINITIALISED STACK as the
    -- deleted-row count. For a stripe with >1 chunk group that garbage was
    -- non-zero (observed 100000), so the read spuriously took the
    -- `deletedRows > 0` branch into ReadChunkRowMask and SIGSEGV'd. Adding the
    -- column makes the catalog match the code; deleted_rows is genuinely 0
    -- for append-only fragment storage.
    deleted_rows bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (storage_id, stripe_num, chunk_group_num)
);

COMMENT ON TABLE chunk_group IS 'BioseqColumn chunk group metadata';

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

COMMENT ON TABLE chunk IS 'BioseqColumn per chunk metadata';

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
  -- this should be equal to bioseq_storage.h/BIOSEQ_COLUMN_FIRST_ROW_NUMBER
  BIOSEQ_COLUMN_FIRST_ROW_NUMBER constant bigint := 1;
BEGIN
  SELECT MAX(row_count) INTO max_row_count FROM bioseq.stripe;
  UPDATE bioseq.stripe SET first_row_number = BIOSEQ_COLUMN_FIRST_ROW_NUMBER +
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
-- DOC0279 §12.3/§17 — index policy + age + table-wide-shard catalogs. Heap-AM.
-- The legacy single-blob index_sidecar / per-row meta_sidecar tables are GONE
-- (the per-fragment / INLINE chunked-storage indexes replace them; §17). The
-- minimizer index + symbol counts now live in the lazily-created seqidx_*
-- tables co-located with the sequence sub-table shards.
-- (Literal install DDL uses bioseq.*; runtime resolves bioseq_internal.* after
--  the ALTER SCHEMA rename below.)
-- ============================================================================
CREATE TABLE IF NOT EXISTS bioseq.index_policy (
    user_relid       oid  NOT NULL,
    main_relfilenode oid  NOT NULL,
    column_attno     int2 NOT NULL,
    index_name       text NOT NULL,
    enabled          bool NOT NULL DEFAULT TRUE,
    update_on_insert bool NOT NULL DEFAULT FALSE,
    is_active_read   bool NOT NULL DEFAULT FALSE,
    PRIMARY KEY (user_relid, main_relfilenode, column_attno, index_name)
) USING heap;
CREATE UNIQUE INDEX IF NOT EXISTS index_policy_one_active_read
    ON bioseq.index_policy (user_relid, main_relfilenode, column_attno) WHERE is_active_read;

-- DOC0279 §12.3 — re-keyed metadata_age_log: last successful index build/write
-- per (user_relid, main_relfilenode, column_attno, index_name). Read by
-- bioseq.list_metadata(); written by bioseq.update_index() and the INSERT-time
-- build. Plain heap-AM so writes never re-fire the bioseq AM insert hook.
CREATE TABLE IF NOT EXISTS bioseq.metadata_age_log (
    user_relid       oid         NOT NULL,
    main_relfilenode oid         NOT NULL,
    column_attno     int2        NOT NULL,
    index_name       text        NOT NULL,
    last_updated     timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_relid, main_relfilenode, column_attno, index_name)
) USING heap;

CREATE TABLE IF NOT EXISTS bioseq.index_table_shard (
    database_oid oid NOT NULL, tablespace_oid oid NOT NULL, main_relfilenode oid NOT NULL,
    column_attno int2 NOT NULL, index_name text NOT NULL, shard_idx int4 NOT NULL,
    index_relid oid NOT NULL, bytes_written bigint NOT NULL DEFAULT 0, is_full bool NOT NULL DEFAULT FALSE,
    PRIMARY KEY (database_oid, tablespace_oid, main_relfilenode, column_attno, index_name, shard_idx)
) USING heap;

-- ============================================================================
-- DOC0260 Phase A — length-bucketed sub-table sharding catalog.
--
-- One row per (database, tablespace, main-relfilenode, column, bucket, shard)
-- triple. Sub-tables are created lazily by the storage manager on first
-- INSERT into a (parent, column, bucket); the partial UNIQUE index enforces
-- "at most one active shard per (parent, column, bucket)" so a concurrent
-- inserter cannot create a duplicate active shard.
-- ============================================================================
CREATE TABLE IF NOT EXISTS bioseq.seq_sub_table (
    database_oid        oid    NOT NULL,
    tablespace_oid      oid    NOT NULL,
    main_relfilenode    oid    NOT NULL,
    column_attno        int2   NOT NULL,
    bucket_idx          int2   NOT NULL,
    shard_idx           int4   NOT NULL,
    sub_table_relid     oid    NOT NULL,
    sidecar_relid       oid    NOT NULL,
    bytes_written       bigint NOT NULL DEFAULT 0 CHECK (bytes_written >= 0),
    is_full             bool   NOT NULL DEFAULT FALSE,
    total_payload_bytes bigint NOT NULL DEFAULT 0 CHECK (total_payload_bytes >= 0),
    PRIMARY KEY (database_oid, tablespace_oid, main_relfilenode,
                 column_attno, bucket_idx, shard_idx)
);
COMMENT ON TABLE bioseq.seq_sub_table IS
    'DOC0260 Phase A: one row per (db, spc, relfilenode, column, bucket, shard) '
    'recording the sub-table relid, its sidecar relid, and bytes_written/is_full.';

-- DOC0260 §6.1 / DOC0268 Appendix — partial index over the ACTIVE (is_full =
-- FALSE) shard of each (database, tablespace, relfilenode, column, bucket).
-- This accelerates SubTableManager::lookup_active_shard. It is intentionally NOT a UNIQUE
-- index: is_full is a mutable predicate column, and a UNIQUE partial index on a
-- flipped predicate column trips false "duplicate key" violations during shard
-- rollover (PostgreSQL's uniqueness check sees the just-updated/deleted old
-- active-shard entry as a live conflict when the next shard is inserted — most
-- visibly under the cap=0 shard-per-insert sentinel, where the old shard is
-- marked full and a fresh active shard is created in lockstep). The
-- "one active shard per (parent, column, bucket)" invariant is enforced
-- instead by the per-bucket transactional advisory lock taken in
-- allocate_new_shard_and_open (§7.3) plus the SubTableManager::lookup_active_shard
-- recheck under that lock — exactly the serialization the spec already mandates
-- for concurrent shard creation.
CREATE INDEX IF NOT EXISTS seq_sub_table_active_shard_idx
    ON bioseq.seq_sub_table
        (database_oid, tablespace_oid, main_relfilenode, column_attno, bucket_idx)
    WHERE is_full = FALSE;

CREATE INDEX IF NOT EXISTS seq_sub_table_by_main_idx
    ON bioseq.seq_sub_table
        (database_oid, tablespace_oid, main_relfilenode);

-- rename bioseq schema to bioseq_internal and tighten security

REVOKE ALL PRIVILEGES ON ALL TABLES IN SCHEMA bioseq FROM PUBLIC;
ALTER SCHEMA bioseq RENAME TO bioseq_internal;
REVOKE ALL PRIVILEGES ON SCHEMA bioseq_internal FROM PUBLIC;

-- Revision-3 minimizer generations use one extension-owned, permanent,
-- database-wide allocator.  The C++ allocator validates this exact catalogue
-- shape before every nextval() so a shadowed, cycling, cached, or narrower
-- sequence cannot silently reuse an immutable generation identifier.
CREATE SEQUENCE bioseq_internal.minimizer_generation_id_seq
    AS bigint
    INCREMENT 1
    MINVALUE 1
    MAXVALUE 9223372036854775807
    START 1
    CACHE 1
    NO CYCLE;
REVOKE ALL ON SEQUENCE
    bioseq_internal.minimizer_generation_id_seq FROM PUBLIC;
COMMENT ON SEQUENCE bioseq_internal.minimizer_generation_id_seq IS
    'Extension-owned noncycling allocator for immutable revision-3 minimizer generation identifiers.';

-- A sequence locator is a physical address, not a content version.  Persist
-- one PostgreSQL-owned source epoch per immutable locator range so a cold
-- revision-3 reader can attest the visible source with one indexed catalogue
-- lookup instead of decoding a multi-Gbp sequence.  The checksum is a signed
-- BIGINT bit-container for the canonical CRC-64 calculated while fragments are
-- already streaming through the storage writer.
CREATE SEQUENCE bioseq_internal.sequence_source_epoch_seq
    AS bigint
    INCREMENT 1
    MINVALUE 1
    MAXVALUE 9223372036854775807
    START 1
    CACHE 1
    NO CYCLE;
REVOKE ALL ON SEQUENCE
    bioseq_internal.sequence_source_epoch_seq FROM PUBLIC;
COMMENT ON SEQUENCE bioseq_internal.sequence_source_epoch_seq IS
    'Extension-owned noncycling allocator for immutable sequence-source epochs.';

CREATE TABLE bioseq_internal.sequence_source_version (
    source_epoch               bigint   NOT NULL,
    user_relid                 oid      NOT NULL,
    database_oid               oid      NOT NULL,
    tablespace_oid             oid      NOT NULL,
    main_relfilenode           oid      NOT NULL,
    main_storage_id            bigint   NOT NULL,
    column_attno               int2     NOT NULL,
    locator_version            int2     NOT NULL,
    bucket_idx                 int2     NOT NULL,
    shard_idx                  int4     NOT NULL,
    fragment_relid             oid,
    fragment_storage_id        bigint   NOT NULL,
    first_sub_row_no           bigint   NOT NULL,
    num_fragments              bigint   NOT NULL,
    total_bp_size              bigint   NOT NULL,
    alphabet_code              int2     NOT NULL,
    storage_unit_byte_width    int2     NOT NULL,
    padding_letters            int4     NOT NULL,
    source_content_checksum    bigint   NOT NULL,
    state                      int2     NOT NULL,
    CONSTRAINT sequence_source_version_pkey
        PRIMARY KEY (source_epoch),
    CONSTRAINT sequence_source_version_source_epoch_check
        CHECK (source_epoch > 0),
    CONSTRAINT sequence_source_version_main_storage_id_check
        CHECK (main_storage_id > 0),
    CONSTRAINT sequence_source_version_column_attno_check
        CHECK (column_attno > 0),
    CONSTRAINT sequence_source_version_locator_version_check
        CHECK (locator_version = 3),
    CONSTRAINT sequence_source_version_bucket_idx_check
        CHECK (bucket_idx BETWEEN 0 AND 255),
    CONSTRAINT sequence_source_version_shard_idx_check
        CHECK (shard_idx >= 0),
    CONSTRAINT sequence_source_version_fragment_storage_id_check
        CHECK (fragment_storage_id >= 0),
    CONSTRAINT sequence_source_version_first_sub_row_no_check
        CHECK (first_sub_row_no >= 0),
    CONSTRAINT sequence_source_version_num_fragments_check
        CHECK (num_fragments BETWEEN 0 AND 4294967295),
    CONSTRAINT sequence_source_version_total_bp_size_check
        CHECK (total_bp_size >= 0),
    CONSTRAINT sequence_source_version_alphabet_code_check
        CHECK (alphabet_code BETWEEN 0 AND 255),
    CONSTRAINT sequence_source_version_storage_unit_byte_width_check
        CHECK (storage_unit_byte_width BETWEEN 1 AND 15),
    CONSTRAINT sequence_source_version_padding_letters_check
        CHECK (padding_letters BETWEEN 0 AND 65535),
    CONSTRAINT sequence_source_version_state_check
        CHECK (state IN (0, 1, 2)),
    CONSTRAINT sequence_source_version_fragment_shape_check
        CHECK ((num_fragments = 0
                AND fragment_relid IS NULL
                AND fragment_storage_id = 0
                AND first_sub_row_no = 0)
               OR
               (num_fragments > 0
                AND fragment_relid IS NOT NULL
                AND fragment_storage_id > 0))
) USING heap;
-- Non-empty immutable ranges may never be aliased by a second epoch. Empty
-- sources carry NULL fragment_relid, so PostgreSQL's default NULLS DISTINCT
-- uniqueness admits repeated empty sources without a partial/expression index.
-- That matters because this internal relation uses CatalogTupleInsert, whose
-- index-maintenance contract supports only ordinary, non-partial columns.
CREATE UNIQUE INDEX sequence_source_version_locator_key
    ON bioseq_internal.sequence_source_version
        (fragment_relid, fragment_storage_id, first_sub_row_no);
CREATE INDEX sequence_source_version_fragment_row_idx
    ON bioseq_internal.sequence_source_version
        (fragment_relid, first_sub_row_no);
REVOKE ALL ON TABLE
    bioseq_internal.sequence_source_version FROM PUBLIC;
COMMENT ON TABLE bioseq_internal.sequence_source_version IS
    'PostgreSQL-owned MVCC source epoch and insertion-time content checksum '
    'for one immutable sequence locator range; state 0=UNPUBLISHED, '
    '1=READY, 2=terminal INVALID.';

-- create bioseq schema with public usage privileges

CREATE SCHEMA bioseq;
GRANT USAGE ON SCHEMA bioseq TO PUBLIC;

-- DOC0260 Phase A — SECURITY DEFINER helper used by the storage manager
-- to register a freshly-CREATE'd sub-table / sidecar pair with the bioseq
-- extension. Owned by the extension owner so non-owner backends can lazily
-- allocate shards. See DOC0260 §7.3 for the race / fallback semantics.
CREATE OR REPLACE FUNCTION bioseq_internal.add_to_extension_secdef(
    sub_name text, sidecar_name text)
    RETURNS void
    LANGUAGE plpgsql
    SECURITY DEFINER
    SET search_path = pg_catalog, public
AS $$
BEGIN
    IF EXISTS (SELECT 1 FROM pg_extension WHERE extname = 'bioseq') THEN
        EXECUTE format('ALTER EXTENSION bioseq ADD TABLE bioseq_internal.%I',
                       sub_name);
        EXECUTE format('ALTER EXTENSION bioseq ADD TABLE bioseq_internal.%I',
                       sidecar_name);
        -- NOTE: pg_extension_config_dump() can only be called from a SQL
        -- script executed by CREATE EXTENSION (Postgres throws
        -- ERRCODE_INVALID_TRANSACTION_STATE otherwise). The lazily-created
        -- sub-tables / sidecars are extension members (via ALTER EXTENSION
        -- ADD TABLE above) but they cannot be flagged "skip rows on dump"
        -- here. That's OK in practice because the dump path goes through
        -- `pg_dump`'s SELECT/COPY which re-hydrates seq columns
        -- transparently and the extension's CREATE script re-creates the
        -- catalog from scratch on restore.
    END IF;
END $$;
REVOKE EXECUTE ON FUNCTION bioseq_internal.add_to_extension_secdef(text, text)
    FROM PUBLIC;
GRANT EXECUTE ON FUNCTION bioseq_internal.add_to_extension_secdef(text, text)
    TO PUBLIC;

-- DOC0260 Phase A — the seq_sub_table is an extension-owned catalog; skip
-- its rows in pg_dump so the extension's CREATE script re-creates a clean
-- catalog (lazy sub-tables are created on-demand by the first INSERT).
SELECT pg_extension_config_dump('bioseq_internal.seq_sub_table', 'WHERE FALSE');
SELECT pg_extension_config_dump(
    'bioseq_internal.sequence_source_version', 'WHERE FALSE');
-- DOC0279 — the new index catalogs carry OIDs/relfilenodes that do not survive
-- a dump/restore, so dump the table definition but no rows.
SELECT pg_extension_config_dump('bioseq_internal.index_policy', 'WHERE FALSE');
SELECT pg_extension_config_dump('bioseq_internal.metadata_age_log', 'WHERE FALSE');
SELECT pg_extension_config_dump('bioseq_internal.index_table_shard', 'WHERE FALSE');

-- DOC0260 §6.1 — operator-facing summary view over the seq_sub_table catalog.
-- LEFT JOIN pg_class so DDL races (a DROP TABLE racing this SELECT) return
-- NULL instead of erroring; downstream monitoring tools should null-check.
CREATE OR REPLACE VIEW bioseq.seq_sub_table_summary AS
SELECT
    s.database_oid,
    s.tablespace_oid,
    s.main_relfilenode,
    s.column_attno,
    s.bucket_idx,
    s.shard_idx,
    c1.relname AS shard_relation_name,
    c2.relname AS sidecar_relation_name,
    s.bytes_written,
    pg_size_pretty(s.bytes_written) AS payload_pretty_size,
    s.is_full,
    s.total_payload_bytes
FROM bioseq_internal.seq_sub_table s
LEFT JOIN pg_class c1 ON c1.oid = s.sub_table_relid
LEFT JOIN pg_class c2 ON c2.oid = s.sidecar_relid
ORDER BY s.database_oid, s.tablespace_oid, s.main_relfilenode,
         s.column_attno, s.bucket_idx, s.shard_idx;

GRANT SELECT ON bioseq.seq_sub_table_summary TO PUBLIC;

-- DOC0260 §11.4 — operator-facing UDF + procedure for shard listing and
-- vacuum. list_seq_storage_shards resolves the (db, spc, relfilenode) triple
-- for `rel` and returns the catalog rows that belong to it; the matching
-- VACUUM procedure walks the list, commits between shards, and skips rows
-- whose sub-table was dropped mid-loop.
CREATE OR REPLACE FUNCTION bioseq.list_seq_storage_shards(rel regclass)
    RETURNS TABLE (
        sub_relation     regclass,
        sidecar_relation regclass,
        column_attno     int2,
        bucket_idx       int2,
        shard_idx        int4,
        bytes_written    bigint,
        is_full          bool
    )
    LANGUAGE sql STABLE
AS $$
    WITH parent AS (
        SELECT
            (SELECT oid FROM pg_database WHERE datname = current_database())
                AS db_oid,
            COALESCE(NULLIF(c.reltablespace, 0),
                     (SELECT dattablespace FROM pg_database
                      WHERE datname = current_database()))
                AS spc_oid,
            c.relfilenode AS rfn
        FROM pg_class c
        WHERE c.oid = rel
    )
    SELECT
        s.sub_table_relid::regclass AS sub_relation,
        s.sidecar_relid::regclass   AS sidecar_relation,
        s.column_attno,
        s.bucket_idx,
        s.shard_idx,
        s.bytes_written,
        s.is_full
    FROM bioseq_internal.seq_sub_table s, parent p
    WHERE s.database_oid     = p.db_oid
      AND s.tablespace_oid   = p.spc_oid
      AND s.main_relfilenode = p.rfn
    ORDER BY s.column_attno, s.bucket_idx, s.shard_idx;
$$;

GRANT EXECUTE ON FUNCTION bioseq.list_seq_storage_shards(regclass) TO PUBLIC;

CREATE OR REPLACE PROCEDURE bioseq.vacuum_seq_storage(rel regclass)
    LANGUAGE plpgsql
AS $$
DECLARE
    r RECORD;
BEGIN
    -- DOC0260 §11.4 — materialise the shard list once into a TEMP table so
    -- the per-iteration COMMIT cannot invalidate the underlying cursor.
    CREATE TEMP TABLE IF NOT EXISTS _bioseq_shards_to_vacuum
        ON COMMIT DROP
        AS SELECT * FROM bioseq.list_seq_storage_shards(rel) WITH NO DATA;
    TRUNCATE _bioseq_shards_to_vacuum;
    INSERT INTO _bioseq_shards_to_vacuum
        SELECT * FROM bioseq.list_seq_storage_shards(rel);

    FOR r IN SELECT * FROM _bioseq_shards_to_vacuum LOOP
        COMMIT;
        BEGIN
            EXECUTE format('VACUUM (ANALYZE) %s', r.sub_relation);
            EXECUTE format('VACUUM (ANALYZE) %s', r.sidecar_relation);
        EXCEPTION WHEN undefined_table THEN
            CONTINUE;
        END;
    END LOOP;
END $$;

GRANT EXECUTE ON PROCEDURE bioseq.vacuum_seq_storage(regclass) TO PUBLIC;

-- DOC0260 §11.3 step 3 — orphan reaper. Sweeps the seq_sub_table catalog in
-- both directions: drops catalog rows whose sub_table_relid / sidecar_relid
-- is no longer in pg_class, and drops pg_class entries matching the
-- naming convention that have no catalog row. Operators run this as a
-- recurring administrative task (Phase D D-VACUUM ships a background
-- worker that calls it on a schedule). Idempotent and safe to repeat.
CREATE OR REPLACE FUNCTION bioseq.reap_orphan_seq_sub_tables()
    RETURNS TABLE (
        action      text,
        target      text
    )
    LANGUAGE plpgsql
AS $$
DECLARE
    r RECORD;
BEGIN
    /* Direction A: catalog rows pointing at missing pg_class entries. */
    FOR r IN
        SELECT s.database_oid::text || '/' ||
               s.main_relfilenode::text || '/' ||
               s.column_attno::text || '/' ||
               s.bucket_idx::text || '/' ||
               s.shard_idx::text AS k
        FROM bioseq_internal.seq_sub_table s
        WHERE s.database_oid = (SELECT oid FROM pg_database
                                WHERE datname = current_database())
          AND NOT EXISTS (SELECT 1 FROM pg_class c
                          WHERE c.oid = s.sub_table_relid)
    LOOP
        action := 'delete-catalog-row';
        target := r.k;
        RETURN NEXT;
    END LOOP;

    DELETE FROM bioseq_internal.seq_sub_table s
        WHERE s.database_oid = (SELECT oid FROM pg_database
                                WHERE datname = current_database())
          AND NOT EXISTS (SELECT 1 FROM pg_class c
                          WHERE c.oid = s.sub_table_relid);

    /* Direction B: pg_class entries with no catalog row. */
    FOR r IN
        SELECT n.nspname || '.' || quote_ident(c.relname) AS qname,
               c.relname AS rn
        FROM pg_class c
        JOIN pg_namespace n ON c.relnamespace = n.oid
        WHERE n.nspname = 'bioseq_internal'
          AND (c.relname LIKE 'seq_sub_%' OR c.relname LIKE 'idx_sidecar_%')
          AND NOT EXISTS (
              SELECT 1 FROM bioseq_internal.seq_sub_table s
              WHERE s.sub_table_relid = c.oid
                 OR s.sidecar_relid   = c.oid)
    LOOP
        BEGIN
            EXECUTE format('DROP TABLE IF EXISTS %s', r.qname);
            action := 'drop-relation';
            target := r.qname;
            RETURN NEXT;
        EXCEPTION WHEN OTHERS THEN
            action := 'drop-relation-failed:' || SQLERRM;
            target := r.qname;
            RETURN NEXT;
        END;
    END LOOP;
END $$;

GRANT EXECUTE ON FUNCTION bioseq.reap_orphan_seq_sub_tables() TO PUBLIC;

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
                        'minimizer_generation_id_seq',
                        'sequence_source_epoch_seq',
                        'sequence_source_version',
                        'sequence_source_version_fragment_row_idx',
                        'sequence_source_version_locator_key',
                        'sequence_source_version_pkey',
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
COMMENT ON VIEW bioseq.storage IS 'BioseqColumn relation ID to storage ID mapping.';
GRANT SELECT ON bioseq.storage TO PUBLIC;

CREATE VIEW bioseq.options WITH (security_barrier) AS
  SELECT regclass AS relation, chunk_group_row_limit,
         stripe_row_limit, compression, compression_level
    FROM bioseq_internal.options o, pg_class c
    WHERE o.regclass = c.oid
      AND pg_has_role(c.relowner, 'USAGE');
COMMENT ON VIEW bioseq.options
  IS 'BioseqColumn options for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.options TO PUBLIC;

CREATE VIEW bioseq.stripe WITH (security_barrier) AS
  SELECT relation, storage.storage_id, stripe_num, file_offset, data_length,
         column_count, chunk_row_count, row_count, chunk_group_count, first_row_number
    FROM bioseq_internal.stripe stripe, bioseq.storage storage
    WHERE stripe.storage_id = storage.storage_id;
COMMENT ON VIEW bioseq.stripe
  IS 'BioseqColumn stripe information for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.stripe TO PUBLIC;

CREATE VIEW bioseq.chunk_group WITH (security_barrier) AS
  SELECT relation, storage.storage_id, stripe_num, chunk_group_num, row_count
    FROM bioseq_internal.chunk_group cg, bioseq.storage storage
    WHERE cg.storage_id = storage.storage_id;
COMMENT ON VIEW bioseq.chunk_group
  IS 'BioseqColumn chunk group information for tables on which the current user has ownership privileges.';
GRANT SELECT ON bioseq.chunk_group TO PUBLIC;

CREATE VIEW bioseq.chunk WITH (security_barrier) AS
  SELECT relation, storage.storage_id, stripe_num, attr_num, chunk_group_num,
         minimum_value, maximum_value, value_stream_offset, value_stream_length,
         exists_stream_offset, exists_stream_length, value_compression_type,
         value_compression_level, value_decompressed_length, value_count
    FROM bioseq_internal.chunk chunk, bioseq.storage storage
    WHERE chunk.storage_id = storage.storage_id;
COMMENT ON VIEW bioseq.chunk
  IS 'BioseqColumn chunk information for tables on which the current user has ownership privileges.';
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

-- DOC0274 §1.8 — every absolute coordinate / length the aligner reports is
-- `sequence_size_t` (uint64) inside `MatchResult` (minimap_port.cppm:334).
-- The eight projections below (query_len … block_length) therefore RETURN
-- BIGINT (PG int8, int64-compatible) so positions/lengths in sequences
-- longer than `INT32_MAX` (~2.15 Gbp) are not silently truncated by the
-- custom-scan placeholder filler. The remaining helpers (score, mapq,
-- chain_score, …) are genuinely `int32_t` in `MatchResult`, so they stay
-- INTEGER — widening them would not recover any precision the source lacks.
CREATE OR REPLACE FUNCTION bioseq.query_len(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_query_len_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_start(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_query_start_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_end(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_query_end_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_len(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_target_len_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_start(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_target_start_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_end(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_target_end_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.num_matches(el ANYELEMENT)
    RETURNS BIGINT
    AS 'MODULE_PATHNAME', 'bioseq_num_matches_stub'
    LANGUAGE C IMMUTABLE STRICT
    PARALLEL SAFE
    SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.block_length(el ANYELEMENT)
    RETURNS BIGINT
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

-- 6. bp_size / unit_size projection helpers. DOC0260 §1.1 #10 widened
--    these from INT4 to BIGINT so sequences with bp_size > INT32_MAX
--    (Phase C and beyond) are not silently truncated. The C
--    implementation reads loc.total_bp_size (uint64) for locator-stub
--    values and hdr->bp_size for inline payloads.
DROP FUNCTION IF EXISTS bioseq.len(bioseq.seq);
DROP FUNCTION IF EXISTS bioseq.unit_len(bioseq.seq);

CREATE OR REPLACE FUNCTION bioseq.len(bioseq.seq)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_len'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE OR REPLACE FUNCTION bioseq.unit_len(bioseq.seq)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'pg_compact_sequence_unit_len'
    LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

-- ---------------------------------------------------------------------------
-- bioseq.symbol_count — DOC0279 §12.5. O(n) scalar fallback + cheap
-- custom-scan fast path, returning the REAL per-rank tally (no 49-stub).
--
-- The scalar entry computes tally_symbol_counts() over a literal's inline
-- bases. On a columnar scan the VarInjector rewrites every
-- `symbol_count(Var, 'L')` into a placeholder Const populated per-row from
-- the stored INLINE symbol_counts index (if present, gated by
-- bioseq.read_sequence_index) with an UNCONDITIONAL tally_symbol_counts
-- recompute fallback — so the value is never NULL for a present row. The
-- SUPPORT function inflates the planner cost so any query that *can* go
-- through the custom scan will prefer it.
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
-- index_read_requirement is an exact per-call contract:
-- 0 = allow current production policy,
-- 1 = require_absent (never read persisted target minimizer payloads),
-- 2 = require_compatible_persisted (force a revision-3 READY-compatible read
--     or error; current legacy MPF/MPS layouts do not satisfy this contract).
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
    base_alignment_algorithm  integer,
    minimizer_algorithm        integer,
    vulkan_enabled             boolean,
    vulkan_seed_hits_enabled   boolean,
    vulkan_index_cache_enabled boolean,
    index_read_requirement      integer
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
    base_alignment_algorithm  integer  DEFAULT NULL,
    minimizer_algorithm        integer  DEFAULT NULL,
    vulkan_enabled             boolean  DEFAULT NULL,
    vulkan_seed_hits_enabled   boolean  DEFAULT NULL,
    vulkan_index_cache_enabled boolean  DEFAULT NULL,
    index_read_requirement      integer  DEFAULT NULL
) RETURNS bioseq_aligner_options AS $$
    SELECT ROW(
        kmer_size, window_size, max_gap, max_gap_ref, bandwidth, bandwidth_long,
        max_chain_skip, max_chain_iter, min_seed_count, min_chain_score,
        chain_gap_scale, chain_skip_scale, mask_level, mask_len, pri_ratio,
        best_n, match_score, mismatch_pen, gap_open1, gap_ext1, gap_open2,
        gap_ext2, sc_ambi, zdrop, end_bonus, max_occ, mid_occ,
        block_min_size, block_max_size,
        chaining_algorithm, base_alignment_algorithm, minimizer_algorithm,
        vulkan_enabled, vulkan_seed_hits_enabled, vulkan_index_cache_enabled,
        index_read_requirement
    )::bioseq_aligner_options
$$ LANGUAGE SQL IMMUTABLE;

-- Overload of mmsearch that accepts an aligner_options payload. The third
-- argument is read by BioseqColumnScan_BeginCustomScan (see
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
-- bioseq.list_metadata — DOC0279 §12.1/§12.2. One row per (user bioseq table,
-- bioseq.seq column, registered index). The index set is the compile-time
-- pg_supported_indexes::list_types() registry; new indexes appear when the
-- registry grows (no runtime registration). Not gated on chunk existence, so
-- a fresh table already lists all production indexes with size '0 bytes' and
-- a NULL age.
-- ============================================================================
CREATE OR REPLACE FUNCTION bioseq.list_metadata()
    RETURNS TABLE (
        table_name      regclass,
        column_attno    int2,
        column_name     text,
        name            text,
        index_type      text,        -- 'CHUNKED_PER_FRAGMENT' | 'INLINE_PER_SEQUENCE' | 'CHUNKED_PER_TABLE'
        update_policy   text,        -- 'UPDATE_ON_INSERT' | 'UPDATE_MANUALLY'
        size            text,        -- pg_size_pretty output, e.g. '5.1 GB'
        age             interval     -- now() - last_updated; NULL if never updated
    )
    AS 'MODULE_PATHNAME', 'bioseq_list_metadata'
    LANGUAGE C VOLATILE PARALLEL SAFE;

COMMENT ON FUNCTION bioseq.list_metadata() IS
    'Enumerate every (user bioseq table, bioseq.seq column, registered index) '
    'triple, with index_type, update_policy (UPDATE_ON_INSERT/UPDATE_MANUALLY), '
    'derived on-disk size, and age since last successful build.';

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
-- The function (DOC0279 §11.4):
--   * raises ERRCODE_INVALID_PARAMETER_VALUE if `name` is not a registered
--     index (pg_supported_indexes::list_types());
--   * errors if the relation is not a USING bioseq table;
--   * clears the named index across all shards, then rebuilds it for every
--     bioseq.seq column of every row (scanner_pg-streamed, RSS-bounded);
--   * stamps bioseq_internal.metadata_age_log on success;
--   * returns the count of sequences (re)indexed.
--
-- Not PARALLEL SAFE: writes the seqidx_* chunk tables and the age-log.
-- ============================================================================
CREATE OR REPLACE FUNCTION bioseq.update_index(rel regclass, kind text)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_update_index'
    LANGUAGE C VOLATILE STRICT;

-- ============================================================================
-- DOC0295 §9.4 — bioseq.load(target_table, file_path, header_format, columns):
-- stream a FASTA/FASTQ file into a USING bioseq table via the postgres-agnostic
-- IO reader, parsing the sequence identifier (header) line into zero or more
-- typed destination columns. The table must have exactly one bioseq.seq column
-- (its typmod drives alphabet/width). header_format selects the dialect parser
-- ('auto' detects from sampled headers); columns => ARRAY[(column_name,
-- data_source)] explicitly maps a parsed field / 0-based positional index to a
-- column. Unmapped NOT NULL columns must have a default (evaluated per row).
-- When bioseq.update_index_on_insert is on, policy-enabled UPDATE_ON_INSERT
-- indexes are built in one batched pass after the load. Returns records loaded.
-- ============================================================================

-- New composite type for the columns => ARRAY[...] argument (DOC0295 §9.4).
CREATE TYPE bioseq.column_mapping AS (
    column_name  text,    -- target column (matched case/separator-insensitively)
    data_source  text     -- field name OR 0-based positional index (as text)
);

-- Replace the 2-arg signature with a 4-arg one (defaults preserve the old call).
-- Drops STRICT (columns/header_format may be NULL); the C code checks ARGISNULL.
DROP FUNCTION IF EXISTS bioseq.load(regclass, text);
CREATE OR REPLACE FUNCTION bioseq.load(
        target_table  regclass,
        file_path     text,
        header_format text                    DEFAULT 'auto',
        columns       bioseq.column_mapping[] DEFAULT NULL)
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_load'
    LANGUAGE C VOLATILE;        -- not STRICT
COMMENT ON FUNCTION bioseq.load(regclass, text, text, bioseq.column_mapping[]) IS
    'DOC0295: stream a FASTA/FASTQ file into a USING bioseq table, parsing the '
    'identifier (header) line into typed destination columns. One bioseq.seq '
    'column (required, typmod drives alphabet/width). header_format selects the '
    'dialect (''auto'' detects); columns => ARRAY[(column_name, data_source)] '
    'maps a parsed field or 0-based positional index to a column. Unmapped NOT '
    'NULL columns must have a default. Builds policy-enabled on-insert indexes '
    'in one batched pass when bioseq.update_index_on_insert is on. Returns '
    'records loaded.';

-- DOC0279 §12.2 — read the backend-local minimizers_per_sequence merge run
-- counter (test observability for the multi-cursor k-way merge).
CREATE OR REPLACE FUNCTION bioseq.last_index_merge_run_count()
    RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_last_index_merge_run_count'
    LANGUAGE C VOLATILE;

-- DOC0371 §5.3 — immutable backend-local snapshot of the most recently
-- finished alignment-owning executor root. One row per canonical stage.
CREATE OR REPLACE FUNCTION bioseq.last_alignment_execution_stats()
RETURNS TABLE (
    execution_id bigint,
    backend_pid integer,
    database_oid oid,
    effective_cpu_workers integer,
    gpu_device_uuid text,
    gpu_driver text,
    gpu_pipeline_cache_policy text,
    gpu_pipeline_cache_seed_sha256 text,
    gpu_pipeline_cache_seed_size bigint,
    gpu_pipeline_cache_manifest_checksum text,
    gpu_pipeline_cache_hit boolean,
    scan_node_count integer,
    nested_alignment_executor_count integer,
    completed boolean,
    partial boolean,
    fully_drained boolean,
    requested_backend text,
    actual_backend text,
    requested_target_index_source text,
    actual_target_index_source text,
    persisted_generation_id bigint,
    persisted_source_identity text,
    mixed_persisted_identity boolean,
    stage text,
    eligible_jobs bigint,
    executed_host_io_jobs bigint,
    executed_scalar_cpu_jobs bigint,
    executed_simd_cpu_jobs bigint,
    executed_vulkan_gpu_jobs bigint,
    fallback_jobs bigint,
    stage_host_wall_ns bigint,
    stage_gpu_device_ns bigint,
    target_rows bigint,
    attempted_alignment_jobs bigint,
    fully_attested_alignment_jobs bigint,
    unattested_alignment_jobs bigint,
    first_unattested_target_row bigint,
    first_unattested_query_ordinal bigint,
    cpu_prepared_index_builds bigint,
    cpu_prepared_index_reuses bigint,
    cpu_prepared_unique_keys bigint,
    cpu_prepared_duplicate_build_attempts bigint,
    cpu_prepared_key_set_checksum text,
    cpu_prepared_build_set_checksum text,
    operational_identity_conflict boolean,
    persisted_index_rows bigint,
    missing_persisted_index_rows bigint,
    recomputed_target_index_rows bigint,
    persisted_generation_read_sessions bigint,
    persisted_relation_opens bigint,
    persisted_index_scans bigint,
    persisted_head_scans bigint,
    persisted_head_rows_read bigint,
    persisted_head_bytes_read bigint,
    persisted_manifest_directory_scans bigint,
    persisted_manifest_rows_read bigint,
    persisted_manifest_bytes_read bigint,
    persisted_directory_chunks_read bigint,
    persisted_directory_bytes_read bigint,
    persisted_metadata_repeated_reads bigint,
    persisted_metadata_heap_rows_read bigint,
    persisted_metadata_bytes_read bigint,
    persisted_payload_heap_rows_read bigint,
    persisted_payload_bytes_read bigint,
    persisted_heap_rows_read bigint,
    persisted_toast_values_read bigint,
    persisted_detoast_bytes bigint,
    persisted_explicit_copy_bytes bigint,
    persisted_wanted_subkeys bigint,
    persisted_read_subkeys bigint,
    persisted_wanted_extents bigint,
    persisted_wanted_chunks bigint,
    persisted_skipped_rows bigint,
    persisted_repeated_chunk_reads bigint,
    persisted_extents_read bigint,
    persisted_chunks_read bigint,
    persisted_bytes_read bigint,
    persisted_bulk_read_calls bigint,
    persisted_planned_request_groups bigint,
    persisted_executed_request_groups bigint,
    persisted_range_scan_calls bigint,
    persisted_skip_budget_rows bigint,
    persisted_hash_build_calls bigint,
    persisted_assembly_ns bigint,
    pair_sort_calls bigint,
    host_pair_materialization_calls bigint,
    host_biological_hash_filter_calls bigint,
    host_query_hash_readbacks bigint,
    host_query_symbol_packing_calls bigint,
    host_target_symbol_packing_calls bigint,
    host_alignment_job_classification_calls bigint,
    host_anchor_concatenation_calls bigint,
    host_cigar_construction_calls bigint,
    full_segment_hydrations bigint,
    nthash_vector_batches bigint,
    nthash_scalar_backend_branches bigint,
    nthash_scalar_tail_symbols bigint,
    nthash_masked_tail_lanes bigint,
    simd_chain_vector_batches bigint,
    simd_chain_backtrack_vector_batches bigint,
    simd_chain_scalar_backend_branches bigint,
    simd_chain_backtrack_scalar_branches bigint,
    cpu_seed_positions_rejected_outside_collection bigint,
    gpu_partition_select_dispatches bigint,
    gpu_partition_anchor_merge_dispatches bigint,
    gpu_index_build_candidates bigint,
    gpu_index_build_canonical_emitted bigint,
    gpu_index_build_overlap_duplicates_rejected bigint,
    gpu_index_build_noncanonical_extras_rejected bigint,
    gpu_index_build_surviving_extras bigint,
    gpu_persisted_resident_uploads bigint,
    gpu_persisted_resident_cache_hits bigint,
    gpu_overlap_anchor_duplicates bigint,
    gpu_seed_dispatches bigint,
    gpu_chain_dispatches bigint,
    gpu_chain_backtrack_dispatches bigint,
    gpu_extend_dispatches bigint,
    gpu_target_window_request_dispatches bigint,
    gpu_target_window_logical_units_requested bigint,
    host_target_window_requests_consumed bigint,
    host_target_window_physical_units_read bigint,
    host_target_window_remap_units_filled bigint,
    host_target_window_packed_bytes_read bigint,
    gpu_runtime_init_ns bigint,
    gpu_pipeline_cache_loads bigint,
    gpu_pipeline_cache_seed_bytes bigint,
    gpu_pipeline_cache_load_ns bigint,
    gpu_pipeline_creation_ns bigint,
    gpu_submit_cpu_ns bigint,
    gpu_wait_cpu_ns bigint,
    gpu_ordinal_readback_cpu_ns bigint,
    gpu_result_readback_cpu_ns bigint,
    gpu_device_critical_path_ns bigint,
    gpu_timestamp_query_failures bigint,
    gpu_queue_submits bigint,
    gpu_host_waits bigint,
    gpu_buffer_creates bigint,
    gpu_memory_allocations bigint,
    gpu_pipeline_creates bigint,
    backend_declines bigint,
    backend_fallbacks bigint,
    stats_saturated boolean,
    persisted_source_parent_rows_read bigint,
    persisted_source_catalog_rows_read bigint,
    persisted_source_catalog_index_scans bigint,
    persisted_source_fragment_rows_read bigint,
    persisted_source_decoded_bp bigint,
    persisted_legacy_globalize_calls bigint,
    persisted_legacy_rebase_calls bigint,
    cold_index_preparation jsonb
)
LANGUAGE C VOLATILE PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'bioseq_last_alignment_execution_stats';

-- DOC0372: backward-compatible evidence ABI. The first 150 OUT columns are
-- type-for-type the v1 declaration above; only column 151 is new.
CREATE OR REPLACE FUNCTION bioseq.last_alignment_execution_stats_v2()
RETURNS TABLE (
    execution_id bigint,
    backend_pid integer,
    database_oid oid,
    effective_cpu_workers integer,
    gpu_device_uuid text,
    gpu_driver text,
    gpu_pipeline_cache_policy text,
    gpu_pipeline_cache_seed_sha256 text,
    gpu_pipeline_cache_seed_size bigint,
    gpu_pipeline_cache_manifest_checksum text,
    gpu_pipeline_cache_hit boolean,
    scan_node_count integer,
    nested_alignment_executor_count integer,
    completed boolean,
    partial boolean,
    fully_drained boolean,
    requested_backend text,
    actual_backend text,
    requested_target_index_source text,
    actual_target_index_source text,
    persisted_generation_id bigint,
    persisted_source_identity text,
    mixed_persisted_identity boolean,
    stage text,
    eligible_jobs bigint,
    executed_host_io_jobs bigint,
    executed_scalar_cpu_jobs bigint,
    executed_simd_cpu_jobs bigint,
    executed_vulkan_gpu_jobs bigint,
    fallback_jobs bigint,
    stage_host_wall_ns bigint,
    stage_gpu_device_ns bigint,
    target_rows bigint,
    attempted_alignment_jobs bigint,
    fully_attested_alignment_jobs bigint,
    unattested_alignment_jobs bigint,
    first_unattested_target_row bigint,
    first_unattested_query_ordinal bigint,
    cpu_prepared_index_builds bigint,
    cpu_prepared_index_reuses bigint,
    cpu_prepared_unique_keys bigint,
    cpu_prepared_duplicate_build_attempts bigint,
    cpu_prepared_key_set_checksum text,
    cpu_prepared_build_set_checksum text,
    operational_identity_conflict boolean,
    persisted_index_rows bigint,
    missing_persisted_index_rows bigint,
    recomputed_target_index_rows bigint,
    persisted_generation_read_sessions bigint,
    persisted_relation_opens bigint,
    persisted_index_scans bigint,
    persisted_head_scans bigint,
    persisted_head_rows_read bigint,
    persisted_head_bytes_read bigint,
    persisted_manifest_directory_scans bigint,
    persisted_manifest_rows_read bigint,
    persisted_manifest_bytes_read bigint,
    persisted_directory_chunks_read bigint,
    persisted_directory_bytes_read bigint,
    persisted_metadata_repeated_reads bigint,
    persisted_metadata_heap_rows_read bigint,
    persisted_metadata_bytes_read bigint,
    persisted_payload_heap_rows_read bigint,
    persisted_payload_bytes_read bigint,
    persisted_heap_rows_read bigint,
    persisted_toast_values_read bigint,
    persisted_detoast_bytes bigint,
    persisted_explicit_copy_bytes bigint,
    persisted_wanted_subkeys bigint,
    persisted_read_subkeys bigint,
    persisted_wanted_extents bigint,
    persisted_wanted_chunks bigint,
    persisted_skipped_rows bigint,
    persisted_repeated_chunk_reads bigint,
    persisted_extents_read bigint,
    persisted_chunks_read bigint,
    persisted_bytes_read bigint,
    persisted_bulk_read_calls bigint,
    persisted_planned_request_groups bigint,
    persisted_executed_request_groups bigint,
    persisted_range_scan_calls bigint,
    persisted_skip_budget_rows bigint,
    persisted_hash_build_calls bigint,
    persisted_assembly_ns bigint,
    pair_sort_calls bigint,
    host_pair_materialization_calls bigint,
    host_biological_hash_filter_calls bigint,
    host_query_hash_readbacks bigint,
    host_query_symbol_packing_calls bigint,
    host_target_symbol_packing_calls bigint,
    host_alignment_job_classification_calls bigint,
    host_anchor_concatenation_calls bigint,
    host_cigar_construction_calls bigint,
    full_segment_hydrations bigint,
    nthash_vector_batches bigint,
    nthash_scalar_backend_branches bigint,
    nthash_scalar_tail_symbols bigint,
    nthash_masked_tail_lanes bigint,
    simd_chain_vector_batches bigint,
    simd_chain_backtrack_vector_batches bigint,
    simd_chain_scalar_backend_branches bigint,
    simd_chain_backtrack_scalar_branches bigint,
    cpu_seed_positions_rejected_outside_collection bigint,
    gpu_partition_select_dispatches bigint,
    gpu_partition_anchor_merge_dispatches bigint,
    gpu_index_build_candidates bigint,
    gpu_index_build_canonical_emitted bigint,
    gpu_index_build_overlap_duplicates_rejected bigint,
    gpu_index_build_noncanonical_extras_rejected bigint,
    gpu_index_build_surviving_extras bigint,
    gpu_persisted_resident_uploads bigint,
    gpu_persisted_resident_cache_hits bigint,
    gpu_overlap_anchor_duplicates bigint,
    gpu_seed_dispatches bigint,
    gpu_chain_dispatches bigint,
    gpu_chain_backtrack_dispatches bigint,
    gpu_extend_dispatches bigint,
    gpu_target_window_request_dispatches bigint,
    gpu_target_window_logical_units_requested bigint,
    host_target_window_requests_consumed bigint,
    host_target_window_physical_units_read bigint,
    host_target_window_remap_units_filled bigint,
    host_target_window_packed_bytes_read bigint,
    gpu_runtime_init_ns bigint,
    gpu_pipeline_cache_loads bigint,
    gpu_pipeline_cache_seed_bytes bigint,
    gpu_pipeline_cache_load_ns bigint,
    gpu_pipeline_creation_ns bigint,
    gpu_submit_cpu_ns bigint,
    gpu_wait_cpu_ns bigint,
    gpu_ordinal_readback_cpu_ns bigint,
    gpu_result_readback_cpu_ns bigint,
    gpu_device_critical_path_ns bigint,
    gpu_timestamp_query_failures bigint,
    gpu_queue_submits bigint,
    gpu_host_waits bigint,
    gpu_buffer_creates bigint,
    gpu_memory_allocations bigint,
    gpu_pipeline_creates bigint,
    backend_declines bigint,
    backend_fallbacks bigint,
    stats_saturated boolean,
    persisted_source_parent_rows_read bigint,
    persisted_source_catalog_rows_read bigint,
    persisted_source_catalog_index_scans bigint,
    persisted_source_fragment_rows_read bigint,
    persisted_source_decoded_bp bigint,
    persisted_legacy_globalize_calls bigint,
    persisted_legacy_rebase_calls bigint,
    cold_index_preparation jsonb,
    simd_execution_evidence jsonb
)
LANGUAGE C VOLATILE PARALLEL UNSAFE
AS 'MODULE_PATHNAME', 'bioseq_last_alignment_execution_stats_v2';

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence()
RETURNS jsonb
LANGUAGE C VOLATILE PARALLEL UNSAFE
AS 'MODULE_PATHNAME',
   'bioseq_last_alignment_format5_reader_memory_evidence';

CREATE OR REPLACE FUNCTION bioseq.last_alignment_persisted_gpu_operation_evidence_v1()
RETURNS jsonb
LANGUAGE C VOLATILE PARALLEL RESTRICTED
AS 'MODULE_PATHNAME',
   'bioseq_last_alignment_persisted_gpu_operation_evidence_v1';

CREATE OR REPLACE FUNCTION bioseq_internal.reset_active_read_policy_debug()
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_reset_active_read_policy_debug'
LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION bioseq_internal.reset_active_read_policy_debug()
FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.active_read_policy_debug()
RETURNS TABLE (
    resolver_configured_rows bigint,
    resolver_authoritative_defaults bigint,
    resolver_non_relation bigint,
    dispatch_configured_batches bigint,
    dispatch_authoritative_default_batches bigint,
    dispatch_non_relation_batches bigint,
	last_dispatch_source text,
	prohibited_file_constructor_entries bigint,
	prohibited_file_writer_entries bigint
)
AS 'MODULE_PATHNAME', 'bioseq_internal_active_read_policy_debug'
LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION bioseq_internal.active_read_policy_debug()
FROM PUBLIC;

-- DOC0299/DOC0300 — backend-local snapshot of the most recent
-- minimizers_per_sequence sorted-run build in this backend. Query from the
-- same backend that executed mmsearch/update_index; the values intentionally
-- reset at the start of each build/update path.
CREATE OR REPLACE FUNCTION bioseq.last_mps_build_stats()
    RETURNS TABLE (
        backend_pid bigint,
        db_oid bigint,
        relfilenode bigint,
        sequence_rowno bigint,
        total_run_count bigint,
        resident_runs bigint,
        file_runs bigint,
        resident_run_bytes bigint,
        resident_peak_bytes bigint,
        spilled_run_bytes bigint,
        postgres_temp_file_bytes bigint,
        postgres_temp_file_count bigint,
        merge_reader_buffer_bytes bigint,
        merge_pass_count bigint,
        spill_count_from_pressure bigint,
        resident_demotions bigint,
        position_spill_bytes bigint,
        position_spill_count bigint
    )
    AS 'MODULE_PATHNAME', 'bioseq_last_mps_build_stats'
    LANGUAGE C VOLATILE;

-- DOC0301/A.7 — shared-memory snapshot of the most recent async
-- minimizers_per_sequence build completed by the static scheduler worker.
CREATE OR REPLACE FUNCTION bioseq.scheduler_mps_build_stats()
    RETURNS TABLE (
        backend_pid bigint,
        db_oid bigint,
        relfilenode bigint,
        sequence_rowno bigint,
        total_run_count bigint,
        resident_runs bigint,
        file_runs bigint,
        resident_run_bytes bigint,
        resident_peak_bytes bigint,
        spilled_run_bytes bigint,
        postgres_temp_file_bytes bigint,
        postgres_temp_file_count bigint,
        merge_reader_buffer_bytes bigint,
        merge_pass_count bigint,
        spill_count_from_pressure bigint,
        resident_demotions bigint,
        position_spill_bytes bigint,
        position_spill_count bigint,
        generation bigint,
        worker_build_ns bigint,
        pipeline_mode bigint,
        max_concurrency bigint,
        index_memory_cap_mb bigint,
        lazy_worker_shareable bigint
    )
    AS 'MODULE_PATHNAME', 'bioseq_scheduler_mps_build_stats'
    LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION bioseq_internal.arm_async_mps_worker_failure(
    failure_point text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'bioseq_internal_arm_async_mps_worker_failure'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.arm_async_mps_worker_failure(text)
    FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.disarm_async_mps_worker_failure()
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_disarm_async_mps_worker_failure'
LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.disarm_async_mps_worker_failure()
    FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.async_mps_worker_boundary_stats()
RETURNS TABLE (
    relation_leaf_calls bigint,
    spi_leaf_calls bigint,
    materialize_interrupt_leaf_calls bigint,
    materialize_scanner_read_leaf_calls bigint,
    pg_leaf_failures bigint,
    injected_pg_leaf_failures bigint,
    materialize_admissions bigint,
    materialize_failure_unwinds bigint,
    materialize_admission_restored bigint,
    materialize_admission_mismatches bigint,
    last_failure_point text,
    armed_failure_point text,
    last_failure_sqlstate text,
    last_failure_message text
)
AS 'MODULE_PATHNAME', 'bioseq_internal_async_mps_worker_boundary_stats'
LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.async_mps_worker_boundary_stats()
    FROM PUBLIC;

-- DOC0294 — scheduler supervision/admin surface.
CREATE OR REPLACE FUNCTION bioseq.scheduler_restart()
    RETURNS void
    AS 'MODULE_PATHNAME', 'bioseq_scheduler_restart'
    LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION bioseq.scheduler_stats()
    RETURNS TABLE (
        supervisor boolean,
        config_generation integer,
        pipeline_concurrency_mode text,
        min_threads integer,
        max_threads integer,
        current_threads integer,
        batches_scheduled bigint,
        batches_awaiting bigint,
        batches_processed bigint,
        queries_waiting bigint,
        queries_processed bigint,
        oldest_waiting_batch_age_secs double precision,
        queries_per_sec double precision,
        uptime_secs double precision,
        async_persist_jobs_waiting bigint,
        index_memory_reserved_bytes bigint
    )
    AS 'MODULE_PATHNAME', 'bioseq_scheduler_stats'
    LANGUAGE C VOLATILE;

-- Test-only, revoked seam: proves an oversized query-union and a later tiny
-- union can inhabit the same CpuPreparedTarget object without inherited
-- vector capacity or a prematurely released memory-admission reservation.
CREATE OR REPLACE FUNCTION
    bioseq_internal.cpu_prepared_target_same_object_reprepare_probe()
    RETURNS boolean
    AS 'MODULE_PATHNAME',
       'bioseq_internal_cpu_prepared_target_same_object_reprepare_probe'
    LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.cpu_prepared_target_same_object_reprepare_probe()
    FROM PUBLIC;

-- GPU cache warm-up: pre-builds all static shader .metallib/.metalar files in the
-- backend-local cache directory, installing the archive interposer env. Returns a
-- per-variant status transcript; "RESULT: ok variants=N failed=0" on success.
CREATE OR REPLACE FUNCTION bioseq.gpu_warmup_shader_cache()
    RETURNS text
    AS 'MODULE_PATHNAME', 'bioseq_gpu_warmup_shader_cache'
    LANGUAGE C VOLATILE;

CREATE OR REPLACE FUNCTION bioseq.vulkan_runtime_status(
    OUT available boolean,
    OUT device_name text,
    OUT max_workgroup_count_x integer,
    OUT scalar_block_layout boolean,
    OUT initialization_error text,
    OUT device_local_bytes bigint,
    OUT software_device boolean
)
    RETURNS record
    AS 'MODULE_PATHNAME', 'bioseq_vulkan_runtime_status'
    LANGUAGE C STABLE PARALLEL SAFE;

-- DOC0279 §12.2 — admin/test SQL surface for the new chunked index storage.
CREATE OR REPLACE FUNCTION bioseq.set_index_policy(rel regclass, name text, policy text) RETURNS void
    AS 'MODULE_PATHNAME', 'bioseq_set_index_policy' LANGUAGE C VOLATILE STRICT;
CREATE OR REPLACE FUNCTION bioseq.set_active_read_index(rel regclass, name text) RETURNS void
    AS 'MODULE_PATHNAME', 'bioseq_set_active_read_index' LANGUAGE C VOLATILE STRICT;
CREATE OR REPLACE FUNCTION bioseq.drop_all_index_tables() RETURNS void
    AS 'MODULE_PATHNAME', 'bioseq_drop_all_index_tables' LANGUAGE C VOLATILE;
CREATE OR REPLACE FUNCTION bioseq.index_table_row_count(rel regclass, name text) RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_index_table_row_count' LANGUAGE C VOLATILE STRICT;

-- Fail-closed, metadata-only evidence for a standard cold no-index cell. The
-- implementation never scans a target tuple or opens a sequence fragment; an
-- SPI/catalog failure is an ERROR rather than an absence result.
CREATE OR REPLACE FUNCTION bioseq.inspect_cold_no_index_absence(
    rel regclass,
    name text,
    OUT target_rows bigint,
    OUT target_cardinality_metadata_only boolean,
    OUT source_parent_rows_read bigint,
    OUT source_fragment_rows_read bigint,
    OUT source_decoded_bp bigint,
    OUT physical_index_relations bigint,
    OUT persisted_index_rows bigint,
    OUT generation_objects bigint)
RETURNS record
    AS 'MODULE_PATHNAME', 'bioseq_inspect_cold_no_index_absence'
    LANGUAGE C VOLATILE STRICT;
-- DOC0371 setup/post-measurement audit only.  This function validates one
-- complete READY revision-3 generation and its authoritative source row under
-- one MVCC snapshot.  It does not read payload extents, activate ordinal 2, or
-- mutate/cache an executable index.  SECURITY INVOKER plus the C-side table
-- ownership check keeps physical locator and ABI evidence administrative.
CREATE OR REPLACE FUNCTION bioseq.inspect_ready_minimizer_generation(
    rel regclass,
    index_name text,
    query_key bigint)
RETURNS TABLE (
    generation_id bigint,
    source_identity text,
    compatibility_checksum text,
    source_content_checksum text,
    database_oid oid,
    main_relfilenode oid,
    column_attno smallint,
    sequence_rowno bigint,
    locator_version smallint,
    bucket_idx smallint,
    shard_idx integer,
    num_fragments bigint,
    total_bp_size bigint,
    first_sub_row_no bigint,
    padding_letters integer,
    alphabet_code smallint,
    storage_unit_byte_width smallint,
    kmer_size smallint,
    window_size smallint,
    gpu_abi_version integer,
    segment_geometry_version smallint,
    position_segment_span_bp bigint,
    position_segment_count integer)
AS 'MODULE_PATHNAME', 'bioseq_inspect_ready_minimizer_generation'
LANGUAGE C STABLE STRICT PARALLEL RESTRICTED SECURITY INVOKER;
REVOKE EXECUTE ON FUNCTION
    bioseq.inspect_ready_minimizer_generation(regclass, text, bigint)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq.inspect_ready_minimizer_generation(regclass, text, bigint) IS
    'DOC0371 administrative cold-benchmark proof: validate one source-bound '
    'READY revision-3 minimizer generation without reading payload extents or '
    'enabling the live ordinal-2 reader.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_build_fixture(
    source_table regclass,
    index_name text,
    sequence_rowno bigint,
    kmer_size smallint,
    window_size smallint,
    selected_wire_format smallint,
    requested_backend text DEFAULT 'scalar')
RETURNS TABLE (
    sidecar_table text,
    wire_format smallint,
    generation_id bigint,
    source_identity text,
    source_content_checksum text,
    compatibility_checksum text,
    source_num_fragments bigint,
    source_total_bp bigint,
    source_padding_letters bigint,
    writer_rows bigint,
    writer_batches bigint,
    published_head_bytes bigint,
    control_count bigint,
    position_count bigint,
    directory_entries bigint,
    position_segment_count bigint,
    control_encoded_bytes bigint,
    position_encoded_bytes bigint,
    control_chunks bigint,
    position_chunks bigint)
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_build_fixture'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_build_fixture(
        regclass, text, bigint, smallint, smallint, smallint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_build_fixture(
        regclass, text, bigint, smallint, smallint, smallint, text) IS
    'Integration-only live revision-3/4 writer/token/publisher fixture for one explicit sketch backend; opens no read session and does not activate ordinal 2.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_read_fixture(
    source_table regclass,
    index_name text,
	sequence_rowno bigint,
	kmer_size smallint,
	window_size smallint,
	selected_wire_format smallint,
	requested_backend text DEFAULT 'scalar')
RETURNS TABLE (
    sidecar_table text,
    wire_format smallint,
    generation_id bigint,
    source_identity text,
    source_content_checksum text,
    compatibility_checksum text,
    source_num_fragments bigint,
    source_total_bp bigint,
    source_padding_letters bigint,
    validated_control_bytes bigint,
    validated_position_bytes bigint,
    validated_control_checksum text,
    validated_position_checksum text,
    lookup_count bigint,
    lookup_bp bigint,
    lookup_reverse boolean,
    lookup_positions_complete boolean,
    relation_opens bigint,
    index_scans bigint,
    head_scans bigint,
    manifest_directory_scans bigint,
    planned_request_groups bigint,
    executed_request_groups bigint,
    bulk_read_calls bigint,
    range_scan_calls bigint,
    payload_wanted_extents bigint,
    payload_read_extents bigint,
    payload_wanted_chunks bigint,
    payload_delivered_chunks bigint,
    payload_delivered_bytes bigint,
    payload_heap_rows_read bigint,
    payload_skipped_rows bigint,
    toast_values_read bigint,
    detoast_bytes bigint,
    cache_lookups bigint,
    selective_cold_index_preparation jsonb)
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_read_fixture'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
	bioseq_internal.rev3_test_read_fixture(
		regclass, text, bigint, smallint, smallint, smallint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
	bioseq_internal.rev3_test_read_fixture(
		regclass, text, bigint, smallint, smallint, smallint, text) IS
    'Integration-only independently opened revision-3/4 authenticated read-session fixture; does not activate ordinal 2.';
CREATE OR REPLACE FUNCTION
    bioseq_internal.rev3_test_read_full_generation_fixture(
    source_table regclass,
    index_name text,
    sequence_rowno bigint,
    kmer_size smallint,
    window_size smallint,
    selected_wire_format smallint,
    requested_backend text)
RETURNS TABLE (
    sidecar_table text,
    selected_backend text,
    generation_id bigint,
    control_bytes bigint,
    position_bytes bigint,
    control_checksum text,
    position_checksum text,
    relation_opens bigint,
    index_scans bigint,
    range_scan_calls bigint,
    payload_heap_rows bigint,
    delivered_chunks bigint,
    delivered_bytes bigint,
    planned_extents bigint,
    completed_extents bigint,
    scan_calls bigint)
AS 'MODULE_PATHNAME',
    'bioseq_internal_rev3_test_read_full_generation_fixture'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_read_full_generation_fixture(
        regclass, text, bigint, smallint, smallint, smallint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_read_full_generation_fixture(
        regclass, text, bigint, smallint, smallint, smallint, text) IS
    'Integration-only revision-3 full-generation single-scan payload proof for one exact requested sketch backend; payload remains opaque and ordinal 2 is not activated.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_remove_source_fragment(
    source_table regclass,
    index_name text,
    sequence_rowno bigint,
    fragment_ordinal integer)
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_remove_source_fragment'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_remove_source_fragment(
        regclass, text, bigint, integer)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_remove_source_fragment(
        regclass, text, bigint, integer) IS
    'Integration-only adversary that removes a non-first locator-referenced sequence fragment.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_copy_primary_index_shape(
    target_table regclass,
    donor_index regclass,
    shape text)
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_copy_primary_index_shape'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_copy_primary_index_shape(regclass, regclass, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_copy_primary_index_shape(
        regclass, regclass, text) IS
    'Integration-only transactional pg_index predicate/expression corruption seam.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_rewrite_source_checksum(
    target_table regclass,
    query_key bigint,
    xor_mask bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_rewrite_source_checksum'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_rewrite_source_checksum(regclass, bigint, bigint)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_rewrite_source_checksum(
        regclass, bigint, bigint) IS
    'Integration-only self-consistent head/manifest source-checksum corruption seam.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_session_failure(
    source_table regclass,
    index_name text,
    query_key bigint,
    failure_mode text)
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_session_failure'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_session_failure(regclass, text, bigint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_session_failure(
        regclass, text, bigint, text) IS
    'Integration-only read-session state, permit-denial, and cleanup seam.';
CREATE OR REPLACE FUNCTION bioseq_internal.arm_inline_sequence_pg_failure(
    failure_point text)
RETURNS void
AS 'MODULE_PATHNAME', 'bioseq_internal_arm_inline_sequence_pg_failure'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.arm_inline_sequence_pg_failure(text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.arm_inline_sequence_pg_failure(text) IS
    'Integration-only one-shot failure at an inline Sequence lookup/read/detoast/close PostgreSQL leaf.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_generation_storage_state(
    source_table regclass,
    index_name text,
    sequence_rowno bigint,
    operation text)
RETURNS TABLE (
    before_oid oid,
    operation_oid oid,
    after_oid oid,
    same_handle_rebound boolean,
    generation_schema_after boolean,
    chunk_cache_hit boolean,
    assembled_cache_hit boolean)
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_generation_storage_state'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_generation_storage_state(
        regclass, text, bigint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_generation_storage_state(
        regclass, text, bigint, text) IS
    'Integration-only same-handle subtransaction rollback, generation clear, and cross-backend index-residency invalidation seam.';
CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_build_ready_mpf_generation(
    source_table regclass,
    index_name text,
    sequence_rowno bigint,
    backend text)
RETURNS TABLE (
    sidecar_table text,
    column_attno smallint,
    source_sequence_rowno bigint,
    source_epoch text,
    source_num_fragments bigint,
    source_total_bp bigint,
    selected_backend text,
    wire_format smallint,
    published boolean,
    head_manifest_verified boolean,
    generation_id bigint,
    source_identity text,
    source_content_checksum text,
    compatibility_checksum text,
    manifest_target_length bigint,
    kmer_size smallint,
    window_size smallint,
    minimizer_scheme_id integer,
    minimizer_scheme_version integer,
    gpu_abi_version integer,
    position_segment_span_bp bigint,
    position_segment_count integer,
    writer_rows bigint,
    writer_batches bigint,
    control_count bigint,
    position_count bigint,
    producer_scan_count bigint,
    geometry_retry_count bigint,
    short_core_runs bigint,
    scalar_core_runs bigint,
    exact_simd_core_runs bigint,
    paper_nthash_core_runs bigint,
    vulkan_core_runs bigint,
    execution_accepted boolean,
    exact_proof_present boolean,
    exact_proof_accepted boolean,
    exact_vector_batches bigint,
    exact_scalar_backend_branches bigint,
    paper_proof_present boolean,
    paper_status text,
    paper_proof_accepted boolean,
    paper_packed_decode_batches bigint,
    paper_fallback_calls bigint,
    vulkan_completed_roundtrips bigint,
    vulkan_downloaded_minimizers bigint)
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_build_ready_mpf_generation'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_build_ready_mpf_generation(
        regclass, text, bigint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_build_ready_mpf_generation(
        regclass, text, bigint, text) IS
    'Integration-only real PostgreSQL-backed execution proof for the inactive format-4 MPF generation coordinator; does not activate a live route or update metadata age.';

CREATE OR REPLACE FUNCTION bioseq_internal.rev3_test_build_ready_mps_generation(
    source_table regclass,
    index_name text,
    sequence_rowno bigint,
    backend text)
RETURNS TABLE (
    sidecar_table text,
    column_attno smallint,
    source_sequence_rowno bigint,
    source_epoch text,
    source_num_fragments bigint,
    source_total_bp bigint,
    selected_backend text,
    wire_format smallint,
    published boolean,
    head_manifest_verified boolean,
    generation_id bigint,
    source_identity text,
    source_content_checksum text,
    compatibility_checksum text,
    manifest_target_length bigint,
    kmer_size smallint,
    window_size smallint,
    minimizer_scheme_id integer,
    minimizer_scheme_version integer,
    gpu_abi_version integer,
    position_segment_span_bp bigint,
    position_segment_count integer,
    writer_rows bigint,
    writer_batches bigint,
    control_count bigint,
    position_count bigint,
    spill_run_count bigint,
    spilled_event_count bigint,
    spill_operation_epoch bigint,
    spill_generation_token bigint,
    spill_mutation_version bigint,
    spill_qualification_scan_count bigint,
    spill_qualification_merge_count bigint,
    configured_core_span_bp bigint,
    peak_core_span_bp bigint,
    peak_core_events bigint,
    short_core_runs bigint,
    scalar_core_runs bigint,
    exact_simd_core_runs bigint,
    paper_nthash_core_runs bigint,
    vulkan_core_runs bigint,
    execution_accepted boolean,
    exact_proof_present boolean,
    exact_proof_accepted boolean,
    exact_vector_batches bigint,
    exact_scalar_backend_branches bigint,
    paper_proof_present boolean,
    paper_status text,
    paper_proof_accepted boolean,
    paper_packed_decode_batches bigint,
    paper_fallback_calls bigint,
    vulkan_completed_roundtrips bigint,
    vulkan_downloaded_minimizers bigint,
    replay_scan_count bigint,
    replay_merge_pass_count bigint,
    replay_reroute_pass_count bigint,
    replay_completed_scan_count bigint,
    replay_failed_scan_count bigint,
    replay_binding_checked_failed_scan_count bigint,
    replay_illegal_span_transition_count bigint,
    replay_first_completed_event_count bigint,
    replay_last_completed_event_count bigint,
    replay_final_span_bp bigint,
    replay_final_generation_token bigint,
    replay_final_mutation_version bigint,
    replay_visited_event_count bigint,
    evidence_manifest_position_count bigint,
    evidence_manifest_position_segment_span_bp bigint,
    release_generation_token bigint,
    release_mutation_before bigint,
    release_mutation_after bigint,
    spill_resources_released boolean)
AS 'MODULE_PATHNAME', 'bioseq_internal_rev3_test_build_ready_mps_generation'
LANGUAGE C VOLATILE STRICT PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.rev3_test_build_ready_mps_generation(
        regclass, text, bigint, text)
    FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.rev3_test_build_ready_mps_generation(
        regclass, text, bigint, text) IS
    'Revoked superuser-only real PostgreSQL execution proof for the inactive format-4 MPS generation coordinator; releases all PostgreSQL-owned spill runs before READY publication and does not update metadata age.';

-- Integration-only, backend-local proof for the lazy-load DROP EXTENSION
-- path.  The counters are observational and monotonic; they do not participate
-- in teardown classification and intentionally survive DROP/CREATE in a
-- backend that has already loaded the library.
CREATE OR REPLACE FUNCTION bioseq_internal.teardown_diagnostics(
    OUT lock_fallback_classifications bigint,
    OUT missing_relation_observations bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'bioseq_internal_teardown_diagnostics'
LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION bioseq_internal.teardown_diagnostics() FROM PUBLIC;
COMMENT ON FUNCTION bioseq_internal.teardown_diagnostics() IS
    'Integration-only backend-local counters proving lazy DROP EXTENSION lock classification and absence of stale relation revisits.';

-- A revoked integration seam that constructs planner nodes in C and invokes
-- the exact production BatchJoin ProjectionPath eligibility guard. Ordinary
-- SQL cannot reliably keep an unrelated expression at the base projection.
CREATE OR REPLACE FUNCTION bioseq_internal.batch_join_projection_guard_probe(
    OUT accepts_exact_helper boolean,
    OUT rejects_unrelated_phv boolean,
    OUT rejects_volatile_mixed_expression boolean,
    OUT preserves_wrapper_cost_owner boolean,
    OUT accepts_plain_target_var boolean,
    OUT rejects_plain_target_sequence_var boolean,
    OUT rejects_whole_row_var boolean,
    OUT rejects_system_var boolean,
    OUT rejects_nulling_var boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'bioseq_internal_batch_join_projection_guard_probe'
LANGUAGE C VOLATILE PARALLEL UNSAFE SECURITY INVOKER;
REVOKE ALL ON FUNCTION
    bioseq_internal.batch_join_projection_guard_probe() FROM PUBLIC;
COMMENT ON FUNCTION
    bioseq_internal.batch_join_projection_guard_probe() IS
    'Integration-only direct proof of the production BatchJoin base ProjectionPath guard and wrapper cost ownership.';
CREATE OR REPLACE FUNCTION bioseq.index_distinct_sub_keys(rel regclass, name text, query_key bigint) RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_index_distinct_sub_keys' LANGUAGE C VOLATILE STRICT;
CREATE OR REPLACE FUNCTION bioseq.column_row_number(rel regclass, ctid tid) RETURNS bigint
    AS 'MODULE_PATHNAME', 'bioseq_column_row_number' LANGUAGE C VOLATILE STRICT;

-- DOC0279 §12.2/§16.2 — TEST-ONLY controlled-corruption hooks. They resolve the
-- dynamic seqidx_* shard for (rel, name) internally and write / delete a single
-- chunk row, so the integration tests can pin reserved-sentinel filtering,
-- missing-manifest -> recompute, and present-manifest/missing-fragment
-- corruption detection.
CREATE OR REPLACE FUNCTION bioseq.test_write_index_chunk(rel regclass, name text, query_key bigint,
        sub_key bigint, chunk_no int, payload bytea) RETURNS void
    AS 'MODULE_PATHNAME', 'bioseq_test_write_index_chunk' LANGUAGE C VOLATILE STRICT;
CREATE OR REPLACE FUNCTION bioseq.test_delete_index_manifest(rel regclass, name text, query_key bigint) RETURNS void
    AS 'MODULE_PATHNAME', 'bioseq_test_delete_index_manifest' LANGUAGE C VOLATILE STRICT;

COMMENT ON FUNCTION bioseq.update_index(regclass, text) IS
    'Recompute (or create) the named metadata kind for the given table. '
    'Returns the number of entries written. Used to backfill metadata '
    'after bulk INSERTs were run with bioseq.update_index_on_insert = off.';

-- ============================================================================
-- DOC0264 Phase C — sample-sequence generation and locator inspection.
-- ============================================================================

-- bioseq.create_sample_table(table_name, n_sequences, sequence_bp_size,
--                            [alphabet='dna4'], [seed=42])
-- Creates a USING bioseq table containing `n_sequences` synthetic dna4
-- sequences of `sequence_bp_size` letters each. Streams fragments through
-- Phase C's insert_sequence_streamed so peak RSS during insert is bounded
-- by one fragment's worth of state (~8 KiB) regardless of sequence size.
-- Only the dna4 alphabet is shipped in Phase C; other alphabets are
-- rejected with a clear error pointing at DOC0267 D-EXTENSIBILITY.
-- Runs ANALYZE on the table at the end so planner stats are populated.
CREATE OR REPLACE FUNCTION bioseq.create_sample_table(
    table_name       TEXT,
    n_sequences      INTEGER,
    sequence_bp_size BIGINT,
    alphabet         TEXT    DEFAULT 'dna4',
    seed             BIGINT  DEFAULT 42)
    RETURNS OID
    AS 'MODULE_PATHNAME', 'bioseq_create_sample_table'
    LANGUAGE C VOLATILE;
COMMENT ON FUNCTION bioseq.create_sample_table(TEXT, INTEGER, BIGINT, TEXT, BIGINT) IS
    'DOC0264 Phase C: creates a USING bioseq table with n_sequences synthetic '
    'sequences of sequence_bp_size letters each. Only dna4 is supported in '
    'Phase C; other alphabets are rejected. Bounded RSS regardless of '
    'sequence_bp_size — fragments stream through insert_sequence_streamed.';

-- bioseq.locator_inspect(seq) — returns the v3 locator fields for stub
-- values, or zeros/inline-padding for non-stub values. Used by the
-- integration tests to verify per-fragment row counts, total_bp_size, etc.
CREATE OR REPLACE FUNCTION bioseq.locator_inspect(seq bioseq.seq)
    RETURNS TABLE (
        version          INT2,
        bucket_idx       INT2,
        shard_idx        INT4,
        num_fragments    BIGINT,
        total_bp_size    BIGINT,
        first_sub_row_no BIGINT,
        source_epoch     BIGINT,
        total_padding    INT4)
    AS 'MODULE_PATHNAME', 'bioseq_locator_inspect'
    LANGUAGE C IMMUTABLE STRICT;

-- DOC0274 — `bioseq.create_query_target_sample_table(...)`. Inlines the
-- benchmark generator's pair-emission loop (substitutions + indels +
-- optional reverse-complement) and writes one row per pair: (query, target,
-- truth_target_start, truth_target_end, is_reverse, effective_error_rate,
-- original_query_length). Both seq columns flow through
-- insert_sequence_streamed so peak RSS is bounded by one fragment + the
-- shared reference vector (no full-row materialisation regardless of
-- reference_length).
CREATE OR REPLACE FUNCTION bioseq.create_query_target_sample_table(
    table_name              TEXT,
    fragment_count          INTEGER,
    fragment_length_mean    BIGINT,
    fragment_length_stddev  DOUBLE PRECISION,
    reference_length        BIGINT,
    substitution_rate       DOUBLE PRECISION,
    insertion_rate          DOUBLE PRECISION,
    deletion_rate           DOUBLE PRECISION,
    insertion_length_mean   DOUBLE PRECISION,
    deletion_length_mean    DOUBLE PRECISION,
    reverse_complement_rate DOUBLE PRECISION,
    seed                    BIGINT)
    RETURNS OID
    AS 'MODULE_PATHNAME', 'bioseq_create_query_target_sample_table'
    LANGUAGE C VOLATILE;
COMMENT ON FUNCTION bioseq.create_query_target_sample_table(
    TEXT, INTEGER, BIGINT, DOUBLE PRECISION, BIGINT, DOUBLE PRECISION,
    DOUBLE PRECISION, DOUBLE PRECISION, DOUBLE PRECISION, DOUBLE PRECISION,
    DOUBLE PRECISION, BIGINT) IS
    'DOC0274: creates a USING bioseq table mirroring the bench generator''s '
    '(query, target) pairs with ground-truth metadata. Each row has the same '
    'reference as `target` and a mutated fragment as `query`; aligners are '
    'expected to find the query inside the target at exactly '
    '[truth_target_start, truth_target_end).';
COMMENT ON FUNCTION bioseq.locator_inspect(bioseq.seq) IS
    'DOC0371: returns the v3 locator fields embedded in a bioseq.seq '
    'stub value: (version, bucket_idx, shard_idx, num_fragments, '
    'total_bp_size, first_sub_row_no, total_padding). For inline (non-stub) '
    'values returns zeros for the locator-only fields and the inline '
    'bp_size + padding.';
