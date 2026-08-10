module;

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

export module bioseq.external.providers.ncbi;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.model;
import bioseq.external.provider.messages;
import bioseq.external.provider.query;
import bioseq.external.provider.capabilities;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.locator;
import bioseq.external.provider.budget;
import bioseq.external.provider.concepts;
import bioseq.external.json;
import bioseq.external.api.ncbi;
import bioseq.external.https;
import bioseq.common.context;
import bioseq.parallelism.pool;
import bioseq.common.templating.compile_time_string;

/// @brief NCBI Datasets v2 metadata adapter.
// Discovery uses the generated GET
// client (control plane); this adapter normalizes the taxonomy endpoints
// (tax_name_query / taxonomy_data_report) into taxonomy rows and the genome
// assembly report endpoints (genome_dataset_report /
// genome_dataset_reports_by_taxon) into catalog rows, and lowers a genome
// retrieval into a report operation. No PostgreSQL, alphabet, or transfer
// dependency; parsing reads the owned JSON tree while simdjson remains private
// to its implementation unit. An NCBI_API_KEY environment variable, when
// present, is forwarded to the generated client as credentials.
export namespace bioseq::external::providers {

namespace prov = bioseq::external::provider;
namespace api = bioseq::external::api::ncbi;

using prov::external_error;
using prov::external_error_kind;
using prov::make_external_error;
using prov::provider_id;

inline prov::provider_id ncbi_provider_id() noexcept {
  return {.value = "ncbi"};
}

inline prov::bounded_string<prov::maximum_provider_name_bytes>
ncbi_provider_name() {
  return prov::bounded_string<
      prov::maximum_provider_name_bytes>::make_truncated("NCBI");
}

inline prov::bounded_string<prov::maximum_identifier_bytes>
ncbi_bounded_id(std::string_view value) {
  return prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(
      value);
}

// Compact, validated, non-secret plan.
struct ncbi_plan {
  prov::query_target target{prov::query_target::taxonomy};
  std::string operation_id{"tax_name_query"};
  // Path value for the taxon-keyed operations: a scientific/common name for
  // tax_name_query, or a taxon-id string for taxonomy_data_report /
  // genome_dataset_reports_by_taxon.
  std::string taxon_term;
  // Path value for genome_dataset_report (a genome assembly accession).
  std::string accession;
  std::optional<std::int64_t> taxon_id;
  std::optional<std::uint64_t> limit;

  [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
    return sizeof(ncbi_plan) + operation_id.capacity() + taxon_term.capacity() +
           accession.capacity();
  }

  [[nodiscard]] prov::external_execution_key
  execution_fingerprint() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::string_view text) {
      for (const char symbol : text) {
        hash ^= static_cast<unsigned char>(symbol);
        hash *= 1099511628211ULL;
      }
    };
    mix(operation_id);
    mix(taxon_term);
    mix(accession);
    mix("ncbi");
    prov::external_execution_key key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
      key[index] =
          static_cast<std::byte>((hash >> ((index % 8U) * 8U)) & 0xFFU);
    }
    return key;
  }
};

class ncbi_session {
public:
  ncbi_session(const context::context &memory,
               const bioseq::parallelism::global_pool &pool, ncbi_plan plan)
      : memory_(&memory), pool_(&pool), plan_(std::move(plan)),
        stop_(std::make_unique<std::atomic<bool>>(false)) {}

  ncbi_session(ncbi_session &&) noexcept = default;
  ncbi_session &operator=(ncbi_session &&) noexcept = default;

  std::expected<prov::provider_statistics, external_error>
  run(prov::provider_message_sink_ref sink, std::stop_token stop) {
    prov::provider_statistics stats;

    // Nucleotide sequence retrieval does not go through the Datasets v2 JSON
    // client: that API returns bases only inside ZIP packages, which cannot
    // pass through this JSON path and its body cap. The
    // sequence / sequence_chunk targets fetch FASTA over HTTPS from NCBI's
    // sequence locator and emit target-aware rows. This channel is separate
    // from the Datasets metadata client.
    if (plan_.target == prov::query_target::sequence ||
        plan_.target == prov::query_target::sequence_chunk) {
      return run_sequence_retrieval(sink, stop);
    }

    // Forward NCBI_API_KEY when present; otherwise run unauthenticated.
    std::optional<api::credentials> credentials;
    if (const char *api_key = std::getenv("NCBI_API_KEY");
        api_key != nullptr && *api_key != '\0') {
      credentials = api::credentials{std::string(api_key)};
    }
    api::bioseq_external_api_client client(*memory_, *pool_,
                                           std::move(credentials));

    const std::vector<api::parameter_value> parameters = make_parameters();

    auto response = client.get(plan_.operation_id, parameters);
    if (!response.has_value()) {
      return std::unexpected(make_external_error(
          external_error_kind::transport, ncbi_provider_id(),
          plan_.operation_id, response.error().message));
    }
    stats.requests = 1;
    stats.wire_bytes = response->body_view().size();
    stats.decoded_bytes = response->body_view().size();
    if (response->status_code != 200) {
      return std::unexpected(make_external_error(
          external_error_kind::http, ncbi_provider_id(), plan_.operation_id,
          "NCBI returned a non-200 status"));
    }

    std::expected<void, external_error> parsed =
        plan_.target == prov::query_target::taxonomy
            ? emit_taxonomy_rows(response->body_view(), sink, stats, stop)
        : plan_.target == prov::query_target::record_location
            ? emit_location_rows(response->body_view(), sink, stats, stop)
            : emit_catalog_rows(response->body_view(), sink, stats, stop);
    if (!parsed.has_value()) {
      return std::unexpected(parsed.error());
    }
    stats.is_complete = true;
    return stats;
  }

  void request_stop() noexcept {
    if (stop_) {
      stop_->store(true);
    }
  }

private:
  // Bind the single path parameter each supported operation requires. Only
  // parameters known to the operation are supplied; the generated client
  // rejects unknown ones.
  [[nodiscard]] std::vector<api::parameter_value> make_parameters() const {
    std::vector<api::parameter_value> parameters;
    if (plan_.operation_id == "tax_name_query") {
      parameters.push_back({.name = "taxon_query",
                            .location = api::parameter_location::path,
                            .value = std::string_view{plan_.taxon_term}});
    } else if (plan_.operation_id == "taxonomy_data_report" ||
               plan_.operation_id == "genome_dataset_reports_by_taxon") {
      parameters.push_back({.name = "taxons",
                            .location = api::parameter_location::path,
                            .value = std::string_view{plan_.taxon_term}});
    } else if (plan_.operation_id == "genome_dataset_report") {
      parameters.push_back({.name = "accessions",
                            .location = api::parameter_location::path,
                            .value = std::string_view{plan_.accession}});
    } else if (plan_.operation_id == "gene_dataset_reports_by_id") {
      // gene_ids is an integer array path parameter; supply the single
      // queried gene id parsed from the accession predicate.
      std::int64_t gene_id = 0;
      const auto [parsed_end, error_code] = std::from_chars(
          plan_.accession.data(),
          plan_.accession.data() + plan_.accession.size(), gene_id);
      (void)parsed_end;
      if (error_code == std::errc()) {
        parameters.push_back({.name = "gene_ids",
                              .location = api::parameter_location::path,
                              .value = gene_id});
      }
    }
    return parameters;
  }

  static prov::external_timestamp completion_now() noexcept {
    return std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now());
  }

  // Push one warning diagnostic describing an incomplete result set.
  std::expected<void, external_error>
  emit_partial_diagnostic(prov::provider_message_sink_ref sink,
                          std::string_view code, std::string_view message) {
    prov::owning_provider_diagnostic diagnostic;
    diagnostic.severity = prov::diagnostic_severity::warning;
    diagnostic.provider = ncbi_provider_id();
    diagnostic.provider_name = ncbi_provider_name();
    diagnostic.operation = ncbi_bounded_id(plan_.operation_id);
    diagnostic.code = prov::bounded_string<256>::make_truncated(code);
    diagnostic.message =
        prov::bounded_string<prov::maximum_diagnostic_bytes>::make_truncated(
            message);
    diagnostic.is_retryable = false;
    return sink.push_fn(sink.object,
                        prov::owning_provider_message{std::move(diagnostic)});
  }

  // Emit a partial-result warning when the plan limit truncated the page or
  // the payload advertises a further page token.
  std::expected<void, external_error>
  maybe_emit_partial(const bioseq::external::json::value &document,
                     prov::provider_message_sink_ref sink,
                     prov::provider_statistics &stats, bool truncated) {
    bool has_more = truncated;
    if (const auto token = document.read_string_member("next_page_token");
        token.has_value() && !token->empty()) {
      has_more = true;
    }
    if (!has_more) {
      return {};
    }
    auto emitted = emit_partial_diagnostic(
        sink, "partial_results",
        truncated ? "NCBI result truncated at the plan limit"
                  : "additional NCBI pages available (page token present)");
    if (!emitted.has_value()) {
      return std::unexpected(emitted.error());
    }
    ++stats.warnings;
    return {};
  }

  struct parsed_taxon_identity {
    std::string provider_identifier;
    std::optional<std::int64_t> numeric_identifier;
  };

  // Reads a taxon identity encoded as either a JSON integer or string.
  [[nodiscard]] static parsed_taxon_identity
  parse_taxon_identity(const bioseq::external::json::value &node) {
    if (const auto integer = node.read_signed_integer_member("tax_id")) {
      return {
          .provider_identifier = std::to_string(*integer),
          .numeric_identifier = *integer,
      };
    }
    if (const auto text = node.read_string_member("tax_id")) {
      std::int64_t parsed = 0;
      std::optional<std::int64_t> numeric_identifier;
      if (const auto result = std::from_chars(
              text->data(), text->data() + text->size(), parsed);
          result.ec == std::errc()) {
        numeric_identifier = parsed;
      }
      return {
          .provider_identifier = std::string(*text),
          .numeric_identifier = numeric_identifier,
      };
    }
    return {};
  }

  // Normalize a tax_name_query or taxonomy_data_report payload into taxonomy
  // rows. Field paths are tried defensively; missing fields are tolerated.
  std::expected<void, external_error>
  emit_taxonomy_rows(std::string_view body,
                     prov::provider_message_sink_ref sink,
                     prov::provider_statistics &stats, std::stop_token stop) {
    namespace json = bioseq::external::json;
    auto document = json::parse_document(body);
    if (!document.has_value()) {
      return std::unexpected(make_external_error(
          external_error_kind::parse, ncbi_provider_id(), plan_.operation_id,
          "NCBI taxonomy JSON did not parse"));
    }

    std::uint64_t emitted = 0;
    bool truncated = false;
    // Returns false once the plan limit is reached (caller stops the loop).
    const auto push_row = [&sink, &emitted,
                           this](prov::owning_taxonomy_row &&row)
        -> std::expected<bool, external_error> {
      auto pushed = sink.push_fn(sink.object,
                                 prov::owning_provider_message{std::move(row)});
      if (!pushed.has_value()) {
        return std::unexpected(pushed.error());
      }
      ++emitted;
      if (plan_.limit.has_value() && emitted >= *plan_.limit) {
        return false;
      }
      return true;
    };

    bool handled = false;

    // Shape A — tax_name_query: {"sci_name_and_ids":[{"sci_name":...,
    // "tax_id":"9606","common_name":...,"rank":"SPECIES"}, ...]}. tax_id is
    // a JSON string here.
    if (const json::value *items = document->find_member("sci_name_and_ids");
        items != nullptr && items->is_array()) {
      handled = true;
      for (const json::value &item : items->get_elements()) {
        if (stop.stop_requested() || stop_->load()) {
          break;
        }
        prov::owning_taxonomy_row row;
        row.provider = ncbi_provider_id();
        row.provider_name = ncbi_provider_name();

        const parsed_taxon_identity taxon_identity = parse_taxon_identity(item);
        row.provider_taxon_id =
            ncbi_bounded_id(taxon_identity.provider_identifier);
        row.taxon_id = taxon_identity.numeric_identifier;
        if (const auto name = item.read_string_member("sci_name")) {
          row.scientific_name = ncbi_bounded_id(*name);
        }
        if (const auto common = item.read_string_member("common_name");
            common.has_value() && !common->empty()) {
          row.common_name = ncbi_bounded_id(*common);
        }
        if (const auto rank = item.read_string_member("rank");
            rank.has_value() && !rank->empty()) {
          row.rank = ncbi_bounded_id(*rank);
        }
        row.audit.completed_at = completion_now();

        auto keep_going = push_row(std::move(row));
        if (!keep_going.has_value()) {
          return std::unexpected(keep_going.error());
        }
        if (!*keep_going) {
          truncated = true;
          break;
        }
      }
    }

    // Shape B — taxonomy_data_report: {"reports":[{"taxonomy":{"tax_id":
    // 9606,"rank":"SPECIES","current_scientific_name":{"name":"Homo
    // sapiens"},"parents":[...]}}]}. tax_id is a JSON integer here and the
    // scientific name is nested under current_scientific_name.name.
    if (!handled) {
      if (const json::value *reports = document->find_member("reports");
          reports != nullptr && reports->is_array()) {
        handled = true;
        for (const json::value &report : reports->get_elements()) {
          if (stop.stop_requested() || stop_->load()) {
            break;
          }
          const json::value *taxonomy = report.find_member("taxonomy");
          if (taxonomy == nullptr || !taxonomy->is_object()) {
            continue;
          }

          prov::owning_taxonomy_row row;
          row.provider = ncbi_provider_id();
          row.provider_name = ncbi_provider_name();

          const parsed_taxon_identity taxon_identity =
              parse_taxon_identity(*taxonomy);
          row.provider_taxon_id =
              ncbi_bounded_id(taxon_identity.provider_identifier);
          row.taxon_id = taxon_identity.numeric_identifier;

          if (const json::value *current =
                  taxonomy->find_member("current_scientific_name")) {
            if (const auto name = current->read_string_member("name")) {
              row.scientific_name = ncbi_bounded_id(*name);
            }
          }
          if (!row.scientific_name.has_value()) {
            if (const auto name =
                    taxonomy->read_string_member("organism_name")) {
              row.scientific_name = ncbi_bounded_id(*name);
            }
          }
          if (const auto rank = taxonomy->read_string_member("rank");
              rank.has_value() && !rank->empty()) {
            row.rank = ncbi_bounded_id(*rank);
          }
          if (const auto common =
                  taxonomy->read_string_member("curator_common_name");
              common.has_value() && !common->empty()) {
            row.common_name = ncbi_bounded_id(*common);
          }

          // parents[] is root->immediate-parent ordered; the last
          // entry is the direct parent taxon.
          if (const json::value *parents = taxonomy->find_member("parents");
              parents != nullptr && parents->is_array()) {
            std::vector<std::int64_t> lineage;
            for (const json::value &parent : parents->get_elements()) {
              if (const auto parent_id = parent.read_signed_integer()) {
                lineage.push_back(*parent_id);
              }
            }
            if (!lineage.empty()) {
              row.parent_taxon_id = lineage.back();
              row.lineage = std::move(lineage);
            }
          }
          row.audit.completed_at = completion_now();

          auto keep_going = push_row(std::move(row));
          if (!keep_going.has_value()) {
            return std::unexpected(keep_going.error());
          }
          if (!*keep_going) {
            truncated = true;
            break;
          }
        }
      }
    }

    stats.rows = emitted;
    return maybe_emit_partial(*document, sink, stats, truncated);
  }

  // Normalize a genome_dataset_report / genome_dataset_reports_by_taxon
  // payload into catalog rows.
  static std::uint64_t parse_u64(std::optional<std::string_view> text) {
    std::uint64_t value = 0;
    if (text.has_value() && !text->empty()) {
      std::from_chars(text->data(), text->data() + text->size(), value);
    }
    return value;
  }

  // Parse an NCBI gene dataset report (gene_dataset_reports_by_id) into one
  // catalog row per gene carrying its genomic intervals across every annotated
  // assembly. Live shape:
  //   reports[].gene.annotations[]
  //     .{assembly_accession, assembly_name}
  //     .genomic_locations[]
  //       .{genomic_accession_version, sequence_name}
  //       .genomic_range.{begin, end, orientation}
  // NCBI ranges are 1-based inclusive; the record_locations relation is
  // 0-based half-open, so start0 = begin-1 and end0 = end.
  std::expected<void, external_error>
  emit_location_rows(std::string_view body,
                     prov::provider_message_sink_ref sink,
                     prov::provider_statistics &stats, std::stop_token stop) {
    namespace json = bioseq::external::json;
    auto document = json::parse_document(body);
    if (!document.has_value()) {
      return std::unexpected(make_external_error(
          external_error_kind::parse, ncbi_provider_id(), plan_.operation_id,
          "NCBI gene JSON did not parse"));
    }

    std::uint64_t emitted = 0;
    if (const json::value *reports = document->find_member("reports");
        reports != nullptr && reports->is_array()) {
      for (const json::value &report : reports->get_elements()) {
        if (stop.stop_requested() || stop_->load()) {
          break;
        }
        const json::value *gene = report.find_member("gene");
        if (gene == nullptr || !gene->is_object()) {
          continue;
        }

        std::string gene_id = plan_.accession;
        if (const auto id = gene->read_string_member("gene_id");
            id.has_value() && !id->empty()) {
          gene_id.assign(*id);
        }

        prov::owning_catalog_row row;
        row.provider = ncbi_provider_id();
        row.provider_name = ncbi_provider_name();
        row.kind = prov::entity_kind::gene;
        row.molecule = prov::molecule_kind::nucleotide;
        row.is_retrievable = false;
        row.key.provider = ncbi_provider_id();
        row.key.provider_name = ncbi_provider_name();
        row.key.domain = ncbi_bounded_id("gene");
        row.key.original_identifier = ncbi_bounded_id(gene_id);
        row.key.accession = ncbi_bounded_id(gene_id);
        if (const auto symbol = gene->read_string_member("symbol");
            symbol.has_value() && !symbol->empty()) {
          row.title = ncbi_bounded_id(*symbol);
        }
        if (const auto tax = gene->read_signed_integer_member("tax_id")) {
          row.taxon_id = *tax;
        }

        std::vector<prov::owning_genomic_interval> intervals;
        if (const json::value *annotations = gene->find_member("annotations");
            annotations != nullptr && annotations->is_array()) {
          for (const json::value &ann : annotations->get_elements()) {
            const std::optional<std::string_view> asm_acc =
                ann.read_string_member("assembly_accession");
            const std::optional<std::string_view> asm_name =
                ann.read_string_member("assembly_name");
            const json::value *locs = ann.find_member("genomic_locations");
            if (locs == nullptr || !locs->is_array()) {
              continue;
            }
            for (const json::value &loc : locs->get_elements()) {
              const json::value *grange = loc.find_member("genomic_range");
              if (grange == nullptr) {
                continue;
              }
              prov::owning_genomic_interval gi;
              if (const auto seq =
                      loc.read_string_member("genomic_accession_version");
                  seq.has_value() && !seq->empty()) {
                gi.sequence_alias = ncbi_bounded_id(*seq);
              } else if (const auto sname =
                             loc.read_string_member("sequence_name");
                         sname.has_value()) {
                gi.sequence_alias = ncbi_bounded_id(*sname);
              }
              if (asm_acc.has_value() && !asm_acc->empty()) {
                gi.assembly_accession = ncbi_bounded_id(*asm_acc);
              }
              if (asm_name.has_value() && !asm_name->empty()) {
                gi.assembly_database = ncbi_bounded_id(*asm_name);
              }
              const std::uint64_t begin =
                  parse_u64(grange->read_string_member("begin"));
              const std::uint64_t end =
                  parse_u64(grange->read_string_member("end"));
              gi.start0 = begin > 0 ? begin - 1 : 0;
              gi.end0 = end;
              if (const auto orient =
                      grange->read_string_member("orientation")) {
                if (*orient == "minus") {
                  gi.orientation = prov::genomic_interval::strand::reverse;
                } else if (*orient == "plus") {
                  gi.orientation = prov::genomic_interval::strand::forward;
                }
              }
              intervals.push_back(std::move(gi));
            }
          }
        }
        if (!intervals.empty()) {
          row.locations = std::move(intervals);
        }
        row.audit.completed_at = completion_now();

        const auto pushed = sink.push_fn(
            sink.object, prov::owning_provider_message{std::move(row)});
        if (!pushed.has_value()) {
          return std::unexpected(pushed.error());
        }
        ++emitted;
      }
    }
    stats.rows = emitted;
    return {};
  }

  std::expected<void, external_error>
  emit_catalog_rows(std::string_view body, prov::provider_message_sink_ref sink,
                    prov::provider_statistics &stats, std::stop_token stop) {
    namespace json = bioseq::external::json;
    auto document = json::parse_document(body);
    if (!document.has_value()) {
      return std::unexpected(make_external_error(
          external_error_kind::parse, ncbi_provider_id(), plan_.operation_id,
          "NCBI genome JSON did not parse"));
    }

    std::uint64_t emitted = 0;
    bool truncated = false;
    if (const json::value *reports = document->find_member("reports");
        reports != nullptr && reports->is_array()) {
      for (const json::value &report : reports->get_elements()) {
        if (stop.stop_requested() || stop_->load()) {
          break;
        }
        if (!report.is_object()) {
          continue;
        }

        // An assembly accession keys the row; skip records lacking one.
        std::optional<std::string_view> accession =
            report.read_string_member("accession");
        if (!accession.has_value() || accession->empty()) {
          accession = report.read_string_member("current_accession");
        }
        if (!accession.has_value() || accession->empty()) {
          continue;
        }

        prov::owning_catalog_row row;
        row.provider = ncbi_provider_id();
        row.provider_name = ncbi_provider_name();
        row.kind = prov::entity_kind::assembly;
        row.molecule = prov::molecule_kind::nucleotide;
        row.is_retrievable = true;

        row.key.provider = ncbi_provider_id();
        row.key.provider_name = ncbi_provider_name();
        row.key.domain = ncbi_bounded_id("genome");
        row.key.original_identifier = ncbi_bounded_id(*accession);
        row.key.accession = ncbi_bounded_id(*accession);
        row.assembly_accession = ncbi_bounded_id(*accession);

        if (const json::value *organism = report.find_member("organism")) {
          if (const auto taxon =
                  organism->read_signed_integer_member("tax_id")) {
            row.taxon_id = *taxon;
          }
          std::optional<std::string_view> scientific =
              organism->read_string_member("organism_name");
          if (!scientific.has_value() || scientific->empty()) {
            scientific = organism->read_string_member("sci_name");
          }
          if (scientific.has_value() && !scientific->empty()) {
            row.scientific_name = ncbi_bounded_id(*scientific);
          }
        }
        if (const json::value *info = report.find_member("assembly_info")) {
          if (const auto name = info->read_string_member("assembly_name");
              name.has_value() && !name->empty()) {
            row.title = ncbi_bounded_id(*name);
          }
          if (const auto level = info->read_string_member("assembly_level");
              level.has_value() && !level->empty()) {
            row.assembly_level = ncbi_bounded_id(*level);
          }
          if (const auto status = info->read_string_member("assembly_status");
              status.has_value() && !status->empty()) {
            row.assembly_status = ncbi_bounded_id(*status);
          }
        }
        if (const auto source = report.read_string_member("source_database");
            source.has_value() && !source->empty()) {
          row.assembly_source = ncbi_bounded_id(*source);
        }
        row.audit.completed_at = completion_now();

        const auto pushed = sink.push_fn(
            sink.object, prov::owning_provider_message{std::move(row)});
        if (!pushed.has_value()) {
          return std::unexpected(pushed.error());
        }
        ++emitted;
        if (plan_.limit.has_value() && emitted >= *plan_.limit) {
          truncated = true;
          break;
        }
      }
    }

    stats.rows = emitted;
    return maybe_emit_partial(*document, sink, stats, truncated);
  }

  // ----- Nucleotide sequence retrieval (sequences / sequence_chunks) ------

  // Largest valid-base count for a non-final dna5/64 chunk: the largest
  // multiple of the 21 bases-per-storage-unit not exceeding 1 Mi bases.
  static constexpr std::uint64_t k_sequence_chunk_bases = 1048572;

  // Concatenated bases of the first FASTA record in `body`, dropping header
  // lines ('>') and interior whitespace. dna5 packing normalizes case and
  // validates the alphabet downstream.
  static std::string parse_fasta_bases(std::string_view body) {
    std::string bases;
    bases.reserve(body.size());
    std::size_t i = 0;
    while (i < body.size()) {
      const std::size_t nl = body.find('\n', i);
      const std::string_view line = body.substr(
          i, nl == std::string_view::npos ? body.size() - i : nl - i);
      i = (nl == std::string_view::npos) ? body.size() : nl + 1;
      if (!line.empty() && line.front() == '>') {
        if (!bases.empty()) {
          break; // second record — keep only the first
        }
        continue;
      }
      for (const char symbol : line) {
        if (symbol != '\r' && symbol != ' ' && symbol != '\t') {
          bases.push_back(symbol);
        }
      }
    }
    return bases;
  }

  // Shared owning_sequence_payload identity for the queried record.
  prov::owning_sequence_payload sequence_payload_template() const {
    prov::owning_sequence_payload payload;
    payload.provider = ncbi_provider_id();
    payload.provider_name = ncbi_provider_name();
    payload.molecule = prov::molecule_kind::nucleotide;
    payload.key.provider = ncbi_provider_id();
    payload.key.provider_name = ncbi_provider_name();
    payload.key.domain = ncbi_bounded_id("sequence");
    payload.key.original_identifier = ncbi_bounded_id(plan_.accession);
    payload.key.accession = ncbi_bounded_id(plan_.accession);
    payload.strand = prov::genomic_interval::strand::forward;
    payload.audit.completed_at = completion_now();
    return payload;
  }

  // sequences -> one whole-record row; sequence_chunks -> ordered chunk rows
  // of at most k_sequence_chunk_bases bases with monotonic index / base_offset
  // and end_of_record on the final chunk.
  std::expected<std::uint64_t, external_error>
  emit_sequence_payloads(prov::provider_message_sink_ref sink,
                         const std::string &bases, std::stop_token stop) {
    if (plan_.target == prov::query_target::sequence) {
      prov::owning_sequence_payload payload = sequence_payload_template();
      payload.bases = bases;
      payload.end_of_record = true;
      const auto pushed = sink.push_fn(
          sink.object, prov::owning_provider_message{std::move(payload)});
      if (!pushed.has_value()) {
        return std::unexpected(pushed.error());
      }
      return std::uint64_t{1};
    }

    const std::uint64_t total = bases.size();
    std::uint64_t offset = 0;
    std::uint64_t index = 0;
    std::uint64_t rows = 0;
    while (offset < total) {
      if (stop.stop_requested() || stop_->load()) {
        break;
      }
      const std::uint64_t remaining = total - offset;
      const std::uint64_t take = remaining < k_sequence_chunk_bases
                                     ? remaining
                                     : k_sequence_chunk_bases;
      prov::owning_sequence_payload payload = sequence_payload_template();
      payload.bases.assign(bases, static_cast<std::size_t>(offset),
                           static_cast<std::size_t>(take));
      payload.chunk_index = index;
      payload.base_offset = offset;
      payload.end_of_record = (offset + take >= total);
      const auto pushed = sink.push_fn(
          sink.object, prov::owning_provider_message{std::move(payload)});
      if (!pushed.has_value()) {
        return std::unexpected(pushed.error());
      }
      ++rows;
      offset += take;
      ++index;
    }
    return rows;
  }

  // Fetch FASTA for plan_.accession over HTTPS and emit it as one sequences
  // row or ordered sequence_chunks rows.
  std::expected<prov::provider_statistics, external_error>
  run_sequence_retrieval(prov::provider_message_sink_ref sink,
                         std::stop_token stop) {
    prov::provider_statistics stats;
    if (plan_.accession.empty()) {
      return std::unexpected(make_external_error(
          external_error_kind::unsupported_capability, ncbi_provider_id(),
          "efetch_nuccore_fasta",
          "NCBI sequence retrieval requires an accession predicate"));
    }

    // Direct HTTPS FASTA locator. Efetch is unauthenticated, so credentials
    // never enter the URL.
    bioseq::external::https_client client("eutils.ncbi.nlm.nih.gov");
    std::string path = "/entrez/eutils/efetch.fcgi?db=nuccore&id=";
    path += plan_.accession;
    path += "&rettype=fasta&retmode=text";

    auto response = client.get(path);
    if (!response.has_value()) {
      return std::unexpected(make_external_error(
          external_error_kind::transport, ncbi_provider_id(),
          "efetch_nuccore_fasta", response.error().message));
    }
    stats.requests = 1;
    stats.wire_bytes = response->body.size();
    stats.decoded_bytes = response->body.size();
    if (response->status_code != 200) {
      return std::unexpected(make_external_error(
          external_error_kind::http, ncbi_provider_id(), "efetch_nuccore_fasta",
          "NCBI efetch returned a non-200 status"));
    }

    const std::string bases = parse_fasta_bases(response->body);
    if (bases.empty()) {
      return std::unexpected(make_external_error(
          external_error_kind::parse, ncbi_provider_id(),
          "efetch_nuccore_fasta", "NCBI efetch returned no sequence bases"));
    }

    auto emitted = emit_sequence_payloads(sink, bases, stop);
    if (!emitted.has_value()) {
      return std::unexpected(emitted.error());
    }
    stats.rows = *emitted;
    stats.is_complete = true;
    return stats;
  }

  const context::context *memory_;
  const bioseq::parallelism::global_pool *pool_;
  ncbi_plan plan_;
  std::unique_ptr<std::atomic<bool>> stop_;
};

static_assert(prov::external_api_session_compatible<ncbi_session>);

struct ncbi_provider {
  static constexpr auto id = bioseq::common::templating::compile_time_string("ncbi");
  static constexpr auto name = bioseq::common::templating::compile_time_string("NCBI");
  static constexpr bool supports_sequence_retrieval = true;

  using plan_type = ncbi_plan;
  using session_type = ncbi_session;

  static prov::provider_capabilities make_potential_capabilities() {
    prov::provider_capabilities caps;
    caps.flags.set(
        static_cast<std::size_t>(prov::provider_capability::taxonomy_search));
    caps.flags.set(
        static_cast<std::size_t>(prov::provider_capability::catalog_search));
    caps.flags.set(static_cast<std::size_t>(
        prov::provider_capability::nucleotide_locator));
    caps.flags.set(
        static_cast<std::size_t>(prov::provider_capability::direct_inspect));
    caps.flags.set(
        static_cast<std::size_t>(prov::provider_capability::exact_taxon));
    caps.flags.set(
        static_cast<std::size_t>(prov::provider_capability::descendant_taxon));
    caps.maximum_metadata_page = 1000;
    caps.maximum_identifier_batch = 1;
    return caps;
  }

  static std::span<const prov::request_policy_profile_view>
  list_request_policy_profiles() {
    return {};
  }

  static std::span<const prov::locator_policy_profile_view>
  list_locator_policy_profiles() {
    return {};
  }

  static std::expected<prov::provider_plan_estimate, external_error>
  estimate(const prov::normalized_external_query &query,
           const prov::provider_capability_snapshot &,
           const prov::external_planning_limits &) {
    prov::provider_plan_estimate estimate;
    estimate.plan_bytes = 4096;
    estimate.metadata_page_bytes = 2U * 1024U * 1024U;
    estimate.staging_message_bytes = 64U * 1024U;
    estimate.transfer_count =
        (query.target == prov::query_target::sequence ||
         query.target == prov::query_target::sequence_chunk)
            ? 1U
            : 0U;
    return estimate;
  }

  static std::expected<ncbi_plan, external_error>
  plan(const prov::normalized_external_query &query,
       const prov::provider_capability_snapshot &,
       const prov::external_slot_budget &) {
    // Pull the first usable text/taxon/accession predicate out of the query
    // so the plan targets a specific record when one was requested.
    std::optional<std::string_view> text_term;
    std::optional<std::int64_t> taxon;
    std::optional<std::string_view> accession;
    for (const prov::predicate_node &node : query.predicates) {
      if (const auto *text =
              std::get_if<prov::text_search_predicate>(&node.leaf)) {
        if (!text_term) {
          text_term = text->query.view();
        }
      } else if (const auto *taxon_leaf =
                     std::get_if<prov::taxon_predicate>(&node.leaf)) {
        if (!taxon) {
          taxon = taxon_leaf->taxon_id;
        }
      } else if (const auto *comparison =
                     std::get_if<prov::comparison_predicate>(&node.leaf)) {
        if (comparison->operation != prov::predicate_operator::equal ||
            comparison->values.empty()) {
          continue;
        }
        const prov::external_scalar &first = comparison->values.front();
        switch (comparison->field) {
        case prov::external_field::scientific_name:
        case prov::external_field::common_name:
          if (const auto *value = std::get_if<
                  prov::bounded_string<prov::maximum_identifier_bytes>>(&first);
              value != nullptr && !text_term) {
            text_term = value->view();
          }
          break;
        case prov::external_field::taxon_id:
        case prov::external_field::parent_taxon_id:
          if (const auto *value = std::get_if<std::int64_t>(&first);
              value != nullptr && !taxon) {
            taxon = *value;
          }
          break;
        case prov::external_field::accession:
        case prov::external_field::assembly_accession:
          if (const auto *value = std::get_if<
                  prov::bounded_string<prov::maximum_identifier_bytes>>(&first);
              value != nullptr && !accession) {
            accession = value->view();
          }
          break;
        default:
          break;
        }
      }
    }

    ncbi_plan plan;
    plan.target = query.target;
    plan.limit = query.limit;
    if (query.target == prov::query_target::taxonomy) {
      if (taxon) {
        plan.operation_id = "taxonomy_data_report";
        plan.taxon_id = taxon;
        plan.taxon_term = std::to_string(*taxon);
      } else if (text_term) {
        plan.operation_id = "tax_name_query";
        plan.taxon_term.assign(*text_term);
      } else {
        // A bounded default keeps discovery probes well-formed.
        plan.operation_id = "tax_name_query";
        plan.taxon_term = "Homo sapiens";
      }
    } else if (query.target == prov::query_target::record_location) {
      // record_locations lowers to the NCBI gene dataset report, keyed by
      // the queried gene id (the accession predicate). Genomic intervals
      // are parsed from the gene's per-assembly annotations.
      plan.operation_id = "gene_dataset_reports_by_id";
      if (accession) {
        plan.accession.assign(*accession);
      }
    } else if (query.target == prov::query_target::sequence ||
               query.target == prov::query_target::sequence_chunk) {
      // Nucleotide sequence retrieval is keyed by the queried accession;
      // the session fetches FASTA over HTTPS (run_sequence_retrieval) and
      // splits it into chunk rows for the sequence_chunk relation.
      plan.operation_id = "efetch_nuccore_fasta";
      if (accession) {
        plan.accession.assign(*accession);
      }
    } else {
      // Catalog target resolves to a genome assembly report.
      if (accession) {
        plan.operation_id = "genome_dataset_report";
        plan.accession.assign(*accession);
      } else if (taxon) {
        plan.operation_id = "genome_dataset_reports_by_taxon";
        plan.taxon_id = taxon;
        plan.taxon_term = std::to_string(*taxon);
      } else {
        plan.operation_id = "genome_dataset_reports_by_taxon";
        plan.taxon_term = "9606";
      }
    }
    return plan;
  }

  static std::expected<ncbi_session, external_error>
  open(context::context &worker_memory,
       const bioseq::parallelism::global_pool &pool,
       prov::external_runtime_services &, std::optional<prov::credential_view>,
       const ncbi_plan &plan, const prov::external_slot_budget &) {
    return ncbi_session(worker_memory, pool, plan);
  }
};

static_assert(prov::external_api_provider_compatible<ncbi_provider>);

} // namespace bioseq::external::providers
