module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.collector;

export import :model;
export import :options;
export import :gucs;
import bioseq.codegen.structural_analysis;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::optional<entity_kind> annotation_entity_kind(
    std::string_view name) {
    if (name == "schema") return entity_kind::schema;
    if (name == "session_setting") return entity_kind::session_setting;
    if (name == "sequence") return entity_kind::sequence;
    if (name == "table") return entity_kind::table;
    if (name == "column") return entity_kind::column;
    if (name == "primary_key") return entity_kind::primary_key;
    if (name == "unique") return entity_kind::unique;
    if (name == "foreign_key") return entity_kind::foreign_key;
    if (name == "check") return entity_kind::check;
    if (name == "index") return entity_kind::index;
    if (name == "view") return entity_kind::view;
    if (name == "function") return entity_kind::function;
    if (name == "procedure") return entity_kind::procedure;
    if (name == "sql_function") return entity_kind::sql_function;
    if (name == "type") return entity_kind::type;
    if (name == "cast") return entity_kind::cast;
    if (name == "operator_") return entity_kind::operator_;
    if (name == "aggregate") return entity_kind::aggregate;
    if (name == "opclass") return entity_kind::opclass;
    if (name == "access_method") return entity_kind::access_method;
    if (name == "guc") return entity_kind::guc;
    if (name == "guc_value") return entity_kind::guc_value;
    if (name == "grant") return entity_kind::grant;
    if (name == "revoke") return entity_kind::revoke;
    if (name == "config_dump") return entity_kind::config_dump;
    if (name == "phase") return entity_kind::phase;
    if (name == "am_catalog_dependencies")
        return entity_kind::catalog_dependencies;
    if (name == "type_input" || name == "type_output"
        || name == "type_receive" || name == "type_send"
        || name == "type_typmod_in" || name == "type_typmod_out"
        || name == "type_analyze" || name == "type_subscript") {
        return entity_kind::type_role;
    }
    return std::nullopt;
}

[[nodiscard]] inline bool companion_kind(entity_kind kind) noexcept {
    switch (kind) {
        case entity_kind::column:
        case entity_kind::primary_key:
        case entity_kind::unique:
        case entity_kind::foreign_key:
        case entity_kind::check:
        case entity_kind::index:
        case entity_kind::type_role:
        case entity_kind::cast:
        case entity_kind::operator_:
        case entity_kind::access_method:
        case entity_kind::guc_value:
        case entity_kind::grant:
        case entity_kind::revoke:
        case entity_kind::config_dump:
        case entity_kind::catalog_dependencies:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] inline bool routine_kind(entity_kind kind) noexcept {
    return kind == entity_kind::function
        || kind == entity_kind::procedure
        || kind == entity_kind::sql_function;
}

[[nodiscard]] inline std::optional<std::string>
logical_vector_sql_type(std::string_view cpp_type) {
    std::string compact;
    compact.reserve(cpp_type.size());
    for (const unsigned char value : cpp_type) {
        if (!std::isspace(value)) {
            compact.push_back(static_cast<char>(value));
        }
    }
    const std::size_t open = compact.find('<');
    if (open == std::string::npos || !compact.ends_with('>')) {
        return std::nullopt;
    }
    const std::size_t outer_separator =
        compact.substr(0, open).rfind("::");
    const std::string_view outer =
        outer_separator == std::string_view::npos
        ? std::string_view{compact}.substr(0, open)
        : std::string_view{compact}.substr(
              outer_separator + 2,
              open - outer_separator - 2);
    if (outer != "vector_operand"
        && outer != "vector_column"
        && outer != "vector_nullmask"
        && outer != "vector_result") {
        return std::nullopt;
    }

    const std::string_view arguments =
        std::string_view{compact}.substr(
            open + 1, compact.size() - open - 2);
    int depth = 0;
    std::size_t first_end = arguments.size();
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (arguments[index] == '<') {
            ++depth;
        } else if (arguments[index] == '>') {
            --depth;
        } else if (arguments[index] == ',' && depth == 0) {
            first_end = index;
            break;
        }
    }
    const std::string_view logical =
        arguments.substr(0, first_end);
    const std::size_t logical_separator = logical.rfind("::");
    std::string result{
        logical_separator == std::string_view::npos
        ? logical
        : logical.substr(logical_separator + 2)};
    if (result == "character") return std::string{"char"};
    if (result == "double_precision") {
        return std::string{"double precision"};
    }
    static constexpr std::array<std::string_view, 9> supported{
        "boolean",
        "smallint",
        "integer",
        "bigint",
        "date",
        "time",
        "timestamp",
        "timestamptz",
        "quoted_any",
    };
    if (!std::ranges::contains(supported, result)) {
        throw std::runtime_error(
            "unregistered logical SQL type in vector boundary C++ type '"
            + std::string{cpp_type} + "'");
    }
    return result;
}

[[nodiscard]] inline std::string sql_type_from_cpp(
    std::string_view cpp_type) {
    const auto contains = [cpp_type](std::string_view value) {
        return cpp_type.find(value) != std::string_view::npos;
    };
    if (contains("optional<")) {
        const std::size_t open = cpp_type.find('<');
        const std::size_t close = cpp_type.rfind('>');
        if (close != std::string_view::npos && close > open) {
            return sql_type_from_cpp(
                cpp_type.substr(open + 1, close - open - 1));
        }
    }
    if (const std::optional<std::string> logical =
            logical_vector_sql_type(cpp_type)) {
        return *logical;
    }
    if (contains("timestamp_with_time_zone")) return "timestamptz";
    if (contains("interval_value")) return "interval";
    if (contains("jsonb_value")) return "jsonb";
    if (contains("bytea_value")) return "bytea";
    if (contains("bigint_array")) return "bigint[]";
    if (contains("tuple_identifier")) return "tid";
    if (contains("bool")) return "boolean";
    if (contains("TimestampTz")) return "timestamptz";
    if (contains("Timestamp")) return "timestamp";
    if (contains("TimeADT")) return "time";
    if (contains("DateADT")) return "date";
    if (contains("Interval")) return "interval";
    if (contains("Jsonb")) return "jsonb";
    if (contains("FullTransactionId")) return "xid8";
    if (contains("NameData")) return "name";
    if (contains("bytea")) return "bytea";
    if (contains("text")) return "text";
    if (contains("int16_t") || contains("short")) return "smallint";
    if (contains("int32_t") || cpp_type == "int") return "integer";
    if (contains("int64_t") || contains("long")) return "bigint";
    if (contains("float")) return "real";
    if (contains("double")) return "double precision";
    if (contains("string") || contains("string_view")) return "text";
    if (contains("Oid")) return "oid";
    if (contains("Datum")) return "internal";
    if (contains("void")) return "void";
    const std::size_t separator = cpp_type.rfind("::");
    std::string result{
        separator == std::string_view::npos
            ? cpp_type
            : cpp_type.substr(separator + 2)};
    result.erase(std::ranges::remove_if(
        result,
        [](char value) {
            return value == '&' || value == '*' || value == ' '
                || value == '\t' || value == '\n';
        }).begin(),
        result.end());
    return result.empty() ? "internal" : result;
}

[[nodiscard]] inline bool injected_call_context(
        std::string_view cpp_type) {
    std::string compact;
    compact.reserve(cpp_type.size());
    for (const unsigned char value : cpp_type) {
        if (!std::isspace(value) && value != '&') {
            compact.push_back(static_cast<char>(value));
        }
    }
    while (compact.starts_with("const")) {
        compact.erase(0, std::string_view{"const"}.size());
    }
    return compact
            == "bioseq::postgres::support::function_call_context"
        || compact == "function_call_context";
}

struct parameter_sql_contract final {
    std::string sql_type;
    std::optional<std::string> explicit_default;
};

[[nodiscard]] inline parameter_sql_contract parameter_sql_override(
    const structural_analysis::parameter_record& parameter) {
    parameter_sql_contract result;
    bool saw_parameter_annotation = false;
    for (const structural_analysis::attribute_record& attribute :
         parameter.attributes) {
        if (attribute.vendor_namespace != "bioseq_sql"
            || attribute.name != "param") {
            continue;
        }
        if (saw_parameter_annotation) {
            throw std::runtime_error(
                "SQL parameter '" + parameter.name
                + "' has more than one bioseq_sql::param annotation");
        }
        saw_parameter_annotation = true;
        for (const option_value& option : parse_options(attribute)) {
            if (option.name == "sql_type") {
                if (option.expression.contains("type_of<")) {
                    result.sql_type = option.expression;
                } else {
                    result.sql_type =
                        detail::leaf_identifier(option.expression);
                }
            } else if (option.name == "default_value") {
                result.explicit_default = option.expression;
            } else {
                throw std::runtime_error(
                    "bioseq_sql::param has unknown option '."
                    + option.name + "'");
            }
        }
    }
    if (result.explicit_default
        && !result.explicit_default->contains("checked_sql")) {
        throw std::runtime_error(
            "SQL parameter '" + parameter.name
            + "' option .default_value must be a checked_sql expression");
    }
    return result;
}

[[nodiscard]] inline std::string result_sql_override(
    const structural_analysis::declaration_record& declaration) {
    bool saw_result_annotation = false;
    std::string result;
    for (const structural_analysis::attribute_record& attribute :
         declaration.attributes) {
        if (attribute.vendor_namespace != "bioseq_sql"
            || attribute.name != "result") {
            continue;
        }
        if (saw_result_annotation) {
            throw std::runtime_error(
                "SQL routine '" + declaration.qualified_name
                + "' has more than one bioseq_sql::result annotation");
        }
        saw_result_annotation = true;
        for (const option_value& option : parse_options(attribute)) {
            if (option.name == "sql_type") {
                result = option.expression.contains("type_of<")
                    ? option.expression
                    : detail::leaf_identifier(option.expression);
            } else {
                throw std::runtime_error(
                    "bioseq_sql::result has unknown option '."
                    + option.name + "'");
            }
        }
    }
    return result;
}

[[nodiscard]] inline std::string named_child_name(
    entity_kind kind,
    const structural_analysis::declaration_record& declaration,
    const std::vector<option_value>& options) {
    if (kind == entity_kind::access_method
        && declaration.name.ends_with("_handler")) {
        return declaration.name.substr(
            0, declaration.name.size() - std::string_view{"_handler"}.size());
    }
    if (kind == entity_kind::operator_) {
        for (const option_value& option : options) {
            if (option.name == "symbol") {
                return detail::unquoted_literal(option.expression);
            }
        }
    }
    if (kind == entity_kind::primary_key || kind == entity_kind::unique
        || kind == entity_kind::foreign_key || kind == entity_kind::check
        || kind == entity_kind::index) {
        for (const option_value& option : options) {
            if (option.name == "name") {
                return detail::unquoted_literal(option.expression);
            }
        }
    }
    return declaration.name;
}

[[nodiscard]] inline std::string entity_diagnostic(
    const std::filesystem::path& path,
    const structural_analysis::source_range& range) {
    return path.string() + ":" + std::to_string(range.begin.line)
        + ":" + std::to_string(range.begin.column);
}

inline void validate_annotation_target(
    entity_kind kind,
    const structural_analysis::declaration_record& declaration,
    const structural_analysis::attribute_record& attribute) {
    using structural_analysis::declaration_kind;
    const auto fail = [&attribute, &declaration] {
        throw std::runtime_error(
            "bioseq_sql::" + attribute.name + " is not valid on a "
            + structural_analysis::spell_declaration_kind(declaration.kind)
            + " declaration");
    };
    if ((kind == entity_kind::function
         || kind == entity_kind::procedure
         || kind == entity_kind::sql_function
         || kind == entity_kind::cast
         || kind == entity_kind::operator_
         || kind == entity_kind::type_role
         || kind == entity_kind::access_method
         || kind == entity_kind::catalog_dependencies)
        && declaration.kind != declaration_kind::function) {
        fail();
    }
    if ((kind == entity_kind::table || kind == entity_kind::type)
        && declaration.kind != declaration_kind::record
        && declaration.kind != declaration_kind::alias) {
        fail();
    }
    if (kind == entity_kind::column
        && declaration.kind != declaration_kind::field
        && declaration.kind != declaration_kind::variable) {
        fail();
    }
    if (kind == entity_kind::config_dump
        && declaration.kind != declaration_kind::record
        && declaration.kind != declaration_kind::alias) {
        fail();
    }
    if ((kind == entity_kind::aggregate
         || kind == entity_kind::opclass)
        && declaration.kind != declaration_kind::variable) {
        fail();
    }
    if (kind == entity_kind::sequence
        && declaration.kind != declaration_kind::variable) {
        fail();
    }
    if (kind == entity_kind::session_setting
        && declaration.kind != declaration_kind::variable) {
        fail();
    }
    if (kind == entity_kind::guc_value
        && attribute.placement
            != structural_analysis::attribute_placement::enumerator_tail) {
        throw std::runtime_error(
            "[BIOSEQ_SQL_E_GUC_ENUMERATOR_PLACEMENT] "
            "bioseq_sql::guc_value must follow the enumerator identifier");
    }
    if (kind == entity_kind::guc_value
        && declaration.kind != declaration_kind::enumerator) {
        fail();
    }
    if (kind == entity_kind::type
        && declaration.kind == declaration_kind::record
        && attribute.placement
            != structural_analysis::attribute_placement::class_head) {
        throw std::runtime_error(
            "[BIOSEQ_SQL_E_CLASS_HEAD_PLACEMENT] "
            "bioseq_sql::type on a class must use class-head placement");
    }
    if (kind == entity_kind::schema
        && declaration.kind != declaration_kind::namespace_declaration
        && declaration.kind != declaration_kind::variable) {
        fail();
    }
}

inline void validate_options(
    entity_kind kind,
    const std::vector<option_value>& options,
    const structural_analysis::attribute_record& attribute) {
    std::set<std::string, std::less<>> names;
    for (const option_value& option : options) {
        names.insert(option.name);
    }
    const auto one_of = [](std::string_view name,
                           std::initializer_list<std::string_view> allowed) {
        return std::ranges::find(allowed, name) != allowed.end();
    };
    const auto natural_option = [&](std::string_view name) {
        switch (kind) {
            case entity_kind::function:
                return one_of(name, {
                    "schema", "create", "volatility", "parallel",
                    "security", "leakproof", "cost", "rows",
                    "has_cost", "has_rows", "support",
                    "configuration", "null_input"});
            case entity_kind::procedure:
                return one_of(
                    name, {"schema", "create", "security"});
            case entity_kind::table:
                return one_of(name, {
                    "schema", "persistence", "create",
                    "access_method", "user_catalog_table",
                    "relation_options"});
            case entity_kind::column:
                return one_of(name, {
                    "sql_type", "default_value", "collation"});
            case entity_kind::schema:
                return one_of(name, {"create", "provision"});
            case entity_kind::sequence:
                return one_of(name, {
                    "schema", "create", "increment", "minimum",
                    "maximum", "cache", "cycle"});
            case entity_kind::view:
                return one_of(name, {
                    "schema", "create", "recursive", "temporary",
                    "security_barrier", "security_invoker"});
            case entity_kind::sql_function:
                return one_of(name, {
                    "schema", "create", "volatility", "parallel",
                    "security"});
            case entity_kind::operator_:
                return one_of(name, {
                    "symbol", "commutator", "negator", "restrict",
                    "join", "hashes", "merges"});
            case entity_kind::type:
                return one_of(name, {
                    "schema", "internal_length", "variable_length",
                    "passed_by_value", "alignment", "storage",
                    "category", "preferred", "delimiter",
                    "collatable", "element",
                    "default_value"});
            case entity_kind::type_role:
                return one_of(name, {"for_type"});
            case entity_kind::cast:
                return one_of(
                    name, {"assignment", "implicit", "inout", "binary"});
            case entity_kind::aggregate:
                return one_of(name, {
                    "transition", "final", "initial_condition",
                    "serial", "deserial", "state_space", "combine",
                    "final_function_extra", "final_function_modify",
                    "moving_transition", "moving_inverse",
                    "moving_final", "moving_state_space",
                    "moving_initial_condition",
                    "moving_final_function_extra",
                    "moving_final_function_modify", "sort_operator",
                    "parallel"});
            case entity_kind::opclass:
                return one_of(name, {
                    "access_method", "is_default", "storage",
                    "family", "operators", "functions"});
            case entity_kind::access_method:
                return one_of(name, {"kind"});
            case entity_kind::guc:
                return one_of(
                    name,
                    {"context", "flags", "minimum", "maximum",
                     "plan_cache"});
            case entity_kind::guc_value:
                return one_of(name, {"omit", "aliases"});
            case entity_kind::primary_key:
            case entity_kind::unique:
                return one_of(name, {"name", "placement", "columns"});
            case entity_kind::index:
                return one_of(name, {
                    "name", "access_method", "create", "unique",
                    "expression", "predicate"});
            case entity_kind::check:
                return one_of(
                    name, {"name", "expression", "placement"});
            case entity_kind::foreign_key:
                return one_of(name, {"name", "placement"});
            case entity_kind::grant:
                return one_of(name, {
                    "privileges", "to", "scope", "grant_option"});
            case entity_kind::revoke:
                return one_of(name, {
                    "privileges", "from", "scope", "grant_option"});
            case entity_kind::catalog_dependencies:
                return one_of(name, {"roots"});
            case entity_kind::comment:
            case entity_kind::session_setting:
            case entity_kind::config_dump:
            case entity_kind::phase:
                return false;
        }
        return false;
    };
    for (const option_value& option : options) {
        if (!natural_option(option.name)) {
            throw std::runtime_error(
                "bioseq_sql::" + attribute.name
                + " has unknown option '." + option.name + "'");
        }
    }
    if (routine_kind(kind)
        && (names.contains("name") || names.contains("c_symbol"))) {
        throw std::runtime_error(
            "bioseq_sql::" + attribute.name
            + " derives its SQL name and C symbol from the C++ declaration");
    }
    if (kind == entity_kind::aggregate
        && (names.contains("name") || names.contains("schema")
            || names.contains("arguments") || names.contains("state_type")
            || names.contains("implementation"))) {
        throw std::runtime_error(
            "bioseq_sql::aggregate contains redundant derived metadata");
    }
    if (kind == entity_kind::aggregate
        && !names.contains("transition")) {
        throw std::runtime_error(
            "bioseq_sql::aggregate requires a direct .transition "
            "function declaration");
    }
    if (kind == entity_kind::opclass
        && (names.contains("name") || names.contains("schema")
            || names.contains("indexed_type")
            || names.contains("implementation"))) {
        throw std::runtime_error(
            "bioseq_sql::opclass contains redundant derived metadata");
    }
    if (kind == entity_kind::opclass
        && (!names.contains("access_method")
            || !names.contains("operators")
            || !names.contains("functions"))) {
        throw std::runtime_error(
            "bioseq_sql::opclass requires direct .access_method, "
            ".operators, and .functions declarations");
    }
    if (kind == entity_kind::config_dump
        && !options.empty()) {
        throw std::runtime_error(
            "bioseq_sql::config_dump does not accept arguments");
    }
    if (kind == entity_kind::catalog_dependencies
        && !names.contains("roots")) {
        throw std::runtime_error(
            "bioseq_sql::am_catalog_dependencies requires typed .roots");
    }
}

[[nodiscard]] inline std::string compact_cpp_type(
        std::string_view cpp_type) {
    cpp_type = collector_trim(cpp_type);
    while (cpp_type.starts_with("const ")
           || cpp_type.starts_with("volatile ")) {
        cpp_type.remove_prefix(
            cpp_type.starts_with("const ") ? 6 : 9);
        cpp_type = collector_trim(cpp_type);
    }
    while (!cpp_type.empty()
           && (cpp_type.back() == '&'
               || collector_ascii_space(cpp_type.back()))) {
        cpp_type.remove_suffix(1);
    }
    std::string result;
    result.reserve(cpp_type.size());
    for (const unsigned char value : cpp_type) {
        if (!std::isspace(value)) {
            result.push_back(static_cast<char>(value));
        }
    }
    while (result.starts_with("::")) {
        result.erase(0, 2);
    }
    return result;
}

struct cpp_template_use {
    std::string outer;
    std::string argument;
    std::vector<std::string> arguments;
    bool valid{};
};

[[nodiscard]] inline cpp_template_use parse_cpp_template(
        std::string_view cpp_type) {
    const std::string compact = compact_cpp_type(cpp_type);
    const std::size_t open = compact.find('<');
    if (open == std::string::npos || compact.back() != '>') {
        return {};
    }
    int depth = 0;
    std::size_t close = std::string::npos;
    for (std::size_t index = open; index < compact.size(); ++index) {
        if (compact[index] == '<') {
            ++depth;
        } else if (compact[index] == '>') {
            --depth;
            if (depth == 0) {
                close = index;
                break;
            }
        }
    }
    if (close != compact.size() - 1 || depth != 0) {
        return {};
    }
    std::vector<std::string> arguments;
    std::size_t argument_begin = open + 1;
    depth = 0;
    for (std::size_t index = argument_begin; index <= close; ++index) {
        const char value =
            index == close ? ',' : compact[index];
        if (value == '<') {
            ++depth;
        } else if (value == '>') {
            --depth;
        } else if (value == ',' && depth == 0) {
            if (index == argument_begin) return {};
            arguments.push_back(
                compact.substr(argument_begin, index - argument_begin));
            argument_begin = index + 1;
        }
    }
    if (arguments.empty()) return {};
    return {
        .outer = compact.substr(0, open),
        .argument =
            arguments.size() == 1 ? arguments.front() : std::string{},
        .arguments = std::move(arguments),
        .valid = true,
    };
}

[[nodiscard]] inline std::string cpp_leaf_name(
        std::string_view cpp_name) {
    const std::size_t separator = cpp_name.rfind("::");
    return std::string{
        separator == std::string_view::npos
        ? cpp_name
        : cpp_name.substr(separator + 2)};
}

[[nodiscard]] inline routine_result_contract collect_result_contract(
        std::string_view cpp_return_type,
        std::string_view sql_override,
        std::string_view diagnostic) {
    std::string cpp_type = compact_cpp_type(cpp_return_type);
    cpp_template_use template_use = parse_cpp_template(cpp_type);
    if (template_use.valid
        && cpp_leaf_name(template_use.outer)
            == "sql_function_body") {
        cpp_type = template_use.argument;
        template_use = parse_cpp_template(cpp_type);
    }

    routine_result_contract result{
        .cpp_type = cpp_type,
    };
    if (cpp_type == "void") {
        result.form = routine_result_form::void_;
        result.sql_type =
            sql_override.empty()
            ? std::string{"void"}
            : std::string{sql_override};
        return result;
    }
    if (!template_use.valid) {
        result.form = routine_result_form::scalar;
        result.sql_type =
            sql_override.empty()
            ? sql_type_from_cpp(cpp_type)
            : std::string{sql_override};
        return result;
    }

    const std::string wrapper =
        cpp_leaf_name(template_use.outer);
    if (wrapper == "vector_result") {
        if (template_use.arguments.size() != 2) {
            throw std::runtime_error(
                std::string{diagnostic}
                + ": vector_result requires logical SQL and lane types");
        }
        if (!sql_override.empty()) {
            throw std::runtime_error(
                std::string{diagnostic}
                + ": vector_result derives its SQL result type from its "
                  "logical template argument");
        }
        result.form = routine_result_form::scalar;
        result.cpp_type = cpp_type;
        result.sql_type = sql_type_from_cpp(cpp_type);
        return result;
    }
    if (wrapper == "set_of") {
        result.form = routine_result_form::set_of;
        result.cpp_type = template_use.argument;
        result.sql_type =
            sql_override.empty()
            ? sql_type_from_cpp(template_use.argument)
            : std::string{sql_override};
        return result;
    }
    if (wrapper == "table_rows"
        || wrapper == "named_output"
        || wrapper == "named_output_set") {
        if (!sql_override.empty()) {
            throw std::runtime_error(
                std::string{diagnostic}
                + ": a row result wrapper derives its SQL shape from "
                  "the Row aggregate and cannot use bioseq_sql::result");
        }
        result.form =
            wrapper == "table_rows"
            ? routine_result_form::table_rows
            : wrapper == "named_output"
            ? routine_result_form::named_output
            : routine_result_form::named_output_set;
        result.cpp_type = template_use.argument;
        result.row_cpp_name = template_use.argument;
        result.sql_type =
            wrapper == "table_rows" ? "table" : "record";
        return result;
    }

    result.form = routine_result_form::scalar;
    result.sql_type =
        sql_override.empty()
        ? sql_type_from_cpp(cpp_type)
        : std::string{sql_override};
    return result;
}

[[nodiscard]] inline std::string field_sql_override(
    const structural_analysis::aggregate_field_record& field,
    const std::filesystem::path& source_path) {
    std::string result;
    bool saw_column = false;
    for (const structural_analysis::attribute_record& attribute :
         field.attributes) {
        if (attribute.vendor_namespace == "bioseq::sql") {
            throw std::runtime_error(
                entity_diagnostic(source_path, attribute.spelling_range)
                + ": [BIOSEQ_SQL_E_ATTRIBUTE_NAMESPACE] "
                  "invalid nested SQL attribute namespace '"
                + attribute.qualified_name
                + "'; use the independent bioseq_sql vendor namespace");
        }
        if (attribute.vendor_namespace != "bioseq_sql") continue;
        if (attribute.name != "column") {
            throw std::runtime_error(
                entity_diagnostic(source_path, attribute.spelling_range)
                + ": result-row fields only accept "
                  "bioseq_sql::column({.sql_type = ...})");
        }
        if (saw_column) {
            throw std::runtime_error(
                entity_diagnostic(source_path, attribute.spelling_range)
                + ": repeated bioseq_sql::column result-field override");
        }
        saw_column = true;
        const std::vector<option_value> options =
            parse_options(attribute);
        if (options.size() != 1
            || options.front().name != "sql_type") {
            throw std::runtime_error(
                entity_diagnostic(source_path, attribute.spelling_range)
                + ": result-row bioseq_sql::column accepts exactly "
                  "one .sql_type option");
        }
        const std::string& expression =
            options.front().expression;
        if (expression.contains("type_of<")) {
            const std::size_t open = expression.find('<');
            const std::size_t close = expression.rfind('>');
            if (open == std::string::npos
                || close == std::string::npos || close <= open) {
                throw std::runtime_error(
                    entity_diagnostic(source_path, attribute.spelling_range)
                    + ": malformed type_of result-field SQL type");
            }
            result = expression;
        } else {
            result = leaf_identifier(expression);
        }
        if (result == "double_precision") {
            result = "double precision";
        }
    }
    return result;
}

[[nodiscard]] inline bool valid_result_field_name(
        std::string_view name) noexcept {
    if (name.empty() || name.size() > 63
        || !((name.front() >= 'a' && name.front() <= 'z')
             || name.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(
        name.substr(1),
        [](char value) {
            return (value >= 'a' && value <= 'z')
                || (value >= '0' && value <= '9')
                || value == '_';
        });
}

struct aggregate_location {
    const structural_analysis::translation_unit* unit{};
    const structural_analysis::aggregate_record* aggregate{};
};

[[nodiscard]] inline aggregate_location resolve_result_row(
    std::string_view requested_name,
    const collected_entity& owner,
    const std::vector<structural_analysis::translation_unit>& units) {
    const bool globally_qualified = collector_trim(requested_name)
        .starts_with("::");
    std::string requested = compact_cpp_type(requested_name);
    const bool qualified = globally_qualified
        || requested.contains("::");
    std::string owner_scope = owner.cpp_qualified_name;
    if (const std::size_t separator = owner_scope.rfind("::");
        separator != std::string::npos) {
        owner_scope.erase(separator);
    } else {
        owner_scope.clear();
    }

    std::vector<std::string> candidates;
    candidates.push_back(requested);
    if (!qualified && !owner_scope.empty()) {
        candidates.push_back(owner_scope + "::" + requested);
    }
    for (const std::string& candidate : candidates) {
        aggregate_location match;
        for (const structural_analysis::translation_unit& unit : units) {
            for (const structural_analysis::aggregate_record& aggregate :
                 unit.aggregate_records) {
                if (aggregate.qualified_name != candidate) continue;
                if (match.aggregate) {
                    throw std::runtime_error(
                        entity_diagnostic(
                            owner.source_path, owner.source_range)
                        + ": result Row '" + requested
                        + "' has multiple aggregate definitions");
                }
                match = {.unit = &unit, .aggregate = &aggregate};
            }
        }
        if (match.aggregate) return match;
    }

    if (qualified) {
        throw std::runtime_error(
            entity_diagnostic(owner.source_path, owner.source_range)
            + ": result Row '" + requested
            + "' was not found in an imported .sql_rows module");
    }
    aggregate_location suffix_match;
    const std::string suffix = "::" + requested;
    for (const structural_analysis::translation_unit& unit : units) {
        for (const structural_analysis::aggregate_record& aggregate :
             unit.aggregate_records) {
            if (aggregate.qualified_name != requested
                && !aggregate.qualified_name.ends_with(suffix)) {
                continue;
            }
            if (suffix_match.aggregate) {
                throw std::runtime_error(
                    entity_diagnostic(
                        owner.source_path, owner.source_range)
                    + ": result Row '" + requested
                    + "' is ambiguous; use its qualified C++ name");
            }
            suffix_match = {
                .unit = &unit,
                .aggregate = &aggregate,
            };
        }
    }
    if (suffix_match.aggregate) return suffix_match;
    throw std::runtime_error(
        entity_diagnostic(owner.source_path, owner.source_range)
        + ": result Row '" + requested
        + "' was not found in an imported .sql_rows module");
}

[[nodiscard]] inline bool annotated_result_record(
    const aggregate_location& location) {
    return std::ranges::any_of(
        location.unit->declarations,
        [&location](
            const structural_analysis::declaration_record& declaration) {
            return declaration.kind
                    == structural_analysis::declaration_kind::record
                && declaration.qualified_name
                    == location.aggregate->qualified_name
                && std::ranges::any_of(
                    declaration.attributes,
                    [](const structural_analysis::attribute_record& attribute) {
                        return attribute.vendor_namespace
                            == "bioseq_sql";
                    });
        });
}

[[nodiscard]] inline bool result_row_module(
        std::string_view module_name) noexcept {
    return module_name.ends_with(".sql_rows")
        || module_name.ends_with(".rows");
}

[[nodiscard]] inline collected_result_row collect_result_row(
    const aggregate_location& location,
    const collected_entity& owner) {
    const structural_analysis::aggregate_record& aggregate =
        *location.aggregate;
    const std::string diagnostic =
        entity_diagnostic(owner.source_path, owner.source_range);
    if (!result_row_module(location.unit->module_name)) {
        throw std::runtime_error(
            diagnostic + ": result Row '" + aggregate.qualified_name
            + "' must be declared in a .sql_rows or .rows module");
    }
    if (!aggregate.exported) {
        throw std::runtime_error(
            diagnostic + ": result Row '" + aggregate.qualified_name
            + "' must be exported from its result-row module");
    }
    if (annotated_result_record(location)) {
        throw std::runtime_error(
            diagnostic + ": result Row '" + aggregate.qualified_name
            + "' must be unannotated; its wrapper template argument "
              "is the registration trigger");
    }
    if (!aggregate.structural_errors.empty()) {
        throw std::runtime_error(
            diagnostic + ": invalid result Row '"
            + aggregate.qualified_name + "': "
            + aggregate.structural_errors.front());
    }
    if (aggregate.fields.empty()) {
        throw std::runtime_error(
            diagnostic + ": result Row '" + aggregate.qualified_name
            + "' must contain at least one public data member");
    }

    collected_result_row result{
        .name = aggregate.name,
        .cpp_qualified_name = aggregate.qualified_name,
        .source_module = location.unit->module_name,
        .source_path = location.unit->source_path,
        .source_sha256 = location.unit->source_sha256,
        .source_range = aggregate.range,
    };
    std::set<std::string, std::less<>> folded_names;
    for (const structural_analysis::aggregate_field_record& field :
         aggregate.fields) {
        if (field.access
            != structural_analysis::member_access::public_access) {
            throw std::runtime_error(
                diagnostic + ": result Row field '"
                + field.qualified_name + "' must be public");
        }
        if (field.has_default_initializer) {
            throw std::runtime_error(
                diagnostic + ": result Row field '"
                + field.qualified_name
                + "' cannot have a default member initializer");
        }
        if (field.bit_field) {
            throw std::runtime_error(
                diagnostic + ": result Row field '"
                + field.qualified_name + "' cannot be a bit-field");
        }
        if (field.multiple_declarators) {
            throw std::runtime_error(
                diagnostic + ": result Row field declaration for '"
                + field.qualified_name
                + "' must declare exactly one field");
        }
        if (!valid_result_field_name(field.name)) {
            throw std::runtime_error(
                diagnostic + ": result Row field '" + field.name
                + "' must be a lowercase PostgreSQL identifier of "
                  "at most 63 bytes");
        }
        std::string folded = field.name;
        std::ranges::transform(
            folded,
            folded.begin(),
            [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        if (!folded_names.insert(folded).second) {
            throw std::runtime_error(
                diagnostic + ": result Row has colliding field name '"
                + field.name + "'");
        }
        const std::string override =
            field_sql_override(field, location.unit->source_path);
        if (override.empty()
            && field.cpp_type.contains("Datum")) {
            throw std::runtime_error(
                diagnostic + ": result Row field '"
                + field.qualified_name
                + "' uses ambiguous Datum and requires a .sql_type "
                  "override");
        }
        if (field.cpp_type.contains("string_view")) {
            throw std::runtime_error(
                diagnostic + ": result Row field '"
                + field.qualified_name
                + "' cannot use borrowed std::string_view storage");
        }
        result.fields.push_back(collected_result_field{
            .name = field.name,
            .cpp_type = field.cpp_type,
            .sql_type = override.empty()
                ? sql_type_from_cpp(field.cpp_type)
                : override,
            .nullable = field.cpp_type.contains("optional<"),
            .documentation = field.documentation,
            .source_range = field.range,
        });
    }
    return result;
}

[[nodiscard]] inline std::string table_rows_sql_type(
        const collected_result_row& row) {
    std::string result{"table ("};
    for (std::size_t index = 0; index < row.fields.size(); ++index) {
        if (index != 0) result += ", ";
        result += row.fields[index].name + " "
            + row.fields[index].sql_type;
    }
    result += ")";
    return result;
}

struct natural_session_setting_action {
    std::string setting_reference;
    std::string value_expression;
};

[[nodiscard]] inline bool direct_cpp_declaration_reference(
        std::string_view expression) noexcept {
    expression = collector_trim(expression);
    if (expression.starts_with("::")) {
        expression.remove_prefix(2);
    }
    if (expression.empty()) return false;
    bool expect_identifier = true;
    for (std::size_t index = 0; index < expression.size();) {
        if (expect_identifier) {
            const unsigned char first =
                static_cast<unsigned char>(expression[index]);
            if (!(std::isalpha(first) || expression[index] == '_')) {
                return false;
            }
            ++index;
            while (index < expression.size()) {
                const unsigned char value =
                    static_cast<unsigned char>(expression[index]);
                if (!(std::isalnum(value)
                      || expression[index] == '_')) {
                    break;
                }
                ++index;
            }
            expect_identifier = false;
            continue;
        }
        if (index + 1 >= expression.size()
            || expression[index] != ':'
            || expression[index + 1] != ':') {
            return false;
        }
        index += 2;
        expect_identifier = true;
    }
    return !expect_identifier;
}

[[nodiscard]] inline natural_session_setting_action
parse_natural_session_setting_action(
    const collected_entity& entity) {
    const std::string diagnostic =
        entity_diagnostic(entity.source_path, entity.source_range)
        + ": natural bioseq_sql::session_setting '"
        + entity.cpp_qualified_name + "'";
    const std::string_view initializer =
        collector_trim(entity.initializer_expression);
    if (initializer.empty()) {
        throw std::runtime_error(
            diagnostic
            + " requires a set_after_guard(setting, value) initializer");
    }
    const std::size_t open = initializer.find('(');
    if (open == std::string_view::npos
        || !encloses_entire_expression(
            initializer.substr(open), '(', ')')) {
        throw std::runtime_error(
            diagnostic
            + " requires one direct set_after_guard(setting, value) call");
    }
    const std::string callee =
        canonical_expression(initializer.substr(0, open));
    if (!direct_cpp_declaration_reference(callee)
        || cpp_leaf_name(callee) != "set_after_guard") {
        throw std::runtime_error(
            diagnostic
            + " requires one direct set_after_guard(setting, value) call");
    }
    std::vector<std::string> arguments;
    try {
        arguments = split_top_level(initializer.substr(
            open + 1, initializer.size() - open - 2));
    } catch (const std::exception& error) {
        throw std::runtime_error(
            diagnostic + " has a malformed set_after_guard call: "
            + error.what());
    }
    if (arguments.size() != 2) {
        throw std::runtime_error(
            diagnostic
            + " requires exactly two set_after_guard arguments");
    }
    std::string setting_reference =
        canonical_expression(arguments.front());
    if (!direct_cpp_declaration_reference(setting_reference)) {
        throw std::runtime_error(
            diagnostic
            + " requires a direct annotated GUC declaration reference");
    }
    std::string value_expression =
        canonical_expression(arguments.back());
    if (value_expression.empty()) {
        throw std::runtime_error(
            diagnostic + " requires a non-empty setting value");
    }
    return {
        .setting_reference = std::move(setting_reference),
        .value_expression = std::move(value_expression),
    };
}

inline void lower_natural_session_settings(
    std::vector<collected_entity>& entities) {
    for (collected_entity& entity : entities) {
        if (entity.kind != entity_kind::session_setting) {
            continue;
        }
        const std::string diagnostic =
            entity_diagnostic(entity.source_path, entity.source_range)
            + ": natural bioseq_sql::session_setting '"
            + entity.cpp_qualified_name + "'";
        if (find_option(entity, "value") != nullptr) {
            throw std::runtime_error(
                diagnostic
                + " cannot duplicate the value derived from "
                  "set_after_guard");
        }
        natural_session_setting_action action =
            parse_natural_session_setting_action(entity);
        const bool globally_qualified =
            action.setting_reference.starts_with("::");
        std::string normalized_reference =
            action.setting_reference;
        if (globally_qualified) {
            normalized_reference.erase(0, 2);
        }
        std::vector<const collected_entity*> matches;
        for (const collected_entity& candidate : entities) {
            if (candidate.kind != entity_kind::guc) {
                continue;
            }
            const bool exact =
                candidate.cpp_qualified_name == normalized_reference;
            const bool suffix =
                !globally_qualified
                && candidate.cpp_qualified_name.ends_with(
                    "::" + normalized_reference);
            if (exact || suffix) {
                matches.push_back(&candidate);
            }
        }
        if (matches.empty()) {
            throw std::runtime_error(
                diagnostic + " references unresolved natural GUC '"
                + action.setting_reference + "'");
        }
        if (matches.size() != 1) {
            throw std::runtime_error(
                diagnostic + " references ambiguous natural GUC '"
                + action.setting_reference
                + "'; use its globally qualified C++ name");
        }
        entity.name = guc_detail::natural_guc_name(
            matches.front()->cpp_qualified_name);
        entity.options.push_back(option_value{
            .name = "value",
            .expression = std::move(action.value_expression),
            .range = entity.source_range,
        });
    }
}

inline void bind_routine_companion_metadata(
    std::vector<collected_entity>& entities) {
    std::map<std::string, const collected_entity*, std::less<>>
        routines_by_cpp_name;
    for (const collected_entity& entity : entities) {
        if (!routine_kind(entity.kind)) {
            continue;
        }
        const auto [routine, inserted] =
            routines_by_cpp_name.emplace(
                entity.cpp_qualified_name, &entity);
        if (!inserted) {
            routine->second = nullptr;
        }
    }

    for (collected_entity& entity : entities) {
        if (entity.kind != entity_kind::cast
            && entity.kind != entity_kind::operator_) {
            continue;
        }
        const auto routine =
            routines_by_cpp_name.find(entity.cpp_qualified_name);
        if (routine == routines_by_cpp_name.end()) {
            throw std::runtime_error(
                entity_diagnostic(
                    entity.source_path, entity.source_range)
                + ": bioseq_sql::" + entity.annotation_name
                + " requires a co-located natural SQL routine "
                  "annotation");
        }
        if (routine->second == nullptr) {
            throw std::runtime_error(
                entity_diagnostic(
                    entity.source_path, entity.source_range)
                + ": bioseq_sql::" + entity.annotation_name
                + " resolves more than one co-located natural SQL "
                  "routine owner");
        }
        entity.schema = routine->second->schema;
        if (entity.kind == entity_kind::operator_) {
            entity.parameters = routine->second->parameters;
        }
    }
}

inline void rebind_operator_companion_parameters(
    std::vector<collected_entity>& entities) {
    std::map<std::string, const collected_entity*, std::less<>>
        routines_by_cpp_name;
    for (const collected_entity& entity : entities) {
        if (!routine_kind(entity.kind)) continue;
        const auto [owner, inserted] = routines_by_cpp_name.emplace(
            entity.cpp_qualified_name, &entity);
        if (!inserted) owner->second = nullptr;
    }
    for (collected_entity& entity : entities) {
        if (entity.kind != entity_kind::operator_) continue;
        const auto owner = routines_by_cpp_name.find(
            entity.cpp_qualified_name);
        if (owner == routines_by_cpp_name.end()
            || owner->second == nullptr) {
            throw std::runtime_error(
                "operator companion lost its unique natural routine owner");
        }
        entity.parameters = owner->second->parameters;
    }
}

[[nodiscard]] inline std::string resolve_type_of_sql_type(
    std::string_view sql_type,
    const std::vector<collected_entity>& entities,
    std::string_view diagnostic,
    std::string* stable_identity_sql_type = nullptr) {
    const std::size_t marker = sql_type.find("type_of<");
    if (marker == std::string_view::npos) {
        return std::string{sql_type};
    }
    const std::size_t begin =
        marker + std::string_view{"type_of<"}.size();
    int depth = 1;
    std::size_t end = std::string_view::npos;
    for (std::size_t index = begin; index < sql_type.size(); ++index) {
        if (sql_type[index] == '<') {
            ++depth;
        } else if (sql_type[index] == '>') {
            --depth;
            if (depth == 0) {
                end = index;
                break;
            }
        }
    }
    if (end == std::string_view::npos
        || !collector_trim(sql_type.substr(end + 1)).empty()) {
        throw std::runtime_error(
            std::string{diagnostic}
            + ": malformed bioseq::sql::type_of SQL type");
    }
    const std::string_view raw_reference = collector_trim(
        sql_type.substr(begin, end - begin));
    const bool globally_qualified = raw_reference.starts_with("::");
    std::string reference = compact_cpp_type(raw_reference);
    const bool qualified = globally_qualified
        || reference.contains("::");
    std::vector<const collected_entity*> matches;
    for (const collected_entity& candidate : entities) {
        if (candidate.kind != entity_kind::type) {
            continue;
        }
        const bool exact =
            candidate.cpp_qualified_name == reference;
        const bool suffix =
            !qualified
            && candidate.cpp_qualified_name.ends_with(
                "::" + reference);
        if (exact || suffix) {
            matches.push_back(&candidate);
        }
    }
    if (matches.empty()) {
        throw std::runtime_error(
            std::string{diagnostic}
            + ": unresolved bioseq::sql::type_of reference '"
            + reference + "'");
    }
    if (matches.size() != 1) {
        throw std::runtime_error(
            std::string{diagnostic}
            + ": ambiguous bioseq::sql::type_of reference '"
            + reference + "'; use its globally qualified C++ name");
    }
    const collected_entity& type = *matches.front();
    if (stable_identity_sql_type != nullptr) {
        *stable_identity_sql_type =
            type.schema + "." + type.name;
    }
    return type.schema == "$extension"
        ? type.name
        : type.schema + "." + type.name;
}

inline void resolve_type_of_sql_types(collected_program& program) {
    for (collected_result_row& row : program.result_rows) {
        for (collected_result_field& field : row.fields) {
            field.sql_type = resolve_type_of_sql_type(
                field.sql_type,
                program.entities,
                row.source_path.string() + ":"
                    + std::to_string(field.source_range.begin.line)
                    + ": result field '" + field.name + "'",
                &field.stable_identity_sql_type);
        }
    }

    for (collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::column) continue;
        const std::string declared = find_option(entity, "sql_type")
            ? option_literal(entity, "sql_type")
            : sql_type_from_cpp(entity.cpp_return_type);
        if (declared.contains("type_of<")) {
            entity.resolved_sql_type = resolve_type_of_sql_type(
                declared,
                program.entities,
                entity_diagnostic(entity.source_path, entity.source_range)
                    + ": SQL column '" + entity.name + "'");
        } else {
            entity.resolved_sql_type = leaf_identifier(declared);
            if (entity.resolved_sql_type == "double_precision") {
                entity.resolved_sql_type = "double precision";
            }
        }
    }

    for (collected_entity& entity : program.entities) {
        if (!routine_kind(entity.kind)) {
            continue;
        }
        const std::string diagnostic =
            entity_diagnostic(entity.source_path, entity.source_range)
            + ": SQL routine '" + entity.name + "'";
        for (routine_parameter& parameter : entity.parameters) {
            if (parameter.injected_call_context) continue;
            parameter.sql_type = resolve_type_of_sql_type(
                parameter.sql_type,
                program.entities,
                diagnostic + " parameter '" + parameter.name + "'",
                &parameter.stable_identity_sql_type);
        }
        entity.result.sql_type = resolve_type_of_sql_type(
            entity.result.sql_type,
            program.entities,
            diagnostic + " result",
            &entity.result.stable_identity_sql_type);
        switch (entity.result.form) {
            case routine_result_form::set_of:
                entity.result_sql_type =
                    "setof " + entity.result.sql_type;
                break;
            case routine_result_form::scalar:
            case routine_result_form::void_:
                entity.result_sql_type = entity.result.sql_type;
                break;
            case routine_result_form::table_rows: {
                const collected_result_row* row = nullptr;
                for (const collected_result_row& candidate :
                     program.result_rows) {
                    if (candidate.cpp_qualified_name
                        == entity.result.row_cpp_name) {
                        row = &candidate;
                        break;
                    }
                }
                if (row == nullptr) {
                    throw std::runtime_error(
                        diagnostic
                        + " has no registered result-row aggregate");
                }
                entity.result_sql_type = table_rows_sql_type(*row);
                entity.result.sql_type = entity.result_sql_type;
                break;
            }
            case routine_result_form::named_output_set:
                entity.result_sql_type = "setof record";
                entity.result.sql_type = "record";
                break;
            case routine_result_form::named_output:
                entity.result_sql_type = "record";
                entity.result.sql_type = "record";
                break;
        }
    }

}

}  // namespace detail

[[nodiscard]] inline std::string infer_sql_type_from_cpp(
    std::string_view cpp_type) {
    return detail::sql_type_from_cpp(cpp_type);
}

[[nodiscard]] inline collected_program collect_translation_units(
    std::vector<structural_analysis::translation_unit> translation_units) {
    collected_program result{
        .translation_units = std::move(translation_units)};
    for (const structural_analysis::translation_unit& unit :
         result.translation_units) {
        std::set<std::string, std::less<>> natural_row_records;
        for (const structural_analysis::declaration_record& declaration :
             unit.declarations) {
            if (declaration.kind
                != structural_analysis::declaration_kind::record) {
                continue;
            }
            const bool row_owner = std::ranges::any_of(
                declaration.attributes,
                [&unit](const structural_analysis::attribute_record& attribute) {
                    return attribute.vendor_namespace == "bioseq_sql"
                        && (attribute.name == "table"
                            || (attribute.name == "type"
                                && unit.module_name.ends_with(
                                    ".sql_rows")));
                });
            if (row_owner) {
                natural_row_records.insert(declaration.qualified_name);
            }
        }
        for (const structural_analysis::declaration_record& declaration :
             unit.declarations) {
            const bool natural_row_field =
                natural_row_records.contains(
                    declaration.enclosing_qualified_name);
            bool explicit_column_override = false;
            if (natural_row_field) {
                for (const structural_analysis::attribute_record& attribute :
                     declaration.attributes) {
                    if (attribute.vendor_namespace != "bioseq_sql"
                        || attribute.name != "column") {
                        continue;
                    }
                    if (parse_options(attribute).empty()) {
                        throw std::runtime_error(
                            detail::entity_diagnostic(
                                unit.source_path, attribute.spelling_range)
                            + ": redundant bioseq_sql::column marker on "
                              "an unambiguous natural row field");
                    }
                    explicit_column_override = true;
                }
                if (!explicit_column_override) {
                    result.entities.push_back(collected_entity{
                        .kind = entity_kind::column,
                        .annotation_name = "column",
                        .name = declaration.name,
                        .cpp_qualified_name =
                            declaration.qualified_name,
                        .enclosing_cpp_name =
                            declaration.enclosing_qualified_name,
                        .source_module = unit.module_name,
                        .source_path = unit.source_path,
                        .source_sha256 = unit.source_sha256,
                        .declaration_kind = declaration.kind,
                        .source_range = declaration.range,
                        .active_condition =
                            declaration.active_condition,
                        .schema = "$extension",
                        .cpp_signature =
                            declaration.signature_text,
                        .cpp_return_type =
                            declaration.return_type,
                        .initializer_expression =
                            declaration.initializer_text,
                        .documentation =
                            declaration.documentation,
                        .companion = true,
                    });
                }
            }
            std::vector<dependency_reference> declaration_dependencies;
            for (const structural_analysis::attribute_record& attribute :
                 declaration.attributes) {
                if (attribute.vendor_namespace == "bioseq::sql") {
                    throw std::runtime_error(
                        detail::entity_diagnostic(
                            unit.source_path, attribute.spelling_range)
                        + ": [BIOSEQ_SQL_E_ATTRIBUTE_NAMESPACE] "
                          "invalid nested SQL attribute namespace '"
                        + attribute.qualified_name
                        + "'; use the independent bioseq_sql vendor namespace");
                }
                if (attribute.vendor_namespace != "bioseq_sql") continue;
                if (attribute.name == "depends") {
                    const auto dependencies = parse_dependencies(attribute);
                    declaration_dependencies.insert(
                        declaration_dependencies.end(),
                        dependencies.begin(),
                        dependencies.end());
                }
            }

            std::map<std::string, std::string, std::less<>>
                annotation_arguments;
            for (const structural_analysis::attribute_record& attribute :
                 declaration.attributes) {
                if (attribute.vendor_namespace != "bioseq_sql"
                    || attribute.name == "depends"
                    || attribute.name == "param"
                    || attribute.name == "result") {
                    continue;
                }
                const std::optional<entity_kind> kind =
                    detail::annotation_entity_kind(attribute.name);
                if (!kind) {
                    throw std::runtime_error(
                        detail::entity_diagnostic(
                            unit.source_path, attribute.spelling_range)
                        + ": unsupported SQL annotation '"
                        + attribute.qualified_name + "'");
                }
                detail::validate_annotation_target(
                    *kind, declaration, attribute);
                std::vector<option_value> options =
                    parse_options(attribute);
                detail::validate_options(
                    *kind, options, attribute);
                const std::string canonical =
                    detail::canonical_expression(attribute.argument_text);
                std::string annotation_key = attribute.name;
                const bool repeatable_named_constraint =
                    *kind == entity_kind::unique
                    || *kind == entity_kind::foreign_key
                    || *kind == entity_kind::check
                    || *kind == entity_kind::index;
                if (repeatable_named_constraint) {
                    for (const option_value& option : options) {
                        if (option.name == "name") {
                            annotation_key += ":"
                                + detail::unquoted_literal(
                                    option.expression);
                            break;
                        }
                    }
                } else if (*kind == entity_kind::grant
                           || *kind == entity_kind::revoke) {
                    // One SQL object can own several distinct ACL effects
                    // (for example a schema-object revoke and a schema-wide
                    // table revoke). Canonical descriptor text distinguishes
                    // those companions while an exact duplicate remains an
                    // error below.
                    annotation_key += ":" + canonical;
                }
                const auto [previous, inserted] =
                    annotation_arguments.emplace(
                        std::move(annotation_key), canonical);
                if (!inserted) {
                    throw std::runtime_error(
                        detail::entity_diagnostic(
                            unit.source_path, attribute.spelling_range)
                        + (previous->second == canonical
                               ? ": redundant repeated SQL annotation '"
                               : ": conflicting repeated SQL annotation '")
                        + attribute.qualified_name + "'");
                }
                collected_entity entity{
                    .kind = *kind,
                    .annotation_name = attribute.name,
                    .name = detail::named_child_name(
                        *kind, declaration, options),
                    .cpp_qualified_name = declaration.qualified_name,
                    .enclosing_cpp_name =
                        declaration.enclosing_qualified_name,
                    .source_module = unit.module_name,
                    .source_path = unit.source_path,
                    .source_sha256 = unit.source_sha256,
                    .declaration_kind = declaration.kind,
                    .source_range = declaration.range,
                    .active_condition = declaration.active_condition,
                    .schema = derive_schema(options),
                    .cpp_signature = declaration.signature_text,
                    .cpp_return_type = declaration.return_type,
                    .initializer_expression =
                        declaration.initializer_text,
                    .options = std::move(options),
                    .dependencies = declaration_dependencies,
                    .documentation = declaration.documentation,
                    .public_documentation = std::ranges::contains(
                        declaration.documentation.groups,
                        std::string{"bioseq_sql_public_api"}),
                    .companion = detail::companion_kind(*kind)
                        || attribute.name == "am_catalog_dependencies",
                };
                if (detail::routine_kind(*kind)) {
                    for (const structural_analysis::parameter_record& parameter :
                         declaration.parameters) {
                        const detail::parameter_sql_contract contract =
                            detail::parameter_sql_override(parameter);
                        const bool injected_context =
                            detail::injected_call_context(
                                parameter.cpp_type);
                        if (contract.explicit_default
                            && parameter.has_default) {
                            throw std::runtime_error(
                                detail::entity_diagnostic(
                                    unit.source_path, parameter.range)
                                + ": SQL parameter '" + parameter.name
                                + "' explicit SQL default duplicates or "
                                  "conflicts with its C++ default expression");
                        }
                        entity.parameters.push_back(routine_parameter{
                            .name = parameter.name,
                            .cpp_type = parameter.cpp_type,
                            .sql_type = injected_context
                                ? std::string{}
                                : contract.sql_type.empty()
                                ? detail::sql_type_from_cpp(parameter.cpp_type)
                                : contract.sql_type,
                            .default_expression =
                                contract.explicit_default
                                ? *contract.explicit_default
                                : parameter.default_expression,
                            .has_default =
                                contract.explicit_default.has_value()
                                || parameter.has_default,
                            .nullable =
                                parameter.cpp_type.contains("optional<"),
                            .injected_call_context = injected_context,
                            .range = parameter.range,
                        });
                    }
                    const std::string result_override =
                        detail::result_sql_override(declaration);
                    entity.result =
                        detail::collect_result_contract(
                            declaration.return_type,
                            result_override,
                            detail::entity_diagnostic(
                                unit.source_path,
                                declaration.range));
                    switch (entity.result.form) {
                        case routine_result_form::set_of:
                            entity.result_sql_type =
                                "setof " + entity.result.sql_type;
                            break;
                        case routine_result_form::named_output_set:
                            entity.result_sql_type = "setof record";
                            break;
                        case routine_result_form::table_rows:
                            entity.result_sql_type = "table";
                            break;
                        case routine_result_form::named_output:
                            entity.result_sql_type = "record";
                            break;
                        case routine_result_form::scalar:
                        case routine_result_form::void_:
                            entity.result_sql_type =
                                entity.result.sql_type;
                            break;
                    }
                    for (const auto& documented :
                         declaration.documentation.parameters) {
                        if (!std::ranges::contains(
                                sql_parameters(entity),
                                documented.name,
                                [](const routine_parameter* parameter) {
                                    return parameter->name;
                                })) {
                            throw std::runtime_error(
                                detail::entity_diagnostic(
                                    unit.source_path, declaration.range)
                                + ": Doxygen @param '" + documented.name
                                + "' does not name a C++ SQL parameter");
                        }
                    }
                }
                result.entities.push_back(std::move(entity));
            }
        }
    }
    detail::bind_routine_companion_metadata(result.entities);
    detail::lower_natural_session_settings(result.entities);
    std::map<std::string, std::size_t, std::less<>>
        registered_result_rows;
    for (collected_entity& entity : result.entities) {
        if (!detail::routine_kind(entity.kind)) {
            continue;
        }
        const bool row_result =
            entity.result.form
                == routine_result_form::table_rows
            || entity.result.form
                == routine_result_form::named_output
            || entity.result.form
                == routine_result_form::named_output_set;
        if (!row_result) continue;

        const detail::aggregate_location location =
            detail::resolve_result_row(
                entity.result.row_cpp_name,
                entity,
                result.translation_units);
        const std::string& qualified =
            location.aggregate->qualified_name;
        auto [registered, inserted] =
            registered_result_rows.emplace(
                qualified, result.result_rows.size());
        if (inserted) {
            result.result_rows.push_back(
                detail::collect_result_row(location, entity));
        }
        entity.result.row_cpp_name = qualified;
        const collected_result_row& row =
            result.result_rows[registered->second];
        if (entity.result.form
            == routine_result_form::table_rows) {
            entity.result_sql_type =
                detail::table_rows_sql_type(row);
            entity.result.sql_type = entity.result_sql_type;
        } else if (entity.result.form
                   == routine_result_form::named_output_set) {
            entity.result_sql_type = "setof record";
            entity.result.sql_type = "record";
        } else {
            entity.result_sql_type = "record";
            entity.result.sql_type = "record";
        }
    }
    if (!registered_result_rows.empty()) {
        std::erase_if(
            result.entities,
            [&registered_result_rows](
                const collected_entity& entity) {
                return entity.kind == entity_kind::column
                    && registered_result_rows.contains(
                        entity.enclosing_cpp_name);
            });
    }
    detail::resolve_type_of_sql_types(result);
    detail::rebind_operator_companion_parameters(result.entities);
    if (result.entities.empty()) {
        throw std::runtime_error(
            "SQL source collection found no bioseq_sql annotations");
    }
    return result;
}

namespace detail {

[[nodiscard]] inline std::vector<
    structural_analysis::translation_unit>
collect_direct_import_enum_authorities(
    const collected_program& program,
    const structural_analysis::module_graph& module_graph,
    const std::vector<
        structural_analysis::dependency_module_identity>&
        dependency_modules) {
    std::set<std::string, std::less<>> guc_owner_modules;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind == entity_kind::guc) {
            guc_owner_modules.emplace(entity.source_module);
        }
    }

    std::set<std::string, std::less<>> direct_imports;
    for (const std::string& owner : guc_owner_modules) {
        const auto graph_unit = std::ranges::find(
            module_graph.units,
            owner,
            &structural_analysis::module_graph_unit::provided_module);
        if (graph_unit == module_graph.units.end()) {
            throw std::runtime_error(
                "registered GUC owner module '" + owner
                + "' is absent from the validated module graph");
        }
        direct_imports.insert(
            graph_unit->exported_imports.begin(),
            graph_unit->exported_imports.end());
        direct_imports.insert(
            graph_unit->internal_imports.begin(),
            graph_unit->internal_imports.end());
    }

    std::map<
        std::string,
        const structural_analysis::dependency_module_identity*,
        std::less<>>
        dependency_by_module;
    for (const auto& dependency : dependency_modules) {
        if (!dependency_by_module.emplace(
                dependency.module_name, &dependency).second) {
            throw std::runtime_error(
                "validated PREPARE module closure duplicates module '"
                + dependency.module_name + "'");
        }
    }

    std::vector<structural_analysis::translation_unit> result;
    result.reserve(direct_imports.size());
    for (const std::string& imported : direct_imports) {
        const auto dependency = dependency_by_module.find(imported);
        if (dependency == dependency_by_module.end()) {
            throw std::runtime_error(
                "registered GUC owner direct import '" + imported
                + "' is absent from the validated PREPARE module closure");
        }
        structural_analysis::translation_unit authority =
            structural_analysis::scan_token_slice(
                dependency->second->source_path);
        if (authority.module_name != imported
            || authority.source_sha256
                != dependency->second->source_sha256) {
            throw std::runtime_error(
                "registered GUC owner direct import '" + imported
                + "' disagrees with its validated PREPARE source authority");
        }
        result.push_back(std::move(authority));
    }
    return result;
}

}  // namespace detail

[[nodiscard]] inline collected_program collect_sources(
    const std::vector<std::filesystem::path>& source_paths,
    const source_collection_environment& environment) {
    if (source_paths.empty()) {
        throw std::runtime_error(
            "SQL source collection requires at least one .cppm owner");
    }
    for (const std::filesystem::path& path : source_paths) {
        if (path.extension() != ".cppm") {
            throw std::runtime_error(
                "SQL owner source must be an authored .cppm module: "
                + path.string());
        }
    }
    if (environment.compile_database.empty()
        || environment.pcm_root.empty()
        || environment.compiler_profile.empty()
        || environment.dependency_modules.empty()) {
        throw std::runtime_error(
            "production SQL source collection requires an explicit "
            "Clang compile database, PCM root, compiler profile, and "
            "validated dependency-module closure");
    }
    try {
        structural_analysis::module_graph module_graph =
            structural_analysis::scan_module_graph_from_database(
                source_paths,
                environment.compile_database,
                environment.compiler_profile,
                environment.pcm_root);
        if (module_graph.units.size() != source_paths.size()) {
            throw std::runtime_error(
                "Clang module-graph pass did not cover every registered "
                "SQL owner");
        }
        std::set<std::string, std::less<>> provided_modules;
        for (const structural_analysis::module_graph_unit& unit :
             module_graph.units) {
            if (unit.provided_module.empty()) {
                throw std::runtime_error(
                    unit.source_identity
                    + ": registered SQL owner does not provide a named "
                      "module");
            }
            if (!provided_modules.emplace(unit.provided_module).second) {
                throw std::runtime_error(
                    "registered SQL owner module appears more than once: "
                    + unit.provided_module);
            }
        }
        structural_analysis::structural_index index =
            structural_analysis::scan_translation_units_from_database(
                source_paths,
                environment.compile_database,
                environment.compiler_profile,
                environment.pcm_root);
        if (module_graph.clang_release != index.clang_release
            || module_graph.compilation_signature
                != index.compilation_signature) {
            throw std::runtime_error(
                "Clang structural and module-graph passes disagree on "
                "their compiler environment");
        }
        if (index.translation_units.size() != source_paths.size()) {
            throw std::runtime_error(
                "Clang structural pass did not cover every registered SQL "
                "owner");
        }
        collected_program result =
            collect_translation_units(
                std::move(index.translation_units));
        result.enum_authority_units =
            detail::collect_direct_import_enum_authorities(
                result,
                module_graph,
                environment.dependency_modules);
        result.module_graph = std::move(module_graph);
        result.compiler_profile =
            environment.compiler_profile;
        result.compilation_signature =
            std::move(index.compilation_signature);
        return result;
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Clang SQL source collection failed for profile '"
            + environment.compiler_profile + "': " + error.what());
    }
}

/// Source-only fixture for contract tests that exercise lexical collection.
/// Production generation never calls this entry point.
[[nodiscard]] inline collected_program collect_fixture_sources(
    const std::vector<std::filesystem::path>& source_paths,
    const std::vector<std::filesystem::path>&
        enum_authority_paths = {}) {
    std::vector<structural_analysis::translation_unit> units;
    units.reserve(source_paths.size());
    std::vector<structural_analysis::translation_unit>
        enum_authority_units;
    enum_authority_units.reserve(
        source_paths.size() + enum_authority_paths.size());
    structural_analysis::module_graph module_graph;
    module_graph.units.reserve(source_paths.size());
    std::set<std::string, std::less<>> provided_modules;
    for (const std::filesystem::path& path : source_paths) {
        structural_analysis::translation_unit semantic =
            structural_analysis::scan_translation_unit(path);
        structural_analysis::translation_unit authority =
            structural_analysis::scan_token_slice(path);
        if (authority.module_name.empty()
            || semantic.module_name != authority.module_name
            || semantic.source_sha256 != authority.source_sha256
            || !provided_modules.emplace(authority.module_name).second) {
            throw std::runtime_error(
                path.string()
                + ": fixture SQL owner has inconsistent or duplicate "
                  "module authority");
        }
        std::vector<std::string> internal_imports =
            authority.source_module.imported_modules;
        std::ranges::sort(internal_imports);
        internal_imports.erase(
            std::ranges::unique(internal_imports).begin(),
            internal_imports.end());
        module_graph.units.push_back({
            .source_identity =
                structural_analysis::repository_source_identity(
                    authority.source_path),
            .provided_module = authority.module_name,
            .internal_imports = std::move(internal_imports),
        });
        units.push_back(std::move(semantic));
        enum_authority_units.push_back(std::move(authority));
    }
    for (const std::filesystem::path& path : enum_authority_paths) {
        structural_analysis::translation_unit authority =
            structural_analysis::scan_token_slice(path);
        if (authority.module_name.empty()
            || !std::ranges::any_of(
                module_graph.units,
                [&](const structural_analysis::module_graph_unit& owner) {
                    return std::ranges::contains(
                               owner.exported_imports,
                               authority.module_name)
                        || std::ranges::contains(
                               owner.internal_imports,
                               authority.module_name);
                })) {
            throw std::runtime_error(
                path.string()
                + ": fixture enum authority must be a direct import of a "
                  "registered SQL owner");
        }
        enum_authority_units.push_back(std::move(authority));
    }
    collected_program result =
        collect_translation_units(std::move(units));
    result.enum_authority_units =
        std::move(enum_authority_units);
    result.module_graph = std::move(module_graph);
    return result;
}

[[nodiscard]] inline collected_program collect_source_text(
    std::filesystem::path source_path,
    std::string_view source) {
    std::vector<structural_analysis::translation_unit> units;
    units.push_back(
        structural_analysis::scan_translation_unit(source_path, source));
    collected_program result =
        collect_translation_units(std::move(units));
    structural_analysis::translation_unit authority =
        structural_analysis::scan_token_slice(source_path, source);
    result.module_graph.units.push_back({
        .source_identity =
            structural_analysis::repository_source_identity(
                authority.source_path),
        .provided_module = authority.module_name,
        .internal_imports =
            authority.source_module.imported_modules,
    });
    result.enum_authority_units.push_back(std::move(authority));
    return result;
}

[[nodiscard]] inline bool contains_structural_terminal_identifier(
    const structural_analysis::translation_unit& unit,
    std::string_view identifier) {
    bool module_or_import_name = false;
    for (const structural_analysis::lexical_token& token : unit.tokens) {
        if (module_or_import_name) {
            if (token.spelling == ";") {
                module_or_import_name = false;
            }
            continue;
        }
        if (token.spelling == "module" || token.spelling == "import") {
            module_or_import_name = true;
            continue;
        }
        if (token.spelling == identifier) {
            return true;
        }
    }
    return false;
}

/**
 * Run the structural scanner, enforce path-owned authority for generated-only
 * PostgreSQL boundary capabilities, then collect SQL annotations. Authority is
 * checked before collection so an otherwise annotation-free source cannot
 * bypass the generated-path contract. Terminal identifiers are resolved from
 * the scanner's comment/string-free model, excluding module and import names,
 * so namespace aliases, using declarations, and local type aliases cannot
 * conceal a reserved capability.
 */
[[nodiscard]] inline collected_program
collect_provenance_checked_fixture(
    const std::filesystem::path& source_path,
    const std::vector<std::filesystem::path>& allowed_sources) {
    if (!std::filesystem::is_regular_file(source_path)) {
        throw std::runtime_error(
            "provenance fixture is not a regular file: "
            + source_path.string());
    }
    structural_analysis::translation_unit unit =
        structural_analysis::scan_translation_unit(source_path);
    const bool uses_catalog_handle =
        contains_structural_terminal_identifier(
            unit, "relation_entity_handle");
    const bool uses_guc_access =
        contains_structural_terminal_identifier(
            unit, "postgres_enum_registration_access");
    const bool uses_guc_registration =
        contains_structural_terminal_identifier(
            unit, "postgres_enum_registration");
    const std::filesystem::path canonical_source =
        std::filesystem::canonical(source_path);
    const bool authorized = std::ranges::any_of(
        allowed_sources,
        [&canonical_source](const std::filesystem::path& allowed) {
            return std::filesystem::is_regular_file(allowed)
                && std::filesystem::canonical(allowed)
                    == canonical_source;
        });
    if (!authorized && uses_catalog_handle) {
        throw std::runtime_error(
            "[BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE] "
            "catalog-handle specialization lacks configured generated-path "
            "authority");
    }
    if (!authorized && uses_guc_access) {
        throw std::runtime_error(
            "[BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE] "
            "GUC registration access lacks configured generated-path "
            "authority");
    }
    if (!authorized && uses_guc_registration) {
        throw std::runtime_error(
            "[BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE] "
            "PostgreSQL enum registration lacks configured generated-path "
            "authority");
    }
    std::vector<structural_analysis::translation_unit> units;
    units.push_back(std::move(unit));
    return collect_translation_units(std::move(units));
}

}  // namespace bioseq::codegen::sql
