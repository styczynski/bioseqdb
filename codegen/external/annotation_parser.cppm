/**
 * @file annotation_parser.cppm
 * @brief Reads external-client annotations and their embedded API specifications.
 */
module;

#include <algorithm>
#include <cppast/cpp_attribute.hpp>
#include <cppast/cpp_class.hpp>
#include <cppast/cpp_entity_index.hpp>
#include <cppast/cpp_entity_kind.hpp>
#include <cppast/cpp_expression.hpp>
#include <cppast/cpp_file.hpp>
#include <cppast/cpp_variable.hpp>
#include <cppast/libclang_parser.hpp>
#include <cppast/visitor.hpp>
#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.external:annotation_parser;

import :model;
import bioseq.codegen.support;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

/// @brief Module names mapped to the descriptor files that export them.
using module_provider_index =
    std::map<std::string, std::vector<const descriptor_source*>, std::less<>>;

namespace {

constexpr std::string_view fragment_annotation_name = "bioseq::api_client_fragments";
constexpr std::size_t maximum_raw_delimiter_size = 16;

[[nodiscard]] constexpr auto is_ascii_whitespace(char value) noexcept -> bool {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
           value == '\v';
}

[[nodiscard]] constexpr auto is_identifier_start(char value) noexcept -> bool {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] constexpr auto is_identifier_continue(char value) noexcept -> bool {
    return is_identifier_start(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] auto trim_view(std::string_view value) noexcept -> std::string_view {
    while (!value.empty() && is_ascii_whitespace(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_ascii_whitespace(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

auto skip_whitespace(std::string_view value, std::size_t& position) noexcept -> void {
    while (position < value.size() && is_ascii_whitespace(value[position])) {
        ++position;
    }
}

auto blank_non_newline_characters(std::string& masked, std::size_t begin, std::size_t end) -> void {
    for (std::size_t position = begin; position < end; ++position) {
        if (masked[position] != '\n' && masked[position] != '\r') {
            masked[position] = ' ';
        }
    }
}

[[nodiscard]] auto is_valid_raw_delimiter(std::string_view delimiter) noexcept -> bool {
    if (delimiter.size() > maximum_raw_delimiter_size) {
        return false;
    }
    return std::ranges::none_of(delimiter, [](char value) -> bool {
        return is_ascii_whitespace(value) || value == '(' || value == ')' || value == '\\' ||
               value == '"';
    });
}

/// @brief Replaces comments and literals with spaces so searches rnly see C++ code.
/// @param source C++ source bytes.
/// @return Text of  the same size, preserving line endings and code positions.
[[nodiscard]] auto mask_cpp_non_code(std::string_view source) -> std::string {
    std::string masked(source);
    std::size_t position = 0;
    while (position < source.size()) {
        if (source.substr(position).starts_with("//")) {
            const std::size_t line_end = source.find('\n', position + 2);
            const std::size_t masked_end =
                line_end == std::string_view::npos ? source.size() : line_end;
            blank_non_newline_characters(masked, position, masked_end);
            position = masked_end;
            continue;
        }
        if (source.substr(position).starts_with("/*")) {
            const std::size_t comment_end = source.find("*/", position + 2);
            const std::size_t masked_end =
                comment_end == std::string_view::npos ? source.size() : comment_end + 2;
            blank_non_newline_characters(masked, position, masked_end);
            position = masked_end;
            continue;
        }
        if (source.substr(position).starts_with("R\"")) {
            const std::size_t delimiter_end = source.find('(', position + 2);
            if (delimiter_end != std::string_view::npos) {
                const std::string_view delimiter =
                    source.substr(position + 2, delimiter_end - position - 2);
                if (is_valid_raw_delimiter(delimiter)) {
                    const std::string terminator = ")" + std::string(delimiter) + "\"";
                    const std::size_t content_end = source.find(terminator, delimiter_end + 1);
                    const std::size_t masked_end = content_end == std::string_view::npos
                                                       ? source.size()
                                                       : content_end + terminator.size();
                    blank_non_newline_characters(masked, position, masked_end);
                    position = masked_end;
                    continue;
                }
            }
        }
        if (source[position] == '"' || source[position] == '\'') {
            const char quote = source[position];
            std::size_t literal_end = position + 1;
            bool escaped = false;
            while (literal_end < source.size()) {
                const char value = source[literal_end];
                ++literal_end;
                if (escaped) {
                    escaped = false;
                } else if (value == '\\') {
                    escaped = true;
                } else if (value == quote) {
                    break;
                }
            }
            blank_non_newline_characters(masked, position, literal_end);
            position = literal_end;
            continue;
        }
        ++position;
    }
    return masked;
}

[[nodiscard]] auto find_word(std::string_view source,
                             std::string_view word,
                             std::size_t begin = 0) noexcept -> std::size_t {
    std::size_t position = source.find(word, begin);
    while (position != std::string_view::npos) {
        const bool valid_prefix = position == 0 || !is_identifier_continue(source[position - 1]);
        const std::size_t end = position + word.size();
        const bool valid_suffix = end == source.size() || !is_identifier_continue(source[end]);
        if (valid_prefix && valid_suffix) {
            return position;
        }
        position = source.find(word, position + word.size());
    }
    return std::string_view::npos;
}

[[nodiscard]] auto consume_word(std::string_view source,
                                std::size_t& position,
                                std::string_view expected) noexcept -> bool {
    skip_whitespace(source, position);
    if (!source.substr(position).starts_with(expected)) {
        return false;
    }
    const std::size_t end = position + expected.size();
    if ((position > 0 && is_identifier_continue(source[position - 1])) ||
        (end < source.size() && is_identifier_continue(source[end]))) {
        return false;
    }
    position = end;
    return true;
}

[[nodiscard]] auto
consume_character(std::string_view source, std::size_t& position, char expected) noexcept -> bool {
    skip_whitespace(source, position);
    if (position >= source.size() || source[position] != expected) {
        return false;
    }
    ++position;
    return true;
}

[[nodiscard]] auto consume_scope_operator(std::string_view source, std::size_t& position) noexcept
    -> bool {
    skip_whitespace(source, position);
    if (!source.substr(position).starts_with("::")) {
        return false;
    }
    position += 2;
    return true;
}

[[nodiscard]] auto parse_identifier(std::string_view source, std::size_t& position)
    -> std::optional<std::string> {
    skip_whitespace(source, position);
    if (position >= source.size() || !is_identifier_start(source[position])) {
        return std::nullopt;
    }
    const std::size_t begin = position++;
    while (position < source.size() && is_identifier_continue(source[position])) {
        ++position;
    }
    return std::string(source.substr(begin, position - begin));
}

[[nodiscard]] auto parse_qualified_identifier(std::string_view source, std::size_t& position)
    -> std::optional<std::string> {
    std::optional<std::string> first = parse_identifier(source, position);
    if (!first) {
        return std::nullopt;
    }
    std::string result = std::move(*first);
    while (true) {
        const std::size_t before_scope = position;
        if (!consume_scope_operator(source, position)) {
            position = before_scope;
            break;
        }
        std::optional<std::string> component = parse_identifier(source, position);
        if (!component) {
            return std::nullopt;
        }
        result += "::";
        result += *component;
    }
    return result;
}

[[nodiscard]] auto find_matching_delimiter(std::string_view source,
                                           std::size_t opening_position,
                                           char opening,
                                           char closing) -> std::size_t {
    std::size_t depth = 0;
    for (std::size_t position = opening_position; position < source.size(); ++position) {
        if (source[position] == opening) {
            ++depth;
        } else if (source[position] == closing) {
            if (depth == 0) {
                break;
            }
            --depth;
            if (depth == 0) {
                return position;
            }
        }
    }
    return std::string_view::npos;
}

[[nodiscard]] auto is_module_name(std::string_view value) noexcept -> bool {
    if (value.empty() || !is_identifier_start(value.front())) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) -> bool {
        return is_identifier_continue(character) || character == '.' || character == ':';
    });
}

/// @brief Reads the exported module name from a source file.
/// @param source Interface source text.
/// @param source_path Path used in errors.
/// @return Module name, or std::nullopt if no exported module is declared.
/// @throws std::runtime_error For malformed or multiple exported module declarations.
[[nodiscard]] auto parse_exported_module_name(std::string_view source, const fs::path& source_path)
    -> std::optional<std::string> {
    const std::string masked = mask_cpp_non_code(source);
    std::vector<std::string> names;
    std::size_t search_position = 0;
    while (true) {
        const std::size_t export_position = find_word(masked, "export", search_position);
        if (export_position == std::string_view::npos) {
            break;
        }
        std::size_t position = export_position + std::string_view{"export"}.size();
        if (consume_word(masked, position, "module")) {
            skip_whitespace(masked, position);
            const std::size_t terminator = masked.find(';', position);
            if (terminator == std::string_view::npos) {
                throw std::runtime_error("unterminated exported module declaration in " +
                                         source_path.string());
            }
            const std::string_view name =
                trim_view(std::string_view{masked}.substr(position, terminator - position));
            if (!is_module_name(name)) {
                throw std::runtime_error("invalid exported module declaration in " +
                                         source_path.string());
            }
            names.emplace_back(name);
            search_position = terminator + 1;
            continue;
        }
        search_position = export_position + std::string_view{"export"}.size();
    }
    if (names.size() > 1) {
        throw std::runtime_error("multiple exported module declarations in " +
                                 source_path.string());
    }
    if (names.empty()) {
        return std::nullopt;
    }
    return names.front();
}

/// @brief Reads named-module imports and expands relative partition names.
/// @param source Primary module source text.
/// @param primary_module Name of the primary module.
/// @param source_path Path used in errors.
/// @return Unique full module names imported by the source.
[[nodiscard]] auto parse_imported_modules(std::string_view source,
                                          std::string_view primary_module,
                                          const fs::path& source_path)
    -> std::set<std::string, std::less<>> {
    const std::string masked = mask_cpp_non_code(source);
    const std::size_t primary_partition = primary_module.find(':');
    const std::string_view primary_name = primary_module.substr(0, primary_partition);
    std::set<std::string, std::less<>> imports;
    std::size_t search_position = 0;
    while (true) {
        const std::size_t import_position = find_word(masked, "import", search_position);
        if (import_position == std::string_view::npos) {
            break;
        }
        std::size_t position = import_position + std::string_view{"import"}.size();
        skip_whitespace(masked, position);
        const std::size_t terminator = masked.find(';', position);
        if (terminator == std::string_view::npos) {
            throw std::runtime_error("unterminated import declaration in " + source_path.string());
        }
        const std::string_view imported =
            trim_view(std::string_view{masked}.substr(position, terminator - position));
        if (!imported.empty() && imported.front() != '<' && imported.front() != '"') {
            std::string resolved(imported);
            if (imported.front() == ':') {
                resolved = std::string(primary_name) + std::string(imported);
            }
            if (is_module_name(resolved)) {
                imports.insert(std::move(resolved));
            }
        }
        search_position = terminator + 1;
    }
    return imports;
}

/// @brief Counts exact attribute invocations outside comments and literals.
/// @param source Authored C++ source bytes.
/// @param attribute_name Fully qualified vendor-attribute name.
/// @return Number of matching `[[name(` token sequences.
[[nodiscard]] auto count_attribute_invocations(std::string_view source,
                                               std::string_view attribute_name) -> std::size_t {
    const std::string masked = mask_cpp_non_code(source);
    std::size_t count = 0;
    std::size_t position = masked.find(attribute_name);
    while (position != std::string_view::npos) {
        const bool valid_prefix = position == 0 || (!is_identifier_continue(masked[position - 1]) &&
                                                    masked[position - 1] != ':');
        const std::size_t name_end = position + attribute_name.size();
        const bool valid_suffix =
            name_end == masked.size() ||
            (!is_identifier_continue(masked[name_end]) && masked[name_end] != ':');
        std::size_t attribute_begin = position;
        while (attribute_begin > 0 && is_ascii_whitespace(masked[attribute_begin - 1])) {
            --attribute_begin;
        }
        std::size_t argument_begin = name_end;
        skip_whitespace(masked, argument_begin);
        if (valid_prefix && valid_suffix && attribute_begin >= 2 &&
            masked.substr(attribute_begin - 2, 2) == "[[" && argument_begin < masked.size() &&
            masked[argument_begin] == '(') {
            ++count;
        }
        position = masked.find(attribute_name, name_end);
    }
    return count;
}

[[nodiscard]] auto declaration_boundary_before(std::string_view source,
                                               std::size_t position) noexcept -> std::size_t {
    std::size_t boundary = 0;
    for (const char delimiter : std::string_view{";{}"}) {
        const std::size_t candidate = source.rfind(delimiter, position);
        if (candidate != std::string_view::npos) {
            boundary = std::max(boundary, candidate + 1);
        }
    }
    return boundary;
}

/// @brief Reads the ordered list of specification fragments from a local array.
/// @param source Primary module source text.
/// @param variable_name Array named by api_client_fragments.
/// @param source_path Path used  in errors.
/// @return Qualified constant names in array order.
/// @details The array must use static constexpr auto and
/// std::to_array<std::string_view>. Empty lists and duplicate names are rejected.
[[nodiscard]] auto parse_fragment_references(std::string_view source,
                                             std::string_view variable_name,
                                             const fs::path& source_path)
    -> std::vector<std::string> {
    if (variable_name.empty() || !is_identifier_start(variable_name.front()) ||
        !std::ranges::all_of(variable_name, is_identifier_continue)) {
        throw std::runtime_error(
            "api_client_fragments argument must be one unqualified identifier in " +
            source_path.string());
    }

    const std::string masked = mask_cpp_non_code(source);
    std::vector<std::pair<std::size_t, std::size_t>> initializer_ranges;
    std::size_t search_position = 0;
    while (true) {
        const std::size_t variable_position = find_word(masked, variable_name, search_position);
        if (variable_position == std::string_view::npos) {
            break;
        }
        search_position = variable_position + variable_name.size();

        std::size_t declaration_position = declaration_boundary_before(masked, variable_position);
        if (!consume_word(masked, declaration_position, "static") ||
            !consume_word(masked, declaration_position, "constexpr") ||
            !consume_word(masked, declaration_position, "auto")) {
            continue;
        }
        skip_whitespace(masked, declaration_position);
        if (declaration_position != variable_position) {
            continue;
        }

        std::size_t position = variable_position + variable_name.size();
        if (!consume_character(masked, position, '=')) {
            continue;
        }
        std::optional<std::string> factory = parse_qualified_identifier(masked, position);
        if (!factory || *factory != "std::to_array") {
            continue;
        }
        if (!consume_character(masked, position, '<')) {
            continue;
        }
        const std::size_t template_begin = position;
        const std::size_t template_end =
            find_matching_delimiter(masked, template_begin - 1, '<', '>');
        if (template_end == std::string_view::npos) {
            continue;
        }
        std::size_t template_position = template_begin;
        std::optional<std::string> element_type = parse_qualified_identifier(
            std::string_view{masked}.substr(0, template_end), template_position);
        skip_whitespace(masked, template_position);
        if (!element_type || *element_type != "std::string_view" ||
            template_position != template_end) {
            continue;
        }
        position = template_end + 1;
        if (!consume_character(masked, position, '(') ||
            !consume_character(masked, position, '{')) {
            continue;
        }
        const std::size_t initializer_begin = position;
        const std::size_t initializer_end =
            find_matching_delimiter(masked, initializer_begin - 1, '{', '}');
        if (initializer_end == std::string_view::npos) {
            continue;
        }
        position = initializer_end + 1;
        if (!consume_character(masked, position, ')') ||
            !consume_character(masked, position, ';')) {
            continue;
        }
        initializer_ranges.emplace_back(initializer_begin, initializer_end);
    }

    if (initializer_ranges.size() != 1) {
        throw std::runtime_error("api_client_fragments argument '" + std::string(variable_name) +
                                 "' must resolve to exactly one same-file "
                                 "std::to_array<std::string_view> initializer in " +
                                 source_path.string());
    }

    const auto [initializer_begin, initializer_end] = initializer_ranges.front();
    std::vector<std::string> references;
    std::set<std::string, std::less<>> unique_references;
    std::size_t element_begin = initializer_begin;
    while (element_begin < initializer_end) {
        while (element_begin < initializer_end && is_ascii_whitespace(masked[element_begin])) {
            ++element_begin;
        }
        if (element_begin == initializer_end) {
            break;
        }
        const std::size_t comma = masked.find(',', element_begin);
        const std::size_t element_end =
            comma == std::string_view::npos || comma > initializer_end ? initializer_end : comma;
        const std::string_view element =
            trim_view(std::string_view{masked}.substr(element_begin, element_end - element_begin));
        std::size_t element_position = 0;
        std::optional<std::string> reference =
            parse_qualified_identifier(element, element_position);
        skip_whitespace(element, element_position);
        if (!reference || !reference->contains("::") || element_position != element.size()) {
            throw std::runtime_error(
                "api_client_fragments array contains a non-qualified constant in " +
                source_path.string());
        }
        if (!unique_references.insert(*reference).second) {
            throw std::runtime_error("api_client_fragments array contains duplicate constant '" +
                                     *reference + "' in " + source_path.string());
        }
        references.push_back(std::move(*reference));
        if (element_end == initializer_end) {
            break;
        }
        element_begin = element_end + 1;
    }
    if (references.empty()) {
        throw std::runtime_error("api_client_fragments array must not be empty in " +
                                 source_path.string());
    }
    return references;
}

/// @brief Decodes an initializer only when it is one complete raw literal.
/// @param initializer Exact initializer spelling without the semicolon.
/// @param symbol Qualified constant name used in diagnostics.
/// @param source_path Declaring partition path.
/// @return Exact bytes inside the raw delimiter.
[[nodiscard]] auto decode_complete_raw_string(std::string_view initializer,
                                              std::string_view symbol,
                                              const fs::path& source_path) -> std::string {
    initializer = trim_view(initializer);
    if (!initializer.starts_with("R\"")) {
        throw std::runtime_error("fragment constant '" + std::string(symbol) +
                                 "' has a non-raw initializer in " + source_path.string());
    }
    const std::size_t delimiter_end = initializer.find('(', 2);
    if (delimiter_end == std::string_view::npos) {
        throw std::runtime_error("fragment constant '" + std::string(symbol) +
                                 "' has a malformed raw delimiter in " + source_path.string());
    }
    const std::string_view delimiter = initializer.substr(2, delimiter_end - 2);
    if (!is_valid_raw_delimiter(delimiter)) {
        throw std::runtime_error("fragment constant '" + std::string(symbol) +
                                 "' has an invalid raw delimiter in " + source_path.string());
    }
    const std::string terminator = ")" + std::string(delimiter) + "\"";
    const std::size_t content_end = initializer.find(terminator, delimiter_end + 1);
    if (content_end == std::string_view::npos ||
        content_end + terminator.size() != initializer.size()) {
        throw std::runtime_error("fragment constant '" + std::string(symbol) +
                                 "' must be one complete raw string literal in " +
                                 source_path.string());
    }
    return std::string(initializer.substr(delimiter_end + 1, content_end - delimiter_end - 1));
}

/// @brief Reads raw string_view constants from exported namespaces.
/// @param source An explicitly imported interface partition.
/// @return Qualified constant names and their string contents.
[[nodiscard]] auto parse_exported_fragment_constants(const descriptor_source& source)
    -> std::map<std::string, std::string, std::less<>> {
    const std::string masked = mask_cpp_non_code(source.text);
    std::map<std::string, std::string, std::less<>> constants;
    std::size_t search_position = 0;
    while (true) {
        const std::size_t export_position = find_word(masked, "export", search_position);
        if (export_position == std::string_view::npos) {
            break;
        }
        std::size_t namespace_position = export_position + std::string_view{"export"}.size();
        if (!consume_word(masked, namespace_position, "namespace")) {
            search_position = namespace_position;
            continue;
        }
        std::optional<std::string> namespace_name =
            parse_qualified_identifier(masked, namespace_position);
        if (!namespace_name || !consume_character(masked, namespace_position, '{')) {
            throw std::runtime_error("invalid exported fragment namespace in " +
                                     source.path.string());
        }
        const std::size_t namespace_begin = namespace_position - 1;
        const std::size_t namespace_end =
            find_matching_delimiter(masked, namespace_begin, '{', '}');
        if (namespace_end == std::string_view::npos) {
            throw std::runtime_error("unterminated exported fragment namespace in " +
                                     source.path.string());
        }

        std::size_t inline_search = namespace_begin + 1;
        while (inline_search < namespace_end) {
            const std::size_t inline_position = find_word(
                std::string_view{masked}.substr(0, namespace_end), "inline", inline_search);
            if (inline_position == std::string_view::npos) {
                break;
            }
            inline_search = inline_position + std::string_view{"inline"}.size();
            std::size_t declaration_position = inline_position;
            if (!consume_word(masked, declaration_position, "inline") ||
                !consume_word(masked, declaration_position, "constexpr")) {
                continue;
            }
            std::optional<std::string> value_type =
                parse_qualified_identifier(masked, declaration_position);
            if (!value_type || *value_type != "std::string_view") {
                continue;
            }
            std::optional<std::string> variable_name =
                parse_identifier(masked, declaration_position);
            if (!variable_name || !consume_character(masked, declaration_position, '=')) {
                continue;
            }
            const std::size_t initializer_begin = declaration_position;
            const std::size_t initializer_end = masked.find(';', initializer_begin);
            if (initializer_end == std::string_view::npos || initializer_end > namespace_end) {
                throw std::runtime_error("unterminated fragment constant '" + *variable_name +
                                         "' in " + source.path.string());
            }
            const std::string qualified_name = *namespace_name + "::" + *variable_name;
            const std::string value = decode_complete_raw_string(
                std::string_view{source.text}.substr(initializer_begin,
                                                     initializer_end - initializer_begin),
                qualified_name,
                source.path);
            if (!constants.emplace(qualified_name, value).second) {
                throw std::runtime_error("duplicate exported fragment constant '" + qualified_name +
                                         "' in " + source.path.string());
            }
            inline_search = initializer_end + 1;
        }
        search_position = namespace_end + 1;
    }
    return constants;
}

[[nodiscard]] auto source_has_registered_annotation(std::string_view source,
                                                    const annotation_registry& registry) -> bool {
    return std::ranges::any_of(registry, [source](const auto& registry_entry) -> bool {
        return count_attribute_invocations(source, registry_entry.first) != 0;
    });
}

struct resolved_fragment_specification {
    std::string specification;
    std::vector<fs::path> dependencies;
};

/// @brief Joins specification fragments from explicitly imported partitions.
/// @param source_path Primary module path used in errors.
/// @param source Primary module source text
/// @param fragment_argument Local array named by api_client_fragments.
/// @param providers Files exporting each module naie.
/// @param registry Client annotations forbidden in fragment files.
/// @return Joined fragment text and source paths in first-use order.
/// @details Every  fragment must have one definition, in the primary module's
/// descriptor namespace. Missing imports, duplicate definitions and nested
/// client annotations are errors.
[[nodiscard]] auto resolve_fragment_specification(const fs::path& source_path,
                                                  std::string_view source,
                                                  std::string_view fragment_argument,
                                                  const module_provider_index& providers,
                                                  const annotation_registry& registry)
    -> resolved_fragment_specification {
    const std::optional<std::string> primary_module =
        parse_exported_module_name(source, source_path);
    if (!primary_module) {
        throw std::runtime_error("api_client_fragments requires an exported primary module in " +
                                 source_path.string());
    }
    const auto primary_provider = providers.find(*primary_module);
    if (primary_provider == providers.end() || primary_provider->second.size() != 1) {
        throw std::runtime_error("ambiguous module provider for primary module '" +
                                 *primary_module + "' in " + source_path.string());
    }

    const std::vector<std::string> references =
        parse_fragment_references(source, fragment_argument, source_path);
    std::string expected_namespace;
    expected_namespace.reserve(primary_module->size() + std::string_view{"::descriptor::"}.size());
    for (const char character : *primary_module) {
        if (character == '.') {
            expected_namespace += "::";
        } else {
            expected_namespace += character;
        }
    }
    expected_namespace += "::descriptor::";
    for (const std::string& reference : references) {
        if (!reference.starts_with(expected_namespace)) {
            throw std::runtime_error("foreign fragment constant '" + reference + "' in " +
                                     source_path.string());
        }
    }
    const std::set<std::string, std::less<>> imported_modules =
        parse_imported_modules(source, *primary_module, source_path);

    struct resolved_constant {
        const descriptor_source* source{};
        std::string value;
    };
    std::map<std::string, std::vector<resolved_constant>, std::less<>> constants;
    std::size_t imported_partition_count = 0;
    const std::string partition_prefix = *primary_module + ":";
    for (const std::string& imported_module : imported_modules) {
        if (!imported_module.starts_with(partition_prefix)) {
            continue;
        }
        ++imported_partition_count;
        const auto provider_position = providers.find(imported_module);
        if (provider_position == providers.end()) {
            throw std::runtime_error("explicitly imported fragment partition '" + imported_module +
                                     "' has no module provider for " + source_path.string());
        }
        if (provider_position->second.size() != 1) {
            throw std::runtime_error("ambiguous module provider for imported fragment partition '" +
                                     imported_module + "' in " + source_path.string());
        }
        const descriptor_source& provider = *provider_position->second.front();
        if (source_has_registered_annotation(provider.text, registry)) {
            throw std::runtime_error("fragment unit registers api_client: " +
                                     provider.path.string());
        }
        for (auto& [name, value] : parse_exported_fragment_constants(provider)) {
            constants[name].push_back(resolved_constant{
                .source = &provider,
                .value = std::move(value),
            });
        }
    }
    if (imported_partition_count == 0) {
        throw std::runtime_error(
            "api_client_fragments requires explicitly imported partitions in " +
            source_path.string());
    }

    resolved_fragment_specification resolved;
    std::set<std::string, std::less<>> dependency_paths;
    for (const std::string& reference : references) {
        const auto constant_position = constants.find(reference);
        if (constant_position == constants.end()) {
            throw std::runtime_error(
                "fragment constant '" + reference +
                "' is missing or belongs to a partition not explicitly imported by " +
                source_path.string());
        }
        if (constant_position->second.size() != 1) {
            throw std::runtime_error("fragment constant '" + reference +
                                     "' is ambiguous across explicitly imported partitions for " +
                                     source_path.string());
        }
        const resolved_constant& constant = constant_position->second.front();
        resolved.specification += constant.value;
        const std::string dependency_key = constant.source->path.generic_string();
        if (dependency_paths.insert(dependency_key).second) {
            resolved.dependencies.push_back(constant.source->path);
        }
    }
    return resolved;
}

[[nodiscard]] auto bioseq_attribute_begin(std::string_view masked, std::size_t opening) noexcept
    -> bool {
    if (!masked.substr(opening).starts_with("[[")) {
        return false;
    }
    std::size_t name_begin = opening + 2;
    skip_whitespace(masked, name_begin);
    return masked.substr(name_begin).starts_with("bioseq::");
}

/// @brief Moves attributes after struct so cppast attaches them to the class.
/// @param source Module source text.
/// @return Source with Bioseq-annotated exported structs prepared for parsing.
[[nodiscard]] auto normalize_annotation_declarations(std::string_view source) -> std::string {
    const std::string masked = mask_cpp_non_code(source);
    std::string normalized;
    normalized.reserve(source.size());
    std::size_t copied_through = 0;
    std::size_t search_position = 0;
    while (true) {
        const std::size_t attribute_begin = masked.find("[[", search_position);
        if (attribute_begin == std::string_view::npos) {
            break;
        }
        if (!bioseq_attribute_begin(masked, attribute_begin)) {
            search_position = attribute_begin + 2;
            continue;
        }

        std::size_t attributes_end = masked.find("]]", attribute_begin + 2);
        if (attributes_end == std::string_view::npos) {
            break;
        }
        attributes_end += 2;
        std::size_t declaration_begin = attributes_end;
        while (true) {
            skip_whitespace(masked, declaration_begin);
            if (!masked.substr(declaration_begin).starts_with("[[")) {
                break;
            }
            const std::size_t attribute_end = masked.find("]]", declaration_begin + 2);
            if (attribute_end == std::string_view::npos) {
                break;
            }
            attributes_end = attribute_end + 2;
            declaration_begin = attributes_end;
        }
        skip_whitespace(masked, declaration_begin);
        std::size_t struct_position = declaration_begin;
        if (!consume_word(masked, struct_position, "export") ||
            !consume_word(masked, struct_position, "struct")) {
            search_position = attributes_end;
            continue;
        }

        normalized.append(source.substr(copied_through, attribute_begin - copied_through));
        normalized += "struct ";
        normalized.append(source.substr(attribute_begin, attributes_end - attribute_begin));
        normalized.append(source.substr(attributes_end, declaration_begin - attributes_end));
        copied_through = struct_position;
        search_position = struct_position;
    }
    normalized.append(source.substr(copied_through));
    return normalized;
}

} // namespace

/// @brief Lists the source files that export each module name.
/// @param sources Discovered descriptor sources.
/// @return Module names and all matching files, including duplicate declarations.
[[nodiscard]] auto build_module_provider_index(std::span<const descriptor_source> sources)
    -> module_provider_index {
    module_provider_index providers;
    for (const descriptor_source& source : sources) {
        const std::optional<std::string> module_name =
            parse_exported_module_name(source.text, source.path);
        if (module_name) {
            providers[*module_name].push_back(&source);
        }
    }
    return providers;
}

/// @brief Resolves the specification fragment array without running cppast.
/// @param source_path Primary descriptor path used in errors.
/// @param source Primary descriptor source text.
/// @param fragment_argument Local array named by api_client_fragments.
/// @param providers Files exporting the imported modules.
/// @param registry Registered  client annotation handlers.
/// @return Joined fragment text and source paths in first-use order.
[[nodiscard]] auto resolve_api_client_fragment_contract(const fs::path& source_path,
                                                        std::string_view source,
                                                        std::string_view fragment_argument,
                                                        const module_provider_index& providers,
                                                        const annotation_registry& registry)
    -> std::pair<std::string, std::vector<fs::path>> {
    resolved_fragment_specification resolved =
        resolve_fragment_specification(source_path, source, fragment_argument, providers, registry);
    return {
        std::move(resolved.specification),
        std::move(resolved.dependencies),
    };
}

/// @brief Detects a registered source annotation outside comments and literals.
/// @param source Exact module source bytes.
/// @param registry Registered annotation processors.
/// @return True when one registered attribute invocation occurs in authored code.
[[nodiscard]] auto has_registered_annotation(std::string_view source,
                                             const annotation_registry& registry) -> bool {
    return source_has_registered_annotation(source, registry);
}

/// @brief Checks for api_client_fragments outside comments and literals.
/// @param source Module source text.
/// @return True if the attribute is present.
[[nodiscard]] auto has_fragment_annotation(std::string_view source) -> bool {
    return count_attribute_invocations(source, fragment_annotation_name) != 0;
}

/// @brief Prepares a module's annotated declaration for cppast.
/// @param source Module source text.
/// @return C++ source without module syntax, keeping both client attributes.
/// @details Fragment-based clients need only the attributed struct declaratjon
/// here. Their specification text is resolved separately from imported files.
[[nodiscard]] auto prepare_annotation_parser_translation_unit(std::string_view source)
    -> std::string {
    std::string normalized = normalize_annotation_declarations(source);
    if (!has_fragment_annotation(source)) {
        return prepare_clang_translation_unit(normalized);
    }

    const std::string masked = mask_cpp_non_code(normalized);
    const std::size_t fragment_position = masked.find(fragment_annotation_name);
    const std::size_t struct_position = masked.rfind("struct", fragment_position);
    const std::size_t declaration_end = masked.find(';', fragment_position);
    if (fragment_position == std::string_view::npos || struct_position == std::string_view::npos ||
        declaration_end == std::string_view::npos) {
        throw std::runtime_error("cannot isolate api_client_fragments declaration for cppast");
    }
    return normalized.substr(struct_position, declaration_end - struct_position + 1) + '\n';
}

/// @brief Extracts source text from a supported cppast expression.
/// @param expression Parsed initializer expression.
/// @return Exact expression spelling saved by cppast.
/// @throws std::runtime_error When cppast exposes an unsupported expression kind.
[[nodiscard]] auto extract_expression_source(const cppast::cpp_expression& expression)
    -> std::string {
    if (expression.kind() == cppast::cpp_expression_kind::literal_t) {
        return static_cast<const cppast::cpp_literal_expression&>(expression).value();
    }
    if (expression.kind() == cppast::cpp_expression_kind::unexposed_t) {
        return static_cast<const cppast::cpp_unexposed_expression&>(expression)
            .expression()
            .as_string();
    }
    throw std::runtime_error("unsupported cppast expression kind");
}

/// @brief Uses cppast to  find one client annotation and read its API specification.
/// @param source_path Original descriptor path used in errors.
/// @param source_text Primary module source text
/// @param parser_source_path Copy prepared for cppast.
/// @param clang_binary Clang executable used by cppast.
/// @param registry Registered annotation handlers.
/// @param providers Files exporting imported modules.
/// @return Annotation, specification text, and any imported fragment dependencies.
/// @throws std::runtime_error If parsing fails or the annotation cannot be resolved uniquely.
[[nodiscard]] auto parse_source_annotation(const fs::path& source_path,
                                           std::string_view source_text,
                                           const fs::path& parser_source_path,
                                           const std::string& clang_binary,
                                           const annotation_registry& registry,
                                           const module_provider_index& providers)
    -> annotation_request {
    std::size_t lexical_annotation_count = 0;
    for (const auto& registry_entry : registry) {
        lexical_annotation_count += count_attribute_invocations(source_text, registry_entry.first);
    }
    if (lexical_annotation_count != 1) {
        throw std::runtime_error(source_path.string() +
                                 " must contain exactly one registered Bioseq annotation");
    }

    cppast::cpp_entity_index entity_index;
    cppast::libclang_compile_config compile_config(clang_binary);
    compile_config.set_flags(cppast::cpp_standard::cpp_20);
    compile_config.fast_preprocessing(false);

    cppast::libclang_parser parser;
    auto parsed_file = parser.parse(entity_index, parser_source_path.string(), compile_config);
    if (!parsed_file || parser.error()) {
        throw std::runtime_error("cppast/libclang failed to parse " + source_path.string());
    }

    std::map<std::string, std::string, std::less<>> initializers;
    const std::string cppast_fragment_annotation_name(fragment_annotation_name);

    /// @brief One annotated class discovered during source traversal.
    struct annotation_match {
        /// @brief Registered attribute spelling.
        std::string annotation_name;
        /// @brief Annotated class name.
        std::string declared_type;
        /// @brief Attribute argument expression spelling.
        std::string argument_expression;
        /// @brief Fragment-array argument, if this class has api_client_fragments.
        std::optional<std::string> fragment_argument_expression;
    };

    std::vector<annotation_match> annotation_matches;
    std::size_t fragment_entity_count = 0;
    cppast::visit(*parsed_file,
                  [&initializers,
                   &annotation_matches,
                   &fragment_entity_count,
                   &registry,
                   &cppast_fragment_annotation_name](const cppast::cpp_entity& entity,
                                                     cppast::visitor_info visitor_state) -> void {
                      if (!visitor_state.is_new_entity()) {
                          return;
                      }

                      if (entity.kind() == cppast::cpp_entity_kind::variable_t) {
                          const auto& variable = static_cast<const cppast::cpp_variable&>(entity);
                          if (const auto initializer = variable.default_value()) {
                              initializers.emplace(variable.name(),
                                                   extract_expression_source(initializer.value()));
                          }
                      }

                      if (entity.kind() != cppast::cpp_entity_kind::class_t) {
                          return;
                      }
                      const auto fragment_attribute =
                          cppast::has_attribute(entity, cppast_fragment_annotation_name);
                      std::optional<std::string> fragment_argument;
                      if (fragment_attribute) {
                          ++fragment_entity_count;
                          if (!fragment_attribute.value().arguments()) {
                              throw std::runtime_error(std::string(fragment_annotation_name) +
                                                       " requires one argument");
                          }
                          fragment_argument = trim_ascii_whitespace(
                              fragment_attribute.value().arguments().value().as_string());
                      }

                      for (const auto& registry_entry : registry) {
                          const std::string& annotation_name = registry_entry.first;
                          const auto attribute = cppast::has_attribute(entity, annotation_name);
                          if (!attribute) {
                              continue;
                          }
                          if (!attribute.value().arguments()) {
                              throw std::runtime_error(annotation_name + " requires one argument");
                          }
                          annotation_matches.push_back(annotation_match{
                              .annotation_name = annotation_name,
                              .declared_type = entity.name(),
                              .argument_expression = trim_ascii_whitespace(
                                  attribute.value().arguments().value().as_string()),
                              .fragment_argument_expression = fragment_argument,
                          });
                      }
                  });

    if (annotation_matches.size() != 1) {
        throw std::runtime_error(source_path.string() +
                                 " must contain exactly one registered Bioseq annotation");
    }

    const annotation_match& match = annotation_matches.front();
    const std::size_t lexical_fragment_count =
        count_attribute_invocations(source_text, fragment_annotation_name);
    if (lexical_fragment_count > 1 || fragment_entity_count > 1 ||
        lexical_fragment_count != fragment_entity_count ||
        (lexical_fragment_count == 1 && !match.fragment_argument_expression)) {
        throw std::runtime_error(source_path.string() +
                                 " must contain at most one api_client_fragments attribute on the "
                                 "api_client declaration");
    }

    std::string specification;
    std::vector<fs::path> dependencies;
    if (match.fragment_argument_expression) {
        resolved_fragment_specification resolved = resolve_fragment_specification(
            source_path, source_text, *match.fragment_argument_expression, providers, registry);
        specification = std::move(resolved.specification);
        dependencies = std::move(resolved.dependencies);
    } else {
        const auto initializer = initializers.find(match.argument_expression);
        if (initializer == initializers.end()) {
            throw std::runtime_error("cannot resolve annotation argument '" +
                                     match.argument_expression + "' in " + source_path.string());
        }
        specification = decode_cpp_raw_string(initializer->second);
    }

    return annotation_request{
        .source_path = source_path,
        .parser_source_path = parser_source_path,
        .annotation_name = match.annotation_name,
        .declared_type = match.declared_type,
        .argument_expression = match.argument_expression,
        .specification = std::move(specification),
        .specification_dependencies = std::move(dependencies),
    };
}

} // namespace bioseq::codegen::external
