---
title: "Benchmarking and performance evaluation"
slug: project_benchmarking
description: "Benchmarking and performance evaluation"
order: 120
keywords: [BioseqDB, project]
draft: false
---

## Benchmarking and performance evaluation

<a id="sec-final-bioseqdb-benchmarking"></a>

The following chapter contains interpretation of the benchmarking results and then raw table listings with all timings.

### What the results show

<a id="sec-benchmark-interpretation"></a>

BioseqDB was evaluated on JOIN between query and reference tables where reference table contained only one sequence and reference table contained 1024 randomly generated sequences. The cases where divided by me into two groups:

- **Index:** build the reference index first, restart PostgreSQL, then measure the query, including access to the stored index.

- **No index:** measure construction of a temporary reference index and the query together. The query batch shares that temporary index.

I measured two times: A and B, where A (always top number) represents total processing time and B represents the sum of time with excluded CIGAR/MAPQ work. For heavy parallelisation this doesn’t mean that B was exactly equal to all processing time for seeding, chaining till final alignment, but it’s good measurement to validate where the majority of work can be spent. In the future I plan to measure exactly which stages take the longest time with better instrumentation.

The full tables below present only specific cell values, because the code for benchmarking selected tuples in non-deterministic order depending on processing speeds. The most important benchmarking values were still collected as main points of my interest were comparisons between DNA4/DNA5, different lengths, different k/w parameters and some configurations wheren’t optimised in reality (SIMD-ref role as naive SIMD scan implementation for example was to see how this performs in practice).

Overall the main outcomes are as follows:

1.  BioseqDB is the first tool to my knowing that integrates database with aligner software, so benchmarks are first of this kind.

2.  Memory admission system and custom grouped-MPF index allow to achieve speedups for already built index by 3x to 8x compared to minimap2 in typical cases and 100x where heavy swapping occurs for long sequences

3.  BioseqDB for preexisting index beats all common aligners and doesn’t require any data preparation (other tools required customized scripts to split the long sequences into chunks)

4.  BioseqDB without index is usually slower than minimap2, but faster than other similar aligner tools (this is acceptable due to nature of the typical use cases)

5.  BisoeqDB exposed failures in terms of very inneficient GPU implementation and anomalous implementations of some of other coinfigurations (like scalar)

6.  External aligners excluding BLAST, required me to split sequences beforehand and some of the splits were relatively small, meaning that the job for the tool (Winnowmap) was significantly easier. This means that BioseqDB is only capable solution outside of BLAST that can directly load very large sequences. When reading tables please note that this preprocessing bias the measured times for other aligners.

#### Problems with indexed comaprison for minimizers

<a id="sec-bench-indexed-minimizers"></a>

Scalar and SIMD-ref have similar indexed times in this example, where they reuse the same kind of reference index and share most of the query pipeline.

<a id="tab-bench-indexed-minimizers"></a>

<div class="project-benchmark" data-table="aws-2-k15-w10" data-rows="mpf-scalar-scalar-block,mpf-simdref-scalar-block" data-columns="index-2-k15-w10-dna4-m7a,index-2-k15-w10-dna4-m7i"></div>

Two minimizer implementations with MPF, Scalar-DP and Block held fixed.

[Benchmark results](/benchmarks/?table=aws-2-k15-w10)

> [!WARNING]
> **An indexed row does not measure reference-index construction**
>
> The benchmark requires a compatible persisted *reference* index. It builds that index before restarting PostgreSQL, and does not rebuild its minimizers inside the measured query. The query sequences themselves have no persisted minimizer index: their minimizers are extracted to search the reference. Changing the minimizer therefore does not measure reference construction again, so the results above measure the same thing for the "index" case.

In each small table, the upper duration is A: the complete query measurement, and the lower duration is B: the A time with substracted time for recorded CIGAR/MAPQ. This means that in BioseqDB B represents all the time excluding final alignment. You can find complete raw result tables in Benchmark results Section [benchmark results](../project_benchmarking/#sec-benchmark-results).

The measurements were performed on AWS machines of various classes always of size xlarge and local machine with MacOS ARM64. The exact specifications and measurement methotolopgy can be found in the benchmark section as well.

The minimap2 baseline is always gray in the tables, while preferred configuration (grouped-MPF with all SIMD routes) is marked with yellow colour.

#### Paying for the reference once

<a id="sec-bench-index-benefit"></a>

The preferred BioseqDB configuration beats minimap2 when the reference index is available, but loses when index construction is included in this example.

<a id="tab-bench-index-benefit"></a>

<div class="project-benchmark" data-table="aws-2-k15-w10" data-rows="minimap2,mpf-bounded-simd-block" data-columns="index-2-k15-w10-dna4-m7i,noindex-2-k15-w10-dna4-m7i"></div>

The preferred configuration on M7I, with and without a prepared reference index.

[Benchmark results](/benchmarks/?table=aws-2-k15-w10)

BioseqDB can then read its prepared lookup structures, as shown in Figures [index join read paths](../project_index_storage/#fig-index-join-read-paths) and [index selective load](../project_index_storage/#fig-index-selective-load), which is great success. User can usually spend more time once waiting for data load to benefit from faster queries.

#### Specialized kernel failure

<a id="sec-bench-wide-hash"></a>

The scalar MPF configuration takes much longer on the smaller reference with longer k-mers than on the larger reference with shorter k-mers.

<a id="tab-bench-short-hash"></a>

<div class="project-benchmark" data-table="aws-4-k15-w10" data-rows="mpf-scalar-scalar-block,mpf-bounded-simd-block" data-columns="index-4-k15-w10-dna4-m7a,index-4-k15-w10-dna4-m7i"></div>

Indexed DNA4 with the shorter k-mer profile.

[Benchmark results](/benchmarks/?table=aws-4-k15-w10)

<a id="tab-bench-wide-hash"></a>

<div class="project-benchmark" data-table="aws-2-k20-w15" data-rows="mpf-scalar-scalar-block,mpf-bounded-simd-block" data-columns="index-2-k20-w15-dna4-m7a,index-2-k20-w15-dna4-m7i"></div>

Indexed DNA4 with the longer k-mer profile, despite a smaller reference.

[Benchmark results](/benchmarks/?table=aws-2-k20-w15)

Smaller hashes can be directly fetched from MPF index, while for longer the directory row is used [index logical layouts](../project_index_storage/#sec-index-logical-layouts) [index cold loading](../project_index_storage/#sec-index-cold-loading), this however doesn’t explain the difference. One possible explanation is that hash for scalar is too wide and it forces these lookups, because bounded SIMD algorithm limits the width of hashes. Alternatively this could be a problem with how the algorithm uses memory admission system and going into SWAP usage, but such drastic change is unlikely. What I think is the most likely, is the k=15, w=10 has certain optimisations I hardcoded first. When migrating algorithms to be generic I could do it in a way that the performance is heavily deraded for non-default caase.

Nevertheless I’m still accepting these results as k=15, w=10 are the most common (and default minimap2) used parameters.

#### RMQ/SIMD chaining performance

<a id="sec-bench-chaining-choice"></a>

The preferred SIMD configuration is competitive, but the fastest chaining choice changes with the testing case and the machine. Suprisingly on MacOS where the most of the development was done SIMD-DP is the fastest one. I didn’t measure region-specific timings (forward chaining/backward chaining) in this benchmark, so in the future I could spend more time investigating how algorithms compare in terms of memory accesses and other datasets.

<a id="tab-bench-chaining-small"></a>

<div class="project-benchmark" data-table="aws-2-k15-w10" data-rows="mpf-bounded-scalar-block,mpf-bounded-simd-block,mpf-bounded-rmq-block" data-columns="index-2-k15-w10-dna4-m7a,index-2-k15-w10-dna4-m7i"></div>

Chaining choices with MPF, bounded minimizers and Block fixed.

[Benchmark results](/benchmarks/?table=aws-2-k15-w10)

<a id="tab-bench-chaining-large"></a>

<div class="project-benchmark" data-table="aws-4-k15-w10" data-rows="mpf-bounded-scalar-block,mpf-bounded-simd-block,mpf-bounded-rmq-block" data-columns="index-4-k15-w10-dna4-m7a,index-4-k15-w10-dna4-m7i"></div>

The same choices on the larger DNA4 reference.

[Benchmark results](/benchmarks/?table=aws-4-k15-w10)

<a id="tab-bench-chaining-local"></a>

<div class="project-benchmark" data-table="local" data-rows="mpf-bounded-scalar-block,mpf-bounded-simd-block,mpf-bounded-rmq-block" data-columns="index-2-k20-w15-dna4-local"></div>

A local case in which SIMD-DP wins and RMQ loses.

[Benchmark results](/benchmarks/?table=local)

#### Ambiguous bases problem

<a id="sec-bench-dna5"></a>

The local DNA5 the performance is much lower for the measurement without index than in its index measurement.

<a id="tab-bench-dna5"></a>

<div class="project-benchmark" data-table="local" data-rows="mpf-bounded-simd-block" data-columns="index-2-k15-w10-dna4-local,index-2-k15-w10-dna5-local,noindex-2-k15-w10-dna4-local,noindex-2-k15-w10-dna5-local"></div>

The preferred local configuration on DNA4 and DNA5.

[Benchmark results](/benchmarks/?table=local)

"N" bases can be troublesome as the bit-packing representation if less-performant as padding for bits occupy more space. This also means that usual SIMD instructions needs extra bitshifts to extract the bits.

#### Anomalous A/B difference

<a id="sec-bench-a-b"></a>

This MPS configuration loses to minimap2 on complete time even though its B time is lower.

<a id="tab-bench-a-b"></a>

<div class="project-benchmark" data-table="aws-2-k20-w15" data-rows="minimap2,mps-scalar-simd-block" data-columns="index-2-k20-w15-dna4-m7a,index-2-k20-w15-dna4-m7i"></div>

An indexed comparison whose direction changes between A and B.

[Benchmark results](/benchmarks/?table=aws-2-k20-w15)

I previously had problem with implementation of divergence-sort in Section [minimizer divergence history](../project_minimizers/#sec-minimizer-divergence-history) but divergence is not substracted in B. Such big difference may be caused by either problems with measurement methodology (because alignment is massively parallel and calculating B is not correct) or most of the slowdown happens in CIGAR work due to special treatment of N bases in DNA5.

#### Block aligner is usually faster than KSW2

<a id="sec-bench-base-aligner"></a>

Block Aligner improves complete time compared to KSW2.

<a id="tab-bench-base-aligner"></a>

<div class="project-benchmark" data-table="aws-2-k15-w10" data-rows="mpf-bounded-simd-block,mpf-bounded-simd-ksw2" data-columns="index-2-k15-w10-dna4-m7a,index-2-k15-w10-dna4-m7i,index-2-k15-w10-dna4-m4"></div>

Changing only the requested base aligner in the preferred configuration.

[Benchmark results](/benchmarks/?table=aws-2-k15-w10)

Block Aligner follows an adaptive block through the scoring matrix, whereas KSW2 uses its banded dynamic programming and stopping rules [final alignment cpu contracts](../project_final_alignment_postprocessing_and_preprocessing/#sec-final-alignment-cpu-contracts).

#### MPF is preffered over MPS

<a id="sec-bench-index-layout"></a>

MPF is both faster to query and faster to construct.

<a id="tab-bench-index-layout"></a>

<div class="project-benchmark" data-table="aws-2-k15-w10" data-rows="mpf-bounded-simd-block,mps-bounded-simd-block" data-columns="index-2-k15-w10-dna4-m7a,noindex-2-k15-w10-dna4-m7a"></div>

MPF and MPS with the same minimizer, chaining and alignment choices.

[Benchmark results](/benchmarks/?table=aws-2-k15-w10)

This happens because MPS overall offers no benefit because it construction requires merge sort with potential disk spilling and querying it is more complicated than in case of MPF [index mpf mps](../project_index_storage/#fig-index-mpf-mps)

<a id="tab-bench-mps-dna5"></a>

<div class="project-benchmark" data-table="aws-4-k20-w15" data-rows="mpf-bounded-simd-block,mps-bounded-simd-block" data-columns="index-4-k20-w15-dna5-m7a,index-4-k20-w15-dna5-m7i"></div>

The large DNA5 MPS slowdown appears on several machine classes.

[Benchmark results](/benchmarks/?table=aws-4-k20-w15)

In the example here we can see that likely the k=20, w=15 contains more unique minimizers than k=15, w=10 so the disk spillage happened during measurement and the query needed to check more unique minimizers, which caused multiple random accesses to large index.

#### BioseqDB memory admission system

<a id="sec-bench-machine-pressure"></a>

The minimap2 baseline itself slows sharply on C7G.

<a id="tab-bench-machine-pressure"></a>

<div class="project-benchmark" data-table="aws-4-k20-w15" data-rows="minimap2,mpf-bounded-simd-block" data-columns="index-4-k20-w15-dna4-m7a,index-4-k20-w15-dna4-m7i,index-4-k20-w15-dna4-m4,index-4-k20-w15-dna4-c7g"></div>

The same indexed workload across the AWS machine classes.

[Benchmark results](/benchmarks/?table=aws-4-k20-w15)

The C7G minimap2 record reports severe paging during measurement. C7G has less physical RAM than the other AWS classes, so the index with more unique minimizers causes excessive paging.

M4 is slower in many comparisons, but is not uniformly the slowest machine. Its older CPU and lower EBS (AWS disk) bandwidth make it slow, but in cases like presented here C7G can also undergo reduced performance \[[149](../bibliography/#ref-aws_ebs_limits)\] The benefit of BioseqDB is that we activelly avoid using SWAP by making sure the memory is sufficient for all allocations of commom memory arenas.

#### Index query slower than no-index query

<a id="sec-bench-index-reversal"></a>

For this case of C7G measurement benchmark reported that indexed query is slower than no-index query.

<a id="tab-bench-index-reversal"></a>

<div class="project-benchmark" data-table="aws-2-k20-w15" data-rows="minimap2,mpf-simdref-simd-block" data-columns="index-2-k20-w15-dna4-c7g,noindex-2-k20-w15-dna4-c7g"></div>

The apparently reversed index benefit for MPF and SIMD-ref.

[Benchmark results](/benchmarks/?table=aws-2-k20-w15)

This means that BioseqDB requires further optimisation to enable adappitve decisioning whether to use index of not. In this case despited repeated measurements the machine could go period of temproarily reduced disk IOPS, which caused disk reads to be slower than computing index from scratch (in second case the pipeline doesn’t have to interface with disk as much)

#### Outliers

<a id="sec-bench-long-runs"></a>

Several wide-hash MPF were unable to complete in time.

<a id="tab-bench-long-runs"></a>

<div class="project-benchmark" data-table="aws-4-k20-w15" data-rows="mpf-scalar-scalar-block,mpf-simdref-rmq-block" data-columns="index-4-k20-w15-dna4-c7g"></div>

Examples of the longest indexed observations. Superscript s denotes a single sample.

[Benchmark results](/benchmarks/?table=aws-4-k20-w15)

It’s likely that this is a bug in memory admission system which stalled entire pipeline. Other machines didn’t report such bad timings.

#### GPU native algorithms are slower than CPU

<a id="sec-bench-gpu-performance"></a>

Unfortunatelly BioseqDB was my first GPU implementation in Vulkan. I tested it both on local machine with integrated GPU and on remote Vast.ai rented GPU machine. In both of the cases there were issues with allocating to much memory on GPU or using CPU host memory instead of GPU. It’s worth considering G3SA approach with different order of chaining scores computations and also consider one integerated kernel to perform all pipeline stages on GPU without frequent host-device transfers.

<a id="tab-bench-gpu-local"></a>

<div class="project-benchmark" data-table="local" data-rows="minimap2,mpf-vulkan-vulkan-block" data-columns="index-2-k15-w10-dna4-local,index-2-k20-w15-dna4-local"></div>

The local integrated GPU with Vulkan seeding and chaining.

[Benchmark results](/benchmarks/?table=local)

Initially my line of reasoning was to ignore my shortcomings when it comes to implementations in my code. Unfortunately, a GPU faster by several orders of magnitude didn’t translate into better performance.

<a id="tab-bench-gpu-vast"></a>

<div class="project-benchmark" data-table="vastai" data-rows="minimap2,mps-vulkan-vulkan-block" data-columns="index-2-k15-w10-dna4-vastai,index-2-k20-w15-dna4-vastai"></div>

The MPS Vulkan route and the same-host minimap2 baseline on an RTX 5090.

[Benchmark results](/benchmarks/?table=vastai)

#### GPU capacity failures

<a id="sec-bench-gpu-capacity"></a>

I tried putting parts of the MPS index directly on the GPU. However, the buffer size is limited and the current Vulkan code fails on the measured cases for 4 Gbp sequences.

<a id="tab-bench-gpu-capacity"></a>

<div class="project-benchmark" data-table="vastai" data-rows="mpf-vulkan-vulkan-block,mps-vulkan-vulkan-block" data-columns="index-4-k15-w10-dna4-vastai,index-4-k20-w15-dna4-vastai"></div>

Completed MPF observations and MPS preparation failures on the RTX 5090.

[Benchmark results](/benchmarks/?table=vastai)

### Benchmarking methodology

<a id="sec-benchmark-results"></a>

The following tables present results comparins BioseqDB and minimap2 on one Macos machine and 4 AWS machines.

- **Index.** A persisted index is constructed before measurement. Each repeat starts a fresh process and measures reading the index and mapping the queries. Construction of the persisted index is excluded. Operating-system page caches are not purged, so this is process-cold execution, but not a guaranteed cold disk read (I decided to measure it this way, because minimap2 still has a chance to use system page cache).

- **No index.** No pre-existing user index is available.

- **Two measurements per cell.** The first duration, A, is the nearest-rank P50 of the complete measured interval. The second, B, is the nearest-rank P50 after subtracting the union of instrumented CIGAR and MAPQ in each repeat

By default for benchmarks, the chunk cache for decompressed columns is disabled. Each test case runs 1,024 queries against one reference sequence:

- References contain exactly $2\times10^9$ or $4\times10^9$ base pairs. The parameter pairs are $(k,w)=(15,10)$ and $(20,15)$.

- Each query and target is randomly generated. DNA4 uses the same probability for each base (25%) and the DNA5 generator assigns probability 1% to N and 24.75% to each base.

- Query samples have randomly generated lengths with a mean of 20,000 bp and a standard deviation of 500 bp (before applying insertions and deletions), and each sample is reverse-complemented with probability 0.5.

- The substitution probability is 0.001 and for insertions/deletions it’s 0.0005.

- Each query originates from one known sampled reference interval and later substitutions/insertions/deletions are applied to it.

BioseqDB uses task-parallel (most parallel-friendly mode) execution with prefiltering disabled and both CIGAR and MAPQ projection enabled.

AWS machines were missing GPUs so all Vulkan rows are grouped together and marked as not available.

#### Reading comparisons and highlights.

Each cell places the A duration above its comparison, followed by the B duration and its own comparison.

- Minimap2 (light-gray), as the baseline, is the first row in each table.

- All of the rows apart from minimap2 are ordered top-to-bottom by the minimum A time for the given "index" configuration.

- A BioseqDB value is compared with minimap2 for the same configuration (green/red triangles), while the minimap2 row compares with the fastest measured eligible BioseqDB effective path in that same column.

- Pale yellow identifies the preferred BioseqDB configuration: MPF / Bounded / SIMD-DP / Block. This is the author’s (my) selected configuration.

- A dot means there is no measurement, while a horizontal dash with <sup>g</sup> indicates that the GPU configuration wasn’t available (no GPU on AWS machines).

### Complete raw results

*Machine specs* <a id="tab-benchmark-machines"></a>

| Property | Local | M7A | M7I | M4 | C7G |
|:---|:---|:---|:---|:---|:---|
| Machine | MacBook Pro | m7a.xlarge | m7i.xlarge | m4.xlarge | c7g.xlarge |
| CPU | Apple M1 Pro | AMD EPYC 9R14 | Xeon Platinum 8488C | Xeon E5-2686 v4 @ 2.30GHz | Neoverse-V1 |
| Architecture | arm64 | x86_64 | x86_64 | x86_64 | aarch64 |
| OS version / kernel | macOS 15.6 / Darwin 24.6.0 | Linux / 7.0.0-1012-aws | Linux / 7.0.0-1012-aws | Linux / 7.0.0-1012-aws | Linux / 7.0.0-1012-aws |
| OS build / distribution | 24G5054d | Distribution not recorded | Distribution not recorded | Distribution not recorded | Distribution not recorded |
| Logical / physical cores | 8 / 8 (6P + 2E) | 4 / 4 | 4 / 2 | 4 / 2 | 4 / 4 |
| Physical RAM | 16 GiB unified | 16 GiB | 16 GiB | 16 GiB | 8 GiB |
| GPU | Apple M1 Pro, 14 cores | None available | None available | None available | None available |
| Storage | APPLE SSD AP0512R | EBS gp3 | EBS gp3 | EBS gp3 | EBS gp3 |
| Disk capacity | 500.28 GB | 500 GiB | 500 GiB | 500 GiB | 500 GiB |
| Provisioned disk I/O | Not recorded | 6,000 IOPS / 500 MiB/s | 6,000 IOPS / 500 MiB/s | 6,000 IOPS / 500 MiB/s | 6,000 IOPS / 500 MiB/s |

<a id="tab-bench-local"></a>

<div class="project-benchmark" data-table="local"></div>

[Benchmark results](/benchmarks/?table=local)

Each cell: A and B times, $\cdot$: no measurement. —<sup>g</sup>: GPU unavailable. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

MPF/MPS: minimizers per fragment/sequence. SIMD-ref: naive SIMD scan. Bounded: bounded SIMD algorithm.

<a id="tab-bench-aws-4-k15-w10"></a>

<div class="project-benchmark" data-table="aws-4-k15-w10"></div>

[Benchmark results](/benchmarks/?table=aws-4-k15-w10)

Each cell: A and B times, $\cdot$: no measurement. —<sup>g</sup>: GPU unavailable. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

MPF/MPS: minimizers per fragment/sequence. SIMD-ref: naive SIMD scan. Bounded: bounded SIMD algorithm.

<a id="tab-bench-aws-4-k20-w15"></a>

<div class="project-benchmark" data-table="aws-4-k20-w15"></div>

[Benchmark results](/benchmarks/?table=aws-4-k20-w15)

Each cell: A and B times, $\cdot$: no measurement. —<sup>g</sup>: GPU unavailable. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

MPF/MPS: minimizers per fragment/sequence. SIMD-ref: naive SIMD scan. Bounded: bounded SIMD algorithm.

<a id="tab-bench-aws-2-k15-w10"></a>

<div class="project-benchmark" data-table="aws-2-k15-w10"></div>

[Benchmark results](/benchmarks/?table=aws-2-k15-w10)

Each cell: A and B times, $\cdot$: no measurement. —<sup>g</sup>: GPU unavailable. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

MPF/MPS: minimizers per fragment/sequence. SIMD-ref: naive SIMD scan. Bounded: bounded SIMD algorithm.

<a id="tab-bench-aws-2-k20-w15"></a>

<div class="project-benchmark" data-table="aws-2-k20-w15"></div>

[Benchmark results](/benchmarks/?table=aws-2-k20-w15)

Each cell: A and B times, $\cdot$: no measurement. —<sup>g</sup>: GPU unavailable. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

MPF/MPS: minimizers per fragment/sequence. SIMD-ref: naive SIMD scan. Bounded: bounded SIMD algorithm.

### Comparison with other aligners

<a id="sec-external-aligner-results"></a> The following adjustments were needed to run the compared aligners:

- **Minimap2.** A single FASTA record must fit in 32-bit integer, so I split the 4 Gbp reference into two records of about 2 Gbp each. Returned positions are converted back to coordinates in the original reference. The 2 Gbp reference didn’t require any spliting, so I left it as-is.

- **Winnowmap.** I re-enabled saving its native index and changed loading to rebuild the weighting Bloom filter from the saved list of frequent k-mers produced by Meryl (Meryl is the tool Winnomap uses for extracting k-mers). The 4 Gbp references used index batches of 2 Mbp, because otherwise Winnowmap failed even with sufficient SWAP. This means that Winnowmap had far better conditions to minimap2. Sadly comparison wasn’t possible otherwise, beccause I encountered repeatable memory errors.

- **blastn.** BLAST used sequences unchanged. It has no second branch and scores N symbol differently - as $-3$ rather than $-1$, but it had no impact on results.

- **LexicMap.** It uses 28-bit coordinates meaning that the sequences was required to be split into 268Mbp parts. On machines with less than 10 GiB RAM, the 4 Gbp reference was indexed one record per batch, because otherwise it produced an error. LexicMap by default replaces N with A in target, which also didn’t affect the quality of results.

<a id="tab-bench-external-4-k15-w10"></a> <a id="tab-external-aligners"></a>

<div class="project-benchmark" data-table="external-4-k15-w10"></div>

[Benchmark results](/benchmarks/?table=external-4-k15-w10)

Each cell: A, comparison, B, comparison. $\cdot$: no measurement. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

Comparisons use minimap2. The minimap2 row compares with BioseqDB.

<sup>\*</sup> Each algorithm except BLAST required intensive splitting, sometimes not equal. The splitting was the biggest possible to allow algorithm not to crash.

<sup>a</sup>BLAST scores N as -3, compared with -1 in minimap2.

<sup>b</sup>LexicMap stores reference N as A.

<sup>c</sup>The 4 Gbp reference is split into two input records.

<sup>d</sup>Winnowmap uses overlapping 250 Mb records and 499M index batches. Merging results can affect MAPQ.

<sup>e</sup>Winnowmap merges results from 1999M index batches.

<a id="tab-bench-external-4-k20-w15"></a>

<div class="project-benchmark" data-table="external-4-k20-w15"></div>

[Benchmark results](/benchmarks/?table=external-4-k20-w15)

Each cell: A, comparison, B, comparison. $\cdot$: no measurement. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

Comparisons use minimap2. The minimap2 row compares with BioseqDB.

<sup>\*</sup> Each algorithm except BLAST required intensive splitting, sometimes not equal. The splitting was the biggest possible to allow algorithm not to crash.

<sup>a</sup>BLAST scores N as -3, compared with -1 in minimap2.

<sup>b</sup>LexicMap stores reference N as A.

<sup>c</sup>The 4 Gbp reference is split into two input records.

<sup>d</sup>Winnowmap uses overlapping 250 Mb records and 499M index batches. Merging results can affect MAPQ.

<sup>e</sup>Winnowmap merges results from 1999M index batches.

<a id="tab-bench-external-2-k15-w10"></a>

<div class="project-benchmark" data-table="external-2-k15-w10"></div>

[Benchmark results](/benchmarks/?table=external-2-k15-w10)

Each cell: A, comparison, B, comparison. $\cdot$: no measurement. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

Comparisons use minimap2. The minimap2 row compares with BioseqDB.

<sup>\*</sup> Each algorithm except BLAST required intensive splitting, sometimes not equal. The splitting was the biggest possible to allow algorithm not to crash.

<sup>a</sup>BLAST scores N as -3, compared with -1 in minimap2.

<sup>b</sup>LexicMap stores reference N as A.

<sup>d</sup>Winnowmap uses overlapping 250 Mb records and 499M index batches. Merging results can affect MAPQ.

<sup>f</sup>Swap settings were not recorded.

<a id="tab-bench-external-2-k20-w15"></a>

<div class="project-benchmark" data-table="external-2-k20-w15"></div>

[Benchmark results](/benchmarks/?table=external-2-k20-w15)

Each cell: A, comparison, B, comparison. $\cdot$: no measurement. n/a: no comparison.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

Gray: minimap2. Yellow: my preferred MPF / Bounded / SIMD-DP / Block configuration.

Comparisons use minimap2. The minimap2 row compares with BioseqDB.

<sup>\*</sup> Each algorithm except BLAST required intensive splitting, sometimes not equal. The splitting was the biggest possible to allow algorithm not to crash.

<sup>a</sup>BLAST scores N as -3, compared with -1 in minimap2.

<sup>b</sup>LexicMap stores reference N as A.

### Vulkan on a Vast.ai GPU instance

<a id="sec-vastai-gpu-results"></a>

*Vast.ai machine specs* <a id="tab-vastai-machine"></a>

| Property      | Value                           |
|:--------------|:--------------------------------|
| CPU           | AMD EPYC 7282 16-Core Processor |
| CPU quota     | 7.68 cores                      |
| Memory limit  | 120.8 GiB                       |
| GPU           | NVIDIA GeForce RTX 5090         |
| GPU memory    | 32,607 MiB                      |
| OS            | Ubuntu 24.04                    |
| NVIDIA driver | 595.84                          |
| Vulkan API    | 1.4.329                         |

<a id="tab-vastai-gpu"></a> <a id="tab-bench-vastai"></a>

<div class="project-benchmark" data-table="vastai"></div>

[Benchmark results](/benchmarks/?table=vastai)

Each cell: A, comparison, B, comparison. Light gray = minimap2.

Blue: one lowest A and one lowest B measurement for each column for each configuration.

MPF/MPS = minimizers per fragment/sequence. Vulkan rows compare with minimap2 on the same machine.

$\cdot$ = no result. n/a: no comparison. <span class="project-color-brickred">$\bigcirc$</span> = setup rejected. <span class="project-color-brickred">$\times$</span> = execution failed.

<sup>1</sup>The GPU index exceeds the available memory budget.

<sup>2</sup>The MPS control buffer exceeds the Vulkan buffer limit.
