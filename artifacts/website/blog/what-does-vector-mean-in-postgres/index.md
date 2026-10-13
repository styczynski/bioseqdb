---
title: What does vector mean in PostgreSQL?
slug: what-does-vector-mean-in-postgres
description: Text-search vectors, embeddings, arrays, geometric points and execution batches share a word but have different jobs. A few small SQL queries help tell them apart.
date: '2026-10-12'
author: Piotr Styczyński
category: others
keywords: [PostgreSQL, SQL, vector, tsvector, pgvector, arrays, vectorized execution]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Muted coloured A, C, T and G letters in translucent boxes.
draft: false
---

If a database feature says it supports vectors, I still need to ask what I can put into it and which operations it supports. A vector might be a value stored in one row, a collection of values being processed together, or the outline of an illustration on this page. Those uses can appear in the same application without sharing an implementation.

I find it easier to start with a sample record than with the word itself. Imagine a specimen with a free-text note, three measurements and a position on a laboratory diagram. All three belong to the sample, but searching the note, comparing measurements and drawing its position are separate tasks.

![Three representations of a specimen record: searchable terms, numeric measurements and a geometric location. A separate row shows execution batches operating across records.](/assets/6ad246586dfbc11854332631a95d00e528509581cd8f2f43ca4918229b9662a4/figures/blog/vector-meanings.svg)

## The vector made of words

PostgreSQL's `tsvector` stores lexemes, which are the normalized terms used by full-text search, with optional positions and weights. It is not a list of embedding coordinates. I can inspect the difference using a note that mentions the same thing twice:

```sql
WITH note AS (
  SELECT to_tsvector('english',
    'Samples were frozen; the second sample thawed.') AS terms
)
SELECT terms,
       terms @@ plainto_tsquery('english', 'frozen sample')
         AS matches
FROM note;
```

The rendered value is organized by lexeme, so it no longer looks like the original sentence. Positions preserve information about where terms occurred. A `tsquery` supplies the search condition, and `@@` tests whether the document representation matches it. There is no embedding model hidden inside this query. [Text-search types](https://www.postgresql.org/docs/current/datatype-textsearch.html) and [text-search operators](https://www.postgresql.org/docs/current/functions-textsearch.html) describe the two types and their relationship.

For these examples I name the `english` configuration explicitly. A sample identifier is a different problem from ordinary English prose, and I would inspect its tokenization before deciding to feed it through the same configuration. In particular, turning a note into searchable terms is not a reversible way to store the note itself; I would keep the original text as well.

## A list of numbers still needs a meaning

Three measurements fit in a PostgreSQL array, although their interpretation remains part of the schema and the application. Here I have arranged them in the order concentration, volume and temperature. The database cannot infer those labels from the array:

```sql
WITH sample(measurements) AS (
  VALUES (ARRAY[12.5, 40.0, 4.0]::real[])
)
SELECT measurements[1] AS concentration,
       measurements[2] AS volume,
       measurements[3] AS temperature,
       cardinality(measurements) AS dimensions
FROM sample;
```

Writing an array declaration with a size, such as `real[3]`, does not enforce that size in PostgreSQL. If I need exactly three entries I must enforce that separately, and array bounds deserve attention too if values can arrive from outside the application. [The array documentation](https://www.postgresql.org/docs/current/arrays.html) covers these details.

Even when two arrays have the same length, blindly computing their distance might be meaningless. A change of one degree and a change of one microlitre are different quantities; converting just one measurement to another unit would change the numerical distance. That is a modelling decision, not something an index can fix.

An embedding also contains numbers, but its coordinates come from a chosen representation of the input. pgvector supplies a `vector` type and distance operators for this use. The following optional example requires the separately installed pgvector extension; it uses invented coordinates rather than real specimen embeddings:

```sql
CREATE EXTENSION IF NOT EXISTS vector;
SELECT '[0.2,0.4,0.6]'::vector(3)
   <-> '[0.2,0.4,0.9]'::vector(3) AS distance;
```

Here `<->` means Euclidean distance for those operand types. Choosing a distance and generating comparable inputs remain separate from storing the values. The [pgvector reference](https://github.com/pgvector/pgvector#querying) lists the supported operators. In an actual experiment I would record the embedding model and preprocessing alongside the data, so a later model change cannot quietly mix incompatible representations.

## The same operator can describe a different distance

A point on a two-dimensional laboratory drawing does not need an embedding extension. PostgreSQL already has geometric types, including `point`, and the point-to-point distance operator is also written `<->`:

```sql
SELECT point(2, 3) AS storage_position,
       point(5, 7) AS workbench_position,
       point(2, 3) <-> point(5, 7) AS drawing_distance;
```

This returns a distance of five drawing units. The type tells PostgreSQL which operation to perform, even though the operator spelling is shared with the previous example. These built-in coordinates describe a plane; they do not carry a geographic coordinate reference system. [Geometric types](https://www.postgresql.org/docs/current/datatype-geometric.html) and [their operators](https://www.postgresql.org/docs/current/functions-geometry.html) specify the available operations.

Vector graphics add another use of the word. The illustration above consists of shapes and text, while an SVG path can describe a shape through drawing commands and coordinates. The browser paints that description at the requested size. Those coordinates are instructions for an image, not a search representation of its meaning. [The SVG path specification](https://www.w3.org/TR/SVG2/paths.html) explains that format.

## A vector can also span many rows

Vectorized execution refers to how a database processes data internally. For example, DuckDB's execution format passes batches of values through operators, and a batch can contain ordinary integers or strings. Its documentation distinguishes several physical representations, including flat and constant vectors. The SQL column does not have to contain embeddings for this execution method to be useful. DuckDB describes these representations in its [execution-format documentation](https://duckdb.org/docs/current/internals/vector).

That distinction prevents an occassional misunderstanding: adding a `vector` column to a PostgreSQL table does not, by itself, turn the query executor into a batch-processing engine. A stored value's type and the way an executor moves through rows answer different questions.

When reading about a vector feature, I therefore try to identify the stored value, the operation and the execution method separately. For the specimen example those questions lead to fairly ordinary choices: retain the note, define the measurements, name the coordinate system and test the search behaviour. The shared vocabulary becomes much less troublesome once those choices are written down.

