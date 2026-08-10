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
target to the plan, and every worker executes that exact target bit.

## Inspect dispatch evidence

The following query executes a real `BioseqScan`. Its JSON plan carries the five
stable labels used by production diagnostics: `Highway Compiled Targets`,
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

`Runnable Targets` is the intersection of `Highway Compiled Targets` and
`CPU-Supported Targets`. `Planned Architecture` records the single target bound
by the leader. With `ANALYZE`, `Executed Architecture` reports the target that
actually ran, or `mixed` only when execution evidence genuinely disagrees.

Dispatch is fail-closed. If the stored alphabet, storage width, or selected
operation has no compiled runnable coordinate, Bioseq reports an unsupported
coordinate instead of falling back to an unreported scalar implementation.
That makes the five labels suitable for release qualification and incident
diagnosis, not just performance hints.
