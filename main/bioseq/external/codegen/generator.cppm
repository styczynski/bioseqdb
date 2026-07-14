module;

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <print>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <cppast/cpp_attribute.hpp>
#include <cppast/cpp_class.hpp>
#include <cppast/cpp_entity_index.hpp>
#include <cppast/cpp_entity_kind.hpp>
#include <cppast/cpp_expression.hpp>
#include <cppast/cpp_file.hpp>
#include <cppast/cpp_variable.hpp>
#include <cppast/libclang_parser.hpp>
#include <cppast/visitor.hpp>
#include <yaml-cpp/yaml.h>

export module bioseq.external.codegen;

import bioseq.common.hashing;
import bioseq.external.http_validation;

namespace bioseq::external::codegen {

namespace fs = std::filesystem;
namespace http_validation = bioseq::external::http_validation;

enum class parameter_location : std::uint8_t { path, query, header };
enum class parameter_kind : std::uint8_t { string, integer, number, boolean };

struct openapi_parameter {
    std::string name;
    parameter_location location{};
    parameter_kind kind{};
    bool required{};
    bool is_array{};
};

struct openapi_operation {
    std::string operation_id;
    std::string path;
    std::string accept;
    std::string api_key_name;
    std::optional<parameter_location> api_key_location;
    std::vector<openapi_parameter> parameters;
};

enum class integration_mode : std::uint8_t { single, batch };

using integration_scalar = std::variant<std::string, std::int64_t, double, bool>;

struct integration_parameter {
    std::string name;
    parameter_location location{};
    parameter_kind kind{};
    integration_scalar value;
};

struct integration_header_expectation {
    std::string name;
    std::string value_contains;
};

struct integration_case {
    std::string name;
    std::string operation_id;
    std::string accept;
    integration_mode mode{};
    std::size_t count{1};
    std::size_t pre_delay_without_credentials_ms{};
    long expected_status{200};
    std::vector<integration_parameter> parameters;
    std::vector<std::string> body_contains;
    std::vector<integration_header_expectation> header_contains;
};

struct openapi_document {
    std::string title;
    std::string provider_name;
    std::string server_url;
    std::string host;
    std::uint16_t port{443};
    std::string base_path;
    std::vector<openapi_operation> get_operations;
    std::vector<integration_case> integration_cases;
};

struct annotation_request {
    fs::path source;
    fs::path shadow_source;
    std::string annotation_name;
    std::string declared_type;
    std::string argument_expression;
    std::string spec;
};

struct generated_api {
    std::string client_name;
    std::string declared_type;
    openapi_document document;
};

struct generated_output {
    std::string name;
    std::function<void(const fs::path&, std::string_view)> emit;
};

struct annotation_handler {
    std::function<generated_output(const annotation_request&)> generate;
    std::function<std::string(const fs::path&)> output_name;
    std::function<fs::path(std::string_view)> owned_output_path;
    std::function<bool(const fs::path&, std::string_view)> outputs_exist;
    std::function<void(std::ostringstream&, std::string_view)> append_cmake_inventory;
};

using annotation_handlers = std::unordered_map<std::string, annotation_handler>;

struct command_line {
    fs::path generator_binary;
    fs::path source_root;
    fs::path output_root;
    fs::path stamp;
    std::string clang_binary;
    std::string compiler_id;
};

/// @brief Reads an entire file as binary text.
/// @param path File to read.
/// @return Exact file contents.
[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read " + path.string());
    }
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    if (length < 0) {
        throw std::runtime_error("cannot size " + path.string());
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    input.seekg(0, std::ios::beg);
    input.read(result.data(), static_cast<std::streamsize>(result.size()));
    if (!input && !input.eof()) {
        throw std::runtime_error("cannot finish reading " + path.string());
    }
    return result;
}

/// @brief Atomically publishes content only when the destination differs.
/// @param path Destination artifact.
/// @param content Desired bytes.
void write_if_different(const fs::path& path, std::string_view content) {
    std::error_code error;
    if (fs::exists(path, error) && !error) {
        const auto size = fs::file_size(path, error);
        if (!error && size == content.size() && read_file(path) == content) {
            return;
        }
    }
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write " + temporary.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("cannot finish writing " + temporary.string());
        }
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(path, error);
        error.clear();
        fs::rename(temporary, path, error);
    }
    if (error) {
        throw std::runtime_error("cannot publish " + path.string() + ": " + error.message());
    }
}

/// @brief Creates or retimestamps the build completion stamp.
/// @param path Stamp path.
void touch_stamp(const fs::path& path) {
    fs::create_directories(path.parent_path());
    if (!fs::exists(path)) {
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("cannot create stamp " + path.string());
        }
    }
    fs::last_write_time(path, fs::file_time_type::clock::now());
}

/// @brief Removes surrounding ASCII whitespace.
/// @param value Text to trim.
/// @return Trimmed copy.
[[nodiscard]] std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

/// @brief Locates a Bioseq attribute while allowing whitespace after `[[`.
/// @param line Source line to inspect.
/// @return Attribute offset or `std::string_view::npos`.
[[nodiscard]] std::size_t find_bioseq_attribute_begin(std::string_view line) {
    std::size_t attribute_begin = line.find("[[");
    while (attribute_begin != std::string_view::npos) {
        std::size_t name_begin = attribute_begin + 2;
        while (name_begin < line.size()
               && (line[name_begin] == ' ' || line[name_begin] == '\t')) {
            ++name_begin;
        }
        if (line.substr(name_begin).starts_with("bioseq::")) {
            return attribute_begin;
        }
        attribute_begin = line.find("[[", attribute_begin + 2);
    }
    return std::string_view::npos;
}

/// @brief Rewrites a module descriptor into a libclang-parseable shadow TU.
/// @param source Original module source.
/// @return Normalized translation-unit text.
[[nodiscard]] std::string normalize_shadow(std::string_view source) {
    std::istringstream lines{std::string(source)};
    std::ostringstream result;
    std::string line;
    std::optional<std::string> raw_terminator;
    while (std::getline(lines, line)) {
        if (raw_terminator) {
            result << line << '\n';
            if (line.find(*raw_terminator) != std::string::npos) {
                raw_terminator.reset();
            }
            continue;
        }
        const auto raw_prefix = line.find("R\"");
        const auto delimiter_end = raw_prefix == std::string::npos
            ? std::string::npos : line.find('(', raw_prefix + 2);
        if (delimiter_end != std::string::npos) {
            const std::string delimiter = line.substr(
                raw_prefix + 2, delimiter_end - raw_prefix - 2);
            const std::string terminator = ")" + delimiter + "\"";
            const bool closes_on_line =
                line.find(terminator, delimiter_end + 1) != std::string::npos;
            const auto constexpr_position = line.find("constexpr");
            if (constexpr_position != std::string::npos
                && constexpr_position < raw_prefix) {
                line.replace(constexpr_position, std::string_view{"constexpr"}.size(), "const");
            }
            result << line << '\n';
            if (!closes_on_line) {
                raw_terminator = terminator;
            }
            continue;
        }
        const std::string stripped = trim(line);
        if (stripped == "module;" || stripped.starts_with("export module ")
            || stripped.starts_with("import ") || stripped.starts_with("export import ")) {
            continue;
        }
        const auto attribute_begin = find_bioseq_attribute_begin(line);
        if (attribute_begin != std::string::npos) {
            std::size_t position = 0;
            while ((position = line.find("export struct", position)) != std::string::npos) {
                line.erase(position, std::string_view{"export "}.size());
                position += std::string_view{"struct"}.size();
            }
            const auto attribute_end = line.find("]]", attribute_begin);
            auto struct_position = attribute_end == std::string::npos
                ? std::string::npos : line.find("struct", attribute_end + 2);
            if (attribute_end != std::string::npos && struct_position != std::string::npos
                && attribute_begin < struct_position) {
                const std::string attribute = line.substr(
                    attribute_begin, attribute_end - attribute_begin + 2);
                std::size_t erase_end = attribute_end + 2;
                while (erase_end < line.size()
                       && (line[erase_end] == ' ' || line[erase_end] == '\t')) {
                    ++erase_end;
                }
                line.erase(attribute_begin, erase_end - attribute_begin);
                struct_position = line.find("struct", attribute_begin);
                line.insert(
                    struct_position + std::string_view{"struct"}.size(), " " + attribute);
            }
        }
        result << line << '\n';
    }
    return result.str();
}

/// @brief Extracts source text from a supported cppast expression.
/// @param expression Parsed initializer expression.
/// @return Expression spelling.
[[nodiscard]] std::string expression_text(const cppast::cpp_expression& expression) {
    if (expression.kind() == cppast::cpp_expression_kind::literal_t) {
        return static_cast<const cppast::cpp_literal_expression&>(expression).value();
    }
    if (expression.kind() == cppast::cpp_expression_kind::unexposed_t) {
        return static_cast<const cppast::cpp_unexposed_expression&>(expression)
            .expression().as_string();
    }
    throw std::runtime_error("unsupported cppast expression kind");
}

/// @brief Decodes the payload of a C++ raw string initializer.
/// @param expression Raw string expression spelling.
/// @return Text between the raw delimiters.
[[nodiscard]] std::string decode_raw_string(std::string_view expression) {
    const auto prefix = expression.find("R\"");
    if (prefix == std::string_view::npos) {
        throw std::runtime_error("OpenAPI spec initializer is not a raw string literal");
    }
    const auto delimiter_end = expression.find('(', prefix + 2);
    if (delimiter_end == std::string_view::npos) {
        throw std::runtime_error("malformed raw string delimiter");
    }
    const std::string delimiter(expression.substr(prefix + 2, delimiter_end - prefix - 2));
    const std::string terminator = ")" + delimiter + "\"";
    const auto content_end = expression.rfind(terminator);
    if (content_end == std::string_view::npos || content_end < delimiter_end) {
        throw std::runtime_error("unterminated OpenAPI raw string literal");
    }
    return std::string(expression.substr(delimiter_end + 1, content_end - delimiter_end - 1));
}

/// @brief Uses cppast/libclang to resolve one registered annotation and its spec.
/// @param source Original descriptor path.
/// @param shadow Normalized parser input path.
/// @param clang_binary Clang binary used by cppast.
/// @param handlers Registered annotation contracts.
/// @return Resolved annotation request.
[[nodiscard]] annotation_request parse_annotation(
    const fs::path& source,
    const fs::path& shadow,
    const std::string& clang_binary,
    const annotation_handlers& handlers) {
    cppast::cpp_entity_index index;
    cppast::libclang_compile_config config(clang_binary);
    config.set_flags(cppast::cpp_standard::cpp_20);
    config.fast_preprocessing(false);
    cppast::libclang_parser parser;
    auto file = parser.parse(index, shadow.string(), config);
    if (!file || parser.error()) {
        throw std::runtime_error("cppast/libclang failed to parse " + source.string());
    }

    std::unordered_map<std::string, std::string> initializers;
    struct annotated_class {
        std::string annotation_name;
        std::string declared_type;
        std::string argument;
    };
    std::vector<annotated_class> annotated_classes;
    cppast::visit(*file, [&initializers, &annotated_classes, &handlers](
                             const cppast::cpp_entity& entity,
                             cppast::visitor_info info) {
        if (!info.is_new_entity()) {
            return;
        }
        if (entity.kind() == cppast::cpp_entity_kind::variable_t) {
            const auto& variable = static_cast<const cppast::cpp_variable&>(entity);
            if (const auto value = variable.default_value()) {
                initializers.emplace(variable.name(), expression_text(value.value()));
            }
        }
        if (entity.kind() == cppast::cpp_entity_kind::class_t) {
            for (const auto& [annotation_name, handler] : handlers) {
                (void)handler;
                if (const auto attribute = cppast::has_attribute(entity, annotation_name)) {
                    if (!attribute.value().arguments()) {
                        throw std::runtime_error(annotation_name + " requires one argument");
                    }
                    annotated_classes.push_back(annotated_class{
                        .annotation_name = annotation_name,
                        .declared_type = entity.name(),
                        .argument = trim(attribute.value().arguments().value().as_string()),
                    });
                }
            }
        }
    });

    if (annotated_classes.size() != 1) {
        throw std::runtime_error(
            source.string() + " must contain exactly one registered Bioseq annotation");
    }
    const auto& matched = annotated_classes.front();
    const auto initializer = initializers.find(matched.argument);
    if (initializer == initializers.end()) {
        throw std::runtime_error(
            "cannot resolve annotation argument '" + matched.argument + "' in " + source.string());
    }
    return annotation_request{
        .source = source,
        .shadow_source = shadow,
        .annotation_name = matched.annotation_name,
        .declared_type = matched.declared_type,
        .argument_expression = matched.argument,
        .spec = decode_raw_string(initializer->second),
    };
}

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

/// @brief Maps an OpenAPI parameter location into generator metadata.
/// @param value OpenAPI `in` value.
/// @return Supported parameter location.
[[nodiscard]] parameter_location parse_location(std::string_view value) {
    if (value == "path") return parameter_location::path;
    if (value == "query") return parameter_location::query;
    if (value == "header") return parameter_location::header;
    throw std::runtime_error("unsupported OpenAPI parameter location: " + std::string(value));
}

/// @brief Maps an OpenAPI scalar type into generator metadata.
/// @param value OpenAPI schema type.
/// @return Supported scalar kind.
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

/// @brief Resolves and lowers one OpenAPI parameter definition.
/// @param document Root OpenAPI node.
/// @param unresolved Parameter node or reference.
/// @return Normalized parameter metadata.
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
        && !http_validation::valid_header_name(name)) {
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

/// @brief Merges path- or operation-level parameters by location and name.
/// @param document Root OpenAPI node.
/// @param list Parameter sequence to merge.
/// @param destination Accumulated normalized parameters.
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

/// @brief Selects the preferred successful response media type.
/// @param document Root OpenAPI node.
/// @param operation Operation containing response definitions.
/// @return Preferred Accept value, falling back to `*/*`.
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
                // A charset parameter has no semantics for application/json and EBI
                // rejects the spelling it publishes. Preserve all other parameters,
                // including versioned vendor media types.
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

/// @brief Resolves the effective supported API-key security requirement.
/// @param document Root OpenAPI node.
/// @param operation Operation whose security overrides document security.
/// @return Header/query API-key metadata when one applies.
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
                && !http_validation::valid_header_name(candidate.name))) {
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

/// @brief Extracts a stable provider label from an OpenAPI title.
/// @param title Human-readable `info.title` value.
/// @return First significant title token, skipping a leading English article.
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

/// @brief Rejects unknown keys in generator-owned OpenAPI extension metadata.
/// @param node Mapping to validate.
/// @param allowed Accepted key spellings.
/// @param context Diagnostic context.
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

/// @brief Parses a bounded integer from scalar extension metadata.
/// @param node Scalar YAML node.
/// @param minimum Inclusive minimum.
/// @param maximum Inclusive maximum.
/// @param context Diagnostic context.
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

/// @brief Parses one fixture value according to normalized OpenAPI schema kind.
/// @param node Fixture `value` node.
/// @param kind Expected OpenAPI scalar kind.
/// @return Strongly typed fixture scalar.
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
        const auto parsed = std::from_chars(
            spelling.data(), spelling.data() + spelling.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != spelling.data() + spelling.size()
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

/// @brief Parses and validates ordered live-test cases from an OpenAPI extension.
/// @param document Root OpenAPI node.
/// @param result Normalized document whose operations have already been parsed.
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
                || !http_validation::valid_header_value(parsed.accept)) {
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
                if (!http_validation::valid_header_name(parsed_header.name)
                    || parsed_header.value_contains.empty()
                    || !http_validation::valid_header_value(
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

/// @brief Derives a stable identifier for a GET operation that omits operationId.
/// @param path Validated OpenAPI path template.
/// @return Lowercase `get_<path>` identifier with punctuation collapsed.
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

/// @brief Encodes an exact path into an identifier-safe stable suffix.
/// @param path OpenAPI path that owns the synthesized identifier.
/// @return Reversible lowercase hexadecimal spelling of every path byte.
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

/// @brief Parses the fixed HTTPS server subset supported by the transport.
/// @param server_url OpenAPI server URL.
/// @return Validated host, port, and path prefix.
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
    if (!http_validation::valid_dns_host(host)) {
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
            result.base_path, http_validation::invalid_request_target_byte)) {
        throw std::runtime_error("OpenAPI HTTPS server base path is invalid");
    }
    while (!result.base_path.empty() && result.base_path.back() == '/') {
        result.base_path.pop_back();
    }
    return result;
}

/// @brief Parses and validates the OpenAPI subset consumed by the renderer.
/// @param yaml Embedded OpenAPI YAML text.
/// @return Normalized service and GET-operation metadata.
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
                path, http_validation::invalid_request_target_byte)) {
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

/// @brief Sanitizes arbitrary text into a C++ identifier.
/// @param value Text to sanitize.
/// @return Identifier-safe spelling.
[[nodiscard]] std::string identifier(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 1);
    if (value.empty() || (value.front() >= '0' && value.front() <= '9')) {
        result.push_back('_');
    }
    for (const char character : value) {
        const bool valid = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || character == '_';
        result.push_back(valid ? character : '_');
    }
    return result;
}

/// @brief Lowers an API annotation into renderer-ready metadata.
/// @param request Resolved annotation and embedded specification.
/// @return Validated generated API model.
[[nodiscard]] generated_api generate_api_client(const annotation_request& request) {
    auto document = parse_openapi(request.spec);
    const std::string client_name = request.source.stem().string();
    if (identifier(client_name) != client_name || identifier(request.declared_type) != request.declared_type) {
        throw std::runtime_error(
            "API descriptor stem and annotated class must be simple C++ identifiers: "
            + request.source.string());
    }
    return generated_api{
        .client_name = client_name,
        .declared_type = request.declared_type,
        .document = std::move(document),
    };
}

/// @brief Escapes text for a generated C++ string literal.
/// @param value Unescaped text.
/// @return C++-escaped spelling without surrounding quotes.
[[nodiscard]] std::string cpp_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result;
}

/// @brief Derives the conventional API-key environment variable name.
/// @param client_name Generated client name.
/// @return Uppercase `<CLIENT>_API_KEY` spelling.
[[nodiscard]] std::string api_key_environment_name(std::string_view client_name) {
    std::string result;
    result.reserve(client_name.size() + std::string_view{"_API_KEY"}.size());
    for (const char character : client_name) {
        if (character >= 'a' && character <= 'z') {
            result.push_back(static_cast<char>(character - 'a' + 'A'));
        } else if ((character >= 'A' && character <= 'Z')
                   || (character >= '0' && character <= '9')) {
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
    case parameter_location::path: return "parameter_location::path";
    case parameter_location::query: return "parameter_location::query";
    case parameter_location::header: return "parameter_location::header";
    }
    throw std::runtime_error("invalid parameter location");
}

/// @brief Renders a parameter-kind enum expression.
/// @param kind Normalized scalar kind.
/// @return Generated C++ enum literal.
[[nodiscard]] std::string kind_literal(parameter_kind kind) {
    switch (kind) {
    case parameter_kind::string: return "parameter_kind::string";
    case parameter_kind::integer: return "parameter_kind::integer";
    case parameter_kind::number: return "parameter_kind::number";
    case parameter_kind::boolean: return "parameter_kind::boolean";
    }
    throw std::runtime_error("invalid parameter kind");
}

/// @brief Renders a typed integration-fixture scalar as generated C++.
/// @param value Parsed fixture value.
/// @return C++ expression with an exact `parameter_scalar` alternative.
[[nodiscard]] std::string integration_scalar_literal(const integration_scalar& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return "std::string_view{\"" + cpp_escape(*text) + "\"}";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        if (*integer == std::numeric_limits<std::int64_t>::min()) {
            return "std::numeric_limits<std::int64_t>::min()";
        }
        return "std::int64_t{" + std::to_string(*integer) + "}";
    }
    if (const auto* number = std::get_if<double>(&value)) {
        std::ostringstream spelling;
        spelling << std::setprecision(std::numeric_limits<double>::max_digits10) << *number;
        return "double{" + spelling.str() + "}";
    }
    return std::get<bool>(value) ? "true" : "false";
}

/// @brief Renders the complete C++ module for one API client.
/// @param api Validated API model.
/// @return Generated module source.
[[nodiscard]] std::string render_module(const generated_api& api) {
    const std::string namespace_suffix = identifier(api.client_name);
    std::size_t total_parameter_count = 0;
    for (const auto& operation : api.document.get_operations) {
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

export module bioseq.external.api.)cpp" << namespace_suffix << R"cpp(;

import bioseq.common.context;
import bioseq.external.https;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;

namespace bioseq::external::api::)cpp" << namespace_suffix << R"cpp( {

export enum class parameter_location : std::uint8_t { path, query, header };
enum class parameter_kind : std::uint8_t { string, integer, number, boolean };

export using parameter_scalar = std::variant<std::string_view, std::int64_t, double, bool>;
export using response_header = bioseq::external::http_header;

export struct parameter_value {
    std::string_view name;
    parameter_location location{};
    parameter_scalar value;
};

export inline constexpr std::size_t default_maximum_response_body_bytes =
    64U * 1024U * 1024U;

/// @brief Non-owning GET invocation whose views must outlive synchronous get_batch().
export struct get_request {
    std::string_view operation_id;
    std::span<const parameter_value> parameters{};
    std::size_t maximum_response_body_bytes{default_maximum_response_body_bytes};
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

constexpr std::array<parameter_descriptor, )cpp" << total_parameter_count << R"cpp(> parameters{{
)cpp";
    std::size_t parameter_offset = 0;
    for (const auto& operation : api.document.get_operations) {
        for (const auto& parameter : operation.parameters) {
            out << "    parameter_descriptor{\"" << cpp_escape(parameter.name) << "\", "
                << location_literal(parameter.location) << ", " << kind_literal(parameter.kind)
                << ", " << (parameter.required ? "true" : "false") << ", "
                << (parameter.is_array ? "true" : "false") << "},\n";
        }
    }
    out << "}};\n\nconstexpr std::array<operation_descriptor, "
        << api.document.get_operations.size() << "> operations{{\n";
    for (const auto& operation : api.document.get_operations) {
        out << "    operation_descriptor{\"" << cpp_escape(operation.operation_id) << "\", \""
            << cpp_escape(operation.path) << "\", \"" << cpp_escape(operation.accept)
            << "\", \"" << cpp_escape(operation.api_key_name) << "\", "
            << location_literal(operation.api_key_location.value_or(parameter_location::header))
            << ", " << (operation.api_key_location ? "true" : "false") << ", "
            << parameter_offset << ", "
            << operation.parameters.size() << "},\n";
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

export struct )cpp" << api.declared_type << R"cpp( {
    constexpr static std::string client_name = ")cpp" << cpp_escape(api.document.provider_name)
        << R"cpp(";
    constexpr static std::size_t default_response_body_limit =
        default_maximum_response_body_bytes;

    /// @brief Binds the result arena, worker pool, and optional authentication.
    )cpp" << api.declared_type << R"cpp((
        const context::context& memory_context,
        const bioseq::parallelism::GlobalPool& pool,
        std::optional<credentials> supplied_credentials = std::nullopt)
        : memory_context_(memory_context)
        , pool_(pool)
        , credentials_(std::move(supplied_credentials))
        , transport_(")cpp" << cpp_escape(api.document.host) << "\", "
        << api.document.port << R"cpp() {}

    /// @brief Executes one generated GET operation into the result arena.
    [[nodiscard]] std::expected<response, client_error> get(
        std::string_view operation_id,
        std::span<const parameter_value> supplied = {},
        std::size_t maximum_response_body_bytes = default_response_body_limit) {
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
        std::size_t maximum_response_body_bytes = default_response_body_limit) {
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
        std::vector<std::expected<response, client_error>> result;
        result.reserve(requests.size());
        for (auto& slot : slots) {
            if (slot) {
                result.emplace_back(materialize(std::move(*slot)));
            } else {
                result.emplace_back(std::unexpected(std::move(slot.error())));
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
        std::string path = ")cpp" << cpp_escape(api.document.base_path) << R"cpp(";
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
                    joined += bioseq::external::url_encode(value);
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
                    path += bioseq::external::url_encode(descriptor.name);
                    path += '=';
                    path += bioseq::external::url_encode(value);
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
                path += bioseq::external::url_encode(operation->api_key_name);
                path += '=';
                path += bioseq::external::url_encode(credentials_->api_key);
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
    /// default_maximum_response_body_bytes = 64 MiB) through the small-object
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
    const bioseq::parallelism::GlobalPool& pool_;
    std::optional<credentials> credentials_;
    bioseq::external::https_client transport_;
};

} // namespace bioseq::external::api::)cpp" << namespace_suffix << R"cpp(
)cpp";
    return out.str();
}

/// @brief Renders a live Catch2 integration test for one client.
/// @param api Validated API model.
/// @return Generated integration-test source.
[[nodiscard]] std::string render_test(const generated_api& api) {
    const std::string namespace_suffix = identifier(api.client_name);
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
import bioseq.external.api.)cpp" << namespace_suffix << R"cpp(;
import bioseq.external.https;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;

namespace generated_api = bioseq::external::api::)cpp" << namespace_suffix << R"cpp(;

struct generated_pool_restore {
    bioseq::parallelism::GlobalPool& pool;
    bool was_active;
    bioseq::parallelism::PoolConfig previous;
    bool changed = false;

    /// @brief Restores the prior GlobalPool state without throwing.
    ~generated_pool_restore() {
        if (!changed) return;
        try {
            if (was_active) pool.restart(previous);
            else pool.shutdown();
        } catch (...) {
        }
    }
};

TEST_CASE("generated external API client performs live requests",
          "[external][)cpp" << api.client_name << R"cpp(][integration]") {
    bioseq::external::https_client validation_transport("example.org");
    REQUIRE_FALSE(validation_transport.get("/invalid\npath").has_value());
    const std::vector<bioseq::external::http_header> invalid_headers{
        {"Bad Header", "value"},
    };
    REQUIRE_FALSE(validation_transport.get("/", invalid_headers).has_value());
    REQUIRE_FALSE(validation_transport.get("/", {}, 0).has_value());

    context::context memory_context;
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    generated_pool_restore restore{
        .pool = pool,
        .was_active = pool.active(),
        .previous = bioseq::parallelism::PoolConfig{
            .max_concurrency = pool.max_concurrency(),
            .mode = pool.mode(),
        },
    };
    if (!pool.active() || pool.max_concurrency() < 2) {
        restore.changed = true;
        pool.restart(bioseq::parallelism::PoolConfig{
            .max_concurrency = 2,
            .mode = bioseq::parallelism::PipelineConcurrencyMode::TASK_PARALLEL,
        });
    }
    std::optional<generated_api::credentials> supplied_credentials;
    [[maybe_unused]] bool has_api_key = false;
    if (const char* api_key = std::getenv(")cpp" << api_key_environment_name(api.client_name)
        << R"cpp("); api_key != nullptr && *api_key != '\0') {
        supplied_credentials = generated_api::credentials{std::string(api_key)};
        has_api_key = true;
    }
    generated_api::)cpp" << api.declared_type << R"cpp( client(
        memory_context, pool, std::move(supplied_credentials));
    REQUIRE(client.client_name == ")cpp" << cpp_escape(api.document.provider_name) << R"cpp(");
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
        const auto& fixture = api.document.integration_cases[case_index];
        const std::string prefix = "fixture_" + std::to_string(case_index);
        out << "\n    INFO(\"fixture: " << cpp_escape(fixture.name) << "\");\n";
        if (fixture.pre_delay_without_credentials_ms != 0) {
            out << "    if (!has_api_key) {\n"
                << "        std::this_thread::sleep_for(std::chrono::milliseconds("
                << fixture.pre_delay_without_credentials_ms << "));\n"
                << "    }\n";
        }
        if (!fixture.parameters.empty()) {
            out << "    const std::array " << prefix << "_parameters{\n";
            for (const auto& parameter : fixture.parameters) {
                out << "        generated_api::parameter_value{\""
                    << cpp_escape(parameter.name) << "\", generated_api::"
                    << location_literal(parameter.location) << ", "
                    << integration_scalar_literal(parameter.value) << "},\n";
            }
            out << "    };\n";
        }

        const std::string parameter_expression = fixture.parameters.empty()
            ? std::string{"{}"} : prefix + "_parameters";
        const std::string accept_expression =
            "std::string_view{\"" + cpp_escape(fixture.accept) + "\"}";
        if (fixture.mode == integration_mode::single) {
            out << "    auto " << prefix << "_response = client."
                << (fixture.accept.empty() ? "get" : "get_with_accept") << "(\""
                << cpp_escape(fixture.operation_id) << "\", "
                << parameter_expression;
            if (!fixture.accept.empty()) {
                out << ", " << accept_expression;
            }
            out << ");\n"
                << "    INFO((" << prefix << "_response ? std::string("
                << prefix << "_response->body_view()) : " << prefix
                << "_response.error().message));\n"
                << "    REQUIRE(" << prefix << "_response.has_value());\n"
                << "    REQUIRE(" << prefix << "_response->status_code == "
                << fixture.expected_status << ");\n";
            for (const auto& marker : fixture.body_contains) {
                out << "    REQUIRE(" << prefix << "_response->body_view().find(\""
                    << cpp_escape(marker) << "\") != std::string_view::npos);\n";
            }
            for (const auto& header : fixture.header_contains) {
                out << "    REQUIRE(generated_api::response_header_contains("
                    << prefix << "_response->headers, \""
                    << cpp_escape(header.name) << "\", \""
                    << cpp_escape(header.value_contains) << "\"));\n";
            }
        } else {
            out << "    const std::array " << prefix << "_requests{\n";
            for (std::size_t request_index = 0;
                 request_index < fixture.count; ++request_index) {
                out << "        generated_api::get_request{\n"
                    << "            .operation_id = \""
                    << cpp_escape(fixture.operation_id) << "\",\n"
                    << "            .parameters = " << parameter_expression;
                if (!fixture.accept.empty()) {
                    out << ",\n            .accept = " << accept_expression;
                }
                out << ",\n        },\n";
            }
            out << "    };\n"
                << "    auto " << prefix << "_responses = client.get_batch("
                << prefix << "_requests);\n"
                << "    REQUIRE(" << prefix << "_responses.size() == "
                << fixture.count << ");\n"
                << "    for (const auto& item : " << prefix << "_responses) {\n"
                << "        INFO((item ? std::string(item->body_view()) : item.error().message));\n"
                << "        REQUIRE(item.has_value());\n"
                << "        REQUIRE(item->status_code == " << fixture.expected_status << ");\n";
            for (const auto& marker : fixture.body_contains) {
                out << "        REQUIRE(item->body_view().find(\""
                    << cpp_escape(marker) << "\") != std::string_view::npos);\n";
            }
            for (const auto& header : fixture.header_contains) {
                out << "        REQUIRE(generated_api::response_header_contains("
                    << "item->headers, \"" << cpp_escape(header.name)
                    << "\", \"" << cpp_escape(header.value_contains)
                    << "\"));\n";
            }
            out << "    }\n";
        }
    }
    out << R"cpp(
}
)cpp";
    return out.str();
}

/// @brief Exercises pure parser/renderer contracts without network or project sources.
void run_generator_contract_self_tests() {
    const auto require = [](bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error("generator self-test failed: " + std::string(message));
        }
    };
    const auto require_failure = [](auto operation, std::string_view expected) {
        try {
            operation();
        } catch (const std::exception& error) {
            if (std::string_view{error.what()}.contains(expected)) return;
            throw std::runtime_error(
                "generator self-test received unexpected diagnostic: "
                + std::string(error.what()));
        }
        throw std::runtime_error(
            "generator self-test expected failure containing: " + std::string(expected));
    };

    const auto server = parse_https_server("https://api.example.org:8443/v1/");
    require(server.host == "api.example.org", "explicit-port host parsing");
    require(server.port == 8443, "explicit-port value parsing");
    require(server.base_path == "/v1", "server path normalization");
    require(
        synthesized_get_operation_id("/uniprot/{identifier}.json")
            == "get_uniprot_identifier_json",
        "missing operationId synthesis");
    require(
        synthesized_get_operation_id("/uniprot/{identifiers}.json")
            == "get_uniprot_identifiers_json",
        "synthesized operationId uniqueness");
    const auto collision_contract = parse_openapi(R"yaml(
openapi: 3.0.0
info: {title: Collision API}
servers: [{url: 'https://api.example.org'}]
paths:
  /a-b:
    get: {responses: {'200': {}}}
  /a_b:
    get: {responses: {'200': {}}}
x-bioseq-client-name: Collision
x-bioseq-integration:
  - name: collision-suffix
    operationId: get_a_b_path_2f612d62
    expect: {status: 200, bodyContains: [ok]}
)yaml");
    require(
        collision_contract.get_operations.size() == 2
            && collision_contract.get_operations[0].operation_id
                != collision_contract.get_operations[1].operation_id
            && collision_contract.get_operations[0].operation_id.contains("_path_")
            && collision_contract.get_operations[1].operation_id.contains("_path_"),
        "collision-free synthesized operationId suffixes");
    const auto stable_collision_id = std::ranges::find_if(
        collision_contract.get_operations,
        [](const openapi_operation& operation) {
            return operation.path == std::string_view{"/a-b"};
        });
    require(
        stable_collision_id != collision_contract.get_operations.end()
            && stable_collision_id->operation_id
                == synthesized_get_operation_id("/a-b")
                    + synthesized_path_suffix("/a-b"),
        "synthesized operationId depends only on its exact path");
    const auto reordered_collision_contract = parse_openapi(R"yaml(
openapi: 3.0.0
info: {title: Collision API}
servers: [{url: 'https://api.example.org'}]
paths:
  /a_b:
    get: {responses: {'200': {}}}
  /a-b:
    get: {responses: {'200': {}}}
x-bioseq-client-name: Collision
x-bioseq-integration:
  - name: collision-suffix
    operationId: get_a_b_path_2f612d62
    expect: {status: 200, bodyContains: [ok]}
)yaml");
    require(
        reordered_collision_contract.get_operations.size()
                == collision_contract.get_operations.size()
            && reordered_collision_contract.get_operations[0].operation_id
                == collision_contract.get_operations[0].operation_id
            && reordered_collision_contract.get_operations[1].operation_id
                == collision_contract.get_operations[1].operation_id,
        "synthesized operationIds are independent of YAML path order");
    require_failure(
        [] { (void)parse_https_server("https://user@example.org/v1"); },
        "userinfo");
    require_failure(
        [] { (void)parse_https_server("https://a..example.org/v1"); },
        "DNS host");
    require_failure(
        [] { (void)parse_https_server("https://example.org/v1\nnext"); },
        "base path");

    constexpr std::string_view valid_openapi = R"yaml(
openapi: 3.0.0
info: {title: Contract API}
servers: [{url: 'https://api.example.org:8443/v1/'}]
security:
  - ApiKey: []
paths:
  /items/{id}:
    get:
      operationId: fetchItem
      parameters:
        - {name: id, in: path, required: true, schema: {type: integer}}
        - name: tag
          in: query
          schema: {type: array, items: {type: string}}
        - {name: enabled, in: query, schema: {type: boolean}}
        - {name: ratio, in: query, schema: {type: number}}
        - {name: Accept, in: header, required: true, schema: {type: string}}
      responses:
        '200':
          content:
            'application/vnd.example+json;version=2': {}
components:
  securitySchemes:
    ApiKey: {type: apiKey, in: header, name: X-Api-Key}
x-bioseq-client-name: Contract
x-bioseq-integration:
  - name: typed-batch
    operationId: fetchItem
    accept: application/json
    mode: batch
    count: 2
    parameters:
      - {name: id, in: path, value: -9223372036854775808}
      - {name: tag, in: query, value: first}
      - {name: tag, in: query, value: second}
      - {name: enabled, in: query, value: true}
      - {name: ratio, in: query, value: 0.5}
    expect:
      status: 200
      bodyContains: [ok]
      headerContains:
        - {name: Link, valueContains: 'rel="next"'}
)yaml";
    const auto contract = parse_openapi(valid_openapi);
    require(contract.host == "api.example.org" && contract.port == 8443,
            "OpenAPI authority lowering");
    require(contract.get_operations.size() == 1
                && contract.get_operations.front().accept
                    == "application/vnd.example+json;version=2"
                && contract.get_operations.front().parameters.size() == 4,
            "OpenAPI operation/media lowering");
    require(contract.get_operations.front().api_key_name == "X-Api-Key",
            "OpenAPI API-key lowering");
    require(contract.integration_cases.size() == 1
                && contract.integration_cases.front().parameters.size() == 5
                && contract.integration_cases.front().count == 2
                && contract.integration_cases.front().accept == "application/json"
                && contract.integration_cases.front().header_contains.size() == 1,
            "typed batch fixture lowering");
    const generated_api contract_api{
        .client_name = "contract",
        .declared_type = "bioseq_external_api_client",
        .document = contract,
    };
    require(render_module(contract_api).contains(
                "transport_(\"api.example.org\", 8443)"),
            "explicit-port client rendering");
    require(render_test(contract_api).contains(
                "std::numeric_limits<std::int64_t>::min()"),
            "typed fixture rendering");
    require(render_module(contract_api).contains(
                "accept.empty() ? operation->accept : accept"),
            "explicit Accept precedence rendering");
    require(render_module(contract_api).contains("get_with_accept"),
            "unambiguous Accept entry point rendering");
    require(render_test(contract_api).contains("response_header_contains"),
            "response-header expectation rendering");
    YAML::Node invalid_metadata = YAML::Load(std::string(valid_openapi));
    invalid_metadata["x-bioseq-integration"][0]["unknown"] = true;
    const std::string invalid_metadata_spec = YAML::Dump(invalid_metadata);
    require_failure(
        [invalid_metadata_spec] { (void)parse_openapi(invalid_metadata_spec); },
        "unknown integration case key");
    YAML::Node invalid_accept = YAML::Load(std::string(valid_openapi));
    invalid_accept["x-bioseq-integration"][0]["accept"] =
        "text/plain\r\nInjected: yes";
    const std::string invalid_accept_spec = YAML::Dump(invalid_accept);
    require_failure(
        [invalid_accept_spec] { (void)parse_openapi(invalid_accept_spec); },
        "integration Accept override is invalid");

    const YAML::Node plain_json = YAML::Load(R"yaml(
operation:
  responses:
    '200':
      content:
        'application/json; charset=UTF-8': {}
)yaml");
    require(
        preferred_response_media_type(plain_json, plain_json["operation"])
            == "application/json",
        "plain JSON charset normalization");
    const YAML::Node vendor_json = YAML::Load(R"yaml(
operation:
  responses:
    '200':
      content:
        'application/vnd.example+json;version=2': {}
)yaml");
    require(
        preferred_response_media_type(vendor_json, vendor_json["operation"])
            == "application/vnd.example+json;version=2",
        "versioned JSON media preservation");

    const YAML::Node compound_security = YAML::Load(R"yaml(
components:
  securitySchemes:
    first: {type: apiKey, in: header, name: X-First-Key}
    second: {type: apiKey, in: query, name: second_key}
security:
  - first: []
    second: []
operation: {}
)yaml");
    require_failure(
        [&compound_security] {
            (void)effective_api_key_requirement(
                compound_security, compound_security["operation"]);
        },
        "compound");
    const YAML::Node anonymous_security = YAML::Load(R"yaml(
security: [{}]
operation: {}
)yaml");
    require(
        !effective_api_key_requirement(
            anonymous_security, anonymous_security["operation"]),
        "anonymous security alternative");
    require(
        !http_validation::valid_header_name("Bad Header"),
        "header token rejection");
    require(
        integration_scalar_literal(integration_scalar{
            std::numeric_limits<std::int64_t>::min()})
            == "std::numeric_limits<std::int64_t>::min()",
        "INT64_MIN literal rendering");
}

/// @brief Renders the standalone CMake graph for one generated client.
/// @param api Validated API model.
/// @return Generated CMake source.
[[nodiscard]] std::string render_cmake(const generated_api& api) {
    std::ostringstream out;
    out << "# Generated by bioseq_external_codegen.\n"
        << "include(\"${CMAKE_CURRENT_LIST_DIR}/../../generated_client.cmake\")\n"
        << "bioseq_register_generated_api_client(\n"
        << "  CLIENT_NAME \"" << api.client_name << "\"\n"
        << "  CLIENT_ROOT \"${CMAKE_CURRENT_LIST_DIR}\")\n";
    return out.str();
}

/// @brief Publishes all artifacts for one generated API without timestamp churn.
/// @param api Validated API model.
/// @param output_root Generator-owned artifact root.
/// @param compiler_id Client compiler provenance.
void emit_api(
    const generated_api& api,
    const fs::path& output_root,
    std::string_view compiler_id) {
    const fs::path root = output_root / "api" / api.client_name;
    write_if_different(root / "client/index.cppm", render_module(api));
    write_if_different(
        root / "integration_tests" / ("test_" + api.client_name + "_client.cpp"),
        render_test(api));
    write_if_different(root / "CMakeLists.txt", render_cmake(api));

    std::ostringstream manifest;
    manifest << "client\t" << api.client_name << '\n'
             << "module\tbioseq.external.api." << identifier(api.client_name) << '\n'
             << "class\t" << api.declared_type << '\n'
             << "get_operations\t" << api.document.get_operations.size() << '\n'
             << "integration_cases\t" << api.document.integration_cases.size() << '\n';
    write_if_different(root / "manifest.tsv", manifest.str());

    std::ostringstream provenance;
    provenance << "generator_compiler=GCC-16\n"
               << "client_compiler=" << compiler_id << '\n'
               << "parser=cppast/libclang\n"
               << "openapi_title=" << api.document.title << '\n';
    write_if_different(root / "provenance.txt", provenance.str());
}

/// @brief Wraps API metadata in the registry's type-erased emitter.
/// @param request Resolved API annotation.
/// @return Named deferred output emitter.
[[nodiscard]] generated_output generate_api_output(const annotation_request& request) {
    generated_api api = generate_api_client(request);
    const std::string name = api.client_name;
    return generated_output{
        .name = name,
        .emit = [api = std::move(api)](
                    const fs::path& output_root,
                    std::string_view compiler_id) {
            emit_api(api, output_root, compiler_id);
        },
    };
}

/// @brief Derives an API output identity from its descriptor filename.
/// @param source Descriptor path.
/// @return Descriptor stem.
[[nodiscard]] std::string api_output_name(const fs::path& source) {
    return source.stem().string();
}

/// @brief Maps an API identity to its generator-owned relative root.
/// @param output_name API output identity.
/// @return Relative `api/<name>` path.
[[nodiscard]] fs::path api_owned_output_path(std::string_view output_name) {
    return fs::path("api") / output_name;
}

/// @brief Checks whether every required API artifact is present.
/// @param output_root Generator-owned artifact root.
/// @param output_name API output identity.
/// @return True when the complete output set exists.
[[nodiscard]] bool api_outputs_exist(
    const fs::path& output_root,
    std::string_view output_name) {
    const fs::path root = output_root / "api" / output_name;
    return fs::exists(root / "client/index.cppm")
        && fs::exists(root / "integration_tests" /
            ("test_" + std::string(output_name) + "_client.cpp"))
        && fs::exists(root / "CMakeLists.txt")
        && fs::exists(root / "manifest.tsv")
        && fs::exists(root / "provenance.txt");
}

/// @brief Adds one API subdirectory to the generated CMake inventory.
/// @param inventory Inventory stream to extend.
/// @param output_name API output identity.
void append_api_cmake_inventory(
    std::ostringstream& inventory,
    std::string_view output_name) {
    inventory << "add_subdirectory(\"${CMAKE_CURRENT_LIST_DIR}/api/"
              << output_name << "\")\n";
}

/// @brief Normalizes and confines a handler-owned path to the output root.
/// @param path Candidate relative path.
/// @param owner Source used in validation diagnostics.
/// @return Safe normalized relative path.
[[nodiscard]] fs::path checked_owned_output_path(
    fs::path path,
    std::string_view owner) {
    path = path.lexically_normal();
    if (path.empty() || path == "." || path.is_absolute()) {
        throw std::runtime_error(
            "annotation output path must be a non-empty relative path: "
            + std::string(owner));
    }
    for (const auto& component : path) {
        if (component == "..") {
            throw std::runtime_error(
                "annotation output path escapes the generation root: "
                + std::string(owner));
        }
    }
    return path;
}

/// @brief Parses and validates generator command-line options.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Normalized command configuration.
[[nodiscard]] command_line parse_command_line(int argc, char** argv) {
    command_line result{.generator_binary = fs::absolute(argv[0])};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + std::string(argument));
        }
        const std::string value = argv[++index];
        if (argument == "--source-root") result.source_root = value;
        else if (argument == "--output-root") result.output_root = value;
        else if (argument == "--clang-binary") result.clang_binary = value;
        else if (argument == "--compiler-id") result.compiler_id = value;
        else if (argument == "--stamp") result.stamp = value;
        else throw std::runtime_error("unknown argument: " + std::string(argument));
    }
    if (result.source_root.empty() || result.output_root.empty()
        || result.clang_binary.empty() || result.compiler_id.empty()
        || result.stamp.empty()) {
        throw std::runtime_error(
            "required arguments: --source-root --output-root --clang-binary "
            "--compiler-id --stamp");
    }
    return result;
}

struct source_state {
    std::uintmax_t size{};
    std::int64_t ticks{};
    std::uint64_t hash{};
    bool annotated{};
    std::string annotation_name;
    std::string output_name;
    std::string owned_output_path;
};

/// @brief Loads cached per-source metadata and checks its registry signature.
/// @param path State file path.
/// @param signature Expected state signature.
/// @param signature_matches Receives whether the cache is reusable.
/// @return Parsed states, including legacy cleanup metadata.
[[nodiscard]] std::unordered_map<std::string, source_state>
load_source_state(
    const fs::path& path,
    std::string_view signature,
    bool& signature_matches) {
    std::unordered_map<std::string, source_state> result;
    signature_matches = false;
    if (!fs::exists(path)) return result;
    std::istringstream input(read_file(path));
    std::string line;
    if (!std::getline(input, line)) return result;
    signature_matches = line == "#signature\t" + std::string(signature);
    while (std::getline(input, line)) {
        std::array<std::string, 8> columns{};
        std::size_t begin = 0;
        std::size_t count = 0;
        while (count < columns.size()) {
            const auto tab = line.find('\t', begin);
            columns[count++] = line.substr(begin, tab - begin);
            if (tab == std::string::npos) break;
            begin = tab + 1;
        }
        if (count < 4) continue;
        source_state state;
        const auto size_result = std::from_chars(
            columns[1].data(), columns[1].data() + columns[1].size(), state.size);
        const auto ticks_result = std::from_chars(
            columns[2].data(), columns[2].data() + columns[2].size(), state.ticks);
        const auto hash_result = std::from_chars(
            columns[3].data(), columns[3].data() + columns[3].size(), state.hash, 16);
        if (size_result.ec != std::errc{} || ticks_result.ec != std::errc{}
            || hash_result.ec != std::errc{}) {
            continue;
        }
        state.annotated = count < 5 || columns[4] == "1";
        if (state.annotated) {
            if (count >= 7 && !columns[5].empty() && !columns[6].empty()) {
                state.annotation_name = columns[5];
                state.output_name = columns[6];
            } else {
                // State v1 only supported api_client, whose output name was the source stem.
                state.annotation_name = "bioseq::api_client";
                state.output_name = fs::path(columns[0]).stem().string();
            }
            if (count >= 8 && !columns[7].empty()) {
                state.owned_output_path = columns[7];
            } else if (state.annotation_name == "bioseq::api_client") {
                state.owned_output_path =
                    (fs::path("api") / state.output_name).generic_string();
            }
        }
        result.emplace(columns[0], state);
    }
    return result;
}

/// @brief Fingerprints handlers, renderer compiler, parser, and generator.
/// @param command Active generator configuration.
/// @param handlers Registered annotation contracts.
/// @return Hexadecimal cache signature.
[[nodiscard]] std::string state_signature(
    const command_line& command,
    const annotation_handlers& handlers) {
    std::vector<std::string> annotation_names;
    annotation_names.reserve(handlers.size());
    for (const auto& [name, handler] : handlers) {
        (void)handler;
        annotation_names.push_back(name);
    }
    std::ranges::sort(annotation_names);
    std::ostringstream input;
    input << "state-v4\n";
    for (const auto& name : annotation_names) input << name << '\n';
    const fs::path clang_binary = fs::weakly_canonical(command.clang_binary);
    input << command.compiler_id << '\n'
          << clang_binary.generic_string() << '\n'
          << fs::file_size(clang_binary) << '\n'
          << fs::last_write_time(clang_binary).time_since_epoch().count() << '\n'
          << fs::file_size(command.generator_binary) << '\n'
          << fs::last_write_time(command.generator_binary).time_since_epoch().count();
    std::ostringstream output;
    output << std::hex << bioseq::common::fnv1a_64(input.str());
    return output.str();
}

/// @brief Cheaply detects registered attribute spellings before cppast parsing.
/// @param content Source text to scan.
/// @param handlers Registered annotation contracts.
/// @return True when a registered Bioseq attribute is present.
[[nodiscard]] bool contains_registered_annotation(
    std::string_view content,
    const annotation_handlers& handlers) {
    return std::ranges::any_of(handlers, [content](const auto& entry) {
        std::size_t position = content.find(entry.first);
        while (position != std::string_view::npos) {
            std::size_t prefix = position;
            while (prefix > 0) {
                const char character = content[prefix - 1];
                if (character != ' ' && character != '\t') {
                    break;
                }
                --prefix;
            }
            std::size_t suffix = position + entry.first.size();
            while (suffix < content.size()
                   && (content[suffix] == ' ' || content[suffix] == '\t')) {
                ++suffix;
            }
            if (prefix >= 2 && content[prefix - 1] == '['
                && content[prefix - 2] == '[' && suffix < content.size()
                && content[suffix] == '(') {
                return true;
            }
            position = content.find(entry.first, position + entry.first.size());
        }
        return false;
    });
}

/// @brief Serializes one source's incremental metadata as TSV.
/// @param output State stream to extend.
/// @param relative Source path relative to the scan root.
/// @param state Metadata to persist.
void append_source_state(
    std::ostringstream& output,
    std::string_view relative,
    const source_state& state) {
    output << relative << '\t' << state.size << '\t' << state.ticks << '\t'
           << std::hex << state.hash << std::dec << '\t'
           << (state.annotated ? '1' : '0') << '\t'
           << state.annotation_name << '\t' << state.output_name << '\t'
           << state.owned_output_path << '\n';
}

/// @brief Checks whether original and normalized parser copies are staged.
/// @param output_root Generator-owned artifact root.
/// @param relative_path Descriptor path relative to the scan root.
/// @return True when both staged files exist.
[[nodiscard]] bool staged_sources_exist(
    const fs::path& output_root,
    const fs::path& relative_path) {
    fs::path shadow_relative = relative_path;
    shadow_relative.replace_extension(".shadow.cpp");
    return fs::exists(output_root / "annotation_sources" / relative_path)
        && fs::exists(output_root / "parser_sources" / shadow_relative);
}

/// @brief Scans descriptors, incrementally emits outputs, and removes stale artifacts.
/// @param command Validated generator configuration.
/// @return Zero after successful generation.
[[nodiscard]] int execute(const command_line& command) {
    const annotation_handlers handlers{
        {"bioseq::api_client", annotation_handler{
            .generate = &generate_api_output,
            .output_name = &api_output_name,
            .owned_output_path = &api_owned_output_path,
            .outputs_exist = &api_outputs_exist,
            .append_cmake_inventory = &append_api_cmake_inventory,
        }},
    };
    std::vector<fs::path> sources;
    for (const auto& entry : fs::recursive_directory_iterator(command.source_root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cppm") {
            sources.push_back(entry.path());
        }
    }
    std::ranges::sort(sources);

    struct pending_output {
        std::string annotation_name;
        generated_output output;
    };
    struct registered_output {
        const annotation_handler* handler{};
        std::string annotation_name;
        std::string name;
        std::string source;
        std::string owned_output_path;
    };
    std::vector<pending_output> generated;
    std::map<std::string, registered_output> output_inventory;
    std::map<std::string, std::pair<std::string, std::string>> owned_output_owners;
    std::map<std::string, std::string> source_outputs;
    std::ostringstream state;
    const fs::path state_path = command.output_root / "generation_state.tsv";
    const std::string signature = state_signature(command, handlers);
    state << "#signature\t" << signature << '\n';
    bool state_cache_valid = false;
    const auto stored_state = load_source_state(state_path, signature, state_cache_valid);
    const std::unordered_map<std::string, source_state> empty_state;
    const auto& previous_state = state_cache_valid ? stored_state : empty_state;
    const auto output_key = [](std::string_view annotation_name, std::string_view output_name) {
        std::string key(annotation_name);
        key.push_back('\n');
        key += output_name;
        return key;
    };
    const auto register_output = [
                                     &handlers,
                                     &owned_output_owners,
                                     &output_inventory,
                                     &output_key,
                                     &source_outputs](
                                     std::string_view annotation_name,
                                     std::string_view name,
                                     const std::string& relative) {
        const auto handler = handlers.find(std::string(annotation_name));
        if (handler == handlers.end()) {
            throw std::runtime_error(
                "source state references unregistered annotation '"
                + std::string(annotation_name) + "': " + relative);
        }
        if (name.empty() || name.find_first_of("\t\r\n") != std::string_view::npos) {
            throw std::runtime_error(
                "annotation handler produced an invalid output name for " + relative);
        }
        const std::string owned_output_path = checked_owned_output_path(
            handler->second.owned_output_path(name), relative).generic_string();
        const std::string key = output_key(annotation_name, name);
        const auto [owner, owns_path] = owned_output_owners.emplace(
            owned_output_path, std::pair{key, relative});
        if (!owns_path && owner->second.first != key) {
            throw std::runtime_error(
                "annotation outputs collide at '" + owned_output_path + "': "
                + owner->second.second + " and " + relative);
        }
        const auto [position, inserted] = output_inventory.emplace(
            key,
            registered_output{
                .handler = &handler->second,
                .annotation_name = std::string(annotation_name),
                .name = std::string(name),
                .source = relative,
                .owned_output_path = owned_output_path,
            });
        if (!inserted && position->second.source != relative) {
            throw std::runtime_error(
                "annotated sources produce duplicate output '" + std::string(name)
                + "' for " + std::string(annotation_name) + ": "
                + position->second.source + " and " + relative);
        }
        const auto [source_position, source_inserted] =
            source_outputs.emplace(relative, key);
        if (!source_inserted && source_position->second != key) {
            throw std::runtime_error(
                "source contains multiple registered annotations: " + relative);
        }
        return owned_output_path;
    };
    for (const auto& source : sources) {
        const fs::path relative_path = fs::relative(source, command.source_root);
        const std::string relative = relative_path.generic_string();
        const auto size = fs::file_size(source);
        const auto ticks = static_cast<std::int64_t>(
            fs::last_write_time(source).time_since_epoch().count());
        const auto previous = previous_state.find(relative);
        if (previous != previous_state.end()
            && previous->second.size == size && previous->second.ticks == ticks
            && (!previous->second.annotated
                || (handlers.contains(previous->second.annotation_name)
                    && !previous->second.owned_output_path.empty()
                    && handlers.at(previous->second.annotation_name).outputs_exist(
                        command.output_root, previous->second.output_name)
                    && staged_sources_exist(command.output_root, relative_path)))) {
            append_source_state(state, relative, previous->second);
            if (previous->second.annotated) {
                const std::string owned_output_path = register_output(
                    previous->second.annotation_name,
                    previous->second.output_name,
                    relative);
                if (owned_output_path != previous->second.owned_output_path) {
                    throw std::runtime_error(
                        "cached annotation output path changed without state invalidation: "
                        + relative);
                }
            }
            continue;
        }

        const std::string content = read_file(source);
        const bool annotated = contains_registered_annotation(content, handlers);
        source_state current{
            .size = size,
            .ticks = ticks,
            .hash = bioseq::common::fnv1a_64(content),
            .annotated = annotated,
        };
        if (!annotated) {
            append_source_state(state, relative, current);
            continue;
        }
        if (previous != previous_state.end()
            && previous->second.annotated && previous->second.hash == current.hash
            && handlers.contains(previous->second.annotation_name)
            && !previous->second.owned_output_path.empty()
            && handlers.at(previous->second.annotation_name).outputs_exist(
                command.output_root, previous->second.output_name)
            && staged_sources_exist(command.output_root, relative_path)) {
            current.annotation_name = previous->second.annotation_name;
            current.output_name = previous->second.output_name;
            current.owned_output_path = register_output(
                current.annotation_name, current.output_name, relative);
            append_source_state(state, relative, current);
            continue;
        }

        write_if_different(
            command.output_root / "annotation_sources" / relative_path, content);
        fs::path shadow_relative = relative_path;
        shadow_relative.replace_extension(".shadow.cpp");
        const fs::path shadow = command.output_root / "parser_sources" / shadow_relative;
        const std::string normalized = normalize_shadow(content);
        write_if_different(shadow, normalized);
        const auto request = parse_annotation(source, shadow, command.clang_binary, handlers);
        const auto handler = handlers.find(request.annotation_name);
        if (handler == handlers.end()) {
            throw std::runtime_error(
                "parser returned unregistered annotation '" + request.annotation_name + "'");
        }
        current.annotation_name = request.annotation_name;
        current.output_name = handler->second.output_name(source);
        current.owned_output_path = register_output(
            current.annotation_name, current.output_name, relative);
        generated_output output = handler->second.generate(request);
        if (output.name != current.output_name) {
            throw std::runtime_error(
                "annotation handler output name mismatch for " + source.string()
                + ": expected '" + current.output_name + "', generated '"
                + output.name + "'");
        }
        if (!output.emit) {
            throw std::runtime_error(
                "annotation handler returned no emitter for " + source.string());
        }
        generated.push_back(pending_output{
            .annotation_name = request.annotation_name,
            .output = std::move(output),
        });
        append_source_state(state, relative, current);
    }
    std::ranges::sort(generated, [](const pending_output& left, const pending_output& right) {
        if (left.annotation_name != right.annotation_name) {
            return left.annotation_name < right.annotation_name;
        }
        return left.output.name < right.output.name;
    });

    for (const auto& [relative, previous] : stored_state) {
        if (!previous.annotated) continue;
        const fs::path relative_path(relative);
        const std::string key = output_key(previous.annotation_name, previous.output_name);
        const auto current_output = output_inventory.find(key);
        const fs::path previous_owned_output_path = checked_owned_output_path(
            previous.owned_output_path, relative);
        const bool output_path_is_current = current_output != output_inventory.end()
            && current_output->second.owned_output_path
                == previous_owned_output_path.generic_string();
        if (!output_path_is_current) {
            fs::remove_all(command.output_root / previous_owned_output_path);
        }
        if (source_outputs.contains(relative)) {
            continue;
        }
        fs::remove(command.output_root / "annotation_sources" / relative_path);
        fs::path shadow_relative = relative_path;
        shadow_relative.replace_extension(".shadow.cpp");
        fs::remove(command.output_root / "parser_sources" / shadow_relative);
    }

    std::ostringstream cmake_manifest;
    cmake_manifest << "# Generated external code inventory.\n";
    for (const auto& pending : generated) {
        pending.output.emit(command.output_root, command.compiler_id);
    }
    for (const auto& [key, output] : output_inventory) {
        (void)key;
        output.handler->append_cmake_inventory(cmake_manifest, output.name);
    }
    write_if_different(state_path, state.str());
    write_if_different(command.output_root / "generated_apis.cmake", cmake_manifest.str());
    touch_stamp(command.stamp);
    return 0;
}

/// @brief Runs the generator and converts failures into a diagnostic exit status.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return Process-style success or failure code.
export int run(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--self-test") {
            run_generator_contract_self_tests();
            return 0;
        }
        return execute(parse_command_line(argc, argv));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_external_codegen: {}", error.what());
        return 1;
    }
}

} // namespace bioseq::external::codegen
