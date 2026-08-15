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

Install the extension once in each database that stores Bioseq columns:

```sql
CREATE EXTENSION bioseq;
```

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

Use `dna4` when every value contains only A, C, G, and T. Use `dna5` when N is
also a valid symbol and the installed build includes that alphabet:

```sql
bioseq.seq('dna5', 64)
```

The alphabet is part of the SQL type. Values with different alphabets are not
interchangeable.

## Round-tripping sequence values

Searchable sequence tables use the `bioseq` table access method. The example
below creates a small table, keeps its indexes current during inserts, and reads
the values back with their length in biological symbols using
[[sql:bioseq.len]].

```sql runnable id=sequence_roundtrip timeout=10s entity="sql:function:bioseq.len(bioseq.seq)"
-- bioseq:setup
SET bioseq.index.insert_maintenance.enabled = on;
CREATE TABLE tutorial_reads (
    id integer PRIMARY KEY,
    sequence bioseq.seq('dna4', 16)
) USING bioseq;
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
> you need. Do not use it as a sequence-length constraint.

## Loading a FASTA file

[[sql:bioseq.load|`bioseq.load`]] reads a server-side FASTA or FASTQ file. This
example maps the UniProt accession in each FASTA header to a text column. The
function returns the number of loaded records.

```sql runnable id=sequence_load timeout=20s
-- bioseq:setup
-- bioseq:visible
CREATE TABLE tutorial_loaded_reads (
  row_no bigserial PRIMARY KEY,
  accession text NOT NULL,
  seq bioseq.seq('dna4', 64)
) USING bioseq;

SET bioseq.index.insert_maintenance.enabled = on;

SELECT bioseq.load(
  'tutorial_loaded_reads'::regclass,
  'landing_reads.fasta',
  'uni_prot',
  ARRAY[
    ('accession', 'accession')::bioseq.column_mapping
  ]
);

SELECT accession,
       bioseq.len(seq) AS bases,
       seq::text AS sequence
FROM tutorial_loaded_reads
ORDER BY accession;
-- bioseq:teardown
DROP TABLE tutorial_loaded_reads;
```

The file path is resolved by the PostgreSQL server. The database role needs
permission to read the file.

## Input errors

Input is checked against the column alphabet. N is not valid for `dna4`, so
this statement fails with SQLSTATE `22P02`:

```sql
SELECT 'ACGN'::bioseq.seq('dna4', 64);
```

An explicit cast or assignment between `dna4` and `dna5` fails with SQLSTATE
`42804`:

```sql
SELECT 'A'::bioseq.seq('dna4', 64)::bioseq.seq('dna5', 64);
```

Drop a tutorial table when you no longer need it:

```sql
DROP TABLE tutorial_reads;
```
