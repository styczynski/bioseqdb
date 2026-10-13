---
title: Where PostgreSQL table storage can change
slug: where-postgres-table-storage-can-change
description: Following a small specimen table from SQL into its access method, and looking at the work that remains after a custom engine can return its first row.
date: '2026-10-12'
author: Piotr Styczyński
category: others
keywords: [PostgreSQL, table access methods, storage, heap, indexing, MVCC, WAL, VACUUM]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Muted coloured DNA letters arranged in a shallow grid.
draft: false
---

When I look at a slow query, I usually start with the query and its indexes, because those are the parts I can change without asking awkward questions about where the rows actually live. PostgreSQL also has a place to change that last part: a table access method.

For this post I wanted to start with an ordinary table and a question I can answer from SQL: which part owns its storage, and which part helps me find a row?

For a working extension to compile and inspect, continue with [writing a small table access method in C++](../writing-a-postgres-table-access-method-in-cpp/).

## Two meanings of access method

The [pg_am catalog](https://www.postgresql.org/docs/17/catalog-pg-am.html) records both table and index access methods. Its `amtype` column distinguishes them, which is useful because `USING` appears in both table and index definitions and can make the two choices look more similar than they are.

```sql
SELECT amname,
  CASE amtype
    WHEN 't' THEN 'table'
    WHEN 'i' THEN 'index'
  END AS method_kind
FROM pg_am
WHERE amname IN ('heap', 'btree', 'gin')
ORDER BY method_kind, amname;
```

A table method manages the table's rows; an [index method](https://www.postgresql.org/docs/17/indexam.html) manages an index. Choosing a B-tree for a specimen identifier does not turn the underlying table into a B-tree table, and dropping that index does not discard the specimen records. I find that distinction easier to remember when I inspect both objects together.

## Follow one table

Here is the small table I use below. The specimens are invented, and the counts are just values which make the output easy to check.

```sql
CREATE TABLE lab_observations (
  specimen text PRIMARY KEY,
  read_count integer NOT NULL,
  needs_review boolean NOT NULL
) USING heap;

CREATE INDEX lab_observations_count_idx
  ON lab_observations USING btree (read_count);
```

`heap` is PostgreSQL's [default table access method](https://www.postgresql.org/docs/17/runtime-config-client.html#GUC-DEFAULT-TABLE-ACCESS-METHOD). I spell it out so the example does not depend on a changed session default. The method actually attached to a relation is recorded in [pg_class.relam](https://www.postgresql.org/docs/17/catalog-pg-class.html), so I can join that value back to the catalog above:

```sql
SELECT relation.relname AS object_name,
  method.amname AS access_method
FROM pg_class AS relation
JOIN pg_am AS method ON method.oid = relation.relam
WHERE relation.oid IN (
  'lab_observations'::regclass,
  'lab_observations_count_idx'::regclass
)
ORDER BY object_name;
```

There are two objects here with two different jobs, even though they describe the same collection of observations. That gives me a useful boundary for an experiment: I can ask whether I need a different way to locate candidates, or whether I really need a different representation of the table itself.

![The SQL executor reaches table storage through table callbacks, while an index supplies candidate row identifiers.](/assets/6ad246586dfbc11854332631a95d00e528509581cd8f2f43ca4918229b9662a4/figures/blog/table-access-boundary.svg)

## Returning a row is the beginning

A table access method supplies a handler returning a `TableAmRoutine`, whose callbacks let PostgreSQL request operations such as starting a scan or inserting a tuple. A tuple slot provides a representation of a row that the rest of the executor can use without understanding every detail of its storage. The [table access method documentation](https://www.postgresql.org/docs/17/tableam.html) describes that interface and the constraints around row identifiers.

This is an attractive place to experiment, but the first successful `SELECT` answers a very small question. If I put three records into an array and read them back, I have demonstrated that particular path through my code. I have not established what happens when a second connection arrives, a transaction aborts, or the server disappears halfway through a write.

For example, [MVCC](https://www.postgresql.org/docs/17/mvcc-intro.html) gives a query a view of the database through a snapshot. “Return the newest record I can find” is not enough when that record belongs to a transaction the reader cannot see. I would test an uncommitted update from another connection before spending time comparing scan speeds.

An index lookup has the same visibility problem. The table method's `index_fetch_tuple` callback receives a candidate identifier and a snapshot; it must find a suitable visible tuple rather than blindly trust the index entry. The [API comments](https://github.com/postgres/postgres/blob/REL_17_STABLE/src/include/access/tableam.h) also distinguish ordinary vacuum work from the callbacks used by `ANALYZE` and table rewrites. These are different operations, even when they all involve visiting stored data.

## Keep an ordinary query as a reference

I would keep this kind of deliberately boring query beside a storage experiment:

```sql
SELECT specimen, read_count
FROM lab_observations
WHERE needs_review
ORDER BY read_count DESC, specimen;
```

The expected result is small enough to check by eye. From there I would add updates, rollbacks, deleted rows and overlapping readers, keeping the expected answers seperate from the engine's implementation. A faster result is useful only after I know it is the right result.

Reclaiming obsolete data also needs attention. PostgreSQL's heap requires [routine vacuuming](https://www.postgresql.org/docs/17/routine-vacuuming.html); a different storage design still needs a clear account of when old information can be removed. I would include a long-running reader in that experiment, because reclaiming something too early is harder to notice than retaining it too long.

## The part that survives a restart

[Write-ahead logging](https://www.postgresql.org/docs/17/wal-intro.html) provides the ordering needed to recover logged changes after a crash. A custom method needs its own coherent recovery story; PostgreSQL's table interface permits its WAL facilities or another implementation. An in-memory demonstration should not be mistaken for durable storage just because it accepts familiar SQL.

There is an operational boundary too. In PostgreSQL 17, changing an existing table with `ALTER TABLE ... SET ACCESS METHOD` rewrites it and requires an `ACCESS EXCLUSIVE` lock. I would plan that as a migration, with room for the rewrite and a tested way back, rather than treat it as an inexpensive setting. The version-specific [ALTER TABLE documentation](https://www.postgresql.org/docs/17/sql-altertable.html) is the place to check before doing it.

What interests me here is the amount of PostgreSQL I can keep while investigating a different storage layout. The SQL examples above deliberately stay with heap; they establish the boundary and a few expected results, leaving the larger implementation questions visible instead of hiding them behind a successful demo.
