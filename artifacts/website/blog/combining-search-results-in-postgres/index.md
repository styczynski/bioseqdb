---
title: Combining search results in PostgreSQL
slug: combining-search-results-in-postgres
description: A small SQL experiment with filtered nearest neighbours, text matches and reciprocal rank fusion, including the results that disappear when LIMIT comes too early.
date: '2026-10-12'
author: Piotr Styczyński
category: others
keywords: [PostgreSQL, SQL, hybrid search, vector search, full text search, pgvector, ranking]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Translucent boxes containing coloured DNA letters.
draft: false
---

Suppose I have a table of specimen notes and want to find something about a failed library preparation. Some notes use exactly those words, while others describe the same problem as “too little material after amplification”. I would like both to appear, but only if they belong to the study I am currently looking at.

There are two separate decisions here: which rows are allowed into the answer, and how to order those rows. Keeping them separate makes it much easier to explain why a search returned something odd, especially when more than one search method contributes results.

The term hybrid search is used for both combining lexical and semantic retrieval, and combining vector retrieval with ordinary filters. I want to look at the boundary between these two uses with a deliberately small SQL experiment. The numbers below are invented distances and ranks, so the examples require PostgreSQL but no embedding model or additional extension.

## Losing a row before the search is finished

Here are four notes, already assigned distances from a query. Smaller means closer. Only study A is relevant to the request, and I want two results.

```sql
WITH notes(id, study, distance) AS (
  VALUES (11, 'B', 0.04), (12, 'A', 0.08),
         (13, 'B', 0.10), (14, 'A', 0.16)
), nearest AS (
  SELECT * FROM notes
  ORDER BY distance, id
  LIMIT 2
), filtered_first AS (
  SELECT * FROM notes WHERE study = 'A'
  ORDER BY distance, id
  LIMIT 2
)
SELECT 'limit then filter' AS approach,
       array_agg(id ORDER BY distance, id) AS ids
FROM nearest WHERE study = 'A'
UNION ALL
SELECT 'filter then limit',
       array_agg(id ORDER BY distance, id)
FROM filtered_first;
```

The first answer contains only note 12. Note 14 never gets a chance because the inner query has already discarded it. Increasing the final limit cannot bring it back. This is a consequence of where I put the limit, even with perfectly accurate distances and no index involved.

The ID is a tie-breaker, which makes the example reproducible if two distances become equal. PostgreSQL does not promise which tied rows a limited query will choose unless the ordering distinguishes them. That matters for tests and for a reader trying the example twice. [PostgreSQL's LIMIT documentation](https://www.postgresql.org/docs/current/queries-limit.html) describes this behaviour.

![Study eligibility is applied separately to text and vector candidates before their ranked results are combined.](/assets/fb67d2122e16debc58ea1a587c974bf0ddd51142438e3f11da4b86dc044892c2/figures/blog/search-candidates.svg)

## Where an approximate index changes the problem

With pgvector, an approximate index can find nearby candidates without comparing the query against every stored embedding. Filters are applied to those candidates, which means a restrictive study filter can leave fewer results than requested. Iterative scans, available since pgvector 0.8.0, can continue looking, but scan limits still matter. An ordered result also does not prove that every closer eligible row was found. [pgvector documents both filtering and iterative scans](https://github.com/pgvector/pgvector#filtering).

For a small eligible subset I would first measure an exact distance sort after filtering, then compare an approximate plan against that answer. There is no universal row count at which one becomes better; the distance calculation, the distribution of studies and the amount of data all enter the measurement.

Having an index on the study column and another on embeddings does not by itself settle that choice. PostgreSQL can combine suitable index scans through bitmaps, but the bitmap represents matching table locations and loses the original index ordering. An ordered nearest-neighbour search needs more than a set of matching locations. [Combining multiple indexes](https://www.postgresql.org/docs/current/indexes-bitmap-scans.html) explains the bitmap part.

## Combining two ranked lists

Now suppose the eligible notes have been searched twice. A lexical search found note 21 first, while the embedding search preferred note 24. PostgreSQL's built-in text search can produce the lexical matches and a relevance ranking; its scores describe that ranking method, not a probability that a note answers the question. [The text-search documentation](https://www.postgresql.org/docs/current/textsearch-controls.html#TEXTSEARCH-RANKING) explains the available ranking functions.

I do not want to add a text score directly to a vector distance: their scales and directions have different meanings. For this experiment I use reciprocal rank fusion instead, adding `1 / (60 + position)` for each list containing a note. A missing note contributes nothing. The constant 60 comes from the original RRF paper; it is a choice to evaluate, not a property of PostgreSQL. [Cormack, Clarke and Buettcher, 2009](https://cormack.uwaterloo.ca/cormacksigir09-rrf.pdf).

```sql
WITH lexical(note_id, position) AS (
  VALUES (21, 1), (24, 2), (28, 3)
), semantic(note_id, position) AS (
  VALUES (24, 1), (31, 2), (21, 3)
), contributions AS (
  SELECT note_id, 1.0 / (60 + position) AS weight
  FROM lexical
  UNION ALL
  SELECT note_id, 1.0 / (60 + position)
  FROM semantic
)
SELECT note_id, round(sum(weight), 6) AS fused_score
FROM contributions
GROUP BY note_id
ORDER BY sum(weight) DESC, note_id;
```

Note 24 wins because it appears near the top of both lists. Note 31 still appears even though the lexical search did not find it. `UNION ALL` is intentional: each list provides its own contribution. Within each input list, however, a note must occur only once; otherwise joins that duplicate rows would accidentally give it extra votes.

This is a useful place to stop and inspect the candidates. Fusion cannot recover a note absent from both lists, and asking each method for only two candidates may remove a note that would have done well in the combined ranking. I would choose candidate limits separately from the number of results displayed to the reader.

## What I would measure

For each test query I would keep the eligible IDs, each candidate list and the final ordering, together with the query time. This makes a missing result traceable: was it excluded by the study filter, absent from retrieval, or just ranked too low? A single response-time number cannot answer that.

I would also include awkward cases: a rare study, a note containing an exact sample identifier, two notes with tied scores, and a query with no lexical match. The occasional empty answer is less suprising when those cases are part of the test data from the beginning.

The tiny datasets here are a way to inspect the decisions in SQL before choosing an index or a larger retrieval setup.
