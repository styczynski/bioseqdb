/**
 * @file external/providers/ncbi.cppm
 * @brief Read NCBI Datasets metadata and fetch nucleotide FASTA records.
 */
module;

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <limits>
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

/// NCBI Datasets metadata and EFetch sequence adapter.
export namespace bioseq::external::providers {

namespace prov = bioseq::external::provider;
namespace api = bioseq::external::api::ncbi;

using prov::external_error;
using prov::external_error_kind;
using prov::make_external_error;
using prov::provider_id;

/// Return the provider ID used for NCBI rows and errors.
inline prov::provider_id ncbi_provider_id() noexcept {
    return {.value = "ncbi"};
}

/// Return the bounded NCBI display name stored in rows.
inline prov::bounded_string<prov::maximum_provider_name_bytes> ncbi_provider_name() {
    return prov::bounded_string<prov::maximum_provider_name_bytes>::make_truncated("NCBI");
}

/// Copy an identifier, truncating it at the shared byte limit.
inline prov::bounded_string<prov::maximum_identifier_bytes>
ncbi_bounded_id(std::string_view value) {
    return prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(value);
}

/// NCBI operation and  looklp values chosen for a query.
struct ncbi_plan {
    prov::query_target target{prov::query_target::taxonomy};
    std::string operation_id{"tax_name_query"};
    // Taxon name for tax_name_query, or a numeric TaxID written as text
    // for taxonomy_data_report and genome_dataset_reports_by_taxon.
    std::string taxon_term;
    // Assembly or sequence accession, or GeneID for a gene-location request
    std::string accession;
    std::optional<std::int64_t> taxon_id;
    std::optional<std::uint64_t> limit;
    // Optional genomic interval, using zero-based, half-open coordinates.
    std::optional<std::uint64_t> start0;
    std::optional<std::uint64_t> end0;

    /// Estimate storage for the plan and its strings.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        return sizeof(ncbi_plan) + operation_id.capacity() + taxon_term.capacity() +
               accession.capacity();
    }

    /// Hash the operation, lookup values, and requested interval for plan identification.
    /// The target and row limit are excluded.
    [[nodiscard]] prov::external_execution_key execution_fingerprint() const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        // Add one text field to the plan hash.
        const auto mix = [&hash](std::string_view text) {
            for (const char symbol : text) {
                hash ^= static_cast<unsigned char>(symbol);
                hash *= 1099511628211ULL;
            }
        };
        mix(operation_id);
        mix(taxon_term);
        mix(accession);
        if (start0) {
            mix("range");
            for (const std::uint64_t coordinate : {*start0, *end0}) {
                for (unsigned shift = 0; shift < 64; shift += 8) {
                    hash ^= (coordinate >> shift) & 0xffU;
                    hash *= 1099511628211ULL;
                }
            }
        }
        mix("ncbi");
        prov::external_execution_key key{};
        for (std::size_t index = 0; index < key.size(); ++index) {
            key[index] = static_cast<std::byte>((hash >> ((index % 8U) * 8U)) & 0xFFU);
        }
        return key;
    }
};

/// Fetch NCBI metadata or a nucleotide, sequence for one plan.
class ncbi_session {
  public:
    /// Own the plan while borrowing the memory  context and worker pool.
    ncbi_session(const context::context& memory,
                 const bioseq::parallelism::global_pool& pool,
                 ncbi_plan plan)
        : memory_(&memory), pool_(&pool), plan_(std::move(plan)),
          stop_(std::make_unique<std::atomic<bool>>(false)) {}

    ncbi_session(ncbi_session&&) noexcept = default;
    ncbi_session& operator=(ncbi_session&&) noexcept = default;

    /// Fetch one metadata response or FASTA record and emit provider messages.
    std::expected<prov::provider_statistics, external_error>
    run(prov::provider_message_sink_ref sink, std::stop_token stop) {
        prov::provider_statistics stats;

        // Datasets returns sequence bases in ZIP packages, so sequence targets use
        // EFetch FASTA. Metadata requests use the Datasets JSON client below.
        if (plan_.target == prov::query_target::sequence ||
            plan_.target == prov::query_target::sequence_chunk) {
            return run_sequence_retrieval(sink, stop);
        }

        // Forward NCBI_API_KEY when present. Otherwise run unauthenticated.
        std::optional<api::credentials> credentials;
        if (const char* api_key = std::getenv("NCBI_API_KEY");
            api_key != nullptr && *api_key != '\0') {
            credentials = api::credentials{std::string(api_key)};
        }
        api::bioseq_external_api_client client(*memory_, *pool_, std::move(credentials));

        const std::vector<api::parameter_value> parameters = make_parameters();

        auto response = client.get(plan_.operation_id, parameters);
        if (!response.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::transport,
                                                       ncbi_provider_id(),
                                                       plan_.operation_id,
                                                       response.error().message));
        }
        stats.requests = 1;
        stats.wire_bytes = response->body_view().size();
        stats.decoded_bytes = response->body_view().size();
        if (response->status_code != 200) {
            return std::unexpected(make_external_error(external_error_kind::http,
                                                       ncbi_provider_id(),
                                                       plan_.operation_id,
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

    /// Ask the row and chunk emission loops to stop.
    void request_stop() noexcept {
        if (stop_) {
            stop_->store(true);
        }
    }

  private:
    /// Binds only the path parameters accepted by the selected operation.
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
            // Parse the queried GeneID for the generated integer path parameter.
            std::int64_t gene_id = 0;
            const auto [parsed_end, error_code] = std::from_chars(
                plan_.accession.data(), plan_.accession.data() + plan_.accession.size(), gene_id);
            (void)parsed_end;
            if (error_code == std::errc()) {
                parameters.push_back({.name = "gene_ids",
                                      .location = api::parameter_location::path,
                                      .value = gene_id});
            }
        }
        return parameters;
    }

    /// Return the current time at microsecond precision.
    static prov::external_timestamp completion_now() noexcept {
        return std::chrono::time_point_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now());
    }

    /// Pushes one warning diagnostic for an incomplete result set.
    std::expected<void, external_error> emit_partial_diagnostic(
        prov::provider_message_sink_ref sink, std::string_view code, std::string_view message) {
        prov::owning_provider_diagnostic diagnostic;
        diagnostic.severity = prov::diagnostic_severity::warning;
        diagnostic.provider = ncbi_provider_id();
        diagnostic.provider_name = ncbi_provider_name();
        diagnostic.operation = ncbi_bounded_id(plan_.operation_id);
        diagnostic.code = prov::bounded_string<256>::make_truncated(code);
        diagnostic.message =
            prov::bounded_string<prov::maximum_diagnostic_bytes>::make_truncated(message);
        diagnostic.is_retryable = false;
        return sink.push_fn(sink.object, prov::owning_provider_message{std::move(diagnostic)});
    }

    /// Warn when emission reached the limit or a next-page token is present.
    std::expected<void, external_error>
    maybe_emit_partial(const bioseq::external::json::value& document,
                       prov::provider_message_sink_ref sink,
                       prov::provider_statistics& stats,
                       bool truncated) {
        bool has_more = truncated;
        if (const auto token = document.read_string_member("next_page_token");
            token.has_value() && !token->empty()) {
            has_more = true;
        }
        if (!has_more) {
            return {};
        }
        auto emitted = emit_partial_diagnostic(
            sink,
            "partial_results",
            truncated ? "NCBI result truncated at the plan limit"
                      : "additional NCBI pages available (page token present)");
        if (!emitted.has_value()) {
            return std::unexpected(emitted.error());
        }
        ++stats.warnings;
        return {};
    }

    /// Taxon identifief as text and as a number when parsing succeeds.
    struct parsed_taxon_identity {
        std::string provider_identifier;
        std::optional<std::int64_t> numeric_identifier;
    };

    /// Reads a taxon identity encoded as either a JSON integer or string.
    [[nodiscard]] static parsed_taxon_identity
    parse_taxon_identity(const bioseq::external::json::value& node) {
        if (const auto integer = node.read_signed_integer_member("tax_id")) {
            return {
                .provider_identifier = std::to_string(*integer),
                .numeric_identifier = *integer,
            };
        }
        if (const auto text = node.read_string_member("tax_id")) {
            std::int64_t parsed = 0;
            std::optional<std::int64_t> numeric_identifier;
            if (const auto result =
                    std::from_chars(text->data(), text->data() + text->size(), parsed);
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

    /// Read  taxonomy search or report JSON, leaving missing fields unset.
    std::expected<void, external_error> emit_taxonomy_rows(std::string_view body,
                                                           prov::provider_message_sink_ref sink,
                                                           prov::provider_statistics& stats,
                                                           std::stop_token stop) {
        namespace json = bioseq::external::json;
        auto document = json::parse_document(body);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::parse,
                                                       ncbi_provider_id(),
                                                       plan_.operation_id,
                                                       "NCBI taxonomy JSON did not parse"));
        }

        std::uint64_t emitted = 0;
        bool truncated = false;
        // Returns false once the plan limit is reached (caller stops the loop).
        const auto push_row =
            [&sink, &emitted, this](
                prov::owning_taxonomy_row&& row) -> std::expected<bool, external_error> {
            auto pushed = sink.push_fn(sink.object, prov::owning_provider_message{std::move(row)});
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

        // Name searches return sci_name_and_ids with fields such as sci_name
        // and tax_id. The TaxID may be stored as text, for example "9606".
        if (const json::value* items = document->find_member("sci_name_and_ids");
            items != nullptr && items->is_array()) {
            handled = true;
            for (const json::value& item : items->get_elements()) {
                if (stop.stop_requested() || stop_->load()) {
                    break;
                }
                prov::owning_taxonomy_row row;
                row.provider = ncbi_provider_id();
                row.provider_name = ncbi_provider_name();

                const parsed_taxon_identity taxon_identity = parse_taxon_identity(item);
                row.provider_taxon_id = ncbi_bounded_id(taxon_identity.provider_identifier);
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

        // Taxonomy reports nest each record under reports[].taxonomy.
        // Read the scientific name from current_scientific_name.name and
        // accept an integer TaxID such as 9606.
        if (!handled) {
            if (const json::value* reports = document->find_member("reports");
                reports != nullptr && reports->is_array()) {
                handled = true;
                for (const json::value& report : reports->get_elements()) {
                    if (stop.stop_requested() || stop_->load()) {
                        break;
                    }
                    const json::value* taxonomy = report.find_member("taxonomy");
                    if (taxonomy == nullptr || !taxonomy->is_object()) {
                        continue;
                    }

                    prov::owning_taxonomy_row row;
                    row.provider = ncbi_provider_id();
                    row.provider_name = ncbi_provider_name();

                    const parsed_taxon_identity taxon_identity = parse_taxon_identity(*taxonomy);
                    row.provider_taxon_id = ncbi_bounded_id(taxon_identity.provider_identifier);
                    row.taxon_id = taxon_identity.numeric_identifier;

                    if (const json::value* current =
                            taxonomy->find_member("current_scientific_name")) {
                        if (const auto name = current->read_string_member("name")) {
                            row.scientific_name = ncbi_bounded_id(*name);
                        }
                    }
                    if (!row.scientific_name.has_value()) {
                        if (const auto name = taxonomy->read_string_member("organism_name")) {
                            row.scientific_name = ncbi_bounded_id(*name);
                        }
                    }
                    if (const auto rank = taxonomy->read_string_member("rank");
                        rank.has_value() && !rank->empty()) {
                        row.rank = ncbi_bounded_id(*rank);
                    }
                    if (const auto common = taxonomy->read_string_member("curator_common_name");
                        common.has_value() && !common->empty()) {
                        row.common_name = ncbi_bounded_id(*common);
                    }

                    // Treat the last entry in parents[] as the immediate parent
                    // Keep the full list as the lineage.
                    if (const json::value* parents = taxonomy->find_member("parents");
                        parents != nullptr && parents->is_array()) {
                        std::vector<std::int64_t> lineage;
                        for (const json::value& parent : parents->get_elements()) {
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

    /// Read an unsigned decimal prefix, returning zero when absent or unparseable.
    static std::uint64_t parse_u64(std::optional<std::string_view> text) {
        std::uint64_t value = 0;
        if (text.has_value() && !text->empty()) {
            std::from_chars(text->data(), text->data() + text->size(), value);
        }
        return value;
    }

    /// Emit gene catalog rows containing their  genomic locations.
    /// Convert one-based inclusive ranges to zero-based, half-open intervals.
    std::expected<void, external_error> emit_location_rows(std::string_view body,
                                                           prov::provider_message_sink_ref sink,
                                                           prov::provider_statistics& stats,
                                                           std::stop_token stop) {
        namespace json = bioseq::external::json;
        auto document = json::parse_document(body);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::parse,
                                                       ncbi_provider_id(),
                                                       plan_.operation_id,
                                                       "NCBI gene JSON did not parse"));
        }

        std::uint64_t emitted = 0;
        if (const json::value* reports = document->find_member("reports");
            reports != nullptr && reports->is_array()) {
            for (const json::value& report : reports->get_elements()) {
                if (stop.stop_requested() || stop_->load()) {
                    break;
                }
                const json::value* gene = report.find_member("gene");
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
                if (const json::value* annotations = gene->find_member("annotations");
                    annotations != nullptr && annotations->is_array()) {
                    for (const json::value& ann : annotations->get_elements()) {
                        const std::optional<std::string_view> asm_acc =
                            ann.read_string_member("assembly_accession");
                        const std::optional<std::string_view> asm_name =
                            ann.read_string_member("assembly_name");
                        const json::value* locs = ann.find_member("genomic_locations");
                        if (locs == nullptr || !locs->is_array()) {
                            continue;
                        }
                        for (const json::value& loc : locs->get_elements()) {
                            const json::value* grange = loc.find_member("genomic_range");
                            if (grange == nullptr) {
                                continue;
                            }
                            prov::owning_genomic_interval gi;
                            if (const auto seq =
                                    loc.read_string_member("genomic_accession_version");
                                seq.has_value() && !seq->empty()) {
                                gi.sequence_alias = ncbi_bounded_id(*seq);
                            } else if (const auto sname = loc.read_string_member("sequence_name");
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
                            const std::uint64_t end = parse_u64(grange->read_string_member("end"));
                            gi.start0 = begin > 0 ? begin - 1 : 0;
                            gi.end0 = end;
                            if (const auto orient = grange->read_string_member("orientation")) {
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

                const auto pushed =
                    sink.push_fn(sink.object, prov::owning_provider_message{std::move(row)});
                if (!pushed.has_value()) {
                    return std::unexpected(pushed.error());
                }
                ++emitted;
            }
        }
        stats.rows = emitted;
        return {};
    }

    /// Convert NCBI genome reports into assembly catalog rows.
    std::expected<void, external_error> emit_catalog_rows(std::string_view body,
                                                          prov::provider_message_sink_ref sink,
                                                          prov::provider_statistics& stats,
                                                          std::stop_token stop) {
        namespace json = bioseq::external::json;
        auto document = json::parse_document(body);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::parse,
                                                       ncbi_provider_id(),
                                                       plan_.operation_id,
                                                       "NCBI genome JSON did not parse"));
        }

        std::uint64_t emitted = 0;
        bool truncated = false;
        if (const json::value* reports = document->find_member("reports");
            reports != nullptr && reports->is_array()) {
            for (const json::value& report : reports->get_elements()) {
                if (stop.stop_requested() || stop_->load()) {
                    break;
                }
                if (!report.is_object()) {
                    continue;
                }

                // An assembly accession keys the row. Skip records lacking one.
                std::optional<std::string_view> accession = report.read_string_member("accession");
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

                if (const json::value* organism = report.find_member("organism")) {
                    if (const auto taxon = organism->read_signed_integer_member("tax_id")) {
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
                if (const json::value* info = report.find_member("assembly_info")) {
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

                const auto pushed =
                    sink.push_fn(sink.object, prov::owning_provider_message{std::move(row)});
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

    // Nucleotide sequences and chunks.

    // Keep non-final chunks aligned to dna5/64 storage units of 21 bases.
    // This is, the largest such chunk below 1 Mi bases.
    static constexpr std::uint64_t k_sequence_chunk_bases = 1048572;

    /// Collect FASTA bases until a later header follows a nonempty sequence.
    /// Skip headers, spaces, tabs, and line endings.
    static std::string parse_fasta_bases(std::string_view body) {
        std::string bases;
        bases.reserve(body.size());
        std::size_t i = 0;
        while (i < body.size()) {
            const std::size_t nl = body.find('\n', i);
            const std::string_view line =
                body.substr(i, nl == std::string_view::npos ? body.size() - i : nl - i);
            i = (nl == std::string_view::npos) ? body.size() : nl + 1;
            if (!line.empty() && line.front() == '>') {
                if (!bases.empty()) {
                    break; // A later record starts here. Keep the bases already collected.
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

    /// Returns shared sequence identity and audit fields for the queried record.
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
        payload.start0 = plan_.start0;
        payload.end0 = plan_.end0;
        payload.audit.completed_at = completion_now();
        return payload;
    }

    /// Emit the collecded bases as one row or as ordered sequence chunks.
    std::expected<std::uint64_t, external_error> emit_sequence_payloads(
        prov::provider_message_sink_ref sink, const std::string& bases, std::stop_token stop) {
        if (plan_.target == prov::query_target::sequence) {
            prov::owning_sequence_payload payload = sequence_payload_template();
            payload.bases = bases;
            payload.end_of_record = true;
            const auto pushed =
                sink.push_fn(sink.object, prov::owning_provider_message{std::move(payload)});
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
            const std::uint64_t take =
                remaining < k_sequence_chunk_bases ? remaining : k_sequence_chunk_bases;
            prov::owning_sequence_payload payload = sequence_payload_template();
            payload.bases.assign(
                bases, static_cast<std::size_t>(offset), static_cast<std::size_t>(take));
            payload.chunk_index = index;
            payload.base_offset = offset;
            payload.end_of_record = (offset + take >= total);
            const auto pushed =
                sink.push_fn(sink.object, prov::owning_provider_message{std::move(payload)});
            if (!pushed.has_value()) {
                return std::unexpected(pushed.error());
            }
            ++rows;
            offset += take;
            ++index;
        }
        return rows;
    }

    /// Download FASTA, collect  its first nonempty sequence, and emit rows.
    std::expected<prov::provider_statistics, external_error>
    run_sequence_retrieval(prov::provider_message_sink_ref sink, std::stop_token stop) {
        prov::provider_statistics stats;
        if (plan_.accession.empty()) {
            return std::unexpected(
                make_external_error(external_error_kind::unsupported_capability,
                                    ncbi_provider_id(),
                                    "efetch_nuccore_fasta",
                                    "NCBI sequence retrieval requires an accession predicate"));
        }

        // Fetch nucleotide FASTA directly, without adding authentication parameters.
        bioseq::external::https_client client("eutils.ncbi.nlm.nih.gov");
        std::string path = "/entrez/eutils/efetch.fcgi?db=nuccore&id=";
        path += plan_.accession;
        path += "&rettype=fasta&retmode=text";
        if (plan_.start0) {
            // EFetch uses one-based, closed coordinates. The exclusive end
            // has the same integer value in both coordinate systems.
            path += "&seq_start=" + std::to_string(*plan_.start0 + 1);
            path += "&seq_stop=" + std::to_string(*plan_.end0);
        }

        auto response = client.get(path);
        if (!response.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::transport,
                                                       ncbi_provider_id(),
                                                       "efetch_nuccore_fasta",
                                                       response.error().message));
        }
        stats.requests = 1;
        stats.wire_bytes = response->body.size();
        stats.decoded_bytes = response->body.size();
        if (response->status_code != 200) {
            return std::unexpected(make_external_error(external_error_kind::http,
                                                       ncbi_provider_id(),
                                                       "efetch_nuccore_fasta",
                                                       "NCBI efetch returned a non-200 status"));
        }

        const std::string bases = parse_fasta_bases(response->body);
        if (bases.empty()) {
            return std::unexpected(make_external_error(external_error_kind::parse,
                                                       ncbi_provider_id(),
                                                       "efetch_nuccore_fasta",
                                                       "NCBI efetch returned no sequence bases"));
        }
        if (plan_.start0 && bases.size() != *plan_.end0 - *plan_.start0) {
            return std::unexpected(make_external_error(
                external_error_kind::integrity, ncbi_provider_id(),
                "efetch_nuccore_fasta",
                "NCBI returned a different sequence length than the requested interval"));
        }

        auto emitted = emit_sequence_payloads(sink, bases, stop);
        if (!emitted.has_value()) {
            return std::unexpected(emitted.error());
        }
        stats.rows = *emitted;
        stats.is_complete = true;
        return stats;
    }

    const context::context* memory_;
    const bioseq::parallelism::global_pool* pool_;
    ncbi_plan plan_;
    std::unique_ptr<std::atomic<bool>> stop_;
};

static_assert(prov::external_api_session_compatible<ncbi_session>);

/// Provider adapter for NCBI metadata and nucleotide sequences.
struct ncbi_provider {
    static constexpr auto id = bioseq::common::templating::compile_time_string("ncbi");
    static constexpr auto name = bioseq::common::templating::compile_time_string("NCBI");
    static constexpr bool supports_sequence_retrieval = true;

    /// Request plan prepared by this adapter.
    using plan_type = ncbi_plan;
    /// Session executing a prepared request plan.
    using session_type = ncbi_session;

    /// Return the NCBI features advertised by this adapter.
    static prov::provider_capabilities make_potential_capabilities() {
        prov::provider_capabilities caps;
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::taxonomy_search));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::catalog_search));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::nucleotide_locator));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::direct_inspect));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::exact_taxon));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::descendant_taxon));
        caps.maximum_metadata_page = 1000;
        caps.maximum_identifier_batch = 1;
        return caps;
    }

    /// Return an empty list of request-policy profiles.
    static std::span<const prov::request_policy_profile_view> list_request_policy_profiles() {
        return {};
    }

    /// Return an empty list of URL-policy profiles.
    static std::span<const prov::locator_policy_profile_view> list_locator_policy_profiles() {
        return {};
    }

    /// Estimate plan and buffer sizes, reserving a transfer for sequence targets.
    static std::expected<prov::provider_plan_estimate, external_error>
    estimate(const prov::normalized_external_query& query,
             const prov::provider_capability_snapshot&,
             const prov::external_planning_limits&) {
        prov::provider_plan_estimate estimate;
        estimate.plan_bytes = 4096;
        estimate.metadata_page_bytes = 2U * 1024U * 1024U;
        estimate.staging_message_bytes = 64U * 1024U;
        estimate.transfer_count = (query.target == prov::query_target::sequence ||
                                   query.target == prov::query_target::sequence_chunk)
                                      ? 1U
                                      : 0U;
        return estimate;
    }

    /// Choose an NCBI operation from the query target and lookup filters.
    static std::expected<ncbi_plan, external_error>
    plan(const prov::normalized_external_query& query,
         const prov::provider_capability_snapshot&,
         const prov::external_slot_budget&) {
        // Take the first usable name, TaxID, and accession filters for the request
        std::optional<std::string_view> text_term;
        std::optional<std::int64_t> taxon;
        std::optional<std::string_view> accession;
        std::optional<std::int64_t> start0;
        std::optional<std::int64_t> end0;
        for (const prov::predicate_node& node : query.predicates) {
            if (const auto* text = std::get_if<prov::text_search_predicate>(&node.leaf)) {
                if (!text_term) {
                    text_term = text->query.view();
                }
            } else if (const auto* taxon_leaf = std::get_if<prov::taxon_predicate>(&node.leaf)) {
                if (!taxon) {
                    taxon = taxon_leaf->taxon_id;
                }
            } else if (const auto* comparison =
                           std::get_if<prov::comparison_predicate>(&node.leaf)) {
                if (comparison->operation != prov::predicate_operator::equal ||
                    comparison->values.empty()) {
                    continue;
                }
                const prov::external_scalar& first = comparison->values.front();
                switch (comparison->field) {
                case prov::external_field::scientific_name:
                case prov::external_field::common_name:
                    if (const auto* value =
                            std::get_if<prov::bounded_string<prov::maximum_identifier_bytes>>(
                                &first);
                        value != nullptr && !text_term) {
                        text_term = value->view();
                    }
                    break;
                case prov::external_field::taxon_id:
                case prov::external_field::parent_taxon_id:
                    if (const auto* value = std::get_if<std::int64_t>(&first);
                        value != nullptr && !taxon) {
                        taxon = *value;
                    }
                    break;
                case prov::external_field::accession:
                case prov::external_field::assembly_accession:
                    if (const auto* value =
                            std::get_if<prov::bounded_string<prov::maximum_identifier_bytes>>(
                                &first);
                        value != nullptr && !accession) {
                        accession = value->view();
                    }
                    break;
                case prov::external_field::start0:
                case prov::external_field::end0: {
                    std::optional<std::int64_t> coordinate;
                    if (const auto* value = std::get_if<std::int64_t>(&first)) {
                        coordinate = *value;
                    } else if (const auto* value = std::get_if<std::uint64_t>(&first);
                               value && *value <= static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max())) {
                        coordinate = static_cast<std::int64_t>(*value);
                    }
                    if (!coordinate) {
                        return std::unexpected(make_external_error(
                            external_error_kind::invalid_query, ncbi_provider_id(),
                            "efetch_nuccore_fasta", "NCBI interval bounds must be bigint values"));
                    }
                    if (comparison->field == prov::external_field::start0) {
                        start0 = *coordinate;
                    } else {
                        end0 = *coordinate;
                    }
                    break;
                }
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
                // Default an unfiltered taxonomy  lookup to Homo sapiens.
                plan.operation_id = "tax_name_query";
                plan.taxon_term = "Homo sapiens";
            }
        } else if (query.target == prov::query_target::record_location) {
            // Gene-location queries use the accession filter as a GeneID.
            // The session reads genomic intervals from each assembly annotation.
            plan.operation_id = "gene_dataset_reports_by_id";
            if (accession) {
                plan.accession.assign(*accession);
            }
        } else if (query.target == prov::query_target::sequence ||
                   query.target == prov::query_target::sequence_chunk) {
            // Fetch the accession as nucleotide FASTA. sequence_chunk results
            // split the collected bases into rows with increasing offsets.
            plan.operation_id = "efetch_nuccore_fasta";
            if (accession) {
                plan.accession.assign(*accession);
            }
            if (start0 || end0) {
                if (!start0 || !end0 || *start0 < 0 || *end0 <= *start0) {
                    return std::unexpected(make_external_error(
                        external_error_kind::invalid_query, ncbi_provider_id(),
                        "efetch_nuccore_fasta",
                        "NCBI sequence intervals require both start0 and end0 with 0 <= start0 < end0"));
                }
                plan.start0 = static_cast<std::uint64_t>(*start0);
                plan.end0 = static_cast<std::uint64_t>(*end0);
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
            } else if (text_term) {
                // NCBI Datasets accepts a scientific or common name here as
                // well as a TaxID. Preserve the requested organism instead
                // of querying the unfiltered default organism.
                plan.operation_id = "genome_dataset_reports_by_taxon";
                plan.taxon_term.assign(*text_term);
            } else {
                plan.operation_id = "genome_dataset_reports_by_taxon";
                plan.taxon_term = "9606";
            }
        }
        return plan;
    }

    /// Open a session that owns a copy of the plan.
    static std::expected<ncbi_session, external_error>
    open(context::context& worker_memory,
         const bioseq::parallelism::global_pool& pool,
         prov::external_runtime_services&,
         std::optional<prov::credential_view>,
         const ncbi_plan& plan,
         const prov::external_slot_budget&) {
        return ncbi_session(worker_memory, pool, plan);
    }
};

static_assert(prov::external_api_provider_compatible<ncbi_provider>);

} // namespace bioseq::external::providers
