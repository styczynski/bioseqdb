---
title: Caching sequence-search results
slug: query_cache
description: Reuse exact Bioseq searches, inspect cache health, and invalidate safely.
order: 30
keywords: [query cache, reusable fragments, statistics, invalidation, BioseqScan]
draft: false
---

The query cache is disabled by default. To use it, add `bioseq_cache` before
`bioseq` in `shared_preload_libraries`, restart PostgreSQL, and install both
extensions:

```sql
CREATE EXTENSION bioseq_cache;
CREATE EXTENSION bioseq;
```

Setting a cache mode before the preload and restart fails instead of running
without a cache.

Start with [[sql:bioseq_cache.version|`bioseq_cache.version()`]], then use
[[sql:bioseq_cache.stats|`stats()`]] and
[[sql:bioseq_cache.entries|`entries()`]] to observe decisions. These
diagnostics require membership in PostgreSQL's `pg_read_all_stats` role.

## Exact-query hits

`exact_query` stores the final result for one complete statement. The hidden
setup below runs the same `BioseqScan` twice. The visible query shows the
counters after the first run and the repeated run.

```sql runnable id=cache_exact_query timeout=30s entity="sql:function:bioseq_cache.stats()"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq_cache.query_cache.mode = 'exact_query';
CREATE TABLE cache_exact_reads (
  pk integer PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO cache_exact_reads VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA');
SELECT count(*) FROM cache_exact_reads
WHERE public.mmsearch(
  seq0,
  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));
SELECT count(*) FROM cache_exact_reads
WHERE public.mmsearch(
  seq0,
  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));
-- bioseq:visible
SELECT produced, completed_hits, reuse_hits, fallbacks, invalidations
FROM bioseq_cache.stats();
-- bioseq:teardown
DROP TABLE cache_exact_reads;
```

`produced` increases for the cold execution and `completed_hits` for a completed
exact result. A nonzero `fallbacks` count is a signal to inspect the plan and
provider readiness before increasing cache capacity.

## Reusable-fragment mode

`reusable_fragments` can reuse cached work across related searches. It can also
reuse a complete result when the statements match. The supported index recipe
names are `minimizers_per_sequence` and `minimizers_per_fragment`. Use these
names when you create or update an index.

```sql runnable id=cache_reusable_fragments timeout=30s entity="sql:function:bioseq_cache.stats()" entity="sql:function:bioseq_cache.entries()"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq_cache.query_cache.mode = 'reusable_fragments';
CREATE TABLE cache_reusable_reads (
  pk integer PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO cache_reusable_reads VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA');
SELECT count(*) FROM cache_reusable_reads
WHERE public.mmsearch(
  seq0,
  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));
SELECT count(*) FROM cache_reusable_reads
WHERE public.mmsearch(
  seq0,
  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32));
-- bioseq:visible
SELECT s.produced, s.completed_hits, s.reuse_hits, s.fallbacks,
       (SELECT count(*) FROM bioseq_cache.entries()
        WHERE state = 'COMPLETE') AS complete_entries
FROM bioseq_cache.stats() AS s;
-- bioseq:teardown
DROP TABLE cache_reusable_reads;
```

Compare `logical_bytes` and `stored_bytes` in `stats()` when sizing shared
memory. Reuse is most useful for repeated, selective searches. Leave the cache
disabled for one-off scans that would store large results.

Use `EXPLAIN (ANALYZE, VERBOSE)` to inspect `Cache Mode` and
`Actual Cache Action`. These fields show whether the query produced, reused,
or bypassed a cache entry.

## Invalidation and privilege errors

Table writes retire dependent generations automatically. A superuser can also
invalidate one relation with
[[sql:bioseq_cache.invalidate_relation|`invalidate_relation()`]] or
all generations with
[[sql:bioseq_cache.invalidate_all|`invalidate_all()`]], and can clear counters
with [[sql:bioseq_cache.reset_stats|`reset_stats()`]]:

```sql
SELECT bioseq_cache.invalidate_relation('reads'::regclass);
SELECT bioseq_cache.invalidate_all();
SELECT bioseq_cache.reset_stats();
```

The website examples run as isolated, non-superuser roles. This example shows
that an unauthorized invalidation fails with `insufficient_privilege`.

```sql runnable id=cache_invalidation_denied timeout=15s expect=error sqlstate=42501 entity="sql:function:bioseq_cache.invalidate_relation(regclass)"
-- bioseq:setup
CREATE TABLE cache_invalidation_target (id integer PRIMARY KEY);
-- bioseq:visible
SELECT bioseq_cache.invalidate_relation(
  'cache_invalidation_target'::regclass);
-- bioseq:teardown
DROP TABLE cache_invalidation_target;
```

Repeated fallbacks mean the cache could not serve the query. Frequent
invalidations can mean that table writes are removing entries before they are
reused. If stored bytes approach the configured limit, first narrow the query
and check its cache action in `EXPLAIN`. Increase cache capacity only after
those checks.
