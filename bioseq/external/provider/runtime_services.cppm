/**
 * @file external/provider/runtime_services.cppm
 * @brief Bounded runtime services supplied to provider executions.
 */
module;

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>

export module bioseq.external.provider.runtime_services;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.capabilities;

/// @brief Type-erased runtime service references and request-policy views.
// structs. Every dependency the provider core injects (clock, sleeper, jitter
// RNG, capability cache, request policy) is a trivially-copyable ref: an opaque
// object pointer plus function pointers. This keeps the core free of the
// concrete runtime, PostgreSQL, and HTTP edges.
export namespace bioseq::external::provider {

// ---------------------------------------------------------------------------
// Credential views (backed by a statement-owned credential_store).
// ---------------------------------------------------------------------------
struct credential_entry_view {
  std::string_view name;
  std::span<const std::byte> secret;
};

struct credential_quota_identity_view {
  request_policy_id policy;
  std::string_view rate_tier;
  std::array<std::byte, 32> digest;
};

struct credential_view {
  std::span<const credential_entry_view> entries;
  std::span<const credential_quota_identity_view> quota_identities;
};

// ---------------------------------------------------------------------------
// Injected runtime primitives.
// ---------------------------------------------------------------------------
struct external_clock_ref {
  void *object{};
  std::chrono::nanoseconds (*monotonic_now_fn)(void *) noexcept {};
  external_timestamp (*wall_now_fn)(void *) noexcept {};
};

struct external_sleeper_ref {
  void *object{};
  bool (*sleep_until_fn)(void *, std::chrono::nanoseconds, std::stop_token){};
};

struct external_jitter_rng_ref {
  void *object{};
  std::uint64_t (*next_fn)(void *) noexcept {};
};

struct provider_capability_cache_ref {
  void *object{};
  std::shared_ptr<const owning_provider_capability_snapshot> (*load_fn)(
      void *, provider_id){};
  void (*publish_fn)(
      void *, std::shared_ptr<const owning_provider_capability_snapshot>){};
  void (*reset_for_tests_fn)(void *) noexcept {};
};

// ---------------------------------------------------------------------------
// Request-policy views and the type-erased policy reference.
// ---------------------------------------------------------------------------
struct request_rate_tier_view {
  std::string_view name;
  std::chrono::nanoseconds initial_spacing;
  std::chrono::nanoseconds minimum_spacing;
  std::uint32_t burst;
  std::uint32_t maximum_in_flight;
};

struct http_status_interval {
  std::uint16_t first;
  std::uint16_t last;
};

struct request_policy_profile_view {
  request_policy_id id;
  std::span<const request_rate_tier_view> rate_tiers;
  std::span<const http_status_interval> retryable_http_statuses;
  std::uint32_t maximum_attempts;
  std::chrono::nanoseconds initial_backoff;
  std::chrono::nanoseconds maximum_backoff;
  std::chrono::nanoseconds maximum_retry_after;
  bool retry_connect_failure;
  bool retry_idle_timeout;
};

struct normalized_rate_observation {
  std::optional<std::chrono::nanoseconds> advertised_spacing;
  std::optional<std::uint64_t> remaining;
  std::optional<std::chrono::nanoseconds> reset_at;
  std::optional<std::chrono::nanoseconds> retry_at;
};

enum class policy_attempt_outcome : std::uint8_t {
  response,
  transport_failure,
  timeout,
  cancelled
};

struct policy_attempt_token {
  std::uint64_t slot;
  std::uint64_t generation;
};

enum class request_policy_action : std::uint8_t {
  proceed,
  retry_at,
  do_not_retry
};

struct request_policy_result {
  request_policy_action action;
  std::chrono::nanoseconds retry_at{};         // Absolute monotonic time.
  std::optional<policy_attempt_token> attempt; // Present only for proceed.
};

struct policy_request_view {
  request_policy_profile_view profile;
  std::string_view rate_tier;
  std::string_view normalized_origin;
  std::array<std::byte, 32> credential_identity;
  std::uint32_t attempt;
  bool is_idempotent;
  std::chrono::nanoseconds total_deadline; // Absolute monotonic time.
};

struct policy_response_view {
  policy_attempt_outcome outcome;
  std::optional<int> http_status;
  std::optional<long> transport_code;
  normalized_rate_observation rate;
};

struct request_policy_ref {
  void *object{};
  std::expected<request_policy_result, external_error> (*acquire_fn)(
      void *, const policy_request_view &, std::stop_token){};
  std::expected<request_policy_result, external_error> (*observe_fn)(
      void *, policy_attempt_token, const policy_request_view &,
      const policy_response_view &){};
  void (*abandon_fn)(void *, policy_attempt_token,
                     const policy_request_view &) noexcept {};
  void (*reset_for_tests_fn)(void *) noexcept {};
};

struct external_runtime_services {
  external_clock_ref clock;
  external_sleeper_ref sleeper;
  external_jitter_rng_ref jitter;
  provider_capability_cache_ref capability_cache;
  request_policy_ref requests;
};

} // namespace bioseq::external::provider
