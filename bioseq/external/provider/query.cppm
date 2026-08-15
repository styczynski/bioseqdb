/**
 * @file external/provider/query.cppm
 * @brief Provider query variants, validation, and stable hashing.
 */
module;

#include <bitset>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

export module bioseq.external.provider.query;

import bioseq.external.provider.base;

/// @brief Common immutable query contract.
// A single owning query
// representation is shared by the PostgreSQL planner and the non-PG facade.
// This module also carries the field-to-SQL-column mapping consumed by the PG
// bridge and the pushdown-quality classification produced by each adapter's
// predicate translator.
export namespace bioseq::external::provider {

// ---------------------------------------------------------------------------
// Targets, projections, and limits.
// ---------------------------------------------------------------------------
enum class query_target : std::uint8_t {
  taxonomy,
  catalog,
  record_gene,
  record_location,
  sequence,
  sequence_chunk
};

enum class sequence_delivery_mode : std::uint8_t {
  not_applicable,
  raw_bytes,
  materialized_record,
  chunked_records
};

enum class external_field : std::uint16_t {
  provider,
  domain,
  entity_kind,
  molecule_kind,
  name_space,
  original_identifier,
  accession,
  accession_version,
  alias,
  aliases,
  taxon_id,
  provider_taxon_id,
  scientific_name,
  common_name,
  taxon_rank,
  parent_taxon_id,
  lineage_taxon_id,
  taxon_lineage,
  taxon_synonym,
  taxon_synonyms,
  title,
  assembly_accession,
  assembly_database,
  assembly_source,
  assembly_level,
  assembly_status,
  reference,
  annotated,
  reviewed,
  fragment,
  gene_name_space,
  gene_id,
  gene_symbol,
  gene_name,
  locus_tag,
  gene_type,
  gene_index,
  gene_count,
  keyword,
  keywords,
  gene_references,
  record_locations,
  host_taxon_id,
  geography,
  lineage_name,
  length,
  release_date,
  update_date,
  chromosome,
  sequence_alias,
  start0,
  end0,
  strand,
  location_index,
  location_count,
  chunk_index,
  base_offset,
  end_of_record,
  sequence_bytes,
  retrievable,
  provider_attributes,
  provenance,
  provider_version,
  retrieved_at,
  count
};

using external_field_mask =
    std::bitset<static_cast<std::size_t>(external_field::count)>;

using external_scalar = std::variant<bool, std::int64_t, std::uint64_t, double,
                                     bounded_string<maximum_identifier_bytes>,
                                     external_date, external_timestamp>;

enum class predicate_operator : std::uint8_t {
  equal,
  not_equal,
  less,
  less_equal,
  greater,
  greater_equal,
  contains,
  prefix,
  in,
  between,
  is_null,
  is_not_null
};

struct comparison_predicate {
  predicate_operator operation; // A non-logical operator.
  external_field field;
  std::vector<external_scalar> values;
};

struct taxon_predicate {
  external_field field; // taxon_id only.
  std::int64_t taxon_id;
  taxon_match match;
};

enum class text_search_scope : std::uint8_t {
  any,
  organism,
  assembly,
  gene,
  record_description
};

struct text_search_predicate {
  text_search_scope scope;
  bounded_string<maximum_identifier_bytes> query;
};

struct native_query_predicate {
  provider_id provider;
  bounded_string<maximum_identifier_bytes> query;
};

enum class predicate_node_kind : std::uint8_t {
  comparison,
  taxon,
  text_search,
  native_query,
  and_,
  or_,
  not_
};

// A flat post-order tree avoids recursive allocation. Logical children are
// roots named by a separate child-index array.
struct predicate_node {
  constexpr static std::uint32_t no_child =
      std::numeric_limits<std::uint32_t>::max();

  predicate_node_kind kind;
  std::uint32_t first_child_offset{no_child};
  std::uint16_t child_count{};
  std::variant<std::monostate, comparison_predicate, taxon_predicate,
               text_search_predicate, native_query_predicate>
      leaf;
};

struct external_projection {
  external_field_mask fields;
};

struct external_order_key {
  external_field field;
  bool is_descending;
  bool are_nulls_first;
};

struct normalized_external_query {
  query_target target;
  sequence_delivery_mode delivery;
  std::vector<provider_id> providers;
  std::vector<predicate_node> predicates;
  std::vector<std::uint32_t> predicate_children;
  std::optional<std::uint32_t> root_predicate;
  external_projection projection;
  std::vector<external_order_key> order_by;
  std::optional<std::uint64_t> limit;
  std::optional<std::uint64_t> offset;
  std::size_t requested_page_size;
  /// @brief Measures resident and dynamically allocated query storage.
  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(normalized_external_query);
    total += providers.capacity() * sizeof(provider_id);
    total += predicates.capacity() * sizeof(predicate_node);
    total += predicate_children.capacity() * sizeof(std::uint32_t);
    total += order_by.capacity() * sizeof(external_order_key);
    for (const predicate_node &node : predicates) {
      if (const auto *comparison =
              std::get_if<comparison_predicate>(&node.leaf)) {
        total += comparison->values.capacity() * sizeof(external_scalar);
        for (const external_scalar &value : comparison->values) {
          if (const auto *text =
                  std::get_if<bounded_string<maximum_identifier_bytes>>(
                      &value)) {
            total += text->size();
          }
        }
      } else if (const auto *text =
                     std::get_if<text_search_predicate>(&node.leaf)) {
        total += text->query.size();
      } else if (const auto *native =
                     std::get_if<native_query_predicate>(&node.leaf)) {
        total += native->query.size();
      }
    }
    return total;
  }
};

// ---------------------------------------------------------------------------
// Translation and correctness.
// ---------------------------------------------------------------------------
enum class pushdown_quality : std::uint8_t {
  exact,      // Remote result has exactly the requested semantics.
  superset,   // Remote filter is safe, but local recheck is required.
  local_only, // Returned metadata can be rechecked locally.
  unsupported // Provider cannot return enough data to evaluate it.
};

// Maps a projected/recheck field to its canonical SQL column name for the PG
// bridge. Identical names map directly; exceptions are name_space ->
// "namespace", taxon_lineage -> "lineage", taxon_synonyms
// -> "synonyms", gene_references -> "genes", record_locations -> "locations",
// the four flags -> is_*, length -> "sequence_length", sequence_bytes ->
// "sequence"). The `count` sentinel is never selectable and has no column.
[[nodiscard]] constexpr std::string_view
to_sql_column(external_field field) noexcept {
  switch (field) {
  case external_field::provider:
    return "provider";
  case external_field::domain:
    return "domain";
  case external_field::entity_kind:
    return "entity_kind";
  case external_field::molecule_kind:
    return "molecule_kind";
  case external_field::name_space:
    return "namespace";
  case external_field::original_identifier:
    return "original_identifier";
  case external_field::accession:
    return "accession";
  case external_field::accession_version:
    return "accession_version";
  case external_field::alias:
    return "alias";
  case external_field::aliases:
    return "aliases";
  case external_field::taxon_id:
    return "taxon_id";
  case external_field::provider_taxon_id:
    return "provider_taxon_id";
  case external_field::scientific_name:
    return "scientific_name";
  case external_field::common_name:
    return "common_name";
  case external_field::taxon_rank:
    return "taxon_rank";
  case external_field::parent_taxon_id:
    return "parent_taxon_id";
  case external_field::lineage_taxon_id:
    return "lineage_taxon_id";
  case external_field::taxon_lineage:
    return "lineage";
  case external_field::taxon_synonym:
    return "taxon_synonym";
  case external_field::taxon_synonyms:
    return "synonyms";
  case external_field::title:
    return "title";
  case external_field::assembly_accession:
    return "assembly_accession";
  case external_field::assembly_database:
    return "assembly_database";
  case external_field::assembly_source:
    return "assembly_source";
  case external_field::assembly_level:
    return "assembly_level";
  case external_field::assembly_status:
    return "assembly_status";
  case external_field::reference:
    return "is_reference";
  case external_field::annotated:
    return "is_annotated";
  case external_field::reviewed:
    return "is_reviewed";
  case external_field::fragment:
    return "is_fragment";
  case external_field::gene_name_space:
    return "gene_name_space";
  case external_field::gene_id:
    return "gene_id";
  case external_field::gene_symbol:
    return "gene_symbol";
  case external_field::gene_name:
    return "gene_name";
  case external_field::locus_tag:
    return "locus_tag";
  case external_field::gene_type:
    return "gene_type";
  case external_field::gene_index:
    return "gene_index";
  case external_field::gene_count:
    return "gene_count";
  case external_field::keyword:
    return "keyword";
  case external_field::keywords:
    return "keywords";
  case external_field::gene_references:
    return "genes";
  case external_field::record_locations:
    return "locations";
  case external_field::host_taxon_id:
    return "host_taxon_id";
  case external_field::geography:
    return "geography";
  case external_field::lineage_name:
    return "lineage_name";
  case external_field::length:
    return "sequence_length";
  case external_field::release_date:
    return "release_date";
  case external_field::update_date:
    return "update_date";
  case external_field::chromosome:
    return "chromosome";
  case external_field::sequence_alias:
    return "sequence_alias";
  case external_field::start0:
    return "start0";
  case external_field::end0:
    return "end0";
  case external_field::strand:
    return "strand";
  case external_field::location_index:
    return "location_index";
  case external_field::location_count:
    return "location_count";
  case external_field::chunk_index:
    return "chunk_index";
  case external_field::base_offset:
    return "base_offset";
  case external_field::end_of_record:
    return "end_of_record";
  case external_field::sequence_bytes:
    return "sequence";
  case external_field::retrievable:
    return "retrievable";
  case external_field::provider_attributes:
    return "provider_attributes";
  case external_field::provenance:
    return "provenance";
  case external_field::provider_version:
    return "provider_version";
  case external_field::retrieved_at:
    return "retrieved_at";
  case external_field::count:
    return "";
  }
  return "";
}

} // namespace bioseq::external::provider
