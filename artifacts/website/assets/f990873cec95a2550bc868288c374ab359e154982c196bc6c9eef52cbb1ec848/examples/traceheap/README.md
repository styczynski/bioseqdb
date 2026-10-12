# traceheap

Educational PostgreSQL 17 table access method from the BioseqDB blog. It keeps
heap storage and counts selected callbacks in each backend process. It is not
BioseqDB's columnar storage engine or a production extension.

Read the tutorial:
https://bioseqdb.com/blog/writing-a-postgres-table-access-method-in-cpp/

Build with a C++ compiler and PostgreSQL 17 server development files:

```sh
make PG_CONFIG=/path/to/postgresql-17/bin/pg_config
make PG_CONFIG=/path/to/postgresql-17/bin/pg_config install
psql -X -v ON_ERROR_STOP=1 -d your_disposable_database -f test.sql
```

The database must be fresh and the connecting role must be able to create an
access method (superuser). `test.sql` creates the extension and its test tables.
It stops on an assertion failure. Tested with PostgreSQL 17.10 on macOS ARM64;
other major versions are explicitly rejected by the source guard.

Supported teaching paths tested here: sequential scans, ordinary insertion,
COPY, update/delete, rollback, ANALYZE, ordinary VACUUM, native-heap TOAST and
TRUNCATE. User indexes, primary keys/unique constraints requiring an index,
index validation and the clustering rewrite callback (including VACUUM FULL)
are explicitly unsupported. This is not a promise that all other operations
work. Test any new operation before adding it to the example's scope.

`traceheap.stats()` and `traceheap.reset()` refer only to the current backend.
Counters are not transactional, persistent, per-table, or aggregated across
parallel workers. Sampling and other delegated scan paths are not all counted.

No C++ object requiring destruction is held across a PostgreSQL call that can
raise an error. PostgreSQL errors do not provide ordinary C++ stack unwinding.

SPDX-License-Identifier: Apache-2.0
