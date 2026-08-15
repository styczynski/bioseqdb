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

## Choose the alphabet from the data contract

An alphabet is validation, storage interpretation, and function behavior in
one type choice. Pick the narrowest alphabet that accepts every symbol your
dataset is allowed to contain.

| Alphabet | Accepted symbols in this tutorial | Typical use |
| --- | --- | --- |
| `dna4` | A, C, G, T | Resolved nucleotide calls with no unknown base |
| `dna5` | A, C, G, T, N | Nucleotide data where N is a valid unknown base |

The installed build decides which alphabets are registered. The standard
profile can be narrower than an all-alphabet build. A type declaration for an
alphabet that is not registered fails instead of substituting another
alphabet.

Do not remove `N` from input merely to fit `dna4`. If N is meaningful, use a
build and column type that support `dna5`. If N means that a row should be
rejected, keep `dna4` and let the input error enforce the rule.

Store the alphabet in schema, not in an application comment:

```sql
CREATE TABLE resolved_reads (
    read_id bigint PRIMARY KEY,
    sequence bioseq.seq('dna4', 64) NOT NULL
) USING bioseq;
```

```sql
CREATE TABLE reads_with_unknowns (
    read_id bigint PRIMARY KEY,
    sequence bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
```

The second statement requires a build that includes `dna5`.

## Choose a packed storage-unit width

Bioseq supports packed unit widths of 8, 16, 32, and 64 bits for registered
alphabet and width combinations:

```sql
bioseq.seq('dna4', 8)
```

```sql
bioseq.seq('dna4', 16)
```

```sql
bioseq.seq('dna4', 32)
```

```sql
bioseq.seq('dna4', 64)
```

The width describes the integer unit used by the packed representation. It is
not bits per symbol, a row limit, a sequence limit, or a requested CPU vector
width.

A 64-bit storage unit does not request a 64-bit-only search path. Highway SIMD
selection, PostgreSQL plan choice, and Vulkan routing have separate controls and
EXPLAIN fields.

For a first table, use the width already used by the surrounding dataset and
queries. Changing width without measuring the actual read, write, and search
workload adds conversion without necessarily improving it.

## Name the units in every report

Bioseq exposes two length functions with different units:

```sql
SELECT bioseq.len(
           'ACGTACGT'::bioseq.seq('dna4', 32)
       ) AS symbols,
       bioseq.unit_len(
           'ACGTACGT'::bioseq.seq('dna4', 32)
       ) AS packed_units;
```

`bioseq.len` returns biological symbols. `bioseq.unit_len` returns packed
storage units. Both return `bigint`.

Do not label `unit_len` as bases. Do not label `len` as bytes. Names such as
`sequence_symbols` and `packed_units` keep the result meaningful when width or
alphabet changes.

Compare widths without changing the symbols:

```sql
SELECT width_name, symbols, packed_units
FROM (
    VALUES
        ('w8',
         bioseq.len('ACGTACGTACGT'::bioseq.seq('dna4', 8)),
         bioseq.unit_len('ACGTACGTACGT'::bioseq.seq('dna4', 8))),
        ('w16',
         bioseq.len('ACGTACGTACGT'::bioseq.seq('dna4', 16)),
         bioseq.unit_len('ACGTACGTACGT'::bioseq.seq('dna4', 16))),
        ('w32',
         bioseq.len('ACGTACGTACGT'::bioseq.seq('dna4', 32)),
         bioseq.unit_len('ACGTACGTACGT'::bioseq.seq('dna4', 32))),
        ('w64',
         bioseq.len('ACGTACGTACGT'::bioseq.seq('dna4', 64)),
         bioseq.unit_len('ACGTACGTACGT'::bioseq.seq('dna4', 64)))
) AS values_by_width(width_name, symbols, packed_units)
ORDER BY width_name;
```

An empty sequence has zero symbols and zero packed units:

```sql
SELECT bioseq.len(
           ''::bioseq.seq('dna4', 32)
       ) AS symbols,
       bioseq.unit_len(
           ''::bioseq.seq('dna4', 32)
       ) AS packed_units;
```

An empty sequence is not NULL. It can participate in concatenation and can be
stored in a nullable or non-nullable column.

## Cast between widths of the same alphabet

Use an explicit cast when a query or destination column requires a different
packed width:

```sql
SELECT 'ACGTA'::bioseq.seq('dna4', 16)
               ::bioseq.seq('dna4', 64);
```

The symbols and alphabet remain the same. Only the packed representation
changes.

Bind application parameters at the target width:

```sql
PREPARE insert_resolved_read(bigint, text) AS
INSERT INTO resolved_reads (read_id, sequence)
VALUES ($1, $2::bioseq.seq('dna4', 64));
```

```sql
EXECUTE insert_resolved_read(1, 'ACGTACGT');
```

The text parameter is checked as `dna4` before assignment.

```sql
DEALLOCATE insert_resolved_read;
```

## Do not cast directly between bound alphabets

A bound `dna4` value and a bound `dna5` value have different biological types.
Bioseq does not silently widen or narrow them.

If an import must narrow a known `dna5` value to `dna4`, make validation
explicit through text:

```sql
SELECT source_sequence::text::bioseq.seq('dna4', 64)
FROM incoming_sequences
WHERE source_sequence::text ~ '^[ACGT]*$';
```

The regular expression documents the row policy. The target cast remains the
final symbol validator.

Use this boundary for controlled import work, not as an automatic conversion in
every query. If the application regularly needs N, model the destination with
`dna5` instead.

## Work with typmodless sequence values carefully

Some interfaces expose a typmodless `bioseq.seq`. The external-provider
`sequences` and `sequence_chunks` columns are examples. Relation options choose
their physical alphabet and width even though PostgreSQL reports typmod `-1` for
the column.

Operations that need only stored length or text can still work:

```sql
SELECT bioseq.len(sequence), sequence::text
FROM bioseq_external.sequences
WHERE provider = 'ucsc'
  AND assembly_database = 'hg38'
  AND chromosome = 'chrM'
  AND start0 = 0
  AND end0 = 100;
```

Alphabet-dependent operations need a bound alphabet. Indexing one letter,
turning a sequence into `bioseq.letter[]`, and reverse complement cannot infer
an alphabet from a bare typmodless SQL declaration.

At a trusted boundary, bind through validated text:

```sql
SELECT bioseq.reverse_complement(
    sequence::text::bioseq.seq('dna5', 64)
)
FROM bioseq_external.sequences
WHERE provider = 'ucsc'
  AND assembly_database = 'hg38'
  AND chromosome = 'chrM'
  AND start0 = 0
  AND end0 = 100;
```

Use the actual external relation options and installed alphabet. Do not guess
the alphabet from one returned row.

## Convert to client-friendly forms at the boundary

Cast to text when a client needs the complete symbol string:

```sql
SELECT sequence::text
FROM resolved_reads
WHERE read_id = 1;
```

Keep the value as `bioseq.seq` while using Bioseq functions. A text round trip
adds parsing and loses the bound type until it is cast back.

Convert to a letter array when an array operation is specifically needed:

```sql
SELECT bioseq.to_array(sequence)
FROM resolved_reads
WHERE read_id = 1;
```

For a preview, a typed slice is smaller than a whole-value text cast:

```sql
SELECT sequence[:20]::text AS prefix,
       sequence[-20:]::text AS suffix
FROM resolved_reads
WHERE read_id = 1;
```

The slice tutorial covers the zero-based and half-open bound rules.

## Compare values under the sequence contract

Sequence ordering is lexicographic under the bound alphabet. Equality and hash
behavior compare the biological sequence rather than treating a same-alphabet
storage width as different content.

Use a common explicit type when a query combines values from different widths:

```sql
SELECT (
    'ACGTA'::bioseq.seq('dna4', 16)::bioseq.seq('dna4', 64)
    OPERATOR(bioseq.=)
    'ACGTA'::bioseq.seq('dna4', 64)
) AS same_symbols;
```

The result is true.

The comparison operators belong to the `bioseq` schema. With a restrictive
search path, use `OPERATOR(bioseq.=)`, `OPERATOR(bioseq.<)`, and the matching
qualified form instead of relying on unqualified operator lookup.

Do not compare different bound alphabets as if they were different encodings of
one SQL type. Convert through a deliberate validation boundary or keep them in
separate query branches.

## Put more than one sequence type in a table

Each sequence column has its own type modifier:

```sql
CREATE TABLE paired_sequence_storage (
    pair_id bigint PRIMARY KEY,
    resolved bioseq.seq('dna4', 32) NOT NULL,
    with_unknowns bioseq.seq('dna5', 64) NOT NULL
) USING bioseq;
```

This requires a build that includes both alphabets. A query must respect each
column's type:

```sql
SELECT pair_id,
       bioseq.len(resolved) AS resolved_symbols,
       bioseq.len(with_unknowns) AS symbols_with_unknowns
FROM paired_sequence_storage;
```

Do not feed `with_unknowns` directly as a query for `resolved`. Their alphabets
are not interchangeable. The multi-sequence tutorial uses two columns with the
same `dna4` type for stored-target and stored-query joins.

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

Choose the destination alphabet before loading. `bioseq.load` writes through
the destination column type, so a file containing N fails for a `dna4` column.
Use a staging and validation workflow if the source alphabet is not already
known.

For a large file, decide whether insert maintenance stays on during the load or
whether one `bioseq.update_index` runs afterward. The fast read, search, and
write tutorial shows both patterns.

## Inspect metadata work with EXPLAIN

Length and symbol counts can be answered without returning a full sequence to
the client:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT bioseq.len(sequence),
       bioseq.symbol_count(sequence, 'A')
FROM resolved_reads;
```

On a `BioseqScan`, `Bioseq Fetched From Meta` lists supported metadata
projections. `Bioseq Projected Columns` shows every column the node supplies.

Adding `sequence::text` changes the workload:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT bioseq.len(sequence),
       sequence::text
FROM resolved_reads;
```

Read the plan for the exact statement sent by the application. A metadata fast
path for one expression does not remove payload work required by another.

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

An unsupported alphabet or width fails with SQLSTATE `22023`. This can happen
when a deployment uses a narrower build profile than the schema expects.

An unqualified Bioseq operator can fail with SQLSTATE `42883` when `bioseq` is
not on `search_path`. Qualify the operator instead of broadening a security-
sensitive search path:

```sql
SELECT 'AC'::bioseq.seq('dna4', 32)
       OPERATOR(bioseq.||)
       'GT'::text;
```

Treat these errors differently in an importer:

- `22P02` means input text contains a symbol the selected alphabet rejects
- `42804` means bound sequence types are incompatible
- `22023` means a requested alphabet or width is not supported by this build
- `42883` can mean an operator was not found under the current search path

Return enough context to identify the source record and destination column. Do
not include an entire very large sequence in an error log.

## A schema checklist

Before creating a long-lived sequence table, answer these questions:

1. Can N occur, and is it meaningful or invalid
2. Which alphabets are included in every deployment build
3. Which packed width will the table use
4. Which columns can be NULL and which can hold an empty sequence
5. Whether ordinary writes maintain the index immediately
6. How bulk loads publish a completed index generation
7. Which queries need full text, slices, length, symbol counts, or search
8. Whether stored query columns have the same alphabet as stored targets

Put the resulting alphabet and width in the SQL type modifier. That lets
PostgreSQL validate the contract at every write boundary.

Drop a tutorial table when you no longer need it:

```sql
DROP TABLE tutorial_reads;
```
