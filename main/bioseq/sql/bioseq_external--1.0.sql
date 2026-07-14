-- DOC0372 — Common External Sequence Table Provider.
-- Appended to the fresh-install bioseq--1.0.sql only when
-- BIOSEQ_BUILD_EXTERNAL_PG=ON. There are no existing consumers and no stored
-- data, so this is a fresh extension-build contract with no upgrade/compat SQL
-- (DOC0321). It defines, in order: the external table access method, the two
-- marker predicate functions, the bioseq_external schema, the six
-- non-materialized logical relations, and least-privilege ACLs.

-- ---------------------------------------------------------------------------
-- Table access method. The handler returns a distinct TableAmRoutine identity
-- (GetBioseqExternalTableAmRoutine); physical scans raise "Custom Scan
-- required" and all writes raise read-only errors.
-- ---------------------------------------------------------------------------
CREATE FUNCTION bioseq.bioseq_external_handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'bioseq_external_handler'
LANGUAGE C STRICT;

COMMENT ON FUNCTION bioseq.bioseq_external_handler(internal) IS
  'bioseq external logical-relation table access method handler (DOC0372)';

CREATE ACCESS METHOD bioseq_external
TYPE TABLE HANDLER bioseq.bioseq_external_handler;

CREATE SCHEMA bioseq_external;

-- ---------------------------------------------------------------------------
-- Marker predicate functions. They express semantics that are not row fields
-- (descendant taxon matching, provider-native query). They are consumed by the
-- custom scan; if one survives onto a physical executor it raises a feature
-- error, preventing accidental local execution with invented semantics.
-- ---------------------------------------------------------------------------
CREATE FUNCTION bioseq_external.taxon_matches(bigint, bigint, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'bioseq_external_taxon_matches'
LANGUAGE C IMMUTABLE STRICT PARALLEL UNSAFE;

CREATE FUNCTION bioseq_external.native_query(text, text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'bioseq_external_native_query'
LANGUAGE C IMMUTABLE STRICT PARALLEL UNSAFE;

-- ---------------------------------------------------------------------------
-- Six non-materialized relations. All use the bioseq_external access method
-- and reject INSERT/UPDATE/DELETE/COPY FROM/index creation/physical scans.
--
-- The logical entity of each relation is derived from its (fixed) relation name
-- by the CustomScan executor, NOT from a WITH (entity=...) reloption: for a
-- RELKIND_RELATION, PostgreSQL parses WITH options through heap_reloptions ->
-- default_reloptions(RELOPT_KIND_HEAP), which cannot populate a custom options
-- struct, so a custom `entity` reloption would be rejected at CREATE TABLE. The
-- names below (taxonomy, catalog, record_genes, record_locations, sequences,
-- sequence_chunks) are therefore load-bearing and must match the executor.
-- ---------------------------------------------------------------------------
CREATE TABLE bioseq_external.taxonomy (
    provider              text        NOT NULL,
    taxon_id              bigint,
    provider_taxon_id     text        NOT NULL,
    scientific_name       text,
    common_name           text,
    rank                  text,
    parent_taxon_id       bigint,
    lineage               bigint[],
    synonyms              text[],
    provider_attributes   jsonb,
    provider_version      text,
    provenance            jsonb       NOT NULL,
    retrieved_at          timestamptz NOT NULL
) USING bioseq_external;

CREATE TABLE bioseq_external.catalog (
    provider              text        NOT NULL,
    domain                text        NOT NULL,
    entity_kind           text        NOT NULL,
    molecule_kind         text,
    namespace             text,
    original_identifier   text        NOT NULL,
    accession             text        NOT NULL,
    accession_version     integer,
    aliases               jsonb,
    taxon_id              bigint,
    scientific_name       text,
    title                 text,
    genes                 jsonb,
    keywords              text[],
    locations             jsonb,
    host_taxon_id         bigint,
    geography             text,
    lineage_name          text,
    sequence_length       bigint,
    assembly_accession    text,
    assembly_database     text,
    assembly_source       text,
    assembly_level        text,
    assembly_status       text,
    release_date          date,
    update_date           date,
    is_reference          boolean,
    is_reviewed           boolean,
    is_annotated          boolean,
    is_fragment           boolean,
    retrievable           boolean     NOT NULL,
    provider_attributes   jsonb,
    provider_version      text,
    provenance            jsonb       NOT NULL,
    retrieved_at          timestamptz NOT NULL
) USING bioseq_external;

CREATE TABLE bioseq_external.record_genes (
    provider              text        NOT NULL,
    domain                text        NOT NULL,
    namespace             text,
    original_identifier   text        NOT NULL,
    accession             text        NOT NULL,
    accession_version     integer,
    gene_index            integer     NOT NULL,
    gene_count            integer     NOT NULL,
    gene_namespace        text        NOT NULL,
    gene_id               text        NOT NULL,
    gene_symbol           text,
    gene_name             text,
    gene_type             text,
    locus_tag             text,
    provider_version      text,
    provenance            jsonb       NOT NULL,
    retrieved_at          timestamptz NOT NULL
) USING bioseq_external;

CREATE TABLE bioseq_external.record_locations (
    provider              text        NOT NULL,
    domain                text        NOT NULL,
    namespace             text,
    original_identifier   text        NOT NULL,
    accession             text        NOT NULL,
    accession_version     integer,
    location_index        integer     NOT NULL,
    location_count        integer     NOT NULL,
    assembly_accession    text,
    assembly_database     text,
    sequence_alias        text        NOT NULL,
    start0                bigint      NOT NULL,
    end0                  bigint      NOT NULL,
    strand                smallint    NOT NULL,
    provider_version      text,
    provenance            jsonb       NOT NULL,
    retrieved_at          timestamptz NOT NULL
) USING bioseq_external;

CREATE TABLE bioseq_external.sequences (
    provider              text        NOT NULL,
    domain                text        NOT NULL,
    namespace             text,
    original_identifier   text        NOT NULL,
    accession             text        NOT NULL,
    accession_version     integer,
    molecule_kind         text        NOT NULL,
    taxon_id              bigint,
    assembly_accession    text,
    assembly_database     text,
    chromosome            text,
    start0                bigint,
    end0                  bigint,
    strand                smallint,
    sequence              bioseq.seq  NOT NULL,
    provider_version      text,
    provenance            jsonb       NOT NULL,
    retrieved_at          timestamptz NOT NULL
) USING bioseq_external;

CREATE TABLE bioseq_external.sequence_chunks (
    provider              text        NOT NULL,
    domain                text        NOT NULL,
    namespace             text,
    original_identifier   text        NOT NULL,
    accession             text        NOT NULL,
    accession_version     integer,
    molecule_kind         text        NOT NULL,
    taxon_id              bigint,
    assembly_accession    text,
    assembly_database     text,
    chromosome            text,
    start0                bigint,
    end0                  bigint,
    strand                smallint,
    chunk_index           bigint      NOT NULL,
    base_offset           bigint      NOT NULL,
    sequence_chunk        bioseq.seq  NOT NULL,
    end_of_record         boolean     NOT NULL,
    provider_version      text,
    provenance            jsonb       NOT NULL,
    retrieved_at          timestamptz NOT NULL
) USING bioseq_external;

-- ---------------------------------------------------------------------------
-- Least-privilege ACLs. Remote database access is not silently granted to
-- every login; the administrator issues explicit grants.
-- ---------------------------------------------------------------------------
REVOKE ALL ON SCHEMA bioseq_external FROM PUBLIC;
REVOKE ALL ON ALL TABLES IN SCHEMA bioseq_external FROM PUBLIC;
REVOKE ALL ON FUNCTION bioseq.bioseq_external_handler(internal) FROM PUBLIC;
REVOKE ALL ON FUNCTION bioseq_external.taxon_matches(bigint, bigint, text) FROM PUBLIC;
REVOKE ALL ON FUNCTION bioseq_external.native_query(text, text) FROM PUBLIC;
