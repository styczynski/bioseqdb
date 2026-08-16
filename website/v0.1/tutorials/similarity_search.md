---
title: Searching sequences with mmsearch
slug: similarity_search
description: Run an index-backed sequence search and inspect its real alignment coordinates.
order: 30
keywords: [mmsearch, BioseqScan, similarity search, alignment, CIGAR]
draft: false
---

[[sql:bioseq.mmsearch]] is a PostgreSQL predicate: put it in a
`WHERE` clause over a `USING bioseq` table and Bioseq plans a sequence-aware
custom scan. Its first argument is the stored target column. Its second argument
is a [[sql:bioseq.seq]] query value with the same alphabet.

Keep insert-time index maintenance enabled before loading rows:

```sql
SET bioseq.index.insert_maintenance.enabled = on;
```

If you disabled maintenance for a bulk import, rebuild the index before the
search:

```sql
SELECT bioseq.update_index(
  'tutorial_mmsearch'::regclass,
  'minimizers_per_fragment'
);
```

## Search stored sequences

The query returns each matching primary key together with query and target
coordinates and the alignment's CIGAR string. Those projections come from
[[sql:bioseq.query_start]], [[sql:bioseq.query_end]],
[[sql:bioseq.target_start]], [[sql:bioseq.target_end]], and
[[sql:bioseq.cigar]].

```sql runnable id=canonical_mmsearch timeout=15s entity="sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)" entity="sql:function:bioseq.query_start(anyelement)" entity="sql:function:bioseq.query_end(anyelement)" entity="sql:function:bioseq.target_start(anyelement)" entity="sql:function:bioseq.target_end(anyelement)" entity="sql:function:bioseq.cigar(anyelement)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.vectorization.enabled = on;
CREATE TABLE tutorial_mmsearch (
  pk integer PRIMARY KEY,
  organism text NOT NULL,
  collected_on date NOT NULL,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO tutorial_mmsearch (pk, organism, collected_on, seq0) VALUES
  (1, 'Homo sapiens', DATE '2026-06-10', 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
  (2, 'Homo sapiens', DATE '2025-11-04', 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'),
  (3, 'Homo sapiens', DATE '2024-02-18', 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'),
  (4, 'Mus musculus', DATE '2026-07-01', 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'),
  (5, 'Homo sapiens', DATE '2026-01-20', 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA');
DO $$
DECLARE
  plan_json jsonb;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) '
          'SELECT pk FROM tutorial_mmsearch '
          'WHERE organism = ''Homo sapiens'' '
          'AND collected_on >= DATE ''2025-01-01'' '
          'AND public.mmsearch(seq0, '
          '''CTAGTCACTGTAGCCTGATCGTAGCTAGT''::bioseq.seq(''dna4'', 32), '
          'aligner_options(best_n => 3))'
    INTO plan_json;
  IF plan_json #>> '{0,Plan,"Node Type"}' IS DISTINCT FROM 'Custom Scan'
     OR plan_json #>> '{0,Plan,"Custom Plan Provider"}' IS DISTINCT FROM 'BioseqScan' THEN
    RAISE EXCEPTION
      'canonical_mmsearch: expected BioseqScan Custom Scan, got %', plan_json;
  END IF;
END $$;
-- bioseq:visible
SELECT pk,
       organism,
       COUNT(*) OVER (PARTITION BY pk) AS matches_for_row,
       bioseq.query_start(pk) AS query_start,
       bioseq.query_end(pk) AS query_end,
       bioseq.target_start(pk) AS target_start,
       bioseq.target_end(pk) AS target_end,
       bioseq.cigar(pk) AS cigar
FROM tutorial_mmsearch
WHERE organism = 'Homo sapiens'
  AND collected_on >= DATE '2025-01-01'
  AND public.mmsearch(
        seq0,
        'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32),
        aligner_options(best_n => 3)
      )
ORDER BY pk, target_start, target_end;
-- bioseq:teardown
DROP TABLE tutorial_mmsearch;
RESET bioseq.vectorization.enabled;
RESET bioseq.index.insert_maintenance.enabled;
```

The query can return more than one alignment for one stored row. `best_n` limits
the secondary alignments retained for each target. The `pk` argument passed to
each projection function is the result carrier that ties the projected value to
the current alignment row.

Coordinates are zero-based and half-open. A start position is included and an
end position is excluded. The aligned span is therefore `end - start`.

The CIGAR string uses run lengths. `M` consumes query and target symbols. `I`
consumes query symbols. `D` consumes target symbols. For example, `29M` is an
aligned run of 29 symbols.

For this fixture, the filtered query returns five rows. Targets 1 and 2 each
return one `29M` alignment. Target 5 returns three `29M` alignments starting at
target positions 4, 38, and 84. That is a useful first check that the client
keeps alignment rows separate instead of silently collapsing them by primary
key.

## Choose alignment projections for the caller

Coordinates and CIGAR are not the only match projections. Ask for scoring and
seed fields only when the endpoint uses them:

```sql
SELECT pk,
       bioseq.score(pk) AS score,
       bioseq.divergence(pk) AS divergence,
       bioseq.edit(pk) AS edits,
       bioseq.seed_count(pk) AS seed_count,
       bioseq.target_start(pk) AS target_start,
       bioseq.cigar(pk) AS cigar
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32),
    aligner_options(best_n => 3)
)
ORDER BY score DESC, pk, target_start;
```

`score` is useful for ordering alignments under one chosen option set.
`divergence`, `edit`, and `seed_count` are reported properties of that
alignment. Do not compare scores produced by different option sets as if they
were one common scale. Keep the options beside any stored result or exported
report.

## Bind query text to the target type

Applications normally receive a query as text. Cast it at the SQL boundary so
PostgreSQL validates the alphabet before planning the search:

```sql
PREPARE find_tutorial_matches(text) AS
SELECT pk,
       bioseq.target_start(pk) AS target_start,
       bioseq.target_end(pk) AS target_end,
       bioseq.cigar(pk) AS cigar
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    $1::bioseq.seq('dna4', 32)
)
ORDER BY pk, target_start, target_end;
```

```sql
EXECUTE find_tutorial_matches(
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'
);
```

```sql
DEALLOCATE find_tutorial_matches;
```

The target column and query value use the same bound alphabet and width. A text
parameter containing N fails for `dna4` with SQLSTATE `22P02`.

Do not concatenate a query sequence into SQL text. A typed parameter separates
data from the statement and gives the server one explicit validation point.

In the remaining application-query snippets, `$1`, `$2`, and `$3` denote bound
parameters supplied through a prepared statement or PostgreSQL's extended query
protocol.

## Combine metadata filters with sequence search

Keep selective ordinary predicates beside `mmsearch`:

```sql
SELECT pk,
       organism,
       collected_on
FROM tutorial_mmsearch
WHERE organism = $1
  AND collected_on >= $2
  AND public.mmsearch(
      seq0,
      $3::bioseq.seq('dna4', 32)
  )
ORDER BY pk;
```

PostgreSQL and Bioseq can assign the metadata and sequence work to their
appropriate paths. Explain the final statement with representative parameter
types and table statistics because selectivity affects plan choice.

Project only what the caller needs. A key-only endpoint can omit CIGAR and
coordinates:

```sql
SELECT DISTINCT pk
FROM tutorial_mmsearch
WHERE organism = 'Homo sapiens'
  AND public.mmsearch(
      seq0,
      'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
  )
ORDER BY pk;
```

`DISTINCT` changes the result from alignment rows to target keys. Use it only
when the application truly does not need separate alignments.

## Control secondary alignments with best_n

The two-argument form uses the configured defaults:

```sql
public.mmsearch(target_sequence, query_sequence)
```

The three-argument form accepts `aligner_options`:

```sql
public.mmsearch(
    target_sequence,
    query_sequence,
    aligner_options(best_n => 3)
)
```

`best_n` limits secondary alignments retained for each target. It does not mean
three target rows, and it does not replace a SQL `LIMIT` over the final result.

If the caller wants at most 100 alignment rows after ordering, express that
separately:

```sql
SELECT pk,
       bioseq.target_start(pk) AS target_start,
       bioseq.target_end(pk) AS target_end,
       bioseq.cigar(pk) AS cigar
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    $1::bioseq.seq('dna4', 32),
    aligner_options(best_n => 3)
)
ORDER BY pk, target_start, target_end
LIMIT 100;
```

Choose a stable ordering before paginating. Without `ORDER BY`, PostgreSQL does
not promise result order.

## Convert coordinates to spans

Query and target intervals are half-open. Calculate lengths by subtraction:

```sql
WITH match_coordinates AS MATERIALIZED (
    SELECT pk,
           bioseq.query_start(pk) AS query_start,
           bioseq.query_end(pk) AS query_end,
           bioseq.target_start(pk) AS target_start,
           bioseq.target_end(pk) AS target_end
    FROM tutorial_mmsearch
    WHERE public.mmsearch(
        seq0,
        $1::bioseq.seq('dna4', 32)
    )
)
SELECT pk,
       query_start,
       query_end,
       query_end - query_start AS query_span,
       target_start,
       target_end,
       target_end - target_start AS target_span
FROM match_coordinates
ORDER BY pk, target_start;
```

Do not add one to the difference. A span `[10, 15)` contains five positions.

If the target is a stored slice, target coordinates are relative to that slice:

```sql
WITH relative_matches AS MATERIALIZED (
    SELECT pk,
           100 AS slice_origin,
           bioseq.target_start(pk) AS relative_target_start,
           bioseq.target_end(pk) AS relative_target_end
    FROM tutorial_mmsearch
    WHERE public.mmsearch(
        seq0[100:500],
        $1::bioseq.seq('dna4', 32)
    )
)
SELECT pk,
       slice_origin + relative_target_start AS stored_target_start,
       slice_origin + relative_target_end AS stored_target_end
FROM relative_matches
ORDER BY pk, stored_target_start;
```

The fixed lower bound becomes the coordinate offset. For a row-dependent or
negative lower bound, compute the resolved slice origin rather than adding a
hard-coded value. Keep match helpers as plain projections in the search level,
then perform coordinate arithmetic in the outer level.

## Read a search plan in layers

Start with plain EXPLAIN:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT pk
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
);
```

Check these fields in order:

1. `Custom Scan (BioseqScan)` is the selected node
2. `Bioseq Projected Columns` contains only needed output
3. `Bioseq Index Read Requirement` describes the target-index contract
4. `Bioseq Fetched From Sequence Index` and `Bioseq Recomputed` describe the
   planned mix
5. `Bioseq Pipeline` lists selected algorithms and stages
6. `Highway SIMD Dispatch` lists compiled, runnable, and planned CPU targets

Then use analyzed output when executing the search is acceptable:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, FORMAT JSON)
SELECT pk
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
);
```

The execution section adds actual backend, actual target-index source,
persisted and recomputed rows, GPU dispatches, prefilter work, host work, and
executed Highway architecture.

Use JSON when an application records selected fields. Keep text output for
interactive reading. The EXPLAIN tutorial describes every Bioseq group and
property.

## Compare stored-index reads with recompute

`bioseq.index.sequence_read.enabled` controls whether a search may read stored
sequence-index chunks.

To inspect the stored-index route:

```sql
SET bioseq.index.sequence_read.enabled = on;
```

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT pk
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    $1::bioseq.seq('dna4', 32)
);
```

To force recomputation for a controlled comparison:

```sql
SET bioseq.index.sequence_read.enabled = off;
```

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT pk
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    $1::bioseq.seq('dna4', 32)
);
```

Compare result rows before timing. Then compare `Actual Target Index Source`,
`Persisted Index Rows`, `Missing Persisted Index Rows`, and `Recomputed Target
Index Rows`.

This setting is useful for diagnosis and controlled comparisons. It is not a
replacement for maintaining the index after a bulk load.

## Compare scalar and vectorized execution

The vectorization switch is a Boolean route control:

```sql
SET bioseq.vectorization.enabled = off;
```

Run the search and retain its ordered result. Then repeat with:

```sql
SET bioseq.vectorization.enabled = on;
```

The result rows and alignment projections should agree. Use EXPLAIN to compare
the chosen pipeline, Highway functions, executed architecture, and runtime
counters.

The switch does not select a particular CPU ISA. Highway chooses one runnable
target from the compiled and CPU-supported target sets.

For GPU alignment controls, use the Highway and GPU tutorial. Enabling Vulkan
alone does not mean a device stage ran. Read actual backend and GPU dispatch
counters.

## Keep one search owner per target frontier

Do not write two `mmsearch` predicates over the same target in one `WHERE`
clause:

```sql
SELECT pk
FROM tutorial_mmsearch
WHERE public.mmsearch(
        seq0,
        $1::bioseq.seq('dna4', 32)
      )
  AND public.mmsearch(
        seq0,
        $2::bioseq.seq('dna4', 32)
      );
```

This stacked shape fails with SQLSTATE `0A000`. Each search carries alignment
state for its owned target frontier, and two owners on the same frontier would
make helpers such as `bioseq.cigar(pk)` ambiguous.

Run two statements and intersect their keys, or use separate target frontiers
in a sequence join. The multi-sequence tutorial shows a three-table chain with
two independent searches.

## Refresh after maintenance-off writes

If insert maintenance was off during a load or update, refresh the relation
before searching the new data:

```sql
SELECT bioseq.update_index(
    'tutorial_mmsearch'::regclass,
    'minimizers_per_fragment'
);
```

For steady application writes, enable maintenance in the writing transaction:

```sql
BEGIN;
```

```sql
SET LOCAL bioseq.index.insert_maintenance.enabled = on;
```

```sql
INSERT INTO tutorial_mmsearch (
    pk,
    organism,
    collected_on,
    seq0
)
VALUES (
    6,
    'Homo sapiens',
    DATE '2026-08-16',
    $1::bioseq.seq('dna4', 32)
);
```

```sql
COMMIT;
```

Choose one write policy for each load. A search between maintenance-off writes
and the later index update can observe a different target-index mix from the
ready state.

## Handle search errors by category

An input error with SQLSTATE `22P02` means the query text does not fit the
declared alphabet.

A valid query with no matches returns zero rows. It is not an input error and
does not return one row containing false. Treat an empty result separately from
planner, index, and provider failures.

An incompatible bound type with SQLSTATE `42804` means target and query
alphabets do not satisfy the same search contract.

A `stacked mmsearch` error with SQLSTATE `0A000` means the statement gives one
target frontier more than one search owner.

A missing operator or function with SQLSTATE `42883` can mean the session has a
restrictive search path and the SQL used an unqualified Bioseq name. Use
`public.mmsearch` and schema-qualified helper functions as shown here.

If the query runs but does not select `BioseqScan`, inspect the plan and the
table access method before changing alignment parameters.

## Troubleshooting

Use `EXPLAIN (FORMAT JSON)` when a search does not select `BioseqScan`. Check
that the table uses `USING bioseq`, the query alphabet matches the target
column, index maintenance or `update_index` has run, and `mmsearch` is a
top-level predicate in the `WHERE` clause.
