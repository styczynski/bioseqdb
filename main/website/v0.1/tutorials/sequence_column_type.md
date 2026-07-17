---
title: The bioseq.seq column type
slug: sequence_column_type
description: Store and query biological sequences with the bioseq.seq column type.
order: 10
keywords: [bioseq.seq, sequence type, DNA, symbol_count]
draft: false
---

# The bioseq.seq column type

Bioseq stores biological sequences in a compact PostgreSQL value called
[[sql:bioseq.seq|the sequence type]]. The alphabet and maximum length are part of
the type modifier, so a column declaration records exactly what a column may
hold. The shorthand `#bioseq.seq` refers to the same type throughout this page.

> [!NOTE]
> Sequence length is part of the type modifier. `bioseq.seq('dna4', 64)` stores
> DNA sequences of up to 64 bases over the four-letter `dna4` alphabet.

## Declaring a sequence column

A `bioseq.seq` column names its alphabet and maximum length directly in the type
modifier, exactly like `numeric(precision, scale)` records precision.

```sql id=seq_type_example annotated
CREATE TABLE reads (sequence bioseq.seq('dna4', 64));
```

```annotation target=seq_type_example match="bioseq.seq('dna4', 64)" title="The bioseq.seq column type"
The alphabet and maximum length are encoded in the PostgreSQL type modifier.
```

## Round-tripping sequence values

The example below creates a small table, inserts two reads, and reads them back
with their length and a per-symbol count. Use [[sql:bioseq.len]] for
the length in symbol units and [[sql:bioseq.symbol_count]] to
count occurrences of one symbol.

```sql runnable id=sequence_roundtrip timeout=10s entity="sql:function:bioseq.len(bioseq.seq)" entity="sql:function:bioseq.symbol_count(bioseq.seq,text)"
-- bioseq:setup
CREATE TABLE tutorial_reads (
    id integer PRIMARY KEY,
    sequence bioseq.seq('dna4', 16)
);
INSERT INTO tutorial_reads VALUES (1, 'ACGT'), (2, 'TTAA');
-- bioseq:visible
SELECT id,
       sequence::text AS sequence,
       bioseq.len(sequence) AS length
       -- TEMPORARY: bioseq.symbol_count(sequence, 'A') AS a_count
       -- aborts the backend (SIGABRT); see COUNT_BUG.md. Restore this
       -- column once symbol_count is fixed.
FROM tutorial_reads
ORDER BY id;
-- bioseq:teardown
DROP TABLE tutorial_reads;
```

> [!WARNING]
> This query scans every row. Add a predicate on an indexed column before
> running it against a large table.

## Ordering by sequence

Sequence values order lexicographically by their decoded symbols, so an
`ORDER BY` on a `bioseq.seq` column is stable and deterministic.

```sql runnable id=sequence_ordering timeout=10s
-- bioseq:setup
CREATE TABLE tutorial_ordered (sequence bioseq.seq('dna4', 8));
INSERT INTO tutorial_ordered VALUES ('TTAA'), ('ACGT'), ('AAAA');
-- bioseq:visible
SELECT sequence::text AS sequence
FROM tutorial_ordered
ORDER BY sequence;
-- bioseq:teardown
DROP TABLE tutorial_ordered;
```
