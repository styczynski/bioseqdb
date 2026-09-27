/**
 * @file
 * @brief Generates the configured C++ module, compile checks and JSON manifest.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.build_config:renderer;

import :model;
import :validator;
import bioseq.codegen.structural_analysis;
import bioseq.codegen.support;

export namespace bioseq::codegen::build_config {

namespace detail {

struct source_edit final {
    std::size_t begin{};
    std::size_t end{};
    std::string replacement;
};

[[nodiscard]] inline std::string apply_edits(const std::string_view source,
                                             std::vector<source_edit> edits) {
    std::ranges::sort(edits, {}, &source_edit::begin);
    std::size_t previous_end = 0U;
    for (const source_edit& edit : edits) {
        if (edit.begin < previous_end || edit.end < edit.begin || edit.end > source.size()) {
            throw std::runtime_error(
                "build-config renderer received overlapping or stale source geometry");
        }
        previous_end = edit.end;
    }
    std::string result;
    result.reserve(source.size());
    std::size_t cursor = 0U;
    for (const source_edit& edit : edits) {
        result.append(source.substr(cursor, edit.begin - cursor));
        result.append(edit.replacement);
        cursor = edit.end;
    }
    result.append(source.substr(cursor));
    return result;
}

[[nodiscard]] inline std::string value_expression(const resolved_entry& entry) {
    switch (entry.entry.kind) {
    case value_kind::boolean:
        return "(" + entry.entry.macro_name + " != 0)";
    case value_kind::signed_integer:
        return "static_cast<int>(" + entry.entry.macro_name + ")";
    case value_kind::unsigned_integer:
        return "static_cast<unsigned int>(" + entry.entry.macro_name + ")";
    }
    throw std::runtime_error("invalid build-config value kind");
}

[[nodiscard]] inline std::string json_escape(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    constexpr std::string_view digits = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\b':
            result += "\\b";
            break;
        case '\f':
            result += "\\f";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (character < 0x20U) {
                result += "\\u00";
                result.push_back(digits[character >> 4U]);
                result.push_back(digits[character & 0x0fU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::string canonical_identity(const config_model& model,
                                                    const std::vector<resolved_entry>& entries) {
    std::string material = "bioseq-build-config\nrevision=1\nsource=" + model.source_sha256 + "\n";
    for (const resolved_entry& entry : entries) {
        material += entry.entry.name + "\t" + entry.entry.macro_name + "\t" + entry.entry.cpp_type +
                    "\t" + entry.entry.unit + "\t" + entry.entry.domain.spelling + "\t" +
                    entry.expression + "\n";
    }
    return bioseq::common::sha256_hex(material);
}

[[nodiscard]] inline std::string render_manifest(const config_model& model,
                                                 const std::vector<resolved_entry>& entries,
                                                 const std::string_view config_digest) {
    const std::filesystem::path normalized_source = model.source_path.lexically_normal();
    if (normalized_source.empty() || normalized_source == "." ||
        (!normalized_source.empty() && *normalized_source.begin() == "..")) {
        throw std::runtime_error("build-config source identity must not escape its root");
    }
    const std::string source_identity =
        normalized_source.is_absolute()
            ? structural_analysis::repository_source_identity(normalized_source)
            : normalized_source.generic_string();
    std::ostringstream output;
    output << "{\n"
           << "  \"schema_revision\": 1,\n"
           << "  \"generator\": \"bioseq-build-config\",\n"
           << "  \"module\": \"" << json_escape(model.module_name) << "\",\n"
           << "  \"source\": \"" << json_escape(source_identity) << "\",\n"
           << "  \"source_sha256\": \"" << model.source_sha256 << "\",\n"
           << "  \"config_sha256\": \"" << config_digest << "\",\n"
           << "  \"entries\": [\n";
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        const resolved_entry& entry = entries[index];
        output << "    {\"name\":\"" << json_escape(entry.entry.name) << "\",\"macro\":\""
               << json_escape(entry.entry.macro_name) << "\",\"type\":\""
               << json_escape(entry.entry.cpp_type) << "\",\"unit\":\""
               << json_escape(entry.entry.unit) << "\",\"domain\":\""
               << json_escape(entry.entry.domain.spelling) << "\",\"default\":\""
               << json_escape(entry.entry.default_expression) << "\",\"value\":" << entry.expression
               << "}" << (index + 1U == entries.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

[[nodiscard]] inline std::string
render_compile_contract(const std::vector<resolved_entry>& entries) {
    std::ostringstream output;
    output << "/**\n"
              " * @file\n"
              " * @brief Verifies the configured module values during compilation.\n"
              " */\n"
              "import bioseq.build_config;\n\n";
    for (const resolved_entry& entry : entries) {
        std::string expected;
        switch (entry.entry.kind) {
        case value_kind::boolean:
            expected = entry.value == 0 ? "false" : "true";
            break;
        case value_kind::signed_integer:
            expected = std::to_string(entry.value);
            break;
        case value_kind::unsigned_integer:
            expected = std::to_string(entry.value) + "U";
            break;
        }
        output << "static_assert(bioseq::build_config::" << entry.entry.name << " == " << expected
               << ");\n";
    }
    return output.str();
}

} // namespace detail

[[nodiscard]] inline generation_result generate(const config_model& model,
                                                const std::vector<config_override>& overrides) {
    std::vector<resolved_entry> entries = resolve_config(model, overrides);
    std::vector<detail::source_edit> edits;
    edits.reserve(entries.size() * 2U);
    for (const resolved_entry& entry : entries) {
        edits.push_back(detail::source_edit{
            .begin = entry.entry.attribute_begin,
            .end = entry.entry.attribute_end,
        });
        edits.push_back(detail::source_edit{
            .begin = entry.entry.initializer_begin,
            .end = entry.entry.initializer_end,
            .replacement = detail::value_expression(entry),
        });
    }

    std::ostringstream boundary;
    boundary << "module;\n\n";
    for (const resolved_entry& entry : entries) {
        boundary << "#define " << entry.entry.macro_name << ' ' << entry.expression << '\n';
    }
    boundary << '\n';

    std::string module_source =
        boundary.str() + detail::apply_edits(model.source, std::move(edits));
    if (!module_source.ends_with('\n'))
        module_source.push_back('\n');
    module_source.push_back('\n');
    for (const resolved_entry& entry : entries) {
        module_source += "#undef " + entry.entry.macro_name + "\n";
    }

    const std::string digest = detail::canonical_identity(model, entries);
    return generation_result{
        .module_source = std::move(module_source),
        .compile_contract_source = detail::render_compile_contract(entries),
        .manifest_json = detail::render_manifest(model, entries, digest),
        .entries = std::move(entries),
    };
}

} // namespace bioseq::codegen::build_config
