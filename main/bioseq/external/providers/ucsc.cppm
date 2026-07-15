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

export module bioseq.external.providers.ucsc;

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
import bioseq.external.api.ucsc;
import bioseq.common.context;
import bioseq.parallelism.pool;
import bioseq.common.templating.template_str;

// DOC0372 — UCSC Genome Browser metadata adapter. Discovery uses the generated
// GET client (control plane); this adapter normalizes /list/ucscGenomes and
// /findGenome into catalog rows and lowers a bounded region into a
// getDnaSequence sequence-retrieval plan. No PostgreSQL, alphabet, or transfer
// dependency; parsing uses simdjson.
export namespace bioseq::external::providers {

namespace prov = bioseq::external::provider;
namespace api = bioseq::external::api::ucsc;

using prov::external_error;
using prov::external_error_kind;
using prov::make_external_error;
using prov::provider_id;

inline prov::provider_id ucsc_provider_id() noexcept { return {.value = "ucsc"}; }

inline prov::bounded_string<prov::maximum_identifier_bytes> ucsc_bounded_id(
    std::string_view value) {
    return prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(
        value);
}

// Compact, validated, non-secret plan.
struct ucsc_plan {
    prov::query_target target{prov::query_target::catalog};
    std::string operation_id{"listUcscGenomes"};
    std::string genome;
    std::string chrom;
    std::optional<std::uint64_t> start0;
    std::optional<std::uint64_t> end0;
    std::string free_text;
    std::optional<std::uint64_t> limit;

    [[nodiscard]] std::size_t deep_size_bytes() const noexcept {
        return sizeof(ucsc_plan) + operation_id.capacity() + genome.capacity() +
               chrom.capacity() + free_text.capacity();
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
        mix(genome);
        mix(chrom);
        mix(free_text);
        mix("ucsc");
        prov::external_execution_key key{};
        for (std::size_t index = 0; index < key.size(); ++index) {
            key[index] = static_cast<std::byte>(
                (hash >> ((index % 8U) * 8U)) & 0xFFU);
        }
        return key;
    }
};

class ucsc_session {
public:
    ucsc_session(const context::context& memory,
                 const bioseq::parallelism::GlobalPool& pool, ucsc_plan plan)
        : memory_(&memory), pool_(&pool), plan_(std::move(plan)),
          stop_(std::make_unique<std::atomic<bool>>(false)) {}

    ucsc_session(ucsc_session&&) noexcept = default;
    ucsc_session& operator=(ucsc_session&&) noexcept = default;

    std::expected<prov::provider_statistics, external_error> run(
        prov::provider_message_sink_ref sink, std::stop_token stop) {
        prov::provider_statistics stats;
        api::bioseq_external_api_client client(*memory_, *pool_, std::nullopt);

        std::vector<api::parameter_value> params;
        if (plan_.operation_id == "findGenome" && !plan_.free_text.empty()) {
            params.push_back({.name = "q",
                              .location = api::parameter_location::query,
                              .value = std::string_view{plan_.free_text}});
        } else if (plan_.operation_id == "getDnaSequence") {
            params.push_back({.name = "genome",
                              .location = api::parameter_location::query,
                              .value = std::string_view{plan_.genome}});
            params.push_back({.name = "chrom",
                              .location = api::parameter_location::query,
                              .value = std::string_view{plan_.chrom}});
            if (plan_.start0.has_value()) {
                params.push_back({.name = "start",
                                  .location = api::parameter_location::query,
                                  .value = static_cast<std::int64_t>(
                                      *plan_.start0)});
            }
            if (plan_.end0.has_value()) {
                params.push_back({.name = "end",
                                  .location = api::parameter_location::query,
                                  .value = static_cast<std::int64_t>(
                                      *plan_.end0)});
            }
        }

        auto response = client.get(plan_.operation_id, params);
        if (!response.has_value()) {
            return std::unexpected(make_external_error(
                external_error_kind::transport, ucsc_provider_id(),
                plan_.operation_id, response.error().message));
        }
        stats.requests = 1;
        stats.wire_bytes = response->body_view().size();
        stats.decoded_bytes = response->body_view().size();
        if (response->status_code != 200) {
            return std::unexpected(make_external_error(
                external_error_kind::http, ucsc_provider_id(),
                plan_.operation_id, "UCSC returned a non-200 status"));
        }

        auto parsed =
            plan_.operation_id == "getDnaSequence"
                ? emit_sequence(response->body_view(), sink, stats, stop)
                : emit_catalog_rows(response->body_view(), sink, stats, stop);
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
    // Parse a UCSC /getData/sequence response ({"dna","chrom","start","end",
    // "genome"}) into one owning_sequence_payload. UCSC returns 0-based
    // half-open coordinates, matching start0/end0 directly.
    std::expected<void, external_error> emit_sequence(
        std::string_view body, prov::provider_message_sink_ref sink,
        prov::provider_statistics& stats, std::stop_token stop) {
        namespace json = bioseq::external::json;
        (void) stop;
        std::string parse_error;
        auto document = json::parse(body, parse_error);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(
                external_error_kind::parse, ucsc_provider_id(),
                plan_.operation_id, "UCSC sequence JSON did not parse"));
        }
        const auto dna = document->string_at("dna");
        if (!dna.has_value()) {
            stats.rows = 0;
            return {};
        }

        prov::owning_sequence_payload payload;
        payload.provider = ucsc_provider_id();
        payload.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::
                make_truncated("UCSC");
        payload.molecule = prov::molecule_kind::nucleotide;

        std::string genome = plan_.genome;
        if (const auto g = document->string_at("genome");
            g.has_value() && !g->empty()) {
            genome.assign(*g);
        }
        std::string chrom = plan_.chrom;
        if (const auto c = document->string_at("chrom");
            c.has_value() && !c->empty()) {
            chrom.assign(*c);
        }
        std::string accession = genome;
        if (!chrom.empty()) {
            accession += ':';
            accession += chrom;
        }
        payload.key.provider = ucsc_provider_id();
        payload.key.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::
                make_truncated("UCSC");
        payload.key.domain = ucsc_bounded_id("sequence");
        payload.key.original_identifier = ucsc_bounded_id(accession);
        payload.key.accession = ucsc_bounded_id(accession);
        if (!genome.empty()) {
            payload.assembly_database = ucsc_bounded_id(genome);
        }
        if (!chrom.empty()) {
            payload.chromosome = ucsc_bounded_id(chrom);
        }
        if (const auto s = document->int_at("start")) {
            payload.start0 = static_cast<std::uint64_t>(*s);
        } else {
            payload.start0 = plan_.start0;
        }
        if (const auto e = document->int_at("end")) {
            payload.end0 = static_cast<std::uint64_t>(*e);
        } else {
            payload.end0 = plan_.end0;
        }
        payload.bases.assign(*dna);
        payload.end_of_record = true;
        payload.audit.completed_at =
            std::chrono::time_point_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now());

        stats.rows = 1;
        return sink.push_fn(
            sink.object, prov::owning_provider_message{std::move(payload)});
    }

    // Normalize the /list/ucscGenomes or /findGenome payload into catalog rows.
    std::expected<void, external_error> emit_catalog_rows(
        std::string_view body, prov::provider_message_sink_ref sink,
        prov::provider_statistics& stats, std::stop_token stop) {
        namespace json = bioseq::external::json;
        std::string parse_error;
        auto document = json::parse(body, parse_error);
        if (!document.has_value()) {
            return std::unexpected(make_external_error(
                external_error_kind::parse, ucsc_provider_id(),
                plan_.operation_id, "UCSC JSON did not parse"));
        }

        // Both endpoints nest assemblies under a top-level object keyed by the
        // UCSC database name. /list/ucscGenomes -> "ucscGenomes"; /findGenome ->
        // "data". Fall back to the document's own object members.
        const json::value* genomes = nullptr;
        for (std::string_view container : {"ucscGenomes", "data"}) {
            if (const json::value* node = document->find(container);
                node != nullptr && node->is_object()) {
                genomes = node;
                break;
            }
        }
        if (genomes == nullptr && document->is_object()) {
            genomes = &*document;
        }
        if (genomes == nullptr) {
            return std::unexpected(make_external_error(
                external_error_kind::parse, ucsc_provider_id(),
                plan_.operation_id, "UCSC payload had no genome object"));
        }

        std::uint64_t emitted = 0;
        for (const json::member& entry : genomes->members()) {
            if (stop.stop_requested() || stop_->load()) {
                break;
            }
            const json::value& detail = entry.val;
            if (!detail.is_object()) {
                continue;
            }
            prov::owning_catalog_row row;
            row.provider = ucsc_provider_id();
            row.provider_name =
                prov::bounded_string<prov::maximum_provider_name_bytes>::
                    make_truncated("ucsc");
            row.kind = prov::entity_kind::assembly;
            row.molecule = prov::molecule_kind::nucleotide;
            row.retrievable = true;

            row.key.provider = ucsc_provider_id();
            row.key.provider_name =
                prov::bounded_string<prov::maximum_provider_name_bytes>::
                    make_truncated("ucsc");
            row.key.domain = ucsc_bounded_id("ucscGenomes");
            row.key.original_identifier = ucsc_bounded_id(entry.key);
            row.key.accession = ucsc_bounded_id(entry.key);

            row.assembly_database = ucsc_bounded_id(entry.key);

            if (const auto text = detail.string_at("scientificName")) {
                row.scientific_name = ucsc_bounded_id(*text);
            }
            if (const auto text = detail.string_at("description")) {
                row.title = ucsc_bounded_id(*text);
            } else if (const auto organism = detail.string_at("organism")) {
                row.title = ucsc_bounded_id(*organism);
            }
            if (const auto taxon = detail.int_at("taxId")) {
                row.taxon_id = *taxon;
            }

            row.audit.completed_at = std::chrono::time_point_cast<
                std::chrono::microseconds>(std::chrono::system_clock::now());

            const auto pushed = sink.push_fn(
                sink.object, prov::owning_provider_message{std::move(row)});
            if (!pushed.has_value()) {
                return std::unexpected(pushed.error());
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
    const bioseq::parallelism::GlobalPool* pool_;
    ucsc_plan plan_;
    std::unique_ptr<std::atomic<bool>> stop_;
};

static_assert(prov::external_api_session_implementation<ucsc_session>);

struct ucsc_provider {
    static constexpr auto id = bioseq::common::templating::CompTimeStr("ucsc");
    static constexpr auto name = bioseq::common::templating::CompTimeStr("UCSC");
    static constexpr bool supports_sequence_retrieval = true;

    using plan_type = ucsc_plan;
    using session_type = ucsc_session;

    static prov::provider_capabilities potential_capabilities() {
        prov::provider_capabilities caps;
        caps.flags.set(static_cast<std::size_t>(
            prov::provider_capability::catalog_search));
        caps.flags.set(static_cast<std::size_t>(
            prov::provider_capability::nucleotide_locator));
        caps.flags.set(static_cast<std::size_t>(
            prov::provider_capability::direct_inspect));
        caps.maximum_metadata_page = 1000;
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

    static std::expected<ucsc_plan, external_error> plan(
        const prov::normalized_external_query& query,
        const prov::provider_capability_snapshot&,
        const prov::external_slot_budget&) {
        ucsc_plan plan;
        plan.target = query.target;
        plan.limit = query.limit;
        if (query.target == prov::query_target::sequence ||
            query.target == prov::query_target::sequence_chunk) {
            plan.operation_id = "getDnaSequence";
            // Fill the region from the pushed equality predicates: assembly
            // database -> UCSC genome, chromosome, and the 0-based half-open
            // [start0, end0) window that UCSC /getData/sequence consumes as-is.
            for (const prov::predicate_node& node : query.predicates) {
                const auto* cmp =
                    std::get_if<prov::comparison_predicate>(&node.leaf);
                if (cmp == nullptr ||
                    cmp->operation != prov::predicate_operator::equal ||
                    cmp->values.empty()) {
                    continue;
                }
                const prov::external_scalar& first = cmp->values.front();
                switch (cmp->field) {
                    case prov::external_field::assembly_database:
                    case prov::external_field::assembly_accession:
                        if (const auto* s = std::get_if<prov::bounded_string<
                                prov::maximum_identifier_bytes>>(&first)) {
                            plan.genome = std::string(s->view());
                        }
                        break;
                    case prov::external_field::chromosome:
                    case prov::external_field::sequence_alias:
                        if (const auto* s = std::get_if<prov::bounded_string<
                                prov::maximum_identifier_bytes>>(&first)) {
                            plan.chrom = std::string(s->view());
                        }
                        break;
                    case prov::external_field::start0:
                        if (const auto* v = std::get_if<std::int64_t>(&first)) {
                            plan.start0 = static_cast<std::uint64_t>(*v);
                        }
                        break;
                    case prov::external_field::end0:
                        if (const auto* v = std::get_if<std::int64_t>(&first)) {
                            plan.end0 = static_cast<std::uint64_t>(*v);
                        }
                        break;
                    default:
                        break;
                }
            }
        } else if (query.target == prov::query_target::taxonomy) {
            return std::unexpected(make_external_error(
                external_error_kind::unsupported_capability, ucsc_provider_id(),
                "taxonomy", "UCSC has no taxonomy search endpoint"));
        } else {
            plan.operation_id = "listUcscGenomes";
        }
        return plan;
    }

    static std::expected<ucsc_session, external_error> open(
        context::context& worker_memory,
        const bioseq::parallelism::GlobalPool& pool,
        prov::external_runtime_services&, std::optional<prov::credential_view>,
        const ucsc_plan& plan, const prov::external_slot_budget&) {
        return ucsc_session(worker_memory, pool, plan);
    }
};

static_assert(prov::external_api_provider_implementation<ucsc_provider>);

}  // namespace bioseq::external::providers
