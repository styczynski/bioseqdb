/**
 * @file external/providers/uniprot.cppm
 * @brief Read UniProtKB protein metadata into catalog rows.
 */
module;

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

export module bioseq.external.providers.uniprot;

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
import bioseq.external.api.uniprot;
import bioseq.common.context;
import bioseq.parallelism.pool;
import bioseq.common.templating.compile_time_string;

/// UniProtKB protein metadata adapter.
export namespace bioseq::external::providers {

namespace prov = bioseq::external::provider;
namespace api = bioseq::external::api::uniprot;

using prov::external_error;
using prov::external_error_kind;
using prov::make_external_error;
using prov::provider_id;

/// Return the provider ID used for UniProt rows and errors.
inline prov::provider_id uniprot_provider_id() noexcept {
    return {.value = "uniprot"};
}

/// Copy an identifier, truncating it at the shared byte limit.
inline prov::bounded_string<prov::maximum_identifier_bytes>
uniprot_bounded_id(std::string_view value) {
    return prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(value);
}

/// UniProt lookup or search request and its row limit.
struct uniprot_plan {
    prov::query_target target{prov::query_target::catalog};
    std::string operation_id{"searchCursor"};
    std::string accession;  // Set when an accession-equality predicate is present.
    std::string query{"*"}; // UniProtKB text or organism search expression.
    std::optional<std::uint64_t> limit;

    /// Estimate storage for the plan and its strings.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        return sizeof(uniprot_plan) + operation_id.capacity() + accession.capacity() +
               query.capacity();
    }

    /// Hash the operation, accession, and query for plan identification.
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
        mix(accession);
        mix(query);
        mix("uniprot");
        prov::external_execution_key key{};
        for (std::size_t index = 0; index < key.size(); ++index) {
            key[index] = static_cast<std::byte>((hash >> ((index % 8U) * 8U)) & 0xFFU);
        }
        return key;
    }
};

/// Fetch one UniProt response and send catalog rows to a consumer.
class uniprot_session {
  public:
    /// Own the plan while borrowing the memory context and worker pool.
    uniprot_session(const context::context& memory,
                    const bioseq::parallelism::global_pool& pool,
                    uniprot_plan plan)
        : memory_(&memory), pool_(&pool), plan_(std::move(plan)),
          stop_(std::make_unique<std::atomic<bool>>(false)) {}

    uniprot_session(uniprot_session&&) noexcept = default;
    uniprot_session& operator=(uniprot_session&&) noexcept = default;

    /// Fetch one entry or one search page and emit protein catalog rows.
    std::expected<prov::provider_statistics, external_error>
    run(prov::provider_message_sink_ref sink, std::stop_token stop) {
        prov::provider_statistics stats;
        api::bioseq_external_api_client client(*memory_, *pool_, std::nullopt);

        std::vector<api::parameter_value> params;
        if (plan_.operation_id == "getByAccession") {
            params.push_back({.name = "accession",
                              .location = api::parameter_location::path,
                              .value = std::string_view{plan_.accession}});
        } else {
            params.push_back({.name = "query",
                              .location = api::parameter_location::query,
                              .value = std::string_view{plan_.query}});
            if (plan_.limit.has_value()) {
                // Request at most 500 entries in this search page.
                const std::uint64_t capped = *plan_.limit > 500U ? 500U : *plan_.limit;
                params.push_back({.name = "size",
                                  .location = api::parameter_location::query,
                                  .value = static_cast<std::int64_t>(capped)});
            }
        }

        // Request JSON explicitly because the row parser expects a JSON response.
        auto response = client.get_with_accept(plan_.operation_id, params, "application/json");
        if (!response.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::transport,
                                                       uniprot_provider_id(),
                                                       plan_.operation_id,
                                                       response.error().message));
        }
        stats.requests = 1;
        stats.wire_bytes = response->body_view().size();
        stats.decoded_bytes = response->body_view().size();
        if (response->status_code != 200) {
            return std::unexpected(make_external_error(external_error_kind::http,
                                                       uniprot_provider_id(),
                                                       plan_.operation_id,
                                                       "UniProt returned a non-200 status"));
        }

        auto parsed = emit_catalog_rows(response->body_view(), sink, stats, stop);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        stats.is_complete = true;
        return stats;
    }

    /// Ask the search-result emission loop to stop.
    void request_stop() noexcept {
        if (stop_) {
            stop_->store(true);
        }
    }

  private:
    /// Read a direct accession response or one search page into catalog rows.
    std::expected<void, external_error> emit_catalog_rows(std::string_view body,
                                                          prov::provider_message_sink_ref sink,
                                                          prov::provider_statistics& stats,
                                                          std::stop_token stop) {
        namespace json = bioseq::external::json;
        auto document = json::parse_document(body);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::parse,
                                                       uniprot_provider_id(),
                                                       plan_.operation_id,
                                                       "UniProt JSON did not parse"));
        }

        std::uint64_t emitted = 0;
        if (plan_.operation_id == "getByAccession") {
            // getByAccession returns the entry object directly.
            const auto pushed = emit_entry(*document, sink);
            if (!pushed.has_value()) {
                return std::unexpected(pushed.error());
            }
            emitted = 1;
        } else {
            // searchCursor returns {"results":[ <entry>, ... ]}.
            const json::value* results = document->find_member("results");
            if (results == nullptr || !results->is_array()) {
                return std::unexpected(
                    make_external_error(external_error_kind::parse,
                                        uniprot_provider_id(),
                                        plan_.operation_id,
                                        "UniProt search payload had no results array"));
            }
            for (const json::value& entry : results->get_elements()) {
                if (stop.stop_requested() || stop_->load()) {
                    break;
                }
                const auto pushed = emit_entry(entry, sink);
                if (!pushed.has_value()) {
                    return std::unexpected(pushed.error());
                }
                ++emitted;
                if (plan_.limit.has_value() && emitted >= *plan_.limit) {
                    break;
                }
            }
        }
        stats.rows = emitted;
        return {};
    }

    /// Emit one UniProtKB catalog row, leaving missing metadata unset.
    std::expected<void, external_error> emit_entry(const bioseq::external::json::value& entry,
                                                   prov::provider_message_sink_ref sink) {
        namespace json = bioseq::external::json;
        prov::owning_catalog_row row;
        row.provider = uniprot_provider_id();
        row.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::make_truncated("uniprot");
        row.kind = prov::entity_kind::protein;
        row.molecule = prov::molecule_kind::protein;
        row.is_retrievable = true;

        row.key.provider = uniprot_provider_id();
        row.key.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::make_truncated("uniprot");
        row.key.domain = uniprot_bounded_id("uniprotkb");

        if (const auto accession = entry.read_string_member("primaryAccession")) {
            row.key.original_identifier = uniprot_bounded_id(*accession);
            row.key.accession = uniprot_bounded_id(*accession);
        }

        if (const json::value* organism = entry.find_member("organism")) {
            if (const auto scientific_name = organism->read_string_member("scientificName")) {
                row.scientific_name = uniprot_bounded_id(*scientific_name);
            }
            if (const auto taxon = organism->read_signed_integer_member("taxonId")) {
                row.taxon_id = *taxon;
            }
        }

        if (const json::value* sequence = entry.find_member("sequence")) {
            if (const auto length = sequence->read_signed_integer_member("length");
                length.has_value() && *length >= 0) {
                row.length = static_cast<std::uint64_t>(*length);
            }
        }

        // Use the recommended full protein name as the catalog title.
        if (const json::value* description = entry.find_member("proteinDescription")) {
            if (const json::value* recommended = description->find_member("recommendedName")) {
                if (const json::value* full_name = recommended->find_member("fullName")) {
                    if (const auto title = full_name->read_string_member("value")) {
                        row.title = uniprot_bounded_id(*title);
                    }
                }
            }
        }

        if (const auto entry_type = entry.read_string_member("entryType")) {
            row.is_reviewed = (*entry_type == "UniProtKB reviewed (Swiss-Prot)");
        }

        if (const json::value* genes = entry.find_member("genes")) {
            std::vector<prov::owning_gene_reference> gene_refs;
            for (const json::value& gene : genes->get_elements()) {
                if (const json::value* gene_name = gene.find_member("geneName")) {
                    if (const auto name = gene_name->read_string_member("value")) {
                        prov::owning_gene_reference gene_ref;
                        gene_ref.name_space = uniprot_bounded_id("UniProt");
                        gene_ref.identifier = uniprot_bounded_id(*name);
                        gene_ref.symbol = uniprot_bounded_id(*name);
                        gene_refs.push_back(std::move(gene_ref));
                    }
                }
            }
            if (!gene_refs.empty()) {
                row.genes = std::move(gene_refs);
            }
        }

        row.audit.completed_at = std::chrono::time_point_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now());

        return sink.push_fn(sink.object, prov::owning_provider_message{std::move(row)});
    }

    const context::context* memory_;
    const bioseq::parallelism::global_pool* pool_;
    uniprot_plan plan_;
    std::unique_ptr<std::atomic<bool>> stop_;
};

static_assert(prov::external_api_session_compatible<uniprot_session>);

/// Provider adapter for UniProtKB catalog metadata.
struct uniprot_provider {
    static constexpr auto id = bioseq::common::templating::compile_time_string("uniprot");
    static constexpr auto name = bioseq::common::templating::compile_time_string("UniProt");
    static constexpr bool supports_sequence_retrieval = true;

    /// Request plan prepared by this adapter.
    using plan_type = uniprot_plan;
    /// Session executing a prepared request plan.
    using session_type = uniprot_session;

    /// Return the UniProt features advertised by this adapter.
    static prov::provider_capabilities make_potential_capabilities() {
        prov::provider_capabilities caps;
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::catalog_search));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::protein_locator));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::direct_inspect));
        caps.maximum_metadata_page = 500;
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

    /// Estimate plan and buffer sizes for a UniProt request.
    static std::expected<prov::provider_plan_estimate, external_error>
    estimate(const prov::normalized_external_query& query,
             const prov::provider_capability_snapshot&,
             const prov::external_planning_limits&) {
        prov::provider_plan_estimate estimate;
        estimate.plan_bytes = 4096;
        estimate.metadata_page_bytes = 2U * 1024U * 1024U;
        estimate.staging_message_bytes = 64U * 1024U;
        estimate.transfer_count = query.target == prov::query_target::sequence ? 1U : 0U;
        return estimate;
    }

    /// Choose an accession lookup or text search for catalog metadata.
    /// Taxonomy and sequence targets return unsupported_capability.
    static std::expected<uniprot_plan, external_error>
    plan(const prov::normalized_external_query& query,
         const prov::provider_capability_snapshot&,
         const prov::external_slot_budget&) {
        uniprot_plan plan;
        plan.target = query.target;
        plan.limit = query.limit;
        if (query.target == prov::query_target::taxonomy) {
            return std::unexpected(make_external_error(external_error_kind::unsupported_capability,
                                                       uniprot_provider_id(),
                                                       "taxonomy",
                                                       "UniProt has no taxonomy search endpoint"));
        }
        if (query.target == prov::query_target::sequence ||
            query.target == prov::query_target::sequence_chunk) {
            // Sequence rows are not implemented by this adapter.
            return std::unexpected(
                make_external_error(external_error_kind::unsupported_capability,
                                    uniprot_provider_id(),
                                    "sequence",
                                    "UniProt protein sequence retrieval is not implemented"));
        }

        // An accession filter selects getByAccession. Otherwise searchCursor uses
        // the first text filter, adding organism_name: for an organism search.
        std::string accession;
        std::string search_query;
        for (const auto& node : query.predicates) {
            if (const auto* comparison = std::get_if<prov::comparison_predicate>(&node.leaf)) {
                if (comparison->operation == prov::predicate_operator::equal &&
                    comparison->field == prov::external_field::accession &&
                    !comparison->values.empty()) {
                    if (const auto* value =
                            std::get_if<prov::bounded_string<prov::maximum_identifier_bytes>>(
                                &comparison->values.front())) {
                        accession = std::string(value->view());
                    }
                }
            } else if (const auto* text = std::get_if<prov::text_search_predicate>(&node.leaf)) {
                if (search_query.empty() && !text->query.is_empty()) {
                    if (text->scope == prov::text_search_scope::organism) {
                        search_query = "organism_name:" + std::string(text->query.view());
                    } else {
                        search_query = std::string(text->query.view());
                    }
                }
            }
        }

        if (!accession.empty()) {
            plan.operation_id = "getByAccession";
            plan.accession = std::move(accession);
        } else {
            plan.operation_id = "searchCursor";
            if (!search_query.empty()) {
                plan.query = std::move(search_query);
            }
        }
        return plan;
    }

    /// Open a session that owns a copy of the plan.
    static std::expected<uniprot_session, external_error>
    open(context::context& worker_memory,
         const bioseq::parallelism::global_pool& pool,
         prov::external_runtime_services&,
         std::optional<prov::credential_view>,
         const uniprot_plan& plan,
         const prov::external_slot_budget&) {
        return uniprot_session(worker_memory, pool, plan);
    }
};

static_assert(prov::external_api_provider_compatible<uniprot_provider>);

} // namespace bioseq::external::providers
