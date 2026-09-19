/**
 * @file active_read_policy_resolution_contract.cppm
 * @brief Declares backend-local active-read resolution and dispatch diagnostics.
 */
module;

#include <cstdint>
#include <string>

export module bioseq.postgres.storage.indexing.active_read_policy_resolution_contract;

export import bioseq.postgres.storage.indexing.active_read_policy;

export namespace bioseq::storage::indexing {

/// @brief Selected active-read index and the authority that selected it.
struct active_read_policy_resolution {
    std::string index_name;
    active_read_policy_source source = active_read_policy_source::unresolved;
};

/// @brief Backend-local counts of active-read policy resolution outcomes.
struct active_read_policy_resolution_counters {
    std::uint64_t non_relation = 0;
    std::uint64_t configured_rows = 0;
    std::uint64_t authoritative_defaults = 0;
};

/// @brief Backend-local counts of batches dispatched by policy source.
struct active_read_policy_dispatch_counters {
    std::uint64_t configured_batches = 0;
    std::uint64_t authoritative_default_batches = 0;
    std::uint64_t non_relation_batches = 0;
};

// PostgreSQL backends are single-threaded. These counters intentionally show
// whether a default came from an authoritative empty policy result rather
// than treating an SPI failure as an empty result.
inline active_read_policy_resolution_counters g_active_read_policy_resolution_counters{};
inline active_read_policy_dispatch_counters g_active_read_policy_dispatch_stats{};
inline active_read_policy_source g_last_active_read_policy_dispatch_source =
    active_read_policy_source::unresolved;

/// @brief Clears backend-local policy diagnostics between integration checks.
inline void reset_active_read_policy_debug_state() noexcept {
    g_active_read_policy_resolution_counters = {};
    g_active_read_policy_dispatch_stats = {};
    g_last_active_read_policy_dispatch_source = active_read_policy_source::unresolved;
}

} // namespace bioseq::storage::indexing
