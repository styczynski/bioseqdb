/*-------------------------------------------------------------------------
 *
 * sidecar.cppm
 *
 * Glue module for per-sequence minimizer/metadata sidecar tables
 * bioseq_internal.index_sidecar and bioseq_internal.meta_sidecar.
 *
 * Responsibilities:
 *   - Lazy OID resolution for both sidecar relations (cached per backend).
 *   - INSERT path: compute + serialize TargetIndex, write metadata count.
 *   - SCAN path: fetch serialized blobs via a btree index scan on the
 *     (user_relid, sequence_rowno) PK.
 *
 * The module intentionally does NOT wrap in HWY_NAMESPACE: no Highway
 * primitives are used directly here. The SIMD-sensitive pieces
 * (sketch_collector, collect_minimizers, TargetIndex) live inside their
 * own HWY-wrapped modules (bioseq_sequences.kmers, bioseq_sequences.minimap_port)
 * and are reached via imported names at the one dispatch site.
 *
 * See RESEARCH/SPEC_MINIMIZER_STORAGE.md for the full design.
 *-------------------------------------------------------------------------
 */
module;

#include <hwy/highway.h>   /* needed only for the HWY_NAMESPACE macro so we
                            * can name the bioseq::simd::HWY_NAMESPACE
                            * namespace where TargetIndex et al. live. We
                            * do NOT wrap this module in HWY_*_NAMESPACE —
                            * no Highway primitives are used in sidecar.cppm. */
#include "common.h"

#include "columnar/columnar.h"

extern "C" {
#include "utils/inval.h"
#include "utils/array.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "storage/lmgr.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/heapam.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

export module bioseq_sidecar;

import mlogging;
import pg_utils;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_sequences.minimap_port;
import bioseq_types.pg_compact_sequence;
import sequan_compat;
import bioseq_metadata_admin;

/* Pull TargetIndex / sketch_collector / collect_minimizers / minimap_int_hasher
 * / pgseq_t / compact_sequence_range out of their home namespaces so the
 * compute path below reads clean. The minimap module places all of these
 * inside bioseq::simd::HWY_NAMESPACE; on ARM64 HWY_NAMESPACE resolves to
 * N_NEON. */
using bioseq::simd::HWY_NAMESPACE::TargetIndex;
using bioseq::simd::HWY_NAMESPACE::sketch_collector;
using bioseq::simd::HWY_NAMESPACE::collect_minimizers;
using bioseq::simd::HWY_NAMESPACE::minimap_int_hasher;
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

/* Forward declarations for the two bioseq_table symbols we need at plan-
 * time for estimate_index_presence's bioseq-AM row-count fallback.
 * `extern "C++"` attaches the declarations to the global module fragment
 * instead of `bioseq_sidecar`, so the linker matches the corresponding
 * `extern "C++"` definitions in backend/table/access_method.cppm. Keeping
 * the declarations out of an `import bioseq_table` preserves the current
 * module graph (bioseq_sidecar is imported BY bioseq_table — reversing
 * the edge would cause a circular import). */
extern "C++" uint64_t ColumnarTableRowCount(Relation relation);
extern "C++" bool     IsColumnarTableAmTable(Oid relationId);
/* tid_to_row_number is the inverse of row_number_to_tid (defined in
 * backend/table/access_method.cppm). Used by recompute_per_row_*_for_relid
 * below to recover the writtenRowNumber assigned by ColumnarWriteRow at
 * INSERT time. Declared via extern "C++" for the same reason as the two
 * symbols above (avoids a circular import on bioseq_table). */
extern "C++" uint64_t tid_to_row_number(ItemPointerData tid);
/* pgseq_t and compact_sequence_range live in the root namespace (the
 * pg_compact_sequence and bioseq_sequences modules don't wrap in
 * HWY_BEFORE_NAMESPACE). They are reachable directly. */

namespace bioseq::sidecar {

/* ---------------------------------------------------------------- */
/* OID resolution                                                   */
/* ---------------------------------------------------------------- */

struct SidecarOids {
	Oid index_sidecar_relid = InvalidOid;
	Oid meta_sidecar_relid  = InvalidOid;
	Oid bioseq_seq_type_oid = InvalidOid;
	bool initialised        = false;
};

static SidecarOids cached_oids{};

static void invalidate_sidecar_oids(Datum /*arg*/, Oid /*relid*/) {
	cached_oids.initialised = false;
}

[[nodiscard]] static Oid resolve_relid_by_name(const char* qualified) {
	return DatumGetObjectId(DirectFunctionCall1(regclassin,
		CStringGetDatum(qualified)));
}

[[nodiscard]] static Oid resolve_type_oid(const char* name) {
	return DatumGetObjectId(DirectFunctionCall1(regtypein,
		CStringGetDatum(name)));
}

[[nodiscard]] static const SidecarOids& get_sidecar_oids() {
	if (cached_oids.initialised) {
		return cached_oids;
	}
	static bool invalidation_registered = false;
	if (!invalidation_registered) {
		/* Invalidate the cache whenever any relcache entry is changed. This
		 * is overly broad but cheap: the cost is just a re-resolution on the
		 * next sidecar use, which already goes through regclassin (syscache
		 * backed). */
		CacheRegisterRelcacheCallback(invalidate_sidecar_oids,
									  PointerGetDatum(NULL));
		invalidation_registered = true;
	}
	PG_TRY();
	{
		cached_oids.index_sidecar_relid =
			resolve_relid_by_name("bioseq_internal.index_sidecar");
		cached_oids.meta_sidecar_relid  =
			resolve_relid_by_name("bioseq_internal.meta_sidecar");
		cached_oids.bioseq_seq_type_oid =
			resolve_type_oid("bioseq.seq");
		cached_oids.initialised = true;
		JustLog("[bioseq] sidecar: resolved index_sidecar={} meta_sidecar={} bioseq.seq={}",
				static_cast<unsigned>(cached_oids.index_sidecar_relid),
				static_cast<unsigned>(cached_oids.meta_sidecar_relid),
				static_cast<unsigned>(cached_oids.bioseq_seq_type_oid));
	}
	PG_CATCH();
	{
		cached_oids = SidecarOids{};
		PG_RE_THROW();
	}
	PG_END_TRY();
	return cached_oids;
}

/* ---------------------------------------------------------------- */
/* Write helpers                                                    */
/* ---------------------------------------------------------------- */

/* Insert one row into a heap table. Does a fresh relation_open per call;
 * good enough for single-tuple INSERT, but bioseq_multi_insert callers
 * should prefer insert_heap_row_batch (TODO follow-up). */
static void insert_heap_row(Oid relid, int num_attrs, Datum* values, const bool* nulls) {
	Relation rel = try_safe_relation_open(relid, RowExclusiveLock);
	if (rel == nullptr) {
		JustLog("[bioseq] sidecar: skipping insert; relation oid={} is not open-able",
				static_cast<unsigned>(relid));
		return;
	}

	TupleDesc td = RelationGetDescr(rel);
	if (td->natts != num_attrs) {
		JustLog("[bioseq] sidecar: tupdesc natts={} expected={}; skipping insert",
				td->natts, num_attrs);
		relation_close(rel, RowExclusiveLock);
		return;
	}

	HeapTuple tup = heap_form_tuple(td, values, const_cast<bool*>(nulls));
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	relation_close(rel, RowExclusiveLock);
}

/* Build a Postgres INT8[] array from a contiguous run of int64 counts.
 * The returned ArrayType* lives in CurrentMemoryContext and is 1-indexed
 * (the stored Postgres array is accessed as symbol_counts[1..N]).
 *
 * construct_array() only reads the Datum array — it copies bytes into the
 * ArrayType — so a stack-local buffer is safe and avoids one palloc + one
 * pfree per INSERT. Capacity is bounded by the largest supported alphabet
 * (dna5 = 5); kMaxAlphabetSize in sidecar_compute_and_write_meta() caps
 * at 8, which we mirror here. */
[[nodiscard]] static ArrayType* make_int8_array(const int64_t* counts, int n) {
	constexpr int kStackBufferSlots = 8;
	if (n > kStackBufferSlots) {
		/* Fall back to palloc for unexpectedly large alphabets. */
		Datum* datums = static_cast<Datum*>(palloc(sizeof(Datum) * n));
		for (int i = 0; i < n; ++i) {
			datums[i] = Int64GetDatum(counts[i]);
		}
		ArrayType* arr = construct_array(datums, n, INT8OID,
										/*elemlen=*/ 8,
										/*elembyval=*/ true,
										/*elemalign=*/ 'd');
		pfree(datums);
		return arr;
	}
	std::array<Datum, kStackBufferSlots> datums{};
	for (int i = 0; i < n; ++i) {
		datums[i] = Int64GetDatum(counts[i]);
	}
	return construct_array(datums.data(), n, INT8OID,
						   /*elemlen=*/ 8,
						   /*elembyval=*/ true,
						   /*elemalign=*/ 'd');
}

export void sidecar_write_meta(Oid user_relid, int64_t rowno,
							   int16_t alphabet_code,
							   const int64_t* counts, int num_counts)
{
	const SidecarOids& oids = get_sidecar_oids();
	if (oids.meta_sidecar_relid == InvalidOid) {
		return;
	}
	ArrayType* arr = make_int8_array(counts, num_counts);

	Datum values[4];
	bool  nulls[4]  = { false, false, false, false };
	values[0] = ObjectIdGetDatum(user_relid);
	values[1] = Int64GetDatum(rowno);
	values[2] = Int16GetDatum(alphabet_code);
	values[3] = PointerGetDatum(arr);
	insert_heap_row(oids.meta_sidecar_relid, 4, values, nulls);
	JustLog("[bioseq] sidecar: wrote meta row (relid={}, rowno={}, alphabet={}, n_counts={})",
			static_cast<unsigned>(user_relid), rowno,
			static_cast<int>(alphabet_code), num_counts);

	pfree(arr);
}

/* Fused compute + serialize + insert for the minimizer index. The intermediate
 * std::vector<char> that the previous revision used has been removed: we
 * allocate a single varlena of exact size and serialize straight into
 * VARDATA(ptr), saving one malloc + one memcpy per INSERT. */
export void sidecar_compute_and_write_index(Oid user_relid, int64_t rowno,
											const pgseq_t* hdr)
{
	if (hdr == nullptr) {
		return;
	}
	const SidecarOids& oids = get_sidecar_oids();
	if (oids.index_sidecar_relid == InvalidOid) {
		return;
	}

	/* Build the TargetIndex first (inside the alphabet dispatch); after the
	 * lambda returns we palloc and serialize in one go. The TargetIndex owns
	 * std::unordered_map / std::vector buckets that outlive the dispatch
	 * so the lambda must return without consuming it, hence we store the
	 * idx and its byte count in captured locals. */
	TargetIndex idx;
	std::size_t blob_bytes = 0;

	pgseq_t::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width,
		[&]<class S>() {
			compact_sequence_range<S> seq{*hdr};

			sketch_collector collector;
			collect_minimizers<minimap_int_hasher, MinimapSketchData>(
				collector, seq, seq.begin(), seq.end(),
				/*w=*/10, /*k=*/15);
			// Move the vector — collector dies at lambda exit.
			auto minimizers = std::move(collector).extract_sketches_data();

			idx.build(minimizers, static_cast<int32_t>(seq.bp_size()));
			blob_bytes = idx.serialize_binary_size();
		});

	bytea* target_index_varlena = reinterpret_cast<bytea*>(palloc(VARHDRSZ + blob_bytes));
	SET_VARSIZE(target_index_varlena, VARHDRSZ + blob_bytes);
	if (blob_bytes > 0) {
		idx.serialize_to_binary(VARDATA(target_index_varlena));
	}

	Datum values[3];
	bool  nulls[3]  = { false, false, false };
	values[0] = ObjectIdGetDatum(user_relid);
	values[1] = Int64GetDatum(rowno);
	values[2] = PointerGetDatum(target_index_varlena);
	insert_heap_row(oids.index_sidecar_relid, 3, values, nulls);
	JustLog("[bioseq] sidecar: wrote index row (relid={}, rowno={}, blob_bytes={})",
			static_cast<unsigned>(user_relid), rowno, blob_bytes);

	pfree(target_index_varlena);
}

/* Compute the per-symbol count array by calling count_symbols() on the
 * compact_sequence_range and forward the result to sidecar_write_meta.
 *
 * The maximum alphabet size in the extension is 5 (dna5); we size a small
 * stack buffer to hold the counts plus a couple of spares so the function
 * never needs to heap-allocate here. */
export void sidecar_compute_and_write_meta(Oid user_relid, int64_t rowno,
										   const pgseq_t* hdr)
{
	if (hdr == nullptr) {
		return;
	}

	constexpr int kMaxAlphabetSize = 8;
	int64_t counts[kMaxAlphabetSize] = { 0 };
	int num_counts = 0;

	pgseq_t::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width,
		[&]<class S>() {
			compact_sequence_range<S> seq{*hdr};
			auto tally = seq.count_symbols();
			num_counts = static_cast<int>(tally.size());
			for (int i = 0; i < num_counts && i < kMaxAlphabetSize; ++i) {
				counts[i] = static_cast<int64_t>(tally[i].first);
			}
		});

	sidecar_write_meta(user_relid, rowno,
					   static_cast<int16_t>(hdr->alphabet_code),
					   counts, num_counts);
}

/* Called from bioseq_tuple_insert / bioseq_multi_insert right after the
 * user row has been written. Always writes to meta_sidecar; conditionally
 * writes to index_sidecar based on the bioseq.update_index_on_insert GUC. */
export void sidecar_after_insert(Relation user_rel,
								 TupleTableSlot* slot,
								 Datum* detoasted_values,
								 uint64_t written_row_number)
{
	if (user_rel == nullptr || slot == nullptr) {
		return;
	}

	const SidecarOids& oids = get_sidecar_oids();
	const Oid rel_oid = RelationGetRelid(user_rel);
	if (rel_oid == oids.index_sidecar_relid || rel_oid == oids.meta_sidecar_relid) {
		return;          /* recursion guard — sidecars must not self-reference */
	}

	/* Coarse-granularity age-log update for the two PER_ROW kinds owned by
	 * this module. The helper is steady-state O(1) (one std::unordered_map
	 * lookup); the actual heap write fires only at the throttle boundary
	 * (~once per 60 s per (rel, kind) pair). Placed BEFORE the GUC early
	 * return so the age log keeps tracking even when the GUC is off — an
	 * operator who flips the GUC for a bulk load still sees the right
	 * "rel was last touched recently" signal in bioseq.list_metadata().
	 *
	 * The two literal kind names are the v1 set: both are PER_ROW with
	 * insert_support = true. A future PER_TABLE kind with
	 * insert_support = false would NOT want age-log updates from INSERTs;
	 * when that lands, replace these two calls with a registry-driven
	 * loop over MetadataKindRegistry::instance().kinds() that filters by
	 * `insert_support`. */
	bioseq::metadata_admin::age_log::note_per_insert(rel_oid, "minimizers");
	bioseq::metadata_admin::age_log::note_per_insert(rel_oid, "symbol_counts");

	/* Both sidecars are gated on bioseq.update_index_on_insert. When OFF,
	 * neither meta nor index sidecars are written; the scalar bioseq.
	 * symbol_count still returns correct results because it falls back to
	 * re-computing count_symbols() on the varlena directly, and the scan
	 * path will backfill on first read. */
	if (!bioseq_update_index_on_insert) {
		return;
	}

	TupleDesc td = slot->tts_tupleDescriptor;
	AttrNumber seq_attno = InvalidAttrNumber;
	for (int i = 0; i < td->natts; ++i) {
		Form_pg_attribute attr = TupleDescAttr(td, i);
		if (attr->atttypid == oids.bioseq_seq_type_oid) {
			seq_attno = static_cast<AttrNumber>(i + 1);
			break;
		}
	}
	if (seq_attno == InvalidAttrNumber) {
		return;          /* no bioseq.seq column; nothing to write */
	}
	if (slot->tts_isnull[seq_attno - 1]) {
		return;          /* NULL seq column; skip */
	}

	const pgseq_t* hdr = reinterpret_cast<const pgseq_t*>(
		DatumGetPointer(detoasted_values[seq_attno - 1]));

	/* Meta first: computing a symbol tally is cheap and gives us a consistent
	 * meta_sidecar row even if index-side computation fails later. */
	sidecar_compute_and_write_meta(rel_oid,
								   static_cast<int64_t>(written_row_number),
								   hdr);
	sidecar_compute_and_write_index(rel_oid,
									static_cast<int64_t>(written_row_number),
									hdr);
}

/* ---------------------------------------------------------------- */
/* Read helpers                                                     */
/* ---------------------------------------------------------------- */

/* find_primary_key_oid moved to pg_utils so metadata_admin.cppm can
 * share the same helper without introducing a circular module
 * dependency. The unqualified name resolves to the global ::find_primary_key_oid
 * via standard C++ name lookup. */

/* Read path: load one row (user_relid, rowno) -> serialized blob. Returns
 * empty optional when missing. Uses the PK btree via indexOK=true so the
 * lookup is O(log n) rather than a full heap scan. */
export std::optional<std::vector<char>> sidecar_read_index(Oid user_relid, int64_t rowno) {
	const SidecarOids& oids = get_sidecar_oids();
	if (oids.index_sidecar_relid == InvalidOid) {
		return std::nullopt;
	}

	Relation rel = try_safe_relation_open(oids.index_sidecar_relid, AccessShareLock);
	if (rel == nullptr) {
		return std::nullopt;
	}

	const Oid pk_index_oid = find_primary_key_oid(rel);
	if (pk_index_oid == InvalidOid) {
		JustLog("[bioseq] sidecar: index_sidecar has no primary-key btree; "
				"falling back to sequential scan (install is broken)");
	}

	ScanKeyData keys[2];
	ScanKeyInit(&keys[0], 1 /*user_relid*/,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(user_relid));
	ScanKeyInit(&keys[1], 2 /*sequence_rowno*/,
				BTEqualStrategyNumber, F_INT8EQ,
				Int64GetDatum(rowno));

	SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
										  /*indexOK=*/ pk_index_oid != InvalidOid,
										  NULL /*snapshot*/, 2, keys);

	std::optional<std::vector<char>> result;
	HeapTuple tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup)) {
		bool isnull = false;
		Datum blob_datum = heap_getattr(tup, 3 /*target_index*/,
										RelationGetDescr(rel), &isnull);
		if (!isnull) {
			bytea* flat = reinterpret_cast<bytea*>(PG_DETOAST_DATUM(blob_datum));
			const std::size_t data_size = VARSIZE_ANY_EXHDR(flat);
			std::vector<char> v(data_size);
			if (data_size > 0) {
				std::memcpy(v.data(), VARDATA_ANY(flat), data_size);
			}
			result = std::move(v);
			if (reinterpret_cast<void*>(flat) != reinterpret_cast<void*>(DatumGetPointer(blob_datum))) {
				pfree(flat);
			}
		}
	}
	systable_endscan(scan);
	relation_close(rel, AccessShareLock);
	return result;
}

/* Count the rows in the user relation. Strategy:
 *   1. If `estimated_total_rows` (pg_class.reltuples) is positive, trust
 *      the planner's stats.
 *   2. Otherwise, if the relation uses the bioseq columnar AM, fall back
 *      to ColumnarTableRowCount() which reads the stripe-metadata stats
 *      instead of doing a full scan.
 *   3. Otherwise, return -1.0 — caller decides how to handle (today we
 *      assume full coverage to avoid mis-estimating cost).
 *
 * The bioseq-AM fallback matters because our custom access method does
 * not implement a working ANALYZE at the moment — pg_class.reltuples
 * stays at the initial -1 sentinel until a user explicitly updates it.
 * The stripe-metadata count is exact and cheap to compute. */
[[nodiscard]] static double resolve_user_relation_tuples(
	Oid user_relid, double estimated_total_rows)
{
	if (estimated_total_rows > 0.0) {
		return estimated_total_rows;
	}
	if (!IsColumnarTableAmTable(user_relid)) {
		return -1.0;
	}
	Relation user_rel = try_safe_relation_open(user_relid, AccessShareLock);
	if (user_rel == nullptr) {
		return -1.0;
	}
	const uint64_t rows = ColumnarTableRowCount(user_rel);
	relation_close(user_rel, AccessShareLock);
	return static_cast<double>(rows);
}

/* Estimate what fraction of `user_relid`'s rows have a matching entry in
 * bioseq_internal.index_sidecar. Returns a float in [0.0, 1.0]:
 *
 *   0.0 → no minimizers stored (GUC was OFF at every INSERT for this rel)
 *   1.0 → every row has a minimizer (the default path with GUC=ON)
 *   fractional → some rows were inserted with the GUC off, or a bulk COPY
 *                bypassed the ON path, or the sidecar was partially
 *                vacuumed.
 *
 * Called from the planner (AddColumnarScanPath) to scale the cost
 * reduction that drives the custom scan's "fetch from meta" fast path,
 * and from ColumnarScan_ExplainCustomScan to surface the estimate in
 * EXPLAIN output. Implementation cost: O(matching_rows) via the
 * index_sidecar PK btree — amortised across a single query plan. The
 * user-relation row count is O(1) for bioseq-AM tables (stripe metadata)
 * and uses pg_class.reltuples otherwise.
 *
 * `estimated_total_rows` is typically `rel->tuples` from the
 * RelOptInfo — pg_class.reltuples for the user relation. When stats are
 * missing we fall through to ColumnarTableRowCount() for bioseq-AM
 * tables; for everything else we assume full coverage. */
export [[nodiscard]] float estimate_index_presence(Oid user_relid,
												   double estimated_total_rows) {
	const SidecarOids& oids = get_sidecar_oids();
	if (oids.index_sidecar_relid == InvalidOid || !OidIsValid(user_relid)) {
		return 0.0f;
	}

	/* Resolve the user-relation row count FIRST so we can cap the sidecar
	 * scan at that count. On a fully-covered, very large table we would
	 * otherwise walk every single sidecar row just to learn the ratio
	 * clamps to 1.0. Early-termination once `sidecar_rows > resolved_rows`
	 * bounds the planner cost at O(min(sidecar_rows, estimated_total_rows)). */
	const double resolved_rows = resolve_user_relation_tuples(
		user_relid, estimated_total_rows);
	const bool   have_total    = resolved_rows > 0.0;
	const uint64_t cap         = have_total
		? static_cast<uint64_t>(resolved_rows) + 1  /* +1 so we can detect overshoot */
		: std::numeric_limits<uint64_t>::max();

	Relation rel = try_safe_relation_open(oids.index_sidecar_relid, AccessShareLock);
	if (rel == nullptr) {
		return 0.0f;
	}

	const Oid pk_index_oid = find_primary_key_oid(rel);
	if (pk_index_oid == InvalidOid) {
		/* The extension install is broken (the PK btree must exist per
		 * bioseq--1.0.sql §2.1). Fall through to a sequential scan and
		 * log loudly so the operator notices before they wonder why
		 * planner time blew up under load. Mirrors sidecar_read_index. */
		JustLog("[bioseq] sidecar: index_sidecar has no primary-key btree; "
				"falling back to sequential scan for presence estimation "
				"(install is broken, expect slow plans)");
	}

	ScanKeyData keys[1];
	ScanKeyInit(&keys[0], 1 /*user_relid*/,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(user_relid));

	SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
										  /*indexOK=*/ pk_index_oid != InvalidOid,
										  NULL /*snapshot*/, 1, keys);

	uint64_t sidecar_rows = 0;
	while (sidecar_rows < cap && HeapTupleIsValid(systable_getnext(scan))) {
		++sidecar_rows;
	}
	systable_endscan(scan);
	relation_close(rel, AccessShareLock);

	if (!have_total) {
		/* Stats genuinely missing for a non-columnar table — treat the
		 * sidecar presence as binary: if any row exists, assume the
		 * caller wants the fast path; otherwise 0. */
		return sidecar_rows == 0 ? 0.0f : 1.0f;
	}

	const float ratio = static_cast<float>(
		static_cast<double>(sidecar_rows) / resolved_rows);
	if (ratio < 0.0f) return 0.0f;
	if (ratio > 1.0f) return 1.0f;
	return ratio;
}

/* Fixed-size result of a meta_sidecar lookup. The maximum alphabet size
 * supported by pgseq_t::dispatch_instance is 5 (dna5); we round to 8 to
 * keep the struct cache-line friendly and leave headroom for future
 * alphabets.
 * Using a stack-sized struct here avoids the std::vector<int64_t>
 * heap allocation that every per-row scalar lookup otherwise pays for. */
export struct SymbolCounts {
	std::array<int64_t, 8> values{};
	uint8_t                size    = 0;
};

/* Read a single (user_relid, rowno) row from meta_sidecar and return the
 * stored symbol_counts[] array inside a stack-sized SymbolCounts struct,
 * or empty optional when the row is missing. Uses the same PK-btree
 * fast-path as sidecar_read_index. */
export std::optional<SymbolCounts> sidecar_read_meta_counts(Oid user_relid, int64_t rowno) {
	const SidecarOids& oids = get_sidecar_oids();
	if (oids.meta_sidecar_relid == InvalidOid) {
		return std::nullopt;
	}

	Relation rel = try_safe_relation_open(oids.meta_sidecar_relid, AccessShareLock);
	if (rel == nullptr) {
		return std::nullopt;
	}

	const Oid pk_index_oid = find_primary_key_oid(rel);
	if (pk_index_oid == InvalidOid) {
		JustLog("[bioseq] sidecar: meta_sidecar has no primary-key btree; falling back to seqscan");
	}

	ScanKeyData keys[2];
	ScanKeyInit(&keys[0], 1 /*user_relid*/,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(user_relid));
	ScanKeyInit(&keys[1], 2 /*sequence_rowno*/,
				BTEqualStrategyNumber, F_INT8EQ,
				Int64GetDatum(rowno));

	SysScanDesc scan = systable_beginscan(rel, pk_index_oid,
										  /*indexOK=*/ pk_index_oid != InvalidOid,
										  NULL /*snapshot*/, 2, keys);

	std::optional<SymbolCounts> result;
	HeapTuple tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup)) {
		bool isnull = false;
		Datum arr_datum = heap_getattr(tup, 4 /*symbol_counts*/,
									   RelationGetDescr(rel), &isnull);
		if (!isnull) {
			ArrayType* arr = DatumGetArrayTypeP(arr_datum);
			Datum*  elem_datums;
			bool*   elem_nulls;
			int     n_returned = 0;
			deconstruct_array(arr, INT8OID, 8, true, 'd',
							  &elem_datums, &elem_nulls, &n_returned);
			SymbolCounts out{};
			/* Warn loudly when a stored meta row has more counts than the
			 * stack-sized wrapper can carry. Today the writer caps at 8 via
			 * kMaxAlphabetSize in sidecar_compute_and_write_meta, but a
			 * hand-restored dump or a future wider alphabet would silently
			 * return 0 for the truncated slots and present as
			 * "symbol_count = 0" — better to surface the misconfiguration. */
			if (n_returned > static_cast<int>(out.values.size())) {
				JustLog("[bioseq] sidecar: meta row (relid={}, rowno={}) has {} counts, capped to {} — update SymbolCounts::values",
						static_cast<unsigned>(user_relid), rowno,
						n_returned, static_cast<int>(out.values.size()));
			}
			const int capped = std::min(n_returned,
										static_cast<int>(out.values.size()));
			for (int i = 0; i < capped; ++i) {
				out.values[i] = elem_nulls[i] ? int64_t{0} : DatumGetInt64(elem_datums[i]);
			}
			out.size = static_cast<uint8_t>(capped);
			pfree(elem_datums);
			pfree(elem_nulls);
			result = out;
			if (reinterpret_cast<void*>(arr) != reinterpret_cast<void*>(DatumGetPointer(arr_datum))) {
				pfree(arr);
			}
		}
	}
	systable_endscan(scan);
	relation_close(rel, AccessShareLock);
	return result;
}

/* Resolve an alphabet letter to its seqan3 rank. Returns -1 for letters
 * outside the alphabet. The helper exists so the custom-scan planner can
 * pre-compute the rank once per symbol_count() call site without paying
 * for a pgseq_t::dispatch_instance template instantiation + seqan3
 * assign_char on every EXPLAIN re-render or VarInjector rewrite.
 *
 * The table is built at static-init time by walking the seqan3 alphabets
 * for every supported alphabet_code; the per-lookup cost is a pair of
 * array indexes. Only alphabet_code is load-bearing; the storage byte
 * width is irrelevant because the rank mapping is identical for every
 * storage width of the same alphabet. */
namespace detail {

/* pgseq_supported_alphabets::alphabet_count is the single source of truth
 * — it lives in `bioseq_sequences:sequences` and grows automatically when
 * the pack is extended. The build-time table mirrors that count. */
constexpr std::size_t kNumAlphabets =
	static_cast<std::size_t>(pgseq_supported_alphabets::alphabet_count);
constexpr std::size_t kCharDomain   = 256;

inline auto build_letter_rank_table() {
	std::array<std::array<int8_t, kCharDomain>, kNumAlphabets> table{};
	for (auto& row : table) {
		row.fill(static_cast<int8_t>(-1));
	}
	for (uint8_t alphabet_code = 0; alphabet_code < kNumAlphabets; ++alphabet_code) {
		pgseq_t::dispatch_instance(alphabet_code, /*byte_width=*/ 1,
			[&]<class S>() {
				using alphabet_t = typename S::alphabet_type;
				for (int c = 0; c < static_cast<int>(kCharDomain); ++c) {
					const char letter = static_cast<char>(c);
					if (!seqan3::char_is_valid_for<alphabet_t>(letter)) {
						continue;
					}
					alphabet_t sym{};
					sym.assign_char(letter);
					table[alphabet_code][static_cast<uint8_t>(c)] =
						static_cast<int8_t>(sym.to_rank());
				}
			});
	}
	return table;
}

inline const auto& letter_rank_table() {
	static const auto table = build_letter_rank_table();
	return table;
}

} // namespace detail

export [[nodiscard]] inline int letter_to_rank(uint8_t alphabet_code, char letter) {
	if (alphabet_code >= detail::kNumAlphabets) {
		return -1;
	}
	const auto& table = detail::letter_rank_table();
	return static_cast<int>(table[alphabet_code][static_cast<uint8_t>(letter)]);
}

/* ---------------------------------------------------------------- */
/* PER_ROW recompute helpers — used by bioseq.update_index().       */
/* ---------------------------------------------------------------- */

/* Loop body shared by both PER_ROW recomputes. Walks the user table once,
 * pre-deletes any existing rows for `user_relid` from the backing sidecar,
 * and writes one fresh blob per surviving user-table tuple via the supplied
 * writer callable.
 *
 * Invariant: caller (update_index_impl) has verified user_relid uses the
 * bioseq columnar AM. tid_to_row_number is the inverse of row_number_to_tid
 * and is correct only for the bioseq AM.
 *
 * Locking: takes ShareRowExclusiveLock on the backing sidecar so concurrent
 * update_index calls on the same (rel, kind) pair serialise instead of
 * racing into duplicate-key errors. Plain SELECTs (AccessShareLock) on the
 * sidecar continue uninterrupted. */
template <typename WriteOne>
[[nodiscard]] static int64_t recompute_per_row_for_relid_impl(
	Oid user_relid, Oid backing_sidecar_relid,
	const char* delete_sql, const char* progress_label,
	WriteOne&& write_one)
{
	if (!OidIsValid(backing_sidecar_relid)) {
		return 0;
	}

	LockRelationOid(backing_sidecar_relid, ShareRowExclusiveLock);

	if (SPI_connect() != SPI_OK_CONNECT) {
		ereport(ERROR, (errmsg("bioseq.update_index: SPI_connect failed")));
	}
	Oid   delete_arg_types[1]  = { OIDOID };
	Datum delete_arg_values[1] = { ObjectIdGetDatum(user_relid) };
	int64_t pre_existing = 0;
	if (SPI_execute_with_args(delete_sql, 1, delete_arg_types, delete_arg_values,
							  /*Nulls=*/nullptr,
							  /*read_only=*/false, /*tcount=*/0) != SPI_OK_DELETE) {
		SPI_finish();
		ereport(ERROR,
				(errmsg("bioseq.update_index: DELETE failed for sidecar oid=%u",
						static_cast<unsigned>(backing_sidecar_relid))));
	}
	pre_existing = static_cast<int64_t>(SPI_processed);
	SPI_finish();

	const SidecarOids& oids = get_sidecar_oids();

	Relation user_rel = try_safe_relation_open(user_relid, AccessShareLock);
	if (user_rel == nullptr) {
		return 0;
	}
	TupleDesc user_td = RelationGetDescr(user_rel);

	AttrNumber seq_attno = InvalidAttrNumber;
	for (int i = 0; i < user_td->natts; ++i) {
		Form_pg_attribute attr = TupleDescAttr(user_td, i);
		if (attr->atttypid == oids.bioseq_seq_type_oid) {
			seq_attno = static_cast<AttrNumber>(i + 1);
			break;
		}
	}
	if (seq_attno == InvalidAttrNumber) {
		relation_close(user_rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("bioseq.update_index: relation %s has no "
						"bioseq.seq column", get_rel_name(user_relid))));
	}

	Snapshot snapshot = GetActiveSnapshot();
	TableScanDesc scan = table_beginscan_strat(
		user_rel, snapshot, /*nkeys=*/0, /*key=*/nullptr,
		/*allow_strat=*/true, /*allow_sync=*/true);
	TupleTableSlot* slot = MakeSingleTupleTableSlot(
		user_td, table_slot_callbacks(user_rel));

	int64_t written = 0;
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot)) {
		slot_getallattrs(slot);
		bool isnull = false;
		Datum dat = slot_getattr(slot, seq_attno, &isnull);
		if (isnull) {
			continue;
		}

		const pgseq_t* hdr =
			reinterpret_cast<const pgseq_t*>(
				PG_DETOAST_DATUM(dat));

		const int64_t rowno = static_cast<int64_t>(
			tid_to_row_number(slot->tts_tid));

		write_one(user_relid, rowno, hdr);
		++written;

		if ((written & 0x3FFF) == 0) {
			JustLog("[bioseq] update_index: {} progress relid={} written={}",
					progress_label,
					static_cast<unsigned>(user_relid), written);
		}

		if (reinterpret_cast<const void*>(hdr)
			!= reinterpret_cast<const void*>(DatumGetPointer(dat))) {
			pfree(const_cast<pgseq_t*>(hdr));
		}
	}

	ExecDropSingleTupleTableSlot(slot);
	table_endscan(scan);
	relation_close(user_rel, AccessShareLock);

	JustLog("[bioseq] update_index: {} DONE relid={} pre_existing={} written={}",
			progress_label, static_cast<unsigned>(user_relid),
			pre_existing, written);
	return written;
}

/* Scan `user_relid` once and write one (user_relid, rowno, blob) tuple
 * into bioseq_internal.index_sidecar per surviving row. Pre-deletes any
 * existing rows for `user_relid` so the result is idempotent regardless
 * of prior state. Returns the count of rows written. */
export [[nodiscard]] int64_t recompute_per_row_index_for_relid(Oid user_relid) {
	const SidecarOids& oids = get_sidecar_oids();
	return recompute_per_row_for_relid_impl(
		user_relid, oids.index_sidecar_relid,
		"DELETE FROM bioseq_internal.index_sidecar WHERE user_relid = $1",
		"minimizers",
		[](Oid relid, int64_t rowno, const pgseq_t* hdr) {
			sidecar_compute_and_write_index(relid, rowno, hdr);
		});
}

/* Same shape, but writes meta_sidecar rows. */
export [[nodiscard]] int64_t recompute_per_row_meta_for_relid(Oid user_relid) {
	const SidecarOids& oids = get_sidecar_oids();
	return recompute_per_row_for_relid_impl(
		user_relid, oids.meta_sidecar_relid,
		"DELETE FROM bioseq_internal.meta_sidecar WHERE user_relid = $1",
		"symbol_counts",
		[](Oid relid, int64_t rowno, const pgseq_t* hdr) {
			sidecar_compute_and_write_meta(relid, rowno, hdr);
		});
}

/* ---------------------------------------------------------------- */
/* MetadataKind descriptors — bound to the metadata_admin registry. */
/* ---------------------------------------------------------------- */

namespace {

[[nodiscard]] Oid minimizers_backing_relation_oid() noexcept {
	return get_sidecar_oids().index_sidecar_relid;
}

[[nodiscard]] Oid symbol_counts_backing_relation_oid() noexcept {
	return get_sidecar_oids().meta_sidecar_relid;
}

[[nodiscard]] int64_t minimizers_estimate_size_bytes(Oid user_relid) {
	return bioseq::metadata_admin::estimate_blob_size_for_relid(
		minimizers_backing_relation_oid(),
		/*blob_attno=*/3, /* index_sidecar.target_index */
		user_relid);
}

[[nodiscard]] int64_t symbol_counts_estimate_size_bytes(Oid user_relid) {
	return bioseq::metadata_admin::estimate_blob_size_for_relid(
		symbol_counts_backing_relation_oid(),
		/*blob_attno=*/4, /* meta_sidecar.symbol_counts */
		user_relid);
}

void minimizers_enumerate_user_relids(
	bioseq::metadata_admin::UserRelidEmitter emit) {
	bioseq::metadata_admin::enumerate_distinct_user_relids(
		minimizers_backing_relation_oid(), emit);
}

void symbol_counts_enumerate_user_relids(
	bioseq::metadata_admin::UserRelidEmitter emit) {
	bioseq::metadata_admin::enumerate_distinct_user_relids(
		symbol_counts_backing_relation_oid(), emit);
}

[[nodiscard]] int64_t minimizers_recompute_for_relid(Oid user_relid) {
	return recompute_per_row_index_for_relid(user_relid);
}

[[nodiscard]] int64_t symbol_counts_recompute_for_relid(Oid user_relid) {
	return recompute_per_row_meta_for_relid(user_relid);
}

}  // namespace

/* Called once from bioseq_init() (via bioseq::sidecar::register_default_metadata_kinds).
 * Adds the 'minimizers' and 'symbol_counts' kinds to the process-global
 * registry. noexcept: the registry is fixed-capacity and register_kind
 * cannot throw. */
export void register_default_metadata_kinds() noexcept {
	namespace mda = bioseq::metadata_admin;
	auto& registry = mda::MetadataKindRegistry::instance();
	registry.register_kind({
		.name                  = "minimizers",
		.mode                  = mda::MetadataMode::PER_ROW,
		.insert_support        = true,
		.backing_relation_oid  = &minimizers_backing_relation_oid,
		.estimate_size_bytes   = &minimizers_estimate_size_bytes,
		.enumerate_user_relids = &minimizers_enumerate_user_relids,
		.recompute_for_relid   = &minimizers_recompute_for_relid,
	});
	registry.register_kind({
		.name                  = "symbol_counts",
		.mode                  = mda::MetadataMode::PER_ROW,
		.insert_support        = true,
		.backing_relation_oid  = &symbol_counts_backing_relation_oid,
		.estimate_size_bytes   = &symbol_counts_estimate_size_bytes,
		.enumerate_user_relids = &symbol_counts_enumerate_user_relids,
		.recompute_for_relid   = &symbol_counts_recompute_for_relid,
	});
}

} // namespace bioseq::sidecar
