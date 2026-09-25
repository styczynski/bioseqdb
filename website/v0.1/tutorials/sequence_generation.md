---
title: Generate deterministic sequence tables
slug: sequence_generation
description: Create small reproducible DNA4 tables and query-target truth sets directly from SQL.
order: 30
keywords: [sequence generation, synthetic data, reproducibility, seed, query target, dna4]
draft: false
---

Bioseq has two SQL-visible generators. They create `USING bioseq` tables in the
current creation schema, stream sequence payloads into storage, and return the
new relation OID. Cast that OID to `regclass` when a stable relation name is
more useful in output.

Use them for examples, query development, index exercises, and route checks
that need repeatable input. They are not biological simulators for every
alphabet and they do not write FASTA or FASTQ files.

Prerequisite: understand the table access method and sequence layout from
[Columnar storage and vectorized aggregates](../columnar_storage_and_vectorized_aggregates/).

## Choose the generator for the task

`bioseq.create_sample_table` creates independent, uniformly generated DNA4
sequences. Its arguments are:

```sql
bioseq.create_sample_table(
    table_name,
    n_sequences,
    sequence_bp_size,
    alphabet,
    seed
)
```

The output schema is:

```text
id  bigint primary key
seq bioseq.seq('dna4', 32)
```

`bioseq.create_query_target_sample_table` creates query-target pairs and the
coordinates used to check an alignment. Its arguments are:

```sql
bioseq.create_query_target_sample_table(
    table_name,
    fragment_count,
    fragment_length_mean,
    fragment_length_stddev,
    reference_length,
    substitution_rate,
    insertion_rate,
    deletion_rate,
    insertion_length_mean,
    deletion_length_mean,
    reverse_complement_rate,
    seed
)
```

The output has `query` and `target` columns bound to
`bioseq.seq('dna4', 64)`, plus `truth_target_start`, `truth_target_end`,
`is_reverse`, `effective_error_rate`, and `original_query_length`.

The first generator defaults to `dna4` and seed `42`. Its SQL route currently
rejects any other alphabet. The query-target generator is also fixed to DNA4.
The generic C++ generator supports more alphabets, but that does not make them
available through these SQL functions.

The current SQL distributions are:

| Generated property | Distribution or rule |
| --- | --- |
| Independent sequence and reference symbols | Uniform over A, C, G, and T |
| Original query length | Gaussian around `fragment_length_mean` and `fragment_length_stddev`, rounded and clamped to fit the reference |
| Query start | Uniform over starts where the original fragment fits |
| Substitution, insertion, and deletion events | Independent per visited source position using the three supplied rates |
| Insertion and deletion length | Gaussian around the supplied mean, rounded at zero, with standard deviation fixed to `1.0` by the SQL interface |
| Reverse orientation | Bernoulli choice using `reverse_complement_rate` |

`effective_error_rate` is the sum of the configured substitution, insertion,
and deletion rates. It is not a measured edit fraction for that generated row.
The realized query length can differ from `original_query_length` after indels.

## Generate equal-length sequences

The generator emits exactly `n_sequences` values with the requested logical
length. A seed makes the values repeatable:

```sql
SELECT bioseq.create_sample_table(
    'generated_reads',
    100,
    150,
    'dna4',
    20260816
)::regclass AS created_table;
```

Inspect biological length with [[sql:bioseq.len]]. Do not infer it from the
packed width in the column type:

```sql
SELECT id,
       bioseq.len(seq) AS bases,
       bioseq.symbol_count(seq, 'G') AS guanines
FROM generated_reads
ORDER BY id
LIMIT 10;
```

`n_sequences` must be from 1 through 100,000,000. `sequence_bp_size` may be
zero but cannot be negative. A zero-length sequence is a valid value and does
not need a payload fragment relation.

Choose a new table name or drop the old table before regenerating. The function
uses `CREATE TABLE`, so a name collision is an error rather than an implicit
overwrite. The name is treated as one SQL identifier. Select the intended
creation schema through `search_path` rather than passing `schema.table` as the
name. Both generators create their fixed output schema. They do not append to
an existing application table.

For the query-target generator, `table_name` is required. A supplied
`fragment_count` must be at least 1, `fragment_length_mean` must not be
negative, and `reference_length` must be at least 1. Use probabilities from
0 through 1 and nonnegative distribution parameters. The SQL wrapper does not
apply a broad semantic validator to every floating-point argument, so validate
an application-supplied configuration before calling it.

## Generate query-target truth pairs

The query-target generator first creates a deterministic reference, samples a
fragment length around the requested mean and standard deviation, then applies
substitution and indel events. `reverse_complement_rate` controls how often a
query uses the reverse-complement orientation.

For an exact, easy-to-read fixture, set all error rates to zero:

```sql
SELECT bioseq.create_query_target_sample_table(
    'generated_pairs',
    8,
    64,
    0.0,
    512,
    0.0,
    0.0,
    0.0,
    1.0,
    1.0,
    0.5,
    20260816
)::regclass AS created_table;
```

Each row contains its own query, the reference target, and the source interval.
For a reversed row, `query` is reverse-complemented while the truth interval
still refers to the target coordinate system.

Use a small nonzero substitution rate when testing scoring or edit projections.
Add insertion or deletion rates when testing CIGAR handling. Keep the seed and
all parameters beside a saved result because changing any of them describes a
different generated data set.

## Run both generators as a reproducible example

The following block demonstrates same-seed equality for the simple generator and
shows the complete truth shape produced by the pair generator.

```sql runnable id=deterministic_sequence_generation timeout=25s entity="sql:function:bioseq.symbol_count(bioseq.seq,text)"
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
-- bioseq:visible
SELECT bioseq.create_sample_table(
    'tutorial_generated_a', 4, 48, 'dna4', 314159
)::regclass AS created_table;
SELECT bioseq.create_sample_table(
    'tutorial_generated_b', 4, 48, 'dna4', 314159
)::regclass AS created_table;

SELECT a.id,
       bioseq.len(a.seq) AS bases,
       bioseq.symbol_count(a.seq, 'A') AS adenines,
       a.seq = b.seq AS same_seed_same_sequence
FROM tutorial_generated_a AS a
JOIN tutorial_generated_b AS b USING (id)
ORDER BY a.id;

SELECT bioseq.create_query_target_sample_table(
    'tutorial_generated_pairs',
    3,
    48,
    0.0,
    256,
    0.0,
    0.0,
    0.0,
    1.0,
    1.0,
    0.5,
    271828
)::regclass AS created_table;

SELECT id,
       bioseq.len(query) AS query_bases,
       bioseq.len(target) AS target_bases,
       truth_target_start,
       truth_target_end,
       is_reverse,
       effective_error_rate,
       original_query_length
FROM tutorial_generated_pairs
ORDER BY id;
-- bioseq:teardown
DROP TABLE tutorial_generated_pairs;
DROP TABLE tutorial_generated_b;
DROP TABLE tutorial_generated_a;
RESET max_parallel_workers_per_gather;
```

The generator-call result tables show stable relation names because the example
casts each returned OID to `regclass`. The equality table should report `true`
for every same-seed pair. The truth table has three rows, a 48-symbol original
query length, a 256-symbol target, and zero effective error rate. Strand
orientation can differ by row because its probability is 0.5.

## Put generated data through the normal lifecycle

Generated relations are ordinary Bioseq tables after creation. You can:

- add scalar metadata columns and update them
- use `bioseq.update_index` to build a persisted target form
- run `ANALYZE` for PostgreSQL planner statistics
- search a generated target with [[sql:bioseq.mmsearch]]
- compare CPU, SIMD, and supported GPU plans with the same seed
- inspect columnar shards and index metadata through their public functions

For a repeated many-query workload, put targets in one table and generated
queries in another, then use the `BioseqBatchJoin` shape from the multi-column
tutorial. The query-target generator repeats a reference target per row, which
is convenient for truth checks but is not always the best normalized
application schema.

## Understand output and memory behavior

The SQL wrappers write tables directly. They do not return a giant array of
sequence text to the client. The simple generator streams long sequence values
into storage fragments. The query-target generator emits bounded waves of
pairs under the scheduler's memory admission rules.

This keeps client transfer small, but the requested table still consumes the
full stored payload. Start with a few rows and short sequences. A request near
the 100-million-row safety limit is an administrative data-generation job, not
an interactive tutorial query.

There is no SQL argument that selects a Vulkan random-number route. Do not
infer GPU generation from `bioseq.aligner.vulkan.enabled`.
Alignment settings apply later when a search runs.

## Move from synthetic input to real files

Use generation when deterministic content matters more than biological source
provenance. Use `bioseq.load` for FASTA, FASTQ, and supported gzip files.
Use the external-provider relations for administrator-enabled remote nucleotide
sources.

Next: [Load FASTA, FASTQ, and gzip data](../fasta_fastq_gzip_and_io/) covers
local files, header mapping, bounded streaming, and the boundary between the SQL
loader and Bioseq's separate C++ batch reader.
