module;

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

module bioseq.codegen.external:client_renderer;

import :model;
import bioseq.codegen.support;

namespace bioseq::codegen::external {

/// @brief Derives the conventional API-key environment variable name.
/// @param client_name Generated client name.
/// @return Uppercase `<CLIENT>_API_KEY` spelling.
[[nodiscard]] std::string
api_key_environment_name(std::string_view client_name) {
  std::string result;
  result.reserve(client_name.size() + std::string_view{"_API_KEY"}.size());
  for (const char character : client_name) {
    if (character >= 'a' && character <= 'z') {
      result.push_back(static_cast<char>(character - 'a' + 'A'));
    } else if ((character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9')) {
      result.push_back(character);
    } else {
      result.push_back('_');
    }
  }
  result += "_API_KEY";
  return result;
}

/// @brief Renders a parameter-location enum expression.
/// @param location Normalized location.
/// @return Generated C++ enum literal.
[[nodiscard]] std::string location_literal(parameter_location location) {
  switch (location) {
  case parameter_location::path:
    return "parameter_location::path";
  case parameter_location::query:
    return "parameter_location::query";
  case parameter_location::header:
    return "parameter_location::header";
  }
  throw std::runtime_error("invalid parameter location");
}

/// @brief Renders a parameter-kind enum expression.
/// @param kind Normalized scalar kind.
/// @return Generated C++ enum literal.
[[nodiscard]] std::string kind_literal(parameter_kind kind) {
  switch (kind) {
  case parameter_kind::string:
    return "parameter_kind::string";
  case parameter_kind::integer:
    return "parameter_kind::integer";
  case parameter_kind::number:
    return "parameter_kind::number";
  case parameter_kind::boolean:
    return "parameter_kind::boolean";
  }
  throw std::runtime_error("invalid parameter kind");
}

/// @brief Renders a typed integration-fixture scalar as generated C++.
/// @param value Parsed fixture value.
/// @return C++ expression with an exact `parameter_scalar` alternative.
[[nodiscard]] std::string
integration_scalar_literal(const integration_scalar &value) {
  if (const auto *text = std::get_if<std::string>(&value)) {
    return "std::string_view{\"" + escape_cpp_string_literal(*text) + "\"}";
  }
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    if (*integer == std::numeric_limits<std::int64_t>::min()) {
      return "std::numeric_limits<std::int64_t>::min()";
    }
    return "std::int64_t{" + std::to_string(*integer) + "}";
  }
  if (const auto *number = std::get_if<double>(&value)) {
    std::ostringstream spelling;
    spelling << std::setprecision(std::numeric_limits<double>::max_digits10)
             << *number;
    return "double{" + spelling.str() + "}";
  }
  return std::get<bool>(value) ? "true" : "false";
}

/// @brief Renders the complete C++ module for one API client.
/// @param api Validated API model.
/// @return Generated module source.
[[nodiscard]] std::string render_module(const generated_api &api) {
  const std::string namespace_suffix = make_cpp_identifier(api.client_name);
  std::size_t total_parameter_count = 0;
  for (const auto &operation : api.document.get_operations) {
    total_parameter_count += operation.parameters.size();
  }
  std::ostringstream out;
  out << R"cpp(module;

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.external.api.)cpp"
      << namespace_suffix << R"cpp(;

import bioseq.common.context;
import bioseq.external.https;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

namespace bioseq::external::api::)cpp"
      << namespace_suffix << R"cpp( {

export enum class parameter_location : std::uint8_t { path, query, header };
enum class parameter_kind : std::uint8_t { string, integer, number, boolean };

export using parameter_scalar = std::variant<std::string_view, std::int64_t, double, bool>;
export using response_header = bioseq::external::http_header;

export struct parameter_value {
    std::string_view name;
    parameter_location location{};
    parameter_scalar value;
};

export inline constexpr std::size_t k_default_maximum_response_body_bytes =
    64U * 1024U * 1024U;

/// @brief Non-owning GET invocation whose views must outlive synchronous get_batch().
export struct get_request {
    std::string_view operation_id;
    std::span<const parameter_value> parameters{};
    std::size_t maximum_response_body_bytes{k_default_maximum_response_body_bytes};
    std::string_view accept{};
};

export struct credentials {
    std::string api_key;
};

export struct client_error {
    std::string message;
    long transport_code{};
};

export struct response {
    long status_code{};
    std::string body;
    std::vector<response_header> headers;

    /// @brief Views the heap-owned response bytes without copying them.
    [[nodiscard]] std::string_view body_view() const noexcept {
        return {body.data(), body.size()};
    }
};

namespace detail {

struct parameter_descriptor {
    std::string_view name;
    parameter_location location{};
    parameter_kind kind{};
    bool required{};
    bool is_array{};
};

struct operation_descriptor {
    std::string_view operation_id;
    std::string_view path;
    std::string_view accept;
    std::string_view api_key_name;
    parameter_location api_key_location{};
    bool uses_api_key{};
    std::size_t first_parameter{};
    std::size_t parameter_count{};
};

constexpr std::array<parameter_descriptor, )cpp"
      << total_parameter_count << R"cpp(> parameters{{
)cpp";
  std::size_t parameter_offset = 0;
  for (const auto &operation : api.document.get_operations) {
    for (const auto &parameter : operation.parameters) {
      out << "    parameter_descriptor{\"" << escape_cpp_string_literal(parameter.name)
          << "\", " << location_literal(parameter.location) << ", "
          << kind_literal(parameter.kind) << ", "
          << (parameter.required ? "true" : "false") << ", "
          << (parameter.is_array ? "true" : "false") << "},\n";
    }
  }
  out << "}};\n\nconstexpr std::array<operation_descriptor, "
      << api.document.get_operations.size() << "> operations{{\n";
  for (const auto &operation : api.document.get_operations) {
    out << "    operation_descriptor{\"" << escape_cpp_string_literal(operation.operation_id)
        << "\", \"" << escape_cpp_string_literal(operation.path) << "\", \""
        << escape_cpp_string_literal(operation.accept) << "\", \""
        << escape_cpp_string_literal(operation.api_key_name) << "\", "
        << location_literal(
               operation.api_key_location.value_or(parameter_location::header))
        << ", " << (operation.api_key_location ? "true" : "false") << ", "
        << parameter_offset << ", " << operation.parameters.size() << "},\n";
    parameter_offset += operation.parameters.size();
  }
  out << R"cpp(}};

/// @brief Finds generated metadata for an operation identifier.
/// @param id Operation identifier to locate.
/// @return Descriptor pointer, or null when the identifier is unknown.
[[nodiscard]] inline const operation_descriptor* find_operation(std::string_view id) noexcept {
    const auto found = std::ranges::lower_bound(
        operations, id, {}, &operation_descriptor::operation_id);
    return found != operations.end() && found->operation_id == id ? &*found : nullptr;
}

/// @brief Serializes a typed operation parameter for HTTP transport.
/// @param value Scalar value to serialize.
/// @return Transport spelling.
[[nodiscard]] inline std::string scalar_to_string(const parameter_scalar& value) {
    return std::visit([](const auto& item) -> std::string {
        using item_type = std::remove_cvref_t<decltype(item)>;
        if constexpr (std::same_as<item_type, std::string_view>) {
            return std::string(item);
        } else if constexpr (std::same_as<item_type, bool>) {
            return item ? "true" : "false";
        } else {
            std::array<char, 64> buffer{};
            const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), item);
            return converted.ec == std::errc{}
                ? std::string(buffer.data(), converted.ptr)
                : std::string{};
        }
    }, value);
}

/// @brief Checks a supplied scalar against generated schema metadata.
/// @param kind Expected scalar kind.
/// @param value Supplied scalar value.
/// @return True when the variant alternative matches the schema kind.
[[nodiscard]] inline bool value_matches(parameter_kind kind, const parameter_scalar& value) {
    switch (kind) {
    case parameter_kind::string: return std::holds_alternative<std::string_view>(value);
    case parameter_kind::integer: return std::holds_alternative<std::int64_t>(value);
    case parameter_kind::number: return std::holds_alternative<double>(value);
    case parameter_kind::boolean: return std::holds_alternative<bool>(value);
    }
    return false;
}

/// @brief Compares HTTP header names with ASCII case folding.
/// @param left First header name.
/// @param right Second header name.
/// @return True when both names are equal ignoring ASCII case.
[[nodiscard]] inline bool ascii_iequals(
    std::string_view left,
    std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](char character) {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a') : character;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

} // namespace detail

/// @brief Tests whether a response contains a named header value fragment.
/// @param headers Durable response headers.
/// @param name Header name compared with HTTP ASCII case folding.
/// @param value_contains Case-sensitive value fragment.
/// @return True when one header satisfies both predicates.
export [[nodiscard]] inline bool response_header_contains(
    std::span<const response_header> headers,
    std::string_view name,
    std::string_view value_contains) noexcept {
    return std::ranges::any_of(headers, [=](const response_header& header) {
        return detail::ascii_iequals(header.name, name)
            && header.value.find(value_contains) != std::string::npos;
    });
}

export struct )cpp"
      << api.declared_type << R"cpp( {
    constexpr static std::string client_name = ")cpp"
      << escape_cpp_string_literal(api.document.provider_name) << R"cpp(";
    constexpr static std::size_t k_default_response_body_limit =
        k_default_maximum_response_body_bytes;

    /// @brief Binds the result arena, worker pool, and optional authentication.
    )cpp"
      << api.declared_type << R"cpp((
        const context::context& memory_context,
        const bioseq::parallelism::global_pool& pool,
        std::optional<credentials> supplied_credentials = std::nullopt)
        : memory_context_(memory_context)
        , pool_(pool)
        , credentials_(std::move(supplied_credentials))
        , transport_(")cpp"
      << escape_cpp_string_literal(api.document.host) << "\", " << api.document.port
      << R"cpp() {}

    /// @brief Executes one generated GET operation into the result arena.
    [[nodiscard]] std::expected<response, client_error> get(
        std::string_view operation_id,
        std::span<const parameter_value> supplied = {},
        std::size_t maximum_response_body_bytes = k_default_response_body_limit) {
        auto transported = request_transport(
            operation_id, supplied, {}, maximum_response_body_bytes);
        if (!transported) {
            return std::unexpected(std::move(transported.error()));
        }
        return materialize(std::move(*transported));
    }

    /// @brief Executes one generated GET with an explicit Accept override.
    [[nodiscard]] std::expected<response, client_error> get_with_accept(
        std::string_view operation_id,
        std::span<const parameter_value> supplied,
        std::string_view accept,
        std::size_t maximum_response_body_bytes = k_default_response_body_limit) {
        auto transported = request_transport(
            operation_id, supplied, accept, maximum_response_body_bytes);
        if (!transported) {
            return std::unexpected(std::move(transported.error()));
        }
        return materialize(std::move(*transported));
    }

    /// @brief Runs transports on the worker pool, then materializes responses serially.
    [[nodiscard]] std::vector<std::expected<response, client_error>>
    get_batch(std::span<const get_request> requests) {
        std::vector<std::expected<bioseq::external::transport_response, client_error>> slots(
            requests.size());
        if (!requests.empty()) {
            pool_.parallel_for(
                0, requests.size(), 1,
                bioseq::parallelism::range_body{
                    [this, requests, &slots](std::size_t begin, std::size_t end) {
                        for (std::size_t index = begin; index < end; ++index) {
                            slots[index] = request_transport(
                                requests[index].operation_id,
                                requests[index].parameters,
                                requests[index].accept,
                                requests[index].maximum_response_body_bytes);
                        }
                    }});
        }
        std::vector<std::expected<response, client_error>> result(
            requests.size());
        for (std::size_t index = 0; index < requests.size(); ++index) {
            auto& slot = slots[index];
            if (slot) {
                result[index] = materialize(std::move(*slot));
            } else {
                result[index] = std::unexpected(std::move(slot.error()));
            }
        }
        return result;
    }

private:
    /// @brief Validates parameters and performs transport without touching the result arena.
    [[nodiscard]] std::expected<bioseq::external::transport_response, client_error>
    request_transport(
        std::string_view operation_id,
        std::span<const parameter_value> supplied,
        std::string_view accept,
        std::size_t maximum_response_body_bytes) const {
        using namespace detail;
        const operation_descriptor* operation = find_operation(operation_id);
        if (operation == nullptr) {
            return std::unexpected(client_error{"unknown GET operation: " + std::string(operation_id), 0});
        }
        std::string path = ")cpp"
      << escape_cpp_string_literal(api.document.base_path) << R"cpp(";
        path += operation->path;
        std::vector<bioseq::external::http_header> headers;
        headers.push_back({"Accept", std::string(
            accept.empty() ? operation->accept : accept)});
        bool has_query = false;
        const auto descriptors = std::span{parameters}.subspan(
            operation->first_parameter, operation->parameter_count);
        for (const auto& descriptor : descriptors) {
            std::vector<const parameter_value*> values;
            for (const auto& candidate : supplied) {
                if (candidate.name == descriptor.name && candidate.location == descriptor.location) {
                    values.push_back(&candidate);
                }
            }
            if (descriptor.required && values.empty()) {
                return std::unexpected(client_error{
                    "missing required parameter: " + std::string(descriptor.name), 0});
            }
            if (!descriptor.is_array && values.size() > 1) {
                return std::unexpected(client_error{
                    "scalar parameter repeated: " + std::string(descriptor.name), 0});
            }
            std::vector<std::string> serialized_values;
            serialized_values.reserve(values.size());
            for (const parameter_value* value : values) {
                if (!value_matches(descriptor.kind, value->value)) {
                    return std::unexpected(client_error{
                        "parameter type mismatch: " + std::string(descriptor.name), 0});
                }
                serialized_values.push_back(scalar_to_string(value->value));
            }
            if (descriptor.location == parameter_location::path && !serialized_values.empty()) {
                std::string joined;
                for (const auto& value : serialized_values) {
                    if (!joined.empty()) joined += ',';
                    joined += bioseq::external::encode_url_component(value);
                }
                const std::string marker = "{" + std::string(descriptor.name) + "}";
                const auto position = path.find(marker);
                if (position == std::string::npos) {
                    return std::unexpected(client_error{"path marker not found: " + marker, 0});
                }
                path.replace(position, marker.size(), joined);
            } else if (descriptor.location == parameter_location::query) {
                for (const auto& value : serialized_values) {
                    path += has_query ? '&' : '?';
                    has_query = true;
                    path += bioseq::external::encode_url_component(descriptor.name);
                    path += '=';
                    path += bioseq::external::encode_url_component(value);
                }
            } else if (descriptor.location == parameter_location::header) {
                if (!serialized_values.empty()) {
                    std::string joined;
                    for (const auto& value : serialized_values) {
                        if (!joined.empty()) joined += ',';
                        joined += value;
                    }
                    headers.push_back({std::string(descriptor.name), std::move(joined)});
                }
            }
        }
        for (const auto& candidate : supplied) {
            const bool known = std::ranges::any_of(descriptors, [&candidate](const parameter_descriptor& descriptor) {
                return descriptor.name == candidate.name && descriptor.location == candidate.location;
            });
            if (!known) {
                return std::unexpected(client_error{
                    "unknown operation parameter: " + std::string(candidate.name), 0});
            }
        }
        if (operation->uses_api_key && credentials_ && !credentials_->api_key.empty()) {
            if (operation->api_key_location == parameter_location::header) {
                headers.push_back({std::string(operation->api_key_name), credentials_->api_key});
            } else {
                path += has_query ? '&' : '?';
                has_query = true;
                path += bioseq::external::encode_url_component(operation->api_key_name);
                path += '=';
                path += bioseq::external::encode_url_component(credentials_->api_key);
            }
        }
        auto result = transport_.get(
            std::move(path), headers, maximum_response_body_bytes);
        if (!result) {
            return std::unexpected(client_error{
                .message = std::move(result.error().message),
                .transport_code = result.error().transport_code,
            });
        }
        return std::move(*result);
    }

    /// @brief Adopts a transported response body for the response lifetime.
    ///
    /// The body is transient scratch — parsed exactly once into owning provider
    /// messages, then discarded — so it lives on the standard heap, not a
    /// context arena. Routing an arbitrarily large HTTP body (up to
    /// k_default_maximum_response_body_bytes = 64 MiB) through the small-object
    /// result arena is both unsupportable (its 64 KiB blocks are far smaller
    /// than a body can be, which trips a foonathan_memory reserve assertion)
    /// and pointless (a needless copy). Moving the transport's std::string
    /// through is O(1) and cannot fail.
    [[nodiscard]] response materialize(bioseq::external::transport_response transported) {
        return response{
            transported.status_code,
            std::move(transported.body),
            std::move(transported.headers),
        };
    }

    const context::context& memory_context_;
    const bioseq::parallelism::global_pool& pool_;
    std::optional<credentials> credentials_;
    bioseq::external::https_client transport_;
};

} // namespace bioseq::external::api::)cpp"
      << namespace_suffix << R"cpp(
)cpp";
  return out.str();
}

/// @brief Renders a live Catch2 integration test for one client.
/// @param api Validated API model.
/// @return Generated integration-test source.
[[nodiscard]] std::string render_test(const generated_api &api) {
  const std::string namespace_suffix = make_cpp_identifier(api.client_name);
  std::ostringstream out;
  out << R"cpp(#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

import bioseq.common.context;
import bioseq.external.api.)cpp"
      << namespace_suffix << R"cpp(;
import bioseq.external.https;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;

namespace generated_api = bioseq::external::api::)cpp"
      << namespace_suffix << R"cpp(;

struct generated_pool_restore {
    bioseq::parallelism::global_pool& pool;
    bool was_active;
    bioseq::parallelism::pool_config previous;
    bool changed = false;

    /// @brief Restores the prior global pool state without throwing.
    ~generated_pool_restore() {
        if (!changed) return;
        try {
            if (was_active) pool.restart(previous);
            else pool.shutdown();
        } catch (...) {
        }
    }
};

TEST_CASE("generated )cpp"
      << namespace_suffix << R"cpp( external API client performs live requests",
          "[component:external_api][layer:integration][feature:external_source]"
          "[resource:network]") {
    bioseq::external::https_client validation_transport("example.org");
    REQUIRE_FALSE(validation_transport.get("/invalid\npath").has_value());
    const std::vector<bioseq::external::http_header> invalid_headers{
        {"Bad Header", "value"},
    };
    REQUIRE_FALSE(validation_transport.get("/", invalid_headers).has_value());
    REQUIRE_FALSE(validation_transport.get("/", {}, 0).has_value());

    context::context memory_context;
    auto& pool = bioseq::parallelism::global_pool::instance();
    generated_pool_restore restore{
        .pool = pool,
        .was_active = pool.is_active(),
        .previous = bioseq::parallelism::pool_config{
            .maximum_concurrency = pool.maximum_concurrency(),
            .mode = pool.get_mode(),
        },
    };
    if (!pool.is_active() || pool.maximum_concurrency() < 2) {
        restore.changed = true;
        pool.restart(bioseq::parallelism::pool_config{
            .maximum_concurrency = 2,
            .mode = bioseq::parallelism::pipeline_concurrency_mode::task_parallel,
        });
    }
    std::optional<generated_api::credentials> supplied_credentials;
    [[maybe_unused]] bool has_api_key = false;
    if (const char* api_key = std::getenv(")cpp"
      << api_key_environment_name(api.client_name)
      << R"cpp("); api_key != nullptr && *api_key != '\0') {
        supplied_credentials = generated_api::credentials{std::string(api_key)};
        has_api_key = true;
    }
    generated_api::)cpp"
      << api.declared_type << R"cpp( client(
        memory_context, pool, std::move(supplied_credentials));
    REQUIRE(client.client_name == ")cpp"
      << escape_cpp_string_literal(api.document.provider_name) << R"cpp(");
    const generated_api::get_request positional_request{
        "__bioseq_compatibility_probe__", {}, 1U};
    REQUIRE(positional_request.maximum_response_body_bytes == 1U);
    REQUIRE(positional_request.accept.empty());
    auto positional_response = client.get(
        "__bioseq_compatibility_probe__", {}, 1U);
    REQUIRE_FALSE(positional_response.has_value());
    auto brace_response = client.get(
        "__bioseq_compatibility_probe__", {}, {});
    REQUIRE_FALSE(brace_response.has_value());
)cpp";
  for (std::size_t case_index = 0;
       case_index < api.document.integration_cases.size(); ++case_index) {
    const auto &fixture = api.document.integration_cases[case_index];
    const std::string prefix = "fixture_" + std::to_string(case_index);
    out << "\n    INFO(\"fixture: " << escape_cpp_string_literal(fixture.name) << "\");\n";
    if (fixture.pre_delay_without_credentials_ms != 0) {
      out << "    if (!has_api_key) {\n"
          << "        std::this_thread::sleep_for(std::chrono::milliseconds("
          << fixture.pre_delay_without_credentials_ms << "));\n"
          << "    }\n";
    }
    if (!fixture.parameters.empty()) {
      out << "    const std::array " << prefix << "_parameters{\n";
      for (const auto &parameter : fixture.parameters) {
        out << "        generated_api::parameter_value{\""
            << escape_cpp_string_literal(parameter.name)
            << "\", generated_api::" << location_literal(parameter.location)
            << ", " << integration_scalar_literal(parameter.value) << "},\n";
      }
      out << "    };\n";
    }

    const std::string parameter_expression =
        fixture.parameters.empty() ? std::string{"{}"} : prefix + "_parameters";
    const std::string accept_expression =
        "std::string_view{\"" + escape_cpp_string_literal(fixture.accept) + "\"}";
    if (fixture.mode == integration_mode::single) {
      out << "    auto " << prefix << "_response = client."
          << (fixture.accept.empty() ? "get" : "get_with_accept") << "(\""
          << escape_cpp_string_literal(fixture.operation_id) << "\", " << parameter_expression;
      if (!fixture.accept.empty()) {
        out << ", " << accept_expression;
      }
      out << ");\n"
          << "    INFO((" << prefix << "_response ? std::string(" << prefix
          << "_response->body_view()) : " << prefix
          << "_response.error().message));\n"
          << "    REQUIRE(" << prefix << "_response.has_value());\n"
          << "    REQUIRE(" << prefix
          << "_response->status_code == " << fixture.expected_status << ");\n";
      for (const auto &marker : fixture.body_contains) {
        out << "    REQUIRE(" << prefix << "_response->body_view().find(\""
            << escape_cpp_string_literal(marker) << "\") != std::string_view::npos);\n";
      }
      for (const auto &header : fixture.header_contains) {
        out << "    REQUIRE(generated_api::response_header_contains(" << prefix
            << "_response->headers, \"" << escape_cpp_string_literal(header.name) << "\", \""
            << escape_cpp_string_literal(header.value_contains) << "\"));\n";
      }
    } else {
      out << "    const std::array " << prefix << "_requests{\n";
      for (std::size_t request_index = 0; request_index < fixture.count;
           ++request_index) {
        out << "        generated_api::get_request{\n"
            << "            .operation_id = \""
            << escape_cpp_string_literal(fixture.operation_id) << "\",\n"
            << "            .parameters = " << parameter_expression;
        if (!fixture.accept.empty()) {
          out << ",\n            .accept = " << accept_expression;
        }
        out << ",\n        },\n";
      }
      out << "    };\n"
          << "    auto " << prefix << "_responses = client.get_batch(" << prefix
          << "_requests);\n"
          << "    REQUIRE(" << prefix
          << "_responses.size() == " << fixture.count << ");\n"
          << "    for (const auto& item : " << prefix << "_responses) {\n"
          << "        INFO((item ? std::string(item->body_view()) : "
             "item.error().message));\n"
          << "        REQUIRE(item.has_value());\n"
          << "        REQUIRE(item->status_code == " << fixture.expected_status
          << ");\n";
      for (const auto &marker : fixture.body_contains) {
        out << "        REQUIRE(item->body_view().find(\"" << escape_cpp_string_literal(marker)
            << "\") != std::string_view::npos);\n";
      }
      for (const auto &header : fixture.header_contains) {
        out << "        REQUIRE(generated_api::response_header_contains("
            << "item->headers, \"" << escape_cpp_string_literal(header.name) << "\", \""
            << escape_cpp_string_literal(header.value_contains) << "\"));\n";
      }
      out << "    }\n";
    }
  }
  out << R"cpp(
}
)cpp";
  return out.str();
}

} // namespace bioseq::codegen::external
