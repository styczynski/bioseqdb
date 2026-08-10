module;

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

export module bioseq.external.provider.errors;

import bioseq.external.provider.base;

/// @brief Structured, owning, PostgreSQL-independent errors.
// `external_error`
// imports only `base` (never sink refs) so the dependency chain stays acyclic.
export namespace bioseq::external::provider {

enum class external_error_kind : std::uint8_t {
  invalid_query,
  unsupported_capability,
  not_implemented,
  transport,
  timeout,
  http,
  rate_limited,
  incomplete_result,
  response_too_large,
  parse,
  integrity,
  cancelled,
  provider_contract
};

struct external_error {
  external_error_kind kind{};
  std::optional<provider_id> known_provider;
  std::optional<bounded_string<maximum_provider_name_bytes>> provider_name;
  bounded_string<maximum_identifier_bytes> operation;
  bounded_string<maximum_identifier_bytes> item_id;
  long http_status{};
  long transport_code{};
  bounded_string<maximum_identifier_bytes> request_id;
  bounded_string<256> content_type;
  bounded_string<maximum_diagnostic_bytes> message;
  bounded_string<maximum_diagnostic_bytes> response_excerpt;
  std::optional<std::chrono::seconds> retry_after;
  bool is_retryable{};
};

// Checked factory. All construction goes through here so the compile-time caps
// and the smaller runtime error-body admission cap are both enforced; message
// text is truncated rather than rejected so an error is never lost.
[[nodiscard]] inline external_error
make_external_error(external_error_kind kind, std::string_view message,
                    std::optional<provider_id> known_provider = std::nullopt) {
  external_error error;
  error.kind = kind;
  error.known_provider = known_provider;
  error.message =
      bounded_string<maximum_diagnostic_bytes>::make_truncated(message);
  error.is_retryable = (kind == external_error_kind::timeout ||
                        kind == external_error_kind::rate_limited ||
                        kind == external_error_kind::transport);
  return error;
}

[[nodiscard]] inline external_error
make_external_error(external_error_kind kind, provider_id provider,
                    std::string_view operation, std::string_view message) {
  external_error error = make_external_error(kind, message, provider);
  error.operation =
      bounded_string<maximum_identifier_bytes>::make_truncated(operation);
  error.provider_name =
      bounded_string<maximum_provider_name_bytes>::make_truncated(
          provider.value);
  return error;
}

[[nodiscard]] constexpr std::string_view
to_string(external_error_kind kind) noexcept {
  switch (kind) {
  case external_error_kind::invalid_query:
    return "invalid_query";
  case external_error_kind::unsupported_capability:
    return "unsupported_capability";
  case external_error_kind::not_implemented:
    return "not_implemented";
  case external_error_kind::transport:
    return "transport";
  case external_error_kind::timeout:
    return "timeout";
  case external_error_kind::http:
    return "http";
  case external_error_kind::rate_limited:
    return "rate_limited";
  case external_error_kind::incomplete_result:
    return "incomplete_result";
  case external_error_kind::response_too_large:
    return "response_too_large";
  case external_error_kind::parse:
    return "parse";
  case external_error_kind::integrity:
    return "integrity";
  case external_error_kind::cancelled:
    return "cancelled";
  case external_error_kind::provider_contract:
    return "provider_contract";
  }
  return "provider_contract";
}

} // namespace bioseq::external::provider
