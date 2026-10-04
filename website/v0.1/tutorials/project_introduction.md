---
title: "Introduction"
slug: project_introduction
description: "Introduction"
order: 20
keywords: [BioseqDB, project]
draft: false
---

The project is structured in the way that each stage of pipeline prosecessing is discussed in chronological order, while the Postgres storage details are addressed later. At the end of the project I provide samples of usage to demonstrate all the features and unprocessed benchmark results for different scenarios and machines.

## Primary goal

The primary goal of BioseqDB is allow labs to amabdon storing FASTA/FASTQ files on large disks without versioning and allow to perform alignment jobs directly in the database:

<a id="lst-first-sql-example-main"></a>

```sql runnable id=project_lst-first-sql-example-main timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE samples (id integer, age integer, seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO samples VALUES (1, 35, 'CAGTAACCACCACCCAGTTCAGTTCCCATATGAAAA');
-- bioseq:visible
SELECT id, bioseq.cigar(seq) FROM samples WHERE age >= 30 AND mmsearch(seq, 'CAGTAACCACCACCCAGTTCAGTTCCCATATGAAAA'::bioseq.seq('dna4', 64));
-- bioseq:teardown
DROP TABLE samples;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Simplest BioseqDB SQL query

## Secondary goals

The main goals of BioseqDB were:

- Explore existing algorithms and literature and provide easy way to understand various approaches and their trade-offs, so this project is a form of educational material

- Port existing algorithms to modern C++ by writing them from scratch, so they run on any device

- Integrate state-of-the-art algorithms into one pipeline

- Create own optimisations and measure them on randomized data

- Provide first of its kind integration of aligner software into relational database

- Optimise pipeline until it’s more performant than baseline tool (I compare myself to minimap2)

- Provide support for very large sequences, so storage doesn’t require any external tools to split or reorder or process in other ways the input sequences

- Directly integrate with SQL api in a way that’s intuitive

## Work context

This project required several years of exploration and experiments. I began without a formal background in computational biology, thus learning the field was part of the research. I proposed the database idea and prepared the initial design, when I was still working for my funded startup. I directed the 2022 prototype, whose implementation was written by colleagues and plugged BWA-MEM call directly into Postgres via C bindings.<sup><a href="#project-note-1">1</a></sup> The early prototype was to simplistic as technically I still didn’t understand how aligners work internally.

The idea of writing my own alignment software was my long standing dream. I explored the field hobbistically and this project stands as the final result of all that work. I had oppotunity to open issue and communicate directly with authors of MMSeqs2 or Joe Conway who is one of the major contributors of Postgres and few years ago helped me to understand limitations of parallel execution in Postgres.

![main diagram](/static/figures/project/main-tikz-01.svg)

<a id="sec-introduction-timeline"></a>

Project history

<sup><a href="#project-note-2">2</a></sup>

<a id="project-note-1"></a>

**1.** The current BioSeqDB is a separate project, designed and written by me. It contains no code or intellectual property from the early BWA-MEM integration experiments.

<a id="project-note-2"></a>

**2.** I was a co-founder (shareholder) until 15 July 2022 (KRS update on 3 Jun 2022) in Covid Genomics Sp. z o.o. (registered in Poland in Jun 2020, KRS 0000846289) which then changed its legal name to CGen Sp. z o.o. (on 3 Jun 2022) and then changed its branding name to Deepflare (on Jul 2022). I was also a Chief Engineering Officer from 1 Apr 2020 till 31 Aug 2022.
