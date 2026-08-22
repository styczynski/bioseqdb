---
title: Columnar storage and vectorized aggregates
slug: columnar_storage_and_vectorized_aggregates
description: Model scalar and sequence columns together, inspect Bioseq storage, and select the exact supported vector aggregate route.
order: 20
keywords: [columnar storage, VectorAggNode, aggregates, compression, chunk groups, EXPLAIN]
draft: false
---

A `USING bioseq` table stores ordinary PostgreSQL values and biological
sequences in a column-oriented layout. A query can read only the columns it
needs, skip chunk groups using scalar metadata, and process an eligible
aggregate as batches. These are separate decisions. Columnar storage does not
make every expression or aggregate vectorized.

This tutorial shows the boundary from SQL:

1. Design a mixed scalar and sequence table
2. Choose table storage settings before creating it
3. Inspect sequence shard relations without reading private catalogs
4. Run the exact aggregate shape that selects `VectorAggNode`
5. Recognize plan shapes that stay on PostgreSQL's ordinary aggregate path

Prerequisite: [The bioseq.seq column type](../sequence_column_type/) explains
alphabets and packed widths used by the declarations on this page.

## Understand the visible storage boundaries

PostgreSQL still presents rows. `INSERT`, `SELECT`, transactions, constraints,
and joins use normal SQL. The `bioseq` table access method changes the physical
organization behind those rows.

Scalar values are collected into stripes and chunk groups. Each column has its
own presence and value streams. Bioseq records scalar min/max information for
eligible filters and compresses value streams independently. A scan can
therefore project `sample_id` and `read_count` without also converting a large
sequence to text.

Sequence payloads use sidecar shard relations. The parent row retains the
logical sequence value and its source identity. [[sql:bioseq.len]] reports the
number of biological symbols. `bioseq.unit_len` reports packed storage
units.
Neither number is a row count or a byte count.

Multiple sequence columns have separate column numbers and shard families.
They can use different bound alphabets or requested widths, but a search still
requires compatible target and query alphabets.

## Choose storage settings before table creation

The main user-facing settings are:

| Setting | Meaning | Current default | Scope |
| --- | --- | --- | --- |
| `bioseq.storage.stripe.row_limit` | Maximum rows collected in a stripe | `150000` | Captured as a table option |
| `bioseq.storage.chunk_group.row_limit` | Maximum rows in one chunk group | `10000` | Captured as a table option |
| `bioseq.storage.compression.algorithm` | Compression for new column chunks | `zstd`, else `lz4`, else `pglz`, as compiled | Captured as a table option |
| `bioseq.storage.compression.level` | Compression level | `3` | Captured as a table option |
| `bioseq.storage.sequence_shard.max_size_gib` | Sequence shard rollover size | `1024` | Superuser setting used for later shard allocation |

`none` and `pglz` are always recognized compression choices. `lz4` and `zstd`
exist only when the extension was built with those libraries. Read the active
value with `SHOW` before selecting one.

Set storage options before `CREATE TABLE` when you want a table to capture a
non-default choice:

```sql
SET bioseq.storage.compression.algorithm = 'pglz';
SET bioseq.storage.compression.level = 3;

CREATE TABLE run_measurements (
    read_id bigint PRIMARY KEY,
    sample_id text NOT NULL,
    read_count integer NOT NULL,
    collected_on date NOT NULL,
    forward_read bioseq.seq('dna4', 32) NOT NULL,
    reverse_read bioseq.seq('dna4', 32)
) USING bioseq;

RESET bioseq.storage.compression.level;
RESET bioseq.storage.compression.algorithm;
```

The stripe and chunk-group settings accept values from 1,000 to 100,000,000.
Smaller groups create more metadata and more opportunities to skip work.
Larger groups reduce metadata overhead but make each admitted group broader.
Choose them for the table's filter and ingest pattern, not for one isolated
query.

Compression is a storage choice, not a promise that every chunk becomes
smaller. Bioseq can retain an uncompressed representation when compression is
not useful for that chunk.

## Change stored table options deliberately

The table owner can change the four captured relation options with
`pg_catalog.alter_bioseq_table_set`:

```sql
SELECT pg_catalog.alter_bioseq_table_set(
    'run_measurements'::regclass,
    stripe_row_limit => 20000,
    chunk_group_row_limit => 2000,
    compression => 'pglz',
    compression_level => 3
);
```

The function takes an `ACCESS EXCLUSIVE` lock and leaves omitted options
unchanged. The new values govern later column writes. They do not recompress or
regroup chunks that the table already contains. Build a replacement relation
and reload it when every chunk must use one new physical policy.

`pg_catalog.alter_bioseq_table_reset` copies the current GUC value for
each selected option back into the table metadata:

```sql
SELECT pg_catalog.alter_bioseq_table_reset(
    'run_measurements'::regclass,
    stripe_row_limit => true,
    chunk_group_row_limit => true,
    compression => true,
    compression_level => true
);
```

Set the intended session defaults before calling the reset function. A reset
does not mean that older chunks are rewritten.

## Project only what the query needs

Start with a scalar projection and filter:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT sample_id, read_count
FROM run_measurements
WHERE read_count >= 1000;
```

For an owned columnar scan, read these properties:

- `Bioseq Projected Columns` lists values supplied by the scan
- `Bioseq Chunk Group Filters` lists scalar predicates admitted for pruning
- `Bioseq Chunk Groups Removed by Filter` reports actual pruning with
  `ANALYZE`
- `Bioseq Vectorized Filter` names a filter evaluated over batches

A predicate can be useful for chunk pruning without being a vectorized
expression. A sequence conversion such as `forward_read::text` also forces
payload work that a metadata-only projection avoids.

Use ordinary `ANALYZE run_measurements` after a representative load. PostgreSQL
statistics still guide scalar selectivity and join choices. Bioseq does not
replace `ANALYZE` with a separate user-facing statistics command.

## Cache decompressed columns within one scan

`bioseq.column_cache.enabled` controls a backend-local cache of decompressed
column chunks. It is off by default. When enabled, one Bioseq scan can reuse a
chunk that it reads again through the same relation, stripe, chunk group, and
column. The cache is cleared when that scan ends, so it is not a result cache
across SQL statements.

`bioseq.column_cache.capacity_mb` defaults to 200 MiB and accepts values from
20 through 20,000 MiB. Set the capacity before the statement that first uses
the cache in that scan:

```sql
SET bioseq.column_cache.capacity_mb = 200;
SET bioseq.column_cache.enabled = on;

EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT forward_read[0:64]::text
FROM run_measurements
WHERE sample_id = 'run-042';

RESET bioseq.column_cache.enabled;
RESET bioseq.column_cache.capacity_mb;
```

With the cache enabled, the `BioseqScan` section reports `Cache Hits`, `Cache
Misses`, `Cache Evictions`, `Cache Writes`, peak and ending bytes, and the
ending entry count. Read those counters from the analyzed statement. A later
statement starts a new scan and cannot claim reuse from this one.

This is separate from the optional `bioseq_cache` extension. The column cache
holds decompressed column data during a scan. The query-cache tutorial covers
shared completed results, concurrent producers, TTL, and relation invalidation.

## Use the supported vector aggregate registry

The current vector aggregate registry is deliberately small:

| Aggregate | Supported input types |
| --- | --- |
| `count(*)` | no input expression |
| `count(column)` | a direct column value |
| `sum`, `avg`, `min`, `max` | `smallint`, `integer`, `bigint` |
| `min`, `max` | `date` |

The eligible plan is a plain aggregate directly above one `BioseqScan`.
`bioseq.vectorization.enabled` must be on. Grouped aggregates, joins,
`DISTINCT`, aggregate `FILTER` clauses, slice projections, and aggregates over
an owned `mmsearch` result stay on a safe ordinary path.

The following fixture is nonempty and uses no NULL aggregate inputs. It makes
the current vector route easy to identify without relying on its known
empty/all-NULL edge behavior.

```sql runnable id=columnar_vector_aggregate timeout=20s entity="sql:type:bioseq.seq" entity="sql:function:bioseq.len(bioseq.seq)"
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.vectorization.enabled = on;
CREATE TABLE tutorial_columnar_runs (
    read_id integer PRIMARY KEY,
    sample_id text NOT NULL,
    read_count integer NOT NULL,
    bases bigint NOT NULL,
    collected_on date NOT NULL,
    seq0 bioseq.seq('dna4', 32) NOT NULL,
    seq1 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO tutorial_columnar_runs VALUES
    (1, 'alpha', 12, 32, DATE '2026-01-02', 'ACGTACGTACGTACGTACGTACGTACGTACGT', 'TGCATGCATGCATGCA'),
    (2, 'alpha', 20, 32, DATE '2026-01-03', 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTA', 'ACGTACGTACGTACGT'),
    (3, 'beta',  28, 32, DATE '2026-01-04', 'GGGGCCCCTTTTAAAAGGGGCCCCTTTTAAAA', NULL),
    (4, 'beta',  36, 32, DATE '2026-01-05', 'CTAGTCACTGTAGCCTGATCGTAGCTAGTCAA', 'AAAACCCCGGGGTTTT');
ANALYZE tutorial_columnar_runs;
-- bioseq:visible
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT count(*) AS rows,
       sum(read_count) AS reads,
       avg(bases) AS average_bases,
       min(collected_on) AS first_day,
       max(collected_on) AS last_day
FROM tutorial_columnar_runs;

SELECT count(*) AS rows,
       sum(read_count) AS reads,
       avg(bases) AS average_bases,
       min(collected_on) AS first_day,
       max(collected_on) AS last_day
FROM tutorial_columnar_runs;

SELECT column_attno,
       bucket_idx,
       shard_idx,
       bytes_written,
       is_full
FROM bioseq.list_seq_storage_shards(
    'tutorial_columnar_runs'::regclass)
ORDER BY column_attno, bucket_idx, shard_idx;
-- bioseq:teardown
DROP TABLE tutorial_columnar_runs;
RESET bioseq.vectorization.enabled;
RESET max_parallel_workers_per_gather;
```

The first result must contain `Custom Scan (VectorAggNode)` with a
`BioseqScan` child. The second result is the aggregate value returned to the
application. The third result lists the public shard view for both sequence
columns. A NULL `seq1` does not remove the parent row and does not make the
`seq0` payload part of the aggregate.

## Know why a query stays scalar

This grouped query is useful SQL, but it is not an eligible plain aggregate:

```sql
SELECT sample_id, sum(read_count)
FROM tutorial_columnar_runs
GROUP BY sample_id
ORDER BY sample_id;
```

This aggregate also stays ordinary because the slice owner publishes scalar
PostgreSQL values:

```sql
SELECT max(bioseq.len(seq0[0:16]))
FROM tutorial_columnar_runs;
```

Likewise, do not expect a generic user-defined aggregate, a numeric expression
that is absent from the generated registry, or `count(DISTINCT sample_id)` to
select `VectorAggNode`. The fallback is the normal PostgreSQL aggregate. It is
not an error.

## Treat NULL and empty inputs as a current limitation

The current vector aggregate implementation does not yet match PostgreSQL's
empty and all-NULL results for every registered integer and date aggregate.
For example, some integer sums can expose their zero transition state and some
min/max functions can expose their initial extrema.

Do not enable this route for a query whose correctness depends on those edge
cases. Either keep `bioseq.vectorization.enabled` off for that statement or
write a query shape that stays on PostgreSQL's aggregate path. The runnable
example uses non-NULL inputs so it demonstrates the supported route without
presenting the edge behavior as a contract.

## Inspect storage without private catalogs

Use `bioseq.list_seq_storage_shards` for shard and sidecar relation
identity, column number, bucket, shard number, bytes written, and rollover
state. Use PostgreSQL's `pg_relation_size` on a returned relation when an
operator needs byte totals.

Do not query private storage catalogs in application monitoring. Their schemas
can change independently of the public tutorial surface.

For index sizes and update ages, use the index metadata interface covered in
the index-lifecycle tutorial. Storage shards describe sequence payload. Index
metadata describes persisted search aids. They answer different questions.

## Maintain the parent and sequence shards

Ordinary `VACUUM` maintains the Bioseq parent relation. Sequence payload also
has shard and sidecar relations. Use the public
`bioseq.vacuum_seq_storage` procedure when those relations need their own
`VACUUM ANALYZE` pass:

```sql
VACUUM (ANALYZE) run_measurements;
CALL bioseq.vacuum_seq_storage('run_measurements'::regclass);
```

Call the procedure as a top-level statement. It is deliberately non-atomic and
cannot run inside an explicit `BEGIN` block. It may commit between shard pairs,
so an error can leave earlier pairs maintained. Correct the error and call it
again. A table with no allocated sequence shards is a no-op.

This procedure does not rebuild minimizer or symbol-count indexes. Use the
index maintenance API in the next lifecycle tutorial for those persisted
forms.

## Operational sequence

For a new columnar table:

1. Bind each sequence column to the intended alphabet and width
2. Set non-default compression, stripe, or chunk-group values before creation
3. Load representative rows and run `ANALYZE`
4. Explain the real projection, filter, and aggregate shapes
5. Verify `VectorAggNode` only for queries covered by the exact registry
6. Inspect public shard metadata and relation sizes when sizing storage
7. Schedule parent and sequence-shard maintenance for write-heavy tables
8. Keep an ordinary-plan comparison for NULL-sensitive aggregate workloads

Next: use [Generate deterministic sequence tables](../sequence_generation/) to
create small inputs for storage, search, and dispatch experiments.
