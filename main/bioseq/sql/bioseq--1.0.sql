-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION bioseq" to load this file. \quit

SET bioseq.logging = off;

CREATE SCHEMA bioseq_internal;

CREATE SCHEMA bioseq;

GRANT USAGE ON SCHEMA bioseq TO PUBLIC;

CREATE SEQUENCE bioseq_internal.row_mask_seq
    AS bigint
    START 1;

CREATE TABLE bioseq_internal.row_mask (
    id bigint NOT NULL,
    storage_id bigint NOT NULL,
    stripe_id bigint NOT NULL,
    chunk_id integer NOT NULL,
    start_row_number bigint NOT NULL,
    end_row_number bigint NOT NULL,
    deleted_rows integer NOT NULL,
    mask bytea,
    PRIMARY KEY (id, storage_id, start_row_number, end_row_number)
) WITH (user_catalog_table = true);

ALTER TABLE bioseq_internal.row_mask
    ADD CONSTRAINT row_mask_stripe_unique UNIQUE (storage_id, stripe_id, start_row_number);

ALTER TABLE bioseq_internal.row_mask
    ADD CONSTRAINT row_mask_chunk_unique UNIQUE (storage_id, stripe_id, chunk_id, start_row_number);

COMMENT ON TABLE bioseq_internal.row_mask IS 'Chunk mask metadata.';

CREATE SEQUENCE IF NOT EXISTS bioseq_internal.storageid_seq
    AS bigint
    MINVALUE 10000000000
    START 10000000000;

CREATE TABLE IF NOT EXISTS bioseq_internal.options (
    regclass regclass NOT NULL,
    chunk_group_row_limit integer NOT NULL,
    stripe_row_limit integer NOT NULL,
    compression_level integer NOT NULL,
    compression name NOT NULL,
    PRIMARY KEY (regclass)
) WITH (user_catalog_table = true);

COMMENT ON TABLE bioseq_internal.options IS 'Per-table Bioseq storage options.';

CREATE TABLE IF NOT EXISTS bioseq_internal.stripe (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    file_offset bigint NOT NULL,
    data_length bigint NOT NULL,
    column_count integer NOT NULL,
    chunk_row_count integer NOT NULL,
    row_count bigint NOT NULL,
    chunk_group_count integer NOT NULL,
    first_row_number bigint NOT NULL,
    PRIMARY KEY (storage_id, stripe_num),
    CONSTRAINT stripe_first_row_number_idx UNIQUE (storage_id, first_row_number)
) WITH (user_catalog_table = true);

COMMENT ON TABLE bioseq_internal.stripe IS 'Per-stripe storage metadata.';

CREATE TABLE IF NOT EXISTS bioseq_internal.chunk_group (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    chunk_group_num integer NOT NULL,
    row_count bigint NOT NULL,
    deleted_rows bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (storage_id, stripe_num, chunk_group_num)
);

COMMENT ON TABLE bioseq_internal.chunk_group IS 'Per-chunk-group storage metadata.';

CREATE TABLE IF NOT EXISTS bioseq_internal.chunk (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    attr_num integer NOT NULL,
    chunk_group_num integer NOT NULL,
    minimum_value bytea,
    maximum_value bytea,
    value_stream_offset bigint NOT NULL,
    value_stream_length bigint NOT NULL,
    exists_stream_offset bigint NOT NULL,
    exists_stream_length bigint NOT NULL,
    value_compression_type integer NOT NULL,
    value_compression_level integer NOT NULL,
    value_decompressed_length bigint NOT NULL,
    value_count bigint NOT NULL,
    PRIMARY KEY (storage_id, stripe_num, attr_num, chunk_group_num)
) WITH (user_catalog_table = true);

COMMENT ON TABLE bioseq_internal.chunk IS 'Per-chunk storage metadata.';

CREATE OR REPLACE FUNCTION bioseq.bioseq_handler(handler_context internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'bioseq_handler'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

COMMENT ON FUNCTION bioseq.bioseq_handler(internal) IS 'Returns the table access-method handler for Bioseq tables.';

CREATE ACCESS METHOD bioseq
    TYPE TABLE
    HANDLER bioseq.bioseq_handler;

CREATE OR REPLACE FUNCTION pg_catalog.alter_bioseq_table_set(table_name regclass, chunk_group_row_limit integer DEFAULT NULL, stripe_row_limit integer DEFAULT NULL, compression name DEFAULT NULL, compression_level integer DEFAULT NULL)
RETURNS void
AS 'MODULE_PATHNAME', 'alter_bioseq_table_set'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

COMMENT ON FUNCTION pg_catalog.alter_bioseq_table_set(regclass, integer, integer, name, integer) IS 'Sets selected storage options on a Bioseq table; NULL options are unchanged.';

CREATE OR REPLACE FUNCTION pg_catalog.alter_bioseq_table_reset(table_name regclass, chunk_group_row_limit boolean DEFAULT FALSE, stripe_row_limit boolean DEFAULT FALSE, compression boolean DEFAULT FALSE, compression_level boolean DEFAULT FALSE)
RETURNS void
AS 'MODULE_PATHNAME', 'alter_bioseq_table_reset'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

COMMENT ON FUNCTION pg_catalog.alter_bioseq_table_reset(regclass, boolean, boolean, boolean, boolean) IS 'Resets selected Bioseq table options to their system defaults.';

CREATE TABLE IF NOT EXISTS bioseq_internal.index_policy (
    user_relid oid NOT NULL,
    main_relfilenode oid NOT NULL,
    column_attno smallint NOT NULL,
    index_name text NOT NULL,
    enabled boolean NOT NULL DEFAULT TRUE,
    update_on_insert boolean NOT NULL DEFAULT FALSE,
    is_active_read boolean NOT NULL DEFAULT FALSE,
    PRIMARY KEY (user_relid, main_relfilenode, column_attno, index_name)
) USING heap;

CREATE UNIQUE INDEX IF NOT EXISTS index_policy_one_active_read ON bioseq_internal.index_policy (user_relid, main_relfilenode, column_attno) WHERE is_active_read;

CREATE TABLE IF NOT EXISTS bioseq_internal.metadata_age_log (
    user_relid oid NOT NULL,
    main_relfilenode oid NOT NULL,
    column_attno smallint NOT NULL,
    index_name text NOT NULL,
    last_updated timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_relid, main_relfilenode, column_attno, index_name)
) USING heap;

CREATE TABLE IF NOT EXISTS bioseq_internal.index_table_shard (
    database_oid oid NOT NULL,
    tablespace_oid oid NOT NULL,
    main_relfilenode oid NOT NULL,
    column_attno smallint NOT NULL,
    index_name text NOT NULL,
    shard_idx integer NOT NULL,
    index_relid oid NOT NULL,
    bytes_written bigint NOT NULL DEFAULT 0,
    is_full boolean NOT NULL DEFAULT FALSE,
    PRIMARY KEY (database_oid, tablespace_oid, main_relfilenode, column_attno, index_name, shard_idx)
) USING heap;

CREATE TABLE IF NOT EXISTS bioseq_internal.seq_sub_table (
    database_oid oid NOT NULL,
    tablespace_oid oid NOT NULL,
    main_relfilenode oid NOT NULL,
    column_attno smallint NOT NULL,
    bucket_idx smallint NOT NULL,
    shard_idx integer NOT NULL,
    sub_table_relid oid NOT NULL,
    sidecar_relid oid NOT NULL,
    bytes_written bigint NOT NULL DEFAULT 0,
    is_full boolean NOT NULL DEFAULT FALSE,
    total_payload_bytes bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (database_oid, tablespace_oid, main_relfilenode, column_attno, bucket_idx, shard_idx),
    CHECK (bytes_written >= 0),
    CHECK (total_payload_bytes >= 0)
);

COMMENT ON TABLE bioseq_internal.seq_sub_table IS 'Sequence storage shard locations and capacity metadata.';

CREATE INDEX IF NOT EXISTS seq_sub_table_active_shard_idx ON bioseq_internal.seq_sub_table (database_oid, tablespace_oid, main_relfilenode,
column_attno, bucket_idx) WHERE is_full = false;

CREATE INDEX IF NOT EXISTS seq_sub_table_by_main_idx ON bioseq_internal.seq_sub_table (database_oid, tablespace_oid, main_relfilenode);

REVOKE ALL ON ALL TABLES IN SCHEMA bioseq_internal FROM PUBLIC;

REVOKE ALL ON SCHEMA bioseq_internal FROM PUBLIC;

CREATE SEQUENCE bioseq_internal.minimizer_generation_id_seq
    AS bigint
    START 1;

REVOKE ALL ON SEQUENCE bioseq_internal.minimizer_generation_id_seq FROM PUBLIC;

COMMENT ON SEQUENCE bioseq_internal.minimizer_generation_id_seq IS 'Noncycling allocator for immutable minimizer generation identifiers.';

CREATE UNLOGGED TABLE bioseq_internal.minimizer_generation_storage (
    database_oid oid NOT NULL,
    tablespace_oid oid NOT NULL,
    main_relfilenode oid NOT NULL,
    user_relid oid NOT NULL,
    column_attno smallint NOT NULL,
    index_name name NOT NULL,
    query_key bigint NOT NULL,
    generation_id bigint NOT NULL,
    provider smallint NOT NULL,
    payload_relation_oid oid,
    payload_database_oid oid,
    payload_tablespace_oid oid,
    payload_relfilenode oid,
    payload_relation_name name,
    image_bytes bigint NOT NULL,
    header_checksum bigint NOT NULL,
    retire_full_xid xid8,
    state smallint NOT NULL,
    PRIMARY KEY (database_oid, tablespace_oid, main_relfilenode, user_relid, column_attno, index_name, query_key, generation_id),
    CHECK (database_oid <> 0),
    CHECK (tablespace_oid <> 0),
    CHECK (main_relfilenode <> 0),
    CHECK (user_relid <> 0),
    CHECK (column_attno > 0),
    CHECK (index_name = 'minimizers_per_fragment'),
    CHECK (query_key >= 0),
    CHECK (generation_id > 0),
    CHECK (provider IN (0, 1)),
    CHECK ((provider = 0
AND payload_relation_oid IS NOT NULL
AND payload_database_oid IS NOT NULL
AND payload_tablespace_oid IS NOT NULL
AND payload_relfilenode IS NOT NULL
AND payload_relation_name IS NOT NULL
AND payload_relation_oid <> 0
AND payload_database_oid = database_oid
AND payload_tablespace_oid <> 0
AND payload_relfilenode <> 0)
OR (provider = 1
AND payload_relation_oid IS NULL
AND payload_database_oid IS NULL
AND payload_tablespace_oid IS NULL
AND payload_relfilenode IS NULL
AND payload_relation_name IS NULL)),
    CHECK (image_bytes >= 4096),
    CHECK ((state = 1 AND retire_full_xid IS NULL)
OR (state = 2 AND retire_full_xid IS NOT NULL)),
    CHECK (state IN (1, 2))
) USING heap;

CREATE UNIQUE INDEX minimizer_generation_storage_one_live ON bioseq_internal.minimizer_generation_storage (database_oid, tablespace_oid, main_relfilenode,
user_relid, column_attno, index_name, query_key) WHERE state = smallint '1';

CREATE INDEX minimizer_generation_storage_retired ON bioseq_internal.minimizer_generation_storage (retire_full_xid, database_oid, tablespace_oid,
main_relfilenode, user_relid, column_attno, index_name, query_key,
generation_id) WHERE state = smallint '2';

CREATE UNLOGGED TABLE bioseq_internal.minimizer_generation_reaper_state (
    database_oid oid NOT NULL,
    tablespace_root_ordinal integer NOT NULL DEFAULT 0,
    shard_ordinal smallint NOT NULL DEFAULT 0,
    last_canonical_filename text NOT NULL DEFAULT text '',
    pass bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (database_oid),
    CHECK (database_oid <> 0),
    CHECK (tablespace_root_ordinal >= 0),
    CHECK (shard_ordinal BETWEEN 0 AND 255),
    CHECK (pass >= 0)
) USING heap;

REVOKE ALL ON TABLE bioseq_internal.minimizer_generation_storage FROM PUBLIC;

REVOKE ALL ON TABLE bioseq_internal.minimizer_generation_reaper_state FROM PUBLIC;

COMMENT ON TABLE bioseq_internal.minimizer_generation_storage IS 'Immutable minimizer generation descriptor; provider 0 uses a PostgreSQL relation, provider 1 a mapped file; state 1 is live and state 2 retired.';

COMMENT ON TABLE bioseq_internal.minimizer_generation_reaper_state IS 'Cursor for bounded mapped-generation orphan reconciliation.';

CREATE SEQUENCE bioseq_internal.sequence_source_epoch_seq
    AS bigint
    START 1;

REVOKE ALL ON SEQUENCE bioseq_internal.sequence_source_epoch_seq FROM PUBLIC;

COMMENT ON SEQUENCE bioseq_internal.sequence_source_epoch_seq IS 'Extension-owned noncycling allocator for immutable sequence-source epochs.';

CREATE TABLE bioseq_internal.sequence_source_version (
    source_epoch bigint NOT NULL,
    user_relid oid NOT NULL,
    database_oid oid NOT NULL,
    tablespace_oid oid NOT NULL,
    main_relfilenode oid NOT NULL,
    main_storage_id bigint NOT NULL,
    column_attno smallint NOT NULL,
    locator_version smallint NOT NULL,
    bucket_idx smallint NOT NULL,
    shard_idx integer NOT NULL,
    fragment_relid oid,
    fragment_storage_id bigint NOT NULL,
    first_sub_row_no bigint NOT NULL,
    num_fragments bigint NOT NULL,
    total_bp_size bigint NOT NULL,
    alphabet_code smallint NOT NULL,
    storage_unit_byte_width smallint NOT NULL,
    padding_letters integer NOT NULL,
    source_content_checksum bigint NOT NULL,
    state smallint NOT NULL,
    CONSTRAINT sequence_source_version_pkey PRIMARY KEY (source_epoch),
    CONSTRAINT sequence_source_version_source_epoch_check CHECK (source_epoch > 0),
    CONSTRAINT sequence_source_version_main_storage_id_check CHECK (main_storage_id > 0),
    CONSTRAINT sequence_source_version_column_attno_check CHECK (column_attno > 0),
    CONSTRAINT sequence_source_version_locator_version_check CHECK (locator_version = 3),
    CONSTRAINT sequence_source_version_bucket_idx_check CHECK (bucket_idx BETWEEN 0 AND 255),
    CONSTRAINT sequence_source_version_shard_idx_check CHECK (shard_idx >= 0),
    CONSTRAINT sequence_source_version_fragment_shape_check CHECK ((num_fragments = 0
AND fragment_relid IS NULL
AND fragment_storage_id = 0
AND first_sub_row_no = 0)
OR (num_fragments > 0
AND fragment_relid IS NOT NULL
AND fragment_storage_id > 0)),
    CONSTRAINT sequence_source_version_fragment_storage_id_check CHECK (fragment_storage_id >= 0),
    CONSTRAINT sequence_source_version_first_sub_row_no_check CHECK (first_sub_row_no >= 0),
    CONSTRAINT sequence_source_version_num_fragments_check CHECK (num_fragments BETWEEN 0 AND 4294967295),
    CONSTRAINT sequence_source_version_total_bp_size_check CHECK (total_bp_size >= 0),
    CONSTRAINT sequence_source_version_alphabet_code_check CHECK (alphabet_code BETWEEN 0 AND 255),
    CONSTRAINT sequence_source_version_storage_unit_byte_width_check CHECK (storage_unit_byte_width BETWEEN 1 AND 15),
    CONSTRAINT sequence_source_version_padding_letters_check CHECK (padding_letters BETWEEN 0 AND 65535),
    CONSTRAINT sequence_source_version_state_check CHECK (state IN (0, 1, 2))
) USING heap;

CREATE UNIQUE INDEX sequence_source_version_locator_key ON bioseq_internal.sequence_source_version (fragment_relid, fragment_storage_id,
first_sub_row_no);

CREATE INDEX sequence_source_version_fragment_row_idx ON bioseq_internal.sequence_source_version (fragment_relid, first_sub_row_no);

REVOKE ALL ON TABLE bioseq_internal.sequence_source_version FROM PUBLIC;

COMMENT ON TABLE bioseq_internal.sequence_source_version IS 'PostgreSQL-owned MVCC source epoch and insertion-time content checksum for one immutable sequence locator range; state 0=UNPUBLISHED, 1=READY, 2=terminal INVALID.';

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.seq_sub_table', 'WHERE FALSE');

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.sequence_source_version', 'WHERE FALSE');

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.minimizer_generation_storage', 'WHERE FALSE');

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.minimizer_generation_reaper_state', 'WHERE FALSE');

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.index_policy', 'WHERE FALSE');

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.metadata_age_log', 'WHERE FALSE');

SELECT pg_catalog.pg_extension_config_dump('bioseq_internal.index_table_shard', 'WHERE FALSE');

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
ORDER BY
    s.database_oid,
    s.tablespace_oid,
    s.main_relfilenode,
    s.column_attno,
    s.bucket_idx,
    s.shard_idx;

GRANT SELECT ON TABLE bioseq.seq_sub_table_summary TO public;

CREATE OR REPLACE FUNCTION bioseq.list_seq_storage_shards(rel regclass)
RETURNS table (sub_relation regclass, sidecar_relation regclass, column_attno smallint, bucket_idx smallint, shard_idx integer, bytes_written bigint, is_full boolean)
LANGUAGE SQL
AS $sql$
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
    s.sidecar_relid::regclass AS sidecar_relation,
    s.column_attno,
    s.bucket_idx,
    s.shard_idx,
    s.bytes_written,
    s.is_full
FROM bioseq_internal.seq_sub_table s, parent p
WHERE s.database_oid = p.db_oid
  AND s.tablespace_oid = p.spc_oid
  AND s.main_relfilenode = p.rfn
ORDER BY s.column_attno, s.bucket_idx, s.shard_idx;
$sql$
STABLE
PARALLEL UNSAFE
SECURITY INVOKER;

GRANT EXECUTE ON FUNCTION bioseq.list_seq_storage_shards(regclass) TO public;

CREATE OR REPLACE PROCEDURE bioseq.vacuum_seq_storage(rel regclass)
AS 'MODULE_PATHNAME', 'vacuum_seq_storage'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

GRANT EXECUTE ON PROCEDURE bioseq.vacuum_seq_storage(regclass) TO PUBLIC;

CREATE OR REPLACE FUNCTION bioseq.reap_orphan_seq_sub_tables()
RETURNS TABLE (action text, target text)
AS 'MODULE_PATHNAME', 'reap_orphan_seq_sub_tables'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

GRANT EXECUTE ON FUNCTION bioseq.reap_orphan_seq_sub_tables() TO PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.upgrade_bioseq_storage(rel regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'upgrade_bioseq_storage'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

COMMENT ON FUNCTION bioseq_internal.upgrade_bioseq_storage(regclass) IS 'Upgrades Bioseq storage metadata when the relation uses an older layout.';

CREATE OR REPLACE FUNCTION bioseq_internal.downgrade_bioseq_storage(rel regclass)
RETURNS void
AS 'MODULE_PATHNAME', 'downgrade_bioseq_storage'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

COMMENT ON FUNCTION bioseq_internal.downgrade_bioseq_storage(regclass) IS 'Downgrades Bioseq storage metadata when the relation uses a newer layout.';

INSERT INTO pg_catalog.pg_depend (
    classid, objid, objsubid,
    refclassid, refobjid, refobjsubid, deptype)
WITH desired(relid) AS (
    SELECT c.oid
      FROM pg_catalog.pg_class AS c
      JOIN pg_catalog.pg_namespace AS n
        ON n.oid = c.relnamespace
     WHERE n.nspname = 'bioseq_internal'
       AND c.relname IN (
           'chunk',
           'chunk_group',
           'chunk_group_pkey',
           'chunk_pkey',
           'minimizer_generation_id_seq',
           'options',
           'options_pkey',
           'sequence_source_epoch_seq',
           'sequence_source_version',
           'sequence_source_version_fragment_row_idx',
           'sequence_source_version_locator_key',
           'sequence_source_version_pkey',
           'storageid_seq',
           'stripe',
           'stripe_first_row_number_idx',
           'stripe_pkey')
),
edges AS (
    SELECT 'pg_catalog.pg_am'::regclass::oid,
           am.oid,
           0,
           'pg_catalog.pg_class'::regclass::oid,
           desired.relid,
           0,
           'n'::"char"
      FROM desired
      CROSS JOIN pg_catalog.pg_am AS am
     WHERE am.amname = 'bioseq'
)
SELECT * FROM edges
EXCEPT
SELECT classid, objid, objsubid,
       refclassid, refobjid, refobjsubid, deptype
  FROM pg_catalog.pg_depend;

CREATE FUNCTION bioseq.get_storage_id(relation_id regclass)
RETURNS bigint
AS 'MODULE_PATHNAME', 'get_storage_id'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE VIEW bioseq.storage WITH (security_barrier = true) AS
SELECT
    c.oid::regclass AS relation,
    bioseq.get_storage_id(c.oid) AS storage_id
FROM pg_class c, pg_am am
WHERE c.relam = am.oid
  AND am.amname = 'bioseq'
  AND pg_has_role(c.relowner, 'USAGE');

COMMENT ON VIEW bioseq.storage IS 'Bioseq relation ID to storage ID mapping.';

GRANT SELECT ON TABLE bioseq.storage TO public;

CREATE VIEW bioseq.options WITH (security_barrier = true) AS
SELECT
    regclass AS relation,
    chunk_group_row_limit,
    stripe_row_limit,
    compression,
    compression_level
FROM bioseq_internal.options o, pg_class c
WHERE o.regclass = c.oid
  AND pg_has_role(c.relowner, 'USAGE');

COMMENT ON VIEW bioseq.options IS 'Bioseq options for tables owned by the current user.';

GRANT SELECT ON TABLE bioseq.options TO public;

CREATE VIEW bioseq.stripe WITH (security_barrier = true) AS
SELECT
    relation,
    storage.storage_id,
    stripe_num,
    file_offset,
    data_length,
    column_count,
    chunk_row_count,
    row_count,
    chunk_group_count,
    first_row_number
FROM bioseq_internal.stripe stripe, bioseq.storage storage
WHERE stripe.storage_id = storage.storage_id;

COMMENT ON VIEW bioseq.stripe IS 'Bioseq stripe information for tables owned by the current user.';

GRANT SELECT ON TABLE bioseq.stripe TO public;

CREATE VIEW bioseq.chunk_group WITH (security_barrier = true) AS
SELECT
    relation,
    storage.storage_id,
    stripe_num,
    chunk_group_num,
    row_count
FROM bioseq_internal.chunk_group cg, bioseq.storage storage
WHERE cg.storage_id = storage.storage_id;

COMMENT ON VIEW bioseq.chunk_group IS 'Bioseq chunk-group information for tables owned by the current user.';

GRANT SELECT ON TABLE bioseq.chunk_group TO public;

CREATE VIEW bioseq.chunk WITH (security_barrier = true) AS
SELECT
    relation,
    storage.storage_id,
    stripe_num,
    attr_num,
    chunk_group_num,
    minimum_value,
    maximum_value,
    value_stream_offset,
    value_stream_length,
    exists_stream_offset,
    exists_stream_length,
    value_compression_type,
    value_compression_level,
    value_decompressed_length,
    value_count
FROM bioseq_internal.chunk chunk, bioseq.storage storage
WHERE chunk.storage_id = storage.storage_id;

COMMENT ON VIEW bioseq.chunk IS 'Bioseq chunk information for tables owned by the current user.';

GRANT SELECT ON TABLE bioseq.chunk TO public;

CREATE FUNCTION vemptycount(running_count bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'vemptycount'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE FUNCTION vanycount(running_count bigint, input "any")
RETURNS bigint
AS 'MODULE_PATHNAME', 'vanycount'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vcount(*) (
    SFUNC = vemptycount,
    STYPE = bigint,
    INITCOND = '0'
);

CREATE AGGREGATE vcount("any") (
    SFUNC = vanycount,
    STYPE = bigint,
    INITCOND = '0'
);

CREATE FUNCTION vint2int4avg(state internal)
RETURNS numeric
AS 'MODULE_PATHNAME', 'vint2int4avg'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE FUNCTION vint2sum(running_sum bigint, input smallint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'vint2sum'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vsum(smallint) (
    SFUNC = vint2sum,
    STYPE = bigint,
    INITCOND = '0'
);

CREATE FUNCTION vint2acc(state internal, input smallint)
RETURNS internal
AS 'MODULE_PATHNAME', 'vint2acc'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vavg(smallint) (
    SFUNC = vint2acc,
    STYPE = internal,
    FINALFUNC = vint2int4avg,
    INITCOND = '{0,0}'
);

CREATE FUNCTION vint2larger(running_max smallint, input smallint)
RETURNS smallint
AS 'MODULE_PATHNAME', 'vint2larger'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmax(smallint) (
    SFUNC = vint2larger,
    STYPE = smallint,
    INITCOND = '-32768'
);

CREATE FUNCTION vint2smaller(running_min smallint, input smallint)
RETURNS smallint
AS 'MODULE_PATHNAME', 'vint2smaller'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmin(smallint) (
    SFUNC = vint2smaller,
    STYPE = smallint,
    INITCOND = '32767'
);

CREATE FUNCTION vint4sum(running_sum bigint, input integer)
RETURNS bigint
AS 'MODULE_PATHNAME', 'vint4sum'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vsum(integer) (
    SFUNC = vint4sum,
    STYPE = bigint,
    INITCOND = '0'
);

CREATE FUNCTION vint4acc(state internal, input integer)
RETURNS internal
AS 'MODULE_PATHNAME', 'vint4acc'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vavg(integer) (
    SFUNC = vint4acc,
    STYPE = internal,
    FINALFUNC = vint2int4avg,
    INITCOND = '{0,0}'
);

CREATE FUNCTION vint4larger(running_max integer, input integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'vint4larger'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmax(integer) (
    SFUNC = vint4larger,
    STYPE = integer,
    INITCOND = '-2147483648'
);

CREATE FUNCTION vint4smaller(running_min integer, input integer)
RETURNS integer
AS 'MODULE_PATHNAME', 'vint4smaller'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmin(integer) (
    SFUNC = vint4smaller,
    STYPE = integer,
    INITCOND = '2147483647'
);

CREATE FUNCTION vint8acc(state internal, input bigint)
RETURNS internal
AS 'MODULE_PATHNAME', 'vint8acc'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE FUNCTION vint8sum(state internal)
RETURNS numeric
AS 'MODULE_PATHNAME', 'vint8sum'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vsum(bigint) (
    SFUNC = vint8acc,
    STYPE = internal,
    FINALFUNC = vint8sum,
    SERIALFUNC = pg_catalog.int8_avg_serialize,
    DESERIALFUNC = pg_catalog.int8_avg_deserialize
);

CREATE FUNCTION vint8avg(state internal)
RETURNS numeric
AS 'MODULE_PATHNAME', 'vint8avg'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vavg(bigint) (
    SFUNC = vint8acc,
    STYPE = internal,
    FINALFUNC = vint8avg,
    SERIALFUNC = pg_catalog.int8_avg_serialize,
    DESERIALFUNC = pg_catalog.int8_avg_deserialize
);

CREATE FUNCTION vint8larger(running_max bigint, input bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'vint8larger'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmax(bigint) (
    SFUNC = vint8larger,
    STYPE = bigint,
    INITCOND = '-9223372036854775808'
);

CREATE FUNCTION vint8smaller(running_min bigint, input bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'vint8smaller'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmin(bigint) (
    SFUNC = vint8smaller,
    STYPE = bigint,
    INITCOND = '9223372036854775807'
);

CREATE FUNCTION vdatelarger(running_max date, input date)
RETURNS date
AS 'MODULE_PATHNAME', 'vdatelarger'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmax(date) (
    SFUNC = vdatelarger,
    STYPE = date,
    INITCOND = '-infinity'
);

CREATE FUNCTION vdatesmaller(running_min date, input date)
RETURNS date
AS 'MODULE_PATHNAME', 'vdatesmaller'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE AGGREGATE vmin(date) (
    SFUNC = vdatesmaller,
    STYPE = date,
    INITCOND = 'infinity'
);

CREATE FUNCTION vbpchareq(left char, right char)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vbpchareq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vbpcharne(left char, right char)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vbpcharne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vbpchargt(left char, right char)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vbpchargt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vbpcharlt(left char, right char)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vbpcharlt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vbpcharle(left char, right char)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vbpcharle'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vbpcharge(left char, right char)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vbpcharge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint2eq(left smallint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint2eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint2ne(left smallint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint2ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint2gt(left smallint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint2gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint2lt(left smallint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint2lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint2le(left smallint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint2le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint2ge(left smallint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint2ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint24eq(left smallint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint24eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint24ne(left smallint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint24ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint24gt(left smallint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint24gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint24lt(left smallint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint24lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint24le(left smallint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint24le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint24ge(left smallint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint24ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint28eq(left smallint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint28eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint28ne(left smallint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint28ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint28gt(left smallint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint28gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint28lt(left smallint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint28lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint28le(left smallint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint28le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint28ge(left smallint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint28ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint4eq(left integer, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint4eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint4ne(left integer, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint4ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint4gt(left integer, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint4gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint4lt(left integer, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint4lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint4le(left integer, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint4le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint4ge(left integer, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint4ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint42eq(left integer, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint42eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint42ne(left integer, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint42ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint42gt(left integer, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint42gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint42lt(left integer, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint42lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint42le(left integer, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint42le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint42ge(left integer, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint42ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint48eq(left integer, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint48eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint48ne(left integer, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint48ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint48gt(left integer, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint48gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint48lt(left integer, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint48lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint48le(left integer, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint48le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint48ge(left integer, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint48ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint8eq(left bigint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint8eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint8ne(left bigint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint8ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint8gt(left bigint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint8gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint8lt(left bigint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint8lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint8le(left bigint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint8le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint8ge(left bigint, right bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint8ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint82eq(left bigint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint82eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint82ne(left bigint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint82ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint82gt(left bigint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint82gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint82lt(left bigint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint82lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint82le(left bigint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint82le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint82ge(left bigint, right smallint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint82ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint84eq(left bigint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint84eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint84ne(left bigint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint84ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint84gt(left bigint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint84gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint84lt(left bigint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint84lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint84le(left bigint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint84le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vint84ge(left bigint, right integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vint84ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vdate_eq(left date, right date)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vdate_eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vdate_ne(left date, right date)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vdate_ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vdate_gt(left date, right date)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vdate_gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vdate_lt(left date, right date)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vdate_lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vdate_le(left date, right date)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vdate_le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vdate_ge(left date, right date)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vdate_ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtime_eq(left time, right time)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtime_eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtime_ne(left time, right time)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtime_ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtime_gt(left time, right time)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtime_gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtime_lt(left time, right time)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtime_lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtime_le(left time, right time)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtime_le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtime_ge(left time, right time)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtime_ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamp_eq(left timestamp, right timestamp)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamp_eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamp_ne(left timestamp, right timestamp)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamp_ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamp_gt(left timestamp, right timestamp)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamp_gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamp_lt(left timestamp, right timestamp)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamp_lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamp_le(left timestamp, right timestamp)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamp_le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamp_ge(left timestamp, right timestamp)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamp_ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamptz_eq(left timestamptz, right timestamptz)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamptz_eq'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamptz_ne(left timestamptz, right timestamptz)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamptz_ne'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamptz_gt(left timestamptz, right timestamptz)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamptz_gt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamptz_lt(left timestamptz, right timestamptz)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamptz_lt'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamptz_le(left timestamptz, right timestamptz)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamptz_le'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION vtimestamptz_ge(left timestamptz, right timestamptz)
RETURNS boolean
AS 'MODULE_PATHNAME', 'vtimestamptz_ge'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION mmsearch_support(request_datum internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'mmsearch_support'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq_placeholder_support(request_datum internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'bioseq_placeholder_support'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.score(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'score'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.dp_score(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'dp_score'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_len(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'query_len'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.query_start(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'query_start'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

COMMENT ON FUNCTION bioseq.query_start(anyelement) IS 'Start offset of the current mmsearch alignment on the query sequence.';

CREATE OR REPLACE FUNCTION bioseq.query_end(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'query_end'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

COMMENT ON FUNCTION bioseq.query_end(anyelement) IS 'End offset of the current mmsearch alignment on the query sequence.';

CREATE OR REPLACE FUNCTION bioseq.target_len(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'target_len'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.target_start(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'target_start'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

COMMENT ON FUNCTION bioseq.target_start(anyelement) IS 'Start offset of the current mmsearch alignment on the stored target sequence.';

CREATE OR REPLACE FUNCTION bioseq.target_end(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'target_end'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

COMMENT ON FUNCTION bioseq.target_end(anyelement) IS 'End offset of the current mmsearch alignment on the stored target sequence.';

CREATE OR REPLACE FUNCTION bioseq.num_matches(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'num_matches'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.block_length(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'block_length'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.edit(el anyelement)
RETURNS bigint
AS 'MODULE_PATHNAME', 'edit'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
COST 100000.0
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.mapq(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'mapq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.chain_score(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'chain_score'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.dp_max_score(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'dp_max_score'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.seed_count(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'seed_count'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.secondary_chain_score(el anyelement)
RETURNS integer
AS 'MODULE_PATHNAME', 'secondary_chain_score'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.is_reverse(el anyelement)
RETURNS boolean
AS 'MODULE_PATHNAME', 'is_reverse'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.is_primary(el anyelement)
RETURNS boolean
AS 'MODULE_PATHNAME', 'is_primary'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.has_cigar(el anyelement)
RETURNS boolean
AS 'MODULE_PATHNAME', 'has_cigar'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.divergence(el anyelement)
RETURNS real
AS 'MODULE_PATHNAME', 'divergence'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

CREATE OR REPLACE FUNCTION bioseq.cigar(el anyelement)
RETURNS text
AS 'MODULE_PATHNAME', 'cigar'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_placeholder_support;

COMMENT ON FUNCTION bioseq.cigar(anyelement) IS 'CIGAR string describing the current mmsearch alignment.';

CREATE TYPE bioseq.seq;

CREATE OR REPLACE FUNCTION bioseq.seq_in(input cstring, type_oid oid, typmod integer)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_in'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_out(input bioseq.seq)
RETURNS cstring
AS 'MODULE_PATHNAME', 'seq_out'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_recv(buffer internal, type_oid oid, typmod integer)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_recv'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_send(input bioseq.seq)
RETURNS bytea
AS 'MODULE_PATHNAME', 'seq_send'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_typmod_in(array cstring[])
RETURNS integer
AS 'MODULE_PATHNAME', 'seq_typmod_in'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_typmod_out(typmod integer)
RETURNS cstring
AS 'MODULE_PATHNAME', 'seq_typmod_out'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE TYPE bioseq.letter;

CREATE FUNCTION bioseq.letter_in(input cstring, type_oid oid, typmod integer)
RETURNS bioseq.letter
AS 'MODULE_PATHNAME', 'letter_in'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_out(value bioseq.letter)
RETURNS cstring
AS 'MODULE_PATHNAME', 'letter_out'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_recv(buffer internal, type_oid oid, typmod integer)
RETURNS bioseq.letter
AS 'MODULE_PATHNAME', 'letter_recv'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_send(value bioseq.letter)
RETURNS bytea
AS 'MODULE_PATHNAME', 'letter_send'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_typmod_in(array cstring[])
RETURNS integer
AS 'MODULE_PATHNAME', 'letter_typmod_in'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_typmod_out(typmod integer)
RETURNS cstring
AS 'MODULE_PATHNAME', 'letter_typmod_out'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE TYPE bioseq.letter (
    INPUT = bioseq.letter_in,
    OUTPUT = bioseq.letter_out,
    RECEIVE = bioseq.letter_recv,
    SEND = bioseq.letter_send,
    TYPMOD_IN = bioseq.letter_typmod_in,
    TYPMOD_OUT = bioseq.letter_typmod_out,
    INTERNALLENGTH = 2,
    PASSEDBYVALUE,
    ALIGNMENT = int2
);

CREATE FUNCTION bioseq.letter(input bioseq.letter, target_typmod integer, explicit_cast boolean)
RETURNS bioseq.letter
AS 'MODULE_PATHNAME', 'letter'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE CAST (bioseq.letter AS bioseq.letter)
    WITH FUNCTION bioseq.letter(bioseq.letter, integer, boolean)
    AS ASSIGNMENT;

CREATE FUNCTION bioseq.letter_to_text(value bioseq.letter)
RETURNS text
AS 'MODULE_PATHNAME', 'letter_to_text'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE CAST (bioseq.letter AS text)
    WITH FUNCTION bioseq.letter_to_text(bioseq.letter);

CREATE FUNCTION bioseq.letter_eq(left bioseq.letter, right bioseq.letter)
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_eq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_ne(left bioseq.letter, right bioseq.letter)
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_ne'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_lt(left bioseq.letter, right bioseq.letter)
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_lt'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_le(left bioseq.letter, right bioseq.letter)
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_le'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_gt(left bioseq.letter, right bioseq.letter)
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_gt'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_ge(left bioseq.letter, right bioseq.letter)
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_ge'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_cmp(left bioseq.letter, right bioseq.letter)
RETURNS integer
AS 'MODULE_PATHNAME', 'letter_cmp'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_hash(value bioseq.letter)
RETURNS integer
AS 'MODULE_PATHNAME', 'letter_hash'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_hash_extended(value bioseq.letter, seed bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'letter_hash_extended'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OPERATOR bioseq.< (
    FUNCTION = bioseq.letter_lt,
    LEFTARG = bioseq.letter,
    RIGHTARG = bioseq.letter,
    COMMUTATOR = OPERATOR(bioseq.>),
    NEGATOR = OPERATOR(bioseq.>=),
    RESTRICT = pg_catalog.scalarltsel,
    JOIN = pg_catalog.scalarltjoinsel
);

CREATE OPERATOR bioseq.<= (
    FUNCTION = bioseq.letter_le,
    LEFTARG = bioseq.letter,
    RIGHTARG = bioseq.letter,
    COMMUTATOR = OPERATOR(bioseq.>=),
    NEGATOR = OPERATOR(bioseq.>),
    RESTRICT = pg_catalog.scalarlesel,
    JOIN = pg_catalog.scalarlejoinsel
);

CREATE OPERATOR bioseq.= (
    FUNCTION = bioseq.letter_eq,
    LEFTARG = bioseq.letter,
    RIGHTARG = bioseq.letter,
    COMMUTATOR = OPERATOR(bioseq.=),
    NEGATOR = OPERATOR(bioseq.<>),
    RESTRICT = pg_catalog.eqsel,
    JOIN = pg_catalog.eqjoinsel,
    HASHES,
    MERGES
);

CREATE OPERATOR bioseq.<> (
    FUNCTION = bioseq.letter_ne,
    LEFTARG = bioseq.letter,
    RIGHTARG = bioseq.letter,
    COMMUTATOR = OPERATOR(bioseq.<>),
    NEGATOR = OPERATOR(bioseq.=),
    RESTRICT = pg_catalog.neqsel,
    JOIN = pg_catalog.neqjoinsel
);

CREATE OPERATOR bioseq.>= (
    FUNCTION = bioseq.letter_ge,
    LEFTARG = bioseq.letter,
    RIGHTARG = bioseq.letter,
    COMMUTATOR = OPERATOR(bioseq.<=),
    NEGATOR = OPERATOR(bioseq.<),
    RESTRICT = pg_catalog.scalargesel,
    JOIN = pg_catalog.scalargejoinsel
);

CREATE OPERATOR bioseq.> (
    FUNCTION = bioseq.letter_gt,
    LEFTARG = bioseq.letter,
    RIGHTARG = bioseq.letter,
    COMMUTATOR = OPERATOR(bioseq.<),
    NEGATOR = OPERATOR(bioseq.<=),
    RESTRICT = pg_catalog.scalargtsel,
    JOIN = pg_catalog.scalargtjoinsel
);

CREATE OPERATOR CLASS bioseq.letter_btree_ops DEFAULT
    FOR TYPE bioseq.letter USING btree AS
    OPERATOR 1 bioseq.< (bioseq.letter, bioseq.letter),
    OPERATOR 2 bioseq.<= (bioseq.letter, bioseq.letter),
    OPERATOR 3 bioseq.= (bioseq.letter, bioseq.letter),
    OPERATOR 4 bioseq.>= (bioseq.letter, bioseq.letter),
    OPERATOR 5 bioseq.> (bioseq.letter, bioseq.letter),
    FUNCTION 1 bioseq.letter_cmp(bioseq.letter, bioseq.letter)
;

CREATE OPERATOR CLASS bioseq.letter_hash_ops DEFAULT
    FOR TYPE bioseq.letter USING hash AS
    OPERATOR 1 bioseq.= (bioseq.letter, bioseq.letter),
    FUNCTION 1 bioseq.letter_hash(bioseq.letter),
    FUNCTION 2 bioseq.letter_hash_extended(bioseq.letter, bigint)
;

CREATE FUNCTION bioseq.seq_subscript_handler(input internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'seq_subscript_handler'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER;

CREATE TYPE bioseq.seq (
    INPUT = bioseq.seq_in,
    OUTPUT = bioseq.seq_out,
    RECEIVE = bioseq.seq_recv,
    SEND = bioseq.seq_send,
    TYPMOD_IN = bioseq.seq_typmod_in,
    TYPMOD_OUT = bioseq.seq_typmod_out,
    SUBSCRIPT = bioseq.seq_subscript_handler,
    INTERNALLENGTH = VARIABLE,
    ALIGNMENT = double,
    STORAGE = extended
);

COMMENT ON TYPE bioseq.seq IS 'Compact, alphabet-aware biological sequence stored as a variable-length, TOASTable PostgreSQL value for indexing, similarity search, and analysis.';

CREATE OR REPLACE FUNCTION bioseq.seq_cast(input bioseq.seq, target_typmod integer, explicit_cast boolean)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_cast'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE CAST (bioseq.seq AS bioseq.seq)
    WITH FUNCTION bioseq.seq_cast(bioseq.seq, integer, boolean)
    AS ASSIGNMENT;

CREATE OR REPLACE FUNCTION bioseq.seq_cmp(left bioseq.seq, right bioseq.seq)
RETURNS integer
AS 'MODULE_PATHNAME', 'seq_cmp'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_lt(left bioseq.seq, right bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'seq_lt'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_le(left bioseq.seq, right bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'seq_le'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_eq(left bioseq.seq, right bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'seq_eq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_ne(left bioseq.seq, right bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'seq_ne'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_ge(left bioseq.seq, right bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'seq_ge'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.seq_gt(left bioseq.seq, right bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'seq_gt'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OPERATOR bioseq.< (
    FUNCTION = bioseq.seq_lt,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq,
    COMMUTATOR = OPERATOR(bioseq.>),
    NEGATOR = OPERATOR(bioseq.>=),
    RESTRICT = pg_catalog.scalarltsel,
    JOIN = pg_catalog.scalarltjoinsel
);

CREATE OPERATOR bioseq.<= (
    FUNCTION = bioseq.seq_le,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq,
    COMMUTATOR = OPERATOR(bioseq.>=),
    NEGATOR = OPERATOR(bioseq.>),
    RESTRICT = pg_catalog.scalarlesel,
    JOIN = pg_catalog.scalarlejoinsel
);

CREATE OPERATOR bioseq.= (
    FUNCTION = bioseq.seq_eq,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq,
    COMMUTATOR = OPERATOR(bioseq.=),
    NEGATOR = OPERATOR(bioseq.<>),
    RESTRICT = pg_catalog.eqsel,
    JOIN = pg_catalog.eqjoinsel,
    HASHES,
    MERGES
);

CREATE OPERATOR bioseq.<> (
    FUNCTION = bioseq.seq_ne,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq,
    COMMUTATOR = OPERATOR(bioseq.<>),
    NEGATOR = OPERATOR(bioseq.=),
    RESTRICT = pg_catalog.neqsel,
    JOIN = pg_catalog.neqjoinsel
);

CREATE OPERATOR bioseq.>= (
    FUNCTION = bioseq.seq_ge,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq,
    COMMUTATOR = OPERATOR(bioseq.<=),
    NEGATOR = OPERATOR(bioseq.<),
    RESTRICT = pg_catalog.scalargesel,
    JOIN = pg_catalog.scalargejoinsel
);

CREATE OPERATOR bioseq.> (
    FUNCTION = bioseq.seq_gt,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq,
    COMMUTATOR = OPERATOR(bioseq.<),
    NEGATOR = OPERATOR(bioseq.<=),
    RESTRICT = pg_catalog.scalargtsel,
    JOIN = pg_catalog.scalargtjoinsel
);

CREATE OPERATOR CLASS bioseq.seq_ops DEFAULT
    FOR TYPE bioseq.seq USING btree AS
    OPERATOR 1 bioseq.< (bioseq.seq, bioseq.seq),
    OPERATOR 2 bioseq.<= (bioseq.seq, bioseq.seq),
    OPERATOR 3 bioseq.= (bioseq.seq, bioseq.seq),
    OPERATOR 4 bioseq.>= (bioseq.seq, bioseq.seq),
    OPERATOR 5 bioseq.> (bioseq.seq, bioseq.seq),
    FUNCTION 1 bioseq.seq_cmp(bioseq.seq, bioseq.seq)
;

CREATE FUNCTION bioseq.seq_concat(left bioseq.seq, right bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_concat'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.seq_concat_text(sequence bioseq.seq, value text)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_concat_text'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.text_concat_seq(value text, sequence bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'text_concat_seq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.seq_concat_varchar(sequence bioseq.seq, value varchar)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_concat_varchar'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.varchar_concat_seq(value varchar, sequence bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'varchar_concat_seq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.seq_concat_text_array(sequence bioseq.seq, array text[])
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_concat_text_array'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.text_array_concat_seq(array text[], sequence bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'text_array_concat_seq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.seq_concat_letter_array(sequence bioseq.seq, array bioseq.letter[])
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'seq_concat_letter_array'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.letter_array_concat_seq(array bioseq.letter[], sequence bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'letter_array_concat_seq'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.reverse(input bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'reverse'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.reverse_complement(input bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'reverse_complement'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.array_cat(left bioseq.seq, right bioseq.seq)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'array_cat'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.array_length(input bioseq.seq, dimension integer)
RETURNS bigint
AS 'MODULE_PATHNAME', 'array_length'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.cardinality(input bioseq.seq)
RETURNS bigint
AS 'MODULE_PATHNAME', 'cardinality'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.to_text(input bioseq.seq)
RETURNS text
AS 'MODULE_PATHNAME', 'to_text'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.to_varchar(input bioseq.seq)
RETURNS varchar
AS 'MODULE_PATHNAME', 'to_varchar'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.to_array(input bioseq.seq)
RETURNS bioseq.letter[]
AS 'MODULE_PATHNAME', 'to_array'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.from_array(array bioseq.letter[], width_bits integer)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'from_array'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.from_array(array bioseq.letter[], alphabet_name text, width_bits integer)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'from_array'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.seq_concat,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.seq
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.seq_concat_text,
    LEFTARG = bioseq.seq,
    RIGHTARG = text
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.text_concat_seq,
    LEFTARG = text,
    RIGHTARG = bioseq.seq
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.seq_concat_varchar,
    LEFTARG = bioseq.seq,
    RIGHTARG = varchar
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.varchar_concat_seq,
    LEFTARG = varchar,
    RIGHTARG = bioseq.seq
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.seq_concat_text_array,
    LEFTARG = bioseq.seq,
    RIGHTARG = text[]
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.text_array_concat_seq,
    LEFTARG = text[],
    RIGHTARG = bioseq.seq
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.seq_concat_letter_array,
    LEFTARG = bioseq.seq,
    RIGHTARG = bioseq.letter[]
);

CREATE OPERATOR bioseq.|| (
    FUNCTION = bioseq.letter_array_concat_seq,
    LEFTARG = bioseq.letter[],
    RIGHTARG = bioseq.seq
);

CREATE CAST (bioseq.seq AS text)
    WITH FUNCTION bioseq.to_text(bioseq.seq)
    AS ASSIGNMENT;

CREATE CAST (bioseq.seq AS varchar)
    WITH FUNCTION bioseq.to_varchar(bioseq.seq)
    AS ASSIGNMENT;

CREATE FUNCTION bioseq.seq_hash(input bioseq.seq)
RETURNS integer
AS 'MODULE_PATHNAME', 'seq_hash'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq.seq_hash_extended(input bioseq.seq, seed bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'seq_hash_extended'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq_internal.slice_trace_self_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'slice_trace_self_test'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE EXECUTE ON FUNCTION bioseq_internal.slice_trace_self_test() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.slice_payload_allocation_probe(action integer)
RETURNS bigint
AS 'MODULE_PATHNAME', 'slice_payload_allocation_probe'
LANGUAGE C
VOLATILE
PARALLEL RESTRICTED
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq_internal.slice_payload_allocation_probe(integer) FROM PUBLIC;

CREATE FUNCTION bioseq_internal.slice_trace_worker_failure_probe(action integer)
RETURNS boolean
AS 'MODULE_PATHNAME', 'slice_trace_worker_failure_probe'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq_internal.slice_trace_worker_failure_probe(integer) FROM PUBLIC;

CREATE FUNCTION bioseq_internal.arm_slice_dsm_fixture(mode text)
RETURNS void
AS 'MODULE_PATHNAME', 'arm_slice_dsm_fixture'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.arm_slice_dsm_fixture(text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.arm_slice_dsm_fixture(text) IS 'Arms one SliceProject DSM payload as incomplete, malformed, or absent for worker-state validation.';

CREATE FUNCTION bioseq_internal.synthetic_lazy_sequence(requested bigint)
RETURNS bioseq.seq
AS 'MODULE_PATHNAME', 'synthetic_lazy_sequence'
LANGUAGE C
IMMUTABLE
PARALLEL RESTRICTED
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq_internal.synthetic_lazy_sequence(bigint) FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_piece_count(input bioseq.seq)
RETURNS bigint
AS 'MODULE_PATHNAME', 'sequence_piece_count'
LANGUAGE C
IMMUTABLE
PARALLEL RESTRICTED
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq_internal.sequence_piece_count(bioseq.seq) FROM PUBLIC;

CREATE FUNCTION bioseq_internal.materialization_size_self_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'materialization_size_self_test'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE EXECUTE ON FUNCTION bioseq_internal.materialization_size_self_test() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.piece_tree_self_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'piece_tree_self_test'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE EXECUTE ON FUNCTION bioseq_internal.piece_tree_self_test() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.expanded_ownership_self_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'expanded_ownership_self_test'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE EXECUTE ON FUNCTION bioseq_internal.expanded_ownership_self_test() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.letter_wire_self_test()
RETURNS boolean
AS 'MODULE_PATHNAME', 'letter_wire_self_test'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE EXECUTE ON FUNCTION bioseq_internal.letter_wire_self_test() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.letter_hook_chain_calls()
RETURNS bigint
AS 'MODULE_PATHNAME', 'letter_hook_chain_calls'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE EXECUTE ON FUNCTION bioseq_internal.letter_hook_chain_calls() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.arm_sequence_source_failure(failure_point text)
RETURNS void
AS 'MODULE_PATHNAME', 'arm_sequence_source_failure'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.arm_sequence_source_failure(text) FROM PUBLIC;

CREATE FUNCTION bioseq_internal.disarm_sequence_source_failure()
RETURNS void
AS 'MODULE_PATHNAME', 'disarm_sequence_source_failure'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.disarm_sequence_source_failure() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.reset_sequence_source_counters()
RETURNS void
AS 'MODULE_PATHNAME', 'reset_sequence_source_counters'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.reset_sequence_source_counters() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_source_counters()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'sequence_source_counters'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.sequence_source_counters() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_source_wrong_thread_probe()
RETURNS boolean
AS 'MODULE_PATHNAME', 'sequence_source_wrong_thread_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.sequence_source_wrong_thread_probe() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_source_fault_contract_probe()
RETURNS boolean
AS 'MODULE_PATHNAME', 'sequence_source_fault_contract_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.sequence_source_fault_contract_probe() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_source_route_alias_probe()
RETURNS boolean
AS 'MODULE_PATHNAME', 'sequence_source_route_alias_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.sequence_source_route_alias_probe() FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_facade_orientation_probe(sequence bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'sequence_facade_orientation_probe'
LANGUAGE C
STABLE
PARALLEL SAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.sequence_facade_orientation_probe(bioseq.seq) FROM PUBLIC;

CREATE FUNCTION bioseq_internal.sequence_source_pre_prepare_probe()
RETURNS void
AS 'MODULE_PATHNAME', 'sequence_source_pre_prepare_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.sequence_source_pre_prepare_probe() FROM PUBLIC;

CREATE OPERATOR CLASS bioseq.seq_hash_ops DEFAULT
    FOR TYPE bioseq.seq USING hash AS
    OPERATOR 1 bioseq.= (bioseq.seq, bioseq.seq),
    FUNCTION 1 bioseq.seq_hash(bioseq.seq),
    FUNCTION 2 bioseq.seq_hash_extended(bioseq.seq, bigint)
;

CREATE OR REPLACE FUNCTION bioseq.len(input bioseq.seq)
RETURNS bigint
AS 'MODULE_PATHNAME', 'len'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

COMMENT ON FUNCTION bioseq.len(bioseq.seq) IS 'Length of a bioseq.seq value in biological symbols (residues).';

CREATE OR REPLACE FUNCTION bioseq.unit_len(input bioseq.seq)
RETURNS bigint
AS 'MODULE_PATHNAME', 'unit_len'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE FUNCTION bioseq_symbol_count_support(request_datum internal)
RETURNS internal
AS 'MODULE_PATHNAME', 'bioseq_symbol_count_support'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.symbol_count(el bioseq.seq, sym text)
RETURNS bigint
AS 'MODULE_PATHNAME', 'symbol_count'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT bioseq_symbol_count_support;

COMMENT ON FUNCTION bioseq.symbol_count(bioseq.seq, text) IS 'Number of occurrences of a given symbol within a bioseq.seq value.';

CREATE OR REPLACE FUNCTION mmsearch(el bioseq.seq, query bioseq.seq)
RETURNS boolean
AS 'MODULE_PATHNAME', 'mmsearch'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
STRICT
SUPPORT mmsearch_support;

COMMENT ON FUNCTION mmsearch(bioseq.seq, bioseq.seq) IS 'Requests similarity search with default aligner options.';

CREATE TYPE bioseq_aligner_options AS (
    kmer_size integer,
    window_size integer,
    max_gap integer,
    max_gap_ref integer,
    bandwidth integer,
    bandwidth_long integer,
    max_chain_skip integer,
    max_chain_iter integer,
    min_seed_count integer,
    min_chain_score integer,
    chain_gap_scale double precision,
    chain_skip_scale double precision,
    mask_level double precision,
    mask_len integer,
    pri_ratio double precision,
    best_n integer,
    match_score integer,
    mismatch_pen integer,
    gap_open1 integer,
    gap_ext1 integer,
    gap_open2 integer,
    gap_ext2 integer,
    sc_ambi integer,
    zdrop integer,
    end_bonus integer,
    max_occ integer,
    mid_occ integer,
    block_min_size integer,
    block_max_size integer,
    chaining_algorithm integer,
    base_alignment_algorithm integer,
    minimizer_algorithm integer,
    vulkan_enabled boolean,
    vulkan_seed_hits_enabled boolean,
    vulkan_index_cache_enabled boolean,
    index_read_requirement integer
);

COMMENT ON TYPE bioseq_aligner_options IS 'Per-call aligner tuning options for the mmsearch similarity predicate. NULL fields inherit the corresponding session aligner setting.';

CREATE OR REPLACE FUNCTION aligner_options(kmer_size integer DEFAULT NULL, window_size integer DEFAULT NULL, max_gap integer DEFAULT NULL, max_gap_ref integer DEFAULT NULL, bandwidth integer DEFAULT NULL, bandwidth_long integer DEFAULT NULL, max_chain_skip integer DEFAULT NULL, max_chain_iter integer DEFAULT NULL, min_seed_count integer DEFAULT NULL, min_chain_score integer DEFAULT NULL, chain_gap_scale double precision DEFAULT NULL, chain_skip_scale double precision DEFAULT NULL, mask_level double precision DEFAULT NULL, mask_len integer DEFAULT NULL, pri_ratio double precision DEFAULT NULL, best_n integer DEFAULT NULL, match_score integer DEFAULT NULL, mismatch_pen integer DEFAULT NULL, gap_open1 integer DEFAULT NULL, gap_ext1 integer DEFAULT NULL, gap_open2 integer DEFAULT NULL, gap_ext2 integer DEFAULT NULL, sc_ambi integer DEFAULT NULL, zdrop integer DEFAULT NULL, end_bonus integer DEFAULT NULL, max_occ integer DEFAULT NULL, mid_occ integer DEFAULT NULL, block_min_size integer DEFAULT NULL, block_max_size integer DEFAULT NULL, chaining_algorithm integer DEFAULT NULL, base_alignment_algorithm integer DEFAULT NULL, minimizer_algorithm integer DEFAULT NULL, vulkan_enabled boolean DEFAULT NULL, vulkan_seed_hits_enabled boolean DEFAULT NULL, vulkan_index_cache_enabled boolean DEFAULT NULL, index_read_requirement integer DEFAULT NULL)
RETURNS bioseq_aligner_options
LANGUAGE SQL
AS $sql$
SELECT
    kmer_size, window_size, max_gap, max_gap_ref, bandwidth, bandwidth_long,
    max_chain_skip, max_chain_iter, min_seed_count, min_chain_score,
    chain_gap_scale, chain_skip_scale, mask_level, mask_len, pri_ratio,
    best_n, match_score, mismatch_pen, gap_open1, gap_ext1, gap_open2,
    gap_ext2, sc_ambi, zdrop, end_bonus, max_occ, mid_occ,
    block_min_size, block_max_size,
    chaining_algorithm, base_alignment_algorithm, minimizer_algorithm,
    vulkan_enabled, vulkan_seed_hits_enabled, vulkan_index_cache_enabled,
    index_read_requirement
$sql$
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION mmsearch(el bioseq.seq, query bioseq.seq, opts bioseq_aligner_options)
RETURNS boolean
AS 'MODULE_PATHNAME', 'mmsearch'
LANGUAGE C
IMMUTABLE
PARALLEL SAFE
SECURITY INVOKER
SUPPORT mmsearch_support;

COMMENT ON FUNCTION mmsearch(bioseq.seq, bioseq.seq, bioseq_aligner_options) IS 'Similarity-search predicate with explicit per-call aligner options.';

CREATE OR REPLACE FUNCTION bioseq.list_metadata()
RETURNS TABLE (table_name regclass, column_attno smallint, column_name text, name text, index_type text, update_policy text, size text, age interval)
AS 'MODULE_PATHNAME', 'list_metadata'
LANGUAGE C
VOLATILE
PARALLEL SAFE
SECURITY INVOKER;

COMMENT ON FUNCTION bioseq.list_metadata() IS 'Lists registered indexes, policies, sizes, and ages for Bioseq sequence columns.';

CREATE OR REPLACE FUNCTION bioseq.update_index(rel regclass, kind text)
RETURNS bigint
AS 'MODULE_PATHNAME', 'update_index'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE TYPE bioseq.column_mapping AS (
    column_name text,
    data_source text
);

CREATE OR REPLACE FUNCTION bioseq.load(target_table regclass, file_path text, header_format text DEFAULT 'auto', columns bioseq.column_mapping[] DEFAULT NULL)
RETURNS bigint
AS 'MODULE_PATHNAME', 'load'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

COMMENT ON FUNCTION bioseq.load(regclass, text, text, bioseq.column_mapping[]) IS 'Loads FASTA or FASTQ records into a Bioseq table.';

CREATE OR REPLACE FUNCTION bioseq.last_index_merge_run_count()
RETURNS bigint
AS 'MODULE_PATHNAME', 'last_index_merge_run_count'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_execution_stats()
RETURNS TABLE (execution_id bigint, backend_pid integer, database_oid oid, effective_cpu_workers integer, gpu_device_uuid text, gpu_driver text, gpu_pipeline_cache_policy text, gpu_pipeline_cache_seed_sha256 text, gpu_pipeline_cache_seed_size bigint, gpu_pipeline_cache_manifest_checksum text, gpu_pipeline_cache_hit boolean, scan_node_count integer, nested_alignment_executor_count integer, completed boolean, partial boolean, fully_drained boolean, requested_backend text, actual_backend text, requested_target_index_source text, actual_target_index_source text, persisted_generation_id bigint, persisted_source_identity text, mixed_persisted_identity boolean, stage text, eligible_jobs bigint, executed_host_io_jobs bigint, executed_scalar_cpu_jobs bigint, executed_simd_cpu_jobs bigint, executed_vulkan_gpu_jobs bigint, fallback_jobs bigint, stage_host_wall_ns bigint, stage_gpu_device_ns bigint, target_rows bigint, attempted_alignment_jobs bigint, fully_attested_alignment_jobs bigint, unattested_alignment_jobs bigint, first_unattested_target_row bigint, first_unattested_query_ordinal bigint, cpu_prepared_index_builds bigint, cpu_prepared_index_reuses bigint, cpu_prepared_unique_keys bigint, cpu_prepared_duplicate_build_attempts bigint, cpu_prepared_key_set_checksum text, cpu_prepared_build_set_checksum text, operational_identity_conflict boolean, persisted_index_rows bigint, missing_persisted_index_rows bigint, recomputed_target_index_rows bigint, persisted_generation_read_sessions bigint, persisted_relation_opens bigint, persisted_index_scans bigint, persisted_head_scans bigint, persisted_head_rows_read bigint, persisted_head_bytes_read bigint, persisted_manifest_directory_scans bigint, persisted_manifest_rows_read bigint, persisted_manifest_bytes_read bigint, persisted_directory_chunks_read bigint, persisted_directory_bytes_read bigint, persisted_metadata_repeated_reads bigint, persisted_metadata_heap_rows_read bigint, persisted_metadata_bytes_read bigint, persisted_payload_heap_rows_read bigint, persisted_payload_bytes_read bigint, persisted_heap_rows_read bigint, persisted_toast_values_read bigint, persisted_detoast_bytes bigint, persisted_explicit_copy_bytes bigint, persisted_wanted_subkeys bigint, persisted_read_subkeys bigint, persisted_wanted_extents bigint, persisted_wanted_chunks bigint, persisted_skipped_rows bigint, persisted_repeated_chunk_reads bigint, persisted_extents_read bigint, persisted_chunks_read bigint, persisted_bytes_read bigint, persisted_bulk_read_calls bigint, persisted_planned_request_groups bigint, persisted_executed_request_groups bigint, persisted_range_scan_calls bigint, persisted_skip_budget_rows bigint, persisted_hash_build_calls bigint, persisted_assembly_ns bigint, pair_sort_calls bigint, host_pair_materialization_calls bigint, host_biological_hash_filter_calls bigint, host_query_hash_readbacks bigint, host_query_symbol_packing_calls bigint, host_target_symbol_packing_calls bigint, host_alignment_job_classification_calls bigint, host_anchor_concatenation_calls bigint, host_cigar_construction_calls bigint, full_segment_hydrations bigint, nthash_vector_batches bigint, nthash_scalar_backend_branches bigint, nthash_scalar_tail_symbols bigint, nthash_masked_tail_lanes bigint, simd_chain_vector_batches bigint, simd_chain_backtrack_vector_batches bigint, simd_chain_scalar_backend_branches bigint, simd_chain_backtrack_scalar_branches bigint, cpu_seed_positions_rejected_outside_collection bigint, gpu_partition_select_dispatches bigint, gpu_partition_anchor_merge_dispatches bigint, gpu_index_build_candidates bigint, gpu_index_build_canonical_emitted bigint, gpu_index_build_overlap_duplicates_rejected bigint, gpu_index_build_noncanonical_extras_rejected bigint, gpu_index_build_surviving_extras bigint, gpu_persisted_resident_uploads bigint, gpu_persisted_resident_cache_hits bigint, gpu_overlap_anchor_duplicates bigint, gpu_seed_dispatches bigint, gpu_chain_dispatches bigint, gpu_chain_backtrack_dispatches bigint, gpu_extend_dispatches bigint, gpu_target_window_request_dispatches bigint, gpu_target_window_logical_units_requested bigint, host_target_window_requests_consumed bigint, host_target_window_physical_units_read bigint, host_target_window_remap_units_filled bigint, host_target_window_packed_bytes_read bigint, gpu_runtime_init_ns bigint, gpu_pipeline_cache_loads bigint, gpu_pipeline_cache_seed_bytes bigint, gpu_pipeline_cache_load_ns bigint, gpu_pipeline_creation_ns bigint, gpu_submit_cpu_ns bigint, gpu_wait_cpu_ns bigint, gpu_ordinal_readback_cpu_ns bigint, gpu_result_readback_cpu_ns bigint, gpu_device_critical_path_ns bigint, gpu_timestamp_query_failures bigint, gpu_queue_submits bigint, gpu_host_waits bigint, gpu_buffer_creates bigint, gpu_memory_allocations bigint, gpu_pipeline_creates bigint, backend_declines bigint, backend_fallbacks bigint, stats_saturated boolean, persisted_source_parent_rows_read bigint, persisted_source_catalog_rows_read bigint, persisted_source_catalog_index_scans bigint, persisted_source_fragment_rows_read bigint, persisted_source_decoded_bp bigint, persisted_legacy_globalize_calls bigint, persisted_legacy_rebase_calls bigint, cold_index_preparation jsonb)
AS 'MODULE_PATHNAME', 'last_alignment_execution_stats'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_execution_stats_v2()
RETURNS TABLE (execution_id bigint, backend_pid integer, database_oid oid, effective_cpu_workers integer, gpu_device_uuid text, gpu_driver text, gpu_pipeline_cache_policy text, gpu_pipeline_cache_seed_sha256 text, gpu_pipeline_cache_seed_size bigint, gpu_pipeline_cache_manifest_checksum text, gpu_pipeline_cache_hit boolean, scan_node_count integer, nested_alignment_executor_count integer, completed boolean, partial boolean, fully_drained boolean, requested_backend text, actual_backend text, requested_target_index_source text, actual_target_index_source text, persisted_generation_id bigint, persisted_source_identity text, mixed_persisted_identity boolean, stage text, eligible_jobs bigint, executed_host_io_jobs bigint, executed_scalar_cpu_jobs bigint, executed_simd_cpu_jobs bigint, executed_vulkan_gpu_jobs bigint, fallback_jobs bigint, stage_host_wall_ns bigint, stage_gpu_device_ns bigint, target_rows bigint, attempted_alignment_jobs bigint, fully_attested_alignment_jobs bigint, unattested_alignment_jobs bigint, first_unattested_target_row bigint, first_unattested_query_ordinal bigint, cpu_prepared_index_builds bigint, cpu_prepared_index_reuses bigint, cpu_prepared_unique_keys bigint, cpu_prepared_duplicate_build_attempts bigint, cpu_prepared_key_set_checksum text, cpu_prepared_build_set_checksum text, operational_identity_conflict boolean, persisted_index_rows bigint, missing_persisted_index_rows bigint, recomputed_target_index_rows bigint, persisted_generation_read_sessions bigint, persisted_relation_opens bigint, persisted_index_scans bigint, persisted_head_scans bigint, persisted_head_rows_read bigint, persisted_head_bytes_read bigint, persisted_manifest_directory_scans bigint, persisted_manifest_rows_read bigint, persisted_manifest_bytes_read bigint, persisted_directory_chunks_read bigint, persisted_directory_bytes_read bigint, persisted_metadata_repeated_reads bigint, persisted_metadata_heap_rows_read bigint, persisted_metadata_bytes_read bigint, persisted_payload_heap_rows_read bigint, persisted_payload_bytes_read bigint, persisted_heap_rows_read bigint, persisted_toast_values_read bigint, persisted_detoast_bytes bigint, persisted_explicit_copy_bytes bigint, persisted_wanted_subkeys bigint, persisted_read_subkeys bigint, persisted_wanted_extents bigint, persisted_wanted_chunks bigint, persisted_skipped_rows bigint, persisted_repeated_chunk_reads bigint, persisted_extents_read bigint, persisted_chunks_read bigint, persisted_bytes_read bigint, persisted_bulk_read_calls bigint, persisted_planned_request_groups bigint, persisted_executed_request_groups bigint, persisted_range_scan_calls bigint, persisted_skip_budget_rows bigint, persisted_hash_build_calls bigint, persisted_assembly_ns bigint, pair_sort_calls bigint, host_pair_materialization_calls bigint, host_biological_hash_filter_calls bigint, host_query_hash_readbacks bigint, host_query_symbol_packing_calls bigint, host_target_symbol_packing_calls bigint, host_alignment_job_classification_calls bigint, host_anchor_concatenation_calls bigint, host_cigar_construction_calls bigint, full_segment_hydrations bigint, nthash_vector_batches bigint, nthash_scalar_backend_branches bigint, nthash_scalar_tail_symbols bigint, nthash_masked_tail_lanes bigint, simd_chain_vector_batches bigint, simd_chain_backtrack_vector_batches bigint, simd_chain_scalar_backend_branches bigint, simd_chain_backtrack_scalar_branches bigint, cpu_seed_positions_rejected_outside_collection bigint, gpu_partition_select_dispatches bigint, gpu_partition_anchor_merge_dispatches bigint, gpu_index_build_candidates bigint, gpu_index_build_canonical_emitted bigint, gpu_index_build_overlap_duplicates_rejected bigint, gpu_index_build_noncanonical_extras_rejected bigint, gpu_index_build_surviving_extras bigint, gpu_persisted_resident_uploads bigint, gpu_persisted_resident_cache_hits bigint, gpu_overlap_anchor_duplicates bigint, gpu_seed_dispatches bigint, gpu_chain_dispatches bigint, gpu_chain_backtrack_dispatches bigint, gpu_extend_dispatches bigint, gpu_target_window_request_dispatches bigint, gpu_target_window_logical_units_requested bigint, host_target_window_requests_consumed bigint, host_target_window_physical_units_read bigint, host_target_window_remap_units_filled bigint, host_target_window_packed_bytes_read bigint, gpu_runtime_init_ns bigint, gpu_pipeline_cache_loads bigint, gpu_pipeline_cache_seed_bytes bigint, gpu_pipeline_cache_load_ns bigint, gpu_pipeline_creation_ns bigint, gpu_submit_cpu_ns bigint, gpu_wait_cpu_ns bigint, gpu_ordinal_readback_cpu_ns bigint, gpu_result_readback_cpu_ns bigint, gpu_device_critical_path_ns bigint, gpu_timestamp_query_failures bigint, gpu_queue_submits bigint, gpu_host_waits bigint, gpu_buffer_creates bigint, gpu_memory_allocations bigint, gpu_pipeline_creates bigint, backend_declines bigint, backend_fallbacks bigint, stats_saturated boolean, persisted_source_parent_rows_read bigint, persisted_source_catalog_rows_read bigint, persisted_source_catalog_index_scans bigint, persisted_source_fragment_rows_read bigint, persisted_source_decoded_bp bigint, persisted_legacy_globalize_calls bigint, persisted_legacy_rebase_calls bigint, cold_index_preparation jsonb, simd_execution_evidence jsonb)
AS 'MODULE_PATHNAME', 'last_alignment_execution_stats_v2'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format5_reader_memory_evidence'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence_v2()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format5_reader_memory_evidence_v2'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence_v3()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format5_reader_memory_evidence_v3'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence_v4()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format5_reader_memory_evidence_v4'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence_v5()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format5_reader_memory_evidence_v5'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format5_reader_memory_evidence_v6()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format5_reader_memory_evidence_v6'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_format6_reader_evidence()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_format6_reader_evidence'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_persisted_gpu_operation_evidence_v1()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_persisted_gpu_operation_evidence_v1'
LANGUAGE C
VOLATILE
PARALLEL RESTRICTED
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.last_alignment_persisted_gpu_operation_evidence_v2()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'last_alignment_persisted_gpu_operation_evidence_v2'
LANGUAGE C
VOLATILE
PARALLEL RESTRICTED
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq_internal.reset_active_read_policy_debug()
RETURNS void
AS 'MODULE_PATHNAME', 'reset_active_read_policy_debug'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.reset_active_read_policy_debug() FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.active_read_policy_debug()
RETURNS TABLE (resolver_configured_rows bigint, resolver_authoritative_defaults bigint, resolver_non_relation bigint, dispatch_configured_batches bigint, dispatch_authoritative_default_batches bigint, dispatch_non_relation_batches bigint, last_dispatch_source text, prohibited_file_constructor_entries bigint, prohibited_file_writer_entries bigint)
AS 'MODULE_PATHNAME', 'active_read_policy_debug'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.active_read_policy_debug() FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq.last_mps_build_stats()
RETURNS TABLE (backend_pid bigint, db_oid bigint, relfilenode bigint, sequence_rowno bigint, total_run_count bigint, resident_runs bigint, file_runs bigint, resident_run_bytes bigint, resident_peak_bytes bigint, spilled_run_bytes bigint, postgres_temp_file_bytes bigint, postgres_temp_file_count bigint, merge_reader_buffer_bytes bigint, merge_pass_count bigint, spill_count_from_pressure bigint, resident_demotions bigint, position_spill_bytes bigint, position_spill_count bigint)
AS 'MODULE_PATHNAME', 'last_mps_build_stats'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.scheduler_mps_build_stats()
RETURNS TABLE (backend_pid bigint, db_oid bigint, relfilenode bigint, sequence_rowno bigint, total_run_count bigint, resident_runs bigint, file_runs bigint, resident_run_bytes bigint, resident_peak_bytes bigint, spilled_run_bytes bigint, postgres_temp_file_bytes bigint, postgres_temp_file_count bigint, merge_reader_buffer_bytes bigint, merge_pass_count bigint, spill_count_from_pressure bigint, resident_demotions bigint, position_spill_bytes bigint, position_spill_count bigint, generation bigint, worker_build_ns bigint, pipeline_mode bigint, max_concurrency bigint, index_memory_cap_mb bigint, lazy_worker_shareable bigint)
AS 'MODULE_PATHNAME', 'scheduler_mps_build_stats'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq_internal.arm_async_mps_worker_failure(failure_point text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'arm_async_mps_worker_failure'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.arm_async_mps_worker_failure(text) FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.disarm_async_mps_worker_failure()
RETURNS void
AS 'MODULE_PATHNAME', 'disarm_async_mps_worker_failure'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.disarm_async_mps_worker_failure() FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq_internal.async_mps_worker_boundary_stats()
RETURNS TABLE (relation_leaf_calls bigint, spi_leaf_calls bigint, materialize_interrupt_leaf_calls bigint, materialize_scanner_read_leaf_calls bigint, pg_leaf_failures bigint, injected_pg_leaf_failures bigint, materialize_admissions bigint, materialize_failure_unwinds bigint, materialize_admission_restored bigint, materialize_admission_mismatches bigint, last_failure_point text, armed_failure_point text, last_failure_sqlstate text, last_failure_message text)
AS 'MODULE_PATHNAME', 'async_mps_worker_boundary_stats'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.async_mps_worker_boundary_stats() FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq.scheduler_restart()
RETURNS void
AS 'MODULE_PATHNAME', 'scheduler_restart'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.scheduler_stats()
RETURNS TABLE (supervisor boolean, config_generation integer, pipeline_concurrency_mode text, min_threads integer, max_threads integer, current_threads integer, batches_scheduled bigint, batches_awaiting bigint, batches_processed bigint, queries_waiting bigint, queries_processed bigint, oldest_waiting_batch_age_secs double precision, queries_per_sec double precision, uptime_secs double precision, async_persist_jobs_waiting bigint, index_memory_reserved_bytes bigint)
AS 'MODULE_PATHNAME', 'scheduler_stats'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq_internal.cpu_prepared_target_same_object_reprepare_probe()
RETURNS boolean
AS 'MODULE_PATHNAME', 'cpu_prepared_target_same_object_reprepare_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.cpu_prepared_target_same_object_reprepare_probe() FROM PUBLIC;

CREATE OR REPLACE FUNCTION bioseq.gpu_warmup_shader_cache()
RETURNS text
AS 'MODULE_PATHNAME', 'gpu_warmup_shader_cache'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.vulkan_runtime_status(OUT available boolean, OUT device_name text, OUT max_workgroup_count_x integer, OUT scalar_block_layout boolean, OUT initialization_error text, OUT device_local_bytes bigint, OUT software_device boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'vulkan_runtime_status'
LANGUAGE C
STABLE
PARALLEL SAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.set_index_policy(rel regclass, name text, policy text)
RETURNS void
AS 'MODULE_PATHNAME', 'set_index_policy'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.set_active_read_index(rel regclass, name text)
RETURNS void
AS 'MODULE_PATHNAME', 'set_active_read_index'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.drop_all_index_tables()
RETURNS void
AS 'MODULE_PATHNAME', 'drop_all_index_tables'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

CREATE OR REPLACE FUNCTION bioseq.index_table_row_count(rel regclass, name text)
RETURNS bigint
AS 'MODULE_PATHNAME', 'index_table_row_count'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.inspect_cold_no_index_absence(rel regclass, name text, OUT target_rows bigint, OUT target_cardinality_metadata_only boolean, OUT source_parent_rows_read bigint, OUT source_fragment_rows_read bigint, OUT source_decoded_bp bigint, OUT physical_index_relations bigint, OUT persisted_index_rows bigint, OUT generation_objects bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'inspect_cold_no_index_absence'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.inspect_ready_minimizer_generation(rel regclass, index_name text, query_key bigint)
RETURNS TABLE (generation_id bigint, source_identity text, compatibility_checksum text, source_content_checksum text, database_oid oid, main_relfilenode oid, column_attno smallint, sequence_rowno bigint, locator_version smallint, bucket_idx smallint, shard_idx integer, num_fragments bigint, total_bp_size bigint, first_sub_row_no bigint, padding_letters integer, alphabet_code smallint, storage_unit_byte_width smallint, kmer_size smallint, window_size smallint, gpu_abi_version integer, segment_geometry_version smallint, position_segment_span_bp bigint, position_segment_count integer)
AS 'MODULE_PATHNAME', 'inspect_ready_minimizer_generation'
LANGUAGE C
STABLE
PARALLEL RESTRICTED
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq.inspect_ready_minimizer_generation(regclass, text, bigint) FROM PUBLIC;

COMMENT ON FUNCTION bioseq.inspect_ready_minimizer_generation(regclass, text, bigint) IS 'Validates a source-bound ready minimizer generation without reading payload extents or enabling live generation reads.';

CREATE OR REPLACE FUNCTION bioseq.inspect_ready_minimizer_format6_generation(rel regclass, index_name text, query_key bigint)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'inspect_ready_minimizer_format6_generation'
LANGUAGE C
STABLE
PARALLEL RESTRICTED
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq.inspect_ready_minimizer_format6_generation(regclass, text, bigint) FROM PUBLIC;

COMMENT ON FUNCTION bioseq.inspect_ready_minimizer_format6_generation(regclass, text, bigint) IS 'Validates a source-bound ready Format-6 generation and verifies its metadata, catalog rows, byte counts, and checksums.';

CREATE OR REPLACE FUNCTION bioseq_internal.format6_parallel_scan_probe(source_table regclass, index_name text, sequence_rowno bigint, requested_workers integer)
RETURNS jsonb
AS 'MODULE_PATHNAME', 'format6_parallel_scan_probe'
LANGUAGE C
VOLATILE
PARALLEL RESTRICTED
SECURITY INVOKER
STRICT;

REVOKE EXECUTE ON FUNCTION bioseq_internal.format6_parallel_scan_probe(regclass, text, bigint, integer) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.format6_parallel_scan_probe(regclass, text, bigint, integer) IS 'Validates a ready generation row, plain payload, and address checksum using a serial scan when requested_workers is zero or a shared parallel heap scan when it is positive.';

CREATE OR REPLACE FUNCTION bioseq_internal.build_minimizer_fixture(source_table regclass, index_name text, sequence_rowno bigint, kmer_size smallint, window_size smallint, selected_wire_format smallint, requested_backend text DEFAULT 'scalar')
RETURNS TABLE (sidecar_table text, wire_format smallint, generation_id bigint, source_identity text, source_content_checksum text, compatibility_checksum text, source_num_fragments bigint, source_total_bp bigint, source_padding_letters bigint, writer_rows bigint, writer_batches bigint, published_head_bytes bigint, control_count bigint, position_count bigint, directory_entries bigint, position_segment_count bigint, control_encoded_bytes bigint, position_encoded_bytes bigint, control_chunks bigint, position_chunks bigint)
AS 'MODULE_PATHNAME', 'build_minimizer_fixture'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.build_minimizer_fixture(regclass, text, bigint, smallint, smallint, smallint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.build_minimizer_fixture(regclass, text, bigint, smallint, smallint, smallint, text) IS 'Builds and publishes an authenticated minimizer generation for one explicit sketch backend without opening a read session.';

CREATE OR REPLACE FUNCTION bioseq_internal.read_minimizer_fixture(source_table regclass, index_name text, sequence_rowno bigint, kmer_size smallint, window_size smallint, selected_wire_format smallint, requested_backend text DEFAULT 'scalar')
RETURNS TABLE (sidecar_table text, wire_format smallint, generation_id bigint, source_identity text, source_content_checksum text, compatibility_checksum text, source_num_fragments bigint, source_total_bp bigint, source_padding_letters bigint, validated_control_bytes bigint, validated_position_bytes bigint, validated_control_checksum text, validated_position_checksum text, lookup_count bigint, lookup_bp bigint, lookup_reverse boolean, lookup_positions_complete boolean, relation_opens bigint, index_scans bigint, head_scans bigint, manifest_directory_scans bigint, planned_request_groups bigint, executed_request_groups bigint, bulk_read_calls bigint, range_scan_calls bigint, payload_wanted_extents bigint, payload_read_extents bigint, payload_wanted_chunks bigint, payload_delivered_chunks bigint, payload_delivered_bytes bigint, payload_heap_rows_read bigint, payload_skipped_rows bigint, toast_values_read bigint, detoast_bytes bigint, cache_lookups bigint, selective_cold_index_preparation jsonb)
AS 'MODULE_PATHNAME', 'read_minimizer_fixture'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.read_minimizer_fixture(regclass, text, bigint, smallint, smallint, smallint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.read_minimizer_fixture(regclass, text, bigint, smallint, smallint, smallint, text) IS 'Opens an authenticated minimizer-generation read session and returns payload validation, lookup, scan, and cache counters.';

CREATE OR REPLACE FUNCTION bioseq_internal.scan_minimizer_generation_fixture(source_table regclass, index_name text, sequence_rowno bigint, kmer_size smallint, window_size smallint, selected_wire_format smallint, requested_backend text)
RETURNS TABLE (sidecar_table text, selected_backend text, generation_id bigint, control_bytes bigint, position_bytes bigint, control_checksum text, position_checksum text, relation_opens bigint, index_scans bigint, range_scan_calls bigint, payload_heap_rows bigint, delivered_chunks bigint, delivered_bytes bigint, planned_extents bigint, completed_extents bigint, scan_calls bigint)
AS 'MODULE_PATHNAME', 'scan_minimizer_generation_fixture'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.scan_minimizer_generation_fixture(regclass, text, bigint, smallint, smallint, smallint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.scan_minimizer_generation_fixture(regclass, text, bigint, smallint, smallint, smallint, text) IS 'Scans one complete minimizer generation through the requested sketch backend and returns payload and scan counters without decoding payload bytes.';

CREATE OR REPLACE FUNCTION bioseq_internal.remove_sequence_fragment(source_table regclass, index_name text, sequence_rowno bigint, fragment_ordinal integer)
RETURNS void
AS 'MODULE_PATHNAME', 'remove_sequence_fragment'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.remove_sequence_fragment(regclass, text, bigint, integer) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.remove_sequence_fragment(regclass, text, bigint, integer) IS 'Removes the selected non-first sequence fragment referenced by a source locator.';

CREATE OR REPLACE FUNCTION bioseq_internal.copy_primary_index_shape(target_table regclass, donor_index regclass, shape text)
RETURNS void
AS 'MODULE_PATHNAME', 'copy_primary_index_shape'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.copy_primary_index_shape(regclass, regclass, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.copy_primary_index_shape(regclass, regclass, text) IS 'Copies selected primary-index predicate or expression metadata from a donor index inside the current transaction.';

CREATE OR REPLACE FUNCTION bioseq_internal.rewrite_source_checksum(target_table regclass, query_key bigint, xor_mask bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'rewrite_source_checksum'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.rewrite_source_checksum(regclass, bigint, bigint) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.rewrite_source_checksum(regclass, bigint, bigint) IS 'Applies the requested mask to matching source and generation checksums while keeping their metadata consistent.';

CREATE OR REPLACE FUNCTION bioseq_internal.exercise_generation_session_failure(source_table regclass, index_name text, query_key bigint, failure_mode text)
RETURNS void
AS 'MODULE_PATHNAME', 'exercise_generation_session_failure'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.exercise_generation_session_failure(regclass, text, bigint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.exercise_generation_session_failure(regclass, text, bigint, text) IS 'Exercises the requested read-session failure mode and verifies permit and resource cleanup.';

CREATE OR REPLACE FUNCTION bioseq_internal.arm_inline_sequence_pg_failure(failure_point text)
RETURNS void
AS 'MODULE_PATHNAME', 'arm_inline_sequence_pg_failure'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.arm_inline_sequence_pg_failure(text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.arm_inline_sequence_pg_failure(text) IS 'Arms one PostgreSQL error at the selected inline-sequence lookup, read, detoast, or close operation.';

CREATE OR REPLACE FUNCTION bioseq_internal.inspect_generation_storage_transition(source_table regclass, index_name text, sequence_rowno bigint, operation text)
RETURNS TABLE (before_oid oid, operation_oid oid, after_oid oid, same_handle_rebound boolean, generation_schema_after boolean, chunk_cache_hit boolean, assembled_cache_hit boolean)
AS 'MODULE_PATHNAME', 'inspect_generation_storage_transition'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.inspect_generation_storage_transition(regclass, text, bigint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.inspect_generation_storage_transition(regclass, text, bigint, text) IS 'Runs the selected generation-storage transition and returns relation-handle, rollback, and cache-invalidation state.';

CREATE OR REPLACE FUNCTION bioseq_internal.build_ready_mpf_generation(source_table regclass, index_name text, sequence_rowno bigint, backend text)
RETURNS TABLE (sidecar_table text, column_attno smallint, source_sequence_rowno bigint, source_epoch text, source_num_fragments bigint, source_total_bp bigint, selected_backend text, wire_format smallint, published boolean, head_manifest_verified boolean, generation_id bigint, source_identity text, source_content_checksum text, compatibility_checksum text, manifest_target_length bigint, kmer_size smallint, window_size smallint, minimizer_scheme_id integer, minimizer_scheme_version integer, gpu_abi_version integer, position_segment_span_bp bigint, position_segment_count integer, writer_rows bigint, writer_batches bigint, control_count bigint, position_count bigint, producer_scan_count bigint, geometry_retry_count bigint, short_core_runs bigint, scalar_core_runs bigint, exact_simd_core_runs bigint, paper_nthash_core_runs bigint, vulkan_core_runs bigint, execution_accepted boolean, exact_proof_present boolean, exact_proof_accepted boolean, exact_vector_batches bigint, exact_scalar_backend_branches bigint, paper_proof_present boolean, paper_status text, paper_proof_accepted boolean, paper_packed_decode_batches bigint, paper_fallback_calls bigint, vulkan_completed_roundtrips bigint, vulkan_downloaded_minimizers bigint)
AS 'MODULE_PATHNAME', 'build_ready_mpf_generation'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.build_ready_mpf_generation(regclass, text, bigint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.build_ready_mpf_generation(regclass, text, bigint, text) IS 'Builds and publishes a ready PostgreSQL-backed MPF generation without changing active routing or metadata age.';

CREATE OR REPLACE FUNCTION bioseq_internal.build_ready_mps_generation(source_table regclass, index_name text, sequence_rowno bigint, backend text)
RETURNS TABLE (sidecar_table text, column_attno smallint, source_sequence_rowno bigint, source_epoch text, source_num_fragments bigint, source_total_bp bigint, selected_backend text, wire_format smallint, published boolean, head_manifest_verified boolean, generation_id bigint, source_identity text, source_content_checksum text, compatibility_checksum text, manifest_target_length bigint, kmer_size smallint, window_size smallint, minimizer_scheme_id integer, minimizer_scheme_version integer, gpu_abi_version integer, position_segment_span_bp bigint, position_segment_count integer, writer_rows bigint, writer_batches bigint, control_count bigint, position_count bigint, spill_run_count bigint, spilled_event_count bigint, spill_operation_epoch bigint, spill_generation_token bigint, spill_mutation_version bigint, spill_qualification_scan_count bigint, spill_qualification_merge_count bigint, configured_core_span_bp bigint, peak_core_span_bp bigint, peak_core_events bigint, short_core_runs bigint, scalar_core_runs bigint, exact_simd_core_runs bigint, paper_nthash_core_runs bigint, vulkan_core_runs bigint, execution_accepted boolean, exact_proof_present boolean, exact_proof_accepted boolean, exact_vector_batches bigint, exact_scalar_backend_branches bigint, paper_proof_present boolean, paper_status text, paper_proof_accepted boolean, paper_packed_decode_batches bigint, paper_fallback_calls bigint, vulkan_completed_roundtrips bigint, vulkan_downloaded_minimizers bigint, replay_scan_count bigint, replay_merge_pass_count bigint, replay_reroute_pass_count bigint, replay_completed_scan_count bigint, replay_failed_scan_count bigint, replay_binding_checked_failed_scan_count bigint, replay_illegal_span_transition_count bigint, replay_first_completed_event_count bigint, replay_last_completed_event_count bigint, replay_final_span_bp bigint, replay_final_generation_token bigint, replay_final_mutation_version bigint, replay_visited_event_count bigint, evidence_manifest_position_count bigint, evidence_manifest_position_segment_span_bp bigint, release_generation_token bigint, release_mutation_before bigint, release_mutation_after bigint, spill_resources_released boolean)
AS 'MODULE_PATHNAME', 'build_ready_mps_generation'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.build_ready_mps_generation(regclass, text, bigint, text) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.build_ready_mps_generation(regclass, text, bigint, text) IS 'Builds and publishes a ready PostgreSQL-backed MPS generation, releases its spill runs, and leaves active routing and metadata age unchanged.';

CREATE OR REPLACE FUNCTION bioseq_internal.teardown_diagnostics(OUT lock_fallback_classifications bigint, OUT missing_relation_observations bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'teardown_diagnostics'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.teardown_diagnostics() FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.teardown_diagnostics() IS 'Returns backend-local DROP EXTENSION lock-fallback and missing-relation counters.';

CREATE OR REPLACE FUNCTION bioseq_internal.match_owner_finalization_scope_diagnostics(reset boolean, OUT scopes_entered bigint, OUT scopes_exited bigint, OUT scopes_active bigint)
RETURNS record
AS 'MODULE_PATHNAME', 'match_owner_finalization_scope_diagnostics'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

REVOKE ALL ON FUNCTION bioseq_internal.match_owner_finalization_scope_diagnostics(boolean) FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.match_owner_finalization_scope_diagnostics(boolean) IS 'Returns and optionally resets counters for match-owner finalization scopes across PostgreSQL errors.';

CREATE OR REPLACE FUNCTION bioseq_internal.batch_join_projection_guard_probe(OUT accepts_exact_helper boolean, OUT rejects_unrelated_phv boolean, OUT rejects_volatile_mixed_expression boolean, OUT preserves_wrapper_cost_owner boolean, OUT accepts_plain_target_var boolean, OUT rejects_plain_target_sequence_var boolean, OUT rejects_whole_row_var boolean, OUT rejects_system_var boolean, OUT rejects_nulling_var boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'batch_join_projection_guard_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.batch_join_projection_guard_probe() FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.batch_join_projection_guard_probe() IS 'Reports BatchJoin base ProjectionPath eligibility and wrapper-cost ownership decisions.';

CREATE OR REPLACE FUNCTION bioseq_internal.match_helper_wrapper_propagation_probe(OUT exact_owner_found boolean, OUT clone_applied boolean, OUT owner_cloned boolean, OUT projection_cloned boolean, OUT material_cloned boolean, OUT memoize_cloned boolean, OUT unique_cloned boolean, OUT append_cloned boolean, OUT helper_on_owner boolean, OUT helper_on_projection boolean, OUT helper_on_material boolean, OUT helper_on_memoize boolean, OUT helper_on_unique boolean, OUT helper_on_append boolean, OUT originals_unchanged boolean, OUT sibling_unchanged boolean, OUT duplicate_owner_rejected boolean, OUT unknown_wrapper_rejected boolean, OUT group_result_qual_rejected boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'match_helper_wrapper_propagation_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.match_helper_wrapper_propagation_probe() FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.match_helper_wrapper_propagation_probe() IS 'Reports match-helper ownership and copy-on-write propagation through Projection, Material, Memoize, Unique, and Append paths.';

CREATE OR REPLACE FUNCTION bioseq_internal.base_match_executor_translation_probe(OUT target_offset_accepted boolean, OUT target_attno_disagreement_rejected boolean, OUT exact_site_offset_accepted boolean, OUT lineage_alias_disagreement_rejected boolean, OUT outer_slot_accepted boolean, OUT outer_wrong_slot_rejected boolean, OUT inner_slot_accepted boolean, OUT inner_wrong_slot_rejected boolean, OUT index_slot_accepted boolean, OUT index_wrong_slot_rejected boolean, OUT special_out_of_range_rejected boolean)
RETURNS record
AS 'MODULE_PATHNAME', 'base_match_executor_translation_probe'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

REVOKE ALL ON FUNCTION bioseq_internal.base_match_executor_translation_probe() FROM PUBLIC;

COMMENT ON FUNCTION bioseq_internal.base_match_executor_translation_probe() IS 'Reports whether carrier translation accepts exact setrefs offsets and slots and rejects attribute, alias, slot, or range mismatches.';

CREATE OR REPLACE FUNCTION bioseq.index_distinct_sub_keys(rel regclass, name text, query_key bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'index_distinct_sub_keys'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.column_row_number(rel regclass, ctid tid)
RETURNS bigint
AS 'MODULE_PATHNAME', 'column_row_number'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq_internal.write_index_chunk(rel regclass, name text, query_key bigint, sub_key bigint, chunk_no integer, payload bytea)
RETURNS void
AS 'MODULE_PATHNAME', 'write_index_chunk'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq_internal.delete_index_manifest(rel regclass, name text, query_key bigint)
RETURNS void
AS 'MODULE_PATHNAME', 'delete_index_manifest'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

COMMENT ON FUNCTION bioseq.update_index(regclass, text) IS 'Rebuilds a named sequence index and returns the number of indexed sequences.';

CREATE OR REPLACE FUNCTION bioseq.create_sample_table(table_name text, n_sequences integer, sequence_bp_size bigint, alphabet text DEFAULT 'dna4', seed bigint DEFAULT 42)
RETURNS oid
AS 'MODULE_PATHNAME', 'create_sample_table'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

COMMENT ON FUNCTION bioseq.create_sample_table(text, integer, bigint, text, bigint) IS 'Creates a Bioseq table containing streamed synthetic dna4 sequences.';

CREATE OR REPLACE FUNCTION bioseq.inspect_locator(seq bioseq.seq)
RETURNS TABLE (version smallint, bucket_idx smallint, shard_idx integer, num_fragments bigint, total_bp_size bigint, first_sub_row_no bigint, source_epoch bigint, total_padding integer)
AS 'MODULE_PATHNAME', 'inspect_locator'
LANGUAGE C
IMMUTABLE
PARALLEL UNSAFE
SECURITY INVOKER
STRICT;

CREATE OR REPLACE FUNCTION bioseq.create_query_target_sample_table(table_name text, fragment_count integer, fragment_length_mean bigint, fragment_length_stddev double precision, reference_length bigint, substitution_rate double precision, insertion_rate double precision, deletion_rate double precision, insertion_length_mean double precision, deletion_length_mean double precision, reverse_complement_rate double precision, seed bigint)
RETURNS oid
AS 'MODULE_PATHNAME', 'create_query_target_sample_table'
LANGUAGE C
VOLATILE
PARALLEL UNSAFE
SECURITY INVOKER;

COMMENT ON FUNCTION bioseq.create_query_target_sample_table(text, integer, bigint, double precision, bigint, double precision, double precision, double precision, double precision, double precision, double precision, bigint) IS 'Creates synthetic query-target pairs with ground-truth coordinates.';

COMMENT ON FUNCTION bioseq.inspect_locator(bioseq.seq) IS 'Decodes locator version, shard, fragment range, source epoch, and padding fields from a sequence value.';
