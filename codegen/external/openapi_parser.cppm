module;

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

module bioseq.codegen.external:openapi_parser;

import :model;
import bioseq.codegen.support;
import bioseq.common.http.validation;

namespace http_validation = bioseq::common::http::validation;

namespace bioseq::codegen::external {

/// @brief Resolves a local JSON Pointer reference within an OpenAPI document.
/// @param document Root OpenAPI node.
/// @param node Node that may contain `$ref`.
/// @return Resolved node or the original non-reference node.
[[nodiscard]] YAML::Node resolve_local_ref(const YAML::Node& document, const YAML::Node& node) {
    if (!node || !node["$ref"]) {
        return node;
    }
    const std::string reference = node["$ref"].as<std::string>();
    constexpr std::string_view prefix = "#/";
    if (!reference.starts_with(prefix)) {
        throw std::runtime_error("only local OpenAPI parameter references are supported: " + reference);
    }
    YAML::Node current = document;
    std::size_t begin = prefix.size();
    while (begin <= reference.size()) {
        const auto slash = reference.find('/', begin);
        std::string key = reference.substr(begin, slash - begin);
        std::size_t position = 0;
        while ((position = key.find("~1", position)) != std::string::npos) {
            key.replace(position, 2, "/");
        }
        position = 0;
        while ((position = key.find("~0", position)) != std::string::npos) {
            key.replace(position, 2, "~");
        }
        current.reset(current[key]);
        if (!current) {
            throw std::runtime_error("unresolved OpenAPI reference: " + reference);
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1;
    }
    return current;
}

/// @brief Reads an OpenAPI parameter's location.
/// @param value OpenAPI `in` value.
/// @return Path, query or header  location.
/// @throws std::runtime_error For an unsupported location.
[[nodiscard]] parameter_location parse_location(std::string_view value) {
    if (value == "path") return parameter_location::path;
    if (value == "query") return parameter_location::query;
    if (value == "header") return parameter_location::header;
    throw std::runtime_error("unsupported OpenAPI parameter location: " + std::string(value));
}

/// @brief Reads an OpenAPI parameter's scalar type.
/// @param value OpenAPI schema type. An empty type means string
/// @return String, integer, number or boolean kind.
/// @throws std::runtime_error For an unsupported type.
[[nodiscard]] parameter_kind parse_kind(std::string_view value) {
    if (value == "string" || value.empty()) return parameter_kind::string;
    if (value == "integer") return parameter_kind::integer;
    if (value == "number") return parameter_kind::number;
    if (value == "boolean") return parameter_kind::boolean;
    throw std::runtime_error("unsupported OpenAPI parameter type: " + std::string(value));
}

/// @brief Identifies request-header parameters reserved by OpenAPI itself.
/// @param name Validated HTTP header name.
/// @return True for Accept, Content-Type, or Authorization, ignoring ASCII case.
[[nodiscard]] bool reserved_openapi_header_parameter(std::string_view name) noexcept {
    const auto equals = [name](std::string_view expected) {
        if (name.size() != expected.size()) return false;
        for (std::size_t index = 0; index < name.size(); ++index) {
            const auto fold = [](char character) {
                return character >= 'A' && character <= 'Z'
                    ? static_cast<char>(character - 'A' + 'a') : character;
            };
            if (fold(name[index]) != fold(expected[index])) return false;
        }
        return true;
    };
    return equals("accept") || equals("content-type") || equals("authorization");
}

/// @brief Reads one parameter definition, resolving local references when needed.
/// @param document Root OpenAPI node.
/// @param unresolved Parameter node or reference.
/// @return Parameter name, location, type and required/array flags.
[[nodiscard]] openapi_parameter parse_parameter(
    const YAML::Node& document,
    const YAML::Node& unresolved) {
    const YAML::Node parameter = resolve_local_ref(document, unresolved);
    if (!parameter["name"] || !parameter["in"] || !parameter["schema"]) {
        throw std::runtime_error("OpenAPI parameter is missing name, in, or schema");
    }
    YAML::Node schema = resolve_local_ref(document, parameter["schema"]);
    std::string type = schema["type"] ? schema["type"].as<std::string>() : "string";
    bool is_array = type == "array";
    if (is_array) {
        if (!schema["items"]) {
            throw std::runtime_error("array parameter has no items schema");
        }
        const YAML::Node items = resolve_local_ref(document, schema["items"]);
        type = items["type"] ? items["type"].as<std::string>() : "string";
    }
    const std::string name = parameter["name"].as<std::string>();
    const auto location = parse_location(parameter["in"].as<std::string>());
    if (location == parameter_location::header
        && !http_validation::is_valid_header_name(name)) {
        throw std::runtime_error("OpenAPI header parameter name is not an HTTP token");
    }
    const bool required = location == parameter_location::path
        || (parameter["required"] && parameter["required"].as<bool>());
    return openapi_parameter{
        .name = name,
        .location = location,
        .kind = parse_kind(type),
        .required = required,
        .is_array = is_array,
    };
}

/// @brief Adds parameters, replacing an earlier entry with the same name and location.
/// @param document Root OpenAPI node.
/// @param list Parameters from a path or operation.
/// @param destination Parameters collected so far.
/// @details Operation parameters override  path parameters. Accept, Content-Type
/// and Authorization header parameters are skipped.
void append_parameters(
    const YAML::Node& document,
    const YAML::Node& list,
    std::vector<openapi_parameter>& destination) {
    if (!list) return;
    if (!list.IsSequence()) {
        throw std::runtime_error("OpenAPI parameters must be a sequence");
    }
    for (const auto& value : list) {
        auto parsed = parse_parameter(document, value);
        if (parsed.location == parameter_location::header
            && reserved_openapi_header_parameter(parsed.name)) {
            continue;
        }
        const auto duplicate = std::find_if(
            destination.begin(), destination.end(), [&parsed](const openapi_parameter& existing) {
                return existing.name == parsed.name && existing.location == parsed.location;
            });
        if (duplicate == destination.end()) {
            destination.push_back(std::move(parsed));
        } else {
            *duplicate = std::move(parsed);
        }
    }
}

/// @brief Chooses an Accept header from the operation's response types.
/// @param document Root OpenAPI node.
/// @param operation Operation containing response definitions.
/// @return Preferred media type, or `*/*` if none is listed.
/// @details Response 200 takes priority, followed by other successful responses
/// and then the remaining responses. JSON types are preferred within eacf group.
[[nodiscard]] std::string preferred_response_media_type(
    const YAML::Node& document,
    const YAML::Node& operation) {
    std::vector<std::string> primary_media_types;
    std::vector<std::string> success_media_types;
    std::vector<std::string> fallback_media_types;
    const YAML::Node responses = operation["responses"];
    if (!responses || !responses.IsMap()) return "*/*";
    for (const auto& response_entry : responses) {
        const std::string status = response_entry.first.as<std::string>();
        const YAML::Node response = resolve_local_ref(document, response_entry.second);
        const YAML::Node content = response["content"];
        if (!content || !content.IsMap()) continue;
        auto& destination = status == "200" ? primary_media_types
            : status.size() == 3 && status.front() == '2' ? success_media_types
            : fallback_media_types;
        for (const auto& media_entry : content) {
            destination.push_back(media_entry.first.as<std::string>());
        }
    }
    auto choose = [](const std::vector<std::string>& media_types) -> std::string {
        if (media_types.empty()) return {};
        for (const auto& media_type : media_types) {
            std::string normalized(
                std::string_view{media_type}.substr(0, media_type.find(';')));
            while (!normalized.empty()
                   && (normalized.front() == ' ' || normalized.front() == '\t')) {
                normalized.erase(normalized.begin());
            }
            while (!normalized.empty()
                   && (normalized.back() == ' ' || normalized.back() == '\t')) {
                normalized.pop_back();
            }
            for (char& character : normalized) {
                if (character >= 'A' && character <= 'Z') {
                    character = static_cast<char>(character - 'A' + 'a');
                }
            }
            if (normalized == "application/json" || normalized.ends_with("+json")
                || normalized == "application/x-ndjson") {
                // EBI rejects application/json with the charset parameter from its own
                // specification. Drop a lone charset parameter for this media type. Keep
                // other parameters, including versions on, vendor media types.
                if (normalized == "application/json") {
                    const auto separator = media_type.find(';');
                    if (separator == std::string::npos) return media_type;
                    std::string parameters = media_type.substr(separator + 1);
                    parameters.erase(std::remove_if(
                        parameters.begin(), parameters.end(), [](char character) {
                            return character == ' ' || character == '\t';
                        }), parameters.end());
                    for (char& character : parameters) {
                        if (character >= 'A' && character <= 'Z') {
                            character = static_cast<char>(character - 'A' + 'a');
                        }
                    }
                    if (parameters.starts_with("charset=")
                        && !std::string_view{parameters}.contains(';')) {
                        return "application/json";
                    }
                }
                return media_type;
            }
        }
        return media_types.front();
    };
    if (auto selected = choose(primary_media_types); !selected.empty()) return selected;
    if (auto selected = choose(success_media_types); !selected.empty()) return selected;
    if (auto selected = choose(fallback_media_types); !selected.empty()) return selected;
    return "*/*";
}

struct api_key_requirement {
    std::string name;
    parameter_location location;
};

/// @brief Reads the operation's API-key requirement, falling  back to document security.
/// @param document Root OpenAPI node.
/// @param operation Operation whose security setting overrides the document.
/// @return Header or query API-key settings, or std::nullopt when none apply.
/// @throws std::runtime_error For unsupported or conflicting security schemes.
[[nodiscard]] std::optional<api_key_requirement> effective_api_key_requirement(
    const YAML::Node& document,
    const YAML::Node& operation) {
    const YAML::Node operation_security = operation["security"];
    const YAML::Node security = operation_security.IsDefined()
        ? operation_security : document["security"];
    if (!security) return std::nullopt;
    if (!security.IsSequence()) {
        throw std::runtime_error("OpenAPI security must be a sequence");
    }
    if (security.size() == 0) return std::nullopt;
    std::optional<api_key_requirement> selected;
    for (const auto& requirement : security) {
        if (!requirement.IsMap()) {
            throw std::runtime_error("OpenAPI security requirement must be a mapping");
        }
        if (requirement.size() == 0) continue;
        const YAML::Node components = document["components"];
        const YAML::Node schemes = components && components.IsMap()
            ? components["securitySchemes"] : YAML::Node{};
        if (!schemes || !schemes.IsMap()) {
            throw std::runtime_error(
                "OpenAPI security requirements need components.securitySchemes");
        }
        if (requirement.size() != 1) {
            throw std::runtime_error(
                "compound OpenAPI security requirements are not supported");
        }
        const auto requested_scheme = *requirement.begin();
        const std::string scheme_name = requested_scheme.first.as<std::string>();
        if (!schemes[scheme_name]) {
            throw std::runtime_error(
                "OpenAPI security requirement references an unknown scheme: "
                + scheme_name);
        }
        const YAML::Node scheme = resolve_local_ref(document, schemes[scheme_name]);
        if (!scheme || !scheme["type"]
            || scheme["type"].as<std::string>() != "apiKey"
            || !scheme["in"] || !scheme["name"]) {
            throw std::runtime_error(
                "only header/query OpenAPI apiKey security is supported: "
                + scheme_name);
        }
        const std::string location = scheme["in"].as<std::string>();
        if (location != "header" && location != "query") {
            throw std::runtime_error(
                "OpenAPI apiKey security location must be header or query: "
                + scheme_name);
        }
        api_key_requirement candidate{
            .name = scheme["name"].as<std::string>(),
            .location = parse_location(location),
        };
        if (candidate.name.empty()
            || (candidate.location == parameter_location::header
                && !http_validation::is_valid_header_name(candidate.name))) {
            throw std::runtime_error(
                "OpenAPI API-key name is invalid: " + scheme_name);
        }
        if (selected && (selected->name != candidate.name
                         || selected->location != candidate.location)) {
            throw std::runtime_error(
                "multiple alternative OpenAPI API-key schemes are not supported");
        }
        selected = std::move(candidate);
    }
    return selected;
}

/// @brief Uses the first title word as the provider name.
/// @param title OpenAPI info.title value.
/// @return First word after an optional leading "The" or "the".
[[nodiscard]] std::string provider_name_from_title(std::string_view title) {
    std::size_t begin = title.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        throw std::runtime_error("OpenAPI info.title is empty");
    }
    std::size_t end = title.find_first_of(" \t", begin);
    std::string_view token = title.substr(begin, end - begin);
    if ((token == "The" || token == "the") && end != std::string_view::npos) {
        begin = title.find_first_not_of(" \t", end);
        if (begin == std::string_view::npos) {
            throw std::runtime_error("OpenAPI info.title contains only an article");
        }
        end = title.find_first_of(" \t", begin);
        token = title.substr(begin, end - begin);
    }
    return std::string(token);
}

/// @brief Rejects unexpected fields in Bioseq OpenAPI extensions.
/// @param node Mapping to check
/// @param allowed Accepted field names.
/// @param context Description used in errors.
void validate_metadata_keys(
    const YAML::Node& node,
    std::initializer_list<std::string_view> allowed,
    std::string_view context) {
    if (!node || !node.IsMap()) {
        throw std::runtime_error(std::string(context) + " must be a mapping");
    }
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        if (std::ranges::none_of(allowed, [&key](std::string_view candidate) {
                return candidate == key;
            })) {
            throw std::runtime_error(
                "unknown " + std::string(context) + " key: " + key);
        }
    }
}

/// @brief Reads an integer within the allowed range.
/// @param node Scalar YAML value.
/// @param minimum Inclusive minimum.
/// @param maximum Inclusive maximum.
/// @param context Description used  in errors.
/// @return Parsed integer.
[[nodiscard]] std::int64_t parse_metadata_integer(
    const YAML::Node& node,
    std::int64_t minimum,
    std::int64_t maximum,
    std::string_view context) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error(std::string(context) + " must be an integer scalar");
    }
    const std::string spelling = node.Scalar();
    std::int64_t value{};
    const auto parsed = std::from_chars(
        spelling.data(), spelling.data() + spelling.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != spelling.data() + spelling.size()
        || value < minimum || value > maximum) {
        throw std::runtime_error(
            std::string(context) + " is outside its supported integer range");
    }
    return value;
}

/// @brief Reads a test parameter value using the operation's declared type.
/// @param node Fixture value node.
/// @param kind Expected scalar type.
/// @return Value stored in the matching integration_scalar alternhtive.
[[nodiscard]] integration_scalar parse_integration_scalar(
    const YAML::Node& node,
    parameter_kind kind) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("integration parameter value must be a scalar");
    }
    const std::string spelling = node.Scalar();
    switch (kind) {
    case parameter_kind::string:
        return spelling;
    case parameter_kind::integer:
        return parse_metadata_integer(
            node,
            std::numeric_limits<std::int64_t>::min(),
            std::numeric_limits<std::int64_t>::max(),
            "integration parameter value");
    case parameter_kind::number: {
        double value{};
        std::istringstream parser{spelling};
        parser.imbue(std::locale::classic());
        parser.unsetf(std::ios::skipws);
        parser >> value;
        if (spelling.starts_with('+') || !parser || !parser.eof()
            || !std::isfinite(value)) {
            throw std::runtime_error("integration number value is invalid");
        }
        return value;
    }
    case parameter_kind::boolean:
        if (spelling == "true") return true;
        if (spelling == "false") return false;
        throw std::runtime_error("integration boolean value must be true or false");
    }
    throw std::runtime_error("invalid integration parameter kind");
}

/// @brief Reads and validates the x-bioseq-integration test cases.
/// @param document Root OpenAPI node.
/// @param result API description with its operations already parsed.
/// @details Each case must name an existing operation and supply its required
/// parameters. Expected status, body text and header values become test checks.
void parse_integration_cases(const YAML::Node& document, openapi_document& result) {
    const YAML::Node cases = document["x-bioseq-integration"];
    if (!cases || !cases.IsSequence() || cases.size() == 0) {
        throw std::runtime_error(
            "OpenAPI document requires a non-empty x-bioseq-integration sequence");
    }

    std::map<std::string, bool> case_names;
    for (const auto& node : cases) {
        validate_metadata_keys(
            node,
            {"name", "operationId", "accept", "mode", "count",
             "preDelayWithoutCredentialsMs", "parameters", "expect"},
            "integration case");
        if (!node["name"] || !node["operationId"] || !node["expect"]) {
            throw std::runtime_error(
                "integration case requires name, operationId, and expect");
        }

        integration_case parsed;
        parsed.name = node["name"].as<std::string>();
        parsed.operation_id = node["operationId"].as<std::string>();
        if (parsed.name.empty() || !case_names.emplace(parsed.name, true).second) {
            throw std::runtime_error("integration case names must be non-empty and unique");
        }
        const auto operation = std::ranges::find(
            result.get_operations, parsed.operation_id, &openapi_operation::operation_id);
        if (operation == result.get_operations.end()) {
            throw std::runtime_error(
                "integration case references unknown GET operation: " + parsed.operation_id);
        }
        if (node["accept"]) {
            if (!node["accept"].IsScalar()) {
                throw std::runtime_error("integration Accept override must be a scalar");
            }
            parsed.accept = node["accept"].as<std::string>();
            if (parsed.accept.empty()
                || !http_validation::is_valid_header_value(parsed.accept)) {
                throw std::runtime_error("integration Accept override is invalid");
            }
        }

        const std::string mode = node["mode"]
            ? node["mode"].as<std::string>() : std::string{"single"};
        if (mode == "single") {
            parsed.mode = integration_mode::single;
            parsed.count = node["count"]
                ? static_cast<std::size_t>(parse_metadata_integer(
                      node["count"], 1, 1, "single integration count"))
                : 1;
        } else if (mode == "batch") {
            parsed.mode = integration_mode::batch;
            if (!node["count"]) {
                throw std::runtime_error("batch integration case requires count");
            }
            parsed.count = static_cast<std::size_t>(parse_metadata_integer(
                node["count"], 2, 32, "batch integration count"));
        } else {
            throw std::runtime_error("integration mode must be single or batch");
        }
        if (node["preDelayWithoutCredentialsMs"]) {
            parsed.pre_delay_without_credentials_ms = static_cast<std::size_t>(
                parse_metadata_integer(
                    node["preDelayWithoutCredentialsMs"], 0, 60'000,
                    "integration pre-delay"));
        }

        const YAML::Node parameters = node["parameters"];
        if (parameters && !parameters.IsSequence()) {
            throw std::runtime_error("integration parameters must be a sequence");
        }
        if (parameters) {
            for (const auto& parameter_node : parameters) {
                validate_metadata_keys(
                    parameter_node, {"name", "in", "value"},
                    "integration parameter");
                if (!parameter_node["name"] || !parameter_node["in"]
                    || !parameter_node["value"]) {
                    throw std::runtime_error(
                        "integration parameter requires name, in, and value");
                }
                const std::string name = parameter_node["name"].as<std::string>();
                const auto location = parse_location(
                    parameter_node["in"].as<std::string>());
                const auto descriptor = std::ranges::find_if(
                    operation->parameters,
                    [&name, location](const openapi_parameter& candidate) {
                        return candidate.name == name && candidate.location == location;
                    });
                if (descriptor == operation->parameters.end()) {
                    throw std::runtime_error(
                        "unknown integration parameter for " + parsed.operation_id
                        + ": " + name);
                }
                const auto duplicate_count = std::ranges::count_if(
                    parsed.parameters,
                    [&name, location](const integration_parameter& existing) {
                        return existing.name == name && existing.location == location;
                    });
                if (!descriptor->is_array && duplicate_count != 0) {
                    throw std::runtime_error(
                        "scalar integration parameter is repeated: " + name);
                }
                if (location == parameter_location::path
                    && operation->path.find("{" + name + "}") == std::string::npos) {
                    throw std::runtime_error(
                        "integration path parameter has no path marker: " + name);
                }
                parsed.parameters.push_back(integration_parameter{
                    .name = name,
                    .location = location,
                    .kind = descriptor->kind,
                    .value = parse_integration_scalar(
                        parameter_node["value"], descriptor->kind),
                });
            }
        }
        for (const auto& descriptor : operation->parameters) {
            if (!descriptor.required) continue;
            const bool supplied = std::ranges::any_of(
                parsed.parameters,
                [&descriptor](const integration_parameter& parameter) {
                    return parameter.name == descriptor.name
                        && parameter.location == descriptor.location;
                });
            if (!supplied) {
                throw std::runtime_error(
                    "integration case omits required parameter: " + descriptor.name);
            }
        }

        const YAML::Node expectation = node["expect"];
        validate_metadata_keys(
            expectation, {"status", "bodyContains", "headerContains"},
            "integration expectation");
        if (expectation["status"]) {
            parsed.expected_status = static_cast<long>(parse_metadata_integer(
                expectation["status"], 100, 599, "integration expected status"));
        }
        const YAML::Node markers = expectation["bodyContains"];
        if (!markers || !markers.IsSequence() || markers.size() == 0) {
            throw std::runtime_error(
                "integration expectation requires non-empty bodyContains markers");
        }
        for (const auto& marker : markers) {
            const std::string value = marker.as<std::string>();
            if (value.empty()) {
                throw std::runtime_error("integration body marker cannot be empty");
            }
            parsed.body_contains.push_back(value);
        }
        const YAML::Node header_markers = expectation["headerContains"];
        if (header_markers && !header_markers.IsSequence()) {
            throw std::runtime_error(
                "integration headerContains must be a sequence");
        }
        if (header_markers) {
            for (const auto& header_marker : header_markers) {
                validate_metadata_keys(
                    header_marker, {"name", "valueContains"},
                    "integration header expectation");
                if (!header_marker["name"] || !header_marker["valueContains"]
                    || !header_marker["name"].IsScalar()
                    || !header_marker["valueContains"].IsScalar()) {
                    throw std::runtime_error(
                        "integration header expectation requires name and valueContains");
                }
                integration_header_expectation parsed_header{
                    .name = header_marker["name"].as<std::string>(),
                    .value_contains = header_marker["valueContains"].as<std::string>(),
                };
                if (!http_validation::is_valid_header_name(parsed_header.name)
                    || parsed_header.value_contains.empty()
                    || !http_validation::is_valid_header_value(
                        parsed_header.value_contains)) {
                    throw std::runtime_error(
                        "integration header expectation is invalid");
                }
                parsed.header_contains.push_back(std::move(parsed_header));
            }
        }
        result.integration_cases.push_back(std::move(parsed));
    }
}

struct parsed_https_server {
    std::string host;
    std::uint16_t port{443};
    std::string base_path;
};

/// @brief Builds the readable part of an identifier for a GET without operationId.
/// @param path OpenAPI path template.
/// @return Lowercase name starting with get_, with punctuation replaced by separators.
[[nodiscard]] std::string synthesized_get_operation_id(std::string_view path) {
    std::string result{"get"};
    bool needs_separator = true;
    for (char character : path) {
        const bool alphanumeric = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
        if (!alphanumeric) {
            needs_separator = true;
            continue;
        }
        if (needs_separator) result.push_back('_');
        result.push_back(character >= 'A' && character <= 'Z'
            ? static_cast<char>(character - 'A' + 'a') : character);
        needs_separator = false;
    }
    if (result == "get") result += "_root";
    return result;
}

/// @brief Encodes the path as  a suffix so synthesized operation names stay unique.
/// @param path Exact OpenAPI path.
/// @return `_path_` followed by the path bytes in lowercase hexadecimal.
[[nodiscard]] std::string synthesized_path_suffix(std::string_view path) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result{"_path_"};
    result.reserve(result.size() + path.size() * 2);
    for (const unsigned char byte : path) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 0x0f]);
    }
    return result;
}

/// @brief Reads a fixed HTTPS server URL supported by the transport.
/// @param server_url OpenAPI server URL.
/// @return Validated DNS host, port and path prefix.
/// @details User information, IP literals, query strings and template variables
/// are unsupported. The path prefix is stored without trailing slashes.
[[nodiscard]] parsed_https_server parse_https_server(std::string_view server_url) {
    constexpr std::string_view scheme = "https://";
    if (!server_url.starts_with(scheme)) {
        throw std::runtime_error("only verified HTTPS OpenAPI servers are supported");
    }
    const std::string_view remainder = server_url.substr(scheme.size());
    const auto delimiter = remainder.find_first_of("/?#");
    const std::string_view authority = remainder.substr(0, delimiter);
    if (authority.empty()) {
        throw std::runtime_error("OpenAPI HTTPS server authority is empty");
    }
    if (authority.contains('@')) {
        throw std::runtime_error("OpenAPI HTTPS server userinfo is not supported");
    }
    if (authority.contains('{') || authority.contains('}')) {
        throw std::runtime_error("OpenAPI HTTPS server variables are not supported");
    }
    if (authority.front() == '[' || authority.contains(']')) {
        throw std::runtime_error("OpenAPI HTTPS IPv6 literals are not supported");
    }

    std::string_view host = authority;
    std::uint16_t port = 443;
    const auto port_separator = authority.rfind(':');
    if (port_separator != std::string_view::npos) {
        if (authority.find(':') != port_separator) {
            throw std::runtime_error("OpenAPI HTTPS IPv6 literals are not supported");
        }
        host = authority.substr(0, port_separator);
        const std::string_view spelling = authority.substr(port_separator + 1);
        unsigned int parsed_port{};
        const auto parsed = std::from_chars(
            spelling.data(), spelling.data() + spelling.size(), parsed_port);
        if (spelling.empty() || parsed.ec != std::errc{}
            || parsed.ptr != spelling.data() + spelling.size()
            || parsed_port == 0 || parsed_port > 65'535) {
            throw std::runtime_error("OpenAPI HTTPS server port is invalid");
        }
        port = static_cast<std::uint16_t>(parsed_port);
    }
    if (!host.empty() && host.back() == '.') host.remove_suffix(1);
    if (!http_validation::is_valid_dns_host(host)) {
        throw std::runtime_error(
            "OpenAPI HTTPS server DNS host is invalid or an unsupported IP literal");
    }

    parsed_https_server result{
        .host = std::string(host),
        .port = port,
    };
    if (delimiter == std::string_view::npos) return result;
    if (remainder[delimiter] != '/') {
        throw std::runtime_error("OpenAPI HTTPS server query/fragment is not supported");
    }
    result.base_path = std::string(remainder.substr(delimiter));
    if (result.base_path.find_first_of("?#{}") != std::string::npos
        || std::ranges::any_of(
            result.base_path, http_validation::is_invalid_request_target_byte)) {
        throw std::runtime_error("OpenAPI HTTPS server base path is invalid");
    }
    while (!result.base_path.empty() && result.base_path.back() == '/') {
        result.base_path.pop_back();
    }
    return result;
}

/// @brief Reads the supported OpenAPI fields and the generated client's test cases.
/// @param yaml Embedded OpenAPI YAML.
/// @return Server settings, GET operations and integration tests.
/// @details Operations are sorted by identifier. Missing identifiers are derived
/// from, the exact request path, and  conflicting identifiers are rejected.
[[nodiscard]] openapi_document parse_openapi(std::string_view yaml) {
    const YAML::Node document = YAML::Load(std::string(yaml));
    if (!document["openapi"] || !document["info"] || !document["paths"]
        || !document["servers"] || document["servers"].size() == 0) {
        throw std::runtime_error("OpenAPI document lacks openapi/info/servers/paths");
    }
    openapi_document result;
    result.title = document["info"]["title"].as<std::string>();
    result.provider_name = document["x-bioseq-client-name"]
        ? document["x-bioseq-client-name"].as<std::string>()
        : provider_name_from_title(result.title);
    if (result.provider_name.empty()) {
        throw std::runtime_error("x-bioseq-client-name cannot be empty");
    }
    result.server_url = document["servers"][0]["url"].as<std::string>();
    auto server = parse_https_server(result.server_url);
    result.host = std::move(server.host);
    result.port = server.port;
    result.base_path = std::move(server.base_path);

    std::map<std::string, bool> explicit_operation_ids;
    std::vector<std::pair<std::string, std::string>> synthesized_operations;
    for (const auto& path_entry : document["paths"]) {
        const YAML::Node operation = path_entry.second["get"];
        if (!operation) continue;
        if (operation["operationId"]) {
            const std::string operation_id = operation["operationId"].as<std::string>();
            if (operation_id.empty()) {
                throw std::runtime_error(
                    "GET operation has an empty operationId at "
                    + path_entry.first.as<std::string>());
            }
            if (!explicit_operation_ids.emplace(operation_id, true).second) {
                throw std::runtime_error("duplicate OpenAPI operationId: " + operation_id);
            }
        } else {
            const std::string path = path_entry.first.as<std::string>();
            const std::string base = synthesized_get_operation_id(path);
            synthesized_operations.emplace_back(path, base);
        }
    }

    std::ranges::sort(synthesized_operations, [](const auto& left, const auto& right) {
        return left.first < right.first;
    });
    std::map<std::string, std::string> synthesized_ids_by_path;
    std::map<std::string, bool> reserved_operation_ids = explicit_operation_ids;
    for (const auto& [path, base] : synthesized_operations) {
        std::string operation_id = base + synthesized_path_suffix(path);
        if (reserved_operation_ids.contains(operation_id)) {
            throw std::runtime_error(
                "duplicate explicit/synthesized OpenAPI operationId: " + operation_id);
        }
        reserved_operation_ids.emplace(operation_id, true);
        synthesized_ids_by_path.emplace(path, std::move(operation_id));
    }

    std::map<std::string, bool> operation_ids;
    for (const auto& path_entry : document["paths"]) {
        const std::string path = path_entry.first.as<std::string>();
        if (path.empty() || path.front() != '/') {
            throw std::runtime_error("OpenAPI path keys must begin with '/': " + path);
        }
        if (path.find_first_of("?#") != std::string::npos
            || std::ranges::any_of(
                path, http_validation::is_invalid_request_target_byte)) {
            throw std::runtime_error("OpenAPI path key is not a valid request path: " + path);
        }
        const YAML::Node path_item = path_entry.second;
        if (!path_item["get"]) continue;
        const YAML::Node operation = path_item["get"];
        openapi_operation parsed;
        if (operation["operationId"]) {
            parsed.operation_id = operation["operationId"].as<std::string>();
        } else {
            parsed.operation_id = synthesized_ids_by_path.at(path);
        }
        parsed.path = path;
        parsed.accept = preferred_response_media_type(document, operation);
        if (auto api_key = effective_api_key_requirement(document, operation)) {
            parsed.api_key_name = std::move(api_key->name);
            parsed.api_key_location = api_key->location;
        }
        append_parameters(document, path_item["parameters"], parsed.parameters);
        append_parameters(document, operation["parameters"], parsed.parameters);
        if (!operation_ids.emplace(parsed.operation_id, true).second) {
            throw std::runtime_error("duplicate OpenAPI operationId: " + parsed.operation_id);
        }
        result.get_operations.push_back(std::move(parsed));
    }
    std::ranges::sort(result.get_operations, {}, &openapi_operation::operation_id);
    if (result.get_operations.empty()) {
        throw std::runtime_error("OpenAPI document contains no GET operations");
    }
    parse_integration_cases(document, result);
    return result;
}

}
