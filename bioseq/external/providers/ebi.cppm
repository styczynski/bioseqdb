/**
 * @file external/providers/ebi.cppm
 * @brief Search EBI metadata and turn the response into provider rows.
 */
module;

#include <array>
#include <atomic>
#include <charconv>
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
#include <system_error>
#include <variant>
#include <vector>

export module bioseq.external.providers.ebi;

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
import bioseq.external.api.ebi;
import bioseq.common.context;
import bioseq.parallelism.pool;
import bioseq.common.templating.compile_time_string;

/// EBI Search metadata adapter.
export namespace bioseq::external::providers {

namespace prov = bioseq::external::provider;
namespace api = bioseq::external::api::ebi;

using prov::external_error;
using prov::external_error_kind;
using prov::make_external_error;
using prov::provider_id;

/// Return the provider ID used for EBI rows and errors.
inline prov::provider_id ebi_provider_id() noexcept {
    return {.value = "ebi"};
}

/// Copy an identifier, truncating it at the shared byte limit.
inline prov::bounded_string<prov::maximum_identifier_bytes> ebi_bounded_id(std::string_view value) {
    return prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(value);
}

/// Return the bounded EBI name stored in result rows.
inline prov::bounded_string<prov::maximum_provider_name_bytes> ebi_bounded_name() {
    return prov::bounded_string<prov::maximum_provider_name_bytes>::make_truncated("ebi");
}

/// EBI search operation, domain, and query text.
struct ebi_plan {
    prov::query_target target{prov::query_target::catalog};
    std::string operation_id{"search"};
    std::string domain{"uniprot"};
    std::string query_text;
    std::optional<std::uint64_t> limit;

    /// Estimate storage for the plan and its strings.
    [[nodiscard]] std::size_t measure_deep_size_bytes() const noexcept {
        return sizeof(ebi_plan) + operation_id.capacity() + domain.capacity() +
               query_text.capacity();
    }

    /// Hash the operation, domain, and search text for plan identification.
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
        mix(domain);
        mix(query_text);
        mix("ebi");
        prov::external_execution_key key{};
        for (std::size_t index = 0; index < key.size(); ++index) {
            key[index] = static_cast<std::byte>((hash >> ((index % 8U) * 8U)) & 0xFFU);
        }
        return key;
    }
};

/// Fetch one EBI Search response and send its rows to a consumer.
class ebi_session {
  public:
    /// Own the plan while borrowing the memory context and worker pool.
    ebi_session(const context::context& memory,
                const bioseq::parallelism::global_pool& pool,
                ebi_plan plan)
        : memory_(&memory), pool_(&pool), plan_(std::move(plan)),
          stop_(std::make_unique<std::atomic<bool>>(false)) {}

    ebi_session(ebi_session&&) noexcept = default;
    ebi_session& operator=(ebi_session&&) noexcept = default;

    /// Fetch one search page and emit taxonomy or catalog rows.
    std::expected<prov::provider_statistics, external_error>
    run(prov::provider_message_sink_ref sink, std::stop_token stop) {
        prov::provider_statistics stats;
        api::bioseq_external_api_client client(*memory_, *pool_, std::nullopt);

        // The generated search API expects size as text. Keep that text alive
        // until get() has used the parameter view.
        std::string size_text;
        std::vector<api::parameter_value> params;
        params.push_back({.name = "domain",
                          .location = api::parameter_location::path,
                          .value = std::string_view{plan_.domain}});
        params.push_back({.name = "query",
                          .location = api::parameter_location::query,
                          .value = std::string_view{plan_.query_text}});
        params.push_back({.name = "format",
                          .location = api::parameter_location::query,
                          .value = std::string_view{"json"}});
        if (plan_.limit.has_value()) {
            size_text = std::to_string(*plan_.limit);
            params.push_back({.name = "size",
                              .location = api::parameter_location::query,
                              .value = std::string_view{size_text}});
        }

        auto response = client.get(plan_.operation_id, params);
        if (!response.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::transport,
                                                       ebi_provider_id(),
                                                       plan_.operation_id,
                                                       response.error().message));
        }
        stats.requests = 1;
        stats.wire_bytes = response->body_view().size();
        stats.decoded_bytes = response->body_view().size();
        if (response->status_code != 200) {
            return std::unexpected(make_external_error(external_error_kind::http,
                                                       ebi_provider_id(),
                                                       plan_.operation_id,
                                                       "EBI returned a non-200 status"));
        }

        auto parsed = emit_rows(response->body_view(), sink, stats, stop);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        stats.is_complete = true;
        return stats;
    }

    /// Ask the row-emission loop to stop.
    void request_stop() noexcept {
        if (stop_) {
            stop_->store(true);
        }
    }

  private:
    /// Read EBI entries, skipping missing IDs and leaving absent fields unset.
    std::expected<void, external_error> emit_rows(std::string_view body,
                                                  prov::provider_message_sink_ref sink,
                                                  prov::provider_statistics& stats,
                                                  std::stop_token stop) {
        namespace json = bioseq::external::json;
        auto document = json::parse_document(body);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(external_error_kind::parse,
                                                       ebi_provider_id(),
                                                       plan_.operation_id,
                                                       "EBI JSON did not parse"));
        }

        const json::value* entries = document->find_member("entries");
        if (entries == nullptr || !entries->is_array()) {
            stats.rows = 0;
            return {};
        }

        const bool taxonomy_domain = plan_.domain == "taxonomy";

        // Read the numeric TaxID prefix when the field starts with a number.
        const auto parse_taxon = [](std::string_view text) -> std::optional<std::int64_t> {
            std::int64_t numeric_identifier = 0;
            const char* const first = text.data();
            const char* const last = text.data() + text.size();
            const auto result = std::from_chars(first, last, numeric_identifier);
            if (result.ec != std::errc{}) {
                return std::nullopt;
            }
            return numeric_identifier;
        };

        std::uint64_t emitted = 0;
        for (const json::value& entry : entries->get_elements()) {
            if (stop.stop_requested() || stop_->load()) {
                break;
            }
            const auto id = entry.read_string_member("id");
            if (!id.has_value()) {
                continue;
            }
            const json::value* fields = entry.find_member("fields");

            const auto now = std::chrono::time_point_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now());

            if (taxonomy_domain) {
                prov::owning_taxonomy_row row;
                row.provider = ebi_provider_id();
                row.provider_name = ebi_bounded_name();
                row.provider_taxon_id = ebi_bounded_id(*id);
                if (const auto taxon = parse_taxon(*id)) {
                    row.taxon_id = *taxon;
                } else if (fields != nullptr) {
                    if (const auto tid = fields->read_first_string_member("tax_id");
                        tid.has_value()) {
                        row.taxon_id = parse_taxon(*tid);
                    }
                }
                if (fields != nullptr) {
                    if (const auto name = fields->read_first_string_member("name")) {
                        row.scientific_name = ebi_bounded_id(*name);
                    } else if (const auto organism = fields->read_first_string_member("organism")) {
                        row.scientific_name = ebi_bounded_id(*organism);
                    }
                }
                row.audit.completed_at = now;

                const auto pushed =
                    sink.push_fn(sink.object, prov::owning_provider_message{std::move(row)});
                if (!pushed.has_value()) {
                    return std::unexpected(pushed.error());
                }
            } else {
                prov::owning_catalog_row row;
                row.provider = ebi_provider_id();
                row.provider_name = ebi_bounded_name();
                // Label catalog hits as proteins, matching the default UniProt domain.
                row.kind = prov::entity_kind::protein;
                row.is_retrievable = false;

                row.key.provider = ebi_provider_id();
                row.key.provider_name = ebi_bounded_name();
                row.key.domain = ebi_bounded_id(plan_.domain);
                row.key.original_identifier = ebi_bounded_id(*id);
                // Use acc, such as P04637, as the accession. The id field can contain
                // an entry name such as P53_HUMAN and is kept as the original identifier.
                if (const auto acc = entry.read_string_member("acc");
                    acc.has_value() && !acc->empty()) {
                    row.key.accession = ebi_bounded_id(*acc);
                } else {
                    row.key.accession = ebi_bounded_id(*id);
                }

                if (fields != nullptr) {
                    if (const auto name = fields->read_first_string_member("name")) {
                        row.title = ebi_bounded_id(*name);
                    } else if (const auto description =
                                   fields->read_first_string_member("description")) {
                        row.title = ebi_bounded_id(*description);
                    }
                    if (const auto organism = fields->read_first_string_member("organism")) {
                        row.scientific_name = ebi_bounded_id(*organism);
                    }
                    if (const auto tid = fields->read_first_string_member("tax_id");
                        tid.has_value()) {
                        row.taxon_id = parse_taxon(*tid);
                    }
                }
                row.audit.completed_at = now;

                const auto pushed =
                    sink.push_fn(sink.object, prov::owning_provider_message{std::move(row)});
                if (!pushed.has_value()) {
                    return std::unexpected(pushed.error());
                }
            }

            ++emitted;
            if (plan_.limit.has_value() && emitted >= *plan_.limit) {
                break;
            }
        }
        stats.rows = emitted;
        return {};
    }

    const context::context* memory_;
    const bioseq::parallelism::global_pool* pool_;
    ebi_plan plan_;
    std::unique_ptr<std::atomic<bool>> stop_;
};

static_assert(prov::external_api_session_compatible<ebi_session>);

/// Provider adapter for EBI Search metadata.
struct ebi_provider {
    static constexpr auto id = bioseq::common::templating::compile_time_string("ebi");
    static constexpr auto name = bioseq::common::templating::compile_time_string("EBI");
    static constexpr bool supports_sequence_retrieval = false;

    /// Request plan prepared by this adapter.
    using plan_type = ebi_plan;
    /// Session executing a prepared request plan.
    using session_type = ebi_session;

    /// Return the EBI features advertised by this adapter.
    static prov::provider_capabilities make_potential_capabilities() {
        prov::provider_capabilities caps;
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::catalog_search));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::taxonomy_search));
        caps.flags.set(static_cast<std::size_t>(prov::provider_capability::direct_inspect));
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

    /// Return fixed estimates for the plan and metadata buffers.
    static std::expected<prov::provider_plan_estimate, external_error>
    estimate(const prov::normalized_external_query&,
             const prov::provider_capability_snapshot&,
             const prov::external_planning_limits&) {
        prov::provider_plan_estimate estimate;
        estimate.plan_bytes = 4096;
        estimate.metadata_page_bytes = 2U * 1024U * 1024U;
        estimate.staging_message_bytes = 64U * 1024U;
        estimate.transfer_count = 0U; // Metadata searches reserve no sequence transfers.
        return estimate;
    }

    /// Choose an EBI search domain and text from the query filters.
    /// Sequence targets return unsupported_capability.
    static std::expected<ebi_plan, external_error>
    plan(const prov::normalized_external_query& query,
         const prov::provider_capability_snapshot&,
         const prov::external_slot_budget&) {
        if (query.target == prov::query_target::sequence ||
            query.target == prov::query_target::sequence_chunk) {
            return std::unexpected(
                make_external_error(external_error_kind::unsupported_capability,
                                    ebi_provider_id(),
                                    "search",
                                    "EBI Search is discovery only; no sequence retrieval"));
        }
        ebi_plan plan;
        plan.target = query.target;
        plan.limit = query.limit;
        plan.operation_id = "search";
        const bool taxonomy_target = query.target == prov::query_target::taxonomy;
        plan.domain = taxonomy_target ? "taxonomy" : "uniprot";

        // Use taxonomy for taxon rows and UniProt by default for catalog rows.
        // A catalog domain filter can select a different EBI database.
        for (const prov::predicate_node& node : query.predicates) {
            if (const auto* text = std::get_if<prov::text_search_predicate>(&node.leaf)) {
                plan.query_text = std::string(text->query.view());
            } else if (const auto* comparison =
                           std::get_if<prov::comparison_predicate>(&node.leaf)) {
                if (comparison->operation != prov::predicate_operator::equal ||
                    comparison->values.empty()) {
                    continue;
                }
                const prov::external_scalar& first = comparison->values.front();
                const auto* text_value =
                    std::get_if<prov::bounded_string<prov::maximum_identifier_bytes>>(&first);
                switch (comparison->field) {
                case prov::external_field::domain:
                    if (!taxonomy_target && text_value != nullptr) {
                        plan.domain = std::string(text_value->view());
                    }
                    break;
                case prov::external_field::accession:
                case prov::external_field::scientific_name:
                case prov::external_field::common_name:
                    // Use a name or accession as search text when no text filter supplied it.
                    if (text_value != nullptr && plan.query_text.empty()) {
                        plan.query_text = std::string(text_value->view());
                    }
                    break;
                case prov::external_field::taxon_id:
                    if (const auto* taxon = std::get_if<std::int64_t>(&first);
                        taxon != nullptr && plan.query_text.empty()) {
                        plan.query_text = std::to_string(*taxon);
                    }
                    break;
                default:
                    break;
                }
            }
        }
        return plan;
    }

    /// Open a session that owns a copy of the plan.
    static std::expected<ebi_session, external_error>
    open(context::context& worker_memory,
         const bioseq::parallelism::global_pool& pool,
         prov::external_runtime_services&,
         std::optional<prov::credential_view>,
         const ebi_plan& plan,
         const prov::external_slot_budget&) {
        return ebi_session(worker_memory, pool, plan);
    }
};

static_assert(prov::external_api_provider_compatible<ebi_provider>);

} // namespace bioseq::external::providers
