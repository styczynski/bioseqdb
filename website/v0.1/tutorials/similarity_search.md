---
title: Searching sequences with mmsearch
slug: similarity_search
description: Run an index-backed sequence search and inspect its real alignment coordinates.
order: 20
keywords: [mmsearch, BioseqScan, similarity search, alignment, CIGAR]
draft: false
---

[[sql:public.mmsearch|`mmsearch`]] is a PostgreSQL predicate: put it in a
`WHERE` clause over a `USING bioseq` table and Bioseq plans a sequence-aware
custom scan. Its first argument is the stored target column. Its second argument
is a query value with the same alphabet.

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

> [!IMPORTANT]
> The hidden setup runs `EXPLAIN (FORMAT JSON)` for the exact visible predicate
> and aborts publication unless PostgreSQL selects `Custom Scan` with the
> `BioseqScan` provider. The result table below is captured from that verified
> execution. It is not authored as expected output.

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

## Troubleshooting

Use `EXPLAIN (FORMAT JSON)` when a search does not select `BioseqScan`. Check
that the table uses `USING bioseq`, the query alphabet matches the target
column, index maintenance or `update_index` has run, and `mmsearch` is a
top-level predicate in the `WHERE` clause.
