module;

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
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

namespace bioseq::external::codegen {

namespace fs = std::filesystem;

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

struct openapi_document {
    std::string title;
    std::string provider_name;
    std::string server_url;
    std::string host;
    std::string base_path;
    std::vector<openapi_operation> get_operations;
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
    const auto location = parse_location(parameter["in"].as<std::string>());
    const bool required = location == parameter_location::path
        || (parameter["required"] && parameter["required"].as<bool>());
    return openapi_parameter{
        .name = parameter["name"].as<std::string>(),
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
        if (const auto json = std::ranges::find_if(media_types, [](std::string_view value) {
            return value == "application/json";
        });
            json != media_types.end()) {
            return *json;
        }
        if (const auto json = std::ranges::find_if(media_types, [](std::string_view value) {
                const auto separator = value.find(';');
                value = value.substr(0, separator);
                return value.ends_with("+json") || value == "application/x-ndjson";
            }); json != media_types.end()) {
            return *json;
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
    if (!security || !security.IsSequence()) return std::nullopt;
    const YAML::Node schemes = document["components"]["securitySchemes"];
    if (!schemes || !schemes.IsMap()) return std::nullopt;
    for (const auto& requirement : security) {
        if (!requirement.IsMap()) continue;
        for (const auto& requested_scheme : requirement) {
            const std::string scheme_name = requested_scheme.first.as<std::string>();
            const YAML::Node scheme = resolve_local_ref(document, schemes[scheme_name]);
            if (!scheme || !scheme["type"] || !scheme["in"] || !scheme["name"]
                || scheme["type"].as<std::string>() != "apiKey") {
                continue;
            }
            const std::string location = scheme["in"].as<std::string>();
            if (location == "header" || location == "query") {
                return api_key_requirement{
                    .name = scheme["name"].as<std::string>(),
                    .location = parse_location(location),
                };
            }
        }
    }
    return std::nullopt;
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
    const auto separator = result.title.find_first_of(" \t");
    result.provider_name = result.title.substr(0, separator);
    result.server_url = document["servers"][0]["url"].as<std::string>();
    constexpr std::string_view scheme = "https://";
    if (!result.server_url.starts_with(scheme)) {
        throw std::runtime_error("only verified HTTPS OpenAPI servers are supported");
    }
    const auto path_start = result.server_url.find('/', scheme.size());
    result.host = result.server_url.substr(
        scheme.size(), path_start == std::string::npos ? std::string::npos : path_start - scheme.size());
    result.base_path = path_start == std::string::npos ? std::string{} : result.server_url.substr(path_start);

    std::map<std::string, bool> operation_ids;
    for (const auto& path_entry : document["paths"]) {
        const std::string path = path_entry.first.as<std::string>();
        const YAML::Node path_item = path_entry.second;
        if (!path_item["get"]) continue;
        const YAML::Node operation = path_item["get"];
        if (!operation["operationId"]) {
            throw std::runtime_error("GET operation lacks operationId at " + path);
        }
        openapi_operation parsed;
        parsed.operation_id = operation["operationId"].as<std::string>();
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

/// @brief Renders the complete C++ module for one API client.
/// @param api Validated API model.
/// @return Generated module source.
[[nodiscard]] std::string render_module(const generated_api& api) {
    const std::string namespace_suffix = identifier(api.client_name);
    const auto has_operation = [&api](std::string_view operation_id) {
        return std::ranges::any_of(api.document.get_operations, [operation_id](const auto& operation) {
            return operation.operation_id == operation_id;
        });
    };
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

export struct parameter_value {
    std::string_view name;
    parameter_location location{};
    parameter_scalar value;
};

/// @brief Non-owning GET invocation whose views must outlive synchronous get_batch().
export struct get_request {
    std::string_view operation_id;
    std::span<const parameter_value> parameters{};
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
    context::result_vector<char> body;

    /// @brief Views arena-owned response bytes without copying them.
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

} // namespace detail

export struct )cpp" << api.declared_type << R"cpp( {
    constexpr static std::string client_name = ")cpp" << cpp_escape(api.document.provider_name)
        << R"cpp(";

    /// @brief Binds the result arena, worker pool, and optional authentication.
    )cpp" << api.declared_type << R"cpp((
        const context::context& memory_context,
        const bioseq::parallelism::GlobalPool& pool,
        std::optional<credentials> supplied_credentials = std::nullopt)
        : memory_context_(memory_context)
        , pool_(pool)
        , credentials_(std::move(supplied_credentials))
        , transport_(")cpp" << cpp_escape(api.document.host) << R"cpp(") {}

    /// @brief Executes one generated GET operation into the result arena.
    [[nodiscard]] std::expected<response, client_error> get(
        std::string_view operation_id,
        std::span<const parameter_value> supplied = {}) {
        auto transported = request_transport(operation_id, supplied);
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
                                requests[index].operation_id, requests[index].parameters);
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
)cpp";
    if (has_operation("version")) {
        out << R"cpp(

    /// @brief Retrieves the service version document.
    [[nodiscard]] std::expected<response, client_error> version() {
        return get("version");
    }
)cpp";
    }
    if (has_operation("assembly_accessions_for_sequence_accession")) {
        out << R"cpp(

    /// @brief Retrieves assemblies associated with a sequence accession.
    [[nodiscard]] std::expected<response, client_error>
    assembly_accessions_for_sequence_accession(std::string_view accession) {
        const std::array values{
            parameter_value{"accession", parameter_location::path, accession},
        };
        return get("assembly_accessions_for_sequence_accession", values);
    }
)cpp";
    }
    if (has_operation("version")) {
        out << R"cpp(

    /// @brief Retrieves multiple version documents concurrently through get_batch().
    [[nodiscard]] std::vector<std::expected<response, client_error>>
    version_batch(std::size_t request_count) {
        const std::vector<get_request> requests(
            request_count, get_request{"version", {}});
        return get_batch(requests);
    }
)cpp";
    }
    out << R"cpp(

private:
    /// @brief Validates parameters and performs transport without touching the result arena.
    [[nodiscard]] std::expected<bioseq::external::transport_response, client_error>
    request_transport(
        std::string_view operation_id,
        std::span<const parameter_value> supplied) const {
        using namespace detail;
        const operation_descriptor* operation = find_operation(operation_id);
        if (operation == nullptr) {
            return std::unexpected(client_error{"unknown GET operation: " + std::string(operation_id), 0});
        }
        std::string path = ")cpp" << cpp_escape(api.document.base_path) << R"cpp(";
        path += operation->path;
        std::vector<bioseq::external::http_header> headers;
        headers.push_back({"Accept", std::string(operation->accept)});
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
        auto result = transport_.get(std::move(path), headers);
        if (!result) {
            return std::unexpected(client_error{
                .message = std::move(result.error().message),
                .transport_code = result.error().transport_code,
            });
        }
        return std::move(*result);
    }

    /// @brief Copies a transported response into the context result arena.
    [[nodiscard]] response materialize(bioseq::external::transport_response transported) {
        auto bytes = memory_context_.make_result_vector<char>();
        while (bytes.capacity() < transported.body.size()) {
            constexpr std::size_t allocation_quantum = alignof(std::max_align_t);
            const std::size_t arena_capacity =
                memory_context_.result_arena().next_capacity();
            if (arena_capacity <= allocation_quantum) {
                throw std::length_error("result arena block is too small for HTTP response");
            }
            const std::size_t safe_arena_capacity =
                ((arena_capacity - allocation_quantum) / allocation_quantum)
                * allocation_quantum;
            if (safe_arena_capacity == 0) {
                throw std::length_error("result arena block has no usable HTTP capacity");
            }
            std::size_t desired_capacity = bytes.capacity() == 0
                ? std::min(transported.body.size(), safe_arena_capacity)
                : bytes.capacity() > transported.body.size() / 2
                    ? transported.body.size()
                    : bytes.capacity() * 2;
            const std::size_t remainder = desired_capacity % allocation_quantum;
            if (remainder != 0) {
                const std::size_t increment = allocation_quantum - remainder;
                if (desired_capacity > std::numeric_limits<std::size_t>::max() - increment) {
                    throw std::length_error("HTTP response exceeds result vector capacity");
                }
                desired_capacity += increment;
            }
            const std::size_t requested_capacity = std::min(
                safe_arena_capacity, desired_capacity);
            if (requested_capacity <= bytes.capacity()) {
                throw std::length_error("result arena failed to grow for HTTP response");
            }
            bytes.reserve(requested_capacity);
        }
        bytes.insert(bytes.end(), transported.body.begin(), transported.body.end());
        return response{transported.status_code, std::move(bytes)};
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
    const auto find_operation = [&api](std::string_view operation_id) {
        return std::ranges::find_if(api.document.get_operations, [operation_id](const auto& operation) {
            return operation.operation_id == operation_id;
        });
    };
    const auto version_operation = find_operation("version");
    const auto accession_operation = find_operation("assembly_accessions_for_sequence_accession");
    const auto parameterless_operation = std::ranges::find_if(
        api.document.get_operations, [](const auto& operation) {
            return std::ranges::none_of(operation.parameters, &openapi_parameter::required);
        });
    const auto batch_operation = version_operation != api.document.get_operations.end()
        ? version_operation
        : parameterless_operation;
    std::ostringstream out;
    out << R"cpp(#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

import bioseq.common.context;
import bioseq.external.api.)cpp" << namespace_suffix << R"cpp(;
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
    bool has_api_key = false;
    if (const char* api_key = std::getenv(")cpp" << api_key_environment_name(api.client_name)
        << R"cpp("); api_key != nullptr && *api_key != '\0') {
        supplied_credentials = generated_api::credentials{std::string(api_key)};
        has_api_key = true;
    }
    generated_api::)cpp" << api.declared_type << R"cpp( client(
        memory_context, pool, std::move(supplied_credentials));
    REQUIRE(client.client_name == ")cpp" << cpp_escape(api.document.provider_name) << R"cpp(");
)cpp";
    if (version_operation != api.document.get_operations.end()) {
        out << R"cpp(

    auto version = client.version();
    INFO((version ? std::string(version->body_view()) : version.error().message));
    REQUIRE(version.has_value());
    REQUIRE(version->status_code == 200);
    REQUIRE(version->body_view().find("version") != std::string_view::npos);
)cpp";
    }
    if (accession_operation != api.document.get_operations.end()) {
        out << R"cpp(

    auto accession = client.assembly_accessions_for_sequence_accession("NC_000001.11");
    INFO((accession ? std::string(accession->body_view()) : accession.error().message));
    REQUIRE(accession.has_value());
    REQUIRE(accession->status_code == 200);
    REQUIRE(accession->body_view().find("GCF_") != std::string_view::npos);
)cpp";
    }
    if (batch_operation != api.document.get_operations.end()) {
        out << R"cpp(

    if (!has_api_key) {
        // Keep the concurrent proof below within conservative unauthenticated rate limits.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    }
    const std::array batch_requests{
        generated_api::get_request{")cpp"
            << cpp_escape(batch_operation->operation_id) << R"cpp(", {}},
        generated_api::get_request{")cpp"
            << cpp_escape(batch_operation->operation_id) << R"cpp(", {}},
    };
    auto batch = client.get_batch(batch_requests);
    REQUIRE(batch.size() == 2);
    for (const auto& item : batch) {
        INFO((item ? std::string(item->body_view()) : item.error().message));
        REQUIRE(item.has_value());
        REQUIRE(item->status_code == 200);
    }
)cpp";
    }
    out << R"cpp(
}
)cpp";
    return out.str();
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
             << "get_operations\t" << api.document.get_operations.size() << '\n';
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
        return execute(parse_command_line(argc, argv));
    } catch (const std::exception& error) {
        std::println(stderr, "bioseq_external_codegen: {}", error.what());
        return 1;
    }
}

} // namespace bioseq::external::codegen
