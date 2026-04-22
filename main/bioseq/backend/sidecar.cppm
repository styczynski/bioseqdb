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
}

#include <cstdint>
#include <cstring>
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

/* Pull TargetIndex / sketch_collector / collect_minimizers / minimap_int_hasher
 * / dispatch_pgseq / make_seq_range_from_pg / compact_sequence_range /
 * PgCompactSequenceHdr out of their home namespaces so the compute path below
 * reads clean. The minimap module places all of these inside
 * bioseq::simd::HWY_NAMESPACE; on ARM64 HWY_NAMESPACE resolves to N_NEON. */
using bioseq::simd::HWY_NAMESPACE::TargetIndex;
using bioseq::simd::HWY_NAMESPACE::sketch_collector;
using bioseq::simd::HWY_NAMESPACE::collect_minimizers;
using bioseq::simd::HWY_NAMESPACE::minimap_int_hasher;
/* dispatch_pgseq / make_seq_range_from_pg / compact_sequence_range /
 * PgCompactSequenceHdr live in the root namespace (the pg_compact_sequence
 * module doesn't wrap in HWY_BEFORE_NAMESPACE). They are reachable directly. */

namespace bioseq::sidecar {

/* ---------------------------------------------------------------- */
/* Thread-local scan-row context                                    */
/*                                                                  */
/* Set by the custom-scan execution loop (one entry per projected   */
/* tuple) before ExecProject fires, so that scalar functions like   */
/* bioseq.symbol_count can recover the (user_relid, rowno) pair and */
/* use the meta_sidecar instead of recomputing from the varlena.    */
/* ---------------------------------------------------------------- */
export struct ScanRowContext {
	Oid     user_relid     = InvalidOid;
	int64_t sequence_rowno = -1;
	bool    active         = false;
};

/* Module-linkage thread_local — callers inside bioseq::sidecar see this
 * directly; the symbol is also visible via `import bioseq_sidecar` from
 * other cppm modules (mod.cppm's bioseq_symbol_count and columnar_customscan
 * exec helpers). */
export inline thread_local ScanRowContext g_scan_row_ctx{};

/* RAII guard that activates g_scan_row_ctx for the lifetime of a single
 * projected tuple. Exceptions / longjmps on the ExecProject path cannot
 * desync the context because this guard's destructor runs via normal
 * stack-unwind (there is no PG_TRY around the guard itself; PG_CATCH in
 * the executor is above this frame). */
export struct ScanRowContextScope {
	ScanRowContext previous;
	ScanRowContextScope(Oid relid, int64_t rowno) {
		previous = g_scan_row_ctx;
		g_scan_row_ctx = ScanRowContext{relid, rowno, true};
	}
	~ScanRowContextScope() {
		g_scan_row_ctx = previous;
	}
	ScanRowContextScope(const ScanRowContextScope&) = delete;
	ScanRowContextScope& operator=(const ScanRowContextScope&) = delete;
};

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
 * (the stored Postgres array is accessed as symbol_counts[1..N]). */
[[nodiscard]] static ArrayType* make_int8_array(const int64_t* counts, int n) {
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
											const PgCompactSequenceHdr* hdr)
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

	dispatch_pgseq(hdr->alphabet_code, hdr->width_code,
		[&]<class Dst>() {
			compact_sequence_range<Dst> seq = make_seq_range_from_pg<Dst>(hdr);

			sketch_collector collector;
			collect_minimizers<minimap_int_hasher>(
				collector, seq, seq.begin(), seq.end(),
				/*w=*/10, /*k=*/15);
			auto minimizers = collector.extract_sketches_data();

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
										   const PgCompactSequenceHdr* hdr)
{
	if (hdr == nullptr) {
		return;
	}

	constexpr int kMaxAlphabetSize = 8;
	int64_t counts[kMaxAlphabetSize] = { 0 };
	int num_counts = 0;

	dispatch_pgseq(hdr->alphabet_code, hdr->width_code,
		[&]<class Dst>() {
			compact_sequence_range<Dst> seq = make_seq_range_from_pg<Dst>(hdr);
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

	const PgCompactSequenceHdr* hdr = reinterpret_cast<const PgCompactSequenceHdr*>(
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

/* Discover the OID of the primary-key btree on `rel`, or InvalidOid when
 * the relation has none. Factored so both sidecar read paths can call the
 * same logic and stay in sync. */
[[nodiscard]] static Oid find_primary_key_oid(Relation rel) {
	Oid pk_index_oid = InvalidOid;
	List* ilist = RelationGetIndexList(rel);
	ListCell* lc;
	foreach (lc, ilist) {
		Oid ioid = lfirst_oid(lc);
		Relation irel = index_open(ioid, AccessShareLock);
		if (irel->rd_index != nullptr && irel->rd_index->indisprimary) {
			pk_index_oid = ioid;
		}
		index_close(irel, AccessShareLock);
		if (pk_index_oid != InvalidOid) {
			break;
		}
	}
	list_free(ilist);
	return pk_index_oid;
}

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

/* Read a single (user_relid, rowno) row from meta_sidecar and return the
 * stored symbol_counts[] array as a std::vector<int64_t>, or empty optional
 * when missing. Uses the same PK-btree fast-path as sidecar_read_index. */
export std::optional<std::vector<int64_t>> sidecar_read_meta_counts(Oid user_relid, int64_t rowno) {
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

	std::optional<std::vector<int64_t>> result;
	HeapTuple tup = systable_getnext(scan);
	if (HeapTupleIsValid(tup)) {
		bool isnull = false;
		Datum arr_datum = heap_getattr(tup, 4 /*symbol_counts*/,
									   RelationGetDescr(rel), &isnull);
		if (!isnull) {
			ArrayType* arr = DatumGetArrayTypeP(arr_datum);
			const int nelems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
			std::vector<int64_t> out;
			out.reserve(nelems);
			Datum*  elem_datums;
			bool*   elem_nulls;
			int     n_returned = 0;
			deconstruct_array(arr, INT8OID, 8, true, 'd',
							  &elem_datums, &elem_nulls, &n_returned);
			for (int i = 0; i < n_returned; ++i) {
				out.push_back(elem_nulls[i] ? int64_t{0} : DatumGetInt64(elem_datums[i]));
			}
			pfree(elem_datums);
			pfree(elem_nulls);
			result = std::move(out);
			if (reinterpret_cast<void*>(arr) != reinterpret_cast<void*>(DatumGetPointer(arr_datum))) {
				pfree(arr);
			}
		}
	}
	systable_endscan(scan);
	relation_close(rel, AccessShareLock);
	return result;
}

/* DEPRECATED: kept only to document the rank orderings for reviewers;
 * callers should use seqan3 via `dispatch_pgseq` + `alphabet_t{}.assign_char`
 * so there is a single source of truth for rank mapping. Intentionally
 * unused — the one call site in bioseq_symbol_count was switched to the
 * seqan3 path. Delete when the next refactor lands. */
[[maybe_unused, nodiscard]] static inline int
letter_to_rank_for_alphabet_reference_only(uint8_t alphabet_code, char letter) {
	switch (alphabet_code) {
		case 0: /* dna4 */
			switch (letter) {
				case 'A': case 'a': return 0;
				case 'C': case 'c': return 1;
				case 'G': case 'g': return 2;
				case 'T': case 't': return 3;
				default: return -1;
			}
		case 1: /* dna5 — seqan3 rank: A=0, C=1, G=2, N=3, T=4 */
			switch (letter) {
				case 'A': case 'a': return 0;
				case 'C': case 'c': return 1;
				case 'G': case 'g': return 2;
				case 'N': case 'n': return 3;
				case 'T': case 't': return 4;
				default: return -1;
			}
		default:
			return -1;
	}
}

} // namespace bioseq::sidecar
