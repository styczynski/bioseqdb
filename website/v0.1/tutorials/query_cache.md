---
title: Caching sequence-search results
slug: query_cache
description: Reuse exact Bioseq searches, inspect cache health, and invalidate safely.
order: 60
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

Put the libraries in this order in `postgresql.conf`:

```text
shared_preload_libraries = 'bioseq_cache,bioseq'
```

Restart the server after changing that setting. A reload is not enough because
the cache allocates shared state during server startup.

The mode is a session setting:

```sql
SHOW bioseq_cache.query_cache.mode;
```

The available values are:

| Mode | Behavior |
| --- | --- |
| `disabled` | Execute the retained PostgreSQL and provider plan without query-result caching |
| `exact_query` | Store and replay the final result for one exact statement identity |
| `reusable_fragments` | Reuse compatible coverage branches and compute any residual work |

`disabled` is the default. Start with it during application validation, then
enable one cache mode for a measured repeated workload.

Completed exact results use this TTL setting:

```sql
SHOW bioseq_cache.query_cache.completed_result_ttl_ms;
```

The value is in milliseconds. It defaults to 60 minutes. A longer TTL can keep
more completed data live. It does not override dependency invalidation after a
write.

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

## Understand the automatic producer lock

When two sessions run the same cold exact query, the cache chooses one producer.
The other session waits for that in-flight entry instead of producing the same
final result again.

There is no SQL lock or unlock function for this. Coordination is automatic and
scoped to the cache entry.

Use three `psql` sessions to observe it on a query that runs long enough to
overlap.

In session A, enable exact mode and start the cold query:

```sql
SET bioseq_cache.query_cache.mode = 'exact_query';
```

```sql
SELECT count(*)
FROM cache_exact_reads
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
);
```

While session A is running, send the same statement from session B. Statement
text, parameters, role context, and dependencies all contribute to whether the
entry is the same. For a locking check, do not change whitespace, casts,
options, or session role between the two statements.

From session C, inspect entries:

```sql
SELECT entry_key,
       generation,
       kind,
       state,
       producer_pid,
       waiters,
       readers,
       age,
       ttl,
       sql
FROM bioseq_cache.entries()
WHERE state = 'BUILDING'
ORDER BY created_at;
```

During the overlap, one entry can show one producer and one or more waiters.
Producer SQL and PID visibility depends on diagnostic permissions.

After both statements finish, read the counters:

```sql
SELECT produced,
       completed_hits,
       inflight_waits,
       fallbacks
FROM bioseq_cache.stats();
```

For one cold producer and one waiter that consumes its completed result, expect
one production, one completed hit, and one in-flight wait to be added.

Canceling a waiting statement does not cancel the producer. The producer can
finish for other waiters and later consumers. Canceling the producer follows a
different failure path, and consumers must not receive a partially stored
result.

## Inspect exact-query decisions in EXPLAIN

Use plain EXPLAIN to see the planned cache wrapper:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT count(*)
FROM cache_exact_reads
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
);
```

An exact `BioseqCacheScan` reports:

- `Cache Mode`
- `Planned Cache Action`
- `Cache Force Bypass`
- `Cache Bypass Reason` when forced
- `Provider Runtime Recipes`
- `Cache Parameter Count`
- `Cache Dependency Count`
- `Cache Dependencies`
- `SQL`
- `Cache Entry Kind`
- `Cache Readiness`
- `Cache Storage`
- `Cache Storage Ownership`
- `Cache Janitor`

`Cache Entry Kind` is `ExactFinalResult` for this mode.

Run `EXPLAIN ANALYZE` when executing the query is acceptable:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT count(*)
FROM cache_exact_reads
WHERE public.mmsearch(
    seq0,
    'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32)
);
```

It adds `Provider Runtime Recipe State` and `Actual Cache Action`. The actual
action is one of these exact-query outcomes:

- `[Hit] exact final result`
- `[Produce] exact final result`
- `[Bypass] retained child`

Read the actual action before using cache counters to explain one statement.
The global counters include other sessions and queries.

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

## Read partial reuse and residual recompute

Reusable mode can combine completed cache branches with fresh work. This is the
partial-recompute path. It does not return a stale partial result as if it were
complete.

Start with a related query family, for example searches over the same relation
and sequence predicate with different metadata coverage:

The `$1` values below are application parameters supplied through a prepared
statement or PostgreSQL's extended query protocol.

```sql
SET bioseq_cache.query_cache.mode = 'reusable_fragments';
```

```sql
SELECT pk
FROM cache_reusable_reads
WHERE pk IN (1, 2)
  AND public.mmsearch(
      seq0,
      $1::bioseq.seq('dna4', 32)
  );
```

Then run a related statement whose requested coverage overlaps but is not
identical:

```sql
SELECT pk
FROM cache_reusable_reads
WHERE pk IN (1, 3)
  AND public.mmsearch(
      seq0,
      $1::bioseq.seq('dna4', 32)
  );
```

Whether branches can be reused depends on the provider recipe, parameters,
security namespace, dependency generation, and predicate coverage. Do not
assume overlap from SQL text alone.

Inspect the second statement with analyzed structured output:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, FORMAT JSON)
SELECT pk
FROM cache_reusable_reads
WHERE pk IN (1, 3)
  AND public.mmsearch(
      seq0,
      $1::bioseq.seq('dna4', 32)
  );
```

The reusable node reports:

- `Cache Entry Kind: ReusableCoverage`
- `Cache Mode: reusable_fragments`
- `Cache Force Bypass`
- `Planned Cache Action`
- `Actual Cache Action`
- `Cache Entry`
- `SQL`
- `Reusable Sources`
- `Selected Cache Branches`
- `Provider Runtime Recipe Valid`

The actual action distinguishes four outcomes:

- `[Read] Read current reusable generation`
- `[Reuse] Reused selected branches and produced residual`
- `[Produce] Produce current reusable generation`
- `[Bypass] Execute retained provider child`

The second action is the partial-recompute case. Cached branches supply part of
the result and the residual child computes the missing coverage.

Each `Cache Branch` section reports an `Action` and can report a `Recheck`.
The `Residual Child` section reports its `Predicate`. Read all branch rechecks
and the residual predicate before deciding which portion was reused.

The optional `Provider` section contains provider-specific coverage detail. Its
fields are supplied by that provider rather than by the query-cache core.

## Inspect the current reusable generation

When a reusable generation is available, EXPLAIN can show:

- `State`
- `Producer PID` when the viewer has permission
- `Age`
- `Effective TTL`
- `Rows`
- `Logical Bytes`
- `Stored Bytes`
- `Dependencies`

Compare age with effective TTL. Compare logical and stored bytes to see the
storage reduction for that generation. A `planned/provisional` state means no
current snapshot was available at the point described by the plan.

Query the entry table for an operational view:

```sql
SELECT entry_key,
       generation,
       kind,
       state,
       age,
       ttl,
       rows,
       logical_bytes,
       stored_bytes,
       waiters,
       readers,
       hits,
       dependencies
FROM bioseq_cache.entries()
ORDER BY created_at DESC;
```

`logical_bytes` is the uncompressed logical result size tracked by the cache.
`stored_bytes` is the stored payload size. Neither is PostgreSQL table size.

## Failure during partial production

Reusable production stages rows before publishing a completed generation. If
the producer fails, the cache discards staged partial rows. It does not publish
them as a complete reusable result.

When a reusable route cannot be used safely, the executor can run the retained
child plan and increase `fallbacks`. The query still has to return the child
plan's correct result or raise the original query error.

After an unexpected failure, check:

```sql
SELECT produced,
       reuse_hits,
       fallbacks,
       invalidations,
       evictions
FROM bioseq_cache.stats();
```

Then inspect incomplete or failed entries:

```sql
SELECT entry_key,
       generation,
       kind,
       state,
       producer_pid,
       age,
       waiters,
       readers,
       sql
FROM bioseq_cache.entries()
WHERE state <> 'COMPLETE'
ORDER BY created_at DESC;
```

Do not delete shared files or manipulate cache state outside the SQL interface.
Use the invalidation functions when an administrator needs an explicit reset.

## Let writes invalidate dependent generations

Successful writes retire cache generations that depend on the changed
relation. This happens automatically at transaction boundaries.

For example:

```sql
BEGIN;
```

```sql
SET LOCAL bioseq.index.insert_maintenance.enabled = on;
```

```sql
UPDATE cache_reusable_reads
SET seq0 = 'ACGTACGTACGTACGTACGTACGTACGTACGT'
WHERE pk = 1;
```

```sql
COMMIT;
```

After commit, a cache generation that depended on the relation cannot continue
serving the pre-write result. The next query produces or reuses coverage under
the current dependency generation.

An aborted transaction does not publish its data change as a committed cache
invalidation. Savepoint rollback also discards the matching dirty state.

This dependency behavior is why increasing TTL cannot make a changed relation
serve an old completed result until the timer expires.

## Read all global counters

The statistics row contains:

| Column | Meaning |
| --- | --- |
| `produced` | Completed productions |
| `completed_hits` | Replays of completed exact results |
| `inflight_waits` | Consumers that waited for an active producer |
| `reuse_hits` | Reusable coverage hits |
| `fallbacks` | Executions that used a retained child instead of cache reuse |
| `invalidations` | Invalidations observed by the cache |
| `evictions` | Entries removed by capacity or lifecycle policy |
| `logical_bytes` | Aggregate logical bytes tracked |
| `stored_bytes` | Aggregate stored bytes tracked |

Read the full row:

```sql
SELECT produced,
       completed_hits,
       inflight_waits,
       reuse_hits,
       fallbacks,
       invalidations,
       evictions,
       logical_bytes,
       stored_bytes
FROM bioseq_cache.stats();
```

Take a before and after snapshot around a controlled query when several
applications share the server. A single final counter row cannot attribute work
to one statement.

## Diagnostic permissions

`stats()` and `entries()` require membership in PostgreSQL's
`pg_read_all_stats` role for diagnostic access:

```sql
GRANT pg_read_all_stats TO cache_observer;
```

Grant it deliberately. Entry SQL, producer identity, dependencies, and sizes
can expose operational details that an ordinary application role does not need.

The application executing a cached query does not need permission to call the
diagnostic functions simply to benefit from caching.

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

## A practical cache checklist

Before enabling the cache for a production query family:

1. Confirm the uncached query returns the intended rows
2. Capture its `BioseqScan` or join plan with cache disabled
3. Choose exact mode for identical final results or reusable mode for related
   coverage
4. Run one cold execution and one warm execution
5. Read `Actual Cache Action` for both
6. Compare `produced`, hits, waits, reuse, and fallbacks before and after
7. Inspect entry age, TTL, rows, logical bytes, and stored bytes
8. Run a representative write and confirm the next read does not serve the old
   generation
9. Cancel a waiting test session and confirm the producer remains healthy
10. Keep manual invalidation restricted to an administrative role

Disable caching for one-off result sets that are unlikely to be read before
expiry or invalidation. Cache space is most useful when a real repeated workload
consumes it.
