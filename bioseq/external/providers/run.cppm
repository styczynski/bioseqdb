/**
 * @file external/providers/run.cppm
 * @brief Synchronous bridge from callers to the configured provider pack.
 */
// Given a normalized query and target provider, the bridge uses
// the compile-time provider pack. It
// provider id, it dispatches once through configured_external_api_provider,
// opens a session, runs it to completion collecting every pushed
// owning_provider_message into a vector, and returns that owned buffer. It is
// deliberately PostgreSQL-free (NO_POSTGRES): the PG side imports this module
// and materializes the collected messages into tuples. Collection is on the
// caller thread; the provider session drives the generated HTTP client.
module;

#include <cstddef>
#include <expected>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.external.providers.run;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.capabilities;
import bioseq.external.provider.runtime_services;
import bioseq.external.provider.budget;
import bioseq.external.providers.configured;
import bioseq.common.context;
import bioseq.parallelism.pool;

export namespace bioseq::external {

namespace prov = bioseq::external::provider;

/// Owning messages, counters, and terminal error from one provider run.
struct external_query_result {
  std::vector<prov::owning_provider_message> messages;
  prov::provider_statistics statistics{};
  std::optional<prov::external_error> error;

  /// Return whether planning, opening, and session execution all succeeded.
  [[nodiscard]] bool is_successful() const noexcept {
    return !error.has_value();
  }
};

// Collector satisfying the owning-message push protocol; appends to a vector.
namespace detail {

/// Append one owning provider message to a vector sink.
inline std::expected<void, prov::external_error>
collect_message(void *self, prov::owning_provider_message &&message) {
  auto *messages =
      static_cast<std::vector<prov::owning_provider_message> *>(self);
  messages->push_back(std::move(message));
  return {};
}

} // namespace detail

/// Run one configured provider synchronously and collect its owning messages.
///
/// Unknown providers and unsupported targets return a bounded error instead of
/// falling through to another adapter.
inline external_query_result run_external_query(
    context::context &memory, const bioseq::parallelism::global_pool &pool,
    prov::external_runtime_services &runtime,
    std::optional<prov::credential_view> credentials,
    prov::provider_id provider, const prov::normalized_external_query &query,
    const prov::external_slot_budget &budget) {
  external_query_result result;

  prov::provider_message_sink_ref sink{.object = &result.messages,
                                       .push_fn = &detail::collect_message};
  std::stop_source stop;

  // Generic handler: the pack invokes handler.operator()<p_provider_type>()
  // exactly once, for the adapter whose stable id equals `provider`.
  auto handler = [&memory, &pool, &runtime, &credentials, &query, &budget,
                  &sink, &stop, &result]<class p_provider_type>()
      -> std::expected<void, prov::external_error> {
    const prov::provider_capability_snapshot snapshot{};
    auto query_plan = p_provider_type::plan(query, snapshot, budget);
    if (!query_plan.has_value()) {
      return std::unexpected(query_plan.error());
    }
    auto session = p_provider_type::open(memory, pool, runtime, credentials,
                                         *query_plan, budget);
    if (!session.has_value()) {
      return std::unexpected(session.error());
    }
    auto provider_statistics_result = session->run(sink, stop.get_token());
    if (!provider_statistics_result.has_value()) {
      return std::unexpected(provider_statistics_result.error());
    }
    result.statistics = *provider_statistics_result;
    return {};
  };

  auto dispatch_result =
      providers::configured_external_api_provider::dispatch_to_provider(
          provider, handler);
  if (!dispatch_result.has_value()) {
    result.error = dispatch_result.error();
  }
  return result;
}

} // namespace bioseq::external
