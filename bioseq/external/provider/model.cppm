/**
 * @file external/provider/model.cppm
 * @brief Provider records, assets, provenance, and delivery models.
 */
module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.external.provider.model;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.common.sink_control;

/// @brief Normalized transient and owning provider row models.
// Transient view structs a provider session
// emits into a sink (valid only for the duration of one callback), plus the
// provider-neutral OWNING mirrors moved through channels. Owning forms preserve
// the field order/optionality of their view, replace `string_view` with
// `bounded_string`, replace `span<T>` with `vector<owning_T>`, and expose
// `measure_deep_size_bytes()` (string/vector capacities + fixed overhead) plus
// `view()` which reconstructs a transient view referencing the owning storage.
// Repeated data whose view element type differs from the owning element type is
// rebuilt into `mutable` scratch vectors owned by the top-level owning row; the
// returned view outlives only the callback, so pointing spans at that scratch
// is correct.
export namespace bioseq::external::provider {

// ===========================================================================
// Transient row views.
// ===========================================================================

struct provenance_hop_view {
  std::optional<provider_id> known_provider;
  std::string_view provider_name;
  std::string_view operation;
  std::string_view provider_version;
  std::string_view request_id;
  std::string_view source_url; // Sanitized; never contains credentials.
  std::string_view media_type;
  std::chrono::system_clock::time_point retrieved_at;
};

struct row_audit_view {
  std::optional<std::string_view> logical_provider_version;
  std::chrono::system_clock::time_point completed_at;
  std::span<const provenance_hop_view> provenance;
};

struct gene_reference_view {
  std::string_view name_space; // NCBI GeneID, HGNC, locus tag, provider.
  std::string_view identifier;
  std::optional<std::string_view> symbol;
  std::optional<std::string_view> name;
  std::optional<std::string_view> type;
  std::optional<std::string_view> locus_tag;
};

enum class integrity_scope : std::uint8_t {
  transfer_bytes,
  decoded_member,
  decoded_record
};

struct integrity_check_view {
  std::string_view algorithm;
  std::string_view value;
  integrity_scope applies_to;
};

struct taxonomy_row_view {
  provider_id provider;
  std::optional<std::int64_t> taxon_id; // Normalized NCBI TaxID when known.
  std::string_view provider_taxon_id;
  std::optional<std::string_view> scientific_name;
  std::optional<std::string_view> common_name;
  std::optional<std::string_view> rank;
  std::optional<std::int64_t> parent_taxon_id;
  std::optional<std::span<const std::int64_t>> lineage;
  std::optional<std::span<const std::string_view>> synonyms;
  std::optional<std::string_view> provider_attributes_json;
  row_audit_view audit;
};

struct catalog_row_view {
  record_key_view key;
  entity_kind kind;
  std::optional<molecule_kind> molecule;
  std::optional<std::span<const record_key_view>> aliases;
  std::optional<std::int64_t> taxon_id;
  std::optional<std::string_view> scientific_name;
  std::optional<std::string_view> title;
  std::optional<std::span<const gene_reference_view>> genes;
  std::optional<std::span<const std::string_view>> keywords;
  std::optional<std::int64_t> host_taxon_id;
  std::optional<std::string_view> geography;
  std::optional<std::string_view> lineage_name;
  std::optional<std::uint64_t> length;
  std::optional<bool> is_reference;
  std::optional<bool> is_reviewed;
  std::optional<bool> is_annotated;
  std::optional<bool> is_fragment;
  std::optional<std::span<const genomic_interval>> locations;
  std::optional<std::string_view> assembly_accession;
  std::optional<std::string_view> assembly_database;
  std::optional<std::string_view> assembly_source;
  std::optional<std::string_view> assembly_level;
  std::optional<std::string_view> assembly_status;
  std::optional<external_date> release_date;
  std::optional<external_date> update_date;
  bool is_retrievable;
  std::optional<std::string_view> provider_attributes_json;
  row_audit_view audit;
};

struct record_gene_row_view {
  record_key_view key;
  gene_reference_view gene;
  std::uint32_t gene_index;
  std::uint32_t gene_count;
  row_audit_view audit;
};

struct record_location_row_view {
  record_key_view key;
  genomic_interval location;
  std::uint32_t location_index;
  std::uint32_t location_count;
  row_audit_view audit;
};

// ===========================================================================
// Shared owning leaf types. Field order and optionality match their views.
// ===========================================================================

struct owning_record_key {
  provider_id provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  bounded_string<maximum_identifier_bytes> domain;
  std::optional<bounded_string<maximum_identifier_bytes>> name_space;
  bounded_string<maximum_identifier_bytes> original_identifier;
  bounded_string<maximum_identifier_bytes> accession;
  std::optional<std::uint32_t> version;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
    total += domain.measure_deep_size_bytes() - sizeof(domain);
    if (name_space) {
      total += name_space->measure_deep_size_bytes() - sizeof(*name_space);
    }
    total += original_identifier.measure_deep_size_bytes() -
             sizeof(original_identifier);
    total += accession.measure_deep_size_bytes() - sizeof(accession);
    return total;
  }

  [[nodiscard]] record_key_view view() const noexcept {
    record_key_view result;
    result.provider = provider;
    result.domain = domain.view();
    if (name_space) {
      result.name_space = name_space->view();
    }
    result.original_identifier = original_identifier.view();
    result.accession = accession.view();
    result.version = version;
    return result;
  }
};

struct owning_integrity_check {
  bounded_string<64> algorithm;
  bounded_string<maximum_identifier_bytes> value;
  integrity_scope applies_to;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += algorithm.measure_deep_size_bytes() - sizeof(algorithm);
    total += value.measure_deep_size_bytes() - sizeof(value);
    return total;
  }

  [[nodiscard]] integrity_check_view view() const noexcept {
    integrity_check_view result;
    result.algorithm = algorithm.view();
    result.value = value.view();
    result.applies_to = applies_to;
    return result;
  }
};

struct owning_integrity_result {
  owning_integrity_check expected;
  bounded_string<maximum_identifier_bytes> actual_value;
  bool is_verified;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += expected.measure_deep_size_bytes() - sizeof(expected);
    total += actual_value.measure_deep_size_bytes() - sizeof(actual_value);
    return total;
  }
};

struct owning_sequence_origin {
  std::optional<std::int64_t> taxon_id;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_accession;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_database;
  std::optional<bounded_string<maximum_identifier_bytes>> sequence_alias;
  std::optional<std::uint64_t> start0;
  std::optional<std::uint64_t> end0;
  genomic_interval::strand orientation{genomic_interval::strand::unknown};

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    if (assembly_accession) {
      total += assembly_accession->measure_deep_size_bytes() -
               sizeof(*assembly_accession);
    }
    if (assembly_database) {
      total += assembly_database->measure_deep_size_bytes() -
               sizeof(*assembly_database);
    }
    if (sequence_alias) {
      total +=
          sequence_alias->measure_deep_size_bytes() - sizeof(*sequence_alias);
    }
    return total;
  }

  [[nodiscard]] sequence_origin_view view() const noexcept {
    sequence_origin_view result;
    result.taxon_id = taxon_id;
    if (assembly_accession) {
      result.assembly_accession = assembly_accession->view();
    }
    if (assembly_database) {
      result.assembly_database = assembly_database->view();
    }
    if (sequence_alias) {
      result.sequence_alias = sequence_alias->view();
    }
    result.start0 = start0;
    result.end0 = end0;
    result.orientation = orientation;
    return result;
  }
};

struct owning_provenance_hop {
  std::optional<provider_id> known_provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  bounded_string<maximum_identifier_bytes> operation;
  bounded_string<maximum_identifier_bytes> provider_version;
  bounded_string<maximum_identifier_bytes> request_id;
  bounded_string<maximum_identifier_bytes> sanitized_source_url;
  bounded_string<256> media_type;
  external_timestamp retrieved_at;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
    total += operation.measure_deep_size_bytes() - sizeof(operation);
    total +=
        provider_version.measure_deep_size_bytes() - sizeof(provider_version);
    total += request_id.measure_deep_size_bytes() - sizeof(request_id);
    total += sanitized_source_url.measure_deep_size_bytes() -
             sizeof(sanitized_source_url);
    total += media_type.measure_deep_size_bytes() - sizeof(media_type);
    return total;
  }

  [[nodiscard]] provenance_hop_view view() const noexcept {
    provenance_hop_view result;
    result.known_provider = known_provider;
    result.provider_name = provider_name.view();
    result.operation = operation.view();
    result.provider_version = provider_version.view();
    result.request_id = request_id.view();
    result.source_url = sanitized_source_url.view();
    result.media_type = media_type.view();
    result.retrieved_at =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            retrieved_at);
    return result;
  }
};

struct owning_row_audit {
  std::optional<bounded_string<maximum_identifier_bytes>>
      logical_provider_version;
  external_timestamp completed_at;
  std::vector<owning_provenance_hop> provenance;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    if (logical_provider_version) {
      total += logical_provider_version->measure_deep_size_bytes() -
               sizeof(*logical_provider_version);
    }
    total += provenance.capacity() * sizeof(owning_provenance_hop);
    for (const auto &hop : provenance) {
      total += hop.measure_deep_size_bytes() - sizeof(hop);
    }
    return total;
  }
};

struct owning_sequence_audit_seed {
  std::optional<bounded_string<maximum_identifier_bytes>>
      logical_provider_version;
  std::vector<owning_provenance_hop> provenance;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    if (logical_provider_version) {
      total += logical_provider_version->measure_deep_size_bytes() -
               sizeof(*logical_provider_version);
    }
    total += provenance.capacity() * sizeof(owning_provenance_hop);
    for (const auto &hop : provenance) {
      total += hop.measure_deep_size_bytes() - sizeof(hop);
    }
    return total;
  }
};

// ---------------------------------------------------------------------------
// Owning helper leaves for repeated catalog data.
// ---------------------------------------------------------------------------

struct owning_gene_reference {
  bounded_string<maximum_identifier_bytes> name_space;
  bounded_string<maximum_identifier_bytes> identifier;
  std::optional<bounded_string<maximum_identifier_bytes>> symbol;
  std::optional<bounded_string<maximum_identifier_bytes>> name;
  std::optional<bounded_string<maximum_identifier_bytes>> type;
  std::optional<bounded_string<maximum_identifier_bytes>> locus_tag;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += name_space.measure_deep_size_bytes() - sizeof(name_space);
    total += identifier.measure_deep_size_bytes() - sizeof(identifier);
    if (symbol)
      total += symbol->measure_deep_size_bytes() - sizeof(*symbol);
    if (name)
      total += name->measure_deep_size_bytes() - sizeof(*name);
    if (type)
      total += type->measure_deep_size_bytes() - sizeof(*type);
    if (locus_tag) {
      total += locus_tag->measure_deep_size_bytes() - sizeof(*locus_tag);
    }
    return total;
  }

  [[nodiscard]] gene_reference_view view() const noexcept {
    gene_reference_view result;
    result.name_space = name_space.view();
    result.identifier = identifier.view();
    if (symbol)
      result.symbol = symbol->view();
    if (name)
      result.name = name->view();
    if (type)
      result.type = type->view();
    if (locus_tag)
      result.locus_tag = locus_tag->view();
    return result;
  }
};

struct owning_genomic_interval {
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_accession;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_database;
  bounded_string<maximum_identifier_bytes> sequence_alias;
  std::uint64_t start0{};
  std::uint64_t end0{};
  genomic_interval::strand orientation{genomic_interval::strand::unknown};

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    if (assembly_accession) {
      total += assembly_accession->measure_deep_size_bytes() -
               sizeof(*assembly_accession);
    }
    if (assembly_database) {
      total += assembly_database->measure_deep_size_bytes() -
               sizeof(*assembly_database);
    }
    total += sequence_alias.measure_deep_size_bytes() - sizeof(sequence_alias);
    return total;
  }

  [[nodiscard]] genomic_interval view() const noexcept {
    genomic_interval result;
    if (assembly_accession) {
      result.assembly_accession = assembly_accession->view();
    }
    if (assembly_database) {
      result.assembly_database = assembly_database->view();
    }
    result.sequence_alias = sequence_alias.view();
    result.start0 = start0;
    result.end0 = end0;
    result.orientation = orientation;
    return result;
  }
};

/// @brief Owns transient provenance views used while exposing an owning audit.
///
/// The returned view remains valid until the next call on the same storage or
/// until the storage is destroyed.
struct row_audit_view_storage {
  mutable std::vector<provenance_hop_view> provenance;

  /// @brief Reconstructs a callback-scoped view without output parameters.
  [[nodiscard]] row_audit_view
  make_view(const owning_row_audit &audit) const noexcept {
    provenance.clear();
    provenance.reserve(audit.provenance.size());
    for (const auto &hop : audit.provenance) {
      provenance.push_back(hop.view());
    }
    row_audit_view result;
    if (audit.logical_provider_version) {
      result.logical_provider_version = audit.logical_provider_version->view();
    }
    result.completed_at =
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            audit.completed_at);
    result.provenance = std::span<const provenance_hop_view>(provenance.data(),
                                                             provenance.size());
    return result;
  }

  /// @brief Returns the dynamic storage retained for reconstructed views.
  [[nodiscard]] std::size_t measure_storage_bytes() const noexcept {
    return provenance.capacity() * sizeof(provenance_hop_view);
  }
};

// ===========================================================================
// Owning row messages mirror taxonomy_row_view and catalog_row_view while
// retaining provider identity and callback-view storage.
// ===========================================================================

struct owning_taxonomy_row {
  provider_id provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  std::optional<std::int64_t> taxon_id;
  bounded_string<maximum_identifier_bytes> provider_taxon_id;
  std::optional<bounded_string<maximum_identifier_bytes>> scientific_name;
  std::optional<bounded_string<maximum_identifier_bytes>> common_name;
  std::optional<bounded_string<maximum_identifier_bytes>> rank;
  std::optional<std::int64_t> parent_taxon_id;
  std::optional<std::vector<std::int64_t>> lineage;
  std::optional<std::vector<bounded_string<maximum_identifier_bytes>>> synonyms;
  std::optional<bounded_string<maximum_attributes_json_bytes>>
      provider_attributes_json;
  owning_row_audit audit;

  // Transient reconstruction storage, rebuilt for each callback view.
  mutable std::vector<std::string_view> synonyms_scratch_;
  mutable row_audit_view_storage audit_view_storage_;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
    total +=
        provider_taxon_id.measure_deep_size_bytes() - sizeof(provider_taxon_id);
    if (scientific_name) {
      total +=
          scientific_name->measure_deep_size_bytes() - sizeof(*scientific_name);
    }
    if (common_name) {
      total += common_name->measure_deep_size_bytes() - sizeof(*common_name);
    }
    if (rank)
      total += rank->measure_deep_size_bytes() - sizeof(*rank);
    if (lineage) {
      total += lineage->capacity() * sizeof(std::int64_t);
    }
    if (synonyms) {
      total += synonyms->capacity() *
               sizeof(bounded_string<maximum_identifier_bytes>);
      for (const auto &syn : *synonyms) {
        total += syn.measure_deep_size_bytes() - sizeof(syn);
      }
    }
    if (provider_attributes_json) {
      total += provider_attributes_json->measure_deep_size_bytes() -
               sizeof(*provider_attributes_json);
    }
    total += audit.measure_deep_size_bytes() - sizeof(audit);
    total += synonyms_scratch_.capacity() * sizeof(std::string_view);
    total += audit_view_storage_.measure_storage_bytes();
    return total;
  }

  [[nodiscard]] taxonomy_row_view view() const noexcept {
    taxonomy_row_view result;
    result.provider = provider;
    result.taxon_id = taxon_id;
    result.provider_taxon_id = provider_taxon_id.view();
    if (scientific_name)
      result.scientific_name = scientific_name->view();
    if (common_name)
      result.common_name = common_name->view();
    if (rank)
      result.rank = rank->view();
    result.parent_taxon_id = parent_taxon_id;
    if (lineage) {
      result.lineage =
          std::span<const std::int64_t>(lineage->data(), lineage->size());
    }
    if (synonyms) {
      synonyms_scratch_.clear();
      synonyms_scratch_.reserve(synonyms->size());
      for (const auto &syn : *synonyms) {
        synonyms_scratch_.push_back(syn.view());
      }
      result.synonyms = std::span<const std::string_view>(
          synonyms_scratch_.data(), synonyms_scratch_.size());
    }
    if (provider_attributes_json) {
      result.provider_attributes_json = provider_attributes_json->view();
    }
    result.audit = audit_view_storage_.make_view(audit);
    return result;
  }
};

// Owning payload for the sequences / sequence_chunks relations: the retrieved
// nucleotide letters plus the region metadata needed to fill either relation.
// The row writer selects columns from the target entity; chunk_index /
// base_offset / end_of_record are meaningful only for the sequence_chunks
// relation. `bases` holds the raw letters on the standard heap (transient
// scratch, materialised into a bioseq.seq Datum by the PG row writer).
struct owning_sequence_payload {
  provider_id provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  owning_record_key key;
  molecule_kind molecule{molecule_kind::nucleotide};
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_accession;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_database;
  std::optional<bounded_string<maximum_identifier_bytes>> chromosome;
  std::optional<std::uint64_t> start0;
  std::optional<std::uint64_t> end0;
  genomic_interval::strand strand{genomic_interval::strand::unknown};
  std::string bases;
  std::uint64_t chunk_index{};
  std::uint64_t base_offset{};
  bool end_of_record{true};
  owning_row_audit audit;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
    total += key.measure_deep_size_bytes() - sizeof(key);
    if (assembly_accession) {
      total += assembly_accession->measure_deep_size_bytes() -
               sizeof(*assembly_accession);
    }
    if (assembly_database) {
      total += assembly_database->measure_deep_size_bytes() -
               sizeof(*assembly_database);
    }
    if (chromosome) {
      total += chromosome->measure_deep_size_bytes() - sizeof(*chromosome);
    }
    total += bases.capacity();
    total += audit.measure_deep_size_bytes() - sizeof(audit);
    return total;
  }
};

struct owning_catalog_row {
  provider_id provider;
  bounded_string<maximum_provider_name_bytes> provider_name;
  owning_record_key key;
  entity_kind kind{};
  std::optional<molecule_kind> molecule;
  std::optional<std::vector<owning_record_key>> aliases;
  std::optional<std::int64_t> taxon_id;
  std::optional<bounded_string<maximum_identifier_bytes>> scientific_name;
  std::optional<bounded_string<maximum_identifier_bytes>> title;
  std::optional<std::vector<owning_gene_reference>> genes;
  std::optional<std::vector<bounded_string<maximum_identifier_bytes>>> keywords;
  std::optional<std::int64_t> host_taxon_id;
  std::optional<bounded_string<maximum_identifier_bytes>> geography;
  std::optional<bounded_string<maximum_identifier_bytes>> lineage_name;
  std::optional<std::uint64_t> length;
  std::optional<bool> is_reference;
  std::optional<bool> is_reviewed;
  std::optional<bool> is_annotated;
  std::optional<bool> is_fragment;
  std::optional<std::vector<owning_genomic_interval>> locations;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_accession;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_database;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_source;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_level;
  std::optional<bounded_string<maximum_identifier_bytes>> assembly_status;
  std::optional<external_date> release_date;
  std::optional<external_date> update_date;
  bool is_retrievable{};
  std::optional<bounded_string<maximum_attributes_json_bytes>>
      provider_attributes_json;
  owning_row_audit audit;

  // Transient reconstruction storage, rebuilt for each callback view.
  mutable std::vector<record_key_view> aliases_scratch_;
  mutable std::vector<gene_reference_view> genes_scratch_;
  mutable std::vector<std::string_view> keywords_scratch_;
  mutable std::vector<genomic_interval> locations_scratch_;
  mutable row_audit_view_storage audit_view_storage_;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    std::size_t total = sizeof(*this);
    total += provider_name.measure_deep_size_bytes() - sizeof(provider_name);
    total += key.measure_deep_size_bytes() - sizeof(key);
    if (aliases) {
      total += aliases->capacity() * sizeof(owning_record_key);
      for (const auto &alias : *aliases) {
        total += alias.measure_deep_size_bytes() - sizeof(alias);
      }
    }
    if (scientific_name) {
      total +=
          scientific_name->measure_deep_size_bytes() - sizeof(*scientific_name);
    }
    if (title)
      total += title->measure_deep_size_bytes() - sizeof(*title);
    if (genes) {
      total += genes->capacity() * sizeof(owning_gene_reference);
      for (const auto &gene : *genes) {
        total += gene.measure_deep_size_bytes() - sizeof(gene);
      }
    }
    if (keywords) {
      total += keywords->capacity() *
               sizeof(bounded_string<maximum_identifier_bytes>);
      for (const auto &kw : *keywords) {
        total += kw.measure_deep_size_bytes() - sizeof(kw);
      }
    }
    if (geography) {
      total += geography->measure_deep_size_bytes() - sizeof(*geography);
    }
    if (lineage_name) {
      total += lineage_name->measure_deep_size_bytes() - sizeof(*lineage_name);
    }
    if (locations) {
      total += locations->capacity() * sizeof(owning_genomic_interval);
      for (const auto &loc : *locations) {
        total += loc.measure_deep_size_bytes() - sizeof(loc);
      }
    }
    if (assembly_accession) {
      total += assembly_accession->measure_deep_size_bytes() -
               sizeof(*assembly_accession);
    }
    if (assembly_database) {
      total += assembly_database->measure_deep_size_bytes() -
               sizeof(*assembly_database);
    }
    if (assembly_source) {
      total +=
          assembly_source->measure_deep_size_bytes() - sizeof(*assembly_source);
    }
    if (assembly_level) {
      total +=
          assembly_level->measure_deep_size_bytes() - sizeof(*assembly_level);
    }
    if (assembly_status) {
      total +=
          assembly_status->measure_deep_size_bytes() - sizeof(*assembly_status);
    }
    if (provider_attributes_json) {
      total += provider_attributes_json->measure_deep_size_bytes() -
               sizeof(*provider_attributes_json);
    }
    total += audit.measure_deep_size_bytes() - sizeof(audit);
    total += aliases_scratch_.capacity() * sizeof(record_key_view);
    total += genes_scratch_.capacity() * sizeof(gene_reference_view);
    total += keywords_scratch_.capacity() * sizeof(std::string_view);
    total += locations_scratch_.capacity() * sizeof(genomic_interval);
    total += audit_view_storage_.measure_storage_bytes();
    return total;
  }

  [[nodiscard]] catalog_row_view view() const noexcept {
    catalog_row_view result;
    result.key = key.view();
    result.kind = kind;
    result.molecule = molecule;
    if (aliases) {
      aliases_scratch_.clear();
      aliases_scratch_.reserve(aliases->size());
      for (const auto &alias : *aliases) {
        aliases_scratch_.push_back(alias.view());
      }
      result.aliases = std::span<const record_key_view>(
          aliases_scratch_.data(), aliases_scratch_.size());
    }
    result.taxon_id = taxon_id;
    if (scientific_name)
      result.scientific_name = scientific_name->view();
    if (title)
      result.title = title->view();
    if (genes) {
      genes_scratch_.clear();
      genes_scratch_.reserve(genes->size());
      for (const auto &gene : *genes) {
        genes_scratch_.push_back(gene.view());
      }
      result.genes = std::span<const gene_reference_view>(
          genes_scratch_.data(), genes_scratch_.size());
    }
    if (keywords) {
      keywords_scratch_.clear();
      keywords_scratch_.reserve(keywords->size());
      for (const auto &kw : *keywords) {
        keywords_scratch_.push_back(kw.view());
      }
      result.keywords = std::span<const std::string_view>(
          keywords_scratch_.data(), keywords_scratch_.size());
    }
    result.host_taxon_id = host_taxon_id;
    if (geography)
      result.geography = geography->view();
    if (lineage_name)
      result.lineage_name = lineage_name->view();
    result.length = length;
    result.is_reference = is_reference;
    result.is_reviewed = is_reviewed;
    result.is_annotated = is_annotated;
    result.is_fragment = is_fragment;
    if (locations) {
      locations_scratch_.clear();
      locations_scratch_.reserve(locations->size());
      for (const auto &loc : *locations) {
        locations_scratch_.push_back(loc.view());
      }
      result.locations = std::span<const genomic_interval>(
          locations_scratch_.data(), locations_scratch_.size());
    }
    if (assembly_accession) {
      result.assembly_accession = assembly_accession->view();
    }
    if (assembly_database)
      result.assembly_database = assembly_database->view();
    if (assembly_source)
      result.assembly_source = assembly_source->view();
    if (assembly_level)
      result.assembly_level = assembly_level->view();
    if (assembly_status)
      result.assembly_status = assembly_status->view();
    result.release_date = release_date;
    result.update_date = update_date;
    result.is_retrievable = is_retrievable;
    if (provider_attributes_json) {
      result.provider_attributes_json = provider_attributes_json->view();
    }
    result.audit = audit_view_storage_.make_view(audit);
    return result;
  }
};

} // namespace bioseq::external::provider
