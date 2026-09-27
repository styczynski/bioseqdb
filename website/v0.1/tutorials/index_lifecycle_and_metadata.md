---
title: Index lifecycle, choices, and metadata
slug: index_lifecycle_and_metadata
description: Build and maintain Bioseq's persisted search forms, inspect public metadata, and verify actual index use.
order: 50
keywords: [index lifecycle, minimizers, symbol counts, metadata, generations, maintenance, EXPLAIN]
draft: false
---

Bioseq's persisted sequence indexes are not PostgreSQL `CREATE INDEX` objects.
They are registered storage forms attached to each [[sql:bioseq.seq]] column of
a `USING bioseq` table. Use Bioseq's policy and maintenance functions to manage
them, then use `EXPLAIN ANALYZE` to verify what a search actually read.

This tutorial covers the full lifecycle:

1. Prepare a table and sequence column
2. Choose a persisted target form and write policy
3. Load or insert data
4. Build, inspect, and select an active read index
5. Run a strict or fallback-capable search
6. Respond to inserts, updates, deletes, and bulk reloads

Prerequisite: [Load FASTA, FASTQ, and gzip data](../fasta_fastq_gzip_and_io/)
explains the file-loading step. You can also follow this page with inserted or
generated rows.

## Start with the three registered index families

Every current Bioseq sequence column has these logical index entries:

| Index name | Logical content | Default policy | Main use |
| --- | --- | --- | --- |
| `minimizers_per_fragment` | Minimizer sketches retained in fragment-native target chunks | `UPDATE_ON_INSERT` | Default persisted target read for long or fragmented sequences |
| `minimizers_per_sequence` | One sequence-wide minimizer target form | `UPDATE_MANUALLY` | Selective repeated reads that benefit from one prepared sequence image |
| `symbol_counts` | Per-sequence counts for alphabet symbols | `UPDATE_ON_INSERT` | [[sql:bioseq.symbol_count]] and metadata-side projections |

`minimizers_per_fragment` is the default active read index. It preserves the
storage fragment boundary so a reader can admit relevant chunks without first
assembling a sequence-wide image.

`minimizers_per_sequence` represents the whole sequence as one logical target
preparation. It is heavier to maintain during write-heavy ingestion, so its
default policy is manual. The current metadata catalog reports both minimizer
families with physical layout name `CHUNKED_PER_FRAGMENT`. Do not use that
shared layout string to conclude that their logical preparation and read routes
are identical.

`symbol_counts` is not an alignment index. It supports letter-count metadata
such as [[sql:bioseq.symbol_count]] and lets an owned scan report metadata work
without returning the sequence payload.

A direct projection such as `bioseq.symbol_count(sequence, 'G')` is the current
recognized metadata lookup. Its plan lists the call under `Bioseq Fetched From
Meta`. That field is separate from the minimizer index source because the query
does not run the alignment pipeline. When a compatible stored count is absent,
the function recomputes the exact tally from the sequence. A symbol-count
predicate does not become a conventional PostgreSQL index scan merely because
the stored tally exists.

## Prepare a searchable table

A persisted Bioseq index requires a bound sequence column on a Bioseq table:

```sql
CREATE TABLE indexed_reads (
    read_id bigint PRIMARY KEY,
    sample_id text NOT NULL,
    sequence bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
```

Creating the table registers all three families for `sequence`. It does not
materialize index rows for an empty table. A table with two sequence columns
gets a separate set of metadata entries for each column number.

Alphabet, packed width, minimizer parameters, sketch scheme and format, source
identity, and accelerator compatibility are part of a persisted minimizer
generation's compatibility. Rebuilding after changing a relevant setting is
safer than assuming an older generation can be relabeled.

## Choose continuous or bulk maintenance

There are two layers of write policy.

`bioseq.index.insert_maintenance.enabled` is the session-wide gate. When it is
on, INSERT maintains every family whose table policy is `UPDATE_ON_INSERT`.
When it is off, inserts write the source sequence without those index rows.

The per-table policy is changed by `bioseq.set_index_policy`. In this release,
policy mutation uses the caller's internal `index_policy` catalog privileges;
table ownership and function EXECUTE alone do not suffice. The following is an
administrator/policy-manager operation, not an ordinary-role example:

```sql
SELECT bioseq.set_index_policy(
    'indexed_reads'::regclass,
    'minimizers_per_sequence',
    'UPDATE_ON_INSERT'
);
```

The only policy spellings are `UPDATE_ON_INSERT` and `UPDATE_MANUALLY`.
`minimizers_per_sequence` can opt into insert maintenance on the ordinary
layout. When `bioseq.aligner.grouped_soa_generation.enabled` is on, insert-time
minimizer publication is forbidden. The optimized defaults therefore leave
INSERT maintenance and asynchronous persistence off. Use one explicit
`update_index` after the source transaction commits instead. Normal searches
can prepare transient indexes before that publication; a required-persisted
search cannot.

For small steady writes, the defaults are a useful starting point. For a bulk
file or large generated table, use this sequence:

```sql
BEGIN;
SET LOCAL bioseq.index.insert_maintenance.enabled = off;

-- COPY, INSERT batches, bioseq.load, or a generator runs here
COMMIT;

SELECT bioseq.update_index(
    'indexed_reads'::regclass,
    'minimizers_per_fragment'
);
SELECT bioseq.update_index(
    'indexed_reads'::regclass,
    'symbol_counts'
);
-- ANALYZE currently has a sequence-hydration snapshot limitation; see below.
```

`bioseq.update_index` rebuilds the named family and returns the number
of sequence values indexed. It applies to every sequence column registered for
the table. Run it again when the source or compatible index recipe changes.

`ANALYZE` normally updates PostgreSQL statistics, but currently can fail on
sequence-bearing Bioseq tables because its sampling path lacks an explicit
hydration snapshot. It is omitted here; see the
[storage limitation](../columnar_storage_and_vectorized_aggregates/).
`VACUUM` reclaims ordinary dead-row
storage according to PostgreSQL rules. Neither command substitutes for
`bioseq.update_index`. Likewise, conventional `CREATE INDEX` or `REINDEX`
manages PostgreSQL indexes on scalar keys, not these three sequence forms.

## Read the public metadata catalog

`bioseq.list_metadata` returns one row per table, sequence column, and index
family:

```sql
SELECT table_name,
       column_attno,
       column_name,
       index_name,
       index_type,
       update_policy,
       storage_size,
       time_since_update
FROM bioseq.list_metadata()
WHERE table_name = 'indexed_reads'::regclass
ORDER BY column_attno, index_name;
```

A new empty table still lists the registered families. Their storage size is
`0 bytes` and their update age is NULL. After a successful build, size and age
describe the persisted storage known to the catalog.

Use `bioseq.index_table_row_count` when an operator needs a physical row count:

```sql
SELECT bioseq.index_table_row_count(
    'indexed_reads'::regclass,
    'minimizers_per_fragment'
);
```

For a current tuple, use `bioseq.column_row_number` to resolve its logical
column-store key and `bioseq.index_distinct_sub_keys` to count stored
fragment subkeys:

```sql
WITH source_row AS (
    SELECT bioseq.column_row_number(
               'indexed_reads'::regclass,
               ctid
           ) AS row_number
    FROM indexed_reads
    WHERE read_id = 42
)
SELECT bioseq.index_distinct_sub_keys(
           'indexed_reads'::regclass,
           'minimizers_per_fragment',
           row_number
       ) AS stored_fragments
FROM source_row;
```

`ctid` is a locator for the current row version, not an application identifier.
Resolve and use it in one short operation. Physical row and subkey counts are
useful observations, but a positive count alone does not establish
compatibility with the next search.

## Select the persisted read family

Choose the active minimizer family explicitly with
`bioseq.set_active_read_index` when an application depends on it. This mutation
also requires an administrator/policy-manager with internal `index_policy`
write access; it is not implied by owning the source table:

```sql
SELECT bioseq.set_active_read_index(
    'indexed_reads'::regclass,
    'minimizers_per_fragment'
);
```

`bioseq.index.sequence_read.enabled` controls whether ordinary allow-mode
searches may use persisted sequence indexes. Turning it off forces a controlled
recompute comparison. It does not delete index storage.

The per-call `aligner_options.index_read_requirement` is an integer enum:

| Value | Meaning |
| --- | --- |
| `0` | Allow a compatible persisted index or an eligible fallback |
| `1` | Require the persisted target index to be absent, forcing transient preparation |
| `2` | Require a compatible persisted target index and fail if it is unavailable |

Use value `2` for a readiness check whose success must mean that persisted data
was accepted. Use value `0` for normal workloads that are allowed to recompute.
Use value `1` only for controlled comparisons and transformed logical targets.

The native compatible-persisted CPU reader is currently owned by `BioseqBatchJoin`.
Use a query relation joined to the target table for value `2`, as below. Ordinary
column scans reject value `2`; with value `0` they may prepare transient indexes
instead. Do not infer a persisted read merely from having built an index.

## Build and verify a complete index lifecycle

This block begins with maintenance off, shows the empty metadata state, builds
all three forms, retains the default fragment-native read index, and requires a
compatible persisted index during the search. The isolated example role has the
explicit publication privileges used by the website fixture, but no policy
catalog write permission. It does not silently acquire administrator rights.

```sql runnable id=index_lifecycle_metadata timeout=45s entity="sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq,$extension.bioseq_aligner_options)" entity="sql:function:bioseq.target_start(anyelement)" entity="sql:function:bioseq.target_end(anyelement)"
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
SET bioseq.index.sequence_read.enabled = on;
SET bioseq.index.async_persistence.enabled = off;
SET bioseq.index.recompute_async_persistence.enabled = off;
SET bioseq.aligner.grouped_soa_generation.enabled = on;
SET bioseq.aligner.vulkan.enabled = off;
SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';
SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';
SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';
CREATE TABLE tutorial_indexed_reads (
    read_id integer PRIMARY KEY,
    sample_id text NOT NULL,
    seq0 bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
INSERT INTO tutorial_indexed_reads VALUES
    (1, 'alpha', (repeat('ACGT', 96) || repeat('TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA', 4) || repeat('TGCA', 32))::bioseq.seq('dna4', 64)),
    (2, 'alpha', (repeat('TGCA', 72) || repeat('TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA', 4) || repeat('ACGT', 48))::bioseq.seq('dna4', 64)),
    (3, 'beta',  repeat('GATTACA', 80)::bioseq.seq('dna4', 64));
CREATE TABLE tutorial_index_queries (
    query_id integer PRIMARY KEY,
    seq0 bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
INSERT INTO tutorial_index_queries VALUES
    (1, repeat('TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA', 4)::bioseq.seq('dna4', 64));
-- bioseq:visible
SELECT column_name,
       index_name,
       index_type,
       update_policy,
       storage_size,
       time_since_update IS NULL AS never_built
FROM bioseq.list_metadata()
WHERE table_name = 'tutorial_indexed_reads'::regclass
ORDER BY column_attno, index_name;

SELECT bioseq.update_index(
    'tutorial_indexed_reads'::regclass,
    'minimizers_per_fragment') AS fragment_indexed;
SELECT bioseq.update_index(
    'tutorial_indexed_reads'::regclass,
    'minimizers_per_sequence') AS sequence_indexed;
SELECT bioseq.update_index(
    'tutorial_indexed_reads'::regclass,
    'symbol_counts') AS symbol_counts_indexed;
-- Keep the default MPF selection; changing policy requires administrator access.
-- ANALYZE is omitted: the current sequence sampling path lacks its snapshot.

SELECT index_name,
       update_policy,
       storage_size,
       time_since_update IS NOT NULL AS has_update_time,
       bioseq.index_table_row_count(
           'tutorial_indexed_reads'::regclass,
           index_name
       ) AS physical_rows
FROM bioseq.list_metadata()
WHERE table_name = 'tutorial_indexed_reads'::regclass
ORDER BY index_name;

EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT read_id,
       bioseq.symbol_count(seq0, 'A') AS adenines
FROM tutorial_indexed_reads
ORDER BY read_id;

EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT read_id,
       bioseq.target_start(read_id) AS target_start,
       bioseq.target_end(read_id) AS target_end
FROM tutorial_index_queries AS q CROSS JOIN tutorial_indexed_reads AS t
WHERE public.mmsearch(
    t.seq0,
    q.seq0,
    aligner_options(index_read_requirement => 2)
)
ORDER BY read_id, target_start;

SELECT requested_target_index_source,
       actual_target_index_source,
       persisted_generation_id,
       persisted_source_identity,
       persisted_index_rows,
       missing_persisted_index_rows,
       recomputed_target_index_rows,
       persisted_generation_read_sessions
FROM bioseq.last_alignment_execution_stats()
WHERE stage = 'query_sketch';
-- bioseq:teardown
DROP TABLE tutorial_index_queries;
DROP TABLE tutorial_indexed_reads;
RESET bioseq.aligner.base_alignment.algorithm;
RESET bioseq.aligner.chaining.algorithm;
RESET bioseq.aligner.minimizer.algorithm;
RESET bioseq.aligner.vulkan.enabled;
RESET bioseq.aligner.grouped_soa_generation.enabled;
RESET bioseq.index.recompute_async_persistence.enabled;
RESET bioseq.index.async_persistence.enabled;
RESET bioseq.index.sequence_read.enabled;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

The first result has three registered rows with zero storage. Each build returns
three indexed sequences. The post-build table shows nonzero physical storage.
The letter-count plan reports `Bioseq Fetched From Meta` for `seq0.symbol_count`.
The analyzed plan must report `Bioseq Index Read Requirement` as
`require_compatible_persisted`; the batch-join execution summary reports
`actual_index_source=persisted`. Its query is 372 bases long so the demonstration
uses the default minimizer recipe and clears the alignment filters.

The final public `bioseq.last_alignment_execution_stats` snapshot supplies
generation identity fields and persisted-read counts. `missing_persisted_index_rows`
and `recomputed_target_index_rows` should be zero for this strict query. Read
the snapshot immediately after the statement because it is backend-local
last-run state.

For a multi-target statement, inspect the execution summary's
`persisted_identity_mixed` and `identity_conflict` flags before interpreting the
aggregate identity fields as one unique generation. Persisted-read counts with
zero missing/recomputed rows prove the index route; they do not by themselves
prove one uniform backend or source identity for every alignment job.

## Verify readiness through public SQL

Use three layers of evidence:

1. `list_metadata()` shows registered policy, size, and update age
2. Row and subkey counts show that physical content exists
3. A strict analyzed search shows that the current recipe accepted a persisted
   generation, while execution statistics identify that generation and source

Private generation inspectors and storage relations are not an application
interface. Do not grant them to an application to make a dashboard more
detailed. The three public checks above are the supported workflow.

## Understand mutation behavior

Sequence source identity changes with row versions.

- INSERT writes a new source and, when both policy layers allow it, builds the
  default fragment and symbol-count entries
- UPDATE retires the old source epoch, masks the old row version, and inserts a
  new source through the current write policy
- DELETE retires the old source identity and removes that row from visible
  reads
- A manual index does not become current for a new source merely because an old
  physical generation still exists

This prevents a strict read from accepting index data for an earlier sequence
value. With requirement `2`, a missing compatible generation is an error. With
allow mode, the plan can recompute if the selected route permits it. Read
`Persisted Index Rows`, `Missing Persisted Index Rows`, and
`Recomputed Target Index Rows` instead of inferring the outcome from table age.

After a bulk UPDATE or DELETE, run the explicit rebuilds required by the
application's active policies. Then run `ANALYZE` if scalar distributions or
row counts changed materially.

## Plan for concurrent readers and writers

Keep the initial load and index publication in one transaction when readers
must see either the old ready data set or the new ready data set. PostgreSQL
locks and snapshots govern row visibility. Bioseq source epochs keep a reader
from attaching a stale persisted generation to a newer row version.

A concurrent writer can make an allow-mode query observe a mix of persisted
and recomputed target rows under its snapshot. That is valid when results are
the same. It is not a readiness check. Use a strict query after the writer
commits when deployment policy requires every visible target to have compatible
persisted data.

## Choose a family for the workload

| Workload | Starting choice |
| --- | --- |
| Small table or one-off search | Allow transient preparation and measure before persisting every form |
| Long fragmented targets | `minimizers_per_fragment` |
| Repeated reads of stable whole sequences | Compare `minimizers_per_sequence` with the fragment route |
| Frequent inserts | Keep default MPF and symbol counts on insert, leave MPS manual |
| Bulk load | Disable insert maintenance, then rebuild selected families once |
| Strict serving tier | Build, select the active family, and run requirement `2` before publishing |
| Letter-composition filters | Maintain `symbol_counts` |

The best index is constrained by alphabet, width, query shape, minimizer
settings, target size, update rate, and repeated-read behavior. Do not choose
MPS only because its name sounds broader, or MPF only because it is the
default.

## Control build memory without promising resident execution

`bioseq.index.memory_cap_mb` bounds admitted index-build memory. A value of zero
uses the runtime memory budget. A large manual build can spill sorted runs to
PostgreSQL temporary files and merge them. Read the backend-local count
immediately after the build with `bioseq.last_index_merge_run_count`:

```sql
SELECT bioseq.last_index_merge_run_count() AS merge_runs;
```

A zero count means the last build in this backend had no runs to merge. A
positive count reports merge inputs, not a permanent table property. A later
build in the same backend replaces it.

The public `bioseq.scheduler_stats` snapshot reports current index memory
reservation and queued jobs:

```sql
SELECT current_threads,
       batches_awaiting,
       async_persist_jobs_waiting,
       index_memory_reserved_bytes
FROM bioseq.scheduler_stats();
```

`bioseq.index.async_persistence.enabled` and
`bioseq.index.recompute_async_persistence.enabled` control optional background
publication after eligible work. Both are off by default and must remain off
with grouped-SoA publication. They belong to the legacy persistence path, not
the optimized default index format.

## Safe operational checklist

Before serving indexed searches:

1. Confirm the table uses `USING bioseq` and every target column has a bound
   alphabet
2. Record the active minimizer settings and chosen family
3. Choose insert maintenance or a bulk manual build
4. Build symbol counts separately when composition queries need them
5. Run `ANALYZE` for PostgreSQL selectivity
6. Read public metadata and physical counts
7. Run the real query with `EXPLAIN ANALYZE`
8. Compare requested and actual target-index source
9. For a readiness gate, require compatible persisted data and record the
   accepted generation and source identity
10. Repeat the check after mutations or a recipe change

Next: [Searching sequences with mmsearch](../similarity_search/) uses this
prepared table to interpret set-valued results, thresholds, coordinates,
strand, scores, and CIGAR.
