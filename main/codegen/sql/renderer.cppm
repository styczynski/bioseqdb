module;

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.codegen.sql:baseline_renderer;

import :model;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::string json_escape(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 8);
    for (const unsigned char value : text) {
        switch (value) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (value < 0x20U) {
                    constexpr char digits[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(digits[(value >> 4U) & 0x0fU]);
                    result.push_back(digits[value & 0x0fU]);
                } else {
                    result.push_back(static_cast<char>(value));
                }
        }
    }
    return result;
}

}  // namespace detail

/// Reassembles the exact canonical source after semantic validation.
[[nodiscard]] inline std::string render_install_script(
    const install_script& script) {
    std::string result = script.preamble;
    for (const sql_statement& statement : script.statements) {
        if (statement.disposition == statement_disposition::retained) {
            result += statement.text;
        }
    }
    result += script.trailing_text;
    if (result.empty() || result.back() != '\n') {
        result.push_back('\n');
    }
    return result;
}

/// Emits the normalized statement manifest shared by validation and website
/// consumers.
[[nodiscard]] inline std::string render_manifest(
    const install_script& script,
    std::string_view rendered_sql) {
    std::map<std::string_view, std::size_t, std::less<>> kind_counts;
    std::size_t retained_count = 0;
    for (const sql_statement& statement : script.statements) {
        if (statement.disposition != statement_disposition::retained) {
            continue;
        }
        ++kind_counts[kind_name(statement.kind)];
        ++retained_count;
    }

    std::ostringstream output;
    output << "{\n"
           << "  \"manifest_version\": 1,\n"
           << "  \"authority_mode\": \"legacy_baseline_migration\",\n"
           << "  \"script_sha256\": \"" << sha256_hex(rendered_sql)
           << "\",\n"
           << "  \"physical_statement_count\": "
           << script.statements.size() << ",\n"
           << "  \"statement_count\": " << retained_count << ",\n"
           << "  \"kind_counts\": {";
    bool first_kind = true;
    for (const auto& [name, count] : kind_counts) {
        output << (first_kind ? "\n" : ",\n")
               << "    \"" << name << "\": " << count;
        first_kind = false;
    }
    if (!kind_counts.empty()) {
        output << '\n';
    }
    output << "  },\n"
           << "  \"statements\": [\n";
    for (std::size_t index = 0; index < script.statements.size(); ++index) {
        const sql_statement& statement = script.statements[index];
        output << "    {\n"
               << "      \"physical_id\": \"P";
        output.width(3);
        output.fill('0');
        output << statement.physical_ordinal;
        output.fill(' ');
        output << "\",\n"
               << "      \"kind\": \"" << kind_name(statement.kind)
               << "\",\n"
               << "      \"disposition\": \""
               << disposition_name(statement.disposition) << "\",\n"
               << "      \"line_begin\": " << statement.span.line_begin
               << ",\n"
               << "      \"line_end\": " << statement.span.line_end
               << ",\n"
               << "      \"normalized_sha256\": \""
               << statement.normalized_hash << "\"";
        if (statement.routine_identity) {
            output << ",\n      \"identity\": \""
                   << detail::json_escape(*statement.routine_identity) << "\"";
        }
        if (statement.superseded_by_physical_ordinal) {
            output << ",\n      \"superseded_by\": \"P";
            output.width(3);
            output.fill('0');
            output << *statement.superseded_by_physical_ordinal;
            output.fill(' ');
            output << "\"";
        }
        output << "\n    }"
               << (index + 1 == script.statements.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    return output.str();
}

/// Publishes build-tree artifacts and checks or updates the checked SQL golden.
inline void publish_artifacts(
    const command_line& command,
    std::string_view rendered_sql,
    std::string_view manifest,
    std::string_view publication_material) {
    write_if_different(command.output, rendered_sql);
    write_if_different(command.manifest, manifest);
    if (command.update_golden && command.golden.empty()) {
        throw std::runtime_error(
            "the explicit update-golden command requires --golden");
    }
    if (command.update_golden) {
        write_if_different(command.golden, rendered_sql);
    } else if (!command.golden.empty()
               && read_file(command.golden) != rendered_sql) {
        throw std::runtime_error(
            "generated install SQL differs from checked golden "
            + command.golden.string()
            + "; run the explicit bioseq_sql_update_golden target after "
              "reviewing the manifest delta");
    }
    const std::string action =
        command.action == command_action::migrate_baseline
        ? "migrate-baseline"
        : command.action == command_action::update_golden
        ? "update-golden"
        : "generate";
    const std::string stamp =
        "bioseq_sql_artifact_stamp/v1\n"
        "operation=" + action + "\n"
        "profile="
        + (command.compiler_profile.empty()
               ? std::string{"legacy-baseline"}
               : command.compiler_profile)
        + "\nsql_sha256=" + sha256_hex(rendered_sql)
        + "\nmanifest_sha256=" + sha256_hex(manifest)
        + "\npublication_closure_sha256="
        + sha256_hex(publication_material) + "\n";
    write_if_different(command.stamp, stamp);
    touch_stamp(command.stamp);
}

}  // namespace bioseq::codegen::sql
