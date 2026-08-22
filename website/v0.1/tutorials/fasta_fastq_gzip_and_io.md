---
title: Load FASTA, FASTQ, and gzip data
slug: fasta_fastq_gzip_and_io
description: Stream local sequence files into typed tables, map headers, understand gzip routes, and build search indexes after a load.
order: 40
keywords: [FASTA, FASTQ, gzip, BGZF, bioseq.load, ingestion, compression, streaming]
draft: false
---

`bioseq.load` reads a file visible to the PostgreSQL server and inserts
its records into an existing `USING bioseq` table. The destination column fixes
the alphabet and packed width. Optional header mappings fill ordinary scalar
columns beside the sequence.

This page separates three interfaces that are easy to confuse:

- `bioseq.load` is the public SQL loader for local files
- the external-provider relations retrieve administrator-enabled remote data
- the C++ reader module has a separate batch path, including Bioseq's custom
  parallel deflate implementation

Only the first interface is used by the runnable SQL below.

Prerequisite: use [Generate deterministic sequence tables](../sequence_generation/)
first if you need a network-free way to create practice data.

## Recognize supported local file names

The SQL loader recognizes these suffixes without regard to ASCII case:

| Content | Suffixes |
| --- | --- |
| FASTA | `.fasta`, `.fa`, `.fna` |
| FASTQ | `.fastq`, `.fq`, `.fnq` |
| Optional gzip layer | `.gz`, `.bgz`, `.bgzf` after a sequence suffix |

The base suffix chooses the parser. Bioseq also checks gzip magic bytes, so
decompression does not depend only on the final name.

The `.bgz` and `.bgzf` spellings are accepted as gzip inputs. The SQL loader
does not consume a BGZF index and does not provide random range access. It
starts at the beginning and streams decompressed records in order.

ZIP and tar files are not local `bioseq.load` formats. The NCBI external
provider has a separate bounded assembly-ZIP path. That provider downloads and
decodes selected FASTA members rather than turning `bioseq.load` into a general
archive reader.

## Understand the NCBI assembly archive route

An external sequence query for one NCBI accession beginning with `GCF_` or
`GCA_` selects the NCBI Datasets package route. For example, an
administrator-enabled installation can request ordered assembly chunks:

```sql
SELECT chunk_index,
       base_offset,
       bioseq.len(sequence_chunk) AS bases,
       end_of_record
FROM bioseq_external.sequence_chunks
WHERE provider = 'ncbi'
  AND accession = 'GCF_009858895.2'
ORDER BY chunk_index;
```

This is a remote-provider query, not a call to `bioseq.load`. The current route
downloads the complete ZIP before extraction. It caps the compressed response
at 64 MiB, caps total decoded member data at 512 MiB, and rejects an archive
with more than 4,096 entries. It also rejects unsafe member paths, requires the
package catalog and checksum manifest, selects the catalog-listed FASTA
members, and verifies each selected member before publishing its rows.

Use `sequence_chunks` when the decoded assembly should not become one large
PostgreSQL value. Chunking bounds the rows returned to PostgreSQL, but the ZIP
route still buffers a selected member while checking its digest. It is
therefore not a constant-memory or range-read interface for arbitrary assembly
sizes.

Transient download failures can be retried before extraction, for at most five
attempts. Extraction is terminal once it starts publishing rows. The provider
does not fetch a central ZIP range or seek directly to a late FASTA record.
The remote-provider tutorial covers access grants, provider errors, and staging
the returned chunks into a local search table.

## Design the destination first

The current loader accepts exactly one destination column of a bound
[[sql:bioseq.seq]] type. It rejects a typmodless sequence column and a table
with more than one sequence destination.

Create scalar columns for header fields that the application will query:

```sql
CREATE TABLE imported_reads (
    accession text PRIMARY KEY,
    organism_name text,
    collected_on date,
    seq bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
```

Identity and generated columns are not supported by `bioseq.load`. An unmapped
`NOT NULL` scalar column must have a default. This keeps every inserted row
complete without inventing a value inside the loader.

Use `dna5` when N must remain in the stored sequence. FASTA and FASTQ parsing
currently drops source bytes that are absent from the destination alphabet.
Loading an N into `dna4` can therefore shorten the value and shift later
coordinates. This differs from a direct SQL cast, where an invalid DNA4 symbol
is an input error.

## Map header fields to typed columns

The function signature is:

```sql
bioseq.load(
    target_table regclass,
    file_path text,
    header_format text DEFAULT 'auto',
    columns bioseq.column_mapping[] DEFAULT NULL
)
```

The `bioseq.column_mapping` composite type pairs a destination column
with a parsed header source:

```sql
ARRAY[
  ('accession', 'accession')::bioseq.column_mapping,
  ('organism_name', 'organism_name')::bioseq.column_mapping
]
```

The current header dialect names are `auto`, `plain`, `uni_prot`, `uni_ref`,
`uni_parc`, `uni_mes`, `spd`, `ncbi`, `ncbi_gi`, `ncbi_pdb`, `refseq`,
`genbank`, `gisaid`, and `imgt`.

`auto` samples a decompressed prefix and chooses a known dialect when possible.
If a table has exactly one text metadata column and no explicit mappings, auto
mode preserves the complete header in that column. Use an explicit format and
mapping for a durable import contract. Provider header conventions can evolve,
and silent changes in auto-detection are hard to audit later.

Mapped text is coerced through the destination PostgreSQL type. A bad date,
range, enum, domain, or constraint fails the statement with record and column
context.

## Load, build indexes, and search

For bulk ingestion, disable insert-time index maintenance during the load, then
build each required persisted form once. The website supplies a small local
FASTA fixture, so this example does not depend on a network service.

```sql runnable id=local_fasta_load_index_search timeout=35s entity="sql:type:bioseq.seq" entity="sql:function:bioseq.len(bioseq.seq)" entity="sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)" entity="sql:function:bioseq.target_start(anyelement)" entity="sql:function:bioseq.target_end(anyelement)"
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.index.sequence_read.enabled = on;
CREATE TABLE tutorial_file_reads (
    accession text PRIMARY KEY,
    organism_name text NOT NULL,
    seq bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
-- bioseq:visible
SELECT bioseq.load(
    'tutorial_file_reads'::regclass,
    'landing_reads.fasta',
    'uni_prot',
    ARRAY[
      ('accession', 'accession')::bioseq.column_mapping,
      ('organism_name', 'organism_name')::bioseq.column_mapping
    ]
) AS loaded_records;

SELECT bioseq.update_index(
    'tutorial_file_reads'::regclass,
    'minimizers_per_fragment'
) AS indexed_sequences;
SELECT bioseq.update_index(
    'tutorial_file_reads'::regclass,
    'symbol_counts'
) AS counted_sequences;

SELECT accession,
       organism_name,
       bioseq.len(seq) AS bases
FROM tutorial_file_reads
ORDER BY accession;

EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT accession,
       bioseq.target_start(accession) AS target_start,
       bioseq.target_end(accession) AS target_end
FROM tutorial_file_reads
WHERE public.mmsearch(
    seq,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 64)
)
ORDER BY accession, target_start;
-- bioseq:teardown
DROP TABLE tutorial_file_reads;
RESET bioseq.index.sequence_read.enabled;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

The load result is the number of parsed records. `bioseq.update_index`
returns the number of sequence values rebuilt for that index family. The final
plan and result group show the local search route and real match coordinates.

For ordinary trickle inserts, leave insert maintenance on instead. The index
lifecycle tutorial explains the three index families and the mutation rules in
detail.

## Understand streaming and memory

`bioseq.load` uses the streaming reader. It emits bounded packed chunks to the
column writer, so one very long FASTA sequence is not first assembled as one
PostgreSQL-sized buffer. FASTQ parsing must retain quality scratch for the
current record so it can validate the sequence/quality boundary. The SQL table
stores the sequence and mapped header fields, not FASTQ quality scores.

The SQL streaming parser is single-threaded by contract because one backend
owns the column write state. `bioseq.load.write_overlap.enabled` can overlap
input decoding with column writes when the scheduler admits a worker, but it
does not change record order or turn the parser into independent file shards.
The setting is off by default.

Treat this as bounded-memory ingestion, not as a promise that memory is
constant for every FASTQ record. A single enormous quality string still needs
record-sized scratch.

## Distinguish zlib from the custom deflate reader

The SQL streaming route opens gzip input through the regular zlib-backed
source. It does not select Bioseq's custom parallel deflate decoder.

The separate C++ batch reader `bioseq.io.reader` exposes
`read_sequences_file`. With default reader options, a memory-mapped gzip file
of at least 16 MiB compressed size may select the custom `parallel_gzip`
source. That source plans ordered decoded chunks of about 4 MiB and chooses up
to eight workers from the process concurrency limit. If construction of that
source fails, the batch reader can fall back to the zlib source before emitting
records. A failure after records have begun is terminal rather than a restart
that could duplicate output.

The custom decoder handles the DEFLATE payload inside gzip members. It maps the
compressed file and drains decoded chunks in encoded order. It is not a seek
index, HTTP range reader, or partial-record query interface. Reading a record
near the end still requires decoding the preceding stream structure.

Those C++ reader options are not PostgreSQL GUCs. Changing an aligner, cache,
or column-compression setting does not select the custom gzip backend for
`bioseq.load`.

## Handle transactions and failures

The load is one SQL statement inside the caller's transaction. A parser,
mapping, coercion, constraint, storage, or index-build failure aborts the
statement. PostgreSQL does not commit an accepted prefix as a successful load.

A safe import sequence is:

```sql
BEGIN;
SET LOCAL bioseq.index.insert_maintenance.enabled = off;

SELECT bioseq.load(
    'incoming_reads'::regclass,
    '/srv/import/run-042.fastq.gz'
);

SELECT bioseq.update_index(
    'incoming_reads'::regclass,
    'minimizers_per_fragment'
);
SELECT bioseq.update_index(
    'incoming_reads'::regclass,
    'symbol_counts'
);
ANALYZE incoming_reads;
COMMIT;
```

The file path is resolved by the database server, not the SQL client. The
server process must be able to open it, and the database role must be allowed
to invoke the loader. Local-file reads do not have an automatic retry policy.
Fix the file or destination contract and retry the transaction deliberately.

## Keep local and remote ingestion separate

`bioseq.load` does not accept an HTTP URL. Remote data uses
`bioseq_external` relations and their provider-specific filters, timeouts, and
error behavior. Stage a remote nucleotide value into a local `USING bioseq`
table before building local indexes.

Remote retries are safe only before a provider scan emits rows. After emission,
a provider error is terminal so a retry cannot silently duplicate an earlier
prefix. The remote-provider tutorial gives the current NCBI, UCSC, UniProt,
and EBI capability boundaries.

## Know the export boundary

There is no current SQL function that writes a Bioseq table as FASTA, FASTQ, or
gzip. The C++ `bioseq.io.writer` module writes plain FASTA or FASTQ batches, but
it is not a SQL export API and has no gzip output option.

Applications can format selected rows and use PostgreSQL `COPY` for their own
text export, but that is application-owned serialization. Do not describe it
as a Bioseq FASTA/FASTQ round trip unless the application also preserves and
validates headers, qualities, alphabets, and record ordering.

Next: [Index lifecycle, choices, and metadata](../index_lifecycle_and_metadata/)
covers the persisted forms that follow a load, their write policies, public
metadata, and EXPLAIN evidence.
