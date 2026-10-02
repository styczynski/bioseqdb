---
title: "Conclusions"
slug: project_conclusion
description: "Conclusions"
order: 130
keywords: [BioseqDB, project]
draft: false
---

<a id="chap-conclusion"></a>

There are several limitations to be addressed in the BioseqDB:

- Grouped-MPF does not correctly work for update on insert.

- Grouped-MPF uses woraround with writing a file to efficiently support joins.

- The data is fetched only in one Postgres process, which causes the one core of CPU to be bottlenecked.

- Some kernels contains probable mistakes that slows down processing for any cases than k=15,w=10 significantly.

- The pipeline code is overcomplicated and current type-driven template system causes the build to last up to 2 hours for optimsied builds on some machines.

- The storage is optimised only for large sequences, so short sequences overuse memory significantly.

- Buggy conditions cause minimizers to be recomputed instead of being loaded from the index.

- Index format contains too many fields of which most are not used and the loading code heavily relies of bitwise operations.

- Cache is experimental and didn’t go proper testing, so I have no confirmation if it works reliably.

- Benchmarking was done only on synthetic datasets instead of sequences from NCBI, which can mean that practical speedups are significantly lower.

- Currrent join logic assumes reuse of target fragments, which means that BioseqDB can handle large table vs. small table, but will be slow for multi-alignment (large table vs. large table)

- The error messages created by BioseqDB are hard to interpret and not very helpful to the user

- Compilation requires expiermental version of tools e.g latest CMake version with special flag enabled to build C++ std modules.

However, the project can be classified as a success:

- BioseqDB is first in literature aim to integrate existing database software with genomic toolset.

- BioseqDB supports abitratly large sequences of any sizes, so it doesn’t need any splitting step

- Most of the common operations like slicing, ordering, transforming sequences can be done in SQL instead of external tools

- Most of the algorithms were ported by me based on their original source code, so now they’re supported on any architecture.

- BioseqDB aligner pipeline can be called as standalone binary directly.

- I provided extensive analysis of algoritms that are poorly described in literature, so the provided illustrations and code listings can be used as a base to develop new software more easily and great learning material.

- The overall performance was very satisfactory and speedups of x2, x10 to x100 on heavy SWAPped systems were achieved.

- Utilise features of PG19 (current releases for PG19 don’t take advantage of streaming APIs or other interesting features)

In the future, BioseqDB should:

- Rewrite existing GPU algorithms to be more similar to G3SA implementations. Original authors claimed performance speedups, so it is worth considering exploring Vulkan more. The current implementation is very simple and due to personal inexperience it performed poorly.

- Improiving benchmaring methodology as I only measured total runtime and in some placed the results are outliers for unknown reasons. It requires more investigation.

- Improving optimisations for short sequences

- Adding support for various other different algorithms like BWA or aligner-free approaches

- Adding support for more alphabets

- Resolving problems with very high build times

- Optimising access to indices and "no-index" timings compared to minimap2
