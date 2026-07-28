module;

#include <cstddef>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

export module bioseq.codegen.sql:migration;

import :model;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::string migration_physical_id(
    std::size_t ordinal) {
    std::ostringstream output;
    output << 'P' << std::setw(3) << std::setfill('0') << ordinal;
    return output.str();
}

[[nodiscard]] inline std::string migration_identifier(
    std::size_t ordinal) {
    std::ostringstream output;
    output << "statement_p" << std::setw(3) << std::setfill('0') << ordinal;
    return output.str();
}

[[nodiscard]] inline std::string raw_delimiter(
    std::string_view text,
    std::size_t ordinal) {
    for (char suffix = 'a'; suffix <= 'z'; ++suffix) {
        std::ostringstream candidate;
        candidate << 'B' << std::setw(3) << std::setfill('0') << ordinal
                  << suffix;
        const std::string delimiter = candidate.str();
        if (!text.contains(")" + delimiter + "\"")) {
            return delimiter;
        }
    }
    throw std::runtime_error(
        "cannot select a collision-free migration raw-string delimiter");
}

[[nodiscard]] inline std::string cpp_string_literal(
    std::string_view text) {
    std::string result{"\""};
    for (const char value : text) {
        switch (value) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back(value);
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] inline std::string_view typed_annotation_name(
    const sql_statement& statement) {
    switch (statement.kind) {
        case statement_kind::alter: return "alter";
        case statement_kind::comment: return "comment";
        case statement_kind::create_access_method: return "access_method";
        case statement_kind::create_aggregate: return "aggregate";
        case statement_kind::create_cast: return "cast";
        case statement_kind::create_function:
            return statement.routine && statement.routine->language == "sql"
                ? "sql_function"
                : "function";
        case statement_kind::create_index: return "index";
        case statement_kind::create_operator: return "operator_";
        case statement_kind::create_operator_class: return "opclass";
        case statement_kind::create_procedure: return "procedure";
        case statement_kind::create_schema: return "schema";
        case statement_kind::create_sequence: return "sequence";
        case statement_kind::create_table: return "table";
        case statement_kind::create_type: return "type";
        case statement_kind::create_view: return "view";
        case statement_kind::grant: return "grant";
        case statement_kind::revoke: return "revoke";
        case statement_kind::select: return "config_dump";
        case statement_kind::set: return "session_setting";
        case statement_kind::insert: return "am_catalog_dependencies";
        case statement_kind::reset:
        case statement_kind::unknown: return "install_action";
    }
    return "install_action";
}

[[nodiscard]] inline std::string_view typed_statement_kind_name(
    const sql_statement& statement) {
    switch (statement.kind) {
        case statement_kind::alter: return "alter";
        case statement_kind::comment: return "comment";
        case statement_kind::create_access_method: return "access_method";
        case statement_kind::create_aggregate: return "aggregate";
        case statement_kind::create_cast: return "cast";
        case statement_kind::create_function:
            return statement.routine && statement.routine->language == "sql"
                ? "sql_function"
                : "function";
        case statement_kind::create_index: return "index";
        case statement_kind::create_operator: return "operator_";
        case statement_kind::create_operator_class: return "opclass";
        case statement_kind::create_procedure: return "procedure";
        case statement_kind::create_schema: return "schema";
        case statement_kind::create_sequence: return "sequence";
        case statement_kind::create_table: return "table";
        case statement_kind::create_type: return "type";
        case statement_kind::create_view: return "view";
        case statement_kind::grant: return "grant";
        case statement_kind::revoke: return "revoke";
        case statement_kind::select: return "config_dump";
        case statement_kind::set: return "session_setting";
        case statement_kind::insert: return "catalog_dependencies";
        case statement_kind::reset:
        case statement_kind::unknown: return "alter";
    }
    return "alter";
}

[[nodiscard]] inline std::string unsupported_ledger_reason(
    const sql_statement& statement) {
    switch (statement.kind) {
        case statement_kind::insert:
            if (statement.normalized_text.starts_with(
                    "insert into pg_catalog.pg_depend (")
                && statement.normalized_text.contains(
                    "join pg_catalog.pg_am as am")
                && statement.normalized_text.contains(
                    "where am.amname = 'bioseq'")) {
                return {};
            }
            return "postgresql_catalog_dependency_dml_has_no_typed_model";
        case statement_kind::reset:
            return "session_reset_has_no_typed_model";
        case statement_kind::select:
            if (!statement.normalized_text.starts_with(
                    "select pg_catalog.pg_extension_config_dump(")
                && !statement.normalized_text.starts_with(
                    "select pg_extension_config_dump(")) {
                return "top_level_select_has_no_typed_model";
            }
            return {};
        case statement_kind::unknown:
            return "unclassified_top_level_sql_has_no_typed_model";
        default:
            return {};
    }
}

[[nodiscard]] inline std::string migration_enum(
    std::string_view enum_type,
    std::string_view value) {
    return "bioseq::sql::" + std::string{enum_type}
        + "::" + std::string{value};
}

}  // namespace detail

/// Serializes each retained statement into a compiler-checked, typed C++
/// install owner. Ordinary generation reads only this source authority.
[[nodiscard]] inline std::string render_migration_owner(
    const install_script& script) {
    std::ostringstream output;
    output
        << "export module bioseq.sql.install_owner;\n\n"
        << "import bioseq.codegen.sql.annotation_model.migration;\n\n"
        << "namespace bioseq::sql_install {\n\n"
        << "// Generated only by `bioseq_codegen sql migrate-baseline`.\n"
        << "// Typed descriptors own install semantics; exact SQL bytes remain "
           "the formatting-preserving compatibility facet.\n\n";
    for (const sql_statement& statement : script.statements) {
        if (statement.disposition != statement_disposition::retained) {
            continue;
        }
        const std::string physical_id =
            detail::migration_physical_id(statement.physical_ordinal);
        const std::string identifier =
            detail::migration_identifier(statement.physical_ordinal);
        std::string delimiter_guard =
            statement.text + statement.normalized_text;
        if (statement.routine) {
            const routine_metadata& routine = *statement.routine;
            delimiter_guard += routine.identity;
            delimiter_guard += routine.argument_declarations;
            delimiter_guard += routine.returns;
            delimiter_guard += routine.configuration;
            delimiter_guard += routine.cost;
            delimiter_guard += routine.rows;
            delimiter_guard += routine.support;
        }
        const std::string delimiter = detail::raw_delimiter(
            delimiter_guard, statement.physical_ordinal);
        const std::string reason =
            detail::unsupported_ledger_reason(statement);
        if (!reason.empty()) {
            output
                << "[[bioseq_sql::install_action({\n"
                << "    .baseline_physical_id = \"" << physical_id
                << "\",\n"
                << "    .migration_ledger_reason = \"" << reason
                << "\",\n"
                << "})]]\n"
                << "constexpr bioseq::sql::migration_install_action "
                << identifier << " = {\n"
                << "    .sql = bioseq::sql::checked_sql{R\"" << delimiter
                << "(" << statement.text << ")" << delimiter << "\"},\n"
                << "    .reason = \"" << reason << "\",\n"
                << "};\n\n";
            continue;
        }
        const std::string_view annotation =
            detail::typed_annotation_name(statement);
        output
            << "[[bioseq_sql::" << annotation << "({\n"
            << "    .baseline_physical_id = \"" << physical_id << "\",\n"
            << "    .sql_identity = "
            << detail::cpp_string_literal(
                statement.routine
                    ? statement.routine->identity
                    : statement.normalized_text)
            << ",\n"
            << "    .create_or_replace = "
            << (statement.create_or_replace ? "true" : "false")
            << ",\n";
        if (statement.routine) {
            const routine_metadata& routine = *statement.routine;
            output
                << "    .language = "
                << detail::migration_enum(
                    "migration_routine_language", routine.language)
                << ",\n"
                << "    .c_symbol = "
                << detail::cpp_string_literal(routine.c_symbol)
                << ",\n"
                << "    .argument_declarations = "
                << detail::cpp_string_literal(
                    routine.argument_declarations)
                << ",\n"
                << "    .returns = "
                << detail::cpp_string_literal(routine.returns)
                << ",\n"
                << "    .volatility_value = "
                << detail::migration_enum(
                    "migration_volatility",
                    routine.volatility == "volatile"
                        ? "volatile_"
                        : routine.volatility)
                << ",\n"
                << "    .null_input = "
                << detail::migration_enum(
                    "migration_null_input_policy", routine.null_input)
                << ",\n"
                << "    .security = "
                << detail::migration_enum(
                    "migration_security_mode", routine.security)
                << ",\n"
                << "    .parallel = "
                << detail::migration_enum(
                    "migration_parallel_safety", routine.parallel)
                << ",\n"
                << "    .configuration = "
                << detail::cpp_string_literal(routine.configuration)
                << ",\n"
                << "    .cost = "
                << detail::cpp_string_literal(routine.cost)
                << ",\n"
                << "    .rows = "
                << detail::cpp_string_literal(routine.rows)
                << ",\n"
                << "    .support = "
                << detail::cpp_string_literal(routine.support)
                << ",\n";
        }
        output
            << "})]]\n"
            << "constexpr bioseq::sql::typed_install_statement "
            << identifier << " = {\n"
            << "    .sql = bioseq::sql::checked_sql{R\"" << delimiter
            << "(" << statement.text << ")" << delimiter << "\"},\n"
            << "    .kind = bioseq::sql::migration_statement_kind::"
            << detail::typed_statement_kind_name(statement) << ",\n"
            << "    .sql_identity = R\""
            << delimiter << "("
            << (statement.routine
                    ? statement.routine->identity
                    : statement.normalized_text)
            << ")" << delimiter << "\",\n";
        output
            << "    .create_or_replace = "
            << (statement.create_or_replace ? "true" : "false")
            << ",\n";
        if (statement.routine) {
            const routine_metadata& routine = *statement.routine;
            output
                << "    .routine = {\n"
                << "        .identity = R\""
                << delimiter << "(" << routine.identity << ")"
                << delimiter << "\",\n"
                << "        .argument_declarations = "
                   "R\""
                << delimiter << "(" << routine.argument_declarations << ")"
                << delimiter << "\",\n"
                << "        .returns = R\""
                << delimiter << "(" << routine.returns << ")"
                << delimiter << "\",\n"
                << "        .language = "
                << detail::migration_enum(
                    "migration_routine_language", routine.language)
                << ",\n"
                << "        .c_symbol = "
                << detail::cpp_string_literal(routine.c_symbol)
                << ",\n"
                << "        .volatility_value = "
                << detail::migration_enum(
                    "migration_volatility",
                    routine.volatility == "volatile"
                        ? "volatile_"
                        : routine.volatility)
                << ",\n"
                << "        .null_input = "
                << detail::migration_enum(
                    "migration_null_input_policy", routine.null_input)
                << ",\n"
                << "        .security = "
                << detail::migration_enum(
                    "migration_security_mode", routine.security)
                << ",\n"
                << "        .parallel = "
                << detail::migration_enum(
                    "migration_parallel_safety", routine.parallel)
                << ",\n"
                << "        .configuration = "
                   "R\""
                << delimiter << "(" << routine.configuration << ")"
                << delimiter << "\",\n"
                << "        .cost = R\""
                << delimiter << "(" << routine.cost << ")"
                << delimiter << "\",\n"
                << "        .rows = R\""
                << delimiter << "(" << routine.rows << ")"
                << delimiter << "\",\n"
                << "        .support = R\""
                << delimiter << "(" << routine.support << ")"
                << delimiter << "\",\n"
                << "    },\n";
        }
        output
            << "};\n\n";
    }
    output << "}  // namespace bioseq::sql_install\n";
    return output.str();
}

}  // namespace bioseq::codegen::sql
