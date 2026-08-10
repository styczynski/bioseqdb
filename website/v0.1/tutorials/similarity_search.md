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
custom scan. The example on this page uses the canonical five-row fixture and
probe exercised by Bioseq's integration and SQL tests.

> [!IMPORTANT]
> The hidden setup runs `EXPLAIN (FORMAT JSON)` for the exact visible predicate
> and aborts publication unless PostgreSQL selects `Custom Scan` with the
> `BioseqScan` provider. The result table below is captured from that verified
> execution; it is not authored as expected output.

## Search the canonical fixture

The query returns each matching primary key together with query and target
coordinates and the alignment's CIGAR string. Those projections come from
[[sql:bioseq.query_start]], [[sql:bioseq.query_end]],
[[sql:bioseq.target_start]], [[sql:bioseq.target_end]], and
[[sql:bioseq.cigar]].

```sql runnable id=canonical_mmsearch timeout=15s entity="sql:function:public.mmsearch(bioseq.seq,bioseq.seq)" entity="sql:function:bioseq.query_start(anyelement)" entity="sql:function:bioseq.query_end(anyelement)" entity="sql:function:bioseq.target_start(anyelement)" entity="sql:function:bioseq.target_end(anyelement)" entity="sql:function:bioseq.cigar(anyelement)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.vectorization.enabled = on;
CREATE TABLE tutorial_mmsearch (
  pk integer PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO tutorial_mmsearch (pk, seq0) VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA'),
  (4, 'TTTCATCATCATCATCTACTCCTATCTATCATCATCATCATCTACTCCTATCTACTCTCATCATCCTGATCGTAGCTATACTCCTACTATCCTATT'),
  (5, 'ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGTGTACTCTAGTCACTGTAGCCTGATCGTAGCTAGTTGATCCCCATATCTTCACTAGTCACTGTAGCCTGATCGTAGCTAGTTTTTTTA');
DO $$
DECLARE
  plan_json jsonb;
BEGIN
  EXECUTE 'EXPLAIN (FORMAT JSON) '
          'SELECT pk FROM tutorial_mmsearch '
          'WHERE public.mmsearch(seq0, '
          '''CTAGTCACTGTAGCCTGATCGTAGCTAGT''::bioseq.seq(''dna4'', 32))'
    INTO plan_json;
  IF plan_json #>> '{0,Plan,"Node Type"}' IS DISTINCT FROM 'Custom Scan'
     OR plan_json #>> '{0,Plan,"Custom Plan Provider"}' IS DISTINCT FROM 'BioseqScan' THEN
    RAISE EXCEPTION
      'canonical_mmsearch: expected BioseqScan Custom Scan, got %', plan_json;
  END IF;
END $$;
-- bioseq:visible
SELECT pk,
       COUNT(*) OVER () AS match_count,
       bioseq.query_start(pk) AS query_start,
       bioseq.query_end(pk) AS query_end,
       bioseq.target_start(pk) AS target_start,
       bioseq.target_end(pk) AS target_end,
       bioseq.cigar(pk) AS cigar
FROM tutorial_mmsearch
WHERE public.mmsearch(
        seq0,
        'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
      )
ORDER BY pk;
-- bioseq:teardown
DROP TABLE tutorial_mmsearch;
```

The setup data is copied from `hoe_seed_rows()` and the probe is the exact
`kHoeProbeSequence` used by the integration suite. The published rows and
coordinates remain whatever the installed extension actually returns.
