module;

#include <concepts>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>

export module bioseq.external.provider.concepts;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.capabilities;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.messages;
import bioseq.external.provider.locator;
import bioseq.external.provider.budget;
import bioseq.common.context;
import bioseq.parallelism.pool;

// DOC0372 — provider implementation concepts. The metadata provider concept has
// no retrieval-associated type, so discovery providers compile and run without
// transfer/archive/parser/alphabet targets and a provider/retrieval ownership
// cycle is impossible. No virtual methods, no inheritance.
export namespace bioseq::external::provider {

template <class Session>
concept external_api_session_implementation =
    requires(Session& session,
             provider_message_sink_ref messages,
             std::stop_token stop) {
        { session.run(messages, stop) }
            -> std::same_as<std::expected<provider_statistics, external_error>>;
        { session.request_stop() } noexcept -> std::same_as<void>;
    };

template <class P>
concept external_api_provider_implementation =
    requires(const normalized_external_query& query,
             const provider_capability_snapshot& capability_snapshot,
             const external_planning_limits& planning_limits,
             context::context& worker_memory,
             const bioseq::parallelism::GlobalPool& pool,
             external_runtime_services& runtime,
             std::optional<credential_view> credentials,
             const typename P::plan_type& plan,
             const external_slot_budget& budget) {
        typename P::plan_type;
        typename P::session_type;
        requires std::move_constructible<typename P::plan_type>;
        requires std::move_constructible<typename P::session_type>;
        requires external_api_session_implementation<typename P::session_type>;
        { P::id.view() } -> std::convertible_to<std::string_view>;
        { P::name.view() } -> std::convertible_to<std::string_view>;
        { P::supports_sequence_retrieval } -> std::convertible_to<bool>;
        { P::potential_capabilities() } -> std::same_as<provider_capabilities>;
        { P::request_policy_profiles() }
            -> std::same_as<std::span<const request_policy_profile_view>>;
        { P::locator_policy_profiles() }
            -> std::same_as<std::span<const locator_policy_profile_view>>;
        { P::estimate(query, capability_snapshot, planning_limits) }
            -> std::same_as<
                std::expected<provider_plan_estimate, external_error>>;
        { P::plan(query, capability_snapshot, budget) }
            -> std::same_as<
                std::expected<typename P::plan_type, external_error>>;
        { plan.deep_size_bytes() } -> std::same_as<std::size_t>;
        { plan.execution_fingerprint() }
            -> std::same_as<external_execution_key>;
        { P::open(worker_memory, pool, runtime, credentials, plan, budget) }
            -> std::same_as<
                std::expected<typename P::session_type, external_error>>;
    };

}  // namespace bioseq::external::provider
