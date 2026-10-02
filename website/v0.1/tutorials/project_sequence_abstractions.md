---
title: "Sequence representations and lazy evaluation"
slug: project_sequence_abstractions
description: "Sequence representations and lazy evaluation"
order: 80
keywords: [BioseqDB, project]
draft: false
---

## Designing sequence abstraction

<a id="sec-sequence-abstractions"></a>

Suppose user stores large genome sequence in one row, but only wants to align specific small part of it (let’s say we know location of one protein-coding human gene and are interested only in finding occurences of that gene). It would be very wasteful if I didn’t provide built-in functionality to load only that small region. This region could be stored on disk, so we only have it’s locator poiting to a specific chunk in the table.

> [!NOTE]
> **Physical storage**
>
> "Locator" refers to a small structure holding sequence metadata and pointer to its actal location in the storage system. More on is explained in physcial storage chapter [pg storage role](../project_postgres_storage/#sec-pg-storage-role). For now, I assume it’s only an opaque pointer to sequence and use term "hydration" describing the process of converting it into a sequence itself or its fragment (as continous byte array in RAM).

On the other hand, we can load sequences from fasta file, where parser can be writing to RAM, so our sequence could refer to location in memory, where all sequences are pooled into one memory region to avoid constant allocation/reallocation. In yet another case, in tests we would like to use standard library containers to prepare and examine sequences (potentially even somebody could directly call some internal BioseqDB functions as standalone alignment library). This created a design challenge: same sequence can arrive as relationship locator, region in memory pool in RAM, can be provided as a whole as standrad container or as only a fragment (if user wants to operate only on specific slice or if it does not fit in RAM or RAM is limited, so we cannot fit entire sequence directly there), while these fragments should also be shareable, so parallel alignment to the same sequence doesn’t have to load them multiple times.

There are several existing libraries for interfacing with genomic sequences. SeqAn\[[125](../bibliography/#ref-REINERT2017157), [128](../bibliography/#ref-seqanManual)\] is a C++ library that provides modern interface for composing efficient sequence-analysis software from generic C++ components. Its library includes sequence representations, indexes and configurable alignment algorithms, with SIMD alignment, but it has it’s own limitations. However SeqAn SIMD algorithms are mostly limited to auto-vectoriztion provided by the compiler (which doesn’t have to be portable in most cases) and offers very narrow set of features for custom pipelines. I would be hard to directly reuse it for implementation of mentioned feautres. Using SeqAn would require deep integration, that’s why I decided to address the design challenges by designing own C++ system of templates with different specializations driven by C++ concepts (inspired by nice-looking HPC MPI code \[[3](../bibliography/#ref-uhl2026conceptspracticecmpi)\]).

> [!NOTE]
> **SeqAn compatibility**
>
> Because the architecture of BioseqDB pipeline is very custom, I decided to only use SeqAn’s alphabet layer \[[129](../bibliography/#ref-seqan3AlphabetsDocs)\] and utilities exposed by its public API. This means that libraries using SeqAn code as base, could be slightly adjusted to be able to work with BioseqDB pipelines.

Table [sequence representation specializations](../project_sequence_abstractions/#tab-sequence-representation-specializations) lists every specialization of all BioseqDB sequence representations, to showcase how they are used in different scenarios. SeqAn offers abstractions over lazy sequences as well \[[130](../bibliography/#ref-seqanSequencesInDepth)\], but they’re not as sophisticated as in my case. The lazy interface is really important for BioseqDB to improve it’s performance utilising late-materialization strategies similar to those described in literature \[[100](../bibliography/#ref-Chernishev2022ACS)\]

*Iterfaces for sequence representations in BioSeqDB.*<a id="tab-sequence-representation-specializations"></a>

<table>
<thead>
<tr>
<th style="text-align: left;"><strong>Template</strong></th>
<th style="text-align: left;"><strong>Specialization, and when it is selected</strong></th>
<th style="text-align: left;"><strong>Description and usage context</strong></th>
</tr>
</thead>
<tbody>
<tr>
<td style="text-align: left;"><p><code>compact_sequence_range&lt;R&gt;</code></p>
<p>This is the core type representing sequence that resides completely in memory. Template allows only four available specializations depending on what <code>R</code> is and each of them have entirely separate handling logic.</p></td>
<td style="text-align: left;"><code>R</code> is a container of alphabet values, such as <code>std::vector&lt;dna4&gt;</code></td>
<td style="text-align: left;">This is only adapter for existing sequence containers to provide compatibility with SeqAn and standard library containers. There is no alphabet packing and the type borrows <code>const R&amp;</code> Adapter just provides common sequence interface. Adapting one costs <span class="math inline"><em>O</em>(1)</span> as we proxy all operations to the underlying container</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>R</code> is a packing configuration <code>sequence_custom_alphabet&lt;A, U, b&gt;</code></td>
<td style="text-align: left;">Bitpacked vector that stores sequences according to alphabet rules passed as type paramter. Owns storage. Can convert to and from other bitpacked or not-bitpacked sequences, but if constructed from non-bitpacked sequence, then constructor costs <span class="math inline"><em>O</em>(<em>N</em>)</span></td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>R</code> is an SeqAn alphabet type, as in <code>compact_sequence_range&lt;dna4&gt;</code></td>
<td style="text-align: left;">The same as <code>sequence_custom_alphabet&lt;A, U, b&gt;</code>, but has sensible defaults for the underlying alphabet’s storage unit width etc. Commonly used in tests. Code interfacing directly with BioseqDB C++ API should use this specialization.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>R</code> is <code>compact_sequence_stdc_instance&lt;...&gt;</code></td>
<td style="text-align: left;">Holds only reference to the underlying object. Adapter to convert <code>compact_sequence_stdc_instance&lt;...&gt;</code> to <code>compact_sequence_range&lt;R&gt;</code>. <code>compact_sequence_stdc_instance&lt;...&gt;</code> is a type that comes from <code>compact_sequence_stdc&lt;...&gt;</code> - described below and used internally when passing sequences from Postgres</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>compact_sequence_stdc&lt;...&gt;</code></p>
<p>Represents standard layout C type [<a href="../bibliography/#ref-cppreference_standard_layout_type">2</a>] that is used to type-erase <code>compact_sequence_range&lt;R&gt;</code> types i.e allow user to have one type for storing possibly multiple different sequences types. Metadata like alphabet type and storage unit width are stored inside the struct and specialized template dispatch lambda is called to convert it back to the <code>compact_sequence_range&lt;R&gt;</code> type. This is heavily used by Postgres code to store sequences as C structures.</p></td>
<td style="text-align: left;"></td>
<td style="text-align: left;">Owns everything and stores fixed header that records struct (self) size in bytes (compatible with Postgres varlena), alphabet code, unit width, padding, and symbol count Used heavy by Postgres code as we cannot store template instances directly and need to hold them in one object that converts type parameters of templates to runtime values (with correct runtime dispatch). If you want to store sequences of different alphabets in one single container, you should use this.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>compact_sequence_stdc_instance&lt;Stdc, A, W&gt;</code>, internal type obtained when dispatching the type at runtime</td>
<td style="text-align: left;">This type carries no data and is used as a runtime tag (this is a common pattern to differentiate between template specialisations). It shouldn’t be used directly. One should receive <code>compact_sequence_range&lt;compact_sequence_stdc_instance&lt;...&gt;&gt;</code> when using dispatch methods of the stdc template</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>lazy_sequence&lt;R, Scanner&gt;</code></p>
<p>Represents sequence that can be loaded in fragments by some storage provider represented by type parameter <code>Scanner</code></p></td>
<td style="text-align: left;"><code>Scanner</code> has static constexpr method <code>is_fragmented()</code> that returns false (this means that Scanner always holds entire sequence in memory and there’s only one fragment). This is used as compatibility layer so you can directly create lazy sequence from compact sequence range instance. I have <code>resident_sequence_scanner&lt;U&gt;</code> for trivial implementation of non-fragmented scanners so you can write <code>lazy_sequence&lt;R, resident_sequence_scanner&lt;R&gt;&gt;</code> and don’t have to implement scanner type on your own.</td>
<td style="text-align: left;">Holds a single (borrowed) pointer to <code>compact_sequence_range&lt;R&gt;</code> Used as adapter to convert compact sequence range to lazy sequence. All primary pipeline operations should accept it as parameter</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>Scanner</code> is fragmented (<code>is_fragmented()</code> returns true)</td>
<td style="text-align: left;">Owns a scanner and a small internal cache for sequence fragments (which are neither copyable nor movable). Allows iteration over data source of sequences that doesn’t fit into memory at once. This is primary specialisation for loading sequences from PostgreSQL relations. The Postgres side implements large scanner type that tells the lazy sequence template how to fetch blocks of sequences</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>bit_casted_view&lt;Base, Out&gt;</code></p>
<p>Only legal for few types of alphabets and storage widths.</p></td>
<td style="text-align: left;">Special non-owning type that is used to provide zero-cost casts for bit-packed sequences. It allows to cast bitpacked sequences to different storage widths i.e cast sequence encoded on 32-bit int to be read as sequence encoded on 64-bit or 16-bit or 8-bit units. There are special requirements for alphabet to be able to do that (discussed below).</td>
<td style="text-align: left;">Holds only reference to underlying compact sequence range This is adapter type that provides zero-cost conversion between different specialisations of compact sequence range. Used for example by SIMD algorithms to have specific vector lane widths</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>lazy_sequence_view&lt;A&gt;</code></p>
<p>Facade over every representation listed above that is typed-erased i.e it is templated on the alphabet only, so Postgres can directly accept the concrete type instead of template. It consists of two pointers: opaque backend state and a static <code>backend_ops</code> table that holds <code>bp_size</code>, four orientation-specific <code>read_at</code> slots (read forward or backward, write forward or backward), <code>try_resident_packed</code>, <code>copy_packed_sequence</code> and <code>worker_shareable</code>. Specialization here means which callback table is installed.</p></td>
<td style="text-align: left;"><code>owned_sequence_adapter&lt;R, Scanner&gt;</code>, created by <code>make_lazy_sequence_view</code> for a concrete <code>lazy_sequence&lt;R, Scanner&gt;</code></td>
<td style="text-align: left;">Complete table over an owning lazy sequence. This is the table behind every lazy sequence that pipeline code receives, where Postgres provides pointers to actual functions.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>compact_range_adapter&lt;R&gt;</code>, created by <code>make_lazy_sequence_view</code> for <code>compact_sequence_range&lt;R&gt;</code></td>
<td style="text-align: left;">Used as convenient adapter to convert compact sequence range to lazy sequence view without worrying about scanner type and ownership. Useful for testing.</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>facade_adapter&lt;A&gt;</code>, created by <code>make_pg_sequence_call_owner</code> on the Postgres side</td>
<td style="text-align: left;">This struct holds Postgres Scanner state - it is either a detoasted flat value or a piece tree root together with its read session, because sequences need to be represented as trees of slice operations</td>
</tr>
<tr>
<td style="text-align: left;"></td>
<td style="text-align: left;"><code>resident_adapter&lt;A&gt;</code>, created by <code>admitted_packed_sequence::view()</code></td>
<td style="text-align: left;">Postgres Scanner for sequence only holds descriptor for already materialized sequence</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>retained_lazy_sequence_view&lt;A&gt;</code></p>
<p>This type has only one form. It’s a struct that has 3 fields: the view mentioned above, an opaque lifetime pointer to some data and a destructor callback pointer. It’s used in Postgres code for management of sequence lifecycle during DB operations</p></td>
<td style="text-align: left;">Constructed from a view, an opaque lifetime pointer and a <code>noexcept</code> release callback.</td>
<td style="text-align: left;">Used wherever a view has to outlive the expression that produced it. The Postgres read session allocates the owner in its own memory context and registers it there, so the destructor callback can be called even if Postgres fails (Postgres performs <code>longjmp</code> and skips C++ destructors). This means that this type is specific version of <code>std::unique_ptr</code> that allows us to provide custom lifecycle management during scans.</td>
</tr>
<tr>
<td style="text-align: left;"><p><code>admitted_packed_sequence&lt;A&gt;</code></p>
<p>Supporting type to propagate sequence along with memory permit (memory permits are used in BioseqDB to manage large memory regions for sequences)</p></td>
<td style="text-align: left;">Created during materialization by <code>materialize_packed_sequence</code>. In BioseqDB there’s a little sequence access planner that is responsible for deciding if we can materialize it all at once and directly or it needs to be streamed. <code>plan_sequence_access</code> picks this type when a contiguous resident region and we cannot use existing materialization (for example we access specific nucleotide that doesn’t align well with storage unit boundary, but the GPU algorithm requires continous region of sequence, so we need to copy it)</td>
<td style="text-align: left;">The struct owns its memory permit, the aligned buffer and its descriptor. Their resources, including the accounted bytes, are released through the usual RAII. Its <code>view()</code> returns an ordinary view over <code>resident_adapter&lt;A&gt;</code>, which is how SIMD and GPU kernels obtain contiguous packed input from a sequence that is tree-backed or fragmented</td>
</tr>
</tbody>
</table>

## Packed symbols and a common C++ interface

<a id="sec-compact-sequence-range"></a> <a id="sec-sequence-bit-layout"></a>

The template-driven design allows usage of any arbitrary alphabet for sequences, though for now I only support DNA4 and DNA5.

Sequence is encoded using bitpacking - the symbols are converted into integers values with specified fixed number of bits per symbol, then several symbols are packed into one single storage unit. Storage unit can be integer of any physical size. This is generalisation of bitpacking strategies commonly used in other software like packed-seq \[[142](../bibliography/#ref-packed_seq_library)\] Rust library developed by author of SIMD minimizers paper. packed-seq Rust library offers non-alphabet-generic packing \[[143](../bibliography/#ref-packed_seq_docs)\]. My design was more inspired by SeqAn approach where packing container is available, but very limited in features \[[128](../bibliography/#ref-seqanManual)\].

SeqAn alphabet assigns each symbol an integer called its *rank*. For DNA4, the four ranks are $A=0$, $C=1$, $G=2$, and $T=3$. A rank is an encoding of a symbol. Each `compact_sequence_range<R>` type needs to know certain properties of the encoding that are held (using CRTP) in `compact_sequence_range<R>::traits` type. In my storage symbols never spill to other storage units i.e to retrieve some symbol you always need to access at most one storage unit. The symbols in the storage unit are always packed in "little-endian" order i.e the first symbol is in the lowest bits of the storage unit and the last symbol is in the highest bits. The properties in traits type are either types or constexpr static expressions/functions:

- `alphabet_type` - type of the alphabet

- `storage_unit` - type of the underlying storage unit (usually `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`)

- `alphabet_size` - number of symbols in the alphabet

- `unit_width` - width in bits of the storage unit: $\text{sizeof(storage\_unit)} \times 8$

- `bits_per_char` - stored bits per symbol. The default packed representation uses $\left\lceil\log_2(\text{alphabet\_size})\right\rceil$, giving two bits for DNA4 and three for DNA5.

- `letters_per_unit` - number of symbols that can be packed into one storage unit: $\left\lfloor\frac{\text{unit\_width}}{\text{bits\_per\_char}}\right\rfloor$

- `unit_used_bits` - number of bits used in the storage unit (for alphabet sizes that are not a power of two, some bits must stay unassigned): $\text{letters\_per\_unit} \times \text{bits\_per\_char}$

- `unit_leftover_bits` - number of bits leftover in the storage unit: $\text{unit\_width} - \text{unit\_used\_bits}$

- `char_mask` - mask for the last symbol in the storage unit

- `rev_char_mask` - mask for the first symbol in the storage unit

- `used_storage_mask` - mask for the used bits in the storage unit

- Conversion functions for converting between storage unit and symbol: `letter_to_symbol`, `letter_to_char`, `letter_complement`

- Utilities for generating k-mer masks: `kmer_mask`

The figure below [lsb packed layout](../project_sequence_abstractions/#fig-lsb-packed-layout) shows an example of how I bitpack DNA4 sequence in storage units.

![01 values and ranges diagram](/static/figures/project/chapters-09-sequence-abstractions-01-values-and-ranges-tikz-01.svg)

*Packing `ACGTGA` with four DNA4 symbols per byte. The dashed slots are padding, which occurs at the end of the sequence as rarely sequence size is multiple of `compact_sequence_range<R>::traits::letters_per_unit` so they are not part of the sequence.*

<a id="fig-lsb-packed-layout"></a>

> [!WARNING]
> **Template instance explosion**
>
> In BioseqDB I tried to allow user to specify any storage width that is power of two (8, 16, 32, 64) and alphabet (currently supported is DNA4 and DNA5). Because both of parameters are constexpr template parameters, this leads in some parts to creation of hundreds separate versions of pipeline. This slows downs compilation times and led to creation of absurdly large BMIs resulting in multi-gigabyte Clang files. Resulting binary still is optimised down to more convenient size, however I put lots of burden onto the compiler and linker. The goal of user-supplied storage widths was to observe how they affect performance and if they affect it at all. Currently in some parts of the pipeline I still cast the stored types to , this happens because some algorithms like SIMD chaining require larger widths to store indices and scores. Extracting symbols directly from storage unit is a complex process involving lots of bitwise operations, so at this time most algorithms (SIMD/GPU) cannot operate directly on packed sequences as this would require lots of additional engineering effort. I added conversion helpers like one presented in the listing below [compact range interface](../project_sequence_abstractions/#lst-compact-range-interface) that convert requested ranges of sequence into non-bitpacked sequence that is easier to work with. This operation is done on demand and stored in separate memory pool region, so pipeline doesn’t have to convert sequence from bitpacked to non-bitpacked format multiple times. Directly unpacking sequence inside pipeline at the latest point in time is potential opportunity for future improvements.

The following example uses actual code snippet from BioseqDB - in this case the caller owns `output`, which has at least $last-first$ bytes, and passes bounds satisfying $0\leq first\leq last\leq N$.

<a id="lst-compact-range-interface"></a>

```cpp
template<supports_compact_sequence_range R>
void read_fragment(const compact_sequence_range<R>& sequence,
                   std::size_t first, std::size_t last,
                   std::span<std::uint8_t> output) {
    // One rank per output byte, in forward order.
    sequence.template to_encoded_into<false>(output, first, last);
}
```

A consumer requests only the ranks it needs.

## Slices keep logical coordinates

<a id="sec-sequence-sql-slicing"></a>

Postgres support array subscription operations natively, so I decided to support slicing as well in first version of BioseqDB.

A slice selects positions $[a,b)$: it includes $a$ and excludes $b$. BioSeqDB uses zero-based positions, so $S[0]$ is the first symbol of the slice.

> [!WARNING]
> **0-based indexing**
>
> This contradicts the fact that Postgres slices are 1-based, but it wasn’t intentional decision. I had limited experience with Postgres array functionality and this realisation came only after most of the support was already implemented. What is more interesting that SeqKit - tool for operating on FASTA/Q files use 1-based indexing \[[148](../bibliography/#ref-seqkit_subseq_usage)\]. This incompatibility might be something to reconsider in the future.

I provided utility functions for common operations like reversing and reverse-complementing slices as well, example available in listing [sequence sql examples](../project_sequence_abstractions/#lst-sequence-sql-examples).

<a id="lst-sequence-sql-examples"></a>

```sql runnable id=project_lst-sequence-sql-examples timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
-- bioseq:visible
WITH v(s) AS (
  VALUES ('ACGTAACCGTTA'::bioseq.seq('dna4', 32))     -- ACGTAACCGTTA
)
SELECT s[2:10]::text AS middle,                       -- GTAACCGT
       ((s[2:10])[1:5])::text AS nested,              -- TAAC
       s[-4:]::text AS suffix,                        -- GTTA
       bioseq.reverse(s[2:10])::text AS rev,          -- TGCCAATG
       bioseq.reverse_complement(s[2:10])::text AS rc -- ACGGTTAC
FROM v;
-- bioseq:teardown
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Zero-based slices and sequence transforms in SQL.

The bounds follow a small set of intuitive rules simiar to design of slices in Python \[[147](../bibliography/#ref-python_common_sequence_operations)\]:

- A missing lower bound means zero. A missing upper bound means the sequence length.

- A negative bound counts from the end (example: for length 12, $-4$ means 8) and bounds outside the sequence are clamped to its ends.

- An upper bound below the lower bound gives an empty slice, so `s[4:2]` has length zero.

- User can extract any element (symbol) from slice, which has separate SQL type `LETTER`. An out-of-range element, such as `s[12]` in this example, gives SQL `NULL`. An explicit null slice bound also gives `NULL` i.e $s[a:\text{NULL}]$ is equivalent to $s[\text{NULL}:a]$ and is also equivalent to $s[\text{NULL}:\text{NULL}]$ (all of them result in `NULL`)

Slicing can be implemented by just holding slice coordinates, without actually loading any sequences, using very simple coordinate arithmetic shown in the figure below [sequence slice coordinates](../project_sequence_abstractions/#fig-sequence-slice-coordinates). For a slice view $V=S[a,b)$, view position $j$ refers to $S[a+j]$. A second slice can combine its offsets directly to get: $$V[c,d)=S[a+c,a+d).$$ The example’s $S[2,10)[1,5)$ is $S[3,7)$, which reads `TAAC`. The reversal operation is represented by reversing the start and end coordinates. For $W=\operatorname{reverse}(S[a,b))$ we have: $$W[j]=S[b-1-j],\qquad
  W[c,d)=\operatorname{reverse}(S[b-d,b-c)).$$ The complementing operation leaves the coordinates unchanged, but it changes the binary flag representing whether the slice is complemented or not.

![02 slice coordinates diagram](/static/figures/project/chapters-09-sequence-abstractions-02-slice-coordinates-tikz-01.svg)

*Blue cells form the first slice and orange cells show a second slice*

<a id="fig-sequence-slice-coordinates"></a>

## Composing sequences without joining their payloads

<a id="sec-sequence-piece-tree"></a>

The slice model I showed is very primitive. If we add ability to concatenate sequences we can represent any slice as a tree of pieces that represent operations on original sequence. In BioseqDB this tree refers to Postgres operators and datums.

The slice tree is constructed from the following elements:

- A **source leaf** describes an interval in the source sequence.

- A **literal leaf** holds a literal value referencing a Postgres datum with additional metadata.

- A **branch** node represents the concatenation of two children. It holds the total length, piece count (total number of leaves in the subtree) and tree height. We collect the total length to estimate how long the memory region in the pool should be to accommodate the materialized sequence.

- An **edge** connects a node to its child. It holds reverse and complement flags.

Figure [piece tree](../project_sequence_abstractions/#fig-piece-tree) illustrates concatenation of three pieces including two source leaves and one literal leaf: $$Z=S[2,6)\ \Vert\ \operatorname{reverse}(S[8,12))\ \Vert\ \verb|AC|,$$ The result is `GTAAATTGAC`, with length $4+4+2=10$.

![03 piece trees diagram](/static/figures/project/chapters-09-sequence-abstractions-03-piece-trees-tikz-01.svg)

*A piece tree describes how BioseqDB process slices of a sequence.*

<a id="fig-piece-tree"></a>

Each of the operations can easily be implemented by traversing the tree: concatenation joins roots and restores tree balance, slicing splits at the two bounds and leaves the middle part, reversing just as complementation flipps flags on the edges.

Reading actual slice content in BioseqDB is implemented using `lazy_sequence_view<A>` of table [sequence representation specializations](../project_sequence_abstractions/#tab-sequence-representation-specializations). It’s built for the duration of a single call and the tree operations read only metadata from sequence locators (such as lengths). This means that we don’t need to fetch large relations with sequence content and slicing is very performant. In the pipeline specific stages (like SIMD or GPU kernels) can request contiguous region of sequence and BioseqDB will materialize it into its own packed buffer first.

I implemented concatenation support for SQL text literals as well. This means that we can concatenate sequences with text directly without casting them, but this has it’s own limitations:

- Slicing and reversal can operate on text without no known alphabet because they need only positions and order.

- Joining a known-alphabet sequence with a text literal validates the literal against that alphabet, so we require to go through entire literal (which shouldn’t be a problem as we don’t expect very long literals as this should be done via loading FASTA first into the table)

- Reverse-complement requires known alphabet with a complement rule for every symbol in that alphabet.

- Two different alphabets are not converted automatically to one another and this will result in an error.

## Reading only what the algorithm requires

<a id="sec-lazy-sequence-facade"></a> <a id="sec-sequence-materialization"></a>

The storage can be provided in different forms as explained previously in the first section [sequence abstractions](../project_sequence_abstractions/#sec-sequence-abstractions) In BioseqDB main implementation of Postgres-based storage is done via `lazy_sequence_view<A>` and `retained_lazy_sequence_view<A>` mentioned in table [sequence representation specializations](../project_sequence_abstractions/#tab-sequence-representation-specializations). These types holds pointers to functions that implement the storage and represent the following operations:

- Report sizes of sequences

- Copy selected intervals into caller-owned memory, which in this case can be GPU-owned memory in GPU-accelerated pipeline stages

- Reading successive chunks

- Checking if the compatible interval was already materialized and can be borrowed (caching)

Internally I distinguish several access policies which are specified by the pipeline stage, they are the following choices:

1.  **Stream.** If the algorithm accepts a stream, read the selected interval in bounded chunks and save it to reusable scratch memory (large memory pool)

2.  **Borrow.** If the algorithm needs packed data, first check if’s already in cache and safe to borrow.

3.  **Pack.** In case if borrowing is not possible, algorithm needs to request new memory permit (this is routed through custom pool allocator). Caller is then responsible for this memory chunk.

![04 reading intervals diagram](/static/figures/project/chapters-09-sequence-abstractions-04-reading-intervals-tikz-01.svg)

*Simplified view of different access policies*

<a id="fig-sequence-abstraction-layers"></a>

> [!NOTE]
> **Borrowing**
>
> Borrowing can fail if the region for example starts in the middle of storage unit in packed sequence, then we cannot reuse that memory chunk as it’s misaligned

### Copying ranks from a PostgreSQL sequence

<a id="sec-sequence-pg-rank-copy"></a>

The PostgreSQL source-provider view keeps its scanner and packed fragment slots in the read session. A rank request visits the requested symbols, looks up each symbol’s fragment in that scanner and unpacks its rank. Repeated small requests can reuse packed fragments, but still repeat the callback and symbol-extraction work; they need not read the fragment again from disk or decode the whole fragment into ranks. Retaining a packed target for the alignment stage avoids these source-provider calls, although an algorithm requesting byte ranks must still unpack the selected interval.

![04 reading intervals diagram](/static/figures/project/chapters-09-sequence-abstractions-04-reading-intervals-tikz-02.svg)

*Illustration of structures used and operations performed on sequences during scan lifecycle*

<a id="fig-sequence-scan-lifecycle"></a>

At this point I need to describe lifecycles of various entities to illustrate how reading is performed. Figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle) shows how what is happening under the hood during the scan:

- (a) Each row of a batch gets its own scanner with 16 fragment slots for caching. A miss looks in the decoded chunk cache (this is lower-layer cache for sequences chunks which may store multiple unrelated sequences and is discussed in later chapters), then in the columnar relation, and copies one fragment into a slot.

- (b) The incoming queries are grouped in batches. Each batch has it’s own session, which is destroyed when batch processing is finished. The session is different entity to the chunk cache so relation isn’t reopen when batch is finished and we keep large sequence chunks still in memory. At the start of the batch the backend first builds one packed copy in memory and distributes that copy to workers from the pool. It’s important that in this architecture we only have one backend thread which is responsible for reading relation. This means that the actual pipeline is run by single thread that submits various work to worker pool. I needed to read the relation on a single thread to avoid race conditions on PostgreSQL’s process-local state which is not thread-safe - PostgreSQL starts (forks) a new server process for each client connection, and even parallel SQL starts extra background worker processes rather than extra threads inside the same backend \[[121](../bibliography/#ref-postgresql17_tutorial_arch), [122](../bibliography/#ref-postgresql17_how_parallel_query)\]. The same read could still be done in parallel by giving each reader its own backend: one way would be to fork a background worker with a database connection (which in this context is problematic as forking with pool running will result in segfaults) or open a second client session through libpq (libpq is thread-safe when each thread uses its own connection) \[[123](../bibliography/#ref-postgresql17_bgworker), [124](../bibliography/#ref-postgresql17_libpq_threading)\]. I did not take those paths for fragment reads during a query as it would complicate the implementation and this is something to be done in the future.

- (c) A view is built from two pointers - `state` is a piece tree together with the session that holds one scanner per source row and `ops` contains length, two flags and two function pointers. Providing function pointer serves as abstraction layer to divide non-Postgres code with actual Postgres implementation. Using templated types could be possible here, but they still would need type erasure. A retained view additionally holds a release callback (C-style destructor)

- (d) At every batch end, in this order: (1) release retained views, (2) finish each scanner so its slot copies go, (3) drop the session’s memory. The worker pool and the snapshot naturally outlive the batch as discussed here. It might happen that we encounter an error and failing query skips C++ destructors, so steps 1–3 also run from a memory context callback (retained view release callback) <a id="lst-sequence-scan-lifecycle-steps"></a>

Panel (d) of Figure [sequence scan lifecycle](../project_sequence_abstractions/#fig-sequence-scan-lifecycle) has a gap between the end of batch 1 and the start of batch 2. In that gap the backend collects the next tuples from the relation (metadata and sequence locators, not entire sequences), applies the query’s `WHERE` clauses, and places surviving tuples in the dispatch buffer. When enough tuples are collected, it submits the next batch. That collection still uses one relation-reading thread per PostgreSQL process. There’s however support for `bioseq.scheduler.postgres_parallel.enabled` for any query that do not use alignment. The reasoning behind that is that only the thread pool wasn’t designed around being multi-process safe and current statistics collection doesn’t support it. The scans that do not use alignment are marked parallel-safe. This means that example shown on lsiting below [simple table read with slice](../project_sequence_abstractions/#code-simple-table-read-with-slice) can be executed by postgres parallel scan.

Scans in details are covered in the next chapter [postgres custom scan scope](../project_postgres_custom_scan/#sec-postgres-custom-scan-scope) in detail.

<a id="code-simple-table-read-with-slice"></a>

```sql runnable id=project_code-simple-table-read-with-slice timeout=30s
-- bioseq:setup
SET max_parallel_workers_per_gather = 0;
SET bioseq.index.insert_maintenance.enabled = off;
CREATE TABLE t (id integer, s bioseq.seq('dna4', 64)) USING bioseq;
INSERT INTO t VALUES (1, 'ACGTAACCGTTA');
-- bioseq:visible
SELECT id, bioseq.to_text(s[0:8]) FROM t;
-- bioseq:teardown
DROP TABLE t;
RESET bioseq.index.insert_maintenance.enabled;
RESET max_parallel_workers_per_gather;
```

Simple table read

> [!WARNING]
> **Parallel scan**
>
> This is potential optimisation in the future. Statistics collection can be easily made multi-process safe by implementing reduce function, while thread pool design needs to be revisited and reimplemented.
