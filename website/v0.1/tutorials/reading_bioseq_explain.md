---
title: Read every Bioseq EXPLAIN section
slug: reading_bioseq_explain
description: Interpret Bioseq scan, pipeline, SIMD, GPU, slice, join, cache, sort, index, and external-provider plan sections.
order: 90
keywords: [EXPLAIN, BioseqScan, pipeline, cache, slice, join, GPU, external provider]
draft: false
---

Bioseq adds custom nodes and properties to PostgreSQL `EXPLAIN`. The plan tells
you which sequence path PostgreSQL selected. `EXPLAIN ANALYZE` adds what the
executor actually did.

This page covers every production Bioseq section currently emitted by these
nodes:

- `BioseqScan`
- `BioseqIndexScan`
- `BioseqSliceProject`
- `BioseqBatchJoin` and its per-row recipe form
- `BioseqBatchSortScan`
- `BioseqCacheScan` for exact and reusable cache entries
- `BioseqExternalScan`

It also explains the nested pipeline, Highway, execution, prefilter, column
cache, and materialization sections shared by several nodes.

## Begin with the right EXPLAIN form

Use plain text for a quick human read:

```sql
EXPLAIN (VERBOSE, COSTS OFF)
SELECT pk
FROM reads
WHERE public.mmsearch(
    sequence,
    $1::bioseq.seq('dna4', 32)
);
```

Use JSON when a tool needs stable nesting:

```sql
EXPLAIN (VERBOSE, COSTS OFF, FORMAT JSON)
SELECT pk
FROM reads
WHERE public.mmsearch(
    sequence,
    $1::bioseq.seq('dna4', 32)
);
```

Use `ANALYZE` when it is safe to execute the statement:

```sql
EXPLAIN (ANALYZE, VERBOSE, BUFFERS, COSTS OFF, FORMAT JSON)
SELECT pk
FROM reads
WHERE public.mmsearch(
    sequence,
    $1::bioseq.seq('dna4', 32)
);
```

`EXPLAIN ANALYZE` runs the query. It can perform expensive alignments, contact
remote providers, wait on cache producers, and execute any other work in the
statement. Plain `EXPLAIN` is the safe first step.

## Separate plan facts from execution facts

Plan facts describe the chosen path and frozen configuration. Examples include
the custom node name, projected columns, planned architecture, pipeline stages,
cache mode, and planned cache action.

Execution facts describe observed work. Examples include actual backend, GPU
dispatch counters, actual cache action, query batches, materialized bytes, and
executed architecture. These need `EXPLAIN ANALYZE`.

Do not treat an enabled setting as an execution fact. A Vulkan switch can be on
while a query runs on the CPU. A cache mode can be active while a statement
bypasses the cache. A custom sort node can execute its CPU comparator. Read the
actual fields.

## Identify the custom node first

In text output, look for a line like:

```text
Custom Scan (BioseqScan)
```

In JSON, check both values:

```text
Node Type: Custom Scan
Custom Plan Provider: BioseqScan
```

The provider name tells you which section of this guide applies.

| Provider | Main job |
| --- | --- |
| `BioseqScan` | Scan a local `USING bioseq` relation and optionally run `mmsearch` |
| `BioseqIndexScan` | Wrap a PostgreSQL index scan while using Bioseq tuple fetch |
| `BioseqSliceProject` | Evaluate lazy slice and transform expressions |
| `BioseqBatchJoin` | Own a sequence join or a post-join per-row search recipe |
| `BioseqBatchSortScan` | Sort one eligible sequence ordering key |
| `BioseqCacheScan` | Produce, replay, reuse, or bypass a query-cache entry |
| `BioseqExternalScan` | Read a provider-backed external relation |

The rest of the PostgreSQL plan still matters. Read parent `Sort`, `Limit`,
`Aggregate`, join, and filter nodes in the normal way. A Bioseq custom node owns
only its part of the statement.

## BioseqScan projection and filtering properties

Every `BioseqScan` starts with `Bioseq Projected Columns`. It lists the columns
that the custom node must supply to its parent.

Use it to catch unnecessary payloads. If a metadata endpoint needs only an ID
and length, a full sequence projection should not appear simply because an
application object normally includes it.

The scan can also report these filter properties:

| Property | Meaning |
| --- | --- |
| `Bioseq Chunk Group Filters` | Qualifiers considered for chunk-group pruning |
| `Bioseq Chunk Groups Removed by Filter` | Groups removed by that pruning path |
| `Bioseq Vectorized Filter` | Qualifiers evaluated by the vectorized filter path |

`Bioseq Chunk Groups Removed by Filter` is an observed count when a scan
descriptor is available. Read it beside the ordinary PostgreSQL row counts.

`Bioseq Fetched From Meta` lists metadata projections resolved without reading
the sequence payload. A symbol count can appear in a form such as the sequence
column, requested symbol, and alphabet rank.

This is the section to inspect for a fast metadata query:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT pk,
       bioseq.symbol_count(sequence, 'A')
FROM reads;
```

If the query also returns `sequence::text`, payload work is still required for
that projection even when the symbol count comes from metadata.

## BioseqScan search-source properties

When a scan owns `mmsearch`, it adds the search-source properties below.

### Bioseq Index Read Requirement

`Bioseq Index Read Requirement` names the index-source contract frozen into the
plan. Read it before the percentage lines. It tells you whether the route allows,
requires, or excludes a persisted target index.

### Bioseq Match Operand Evaluation

`Bioseq Match Operand Evaluation` appears when the target or query must be
evaluated as an expression rather than read through the ordinary persisted
target-column path. Its text explains that evaluation route.

A sliced or transformed target can require this path. It is not equivalent to
reading a prebuilt target index for the original stored column.

### Bioseq Typmod Coalesce

`Bioseq Typmod Coalesce` reports a compatible query value being re-encoded to
the target column's alphabet and storage width. This is visible in plain
planning when the planner can derive it.

The coalesce does not convert between different biological alphabets. It handles
a compatible representation choice within the supported search contract.

### Bioseq Fetched From Sequence Index

`Bioseq Fetched From Sequence Index` reports the estimated persisted-index
presence used by the plan. It can also mention the coalesced type when that
route is active.

### Bioseq Recomputed

`Bioseq Recomputed` reports the share expected to rebuild minimizers. Common
reasons include missing persisted data, an evaluated logical view, or
`bioseq.index.sequence_read.enabled = off`.

The fetched and recomputed percentages describe the plan's view of the target
rows. With `ANALYZE`, use `Persisted Index Rows`, `Missing Persisted Index Rows`,
and `Recomputed Target Index Rows` for observed counts.

### Bioseq Scheduler

`Bioseq Scheduler` is a compact scheduler snapshot. It can include supervisor
state, concurrency mode, current and maximum workers, scheduled and processed
batches, waiting work, query rates, asynchronous persistence work, and reserved
index memory.

Read it as a point-in-time diagnostic. It is not a per-query timing breakdown.
The pipeline and execution-statistics sections carry the query's own work.

## Bioseq Pipeline

Structured output places the search description under `Bioseq Pipeline`.
Text output renders the same information as a one-to-many alignment tree.

The top-level pipeline fields are:

| Field | Meaning |
| --- | --- |
| `Chaining Algorithm` | Chaining algorithm captured for this scan |
| `Base Alignment Algorithm` | Base-alignment algorithm captured for this scan |
| `Pipeline Concurrency Mode` | Single-thread, dispatch-parallel, or task-parallel mode |
| `Max Concurrency` | Maximum scheduler threads available to the pipeline |
| `Index Memory Reserved Bytes` | Index memory reserved in the scheduler snapshot |

The algorithms come from the parameters captured when execution begins. They
are not reconstructed from settings that might change later in the session.

The pipeline contains two nested sections:

- `Highway SIMD Dispatch`
- `Pipeline Stages`

## Highway SIMD Dispatch

This section separates what the binary contains, what the host can execute,
what the plan chose, and what execution recorded.

| Field | Meaning |
| --- | --- |
| `Highway Compiled Targets` | Target kernels compiled into the extension |
| `CPU-Supported Targets` | Targets allowed by the current CPU and operating system |
| `Runnable Targets` | Intersection of compiled and CPU-supported targets |
| `Planned Architecture` | One target selected for the plan |
| `Executed Architecture` | Target recorded during execution |

`Executed Architecture` appears only after execution has recorded a target. A
value of `mixed` means aggregated workers reported different executed targets.

`Planned Highway Functions` is an array in structured output. Each `Highway
Function` object can contain:

- `Function`
- `Planned Architecture`
- `Stage`

The top-level function list is used for a query-sketch function that is not
already represented by a pipeline stage. Stage-owned functions appear under
that stage instead.

Check these relationships:

1. `Planned Architecture` belongs to `Runnable Targets`
2. Each planned function names the same selected architecture
3. With `ANALYZE`, `Executed Architecture` matches the selected architecture
4. A performance comparison preserves the full target lists, not only one host
   name

The Highway tutorial explains the user controls and the fail-closed dispatch
contract.

## Pipeline Stages

`Pipeline Stages` is an array of stage objects. Each object can contain:

| Field | Meaning |
| --- | --- |
| `Stage Name` | Stable stage label |
| `Algorithm` | Algorithm selected for that stage |
| Algorithm parameters | Frozen parameter values specific to that algorithm |
| Computed properties | Derived properties reported by the stage description |
| `Highway Functions` | SIMD kernels owned by this stage |
| `Description` | Text for a stage that has no algorithm object |
| `Estimated Batch Size` | Reserved or estimated stage batch size |

A stage can be absent when its enabling condition is off. Do not expect a fixed
number of stages for every search. CIGAR, divergence, mapping-quality, algorithm,
and backend choices can change the described pipeline.

Under a stage's `Highway Functions`, each object contains `Function` and
`Planned Architecture`. The stage itself supplies the semantic owner, so the
nested object does not repeat `Stage`.

Read stages in order. A late alignment counter cannot explain a bottleneck in
query sketch or target-index construction.

Text output ends the pipeline tree with `Submit results`. After execution, that
line can include target, persisted-index, and recomputed counts. Structured
output puts the same observed work in `Bioseq Execution Statistics`.

When `bioseq.aligner.profiling.enabled` is on, text output also adds a `Runtime`
block. It says profiling is enabled and can print a batch summary with target
rows, persisted-index rows, recomputed rows, attempted jobs, and unattested
jobs. Detailed per-stage profiling remains in Bioseq debug logging. The Runtime
block does not replace PostgreSQL node timing or the structured execution
statistics below.

## Bioseq Execution Statistics

Structured `EXPLAIN ANALYZE` groups actual search work under `Bioseq Execution
Statistics`. Text output prints compact execution, index, GPU, host, and
prefilter lines.

### Backend and index-source fields

| Field | Meaning |
| --- | --- |
| `Requested Backend` | Backend requested by the captured route |
| `Actual Backend` | Backend that completed the work |
| `Requested Target Index Source` | Target-index source requested by the route |
| `Actual Target Index Source` | Source actually used |

Compare requested and actual values before drawing conclusions from a setting.
A difference can be a declared decline or fallback. The counters below show how
often that happened.

### Alignment job identity fields

| Field | Meaning |
| --- | --- |
| `Attempted Alignment Jobs` | Jobs sent into alignment processing |
| `Fully Attested Alignment Jobs` | Jobs with complete recorded route identity |
| `Unattested Alignment Jobs` | Jobs missing that complete record |
| `First Unattested Target Row` | First target row associated with an unattested job |
| `First Unattested Query Ordinal` | First query ordinal associated with one |

The two first-unattested fields appear only when the unattested count is
nonzero.

These Boolean diagnostics describe aggregation health:

- `Environment Mixed Invalid`
- `Mixed Persisted Identity`
- `Operational Identity Conflict`
- `Stats Saturated`

A true value deserves investigation before using the remaining counters in a
performance claim.

### Target-index outcome fields

| Field | Meaning |
| --- | --- |
| `Target Rows` | Target rows processed |
| `Persisted Index Rows` | Rows served with persisted target-index data |
| `Missing Persisted Index Rows` | Rows where expected persisted data was missing |
| `Recomputed Target Index Rows` | Rows whose target index was recomputed |

These are actual counts. Compare them with the plan-time fetched and recomputed
percentages. A changed table or incomplete maintenance can make the execution
mix different from an earlier plan.

### GPU dispatch and fallback fields

| Field | Meaning |
| --- | --- |
| `GPU Seed Dispatches` | Seed-stage device dispatches |
| `GPU Chain Dispatches` | Chaining device dispatches |
| `GPU Chain Backtrack Dispatches` | Chain-backtrack device dispatches |
| `GPU Coordinate Vote Dispatches` | Coordinate-vote device dispatches |
| `GPU Extend Dispatches` | Extension device dispatches |
| `Backend Declines` | Requested backend routes declined before use |
| `Backend Fallbacks` | Work that returned to another backend |

A GPU setting or GPU-capable plan is not enough. Nonzero stage dispatch counters
show that the corresponding work reached the device route.

### Host work and timing fields

| Field | Meaning |
| --- | --- |
| `Query Sketch Host Wall Ns` | Host wall time recorded for query sketch |
| `Target Index Build Host Wall Ns` | Host wall time recorded for target-index build |
| `Host Query Hash Readbacks` | Query-hash readbacks performed on the host |
| `Host Query Symbol Packing Calls` | Host calls that packed query symbols |
| `Host Target Symbol Packing Calls` | Host calls that packed target symbols |
| `Host Alignment Job Classification Calls` | Host job-classification calls |
| `Host CIGAR Construction Calls` | Host CIGAR construction calls |

Nanosecond fields are counters from the pipeline, not the same as PostgreSQL's
whole-node `Actual Total Time`. Use each for its stated scope.

## Prefilter fields inside execution statistics

Prefilter properties live inside `Bioseq Execution Statistics` in structured
output. They answer which prefilter ran and what it removed before full
alignment.

Start with availability and route:

| Field | Meaning |
| --- | --- |
| `Prefilter Stats Available` | Whether this execution produced prefilter statistics |
| `Prefilter Resolved Algorithm` | Algorithm chosen after route resolution |
| `Prefilter Resolved Backend` | Backend chosen for the prefilter |
| `Prefilter Stats Saturated` | Whether a counter reached its representable limit |
| `Prefilter Stats Consistent` | Whether internal counter relationships are valid |

Then read dispatch counts:

- `Prefilter Dispatches`
- `Prefilter SneakySnake Dispatches`
- `Prefilter GateKeeper Dispatches`
- `Prefilter Scalar Dispatches`
- `Prefilter SIMD Dispatches`
- `Prefilter Vulkan Dispatches`

The algorithm dispatches and backend dispatches describe different dimensions.
Do not add them into one total unless the route's counter relationship calls for
that.

Read workload classification next:

- `Prefilter Jobs`
- `Prefilter Evaluated Jobs`
- `Prefilter Length 64 Jobs`
- `Prefilter Unequal-Length Jobs`
- `Prefilter Unsupported Jobs`

Finally read outcome and guard counters:

- `Prefilter Rejected Jobs`
- `Prefilter Rejected Regions`
- `Prefilter Surviving Regions`
- `Prefilter Mask Rejection Candidates`
- `Prefilter Exact Guard Activations`
- `Prefilter Guard Confirmed Rejections`
- `Prefilter Hamming Exact Rejections`

High rejection can be useful when it avoids more expensive alignment work. It
is not automatically better. Check total work, surviving regions, false-negative
requirements, and end-to-end time.

## Column Cache

When `bioseq.column_cache.enabled` is on, a `BioseqScan` can emit a `Cache`
section. This is the sequence column cache, not the optional SQL query cache.

Its fields are:

- `Cache Hits`
- `Cache Misses`
- `Cache Evictions`
- `Cache Writes`
- `Cache Maximum Size`
- `Cache Ending Size`
- `Total Cache Entries`

These values come from the column-cache statistics snapshot. They describe
sequence-column caching around the scan. Query-cache fields use names such as
`Cache Mode`, `Planned Cache Action`, and `Actual Cache Action` and appear on a
different custom node.

## BioseqSliceProject header

A lazy slice or transform plan can add `Custom Scan (BioseqSliceProject)`.
Structured output begins with:

| Field | Meaning |
| --- | --- |
| `Projection Mode` | `source-backed lazy` for this path |
| `Source Pieces` | Number of source pieces in the recipe |
| `Estimated Piece Boundaries` | Estimated boundaries between pieces |
| `Fallback Reason` | Reason recorded when the ideal lazy route is not used |

Text output moves directly into `Slice Recipe` and `Slice Materialization`.

Explain both a metadata-only expression and a client-output expression:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, FORMAT JSON)
SELECT bioseq.len(sequence[10:-10])
FROM reads;
```

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, FORMAT JSON)
SELECT sequence[10:-10]::text
FROM reads;
```

The comparison shows where a concrete payload becomes necessary.

## Slice Recipe

`Slice Recipe` is an array of `Recipe Node` objects. Each object can contain:

- `Stable ID`
- `Opcode`
- `Expression`
- `Depends On`
- `Lower Bound`
- `Upper Bound`

The opcodes include source, literal, element, slice, concat, reverse, reverse
complement, length, cardinality, literal bridge, and materialize.

Dependencies name earlier recipe values. Bounds appear for element and slice
work when the corresponding expression is present. An omitted bound is not
invented as a numeric value.

Use the recipe to follow data flow. A sequence can be sliced, concatenated, and
reversed while still represented by source pieces. Recipe presence alone does
not mean a full value was copied.

## Slice Materialization

`Slice Materialization` is an array of `Materialization` objects. Each object
can contain:

| Field | Availability and meaning |
| --- | --- |
| `Variable` | Boundary value name |
| `Expression` | SQL expression materialized at the boundary |
| `Mechanism` | Mechanism used to create the concrete value |
| `Reason` | Why a concrete value was required |
| `Depends On` | Earlier materialization boundaries |
| `Encoding` | Output encoding at the boundary |
| `Cache Scope` | Reuse scope for the concrete value |
| `Producer Value` | Recipe producer, with `VERBOSE` |
| `Source Provenance` | Source expression, with `VERBOSE` |
| `Estimated Symbols` | Estimate when costs are enabled |
| `Estimated Bytes` | Byte estimate when costs are enabled |

`EXPLAIN ANALYZE` adds:

- `Actual Calls`
- `Actual Input Symbols`
- `Actual Output Symbols`
- `Actual Bytes`
- `Reused Calls`
- `Peak Retained Bytes`
- `Counter Overflow`
- `Actual Time` when timing is enabled

`Incomplete Workers` appears when analyzed parallel trace aggregation did not
receive a complete worker record.

In text output with costs enabled, the node also prints `Planned Materialized
Symbols` and `Planned Materialized Bytes` after the materialization list.

An empty materialization list is useful. It means the recipe answered the query
without a recorded concrete payload boundary.

## BioseqBatchJoin

A batch sequence join emits these plan and execution properties:

| Field | Meaning |
| --- | --- |
| `Bioseq Query Batch Capacity` | Fixed runtime query buffer capacity |
| `Bioseq Residual Mmsearch Qual` | Whether a raw search predicate remains |
| `Bioseq Batch ABI Version` | Batch plan contract version |
| `Bioseq Honest Startup Cost` | Startup cost retained by the custom path |
| `Bioseq Honest Total Cost` | Total cost retained by the custom path |
| `Bioseq Incumbent Cost Floor` | Cost floor of the replaced candidate path |
| `Bioseq Match Placeholder Sites` | Rewritten match-result projection sites |
| `Bioseq Index Read Requirement` | Target-index source requirement |
| `Bioseq Requirement Executable` | Whether the frozen requirement is executable |
| `Bioseq CPU Prepared Target` | Description of the retained CPU target route |
| `Bioseq Query Planner Rows` | PostgreSQL's query-side row estimate |
| `Bioseq Query Planner Width` | PostgreSQL's estimated row width in bytes |
| `Bioseq Query Planner Admission` | Planned query materialization bytes |
| `Bioseq Query Actual Materialization` | Actual query materialization bytes |
| `Bioseq Distinct Targets` | Distinct target identities observed |
| `Bioseq Query Batches` | Query batches executed |

The first three can appear before runtime initialization. Runtime-dependent
cost, placeholder, admission, and actual fields require an initialized node.

The batch join also emits the shared Highway and `Bioseq Execution Statistics`
sections. Read those per custom join node when a statement has two searches.

For an owned batch join, `Bioseq Residual Mmsearch Qual` should be `absent`.
That means the custom node owns the predicate rather than leaving it as an
ordinary join filter.

## BioseqBatchJoin per-row recipe

A transformed target can use the per-row recipe form under the same custom
provider name. Identify it by:

```text
Bioseq Mmsearch Ownership: per-row recipe
Bioseq Reconstructed Join: child plan
```

Its ownership diagnostics are:

- `Bioseq Owned RestrictInfo Path Copies`
- `Bioseq Executable Mmsearch Qual Copies`
- `Bioseq Vector Mmsearch Qual Copies`
- `Bioseq Storage Mmsearch Qual Copies`
- `Bioseq Descendant Mmsearch Owner Copies`
- `Bioseq Residual Mmsearch Qual`
- `Bioseq Batch Capacity`
- `Bioseq Index Read Requirement`

The transformed-target path reports an absent index requirement because it
cannot reuse the stored index for an arbitrary logical view.

After runtime initialization it adds the honest cost fields, `Bioseq Match
Placeholder Sites`, Highway dispatch, and execution statistics.

The copy counters are planner ownership checks. A raw executable `mmsearch`
copy in the wrong descendant or a residual predicate is not a performance knob.
It indicates a query shape that did not reach the intended owner.

## BioseqBatchSortScan

With `bioseq.planner.sequence_sort_scan.enabled = on`, an eligible single-key
sequence ordering can become `Custom Scan (BioseqBatchSortScan)`.

The node reports:

| Field | Meaning |
| --- | --- |
| `Bioseq Sort Accelerator` | `vulkan`, `stdc_compare`, another recorded CPU source, or not initialized |
| `Bioseq Sort Rows` | Rows sorted by the custom executor |
| `Bioseq Sort Runtime Ns` | Sort runtime recorded by the node |
| `Bioseq GPU Sort Dispatches` | Device sort dispatches |
| `Bioseq GPU Global Compare Dispatches` | Full-payload compare dispatches |
| `Bioseq GPU Dense Scatter Dispatches` | Prefix dense-scatter dispatches |
| `Bioseq GPU Sort Uploaded Bytes` | Bytes uploaded by GPU sort attempts |
| `Bioseq Source Rows Admitted` | Source rows admitted into the sort representation |
| `Bioseq Source Bytes Admitted` | Source bytes admitted |
| `Bioseq Source Admission Fallbacks` | Source-admission failures that used the fallback |
| `Bioseq Source Routes Acquired` | Whether source routes were acquired |

The custom node means the planner selected the batch sorter. It does not mean
the GPU ran. `Bioseq Sort Accelerator: vulkan` and a nonzero `Bioseq GPU Sort
Dispatches` show actual GPU sort work.

With Vulkan disabled, expect the CPU comparator and zero GPU dispatch counters.
The Highway and GPU controls tutorial contains a complete A/B query.

## Exact BioseqCacheScan

The optional SQL query cache wraps a finalized plan in `BioseqCacheScan`. In
`exact_query` mode, read these planning fields first:

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

`Cache Entry Kind` is `ExactFinalResult` for this path.

With `ANALYZE`, read:

- `Provider Runtime Recipe State`
- `Actual Cache Action`

Actual actions include:

- `[Hit] exact final result`
- `[Produce] exact final result`
- `[Bypass] retained child`

The rest of the exact-cache diagnostics describe readiness and ownership:

| Field | Meaning |
| --- | --- |
| `Cache Readiness` | Current correctness readiness state |
| `Cache Storage` | Storage readiness, attachment, reservations, ceilings, manifests, and waiters |
| `Cache Storage Ownership` | Readers, pending reservations, registry references, and planner pins |
| `Cache Janitor` | Background janitor lifecycle |

These storage strings contain several named values. Treat them as human
diagnostics. Use `bioseq_cache.stats()` and `bioseq_cache.entries()` for tabular
monitoring.

## Reusable BioseqCacheScan

In `reusable_fragments` mode, `Cache Entry Kind` is `ReusableCoverage`. The
top-level fields are:

- `Cache Entry Kind`
- `Cache Mode`
- `Cache Force Bypass`
- `Planned Cache Action`
- `Actual Cache Action` with `ANALYZE`
- `Cache Entry`
- `SQL`
- `Reusable Sources`
- `Selected Cache Branches`
- `Provider Runtime Recipe Valid`

When a current generation snapshot is available, the node can also report:

- `State`
- `Producer PID` when the viewer has diagnostic permission
- `Age`
- `Effective TTL`
- `Rows`
- `Logical Bytes`
- `Stored Bytes`
- `Dependencies`

Without a current snapshot, state is `planned/provisional`. Analyzed output can
still report rows and byte counts collected by the executor.

Each selected `Cache Branch` contains:

- `Action`
- `Recheck` when a branch needs a local recheck expression

The `Residual Child` section contains `Predicate` when uncached work remains.
The optional `Provider` section contains provider-specific reusable-domain
details supplied by the provider. Its fields depend on that provider rather
than the cache core.

Read branch actions before concluding that a partial result was fully cached. A
statement can reuse selected branches and execute a residual child for the rest.

## BioseqExternalScan

An external-provider relation reports a small, deliberate Bioseq section:

- `Bioseq External Entity`
- `Bioseq External Provider`

The entity names the logical relation target such as taxonomy, catalog,
sequence, or sequence chunk. The provider names the selected remote adapter.

Read the ordinary PostgreSQL `Filter` beside these fields. It contains the
provider and record restrictions present in the SQL.

Plain `EXPLAIN` does not make the remote request. `EXPLAIN ANALYZE` does. The
node's PostgreSQL actual rows and timing therefore become the main runtime view
for the external scan.

If a plan falls back to a physical sequential scan on an external relation, it
cannot return a valid remote result. Fix the custom-scan planning issue rather
than treating an empty physical relation as data.

## BioseqIndexScan

`BioseqIndexScan` wraps an ordinary PostgreSQL index scan so tuple fetch can use
Bioseq's projected-column reader.

Its first custom property is `BioseqIndexScan using ` with the index name as the
value. It then emits the familiar PostgreSQL index details:

- `Index Cond`
- `Rows Removed by Index Recheck` with `ANALYZE` when nonzero
- `Order By`
- `Filter`
- `Rows Removed by Filter` with `ANALYZE` when nonzero

Interpret the conditions and row counters as you would for a PostgreSQL index
scan. The custom node changes tuple fetch for a Bioseq relation. It does not
turn an unrelated metadata index into a sequence-alignment index.

## Test-build-only DSM diagnostics

A build compiled with Bioseq test indexes can add slice DSM fixture fields under
an analyzed `BioseqSliceProject`:

- `DSM Test Fixture`
- `DSM Fixture Validated`
- `DSM Async Lifecycle Validated`
- `DSM Fixture Checks`
- `DSM Fixture Slot Headers Read`
- `DSM Fixture Counter Records Read`
- `DSM Async Cleanup Callbacks`

These fields exercise test fixtures for parallel trace handling. They are not
application settings or production tuning controls. Their absence from a
production plan is normal.

## A complete reading order for a literal search

For one `mmsearch` over a local table, read the plan in this order:

1. Confirm `Custom Scan (BioseqScan)`
2. Check `Bioseq Projected Columns`
3. Read chunk-group and vectorized filters
4. Read metadata fast paths
5. Check `Bioseq Index Read Requirement`
6. Compare fetched and recomputed index shares
7. Read scheduler mode and reserved index memory
8. Read the planned pipeline algorithms
9. Confirm the Highway planned target is runnable
10. Read every enabled pipeline stage
11. With `ANALYZE`, compare requested and actual backend and index source
12. Check persisted, missing, and recomputed target counts
13. Check GPU dispatch, declines, and fallbacks
14. Read prefilter outcomes
15. Read host work and timing
16. Read column-cache counters if the section is present

This order moves from query shape to route choice to actual work.

## A complete reading order for a sequence join

For a join, use this order:

1. Identify each `BioseqBatchJoin` node separately
2. Confirm which alias supplies the stored target
3. Confirm `Bioseq Residual Mmsearch Qual` is absent
4. Read the index requirement and CPU prepared-target description
5. Compare planned query rows and bytes with actual materialization
6. Read query batch count and distinct target count
7. Read the Highway and execution sections for that node
8. Repeat for a second custom join node before combining totals

For a per-row recipe, first confirm `Bioseq Mmsearch Ownership: per-row recipe`
and inspect the copy counters.

## A complete reading order for cache reuse

For an exact cache node:

1. Read cache mode and force-bypass state
2. Read parameter and dependency counts
3. Check provider runtime recipes
4. With `ANALYZE`, read actual action
5. Check readiness, storage, ownership, and janitor only if the action or cache
   health needs investigation

For reusable coverage:

1. Read selected branch count and actual action
2. Inspect every emitted `Cache Branch`
3. Read each branch recheck
4. Inspect the `Residual Child` predicate
5. Read generation age, TTL, rows, and bytes
6. Inspect provider detail when present

This prevents a partial branch hit from being reported as a full-result hit.

## Compare plans without losing context

When changing a GUC, preserve two complete plans from the same data state. For
example, compare CPU vectorization:

```sql
SET bioseq.vectorization.enabled = off;
```

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, FORMAT JSON)
SELECT pk
FROM reads
WHERE public.mmsearch(
    sequence,
    $1::bioseq.seq('dna4', 32)
);
```

```sql
SET bioseq.vectorization.enabled = on;
```

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF, FORMAT JSON)
SELECT pk
FROM reads
WHERE public.mmsearch(
    sequence,
    $1::bioseq.seq('dna4', 32)
);
```

Compare result rows first. Then compare custom node, pipeline, planned and
executed architecture, requested and actual backend, target-index source,
prefilter work, dispatch counters, and timings.

Do not compare only PostgreSQL's final execution time while the plans use
different result counts, data generations, providers, cache states, or target
index mixes.

## Common interpretation mistakes

Seeing `BioseqBatchSortScan` does not mean the GPU sorted. Read accelerator and
dispatch counters.

Seeing Vulkan enabled does not mean GPU alignment ran. Read stage dispatches,
actual backend, declines, and fallbacks.

Seeing `BioseqCacheScan` does not mean the cache hit. Read `Actual Cache Action`.

Seeing a slice recipe does not mean materialization occurred. Read `Slice
Materialization` and actual bytes.

Seeing `Bioseq Fetched From Meta` does not mean no sequence payload was read for
other projections.

Seeing a high prefilter rejection count does not by itself mean a faster query.
Compare full execution work and time.

Seeing a planned Highway architecture does not mean it executed. Use
`EXPLAIN ANALYZE` and read `Executed Architecture`.

Finally, do not read a custom node in isolation from its PostgreSQL parents and
children. A parent sort, aggregate, materialization, or nested loop can dominate
the statement even when the Bioseq node is working as intended.
