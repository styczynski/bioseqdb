---
title: Query remote providers, stage sequences, and search them
slug: remote_provider_to_search
description: Use the external SQL relations for NCBI, UCSC, UniProt, and EBI, then move nucleotide records into a searchable Bioseq table.
order: 50
keywords: [remote providers, NCBI, UCSC, UniProt, EBI, external tables, mmsearch]
draft: false
---

A Bioseq build with PostgreSQL external-provider support exposes remote data as
six read-only SQL relations. You can inspect taxonomy and catalog metadata,
expand genes and locations, retrieve a bounded sequence, or stream a large
record as ordered chunks.

Remote relations use the `bioseq_external` table access method. Local searchable
relations use `USING bioseq`. A practical workflow therefore has two clear
parts:

1. Select a narrow set of remote nucleotide records through SQL
2. Insert those values into a local `USING bioseq` table and search them there

The boundary is useful. Remote queries deal with provider availability, network
latency, provider filters, and access policy. Local queries get normal Bioseq
storage, index maintenance, joins, query caching, and repeatable search latency.

## Check that external-provider support is installed

The feature is present when the database contains the external schema and table
access method:

```sql
SELECT nspname
FROM pg_namespace
WHERE nspname = 'bioseq_external';
```

```sql
SELECT amname, amtype
FROM pg_am
WHERE amname = 'bioseq_external';
```

List the six extension-owned relations:

```sql
SELECT schemaname, tablename
FROM pg_tables
WHERE schemaname = 'bioseq_external'
ORDER BY tablename;
```

For a deployment check, return all three facts in one row:

```sql
SELECT to_regnamespace('bioseq_external') IS NOT NULL AS schema_ready,
       EXISTS (
           SELECT 1
           FROM pg_am
           WHERE amname = 'bioseq_external'
             AND amtype = 't'
       ) AS table_am_ready,
       (
           SELECT count(*)
           FROM pg_tables
           WHERE schemaname = 'bioseq_external'
       ) AS relation_count;
```

A ready installation reports a schema, a table access method, and six
relations. A standard build without external-provider support reports false,
false, and zero. Stop the remote workflow at that point. Do not let an
application discover the missing feature after it has started an import.

The expected names are:

- `taxonomy`
- `catalog`
- `record_genes`
- `record_locations`
- `sequences`
- `sequence_chunks`

If the schema is absent, the installed Bioseq build does not include the
PostgreSQL external-provider feature. Adding SQL objects by hand is not a
substitute because the custom table access method and provider runtime live in
the extension binary.

## Grant remote access deliberately

The external schema, relations, and marker functions are not granted to PUBLIC
by default. A database administrator chooses which roles can make remote
requests.

A read-only application role normally needs schema usage and SELECT on the
specific relations it uses:

```sql
GRANT USAGE ON SCHEMA bioseq_external TO sequence_importer;
```

```sql
GRANT SELECT ON bioseq_external.taxonomy,
                bioseq_external.catalog,
                bioseq_external.record_genes,
                bioseq_external.record_locations,
                bioseq_external.sequences,
                bioseq_external.sequence_chunks
TO sequence_importer;
```

Grant the marker functions only if the role needs those query forms:

```sql
GRANT EXECUTE ON FUNCTION
    bioseq_external.taxon_matches(bigint, bigint, text)
TO sequence_importer;
```

```sql
GRANT EXECUTE ON FUNCTION
    bioseq_external.native_query(text, text)
TO sequence_importer;
```

Do not grant INSERT, UPDATE, or DELETE. External relations are read-only and do
not store PostgreSQL tuples. Their table access method rejects writes, indexes,
`COPY FROM`, storage rewrites, and physical scans.

Keep provider credentials in the server configuration used by the provider
runtime. Do not concatenate secrets into `native_query` text or application
logs.

## Start with taxonomy

Use an exact provider filter and a selective name:

```sql
SELECT provider,
       taxon_id,
       provider_taxon_id,
       scientific_name,
       common_name,
       rank
FROM bioseq_external.taxonomy
WHERE provider = 'ncbi'
  AND scientific_name = 'Homo sapiens'
LIMIT 5;
```

The provider value is lowercase. Keep it explicit even when only one provider
is configured. It makes routing visible in SQL and prevents an accidental
multi-provider request.

The normalized `taxon_id` is a bigint when one is available. The
`provider_taxon_id` remains text because provider identifiers do not all share
one numeric representation.

The optional taxonomy marker supports exact or descendant matching:

```sql
SELECT provider,
       taxon_id,
       scientific_name
FROM bioseq_external.taxonomy
WHERE provider = 'ncbi'
  AND bioseq_external.taxon_matches(
      taxon_id,
      9606,
      'exact'
  );
```

Use `'descendants'` only when the selected provider supports that exact marker
semantics. The planner rejects a marker it cannot translate exactly rather than
pretending a local Boolean function can reproduce the remote operation.

## Search provider catalogs without downloading sequences

The catalog relation contains normalized metadata. It is the right starting
point for finding accessions:

```sql
SELECT provider,
       accession,
       molecule_kind,
       taxon_id,
       scientific_name,
       title,
       sequence_length,
       retrievable
FROM bioseq_external.catalog
WHERE provider = 'uniprot'
  AND accession = 'P04637';
```

This asks for metadata, not sequence bytes.

Use an array parameter for a bounded accession list:

```sql
PREPARE external_metadata(text[]) AS
SELECT provider,
       accession,
       molecule_kind,
       taxon_id,
       sequence_length
FROM bioseq_external.catalog
WHERE provider IN ('ncbi', 'uniprot', 'ebi')
  AND taxon_id = 9606
  AND accession = ANY($1::text[])
ORDER BY provider, accession;
```

```sql
EXECUTE external_metadata(
    ARRAY['P04637', 'NC_012920.1']::text[]
);
```

The provider set and accession list are both bounded. This is a much safer
remote request than an unconstrained catalog scan.

When the session is finished with the prepared statement:

```sql
DEALLOCATE external_metadata;
```

## Expand genes and genomic locations

`record_genes` returns one row per normalized gene reference:

```sql
SELECT accession,
       gene_index,
       gene_count,
       gene_namespace,
       gene_id,
       gene_symbol,
       gene_name
FROM bioseq_external.record_genes
WHERE provider = 'uniprot'
  AND accession = 'P04637'
ORDER BY gene_index;
```

`gene_index` is zero-based and `gene_count` reports the record's child count.
Keep both when the order or completeness of the child list matters.

`record_locations` exposes normalized, zero-based genomic intervals:

```sql
SELECT accession,
       location_index,
       location_count,
       assembly_accession,
       assembly_database,
       sequence_alias,
       start0,
       end0,
       strand
FROM bioseq_external.record_locations
WHERE provider = 'ncbi'
  AND accession = '7157'
  AND start0 < end0
ORDER BY location_index
LIMIT 2;
```

The interval is half-open. `start0` is included and `end0` is excluded. Strand
is represented as a small integer, with provider data normalized into the
relation's contract.

## Use plain EXPLAIN before making a request

Plain EXPLAIN plans the remote scan without performing remote I/O:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT provider,
       taxon_id,
       scientific_name
FROM bioseq_external.taxonomy
WHERE provider = 'ncbi'
  AND scientific_name = 'Homo sapiens'
LIMIT 5;
```

Look for `Custom Scan (BioseqExternalScan)`. The custom node reports:

- `Bioseq External Entity`, such as `taxonomy` or `sequence`
- `Bioseq External Provider`, such as `ncbi` or `ucsc`

Read the ordinary PostgreSQL `Filter` too. It shows the selected provider and
record constraints in the planned statement.

`EXPLAIN ANALYZE` is different:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT provider,
       taxon_id,
       scientific_name
FROM bioseq_external.taxonomy
WHERE provider = 'ncbi'
  AND scientific_name = 'Homo sapiens'
LIMIT 5;
```

It executes the scan and can contact the provider. Use it only when a live
request is intended. A remote provider can be slow or unavailable even when
plain planning succeeds.

## Retrieve a bounded nucleotide region

The `sequences` relation returns a complete record or requested region in one
[[sql:bioseq.seq]] value:

```sql
SELECT provider,
       accession,
       assembly_database,
       chromosome,
       start0,
       end0,
       bioseq.len(sequence) AS bases
FROM bioseq_external.sequences
WHERE provider = 'ucsc'
  AND assembly_database = 'hg38'
  AND chromosome = 'chrM'
  AND start0 = 0
  AND end0 = 100;
```

Selecting `sequence` or an expression that needs it requests nucleotide
materialization. The standard external sequence relation stores the returned
value as `dna5` in 64-bit units, selected by relation options. Use
[[sql:bioseq.len]] to report its biological length without converting the
sequence to text.

If you need only metadata, query `catalog` instead. Do not select a sequence
value and discard it in the client.

The complete-record relation is for values that fit in one PostgreSQL datum.
Use `sequence_chunks` for a potentially large genome.

## Retrieve a large record as ordered chunks

Prepare a bounded accession query:

```sql
PREPARE external_chunks(text) AS
SELECT chunk_index,
       base_offset,
       bioseq.len(sequence_chunk) AS bases,
       end_of_record
FROM bioseq_external.sequence_chunks
WHERE provider = 'ncbi'
  AND accession = $1::text
ORDER BY chunk_index;
```

```sql
EXECUTE external_chunks('NC_012920.1');
```

The standard non-final chunk contains 1,048,572 bases. That value is aligned to
the packed `dna5` 64-bit unit layout. The final chunk can be shorter.

`chunk_index` starts at zero. `base_offset` is the chunk's starting position in
the complete sequence. `end_of_record` marks the final chunk.

Keep the explicit `ORDER BY chunk_index`. It documents the assembly order even
when a single-record plan can already provide that path order.

```sql
DEALLOCATE external_chunks;
```

## Create a local staging table for search

Remote relations do not replace a local search index. Create a normal Bioseq
table for the selected nucleotide material:

```sql
CREATE TABLE imported_provider_targets (
    provider text NOT NULL,
    accession text NOT NULL,
    start0 bigint NOT NULL,
    end0 bigint NOT NULL,
    sequence bioseq.seq('dna4', 64) NOT NULL,
    imported_at timestamptz NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (provider, accession, start0, end0)
) USING bioseq;
```

This tutorial chooses `dna4` because the next search uses a `dna4` query. The
external relation uses `dna5`, so narrowing must validate that the selected
region contains only A, C, G, and T.

Enable index maintenance for a small import:

```sql
SET bioseq.index.insert_maintenance.enabled = on;
```

You can rehearse the local half without contacting a provider. This checks the
table, index maintenance, [[sql:bioseq.mmsearch]],
[[sql:bioseq.target_start]], [[sql:bioseq.target_end]], and
[[sql:bioseq.cigar]] with one small row:

```sql
INSERT INTO imported_provider_targets (
    provider,
    accession,
    start0,
    end0,
    sequence
)
VALUES (
    'local-check',
    'region-1',
    1000,
    1058,
    'AAAATAGTCACTGTAGCCTGATCGTAGCTAGTCAACGTACGTACGTACGTACGTAAAA'
);
```

```sql
WITH local_matches AS MATERIALIZED (
    SELECT provider,
           accession,
           start0,
           bioseq.target_start(accession) AS match_start_in_region,
           bioseq.target_end(accession) AS match_end_in_region,
           bioseq.cigar(accession) AS cigar
    FROM imported_provider_targets
    WHERE public.mmsearch(
        sequence,
        'TAGTCACTGTAGCCTGATCGTAGCTAGTCA'::bioseq.seq('dna4', 64),
        aligner_options(best_n => 3)
    )
)
SELECT provider,
       accession,
       start0 + match_start_in_region AS provider_start0,
       start0 + match_end_in_region AS provider_end0,
       cigar
FROM local_matches
ORDER BY provider, accession, provider_start0;
```

The row returns the exact match as `[1004, 1034)` with CIGAR `30M`.

The runnable version records the installation's real external-provider
capability before running the local search. It does not contact a remote
service. A build without the optional provider feature therefore renders
`false`, `false`, and `0` in the first result instead of publishing a simulated
provider response.

```sql runnable id=remote_provider_capability_and_search timeout=30s entity="sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)" entity="sql:function:bioseq.target_start(anyelement)" entity="sql:function:bioseq.target_end(anyelement)" entity="sql:function:bioseq.cigar(anyelement)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.vectorization.enabled = on;
CREATE TABLE remote_provider_stage (
    provider text NOT NULL,
    accession text NOT NULL,
    start0 bigint NOT NULL,
    end0 bigint NOT NULL,
    sequence bioseq.seq('dna4', 64) NOT NULL,
    PRIMARY KEY (provider, accession, start0, end0)
) USING bioseq;
INSERT INTO remote_provider_stage VALUES (
    'local-check',
    'region-1',
    1000,
    1058,
    'AAAATAGTCACTGTAGCCTGATCGTAGCTAGTCAACGTACGTACGTACGTACGTAAAA'
);
-- bioseq:visible
SELECT to_regnamespace('bioseq_external') IS NOT NULL AS schema_ready,
       EXISTS (
           SELECT 1
           FROM pg_am
           WHERE amname = 'bioseq_external'
             AND amtype = 't'
       ) AS table_am_ready,
       (
           SELECT count(*)
           FROM pg_tables
           WHERE schemaname = 'bioseq_external'
       ) AS relation_count;

WITH local_matches AS MATERIALIZED (
    SELECT provider,
           accession,
           start0,
           bioseq.target_start(accession) AS match_start_in_region,
           bioseq.target_end(accession) AS match_end_in_region,
           bioseq.cigar(accession) AS cigar
    FROM remote_provider_stage
    WHERE public.mmsearch(
        sequence,
        'TAGTCACTGTAGCCTGATCGTAGCTAGTCA'::bioseq.seq('dna4', 64),
        aligner_options(best_n => 3)
    )
)
SELECT provider,
       accession,
       start0 + match_start_in_region AS provider_start0,
       start0 + match_end_in_region AS provider_end0,
       cigar
FROM local_matches
ORDER BY provider, accession, provider_start0;
-- bioseq:teardown
DROP TABLE remote_provider_stage;
RESET bioseq.vectorization.enabled;
RESET bioseq.index.insert_maintenance.enabled;
```

Remove the check row before importing provider data:

```sql
DELETE FROM imported_provider_targets
WHERE provider = 'local-check'
  AND accession = 'region-1';
```

## Move a remote region into local storage

Use an explicit text bridge when narrowing from external `dna5` to local
`dna4`:

```sql
INSERT INTO imported_provider_targets (
    provider,
    accession,
    start0,
    end0,
    sequence
)
SELECT provider,
       accession,
       start0,
       end0,
       sequence::text::bioseq.seq('dna4', 64)
FROM bioseq_external.sequences
WHERE provider = 'ucsc'
  AND assembly_database = 'hg38'
  AND chromosome = 'chrM'
  AND start0 = 0
  AND end0 = 100
  AND sequence::text ~ '^[ACGT]*$';
```

The text cast is deliberate. Bound values with different alphabets do not
silently convert through a direct sequence-to-sequence cast. Parsing text into
`bioseq.seq('dna4', 64)` checks every symbol against the local alphabet.

The regular expression makes the chosen policy visible, and the target cast is
still the final validator. If the remote region contains `N`, the row is not
inserted by this statement.

Confirm what became local:

```sql
SELECT provider,
       accession,
       start0,
       end0,
       bioseq.len(sequence) AS bases
FROM imported_provider_targets
ORDER BY provider, accession, start0;
```

## Search the staged sequence

The local relation now uses the normal search predicate:

The `$1` value below is a bound application parameter supplied through a
prepared statement or PostgreSQL's extended query protocol.

```sql
SELECT provider,
       accession,
       start0,
       end0,
       bioseq.target_start(accession) AS match_start_in_region,
       bioseq.target_end(accession) AS match_end_in_region,
       bioseq.cigar(accession) AS cigar
FROM imported_provider_targets
WHERE public.mmsearch(
    sequence,
    $1::bioseq.seq('dna4', 64)
)
ORDER BY provider, accession, match_start_in_region;
```

The returned target coordinates are relative to the staged sequence. Project
them at the search level, then convert them to the provider coordinate system
in an outer query:

```sql
WITH local_matches AS MATERIALIZED (
    SELECT provider,
           accession,
           start0,
           bioseq.target_start(accession) AS match_start_in_region,
           bioseq.target_end(accession) AS match_end_in_region,
           bioseq.cigar(accession) AS cigar
    FROM imported_provider_targets
    WHERE public.mmsearch(
        sequence,
        $1::bioseq.seq('dna4', 64)
    )
)
SELECT provider,
       accession,
       start0 + match_start_in_region AS provider_start0,
       start0 + match_end_in_region AS provider_end0,
       cigar
FROM local_matches
ORDER BY provider, accession, provider_start0;
```

Keep the alignment helpers as plain projections inside the search level. Do
arithmetic on their aliases outside it. This gives the custom scan clear match
placeholder sites and avoids treating a missing placeholder value as a real
coordinate.

This calculation is valid for a forward region stored without an additional
orientation transform. If you reverse-complement before staging, record that
transform and map coordinates under that orientation.

## Use bulk maintenance for many remote records

For a larger bounded import, leave insert maintenance off during the transfer:

```sql
SET bioseq.index.insert_maintenance.enabled = off;
```

Run the remote-to-local `INSERT ... SELECT`, then build the local index once:

```sql
SELECT bioseq.update_index(
    'imported_provider_targets'::regclass,
    'minimizers_per_fragment'
);
```

Do not expose the staging relation to searches until the update finishes. A
common publishing pattern is:

1. Create a new generation table
2. Import a bounded provider result into it
3. Validate row counts, lengths, and accessions
4. Run `bioseq.update_index`
5. Switch application metadata to the ready generation

This keeps a provider timeout or validation error away from the currently
searchable dataset.

## Store provenance beside the local copy

The external sequence relations include `provider_version`, `provenance`, and
`retrieved_at`. Preserve them when reproducibility matters:

```sql
CREATE TABLE imported_provider_targets_with_provenance (
    provider text NOT NULL,
    accession text NOT NULL,
    start0 bigint NOT NULL,
    end0 bigint NOT NULL,
    sequence bioseq.seq('dna4', 64) NOT NULL,
    provider_version text,
    provenance jsonb NOT NULL,
    retrieved_at timestamptz NOT NULL,
    PRIMARY KEY (provider, accession, start0, end0)
) USING bioseq;
```

Then project the fields in the same remote statement that retrieves the
sequence. Do not invent version data in application code when the provider row
already supplies it.

## Know the current protein boundary

UniProt catalog metadata is available through `bioseq_external.catalog` and
gene expansion is available through `record_genes`. Protein sequence
materialization through `bioseq_external.sequences` is not implemented in this
interface. It fails with SQLSTATE `0A000`.

Use the catalog query for protein metadata:

```sql
SELECT accession,
       title,
       sequence_length,
       is_reviewed,
       retrievable
FROM bioseq_external.catalog
WHERE provider = 'uniprot'
  AND accession = 'P04637';
```

Do not write a tutorial or application path that treats the sequence relation
as a protein loader. A clear feature error is better than parsing protein data
through a nucleotide alphabet.

## Handle remote failure separately from local search failure

A remote statement can fail because of provider availability, rate limits,
transport errors, malformed responses, unsupported fields, or cancellation.
Run it in a staging workflow that can be retried without changing the active
local table.

A local `mmsearch` failure has different checks. Inspect the local table access
method, alphabet, index maintenance, search shape, and custom plan.

Keep the two phases separate in logs:

- Remote selection identifies provider, entity, bounded filters, and request
  outcome
- Local import identifies destination generation, accepted rows, rejected
  alphabets, and index completion
- Local search identifies query type, plan, target generation, and result count

Do not log provider secrets, full authorization headers, or opaque cursors.

## Clean up the staging examples

```sql
DROP TABLE IF EXISTS imported_provider_targets;
```

```sql
DROP TABLE IF EXISTS imported_provider_targets_with_provenance;
```
