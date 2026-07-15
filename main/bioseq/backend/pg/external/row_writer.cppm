/*-------------------------------------------------------------------------
 *
 * external/row_writer.cppm
 *
 * DOC0372 — "PostgreSQL logical tables". Materialize a normalized owning
 * provider message into a virtual TupleTableSlot for one of the six logical
 * bioseq_external relations.
 *
 * The CustomScan (scan_state.cppm) drives a stream of owning_provider_message
 * values out of provider core. Each message can "explode" into 0..N output
 * rows depending on the relation shape (e.g. one catalog message yields one
 * catalog row but genes.size() record_genes rows). This translation unit owns
 * the two pieces of that mapping:
 *
 *   - bioseq_external_message_row_count(): how many rows a message yields for
 *     a given entity (explode sizing), and
 *   - bioseq_external_fill_slot(): fill slot->tts_values[]/tts_isnull[] for one
 *     sub-row.
 *
 * The column order for each relation is authoritative in
 * bioseq/sql/bioseq_external--1.0.sql; the per-entity fillers below fill
 * tts_values in exactly that 0-based order and Assert() the expected natts to
 * catch column drift in assert-enabled builds. Every palloc'd value (text,
 * jsonb, arrays) is allocated in CurrentMemoryContext — the executor's
 * per-tuple context — because ExecStoreVirtualTuple (done by the caller) keeps
 * the pointers; nothing is freed here.
 *
 * The sequence / sequence_chunk relations materialize an owning_sequence_payload
 * (the retrieved nucleotide letters + region metadata) into the bioseq.seq
 * column via stdc_convert_from_cstr<pgseq_t, seq::dna5> + pg_alloc.
 *
 *-------------------------------------------------------------------------
 */
module;
#include "common.h"

extern "C" {
#include "utils/builtins.h"     /* cstring_to_text_with_len, CStringGetTextDatum */
#include "utils/jsonb.h"        /* Jsonb type; jsonb_in also declared in fmgrprotos.h */
#include "utils/timestamp.h"    /* TimestampTz, TimestampTzGetDatum */
#include "utils/date.h"         /* DateADT, DateADTGetDatum */
#include "utils/datetime.h"     /* date2j — NOT pulled in by utils/date.h */
#include "utils/array.h"        /* construct_array */
#include "catalog/pg_type_d.h"  /* INT8OID, TEXTOID */
}

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module bioseq.pg.external.row_writer;

import bioseq.pg.external.relation;   /* BioseqExternalEntityKind */
import bioseq.external.provider.base;
import bioseq.external.provider.model;
import bioseq.external.provider.messages;
import bioseq.common.sequan_compat;   /* seq::dna5 */
import bioseq.sequences.simd;         /* ::bioseq::simd::active_target::stdc_convert_from_cstr */
import bioseq.types.pg_compact_sequence; /* pgseq_t, pg_alloc — the bioseq.seq varlena */

namespace prov = bioseq::external::provider;

/* ======================================================================== */
/* Public entry points                                                       */
/* ======================================================================== */

/*
 * How many output rows a message yields for `entity` (explode sizing). A
 * message whose active variant does not match the entity yields 0 rows; so do
 * retrieval plans and diagnostics for every relational entity.
 */
export std::size_t bioseq_external_message_row_count(
	BioseqExternalEntityKind entity, const prov::owning_provider_message& msg);

/*
 * Fill `slot`'s tts_values[]/tts_isnull[] for the sub-row `sub_index` of `msg`
 * under `entity`. The caller does ExecClearTuple + ExecStoreVirtualTuple.
 * Returns true if a row was written (msg matched entity and sub_index is in
 * range), false to skip. Fills ALL slot->tts_tupleDescriptor->natts columns
 * (each column gets a value or isnull=true).
 */
export bool bioseq_external_fill_slot(
	BioseqExternalEntityKind entity, const prov::owning_provider_message& msg,
	std::size_t sub_index, TupleTableSlot* slot);

/* ======================================================================== */
/* Column counts (authoritative: bioseq_external--1.0.sql)                   */
/* ======================================================================== */

namespace {

/* [[maybe_unused]]: Assert() is a no-op in non-assert PG builds, which would
 * otherwise leave these unreferenced under -Wunused-const-variable. */
[[maybe_unused]] constexpr int TAXONOMY_NATTS = 13;
[[maybe_unused]] constexpr int CATALOG_NATTS = 35;
[[maybe_unused]] constexpr int RECORD_GENES_NATTS = 17;
[[maybe_unused]] constexpr int RECORD_LOCATIONS_NATTS = 17;
[[maybe_unused]] constexpr int SEQUENCE_NATTS = 18;
[[maybe_unused]] constexpr int SEQUENCE_CHUNK_NATTS = 21;

/* ======================================================================== */
/* Datum construction helpers (all allocate in CurrentMemoryContext)         */
/* ======================================================================== */

/* Escape a run of (identifier) text into a JSON string body: quote and
 * backslash per JSON, control bytes as \u00XX so jsonb_in never rejects the
 * value. Bytes >= 0x80 pass through unchanged (valid UTF-8 continuation). */
inline void
append_json_escaped(std::string& out, std::string_view sv)
{
	static constexpr char hex[] = "0123456789abcdef";
	for (unsigned char c : sv)
	{
		switch (c)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (c < 0x20)
				{
					out += "\\u00";
					out += hex[(c >> 4) & 0x0F];
					out += hex[c & 0x0F];
				}
				else
				{
					out += static_cast<char>(c);
				}
		}
	}
}

inline void
set_null(TupleTableSlot* slot, std::size_t i)
{
	slot->tts_isnull[i] = true;
	slot->tts_values[i] = static_cast<Datum>(0);
}

inline void
set_text(TupleTableSlot* slot, std::size_t i, std::string_view sv)
{
	const char* p = sv.empty() ? "" : sv.data();
	slot->tts_values[i] =
		PointerGetDatum(cstring_to_text_with_len(p, static_cast<int>(sv.size())));
	slot->tts_isnull[i] = false;
}

/* Any bounded_string<N> optional -> text, null when absent. */
template <std::size_t N>
inline void
set_text_opt(TupleTableSlot* slot, std::size_t i,
			 const std::optional<prov::bounded_string<N>>& v)
{
	if (v)
		set_text(slot, i, v->view());
	else
		set_null(slot, i);
}

inline void
set_int64(TupleTableSlot* slot, std::size_t i, std::int64_t v)
{
	slot->tts_values[i] = Int64GetDatum(v);
	slot->tts_isnull[i] = false;
}

inline void
set_int64_opt(TupleTableSlot* slot, std::size_t i, const std::optional<std::int64_t>& v)
{
	if (v)
		set_int64(slot, i, *v);
	else
		set_null(slot, i);
}

/* uint64 (e.g. sequence_length) -> bigint, null when absent. */
inline void
set_uint64_opt_as_int64(TupleTableSlot* slot, std::size_t i,
						const std::optional<std::uint64_t>& v)
{
	if (v)
		set_int64(slot, i, static_cast<std::int64_t>(*v));
	else
		set_null(slot, i);
}

inline void
set_int32(TupleTableSlot* slot, std::size_t i, std::int32_t v)
{
	slot->tts_values[i] = Int32GetDatum(v);
	slot->tts_isnull[i] = false;
}

/* uint32 (e.g. accession version) -> int4, null when absent. */
inline void
set_uint32_opt_as_int32(TupleTableSlot* slot, std::size_t i,
						const std::optional<std::uint32_t>& v)
{
	if (v)
		set_int32(slot, i, static_cast<std::int32_t>(*v));
	else
		set_null(slot, i);
}

inline void
set_int16(TupleTableSlot* slot, std::size_t i, std::int16_t v)
{
	slot->tts_values[i] = Int16GetDatum(v);
	slot->tts_isnull[i] = false;
}

inline void
set_bool(TupleTableSlot* slot, std::size_t i, bool v)
{
	slot->tts_values[i] = BoolGetDatum(v);
	slot->tts_isnull[i] = false;
}

inline void
set_bool_opt(TupleTableSlot* slot, std::size_t i, const std::optional<bool>& v)
{
	if (v)
		set_bool(slot, i, *v);
	else
		set_null(slot, i);
}

/* external_timestamp is microseconds since 1970-01-01; PG TimestampTz is
 * microseconds since 2000-01-01. 946684800000000 = us between the two epochs. */
inline void
set_timestamptz(TupleTableSlot* slot, std::size_t i, prov::external_timestamp ts)
{
	const std::int64_t us_1970 = ts.time_since_epoch().count();
	const TimestampTz pg_ts =
		static_cast<TimestampTz>(us_1970 - INT64CONST(946684800000000));
	slot->tts_values[i] = TimestampTzGetDatum(pg_ts);
	slot->tts_isnull[i] = false;
}

/* external_date{year:int16, month:uint8, day:uint8} -> PG DateADT (days since
 * 2000-01-01), null when absent. */
inline void
set_date_opt(TupleTableSlot* slot, std::size_t i, const std::optional<prov::external_date>& d)
{
	if (!d)
	{
		set_null(slot, i);
		return;
	}
	const DateADT day = static_cast<DateADT>(
		date2j(d->year, d->month, d->day) - POSTGRES_EPOCH_JDATE);
	slot->tts_values[i] = DateADTGetDatum(day);
	slot->tts_isnull[i] = false;
}

/* ------------------------------------------------------------------------ */
/* jsonb helpers                                                             */
/* ------------------------------------------------------------------------ */

inline Datum
jsonb_datum_from_cstring(const char* cstr)
{
	return DirectFunctionCall1(jsonb_in, CStringGetDatum(cstr));
}

inline void
set_jsonb_from_string(TupleTableSlot* slot, std::size_t i, const std::string& json)
{
	slot->tts_values[i] = jsonb_datum_from_cstring(json.c_str());
	slot->tts_isnull[i] = false;
}

/* provenance is NOT NULL: always emit at least a minimal object identifying
 * the provider. Full provenance-hop serialization is a later refinement. */
inline void
set_provenance(TupleTableSlot* slot, std::size_t i, prov::provider_id provider)
{
	std::string json = "{\"provider\":\"";
	append_json_escaped(json, provider.value);
	json += "\"}";
	set_jsonb_from_string(slot, i, json);
}

/* provider_attributes_json is already a JSON document: pass it through jsonb_in
 * (copied to a NUL-terminated buffer). Guard: skip (null) anything that does
 * not begin with '{' or '['. */
template <std::size_t N>
inline void
set_provider_attributes(TupleTableSlot* slot, std::size_t i,
						const std::optional<prov::bounded_string<N>>& v)
{
	if (!v)
	{
		set_null(slot, i);
		return;
	}
	const std::string_view sv = v->view();
	if (sv.empty() || (sv.front() != '{' && sv.front() != '['))
	{
		set_null(slot, i);
		return;
	}
	set_jsonb_from_string(slot, i, std::string(sv));
}

/* ------------------------------------------------------------------------ */
/* array helpers                                                             */
/* ------------------------------------------------------------------------ */

inline void
set_bigint_array_opt(TupleTableSlot* slot, std::size_t i,
					 const std::optional<std::vector<std::int64_t>>& v)
{
	if (!v)
	{
		set_null(slot, i);
		return;
	}
	const std::size_t n = v->size();
	Datum* elems = static_cast<Datum*>(palloc(sizeof(Datum) * (n == 0 ? 1 : n)));
	for (std::size_t k = 0; k < n; ++k)
		elems[k] = Int64GetDatum((*v)[k]);
	ArrayType* arr =
		construct_array(elems, static_cast<int>(n), INT8OID, 8, true, 'd');
	slot->tts_values[i] = PointerGetDatum(arr);
	slot->tts_isnull[i] = false;
}

template <std::size_t N>
inline void
set_text_array_opt(TupleTableSlot* slot, std::size_t i,
				   const std::optional<std::vector<prov::bounded_string<N>>>& v)
{
	if (!v)
	{
		set_null(slot, i);
		return;
	}
	const std::size_t n = v->size();
	Datum* elems = static_cast<Datum*>(palloc(sizeof(Datum) * (n == 0 ? 1 : n)));
	for (std::size_t k = 0; k < n; ++k)
	{
		const std::string_view sv = (*v)[k].view();
		const char* p = sv.empty() ? "" : sv.data();
		elems[k] =
			PointerGetDatum(cstring_to_text_with_len(p, static_cast<int>(sv.size())));
	}
	ArrayType* arr =
		construct_array(elems, static_cast<int>(n), TEXTOID, -1, false, 'i');
	slot->tts_values[i] = PointerGetDatum(arr);
	slot->tts_isnull[i] = false;
}

/* ------------------------------------------------------------------------ */
/* jsonb array builders (catalog aliases / genes / locations)               */
/* ------------------------------------------------------------------------ */

inline void
set_aliases_jsonb(TupleTableSlot* slot, std::size_t i,
				  const std::optional<std::vector<prov::owning_record_key>>& aliases)
{
	if (!aliases)
	{
		set_null(slot, i);
		return;
	}
	std::string json = "[";
	bool first = true;
	for (const auto& alias : *aliases)
	{
		if (!first)
			json += ',';
		first = false;
		json += "{\"domain\":\"";
		append_json_escaped(json, alias.domain.view());
		json += "\",\"accession\":\"";
		append_json_escaped(json, alias.accession.view());
		json += "\"";
		if (alias.name_space)
		{
			json += ",\"namespace\":\"";
			append_json_escaped(json, alias.name_space->view());
			json += "\"";
		}
		json += "}";
	}
	json += "]";
	set_jsonb_from_string(slot, i, json);
}

inline void
set_genes_jsonb(TupleTableSlot* slot, std::size_t i,
				const std::optional<std::vector<prov::owning_gene_reference>>& genes)
{
	if (!genes)
	{
		set_null(slot, i);
		return;
	}
	std::string json = "[";
	bool first = true;
	for (const auto& gene : *genes)
	{
		if (!first)
			json += ',';
		first = false;
		json += "{\"namespace\":\"";
		append_json_escaped(json, gene.name_space.view());
		json += "\",\"id\":\"";
		append_json_escaped(json, gene.identifier.view());
		json += "\"";
		if (gene.symbol)
		{
			json += ",\"symbol\":\"";
			append_json_escaped(json, gene.symbol->view());
			json += "\"";
		}
		if (gene.name)
		{
			json += ",\"name\":\"";
			append_json_escaped(json, gene.name->view());
			json += "\"";
		}
		if (gene.type)
		{
			json += ",\"type\":\"";
			append_json_escaped(json, gene.type->view());
			json += "\"";
		}
		if (gene.locus_tag)
		{
			json += ",\"locus_tag\":\"";
			append_json_escaped(json, gene.locus_tag->view());
			json += "\"";
		}
		json += "}";
	}
	json += "]";
	set_jsonb_from_string(slot, i, json);
}

inline void
set_locations_jsonb(TupleTableSlot* slot, std::size_t i,
					const std::optional<std::vector<prov::owning_genomic_interval>>& locations)
{
	if (!locations)
	{
		set_null(slot, i);
		return;
	}
	std::string json = "[";
	bool first = true;
	for (const auto& loc : *locations)
	{
		if (!first)
			json += ',';
		first = false;
		json += "{\"sequence_alias\":\"";
		append_json_escaped(json, loc.sequence_alias.view());
		json += "\",\"start0\":";
		json += std::to_string(loc.start0);
		json += ",\"end0\":";
		json += std::to_string(loc.end0);
		json += ",\"strand\":";
		json += std::to_string(static_cast<int>(loc.orientation));
		if (loc.assembly_accession)
		{
			json += ",\"assembly_accession\":\"";
			append_json_escaped(json, loc.assembly_accession->view());
			json += "\"";
		}
		if (loc.assembly_database)
		{
			json += ",\"assembly_database\":\"";
			append_json_escaped(json, loc.assembly_database->view());
			json += "\"";
		}
		json += "}";
	}
	json += "]";
	set_jsonb_from_string(slot, i, json);
}

/* ======================================================================== */
/* Per-entity fillers. Column order mirrors bioseq_external--1.0.sql exactly. */
/* ======================================================================== */

/* taxonomy (13). */
void
fill_taxonomy(const prov::owning_taxonomy_row& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == TAXONOMY_NATTS);

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_int64_opt(slot, 1, row.taxon_id);                           /* taxon_id */
	set_text(slot, 2, row.provider_taxon_id.view());                /* provider_taxon_id */
	set_text_opt(slot, 3, row.scientific_name);                     /* scientific_name */
	set_text_opt(slot, 4, row.common_name);                         /* common_name */
	set_text_opt(slot, 5, row.rank);                                /* rank */
	set_int64_opt(slot, 6, row.parent_taxon_id);                    /* parent_taxon_id */
	set_bigint_array_opt(slot, 7, row.lineage);                     /* lineage bigint[] */
	set_text_array_opt(slot, 8, row.synonyms);                      /* synonyms text[] */
	set_provider_attributes(slot, 9, row.provider_attributes_json); /* provider_attributes */
	set_text_opt(slot, 10, row.audit.logical_provider_version);     /* provider_version */
	set_provenance(slot, 11, row.provider);                         /* provenance NOT NULL */
	set_timestamptz(slot, 12, row.audit.completed_at);              /* retrieved_at NOT NULL */
}

/* catalog (35). */
void
fill_catalog(const prov::owning_catalog_row& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == CATALOG_NATTS);

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_text(slot, 1, row.key.domain.view());                       /* domain */
	set_text(slot, 2, prov::to_string(row.kind));                   /* entity_kind */
	if (row.molecule)                                               /* molecule_kind */
		set_text(slot, 3, prov::to_string(*row.molecule));
	else
		set_null(slot, 3);
	set_text_opt(slot, 4, row.key.name_space);                      /* namespace */
	set_text(slot, 5, row.key.original_identifier.view());          /* original_identifier */
	set_text(slot, 6, row.key.accession.view());                    /* accession */
	set_uint32_opt_as_int32(slot, 7, row.key.version);              /* accession_version */
	set_aliases_jsonb(slot, 8, row.aliases);                        /* aliases jsonb */
	set_int64_opt(slot, 9, row.taxon_id);                           /* taxon_id */
	set_text_opt(slot, 10, row.scientific_name);                    /* scientific_name */
	set_text_opt(slot, 11, row.title);                              /* title */
	set_genes_jsonb(slot, 12, row.genes);                           /* genes jsonb */
	set_text_array_opt(slot, 13, row.keywords);                     /* keywords text[] */
	set_locations_jsonb(slot, 14, row.locations);                   /* locations jsonb */
	set_int64_opt(slot, 15, row.host_taxon_id);                     /* host_taxon_id */
	set_text_opt(slot, 16, row.geography);                          /* geography */
	set_text_opt(slot, 17, row.lineage_name);                       /* lineage_name */
	set_uint64_opt_as_int64(slot, 18, row.length);                  /* sequence_length */
	set_text_opt(slot, 19, row.assembly_accession);                 /* assembly_accession */
	set_text_opt(slot, 20, row.assembly_database);                  /* assembly_database */
	set_text_opt(slot, 21, row.assembly_source);                    /* assembly_source */
	set_text_opt(slot, 22, row.assembly_level);                     /* assembly_level */
	set_text_opt(slot, 23, row.assembly_status);                    /* assembly_status */
	set_date_opt(slot, 24, row.release_date);                       /* release_date */
	set_date_opt(slot, 25, row.update_date);                        /* update_date */
	set_bool_opt(slot, 26, row.is_reference);                       /* is_reference */
	set_bool_opt(slot, 27, row.is_reviewed);                        /* is_reviewed */
	set_bool_opt(slot, 28, row.is_annotated);                       /* is_annotated */
	set_bool_opt(slot, 29, row.is_fragment);                        /* is_fragment */
	set_bool(slot, 30, row.retrievable);                            /* retrievable NOT NULL */
	set_provider_attributes(slot, 31, row.provider_attributes_json);/* provider_attributes */
	set_text_opt(slot, 32, row.audit.logical_provider_version);     /* provider_version */
	set_provenance(slot, 33, row.provider);                         /* provenance NOT NULL */
	set_timestamptz(slot, 34, row.audit.completed_at);              /* retrieved_at NOT NULL */
}

/* record_genes (17). One row per gene in the parent catalog row. */
void
fill_record_gene(const prov::owning_catalog_row& row, std::size_t sub_index,
				 TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == RECORD_GENES_NATTS);
	const prov::owning_gene_reference& gene = (*row.genes)[sub_index];

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_text(slot, 1, row.key.domain.view());                       /* domain */
	set_text_opt(slot, 2, row.key.name_space);                      /* namespace */
	set_text(slot, 3, row.key.original_identifier.view());          /* original_identifier */
	set_text(slot, 4, row.key.accession.view());                    /* accession */
	set_uint32_opt_as_int32(slot, 5, row.key.version);              /* accession_version */
	set_int32(slot, 6, static_cast<std::int32_t>(sub_index));       /* gene_index NOT NULL */
	set_int32(slot, 7, static_cast<std::int32_t>(row.genes->size()));/* gene_count NOT NULL */
	set_text(slot, 8, gene.name_space.view());                      /* gene_namespace NOT NULL */
	set_text(slot, 9, gene.identifier.view());                      /* gene_id NOT NULL */
	set_text_opt(slot, 10, gene.symbol);                            /* gene_symbol */
	set_text_opt(slot, 11, gene.name);                              /* gene_name */
	set_text_opt(slot, 12, gene.type);                              /* gene_type */
	set_text_opt(slot, 13, gene.locus_tag);                         /* locus_tag */
	set_text_opt(slot, 14, row.audit.logical_provider_version);     /* provider_version */
	set_provenance(slot, 15, row.provider);                         /* provenance NOT NULL */
	set_timestamptz(slot, 16, row.audit.completed_at);              /* retrieved_at NOT NULL */
}

/* record_locations (17). One row per genomic interval in the parent catalog. */
void
fill_record_location(const prov::owning_catalog_row& row, std::size_t sub_index,
					 TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == RECORD_LOCATIONS_NATTS);
	const prov::owning_genomic_interval& loc = (*row.locations)[sub_index];

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_text(slot, 1, row.key.domain.view());                       /* domain */
	set_text_opt(slot, 2, row.key.name_space);                      /* namespace */
	set_text(slot, 3, row.key.original_identifier.view());          /* original_identifier */
	set_text(slot, 4, row.key.accession.view());                    /* accession */
	set_uint32_opt_as_int32(slot, 5, row.key.version);              /* accession_version */
	set_int32(slot, 6, static_cast<std::int32_t>(sub_index));       /* location_index NOT NULL */
	set_int32(slot, 7, static_cast<std::int32_t>(row.locations->size()));/* location_count NOT NULL */
	set_text_opt(slot, 8, loc.assembly_accession);                  /* assembly_accession */
	set_text_opt(slot, 9, loc.assembly_database);                   /* assembly_database */
	set_text(slot, 10, loc.sequence_alias.view());                  /* sequence_alias NOT NULL */
	set_int64(slot, 11, static_cast<std::int64_t>(loc.start0));     /* start0 NOT NULL */
	set_int64(slot, 12, static_cast<std::int64_t>(loc.end0));       /* end0 NOT NULL */
	set_int16(slot, 13, static_cast<std::int16_t>(loc.orientation));/* strand NOT NULL */
	set_text_opt(slot, 14, row.audit.logical_provider_version);     /* provider_version */
	set_provenance(slot, 15, row.provider);                         /* provenance NOT NULL */
	set_timestamptz(slot, 16, row.audit.completed_at);              /* retrieved_at NOT NULL */
}

/* Build a bioseq.seq varlena Datum from raw nucleotide letters. dna5 preserves
 * A/C/G/T/N (U->T; any other IUPAC/junk byte -> N); the factory pallocs in
 * CurrentMemoryContext and writes the varlena header, mirroring bioseq.seq_in. */
inline void
set_seq(TupleTableSlot* slot, std::size_t i, std::string_view bases)
{
	pgseq_t* header =
		::bioseq::simd::active_target::stdc_convert_from_cstr<pgseq_t, seq::dna5>(
			static_cast<std::uint8_t>(1),
			static_cast<pgseq_t::bp_size_t>(bases.size()),
			bases.empty() ? "" : bases.data(),
			pg_alloc);
	if (header == nullptr)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("bioseq_external: out of memory building bioseq.seq")));
	slot->tts_values[i] = PointerGetDatum(header);
	slot->tts_isnull[i] = false;
}

/* sequences (18). */
void
fill_sequence(const prov::owning_sequence_payload& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == SEQUENCE_NATTS);

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_text(slot, 1, row.key.domain.view());                       /* domain */
	set_text_opt(slot, 2, row.key.name_space);                      /* namespace */
	set_text(slot, 3, row.key.original_identifier.view());          /* original_identifier */
	set_text(slot, 4, row.key.accession.view());                    /* accession */
	set_uint32_opt_as_int32(slot, 5, row.key.version);              /* accession_version */
	set_text(slot, 6, prov::to_string(row.molecule));               /* molecule_kind NOT NULL */
	set_null(slot, 7);                                              /* taxon_id */
	set_text_opt(slot, 8, row.assembly_accession);                  /* assembly_accession */
	set_text_opt(slot, 9, row.assembly_database);                   /* assembly_database */
	set_text_opt(slot, 10, row.chromosome);                         /* chromosome */
	set_uint64_opt_as_int64(slot, 11, row.start0);                  /* start0 */
	set_uint64_opt_as_int64(slot, 12, row.end0);                    /* end0 */
	set_int16(slot, 13, static_cast<std::int16_t>(row.strand));     /* strand NOT NULL */
	set_seq(slot, 14, row.bases);                                   /* sequence bioseq.seq NOT NULL */
	set_text_opt(slot, 15, row.audit.logical_provider_version);     /* provider_version */
	set_provenance(slot, 16, row.provider);                         /* provenance NOT NULL */
	set_timestamptz(slot, 17, row.audit.completed_at);              /* retrieved_at NOT NULL */
}

/* sequence_chunks (21). */
void
fill_sequence_chunk(const prov::owning_sequence_payload& row,
					TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == SEQUENCE_CHUNK_NATTS);

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_text(slot, 1, row.key.domain.view());                       /* domain */
	set_text_opt(slot, 2, row.key.name_space);                      /* namespace */
	set_text(slot, 3, row.key.original_identifier.view());          /* original_identifier */
	set_text(slot, 4, row.key.accession.view());                    /* accession */
	set_uint32_opt_as_int32(slot, 5, row.key.version);              /* accession_version */
	set_text(slot, 6, prov::to_string(row.molecule));               /* molecule_kind NOT NULL */
	set_null(slot, 7);                                              /* taxon_id */
	set_text_opt(slot, 8, row.assembly_accession);                  /* assembly_accession */
	set_text_opt(slot, 9, row.assembly_database);                   /* assembly_database */
	set_text_opt(slot, 10, row.chromosome);                         /* chromosome */
	set_uint64_opt_as_int64(slot, 11, row.start0);                  /* start0 */
	set_uint64_opt_as_int64(slot, 12, row.end0);                    /* end0 */
	set_int16(slot, 13, static_cast<std::int16_t>(row.strand));     /* strand NOT NULL */
	set_int64(slot, 14, static_cast<std::int64_t>(row.chunk_index));/* chunk_index NOT NULL */
	set_int64(slot, 15, static_cast<std::int64_t>(row.base_offset));/* base_offset NOT NULL */
	set_seq(slot, 16, row.bases);                                   /* sequence_chunk NOT NULL */
	set_bool(slot, 17, row.end_of_record);                          /* end_of_record NOT NULL */
	set_text_opt(slot, 18, row.audit.logical_provider_version);     /* provider_version */
	set_provenance(slot, 19, row.provider);                         /* provenance NOT NULL */
	set_timestamptz(slot, 20, row.audit.completed_at);              /* retrieved_at NOT NULL */
}

} // namespace

/* ======================================================================== */
/* Public entry point definitions                                            */
/* ======================================================================== */

std::size_t
bioseq_external_message_row_count(BioseqExternalEntityKind entity,
								  const prov::owning_provider_message& msg)
{
	switch (entity)
	{
		case EXTERNAL_ENTITY_TAXONOMY:
			return std::get_if<prov::owning_taxonomy_row>(&msg) != nullptr ? 1u : 0u;

		case EXTERNAL_ENTITY_CATALOG:
			return std::get_if<prov::owning_catalog_row>(&msg) != nullptr ? 1u : 0u;

		case EXTERNAL_ENTITY_RECORD_GENE:
			if (const auto* row = std::get_if<prov::owning_catalog_row>(&msg))
				return row->genes ? row->genes->size() : 0u;
			return 0u;

		case EXTERNAL_ENTITY_RECORD_LOCATION:
			if (const auto* row = std::get_if<prov::owning_catalog_row>(&msg))
				return row->locations ? row->locations->size() : 0u;
			return 0u;

		case EXTERNAL_ENTITY_SEQUENCE:
		case EXTERNAL_ENTITY_SEQUENCE_CHUNK:
			return std::get_if<prov::owning_sequence_payload>(&msg) != nullptr
					   ? 1u
					   : 0u;

		case EXTERNAL_ENTITY_UNSPECIFIED:
		default:
			return 0u;
	}
}

bool
bioseq_external_fill_slot(BioseqExternalEntityKind entity,
						  const prov::owning_provider_message& msg,
						  std::size_t sub_index, TupleTableSlot* slot)
{
	switch (entity)
	{
		case EXTERNAL_ENTITY_TAXONOMY:
		{
			const auto* row = std::get_if<prov::owning_taxonomy_row>(&msg);
			if (row == nullptr)
				return false;
			fill_taxonomy(*row, slot);
			return true;
		}

		case EXTERNAL_ENTITY_CATALOG:
		{
			const auto* row = std::get_if<prov::owning_catalog_row>(&msg);
			if (row == nullptr)
				return false;
			fill_catalog(*row, slot);
			return true;
		}

		case EXTERNAL_ENTITY_RECORD_GENE:
		{
			const auto* row = std::get_if<prov::owning_catalog_row>(&msg);
			if (row == nullptr || !row->genes || sub_index >= row->genes->size())
				return false;
			fill_record_gene(*row, sub_index, slot);
			return true;
		}

		case EXTERNAL_ENTITY_RECORD_LOCATION:
		{
			const auto* row = std::get_if<prov::owning_catalog_row>(&msg);
			if (row == nullptr || !row->locations || sub_index >= row->locations->size())
				return false;
			fill_record_location(*row, sub_index, slot);
			return true;
		}

		case EXTERNAL_ENTITY_SEQUENCE:
		{
			const auto* row = std::get_if<prov::owning_sequence_payload>(&msg);
			if (row == nullptr || sub_index != 0)
				return false;
			fill_sequence(*row, slot);
			return true;
		}

		case EXTERNAL_ENTITY_SEQUENCE_CHUNK:
		{
			const auto* row = std::get_if<prov::owning_sequence_payload>(&msg);
			if (row == nullptr || sub_index != 0)
				return false;
			fill_sequence_chunk(*row, slot);
			return true;
		}

		case EXTERNAL_ENTITY_UNSPECIFIED:
		default:
			return false;
	}
}
