---
title: The bioseq.seq column type
slug: sequence_column_type
description: Store and query biological sequences with the bioseq.seq column type.
order: 10
keywords: [bioseq.seq, sequence type, DNA, packed storage]
draft: false
---

Bioseq stores biological sequences in a compact PostgreSQL value called
[[sql:bioseq.seq|the sequence type]]. Its type modifier records the biological
alphabet and the width of each packed storage unit. It does not impose a maximum
sequence length.

> [!NOTE]
> In `bioseq.seq('dna4', 64)`, `dna4` selects the four-symbol DNA alphabet and
> `64` selects 64-bit packed storage units. A value may contain more than 64
> bases.

## Reading the type modifier

The type modifier names its alphabet and storage-unit width directly:

```sql id=seq_type_example annotated
bioseq.seq('dna4', 64)
```

```annotation target=seq_type_example match="'dna4'" title="Alphabet"
The `'dna4'` modifier selects the four-symbol DNA alphabet: A, C, G, and T.
```

```annotation target=seq_type_example match="64" title="Storage-unit width"
The `64` modifier selects 64-bit packed storage units. It does not limit the sequence to 64 bases.
```

## Round-tripping sequence values

The example below creates a small table, inserts two reads, and reads them back
with their length in biological symbols using [[sql:bioseq.len]].

```sql runnable id=sequence_roundtrip timeout=10s entity="sql:function:bioseq.len(bioseq.seq)"
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
FROM tutorial_reads
ORDER BY id;
-- bioseq:teardown
DROP TABLE tutorial_reads;
```

> [!TIP]
> Choose the storage-unit width for the packing and execution characteristics
> you need; do not use it as a sequence-length constraint.

## Ordering decoded sequences

Cast a sequence to `text` when a query needs an explicit lexicographic order by
decoded symbols.

```sql runnable id=sequence_ordering timeout=10s
-- bioseq:setup
CREATE TABLE tutorial_ordered (sequence bioseq.seq('dna4', 8));
INSERT INTO tutorial_ordered VALUES ('TTAA'), ('ACGT'), ('AAAA');
-- bioseq:visible
SELECT sequence::text AS sequence
FROM tutorial_ordered
ORDER BY sequence::text;
-- bioseq:teardown
DROP TABLE tutorial_ordered;
```
