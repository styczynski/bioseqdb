/*-------------------------------------------------------------------------
 *
 * table/access_common.cppm
 *
 * Shared table-access helpers with no dependency on routine assembly.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

#include <cstdint>
#include <cstring>

export module bioseq_table:access_common;

import mlogging;
import pg_utils;
import bioseq_table_types;
import bioseq_storage;
import bioseq_am_cache;
import bioseq_types.pg_compact_sequence;

namespace {

static TupleTableSlotOps TTSOpsBioseqColumn;

/*
 * Implementation of TupleTableSlotOps.copy_heap_tuple for TTSOpsBioseqColumn.
 */
static HeapTuple
BioseqColumnSlotCopyHeapTuple(TupleTableSlot *slot)
{
	bioseq_log<DBG>("[bioseq] pg: BioseqColumnSlotCopyHeapTuple() entered");
	Assert(!TTS_EMPTY(slot));

	HeapTuple tuple = heap_form_tuple(slot->tts_tupleDescriptor,
									  slot->tts_values,
									  slot->tts_isnull);

	/* slot->tts_tid is filled in bioseq_getnextslot */
	tuple->t_self = slot->tts_tid;

	return tuple;
}

}

export void
BioseqColumnInitTupleSlotOps()
{
	TTSOpsBioseqColumn = TTSOpsVirtual;
	TTSOpsBioseqColumn.copy_heap_tuple = BioseqColumnSlotCopyHeapTuple;
}

export const TupleTableSlotOps *
bioseq_slot_callbacks(Relation relation)
{
	return &TTSOpsBioseqColumn;
}

export MemoryContext
CreateBioseqColumnScanMemoryContext(void)
{
	return AllocSetContextCreate(CurrentMemoryContext, "BioseqColumn Scan Context",
								 ALLOCSET_DEFAULT_SIZES);
}

/*
 * NeededColumnsList returns a list of AttrNumber's for the columns that
 * are not dropped and specified by attr_needed.
 */
export [[nodiscard]] List *
NeededColumnsList(TupleDesc tupdesc, const PBitmapset& attr_needed)
{
	PIntList columnList;
	for (int i = 0; i < tupdesc->natts; i++)
	{
		if (TupleDescAttr(tupdesc, i)->attisdropped)
		{
			continue;
		}

		/* attr_needed is 0-indexed but columnList is 1-indexed */
		if (attr_needed.contains(i))
		{
			columnList.append(i + 1);
		}
	}

	return columnList.release();
}

/*
 * Detoast and decompress all values. If there's no work to do, return
 * original pointer; otherwise return a newly-allocated values array. Should
 * be called in per-tuple context.
 */
export Datum *
detoast_values(TupleDesc tupleDesc, Datum *orig_values, bool *isnull)
{
	int natts = tupleDesc->natts;

	/* copy on write to optimize for case where nothing is toasted */
	Datum *values = orig_values;

	for (int i = 0; i < tupleDesc->natts; i++)
	{
		if (!isnull[i] && TupleDescAttr(tupleDesc, i)->attlen == -1 &&
			VARATT_IS_EXTENDED(values[i]))
		{
			/* make a copy */
			if (values == orig_values)
			{
				values = make_array<Datum>(natts);
				memcpy(values, orig_values, sizeof(Datum) * natts);
			}

			/* will be freed when per-tuple context is reset */
			varlena *new_value = reinterpret_cast<varlena *>(DatumGetPointer(values[i]));
			new_value = detoast_attr(new_value);
			values[i] = PointerGetDatum(new_value);
		}
	}

	return values;
}

/*
 * BioseqColumnCheckLogicalReplication throws an error if the relation is
 * part of any publication. This should be called before any write to a bioseq
 * column table, because column changes are not replicated with logical
 * replication (similar to a row table without a replica identity).
 */
export void
BioseqColumnCheckLogicalReplication(Relation rel)
{
	bool pubActionInsert = false;

	if (!is_publishable_relation(rel))
	{
		return;
	}

	{
		PublicationDesc pubdesc;

		RelationBuildPublicationDesc(rel, &pubdesc);
		pubActionInsert = pubdesc.pubactions.pubinsert;
	}

	if (pubActionInsert)
	{
		bioseq_report<ERR, PG>().code(ERRCODE_FEATURE_NOT_SUPPORTED)
			.msg("cannot insert into columnar table that is a part of a publication");
	}
}

/*
 * Hydrate locator stubs in SEQ columns into real varlena values. The caller
 * must run with a memory context where the hydrated allocations are legal.
 */
export [[gnu::always_inline]] inline void
hydrate_seq_columns_in_slot(Relation main_rel, TupleTableSlot* slot,
                            bioseq::storage::seq_caches* cache,
                            const bioseq::storage::BioseqAmCache& amc,
                            MemoryContext payload_mctx)
{
	if (cache == nullptr || amc.seq_attnos.empty()) {
		return;
	}
	const RelFileLocator& loc = main_rel->rd_locator;
	for (std::int16_t attno : amc.seq_attnos) {
		const int idx = attno - 1;
		if (idx < 0 || idx >= slot->tts_tupleDescriptor->natts) continue;
		if (slot->tts_isnull[idx]) continue;
		auto* hdr = reinterpret_cast<const pgseq_t*>(
			DatumGetPointer(slot->tts_values[idx]));
		if (!bioseq::storage::is_locator_stub(hdr)) continue;
		const auto loc_decoded = bioseq::storage::decode_locator_from_stub(hdr);
		const std::uint64_t projected_payload_bytes =
			(loc_decoded.total_bp_size * /*bits_per_letter min=*/2u + 7u) / 8u;
		const std::uint64_t projected_varlena =
			static_cast<std::uint64_t>(pgseq_t::header_bytes())
			+ projected_payload_bytes;
		if (projected_varlena > static_cast<std::uint64_t>(MaxAllocSize)) {
			bioseq_log<ERR, PG>(
			       "bioseq storage Phase C: sequence too large ({} bp => ~{} bytes) "
			       "to materialise as a single varlena. SELECT seq / pg_dump "
			       "cannot return this row directly. Use bioseq.substring(seq, ...) "
			       "to project a sub-region, or wait for Phase D D-VARLENA. "
			       "See DOC0267.",
			       loc_decoded.total_bp_size, projected_varlena);
		}
		pgseq_t* real = bioseq::storage::bioseq_default_storage_manager::fetch_payload(
			loc.dbOid, loc.spcOid, loc.relNumber,
			attno, loc_decoded, payload_mctx, cache);
		slot->tts_values[idx] = PointerGetDatum(real);
	}
}

export void
bioseq_hydrate_scan_slot(TableScanDesc sscan, TupleTableSlot* slot, bool force)
{
	BioseqColumnScanDesc scan = reinterpret_cast<BioseqColumnScanDesc>(sscan);
	if (scan == nullptr) {
		return;
	}
	if (!force && scan->defer_seq_hydration) {
		return;
	}
	const auto& amc = bioseq::storage::bioseq_am_cache_for(scan->cs_base.rs_rd);
	auto* caches = static_cast<bioseq::storage::seq_caches*>(scan->seq_cache);
	hydrate_seq_columns_in_slot(scan->cs_base.rs_rd, slot, caches, amc,
	                            scan->scanContext);
}
