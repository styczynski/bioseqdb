module;

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.graph;

export import :model;
import bioseq.codegen.structural_analysis;
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

[[nodiscard]] inline std::string option_literal(
    std::string_view value) {
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'
               || value.front() == '\n' || value.front() == '\r'
               || value.front() == '{')) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'
               || value.back() == '\n' || value.back() == '\r'
               || value.back() == '}')) {
        value.remove_suffix(1);
    }
    if (value.size() >= 2
        && ((value.front() == '"' && value.back() == '"')
            || (value.front() == '\'' && value.back() == '\''))) {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    const std::size_t separator = value.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? value
            : value.substr(separator + 2)};
}

[[nodiscard]] inline std::string option_identifier(
    const collected_entity& entity,
    std::string_view name,
    std::string_view fallback) {
    const option_value* option = find_option(entity, name);
    return option == nullptr
        ? std::string{fallback}
        : option_literal(option->expression);
}

[[nodiscard]] inline bool schema_less(entity_kind kind) noexcept {
    return kind == entity_kind::comment
        || kind == entity_kind::schema
        || kind == entity_kind::session_setting
        || kind == entity_kind::guc
        || kind == entity_kind::guc_value
        || kind == entity_kind::grant
        || kind == entity_kind::revoke
        || kind == entity_kind::phase;
}

[[nodiscard]] inline bool routine_identity_kind(
    entity_kind kind) noexcept {
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
        if (index != 0) result += ',';
        const std::string& sql_type =
            parameters[index]->stable_identity_sql_type.empty()
            ? parameters[index]->sql_type
            : parameters[index]->stable_identity_sql_type;
        result += sql_type == "quoted_any" ? "\"any\"" : sql_type;
    }
    return result + ")";
}

[[nodiscard]] inline std::string option_identity(
    const collected_entity& entity) {
    std::vector<std::pair<std::string, std::string>> options;
    options.reserve(entity.options.size());
    for (const option_value& option : entity.options) {
        options.emplace_back(option.name, option.expression);
    }
    std::ranges::sort(options);
    std::string result;
    for (const auto& [name, expression] : options) {
        result += ':' + name + '=' + expression;
    }
    return result;
}

[[nodiscard]] inline std::string stable_identity(
    const collected_entity& entity) {
    if (entity.kind == entity_kind::schema) {
        return "sql:schema:" + entity.name;
    }
    if (entity.kind == entity_kind::guc) {
        std::string path = entity.cpp_qualified_name;
        std::ranges::replace(path, ':', '.');
        while (path.contains("..")) {
            path.replace(path.find(".."), 2, ".");
        }
        constexpr std::string_view marker{".settings."};
        if (const std::size_t offset = path.find(marker);
            offset != std::string::npos) {
            path.erase(offset, marker.size() - 1);
        }
        return "sql:guc:" + path;
    }
    if (entity.kind == entity_kind::guc_value) {
        return "sql:guc-value:" + entity.cpp_qualified_name;
    }
    std::string result{"sql:"};
    result += kind_name(entity.kind);
    result += ':';
    if (!schema_less(entity.kind)) {
        result += entity.schema + '.';
    }
    if (entity.companion && entity.kind != entity_kind::operator_
        && entity.kind != entity_kind::cast
        && entity.kind != entity_kind::grant
        && entity.kind != entity_kind::revoke) {
        result += entity.enclosing_cpp_name + '.';
    }
    result += entity.name;
    if (routine_identity_kind(entity.kind)) {
        result += routine_signature(entity);
    }
    if (entity.kind == entity_kind::grant
        || entity.kind == entity_kind::revoke
        || entity.kind == entity_kind::aggregate) {
        result += option_identity(entity);
    }
    return result;
}

[[nodiscard]] inline std::string installed_identity(
    std::string identity,
    std::string_view extension_schema) {
    std::size_t offset = 0;
    while ((offset = identity.find("$extension", offset))
           != std::string::npos) {
        identity.replace(
            offset,
            std::string_view{"$extension"}.size(),
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

[[nodiscard]] inline bool allowed_cycle_kind(entity_kind kind) noexcept {
    return kind == entity_kind::type
        || kind == entity_kind::type_role
        || kind == entity_kind::function
        || kind == entity_kind::operator_
        || kind == entity_kind::cast;
}

[[nodiscard]] inline int kind_order(entity_kind kind) noexcept {
    switch (kind) {
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
        case entity_kind::catalog_dependencies: return 90;
        case entity_kind::phase: return 100;
    }
    return 1000;
}

[[nodiscard]] inline bool handwritten_fmgr_export_macro_name(
    std::string_view spelling) noexcept {
    return spelling == "CXX_PG_FUNCTION_INFO_V1"
        || spelling == "PG_FUNCTION_INFO_V1"
        || spelling == "POSTGRES_FN"
        || spelling == "E_POSTGRES_FN";
}

[[nodiscard]] inline std::vector<handwritten_fmgr_export_owner>
scan_handwritten_fmgr_export_owners(
    const std::vector<structural_analysis::translation_unit>& units) {
    std::vector<handwritten_fmgr_export_owner> result;
    for (const structural_analysis::translation_unit& unit : units) {
        const auto& tokens = unit.tokens;
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (!handwritten_fmgr_export_macro_name(
                    tokens[index].spelling)) {
                continue;
            }
            if (index + 3 >= tokens.size()
                || tokens[index + 1].spelling != "("
                || tokens[index + 3].spelling != ")") {
                throw std::runtime_error(
                    unit.source_path.string() + ":"
                    + std::to_string(tokens[index].range.begin.line)
                    + ": malformed handwritten fmgr export macro "
                    + tokens[index].spelling);
            }
            const std::string& symbol = tokens[index + 2].spelling;
            if (!sql_identifier(symbol)
                || reserved_sql_identifier(symbol)) {
                throw std::runtime_error(
                    unit.source_path.string() + ":"
                    + std::to_string(tokens[index].range.begin.line)
                    + ": handwritten fmgr export macro has invalid symbol '"
                    + symbol + "'");
            }
            result.push_back(handwritten_fmgr_export_owner{
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
        [](const handwritten_fmgr_export_owner& left,
           const handwritten_fmgr_export_owner& right) {
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
        &handwritten_fmgr_export_owner::symbol);
    if (duplicate != result.end()) {
        throw std::runtime_error(
            "handwritten fmgr export macro for symbol '"
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
        if (entity.kind == entity_kind::table
            || entity.kind == entity_kind::type) {
            record_owners[entity.cpp_qualified_name].push_back(index);
        }
    }

    for (collected_entity& entity : program.entities) {
        if (entity.kind != entity_kind::column) {
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
    std::string extension_schema = "public",
    std::string extension_name = "bioseq",
    std::string generated_module_prefix = "bioseq.sql.generated",
    std::string fmgr_boundary_module =
        "bioseq.postgres.support.sql_boundary",
    std::string fmgr_prelude = "bioseq_common") {
    if (!detail::sql_identifier(extension_schema)
        || detail::reserved_sql_identifier(extension_schema)) {
        throw std::runtime_error(
            "invalid install-resolved extension schema '" + extension_schema
            + "'");
    }
    if (!detail::sql_identifier(extension_name)
        || detail::reserved_sql_identifier(extension_name)) {
        throw std::runtime_error(
            "invalid extension name '" + extension_name + "'");
    }
    const auto valid_module_name = [](std::string_view value) {
        if (value.empty() || value.front() == '.'
            || value.back() == '.' || value.contains("..")) {
            return false;
        }
        std::size_t begin = 0;
        while (begin < value.size()) {
            const std::size_t end = value.find('.', begin);
            const std::string_view component = value.substr(
                begin,
                end == std::string_view::npos
                    ? value.size() - begin
                    : end - begin);
            if (component.empty()
                || !((component.front() >= 'a'
                      && component.front() <= 'z')
                     || (component.front() >= 'A'
                         && component.front() <= 'Z')
                     || component.front() == '_')
                || !std::ranges::all_of(
                    component.substr(1),
                    [](char item) {
                        return (item >= 'a' && item <= 'z')
                            || (item >= 'A' && item <= 'Z')
                            || (item >= '0' && item <= '9')
                            || item == '_';
                    })) {
                return false;
            }
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        return true;
    };
    if (!valid_module_name(generated_module_prefix)) {
        throw std::runtime_error(
            "invalid generated module prefix '"
            + generated_module_prefix + "'");
    }
    if (!valid_module_name(fmgr_boundary_module)) {
        throw std::runtime_error(
            "invalid fmgr boundary module '"
            + fmgr_boundary_module + "'");
    }
    if (fmgr_prelude != "bioseq_common"
        && fmgr_prelude != "postgresql") {
        throw std::runtime_error(
            "invalid fmgr prelude '" + fmgr_prelude
            + "'; expected bioseq_common or postgresql");
    }
    detail::inherit_natural_column_schemas(program);
    std::vector<handwritten_fmgr_export_owner>
        handwritten_fmgr_export_owners =
            detail::scan_handwritten_fmgr_export_owners(
            program.translation_units);
    sql_graph graph{
        .extension_name = std::move(extension_name),
        .extension_schema = std::move(extension_schema),
        .generated_module_prefix =
            std::move(generated_module_prefix),
        .fmgr_boundary_module =
            std::move(fmgr_boundary_module),
        .fmgr_prelude = std::move(fmgr_prelude),
        .compiler_profile = std::move(program.compiler_profile),
        .clang_release =
            std::move(program.module_graph.clang_release),
        .compilation_signature =
            std::move(program.compilation_signature),
        .handwritten_fmgr_export_owners =
            std::move(handwritten_fmgr_export_owners),
    };
    graph.result_rows = std::move(program.result_rows);
    std::vector<std::string> stable_identities;
    stable_identities.reserve(program.entities.size());
    for (const collected_entity& entity : program.entities) {
        stable_identities.push_back(
            detail::stable_identity(entity));
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
                detail::option_identifier(
                    entity.source, "access_method", "default");
            if (access_method != "default") {
                std::vector<std::size_t> natural_access_methods;
                for (std::size_t candidate = 0;
                     candidate < graph.entities.size();
                     ++candidate) {
                    const collected_entity& method =
                        graph.entities[candidate].source;
                    if (method.kind == entity_kind::access_method
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
            const std::string& candidate = entity.stable_identity;
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
                const std::string& left_key = left_entity.stable_identity;
                const std::string& right_key = right_entity.stable_identity;
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
