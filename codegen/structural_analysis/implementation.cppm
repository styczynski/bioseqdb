module;

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Comment.h>
#include <clang/AST/CommentCommandTraits.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Mangle.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RawCommentList.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/Linkage.h>
#include <clang/Basic/Module.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/Specifiers.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/DependencyScanningTool.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/Allocator.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/StringSaver.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module bioseq.codegen.structural_analysis;

import :model;
import bioseq.codegen.support;

namespace bioseq::codegen::structural_analysis {

namespace detail {

struct comment_record {
    std::string text;
    bool doxygen{};
    source_range range;
};

[[nodiscard]] inline bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n'
        || value == '\r' || value == '\f' || value == '\v';
}

[[nodiscard]] inline bool identifier_begin(char value) noexcept {
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] inline bool identifier_continue(char value) noexcept {
    return identifier_begin(value) || (value >= '0' && value <= '9');
}

[[nodiscard]] inline bool source_has_structural_marker(
    std::string_view source) noexcept {
    return source.contains("bioseq_sql")
        || source.contains("bioseq::sql")
        // Exact spelling is confirmed after lexical attribute parsing. Keep
        // this cheap front-door tolerant of whitespace/comments between the
        // qualified-name components.
        || (source.contains("bioseq") && source.contains("simd"))
        || source.contains(".sql_rows")
        || source.contains("relation_entity_handle")
        || source.contains("guc_registration_access")
        || source.contains("postgres_enum_registration")
        || source.contains("CXX_PG_FUNCTION_INFO_V1")
        || source.contains("PG_FUNCTION_INFO_V1")
        || source.contains("POSTGRES_FN")
        || source.contains("E_POSTGRES_FN");
}

[[nodiscard]] inline bool is_sql_attribute(
    const attribute_record& attribute) noexcept {
    return attribute.vendor_namespace == "bioseq_sql"
        || attribute.vendor_namespace == "bioseq::sql";
}

[[nodiscard]] inline bool is_simd_namespace_marker(
    const attribute_record& attribute) noexcept {
    return attribute.qualified_name == "bioseq::simd"
        && attribute.vendor_namespace == "bioseq"
        && attribute.name == "simd";
}

[[nodiscard]] inline bool is_structural_attribute(
    const attribute_record& attribute) noexcept {
    return is_sql_attribute(attribute)
        || is_simd_namespace_marker(attribute);
}

[[nodiscard]] inline bool is_materialization_attribute(
    const attribute_record& attribute) noexcept {
    return attribute.vendor_namespace == "bioseq"
        && (attribute.name == "template_flatten"
            || attribute.name == "template_flatten_when"
            || attribute.name == "template_flatten_entry");
}

[[nodiscard]] inline bool is_probe_source_identity_attribute(
    const attribute_record& attribute) noexcept {
    return attribute.qualified_name
        == "bioseq::template_flatten_provenance";
}

[[nodiscard]] inline std::string_view trim_view(std::string_view text) noexcept {
    while (!text.empty() && ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

class source_positions {
public:
    explicit source_positions(std::string_view source) {
        line_begins_.reserve(
            1 + static_cast<std::size_t>(
                    std::ranges::count(source, '\n')));
        line_begins_.push_back(0);
        for (std::size_t offset = 0; offset < source.size(); ++offset) {
            if (source[offset] == '\n') {
                line_begins_.push_back(offset + 1);
            }
        }
    }

    [[nodiscard]] source_position at(std::size_t offset) const noexcept {
        const auto next = std::ranges::upper_bound(line_begins_, offset);
        const std::size_t line_index = static_cast<std::size_t>(
            std::distance(line_begins_.begin(), next) - 1);
        return {
            .byte_offset = offset,
            .line = line_index + 1,
            .column = offset - line_begins_[line_index] + 1,
        };
    }

    [[nodiscard]] source_range range(
        std::size_t begin,
        std::size_t end) const noexcept {
        return {
            .begin = at(begin),
            .end = at(end),
        };
    }

private:
    std::vector<std::size_t> line_begins_;
};

[[nodiscard]] inline std::optional<std::size_t> raw_string_end(
    std::string_view source,
    std::size_t offset) {
    static constexpr std::string_view prefixes[] = {
        "R\"", "u8R\"", "uR\"", "UR\"", "LR\""};
    std::size_t quote = std::string_view::npos;
    for (const std::string_view prefix : prefixes) {
        if (source.substr(offset).starts_with(prefix)) {
            quote = offset + prefix.size() - 1;
            break;
        }
    }
    if (quote == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t open = source.find('(', quote + 1);
    if (open == std::string_view::npos || open - quote - 1 > 16) {
        return std::nullopt;
    }
    const std::string delimiter{
        source.substr(quote + 1, open - quote - 1)};
    if (delimiter.find_first_of(" ()\\\t\r\n") != std::string::npos) {
        return std::nullopt;
    }
    const std::string close = ")" + delimiter + "\"";
    const std::size_t close_offset = source.find(close, open + 1);
    if (close_offset == std::string_view::npos) {
        throw std::runtime_error("unterminated C++ raw string literal");
    }
    return close_offset + close.size();
}

[[nodiscard]] inline std::pair<
    std::vector<lexical_token>,
    std::vector<comment_record>>
lex_source(std::string_view source) {
    std::vector<lexical_token> tokens;
    std::vector<comment_record> comments;
    const source_positions positions{source};
    std::size_t index = 0;
    while (index < source.size()) {
        if (ascii_space(source[index])) {
            ++index;
            continue;
        }
        if (source.substr(index).starts_with("//")) {
            const std::size_t end = source.find('\n', index + 2);
            const std::size_t bounded_end =
                end == std::string_view::npos ? source.size() : end;
            comments.push_back(comment_record{
                .text = std::string{source.substr(
                    index, bounded_end - index)},
                .doxygen = source.substr(index).starts_with("///")
                    || source.substr(index).starts_with("//!"),
                .range = positions.range(index, bounded_end),
            });
            index = bounded_end;
            continue;
        }
        if (source.substr(index).starts_with("/*")) {
            const std::size_t close = source.find("*/", index + 2);
            if (close == std::string_view::npos) {
                throw std::runtime_error("unterminated C++ block comment");
            }
            const std::size_t end = close + 2;
            comments.push_back(comment_record{
                .text = std::string{source.substr(index, end - index)},
                .doxygen = source.substr(index).starts_with("/**")
                    || source.substr(index).starts_with("/*!"),
                .range = positions.range(index, end),
            });
            index = end;
            continue;
        }
        if (const auto end = raw_string_end(source, index)) {
            tokens.push_back(lexical_token{
                .spelling = std::string{source.substr(index, *end - index)},
                .range = positions.range(index, *end),
            });
            index = *end;
            continue;
        }
        if (source[index] == '"' || source[index] == '\'') {
            const char quote = source[index];
            const std::size_t begin = index++;
            bool escaped = false;
            while (index < source.size()) {
                const char value = source[index++];
                if (escaped) {
                    escaped = false;
                } else if (value == '\\') {
                    escaped = true;
                } else if (value == quote) {
                    break;
                }
            }
            if (source[index - 1] != quote) {
                throw std::runtime_error("unterminated C++ quoted literal");
            }
            tokens.push_back(lexical_token{
                .spelling =
                    std::string{source.substr(begin, index - begin)},
                .range = positions.range(begin, index),
            });
            continue;
        }
        if (identifier_begin(source[index])) {
            const std::size_t begin = index++;
            while (index < source.size()
                   && identifier_continue(source[index])) {
                ++index;
            }
            tokens.push_back(lexical_token{
                .spelling =
                    std::string{source.substr(begin, index - begin)},
                .range = positions.range(begin, index),
            });
            continue;
        }
        if (source[index] >= '0' && source[index] <= '9') {
            const std::size_t begin = index++;
            while (index < source.size()
                   && (identifier_continue(source[index])
                       || source[index] == '.'
                       || source[index] == '\'')) {
                ++index;
            }
            tokens.push_back(lexical_token{
                .spelling =
                    std::string{source.substr(begin, index - begin)},
                .range = positions.range(begin, index),
            });
            continue;
        }
        const std::size_t begin = index;
        static constexpr std::string_view multi[] = {
            "::", "->", "&&", "||", "==", "!=", "<=>", "<=", ">=",
            "...", "<<", ">>", "++", "--"};
        bool matched = false;
        for (const std::string_view punctuation : multi) {
            if (source.substr(index).starts_with(punctuation)) {
                index += punctuation.size();
                tokens.push_back(lexical_token{
                    .spelling = std::string{punctuation},
                    .range = positions.range(begin, index),
                });
                matched = true;
                break;
            }
        }
        if (!matched) {
            ++index;
            tokens.push_back(lexical_token{
                .spelling = std::string{source.substr(begin, 1)},
                .range = positions.range(begin, index),
            });
        }
    }
    return {std::move(tokens), std::move(comments)};
}

/// Tokenizes a source buffer with Clang's raw lexer only.  This deliberately
/// constructs no CompilerInstance or Preprocessor, so callers cannot
/// accidentally run Sema or compile a body-bearing generator input.
[[nodiscard]] inline std::vector<lexical_token> clang_raw_tokens(
    const std::filesystem::path& source_path,
    std::string_view source) {
    auto diagnostic_ids =
        llvm::makeIntrusiveRefCnt<clang::DiagnosticIDs>();
    clang::DiagnosticOptions diagnostic_options;
    clang::IgnoringDiagConsumer diagnostic_consumer;
    clang::DiagnosticsEngine diagnostics{
        std::move(diagnostic_ids),
        diagnostic_options,
        &diagnostic_consumer,
        false};
    clang::FileSystemOptions file_system_options;
    clang::FileManager file_manager{file_system_options};
    clang::SourceManager sources{diagnostics, file_manager};
    const clang::FileID file_id = sources.createFileID(
        llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef{source.data(), source.size()},
            source_path.string()));
    const llvm::MemoryBufferRef buffer =
        sources.getBufferOrFake(file_id);
    clang::LangOptions language;
    language.CPlusPlus = true;
    language.CPlusPlus11 = true;
    language.CPlusPlus14 = true;
    language.CPlusPlus17 = true;
    language.CPlusPlus20 = true;
    language.CPlusPlus23 = true;
    language.CPlusPlus26 = true;
    language.CPlusPlusModules = true;
    language.LineComment = true;
    clang::Lexer lexer{file_id, buffer, sources, language};
    lexer.SetCommentRetentionState(false);

    const source_positions positions{source};
    std::vector<lexical_token> result;
    clang::Token token;
    while (true) {
        const bool end = lexer.LexFromRawLexer(token);
        if (token.is(clang::tok::eof)) {
            break;
        }
        if (token.is(clang::tok::comment)) {
            continue;
        }
        bool invalid = false;
        const std::string spelling = clang::Lexer::getSpelling(
            token, sources, language, &invalid);
        if (invalid || token.getLocation().isInvalid()) {
            throw std::runtime_error(
                source_path.string()
                + ": Clang raw lexer produced an invalid token");
        }
        const std::size_t begin =
            sources.getFileOffset(token.getLocation());
        const std::size_t end_offset = begin + token.getLength();
        if (end_offset > source.size()) {
            throw std::runtime_error(
                source_path.string()
                + ": Clang raw token escapes its source buffer");
        }
        result.push_back({
            .spelling = spelling,
            .range = positions.range(begin, end_offset),
        });
        // LexFromRawLexer reports whether its next-character pointer reached
        // the buffer end. The token returned by that call is still valid, so
        // retain it before stopping (notably a final `}` without a newline).
        if (end) break;
    }
    return result;
}

[[nodiscard]] inline bool adjacent(
    const lexical_token& left,
    const lexical_token& right) noexcept {
    return left.range.end.byte_offset == right.range.begin.byte_offset;
}

[[nodiscard]] inline bool attribute_open(
    const std::vector<lexical_token>& tokens,
    std::size_t index) noexcept {
    return index + 1 < tokens.size() && tokens[index].spelling == "["
        && tokens[index + 1].spelling == "["
        && adjacent(tokens[index], tokens[index + 1]);
}

[[nodiscard]] inline bool attribute_close(
    const std::vector<lexical_token>& tokens,
    std::size_t index) noexcept {
    return index + 1 < tokens.size() && tokens[index].spelling == "]"
        && tokens[index + 1].spelling == "]"
        && adjacent(tokens[index], tokens[index + 1]);
}

struct attribute_block {
    std::size_t token_begin{};
    std::size_t token_end{};
    source_range range;
    std::vector<attribute_record> attributes;
};

[[nodiscard]] inline std::vector<attribute_record> parse_attribute_segments(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    std::size_t begin,
    std::size_t end,
    source_range block_range) {
    std::vector<std::pair<std::size_t, std::size_t>> segments;
    std::size_t segment_begin = begin;
    int parentheses = 0;
    int braces = 0;
    int brackets = 0;
    for (std::size_t index = begin; index < end; ++index) {
        const std::string_view token = tokens[index].spelling;
        if (token == "(") ++parentheses;
        else if (token == ")") --parentheses;
        else if (token == "{") ++braces;
        else if (token == "}") --braces;
        else if (token == "[") ++brackets;
        else if (token == "]") --brackets;
        else if (token == "," && parentheses == 0 && braces == 0
                 && brackets == 0) {
            segments.emplace_back(segment_begin, index);
            segment_begin = index + 1;
        }
    }
    segments.emplace_back(segment_begin, end);

    std::vector<attribute_record> result;
    for (std::size_t segment_index = 0;
         segment_index < segments.size();
         ++segment_index) {
        const auto [first, last] = segments[segment_index];
        if (first >= last) {
            continue;
        }
        std::size_t name_end = first;
        while (name_end < last && tokens[name_end].spelling != "(") {
            ++name_end;
        }
        std::string qualified;
        for (std::size_t index = first; index < name_end; ++index) {
            qualified += tokens[index].spelling;
        }
        const std::size_t separator = qualified.rfind("::");
        const std::string vendor = separator == std::string::npos
            ? std::string{}
            : qualified.substr(0, separator);
        const std::string name = separator == std::string::npos
            ? qualified
            : qualified.substr(separator + 2);
        std::string argument;
        source_range argument_range{};
        if (name_end < last && tokens[name_end].spelling == "(") {
            std::size_t close = last;
            if (last > name_end + 1 && tokens[last - 1].spelling == ")") {
                close = last - 1;
            }
            if (close > name_end + 1) {
                const std::size_t byte_begin =
                    tokens[name_end + 1].range.begin.byte_offset;
                const std::size_t byte_end =
                    tokens[close - 1].range.end.byte_offset;
                argument = std::string{
                    source.substr(byte_begin, byte_end - byte_begin)};
                argument_range = {
                    .begin = tokens[name_end + 1].range.begin,
                    .end = tokens[close - 1].range.end,
                };
            }
        }
        source_range complete_range = block_range;
        if (segments.size() != 1) {
            // A segment in a shared attribute-specifier is removable only
            // together with one adjacent comma. The first segment owns the
            // following comma; all later segments own the preceding comma.
            // Policy modes still reject multiple bioseq materialization
            // markers in one block so independently planned removals cannot
            // overlap on that separator.
            if (segment_index == 0) {
                complete_range = {
                    .begin = tokens[first].range.begin,
                    .end = tokens[last].range.end,
                };
            } else {
                complete_range = {
                    .begin = tokens[first - 1].range.begin,
                    .end = tokens[last - 1].range.end,
                };
            }
        }
        result.push_back(attribute_record{
            .qualified_name = std::move(qualified),
            .vendor_namespace = vendor,
            .name = name,
            .argument_text = std::move(argument),
            .spelling_range = {
                .begin = tokens[first].range.begin,
                .end = tokens[last - 1].range.end,
            },
            .argument_range = argument_range,
            .removal_range = complete_range,
        });
    }
    return result;
}

[[nodiscard]] inline std::vector<attribute_block> collect_attribute_blocks(
    std::string_view source,
    const std::vector<lexical_token>& tokens) {
    std::vector<attribute_block> result;
    for (std::size_t index = 0; index + 1 < tokens.size(); ++index) {
        if (!attribute_open(tokens, index)) {
            continue;
        }
        const std::size_t token_begin = index;
        std::size_t cursor = index + 2;
        int nested = 0;
        for (; cursor + 1 < tokens.size(); ++cursor) {
            if (attribute_open(tokens, cursor)) {
                ++nested;
                ++cursor;
                continue;
            }
            if (attribute_close(tokens, cursor)) {
                if (nested == 0) {
                    break;
                }
                --nested;
                ++cursor;
            }
        }
        if (cursor + 1 >= tokens.size()) {
            throw std::runtime_error("unterminated C++ attribute specifier");
        }
        const source_range range{
            .begin = tokens[token_begin].range.begin,
            .end = tokens[cursor + 1].range.end,
        };
        result.push_back(attribute_block{
            .token_begin = token_begin,
            .token_end = cursor + 2,
            .range = range,
            .attributes = parse_attribute_segments(
                source, tokens, token_begin + 2, cursor, range),
        });
        index = cursor + 1;
    }
    return result;
}

[[nodiscard]] inline std::string decode_ordinary_narrow_string(
    std::string_view token) {
    if (token.size() < 2 || token.front() != '"'
        || token.back() != '"') {
        throw std::runtime_error(
            "probe source-identity arguments must be ordinary narrow string literals");
    }
    std::string result;
    for (std::size_t index = 1; index + 1 < token.size(); ++index) {
        const char value = token[index];
        if (value == '\0' || value == '\n' || value == '\r') {
            throw std::runtime_error(
                "probe source-identity string contains a forbidden character");
        }
        if (value != '\\') {
            result.push_back(value);
            continue;
        }
        if (++index + 1 >= token.size()) {
            throw std::runtime_error(
                "probe source-identity string ends in an escape");
        }
        switch (token[index]) {
            case '\\': result.push_back('\\'); break;
            case '"': result.push_back('"'); break;
            case '\'': result.push_back('\''); break;
            case '?': result.push_back('?'); break;
            case 'a': result.push_back('\a'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'v': result.push_back('\v'); break;
            default:
                throw std::runtime_error(
                    "probe source-identity string uses an unsupported escape");
        }
    }
    return result;
}

[[nodiscard]] inline std::pair<std::string, std::string>
parse_probe_source_identity_arguments(std::string_view argument_text) {
    const auto [tokens, comments] = lex_source(argument_text);
    if (!comments.empty() || tokens.size() != 3
        || tokens[1].spelling != ",") {
        throw std::runtime_error(
            "template_flatten_provenance requires exactly two ordinary narrow strings");
    }
    return {
        decode_ordinary_narrow_string(tokens[0].spelling),
        decode_ordinary_narrow_string(tokens[2].spelling),
    };
}

[[nodiscard]] inline std::size_t token_at_or_after(
    const std::vector<lexical_token>& tokens,
    std::size_t byte_offset) {
    return static_cast<std::size_t>(std::ranges::lower_bound(
        tokens,
        byte_offset,
        {},
        [](const lexical_token& token) {
            return token.range.begin.byte_offset;
        }) - tokens.begin());
}

[[nodiscard]] inline std::size_t skip_attribute_blocks(
    const std::vector<lexical_token>& tokens,
    std::size_t index) {
    while (attribute_open(tokens, index)) {
        int nested = 0;
        index += 2;
        while (index + 1 < tokens.size()) {
            if (attribute_open(tokens, index)) {
                ++nested;
                index += 2;
            } else if (attribute_close(tokens, index)) {
                index += 2;
                if (nested == 0) {
                    break;
                }
                --nested;
            } else {
                ++index;
            }
        }
    }
    return index;
}

[[nodiscard]] inline bool qualifier(std::string_view token) noexcept {
    return token == "export" || token == "inline" || token == "static"
        || token == "constexpr" || token == "consteval"
        || token == "extern" || token == "friend"
        || token == "[[maybe_unused]]";
}

struct subject {
    declaration_kind kind{declaration_kind::unknown};
    std::string name;
    std::size_t token_begin{};
    std::size_t token_end{};
    std::size_t name_token{};
    std::optional<std::size_t> open_parenthesis;
    std::optional<std::size_t> close_parenthesis;
    std::optional<std::size_t> namespace_keyword;
    std::optional<std::size_t> namespace_open_brace;
    std::optional<std::size_t> namespace_close_brace;
};

[[nodiscard]] inline std::optional<std::size_t> matching_token(
    const std::vector<lexical_token>& tokens,
    std::size_t open,
    std::string_view left,
    std::string_view right) {
    int depth = 0;
    for (std::size_t index = open; index < tokens.size(); ++index) {
        if (tokens[index].spelling == left) {
            ++depth;
        } else if (tokens[index].spelling == right) {
            --depth;
            if (depth == 0) {
                return index;
            }
        }
    }
    return std::nullopt;
}

inline void adjust_angle_depth(
        std::string_view token,
        int& angles) noexcept {
    if (token == "<") {
        ++angles;
    } else if (token == ">" && angles > 0) {
        --angles;
    } else if (token == ">>" && angles >= 2) {
        angles -= 2;
    }
}

[[nodiscard]] inline subject find_subject(
    const std::vector<lexical_token>& tokens,
    const attribute_block& block) {
    const bool has_simd_namespace_marker = std::ranges::any_of(
        block.attributes, is_simd_namespace_marker);
    if (has_simd_namespace_marker
        && (block.token_begin == 0
            || tokens[block.token_begin - 1].spelling
                != "namespace")) {
        return {};
    }
    const bool has_param = std::ranges::any_of(
        block.attributes,
        [](const attribute_record& attribute) {
            return attribute.vendor_namespace == "bioseq_sql"
                && attribute.name == "param";
        });
    const bool has_enumerator = std::ranges::any_of(
        block.attributes,
        [](const attribute_record& attribute) {
            return attribute.vendor_namespace == "bioseq_sql"
                && attribute.name == "guc_value";
        });
    if (has_enumerator && block.token_begin > 0
        && block.token_end < tokens.size()
        && tokens[block.token_end].spelling == "="
        && identifier_begin(tokens[block.token_begin - 1].spelling.front())) {
        return {
            .kind = declaration_kind::enumerator,
            .name = tokens[block.token_begin - 1].spelling,
            .token_begin = block.token_begin - 1,
            .token_end = block.token_end + 1,
            .name_token = block.token_begin - 1,
        };
    }
    if (block.token_begin >= 2
        && block.token_end < tokens.size()
        && tokens[block.token_begin - 2].spelling == "using"
        && identifier_begin(
            tokens[block.token_begin - 1].spelling.front())
        && tokens[block.token_end].spelling == "=") {
        return {
            .kind = declaration_kind::alias,
            .name = tokens[block.token_begin - 1].spelling,
            .token_begin = block.token_begin - 2,
            .token_end = block.token_end,
            .name_token = block.token_begin - 1,
        };
    }

    // Namespace attributes are required by the grammar to follow the
    // `namespace` keyword.  Associate the exact bioseq::simd marker only in
    // that position, and prove both braces before exposing the subject to a
    // generator.
    if (has_simd_namespace_marker && block.token_begin > 0
        && tokens[block.token_begin - 1].spelling == "namespace") {
        const std::size_t keyword = block.token_begin - 1;
        const std::size_t name =
            skip_attribute_blocks(tokens, block.token_end);
        if (name >= tokens.size() || tokens[name].spelling.empty()
            || !identifier_begin(tokens[name].spelling.front())) {
            return {};
        }
        std::size_t open = name + 1;
        while (open < tokens.size()
               && tokens[open].spelling != "{"
               && tokens[open].spelling != ";"
               && tokens[open].spelling != "=") {
            ++open;
        }
        if (open >= tokens.size() || tokens[open].spelling != "{") {
            return {};
        }
        const std::optional<std::size_t> close =
            matching_token(tokens, open, "{", "}");
        if (!close) {
            return {};
        }
        return {
            .kind = declaration_kind::namespace_declaration,
            .name = tokens[name].spelling,
            .token_begin = keyword,
            .token_end = open + 1,
            .name_token = name,
            .namespace_keyword = keyword,
            .namespace_open_brace = open,
            .namespace_close_brace = close,
        };
    }

    std::size_t cursor = skip_attribute_blocks(tokens, block.token_end);
    if (block.token_begin > 0
        && (tokens[block.token_begin - 1].spelling == "struct"
            || tokens[block.token_begin - 1].spelling == "class"
            || tokens[block.token_begin - 1].spelling == "union"
            || tokens[block.token_begin - 1].spelling == "enum")
        && cursor < tokens.size()
        && identifier_begin(tokens[cursor].spelling.front())) {
        return {
            .kind = tokens[block.token_begin - 1].spelling == "enum"
                ? declaration_kind::enumeration
                : declaration_kind::record,
            .name = tokens[cursor].spelling,
            .token_begin = block.token_begin - 1,
            .token_end = cursor + 1,
            .name_token = cursor,
        };
    }
    while (cursor < tokens.size() && qualifier(tokens[cursor].spelling)) {
        ++cursor;
        cursor = skip_attribute_blocks(tokens, cursor);
    }
    if (cursor >= tokens.size()) {
        return {};
    }
    const std::size_t declaration_begin = cursor;
    if (tokens[cursor].spelling == "struct"
        || tokens[cursor].spelling == "class"
        || tokens[cursor].spelling == "union") {
        ++cursor;
        if (cursor < tokens.size()
            && identifier_begin(tokens[cursor].spelling.front())) {
            return {
                .kind = declaration_kind::record,
                .name = tokens[cursor].spelling,
                .token_begin = declaration_begin,
                .token_end = cursor + 1,
                .name_token = cursor,
            };
        }
    }
    if (tokens[cursor].spelling == "enum") {
        ++cursor;
        if (cursor < tokens.size()
            && (tokens[cursor].spelling == "class"
                || tokens[cursor].spelling == "struct")) {
            ++cursor;
        }
        if (cursor < tokens.size()
            && identifier_begin(tokens[cursor].spelling.front())) {
            return {
                .kind = declaration_kind::enumeration,
                .name = tokens[cursor].spelling,
                .token_begin = declaration_begin,
                .token_end = cursor + 1,
                .name_token = cursor,
            };
        }
    }
    if (tokens[cursor].spelling == "using" && cursor + 1 < tokens.size()) {
        return {
            .kind = declaration_kind::alias,
            .name = tokens[cursor + 1].spelling,
            .token_begin = declaration_begin,
            .token_end = cursor + 2,
            .name_token = cursor + 1,
        };
    }

    int parentheses = 0;
    int brackets = 0;
    int braces = 0;
    int angles = 0;
    bool saw_initializer_equals = false;
    std::optional<std::size_t> first_parenthesis;
    std::optional<std::size_t> direct_list_initializer;
    std::size_t terminator = cursor;
    for (; terminator < tokens.size(); ++terminator) {
        const std::string_view token = tokens[terminator].spelling;
        if (has_param && token == ")"
            && parentheses == 0 && brackets == 0
            && braces == 0 && angles == 0) {
            break;
        }
        if (token == "(") {
            if (!first_parenthesis && !saw_initializer_equals
                && parentheses == 0 && brackets == 0 && braces == 0
                && angles == 0) {
                first_parenthesis = terminator;
            }
            ++parentheses;
        } else if (token == ")" && parentheses > 0) {
            --parentheses;
        } else if (token == "[") {
            ++brackets;
        } else if (token == "]") {
            --brackets;
        } else if (token == "{") {
            if (saw_initializer_equals || !first_parenthesis) {
                if (!saw_initializer_equals && !first_parenthesis
                    && parentheses == 0 && brackets == 0
                    && braces == 0 && angles == 0) {
                    direct_list_initializer = terminator;
                }
                ++braces;
            }
        } else if (token == "}" && braces > 0) {
            --braces;
        } else if (token == "=" && parentheses == 0 && brackets == 0
                   && braces == 0 && angles == 0) {
            saw_initializer_equals = true;
        } else {
            adjust_angle_depth(token, angles);
        }
        if (parentheses == 0 && brackets == 0 && braces == 0
            && angles == 0
            && (token == ";" || (token == "{" && !saw_initializer_equals)
                || token == ",")) {
            break;
        }
    }
    if (has_param) {
        std::size_t end = terminator;
        for (std::size_t index = declaration_begin; index < end; ++index) {
            if (tokens[index].spelling == "=") {
                end = index;
                break;
            }
        }
        for (std::size_t index = end; index-- > declaration_begin;) {
            if (!tokens[index].spelling.empty()
                && identifier_begin(tokens[index].spelling.front())) {
                return {
                    .kind = declaration_kind::parameter,
                    .name = tokens[index].spelling,
                    .token_begin = declaration_begin,
                    .token_end = terminator,
                    .name_token = index,
                };
            }
        }
    }
    if (first_parenthesis && *first_parenthesis > declaration_begin) {
        std::size_t name_token = *first_parenthesis;
        while (name_token > declaration_begin) {
            --name_token;
            if (!tokens[name_token].spelling.empty()
                && identifier_begin(tokens[name_token].spelling.front())) {
                const auto close = matching_token(
                    tokens, *first_parenthesis, "(", ")");
                return {
                    .kind = declaration_kind::function,
                    .name = tokens[name_token].spelling,
                    .token_begin = declaration_begin,
                    .token_end =
                        close ? *close + 1 : terminator,
                    .name_token = name_token,
                    .open_parenthesis = first_parenthesis,
                    .close_parenthesis = close,
                };
            }
        }
    }
    std::size_t variable_end =
        direct_list_initializer.value_or(terminator);
    for (std::size_t index = declaration_begin; index < terminator; ++index) {
        if (tokens[index].spelling == "=") {
            variable_end = index;
            break;
        }
    }
    for (std::size_t index = variable_end; index-- > declaration_begin;) {
        if (!tokens[index].spelling.empty()
            && identifier_begin(tokens[index].spelling.front())
            && !qualifier(tokens[index].spelling)) {
            return {
                .kind = declaration_kind::variable,
                .name = tokens[index].spelling,
                .token_begin = declaration_begin,
                .token_end = terminator,
                .name_token = index,
            };
        }
    }
    return {};
}

struct scope_region {
    enum class category {
        namespace_,
        record,
        enum_,
    };

    std::size_t token_begin{};
    std::size_t token_end{};
    std::size_t declaration_token_begin{};
    std::size_t name_token{};
    category kind{category::namespace_};
    aggregate_kind record_kind{aggregate_kind::structure};
    std::string name;
    std::string qualified_name;
    bool exported{};
    bool has_base_classes{};
};

[[nodiscard]] inline std::vector<scope_region> collect_scopes(
    const std::vector<lexical_token>& tokens,
    const std::vector<attribute_block>& attribute_blocks) {
    struct open_scope {
        std::size_t token_begin{};
        std::size_t declaration_token_begin{};
        std::size_t name_token{};
        scope_region::category kind{scope_region::category::namespace_};
        aggregate_kind record_kind{aggregate_kind::structure};
        std::string name;
        std::string qualified_name;
        bool exported{};
        bool has_base_classes{};
    };
    std::vector<scope_region> result;
    std::vector<std::optional<open_scope>> braces;
    std::vector<std::string> named_stack;
    std::vector<bool> exported_stack;
    std::vector<bool> in_attribute(tokens.size());
    for (const attribute_block& block : attribute_blocks) {
        const std::size_t end =
            std::min(block.token_end, in_attribute.size());
        for (std::size_t index = block.token_begin; index < end; ++index) {
            in_attribute[index] = true;
        }
    }
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (in_attribute[index]) continue;
        if (tokens[index].spelling == "{") {
            std::size_t boundary = index;
            while (boundary > 0) {
                if (in_attribute[boundary - 1]) {
                    --boundary;
                    continue;
                }
                const std::string_view token = tokens[boundary - 1].spelling;
                if (token == ";" || token == "{" || token == "}") {
                    break;
                }
                --boundary;
            }
            std::optional<open_scope> discovered;
            for (std::size_t cursor = boundary; cursor < index; ++cursor) {
                if (tokens[cursor].spelling == "namespace"
                    && cursor + 1 < index) {
                    std::string namespace_name;
                    std::size_t name_token = cursor + 1;
                    for (std::size_t part = cursor + 1; part < index; ++part) {
                        if (in_attribute[part]) continue;
                        if (tokens[part].spelling == "inline") continue;
                        if (tokens[part].spelling == "::"
                            || identifier_begin(
                                tokens[part].spelling.front())) {
                            if (namespace_name.empty()
                                && tokens[part].spelling != "::") {
                                name_token = part;
                            }
                            namespace_name += tokens[part].spelling;
                        }
                    }
                    if (!namespace_name.empty()) {
                        discovered = open_scope{
                            .token_begin = index,
                            .declaration_token_begin = boundary,
                            .name_token = name_token,
                            .kind = scope_region::category::namespace_,
                            .name = std::move(namespace_name),
                            .exported = std::ranges::any_of(
                                tokens.begin()
                                    + static_cast<std::ptrdiff_t>(boundary),
                                tokens.begin()
                                    + static_cast<std::ptrdiff_t>(cursor),
                                [](const lexical_token& token) {
                                    return token.spelling == "export";
                                }),
                        };
                    }
                    break;
                }
                if ((tokens[cursor].spelling == "struct"
                     || tokens[cursor].spelling == "class"
                     || tokens[cursor].spelling == "union"
                     || tokens[cursor].spelling == "enum")
                    && cursor + 1 < index) {
                    std::size_t name_index = cursor + 1;
                    if (tokens[cursor].spelling == "enum"
                        && name_index < index
                        && (tokens[name_index].spelling == "class"
                            || tokens[name_index].spelling == "struct")) {
                        ++name_index;
                    }
                    name_index =
                        skip_attribute_blocks(tokens, name_index);
                    if (name_index < index
                        && identifier_begin(
                            tokens[name_index].spelling.front())) {
                        const bool record =
                            tokens[cursor].spelling != "enum";
                        bool has_base_classes = false;
                        if (record) {
                            for (std::size_t part = name_index + 1;
                                 part < index;
                                 ++part) {
                                if (tokens[part].spelling == ":") {
                                    has_base_classes = true;
                                    break;
                                }
                            }
                        }
                        discovered = open_scope{
                            .token_begin = index,
                            .declaration_token_begin = boundary,
                            .name_token = name_index,
                            .kind = record
                                ? scope_region::category::record
                                : scope_region::category::enum_,
                            .record_kind =
                                tokens[cursor].spelling == "class"
                                ? aggregate_kind::class_record
                                : tokens[cursor].spelling == "union"
                                ? aggregate_kind::union_record
                                : aggregate_kind::structure,
                            .name = tokens[name_index].spelling,
                            .exported = std::ranges::any_of(
                                tokens.begin()
                                    + static_cast<std::ptrdiff_t>(boundary),
                                tokens.begin()
                                    + static_cast<std::ptrdiff_t>(cursor),
                                [](const lexical_token& token) {
                                    return token.spelling == "export";
                                }),
                            .has_base_classes = has_base_classes,
                        };
                    }
                    break;
                }
            }
            if (discovered) {
                std::string qualified;
                for (const std::string& parent : named_stack) {
                    if (!qualified.empty()) qualified += "::";
                    qualified += parent;
                }
                if (!qualified.empty()) qualified += "::";
                qualified += discovered->name;
                discovered->qualified_name = std::move(qualified);
                discovered->exported =
                    discovered->exported
                    || (!exported_stack.empty()
                        && exported_stack.back());
                named_stack.push_back(discovered->name);
                exported_stack.push_back(discovered->exported);
                braces.push_back(std::move(discovered));
            } else {
                braces.push_back(std::nullopt);
            }
        } else if (tokens[index].spelling == "}" && !braces.empty()) {
            if (braces.back()) {
                result.push_back(scope_region{
                    .token_begin = braces.back()->token_begin,
                    .token_end = index,
                    .declaration_token_begin =
                        braces.back()->declaration_token_begin,
                    .name_token = braces.back()->name_token,
                    .kind = braces.back()->kind,
                    .record_kind = braces.back()->record_kind,
                    .name = braces.back()->name,
                    .qualified_name = braces.back()->qualified_name,
                    .exported = braces.back()->exported,
                    .has_base_classes =
                        braces.back()->has_base_classes,
                });
                named_stack.pop_back();
                exported_stack.pop_back();
            }
            braces.pop_back();
        }
    }
    return result;
}

[[nodiscard]] inline std::string enclosing_scope(
    const std::vector<scope_region>& scopes,
    std::size_t token_index) {
    const scope_region* best = nullptr;
    for (const scope_region& scope : scopes) {
        if (scope.token_begin < token_index && token_index < scope.token_end
            && (!best || scope.token_begin > best->token_begin)) {
            best = &scope;
        }
    }
    return best ? best->qualified_name : std::string{};
}

[[nodiscard]] inline std::string clean_doc_line(std::string_view line) {
    line = trim_view(line);
    if (line.starts_with("///") || line.starts_with("//!")) {
        line.remove_prefix(3);
    } else if (line.starts_with("/**") || line.starts_with("/*!")) {
        line.remove_prefix(3);
    } else if (line.starts_with("/*")) {
        line.remove_prefix(2);
    }
    line = trim_view(line);
    if (line.starts_with("*")) {
        line.remove_prefix(1);
    }
    line = trim_view(line);
    if (line.ends_with("*/")) {
        line.remove_suffix(2);
    }
    return std::string{trim_view(line)};
}

[[nodiscard]] inline documentation_record parse_documentation(
    std::string_view text,
    source_range range) {
    documentation_record result{.range = range};
    enum class section {
        details,
        brief,
        remark,
        param,
        returns,
        example,
    };
    section current = section::details;
    std::size_t current_param = 0;
    bool in_sql_fence = false;
    std::string example;
    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t newline = text.find('\n', cursor);
        const std::size_t end =
            newline == std::string_view::npos ? text.size() : newline;
        std::string line = clean_doc_line(text.substr(cursor, end - cursor));
        const auto command = [&line](std::string_view name) {
            return line.starts_with("@" + std::string{name})
                || line.starts_with("\\" + std::string{name});
        };
        const auto after_command = [&line](std::string_view name) {
            const std::size_t offset = name.size() + 1;
            return std::string{trim_view(
                std::string_view{line}.substr(offset))};
        };
        if (line.starts_with("```sql")) {
            in_sql_fence = true;
            current = section::example;
            example.clear();
        } else if (in_sql_fence && line == "```") {
            in_sql_fence = false;
            result.runnable_examples.push_back(example);
            current = section::details;
        } else if (in_sql_fence) {
            if (!example.empty()) example += '\n';
            example += line;
        } else if (command("brief")) {
            result.has_explicit_brief = true;
            result.brief = after_command("brief");
            current = section::brief;
        } else if (command("remark")) {
            result.remarks.push_back(after_command("remark"));
            current = section::remark;
        } else if (command("ingroup")) {
            result.groups.push_back(after_command("ingroup"));
            current = section::details;
        } else if (command("param")) {
            std::string rest = after_command("param");
            const std::size_t split = rest.find_first_of(" \t");
            result.parameters.push_back(documented_parameter{
                .name = split == std::string::npos
                    ? rest
                    : rest.substr(0, split),
                .text = split == std::string::npos
                    ? std::string{}
                    : std::string{trim_view(
                          std::string_view{rest}.substr(split))},
            });
            current_param = result.parameters.size() - 1;
            current = section::param;
        } else if (command("return") || command("returns")) {
            result.returns = command("returns")
                ? after_command("returns")
                : after_command("return");
            current = section::returns;
        } else if (!line.empty() && line != "/" && line != "*/") {
            auto append = [&line](std::string& destination) {
                if (!destination.empty()) destination += ' ';
                destination += line;
            };
            switch (current) {
                case section::brief: append(result.brief); break;
                case section::remark:
                    if (!result.remarks.empty()) {
                        append(result.remarks.back());
                    }
                    break;
                case section::param:
                    append(result.parameters[current_param].text);
                    break;
                case section::returns: append(result.returns); break;
                case section::details: append(result.details); break;
                case section::example: break;
            }
        }
        if (newline == std::string_view::npos) break;
        cursor = newline + 1;
    }
    return result;
}

[[nodiscard]] inline documentation_record documentation_before(
    std::string_view source,
    const std::vector<comment_record>& comments,
    std::size_t offset) {
    if (comments.empty()) return {};
    std::vector<const comment_record*> selected;
    std::size_t cursor = offset;
    for (std::size_t index = comments.size(); index-- > 0;) {
        const comment_record& comment = comments[index];
        if (comment.range.end.byte_offset > cursor) continue;
        const std::string_view gap = source.substr(
            comment.range.end.byte_offset,
            cursor - comment.range.end.byte_offset);
        if (!std::ranges::all_of(gap, ascii_space)) {
            break;
        }
        if (!comment.doxygen) break;
        selected.push_back(&comment);
        cursor = comment.range.begin.byte_offset;
    }
    if (selected.empty()) return {};
    std::ranges::reverse(selected);
    std::string combined;
    for (const comment_record* comment : selected) {
        if (!combined.empty()) combined += '\n';
        combined += comment->text;
    }
    return parse_documentation(
        combined,
        {
            .begin = selected.front()->range.begin,
            .end = selected.back()->range.end,
        });
}

/// Parses the raw Doxygen comments covered by a Clang documentation range.
[[nodiscard]] inline documentation_record documentation_overlapping(
    const std::vector<comment_record>& comments,
    const source_range& range) {
    if (range.end.byte_offset <= range.begin.byte_offset) {
        return {};
    }
    std::vector<const comment_record*> selected;
    for (const comment_record& comment : comments) {
        if (!comment.doxygen
            || comment.range.end.byte_offset
                <= range.begin.byte_offset
            || range.end.byte_offset
                <= comment.range.begin.byte_offset) {
            continue;
        }
        selected.push_back(&comment);
    }
    if (selected.empty()) {
        return {};
    }
    std::string combined;
    for (const comment_record* comment : selected) {
        if (!combined.empty()) {
            combined += '\n';
        }
        combined += comment->text;
    }
    return parse_documentation(
        combined,
        {
            .begin = selected.front()->range.begin,
            .end = selected.back()->range.end,
        });
}

/**
 * Return Doxygen commentary immediately following a SQL attribute block.
 *
 * Both
 *
 *   Doxygen comment, SQL attribute, declaration
 *
 * and
 *
 *   SQL attribute, Doxygen comment, declaration
 *
 * are valid, readable source layouts.  Clang does not consistently attach
 * the latter form to declarations carrying ignored vendor attributes, so the
 * structural pass also confirms the association lexically.
 */
[[nodiscard]] inline documentation_record documentation_after(
    std::string_view source,
    const std::vector<comment_record>& comments,
    std::size_t offset,
    std::size_t limit) {
    if (comments.empty() || offset >= limit || limit > source.size()) {
        return {};
    }
    std::vector<const comment_record*> selected;
    std::size_t cursor = offset;
    for (const comment_record& comment : comments) {
        if (comment.range.begin.byte_offset < cursor) continue;
        if (comment.range.end.byte_offset > limit) break;
        const std::string_view gap = source.substr(
            cursor,
            comment.range.begin.byte_offset - cursor);
        if (!std::ranges::all_of(gap, ascii_space)
            || !comment.doxygen) {
            break;
        }
        selected.push_back(&comment);
        cursor = comment.range.end.byte_offset;
    }
    if (selected.empty()) return {};
    std::string combined;
    for (const comment_record* comment : selected) {
        if (!combined.empty()) combined += '\n';
        combined += comment->text;
    }
    return parse_documentation(
        combined,
        {
            .begin = selected.front()->range.begin,
            .end = selected.back()->range.end,
        });
}

[[nodiscard]] inline bool documentation_empty(
    const documentation_record& documentation) noexcept {
    return !documentation.has_explicit_brief
        && documentation.brief.empty()
        && documentation.details.empty()
        && documentation.remarks.empty()
        && documentation.groups.empty()
        && documentation.parameters.empty()
        && documentation.returns.empty()
        && documentation.runnable_examples.empty();
}

inline void merge_documentation_groups(
    documentation_record& destination,
    const documentation_record& source) {
    for (const std::string& group : source.groups) {
        if (!group.empty()
            && !std::ranges::contains(destination.groups, group)) {
            destination.groups.push_back(group);
        }
    }
}

[[nodiscard]] inline std::string condition_at(
    std::string_view source,
    std::size_t offset) {
    std::vector<std::string> stack;
    std::size_t line_begin = 0;
    while (line_begin < offset) {
        const std::size_t newline = source.find('\n', line_begin);
        const std::size_t line_end = std::min(
            newline == std::string_view::npos ? source.size() : newline,
            offset);
        std::string_view line =
            trim_view(source.substr(line_begin, line_end - line_begin));
        if (line.starts_with("#if ")) {
            stack.emplace_back(trim_view(line.substr(4)));
        } else if (line.starts_with("#ifdef ")) {
            stack.emplace_back("defined(" + std::string{
                trim_view(line.substr(7))} + ")");
        } else if (line.starts_with("#ifndef ")) {
            stack.emplace_back("!defined(" + std::string{
                trim_view(line.substr(8))} + ")");
        } else if (line.starts_with("#elif ") && !stack.empty()) {
            stack.back() = std::string{trim_view(line.substr(6))};
        } else if (line == "#else" && !stack.empty()) {
            stack.back() = "!(" + stack.back() + ")";
        } else if (line == "#endif" && !stack.empty()) {
            stack.pop_back();
        }
        if (newline == std::string_view::npos) break;
        line_begin = newline + 1;
    }
    std::string result;
    for (const std::string& condition : stack) {
        if (!result.empty()) result += " && ";
        result += "(" + condition + ")";
    }
    return result;
}

[[nodiscard]] inline marked_namespace_record make_marked_namespace(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    const attribute_block& block,
    const subject& subject,
    std::string qualified_name,
    std::string target_usr = {}) {
    if (subject.kind != declaration_kind::namespace_declaration
        || !subject.namespace_keyword
        || !subject.namespace_open_brace
        || !subject.namespace_close_brace) {
        throw std::runtime_error(
            "structural namespace marker has no confirmed namespace geometry");
    }
    const std::size_t keyword = *subject.namespace_keyword;
    const std::size_t open = *subject.namespace_open_brace;
    const std::size_t close = *subject.namespace_close_brace;
    if (keyword >= tokens.size() || subject.name_token >= tokens.size()
        || open >= tokens.size() || close >= tokens.size()
        || keyword >= open || open >= close) {
        throw std::runtime_error(
            "structural namespace marker has invalid token geometry");
    }
    const auto marker = std::ranges::find_if(
        block.attributes, is_simd_namespace_marker);
    if (marker == block.attributes.end()) {
        throw std::runtime_error(
            "structural namespace subject has no exact bioseq::simd marker");
    }

    std::size_t header_begin = keyword;
    while (header_begin > 0) {
        const std::string_view previous =
            tokens[header_begin - 1].spelling;
        if (previous == ";" || previous == "{" || previous == "}") {
            break;
        }
        --header_begin;
    }
    const bool explicit_export = std::ranges::any_of(
        tokens.begin() + static_cast<std::ptrdiff_t>(header_begin),
        tokens.begin() + static_cast<std::ptrdiff_t>(close),
        [](const lexical_token& token) {
            return token.spelling == "export";
        });
    const std::string boundary_condition = condition_at(
        source, tokens[keyword].range.begin.byte_offset);
    const bool crosses_preprocessor_region =
        !boundary_condition.empty()
        || condition_at(source, tokens[open].range.end.byte_offset)
            != boundary_condition
        || condition_at(source, tokens[close].range.begin.byte_offset)
            != boundary_condition
        || condition_at(source, tokens[close].range.end.byte_offset)
            != boundary_condition;

    attribute_record recorded_marker = *marker;
    recorded_marker.target_usr = target_usr;
    recorded_marker.placement = attribute_placement::namespace_head;
    return {
        .qualified_name = std::move(qualified_name),
        .target_usr = std::move(target_usr),
        .marker = std::move(recorded_marker),
        .marker_block_range = block.range,
        .declaration_range = {
            .begin = tokens[keyword].range.begin,
            .end = tokens[close].range.end,
        },
        .keyword_range = tokens[keyword].range,
        .name_range = tokens[subject.name_token].range,
        .open_brace_range = tokens[open].range,
        .body_range = {
            .begin = tokens[open].range.end,
            .end = tokens[close].range.begin,
        },
        .close_brace_range = tokens[close].range,
        .has_explicit_export = explicit_export,
        .crosses_preprocessor_region =
            crosses_preprocessor_region,
    };
}

[[nodiscard]] inline std::string source_between(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    std::size_t begin,
    std::size_t end) {
    if (begin >= end || begin >= tokens.size()) return {};
    end = std::min(end, tokens.size());
    const std::size_t byte_begin = tokens[begin].range.begin.byte_offset;
    const std::size_t byte_end = tokens[end - 1].range.end.byte_offset;
    return std::string{source.substr(byte_begin, byte_end - byte_begin)};
}

[[nodiscard]] inline parameter_record parse_parameter(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    std::size_t begin,
    std::size_t end,
    const std::vector<attribute_block>& blocks) {
    parameter_record result;
    if (begin >= end) return result;
    std::size_t equals = end;
    int nested = 0;
    for (std::size_t index = begin; index < end; ++index) {
        if (tokens[index].spelling == "(" || tokens[index].spelling == "{"
            || tokens[index].spelling == "[") {
            ++nested;
        } else if (tokens[index].spelling == ")"
                   || tokens[index].spelling == "}"
                   || tokens[index].spelling == "]") {
            --nested;
        } else if (tokens[index].spelling == "=" && nested == 0) {
            equals = index;
            break;
        }
    }
    const std::size_t type_begin =
        skip_attribute_blocks(tokens, begin);
    std::vector<bool> in_attribute(tokens.size());
    for (const attribute_block& block : blocks) {
        if (block.token_begin < begin || block.token_end > equals) continue;
        for (std::size_t index = block.token_begin;
             index < block.token_end;
             ++index) {
            in_attribute[index] = true;
        }
    }
    const auto non_name_keyword = [](std::string_view spelling) {
        static constexpr std::array keywords{
            std::string_view{"alignas"}, std::string_view{"auto"},
            std::string_view{"bool"}, std::string_view{"char"},
            std::string_view{"char8_t"}, std::string_view{"char16_t"},
            std::string_view{"char32_t"}, std::string_view{"class"},
            std::string_view{"const"}, std::string_view{"consteval"},
            std::string_view{"constexpr"}, std::string_view{"constinit"},
            std::string_view{"decltype"}, std::string_view{"double"},
            std::string_view{"enum"}, std::string_view{"explicit"},
            std::string_view{"extern"}, std::string_view{"float"},
            std::string_view{"friend"}, std::string_view{"inline"},
            std::string_view{"int"}, std::string_view{"long"},
            std::string_view{"mutable"}, std::string_view{"noexcept"},
            std::string_view{"register"}, std::string_view{"requires"},
            std::string_view{"short"}, std::string_view{"signed"},
            std::string_view{"static"}, std::string_view{"struct"},
            std::string_view{"template"}, std::string_view{"thread_local"},
            std::string_view{"typename"}, std::string_view{"union"},
            std::string_view{"unsigned"}, std::string_view{"virtual"},
            std::string_view{"void"}, std::string_view{"volatile"},
            std::string_view{"wchar_t"},
        };
        return std::ranges::contains(keywords, spelling);
    };
    std::optional<std::size_t> candidate_name;
    int parentheses = 0;
    int brackets = 0;
    int angles = 0;
    for (std::size_t index = type_begin; index < equals; ++index) {
        if (in_attribute[index]) continue;
        const std::string_view spelling = tokens[index].spelling;
        if (spelling == "(") ++parentheses;
        else if (spelling == ")") --parentheses;
        else if (spelling == "[") ++brackets;
        else if (spelling == "]") --brackets;
        else if (spelling == "<") ++angles;
        else if (spelling == ">" && angles != 0) --angles;
        else if (spelling == ">>" && angles >= 2) angles -= 2;
        else if (parentheses == 0 && brackets == 0 && angles == 0
                 && !spelling.empty()
                 && identifier_begin(spelling.front())
                 && std::ranges::all_of(spelling, identifier_continue)
                 && !non_name_keyword(spelling)) {
            candidate_name = index;
        }
    }
    bool has_prior_type = false;
    if (candidate_name
        && (*candidate_name == type_begin
            || tokens[*candidate_name - 1].spelling == "::")) {
        candidate_name.reset();
    }
    if (candidate_name) {
        for (std::size_t index = type_begin;
             index < *candidate_name;
             ++index) {
            if (in_attribute[index]) continue;
            const std::string_view spelling = tokens[index].spelling;
            const bool type_keyword = spelling == "auto"
                || spelling == "bool" || spelling == "char"
                || spelling == "char8_t" || spelling == "char16_t"
                || spelling == "char32_t" || spelling == "decltype"
                || spelling == "double" || spelling == "float"
                || spelling == "int" || spelling == "long"
                || spelling == "short" || spelling == "signed"
                || spelling == "unsigned" || spelling == "void"
                || spelling == "wchar_t" || spelling == "typename"
                || spelling == "class" || spelling == "struct"
                || spelling == "enum";
            const bool user_type = !spelling.empty()
                && identifier_begin(spelling.front())
                && std::ranges::all_of(spelling, identifier_continue)
                && !non_name_keyword(spelling);
            if (type_keyword || user_type) {
                has_prior_type = true;
                break;
            }
        }
        if (!has_prior_type) candidate_name.reset();
    }
    if (candidate_name) {
        result.name = tokens[*candidate_name].spelling;
        result.name_range = tokens[*candidate_name].range;
    }
    result.cpp_type = source_between(
        source,
        tokens,
        type_begin,
        candidate_name.value_or(equals));
    result.has_default = equals < end;
    if (result.has_default) {
        result.default_expression =
            source_between(source, tokens, equals + 1, end);
    }
    result.range = {
        .begin = tokens[begin].range.begin,
        .end = tokens[end - 1].range.end,
    };
    for (const attribute_block& block : blocks) {
        if (block.token_begin >= begin && block.token_end <= end) {
            for (const attribute_record& attribute : block.attributes) {
                result.attributes.push_back(attribute);
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::vector<parameter_record> parse_parameters(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    std::size_t open,
    std::size_t close,
    const std::vector<attribute_block>& blocks) {
    std::vector<parameter_record> result;
    std::size_t begin = open + 1;
    int parentheses = 0;
    int braces = 0;
    int brackets = 0;
    int angles = 0;
    for (std::size_t index = begin; index <= close; ++index) {
        const bool at_end = index == close;
        if (!at_end) {
            const std::string_view token = tokens[index].spelling;
            if (token == "(") ++parentheses;
            else if (token == ")") --parentheses;
            else if (token == "{") ++braces;
            else if (token == "}") --braces;
            else if (token == "[") ++brackets;
            else if (token == "]") --brackets;
            else adjust_angle_depth(token, angles);
        }
        if (at_end
            || (tokens[index].spelling == ","
                && parentheses == 0 && braces == 0
                && brackets == 0 && angles == 0)) {
            parameter_record parameter =
                parse_parameter(source, tokens, begin, index, blocks);
            if (!parameter.cpp_type.empty()
                && !(parameter.name.empty()
                     && trim_view(parameter.cpp_type) == "void")) {
                parameter.ordinal = result.size();
                result.push_back(std::move(parameter));
            }
            begin = index + 1;
        }
    }
    return result;
}

[[nodiscard]] inline member_access access_from_token(
        std::string_view token) noexcept {
    if (token == "private") return member_access::private_access;
    if (token == "protected") return member_access::protected_access;
    return member_access::public_access;
}

[[nodiscard]] inline std::string enclosing_name(
        std::string_view qualified_name) {
    const std::size_t separator = qualified_name.rfind("::");
    return separator == std::string_view::npos
        ? std::string{}
        : std::string{qualified_name.substr(0, separator)};
}

[[nodiscard]] inline aggregate_record collect_aggregate(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    const std::vector<comment_record>& comments,
    const std::vector<attribute_block>& blocks,
    const source_positions& positions,
    const scope_region& scope) {
    const std::size_t record_begin =
        scope.declaration_token_begin;
    const std::size_t begin_offset =
        tokens[record_begin].range.begin.byte_offset;
    aggregate_record result{
        .name = scope.name,
        .qualified_name = scope.qualified_name,
        .enclosing_qualified_name =
            enclosing_name(scope.qualified_name),
        .kind = scope.record_kind,
        .range = {
            .begin = positions.at(begin_offset),
            .end = tokens[scope.token_end].range.end,
        },
        .documentation =
            documentation_before(source, comments, begin_offset),
        .exported = scope.exported,
        .has_base_classes = scope.has_base_classes,
    };
    if (scope.record_kind == aggregate_kind::union_record) {
        result.structural_errors.emplace_back(
            "result rows cannot be unions");
    }
    if (scope.has_base_classes) {
        result.structural_errors.emplace_back(
            "result rows cannot have base classes");
    }

    member_access access =
        scope.record_kind == aggregate_kind::class_record
        ? member_access::private_access
        : member_access::public_access;
    std::size_t member_begin = scope.token_begin + 1;
    int parentheses = 0;
    int brackets = 0;
    int braces = 0;
    int angles = 0;

    for (std::size_t index = member_begin;
         index < scope.token_end;
         ++index) {
        const std::string_view token = tokens[index].spelling;

        // A method definition has no terminating semicolon. Discard it at
        // its closing brace so it cannot absorb the following data member.
        if (token == "{" && parentheses == 0 && brackets == 0
            && braces == 0 && angles == 0) {
            bool method = false;
            int local_angles = 0;
            for (std::size_t cursor = member_begin;
                 cursor < index;
                 ++cursor) {
                adjust_angle_depth(
                    tokens[cursor].spelling, local_angles);
                if (tokens[cursor].spelling == "("
                    && local_angles == 0) {
                    method = true;
                    break;
                }
            }
            if (method) {
                if (std::ranges::any_of(
                        tokens.begin()
                            + static_cast<std::ptrdiff_t>(member_begin),
                        tokens.begin()
                            + static_cast<std::ptrdiff_t>(index),
                        [](const lexical_token& candidate) {
                            return candidate.spelling == "virtual";
                        })) {
                    result.has_virtual_members = true;
                }
                if (const auto close =
                        matching_token(tokens, index, "{", "}")) {
                    member_begin = *close + 1;
                    index = *close;
                    continue;
                }
            }
        }

        if (token == "(") ++parentheses;
        else if (token == ")") --parentheses;
        else if (token == "[") ++brackets;
        else if (token == "]") --brackets;
        else if (token == "{") ++braces;
        else if (token == "}") --braces;
        else adjust_angle_depth(token, angles);

        if (token != ";" || parentheses != 0 || brackets != 0
            || braces != 0 || angles != 0) {
            continue;
        }

        std::size_t begin = member_begin;
        while (begin + 1 < index
               && (tokens[begin].spelling == "public"
                   || tokens[begin].spelling == "private"
                   || tokens[begin].spelling == "protected")
               && tokens[begin + 1].spelling == ":") {
            access = access_from_token(tokens[begin].spelling);
            begin += 2;
        }
        if (begin >= index) {
            member_begin = index + 1;
            continue;
        }

        const std::size_t attribute_begin = begin;
        begin = skip_attribute_blocks(tokens, begin);
        if (begin >= index) {
            member_begin = index + 1;
            continue;
        }

        const bool nested_declaration =
            tokens[begin].spelling == "struct"
            || tokens[begin].spelling == "class"
            || tokens[begin].spelling == "union"
            || tokens[begin].spelling == "enum";
        const bool ignorable_non_field =
            nested_declaration
            || tokens[begin].spelling == "using"
            || tokens[begin].spelling == "typedef"
            || tokens[begin].spelling == "friend"
            || tokens[begin].spelling == "static_assert"
            || tokens[begin].spelling == "template";
        if (nested_declaration) {
            result.structural_errors.emplace_back(
                "result rows cannot contain nested aggregate members");
        }

        int local_parentheses = 0;
        int local_brackets = 0;
        int local_braces = 0;
        int local_angles = 0;
        std::size_t initializer = index;
        std::size_t bit_field = index;
        bool method = false;
        bool is_static = false;
        bool multiple_declarators = false;
        bool saw_virtual = false;
        for (std::size_t cursor = begin; cursor < index; ++cursor) {
            const std::string_view current = tokens[cursor].spelling;
            const bool top_level =
                local_parentheses == 0 && local_brackets == 0
                && local_braces == 0 && local_angles == 0;
            if (top_level && current == "static") {
                is_static = true;
            } else if (top_level && current == "virtual") {
                saw_virtual = true;
            } else if (top_level && current == "(") {
                method = true;
            } else if (top_level && current == ",") {
                multiple_declarators = true;
            } else if (top_level && current == ":") {
                bit_field = cursor;
            } else if (top_level
                       && (current == "=" || current == "{")) {
                initializer = cursor;
            }

            if (current == "(") ++local_parentheses;
            else if (current == ")") --local_parentheses;
            else if (current == "[") ++local_brackets;
            else if (current == "]") --local_brackets;
            else if (current == "{") ++local_braces;
            else if (current == "}") --local_braces;
            else adjust_angle_depth(current, local_angles);
        }
        if (saw_virtual) result.has_virtual_members = true;
        if (ignorable_non_field || method || is_static) {
            member_begin = index + 1;
            continue;
        }

        const std::size_t declarator_end =
            std::min(initializer, bit_field);
        std::size_t name_token = declarator_end;
        while (name_token > begin) {
            --name_token;
            if (!tokens[name_token].spelling.empty()
                && identifier_begin(
                    tokens[name_token].spelling.front())
                && !qualifier(tokens[name_token].spelling)
                && tokens[name_token].spelling != "const"
                && tokens[name_token].spelling != "volatile"
                && tokens[name_token].spelling != "mutable") {
                break;
            }
        }
        const std::string cpp_type =
            source_between(source, tokens, begin, name_token);
        if (name_token < begin
            || name_token >= declarator_end
            || trim_view(cpp_type).empty()) {
            result.structural_errors.emplace_back(
                "result rows cannot contain anonymous data members");
            member_begin = index + 1;
            continue;
        }

        std::vector<attribute_record> attributes;
        for (const attribute_block& block : blocks) {
            if (block.token_begin >= attribute_begin
                && block.token_end <= index) {
                for (const attribute_record& attribute :
                     block.attributes) {
                    attributes.push_back(attribute);
                }
            }
        }
        const std::size_t field_offset =
            tokens[attribute_begin].range.begin.byte_offset;
        result.fields.push_back(aggregate_field_record{
            .name = tokens[name_token].spelling,
            .qualified_name = scope.qualified_name + "::"
                + tokens[name_token].spelling,
            .cpp_type = cpp_type,
            .initializer_text =
                initializer == index
                ? std::string{}
                : source_between(
                    source, tokens, initializer + 1, index),
            .active_condition =
                condition_at(source, field_offset),
            .attributes = std::move(attributes),
            .documentation =
                documentation_before(source, comments, field_offset),
            .range = {
                .begin = positions.at(field_offset),
                .end = tokens[index].range.end,
            },
            .access = access,
            .has_default_initializer = initializer != index,
            .bit_field = bit_field != index,
            .multiple_declarators = multiple_declarators,
        });
        member_begin = index + 1;
    }
    if (result.has_virtual_members) {
        result.structural_errors.emplace_back(
            "result rows cannot have virtual members");
    }
    return result;
}

inline constexpr std::array<std::string_view, 16>
    repository_source_roots{
        "bench",
        "bench_framework",
        "bioseq",
        "bioseq_caching",
        "build_mcp",
        "cmake",
        "codegen",
        "common",
        "config",
        "data",
        "DOCS",
        "infra",
        "overlays",
        "research",
        "vendor",
        "website",
    };

[[nodiscard]] inline bool repository_source_root(
    std::string_view component) noexcept {
    return std::ranges::contains(repository_source_roots, component);
}

/// Produces the stable source spelling used by generated identities. Absolute
/// paths begin at a recognized repository-root child; logical relative paths
/// retain their full spelling.
[[nodiscard]] inline std::string canonical_repository_source_identity(
    const std::filesystem::path& source_path) {
    if (source_path.empty()) {
        throw std::runtime_error("source identity path is empty");
    }
    const std::string raw_spelling = source_path.generic_string();
    if (raw_spelling.contains('\\')
        || (raw_spelling.size() >= 2
            && std::isalpha(
                static_cast<unsigned char>(raw_spelling.front()))
            && raw_spelling[1] == ':')) {
        throw std::runtime_error(
            "source identity contains a drive or non-canonical separator");
    }

    for (const std::filesystem::path& component : source_path) {
        if (component == "..") {
            throw std::runtime_error(
                "source identity contains parent traversal");
        }
    }

    const std::filesystem::path normalized =
        source_path.lexically_normal();
    std::vector<std::string> components;
    for (const std::filesystem::path& component : normalized) {
        const std::string spelling = component.generic_string();
        if (spelling.empty() || spelling == "."
            || spelling == normalized.root_name().generic_string()
            || spelling == normalized.root_directory().generic_string()) {
            continue;
        }
        components.push_back(spelling);
    }

    if (normalized.is_absolute()) {
        const auto source_root = std::ranges::find_if(
            components,
            [](std::string_view component) {
                return repository_source_root(component);
            });
        if (source_root == components.end()) {
            throw std::runtime_error(
                "absolute source identity is outside the repository source root");
        }
        if (source_root != components.begin()
            && *std::prev(source_root) == "main") {
            throw std::runtime_error(
                "absolute source identity uses the forbidden main source-root prefix");
        }
        components.erase(components.begin(), source_root);
    } else if (!components.empty() && components.front() == "main") {
        throw std::runtime_error(
            "source identity uses the forbidden main source-root prefix");
    }

    if (components.empty()) {
        throw std::runtime_error(
            "source identity has no repository-relative payload");
    }
    std::string identity;
    for (const std::string& component : components) {
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error(
                "source identity contains a non-canonical component");
        }
        if (!identity.empty()) identity.push_back('/');
        identity += component;
    }
    if (identity.empty() || identity.starts_with('/')) {
        throw std::runtime_error(
            "source identity is not repository-relative");
    }
    return identity;
}

[[nodiscard]] inline bool has_repository_source_root_component(
    const std::filesystem::path& source_path) {
    return std::ranges::any_of(
        source_path,
        [](const std::filesystem::path& component) {
            return repository_source_root(component.generic_string());
        });
}

[[nodiscard]] inline std::string source_entity_identity(
    const std::filesystem::path& source_path,
    std::string_view entity,
    std::size_t byte_offset) {
    const std::filesystem::path identity_path =
        source_path.is_absolute()
            && !has_repository_source_root_component(source_path)
        ? source_path.filename()
        : source_path;
    return canonical_repository_source_identity(identity_path)
        + "#" + std::string{entity}
        + ":" + std::to_string(byte_offset);
}

[[nodiscard]] inline preprocessor_directive_kind directive_kind_of(
    std::string_view spelling) noexcept {
    if (spelling == "if") {
        return preprocessor_directive_kind::if_directive;
    }
    if (spelling == "ifdef") return preprocessor_directive_kind::ifdef;
    if (spelling == "ifndef") return preprocessor_directive_kind::ifndef;
    if (spelling == "elif") return preprocessor_directive_kind::elif;
    if (spelling == "else") {
        return preprocessor_directive_kind::else_directive;
    }
    if (spelling == "endif") return preprocessor_directive_kind::endif;
    if (spelling == "define") return preprocessor_directive_kind::define;
    if (spelling == "undef") return preprocessor_directive_kind::undef;
    if (spelling == "include") return preprocessor_directive_kind::include;
    if (spelling == "pragma") return preprocessor_directive_kind::pragma;
    if (spelling == "error") return preprocessor_directive_kind::error;
    if (spelling == "line") return preprocessor_directive_kind::line;
    return preprocessor_directive_kind::other;
}

[[nodiscard]] inline bool preprocessing_space(
    std::string_view source) noexcept {
    std::size_t cursor = 0;
    while (cursor < source.size()) {
        if (ascii_space(source[cursor])) {
            ++cursor;
            continue;
        }
        if (cursor + 1 < source.size()
            && source[cursor] == '/'
            && source[cursor + 1] == '*') {
            const std::size_t close = source.find("*/", cursor + 2);
            if (close == std::string_view::npos) return false;
            cursor = close + 2;
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] inline std::vector<preprocessor_directive_record>
collect_preprocessor_directives(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    const source_positions& positions) {
    std::vector<preprocessor_directive_record> result;
    std::size_t nesting_depth = 0;
    for (const lexical_token& token : tokens) {
        if (token.spelling != "#") continue;
        const std::size_t hash = token.range.begin.byte_offset;
        if (std::ranges::any_of(
                result,
                [hash](const preprocessor_directive_record& directive) {
                    return hash
                            >= directive.complete_range.begin.byte_offset
                        && hash
                            < directive.complete_range.end.byte_offset;
                })) {
            continue;
        }
        const std::size_t line_begin = hash == 0
            ? 0
            : source.rfind('\n', hash - 1) == std::string_view::npos
            ? 0
            : source.rfind('\n', hash - 1) + 1;
        if (!preprocessing_space(
                source.substr(line_begin, hash - line_begin))) {
            continue;
        }

        std::size_t logical_end = line_begin;
        while (logical_end < source.size()) {
            const std::size_t newline = source.find('\n', logical_end);
            const std::size_t physical_end = newline == std::string_view::npos
                ? source.size()
                : newline;
            std::size_t continuation = physical_end;
            if (continuation > logical_end
                && source[continuation - 1] == '\r') {
                --continuation;
            }
            const bool continued = continuation > logical_end
                && source[continuation - 1] == '\\';
            logical_end = newline == std::string_view::npos
                ? source.size()
                : newline + 1;
            if (!continued || newline == std::string_view::npos) break;
        }

        std::size_t name_begin = hash + 1;
        while (name_begin < logical_end
               && ascii_space(source[name_begin])) {
            ++name_begin;
        }
        std::size_t name_end = name_begin;
        while (name_end < logical_end
               && identifier_continue(source[name_end])) {
            ++name_end;
        }
        const std::string_view name = source.substr(
            name_begin, name_end - name_begin);
        const preprocessor_directive_kind kind =
            directive_kind_of(name);
        if (kind == preprocessor_directive_kind::endif
            && nesting_depth != 0) {
            --nesting_depth;
        }
        const std::size_t record_depth =
            (kind == preprocessor_directive_kind::elif
             || kind == preprocessor_directive_kind::else_directive)
                && nesting_depth != 0
            ? nesting_depth - 1
            : nesting_depth;
        std::size_t argument_begin = name_end;
        while (argument_begin < logical_end
               && ascii_space(source[argument_begin])) {
            ++argument_begin;
        }
        std::size_t argument_end = logical_end;
        while (argument_end > argument_begin
               && ascii_space(source[argument_end - 1])) {
            --argument_end;
        }
        result.push_back({
            .kind = kind,
            .complete_range = positions.range(line_begin, logical_end),
            .argument_range =
                positions.range(argument_begin, argument_end),
            .spelling = std::string{
                source.substr(line_begin, logical_end - line_begin)},
            .nesting_depth = record_depth,
        });
        if (kind == preprocessor_directive_kind::if_directive
            || kind == preprocessor_directive_kind::ifdef
            || kind == preprocessor_directive_kind::ifndef) {
            ++nesting_depth;
        }
    }
    return result;
}

[[nodiscard]] inline bool offset_in_directive(
    std::size_t offset,
    const std::vector<preprocessor_directive_record>& directives) noexcept {
    return std::ranges::any_of(
        directives,
        [offset](const preprocessor_directive_record& directive) {
            return offset >= directive.complete_range.begin.byte_offset
                && offset < directive.complete_range.end.byte_offset;
        });
}

[[nodiscard]] inline std::string joined_token_spelling(
    const std::vector<lexical_token>& tokens,
    std::size_t begin,
    std::size_t end) {
    std::string result;
    for (std::size_t index = begin; index < end; ++index) {
        result += tokens[index].spelling;
    }
    return result;
}

[[nodiscard]] inline module_record collect_module_record(
    const std::filesystem::path& source_path,
    const std::vector<lexical_token>& tokens,
    const std::vector<preprocessor_directive_record>& directives) {
    module_record result{.source_path = source_path};
    int braces = 0;
    int parentheses = 0;
    int brackets = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const std::string_view token = tokens[index].spelling;
        if (offset_in_directive(
                tokens[index].range.begin.byte_offset, directives)) {
            continue;
        }
        if (token == "{") {
            ++braces;
            continue;
        }
        if (token == "}") {
            if (braces == 0) {
                throw std::runtime_error(
                    source_path.string()
                    + ": token slice has an unmatched closing brace");
            }
            --braces;
            continue;
        }
        if (token == "(") ++parentheses;
        else if (token == ")") --parentheses;
        else if (token == "[") ++brackets;
        else if (token == "]") --brackets;
        if (parentheses < 0 || brackets < 0) {
            throw std::runtime_error(
                source_path.string()
                + ": token slice has unbalanced top-level delimiters");
        }
        if (braces != 0 || parentheses != 0 || brackets != 0) continue;
        const bool declaration_start = index == 0
            || tokens[index - 1].spelling == ";"
            || tokens[index - 1].spelling == "}";
        if (!declaration_start) continue;
        bool exported = false;
        std::size_t keyword = index;
        if (token == "export" && index + 1 < tokens.size()
            && (tokens[index + 1].spelling == "module"
                || tokens[index + 1].spelling == "import")) {
            exported = true;
            keyword = ++index;
        }
        const bool module_declaration =
            tokens[keyword].spelling == "module";
        const bool import_declaration =
            tokens[keyword].spelling == "import";
        if (!module_declaration && !import_declaration) continue;
        std::size_t semicolon = keyword + 1;
        while (semicolon < tokens.size()
               && tokens[semicolon].spelling != ";") {
            ++semicolon;
        }
        if (semicolon == tokens.size()) {
            throw std::runtime_error(
                source_path.string()
                + ": unterminated module/import declaration in token slice");
        }
        const std::string name = joined_token_spelling(
            tokens, keyword + 1, semicolon);
        const bool module_fragment = name.empty()
            || name == ":private";
        if (module_declaration && !module_fragment) {
            if (!result.provided_module.empty()) {
                throw std::runtime_error(
                    source_path.string()
                    + ": token slice contains multiple named-module declarations");
            }
            const std::size_t declaration_begin = exported
                ? tokens[keyword - 1].range.begin.byte_offset
                : tokens[keyword].range.begin.byte_offset;
            result.declaration_range = {
                .begin = source_position{
                    .byte_offset = declaration_begin,
                    .line = exported
                        ? tokens[keyword - 1].range.begin.line
                        : tokens[keyword].range.begin.line,
                    .column = exported
                        ? tokens[keyword - 1].range.begin.column
                        : tokens[keyword].range.begin.column,
                },
                .end = tokens[semicolon].range.end,
            };
            result.provided_module = name;
            result.is_interface = exported;
            result.is_partition = name.contains(':');
        } else if (import_declaration && !name.empty()) {
            result.imported_modules.push_back(name);
        }
        index = semicolon;
    }
    if (braces != 0 || parentheses != 0 || brackets != 0) {
        throw std::runtime_error(
            source_path.string()
            + ": token slice has unbalanced top-level delimiters");
    }
    std::set<std::string, std::less<>> seen_imports;
    result.imported_modules.erase(
        std::remove_if(
            result.imported_modules.begin(),
            result.imported_modules.end(),
            [&](const std::string& imported) {
                return !seen_imports.insert(imported).second;
            }),
        result.imported_modules.end());
    return result;
}

[[nodiscard]] inline std::vector<namespace_record>
collect_namespace_records(
    const std::filesystem::path& source_path,
    const std::vector<lexical_token>& tokens,
    const std::vector<attribute_block>& blocks) {
    struct open_namespace {
        std::size_t declaration_begin{};
        std::size_t keyword{};
        std::size_t name_begin{};
        std::size_t name_end{};
        std::size_t open_brace{};
        std::string name;
        std::string qualified_name;
        std::string source_identity;
        std::vector<attribute_record> attributes;
        bool is_inline{};
        bool is_anonymous{};
        bool has_explicit_export{};
        bool is_in_export_context{};
    };
    struct brace_frame {
        std::optional<open_namespace> name_space;
        bool pushed_component{};
        bool opens_export_context{};
    };

    std::vector<bool> in_attribute(tokens.size());
    for (const attribute_block& block : blocks) {
        for (std::size_t index = block.token_begin;
             index < std::min(block.token_end, tokens.size());
             ++index) {
            in_attribute[index] = true;
        }
    }
    std::vector<brace_frame> braces;
    std::vector<std::string> named_stack;
    std::vector<namespace_record> result;
    std::size_t export_context_depth = 0;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (in_attribute[index]) continue;
        if (tokens[index].spelling == "{") {
            std::size_t boundary = index;
            while (boundary > 0) {
                if (in_attribute[boundary - 1]) {
                    --boundary;
                    continue;
                }
                const std::string_view previous =
                    tokens[boundary - 1].spelling;
                if (previous == ";" || previous == "{"
                    || previous == "}") {
                    break;
                }
                --boundary;
            }
            std::optional<std::size_t> namespace_keyword;
            for (std::size_t cursor = boundary; cursor < index; ++cursor) {
                if (!in_attribute[cursor]
                    && tokens[cursor].spelling == "namespace") {
                    namespace_keyword = cursor;
                    break;
                }
            }
            if (!namespace_keyword) {
                const bool opens_export_context = std::ranges::any_of(
                    tokens.begin()
                        + static_cast<std::ptrdiff_t>(boundary),
                    tokens.begin()
                        + static_cast<std::ptrdiff_t>(index),
                    [](const lexical_token& candidate) {
                        return candidate.spelling == "export";
                    });
                braces.push_back({
                    .opens_export_context = opens_export_context,
                });
                if (opens_export_context) ++export_context_depth;
                continue;
            }
            const std::size_t keyword = *namespace_keyword;
            std::size_t name_begin = index;
            std::size_t name_end = index;
            std::string name;
            for (std::size_t cursor = keyword + 1;
                 cursor < index;
                 ++cursor) {
                if (in_attribute[cursor]
                    || tokens[cursor].spelling == "inline") {
                    continue;
                }
                const std::string_view spelling =
                    tokens[cursor].spelling;
                if (spelling == "::"
                    || (!spelling.empty()
                        && identifier_begin(spelling.front())
                        && std::ranges::all_of(
                            spelling, identifier_continue))) {
                    if (name.empty() && spelling != "::") {
                        name_begin = cursor;
                    }
                    if (spelling != "::") name_end = cursor + 1;
                    name += spelling;
                }
            }
            std::string qualified;
            for (const std::string& parent : named_stack) {
                if (!qualified.empty()) qualified += "::";
                qualified += parent;
            }
            if (!name.empty()) {
                if (!qualified.empty()) qualified += "::";
                qualified += name;
            } else {
                if (!qualified.empty()) qualified += "::";
                qualified += "<anonymous>";
            }
            std::vector<attribute_record> attributes;
            for (const attribute_block& block : blocks) {
                if (block.token_begin > keyword
                    && block.token_end <= index) {
                    attributes.insert(
                        attributes.end(),
                        block.attributes.begin(),
                        block.attributes.end());
                }
            }
            for (attribute_record& attribute : attributes) {
                attribute.placement = attribute_placement::namespace_head;
            }
            const bool is_inline = std::ranges::any_of(
                tokens.begin() + static_cast<std::ptrdiff_t>(boundary),
                tokens.begin() + static_cast<std::ptrdiff_t>(keyword),
                [](const lexical_token& candidate) {
                    return candidate.spelling == "inline";
                });
            const bool has_explicit_export = std::ranges::any_of(
                tokens.begin()
                    + static_cast<std::ptrdiff_t>(boundary),
                tokens.begin()
                    + static_cast<std::ptrdiff_t>(keyword),
                [](const lexical_token& candidate) {
                    return candidate.spelling == "export";
                });
            open_namespace opened{
                .declaration_begin = boundary,
                .keyword = keyword,
                .name_begin = name_begin,
                .name_end = name_end,
                .open_brace = index,
                .name = name,
                .qualified_name = std::move(qualified),
                .source_identity = source_entity_identity(
                    source_path,
                    "namespace",
                    tokens[keyword].range.begin.byte_offset),
                .attributes = std::move(attributes),
                .is_inline = is_inline,
                .is_anonymous = name.empty(),
                .has_explicit_export = has_explicit_export,
                .is_in_export_context = has_explicit_export
                    || export_context_depth != 0,
            };
            const bool pushed_component = true;
            named_stack.push_back(
                name.empty() ? std::string{"<anonymous>"} : name);
            braces.push_back({
                .name_space = std::move(opened),
                .pushed_component = pushed_component,
                .opens_export_context = has_explicit_export,
            });
            if (has_explicit_export) ++export_context_depth;
            continue;
        }
        if (tokens[index].spelling != "}") continue;
        if (braces.empty()) {
            throw std::runtime_error(
                source_path.string()
                + ": token slice has an unmatched closing brace");
        }
        brace_frame frame = std::move(braces.back());
        braces.pop_back();
        if (frame.opens_export_context) {
            if (export_context_depth == 0) {
                throw std::runtime_error(
                    source_path.string()
                    + ": token slice export-context stack underflow");
            }
            --export_context_depth;
        }
        if (!frame.name_space) continue;
        const open_namespace& opened = *frame.name_space;
        namespace_record record{
            .declaration_range = {
                .begin = tokens[opened.declaration_begin].range.begin,
                .end = tokens[index].range.end,
            },
            .body_range = {
                .begin = tokens[opened.open_brace].range.end,
                .end = tokens[index].range.begin,
            },
            .keyword_range = tokens[opened.keyword].range,
            .name_range = opened.is_anonymous
                ? source_range{}
                : source_range{
                      .begin = tokens[opened.name_begin].range.begin,
                      .end = tokens[opened.name_end - 1].range.end,
                  },
            .open_brace_range = tokens[opened.open_brace].range,
            .close_brace_range = tokens[index].range,
            .source_identity = opened.source_identity,
            .qualified_name = opened.qualified_name,
            .attributes = opened.attributes,
            .is_inline = opened.is_inline,
            .is_anonymous = opened.is_anonymous,
            .is_nested_namespace_definition =
                opened.name.contains("::"),
            .has_explicit_export = opened.has_explicit_export,
            .is_in_export_context = opened.is_in_export_context,
        };
        result.push_back(std::move(record));
        if (frame.pushed_component) named_stack.pop_back();
    }
    if (!braces.empty()) {
        throw std::runtime_error(
            source_path.string()
            + ": token slice has unbalanced source braces");
    }
    std::ranges::sort(
        result,
        {},
        [](const namespace_record& record) {
            return record.declaration_range.begin.byte_offset;
        });
    std::map<std::string, std::size_t, std::less<>> occurrences;
    for (const namespace_record& record : result) {
        ++occurrences[record.qualified_name];
    }
    for (namespace_record& record : result) {
        record.is_reopened = occurrences[record.qualified_name] > 1;
    }
    return result;
}

struct sliced_declaration_bounds {
    std::size_t token_end{};
    std::optional<std::size_t> semicolon;
    std::optional<std::size_t> body_open;
    std::optional<std::size_t> body_close;
    std::optional<std::size_t> initializer;
    std::optional<std::size_t> first_parenthesis;
    std::optional<std::size_t> parameter_close;
    bool function_special_definition{};
    bool multiple_declarators{};
};

[[nodiscard]] inline std::vector<std::size_t> attribute_end_by_begin(
    std::size_t token_count,
    const std::vector<attribute_block>& blocks) {
    std::vector<std::size_t> result(token_count);
    for (const attribute_block& block : blocks) {
        if (block.token_begin < result.size()) {
            result[block.token_begin] = block.token_end;
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<std::size_t> directive_end_at(
    const lexical_token& token,
    const std::vector<preprocessor_directive_record>& directives) {
    for (const preprocessor_directive_record& directive : directives) {
        if (token.range.begin.byte_offset
                >= directive.complete_range.begin.byte_offset
            && token.range.begin.byte_offset
                < directive.complete_range.end.byte_offset) {
            return directive.complete_range.end.byte_offset;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline sliced_declaration_bounds
slice_direct_declaration_bounds(
    const std::filesystem::path& source_path,
    const std::vector<lexical_token>& tokens,
    const std::vector<std::size_t>& attribute_ends,
    const std::vector<preprocessor_directive_record>& directives,
    std::size_t begin,
    std::size_t limit) {
    sliced_declaration_bounds result;
    int parentheses = 0;
    int brackets = 0;
    int angles = 0;
    bool saw_initializer = false;
    bool declaration_scope = false;
    bool constructor_initializer = false;
    bool constructor_component_complete = false;
    bool skipped_requires_body = false;
    for (std::size_t cursor = begin; cursor < limit; ++cursor) {
        if (attribute_ends[cursor] > cursor) {
            cursor = attribute_ends[cursor] - 1;
            continue;
        }
        if (const auto directive_end =
                directive_end_at(tokens[cursor], directives)) {
            cursor = token_at_or_after(tokens, *directive_end);
            if (cursor != 0) --cursor;
            continue;
        }
        const std::string_view spelling = tokens[cursor].spelling;
        const bool delimiter_top_level = parentheses == 0
            && brackets == 0;
        const bool type_top_level = delimiter_top_level
            && angles == 0;
        if (type_top_level && spelling == "=") {
            const bool after_function_parameters =
                result.parameter_close
                && cursor > *result.parameter_close;
            const bool special_function_definition =
                after_function_parameters
                && cursor + 1 < limit
                && (tokens[cursor + 1].spelling == "default"
                    || tokens[cursor + 1].spelling == "delete"
                    || tokens[cursor + 1].spelling == "0");
            if (special_function_definition) {
                result.function_special_definition = true;
            } else {
                if (!result.initializer) result.initializer = cursor;
                saw_initializer = true;
            }
        } else if (type_top_level && spelling == ",") {
            result.multiple_declarators = true;
            if (constructor_initializer) {
                constructor_component_complete = false;
            }
        } else if (delimiter_top_level && spelling == ";") {
            result.semicolon = cursor;
            result.token_end = cursor + 1;
            return result;
        } else if (delimiter_top_level && spelling == ":"
                   && result.parameter_close
                   && cursor > *result.parameter_close
                   && !saw_initializer) {
            constructor_initializer = true;
            constructor_component_complete = false;
        } else if (delimiter_top_level && spelling == "{") {
            if (!result.body_open && !saw_initializer) {
                const bool has_scope_keyword = std::ranges::any_of(
                    tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                    tokens.begin() + static_cast<std::ptrdiff_t>(cursor),
                    [](const lexical_token& token) {
                        return token.spelling == "namespace"
                            || token.spelling == "struct"
                            || token.spelling == "class"
                            || token.spelling == "union"
                            || token.spelling == "enum";
                    });
                declaration_scope = has_scope_keyword
                    || result.first_parenthesis.has_value();
            }
            const std::optional<std::size_t> close =
                matching_token(tokens, cursor, "{", "}");
            if (!close || *close >= limit) {
                throw std::runtime_error(
                    source_path.string() + ":"
                    + std::to_string(tokens[cursor].range.begin.line)
                    + ": direct declaration has unbalanced braces");
            }
            const bool constructor_braced_initializer =
                declaration_scope && constructor_initializer
                && !constructor_component_complete
                && cursor > begin
                && tokens[cursor - 1].spelling != ")"
                && tokens[cursor - 1].spelling != "}";
            bool requires_expression_body = false;
            if (declaration_scope && !skipped_requires_body
                && result.parameter_close
                && cursor > *result.parameter_close) {
                const auto requires_token = std::ranges::find_if(
                    tokens.begin()
                        + static_cast<std::ptrdiff_t>(
                            *result.parameter_close + 1),
                    tokens.begin()
                        + static_cast<std::ptrdiff_t>(cursor),
                    [](const lexical_token& token) {
                        return token.spelling == "requires";
                    });
                if (requires_token
                    != tokens.begin()
                        + static_cast<std::ptrdiff_t>(cursor)) {
                    const std::string_view previous =
                        tokens[cursor - 1].spelling;
                    requires_expression_body = previous == "requires"
                        || (previous == ")"
                            && std::ranges::count_if(
                                requires_token,
                                tokens.begin()
                                    + static_cast<std::ptrdiff_t>(cursor),
                                [](const lexical_token& token) {
                                    return token.spelling == "requires";
                                }) >= 2);
                }
            }
            if (constructor_braced_initializer
                || requires_expression_body) {
                cursor = *close;
                constructor_component_complete =
                    constructor_braced_initializer;
                skipped_requires_body = requires_expression_body;
                continue;
            }
            if (declaration_scope && !saw_initializer) {
                result.body_open = cursor;
                result.body_close = *close;
                result.token_end = *close + 1;
                if (result.token_end < limit
                    && tokens[result.token_end].spelling == ";") {
                    result.semicolon = result.token_end;
                    ++result.token_end;
                }
                return result;
            }
            if (!result.initializer) result.initializer = cursor;
            saw_initializer = true;
            cursor = *close;
            continue;
        }

        if (spelling == "(") {
            if (type_top_level && !saw_initializer
                && !result.first_parenthesis) {
                result.first_parenthesis = cursor;
            }
            ++parentheses;
        } else if (spelling == ")") {
            --parentheses;
            if (parentheses == 0 && result.first_parenthesis) {
                if (!result.parameter_close) {
                    result.parameter_close = cursor;
                } else if (constructor_initializer
                           && cursor > *result.parameter_close) {
                    constructor_component_complete = true;
                }
            }
        } else if (spelling == "[") {
            ++brackets;
        } else if (spelling == "]") {
            --brackets;
        } else if (spelling == "<" && !saw_initializer
                   && (cursor == begin
                       || tokens[cursor - 1].spelling != "operator")) {
            ++angles;
        } else if (spelling == ">" && angles != 0) {
            --angles;
        } else if (spelling == ">>" && angles >= 2) {
            angles -= 2;
        }
        if (parentheses < 0 || brackets < 0 || angles < 0) {
            throw std::runtime_error(
                source_path.string() + ":"
                + std::to_string(tokens[cursor].range.begin.line)
                + ": direct declaration has unbalanced tokens");
        }
    }
    throw std::runtime_error(
        source_path.string() + ":"
        + std::to_string(tokens[begin].range.begin.line)
        + ": cannot prove the end of a direct namespace declaration");
}

[[nodiscard]] inline bool declaration_keyword(
    std::string_view spelling) noexcept {
    return spelling == "export" || spelling == "static"
        || spelling == "constexpr" || spelling == "inline"
        || spelling == "consteval" || spelling == "extern"
        || spelling == "thread_local" || spelling == "constinit"
        || spelling == "const" || spelling == "volatile"
        || spelling == "mutable" || spelling == "typename"
        || spelling == "template" || spelling == "friend";
}

[[nodiscard]] inline bool type_prefix_specifier(
    std::string_view spelling) noexcept {
    return spelling == "export" || spelling == "static"
        || spelling == "constexpr" || spelling == "inline"
        || spelling == "consteval" || spelling == "extern"
        || spelling == "thread_local" || spelling == "constinit"
        || spelling == "mutable" || spelling == "friend";
}

[[nodiscard]] inline std::optional<std::size_t>
previous_identifier_token(
    const std::vector<lexical_token>& tokens,
    const std::vector<std::size_t>& attribute_ends,
    std::size_t begin,
    std::size_t before) {
    std::vector<bool> in_attribute(tokens.size());
    for (std::size_t block_begin = 0;
         block_begin < attribute_ends.size();
         ++block_begin) {
        for (std::size_t index = block_begin;
             attribute_ends[block_begin] > block_begin
                 && index < attribute_ends[block_begin];
             ++index) {
            in_attribute[index] = true;
        }
    }
    while (before > begin) {
        --before;
        const std::string_view spelling = tokens[before].spelling;
        if (!in_attribute[before] && !spelling.empty()
            && identifier_begin(spelling.front())
            && std::ranges::all_of(spelling, identifier_continue)
            && !declaration_keyword(spelling)) {
            return before;
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string explicit_type_before_name(
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    const std::vector<std::size_t>& attribute_ends,
    std::size_t begin,
    std::size_t name_token) {
    std::size_t type_begin = begin;
    while (type_begin < name_token) {
        if (attribute_ends[type_begin] > type_begin) {
            type_begin = attribute_ends[type_begin];
            continue;
        }
        if (tokens[type_begin].spelling == "template"
            && type_begin + 1 < name_token
            && tokens[type_begin + 1].spelling == "<") {
            int depth = 0;
            std::size_t cursor = type_begin + 1;
            for (; cursor < name_token; ++cursor) {
                if (tokens[cursor].spelling == "<") ++depth;
                else if (tokens[cursor].spelling == ">") --depth;
                else if (tokens[cursor].spelling == ">>") depth -= 2;
                if (depth == 0) {
                    ++cursor;
                    break;
                }
            }
            type_begin = cursor;
            continue;
        }
        if (type_prefix_specifier(tokens[type_begin].spelling)) {
            ++type_begin;
            continue;
        }
        break;
    }
    if (type_begin >= name_token) return {};
    const std::size_t byte_begin =
        tokens[type_begin].range.begin.byte_offset;
    const std::size_t byte_end =
        tokens[name_token].range.begin.byte_offset;
    return std::string{trim_view(
        source.substr(byte_begin, byte_end - byte_begin))};
}

[[nodiscard]] inline declaration_record make_sliced_declaration(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    const std::vector<attribute_block>& blocks,
    const std::vector<std::size_t>& attribute_ends,
    const namespace_record& owner,
    const module_record& source_module,
    std::size_t begin,
    const sliced_declaration_bounds& bounds) {
    const std::size_t end = bounds.token_end;
    const std::size_t head_end = bounds.body_open.value_or(
        bounds.initializer.value_or(
            bounds.semicolon.value_or(end)));
    if (begin >= end || end > tokens.size()
        || head_end >= tokens.size() || head_end < begin) {
        throw std::runtime_error(
            source_path.string()
            + ": token slicer produced invalid declaration bounds");
    }
    std::optional<std::size_t> using_keyword;
    std::optional<std::size_t> record_keyword;
    std::optional<std::size_t> namespace_keyword;
    int classification_parentheses = 0;
    int classification_brackets = 0;
    int classification_angles = 0;
    for (std::size_t cursor = begin; cursor < head_end; ++cursor) {
        if (attribute_ends[cursor] > cursor) {
            cursor = attribute_ends[cursor] - 1;
            continue;
        }
        const std::string_view spelling = tokens[cursor].spelling;
        const bool declaration_level = classification_parentheses == 0
            && classification_brackets == 0
            && classification_angles == 0;
        if (declaration_level) {
            if (spelling == "using" && !using_keyword) {
                using_keyword = cursor;
            } else if ((spelling == "struct" || spelling == "class"
                        || spelling == "union" || spelling == "enum")
                       && !record_keyword) {
                record_keyword = cursor;
            } else if (spelling == "namespace"
                       && !namespace_keyword) {
                namespace_keyword = cursor;
            }
        }
        if (spelling == "(") {
            ++classification_parentheses;
        } else if (spelling == ")") {
            --classification_parentheses;
        } else if (spelling == "[") {
            ++classification_brackets;
        } else if (spelling == "]") {
            --classification_brackets;
        } else if (spelling == "<"
                   && (cursor == begin
                       || tokens[cursor - 1].spelling != "operator")) {
            ++classification_angles;
        } else if (spelling == ">" && classification_angles != 0) {
            --classification_angles;
        } else if (spelling == ">>" && classification_angles >= 2) {
            classification_angles -= 2;
        }
    }

    declaration_kind kind = declaration_kind::variable;
    std::optional<std::size_t> name_token;
    if (namespace_keyword) {
        kind = declaration_kind::namespace_declaration;
        for (std::size_t cursor = *namespace_keyword + 1;
             cursor < head_end;
             ++cursor) {
            const std::string_view spelling = tokens[cursor].spelling;
            if (attribute_ends[cursor] > cursor) {
                cursor = attribute_ends[cursor] - 1;
            } else if (!spelling.empty()
                       && identifier_begin(spelling.front())
                       && spelling != "inline") {
                name_token = cursor;
                break;
            }
        }
    } else if (record_keyword) {
        kind = tokens[*record_keyword].spelling == "enum"
            ? declaration_kind::enumeration
            : declaration_kind::record;
        std::size_t cursor = *record_keyword + 1;
        if (cursor < head_end
            && (tokens[cursor].spelling == "class"
                || tokens[cursor].spelling == "struct")) {
            ++cursor;
        }
        for (; cursor < head_end; ++cursor) {
            const std::string_view spelling = tokens[cursor].spelling;
            if (attribute_ends[cursor] > cursor) {
                cursor = attribute_ends[cursor] - 1;
            } else if (!spelling.empty()
                       && identifier_begin(spelling.front())) {
                name_token = cursor;
                break;
            }
        }
    } else if (using_keyword && bounds.initializer) {
        kind = declaration_kind::alias;
        for (std::size_t cursor = *using_keyword + 1;
             cursor < *bounds.initializer;
             ++cursor) {
            if (attribute_ends[cursor] > cursor) {
                cursor = attribute_ends[cursor] - 1;
            } else if (!tokens[cursor].spelling.empty()
                && identifier_begin(tokens[cursor].spelling.front())) {
                name_token = cursor;
                break;
            }
        }
    } else if (bounds.first_parenthesis
               && (!bounds.initializer
                   || *bounds.first_parenthesis < *bounds.initializer)) {
        kind = declaration_kind::function;
        name_token = previous_identifier_token(
            tokens,
            attribute_ends,
            begin,
            *bounds.first_parenthesis);
    } else {
        kind = declaration_kind::variable;
        const std::size_t before = bounds.initializer.value_or(
            bounds.semicolon.value_or(head_end));
        name_token = previous_identifier_token(
            tokens, attribute_ends, begin, before);
    }
    std::string name = name_token
        ? tokens[*name_token].spelling
        : kind == declaration_kind::namespace_declaration
        ? std::string{"<anonymous>"}
        : std::string{};
    std::optional<std::size_t> namespace_name_end;
    if (kind == declaration_kind::namespace_declaration && name_token) {
        name.clear();
        for (std::size_t cursor = *name_token;
             cursor < head_end;
             ++cursor) {
            if (attribute_ends[cursor] > cursor) {
                cursor = attribute_ends[cursor] - 1;
                continue;
            }
            const std::string_view spelling = tokens[cursor].spelling;
            if (spelling == "::") continue;
            if (spelling.empty()
                || !identifier_begin(spelling.front())
                || spelling == "inline") {
                break;
            }
            if (!name.empty()) name += "::";
            name += spelling;
            namespace_name_end = cursor + 1;
        }
    }
    const std::string qualified_name = owner.qualified_name.empty()
        ? name
        : owner.qualified_name + "::" + name;
    const std::size_t declaration_byte_begin =
        tokens[begin].range.begin.byte_offset;
    const source_range declaration_range{
        .begin = tokens[begin].range.begin,
        .end = tokens[end - 1].range.end,
    };
    const source_range declaration_head_range{
        .begin = tokens[begin].range.begin,
        .end = tokens[head_end].range.begin,
    };

    declaration_payload_kind payload_kind =
        declaration_payload_kind::none;
    source_range payload_range;
    if (bounds.body_open && bounds.body_close) {
        if (kind == declaration_kind::function) {
            payload_kind = declaration_payload_kind::function_body;
            payload_range = {
                .begin = tokens[*bounds.body_open].range.begin,
                .end = tokens[*bounds.body_close].range.end,
            };
        }
    } else if (bounds.initializer) {
        const bool direct_list =
            tokens[*bounds.initializer].spelling == "{";
        const std::size_t payload_begin = *bounds.initializer
            + (direct_list ? 0 : 1);
        const std::size_t payload_end =
            bounds.semicolon.value_or(end);
        if (payload_begin < payload_end) {
            payload_range = {
                .begin = tokens[payload_begin].range.begin,
                .end = tokens[payload_end - 1].range.end,
            };
        }
        payload_kind = kind == declaration_kind::alias
            ? declaration_payload_kind::alias_rhs
            : declaration_payload_kind::variable_initializer;
    }
    std::vector<attribute_record> attributes;
    for (const attribute_block& block : blocks) {
        if (block.token_begin >= begin
            && block.token_end <= head_end) {
            attributes.insert(
                attributes.end(),
                block.attributes.begin(),
                block.attributes.end());
        }
    }
    const auto has_token = [&](std::string_view expected) {
        return std::ranges::any_of(
            tokens.begin() + static_cast<std::ptrdiff_t>(begin),
            tokens.begin() + static_cast<std::ptrdiff_t>(head_end),
            [expected](const lexical_token& token) {
                return token.spelling == expected;
            });
    };
    std::string explicit_type;
    if (kind == declaration_kind::alias && payload_range.end.byte_offset
        > payload_range.begin.byte_offset) {
        explicit_type = std::string{trim_view(source.substr(
            payload_range.begin.byte_offset,
            payload_range.end.byte_offset
                - payload_range.begin.byte_offset))};
    } else if (name_token) {
        explicit_type = explicit_type_before_name(
            source, tokens, attribute_ends, begin, *name_token);
    }
    const bool has_deduced_type = explicit_type == "auto"
        || explicit_type.contains("decltype(auto)");
    const bool has_default_arguments =
        kind == declaration_kind::function
        && bounds.first_parenthesis
        && bounds.parameter_close
        && std::ranges::any_of(
            tokens.begin()
                + static_cast<std::ptrdiff_t>(*bounds.first_parenthesis + 1),
            tokens.begin()
                + static_cast<std::ptrdiff_t>(*bounds.parameter_close),
            [](const lexical_token& token) {
                return token.spelling == "=";
            });
    std::vector<parameter_record> parameters;
    if (kind == declaration_kind::function
        && bounds.first_parenthesis && bounds.parameter_close) {
        parameters = parse_parameters(
            source,
            tokens,
            *bounds.first_parenthesis,
            *bounds.parameter_close,
            blocks);
    }
    const module_attachment attachment =
        !source_module.provided_module.empty()
            && declaration_byte_begin
                >= source_module.declaration_range.end.byte_offset
        ? module_attachment::named_module
        : module_attachment::global_module;
    const bool anonymous_owner = owner.is_anonymous
        || owner.qualified_name.contains("<anonymous>");
    const entity_linkage linkage = anonymous_owner || has_token("static")
        ? entity_linkage::internal
        : attachment == module_attachment::named_module
        ? (has_token("export")
               ? entity_linkage::external
               : entity_linkage::module)
        : entity_linkage::external;
    declaration_record result{
        .kind = kind,
        .name = name,
        .qualified_name = qualified_name,
        .enclosing_qualified_name = owner.qualified_name,
        .signature_text = std::string{trim_view(source.substr(
            declaration_byte_begin,
            tokens[head_end].range.begin.byte_offset
                - declaration_byte_begin))},
        .return_type = explicit_type,
        .initializer_text = payload_range.end.byte_offset
                > payload_range.begin.byte_offset
            ? std::string{source.substr(
                  payload_range.begin.byte_offset,
                  payload_range.end.byte_offset
                      - payload_range.begin.byte_offset)}
            : std::string{},
        .parameters = std::move(parameters),
        .attributes = std::move(attributes),
        .range = declaration_range,
        .name_range = name_token
            ? source_range{
                  .begin = tokens[*name_token].range.begin,
                  .end = namespace_name_end
                      ? tokens[*namespace_name_end - 1].range.end
                      : tokens[*name_token].range.end,
              }
            : source_range{},
        .exported = has_token("export"),
        .source_identity = source_entity_identity(
            source_path, "declaration", declaration_byte_begin),
        .immediate_namespace_source_identity = owner.source_identity,
        .explicit_declared_type = explicit_type,
        .declaration_range = declaration_range,
        .declaration_head_range = declaration_head_range,
        .body_or_initializer_range = payload_range,
        .linkage = linkage,
        .attachment = attachment,
        .payload_kind = payload_kind,
        .specifiers = {
            .direct_namespace_member = true,
            .is_definition = bounds.body_open.has_value()
                || bounds.initializer.has_value()
                || bounds.function_special_definition
                || kind == declaration_kind::alias
                || kind == declaration_kind::namespace_declaration,
            .is_using_alias = kind == declaration_kind::alias,
            .is_variable = kind == declaration_kind::variable,
            .is_function = kind == declaration_kind::function,
            .is_static = has_token("static"),
            .is_constexpr = has_token("constexpr"),
            .is_inline = has_token("inline"),
            .is_consteval = has_token("consteval"),
            .is_extern = has_token("extern"),
            .is_thread_local = has_token("thread_local"),
            .has_explicit_type = !explicit_type.empty()
                && !has_deduced_type,
            .has_deduced_type = has_deduced_type,
            .has_default_arguments = has_default_arguments,
            .is_template = has_token("template"),
            .is_operator = has_token("operator"),
            .has_c_language_linkage = has_token("extern")
                && std::ranges::any_of(
                    tokens.begin() + static_cast<std::ptrdiff_t>(begin),
                    tokens.begin() + static_cast<std::ptrdiff_t>(head_end),
                    [](const lexical_token& token) {
                        return token.spelling == "\"C\"";
                    }),
            .multiple_declarators = bounds.multiple_declarators,
        },
    };
    return result;
}

[[nodiscard]] inline std::vector<declaration_record>
collect_direct_declarations(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<lexical_token>& tokens,
    const std::vector<attribute_block>& blocks,
    const std::vector<preprocessor_directive_record>& directives,
    const module_record& source_module,
    const std::vector<namespace_record>& namespaces) {
    const std::vector<std::size_t> attribute_ends =
        attribute_end_by_begin(tokens.size(), blocks);
    std::vector<declaration_record> result;
    for (const namespace_record& name_space : namespaces) {
        std::size_t cursor = token_at_or_after(
            tokens, name_space.body_range.begin.byte_offset);
        const std::size_t limit = token_at_or_after(
            tokens, name_space.body_range.end.byte_offset);
        while (cursor < limit) {
            if (tokens[cursor].spelling == ";") {
                ++cursor;
                continue;
            }
            if (const auto directive_end =
                    directive_end_at(tokens[cursor], directives)) {
                cursor = token_at_or_after(tokens, *directive_end);
                continue;
            }
            const sliced_declaration_bounds bounds =
                slice_direct_declaration_bounds(
                    source_path,
                    tokens,
                    attribute_ends,
                    directives,
                    cursor,
                    limit);
            if (bounds.token_end <= cursor) {
                throw std::runtime_error(
                    source_path.string()
                    + ": token slicer did not advance a declaration boundary");
            }
            result.push_back(make_sliced_declaration(
                source_path,
                source,
                tokens,
                blocks,
                attribute_ends,
                name_space,
                source_module,
                cursor,
                bounds));
            cursor = bounds.token_end;
        }
    }
    std::ranges::sort(
        result,
        {},
        [](const declaration_record& declaration) {
            return declaration.declaration_range.begin.byte_offset;
        });
    result.erase(
        std::ranges::unique(
            result,
            {},
            [](const declaration_record& declaration) {
                return declaration.source_identity;
            }).begin(),
        result.end());
    return result;
}

inline void populate_slice_overloads_and_references(
    const std::vector<lexical_token>& tokens,
    std::vector<declaration_record>& declarations) {
    std::map<std::string, std::size_t, std::less<>> function_counts;
    for (const declaration_record& declaration : declarations) {
        if (declaration.kind == declaration_kind::function) {
            ++function_counts[declaration.qualified_name];
        }
    }
    for (declaration_record& declaration : declarations) {
        if (declaration.kind == declaration_kind::function) {
            declaration.specifiers.is_overloaded =
                function_counts[declaration.qualified_name] > 1;
        }
        if (declaration.declaration_head_range.end.byte_offset
            <= declaration.declaration_head_range.begin.byte_offset) {
            continue;
        }
        for (const declaration_record& referenced : declarations) {
            if (&referenced == &declaration || referenced.name.empty()
                || referenced.name.contains("::")
                || referenced.enclosing_qualified_name
                    != declaration.enclosing_qualified_name) {
                continue;
            }
            const bool appears = std::ranges::any_of(
                tokens,
                [&](const lexical_token& token) {
                    return token.range.begin.byte_offset
                            >= declaration.declaration_head_range
                                   .begin.byte_offset
                        && token.range.end.byte_offset
                            <= declaration.declaration_head_range
                                   .end.byte_offset
                        && token.spelling == referenced.name;
                });
            if (appears) {
                declaration.referenced_declaration_source_identities
                    .push_back(referenced.source_identity);
            }
        }
        std::ranges::sort(
            declaration.referenced_declaration_source_identities);
        declaration.referenced_declaration_source_identities.erase(
            std::ranges::unique(
                declaration.referenced_declaration_source_identities).begin(),
            declaration.referenced_declaration_source_identities.end());
    }
}

[[nodiscard]] inline translation_unit scan_token_slice_impl(
    const std::filesystem::path& source_path,
    std::string_view source,
    bool allow_probe_source_identity) {
    const source_positions positions{source};
    std::vector<lexical_token> raw_tokens =
        clang_raw_tokens(source_path, source);
    std::vector<preprocessor_directive_record> directives =
        collect_preprocessor_directives(source, raw_tokens, positions);
    std::vector<lexical_token> structural_tokens;
    structural_tokens.reserve(raw_tokens.size());
    std::ranges::copy_if(
        raw_tokens,
        std::back_inserter(structural_tokens),
        [&](const lexical_token& token) {
            return !offset_in_directive(
                token.range.begin.byte_offset, directives);
        });
    const std::vector<attribute_block> blocks =
        collect_attribute_blocks(source, structural_tokens);
    for (const attribute_block& block : blocks) {
        const std::size_t materialization_marker_count =
            std::ranges::count_if(
                block.attributes,
                [](const attribute_record& attribute) {
                    return is_materialization_attribute(attribute)
                        || is_probe_source_identity_attribute(attribute);
                });
        if (materialization_marker_count > 1) {
            throw std::runtime_error(
                source_path.string()
                + ": an attribute specifier cannot contain multiple bioseq materialization markers");
        }
        if (!allow_probe_source_identity
            && std::ranges::any_of(
                block.attributes,
                is_probe_source_identity_attribute)) {
            throw std::runtime_error(
                source_path.string()
                + ": template_flatten_provenance is reserved for generated declaration probes");
        }
    }

    module_record source_module = collect_module_record(
        source_path, structural_tokens, directives);
    std::vector<namespace_record> namespaces =
        collect_namespace_records(
            source_path, structural_tokens, blocks);
    std::vector<declaration_record> declarations =
        collect_direct_declarations(
            source_path,
            source,
            structural_tokens,
            blocks,
            directives,
            source_module,
            namespaces);
    std::set<std::size_t> bound_source_identity_attributes;
    for (declaration_record& declaration : declarations) {
        auto marker = std::ranges::find_if(
            declaration.attributes,
            is_probe_source_identity_attribute);
        if (marker == declaration.attributes.end()) continue;
        if (!allow_probe_source_identity) {
            throw std::runtime_error(
                source_path.string()
                + ": authored token slices cannot carry probe source identities");
        }
        if (std::ranges::find_if(
                std::next(marker),
                declaration.attributes.end(),
                is_probe_source_identity_attribute)
            != declaration.attributes.end()) {
            throw std::runtime_error(
                source_path.string()
                + ": a declaration probe has multiple source-identity markers");
        }
        const auto [original_source_identity, coordinate_ordinal] =
            parse_probe_source_identity_arguments(marker->argument_text);
        if (original_source_identity.empty() || coordinate_ordinal.empty()
            || !std::ranges::all_of(
                coordinate_ordinal,
                [](char value) { return value >= '0' && value <= '9'; })) {
            throw std::runtime_error(
                source_path.string()
                + ": declaration probe source identity/coordinate is invalid");
        }
        declaration.source_identity = original_source_identity;
        declaration.source_coordinate = coordinate_ordinal;
        bound_source_identity_attributes.insert(
            marker->removal_range.begin.byte_offset);
        declaration.attributes.erase(marker);
    }
    if (allow_probe_source_identity) {
        for (const attribute_block& block : blocks) {
            for (const attribute_record& attribute : block.attributes) {
                if (is_probe_source_identity_attribute(attribute)
                    && !bound_source_identity_attributes.contains(
                        attribute.removal_range.begin.byte_offset)) {
                    throw std::runtime_error(
                        source_path.string()
                        + ": declaration probe source-identity marker has no target");
                }
            }
        }
    }
    populate_slice_overloads_and_references(
        structural_tokens, declarations);

    translation_unit result{
        .source_path = source_path,
        .module_name = source_module.provided_module,
        .source_sha256 = bioseq::common::sha256_hex(source),
        .tokens = std::move(raw_tokens),
        .declarations = std::move(declarations),
        .mode = collection_mode::token_slice,
        .source_module = std::move(source_module),
        .namespaces = std::move(namespaces),
        .preprocessor_directives = std::move(directives),
    };
    for (const attribute_block& block : blocks) {
        if (!std::ranges::any_of(
                block.attributes,
                is_simd_namespace_marker)) {
            continue;
        }
        const subject target = find_subject(structural_tokens, block);
        if (target.kind != declaration_kind::namespace_declaration
            || !target.namespace_close_brace) {
            throw std::runtime_error(
                source_path.string()
                + ": bioseq::simd must annotate a balanced named namespace");
        }
        std::string qualified_name = target.name;
        const auto semantic_namespace = std::ranges::find_if(
            result.namespaces,
            [&](const namespace_record& candidate) {
                return candidate.name_range.begin.byte_offset
                        == structural_tokens[target.name_token]
                               .range.begin.byte_offset
                    || candidate.qualified_name.ends_with(
                        "::" + target.name)
                    || candidate.qualified_name == target.name;
            });
        if (semantic_namespace != result.namespaces.end()) {
            qualified_name = semantic_namespace->qualified_name;
        }
        result.marked_namespaces.push_back(make_marked_namespace(
            source,
            structural_tokens,
            block,
            target,
            qualified_name));
    }
    return result;
}

[[nodiscard]] inline bool record_has_default_member_initializer(
    const declaration_record& declaration,
    const std::vector<lexical_token>& tokens) {
    int braces = 0;
    int parentheses = 0;
    int brackets = 0;
    bool member_has_using = false;
    bool member_has_parenthesis = false;
    for (const lexical_token& token : tokens) {
        if (token.range.begin.byte_offset
                < declaration.declaration_range.begin.byte_offset
            || token.range.end.byte_offset
                > declaration.declaration_range.end.byte_offset) {
            continue;
        }
        if (token.spelling == "{") {
            ++braces;
            continue;
        }
        if (token.spelling == "}") {
            --braces;
            continue;
        }
        if (braces != 1) continue;
        if (token.spelling == "(") {
            ++parentheses;
            member_has_parenthesis = true;
        } else if (token.spelling == ")") {
            --parentheses;
        } else if (token.spelling == "[") {
            ++brackets;
        } else if (token.spelling == "]") {
            --brackets;
        } else if (token.spelling == ";") {
            member_has_using = false;
            member_has_parenthesis = false;
        } else if (token.spelling == "using") {
            member_has_using = true;
        } else if (token.spelling == "=" && parentheses == 0
                   && brackets == 0 && !member_has_using
                   && !member_has_parenthesis) {
            return true;
        }
    }
    return false;
}

inline void validate_body_free_probe_syntax(
    const translation_unit& probe) {
    for (const declaration_record& declaration : probe.declarations) {
        if (declaration.kind == declaration_kind::function
            && declaration.payload_kind
                == declaration_payload_kind::function_body) {
            throw std::runtime_error(
                probe.source_path.string()
                + ": declaration probe contains a function body");
        }
        if ((declaration.kind == declaration_kind::record
             || declaration.kind == declaration_kind::enumeration)
            && declaration.specifiers.is_definition) {
            if (record_has_default_member_initializer(
                    declaration, probe.tokens)) {
                throw std::runtime_error(
                    probe.source_path.string()
                    + ": declaration probe contains a default member initializer");
            }
            throw std::runtime_error(
                probe.source_path.string()
                + ": declaration probe contains a record or enum definition");
        }
        if (declaration.payload_kind
                == declaration_payload_kind::variable_initializer
            && std::ranges::any_of(
                probe.tokens,
                [&](const lexical_token& token) {
                    return token.spelling == "{"
                        && token.range.begin.byte_offset
                            >= declaration.body_or_initializer_range
                                   .begin.byte_offset
                        && token.range.end.byte_offset
                            <= declaration.body_or_initializer_range
                                   .end.byte_offset;
                })) {
            throw std::runtime_error(
                probe.source_path.string()
                + ": declaration probe contains a body-bearing initializer");
        }
    }
}

}  // namespace detail

[[nodiscard]] std::string repository_source_identity(
    const std::filesystem::path& source_path) {
    return detail::canonical_repository_source_identity(source_path);
}

[[nodiscard]] translation_unit scan_token_slice(
    const std::filesystem::path& source_path,
    std::string_view source) {
    return detail::scan_token_slice_impl(
        source_path, source, false);
}

[[nodiscard]] translation_unit scan_token_slice(
    const std::filesystem::path& source_path) {
    return scan_token_slice(source_path, read_artifact_bytes(source_path));
}

/// Performs a compiler-independent structural source scan. The resulting POD
/// model is deliberately the same front door consumed by the SQL collector;
/// a Clang-backed producer can replace this implementation without changing
/// collector or renderer BMIs.
[[nodiscard]] translation_unit scan_translation_unit(
    const std::filesystem::path& source_path,
    std::string_view source) {
    if (!detail::source_has_structural_marker(source)) {
        return {
            .source_path = source_path,
        };
    }
    const detail::source_positions positions{source};
    const auto [tokens, comments] = detail::lex_source(source);
    const std::vector<detail::attribute_block> blocks =
        detail::collect_attribute_blocks(source, tokens);
    const std::vector<detail::scope_region> scopes =
        detail::collect_scopes(tokens, blocks);

    translation_unit result{
        .source_path = source_path,
        .source_sha256 = bioseq::common::sha256_hex(source),
        .tokens = tokens,
    };
    for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens[index].spelling == "export"
            && tokens[index + 1].spelling == "module") {
            std::string module_name;
            for (std::size_t cursor = index + 2;
                 cursor < tokens.size() && tokens[cursor].spelling != ";";
                 ++cursor) {
                module_name += tokens[cursor].spelling;
            }
            result.module_name = std::move(module_name);
            break;
        }
    }
    if (result.module_name.ends_with(".sql_rows")) {
        for (const detail::scope_region& scope : scopes) {
            if (scope.kind
                != detail::scope_region::category::record) {
                continue;
            }
            result.aggregate_records.push_back(
                detail::collect_aggregate(
                    source,
                    tokens,
                    comments,
                    blocks,
                    positions,
                    scope));
        }
        std::ranges::sort(
            result.aggregate_records,
            {},
            [](const aggregate_record& aggregate) {
                return aggregate.range.begin.byte_offset;
            });
    }

    std::map<std::string, std::size_t, std::less<>> declaration_by_key;
    for (const detail::attribute_block& block : blocks) {
        const bool relevant = std::ranges::any_of(
            block.attributes,
            [](const attribute_record& attribute) {
                return detail::is_structural_attribute(attribute);
            });
        if (!relevant) continue;
        const detail::subject subject = detail::find_subject(tokens, block);
        if (subject.name.empty()) {
            throw std::runtime_error(
                source_path.string() + ":"
                + std::to_string(block.range.begin.line)
                + ": cannot associate bioseq structural attribute "
                  "with a declaration");
        }
        const std::string scope =
            detail::enclosing_scope(scopes, subject.token_begin);
        const std::string qualified =
            scope.empty() ? subject.name : scope + "::" + subject.name;
        const std::string key = qualified + "#"
            + std::to_string(static_cast<int>(subject.kind)) + "#"
            + std::to_string(subject.name_token);
        auto [entry, inserted] =
            declaration_by_key.emplace(key, result.declarations.size());
        if (inserted) {
            const std::size_t begin_offset = std::min(
                block.range.begin.byte_offset,
                tokens[subject.token_begin].range.begin.byte_offset);
            declaration_record declaration{
                .kind = subject.kind,
                .name = subject.name,
                .qualified_name = qualified,
                .enclosing_qualified_name = scope,
                .signature_text = detail::source_between(
                    source, tokens, subject.token_begin, subject.token_end),
                .active_condition =
                    detail::condition_at(source, begin_offset),
                .range = {
                    .begin = positions.at(begin_offset),
                    .end = tokens[
                        std::min(subject.token_end, tokens.size()) - 1]
                        .range.end,
                },
                .name_range = tokens[subject.name_token].range,
            };
            if (subject.kind == declaration_kind::namespace_declaration
                && subject.namespace_close_brace) {
                declaration.range.end =
                    tokens[*subject.namespace_close_brace].range.end;
            }
            if (subject.kind == declaration_kind::function
                && subject.open_parenthesis && subject.close_parenthesis) {
                declaration.return_type = detail::source_between(
                    source,
                    tokens,
                    subject.token_begin,
                    subject.name_token);
                declaration.parameters = detail::parse_parameters(
                    source,
                    tokens,
                    *subject.open_parenthesis,
                    *subject.close_parenthesis,
                    blocks);
                std::size_t body_open = *subject.close_parenthesis + 1;
                while (body_open < tokens.size()
                       && tokens[body_open].spelling != "{"
                       && tokens[body_open].spelling != ";") {
                    ++body_open;
                }
                if (body_open < tokens.size()
                    && tokens[body_open].spelling == "{") {
                    if (const auto body_close = detail::matching_token(
                            tokens, body_open, "{", "}")) {
                        declaration.initializer_text =
                            detail::source_between(
                                source,
                                tokens,
                                body_open + 1,
                                *body_close);
                        declaration.range.end =
                            tokens[*body_close].range.end;
                    }
                }
            }
            if (subject.kind == declaration_kind::variable
                || subject.kind == declaration_kind::field
                || subject.kind == declaration_kind::enumerator) {
                declaration.return_type = detail::source_between(
                    source,
                    tokens,
                    subject.token_begin,
                    subject.name_token);
                for (std::size_t cursor = subject.name_token + 1;
                     cursor < subject.token_end && cursor < tokens.size();
                     ++cursor) {
                    if (tokens[cursor].spelling == "=") {
                        declaration.initializer_text =
                            detail::source_between(
                                source,
                                tokens,
                                cursor + 1,
                                subject.token_end);
                        break;
                    }
                    if (tokens[cursor].spelling == "{") {
                        const std::optional<std::size_t> close =
                            detail::matching_token(
                                tokens, cursor, "{", "}");
                        if (!close || *close >= subject.token_end) {
                            throw std::runtime_error(
                                source_path.string() + ":"
                                + std::to_string(
                                    tokens[cursor]
                                        .range.begin.line)
                                + ": annotated direct-list initializer "
                                  "has unbalanced braces");
                        }
                        declaration.initializer_text =
                            detail::source_between(
                                source,
                                tokens,
                                cursor + 1,
                                *close);
                        break;
                    }
                }
            }
            declaration.documentation = detail::documentation_before(
                source, comments, begin_offset);
            result.declarations.push_back(std::move(declaration));
        }
        declaration_record& declaration =
            result.declarations[entry->second];
        const std::array documentation_candidates{
            detail::documentation_before(
                source,
                comments,
                block.range.begin.byte_offset),
            detail::documentation_after(
                source,
                comments,
                block.range.end.byte_offset,
                tokens[subject.name_token].range.begin.byte_offset),
        };
        if (detail::documentation_empty(declaration.documentation)) {
            const auto candidate = std::ranges::find_if(
                documentation_candidates,
                [](const documentation_record& documentation) {
                    return !detail::documentation_empty(documentation);
                });
            if (candidate != documentation_candidates.end()) {
                declaration.documentation = *candidate;
            }
        }
        for (const documentation_record& candidate :
             documentation_candidates) {
            detail::merge_documentation_groups(
                declaration.documentation, candidate);
        }
        for (attribute_record attribute : block.attributes) {
            if (detail::is_structural_attribute(attribute)) {
                if (subject.kind == declaration_kind::enumerator
                    && block.token_begin > 0) {
                    attribute.placement =
                        attribute_placement::enumerator_tail;
                } else if (subject.kind == declaration_kind::parameter) {
                    attribute.placement =
                        attribute_placement::parameter_prefix;
                } else if (block.token_begin > 0
                           && (tokens[block.token_begin - 1].spelling
                                   == "struct"
                               || tokens[block.token_begin - 1].spelling
                                   == "class"
                               || tokens[block.token_begin - 1].spelling
                                   == "union"
                               || tokens[block.token_begin - 1].spelling
                                   == "enum")) {
                    attribute.placement =
                        attribute_placement::class_head;
                } else if (subject.kind
                               == declaration_kind::namespace_declaration
                           && detail::is_simd_namespace_marker(
                               attribute)) {
                    attribute.placement =
                        attribute_placement::namespace_head;
                }
                declaration.attributes.push_back(std::move(attribute));
            }
        }
        if (subject.kind == declaration_kind::namespace_declaration
            && std::ranges::any_of(
                block.attributes,
                detail::is_simd_namespace_marker)) {
            result.marked_namespaces.push_back(
                detail::make_marked_namespace(
                    source,
                    tokens,
                    block,
                    subject,
                    qualified));
        }
    }

    // A natural SQL row owns its ordinary data members even when the members
    // need no SQL-specific override. The collector decides whether those
    // structural fields are implicit table columns or composite attributes.
    std::set<std::string, std::less<>> natural_row_records;
    for (const declaration_record& declaration : result.declarations) {
        if (declaration.kind != declaration_kind::record) continue;
        const bool row = std::ranges::any_of(
            declaration.attributes,
            [&result](const attribute_record& attribute) {
                return attribute.vendor_namespace == "bioseq_sql"
                    && (attribute.name == "table"
                        || (attribute.name == "type"
                            && result.module_name.ends_with(".sql_rows")));
            });
        if (row) {
            natural_row_records.insert(declaration.qualified_name);
        }
    }

    std::set<std::string, std::less<>> known_declarations;
    for (const declaration_record& declaration : result.declarations) {
        known_declarations.insert(declaration.qualified_name);
    }

    for (const detail::scope_region& scope : scopes) {
        if (!natural_row_records.contains(scope.qualified_name)) continue;

        std::size_t member_begin = scope.token_begin + 1;
        int parentheses = 0;
        int brackets = 0;
        int braces = 0;
        int angles = 0;
        for (std::size_t index = member_begin;
             index < scope.token_end;
             ++index) {
            const std::string_view token = tokens[index].spelling;
            if (token == "(") ++parentheses;
            else if (token == ")") --parentheses;
            else if (token == "[") ++brackets;
            else if (token == "]") --brackets;
            else if (token == "{") ++braces;
            else if (token == "}") --braces;
            else detail::adjust_angle_depth(token, angles);

            if (token != ";" || parentheses != 0 || brackets != 0
                || braces != 0 || angles != 0) {
                continue;
            }

            std::size_t begin = member_begin;
            while (begin < index
                   && (tokens[begin].spelling == "public"
                       || tokens[begin].spelling == "private"
                       || tokens[begin].spelling == "protected"
                       || tokens[begin].spelling == ":")) {
                ++begin;
            }
            if (begin == index) {
                member_begin = index + 1;
                continue;
            }

            std::size_t initializer = index;
            int local_parentheses = 0;
            int local_brackets = 0;
            int local_braces = 0;
            int local_angles = 0;
            bool unsupported = false;
            for (std::size_t cursor = begin; cursor < index; ++cursor) {
                const std::string_view current = tokens[cursor].spelling;
                if (current == "(") {
                    if (local_parentheses == 0 && local_brackets == 0
                        && local_braces == 0 && local_angles == 0) {
                        unsupported = true;
                    }
                    ++local_parentheses;
                } else if (current == ")") {
                    --local_parentheses;
                } else if (current == "[") {
                    ++local_brackets;
                } else if (current == "]") {
                    --local_brackets;
                } else if (current == "{") {
                    ++local_braces;
                } else if (current == "}") {
                    --local_braces;
                } else if (current == "<" || current == ">"
                           || current == ">>") {
                    detail::adjust_angle_depth(current, local_angles);
                } else if (current == ","
                           && local_parentheses == 0
                           && local_brackets == 0
                           && local_braces == 0
                           && local_angles == 0) {
                    unsupported = true;
                } else if (current == "="
                           && local_parentheses == 0
                           && local_brackets == 0
                           && local_braces == 0
                           && local_angles == 0) {
                    initializer = cursor;
                    break;
                }
            }
            if (unsupported) {
                throw std::runtime_error(
                    source_path.string() + ":"
                    + std::to_string(tokens[begin].range.begin.line)
                    + ": annotated SQL row records must use one "
                      "ordinary data member per declaration");
            }

            std::size_t name_token = initializer;
            while (name_token > begin) {
                --name_token;
                if (!tokens[name_token].spelling.empty()
                    && detail::identifier_begin(
                        tokens[name_token].spelling.front())
                    && !detail::qualifier(tokens[name_token].spelling)) {
                    break;
                }
            }
            if (name_token == begin
                && (tokens[name_token].spelling.empty()
                    || !detail::identifier_begin(
                        tokens[name_token].spelling.front()))) {
                throw std::runtime_error(
                    source_path.string() + ":"
                    + std::to_string(tokens[begin].range.begin.line)
                    + ": cannot identify an annotated SQL row field");
            }

            const std::string qualified =
                scope.qualified_name + "::"
                + tokens[name_token].spelling;
            if (!known_declarations.contains(qualified)) {
                const std::size_t begin_offset =
                    tokens[begin].range.begin.byte_offset;
                declaration_record field{
                    .kind = declaration_kind::field,
                    .name = tokens[name_token].spelling,
                    .qualified_name = qualified,
                    .enclosing_qualified_name = scope.qualified_name,
                    .signature_text = detail::source_between(
                        source, tokens, begin, index),
                    .return_type = detail::source_between(
                        source, tokens, begin, name_token),
                    .initializer_text =
                        initializer == index
                        ? std::string{}
                        : detail::source_between(
                            source, tokens, initializer + 1, index),
                    .active_condition =
                        detail::condition_at(source, begin_offset),
                    .documentation = detail::documentation_before(
                        source, comments, begin_offset),
                    .range = {
                        .begin = positions.at(begin_offset),
                        .end = tokens[index].range.end,
                    },
                };
                result.declarations.push_back(std::move(field));
                known_declarations.insert(qualified);
            }
            member_begin = index + 1;
        }
    }
    return result;
}

[[nodiscard]] translation_unit scan_translation_unit(
    const std::filesystem::path& source_path) {
    return scan_translation_unit(source_path, read_artifact_bytes(source_path));
}

namespace clang_backend {

struct conditional_event {
    std::size_t byte_offset{};
    std::string expression;
};

struct skipped_region {
    std::size_t begin{};
    std::size_t end{};
};

struct conditional_state {
    std::vector<std::string> stack;
    std::vector<conditional_event> events;
    std::vector<skipped_region> skipped;
};

[[nodiscard]] source_position source_position_of(
    clang::SourceLocation location,
    const clang::SourceManager& sources) {
    location = sources.getSpellingLoc(location);
    if (location.isInvalid() || !location.isFileID()) {
        return {};
    }
    return {
        .byte_offset = sources.getFileOffset(location),
        .line = sources.getSpellingLineNumber(location),
        .column = sources.getSpellingColumnNumber(location),
    };
}

[[nodiscard]] source_range source_range_of(
    clang::SourceRange range,
    const clang::SourceManager& sources,
    const clang::LangOptions& language) {
    if (range.isInvalid()) {
        return {};
    }
    clang::SourceLocation begin = sources.getSpellingLoc(range.getBegin());
    clang::SourceLocation end = sources.getSpellingLoc(range.getEnd());
    if (begin.isInvalid() || end.isInvalid()) {
        return {};
    }
    const clang::SourceLocation after = clang::Lexer::getLocForEndOfToken(
        end, 0, sources, language);
    return {
        .begin = source_position_of(begin, sources),
        .end = source_position_of(
            after.isValid() ? after : end, sources),
    };
}

[[nodiscard]] source_range token_range_of(
    clang::SourceLocation location,
    const clang::SourceManager& sources,
    const clang::LangOptions& language) {
    return source_range_of(
        clang::SourceRange{location, location}, sources, language);
}

[[nodiscard]] std::string source_text(
    clang::SourceRange range,
    const clang::SourceManager& sources,
    const clang::LangOptions& language) {
    if (range.isInvalid()) {
        return {};
    }
    bool invalid = false;
    const llvm::StringRef text = clang::Lexer::getSourceText(
        clang::CharSourceRange::getTokenRange(range),
        sources,
        language,
        &invalid);
    return invalid ? std::string{} : text.str();
}

[[nodiscard]] bool is_main_file_declaration(
    const clang::Decl* declaration,
    const clang::SourceManager& sources) {
    if (declaration == nullptr || declaration->isImplicit()) {
        return false;
    }
    const clang::SourceLocation location =
        sources.getSpellingLoc(declaration->getLocation());
    return location.isValid() && sources.isWrittenInMainFile(location);
}

[[nodiscard]] std::string raw_stable_usr(
    const clang::Decl* declaration) {
    llvm::SmallString<192> value;
    if (declaration == nullptr
        || clang::index::generateUSRForDecl(declaration, value)) {
        return {};
    }
    return value.str().str();
}

[[nodiscard]] bool has_class_structural_template_argument(
    clang::QualType type,
    std::unordered_set<const clang::Type*>& visited);

[[nodiscard]] bool has_class_structural_template_argument(
    const clang::TemplateArgument& argument,
    std::unordered_set<const clang::Type*>& visited) {
    switch (argument.getKind()) {
        case clang::TemplateArgument::Type:
            return has_class_structural_template_argument(
                argument.getAsType(), visited);
        case clang::TemplateArgument::Declaration:
            if (const auto* object =
                    llvm::dyn_cast<clang::TemplateParamObjectDecl>(
                        argument.getAsDecl())) {
                return object->getType()->isRecordType();
            }
            return false;
        case clang::TemplateArgument::StructuralValue:
            return argument.getStructuralValueType()->isRecordType();
        case clang::TemplateArgument::Pack:
            return std::ranges::any_of(
                argument.pack_elements(),
                [&visited](const clang::TemplateArgument& element) {
                    return has_class_structural_template_argument(
                        element, visited);
                });
        case clang::TemplateArgument::Null:
        case clang::TemplateArgument::NullPtr:
        case clang::TemplateArgument::Integral:
        case clang::TemplateArgument::Template:
        case clang::TemplateArgument::TemplateExpansion:
        case clang::TemplateArgument::Expression:
            return false;
    }
    return false;
}

[[nodiscard]] bool has_class_structural_template_argument(
    clang::QualType type,
    std::unordered_set<const clang::Type*>& visited) {
    if (type.isNull()) {
        return false;
    }
    const clang::Type* canonical =
        type.getCanonicalType().getTypePtrOrNull();
    if (canonical == nullptr || !visited.insert(canonical).second) {
        return false;
    }
    if (const auto* record =
            canonical->getAs<clang::RecordType>()) {
        if (const auto* specialization =
                llvm::dyn_cast<
                    clang::ClassTemplateSpecializationDecl>(
                    record->getDecl())) {
            if (std::ranges::any_of(
                    specialization->getTemplateArgs().asArray(),
                    [&visited](
                        const clang::TemplateArgument& argument) {
                        return has_class_structural_template_argument(
                            argument, visited);
                    })) {
                return true;
            }
        }
    }
    if (const auto* pointer =
            llvm::dyn_cast<clang::PointerType>(canonical)) {
        return has_class_structural_template_argument(
            pointer->getPointeeType(), visited);
    }
    if (const auto* reference =
            llvm::dyn_cast<clang::ReferenceType>(canonical)) {
        return has_class_structural_template_argument(
            reference->getPointeeType(), visited);
    }
    if (const auto* array =
            llvm::dyn_cast<clang::ArrayType>(canonical)) {
        return has_class_structural_template_argument(
            array->getElementType(), visited);
    }
    return false;
}

[[nodiscard]] bool has_class_structural_template_argument(
    const clang::FunctionDecl* function) {
    if (function == nullptr) {
        return false;
    }
    for (const clang::ParmVarDecl* parameter :
         function->parameters()) {
        std::unordered_set<const clang::Type*> visited;
        if (has_class_structural_template_argument(
                parameter->getType(), visited)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::string compiler_owned_function_identity(
    const clang::FunctionDecl* function,
    std::string_view owning_module) {
    if (function == nullptr || function->getDeclName().isEmpty()
        || owning_module.empty()
        || function->getTemplatedKind()
            != clang::FunctionDecl::TK_NonTemplate
        || function->getType().isNull()
        || function->getType()->isDependentType()
        || function->getType()
                ->getAs<clang::FunctionProtoType>()
            == nullptr
        || !has_class_structural_template_argument(function)) {
        return {};
    }
    std::unique_ptr<clang::MangleContext> mangler{
        function->getASTContext().createMangleContext()};
    if (!mangler
        || !mangler->shouldMangleDeclName(function)) {
        return {};
    }
    llvm::SmallString<256> mangled;
    llvm::raw_svector_ostream output{mangled};
    mangler->mangleName(clang::GlobalDecl{function}, output);
    if (mangled.empty()) {
        return {};
    }
    return "clang-fallback:function:"
        + std::string{owning_module} + ":" + mangled.str().str();
}

[[nodiscard]] bool compiler_owned_function_identity(
    std::string_view identity) noexcept {
    return identity.starts_with(
        "clang-fallback:function:");
}

[[nodiscard]] clang::PrintingPolicy printing_policy(
    const clang::ASTContext& context) {
    clang::PrintingPolicy policy{context.getLangOpts()};
    policy.FullyQualifiedName = true;
    policy.SuppressTagKeyword = true;
    policy.SuppressScope = false;
    policy.Bool = true;
    policy.TerseOutput = false;
    policy.PolishForDeclaration = true;
    return policy;
}

[[nodiscard]] std::string enclosing_name(
    std::string_view qualified_name,
    std::string_view name) {
    if (qualified_name.size() <= name.size()) {
        return {};
    }
    const std::size_t separator = qualified_name.rfind("::");
    return separator == std::string_view::npos
        ? std::string{}
        : std::string{qualified_name.substr(0, separator)};
}

[[nodiscard]] aggregate_kind record_kind_of(
    const clang::RecordDecl* record) {
    if (record->isUnion()) {
        return aggregate_kind::union_record;
    }
    if (record->isStruct()) {
        return aggregate_kind::structure;
    }
    return aggregate_kind::class_record;
}

[[nodiscard]] member_access access_of(clang::AccessSpecifier access) {
    switch (access) {
        case clang::AS_private: return member_access::private_access;
        case clang::AS_protected: return member_access::protected_access;
        case clang::AS_public:
        case clang::AS_none: return member_access::public_access;
    }
    return member_access::public_access;
}

[[nodiscard]] const char* member_kind_of(
    const clang::FunctionDecl* function) {
    if (llvm::isa<clang::CXXConstructorDecl>(function)) {
        return "constructor";
    }
    if (llvm::isa<clang::CXXDestructorDecl>(function)) {
        return "destructor";
    }
    if (llvm::isa<clang::CXXConversionDecl>(function)) {
        return "conversion";
    }
    if (function->isOverloadedOperator()) {
        return "operator";
    }
    if (llvm::isa<clang::CXXMethodDecl>(function)) {
        return "method";
    }
    return "free";
}

[[nodiscard]] callable_kind callable_kind_of(
    const clang::FunctionDecl* function) noexcept {
    if (llvm::isa<clang::CXXConstructorDecl>(function)) {
        return callable_kind::constructor;
    }
    if (llvm::isa<clang::CXXDestructorDecl>(function)) {
        return callable_kind::destructor;
    }
    if (llvm::isa<clang::CXXConversionDecl>(function)) {
        return callable_kind::conversion_function;
    }
    if (function->isOverloadedOperator()) {
        return callable_kind::operator_function;
    }
    if (llvm::isa<clang::CXXMethodDecl>(function)) {
        return callable_kind::member_function;
    }
    return callable_kind::function;
}

[[nodiscard]] std::string normalized_function_signature(
    const clang::FunctionDecl* function,
    const clang::PrintingPolicy& policy) {
    std::string result = function->getQualifiedNameAsString() + "(";
    bool first = true;
    for (const clang::ParmVarDecl* parameter : function->parameters()) {
        if (!first) {
            result += ", ";
        }
        result += parameter->getType().getAsString(policy);
        first = false;
    }
    result += ")";
    if (const auto* method =
            llvm::dyn_cast<clang::CXXMethodDecl>(function)) {
        if (method->isConst()) {
            result += " const";
        }
        if (method->isVolatile()) {
            result += " volatile";
        }
        if (method->getRefQualifier() == clang::RQ_LValue) {
            result += " &";
        } else if (method->getRefQualifier() == clang::RQ_RValue) {
            result += " &&";
        }
    }
    return result;
}

void append_comment_text(
    const clang::comments::Comment* comment,
    std::string& output) {
    if (comment == nullptr) {
        return;
    }
    if (const auto* text =
            llvm::dyn_cast<clang::comments::TextComment>(comment)) {
        if (!output.empty() && !output.ends_with(' ')
            && (text->getText().empty()
                || text->getText().front() != ' ')) {
            output.push_back(' ');
        }
        output += text->getText().str();
        return;
    }
    if (const auto* command =
            llvm::dyn_cast<clang::comments::InlineCommandComment>(
                comment)) {
        for (unsigned index = 0; index < command->getNumArgs(); ++index) {
            if (!output.empty() && !output.ends_with(' ')) {
                output.push_back(' ');
            }
            output += command->getArgText(index).str();
        }
    }
    if (const auto* verbatim =
            llvm::dyn_cast<clang::comments::VerbatimBlockLineComment>(
                comment)) {
        if (!output.empty()) {
            output.push_back('\n');
        }
        output += verbatim->getText().str();
        return;
    }
    if (const auto* verbatim =
            llvm::dyn_cast<clang::comments::VerbatimLineComment>(
                comment)) {
        if (!output.empty()) {
            output.push_back('\n');
        }
        output += verbatim->getText().str();
        return;
    }
    for (auto child = comment->child_begin();
         child != comment->child_end();
         ++child) {
        append_comment_text(*child, output);
    }
}

[[nodiscard]] std::string comment_text(
    const clang::comments::Comment* comment) {
    std::string value;
    append_comment_text(comment, value);
    return std::string{detail::trim_view(value)};
}

[[nodiscard]] documentation_record documentation_of(
    const clang::Decl* declaration,
    clang::ASTContext& context,
    std::string_view usr) {
    documentation_record result;
    result.associated_usr = std::string{usr};
    const clang::comments::FullComment* comment =
        context.getCommentForDecl(declaration, nullptr);
    if (comment == nullptr) {
        const clang::DeclContext* lexical_context =
            declaration->getLexicalDeclContext();
        if (lexical_context != nullptr
            && lexical_context->getDeclKind()
                == clang::Decl::Export) {
            comment = context.getCommentForDecl(
                clang::Decl::castFromDeclContext(lexical_context),
                nullptr);
        }
    }
    if (comment == nullptr) {
        return result;
    }
    const clang::SourceManager& sources = context.getSourceManager();
    result.range = source_range_of(
        comment->getSourceRange(), sources, context.getLangOpts());
    const clang::comments::CommandTraits& traits =
        context.getCommentCommandTraits();
    for (const clang::comments::BlockContentComment* block :
         comment->getBlocks()) {
        if (const auto* parameter =
                llvm::dyn_cast<clang::comments::ParamCommandComment>(
                    block)) {
            result.parameters.push_back(documented_parameter{
                .name = parameter->hasParamName()
                    ? parameter->getParamNameAsWritten().str()
                    : std::string{},
                .text = comment_text(parameter->getParagraph()),
            });
            continue;
        }
        if (const auto* command =
                llvm::dyn_cast<clang::comments::BlockCommandComment>(
                    block)) {
            const clang::comments::CommandInfo* command_info =
                traits.getCommandInfo(command->getCommandID());
            const std::string name = command_info->Name;
            const std::string text = comment_text(command);
            if (command_info->IsBriefCommand) {
                result.has_explicit_brief = true;
                result.brief = text;
            } else if (command_info->IsReturnsCommand) {
                result.returns = text;
            } else if (name == "remark" || name == "remarks"
                       || name == "note" || name == "warning") {
                result.remarks.push_back(text);
            } else if (name == "ingroup" || name == "addtogroup") {
                for (unsigned index = 0;
                     index < command->getNumArgs();
                     ++index) {
                    result.groups.emplace_back(
                        command->getArgText(index).str());
                }
            } else if (name == "code" || name == "example"
                       || name == "snippet") {
                result.runnable_examples.push_back(text);
            } else if (!text.empty()) {
                if (!result.details.empty()) {
                    result.details += "\n\n";
                }
                result.details += text;
            }
            continue;
        }
        const std::string text = comment_text(block);
        if (!text.empty()) {
            if (!result.details.empty()) {
                result.details += "\n\n";
            }
            result.details += text;
        }
    }
    if (result.brief.empty() && !result.details.empty()) {
        const std::size_t sentence = result.details.find('.');
        if (sentence != std::string::npos) {
            result.brief = result.details.substr(0, sentence + 1);
        }
    }
    return result;
}

[[nodiscard]] std::string join_conditions(
    const std::vector<std::string>& conditions) {
    std::string result;
    for (const std::string& condition : conditions) {
        if (condition.empty()) {
            continue;
        }
        if (!result.empty()) {
            result += " && ";
        }
        result += "(" + condition + ")";
    }
    return result;
}

class condition_callbacks final : public clang::PPCallbacks {
  public:
    condition_callbacks(
        conditional_state& target,
        const clang::SourceManager& sources,
        const clang::LangOptions& language)
        : target_(target), sources_(sources), language_(language) {
    }

    void If(
        clang::SourceLocation location,
        clang::SourceRange condition,
        ConditionValueKind) override {
        push(location, source_text(condition, sources_, language_));
    }

    void Ifdef(
        clang::SourceLocation location,
        const clang::Token& macro,
        const clang::MacroDefinition&) override {
        push(location, "defined(" + spelling(macro) + ")");
    }

    void Ifndef(
        clang::SourceLocation location,
        const clang::Token& macro,
        const clang::MacroDefinition&) override {
        push(location, "!defined(" + spelling(macro) + ")");
    }

    void Elif(
        clang::SourceLocation location,
        clang::SourceRange condition,
        ConditionValueKind,
        clang::SourceLocation) override {
        replace(location, source_text(condition, sources_, language_));
    }

    void Else(
        clang::SourceLocation location,
        clang::SourceLocation) override {
        if (!target_.stack.empty()) {
            target_.stack.back() =
                "!(" + target_.stack.back() + ")";
        }
        record(location);
    }

    void Endif(
        clang::SourceLocation location,
        clang::SourceLocation) override {
        if (!target_.stack.empty()) {
            target_.stack.pop_back();
        }
        record(location);
    }

    void SourceRangeSkipped(
        clang::SourceRange range,
        clang::SourceLocation) override {
        if (!in_main_file(range.getBegin())
            || !in_main_file(range.getEnd())) {
            return;
        }
        const source_range converted =
            source_range_of(range, sources_, language_);
        target_.skipped.push_back({
            .begin = converted.begin.byte_offset,
            .end = converted.end.byte_offset,
        });
    }

  private:
    [[nodiscard]] bool in_main_file(
        clang::SourceLocation location) const {
        location = sources_.getSpellingLoc(location);
        return location.isValid()
            && sources_.isWrittenInMainFile(location);
    }

    [[nodiscard]] std::string spelling(
        const clang::Token& token) const {
        bool invalid = false;
        std::string result = clang::Lexer::getSpelling(
            token, sources_, language_, &invalid);
        return invalid ? std::string{} : result;
    }

    void push(
        clang::SourceLocation location,
        std::string expression) {
        target_.stack.push_back(std::move(expression));
        record(location);
    }

    void replace(
        clang::SourceLocation location,
        std::string expression) {
        if (target_.stack.empty()) {
            target_.stack.push_back(std::move(expression));
        } else {
            target_.stack.back() = std::move(expression);
        }
        record(location);
    }

    void record(clang::SourceLocation location) {
        if (!in_main_file(location)) {
            return;
        }
        target_.events.push_back({
            .byte_offset =
                source_position_of(location, sources_).byte_offset,
            .expression = join_conditions(target_.stack),
        });
    }

    conditional_state& target_;
    const clang::SourceManager& sources_;
    const clang::LangOptions& language_;
};

[[nodiscard]] std::string condition_at(
    const conditional_state& conditions,
    std::size_t byte_offset) {
    std::string result;
    for (const conditional_event& event : conditions.events) {
        if (event.byte_offset > byte_offset) {
            break;
        }
        result = event.expression;
    }
    return result;
}

[[nodiscard]] bool skipped_at(
    const conditional_state& conditions,
    std::size_t byte_offset) {
    return std::ranges::any_of(
        conditions.skipped,
        [byte_offset](const skipped_region& region) {
            return byte_offset >= region.begin
                && byte_offset < region.end;
        });
}

[[nodiscard]] std::vector<lexical_token> clang_tokens(
    const clang::SourceManager& sources,
    const clang::LangOptions& language) {
    const clang::FileID main_file = sources.getMainFileID();
    bool invalid = false;
    const llvm::StringRef buffer =
        sources.getBufferData(main_file, &invalid);
    if (invalid) {
        throw std::runtime_error(
            "Clang could not read the structural source buffer");
    }
    const clang::SourceLocation start =
        sources.getLocForStartOfFile(main_file);
    clang::Lexer lexer{
        start,
        language,
        buffer.begin(),
        buffer.begin(),
        buffer.end()};
    lexer.SetCommentRetentionState(false);
    std::vector<lexical_token> result;
    clang::Token token;
    while (!lexer.LexFromRawLexer(token)) {
        if (token.is(clang::tok::eof)) {
            break;
        }
        bool spelling_invalid = false;
        std::string spelling = clang::Lexer::getSpelling(
            token, sources, language, &spelling_invalid);
        if (spelling_invalid || spelling.empty()) {
            continue;
        }
        result.push_back({
            .spelling = std::move(spelling),
            .range = token_range_of(
                token.getLocation(), sources, language),
        });
    }
    return result;
}

struct parameter_location {
    std::size_t declaration{};
    std::size_t parameter{};
};

struct field_location {
    std::size_t aggregate{};
    std::size_t field{};
};

struct callable_candidate {
    callable_insertion_record record;
    bool insertion_proven{};
};

[[nodiscard]] entity_linkage entity_linkage_of(
    const clang::NamedDecl* declaration) {
    switch (declaration->getLinkageInternal()) {
        case clang::Linkage::Invalid:
            throw std::runtime_error(
                "Clang returned invalid linkage for declaration '"
                + declaration->getQualifiedNameAsString() + "'");
        case clang::Linkage::None:
        case clang::Linkage::VisibleNone:
            return entity_linkage::none;
        case clang::Linkage::Internal:
        case clang::Linkage::UniqueExternal:
            return entity_linkage::internal;
        case clang::Linkage::Module:
            return entity_linkage::module;
        case clang::Linkage::External:
            return entity_linkage::external;
    }
    return entity_linkage::none;
}

[[nodiscard]] bool direct_namespace_member(
    const clang::NamedDecl* declaration) noexcept {
    const clang::DeclContext* context =
        declaration->getLexicalDeclContext();
    return context != nullptr && context->isNamespace();
}

[[nodiscard]] bool declaration_range_contains_macro(
    const clang::Decl* declaration) noexcept {
    return declaration->getBeginLoc().isMacroID()
        || declaration->getEndLoc().isMacroID()
        || declaration->getLocation().isMacroID();
}

[[nodiscard]] const clang::NamespaceDecl* immediate_namespace_of(
    const clang::Decl* declaration) noexcept {
    const clang::DeclContext* context = declaration->getDeclContext();
    while (context != nullptr) {
        if (const auto* name_space =
                llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            return name_space;
        }
        context = context->getParent();
    }
    return nullptr;
}

[[nodiscard]] std::string namespace_qualified_name(
    const clang::NamespaceDecl* declaration) {
    if (declaration == nullptr) return {};
    const clang::DeclContext* parent = declaration->getParent();
    while (parent != nullptr
           && !llvm::isa<clang::NamespaceDecl>(parent)) {
        parent = parent->getParent();
    }
    const auto* parent_namespace =
        llvm::dyn_cast_or_null<clang::NamespaceDecl>(parent);
    const std::string parent_name =
        namespace_qualified_name(parent_namespace);
    const std::string component = declaration->isAnonymousNamespace()
        ? std::string{"<anonymous>"}
        : declaration->getNameAsString();
    return parent_name.empty()
        ? component
        : parent_name + "::" + component;
}

class structural_visitor final
    : public clang::RecursiveASTVisitor<structural_visitor> {
  public:
    structural_visitor(
        clang::ASTContext& context,
        translation_unit& output,
        const conditional_state& conditions)
        : context_(context),
          sources_(context.getSourceManager()),
          language_(context.getLangOpts()),
          policy_(printing_policy(context)),
          output_(output),
          conditions_(conditions) {
    }

    bool VisitNamespaceDecl(clang::NamespaceDecl* declaration) {
        bind_namespace_semantics(declaration);
        if (eligible(declaration)
            && !declaration->isAnonymousNamespace()) {
            static_cast<void>(
                add_named(
                    declaration,
                    declaration_kind::namespace_declaration));
        }
        return true;
    }

    void finalize_semantic_facts() {
        std::map<std::string, std::size_t, std::less<>> functions;
        for (const declaration_record& declaration :
             output_.declarations) {
            if (declaration.kind == declaration_kind::function) {
                ++functions[declaration.qualified_name];
            }
        }
        for (declaration_record& declaration : output_.declarations) {
            if (declaration.kind == declaration_kind::function) {
                declaration.specifiers.is_overloaded =
                    functions[declaration.qualified_name] > 1;
            }
        }
    }

    bool VisitCXXRecordDecl(clang::CXXRecordDecl* declaration) {
        if (!eligible(declaration)
            || declaration->isInjectedClassName()) {
            return true;
        }
        const std::size_t declaration_index =
            add_named(declaration, declaration_kind::record);
        output_.declarations[declaration_index].record_kind =
            record_kind_of(declaration);
        if (declaration->isThisDeclarationADefinition()) {
            add_aggregate(declaration);
        }
        return true;
    }

    bool VisitFunctionDecl(clang::FunctionDecl* function) {
        if (!eligible(function)) {
            return true;
        }
        const std::size_t index =
            add_named(function, declaration_kind::function);
        declaration_record& record = output_.declarations[index];
        record.return_type =
            function->getReturnType().getAsString(policy_);
        record.signature_text =
            normalized_function_signature(function, policy_);
        record.member_kind = member_kind_of(function);
        if (function->doesThisDeclarationHaveABody()) {
            if (const clang::Stmt* body = function->getBody()) {
                record.body_or_initializer_range = source_range_of(
                    body->getSourceRange(), sources_, language_);
                record.declaration_head_range.end =
                    record.body_or_initializer_range.begin;
                record.payload_kind =
                    declaration_payload_kind::function_body;
                record.initializer_text = source_text(
                    body->getSourceRange(), sources_, language_);
                if (record.initializer_text.size() >= 2
                    && record.initializer_text.front() == '{'
                    && record.initializer_text.back() == '}') {
                    record.initializer_text =
                        std::string{detail::trim_view(
                            std::string_view{record.initializer_text}
                                .substr(
                                    1,
                                    record.initializer_text.size() - 2))};
                }
            }
        }
        record.explicit_declared_type = record.return_type;
        record.specifiers.is_definition =
            function->doesThisDeclarationHaveABody()
            || function->isDeleted()
            || function->isDefaulted();
        record.specifiers.is_function = true;
        record.specifiers.is_static =
            function->getStorageClass() == clang::SC_Static;
        record.specifiers.is_constexpr =
            function->isConstexprSpecified();
        record.specifiers.is_inline =
            function->isInlineSpecified();
        record.specifiers.is_consteval = function->isConsteval();
        record.specifiers.is_extern =
            function->getStorageClass() == clang::SC_Extern
            || function->getStorageClass() == clang::SC_PrivateExtern;
        record.specifiers.has_deduced_type =
            function->getReturnType()->getContainedAutoType() != nullptr;
        record.specifiers.has_explicit_type =
            !record.specifiers.has_deduced_type
            && !llvm::isa<clang::CXXConstructorDecl>(function)
            && !llvm::isa<clang::CXXDestructorDecl>(function)
            && !llvm::isa<clang::CXXConversionDecl>(function);
        record.specifiers.has_default_arguments =
            std::ranges::any_of(
                function->parameters(),
                [](const clang::ParmVarDecl* parameter) {
                    return parameter->hasDefaultArg();
                });
        record.specifiers.is_template =
            function->getTemplatedKind()
            != clang::FunctionDecl::TK_NonTemplate;
        record.specifiers.is_operator =
            function->isOverloadedOperator()
            || llvm::isa<clang::CXXConversionDecl>(function);
        record.specifiers.has_c_language_linkage =
            function->getLanguageLinkage()
            == clang::CLanguageLinkage;
        record.parameters.reserve(function->getNumParams());
        for (std::size_t ordinal = 0;
             ordinal < function->getNumParams();
             ++ordinal) {
            const clang::ParmVarDecl* parameter =
                function->getParamDecl(ordinal);
            std::string parameter_usr =
                raw_stable_usr(parameter);
            if (parameter_usr.empty()
                && !parameter->getName().empty()
                && compiler_owned_function_identity(
                    record.stable_usr)) {
                parameter_usr = record.stable_usr
                    + "#param:" + std::to_string(ordinal);
            }
            parameter_record item{
                .stable_usr = std::move(parameter_usr),
                .name = parameter->getNameAsString(),
                .cpp_type =
                    parameter->getType().getAsString(policy_),
                .ordinal = ordinal,
                .range = source_range_of(
                    parameter->getSourceRange(),
                    sources_,
                    language_),
                .name_range = token_range_of(
                    parameter->getLocation(),
                    sources_,
                    language_),
            };
            if (parameter->hasDefaultArg()) {
                if (const clang::Expr* value =
                        parameter->getDefaultArg()) {
                    item.default_expression = source_text(
                        value->getSourceRange(),
                        sources_,
                        language_);
                    item.has_default = true;
                }
            }
            record.parameters.push_back(std::move(item));
            const source_position position =
                source_position_of(
                    parameter->getLocation(), sources_);
            parameter_by_name_offset_.emplace(
                position.byte_offset,
                parameter_location{
                    .declaration = index,
                    .parameter = ordinal,
                });
        }
        record_callable(function);
        return true;
    }

    bool VisitLambdaExpr(clang::LambdaExpr* lambda) {
        if (lambda == nullptr || lambda->getBody() == nullptr) {
            return true;
        }
        const auto* body = llvm::dyn_cast<clang::CompoundStmt>(
            lambda->getBody());
        if (body == nullptr) {
            return true;
        }
        const clang::SourceLocation body_begin =
            sources_.getSpellingLoc(body->getBeginLoc());
        const clang::SourceLocation body_end =
            sources_.getSpellingLoc(body->getEndLoc());
        if (body_begin.isInvalid() || body_end.isInvalid()
            || !sources_.isWrittenInMainFile(body_begin)
            || !sources_.isWrittenInMainFile(body_end)) {
            return true;
        }
        const clang::CXXMethodDecl* call_operator =
            lambda->getCallOperator();
        const source_range body_range = source_range_of(
            body->getSourceRange(), sources_, language_);
        const auto direct_main_file_location = [&](clang::SourceLocation location) {
            if (location.isInvalid() || location.isMacroID()) {
                return clang::SourceLocation{};
            }
            location = sources_.getSpellingLoc(location);
            return location.isValid() && sources_.isWrittenInMainFile(location)
                ? location
                : clang::SourceLocation{};
        };
        const clang::SourceLocation lambda_begin =
            direct_main_file_location(lambda->getBeginLoc());
        clang::SourceLocation declarator_end =
            direct_main_file_location(lambda->getIntroducerRange().getEnd());
        if (lambda->hasExplicitParameters() && call_operator != nullptr) {
            const clang::FunctionTypeLoc function_type =
                call_operator->getFunctionTypeLoc();
            if (!function_type.isNull()) {
                declarator_end = direct_main_file_location(
                    function_type.getRParenLoc());
            } else {
                declarator_end = {};
            }
        } else if (const clang::TemplateParameterList* parameters =
                       lambda->getTemplateParameterList()) {
            clang::SourceLocation template_end = parameters->getRAngleLoc();
            if (const clang::Expr* requires_clause =
                    parameters->getRequiresClause()) {
                template_end = requires_clause->getEndLoc();
            }
            declarator_end = direct_main_file_location(template_end);
        }
        source_position insertion = declarator_end.isValid()
            ? token_range_of(declarator_end, sources_, language_).end
            : source_position{};
        bool attribute_geometry_valid = true;
        if (declarator_end.isValid() && !lambda->hasExplicitParameters()) {
            const auto& tokens = output_.tokens;
            std::size_t cursor = static_cast<std::size_t>(std::distance(
                tokens.begin(),
                std::ranges::lower_bound(
                    tokens,
                    insertion.byte_offset,
                    {},
                    [](const lexical_token& token) {
                        return token.range.begin.byte_offset;
                    })));
            while (cursor < tokens.size()
                   && tokens[cursor].range.begin.byte_offset
                       < body_range.begin.byte_offset
                   && detail::attribute_open(tokens, cursor)) {
                std::size_t close = cursor + 2u;
                int nested = 0;
                for (; close + 1u < tokens.size(); ++close) {
                    if (detail::attribute_open(tokens, close)) {
                        ++nested;
                        ++close;
                    } else if (detail::attribute_close(tokens, close)) {
                        if (nested == 0) break;
                        --nested;
                        ++close;
                    }
                }
                if (close + 1u >= tokens.size()
                    || tokens[close + 1u].range.end.byte_offset
                        > body_range.begin.byte_offset) {
                    attribute_geometry_valid = false;
                    break;
                }
                insertion = tokens[close + 1u].range.end;
                cursor = close + 2u;
            }
        }
        callable_candidates_.push_back(callable_candidate{
            .record = {
                .kind = callable_kind::lambda,
                .qualified_name = call_operator == nullptr
                    ? std::string{"<lambda>"}
                    : call_operator->getQualifiedNameAsString(),
                .stable_usr = raw_stable_usr(call_operator),
                .templated = lambda->isGenericLambda(),
                .declaration_range = source_range_of(
                    lambda->getSourceRange(), sources_, language_),
                .body_range = body_range,
                .highway_attribute_insertion = insertion,
            },
            .insertion_proven =
                lambda_begin.isValid()
                    && declarator_end.isValid()
                    && attribute_geometry_valid
                    && insertion.byte_offset
                        >= source_position_of(lambda_begin, sources_).byte_offset
                    && insertion.byte_offset <= body_range.begin.byte_offset
                    && body_range.begin.byte_offset
                        < body_range.end.byte_offset,
        });
        return true;
    }

    bool VisitEnumDecl(clang::EnumDecl* declaration) {
        if (eligible(declaration)) {
            static_cast<void>(
                add_named(declaration, declaration_kind::enumeration));
        }
        return true;
    }

    bool VisitEnumConstantDecl(
        clang::EnumConstantDecl* declaration) {
        if (eligible(declaration)) {
            const std::size_t index =
                add_named(
                    declaration, declaration_kind::enumerator);
            if (const clang::Expr* initializer =
                    declaration->getInitExpr()) {
                output_.declarations[index].initializer_text =
                    source_text(
                        initializer->getSourceRange(),
                        sources_,
                        language_);
            }
        }
        return true;
    }

    bool VisitTypeAliasDecl(clang::TypeAliasDecl* declaration) {
        if (eligible(declaration)) {
            const std::size_t index =
                add_named(declaration, declaration_kind::alias);
            output_.declarations[index].return_type =
                declaration->getUnderlyingType().getAsString(policy_);
            declaration_record& record = output_.declarations[index];
            record.explicit_declared_type = record.return_type;
            record.specifiers.is_using_alias = true;
            record.specifiers.is_definition = true;
            if (const clang::TypeSourceInfo* type_source =
                    declaration->getTypeSourceInfo()) {
                record.body_or_initializer_range = source_range_of(
                    type_source->getTypeLoc().getSourceRange(),
                    sources_,
                    language_);
                record.payload_kind =
                    declaration_payload_kind::alias_rhs;
            }
        }
        return true;
    }

    bool VisitTypedefDecl(clang::TypedefDecl* declaration) {
        if (eligible(declaration)) {
            const std::size_t index =
                add_named(declaration, declaration_kind::alias);
            output_.declarations[index].return_type =
                declaration->getUnderlyingType().getAsString(policy_);
            declaration_record& record = output_.declarations[index];
            record.explicit_declared_type = record.return_type;
            record.specifiers.is_definition = true;
        }
        return true;
    }

    bool VisitConceptDecl(clang::ConceptDecl* declaration) {
        if (eligible(declaration)) {
            static_cast<void>(
                add_named(
                    declaration,
                    declaration_kind::concept_declaration));
        }
        return true;
    }

    bool VisitVarDecl(clang::VarDecl* declaration) {
        if (!eligible(declaration)
            || declaration->isLocalVarDeclOrParm()) {
            return true;
        }
        const std::size_t index =
            add_named(declaration, declaration_kind::variable);
        declaration_record& record = output_.declarations[index];
        record.return_type =
            declaration->getType().getAsString(policy_);
        record.explicit_declared_type = record.return_type;
        record.specifiers.is_variable = true;
        record.specifiers.is_definition =
            declaration->isThisDeclarationADefinition()
            != clang::VarDecl::DeclarationOnly;
        record.specifiers.is_static =
            declaration->getStorageClass() == clang::SC_Static;
        record.specifiers.is_constexpr = declaration->isConstexpr();
        record.specifiers.is_inline =
            declaration->isInlineSpecified();
        record.specifiers.is_extern =
            declaration->getStorageClass() == clang::SC_Extern
            || declaration->getStorageClass() == clang::SC_PrivateExtern;
        record.specifiers.is_thread_local =
            declaration->getTSCSpec() != clang::TSCS_unspecified;
        record.specifiers.has_deduced_type =
            declaration->getType()->getContainedAutoType() != nullptr;
        record.specifiers.has_explicit_type =
            !record.specifiers.has_deduced_type;
        if (const clang::Expr* initializer = declaration->getInit()) {
            record.body_or_initializer_range = source_range_of(
                initializer->getSourceRange(), sources_, language_);
            record.declaration_head_range.end =
                record.body_or_initializer_range.begin;
            record.payload_kind =
                declaration_payload_kind::variable_initializer;
            record.initializer_text = source_text(
                initializer->getSourceRange(), sources_, language_);
        }
        return true;
    }

    bool VisitFieldDecl(clang::FieldDecl* declaration) {
        if (!eligible(declaration)) {
            return true;
        }
        const std::size_t index =
            add_named(declaration, declaration_kind::field);
        declaration_record& record = output_.declarations[index];
        record.return_type =
            declaration->getType().getAsString(policy_);
        record.explicit_declared_type = record.return_type;
        record.specifiers.is_definition = true;
        record.specifiers.has_deduced_type =
            declaration->getType()->getContainedAutoType() != nullptr;
        record.specifiers.has_explicit_type =
            !record.specifiers.has_deduced_type;
        if (const clang::Expr* initializer =
                declaration->getInClassInitializer()) {
            record.body_or_initializer_range = source_range_of(
                initializer->getSourceRange(), sources_, language_);
            record.declaration_head_range.end =
                record.body_or_initializer_range.begin;
            record.payload_kind =
                declaration_payload_kind::variable_initializer;
            record.initializer_text = source_text(
                initializer->getSourceRange(), sources_, language_);
        }
        return true;
    }

    void bind_attributes(
        std::string_view source,
        const std::vector<detail::attribute_block>& blocks,
        const std::vector<lexical_token>& tokens) {
        for (const detail::attribute_block& block : blocks) {
            const bool relevant = std::ranges::any_of(
                block.attributes,
                [](const attribute_record& attribute) {
                    return detail::is_structural_attribute(attribute);
                });
            if (!relevant) {
                continue;
            }
            const detail::subject subject =
                detail::find_subject(tokens, block);
            if (subject.name.empty()
                || subject.name_token >= tokens.size()) {
                throw std::runtime_error(
                    output_.source_path.string() + ":"
                    + std::to_string(block.range.begin.line)
                    + ": Clang token pass cannot associate structural "
                      "attribute with a declaration");
            }
            const std::size_t offset =
                tokens[subject.name_token].range.begin.byte_offset;
            bool attached = false;
            declaration_record* attached_declaration = nullptr;
            if (const auto declaration =
                    declaration_by_name_offset_.find(offset);
                declaration
                != declaration_by_name_offset_.end()) {
                declaration_record& target =
                    output_.declarations[declaration->second];
                append_attributes(
                    target.attributes,
                    block,
                    target.stable_usr,
                    subject,
                    tokens);
                attached_declaration = &target;
                attached = true;
            }
            if (const auto parameter =
                    parameter_by_name_offset_.find(offset);
                parameter != parameter_by_name_offset_.end()) {
                parameter_record& target =
                    output_
                        .declarations[parameter->second.declaration]
                        .parameters[parameter->second.parameter];
                append_attributes(
                    target.attributes,
                    block,
                    target.stable_usr,
                    subject,
                    tokens);
                attached = true;
            }
            if (const auto field =
                    aggregate_field_by_name_offset_.find(offset);
                field
                != aggregate_field_by_name_offset_.end()) {
                aggregate_field_record& target =
                    output_.aggregate_records[field->second.aggregate]
                        .fields[field->second.field];
                append_attributes(
                    target.attributes,
                    block,
                    target.stable_usr,
                    subject,
                    tokens);
                attached = true;
            }
            if (!attached
                && !skipped_at(conditions_, offset)) {
                throw std::runtime_error(
                    output_.source_path.string() + ":"
                    + std::to_string(block.range.begin.line)
                    + ": active structural attribute has no Clang AST "
                      "declaration target");
            }
            if (std::ranges::any_of(
                    block.attributes,
                    detail::is_simd_namespace_marker)) {
                if (attached_declaration == nullptr
                    && skipped_at(conditions_, offset)) {
                    continue;
                }
                if (subject.kind
                        != declaration_kind::namespace_declaration
                    || attached_declaration == nullptr
                    || attached_declaration->kind
                        != declaration_kind::namespace_declaration) {
                    throw std::runtime_error(
                        output_.source_path.string() + ":"
                        + std::to_string(block.range.begin.line)
                        + ": bioseq::simd must annotate a named "
                          "namespace head");
                }
                marked_namespace_record marked =
                    detail::make_marked_namespace(
                        source,
                        tokens,
                        block,
                        subject,
                        attached_declaration->qualified_name,
                        attached_declaration->stable_usr);
                populate_callable_insertions(marked);
                output_.marked_namespaces.push_back(
                    std::move(marked));
            }
        }
    }

  private:
    void bind_namespace_semantics(
        const clang::NamespaceDecl* declaration) {
        if (!is_main_file_declaration(declaration, sources_)) return;
        const std::string qualified =
            namespace_qualified_name(declaration);
        const source_position begin = source_position_of(
            declaration->getBeginLoc(), sources_);
        const source_position name = source_position_of(
            declaration->getLocation(), sources_);
        namespace_record* match = nullptr;
        for (namespace_record& candidate : output_.namespaces) {
            if (candidate.qualified_name != qualified) continue;
            const bool exact_begin =
                candidate.keyword_range.begin.byte_offset
                == begin.byte_offset;
            const bool contains_name =
                !candidate.is_anonymous
                && name.byte_offset
                    >= candidate.name_range.begin.byte_offset
                && name.byte_offset
                    < candidate.name_range.end.byte_offset;
            if (exact_begin || contains_name) {
                match = &candidate;
                if (exact_begin) break;
            }
        }
        if (match == nullptr) return;
        match->stable_usr = raw_stable_usr(declaration);
        match->is_inline = declaration->isInline();
        match->is_anonymous = declaration->isAnonymousNamespace();
        match->is_reopened = match->is_reopened
            || declaration->getCanonicalDecl() != declaration;
        match->is_nested_namespace_definition =
            match->is_nested_namespace_definition
            || declaration->isNested();
        match->is_in_export_context =
            match->is_in_export_context
            || declaration->isInExportDeclContext();
    }

    void populate_callable_insertions(
        marked_namespace_record& marked) const {
        const std::size_t namespace_begin =
            marked.body_range.begin.byte_offset;
        const std::size_t namespace_end =
            marked.body_range.end.byte_offset;
        for (const callable_candidate& candidate :
             callable_candidates_) {
            const std::size_t declaration_begin = candidate.record
                .declaration_range.begin.byte_offset;
            if (declaration_begin < namespace_begin
                || declaration_begin >= namespace_end) {
                continue;
            }
            const std::size_t body_begin =
                candidate.record.body_range.begin.byte_offset;
            const std::size_t body_end =
                candidate.record.body_range.end.byte_offset;
            const std::size_t insertion = candidate.record
                .highway_attribute_insertion.byte_offset;
            const bool lambda_insertion =
                candidate.record.kind == callable_kind::lambda
                && insertion >= declaration_begin
                && insertion <= body_begin;
            const bool function_insertion =
                candidate.record.kind != callable_kind::lambda
                && insertion >= declaration_begin
                && insertion < body_begin;
            if (!candidate.insertion_proven
                || (!lambda_insertion && !function_insertion)
                || body_begin < namespace_begin
                || body_end > namespace_end
                || body_begin >= body_end) {
                throw std::runtime_error(
                    output_.source_path.string() + ":"
                    + std::to_string(
                        candidate.record.declaration_range.begin.line)
                    + ": callable '"
                    + candidate.record.qualified_name
                    + "' in bioseq::simd namespace has no confirmed "
                      "compound-body attribute insertion point");
            }
            marked.callables.push_back(candidate.record);
        }
        std::ranges::sort(
            marked.callables,
            {},
            [](const callable_insertion_record& callable) {
                return callable.highway_attribute_insertion.byte_offset;
            });
        if (std::ranges::adjacent_find(
                marked.callables,
                {},
                [](const callable_insertion_record& callable) {
                    return callable.highway_attribute_insertion.byte_offset;
                }) != marked.callables.end()) {
            throw std::runtime_error(
                output_.source_path.string()
                + ": duplicate callable insertion point in bioseq::simd "
                  "namespace '" + marked.qualified_name + "'");
        }
    }

    void record_callable(const clang::FunctionDecl* function) {
        if (const auto* method =
                llvm::dyn_cast<clang::CXXMethodDecl>(function);
            method != nullptr && method->getParent()->isLambda()) {
            // Lambda call operators need HWY_ATTR immediately after their
            // explicit parameter list (or introducer when parentheses are
            // omitted), before specifiers, standard attributes, or a trailing
            // return type. The LambdaExpr visitor owns that insertion;
            // treating the synthetic call operator as a normal member would
            // record the body twice.
            return;
        }
        if (function->isDefaulted() || function->isDeleted()) {
            // Defaulted and deleted declarations have no authored compound
            // body whose code generation can benefit from a target attribute.
            // Clang may nevertheless report an explicitly defaulted special
            // member as a definition, so exclude both forms before the body
            // test rather than manufacturing an insertion point at `=`.
            return;
        }
        if (!function->doesThisDeclarationHaveABody()) {
            return;
        }
        const clang::Stmt* body = function->getBody();
        if (body == nullptr) {
            return;
        }
        const source_range body_range = source_range_of(
            body->getSourceRange(), sources_, language_);
        const clang::SourceLocation body_begin =
            sources_.getSpellingLoc(body->getBeginLoc());
        const clang::SourceLocation body_end =
            sources_.getSpellingLoc(body->getEndLoc());
        // A declaration prefix may begin with a macro such as HWY_INLINE.
        // Its spelling location belongs to the macro definition (typically a
        // dependency header), while the only writable and semantically valid
        // attribute position is the expansion site in the source owner.
        const clang::SourceLocation declaration_begin =
            sources_.getExpansionLoc(function->getBeginLoc());
        const clang::SourceLocation declaration_end =
            sources_.getExpansionLoc(function->getEndLoc());
        const clang::SourceLocation insertion_location =
            sources_.getExpansionLoc(function->getInnerLocStart());
        const bool main_file_body = body_begin.isValid()
            && body_end.isValid()
            && insertion_location.isValid()
            && sources_.isWrittenInMainFile(body_begin)
            && sources_.isWrittenInMainFile(body_end)
            && sources_.isWrittenInMainFile(insertion_location);
        const bool compound = llvm::isa<clang::CompoundStmt>(body);
        const source_position insertion =
            source_position_of(insertion_location, sources_);
        callable_candidates_.push_back(callable_candidate{
            .record = {
                .kind = callable_kind_of(function),
                .qualified_name =
                    function->getQualifiedNameAsString(),
                .stable_usr = raw_stable_usr(function),
                .templated = function->getTemplatedKind()
                    != clang::FunctionDecl::TK_NonTemplate,
                .declaration_range = source_range_of(
                    clang::SourceRange{
                        declaration_begin,
                        declaration_end},
                    sources_,
                    language_),
                .body_range = body_range,
                .highway_attribute_insertion = insertion,
            },
            .insertion_proven = main_file_body && compound
                && insertion.byte_offset < body_range.begin.byte_offset
                && body_range.begin.byte_offset
                    < body_range.end.byte_offset,
        });
    }

    [[nodiscard]] bool eligible(
        const clang::NamedDecl* declaration) const {
        return !declaration->getDeclName().isEmpty()
            && is_main_file_declaration(declaration, sources_);
    }

    [[nodiscard]] std::size_t add_named(
        const clang::NamedDecl* declaration,
        declaration_kind kind) {
        const source_position name_position =
            source_position_of(declaration->getLocation(), sources_);
        if (const auto found =
                declaration_by_name_offset_.find(
                    name_position.byte_offset);
            found != declaration_by_name_offset_.end()) {
            return found->second;
        }
        const std::string name = declaration->getNameAsString();
        const std::string qualified =
            declaration->getQualifiedNameAsString();
        std::string owning_module = output_.module_name;
        if (const clang::Module* owner =
                declaration->getOwningModule()) {
            owning_module = owner->getFullModuleName();
        }
        std::string usr = raw_stable_usr(declaration);
        if (usr.empty() && kind == declaration_kind::function) {
            if (const auto* function =
                    llvm::dyn_cast<clang::FunctionDecl>(
                        declaration)) {
                usr = compiler_owned_function_identity(
                    function, owning_module);
                if (!usr.empty()) {
                    const auto [existing, inserted] =
                        fallback_function_owners_.emplace(
                            usr, function->getCanonicalDecl());
                    if (!inserted
                        && existing->second
                            != function->getCanonicalDecl()) {
                        throw std::runtime_error(
                            output_.source_path.string() + ":"
                            + std::to_string(name_position.line)
                            + ": compiler-owned function identity "
                              "collides for declaration '"
                            + qualified + "'");
                    }
                }
            }
        }
        declaration_record record{
            .stable_usr = usr,
            .kind = kind,
            .name = name,
            .qualified_name = qualified,
            .enclosing_qualified_name =
                enclosing_name(qualified, name),
            .signature_text = qualified,
            .active_condition =
                condition_at(
                    conditions_, name_position.byte_offset),
            .owning_module = owning_module,
            .documentation =
                documentation_of(declaration, context_, usr),
            .range = source_range_of(
                declaration->getSourceRange(),
                sources_,
                language_),
            .name_range = token_range_of(
                declaration->getLocation(), sources_, language_),
            .exported = declaration->isInExportDeclContext(),
        };
        record.source_identity = detail::source_entity_identity(
            output_.source_path,
            "declaration",
            record.range.begin.byte_offset);
        record.declaration_range = record.range;
        record.declaration_head_range = record.range;
        record.linkage = entity_linkage_of(declaration);
        record.attachment = declaration->isInNamedModule()
            ? module_attachment::named_module
            : module_attachment::global_module;
        record.specifiers.direct_namespace_member =
            direct_namespace_member(declaration);
        record.specifiers.range_contains_macro_expansion =
            declaration_range_contains_macro(declaration);
        record.specifiers.is_using_alias =
            kind == declaration_kind::alias
            && llvm::isa<clang::TypeAliasDecl>(declaration);
        record.specifiers.is_variable =
            kind == declaration_kind::variable;
        record.specifiers.is_function =
            kind == declaration_kind::function;
        if (kind == declaration_kind::namespace_declaration
            || kind == declaration_kind::alias
            || kind == declaration_kind::concept_declaration) {
            record.specifiers.is_definition = true;
        } else if (const auto* tag =
                       llvm::dyn_cast<clang::TagDecl>(declaration)) {
            record.specifiers.is_definition =
                tag->isThisDeclarationADefinition();
        }
        if (const clang::NamespaceDecl* immediate_namespace =
                immediate_namespace_of(declaration)) {
            const std::string immediate_qualified =
                namespace_qualified_name(immediate_namespace);
            const auto namespace_fact = std::ranges::find_if(
                output_.namespaces,
                [&](const namespace_record& candidate) {
                    return candidate.qualified_name
                            == immediate_qualified
                        && record.range.begin.byte_offset
                            >= candidate.body_range.begin.byte_offset
                        && record.range.begin.byte_offset
                            < candidate.body_range.end.byte_offset;
                });
            if (namespace_fact != output_.namespaces.end()) {
                record.immediate_namespace_source_identity =
                    namespace_fact->source_identity;
                record.immediate_namespace_stable_usr =
                    namespace_fact->stable_usr;
            } else {
                record.immediate_namespace_stable_usr =
                    raw_stable_usr(immediate_namespace);
            }
        }
        const std::size_t index = output_.declarations.size();
        output_.declarations.push_back(std::move(record));
        declaration_by_name_offset_.emplace(
            name_position.byte_offset, index);
        return index;
    }

    void add_aggregate(clang::CXXRecordDecl* declaration) {
        const std::string usr = raw_stable_usr(declaration);
        aggregate_record aggregate{
            .stable_usr = usr,
            .name = declaration->getNameAsString(),
            .qualified_name =
                declaration->getQualifiedNameAsString(),
            .enclosing_qualified_name = enclosing_name(
                declaration->getQualifiedNameAsString(),
                declaration->getNameAsString()),
            .kind = record_kind_of(declaration),
            .range = source_range_of(
                declaration->getSourceRange(),
                sources_,
                language_),
            .name_range = token_range_of(
                declaration->getLocation(), sources_, language_),
            .documentation =
                documentation_of(declaration, context_, usr),
            .exported = declaration->isInExportDeclContext(),
            .anonymous =
                declaration->isAnonymousStructOrUnion(),
            .has_base_classes = declaration->getNumBases() != 0,
            .has_virtual_members = declaration->isPolymorphic(),
        };
        std::map<std::size_t, std::size_t> declaration_begins;
        std::size_t ordinal = 0;
        for (const clang::FieldDecl* field : declaration->fields()) {
            const source_position begin =
                source_position_of(field->getBeginLoc(), sources_);
            ++declaration_begins[begin.byte_offset];
            const std::string field_usr = raw_stable_usr(field);
            aggregate.fields.push_back(aggregate_field_record{
                .stable_usr = field_usr,
                .name = field->getNameAsString(),
                .qualified_name =
                    field->getQualifiedNameAsString(),
                .cpp_type =
                    field->getType().getAsString(policy_),
                .initializer_text =
                    field->hasInClassInitializer()
                    ? source_text(
                        field->getInClassInitializer()
                            ->getSourceRange(),
                        sources_,
                        language_)
                    : std::string{},
                .active_condition = condition_at(
                    conditions_,
                    source_position_of(
                        field->getLocation(), sources_)
                        .byte_offset),
                .ordinal = ordinal,
                .documentation =
                    documentation_of(field, context_, field_usr),
                .range = source_range_of(
                    field->getSourceRange(),
                    sources_,
                    language_),
                .name_range = token_range_of(
                    field->getLocation(), sources_, language_),
                .access = access_of(field->getAccess()),
                .has_default_initializer =
                    field->hasInClassInitializer(),
                .bit_field = field->isBitField(),
            });
            ++ordinal;
        }
        for (aggregate_field_record& field : aggregate.fields) {
            if (declaration_begins[
                    field.range.begin.byte_offset]
                > 1) {
                field.multiple_declarators = true;
            }
        }
        const std::size_t aggregate_index =
            output_.aggregate_records.size();
        output_.aggregate_records.push_back(std::move(aggregate));
        const aggregate_record& stored =
            output_.aggregate_records.back();
        for (std::size_t field_index = 0;
             field_index < stored.fields.size();
             ++field_index) {
            aggregate_field_by_name_offset_.emplace(
                stored.fields[field_index]
                    .name_range.begin.byte_offset,
                field_location{
                    .aggregate = aggregate_index,
                    .field = field_index,
                });
        }
    }

    void append_attributes(
        std::vector<attribute_record>& destination,
        const detail::attribute_block& block,
        std::string_view target_usr,
        const detail::subject& subject,
        const std::vector<lexical_token>& tokens) {
        if (target_usr.empty()) {
            throw std::runtime_error(
                output_.source_path.string() + ":"
                + std::to_string(block.range.begin.line)
                + ": annotated declaration '" + subject.name
                + "' has no stable Clang USR");
        }
        for (attribute_record attribute : block.attributes) {
            if (!detail::is_structural_attribute(attribute)) {
                continue;
            }
            attribute.target_usr = std::string{target_usr};
            if (subject.kind == declaration_kind::enumerator
                && block.token_begin > 0) {
                attribute.placement =
                    attribute_placement::enumerator_tail;
            } else if (subject.kind
                       == declaration_kind::parameter) {
                attribute.placement =
                    attribute_placement::parameter_prefix;
            } else if (block.token_begin > 0
                       && (tokens[block.token_begin - 1].spelling
                               == "struct"
                           || tokens[block.token_begin - 1].spelling
                               == "class"
                           || tokens[block.token_begin - 1].spelling
                               == "union"
                           || tokens[block.token_begin - 1].spelling
                               == "enum")) {
                attribute.placement =
                    attribute_placement::class_head;
            } else if (subject.kind
                       == declaration_kind::namespace_declaration
                       && detail::is_simd_namespace_marker(
                           attribute)) {
                attribute.placement =
                    attribute_placement::namespace_head;
            }
            destination.push_back(std::move(attribute));
        }
    }

    clang::ASTContext& context_;
    const clang::SourceManager& sources_;
    const clang::LangOptions& language_;
    clang::PrintingPolicy policy_;
    translation_unit& output_;
    const conditional_state& conditions_;
    std::unordered_map<std::size_t, std::size_t>
        declaration_by_name_offset_;
    std::unordered_map<std::size_t, parameter_location>
        parameter_by_name_offset_;
    std::unordered_map<std::size_t, field_location>
        aggregate_field_by_name_offset_;
    std::unordered_map<std::string, const clang::FunctionDecl*>
        fallback_function_owners_;
    std::vector<callable_candidate> callable_candidates_;
};

class structural_consumer final : public clang::ASTConsumer {
  public:
    structural_consumer(
        std::vector<translation_unit>& output,
        std::shared_ptr<conditional_state> conditions,
        std::string source_path,
        std::string compiler_profile)
        : output_(output),
          conditions_(std::move(conditions)),
          source_path_(std::move(source_path)),
          compiler_profile_(std::move(compiler_profile)) {
    }

    void HandleTranslationUnit(clang::ASTContext& context) override {
        const clang::SourceManager& sources =
            context.getSourceManager();
        bool invalid = false;
        const llvm::StringRef source = sources.getBufferData(
            sources.getMainFileID(), &invalid);
        if (invalid) {
            throw std::runtime_error(
                "Clang structural pass cannot read " + source_path_);
        }
        translation_unit unit{
            .source_path = source_path_,
            .source_sha256 = bioseq::common::sha256_hex(std::string_view{
                source.data(), source.size()}),
            .compiler_profile = compiler_profile_,
            .clang_release = CLANG_VERSION_STRING,
            .tokens = clang_tokens(
                sources, context.getLangOpts()),
            .mode = collection_mode::full_translation_unit,
        };
        const std::string_view source_view{
            source.data(), source.size()};
        const detail::source_positions source_positions{source_view};
        const std::vector<lexical_token> raw_tokens =
            detail::clang_raw_tokens(source_path_, source_view);
        unit.preprocessor_directives =
            detail::collect_preprocessor_directives(
                source_view, raw_tokens, source_positions);
        for (preprocessor_directive_record& directive :
             unit.preprocessor_directives) {
            directive.active = !skipped_at(
                *conditions_,
                directive.complete_range.begin.byte_offset);
        }
        std::vector<lexical_token> structural_tokens;
        structural_tokens.reserve(raw_tokens.size());
        std::ranges::copy_if(
            raw_tokens,
            std::back_inserter(structural_tokens),
            [&](const lexical_token& token) {
                return !detail::offset_in_directive(
                    token.range.begin.byte_offset,
                    unit.preprocessor_directives);
            });
        const std::vector<detail::attribute_block> raw_blocks =
            detail::collect_attribute_blocks(
                source_view, structural_tokens);
        unit.source_module = detail::collect_module_record(
            source_path_,
            structural_tokens,
            unit.preprocessor_directives);
        unit.namespaces = detail::collect_namespace_records(
            source_path_, structural_tokens, raw_blocks);
        if (const clang::Module* module =
                context.getCurrentNamedModule()) {
            unit.module_name = module->getFullModuleName();
        } else {
            unit.module_name = unit.source_module.provided_module;
        }
        const std::vector<detail::attribute_block> blocks =
            detail::collect_attribute_blocks(
                std::string_view{source.data(), source.size()},
                unit.tokens);
        structural_visitor visitor{
            context, unit, *conditions_};
        visitor.TraverseDecl(context.getTranslationUnitDecl());
        visitor.finalize_semantic_facts();
        visitor.bind_attributes(
            std::string_view{source.data(), source.size()},
            blocks,
            unit.tokens);
        const auto lexical = detail::lex_source(source_view);
        const std::vector<detail::comment_record>& comments =
            lexical.second;
        for (declaration_record& declaration : unit.declarations) {
            if (declaration.attributes.empty()) {
                continue;
            }
            std::size_t subject_begin =
                declaration.range.begin.byte_offset;
            for (const attribute_record& attribute :
                 declaration.attributes) {
                subject_begin = std::min(
                    subject_begin,
                    attribute.spelling_range.begin.byte_offset);
            }
            std::vector<documentation_record> lexical_candidates;
            lexical_candidates.push_back(
                detail::documentation_overlapping(
                    comments,
                    declaration.documentation.range));
            lexical_candidates.push_back(
                detail::documentation_before(
                    source_view, comments, subject_begin));
            for (const attribute_record& attribute :
                 declaration.attributes) {
                lexical_candidates.push_back(
                    detail::documentation_before(
                        source_view,
                        comments,
                        attribute.spelling_range.begin.byte_offset));
                lexical_candidates.push_back(
                    detail::documentation_after(
                        source_view,
                        comments,
                        attribute.spelling_range.end.byte_offset,
                        declaration.name_range.begin.byte_offset));
            }
            if (detail::documentation_empty(
                    declaration.documentation)) {
                const auto candidate = std::ranges::find_if(
                    lexical_candidates,
                    [](const documentation_record& documentation) {
                        return !detail::documentation_empty(
                            documentation);
                    });
                if (candidate != lexical_candidates.end()) {
                    declaration.documentation = *candidate;
                }
            }
            for (const documentation_record& candidate :
                 lexical_candidates) {
                detail::merge_documentation_groups(
                    declaration.documentation, candidate);
            }
            declaration.documentation.associated_usr =
                declaration.stable_usr;
        }
        output_.push_back(std::move(unit));
    }

  private:
    std::vector<translation_unit>& output_;
    std::shared_ptr<conditional_state> conditions_;
    std::string source_path_;
    std::string compiler_profile_;
};

class structural_action final : public clang::ASTFrontendAction {
  public:
    structural_action(
        std::vector<translation_unit>& output,
        std::string compiler_profile)
        : output_(output),
          compiler_profile_(std::move(compiler_profile)),
          conditions_(std::make_shared<conditional_state>()) {
    }

    bool BeginSourceFileAction(
        clang::CompilerInstance& compiler) override {
        compiler.getPreprocessor().addPPCallbacks(
            std::make_unique<condition_callbacks>(
                *conditions_,
                compiler.getSourceManager(),
                compiler.getLangOpts()));
        return clang::ASTFrontendAction::BeginSourceFileAction(
            compiler);
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance&,
        llvm::StringRef input_file) override {
        return std::make_unique<structural_consumer>(
            output_,
            conditions_,
            input_file.str(),
            compiler_profile_);
    }

  private:
    std::vector<translation_unit>& output_;
    std::string compiler_profile_;
    std::shared_ptr<conditional_state> conditions_;
};

class structural_factory final
    : public clang::tooling::FrontendActionFactory {
  public:
    structural_factory(
        std::vector<translation_unit>& output,
        std::string compiler_profile)
        : output_(output),
          compiler_profile_(std::move(compiler_profile)) {
    }

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<structural_action>(
            output_, compiler_profile_);
    }

  private:
    std::vector<translation_unit>& output_;
    std::string compiler_profile_;
};

class module_edges_visitor final
    : public clang::RecursiveASTVisitor<module_edges_visitor> {
  public:
    explicit module_edges_visitor(
        const clang::SourceManager& sources) noexcept
        : sources_(sources) {
    }

    module_graph_unit* output{};

    bool VisitImportDecl(clang::ImportDecl* declaration) {
        const clang::SourceLocation location =
            sources_.getExpansionLoc(declaration->getLocation());
        if (location.isInvalid()
            || !sources_.isWrittenInMainFile(location)) {
            return true;
        }
        const clang::Module* imported =
            declaration->getImportedModule();
        if (imported == nullptr) {
            return true;
        }
        std::string name = imported->getFullModuleName();
        if (declaration->isInExportDeclContext()) {
            output->exported_imports.push_back(std::move(name));
        } else {
            output->internal_imports.push_back(std::move(name));
        }
        return true;
    }

  private:
    const clang::SourceManager& sources_;
};

class module_edges_consumer final : public clang::ASTConsumer {
  public:
    explicit module_edges_consumer(
        std::vector<module_graph_unit>& output)
        : output_(output) {
    }

    void HandleTranslationUnit(clang::ASTContext& context) override {
        module_graph_unit unit;
        const clang::SourceManager& sources =
            context.getSourceManager();
        if (const auto file =
                sources.getFileEntryRefForID(
                    sources.getMainFileID())) {
            unit.source_identity = repository_source_identity(
                std::filesystem::path{file->getName().str()});
        }
        if (const clang::Module* module =
                context.getCurrentNamedModule()) {
            unit.provided_module = module->getFullModuleName();
        }
        module_edges_visitor visitor{sources};
        visitor.output = &unit;
        visitor.TraverseDecl(context.getTranslationUnitDecl());
        std::ranges::sort(unit.exported_imports);
        unit.exported_imports.erase(
            std::ranges::unique(unit.exported_imports).begin(),
            unit.exported_imports.end());
        std::ranges::sort(unit.internal_imports);
        unit.internal_imports.erase(
            std::ranges::unique(unit.internal_imports).begin(),
            unit.internal_imports.end());
        output_.push_back(std::move(unit));
    }

  private:
    std::vector<module_graph_unit>& output_;
};

class module_edges_action final : public clang::ASTFrontendAction {
  public:
    explicit module_edges_action(
        std::vector<module_graph_unit>& output)
        : output_(output) {
    }

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance&,
        llvm::StringRef) override {
        return std::make_unique<module_edges_consumer>(output_);
    }

  private:
    std::vector<module_graph_unit>& output_;
};

class module_edges_factory final
    : public clang::tooling::FrontendActionFactory {
  public:
    explicit module_edges_factory(
        std::vector<module_graph_unit>& output)
        : output_(output) {
    }

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<module_edges_action>(output_);
    }

  private:
    std::vector<module_graph_unit>& output_;
};

void append_expanded_argument(
    const std::string& argument,
    clang::tooling::CommandLineArguments& output) {
    if (argument.empty() || argument.front() != '@') {
        output.push_back(argument);
        return;
    }
    std::ifstream stream{argument.substr(1)};
    if (!stream) {
        output.push_back(argument);
        return;
    }
    std::string token;
    while (stream >> token) {
        output.push_back(std::move(token));
    }
}

[[nodiscard]] clang::tooling::ArgumentsAdjuster
structural_arguments_adjuster() {
    return [](
        const clang::tooling::CommandLineArguments& arguments,
        llvm::StringRef) {
        static const std::set<std::string> drop_with_value{
            "-o",
            "-MF",
            "-MT",
            "-MQ",
            "-MJ",
            "--serialize-diagnostics",
        };
        static const std::set<std::string> drop_flags{
            "-c", "--precompile", "-MD", "-MMD", "-MG", "-MP",
            "-M", "-MM",
        };
        clang::tooling::CommandLineArguments expanded;
        for (const std::string& argument : arguments) {
            append_expanded_argument(argument, expanded);
        }
        clang::tooling::CommandLineArguments result;
        result.reserve(expanded.size() + 3);
        for (std::size_t index = 0;
             index < expanded.size();
             ++index) {
            const std::string& argument = expanded[index];
            if (drop_flags.contains(argument)) {
                continue;
            }
            if (drop_with_value.contains(argument)) {
                ++index;
                continue;
            }
            if (argument.starts_with("-fmodule-output")
                || argument.starts_with("-MF")
                || argument.starts_with("-MT")
                || argument.starts_with("-MQ")
                || argument.starts_with("-MJ")) {
                continue;
            }
            result.push_back(argument);
        }
        result.emplace_back("-fsyntax-only");
        result.emplace_back("-Wno-unknown-attributes");
        result.emplace_back("-Wno-ignored-attributes");
        return result;
    };
}

class scoped_compile_database_cwd {
  public:
    explicit scoped_compile_database_cwd(
        const std::filesystem::path& compile_database) {
        if (llvm::sys::fs::current_path(previous_)) {
            return;
        }
        const std::string database_path =
            compile_database.string();
        const llvm::StringRef directory =
            llvm::sys::path::parent_path(
                database_path);
        if (!directory.empty()) {
            restore_ = !static_cast<bool>(
                llvm::sys::fs::set_current_path(directory));
        }
    }

    scoped_compile_database_cwd(
        const scoped_compile_database_cwd&) = delete;
    scoped_compile_database_cwd& operator=(
        const scoped_compile_database_cwd&) = delete;

    ~scoped_compile_database_cwd() {
        if (restore_) {
            static_cast<void>(
                llvm::sys::fs::set_current_path(previous_));
        }
    }

  private:
    llvm::SmallString<256> previous_;
    bool restore_{};
};

[[nodiscard]] bool command_is_satisfiable(
    const clang::tooling::CompileCommand& command) {
    for (const std::string& argument : command.CommandLine) {
        if (!argument.empty() && argument.front() == '@'
            && !llvm::sys::fs::exists(argument.substr(1))) {
            return false;
        }
    }
    return true;
}

class satisfiable_compilation_database final
    : public clang::tooling::CompilationDatabase {
  public:
    explicit satisfiable_compilation_database(
        const clang::tooling::CompilationDatabase& inner)
        : inner_(inner) {
    }

    std::vector<clang::tooling::CompileCommand>
    getCompileCommands(llvm::StringRef file) const override {
        std::vector<clang::tooling::CompileCommand> commands =
            inner_.getCompileCommands(file);
        for (clang::tooling::CompileCommand& command : commands) {
            if (command_is_satisfiable(command)) {
                return {std::move(command)};
            }
        }
        return {};
    }

    std::vector<std::string> getAllFiles() const override {
        return inner_.getAllFiles();
    }

    std::vector<clang::tooling::CompileCommand>
    getAllCompileCommands() const override {
        return inner_.getAllCompileCommands();
    }

  private:
    const clang::tooling::CompilationDatabase& inner_;
};

[[nodiscard]] std::vector<std::string> buildable_units(
    const clang::tooling::CompilationDatabase& database,
    const std::vector<std::filesystem::path>& source_paths) {
    std::vector<std::string> result;
    result.reserve(source_paths.size());
    for (const std::filesystem::path& path : source_paths) {
        for (const clang::tooling::CompileCommand& command :
             database.getCompileCommands(path.string())) {
            if (command_is_satisfiable(command)) {
                result.push_back(path.string());
                break;
            }
        }
    }
    return result;
}

struct lexical_module_description {
    std::string provided_module;
    std::vector<std::string> imports;
};

[[nodiscard]] std::string module_name_from_tokens(
    const std::vector<lexical_token>& tokens,
    std::size_t begin,
    std::size_t end) {
    std::string result;
    for (std::size_t index = begin; index < end; ++index) {
        const std::string_view spelling = tokens[index].spelling;
        if (spelling == "." || spelling == ":") {
            result += spelling;
            continue;
        }
        if (spelling.empty()
            || !detail::identifier_begin(spelling.front())
            || !std::ranges::all_of(
                spelling,
                [](char value) {
                    return detail::identifier_continue(value);
                })) {
            return {};
        }
        result += spelling;
    }
    return result;
}

[[nodiscard]] lexical_module_description describe_module_provider(
    const std::filesystem::path& source_path) {
    const std::string source = read_artifact_bytes(source_path);
    const auto [tokens, comments] = detail::lex_source(source);
    static_cast<void>(comments);
    lexical_module_description result;
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        bool exported = false;
        if (tokens[index].spelling == "export"
            && index + 1 < tokens.size()
            && (tokens[index + 1].spelling == "module"
                || tokens[index + 1].spelling == "import")) {
            exported = true;
            ++index;
        }
        const bool module_declaration =
            tokens[index].spelling == "module";
        if (!module_declaration
            && tokens[index].spelling != "import") {
            continue;
        }
        std::size_t end = index + 1;
        while (end < tokens.size()
               && tokens[end].spelling != ";") {
            ++end;
        }
        if (end == tokens.size()) {
            throw std::runtime_error(
                source_path.string()
                + ": unterminated module/import declaration");
        }
        const std::string name =
            module_name_from_tokens(tokens, index + 1, end);
        if (module_declaration && exported && !name.empty()) {
            if (!result.provided_module.empty()
                && result.provided_module != name) {
                throw std::runtime_error(
                    source_path.string()
                    + ": provides more than one named module");
            }
            result.provided_module = name;
        }
        index = end;
    }
    return result;
}

[[nodiscard]] std::filesystem::path absolute_command_path(
    std::string_view directory,
    const std::filesystem::path& path) {
    if (path.is_absolute()) {
        return path.lexically_normal();
    }
    return (std::filesystem::path{directory} / path)
        .lexically_normal();
}

[[nodiscard]] std::optional<std::string_view>
profile_path_component(const std::filesystem::path& source) {
    std::optional<std::string_view> result;
    for (const std::filesystem::path& component : source) {
        std::string_view profile;
        if (component == "production") {
            profile = "production";
        } else if (component == "test_indexes") {
            profile = "test_indexes";
        } else if (component == "test_support") {
            profile = "test_support";
        } else {
            continue;
        }
        if (result && *result != profile) {
            throw std::runtime_error(
                "generated SQL module path contains conflicting profile "
                "components: " + source.string());
        }
        result = profile;
    }
    return result;
}

[[nodiscard]] std::filesystem::path path_for_profile(
    const std::filesystem::path& source,
    std::string_view compiler_profile) {
    std::filesystem::path result;
    for (const std::filesystem::path& component : source) {
        if (component == "production"
            || component == "test_indexes"
            || component == "test_support") {
            result /= compiler_profile;
        } else {
            result /= component;
        }
    }
    return result.lexically_normal();
}

[[nodiscard]] std::vector<std::filesystem::path>
profile_alternate_paths(const std::filesystem::path& source) {
    const std::optional<std::string_view> source_profile =
        profile_path_component(source);
    if (!source_profile) return {};
    std::vector<std::filesystem::path> result;
    static constexpr std::array profiles{
        std::string_view{"production"},
        std::string_view{"test_indexes"},
        std::string_view{"test_support"},
    };
    for (const std::string_view profile :
         profiles) {
        if (profile != *source_profile) {
            result.push_back(path_for_profile(source, profile));
        }
    }
    return result;
}

[[nodiscard]] bool known_generated_sql_profile_path(
    const std::filesystem::path& source) {
    const std::string spelling = source.generic_string();
    return spelling.contains("/generated/")
        && spelling.contains("/sql/");
}

[[nodiscard]] clang::tooling::CompileCommand
retarget_compile_command(
    const clang::tooling::CompileCommand& command,
    const std::filesystem::path& old_source,
    const std::filesystem::path& new_source) {
    clang::tooling::CompileCommand result = command;
    result.Filename = new_source.string();
    for (std::string& argument : result.CommandLine) {
        if (argument.empty() || argument.front() == '-') continue;
        if (absolute_command_path(
                result.Directory,
                std::filesystem::path{argument})
            == old_source) {
            argument = new_source.string();
        }
    }
    return result;
}

[[nodiscard]] bool test_indexes_definition(
    const std::vector<std::string>& arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "-DBIOSEQ_TEST_INDEXES"
            || argument == "-DBIOSEQ_TEST_INDEXES=1") {
            return true;
        }
        if (argument == "-D" && index + 1 < arguments.size()
            && (arguments[index + 1] == "BIOSEQ_TEST_INDEXES"
                || arguments[index + 1]
                    == "BIOSEQ_TEST_INDEXES=1")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool cache_test_support_definition(
    const std::vector<std::string>& arguments) {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "-DBIOSEQ_CACHE_TESTING"
            || argument == "-DBIOSEQ_CACHE_TESTING=1") {
            return true;
        }
        if (argument == "-D" && index + 1 < arguments.size()
            && (arguments[index + 1] == "BIOSEQ_CACHE_TESTING"
                || arguments[index + 1]
                    == "BIOSEQ_CACHE_TESTING=1")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool command_matches_profile(
    const std::vector<std::string>& arguments,
    std::string_view compiler_profile) {
    const bool test_indexes =
        test_indexes_definition(arguments);
    const bool test_support =
        cache_test_support_definition(arguments);
    if (compiler_profile == "production") {
        return !test_indexes && !test_support;
    }
    if (compiler_profile == "test_indexes") {
        return test_indexes && !test_support;
    }
    if (compiler_profile == "test_support") {
        return !test_indexes && test_support;
    }
    return false;
}

[[nodiscard]] bool path_within_root(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
    std::error_code candidate_error;
    std::error_code root_error;
    const std::filesystem::path normalized_candidate =
        std::filesystem::weakly_canonical(
            std::filesystem::absolute(candidate),
            candidate_error);
    const std::filesystem::path normalized_root =
        std::filesystem::weakly_canonical(
            std::filesystem::absolute(root),
            root_error);
    if (candidate_error || root_error) return false;
    const std::filesystem::path relative =
        normalized_candidate.lexically_relative(normalized_root);
    if (relative.empty() || relative.is_absolute()) return false;
    return *relative.begin() != "..";
}

void validate_owned_compile_database(
    const clang::tooling::CompilationDatabase& database,
    const std::vector<std::filesystem::path>& source_paths,
    std::string_view compiler_profile,
    const std::filesystem::path& pcm_root) {
    if (compiler_profile != "production"
        && compiler_profile != "test_indexes"
        && compiler_profile != "test_support") {
        throw std::runtime_error(
            "owned Clang compile database received an unknown profile");
    }
    for (const std::filesystem::path& source : source_paths) {
        const std::vector<clang::tooling::CompileCommand> commands =
            database.getCompileCommands(source.string());
        const auto selected = std::ranges::find_if(
            commands, command_is_satisfiable);
        if (selected == commands.end()) {
            throw std::runtime_error(
                "owned Clang compile database has no satisfiable command "
                "for " + source.string());
        }
        if (!command_matches_profile(
                selected->CommandLine, compiler_profile)) {
            throw std::runtime_error(
                "owned Clang compile database profile mismatch for "
                + source.string());
        }
        bool has_owned_pcm_output = false;
        for (const std::string& argument :
             selected->CommandLine) {
            std::string_view pcm_path;
            if (argument.starts_with("-fmodule-output=")) {
                pcm_path = std::string_view{argument}.substr(
                    std::string_view{"-fmodule-output="}.size());
                has_owned_pcm_output = true;
            } else if (argument.starts_with("-fmodule-file=")) {
                const std::string_view mapping =
                    std::string_view{argument}.substr(
                        std::string_view{"-fmodule-file="}.size());
                const std::size_t separator = mapping.find('=');
                if (separator == std::string_view::npos
                    || separator + 1 == mapping.size()) {
                    throw std::runtime_error(
                        "malformed owned Clang module mapping for "
                        + source.string());
                }
                pcm_path = mapping.substr(separator + 1);
            }
            if (!pcm_path.empty()
                && !path_within_root(
                    std::filesystem::path{
                        std::string{pcm_path}},
                    pcm_root)) {
                throw std::runtime_error(
                    "owned Clang compile command references a PCM "
                    "outside its authority root for "
                    + source.string());
            }
        }
        if (!has_owned_pcm_output) {
            throw std::runtime_error(
                "owned Clang compile command has no content-keyed PCM "
                "output for " + source.string());
        }
    }
}

[[nodiscard]] int command_preference(
    const clang::tooling::CompileCommand& command,
    std::string_view compiler_profile) {
    int score = 0;
    for (const std::string& argument : command.CommandLine) {
        if (argument.contains("postgresql_bioseq.dir")) {
            score += 20;
        }
        if (argument.contains("bioseq_codegen.dir")) {
            score += 10;
        }
    }
    if (command_matches_profile(
            command.CommandLine, compiler_profile)) {
        score += 5;
    }
    for (const std::string& argument : command.CommandLine) {
        if (argument.contains("__cmake_cxx26")) {
            score += 50;
        } else if (argument.contains("__cmake_cxx23")) {
            score += 25;
        }
    }
    return score;
}

[[nodiscard]] std::vector<std::string> expanded_command_line(
    const clang::tooling::CompileCommand& command,
    std::vector<std::filesystem::path>& dependencies) {
    std::vector<std::string> result;
    std::set<std::filesystem::path> active_responses;
    std::function<void(const std::string&)> append_argument =
        [&](const std::string& argument) {
        if (argument.empty() || argument.front() != '@') {
            result.push_back(argument);
            return;
        }
        const std::filesystem::path response_path =
            absolute_command_path(
                command.Directory,
                std::filesystem::path{
                    std::string{
                        std::string_view{argument}.substr(1)}});
        std::error_code canonical_error;
        const std::filesystem::path canonical_response =
            std::filesystem::weakly_canonical(
                response_path, canonical_error);
        const std::filesystem::path response =
            canonical_error
            ? response_path.lexically_normal()
            : canonical_response;
        const std::string response_name =
            response.filename().string();
        const bool generated_cmake_modmap =
            response_name.ends_with(".o.modmap")
            && response.generic_string().contains(
                "/CMakeFiles/");
        if (generated_cmake_modmap) {
            // CMake's dyndep mapping is neither read nor hashed: the
            // exact P1689 provider/import graph below is the sole PCM
            // authority.
            return;
        }
        if (!std::filesystem::is_regular_file(response)) {
            throw std::runtime_error(
                "compile command response file does not exist: "
                + response.string());
        }
        if (!active_responses.insert(response).second) {
            throw std::runtime_error(
                "compile command response-file cycle at "
                + response.string());
        }
        dependencies.push_back(response);
        const std::string content = read_artifact_bytes(response);
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver{allocator};
        llvm::SmallVector<const char*, 32> tokens;
        llvm::cl::TokenizeGNUCommandLine(
            content, saver, tokens);
        for (const char* token : tokens) {
            append_argument(token);
        }
        active_responses.erase(response);
    };
    for (const std::string& argument : command.CommandLine) {
        append_argument(argument);
    }
    return result;
}

[[nodiscard]] std::size_t compiler_argument_index(
    const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        throw std::runtime_error(
            "compile command has an empty argument vector");
    }
    std::size_t index = 0;
    while (index < arguments.size()) {
        const std::string launcher =
            std::filesystem::path{arguments[index]}
                .filename().string();
        if (launcher == "ccache" || launcher == "sccache"
            || launcher
                == "gcc_hwy_header_unit_launcher.py") {
            ++index;
            continue;
        }
        if ((launcher == "python"
             || launcher.starts_with("python3"))
            && index + 1 < arguments.size()
            && std::filesystem::path{arguments[index + 1]}
                   .filename()
                == "gcc_hwy_header_unit_launcher.py") {
            index += 2;
            continue;
        }
        break;
    }
    if (index == arguments.size()
        || arguments[index].empty()
        || arguments[index].front() == '-') {
        throw std::runtime_error(
            "compile command launcher chain has no unambiguous compiler");
    }
    const std::string compiler =
        std::filesystem::path{arguments[index]}
            .filename().string();
    const bool recognized_compiler =
        compiler == "c++" || compiler == "g++"
        || compiler == "clang" || compiler == "clang++"
        || compiler.starts_with("g++-")
        || compiler.starts_with("clang-")
        || compiler.starts_with("clang++-");
    if (!recognized_compiler) {
        throw std::runtime_error(
            "compile command launcher chain does not resolve to a "
            "recognized C++ compiler: " + arguments[index]);
    }
    return index;
}

[[nodiscard]] bool drop_normalized_argument(
    std::string_view argument) {
    return argument == "-c" || argument == "--precompile"
        || argument == "-MD"
        || argument == "-MMD" || argument == "-MP"
        || argument == "-M" || argument == "-MM"
        || argument == "-fsyntax-only"
        || argument == "-save-temps"
        || argument == "--save-temps"
        || argument.starts_with("-save-temps=")
        || argument.starts_with("--save-temps=")
        || argument == "-fmodules-ts"
        || argument == "-fmodules-reduced-bmi"
        || argument.starts_with("-fmodule-output")
        || argument.starts_with("-fmodule-file")
        || argument.starts_with("-fprebuilt-module-path")
        || argument.starts_with("-fmodule-mapper")
        || argument.starts_with("-fdeps-")
        || argument.starts_with("-MF")
        || argument.starts_with("-MT")
        || argument.starts_with("-MQ")
        || argument.starts_with("-MJ")
        || argument.starts_with("-resource-dir=")
        || argument.starts_with("-ffile-prefix-map=")
        || argument.starts_with("-fmacro-prefix-map=")
        || argument.starts_with("-fdebug-prefix-map=")
        || argument.starts_with("-fdebug-compilation-dir=")
        || argument.starts_with("-x")
        || argument.starts_with("-DBIOSEQ_TEST_INDEXES")
        || argument.starts_with("-UBIOSEQ_TEST_INDEXES")
        || argument.starts_with("-DBIOSEQ_CACHE_TESTING")
        || argument.starts_with("-UBIOSEQ_CACHE_TESTING");
}

[[nodiscard]] std::vector<std::string> normalized_base_arguments(
    const clang::tooling::CompileCommand& command,
    const std::filesystem::path& source_path,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& resource_dir,
    const std::filesystem::path& relocation_root,
    std::string_view compiler_profile,
    std::vector<std::filesystem::path>& dependencies) {
    const std::vector<std::string> expanded =
        expanded_command_line(command, dependencies);
    std::vector<std::string> result{
        clang_binary.string(),
        "-resource-dir",
        resource_dir.string(),
    };
    static const std::set<std::string, std::less<>> drop_with_value{
        "-o", "-MF", "-MT", "-MQ", "-MJ", "-resource-dir", "-x",
        "--serialize-diagnostics",
    };
    static constexpr std::string_view builtin_headers_in_system_modules =
        "-fbuiltin-headers-in-system-modules";
    static constexpr std::string_view no_builtin_headers_in_system_modules =
        "-fno-builtin-headers-in-system-modules";
    for (std::size_t index =
             compiler_argument_index(expanded) + 1;
         index < expanded.size();
         ++index) {
        const std::string& argument = expanded[index];
        if (argument == "-Xclang"
            && index + 1 < expanded.size()
            && (expanded[index + 1]
                    == builtin_headers_in_system_modules
                || expanded[index + 1]
                    == no_builtin_headers_in_system_modules)) {
            ++index;
            continue;
        }
        if (argument == builtin_headers_in_system_modules
            || argument == no_builtin_headers_in_system_modules) {
            continue;
        }
        if (drop_with_value.contains(argument)) {
            ++index;
            continue;
        }
        if (argument == "-D" || argument == "-U") {
            if (index + 1 < expanded.size()
                && (expanded[index + 1]
                        .starts_with("BIOSEQ_TEST_INDEXES")
                    || expanded[index + 1]
                        .starts_with("BIOSEQ_CACHE_TESTING"))) {
                ++index;
                continue;
            }
        }
        const std::filesystem::path argument_path{argument};
        if ((!argument.empty() && argument.front() != '-')
            && absolute_command_path(
                   command.Directory, argument_path)
                == source_path) {
            continue;
        }
        if (drop_normalized_argument(argument)) {
            continue;
        }
        result.push_back(argument);
    }
    if (compiler_profile == "test_indexes") {
        result.emplace_back("-DBIOSEQ_TEST_INDEXES=1");
    } else if (compiler_profile == "test_support") {
        result.emplace_back("-DBIOSEQ_CACHE_TESTING=1");
    }
    const std::string relocation_root_spelling =
        relocation_root.generic_string();
    result.emplace_back(
        "-ffile-prefix-map=" + relocation_root_spelling
        + "=/__bioseq_relocatable_root__");
    result.emplace_back(
        "-fmacro-prefix-map=" + relocation_root_spelling
        + "=/__bioseq_relocatable_root__");
    result.emplace_back(
        "-fdebug-prefix-map=" + relocation_root_spelling
        + "=/__bioseq_relocatable_root__");
    result.emplace_back(
        "-fdebug-compilation-dir=/__bioseq_relocatable_root__");
    result.emplace_back("-x");
    result.emplace_back("c++-module");
    // Linked-Clang structural actions enable builtin headers in system
    // modules. Prepare every owned PCM under the same cc1 language mode so
    // declaration probes can consume it without a module-configuration split.
    result.emplace_back("-Xclang");
    result.emplace_back(builtin_headers_in_system_modules);
    result.emplace_back("-Wno-unknown-attributes");
    result.emplace_back("-Wno-ignored-attributes");
    return result;
}

struct relocation_context {
    std::filesystem::path source_root;
    std::filesystem::path build_root;
    std::filesystem::path build_root_spelling;
};

[[nodiscard]] std::string relocatable_path_spelling(
    const std::filesystem::path& path,
    const relocation_context& relocation);

[[nodiscard]] std::string relocatable_text(
    std::string_view text,
    const relocation_context& relocation);

[[nodiscard]] std::string relocatable_document_hash(
    std::string_view document,
    const relocation_context& relocation);

[[nodiscard]] std::string command_input_hash(
    std::vector<std::filesystem::path> inputs) {
    std::ranges::sort(inputs);
    inputs.erase(
        std::ranges::unique(inputs).begin(), inputs.end());
    std::string material;
    for (const std::filesystem::path& input : inputs) {
        material += input.generic_string() + "\t"
            + bioseq::common::sha256_hex(read_artifact_bytes(input)) + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

[[nodiscard]] std::string relocatable_command_input_hash(
    std::vector<std::filesystem::path> inputs,
    const relocation_context& relocation) {
    std::ranges::sort(inputs);
    inputs.erase(
        std::ranges::unique(inputs).begin(), inputs.end());
    std::vector<std::string> rows;
    rows.reserve(inputs.size());
    for (const std::filesystem::path& input : inputs) {
        rows.push_back(
            relocatable_path_spelling(input, relocation)
            + "\t"
            + relocatable_document_hash(
                read_artifact_bytes(input), relocation));
    }
    std::ranges::sort(rows);
    if (std::ranges::adjacent_find(rows) != rows.end()) {
        throw std::runtime_error(
            "PREPARE command inputs collapse to a duplicate "
            "relocatable identity");
    }
    std::string material;
    for (const std::string& row : rows) {
        material += row + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(hex[character >> 4]);
                    result.push_back(hex[character & 0xf]);
                } else {
                    result.push_back(static_cast<char>(character));
                }
        }
    }
    return result;
}

[[nodiscard]] std::string safe_module_filename(
    std::string_view module_name) {
    std::string result;
    result.reserve(module_name.size());
    for (const unsigned char value : module_name) {
        if ((value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9')
            || value == '_') {
            result.push_back(static_cast<char>(value));
        } else {
            result.push_back('-');
        }
    }
    return result;
}

inline constexpr std::string_view k_module_preparation_policy_domain{
    "bioseq_clang_module_preparation\nrevision=5"};

struct module_build_record {
    std::string module_name;
    std::filesystem::path source_path;
    lexical_module_description description;
    clang::tooling::CompileCommand command;
    std::vector<std::string> base_arguments;
    std::vector<std::string> direct_arguments;
    std::vector<std::string> owned_arguments;
    std::vector<std::filesystem::path> command_inputs;
    std::filesystem::path dependency_scan_database_path;
    std::filesystem::path dependency_scan_result_path;
    std::filesystem::path dependency_scan_diagnostics_path;
    std::filesystem::path pcm_path;
    std::filesystem::path pcm_provenance_path;
    std::filesystem::path depfile_path;
    std::filesystem::path pcm_depfile_path;
    std::string source_sha256;
    std::string relocatable_source_sha256;
    std::string argument_sha256;
    std::string relocatable_argument_sha256;
    std::string command_input_sha256;
    std::string relocatable_command_input_sha256;
    std::string dependency_sha256;
    std::string relocatable_dependency_sha256;
    std::string depfile_sha256;
    std::string relocatable_depfile_sha256;
    std::string dependency_scan_database_sha256;
    std::string relocatable_dependency_scan_database_sha256;
    std::string dependency_scan_result_sha256;
    std::string relocatable_dependency_scan_result_sha256;
    std::string dependency_scan_diagnostics_sha256;
    std::string relocatable_dependency_scan_diagnostics_sha256;
    std::string pcm_sha256;
    std::string pcm_provenance_sha256;
    std::string content_key;
    std::string relocatable_content_key;
    std::vector<std::filesystem::path> discovered_dependencies;
};

struct active_import_artifact_paths {
    std::filesystem::path database;
    std::filesystem::path result;
    std::filesystem::path object;
    std::filesystem::path diagnostics;
};

struct pcm_artifact_paths {
    std::filesystem::path pcm;
    std::filesystem::path object;
    std::filesystem::path depfile;
    std::filesystem::path provenance;
};

[[nodiscard]] std::string render_pcm_provenance(
    const module_build_record& record,
    std::string_view clang_binary_sha256,
    std::string_view clang_scan_deps_sha256,
    std::string_view pcm_sha256) {
    return
        "bioseq_clang_pcm_provenance\n"
        "revision=1\n"
        "module=" + record.module_name + "\n"
        "content_key=" + record.content_key + "\n"
        "source_sha256=" + record.source_sha256 + "\n"
        "arguments_sha256=" + record.argument_sha256 + "\n"
        "command_input_sha256="
        + record.command_input_sha256 + "\n"
        "dependency_content_sha256=" + record.dependency_sha256 + "\n"
        "clang_binary_sha256=" + std::string{clang_binary_sha256} + "\n"
        "clang_scan_deps_sha256="
        + std::string{clang_scan_deps_sha256} + "\n"
        "pcm_sha256=" + std::string{pcm_sha256} + "\n";
}

class scoped_process_cwd {
  public:
    explicit scoped_process_cwd(
        const std::filesystem::path& directory)
        : previous_(std::filesystem::current_path()) {
        std::filesystem::current_path(directory);
    }

    scoped_process_cwd(const scoped_process_cwd&) = delete;
    scoped_process_cwd& operator=(const scoped_process_cwd&) = delete;

    ~scoped_process_cwd() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
    }

  private:
    std::filesystem::path previous_;
};

void execute_clang(
    const std::filesystem::path& clang_binary,
    const std::vector<std::string>& arguments,
    const std::filesystem::path& directory,
    std::string_view module_name,
    std::string_view operation,
    const std::filesystem::path& diagnostics_path = {}) {
    std::vector<llvm::StringRef> refs;
    refs.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        refs.emplace_back(argument);
    }
    std::string error;
    bool execution_failed = false;
    const scoped_process_cwd cwd{directory};
    std::array<std::optional<llvm::StringRef>, 3> redirects{
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    std::string diagnostics_storage;
    if (!diagnostics_path.empty()) {
        std::filesystem::create_directories(
            diagnostics_path.parent_path());
        diagnostics_storage = diagnostics_path.string();
        redirects[2] = llvm::StringRef{diagnostics_storage};
    }
    const int status = llvm::sys::ExecuteAndWait(
        clang_binary.string(),
        refs,
        std::nullopt,
        diagnostics_path.empty()
            ? llvm::ArrayRef<
                  std::optional<llvm::StringRef>>{}
            : llvm::ArrayRef<
                  std::optional<llvm::StringRef>>{redirects},
        0,
        0,
        &error,
        &execution_failed);
    if (execution_failed || status != 0) {
        std::string diagnostics;
        if (!diagnostics_path.empty()
            && std::filesystem::is_regular_file(
                diagnostics_path)) {
            diagnostics = read_artifact_bytes(diagnostics_path);
        }
        std::string invocation;
        for (const std::string& argument : arguments) {
            if (!invocation.empty()) invocation.push_back(' ');
            invocation += argument;
        }
        throw std::runtime_error(
            "Clang " + std::string{operation}
            + " failed for module '"
            + std::string{module_name} + "'"
            + (error.empty() ? std::string{}
                             : ": " + error)
            + "\nWorking directory: " + directory.string()
            + "\nInvocation: " + invocation
            + (diagnostics.empty() ? std::string{}
                                   : "\n" + diagnostics));
    }
}

[[nodiscard]] std::filesystem::path weakly_canonical_path(
    const std::filesystem::path& path);

[[nodiscard]] std::vector<std::filesystem::path>
parse_make_depfile(
    const std::filesystem::path& depfile,
    const std::filesystem::path& command_directory) {
    std::string content = read_artifact_bytes(depfile);
    for (std::size_t position = 0;
         (position = content.find("\\\n", position))
            != std::string::npos;) {
        content.replace(position, 2, " ");
    }
    std::size_t separator = std::string::npos;
    bool escaped = false;
    for (std::size_t index = 0; index < content.size(); ++index) {
        if (escaped) {
            escaped = false;
        } else if (content[index] == '\\') {
            escaped = true;
        } else if (content[index] == ':') {
            separator = index;
            break;
        }
    }
    if (separator == std::string::npos) {
        throw std::runtime_error(
            "malformed Clang depfile " + depfile.string());
    }
    std::vector<std::filesystem::path> result;
    std::string token;
    escaped = false;
    const auto flush = [&] {
        if (token.empty()) return;
        std::filesystem::path dependency{token};
        if (dependency.is_relative()) {
            dependency = command_directory / dependency;
        }
        dependency = weakly_canonical_path(
            dependency.lexically_normal());
        if (!std::filesystem::is_regular_file(dependency)) {
            throw std::runtime_error(
                "Clang depfile dependency is not a regular file: "
                + dependency.string());
        }
        result.push_back(std::move(dependency));
        token.clear();
    };
    for (std::size_t index = separator + 1;
         index < content.size();
         ++index) {
        const char character = content[index];
        if (escaped) {
            token.push_back(character);
            escaped = false;
        } else if (character == '\\') {
            escaped = true;
        } else if (detail::ascii_space(character)) {
            flush();
        } else {
            token.push_back(character);
        }
    }
    if (escaped) token.push_back('\\');
    flush();
    std::ranges::sort(result);
    result.erase(
        std::ranges::unique(result).begin(), result.end());
    return result;
}

[[nodiscard]] bool depfile_is_current(
    const std::filesystem::path& depfile,
    const std::filesystem::path& command_directory) {
    std::error_code error;
    const std::filesystem::file_time_type depfile_time =
        std::filesystem::last_write_time(depfile, error);
    if (error) return false;
    std::vector<std::filesystem::path> dependencies;
    try {
        dependencies =
            parse_make_depfile(depfile, command_directory);
    } catch (...) {
        return false;
    }
    if (dependencies.empty()) return false;
    for (const std::filesystem::path& dependency : dependencies) {
        error.clear();
        const std::filesystem::file_time_type dependency_time =
            std::filesystem::last_write_time(dependency, error);
        if (error || dependency_time > depfile_time) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool generated_pcm_dependency(
    const std::filesystem::path& dependency,
    const std::filesystem::path& pcm_root) {
    if (!path_within_root(dependency, pcm_root)) return false;
    return dependency.extension() == ".pcm"
        || dependency.extension() == ".o"
        || dependency.extension() == ".d";
}

[[nodiscard]] std::string dependency_content_hash(
    const std::vector<std::filesystem::path>& dependencies,
    const std::filesystem::path& pcm_root,
    std::unordered_map<std::string, std::string>& hash_cache) {
    std::string material;
    for (const std::filesystem::path& dependency : dependencies) {
        if (generated_pcm_dependency(dependency, pcm_root)) {
            continue;
        }
        const std::string key = dependency.generic_string();
        auto [position, inserted] =
            hash_cache.try_emplace(key);
        if (inserted) {
            if (!std::filesystem::is_regular_file(dependency)) {
                throw std::runtime_error(
                    "Clang dependency is not a regular file: "
                    + dependency.string());
            }
            position->second =
                bioseq::common::sha256_hex(read_artifact_bytes(dependency));
        }
        material += key + "\t" + position->second + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

[[nodiscard]] std::string relocatable_dependency_content_hash(
    const std::vector<std::filesystem::path>& dependencies,
    const std::filesystem::path& pcm_root,
    const relocation_context& relocation,
    std::unordered_map<std::string, std::string>& hash_cache) {
    std::vector<std::string> rows;
    rows.reserve(dependencies.size());
    for (const std::filesystem::path& dependency : dependencies) {
        if (generated_pcm_dependency(dependency, pcm_root)) {
            continue;
        }
        const std::string key = dependency.generic_string();
        auto [position, inserted] =
            hash_cache.try_emplace(key);
        if (inserted) {
            if (!std::filesystem::is_regular_file(dependency)) {
                throw std::runtime_error(
                    "Clang dependency is not a regular file: "
                    + dependency.string());
            }
            position->second =
                relocatable_document_hash(
                    read_artifact_bytes(dependency), relocation);
        }
        rows.push_back(
            relocatable_path_spelling(
                dependency, relocation)
            + "\t" + position->second);
    }
    std::ranges::sort(rows);
    rows.erase(
        std::ranges::unique(rows).begin(), rows.end());
    std::string material;
    for (const std::string& row : rows) {
        material += row + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

[[nodiscard]] std::string directory_content_hash(
    const std::filesystem::path& root,
    std::vector<std::filesystem::path>& dependencies) {
    if (!std::filesystem::is_directory(root)) {
        throw std::runtime_error(
            "toolchain resource directory does not exist: "
            + root.string());
    }
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator{
             root,
             std::filesystem::directory_options::
                 skip_permission_denied,
             error},
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error) {
            files.push_back(
                weakly_canonical_path(iterator->path()));
        }
        error.clear();
    }
    if (error || files.empty()) {
        throw std::runtime_error(
            "cannot inventory toolchain resource directory: "
            + root.string());
    }
    std::ranges::sort(files);
    std::string material;
    for (const std::filesystem::path& file : files) {
        dependencies.push_back(file);
        material += file.lexically_relative(root).generic_string()
            + "\t" + bioseq::common::sha256_hex(read_artifact_bytes(file)) + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

void validate_clang_binary_version(
    const std::filesystem::path& clang_binary) {
    llvm::SmallString<256> output_path;
    if (const std::error_code error =
            llvm::sys::fs::createTemporaryFile(
                "bioseq-clang-version", "txt", output_path)) {
        throw std::runtime_error(
            "cannot create Clang version probe output: "
            + error.message());
    }
    const std::vector<std::string> arguments{
        clang_binary.string(),
        "--version",
    };
    std::vector<llvm::StringRef> refs;
    refs.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        refs.emplace_back(argument);
    }
    const std::array<std::optional<llvm::StringRef>, 3> redirects{
        std::nullopt,
        llvm::StringRef{output_path},
        llvm::StringRef{},
    };
    std::string error;
    bool execution_failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        clang_binary.string(),
        refs,
        std::nullopt,
        redirects,
        15,
        0,
        &error,
        &execution_failed);
    std::string output;
    try {
        output = read_artifact_bytes(std::filesystem::path{
            output_path.str().str()});
    } catch (...) {
        static_cast<void>(llvm::sys::fs::remove(output_path));
        throw;
    }
    static_cast<void>(llvm::sys::fs::remove(output_path));
    if (execution_failed || status != 0) {
        throw std::runtime_error(
            "cannot execute configured Clang binary"
            + (error.empty() ? std::string{}
                             : ": " + error));
    }
    if (!output.contains(CLANG_VERSION_STRING)) {
        throw std::runtime_error(
            "configured Clang binary version does not match linked "
            "LibTooling " + std::string{CLANG_VERSION_STRING}
            + ": " + output);
    }
}

[[nodiscard]] std::string probe_tool_version(
    const std::filesystem::path& tool,
    std::string_view tool_name) {
    if (!std::filesystem::is_regular_file(tool)) {
        throw std::runtime_error(
            std::string{tool_name} + " binary does not exist: "
            + tool.string());
    }
    llvm::SmallString<256> output_path;
    if (const std::error_code error =
            llvm::sys::fs::createTemporaryFile(
                "bioseq-tool-version", "txt", output_path)) {
        throw std::runtime_error(
            "cannot create tool version probe output: "
            + error.message());
    }
    const std::vector<std::string> arguments{
        tool.string(),
        "--version",
    };
    std::vector<llvm::StringRef> refs;
    refs.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        refs.emplace_back(argument);
    }
    const std::array<std::optional<llvm::StringRef>, 3> redirects{
        std::nullopt,
        llvm::StringRef{output_path},
        llvm::StringRef{output_path},
    };
    std::string error;
    bool execution_failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        tool.string(),
        refs,
        std::nullopt,
        redirects,
        15,
        0,
        &error,
        &execution_failed);
    std::string output;
    try {
        output = read_artifact_bytes(std::filesystem::path{
            output_path.str().str()});
    } catch (...) {
        static_cast<void>(llvm::sys::fs::remove(output_path));
        throw;
    }
    static_cast<void>(llvm::sys::fs::remove(output_path));
    if (execution_failed || status != 0) {
        throw std::runtime_error(
            "cannot execute " + std::string{tool_name}
            + (error.empty() ? std::string{}
                             : ": " + error));
    }
    if (!output.contains(CLANG_VERSION_STRING)) {
        throw std::runtime_error(
            std::string{tool_name}
            + " version does not match linked LibTooling "
            + std::string{CLANG_VERSION_STRING} + ": " + output);
    }
    return output;
}

[[nodiscard]] std::string probe_tool_argument(
    const std::filesystem::path& tool,
    std::string_view argument,
    std::string_view tool_name) {
    llvm::SmallString<256> output_path;
    if (const std::error_code error =
            llvm::sys::fs::createTemporaryFile(
                "bioseq-tool-probe", "txt", output_path)) {
        throw std::runtime_error(
            "cannot create tool probe output: "
            + error.message());
    }
    const std::vector<std::string> arguments{
        tool.string(),
        std::string{argument},
    };
    std::vector<llvm::StringRef> refs;
    for (const std::string& value : arguments) {
        refs.emplace_back(value);
    }
    const std::array<std::optional<llvm::StringRef>, 3> redirects{
        std::nullopt,
        llvm::StringRef{output_path},
        llvm::StringRef{output_path},
    };
    std::string error;
    bool execution_failed = false;
    const int status = llvm::sys::ExecuteAndWait(
        tool.string(),
        refs,
        std::nullopt,
        redirects,
        15,
        0,
        &error,
        &execution_failed);
    std::string output;
    try {
        output = read_artifact_bytes(std::filesystem::path{
            output_path.str().str()});
    } catch (...) {
        static_cast<void>(llvm::sys::fs::remove(output_path));
        throw;
    }
    static_cast<void>(llvm::sys::fs::remove(output_path));
    while (!output.empty()
           && detail::ascii_space(output.back())) {
        output.pop_back();
    }
    if (execution_failed || status != 0 || output.empty()) {
        throw std::runtime_error(
            "cannot query " + std::string{tool_name} + " "
            + std::string{argument}
            + (error.empty() ? std::string{}
                             : ": " + error));
    }
    return output;
}

struct selected_toolchain_identity {
    std::filesystem::path prefix;
    std::filesystem::path resource_dir;
    std::filesystem::path llvm_config;
    std::string llvm_config_sha256;
    std::string llvm_version;
    std::string clang_full_version;
    std::string clang_scan_deps_full_version;
    std::string linked_clang_full_version;
    std::filesystem::path linked_clang_cpp;
    std::string linked_clang_cpp_sha256;
    std::filesystem::path linked_llvm_library;
    std::string linked_llvm_library_sha256;
};

[[nodiscard]] selected_toolchain_identity
validate_selected_toolchain(
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory) {
    const std::filesystem::path configured_llvm_config =
        weakly_canonical_path(llvm_config);
    const std::filesystem::path linked_llvm_config =
        weakly_canonical_path(BIOSEQ_LINKED_LLVM_CONFIG);
    if (configured_llvm_config != linked_llvm_config
        || bioseq::common::sha256_hex(read_artifact_bytes(configured_llvm_config))
            != BIOSEQ_LINKED_LLVM_CONFIG_SHA256) {
        throw std::runtime_error(
            "selected llvm-config does not match the exact LibTooling "
            "build linked into bioseq_codegen");
    }
    const std::string llvm_version =
        probe_tool_argument(
            configured_llvm_config, "--version", "llvm-config");
    if (llvm_version != BIOSEQ_LINKED_LLVM_VERSION
        || llvm_version != CLANG_VERSION_STRING) {
        throw std::runtime_error(
            "selected llvm-config version differs from linked "
            "LibTooling/Clang: " + llvm_version);
    }
    const std::filesystem::path prefix =
        weakly_canonical_path(probe_tool_argument(
            configured_llvm_config, "--prefix", "llvm-config"));
    if (prefix
        != weakly_canonical_path(BIOSEQ_LINKED_LLVM_PREFIX)) {
        throw std::runtime_error(
            "selected LLVM prefix differs from linked LibTooling prefix");
    }
    const std::filesystem::path bindir =
        weakly_canonical_path(probe_tool_argument(
            configured_llvm_config, "--bindir", "llvm-config"));
    if (!path_within_root(
            weakly_canonical_path(clang_binary), bindir)
        || !path_within_root(
            weakly_canonical_path(clang_scan_deps), bindir)) {
        throw std::runtime_error(
            "Clang and clang-scan-deps must both come from the exact "
            "linked llvm-config bindir");
    }
    const std::string clang_full_version =
        probe_tool_version(clang_binary, "Clang");
    const std::string scanner_full_version =
        probe_tool_version(
            clang_scan_deps, "clang-scan-deps");
    const std::filesystem::path resource_dir =
        weakly_canonical_path(probe_tool_argument(
            clang_binary, "-print-resource-dir", "Clang"));
    const std::string linked_clang_full_version =
        clang::getClangFullVersion();
    if (!clang_full_version.contains(
            linked_clang_full_version)) {
        throw std::runtime_error(
            "selected Clang full build identity differs from linked "
            "LibTooling: " + clang_full_version);
    }
    if (!path_within_root(resource_dir, prefix)
        || !path_within_root(
            weakly_canonical_path(compiler_module_inventory),
            prefix)) {
        throw std::runtime_error(
            "Clang resource directory and libc++ module inventory must "
            "belong to the exact linked LLVM prefix");
    }
    const std::filesystem::path linked_clang_cpp =
        weakly_canonical_path(BIOSEQ_LINKED_CLANG_CPP);
    const std::filesystem::path linked_llvm =
        weakly_canonical_path(BIOSEQ_LINKED_LLVM_LIBRARY);
    if (!path_within_root(linked_clang_cpp, prefix)
        || !path_within_root(linked_llvm, prefix)
        || bioseq::common::sha256_hex(read_artifact_bytes(linked_clang_cpp))
            != BIOSEQ_LINKED_CLANG_CPP_SHA256
        || bioseq::common::sha256_hex(read_artifact_bytes(linked_llvm))
            != BIOSEQ_LINKED_LLVM_LIBRARY_SHA256) {
        throw std::runtime_error(
            "linked Clang/LLVM libraries no longer match their "
            "configure-time identities");
    }
    return {
        .prefix = prefix,
        .resource_dir = resource_dir,
        .llvm_config = configured_llvm_config,
        .llvm_config_sha256 =
            std::string{BIOSEQ_LINKED_LLVM_CONFIG_SHA256},
        .llvm_version = llvm_version,
        .clang_full_version = clang_full_version,
        .clang_scan_deps_full_version =
            scanner_full_version,
        .linked_clang_full_version =
            linked_clang_full_version,
        .linked_clang_cpp = linked_clang_cpp,
        .linked_clang_cpp_sha256 =
            std::string{BIOSEQ_LINKED_CLANG_CPP_SHA256},
        .linked_llvm_library = linked_llvm,
        .linked_llvm_library_sha256 =
            std::string{BIOSEQ_LINKED_LLVM_LIBRARY_SHA256},
    };
}

struct compiler_module_inventory {
    std::filesystem::path metadata_path;
    std::string metadata_sha256;
    std::map<
        std::string,
        std::filesystem::path,
        std::less<>>
        source_by_module;
};

[[nodiscard]] std::filesystem::path weakly_canonical_path(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : canonical;
}

[[nodiscard]] std::filesystem::path absolute_normalized_path(
    const std::filesystem::path& path) {
    return weakly_canonical_path(
        path.is_absolute() ? path : std::filesystem::absolute(path));
}

void accept_absent_owned_path(std::error_code& error) noexcept {
    if (error == std::errc::no_such_file_or_directory) {
        error.clear();
    }
}

[[nodiscard]] std::string relocatable_argument_material(
    const std::vector<std::string>& arguments,
    const relocation_context& relocation,
    std::string_view clang_binary_sha256);

[[nodiscard]] std::filesystem::path ensure_preparation_root(
    const std::filesystem::path& configured_root) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(configured_root, error);
    accept_absent_owned_path(error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect PREPARE PCM root: "
            + configured_root.string());
    }
    if (!std::filesystem::exists(status)) {
        std::filesystem::create_directories(configured_root, error);
        if (error) {
            throw std::runtime_error(
                "cannot create PREPARE PCM root: "
                + configured_root.string());
        }
    }
    error.clear();
    if (!std::filesystem::is_directory(configured_root, error)
        || error) {
        throw std::runtime_error(
            "PREPARE PCM root is not a directory: "
            + configured_root.string());
    }
    const std::filesystem::path root =
        weakly_canonical_path(configured_root);
    if (!root.is_absolute()
        || !std::filesystem::is_directory(root)) {
        throw std::runtime_error(
            "PREPARE PCM root has no canonical directory identity: "
            + configured_root.string());
    }
    return root;
}

[[nodiscard]] std::filesystem::path ensure_owned_directory(
    const std::filesystem::path& canonical_root,
    std::string_view component) {
    const std::filesystem::path relative{std::string{component}};
    if (component.empty() || relative.has_root_path()
        || relative.has_parent_path() || relative == "."
        || relative == "..") {
        throw std::runtime_error(
            "PREPARE owned directory must be one path component");
    }
    const std::filesystem::path directory =
        canonical_root / relative;
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(directory, error);
    accept_absent_owned_path(error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect PREPARE owned directory: "
            + directory.string());
    }
    if (std::filesystem::exists(status)) {
        if (std::filesystem::is_symlink(status)
            || !std::filesystem::is_directory(status)) {
            throw std::runtime_error(
                "PREPARE owned directory is a symlink or non-directory: "
                + directory.string());
        }
    } else {
        if (!std::filesystem::create_directory(directory, error)
            || error) {
            throw std::runtime_error(
                "cannot create PREPARE owned directory: "
                + directory.string());
        }
    }
    if (weakly_canonical_path(directory)
        != directory.lexically_normal()) {
        throw std::runtime_error(
            "PREPARE owned directory escaped its authority root: "
            + directory.string());
    }
    return directory;
}

[[nodiscard]] std::filesystem::path validate_existing_owned_directory(
    const std::filesystem::path& canonical_root,
    std::string_view component) {
    const std::filesystem::path relative{std::string{component}};
    if (component.empty() || relative.has_root_path()
        || relative.has_parent_path()) {
        throw std::runtime_error(
            "PREPARE owned directory must be one path component");
    }
    const std::filesystem::path directory =
        canonical_root / relative;
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(directory, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_directory(status)
        || weakly_canonical_path(directory)
            != directory.lexically_normal()) {
        throw std::runtime_error(
            "PREPARE owned directory is missing, linked, or foreign: "
            + directory.string());
    }
    return directory;
}

void validate_owned_leaf(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& path,
    std::string_view role) {
    const std::filesystem::path normalized =
        path.lexically_normal();
    const std::filesystem::path parent =
        normalized.parent_path();
    const std::filesystem::path canonical_parent =
        weakly_canonical_path(parent);
    std::error_code error;
    const std::filesystem::file_status parent_status =
        std::filesystem::symlink_status(parent, error);
    if (error || std::filesystem::is_symlink(parent_status)
        || !std::filesystem::is_directory(parent_status)
        || canonical_parent != parent
        || (parent != canonical_root
            && !path_within_root(parent, canonical_root))) {
        throw std::runtime_error(
            std::string{role}
            + " parent is outside the PREPARE authority root: "
            + path.string());
    }
    error.clear();
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(normalized, error);
    accept_absent_owned_path(error);
    if (error) {
        throw std::runtime_error(
            "cannot inspect " + std::string{role} + ": "
            + path.string());
    }
    if (std::filesystem::exists(status)
        && (std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status))) {
        throw std::runtime_error(
            std::string{role}
            + " is a symlink or non-regular file: "
            + path.string());
    }
}

[[nodiscard]] bool owned_regular_file_exists(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& path,
    std::string_view role) {
    validate_owned_leaf(canonical_root, path, role);
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    return !error && std::filesystem::is_regular_file(status);
}

void remove_owned_leaf_if_present(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& path,
    std::string_view role) {
    validate_owned_leaf(canonical_root, path, role);
    std::error_code error;
    if (!std::filesystem::remove(path, error) && error) {
        throw std::runtime_error(
            "cannot remove stale " + std::string{role} + ": "
            + path.string());
    }
}

class scoped_artifact_cleanup {
  public:
    explicit scoped_artifact_cleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    scoped_artifact_cleanup(const scoped_artifact_cleanup&) = delete;
    scoped_artifact_cleanup& operator=(
        const scoped_artifact_cleanup&) = delete;

    ~scoped_artifact_cleanup() {
        if (!active_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

[[nodiscard]] std::filesystem::path create_owned_staging_file(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& target,
    std::string_view role) {
    validate_owned_leaf(canonical_root, target, role);
    const std::string model =
        (target.parent_path()
         / (target.filename().string() + ".stage-%%%%%%"))
            .string();
    int descriptor = -1;
    llvm::SmallString<512> storage;
    if (const std::error_code error =
            llvm::sys::fs::createUniqueFile(
                model, descriptor, storage)) {
        throw std::runtime_error(
            "cannot create staged " + std::string{role} + ": "
            + error.message());
    }
    llvm::raw_fd_ostream close_descriptor{descriptor, true};
    close_descriptor.close();
    const std::filesystem::path staged{storage.str().str()};
    validate_owned_leaf(canonical_root, staged, role);
    return staged;
}

void publish_owned_staging_file(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& staged,
    const std::filesystem::path& target,
    std::string_view role,
    bool require_absent = false) {
    validate_owned_leaf(canonical_root, staged, role);
    validate_owned_leaf(canonical_root, target, role);
    if (!std::filesystem::is_regular_file(staged)) {
        throw std::runtime_error(
            "staged " + std::string{role}
            + " is not a regular file: " + staged.string());
    }
    if (require_absent
        && owned_regular_file_exists(
            canonical_root, target, role)) {
        throw std::runtime_error(
            std::string{role}
            + " commit marker already exists: " + target.string());
    }
    std::error_code error;
    std::filesystem::rename(staged, target, error);
    if (error) {
        throw std::runtime_error(
            "cannot atomically publish " + std::string{role}
            + ": " + error.message());
    }
    validate_owned_leaf(canonical_root, target, role);
    if (!std::filesystem::is_regular_file(target)) {
        throw std::runtime_error(
            "published " + std::string{role}
            + " is not a regular file: " + target.string());
    }
}

void atomic_write_owned_file(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& target,
    std::string_view document,
    std::string_view role,
    bool require_absent = false) {
    if (!require_absent
        && owned_regular_file_exists(
            canonical_root, target, role)
        && read_artifact_bytes(target) == document) {
        return;
    }
    const std::filesystem::path staged =
        create_owned_staging_file(canonical_root, target, role);
    scoped_artifact_cleanup cleanup{staged};
    std::error_code error;
    llvm::raw_fd_ostream output{
        staged.string(), error, llvm::sys::fs::OF_None};
    if (error) {
        throw std::runtime_error(
            "cannot open staged " + std::string{role}
            + ": " + error.message());
    }
    output << llvm::StringRef{document.data(), document.size()};
    output.close();
    if (output.has_error()) {
        throw std::runtime_error(
            "cannot write staged " + std::string{role});
    }
    publish_owned_staging_file(
        canonical_root,
        staged,
        target,
        role,
        require_absent);
    cleanup.release();
}

[[nodiscard]] active_import_artifact_paths
derive_active_import_artifact_paths(
    const std::filesystem::path& scan_root,
    std::string_view module_name,
    const std::filesystem::path& source_path,
    const std::vector<std::string>& base_arguments,
    const relocation_context& relocation,
    std::string_view clang_binary_sha256,
    std::string_view clang_scan_deps_sha256) {
    std::string key_material =
        std::string{module_name} + "\n"
        + relocatable_path_spelling(source_path, relocation)
        + "\n" + std::string{clang_scan_deps_sha256} + "\n";
    key_material += relocatable_argument_material(
        base_arguments, relocation, clang_binary_sha256);
    const std::string basename =
        safe_module_filename(module_name) + "-"
        + bioseq::common::sha256_hex(key_material).substr(0, 20);
    return {
        .database =
            scan_root / (basename + ".compile_commands.json"),
        .result = scan_root / (basename + ".p1689.json"),
        .object = scan_root / (basename + ".o"),
        .diagnostics =
            scan_root / (basename + ".diagnostics.txt"),
    };
}

[[nodiscard]] std::string predependency_material(
    std::string_view compiler_profile,
    std::string_view module_name,
    std::string_view source_sha256,
    std::string_view argument_sha256,
    std::string_view command_input_sha256,
    std::string_view scan_database_sha256,
    std::string_view scan_result_sha256,
    std::string_view scan_diagnostics_sha256,
    std::string_view clang_binary_sha256,
    std::string_view clang_scan_deps_sha256,
    const std::vector<std::pair<std::string, std::string>>& imports) {
    std::string material =
        std::string{k_module_preparation_policy_domain}
        + "\n" + std::string{CLANG_VERSION_STRING}
        + "\n" + std::string{clang_binary_sha256}
        + "\n" + std::string{clang_scan_deps_sha256}
        + "\n" + std::string{compiler_profile}
        + "\n" + std::string{module_name}
        + "\n" + std::string{source_sha256}
        + "\n" + std::string{argument_sha256}
        + "\n" + std::string{command_input_sha256}
        + "\n" + std::string{scan_database_sha256}
        + "\n" + std::string{scan_result_sha256}
        + "\n" + std::string{scan_diagnostics_sha256};
    for (const auto& [imported, content_key] : imports) {
        material += "\n" + imported + "=" + content_key;
    }
    return material;
}

[[nodiscard]] std::filesystem::path derive_make_depfile_path(
    const std::filesystem::path& dependency_root,
    std::string_view module_name,
    std::string_view relocatable_predependency_material) {
    return dependency_root
        / (safe_module_filename(module_name) + "-"
           + bioseq::common::sha256_hex(relocatable_predependency_material).substr(
               0, 20)
           + ".d");
}

[[nodiscard]] pcm_artifact_paths derive_pcm_artifact_paths(
    const std::filesystem::path& pcm_root,
    std::string_view module_name,
    std::string_view relocatable_content_key) {
    const std::filesystem::path pcm =
        pcm_root
        / (safe_module_filename(module_name) + "-"
           + std::string{relocatable_content_key.substr(0, 20)}
           + ".pcm");
    return {
        .pcm = pcm,
        .object = pcm.string() + ".o",
        .depfile = pcm.string() + ".d",
        .provenance = pcm.string() + ".provenance",
    };
}

[[nodiscard]] std::vector<std::string> derive_owned_arguments(
    std::vector<std::string> direct_arguments,
    const std::filesystem::path& source_spelling,
    const pcm_artifact_paths& artifacts) {
    direct_arguments.push_back(
        "-fmodule-output=" + artifacts.pcm.string());
    direct_arguments.emplace_back("-fmodules-reduced-bmi");
    // The driver writes the reduced BMI above and a transient full BMI to
    // `-o`; unlike `-c`, this performs no discarded object/backend action.
    direct_arguments.emplace_back("--precompile");
    direct_arguments.push_back(source_spelling.string());
    direct_arguments.emplace_back("-o");
    direct_arguments.push_back(artifacts.object.string());
    direct_arguments.emplace_back("-MD");
    direct_arguments.emplace_back("-MF");
    direct_arguments.push_back(artifacts.depfile.string());
    direct_arguments.emplace_back("-MT");
    direct_arguments.push_back(artifacts.depfile.string());
    return direct_arguments;
}

void validate_pcm_artifact_leaves(
    const std::filesystem::path& pcm_root,
    const pcm_artifact_paths& artifacts) {
    validate_owned_leaf(pcm_root, artifacts.pcm, "PCM output");
    validate_owned_leaf(pcm_root, artifacts.object, "PCM object output");
    validate_owned_leaf(
        pcm_root, artifacts.depfile, "PCM dependency output");
    validate_owned_leaf(
        pcm_root, artifacts.provenance, "PCM provenance output");
}

void remove_pcm_artifact_tuple(
    const std::filesystem::path& pcm_root,
    const pcm_artifact_paths& artifacts) {
    validate_pcm_artifact_leaves(pcm_root, artifacts);
    remove_owned_leaf_if_present(
        pcm_root, artifacts.provenance, "PCM provenance output");
    remove_owned_leaf_if_present(
        pcm_root, artifacts.depfile, "PCM dependency output");
    remove_owned_leaf_if_present(
        pcm_root, artifacts.object, "PCM object output");
    remove_owned_leaf_if_present(
        pcm_root, artifacts.pcm, "PCM output");
}

class scoped_pcm_publication {
  public:
    explicit scoped_pcm_publication(pcm_artifact_paths artifacts)
        : artifacts_(std::move(artifacts)) {}

    scoped_pcm_publication(const scoped_pcm_publication&) = delete;
    scoped_pcm_publication& operator=(
        const scoped_pcm_publication&) = delete;

    ~scoped_pcm_publication() {
        if (!active_) return;
        for (const std::filesystem::path& path : {
                 artifacts_.provenance,
                 artifacts_.depfile,
                 artifacts_.object,
                 artifacts_.pcm}) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }

    void commit() noexcept { active_ = false; }

  private:
    pcm_artifact_paths artifacts_;
    bool active_{true};
};

[[nodiscard]] std::filesystem::path derive_relocation_root(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    const std::filesystem::path& pcm_root) {
    std::filesystem::path common =
        absolute_normalized_path(pcm_root);
    const auto include =
        [&](const std::filesystem::path& candidate_path) {
            const std::filesystem::path candidate =
                absolute_normalized_path(candidate_path);
            while (
                common != common.root_path()
                && candidate != common
                && !path_within_root(candidate, common)
            ) {
                common = common.parent_path();
            }
        };
    include(compile_database);
    for (const std::filesystem::path& source : source_paths) {
        include(source);
    }
    if (common.empty() || common == common.root_path()) {
        throw std::runtime_error(
            "PREPARE inputs have no bounded common relocation root");
    }
    return common.lexically_normal();
}

[[nodiscard]] relocation_context make_relocation_context(
    const std::filesystem::path& source_root,
    const std::filesystem::path& compilation_root) {
    const std::filesystem::path normalized_source_root =
        absolute_normalized_path(source_root);
    const std::filesystem::path normalized_compilation_root =
        absolute_normalized_path(compilation_root);
    const std::filesystem::path compilation_root_spelling =
        (compilation_root.is_absolute()
             ? compilation_root
             : std::filesystem::absolute(compilation_root))
            .lexically_normal();
    if (!path_within_root(
            normalized_compilation_root,
            normalized_source_root)) {
        throw std::runtime_error(
            "PREPARE compilation root is outside the bounded "
            "relocation root");
    }
    return {
        .source_root = normalized_source_root,
        .build_root =
            normalized_compilation_root == normalized_source_root
            ? std::filesystem::path{}
            : normalized_compilation_root,
        .build_root_spelling =
            normalized_compilation_root == normalized_source_root
            ? std::filesystem::path{}
            : compilation_root_spelling,
    };
}

[[nodiscard]] std::string relocatable_path_spelling(
    const std::filesystem::path& path,
    const relocation_context& relocation) {
    const std::filesystem::path normalized =
        absolute_normalized_path(path);
    if (!relocation.build_root.empty()
        && normalized == relocation.build_root) {
        return "$BIOSEQ_BUILD_ROOT";
    }
    if (!relocation.build_root.empty()
        && path_within_root(
            normalized, relocation.build_root)) {
        return (
            std::filesystem::path{"$BIOSEQ_BUILD_ROOT"}
            / normalized.lexically_relative(
                relocation.build_root)
        ).generic_string();
    }
    if (normalized == relocation.source_root) {
        return "$BIOSEQ_RELOCATION_ROOT";
    }
    if (!path_within_root(
            normalized, relocation.source_root)) {
        return normalized.generic_string();
    }
    return (
        std::filesystem::path{"$BIOSEQ_RELOCATION_ROOT"}
        / normalized.lexically_relative(
            relocation.source_root)
    ).generic_string();
}

void replace_relocatable_root(
    std::string& result,
    const std::filesystem::path& root,
    std::string_view logical) {
    if (root.empty()) return;
    const std::string physical = root.generic_string();
    std::size_t position = 0;
    while ((position = result.find(physical, position))
           != std::string::npos) {
        const std::string_view prefix{
            result.data(), position};
        const bool joined_path_prefix =
            prefix.ends_with("-I")
            || prefix.ends_with("-isystem")
            || prefix.ends_with("-iquote")
            || prefix.ends_with("-idirafter")
            || prefix.ends_with("-include")
            || prefix.ends_with("-imacros")
            || prefix.ends_with("-isysroot");
        const bool left_boundary =
            position == 0
            || result[position - 1] == '"'
            || result[position - 1] == '\''
            || result[position - 1] == '='
            || result[position - 1] == ':'
            || result[position - 1] == '@'
            || result[position - 1] == ','
            || result[position - 1] == ';'
            || result[position - 1] == '('
            || result[position - 1] == '['
            || result[position - 1] == '{'
            || std::isspace(
                static_cast<unsigned char>(
                    result[position - 1]))
            || joined_path_prefix;
        const std::size_t after = position + physical.size();
        const bool right_boundary =
            after == result.size()
            || result[after] == '/'
            || result[after] == '\\'
            || result[after] == '"'
            || result[after] == '\''
            || result[after] == '='
            || result[after] == ':'
            || result[after] == ','
            || result[after] == ';'
            || result[after] == ')'
            || result[after] == ']'
            || result[after] == '}'
            || std::isspace(
                static_cast<unsigned char>(result[after]));
        if (!left_boundary || !right_boundary) {
            position = after;
            continue;
        }
        result.replace(position, physical.size(), logical);
        position += logical.size();
    }
}

[[nodiscard]] std::string relocatable_text(
    std::string_view text,
    const relocation_context& relocation) {
    std::string result{text};
    // The configured build directory is a strict child of the source
    // relocation root in ordinary CMake trees. Replace it first so preset
    // names and lane-local package roots remain physical provenance without
    // contaminating the canonical compilation signature.
    replace_relocatable_root(
        result,
        relocation.build_root_spelling,
        "$BIOSEQ_BUILD_ROOT");
    if (relocation.build_root
        != relocation.build_root_spelling) {
        replace_relocatable_root(
            result,
            relocation.build_root,
            "$BIOSEQ_BUILD_ROOT");
    }
    replace_relocatable_root(
        result,
        relocation.source_root,
        "$BIOSEQ_RELOCATION_ROOT");
    return result;
}

[[nodiscard]] std::string relocatable_document_hash(
    std::string_view document,
    const relocation_context& relocation) {
    return bioseq::common::sha256_hex(relocatable_text(document, relocation));
}

inline constexpr std::string_view
    relocatable_clang_driver_mode{"c++-module"};

[[nodiscard]] std::string relocatable_compiler_identity(
    std::string_view clang_binary_sha256) {
    if (clang_binary_sha256.size() != 64) {
        throw std::runtime_error(
            "relocatable Clang identity requires a SHA-256");
    }
    return
        "clang_binary_sha256=" + std::string{clang_binary_sha256}
        + "\nclang_driver_mode="
        + std::string{relocatable_clang_driver_mode} + "\n";
}

[[nodiscard]] std::string relocatable_argument_material(
    const std::vector<std::string>& arguments,
    const relocation_context& relocation,
    std::string_view clang_binary_sha256) {
    if (arguments.empty()) {
        throw std::runtime_error(
            "relocatable Clang argument projection is empty");
    }
    std::string material =
        "bioseq_clang_relocatable_arguments\n"
        "revision=3\n"
        + relocatable_compiler_identity(clang_binary_sha256);
    // argv[0] is an invocation spelling, not compiler identity. PREPARE
    // authenticates the selected executable bytes separately and fixes the
    // C++ module driver mode. Keep every semantic argument in original order.
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        material += relocatable_text(
            arguments[index], relocation);
        material.push_back('\0');
    }
    return material;
}

[[nodiscard]] std::string relocatable_argument_hash(
    const std::vector<std::string>& arguments,
    const relocation_context& relocation,
    std::string_view clang_binary_sha256) {
    return bioseq::common::sha256_hex(relocatable_argument_material(
        arguments, relocation, clang_binary_sha256));
}

[[nodiscard]] std::string
relocatable_compiler_document_hash(
    std::string_view document,
    const relocation_context& relocation,
    const std::filesystem::path& clang_binary,
    std::string_view clang_binary_sha256) {
    std::string projection =
        relocatable_text(document, relocation);
    const std::string compiler_spelling =
        relocatable_text(
            clang_binary.generic_string(), relocation);
    const std::string quoted_compiler =
        "\"" + json_escape(compiler_spelling) + "\"";
    const std::string quoted_identity =
        "\"$BIOSEQ_SELECTED_CLANG{"
        + std::string{clang_binary_sha256}
        + ";driver="
        + std::string{relocatable_clang_driver_mode}
        + "}\"";
    std::size_t replacement_count = 0;
    std::size_t position = 0;
    while ((position = projection.find(
                quoted_compiler, position))
           != std::string::npos) {
        projection.replace(
            position,
            quoted_compiler.size(),
            quoted_identity);
        position += quoted_identity.size();
        ++replacement_count;
    }
    if (replacement_count == 0) {
        throw std::runtime_error(
            "relocatable Clang metadata has no selected compiler "
            "invocation");
    }
    return bioseq::common::sha256_hex(
        "bioseq_clang_relocatable_compiler_document\n"
        "revision=3\n"
        + relocatable_compiler_identity(clang_binary_sha256)
        + projection);
}

[[nodiscard]] compiler_module_inventory
load_compiler_module_inventory(
    const std::filesystem::path& configured_metadata_path) {
    if (!std::filesystem::is_regular_file(
            configured_metadata_path)) {
        throw std::runtime_error(
            "selected Clang compiler-module inventory does not exist: "
            + configured_metadata_path.string());
    }
    const std::filesystem::path metadata_path =
        weakly_canonical_path(configured_metadata_path);
    const std::string document = read_artifact_bytes(metadata_path);
    llvm::Expected<llvm::json::Value> parsed =
        llvm::json::parse(document);
    if (!parsed) {
        throw std::runtime_error(
            "cannot parse selected Clang compiler-module inventory: "
            + llvm::toString(parsed.takeError()));
    }
    const llvm::json::Object* root = parsed->getAsObject();
    const llvm::json::Array* modules =
        root == nullptr ? nullptr : root->getArray("modules");
    if (root == nullptr || modules == nullptr) {
        throw std::runtime_error(
            "selected Clang compiler-module inventory has no modules "
            "array");
    }
    compiler_module_inventory result{
        .metadata_path = weakly_canonical_path(metadata_path),
        .metadata_sha256 = bioseq::common::sha256_hex(document),
    };
    for (const llvm::json::Value& entry_value : *modules) {
        const llvm::json::Object* entry =
            entry_value.getAsObject();
        if (entry == nullptr) continue;
        const std::optional<llvm::StringRef> logical_name =
            entry->getString("logical-name");
        const std::optional<llvm::StringRef> source_path =
            entry->getString("source-path");
        const std::optional<bool> is_standard =
            entry->getBoolean("is-std-library");
        if (!logical_name || !source_path || !is_standard
            || !*is_standard) {
            continue;
        }
        if (*logical_name != "std"
            && *logical_name != "std.compat") {
            continue;
        }
        std::filesystem::path source{source_path->str()};
        if (source.is_relative()) {
            source = result.metadata_path.parent_path() / source;
        }
        source = weakly_canonical_path(source);
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error(
                "selected Clang compiler-module source does not exist: "
                + source.string());
        }
        if (!result.source_by_module.emplace(
                logical_name->str(), source).second) {
            throw std::runtime_error(
                "selected Clang compiler-module inventory duplicates '"
                + logical_name->str() + "'");
        }
    }
    for (const std::string_view required :
         {"std", "std.compat"}) {
        if (!result.source_by_module.contains(required)) {
            throw std::runtime_error(
                "selected Clang compiler-module inventory does not pin '"
                + std::string{required} + "'");
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> parse_p1689_imports(
    const std::filesystem::path& result_path,
    std::string_view provided_module,
    const std::filesystem::path& source_path,
    const std::filesystem::path& command_directory) {
    const std::string document = read_artifact_bytes(result_path);
    llvm::Expected<llvm::json::Value> parsed =
        llvm::json::parse(document);
    if (!parsed) {
        throw std::runtime_error(
            "cannot parse clang-scan-deps P1689 output for module '"
            + std::string{provided_module} + "': "
            + llvm::toString(parsed.takeError()));
    }
    const llvm::json::Object* root = parsed->getAsObject();
    const llvm::json::Array* rules =
        root == nullptr ? nullptr : root->getArray("rules");
    if (root == nullptr || rules == nullptr) {
        throw std::runtime_error(
            "clang-scan-deps P1689 output has no rules array for module '"
            + std::string{provided_module} + "'");
    }
    const llvm::json::Object* selected_rule = nullptr;
    for (const llvm::json::Value& rule_value : *rules) {
        const llvm::json::Object* rule =
            rule_value.getAsObject();
        const llvm::json::Array* provides =
            rule == nullptr ? nullptr : rule->getArray("provides");
        if (provides == nullptr) continue;
        for (const llvm::json::Value& provide_value : *provides) {
            const llvm::json::Object* provide =
                provide_value.getAsObject();
            const std::optional<llvm::StringRef> logical_name =
                provide == nullptr
                ? std::nullopt
                : provide->getString("logical-name");
            if (!logical_name
                || *logical_name != llvm::StringRef{
                    provided_module.data(), provided_module.size()}) {
                continue;
            }
            if (selected_rule != nullptr) {
                throw std::runtime_error(
                    "clang-scan-deps P1689 output duplicates provider '"
                    + std::string{provided_module} + "'");
            }
            if (const std::optional<llvm::StringRef> reported_source =
                    provide->getString("source-path")) {
                std::filesystem::path reported{
                    reported_source->str()};
                if (reported.is_relative()) {
                    reported = command_directory / reported;
                }
                if (weakly_canonical_path(reported)
                    != weakly_canonical_path(source_path)) {
                    throw std::runtime_error(
                        "clang-scan-deps P1689 provider path differs "
                        "from selected source for module '"
                        + std::string{provided_module} + "'");
                }
            }
            selected_rule = rule;
        }
    }
    if (selected_rule == nullptr) {
        throw std::runtime_error(
            "clang-scan-deps P1689 output does not provide selected "
            "module '" + std::string{provided_module} + "'");
    }
    std::vector<std::string> imports;
    if (const llvm::json::Array* requirements =
            selected_rule->getArray("requires")) {
        for (const llvm::json::Value& require_value :
             *requirements) {
            const llvm::json::Object* requirement =
                require_value.getAsObject();
            const std::optional<llvm::StringRef> logical_name =
                requirement == nullptr
                ? std::nullopt
                : requirement->getString("logical-name");
            if (!logical_name || logical_name->empty()) {
                throw std::runtime_error(
                    "clang-scan-deps P1689 requirement has no logical "
                    "name for module '" + std::string{provided_module}
                    + "'");
            }
            std::string imported = logical_name->str();
            if (imported.starts_with(':')) {
                const std::size_t partition =
                    provided_module.find(':');
                imported =
                    std::string{provided_module.substr(0, partition)}
                    + imported;
            }
            imports.push_back(std::move(imported));
        }
    }
    std::ranges::sort(imports);
    imports.erase(
        std::ranges::unique(imports).begin(), imports.end());
    return imports;
}

void scan_active_module_imports(
    module_build_record& record,
    const std::filesystem::path& clang_scan_deps,
    const std::string& clang_binary_sha256,
    const std::string& clang_scan_deps_sha256,
    const std::filesystem::path& scan_root,
    const relocation_context& relocation,
    std::vector<frontend_action_record>& observed_actions) {
    const active_import_artifact_paths artifacts =
        derive_active_import_artifact_paths(
            scan_root,
            record.module_name,
            record.source_path,
            record.base_arguments,
            relocation,
            clang_binary_sha256,
            clang_scan_deps_sha256);
    const std::filesystem::path pcm_root =
        scan_root.parent_path();
    validate_owned_leaf(
        pcm_root, artifacts.database, "P1689 scan database");
    validate_owned_leaf(
        pcm_root, artifacts.result, "P1689 scan result");
    validate_owned_leaf(
        pcm_root, artifacts.object, "P1689 scan object");
    validate_owned_leaf(
        pcm_root, artifacts.diagnostics, "P1689 diagnostics");
    remove_owned_leaf_if_present(
        pcm_root, artifacts.object, "P1689 scan object");
    [[maybe_unused]] scoped_artifact_cleanup object_cleanup{
        artifacts.object};
    record.dependency_scan_database_path = artifacts.database;
    record.dependency_scan_result_path = artifacts.result;
    record.dependency_scan_diagnostics_path = artifacts.diagnostics;
    std::ostringstream database;
    database
        << "[\n  {\n    \"directory\": \""
        << json_escape(record.command.Directory)
        << "\",\n    \"file\": \""
        << json_escape(record.source_path.string())
        << "\",\n    \"arguments\": [";
    std::vector<std::string> scan_command =
        record.base_arguments;
    scan_command.emplace_back("-c");
    scan_command.push_back(record.source_path.string());
    scan_command.emplace_back("-o");
    scan_command.push_back(artifacts.object.string());
    for (std::size_t index = 0;
         index < scan_command.size(); ++index) {
        if (index != 0) database << ", ";
        database << "\"" << json_escape(scan_command[index])
                 << "\"";
    }
    database << "]\n  }\n]\n";
    const std::string database_document = database.str();
    atomic_write_owned_file(
        pcm_root,
        artifacts.database,
        database_document,
        "P1689 scan database");
    record.dependency_scan_database_sha256 =
        bioseq::common::sha256_hex(database_document);
    record.relocatable_dependency_scan_database_sha256 =
        relocatable_compiler_document_hash(
            database_document,
            relocation,
            record.base_arguments.front(),
            clang_binary_sha256);
    const std::filesystem::path staged_result =
        create_owned_staging_file(
            pcm_root, artifacts.result, "P1689 scan result");
    scoped_artifact_cleanup result_cleanup{staged_result};
    const std::filesystem::path staged_diagnostics =
        create_owned_staging_file(
            pcm_root, artifacts.diagnostics, "P1689 diagnostics");
    scoped_artifact_cleanup diagnostics_cleanup{
        staged_diagnostics};
    const std::vector<std::string> arguments{
        clang_scan_deps.string(),
        "-compilation-database=" + artifacts.database.string(),
        "-format=p1689",
        "-j",
        "1",
        "-o",
        staged_result.string(),
    };
    execute_clang(
        clang_scan_deps,
        arguments,
        record.command.Directory,
        record.module_name,
        "P1689 dependency discovery",
        staged_diagnostics);
    publish_owned_staging_file(
        pcm_root,
        staged_result,
        artifacts.result,
        "P1689 scan result");
    result_cleanup.release();
    publish_owned_staging_file(
        pcm_root,
        staged_diagnostics,
        artifacts.diagnostics,
        "P1689 diagnostics");
    diagnostics_cleanup.release();
    const std::string diagnostics =
        read_artifact_bytes(artifacts.diagnostics);
    record.dependency_scan_diagnostics_sha256 =
        bioseq::common::sha256_hex(diagnostics);
    record.relocatable_dependency_scan_diagnostics_sha256 =
        relocatable_document_hash(
            diagnostics, relocation);
    record.dependency_scan_result_sha256 =
        bioseq::common::sha256_hex(read_artifact_bytes(artifacts.result));
    record.relocatable_dependency_scan_result_sha256 =
        relocatable_document_hash(
            read_artifact_bytes(artifacts.result), relocation);
    record.description.imports = parse_p1689_imports(
        artifacts.result,
        record.module_name,
        record.source_path,
        record.command.Directory);
    observed_actions.push_back(frontend_action_record{
        .kind = frontend_action_kind::p1689_dependency_discovery,
        .input_role =
            compilation_input_role::dependency_provider,
        .outcome = frontend_action_outcome::executed,
        .module_name = record.module_name,
        .source_sha256 = record.source_sha256,
    });
}

/// Uses the codegen-linked Clang Make dependency scanner to obtain the exact
/// dependency bytes for the already-authenticated module command. Make mode
/// deliberately uses Clang's dependency forwarder, which includes skipped
/// include-next files that the P1689 module collector omits. Its
/// ReadPCHAndPreprocessAction lexes through EOF without entering the
/// body-bearing Sema path. The separately sealed P1689 result remains the
/// sole import-graph authority.
void scan_make_dependencies(module_build_record& record) {
    if (record.direct_arguments.empty()
        || record.depfile_path.empty()) {
        throw std::runtime_error(
            "linked-Clang make dependency scan has incomplete inputs for '"
            + record.module_name + "'");
    }
    const std::filesystem::path pcm_root =
        record.depfile_path.parent_path().parent_path();
    validate_owned_leaf(
        pcm_root,
        record.depfile_path,
        "linked-Clang make dependency output");
    const std::filesystem::path scan_object =
        record.depfile_path.string() + ".scan.o";
    std::vector<std::string> arguments = record.direct_arguments;
    arguments.emplace_back("-c");
    arguments.push_back(record.source_path.string());
    arguments.emplace_back("-o");
    arguments.push_back(scan_object.string());
    arguments.emplace_back("-MD");
    arguments.emplace_back("-MF");
    arguments.push_back(record.depfile_path.string());
    arguments.emplace_back("-MT");
    arguments.push_back(record.depfile_path.string());
    clang::dependencies::DependencyScanningService service{
        clang::dependencies::ScanningMode::CanonicalPreprocessing,
        clang::dependencies::ScanningOutputFormat::Make,
    };
    clang::tooling::DependencyScanningTool scanner{service};
    clang::IgnoringDiagConsumer diagnostics;
    const std::optional<std::string> make_output =
        scanner.getDependencyFile(
            arguments,
            record.command.Directory,
            diagnostics);
    if (!make_output || make_output->empty()) {
        throw std::runtime_error(
            "linked-Clang make dependency scan failed for module '"
            + record.module_name + "'");
    }
    atomic_write_owned_file(
        pcm_root,
        record.depfile_path,
        *make_output,
        "linked-Clang make dependency output");
    // Atomic owned-file publication intentionally preserves mtimes. A dependency-only
    // miss caused by a newer input must nevertheless become a validated warm
    // cache hit after this successful scan, even when the dependency bytes are
    // unchanged.
    mark_artifact_complete(record.depfile_path);
    record.discovered_dependencies = parse_make_depfile(
        record.depfile_path,
        record.command.Directory);
    const std::filesystem::path canonical_source =
        weakly_canonical_path(record.source_path);
    if (record.discovered_dependencies.empty()
        || !std::ranges::any_of(
            record.discovered_dependencies,
            [&](const std::filesystem::path& dependency) {
                return weakly_canonical_path(dependency)
                    == canonical_source;
            })) {
        throw std::runtime_error(
            "linked-Clang make dependency scan omitted its canonical "
            "source for module '" + record.module_name + "'");
    }
}

[[nodiscard]] std::unique_ptr<clang::tooling::CompilationDatabase>
load_compile_database(const std::filesystem::path& path) {
    std::string error;
    std::unique_ptr<clang::tooling::CompilationDatabase> result =
        clang::tooling::JSONCompilationDatabase::loadFromFile(
            path.string(),
            error,
            clang::tooling::JSONCommandLineSyntax::AutoDetect);
    if (!result) {
        throw std::runtime_error(
            "cannot load compile database " + path.string()
            + ": " + error);
    }
    return result;
}

[[nodiscard]] std::filesystem::path compilation_root(
    const clang::tooling::CompilationDatabase& database) {
    const std::vector<clang::tooling::CompileCommand> commands =
        database.getAllCompileCommands();
    if (commands.empty()) {
        throw std::runtime_error(
            "compile database has no commands from which to derive "
            "its configured build root");
    }
    const std::filesystem::path root =
        (std::filesystem::path{commands.front().Directory}.is_absolute()
             ? std::filesystem::path{commands.front().Directory}
             : std::filesystem::absolute(
                   std::filesystem::path{
                       commands.front().Directory}))
            .lexically_normal();
    const std::filesystem::path identity =
        absolute_normalized_path(root);
    for (const clang::tooling::CompileCommand& command :
         commands) {
        if (absolute_normalized_path(command.Directory)
            != identity) {
            throw std::runtime_error(
                "compile database spans multiple configured build "
                "roots");
        }
    }
    return root;
}

}  // namespace clang_backend

[[nodiscard]] translation_unit scan_translation_unit_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile) {
    std::vector<translation_unit> units;
    std::vector<std::string> arguments = compiler_arguments;
    if (std::ranges::none_of(
            arguments,
            [](std::string_view argument) {
                return argument.starts_with("-std=");
            })) {
        arguments.emplace_back("-std=c++26");
    }
    arguments.emplace_back("-Wno-unknown-attributes");
    arguments.emplace_back("-Wno-ignored-attributes");
    const bool success = clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<clang_backend::structural_action>(
            units, std::string{compiler_profile}),
        llvm::StringRef{source.data(), source.size()},
        arguments,
        source_path.string());
    if (!success || units.size() != 1) {
        throw std::runtime_error(
            source_path.string()
            + ": Clang structural pass reported errors");
    }
    return std::move(units.front());
}

[[nodiscard]] translation_unit merge_declaration_probe(
    translation_unit syntax,
    translation_unit semantics) {
    for (const declaration_record& declaration :
         semantics.declarations) {
        if (declaration.payload_kind
            == declaration_payload_kind::function_body) {
            throw std::runtime_error(
                semantics.source_path.string()
                + ": declaration probe contains a function body");
        }
        if (declaration.kind == declaration_kind::field
            && declaration.payload_kind
                == declaration_payload_kind::variable_initializer) {
            throw std::runtime_error(
                semantics.source_path.string()
                + ": declaration probe contains a default member initializer");
        }
    }

    std::vector<declaration_record> associated;
    std::set<std::pair<std::string, std::string>> associations;
    for (const declaration_record& source_declaration :
         syntax.declarations) {
        if (source_declaration.source_coordinate.empty()) continue;
        const auto association = std::pair{
            source_declaration.source_identity,
            source_declaration.source_coordinate};
        if (!associations.insert(association).second) {
            throw std::runtime_error(
                syntax.source_path.string()
                + ": declaration probe duplicates a source identity/coordinate");
        }
        std::vector<declaration_record*> matches;
        for (declaration_record& semantic : semantics.declarations) {
            if (semantic.kind == source_declaration.kind
                && semantic.name_range.begin.byte_offset
                    == source_declaration.name_range.begin.byte_offset) {
                matches.push_back(&semantic);
            }
        }
        if (matches.size() != 1
            || matches.front()->qualified_name
                != source_declaration.qualified_name
            || matches.front()->name != source_declaration.name) {
            throw std::runtime_error(
                syntax.source_path.string()
                + ": declaration probe source identity cannot be associated with a declaration");
        }
        declaration_record merged = *matches.front();
        if (merged.stable_usr.empty()) {
            throw std::runtime_error(
                syntax.source_path.string()
                + ": declaration probe declaration has no stable Clang USR");
        }
        merged.source_identity = source_declaration.source_identity;
        merged.source_coordinate =
            source_declaration.source_coordinate;
        merged.immediate_namespace_source_identity =
            source_declaration.immediate_namespace_source_identity;
        merged.explicit_declared_type =
            source_declaration.explicit_declared_type.empty()
            ? merged.explicit_declared_type
            : source_declaration.explicit_declared_type;
        merged.referenced_declaration_source_identities =
            source_declaration
                .referenced_declaration_source_identities;
        merged.declaration_range =
            source_declaration.declaration_range;
        merged.declaration_head_range =
            source_declaration.declaration_head_range;
        merged.body_or_initializer_range =
            source_declaration.body_or_initializer_range;
        merged.range = source_declaration.range;
        merged.name_range = source_declaration.name_range;
        merged.attributes = source_declaration.attributes;
        merged.payload_kind = source_declaration.payload_kind;
        merged.specifiers.multiple_declarators =
            source_declaration.specifiers.multiple_declarators;
        associated.push_back(std::move(merged));
    }
    if (associated.empty()) {
        throw std::runtime_error(
            syntax.source_path.string()
            + ": declaration probe contains no source-identity declarations");
    }
    std::ranges::sort(
        associated,
        {},
        [](const declaration_record& declaration) {
            return declaration.declaration_range.begin.byte_offset;
        });
    semantics.declarations = std::move(associated);
    semantics.mode = collection_mode::declaration_probe;
    semantics.module_name = syntax.module_name;
    semantics.source_module = std::move(syntax.source_module);
    semantics.tokens = std::move(syntax.tokens);
    semantics.preprocessor_directives =
        std::move(syntax.preprocessor_directives);
    return semantics;
}

[[nodiscard]] translation_unit scan_declaration_probe_from_code(
    const std::filesystem::path& source_path,
    std::string_view body_free_source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile) {
    translation_unit syntax = detail::scan_token_slice_impl(
        source_path, body_free_source, true);
    detail::validate_body_free_probe_syntax(syntax);
    translation_unit semantics = scan_translation_unit_from_code(
        source_path,
        body_free_source,
        compiler_arguments,
        compiler_profile);
    return merge_declaration_probe(
        std::move(syntax), std::move(semantics));
}

[[nodiscard]] translation_unit scan_declaration_probe_from_database(
    const std::filesystem::path& probe_source_path,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile,
    const std::filesystem::path& pcm_root) {
    return scan_declaration_probe_from_database(
        probe_source_path,
        read_artifact_bytes(probe_source_path),
        signature_source_path,
        compile_database,
        compiler_profile,
        pcm_root);
}

[[nodiscard]] translation_unit scan_declaration_probe_from_database(
    const std::filesystem::path& probe_source_path,
    std::string_view body_free_source,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile,
    const std::filesystem::path& pcm_root) {
    const std::filesystem::path probe_identity =
        clang_backend::weakly_canonical_path(probe_source_path);
    const std::filesystem::path signature_identity =
        clang_backend::weakly_canonical_path(signature_source_path);
    if (probe_identity == signature_identity) {
        throw std::runtime_error(
            "declaration probe and signature donor must be distinct");
    }
    if (!std::filesystem::is_regular_file(signature_identity)) {
        throw std::runtime_error(
            "declaration-probe signature donor source does not exist");
    }
    const translation_unit signature =
        scan_token_slice(signature_identity);
    if (!signature.source_module.is_interface
        || signature.source_module.is_partition
        || signature.source_module.provided_module.empty()) {
        throw std::runtime_error(
            signature_identity.string()
            + ": declaration-probe signature donor must be an exported primary interface");
    }
    translation_unit syntax = detail::scan_token_slice_impl(
        probe_identity, body_free_source, true);
    detail::validate_body_free_probe_syntax(syntax);
    if (!std::ranges::contains(
            syntax.source_module.imported_modules,
            signature.source_module.provided_module)) {
        throw std::runtime_error(
            probe_identity.string()
            + ": declaration probe must import its signature-donor module");
    }

    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(compile_database);
    const std::string effective_profile = compiler_profile.empty()
        ? std::string{"production"}
        : std::string{compiler_profile};
    const std::vector<clang::tooling::CompileCommand> all_commands =
        database->getAllCompileCommands();
    std::vector<std::filesystem::path> prepared_roots;
    prepared_roots.reserve(all_commands.size());
    for (const clang::tooling::CompileCommand& command : all_commands) {
        if (!clang_backend::command_is_satisfiable(command)) {
            throw std::runtime_error(
                "declaration-probe owned compile database has an "
                "unsatisfiable root command");
        }
        prepared_roots.push_back(
            clang_backend::absolute_normalized_path(
                clang_backend::absolute_command_path(
                    command.Directory, command.Filename)));
    }
    std::ranges::sort(prepared_roots);
    if (prepared_roots.empty()
        || std::ranges::adjacent_find(prepared_roots)
            != prepared_roots.end()) {
        throw std::runtime_error(
            "declaration-probe owned compile database has an empty or "
            "duplicate root inventory");
    }
    clang_backend::validate_owned_compile_database(
        *database,
        prepared_roots,
        effective_profile,
        pcm_root);

    std::map<std::string, std::filesystem::path, std::less<>>
        prepared_pcm_by_module;
    std::set<std::filesystem::path> prepared_pcm_paths;
    const auto add_prepared_pcm =
        [&](std::string module_name,
            const std::filesystem::path& configured_pcm) {
            const std::filesystem::path prepared_pcm =
                clang_backend::absolute_normalized_path(configured_pcm);
            if (module_name.empty()
                || !clang_backend::path_within_root(
                    prepared_pcm, pcm_root)
                || !std::filesystem::is_regular_file(prepared_pcm)) {
                throw std::runtime_error(
                    "declaration-probe PCM mapping is missing or outside "
                    "the prepared authority root");
            }
            const auto [position, inserted] =
                prepared_pcm_by_module.emplace(
                    std::move(module_name), prepared_pcm);
            if (!inserted && position->second != prepared_pcm) {
                throw std::runtime_error(
                    "declaration-probe prepared module has conflicting PCM mappings");
            }
            if (inserted
                && !prepared_pcm_paths.insert(prepared_pcm).second) {
                throw std::runtime_error(
                    "declaration-probe prepared modules alias one PCM");
            }
        };
    for (const clang::tooling::CompileCommand& command : all_commands) {
        const std::filesystem::path source =
            clang_backend::absolute_normalized_path(
                clang_backend::absolute_command_path(
                    command.Directory, command.Filename));
        const clang_backend::lexical_module_description provider =
            clang_backend::describe_module_provider(source);
        if (provider.provided_module.empty()) {
            throw std::runtime_error(
                source.string()
                + ": declaration-probe prepared root provides no named module");
        }
        std::optional<std::filesystem::path> output_pcm;
        for (const std::string& argument : command.CommandLine) {
            if (argument.starts_with("-fmodule-output=")) {
                if (output_pcm) {
                    throw std::runtime_error(
                        "declaration-probe prepared root has multiple PCM outputs");
                }
                output_pcm = argument.substr(
                    std::string_view{"-fmodule-output="}.size());
            } else if (argument.starts_with("-fmodule-file=")) {
                const std::string_view mapping =
                    std::string_view{argument}.substr(
                        std::string_view{"-fmodule-file="}.size());
                const std::size_t separator = mapping.find('=');
                if (separator == std::string_view::npos
                    || separator == 0
                    || separator + 1 == mapping.size()) {
                    throw std::runtime_error(
                        "declaration-probe prepared root has a malformed PCM mapping");
                }
                add_prepared_pcm(
                    std::string{mapping.substr(0, separator)},
                    std::filesystem::path{
                        std::string{mapping.substr(separator + 1)}});
            }
        }
        if (!output_pcm) {
            throw std::runtime_error(
                "declaration-probe prepared root has no PCM output");
        }
        add_prepared_pcm(
            provider.provided_module, *output_pcm);
    }
    const std::vector<clang::tooling::CompileCommand> donor_commands =
        database->getCompileCommands(signature_identity.string());
    const auto donor = std::ranges::find_if(
        donor_commands,
        clang_backend::command_is_satisfiable);
    if (donor == donor_commands.end()) {
        throw std::runtime_error(
            signature_identity.string()
            + ": owned compile database has no satisfiable signature-donor command");
    }
    if (!prepared_pcm_by_module.contains(
            signature.source_module.provided_module)) {
        throw std::runtime_error(
            "signature-donor command has no prepared PCM output");
    }
    clang::tooling::CompileCommand probe_command =
        clang_backend::retarget_compile_command(
            *donor, signature_identity, probe_identity);
    std::map<std::string, std::filesystem::path, std::less<>>
        existing_probe_mappings;
    for (const std::string& argument : probe_command.CommandLine) {
        if (!argument.starts_with("-fmodule-file=")) continue;
        const std::string_view mapping =
            std::string_view{argument}.substr(
                std::string_view{"-fmodule-file="}.size());
        const std::size_t separator = mapping.find('=');
        if (separator == std::string_view::npos
            || separator == 0
            || separator + 1 == mapping.size()
            || !existing_probe_mappings.emplace(
                    std::string{mapping.substr(0, separator)},
                    clang_backend::absolute_normalized_path(
                        std::filesystem::path{
                            std::string{mapping.substr(separator + 1)}}))
                    .second) {
            throw std::runtime_error(
                "signature-donor command has a malformed or duplicate PCM mapping");
        }
    }
    for (const auto& [module_name, prepared_pcm] :
         prepared_pcm_by_module) {
        if (const auto existing = existing_probe_mappings.find(module_name);
            existing != existing_probe_mappings.end()) {
            if (existing->second != prepared_pcm) {
                throw std::runtime_error(
                    "signature-donor command has a stale PCM mapping for '"
                    + module_name + "'");
            }
            continue;
        }
        probe_command.CommandLine.push_back(
            "-fmodule-file=" + module_name + "="
            + prepared_pcm.string());
    }
    const clang_backend::scoped_compile_database_cwd cwd{
        compile_database};
    const clang::tooling::CommandLineArguments adjusted =
        clang_backend::structural_arguments_adjuster()(
            probe_command.CommandLine,
            probe_identity.string());
    const std::size_t compiler_index =
        clang_backend::compiler_argument_index(adjusted);
    std::vector<std::string> probe_arguments;
    for (std::size_t index = compiler_index + 1;
         index < adjusted.size();
         ++index) {
        const std::string& argument = adjusted[index];
        if (!argument.empty() && argument.front() != '-'
            && clang_backend::absolute_command_path(
                   probe_command.Directory, argument)
                == probe_identity) {
            continue;
        }
        probe_arguments.push_back(argument);
    }
    translation_unit semantics = scan_translation_unit_from_code(
        probe_identity,
        body_free_source,
        probe_arguments,
        effective_profile);
    return merge_declaration_probe(
        std::move(syntax), std::move(semantics));
}

[[nodiscard]] structural_index scan_translation_units_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile,
    const std::filesystem::path& pcm_root) {
    structural_index result{
        .clang_release = CLANG_VERSION_STRING,
        .compilation_signature = bioseq::common::sha256_hex(
            read_artifact_bytes(compile_database)
            + "\n" + std::string{compiler_profile}
            + "\n" + pcm_root.generic_string()
            + "\n" + std::string{CLANG_VERSION_STRING}),
    };
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(compile_database);
    clang_backend::validate_owned_compile_database(
        *database,
        source_paths,
        compiler_profile,
        pcm_root);
    const clang_backend::scoped_compile_database_cwd cwd{
        compile_database};
    const std::vector<std::string> units =
        clang_backend::buildable_units(*database, source_paths);
    const clang_backend::satisfiable_compilation_database
        satisfiable{*database};
    clang::tooling::ClangTool tool{satisfiable, units};
    tool.appendArgumentsAdjuster(
        clang_backend::structural_arguments_adjuster());
    clang_backend::structural_factory factory{
        result.translation_units,
        std::string{compiler_profile}};
    if (tool.run(&factory) != 0) {
        throw std::runtime_error(
            "Clang structural compile-database pass reported errors");
    }
    return result;
}

[[nodiscard]] module_graph scan_module_graph_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile,
    const std::filesystem::path& pcm_root) {
    module_graph result{
        .clang_release = CLANG_VERSION_STRING,
        .compilation_signature = bioseq::common::sha256_hex(
            read_artifact_bytes(compile_database)
            + "\n" + std::string{compiler_profile}
            + "\n" + pcm_root.generic_string()
            + "\n" + std::string{CLANG_VERSION_STRING}),
    };
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(compile_database);
    clang_backend::validate_owned_compile_database(
        *database,
        source_paths,
        compiler_profile,
        pcm_root);
    const clang_backend::scoped_compile_database_cwd cwd{
        compile_database};
    const std::vector<std::string> units =
        clang_backend::buildable_units(*database, source_paths);
    const clang_backend::satisfiable_compilation_database
        satisfiable{*database};
    clang::tooling::ClangTool tool{satisfiable, units};
    tool.appendArgumentsAdjuster(
        clang_backend::structural_arguments_adjuster());
    clang_backend::module_edges_factory factory{result.units};
    if (tool.run(&factory) != 0) {
        throw std::runtime_error(
            "Clang module-graph compile-database pass reported errors");
    }
    return result;
}

[[nodiscard]] module_graph_unit scan_module_graph_from_code(
    std::string_view source,
    const std::vector<std::string>& compiler_arguments) {
    module_graph result{
        .clang_release = CLANG_VERSION_STRING,
    };
    const bool success = clang::tooling::runToolOnCodeWithArgs(
        std::make_unique<clang_backend::module_edges_action>(
            result.units),
        llvm::StringRef{source.data(), source.size()},
        compiler_arguments,
        "unit.cppm");
    if (!success || result.units.size() != 1) {
        throw std::runtime_error(
            "Clang module-graph pass reported errors");
    }
    return std::move(result.units.front());
}

[[nodiscard]] std::string linked_clang_release() {
    return CLANG_VERSION_STRING;
}

namespace {

inline constexpr std::string_view dependency_authority_suffix{
    ";canonical-token-import-sha256="};

[[nodiscard]] bool base_preparation_authority(
    std::string_view authority) noexcept {
    return authority == "support" || authority == "core"
        || authority == "final" || authority == "safety";
}

[[nodiscard]] bool lowercase_sha256(std::string_view digest) noexcept {
    return digest.size() == 64
        && std::ranges::all_of(
            digest,
            [](const char value) {
                return (value >= '0' && value <= '9')
                    || (value >= 'a' && value <= 'f');
            });
}

[[nodiscard]] bool valid_preparation_authority(
    std::string_view authority) noexcept {
    const std::size_t suffix = authority.find(
        dependency_authority_suffix);
    if (suffix == std::string_view::npos) {
        return base_preparation_authority(authority);
    }
    return base_preparation_authority(authority.substr(0, suffix))
        && authority.find(
               dependency_authority_suffix,
               suffix + dependency_authority_suffix.size())
            == std::string_view::npos
        && lowercase_sha256(authority.substr(
            suffix + dependency_authority_suffix.size()));
}

}  // namespace

[[nodiscard]] dependency_compilation_environment
prepare_compilation_environment(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& raw_compile_database,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    const std::filesystem::path& configured_pcm_root,
    std::string_view compiler_profile,
    std::string_view authority) {
    if (source_paths.empty()) {
        throw std::runtime_error(
            "Clang preparation requires at least one source");
    }
    if (compiler_profile != "production"
        && compiler_profile != "test_indexes"
        && compiler_profile != "test_support") {
        throw std::runtime_error(
            "Clang preparation received an unknown compiler profile");
    }
    if (!valid_preparation_authority(authority)) {
        throw std::runtime_error(
            "Clang preparation received an unknown authority");
    }
    if (!std::filesystem::is_regular_file(clang_binary)) {
        throw std::runtime_error(
            "Clang preparation binary does not exist: "
            + clang_binary.string());
    }
    const auto require_configured_hash =
        [](const std::filesystem::path& path,
           std::string_view expected,
           std::string_view role) {
            if (expected.size() != 64
                || bioseq::common::sha256_hex(read_artifact_bytes(path)) != expected) {
                throw std::runtime_error(
                    std::string{role}
                    + " differs from its configure-time SHA-256");
            }
        };
    require_configured_hash(
        clang_binary,
        expected_clang_sha256,
        "selected Clang");
    require_configured_hash(
        clang_scan_deps,
        expected_clang_scan_deps_sha256,
        "selected clang-scan-deps");
    require_configured_hash(
        llvm_config,
        expected_llvm_config_sha256,
        "selected llvm-config");
    require_configured_hash(
        compiler_module_inventory,
        expected_compiler_module_inventory_sha256,
        "selected compiler-module inventory");
    for (const std::filesystem::path& requested : source_paths) {
        const std::filesystem::path source =
            requested.is_absolute()
            ? requested.lexically_normal()
            : std::filesystem::absolute(requested)
                  .lexically_normal();
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error(
                "registered Clang preparation source does not exist: "
                + source.string());
        }
    }
    const std::filesystem::path pcm_root =
        clang_backend::ensure_preparation_root(
            configured_pcm_root);
    const std::filesystem::path relocation_root =
        clang_backend::derive_relocation_root(
            source_paths, raw_compile_database, pcm_root);
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(
            raw_compile_database);
    const clang_backend::relocation_context relocation =
        clang_backend::make_relocation_context(
            relocation_root,
            clang_backend::compilation_root(*database));
    const clang_backend::selected_toolchain_identity
        toolchain = clang_backend::validate_selected_toolchain(
            clang_binary,
            clang_scan_deps,
            llvm_config,
            compiler_module_inventory);
    const std::string clang_binary_sha256 =
        bioseq::common::sha256_hex(read_artifact_bytes(clang_binary));
    const std::string& clang_scan_deps_version =
        toolchain.clang_scan_deps_full_version;
    const std::string clang_scan_deps_sha256 =
        bioseq::common::sha256_hex(read_artifact_bytes(clang_scan_deps));
    const std::filesystem::path import_scan_root =
        clang_backend::ensure_owned_directory(
            pcm_root, ".module-dependency-scan");
    const std::filesystem::path depfile_root =
        clang_backend::ensure_owned_directory(
            pcm_root, ".dependencies");

    const std::vector<clang::tooling::CompileCommand> all_commands =
        database->getAllCompileCommands();
    std::map<
        std::filesystem::path,
        clang::tooling::CompileCommand>
        raw_commands;
    // The raw database includes unrelated targets. Preserve ambiguity until
    // Dependency traversal confirms that a command belongs to this closure.
    std::set<std::filesystem::path> ambiguous_raw_commands;
    for (const clang::tooling::CompileCommand& command :
         all_commands) {
        const std::filesystem::path source =
            clang_backend::absolute_command_path(
                command.Directory, command.Filename);
        if (source.extension() != ".cppm") {
            continue;
        }
        const auto found = raw_commands.find(source);
        const int candidate_preference =
            clang_backend::command_preference(
                command, compiler_profile);
        const int existing_preference =
            found == raw_commands.end()
            ? std::numeric_limits<int>::min()
            : clang_backend::command_preference(
                  found->second, compiler_profile);
        if (found != raw_commands.end()
            && candidate_preference == existing_preference) {
            std::vector<std::filesystem::path>
                candidate_dependencies;
            std::vector<std::filesystem::path>
                existing_dependencies;
            const std::vector<std::string> candidate_arguments =
                clang_backend::normalized_base_arguments(
                    command,
                    source,
                    clang_binary,
                    toolchain.resource_dir,
                    relocation_root,
                    compiler_profile,
                    candidate_dependencies);
            const std::vector<std::string> existing_arguments =
                clang_backend::normalized_base_arguments(
                    found->second,
                    source,
                    clang_binary,
                    toolchain.resource_dir,
                    relocation_root,
                    compiler_profile,
                    existing_dependencies);
            if (candidate_arguments != existing_arguments
                || clang_backend::command_input_hash(
                       candidate_dependencies)
                    != clang_backend::command_input_hash(
                        existing_dependencies)
                || clang_backend::weakly_canonical_path(
                       command.Directory)
                    != clang_backend::weakly_canonical_path(
                        found->second.Directory)) {
                ambiguous_raw_commands.insert(source);
            }
        }
        if (found == raw_commands.end()
            || candidate_preference > existing_preference) {
            raw_commands.insert_or_assign(source, command);
            ambiguous_raw_commands.erase(source);
        }
    }
    std::map<
        std::filesystem::path,
        clang::tooling::CompileCommand>
        selected_commands;
    std::set<std::filesystem::path> ambiguous_selected_commands;
    for (const auto& [source, command] : raw_commands) {
        const std::vector<std::filesystem::path> alternates =
            clang_backend::profile_alternate_paths(source);
        const bool known_sql_profile_path =
            clang_backend::known_generated_sql_profile_path(source);
        const bool generated_profile_variant =
            !alternates.empty()
            && (std::ranges::any_of(
                    alternates,
                    [&raw_commands](
                        const std::filesystem::path& alternate) {
                        return raw_commands.contains(alternate)
                            || std::filesystem::is_regular_file(
                                alternate);
                    })
                || known_sql_profile_path);
        if (!generated_profile_variant) {
            if (std::filesystem::is_regular_file(source)) {
                selected_commands.emplace(source, command);
                if (ambiguous_raw_commands.contains(source)) {
                    ambiguous_selected_commands.insert(source);
                }
            }
            continue;
        }
        const std::filesystem::path selected_source =
            clang_backend::path_for_profile(
                source, compiler_profile);
        if (!std::filesystem::is_regular_file(selected_source)) {
            continue;
        }
        const auto exact = raw_commands.find(selected_source);
        clang::tooling::CompileCommand selected =
            exact == raw_commands.end()
            ? clang_backend::retarget_compile_command(
                  command, source, selected_source)
            : exact->second;
        const bool selected_is_ambiguous =
            exact == raw_commands.end()
            ? ambiguous_raw_commands.contains(source)
            : ambiguous_raw_commands.contains(selected_source);
        const auto found =
            selected_commands.find(selected_source);
        const int selected_preference =
            clang_backend::command_preference(
                selected, compiler_profile);
        const int existing_selected_preference =
            found == selected_commands.end()
            ? std::numeric_limits<int>::min()
            : clang_backend::command_preference(
                  found->second, compiler_profile);
        if (found == selected_commands.end()
            || selected_preference > existing_selected_preference) {
            selected_commands.insert_or_assign(
                selected_source, std::move(selected));
            if (selected_is_ambiguous) {
                ambiguous_selected_commands.insert(selected_source);
            } else {
                ambiguous_selected_commands.erase(selected_source);
            }
        } else if (
            selected_is_ambiguous
            && selected_preference == existing_selected_preference) {
            ambiguous_selected_commands.insert(selected_source);
        }
    }

    std::map<std::string, clang_backend::module_build_record, std::less<>>
        modules;
    std::map<std::filesystem::path, std::string> module_by_source;
    for (const auto& [source, command] : selected_commands) {
        clang_backend::lexical_module_description description =
            clang_backend::describe_module_provider(source);
        if (description.provided_module.empty()) {
            continue;
        }
        clang_backend::module_build_record record{
            .module_name = description.provided_module,
            .source_path = source,
            .description = std::move(description),
            .command = command,
        };
        const auto [position, inserted] =
            modules.emplace(record.module_name, std::move(record));
        if (!inserted
            && position->second.source_path != source) {
            throw std::runtime_error(
                "duplicate Clang module provider for '"
                + position->first + "': "
                + position->second.source_path.string() + " and "
                + source.string());
        }
        module_by_source.emplace(source, position->first);
    }
    for (const auto& [source, command] : selected_commands) {
        static_cast<void>(command);
        const std::vector<std::filesystem::path> alternates =
            clang_backend::profile_alternate_paths(source);
        const std::optional<std::string_view> source_profile =
            clang_backend::profile_path_component(source);
        if (!alternates.empty() && source_profile
            && (std::ranges::any_of(
                    alternates,
                    [&raw_commands](
                        const std::filesystem::path& alternate) {
                        return raw_commands.contains(alternate)
                            || std::filesystem::is_regular_file(
                                alternate);
                    })
                || clang_backend::known_generated_sql_profile_path(
                    source))
            && *source_profile != compiler_profile) {
            throw std::runtime_error(
                "owned Clang provider inventory leaked generated profile '"
                + std::string{*source_profile} + "' into requested profile '"
                + std::string{compiler_profile} + "': "
                + source.string());
        }
    }

    std::vector<std::filesystem::path> roots;
    roots.reserve(source_paths.size());
    for (const std::filesystem::path& requested : source_paths) {
        const std::filesystem::path source =
            requested.is_absolute()
            ? requested.lexically_normal()
            : std::filesystem::absolute(requested)
                  .lexically_normal();
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error(
                "registered Clang preparation source does not exist: "
                + source.string());
        }
        if (!selected_commands.contains(source)) {
            throw std::runtime_error(
                "raw compile database has no command for registered "
                "source: " + source.string());
        }
        if (!module_by_source.contains(source)) {
            throw std::runtime_error(
                "registered source does not provide an exported named "
                "module: " + source.string());
        }
        roots.push_back(source);
    }
    std::ranges::sort(roots);
    if (std::ranges::adjacent_find(roots) != roots.end()) {
        throw std::runtime_error(
            "Clang preparation source list contains duplicate "
            "identities");
    }

    std::vector<std::filesystem::path> dependencies{
        raw_compile_database,
        clang_binary,
        clang_scan_deps,
        toolchain.llvm_config,
        toolchain.linked_clang_cpp,
        toolchain.linked_llvm_library,
    };
    std::vector<frontend_action_record> observed_actions;
    const std::string resource_directory_sha256 =
        clang_backend::directory_content_hash(
            toolchain.resource_dir, dependencies);
    std::optional<clang_backend::compiler_module_inventory>
        compiler_modules{
            clang_backend::load_compiler_module_inventory(
                compiler_module_inventory)};
    dependencies.push_back(
        compiler_modules->metadata_path);
    for (const auto& [module_name, source] :
         compiler_modules->source_by_module) {
        static_cast<void>(module_name);
        dependencies.push_back(source);
    }
    for (const std::string_view module_name :
         {"std", "std.compat"}) {
        const auto provider =
            modules.find(module_name);
        if (provider == modules.end()) continue;
        const std::filesystem::path expected =
            compiler_modules->source_by_module.at(
                std::string{module_name});
        if (clang_backend::weakly_canonical_path(
                provider->second.source_path)
            != expected) {
            throw std::runtime_error(
                "compiler-owned module '" + std::string{module_name}
                + "' is not provided by the selected Clang "
                "libc++.modules.json inventory: "
                + provider->second.source_path.string());
        }
        dependencies.push_back(expected);
    }
    std::map<std::string, int, std::less<>> visit_state;
    std::vector<std::string> build_order;
    std::vector<std::string> visit_stack;
    std::function<void(const std::string&)> visit =
        [&](const std::string& module_name) {
            const int state = visit_state[module_name];
            if (state == 2) return;
            if (state == 1) {
                std::string cycle;
                for (const std::string& name : visit_stack) {
                    if (!cycle.empty()) cycle += " -> ";
                    cycle += name;
                }
                if (!cycle.empty()) cycle += " -> ";
                cycle += module_name;
                throw std::runtime_error(
                    "Clang module dependency cycle: " + cycle);
            }
            const auto found = modules.find(module_name);
            if (found == modules.end()) {
                if (module_name == "std"
                    || module_name == "std.compat"
                    || module_name.starts_with("std.")) {
                    throw std::runtime_error(
                        "compiler-owned module import '" + module_name
                        + "' has no selected-Clang source provider in "
                          "the raw compile database");
                }
                throw std::runtime_error(
                    "Clang module import has no source provider in the "
                    "raw compile database: " + module_name);
            }
            visit_state[module_name] = 1;
            visit_stack.push_back(module_name);
            clang_backend::module_build_record& record =
                found->second;
            if (record.base_arguments.empty()) {
                if (ambiguous_selected_commands.contains(
                        record.source_path)) {
                    throw std::runtime_error(
                        "raw compile database has equal-preference commands "
                        "with unequal normalized flags for "
                        + record.source_path.string());
                }
                record.source_sha256 =
                    bioseq::common::sha256_hex(read_artifact_bytes(record.source_path));
                record.relocatable_source_sha256 =
                    clang_backend::relocatable_document_hash(
                        read_artifact_bytes(record.source_path), relocation);
                const std::size_t dependency_begin =
                    dependencies.size();
                record.base_arguments =
                    clang_backend::normalized_base_arguments(
                        record.command,
                        record.source_path,
                        clang_binary,
                        toolchain.resource_dir,
                        relocation_root,
                        compiler_profile,
                        dependencies);
                record.command_inputs.assign(
                    dependencies.begin()
                        + static_cast<std::ptrdiff_t>(
                            dependency_begin),
                    dependencies.end());
                std::ranges::sort(record.command_inputs);
                record.command_inputs.erase(
                    std::ranges::unique(
                        record.command_inputs).begin(),
                    record.command_inputs.end());
                record.command_input_sha256 =
                    clang_backend::command_input_hash(
                        record.command_inputs);
                record.relocatable_command_input_sha256 =
                    clang_backend::relocatable_command_input_hash(
                        record.command_inputs,
                        relocation);
                clang_backend::scan_active_module_imports(
                    record,
                    clang_scan_deps,
                    clang_binary_sha256,
                    clang_scan_deps_sha256,
                    import_scan_root,
                    relocation,
                    observed_actions);
            }
            for (const std::string& imported :
                 record.description.imports) {
                if (imported.starts_with("std")
                    && imported != "std"
                    && imported != "std.compat") {
                    throw std::runtime_error(
                        "unsupported compiler-owned module import '"
                        + imported + "' in module '" + module_name
                        + "'; selected-Clang policy permits only std "
                          "and std.compat");
                }
                visit(imported);
            }
            visit_stack.pop_back();
            visit_state[module_name] = 2;
            build_order.push_back(module_name);
        };
    for (const std::filesystem::path& root : roots) {
        visit(module_by_source.at(root));
    }

    std::unordered_map<std::string, std::string>
        dependency_hash_cache;
    std::unordered_map<std::string, std::string>
        relocatable_dependency_hash_cache;
    for (const std::string& module_name : build_order) {
        clang_backend::module_build_record& record =
            modules.at(module_name);
        dependencies.push_back(record.source_path);
        if (record.source_sha256.empty()
            || record.base_arguments.empty()
            || record.dependency_scan_result_sha256.empty()) {
            throw std::runtime_error(
                "Clang active-import discovery did not seal module '"
                + module_name + "'");
        }
        std::string argument_material;
        for (const std::string& argument : record.base_arguments) {
            argument_material += argument;
            argument_material.push_back('\0');
        }
        record.argument_sha256 =
            bioseq::common::sha256_hex(argument_material);
        record.relocatable_argument_sha256 =
            clang_backend::relocatable_argument_hash(
                record.base_arguments,
                relocation,
                clang_binary_sha256);
        std::vector<std::pair<std::string, std::string>>
            physical_import_keys;
        std::vector<std::pair<std::string, std::string>>
            relocatable_import_keys;
        for (const std::string& imported :
             record.description.imports) {
            const clang_backend::module_build_record& dependency =
                modules.at(imported);
            if (dependency.content_key.empty()
                || dependency.relocatable_content_key.empty()) {
                throw std::runtime_error(
                    "Clang module topological order is incomplete for '"
                    + record.module_name + "'");
            }
            physical_import_keys.emplace_back(
                imported, dependency.content_key);
            relocatable_import_keys.emplace_back(
                imported, dependency.relocatable_content_key);
        }
        const std::string content_material =
            clang_backend::predependency_material(
                compiler_profile,
                record.module_name,
                record.source_sha256,
                record.argument_sha256,
                record.command_input_sha256,
                record.dependency_scan_database_sha256,
                record.dependency_scan_result_sha256,
                record.dependency_scan_diagnostics_sha256,
                clang_binary_sha256,
                clang_scan_deps_sha256,
                physical_import_keys);
        const std::string relocatable_content_material =
            clang_backend::predependency_material(
                compiler_profile,
                record.module_name,
                record.relocatable_source_sha256,
                record.relocatable_argument_sha256,
                record.relocatable_command_input_sha256,
                record.relocatable_dependency_scan_database_sha256,
                record.relocatable_dependency_scan_result_sha256,
                record.relocatable_dependency_scan_diagnostics_sha256,
                clang_binary_sha256,
                clang_scan_deps_sha256,
                relocatable_import_keys);
        record.depfile_path =
            clang_backend::derive_make_depfile_path(
                depfile_root,
                module_name,
                relocatable_content_material);
        clang_backend::validate_owned_leaf(
            pcm_root,
            record.depfile_path,
            "linked-Clang make dependency output");
        record.direct_arguments = record.base_arguments;
        std::set<std::string, std::less<>> mapped_modules;
        std::vector<std::string> mapping_order;
        std::function<void(const std::string&)> collect_mapping =
            [&](const std::string& imported) {
                if (!mapped_modules.insert(imported).second) {
                    return;
                }
                const clang_backend::module_build_record& dependency =
                    modules.at(imported);
                for (const std::string& transitive :
                     dependency.description.imports) {
                    collect_mapping(transitive);
                }
                mapping_order.push_back(imported);
            };
        for (const std::string& imported :
             record.description.imports) {
            collect_mapping(imported);
        }
        for (const std::string& imported : mapping_order) {
            const clang_backend::module_build_record& dependency =
                modules.at(imported);
            record.direct_arguments.push_back(
                "-fmodule-file=" + imported + "="
                + dependency.pcm_path.string());
        }
        const bool dependency_scan_cache_hit =
            clang_backend::depfile_is_current(
                record.depfile_path,
                record.command.Directory);
        if (!dependency_scan_cache_hit) {
            clang_backend::scan_make_dependencies(record);
        }
        record.discovered_dependencies =
            clang_backend::parse_make_depfile(
                record.depfile_path,
                record.command.Directory);
        const std::filesystem::path canonical_record_source =
            clang_backend::weakly_canonical_path(
                record.source_path);
        if (std::filesystem::is_symlink(record.depfile_path)
            || !std::filesystem::is_regular_file(
                record.depfile_path)
            || record.discovered_dependencies.empty()
            || !std::ranges::any_of(
                record.discovered_dependencies,
                [&](const std::filesystem::path& dependency) {
                    return clang_backend::weakly_canonical_path(
                        dependency) == canonical_record_source;
                })) {
            throw std::runtime_error(
                "Clang make dependency scan returned an invalid or "
                "source-free closure for '"
                + module_name + "'");
        }
        record.dependency_sha256 =
            clang_backend::dependency_content_hash(
                record.discovered_dependencies,
                pcm_root,
                dependency_hash_cache);
        record.relocatable_dependency_sha256 =
            clang_backend::relocatable_dependency_content_hash(
                record.discovered_dependencies,
                pcm_root,
                relocation,
                relocatable_dependency_hash_cache);
        record.depfile_sha256 =
            bioseq::common::sha256_hex(read_artifact_bytes(record.depfile_path));
        record.relocatable_depfile_sha256 =
            clang_backend::relocatable_document_hash(
                read_artifact_bytes(record.depfile_path),
                relocation);
        observed_actions.push_back(frontend_action_record{
            .kind = frontend_action_kind::make_dependency_scan,
            .input_role = compilation_input_role::dependency_provider,
            .outcome = dependency_scan_cache_hit
                ? frontend_action_outcome::validated_cache_hit
                : frontend_action_outcome::executed,
            .module_name = record.module_name,
            .source_sha256 = record.source_sha256,
        });
        record.content_key = bioseq::common::sha256_hex(
            content_material + "\ndependencies="
            + record.dependency_sha256);
        record.relocatable_content_key = bioseq::common::sha256_hex(
            relocatable_content_material
            + "\ndependencies="
            + record.relocatable_dependency_sha256);
        const clang_backend::pcm_artifact_paths pcm_artifacts =
            clang_backend::derive_pcm_artifact_paths(
                pcm_root,
                module_name,
                record.relocatable_content_key);
        record.pcm_path = pcm_artifacts.pcm;
        record.pcm_depfile_path = pcm_artifacts.depfile;
        record.pcm_provenance_path =
            pcm_artifacts.provenance;
        for (const std::filesystem::path& dependency :
             record.discovered_dependencies) {
            if (!clang_backend::generated_pcm_dependency(
                    dependency, pcm_root)) {
                dependencies.push_back(dependency);
            }
        }
        record.owned_arguments =
            clang_backend::derive_owned_arguments(
                record.direct_arguments,
                record.source_path,
                pcm_artifacts);

        clang_backend::validate_pcm_artifact_leaves(
            pcm_root, pcm_artifacts);
        bool pcm_cache_hit = false;
        const bool pcm_exists =
            clang_backend::owned_regular_file_exists(
                pcm_root, pcm_artifacts.pcm, "PCM output");
        const bool provenance_exists =
            clang_backend::owned_regular_file_exists(
                pcm_root,
                pcm_artifacts.provenance,
                "PCM provenance output");
        std::error_code size_error;
        const std::uintmax_t existing_size = pcm_exists
            ? std::filesystem::file_size(
                  pcm_artifacts.pcm, size_error)
            : 0;
        if (pcm_exists && provenance_exists) {
            if (size_error || existing_size == 0) {
                throw std::runtime_error(
                    "PREPARE PCM is missing, foreign, or corrupt for module '"
                    + module_name + "'");
            }
            record.pcm_sha256 =
                bioseq::common::sha256_hex(read_artifact_bytes(pcm_artifacts.pcm));
            const std::string expected_provenance =
                clang_backend::render_pcm_provenance(
                    record,
                    clang_binary_sha256,
                    clang_scan_deps_sha256,
                    record.pcm_sha256);
            const std::string persisted_provenance =
                read_artifact_bytes(pcm_artifacts.provenance);
            if (persisted_provenance != expected_provenance) {
                throw std::runtime_error(
                    "PREPARE PCM is missing, foreign, or corrupt for module '"
                    + module_name + "'");
            }
            record.pcm_provenance_sha256 =
                bioseq::common::sha256_hex(persisted_provenance);
            pcm_cache_hit = true;
        }
        if (!pcm_cache_hit) {
            // A tuple with an absent payload or commit marker is recoverable
            // interrupted publication state. A complete tuple whose marker
            // does not authenticate its non-empty PCM was rejected above as
            // corruption. Remove only the exact derived siblings and rebuild
            // the incomplete tuple from an absent state.
            clang_backend::remove_pcm_artifact_tuple(
                pcm_root, pcm_artifacts);
            clang_backend::scoped_pcm_publication publication{
                pcm_artifacts};
            clang_backend::execute_clang(
                clang_binary,
                record.owned_arguments,
                record.command.Directory,
                module_name,
                "PCM compilation");
            clang_backend::validate_pcm_artifact_leaves(
                pcm_root, pcm_artifacts);
            size_error.clear();
            const std::uintmax_t built_size =
                std::filesystem::file_size(
                    pcm_artifacts.pcm, size_error);
            if (size_error || built_size == 0
                || !clang_backend::owned_regular_file_exists(
                    pcm_root,
                    pcm_artifacts.depfile,
                    "PCM dependency output")) {
                throw std::runtime_error(
                    "Clang reported success without fresh PCM and depfile for '"
                    + module_name + "'");
            }
            const std::vector<std::filesystem::path>
                compiled_dependencies =
                clang_backend::parse_make_depfile(
                    pcm_artifacts.depfile,
                    record.command.Directory);
            std::unordered_map<std::string, std::string>
                compiled_dependency_hash_cache;
            std::unordered_map<std::string, std::string>
                compiled_relocatable_dependency_hash_cache;
            const std::string compiled_dependency_sha256 =
                clang_backend::dependency_content_hash(
                    compiled_dependencies,
                    pcm_root,
                    compiled_dependency_hash_cache);
            const std::string
                compiled_relocatable_dependency_sha256 =
                    clang_backend::
                        relocatable_dependency_content_hash(
                            compiled_dependencies,
                            pcm_root,
                            relocation,
                            compiled_relocatable_dependency_hash_cache);
            if (compiled_dependency_sha256
                    != record.dependency_sha256
                || compiled_relocatable_dependency_sha256
                    != record.relocatable_dependency_sha256) {
                throw std::runtime_error(
                    "Clang dependency scan and PCM compilation disagree "
                    "for module '" + module_name
                    + "' (scan="
                    + std::to_string(
                        record.discovered_dependencies.size())
                    + ", compile="
                    + std::to_string(compiled_dependencies.size())
                    + ")");
            }
            record.pcm_sha256 =
                bioseq::common::sha256_hex(read_artifact_bytes(pcm_artifacts.pcm));
            const std::string provenance =
                clang_backend::render_pcm_provenance(
                    record,
                    clang_binary_sha256,
                    clang_scan_deps_sha256,
                    record.pcm_sha256);
            // Transient compiler siblings are not part of the committed v4
            // tuple. The provenance file is the atomic commit marker and is
            // therefore published last.
            clang_backend::remove_owned_leaf_if_present(
                pcm_root,
                pcm_artifacts.depfile,
                "PCM dependency output");
            clang_backend::remove_owned_leaf_if_present(
                pcm_root,
                pcm_artifacts.object,
                "PCM object output");
            clang_backend::atomic_write_owned_file(
                pcm_root,
                pcm_artifacts.provenance,
                provenance,
                "PCM provenance output",
                true);
            record.pcm_provenance_sha256 =
                bioseq::common::sha256_hex(provenance);
            publication.commit();
        } else {
            clang_backend::remove_owned_leaf_if_present(
                pcm_root,
                pcm_artifacts.depfile,
                "PCM dependency output");
            clang_backend::remove_owned_leaf_if_present(
                pcm_root,
                pcm_artifacts.object,
                "PCM object output");
        }
        observed_actions.push_back(frontend_action_record{
            .kind = frontend_action_kind::pcm_compilation,
            .input_role = compilation_input_role::dependency_provider,
            .outcome = pcm_cache_hit
                ? frontend_action_outcome::validated_cache_hit
                : frontend_action_outcome::executed,
            .module_name = record.module_name,
            .source_sha256 = record.source_sha256,
        });
        dependencies.push_back(
            record.pcm_provenance_path);
        dependencies.push_back(record.pcm_path);
    }

    std::ostringstream compile_database_json;
    compile_database_json << "[\n";
    bool first_command = true;
    for (const std::filesystem::path& root : roots) {
        const clang_backend::module_build_record& record =
            modules.at(module_by_source.at(root));
        if (!first_command) compile_database_json << ",\n";
        first_command = false;
        compile_database_json
            << "  {\n    \"directory\": \""
            << clang_backend::json_escape(record.command.Directory)
            << "\",\n    \"file\": \""
            << clang_backend::json_escape(
                   record.source_path.string())
            << "\",\n    \"arguments\": [";
        for (std::size_t index = 0;
             index < record.owned_arguments.size();
             ++index) {
            if (index != 0) compile_database_json << ", ";
            compile_database_json << "\""
                << clang_backend::json_escape(
                    record.owned_arguments[index])
                << "\"";
        }
        compile_database_json << "]\n  }";
    }
    compile_database_json << "\n]\n";
    const std::string owned_compile_database =
        compile_database_json.str();
    const std::string owned_compile_database_sha256 =
        bioseq::common::sha256_hex(owned_compile_database);
    const std::string
        relocatable_owned_compile_database_sha256 =
        clang_backend::relocatable_compiler_document_hash(
            owned_compile_database,
            relocation,
            clang_binary,
            clang_binary_sha256);

    std::vector<std::string> signature_rows;
    signature_rows.reserve(build_order.size());
    for (const std::string& module_name : build_order) {
        const clang_backend::module_build_record& record =
            modules.at(module_name);
        signature_rows.push_back(
            module_name + "\t" + record.content_key + "\t"
            + record.pcm_path.string() + "\t"
            + record.depfile_sha256 + "\t"
            + record.dependency_sha256 + "\t"
            + record.dependency_scan_database_sha256 + "\t"
            + record.dependency_scan_result_sha256 + "\t"
            + record.dependency_scan_diagnostics_sha256 + "\t"
            + record.pcm_sha256 + "\t"
            + record.pcm_provenance_sha256);
    }
    std::ranges::sort(signature_rows);
    std::string signature_material =
        std::string{CLANG_VERSION_STRING} + "\n"
        + clang_binary_sha256 + "\n"
        + clang_scan_deps_version + "\n"
        + clang_scan_deps_sha256 + "\n"
        + toolchain.llvm_config_sha256 + "\n"
        + toolchain.llvm_version + "\n"
        + toolchain.prefix.string() + "\n"
        + resource_directory_sha256 + "\n"
        + toolchain.linked_clang_cpp_sha256 + "\n"
        + toolchain.linked_llvm_library_sha256 + "\n"
        + (compiler_modules
               ? compiler_modules->metadata_sha256
               : std::string{"no-compiler-owned-modules"})
        + "\n"
        + owned_compile_database_sha256 + "\n"
        + std::string{compiler_profile} + "\n"
        + std::string{authority} + "\n";
    for (const std::string& row : signature_rows) {
        signature_material += row + "\n";
    }
    const std::string compilation_signature =
        bioseq::common::sha256_hex(signature_material);
    std::vector<std::string> relocatable_signature_rows;
    relocatable_signature_rows.reserve(build_order.size());
    for (const std::string& module_name : build_order) {
        const clang_backend::module_build_record& record =
            modules.at(module_name);
        std::string row =
            module_name
            + "\tsemantic_source="
            + record.relocatable_source_sha256
            + "\targuments="
            + record.relocatable_argument_sha256
            + "\tcommand_inputs="
            + record.relocatable_command_input_sha256
            + "\tdepfile="
            + record.relocatable_depfile_sha256
            + "\tdependencies="
            + record.relocatable_dependency_sha256
            + "\tscan_database="
            + record.relocatable_dependency_scan_database_sha256
            + "\tscan_result="
            + record.relocatable_dependency_scan_result_sha256
            + "\tscan_diagnostics="
            + record.relocatable_dependency_scan_diagnostics_sha256
            + "\tcontent_key="
            + record.relocatable_content_key;
        for (const std::string& imported :
             record.description.imports) {
            row += "\timport=" + imported;
        }
        relocatable_signature_rows.push_back(std::move(row));
    }
    std::ranges::sort(relocatable_signature_rows);
    std::string relocatable_signature_material =
        "bioseq_clang_relocatable_compilation\n"
        "revision=2\n"
        "clang_version=" + std::string{CLANG_VERSION_STRING}
        + "\nclang_binary_sha256=" + clang_binary_sha256
        + "\nclang_scan_deps_version=" + clang_scan_deps_version
        + "\nclang_scan_deps_sha256=" + clang_scan_deps_sha256
        + "\nllvm_config_sha256=" + toolchain.llvm_config_sha256
        + "\nllvm_version=" + toolchain.llvm_version
        + "\nresource_directory_sha256="
        + resource_directory_sha256
        + "\nlinked_clang_cpp_sha256="
        + toolchain.linked_clang_cpp_sha256
        + "\nlinked_llvm_library_sha256="
        + toolchain.linked_llvm_library_sha256
        + "\ncompiler_module_inventory_sha256="
        + (compiler_modules
               ? compiler_modules->metadata_sha256
               : std::string{"no-compiler-owned-modules"})
        + "\ncompiler_profile=" + std::string{compiler_profile}
        + "\nauthority=" + std::string{authority}
        + "\nowned_compile_database_sha256="
        + relocatable_owned_compile_database_sha256 + "\n";
    for (const std::string& row : relocatable_signature_rows) {
        relocatable_signature_material += row + "\n";
    }
    const std::string relocatable_compilation_signature =
        bioseq::common::sha256_hex(relocatable_signature_material);

    std::ostringstream manifest;
    manifest
        << "{\n  \"schema\": \"bioseq_clang_pcm_manifest\",\n"
        << "  \"schema_revision\": 1,\n"
        << "  \"clang_version\": \""
        << clang_backend::json_escape(CLANG_VERSION_STRING)
        << "\",\n  \"clang_binary\": \""
        << clang_backend::json_escape(
               clang_binary.string())
        << "\",\n  \"clang_binary_sha256\": \""
        << clang_binary_sha256
        << "\",\n  \"clang_scan_deps\": \""
        << clang_backend::json_escape(
               clang_scan_deps.string())
        << "\",\n  \"clang_scan_deps_version\": \""
        << clang_backend::json_escape(
               clang_scan_deps_version)
        << "\",\n  \"clang_scan_deps_sha256\": \""
        << clang_scan_deps_sha256
        << "\",\n  \"llvm_config\": \""
        << clang_backend::json_escape(
               toolchain.llvm_config.string())
        << "\",\n  \"llvm_config_sha256\": \""
        << toolchain.llvm_config_sha256
        << "\",\n  \"llvm_version\": \""
        << clang_backend::json_escape(
               toolchain.llvm_version)
        << "\",\n  \"llvm_prefix\": \""
        << clang_backend::json_escape(
               toolchain.prefix.string())
        << "\",\n  \"clang_resource_dir\": \""
        << clang_backend::json_escape(
               toolchain.resource_dir.string())
        << "\",\n  \"clang_resource_dir_sha256\": \""
        << resource_directory_sha256
        << "\",\n  \"clang_full_version\": \""
        << clang_backend::json_escape(
               toolchain.clang_full_version)
        << "\",\n  \"linked_clang_full_version\": \""
        << clang_backend::json_escape(
               toolchain.linked_clang_full_version)
        << "\",\n  \"linked_clang_cpp\": \""
        << clang_backend::json_escape(
               toolchain.linked_clang_cpp.string())
        << "\",\n  \"linked_clang_cpp_sha256\": \""
        << toolchain.linked_clang_cpp_sha256
        << "\",\n  \"linked_llvm_library\": \""
        << clang_backend::json_escape(
               toolchain.linked_llvm_library.string())
        << "\",\n  \"linked_llvm_library_sha256\": \""
        << toolchain.linked_llvm_library_sha256
        << "\",\n  \"compiler_module_policy\": "
           "\"selected-clang-libc++-std-and-std.compat-only\""
        << ",\n  \"compiler_module_inventory\": \""
        << clang_backend::json_escape(
               compiler_modules
               ? compiler_modules->metadata_path.string()
               : std::string{})
        << "\",\n  \"compiler_module_inventory_sha256\": \""
        << (compiler_modules
               ? compiler_modules->metadata_sha256
               : std::string{})
        << "\",\n  \"relocation_root\": \""
        << clang_backend::json_escape(
               relocation_root.generic_string())
        << "\",\n  \"pcm_root\": \""
        << clang_backend::json_escape(
               std::filesystem::absolute(pcm_root)
                   .lexically_normal().string())
        << "\",\n  \"owned_compile_database_sha256\": \""
        << owned_compile_database_sha256
        << "\",\n  \"relocatable_owned_compile_database_sha256\": \""
        << relocatable_owned_compile_database_sha256
        << "\",\n  \"compiler_profile\": \""
        << clang_backend::json_escape(compiler_profile)
        << "\",\n  \"authority\": \""
        << clang_backend::json_escape(authority)
        << "\",\n  \"compilation_signature\": \""
        << compilation_signature
        << "\",\n  \"relocatable_compilation_signature\": \""
        << relocatable_compilation_signature
        << "\",\n  \"modules\": [\n";
    for (std::size_t index = 0;
         index < build_order.size();
         ++index) {
        const clang_backend::module_build_record& record =
            modules.at(build_order[index]);
        if (index != 0) manifest << ",\n";
        manifest
            << "    {\"name\": \""
            << clang_backend::json_escape(record.module_name)
            << "\", \"source\": \""
            << clang_backend::json_escape(
                   record.source_path.string())
            << "\", \"directory\": \""
            << clang_backend::json_escape(
                   record.command.Directory)
            << "\", \"source_sha256\": \""
            << record.source_sha256
            << "\", \"relocatable_source_sha256\": \""
            << record.relocatable_source_sha256
            << "\", \"arguments_sha256\": \""
            << record.argument_sha256
            << "\", \"relocatable_arguments_sha256\": \""
            << record.relocatable_argument_sha256
            << "\", \"command_input_sha256\": \""
            << record.command_input_sha256
            << "\", \"relocatable_command_input_sha256\": \""
            << record.relocatable_command_input_sha256
            << "\", \"command_inputs\": [";
        for (std::size_t input_index = 0;
             input_index < record.command_inputs.size();
             ++input_index) {
            if (input_index != 0) manifest << ", ";
            const std::filesystem::path& input =
                record.command_inputs[input_index];
            manifest
                << "{\"path\": \""
                << clang_backend::json_escape(
                       input.string())
                << "\", \"sha256\": \""
                << bioseq::common::sha256_hex(read_artifact_bytes(input))
                << "\", \"relocatable_sha256\": \""
                << clang_backend::relocatable_document_hash(
                    read_artifact_bytes(input), relocation)
                << "\"}";
        }
        manifest
            << "], \"base_arguments\": [";
        for (std::size_t argument_index = 0;
             argument_index < record.base_arguments.size();
             ++argument_index) {
            if (argument_index != 0) manifest << ", ";
            manifest
                << "\""
                << clang_backend::json_escape(
                       record.base_arguments[argument_index])
                << "\"";
        }
        manifest
            << "], \"active_import_scan_database\": \""
            << clang_backend::json_escape(
                   record.dependency_scan_database_path.string())
            << "\", \"active_import_scan_database_sha256\": \""
            << record.dependency_scan_database_sha256
            << "\", \"relocatable_active_import_scan_database_sha256\": \""
            << record.relocatable_dependency_scan_database_sha256
            << "\", \"active_import_scan_result\": \""
            << clang_backend::json_escape(
                   record.dependency_scan_result_path.string())
            << "\", \"active_import_scan_result_sha256\": \""
            << record.dependency_scan_result_sha256
            << "\", \"relocatable_active_import_scan_result_sha256\": \""
            << record.relocatable_dependency_scan_result_sha256
            << "\", \"active_import_scan_diagnostics\": \""
            << clang_backend::json_escape(
                   record.dependency_scan_diagnostics_path.string())
            << "\", \"active_import_scan_diagnostics_sha256\": \""
            << record.dependency_scan_diagnostics_sha256
            << "\", \"relocatable_active_import_scan_diagnostics_sha256\": \""
            << record.relocatable_dependency_scan_diagnostics_sha256
            << "\", \"direct_imports\": [";
        for (std::size_t import_index = 0;
             import_index < record.description.imports.size();
             ++import_index) {
            if (import_index != 0) manifest << ", ";
            manifest << "\""
                << clang_backend::json_escape(
                    record.description.imports[import_index])
                << "\"";
        }
        manifest
            << "], \"depfile\": \""
            << clang_backend::json_escape(
                   record.depfile_path.string())
            << "\", \"depfile_sha256\": \""
            << record.depfile_sha256
            << "\", \"relocatable_depfile_sha256\": \""
            << record.relocatable_depfile_sha256
            << "\", \"dependency_content_sha256\": \""
            << record.dependency_sha256
            << "\", \"relocatable_dependency_content_sha256\": \""
            << record.relocatable_dependency_sha256
            << "\", \"content_key\": \""
            << record.content_key
            << "\", \"relocatable_content_key\": \""
            << record.relocatable_content_key
            << "\", \"pcm\": \""
            << clang_backend::json_escape(
                   record.pcm_path.string())
            << "\", \"pcm_sha256\": \""
            << record.pcm_sha256
            << "\", \"pcm_provenance\": \""
            << clang_backend::json_escape(
                   record.pcm_provenance_path.string())
            << "\", \"pcm_provenance_sha256\": \""
            << record.pcm_provenance_sha256
            << "\"}";
    }
    manifest << "\n  ]\n}\n";
    const std::string pcm_manifest_document = manifest.str();

    std::vector<std::filesystem::path> completion_authority_files =
        dependencies;
    for (const std::string& module_name : build_order) {
        const clang_backend::module_build_record& record =
            modules.at(module_name);
        completion_authority_files.push_back(
            record.dependency_scan_database_path);
        completion_authority_files.push_back(
            record.dependency_scan_result_path);
        completion_authority_files.push_back(
            record.dependency_scan_diagnostics_path);
        completion_authority_files.push_back(record.depfile_path);
    }
    std::ranges::sort(completion_authority_files);
    completion_authority_files.erase(
        std::ranges::unique(completion_authority_files).begin(),
        completion_authority_files.end());
    std::ranges::sort(dependencies);
    dependencies.erase(
        std::ranges::unique(dependencies).begin(),
        dependencies.end());
    std::ostringstream dependency_lines;
    for (const std::filesystem::path& dependency : dependencies) {
        dependency_lines << dependency.string() << "\n";
    }
    return {
        .compile_database_json =
            owned_compile_database,
        .pcm_manifest_json = pcm_manifest_document,
        .depfile_text = dependency_lines.str(),
        .compilation_signature = compilation_signature,
        .relocatable_compilation_signature =
            relocatable_compilation_signature,
        .relocatable_compile_database_sha256 =
            relocatable_owned_compile_database_sha256,
        .relocatable_pcm_manifest_sha256 =
            bioseq::common::sha256_hex(
                "bioseq_clang_relocatable_pcm_manifest_projection\n"
                "revision=2\n"
                + relocatable_compilation_signature + "\n"
                + relocatable_owned_compile_database_sha256 + "\n"),
        .dependencies = std::move(dependencies),
        .completion_authority_files =
            std::move(completion_authority_files),
        .compiler_profile = std::string{compiler_profile},
        .authority = std::string{authority},
        .clang_binary_sha256 = clang_binary_sha256,
        .clang_scan_deps_sha256 = clang_scan_deps_sha256,
        .llvm_config_sha256 = toolchain.llvm_config_sha256,
        .compiler_module_inventory_sha256 = compiler_modules
            ? compiler_modules->metadata_sha256
            : std::string{},
        .observed_actions = std::move(observed_actions),
    };
}

[[nodiscard]] dependency_compilation_environment
prepare_compilation_environment(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& raw_compile_database,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    const std::filesystem::path& configured_pcm_root,
    std::string_view compiler_profile,
    std::string_view authority) {
    if (source_paths.empty()) {
        throw std::runtime_error(
            "Clang donor-retarget preparation requires at least one source");
    }

    const std::filesystem::path signature_source =
        clang_backend::absolute_normalized_path(signature_source_path);
    if (!std::filesystem::is_regular_file(signature_source)) {
        throw std::runtime_error(
            "Clang donor-retarget signature source does not exist: "
            + signature_source.string());
    }
    const translation_unit signature = scan_token_slice(signature_source);
    if (!signature.source_module.is_interface
        || signature.source_module.is_partition
        || signature.source_module.provided_module.empty()) {
        throw std::runtime_error(
            "Clang donor-retarget signature source must be an attached "
            "primary interface: " + signature_source.string());
    }

    std::vector<std::filesystem::path> normalized_sources;
    normalized_sources.reserve(source_paths.size());
    for (const std::filesystem::path& requested : source_paths) {
        const std::filesystem::path source =
            clang_backend::absolute_normalized_path(requested);
        if (source == signature_source) {
            throw std::runtime_error(
                "Clang donor-retarget signature source must be distinct "
                "from every prepared source");
        }
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error(
                "Clang donor-retarget source does not exist: "
                + source.string());
        }
        normalized_sources.push_back(source);
    }
    std::ranges::sort(normalized_sources);
    if (std::ranges::adjacent_find(normalized_sources)
        != normalized_sources.end()) {
        throw std::runtime_error(
            "Clang donor-retarget source list contains duplicate identities");
    }
    const std::set<std::filesystem::path> source_identities{
        normalized_sources.begin(), normalized_sources.end()};

    std::unique_ptr<clang::tooling::CompilationDatabase> raw_database =
        clang_backend::load_compile_database(raw_compile_database);
    const std::vector<clang::tooling::CompileCommand> raw_commands =
        raw_database->getAllCompileCommands();
    std::vector<clang::tooling::CompileCommand> donor_commands;
    std::vector<clang::tooling::CompileCommand> synthetic_commands;
    synthetic_commands.reserve(
        raw_commands.size() + normalized_sources.size());
    for (const clang::tooling::CompileCommand& command : raw_commands) {
        const std::filesystem::path command_source =
            clang_backend::absolute_normalized_path(
                clang_backend::absolute_command_path(
                    command.Directory, command.Filename));
        if (command_source == signature_source) {
            donor_commands.push_back(command);
        }
        if (!source_identities.contains(command_source)) {
            synthetic_commands.push_back(command);
        }
    }
    if (donor_commands.empty()) {
        throw std::runtime_error(
            "raw compile database has no command for donor-retarget "
            "signature source: " + signature_source.string());
    }

    std::vector<std::filesystem::path> donor_command_inputs;
    for (const clang::tooling::CompileCommand& donor_command :
         donor_commands) {
        const std::filesystem::path donor_command_source =
            clang_backend::absolute_command_path(
                donor_command.Directory, donor_command.Filename);
        clang::tooling::CompileCommand expanded = donor_command;
        expanded.CommandLine = clang_backend::expanded_command_line(
            donor_command, donor_command_inputs);
        expanded.Output.clear();
        for (const std::filesystem::path& source :
             normalized_sources) {
            clang::tooling::CompileCommand retargeted =
                clang_backend::retarget_compile_command(
                    expanded, donor_command_source, source);
            retargeted.Filename = source.string();
            const bool has_retargeted_source = std::ranges::any_of(
                retargeted.CommandLine,
                [&](const std::string& argument) {
                    return !argument.empty()
                        && argument.front() != '-'
                        && clang_backend::absolute_normalized_path(
                               clang_backend::absolute_command_path(
                                   retargeted.Directory, argument))
                            == source;
                });
            if (!has_retargeted_source) {
                throw std::runtime_error(
                    "donor compile command has no retargetable source "
                    "argument: " + signature_source.string());
            }
            synthetic_commands.push_back(std::move(retargeted));
        }
    }

    const auto command_less =
        [](const clang::tooling::CompileCommand& left,
           const clang::tooling::CompileCommand& right) {
            const auto left_key = std::tie(
                left.Directory,
                left.Filename,
                left.CommandLine);
            const auto right_key = std::tie(
                right.Directory,
                right.Filename,
                right.CommandLine);
            return left_key < right_key;
        };
    std::ranges::sort(synthetic_commands, command_less);
    synthetic_commands.erase(
        std::ranges::unique(
            synthetic_commands,
            [](const clang::tooling::CompileCommand& left,
               const clang::tooling::CompileCommand& right) {
                return left.Directory == right.Directory
                    && left.Filename == right.Filename
                    && left.CommandLine == right.CommandLine;
            }).begin(),
        synthetic_commands.end());

    std::ostringstream synthetic_database_document;
    synthetic_database_document << "[\n";
    for (std::size_t command_index = 0;
         command_index < synthetic_commands.size();
         ++command_index) {
        const clang::tooling::CompileCommand& command =
            synthetic_commands[command_index];
        if (command_index != 0) {
            synthetic_database_document << ",\n";
        }
        synthetic_database_document
            << "  {\n    \"directory\": \""
            << clang_backend::json_escape(command.Directory)
            << "\",\n    \"file\": \""
            << clang_backend::json_escape(command.Filename)
            << "\",\n    \"arguments\": [";
        for (std::size_t argument_index = 0;
             argument_index < command.CommandLine.size();
             ++argument_index) {
            if (argument_index != 0) {
                synthetic_database_document << ", ";
            }
            synthetic_database_document
                << "\""
                << clang_backend::json_escape(
                       command.CommandLine[argument_index])
                << "\"";
        }
        synthetic_database_document << "]\n  }";
    }
    synthetic_database_document << "\n]\n";

    const std::filesystem::path canonical_pcm_root =
        clang_backend::ensure_preparation_root(
            configured_pcm_root);
    const std::filesystem::path synthetic_database_root =
        clang_backend::ensure_owned_directory(
            canonical_pcm_root, ".donor-retarget");
    const std::filesystem::path synthetic_database =
        synthetic_database_root / "compile_commands.json";
    clang_backend::atomic_write_owned_file(
        canonical_pcm_root,
        synthetic_database,
        synthetic_database_document.str(),
        "donor-retarget compile database");

    dependency_compilation_environment prepared =
        prepare_compilation_environment(
            normalized_sources,
            synthetic_database,
            clang_binary,
            clang_scan_deps,
            llvm_config,
            compiler_module_inventory,
            expected_clang_sha256,
            expected_clang_scan_deps_sha256,
            expected_llvm_config_sha256,
            expected_compiler_module_inventory_sha256,
            configured_pcm_root,
            compiler_profile,
            authority);
    prepared.dependencies.push_back(
        clang_backend::absolute_normalized_path(
            raw_compile_database));
    prepared.dependencies.push_back(signature_source);
    prepared.dependencies.insert(
        prepared.dependencies.end(),
        donor_command_inputs.begin(),
        donor_command_inputs.end());
    std::ranges::sort(prepared.dependencies);
    prepared.dependencies.erase(
        std::ranges::unique(prepared.dependencies).begin(),
        prepared.dependencies.end());
    prepared.completion_authority_files.insert(
        prepared.completion_authority_files.end(),
        prepared.dependencies.begin(),
        prepared.dependencies.end());
    std::ranges::sort(prepared.completion_authority_files);
    prepared.completion_authority_files.erase(
        std::ranges::unique(
            prepared.completion_authority_files).begin(),
        prepared.completion_authority_files.end());
    return prepared;
}

namespace {

[[nodiscard]] std::string normalized_dependency_import(
    std::string_view imported,
    std::string_view provided_module,
    const std::filesystem::path& source) {
    std::string result{imported};
    if (result.starts_with(':')) {
        const std::size_t partition = provided_module.find(':');
        result = std::string{provided_module.substr(0, partition)}
            + result;
    }
    if (result.empty()
        || !detail::identifier_begin(result.front())
        || !std::ranges::all_of(
            result,
            [](const char value) {
                return detail::identifier_continue(value)
                    || value == '.' || value == ':';
            })) {
        throw std::runtime_error(
            source.string()
            + ": dependency-only preparation supports named-module "
              "imports only; got '" + result + "'");
    }
    return result;
}

}  // namespace

[[nodiscard]] dependency_compilation_authority
derive_dependency_compilation_authority(
    const std::vector<std::filesystem::path>& canonical_source_paths,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile) {
    if (canonical_source_paths.empty()) {
        throw std::runtime_error(
            "dependency-only preparation requires at least one canonical source");
    }
    if (compiler_profile != "production"
        && compiler_profile != "test_indexes"
        && compiler_profile != "test_support") {
        throw std::runtime_error(
            "dependency-only root derivation received an unknown profile");
    }

    struct canonical_input final {
        std::filesystem::path source;
        std::string source_sha256;
        std::string provided_module;
        std::vector<std::string> imports;
    };

    const std::filesystem::path signature_source =
        clang_backend::absolute_normalized_path(signature_source_path);
    if (!std::filesystem::is_regular_file(signature_source)) {
        throw std::runtime_error(
            "dependency-only signature donor does not exist: "
            + signature_source.string());
    }
    const translation_unit signature = scan_token_slice(signature_source);
    if (!signature.source_module.is_interface
        || signature.source_module.is_partition
        || signature.source_module.provided_module.empty()) {
        throw std::runtime_error(
            "dependency-only signature donor must be an attached primary interface: "
            + signature_source.string());
    }

    std::vector<canonical_input> canonical_inputs;
    canonical_inputs.reserve(canonical_source_paths.size());
    std::map<std::string, std::size_t, std::less<>> canonical_by_module;
    std::set<std::filesystem::path> canonical_identities;
    for (const std::filesystem::path& requested :
         canonical_source_paths) {
        const std::filesystem::path source =
            clang_backend::absolute_normalized_path(requested);
        if (source == signature_source) {
            throw std::runtime_error(
                "canonical source and signature donor must be distinct");
        }
        if (!canonical_identities.insert(source).second) {
            throw std::runtime_error(
                "dependency-only canonical source list contains duplicate identities");
        }
        if (!std::filesystem::is_regular_file(source)) {
            throw std::runtime_error(
                "dependency-only canonical source does not exist: "
                + source.string());
        }
        const translation_unit unit = scan_token_slice(source);
        if (!unit.source_module.is_interface
            || unit.source_module.is_partition
            || unit.source_module.provided_module.empty()) {
            throw std::runtime_error(
                "dependency-only canonical source must be an attached primary interface: "
                + source.string());
        }
        if (unit.source_module.provided_module
            == signature.source_module.provided_module) {
            throw std::runtime_error(
                "dependency-only canonical source and signature donor "
                "provide the same module '"
                + unit.source_module.provided_module + "'");
        }
        canonical_input input{
            .source = source,
            .source_sha256 = unit.source_sha256,
            .provided_module = unit.source_module.provided_module,
        };
        input.imports.reserve(
            unit.source_module.imported_modules.size());
        for (const std::string& imported :
             unit.source_module.imported_modules) {
            input.imports.push_back(normalized_dependency_import(
                imported, input.provided_module, source));
        }
        std::ranges::sort(input.imports);
        input.imports.erase(
            std::ranges::unique(input.imports).begin(),
            input.imports.end());
        const std::size_t index = canonical_inputs.size();
        if (!canonical_by_module.emplace(
                input.provided_module, index).second) {
            throw std::runtime_error(
                "dependency-only canonical sources provide duplicate module '"
                + input.provided_module + "'");
        }
        canonical_inputs.push_back(std::move(input));
    }

    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(compile_database);
    const std::vector<clang::tooling::CompileCommand> commands =
        database->getAllCompileCommands();
    std::set<std::filesystem::path> raw_source_paths;
    for (const clang::tooling::CompileCommand& command : commands) {
        const std::filesystem::path source =
            clang_backend::absolute_normalized_path(
                clang_backend::absolute_command_path(
                    command.Directory, command.Filename));
        if (source.extension() == ".cppm") {
            raw_source_paths.insert(source);
        }
    }

    std::set<std::filesystem::path> selected_source_paths;
    for (const std::filesystem::path& source : raw_source_paths) {
        const std::vector<std::filesystem::path> alternates =
            clang_backend::profile_alternate_paths(source);
        const bool generated_profile_variant =
            !alternates.empty()
            && (std::ranges::any_of(
                    alternates,
                    [&](const std::filesystem::path& alternate) {
                        return raw_source_paths.contains(
                                   clang_backend::absolute_normalized_path(
                                       alternate))
                            || std::filesystem::is_regular_file(alternate);
                    })
                || clang_backend::known_generated_sql_profile_path(source));
        const std::filesystem::path selected = generated_profile_variant
            ? clang_backend::absolute_normalized_path(
                  clang_backend::path_for_profile(
                      source, compiler_profile))
            : source;
        if (std::filesystem::is_regular_file(selected)) {
            selected_source_paths.insert(selected);
        }
    }

    std::map<
        std::string,
        std::set<std::filesystem::path>,
        std::less<>> providers_by_module;
    for (const std::filesystem::path& source : selected_source_paths) {
        const clang_backend::lexical_module_description provider =
            clang_backend::describe_module_provider(source);
        if (!provider.provided_module.empty()) {
            providers_by_module[provider.provided_module].insert(source);
        }
    }
    const auto donor_provider = providers_by_module.find(
        signature.source_module.provided_module);
    if (donor_provider == providers_by_module.end()
        || donor_provider->second.size() != 1
        || *donor_provider->second.begin() != signature_source) {
        throw std::runtime_error(
            "dependency-only signature donor has no unique raw compile-database provider: "
            + signature.source_module.provided_module);
    }

    std::set<std::filesystem::path> roots{signature_source};
    std::set<std::string, std::less<>> visited_canonical_modules;
    std::function<void(std::string_view)> visit_canonical =
        [&](const std::string_view canonical_module) {
            if (!visited_canonical_modules.emplace(
                    canonical_module).second) {
                return;
            }
            const auto current = canonical_by_module.find(
                canonical_module);
            if (current == canonical_by_module.end()) {
                throw std::runtime_error(
                    "dependency-only canonical import graph is corrupt");
            }
            const canonical_input& input =
                canonical_inputs.at(current->second);
            for (const std::string& imported : input.imports) {
                if (const auto peer = canonical_by_module.find(imported);
                    peer != canonical_by_module.end()) {
                    visit_canonical(peer->first);
                    continue;
                }
                const auto providers = providers_by_module.find(imported);
                if (providers == providers_by_module.end()) {
                    throw std::runtime_error(
                        input.source.string()
                        + ": dependency-only import has no raw "
                          "compile-database provider: " + imported);
                }
                if (providers->second.size() != 1) {
                    throw std::runtime_error(
                        input.source.string()
                        + ": dependency-only import has ambiguous raw "
                          "compile-database providers: " + imported);
                }
                roots.insert(*providers->second.begin());
            }
        };
    for (const canonical_input& input : canonical_inputs) {
        visit_canonical(input.provided_module);
    }

    std::ranges::sort(
        canonical_inputs,
        {},
        &canonical_input::provided_module);
    std::string canonical_material =
        "bioseq_dependency_canonical_token_imports\n"
        "revision=1\n";
    for (const canonical_input& input : canonical_inputs) {
        canonical_material +=
            "module=" + input.provided_module
            + "\nsource_sha256=" + input.source_sha256 + "\n";
        for (const std::string& imported : input.imports) {
            canonical_material += "import=" + imported + "\n";
        }
        canonical_material += "--\n";
    }
    std::vector<dependency_module_identity>
        canonical_source_identities;
    canonical_source_identities.reserve(canonical_inputs.size());
    for (const canonical_input& input : canonical_inputs) {
        canonical_source_identities.push_back(
            dependency_module_identity{
                .module_name = input.provided_module,
                .source_path = input.source,
                .source_sha256 = input.source_sha256,
            });
    }
    return dependency_compilation_authority{
        .root_source_paths =
            std::vector<std::filesystem::path>(
                roots.begin(), roots.end()),
        .signature_donor = dependency_module_identity{
            .module_name =
                signature.source_module.provided_module,
            .source_path = signature_source,
            .source_sha256 = signature.source_sha256,
        },
        .canonical_inputs =
            std::move(canonical_source_identities),
        .canonical_token_import_sha256 =
            bioseq::common::sha256_hex(canonical_material),
    };
}

[[nodiscard]] std::string dependency_compilation_manifest_authority(
    std::string_view base_authority,
    const dependency_compilation_authority& authority) {
    if (!base_preparation_authority(base_authority)
        || !lowercase_sha256(
            authority.canonical_token_import_sha256)) {
        throw std::runtime_error(
            "dependency-only manifest authority is incomplete");
    }
    return std::string{base_authority}
        + std::string{dependency_authority_suffix}
        + authority.canonical_token_import_sha256;
}

void validate_dependency_compilation_roots(
    const dependency_compilation_authority& authority,
    const std::filesystem::path& compile_database) {
    if (authority.root_source_paths.empty()
        || authority.signature_donor.module_name.empty()
        || !std::filesystem::is_regular_file(
            authority.signature_donor.source_path)
        || !lowercase_sha256(
            authority.signature_donor.source_sha256)
        || authority.canonical_inputs.empty()
        || std::ranges::any_of(
            authority.canonical_inputs,
            [](const dependency_module_identity& input) {
                return input.module_name.empty()
                    || !std::filesystem::is_regular_file(
                        input.source_path)
                    || !lowercase_sha256(input.source_sha256);
            })
        || !lowercase_sha256(
            authority.canonical_token_import_sha256)) {
        throw std::runtime_error(
            "dependency-only compilation authority is incomplete");
    }
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(compile_database);
    const std::vector<clang::tooling::CompileCommand> all_commands =
        database->getAllCompileCommands();
    if (all_commands.size() != authority.root_source_paths.size()) {
        throw std::runtime_error(
            "dependency-only owned compile database root count is stale");
    }
    std::set<std::filesystem::path> actual_roots;
    for (const clang::tooling::CompileCommand& command : all_commands) {
        const std::filesystem::path source =
            clang_backend::absolute_normalized_path(
                clang_backend::absolute_command_path(
                    command.Directory, command.Filename));
        if (!actual_roots.insert(source).second) {
            throw std::runtime_error(
                "dependency-only owned compile database duplicates a root command");
        }
    }
    const std::set<std::filesystem::path> expected_roots{
        authority.root_source_paths.begin(),
        authority.root_source_paths.end()};
    if (actual_roots != expected_roots) {
        throw std::runtime_error(
            "dependency-only owned compile database root authority is stale");
    }
}

[[nodiscard]] dependency_compilation_environment
prepare_dependency_compilation_environment(
    const std::filesystem::path& canonical_source_path,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& raw_compile_database,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    const std::filesystem::path& pcm_root,
    std::string_view compiler_profile,
    std::string_view authority) {
    return prepare_dependency_compilation_environment(
        std::vector<std::filesystem::path>{canonical_source_path},
        signature_source_path,
        raw_compile_database,
        clang_binary,
        clang_scan_deps,
        llvm_config,
        compiler_module_inventory,
        expected_clang_sha256,
        expected_clang_scan_deps_sha256,
        expected_llvm_config_sha256,
        expected_compiler_module_inventory_sha256,
        pcm_root,
        compiler_profile,
        authority);
}

[[nodiscard]] dependency_compilation_environment
prepare_dependency_compilation_environment(
    const std::vector<std::filesystem::path>& canonical_source_paths,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& raw_compile_database,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    const std::filesystem::path& pcm_root,
    std::string_view compiler_profile,
    std::string_view authority) {
    if (canonical_source_paths.empty()) {
        throw std::runtime_error(
            "dependency-only preparation requires at least one canonical source");
    }

    const std::filesystem::path signature_source =
        clang_backend::absolute_normalized_path(signature_source_path);
    std::vector<std::filesystem::path> canonical_sources;
    canonical_sources.reserve(canonical_source_paths.size());
    for (const std::filesystem::path& requested :
         canonical_source_paths) {
        const std::filesystem::path canonical_source =
            clang_backend::absolute_normalized_path(requested);
        if (canonical_source == signature_source) {
            throw std::runtime_error(
                "canonical source and signature donor must be distinct");
        }
        canonical_sources.push_back(canonical_source);
    }
    std::ranges::sort(canonical_sources);
    if (std::ranges::adjacent_find(canonical_sources)
        != canonical_sources.end()) {
        throw std::runtime_error(
            "dependency-only canonical source list contains duplicate identities");
    }

    const dependency_compilation_authority dependency_authority =
        derive_dependency_compilation_authority(
            canonical_sources,
            signature_source,
            raw_compile_database,
            compiler_profile);
    const std::string manifest_authority =
        dependency_compilation_manifest_authority(
            authority, dependency_authority);

    std::unique_ptr<clang::tooling::CompilationDatabase> raw_database =
        clang_backend::load_compile_database(raw_compile_database);
    std::vector<clang::tooling::CompileCommand> sealed_commands;
    for (clang::tooling::CompileCommand command :
         raw_database->getAllCompileCommands()) {
        const std::filesystem::path command_source =
            clang_backend::absolute_normalized_path(
                clang_backend::absolute_command_path(
                    command.Directory, command.Filename));
        if (std::ranges::binary_search(
                canonical_sources, command_source)) {
            continue;
        }
        sealed_commands.push_back(std::move(command));
    }
    const auto command_less =
        [](const clang::tooling::CompileCommand& left,
           const clang::tooling::CompileCommand& right) {
            return std::tie(
                       left.Directory,
                       left.Filename,
                       left.CommandLine)
                < std::tie(
                       right.Directory,
                       right.Filename,
                       right.CommandLine);
        };
    std::ranges::sort(sealed_commands, command_less);
    sealed_commands.erase(
        std::ranges::unique(
            sealed_commands,
            [](const clang::tooling::CompileCommand& left,
               const clang::tooling::CompileCommand& right) {
                return left.Directory == right.Directory
                    && left.Filename == right.Filename
                    && left.CommandLine == right.CommandLine;
            }).begin(),
        sealed_commands.end());

    std::ostringstream sealed_database_document;
    sealed_database_document << "[\n";
    for (std::size_t command_index = 0;
         command_index < sealed_commands.size();
         ++command_index) {
        const clang::tooling::CompileCommand& command =
            sealed_commands[command_index];
        if (command_index != 0) sealed_database_document << ",\n";
        sealed_database_document
            << "  {\n    \"directory\": \""
            << clang_backend::json_escape(command.Directory)
            << "\",\n    \"file\": \""
            << clang_backend::json_escape(command.Filename)
            << "\",\n    \"arguments\": [";
        for (std::size_t argument_index = 0;
             argument_index < command.CommandLine.size();
             ++argument_index) {
            if (argument_index != 0) sealed_database_document << ", ";
            sealed_database_document
                << "\""
                << clang_backend::json_escape(
                       command.CommandLine[argument_index])
                << "\"";
        }
        sealed_database_document << "]\n  }";
    }
    sealed_database_document << "\n]\n";

    const std::filesystem::path canonical_pcm_root =
        clang_backend::ensure_preparation_root(pcm_root);
    const std::filesystem::path sealed_database_root =
        clang_backend::ensure_owned_directory(
            canonical_pcm_root, ".dependency-only");
    const std::filesystem::path sealed_database =
        sealed_database_root / "compile_commands.json";
    clang_backend::atomic_write_owned_file(
        canonical_pcm_root,
        sealed_database,
        sealed_database_document.str(),
        "dependency-only sealed compile database");

    dependency_compilation_environment prepared =
        prepare_compilation_environment(
            dependency_authority.root_source_paths,
            sealed_database,
            clang_binary,
            clang_scan_deps,
            llvm_config,
            compiler_module_inventory,
            expected_clang_sha256,
            expected_clang_scan_deps_sha256,
            expected_llvm_config_sha256,
            expected_compiler_module_inventory_sha256,
            pcm_root,
            compiler_profile,
            manifest_authority);
    const auto matches_identity =
        [](const frontend_action_record& action,
           const dependency_module_identity& identity) {
            return action.module_name == identity.module_name
                && action.source_sha256 == identity.source_sha256;
        };
    for (frontend_action_record& action :
         prepared.observed_actions) {
        if (std::ranges::any_of(
                dependency_authority.canonical_inputs,
                [&action, &matches_identity](
                    const dependency_module_identity& canonical) {
                    return matches_identity(action, canonical);
                })) {
            throw std::runtime_error(
                "canonical dependency-only input reached a PREPARE "
                "semantic frontend action: " + action.module_name);
        }
        action.input_role = matches_identity(
                action, dependency_authority.signature_donor)
            ? compilation_input_role::signature_donor
            : compilation_input_role::dependency_provider;
    }
    std::vector<frontend_action_record> canonical_actions;
    canonical_actions.reserve(
        dependency_authority.canonical_inputs.size()
        + prepared.observed_actions.size());
    for (const dependency_module_identity& canonical :
         dependency_authority.canonical_inputs) {
        canonical_actions.push_back(frontend_action_record{
            .kind = frontend_action_kind::canonical_token_slice,
            .input_role = compilation_input_role::canonical_scan_only,
            .outcome = frontend_action_outcome::executed,
            .module_name = canonical.module_name,
            .source_sha256 = canonical.source_sha256,
        });
    }
    canonical_actions.insert(
        canonical_actions.end(),
        std::make_move_iterator(
            prepared.observed_actions.begin()),
        std::make_move_iterator(
            prepared.observed_actions.end()));
    prepared.observed_actions = std::move(canonical_actions);
    prepared.dependencies.push_back(
        clang_backend::absolute_normalized_path(
            raw_compile_database));
    prepared.dependencies.insert(
        prepared.dependencies.end(),
        canonical_sources.begin(),
        canonical_sources.end());
    std::ranges::sort(prepared.dependencies);
    prepared.dependencies.erase(
        std::ranges::unique(prepared.dependencies).begin(),
        prepared.dependencies.end());
    prepared.completion_authority_files.insert(
        prepared.completion_authority_files.end(),
        prepared.dependencies.begin(),
        prepared.dependencies.end());
    std::ranges::sort(prepared.completion_authority_files);
    prepared.completion_authority_files.erase(
        std::ranges::unique(
            prepared.completion_authority_files).begin(),
        prepared.completion_authority_files.end());
    std::ostringstream dependency_lines;
    for (const std::filesystem::path& dependency :
         prepared.dependencies) {
        dependency_lines << dependency.string() << "\n";
    }
    prepared.depfile_text = dependency_lines.str();
    prepared.dependency_root_sources =
        dependency_authority.root_source_paths;
    prepared.canonical_token_import_sha256 =
        dependency_authority.canonical_token_import_sha256;
    return prepared;
}

[[nodiscard]] dependency_compilation_validation
validate_dependency_compilation_environment(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    const std::filesystem::path& configured_pcm_root,
    const std::filesystem::path& pcm_manifest,
    const std::filesystem::path& configured_clang_binary,
    const std::filesystem::path& configured_clang_scan_deps,
    const std::filesystem::path& configured_llvm_config,
    const std::filesystem::path&
        configured_compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    std::string_view compiler_profile,
    std::string_view authority) {
    if (source_paths.empty()) {
        throw std::runtime_error(
            "prepared Clang validation requires at least one source");
    }
    const std::filesystem::path pcm_root =
        clang_backend::weakly_canonical_path(
            configured_pcm_root);
    const std::filesystem::path scan_root =
        clang_backend::validate_existing_owned_directory(
            pcm_root, ".module-dependency-scan");
    const std::filesystem::path dependency_root =
        clang_backend::validate_existing_owned_directory(
            pcm_root, ".dependencies");
    const std::filesystem::path relocation_root =
        clang_backend::derive_relocation_root(
            source_paths, compile_database, pcm_root);
    std::unique_ptr<clang::tooling::CompilationDatabase> database =
        clang_backend::load_compile_database(compile_database);
    const clang_backend::relocation_context relocation =
        clang_backend::make_relocation_context(
            relocation_root,
            clang_backend::compilation_root(*database));
    const std::string document = read_artifact_bytes(pcm_manifest);
    llvm::Expected<llvm::json::Value> parsed =
        llvm::json::parse(document);
    if (!parsed) {
        throw std::runtime_error(
            "cannot parse PREPARE PCM manifest: "
            + llvm::toString(parsed.takeError()));
    }
    const llvm::json::Object* root = parsed->getAsObject();
    if (root == nullptr) {
        throw std::runtime_error(
            "PREPARE PCM manifest root is not an object");
    }
    const auto required_string =
        [&](const llvm::json::Object& object,
            std::string_view field) -> std::string {
            const std::optional<llvm::StringRef> value =
                object.getString(field);
            if (!value) {
                throw std::runtime_error(
                    "PREPARE PCM manifest has no string field '"
                    + std::string{field} + "'");
            }
            return value->str();
        };
    const auto require_exact_fields =
        [](const llvm::json::Object& object,
           std::initializer_list<std::string_view> fields,
           std::string_view role) {
            for (const std::string_view field : fields) {
                if (object.get(field) == nullptr) {
                    throw std::runtime_error(
                        "PREPARE " + std::string{role}
                        + " is missing field '" + std::string{field}
                        + "'");
                }
            }
            if (object.size() != fields.size()) {
                throw std::runtime_error(
                    "PREPARE " + std::string{role}
                    + " contains an unknown or duplicate field");
            }
        };
    require_exact_fields(
        *root,
        {
            "schema",
            "schema_revision",
            "clang_version",
            "clang_binary",
            "clang_binary_sha256",
            "clang_scan_deps",
            "clang_scan_deps_version",
            "clang_scan_deps_sha256",
            "llvm_config",
            "llvm_config_sha256",
            "llvm_version",
            "llvm_prefix",
            "clang_resource_dir",
            "clang_resource_dir_sha256",
            "clang_full_version",
            "linked_clang_full_version",
            "linked_clang_cpp",
            "linked_clang_cpp_sha256",
            "linked_llvm_library",
            "linked_llvm_library_sha256",
            "compiler_module_policy",
            "compiler_module_inventory",
            "compiler_module_inventory_sha256",
            "relocation_root",
            "pcm_root",
            "owned_compile_database_sha256",
            "relocatable_owned_compile_database_sha256",
            "compiler_profile",
            "authority",
            "compilation_signature",
            "relocatable_compilation_signature",
            "modules",
        },
        "PCM manifest root");
    const std::optional<std::int64_t> schema_revision =
        root->getInteger("schema_revision");
    if (required_string(*root, "schema")
            != "bioseq_clang_pcm_manifest"
        || !schema_revision
        || *schema_revision != 1
        || required_string(*root, "clang_version")
            != CLANG_VERSION_STRING
        || required_string(*root, "compiler_module_policy")
            != "selected-clang-libc++-std-and-std.compat-only"
        || required_string(*root, "compiler_profile")
            != compiler_profile
        || required_string(*root, "authority")
            != authority) {
        throw std::runtime_error(
            "PREPARE PCM manifest authority, profile, schema, or linked "
            "Clang version differs from generation");
    }
    const std::filesystem::path manifest_pcm_root =
        required_string(*root, "pcm_root");
    if (clang_backend::weakly_canonical_path(manifest_pcm_root)
        != clang_backend::weakly_canonical_path(pcm_root)) {
        throw std::runtime_error(
            "PREPARE PCM manifest root differs from generation root");
    }
    if (
        clang_backend::weakly_canonical_path(
            required_string(*root, "relocation_root"))
        != relocation_root
    ) {
        throw std::runtime_error(
            "PREPARE relocation root differs from generation inputs");
    }
    const auto require_configured_path =
        [&](std::string_view field,
            const std::filesystem::path& configured) {
            const std::filesystem::path sealed =
                required_string(*root, field);
            if (clang_backend::weakly_canonical_path(sealed)
                != clang_backend::weakly_canonical_path(
                    configured)) {
                throw std::runtime_error(
                    "PREPARE PCM manifest " + std::string{field}
                    + " path differs from configured generation "
                      "authority");
            }
        };
    require_configured_path(
        "clang_binary", configured_clang_binary);
    require_configured_path(
        "clang_scan_deps", configured_clang_scan_deps);
    require_configured_path(
        "llvm_config", configured_llvm_config);
    require_configured_path(
        "compiler_module_inventory",
        configured_compiler_module_inventory);
    const auto require_configured_hash =
        [](const std::filesystem::path& path,
           std::string_view expected,
           std::string_view role) {
            if (expected.size() != 64
                || bioseq::common::sha256_hex(read_artifact_bytes(path)) != expected) {
                throw std::runtime_error(
                    std::string{role}
                    + " differs from its configure-time SHA-256");
            }
        };
    require_configured_hash(
        configured_clang_binary,
        expected_clang_sha256,
        "configured Clang");
    require_configured_hash(
        configured_clang_scan_deps,
        expected_clang_scan_deps_sha256,
        "configured clang-scan-deps");
    require_configured_hash(
        configured_llvm_config,
        expected_llvm_config_sha256,
        "configured llvm-config");
    require_configured_hash(
        configured_compiler_module_inventory,
        expected_compiler_module_inventory_sha256,
        "configured compiler-module inventory");
    const clang_backend::selected_toolchain_identity
        toolchain = clang_backend::validate_selected_toolchain(
            configured_clang_binary,
            configured_clang_scan_deps,
            configured_llvm_config,
            configured_compiler_module_inventory);
    const std::filesystem::path& clang_binary =
        configured_clang_binary;
    const std::string clang_binary_sha256 =
        bioseq::common::sha256_hex(read_artifact_bytes(clang_binary));
    if (clang_binary_sha256
        != required_string(*root, "clang_binary_sha256")) {
        throw std::runtime_error(
            "PREPARE PCM manifest Clang binary hash is stale");
    }
    const std::filesystem::path& clang_scan_deps =
        configured_clang_scan_deps;
    const std::string& clang_scan_deps_version =
        toolchain.clang_scan_deps_full_version;
    const std::string clang_scan_deps_sha256 =
        bioseq::common::sha256_hex(read_artifact_bytes(clang_scan_deps));
    if (clang_scan_deps_version
            != required_string(
                *root, "clang_scan_deps_version")
        || clang_scan_deps_sha256
            != required_string(
                *root, "clang_scan_deps_sha256")) {
        throw std::runtime_error(
            "PREPARE PCM manifest clang-scan-deps identity is stale");
    }
    std::vector<std::filesystem::path> resource_files;
    const std::string resource_directory_sha256 =
        clang_backend::directory_content_hash(
            toolchain.resource_dir, resource_files);
    if (required_string(*root, "llvm_config_sha256")
            != toolchain.llvm_config_sha256
        || required_string(*root, "llvm_version")
            != toolchain.llvm_version
        || clang_backend::weakly_canonical_path(
               required_string(*root, "llvm_prefix"))
            != toolchain.prefix
        || clang_backend::weakly_canonical_path(
               required_string(*root, "clang_resource_dir"))
            != toolchain.resource_dir
        || required_string(
               *root, "clang_resource_dir_sha256")
            != resource_directory_sha256
        || required_string(*root, "clang_full_version")
            != toolchain.clang_full_version
        || required_string(
               *root, "linked_clang_full_version")
            != toolchain.linked_clang_full_version
        || clang_backend::weakly_canonical_path(
               required_string(*root, "linked_clang_cpp"))
            != toolchain.linked_clang_cpp
        || required_string(
               *root, "linked_clang_cpp_sha256")
            != toolchain.linked_clang_cpp_sha256
        || clang_backend::weakly_canonical_path(
               required_string(*root, "linked_llvm_library"))
            != toolchain.linked_llvm_library
        || required_string(
               *root, "linked_llvm_library_sha256")
            != toolchain.linked_llvm_library_sha256) {
        throw std::runtime_error(
            "PREPARE PCM manifest toolchain build/resource identity "
            "is stale");
    }
    const std::string compile_database_document =
        read_artifact_bytes(compile_database);
    const std::string compile_database_sha256 =
        bioseq::common::sha256_hex(compile_database_document);
    const std::string relocatable_compile_database_sha256 =
        clang_backend::relocatable_compiler_document_hash(
            compile_database_document,
            relocation,
            required_string(*root, "clang_binary"),
            clang_binary_sha256);
    if (compile_database_sha256
        != required_string(
            *root, "owned_compile_database_sha256")
        || relocatable_compile_database_sha256
            != required_string(
                *root,
                "relocatable_owned_compile_database_sha256")) {
        throw std::runtime_error(
            "owned compile database differs from PREPARE manifest");
    }
    const std::string compiler_inventory_path =
        configured_compiler_module_inventory.string();
    const std::string compiler_inventory_sha256 =
        required_string(
            *root, "compiler_module_inventory_sha256");
    if (compiler_inventory_path.empty()
        != compiler_inventory_sha256.empty()) {
        throw std::runtime_error(
            "PREPARE compiler-module inventory seal is incomplete");
    }
    if (!compiler_inventory_path.empty()
        && bioseq::common::sha256_hex(read_artifact_bytes(compiler_inventory_path))
            != compiler_inventory_sha256) {
        throw std::runtime_error(
            "selected Clang compiler-module inventory is stale");
    }

    struct sealed_module {
        std::string name;
        std::filesystem::path source;
        std::filesystem::path source_spelling;
        std::filesystem::path directory;
        std::string source_sha256;
        std::string relocatable_source_sha256;
        std::string arguments_sha256;
        std::string relocatable_arguments_sha256;
        std::string command_input_sha256;
        std::string relocatable_command_input_sha256;
        std::vector<std::filesystem::path> command_inputs;
        std::vector<std::string> base_arguments;
        std::filesystem::path scan_database;
        std::filesystem::path scan_database_spelling;
        std::string scan_database_sha256;
        std::string relocatable_scan_database_sha256;
        std::filesystem::path scan_result;
        std::filesystem::path scan_result_spelling;
        std::string scan_result_sha256;
        std::string relocatable_scan_result_sha256;
        std::filesystem::path scan_diagnostics;
        std::filesystem::path scan_diagnostics_spelling;
        std::string scan_diagnostics_sha256;
        std::string relocatable_scan_diagnostics_sha256;
        std::vector<std::string> imports;
        std::filesystem::path depfile;
        std::filesystem::path depfile_spelling;
        std::string depfile_sha256;
        std::string relocatable_depfile_sha256;
        std::string dependency_sha256;
        std::string relocatable_dependency_sha256;
        std::string content_key;
        std::string relocatable_content_key;
        std::filesystem::path pcm;
        std::filesystem::path pcm_spelling;
        std::string pcm_sha256;
        std::filesystem::path pcm_provenance;
        std::filesystem::path pcm_provenance_spelling;
        std::string pcm_provenance_sha256;
    };
    const llvm::json::Array* module_values =
        root->getArray("modules");
    if (module_values == nullptr || module_values->empty()) {
        throw std::runtime_error(
            "PREPARE PCM manifest has no module closure");
    }
    std::map<std::string, sealed_module, std::less<>> modules;
    std::map<std::filesystem::path, std::string> module_by_source;
    std::set<std::filesystem::path> pcm_paths;
    for (const llvm::json::Value& module_value :
         *module_values) {
        const llvm::json::Object* object =
            module_value.getAsObject();
        if (object == nullptr) {
            throw std::runtime_error(
                "PREPARE PCM manifest module is not an object");
        }
        require_exact_fields(
            *object,
            {
                "name",
                "source",
                "directory",
                "source_sha256",
                "relocatable_source_sha256",
                "arguments_sha256",
                "relocatable_arguments_sha256",
                "command_input_sha256",
                "relocatable_command_input_sha256",
                "command_inputs",
                "base_arguments",
                "active_import_scan_database",
                "active_import_scan_database_sha256",
                "relocatable_active_import_scan_database_sha256",
                "active_import_scan_result",
                "active_import_scan_result_sha256",
                "relocatable_active_import_scan_result_sha256",
                "active_import_scan_diagnostics",
                "active_import_scan_diagnostics_sha256",
                "relocatable_active_import_scan_diagnostics_sha256",
                "direct_imports",
                "depfile",
                "depfile_sha256",
                "relocatable_depfile_sha256",
                "dependency_content_sha256",
                "relocatable_dependency_content_sha256",
                "content_key",
                "relocatable_content_key",
                "pcm",
                "pcm_sha256",
                "pcm_provenance",
                "pcm_provenance_sha256",
            },
            "PCM manifest module");
        sealed_module module{
            .name = required_string(*object, "name"),
            .source = required_string(*object, "source"),
            .directory = required_string(*object, "directory"),
            .source_sha256 =
                required_string(*object, "source_sha256"),
            .relocatable_source_sha256 = required_string(
                *object, "relocatable_source_sha256"),
            .arguments_sha256 =
                required_string(*object, "arguments_sha256"),
            .relocatable_arguments_sha256 = required_string(
                *object, "relocatable_arguments_sha256"),
            .command_input_sha256 = required_string(
                *object, "command_input_sha256"),
            .relocatable_command_input_sha256 = required_string(
                *object, "relocatable_command_input_sha256"),
            .scan_database = required_string(
                *object, "active_import_scan_database"),
            .scan_database_sha256 = required_string(
                *object,
                "active_import_scan_database_sha256"),
            .relocatable_scan_database_sha256 = required_string(
                *object,
                "relocatable_active_import_scan_database_sha256"),
            .scan_result = required_string(
                *object, "active_import_scan_result"),
            .scan_result_sha256 = required_string(
                *object,
                "active_import_scan_result_sha256"),
            .relocatable_scan_result_sha256 = required_string(
                *object,
                "relocatable_active_import_scan_result_sha256"),
            .scan_diagnostics = required_string(
                *object, "active_import_scan_diagnostics"),
            .scan_diagnostics_sha256 = required_string(
                *object,
                "active_import_scan_diagnostics_sha256"),
            .relocatable_scan_diagnostics_sha256 = required_string(
                *object,
                "relocatable_active_import_scan_diagnostics_sha256"),
            .depfile = required_string(*object, "depfile"),
            .depfile_sha256 =
                required_string(*object, "depfile_sha256"),
            .relocatable_depfile_sha256 = required_string(
                *object, "relocatable_depfile_sha256"),
            .dependency_sha256 = required_string(
                *object, "dependency_content_sha256"),
            .relocatable_dependency_sha256 = required_string(
                *object,
                "relocatable_dependency_content_sha256"),
            .content_key =
                required_string(*object, "content_key"),
            .relocatable_content_key =
                required_string(*object, "relocatable_content_key"),
            .pcm = required_string(*object, "pcm"),
            .pcm_sha256 =
                required_string(*object, "pcm_sha256"),
            .pcm_provenance =
                required_string(*object, "pcm_provenance"),
            .pcm_provenance_sha256 = required_string(
                *object, "pcm_provenance_sha256"),
        };
        module.source_spelling = module.source;
        module.scan_database_spelling = module.scan_database;
        module.scan_result_spelling = module.scan_result;
        module.scan_diagnostics_spelling =
            module.scan_diagnostics;
        module.depfile_spelling = module.depfile;
        module.pcm_spelling = module.pcm;
        module.pcm_provenance_spelling =
            module.pcm_provenance;
        const llvm::json::Array* command_inputs =
            object->getArray("command_inputs");
        if (command_inputs == nullptr) {
            throw std::runtime_error(
                "PREPARE PCM manifest module has no command-input "
                "closure");
        }
        for (const llvm::json::Value& input_value :
             *command_inputs) {
            const llvm::json::Object* input =
                input_value.getAsObject();
            if (input == nullptr) {
                throw std::runtime_error(
                    "PREPARE command-input record is not an object");
            }
            require_exact_fields(
                *input,
                {"path", "sha256", "relocatable_sha256"},
                "command-input record");
            const std::filesystem::path path =
                clang_backend::weakly_canonical_path(
                    required_string(*input, "path"));
            if (!std::filesystem::is_regular_file(path)
                || bioseq::common::sha256_hex(read_artifact_bytes(path))
                    != required_string(*input, "sha256")
                || clang_backend::relocatable_document_hash(
                       read_artifact_bytes(path), relocation)
                    != required_string(
                        *input, "relocatable_sha256")) {
                throw std::runtime_error(
                    "PREPARE response-file input is missing or stale");
            }
            module.command_inputs.push_back(path);
        }
        if (!std::ranges::is_sorted(module.command_inputs)
            || std::ranges::adjacent_find(
                   module.command_inputs)
                != module.command_inputs.end()
            || clang_backend::command_input_hash(
                   module.command_inputs)
                != module.command_input_sha256
            || clang_backend::relocatable_command_input_hash(
                   module.command_inputs,
                   relocation)
                != module.relocatable_command_input_sha256) {
            throw std::runtime_error(
                "PREPARE command-input closure is not canonical or "
                "does not match its hash");
        }
        const llvm::json::Array* base_arguments =
            object->getArray("base_arguments");
        if (base_arguments == nullptr || base_arguments->empty()) {
            throw std::runtime_error(
                "PREPARE PCM manifest module has no base arguments");
        }
        std::string argument_material;
        for (const llvm::json::Value& argument_value :
             *base_arguments) {
            const std::optional<llvm::StringRef> argument =
                argument_value.getAsString();
            if (!argument) {
                throw std::runtime_error(
                    "PREPARE base-argument record is not a string");
            }
            module.base_arguments.push_back(argument->str());
            argument_material += argument->str();
            argument_material.push_back('\0');
        }
        if (clang_backend::weakly_canonical_path(
                module.base_arguments.front())
                != clang_backend::weakly_canonical_path(
                    clang_binary)
            || bioseq::common::sha256_hex(argument_material)
                != module.arguments_sha256
            || clang_backend::relocatable_argument_hash(
                   module.base_arguments,
                   relocation,
                   clang_binary_sha256)
                != module.relocatable_arguments_sha256) {
            throw std::runtime_error(
                "PREPARE base-argument closure does not match its "
                "selected compiler or physical and relocatable hashes");
        }
        const llvm::json::Array* imports =
            object->getArray("direct_imports");
        if (imports == nullptr) {
            throw std::runtime_error(
                "PREPARE PCM manifest module has no direct imports");
        }
        for (const llvm::json::Value& import_value :
             *imports) {
            const std::optional<llvm::StringRef> imported =
                import_value.getAsString();
            if (!imported || imported->empty()) {
                throw std::runtime_error(
                    "PREPARE PCM manifest contains an invalid direct "
                    "import");
            }
            module.imports.push_back(imported->str());
        }
        if (!std::ranges::is_sorted(module.imports)
            || std::ranges::adjacent_find(module.imports)
                != module.imports.end()) {
            throw std::runtime_error(
                "PREPARE PCM manifest direct imports are not a unique "
                "canonical set for module '" + module.name + "'");
        }
        module.source =
            clang_backend::weakly_canonical_path(module.source);
        module.depfile =
            clang_backend::weakly_canonical_path(module.depfile);
        module.pcm =
            clang_backend::weakly_canonical_path(module.pcm);
        module.pcm_provenance =
            clang_backend::weakly_canonical_path(
                module.pcm_provenance);
        if (!std::filesystem::is_regular_file(module.source)
            || bioseq::common::sha256_hex(read_artifact_bytes(module.source))
                != module.source_sha256
            || clang_backend::relocatable_document_hash(
                   read_artifact_bytes(module.source),
                   relocation)
                != module.relocatable_source_sha256) {
            throw std::runtime_error(
                "PREPARE source hash is stale for module '"
                + module.name + "'");
        }
        module.scan_database =
            clang_backend::weakly_canonical_path(
                module.scan_database);
        module.scan_result =
            clang_backend::weakly_canonical_path(
                module.scan_result);
        module.scan_diagnostics =
            clang_backend::weakly_canonical_path(
                module.scan_diagnostics);
        const clang_backend::active_import_artifact_paths
            expected_scan_artifacts =
            clang_backend::derive_active_import_artifact_paths(
                scan_root,
                module.name,
                module.source,
                module.base_arguments,
                relocation,
                clang_binary_sha256,
                clang_scan_deps_sha256);
        if (module.scan_database
                != expected_scan_artifacts.database
            || module.scan_result != expected_scan_artifacts.result
            || module.scan_diagnostics
                != expected_scan_artifacts.diagnostics
            || module.scan_database_spelling.lexically_normal()
                != expected_scan_artifacts.database
            || module.scan_result_spelling.lexically_normal()
                != expected_scan_artifacts.result
            || module.scan_diagnostics_spelling.lexically_normal()
                != expected_scan_artifacts.diagnostics
            || !std::filesystem::is_regular_file(
                module.scan_database)
            || !std::filesystem::is_regular_file(
                module.scan_result)
            || !std::filesystem::is_regular_file(
                module.scan_diagnostics)) {
            throw std::runtime_error(
                "PREPARE active-import scan artifacts are missing or "
                "have foreign identities for module '"
                + module.name + "'");
        }
        const std::string scan_database_document =
            read_artifact_bytes(module.scan_database);
        const std::string scan_result_document =
            read_artifact_bytes(module.scan_result);
        const std::string scan_diagnostics_document =
            read_artifact_bytes(module.scan_diagnostics);
        if (bioseq::common::sha256_hex(scan_database_document)
                != module.scan_database_sha256
            || clang_backend::
                   relocatable_compiler_document_hash(
                       scan_database_document,
                       relocation,
                       module.base_arguments.front(),
                       clang_binary_sha256)
                != module.relocatable_scan_database_sha256
            || bioseq::common::sha256_hex(scan_result_document)
                != module.scan_result_sha256
            || clang_backend::relocatable_document_hash(
                   scan_result_document, relocation)
                != module.relocatable_scan_result_sha256
            || bioseq::common::sha256_hex(scan_diagnostics_document)
                != module.scan_diagnostics_sha256
            || clang_backend::relocatable_document_hash(
                   scan_diagnostics_document, relocation)
                != module.relocatable_scan_diagnostics_sha256) {
            throw std::runtime_error(
                "PREPARE active-import scan artifact hash is stale for "
                "module '" + module.name + "'");
        }
        std::ostringstream expected_scan_database_document;
        expected_scan_database_document
            << "[\n  {\n    \"directory\": \""
            << clang_backend::json_escape(
                   module.directory.string())
            << "\",\n    \"file\": \""
            << clang_backend::json_escape(
                   module.source_spelling.string())
            << "\",\n    \"arguments\": [";
        std::vector<std::string> expected_scan_arguments =
            module.base_arguments;
        expected_scan_arguments.emplace_back("-c");
        expected_scan_arguments.push_back(
            module.source_spelling.string());
        expected_scan_arguments.emplace_back("-o");
        expected_scan_arguments.push_back(
            expected_scan_artifacts.object.string());
        for (std::size_t argument_index = 0;
             argument_index < expected_scan_arguments.size();
             ++argument_index) {
            if (argument_index != 0) {
                expected_scan_database_document << ", ";
            }
            expected_scan_database_document
                << "\""
                << clang_backend::json_escape(
                       expected_scan_arguments[argument_index])
                << "\"";
        }
        expected_scan_database_document << "]\n  }\n]\n";
        if (scan_database_document
            != expected_scan_database_document.str()) {
            throw std::runtime_error(
                "PREPARE active-import scan database does not match "
                "its sealed base arguments for module '"
                + module.name + "'");
        }
        if (clang_backend::parse_p1689_imports(
                module.scan_result,
                module.name,
                module.source,
                module.directory)
            != module.imports) {
            throw std::runtime_error(
                "PREPARE active-import scan result differs from the "
                "sealed direct-import graph for module '"
                + module.name + "'");
        }
        if (!module_by_source.emplace(
                module.source, module.name).second
            || !pcm_paths.insert(module.pcm).second
            || !modules.emplace(module.name, module).second) {
            throw std::runtime_error(
                "PREPARE PCM manifest duplicates a module, source, or "
                "PCM path");
        }
    }
    for (const auto& [name, module] : modules) {
        for (const std::string& imported : module.imports) {
            if (!modules.contains(imported)) {
                throw std::runtime_error(
                    "PREPARE manifest import has no sealed provider: "
                    + imported);
            }
        }
        if (!clang_backend::path_within_root(
                module.pcm_provenance, pcm_root)) {
            throw std::runtime_error(
                "PREPARE PCM provenance is missing or foreign for "
                "module '" + name + "'");
        }
        if (!clang_backend::path_within_root(
                module.depfile, pcm_root)) {
            throw std::runtime_error(
                "PREPARE depfile is missing, foreign, or stale for "
                "module '" + name + "'");
        }
        if (!clang_backend::path_within_root(
                module.pcm, pcm_root)) {
            throw std::runtime_error(
                "PREPARE PCM is missing, foreign, or corrupt for module '"
                + name + "'");
        }
    }

    std::map<std::string, int, std::less<>> validation_state;
    std::function<void(const std::string&)> validate_module_artifacts =
        [&](const std::string& name) {
            if (validation_state[name] == 2) {
                return;
            }
            if (validation_state[name] == 1) {
                throw std::runtime_error(
                    "PREPARE PCM manifest contains a module cycle at '"
                    + name + "'");
            }
            validation_state[name] = 1;
            const sealed_module& module = modules.at(name);
            for (const std::string& imported : module.imports) {
                validate_module_artifacts(imported);
            }
            std::vector<std::pair<std::string, std::string>>
                physical_import_keys;
            std::vector<std::pair<std::string, std::string>>
                relocatable_import_keys;
            for (const std::string& imported : module.imports) {
                physical_import_keys.emplace_back(
                    imported, modules.at(imported).content_key);
                relocatable_import_keys.emplace_back(
                    imported,
                    modules.at(imported).relocatable_content_key);
            }
            const std::string physical_material =
                clang_backend::predependency_material(
                    compiler_profile,
                    module.name,
                    module.source_sha256,
                    module.arguments_sha256,
                    module.command_input_sha256,
                    module.scan_database_sha256,
                    module.scan_result_sha256,
                    module.scan_diagnostics_sha256,
                    clang_binary_sha256,
                    clang_scan_deps_sha256,
                    physical_import_keys);
            const std::string relocatable_material =
                clang_backend::predependency_material(
                    compiler_profile,
                    module.name,
                    module.relocatable_source_sha256,
                    module.relocatable_arguments_sha256,
                    module.relocatable_command_input_sha256,
                    module.relocatable_scan_database_sha256,
                    module.relocatable_scan_result_sha256,
                    module.relocatable_scan_diagnostics_sha256,
                    clang_binary_sha256,
                    clang_scan_deps_sha256,
                    relocatable_import_keys);
            if (bioseq::common::sha256_hex(
                    physical_material + "\ndependencies="
                    + module.dependency_sha256)
                    != module.content_key
                || bioseq::common::sha256_hex(
                       relocatable_material + "\ndependencies="
                       + module.relocatable_dependency_sha256)
                    != module.relocatable_content_key) {
                throw std::runtime_error(
                    "PREPARE PCM content key is stale for module '"
                    + name + "'");
            }
            const std::filesystem::path expected_depfile =
                clang_backend::derive_make_depfile_path(
                    dependency_root,
                    module.name,
                    relocatable_material);
            const clang_backend::pcm_artifact_paths
                expected_pcm_artifacts =
                clang_backend::derive_pcm_artifact_paths(
                    pcm_root,
                    module.name,
                    module.relocatable_content_key);
            if (module.depfile_spelling.lexically_normal()
                    != expected_depfile
                || module.depfile != expected_depfile
                || module.pcm_spelling.lexically_normal()
                    != expected_pcm_artifacts.pcm
                || module.pcm != expected_pcm_artifacts.pcm
                || module.pcm_provenance_spelling.lexically_normal()
                    != expected_pcm_artifacts.provenance
                || module.pcm_provenance
                    != expected_pcm_artifacts.provenance) {
                throw std::runtime_error(
                    "PREPARE artifact spelling differs from its exact "
                    "content-derived path for module '" + name + "'");
            }
            validation_state[name] = 2;
        };
    for (const auto& entry : modules) {
        validate_module_artifacts(entry.first);
    }

    for (const auto& [name, module] : modules) {
        if (!clang_backend::path_within_root(
                module.pcm_provenance, pcm_root)
            || !std::filesystem::is_regular_file(
                module.pcm_provenance)) {
            throw std::runtime_error(
                "PREPARE PCM provenance is missing or foreign for "
                "module '" + name + "'");
        }
        const std::string provenance =
            read_artifact_bytes(module.pcm_provenance);
        const std::string expected_provenance =
            "bioseq_clang_pcm_provenance\n"
            "revision=1\n"
            "module=" + module.name + "\n"
            "content_key=" + module.content_key + "\n"
            "source_sha256=" + module.source_sha256 + "\n"
            "arguments_sha256=" + module.arguments_sha256 + "\n"
            "command_input_sha256="
            + module.command_input_sha256 + "\n"
            "dependency_content_sha256="
            + module.dependency_sha256 + "\n"
            "clang_binary_sha256=" + clang_binary_sha256 + "\n"
            "clang_scan_deps_sha256="
            + clang_scan_deps_sha256 + "\n"
            "pcm_sha256=" + module.pcm_sha256 + "\n";
        if (provenance != expected_provenance
            || bioseq::common::sha256_hex(provenance)
                != module.pcm_provenance_sha256) {
            throw std::runtime_error(
                "PREPARE PCM provenance is corrupt or stale for module '"
                + name + "'");
        }
        if (!clang_backend::path_within_root(
                module.depfile, pcm_root)
            || !std::filesystem::is_regular_file(module.depfile)
            || bioseq::common::sha256_hex(read_artifact_bytes(module.depfile))
                != module.depfile_sha256
            || clang_backend::relocatable_document_hash(
                   read_artifact_bytes(module.depfile),
                   relocation)
                != module.relocatable_depfile_sha256) {
            throw std::runtime_error(
                "PREPARE depfile is missing, foreign, or stale for "
                "module '" + name + "'");
        }
        if (!clang_backend::path_within_root(
                module.pcm, pcm_root)
            || !std::filesystem::is_regular_file(module.pcm)
            || bioseq::common::sha256_hex(read_artifact_bytes(module.pcm))
                != module.pcm_sha256) {
            throw std::runtime_error(
                "PREPARE PCM is missing, foreign, or corrupt for module '"
                + name + "'");
        }
    }

    std::unordered_map<std::string, std::string>
        dependency_hash_cache;
    std::unordered_map<std::string, std::string>
        relocatable_dependency_hash_cache;
    for (const auto& [name, module] : modules) {
        const std::vector<std::filesystem::path> dependencies =
            clang_backend::parse_make_depfile(
                module.depfile,
                module.directory);
        if (clang_backend::dependency_content_hash(
                dependencies,
                pcm_root,
                dependency_hash_cache)
                != module.dependency_sha256
            || clang_backend::relocatable_dependency_content_hash(
                   dependencies,
                   pcm_root,
                   relocation,
                   relocatable_dependency_hash_cache)
                != module.relocatable_dependency_sha256) {
            throw std::runtime_error(
                "PREPARE header/source dependency closure is stale for "
                "module '" + name + "'");
        }
    }

    std::set<std::string, std::less<>> reached;
    std::function<void(const std::string&)> collect_reached =
        [&](const std::string& name) {
            if (!reached.insert(name).second) {
                return;
            }
            for (const std::string& imported :
                 modules.at(name).imports) {
                collect_reached(imported);
            }
        };

    std::vector<std::filesystem::path> normalized_sources;
    normalized_sources.reserve(source_paths.size());
    for (const std::filesystem::path& source : source_paths) {
        const std::filesystem::path normalized =
            clang_backend::weakly_canonical_path(
                source.is_absolute()
                ? source
                : std::filesystem::absolute(source));
        if (!std::filesystem::is_regular_file(normalized)
            || !module_by_source.contains(normalized)) {
            throw std::runtime_error(
                "generation source is absent from PREPARE manifest: "
                + normalized.string());
        }
        normalized_sources.push_back(normalized);
    }
    std::ranges::sort(normalized_sources);
    if (std::ranges::adjacent_find(normalized_sources)
        != normalized_sources.end()) {
        throw std::runtime_error(
            "generation source list contains duplicate identities");
    }
    for (const std::filesystem::path& source :
         normalized_sources) {
        const std::string& module_name =
            module_by_source.at(source);
        collect_reached(module_name);
        const sealed_module& module =
            modules.at(module_name);
        const std::vector<clang::tooling::CompileCommand> commands =
            database->getCompileCommands(source.string());
        if (commands.size() != 1) {
            throw std::runtime_error(
                "owned compile database must contain exactly one root "
                "command for " + source.string());
        }
        const clang::tooling::CompileCommand& command =
            commands.front();
        std::map<std::string, std::filesystem::path, std::less<>>
            mappings;
        std::optional<std::filesystem::path> output;
        std::optional<std::filesystem::path> resource_dir;
        for (std::size_t argument_index = 0;
             argument_index < command.CommandLine.size();
             ++argument_index) {
            const std::string& argument =
                command.CommandLine[argument_index];
            if (argument.starts_with("-fmodule-file=")) {
                const std::string_view mapping =
                    std::string_view{argument}.substr(
                        std::string_view{"-fmodule-file="}.size());
                const std::size_t separator = mapping.find('=');
                if (separator == std::string_view::npos
                    || separator == 0
                    || separator + 1 == mapping.size()) {
                    throw std::runtime_error(
                        "owned compile database contains a malformed "
                        "module mapping");
                }
                if (!mappings.emplace(
                        std::string{mapping.substr(0, separator)},
                        clang_backend::weakly_canonical_path(
                            std::filesystem::path{
                                std::string{
                                    mapping.substr(
                                        separator + 1)}}))
                         .second) {
                    throw std::runtime_error(
                        "owned compile database duplicates a module "
                        "mapping");
                }
            } else if (argument.starts_with("-fmodule-output=")) {
                if (output) {
                    throw std::runtime_error(
                        "owned compile database duplicates its PCM "
                        "output");
                }
                output = clang_backend::weakly_canonical_path(
                    std::filesystem::path{
                        argument.substr(
                            std::string{"-fmodule-output="}.size())});
            } else if (argument == "-resource-dir") {
                if (resource_dir
                    || argument_index + 1
                        >= command.CommandLine.size()) {
                    throw std::runtime_error(
                        "owned compile database contains a duplicate or "
                        "malformed Clang resource directory");
                }
                resource_dir = clang_backend::weakly_canonical_path(
                    std::filesystem::path{
                        command.CommandLine[++argument_index]});
            } else if (argument.starts_with("-resource-dir=")) {
                if (resource_dir) {
                    throw std::runtime_error(
                        "owned compile database duplicates its Clang "
                        "resource directory");
                }
                resource_dir = clang_backend::weakly_canonical_path(
                    std::filesystem::path{
                        argument.substr(
                            std::string{"-resource-dir="}.size())});
            }
        }
        if (!resource_dir
            || *resource_dir != toolchain.resource_dir) {
            throw std::runtime_error(
                "owned compile database Clang resource directory "
                "differs from the exact selected toolchain");
        }
        std::set<std::string, std::less<>>
            expected_mapping_closure;
        std::vector<std::string> expected_mapping_order;
        std::function<void(const std::string&)>
            collect_expected_mapping =
                [&](const std::string& imported) {
                    if (!expected_mapping_closure.insert(
                            imported).second) {
                        return;
                    }
                    for (const std::string& transitive :
                         modules.at(imported).imports) {
                        collect_expected_mapping(transitive);
                    }
                    expected_mapping_order.push_back(imported);
                };
        for (const std::string& imported : module.imports) {
            collect_expected_mapping(imported);
        }
        if (!output || *output != module.pcm
            || mappings.size()
                != expected_mapping_closure.size()) {
            throw std::runtime_error(
                "owned compile database PCM output/import closure "
                "differs from PREPARE manifest for module '"
                + module_name + "'");
        }
        for (const std::string& imported :
             expected_mapping_closure) {
            const auto mapping = mappings.find(imported);
            if (mapping == mappings.end()
                || mapping->second != modules.at(imported).pcm) {
                throw std::runtime_error(
                    "owned compile database has a stale or foreign PCM "
                    "mapping for import '" + imported + "'");
            }
        }
        std::vector<std::string> expected_direct_arguments =
            module.base_arguments;
        for (const std::string& imported :
             expected_mapping_order) {
            expected_direct_arguments.push_back(
                "-fmodule-file=" + imported + "="
                + modules.at(imported).pcm.string());
        }
        const clang_backend::pcm_artifact_paths
            expected_pcm_artifacts =
            clang_backend::derive_pcm_artifact_paths(
                pcm_root,
                module.name,
                module.relocatable_content_key);
        const std::vector<std::string> expected_owned_arguments =
            clang_backend::derive_owned_arguments(
                std::move(expected_direct_arguments),
                module.source_spelling,
                expected_pcm_artifacts);
        if (command.CommandLine != expected_owned_arguments
            || clang_backend::weakly_canonical_path(
                   command.Directory)
                != clang_backend::weakly_canonical_path(
                    module.directory)
            || clang_backend::weakly_canonical_path(
                   command.Filename)
                != module.source) {
            throw std::runtime_error(
                "owned compile database command differs from the "
                "sealed base-argument and module mapping closure for '"
                + module_name + "'");
        }
    }
    if (reached.size() != modules.size()) {
        throw std::runtime_error(
            "PREPARE PCM manifest contains modules outside the selected "
            "source closure");
    }

    std::vector<std::string> signature_rows;
    signature_rows.reserve(modules.size());
    for (const auto& [name, module] : modules) {
        signature_rows.push_back(
            name + "\t" + module.content_key + "\t"
            + module.pcm.string() + "\t"
            + module.depfile_sha256 + "\t"
            + module.dependency_sha256 + "\t"
            + module.scan_database_sha256 + "\t"
            + module.scan_result_sha256 + "\t"
            + module.scan_diagnostics_sha256 + "\t"
            + module.pcm_sha256 + "\t"
            + module.pcm_provenance_sha256);
    }
    std::ranges::sort(signature_rows);
    std::string signature_material =
        std::string{CLANG_VERSION_STRING} + "\n"
        + clang_binary_sha256 + "\n"
        + clang_scan_deps_version + "\n"
        + clang_scan_deps_sha256 + "\n"
        + toolchain.llvm_config_sha256 + "\n"
        + toolchain.llvm_version + "\n"
        + toolchain.prefix.string() + "\n"
        + resource_directory_sha256 + "\n"
        + toolchain.linked_clang_cpp_sha256 + "\n"
        + toolchain.linked_llvm_library_sha256 + "\n"
        + (compiler_inventory_sha256.empty()
               ? std::string{"no-compiler-owned-modules"}
               : compiler_inventory_sha256)
        + "\n" + compile_database_sha256
        + "\n" + std::string{compiler_profile}
        + "\n" + std::string{authority} + "\n";
    for (const std::string& row : signature_rows) {
        signature_material += row + "\n";
    }
    const std::string signature =
        bioseq::common::sha256_hex(signature_material);
    if (signature
        != required_string(*root, "compilation_signature")) {
        throw std::runtime_error(
            "PREPARE compilation signature is stale");
    }
    std::vector<std::string> relocatable_signature_rows;
    relocatable_signature_rows.reserve(modules.size());
    for (const auto& [name, module] : modules) {
        std::string row =
            name
            + "\tsemantic_source="
            + module.relocatable_source_sha256
            + "\targuments="
            + module.relocatable_arguments_sha256
            + "\tcommand_inputs="
            + module.relocatable_command_input_sha256
            + "\tdepfile="
            + module.relocatable_depfile_sha256
            + "\tdependencies="
            + module.relocatable_dependency_sha256
            + "\tscan_database="
            + module.relocatable_scan_database_sha256
            + "\tscan_result="
            + module.relocatable_scan_result_sha256
            + "\tscan_diagnostics="
            + module.relocatable_scan_diagnostics_sha256
            + "\tcontent_key="
            + module.relocatable_content_key;
        for (const std::string& imported : module.imports) {
            row += "\timport=" + imported;
        }
        relocatable_signature_rows.push_back(std::move(row));
    }
    std::ranges::sort(relocatable_signature_rows);
    std::string relocatable_signature_material =
        "bioseq_clang_relocatable_compilation\n"
        "revision=2\n"
        "clang_version=" + std::string{CLANG_VERSION_STRING}
        + "\nclang_binary_sha256=" + clang_binary_sha256
        + "\nclang_scan_deps_version=" + clang_scan_deps_version
        + "\nclang_scan_deps_sha256=" + clang_scan_deps_sha256
        + "\nllvm_config_sha256=" + toolchain.llvm_config_sha256
        + "\nllvm_version=" + toolchain.llvm_version
        + "\nresource_directory_sha256="
        + resource_directory_sha256
        + "\nlinked_clang_cpp_sha256="
        + toolchain.linked_clang_cpp_sha256
        + "\nlinked_llvm_library_sha256="
        + toolchain.linked_llvm_library_sha256
        + "\ncompiler_module_inventory_sha256="
        + (compiler_inventory_sha256.empty()
               ? std::string{"no-compiler-owned-modules"}
               : compiler_inventory_sha256)
        + "\ncompiler_profile=" + std::string{compiler_profile}
        + "\nauthority=" + std::string{authority}
        + "\nowned_compile_database_sha256="
        + relocatable_compile_database_sha256 + "\n";
    for (const std::string& row : relocatable_signature_rows) {
        relocatable_signature_material += row + "\n";
    }
    const std::string relocatable_signature =
        bioseq::common::sha256_hex(relocatable_signature_material);
    if (
        relocatable_signature
        != required_string(
            *root,
            "relocatable_compilation_signature")
    ) {
        throw std::runtime_error(
            "PREPARE relocatable compilation signature is stale");
    }
    std::vector<dependency_module_identity> module_identities;
    module_identities.reserve(modules.size());
    for (const auto& [name, module] : modules) {
        module_identities.push_back(dependency_module_identity{
            .module_name = name,
            .source_path = module.source,
            .source_sha256 = module.source_sha256,
        });
    }
    return {
        .manifest_sha256 = bioseq::common::sha256_hex(document),
        .owned_compile_database_sha256 =
            compile_database_sha256,
        .compilation_signature = signature,
        .relocatable_compilation_signature =
            relocatable_signature,
        .compiler_profile = std::string{compiler_profile},
        .authority = std::string{authority},
        .clang_binary_sha256 = clang_binary_sha256,
        .clang_scan_deps_sha256 = clang_scan_deps_sha256,
        .llvm_config_sha256 = toolchain.llvm_config_sha256,
        .compiler_module_inventory_sha256 =
            compiler_inventory_sha256,
        .modules = std::move(module_identities),
    };
}

namespace {

inline constexpr std::string_view k_frontend_action_trace_schema{
    "bioseq_clang_preparation_action_trace"};
inline constexpr std::int64_t k_frontend_action_trace_schema_revision{1};

struct frontend_action_trace_payload {
    std::string compiler_profile;
    std::string authority;
    std::string canonical_token_import_sha256;
    std::string owned_compile_database_sha256;
    std::string pcm_manifest_sha256;
    std::string clang_binary_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::vector<dependency_module_identity> canonical_inputs;
    std::vector<frontend_action_record> actions;
    frontend_action_counts counts;
    std::string trace_identity;
};

[[nodiscard]] constexpr std::string_view frontend_action_kind_spelling(
    const frontend_action_kind kind) noexcept {
    switch (kind) {
        case frontend_action_kind::canonical_token_slice:
            return "canonical_token_slice";
        case frontend_action_kind::p1689_dependency_discovery:
            return "p1689_dependency_discovery";
        case frontend_action_kind::make_dependency_scan:
            return "make_dependency_scan";
        case frontend_action_kind::pcm_compilation:
            return "pcm_compilation";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view compilation_input_role_spelling(
    const compilation_input_role input_role) noexcept {
    switch (input_role) {
        case compilation_input_role::canonical_scan_only:
            return "canonical_scan_only";
        case compilation_input_role::signature_donor:
            return "signature_donor";
        case compilation_input_role::dependency_provider:
            return "prepared_dependency";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view
frontend_action_outcome_spelling(
    const frontend_action_outcome outcome) noexcept {
    switch (outcome) {
        case frontend_action_outcome::executed:
            return "executed";
        case frontend_action_outcome::validated_cache_hit:
            return "validated_cache_hit";
    }
    return "unknown";
}

[[nodiscard]] frontend_action_kind parse_frontend_action_kind(
    const std::string_view value) {
    if (value == "canonical_token_slice") {
        return frontend_action_kind::canonical_token_slice;
    }
    if (value == "p1689_dependency_discovery") {
        return frontend_action_kind::p1689_dependency_discovery;
    }
    if (value == "make_dependency_scan") {
        return frontend_action_kind::make_dependency_scan;
    }
    if (value == "pcm_compilation") {
        return frontend_action_kind::pcm_compilation;
    }
    throw std::runtime_error(
        "PREPARE action trace contains an unknown action kind");
}

[[nodiscard]] compilation_input_role parse_compilation_input_role(
    const std::string_view value) {
    if (value == "canonical_scan_only") {
        return compilation_input_role::canonical_scan_only;
    }
    if (value == "signature_donor") {
        return compilation_input_role::signature_donor;
    }
    if (value == "prepared_dependency") {
        return compilation_input_role::dependency_provider;
    }
    throw std::runtime_error(
        "PREPARE action trace contains an unknown input class");
}

[[nodiscard]] frontend_action_outcome parse_frontend_action_outcome(
    const std::string_view value) {
    if (value == "executed") {
        return frontend_action_outcome::executed;
    }
    if (value == "validated_cache_hit") {
        return frontend_action_outcome::validated_cache_hit;
    }
    throw std::runtime_error(
        "PREPARE action trace contains an unknown disposition");
}

[[nodiscard]] std::string render_frontend_action_trace_payload(
    const frontend_action_trace_payload& payload,
    const std::string_view trace_identity) {
    std::ostringstream output;
    output
        << "{\n  \"schema\": \""
        << k_frontend_action_trace_schema
        << "\",\n  \"schema_revision\": "
        << k_frontend_action_trace_schema_revision
        << ",\n  \"compiler_profile\": \""
        << clang_backend::json_escape(payload.compiler_profile)
        << "\",\n  \"authority\": \""
        << clang_backend::json_escape(payload.authority)
        << "\",\n  \"canonical_token_import_sha256\": \""
        << payload.canonical_token_import_sha256
        << "\",\n  \"owned_compile_database_sha256\": \""
        << payload.owned_compile_database_sha256
        << "\",\n  \"pcm_manifest_sha256\": \""
        << payload.pcm_manifest_sha256
        << "\",\n  \"toolchain\": {"
        << "\"clang_binary_sha256\":\""
        << payload.clang_binary_sha256
        << "\",\"clang_scan_deps_sha256\":\""
        << payload.clang_scan_deps_sha256
        << "\",\"llvm_config_sha256\":\""
        << payload.llvm_config_sha256
        << "\",\"compiler_module_inventory_sha256\":\""
        << payload.compiler_module_inventory_sha256
        << "\"},\n  \"canonical_inputs\": [";
    for (std::size_t index = 0;
         index < payload.canonical_inputs.size();
         ++index) {
        const dependency_module_identity& input =
            payload.canonical_inputs[index];
        output << (index == 0u ? "\n" : ",\n")
            << "    {\"module\":\""
            << clang_backend::json_escape(input.module_name)
            << "\",\"source_sha256\":\""
            << input.source_sha256 << "\"}";
    }
    output << (payload.canonical_inputs.empty() ? "" : "\n  ")
           << "],\n  \"actions\": [";
    for (std::size_t index = 0;
         index < payload.actions.size();
         ++index) {
        const frontend_action_record& action =
            payload.actions[index];
        output << (index == 0u ? "\n" : ",\n")
            << "    {\"ordinal\":" << index
            << ",\"kind\":\""
            << frontend_action_kind_spelling(action.kind)
            << "\",\"input_class\":\""
            << compilation_input_role_spelling(action.input_role)
            << "\",\"module\":\""
            << clang_backend::json_escape(action.module_name)
            << "\",\"source_sha256\":\""
            << action.source_sha256
            << "\",\"disposition\":\""
            << frontend_action_outcome_spelling(action.outcome)
            << "\"}";
    }
    output << (payload.actions.empty() ? "" : "\n  ")
           << "],\n  \"summary\": {"
           << "\"canonical_token_slice_actions\":"
           << payload.counts.canonical_token_slice_actions
           << ",\"canonical_semantic_frontend_actions\":"
           << payload.counts.canonical_semantic_frontend_actions
           << "},\n  \"trace_identity\": \""
           << trace_identity << "\"\n}\n";
    return output.str();
}

[[nodiscard]] std::string frontend_action_trace_identity(
    const frontend_action_trace_payload& payload) {
    return bioseq::common::sha256_hex(
        "bioseq_clang_preparation_action_trace_identity\n"
        "revision=1\n"
        + render_frontend_action_trace_payload(payload, ""));
}

[[nodiscard]] frontend_action_trace_payload
parse_frontend_action_trace(const std::string_view serialized_trace) {
    llvm::Expected<llvm::json::Value> parsed =
        llvm::json::parse(serialized_trace);
    if (!parsed) {
        throw std::runtime_error(
            "cannot parse PREPARE action trace: "
            + llvm::toString(parsed.takeError()));
    }
    const llvm::json::Object* root = parsed->getAsObject();
    if (root == nullptr) {
        throw std::runtime_error(
            "PREPARE action trace root is not an object");
    }
    const auto require_exact_fields =
        [](const llvm::json::Object& object,
           std::initializer_list<std::string_view> fields,
           const std::string_view role) {
            for (const std::string_view field : fields) {
                if (object.get(field) == nullptr) {
                    throw std::runtime_error(
                        "PREPARE action trace " + std::string{role}
                        + " is missing field '" + std::string{field}
                        + "'");
                }
            }
            if (object.size() != fields.size()) {
                throw std::runtime_error(
                    "PREPARE action trace " + std::string{role}
                    + " contains an unknown or duplicate field");
            }
        };
    const auto required_string =
        [](const llvm::json::Object& object,
           const std::string_view field) {
            const std::optional<llvm::StringRef> value =
                object.getString(field);
            if (!value) {
                throw std::runtime_error(
                    "PREPARE action trace has no string field '"
                    + std::string{field} + "'");
            }
            return value->str();
        };
    require_exact_fields(
        *root,
        {
            "schema",
            "schema_revision",
            "compiler_profile",
            "authority",
            "canonical_token_import_sha256",
            "owned_compile_database_sha256",
            "pcm_manifest_sha256",
            "toolchain",
            "canonical_inputs",
            "actions",
            "summary",
            "trace_identity",
        },
        "root");
    const std::optional<std::int64_t> schema_revision =
        root->getInteger("schema_revision");
    if (required_string(*root, "schema")
            != k_frontend_action_trace_schema
        || !schema_revision
        || *schema_revision
            != k_frontend_action_trace_schema_revision) {
        throw std::runtime_error(
            "PREPARE action trace schema is unsupported");
    }
    frontend_action_trace_payload payload{
        .compiler_profile =
            required_string(*root, "compiler_profile"),
        .authority = required_string(*root, "authority"),
        .canonical_token_import_sha256 = required_string(
            *root, "canonical_token_import_sha256"),
        .owned_compile_database_sha256 = required_string(
            *root, "owned_compile_database_sha256"),
        .pcm_manifest_sha256 = required_string(
            *root, "pcm_manifest_sha256"),
        .trace_identity =
            required_string(*root, "trace_identity"),
    };
    const llvm::json::Object* toolchain =
        root->getObject("toolchain");
    if (toolchain == nullptr) {
        throw std::runtime_error(
            "PREPARE action trace toolchain is not an object");
    }
    require_exact_fields(
        *toolchain,
        {
            "clang_binary_sha256",
            "clang_scan_deps_sha256",
            "llvm_config_sha256",
            "compiler_module_inventory_sha256",
        },
        "toolchain");
    payload.clang_binary_sha256 =
        required_string(*toolchain, "clang_binary_sha256");
    payload.clang_scan_deps_sha256 =
        required_string(*toolchain, "clang_scan_deps_sha256");
    payload.llvm_config_sha256 =
        required_string(*toolchain, "llvm_config_sha256");
    payload.compiler_module_inventory_sha256 = required_string(
        *toolchain, "compiler_module_inventory_sha256");

    const llvm::json::Array* canonical_inputs =
        root->getArray("canonical_inputs");
    if (canonical_inputs == nullptr) {
        throw std::runtime_error(
            "PREPARE action trace canonical_inputs is not an array");
    }
    for (const llvm::json::Value& value : *canonical_inputs) {
        const llvm::json::Object* input = value.getAsObject();
        if (input == nullptr) {
            throw std::runtime_error(
                "PREPARE action trace canonical input is not an object");
        }
        require_exact_fields(
            *input, {"module", "source_sha256"},
            "canonical input");
        payload.canonical_inputs.push_back(
            dependency_module_identity{
                .module_name = required_string(*input, "module"),
                .source_sha256 =
                    required_string(*input, "source_sha256"),
            });
    }

    const llvm::json::Array* actions = root->getArray("actions");
    if (actions == nullptr) {
        throw std::runtime_error(
            "PREPARE action trace actions is not an array");
    }
    for (std::size_t index = 0; index < actions->size(); ++index) {
        const llvm::json::Object* action =
            (*actions)[index].getAsObject();
        if (action == nullptr) {
            throw std::runtime_error(
                "PREPARE action trace action is not an object");
        }
        require_exact_fields(
            *action,
            {
                "ordinal",
                "kind",
                "input_class",
                "module",
                "source_sha256",
                "disposition",
            },
            "action");
        const std::optional<std::int64_t> ordinal =
            action->getInteger("ordinal");
        if (!ordinal || *ordinal < 0
            || static_cast<std::uint64_t>(*ordinal) != index) {
            throw std::runtime_error(
                "PREPARE action trace ordinals are not canonical");
        }
        payload.actions.push_back(frontend_action_record{
            .kind = parse_frontend_action_kind(
                required_string(*action, "kind")),
            .input_role = parse_compilation_input_role(
                required_string(*action, "input_class")),
            .outcome = parse_frontend_action_outcome(
                required_string(*action, "disposition")),
            .module_name = required_string(*action, "module"),
            .source_sha256 =
                required_string(*action, "source_sha256"),
        });
    }

    const llvm::json::Object* summary = root->getObject("summary");
    if (summary == nullptr) {
        throw std::runtime_error(
            "PREPARE action trace summary is not an object");
    }
    require_exact_fields(
        *summary,
        {
            "canonical_token_slice_actions",
            "canonical_semantic_frontend_actions",
        },
        "summary");
    const auto required_count =
        [](const llvm::json::Object& object,
           const std::string_view field) -> std::uint32_t {
            const std::optional<std::int64_t> value =
                object.getInteger(field);
            if (!value || *value < 0
                || static_cast<std::uint64_t>(*value)
                    > std::numeric_limits<std::uint32_t>::max()) {
                throw std::runtime_error(
                    "PREPARE action trace has an invalid count field '"
                    + std::string{field} + "'");
            }
            return static_cast<std::uint32_t>(*value);
        };
    payload.counts = frontend_action_counts{
        .canonical_token_slice_actions = required_count(
            *summary, "canonical_token_slice_actions"),
        .canonical_semantic_frontend_actions = required_count(
            *summary, "canonical_semantic_frontend_actions"),
    };
    const std::string expected_identity =
        frontend_action_trace_identity(payload);
    if (!lowercase_sha256(payload.trace_identity)
        || payload.trace_identity != expected_identity) {
        throw std::runtime_error(
            "PREPARE action trace self-seal is invalid");
    }
    if (serialized_trace != render_frontend_action_trace_payload(
            payload, expected_identity)) {
        throw std::runtime_error(
            "PREPARE action trace is not in canonical encoding");
    }
    return payload;
}

[[nodiscard]] std::pair<std::string, std::string>
frontend_identity_key(
    const dependency_module_identity& identity) {
    return {identity.module_name, identity.source_sha256};
}

[[nodiscard]] std::pair<std::string, std::string>
frontend_identity_key(const frontend_action_record& action) {
    return {action.module_name, action.source_sha256};
}

}  // namespace

[[nodiscard]] std::string render_frontend_action_trace(
    const dependency_compilation_environment& environment) {
    frontend_action_trace_payload payload{
        .compiler_profile = environment.compiler_profile,
        .authority = environment.authority,
        .canonical_token_import_sha256 =
            environment.canonical_token_import_sha256,
        .owned_compile_database_sha256 =
            bioseq::common::sha256_hex(environment.compile_database_json),
        .pcm_manifest_sha256 =
            bioseq::common::sha256_hex(environment.pcm_manifest_json),
        .clang_binary_sha256 = environment.clang_binary_sha256,
        .clang_scan_deps_sha256 =
            environment.clang_scan_deps_sha256,
        .llvm_config_sha256 = environment.llvm_config_sha256,
        .compiler_module_inventory_sha256 =
            environment.compiler_module_inventory_sha256,
        .actions = environment.observed_actions,
    };
    for (const frontend_action_record& action :
         payload.actions) {
        if (action.kind
            == frontend_action_kind::canonical_token_slice) {
            payload.canonical_inputs.push_back(
                dependency_module_identity{
                    .module_name = action.module_name,
                    .source_sha256 = action.source_sha256,
                });
            ++payload.counts.canonical_token_slice_actions;
        } else if (action.input_role
                   == compilation_input_role::canonical_scan_only) {
            ++payload.counts.canonical_semantic_frontend_actions;
        }
    }
    std::ranges::sort(
        payload.canonical_inputs,
        [](const dependency_module_identity& left,
           const dependency_module_identity& right) {
            return frontend_identity_key(left)
                < frontend_identity_key(right);
        });
    payload.trace_identity =
        frontend_action_trace_identity(payload);
    return render_frontend_action_trace_payload(
        payload, payload.trace_identity);
}

[[nodiscard]] frontend_trace_validation
validate_frontend_action_trace(
    const std::filesystem::path& action_trace,
    const dependency_compilation_authority& dependency_authority,
    const dependency_compilation_validation& environment) {
    const frontend_action_trace_payload payload =
        parse_frontend_action_trace(
            read_artifact_bytes(action_trace));
    if (payload.compiler_profile != environment.compiler_profile
        || payload.authority != environment.authority
        || payload.canonical_token_import_sha256
            != dependency_authority.canonical_token_import_sha256
        || payload.owned_compile_database_sha256
            != environment.owned_compile_database_sha256
        || payload.pcm_manifest_sha256
            != environment.manifest_sha256
        || payload.clang_binary_sha256
            != environment.clang_binary_sha256
        || payload.clang_scan_deps_sha256
            != environment.clang_scan_deps_sha256
        || payload.llvm_config_sha256
            != environment.llvm_config_sha256
        || payload.compiler_module_inventory_sha256
            != environment.compiler_module_inventory_sha256) {
        throw std::runtime_error(
            "PREPARE action trace metadata differs from the validated "
            "compile database, PCM manifest, or toolchain");
    }

    using identity_key = std::pair<std::string, std::string>;
    std::vector<identity_key> expected_canonical;
    expected_canonical.reserve(
        dependency_authority.canonical_inputs.size());
    for (const dependency_module_identity& identity :
         dependency_authority.canonical_inputs) {
        expected_canonical.push_back(
            frontend_identity_key(identity));
    }
    std::ranges::sort(expected_canonical);
    std::vector<identity_key> traced_canonical;
    traced_canonical.reserve(payload.canonical_inputs.size());
    for (const dependency_module_identity& identity :
         payload.canonical_inputs) {
        traced_canonical.push_back(
            frontend_identity_key(identity));
    }
    if (!std::ranges::is_sorted(traced_canonical)
        || std::ranges::adjacent_find(traced_canonical)
            != traced_canonical.end()
        || traced_canonical != expected_canonical) {
        throw std::runtime_error(
            "PREPARE action trace canonical input authority differs "
            "from the raw-token dependency authority");
    }

    std::set<identity_key> canonical_keys{
        expected_canonical.begin(), expected_canonical.end()};
    std::set<identity_key> prepared_keys;
    for (const dependency_module_identity& identity :
         environment.modules) {
        if (!prepared_keys.insert(
                frontend_identity_key(identity)).second) {
            throw std::runtime_error(
                "validated PREPARE module identities are not unique");
        }
    }
    const identity_key donor_key = frontend_identity_key(
        dependency_authority.signature_donor);
    if (!prepared_keys.contains(donor_key)
        || std::ranges::any_of(
            canonical_keys,
            [&](const identity_key& key) {
                return prepared_keys.contains(key);
            })) {
        throw std::runtime_error(
            "PREPARE action trace authority overlaps or omits its "
            "signature donor");
    }

    std::set<identity_key> observed_canonical_tokens;
    std::set<std::tuple<
        frontend_action_kind,
        std::string,
        std::string>> observed_semantic_actions;
    frontend_action_counts recomputed;
    for (const frontend_action_record& action : payload.actions) {
        const identity_key key = frontend_identity_key(action);
        if (action.module_name.empty()
            || !lowercase_sha256(action.source_sha256)) {
            throw std::runtime_error(
                "PREPARE action trace contains an invalid source identity");
        }
        if (action.kind
            == frontend_action_kind::canonical_token_slice) {
            ++recomputed.canonical_token_slice_actions;
            if (action.input_role
                    != compilation_input_role::canonical_scan_only
                || action.outcome != frontend_action_outcome::executed
                || !canonical_keys.contains(key)
                || !observed_canonical_tokens.insert(key).second) {
                throw std::runtime_error(
                    "PREPARE canonical token-slice action has invalid "
                    "classification, disposition, or identity");
            }
            continue;
        }
        if (canonical_keys.contains(key)) {
            ++recomputed.canonical_semantic_frontend_actions;
            throw std::runtime_error(
                "PREPARE action trace reports a canonical input reached "
                "a semantic frontend action");
        }
        if (!prepared_keys.contains(key)) {
            throw std::runtime_error(
                "PREPARE semantic action has no authenticated PCM "
                "manifest identity");
        }
        const compilation_input_role expected_role = key == donor_key
            ? compilation_input_role::signature_donor
            : compilation_input_role::dependency_provider;
        if (action.input_role != expected_role) {
            throw std::runtime_error(
                "PREPARE semantic action input classification is stale");
        }
        if (action.kind
                == frontend_action_kind::p1689_dependency_discovery
            && action.outcome != frontend_action_outcome::executed) {
            throw std::runtime_error(
                "PREPARE P1689 discovery cannot claim a cache hit");
        }
        if (!observed_semantic_actions.emplace(
                action.kind,
                action.module_name,
                action.source_sha256).second) {
            throw std::runtime_error(
                "PREPARE action trace duplicates a semantic action");
        }
    }
    if (observed_canonical_tokens != canonical_keys) {
        throw std::runtime_error(
            "PREPARE action trace omits a canonical token-slice action");
    }
    for (const identity_key& key : prepared_keys) {
        for (const frontend_action_kind kind : {
                 frontend_action_kind::p1689_dependency_discovery,
                 frontend_action_kind::make_dependency_scan,
                 frontend_action_kind::pcm_compilation,
             }) {
            if (!observed_semantic_actions.contains(
                    {kind, key.first, key.second})) {
                throw std::runtime_error(
                    "PREPARE action trace omits a required authenticated "
                    "module action");
            }
        }
    }
    if (payload.counts.canonical_token_slice_actions
            != recomputed.canonical_token_slice_actions
        || payload.counts.canonical_semantic_frontend_actions
            != recomputed.canonical_semantic_frontend_actions
        || recomputed.canonical_semantic_frontend_actions != 0u) {
        throw std::runtime_error(
            "PREPARE action trace summary differs from observed actions");
    }
    return frontend_trace_validation{
        .trace_identity = payload.trace_identity,
        .counts = recomputed,
    };
}

}  // namespace bioseq::codegen::structural_analysis
