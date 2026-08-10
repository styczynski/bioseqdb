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

/// @brief Compile-time provider and session contracts.
// The metadata provider concept has
// no retrieval-associated type, so discovery providers compile and run without
// transfer/archive/parser/alphabet targets and a provider/retrieval ownership
// cycle is impossible. No virtual methods, no inheritance.
export namespace bioseq::external::provider {

template <class p_session_type>
concept external_api_session_compatible =
    requires(p_session_type &session, provider_message_sink_ref messages,
             std::stop_token stop) {
      {
        session.run(messages, stop)
      } -> std::same_as<std::expected<provider_statistics, external_error>>;
      { session.request_stop() } noexcept -> std::same_as<void>;
    };

template <class p_provider_type>
concept external_api_provider_compatible = requires(
    const normalized_external_query &query,
    const provider_capability_snapshot &capability_snapshot,
    const external_planning_limits &planning_limits,
    context::context &worker_memory,
    const bioseq::parallelism::global_pool &pool,
    external_runtime_services &runtime,
    std::optional<credential_view> credentials,
    const typename p_provider_type::plan_type &plan,
    const external_slot_budget &budget) {
  typename p_provider_type::plan_type;
  typename p_provider_type::session_type;
  requires std::move_constructible<typename p_provider_type::plan_type>;
  requires std::move_constructible<typename p_provider_type::session_type>;
  requires external_api_session_compatible<
      typename p_provider_type::session_type>;
  { p_provider_type::id.view() } -> std::convertible_to<std::string_view>;
  { p_provider_type::name.view() } -> std::convertible_to<std::string_view>;
  { p_provider_type::supports_sequence_retrieval } -> std::convertible_to<bool>;
  {
    p_provider_type::make_potential_capabilities()
  } -> std::same_as<provider_capabilities>;
  {
    p_provider_type::list_request_policy_profiles()
  } -> std::same_as<std::span<const request_policy_profile_view>>;
  {
    p_provider_type::list_locator_policy_profiles()
  } -> std::same_as<std::span<const locator_policy_profile_view>>;
  {
    p_provider_type::estimate(query, capability_snapshot, planning_limits)
  } -> std::same_as<std::expected<provider_plan_estimate, external_error>>;
  {
    p_provider_type::plan(query, capability_snapshot, budget)
  } -> std::same_as<
      std::expected<typename p_provider_type::plan_type, external_error>>;
  { plan.measure_deep_size_bytes() } -> std::same_as<std::size_t>;
  { plan.execution_fingerprint() } -> std::same_as<external_execution_key>;
  {
    p_provider_type::open(worker_memory, pool, runtime, credentials, plan,
                          budget)
  } -> std::same_as<
      std::expected<typename p_provider_type::session_type, external_error>>;
};

} // namespace bioseq::external::provider
