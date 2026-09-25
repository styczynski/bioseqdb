---
title: Searching sequences with mmsearch
slug: similarity_search
description: Run an index-backed sequence search and inspect its real alignment coordinates.
order: 60
keywords: [mmsearch, BioseqScan, similarity search, alignment, CIGAR]
draft: false
---

[[sql:bioseq.mmsearch]] is a PostgreSQL predicate: put it in a
`WHERE` clause over a `USING bioseq` table and Bioseq plans a sequence-aware
custom scan. Its first argument is the stored target column. Its second argument
is a [[sql:bioseq.seq]] query value with the same alphabet.

Prerequisite: [Index lifecycle, choices, and metadata](../index_lifecycle_and_metadata/)
explains how to prepare and verify the persisted target form used here.

The optimized defaults use grouped-SoA indexes. Leave legacy insert-time
maintenance disabled before loading rows:

```sql
SET bioseq.index.insert_maintenance.enabled = off;
```

After the load commits, publish the index for repeated searches. Without a
compatible stored index, normal searches use transient preparation with the
selected algorithm:

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

```sql runnable id=canonical_mmsearch timeout=15s entity="sql:function:bioseq.query_start(anyelement)" entity="sql:function:bioseq.query_end(anyelement)" entity="sql:function:bioseq.target_start(anyelement)" entity="sql:function:bioseq.target_end(anyelement)" entity="sql:function:bioseq.cigar(anyelement)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = off;
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
-- bioseq:visible
EXPLAIN (VERBOSE, COSTS OFF)
SELECT pk
FROM tutorial_mmsearch
WHERE organism = 'Homo sapiens'
  AND collected_on >= DATE '2025-01-01'
  AND public.mmsearch(
        seq0,
        'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32),
        aligner_options(best_n => 3)
      );

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
        'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna4', 32),
        aligner_options(best_n => 3)
      )
ORDER BY pk, target_start, target_end;
-- bioseq:teardown
DROP TABLE tutorial_mmsearch;
RESET bioseq.vectorization.enabled;
RESET bioseq.index.insert_maintenance.enabled;
```

The first result shows whether the planner selected `BioseqScan` for this exact
predicate. The second result contains the matching rows and their alignment
coordinates. The runnable query uses the full first read rather than only its
short shared motif so that it can pass the default alignment filters.

> [!IMPORTANT]
> Confirm that the plan selects `BioseqScan` for this single-table search.
> Its custom execution supplies the alignment coordinates and CIGAR; do not
> interpret an ordinary scan as proof that the sequence-aware path ran.

The query can return more than one alignment for one stored row. `best_n` limits
the secondary alignments retained for each target. The `pk` argument passed to
each projection function is the result carrier that ties the projected value to
the current alignment row.

Coordinates are zero-based and half-open. A start position is included and an
end position is excluded. The aligned span is therefore `end - start`.

The CIGAR string uses run lengths. `M` consumes query and target symbols. `I`
consumes query symbols. `D` consumes target symbols. For example, `29M` is an
aligned run of 29 symbols.

The full-read fixture returns target 1 with a `93M` alignment in the captured
result above. The other targets share only shorter motifs and need not pass
the default alignment filters. For queries that do produce multiple matches,
keep alignment rows separate instead of silently collapsing them by primary
key; the returned coordinates identify each match.

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

## Planner statistics: current limitation

PostgreSQL `ANALYZE` normally updates relation and column statistics to estimate
ordinary predicates such as `organism`, dates, numeric ranges, joins, and
scalar PostgreSQL indexes. In this release, the sequence-bearing Bioseq sampling
path can fail with `sequence hydration requires an explicit snapshot`. Do not
treat this tutorial as proof that `ANALYZE` is supported for that table shape;
see [the storage limitation](../columnar_storage_and_vectorized_aggregates/).
Statistics collection does not build a minimizer or symbol-count index. Use
`bioseq.update_index` for those persisted sequence forms.

Compare estimated and actual rows on the final combined query:

```sql
EXPLAIN (ANALYZE, VERBOSE)
SELECT pk
FROM tutorial_mmsearch
WHERE organism = 'Homo sapiens'
  AND public.mmsearch(
      seq0,
      'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
  );
```

An alignment predicate can return more than one match for a target row, so a
base-table row estimate and the final alignment-row count answer different
questions. Read the ordinary filter estimate, the custom search node, and the
actual output together. When the scalar estimate is poor, refresh statistics
or tune PostgreSQL statistics for that scalar column. Rebuilding a sequence
index does not repair stale scalar statistics.

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

## Tune thresholds and scoring deliberately

`aligner_options` can override one call without changing the session defaults.
Its user-facing groups are:

- sketch shape with `kmer_size`, `window_size`, `max_occ`, and `mid_occ`
- chaining thresholds and geometry with `min_seed_count`, `min_chain_score`,
  gap limits, bandwidths, mask values, and `pri_ratio`
- alignment scoring with `match_score`, `mismatch_pen`, two gap-open and
  gap-extension pairs, `zdrop`, and `end_bonus`
- result breadth with `best_n`
- named algorithm selectors and Vulkan booleans
- persisted-target policy with `index_read_requirement`

Change one group at a time and keep the complete option record with any saved
alignment. The score is meaningful only under the scoring and search parameters
that produced it.

For example, a higher minimum chain score can reject weak candidate chains
before base alignment:

```sql
SELECT pk,
       bioseq.chain_score(pk) AS chain_score,
       bioseq.dp_score(pk) AS dp_score,
       bioseq.edit(pk) AS edits,
       bioseq.mapq(pk) AS mapq,
       bioseq.is_primary(pk) AS is_primary
FROM tutorial_mmsearch
WHERE public.mmsearch(
    seq0,
    $1::bioseq.seq('dna4', 32),
    aligner_options(
        min_chain_score => 30,
        best_n => 2
    )
)
ORDER BY pk, is_primary DESC, chain_score DESC;
```

`chain_score` describes the seed chain. `dp_score` describes base alignment.
`edit` counts edit operations under the selected final alignment. `mapq` is the
mapping-quality projection. These values answer different questions and should
not be substituted for one another as a generic similarity percentage.

Changing `kmer_size` or `window_size` also changes persisted-index
compatibility. Allow mode can recompute when the current persisted generation
does not fit. Strict persisted mode fails instead. Rebuild the selected index
with the intended recipe before using strict mode as a readiness gate.

Bioseq has one recognized edit-distance prefilter shape:

```sql
SELECT pk
FROM tutorial_mmsearch
WHERE public.mmsearch(
        seq0,
        $1::bioseq.seq('dna4', 32)
      )
  AND bioseq.edit(pk) < 5;
```

The bound must be an integer constant and the comparison must remain visible to
the owner scan. The prefilter rejects candidates that cannot pass the bound.
An accepted candidate still goes through the authoritative alignment and exact
filter. Read the prefilter stage and counters in `EXPLAIN ANALYZE` rather than
assuming that every edit predicate selected it.

Traceback and CIGAR construction are projection-driven. A key-only query can
avoid that work. Adding [[sql:bioseq.cigar]] requests the alignment path needed
to return the CIGAR, so explain the same projection list that the application
will use.

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

After a load or update commits, refresh the relation to make subsequent searches
use the persisted index for the new data:

```sql
SELECT bioseq.update_index(
    'tutorial_mmsearch'::regclass,
    'minimizers_per_fragment'
);
```

For steady application writes, keep legacy maintenance off in the writing
transaction, then publish the grouped index after commit when needed:

```sql
BEGIN;
```

```sql
SET LOCAL bioseq.index.insert_maintenance.enabled = off;
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

Next: learn the exact coordinate and materialization rules for
[Slice, concatenate, reverse, and measure sequences](../slice_concat_reverse/).
