module;

#include "json_dom.hpp"

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
import bioseq.external.api.uniprot;
import bioseq.common.context;
import bioseq.parallelism.pool;
import bioseq.common.templating.template_str;

// DOC0372 — UniProt (UniProtKB) protein metadata adapter. Discovery uses the
// generated GET client (control plane); this adapter normalizes a getByAccession
// entry object or a searchCursor result page into protein catalog rows. UniProt
// is a PROTEIN provider: sequence retrieval is advertised (protein FASTA bytes)
// but protein sequence materialization is rejected later in the PostgreSQL
// layer, not here. No PostgreSQL, alphabet, or transfer dependency; JSON is
// requested with an explicit application/json Accept and parsed through the
// owned json_dom facility (simdjson stays behind a classic translation unit).
export namespace bioseq::external::providers {

namespace prov = bioseq::external::provider;
namespace api = bioseq::external::api::uniprot;

using prov::external_error;
using prov::external_error_kind;
using prov::make_external_error;
using prov::provider_id;

inline prov::provider_id uniprot_provider_id() noexcept {
    return {.value = "uniprot"};
}

inline prov::bounded_string<prov::maximum_identifier_bytes> uniprot_bounded_id(
    std::string_view value) {
    return prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(
        value);
}

// Compact, validated, non-secret plan.
struct uniprot_plan {
    prov::query_target target{prov::query_target::catalog};
    std::string operation_id{"searchCursor"};
    std::string accession;   // Set when an accession-equality predicate is present.
    std::string query{"*"};  // UniProtKB free-text/organism search expression.
    std::optional<std::uint64_t> limit;

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        return sizeof(uniprot_plan) + operation_id.capacity() +
               accession.capacity() + query.capacity();
    }

    [[nodiscard]] prov::external_execution_key execution_fingerprint()
        const noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
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
            key[index] = static_cast<std::byte>(
                (hash >> ((index % 8U) * 8U)) & 0xFFU);
        }
        return key;
    }
};

class uniprot_session {
public:
    uniprot_session(const context::context& memory,
                    const bioseq::parallelism::GlobalPool& pool,
                    uniprot_plan plan)
        : memory_(&memory), pool_(&pool), plan_(std::move(plan)),
          stop_(std::make_unique<std::atomic<bool>>(false)) {}

    uniprot_session(uniprot_session&&) noexcept = default;
    uniprot_session& operator=(uniprot_session&&) noexcept = default;

    std::expected<prov::provider_statistics, external_error> run(
        prov::provider_message_sink_ref sink, std::stop_token stop) {
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
                // UniProtKB search caps the page size at 500.
                const std::uint64_t capped =
                    *plan_.limit > 500U ? 500U : *plan_.limit;
                params.push_back({.name = "size",
                                  .location = api::parameter_location::query,
                                  .value = static_cast<std::int64_t>(capped)});
            }
        }

        // The generated operations already default to application/json; request
        // it explicitly so a JSON body is returned regardless of the default.
        auto response = client.get_with_accept(plan_.operation_id, params,
                                               "application/json");
        if (!response.has_value()) {
            return std::unexpected(make_external_error(
                external_error_kind::transport, uniprot_provider_id(),
                plan_.operation_id, response.error().message));
        }
        stats.requests = 1;
        stats.wire_bytes = response->body_view().size();
        stats.decoded_bytes = response->body_view().size();
        if (response->status_code != 200) {
            return std::unexpected(make_external_error(
                external_error_kind::http, uniprot_provider_id(),
                plan_.operation_id, "UniProt returned a non-200 status"));
        }

        auto parsed = emit_catalog_rows(response->body_view(), sink, stats, stop);
        if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
        }
        stats.complete = true;
        return stats;
    }

    void request_stop() noexcept {
        if (stop_) {
            stop_->store(true);
        }
    }

private:
    // Normalize either a single getByAccession entry or a searchCursor result
    // page into protein catalog rows.
    std::expected<void, external_error> emit_catalog_rows(
        std::string_view body, prov::provider_message_sink_ref sink,
        prov::provider_statistics& stats, std::stop_token stop) {
        namespace json = bioseq::external::json;
        std::string parse_error;
        auto document = json::parse(body, parse_error);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(
                external_error_kind::parse, uniprot_provider_id(),
                plan_.operation_id, "UniProt JSON did not parse"));
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
            const json::value* results = document->find("results");
            if (results == nullptr || !results->is_array()) {
                return std::unexpected(make_external_error(
                    external_error_kind::parse, uniprot_provider_id(),
                    plan_.operation_id,
                    "UniProt search payload had no results array"));
            }
            for (const json::value& entry : results->items()) {
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

    // Map one UniProtKBEntry object into an owning_catalog_row and push it.
    // Every JSON field is optional; missing members leave the row default.
    std::expected<void, external_error> emit_entry(
        const bioseq::external::json::value& entry,
        prov::provider_message_sink_ref sink) {
        namespace json = bioseq::external::json;
        prov::owning_catalog_row row;
        row.provider = uniprot_provider_id();
        row.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::
                make_truncated("uniprot");
        row.kind = prov::entity_kind::protein;
        row.molecule = prov::molecule_kind::protein;
        row.retrievable = true;

        row.key.provider = uniprot_provider_id();
        row.key.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::
                make_truncated("uniprot");
        row.key.domain = uniprot_bounded_id("uniprotkb");

        if (const auto accession = entry.string_at("primaryAccession")) {
            row.key.original_identifier = uniprot_bounded_id(*accession);
            row.key.accession = uniprot_bounded_id(*accession);
        }

        if (const json::value* organism = entry.find("organism")) {
            if (const auto scientific_name =
                    organism->string_at("scientificName")) {
                row.scientific_name = uniprot_bounded_id(*scientific_name);
            }
            if (const auto taxon = organism->int_at("taxonId")) {
                row.taxon_id = *taxon;
            }
        }

        if (const json::value* sequence = entry.find("sequence")) {
            if (const auto length = sequence->int_at("length");
                length.has_value() && *length >= 0) {
                row.length = static_cast<std::uint64_t>(*length);
            }
        }

        // proteinDescription.recommendedName.fullName.value
        if (const json::value* description = entry.find("proteinDescription")) {
            if (const json::value* recommended =
                    description->find("recommendedName")) {
                if (const json::value* full_name =
                        recommended->find("fullName")) {
                    if (const auto title = full_name->string_at("value")) {
                        row.title = uniprot_bounded_id(*title);
                    }
                }
            }
        }

        if (const auto entry_type = entry.string_at("entryType")) {
            row.is_reviewed = (*entry_type == "UniProtKB reviewed (Swiss-Prot)");
        }

        if (const json::value* genes = entry.find("genes")) {
            std::vector<prov::owning_gene_reference> gene_refs;
            for (const json::value& gene : genes->items()) {
                if (const json::value* gene_name = gene.find("geneName")) {
                    if (const auto name = gene_name->string_at("value")) {
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

        row.audit.completed_at = std::chrono::time_point_cast<
            std::chrono::microseconds>(std::chrono::system_clock::now());

        return sink.push_fn(
            sink.object, prov::owning_provider_message{std::move(row)});
    }

    const context::context* memory_;
    const bioseq::parallelism::GlobalPool* pool_;
    uniprot_plan plan_;
    std::unique_ptr<std::atomic<bool>> stop_;
};

static_assert(prov::external_api_session_implementation<uniprot_session>);

struct uniprot_provider {
    static constexpr auto id =
        bioseq::common::templating::CompTimeStr("uniprot");
    static constexpr auto name =
        bioseq::common::templating::CompTimeStr("UniProt");
    static constexpr bool supports_sequence_retrieval = true;

    using plan_type = uniprot_plan;
    using session_type = uniprot_session;

    static prov::provider_capabilities potential_capabilities() {
        prov::provider_capabilities caps;
        caps.flags.set(static_cast<std::size_t>(
            prov::provider_capability::catalog_search));
        caps.flags.set(static_cast<std::size_t>(
            prov::provider_capability::protein_locator));
        caps.flags.set(static_cast<std::size_t>(
            prov::provider_capability::direct_inspect));
        caps.maximum_metadata_page = 500;
        caps.maximum_identifier_batch = 1;
        return caps;
    }

    static std::span<const prov::request_policy_profile_view>
    request_policy_profiles() {
        return {};
    }

    static std::span<const prov::locator_policy_profile_view>
    locator_policy_profiles() {
        return {};
    }

    static std::expected<prov::provider_plan_estimate, external_error> estimate(
        const prov::normalized_external_query& query,
        const prov::provider_capability_snapshot&,
        const prov::external_planning_limits&) {
        prov::provider_plan_estimate estimate;
        estimate.plan_bytes = 4096;
        estimate.metadata_page_bytes = 2U * 1024U * 1024U;
        estimate.staging_message_bytes = 64U * 1024U;
        estimate.transfer_count =
            query.target == prov::query_target::sequence ? 1U : 0U;
        return estimate;
    }

    static std::expected<uniprot_plan, external_error> plan(
        const prov::normalized_external_query& query,
        const prov::provider_capability_snapshot&,
        const prov::external_slot_budget&) {
        uniprot_plan plan;
        plan.target = query.target;
        plan.limit = query.limit;
        if (query.target == prov::query_target::taxonomy) {
            return std::unexpected(make_external_error(
                external_error_kind::unsupported_capability,
                uniprot_provider_id(), "taxonomy",
                "UniProt has no taxonomy search endpoint"));
        }
        if (query.target == prov::query_target::sequence ||
            query.target == prov::query_target::sequence_chunk) {
            // Protein sequence materialization is deliberately not implemented
            // (DOC0372); surfaced by the PG bridge as SQLSTATE 0A000.
            return std::unexpected(make_external_error(
                external_error_kind::unsupported_capability,
                uniprot_provider_id(), "sequence",
                "UniProt protein sequence retrieval is not implemented"));
        }

        // Catalog lowering: an accession-equality predicate lowers to a single
        // getByAccession fetch; otherwise a free-text/organism searchCursor.
        std::string accession;
        std::string search_query;
        for (const auto& node : query.predicates) {
            if (const auto* comparison =
                    std::get_if<prov::comparison_predicate>(&node.leaf)) {
                if (comparison->operation == prov::predicate_operator::equal &&
                    comparison->field == prov::external_field::accession &&
                    !comparison->values.empty()) {
                    if (const auto* value = std::get_if<
                            prov::bounded_string<prov::maximum_identifier_bytes>>(
                            &comparison->values.front())) {
                        accession = std::string(value->view());
                    }
                }
            } else if (const auto* text = std::get_if<
                           prov::text_search_predicate>(&node.leaf)) {
                if (search_query.empty() && !text->query.empty()) {
                    if (text->scope == prov::text_search_scope::organism) {
                        search_query =
                            "organism_name:" + std::string(text->query.view());
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

    static std::expected<uniprot_session, external_error> open(
        context::context& worker_memory,
        const bioseq::parallelism::GlobalPool& pool,
        prov::external_runtime_services&, std::optional<prov::credential_view>,
        const uniprot_plan& plan, const prov::external_slot_budget&) {
        return uniprot_session(worker_memory, pool, plan);
    }
};

static_assert(prov::external_api_provider_implementation<uniprot_provider>);

}  // namespace bioseq::external::providers
