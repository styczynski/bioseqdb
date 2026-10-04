---
title: "PostgreSQL API and custom scans"
slug: project_postgres_custom_scan
description: "PostgreSQL API and custom scans"
order: 90
keywords: [BioseqDB, project]
draft: false
---

\[v0.8.2\] \[REL_17_10\]

## Designing SQL API for an aligner

<a id="sec-postgres-custom-scan-scope"></a>

In the previous chapter I presented how the sequence abstrations work and how they interact with the workers, we also had opportunity to look on how batching is done on figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle). This chapter focuses on the main problem - how I incorporated this infrastructure into Postgres.

First, I want to discsuss how we can design SQL API for the aligner. Currently BioseqDB can be used in the following way:

```sql runnable id=project_chapters-10_postgres_custom_scan-01_engineering_problem_0 timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
-- bioseq:visible
CREATE TABLE targets (
  id bigint PRIMARY KEY,
  seq bioseq.seq('dna5', 64)
) USING bioseq;

SELECT t.id,
       bioseq.dp_score(t.seq) AS score,
       bioseq.target_start(t.seq) AS start,
       bioseq.cigar(t.seq) AS cigar
FROM targets AS t
WHERE t.id > 20
  AND mmsearch(t.seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna5', 64));
-- bioseq:teardown
DROP TABLE targets;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Search once and return the fields of each hit.

The first `mmsearch` argument is the target sequence and the second is the query, - here query is supplied by `$1` (text parameter in prepared statement). The `score` alias means the dynamic-programming score of the current hit, CIGAR string describes the alignment and we also request position of the match in the sequence coming from target table.

One insteresting observation is that naturally each alignment can produce multiple hits, so this operation can return multiple rows for a single input row. We intuitively expect that there will be two kinds of selected expressions: Some of them will not depend on alignment (in this case it’s primary key), so they will be the same for all these hits, while some of them like score and cigar will be different. Figure [scan occurrences](../project_postgres_custom_scan/#fig-scan-occurrences) illustrates that alignment should be set-returning function.

<a id="fig-scan-occurrences"></a>

![01 engineering problem diagram](/static/figures/project/chapters-10-postgres-custom-scan-01-engineering-problem-tikz-01.svg)

*Searching `XYX` for `X` gives two occurrences, so the function needs to "clone" metadata of tuple (id key in this case) and for each generated tuple fill-in the alignment hit position.*

The first idea for implementation of such search was the simplest one - implementing alignment as callable Postgres function, which is very easy to do in C++. The function accepts query as raw SQL string:

```sql
SELECT * FROM search_sql(
  'targets',
  'SELECT id, seq FROM reads WHERE id > 20'
);
```

An early interface sketch passes SQL as text.

This however introduces serveral problems:

- **Which is the column we want to align?** We can make a choice to hardcode column name to be `seq` at all times, but this limits flexibility heavily. Other choice is to allow passing the aligned column name as a parameter, which resolves this problem.

- **How do optimiser know about inner query?** As the inner query can be complex the optimiser doesn’t know about it. We can call libpq inside the function, which can optimise the query separately, but then we should change it depending on the context: you can imagine very trivial case that the outer SELECT statement only selects primary key for some reason, so we don’t have to do any alignment. In that case we would expect the function to do this optimisation, but this means that its implementation needs to do the same work as Postgres optimizer.

```sql
-- Hypothetical SQL-string interface
SELECT h.*
FROM search_sql('targets', 'SELECT id, seq FROM reads') AS h
WHERE h.target_id = 27;
-- The function must itself restrict the hidden target scan.
```

An outer condition cannot automatically filter a hidden scan.

The listing illustrates point stated by me: the function sees the target table through the string `'targets'`, but it’s used in a context, where we will do only one alignment. If the function doesn’t interpret its context, then it will perform alignment on entire table. The solution could be to pass this reponsibility to the user, but this generates huge inconvenience - for more complex queries, user would need to transfer SQL statements to and from the raw text pased to search function and manually be responsible for optimisation. In that case this should mean that when we forgot to do that the different is enormous - the statement will align two tables instead of one (could be orders of magnitude slower).

SQL already has a direct way to express one-to-many output. A set-returning function in a `LATERAL` join could return a record for every hit.\[[66](../bibliography/#ref-postgresql17_set_returning_functions)\]. The `LATERAL` clause lets that call refer to the current target row.<sup><a href="../bibliography/#ref-reference-note-1">151</a></sup>

```sql
-- Alternative API sketch, not an existing BioSeqDB function
SELECT t.id, h.score, h.start, h.cigar
FROM targets AS t
CROSS JOIN LATERAL align_hits(t.seq, $1) AS h
WHERE t.id > 20;
```

Different API example

This means that optimiser can work with queries, but user needs to do `CROSS JOIN LATERAL` for each alignment, which adds lots of boilerplate code. Additionally we loose ability to conditional evaluation in this case. Sometimes alignment may not require CIGAR which is more expensive operation as it requires running base alignment and the `SELECT` clause may only specify score. This prevents us from evaluating alignments differently based on what fields are needed.

To more easily indentify what parts of the alignment are needed (because otherwise we would need to track where the record type is used and how), I decided to separate returned record type into calls to functions. The function takes sequence as argument and returns some information about alignment. This concept is presented below.

```sql
-- Hypothetical functions with shared hidden state
SELECT id, last_score(seq)
FROM targets
WHERE save_search(seq, $1)
  AND last_score(seq) > 100;
```

Problematic "hidden state" example

Now the fields are easy to track as we can directly look for a call to a function in the AST expression tree. However if the extension implemented only Postgres function this example couldn’t work. We would need to pass some internal state between function calls - `save_search` would need to save some pointer to lazy execution object invisible to the user so it can be later called by `last_score`. This way we compute only what we need. Declaring these functions `VOLATILE` limits some planner transformations, but does not guarantee execution order between them<sup><a href="../bibliography/#ref-reference-note-2">152</a></sup> This means that it’s not possible to reliably pass this hidden lazy object between functions.

We can see that example above is much cleaner than `LATERAL` join and easier to understand, but to implement it we need to tell Postgres how we want to execute this query - in which order expressions are evaluated and create our own tuples inside. Historically before PostgreSQL 9.5 only way this could be done was via foreign-data wrapper interface, which treated remote table as opaque object. Later Postgres version introduced custom scans and table access methods. There’s overlap in all these interfaces, but the main advantage of TAMs and custom scans was that the logic wasn’t only tied to one specific table and there was deeper and more cleaner integration with the executor.

1.  PostgreSQL 9.1 added foreign tables and the foreign-data wrapper (FDW) API, a partial implementation of SQL/MED \[[57](../bibliography/#ref-postgresql91_release), [54](../bibliography/#ref-postgresql17_ddl_foreign_data)\]. PostgreSQL 9.3 added `postgres_fdw`, the in-tree wrapper that queries another PostgreSQL server \[[58](../bibliography/#ref-postgresql93_release), [52](../bibliography/#ref-postgresql17_postgres_fdw)\].

2.  PostgreSQL 9.5 added custom path and scan methods, so an extension can insert its own executor node into a plan \[[59](../bibliography/#ref-postgresql95_release), [50](../bibliography/#ref-postgresql17_custom_scan)\].

3.  PostgreSQL 12 added table access methods (TAM) through `CREATE ACCESS METHOD …TYPE TABLE`. Heap remains the default TAM \[[60](../bibliography/#ref-postgresql12_release), [106](../bibliography/#ref-postgresql17_tableam)\].

Table [pg scan extension points](../project_postgres_custom_scan/#tab-pg-scan-extension-points) summarises the split.

*PostgreSQL interfaces that can supply rows to a query.*

<a id="tab-pg-scan-extension-points"></a>

|  | FDW | Custom scan | TAM |
|:---|:---|:---|:---|
| First in core | 9.1 API, 9.3 `postgres_fdw` | 9.5 | 12 |
| Attaches to | a foreign table | a plan node | a local table |
| Stores bytes here | no | no | yes |
| Typical work | fetch remote rows | replace a scan or join | fetching tuples from storage manager, vacuuming |

The BioseqDB implements the following components:

- Custom scans - allow to implement syntax suggested here with selective evaluation of the pipeline, efficient batching integrated with optimizer, so I rely on optimisations from Postgres as well. Theoretically custom scan can replace scans to any table, but I don not offer support for tables other than BioseqDB tables (created using `CREATE TABLE USING bioseq` TAM)

- Table access method - stores data in columnar format, so I can load large chunks of metadata and sequence in single row fetch

## Integrating Postgres

<a id="sec-mmsearch-sql-rendezvous"></a>

### Designing SQL API

A full text search interface (`pg_search`) from ParadeDB was an important motivation for the SQL API design of BioseqDB. ParadeDB is an extension written in Rust that uses custom text storage engine under the hood. I deeply inspected its source code to understand how we can implement our own syntax in BioseqDB. ParadeDB resolves similar problem as FTS can also return multiple hits for a single query.[ParadeDB score expression · `score_from_relation 30`](https://github.com/paradedb/paradedb/blob/main/pg_search/src/postgres/customscan/basescan/projections/score.rs#L30)

To return custom results instead of relying on Postgres evaluating the function (We don’t want to let Postgres call actual `bioseq.score()` function, but rether provide it ourselves. ParadeDB has similar syntax and the same problem) ParadeDB replaces score expressions with typed placeholders. These placefolders are filled from the current search result before projection. [ParadeDB placeholder replacement · `inject_placeholders 493`](https://github.com/paradedb/paradedb/blob/main/pg_search/src/postgres/customscan/projections.rs#L493) Similar design was used in BioseqDB - described in Section [match placeholder injection](../project_postgres_custom_scan/#sec-match-placeholder-injection).

<a id="lst-paradedb-pg-search-examples"></a>

```sql
-- Some examples for pg_search
SELECT id, title, pdb.score(id) AS score
FROM documents
WHERE body ||| 'postgresql search'
ORDER BY score DESC, id;

SELECT id, pdb.snippet(body) AS excerpt
FROM documents
WHERE body &&& 'postgresql indexing';
```

ParadeDB pg_search examples

Listing [paradedb pg search examples](../project_postgres_custom_scan/#lst-paradedb-pg-search-examples) shows how similar ParadeDB’s problem is to my initial API designs.\[[70](../bibliography/#ref-pigsty_pg_search)\]\[[71](../bibliography/#ref-neon_pg_search)\]

### Plan lifecycle

<a id="sec-planner-support-and-ownership"></a> <a id="sec-custom-path-plan-lifecycle"></a>

To implement custom scan, we must understand different objects in Postgres:

1.  **Path:** Describes possible inputs, estimated work, output row count, and ordering. The planner compares several paths and keeps the cheapest one \[[55](../bibliography/#ref-postgresql_planner_optimizer)\]\[[56](../bibliography/#ref-conway_postgres_optimizer)\]. The Genetic Query Optimizer is used only when the number of joins exceeds `geqo_threshold`. A single-table `BioseqScan` is ordinary cost comparison, not GEQO. The path is later converted into a plan. I register my own paths so PostgreSQL knows how to access the table.

2.  **Plan:** In our case `CustomScan` contains child plans, expression trees, and descriptions of the search.

3.  **State:** When plan is executed, postgres creates its state via my code. `CustomScanState` holds the input cursor, alignment buffers for sequences and metadata, and slots for generated hit. Postgres will call scan repeatedly to get new rows, so I use the state to hold any information that needs to be preserved between calls.

All types of Postgres API described in table earlier [pg scan extension points](../project_postgres_custom_scan/#tab-pg-scan-extension-points) mainly rely on callbacks (in Postgres called *hooks*). Extension fills the structure with function pointers and let postgres know which call is handled by which function. In this case PostgreSQL calls `PlanCustomPath` when it converts a selected custom path into a plan and later calls `CreateCustomScanState` to create the executor state<sup><a href="../bibliography/#ref-reference-note-3">153</a></sup> Figure [bioseqscan hook lifecycle](../project_postgres_custom_scan/#fig-bioseqscan-hook-lifecycle) shows when PostgreSQL calls those hooks, when `BioseqScan` runs, and when the columnar table access method returns a tuple.

<a id="fig-bioseqscan-hook-lifecycle"></a>

![02 planner diagram](/static/figures/project/chapters-10-postgres-custom-scan-02-planner-tikz-01.svg)

*Lifecycle of `BioseqScan` and the columnar table access method. Bands mark exeuction states: initialization, parsing, planning, and execution. Boxes have various colours: Grey boxes are PostgreSQL, orange boxes are BioSeqDB hooks or the custom scan and green boxes are TAM callbacks. Arrows follow the order in which Postgres calls specific hooks. `column_planner_hook` calls `standard_planner`, then rewrites the finished plan, so we don’t explicitly control GQO, but can affect it using costs functions, registering paths and even after plan is choosen, we have complete constrol over it (we are free to rewrite its parts). The scan is calling TAM to fetch rows from storage and returns them to Postgres until it returns NULL (no more tuples to fetch)*

`ProcessUtility_hook` is used for utility statements such as `CREATE`, `DROP`, `COPY`, and `CREATE INDEX`. This is useful to forbid user from doing certain operations (Currently I only use it to log statements).

The TAM stores the user table in columnar format: A long sequence is not kept in that table row, only a locator is stored there. The sequence itself is stored in companion *fragment sub-tables*, and all of them use the bioseq TAM as well. More on storage format is discussed in Chapter [pg storage role](../project_postgres_storage/#sec-pg-storage-role). I called the process of fetching fragment from locator "hydration" and TAM is responsible for it.

BioseqDB have control over the plan using `column_planner_hook`. This hook is called when PostgreSQL needs to perform planning and SQL query is already parsed. First BioseqDB hook validates `mmsearch` options, then it calls `standard_planner`. `get_relation_info_hook` and `set_rel_pathlist_hook` hooks, where we set options like disabling parallel scans or index-only scans and register custom access paths for cust scan, run inside that call. Then GQO is run and finds the best path that needs to be converted into a plan, so the `PlanCustomPath` is run. When `standard_planner` returns we are still in the `column_planner_hook`, so I’m free to rewrite plan tree freely. The rewrite mutor performs the following changes:

- **Enable vectorized execution for simple aggregates** Using modernized Citus/Hydra columnar vectorized optimisations I allow simple aggregates on metadta columns to be vectorized (processed in batches) for example: `COUNT(*)` or `SUM(foo_bar)`. If current node is Postgres `Agg` it will be replaced by custom aggregator `VectorAggNode`. If this plan has `VectorAggNode` as parent, then I return batches of rows instead of single tuples. Rewrite adds falgs to mark that the plan needs to change its output mode. Additionally Postgres `Agg`

- **Wrap an index scans on columnar tables** PostgreSQL can still choose a metadata index on a `USING bioseq` table and use external `IndexScan`. If that happens this index can read some tuples that shouldn’t be red (columnar TAM allows removing rows. If that happens row is marked in bitmask, but index scan can go around it and still read the row, because it’s physically there). To prevent that the `IndexScan` is replaced by custom `BioseqIndexScan` that hounours bitmask.

As entities like `bioseq.score(id)` are still ordinary functions, I discourage Postgres by using prohibitive execution costs \[[49](../bibliography/#ref-postgresql17_support_functions)\]. The function implementation itself mostly just emits an error, because it shouldn’t be ever called. Instead `ExecCustomScan` will using special Postgres expression nodes i.e `Const*` node to tell Postgres that the scan itself calculated the function’s value and it shouldn’t evaluate it by itself.

### Different types of scans

When I talk about a custom scan I focus on `BioseqScan`, which is the primary way to read a `USING bioseq` table, but there are other types of custom scans used for different purposes in BioseqDB, some of them already mentioned earlier. All scans types are the following ones:

- **Dedicated join support:** `BioseqBatchJoin` is a custom scan with two children: one child is the target plan, the other is the query plan. Normally Postgres would use nested loop join that would read one target and one query and perform alignment for each pair, however we want to avoid this behaviour as target index can be loaded once (running `BioseqScan` for each pair would cause index for one table be loaded, then unloaded and loaded again for each query). This scan, dedicated for joins, prepares that target once (open the stored minimizer reader, or build index manually if not available) and runs the same logic as `BioseqScan` for each query in the batch. Dispatch buffer has separate API for joins, but they mostly effectively do same processing as for `BioseqScan` (collect queries in batches etc.)

- **Slices across operations:** `BioseqSliceProject` wraps a child plan when the query asks for a slice or other piece-tree operation, for example `SELECT seq[100:200]` or `mmsearch(t.seq[a:b], q.seq)`. It evaluates those expressions into expanded piece-tree Datums (Section [sequence piece tree](../project_sequence_abstractions/#sec-sequence-piece-tree)) and sends them to Postgres as read-only expanded objects in a virtual slot. This means that the piece-tree is stored in the tuple itself and can be passed around other scans, joins etc. without translating it into a string yet: later `BioseqScan` or `BioseqBatchJoin` can align that slice lazily. When user requests a slice as in `SELECT seq[100:200]` and it doesn’t go through other operation nodes, then it will be "hydrated" from piece-tree into actual byte sequence.

- **Support for indexing:** `BioseqIndexScan` is the wrapper around `IndexScan` described above, that allows to correctly read deleted columnar rows (support for bitmasks).

- **Sorting by sequence:** `BioseqBatchSortScan` is only for sorting by the sequence itself, for example `ORDER BY seq` or `ORDER BY seq DESC`. The planner will choose it only when there is exactly one `ORDER BY` key and that key has type `bioseq.seq`, and only when `bioseq.planner.sequence_sort_scan.enabled` is on (`ORDER BY seq, pk` and `ORDER BY seq::text` are still ordinary PostgreSQL sorts) This is experimental feature to allow faster sorting by sequence without the need to flatten entire sequence first. The custom scan buffers whole child rows, compares the sequence column as lazy views (not as printed text), and emits those same rows in the resulting permutation. <a id="lst-bioseqscan-types"></a>

## Dispatch buffer as glue between Postgres scan and alignment pipeline

<a id="sec-match-placeholder-injection"></a>

I still need to explain how exactly `CustomBioseqScan` works with worker-architecture shown in Figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle). The main abstraction here is the dispatch buffer which is reposible for transferring data between Postgres and workers.

Figure [execcustomscan dispatch](../project_postgres_custom_scan/#fig-execcustomscan-dispatch) shows how Panel (a) of Figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle) looks in the context of Postgres scan. `BioseqScan` asks for the next row from TAM. This row is placed into dispatch buffer (locator only) and dispatch buffer holds all the structures presented before [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle) - scanners. The dispatch buffer code then handles all view copies to the workers.

<a id="fig-execcustomscan-dispatch"></a>

![03 match fields diagram](/static/figures/project/chapters-10-postgres-custom-scan-03-match-fields-tikz-01.svg)

*Inside `ExecCustomScan`, the following things happen: `BioseqScan` holds dispatch buffer and fetches a locator handles it to the buffer logic and retrieves results from it. The dispatch buffer stores locators, then builds a session that holds scanners. A scanner miss reads the fragment sub-table through the TAM column cache and copies the fragment into a slot. Workers receive a view copy from the scanners. Black arrows show the normal alignmen execution path, however it might happen that we don’t perform alignment and just want to return the sequences itself. In that case the flow uses blue arrows (there’s no `mmsearch` in the query) and `BioseqScan` hydrates the sequence row through cache and just returns it without interacting with dispatch buffer. Dashed arrows repesent loops here.*

The collect loop is the gap between batch 1 and batch 2 in panel (d) of Figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle). `BioseqScan` may call the user-table TAM many times before it returns to fill in the dispatch buffer. The backend thread that owns the scanners performs fragment reads, while workers only see the view copy described in panel (b).

### Returning results back to Postgres

Now I not only need to build the tuple but somehow explain to Postgres that certain values correspond to requested values of function calls like `bioseq.dp_score(t.seq)`. PostgreSQL evaluates output expressions through an expression tree. Before returning its first alignment hit, the executor walks this tree and replaces the function calls with `Const` nodes. It writes the hit’s values and null flags into these nodes and rebuilds the output-expression evaluator so that it reads the changed fields. This makes Postgres know that we already computed these values. The idea was created by analyzing how ParadeDB provides its search results[ParadeDB current-result score · `exec_custom_scan 1784`](https://github.com/paradedb/paradedb/blob/main/pg_search/src/postgres/customscan/basescan/mod.rs#L1784)

#### How to return tuple

<a id="alg-scan-project-hit"></a>

1.  Restore the original columns.

2.  For every requested BioseqDB function like `score(seq)`, `target_start(seq)`, `cigar(seq)` and so on, write its field from this hit into the corresponding typed value node.

3.  Rebuild the evaluator, then evaluate the output expressions.

4.  Return the resulting SQL row

### Supporting joins

A join can lift a helper such as `bioseq.score(t.pk)` out of the scan that produced the hit and an outer join can also emit a null row when one input has no match.

The `Const` rewrite from the previous subsection is nice execution trick inside `BioseqScan`, but the join planner does not see those constants. It still sees an ordinary function call, so PostgreSQL may copy that call onto the other input of a `LEFT JOIN`, or evaluate it again in a `Join` or `Aggregate` node.

PostgreSQL’s `PlaceHolderVar` exactly to resolve this issue, however it’s suprisingly low volume of documentation about it. I only found old mailing list archive, where it’s described as an anti-COALESCE: the expression keeps its value on a matching row and is forced to `NULL` on a missing row \[[65](../bibliography/#ref-lane2010_placeholdervar)\]. The `PlaceHolderVar` records the relation at which the expression must be computed and the value then rises through the plan like a column. When an outer join nulls that relation, the placeholder becomes `NULL` (a `Const` cannot do this as it’s always the same value). [PostgreSQL expression placement · `PlaceHolderVar 2750`](https://github.com/postgres/postgres/blob/REL_17_10/src/include/nodes/pathnodes.h#L2750)

Without that wrapper the following could happen:

- The inner scan of a `LEFT JOIN` receives `bioseq.cigar(t1.pk)` even though the helper belongs to `t1` and that scan rewrites a function it does not own.

- A `Join` or `Aggregate` node calls the C++ stub. As I mentioned the function does not compute an alignment (the actual value is only scan-provided), so we get an error.

- Mishandling of `NULL` values:An unmatched outer-join row keeps a filled score instead of SQL `NULL` or `ORDER BY bioseq.score(t.pk)` after a join can sort on all-null keys.

ParadeDB uses the same mechanism for `pdb.score` and its planner function wraps the call in a `PlaceHolderVar` when the query has a join or an aggregate, so the custom scan keeps emitting the score and upper nodes do not re-run the function. [ParadeDB score placement · `placeholder_support 314`](https://github.com/paradedb/paradedb/blob/main/pg_search/src/postgres/customscan/projections.rs#L314)

In BioSeqDB I do this using `SupportRequestSimplify` hook as `bioseq_placeholder_support`, where we kip placeholders when there’s no joins or aggregates.

### Dispatch buffer internals

<a id="sec-dispatch-buffer-api"></a>

The main component I want to discuss here is the dispatch buffer: a container abstraction reponsible for retrieving, storing, distributing and collecting of the tuples with results. This requires managing memory and lifetime of Postgres tuples (both incoming from table and outgoing to the user).

#### Handling tuples

Postgres offsers `TupleTableSlot*` type, which represents reusable holder for a tuple. The Postgres executor keeps one scan slot on the expression node and asks the table access method to fill that same holder on every fetch.

This holder consist of a tuple descriptor, current column values, null flags, and a small operations (functions pointers) table that knows how to read or copy the physical layout currently stored inside it \[[111](../bibliography/#ref-postgresql17_tuptable)\]. The tuple descriptor is the struct holding information about the columns schema (how many attributes it has, names of the attributes and their types with additonal metadata).

PostgreSQL offsers the following types of slots [Builtin tuple table slot types · `TTSOpsMinimalTuple 38`](https://github.com/postgres/postgres/blob/REL_17_10/src/include/executor/tuptable.h#L38):

- **Buffer heap tuple** (`TTSOpsBufferHeapTuple`). The slot points at a row that still lives in a shared buffer page.

- **Heap tuple** (`TTSOpsHeapTuple`). The slot owns a `palloc` copy of a full on-disk row with system columns (added by Postgres) \[[107](../bibliography/#ref-postgresql17_storage_page_layout)\].

- **Minimal tuple** (`TTSOpsMinimalTuple`). The slot owns a compact copy of the user columns only without system columns added by Postgres. Sort, hash, and materialize nodes use this type because it is smaller than a heap tuple.

- **Virtual tuple** (`TTSOpsVirtual`). Instead of storing entire row it stores arbitrary data in array (usually pointers to the actual values, so we can reference existing tuple).

The TAM has one slot that is used to return tuples and it’s overwriten with each tuple fetch (`getnextslot` call). This means that dispatch buffer has to store the tuple somewhere. In a dispatch buffer method called `submit_scan` I allocate a fresh slot with `TTSOpsMinimalTuple`, copy the current user columns with `ExecCopySlotMinimalTuple`, and store it with `ExecStoreMinimalTuple`. The third argument is `shouldFree=true`, so the copied slot owns the bytes and will `pfree` them when the buffer later drops the slot.

Sequence tuples in the dispatch buffer needs to represent lazy-loadable seqences, so during their lifetime they can become of of the 3 types (I need to distinguish them, because in code they are all just minimal tuples, but semantically express different concepts):

- **Locator**: This object is what is usually stored in the user table and represents entire sequence stored in separate internal table. This is "unhydrated" sequence.

- **Expanded**: Postgres has a flag `VARATT_IS_EXTERNAL_EXPANDED` (I call this in a code "expanded recipe") to mark the value is not a flat byte array yet, but complex structure in memory. This happens when sequence holds piece-tree (slice graph) [sequence piece tree](../project_sequence_abstractions/#sec-sequence-piece-tree), so we have multiple slice operations to perform if we want to convert this value to flat sequence. This is also different form of "unhydrated" sequence.

- **Varlena**: Representing "hydrated" sequence fragment, so the slices or locator were converted to some flat byte array. <a id="lst-tuple-types"></a>

The alignment pipeline uses first two representations directly and as mentioned before when discussing sequence abstractions [sequence abstractions](../project_sequence_abstractions/#sec-sequence-abstractions) the sequence will be kept as a locator or piece-tree until the actual data is requested [execcustomscan dispatch](../project_postgres_custom_scan/#fig-execcustomscan-dispatch). Then I load requested bytes and return them directly. However in some cases we need to convert the tuple into 3rd form. If user requests slice projection directly it goes through separate branch of BioseqDB handling where that conversion is performed:

<a id="lst-sequence-slice-projection"></a>

```sql runnable id=project_lst-sequence-slice-projection timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE "table" (seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO "table" VALUES ('ACGTAACCGTTA');
-- bioseq:visible
SELECT seq[2:3] FROM "table";
-- bioseq:teardown
DROP TABLE "table";
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Direct projection of a sequence slice.

#### Buffer API

Table [dispatch buffer api](../project_postgres_custom_scan/#tab-dispatch-buffer-api) lists the most important methods of the dispatch buffer that are used during `ExecCustomScan` call. `Snapshot` here means the set of row versions this statement is allowed to see (this is mechanism for MVCC support in PostgreSQL) \[[110](../bibliography/#ref-postgresql17_mvcc)\] and is discussed later.

*Dispatch-buffer methods used by the Bioseq custom scans.*<a id="tab-dispatch-buffer-api"></a>

| Method group | When the custom scan calls it | What it does |
|:---|:---|:---|
| `configure_params`, `replace_executor_snapshot`, `bind_sequence_source_provider` | `BeginCustomScan` after the scan has a relation and a snapshot. PostgreSQL parallel workers (only for queries that do not use alignment [simple table read with slice](../project_sequence_abstractions/#code-simple-table-read-with-slice)) restore a copy of that snapshot and call `replace_executor_snapshot`. The worker must use its own registered `Snapshot` pointer for later fragment reads. | Stores aligner settings, the user table identity, the borrowed snapshot, and the statement-owned sequence source that scanners will read. |
| `configure_mmsearch`, `configure_mmsearch_per_row`, `configure_mmsearch_per_row_evaluated` | `BeginCustomScan` once the planner-private list has named the search, or on `ReScan` when a join parameter changes the query. | Records which columns are the target and (if needed) the per-row query, copies a constant query, and turns the buffer into the writing stage. |
| `submit_scan`, `submit_scan_evaluated` | Writing stage of `ExecCustomScan`, after a TAM row passed the filter conditions. The "evaluated" method is called when parameter in `mmsearch` is an expression that needs to be evaluated. | Copies that row into a buffer-owned MinimalTuple [tuple types](../project_postgres_custom_scan/#lst-tuple-types) slot and records its row number, primary key, and sequence locator (or evaluated Datums). Does not run alignment yet. |
| `ready_to_dispatch`, `has_finished_writing`, `mmsearch_active` | The `ExecCustomScan` loop, after each submit and after each drained hit. | Returns flags indicating whether the physical buffer has reached 8192 rows, whether the current batch has already been aligned, and whether this scan is an `mmsearch` scan at all. |
| `run_batch_and_build_cursors` | When the buffer is full, or when the TAM source is exhausted. | Streams the buffered locators into the alignment pipeline, then builds an output cursor for retrieving hits. |
| `read_next_output` | Reading stage of `ExecCustomScan`, once writing has finished. | Returns the next hit together with a pointer to the copied scan slot. The scan then fills the match-field `Const` nodes from Section [match placeholder injection](../project_postgres_custom_scan/#sec-match-placeholder-injection) and projects. |
| `clear_slots` | After a full 8192-row batch has been drained while the TAM source still has rows, and on `ReScan` or `EndCustomScan`. | Frees every copied MinimalTuple slot and the payload arena so the next physical batch can start empty. |
| Support for joins: `configure_mmsearch_join`, `begin_join_target`, `prepare_join_target_queries`, `set_join_query_batch`, `run_join_query_batch_and_build_cursors`, `finish_join_target`, `reset_join_runtime_for_rescan`, `abandon_join_runtime_after_pg_error_noexcept` | Methods called only by `BioseqBatchJoin` custom scan for custom join (discussed in next section [mmsearch join ownership](../project_postgres_custom_scan/#sec-mmsearch-join-ownership)). PostgreSQL `ReScan` restarts a plan node from its first row, often with new parameters (for example the inner side of a nested loop for the next outer row, or a cursor rewind). `ReScanCustomScan` therefore calls the reset helper so leftover join targets and hits are dropped. | Holds one join target in the buffer, streams inner queries in batches of at most 8192, runs alignment against that stored target, then drops the target. The reset and abandon helpers clear join state without asking the caller to redo `submit_scan`. |

The code in `ExecCustomScan` gets one TAM row, runs filtering conditions (these can reject the row based on metadata) and on success calls `submit_scan` to let the buffer copy the tuple. When the copies in the buffer reach the set capacity `dispatch_batch_capacity` $8192 = 2^{13}$, the `ready_to_dispatch` flag changes to true so the scan calls `run_batch_and_build_cursors`. This method is also invoked when there are no more tuples in the TAM source.

`run_batch_and_build_cursors` is the method called when the batch starts and scanners and workers from Figure [execcustomscan dispatch](../project_postgres_custom_scan/#fig-execcustomscan-dispatch) run. It groups the locators already stored in the MinimalTuple copies, lets scanners fetch fragment bytes through the TAM under the Postgres table snapshot object and then distributes the views to workers.

Then the scan needs to read results. Reading stage calls `read_next_output` on every later `ExecCustomScan` to retrieve the next result (this means that in this semantic first call to `ExecCustomScan` takes very long as it needs to align first batch, while later calls return tuples one-after-another from the output cursor until they are exhausted and process repeats). This is the place where we need to set values of nodes and project the tuple to let Postgres construct final results [match placeholder injection](../project_postgres_custom_scan/#sec-match-placeholder-injection). After Postgres consumes that projected tuple, the next call drops the previous result slot and asks for the next hit. When the cursor is empty and the TAM source is not yet exhausted, the node calls `clear_slots` and returns to the writing stage (this corresponds to finishing batch and starting collecting rows again)

##### Execution statistics.

Each completed call to `run_batch_and_build_cursors` writes a `pipeline_execution_stats` aggregate onto the buffer, where I collect debugging statistics. `EXPLAIN ANALYZE` reads then through `execution_stats()`. Table [dispatch buffer stats](../project_postgres_custom_scan/#tab-dispatch-buffer-stats) presents some of them as the list is long and not every one is relevant to the common user.

*Grouped execution statistics collected by the dispatch buffer.*<a id="tab-dispatch-buffer-stats"></a>

| Group | Fields | Description |
|:---|:---|:---|
| Backend algorithm | requested algorithm versus actual algorithm | Whether the scan asked for GPU or CPU, and whether the runtime had to use fallback (hardware is unavailable or the algorithm is not supported). |
| Alignment jobs | counts all started, failed and finished jobs | How many target and query pairs the pipeline tried, how many finished. |
| Target index | target rows, persisted index rows, missing persisted rows, recomputed rows | Whether minimizer indexes were read from storage or rebuilt for this batch. |
| GPU dispatch | seed, chain, backtrack and other stages | Information about which stages actually run on a GPU |
| Stage timings | per-stage time counters (query sketch, index build, seed, chain, extend, …) | Allows us to extract times for specific pipeline stages. |
| Integrity flags | mixed environment / mixed index / saturated counters | Additional flags indicating counter overflow or inconsistent information when statistics are aggregated (for example, different execution environments or persisted index identities). These flags are used for debugging. |

### Memory reservations (permits)

<a id="sec-dispatch-memory-admission"></a>

Before dispatching work, BioseqDB requests a memory reservation for its input and temporary buffers. This is an important part of the BioseqDB design as it allows the dispatch buffer to estimate how much memory will be needed and check this against available memory, the process footprint and existing reservations.

BioseqDB handles these reservations through memory permits, which keep a byte count and a reference to the process-local admission controller. The actual buffers are allocated separately. Pipeline arenas can reuse their allocated space, and growing their backing storage requires another admission check. I use this mechanism to reduce memory pressure and the risk of going into SWAP.

### MVCC support

<a id="sec-scan-row-stability"></a>

PostgreSQL holds its current input row in temporary storage called a tuple slot, but another transaction may also update that row. `Snapshot` in Postgres describes which row versions a statement is allowed to see. PostgreSQL uses this snapshot for visibility checks when reading rows \[[110](../bibliography/#ref-postgresql17_mvcc)\]. `configure_params` borrows this snapshot object so later fragment reads and bulk fetches observe the same MVCC view as the original TAM fetch. Let’s see if this is compatible with the BioseqDB model:

- **Copying values:** Dispatch buffer always copies the original tuple into an owned MinimalTuple slot, so Postgres won’t overwirte this data.

- **Saving Postgres snapshot object:** PostgreSQL uses a snapshot to decide which row versions a command can see. Sequence reads use the scan’s Snapshot to check that the stored reference points to a visible source version (visibility check). The idea is that stored source fragments are immutable and an update publishes a new version, while an older snapshot can still read its earlier version.<sup><a href="../bibliography/#ref-reference-note-4">154</a></sup>

- **Saving row numbers:** While the primary key is important for user, the row internally holds a row number that is used to indentify results.

- **Opening relation:** In scan I use `ACCESS SHARE` relation lock to prevent conflicting operations such as dropping or rewriting a relation while it is in use (concurrent updates and deletes are still possible, but avoided due to snapshot mechanism)<sup><a href="../bibliography/#ref-reference-note-5">155</a></sup>

I use existing Postgres mechanisms to support MVCC so it didn’t require any additional custom code.

### Filter placement

<a id="sec-filter-placement"></a>

As this mentioned here, before saving tuple into the buffer, scan checks if the filter predicate on non-alignment dependent values evaluates to true. This is done to avoid aligning rows that will be discarded anyway. In BioseqDB there are two types of filter conditions:

- **A row condition** such as `t.id > 20` depends only on stored columns, so is evaluated before alignment. Sometimes columns that are comparable (like Postgres primitive types) can allow min/max chunk prunning to be used. This is special strategy to skip entire columnar chunks with data [storage minmax pruning](../project_postgres_storage/#lst-storage-minmax-pruning)

- **A hit condition** This is special case of alignment prefilter. I have a separate stage in the pipeline that can be invoked to run cheaper estimation of edit distance for each sequence. If I detect special predicate of form `bioseq.edit(t.seq) < 10` I run pipeline with prefilter enabled. This is different to the case above, because it still requires batching tuples and running pipeline, but it allows for faster termination. If the distance estimation yields false, then the rest of stages are skipped (there’s no seeding, chaining, backtracking etc.) so we avoid even the cost of fetching the minimizers from the saved index. This results in great speedups, but at this time is yet an experimental feature using code from libraries that weren’t developed by me. <a id="lst-prefilter-condition"></a>

```sql runnable id=project_chapters-10_postgres_custom_scan-04_execution_1 timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE targets (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;
INSERT INTO targets VALUES (27, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA');
-- bioseq:visible
SELECT t.id, bioseq.dp_score(t.seq) AS score
FROM targets AS t
WHERE t.id > 20
  AND mmsearch(t.seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna5', 64))
  AND bioseq.edit(t.seq) < 10
LIMIT 5;
-- bioseq:teardown
DROP TABLE targets;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Filter input rows and then filter alignment hits.

> [!WARNING]
> **Potential features**
>
> Currently I do not optimise limits so `LIMIT 5` will still evaluate all tuples in batch even after more than 5 results are found. I also do not provide any optimisations for score ordering, so `ORDER BY score DESC LIMIT 5` doesn’t result in any specialised algorithm. However there are some very speicific cases that would be interesting to optimise. We can use faster chaining if we know that there’s only one good hit per sequence. This isn’t expressable in SQL with this API, but you can imagine providing custom limit per one row: `WHERE bioseq.count(t.seq) = 1`.

## Joins support

<a id="sec-mmsearch-join-ownership"></a>

Asssime we have a search between two tables `reads` and `targets`:

```sql runnable id=project_chapters-10_postgres_custom_scan-05_joins_and_plans_0 timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE targets (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;
INSERT INTO targets VALUES (27, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA');
CREATE TABLE reads (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;
INSERT INTO reads VALUES (1, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA');
-- bioseq:visible
SELECT t.id AS target_id, q.id AS query_id,
       bioseq.dp_score(t.seq) AS score,
       bioseq.cigar(t.seq) AS cigar
FROM targets AS t
JOIN reads AS q ON mmsearch(t.seq, q.seq)
WHERE t.id > 20 AND q.id <= 100;
-- bioseq:teardown
DROP TABLE reads;
DROP TABLE targets;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Search sequence pairs from two tables.

There are several possible plans for searches:

- **Search before a join:** A base `BioseqScan` can search a target against a constant query. This is the most straightforward operation, but isn’t possible for the described join.

- **Search with an outer parameter:** Postgres can create a nested-loop plan that used `BioseqScan` inside. This was discussed in scan list [bioseqscan types](../project_postgres_custom_scan/#lst-bioseqscan-types) and causes the target to be loaded multiple times, which reduces performance heavily.

- **Search at the join:** `BioseqBatchJoin` replaces the nested-loop plan and allows to keep the target index loaded only once, while iterating over queries. This is the most efficient plan for the described join.

Interesting thing happens if we want to make two searches for the same row. Consider the example below:

<a id="lst-ambiguous-target-binding"></a>

```sql
-- Unsupported: this will cause an error
SELECT t.id, bioseq.dp_score(t.seq)
FROM targets AS t
WHERE mmsearch(t.seq, $1::bioseq.seq('dna5', 64))
  AND mmsearch(t.seq, $2::bioseq.seq('dna5', 64));
```

Two searches with one ambiguous target binding.

In this case due to my choise of SQL API, we don’t know which score `bioseq.dp_score(t.seq)` refers to as we have two potential results with different scores. To resolve this issue we can do a self-join on the same table:

```sql runnable id=project_chapters-10_postgres_custom_scan-05_joins_and_plans_2 timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE targets (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;
INSERT INTO targets VALUES (27, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA');
-- bioseq:visible
-- Supported: this is valid use-case within BioseqDB
SELECT a.id,
       bioseq.dp_score(a.seq) AS score_a,
       bioseq.dp_score(b.seq) AS score_b
FROM targets AS a
JOIN targets AS b ON a.id = b.id
WHERE mmsearch(a.seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna5', 64))
  AND mmsearch(b.seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna5', 64));
-- bioseq:teardown
DROP TABLE targets;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Give each independent search its own target binding.

Now, let $a_i$ be hit $i$ of the search bound to alias `a`, and let $b_j$ be hit $j$ of the search bound to alias `b`. Figure [scan alias pairs](../project_postgres_custom_scan/#fig-scan-alias-pairs) shows their combinations for one target row. We can see that natural extension of the used syntax would be to generate cross-product of hits. It’s worth noting that currently we are not able to express search-specific limits [filter placement](../project_postgres_custom_scan/#sec-filter-placement) meaning that there’s no optimisation for example if we wanted to have only best-hit per each sequence (only tuple with best score on both searches). In that case all tuples needs to be evaluated and user can construct a query manually to select the highest-scoring hits:

```sql runnable id=project_chapters-10_postgres_custom_scan-05_joins_and_plans_3 timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE targets (id integer, seq bioseq.seq('dna5', 64)) USING bioseq;
INSERT INTO targets VALUES (27, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA');
-- bioseq:visible
SELECT a.id,
       MAX(bioseq.dp_score(a.seq)) AS score_a,
       MAX(bioseq.dp_score(b.seq)) AS score_b
FROM targets AS a
JOIN targets AS b ON a.id = b.id
WHERE mmsearch(a.seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna5', 64))
  AND mmsearch(b.seq, 'TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA'::bioseq.seq('dna5', 64))
GROUP BY a.id;
-- bioseq:teardown
DROP TABLE targets;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Keep one highest score from each of the two searches, per stored row.

<a id="fig-scan-alias-pairs"></a>

![05 joins and plans diagram](/static/figures/project/chapters-10-postgres-custom-scan-05-joins-and-plans-tikz-01.svg)

*Two hits from the first search and three from the second produce six combinations. Additional hit conditions may select subset of this cross-product matrix, but this is responsibility of the user as I don’t provide any specialised optimisation for example to extract only one best-hit for each search.*

The intereesting part is that self-joins in relational databases are commonly used for hierarchical structures (trees), detect duplicates, extract consecutive elements (and other comparison operations). In this case it’s used to create new alias for the column without any additional operations.

There is however one interesting exception that is a bug in the BioseqDB code (initially I supported conflicting calls under one alias and there is leftover branch of the code that still supports it). Users should be aware that there still can be some undiscovered edge cases that are not correctly detected. I initially planned to support same-alias searches if no feature is used (we don’t specify CIGAR, score etc.), but this results in usability issue that adding these properties suddently requires user to rewrite the query completely. For this reason I later decided to disable any conflicting alias searches even if we don’t specify any properties in the `SELECT` list.

<a id="lst-constant-extra-mmsearch"></a>

```sql
-- This shouldn't be possible, but currently works (bug)
SELECT t.id, bioseq.dp_score(t.seq) AS score
FROM queries AS q, targets AS t
-- index_read_requirement is a special debug parameter to disable BioseqDB from loading index
-- for some reason enabling it allows you to trigger specific BioseqJoin path that doesn't correctly detect the conficting calls
-- The queries table needs to be normal table, not defined using "USING bioseq"
WHERE mmsearch(t.seq, q.seq, aligner_options(index_read_requirement => 1))
  AND mmsearch(t.seq, 'ACGTACGTACGTACGT'::bioseq.seq('dna5', 64));
```

Low priority bug that still exists in the code

## Caching results

<a id="sec-bioseq-result-cache"></a>

As mentioned before I provide layers of caching:

- Chunk (strip) cache that stores rows of columnar stored (compressed blobs that can potentially store multiple sequences) [execcustomscan dispatch](../project_postgres_custom_scan/#fig-execcustomscan-dispatch). Described more in detail in next chapters [pg storage role](../project_postgres_storage/#sec-pg-storage-role)

- Lazy interface cache implemented by scanner slots [execcustomscan dispatch](../project_postgres_custom_scan/#fig-execcustomscan-dispatch)

- I also provide long-term experimental cache layer for alignments results described here.

An alignment query may run for hours while returning a small result. If two sessions submit the same request, running the alignment twice wastes work. I created separate small `bioseq_cache` extension that is provided along with BioseqDB.

> [!WARNING]
> **Experimental support**
>
> This chapter doesn’t go into details of chache implementation, because it’s still experimental. I didn’t perform enough testing to be confident enough about all edge cases correctness as I needed to be very careful how to lock queries and split work (it needs comparing expression trees to detect query identity or overlap, serializing them to store them, parallel-safe coordination of locking). The subextension isn’t loaded by default and user need to execute `CREATE` `EXTENSION` `bioseq_cache` manually and restard Postgres server. There are some problems with this approach as the MVCC Postgres visiblity can change during execution (there could be insert when alignment is running). This problem wasn’t addressed so the user would see old version of the data. That’s why I decided to disable caching now. Moreover the "excact results" setting is fully supported now and can be enabled if user loads extension, while "reusable fragments" have most of the code ready except that it was only tested on scalar types (INTs, FLOATs etc.) and sequence support will require some alterations (planner will fallback to "exact results" anyway)

The cache subextension coordinates requests and stores completed results. The extension provides two forms of reuse:

- **Exact results:** If user executes two identical queries at the same time, we provide special custom scan in plan that waits for currently running job and loads results from it. It needs to happen during planning stage - planner needs to detect that alignment pipeline is running and then during execution stage block execution until results are available.

- **Reusable fragments:** This is more complicated part of the code that doesn’t completely block execution of the query. If similar query is currently running, planner will try to alter current query to still execute it, but try to cover region that is not covered by running jobs.

*producer* is the query runner that currently executes alignment job. In that timeframe another Postgres-spawned process with other user query waits for results. Submission of partial results would be possible, although very complicated to implement correctly. *sealing* here means that we store last tuple, collect some metadata like tuple counts and create new UUID for the result. Figure [cache lifecycle](../project_postgres_custom_scan/#fig-cache-lifecycle) shows what happens during the execution of two queries. The cache has expiration TTL time after which the results cannot be reused anymore.

<a id="fig-cache-lifecycle"></a>

![06 result cache diagram](/static/figures/project/chapters-10-postgres-custom-scan-06-result-cache-tikz-01.svg)

*Consumer just waits for all results from producer and then reads them.*

There’s one lock that is taken by any consumer during execution just to access shared state. The consumer sleeps and preiodically acquires and releases the lock to check if it can continue execution (polling). If the producer fails or is cancelled I wake all consumers that registered for this job and then each of them will try manually run the query (so the consumer won’t fail due to producer error or cancellation).

The interesting problem is that matching SQL text alone is insufficient. I needed to include analyzed query, parameter values, result types, relevant execution settings, and the database context objects to make sure we reliably detect identical queries. Additionally I keep track of query dependencies (all relations) and if the relation changes (row insert) during processing, then cache will be invalidated. This means that insert during consumer’s wait time will cause the consumer to execute query by themselves (although this require more testing to confirm correctness)

### Reusing fragments of queries

<a id="sec-seq-results-cache"></a>

Suppose that table is kept unchanged, but user slightly changes some metadata condition (for example instead of aligning all sequences with dates between some range, we extend the range to include few more dates). For indentiy comparison I store the analyzed query plan as well so we can say what filter conditions were used for that cache entry.

The general idea of describing what is cached, reading useful part and computing remainer was very loosely inspired by semantic caching paper<sup><a href="../bibliography/#ref-reference-note-6">156</a></sup>

For example, select input rows with $40\leq id<100$. Completed sources cover $0\leq id<60$ and $50\leq id<80$. We read $[40,60)$ from the first source, $[60,80)$ from the second, and compute $[80,100)$. Let’s define it formally via predicates:

- $T(t)$: $t$ is an input tuple

- $A(t,h)$: $h$ is tuple with alignment of tuple $t$

- $p(t)$: the new SQL predicate $P$ evaluates to `TRUE` on $t$

- $c_i(t)$: cached source $i$ has tuple with alginment for which the original predicate $P_i$ evaluates to `TRUE` on $t$.

Order the selected cached sources from $1$ to $m$, source $i$ supplies part $B_i$, while the reminder we have to compute is $D$: <a id="eq-cache-branch"></a> <a id="eq-cache-difference"></a> $$\begin{aligned}
 B_i &= \{(t,h)\mid T(t)\land A(t,h)\land p(t)\land c_i(t)
                     \land \forall j<i:\neg c_j(t)\},
                     \\
 D &= \{t\mid T(t)\land p(t)\land \forall i\in\{1,\ldots,m\}:\neg c_i(t)\}.
                     
\end{aligned}$$ Each requested hit is returned from the first valid source covering its input, or computed from $D$: <a id="eq-cache-result"></a> $$\{(t,h)\mid T(t)\land A(t,h)\land p(t)\}
   = B_1\uplus\cdots\uplus B_m
     \uplus\{(t,h)\mid t\in D\land A(t,h)\}.$$ Here $\uplus$ joins disjoint sets of occurrences. This simple formal description can be implemented as large `UNION ALL` operation of the entries in cache. Figure [cache ranges](../project_postgres_custom_scan/#fig-cache-ranges) shows the same example, including the overlap between the two sources.

<a id="fig-cache-ranges"></a>

![06 result cache diagram](/static/figures/project/chapters-10-postgres-custom-scan-06-result-cache-tikz-02.svg)

*Example of resuable query with one predicate on the primary ID column.The new request uses source 1 for $[40,60)$, source 2 for $[60,80)$, and computes $[80,100)$. The gray overlap in source 2 is omitted because source 1 already contains those occurrences. Cached rows outside the new request are also omitted.*

With both sources valid, the missing inputs have this equivalent SQL description:

<a id="lst-cache-missing-inputs"></a>

```sql runnable id=project_lst-cache-missing-inputs timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE sequences (id integer, seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO sequences SELECT i, 'ACGTA'::bioseq.seq('dna4', 64) FROM generate_series(0, 100) AS i;
-- bioseq:visible
SELECT id, seq
FROM sequences
WHERE id >= 40 AND id < 100 -- New query we want to compute
  AND (id >=  0 AND id < 60) IS NOT TRUE -- Minus source 1 results 
  AND (id >= 50 AND id < 80) IS NOT TRUE; -- Minus source 2 results
-- bioseq:teardown
DROP TABLE sequences;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Input rows on which the expensive operation is still needed.

There are few important details about the implementation:

- **I need to correctly handle NULLs** For a nullable predicate, `NOT P` also becomes unknown when $P$ is unknown. `P IS NOT TRUE` includes both false and unknown, so it keeps every input excluded from the earlier cache.<sup><a href="../bibliography/#ref-reference-note-7">157</a></sup>

- **Handle source failures** If source 1 fails, $c_1$ is falsethen $D$ should become $[40,50)\cup[80,100)$ so on error we don’t abort execution and just adjust computed region (currently this is realised by adding barrier for each query and computing $D$ at the end)

- **Column differences** There could be a case in which source supplies only subset of columns. I don’t optimise this scenario as computing new columns still requires running entire pipeline again, so we assume here that the column list is always the same.
