---
title: Highway runtime dispatch
slug: highway_runtime_dispatch
description: Read Bioseq's compiled, runnable, planned, and executed SIMD target evidence.
order: 40
keywords: [Highway, SIMD, runtime dispatch, EXPLAIN, architecture]
draft: false
---

Bioseq compiles vector kernels for a closed Highway target set and intersects it
with the targets supported by the current CPU. The scheduler binds one runnable
target to the plan, and every worker executes that exact target.

## Inspect dispatch evidence

The following query executes a real `BioseqScan`. Its JSON plan carries five
labels: `Highway Compiled Targets`,
`CPU-Supported Targets`, `Runnable Targets`, `Planned Architecture`, and
`Executed Architecture`.

```sql runnable id=highway_dispatch_explain timeout=30s
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
SET bioseq.vectorization.enabled = on;
SET bioseq.aligner.vulkan.enabled = off;
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

`Highway Compiled Targets` lists the kernels in the extension binary.
`CPU-Supported Targets` lists the targets allowed by the current CPU and
operating system. Their intersection is `Runnable Targets`. Target names vary
between builds and hosts, so do not compare them with a fixed list.

`Planned Architecture` is the one runnable target selected when the plan is
created. Every worker uses that same target. With `ANALYZE`,
`Executed Architecture` reports the target that ran. It is `mixed` only when
workers report different executed targets.

`EXPLAIN` without `ANALYZE` shows the first four labels. Add `ANALYZE` when you
also need the executed target.

Dispatch is fail-closed. If the stored alphabet, storage width, or selected
operation has no compiled runnable combination, Bioseq reports an error instead
of falling back to an unreported scalar implementation.
Use the five labels together when checking why a target was or was not used.
