---
title: "Physical sequence storage in PostgreSQL"
slug: project_postgres_storage
description: "Physical sequence storage in PostgreSQL"
order: 100
keywords: [BioseqDB, project]
draft: false
---

\[v14.0.0\] \[d1f7657ee82265b2b9f1c06fda929bb5b942411b\]

## Store sequences together with their metadata

<a id="sec-pg-storage-role"></a>

I already mentioned before for example when discussing sequence abstractions [sequence abstractions](../project_sequence_abstractions/#sec-sequence-abstractions) that I usually operate on locators, that just point to location where actual sequence is stored.

PostgreSQL already has mechanism for that: it stores large values out of line (outside actual row) through TOAST \[[68](../bibliography/#ref-postgresql17_toast)\]. This mechanism however, has it’s limitations - it’s storage is limited to 1GB per row, which is an effect of maximum width of integer storing row’s OID (object indentifier is internal numbering scheme in Postgres). If we assume that DNA4 sequence is bit-packed and it’s entire human genome (lengths of around 3.1 - 3.2 Gbps) this gives us conservatively $3 * 10^{9} * \frac{2}{8}B = 750MB$ of storage per row. If we want to store both pairs of chromosomes we can pesimisitcally require $1.6GB$ of storage per row. In most cases researchers split the sequence into smaller chunks, but this is manual process requiring additional tools.

Moreover, most of the workflows are OLAP-type.: user is expected to do bulk inserts, do not perform frequent updates and deletes when running alignment. Usual TOAST wouldn’t take advantage of that specific workflow assumption as we cannot read TOAST in bulk.

That’s why I decided to implement custom TOAST-like mechanism in BioseqDB and use columnar storage appropiate for OLAP workflows:

- **User table** holds all the data in columnar format, where the sequence column itself is replaced with small locator object that points to sequence fragment.

- **Sequence fragments** are stored in multiple sharded tables that are normally invisible to the user. All of these tables also use columnar storage, so short sequences are treated the same way as metadata in columnar context - they will be merged into one batch. I call these batches of rows "strips" in the code.

- **Persistent indexes** are separate entities stored as tables that are normally not visible to the user as well. They are responsible for providing pre-computed objects for sequences. There’s small abstraction layer in BioseqDB which allows to extend it by adding different types of indexes that provide different information. <a id="lst-types-of-storage-entities-in-bioseqdb"></a>

> [!WARNING]
> **Adapted from Citus and Hydra Columnar**
>
> BioSeqDB’s columnar layer adapts code from both Citus Columnar and Hydra Columnar \[[72](../bibliography/#ref-citus_columnar_storage), [97](../bibliography/#ref-hydra_columnar_source)\]. Hydra extended the Citus code with features including very primitive vectorized SQL execution and a cache of stripes. In BioSeqDB I modernized the adapted C code by rewriting it completely into C++26 modules with automatic code generation and added support for newer PostgreSQL 19.0 (which in case of Hydra original codebase was not supported). Only small of the original code was left untouched. This code was used as a low-level storagebase, where concepts like sequence locators, fragment readers, and sequence indexes are purely my innovations.

The linked Hydra columnar documentation was unavailable as the theam building it moved to create a different database system. Although I was basing my implementation in the Hydra codebase, understanding the chunk operations required multiple manual investigations to understand it completely \[[97](../bibliography/#ref-hydra_columnar_source)\]. Current BioseqDB storage design is an effect of this process.

It’s worth noting that I mention both Citus \[[95](../bibliography/#ref-citus_distributed_2021)\] and Hydra sources here as Hydra was fork of Citus that diverged at some point to later be abandoned. Citus columnar is still actively maintained but it lacks features like row deletion, vectorization and it’s very simple columnar interface. Meanwhile Hydra offered these features, but due to being abandoned didn’t correctly build for newer Postgres version and lacked bugfixes done by Citus team since the forking moment. Modernizing the Hydra codebase thus involved not only manually inspecting the code, but also tracking commits between Hydra and Citus. This work thus was complicated and very time-consuming involving also detection of new bugs in Hydra code that were undected after retirement of the project.

## Columnar storage

<a id="sec-pg-storage-columnar"></a>

ClickBench is a benchmark for analytical DBMS and it also reported results for both Hydra and Citus. Figure [storage clickbench motivation](../project_postgres_storage/#fig-storage-clickbench-motivation) uses historical benchmark submissions on AWS machines \[[99](../bibliography/#ref-clickbench_2023_columnar)\]. The benchmark consisted of around 43 queries over a dataset of around 100 million rows (3 runs with minum of second two taken as into the sum).

<a id="fig-storage-clickbench-motivation"></a>

![02 columns and pages diagram](/static/figures/project/chapters-11-postgres-storage-02-columns-and-pages-tikz-01.svg)

*Historical ClickBench, calculated from the repository snapshot of 20 September 2023. Each submission uses `c6a.4xlarge` machine with 16 vCPUs, 32 GiB RAM, and 500 GB of GP2 storage. PostgreSQL and Citus results are dated 1 July 2022, and Hydra results 19 September 2023. The times use the sum of shorter of runs two and three for every query. Note that these are "best-fit" results so the configuration was different. It’s good however for quick comparison of performance.*

Both columnar submissions store about 19 GB, compared with PostgreSQL’s 77.8 GB. This was due to support of compression on stored columns. Hydra \[[97](../bibliography/#ref-hydra_columnar_source)\] had much better performance due to introduction of vectorized SQL execution for aggregates and column cache that wasn’t present in Citus \[[95](../bibliography/#ref-citus_distributed_2021)\]. These results do not translate directly to BioseqDB as the benchmarks tests some arbitrarily-built queries, however it motivated me to put big effort into modernizing Hydra codebase and incorporating it into BioseqDB as mentioned in previous section [pg storage role](../project_postgres_storage/#sec-pg-storage-role).

### Columnar layout

The columnar layout has three levels:

- A **stripe** is a batch of chunks coresponding to set of rows. Normally one stripe holds maximum is 150,000 rows.

- A **chunk group** is a group of chunks for the same rows inside a stripe. By default one chunk group holds maximum of 10,000 rows.

- A **column chunk** is the most basic entity that just holds values for one group as a one column. It also stores aggregated values (count, min, max) to enable faster aggregate operations (vectorization) and the bitmask (marking which values are NULLs).

Stripe and chunk-group sizes are options per table, where larger ones allows more efficient compression and smaller ones allows more fine-grained read access \[[96](../bibliography/#ref-citus_columnar_compression_2021)\].

Figure [storage heap to stripe](../project_postgres_storage/#fig-storage-heap-to-stripe) shows this model for the six rows. By default Postgres row-based storage (heap tuple) stores `id`, `sample_id`, and the sequence locator together. The stripe is columnar representation of these rows. Each column occupies its own sequence of chunks. A chunk group is only an abstract concept for chunks that are related to the same rows. The last group of a stripe can be shorter if total row count isn’t a multiple of chunk group size.

<a id="fig-storage-heap-to-stripe"></a>

![02 columns and pages diagram](/static/figures/project/chapters-11-postgres-storage-02-columns-and-pages-tikz-02.svg)

*The same six rows as heap tuples and as one stripe. Matching colours mark the same column. The blue callout is one column chunk: presence bits, present values, min/max, and slot count. The orange callout is the chunk group: a deletion mask and `deleted_rows`*

When I want to store row, each time the writer append this row to the current chunk group (in memory). After sufficient number of row is reached (max chunk group size) the code serializes each column chunk to a stripe buffer that still is in memory If the number of rows per stripe is reached (max stripe size) then stripe is "flushed", which means writing the stripe. Commonly the maximums of stripe row counts will be hit, but there’s additional check if the current chunk group for specific column buffer exceeds $1GB$ limit, then it forces flush of the entire stripe. This means that if we store large BLOB along very small column, the BLOB will force the stripe to end i.e stripes always need to account for the same number of rows in each column (this also implies that the decision to store sequences via locators increases efficiency of that storage model as more tuples with non-sequence fields can be packed into single stripe). Additionally there’s hard limit on maximum size of $256MiB$ for a single value in row.

> [!WARNING]
> **Storing large BLOBs**
>
> Users should avoid storing large BLOBs along small columns - either store BLOBs of similar sizes together or only store smaller payloads in the same table. This is potential optimisation to be implemented in the future, although it’s very low priority.

The figure [storage write path](../project_postgres_storage/#fig-storage-write-path) shows 3 main stages: we save tuples first to accumulate enough of them, then serialize them to a buffer and when stripe’s buffer eaches $1GB$ or the strip max row count, only then it’s flushed. Algorithm [storage flush stripe](../project_postgres_storage/#alg-storage-flush-stripe) presents pseudocode for that process as well.

<a id="fig-storage-write-path"></a>

![02 columns and pages diagram](/static/figures/project/chapters-11-postgres-storage-02-columns-and-pages-tikz-03.svg)

*How we pack chunks groups into stripe buffers, then to flush them if necessary.*

<a id="alg-storage-flush-stripe"></a>

Appending rows

```pseudocode
Require: columns $C$, group limit $G$ (default $10{,}000$), stripe limit $S$ (default $150{,}000$), comparators $\mathrm{Compare}_c$ (possibly $\mathrm{NULL}$)
$G_{\max}\gets\lceil S/G\rceil$
$s\gets\mathrm{NULL}$ // open stripe, or none

Procedure `AllocateNewStripe`()
  $s.n\gets 0$ // rows already in this stripe
  $s.P\gets 0^{|C|\times G}$ // presence bits of the current group
  $s.B\gets\varepsilon^{|C|}$ // serialized present values of the current group
  $s.K\gets\{\min:\bot,\max:\bot,\mathit{cnt}:0\}^{|C|\times G_{\max}}$ // skip min, max, and slot count of every column chunk
  $s.\mathrm{chunk}\gets\{P:\varepsilon,V:\varepsilon\}^{|C|\times G_{\max}}$ // packed streams of every serialized group
  $s.N\gets 0^{G_{\max}}$ // row count of each serialized group
  Return $s$
End Procedure

Procedure `WriteRow`($v$)
  If $\mathrm{IsOpen}(s)\land\exists c\in C:\mathrm{Size}(s.B_c\Vert v_c)>1\,\mathrm{GB}$
    `FlushStripe`($s$)
  End If
  If $\neg\mathrm{IsOpen}(s)$
    $s\gets\verb|AllocateNewStripe|()$
  End If
  $g\gets\lfloor s.n/G\rfloor$,$r\gets s.n\bmod G$
  For all $c\in C$
    If $\mathrm{IsNull}(v_c)$
      $s.P_{c,r}\gets 0$
    Else
      If $\mathrm{Align}(v_c)\ge 256\,\mathrm{MiB}$
        **error**
      End If
      $s.P_{c,r}\gets 1$
      $s.B_c\gets s.B_c\Vert\mathrm{Serialize}(v_c)$
      If $\mathrm{Compare}_c\neq\mathrm{NULL}$
        $s.K_{c,g}.\min\gets\min_{\mathrm{Compare}_c}(s.K_{c,g}.\min,v_c)$
        $s.K_{c,g}.\max\gets\max_{\mathrm{Compare}_c}(s.K_{c,g}.\max,v_c)$ // $\bot$ is the identity
      End If
    End If
    $s.K_{c,g}.\mathit{cnt}\gets s.K_{c,g}.\mathit{cnt}+1$
  End For
  If $r=G-1$
    `SerializeGroup`($s$, $g$, $G$)
  End If
  $s.n\gets s.n+1$
  If $s.n\ge S$
    `FlushStripe`($s$)
  End If
End Procedure

Procedure `SerializeGroup`($s$, $g$, $k$)
  $s.N_g\gets k$
  For all $c\in C$
    $s.\mathrm{chunk}_{c,g}.P\gets \mathrm{PackBits}(\langle s.P_{c,r}\rangle_{r=0}^{k-1})$
  End For
  For all $c\in C$
    $s.\mathrm{chunk}_{c,g}.V\gets\mathrm{Compress}(s.B_c)$ // identity if incompressible
    $s.B_c\gets\varepsilon$
  End For
End Procedure

Procedure `FlushStripe`($s$)
  $k\gets s.n\bmod G$
  If $k>0$
    `SerializeGroup`($s$, $\lfloor s.n/G\rfloor$, $k$)
  End If
  $m\gets\lceil s.n/G\rceil$
  For all $c\in C$
    $\mathrm{Write}(\langle s.\mathrm{chunk}_{c,g}.P\rangle_{g=0}^{m-1})$
    $\mathrm{Write}(\langle s.\mathrm{chunk}_{c,g}.V\rangle_{g=0}^{m-1})$
  End For
  `WriteCatalog`($s$)
  $s\gets\mathrm{NULL}$
End Procedure

Procedure `WriteCatalog`($s$)
  $\mathrm{Write}(\langle s.N, s.K, 0^{s.n} \rangle)$ // Saves row count for group, aggegate stats and deletion mask
End Procedure
```

In’s important that closing the writer also flushes any leftover stripe, so all in-memory buffers are saved to disk when transaction is committed.

As presented on figure [storage heap to stripe](../project_postgres_storage/#fig-storage-heap-to-stripe) column chunk stores a presence bitmap, a value buffer of only the present values, and aggregated values used later for delettion and vectorized aggregation: the smallest and largest non-NULL values (when the type has a comparison function), and a row count (`value_count`) counting all values in the chunk including NULLs.

To support deletion, the chunk group stores `deleted_rows`, along with separate row mask representing which rows were deleted. `DELETE` only sets the matching mask bit in the chunk group’s mask and increments `deleted_rows`. This allows BioseqDB to offer fast deletion without the need to rewrite the columns themselves. The wasted storage can be however reclaimed:

- `VACUUM (TRUNCATE)` may rewrite last stripes in table whose undeleted rows still fit in one stripe and whose decompressed size is below $1\,\mathrm{GB}$. A single last stripe is rewritten only when more than $20\%$ of its rows are deleted. The rewrite copies undeleted rows into a new stripe and drops the old ones.

- The internal tables storing sequence fragments cannot be rewritten as each locator needs to store the row number. Supporting VACUUM for sequence fragments would need to be dispatched to all tables including user table. Currently VACUUM is only supported on the user table.

- `VACUUM FULL` can rewrite the whole table to reclaim wasted space.

### Smart reads

In BioseqDB it’s unnecessary to read all chunks all the time. The two optimisations are:

1.  *Projection* means that only selected columns from `SELECT` query are red. This is trivial optimisation.

2.  *Min/max pruning* is more interesting one. For an ordered column, the writer records the smallest and largest non-NULL values in each chunk (in aggregates fields seen in figure [storage heap to stripe](../project_postgres_storage/#fig-storage-heap-to-stripe)). The BioseqDB TAM presented in figure [bioseqscan hook lifecycle](../project_postgres_custom_scan/#fig-bioseqscan-hook-lifecycle) can determine if none of the values in the chunk group satisfy the filter. If that happens, entire chunk group is skipped. For example, a group with sample identifiers from 1 to 100 cannot satisfy `BETWEEN 500 AND 600`. <a id="lst-storage-minmax-pruning"></a>

Figure [column store layout](../project_postgres_storage/#fig-column-store-layout) shows both optimisations. Group that satisfies both of these conditions still needs to apply filters to each row individually, but I save time but not touching groups that are not revelant to the search. The figure assumes query `SELECT id FROM table WHERE sample_id BETWEEN 500 AND 600` is executed.

<a id="fig-column-store-layout"></a>

![02 columns and pages diagram](/static/figures/project/chapters-11-postgres-storage-02-columns-and-pages-tikz-04.svg)

*Column selection and group pruning for a simple query `SELECT id FROM table WHERE sample_id BETWEEN 500 AND 600`. Blue cells are read, orange crosses mark groups whose aggregates exclude all the rows. Unrequested sequence locators are unread, because they were not specified in the `SELECT` clause. In the drawing I only illustrate the idea of reading rows, not their actual layout in storage.*

> [!NOTE]
> **Prunning for sequence columns**
>
> Sequence columns with locators are excluded for min/max pruning, but it’s possible to implement that feature theoretically. I decided not to do it, because storing min/max values would be simple as locator isn’t big object, however computing these values for very large sequences will be computionally expensive and would negatively impact table inserts.

### Compression

Figure [storage column streams](../project_postgres_storage/#fig-storage-column-streams) shows in detail how presence bitmap is computed. If the NULLs are very frequent, I will save the space by not storing them.

<a id="fig-storage-column-streams"></a>

![02 columns and pages diagram](/static/figures/project/chapters-11-postgres-storage-02-columns-and-pages-tikz-05.svg)

*Presence bitmap is useful to enable better NULL compression*

The Hydra code that BioseqDB columnar TAM was based on supported PGLZ, LZ4, or Zstandard, depending on the build and table options. In BioseqDB all compression is disabled by default as I would like to benchmark in the future best (and more modern) compression algorithms decicated for example to sequence storage. Abadi et al. show that the choice of compression method depends on both the data and the query workload \[[101](../bibliography/#ref-abadi2006compression)\], so I leave it as future extension.

## Raw byte storage

<a id="sec-pg-storage-pages"></a>

After stripe is serialized and flushed it needs to be written to disk - function $Write$ and $WriteCatalog$ in the pseudocode [storage flush stripe](../project_postgres_storage/#alg-storage-flush-stripe)

- Physically first written is the presence bitmap, then the value buffer for a column, then each column follow. These are placed in the user relation database pages.

- Catalog (row counts, aggregates and deletion mask) is written separately from the stripe data and it’s placed in usual heap rows i.e BioseqDB introduces separate relations for storing chunks information.

Catalog is represented by four relations inside the BioseqDB:

<a id="lst-storage-metadata-catalogs"></a>

```sql
CREATE TABLE bioseq_internal.stripe (
  storage_id bigint,
  stripe_num bigint,
  file_offset bigint,
  data_length bigint,
  column_count integer,
  chunk_row_count integer,
  row_count bigint,
  chunk_group_count integer,
  first_row_number bigint
);

CREATE TABLE bioseq_internal.chunk_group (
  storage_id bigint,
  stripe_num bigint,
  chunk_group_num integer,
  row_count bigint,
  deleted_rows bigint
);

CREATE TABLE bioseq_internal.chunk (
  storage_id bigint,
  stripe_num bigint,
  attr_num integer,
  chunk_group_num integer,
  minimum_value bytea,
  maximum_value bytea,
  value_stream_offset bigint,
  value_stream_length bigint,
  exists_stream_offset bigint,
  exists_stream_length bigint,
  value_compression_type integer,
  value_compression_level integer,
  value_decompressed_length bigint,
  value_count bigint
);

CREATE TABLE bioseq_internal.row_mask (
  id bigint,
  storage_id bigint,
  stripe_id bigint,
  chunk_id integer,
  start_row_number bigint,
  end_row_number bigint,
  deleted_rows integer,
  mask bytea
);
```

Metadata catalogs for flushed stripes.

When writing stripe data I need to take care about page boundaries. Let $\mathrm{BLCKSZ}$ be the PostgreSQL page size in bytes (this is compile-time constant provided by Postgres), $H$ the page-header size (technically $\mathrm{sizeof(PageHeaderData)}$) \[[107](../bibliography/#ref-postgresql17_storage_page_layout)\], and $P=\mathrm{BLCKSZ}-H$ the remaining page capacity. If $x\geq0$ is the total offset of the $x$-th byte, then page coordinates are defined as: $$\operatorname{block}(x)=2+\left\lfloor x/P\right\rfloor,
  \qquad
  \operatorname{position}(x)=H+(x\bmod P).$$ BioseqDB will write multiple pages for each stripe, so I need to take additional care about buffers going through multiple page boundaries as seen on figure [storage pages](../project_postgres_storage/#fig-storage-pages).

<a id="fig-storage-pages"></a>

![02 columns and pages diagram](/static/figures/project/chapters-11-postgres-storage-02-columns-and-pages-tikz-06.svg)

*I use logical offsets $\operatorname{block}(x)$ and $\operatorname{position}(x)$ to hide page boundaries. Diagram shows that we want to write some fragment of in-memory buffer $[8,15)$ to the disk, but it needs to be split across multiple pages. In reality the dimensions are larger than just 20 bytes, but it illustrates the idea.*

The original idea comes from Citus page writer that it’s avaialble in their public repository[Citus columnar page mapping · `columnar_storage.c 118`](https://github.com/citusdata/citus/blob/v14.0.0/src/backend/columnar/columnar_storage.c#L118)

To allow concurrent access (queries during inserts) I need to note that the writer does the following things:

- **Reservation:** Identifiers for chunks and groups is done under the relation extension lock.

- **Write:** Writes to pages are done using PostgreSQL buffer locks and generic write-ahead logging (WAL) \[[109](../bibliography/#ref-postgresql17_generic_wal)\].

- **Record completed data:** The logic uses Postgres’ Snapshots so data will be visible only after the transaction is committed.

## Locator implementation

<a id="sec-pg-storage-sequence-indirection"></a>

Sequence locator are implemented as 40-byte struct in C++. The struct must also include the Postgres header so it’s effectively 56 bytes in total. The Postgres header stores alphabet code and base storage unit as `TYPMOD` field (custom field for storing type modifier). I don’t provide small-sequence optimisation in BioseqDB, because that would complicate the implementation (instead of storing locator for small sequences we can potentially store sequence directly in the user table. This would require storing it’s padding so potential maximum sequence length that could be stored inline would be $(40B - 1B)*8/2 = 156$ symbols for DNA4).

<a id="lst-storage-sequence-locator"></a>

```cpp
struct sequence_locator {
  std::uint8_t  payload_version;   // format version used by me to change the locator format (currently it's just set to 3)
  std::uint8_t  bucket_index;      // length bucket 0..34
  std::uint16_t reserved_16;       // pad so shard_index is 4-byte aligned, must be 0
  std::uint32_t shard_index;       // fragment relation inside the bucket
  std::uint32_t fragment_count;    // consecutive fragment rows
  std::uint32_t reserved_32;       // pad so logical_length is 8-byte aligned, must be 0
  std::uint64_t logical_length;    // sequence length in symbols
  std::uint64_t first_fragment_row;// row of fragment 0
  std::uint64_t source_epoch;      // immutable source version, must be > 0
};
```

The 40-byte locator stored after the 16-byte sequence header.

- the internal tables are divided into groups by sequence length and these groups are divided into shards (so I record both of these value to identify sequence)

- the number of row for sequence first fragment and number of fragments (i.e number of rows occupied by sequence)

- sequence length in symbols (logical length)

- version of the sequence in case it’s changed (source epoch)

- version of locator format (not relevant for users)

> [!NOTE]
> **Locator format optimisation**
>
> Currently locators size could be reduces as I don’t need to store version (it was used for developent) and I waste too much space for padding. It’s potential oppornunity to optimsie in the future (it wasn’t a priority as locators occupy overall small space in comparison to sequences themselves).

Each sequence as mentioned in section desribing sequence interfaces [compact sequence range](../project_sequence_abstractions/#sec-compact-sequence-range) has it’s alphabet type and base storage unit. Base store unit is configurable from SQL. User can declare any power of 2 that is width of supported machine integer:

<a id="lst-sql-storage-widths"></a>

```sql runnable id=project_lst-sql-storage-widths timeout=30s expect=error sqlstate=22023
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
-- bioseq:visible
SELECT 'ACTGTGCAACCCCTG'::bioseq.seq('dna4', 64);
SELECT 'ACTGTGCAACCCCTG'::bioseq.seq('dna4', 32);
SELECT 'ACTGTGCAACCCCTG'::bioseq.seq('dna4', 16);
SELECT 'ACTGTGCAACCCCTG'::bioseq.seq('dna4', 8);
-- Example for DNA5 (floor(64/3) = 21 symbols per unit)
SELECT 'ACTGTGCAACCCCTG'::bioseq.seq('dna5', 64);
-- This is illegal as there's no uint128_t. 128-bit unit is just equivalent to two 64-bit units.
SELECT 'ACTGTGCAACCCCTG'::bioseq.seq('dna4', 128);
-- bioseq:teardown
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Examples of different base storage units

> [!NOTE]
> **Sequence fragments**
>
> <a id="info-sequence-fragments"></a> Sequences are divided into fragments to enable lazy loading. A fragment is $1024$ packed storage units, so actual bytes occupied depend by the width of storage supplied by the user. The maximum possible fragment size is for 64-bit storage unit and it’s $1024*8B = 8192B$ (note that this goes beyond default postgres page size of $8192B$, because page needs to include the page header as well)

Fragment abstraction is a separate layer above columnar storage. Shards for fragments are internally held as `CREATE TABLE ... (seq bioseq.seq(...)) USING bioseq`, so each fragment is one TAM row in that single sequence column.

> [!WARNING]
> **Storage considerations**
>
> <a id="warn-storage-considerations"></a> Fragments always storing 1024 storage units which works well for long sequences, however if the sequence is short then only small number of storage units will be occupied, while the rest is filled with zeros. Compression (default is pglz, but zstd is avialable as well and others for manual building) will take care of this problem and shrink the fragment. However it still has measureable effects on processing in the pipeline as BioseqDB will waste space in memory for very short sequences. This along with locator format optimisation is a task for future work. If we assume occupancy rate of 50% then minimal sequence size for 32-bit storageshould be $1024 * \frac{32}{2} * 0.5 = 8192$ symbols and for smallest possible storage: $1024 * \frac{8}{2} * 0.5 = 2048$ symbols.

<a id="fig-sequence-fragment-flow"></a>

![03 fragments and shards diagram](/static/figures/project/chapters-11-postgres-storage-03-fragments-and-shards-tikz-01.svg)

*A $40\,000$-symbol DNA4 value with $32$-bit units uses $1024 \times 32/2 = 16\,384$ symbols per fragment, so two full fragments and one partial fragment. Request $[30\,000,34\,000)$ reads fragments 1 and 2. Each stored row is $4112$ bytes ($4096$ payload plus the $16$-byte sequence header). The TAM concatenates those in column buffers via writer code [storage flush stripe](../project_postgres_storage/#alg-storage-flush-stripe) and during flush, writes them on $8192$-byte pages, skipping the $24$-byte `PageHeaderData` ($8168$ usable bytes). One fragment fits in a page, but two concatenated fragments do not ($8224>8168$), so fragment 1 continues after the next header. The compression will shrink the last fragment as it’s mostly zeroed, but the illustration assumes no compression happened.*

### Two levels of sharding

Postgres tables have limits on the number of stored rows, so in the most extreme cases the amount of sequence data can be more than one table can hold, especially if we store many full human genomes. A (single copy i.e haploid) human genome is about $3.1$ to $3.2$ billion bases ($3.2\times 10^{9}$ symbols and the $32$-bit DNA4 unit from the figure above). PostgreSQL documents a maximum table size of $32$ TB at $8192B$ default page size \[[108](../bibliography/#ref-postgresql17_limits)\]. That bound is a result of internal Postgres $32$-bit page index.

<a id="eq-storage-human-genome-table-cap"></a> $$\begin{aligned}
  F &= 1024 \times \frac{32}{2} = 16\,384
    && \text{symbols per fragment}\\
  n_{\mathrm{frag}} &= \Bigl\lceil \frac{3.2\times 10^{9}}{F} \Bigr\rceil = 195\,313
    && \text{fragment rows per genome}\\
  B &= 16 + 1024 \times 4 = 4\,112
    && \text{bytes per fragment row}\\
  P &= 8192 - 24 = 8\,168
    && \text{usable bytes per page}\\
  n_{\mathrm{page}} &= \Bigl\lceil \frac{n_{\mathrm{frag}} B}{P} \Bigr\rceil = 98\,327
    && \text{pages per genome}\\
  n_{\mathrm{seq}} &= \Bigl\lfloor \frac{4\,294{,}967{,}294}{n_{\mathrm{page}}} \Bigr\rfloor = 43\,680
    && \text{genomes in one table}
\end{aligned}$$

Without compression, one fragment table can therefore hold about $43{,}680$ human-genome sequences before it runs out of pages. Practically compression rates for human genomes were already measured \[[1](../bibliography/#ref-kryukov_comparison_of_fasta_hg_compression)\] and zstd is capable of compressing a human genome to effectively $790$ MB ($7.9\times 10^{8}$ bytes). With that size, each genome occupies $96{,}719$ pages, so the same limit allows $44{,}406$ genomes in one table.

> [!NOTE]
> **Table limits**
>
> I present this calculation to present that hitting the table limit isn’t very improbable. There exist datasets bigger than this limit. To prvent users from hitting limit and requiring them to manually spli tables, I decided to provide internal sharding.

The internal sharding allows fragments to spill over into multiple tables. If the underlying table achieves $1TB$ limit (which is lower than theoretical $32$ Postgres limit), fragements are stored in newly created columnar relations. The sharding is two-level:

- length bucket: groups sequences by their logical number of symbols - precisely $max(0, min(\left\lfloor log_2(|s|)\right\rfloor - 8, 34))$ is the bucket index.

- shard: inside one length bucket BioseqDB can create multiple tables when existing tables reach $1TB$ limit (potentially to store extremally large datasets)

The current layout has 35 buckets. The first covers lengths $[0, 2^8)$ and each next bucket covers $[2^i, 2^{i+1})$ for $i=8..40$. The largest bucket is designated to store sequences longer than $2^{41}$ symbols.

> [!WARNING]
> **Sharding limitations**
>
> It’s very important to note that current during stream access policy [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle) the sequence size is not known until its end is reached, thus my code in BioseqDB decides to store the sequence in the largest bucket (34) even when the sequence is short. This code path is utilised by IO submodule that loads sequences from FASTA file. This means that each sequence loaded from FASTA currently will be stored in the largest bucket. I plan to resolve this problem in the future - solution could be adding ability to IO submodule to search for first sequence end and assume all sequences are similar sizes. Alternatively I could write one sequence and then adjust the bucket after size is known. Both of these solutions however add additional overhead and complexity to the FASTA loader module. Additionally length-based sharding can allow better utilisation of fragments memory (so fragments are smaller even without compression [storage considerations](../project_postgres_storage/#warn-storage-considerations) and they require fewer bytes when allocated in RAM)

<a id="fig-storage-length-buckets"></a>

![03 fragments and shards diagram](/static/figures/project/chapters-11-postgres-storage-03-fragments-and-shards-tikz-02.svg)

*Length-based buckets are choosen based on the sequence length log 2, however sequences coming from FASTA/FASTQ have no known length and are streamed, thus internal code always store them in the largest bucket (34). There are multiple tables (called shards) per one bucket depending if we reach $1TB$ limit.*

## MVCC and deletion for sequence fragments

<a id="sec-pg-storage-versions"></a>

I discussed MVCC support when talking about Postgres Snapshots in chapter about BioseqDB custom scans [scan row stability](../project_postgres_custom_scan/#sec-scan-row-stability). In custom scan case support is rather simple and provided by the Postgres engine itself. It only required me to correctly use it. Storing fragments is entirely separate problem to visibility in columnar storage, because multiple queries can operate on locators, while some other process can modify sequence data in the same time.

Deleting sequence require doing two operations:

- Deleting row for fragment, which under the hood will mark the row as deleted in the row-mask metadata.

- As fragment is separate abstraction layer than columnar storage, I also store versions of rows in locators, so deletion just needs to invalidate that version (epoch)

Each epoch has stored separate row in the catalog table (so it’s one entry per sequence version):

<a id="lst-storage-source-version"></a>

```sql
CREATE TABLE bioseq_internal.sequence_source_version (
  source_epoch bigint PRIMARY KEY, -- immutable version id, must be > 0
  user_relid oid,                  -- parent user table
  database_oid oid,                -- parent database
  tablespace_oid oid,              -- parent tablespace
  main_relfilenode oid,            -- parent file node
  main_storage_id bigint,          -- parent TAM storage generation
  column_attno smallint,           -- 1-based sequence column
  locator_version smallint,        -- locator format, currently 3
  bucket_idx smallint,             -- length bucket 0..34
  shard_idx integer,               -- shard inside the bucket
  fragment_relid oid,              -- fragment table, NULL if empty
  fragment_storage_id bigint,      -- fragment TAM storage generation
  first_sub_row_no bigint,         -- row of fragment 0
  num_fragments bigint,            -- consecutive fragment rows
  total_bp_size bigint,            -- sequence length in symbols
  alphabet_code smallint,          -- packed alphabet
  storage_unit_byte_width smallint,-- unit width in bytes
  padding_letters integer,         -- unused letters in last packed unit
  source_content_checksum bigint,  -- CRC-64 of decoded symbols
  state smallint                   -- 0 unpublished, 1 ready, 2 invalid
);
```

Heap catalog for one immutable sequence version. Invalidation updates `state`.

The specific epoch’s `state` follows very simple linear transitions:

- **Unpublished** ($0$). An insert has allocated an epoch (sequence version) and written fragments. Locator is then filled with state $0$ which is default one. It means that the locator in the user table is not yet written (so the fragment is there, but there’s no saved locator that points to it)

- **Ready** ($1$). Locator was written to a table, so I update the epoch state to mark that this sequence version is used somewhere (not doing that might lead to dangling fragments that occupy storage but have no existing locator). This state is crucial to prevent a situation in which locator write fails or is rolled back, but fragment was written.

- **Invalid** ($2$). Locator cannot read this row version anymore, the underlying fragment shouldn’t be accessed. This happens when user either calls `DELETE` or `UPDATE`.

Note that the catalog contains far more information that is neccessary. I decided to save CRC hashes to make sure the fragment is not malformed during operations or shard coordinates shared by the locator itself. As each row version requires separate entry this limits the maximum number of sequences stored in the BioseqDB, however entries are cheap so I didn’t incorporate it into estimate calculations [storage human genome table cap](../project_postgres_storage/#eq-storage-human-genome-table-cap).

<a id="fig-storage-update-snapshots"></a>

![04 versions and deletion diagram](/static/figures/project/chapters-11-postgres-storage-04-versions-and-deletion-tikz-01.svg)

*Postgres snaphot mechanism allows to check visivility of rows in MVCC model, but it’s not sufficient on its own. Let’s assume that reader A and reader B are reading the same sequence before and after other process commited update. Using fragments epochs versioning system means that read A will see older locator via snapshot, while reader B will get newer locator row with changed epoch. Both of them can complete reads as both epochs are still valid.*

Listing [storage repeatable read](../project_postgres_storage/#lst-storage-repeatable-read) gives SQL code for this example:

<a id="lst-storage-repeatable-read"></a>

```sql
-- Session A
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT bioseq.to_text(seq[0:8]) FROM reads WHERE id = 1;
-- ACGTACGT

-- Session B, while A remains open
BEGIN;
UPDATE reads
SET seq = repeat('TGCA', 4096)::bioseq.seq('dna4', 32)
WHERE id = 1;
COMMIT;

-- Session A
SELECT bioseq.to_text(seq[0:8]) FROM reads WHERE id = 1;
-- ACGTACGT
COMMIT;
SELECT bioseq.to_text(seq[0:8]) FROM reads WHERE id = 1;
-- TGCATGCA
```

A repeatable-read transaction keeps the old sequence version. Initially, row id 1 begins with ACGTACGT.

### BioseqDB all cache layers

<a id="sec-pg-storage-caches"></a>

The optional column cache stores decompressed stripe chunks. It is separate from the scanner-owned packed fragment slots in Figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle).

The setting `bioseq.column_cache.enabled` controls the decompressed cache. The original benchmark campaign disabled it, whereas binary releases enable it by default. It stores decoded chunks inside one PostgreSQL backend process. Its key contains relation, stripe, chunk group, and column identifiers (the configured capacity when enabled defaults to 200 MiB)

The cache can return borrowed pointer to a decompression buffer for a chunk, which allows to prevent additional chunk loads wasting time on decompression multiple times. As I discussed all methods of caching within BisoeqDB already, I present table with comparison of all cache layers:

*Different types of cahces relevant in context of BioseqDB*

<a id="tab-storage-cache-scopes"></a>

| **Name of the cache method** | **Saved data** | **Description** |
|:---|:---|:---|
| PostgreSQL buffer | Relation pages | Internal buffer manager in Postgres loads/unloads pages and can pin pages to optimise page access. In BioseqDB I don’t go to such low level of abstraction and assume page loading is opaque mechanism. |
| Column cache [pg storage caches](../project_postgres_storage/#sec-pg-storage-caches) | Buffer with decompressed column chunks | Can cache decompressed chunks, so loading and decompression of the same chunks can be efficient. |
| Fragment slots [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle) | Copy of one packed fragment | Each scanner can store fragments in its own memory, so when pipeline needs to lookup sequence parts not far away it will resuse already loaded fragments |
| Query-result cache [seq results cache](../project_postgres_custom_scan/#sec-seq-results-cache) | Completed SQL result rows | Separate `bioseq_cache` subextension can store and share results after pipeline execution is finished. This extenion is disabled by default, but it can read saved results if the queries are same. |
