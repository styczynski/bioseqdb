---
title: Highway runtime dispatch
slug: highway_runtime_dispatch
description: Read Bioseq's compiled, runnable, planned, and executed SIMD targets, plus the related Vulkan sort counters.
order: 70
keywords: [Highway, SIMD, runtime dispatch, EXPLAIN, architecture, Vulkan, GPU sort]
draft: false
---

Bioseq compiles a closed set of Highway SIMD targets into the extension. At
planning time, it intersects that set with the targets the current CPU and
operating system can run. The resulting runnable set has one selected target.
The scheduler binds that target once for the query and validates that the bound
kernel table is complete before worker execution starts.

This page separates three questions that are easy to conflate:

- What the extension was built with
- What this host can run and what the planner selected
- What actually ran for this execution

The first two are planning facts. The third is a runtime result, so it needs
`EXPLAIN ANALYZE`.

## SIMD controls and a reproducible dispatch query

`bioseq.vectorization.enabled` enables Bioseq vectorized execution paths. It
does not name an ISA and it is not a request to use a particular Highway target.
Highway chooses from the compiled targets that are runnable on the backend's
current host. Do not hard-code a target name in an application check because
the target inventory can differ by build, CPU, and operating-system support.

The following query selects the deterministic SIMD algorithms used by the
integration examples. Vulkan is disabled so the resulting fields describe
the CPU Highway dispatch rather than a GPU alignment route.

```sql runnable id=highway_dispatch_explain timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.index.sequence_read.enabled = off;
SET bioseq.vectorization.enabled = on;
SET bioseq.aligner.vulkan.enabled = off;
SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';
SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';
SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';
CREATE TABLE highway_dispatch_reads (
  pk integer PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO highway_dispatch_reads VALUES
  (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'),
  (2, 'CCCCAACTTAGTCACTTCTCAAAAATTACTCTCTCATCATCATCTACTCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACACACACCCCAAA'),
  (3, 'GGGGGTCACTTCTCAAAAATTAGGGGGGGGGTAGTCACTGTAGCCTGATCGTAGCTAGGACACATATCATCATCTACTCCTATCATCATCTACTCCTA');
-- bioseq:visible
EXPLAIN (ANALYZE, VERBOSE, FORMAT JSON)
SELECT pk
FROM highway_dispatch_reads
WHERE public.mmsearch(
  seq0,
  'CTAGTCACTGTAGCCTGATCGTAGCTAGT'::bioseq.seq('dna4', 32))
ORDER BY pk;
-- bioseq:teardown
DROP TABLE highway_dispatch_reads;
```

The public control surface in this example is deliberately small. Use
`bioseq.vectorization.enabled = off` for a CPU vectorization A/B comparison.
Use the algorithm settings only when you need a particular supported pipeline
shape. They select the minimizer, chaining, and base-alignment algorithms, not
an individual Highway ISA.

## Read every Highway EXPLAIN field

Every relevant Bioseq scan reports the same five stable dispatch fields. In
JSON they are collected once under the `Highway SIMD Dispatch` object.

| Field | Meaning | When it is available |
| --- | --- | --- |
| `Highway Compiled Targets` | Target kernels present in this extension binary | Planning and execution |
| `CPU-Supported Targets` | Targets Highway says this CPU and OS may execute | Planning and execution |
| `Runnable Targets` | The intersection of compiled and CPU-supported targets | Planning and execution |
| `Planned Architecture` | The single target selected from `Runnable Targets` | Planning and execution |
| `Executed Architecture` | The target recorded by execution | `EXPLAIN ANALYZE` only |

Plain text `EXPLAIN` prints the first four fields and planned `Highway
Function` entries. Those entries identify the selected function and its stage,
such as query sketch, chaining, or alignment. They are an explanation of the
plan, not a record of completed execution.

`EXPLAIN ANALYZE` adds `* Executed Architecture` in text output. For the
normal one-plan, one-bind path it must equal `Planned Architecture`. The value
`mixed` means that the aggregated workers reported different executed targets.
Treat `mixed` as a diagnostic result. It does not say one requested target ran.

Structured output has two useful levels. `Highway SIMD Dispatch` contains the
five dispatch fields. `Planned Highway Functions` lists the top-level planned
functions, while `Highway Functions` can occur under individual pipeline
stages when verbose stage detail is present. A plain structured `EXPLAIN`
intentionally omits `Executed Architecture`. Its presence means that the
statement executed under `ANALYZE`.

The following checks are more useful than comparing host-specific names:

1. Confirm that `Runnable Targets` contains `Planned Architecture`.
2. Confirm that every expected `Highway Function` has the same planned
   architecture.
3. Run `EXPLAIN ANALYZE` and confirm that `Executed Architecture` equals the
   planned architecture.
4. Preserve the complete dispatch object with a performance report, because a
   later build or host may have a different compiled or supported inventory.

## Vulkan controls are capability requests, not execution receipts

`bioseq.aligner.vulkan.enabled` is the top-level Vulkan switch used by both
alignment routing and the sequence batch-sort executor. Enabling it permits an
eligible path to attempt Vulkan. It does not make a GPU available, force a GPU
kernel, or replace the Highway fields above.

For a supported `mmsearch` all-GPU tuple, the additional public controls are
`bioseq.aligner.vulkan.seed_lookup.enabled`,
`bioseq.aligner.vulkan.index_cache.enabled`,
`bioseq.aligner.vulkan.fused_frontend.enabled`,
`bioseq.aligner.vulkan.index_build.enabled`, and
`bioseq.aligner.vulkan.alignment_extension.enabled`. The Vulkan-specific
algorithm choices exercised by the integration suite are
`bioseq.aligner.minimizer.algorithm = 'vulkan_minimap_integer'` and
`bioseq.aligner.chaining.algorithm = 'vulkan_tiled_dynamic_programming'`.
These controls describe a supported alignment route. They do not change the
meaning of the five Highway fields and should be enabled only with a compatible
device, stored representation, query shape, and selected algorithm tuple.

For sequence `ORDER BY`, the separate opt-in
`bioseq.planner.sequence_sort_scan.enabled` asks the planner to replace one
eligible sequence sort with `Custom Scan (BioseqBatchSortScan)`. It defaults to
off. With it off, PostgreSQL keeps its normal `Sort` node. With it on, the
planner still leaves the normal plan in place unless the query has exactly one
sequence ordering key and an eligible input path.

## GPU sequence sort: plan choice versus execution result

The batch-sort custom scan is a sequence ordering accelerator, not a general
promise that every `ORDER BY` runs on the device. The sort executor reads
`bioseq.aligner.vulkan.enabled` at execution time. A GPU attempt requires a
usable Vulkan device, a batch that can be represented as a compatible resident
packed layout, valid bounded sizes, memory admission, and a successful device
round trip. DNA4 and DNA5 have dedicated kernel kinds. Other supported sequence
alphabets use the generic kernel kind.

This query is a portable control experiment. It selects the custom sort plan
and makes the CPU fallback easy to read by explicitly disabling Vulkan.

```sql runnable id=highway_dispatch_batch_sort timeout=30s
-- bioseq:setup
SET bioseq.planner.sequence_sort_scan.enabled = on;
SET bioseq.aligner.vulkan.enabled = off;
CREATE TABLE highway_dispatch_sort_reads (
  pk integer PRIMARY KEY,
  seq0 bioseq.seq('dna4', 32)
) USING bioseq;
INSERT INTO highway_dispatch_sort_reads VALUES
  (1, 'TTTT'),
  (2, 'AAAA'),
  (3, 'CCCC'),
  (4, 'GGGG');
-- bioseq:visible
EXPLAIN (ANALYZE, VERBOSE)
SELECT pk
FROM highway_dispatch_sort_reads
ORDER BY seq0;
-- bioseq:teardown
DROP TABLE highway_dispatch_sort_reads;
```

First look for `Custom Scan (BioseqBatchSortScan)`. That is a planning result
only. Then read its execution properties, which are produced by the custom
scan after it runs:

| EXPLAIN property | What it reports |
| --- | --- |
| `Bioseq Sort Accelerator` | `vulkan`, `stdc_compare`, or `stdc_compare backend source` for this execution |
| `Bioseq Sort Rows` and `Bioseq Sort Runtime Ns` | The observed batch size and sort time |
| `Bioseq GPU Sort Dispatches` | Number of sequence-sort GPU dispatches |
| `Bioseq GPU Global Compare Dispatches` | Number of full-payload compare dispatches |
| `Bioseq GPU Dense Scatter Dispatches` | Number of prefix dense-scatter dispatches |
| `Bioseq GPU Sort Uploaded Bytes` | Bytes uploaded for the recorded sort attempt |
| `Bioseq Source Rows Admitted` and `Bioseq Source Bytes Admitted` | Source materialization admitted before sorting |
| `Bioseq Source Admission Fallbacks` | Count of source-admission failures that reverted to the original source views |
| `Bioseq Source Routes Acquired` | Whether the custom scan acquired source routes |

With the control query above, `Bioseq Sort Accelerator` is `stdc_compare` and
the three GPU dispatch counters are zero. When Vulkan is enabled on a suitable
host, `Bioseq Sort Accelerator: vulkan` and a nonzero `Bioseq GPU Sort
Dispatches` show that a GPU sort actually occurred. A
`BioseqBatchSortScan` node alone does not say the GPU ran. Likewise,
enabling `bioseq.aligner.vulkan.enabled` alone does not say the GPU ran.

The GPU sorter can first try a prefix payload when sampled leading units vary.
It accepts that result only if the runtime recorded a dense-scatter dispatch.
Otherwise it reruns with the full payload for exact ordering. Descending order
reverses the resulting permutation, and equal sequences retain a deterministic
source-index tie break. GPU and CPU paths therefore preserve the same sequence
ordering contract.

## Fallbacks and fail-closed boundaries

The two dispatch systems have intentionally different failure behavior.

- Highway is fail-closed. A plan with no runnable selected target, an incomplete
  target table, or a dynamic bind that disagrees with the planned target raises
  an error. Bioseq does not silently substitute an unreported scalar target for
  that selected SIMD work.
- The batch sorter is opportunistic. If Vulkan is disabled, no device is
  available, the batch cannot satisfy the resident layout contract, memory
  admission fails, payload construction overflows, or a Vulkan sort round trip
  fails, it returns to the CPU comparison sorter. The EXPLAIN accelerator and
  counters make that fallback observable.
- A source-admission failure is also visible. The custom scan increments
  `Bioseq Source Admission Fallbacks` and labels the execution
  `stdc_compare backend source` when that fallback was the reason the CPU path
  was used.
- A requested supported all-GPU alignment tuple is stricter than the sort
  preference. Its routing checks reject incomplete or unauthenticated GPU
  prerequisites rather than silently claiming a GPU execution path.

Use the five Highway fields for CPU SIMD selection and the batch-sort execution
properties for the GPU-sort result. Keep planning labels and execution fields
together when investigating a fallback or comparing performance across hosts.
