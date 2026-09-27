module;

#include <cstddef>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.codegen.support:source_text;

export namespace bioseq::codegen {

/// @brief Removes surrounding spaces, tabs, and line breaks.
/// @param value Text to trim.
/// @return Trimmed copy.
[[nodiscard]] std::string trim_ascii_whitespace(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

/// @brief Finds a bioseq:: attribute, allowing spaces or tabs after [[.
/// @param line Source line to inspect.
/// @return Attribute offset or `std::string_view::npos`.
[[nodiscard]] std::size_t find_bioseq_attribute(std::string_view line) noexcept {
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

/// @brief Adapts module source for the external-annotation Clang parser.
///
/// Removes module/import lines, moves Bioseq struct attributes into their
/// accepted position, and keeps raw-string contents intact.
/// @param source Original module source.
/// @return Source text for the parser.
[[nodiscard]] std::string prepare_clang_translation_unit(
    std::string_view source) {
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
        const std::size_t raw_prefix = line.find("R\"");
        const std::size_t delimiter_end = raw_prefix == std::string::npos
            ? std::string::npos : line.find('(', raw_prefix + 2);
        if (delimiter_end != std::string::npos) {
            const std::string delimiter = line.substr(
                raw_prefix + 2, delimiter_end - raw_prefix - 2);
            const std::string terminator = ")" + delimiter + "\"";
            const bool closes_on_line =
                line.find(terminator, delimiter_end + 1) != std::string::npos;
            const std::size_t constexpr_position = line.find("constexpr");
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
        const std::string stripped = trim_ascii_whitespace(line);
        if (stripped == "module;" || stripped.starts_with("export module ")
            || stripped.starts_with("import ") || stripped.starts_with("export import ")) {
            continue;
        }
        const std::size_t attribute_begin = find_bioseq_attribute(line);
        if (attribute_begin != std::string::npos) {
            std::size_t position = 0;
            while ((position = line.find("export struct", position)) != std::string::npos) {
                line.erase(position, std::string_view{"export "}.size());
                position += std::string_view{"struct"}.size();
            }
            const std::size_t attribute_end = line.find("]]", attribute_begin);
            std::size_t struct_position = attribute_end == std::string::npos
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

/// @brief Extracts the contents of a C++ raw string initializer.
/// @param expression Raw string expression spelling.
/// @return Text  between the raw delimiters.
[[nodiscard]] std::string decode_cpp_raw_string(std::string_view expression) {
    const std::size_t prefix = expression.find("R\"");
    if (prefix == std::string_view::npos) {
        throw std::runtime_error("OpenAPI spec initializer is not a raw string literal");
    }
    const std::size_t delimiter_end = expression.find('(', prefix + 2);
    if (delimiter_end == std::string_view::npos) {
        throw std::runtime_error("malformed raw string delimiter");
    }
    const std::string delimiter(expression.substr(prefix + 2, delimiter_end - prefix - 2));
    const std::string terminator = ")" + delimiter + "\"";
    const std::size_t content_end = expression.rfind(terminator);
    if (content_end == std::string_view::npos || content_end < delimiter_end) {
        throw std::runtime_error("unterminated OpenAPI raw string literal");
    }
    return std::string(expression.substr(delimiter_end + 1, content_end - delimiter_end - 1));
}

/// @brief Replaces characters that cannot appear in a generated C++ identifier.
/// @param value Text to convert.
/// @return Letters, digits, and underscores, with a leading underscore if needed.
[[nodiscard]] std::string make_cpp_identifier(std::string_view value) {
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

/// @brief Escapes text for a generated C++ string literal.
/// @param value Unescaped text.
/// @return C++-escaped spelling without surrounding quotes.
[[nodiscard]] std::string escape_cpp_string_literal(std::string_view value) {
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

}  // namespace bioseq::codegen
