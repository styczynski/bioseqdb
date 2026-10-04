---
title: "Persistent sequence indexes"
slug: project_index_storage
description: "Persistent sequence indexes"
order: 110
keywords: [BioseqDB, project]
draft: false
---

\[79c9cc186b95f50bd899f69b48eba995ced810c6\] \[c77b5afa910bec52c784566e378cb6ebd3d0d453\]

## Keep the lookup work between queries

<a id="sec-index-storage-role"></a>

In previous chapter [pg storage role](../project_postgres_storage/#sec-pg-storage-role) I describe how columnar and sequence storage layers are implemented in BioseqDB. There’s however also aspect for storing indexes for sequences. We would like to be able to store computed minimizers (seeds/sketches) [minimap minimizer cpp decl](../project_minimizers/#lst-minimap-minimizer-cpp-decl) in a way that enables performant computations of alignments. They should be computed once when sequence is loaded from file or updated and then reused for subsequent select queries.

I want to discuss existing aligners’ indexing first.

## What existing sequence indexes store

<a id="sec-index-minimap2-comparison"></a>

### Indexing: minimap2

After computing minimizers they need to be grouped by their hash, minimap2 choose to use the lowest $b$ hash bits (by default $b=14$) to choose one of $2^b$ buckets \[[21](../bibliography/#ref-minimap2_paper), [73](../bibliography/#ref-minimap2_software)\]. Minimap2 builds a hash table per bucket whose keys are the remaining hash bits. This buckets with hasmaps are serialized into `.mmi` file. I performed in-depth analysis of how the `.mmi` format is specified based on available Github old issues \[[112](../bibliography/#ref-minimap2_issue_820), [113](../bibliography/#ref-minimap2_mmi_gist)\] and original code from minimap2’s `mm_idx_dump` and `mm_idx_load` functions. [Writing and loading an index part · `mm_idx_dump, mm_idx_load 475`](https://github.com/lh3/minimap2/blob/79c9cc186b95f50bd899f69b48eba995ced810c6/index.c#L475)

The index file is concatenation of blocks presented in the Figure [index mmi layout](../project_index_storage/#fig-index-mmi-layout):

- **Magic bytes** The bytes are `MMI` followed by `0x02` (`MM_IDX_MAGIC`).

- **Metadata** Minimap2 saves $w$, the $k$-mer length $k$, used bitcount $b$, the number of sequences $n_{\mathrm{seq}}$ and a flag word (for various minimap2-specific bitflags)

- **Sequence table** For each of the $n_{\mathrm{seq}}$ sequences the file stores 1-byte name length $\ell$ followed by $\ell$ ASCII characters representing the sequence name (header taken from original FASTA file), and a 32-bit sequence length

- **Buckets** The buckets store the hash-map, where key is remaining hash and bit indicating if there’s single value for that key or not. The hash-map is stored as two components however. First is the value list $p$ which is used when there are more than 1 value per key. Second is the hash array itself, which stores key/value pairs. If there are more than one value per key (last bit of key set to 1), then the value represents offset and count into the value list $p$ The values, either as values in the key/value pairs or entries in $p$ store sequence id, position and strand bit.

- **Bit-acked bases (optional)** If the `MM_I_NO_SEQ` flag is unset manually by user, bit-packed reference bases are saved at the end. <a id="lst-high-level-mmi-index-format-description"></a>

The depiction of the index file layout is presented in the Figure [index mmi layout](../project_index_storage/#fig-index-mmi-layout) below.

<a id="fig-index-mmi-layout"></a>

![02 mapper indexes diagram](/static/figures/project/chapters-12-index-storage-02-mapper-indexes-tikz-01.svg)

The idea of storing hash-map as two lists inside bucket is also presented in the Figure [index mmi construction](../project_index_storage/#fig-index-mmi-construction) below to allow easier understanding of the index concept.

<a id="fig-index-mmi-construction"></a>

![02 mapper indexes diagram](/static/figures/project/chapters-12-index-storage-02-mapper-indexes-tikz-02.svg)

*A simplified minimap2 hash bucket: hashes 13 and 21 have the same lowest two bits, but different remaining keys, so they land in bucket 1 together. The two occurrences of hash 21 are placed next to each other in the bucket’s position array.*

### Indexing: mmseqs2

There are alternative approaches in literature to building an index. MMseqs2 stores one index file that contains counts and offsets, and then writes larger index files that contain all occurence lists instead of bucketing them like minimap2 does. It’s worth noting that 2017 MMseqs2\[[102](../bibliography/#ref-Steinegger079681), [46](../bibliography/#ref-mmseqs2_2017)\] paper uses spaced k-mers which wasn’t the focus of this project, so I didn’t investigate more how the alignment is precisely done.

MMseqs2 offers great performance in clsutering, which is different problem to alignment, however connected as it involves estimation of edit distances for large datasets. The software also allows sets of taxonomy operations and alignment-based searches as well.\[[103](../bibliography/#ref-mmseqs2_linclust_2018)\]\[[105](../bibliography/#ref-mmseqs2_taxonomy_2021)\]\[[104](../bibliography/#ref-mmseqs2_desktop_2019)\]\[[114](../bibliography/#ref-mmseqs2_gpu_2025)\]. [Count and fill the MMseqs2 index · `IndexBuilder::fillDatabase 56`](https://github.com/soedinglab/MMseqs2/blob/c77b5afa910bec52c784566e378cb6ebd3d0d453/src/prefiltering/IndexBuilder.cpp#L56)

[Read a word’s entry range · `getDBSeqList 178`](https://github.com/soedinglab/MMseqs2/blob/c77b5afa910bec52c784566e378cb6ebd3d0d453/src/prefiltering/IndexTable.h#L178)

[Keep distinct word codes within one target · `addSequence 350`](https://github.com/soedinglab/MMseqs2/blob/c77b5afa910bec52c784566e378cb6ebd3d0d453/src/prefiltering/IndexTable.h#L350)

I illustrate the idea of MMseqs2 index on a small example in the Figure [index mmseqs inverted](../project_index_storage/#fig-index-mmseqs-inverted) below, where I just assume that we use 2-letter k-mers of natural language for simplicity.

<a id="fig-index-mmseqs-inverted"></a>

![02 mapper indexes diagram](/static/figures/project/chapters-12-index-storage-02-mapper-indexes-tikz-03.svg)

*A small example of MMseqs2’s list construction, where word/count/offsets triplets are stored in a single file - practically only indices are stored there, while the actual (very large) lists of entries are stored separately.*

## BioseqDB indexing abstractions

<a id="sec-index-abstraction-layers"></a>

Indexes are stored separately in BioseqDB [types of storage entities in bioseqdb](../project_postgres_storage/#lst-types-of-storage-entities-in-bioseqdb) to all internal sequence fragmented tables [storage length buckets](../project_postgres_storage/#fig-storage-length-buckets) and they are separate entities to the usual indexes in Postgres which provide generic access paths to the tables.

There is thin layer of abstraction over the indexes, so in the future implementation of other types of indexes is easy. The storage abstraction requires implementer only to specify the type and depending on it we only need to provide logic to write the index chunks. One problem with current state of BioseqDB is that the index reading is not abstracted, so we encapsulate details of implementation, but they leak into pipeline (as the pipeline currently needs to understand how we store minimizers).

> [!NOTE]
> **Seed abstraction**
>
> In the future I want to improve curent implementation to use abstract notion of "seed". I already extensively use templates, so introducing new concept to represents types of seeds isn’t a problem. This abstraction will allow to fully decouple indexing from the pipeline. Pipeline stages then could automatically detect how to compute certain seeds (like minimizers) and how to read and store them. Current BioseqDB implementation only uses minimizers, so I decided not to decicate my time to this idea.

I assume here that index store *chunks* which are separate abstraction to the chunks in columnar tables. One chunk consists of bytes the index implementation needs to write or store and I assume the chunk cannot be larger than $960MB$ (due to Postgre’s TOASTing limits). The chunks are stored in internal tables that does not use columnar storage, they’re ordinary heap tables - I decided that columnar storage in this case won’t give any advantage (although I didn’t A/B test this approach). Additionally, index tables are create `UNLOGGED` to avoid unnecessary logging and to reduce the amount of data written to the disk.

There are several supported methods of storing index:

- **`CHUNKED_PER_FRAGMENT`**. One fragment [sequence fragments](../project_postgres_storage/#info-sequence-fragments) of a sequence can have multiple chunks in the index.

- **`INLINE_PER_SEQUENCE`**. One sequence has a single chunk that is stored as Postgres varlena. This means that entire data stored for one sequence cannot exceed $960MB$ if that happens BioseqDB will error. `symbol_counts` is BiosqeDB index that uses this layout. It stores how many times each symbol occurs in that sequence and the payload is so small that it nicely fits into one small row.

- **`CHUNKED_PER_TABLE`**. This is special layout that in its implementation exposes only simple methods to access random chunks of the table. The idea is that some indexing structures may want to implement custom strategies, for example trees of sequences or graphs of fragments that do not fit into other representations. I don’t use this layout at the moment, but it’s supported to provide extension point in the future.

Figure [index storage layouts](../project_index_storage/#fig-index-storage-layouts) shows what three of these layouts are representing.

<a id="fig-index-storage-layouts"></a>

![03 mpf and mps diagram](/static/figures/project/chapters-12-index-storage-03-mpf-and-mps-tikz-01.svg)

In reality the fragment’s number can be viewed as a generic sub-key. This distorts the original meaning of **`CHUNKED_PER_FRAGMENT`**, because in relity it’s just "per-sequence" but with extra key: $\langle\text{query\_key, sub\_key, chunk\_no}\rangle$, while **`CHUNKED_PER_TABLE`** has $\langle\text{chunk\_no}\rangle$. This idea is depicted below by real SQL schema of internal storage:

<a id="lst-chunked-per-fragment-row"></a>

```sql
CREATE TABLE bioseq_internal.seqidx_mpf_... (
  query_key   BIGINT NOT NULL, -- which sequence in this shard
  sub_key     BIGINT NOT NULL, -- which item of that sequence (can be fragment number but doesn't practically have to)
  -- Then we store byte array defined by index implementation but split across chunks:
  chunk_no    INT    NOT NULL, -- 0, 1, 2, ... of that item's byte string
  chunk_bytes INT    NOT NULL, -- how many bytes this chunk holds
  blob        BYTEA  NOT NULL, -- the bytes of this chunk
  PRIMARY KEY (query_key, sub_key, chunk_no)
);
```

One heap row of a per-fragment index.

The payload of an index is one byte array. `CHUNKED_PER_FRAGMENT` stores that array as rows of Listing [chunked per fragment row](../project_index_storage/#lst-chunked-per-fragment-row). When the array is longer than one chunk, it is divided into chunk 0, chunk 1, and the following chunks. Those rows keep the same `query_key` and the same `sub_key`. A reader orders them by `chunk_no` and concatenates `blob`.

> [!NOTE]
> **Storage details**
>
> <a id="info-index-storage-details"></a> It’s important to note that for minimizers per fragment MPF the sub-key number is the fragment number. I use the same column to store another number when the index is not split by fragment. Technically this means that **`CHUNKED_PER_FRAGMENT`** should have its name changed to something more appropriate.

In code `pg_supported_indexes` is a special templated that connects those implementation classes to the registered indexes. This is frequent practice used by me - encoding implementation vs. abstraction pairs as tuples in type lists created using template metaprogramming, so if extension is required we only need to write new type and add it into the `pg_supported_indexes` template list. Checks if implementation is valid are done via C++26 concepts [mpf index storage check](../project_index_storage/#lst-mpf-index-storage-check):

<a id="lst-mpf-index-storage-check"></a>

```cpp
template <class p_storage>
struct minimizers_per_fragment_index {
  static_assert(is_index_chunked_storage_for<
      p_storage, sequence_index_layout::fragment_chunks>);
  static constexpr sequence_index_layout index_type =
      sequence_index_layout::fragment_chunks;
  using storage_type = p_storage;
};
```

Minimizers per fragment require fragment-chunk storage.

## BioseqDB supported indexing

<a id="sec-index-logical-layouts"></a>

There are 3 implementations of indexes inside BioseqDB:

- **`minimizers_per_fragment`** (shortened to `mpf`) stores minimizers calculated per one fragment and uses **`CHUNKED_PER_FRAGMENT`** layout.

- **`minimizers_per_sequence`** (shortened to `mps`) stores minimizers calculated per sequence, but as the number of minimizers grows they would not fit in a single chunk, thus one may consider using **`CHUNKED_PER_TABLE`**. However, as mentioned, the sub-key [index storage details](../project_index_storage/#info-index-storage-details) is a number, so in MPS I use this number as a hash-group id, not as a hash. The rows stay in the **`CHUNKED_PER_FRAGMENT`** layout even though these numbers are not fragment numbers, as seen on the figure below [index mpf mps](../project_index_storage/#fig-index-mpf-mps).

- **`symbol_counts`**: stores counter of occurences of each symbol in a sequence, uses **`INLINE_PER_SEQUENCE`** layout, because the counter struct is very small, so Postgres inlines the varlena into the row. <a id="lst-types-of-indexes-in-bioseqdb"></a>

The main idea behind MPF and MPS was:

- **MPF**: I can keep small hashmaps separated so random queries during seeding stage, where we need to find common minimizers between query and target, can hit CPU caches more often as the accesses are localized.

- **MPS**: The most frequent minimizers are trimmed by minimap2 and MPF trimming is done per fragment, so it’s less reliable and can remove minimizers that are locally frequent, but globally rare reducing total number of minimizers and speeding up processing at the cost of potentially missing some matches. Additionally, MPF needs to perform multiple hash lookups if we want to determine all locations of minimizer’s occurences so there’s also performance penalty. This penalty can be also mitigated by the fact that MPF is easier to parallelize. Thus we can see there are several important trade-offs and it’s not clear if the implementation of MPS should be more or less performant in theory. In case of BioseqDB as it would be very inneficient to store all minimizers individually, I decided to still generate minimizers per fragment, but store them and merge later during process that can potentially spill to disk (if the entire index fits into RAM then no spillage occurs, but if it doesn’t there are heavy performance penalties). Additionally I must note that MPS uses the trick described here [index storage details](../project_index_storage/#info-index-storage-details), so the sub-key is a hash-group number, not fragment number. This implies that if the query is short there will be not many groups to check, and as each group is held as heap rows, then MPS can be very fast, because it reads only the list of one group, not the entire index.

The figure [index mpf mps](../project_index_storage/#fig-index-mpf-mps) below shows 6 minimizer occurrences in a sequence, and panel (c) shows the byte strings that store them. Panels (a) and (b) show the same six minimizers in two groupings. Panel (c)and later illustrate the encoding of the strctures in binary strings (every rectangle onward is the binary string `blob` of one heap row from Listing [chunked per fragment row](../project_index_storage/#lst-chunked-per-fragment-row)), where the label on the left is that row’s `query_key` and `sub_key`. If a string is longer than one chunk, later chunks continue it under the same two numbers.

### Binary coding of non-grouped MPF

There are two types of MPF index depedning on BioseqDB setting flag: `bioseq.aligner.grouped_soa_generation.enabled`. The non-grouped version is depicted in panel (c). The sub-key is just the fragment number. The entry intself contains two arrays similar to what minimap2 [index mmi layout](../project_index_storage/#fig-index-mmi-layout) does. The first array with `controls` contains hash value, start position in `positions` array and count of how many positiosn have that same hash. `positions` just represent minimizer positions. Usually in headers there’s additional metadata and some of it is redundant as the exact shapes evolved during my implementation. There are versions and special magic values to potentially error if index was corrupted.

### Binary coding of grouped MPF

I tried to improve MPF by doing something more sophisticated. The MPF can changed format based on grouped generation setting flag. Panel (d) shows how grouped MPF looks like:

- This is similar to MPF buit instead of storing array of structures (AoS) we store structure of arrays (SoA) so hash, start, and count are now three arrays in a row.

- I use special bit i.e $2^{61}$ to store positions array separately in separate row.

- There also is one special directory row using unique `sub_key` value. The directory is list of $128B$ entries mapping fragment number to specific `sub_key` value.

- Each directory row $128B$ entry constains useful metadata like min(hash), max(hash) so some rows doesn’t have to be read at all if they don’t contain hash values we are interested in.

- Using this `sub_key`$\leftrightarrow$`fragment` mapping allows to potentially split fragment hash/start/count entry into multiple ones. However this is not used in the code currently and effectively `sub_key` $=$ `fragment` for all entries.

- The directory item also contains information that is redundant for example `controls_first` is number of first chunk storing hash/start/count binary string. This is always $0$ as the binary strings are split into chunks, which are zero-indexed. The same logic applies to other fields and the entry size could be massively reduced.

- Grouped-MPF is very generic format and it also supports variable-width entries. The width of hash/start/count/positions can be adjusted independently for example for very short sequences we could use $2B$ `position` array and for very low minimizer density we could use $2B$ for `start_array` or for very unrepetitive sequences where unique minimizers are very common, we could use $2B$ for `count_array`. Practically, current code uses currently same widths for most of the fields and hash is adjusted depending on $k$ and sequence length from minimizer type [minimizer representation layout](../project_minimizers/#fig-minimizer-representation-layout).

On the figure Panel (d) I didn’t depict all fields in the MPF grouped directory as not all fields are relevant. Table [grouped mpf directory entry](../project_index_storage/#tab-grouped-mpf-directory-entry) below lists all fields that are stored:

*Fields of one 128-byte grouped MPF directory entry.*

<a id="tab-grouped-mpf-directory-entry"></a>

| **Bytes** | **Size** | **Field** |
|:---|:---|:---|
| 0–3 | 4 | partition number (this is an index of entry in the directory row’s array) |
| 4–7 | 4 | fragment |
| 8–15 | 8 | min(hash) |
| 16–23 | 8 | max(hash) |
| 24–31 | 8 | sub-key (in theory was meant to use to allow dividing entry for fragment into multiple rows, but currently it’s always equal to frament number) |
| 32–35 | 4 | first chunk of the control byte string (chunks are zero-indexed so it’s always $0$ and should be removed in the future) |
| 36–39 | 4 | how many control chunks (counts in how many chunks hash/start/count binary array will use) |
| 40–43 | 4 | first chunk of the position array (chunks are zero-indexed so it’s always $0$ and should be removed in the future) |
| 44–47 | 4 | how many position chunks (counts in how many chunks position binary array will use) |
| 48–55 | 8 | how many controls (number of items in each of hash/start/count arrays) |
| 56–63 | 8 | how many positions (number of items in the position array) |
| 64–71 | 8 | byte size of the control byte string |
| 72–79 | 8 | byte size of the position array |
| 80–87 | 8 | checksum of the control byte string |
| 88–95 | 8 | checksum of the position array |
| 96 | 1 | hash width: 2, 4, or 8 |
| 97 | 1 | start-array width: 2, 4, or 8 |
| 98 | 1 | count width: 2, 4, or 8 |
| 99 | 1 | position width: 2, 4, or 8 |
| 100 | 1 | Used to represent differnt orders of hash/start/count arrays. Now meaningless and fixed to $1$ |
| 101 | 1 | Used to reresent how many positions there are per one hash entry. It’s always $1$, so this is meaningless. |
| 102 | 1 | Meaningless now, fixed to $2$ |
| 103 | 1 | Number of extra entries in start aray at the end. Now it’s meaningless and fixed to $1$ (and we don’t need to store this one extra item at all) |
| 104–127 | 24 | zeros |

<a id="tab-fields-in-grouped-mpf-directory-entry"></a>

### Binary coding for MPS

The MPS index stores the same rows as non-grouped MPF, but this time we also have additional one directory row. Also the fragment keys are just not reffering to fragments anymore, but they represent groups of hashes similarily to minimap2 [index mmi layout](../project_index_storage/#fig-index-mmi-layout).

The directory contains metadata information like how many distinct hashes are there in total and for every group it stores min(hash), max(hash) to allow us to quickly skip groups if certain hash values do not exist there.

<a id="fig-index-mpf-mps"></a>

![03 mpf and mps diagram](/static/figures/project/chapters-12-index-storage-03-mpf-and-mps-tikz-02.svg)

MPS index building needs to spend additional work on sorting and merging so later queries can skip unrelated hash ranges, while MPF just keeps smaller hashmaps based on fragments. The current default BioseqDB active minimizer index is MPF.

### Index versioning

<a id="sec-index-compatibility"></a>

Internal sequence storage tables use `epoch` to provide sequence versioning [storage sequence locator](../project_postgres_storage/#lst-storage-sequence-locator), indexes don’t have to use separate versioning field. I decided not to store epoch in the index table [fields in grouped mpf directory entry](../project_index_storage/#tab-fields-in-grouped-mpf-directory-entry), but instead `query_key` is number of the row inside the table shard. They are stable with respect to `INSERT`s, `DELETE`s and `UPDATE`s as update will create new sequence version. Special handling is only required when tables are rewritten using `VACCUM FULL`: vacuum full would cause the rows to change numbering, however the indexes tables contain a hash of database, table space and Postgres `relfilenode`, so if the table is rewritten (`relfilenode` will change) BisoeqDB won’t detect the old index. This is the most naive approach, but provides basic protection against table rewrites.

### Symbol count index

<a id="sec-index-symbol-counts"></a>

I implemented one index type that does not store minimizers at all: `symbol_counts` index keeps a very small symbol histogram for one sequence. Each symbol has 64-bit counter (for DNA4, the counters are $A,C,G,T$ so sequence `AACGTA` gives $(3,1,1,1)$). The counts are stored separately because as we use heap rows the value will be stored inline in Postgres table. This allows very fast access and usually users don’t need to count symbols.

<a id="fig-index-symbol-counts"></a>

![03 mpf and mps diagram](/static/figures/project/chapters-12-index-storage-03-mpf-and-mps-tikz-03.svg)

*Hitsogram counter for symbols is stored separately so BioseqDB access it only when specified in the query*

<a id="lst-index-symbol-counts"></a>

```sql runnable id=project_lst-index-symbol-counts timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE reads (id integer, seq bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO reads VALUES (1, 'ACGTAACCGTTA'), (2, 'TTTTAAAA');
-- bioseq:visible
SELECT id, bioseq.symbol_count(seq, 'A') AS a_count
FROM reads;
-- bioseq:teardown
DROP TABLE reads;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Request a symbol count for each stored sequence.

Using this small metadata index allows me to never access the sequence content at all, meaning the read is very fast. If the index entry is not found, then BioseqDB will traverse entire sequence. The primary benefit is that this can be computed when loading FASTA or inserting rows autmatically so the user pays the cost only once (or later when infrequent `INSERT` or `UPDATE` happen)

## Building MPS

<a id="sec-index-construction"></a>

Building MPF index is rather simple as we traverse each fragment and can separately construct hashmaps with minimizers (and hashmaps for fragmenst are relatively small compared to all RAM capacity). Constructing MPS is more complex as I need to somehow store all minimizer globally and this can potentially be bigger than entire RAM capacity. Index builder could collect every $(hash,position,strand)$ record and sort the complete vector, but for maps bigger than RAM this couldn’t be done efficiently, instead current code does similar thing to MPF with a slight differences:

- To reduce memory access costs as much as possible MPF treats each fragment as entirely separate sequence, thus minimizers on edges won’t be generated (the first and last $k+w-2$ symbols). This reduces minimizer density at edges but means that we access only one fragment to generate index entry.

- For MPS I use subtly different approach and I collect all minimizers. This means that during parallelized fragment read, the reader need to read not only own fragment, but also parts of fragment to the right and left. This is depicted as "context" on the figure below [index build halos](../project_index_storage/#fig-index-build-halos).

<a id="fig-index-build-halos"></a>

![04 bounded construction diagram](/static/figures/project/chapters-12-index-storage-04-bounded-construction-tikz-01.svg)

*Each reader to generate minimziers for one fragment in MPS read also neighbouring fragments so first and last minimizers in fragment can be generated. Size of "context" is $w+k-2$ symbols.*

MPS does the following steps to build entire index:

1.  Read fragments and for one fragment compute minimizers, sort them by hash and remove duplicates similarily to MPF.

2.  Write this computed MPF-like part into temporary file storage.

3.  After all fragments have temporary files, then select fixed number of them and merge them into one sorted temporary file.

4.  Continue this merging procedure until one temporary file remains.

5.  Read the final temporary file and build directory row inside storage.

<a id="fig-index-mps-merge"></a>

![04 bounded construction diagram](/static/figures/project/chapters-12-index-storage-04-bounded-construction-tikz-02.svg)

*Merging of hash entries (hash value and minimizer position)until one final entry is left and can be stored permamently in index storage. Letters represent different hash values.*

The spill path uses PostgreSQL’s `BufFile` temporary-file interface. The sorter retains sorted runs in RAM when memory admission succeeds and spills them when it cannot reserve that memory. It can also move a retained run to a temporary file to make room for later work. Thus the merge structure in Figure [index mps merge](../project_index_storage/#fig-index-mps-merge) does not require every run to be written to disk.

## Selective reads

<a id="sec-index-cold-loading"></a> <a id="sec-index-mps-selective-reads"></a>

Main benefit of MPS is that each group can be a single row within Postgres (if $n$, $m$ are not causing the array to spill over multiple chunks [index storage layouts](../project_index_storage/#fig-index-storage-layouts)). This means that minimap2 has to read and check every entry in index, while BioseqDB can only read entiore directory row and using $min(hash)$ and $max(hash)$ values to decide if we should read certain groups of minimizers.

The similar logic applies to grouped MPF index as well, but in case of grouped MPF we can also check count array and if the count is very high, then we can abandon readind reparate position array at all (the most frequent minimizers should be filtered out as they are not good candiates for anchors).

The hash-major MPS reader performs the following steps:

1.  Read and check the complete directory, then find hash ranges that intersect with query hashes.

2.  Read the selected controls to find each key’s exact global occurrence count

3.  Apply the occurence count threshold: if the count is high, we don’t process this hash

4.  Read the chunks selected by this filtering and read positions form them

<a id="fig-index-selective-load"></a> <a id="fig-format6-streams"></a>

![05 selective reads diagram](/static/figures/project/chapters-12-index-storage-05-selective-reads-tikz-01.svg)

*Two decisions save different reads. All directory entries are read; blue ranges select the controls. Counts reject a frequent hash before its long list is loaded. Positions and thresholds are illustrative.*

## Other index considerations

<a id="sec-index-operational-considerations"></a>

Indexes can automatically update themselves on index, however currently enabled by default is grouped MPF index, which due to bugs cannot correctly return minimizers which leads to recomputing them when no explicit index read policy is specified and it doesn’t support update on insert. More details are presented in the Appendix 1, where I also present concrete examples of how the BioseqDB should be used.

This bug was caused by the fact that grouped MPF not only produces relational index, but also saves that compressed relation chunks into separate file that is used to enable faster reads during JOINs. The reader by default rejects ordinary single table queries, so you need to change the GUC to disable grouped-MPF (and use regular MPF instead). Saving file enables the JOIN to concurrently access stored data which is a cheap workaround around the decision to only use single Postgres reader process.

<a id="fig-index-join-read-paths"></a>

![07 policies and reuse diagram](/static/figures/project/chapters-12-index-storage-07-policies-and-reuse-tikz-01.svg)

*For large joins grouped-MPF uses saved direct binary dump of relational index, which speeds up the alignment by reducing time to sequentially read the backing table*

All indexes in BioseqDB can be automatically updated on insert (but as noted earlier, grouped-MPF will produce error on insert due to buggy implementation)

*Maintenance policies*

<a id="tab-index-update-policy"></a>

| **Policy** | **Description** |
|:---|:---|
| `UPDATE_ON_INSERT` | When user inserts data, we automatically compute and store minimizers for new sequence |
| `UPDATE_MANUALLY` | User needs to manually call `bioseq.update_index` |

*Default policies in BioseqDB*

<a id="tab-index-policy-defaults"></a>

| **Index** | **Policy for user updates** | **Description** |
|:---|:---|:---|
| `minimizers_per_fragment` | `UPDATE_ON_INSERT` | Default minimizer index |
| `minimizers_per_sequence` | `UPDATE_MANUALLY` | Not selected by default |
| `symbol_counts` | `UPDATE_ON_INSERT` | Used when user demans to count symbols in a sequence |
