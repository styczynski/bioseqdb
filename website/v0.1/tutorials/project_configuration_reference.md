---
title: "Configuration reference (settings available in BioseqDB)"
slug: project_configuration_reference
description: "Configuration reference (settings available in BioseqDB)"
order: 140
keywords: [BioseqDB, project]
draft: false
---

<a id="sec-appendix-configuration"></a> <a id="sec-configuration-reference"></a> <a id="app-configuration-reference"></a>

This appendix lists all runtime settings registered by BioseqDB (GUCs - GUC stands for Grand Unified Configuration in Postgres and refers to runtime parameters) - their types, defaults, possible values and descriptions. Each setting can be viewed and set with the `SHOW` and `SET` commands \[[118](../bibliography/#ref-postgresql17_pg_settings), [119](../bibliography/#ref-postgresql17_guc_functions)\]. Some settings are hidden so they’re not visible with `SHOW ALL`.

<a id="lst-configuration-inspect"></a>

```sql runnable id=project_lst-configuration-inspect timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
-- bioseq:visible
SHOW bioseq.aligner.minimizer.algorithm;

SELECT name, setting, boot_val, reset_val,
       vartype, context, min_val, max_val, enumvals
FROM pg_settings
WHERE name = 'bioseq.aligner.minimizer.algorithm';

SET bioseq.aligner.minimizer.algorithm = 'scalar';
-- bioseq:teardown
RESET bioseq.aligner.minimizer.algorithm;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Inspecting and settingBioseqDB config values

## Physical storage

*Physical storage settings.*<a id="tab-configuration-storage"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.storage.compression.algorithm</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>zstd</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Choose the compression algorithm used for storing data columns Use Zstandard when compiled in. This is the preferred build-time default. Changing the default does not rewrite any existing indices!</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>none</code></td>
<td style="text-align: left;">Store column chunks without compression.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>pglz</code></td>
<td style="text-align: left;">Use Postgres built-in coded. This becomes the default when neither of previous two is available.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>lz4</code></td>
<td style="text-align: left;">Use LZ4 when compiled in. This becomes the default when Zstandard is unavailable.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.storage.compression.level</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>3</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…19</span></p></td>
<td style="text-align: left;">Set the zstd compression level if zstd is used.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.storage.stripe.row_limit</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>150000</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1000…100000000</span></p>
<p>rows</p></td>
<td style="text-align: left;">Sets the maximum rows in a new table’s column stripe, when writing rows. Existing table options and stored stripes are unchanged.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.storage.chunk_group.row_limit</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>10000</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1000…100000000</span></p>
<p>rows</p></td>
<td style="text-align: left;">Sets the maximum rows in a new table’s chunk group, when writing rows. Chunk groups divide a stripe into smaller column-reading units.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.storage.dml.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Special setting that controls whether reads apply deleted-row masks. Default, shouldn’t be changed. Normally deleted rows that are marked as such, should not be returned in queries.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Bypass any deletion filtering. Useful for debugging, the queries will return all rows including deleted ones. Note that if the table is rewritten using VACCUM FULL, then deleted rows will physically dissapear!</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.column_cache.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Controls the backend’s decompressed column-chunk cache. After decompression save the buffer, so it’s avaialable for reuse Decompression cache only applies to one query. After completion of the query, the cache is cleared.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Read and decompress but do not save buffers in cache.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.column_cache.capacity_mb</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>200</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">20…20000</span></p>
<p>MiB</p></td>
<td style="text-align: left;">Limits the decompressed column-chunk cache BioseqDB will still allocate buffers for decompression, so this setting does not control the total memory used during decompression</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.load.write_overlap.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Experimental setting to use parallelized writing and decoding of data chunks through queue when loading FASTA/FASTQ files. Default reader perform decoding and writing (placing sequences in buffer) sequentially.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Enable parallelized writing and decoding of data chunks through queue.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.storage.sequence_shard.max_size_gib</code></p>
<p><em>superuser</em></p></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>1024</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…2147483647</span></p>
<p>GiB</p></td>
<td style="text-align: left;">Sets the sequence-shard size limit. If the shard table exceeds that size, then new table will be created. Setting it to 0 means that each sequence is stored in a separate table (useful for debugging, but extremally ineffective in practice)</td>
</tr>
</tbody>
</table>

## Planning and execution

*Planning and execution settings.*<a id="tab-configuration-planning"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><p><code>bioseq.execution.parallel.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enable Postgres parallel queries Allow possible parallel scan paths (actual worker availability still depends on PostgreSQL planner, so this setting does not force the planner to use parallel scans). Additionally, any query using mmsearch won’t be parallelized, due to current code architecture. The parallel scans however are possible for metadata queries. Alignment won’t use parallel scans even with this setting enabled.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Disable all parallel scans on BioseqDB tables.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.execution.parallel.minimum_processes</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>8</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…32</span></p></td>
<td style="text-align: left;">Requests the PostgreSQL worker count for possbible Bioseq parallel scan paths. This is soft minimum and the actual worker count is determined by the PostgreSQL planner.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.vectorization.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Can disable vectorized SQL operations for aggregates like SUM(), COUNT(), etc. Use supported vectorized SQL operations when the plan allows them. Vectorized operations only apply to non-sequence aggregates, not alignment pipeline.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Use normal SQL processing for those operations.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.planner.index_scan.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Can allow usage of non-BioseqDB indices with BioseqDB tables. Allow using non-BioseqDB indices during queries, for example it becomes possible to use B-tree index on metadata column. This settings does not control any BioseqDB indices like MPF/MPS etc.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Do not allow using any non-BioseqDB indices on BioseqDB tables. This can heavily degrade performance as Postgres will select plan that ignores existing B-trees, hash indexes etc.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.planner.custom_scan.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Entirely disable all BioseqDB custom scans. Allow the planner to install Bioseq custom scans. Useful to force Postgres to select non-BioseqDB plans. Do not use this unless you know what you’re doing.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Disable custom scans. This means that it won’t be possible to use ’mmsearch’ function anymore as it requires custom scan path. All other metadata queries still will be possible.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.planner.custom_scan.max_paths</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>64</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…1024</span></p></td>
<td style="text-align: left;">Limit paths explored by GQO for BioseqDB custom scans. Increasing the number may yield better plans, but increase planning time.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.planner.qual_pushdown.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Controls behaviour of predicate pushdown. Normally filters can be pushed down into the scan, which improved performance. The predicates that Postgres reports as highly-correlated will be used to possibly reduce number of loaded chunks (min/max prunning mechanism) Pushdown uses correlation statistics for column reported by Postgres to decide if this predicate can be useful to avoid loading chunks</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Leave predicates outside the scan, which may severely degrade performance.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.planner.qual_pushdown.correlation_threshold</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">real</td>
<td style="text-align: left;"><p><span><strong><code>0.4</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1</span></p></td>
<td style="text-align: left;">Sets the minimum absolute column correlation value (reported by Postgres table statistics) used by chunk prunning logic described for bioseq.planner.qual_pushdown.enabled. Values closer to one, mean stronger correlation.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.planner.sequence_sort_scan.enabled</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enable or disable custom sort for BioseqDB sequences. Sort works only if there’s single sorting key that is sequence column itself. Use default Postgres sort.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Consider the custom batch-sort where possible.</td>
</tr>
</tbody>
</table>

## Task scheduling

*Task scheduling settings.*<a id="tab-configuration-scheduler"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><p><code>bioseq.scheduler.backend.max_concurrency</code></p>
<p><em>reload</em></p></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>0</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1024</span></p></td>
<td style="text-align: left;">Limits maximum number of workers in TBB worker pool (this is separate to Postgres max workers setting)</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.scheduler.pipeline.mode</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>task_parallel</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Changes how the pipeline execution is performed Each query dispatches separate worker (exxtension of parallel_dispatch) and additionally each stage of pipeline can submit independent tasks to TBB worker pool In the benchmark I use task_parallel, because it enables the highest degree of parallelism</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>single_thread</code></td>
<td style="text-align: left;">Each query dispatches separate worker, but one pipeline itself isn’t parallelized.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>parallel_dispatch</code></td>
<td style="text-align: left;"></td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.scheduler.postgres_parallel.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Reduce tasks slots to 1 Disable any parallelism completely (note that if bioseq.scheduler.pipeline.mode is set to task_parallel, then pipeline will be executed via task queues, but only one worker will be active at the time. This setting is the highest level limiter on parallelism of workers) This setting effectively forces bioseq.scheduler.backend.max_concurrency to be 1</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Allow &gt;1 parallel tasks limit</td>
</tr>
</tbody>
</table>

## Index construction and reading

*Index construction and reading settings.*<a id="tab-configuration-index"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.index.insert_maintenance.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enables automatic index incremental update when row is inserted into the table. Save new source rows without automatic index builds. You can always still manually update index via update_index function.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Currently grouped MPF index (the default one) does not support this function and reports an error.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.index.sequence_read.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Controls whether the pipeline executor can read indexes. If the option is disabled, then pipeline will always recompute all seeds from scratch. Normally allow reading BioseqDB indices. There’s also per-search option passed via aligner_options: index_read_requirement codes 1 and 2 override this setting.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Force pipeline to always recompute all seeds from scratch.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.index.async_persistence.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Very experimental setting that allows main process to asynchronously update missing indices for sequences: If query does not have seeds in index for certain sequence and needs to compute them, after the query the index will be updated. Do not stage asynchronous persistence work. Grouped MPF index (default) does not implement this feature and using it with this option enabled will produce an error. Additionally you need to enable bioseq.index.recompute_async_persistence.enabled as well.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Allow the limited ordinary-MPS submission path when its other requirements are met.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.index.recompute_async_persistence.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Separate setting, but effectively to enable async persistence you need to enable bioseq.index.async_persistence.enabled together with this one. Same as bioseq.index.async_persistence.enabled Set bioseq.index.async_persistence.enabled to the same value as this one.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Same as bioseq.index.async_persistence.enabled</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.index.memory_cap_mb</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>0</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…2147483647</span></p>
<p>MiB</p></td>
<td style="text-align: left;">Specialized limit on the memory allocated by index builders/readers in MB. Using 0 means that they share the same memory budget as other query buffers.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.index.sequence_minimizer_sort.algorithm</code></p>
<p><em>hidden</em></p></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>std_sort</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Change sorting algorithm used for non-grouped MPF index builds Use the standard-library sort SIMD versions weren’t tested by me in terms of correctness and they were slower than the default</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>simd_u128_vqsort</code></td>
<td style="text-align: left;">Use Highway VQSort on packed 128-bit keys (implementation from C++ Highway repo).</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>simd_u128_vec_stream_1</code></td>
<td style="text-align: left;">Use the custom SIMD sorter with one vector stream.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>simd_u128_vec_stream_2</code></td>
<td style="text-align: left;">Use the custom SIMD sorter with two vector streams.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>simd_u128_vec_stream_4</code></td>
<td style="text-align: left;">Use the custom SIMD sorter with four vector streams.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.index_build.run_buffer_bytes</code></p>
<p><em>hidden, superuser</em></p></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>0</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…2147483647</span></p>
<p>bytes</p></td>
<td style="text-align: left;">For MPS overrides the external-sort run buffer size. Zero computes a size from the common BioseqDB memory budget.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.fragment_minimizer.lookup_route</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>direct_compact_hash_lookup</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Changes how grouped MPF works when reading is performed By default read hash directly from the hash bucket. The directory row should be faster in theory, but it’s disabled, because I’m not sure if it’s correct</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>control_block_scan</code></td>
<td style="text-align: left;">Search the directory row for the hash bucket first</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.grouped_soa_generation.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Selects if we want to use grouped MPF version of index or non-grouped one (for writing only) Prefer grouped version of MPF index over non-grouped (for writing only) Changing this does not rewrite existing indexes, only applies to new ones and does not affect reading!</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Use only non-grouped MPF when possible (for writing only)</td>
</tr>
</tbody>
</table>

## Minimizers and seed filtering

*Minimizers and seed filtering settings.*<a id="tab-configuration-minimizer"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.minimizer.kmer_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>15</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…64</span></p>
<p>symbols</p></td>
<td style="text-align: left;">Sets the number of sequence symbols in each k-mer Per search: <code>kmer_size</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.minimizer.window_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>10</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…1024</span></p>
<p>k-mers</p></td>
<td style="text-align: left;">Sets the number of consecutive k-mers in one minimizer window Per search: <code>window_size</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.minimizer.algorithm</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><strong><code>bounded_nthash_simd</code></strong></p>
<p>code 4</p>
<p>(default)</p></td>
<td style="text-align: left;">Chooses how minimizers are calculated Use the bounded SIMD BioseqDB algorithm Changing the algorithm can invalidate compatibility with saved indexes! Per search: <code>minimizer_algorithm</code>.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>scalar</code></p>
<p>code 0</p></td>
<td style="text-align: left;">Minimap2 scalar algorithm</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>simd_minimap2_reference</code></p>
<p>code 1</p></td>
<td style="text-align: left;">Use the naive version of SIMD implementation of minimap2’s</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>two_stack_nthash</code></p>
<p>code 2</p></td>
<td style="text-align: left;">Use rolling ntHash and two-stack window-minimum inspired by simd-minimizers paper.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>vulkan_minimap_integer</code></p>
<p>code 3</p></td>
<td style="text-align: left;">Use BioseqDB GPU algorithm</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.seeding.maximum_occurrence</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>500</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…16777216</span></p>
<p>positions</p></td>
<td style="text-align: left;">Excludes minimizer keys with more target occurrences than this limit. Per search: <code>max_occ</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.seeding.intermediate_occurrence_threshold</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>500</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…16777216</span></p>
<p>positions</p></td>
<td style="text-align: left;">Currently unused Per search: <code>mid_occ</code>.</td>
</tr>
</tbody>
</table>

## Chaining

*Chaining settings.*<a id="tab-configuration-chaining"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_gap</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>5000</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…1073741824</span></p>
<p>positions</p></td>
<td style="text-align: left;">Sets the requested maximum anchor gap used by chaining. Scalar (minimap2)chaining raises this limit to at least the bandwidth value. Per search: <code>max_gap</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_reference_gap</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>-1</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">−1…1073741824</span></p>
<p>positions</p></td>
<td style="text-align: left;">Currently unused Per search: <code>max_gap_ref</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.bandwidth</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>500</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…1048576</span></p>
<p>positions</p></td>
<td style="text-align: left;">Despite the name it’s also used by aligner stage, limits maximum distane to diagonal. Per search: <code>bandwidth</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.long_join_bandwidth</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>0</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1048576</span></p>
<p>positions</p></td>
<td style="text-align: left;">Currently unused. Per search: <code>bandwidth_long</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_skipped_anchors</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>25</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1048576</span></p></td>
<td style="text-align: left;">Limit of consecutive updates for anchors that do not improve score (only for supported algorithms like scalar) Per search: <code>max_chain_skip</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_predecessors</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>5000</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…4194304</span></p></td>
<td style="text-align: left;">Limits how many predecessors chainer can consider, but scalar chaining (depending on the actual algorithm) can examine one additional candidate. RMQ does not use this field. Per search: <code>max_chain_iter</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.minimum_seed_count</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>3</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1048576</span></p></td>
<td style="text-align: left;">Sets the minimum seed count for an accepted chain. Per search: <code>min_seed_count</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.minimum_score</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>40</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1048576</span></p></td>
<td style="text-align: left;">Sets the minimum accepted chain score (the score is computed by the selected chaining algorithm) Per search: <code>min_chain_score</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.gap_penalty_scale</code></td>
<td style="text-align: left;">real</td>
<td style="text-align: left;"><p><span><strong><code>0.8</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…100</span></p></td>
<td style="text-align: left;">Scales the gap penalty used by chaining (larger values penalize that contribution more strongly) Per search: <code>chain_gap_scale</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.skip_penalty_scale</code></td>
<td style="text-align: left;">real</td>
<td style="text-align: left;"><p><span><strong><code>0.0</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…100</span></p></td>
<td style="text-align: left;">Scales the chaining skip-distance penalty. Per search: <code>chain_skip_scale</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.chaining.algorithm</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><strong><code>simd_minimap2</code></strong></p>
<p>code 1</p>
<p>(default)</p></td>
<td style="text-align: left;">Selects algorithm used for chaining BioseqDB implementation of accelerated minimap2-inspired chaining Selected algorithm might still be rejected if the hardware does not support it Per search: <code>chaining_algorithm</code>.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>scalar_minimap2</code></p>
<p>code 0</p></td>
<td style="text-align: left;">Minimap2 scalar chaining</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>range_minimum_query</code></p>
<p>code 2</p></td>
<td style="text-align: left;">BioseqDB RMQ on AVL trees similar to what minimap2 uses</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>vulkan_tiled_dynamic_programming</code></p>
<p>code 3</p></td>
<td style="text-align: left;">Use tiled GPU chaining</td>
</tr>
</tbody>
</table>

## Primary and secondary alignments

*Primary and secondary alignments settings.*<a id="tab-configuration-secondary"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.secondary.overlap_threshold</code></td>
<td style="text-align: left;">real</td>
<td style="text-align: left;"><p><span><strong><code>0.5</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1</span></p></td>
<td style="text-align: left;">Sets the primary/secondary classification thresholds Per search: <code>mask_level</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.secondary.uncovered_length_limit</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>2147483647</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…2147483647</span></p>
<p>symbols</p></td>
<td style="text-align: left;">Limits the uncovered query length used when classifying an alignment as secondary to an existing primary. Per search: <code>mask_len</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.secondary.score_ratio_threshold</code></td>
<td style="text-align: left;">real</td>
<td style="text-align: left;"><p><span><strong><code>0.8</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1</span></p></td>
<td style="text-align: left;">Sets the secondary-to-primary score-ratio threshold. Zero bypasses this filtering step, including its secondary-count limit. Per search: <code>pri_ratio</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.secondary.maximum_count</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>5</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…1024</span></p></td>
<td style="text-align: left;">Limits retained secondary alignments across the complete list passed to the filtering stage Per search: <code>best_n</code>.</td>
</tr>
</tbody>
</table>

## Base alignment

*Base alignment settings.*<a id="tab-configuration-scoring"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.match_score</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>2</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…127</span></p></td>
<td style="text-align: left;">Sets the score added for an aligned matching symbol. Per search: <code>match_score</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.mismatch_penalty</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>4</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…127</span></p></td>
<td style="text-align: left;">Sets the score penalty for an aligned mismatch. Per search: <code>mismatch_pen</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.short_gap_open_penalty</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>4</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…127</span></p></td>
<td style="text-align: left;">Sets the first gap-opening penalty. KSW2 and BlockAligner use this pair. BlockAligner requires positive opening and extension penalties whose sum is at most 128. Per search: <code>gap_open1</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.short_gap_extension_penalty</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>2</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…127</span></p></td>
<td style="text-align: left;">Sets the first gap-extension penalty. BlockAligner requires it to be positive and its sum with the first opening penalty to be at most 128. Per search: <code>gap_ext1</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.long_gap_open_penalty</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>24</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…127</span></p></td>
<td style="text-align: left;">Sets KSW2’s second gap-opening penalty. Native BlockAligner uses only the first gap-penalty pair. Per search: <code>gap_open2</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.long_gap_extension_penalty</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>1</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…127</span></p></td>
<td style="text-align: left;">Sets KSW2’s second gap-extension penalty. Native BlockAligner does not use this second pair. Per search: <code>gap_ext2</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.ambiguous_base_penalty</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>1</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…127</span></p></td>
<td style="text-align: left;">Sets the alignment penalty for ambiguous symbols on paths that support them. Per search: <code>sc_ambi</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.z_drop_threshold</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>400</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">−1…1048576</span></p></td>
<td style="text-align: left;">Sets KSW2’s Z-drop threshold or BlockAligner’s X-drop threshold for end extensions. BlockAligner extensions require a nonnegative value. Per search: <code>zdrop</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.scoring.end_bonus</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>-1</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">−1…1048576</span></p></td>
<td style="text-align: left;">Sets KSW2’s sequence-end bonus. Negative one disables the bonus. Native BlockAligner does not use it. Per search: <code>end_bonus</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.block_alignment.minimum_block_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>32</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">8…4096</span></p></td>
<td style="text-align: left;">Sets BlockAligner’s minimum adaptive block size. (block sizes must be powers of two and the minimum cannot exceed the maximum) Per search: <code>block_min_size</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.block_alignment.maximum_block_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>256</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">8…16384</span></p></td>
<td style="text-align: left;">Sets BlockAligner’s maximum adaptive block size. (block sizes must be powers of two and the minimum cannot exceed the maximum) Per search: <code>block_max_size</code>.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.base_alignment.algorithm</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><strong><code>block_aligner</code></strong></p>
<p>code 1</p>
<p>(default)</p></td>
<td style="text-align: left;">Selects the base alignment algorithm Adaptive block aligner Per search: <code>base_alignment_algorithm</code>.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><p><code>ksw2</code></p>
<p>code 0</p></td>
<td style="text-align: left;">Use KSW2 aligner</td>
</tr>
</tbody>
</table>

## Vulkan execution

*Vulkan execution settings.*<a id="tab-configuration-vulkan"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enables GPU execution Allow supported GPU work GPU algorithms currently are not optimised and severly increase the query time. Enabling execution doesn’t mean the GPU will be used (for example software-emulated GPUs are disabled by default). The correct GPU algorithm also need to be selected by setting respective GUCs for aligner/chaining. Per search: <code>vulkan_enabled</code>.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">Setting aligner/chaining algorithms (or other) to Vulkan will immediately fail.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.seed_lookup.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enabled GPU-acclelerated minimizers (reading) Disable GPU for reading minimizer indexes Per search: <code>vulkan_seed_hits_enabled</code>.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Enable GPU minimizer lookup (but other GPU settings apply)</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.index_cache.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>on</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Cache minimizer index data to be reused across GPU calls (applies only to single pipeline execution and transfers between GPU/CPU memory) Save buffer with minimizers Per search: <code>vulkan_index_cache_enabled</code>.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>off</code></td>
<td style="text-align: left;">GPU doesn’t reuse minimizers during computation</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.fused_frontend.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enable experimental GPU fused kernel (this doesn’t work correctly) Leave this frontend unselected.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Enable special optimisation for GPU kernels</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.index_build.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enabled GPU-acclelerated minimizers (writing) Disable GPU for wrikting minimizer indexes</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Enable GPU minimizer index building (but other GPU settings apply)</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.alignment_extension.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enabled CIGAR computation on GPU Disbable CIGAR computation on GPU</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Allow computing CIGAR on GPU (but other GPU settings apply)</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.ordered_sketch_tile_bp</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>2048</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…16777216</span></p>
<p>symbols</p></td>
<td style="text-align: left;">Currently unused</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.radix_sort_digit_bits</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>8</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">1…16</span></p>
<p>bits</p></td>
<td style="text-align: left;">Currently unused</td>
</tr>
</tbody>
</table>

## Diagnostics

*Diagnostics settings.*<a id="tab-configuration-diagnostics"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><p><code>bioseq.diagnostics.logging.enabled</code></p>
<p><em>superuser</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enable or dissable internal logging. BioseqDB internally supports logging to file, but current builds are all configured to use only postgres logging mechanisms. Disable internal logger.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Enable internal logger.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.planner.diagnostics.log_level</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>debug3</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Chooses the PostgreSQL severity for planner diagnostic messages (logging levels are Postgres specific). Emit them at DEBUG3 severity. PostgreSQL’s message thresholds determine where the messages appear.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>debug5</code></td>
<td style="text-align: left;">Emit them at DEBUG5 severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>debug4</code></td>
<td style="text-align: left;">Emit them at DEBUG4 severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>debug2</code></td>
<td style="text-align: left;">Emit them at DEBUG2 severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>debug1</code></td>
<td style="text-align: left;">Emit them at DEBUG1 severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>log</code></td>
<td style="text-align: left;">Emit them at LOG severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>info</code></td>
<td style="text-align: left;">Emit them at INFO severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>notice</code></td>
<td style="text-align: left;">Emit them at NOTICE severity.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>warning</code></td>
<td style="text-align: left;">Emit them at WARNING severity.</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq.aligner.profiling.enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enables internal profiling of the pipeline execution profiling measures statistics like number of minimizers/chains/timings per pipeline stage etc.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Profiling can increase execution time</td>
</tr>
</tbody>
</table>

## Optional query-result cache

*Optional query-result cache settings.*<a id="tab-configuration-cache"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>bioseq_cache.query_cache.mode</code></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>disabled</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Enables experimental query result cache (provided by BioseqDB bioseq_cache subextension) Do not use the query result cache (disabled due to my low confidence if it works correctly) Any mode other than ’disabled’ required Postgres shared libraries reload.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>exact_query</code></td>
<td style="text-align: left;">Reuse completed identical requests and coordinate concurrent identical computations.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>reusable_fragments</code></td>
<td style="text-align: left;">Permit completed-fragment reuse for providers that support it. The Bioseq alignment adapter currently supports exact reuse only (this mode is not implemented)</td>
</tr>
<tr>
<td style="text-align: left;"><code>bioseq_cache.query_cache.completed_result_ttl_ms</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><span><strong><code>3600000</code></strong></span></p>
<p>(default)</p>
<p><span class="math inline">0…2147483647</span></p>
<p>ms</p></td>
<td style="text-align: left;">Maximum age of the records in the bioseq_cache tables that can be reused</td>
</tr>
</tbody>
</table>

## Controls present only in test builds

*Settings available only in test builds.*<a id="tab-configuration-testing"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>GUC name</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>GUC values</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><p><code>bioseq.testing.chunked_scanner.enabled</code></p>
<p><em>hidden, superuser</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Forces BioseqDB to load entire sequence into memory and create ’fake’ lazy-loading interface that accesses it. Used by me to test C++ interface for lazy-loading. Use the normal scanner.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Force load entire sequence into memory and create lazy-loader interface for that RAM sequence.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.testing.in_memory_scanner.enabled</code></p>
<p><em>hidden, superuser</em></p></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><p><span><strong><code>off</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Forces BioseqDB to load entire sequence into memory and passes it directly to the pipeline. (Note that this is the same for user as bioseq.testing.chunked_scanner.enabled with the only difference being internally used C++ API). Used by me to test C++ interface for lazy-loading. Use the normal scanner.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>on</code></td>
<td style="text-align: left;">Force load entire sequence into memory and pass it directly to the pipeline.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bioseq.testing.persisted_index.failure_mode</code></p>
<p><em>hidden, superuser</em></p></td>
<td style="text-align: left;">enum</td>
<td style="text-align: left;"><p><span><strong><code>none</code></strong></span></p>
<p>(default)</p></td>
<td style="text-align: left;">Cause ’fake’ index reader failure to be reported during queries. Do nothing.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>unsupported_vector_shape</code></td>
<td style="text-align: left;">Make the index reader report an unsupported vector shape (due to wrong w/k etc. parameters used during index build).</td>
</tr>
</tbody>
</table>

## Per-search options

<a id="sec-appendix-search-options"></a> <a id="sec-appendix-options-precedence"></a>

Additionally instead of operating on global settings, user can also provide single object as third parameter to the `mmsearch` function call.

> [!NOTE]
> **Parameter encoding**
>
> Currently the enum options are encoded by integers - they correspond to the global GUCs, but you can use only numbers. This is just unimplemented feature - the types of the options should be changed to strings and encoded to internal enum int values. It was requiring less effort to expose internal representation, so it was left as is, see listing below [configuration options](../project_configuration_reference/#lst-configuration-options).

Encoding of parameters as integer values:

<a id="lst-configuration-options"></a>

```sql runnable id=project_lst-configuration-options timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
-- bioseq:visible
SELECT (public.aligner_options(
  minimizer_algorithm => 0,
  index_read_requirement => 1)).*;
-- bioseq:teardown
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Passing options by 3rd argument of mmsearch

Every options is available below, for meaning and usage check the GUC tables:

*All nullable alignment options (for `aligner_options` call) in the order as specified in the SQL API*<a id="tab-configuration-options"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>Options field</strong></th>
<th style="text-align: left;"><strong>SQL type</strong></th>
<th style="text-align: left;"><strong>Inherited GUC or independent policy</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>kmer_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.minimizer.kmer_size</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>window_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.minimizer.window_size</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>max_gap</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_gap</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>max_gap_ref</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_reference_gap</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>bandwidth</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.bandwidth</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>bandwidth_long</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.long_join_bandwidth</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>max_chain_skip</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_skipped_anchors</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>max_chain_iter</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.maximum_predecessors</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>min_seed_count</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.minimum_seed_count</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>min_chain_score</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.minimum_score</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>chain_gap_scale</code></td>
<td style="text-align: left;">double</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.gap_penalty_scale</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>chain_skip_scale</code></td>
<td style="text-align: left;">double</td>
<td style="text-align: left;"><code>bioseq.aligner.chaining.skip_penalty_scale</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>mask_level</code></td>
<td style="text-align: left;">double</td>
<td style="text-align: left;"><code>bioseq.aligner.secondary.overlap_threshold</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>mask_len</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.secondary.uncovered_length_limit</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>pri_ratio</code></td>
<td style="text-align: left;">double</td>
<td style="text-align: left;"><code>bioseq.aligner.secondary.score_ratio_threshold</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>best_n</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.secondary.maximum_count</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>match_score</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.match_score</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>mismatch_pen</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.mismatch_penalty</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>gap_open1</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.short_gap_open_penalty</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>gap_ext1</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.short_gap_extension_penalty</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>gap_open2</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.long_gap_open_penalty</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>gap_ext2</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.long_gap_extension_penalty</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>sc_ambi</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.ambiguous_base_penalty</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>zdrop</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.z_drop_threshold</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>end_bonus</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.scoring.end_bonus</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>max_occ</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.seeding.maximum_occurrence</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>mid_occ</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.seeding.intermediate_occurrence_threshold</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>block_min_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.block_alignment.minimum_block_size</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>block_max_size</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><code>bioseq.aligner.block_alignment.maximum_block_size</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>chaining_algorithm</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><code>bioseq.aligner.chaining.algorithm</code></p>
<p>Integer codes should be checked in the GUC table.</p></td>
</tr>
<tr>
<td style="text-align: left;"><code>base_alignment_algorithm</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><code>bioseq.aligner.base_alignment.algorithm</code></p>
<p>Integer codes should be checked in the GUC table.</p></td>
</tr>
<tr>
<td style="text-align: left;"><code>minimizer_algorithm</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><code>bioseq.aligner.minimizer.algorithm</code></p>
<p>Integer codes should be checked in the GUC table.</p></td>
</tr>
<tr>
<td style="text-align: left;"><code>vulkan_enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.enabled</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>vulkan_seed_hits_enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.seed_lookup.enabled</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>vulkan_index_cache_enabled</code></td>
<td style="text-align: left;">bool</td>
<td style="text-align: left;"><code>bioseq.aligner.vulkan.index_cache.enabled</code></td>
</tr>
<tr>
<td style="text-align: left;"><code>index_read_requirement</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;">No GUC. See Table <a href="../project_configuration_reference/#tab-configuration-index-requirement">configuration index requirement</a>.</td>
</tr>
</tbody>
</table>

### The index-read requirement

<a id="sec-appendix-options-index-policy"></a>

This is special field that has no GUC equivalent, that is only available via `aligner_options`. This is special flag very useful during testing and controls how index reader behaves.

*Special option controling index reader*<a id="tab-configuration-index-requirement"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>Options field</strong></th>
<th style="text-align: left;"><strong>Type</strong></th>
<th style="text-align: left;"><strong>Value</strong></th>
<th style="text-align: left;"><strong>Description</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><code>index_read_requirement</code></td>
<td style="text-align: left;">int</td>
<td style="text-align: left;"><p><strong>0: <code>allow</code></strong></p>
<p>(default)</p></td>
<td style="text-align: left;">If no compatible index is available, seeds can be recomputed from scratch. This is the most useful in practical sense as it allows the largest flexibility for user.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;">: <code>require_absent</code></td>
<td style="text-align: left;">Never read index even if it exists, always recompute seeds from scratch. Additionally setting this option will produce errors if deffered index update is ON. Practically used in testing scenarios to see what happens if index does not exist.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"></td>
<td style="text-align: left;">: <code>require_compatible_persisted</code></td>
<td style="text-align: left;">Hard requirement that index must allways exist. If the index does not exist or is incompatible (different k/w parameters etc.), the query will produce error before execution.</td>
</tr>
</tbody>
</table>

<a id="sec-configuration-reference-end"></a>
