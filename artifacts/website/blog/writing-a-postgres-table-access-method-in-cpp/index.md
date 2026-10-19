---
title: Writing a small PostgreSQL table access method in C++
slug: writing-a-postgres-table-access-method-in-cpp
description: Build a PostgreSQL 17 extension, follow scans and writes through its callbacks, and find the places where copying the heap routine is not enough.
date: '2026-10-12'
author: Piotr Styczyński
category: bioseqdb
keywords: [PostgreSQL, C++, table access methods, storage, heap, tuple slots, MVCC, TOAST, VACUUM, BioseqDB]
cover: /static/figures/blog/sequence-letters.svg
image_alt: Coloured DNA letters in a shallow grid.
draft: false
---

When I started changing PostgreSQL storage for BioseqDB, the interesting question was not how to parse `CREATE TABLE`. PostgreSQL already knows how to do that. I needed to understand which functions it would call after the table existed, what those functions had to return, and how much work was hidden behind a successful `INSERT`.

The [earlier storage overview](../where-postgres-table-storage-can-change/) describes that boundary from the SQL side. Here I want to cross it and compile something. We will build `traceheap`, a small C++ extension which uses PostgreSQL's heap storage but counts selected table callbacks. It lets us see a scan visiting four rows even though the query returns only two, distinguish an ordinary insert from a bulk insert, and check what happens to our own state after a transaction rolls back.

[Phil Eaton's introduction to table access methods](https://notes.eatonphil.com/2023-11-01-postgres-table-access-methods.html) is useful background for getting into this part of PostgreSQL. The experiment below takes a different route: it starts with working heap operations, then makes the delegation and its limits visible. The implementation, examples and diagrams here were made for this tutorial.

This is an educational method for **PostgreSQL 17**, tested with **17.10**. It does not implement BioseqDB's columnar format, and it deliberately rejects user indexes and `VACUUM FULL`. Those are quite important limitations, so we will reach them through actual commands rather than leave them in a footnote.

## 1. Get a small extension to build

Download the [complete traceheap example](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/examples/traceheap.tar.gz). It contains the C++ source, the extension files and `test.sql`. You can also read the [source file](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/examples/traceheap/traceheap.cpp) directly.

You need a C++ compiler, `make`, and the server development files for the PostgreSQL 17 installation you will run. Check `pg_config --version`; an unrelated `pg_config` earlier in `PATH` is an easy way to compile against the wrong headers. Use a disposable local database for this experiment. Installing an extension puts executable code inside the server process.

```sh
tar -xzf traceheap.tar.gz
cd traceheap
pg_config --version
make PG_CONFIG=/path/to/postgresql-17/bin/pg_config
make PG_CONFIG=/path/to/postgresql-17/bin/pg_config install
```

Replace the path with your installation's `pg_config`. The installation step needs permission to write that installation's extension and library directories. These SQL examples use the extension you just built.

The Makefile is short because [PGXS](https://www.postgresql.org/docs/17/extend-pgxs.html) supplies the PostgreSQL build rules:

```makefile id=traceheap_makefile
EXTENSION = traceheap
MODULE_big = traceheap
OBJS = traceheap.o
DATA = traceheap--1.0.sql
PG_CONFIG ?= pg_config
PG_CXXFLAGS += -std=c++17 -fno-exceptions -fno-rtti
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
```

`MODULE_big` produces one loadable library from the object files. The control file names version `1.0`, and the SQL installation file creates the handler, the access method and two functions for reading and clearing counters. There is no server restart or preload setting in this example: PostgreSQL loads the library when a function in it is first needed.

## 2. Connect a SQL name to a callback table

The installation SQL contains this binding:

```sql id=traceheap_registration
CREATE FUNCTION traceheap.handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'traceheap_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD traceheap TYPE TABLE HANDLER traceheap.handler;
```

`MODULE_PATHNAME` is replaced during extension installation. The SQL name `traceheap.handler` and the exported symbol `traceheap_handler` are different names for different sides of the boundary. The function returns a pointer to a `TableAmRoutine`, whose members tell PostgreSQL how to scan, insert, update, maintain and inspect a table. [CREATE ACCESS METHOD](https://www.postgresql.org/docs/17/sql-create-access-method.html) documents the registration command; creating an access method requires superuser privileges.

![The traceheap handler returns a callback table. Selected callbacks count operations and delegate to heap, while unsupported index and rewrite paths stop with an explicit error.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/traceheap-dispatch.svg)

The source starts by giving the PostgreSQL declarations C linkage:

```cpp id=traceheap_headers
extern "C" {
#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
}
```

The exported functions also use `extern "C"`, and `PG_FUNCTION_INFO_V1` declares their PostgreSQL calling convention. This stops C++ name mangling from hiding the symbol that the SQL declaration asks PostgreSQL to load. `PG_MODULE_MAGIC` supplies the module compatibility information. These are parts of the server interface, not ordinary functions which take SQL values as C++ arguments.

For the initial callback table, the handler copies `*GetHeapamTableAmRoutine()` into a static `TableAmRoutine`. It then replaces selected members. Both the copy and the pointer to the original heap routine live for the backend's lifetime; returning a pointer to a local stack variable would leave PostgreSQL holding invalid memory after the handler returned.

The distinction between the two pointers matters:

```cpp id=traceheap_handler
heap_methods = GetHeapamTableAmRoutine();
methods = *heap_methods;
methods.scan_begin = traced_begin;
methods.scan_end = traced_end;
methods.scan_rescan = traced_rescan;
methods.scan_getnextslot = traced_next;
methods.tuple_insert = traced_insert;
```

Our wrappers call `heap_methods`, which still points at the original routine. Calling `methods.scan_getnextslot` from `traced_next` would call the wrapper again until the backend ran out of stack. The complete handler in the download installs the other wrappers and the explicit restrictions discussed below.

## 3. Follow one scan through its lifetime

Let's create the extension and a table. There is deliberately no primary key yet.

```sql
CREATE EXTENSION traceheap;
SET max_parallel_workers_per_gather = 0;

CREATE TABLE specimens (
  id integer NOT NULL,
  label text NOT NULL,
  read_count integer NOT NULL
) USING traceheap;

INSERT INTO specimens VALUES
  (1, 'soil', 120), (2, 'river', 900),
  (3, 'leaf', 480), (4, 'air', 60);
```

The `USING` clause selects our table method. It does not select an index method. We disable parallel query for the counter experiment because each backend process has its own copy of our static variables, including each parallel worker.

A sequential scan begins with `scan_begin`, asks repeatedly for another row through `scan_getnextslot`, and finishes with `scan_end`. The beginning callback receives the relation, the snapshot, scan keys, parallel state and option flags. Our wrapper passes every argument through unchanged:

```cpp id=traceheap_begin
TableScanDesc traced_begin(Relation rel, Snapshot snapshot, int nkeys,
                           ScanKeyData* key, ParallelTableScanDesc parallel,
                           uint32 flags) {
    auto scan = heap_methods->scan_begin(rel, snapshot, nkeys, key, parallel, flags);
    ++counters.scans_started;
    return scan;
}
```

The returned descriptor is heap's scan state. We do not cast it to a home-made structure or free it ourselves; the delegated `scan_end` releases it. A method with its own storage would need state appropriate to that storage and matching cleanup. The [table access API](https://www.postgresql.org/docs/17/tableam.html) is the starting point, while the comments in the installed `access/tableam.h` describe each callback's contract.

The row callback is similarly small:

```cpp id=traceheap_next
bool traced_next(TableScanDesc scan, ScanDirection direction, TupleTableSlot* slot) {
    const bool found = heap_methods->scan_getnextslot(scan, direction, slot);
    if (found) ++counters.scan_rows;
    return found;
}
```

The slot is where the executor receives a row. Returning `true` without putting a valid row in that slot would be a broken implementation, even if the count happened to increase. Returning `false` ends this scan direction. We count successful row deliveries, not the final unsuccessful request after the last row.

Because the copied routine keeps heap's slot operations, the slot representation agrees with the delegated scanner. This is not a place where I would replace one callback and guess that all the neighbouring callbacks still understand its data. A slot may refer to memory or a buffer owned elsewhere; a pointer into that row should not be kept after the slot is cleared or reused.

## 4. Count scanned rows, not just returned rows

Now run the query and inspect the counter in a separate statement:

```sql
SELECT traceheap.reset();

SELECT id, label
FROM specimens
WHERE read_count >= 400
ORDER BY id;

SELECT traceheap.stats()->>'scan_rows' AS rows_delivered_by_am;
```

The query returns `2, river` and `3, leaf`. The counter returns `4`. In this plan the table method supplies the visible rows and the executor applies `read_count >= 400` afterwards, so delivering a row from the access method does not imply that the row survives the SQL filter.

![Four rows pass through the scan callback. The executor filters out soil and air, leaving river and leaf, so the table callback counter is four and the final result count is two.](/assets/e5b8991aec67c818395da19c1865cd3c9c09c1e01f8bb9111f647613934ea6d3/figures/blog/traceheap-scan.svg)

Try `EXPLAIN (ANALYZE, COSTS OFF)` on the same query. It executes the query and therefore changes the counters too. Keeping the measurement query separate from the query being measured avoids an awkward evaluation-order question, and resetting immediately before each experiment makes the results easier to read.

The download also wraps `scan_rescan`. A rescan is a request to restart an existing scan with the supplied options; it is not always a new `scan_begin`. This distinction becomes useful once a table is on the inner side of a join. I would check the actual plan before trying to predict a rescan count from the SQL text alone.

These counters are a way to inspect a few call paths. They are not a replacement for `EXPLAIN`, an I/O counter, or a count of every possible way PostgreSQL can read a row. The inherited sampling and maintenance paths have their own callbacks.

## 5. Add writes without losing their result codes

For an ordinary insert the wrapper delegates first, then records that the call completed:

```cpp id=traceheap_insert
void traced_insert(Relation rel, TupleTableSlot* slot, CommandId cid,
                   int options, BulkInsertStateData* state) {
    heap_methods->tuple_insert(rel, slot, cid, options, state);
    ++counters.inserts;
}
```

`COPY` can insert a batch through `multi_insert`, so the example counts its `nslots` argument separately as `bulk_rows`. Wrapping only `tuple_insert` and calling the result “all inserted rows” would miss that path. Speculative insertion has separate callbacks too; this example does not claim to count it.

Updates and deletes return `TM_Result`. The wrapper preserves that result and only increments its success counter when the result is `TM_Ok`. It also preserves the snapshot, waiting behaviour, failure information and index-update output parameters. Replacing every result with success would tell the executor that an operation succeeded even when the underlying method reported a conflict or another outcome.

```sql
SELECT traceheap.reset();
UPDATE specimens SET read_count = read_count + 1 WHERE id = 1;
DELETE FROM specimens WHERE id = 4;
SELECT traceheap.stats()->>'updates' AS updates,
       traceheap.stats()->>'deletes' AS deletes;
```

Both counters are `1`. The download's test also performs a three-row `COPY` and checks `bulk_rows = 3`, rather than assuming that the bulk and ordinary insert paths happen to be the same.

There is a separate issue with the counters themselves:

```sql
SELECT traceheap.reset();
BEGIN;
INSERT INTO specimens VALUES (99, 'rolled back', 0);
ROLLBACK;
SELECT count(*) FROM specimens WHERE id = 99;
SELECT traceheap.stats()->>'inserts';
```

The row count is `0`, but `inserts` is `1`. PostgreSQL rolled back the row change; it did not roll back an integer in our process. That behaviour is intentional here. It would be a serious mistake if this integer were the only record of a storage allocation, a persistent index entry or some other change which had to agree with the transaction.

## 6. Find where copying heap stops being enough

The first version of this experiment copied the heap routine and added counters. Creating a table with a primary key failed with `only heap AM is supported`. Removing the primary key was not sufficient for a table with an unrestricted `text` column, because creating its TOAST table also involves an index.

There are two separate assumptions behind those failures.

First, the PostgreSQL 17 heap index builder calls `heap_getnext()`, which checks that the relation has the original heap routine pointer. Our copied routine has a different address. Copying the function pointer did not remove the assumption inside the function. The relevant checks are in PostgreSQL's [heap scan implementation](https://github.com/postgres/postgres/blob/REL_17_10/src/backend/access/heap/heapam.c) and its [table callback implementation](https://github.com/postgres/postgres/blob/REL_17_10/src/backend/access/heap/heapam_handler.c).

The download replaces the index-build and validation callbacks with a clear `FEATURE_NOT_SUPPORTED` error. This includes primary keys and unique constraints which need an index. I did not change PostgreSQL's relation pointer temporarily to trick the heap check: that would obscure the exact boundary this experiment is supposed to explain.

```sql
CREATE INDEX specimens_reads ON specimens(read_count);
-- ERROR: traceheap does not implement index creation
```

Second, heap's TOAST-method callback returns the parent relation's access-method OID. For our parent relation that would be `traceheap`, whose indexes we just rejected. The example therefore explicitly chooses ordinary heap for the auxiliary TOAST table:

```cpp id=traceheap_toast
Oid toast_method(Relation) {
    return HEAP_TABLE_AM_OID;
}
```

This keeps the teaching table on `traceheap` while its TOAST table uses heap and the auxiliary TOAST index uses an ordinary B-tree. The test inserts and reads back a 32,000-character value assembled from many MD5 strings; it checks the exact returned value, rather than merely checking that table creation succeeded. The [TOAST documentation](https://www.postgresql.org/docs/17/storage-toast.html) explains why long values involve an auxiliary relation at all.

## 7. Test maintenance and visibility as separate operations

Ordinary `VACUUM` has a relation callback which the example wraps. `ANALYZE` uses the inherited analysis callbacks; it is not counted as another ordinary vacuum. `VACUUM FULL` follows the table-rewrite path, and our `relation_copy_for_cluster` replacement rejects that path explicitly.

```sql
ANALYZE specimens;
VACUUM specimens;
VACUUM FULL specimens;
-- ERROR: traceheap does not implement VACUUM FULL or a table rewrite
--        through the clustering callback
```

For this PostgreSQL 17.10 build, I also tested `ALTER TABLE ... SET ACCESS METHOD heap` on a separate six-row table. The conversion succeeded and preserved all six rows. That does not mean every `ALTER TABLE` or rewrite operation has been tested. The useful distinction is the operation and the path it actually takes, not whether its name sounds similar to another working command.

The supplied smoke checks can be run in a fresh database with:

```sh
psql -X -v ON_ERROR_STOP=1 -d your_disposable_database -f test.sql
```

They check ordinary writes, rollback, bulk insertion, rejected indexes and primary keys, analysis, ordinary vacuum, a large text value and truncation. An assertion failure stops the script. During preparation I additionally checked two simultaneous connections: a repeatable-read transaction continued to see `121` while another session changed the row to `777`, both before and after that writer committed; a new transaction then saw `777`. The counters in the reader and writer remained independent.

A clean server restart preserved the six rows and the committed value. This was a clean-restart check, not a crash-recovery test or a claim that a new storage implementation gets recovery for free. Our actual tuple operations still use heap. If those operations change, the evidence from this example no longer establishes their correctness.

## 8. Keep the C++ boundary deliberately boring

The wrappers use plain pointers, scalar values and a trivial counter structure. They do not keep a `std::vector` or a file stream alive across a PostgreSQL call that may raise an error. PostgreSQL's error machinery can jump out of the call without running ordinary C++ destructors. Disabling C++ exceptions does not change that behaviour.

This is why the example does not present RAII as automatic protection around every server API call. A real extension needs a deliberate boundary between PostgreSQL error handling, its memory contexts and any C++ objects which need cleanup. [PostgreSQL's C++ integration notes](https://www.postgresql.org/docs/17/xfunc-c.html#XFUNC-CPP) are relevant here even if the first source file compiles without a warning.

Our counter reset is intentionally simple, and the JSON returned by `traceheap.stats()` is allocated through PostgreSQL's allocation functions. The state is backend-local, covers all `traceheap` tables visited by that backend, is not transactional, and disappears when the process exits. Those properties make it useful for this tutorial and unsuitable as a database-wide monitoring system.

## Where this meets BioseqDB

The part I want to carry into a larger storage engine is the habit of following each callback's inputs, output and lifetime. A successful `CREATE TABLE` tells us very little about updates, snapshots, large values or maintenance. Even this small heap-based experiment found two assumptions which were invisible in a copied `TableAmRoutine` declaration.

BioseqDB's [table callback registration](https://github.com/styczynski/bioseqdb/blob/main/bioseq/backend/pg/table/access_method.cppm) binds its own columnar scan, write and maintenance functions instead of copying heap's routine. Its [storage chapter](../../v0.1/tutorials/project_postgres_storage/) explains the larger storage layout, and the [custom scan chapter](../../v0.1/tutorials/project_postgres_custom_scan/) follows the separate path used for sequence-search execution. A table method and a custom scan solve related but different problems.

For a next experiment, I would change one observable behaviour and keep the current SQL checks beside it. If that change requires a different slot representation or persistent layout, I would write down which other callbacks now have to agree with it before adding another feature. That gives me a callback I can exercise with a small query and a result I can inspect.
