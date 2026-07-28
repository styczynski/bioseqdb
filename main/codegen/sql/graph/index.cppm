module;

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.graph;

export import :model;
export import :compatibility;
export import :fmgr;
import bioseq.codegen.clang_structural;
import bioseq.codegen.sql.collector;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline bool sql_identifier(std::string_view value) noexcept {
    if (value.empty() || value.size() > 63) return false;
    if (!((value.front() >= 'a' && value.front() <= 'z')
          || value.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(
        value.substr(1),
        [](char item) {
            return (item >= 'a' && item <= 'z')
                || (item >= '0' && item <= '9') || item == '_';
        });
}

[[nodiscard]] inline bool sql_guc_name(std::string_view value) noexcept {
    bool saw_separator = false;
    while (!value.empty()) {
        const std::size_t separator = value.find('.');
        const std::string_view component =
            separator == std::string_view::npos
            ? value
            : value.substr(0, separator);
        if (!sql_identifier(component)) return false;
        if (separator == std::string_view::npos) break;
        saw_separator = true;
        value.remove_prefix(separator + 1);
        if (value.empty()) return false;
    }
    return saw_separator;
}

[[nodiscard]] inline bool reserved_sql_identifier(
    std::string_view value) noexcept {
    static constexpr std::string_view reserved[] = {
        "all", "analyse", "analyze", "and", "any", "array", "as",
        "asc", "asymmetric", "authorization", "binary", "both",
        "case", "cast", "check", "collate", "collation", "column",
        "concurrently", "constraint", "create", "cross", "current_catalog",
        "current_date", "current_role", "current_schema", "current_time",
        "current_timestamp", "current_user", "default", "deferrable",
        "desc", "distinct", "do", "else", "end", "except", "false",
        "fetch", "for", "foreign", "freeze", "from", "full", "grant",
        "group", "having", "ilike", "in", "initially", "inner",
        "intersect", "into", "is", "isnull", "join", "lateral", "leading",
        "left", "like", "limit", "localtime", "localtimestamp", "natural",
        "not", "notnull", "null", "offset", "on", "only", "or", "order",
        "outer", "overlaps", "placing", "primary", "references", "returning",
        "right", "select", "session_user", "similar", "some", "symmetric",
        "system_user", "table", "tablesample", "then", "to", "trailing",
        "true", "union", "unique", "user", "using", "variadic", "verbose",
        "when", "where", "window", "with"};
    return std::ranges::binary_search(reserved, value);
}

[[nodiscard]] inline bool punctuation_name(entity_kind kind) noexcept {
    return kind == entity_kind::operator_;
}

struct sequence_descriptor {
    std::string schema;
    std::string name;
    std::string data_type{"bigint"};
    std::int64_t start{1};
    std::int64_t increment{1};
    std::int64_t minimum{1};
    std::int64_t maximum{std::numeric_limits<std::int64_t>::max()};
    std::int64_t cache{1};
    bool cycle{};
    bool if_not_exists{};

    [[nodiscard]] bool operator==(
        const sequence_descriptor&) const = default;
};

[[nodiscard]] inline std::string graph_unquoted_literal(
    std::string_view value) {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\n' || value.front() == '\r')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\n' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    if (value.size() >= 2
        && ((value.front() == '"' && value.back() == '"')
            || (value.front() == '\'' && value.back() == '\''))) {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return std::string{value};
}

[[nodiscard]] inline std::string graph_enum_leaf(
    std::string_view value) {
    while (!value.empty() && value.back() == '}') value.remove_suffix(1);
    const std::size_t separator = value.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? value
            : value.substr(separator + 2)};
}

[[nodiscard]] inline std::int64_t sequence_integer(
    std::string_view value,
    std::string_view field) {
    const std::string normalized = graph_unquoted_literal(value);
    std::int64_t result{};
    const auto [end, error] = std::from_chars(
        normalized.data(), normalized.data() + normalized.size(), result);
    if (error != std::errc{}
        || end != normalized.data() + normalized.size()) {
        throw std::runtime_error(
            "sequence ." + std::string{field}
            + " must be a closed signed 64-bit integer");
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string> sequence_tokens(
    std::string_view sql_identity) {
    std::vector<std::string> result;
    std::string current;
    const auto flush = [&current, &result] {
        if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    };
    for (const char value : sql_identity) {
        if (value == ' ' || value == '\t' || value == '\n'
            || value == '\r' || value == ';') {
            flush();
        } else {
            current.push_back(value);
        }
    }
    flush();
    return result;
}

[[nodiscard]] inline sequence_descriptor typed_sequence_descriptor(
    const collected_entity& entity) {
    const option_value* identity_option =
        find_option(entity, "sql_identity");
    if (!identity_option) {
        throw std::runtime_error(
            "typed sequence install owner has no SQL identity");
    }
    const std::string sql_identity =
        graph_unquoted_literal(identity_option->expression);
    const std::vector<std::string> tokens =
        sequence_tokens(sql_identity);
    if (tokens.size() < 3 || tokens[0] != "create"
        || tokens[1] != "sequence") {
        throw std::runtime_error(
            "typed sequence SQL identity is not CREATE SEQUENCE: "
            + sql_identity);
    }
    sequence_descriptor result;
    std::size_t cursor = 2;
    if (cursor + 2 < tokens.size() && tokens[cursor] == "if"
        && tokens[cursor + 1] == "not"
        && tokens[cursor + 2] == "exists") {
        result.if_not_exists = true;
        cursor += 3;
    }
    if (cursor >= tokens.size()) {
        throw std::runtime_error(
            "typed sequence SQL identity has no qualified name");
    }
    const std::string& qualified = tokens[cursor++];
    const std::size_t separator = qualified.find('.');
    if (separator == std::string::npos
        || qualified.find('.', separator + 1) != std::string::npos) {
        throw std::runtime_error(
            "typed sequence SQL identity must use one fixed schema");
    }
    result.schema = qualified.substr(0, separator);
    result.name = qualified.substr(separator + 1);

    bool explicit_start = false;
    bool explicit_minimum = false;
    bool explicit_maximum = false;
    while (cursor < tokens.size()) {
        const std::string& token = tokens[cursor++];
        if (token == "as") {
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence AS clause has no data type");
            }
            result.data_type = tokens[cursor++];
        } else if (token == "increment") {
            if (cursor < tokens.size() && tokens[cursor] == "by") ++cursor;
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence INCREMENT clause has no value");
            }
            result.increment =
                sequence_integer(tokens[cursor++], "increment");
        } else if (token == "minvalue") {
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence MINVALUE clause has no value");
            }
            result.minimum =
                sequence_integer(tokens[cursor++], "minimum");
            explicit_minimum = true;
        } else if (token == "maxvalue") {
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence MAXVALUE clause has no value");
            }
            result.maximum =
                sequence_integer(tokens[cursor++], "maximum");
            explicit_maximum = true;
        } else if (token == "start") {
            if (cursor < tokens.size() && tokens[cursor] == "with") ++cursor;
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence START clause has no value");
            }
            result.start = sequence_integer(tokens[cursor++], "start");
            explicit_start = true;
        } else if (token == "cache") {
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence CACHE clause has no value");
            }
            result.cache = sequence_integer(tokens[cursor++], "cache");
        } else if (token == "cycle") {
            result.cycle = true;
        } else if (token == "no") {
            if (cursor >= tokens.size()) {
                throw std::runtime_error(
                    "typed sequence NO clause has no subject");
            }
            const std::string& subject = tokens[cursor++];
            if (subject == "cycle") {
                result.cycle = false;
            } else if (subject == "minvalue") {
                result.minimum = result.increment > 0
                    ? 1
                    : std::numeric_limits<std::int64_t>::min();
                explicit_minimum = true;
            } else if (subject == "maxvalue") {
                result.maximum = result.increment > 0
                    ? std::numeric_limits<std::int64_t>::max()
                    : -1;
                explicit_maximum = true;
            } else {
                throw std::runtime_error(
                    "unsupported typed sequence NO " + subject
                    + " clause");
            }
        } else {
            throw std::runtime_error(
                "unsupported typed sequence descriptor token '" + token
                + "'");
        }
    }
    if (result.data_type != "bigint") {
        throw std::runtime_error(
            "generated relation handles currently require BIGINT sequences");
    }
    if (!explicit_minimum) {
        result.minimum = result.increment > 0
            ? 1
            : std::numeric_limits<std::int64_t>::min();
    }
    if (!explicit_maximum) {
        result.maximum = result.increment > 0
            ? std::numeric_limits<std::int64_t>::max()
            : -1;
    }
    if (!explicit_start) {
        result.start =
            result.increment > 0 ? result.minimum : result.maximum;
    }
    return result;
}

[[nodiscard]] inline sequence_descriptor natural_sequence_descriptor(
    const collected_entity& entity) {
    sequence_descriptor result{
        .schema = entity.schema,
        .name = entity.name,
    };
    const std::string cpp_sql_type =
        infer_sql_type_from_cpp(entity.cpp_return_type);
    if (cpp_sql_type != "smallint"
        && cpp_sql_type != "integer"
        && cpp_sql_type != "bigint") {
        throw std::runtime_error(
            "natural sequence '" + result.schema + "." + result.name
            + "' requires a signed 16-, 32-, or 64-bit integral C++ "
              "declaration");
    }
    result.data_type = cpp_sql_type;
    if (entity.initializer_expression.empty()) {
        throw std::runtime_error(
            "natural sequence '" + result.schema + "." + result.name
            + "' requires a C++ initializer that supplies START");
    }
    result.start =
        sequence_integer(entity.initializer_expression, "start");
    if (const option_value* value = find_option(entity, "create")) {
        result.if_not_exists =
            graph_enum_leaf(value->expression) == "if_not_exists";
    }
    if (const option_value* value = find_option(entity, "increment")) {
        result.increment =
            sequence_integer(value->expression, "increment");
    }
    if (const option_value* value = find_option(entity, "minimum")) {
        result.minimum =
            sequence_integer(value->expression, "minimum");
    } else {
        result.minimum = result.increment > 0
            ? 1
            : std::numeric_limits<std::int64_t>::min();
    }
    if (const option_value* value = find_option(entity, "maximum")) {
        result.maximum =
            sequence_integer(value->expression, "maximum");
    } else {
        result.maximum = result.increment > 0
            ? std::numeric_limits<std::int64_t>::max()
            : -1;
    }
    if (const option_value* value = find_option(entity, "cache")) {
        result.cache = sequence_integer(value->expression, "cache");
    }
    if (const option_value* value = find_option(entity, "cycle")) {
        result.cycle = graph_enum_leaf(value->expression) == "true";
    }
    return result;
}

[[nodiscard]] inline std::string sequence_target_key(
    const sequence_descriptor& descriptor) {
    return descriptor.schema + "." + descriptor.name;
}

inline void reconcile_natural_sequence_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>> natural_by_target;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::sequence
            || entity.typed_install_owner) {
            continue;
        }
        if (!entity.active_condition.empty()) {
            throw std::runtime_error(
                entity.source_path.string() + ":"
                + std::to_string(entity.source_range.begin.line)
                + ": generated sequence entity handles require an "
                  "unconditional natural owner; profile condition was '"
                + entity.active_condition + "'");
        }
        const sequence_descriptor descriptor =
            natural_sequence_descriptor(entity);
        const std::string target = sequence_target_key(descriptor);
        if (!natural_by_target.emplace(target, index).second) {
            throw std::runtime_error(
                "duplicate natural sequence owner for '" + target + "'");
        }
    }

    std::set<std::size_t> merged_install_owners;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::sequence
            || !install.typed_install_owner) {
            continue;
        }
        const sequence_descriptor install_descriptor =
            typed_sequence_descriptor(install);
        const auto natural_position = natural_by_target.find(
            sequence_target_key(install_descriptor));
        if (natural_position == natural_by_target.end()) continue;
        collected_entity& natural =
            program.entities[natural_position->second];
        const sequence_descriptor natural_descriptor =
            natural_sequence_descriptor(natural);
        if (natural_descriptor != install_descriptor) {
            throw std::runtime_error(
                "natural sequence owner '" + natural_descriptor.schema
                + "." + natural_descriptor.name
                + "' disagrees with its typed install-owner descriptor");
        }
        if (!natural.physical_statements.empty()
            || !natural.compatibility_sql_initializer.empty()) {
            throw std::runtime_error(
                "natural sequence owner '" + natural_descriptor.schema
                + "." + natural_descriptor.name
                + "' matches more than one typed install owner");
        }
        adopt_typed_install_statement(
            natural,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }

    if (merged_install_owners.empty()) return;
    std::vector<collected_entity> reconciled;
    reconciled.reserve(
        program.entities.size() - merged_install_owners.size());
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        if (!merged_install_owners.contains(index)) {
            reconciled.push_back(std::move(program.entities[index]));
        }
    }
    program.entities = std::move(reconciled);
}

[[nodiscard]] inline std::vector<std::string>
table_expression_identifiers(std::string_view expression) {
    std::vector<std::string> result;
    std::string current;
    const auto flush = [&current, &result] {
        if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    };
    for (const char value : expression) {
        if ((value >= 'a' && value <= 'z')
            || (value >= 'A' && value <= 'Z')
            || (value >= '0' && value <= '9') || value == '_') {
            current.push_back(value);
        } else {
            flush();
        }
    }
    flush();
    return result;
}

[[nodiscard]] inline std::string table_sql_type(
    const collected_entity& column) {
    std::string result;
    if (const option_value* sql_type =
            find_option(column, "sql_type")) {
        result = leaf_identifier(sql_type->expression);
    } else {
        result = infer_sql_type_from_cpp(column.cpp_return_type);
    }
    if (result == "integer") return "int";
    if (result == "smallint") return "smallint";
    if (result == "bigint") return "bigint";
    if (result == "boolean") return "boolean";
    return result;
}

[[nodiscard]] inline std::string canonical_catalog_sql_type(
    std::string_view value) {
    std::string result =
        fmgr_reconciliation_detail::canonical_sql_type(value);
    if (result == "double_precision") return "doubleprecision";
    if (result == "timestampwithtimezone") return "timestamptz";
    return result;
}

[[nodiscard]] inline std::string canonical_catalog_sql_fragment(
    std::string_view value) {
    return fmgr_reconciliation_detail::canonical_sql_fragment(value);
}

[[nodiscard]] inline std::string canonical_catalog_checked_sql(
    std::string_view value) {
    return fmgr_reconciliation_detail::canonical_default(value);
}

[[nodiscard]] inline std::string natural_column_initializer_sql(
    const collected_entity& column) {
    if (column.initializer_expression == "std::string{}"
        || column.initializer_expression == "std::string()") {
        return "text ''";
    }
    return column.initializer_expression;
}

[[nodiscard]] inline std::string catalog_option_identifier(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback) {
    const option_value* option = find_option(entity, name);
    if (option == nullptr) return std::string{fallback};
    std::string result =
        graph_unquoted_literal(option->expression);
    if (result.contains("::")) result = graph_enum_leaf(result);
    std::ranges::transform(
        result,
        result.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    return result;
}

[[nodiscard]] inline bool natural_child_of(
    const collected_entity& child,
    const collected_entity& owner) noexcept {
    return child.cpp_qualified_name == owner.cpp_qualified_name
        || child.enclosing_cpp_name == owner.cpp_qualified_name;
}

struct catalog_column_descriptor {
    std::string name;
    std::string sql_type;
    std::optional<std::string> default_value;
    bool nullable{true};

    [[nodiscard]] bool operator==(
        const catalog_column_descriptor&) const = default;
};

struct catalog_key_descriptor {
    std::optional<std::string> name;
    std::vector<std::string> columns;

    [[nodiscard]] bool operator==(
        const catalog_key_descriptor&) const = default;
};

struct catalog_check_descriptor {
    std::optional<std::string> name;
    std::string expression;

    [[nodiscard]] bool operator==(
        const catalog_check_descriptor&) const = default;
};

struct table_descriptor {
    std::string target;
    std::string persistence{"ordinary"};
    std::string access_method{"default"};
    std::vector<catalog_column_descriptor> columns;
    std::optional<catalog_key_descriptor> primary_key;
    std::vector<catalog_key_descriptor> unique_constraints;
    std::vector<catalog_check_descriptor> checks;
    bool if_not_exists{};
    bool user_catalog_table{};

    [[nodiscard]] bool operator==(
        const table_descriptor&) const = default;
};

inline void normalize_table_descriptor(table_descriptor& descriptor) {
    if (descriptor.primary_key) {
        for (const std::string& primary_column :
             descriptor.primary_key->columns) {
            const auto column = std::ranges::find(
                descriptor.columns,
                primary_column,
                &catalog_column_descriptor::name);
            if (column == descriptor.columns.end()) {
                throw std::runtime_error(
                    "primary key for '" + descriptor.target
                    + "' names unknown column '" + primary_column + "'");
            }
            column->nullable = false;
        }
    }
    const auto key_less = [](
        const catalog_key_descriptor& left,
        const catalog_key_descriptor& right) {
        if (left.name != right.name) return left.name < right.name;
        return left.columns < right.columns;
    };
    std::ranges::sort(descriptor.unique_constraints, key_less);
    std::ranges::sort(
        descriptor.checks,
        [](const catalog_check_descriptor& left,
           const catalog_check_descriptor& right) {
            if (left.name != right.name) return left.name < right.name;
            return left.expression < right.expression;
    });
}

[[nodiscard]] inline std::optional<std::string>
table_descriptor_mismatch(
    const table_descriptor& natural,
    const table_descriptor& install) {
    if (natural.target != install.target) {
        return "target '" + natural.target
            + "' differs from '" + install.target + "'";
    }
    if (natural.persistence != install.persistence) {
        return "persistence '" + natural.persistence
            + "' differs from '" + install.persistence + "'";
    }
    if (natural.access_method != install.access_method) {
        return "access method '" + natural.access_method
            + "' differs from '" + install.access_method + "'";
    }
    if (natural.if_not_exists != install.if_not_exists) {
        return "IF NOT EXISTS setting differs";
    }
    if (natural.user_catalog_table != install.user_catalog_table) {
        return "user_catalog_table setting differs";
    }
    if (natural.columns.size() != install.columns.size()) {
        return "column count " + std::to_string(natural.columns.size())
            + " differs from "
            + std::to_string(install.columns.size());
    }
    for (std::size_t index = 0; index < natural.columns.size();
         ++index) {
        const catalog_column_descriptor& left =
            natural.columns[index];
        const catalog_column_descriptor& right =
            install.columns[index];
        const std::string ordinal =
            "column " + std::to_string(index + 1) + " ('"
            + left.name + "')";
        if (left.name != right.name) {
            return "column " + std::to_string(index + 1)
                + " name '" + left.name + "' differs from '"
                + right.name + "'";
        }
        if (left.sql_type != right.sql_type) {
            return ordinal + " type '" + left.sql_type
                + "' differs from '" + right.sql_type + "'";
        }
        if (left.nullable != right.nullable) {
            return ordinal + " nullability differs";
        }
        if (left.default_value != right.default_value) {
            return ordinal + " default differs";
        }
    }
    if (natural.primary_key != install.primary_key) {
        return "primary key differs";
    }
    if (natural.unique_constraints.size()
        != install.unique_constraints.size()) {
        return "unique-constraint count "
            + std::to_string(natural.unique_constraints.size())
            + " differs from "
            + std::to_string(install.unique_constraints.size());
    }
    for (std::size_t index = 0;
         index < natural.unique_constraints.size();
         ++index) {
        if (natural.unique_constraints[index]
            != install.unique_constraints[index]) {
            return "unique constraint "
                + std::to_string(index + 1) + " differs";
        }
    }
    if (natural.checks.size() != install.checks.size()) {
        return "check-constraint count "
            + std::to_string(natural.checks.size())
            + " differs from "
            + std::to_string(install.checks.size());
    }
    for (std::size_t index = 0; index < natural.checks.size();
         ++index) {
        if (natural.checks[index] != install.checks[index]) {
            return "check constraint "
                + std::to_string(index + 1) + " expression '"
                + natural.checks[index].expression
                + "' differs from '"
                + install.checks[index].expression + "'";
        }
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<std::string>
natural_constraint_columns(const collected_entity& constraint) {
    if (const option_value* columns =
            find_option(constraint, "columns")) {
        return table_expression_identifiers(columns->expression);
    }
    if (constraint.declaration_kind
        == clang_structural::declaration_kind::field) {
        return {constraint.name};
    }
    return {};
}

[[nodiscard]] inline std::optional<std::string>
natural_constraint_name(const collected_entity& constraint) {
    if (const option_value* name = find_option(constraint, "name")) {
        return graph_unquoted_literal(name->expression);
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string relation_child_catalog_name(
    const collected_entity& child,
    const collected_entity& root) {
    if (const std::optional<std::string> explicit_name =
            natural_constraint_name(child)) {
        if (explicit_name->empty()
            || !sql_identifier(*explicit_name)
            || reserved_sql_identifier(*explicit_name)) {
            throw std::runtime_error(
                "relation child for '" + root.schema + "."
                + root.name + "' has invalid explicit name '"
                + *explicit_name + "'");
        }
        return *explicit_name;
    }
    if (child.kind != entity_kind::primary_key) {
        throw std::runtime_error(
            "natural " + std::string{kind_name(child.kind)}
            + " child for '" + root.schema + "." + root.name
            + "' requires an explicit .name before it can be used "
              "as a catalog dependency");
    }
    constexpr std::string_view suffix{"_pkey"};
    if (root.name.size() + suffix.size() > 63) {
        throw std::runtime_error(
            "derived primary-key relation name for '" + root.schema
            + "." + root.name
            + "' exceeds PostgreSQL's 63-byte identifier limit; "
              "specify an explicit .name");
    }
    return root.name + std::string{suffix};
}

[[nodiscard]] inline table_descriptor natural_table_descriptor(
    const collected_entity& table,
    const collected_program& program) {
    std::vector<const collected_entity*> columns;
    const collected_entity* primary_key = nullptr;
    std::vector<const collected_entity*> unique_constraints;
    std::vector<const collected_entity*> checks;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.kind == entity_kind::column
            && candidate.enclosing_cpp_name
                == table.cpp_qualified_name) {
            columns.push_back(&candidate);
        } else if (candidate.kind == entity_kind::primary_key
                   && natural_child_of(candidate, table)) {
            if (primary_key != nullptr) {
                throw std::runtime_error(
                    "natural table '" + table.schema + "."
                    + table.name + "' has more than one primary key");
            }
            primary_key = &candidate;
        } else if (candidate.kind == entity_kind::unique
                   && natural_child_of(candidate, table)) {
            unique_constraints.push_back(&candidate);
        } else if (candidate.kind == entity_kind::check
                   && natural_child_of(candidate, table)) {
            checks.push_back(&candidate);
        }
    }
    std::ranges::sort(
        columns,
        {},
        [](const collected_entity* column) {
            return column->source_range.begin.byte_offset;
        });
    if (columns.empty()) {
        throw std::runtime_error(
            "natural table '" + table.schema + "." + table.name
            + "' has no C++ row fields");
    }

    table_descriptor result{
        .target = table.schema + "." + table.name,
        .persistence =
            catalog_option_identifier(
                table, "persistence", "ordinary"),
        .access_method =
            catalog_option_identifier(
                table, "access_method", "default"),
        .if_not_exists =
            catalog_option_identifier(table, "create", "create")
                == "if_not_exists",
        .user_catalog_table =
            catalog_option_identifier(
                table, "user_catalog_table", "false")
                == "true",
    };
    for (const collected_entity* column : columns) {
        catalog_column_descriptor descriptor{
            .name = column->name,
            .sql_type = canonical_catalog_sql_type(
                table_sql_type(*column)),
            .nullable =
                column->cpp_return_type.contains("optional<"),
        };
        if (const option_value* default_value =
                find_option(*column, "default_value")) {
            descriptor.default_value =
                canonical_catalog_checked_sql(
                    default_value->expression);
        } else if (!column->initializer_expression.empty()) {
            descriptor.default_value =
                canonical_catalog_checked_sql(
                    natural_column_initializer_sql(*column));
        }
        result.columns.push_back(std::move(descriptor));
    }
    if (primary_key != nullptr) {
        std::vector<std::string> names =
            natural_constraint_columns(*primary_key);
        if (names.empty()) {
            throw std::runtime_error(
                "natural table primary key for '" + result.target
                + "' has an empty .columns list");
        }
        if (const option_value* placement =
                find_option(*primary_key, "placement");
            placement
            && graph_enum_leaf(placement->expression)
                == "column_inline"
            && names.size() != 1) {
            throw std::runtime_error(
                "column-inline primary key for '" + result.target
                + "' must name exactly one column");
        }
        result.primary_key = catalog_key_descriptor{
            .name = natural_constraint_name(*primary_key),
            .columns = std::move(names),
        };
    }
    for (const collected_entity* unique : unique_constraints) {
        if (const option_value* placement =
                find_option(*unique, "placement");
            placement
            && graph_enum_leaf(placement->expression)
                == "alter_table") {
            continue;
        }
        const std::vector<std::string> names =
            natural_constraint_columns(*unique);
        if (names.empty()) {
            throw std::runtime_error(
                "natural table unique constraint for '"
                + result.target + "' has an empty .columns list");
        }
        result.unique_constraints.push_back(catalog_key_descriptor{
            .name = natural_constraint_name(*unique),
            .columns = names,
        });
    }
    for (const collected_entity* check : checks) {
        if (const option_value* placement =
                find_option(*check, "placement");
            placement
            && graph_enum_leaf(placement->expression)
                == "alter_table") {
            continue;
        }
        const option_value* expression =
            find_option(*check, "expression");
        if (expression == nullptr) {
            throw std::runtime_error(
                "natural table check for '" + result.target
                + "' has no .expression");
        }
        result.checks.push_back(catalog_check_descriptor{
            .name = natural_constraint_name(*check),
            .expression = canonical_catalog_checked_sql(
                expression->expression),
        });
    }
    normalize_table_descriptor(result);
    return result;
}

[[nodiscard]] inline std::string natural_unique_alter_identity(
    const table_descriptor& table,
    const collected_entity& unique) {
    const option_value* columns_option =
        find_option(unique, "columns");
    const option_value* name = find_option(unique, "name");
    if (columns_option == nullptr || name == nullptr) {
        throw std::runtime_error(
            "natural ALTER TABLE unique constraint for '"
            + table.target + "' requires .name and .columns");
    }
    const std::vector<std::string> columns =
        table_expression_identifiers(columns_option->expression);
    if (columns.empty()) {
        throw std::runtime_error(
            "natural ALTER TABLE unique constraint for '"
            + table.target + "' has an empty .columns list");
    }
    std::string result = "alter table " + table.target
        + " add constraint "
        + graph_unquoted_literal(name->expression) + " unique (";
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index != 0) result += ", ";
        result += columns[index];
    }
    result += ");";
    return result;
}

[[nodiscard]] inline std::string typed_install_sql_identity(
    const collected_entity& entity,
    std::string_view kind) {
    const option_value* identity = find_option(entity, "sql_identity");
    if (identity != nullptr) {
        return decoded_cpp_literal(identity->expression);
    }
    throw std::runtime_error(
        "typed " + std::string{kind} + " install owner '"
        + entity.cpp_qualified_name + "' at "
        + entity.source_path.string() + ":"
        + std::to_string(entity.source_range.begin.line)
        + " has no SQL identity");
}

[[nodiscard]] inline std::string typed_install_context(
    const collected_entity& entity,
    std::string_view kind,
    std::string_view sql_identity) {
    std::string result =
        "typed " + std::string{kind} + " install owner '"
        + entity.cpp_qualified_name + "'";
    if (const option_value* physical =
            find_option(entity, "baseline_physical_id")) {
        result += " (baseline physical ID "
            + graph_unquoted_literal(physical->expression) + ")";
    }
    result += " at " + entity.source_path.string() + ":"
        + std::to_string(entity.source_range.begin.line)
        + " with SQL identity '" + std::string{sql_identity} + "'";
    return result;
}

[[nodiscard]] inline std::vector<std::string> catalog_sql_tokens(
    std::string_view sql) {
    std::vector<std::string> result;
    std::string current;
    const auto flush = [&current, &result] {
        if (!current.empty()) {
            result.push_back(std::move(current));
            current.clear();
        }
    };
    for (std::size_t index = 0; index < sql.size(); ++index) {
        const char value = sql[index];
        if (value == '\'' || value == '"') {
            flush();
            std::string quoted(1, value);
            bool closed = false;
            while (++index < sql.size()) {
                quoted.push_back(sql[index]);
                if (sql[index] != value) continue;
                if (index + 1 < sql.size()
                    && sql[index + 1] == value) {
                    quoted.push_back(sql[++index]);
                    continue;
                }
                closed = true;
                break;
            }
            if (!closed) {
                throw std::runtime_error(
                    "unterminated quoted SQL token");
            }
            result.push_back(std::move(quoted));
        } else if (std::isspace(
                       static_cast<unsigned char>(value))
                   || value == ';') {
            flush();
        } else if (value == '(' || value == ')'
                   || value == ',') {
            flush();
            result.emplace_back(1, value);
        } else {
            current.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(value))));
        }
    }
    flush();
    return result;
}

[[nodiscard]] inline std::string catalog_token_fragment(
    const std::vector<std::string>& tokens,
    std::size_t begin,
    std::size_t end) {
    std::string result;
    for (std::size_t index = begin; index < end; ++index) {
        if (!result.empty()) result += ' ';
        result += tokens[index];
    }
    return result;
}

[[nodiscard]] inline std::string catalog_qualified_target(
    std::string_view target) {
    return target.contains('.')
        ? std::string{target}
        : "$extension." + std::string{target};
}

[[nodiscard]] inline std::string install_resolved_catalog_name(
    std::string value,
    std::string_view extension_schema) {
    constexpr std::string_view marker{"$extension."};
    if (value.starts_with(marker)) {
        value.replace(0, marker.size(), extension_schema);
    }
    return value;
}

[[nodiscard]] inline std::string natural_catalog_name(
    const collected_entity& entity,
    std::string_view extension_schema) {
    if (entity.schema == "$extension") {
        return std::string{extension_schema} + "." + entity.name;
    }
    return entity.schema + "." + entity.name;
}

[[nodiscard]] inline std::size_t catalog_matching_parenthesis(
    const std::vector<std::string>& tokens,
    std::size_t open,
    std::string_view context) {
    if (open >= tokens.size() || tokens[open] != "(") {
        throw std::runtime_error(
            std::string{context} + " requires '('");
    }
    int depth = 0;
    for (std::size_t index = open; index < tokens.size(); ++index) {
        if (tokens[index] == "(") {
            ++depth;
        } else if (tokens[index] == ")") {
            --depth;
            if (depth == 0) return index;
            if (depth < 0) break;
        }
    }
    throw std::runtime_error(
        std::string{context} + " has unbalanced parentheses");
}

[[nodiscard]] inline std::vector<std::vector<std::string>>
catalog_top_level_clauses(
    const std::vector<std::string>& tokens,
    std::size_t begin,
    std::size_t end,
    std::string_view context) {
    std::vector<std::vector<std::string>> result;
    std::size_t clause_begin = begin;
    int depth = 0;
    for (std::size_t index = begin; index <= end; ++index) {
        const bool at_end = index == end;
        if (!at_end && tokens[index] == "(") {
            ++depth;
        } else if (!at_end && tokens[index] == ")") {
            --depth;
            if (depth < 0) {
                throw std::runtime_error(
                    std::string{context}
                    + " has unbalanced clause parentheses");
            }
        }
        if (at_end || (depth == 0 && tokens[index] == ",")) {
            if (index == clause_begin) {
                throw std::runtime_error(
                    std::string{context} + " has an empty clause");
            }
            result.emplace_back(
                tokens.begin() + static_cast<std::ptrdiff_t>(clause_begin),
                tokens.begin() + static_cast<std::ptrdiff_t>(index));
            clause_begin = index + 1;
        }
    }
    if (depth != 0) {
        throw std::runtime_error(
            std::string{context} + " has unbalanced clause parentheses");
    }
    return result;
}

[[nodiscard]] inline std::vector<std::string>
parse_catalog_identifier_list(
    const std::vector<std::string>& tokens,
    std::size_t& cursor,
    std::string_view context) {
    const std::size_t close =
        catalog_matching_parenthesis(tokens, cursor, context);
    std::vector<std::string> result;
    bool expect_identifier = true;
    for (std::size_t index = cursor + 1; index < close; ++index) {
        if (expect_identifier) {
            if (tokens[index] == "," || tokens[index] == "("
                || tokens[index] == ")") {
                throw std::runtime_error(
                    std::string{context}
                    + " has a malformed identifier list");
            }
            result.push_back(tokens[index]);
        } else if (tokens[index] != ",") {
            throw std::runtime_error(
                std::string{context}
                + " has a malformed identifier list");
        }
        expect_identifier = !expect_identifier;
    }
    if (result.empty() || expect_identifier) {
        throw std::runtime_error(
            std::string{context}
            + " has an empty or incomplete identifier list");
    }
    cursor = close + 1;
    return result;
}

[[nodiscard]] inline std::string parse_catalog_check_expression(
    const std::vector<std::string>& tokens,
    std::size_t& cursor,
    std::string_view context) {
    const std::size_t close =
        catalog_matching_parenthesis(tokens, cursor, context);
    if (close == cursor + 1) {
        throw std::runtime_error(
            std::string{context} + " has an empty CHECK expression");
    }
    const std::string result = canonical_catalog_sql_fragment(
        catalog_token_fragment(tokens, cursor + 1, close));
    cursor = close + 1;
    return result;
}

inline void parse_catalog_table_constraint(
    table_descriptor& descriptor,
    const std::vector<std::string>& tokens,
    std::size_t& cursor,
    std::optional<std::string> name,
    std::optional<std::string> inline_column,
    std::string_view context) {
    if (cursor + 1 < tokens.size() && tokens[cursor] == "primary"
        && tokens[cursor + 1] == "key") {
        cursor += 2;
        std::vector<std::string> columns;
        if (inline_column) {
            columns.push_back(*inline_column);
        } else {
            columns =
                parse_catalog_identifier_list(tokens, cursor, context);
        }
        if (descriptor.primary_key) {
            throw std::runtime_error(
                std::string{context}
                + " declares more than one primary key");
        }
        descriptor.primary_key = catalog_key_descriptor{
            .name = std::move(name),
            .columns = std::move(columns),
        };
        return;
    }
    if (cursor < tokens.size() && tokens[cursor] == "unique") {
        ++cursor;
        std::vector<std::string> columns;
        if (inline_column) {
            columns.push_back(*inline_column);
        } else {
            columns =
                parse_catalog_identifier_list(tokens, cursor, context);
        }
        descriptor.unique_constraints.push_back(
            catalog_key_descriptor{
                .name = std::move(name),
                .columns = std::move(columns),
            });
        return;
    }
    if (cursor < tokens.size() && tokens[cursor] == "check") {
        ++cursor;
        descriptor.checks.push_back(catalog_check_descriptor{
            .name = std::move(name),
            .expression = parse_catalog_check_expression(
                tokens, cursor, context),
        });
        return;
    }
    throw std::runtime_error(
        std::string{context}
        + " has an unsupported table constraint");
}

[[nodiscard]] inline bool catalog_column_constraint_start(
    std::string_view token) noexcept {
    return token == "not" || token == "null"
        || token == "default" || token == "primary"
        || token == "unique" || token == "check"
        || token == "constraint" || token == "references"
        || token == "collate";
}

inline void parse_catalog_table_clause(
    table_descriptor& descriptor,
    const std::vector<std::string>& tokens,
    std::string_view context) {
    if (tokens.empty()) {
        throw std::runtime_error(
            std::string{context} + " has an empty table clause");
    }
    std::size_t cursor = 0;
    std::optional<std::string> constraint_name;
    if (tokens[cursor] == "constraint") {
        if (++cursor >= tokens.size()) {
            throw std::runtime_error(
                std::string{context}
                + " has a constraint without a name");
        }
        constraint_name = tokens[cursor++];
    }
    if (cursor < tokens.size()
        && (tokens[cursor] == "primary"
            || tokens[cursor] == "unique"
            || tokens[cursor] == "check")) {
        parse_catalog_table_constraint(
            descriptor,
            tokens,
            cursor,
            std::move(constraint_name),
            std::nullopt,
            context);
        if (cursor != tokens.size()) {
            throw std::runtime_error(
                std::string{context}
                + " has trailing table-constraint tokens");
        }
        return;
    }
    if (constraint_name) {
        throw std::runtime_error(
            std::string{context}
            + " has a named constraint without a constraint kind");
    }

    catalog_column_descriptor column{
        .name = tokens[cursor++],
    };
    const std::size_t type_begin = cursor;
    while (cursor < tokens.size()
           && !catalog_column_constraint_start(tokens[cursor])) {
        ++cursor;
    }
    if (cursor == type_begin) {
        throw std::runtime_error(
            std::string{context} + " column '" + column.name
            + "' has no SQL type");
    }
    column.sql_type = canonical_catalog_sql_type(
        catalog_token_fragment(tokens, type_begin, cursor));

    while (cursor < tokens.size()) {
        if (tokens[cursor] == "not") {
            if (cursor + 1 >= tokens.size()
                || tokens[cursor + 1] != "null") {
                throw std::runtime_error(
                    std::string{context} + " column '"
                    + column.name + "' has malformed NOT NULL");
            }
            column.nullable = false;
            cursor += 2;
        } else if (tokens[cursor] == "null") {
            column.nullable = true;
            ++cursor;
        } else if (tokens[cursor] == "default") {
            const std::size_t default_begin = ++cursor;
            int depth = 0;
            while (cursor < tokens.size()) {
                if (tokens[cursor] == "(") {
                    ++depth;
                } else if (tokens[cursor] == ")") {
                    --depth;
                }
                if (cursor > default_begin && depth == 0
                    && catalog_column_constraint_start(
                        tokens[cursor])) {
                    break;
                }
                ++cursor;
            }
            if (cursor == default_begin) {
                throw std::runtime_error(
                    std::string{context} + " column '"
                    + column.name + "' has an empty DEFAULT");
            }
            column.default_value = canonical_catalog_sql_fragment(
                catalog_token_fragment(
                    tokens, default_begin, cursor));
        } else {
            std::optional<std::string> inline_name;
            if (tokens[cursor] == "constraint") {
                if (++cursor >= tokens.size()) {
                    throw std::runtime_error(
                        std::string{context} + " column '"
                        + column.name
                        + "' has a constraint without a name");
                }
                inline_name = tokens[cursor++];
            }
            parse_catalog_table_constraint(
                descriptor,
                tokens,
                cursor,
                std::move(inline_name),
                column.name,
                context);
        }
    }
    descriptor.columns.push_back(std::move(column));
}

[[nodiscard]] inline table_descriptor typed_table_descriptor(
    const collected_entity& table) {
    const std::string sql_identity =
        typed_install_sql_identity(table, "table");
    const std::string context =
        typed_install_context(table, "table", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.empty() || tokens[0] != "create") {
        throw std::runtime_error(
            context
            + " is not CREATE [UNLOGGED|TEMPORARY] TABLE");
    }
    table_descriptor result;
    std::size_t cursor = 1;
    if (cursor < tokens.size() && tokens[cursor] == "unlogged") {
        result.persistence = "unlogged";
        ++cursor;
    } else if (cursor < tokens.size()
               && (tokens[cursor] == "temporary"
                   || tokens[cursor] == "temp")) {
        result.persistence = "temporary";
        ++cursor;
    } else if (cursor < tokens.size()
               && (tokens[cursor] == "global"
                   || tokens[cursor] == "local")) {
        ++cursor;
        if (cursor >= tokens.size()
            || (tokens[cursor] != "temporary"
                && tokens[cursor] != "temp")) {
            throw std::runtime_error(
                context
                + " has GLOBAL/LOCAL without TEMPORARY");
        }
        result.persistence = "temporary";
        ++cursor;
    }
    if (cursor >= tokens.size() || tokens[cursor] != "table") {
        throw std::runtime_error(
            context
            + " is not CREATE [UNLOGGED|TEMPORARY] TABLE");
    }
    ++cursor;
    if (cursor < tokens.size() && tokens[cursor] == "if") {
        if (cursor + 2 >= tokens.size()
            || tokens[cursor + 1] != "not"
            || tokens[cursor + 2] != "exists") {
            throw std::runtime_error(
                context + " has malformed IF NOT EXISTS");
        }
        result.if_not_exists = true;
        cursor += 3;
    }
    if (cursor >= tokens.size() || tokens[cursor] == "(") {
        throw std::runtime_error(
            context + " has no qualified table target");
    }
    result.target = catalog_qualified_target(tokens[cursor++]);
    if (cursor >= tokens.size() || tokens[cursor] != "(") {
        throw std::runtime_error(
            context + " has no table definition");
    }
    const std::size_t close =
        catalog_matching_parenthesis(tokens, cursor, context);
    for (const std::vector<std::string>& clause :
         catalog_top_level_clauses(
             tokens, cursor + 1, close, context)) {
        parse_catalog_table_clause(result, clause, context);
    }
    cursor = close + 1;
    while (cursor < tokens.size()) {
        if (tokens[cursor] == "using") {
            if (++cursor >= tokens.size()) {
                throw std::runtime_error(
                    context + " has USING without an access method");
            }
            result.access_method = tokens[cursor++];
        } else if (tokens[cursor] == "with") {
            if (++cursor >= tokens.size()
                || tokens[cursor] != "(") {
                throw std::runtime_error(
                    context + " has malformed table storage options");
            }
            const std::size_t with_close =
                catalog_matching_parenthesis(tokens, cursor, context);
            const std::string options =
                canonical_catalog_sql_fragment(
                    catalog_token_fragment(
                        tokens, cursor + 1, with_close));
            if (options != "user_catalog_table=true") {
                throw std::runtime_error(
                    context
                    + " has unsupported table storage options");
            }
            result.user_catalog_table = true;
            cursor = with_close + 1;
        } else {
            throw std::runtime_error(
                context + " has unsupported trailing table syntax");
        }
    }
    if (result.columns.empty()) {
        throw std::runtime_error(
            context + " has no table columns");
    }
    normalize_table_descriptor(result);
    return result;
}

[[nodiscard]] inline std::string typed_table_target(
    const collected_entity& table) {
    return typed_table_descriptor(table).target;
}

inline void reconcile_natural_table_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>> natural_by_target;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::table
            || entity.typed_install_owner
            || !entity.source_module.ends_with(".sql_rows")) {
            continue;
        }
        if (!entity.active_condition.empty()) {
            throw std::runtime_error(
                entity.source_path.string() + ":"
                + std::to_string(entity.source_range.begin.line)
                + ": generated row codecs require an unconditional "
                  "natural table owner");
        }
        const table_descriptor descriptor =
            natural_table_descriptor(entity, program);
        if (!natural_by_target.emplace(
                descriptor.target, index).second) {
            throw std::runtime_error(
                "duplicate natural table owner for '"
                + descriptor.target + "'");
        }
    }

    std::set<std::size_t> merged_install_owners;
    std::set<std::string, std::less<>> matched_table_targets;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::table
            || !install.typed_install_owner) {
            continue;
        }
        const table_descriptor install_descriptor =
            typed_table_descriptor(install);
        const auto natural_position =
            natural_by_target.find(install_descriptor.target);
        if (natural_position == natural_by_target.end()) continue;
        collected_entity& natural =
            program.entities[natural_position->second];
        const table_descriptor natural_descriptor =
            natural_table_descriptor(natural, program);
        if (!matched_table_targets
                 .insert(natural_descriptor.target)
                 .second) {
            throw std::runtime_error(
                "natural table owner '" + natural_descriptor.target
                + "' matches more than one typed table install owner");
        }
        const option_value* identity =
            find_option(install, "sql_identity");
        if (const std::optional<std::string> mismatch =
                table_descriptor_mismatch(
                    natural_descriptor, install_descriptor)) {
            throw std::runtime_error(
                "natural table owner '" + natural_descriptor.target
                + "' disagrees with its typed install-owner descriptor: "
                + *mismatch + " "
                + typed_install_context(
                    install,
                    "table",
                    graph_unquoted_literal(identity->expression)));
        }
        adopt_typed_install_statement(
            natural,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }

    struct natural_alter_unique {
        std::size_t entity_index;
        std::string identity;
    };
    std::map<std::string, natural_alter_unique, std::less<>>
        natural_alter_uniques;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& unique = program.entities[index];
        if (unique.kind != entity_kind::unique
            || unique.typed_install_owner) {
            continue;
        }
        const option_value* placement =
            find_option(unique, "placement");
        const option_value* name = find_option(unique, "name");
        if (placement == nullptr || name == nullptr
            || graph_enum_leaf(placement->expression)
                != "alter_table") {
            continue;
        }
        const auto table = std::ranges::find_if(
            natural_by_target,
            [&program, &unique](const auto& target_and_index) {
                return program.entities[target_and_index.second]
                           .cpp_qualified_name
                    == unique.cpp_qualified_name;
            });
        if (table == natural_by_target.end()) continue;
        const std::string constraint_name =
            graph_unquoted_literal(name->expression);
        const std::string key =
            table->first + "\n" + constraint_name;
        if (!natural_alter_uniques.emplace(
                key,
                natural_alter_unique{
                    .entity_index = index,
                    .identity = natural_unique_alter_identity(
                        natural_table_descriptor(
                            program.entities[table->second], program),
                        unique),
                }).second) {
            throw std::runtime_error(
                "duplicate natural ALTER TABLE unique constraint '"
                + constraint_name + "' for '" + table->first + "'");
        }
    }
    std::set<std::string, std::less<>> matched_alter_uniques;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::alter
            || !install.typed_install_owner) {
            continue;
        }
        const option_value* identity =
            find_option(install, "sql_identity");
        if (identity == nullptr) continue;
        const std::string normalized =
            graph_unquoted_literal(identity->expression);
        const std::vector<std::string> tokens =
            sequence_tokens(normalized);
        if (tokens.size() < 7 || tokens[0] != "alter"
            || tokens[1] != "table" || tokens[3] != "add"
            || tokens[4] != "constraint") {
            continue;
        }
        const std::string key = tokens[2] + "\n" + tokens[5];
        const auto natural = natural_alter_uniques.find(key);
        if (natural == natural_alter_uniques.end()) continue;
        if (!matched_alter_uniques.insert(key).second) {
            throw std::runtime_error(
                "natural ALTER TABLE unique constraint '" + tokens[5]
                + "' for '" + tokens[2]
                + "' matches more than one typed install owner");
        }
        if (normalized != natural->second.identity) {
            throw std::runtime_error(
                "natural ALTER TABLE unique constraint '" + tokens[5]
                + "' for '" + tokens[2]
                + "' disagrees with its typed install-owner descriptor");
        }
        collected_entity& natural_entity =
            program.entities[natural->second.entity_index];
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }

    if (merged_install_owners.empty()) return;
    std::vector<collected_entity> reconciled;
    reconciled.reserve(
        program.entities.size() - merged_install_owners.size());
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        if (!merged_install_owners.contains(index)) {
            reconciled.push_back(std::move(program.entities[index]));
        }
    }
    program.entities = std::move(reconciled);
}

inline void erase_reconciled_install_owners(
    collected_program& program,
    const std::set<std::size_t>& reconciled_indexes) {
    if (reconciled_indexes.empty()) return;
    std::vector<collected_entity> reconciled;
    reconciled.reserve(
        program.entities.size() - reconciled_indexes.size());
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        if (!reconciled_indexes.contains(index)) {
            reconciled.push_back(std::move(program.entities[index]));
        }
    }
    program.entities = std::move(reconciled);
}

struct composite_type_field_descriptor {
    std::string name;
    std::string sql_type;
    bool nullable{true};

    [[nodiscard]] bool operator==(
        const composite_type_field_descriptor&) const = default;
};

struct composite_type_descriptor {
    std::string target;
    std::vector<composite_type_field_descriptor> fields;

    [[nodiscard]] bool operator==(
        const composite_type_descriptor&) const = default;
};

[[nodiscard]] inline composite_type_descriptor
natural_composite_type_descriptor(
    const collected_entity& type,
    const collected_program& program) {
    std::vector<const collected_entity*> fields;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.kind == entity_kind::column
            && candidate.enclosing_cpp_name
                == type.cpp_qualified_name) {
            fields.push_back(&candidate);
        }
    }
    std::ranges::sort(
        fields,
        {},
        [](const collected_entity* field) {
            return field->source_range.begin.byte_offset;
        });
    if (fields.empty()) {
        throw std::runtime_error(
            "natural composite type '" + type.schema + "."
            + type.name + "' has no C++ row fields");
    }

    composite_type_descriptor result{
        .target = type.schema + "." + type.name,
    };
    result.fields.reserve(fields.size());
    for (const collected_entity* field : fields) {
        result.fields.push_back(composite_type_field_descriptor{
            .name = field->name,
            .sql_type = canonical_catalog_sql_type(
                table_sql_type(*field)),
            .nullable =
                field->cpp_return_type.contains("optional<"),
        });
    }
    return result;
}

[[nodiscard]] inline std::optional<composite_type_descriptor>
typed_composite_type_descriptor(const collected_entity& type) {
    const std::string sql_identity =
        typed_install_sql_identity(type, "type");
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.size() < 5 || tokens[0] != "create"
        || tokens[1] != "type" || tokens[3] != "as"
        || tokens[4] != "(") {
        return std::nullopt;
    }
    const std::string context =
        typed_install_context(type, "type", sql_identity);
    composite_type_descriptor result{
        .target = catalog_qualified_target(tokens[2]),
    };
    const std::size_t close =
        catalog_matching_parenthesis(tokens, 4, context);
    if (close + 1 != tokens.size()) {
        throw std::runtime_error(
            context
            + " has unsupported trailing composite-type syntax");
    }
    for (const std::vector<std::string>& clause :
         catalog_top_level_clauses(
             tokens, 5, close, context)) {
        if (clause.size() < 2 || clause[0] == "("
            || clause[0] == ")" || clause[0] == ",") {
            throw std::runtime_error(
                context
                + " has a composite attribute without a name and SQL type");
        }
        result.fields.push_back(composite_type_field_descriptor{
            .name = clause[0],
            .sql_type = canonical_catalog_sql_type(
                catalog_token_fragment(
                    clause, 1, clause.size())),
            .nullable = true,
        });
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string>
composite_type_descriptor_mismatch(
    const composite_type_descriptor& natural,
    const composite_type_descriptor& install) {
    if (natural.target != install.target) {
        return "target mismatch: natural '" + natural.target
            + "', typed '" + install.target + "'";
    }
    if (natural.fields.size() != install.fields.size()) {
        return "field count mismatch: natural "
            + std::to_string(natural.fields.size()) + ", typed "
            + std::to_string(install.fields.size());
    }
    for (std::size_t index = 0; index < natural.fields.size(); ++index) {
        const composite_type_field_descriptor& natural_field =
            natural.fields[index];
        const composite_type_field_descriptor& install_field =
            install.fields[index];
        if (natural_field.name != install_field.name) {
            return "field " + std::to_string(index + 1)
                + " name/order mismatch: natural '"
                + natural_field.name + "', typed '"
                + install_field.name + "'";
        }
        if (natural_field.sql_type != install_field.sql_type) {
            return "field '" + natural_field.name
                + "' SQL type mismatch: natural '"
                + natural_field.sql_type + "', typed '"
                + install_field.sql_type + "'";
        }
        if (natural_field.nullable != install_field.nullable) {
            return "field '" + natural_field.name
                + "' nullability mismatch: natural "
                + (natural_field.nullable ? "nullable" : "not nullable")
                + ", typed "
                + (install_field.nullable ? "nullable" : "not nullable");
        }
    }
    return std::nullopt;
}

inline void reconcile_natural_composite_type_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::type
            || entity.typed_install_owner
            || !entity.source_module.ends_with(".sql_rows")) {
            continue;
        }
        if (!entity.active_condition.empty()) {
            throw std::runtime_error(
                entity.source_path.string() + ":"
                + std::to_string(entity.source_range.begin.line)
                + ": generated row codecs require an unconditional "
                  "natural composite type owner");
        }
        const composite_type_descriptor descriptor =
            natural_composite_type_descriptor(entity, program);
        if (!natural_by_target.emplace(
                descriptor.target, index).second) {
            throw std::runtime_error(
                "duplicate natural composite type owner for '"
                + descriptor.target + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> merged_install_owners;
    std::set<std::string, std::less<>> matched_targets;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::type
            || !install.typed_install_owner) {
            continue;
        }
        const std::optional<composite_type_descriptor>
            install_descriptor =
                typed_composite_type_descriptor(install);
        if (!install_descriptor) continue;
        const auto natural =
            natural_by_target.find(install_descriptor->target);
        if (natural == natural_by_target.end()) continue;
        if (!matched_targets.insert(install_descriptor->target).second) {
            throw std::runtime_error(
                "natural composite type owner '"
                + install_descriptor->target
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        const composite_type_descriptor natural_descriptor =
            natural_composite_type_descriptor(
                natural_entity, program);
        if (const std::optional<std::string> mismatch =
                composite_type_descriptor_mismatch(
                    natural_descriptor, *install_descriptor)) {
            throw std::runtime_error(
                "natural composite type owner '"
                + natural_descriptor.target
                + "' disagrees with its typed install-owner descriptor: "
                + *mismatch + " "
                + typed_install_context(
                    install,
                    "type",
                    typed_install_sql_identity(install, "type")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }
    erase_reconciled_install_owners(
        program, merged_install_owners);
}

struct cast_descriptor {
    std::string source_type;
    std::string target_type;
    std::string function_target;
    std::vector<std::string> function_arguments;
    std::string context{"explicit"};

    [[nodiscard]] bool operator==(
        const cast_descriptor&) const = default;
};

[[nodiscard]] inline std::string catalog_type_list(
    const std::vector<std::string>& values) {
    std::string result{"("};
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result += ",";
        result += values[index];
    }
    result += ")";
    return result;
}

[[nodiscard]] inline std::optional<std::string>
cast_descriptor_mismatch(
    const cast_descriptor& natural,
    const cast_descriptor& install) {
    if (natural.source_type != install.source_type) {
        return "source type '" + natural.source_type
            + "' differs from '" + install.source_type + "'";
    }
    if (natural.target_type != install.target_type) {
        return "target type '" + natural.target_type
            + "' differs from '" + install.target_type + "'";
    }
    if (natural.function_target != install.function_target) {
        return "function target '" + natural.function_target
            + "' differs from '" + install.function_target + "'";
    }
    if (natural.function_arguments != install.function_arguments) {
        return "function arguments "
            + catalog_type_list(natural.function_arguments)
            + " differ from "
            + catalog_type_list(install.function_arguments);
    }
    if (natural.context != install.context) {
        return "cast context '" + natural.context
            + "' differs from '" + install.context + "'";
    }
    return std::nullopt;
}

[[nodiscard]] inline const collected_entity&
natural_cast_function(
    const collected_entity& cast,
    const collected_program& program) {
    const collected_entity* result = nullptr;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.typed_install_owner
            || (candidate.kind != entity_kind::function
                && candidate.kind != entity_kind::sql_function)
            || candidate.cpp_qualified_name
                != cast.cpp_qualified_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "natural cast owner '" + cast.cpp_qualified_name
                + "' resolves more than one function declaration");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "natural cast owner '" + cast.cpp_qualified_name
            + "' has no function declaration");
    }
    return *result;
}

[[nodiscard]] inline cast_descriptor natural_cast_descriptor(
    const collected_entity& cast,
    const collected_program& program) {
    const collected_entity& function =
        natural_cast_function(cast, program);
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(function);
    if (parameters.empty() || parameters.size() > 3) {
        throw std::runtime_error(
            "natural cast function '" + function.cpp_qualified_name
            + "' must have one to three SQL arguments");
    }
    if (parameters.size() >= 2
        && canonical_catalog_sql_type(parameters[1]->sql_type)
            != "integer") {
        throw std::runtime_error(
            "natural cast function '" + function.cpp_qualified_name
            + "' second argument must be integer typmod");
    }
    if (parameters.size() == 3
        && canonical_catalog_sql_type(parameters[2]->sql_type)
            != "boolean") {
        throw std::runtime_error(
            "natural cast function '" + function.cpp_qualified_name
            + "' third argument must be boolean explicitness");
    }
    cast_descriptor result{
        .source_type =
            canonical_catalog_sql_type(parameters.front()->sql_type),
        .target_type =
            canonical_catalog_sql_type(function.result_sql_type),
        .function_target =
            function.schema + "." + function.name,
    };
    for (const routine_parameter* parameter : parameters) {
        result.function_arguments.push_back(
            canonical_catalog_sql_type(parameter->sql_type));
    }
    if (catalog_option_identifier(cast, "assignment", "false")
        == "true") {
        result.context = "assignment";
    } else if (catalog_option_identifier(
                   cast, "implicit", "false")
               == "true") {
        result.context = "implicit";
    }
    return result;
}

[[nodiscard]] inline cast_descriptor typed_cast_descriptor(
    const collected_entity& cast) {
    const std::string sql_identity =
        typed_install_sql_identity(cast, "cast");
    const std::string diagnostic =
        typed_install_context(cast, "cast", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.size() < 10 || tokens[0] != "create"
        || tokens[1] != "cast" || tokens[2] != "(") {
        throw std::runtime_error(
            diagnostic + " is not a supported CREATE CAST");
    }
    const std::size_t cast_close =
        catalog_matching_parenthesis(tokens, 2, diagnostic);
    std::size_t as_index = cast_close;
    for (std::size_t index = 3; index < cast_close; ++index) {
        if (tokens[index] == "as") {
            as_index = index;
            break;
        }
    }
    if (as_index == 3 || as_index + 1 >= cast_close) {
        throw std::runtime_error(
            diagnostic + " has an empty source or target type");
    }
    if (cast_close + 4 >= tokens.size()
        || tokens[cast_close + 1] != "with"
        || tokens[cast_close + 2] != "function"
        || tokens[cast_close + 4] != "(") {
        throw std::runtime_error(
            diagnostic + " requires WITH FUNCTION");
    }
    const std::size_t function_close =
        catalog_matching_parenthesis(
            tokens, cast_close + 4, diagnostic);
    cast_descriptor result{
        .source_type = canonical_catalog_sql_type(
            catalog_token_fragment(tokens, 3, as_index)),
        .target_type = canonical_catalog_sql_type(
            catalog_token_fragment(
                tokens, as_index + 1, cast_close)),
        .function_target =
            catalog_qualified_target(tokens[cast_close + 3]),
    };
    for (const std::vector<std::string>& argument :
         catalog_top_level_clauses(
             tokens,
             cast_close + 5,
             function_close,
             diagnostic)) {
        result.function_arguments.push_back(
            canonical_catalog_sql_type(
                catalog_token_fragment(
                    argument, 0, argument.size())));
    }
    if (result.function_arguments.empty()
        || result.function_arguments.size() > 3) {
        throw std::runtime_error(
            diagnostic
            + " function must have one to three arguments");
    }
    if (function_close + 1 < tokens.size()) {
        if (function_close + 3 != tokens.size()
            || tokens[function_close + 1] != "as"
            || (tokens[function_close + 2] != "assignment"
                && tokens[function_close + 2] != "implicit")) {
            throw std::runtime_error(
                diagnostic + " has unsupported trailing syntax");
        }
        result.context = tokens[function_close + 2];
    }
    return result;
}

[[nodiscard]] inline std::string cast_descriptor_key(
    const cast_descriptor& descriptor) {
    return descriptor.source_type + "->" + descriptor.target_type;
}

inline void reconcile_natural_cast_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::cast
            || entity.typed_install_owner) {
            continue;
        }
        const cast_descriptor descriptor =
            natural_cast_descriptor(entity, program);
        const std::string key = cast_descriptor_key(descriptor);
        if (!natural_by_target.emplace(key, index).second) {
            throw std::runtime_error(
                "duplicate natural cast owner for '" + key + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched_targets;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::cast
            || !install.typed_install_owner) {
            continue;
        }
        const cast_descriptor install_descriptor =
            typed_cast_descriptor(install);
        const std::string key =
            cast_descriptor_key(install_descriptor);
        const auto natural = natural_by_target.find(key);
        if (natural == natural_by_target.end()) continue;
        if (!matched_targets.insert(key).second) {
            throw std::runtime_error(
                "natural cast owner '" + key
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        const cast_descriptor natural_descriptor =
            natural_cast_descriptor(natural_entity, program);
        if (const std::optional<std::string> mismatch =
                cast_descriptor_mismatch(
                    natural_descriptor, install_descriptor)) {
            throw std::runtime_error(
                "natural cast owner '" + key
                + "' disagrees with its typed install-owner descriptor: "
                + *mismatch + " "
                + typed_install_context(
                    install,
                    "cast",
                    typed_install_sql_identity(install, "cast")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct session_setting_descriptor {
    std::string action{"set"};
    std::string name;
    std::string value;

    [[nodiscard]] bool operator==(
        const session_setting_descriptor&) const = default;
};

struct schema_descriptor {
    std::string name;
    std::string create_mode{"create"};

    [[nodiscard]] bool operator==(
        const schema_descriptor&) const = default;
};

[[nodiscard]] inline std::string canonical_session_setting_value(
    std::string_view expression) {
    std::string result = graph_unquoted_literal(expression);
    std::ranges::transform(
        result,
        result.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    if (result == "off" || result == "false") return "false";
    if (result == "on" || result == "true") return "true";
    return result;
}

[[nodiscard]] inline std::vector<std::string>
session_setting_sql_tokens(std::string_view sql) {
    std::vector<std::string> result;
    for (std::string token : catalog_sql_tokens(sql)) {
        if (!token.empty()
            && (token.front() == '\'' || token.front() == '"')) {
            result.push_back(std::move(token));
            continue;
        }
        std::size_t begin = 0;
        for (std::size_t equals = token.find('=', begin);
             equals != std::string::npos;
             equals = token.find('=', begin)) {
            if (equals != begin) {
                result.push_back(token.substr(begin, equals - begin));
            }
            result.emplace_back("=");
            begin = equals + 1;
        }
        if (begin != token.size()) {
            result.push_back(token.substr(begin));
        }
    }
    return result;
}

[[nodiscard]] inline session_setting_descriptor
natural_session_setting_descriptor(
    const collected_entity& setting) {
    const option_value* value = find_option(setting, "value");
    if (value == nullptr) {
        throw std::runtime_error(
            "natural session setting '" + setting.name
            + "' has no set_after_guard value");
    }
    return {
        .name = setting.name,
        .value =
            canonical_session_setting_value(value->expression),
    };
}

[[nodiscard]] inline session_setting_descriptor
typed_session_setting_descriptor(
    const collected_entity& setting) {
    const std::string sql_identity =
        typed_install_sql_identity(setting, "session_setting");
    const std::string context =
        typed_install_context(
            setting, "session_setting", sql_identity);
    const std::vector<std::string> tokens =
        session_setting_sql_tokens(sql_identity);
    if (tokens.size() != 4 || tokens[0] != "set"
        || (tokens[2] != "=" && tokens[2] != "to")) {
        throw std::runtime_error(
            context
            + " must be exactly SET <guc-name> {=|TO} <value>");
    }
    return {
        .action = tokens[0],
        .name = tokens[1],
        .value = canonical_session_setting_value(tokens[3]),
    };
}

[[nodiscard]] inline schema_descriptor natural_schema_descriptor(
    const collected_entity& schema) {
    const std::string mode =
        catalog_option_identifier(schema, "create", "create");
    if (mode != "create" && mode != "if_not_exists"
        && mode != "or_replace") {
        throw std::runtime_error(
            "natural schema owner '" + schema.name
            + "' has unsupported create mode '" + mode + "'");
    }
    return {.name = schema.name, .create_mode = mode};
}

[[nodiscard]] inline schema_descriptor typed_schema_descriptor(
    const collected_entity& schema) {
    const std::string sql_identity =
        typed_install_sql_identity(schema, "schema");
    const std::string context =
        typed_install_context(schema, "schema", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (catalog_option_identifier(
            schema, "create_or_replace", "false") == "true") {
        throw std::runtime_error(
            context + " cannot use CREATE OR REPLACE");
    }
    if (tokens.size() == 3 && tokens[0] == "create"
        && tokens[1] == "schema") {
        return {.name = tokens[2]};
    }
    if (tokens.size() == 6 && tokens[0] == "create"
        && tokens[1] == "schema" && tokens[2] == "if"
        && tokens[3] == "not" && tokens[4] == "exists") {
        return {
            .name = tokens[5],
            .create_mode = "if_not_exists",
        };
    }
    throw std::runtime_error(
        context
        + " must be exactly CREATE SCHEMA [IF NOT EXISTS] <name>");
}

inline void reconcile_natural_bootstrap_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>>
        natural_settings;
    std::map<std::string, std::size_t, std::less<>>
        natural_schemas;
    std::size_t typed_setting_count = 0;
    std::size_t typed_schema_count = 0;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.typed_install_owner) {
            typed_setting_count +=
                entity.kind == entity_kind::session_setting;
            typed_schema_count += entity.kind == entity_kind::schema;
            continue;
        }
        if (entity.kind == entity_kind::session_setting) {
            const session_setting_descriptor descriptor =
                natural_session_setting_descriptor(entity);
            if (!natural_settings.emplace(
                    descriptor.name, index).second) {
                throw std::runtime_error(
                    "duplicate natural session setting owner for '"
                    + descriptor.name + "'");
            }
        } else if (entity.kind == entity_kind::schema) {
            const schema_descriptor descriptor =
                natural_schema_descriptor(entity);
            if (!natural_schemas.emplace(
                    descriptor.name, index).second) {
                throw std::runtime_error(
                    "duplicate natural schema owner for '"
                    + descriptor.name + "'");
            }
        }
    }

    std::set<std::size_t> merged_install_owners;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (!install.typed_install_owner) continue;
        std::size_t natural_index{};
        if (install.kind == entity_kind::session_setting) {
            const session_setting_descriptor typed =
                typed_session_setting_descriptor(install);
            auto natural = natural_settings.find(typed.name);
            if (natural == natural_settings.end()) {
                if (natural_settings.size() == 1
                    && typed_setting_count == 1) {
                    natural = natural_settings.begin();
                } else {
                    continue;
                }
            }
            natural_index = natural->second;
            if (natural_session_setting_descriptor(
                    program.entities[natural_index]) != typed) {
                throw std::runtime_error(
                    "natural session setting owner disagrees with its "
                    "typed install-owner descriptor");
            }
        } else if (install.kind == entity_kind::schema) {
            const schema_descriptor typed =
                typed_schema_descriptor(install);
            const auto natural = natural_schemas.find(typed.name);
            if (natural == natural_schemas.end()) {
                if (!natural_schemas.empty()
                    && typed_schema_count == natural_schemas.size()) {
                    throw std::runtime_error(
                        "typed schema install owner '" + typed.name
                        + "' does not match any natural schema owner");
                }
                continue;
            }
            natural_index = natural->second;
            if (natural_schema_descriptor(
                    program.entities[natural_index]) != typed) {
                throw std::runtime_error(
                    "natural schema owner '" + typed.name
                    + "' disagrees with its typed install-owner "
                      "descriptor");
            }
        } else {
            continue;
        }
        collected_entity& natural = program.entities[natural_index];
        if (!natural.physical_statements.empty()) {
            throw std::runtime_error(
                "natural bootstrap owner '" + natural.name
                + "' matches more than one typed install owner");
        }
        adopt_typed_install_statement(
            natural,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }
    erase_reconciled_install_owners(
        program, merged_install_owners);
}

struct catalog_index_descriptor {
    std::string target;
    std::string name;
    std::string access_method{"btree"};
    std::string expression;
    std::optional<std::string> predicate;
    bool if_not_exists{};
    bool unique{};

    [[nodiscard]] bool operator==(
        const catalog_index_descriptor&) const = default;
};

[[nodiscard]] inline catalog_index_descriptor
natural_index_descriptor(
    const collected_entity& index,
    std::string target) {
    const option_value* expression =
        find_option(index, "expression");
    if (expression == nullptr) {
        throw std::runtime_error(
            "natural index '" + index.name + "' for '" + target
            + "' has no .expression");
    }
    catalog_index_descriptor result{
        .target = std::move(target),
        .name = index.name,
        .access_method =
            catalog_option_identifier(
                index, "access_method", "btree"),
        .expression = canonical_catalog_checked_sql(
            expression->expression),
        .if_not_exists =
            catalog_option_identifier(index, "create", "create")
                == "if_not_exists",
        .unique =
            catalog_option_identifier(index, "unique", "false")
                == "true",
    };
    if (const option_value* predicate =
            find_option(index, "predicate")) {
        result.predicate = canonical_catalog_checked_sql(
            predicate->expression);
    }
    return result;
}

[[nodiscard]] inline catalog_index_descriptor
typed_index_descriptor(const collected_entity& index) {
    const std::string sql_identity =
        typed_install_sql_identity(index, "index");
    const std::string context =
        typed_install_context(index, "index", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    catalog_index_descriptor result;
    std::size_t cursor = 0;
    if (cursor >= tokens.size() || tokens[cursor++] != "create") {
        throw std::runtime_error(
            context + " is not CREATE INDEX");
    }
    if (cursor < tokens.size() && tokens[cursor] == "unique") {
        result.unique = true;
        ++cursor;
    }
    if (cursor >= tokens.size() || tokens[cursor++] != "index") {
        throw std::runtime_error(
            context + " is not CREATE [UNIQUE] INDEX");
    }
    if (cursor < tokens.size() && tokens[cursor] == "if") {
        if (cursor + 2 >= tokens.size()
            || tokens[cursor + 1] != "not"
            || tokens[cursor + 2] != "exists") {
            throw std::runtime_error(
                context + " has malformed IF NOT EXISTS");
        }
        result.if_not_exists = true;
        cursor += 3;
    }
    if (cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no index name");
    }
    result.name = tokens[cursor++];
    if (cursor >= tokens.size() || tokens[cursor++] != "on"
        || cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no indexed table target");
    }
    result.target =
        catalog_qualified_target(tokens[cursor++]);
    if (cursor < tokens.size() && tokens[cursor] == "using") {
        if (++cursor >= tokens.size()) {
            throw std::runtime_error(
                context + " has USING without an access method");
        }
        result.access_method = tokens[cursor++];
    }
    if (cursor >= tokens.size() || tokens[cursor] != "(") {
        throw std::runtime_error(
            context + " has no index expression");
    }
    const std::size_t close =
        catalog_matching_parenthesis(tokens, cursor, context);
    if (close == cursor + 1) {
        throw std::runtime_error(
            context + " has an empty index expression");
    }
    result.expression = canonical_catalog_sql_fragment(
        catalog_token_fragment(tokens, cursor + 1, close));
    cursor = close + 1;
    if (cursor < tokens.size()) {
        if (tokens[cursor++] != "where" || cursor >= tokens.size()) {
            throw std::runtime_error(
                context + " has unsupported trailing index syntax");
        }
        result.predicate = canonical_catalog_sql_fragment(
            catalog_token_fragment(tokens, cursor, tokens.size()));
        cursor = tokens.size();
    }
    if (cursor != tokens.size()) {
        throw std::runtime_error(
            context + " has unsupported trailing index syntax");
    }
    return result;
}

inline void reconcile_natural_index_owners(
    collected_program& program) {
    std::map<std::string, std::string, std::less<>>
        table_target_by_cpp_name;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind == entity_kind::table
            && !entity.typed_install_owner
            && entity.source_module.ends_with(".sql_rows")) {
            table_target_by_cpp_name.emplace(
                entity.cpp_qualified_name,
                entity.schema + "." + entity.name);
        }
    }
    struct natural_index_owner {
        std::size_t entity_index;
        catalog_index_descriptor descriptor;
    };
    std::map<std::string, natural_index_owner, std::less<>>
        natural_by_identity;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::index
            || entity.typed_install_owner) {
            continue;
        }
        const auto table =
            table_target_by_cpp_name.find(entity.cpp_qualified_name);
        if (table == table_target_by_cpp_name.end()) continue;
        catalog_index_descriptor descriptor =
            natural_index_descriptor(entity, table->second);
        const std::string key =
            descriptor.target + "\n" + descriptor.name;
        if (!natural_by_identity.emplace(
                key,
                natural_index_owner{
                    .entity_index = index,
                    .descriptor = std::move(descriptor),
                }).second) {
            throw std::runtime_error(
                "duplicate natural index owner '" + entity.name
                + "' for '" + table->second + "'");
        }
    }
    if (natural_by_identity.empty()) return;

    std::set<std::size_t> merged_install_owners;
    std::set<std::string, std::less<>> matched_identities;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::index
            || !install.typed_install_owner) {
            continue;
        }
        const catalog_index_descriptor descriptor =
            typed_index_descriptor(install);
        const std::string key =
            descriptor.target + "\n" + descriptor.name;
        const auto natural = natural_by_identity.find(key);
        if (natural == natural_by_identity.end()) continue;
        if (!matched_identities.insert(key).second) {
            throw std::runtime_error(
                "natural index owner '" + descriptor.name
                + "' for '" + descriptor.target
                + "' matches more than one typed install owner");
        }
        if (natural->second.descriptor != descriptor) {
            throw std::runtime_error(
                "natural index owner '" + descriptor.name
                + "' for '" + descriptor.target
                + "' disagrees with its typed install-owner descriptor "
                + typed_install_context(
                    install,
                    "index",
                    typed_install_sql_identity(install, "index")));
        }
        adopt_typed_install_statement(
            program.entities[natural->second.entity_index],
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }
    erase_reconciled_install_owners(
        program, merged_install_owners);
}

[[nodiscard]] inline std::string decode_catalog_sql_literal(
    std::string_view token,
    std::string_view context) {
    if (token.size() < 2 || token.front() != '\''
        || token.back() != '\'') {
        throw std::runtime_error(
            std::string{context}
            + " requires a single-quoted SQL literal");
    }
    std::string result;
    for (std::size_t index = 1; index + 1 < token.size(); ++index) {
        if (token[index] == '\'' && index + 2 < token.size()
            && token[index + 1] == '\'') {
            result.push_back('\'');
            ++index;
        } else {
            result.push_back(token[index]);
        }
    }
    return result;
}

struct catalog_config_dump_descriptor {
    std::string target;
    std::string filter;

    [[nodiscard]] bool operator==(
        const catalog_config_dump_descriptor&) const = default;
};

[[nodiscard]] inline catalog_config_dump_descriptor
typed_config_dump_descriptor(const collected_entity& config_dump) {
    const std::string sql_identity =
        typed_install_sql_identity(config_dump, "config_dump");
    const std::string context =
        typed_install_context(
            config_dump, "config_dump", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.size() != 7 || tokens[0] != "select"
        || (tokens[1] != "pg_extension_config_dump"
            && tokens[1]
                != "pg_catalog.pg_extension_config_dump")
        || tokens[2] != "(" || tokens[4] != ","
        || tokens[6] != ")") {
        throw std::runtime_error(
            context
            + " is not SELECT pg_extension_config_dump(table, filter)");
    }
    return catalog_config_dump_descriptor{
        .target = catalog_qualified_target(
            decode_catalog_sql_literal(tokens[3], context)),
        .filter = canonical_catalog_sql_fragment(
            decode_catalog_sql_literal(tokens[5], context)),
    };
}

inline void reconcile_natural_config_dump_owners(
    collected_program& program) {
    std::map<std::string, std::string, std::less<>>
        table_target_by_cpp_name;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind == entity_kind::table
            && !entity.typed_install_owner
            && entity.source_module.ends_with(".sql_rows")) {
            table_target_by_cpp_name.emplace(
                entity.cpp_qualified_name,
                entity.schema + "." + entity.name);
        }
    }
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::config_dump
            || entity.typed_install_owner) {
            continue;
        }
        const auto table =
            table_target_by_cpp_name.find(entity.cpp_qualified_name);
        if (table == table_target_by_cpp_name.end()) continue;
        if (!natural_by_target.emplace(table->second, index).second) {
            throw std::runtime_error(
                "duplicate natural config_dump owner for '"
                + table->second + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> merged_install_owners;
    std::set<std::string, std::less<>> matched_targets;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::config_dump
            || !install.typed_install_owner) {
            continue;
        }
        const catalog_config_dump_descriptor descriptor =
            typed_config_dump_descriptor(install);
        const auto natural =
            natural_by_target.find(descriptor.target);
        if (natural == natural_by_target.end()) continue;
        if (!matched_targets.insert(descriptor.target).second) {
            throw std::runtime_error(
                "natural config_dump owner for '" + descriptor.target
                + "' matches more than one typed install owner");
        }
        if (descriptor.filter
            != canonical_catalog_sql_fragment("WHERE FALSE")) {
            throw std::runtime_error(
                "natural config_dump owner for '" + descriptor.target
                + "' disagrees with its typed install-owner filter "
                + typed_install_context(
                    install,
                    "config_dump",
                    typed_install_sql_identity(
                        install, "config_dump")));
        }
        adopt_typed_install_statement(
            program.entities[natural->second],
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        merged_install_owners.insert(index);
    }
    erase_reconciled_install_owners(
        program, merged_install_owners);
}

[[nodiscard]] inline bool schema_less(entity_kind kind) noexcept {
    return kind == entity_kind::alter
        || kind == entity_kind::comment
        || kind == entity_kind::schema
        || kind == entity_kind::session_setting
        || kind == entity_kind::guc
        || kind == entity_kind::guc_value
        || kind == entity_kind::grant
        || kind == entity_kind::revoke
        || kind == entity_kind::phase;
}

[[nodiscard]] inline bool routine_identity_kind(entity_kind kind) noexcept {
    return kind == entity_kind::function
        || kind == entity_kind::procedure
        || kind == entity_kind::sql_function
        || kind == entity_kind::operator_;
}

[[nodiscard]] inline std::string routine_signature(
    const collected_entity& entity) {
    std::string result{"("};
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(entity);
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index != 0) result += ",";
        const std::string& sql_type =
            parameters[index]->stable_identity_sql_type.empty()
            ? parameters[index]->sql_type
            : parameters[index]->stable_identity_sql_type;
        result += sql_type == "quoted_any"
            ? "\"any\""
            : sql_type;
    }
    result += ")";
    return result;
}

[[nodiscard]] inline std::string stable_identity(
    const collected_entity& entity) {
    if (entity.typed_install_owner) {
        return "sql:install-owner:" + entity.name;
    }
    if (entity.kind == entity_kind::schema) {
        return "sql:schema:" + entity.name;
    }
    if (entity.kind == entity_kind::guc) {
        std::string path = entity.cpp_qualified_name;
        std::ranges::replace(path, ':', '.');
        while (path.contains("..")) {
            path.replace(path.find(".."), 2, ".");
        }
        const std::string marker = ".settings.";
        if (const std::size_t offset = path.find(marker);
            offset != std::string::npos) {
            path.erase(offset, marker.size() - 1);
        }
        return "sql:guc:" + path;
    }
    if (entity.kind == entity_kind::guc_value) {
        return "sql:guc-value:" + entity.cpp_qualified_name;
    }
    std::string result = "sql:";
    result += kind_name(entity.kind);
    result += ':';
    if (!schema_less(entity.kind)) {
        result += entity.schema;
        result += '.';
    }
    if (entity.companion && entity.kind != entity_kind::operator_
        && entity.kind != entity_kind::cast
        && entity.kind != entity_kind::grant
        && entity.kind != entity_kind::revoke) {
        result += entity.enclosing_cpp_name;
        result += '.';
    }
    result += entity.name;
    if (routine_identity_kind(entity.kind)) {
        result += routine_signature(entity);
    }
    return result;
}

[[nodiscard]] inline std::string installed_identity(
    std::string identity,
    std::string_view extension_schema) {
    std::size_t offset = 0;
    while ((offset = identity.find("$extension", offset))
           != std::string::npos) {
        identity.replace(offset, std::string_view{"$extension"}.size(),
                         extension_schema);
        offset += extension_schema.size();
    }
    return identity;
}

[[nodiscard]] inline std::string referenced_cpp_name(
    std::string expression) {
    if (const std::size_t type_of = expression.find("type_of<");
        type_of != std::string::npos) {
        const std::size_t begin = type_of + std::string_view{"type_of<"}.size();
        const std::size_t end = expression.rfind('>');
        if (end != std::string::npos && end > begin) {
            expression = expression.substr(begin, end - begin);
        }
    }
    while (!expression.empty()
           && (expression.front() == '&' || expression.front() == '('
               || expression.front() == '{')) {
        expression.erase(expression.begin());
    }
    while (!expression.empty()
           && (expression.back() == ')' || expression.back() == '}'
               || expression.back() == ' ')) {
        expression.pop_back();
    }
    return expression;
}

[[nodiscard]] inline std::vector<std::size_t> reference_candidates(
    const std::vector<normalized_entity>& entities,
    std::string_view expression) {
    const std::string name = referenced_cpp_name(std::string{expression});
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const std::string& qualified =
            entities[index].source.cpp_qualified_name;
        if (qualified == name
            || (qualified.size() > name.size()
                && qualified.ends_with("::" + name))) {
            result.push_back(index);
        }
    }
    return result;
}

[[nodiscard]] inline bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9') || value == '_';
}

[[nodiscard]] inline bool expression_mentions(
    std::string_view expression,
    std::string_view name) noexcept {
    std::size_t offset = expression.find(name);
    while (offset != std::string_view::npos) {
        const bool left_boundary =
            offset == 0
            || !identifier_character(expression[offset - 1]);
        const std::size_t end = offset + name.size();
        const bool right_boundary =
            end == expression.size()
            || !identifier_character(expression[end]);
        if (left_boundary && right_boundary) return true;
        offset = expression.find(name, offset + 1);
    }
    return false;
}

[[nodiscard]] inline bool cpp_identifier_start(
    char value) noexcept {
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z') || value == '_';
}

[[nodiscard]] inline std::vector<std::string_view>
declaration_reference_tokens(std::string_view expression) {
    std::vector<std::string_view> result;
    std::size_t cursor = 0;
    while (cursor < expression.size()) {
        if (expression[cursor] == '\''
            || expression[cursor] == '"') {
            const char quote = expression[cursor++];
            while (cursor < expression.size()) {
                if (expression[cursor] == '\\') {
                    cursor += std::min<std::size_t>(
                        2, expression.size() - cursor);
                } else if (expression[cursor++] == quote) {
                    break;
                }
            }
            continue;
        }
        if (!cpp_identifier_start(expression[cursor])) {
            ++cursor;
            continue;
        }
        const std::size_t begin = cursor++;
        while (cursor < expression.size()
               && identifier_character(expression[cursor])) {
            ++cursor;
        }
        while (cursor + 2 < expression.size()
               && expression[cursor] == ':'
               && expression[cursor + 1] == ':'
               && cpp_identifier_start(expression[cursor + 2])) {
            cursor += 3;
            while (cursor < expression.size()
                   && identifier_character(expression[cursor])) {
                ++cursor;
            }
        }
        result.push_back(expression.substr(begin, cursor - begin));
    }
    return result;
}

[[nodiscard]] inline bool declaration_reference_matches(
    std::string_view candidate,
    std::string_view token) noexcept {
    return candidate == token
        || (candidate.size() > token.size() + 2
            && candidate[candidate.size() - token.size() - 2] == ':'
            && candidate[candidate.size() - token.size() - 1] == ':'
            && candidate.ends_with(token));
}

[[nodiscard]] inline bool option_reference_candidate(
    entity_kind owner_kind,
    std::string_view option_name,
    entity_kind candidate_kind) noexcept {
    const bool routine =
        candidate_kind == entity_kind::function
        || candidate_kind == entity_kind::sql_function
        || candidate_kind == entity_kind::procedure;
    if (owner_kind == entity_kind::aggregate) {
        const bool routine_option =
            option_name == "transition" || option_name == "final"
            || option_name == "serial" || option_name == "deserial"
            || option_name == "combine"
            || option_name == "moving_transition"
            || option_name == "moving_inverse"
            || option_name == "moving_final";
        return (routine_option && routine)
            || (option_name == "sort_operator"
                && candidate_kind == entity_kind::operator_);
    }
    if (owner_kind == entity_kind::opclass) {
        if (option_name == "operators") {
            return routine || candidate_kind == entity_kind::operator_;
        }
        if (option_name == "functions") return routine;
        return candidate_kind == entity_kind::access_method
            || candidate_kind == entity_kind::type;
    }
    if (owner_kind == entity_kind::catalog_dependencies
        && option_name == "roots") {
        return candidate_kind == entity_kind::table
            || candidate_kind == entity_kind::sequence;
    }
    if (owner_kind == entity_kind::type_role
        && option_name == "for_type") {
        return candidate_kind == entity_kind::type;
    }
    if (owner_kind == entity_kind::operator_) {
        const bool reference_option =
            option_name == "commutator" || option_name == "negator"
            || option_name == "restrict" || option_name == "join";
        return reference_option
            && (routine || candidate_kind == entity_kind::operator_);
    }
    if (owner_kind == entity_kind::type
        && (option_name == "element" || option_name == "array")) {
        return candidate_kind == entity_kind::type;
    }
    return false;
}

[[nodiscard]] inline bool option_has_declaration_references(
    entity_kind owner_kind,
    std::string_view option_name) noexcept {
    return option_reference_candidate(
               owner_kind, option_name, entity_kind::function)
        || option_reference_candidate(
            owner_kind, option_name, entity_kind::type)
        || option_reference_candidate(
            owner_kind, option_name, entity_kind::table)
        || option_reference_candidate(
            owner_kind, option_name, entity_kind::sequence)
        || option_reference_candidate(
            owner_kind, option_name, entity_kind::operator_)
        || option_reference_candidate(
            owner_kind, option_name, entity_kind::access_method);
}

[[nodiscard]] inline std::vector<std::size_t>
option_reference_candidates(
    const std::vector<normalized_entity>& entities,
    entity_kind owner_kind,
    const option_value& option) {
    std::vector<std::size_t> result;
    const std::vector<std::string_view> tokens =
        declaration_reference_tokens(option.expression);
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const collected_entity& candidate = entities[index].source;
        if (!option_reference_candidate(
                owner_kind, option.name, candidate.kind)) {
            continue;
        }
        if (std::ranges::any_of(
                tokens,
                [candidate_name = std::string_view{
                     candidate.cpp_qualified_name}](
                    std::string_view token) {
                    return declaration_reference_matches(
                        candidate_name, token);
                })) {
            result.push_back(index);
        }
    }
    return result;
}

struct operator_descriptor {
    std::string target;
    std::string left_type;
    std::string right_type;
    std::string function_target;
    std::string commutator;
    std::string negator;
    std::string restrict;
    std::string join;
    bool hashes{};
    bool merges{};

    [[nodiscard]] bool operator==(
        const operator_descriptor&) const = default;
};

[[nodiscard]] inline std::optional<std::string>
operator_descriptor_mismatch(
    const operator_descriptor& natural,
    const operator_descriptor& install) {
    if (natural.target != install.target) {
        return "target '" + natural.target
            + "' differs from '" + install.target + "'";
    }
    if (natural.left_type != install.left_type) {
        return "left argument type '" + natural.left_type
            + "' differs from '" + install.left_type + "'";
    }
    if (natural.right_type != install.right_type) {
        return "right argument type '" + natural.right_type
            + "' differs from '" + install.right_type + "'";
    }
    if (natural.function_target != install.function_target) {
        return "function target '" + natural.function_target
            + "' differs from '" + install.function_target + "'";
    }
    if (natural.commutator != install.commutator) {
        return "commutator '" + natural.commutator
            + "' differs from '" + install.commutator + "'";
    }
    if (natural.negator != install.negator) {
        return "negator '" + natural.negator
            + "' differs from '" + install.negator + "'";
    }
    if (natural.restrict != install.restrict) {
        return "restriction estimator '" + natural.restrict
            + "' differs from '" + install.restrict + "'";
    }
    if (natural.join != install.join) {
        return "join estimator '" + natural.join
            + "' differs from '" + install.join + "'";
    }
    if (natural.hashes != install.hashes) {
        return "HASHES is "
            + std::string{natural.hashes ? "enabled" : "disabled"}
            + " naturally but "
            + (install.hashes ? "enabled" : "disabled")
            + " by the typed install owner";
    }
    if (natural.merges != install.merges) {
        return "MERGES is "
            + std::string{natural.merges ? "enabled" : "disabled"}
            + " naturally but "
            + (install.merges ? "enabled" : "disabled")
            + " by the typed install owner";
    }
    return std::nullopt;
}

[[nodiscard]] inline const collected_entity&
natural_operator_function(
    const collected_entity& op,
    const collected_program& program) {
    const collected_entity* result = nullptr;
    for (const collected_entity& candidate : program.entities) {
        const bool routine =
            candidate.kind == entity_kind::function
            || candidate.kind == entity_kind::sql_function;
        if (candidate.typed_install_owner || !routine
            || candidate.cpp_qualified_name
                != op.cpp_qualified_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "natural operator owner '" + op.cpp_qualified_name
                + "' resolves more than one function declaration");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "natural operator owner '" + op.cpp_qualified_name
            + "' has no function declaration");
    }
    return *result;
}

[[nodiscard]] inline const collected_entity*
natural_operator_for_cpp_name(
    std::string_view cpp_name,
    const collected_program& program) {
    const collected_entity* result = nullptr;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.typed_install_owner
            || candidate.kind != entity_kind::operator_
            || candidate.cpp_qualified_name != cpp_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "C++ declaration '" + std::string{cpp_name}
                + "' owns more than one natural SQL operator");
        }
        result = &candidate;
    }
    return result;
}

[[nodiscard]] inline const collected_entity*
natural_reference_entity(
    std::string_view expression,
    const collected_program& program,
    auto predicate,
    std::string_view context) {
    const collected_entity* result = nullptr;
    const std::vector<std::string_view> tokens =
        declaration_reference_tokens(expression);
    for (const collected_entity& candidate : program.entities) {
        if (candidate.typed_install_owner
            || !predicate(candidate.kind)) {
            continue;
        }
        const std::string& qualified = candidate.cpp_qualified_name;
        if (!std::ranges::any_of(
                tokens,
                [qualified_name = std::string_view{qualified}](
                    std::string_view token) {
                    return declaration_reference_matches(
                        qualified_name, token);
                })) {
            continue;
        }
        if (result != nullptr
            && result->cpp_qualified_name != qualified) {
            throw std::runtime_error(
                "ambiguous " + std::string{context}
                + " declaration reference '" + std::string{expression}
                + "' resolves to both '"
                + result->cpp_qualified_name + "' and '"
                + qualified + "'");
        }
        result = &candidate;
    }
    return result;
}

[[nodiscard]] inline std::string natural_operator_reference(
    const collected_entity& owner,
    const collected_program& program,
    std::string_view option_name) {
    const option_value* option = find_option(owner, option_name);
    if (option == nullptr) return {};
    if (const collected_entity* op = natural_reference_entity(
            option->expression,
            program,
            [](entity_kind kind) {
                return kind == entity_kind::operator_;
            },
            "operator")) {
        return op->schema + "." + op->name;
    }
    if (const collected_entity* routine = natural_reference_entity(
            option->expression,
            program,
            [](entity_kind kind) {
                return kind == entity_kind::function
                    || kind == entity_kind::sql_function;
            },
            "operator function")) {
        if (const collected_entity* op =
                natural_operator_for_cpp_name(
                    routine->cpp_qualified_name, program)) {
            return op->schema + "." + op->name;
        }
    }
    throw std::runtime_error(
        "natural operator '" + owner.schema + "." + owner.name
        + "' has an unresolved ." + std::string{option_name}
        + " reference");
}

[[nodiscard]] inline std::string natural_routine_reference(
    const collected_entity& owner,
    const collected_program& program,
    std::string_view option_name) {
    const option_value* option = find_option(owner, option_name);
    if (option == nullptr) return {};
    if (option->expression.contains("pg_catalog")) {
        const std::size_t separator =
            option->expression.rfind("::");
        if (separator == std::string::npos
            || separator + 2 == option->expression.size()) {
            throw std::runtime_error(
                "natural SQL owner '" + owner.schema + "."
                + owner.name + "' has malformed pg_catalog ."
                + std::string{option_name} + " reference");
        }
        return "pg_catalog."
            + option->expression.substr(separator + 2);
    }
    const collected_entity* routine = natural_reference_entity(
        option->expression,
        program,
        [](entity_kind kind) {
            return kind == entity_kind::function
                || kind == entity_kind::sql_function;
        },
        "routine");
    if (routine == nullptr) {
        throw std::runtime_error(
            "natural SQL owner '" + owner.schema + "." + owner.name
            + "' has an unresolved ." + std::string{option_name}
            + " reference");
    }
    return routine->schema + "." + routine->name;
}

struct base_type_definition_descriptor {
    std::string target;
    std::map<std::string, std::string, std::less<>> callbacks;
    std::string internal_length;
    bool passed_by_value{};
    std::string alignment;
    std::string storage;
    std::string category;
    bool preferred{};
    std::string delimiter;
    bool collatable{};
    std::string element;
    std::string array;
    std::string default_value;

    [[nodiscard]] bool operator==(
        const base_type_definition_descriptor&) const = default;
};

struct typed_base_type_statement_descriptor {
    std::string target;
    physical_statement_facet facet{
        physical_statement_facet::base_type_shell};
    std::optional<base_type_definition_descriptor> definition;
};

[[nodiscard]] inline std::string base_type_role_clause(
    std::string_view annotation_name) {
    if (annotation_name == "type_input") return "input";
    if (annotation_name == "type_output") return "output";
    if (annotation_name == "type_receive") return "receive";
    if (annotation_name == "type_send") return "send";
    if (annotation_name == "type_typmod_in") return "typmod_in";
    if (annotation_name == "type_typmod_out") return "typmod_out";
    if (annotation_name == "type_analyze") return "analyze";
    if (annotation_name == "type_subscript") return "subscript";
    throw std::runtime_error(
        "unsupported base-type callback role '"
        + std::string{annotation_name} + "'");
}

[[nodiscard]] inline const collected_entity&
natural_same_declaration_routine(
    const collected_entity& companion,
    const collected_program& program,
    std::string_view context) {
    const collected_entity* result = nullptr;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.typed_install_owner
            || (candidate.kind != entity_kind::function
                && candidate.kind != entity_kind::sql_function
                && candidate.kind != entity_kind::procedure)
            || candidate.cpp_qualified_name
                != companion.cpp_qualified_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                std::string{context} + " '"
                + companion.cpp_qualified_name
                + "' resolves more than one natural routine owner");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            std::string{context} + " '"
            + companion.cpp_qualified_name
            + "' has no natural routine owner");
    }
    return *result;
}

[[nodiscard]] inline bool routine_mentions_base_type(
    const collected_entity& routine,
    const collected_entity& type,
    std::string_view extension_schema) {
    const std::string target =
        natural_catalog_name(type, extension_schema);
    const auto matches = [&type, &target](std::string_view sql_type) {
        return canonical_catalog_sql_type(sql_type)
                == canonical_catalog_sql_type(target)
            || canonical_catalog_sql_type(sql_type)
                == canonical_catalog_sql_type(type.name);
    };
    if (matches(routine.result_sql_type)) return true;
    return std::ranges::any_of(
        sql_parameters(routine),
        [&matches](const routine_parameter* parameter) {
            return matches(parameter->sql_type);
        });
}

[[nodiscard]] inline std::string natural_type_reference_name(
    const option_value& option,
    const collected_program& program,
    std::string_view extension_schema) {
    if (const collected_entity* type = natural_reference_entity(
            option.expression,
            program,
            [](entity_kind kind) {
                return kind == entity_kind::type;
            },
            "base-type option")) {
        return natural_catalog_name(*type, extension_schema);
    }
    return install_resolved_catalog_name(
        catalog_qualified_target(graph_enum_leaf(option.expression)),
        extension_schema);
}

[[nodiscard]] inline base_type_definition_descriptor
natural_base_type_definition_descriptor(
    const collected_entity& type,
    const collected_program& program,
    std::string_view extension_schema) {
    const option_value* internal_length =
        find_option(type, "internal_length");
    if (internal_length == nullptr) {
        throw std::runtime_error(
            "natural base type '" + type.name
            + "' has no .internal_length");
    }
    base_type_definition_descriptor result{
        .target = natural_catalog_name(type, extension_schema),
    };
    result.internal_length =
        graph_enum_leaf(internal_length->expression);
    if (result.internal_length == "variable_length"
        || result.internal_length == "-1") {
        result.internal_length = "variable";
    }
    result.passed_by_value =
        catalog_option_identifier(
            type, "passed_by_value", "false") == "true";
    if (const option_value* value = find_option(type, "alignment")) {
        result.alignment = graph_enum_leaf(value->expression);
        if (result.alignment == "character") result.alignment = "char";
        if (result.alignment == "double_") result.alignment = "double";
    }
    if (const option_value* value = find_option(type, "storage")) {
        result.storage = graph_enum_leaf(value->expression);
    }
    if (const option_value* value = find_option(type, "category")) {
        result.category = graph_unquoted_literal(value->expression);
    }
    result.preferred =
        catalog_option_identifier(type, "preferred", "false") == "true";
    if (const option_value* value = find_option(type, "delimiter")) {
        result.delimiter = graph_unquoted_literal(value->expression);
    }
    result.collatable =
        catalog_option_identifier(type, "collatable", "false") == "true";
    if (const option_value* value = find_option(type, "element")) {
        result.element = natural_type_reference_name(
            *value, program, extension_schema);
    }
    if (const option_value* value = find_option(type, "array")) {
        result.array = natural_type_reference_name(
            *value, program, extension_schema);
    }
    if (const option_value* value = find_option(type, "default_value")) {
        result.default_value =
            canonical_catalog_checked_sql(value->expression);
    }

    for (const collected_entity& role : program.entities) {
        if (role.kind != entity_kind::type_role
            || role.typed_install_owner) {
            continue;
        }
        const collected_entity& routine =
            natural_same_declaration_routine(
                role, program, "base-type callback");
        bool owns = false;
        if (const option_value* for_type =
                find_option(role, "for_type")) {
            const std::size_t separator =
                type.cpp_qualified_name.rfind("::");
            const std::string_view leaf =
                separator == std::string::npos
                ? std::string_view{type.cpp_qualified_name}
                : std::string_view{type.cpp_qualified_name}.substr(
                    separator + 2);
            owns = expression_mentions(
                       for_type->expression,
                       type.cpp_qualified_name)
                || expression_mentions(for_type->expression, leaf);
        } else {
            owns = routine_mentions_base_type(
                routine, type, extension_schema);
        }
        if (!owns) continue;
        const std::string clause =
            base_type_role_clause(role.annotation_name);
        const std::string target =
            natural_catalog_name(routine, extension_schema);
        if (!result.callbacks.emplace(clause, target).second) {
            throw std::runtime_error(
                "natural base type '" + result.target
                + "' has duplicate " + clause + " callbacks");
        }
    }
    if (!result.callbacks.contains("input")
        || !result.callbacks.contains("output")) {
        throw std::runtime_error(
            "natural base type '" + result.target
            + "' requires input and output callbacks");
    }
    return result;
}

[[nodiscard]] inline typed_base_type_statement_descriptor
typed_base_type_statement_descriptor_for(
    const collected_entity& install,
    std::string_view extension_schema) {
    const std::string identity =
        typed_install_sql_identity(install, "base type");
    const std::string context =
        typed_install_context(install, "base type", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() < 3 || tokens[0] != "create"
        || tokens[1] != "type") {
        throw std::runtime_error(
            context + " is not CREATE TYPE");
    }
    typed_base_type_statement_descriptor result{
        .target = install_resolved_catalog_name(
            catalog_qualified_target(tokens[2]), extension_schema),
    };
    if (tokens.size() == 3) return result;
    if (tokens.size() < 6 || tokens[3] != "(") {
        throw std::runtime_error(
            context + " is not a base-type definition");
    }
    const std::size_t close =
        catalog_matching_parenthesis(tokens, 3, context);
    if (close + 1 != tokens.size()) {
        throw std::runtime_error(
            context + " has unsupported trailing syntax");
    }
    base_type_definition_descriptor definition{
        .target = result.target,
    };
    std::set<std::string, std::less<>> seen;
    for (const std::vector<std::string>& clause :
         catalog_top_level_clauses(tokens, 4, close, context)) {
        if (clause.empty()) {
            throw std::runtime_error(
                context + " has an empty base-type clause");
        }
        const std::string& name = clause.front();
        if (!seen.insert(name).second) {
            throw std::runtime_error(
                context + " repeats base-type clause '" + name + "'");
        }
        if (name == "passedbyvalue") {
            if (clause.size() != 1) {
                throw std::runtime_error(
                    context + " has malformed PASSEDBYVALUE");
            }
            definition.passed_by_value = true;
            continue;
        }
        if (clause.size() != 3 || clause[1] != "=") {
            throw std::runtime_error(
                context + " has malformed base-type clause '" + name
                + "'");
        }
        const std::string value =
            graph_unquoted_literal(clause[2]);
        if (name == "input" || name == "output"
            || name == "receive" || name == "send"
            || name == "typmod_in" || name == "typmod_out"
            || name == "analyze" || name == "subscript") {
            definition.callbacks.emplace(
                name,
                install_resolved_catalog_name(
                    catalog_qualified_target(value),
                    extension_schema));
        } else if (name == "internallength") {
            definition.internal_length =
                value == "-1" ? "variable" : value;
        } else if (name == "alignment") {
            definition.alignment = value;
        } else if (name == "storage") {
            definition.storage = value;
        } else if (name == "category") {
            definition.category = value;
        } else if (name == "preferred") {
            definition.preferred = value == "true";
        } else if (name == "delimiter") {
            definition.delimiter = value;
        } else if (name == "collatable") {
            definition.collatable = value == "true";
        } else if (name == "element") {
            definition.element = install_resolved_catalog_name(
                catalog_qualified_target(value), extension_schema);
        } else if (name == "array") {
            definition.array = install_resolved_catalog_name(
                catalog_qualified_target(value), extension_schema);
        } else if (name == "default") {
            definition.default_value = value;
        } else {
            throw std::runtime_error(
                context + " has unsupported base-type clause '"
                + name + "'");
        }
    }
    if (definition.internal_length.empty()
        || !definition.callbacks.contains("input")
        || !definition.callbacks.contains("output")) {
        throw std::runtime_error(
            context + " lacks required base-type clauses");
    }
    result.facet =
        physical_statement_facet::base_type_definition;
    result.definition = std::move(definition);
    return result;
}

inline void reconcile_natural_base_type_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::type
            || entity.typed_install_owner
            || find_option(entity, "internal_length") == nullptr) {
            continue;
        }
        const std::string target =
            natural_catalog_name(entity, extension_schema);
        if (!natural_by_target.emplace(target, index).second) {
            throw std::runtime_error(
                "duplicate natural base-type owner for '" + target + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::map<
        std::string,
        std::set<physical_statement_facet>,
        std::less<>> matched_facets;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::type
            || !install.typed_install_owner) {
            continue;
        }
        const std::vector<std::string> identity_tokens =
            catalog_sql_tokens(
                typed_install_sql_identity(install, "type"));
        if (identity_tokens.size() < 3
            || identity_tokens[0] != "create"
            || identity_tokens[1] != "type") {
            continue;
        }
        const std::string candidate_target =
            install_resolved_catalog_name(
                catalog_qualified_target(identity_tokens[2]),
                extension_schema);
        if (!natural_by_target.contains(candidate_target)) {
            continue;
        }
        const typed_base_type_statement_descriptor descriptor =
            typed_base_type_statement_descriptor_for(
                install, extension_schema);
        const auto natural =
            natural_by_target.find(descriptor.target);
        if (natural == natural_by_target.end()) continue;
        if (!matched_facets[descriptor.target]
                 .insert(descriptor.facet).second) {
            throw std::runtime_error(
                "natural base type '" + descriptor.target
                + "' matches more than one typed "
                + physical_statement_facet_name(descriptor.facet)
                + " install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        if (descriptor.definition) {
            const base_type_definition_descriptor natural_descriptor =
                natural_base_type_definition_descriptor(
                    natural_entity, program, extension_schema);
            if (natural_descriptor != *descriptor.definition) {
                throw std::runtime_error(
                    "natural base type '" + descriptor.target
                    + "' disagrees with its typed install-owner "
                      "definition "
                    + typed_install_context(
                        install,
                        "base type",
                        typed_install_sql_identity(
                            install, "base type")));
            }
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            descriptor.facet,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct catalog_numbered_reference {
    std::uint16_t number{};
    std::string expression;
};

[[nodiscard]] inline std::string_view trim_catalog_expression(
    std::string_view value) noexcept {
    while (!value.empty()
           && std::isspace(
               static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && std::isspace(
               static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] inline std::vector<catalog_numbered_reference>
parse_catalog_numbered_references(
    std::string_view expression,
    std::string_view helper_name) {
    std::vector<catalog_numbered_reference> result;
    std::size_t offset = 0;
    const std::string needle = std::string{helper_name} + "(";
    while ((offset = expression.find(needle, offset))
           != std::string_view::npos) {
        const std::size_t open = offset + needle.size() - 1;
        std::size_t comma = std::string_view::npos;
        std::size_t close = std::string_view::npos;
        int depth = 0;
        for (std::size_t index = open;
             index < expression.size(); ++index) {
            if (expression[index] == '(') {
                ++depth;
            } else if (expression[index] == ')') {
                if (--depth == 0) {
                    close = index;
                    break;
                }
            } else if (expression[index] == ',' && depth == 1
                       && comma == std::string_view::npos) {
                comma = index;
            }
        }
        if (comma == std::string_view::npos
            || close == std::string_view::npos) {
            throw std::runtime_error(
                "malformed " + std::string{helper_name}
                + " declaration");
        }
        const std::string number_text{
            trim_catalog_expression(
                expression.substr(open + 1, comma - open - 1))};
        std::uint32_t number{};
        const auto [end, error] = std::from_chars(
            number_text.data(),
            number_text.data() + number_text.size(),
            number);
        if (error != std::errc{}
            || end != number_text.data() + number_text.size()
            || number == 0
            || number > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error(
                "invalid operator-class entry number '"
                + number_text + "'");
        }
        result.push_back(catalog_numbered_reference{
            .number = static_cast<std::uint16_t>(number),
            .expression = std::string{trim_catalog_expression(
                expression.substr(
                    comma + 1, close - comma - 1))},
        });
        offset = close + 1;
    }
    if (result.empty()) {
        throw std::runtime_error(
            std::string{helper_name} + " list is empty");
    }
    std::set<std::uint16_t> numbers;
    for (const catalog_numbered_reference& entry : result) {
        if (!numbers.insert(entry.number).second) {
            throw std::runtime_error(
                "duplicate operator-class entry number "
                + std::to_string(entry.number));
        }
    }
    return result;
}

struct opclass_entry_descriptor {
    std::uint16_t number{};
    std::string target;
    std::vector<std::string> argument_types;

    [[nodiscard]] bool operator==(
        const opclass_entry_descriptor&) const = default;
};

struct opclass_descriptor {
    std::string target;
    std::string indexed_type;
    std::string access_method;
    bool is_default{};
    std::string family;
    std::string storage;
    std::vector<opclass_entry_descriptor> operators;
    std::vector<opclass_entry_descriptor> functions;

    [[nodiscard]] bool operator==(
        const opclass_descriptor&) const = default;
};

[[nodiscard]] inline std::vector<std::string>
canonical_routine_argument_types(
    const collected_entity& routine,
    std::string_view extension_schema) {
    std::vector<std::string> result;
    for (const routine_parameter* parameter :
         sql_parameters(routine)) {
        result.push_back(install_resolved_catalog_name(
            canonical_catalog_sql_type(parameter->sql_type),
            extension_schema));
    }
    return result;
}

[[nodiscard]] inline const collected_entity*
natural_type_for_catalog_name(
    const collected_program& program,
    std::string_view catalog_name,
    std::string_view extension_schema) {
    const collected_entity* result = nullptr;
    for (const collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::type
            || entity.typed_install_owner
            || natural_catalog_name(entity, extension_schema)
                != catalog_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "more than one natural type resolves to catalog name '"
                + std::string{catalog_name} + "'");
        }
        result = &entity;
    }
    return result;
}

[[nodiscard]] inline opclass_descriptor natural_opclass_descriptor(
    const collected_entity& opclass,
    const collected_program& program,
    std::string_view extension_schema) {
    const option_value* operators =
        find_option(opclass, "operators");
    const option_value* functions =
        find_option(opclass, "functions");
    if (operators == nullptr || functions == nullptr) {
        throw std::runtime_error(
            "natural operator class '" + opclass.name
            + "' requires operators and functions");
    }
    opclass_descriptor result{
        .target = natural_catalog_name(
            opclass, extension_schema),
        .access_method =
            graph_enum_leaf(
                find_option(opclass, "access_method")->expression),
        .is_default =
            catalog_option_identifier(
                opclass, "is_default", "false") == "true",
    };
    if (const option_value* family =
            find_option(opclass, "family")) {
        result.family = graph_enum_leaf(family->expression);
    }
    if (const option_value* storage =
            find_option(opclass, "storage")) {
        result.storage = install_resolved_catalog_name(
            canonical_catalog_sql_type(
                graph_enum_leaf(storage->expression)),
            extension_schema);
    }
    for (const catalog_numbered_reference& entry :
         parse_catalog_numbered_references(
             operators->expression, "opclass_operator")) {
        const collected_entity* routine = natural_reference_entity(
            entry.expression,
            program,
            [](entity_kind kind) {
                return kind == entity_kind::function
                    || kind == entity_kind::sql_function;
            },
            "operator-class operator routine");
        if (routine == nullptr) {
            throw std::runtime_error(
                "natural operator class '" + result.target
                + "' has an unresolved operator routine");
        }
        const collected_entity* op =
            natural_operator_for_cpp_name(
                routine->cpp_qualified_name, program);
        if (op == nullptr) {
            throw std::runtime_error(
                "natural operator class '" + result.target
                + "' routine '" + routine->cpp_qualified_name
                + "' has no operator annotation");
        }
        std::vector<std::string> argument_types =
            canonical_routine_argument_types(
                *routine, extension_schema);
        if (argument_types.empty() || argument_types.size() > 2) {
            throw std::runtime_error(
                "natural operator class '" + result.target
                + "' has a non-unary/binary operator");
        }
        for (const std::string& type : argument_types) {
            if (result.indexed_type.empty()) {
                result.indexed_type = type;
            } else if (result.indexed_type != type) {
                throw std::runtime_error(
                    "natural operator class '" + result.target
                    + "' mixes indexed operand types");
            }
        }
        result.operators.push_back(opclass_entry_descriptor{
            .number = entry.number,
            .target = natural_catalog_name(
                *op, extension_schema),
            .argument_types = std::move(argument_types),
        });
    }
    for (const catalog_numbered_reference& entry :
         parse_catalog_numbered_references(
             functions->expression, "opclass_function")) {
        const collected_entity* routine = natural_reference_entity(
            entry.expression,
            program,
            [](entity_kind kind) {
                return kind == entity_kind::function
                    || kind == entity_kind::sql_function;
            },
            "operator-class support routine");
        if (routine == nullptr) {
            throw std::runtime_error(
                "natural operator class '" + result.target
                + "' has an unresolved support routine");
        }
        std::vector<std::string> argument_types =
            canonical_routine_argument_types(
                *routine, extension_schema);
        if (argument_types.empty()
            || (!result.indexed_type.empty()
                && argument_types.front()
                    != result.indexed_type)) {
            throw std::runtime_error(
                "natural operator class '" + result.target
                + "' support routine does not accept its indexed type");
        }
        result.functions.push_back(opclass_entry_descriptor{
            .number = entry.number,
            .target = natural_catalog_name(
                *routine, extension_schema),
            .argument_types = std::move(argument_types),
        });
    }
    if (result.indexed_type.empty()) {
        throw std::runtime_error(
            "natural operator class '" + result.target
            + "' has no indexed type");
    }
    if (const collected_entity* indexed_type =
            natural_type_for_catalog_name(
                program, result.indexed_type, extension_schema)) {
        const std::string installed_type =
            natural_catalog_name(*indexed_type, extension_schema);
        const std::size_t type_schema_separator =
            installed_type.rfind('.');
        result.target =
            installed_type.substr(0, type_schema_separator)
            + "." + opclass.name;
    }
    const auto by_number =
        [](const opclass_entry_descriptor& left,
           const opclass_entry_descriptor& right) {
            return left.number < right.number;
        };
    std::ranges::sort(result.operators, by_number);
    std::ranges::sort(result.functions, by_number);
    return result;
}

[[nodiscard]] inline std::uint16_t catalog_uint16(
    std::string_view value,
    std::string_view context) {
    std::uint32_t result{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (error != std::errc{}
        || end != value.data() + value.size()
        || result == 0
        || result > std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(
            std::string{context}
            + " requires a positive uint16 entry number");
    }
    return static_cast<std::uint16_t>(result);
}

[[nodiscard]] inline std::vector<std::string>
typed_opclass_argument_types(
    const std::vector<std::string>& clause,
    std::size_t open,
    std::string_view context,
    std::string_view extension_schema) {
    const std::size_t close =
        catalog_matching_parenthesis(clause, open, context);
    if (close + 1 != clause.size()) {
        throw std::runtime_error(
            std::string{context}
            + " has unsupported trailing entry syntax");
    }
    std::vector<std::string> result;
    if (open + 1 == close) return result;
    for (const std::vector<std::string>& argument :
         catalog_top_level_clauses(
             clause, open + 1, close, context)) {
        result.push_back(install_resolved_catalog_name(
            canonical_catalog_sql_type(
                catalog_token_fragment(
                    argument, 0, argument.size())),
            extension_schema));
    }
    return result;
}

[[nodiscard]] inline opclass_descriptor typed_opclass_descriptor(
    const collected_entity& install,
    std::string_view extension_schema) {
    const std::string identity =
        typed_install_sql_identity(install, "operator class");
    const std::string context =
        typed_install_context(
            install, "operator class", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() < 10 || tokens[0] != "create"
        || tokens[1] != "operator"
        || tokens[2] != "class") {
        throw std::runtime_error(
            context + " is not CREATE OPERATOR CLASS");
    }
    opclass_descriptor result{
        .target = install_resolved_catalog_name(
            catalog_qualified_target(tokens[3]), extension_schema),
    };
    std::size_t cursor = 4;
    if (cursor < tokens.size()
        && tokens[cursor] == "default") {
        result.is_default = true;
        ++cursor;
    }
    if (cursor + 4 >= tokens.size()
        || tokens[cursor++] != "for"
        || tokens[cursor++] != "type") {
        throw std::runtime_error(
            context + " has no indexed type");
    }
    result.indexed_type = install_resolved_catalog_name(
        canonical_catalog_sql_type(tokens[cursor++]),
        extension_schema);
    if (cursor >= tokens.size()
        || tokens[cursor++] != "using"
        || cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no access method");
    }
    result.access_method = tokens[cursor++];
    if (cursor < tokens.size()
        && tokens[cursor] == "family") {
        if (++cursor >= tokens.size()) {
            throw std::runtime_error(
                context + " has no operator family");
        }
        result.family = tokens[cursor++];
    }
    if (cursor >= tokens.size()
        || tokens[cursor++] != "as"
        || cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no operator-class entries");
    }
    for (const std::vector<std::string>& clause :
         catalog_top_level_clauses(
             tokens, cursor, tokens.size(), context)) {
        if (clause.size() < 2) {
            throw std::runtime_error(
                context + " has a malformed operator-class entry");
        }
        if (clause[0] == "storage") {
            result.storage = install_resolved_catalog_name(
                canonical_catalog_sql_type(
                    catalog_token_fragment(
                        clause, 1, clause.size())),
                extension_schema);
            continue;
        }
        if ((clause[0] != "operator"
             && clause[0] != "function")
            || clause.size() < 3) {
            throw std::runtime_error(
                context + " has an unsupported operator-class entry");
        }
        opclass_entry_descriptor entry{
            .number = catalog_uint16(clause[1], context),
            .target = install_resolved_catalog_name(
                catalog_qualified_target(clause[2]),
                extension_schema),
        };
        if (clause.size() > 3) {
            if (clause[3] != "(") {
                throw std::runtime_error(
                    context + " has malformed entry arguments");
            }
            entry.argument_types = typed_opclass_argument_types(
                clause, 3, context, extension_schema);
        } else if (clause[0] == "operator") {
            entry.argument_types = {
                result.indexed_type,
                result.indexed_type,
            };
        } else {
            throw std::runtime_error(
                context + " support function requires a signature");
        }
        if (clause[0] == "operator") {
            result.operators.push_back(std::move(entry));
        } else {
            result.functions.push_back(std::move(entry));
        }
    }
    const auto by_number =
        [](const opclass_entry_descriptor& left,
           const opclass_entry_descriptor& right) {
            return left.number < right.number;
        };
    std::ranges::sort(result.operators, by_number);
    std::ranges::sort(result.functions, by_number);
    return result;
}

inline void reconcile_natural_opclass_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::opclass
            || entity.typed_install_owner) {
            continue;
        }
        const opclass_descriptor descriptor =
            natural_opclass_descriptor(
                entity, program, extension_schema);
        const std::string& target = descriptor.target;
        if (const collected_entity* indexed_type =
                natural_type_for_catalog_name(
                    program,
                    descriptor.indexed_type,
                    extension_schema)) {
            entity.schema = indexed_type->schema;
        }
        if (!natural_by_target.emplace(target, index).second) {
            throw std::runtime_error(
                "duplicate natural operator-class owner for '"
                + target + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::opclass
            || !install.typed_install_owner) {
            continue;
        }
        const opclass_descriptor descriptor =
            typed_opclass_descriptor(install, extension_schema);
        const auto natural =
            natural_by_target.find(descriptor.target);
        if (natural == natural_by_target.end()) continue;
        if (!matched.insert(descriptor.target).second) {
            throw std::runtime_error(
                "natural operator class '" + descriptor.target
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        const opclass_descriptor natural_descriptor =
            natural_opclass_descriptor(
                natural_entity, program, extension_schema);
        if (natural_descriptor != descriptor) {
            throw std::runtime_error(
                "natural operator class '" + descriptor.target
                + "' disagrees with its typed install-owner descriptor "
                + typed_install_context(
                    install,
                    "operator class",
                    typed_install_sql_identity(
                        install, "operator class")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct access_method_descriptor {
    std::string name;
    std::string kind;
    std::string handler;

    [[nodiscard]] bool operator==(
        const access_method_descriptor&) const = default;
};

[[nodiscard]] inline access_method_descriptor
natural_access_method_descriptor(
    const collected_entity& access_method,
    const collected_program& program,
    std::string_view extension_schema) {
    const collected_entity& handler =
        natural_same_declaration_routine(
            access_method, program, "access method");
    if (handler.kind != entity_kind::function) {
        throw std::runtime_error(
            "natural access method '" + access_method.name
            + "' handler is not a function");
    }
    return access_method_descriptor{
        .name = access_method.name,
        .kind = catalog_option_identifier(
            access_method, "kind", "table"),
        .handler = natural_catalog_name(
            handler, extension_schema),
    };
}

[[nodiscard]] inline access_method_descriptor
typed_access_method_descriptor(
    const collected_entity& install,
    std::string_view extension_schema) {
    const std::string identity =
        typed_install_sql_identity(install, "access method");
    const std::string context =
        typed_install_context(
            install, "access method", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() != 8 || tokens[0] != "create"
        || tokens[1] != "access" || tokens[2] != "method"
        || tokens[4] != "type" || tokens[6] != "handler") {
        throw std::runtime_error(
            context + " is not a supported CREATE ACCESS METHOD");
    }
    if (tokens[5] != "table" && tokens[5] != "index") {
        throw std::runtime_error(
            context + " has an unsupported access-method kind");
    }
    return access_method_descriptor{
        .name = tokens[3],
        .kind = tokens[5],
        .handler = install_resolved_catalog_name(
            catalog_qualified_target(tokens[7]),
            extension_schema),
    };
}

inline void reconcile_natural_access_method_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_name;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::access_method
            || entity.typed_install_owner) {
            continue;
        }
        if (!natural_by_name.emplace(entity.name, index).second) {
            throw std::runtime_error(
                "duplicate natural access-method owner for '"
                + entity.name + "'");
        }
    }
    if (natural_by_name.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::access_method
            || !install.typed_install_owner) {
            continue;
        }
        const access_method_descriptor descriptor =
            typed_access_method_descriptor(
                install, extension_schema);
        const auto natural =
            natural_by_name.find(descriptor.name);
        if (natural == natural_by_name.end()) continue;
        if (!matched.insert(descriptor.name).second) {
            throw std::runtime_error(
                "natural access method '" + descriptor.name
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        if (natural_access_method_descriptor(
                natural_entity, program, extension_schema)
            != descriptor) {
            throw std::runtime_error(
                "natural access method '" + descriptor.name
                + "' disagrees with its typed install-owner descriptor "
                + typed_install_context(
                    install,
                    "access method",
                    typed_install_sql_identity(
                        install, "access method")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

[[nodiscard]] inline std::string catalog_quoted_sql_literal(
    std::string_view value) {
    std::string result{"'"};
    for (const char character : value) {
        result.push_back(character);
        if (character == '\'') result.push_back('\'');
    }
    result.push_back('\'');
    return result;
}

[[nodiscard]] inline std::string catalog_dependencies_sql(
    std::string_view access_method,
    std::vector<std::pair<std::string, std::string>> relations) {
    std::ranges::sort(relations);
    relations.erase(
        std::unique(relations.begin(), relations.end()),
        relations.end());
    if (relations.empty()) {
        throw std::runtime_error(
            "AM catalog dependency action resolves no relation roots");
    }
    const std::string& schema = relations.front().first;
    if (std::ranges::any_of(
            relations,
            [&schema](const auto& relation) {
                return relation.first != schema;
            })) {
        throw std::runtime_error(
            "AM catalog dependency roots must share one schema");
    }
    std::string result =
        "INSERT INTO pg_catalog.pg_depend (\n"
        "    classid, objid, objsubid,\n"
        "    refclassid, refobjid, refobjsubid, deptype)\n"
        "WITH desired(relid) AS (\n"
        "    SELECT c.oid\n"
        "      FROM pg_catalog.pg_class AS c\n"
        "      JOIN pg_catalog.pg_namespace AS n\n"
        "        ON n.oid = c.relnamespace\n"
        "     WHERE n.nspname = "
        + (schema == "$extension"
               ? std::string{"pg_catalog.current_schema()"}
               : catalog_quoted_sql_literal(schema))
        + "\n"
        "       AND c.relname IN (\n";
    for (std::size_t index = 0; index < relations.size(); ++index) {
        result += "           "
            + catalog_quoted_sql_literal(relations[index].second)
            + (index + 1 == relations.size() ? ")\n" : ",\n");
    }
    result +=
        "),\n"
        "edges AS (\n"
        "    SELECT 'pg_catalog.pg_am'::regclass::oid,\n"
        "           am.oid,\n"
        "           0,\n"
        "           'pg_catalog.pg_class'::regclass::oid,\n"
        "           desired.relid,\n"
        "           0,\n"
        "           'n'::\"char\"\n"
        "      FROM desired\n"
        "      CROSS JOIN pg_catalog.pg_am AS am\n"
        "     WHERE am.amname = "
        + catalog_quoted_sql_literal(access_method)
        + "\n"
        ")\n"
        "SELECT * FROM edges\n"
        "EXCEPT\n"
        "SELECT classid, objid, objsubid,\n"
        "       refclassid, refobjid, refobjsubid, deptype\n"
        "  FROM pg_catalog.pg_depend;";
    return result;
}

struct catalog_dependencies_descriptor {
    std::string access_method;
    std::vector<std::string> statement_tokens;

    [[nodiscard]] bool operator==(
        const catalog_dependencies_descriptor&) const = default;
};

[[nodiscard]] inline std::vector<std::string>
canonical_catalog_dependencies_tokens(
    std::vector<std::string> tokens,
    std::string_view context) {
    std::optional<std::size_t> list_open;
    for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens[index] == "c.relname"
            && tokens[index + 1] == "in"
            && tokens[index + 2] == "(") {
            if (list_open) {
                throw std::runtime_error(
                    std::string{context}
                    + " has more than one c.relname IN list");
            }
            list_open = index + 2;
        }
    }
    if (!list_open) {
        throw std::runtime_error(
            std::string{context}
            + " has no c.relname IN list");
    }
    const std::size_t close =
        catalog_matching_parenthesis(tokens, *list_open, context);
    const auto clauses = catalog_top_level_clauses(
        tokens, *list_open + 1, close, context);
    std::vector<std::string> names;
    names.reserve(clauses.size());
    for (const auto& clause : clauses) {
        if (clause.size() != 1
            || clause.front().size() < 2
            || clause.front().front() != '\''
            || clause.front().back() != '\'') {
            throw std::runtime_error(
                std::string{context}
                + " has a non-literal c.relname IN entry");
        }
        names.push_back(clause.front());
    }
    std::ranges::sort(names);
    if (std::ranges::adjacent_find(names) != names.end()) {
        throw std::runtime_error(
            std::string{context}
            + " repeats a c.relname IN entry");
    }
    std::vector<std::string> result;
    result.reserve(tokens.size());
    result.insert(
        result.end(), tokens.begin(),
        tokens.begin()
            + static_cast<std::ptrdiff_t>(*list_open + 1));
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0) result.emplace_back(",");
        result.push_back(std::move(names[index]));
    }
    result.insert(
        result.end(),
        tokens.begin() + static_cast<std::ptrdiff_t>(close),
        tokens.end());
    return result;
}

[[nodiscard]] inline std::vector<
    std::pair<std::string, std::string>>
natural_catalog_dependency_relations(
    const collected_entity& owner,
    const collected_program& program,
    std::string_view extension_schema) {
    const option_value* roots = find_option(owner, "roots");
    if (roots == nullptr) {
        throw std::runtime_error(
            "natural AM catalog dependencies require .roots");
    }
    const std::vector<std::string_view> reference_tokens =
        declaration_reference_tokens(roots->expression);
    std::vector<const collected_entity*> resolved_roots;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.typed_install_owner
            || (candidate.kind != entity_kind::table
                && candidate.kind != entity_kind::sequence)) {
            continue;
        }
        if (std::ranges::any_of(
                reference_tokens,
                [&candidate](std::string_view token) {
                    return declaration_reference_matches(
                        candidate.cpp_qualified_name, token);
                })) {
            resolved_roots.push_back(&candidate);
        }
    }
    if (resolved_roots.empty()) {
        throw std::runtime_error(
            "natural AM catalog dependencies resolve no .roots");
    }

    std::vector<std::pair<std::string, std::string>> result;
    for (const collected_entity* root : resolved_roots) {
        const std::string schema =
            root->schema == "$extension"
            ? std::string{extension_schema}
            : root->schema;
        result.emplace_back(schema, root->name);
        if (root->kind != entity_kind::table) continue;
        for (const collected_entity& child : program.entities) {
            if (child.typed_install_owner
                || (child.kind != entity_kind::primary_key
                    && child.kind != entity_kind::unique
                    && child.kind != entity_kind::index)
                || !natural_child_of(child, *root)) {
                continue;
            }
            result.emplace_back(
                schema,
                relation_child_catalog_name(child, *root));
        }
    }
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

[[nodiscard]] inline catalog_dependencies_descriptor
natural_catalog_dependencies_descriptor(
    const collected_entity& owner,
    const collected_program& program,
    std::string_view extension_schema) {
    const collected_entity* access_method = nullptr;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.kind != entity_kind::access_method
            || candidate.typed_install_owner
            || candidate.cpp_qualified_name
                != owner.cpp_qualified_name) {
            continue;
        }
        if (access_method != nullptr) {
            throw std::runtime_error(
                "natural AM catalog dependencies resolve more than "
                "one access method");
        }
        access_method = &candidate;
    }
    if (access_method == nullptr) {
        throw std::runtime_error(
            "natural AM catalog dependencies have no same-declaration "
            "access method");
    }
    const auto relations =
        natural_catalog_dependency_relations(
            owner, program, extension_schema);
    return catalog_dependencies_descriptor{
        .access_method = access_method->name,
        .statement_tokens =
            canonical_catalog_dependencies_tokens(
                catalog_sql_tokens(
                    catalog_dependencies_sql(
                        access_method->name, relations)),
                "natural AM catalog dependencies"),
    };
}

[[nodiscard]] inline std::string typed_catalog_dependency_am(
    const std::vector<std::string>& tokens,
    std::string_view context) {
    for (std::size_t index = 0; index + 2 < tokens.size(); ++index) {
        if (tokens[index] == "am.amname"
            && tokens[index + 1] == "=") {
            return graph_unquoted_literal(tokens[index + 2]);
        }
    }
    throw std::runtime_error(
        std::string{context}
        + " has no fixed pg_am.amname predicate");
}

[[nodiscard]] inline catalog_dependencies_descriptor
typed_catalog_dependencies_descriptor(
    const collected_entity& install) {
    const std::string identity =
        typed_install_sql_identity(
            install, "AM catalog dependencies");
    const std::string context =
        typed_install_context(
            install, "AM catalog dependencies", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() < 8 || tokens[0] != "insert"
        || tokens[1] != "into"
        || tokens[2] != "pg_catalog.pg_depend") {
        throw std::runtime_error(
            context
            + " is not a supported pg_depend INSERT");
    }
    return catalog_dependencies_descriptor{
        .access_method =
            typed_catalog_dependency_am(tokens, context),
        .statement_tokens =
            canonical_catalog_dependencies_tokens(
                tokens, context),
    };
}

inline void reconcile_natural_catalog_dependency_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_access_method;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::catalog_dependencies
            || entity.typed_install_owner) {
            continue;
        }
        const catalog_dependencies_descriptor descriptor =
            natural_catalog_dependencies_descriptor(
                entity, program, extension_schema);
        if (!natural_by_access_method.emplace(
                descriptor.access_method, index).second) {
            throw std::runtime_error(
                "duplicate natural AM catalog dependency owner for '"
                + descriptor.access_method + "'");
        }
    }
    if (natural_by_access_method.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::catalog_dependencies
            || !install.typed_install_owner) {
            continue;
        }
        const catalog_dependencies_descriptor descriptor =
            typed_catalog_dependencies_descriptor(install);
        const auto natural =
            natural_by_access_method.find(
                descriptor.access_method);
        if (natural == natural_by_access_method.end()) continue;
        if (!matched.insert(descriptor.access_method).second) {
            throw std::runtime_error(
                "natural AM catalog dependency owner for '"
                + descriptor.access_method
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        if (natural_catalog_dependencies_descriptor(
                natural_entity, program, extension_schema)
            != descriptor) {
            throw std::runtime_error(
                "natural AM catalog dependency owner for '"
                + descriptor.access_method
                + "' disagrees with its typed install-owner SQL "
                + typed_install_context(
                    install,
                    "AM catalog dependencies",
                    typed_install_sql_identity(
                        install, "AM catalog dependencies")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct view_descriptor {
    std::string target;
    std::string create_mode{"create"};
    bool temporary{};
    bool recursive{};
    bool security_barrier{};
    bool security_invoker{};
    std::vector<std::string> body_tokens;

    [[nodiscard]] bool operator==(
        const view_descriptor&) const = default;
};

[[nodiscard]] inline bool catalog_boolean_value(
    std::string_view value,
    std::string_view context) {
    if (value == "true" || value == "on" || value == "1") {
        return true;
    }
    if (value == "false" || value == "off" || value == "0") {
        return false;
    }
    throw std::runtime_error(
        std::string{context}
        + " has an unsupported boolean value '" + std::string{value}
        + "'");
}

[[nodiscard]] inline view_descriptor natural_view_descriptor(
    const collected_entity& view,
    std::string_view extension_schema) {
    view_descriptor result{
        .target = natural_catalog_name(view, extension_schema),
        .create_mode =
            catalog_option_identifier(view, "create", "create"),
        .temporary =
            catalog_option_identifier(view, "temporary", "false")
                == "true",
        .recursive =
            catalog_option_identifier(view, "recursive", "false")
                == "true",
        .security_barrier =
            catalog_option_identifier(
                view, "security_barrier", "false") == "true",
        .security_invoker =
            catalog_option_identifier(
                view, "security_invoker", "false") == "true",
        .body_tokens = catalog_sql_tokens(
            fmgr_reconciliation_detail::unquote_cpp_literal(
                view.initializer_expression)),
    };
    if (result.create_mode == "if_not_exists") {
        throw std::runtime_error(
            "natural view '" + result.target
            + "' requests unsupported IF NOT EXISTS");
    }
    if (result.create_mode != "create"
        && result.create_mode != "or_replace") {
        throw std::runtime_error(
            "natural view '" + result.target
            + "' has an unsupported create mode");
    }
    if (result.body_tokens.empty()) {
        throw std::runtime_error(
            "natural view '" + result.target + "' has an empty body");
    }
    return result;
}

inline void assign_typed_view_option(
    view_descriptor& descriptor,
    const std::vector<std::string>& clause,
    std::string_view context) {
    std::string expression =
        catalog_token_fragment(clause, 0, clause.size());
    std::erase_if(
        expression,
        [](const unsigned char value) {
            return std::isspace(value);
        });
    const std::size_t equals = expression.find('=');
    const std::string name = expression.substr(0, equals);
    const bool enabled =
        equals == std::string::npos
        ? true
        : catalog_boolean_value(
            std::string_view{expression}.substr(equals + 1),
            context);
    if (name == "security_barrier") {
        descriptor.security_barrier = enabled;
    } else if (name == "security_invoker") {
        descriptor.security_invoker = enabled;
    } else {
        throw std::runtime_error(
            std::string{context}
            + " has unsupported view option '" + name + "'");
    }
}

[[nodiscard]] inline view_descriptor typed_view_descriptor(
    const collected_entity& install,
    std::string_view extension_schema) {
    const std::string identity =
        typed_install_sql_identity(install, "view");
    const std::string context =
        typed_install_context(install, "view", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() < 5 || tokens[0] != "create") {
        throw std::runtime_error(
            context + " is not a supported CREATE VIEW");
    }
    view_descriptor result;
    std::size_t cursor = 1;
    if (cursor + 1 < tokens.size()
        && tokens[cursor] == "or"
        && tokens[cursor + 1] == "replace") {
        result.create_mode = "or_replace";
        cursor += 2;
    }
    if (cursor < tokens.size()
        && (tokens[cursor] == "temporary"
            || tokens[cursor] == "temp")) {
        result.temporary = true;
        ++cursor;
    }
    if (cursor < tokens.size()
        && tokens[cursor] == "recursive") {
        result.recursive = true;
        ++cursor;
    }
    if (cursor >= tokens.size() || tokens[cursor] != "view") {
        throw std::runtime_error(
            context + " is not a supported CREATE VIEW");
    }
    ++cursor;
    if (cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no view target");
    }
    result.target = install_resolved_catalog_name(
        catalog_qualified_target(tokens[cursor++]),
        extension_schema);
    if (cursor < tokens.size() && tokens[cursor] == "(") {
        throw std::runtime_error(
            context
            + " uses an explicit view column list, which has no "
              "natural annotation representation");
    }
    if (cursor < tokens.size() && tokens[cursor] == "with") {
        ++cursor;
        if (cursor >= tokens.size() || tokens[cursor] != "(") {
            throw std::runtime_error(
                context + " has a malformed WITH option list");
        }
        const std::size_t close =
            catalog_matching_parenthesis(tokens, cursor, context);
        const auto clauses =
            catalog_top_level_clauses(
                tokens, cursor + 1, close, context);
        if (clauses.empty()) {
            throw std::runtime_error(
                context + " has an empty WITH option list");
        }
        for (const auto& clause : clauses) {
            assign_typed_view_option(result, clause, context);
        }
        cursor = close + 1;
    }
    if (cursor >= tokens.size() || tokens[cursor] != "as") {
        throw std::runtime_error(
            context + " has no AS query body");
    }
    ++cursor;
    result.body_tokens.assign(
        tokens.begin() + static_cast<std::ptrdiff_t>(cursor),
        tokens.end());
    if (result.body_tokens.empty()) {
        throw std::runtime_error(
            context + " has an empty query body");
    }
    return result;
}

[[nodiscard]] inline std::string view_descriptor_key(
    const view_descriptor& descriptor) {
    return descriptor.target;
}

inline void reconcile_natural_view_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::view
            || entity.typed_install_owner) {
            continue;
        }
        const view_descriptor descriptor =
            natural_view_descriptor(entity, extension_schema);
        const std::string key = view_descriptor_key(descriptor);
        if (!natural_by_target.emplace(key, index).second) {
            throw std::runtime_error(
                "duplicate natural view owner for '" + key + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::view
            || !install.typed_install_owner) {
            continue;
        }
        const view_descriptor descriptor =
            typed_view_descriptor(install, extension_schema);
        const std::string key = view_descriptor_key(descriptor);
        const auto natural = natural_by_target.find(key);
        if (natural == natural_by_target.end()) continue;
        if (!matched.insert(key).second) {
            throw std::runtime_error(
                "natural view owner '" + key
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        if (natural_view_descriptor(
                natural_entity, extension_schema) != descriptor) {
            throw std::runtime_error(
                "natural view owner '" + key
                + "' disagrees with its typed install-owner descriptor "
                + typed_install_context(
                    install,
                    "view",
                    typed_install_sql_identity(install, "view")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct revoke_descriptor {
    std::string object_kind;
    std::string target;
    std::set<std::string, std::less<>> privileges;
    std::string role;
    bool grant_option{};

    [[nodiscard]] bool operator==(
        const revoke_descriptor&) const = default;
};

[[nodiscard]] inline std::set<std::string, std::less<>>
natural_acl_privileges(const collected_entity& revoke) {
    const option_value* option =
        find_option(revoke, "privileges");
    const std::string expression = option == nullptr
        ? "execute"
        : option->expression;
    static constexpr std::string_view names[] = {
        "select", "insert", "update", "delete_", "truncate",
        "references", "trigger", "usage", "execute", "create",
        "connect", "temporary"};
    std::set<std::string, std::less<>> result;
    if (expression_mentions(expression, "all")) {
        result.insert("all");
        return result;
    }
    for (const std::string_view name : names) {
        if (expression_mentions(expression, name)) {
            result.insert(
                name == "delete_" ? "delete" : std::string{name});
        }
    }
    if (result.empty()) {
        throw std::runtime_error(
            "natural ACL owner has no recognized privileges");
    }
    return result;
}

[[nodiscard]] inline const collected_entity&
natural_acl_parent(
    const collected_entity& acl,
    const collected_program& program) {
    const collected_entity* result = nullptr;
    for (const collected_entity& candidate : program.entities) {
        if (candidate.typed_install_owner || candidate.companion) {
            continue;
        }
        const bool same =
            candidate.cpp_qualified_name == acl.cpp_qualified_name;
        const bool enclosing =
            candidate.cpp_qualified_name == acl.enclosing_cpp_name;
        if (!same && !enclosing) continue;
        if (result != nullptr) {
            throw std::runtime_error(
                "natural ACL owner '" + acl.cpp_qualified_name
                + "' resolves more than one parent");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "natural ACL owner '" + acl.cpp_qualified_name
            + "' has no parent");
    }
    return *result;
}

[[nodiscard]] inline std::string acl_role_name(
    const collected_entity& acl,
    std::string_view option_name) {
    const std::string value =
        catalog_option_identifier(acl, option_name, "public");
    return value == "public_role" ? "public" : value;
}

inline void normalize_function_all_privileges(
    revoke_descriptor& descriptor) {
    if ((descriptor.object_kind == "function"
         || descriptor.object_kind == "procedure")
        && descriptor.privileges
            == std::set<std::string, std::less<>>{"all"}) {
        descriptor.privileges = {"execute"};
    }
}

[[nodiscard]] inline revoke_descriptor natural_revoke_descriptor(
    const collected_entity& revoke,
    const collected_program& program,
    std::string_view extension_schema) {
    const collected_entity& parent =
        natural_acl_parent(revoke, program);
    revoke_descriptor result{
        .privileges = natural_acl_privileges(revoke),
        .role = acl_role_name(revoke, "from"),
        .grant_option =
            catalog_option_identifier(
                revoke, "grant_option", "false") == "true",
    };
    if (const option_value* scope = find_option(revoke, "scope")) {
        const std::string value =
            graph_enum_leaf(scope->expression);
        if (value != "object") {
            if (parent.kind != entity_kind::schema) {
                throw std::runtime_error(
                    "schema-wide natural revoke has no schema parent");
            }
            if (value == "all_tables_in_schema") {
                result.object_kind = "all tables in schema";
            } else if (value == "all_sequences_in_schema") {
                result.object_kind = "all sequences in schema";
            } else if (value == "all_functions_in_schema") {
                result.object_kind = "all functions in schema";
            } else {
                throw std::runtime_error(
                    "unsupported natural revoke scope '" + value + "'");
            }
            result.target = parent.name;
            normalize_function_all_privileges(result);
            return result;
        }
    }
    switch (parent.kind) {
        case entity_kind::function:
        case entity_kind::sql_function:
            result.object_kind = "function";
            break;
        case entity_kind::procedure:
            result.object_kind = "procedure";
            break;
        case entity_kind::schema:
            result.object_kind = "schema";
            break;
        case entity_kind::sequence:
            result.object_kind = "sequence";
            break;
        case entity_kind::table:
            result.object_kind = "table";
            break;
        case entity_kind::view:
            result.object_kind = "table";
            break;
        case entity_kind::type:
            result.object_kind = "type";
            break;
        default:
            throw std::runtime_error(
                "natural revoke has an unsupported parent kind");
    }
    result.target =
        parent.kind == entity_kind::schema
        ? parent.name
        : natural_catalog_name(parent, extension_schema);
    if (parent.kind == entity_kind::function
        || parent.kind == entity_kind::sql_function
        || parent.kind == entity_kind::procedure) {
        result.target += "(";
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(parent);
        for (std::size_t index = 0;
             index < parameters.size(); ++index) {
            if (index != 0) result.target += ",";
            result.target += install_resolved_catalog_name(
                canonical_catalog_sql_type(
                    parameters[index]->sql_type),
                extension_schema);
        }
        result.target += ")";
    }
    normalize_function_all_privileges(result);
    return result;
}

[[nodiscard]] inline std::set<std::string, std::less<>>
typed_acl_privileges(
    const std::vector<std::string>& tokens,
    std::size_t begin,
    std::size_t end,
    std::string_view context) {
    std::set<std::string, std::less<>> result;
    for (std::size_t index = begin; index < end; ++index) {
        if (tokens[index] == "," || tokens[index] == "privileges") {
            continue;
        }
        static constexpr std::string_view accepted[] = {
            "all", "select", "insert", "update", "delete",
            "truncate", "references", "trigger", "usage",
            "execute", "create", "connect", "temporary"};
        if (!std::ranges::contains(accepted, tokens[index])) {
            throw std::runtime_error(
                std::string{context}
                + " has unsupported ACL privilege '"
                + tokens[index] + "'");
        }
        result.insert(tokens[index]);
    }
    if (result.empty()) {
        throw std::runtime_error(
            std::string{context} + " has no ACL privileges");
    }
    return result;
}

[[nodiscard]] inline revoke_descriptor typed_revoke_descriptor(
    const collected_entity& install,
    std::string_view extension_schema) {
    const std::string identity =
        typed_install_sql_identity(install, "revoke");
    const std::string context =
        typed_install_context(install, "revoke", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() < 6 || tokens[0] != "revoke") {
        throw std::runtime_error(
            context + " is not REVOKE");
    }
    revoke_descriptor result;
    std::size_t cursor = 1;
    if (cursor + 2 < tokens.size()
        && tokens[cursor] == "grant"
        && tokens[cursor + 1] == "option"
        && tokens[cursor + 2] == "for") {
        result.grant_option = true;
        cursor += 3;
    }
    const auto on = std::ranges::find(
        tokens.begin() + static_cast<std::ptrdiff_t>(cursor),
        tokens.end(),
        std::string{"on"});
    if (on == tokens.end()) {
        throw std::runtime_error(
            context + " has no ON clause");
    }
    const std::size_t on_index =
        static_cast<std::size_t>(
            std::distance(tokens.begin(), on));
    result.privileges =
        typed_acl_privileges(tokens, cursor, on_index, context);
    cursor = on_index + 1;
    if (cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no object");
    }
    if (tokens[cursor] == "all") {
        if (cursor + 4 >= tokens.size()
            || tokens[cursor + 2] != "in"
            || tokens[cursor + 3] != "schema") {
            throw std::runtime_error(
                context + " has malformed schema-wide scope");
        }
        const std::string& objects = tokens[cursor + 1];
        if (objects != "tables" && objects != "sequences"
            && objects != "functions") {
            throw std::runtime_error(
                context + " has unsupported schema-wide scope");
        }
        result.object_kind =
            "all " + objects + " in schema";
        result.target = tokens[cursor + 4];
        cursor += 5;
    } else {
        result.object_kind = tokens[cursor++];
        static constexpr std::string_view kinds[] = {
            "function", "procedure", "schema", "sequence",
            "table", "type"};
        if (!std::ranges::contains(kinds, result.object_kind)
            || cursor >= tokens.size()) {
            throw std::runtime_error(
                context + " has unsupported ACL object");
        }
        result.target =
            result.object_kind == "schema"
            ? tokens[cursor++]
            : install_resolved_catalog_name(
                catalog_qualified_target(tokens[cursor++]),
                extension_schema);
        if (result.object_kind == "function"
            || result.object_kind == "procedure") {
            if (cursor >= tokens.size()
                || tokens[cursor] != "(") {
                throw std::runtime_error(
                    context + " requires a routine signature");
            }
            const std::size_t close =
                catalog_matching_parenthesis(
                    tokens, cursor, context);
            result.target += "(";
            if (cursor + 1 != close) {
                const auto arguments =
                    catalog_top_level_clauses(
                        tokens, cursor + 1, close, context);
                for (std::size_t index = 0;
                     index < arguments.size(); ++index) {
                    if (index != 0) result.target += ",";
                    result.target += install_resolved_catalog_name(
                        canonical_catalog_sql_type(
                            catalog_token_fragment(
                                arguments[index],
                                0,
                                arguments[index].size())),
                        extension_schema);
                }
            }
            result.target += ")";
            cursor = close + 1;
        }
    }
    if (cursor + 2 != tokens.size()
        || tokens[cursor] != "from") {
        throw std::runtime_error(
            context + " has malformed FROM clause");
    }
    result.role = tokens[cursor + 1];
    normalize_function_all_privileges(result);
    return result;
}

[[nodiscard]] inline std::string revoke_descriptor_key(
    const revoke_descriptor& descriptor) {
    std::string result =
        descriptor.object_kind + ":" + descriptor.target + ":";
    bool first = true;
    for (const std::string& privilege : descriptor.privileges) {
        if (!first) result += ",";
        result += privilege;
        first = false;
    }
    result += ":" + descriptor.role;
    result += descriptor.grant_option ? ":grant_option" : ":ordinary";
    return result;
}

[[nodiscard]] inline std::string revoke_object_key(
    const revoke_descriptor& descriptor) {
    return descriptor.object_kind + ":" + descriptor.target;
}

inline void reconcile_natural_revoke_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    std::set<std::string, std::less<>> natural_objects;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::revoke
            || entity.typed_install_owner) {
            continue;
        }
        const revoke_descriptor descriptor =
            natural_revoke_descriptor(
                entity, program, extension_schema);
        const std::string key = revoke_descriptor_key(descriptor);
        if (!natural_by_target.emplace(key, index).second) {
            throw std::runtime_error(
                "duplicate natural revoke owner for '" + key + "'");
        }
        natural_objects.emplace(revoke_object_key(descriptor));
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::revoke
            || !install.typed_install_owner) {
            continue;
        }
        const revoke_descriptor descriptor =
            typed_revoke_descriptor(install, extension_schema);
        const std::string key =
            revoke_descriptor_key(descriptor);
        const auto natural = natural_by_target.find(key);
        if (natural == natural_by_target.end()) {
            if (natural_objects.contains(
                    revoke_object_key(descriptor))) {
                throw std::runtime_error(
                    "natural revoke owner for '"
                    + revoke_object_key(descriptor)
                    + "' disagrees with its typed install-owner descriptor "
                    + typed_install_context(
                        install,
                        "revoke",
                        typed_install_sql_identity(
                            install, "revoke")));
            }
            continue;
        }
        if (!matched.insert(key).second) {
            throw std::runtime_error(
                "natural revoke owner '" + key
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        if (natural_revoke_descriptor(
                natural_entity, program, extension_schema)
            != descriptor) {
            throw std::runtime_error(
                "natural revoke owner '" + key
                + "' disagrees with its typed install-owner descriptor "
                + typed_install_context(
                    install,
                    "revoke",
                    typed_install_sql_identity(
                        install, "revoke")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct grant_descriptor {
    std::string object_kind;
    std::string target;
    std::set<std::string, std::less<>> privileges;
    std::string role;
    bool grant_option{};

    [[nodiscard]] bool operator==(
        const grant_descriptor&) const = default;
};

inline void normalize_function_all_privileges(
    grant_descriptor& descriptor) {
    if ((descriptor.object_kind == "function"
         || descriptor.object_kind == "procedure")
        && descriptor.privileges
            == std::set<std::string, std::less<>>{"all"}) {
        descriptor.privileges = {"execute"};
    }
}

[[nodiscard]] inline grant_descriptor natural_grant_descriptor(
    const collected_entity& grant,
    const collected_program& program,
    std::string_view extension_schema) {
    const collected_entity& parent =
        natural_acl_parent(grant, program);
    grant_descriptor result{
        .privileges = natural_acl_privileges(grant),
        .role = acl_role_name(grant, "to"),
        .grant_option =
            catalog_option_identifier(
                grant, "grant_option", "false") == "true",
    };
    if (const option_value* scope = find_option(grant, "scope")) {
        const std::string value =
            graph_enum_leaf(scope->expression);
        if (value != "object") {
            if (parent.kind != entity_kind::schema) {
                throw std::runtime_error(
                    "schema-wide natural grant has no schema parent");
            }
            if (value == "all_tables_in_schema") {
                result.object_kind = "all tables in schema";
            } else if (value == "all_sequences_in_schema") {
                result.object_kind = "all sequences in schema";
            } else if (value == "all_functions_in_schema") {
                result.object_kind = "all functions in schema";
            } else {
                throw std::runtime_error(
                    "unsupported natural grant scope '" + value + "'");
            }
            result.target = parent.name;
            normalize_function_all_privileges(result);
            return result;
        }
    }
    switch (parent.kind) {
        case entity_kind::function:
        case entity_kind::sql_function:
            result.object_kind = "function";
            break;
        case entity_kind::procedure:
            result.object_kind = "procedure";
            break;
        case entity_kind::schema:
            result.object_kind = "schema";
            break;
        case entity_kind::sequence:
            result.object_kind = "sequence";
            break;
        case entity_kind::table:
        case entity_kind::view:
            result.object_kind = "table";
            break;
        case entity_kind::type:
            result.object_kind = "type";
            break;
        default:
            throw std::runtime_error(
                "natural grant has an unsupported parent kind");
    }
    result.target =
        parent.kind == entity_kind::schema
        ? parent.name
        : natural_catalog_name(parent, extension_schema);
    if (parent.kind == entity_kind::function
        || parent.kind == entity_kind::sql_function
        || parent.kind == entity_kind::procedure) {
        result.target += "(";
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(parent);
        for (std::size_t index = 0;
             index < parameters.size(); ++index) {
            if (index != 0) result.target += ",";
            result.target += install_resolved_catalog_name(
                canonical_catalog_sql_type(
                    parameters[index]->sql_type),
                extension_schema);
        }
        result.target += ")";
    }
    normalize_function_all_privileges(result);
    return result;
}

[[nodiscard]] inline grant_descriptor typed_grant_descriptor(
    const collected_entity& install,
    std::string_view extension_schema) {
    const std::string identity =
        typed_install_sql_identity(install, "grant");
    const std::string context =
        typed_install_context(install, "grant", identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(identity);
    if (tokens.size() < 5 || tokens[0] != "grant") {
        throw std::runtime_error(
            context + " is not GRANT");
    }
    grant_descriptor result;
    const auto on = std::ranges::find(
        tokens.begin() + 1, tokens.end(), std::string{"on"});
    if (on == tokens.end()) {
        throw std::runtime_error(
            context + " has no ON clause");
    }
    const std::size_t on_index =
        static_cast<std::size_t>(
            std::distance(tokens.begin(), on));
    result.privileges =
        typed_acl_privileges(tokens, 1, on_index, context);
    std::size_t cursor = on_index + 1;
    if (cursor >= tokens.size()) {
        throw std::runtime_error(
            context + " has no object");
    }
    if (tokens[cursor] == "all") {
        if (cursor + 4 >= tokens.size()
            || tokens[cursor + 2] != "in"
            || tokens[cursor + 3] != "schema") {
            throw std::runtime_error(
                context + " has malformed schema-wide scope");
        }
        const std::string& objects = tokens[cursor + 1];
        if (objects != "tables" && objects != "sequences"
            && objects != "functions") {
            throw std::runtime_error(
                context + " has unsupported schema-wide scope");
        }
        result.object_kind =
            "all " + objects + " in schema";
        result.target = tokens[cursor + 4];
        cursor += 5;
    } else {
        static constexpr std::string_view kinds[] = {
            "function", "procedure", "schema", "sequence",
            "table", "type"};
        if (std::ranges::contains(kinds, tokens[cursor])) {
            result.object_kind = tokens[cursor++];
        } else {
            // PostgreSQL treats an omitted object kind as TABLE. This is
            // the canonical spelling used by GRANT SELECT on views.
            result.object_kind = "table";
        }
        if (cursor >= tokens.size()) {
            throw std::runtime_error(
                context + " has no ACL target");
        }
        result.target =
            result.object_kind == "schema"
            ? tokens[cursor++]
            : install_resolved_catalog_name(
                catalog_qualified_target(tokens[cursor++]),
                extension_schema);
        if (result.object_kind == "function"
            || result.object_kind == "procedure") {
            if (cursor >= tokens.size()
                || tokens[cursor] != "(") {
                throw std::runtime_error(
                    context + " requires a routine signature");
            }
            const std::size_t close =
                catalog_matching_parenthesis(
                    tokens, cursor, context);
            result.target += "(";
            if (cursor + 1 != close) {
                const auto arguments =
                    catalog_top_level_clauses(
                        tokens, cursor + 1, close, context);
                for (std::size_t index = 0;
                     index < arguments.size(); ++index) {
                    if (index != 0) result.target += ",";
                    result.target += install_resolved_catalog_name(
                        canonical_catalog_sql_type(
                            catalog_token_fragment(
                                arguments[index],
                                0,
                                arguments[index].size())),
                        extension_schema);
                }
            }
            result.target += ")";
            cursor = close + 1;
        }
    }
    if (cursor + 1 >= tokens.size()
        || tokens[cursor] != "to") {
        throw std::runtime_error(
            context + " has malformed TO clause");
    }
    result.role = tokens[cursor + 1];
    cursor += 2;
    if (cursor < tokens.size()) {
        if (cursor + 2 >= tokens.size()
            || tokens[cursor] != "with"
            || tokens[cursor + 1] != "grant"
            || tokens[cursor + 2] != "option"
            || cursor + 3 != tokens.size()) {
            throw std::runtime_error(
                context + " has malformed WITH GRANT OPTION");
        }
        result.grant_option = true;
        cursor += 3;
    }
    if (cursor != tokens.size()) {
        throw std::runtime_error(
            context + " has trailing GRANT clauses");
    }
    normalize_function_all_privileges(result);
    return result;
}

[[nodiscard]] inline std::string grant_descriptor_key(
    const grant_descriptor& descriptor) {
    std::string result =
        descriptor.object_kind + ":" + descriptor.target + ":";
    bool first = true;
    for (const std::string& privilege : descriptor.privileges) {
        if (!first) result += ",";
        result += privilege;
        first = false;
    }
    result += ":" + descriptor.role;
    result += descriptor.grant_option ? ":grant_option" : ":ordinary";
    return result;
}

[[nodiscard]] inline std::string grant_object_key(
    const grant_descriptor& descriptor) {
    return descriptor.object_kind + ":" + descriptor.target;
}

inline void reconcile_natural_grant_owners(
    collected_program& program,
    std::string_view extension_schema) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    std::set<std::string, std::less<>> natural_objects;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::grant
            || entity.typed_install_owner) {
            continue;
        }
        const grant_descriptor descriptor =
            natural_grant_descriptor(
                entity, program, extension_schema);
        const std::string key = grant_descriptor_key(descriptor);
        if (!natural_by_target.emplace(key, index).second) {
            throw std::runtime_error(
                "duplicate natural grant owner for '" + key + "'");
        }
        natural_objects.emplace(grant_object_key(descriptor));
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::grant
            || !install.typed_install_owner) {
            continue;
        }
        const grant_descriptor descriptor =
            typed_grant_descriptor(install, extension_schema);
        const std::string key =
            grant_descriptor_key(descriptor);
        const auto natural = natural_by_target.find(key);
        if (natural == natural_by_target.end()) {
            if (natural_objects.contains(
                    grant_object_key(descriptor))) {
                throw std::runtime_error(
                    "natural grant owner for '"
                    + grant_object_key(descriptor)
                    + "' disagrees with its typed install-owner descriptor "
                    + typed_install_context(
                        install,
                        "grant",
                        typed_install_sql_identity(
                            install, "grant")));
            }
            continue;
        }
        if (!matched.insert(key).second) {
            throw std::runtime_error(
                "natural grant owner '" + key
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        if (natural_grant_descriptor(
                natural_entity, program, extension_schema)
            != descriptor) {
            throw std::runtime_error(
                "natural grant owner '" + key
                + "' disagrees with its typed install-owner descriptor "
                + typed_install_context(
                    install,
                    "grant",
                    typed_install_sql_identity(
                        install, "grant")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

struct aggregate_descriptor {
    std::string target;
    std::vector<std::string> argument_types;
    std::string transition;
    std::string state_type;
    std::string final;
    std::string initial_condition;
    std::string serial;
    std::string deserial;

    [[nodiscard]] bool operator==(
        const aggregate_descriptor&) const = default;
};

[[nodiscard]] inline std::optional<std::string>
aggregate_descriptor_mismatch(
    const aggregate_descriptor& natural,
    const aggregate_descriptor& install) {
    if (natural.target != install.target) {
        return "target '" + natural.target
            + "' differs from '" + install.target + "'";
    }
    if (natural.argument_types != install.argument_types) {
        return "aggregate input signature differs";
    }
    if (natural.transition != install.transition) {
        return "transition function '" + natural.transition
            + "' differs from '" + install.transition + "'";
    }
    if (natural.state_type != install.state_type) {
        return "state type '" + natural.state_type
            + "' differs from '" + install.state_type + "'";
    }
    if (natural.final != install.final) {
        return "final function '" + natural.final
            + "' differs from '" + install.final + "'";
    }
    if (natural.initial_condition != install.initial_condition) {
        return "initial condition '" + natural.initial_condition
            + "' differs from '" + install.initial_condition + "'";
    }
    if (natural.serial != install.serial) {
        return "serialization function '" + natural.serial
            + "' differs from '" + install.serial + "'";
    }
    if (natural.deserial != install.deserial) {
        return "deserialization function '" + natural.deserial
            + "' differs from '" + install.deserial + "'";
    }
    return std::nullopt;
}

[[nodiscard]] inline std::string canonical_aggregate_routine_target(
    std::string_view target,
    std::string_view clause) {
    if (target.contains('.')) return std::string{target};
    if ((clause == "serialfunc"
         && target == "int8_avg_serialize")
        || (clause == "deserialfunc"
            && target == "int8_avg_deserialize")) {
        return "pg_catalog." + std::string{target};
    }
    return catalog_qualified_target(target);
}

[[nodiscard]] inline std::string aggregate_integer_expression(
    std::string_view expression) {
    std::string_view value = expression;
    if (const std::size_t open = value.find('{');
        open != std::string_view::npos) {
        const std::size_t close = value.rfind('}');
        if (close == std::string_view::npos || close <= open) {
            throw std::runtime_error(
                "natural aggregate integer initial condition has "
                "malformed braces");
        }
        value = value.substr(open + 1, close - open - 1);
    }
    std::string compact;
    for (const char character : value) {
        if ((character >= '0' && character <= '9')
            || character == '+' || character == '-') {
            compact.push_back(character);
        } else if (!std::isspace(
                       static_cast<unsigned char>(character))
                   && character != 'l' && character != 'L'
                   && character != 'u' && character != 'U') {
            throw std::runtime_error(
                "natural aggregate integer initial condition contains "
                "an unsupported expression");
        }
    }
    if (compact.empty()) {
        throw std::runtime_error(
            "natural aggregate integer initial condition is empty");
    }

    std::size_t cursor =
        compact.front() == '-' || compact.front() == '+' ? 1 : 0;
    while (cursor < compact.size()
           && compact[cursor] >= '0' && compact[cursor] <= '9') {
        ++cursor;
    }
    std::int64_t result{};
    const auto [first_end, first_error] = std::from_chars(
        compact.data(), compact.data() + cursor, result);
    if (first_error != std::errc{}
        || first_end != compact.data() + cursor) {
        throw std::runtime_error(
            "natural aggregate integer initial condition is outside "
            "int64");
    }
    while (cursor < compact.size()) {
        const char operation = compact[cursor++];
        const std::size_t begin = cursor;
        while (cursor < compact.size()
               && compact[cursor] >= '0'
               && compact[cursor] <= '9') {
            ++cursor;
        }
        std::int64_t operand{};
        const auto [end, error] = std::from_chars(
            compact.data() + begin, compact.data() + cursor, operand);
        if (begin == cursor || error != std::errc{}
            || end != compact.data() + cursor) {
            throw std::runtime_error(
                "natural aggregate integer initial condition is malformed");
        }
        if (operation == '+') {
            if (result
                > std::numeric_limits<std::int64_t>::max() - operand) {
                throw std::runtime_error(
                    "natural aggregate integer initial condition "
                    "overflows int64");
            }
            result += operand;
        } else {
            if (result
                < std::numeric_limits<std::int64_t>::min() + operand) {
                throw std::runtime_error(
                    "natural aggregate integer initial condition "
                    "overflows int64");
            }
            result -= operand;
        }
    }
    return std::to_string(result);
}

[[nodiscard]] inline std::string natural_aggregate_initial_condition(
    const option_value* option) {
    if (option == nullptr) return {};
    const std::string_view expression = option->expression;
    if (expression.contains("pg_infinity::negative")) {
        return "-infinity";
    }
    if (expression.contains("pg_infinity::positive")) {
        return "infinity";
    }
    if (const std::size_t input = expression.find("pg_input_text");
        input != std::string_view::npos) {
        const std::size_t quote = expression.find('"', input);
        const std::size_t close = quote == std::string_view::npos
            ? std::string_view::npos
            : expression.find('"', quote + 1);
        if (close == std::string_view::npos) {
            throw std::runtime_error(
                "natural aggregate pg_input_text has no string literal");
        }
        return std::string{
            expression.substr(quote + 1, close - quote - 1)};
    }
    return aggregate_integer_expression(expression);
}

[[nodiscard]] inline aggregate_descriptor
natural_aggregate_descriptor(
    const collected_entity& aggregate,
    const collected_program& program) {
    const option_value* transition_option =
        find_option(aggregate, "transition");
    if (transition_option == nullptr) {
        throw std::runtime_error(
            "natural aggregate '" + aggregate.schema + "."
            + aggregate.name + "' has no transition function");
    }
    const collected_entity* transition = natural_reference_entity(
        transition_option->expression,
        program,
        [](entity_kind kind) {
            return kind == entity_kind::function
                || kind == entity_kind::sql_function;
        },
        "aggregate transition routine");
    if (transition == nullptr) {
        throw std::runtime_error(
            "natural aggregate '" + aggregate.schema + "."
            + aggregate.name
            + "' has an unresolved transition function");
    }
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(*transition);
    if (parameters.empty()) {
        throw std::runtime_error(
            "natural aggregate transition '"
            + transition->cpp_qualified_name
            + "' has no state parameter");
    }
    const std::string state_type =
        canonical_catalog_sql_type(parameters.front()->sql_type);
    if (state_type
        != canonical_catalog_sql_type(
            transition->result_sql_type)) {
        throw std::runtime_error(
            "natural aggregate transition '"
            + transition->cpp_qualified_name
            + "' does not return its state type");
    }
    aggregate_descriptor result{
        .target = aggregate.schema + "." + aggregate.name,
        .transition =
            transition->schema + "." + transition->name,
        .state_type = state_type,
        .final =
            natural_routine_reference(
                aggregate, program, "final"),
        .initial_condition =
            natural_aggregate_initial_condition(
                find_option(aggregate, "initial_condition")),
        .serial =
            natural_routine_reference(
                aggregate, program, "serial"),
        .deserial =
            natural_routine_reference(
                aggregate, program, "deserial"),
    };
    for (std::size_t index = 1; index < parameters.size(); ++index) {
        result.argument_types.push_back(
            canonical_catalog_sql_type(parameters[index]->sql_type));
    }
    return result;
}

[[nodiscard]] inline aggregate_descriptor typed_aggregate_descriptor(
    const collected_entity& aggregate) {
    const std::string sql_identity =
        typed_install_sql_identity(aggregate, "aggregate");
    const std::string diagnostic =
        typed_install_context(aggregate, "aggregate", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.size() < 9 || tokens[0] != "create"
        || tokens[1] != "aggregate" || tokens[3] != "(") {
        throw std::runtime_error(
            diagnostic + " is not a supported CREATE AGGREGATE");
    }
    const std::size_t arguments_close =
        catalog_matching_parenthesis(tokens, 3, diagnostic);
    const std::size_t clauses_open = arguments_close + 1;
    if (clauses_open >= tokens.size()
        || tokens[clauses_open] != "(") {
        throw std::runtime_error(
            diagnostic + " has no aggregate definition");
    }
    const std::size_t clauses_close =
        catalog_matching_parenthesis(tokens, clauses_open, diagnostic);
    if (clauses_close + 1 != tokens.size()) {
        throw std::runtime_error(
            diagnostic + " has unsupported trailing syntax");
    }

    aggregate_descriptor result{
        .target = catalog_qualified_target(tokens[2]),
    };
    const std::vector<std::vector<std::string>> arguments =
        catalog_top_level_clauses(
            tokens, 4, arguments_close, diagnostic);
    if (arguments.size() == 1 && arguments.front().size() == 1
        && arguments.front().front() == "*") {
        // The zero-input aggregate signature is represented by no types.
    } else {
        for (const std::vector<std::string>& argument : arguments) {
            result.argument_types.push_back(
                canonical_catalog_sql_type(
                    catalog_token_fragment(
                        argument, 0, argument.size())));
        }
    }

    std::set<std::string, std::less<>> seen_clauses;
    for (const std::vector<std::string>& clause :
         catalog_top_level_clauses(
             tokens, clauses_open + 1, clauses_close, diagnostic)) {
        std::string name;
        std::size_t value_begin = 0;
        if (clause.size() >= 3 && clause[1] == "=") {
            name = clause[0];
            value_begin = 2;
        } else if (clause.size() >= 2
                   && clause[0].ends_with('=')) {
            name = clause[0].substr(0, clause[0].size() - 1);
            value_begin = 1;
        } else {
            throw std::runtime_error(
                diagnostic + " has a malformed aggregate clause");
        }
        if (name.empty()) {
            throw std::runtime_error(
                diagnostic + " has an unnamed aggregate clause");
        }
        if (!seen_clauses.insert(name).second) {
            throw std::runtime_error(
                diagnostic + " repeats aggregate clause '" + name + "'");
        }
        const std::string value =
            catalog_token_fragment(
                clause, value_begin, clause.size());
        if (name == "sfunc") {
            result.transition =
                canonical_aggregate_routine_target(value, name);
        } else if (name == "stype") {
            result.state_type =
                canonical_catalog_sql_type(value);
        } else if (name == "finalfunc") {
            result.final =
                canonical_aggregate_routine_target(value, name);
        } else if (name == "initcond") {
            result.initial_condition =
                graph_unquoted_literal(value);
        } else if (name == "serialfunc") {
            result.serial =
                canonical_aggregate_routine_target(value, name);
        } else if (name == "deserialfunc") {
            result.deserial =
                canonical_aggregate_routine_target(value, name);
        } else {
            throw std::runtime_error(
                diagnostic + " has unsupported aggregate clause '"
                + name + "'");
        }
    }
    if (result.transition.empty() || result.state_type.empty()) {
        throw std::runtime_error(
            diagnostic + " requires SFUNC and STYPE");
    }
    return result;
}

[[nodiscard]] inline std::string aggregate_descriptor_key(
    const aggregate_descriptor& descriptor) {
    std::string result = descriptor.target + "(";
    if (descriptor.argument_types.empty()) {
        result += "*";
    } else {
        for (std::size_t index = 0;
             index < descriptor.argument_types.size(); ++index) {
            if (index != 0) result += ",";
            result += descriptor.argument_types[index];
        }
    }
    return result + ")";
}

[[nodiscard]] inline std::string graph_stable_identity(
    const collected_entity& entity,
    const collected_program& program,
    std::string_view extension_schema = "public") {
    if (entity.kind == entity_kind::grant
        && !entity.typed_install_owner) {
        return "sql:grant:"
            + grant_descriptor_key(
                natural_grant_descriptor(
                    entity, program, extension_schema));
    }
    if (entity.kind == entity_kind::revoke
        && !entity.typed_install_owner) {
        return "sql:revoke:"
            + revoke_descriptor_key(
                natural_revoke_descriptor(
                    entity, program, extension_schema));
    }
    if (entity.kind != entity_kind::aggregate
        || entity.typed_install_owner) {
        return stable_identity(entity);
    }
    return "sql:aggregate:"
        + aggregate_descriptor_key(
            natural_aggregate_descriptor(entity, program));
}

inline void reconcile_natural_aggregate_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::aggregate
            || entity.typed_install_owner) {
            continue;
        }
        const aggregate_descriptor descriptor =
            natural_aggregate_descriptor(entity, program);
        const std::string key =
            aggregate_descriptor_key(descriptor);
        if (!natural_by_target.emplace(key, index).second) {
            throw std::runtime_error(
                "duplicate natural aggregate owner for '" + key + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched_targets;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::aggregate
            || !install.typed_install_owner) {
            continue;
        }
        const aggregate_descriptor install_descriptor =
            typed_aggregate_descriptor(install);
        const std::string key =
            aggregate_descriptor_key(install_descriptor);
        const auto natural = natural_by_target.find(key);
        if (natural == natural_by_target.end()) continue;
        if (!matched_targets.insert(key).second) {
            throw std::runtime_error(
                "natural aggregate owner '" + key
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        const aggregate_descriptor natural_descriptor =
            natural_aggregate_descriptor(natural_entity, program);
        if (const std::optional<std::string> mismatch =
                aggregate_descriptor_mismatch(
                    natural_descriptor, install_descriptor)) {
            throw std::runtime_error(
                "natural aggregate owner '" + key
                + "' disagrees with its typed install-owner descriptor: "
                + *mismatch + " "
                + typed_install_context(
                    install,
                    "aggregate",
                    typed_install_sql_identity(
                        install, "aggregate")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

[[nodiscard]] inline std::string comment_object_kind(
    entity_kind kind) {
    switch (kind) {
        case entity_kind::table: return "table";
        case entity_kind::function:
        case entity_kind::sql_function: return "function";
        case entity_kind::procedure: return "procedure";
        case entity_kind::type: return "type";
        case entity_kind::view: return "view";
        case entity_kind::schema: return "schema";
        case entity_kind::sequence: return "sequence";
        case entity_kind::aggregate: return "aggregate";
        case entity_kind::access_method: return "access method";
        default: return {};
    }
}

[[nodiscard]] inline std::string natural_comment_target(
    const collected_entity& entity,
    const collected_program& program) {
    const std::string kind = comment_object_kind(entity.kind);
    if (kind.empty()) return {};
    std::string target =
        entity.kind == entity_kind::schema
            || entity.kind == entity_kind::access_method
        ? entity.name
        : entity.schema + "." + entity.name;
    if (entity.kind == entity_kind::function
        || entity.kind == entity_kind::sql_function
        || entity.kind == entity_kind::procedure) {
        target += "(";
        const std::vector<const routine_parameter*> parameters =
            sql_parameters(entity);
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            if (index != 0) target += ",";
            target += canonical_catalog_sql_type(
                parameters[index]->sql_type);
        }
        target += ")";
    } else if (entity.kind == entity_kind::aggregate) {
        const aggregate_descriptor descriptor =
            natural_aggregate_descriptor(entity, program);
        target += "(";
        if (descriptor.argument_types.empty()) {
            target += "*";
        } else {
            for (std::size_t index = 0;
                 index < descriptor.argument_types.size(); ++index) {
                if (index != 0) target += ",";
                target += descriptor.argument_types[index];
            }
        }
        target += ")";
    }
    return kind + ":" + target;
}

struct typed_comment_descriptor {
    std::string kind;
    std::string target;
    std::vector<std::vector<std::string>> arguments;
    bool has_signature{};
};

[[nodiscard]] inline std::optional<typed_comment_descriptor>
typed_comment_target(const collected_entity& comment) {
    const option_value* identity = find_option(comment, "sql_identity");
    if (identity == nullptr) return std::nullopt;
    const std::string sql_identity =
        typed_install_sql_identity(comment, "comment");
    const std::string diagnostic =
        typed_install_context(comment, "comment", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.size() < 6 || tokens[0] != "comment"
        || tokens[1] != "on") {
        return std::nullopt;
    }

    std::string kind;
    std::size_t cursor = 2;
    if (tokens[cursor] == "access") {
        if (++cursor >= tokens.size() || tokens[cursor] != "method") {
            throw std::runtime_error(
                diagnostic + " has malformed ACCESS METHOD kind");
        }
        kind = "access method";
        ++cursor;
    } else {
        kind = tokens[cursor++];
    }
    static constexpr std::string_view supported_kinds[] = {
        "table", "function", "procedure", "type", "view", "schema",
        "sequence", "aggregate", "access method"};
    if (!std::ranges::contains(supported_kinds, kind)) {
        return std::nullopt;
    }
    if (cursor >= tokens.size()) {
        throw std::runtime_error(
            diagnostic + " has no comment target");
    }
    typed_comment_descriptor result{
        .kind = kind,
        .target =
            kind == "schema" || kind == "access method"
            ? tokens[cursor++]
            : catalog_qualified_target(tokens[cursor++]),
    };

    if (kind == "function" || kind == "procedure"
        || kind == "aggregate") {
        if (cursor >= tokens.size() || tokens[cursor] != "(") {
            throw std::runtime_error(
                diagnostic + " requires a target signature");
        }
        const std::size_t close =
            catalog_matching_parenthesis(tokens, cursor, diagnostic);
        result.has_signature = true;
        if (cursor + 1 != close) {
            result.arguments =
                catalog_top_level_clauses(
                    tokens, cursor + 1, close, diagnostic);
        } else if (kind == "aggregate") {
            throw std::runtime_error(
                diagnostic
                + " zero-input aggregates must use the (*) signature");
        }
        cursor = close + 1;
    }
    if (cursor >= tokens.size() || tokens[cursor++] != "is"
        || cursor >= tokens.size()) {
        throw std::runtime_error(
            diagnostic + " has malformed COMMENT ... IS syntax");
    }
    if (tokens[cursor] == "null") {
        if (++cursor != tokens.size()) {
            throw std::runtime_error(
                diagnostic + " has trailing COMMENT text");
        }
    } else {
        while (cursor < tokens.size()) {
            if (tokens[cursor] == "e") {
                ++cursor;
                if (cursor >= tokens.size()) {
                    throw std::runtime_error(
                        diagnostic
                        + " has an incomplete escape string literal");
                }
            }
            const std::string& literal = tokens[cursor++];
            if (literal.size() < 2 || literal.front() != '\''
                || literal.back() != '\'') {
                throw std::runtime_error(
                    diagnostic
                    + " has nonliteral COMMENT text");
            }
        }
    }
    return result;
}

[[nodiscard]] inline bool typed_comment_argument_matches(
        const std::vector<std::string>& declaration,
        std::string_view expected_type) {
    if (declaration.empty()) return false;
    const auto matches_from =
        [&declaration, expected_type](std::size_t begin) {
            return begin < declaration.size()
                && canonical_catalog_sql_type(
                       catalog_token_fragment(
                           declaration, begin, declaration.size()))
                    == expected_type;
        };
    if (matches_from(0)) return true;

    std::size_t begin = 0;
    if (declaration.front() == "in"
        || declaration.front() == "out"
        || declaration.front() == "inout"
        || declaration.front() == "variadic") {
        begin = 1;
        if (matches_from(begin)) return true;
    }
    return begin + 1 < declaration.size()
        && matches_from(begin + 1);
}

[[nodiscard]] inline std::optional<std::string>
matching_natural_comment_target(
        const typed_comment_descriptor& typed,
        const std::map<std::string, std::size_t, std::less<>>&
            natural_by_target) {
    const std::string object_target =
        typed.kind + ":" + typed.target;
    if (!typed.has_signature) {
        return natural_by_target.contains(object_target)
            ? std::optional<std::string>{object_target}
            : std::nullopt;
    }

    std::optional<std::string> result;
    const std::string signature_prefix = object_target + "(";
    for (const auto& [candidate, index] : natural_by_target) {
        static_cast<void>(index);
        if (!candidate.starts_with(signature_prefix)) continue;
        const auto identity =
            fmgr_reconciliation_detail::parse_routine_identity(
                std::string_view{candidate}.substr(
                    typed.kind.size() + 1));
        if (identity.argument_types.size()
            != typed.arguments.size()) {
            continue;
        }
        bool matches = true;
        for (std::size_t argument = 0;
             argument < typed.arguments.size(); ++argument) {
            if (!typed_comment_argument_matches(
                    typed.arguments[argument],
                    identity.argument_types[argument])) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (result) {
            throw std::runtime_error(
                "typed comment target '" + object_target
                + "' ambiguously matches natural overloads '"
                + *result + "' and '" + candidate + "'");
        }
        result = candidate;
    }
    return result;
}

inline void reconcile_natural_comment_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    std::set<std::string, std::less<>> natural_object_targets;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.typed_install_owner
            || !entity.documentation.has_explicit_brief
            || entity.documentation.brief.empty()
            || comment_object_kind(entity.kind).empty()) {
            continue;
        }
        const std::string target =
            natural_comment_target(entity, program);
        if (!natural_by_target.emplace(target, index).second) {
            throw std::runtime_error(
                "duplicate natural comment owner for '" + target + "'");
        }
        natural_object_targets.insert(
            target.substr(0, target.find('(')));
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched_targets;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::comment
            || !install.typed_install_owner) {
            continue;
        }
        const std::optional<typed_comment_descriptor> target =
            typed_comment_target(install);
        if (!target) continue;
        const std::optional<std::string> matched_target =
            matching_natural_comment_target(
                *target, natural_by_target);
        if (!matched_target) {
            const std::string object_target =
                target->kind + ":" + target->target;
            if (natural_object_targets.contains(object_target)) {
                throw std::runtime_error(
                    "typed comment target '" + object_target
                    + "' disagrees with the natural target signature");
            }
            continue;
        }
        const auto natural =
            natural_by_target.find(*matched_target);
        if (!matched_targets.insert(*matched_target).second) {
            throw std::runtime_error(
                "natural comment owner for '" + *matched_target
                + "' matches more than one typed comment install owner");
        }
        adopt_typed_install_statement(
            program.entities[natural->second],
            install,
            physical_statement_facet::catalog_comment,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

[[nodiscard]] inline operator_descriptor
natural_operator_descriptor(
    const collected_entity& op,
    const collected_program& program) {
    const collected_entity& function =
        natural_operator_function(op, program);
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(function);
    if (parameters.size() != 2) {
        throw std::runtime_error(
            "natural operator function '" + function.cpp_qualified_name
            + "' must accept two SQL arguments");
    }
    return operator_descriptor{
        .target = op.schema + "." + op.name,
        .left_type =
            canonical_catalog_sql_type(parameters[0]->sql_type),
        .right_type =
            canonical_catalog_sql_type(parameters[1]->sql_type),
        .function_target =
            function.schema + "." + function.name,
        .commutator =
            natural_operator_reference(op, program, "commutator"),
        .negator =
            natural_operator_reference(op, program, "negator"),
        .restrict =
            natural_routine_reference(op, program, "restrict"),
        .join = natural_routine_reference(op, program, "join"),
        .hashes =
            catalog_option_identifier(op, "hashes", "false")
                == "true",
        .merges =
            catalog_option_identifier(op, "merges", "false")
                == "true",
    };
}

[[nodiscard]] inline operator_descriptor typed_operator_descriptor(
    const collected_entity& op) {
    const std::string sql_identity =
        typed_install_sql_identity(op, "operator");
    const std::string diagnostic =
        typed_install_context(op, "operator", sql_identity);
    const std::vector<std::string> tokens =
        catalog_sql_tokens(sql_identity);
    if (tokens.size() < 8 || tokens[0] != "create"
        || tokens[1] != "operator" || tokens[3] != "(") {
        throw std::runtime_error(
            diagnostic + " is not a supported CREATE OPERATOR");
    }
    const std::size_t close =
        catalog_matching_parenthesis(tokens, 3, diagnostic);
    if (close + 1 != tokens.size()) {
        throw std::runtime_error(
            diagnostic + " has unsupported trailing syntax");
    }
    operator_descriptor result{
        .target = catalog_qualified_target(tokens[2]),
    };
    for (const std::vector<std::string>& clause :
         catalog_top_level_clauses(
             tokens, 4, close, diagnostic)) {
        if (clause.size() == 1
            && (clause[0] == "hashes"
                || clause[0] == "merges")) {
            if (clause[0] == "hashes") result.hashes = true;
            else result.merges = true;
            continue;
        }
        if (clause.size() < 3 || clause[1] != "=") {
            throw std::runtime_error(
                diagnostic + " has a malformed operator clause");
        }
        const std::string& name = clause[0];
        if (name == "leftarg") {
            result.left_type = canonical_catalog_sql_type(
                catalog_token_fragment(
                    clause, 2, clause.size()));
        } else if (name == "rightarg") {
            result.right_type = canonical_catalog_sql_type(
                catalog_token_fragment(
                    clause, 2, clause.size()));
        } else if (name == "function" || name == "procedure") {
            result.function_target =
                catalog_qualified_target(clause[2]);
        } else if (name == "commutator" || name == "negator") {
            if (clause.size() != 6 || clause[2] != "operator"
                || clause[3] != "(" || clause[5] != ")") {
                throw std::runtime_error(
                    diagnostic + " has a malformed " + name
                    + " clause");
            }
            std::string& target =
                name == "commutator"
                ? result.commutator
                : result.negator;
            target = catalog_qualified_target(clause[4]);
        } else if (name == "restrict" || name == "join") {
            std::string& target =
                name == "restrict" ? result.restrict : result.join;
            target = catalog_qualified_target(clause[2]);
        } else {
            throw std::runtime_error(
                diagnostic + " has unsupported operator clause '"
                + name + "'");
        }
    }
    if (result.left_type.empty() || result.right_type.empty()
        || result.function_target.empty()) {
        throw std::runtime_error(
            diagnostic
            + " requires LEFTARG, RIGHTARG, and FUNCTION");
    }
    return result;
}

[[nodiscard]] inline std::string operator_descriptor_key(
    const operator_descriptor& descriptor) {
    return descriptor.target + "(" + descriptor.left_type + ","
        + descriptor.right_type + ")";
}

inline void reconcile_natural_operator_owners(
    collected_program& program) {
    std::map<std::string, std::size_t, std::less<>>
        natural_by_target;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& entity = program.entities[index];
        if (entity.kind != entity_kind::operator_
            || entity.typed_install_owner) {
            continue;
        }
        const operator_descriptor descriptor =
            natural_operator_descriptor(entity, program);
        const std::string key = operator_descriptor_key(descriptor);
        if (!natural_by_target.emplace(key, index).second) {
            throw std::runtime_error(
                "duplicate natural operator owner for '" + key + "'");
        }
    }
    if (natural_by_target.empty()) return;

    std::set<std::size_t> reconciled;
    std::set<std::string, std::less<>> matched_targets;
    for (std::size_t index = 0; index < program.entities.size();
         ++index) {
        const collected_entity& install = program.entities[index];
        if (install.kind != entity_kind::operator_
            || !install.typed_install_owner) {
            continue;
        }
        const operator_descriptor install_descriptor =
            typed_operator_descriptor(install);
        const std::string key =
            operator_descriptor_key(install_descriptor);
        const auto natural = natural_by_target.find(key);
        if (natural == natural_by_target.end()) continue;
        if (!matched_targets.insert(key).second) {
            throw std::runtime_error(
                "natural operator owner '" + key
                + "' matches more than one typed install owner");
        }
        collected_entity& natural_entity =
            program.entities[natural->second];
        const operator_descriptor natural_descriptor =
            natural_operator_descriptor(natural_entity, program);
        if (const std::optional<std::string> mismatch =
                operator_descriptor_mismatch(
                    natural_descriptor, install_descriptor)) {
            throw std::runtime_error(
                "natural operator owner '" + key
                + "' disagrees with its typed install-owner descriptor: "
                + *mismatch + " "
                + typed_install_context(
                    install,
                    "operator",
                    typed_install_sql_identity(install, "operator")));
        }
        adopt_typed_install_statement(
            natural_entity,
            install,
            physical_statement_facet::primary,
            physical_render_source::natural_projection);
        reconciled.insert(index);
    }
    erase_reconciled_install_owners(program, reconciled);
}

[[nodiscard]] inline bool allowed_cycle_kind(entity_kind kind) noexcept {
    return kind == entity_kind::type
        || kind == entity_kind::type_role
        || kind == entity_kind::function
        || kind == entity_kind::operator_
        || kind == entity_kind::cast;
}

[[nodiscard]] inline int kind_order(entity_kind kind) noexcept {
    switch (kind) {
        case entity_kind::alter:
        case entity_kind::comment: return 5;
        case entity_kind::session_setting: return 0;
        case entity_kind::schema: return 10;
        case entity_kind::sequence: return 20;
        case entity_kind::type: return 30;
        case entity_kind::type_role: return 31;
        case entity_kind::table: return 40;
        case entity_kind::column: return 41;
        case entity_kind::primary_key:
        case entity_kind::unique:
        case entity_kind::foreign_key:
        case entity_kind::check: return 42;
        case entity_kind::index: return 45;
        case entity_kind::view: return 50;
        case entity_kind::function:
        case entity_kind::sql_function:
        case entity_kind::procedure: return 60;
        case entity_kind::operator_: return 65;
        case entity_kind::cast: return 66;
        case entity_kind::aggregate: return 70;
        case entity_kind::opclass: return 75;
        case entity_kind::access_method: return 80;
        case entity_kind::guc:
        case entity_kind::guc_value: return 85;
        case entity_kind::grant:
        case entity_kind::revoke:
        case entity_kind::config_dump:
        case entity_kind::catalog_dependencies:
        case entity_kind::install_action: return 90;
        case entity_kind::phase: return 100;
    }
    return 1000;
}

[[nodiscard]] inline bool fmgr_macro_name(
    std::string_view spelling) noexcept {
    return spelling == "CXX_PG_FUNCTION_INFO_V1"
        || spelling == "PG_FUNCTION_INFO_V1"
        || spelling == "POSTGRES_FN"
        || spelling == "E_POSTGRES_FN";
}

[[nodiscard]] inline std::vector<legacy_fmgr_macro_owner>
scan_legacy_fmgr_macro_owners(
    const std::vector<clang_structural::translation_unit>& units) {
    std::vector<legacy_fmgr_macro_owner> result;
    for (const clang_structural::translation_unit& unit : units) {
        const auto& tokens = unit.tokens;
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (!fmgr_macro_name(tokens[index].spelling)) continue;
            if (index + 3 >= tokens.size()
                || tokens[index + 1].spelling != "("
                || tokens[index + 3].spelling != ")") {
                throw std::runtime_error(
                    unit.source_path.string() + ":"
                    + std::to_string(tokens[index].range.begin.line)
                    + ": malformed fmgr ownership macro "
                    + tokens[index].spelling);
            }
            const std::string& symbol = tokens[index + 2].spelling;
            if (!sql_identifier(symbol)
                || reserved_sql_identifier(symbol)) {
                throw std::runtime_error(
                    unit.source_path.string() + ":"
                    + std::to_string(tokens[index].range.begin.line)
                    + ": fmgr ownership macro has invalid symbol '"
                    + symbol + "'");
            }
            result.push_back(legacy_fmgr_macro_owner{
                .macro_name = tokens[index].spelling,
                .symbol = symbol,
                .source_module = unit.module_name,
                .source_path = unit.source_path,
                .source_line = tokens[index].range.begin.line,
            });
        }
    }
    std::ranges::sort(
        result,
        [](const legacy_fmgr_macro_owner& left,
           const legacy_fmgr_macro_owner& right) {
            if (left.symbol != right.symbol) {
                return left.symbol < right.symbol;
            }
            if (left.source_path != right.source_path) {
                return left.source_path.generic_string()
                    < right.source_path.generic_string();
            }
            return left.source_line < right.source_line;
        });
    const auto duplicate = std::ranges::adjacent_find(
        result,
        {},
        &legacy_fmgr_macro_owner::symbol);
    if (duplicate != result.end()) {
        throw std::runtime_error(
            "legacy fmgr ownership macro for symbol '"
            + duplicate->symbol + "' is defined more than once");
    }
    return result;
}

inline void inherit_natural_column_schemas(
    collected_program& program) {
    std::map<std::string, std::vector<std::size_t>, std::less<>>
        record_owners;
    for (std::size_t index = 0; index < program.entities.size(); ++index) {
        const collected_entity& entity = program.entities[index];
        if (!entity.typed_install_owner
            && (entity.kind == entity_kind::table
                || entity.kind == entity_kind::type)) {
            record_owners[entity.cpp_qualified_name].push_back(index);
        }
    }

    for (collected_entity& entity : program.entities) {
        if (entity.typed_install_owner
            || entity.kind != entity_kind::column) {
            continue;
        }
        const auto owners =
            record_owners.find(entity.enclosing_cpp_name);
        if (owners == record_owners.end()) continue;
        if (owners->second.size() != 1) {
            throw std::runtime_error(
                entity.source_path.string() + ":"
                + std::to_string(entity.source_range.begin.line)
                + ": natural SQL column '" + entity.name
                + "' has more than one enclosing table/type owner for '"
                + entity.enclosing_cpp_name + "'");
        }
        entity.schema =
            program.entities[owners->second.front()].schema;
    }
}

}  // namespace detail

[[nodiscard]] inline sql_graph build_sql_graph(
    collected_program program,
    std::string extension_schema = "public") {
    if (!detail::sql_identifier(extension_schema)
        || detail::reserved_sql_identifier(extension_schema)) {
        throw std::runtime_error(
            "invalid install-resolved extension schema '" + extension_schema
            + "'");
    }
    detail::inherit_natural_column_schemas(program);
    std::vector<baseline_physical_statement>
        expected_baseline_physical_statements =
            capture_baseline_physical_ledger(program);
    std::vector<legacy_fmgr_macro_owner> legacy_fmgr_macro_owners =
        detail::scan_legacy_fmgr_macro_owners(
            program.translation_units);
    detail::reconcile_natural_bootstrap_owners(program);
    detail::reconcile_natural_sequence_owners(program);
    detail::reconcile_natural_table_owners(program);
    detail::reconcile_natural_base_type_owners(
        program, extension_schema);
    detail::reconcile_natural_composite_type_owners(program);
    detail::reconcile_natural_index_owners(program);
    detail::reconcile_natural_config_dump_owners(program);
    reconcile_natural_routine_owners(program);
    detail::reconcile_natural_access_method_owners(
        program, extension_schema);
    detail::reconcile_natural_catalog_dependency_owners(
        program, extension_schema);
    detail::reconcile_natural_view_owners(
        program, extension_schema);
    detail::reconcile_natural_aggregate_owners(program);
    detail::reconcile_natural_cast_owners(program);
    detail::reconcile_natural_operator_owners(program);
    detail::reconcile_natural_opclass_owners(
        program, extension_schema);
    detail::reconcile_natural_grant_owners(
        program, extension_schema);
    detail::reconcile_natural_revoke_owners(
        program, extension_schema);
    detail::reconcile_natural_comment_owners(program);
    validate_physical_statement_ownership(
        program, expected_baseline_physical_statements);
    sql_graph graph{
        .extension_schema = std::move(extension_schema),
        .compiler_profile = std::move(program.compiler_profile),
        .clang_version =
            std::move(program.module_graph.clang_version),
        .compilation_signature =
            std::move(program.compilation_signature),
        .legacy_fmgr_macro_owners =
            std::move(legacy_fmgr_macro_owners),
    };
    graph.expected_baseline_physical_statements =
        std::move(expected_baseline_physical_statements);
    graph.result_rows = std::move(program.result_rows);
    std::vector<std::string> stable_identities;
    stable_identities.reserve(program.entities.size());
    for (const collected_entity& entity : program.entities) {
        stable_identities.push_back(
            detail::graph_stable_identity(
                entity, program, extension_schema));
    }
    graph.entities.reserve(program.entities.size());
    std::map<std::string, std::size_t, std::less<>> identity_owner;
    for (std::size_t entity_index = 0;
         entity_index < program.entities.size();
         ++entity_index) {
        collected_entity& entity = program.entities[entity_index];
        const bool valid_name =
            entity.kind == entity_kind::session_setting
            ? detail::sql_guc_name(entity.name)
            : detail::punctuation_name(entity.kind)
            || detail::sql_identifier(entity.name);
        if (!valid_name) {
            throw std::runtime_error(
                entity.source_path.string() + ":"
                + std::to_string(entity.source_range.begin.line)
                + ": SQL name '" + entity.name
                + "' must be an unquoted lowercase PostgreSQL identifier");
        }
        if (!detail::punctuation_name(entity.kind)
            && detail::reserved_sql_identifier(entity.name)) {
            throw std::runtime_error(
                "SQL name '" + entity.name
                + "' is a reserved PostgreSQL keyword");
        }
        if (!detail::schema_less(entity.kind)
            && entity.schema != "$extension"
            && (!detail::sql_identifier(entity.schema)
                || detail::reserved_sql_identifier(entity.schema))) {
            throw std::runtime_error(
                "invalid SQL schema name '" + entity.schema + "'");
        }
        std::string identity =
            std::move(stable_identities[entity_index]);
        const auto [owner, inserted] =
            identity_owner.emplace(identity, graph.entities.size());
        if (!inserted) {
            const collected_entity& previous =
                graph.entities[owner->second].source;
            throw std::runtime_error(
                "duplicate target SQL owner '" + identity + "' at "
                + previous.source_path.string() + ":"
                + std::to_string(previous.source_range.begin.line)
                + " and " + entity.source_path.string() + ":"
                + std::to_string(entity.source_range.begin.line));
        }
        graph.entities.push_back(normalized_entity{
            .source = std::move(entity),
            .stable_identity = std::move(identity),
            .installed_identity = detail::installed_identity(
                owner->first, graph.extension_schema),
        });
    }

    std::vector<std::vector<std::size_t>> dependency_indexes(
        graph.entities.size());
    for (std::size_t index = 0; index < graph.entities.size(); ++index) {
        normalized_entity& entity = graph.entities[index];
        std::set<std::size_t> resolved;
        for (const dependency_reference& dependency :
             entity.source.dependencies) {
            const std::vector<std::size_t> candidates =
                detail::reference_candidates(
                    graph.entities, dependency.expression);
            if (candidates.empty()) {
                throw std::runtime_error(
                    entity.source.source_path.string() + ":"
                    + std::to_string(dependency.range.begin.line)
                    + ": unresolved SQL dependency reference '"
                    + dependency.expression + "'");
            }
            if (candidates.size() != 1) {
                throw std::runtime_error(
                    "ambiguous SQL dependency reference '"
                    + dependency.expression + "'");
            }
            resolved.insert(candidates.front());
        }
        for (const option_value& option : entity.source.options) {
            if (detail::option_has_declaration_references(
                    entity.source.kind, option.name)) {
                const std::vector<std::size_t> candidates =
                    detail::option_reference_candidates(
                        graph.entities, entity.source.kind, option);
                if (candidates.empty()
                    && ((entity.source.kind
                             == entity_kind::aggregate
                         && !option.expression.contains("pg_catalog"))
                        || (entity.source.kind
                                == entity_kind::type_role
                            && option.name == "for_type")
                        || (entity.source.kind
                                == entity_kind::catalog_dependencies
                            && option.name == "roots"))) {
                    throw std::runtime_error(
                        "unresolved typed SQL declaration reference in ."
                        + option.name + " = " + option.expression);
                }
                resolved.insert(candidates.begin(), candidates.end());
                continue;
            }
            if (option.expression.contains("type_of<")) {
                const std::vector<std::size_t> candidates =
                    detail::reference_candidates(
                        graph.entities, option.expression);
                if (candidates.size() != 1) {
                    throw std::runtime_error(
                        candidates.empty()
                            ? "unresolved bioseq::sql::type_of reference '"
                                  + option.expression + "'"
                            : "ambiguous bioseq::sql::type_of reference '"
                                  + option.expression + "'");
                }
                resolved.insert(candidates.front());
            }
        }
        if (entity.source.kind == entity_kind::table) {
            const std::string access_method =
                detail::catalog_option_identifier(
                    entity.source, "access_method", "default");
            if (access_method != "default") {
                std::vector<std::size_t> natural_access_methods;
                for (std::size_t candidate = 0;
                     candidate < graph.entities.size();
                     ++candidate) {
                    const collected_entity& method =
                        graph.entities[candidate].source;
                    if (method.kind == entity_kind::access_method
                        && !method.typed_install_owner
                        && method.name == access_method) {
                        natural_access_methods.push_back(candidate);
                    }
                }
                if (natural_access_methods.size() > 1) {
                    throw std::runtime_error(
                        "table '" + entity.stable_identity
                        + "' resolves access method '" + access_method
                        + "' to more than one natural owner");
                }
                if (natural_access_methods.size() == 1) {
                    resolved.insert(natural_access_methods.front());
                }
            }
        }
        if (entity.source.kind == entity_kind::type_role
            && !find_option(entity.source, "for_type")) {
            std::set<std::size_t> inferred_types;
            for (std::size_t candidate = 0;
                 candidate < graph.entities.size();
                 ++candidate) {
                const collected_entity& function =
                    graph.entities[candidate].source;
                if (function.kind != entity_kind::function
                    || function.cpp_qualified_name
                        != entity.source.cpp_qualified_name) {
                    continue;
                }
                for (std::size_t type_index = 0;
                     type_index < graph.entities.size();
                     ++type_index) {
                    const collected_entity& type =
                        graph.entities[type_index].source;
                    if (type.kind != entity_kind::type) continue;
                    const std::string qualified_type =
                        type.schema == "$extension"
                        ? type.name
                        : type.schema + "." + type.name;
                    const bool parameter_match = std::ranges::any_of(
                        sql_parameters(function),
                        [&type, &qualified_type](
                            const routine_parameter* parameter) {
                            return parameter->sql_type == type.name
                                || parameter->sql_type
                                    == qualified_type;
                        });
                    if (parameter_match
                        || function.result_sql_type == type.name
                        || function.result_sql_type
                            == qualified_type) {
                        inferred_types.insert(type_index);
                    }
                }
            }
            if (inferred_types.size() == 1) {
                resolved.insert(*inferred_types.begin());
            } else if (inferred_types.empty()) {
                throw std::runtime_error(
                    "type callback '" + entity.source.name
                    + "' does not mention an annotated type; use .for_type");
            } else {
                throw std::runtime_error(
                    "type callback '" + entity.source.name
                    + "' refers to more than one annotated type");
            }
        }
        if (entity.source.companion) {
            std::vector<std::size_t> same_declaration;
            std::vector<std::size_t> enclosing;
            for (std::size_t candidate = 0;
                 candidate < graph.entities.size();
                 ++candidate) {
                if (candidate != index
                    && !graph.entities[candidate].source.companion) {
                    if (graph.entities[candidate]
                            .source.cpp_qualified_name
                        == entity.source.cpp_qualified_name) {
                        same_declaration.push_back(candidate);
                    }
                    if (graph.entities[candidate]
                            .source.cpp_qualified_name
                        == entity.source.enclosing_cpp_name) {
                        enclosing.push_back(candidate);
                    }
                }
            }
            const bool enclosing_owned =
                entity.source.kind == entity_kind::column
                || entity.source.kind == entity_kind::primary_key
                || entity.source.kind == entity_kind::unique
                || entity.source.kind == entity_kind::foreign_key
                || entity.source.kind == entity_kind::check
                || entity.source.kind == entity_kind::type_role
                || entity.source.kind == entity_kind::guc_value;
            const std::vector<std::size_t>& preferred =
                enclosing_owned && !enclosing.empty()
                ? enclosing
                : same_declaration;
            if (preferred.size() == 1) {
                resolved.insert(preferred.front());
            }
        }
        if (entity.source.schema != "$extension"
            && entity.source.kind != entity_kind::schema) {
            for (std::size_t candidate = 0;
                 candidate < graph.entities.size();
                 ++candidate) {
                if (graph.entities[candidate].source.kind
                        == entity_kind::schema
                    && graph.entities[candidate].source.name
                        == entity.source.schema) {
                    resolved.insert(candidate);
                }
            }
        }
        resolved.erase(index);
        dependency_indexes[index].assign(resolved.begin(), resolved.end());
        for (const std::size_t dependency : resolved) {
            entity.dependency_identities.push_back(
                graph.entities[dependency].stable_identity);
        }
        std::ranges::sort(entity.dependency_identities);
    }

    const std::size_t size = graph.entities.size();
    std::vector<int> discovery(size, -1);
    std::vector<int> low(size, -1);
    std::vector<std::size_t> stack;
    std::vector<bool> on_stack(size);
    int next_discovery = 0;
    std::vector<std::vector<std::size_t>> components;
    const std::function<void(std::size_t)> visit =
        [&components, &dependency_indexes, &discovery, &low,
         &next_discovery, &on_stack, &stack, &visit](
            std::size_t node) {
            discovery[node] = next_discovery;
            low[node] = next_discovery++;
            stack.push_back(node);
            on_stack[node] = true;
            for (const std::size_t dependency : dependency_indexes[node]) {
                if (discovery[dependency] == -1) {
                    visit(dependency);
                    low[node] = std::min(low[node], low[dependency]);
                } else if (on_stack[dependency]) {
                    low[node] = std::min(
                        low[node], discovery[dependency]);
                }
            }
            if (low[node] != discovery[node]) return;
            std::vector<std::size_t> component;
            while (!stack.empty()) {
                const std::size_t member = stack.back();
                stack.pop_back();
                on_stack[member] = false;
                component.push_back(member);
                if (member == node) break;
            }
            components.push_back(std::move(component));
        };
    for (std::size_t index = 0; index < size; ++index) {
        if (discovery[index] == -1) visit(index);
    }

    std::vector<std::size_t> component_of(size);
    for (std::size_t component = 0; component < components.size();
         ++component) {
        const bool cyclic = components[component].size() > 1
            || (components[component].size() == 1
                && std::ranges::contains(
                    dependency_indexes[components[component].front()],
                    components[component].front()));
        if (cyclic && !std::ranges::all_of(
                components[component],
                [&graph](std::size_t member) {
                    return detail::allowed_cycle_kind(
                        graph.entities[member].source.kind);
                })) {
            throw std::runtime_error(
                "unsupported SQL dependency cycle containing '"
                + graph.entities[components[component].front()]
                      .stable_identity
                + "'");
        }
        for (const std::size_t member : components[component]) {
            component_of[member] = component;
            graph.entities[member].strongly_connected_component = component;
        }
    }

    std::vector<std::set<std::size_t>> component_dependencies(
        components.size());
    std::vector<std::vector<std::size_t>> component_consumers(
        components.size());
    std::vector<std::size_t> indegree(components.size());
    for (std::size_t node = 0; node < size; ++node) {
        for (const std::size_t dependency : dependency_indexes[node]) {
            const std::size_t consumer_component = component_of[node];
            const std::size_t dependency_component = component_of[dependency];
            if (consumer_component != dependency_component
                && component_dependencies[consumer_component].insert(
                    dependency_component).second) {
                ++indegree[consumer_component];
                component_consumers[dependency_component].push_back(
                    consumer_component);
            }
        }
    }
    const auto component_key = [&components, &graph](
                                   std::size_t component) {
        std::string key;
        for (const std::size_t member : components[component]) {
            const normalized_entity& entity = graph.entities[member];
            const std::string& candidate =
                entity.source.compatibility_order_identity.empty()
                ? entity.stable_identity
                : entity.source.compatibility_order_identity;
            if (key.empty() || candidate < key) {
                key = candidate;
            }
        }
        return key;
    };
    std::set<std::pair<std::string, std::size_t>> ready;
    for (std::size_t component = 0; component < components.size();
         ++component) {
        if (indegree[component] == 0) {
            ready.emplace(component_key(component), component);
        }
    }
    std::vector<normalized_entity> ordered;
    ordered.reserve(size);
    while (!ready.empty()) {
        const std::size_t component = ready.begin()->second;
        ready.erase(ready.begin());
        std::ranges::sort(
            components[component],
            [&graph](std::size_t left, std::size_t right) {
                const int left_order =
                    detail::kind_order(graph.entities[left].source.kind);
                const int right_order =
                    detail::kind_order(graph.entities[right].source.kind);
                if (left_order != right_order) {
                    return left_order < right_order;
                }
                const normalized_entity& left_entity =
                    graph.entities[left];
                const normalized_entity& right_entity =
                    graph.entities[right];
                const std::string& left_key =
                    left_entity.source.compatibility_order_identity.empty()
                    ? left_entity.stable_identity
                    : left_entity.source.compatibility_order_identity;
                const std::string& right_key =
                    right_entity.source.compatibility_order_identity.empty()
                    ? right_entity.stable_identity
                    : right_entity.source.compatibility_order_identity;
                return left_key < right_key;
            });
        for (const std::size_t member : components[component]) {
            ordered.push_back(std::move(graph.entities[member]));
        }
        for (const std::size_t consumer :
             component_consumers[component]) {
            if (--indegree[consumer] == 0) {
                ready.emplace(component_key(consumer), consumer);
            }
        }
    }
    if (ordered.size() != size) {
        throw std::runtime_error(
            "internal SQL graph ordering failed to consume every entity");
    }
    graph.entities = std::move(ordered);
    return graph;
}

}  // namespace bioseq::codegen::sql
