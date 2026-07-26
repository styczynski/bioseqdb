module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:fmgr;

import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

struct rendered_fmgr_overload final {
    std::string sql_identity;
    std::string boundary_probe_site_id;
    std::string boundary_fault_key;
    std::string cpp_declaration;
    std::filesystem::path source_path;
    std::size_t source_line{};
    std::size_t sql_arity{};
    std::vector<std::string> argument_converter_keys;
    std::string result_converter_key;
    bool injected_call_context{};
};

struct rendered_fmgr_wrapper final {
    std::string public_symbol;
    std::string finfo_symbol;
    std::vector<rendered_fmgr_overload> overloads;
};

struct rendered_fmgr_entrypoint_module final {
    std::string owner_key;
    std::string source_module;
    std::string module_name;
    std::string content;
    std::string content_sha256;
    std::vector<rendered_fmgr_wrapper> wrappers;
};

namespace fmgr_detail {

inline constexpr std::string_view generated_boundary_fault_key{
    "generated_boundary_tail.std_exception"};

/**
 * Return the PostgreSQL regprocedure spelling used by runtime consumers.
 *
 * Graph identities retain their entity-kind discriminator and relocatable
 * extension-schema tokens.  Neither is valid input to PostgreSQL's
 * regprocedure resolver, so runtime identities remove those graph-only
 * components.  Fixed schemas, quoted types, and argument spellings remain
 * byte-for-byte stable.
 */
[[nodiscard]] inline std::string relocatable_routine_identity(
        const normalized_entity& entity) {
    if (entity.source.kind != entity_kind::function
        && entity.source.kind != entity_kind::sql_function
        && entity.source.kind != entity_kind::procedure) {
        throw std::runtime_error(
            "runtime SQL identity requested for non-routine graph entity '"
            + entity.stable_identity + "'");
    }
    const std::string prefix =
        "sql:" + std::string{kind_name(entity.source.kind)} + ":";
    if (!entity.stable_identity.starts_with(prefix)) {
        throw std::runtime_error(
            "routine stable identity '" + entity.stable_identity
            + "' does not begin with '" + prefix + "'");
    }
    const std::string_view stable_identity =
        std::string_view{entity.stable_identity}.substr(prefix.size());
    constexpr std::string_view extension_prefix{"$extension."};
    const auto is_schema_boundary =
        [stable_identity](std::size_t offset) {
            while (offset != 0
                   && std::isspace(static_cast<unsigned char>(
                       stable_identity[offset - 1]))) {
                --offset;
            }
            return offset == 0
                || stable_identity[offset - 1] == '('
                || stable_identity[offset - 1] == ',';
        };
    const auto begins_identifier =
        [](unsigned char character) {
            return std::isalpha(character)
                || character == '_'
                || character == '"'
                || character >= 0x80;
        };

    std::string identity;
    identity.reserve(stable_identity.size());
    bool quoted_identifier = false;
    for (std::size_t offset = 0;
         offset < stable_identity.size();) {
        if (stable_identity[offset] == '"') {
            identity.push_back(stable_identity[offset++]);
            if (quoted_identifier
                && offset < stable_identity.size()
                && stable_identity[offset] == '"') {
                identity.push_back(stable_identity[offset++]);
                continue;
            }
            quoted_identifier = !quoted_identifier;
            continue;
        }
        if (!quoted_identifier
            && stable_identity.substr(offset).starts_with(
                extension_prefix)
            && is_schema_boundary(offset)) {
            const std::size_t target =
                offset + extension_prefix.size();
            if (target < stable_identity.size()
                && begins_identifier(static_cast<unsigned char>(
                    stable_identity[target]))) {
                offset = target;
                continue;
            }
        }
        identity.push_back(stable_identity[offset++]);
    }
    if (identity.empty()
        || identity.starts_with("sql:")
        || identity.contains(extension_prefix)) {
        throw std::runtime_error(
            "routine stable identity '" + entity.stable_identity
            + "' did not normalize to a runtime SQL identity");
    }
    return identity;
}

[[nodiscard]] inline std::string generated_boundary_probe_site_id(
        std::string_view sql_identity) {
    std::string material{
        "bioseq.safety_runtime_probe.site.v2\n"};
    material += sql_identity;
    material += '\n';
    material += generated_boundary_fault_key;
    material += '\n';
    return "probe:v2:" + sha256_hex(material);
}

struct normalized_cpp_type final {
    std::string canonical;
    bool nullable{};
};

struct converter_binding final {
    std::string canonical_cpp_type;
    std::string key;
};

[[nodiscard]] inline std::string compact_cpp_type(
        std::string_view cpp_type) {
    std::string result;
    result.reserve(cpp_type.size());
    for (const unsigned char value : cpp_type) {
        if (!std::isspace(value)) {
            result.push_back(static_cast<char>(value));
        }
    }
    while (result.starts_with("const")) {
        result.erase(0, std::string_view{"const"}.size());
    }
    while (!result.empty()
           && (result.back() == '&' || result.back() == ' ')) {
        result.pop_back();
    }
    if (result.starts_with("::")) result.erase(0, 2);
    return result;
}

[[nodiscard]] inline normalized_cpp_type normalize_cpp_type(
        std::string_view cpp_type) {
    std::string compact = compact_cpp_type(cpp_type);
    static constexpr std::array optional_prefixes{
        std::string_view{"std::optional<"},
        std::string_view{"std::__1::optional<"},
    };
    for (const std::string_view optional_prefix : optional_prefixes) {
        if (compact.starts_with(optional_prefix)
            && compact.ends_with('>')) {
            return {
                .canonical = compact.substr(
                    optional_prefix.size(),
                    compact.size() - optional_prefix.size() - 1),
                .nullable = true,
            };
        }
    }
    return {.canonical = std::move(compact)};
}

[[nodiscard]] inline std::string canonical_sql_type(
        std::string_view sql_type) {
    std::string result;
    result.reserve(sql_type.size());
    for (const unsigned char value : sql_type) {
        if (!std::isspace(value) && value != '"') {
            result.push_back(static_cast<char>(std::tolower(value)));
        }
    }
    if (result == "bool") return "boolean";
    if (result == "int2") return "smallint";
    if (result == "int4" || result == "int") return "integer";
    if (result == "int8") return "bigint";
    if (result == "float4") return "real";
    if (result == "float8") return "doubleprecision";
    if (result == "char" || result == "bpchar") return "character";
    if (result == "quoted_any") return "any";
    return result;
}

[[nodiscard]] inline bool sql_type_is(
        std::string_view actual,
        std::initializer_list<std::string_view> expected) {
    const std::string canonical = canonical_sql_type(actual);
    return std::ranges::any_of(
        expected,
        [canonical](std::string_view candidate) {
            return canonical == candidate;
        });
}

struct cpp_template_binding final {
    std::string outer;
    std::vector<std::string> arguments;
    bool valid{};
};

[[nodiscard]] inline cpp_template_binding parse_cpp_template(
        std::string_view canonical) {
    const std::size_t open = canonical.find('<');
    if (open == std::string_view::npos || !canonical.ends_with('>')) {
        return {};
    }
    int depth = 0;
    std::size_t close = std::string_view::npos;
    for (std::size_t index = open; index < canonical.size(); ++index) {
        if (canonical[index] == '<') {
            ++depth;
        } else if (canonical[index] == '>') {
            --depth;
            if (depth == 0) {
                close = index;
                break;
            }
        }
    }
    if (close != canonical.size() - 1 || depth != 0) return {};

    std::vector<std::string> arguments;
    std::size_t begin = open + 1;
    depth = 0;
    for (std::size_t index = begin; index <= close; ++index) {
        const char value = index == close ? ',' : canonical[index];
        if (value == '<') {
            ++depth;
        } else if (value == '>') {
            --depth;
        } else if (value == ',' && depth == 0) {
            if (index == begin) return {};
            arguments.emplace_back(
                canonical.substr(begin, index - begin));
            begin = index + 1;
        }
    }
    const bool valid = !arguments.empty();
    return {
        .outer = std::string{canonical.substr(0, open)},
        .arguments = std::move(arguments),
        .valid = valid,
    };
}

[[nodiscard]] inline std::string cpp_leaf(
        std::string_view value) {
    const std::size_t separator = value.rfind("::");
    return std::string{
        separator == std::string_view::npos
        ? value
        : value.substr(separator + 2)};
}

[[nodiscard]] inline std::string vector_logical_sql_type(
        std::string_view expression) {
    std::string logical = cpp_leaf(expression);
    if (logical == "character") return "character";
    if (logical == "boolean" || logical == "smallint"
        || logical == "integer" || logical == "bigint"
        || logical == "date" || logical == "time"
        || logical == "timestamp" || logical == "timestamptz") {
        return logical;
    }
    throw std::runtime_error(
        "unregistered generated fmgr vector logical SQL type '"
        + std::string{expression} + "'");
}

[[nodiscard]] inline bool vector_lane_matches(
        std::string_view logical,
        std::string_view lane) {
    const std::string lane_leaf = cpp_leaf(lane);
    if (logical == "character") return lane_leaf == "bpchar_view";
    if (logical == "smallint") {
        return lane_leaf == "int16_t" || lane_leaf == "int16";
    }
    if (logical == "integer") {
        return lane_leaf == "int32_t" || lane_leaf == "int32";
    }
    if (logical == "bigint") {
        return lane_leaf == "int64_t" || lane_leaf == "int64";
    }
    if (logical == "date") return lane_leaf == "DateADT";
    if (logical == "time") return lane_leaf == "TimeADT";
    if (logical == "timestamp") return lane_leaf == "Timestamp";
    if (logical == "timestamptz") return lane_leaf == "TimestampTz";
    if (logical == "boolean") return lane_leaf == "bool";
    return false;
}

[[nodiscard]] inline converter_binding scalar_converter(
        std::string_view cpp_type,
        std::string_view sql_type,
        bool result) {
    const normalized_cpp_type normalized = normalize_cpp_type(cpp_type);
    const std::string direction = result ? "result." : "argument.";
    converter_binding binding;
    const cpp_template_binding wrapper =
        parse_cpp_template(normalized.canonical);
    const std::string wrapper_leaf =
        wrapper.valid ? cpp_leaf(wrapper.outer) : std::string{};
    if (wrapper_leaf == "vector_operand"
        || wrapper_leaf == "vector_column"
        || wrapper_leaf == "vector_result") {
        if (wrapper.arguments.size() != 2
            || (normalized.nullable
                && wrapper_leaf == "vector_result")) {
            throw std::runtime_error(
                "generated fmgr vector boundary requires one logical SQL "
                "type and one lane type");
        }
        if ((result && wrapper_leaf != "vector_result")
            || (!result && wrapper_leaf != "vector_operand"
                && wrapper_leaf != "vector_column")) {
            throw std::runtime_error(
                "generated fmgr vector operands/columns are input-only and "
                "vector_result is result-only");
        }
        const std::string logical =
            vector_logical_sql_type(wrapper.arguments.front());
        if (canonical_sql_type(sql_type) != logical
            || !vector_lane_matches(
                logical, wrapper.arguments.back())) {
            throw std::runtime_error(
                "generated fmgr vector logical SQL type/lane disagrees "
                "with the routine catalog signature");
        }
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = direction + wrapper_leaf + "["
                + logical + "," + cpp_leaf(wrapper.arguments.back()) + "]",
        };
    } else if (cpp_leaf(normalized.canonical) == "vector_null_mask"
               && !result
               && sql_type_is(sql_type, {"any"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = "argument.vector_null_mask[any]",
        };
    } else if (cpp_leaf(normalized.canonical)
                   == "int64_array_aggregate_state"
               && sql_type_is(sql_type, {"internal"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = direction + "aggregate_state.int64_array",
        };
    } else if (cpp_leaf(normalized.canonical)
                   == "int128_aggregate_state"
               && sql_type_is(sql_type, {"internal"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = direction + "aggregate_state.int128",
        };
    } else if (cpp_leaf(normalized.canonical) == "numeric_result"
               && result
               && sql_type_is(sql_type, {"numeric"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = "result.numeric.owning",
        };
    } else if (cpp_leaf(normalized.canonical) == "sql_jsonb"
               && sql_type_is(sql_type, {"jsonb"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = direction + "jsonb.owning",
        };
    } else if (cpp_leaf(normalized.canonical) == "sql_bytea"
               && !result
               && sql_type_is(sql_type, {"bytea"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = "argument.bytea.owning",
        };
    } else if (cpp_leaf(normalized.canonical) == "sql_interval"
               && result
               && sql_type_is(sql_type, {"interval"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = "result.interval.owning",
        };
    } else if (cpp_leaf(normalized.canonical) == "sql_tid"
               && !result
               && sql_type_is(sql_type, {"tid"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = "argument.tid.by_value",
        };
    } else if (cpp_leaf(normalized.canonical) == "DateADT"
               && sql_type_is(sql_type, {"date"})) {
        binding = {
            .canonical_cpp_type = normalized.canonical,
            .key = direction + "date.by_value",
        };
    } else if (normalized.canonical == "bool"
        && sql_type_is(sql_type, {"boolean"})) {
        binding = {.canonical_cpp_type = "bool",
                   .key = direction + "bool.by_value"};
    } else if ((normalized.canonical == "std::int16_t"
                || normalized.canonical == "int16_t"
                || normalized.canonical == "int16")
               && sql_type_is(sql_type, {"smallint"})) {
        binding = {.canonical_cpp_type = "std::int16_t",
                   .key = direction + "int16.by_value"};
    } else if ((normalized.canonical == "std::int32_t"
                || normalized.canonical == "int32_t"
                || normalized.canonical == "int32"
                || normalized.canonical == "int")
               && sql_type_is(sql_type, {"integer"})) {
        binding = {.canonical_cpp_type = "std::int32_t",
                   .key = direction + "int32.by_value"};
    } else if ((normalized.canonical == "std::int64_t"
                || normalized.canonical == "int64_t"
                || normalized.canonical == "int64"
                || normalized.canonical == "long"
                || normalized.canonical == "longlong")
               && sql_type_is(sql_type, {"bigint"})) {
        binding = {.canonical_cpp_type = "std::int64_t",
                   .key = direction + "int64.by_value"};
    } else if (normalized.canonical == "float"
               && sql_type_is(sql_type, {"real"})) {
        binding = {.canonical_cpp_type = "float",
                   .key = direction + "float32.by_value"};
    } else if (normalized.canonical == "double"
               && sql_type_is(sql_type, {"doubleprecision"})) {
        binding = {.canonical_cpp_type = "double",
                   .key = direction + "float64.by_value"};
    } else if ((normalized.canonical == "Oid"
                || normalized.canonical == "oid")
               && sql_type_is(sql_type, {"oid", "regclass"})) {
        binding = {.canonical_cpp_type = "Oid",
                   .key = direction + "oid.by_value["
                       + canonical_sql_type(sql_type) + "]"};
    } else if (normalized.canonical == "ArrayType*") {
        const std::string canonical = canonical_sql_type(sql_type);
        if (canonical.empty() || canonical == "infer"
            || !canonical.ends_with("[]")) {
            throw std::runtime_error(
                "generated fmgr ArrayType* boundary requires a concrete SQL "
                "array type");
        }
        binding = {
            .canonical_cpp_type = "ArrayType*",
            .key = direction + "array."
                + (result ? "owning[" : "detoasted[")
                + canonical + "]",
        };
    } else if ((normalized.canonical == "char*"
                || normalized.canonical == "constchar*")
               && sql_type_is(sql_type, {"cstring"})) {
        binding = {
            .canonical_cpp_type = "char*",
            .key = direction
                + (result ? "cstring.owning"
                          : "cstring.borrowed"),
        };
    } else if (normalized.canonical == "StringInfo"
               && !result
               && sql_type_is(sql_type, {"internal"})) {
        binding = {
            .canonical_cpp_type = "StringInfo",
            .key = "argument.string_info.borrowed",
        };
    } else if (normalized.canonical == "text*"
               && result
               && sql_type_is(
                   sql_type, {"text", "varchar", "bytea"})) {
        binding = {
            .canonical_cpp_type = "text*",
            .key = direction + canonical_sql_type(sql_type)
                + ".owning",
        };
    } else if (normalized.canonical == "void*"
               && sql_type_is(sql_type, {"internal"})) {
        binding = {
            .canonical_cpp_type = "void*",
            .key = direction + "internal.pointer",
        };
    } else if (cpp_leaf(normalized.canonical)
                   == "table_am_handler_context"
               && !result
               && sql_type_is(sql_type, {"internal"})) {
        binding = {
            .canonical_cpp_type =
                "bioseq::pg_utils::table_am_handler_context",
            .key = "argument.table_am_handler.context",
        };
    } else if ((cpp_leaf(normalized.canonical)
                    == "TableAmRoutine*"
                || cpp_leaf(normalized.canonical)
                    == "TableAmRoutineconst*")
               && result
               && sql_type_is(sql_type, {"table_am_handler"})) {
        binding = {
            .canonical_cpp_type = "const TableAmRoutine*",
            .key = "result.table_am_handler.pointer",
        };
    } else if (cpp_leaf(normalized.canonical) == "NameData"
               && !result
               && sql_type_is(sql_type, {"name"})) {
        binding = {
            .canonical_cpp_type = "NameData",
            .key = "argument.name.owning",
        };
    } else if (cpp_leaf(normalized.canonical) == "seq*"
               && result
               && sql_type_is(sql_type, {"bioseq.seq"})) {
        binding = {
            .canonical_cpp_type = "bioseq::pg_types::seq*",
            .key = direction + "bioseq.seq.pointer",
        };
    } else if (cpp_leaf(normalized.canonical) == "letter"
               && sql_type_is(sql_type, {"bioseq.letter"})) {
        binding = {
            .canonical_cpp_type = "bioseq::pg_types::letter",
            .key = direction + "bioseq.letter.by_value",
        };
    } else if (normalized.canonical == "Datum"
               || normalized.canonical == "datum") {
        if (sql_type.empty() || canonical_sql_type(sql_type) == "infer") {
            throw std::runtime_error(
                "opaque Datum SQL boundary requires an explicit SQL type");
        }
        binding = {.canonical_cpp_type = "Datum",
                   .key = direction + "datum.opaque["
                       + canonical_sql_type(sql_type) + "]"};
    } else if (normalized.canonical == "std::string"
               && sql_type_is(sql_type, {"text", "varchar"})) {
        binding = {.canonical_cpp_type = "std::string",
                   .key = direction + canonical_sql_type(sql_type)
                       + ".owning"};
    } else {
        throw std::runtime_error(
            "no generated fmgr " + direction
            + "converter for C++ type '" + std::string{cpp_type}
            + "' and SQL type '" + std::string{sql_type} + "'");
    }
    if (normalized.nullable) {
        binding.canonical_cpp_type =
            "std::optional<" + binding.canonical_cpp_type + ">";
        binding.key += ".nullable";
    }
    return binding;
}

[[nodiscard]] inline bool call_context_type(
        std::string_view cpp_type) {
    const std::string compact = compact_cpp_type(cpp_type);
    return compact == "bioseq::pg_utils::sql_call_context"
        || compact == "sql_call_context";
}

[[nodiscard]] inline bool compact_sequence_owner_module(
        std::string_view source_module) noexcept {
    return source_module.starts_with(
        "bioseq.types.pg_compact_sequence:");
}

[[nodiscard]] inline bool table_entrypoint_owner_module(
        std::string_view source_module) noexcept {
    return source_module == "bioseq.table:access_hooks"
        || source_module == "bioseq.table:access_method"
        || source_module == "bioseq.table:custom_scan";
}

[[nodiscard]] inline std::string fmgr_owner_key(
        std::string_view source_module) {
    if (compact_sequence_owner_module(source_module)) {
        return "types_pg_compact_sequence";
    }
    if (table_entrypoint_owner_module(source_module)) {
        return "table_access_hooks";
    }
    if (source_module.starts_with("bioseq.")) {
        source_module.remove_prefix(
            std::string_view{"bioseq."}.size());
    }
    std::string result;
    bool separator = false;
    for (const unsigned char value : source_module) {
        if ((value >= 'a' && value <= 'z')
            || (value >= '0' && value <= '9')) {
            if (separator && !result.empty()) result.push_back('_');
            result.push_back(static_cast<char>(value));
            separator = false;
        } else if (value == '.' || value == ':' || value == '_') {
            separator = true;
        } else {
            throw std::runtime_error(
                "SQL owner module '" + std::string{source_module}
                + "' cannot form an fmgr owner key");
        }
    }
    if (result.empty()
        || (result.front() >= '0' && result.front() <= '9')) {
        throw std::runtime_error(
            "SQL owner module '" + std::string{source_module}
            + "' produced an invalid fmgr owner key");
    }
    return result;
}

[[nodiscard]] inline std::string fmgr_module_name(
        std::string_view source_module,
        std::string_view owner_key) {
    if (owner_key == "table_access_hooks") {
        return "bioseq.table:sql_entrypoints";
    }
    if (owner_key == "types_pg_compact_sequence") {
        return "bioseq.types.pg_compact_sequence:"
               "generated_sql_entrypoints";
    }
    return "bioseq.sql.generated.production."
        + std::string{owner_key} + ".entrypoints";
}

[[nodiscard]] inline std::string fmgr_owner_import(
        std::string_view source_module) {
    if (source_module
        == "bioseq.storage.manager:sequence_storage_manager") {
        return "bioseq.storage.manager";
    }
    if (table_entrypoint_owner_module(source_module)) {
        const std::size_t separator = source_module.find(':');
        return ":" + std::string{source_module.substr(separator + 1)};
    }
    if (compact_sequence_owner_module(source_module)) {
        const std::size_t separator = source_module.find(':');
        return ":" + std::string{source_module.substr(separator + 1)};
    }
    return std::string{source_module};
}

[[nodiscard]] inline bool reviewed_handwritten_external_routine(
        const normalized_entity& entity) noexcept {
    return !entity.source.typed_install_owner
        && entity.source.kind == entity_kind::function
        && entity.source.name == "bioseq_external_handler"
        && entity.source.source_module
            == "bioseq.pg.external.relation"
        && entity.source.cpp_qualified_name
            == "bioseq::external::sql_routines::"
               "bioseq_external_handler"
        && entity.source.schema == "bioseq"
        && entity.source.result_sql_type == "table_am_handler";
}

[[nodiscard]] inline bool generated_c_routine(
        const normalized_entity& entity) noexcept {
    return !reviewed_handwritten_external_routine(entity)
        && !entity.source.typed_install_owner
        && (entity.source.kind == entity_kind::function
            || entity.source.kind == entity_kind::procedure);
}

[[nodiscard]] inline std::string cpp_owner_scope(
        std::string_view qualified_name) {
    const std::size_t separator = qualified_name.rfind("::");
    return separator == std::string_view::npos
        ? std::string{}
        : std::string{qualified_name.substr(0, separator)};
}

[[nodiscard]] inline converter_binding parameter_converter(
        const routine_parameter& parameter) {
    return scalar_converter(
        parameter.cpp_type, parameter.sql_type, false);
}

[[nodiscard]] inline bool row_result(
        routine_result_form form) noexcept {
    return form == routine_result_form::returns_table
        || form == routine_result_form::named_out
        || form == routine_result_form::setof_named_out;
}

[[nodiscard]] inline const collected_result_row* result_row_for(
        const collected_entity& entity,
        const sql_graph& graph) {
    if (!row_result(entity.result.form)) return nullptr;
    const collected_result_row* result = nullptr;
    for (const collected_result_row& candidate : graph.result_rows) {
        if (candidate.cpp_qualified_name
            != entity.result.row_cpp_name) {
            continue;
        }
        if (result != nullptr) {
            throw std::runtime_error(
                "generated fmgr SQL owner '"
                + entity.cpp_qualified_name
                + "' resolves duplicate result Row metadata for '"
                + entity.result.row_cpp_name + "'");
        }
        result = &candidate;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "generated fmgr SQL owner '" + entity.cpp_qualified_name
            + "' cannot resolve result Row metadata for '"
            + entity.result.row_cpp_name + "'");
    }
    return result;
}

[[nodiscard]] inline std::string result_form_key(
        routine_result_form form) {
    switch (form) {
        case routine_result_form::returns_table:
            return "returns_table";
        case routine_result_form::named_out:
            return "named_out";
        case routine_result_form::setof_named_out:
            return "setof_named_out";
        case routine_result_form::scalar:
            return "scalar";
        case routine_result_form::void_:
            return "void";
        case routine_result_form::setof:
            return "setof";
    }
    throw std::runtime_error("unknown generated fmgr result form");
}

[[nodiscard]] inline converter_binding result_converter(
        const collected_entity& entity,
        const collected_result_row* row) {
    if (entity.result.form == routine_result_form::void_) {
        if (normalize_cpp_type(entity.cpp_return_type).canonical != "void"
            || canonical_sql_type(entity.result_sql_type) != "void") {
            throw std::runtime_error(
                "generated fmgr void result requires exact C++ void and "
                "SQL void types for '" + entity.cpp_qualified_name + "'");
        }
        return {
            .canonical_cpp_type = "void",
            .key = "result.void",
        };
    }
    if (row != nullptr) {
        std::string key = "result."
            + result_form_key(entity.result.form) + "["
            + row->cpp_qualified_name + "](";
        for (std::size_t index = 0; index < row->fields.size();
             ++index) {
            if (index != 0) key += ",";
            key += row->fields[index].name + ":"
                + canonical_sql_type(row->fields[index].sql_type);
        }
        key += ")";
        return {
            .canonical_cpp_type = entity.result.cpp_type,
            .key = std::move(key),
        };
    }
    return scalar_converter(
        entity.cpp_return_type, entity.result_sql_type, true);
}

[[nodiscard]] inline std::string overload_helper_name(
        std::string_view symbol,
        std::size_t arity) {
    return "invoke_" + std::string{symbol}
        + "_arity_" + std::to_string(arity);
}

[[nodiscard]] inline std::string render_argument_expression(
        const converter_binding& converter,
        std::size_t sql_index) {
    return "bioseq::pg_utils::sql_argument<"
        + converter.canonical_cpp_type + ">(fcinfo, "
        + std::to_string(sql_index) + ")";
}

[[nodiscard]] inline std::string render_owner_call(
        const normalized_entity& entity,
        std::vector<converter_binding>& argument_converters,
        std::vector<std::string>& argument_initializers,
        std::vector<std::string>& argument_capture_names,
        bool& injected_context) {
    argument_converters.clear();
    argument_initializers.clear();
    argument_capture_names.clear();
    injected_context = false;
    std::vector<std::string> arguments;
    arguments.reserve(entity.source.parameters.size() + 1);
    std::size_t sql_index = 0;
    for (const routine_parameter& parameter :
         entity.source.parameters) {
        if (parameter.injected_call_context) {
            if (!call_context_type(parameter.cpp_type)) {
                throw std::runtime_error(
                    "SQL owner '" + entity.source.cpp_qualified_name
                    + "' marks a non-context parameter as injected");
            }
            if (injected_context) {
                throw std::runtime_error(
                    "SQL owner '" + entity.source.cpp_qualified_name
                    + "' has more than one injected sql_call_context");
            }
            injected_context = true;
            arguments.push_back(
                "bioseq::pg_utils::sql_context_argument(fcinfo)");
            continue;
        }
        if (call_context_type(parameter.cpp_type)) {
            throw std::runtime_error(
                "SQL owner '" + entity.source.cpp_qualified_name
                + "' has an unmarked sql_call_context parameter");
        }
        converter_binding converter =
            parameter_converter(parameter);
        if (converter.canonical_cpp_type == "ArrayType*"
            || converter.canonical_cpp_type
                == "std::optional<ArrayType*>") {
            const bool nullable =
                converter.canonical_cpp_type
                == "std::optional<ArrayType*>";
            const std::string local =
                "sql_array_argument_"
                + std::to_string(sql_index);
            argument_initializers.push_back(
                "    auto " + local
                + " = bioseq::pg_utils::sql_"
                + (nullable ? "nullable_array_argument"
                            : "array_argument")
                + "(fcinfo, "
                + std::to_string(sql_index) + ");\n");
            argument_capture_names.push_back(local);
            arguments.push_back(
                local + (nullable ? ".optional()" : ".get()"));
            ++sql_index;
        } else {
            arguments.push_back(
                render_argument_expression(converter, sql_index++));
        }
        argument_converters.push_back(std::move(converter));
    }
    std::string result = "::" + entity.source.cpp_qualified_name + "(";
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) result += ", ";
        result += arguments[index];
    }
    result += ")";
    return result;
}

[[nodiscard]] inline std::size_t sql_arity(
        const normalized_entity& entity) {
    return sql_parameters(entity.source).size();
}

[[nodiscard]] inline std::string result_encoder_name(
        const collected_entity& entity,
        std::size_t arity) {
    return overload_helper_name(entity.name, arity)
        + "_result_encoder";
}

[[nodiscard]] inline std::string render_result_encoder(
        const collected_entity& entity,
        const collected_result_row& row,
        std::size_t arity) {
    if (row.fields.empty()) {
        throw std::runtime_error(
            "generated fmgr result Row '" + row.cpp_qualified_name
            + "' has no fields");
    }
    for (const collected_result_field& field : row.fields) {
        const normalized_cpp_type normalized =
            normalize_cpp_type(field.cpp_type);
        if (normalized.canonical == "Datum"
            || normalized.canonical == "datum") {
            throw std::runtime_error(
                "generated fmgr result Row field '"
                + row.cpp_qualified_name + "::" + field.name
                + "' cannot expose opaque Datum storage");
        }
        static_cast<void>(
            scalar_converter(field.cpp_type, field.sql_type, true));
    }
    std::ostringstream output;
    output
        << "struct " << result_encoder_name(entity, arity)
        << " final {\n"
        << "    inline static constexpr std::array<std::string_view, "
        << row.fields.size() << "> column_names{";
    for (std::size_t index = 0; index < row.fields.size(); ++index) {
        if (index != 0) output << ", ";
        output << "\"" << row.fields[index].name << "\"";
    }
    output
        << "};\n\n"
        << "    [[nodiscard]] auto operator()(\n"
        << "            const ::" << row.cpp_qualified_name
        << "& row) const {\n"
        << "        return bioseq::pg_utils::sql_result_row(\n";
    for (std::size_t index = 0; index < row.fields.size(); ++index) {
        output << "            row." << row.fields[index].name;
        output << (index + 1 == row.fields.size() ? ");\n" : ",\n");
    }
    output << "    }\n"
           << "};\n\n";
    return output.str();
}

[[nodiscard]] inline std::string render_overload_helper(
        const normalized_entity& entity,
        const sql_graph& graph,
        rendered_fmgr_overload& artifact) {
    std::vector<converter_binding> arguments;
    std::vector<std::string> argument_initializers;
    std::vector<std::string> argument_capture_names;
    bool injected_context = false;
    std::string owner_call =
        render_owner_call(
            entity,
            arguments,
            argument_initializers,
            argument_capture_names,
            injected_context);
    if (!argument_initializers.empty()) {
        std::string captures{"["};
        bool first_capture = true;
        if (owner_call.contains("fcinfo")) {
            captures += "fcinfo";
            first_capture = false;
        }
        for (const std::string& capture : argument_capture_names) {
            if (!first_capture) captures += ", ";
            captures += "&" + capture;
            first_capture = false;
        }
        captures += "]";
        owner_call =
            "bioseq::pg_utils::invoke_pg_as_cpp(" + captures
            + " { return "
            + owner_call + "; })";
    }
    const collected_result_row* row =
        result_row_for(entity.source, graph);
    const converter_binding result =
        result_converter(entity.source, row);
    const std::string sql_identity =
        relocatable_routine_identity(entity);
    artifact = {
        .sql_identity = sql_identity,
        .boundary_probe_site_id =
            generated_boundary_probe_site_id(sql_identity),
        .boundary_fault_key =
            std::string{generated_boundary_fault_key},
        .cpp_declaration = entity.source.cpp_qualified_name,
        .source_path = entity.source.source_path,
        .source_line = entity.source.source_range.begin.line,
        .sql_arity = sql_arity(entity),
        .result_converter_key = result.key,
        .injected_call_context = injected_context,
    };
    for (const converter_binding& argument : arguments) {
        artifact.argument_converter_keys.push_back(argument.key);
    }

    std::ostringstream output;
    if (row != nullptr) {
        output << render_result_encoder(
            entity.source, *row, artifact.sql_arity);
    }
    output
        << "[[nodiscard]] Datum "
        << overload_helper_name(entity.source.name, artifact.sql_arity)
        << "(FunctionCallInfo fcinfo) {\n";
    for (const std::string& initializer : argument_initializers) {
        output << initializer;
    }
    if (entity.source.result.form == routine_result_form::void_) {
        output
            << "    " << owner_call << ";\n"
            << "    return bioseq::pg_utils::sql_void_result(fcinfo);\n";
    } else {
        output
            << "    return bioseq::pg_utils::sql_result_to_datum(\n"
            << "        fcinfo, " << owner_call;
        if (row != nullptr) {
            output << ",\n        "
                << result_encoder_name(
                       entity.source, artifact.sql_arity)
                << "{}";
        }
        output << ");\n";
    }
    output << "}\n\n";
    return output.str();
}

[[nodiscard]] inline std::string render_boundary_probe_descriptor(
        std::string_view site_id,
        std::string_view fault_key,
        std::string_view indentation) {
    std::ostringstream output;
    output
        << indentation
        << "bioseq::pg_utils::sql_boundary_probe_descriptor{\n"
        << indentation << "    .site_id = \""
        << site_id << "\",\n"
        << indentation << "    .fault_key = \""
        << fault_key << "\",\n"
        << indentation << "}";
    return output.str();
}

[[nodiscard]] inline std::string render_boundary_probe_descriptor(
        const rendered_fmgr_overload& overload,
        std::string_view indentation) {
    return render_boundary_probe_descriptor(
        overload.boundary_probe_site_id,
        overload.boundary_fault_key,
        indentation);
}

[[nodiscard]] inline std::string render_public_wrapper(
        const rendered_fmgr_wrapper& wrapper) {
    std::ostringstream output;
    output
        << "extern \"C\" {\n"
        << "PG_FUNCTION_INFO_V1(" << wrapper.public_symbol << ");\n"
        << "}\n\n"
        << "extern \"C\" PGDLLEXPORT Datum "
        << wrapper.public_symbol << "(PG_FUNCTION_ARGS) {\n";
    if (wrapper.overloads.size() > 1) {
        output << "    switch (PG_NARGS()) {\n";
        for (const rendered_fmgr_overload& overload :
             wrapper.overloads) {
            output
                << "        case " << overload.sql_arity << ":\n"
                << "            return "
                   "bioseq::pg_utils::invoke_sql_boundary(\n"
                << render_boundary_probe_descriptor(
                       overload, "                ")
                << ",\n"
                << "                [fcinfo] -> Datum {\n"
                << "                    return "
                << overload_helper_name(
                       wrapper.public_symbol,
                       overload.sql_arity)
                << "(fcinfo);\n"
                << "                });\n";
        }
        output
            << "        default:\n"
            << "            ereport(\n"
            << "                ERROR,\n"
            << "                (errcode(ERRCODE_INTERNAL_ERROR),\n"
            << "                 errmsg(\"bioseq SQL callback raised a "
               "C++ exception\"),\n"
            << "                 errdetail(\n"
            << "                     \"%s\",\n"
            << "                     \"generated fmgr overload wrapper "
            << wrapper.public_symbol
            << " received an unsupported arity\")));\n"
            << "            pg_unreachable();\n"
            << "    }\n"
            << "}\n\n";
        return output.str();
    }
    output
        << "    return bioseq::pg_utils::invoke_sql_boundary(\n";
    output
        << render_boundary_probe_descriptor(
               wrapper.overloads.front(), "        ")
        << ",\n";
    output << "        [fcinfo] -> Datum {\n";
    const rendered_fmgr_overload& overload =
        wrapper.overloads.front();
    output
        << "            return "
        << overload_helper_name(
               wrapper.public_symbol, overload.sql_arity)
        << "(fcinfo);\n";
    output
        << "        });\n"
        << "}\n\n";
    return output.str();
}

[[nodiscard]] inline std::string render_module(
        rendered_fmgr_entrypoint_module& module,
        const std::map<
            std::string,
            std::vector<const normalized_entity*>,
            std::less<>>& entities_by_symbol,
        const sql_graph& graph) {
    std::set<std::string, std::less<>> result_row_imports;
    std::set<std::string, std::less<>> owner_imports;
    for (const auto& [symbol, entities] : entities_by_symbol) {
        static_cast<void>(symbol);
        for (const normalized_entity* entity : entities) {
            owner_imports.insert(
                fmgr_owner_import(entity->source.source_module));
            const collected_result_row* row =
                result_row_for(entity->source, graph);
            if (row == nullptr) continue;
            if (!row->source_module.ends_with(".sql_rows")
                || row->source_module.contains(':')) {
                throw std::runtime_error(
                    "generated fmgr result Row '"
                    + row->cpp_qualified_name
                    + "' must be provided by an ordinary .sql_rows "
                      "module");
            }
            if (row->source_module == module.module_name) {
                throw std::runtime_error(
                    "generated fmgr module '" + module.module_name
                    + "' cannot import its own result Row module");
            }
            result_row_imports.insert(row->source_module);
        }
    }
    if (module.owner_key == "types_pg_compact_sequence") {
        owner_imports.insert(":flat_abi");
        owner_imports.insert(":letter");
    }

    std::ostringstream body;
    body << "namespace {\n\n";
    for (const auto& [symbol, entities] : entities_by_symbol) {
        rendered_fmgr_wrapper wrapper{
            .public_symbol = symbol,
            .finfo_symbol = "pg_finfo_" + symbol,
        };
        std::set<std::size_t> arities;
        for (const normalized_entity* entity : entities) {
            rendered_fmgr_overload overload;
            body << render_overload_helper(
                *entity, graph, overload);
            if (!arities.insert(overload.sql_arity).second) {
                throw std::runtime_error(
                    "generated fmgr overload group '" + symbol
                    + "' contains more than one SQL identity with arity "
                    + std::to_string(overload.sql_arity));
            }
            wrapper.overloads.push_back(std::move(overload));
        }
        std::ranges::sort(
            wrapper.overloads,
            {},
            &rendered_fmgr_overload::sql_arity);
        if (wrapper.overloads.size() > 1
            && symbol != "from_array"
            && symbol != "mmsearch") {
            throw std::runtime_error(
                "generated fmgr runtime arity dispatch is permitted "
                "only for the audited from_array/mmsearch wrappers; "
                "unexpected shared symbol '" + symbol + "'");
        }
        if (wrapper.overloads.size() > 1
            && arities
                != std::set<std::size_t>{2, 3}) {
            throw std::runtime_error(
                "audited generated fmgr overload wrapper '" + symbol
                + "' must have exactly arities 2 and 3");
        }
        module.wrappers.push_back(std::move(wrapper));
    }
    body << "}  // namespace\n\n";
    for (const rendered_fmgr_wrapper& wrapper : module.wrappers) {
        body << render_public_wrapper(wrapper);
    }
    const std::string body_text = body.str();

    std::ostringstream output;
    output
        << "module;\n\n"
        << "#include \"common.h\"\n\n"
        << "#include <array>\n"
        << "#include <cstdint>\n"
        << "#include <optional>\n"
        << "#include <stdexcept>\n"
        << "#include <string>\n"
        << "#include <string_view>\n\n"
        << "export module " << module.module_name << ";\n\n";
    for (const std::string& owner_module : owner_imports) {
        output << "import " << owner_module << ";\n";
    }
    for (const std::string& row_module : result_row_imports) {
        output << "import " << row_module << ";\n";
    }
    if (body_text.contains("bioseq::sql::pg::")) {
        output << "import bioseq.codegen.sql.annotation_model;\n";
    }
    output
        << "import bioseq.pg_utils.sql_boundary;\n\n"
        << body_text;
    return output.str();
}

struct vector_association final {
    std::string scalar_name;
    std::string vector_name;
    std::string left_type;
    std::string right_type;
    std::string result_type;
};

[[nodiscard]] inline std::optional<vector_association>
vector_association_for(const normalized_entity& entity) {
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(entity.source);
    if (entity.source.kind != entity_kind::function
        || parameters.size() != 2) {
        return std::nullopt;
    }
    const cpp_template_binding left =
        parse_cpp_template(
            normalize_cpp_type(parameters[0]->cpp_type).canonical);
    const cpp_template_binding right =
        parse_cpp_template(
            normalize_cpp_type(parameters[1]->cpp_type).canonical);
    const cpp_template_binding result =
        parse_cpp_template(
            normalize_cpp_type(
                entity.source.cpp_return_type).canonical);
    if (!left.valid || !right.valid || !result.valid
        || cpp_leaf(left.outer) != "vector_operand"
        || cpp_leaf(right.outer) != "vector_operand"
        || cpp_leaf(result.outer) != "vector_result") {
        return std::nullopt;
    }
    if (left.arguments.size() != 2 || right.arguments.size() != 2
        || result.arguments.size() != 2
        || !entity.source.name.starts_with('v')
        || entity.source.name.size() == 1) {
        throw std::runtime_error(
            "vector comparison owner has a malformed typed boundary: "
            + entity.source.cpp_qualified_name);
    }
    const std::string left_type =
        vector_logical_sql_type(left.arguments.front());
    const std::string right_type =
        vector_logical_sql_type(right.arguments.front());
    const std::string result_type =
        vector_logical_sql_type(result.arguments.front());
    if (!vector_lane_matches(left_type, left.arguments.back())
        || !vector_lane_matches(right_type, right.arguments.back())
        || !vector_lane_matches(result_type, result.arguments.back())
        || result_type != "boolean") {
        throw std::runtime_error(
            "vector comparison owner has an unregistered logical/lane "
            "association: " + entity.source.cpp_qualified_name);
    }
    return vector_association{
        .scalar_name = entity.source.name.substr(1),
        .vector_name = entity.source.name,
        .left_type = left_type,
        .right_type = right_type,
        .result_type = result_type,
    };
}

[[nodiscard]] inline std::map<
    std::string,
    std::pair<std::string, std::string>,
    std::less<>>
expected_vector_associations() {
    struct family final {
        std::string_view vector_stem;
        std::string_view left_type;
        std::string_view right_type;
        bool underscore{};
    };
    static constexpr std::array families{
        family{"vbpchar", "character", "character", false},
        family{"vint2", "smallint", "smallint", false},
        family{"vint24", "smallint", "integer", false},
        family{"vint28", "smallint", "bigint", false},
        family{"vint4", "integer", "integer", false},
        family{"vint42", "integer", "smallint", false},
        family{"vint48", "integer", "bigint", false},
        family{"vint8", "bigint", "bigint", false},
        family{"vint82", "bigint", "smallint", false},
        family{"vint84", "bigint", "integer", false},
        family{"vdate", "date", "date", true},
        family{"vtime", "time", "time", true},
        family{"vtimestamp", "timestamp", "timestamp", true},
        family{
            "vtimestamptz",
            "timestamptz",
            "timestamptz",
            true},
    };
    static constexpr std::array operations{
        std::string_view{"eq"},
        std::string_view{"ne"},
        std::string_view{"gt"},
        std::string_view{"lt"},
        std::string_view{"le"},
        std::string_view{"ge"},
    };
    std::map<
        std::string,
        std::pair<std::string, std::string>,
        std::less<>>
        result;
    for (const family& current : families) {
        for (const std::string_view operation : operations) {
            std::string name{current.vector_stem};
            if (current.underscore) name.push_back('_');
            name += operation;
            result.emplace(
                std::move(name),
                std::pair{
                    std::string{current.left_type},
                    std::string{current.right_type}});
        }
    }
    return result;
}

[[nodiscard]] inline std::string vector_type_oid(
        std::string_view sql_type) {
    if (sql_type == "character") return "BPCHAROID";
    if (sql_type == "smallint") return "INT2OID";
    if (sql_type == "integer") return "INT4OID";
    if (sql_type == "bigint") return "INT8OID";
    if (sql_type == "date") return "DATEOID";
    if (sql_type == "time") return "TIMEOID";
    if (sql_type == "timestamp") return "TIMESTAMPOID";
    if (sql_type == "timestamptz") return "TIMESTAMPTZOID";
    if (sql_type == "boolean") return "BOOLOID";
    throw std::runtime_error(
        "vector comparison registry cannot render SQL type '"
        + std::string{sql_type} + "'");
}

[[nodiscard]] inline std::string generated_profile_name(
        std::string_view compiler_profile) {
    if (compiler_profile.empty()) return "production";
    if (compiler_profile != "production"
        && compiler_profile != "test_indexes") {
        throw std::runtime_error(
            "generated SQL module has unsupported compiler profile '"
            + std::string{compiler_profile} + "'");
    }
    return std::string{compiler_profile};
}

[[nodiscard]] inline std::string vector_registry_module_name(
        std::string_view compiler_profile,
        std::string_view registry) {
    return "bioseq.sql.generated."
        + generated_profile_name(compiler_profile) + "."
        + std::string{registry};
}

inline void validate_vector_associations(
        std::vector<vector_association> associations) {
    const auto expected = expected_vector_associations();
    if (associations.size() != expected.size()) {
        throw std::runtime_error(
            "generated vector comparison registry requires exactly "
            + std::to_string(expected.size()) + " typed owners, found "
            + std::to_string(associations.size()));
    }
    std::ranges::sort(
        associations,
        {},
        &vector_association::vector_name);
    for (const vector_association& association : associations) {
        const auto expected_row = expected.find(association.vector_name);
        if (expected_row == expected.end()
            || expected_row->second.first != association.left_type
            || expected_row->second.second != association.right_type
            || association.scalar_name
                != association.vector_name.substr(1)
            || association.result_type != "boolean") {
            throw std::runtime_error(
                "generated vector comparison registry contains an "
                "unexpected association for '"
                + association.vector_name + "'");
        }
    }
    if (std::ranges::adjacent_find(
            associations,
            {},
            &vector_association::vector_name)
        != associations.end()) {
        throw std::runtime_error(
            "generated vector comparison registry contains a duplicate "
            "owner");
    }
}

[[nodiscard]] inline rendered_fmgr_entrypoint_module
render_vector_comparison_registry_from_associations(
        std::vector<vector_association> associations,
        std::string_view compiler_profile) {
    validate_vector_associations(associations);
    std::ranges::sort(
        associations,
        {},
        &vector_association::vector_name);
    const std::string module_name = vector_registry_module_name(
        compiler_profile, "vector_comparison_registry");

    std::ostringstream output;
    output
        << "module;\n\n"
        << "#include \"common.h\"\n\n"
        << "#include <array>\n"
        << "#include <span>\n"
        << "#include <string_view>\n\n"
        << "export module " << module_name << ";\n\n"
        << "export namespace bioseq::sql::generated::"
           "vector_comparison_registry {\n\n"
        << "struct association final {\n"
        << "    std::string_view scalar_name;\n"
        << "    std::string_view vector_name;\n"
        << "    Oid scalar_namespace;\n"
        << "    Oid left_type;\n"
        << "    Oid right_type;\n"
        << "    Oid result_type;\n"
        << "};\n\n"
        << "inline constexpr std::array<association, "
        << associations.size() << "> associations{{\n";
    for (const vector_association& association : associations) {
        output
            << "    {\"" << association.scalar_name << "\", \""
            << association.vector_name
            << "\", PG_CATALOG_NAMESPACE, "
            << vector_type_oid(association.left_type) << ", "
            << vector_type_oid(association.right_type) << ", "
            << vector_type_oid(association.result_type) << "},\n";
    }
    output
        << "}};\n\n"
        << "[[nodiscard]] inline std::span<const association> all() "
           "noexcept {\n"
        << "    return associations;\n"
        << "}\n\n"
        << "}  // namespace bioseq::sql::generated::"
           "vector_comparison_registry\n";

    rendered_fmgr_entrypoint_module module{
        .owner_key = "vector_comparison_registry",
        .source_module = "generated.vector_comparison_registry",
        .module_name = module_name,
    };
    module.content = output.str();
    module.content_sha256 = sha256_hex(module.content);
    return module;
}

[[nodiscard]] inline std::optional<rendered_fmgr_entrypoint_module>
render_vector_comparison_registry(const sql_graph& graph) {
    std::vector<vector_association> associations;
    for (const normalized_entity& entity : graph.entities) {
        if (const std::optional<vector_association> association =
                vector_association_for(entity)) {
            associations.push_back(*association);
        }
    }
    if (associations.empty()) return std::nullopt;
    return render_vector_comparison_registry_from_associations(
        std::move(associations), graph.compiler_profile);
}

struct vector_aggregate_association final {
    std::string scalar_name;
    std::string vector_name;
    std::string input_type;
    std::string state_type;
    std::string result_type;
    std::string transition_name;
    std::string final_name;

    [[nodiscard]] bool operator==(
        const vector_aggregate_association&) const = default;
};

[[nodiscard]] inline std::string vector_aggregate_key(
        const vector_aggregate_association& association) {
    return association.vector_name + "(" + association.input_type + ")";
}

[[nodiscard]] inline const normalized_entity*
aggregate_routine_reference(
        const normalized_entity& aggregate,
        const sql_graph& graph,
        std::string_view option_name,
        bool required = false) {
    const option_value* const option =
        find_option(aggregate.source, option_name);
    if (option == nullptr) {
        if (required) {
            throw std::runtime_error(
                "vector aggregate '" + aggregate.source.cpp_qualified_name
                + "' requires ." + std::string{option_name});
        }
        return nullptr;
    }
    const normalized_entity* result = nullptr;
    for (const std::string& identity :
         aggregate.dependency_identities) {
        const auto candidate = std::ranges::find(
            graph.entities, identity,
            &normalized_entity::stable_identity);
        if (candidate == graph.entities.end()
            || candidate->source.kind != entity_kind::function) {
            continue;
        }
        const std::string leaf =
            cpp_leaf(candidate->source.cpp_qualified_name);
        if (!option->expression.contains(
                candidate->source.cpp_qualified_name)
            && !option->expression.contains(leaf)) {
            continue;
        }
        if (result != nullptr
            && result->stable_identity != candidate->stable_identity) {
            throw std::runtime_error(
                "vector aggregate '" + aggregate.source.cpp_qualified_name
                + "' has an ambiguous ." + std::string{option_name}
                + " reference");
        }
        result = &*candidate;
    }
    if (result == nullptr && required) {
        throw std::runtime_error(
            "vector aggregate '" + aggregate.source.cpp_qualified_name
            + "' has an unresolved ." + std::string{option_name}
            + " reference");
    }
    return result;
}

[[nodiscard]] inline std::optional<vector_aggregate_association>
vector_aggregate_association_for(
        const normalized_entity& entity,
        const sql_graph& graph) {
    if (entity.source.kind != entity_kind::aggregate
        || !entity.source.name.starts_with('v')) {
        return std::nullopt;
    }
    const std::string scalar_name = entity.source.name.substr(1);
    if (scalar_name != "count" && scalar_name != "sum"
        && scalar_name != "avg" && scalar_name != "max"
        && scalar_name != "min") {
        return std::nullopt;
    }
    const normalized_entity* const transition =
        aggregate_routine_reference(entity, graph, "transition", true);
    const std::vector<const routine_parameter*> parameters =
        sql_parameters(transition->source);
    if (parameters.empty() || parameters.size() > 2) {
        throw std::runtime_error(
            "vector aggregate transition '"
            + transition->source.cpp_qualified_name
            + "' must accept one state and at most one vector input");
    }
    const std::string state_type =
        canonical_sql_type(parameters.front()->sql_type);
    if (state_type
        != canonical_sql_type(transition->source.result_sql_type)) {
        throw std::runtime_error(
            "vector aggregate transition '"
            + transition->source.cpp_qualified_name
            + "' does not return its state type");
    }
    const normalized_entity* const final =
        aggregate_routine_reference(entity, graph, "final");
    return vector_aggregate_association{
        .scalar_name = scalar_name,
        .vector_name = entity.source.name,
        .input_type = parameters.size() == 1
            ? "*"
            : canonical_sql_type(parameters[1]->sql_type),
        .state_type = state_type,
        .result_type = final == nullptr
            ? state_type
            : canonical_sql_type(final->source.result_sql_type),
        .transition_name = transition->source.name,
        .final_name = final == nullptr
            ? std::string{}
            : final->source.name,
    };
}

[[nodiscard]] inline std::map<
    std::string,
    vector_aggregate_association,
    std::less<>>
expected_vector_aggregate_associations() {
    std::map<
        std::string,
        vector_aggregate_association,
        std::less<>>
        result;
    const auto add = [&result](
            std::string scalar_name,
            std::string vector_name,
            std::string input_type,
            std::string state_type,
            std::string result_type,
            std::string transition_name,
            std::string final_name = {}) {
        vector_aggregate_association association{
            .scalar_name = std::move(scalar_name),
            .vector_name = std::move(vector_name),
            .input_type = std::move(input_type),
            .state_type = std::move(state_type),
            .result_type = std::move(result_type),
            .transition_name = std::move(transition_name),
            .final_name = std::move(final_name),
        };
        result.emplace(
            vector_aggregate_key(association),
            std::move(association));
    };
    add("count", "vcount", "*", "bigint", "bigint", "vemptycount");
    add("count", "vcount", "any", "bigint", "bigint", "vanycount");
    add("sum", "vsum", "smallint", "bigint", "bigint", "vint2sum");
    add(
        "avg", "vavg", "smallint", "internal", "numeric",
        "vint2acc", "vint2int4avg");
    add(
        "max", "vmax", "smallint", "smallint", "smallint",
        "vint2larger");
    add(
        "min", "vmin", "smallint", "smallint", "smallint",
        "vint2smaller");
    add("sum", "vsum", "integer", "bigint", "bigint", "vint4sum");
    add(
        "avg", "vavg", "integer", "internal", "numeric",
        "vint4acc", "vint2int4avg");
    add(
        "max", "vmax", "integer", "integer", "integer",
        "vint4larger");
    add(
        "min", "vmin", "integer", "integer", "integer",
        "vint4smaller");
    add(
        "sum", "vsum", "bigint", "internal", "numeric",
        "vint8acc", "vint8sum");
    add(
        "avg", "vavg", "bigint", "internal", "numeric",
        "vint8acc", "vint8avg");
    add(
        "max", "vmax", "bigint", "bigint", "bigint",
        "vint8larger");
    add(
        "min", "vmin", "bigint", "bigint", "bigint",
        "vint8smaller");
    add(
        "max", "vmax", "date", "date", "date",
        "vdatelarger");
    add(
        "min", "vmin", "date", "date", "date",
        "vdatesmaller");
    return result;
}

inline void validate_vector_aggregate_associations(
        std::vector<vector_aggregate_association> associations) {
    const auto expected = expected_vector_aggregate_associations();
    if (associations.size() != expected.size()) {
        throw std::runtime_error(
            "generated vector aggregate registry requires exactly "
            + std::to_string(expected.size()) + " typed owners, found "
            + std::to_string(associations.size()));
    }
    std::ranges::sort(
        associations,
        [](const vector_aggregate_association& left,
           const vector_aggregate_association& right) {
            return vector_aggregate_key(left)
                < vector_aggregate_key(right);
        });
    for (std::size_t index = 0; index < associations.size(); ++index) {
        const std::string key =
            vector_aggregate_key(associations[index]);
        if (index != 0
            && key == vector_aggregate_key(associations[index - 1])) {
            throw std::runtime_error(
                "generated vector aggregate registry contains duplicate "
                "association '" + key + "'");
        }
        const auto expected_row = expected.find(key);
        if (expected_row == expected.end()
            || associations[index] != expected_row->second) {
            throw std::runtime_error(
                "generated vector aggregate registry contains an "
                "unexpected association for '" + key + "'");
        }
    }
}

[[nodiscard]] inline std::string vector_aggregate_type_oid(
        std::string_view sql_type) {
    if (sql_type == "*") return "InvalidOid";
    if (sql_type == "any") return "ANYOID";
    if (sql_type == "smallint") return "INT2OID";
    if (sql_type == "integer") return "INT4OID";
    if (sql_type == "bigint") return "INT8OID";
    if (sql_type == "date") return "DATEOID";
    if (sql_type == "numeric") return "NUMERICOID";
    if (sql_type == "internal") return "INTERNALOID";
    throw std::runtime_error(
        "vector aggregate registry cannot render SQL type '"
        + std::string{sql_type} + "'");
}

[[nodiscard]] inline rendered_fmgr_entrypoint_module
render_vector_aggregate_registry_from_associations(
        std::vector<vector_aggregate_association> associations,
        std::string_view compiler_profile) {
    validate_vector_aggregate_associations(associations);
    std::ranges::sort(
        associations,
        [](const vector_aggregate_association& left,
           const vector_aggregate_association& right) {
            return vector_aggregate_key(left)
                < vector_aggregate_key(right);
        });
    const std::string module_name = vector_registry_module_name(
        compiler_profile, "vector_aggregate_registry");

    std::ostringstream output;
    output
        << "module;\n\n"
        << "#include \"common.h\"\n\n"
        << "#include <array>\n"
        << "#include <span>\n"
        << "#include <string_view>\n\n"
        << "export module " << module_name << ";\n\n"
        << "export namespace bioseq::sql::generated::"
           "vector_aggregate_registry {\n\n"
        << "struct association final {\n"
        << "    std::string_view scalar_name;\n"
        << "    std::string_view vector_name;\n"
        << "    std::string_view transition_name;\n"
        << "    std::string_view final_name;\n"
        << "    Oid scalar_namespace;\n"
        << "    Oid input_type;\n"
        << "    Oid state_type;\n"
        << "    Oid result_type;\n"
        << "};\n\n"
        << "inline constexpr std::array<association, "
        << associations.size() << "> associations{{\n";
    for (const vector_aggregate_association& association :
         associations) {
        output
            << "    {\"" << association.scalar_name << "\", \""
            << association.vector_name << "\", \""
            << association.transition_name << "\", \""
            << association.final_name
            << "\", PG_CATALOG_NAMESPACE, "
            << vector_aggregate_type_oid(association.input_type) << ", "
            << vector_aggregate_type_oid(association.state_type) << ", "
            << vector_aggregate_type_oid(association.result_type)
            << "},\n";
    }
    output
        << "}};\n\n"
        << "[[nodiscard]] inline std::span<const association> all() "
           "noexcept {\n"
        << "    return associations;\n"
        << "}\n\n"
        << "}  // namespace bioseq::sql::generated::"
           "vector_aggregate_registry\n";

    rendered_fmgr_entrypoint_module module{
        .owner_key = "vector_aggregate_registry",
        .source_module = "generated.vector_aggregate_registry",
        .module_name = module_name,
    };
    module.content = output.str();
    module.content_sha256 = sha256_hex(module.content);
    return module;
}

[[nodiscard]] inline std::optional<rendered_fmgr_entrypoint_module>
render_vector_aggregate_registry(const sql_graph& graph) {
    std::vector<vector_aggregate_association> associations;
    for (const normalized_entity& entity : graph.entities) {
        if (const auto association =
                vector_aggregate_association_for(entity, graph)) {
            associations.push_back(*association);
        }
    }
    if (associations.empty()) return std::nullopt;
    return render_vector_aggregate_registry_from_associations(
        std::move(associations), graph.compiler_profile);
}

}  // namespace fmgr_detail

/**
 * Render the closed vector-registry interfaces before FINAL owners import
 * them. FINAL generation independently projects the annotated graph and
 * requires byte equality with these SUPPORT-owned artifacts.
 */
[[nodiscard]] inline std::vector<rendered_fmgr_entrypoint_module>
render_support_vector_registry_modules(
        std::string_view compiler_profile) {
    std::vector<fmgr_detail::vector_association>
        comparison_associations;
    for (const auto& [vector_name, argument_types] :
         fmgr_detail::expected_vector_associations()) {
        comparison_associations.push_back(
            fmgr_detail::vector_association{
                .scalar_name = vector_name.substr(1),
                .vector_name = vector_name,
                .left_type = argument_types.first,
                .right_type = argument_types.second,
                .result_type = "boolean",
            });
    }
    std::vector<fmgr_detail::vector_aggregate_association>
        aggregate_associations;
    for (const auto& [key, association] :
         fmgr_detail::expected_vector_aggregate_associations()) {
        static_cast<void>(key);
        aggregate_associations.push_back(association);
    }
    std::vector<rendered_fmgr_entrypoint_module> result;
    result.push_back(
        fmgr_detail::render_vector_comparison_registry_from_associations(
            std::move(comparison_associations), compiler_profile));
    result.push_back(
        fmgr_detail::render_vector_aggregate_registry_from_associations(
            std::move(aggregate_associations), compiler_profile));
    return result;
}

/**
 * Render one configure-known primary module per natural C++ SQL owner module.
 */
[[nodiscard]] inline std::vector<rendered_fmgr_entrypoint_module>
render_fmgr_entrypoint_modules(const sql_graph& graph) {
    using symbol_group = std::map<
        std::string,
        std::vector<const normalized_entity*>,
        std::less<>>;
    std::map<std::string, symbol_group, std::less<>> by_owner;
    std::map<
        std::string,
        std::set<std::string, std::less<>>,
        std::less<>>
        sources_by_owner;
    for (const normalized_entity& entity : graph.entities) {
        if (!fmgr_detail::generated_c_routine(entity)) continue;
        const std::string owner_key =
            fmgr_detail::fmgr_owner_key(
                entity.source.source_module);
        by_owner[owner_key]
            [entity.source.name].push_back(&entity);
        sources_by_owner[owner_key].insert(
            entity.source.source_module);
    }

    std::vector<rendered_fmgr_entrypoint_module> result;
    for (auto& [owner_key, symbols] : by_owner) {
        const auto sources = sources_by_owner.find(owner_key);
        if (sources == sources_by_owner.end()
            || sources->second.empty()) {
            throw std::runtime_error(
                "generated fmgr owner '" + owner_key
                + "' has no source modules");
        }
        const bool compact_sequence_owner =
            owner_key == "types_pg_compact_sequence";
        const bool table_entrypoint_owner =
            owner_key == "table_access_hooks";
        if (sources->second.size() != 1
            && !compact_sequence_owner
            && !table_entrypoint_owner) {
            throw std::runtime_error(
                "generated fmgr owner key collision for '"
                + owner_key + "'");
        }
        const std::string& source_module =
            *sources->second.begin();
        for (auto& [symbol, entities] : symbols) {
            std::ranges::sort(
                entities,
                [](const normalized_entity* left,
                   const normalized_entity* right) {
                    const std::size_t left_arity =
                        fmgr_detail::sql_arity(*left);
                    const std::size_t right_arity =
                        fmgr_detail::sql_arity(*right);
                    if (left_arity != right_arity) {
                        return left_arity < right_arity;
                    }
                    return left->installed_identity
                        < right->installed_identity;
                });
            const std::string owner_scope =
                fmgr_detail::cpp_owner_scope(
                    entities.front()->source.cpp_qualified_name);
            const std::string schema =
                entities.front()->source.schema;
            const std::string symbol_source_module =
                entities.front()->source.source_module;
            for (const normalized_entity* entity : entities) {
                if ((!compact_sequence_owner
                     && entity->source.source_module
                        != symbol_source_module)
                    || entity->source.schema != schema
                    || fmgr_detail::cpp_owner_scope(
                           entity->source.cpp_qualified_name)
                       != owner_scope) {
                    throw std::runtime_error(
                        "generated fmgr overload group '" + symbol
                        + "' crosses an owner module, schema, or C++ scope");
                }
            }
        }
        rendered_fmgr_entrypoint_module module{
            .owner_key = owner_key,
            .source_module = source_module,
            .module_name =
                fmgr_detail::fmgr_module_name(
                    source_module, owner_key),
        };
        module.content =
            fmgr_detail::render_module(module, symbols, graph);
        module.content_sha256 = sha256_hex(module.content);
        result.push_back(std::move(module));
    }
    if (std::optional<rendered_fmgr_entrypoint_module> registry =
            fmgr_detail::render_vector_comparison_registry(graph)) {
        result.push_back(std::move(*registry));
    }
    if (std::optional<rendered_fmgr_entrypoint_module> registry =
            fmgr_detail::render_vector_aggregate_registry(graph)) {
        result.push_back(std::move(*registry));
    }
    return result;
}

/**
 * Flatten the symbols actually present in emitted wrapper artifacts.
 */
[[nodiscard]] inline std::vector<std::string>
emitted_fmgr_wrapper_symbols(
        const std::vector<rendered_fmgr_entrypoint_module>& modules) {
    std::vector<std::string> result;
    for (const rendered_fmgr_entrypoint_module& module : modules) {
        for (const rendered_fmgr_wrapper& wrapper : module.wrappers) {
            result.push_back(wrapper.public_symbol);
        }
    }
    std::ranges::sort(result);
    const auto duplicate = std::ranges::adjacent_find(result);
    if (duplicate != result.end()) {
        throw std::runtime_error(
            "generated fmgr public symbol '" + *duplicate
            + "' is emitted by more than one artifact");
    }
    return result;
}

}  // namespace bioseq::codegen::sql
