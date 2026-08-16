---
title: Read, search, and write sequences quickly
slug: quick_read_search_write
description: Build a useful Bioseq table, load it safely, read compact values, and run an indexed search.
order: 5
keywords: [getting started, reads, writes, mmsearch, indexing, PostgreSQL]
draft: false
---

This tutorial builds one small sequence table and uses it for the three jobs
that most applications need first:

1. Write sequences beside ordinary PostgreSQL metadata
2. Read lengths and selected bases without turning every value into text
3. Search stored targets with `mmsearch`

The examples use DNA with the four symbols A, C, G, and T. The later tutorials
show the `dna5` alphabet, packed storage widths, slicing, joins, remote
providers, caching, CPU dispatch, and GPU controls in more detail.

## Install the extension

Install Bioseq once in each database where you want to use it:

```sql
CREATE EXTENSION bioseq;
```

The extension creates the `bioseq.seq` type, the `bioseq` table access method,
the search predicate, sequence functions, and the planner support that connects
them.

You can confirm that PostgreSQL sees the extension before creating application
tables:

```sql
SELECT extname, extversion
FROM pg_extension
WHERE extname = 'bioseq';
```

If this returns no row, connect to the intended database and install the
extension there. PostgreSQL extensions are installed per database, not once per
server.

## Create a table for sequences and metadata

A searchable table uses `USING bioseq`. Ordinary PostgreSQL columns can sit
beside one or more sequence columns:

```sql
CREATE TABLE sample_reads (
    read_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    sample_id text NOT NULL,
    collected_on date NOT NULL,
    passed_qc boolean NOT NULL DEFAULT false,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
```

The sequence declaration has two independent parts:

- `dna4` accepts A, C, G, and T
- `32` requests 32-bit packed storage units

The number `32` is not a maximum sequence length. A value may contain far more
than 32 bases. Use `bioseq.len(sequence)` when you need its length in biological
symbols.

The active build profile can normalize accepted widths to one physical width.
Check what this server bound to the column instead of assuming that the number
written in the `CREATE TABLE` text stayed distinct:

```sql
SELECT a.attname AS column_name,
       format_type(a.atttypid, a.atttypmod) AS active_type
FROM pg_attribute AS a
WHERE a.attrelid = 'sample_reads'::regclass
  AND a.attnum > 0
  AND NOT a.attisdropped
ORDER BY a.attnum;
```

The everyday build profile reports accepted sequence widths as the 64-bit
physical form. Profiles that retain all requested widths report them
separately. This does not change the alphabet or the sequence length.

The table access method matters. A regular heap table can hold a
`bioseq.seq` value, but the sequence-aware storage, index maintenance, and
`BioseqScan` search path in this workflow start with `USING bioseq`.

Inspect the table definition in `psql` with:

```text
\d+ sample_reads
```

Check for `Access method: bioseq` in the output.

## Choose a write strategy before loading rows

Bioseq has two normal write patterns. Pick one for each load rather than
switching in the middle without a reason.

### Keep the search index current during ordinary writes

For interactive inserts and steady application traffic, enable insert
maintenance in the session:

```sql
SET bioseq.index.insert_maintenance.enabled = on;
```

Then insert rows through ordinary SQL:

```sql
INSERT INTO sample_reads (
    sample_id,
    collected_on,
    passed_qc,
    sequence
)
VALUES
    ('sample-a', DATE '2026-08-01', true,
     'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGC'),
    ('sample-a', DATE '2026-08-01', true,
     'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATC'),
    ('sample-b', DATE '2026-08-02', false,
     'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAG'),
    ('sample-c', DATE '2026-08-03', true,
     'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTA');
```

The alphabet is checked as the value enters the column. A row containing `N`
would fail for this `dna4` column instead of silently changing the data.

Use a transaction when several metadata and sequence changes belong together:

```sql
BEGIN;
```

```sql
SET LOCAL bioseq.index.insert_maintenance.enabled = on;
```

```sql
INSERT INTO sample_reads (
    sample_id,
    collected_on,
    passed_qc,
    sequence
)
VALUES (
    'sample-d',
    DATE '2026-08-04',
    true,
    'ACGTACGTACGTACGTACGTACGTACGTACGT'
);
```

```sql
COMMIT;
```

`SET LOCAL` lasts only until the transaction ends. This makes the chosen write
policy explicit without changing later work done through the same connection.

### Rebuild once after a bulk load

For a large import, it can be cheaper to leave insert maintenance off while
loading and build the sequence index once afterward:

```sql
SET bioseq.index.insert_maintenance.enabled = off;
```

Load the rows with `COPY`, application batches, or `bioseq.load`. When the load
has completed, build the search index for the relation:

```sql
SELECT bioseq.update_index(
    'sample_reads'::regclass,
    'minimizers_per_fragment'
);
```

The recipe name is part of the call. `minimizers_per_fragment` is suited to
fragment-oriented searching. `minimizers_per_sequence` is the other supported
recipe name.

Do not start searches between the maintenance-off load and `update_index` if
your application expects the newly loaded rows to use the stored index. Finish
the load, update the index, and only then make that dataset visible to search
traffic.

If the load must be published atomically, stage it in a new table, finish its
index update, and switch application metadata to the ready relation in a short
transaction.

## Read sequence metadata first

Start with the smallest projection that answers the question. Length and symbol
counts do not require a text copy in the client:

```sql
SELECT read_id,
       sample_id,
       bioseq.len(sequence) AS bases,
       bioseq.symbol_count(sequence, 'A') AS a_bases
FROM sample_reads
WHERE passed_qc
ORDER BY read_id;
```

`bioseq.len` returns the number of symbols. It does not return bytes or packed
units. `bioseq.symbol_count` counts the requested symbol under the column's
alphabet.

When you are studying packing rather than biology, ask for both symbol length
and packed-unit length:

```sql
SELECT read_id,
       bioseq.len(sequence) AS symbols,
       bioseq.unit_len(sequence) AS packed_units
FROM sample_reads
ORDER BY read_id;
```

Both results are `bigint`. They describe different units and should have names
that make the distinction visible in reports. `unit_len` follows the active
physical width, so two declarations can report the same count after build
profile normalization.

Use a slice when a preview is enough:

```sql
SELECT read_id,
       sequence[:12]::text AS first_twelve,
       sequence[-12:]::text AS last_twelve
FROM sample_reads
ORDER BY read_id;
```

Bioseq slices use zero-based, half-open bounds. `[:12]` includes positions 0
through 11. `[-12:]` starts twelve symbols before the end. The slicing tutorial
shows negative indexes, clamping, NULL bounds, concatenation, and reverse
operations.

Convert the whole value to text only when the caller actually needs every
symbol:

```sql
SELECT read_id, sequence::text
FROM sample_reads
WHERE read_id = 1;
```

This keeps list pages and metadata endpoints from moving full sequence payloads
that they do not display.

## Check the read path with EXPLAIN

Use plain `EXPLAIN` while changing query shape:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT read_id,
       bioseq.len(sequence),
       bioseq.symbol_count(sequence, 'A')
FROM sample_reads
WHERE passed_qc;
```

For a Bioseq custom scan, `Bioseq Projected Columns` shows what the node must
produce. `Bioseq Fetched From Meta` lists supported metadata projections such
as a symbol count. Treat the plan as a way to understand a specific statement.
Do not assume that adding an unused full-text projection is free.

Use `ANALYZE` only when it is acceptable to run the statement:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT read_id,
       bioseq.len(sequence)
FROM sample_reads
WHERE passed_qc;
```

`EXPLAIN ANALYZE` executes the query. Keep that in mind for statements that
contact remote providers or perform writes.

## Search stored targets

`public.mmsearch` is a predicate. Its first argument is the stored target
column and its second argument is the query sequence:

```sql
SELECT read_id,
       sample_id
FROM sample_reads
WHERE public.mmsearch(
    sequence,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
)
ORDER BY read_id;
```

The query value uses the same alphabet and storage width as the target column.
Writing the cast in the SQL makes that contract easy to see and catches bad
input close to the caller.

Ordinary PostgreSQL filters can narrow the candidate rows:

```sql
SELECT read_id,
       sample_id,
       collected_on
FROM sample_reads
WHERE sample_id = 'sample-a'
  AND collected_on >= DATE '2026-08-01'
  AND passed_qc
  AND public.mmsearch(
      sequence,
      'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
  )
ORDER BY read_id;
```

Keep `mmsearch` as a visible predicate in the `WHERE` clause. Hiding it inside
an expression that changes its Boolean meaning can prevent the planner from
assigning the search to `BioseqScan`.

## Return alignment coordinates

Search results can include the matched spans and CIGAR string:

```sql
SELECT read_id,
       bioseq.query_start(read_id) AS query_start,
       bioseq.query_end(read_id) AS query_end,
       bioseq.target_start(read_id) AS target_start,
       bioseq.target_end(read_id) AS target_end,
       bioseq.cigar(read_id) AS cigar
FROM sample_reads
WHERE public.mmsearch(
    sequence,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32),
    aligner_options(best_n => 3)
)
ORDER BY read_id, target_start, target_end;
```

The `read_id` passed to each projection function is the result carrier for the
current alignment row. It ties the projected coordinate to the corresponding
match.

Coordinates are zero-based and half-open. The start is included and the end is
excluded. The span length is therefore `end - start`.

One stored target can produce more than one alignment row. `best_n => 3` limits
the secondary alignments retained for that target. It does not limit the number
of stored target rows that can match.

## Confirm that the search uses BioseqScan

Explain the same predicate that the application sends:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT read_id
FROM sample_reads
WHERE passed_qc
  AND public.mmsearch(
      sequence,
      'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
  );
```

Look for `Custom Scan (BioseqScan)`. Then read these parts of the plan:

- `Bioseq Projected Columns` for the columns the scan produces
- `Bioseq Index Read Requirement` for the planned index need
- `Bioseq Match Operand Evaluation` for how the target and query are obtained
- `Bioseq Pipeline` for the selected search stages
- `Highway SIMD Dispatch` for compiled, runnable, and planned CPU targets

`EXPLAIN (ANALYZE, VERBOSE)` adds actual backend, target-index, prefilter,
cache, and pipeline counters. The EXPLAIN tutorial gives a field-by-field guide
to each Bioseq section.

If PostgreSQL does not select `BioseqScan`, check these facts in order:

1. The table was created with `USING bioseq`
2. The first `mmsearch` argument is a stored sequence column
3. The query value has a compatible alphabet
4. Insert maintenance was enabled or `bioseq.update_index` ran after the load
5. The predicate remains a top-level search condition
6. The session has not disabled the required custom-scan plan

Do not use elapsed time from a first tiny query as a throughput claim. The
first execution can include planner, scheduler, index, and dispatch setup. Use
the plan to confirm the route, check ordered results for correctness, then time
a representative batch with the same projection and filters as the
application.

## Update sequence and metadata together

An update can change ordinary columns and the sequence value in the same row:

```sql
SET bioseq.index.insert_maintenance.enabled = on;
```

```sql
UPDATE sample_reads
SET passed_qc = true,
    sequence = 'ACGTACGTACGTACGTACGTACGTACGTACGT'
WHERE read_id = 3;
```

Keep insert maintenance on for application updates when subsequent searches
must see the updated target through the maintained index.

Delete through ordinary SQL:

```sql
DELETE FROM sample_reads
WHERE read_id = 4;
```

If you deliberately perform a maintenance-off batch of inserts or sequence
updates, finish with `bioseq.update_index` before reopening the relation to
search traffic.

## Keep query values separate from SQL text

Applications should pass the query sequence as a parameter. A prepared
statement keeps data out of the SQL string while preserving the explicit
Bioseq cast:

```sql
PREPARE find_sample_reads(text, text) AS
SELECT read_id,
       sample_id,
       bioseq.target_start(read_id) AS target_start,
       bioseq.target_end(read_id) AS target_end
FROM sample_reads
WHERE sample_id = $1
  AND public.mmsearch(
      sequence,
      $2::bioseq.seq('dna4', 32)
  )
ORDER BY read_id, target_start;
```

Run it with application values:

```sql
EXECUTE find_sample_reads(
    'sample-a',
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'
);
```

The cast validates the parameter against `dna4`. An invalid symbol fails as an
input error rather than being stored or searched under a different meaning.

Release the prepared statement when the session no longer needs it:

```sql
DEALLOCATE find_sample_reads;
```

## A compact end-to-end example

The block below is suitable for a scratch database. It creates a relation,
writes targets, searches them, and cleans up:

```sql runnable id=quick_read_search_write timeout=30s entity="sql:function:bioseq.len(bioseq.seq)" entity="sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.vectorization.enabled = on;
CREATE TABLE quick_reads (
    read_id integer PRIMARY KEY,
    sample_id text NOT NULL,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
INSERT INTO quick_reads VALUES
    (1, 'alpha', 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
    (2, 'alpha', 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'),
    (3, 'beta', 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA');
-- bioseq:visible
SELECT read_id,
       sample_id,
       bioseq.len(sequence) AS bases,
       bioseq.target_start(read_id) AS target_start,
       bioseq.target_end(read_id) AS target_end
FROM quick_reads
WHERE public.mmsearch(
    sequence,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
)
ORDER BY read_id, target_start;
-- bioseq:teardown
DROP TABLE quick_reads;
```

## What to carry into an application

For steady writes, set insert maintenance on in the transaction that changes
sequence values. For bulk loads, keep maintenance off during the load and run
one explicit `bioseq.update_index` afterward.

Record the relation name, index recipe, and returned `update_index` value in
the import log. Publish the table only after that statement succeeds. This
makes a failed load distinguishable from a ready searchable generation.

Read `bioseq.len`, `bioseq.unit_len`, symbol counts, or slices when those are
all the caller needs. Avoid returning full text payloads on metadata paths.

Put the stored target first in `mmsearch`, cast the query to a compatible
`bioseq.seq` type, and keep the predicate visible to the planner. Use the
coordinate functions when callers need the actual aligned span.

Finally, inspect the real application statement with `EXPLAIN`. A short plan
check catches the common mistakes earlier than a performance investigation.
