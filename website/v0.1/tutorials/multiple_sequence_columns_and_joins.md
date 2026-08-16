---
title: Search multiple sequence columns and join by sequence
slug: multiple_sequence_columns_and_joins
description: Model paired sequences, search one column, use another as a query, and run one or two sequence joins.
order: 40
keywords: [multiple sequence columns, self join, BioseqBatchJoin, lateral, mmsearch]
draft: false
---

A `USING bioseq` table can contain more than one sequence column. That is useful
for paired reads, references and probes stored together, assembled and raw
forms, or forward and reverse-oriented values.

The same columns can also take different roles in a query. One alias can supply
stored targets while another alias supplies query sequences. Bioseq then plans
the `mmsearch` predicate as a parameterized sequence join.

This tutorial covers four shapes:

1. A literal search against one of several sequence columns
2. A self join where one row's `seq1` searches another row's `seq0`
3. A join between separate target and query tables
4. A three-table query with two independent sequence searches

It ends with the unsupported stacked-search shape so an application can fail
early instead of building ambiguous SQL.

## Create a table with two sequence columns

Both columns below use the same alphabet and packed width. That makes either
column valid as a query for the other:

```sql
CREATE TABLE paired_reads (
    pk integer PRIMARY KEY,
    sample_id text NOT NULL,
    seq0 bioseq.seq('dna4', 32) NOT NULL,
    seq1 bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
```

Use names that communicate roles in the application. Generic `seq0` and `seq1`
are compact for a tutorial. A production table might use `reference_sequence`
and `probe_sequence`, or `read_one` and `read_two`.

Keep index maintenance current during this small insert:

```sql
SET bioseq.index.insert_maintenance.enabled = on;
```

```sql
SET bioseq.vectorization.enabled = on;
```

```sql
INSERT INTO paired_reads (pk, sample_id, seq0, seq1) VALUES
    (1, 'alpha',
     'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA',
     'CTAGTCACTGTAGCCTGATCGTAGCTAGT'),
    (2, 'alpha',
     'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA',
     'TCGTAGCTAGGA'),
    (3, 'beta',
     'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA',
     'TCATCATCTACTCCTA'),
    (4, 'beta',
     'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT',
     'TGTAGCCTGATCGTA'),
    (5, 'gamma',
     'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA',
     'CACTGTAGCCTGATCGTA');
```

## Read each column independently

Sequence functions take the column explicitly. There is no table-wide implicit
current sequence:

```sql
SELECT pk,
       bioseq.len(seq0) AS target_bases,
       bioseq.len(seq1) AS probe_bases
FROM paired_reads
ORDER BY pk;
```

The two lengths can follow different distributions. Keep their names and units
separate in application models.

Read only the needed sequence payload:

```sql
SELECT pk,
       seq1::text AS probe
FROM paired_reads
WHERE sample_id = 'alpha'
ORDER BY pk;
```

This does not ask the client for `seq0`.

Take different slices from each column:

```sql
SELECT pk,
       seq0[:12]::text AS target_prefix,
       seq1[-8:]::text AS probe_suffix
FROM paired_reads
ORDER BY pk;
```

Each expression retains its own source column and alphabet.

## Search one selected column

Put the stored target column first:

```sql
SELECT pk, sample_id
FROM paired_reads
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
)
ORDER BY pk;
```

Searching `seq1` instead is a different statement:

```sql
SELECT pk, sample_id
FROM paired_reads
WHERE public.mmsearch(
    seq1,
    'TCATCATCTACTCCTA'::bioseq.seq('dna4', 32)
)
ORDER BY pk;
```

The first argument tells the planner which stored sequence owns the search.
That ownership also connects alignment projections to the result rows.

## Return the matching column beside metadata

The aligned coordinates refer to the first `mmsearch` argument. For the `seq0`
search, project the result with the target row's key:

```sql
SELECT pk,
       sample_id,
       bioseq.target_start(pk) AS target_start,
       bioseq.target_end(pk) AS target_end,
       bioseq.query_start(pk) AS query_start,
       bioseq.query_end(pk) AS query_end,
       bioseq.cigar(pk) AS cigar
FROM paired_reads
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32),
    aligner_options(best_n => 3)
)
ORDER BY pk, target_start, target_end;
```

One target can produce several rows. Do not treat `pk` alone as a unique key for
the result set. A client can use `pk`, coordinate columns, and CIGAR together to
identify an alignment.

## Use a self join for stored probes

A self join gives the same physical table two SQL identities. Alias `targets`
supplies `seq0` and alias `probes` supplies `seq1`:

```sql
SELECT targets.pk AS target_pk,
       probes.pk AS probe_pk
FROM paired_reads AS targets
CROSS JOIN paired_reads AS probes
WHERE public.mmsearch(targets.seq0, probes.seq1)
ORDER BY targets.pk, probes.pk;
```

For each probe row, Bioseq can rebind `probes.seq1` as the query for target
rows. The aliases are not cosmetic. They define separate range-table entries
and make the target and query roles visible to PostgreSQL.

Keep the sequence join itself simple, then apply pair filters across a real SQL
boundary. A materialized CTE is useful when the filter depends on both sides:

```sql
WITH raw_matches AS MATERIALIZED (
    SELECT targets.pk AS target_pk,
           probes.pk AS probe_pk,
           targets.sample_id AS target_sample,
           probes.sample_id AS probe_sample
    FROM paired_reads AS targets
    CROSS JOIN paired_reads AS probes
    WHERE public.mmsearch(targets.seq0, probes.seq1)
)
SELECT target_pk,
       probe_pk,
       target_sample,
       probe_sample
FROM raw_matches
WHERE target_sample <> probe_sample
  AND probe_pk IN (1, 3, 5)
ORDER BY target_pk, probe_pk;
```

This boundary makes the order explicit. Bioseq first produces alignment rows.
PostgreSQL then filters those rows. Do not assume that an ordinary predicate
combined with `mmsearch` inside the same custom join condition will be used as
a query-side or target-side admission filter.

If a metadata filter must reduce alignment work, stage the selected rows in a
separate `USING bioseq` target or query table, maintain its index, and join those
relations. A materialized CTE of sequence values is not a replacement for a
Bioseq relation because the search predicate needs an executor owner.

## Project alignment data from a self join

The target carrier comes from the target alias:

```sql
SELECT targets.pk AS target_pk,
       probes.pk AS probe_pk,
       bioseq.target_start(targets.pk) AS target_start,
       bioseq.target_end(targets.pk) AS target_end,
       bioseq.cigar(targets.pk) AS cigar
FROM paired_reads AS targets
CROSS JOIN paired_reads AS probes
WHERE public.mmsearch(targets.seq0, probes.seq1)
ORDER BY target_pk, probe_pk, target_start;
```

Keep projection helpers close to the search they describe. In a query with
several independent searches, a bare carrier can become ambiguous if more than
one match owner could supply it.

If you do not need alignment details, omit the helpers. A simple pair list is
smaller to transfer and easier to aggregate.

## Avoid the diagonal when needed

A self join includes each row paired with itself unless another predicate
removes it. Filter the produced pairs outside the custom join:

```sql
WITH raw_matches AS MATERIALIZED (
    SELECT targets.pk AS target_pk,
           probes.pk AS probe_pk
    FROM paired_reads AS targets
    CROSS JOIN paired_reads AS probes
    WHERE public.mmsearch(targets.seq0, probes.seq1)
)
SELECT target_pk,
       probe_pk
FROM raw_matches
WHERE target_pk <> probe_pk
ORDER BY target_pk, probe_pk;
```

Use `<` instead of `<>` when the relationship is symmetric and each unordered
pair should appear once:

```sql
WITH raw_matches AS MATERIALIZED (
    SELECT left_read.pk AS left_pk,
           right_read.pk AS right_pk
    FROM paired_reads AS left_read
    CROSS JOIN paired_reads AS right_read
    WHERE public.mmsearch(left_read.seq0, right_read.seq1)
)
SELECT left_pk,
       right_pk
FROM raw_matches
WHERE left_pk < right_pk
ORDER BY left_pk, right_pk;
```

Only use this reduction when swapping sides has the meaning your application
expects. `mmsearch(target, query)` has directional target and query roles.

## Separate targets and queries into two tables

When probes have their own lifecycle, store them separately:

```sql
CREATE TABLE search_targets (
    target_id integer PRIMARY KEY,
    cohort text NOT NULL,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
```

```sql
CREATE TABLE search_queries (
    query_id integer PRIMARY KEY,
    query_name text NOT NULL,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
```

Load targets and queries under the write policy appropriate for each relation.
Then join them with the target relation first in `mmsearch`:

```sql
SELECT queries.query_id,
       targets.target_id
FROM search_queries AS queries
JOIN search_targets AS targets
  ON public.mmsearch(targets.sequence, queries.sequence)
ORDER BY queries.query_id, targets.target_id;
```

This query shape can plan as `Custom Scan (BioseqBatchJoin)` when its ownership
and parameterization requirements are met.

## Read a BioseqBatchJoin plan

Explain the join without running it first:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT queries.query_id,
       targets.target_id
FROM search_queries AS queries
JOIN search_targets AS targets
  ON public.mmsearch(targets.sequence, queries.sequence);
```

For a `BioseqBatchJoin`, the join-specific properties answer these questions:

| Property | Question it answers |
| --- | --- |
| `Bioseq Query Batch Capacity` | How many query rows can be prepared in a batch |
| `Bioseq Residual Mmsearch Qual` | Whether an unowned search predicate remains |
| `Bioseq Batch ABI Version` | Which internal batch contract the node uses |
| `Bioseq Match Placeholder Sites` | How many projected match-result sites were rewritten |
| `Bioseq Requirement Executable` | Whether the owned search requirement can execute |
| `Bioseq Query Planner Width` | What width PostgreSQL estimated for query rows |
| `Bioseq Distinct Targets` | How many target identities execution observed |
| `Bioseq Query Batches` | How many query batches execution processed |

Some counters appear only with `EXPLAIN ANALYZE`. The node also contains the
normal Bioseq pipeline, SIMD dispatch, prefilter, index-source, and execution
sections described in the EXPLAIN tutorial.

A join filter that still prints as a raw `mmsearch` expression is worth
investigating. In the owned batch-join path, the search is represented by the
custom node rather than left as an ordinary Boolean function call.

## Use LATERAL when the query role is easier to read that way

A lateral subquery can express one search per outer query row:

```sql
SELECT queries.query_id,
       matches.target_id,
       bioseq.cigar(matches.target_id) AS cigar
FROM search_queries AS queries
CROSS JOIN LATERAL (
    SELECT targets.target_id
    FROM search_targets AS targets
    WHERE public.mmsearch(targets.sequence, queries.sequence)
) AS matches
ORDER BY queries.query_id, matches.target_id;
```

The inner search is parameterized by `queries.sequence`. This form is useful
when the inner query also needs a limit, target-side metadata predicates, or
additional projections.

For example, restrict each query to one cohort:

```sql
SELECT queries.query_id,
       matches.target_id
FROM search_queries AS queries
CROSS JOIN LATERAL (
    SELECT targets.target_id
    FROM search_targets AS targets
    WHERE targets.cohort = 'case'
      AND public.mmsearch(targets.sequence, queries.sequence)
) AS matches
ORDER BY queries.query_id, matches.target_id;
```

Compare the flat join and lateral form with `EXPLAIN`. Choose the one that
expresses the application rule clearly and produces the intended custom plan.

## Run two searches with separate target frontiers

Two searches are supported when each belongs to a separate join frontier. A
three-table chain makes that separation explicit:

Create the second target relation with the same sequence type:

```sql
CREATE TABLE second_search_targets (
    target_id integer PRIMARY KEY,
    cohort text NOT NULL,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
```

Load and index it under the same write rules used for `search_targets`.

```sql
SELECT first_stage.query_id,
       middle_targets.target_id AS middle_target_id,
       final_targets.target_id AS final_target_id
FROM search_queries AS first_stage
JOIN search_targets AS middle_targets
  ON public.mmsearch(
      middle_targets.sequence,
      first_stage.sequence
  )
JOIN second_search_targets AS final_targets
  ON public.mmsearch(
      final_targets.sequence,
      middle_targets.sequence
  )
ORDER BY first_stage.query_id,
         middle_target_id,
         final_target_id;
```

The first search owns `middle_targets.sequence` as its target. The second owns
`final_targets.sequence` as a different target. The middle sequence becomes the
query for the second stage.

Explain the chain before using it on large relations:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT first_stage.query_id,
       middle_targets.target_id,
       final_targets.target_id
FROM search_queries AS first_stage
JOIN search_targets AS middle_targets
  ON public.mmsearch(
      middle_targets.sequence,
      first_stage.sequence
  )
JOIN second_search_targets AS final_targets
  ON public.mmsearch(
      final_targets.sequence,
      middle_targets.sequence
  );
```

Read each custom join node as its own search. Check its target column,
parameterized query input, batch counters, pipeline, and actual backend. Do not
collapse the two nodes into one performance number.

## Keep projected match state unambiguous

When two searches share a lineage, a helper such as `bioseq.cigar(t.pk)` may
have more than one possible owner. Bioseq rejects that ambiguity instead of
returning a CIGAR from an arbitrary search.

For multi-stage statements, project alignment details inside the stage where
the owner is unique. A lateral subquery is often the clearest boundary:

```sql
SELECT q.query_id,
       first_match.target_id,
       first_match.target_start,
       first_match.target_end
FROM search_queries AS q
CROSS JOIN LATERAL (
    SELECT t.target_id,
           bioseq.target_start(t.target_id) AS target_start,
           bioseq.target_end(t.target_id) AS target_end
    FROM search_targets AS t
    WHERE public.mmsearch(t.sequence, q.sequence)
) AS first_match
ORDER BY q.query_id, first_match.target_id, first_match.target_start;
```

The helper carrier belongs to `t`, and the subquery has one `mmsearch` owner.

## Do not stack two searches on one target

This shape is unsupported:

```sql
SELECT pk
FROM paired_reads
WHERE public.mmsearch(
        seq0,
        'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
      )
  AND public.mmsearch(
        seq0,
        'TCATCATCTACTCCTA'::bioseq.seq('dna4', 32)
      );
```

Both predicates ask one target frontier to own different match state. Bioseq
fails with SQLSTATE `0A000` and a `stacked mmsearch` message.

If the application needs targets that match two probes, run two searches with
a clear relational boundary and intersect their keys. One simple approach is
two materialized result tables:

```sql
CREATE TEMP TABLE first_matches AS
SELECT DISTINCT pk
FROM paired_reads
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
);
```

```sql
CREATE TEMP TABLE second_matches AS
SELECT DISTINCT pk
FROM paired_reads
WHERE public.mmsearch(
    seq0,
    'TCATCATCTACTCCTA'::bioseq.seq('dna4', 32)
);
```

```sql
SELECT a.pk
FROM first_matches AS a
JOIN second_matches AS b USING (pk)
ORDER BY a.pk;
```

Each statement has one search owner. The final join is ordinary relational
work. This also lets the application decide whether it needs alignment details
from the first search, the second search, or both.

## Use slices in a join deliberately

A target expression can be a slice when the query should search only a defined
region:

```sql
SELECT targets.pk AS target_pk,
       probes.pk AS probe_pk
FROM paired_reads AS targets
JOIN paired_reads AS probes
  ON public.mmsearch(targets.seq0[16:-16], probes.seq1)
ORDER BY target_pk, probe_pk;
```

Coordinates then describe the sliced target expression. Keep the slice visible
in the SQL and in any result metadata so callers do not confuse slice-relative
positions with full-record positions. Add the slice offset when converting a
relative target coordinate back to the stored sequence coordinate system.

For the `[16:-16]` example:

```sql
WITH relative_matches AS MATERIALIZED (
    SELECT targets.pk AS target_pk,
           probes.pk AS probe_pk,
           16 AS slice_origin,
           bioseq.target_start(targets.pk) AS relative_target_start,
           bioseq.target_end(targets.pk) AS relative_target_end
    FROM paired_reads AS targets
    CROSS JOIN paired_reads AS probes
    WHERE public.mmsearch(targets.seq0[16:-16], probes.seq1)
)
SELECT target_pk,
       probe_pk,
       slice_origin + relative_target_start AS stored_target_start,
       slice_origin + relative_target_end AS stored_target_end
FROM relative_matches
ORDER BY target_pk, probe_pk, stored_target_start;
```

Project the match helpers as plain columns in the search level. Perform
coordinate arithmetic in the outer level. This gives the custom node clear
placeholder sites and keeps coordinate conversion ordinary SQL.

Only apply a fixed offset when the lower slice bound is fixed and nonnegative.
For negative or row-dependent bounds, compute the resolved origin explicitly.

## A runnable self-join example

The following block uses the five-row fixture from this tutorial and returns
stored target and probe pairs:

```sql runnable id=multiple_sequence_self_join timeout=40s entity="sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.vectorization.enabled = on;
CREATE TABLE tutorial_paired_reads (
    pk integer PRIMARY KEY,
    seq0 bioseq.seq('dna4', 32) NOT NULL,
    seq1 bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
INSERT INTO tutorial_paired_reads VALUES
    (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA', 'CTAGTCACTGTAGCCTGATCGTAGCTAGT'),
    (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA', 'TCGTAGCTAGGA'),
    (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA', 'TCATCATCTACTCCTA'),
    (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT', 'TGTAGCCTGATCGTA'),
    (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA', 'CACTGTAGCCTGATCGTA');
-- bioseq:visible
SELECT targets.pk AS target_pk,
       probes.pk AS probe_pk,
       bioseq.target_start(targets.pk) AS target_start,
       bioseq.target_end(targets.pk) AS target_end
FROM tutorial_paired_reads AS targets
CROSS JOIN tutorial_paired_reads AS probes
WHERE public.mmsearch(targets.seq0, probes.seq1)
ORDER BY target_pk, probe_pk, target_start;
-- bioseq:teardown
DROP TABLE tutorial_paired_reads;
```

## Plan and capacity checks

Sequence joins can multiply work quickly. Before moving from a literal search
to stored probes, answer these questions:

1. How many query rows survive ordinary metadata filters
2. How many target rows survive target-side filters
3. Whether `BioseqBatchJoin` was selected
4. What `Bioseq Query Batch Capacity` the plan reports
5. How many `Bioseq Query Batches` and `Bioseq Distinct Targets` execution saw
6. Which index source and execution backend each custom node actually used
7. Whether the result needs every alignment or only distinct key pairs
8. Whether pair filters run after a materialized match boundary or on staged
   Bioseq input relations

Use `SELECT DISTINCT` only when duplicate alignments are not meaningful to the
caller. It changes result semantics and may add a sort or hash step.

Use limits carefully. A global `LIMIT` applies after join order and matching.
If the requirement is a fixed number of targets per probe, put the boundary in
a lateral subquery and verify the resulting plan and ordering.

## Clean up the examples

```sql
DROP TABLE IF EXISTS search_queries;
```

```sql
DROP TABLE IF EXISTS search_targets;
```

```sql
DROP TABLE IF EXISTS second_search_targets;
```

```sql
DROP TABLE IF EXISTS paired_reads;
```
