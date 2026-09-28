/**
 * @file row_writer.cppm
 * @brief Converts provider messages into PostgreSQL rows.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/builtins.h"     /* cstring_to_text_with_len, CStringGetTextDatum */
#include "utils/jsonb.h"        /* Jsonb and jsonb_in declarations */
#include "utils/timestamp.h"    /* TimestampTz, TimestampTzGetDatum */
#include "utils/date.h"         /* DateADT, DateADTGetDatum */
#include "utils/datetime.h"     /* date2j is declared here, separately from utils/date.h. */
#include "utils/array.h"        /* construct_array */
#include "catalog/pg_type_d.h"  /* INT8OID, TEXTOID */
}

#include <array>
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
import bioseq.postgres.types.compact_sequence;

namespace bioseq::postgres::external {
namespace provider = bioseq::external::provider;
}

/* ======================================================================== */
/* Column counts must match external/sql_catalog.cppm. */
/* ======================================================================== */

namespace bioseq::postgres::external {

namespace {

/* These constants are used only by Assert(). maybe_unused prevents warnings
 * in PostgreSQL builds that disable assertions. */
[[maybe_unused]] constexpr int k_taxonomy_column_count = 13;
[[maybe_unused]] constexpr int k_catalog_column_count = 35;
[[maybe_unused]] constexpr int k_record_gene_column_count = 17;
[[maybe_unused]] constexpr int k_record_location_column_count = 17;
[[maybe_unused]] constexpr int k_sequence_column_count = 18;
[[maybe_unused]] constexpr int k_sequence_chunk_column_count = 21;

/* ======================================================================== */
/* PostgreSQL value conversion helpers. */
/* ======================================================================== */

/**
 * @brief Appends text with JSON string escaping, without surrounding quotes.
 *
 * Escapes quotation marks, backslashes, and  control bytes. Bytes at or above
 * 0x80 pass through unchanged.
 */
inline void
append_json_escaped(std::string& output, std::string_view text)
{
	static constexpr auto k_hex_digits = std::to_array("0123456789abcdef");
	static_assert(k_hex_digits.size() == 17u);
	static_assert(sizeof(k_hex_digits) == sizeof("0123456789abcdef"));
	static_assert(alignof(decltype(k_hex_digits)) == alignof(char));
	for (const unsigned char byte_value : text)
	{
		switch (byte_value)
		{
			case '"':  output += "\\\""; break;
			case '\\': output += "\\\\"; break;
			case '\n': output += "\\n"; break;
			case '\r': output += "\\r"; break;
			case '\t': output += "\\t"; break;
			default:
				if (byte_value < 0x20)
				{
					output += "\\u00";
					output += k_hex_digits[(byte_value >> 4) & 0x0F];
					output += k_hex_digits[byte_value & 0x0F];
				}
				else
				{
					output += static_cast<char>(byte_value);
				}
		}
	}
}

/** @brief Sets one tuple column to SQL NULL. */
inline void
set_null(TupleTableSlot* slot, std::size_t column_index)
{
	slot->tts_isnull[column_index] = true;
	slot->tts_values[column_index] = static_cast<Datum>(0);
}

/** @brief Copies text into a tuple column using the current PostgreSQL memory context. */
inline void
set_text(TupleTableSlot* slot, std::size_t column_index, std::string_view text)
{
	const char* const text_bytes = text.empty() ? "" : text.data();
	slot->tts_values[column_index] = PointerGetDatum(
		cstring_to_text_with_len(text_bytes, static_cast<int>(text.size())));
	slot->tts_isnull[column_index] = false;
}

/**
 * @brief Writes an optional bounded string as text or SQL null.
 * @tparam p_capacity Maximum bytes accepted by the provider field.
 */
template <std::size_t p_capacity>
inline void
set_text_opt(TupleTableSlot* slot, std::size_t column_index,
			 const std::optional<provider::bounded_string<p_capacity>>& value)
{
	if (value)
		set_text(slot, column_index, value->view());
	else
		set_null(slot, column_index);
}

/** @brief Writes a signed 64-bit PostgreSQL integer attribute. */
inline void
set_int64(TupleTableSlot* slot, std::size_t column_index, std::int64_t value)
{
	slot->tts_values[column_index] = Int64GetDatum(value);
	slot->tts_isnull[column_index] = false;
}

/** @brief Writes an optional signed 64-bit integer or SQL null. */
inline void
set_int64_opt(TupleTableSlot* slot, std::size_t column_index,
			  const std::optional<std::int64_t>& value)
{
	if (value)
		set_int64(slot, column_index, *value);
	else
		set_null(slot, column_index);
}

/** @brief Casts an optional unsigned 64-bit valde to SQL bigint, or writes NULL. */
inline void
set_uint64_opt_as_int64(TupleTableSlot* slot, std::size_t column_index,
						const std::optional<std::uint64_t>& value)
{
	if (value)
		set_int64(slot, column_index, static_cast<std::int64_t>(*value));
	else
		set_null(slot, column_index);
}

/** @brief Writes a signed 32-bit PostgreSQL integer attribute. */
inline void
set_int32(TupleTableSlot* slot, std::size_t column_index, std::int32_t value)
{
	slot->tts_values[column_index] = Int32GetDatum(value);
	slot->tts_isnull[column_index] = false;
}

/** @brief Casts an optional unsigned 32-bit value to SQL integer, or writes NULL. */
inline void
set_uint32_opt_as_int32(TupleTableSlot* slot, std::size_t column_index,
						const std::optional<std::uint32_t>& value)
{
	if (value)
		set_int32(slot, column_index, static_cast<std::int32_t>(*value));
	else
		set_null(slot, column_index);
}

/** @brief Writes a signed 16-bit PostgreSQL integer attribute. */
inline void
set_int16(TupleTableSlot* slot, std::size_t column_index, std::int16_t value)
{
	slot->tts_values[column_index] = Int16GetDatum(value);
	slot->tts_isnull[column_index] = false;
}

/** @brief Writes a non-null PostgreSQL Boolean attribute. */
inline void
set_bool(TupleTableSlot* slot, std::size_t column_index, bool value)
{
	slot->tts_values[column_index] = BoolGetDatum(value);
	slot->tts_isnull[column_index] = false;
}

/** @brief Writes an optional Boolean or SQL null. */
inline void
set_bool_opt(TupleTableSlot* slot, std::size_t column_index,
			 const std::optional<bool>& value)
{
	if (value)
		set_bool(slot, column_index, *value);
	else
		set_null(slot, column_index);
}

/**
 * @brief Converts a provider timestamp to PostgreSQL timestamptz.
 *
 * Both count microseconds. Provider timestamps start at 1970-01-01, while
 * PostgreSQL timestamps start at 2000-01-01.
 */
inline void
set_timestamptz(TupleTableSlot* slot, std::size_t column_index,
				provider::external_timestamp timestamp)
{
	const std::int64_t unix_epoch_microseconds = timestamp.time_since_epoch().count();
	const TimestampTz postgres_timestamp = static_cast<TimestampTz>(
		unix_epoch_microseconds - INT64CONST(946684800000000));
	slot->tts_values[column_index] = TimestampTzGetDatum(postgres_timestamp);
	slot->tts_isnull[column_index] = false;
}

/** @brief Converts an optional calendar date to SQL date, or writes NULL. */
inline void
set_date_opt(TupleTableSlot* slot, std::size_t column_index,
			 const std::optional<provider::external_date>& date)
{
	if (!date)
	{
		set_null(slot, column_index);
		return;
	}
	const DateADT postgres_date = static_cast<DateADT>(
		date2j(date->year, date->month, date->day) - POSTGRES_EPOCH_JDATE);
	slot->tts_values[column_index] = DateADTGetDatum(postgres_date);
	slot->tts_isnull[column_index] = false;
}

/* ------------------------------------------------------------------------ */
/* jsonb helpers                                                             */
/* ------------------------------------------------------------------------ */

/**
 * @brief Parses a null-terminated JSON string as jsonb.
 * @return A value allocated in the current PostgreSQL memory context
 */
inline auto jsonb_datum_from_cstring(const char* text) -> Datum {
	return DirectFunctionCall1(jsonb_in, CStringGetDatum(text));
}

/** @brief Parses a JSON string and stores the jsonb value in a tuple column. */
inline void
set_jsonb_from_string(TupleTableSlot* slot, std::size_t column_index,
					  const std::string& json)
{
	slot->tts_values[column_index] = jsonb_datum_from_cstring(json.c_str());
	slot->tts_isnull[column_index] = false;
}

/** @brief Stores the  provider name in the provenance JSON object. */
inline void
set_provenance(TupleTableSlot* slot, std::size_t column_index,
			   provider::provider_id provider_identity)
{
	std::string json = "{\"provider\":\"";
	append_json_escaped(json, provider_identity.value);
	json += "\"}";
	set_jsonb_from_string(slot, column_index, json);
}

/**
 * @brief Parses provider attributes that begin with a JSON object or array.
 *
 * Missing values and other prefixes become SQL NULL. PostgreSQL reports an
 * error if a value beginning with { or [ contains invalid JSON.
 * @tparam p_capacity Maximum bytes accepted by the, provider field.
 */
template <std::size_t p_capacity>
inline void
set_provider_attributes(
	TupleTableSlot* slot, std::size_t column_index,
	const std::optional<provider::bounded_string<p_capacity>>& value)
{
	if (!value)
	{
		set_null(slot, column_index);
		return;
	}
	const std::string_view json_view = value->view();
	if (json_view.empty()
		|| (json_view.front() != '{' && json_view.front() != '['))
	{
		set_null(slot, column_index);
		return;
	}
	set_jsonb_from_string(slot, column_index, std::string(json_view));
}

/* ------------------------------------------------------------------------ */
/* array helpers                                                             */
/* ------------------------------------------------------------------------ */

/** @brief Converts an optional integer vector to bigint[], or writes NULL. */
inline void
set_bigint_array_opt(TupleTableSlot* slot, std::size_t column_index,
					 const std::optional<std::vector<std::int64_t>>& values)
{
	if (!values)
	{
		set_null(slot, column_index);
		return;
	}
	const std::size_t element_count = values->size();
	Datum* const elements = static_cast<Datum*>(palloc(
		sizeof(Datum) * (element_count == 0 ? 1 : element_count)));
	for (std::size_t element_index = 0; element_index < element_count;
		 ++element_index)
	{
		elements[element_index] = Int64GetDatum((*values)[element_index]);
	}
	ArrayType* const array = construct_array(
		elements, static_cast<int>(element_count), INT8OID, 8, true, 'd');
	slot->tts_values[column_index] = PointerGetDatum(array);
	slot->tts_isnull[column_index] = false;
}

/**
 * @brief Writes an optional bounded-string vector as a PostgreSQL text array.
 * @tparam p_capacity Maximum bytes accepted by each provider field.
 */
template <std::size_t p_capacity>
inline void
set_text_array_opt(
	TupleTableSlot* slot, std::size_t column_index,
	const std::optional<std::vector<provider::bounded_string<p_capacity>>>& values)
{
	if (!values)
	{
		set_null(slot, column_index);
		return;
	}
	const std::size_t element_count = values->size();
	Datum* const elements = static_cast<Datum*>(palloc(
		sizeof(Datum) * (element_count == 0 ? 1 : element_count)));
	for (std::size_t element_index = 0; element_index < element_count;
		 ++element_index)
	{
		const std::string_view text = (*values)[element_index].view();
		const char* const text_bytes = text.empty() ? "" : text.data();
		elements[element_index] = PointerGetDatum(
			cstring_to_text_with_len(text_bytes, static_cast<int>(text.size())));
	}
	ArrayType* const array = construct_array(
		elements, static_cast<int>(element_count), TEXTOID, -1, false, 'i');
	slot->tts_values[column_index] = PointerGetDatum(array);
	slot->tts_isnull[column_index] = false;
}

/**
 * @brief Converts an optional vector to a jsonb array, or writes NULL.
 * @tparam p_value Type of each vector element.
 * @tparam p_append_value Function that appends one element to the JSON string.
 */
template <class p_value, class p_append_value>
void
set_json_array_opt(TupleTableSlot* slot, std::size_t column_index,
				   const std::optional<std::vector<p_value>>& values,
				   p_append_value append_value)
{
	if (!values)
	{
		set_null(slot, column_index);
		return;
	}

	std::string json{"["};
	bool needs_separator = false;
	for (const p_value& value : *values)
	{
		if (needs_separator)
		{
			json += ',';
		}
		needs_separator = true;
		append_value(json, value);
	}
	json += ']';
	set_jsonb_from_string(slot, column_index, json);
}

/** @brief Stores alternate record identifiers as a jsonb array. */
inline void
set_aliases_jsonb(TupleTableSlot* slot, std::size_t column_index,
				  const std::optional<std::vector<provider::owning_record_key>>& aliases)
{
	set_json_array_opt(
		slot, column_index, aliases,
		/* Append  this alias directly to the array string. */
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

/** @brief Encodes provider gene references as a jsonb array. */
inline void
set_genes_jsonb(TupleTableSlot* slot, std::size_t column_index,
				const std::optional<std::vector<provider::owning_gene_reference>>& genes)
{
	set_json_array_opt(
		slot, column_index, genes,
		/* Append one gene reference directly to the shared JSON buffer. */
		[](std::string &json, const provider::owning_gene_reference &gene)
		{
			json += "{\"namespace\":\"";
			append_json_escaped(json, gene.name_space.view());
			json += "\",\"id\":\"";
			append_json_escaped(json, gene.identifier.view());
			json += '"';
			/* Add an optional named string member when the provider supplied it. */
			const auto append_optional = [&json](
				std::string_view field_name, const auto &value)
			{
				if (value)
				{
					json += ",\"";
					json += field_name;
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

/** @brief Encodes provider genomic intervals as a jsonb array. */
inline void
set_locations_jsonb(TupleTableSlot* slot, std::size_t column_index,
					const std::optional<std::vector<provider::owning_genomic_interval>>& locations)
{
	set_json_array_opt(
		slot, column_index, locations,
		/* Copy the interval bounds. start0 is included and end0 is excluded. */
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
			/* Add optional assembly identifiers to the interval object. */
			const auto append_optional = [&json](
				std::string_view field_name, const auto &value)
			{
				if (value)
				{
					json += ",\"";
					json += field_name;
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

/**
 * @brief Writes the source record identifiers shared by gene, location, and sequence rows.
 * @tparam p_row Provider row type with a record key.
 */
template <class p_row>
void
set_record_identity(TupleTableSlot *slot, const p_row &row)
{
	set_text(slot, 0, row.provider.value);
	set_text(slot, 1, row.key.domain.view());
	set_text_opt(slot, 2, row.key.name_space);
	set_text(slot, 3, row.key.original_identifier.view());
	set_text(slot, 4, row.key.accession.view());
	set_uint32_opt_as_int32(slot, 5, row.key.version);
}

/**
 * @brief Writes the provider version and retrieval details.
 *
 * Provenance contains the provider name. retrieved_at records when the
 * provider request completed.
 * @tparam p_row Provider row typ with audit metadata.
 */
template <class p_row>
void
set_audit_columns(TupleTableSlot *slot, const p_row &row,
				  std::size_t version_column,
				  std::size_t provenance_column,
				  std::size_t retrieved_at_column)
{
	set_text_opt(slot, version_column, row.audit.logical_provider_version);
	set_provenance(slot, provenance_column, row.provider);
	set_timestamptz(slot, retrieved_at_column, row.audit.completed_at);
}

/** @brief Writes sequence identifiers and region metadata, leaving taxon_id NULL. */
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

/** @brief Fills a SQL taxonomy row from a provider taxonomy record. */
void
fill_taxonomy(const provider::owning_taxonomy_row& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == k_taxonomy_column_count);

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

/** @brief Fills a SQL catalog row  from a provider record. */
void
fill_catalog(const provider::owning_catalog_row& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == k_catalog_column_count);

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
	set_bool(slot, 30, row.is_retrievable);                         /* retrievable NOT NULL */
	set_provider_attributes(slot, 31, row.provider_attributes_json);/* provider_attributes */
	set_audit_columns(slot, row, 32, 33, 34);
}

/** @brief Fills one gene row from the catalog record and a gene index. */
void
fill_record_gene(const provider::owning_catalog_row& row, std::size_t sub_index,
				 TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == k_record_gene_column_count);
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

/** @brief Fills one genomic location row from the catalog record and a location index */
void
fill_record_location(const provider::owning_catalog_row& row, std::size_t sub_index,
					 TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == k_record_location_column_count);
	const provider::owning_genomic_interval& location = (*row.locations)[sub_index];

	set_record_identity(slot, row);
	set_int32(slot, 6, static_cast<std::int32_t>(sub_index));       /* location_index NOT NULL */
	set_int32(slot, 7, static_cast<std::int32_t>(row.locations->size()));/* location_count NOT NULL */
	set_text_opt(slot, 8, location.assembly_accession);             /* assembly_accession */
	set_text_opt(slot, 9, location.assembly_database);              /* assembly_database */
	set_text(
		slot, 10, location.sequence_alias.view()); /* sequence_alias NOT NULL */
	set_int64(
		slot, 11,
		static_cast<std::int64_t>(location.start0)); /* start0 NOT NULL */
	set_int64(slot, 12, static_cast<std::int64_t>(location.end0));  /* end0 NOT NULL */
	set_int16(
		slot, 13,
		static_cast<std::int16_t>(location.orientation)); /* strand NOT NULL */
	set_audit_columns(slot, row, 14, 15, 16);
}

/** @brief Builds a bioseq.seq value from ASCII bases for later alphabet and storage-width binding. */
inline void
set_sequence_value(TupleTableSlot* slot, std::size_t column_index,
				   std::string_view bases)
{
	slot->tts_values[column_index] = make_unbound_sequence_from_ascii(
		bases.empty() ? "" : bases.data(), bases.size());
	slot->tts_isnull[column_index] = false;
}

/** @brief Fills a complete sequence row, including its source record metadata. */
void
fill_sequence(const provider::owning_sequence_payload& row, TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == k_sequence_column_count);

	set_sequence_region(slot, row);
	set_sequence_value(slot, 14, row.bases);
	set_audit_columns(slot, row, 15, 16, 17);
}

/** @brief Fills a sequence chunk row with its offset and end-of-record flag. */
void
fill_sequence_chunk(const provider::owning_sequence_payload& row,
					TupleTableSlot* slot)
{
	Assert(slot->tts_tupleDescriptor->natts == k_sequence_chunk_column_count);

	set_sequence_region(slot, row);
	set_int64(slot, 14, static_cast<std::int64_t>(row.chunk_index));/* chunk_index NOT NULL */
	set_int64(slot, 15, static_cast<std::int64_t>(row.base_offset));/* base_offset NOT NULL */
	set_sequence_value(slot, 16, row.bases);
	set_bool(slot, 17, row.end_of_record);                          /* end_of_record NOT NULL */
	set_audit_columns(slot, row, 18, 19, 20);
}

} // namespace

} // namespace bioseq::postgres::external

/* ======================================================================== */
/* Public row conversion functions. */
/* ======================================================================== */

export namespace bioseq::postgres::external {

/**
 * @brief Counts the SQL rows available in a provider message for this table.
 *
 * A catalog message supplies one row per  gene or location for those tables.
 * Other matching row messages supply one row. Returns zero for messages
 * that do not match the, table entity.
 */
[[nodiscard]] auto message_row_count(
	relation_entity entity,
	const provider::owning_provider_message& message) noexcept -> std::size_t
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
			if (const auto* catalog_row =
					std::get_if<provider::owning_catalog_row>(&message))
				return catalog_row->genes ? catalog_row->genes->size() : 0u;
			return 0u;

		case relation_entity::record_location:
			if (const auto* catalog_row =
					std::get_if<provider::owning_catalog_row>(&message))
				return catalog_row->locations ? catalog_row->locations->size() : 0u;
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

/**
 * @brief Copies one provider message row into a virtual tuple slot.
 *
 * The caller clears the slot first and uses an index below message_row_count().
 * On success, all columns contain a PostgreSQL value or SQL NULL. Allocated
 * values belong to the current PostgreSQL memory context.
 * @return True on success, or false if the message cannot supply thi  row.
 */
auto materialize_message_row(relation_entity entity,
							 const provider::owning_provider_message& message,
							 std::size_t row_index,
							 TupleTableSlot* slot) -> bool
{
	switch (entity)
	{
		case relation_entity::taxonomy:
		{
			const auto* taxonomy_row =
				std::get_if<provider::owning_taxonomy_row>(&message);
			if (taxonomy_row == nullptr)
				return false;
			fill_taxonomy(*taxonomy_row, slot);
			return true;
		}

		case relation_entity::catalog:
		{
			const auto* catalog_row =
				std::get_if<provider::owning_catalog_row>(&message);
			if (catalog_row == nullptr)
				return false;
			fill_catalog(*catalog_row, slot);
			return true;
		}

		case relation_entity::record_gene:
		{
			const auto* catalog_row =
				std::get_if<provider::owning_catalog_row>(&message);
			if (catalog_row == nullptr || !catalog_row->genes
				|| row_index >= catalog_row->genes->size())
				return false;
			fill_record_gene(*catalog_row, row_index, slot);
			return true;
		}

		case relation_entity::record_location:
		{
			const auto* catalog_row =
				std::get_if<provider::owning_catalog_row>(&message);
			if (catalog_row == nullptr || !catalog_row->locations
				|| row_index >= catalog_row->locations->size())
				return false;
			fill_record_location(*catalog_row, row_index, slot);
			return true;
		}

		case relation_entity::sequence:
		{
			const auto* sequence_row =
				std::get_if<provider::owning_sequence_payload>(&message);
			if (sequence_row == nullptr || row_index != 0)
				return false;
			fill_sequence(*sequence_row, slot);
			return true;
		}

		case relation_entity::sequence_chunk:
		{
			const auto* sequence_row =
				std::get_if<provider::owning_sequence_payload>(&message);
			if (sequence_row == nullptr || row_index != 0)
				return false;
			fill_sequence_chunk(*sequence_row, slot);
			return true;
		}

		case relation_entity::unspecified:
		default:
			return false;
	}
}

} // namespace bioseq::postgres::external
