/// @brief Materializes normalized provider messages into virtual tuples.
module;
#include <bioseq/pg_compat.h>

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

export module bioseq.postgres.external.row_writer;

import bioseq.postgres.external.relation;
import bioseq.external.provider.base;
import bioseq.external.provider.model;
import bioseq.external.provider.messages;
import bioseq.common.seqan_adapter;
import bioseq.sequences.simd;
import bioseq.postgres.types.compact_sequence;

namespace bioseq::postgres::external {
namespace provider = bioseq::external::provider;
}

export namespace bioseq::postgres::external {

/**
 * @brief Returns the number of SQL rows emitted from one provider message.
 *
 * Catalog messages expand into gene or location rows for the corresponding
 * relation. Non-row messages and mismatched variants return zero.
 */
[[nodiscard]] std::size_t message_row_count(
	relation_entity entity,
	const provider::owning_provider_message &message) noexcept;

/**
 * @brief Materializes one message sub-row into a cleared virtual tuple slot.
 * @return `true` when the message and row index match the relation entity.
 *
 * On success every attribute is assigned either a datum or SQL null. Allocated
 * values use PostgreSQL's current memory context and remain valid for
 * `ExecStoreVirtualTuple`.
 */
bool materialize_message_row(
	relation_entity entity,
	const provider::owning_provider_message &message,
	std::size_t row_index, TupleTableSlot *slot);

} // namespace bioseq::postgres::external

/* ======================================================================== */
/* Column counts (authoritative: external/sql_catalog.cppm)                 */
/* ======================================================================== */

namespace bioseq::postgres::external {

namespace {

/* [[maybe_unused]]: Assert() is a no-op in non-assert PG builds, which would
 * otherwise leave these unreferenced under -Wunused-const-variable. */
[[maybe_unused]] constexpr int taxonomy_column_count = 13;
[[maybe_unused]] constexpr int catalog_column_count = 35;
[[maybe_unused]] constexpr int record_gene_column_count = 17;
[[maybe_unused]] constexpr int record_location_column_count = 17;
[[maybe_unused]] constexpr int sequence_column_count = 18;
[[maybe_unused]] constexpr int sequence_chunk_column_count = 21;

/* ======================================================================== */
/* Datum construction helpers (all allocate in CurrentMemoryContext)         */
/* ======================================================================== */

/* Escape a run of (identifier) text into a JSON string body: quote and
 * backslash per JSON, control bytes as \u00XX so jsonb_in never rejects the
 * value. Bytes >= 0x80 pass through unchanged (valid UTF-8 continuation). */
inline void
append_json_escaped(std::string& out, std::string_view sv)
{
	static constexpr char hex_digits[] = "0123456789abcdef";
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
					out += hex_digits[(c >> 4) & 0x0F];
					out += hex_digits[c & 0x0F];
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
			 const std::optional<provider::bounded_string<N>>& v)
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
set_timestamptz(TupleTableSlot* slot, std::size_t i, provider::external_timestamp ts)
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
set_date_opt(TupleTableSlot* slot, std::size_t i, const std::optional<provider::external_date>& d)
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

/* The SQL provenance column always carries the provider identity. */
inline void
set_provenance(TupleTableSlot* slot, std::size_t i, provider::provider_id provider)
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
						const std::optional<provider::bounded_string<N>>& v)
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
				   const std::optional<std::vector<provider::bounded_string<N>>>& v)
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

template <class value_type, class append_value_type>
void
set_json_array_opt(TupleTableSlot *slot, std::size_t index,
				   const std::optional<std::vector<value_type>> &values,
				   append_value_type append_value)
{
	if (!values)
	{
		set_null(slot, index);
		return;
	}

	std::string json{"["};
	bool needs_separator = false;
	for (const value_type &value : *values)
	{
		if (needs_separator)
		{
			json += ',';
		}
		needs_separator = true;
		append_value(json, value);
	}
	json += ']';
	set_jsonb_from_string(slot, index, json);
}

inline void
set_aliases_jsonb(TupleTableSlot* slot, std::size_t i,
				  const std::optional<std::vector<provider::owning_record_key>>& aliases)
{
	set_json_array_opt(
		slot, i, aliases,
		[](std::string &json, const provider::owning_record_key &alias)
		{
			json += "{\"domain\":\"";
			append_json_escaped(json, alias.domain.view());
			json += "\",\"accession\":\"";
			append_json_escaped(json, alias.accession.view());
			json += '"';
			if (alias.name_space)
			{
				json += ",\"namespace\":\"";
				append_json_escaped(json, alias.name_space->view());
				json += '"';
			}
			json += '}';
		});
}

inline void
set_genes_jsonb(TupleTableSlot* slot, std::size_t i,
				const std::optional<std::vector<provider::owning_gene_reference>>& genes)
{
	set_json_array_opt(
		slot, i, genes,
		[](std::string &json, const provider::owning_gene_reference &gene)
		{
			json += "{\"namespace\":\"";
			append_json_escaped(json, gene.name_space.view());
			json += "\",\"id\":\"";
			append_json_escaped(json, gene.identifier.view());
			json += '"';
			const auto append_optional = [&json](
				std::string_view name, const auto &value)
			{
				if (value)
				{
					json += ",\"";
					json += name;
					json += "\":\"";
					append_json_escaped(json, value->view());
					json += '"';
				}
			};
			append_optional("symbol", gene.symbol);
			append_optional("name", gene.name);
			append_optional("type", gene.type);
			append_optional("locus_tag", gene.locus_tag);
			json += '}';
		});
}

inline void
set_locations_jsonb(TupleTableSlot* slot, std::size_t i,
					const std::optional<std::vector<provider::owning_genomic_interval>>& locations)
{
	set_json_array_opt(
		slot, i, locations,
		[](std::string &json,
		   const provider::owning_genomic_interval &location)
		{
			json += "{\"sequence_alias\":\"";
			append_json_escaped(json, location.sequence_alias.view());
			json += "\",\"start0\":";
			json += std::to_string(location.start0);
			json += ",\"end0\":";
			json += std::to_string(location.end0);
			json += ",\"strand\":";
			json += std::to_string(static_cast<int>(location.orientation));
			const auto append_optional = [&json](
				std::string_view name, const auto &value)
			{
				if (value)
				{
					json += ",\"";
					json += name;
					json += "\":\"";
					append_json_escaped(json, value->view());
					json += '"';
				}
			};
			append_optional("assembly_accession",
							location.assembly_accession);
			append_optional("assembly_database", location.assembly_database);
			json += '}';
		});
}

template <class row_type>
void
set_record_identity(TupleTableSlot *slot, const row_type &row)
{
	set_text(slot, 0, row.provider.value);
	set_text(slot, 1, row.key.domain.view());
	set_text_opt(slot, 2, row.key.name_space);
	set_text(slot, 3, row.key.original_identifier.view());
	set_text(slot, 4, row.key.accession.view());
	set_uint32_opt_as_int32(slot, 5, row.key.version);
}

template <class row_type>
void
set_audit_columns(TupleTableSlot *slot, const row_type &row,
				  std::size_t version_column,
				  std::size_t provenance_column,
				  std::size_t retrieved_at_column)
{
	set_text_opt(slot, version_column, row.audit.logical_provider_version);
	set_provenance(slot, provenance_column, row.provider);
	set_timestamptz(slot, retrieved_at_column, row.audit.completed_at);
}

void
set_sequence_region(TupleTableSlot *slot,
					const provider::owning_sequence_payload &row)
{
	set_record_identity(slot, row);
	set_text(slot, 6, provider::to_string(row.molecule));
	set_null(slot, 7);
	set_text_opt(slot, 8, row.assembly_accession);
	set_text_opt(slot, 9, row.assembly_database);
	set_text_opt(slot, 10, row.chromosome);
	set_uint64_opt_as_int64(slot, 11, row.start0);
	set_uint64_opt_as_int64(slot, 12, row.end0);
	set_int16(slot, 13, static_cast<std::int16_t>(row.strand));
}

/* taxonomy (13). */
void
fill_taxonomy(const provider::owning_taxonomy_row& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == taxonomy_column_count);

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
	set_audit_columns(slot, row, 10, 11, 12);
}

/* catalog (35). */
void
fill_catalog(const provider::owning_catalog_row& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == catalog_column_count);

	set_text(slot, 0, row.provider.value);                          /* provider */
	set_text(slot, 1, row.key.domain.view());                       /* domain */
	set_text(slot, 2, provider::to_string(row.kind));                   /* entity_kind */
	if (row.molecule)                                               /* molecule_kind */
		set_text(slot, 3, provider::to_string(*row.molecule));
	else
		set_null(slot, 3);
	set_text_opt(slot, 4, row.key.name_space);                      /* namespace_name */
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
	set_audit_columns(slot, row, 32, 33, 34);
}

/* record_genes (17). One row per gene in the parent catalog row. */
void
fill_record_gene(const provider::owning_catalog_row& row, std::size_t sub_index,
				 TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == record_gene_column_count);
	const provider::owning_gene_reference& gene = (*row.genes)[sub_index];

	set_record_identity(slot, row);
	set_int32(slot, 6, static_cast<std::int32_t>(sub_index));       /* gene_index NOT NULL */
	set_int32(slot, 7, static_cast<std::int32_t>(row.genes->size()));/* gene_count NOT NULL */
	set_text(slot, 8, gene.name_space.view());                      /* gene_namespace NOT NULL */
	set_text(slot, 9, gene.identifier.view());                      /* gene_id NOT NULL */
	set_text_opt(slot, 10, gene.symbol);                            /* gene_symbol */
	set_text_opt(slot, 11, gene.name);                              /* gene_name */
	set_text_opt(slot, 12, gene.type);                              /* gene_type */
	set_text_opt(slot, 13, gene.locus_tag);                         /* locus_tag */
	set_audit_columns(slot, row, 14, 15, 16);
}

/* record_locations (17). One row per genomic interval in the parent catalog. */
void
fill_record_location(const provider::owning_catalog_row& row, std::size_t sub_index,
					 TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == record_location_column_count);
	const provider::owning_genomic_interval& loc = (*row.locations)[sub_index];

	set_record_identity(slot, row);
	set_int32(slot, 6, static_cast<std::int32_t>(sub_index));       /* location_index NOT NULL */
	set_int32(slot, 7, static_cast<std::int32_t>(row.locations->size()));/* location_count NOT NULL */
	set_text_opt(slot, 8, loc.assembly_accession);                  /* assembly_accession */
	set_text_opt(slot, 9, loc.assembly_database);                   /* assembly_database */
	set_text(slot, 10, loc.sequence_alias.view());                  /* sequence_alias NOT NULL */
	set_int64(slot, 11, static_cast<std::int64_t>(loc.start0));     /* start0 NOT NULL */
	set_int64(slot, 12, static_cast<std::int64_t>(loc.end0));       /* end0 NOT NULL */
	set_int16(slot, 13, static_cast<std::int16_t>(loc.orientation));/* strand NOT NULL */
	set_audit_columns(slot, row, 14, 15, 16);
}

/* Builds a bioseq.seq datum with the configured unconstrained-input alphabet. */
inline void
set_sequence_value(TupleTableSlot* slot, std::size_t i,
				   std::string_view bases)
{
	bioseq::pg_types::seq* header =
		::bioseq::simd::active_target::stdc_convert_from_cstr<
			bioseq::pg_types::seq, pgseq_unconstrained_input_alphabet>(
			pgseq_normalize_storage_unit_byte_width(1u),
			static_cast<bioseq::pg_types::seq::bp_size_t>(bases.size()),
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
fill_sequence(const provider::owning_sequence_payload& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == sequence_column_count);

	set_sequence_region(slot, row);
	set_sequence_value(slot, 14, row.bases);
	set_audit_columns(slot, row, 15, 16, 17);
}

/* sequence_chunks (21). */
void
fill_sequence_chunk(const provider::owning_sequence_payload& row,
					TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == sequence_chunk_column_count);

	set_sequence_region(slot, row);
	set_int64(slot, 14, static_cast<std::int64_t>(row.chunk_index));/* chunk_index NOT NULL */
	set_int64(slot, 15, static_cast<std::int64_t>(row.base_offset));/* base_offset NOT NULL */
	set_sequence_value(slot, 16, row.bases);
	set_bool(slot, 17, row.end_of_record);                          /* end_of_record NOT NULL */
	set_audit_columns(slot, row, 18, 19, 20);
}

} // namespace

/* ======================================================================== */
/* Public entry point definitions                                            */
/* ======================================================================== */

std::size_t
message_row_count(relation_entity entity,
				  const provider::owning_provider_message &message) noexcept
{
	switch (entity)
	{
		case relation_entity::taxonomy:
			return std::get_if<provider::owning_taxonomy_row>(&message) != nullptr
					   ? 1u
					   : 0u;

		case relation_entity::catalog:
			return std::get_if<provider::owning_catalog_row>(&message) != nullptr
					   ? 1u
					   : 0u;

		case relation_entity::record_gene:
			if (const auto *row =
					std::get_if<provider::owning_catalog_row>(&message))
				return row->genes ? row->genes->size() : 0u;
			return 0u;

		case relation_entity::record_location:
			if (const auto *row =
					std::get_if<provider::owning_catalog_row>(&message))
				return row->locations ? row->locations->size() : 0u;
			return 0u;

		case relation_entity::sequence:
		case relation_entity::sequence_chunk:
			return std::get_if<provider::owning_sequence_payload>(&message) != nullptr
					   ? 1u
					   : 0u;

		case relation_entity::unspecified:
		default:
			return 0u;
	}
}

bool
materialize_message_row(relation_entity entity,
						const provider::owning_provider_message &message,
						std::size_t row_index, TupleTableSlot *slot)
{
	switch (entity)
	{
		case relation_entity::taxonomy:
		{
			const auto *row =
				std::get_if<provider::owning_taxonomy_row>(&message);
			if (row == nullptr)
				return false;
			fill_taxonomy(*row, slot);
			return true;
		}

		case relation_entity::catalog:
		{
			const auto *row =
				std::get_if<provider::owning_catalog_row>(&message);
			if (row == nullptr)
				return false;
			fill_catalog(*row, slot);
			return true;
		}

		case relation_entity::record_gene:
		{
			const auto *row =
				std::get_if<provider::owning_catalog_row>(&message);
			if (row == nullptr || !row->genes || row_index >= row->genes->size())
				return false;
			fill_record_gene(*row, row_index, slot);
			return true;
		}

		case relation_entity::record_location:
		{
			const auto *row =
				std::get_if<provider::owning_catalog_row>(&message);
			if (row == nullptr || !row->locations ||
				row_index >= row->locations->size())
				return false;
			fill_record_location(*row, row_index, slot);
			return true;
		}

		case relation_entity::sequence:
		{
			const auto *row =
				std::get_if<provider::owning_sequence_payload>(&message);
			if (row == nullptr || row_index != 0)
				return false;
			fill_sequence(*row, slot);
			return true;
		}

		case relation_entity::sequence_chunk:
		{
			const auto *row =
				std::get_if<provider::owning_sequence_payload>(&message);
			if (row == nullptr || row_index != 0)
				return false;
			fill_sequence_chunk(*row, slot);
			return true;
		}

		case relation_entity::unspecified:
		default:
			return false;
	}
}

} // namespace bioseq::postgres::external
