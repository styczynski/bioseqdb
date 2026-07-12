// DOC0372 — the synchronous run-bridge between the PostgreSQL CustomScan and
// the compile-time provider pack. Given a normalized query and a target
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

export module bioseq.external.run;

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

// The owned outcome of one provider run. `messages` holds the normalized rows
// and diagnostics (owning variants — self-contained, safe to outlive the run).
// `error` is set when planning, opening, or running failed; `messages` may still
// hold whatever was emitted before the failure.
struct external_run_result {
    std::vector<prov::owning_provider_message> messages;
    prov::provider_statistics stats{};
    std::optional<prov::external_error> error;

    [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Collector satisfying the owning-message push protocol; appends to a vector.
namespace detail {

inline std::expected<void, prov::external_error> collect_message(
    void* self, prov::owning_provider_message&& message) {
    auto* out = static_cast<std::vector<prov::owning_provider_message>*>(self);
    out->push_back(std::move(message));
    return {};
}

}  // namespace detail

// Run one provider for one normalized query and collect its messages. Dispatch
// is by stable provider id through the configured pack; an unknown id yields an
// unsupported_capability error (never a fallthrough). The provider's own
// plan()/open() decide capability — an unsupported target returns an error and
// an empty buffer, which the PG layer renders as an empty (but valid) scan.
inline external_run_result run_external_query(
    context::context& memory, const bioseq::parallelism::GlobalPool& pool,
    prov::external_runtime_services& runtime,
    std::optional<prov::credential_view> credentials, prov::provider_id provider,
    const prov::normalized_external_query& query,
    const prov::external_slot_budget& budget) {
    external_run_result result;

    prov::provider_message_sink_ref sink{.object = &result.messages,
                                         .push_fn = &detail::collect_message};
    std::stop_source stop;

    // Generic handler: the pack invokes handler.operator()<Provider>() exactly
    // once, for the adapter whose stable id equals `provider`.
    auto handler = [&]<class Provider>() -> std::expected<void, prov::external_error> {
        const prov::provider_capability_snapshot snapshot{};
        auto plan = Provider::plan(query, snapshot, budget);
        if (!plan.has_value()) {
            return std::unexpected(plan.error());
        }
        auto session = Provider::open(memory, pool, runtime, credentials, *plan,
                                      budget);
        if (!session.has_value()) {
            return std::unexpected(session.error());
        }
        auto stats = session->run(sink, stop.get_token());
        if (!stats.has_value()) {
            return std::unexpected(stats.error());
        }
        result.stats = *stats;
        return {};
    };

    auto dispatched =
        providers::configured_external_api_provider::dispatch(provider, handler);
    if (!dispatched.has_value()) {
        result.error = dispatched.error();
    }
    return result;
}

}  // namespace bioseq::external
