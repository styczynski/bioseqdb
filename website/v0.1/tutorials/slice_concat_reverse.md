---
title: Slice, concatenate, reverse, and measure sequences
slug: slice_concat_reverse
description: Work with sequence regions and transforms while keeping Bioseq values typed and compact.
order: 20
keywords: [slice, concat, reverse, reverse complement, length, lazy sequence]
draft: false
---

Bioseq lets SQL work with a region of a [[sql:bioseq.seq]] value without first
converting the whole value to text. You can index one symbol, take a half-open
slice, concatenate compatible values, reverse a sequence, compute its reverse
complement, and ask [[sql:bioseq.len]] for its biological length or
`bioseq.unit_len` for its packed length.

This tutorial starts with exact slice rules, then combines the operations into
queries that remain useful for long stored values.

## Create a small table to explore

The examples use one short sequence so every bound is easy to check:

```sql
CREATE TABLE slice_reads (
    read_id integer PRIMARY KEY,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
```

```sql
SET bioseq.index.insert_maintenance.enabled = on;
```

```sql
INSERT INTO slice_reads VALUES
    (1, 'ACGTA'),
    (2, 'TTCCA'),
    (3, 'GGACT');
```

The first value has five symbols at positions 0 through 4.

```text
position     0  1  2  3  4
symbol       A  C  G  T  A
```

## Index one symbol

Sequence indexing is zero-based:

```sql
SELECT sequence[0]::text AS first_symbol,
       sequence[4]::text AS fifth_symbol
FROM slice_reads
WHERE read_id = 1;
```

The result is `A` and `A`.

Negative indexes count back from the end. `-1` is the last symbol:

```sql
SELECT sequence[-1]::text AS last_symbol,
       sequence[-5]::text AS first_symbol
FROM slice_reads
WHERE read_id = 1;
```

Both expressions return `A` for `ACGTA`.

An index outside the value returns NULL:

```sql
SELECT sequence[5] IS NULL AS after_end,
       sequence[-6] IS NULL AS before_start
FROM slice_reads
WHERE read_id = 1;
```

This returns true for both columns. It gives a query a simple way to test an
optional position without reading past the sequence.

If the sequence value itself is NULL, indexing also returns NULL. A `NOT NULL`
column avoids that second source of missing values when it does not fit your
data model.

## Take half-open slices

A slice includes its lower bound and excludes its upper bound. The expression
`sequence[1:4]` returns positions 1, 2, and 3:

```sql
SELECT sequence[1:4]::text
FROM slice_reads
WHERE read_id = 1;
```

The result is `CGT`.

Omit the lower bound to start at position 0:

```sql
SELECT sequence[:2]::text
FROM slice_reads
WHERE read_id = 1;
```

The result is `AC`.

Omit the upper bound to continue to the end:

```sql
SELECT sequence[2:]::text
FROM slice_reads
WHERE read_id = 1;
```

The result is `GTA`.

The span length of `[lower:upper]` is normally `upper - lower`. Half-open
bounds also compose cleanly. Splitting at position 2 produces adjacent pieces
without duplicating or losing a symbol:

```sql
SELECT sequence[:2]::text AS left_piece,
       sequence[2:]::text AS right_piece
FROM slice_reads
WHERE read_id = 1;
```

## Use negative slice bounds

Negative bounds are resolved from the end of the value. These three
expressions all select `CGT` from `ACGTA`:

```sql
SELECT sequence[1:4]::text AS positive_bounds,
       sequence[1:-1]::text AS negative_upper,
       sequence[-4:-1]::text AS negative_bounds
FROM slice_reads
WHERE read_id = 1;
```

Use a negative upper bound when an application wants to drop a fixed suffix:

```sql
SELECT read_id,
       sequence[:-2]::text AS without_last_two
FROM slice_reads
ORDER BY read_id;
```

Use a negative lower bound for a fixed-size suffix:

```sql
SELECT read_id,
       sequence[-3:]::text AS last_three
FROM slice_reads
ORDER BY read_id;
```

## Understand clamping and empty slices

Slice bounds are clamped to the available sequence. A large outer range
returns the whole value:

```sql
SELECT sequence[-100:100]::text
FROM slice_reads
WHERE read_id = 1;
```

The result is `ACGTA`.

A lower bound at or after the upper bound returns an empty sequence:

```sql
SELECT bioseq.len(sequence[3:3]) AS equal_bounds,
       bioseq.len(sequence[4:2]) AS reversed_bounds
FROM slice_reads
WHERE read_id = 1;
```

Both lengths are zero. The result is a typed empty sequence, not NULL.

That distinction is useful in aggregates and transformations. An empty slice
can be concatenated with another compatible sequence. A NULL value propagates
through strict sequence operations.

## Omitted bounds are not NULL bounds

An omitted bound means open-ended. A bound whose SQL value is NULL makes the
slice result NULL:

```sql
SELECT sequence[:2] IS NULL AS omitted_lower_is_null,
       sequence[NULL::bigint:2] IS NULL AS null_lower_is_null
FROM slice_reads
WHERE read_id = 1;
```

The first result is false and the second is true.

This matters for parameterized slices. If a client parameter can be NULL, do
not expect Bioseq to interpret it as an omitted colon bound. Choose the desired
behavior in SQL:

```sql
SELECT CASE
         WHEN $1::bigint IS NULL THEN sequence[:$2::bigint]
         ELSE sequence[$1::bigint:$2::bigint]
       END
FROM slice_reads
WHERE read_id = $3;
```

The `CASE` makes the application's NULL policy visible.

## Keep slicing one-dimensional

Bioseq supports one-dimensional indexing and slices with a step of one. It does
not expose Python-style stepped slices such as every second base. It also does
not treat a sequence as a multidimensional array.

For a regular sampling operation, create positions with PostgreSQL and index
the sequence deliberately:

```sql
SELECT p.position,
       r.sequence[p.position]::text AS symbol
FROM slice_reads AS r
CROSS JOIN LATERAL generate_series(
    0,
    bioseq.len(r.sequence) - 1,
    2
) AS p(position)
WHERE r.read_id = 1
ORDER BY p.position;
```

This says exactly which positions become rows. It avoids implying that a
stepped Bioseq subscript exists.

## Concatenate two sequences

The Bioseq concatenation operator is in the `bioseq` schema. With `bioseq` on
the search path, two compatible sequences can use `||` directly:

```sql
SET search_path = bioseq, public, pg_catalog;
```

```sql
SELECT (
    'AC'::bioseq.seq('dna4', 32)
    ||
    'GTA'::bioseq.seq('dna4', 32)
)::text;
```

The result is `ACGTA`.

When an application keeps a restrictive search path, qualify the operator:

```sql
SELECT (
    'AC'::bioseq.seq('dna4', 32)
    OPERATOR(bioseq.||)
    'GTA'::bioseq.seq('dna4', 32)
)::text;
```

Schema qualification is important for operators. Writing `bioseq.||` is not
valid PostgreSQL operator syntax. `OPERATOR(bioseq.||)` is the explicit form.

## Concatenate sequences and text

Bioseq provides typed concatenation in both directions:

```sql
SELECT (
    'ACG'::bioseq.seq('dna4', 32)
    OPERATOR(bioseq.||)
    'TA'::text
)::text AS sequence_then_text;
```

```sql
SELECT (
    'AC'::text
    OPERATOR(bioseq.||)
    'GTA'::bioseq.seq('dna4', 32)
)::text AS text_then_sequence;
```

Both results are `ACGTA`. The text part is checked against the sequence
alphabet. Appending `N` to a `dna4` value fails with SQLSTATE `22P02`:

```sql
SELECT 'ACGT'::bioseq.seq('dna4', 32)
       OPERATOR(bioseq.||)
       'N'::text;
```

This prevents a transform from introducing a symbol that the result type
cannot represent.

## Concatenate symbol arrays

Arrays can be useful when another SQL operation already produces individual
symbols. Bioseq accepts text arrays and `bioseq.letter` arrays in its
concatenation family.

Prepend a text array:

```sql
SELECT (
    ARRAY['A', 'C']::text[]
    OPERATOR(bioseq.||)
    'GTA'::bioseq.seq('dna4', 32)
)::text;
```

Append a `bioseq.letter` array derived from another value:

```sql
SELECT (
    'AC'::bioseq.seq('dna4', 32)
    OPERATOR(bioseq.||)
    bioseq.to_array('GTA'::bioseq.seq('dna4', 32))
)::text;
```

The results are `ACGTA`.

An empty compatible piece is an identity for concatenation:

```sql
SELECT bioseq.len(
    'ACGTA'::bioseq.seq('dna4', 32)
    OPERATOR(bioseq.||)
    ''::text
) AS unchanged_length;
```

The result is 5.

Concatenation is strict. If either operand is NULL, the result is NULL:

```sql
SELECT (
    'ACGTA'::bioseq.seq('dna4', 32)
    OPERATOR(bioseq.||)
    NULL::text
) IS NULL;
```

Use `COALESCE` only when treating NULL as an empty piece is truly the business
rule:

```sql
SELECT 'ACGTA'::bioseq.seq('dna4', 32)
       OPERATOR(bioseq.||)
       COALESCE($1::text, '');
```

## Keep alphabets compatible

Both sequence operands must have compatible bound types. A `dna4` sequence and
a `dna5` sequence do not silently choose a wider common alphabet. An operation
across different bound alphabets fails with SQLSTATE `42804`.

Choose the table alphabet when you design the data model. Validate external
text as it enters that type. Do not depend on concatenation to convert between
biological alphabets.

Storage width is different from alphabet. Values with the same alphabet can be
cast between supported storage widths:

```sql
SELECT (
    'AC'::bioseq.seq('dna4', 16)::bioseq.seq('dna4', 64)
    OPERATOR(bioseq.||)
    'GTA'::bioseq.seq('dna4', 64)
)::text;
```

The result remains `ACGTA`. The cast changes packed representation, not the
symbols.

## Reverse a sequence

`bioseq.reverse` changes symbol order without complementing bases:

```sql
SELECT bioseq.reverse(
    'ACGTA'::bioseq.seq('dna4', 32)
)::text;
```

The result is `ATGCA`.

Use it on a stored slice:

```sql
SELECT read_id,
       bioseq.reverse(sequence[1:4])::text AS reversed_middle
FROM slice_reads
ORDER BY read_id;
```

Reversing an empty sequence returns an empty sequence. Reversing a NULL value
returns NULL.

If the session search path contains only `pg_catalog` and `public`, call the
schema-qualified function as shown. An unqualified `reverse(sequence)` is not a
portable Bioseq call.

## Compute a reverse complement

`bioseq.reverse_complement` reverses the sequence and complements each symbol
under the bound alphabet:

```sql
SELECT bioseq.reverse_complement(
    'ACGTA'::bioseq.seq('dna4', 32)
)::text;
```

The result is `TACGT`.

This is useful when a stored interval has strand `-1` and the caller wants the
forward reading orientation:

```sql
SELECT CASE strand
         WHEN 1 THEN sequence
         WHEN -1 THEN bioseq.reverse_complement(sequence)
       END AS oriented_sequence
FROM imported_intervals;
```

The example assumes that `strand` has only 1 and -1. Add an `ELSE` branch if
the table allows unknown or unstranded values.

Reverse complement requires a bound alphabet with complement rules. A typmodless
`bioseq.seq` value can carry symbols, but alphabet-dependent operations need a
bound type. Cast the value to the known alphabet first:

```sql
SELECT bioseq.reverse_complement(
    $1::bioseq.seq('dna4', 32)
);
```

## Measure symbols and packed units

`bioseq.len` reports biological symbols:

```sql
SELECT bioseq.len('ACGTA'::bioseq.seq('dna4', 32));
```

The result is 5.

`bioseq.unit_len` reports packed storage units:

```sql
SELECT bioseq.unit_len('ACGTA'::bioseq.seq('dna4', 32));
```

Do not compare the two without naming their units. The number of packed units
depends on alphabet and the active physical storage width. The number of
symbols does not. A build profile can normalize several accepted SQL widths to
one physical width, so inspect the bound column type with `format_type` before
using packed-unit counts to compare deployments.

Measure a transformed expression directly:

```sql
SELECT bioseq.len(
    bioseq.reverse(sequence[1:-1])
    OPERATOR(bioseq.||)
    'AC'::text
) AS output_symbols
FROM slice_reads
WHERE read_id = 1;
```

There is no need to cast the intermediate expression to text just to count it.

## Build derived targets from pieces

The operations compose. This query trims one symbol from each side, reverses
the middle, and adds fixed adapters:

```sql
SELECT read_id,
       (
           'AC'::text
           OPERATOR(bioseq.||)
           bioseq.reverse(sequence[1:-1])
           OPERATOR(bioseq.||)
           'GT'::text
       ) AS derived_target
FROM slice_reads
ORDER BY read_id;
```

Keep the expression typed as `bioseq.seq` when a later Bioseq operation consumes
it. Cast to text only at the output boundary:

```sql
SELECT read_id,
       (
           'AC'::text
           OPERATOR(bioseq.||)
           bioseq.reverse(sequence[1:-1])
           OPERATOR(bioseq.||)
           'GT'::text
       )::text AS derived_target_text
FROM slice_reads
ORDER BY read_id;
```

## Search a derived region

A sliced target can participate in a supported search shape. Keep ownership of
each `mmsearch` call clear, especially in joins:

```sql
SELECT read_id
FROM slice_reads
WHERE public.mmsearch(
    sequence[1:-1],
    'CGT'::bioseq.seq('dna4', 32)
)
ORDER BY read_id;
```

For application work, use a query long enough for the selected search
pipeline and dataset. The short literal above illustrates the typed expression,
not a recommended alignment threshold.

The multi-sequence tutorial shows how to give separate search predicates
separate target frontiers. Do not stack two `mmsearch` calls on the same target
frontier in one Boolean expression. That unsupported shape fails instead of
guessing which search owns the projected match state.

## See when payload materialization happens

Explain a metadata-only length first:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT bioseq.len(sequence[1:-1])
FROM slice_reads;
```

Then explain a query that sends text to the client:

```sql
EXPLAIN (ANALYZE, VERBOSE, COSTS OFF)
SELECT sequence[1:-1]::text
FROM slice_reads;
```

Bioseq can describe source-backed lazy expressions through these EXPLAIN
sections:

- `Projection Mode`
- `Source Pieces`
- `Estimated Piece Boundaries`
- `Slice Recipe`
- `Slice Materialization`

The recipe shows the expression graph. Each recipe node can report stable ID,
opcode, expression, dependencies, and lower or upper bounds. It describes what
must be computed without claiming that every payload was already copied.

The materialization section explains where a concrete value was required. A
materialization item can name the variable, expression, mechanism, reason,
dependencies, encoding, and cache scope. With costs enabled it can include
estimated symbols and bytes. With `ANALYZE` it can add actual calls, input and
output symbols, input and output bytes, reused calls, retained bytes, and time.

A length over a lazy slice can remain metadata-only. Casting the slice to text
is a client output boundary and requires symbols to be produced. Read the
reported mechanism and reason instead of inferring materialization from the SQL
spelling alone.

## A runnable operation table

This example puts the core rules in one result:

```sql runnable id=slice_concat_reverse timeout=20s entity="sql:function:bioseq.len(bioseq.seq)"
-- bioseq:setup
CREATE TABLE slice_operation_reads (
    read_id integer PRIMARY KEY,
    sequence bioseq.seq('dna4', 32) NOT NULL
) USING bioseq;
INSERT INTO slice_operation_reads VALUES (1, 'ACGTA');
-- bioseq:visible
SELECT sequence[0]::text AS first_symbol,
       sequence[-1]::text AS last_symbol,
       sequence[1:4]::text AS middle,
       sequence[-100:100]::text AS clamped,
       bioseq.len(sequence[4:2]) AS empty_length,
       (
           sequence[:2]
           OPERATOR(bioseq.||)
           sequence[2:]
       )::text AS rejoined,
       bioseq.reverse(sequence)::text AS reversed,
       bioseq.reverse_complement(sequence)::text AS reverse_complement,
       bioseq.len(sequence) AS symbols,
       bioseq.unit_len(sequence) AS packed_units
FROM slice_operation_reads
WHERE read_id = 1;
-- bioseq:teardown
DROP TABLE slice_operation_reads;
```

For the stored value `ACGTA`, the symbol operations have these stable results:

| Column | Result |
| --- | --- |
| `first_symbol` | `A` |
| `last_symbol` | `A` |
| `middle` | `CGT` |
| `clamped` | `ACGTA` |
| `empty_length` | `0` |
| `rejoined` | `ACGTA` |
| `reversed` | `ATGCA` |
| `reverse_complement` | `TACGT` |
| `symbols` | `5` |

`packed_units` is deliberately not fixed in this table. It follows the active
alphabet and physical width. The other values describe symbols and stay the
same across compatible storage widths.

## Common mistakes

If a slice is unexpectedly NULL, check whether a bound parameter is NULL. An
omitted bound is open-ended. A NULL bound is a NULL input.

If `||` is not found, inspect `search_path` and use
`OPERATOR(bioseq.||)`. The operator belongs to the `bioseq` schema.

If concatenation rejects a symbol, validate the text against the sequence
alphabet. `dna4` cannot accept `N`.

If an operation reports incompatible types, compare alphabets first. Storage
width can be converted within the same alphabet. Different bound alphabets do
not silently merge.

If reverse complement rejects a typmodless value, cast it to the alphabet that
the data is known to follow.

If a plan copies more data than expected, remove full-text projections and
inspect `Slice Materialization`. Ask for length, counts, or a smaller slice when
that is what the client needs.

## Clean up

Drop the scratch table when you finish:

```sql
DROP TABLE slice_reads;
```
