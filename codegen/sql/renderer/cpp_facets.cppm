module;

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:cpp_facets;

import :model;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

namespace detail {

[[nodiscard]] inline std::string entity_handle_module_name(
    std::string_view owner_key) {
    return "bioseq.postgres.sql.generated.core." + std::string{owner_key}
        + ".entity_handles";
}

[[nodiscard]] inline std::string entity_handle_cpp_namespace(
    std::string_view owner_key) {
    return "bioseq::sql::generated::core::" + std::string{owner_key}
        + "::entity_handles";
}

[[nodiscard]] inline std::string render_entity_handle_module(
    const rendered_entity_handle_module& module) {
    std::ostringstream output;
    output
        << "module;\n\n"
        << "extern \"C\" {\n"
        << "#include \"catalog/pg_class_d.h\"\n"
        << "}\n\n"
        << "export module " << module.module_name << ";\n\n"
        << "export import bioseq.postgres.support.sql_catalog_entities;\n\n"
        << "export namespace " << module.cpp_namespace << " {\n\n";
    for (const rendered_entity_handle& handle : module.handles) {
        output
            << "inline constexpr bioseq::postgres::support::relation_entity_handle<\n"
            << "    " << handle.expected_relkind << ",\n"
            << "    bioseq::postgres::support::fixed_catalog_name{\""
            << handle.leaf_name << "\"},\n"
            << "    bioseq::postgres::support::sql_schema_binding::"
            << handle.schema_binding << ",\n"
            << "    bioseq::postgres::support::fixed_catalog_name{\""
            << (handle.schema_binding == "fixed"
                    ? handle.schema_name
                    : "bioseq")
            << "\"}>\n"
            << "    " << handle.variable_name << "{};\n\n";
    }
    output << "} // namespace " << module.cpp_namespace << "\n";
    return output.str();
}

[[nodiscard]] inline std::string row_codec_module_name(
    std::string_view source_module) {
    constexpr std::string_view suffix = ".sql_rows";
    source_module.remove_suffix(suffix.size());
    return std::string{source_module} + ".sql_row_codecs";
}

[[nodiscard]] inline std::string cpp_namespace_from_module(
    std::string_view module_name) {
    std::string result;
    result.reserve(module_name.size() + 8);
    for (const char value : module_name) {
        if (value == '.') {
            result += "::";
        } else {
            result.push_back(value);
        }
    }
    return result;
}

struct row_codec_field {
    enum class storage_kind {
        scalar,
        oid,
        timestamp_tz,
        xid8,
        name,
        bytea_pointer,
        text_pointer,
        text_string,
    };

    std::string cpp_name;
    std::string sql_name;
    std::string cpp_type;
    std::string underlying_cpp_type;
    std::string type_oid;
    std::array<std::string, 5> comparison_procedures;
    std::size_t attribute_number{};
    bool nullable{};
    storage_kind storage{storage_kind::scalar};
};

[[nodiscard]] constexpr bool is_by_reference_storage(
        row_codec_field::storage_kind storage) noexcept {
    return storage == row_codec_field::storage_kind::name
        || storage == row_codec_field::storage_kind::bytea_pointer
        || storage == row_codec_field::storage_kind::text_pointer
        || storage == row_codec_field::storage_kind::text_string;
}

[[nodiscard]] inline std::string_view datum_copy_length_expression(
        row_codec_field::storage_kind storage) {
    switch (storage) {
        case row_codec_field::storage_kind::name:
            return "static_cast<int16>(NAMEDATALEN)";
        case row_codec_field::storage_kind::bytea_pointer:
        case row_codec_field::storage_kind::text_pointer:
        case row_codec_field::storage_kind::text_string:
            return "static_cast<int16>(-1)";
        default:
            throw std::runtime_error(
                "generated Datum copy policy requested for a by-value field");
    }
}

struct normalized_row_cpp_type {
    std::string type;
    bool nullable{};
};

[[nodiscard]] inline normalized_row_cpp_type normalized_cpp_field_type(
    std::string_view cpp_type) {
    std::string compact;
    compact.reserve(cpp_type.size());
    for (const unsigned char value : cpp_type) {
        if (!std::isspace(value)) {
            compact.push_back(static_cast<char>(value));
        }
    }
    bool nullable = false;
    constexpr std::string_view optional_prefix{"std::optional<"};
    if (compact.starts_with(optional_prefix)
        && compact.ends_with(">")) {
        nullable = true;
        compact = compact.substr(
            optional_prefix.size(),
            compact.size() - optional_prefix.size() - 1);
    }
    if (compact == "std::int16_t" || compact == "int16_t"
        || compact == "int16") {
        return {.type = "std::int16_t", .nullable = nullable};
    }
    if (compact == "std::int32_t" || compact == "int32_t"
        || compact == "int32" || compact == "int") {
        return {.type = "std::int32_t", .nullable = nullable};
    }
    if (compact == "std::int64_t" || compact == "int64_t"
        || compact == "int64" || compact == "long"
        || compact == "longlong") {
        return {.type = "std::int64_t", .nullable = nullable};
    }
    if (compact == "bool") {
        return {.type = "bool", .nullable = nullable};
    }
    if (compact == "Oid" || compact == "::Oid") {
        return {.type = "Oid", .nullable = nullable};
    }
    if (compact == "TimestampTz" || compact == "::TimestampTz") {
        return {.type = "TimestampTz", .nullable = nullable};
    }
    if (compact == "FullTransactionId"
        || compact == "::FullTransactionId") {
        return {.type = "FullTransactionId", .nullable = nullable};
    }
    if (compact == "NameData" || compact == "::NameData") {
        return {.type = "NameData", .nullable = nullable};
    }
    if (compact == "bytea*" || compact == "::bytea*") {
        return {.type = "bytea*", .nullable = nullable};
    }
    if (compact == "text*" || compact == "::text*") {
        return {
            .type = "text*",
            .nullable = nullable,
        };
    }
    if (compact == "std::string") {
        return {
            .type = "std::string",
            .nullable = nullable,
        };
    }
    if (compact == "double") {
        return {.type = "double", .nullable = nullable};
    }
    throw std::runtime_error(
        "generated row codec does not support C++ field type '"
        + std::string{cpp_type} + "'");
}

[[nodiscard]] inline std::string row_column_sql_type(
    const collected_entity& column) {
    for (const option_value& option : column.options) {
        if (option.name != "sql_type") continue;
        std::string_view expression = option.expression;
        while (!expression.empty()
               && (expression.back() == '}' || expression.back() == ')'
                   || std::isspace(
                       static_cast<unsigned char>(expression.back())))) {
            expression.remove_suffix(1);
        }
        const std::size_t separator = expression.rfind("::");
        return std::string{
            separator == std::string_view::npos
                ? expression
                : expression.substr(separator + 2)};
    }
    return {};
}

[[nodiscard]] inline row_codec_field make_row_codec_field(
    const collected_entity& column,
    std::size_t attribute_number) {
    const normalized_row_cpp_type cpp_type =
        normalized_cpp_field_type(column.cpp_return_type);
    const std::size_t separator =
        column.cpp_qualified_name.rfind("::");
    const std::string cpp_name =
        separator == std::string::npos
        ? column.cpp_qualified_name
        : column.cpp_qualified_name.substr(separator + 2);
    if (cpp_name.empty()) {
        throw std::runtime_error(
            "generated row codec field has no C++ member name for '"
            + column.cpp_qualified_name + "'");
    }
    row_codec_field result{
        .cpp_name = cpp_name,
        .sql_name = column.name,
        .cpp_type = cpp_type.nullable
            ? "std::optional<" + cpp_type.type + ">"
            : cpp_type.type,
        .underlying_cpp_type = cpp_type.type,
        .attribute_number = attribute_number,
        .nullable = cpp_type.nullable,
    };
    if (result.underlying_cpp_type == "std::int16_t") {
        result.type_oid = "INT2OID";
        result.comparison_procedures = {
            "F_INT2EQ", "F_INT2LT", "F_INT2LE", "F_INT2GT", "F_INT2GE"};
    } else if (result.underlying_cpp_type == "std::int32_t") {
        result.type_oid = "INT4OID";
        result.comparison_procedures = {
            "F_INT4EQ", "F_INT4LT", "F_INT4LE", "F_INT4GT", "F_INT4GE"};
    } else if (result.underlying_cpp_type == "std::int64_t") {
        result.type_oid = "INT8OID";
        result.comparison_procedures = {
            "F_INT8EQ", "F_INT8LT", "F_INT8LE", "F_INT8GT", "F_INT8GE"};
    } else if (result.underlying_cpp_type == "bool") {
        result.type_oid = "BOOLOID";
        result.comparison_procedures = {
            "F_BOOLEQ", "F_BOOLLT", "F_BOOLLE", "F_BOOLGT", "F_BOOLGE"};
    } else if (result.underlying_cpp_type == "double") {
        result.type_oid = "FLOAT8OID";
        result.comparison_procedures = {
            "F_FLOAT8EQ",
            "F_FLOAT8LT",
            "F_FLOAT8LE",
            "F_FLOAT8GT",
            "F_FLOAT8GE"};
    } else if (result.underlying_cpp_type == "Oid") {
        result.storage = row_codec_field::storage_kind::oid;
        const std::string sql_type = row_column_sql_type(column);
        if (sql_type.empty() || sql_type == "oid") {
            result.type_oid = "OIDOID";
        } else if (sql_type == "regclass") {
            result.type_oid = "REGCLASSOID";
        } else {
            throw std::runtime_error(
                "generated row codec Oid field '"
                + column.cpp_qualified_name
                + "' has unsupported SQL type '" + sql_type + "'");
        }
        result.comparison_procedures = {
            "F_OIDEQ", "F_OIDLT", "F_OIDLE", "F_OIDGT", "F_OIDGE"};
    } else if (result.underlying_cpp_type == "TimestampTz") {
        result.storage =
            row_codec_field::storage_kind::timestamp_tz;
        result.type_oid = "TIMESTAMPTZOID";
        result.comparison_procedures = {
            "F_TIMESTAMPTZ_EQ",
            "F_TIMESTAMPTZ_LT",
            "F_TIMESTAMPTZ_LE",
            "F_TIMESTAMPTZ_GT",
            "F_TIMESTAMPTZ_GE"};
    } else if (result.underlying_cpp_type == "FullTransactionId") {
        result.storage = row_codec_field::storage_kind::xid8;
        result.type_oid = "XID8OID";
        result.comparison_procedures = {
            "F_XID8EQ", "F_XID8LT", "F_XID8LE", "F_XID8GT", "F_XID8GE"};
    } else if (result.underlying_cpp_type == "NameData") {
        if (const std::string sql_type = row_column_sql_type(column);
            !sql_type.empty() && sql_type != "name") {
            throw std::runtime_error(
                "generated row codec NameData field '"
                + column.cpp_qualified_name
                + "' has unsupported SQL type '" + sql_type + "'");
        }
        result.storage = row_codec_field::storage_kind::name;
        result.type_oid = "NAMEOID";
        result.comparison_procedures = {
            "F_NAMEEQ", "F_NAMELT", "F_NAMELE", "F_NAMEGT", "F_NAMEGE"};
    } else if (result.underlying_cpp_type == "bytea*") {
        if (const std::string sql_type = row_column_sql_type(column);
            !sql_type.empty() && sql_type != "bytea") {
            throw std::runtime_error(
                "generated row codec bytea field '"
                + column.cpp_qualified_name
                + "' has unsupported SQL type '" + sql_type + "'");
        }
        result.storage =
            row_codec_field::storage_kind::bytea_pointer;
        result.type_oid = "BYTEAOID";
        result.comparison_procedures = {
            "F_BYTEAEQ",
            "F_BYTEALT",
            "F_BYTEALE",
            "F_BYTEAGT",
            "F_BYTEAGE"};
    } else if (result.underlying_cpp_type == "text*") {
        if (const std::string sql_type = row_column_sql_type(column);
            !sql_type.empty() && sql_type != "text") {
            throw std::runtime_error(
                "generated row codec text field '"
                + column.cpp_qualified_name
                + "' has unsupported SQL type '" + sql_type + "'");
        }
        result.storage =
            row_codec_field::storage_kind::text_pointer;
        result.type_oid = "TEXTOID";
        result.comparison_procedures = {
            "F_TEXTEQ",
            "F_TEXT_LT",
            "F_TEXT_LE",
            "F_TEXT_GT",
            "F_TEXT_GE"};
    } else if (result.underlying_cpp_type == "std::string") {
        if (const std::string sql_type = row_column_sql_type(column);
            !sql_type.empty() && sql_type != "text") {
            throw std::runtime_error(
                "generated row codec std::string field '"
                + column.cpp_qualified_name
                + "' has unsupported SQL type '" + sql_type + "'");
        }
        result.storage =
            row_codec_field::storage_kind::text_string;
        result.type_oid = "TEXTOID";
        result.comparison_procedures = {
            "F_TEXTEQ",
            "F_TEXT_LT",
            "F_TEXT_LE",
            "F_TEXT_GT",
            "F_TEXT_GE"};
    }
    if (result.type_oid.empty()
        || std::ranges::any_of(
            result.comparison_procedures,
            [](const std::string& procedure) {
                return procedure.empty();
            })) {
        throw std::runtime_error(
            "generated row codec has no PostgreSQL registry entry for C++ "
            "field type '" + column.cpp_return_type + "' on '"
            + column.cpp_qualified_name + "'");
    }
    return result;
}

[[nodiscard]] inline std::size_t count_text_occurrences(
        std::string_view text,
        std::string_view needle) noexcept {
    if (needle.empty()) return 0;
    std::size_t count = 0;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset))
           != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

/**
 * Fail closed when a generated owning-text codec bypasses the audited
 * PostgreSQL ERROR bridges or drops one of its ownership conversions.
 */
inline void validate_row_codec_text_lifetime_contract(
        std::string_view rendered,
        std::size_t text_field_count) {
    if (text_field_count == 0) return;

    if (rendered.contains("cstring_to_text_with_len(")
        || rendered.contains("DatumGetTextPP(")) {
        throw std::runtime_error(
            "generated row codec text ownership contract contains a "
            "direct PostgreSQL text leaf");
    }

    constexpr std::string_view encode_bridge{
        "bioseq::postgres::support::encode_text_to_memory_context("};
    constexpr std::string_view deform_bridge{
        "bioseq::postgres::support::decode_text("};
    const std::size_t expected_encode_bridges =
        text_field_count + 1;
    if (count_text_occurrences(rendered, encode_bridge)
            != expected_encode_bridges
        || count_text_occurrences(rendered, deform_bridge)
            != text_field_count) {
        throw std::runtime_error(
            "generated row codec text ownership contract has an "
            "incomplete encode/deform bridge projection");
    }
}

/// Renders one strongly typed PostgreSQL row codec from normalized fields.
[[nodiscard]] inline std::string render_row_codec(
    const normalized_entity& row,
    std::vector<const normalized_entity*> columns) {
    std::ranges::sort(
        columns,
        {},
        [](const normalized_entity* column) {
            return column->source.source_range.begin.byte_offset;
        });
    if (columns.empty()) {
        throw std::runtime_error(
            "natural SQL row '" + row.source.cpp_qualified_name
            + "' has no collected fields");
    }

    std::vector<row_codec_field> fields;
    fields.reserve(columns.size());
    for (std::size_t index = 0; index < columns.size(); ++index) {
        fields.push_back(make_row_codec_field(
            columns[index]->source, index + 1));
    }
    const bool has_by_reference_fields = std::ranges::any_of(
        fields,
        [](const row_codec_field& field) {
            return is_by_reference_storage(field.storage);
        });
    const std::size_t text_field_count =
        static_cast<std::size_t>(std::ranges::count_if(
            fields,
            [](const row_codec_field& field) {
                return field.storage
                    == row_codec_field::storage_kind::text_string;
            }));

    const std::string codec_name = row.source.name + "_codec";
    const std::string trait_name = row.source.name + "_field";
    const std::string row_type = "::" + row.source.cpp_qualified_name;
    std::ostringstream output;
    output
        << "namespace detail {\n\n"
        << "template<auto p_member>\n"
        << "struct " << trait_name << ";\n\n";
    for (const row_codec_field& field : fields) {
        output
            << "template<>\n"
            << "struct " << trait_name << "<&" << row_type
            << "::" << field.cpp_name << "> final {\n"
            << "    using value_type = " << field.cpp_type << ";\n"
            << "    static constexpr AttrNumber attribute_number = "
            << field.attribute_number << ";\n"
            << "    static constexpr Oid type_oid = "
            << field.type_oid << ";\n"
            << "    [[nodiscard]] static consteval auto "
               "comparison_procedure(\n"
            << "            bioseq::postgres::support::scan_comparison "
               "comparison) -> RegProcedure {\n"
            << "        switch (comparison) {\n"
            << "            case "
               "bioseq::postgres::support::scan_comparison::equal:\n"
            << "                return "
            << field.comparison_procedures[0] << ";\n"
            << "            case "
               "bioseq::postgres::support::scan_comparison::less:\n"
            << "                return "
            << field.comparison_procedures[1] << ";\n"
            << "            case "
               "bioseq::postgres::support::scan_comparison::less_equal:\n"
            << "                return "
            << field.comparison_procedures[2] << ";\n"
            << "            case "
               "bioseq::postgres::support::scan_comparison::greater:\n"
            << "                return "
            << field.comparison_procedures[3] << ";\n"
            << "            case "
               "bioseq::postgres::support::scan_comparison::greater_equal:\n"
            << "                return "
            << field.comparison_procedures[4] << ";\n"
            << "        }\n"
            << "        return InvalidOid;\n"
            << "    }\n"
            << "    static constexpr bool nullable = "
            << (field.nullable ? "true" : "false") << ";\n"
            << "    static constexpr bool oid_value = "
            << (field.storage == row_codec_field::storage_kind::oid
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool timestamp_tz_value = "
            << (field.storage
                        == row_codec_field::storage_kind::timestamp_tz
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool xid8_value = "
            << (field.storage == row_codec_field::storage_kind::xid8
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool name_value = "
            << (field.storage == row_codec_field::storage_kind::name
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool bytea_pointer = "
            << (field.storage
                        == row_codec_field::storage_kind::bytea_pointer
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool text_pointer = "
            << (field.storage
                        == row_codec_field::storage_kind::text_pointer
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool text_string = "
            << (field.storage
                        == row_codec_field::storage_kind::text_string
                    ? "true"
                    : "false")
            << ";\n"
            << "    static constexpr bool by_reference = "
            << (is_by_reference_storage(field.storage)
                    ? "true"
                    : "false")
            << ";\n";
        if (is_by_reference_storage(field.storage)) {
            output
                << "    static inline constexpr "
                   "bioseq::postgres::support::by_reference_datum_copy_policy "
                   "datum_copy_policy{\n"
                << "        .type_length = "
                << datum_copy_length_expression(field.storage) << ",\n"
                << "        .type_by_value = false,\n"
                << "    };\n";
        }
        output << "};\n\n";
    }
    output
        << "} // namespace detail\n\n"
        << "struct " << codec_name << " final {\n"
        << "    using row_type = " << row_type << ";\n"
        << "    static constexpr std::size_t field_count = "
        << fields.size() << ";\n"
        << "    static constexpr std::string_view row_name = \""
        << row.source.schema << "." << row.source.name << "\";\n\n"
        << "    static inline constexpr std::array<\n"
        << "        bioseq::postgres::support::row_descriptor_field,\n"
        << "        field_count> expected_descriptor{{\n";
    for (const row_codec_field& field : fields) {
        output
            << "        {.attribute_number = " << field.attribute_number
            << ", .name = \"" << field.sql_name
            << "\", .type_oid = " << field.type_oid
            << ", .nullable = "
            << (field.nullable ? "true" : "false") << "},\n";
    }
    output
        << "    }};\n\n"
        << "    template<auto p_member>\n"
        << "    using field = detail::" << trait_name << "<p_member>;\n\n"
        << "    template<auto p_member>\n"
        << "    using member_type = typename field<p_member>::value_type;\n\n"
        << "    template<auto p_member>\n"
        << "    [[nodiscard]] static consteval auto "
           "attribute_number() noexcept -> AttrNumber {\n"
        << "        return field<p_member>::attribute_number;\n"
        << "    }\n\n"
        << "    static auto validate_descriptor(TupleDesc descriptor) "
           "-> void {\n"
        << "        bioseq::postgres::support::validate_row_descriptor(\n"
        << "            descriptor, row_name, expected_descriptor);\n"
        << "    }\n\n"
        << "    struct encoded_tuple_data final {\n"
        << "        Datum* values;\n"
        << "        bool* nulls;\n"
        << "    };\n\n"
        << "    struct const_encoded_tuple_data final {\n"
        << "        const Datum* values;\n"
        << "        const bool* nulls;\n"
        << "    };\n\n"
        << "    class encoded_row final {\n"
        << "    public:\n"
        << "        encoded_row(const encoded_row&) = delete;\n"
        << "        auto operator=(const encoded_row&) -> encoded_row& = delete;\n"
        << "        encoded_row(encoded_row&&) noexcept = default;\n"
        << "        auto operator=(encoded_row&&) noexcept -> encoded_row& = "
           "default;\n\n"
        << "        [[nodiscard]] auto tuple_data(\n"
        << "                TupleDesc descriptor) -> encoded_tuple_data {\n"
        << "            validate_descriptor(descriptor);\n"
        << "            return {\n"
        << "                .values = data_.values_data(),\n"
        << "                .nulls = data_.nulls_data(),\n"
        << "            };\n"
        << "        }\n\n"
        << "        [[nodiscard]] auto tuple_data(\n"
        << "                TupleDesc descriptor) const "
           "-> const_encoded_tuple_data {\n"
        << "            validate_descriptor(descriptor);\n"
        << "            return {\n"
        << "                .values = data_.values_data(),\n"
        << "                .nulls = data_.nulls_data(),\n"
        << "            };\n"
        << "        }\n\n"
        << "        [[nodiscard]] auto form_tuple(\n"
        << "                TupleDesc descriptor) -> HeapTuple {\n"
        << "            return data_.form_tuple(\n"
        << "                descriptor, row_name, expected_descriptor);\n"
        << "        }\n\n"
        << "    private:\n"
        << "        friend struct " << codec_name << ";\n\n"
        << "        explicit encoded_row(const row_type& row";
    if (has_by_reference_fields) {
        output << ", MemoryContext value_context";
    }
    output << ") {\n";
    if (has_by_reference_fields) {
        output
            << "            bioseq::postgres::support::require_row_result_context(\n"
            << "                value_context, row_name);\n";
    }
    for (const row_codec_field& field : fields) {
        switch (field.storage) {
            case row_codec_field::storage_kind::scalar:
                if (field.nullable) {
                    output
                        << "            if (row." << field.cpp_name
                        << ".has_value()) {\n"
                        << "                data_.set("
                        << field.attribute_number << ", *row."
                        << field.cpp_name << ");\n"
                        << "            } else {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            }\n";
                } else {
                    output
                        << "            data_.set("
                        << field.attribute_number << ", row."
                        << field.cpp_name << ");\n";
                }
                break;
            case row_codec_field::storage_kind::oid:
                if (field.nullable) {
                    output
                        << "            if (row." << field.cpp_name
                        << ".has_value()) {\n"
                        << "                data_.set_raw("
                        << field.attribute_number
                        << ", bioseq::postgres::support::pg_datum::from_oid(*row."
                        << field.cpp_name << "));\n"
                        << "            } else {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            }\n";
                } else {
                    output
                        << "            data_.set_raw("
                        << field.attribute_number
                        << ", bioseq::postgres::support::pg_datum::from_oid(row."
                        << field.cpp_name << "));\n";
                }
                break;
            case row_codec_field::storage_kind::timestamp_tz:
                if (field.nullable) {
                    output
                        << "            if (row." << field.cpp_name
                        << ".has_value()) {\n"
                        << "                data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                    TimestampTzGetDatum(*row."
                        << field.cpp_name << ")));\n"
                        << "            } else {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            }\n";
                } else {
                    output
                        << "            data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                TimestampTzGetDatum(row."
                        << field.cpp_name << ")));\n";
                }
                break;
            case row_codec_field::storage_kind::xid8:
                if (field.nullable) {
                    output
                        << "            if (row." << field.cpp_name
                        << ".has_value()) {\n"
                        << "                data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                    FullTransactionIdGetDatum(*row."
                        << field.cpp_name << ")));\n"
                        << "            } else {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            }\n";
                } else {
                    output
                        << "            data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                FullTransactionIdGetDatum(row."
                        << field.cpp_name << ")));\n";
                }
                break;
            case row_codec_field::storage_kind::name:
                if (field.nullable) {
                    output
                        << "            if (row." << field.cpp_name
                        << ".has_value()) {\n"
                        << "                data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                    bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context(\n"
                        << "                        "
                           "bioseq::postgres::support::pg_datum::from_name(\n"
                        << "                            const_cast<Name>(&*row."
                        << field.cpp_name << ")).raw(),\n"
                        << "                        field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                        value_context)));\n"
                        << "            } else {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            }\n";
                } else {
                    output
                        << "            data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                "
                        << "bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context(\n"
                        << "                    "
                        << "bioseq::postgres::support::pg_datum::from_name(\n"
                        << "                        const_cast<Name>(&row."
                        << field.cpp_name << ")).raw(),\n"
                        << "                    field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                    value_context)));\n";
                }
                break;
            case row_codec_field::storage_kind::bytea_pointer:
            case row_codec_field::storage_kind::text_pointer:
                if (field.nullable) {
                    output
                        << "            if (!row." << field.cpp_name
                        << ".has_value() || *row." << field.cpp_name
                        << " == nullptr) {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            } else {\n"
                        << "                data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                    "
                        << "bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context(\n"
                        << "                        "
                        << "PointerGetDatum(*row." << field.cpp_name
                        << "),\n"
                        << "                        field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                        value_context)));\n"
                        << "            }\n";
                } else {
                    output
                        << "            if (row." << field.cpp_name
                        << " == nullptr) {\n"
                        << "                throw "
                           "bioseq::postgres::support::row_codec_error{\n"
                        << "                    std::string{row_name} + "
                           "\" has a null C++ pointer for "
                        << field.cpp_name << "\"};\n"
                        << "            }\n"
                        << "            data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                "
                        << "bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context(\n"
                        << "                    PointerGetDatum(row."
                        << field.cpp_name << "),\n"
                        << "                    field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                    value_context)));\n";
                }
                break;
            case row_codec_field::storage_kind::text_string:
                if (field.nullable) {
                    output
                        << "            if (!row." << field.cpp_name
                        << ".has_value()) {\n"
                        << "                data_.set_null("
                        << field.attribute_number << ");\n"
                        << "            } else {\n"
                        << "                data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                    "
                           "bioseq::postgres::support::encode_text_to_memory_context(\n"
                        << "                        std::string_view{\n"
                        << "                            row." << field.cpp_name
                        << "->data(), row." << field.cpp_name
                        << "->size()},\n"
                        << "                        value_context)));\n"
                        << "            }\n";
                } else {
                    output
                        << "            data_.set_raw("
                        << field.attribute_number << ", "
                        << "bioseq::postgres::support::pg_datum::from_raw(\n"
                        << "                "
                           "bioseq::postgres::support::encode_text_to_memory_context(\n"
                        << "                    std::string_view{\n"
                        << "                        row." << field.cpp_name
                        << ".data(), row." << field.cpp_name
                        << ".size()},\n"
                        << "                    value_context)));\n";
                }
                break;
        }
    }
    output
        << "        }\n\n"
        << "        bioseq::postgres::support::encoded_row_data<field_count> "
           "data_{};\n"
        << "    };\n\n"
        << "    [[nodiscard]] static auto encode(\n"
        << "            const row_type& row";
    if (has_by_reference_fields) {
        output << ",\n            MemoryContext value_context";
    }
    output
        << ") -> encoded_row {\n"
        << "        return encoded_row{row";
    if (has_by_reference_fields) {
        output << ", value_context";
    }
    output
        << "};\n"
        << "    }\n\n";
    if (!has_by_reference_fields) {
        output
            << "    [[nodiscard]] static auto encode(\n"
            << "            const row_type& row,\n"
            << "            MemoryContext) -> encoded_row {\n"
            << "        return encoded_row{row};\n"
            << "    }\n\n";
    }
    output
        << "    [[nodiscard]] static auto deform(\n"
        << "            HeapTuple tuple,\n"
        << "            TupleDesc descriptor,\n"
        << "            MemoryContext result_context) -> row_type {\n"
        << "        const auto data =\n"
        << "            bioseq::postgres::support::deformed_row_data<field_count>::"
           "deform(\n"
        << "                tuple,\n"
        << "                descriptor,\n"
        << "                result_context,\n"
        << "                row_name,\n"
        << "                expected_descriptor);\n";
    for (const row_codec_field& field : fields) {
        if (!field.nullable) {
            output
                << "        bioseq::postgres::support::require_row_attribute_not_null(\n"
                << "            data.is_null(" << field.attribute_number
                << "), " << field.attribute_number
                << ", row_name);\n";
        }
    }
    output << "        return row_type{\n";
    for (const row_codec_field& field : fields) {
        output << "            ." << field.cpp_name << " = ";
        switch (field.storage) {
            case row_codec_field::storage_kind::scalar:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : " << field.cpp_type
                        << "{bioseq::postgres::support::pg_datum::to<"
                        << field.underlying_cpp_type << ">(data.at("
                        << field.attribute_number << "))}";
                } else {
                    output
                        << "bioseq::postgres::support::pg_datum::to<"
                        << field.cpp_type << ">(data.at("
                        << field.attribute_number << "))";
                }
                break;
            case row_codec_field::storage_kind::oid:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : std::optional<Oid>{"
                           "bioseq::postgres::support::pg_datum::to_oid(data.at("
                        << field.attribute_number << "))}";
                } else {
                    output
                        << "bioseq::postgres::support::pg_datum::to_oid(data.at("
                        << field.attribute_number << "))";
                }
                break;
            case row_codec_field::storage_kind::timestamp_tz:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : std::optional<TimestampTz>{"
                           "DatumGetTimestampTz(data.at("
                        << field.attribute_number << ").raw())}";
                } else {
                    output
                        << "DatumGetTimestampTz(data.at("
                        << field.attribute_number << ").raw())";
                }
                break;
            case row_codec_field::storage_kind::xid8:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : "
                           "std::optional<FullTransactionId>{"
                           "DatumGetFullTransactionId(data.at("
                        << field.attribute_number << ").raw())}";
                } else {
                    output
                        << "DatumGetFullTransactionId(data.at("
                        << field.attribute_number << ").raw())";
                }
                break;
            case row_codec_field::storage_kind::name:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : std::optional<NameData>{\n"
                        << "                    *bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context<NameData>(\n"
                        << "                        data.at("
                        << field.attribute_number << "),\n"
                        << "                        field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                        result_context)}";
                } else {
                    output
                        << "*bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context<NameData>(\n"
                        << "                data.at("
                        << field.attribute_number << "),\n"
                        << "                field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                result_context)";
                }
                break;
            case row_codec_field::storage_kind::bytea_pointer:
            case row_codec_field::storage_kind::text_pointer:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : " << field.cpp_type << "{\n"
                        << "                    bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context<"
                        << (field.storage
                                    == row_codec_field::storage_kind::
                                        bytea_pointer
                                ? "bytea"
                                : "text")
                        << ">(\n"
                        << "                        data.at("
                        << field.attribute_number << "),\n"
                        << "                        field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                        result_context)}";
                } else {
                    output
                        << "bioseq::postgres::support::"
                           "copy_pg_datum_to_memory_context<"
                        << (field.storage
                                    == row_codec_field::storage_kind::
                                        bytea_pointer
                                ? "bytea"
                                : "text")
                        << ">(\n"
                        << "                data.at("
                        << field.attribute_number << "),\n"
                        << "                field<&row_type::"
                        << field.cpp_name << ">::datum_copy_policy,\n"
                        << "                result_context)";
                }
                break;
            case row_codec_field::storage_kind::text_string:
                if (field.nullable) {
                    output
                        << "data.is_null(" << field.attribute_number
                        << ")\n"
                        << "                ? std::nullopt\n"
                        << "                : std::optional<std::string>{"
                           "bioseq::postgres::support::decode_text(\n"
                        << "                    data.at("
                        << field.attribute_number << ").raw())}";
                } else {
                    output
                        << "bioseq::postgres::support::decode_text(\n"
                        << "                data.at("
                        << field.attribute_number << ").raw())";
                }
                break;
        }
        output << ",\n";
    }
    output
        << "        };\n"
        << "    }\n\n"
        << "    class update final {\n"
        << "    public:\n"
        << "        update() noexcept = default;\n"
        << "        update(const update&) = delete;\n"
        << "        auto operator=(const update&) -> update& = delete;\n"
        << "        update(update&&) noexcept = default;\n"
        << "        auto operator=(update&&) noexcept -> update& = default;\n\n"
        << "        template<auto p_member>\n"
        << "            requires (!field<p_member>::by_reference)\n"
        << "        auto set(member_type<p_member> value) noexcept -> update& {\n"
        << "            if constexpr (field<p_member>::nullable) {\n"
        << "                if (!value.has_value()) {\n"
        << "                    data_.set_null("
           "attribute_number<p_member>());\n"
        << "                } else if constexpr "
           "(field<p_member>::oid_value) {\n"
        << "                    data_.set_raw(\n"
        << "                        attribute_number<p_member>(),\n"
        << "                        "
           "bioseq::postgres::support::pg_datum::from_oid(*value));\n"
        << "                } else if constexpr "
           "(field<p_member>::timestamp_tz_value) {\n"
        << "                    data_.set_raw(\n"
        << "                        attribute_number<p_member>(),\n"
        << "                        "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            TimestampTzGetDatum(*value)));\n"
        << "                } else if constexpr "
           "(field<p_member>::xid8_value) {\n"
        << "                    data_.set_raw(\n"
        << "                        attribute_number<p_member>(),\n"
        << "                        "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            "
           "FullTransactionIdGetDatum(*value)));\n"
        << "                } else {\n"
        << "                    data_.set(\n"
        << "                        attribute_number<p_member>(), "
           "*value);\n"
        << "                }\n"
        << "            } else if constexpr "
           "(field<p_member>::oid_value) {\n"
        << "                data_.set_raw(\n"
        << "                    attribute_number<p_member>(),\n"
        << "                    "
           "bioseq::postgres::support::pg_datum::from_oid(value));\n"
        << "            } else if constexpr "
           "(field<p_member>::timestamp_tz_value) {\n"
        << "                data_.set_raw(\n"
        << "                    attribute_number<p_member>(),\n"
        << "                    "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                        TimestampTzGetDatum(value)));\n"
        << "            } else if constexpr "
           "(field<p_member>::xid8_value) {\n"
        << "                data_.set_raw(\n"
        << "                    attribute_number<p_member>(),\n"
        << "                    "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                        FullTransactionIdGetDatum(value)));\n"
        << "            } else {\n"
        << "                data_.set(\n"
        << "                    attribute_number<p_member>(), value);\n"
        << "            }\n"
        << "            return *this;\n"
        << "        }\n\n"
        << "        template<auto p_member>\n"
        << "            requires field<p_member>::by_reference\n"
        << "        auto set(\n"
        << "                member_type<p_member> value,\n"
        << "                MemoryContext value_context) -> update& {\n"
        << "            bioseq::postgres::support::require_row_result_context(\n"
        << "                value_context, row_name);\n"
        << "            if constexpr (field<p_member>::name_value) {\n"
        << "                if constexpr (field<p_member>::nullable) {\n"
        << "                    if (!value.has_value()) {\n"
        << "                        data_.set_null(\n"
        << "                            attribute_number<p_member>());\n"
        << "                    } else {\n"
        << "                        data_.set_raw(\n"
        << "                            attribute_number<p_member>(),\n"
        << "                            "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                                "
           "bioseq::postgres::support::copy_pg_datum_to_memory_context(\n"
        << "                                    "
           "bioseq::postgres::support::pg_datum::from_name(\n"
        << "                                        &*value).raw(),\n"
        << "                                    "
           "field<p_member>::datum_copy_policy,\n"
        << "                                    value_context)));\n"
        << "                    }\n"
        << "                } else {\n"
        << "                    data_.set_raw(\n"
        << "                        attribute_number<p_member>(),\n"
        << "                        "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            "
           "bioseq::postgres::support::copy_pg_datum_to_memory_context(\n"
        << "                                "
           "bioseq::postgres::support::pg_datum::from_name(\n"
        << "                                    &value).raw(),\n"
        << "                                "
           "field<p_member>::datum_copy_policy,\n"
        << "                                value_context)));\n"
        << "                }\n"
        << "            } else if constexpr "
           "(field<p_member>::text_string) {\n"
        << "                const auto encode_text = [this, value_context](\n"
        << "                        std::string_view text_value) -> void {\n"
        << "                    data_.set_raw(\n"
        << "                        attribute_number<p_member>(),\n"
        << "                        bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            "
           "bioseq::postgres::support::encode_text_to_memory_context(\n"
        << "                                text_value,\n"
        << "                                value_context)));\n"
        << "                };\n"
        << "                static_assert(std::is_trivially_destructible_v<\n"
        << "                              decltype(encode_text)>);\n"
        << "                if constexpr (field<p_member>::nullable) {\n"
        << "                    if (!value.has_value()) {\n"
        << "                        data_.set_null(\n"
        << "                            attribute_number<p_member>());\n"
        << "                    } else {\n"
        << "                        encode_text(*value);\n"
        << "                    }\n"
        << "                } else {\n"
        << "                    encode_text(value);\n"
        << "                }\n"
        << "            } else if constexpr "
           "(field<p_member>::bytea_pointer\n"
        << "                                 || "
           "field<p_member>::text_pointer) {\n"
        << "                if constexpr (field<p_member>::nullable) {\n"
        << "                    if (!value.has_value()\n"
        << "                        || *value == nullptr) {\n"
        << "                        data_.set_null(\n"
        << "                            attribute_number<p_member>());\n"
        << "                    } else {\n"
        << "                        data_.set_raw(\n"
        << "                            attribute_number<p_member>(),\n"
        << "                            "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                                "
           "bioseq::postgres::support::copy_pg_datum_to_memory_context(\n"
        << "                                    PointerGetDatum(*value),\n"
        << "                                    "
           "field<p_member>::datum_copy_policy,\n"
        << "                                    value_context)));\n"
        << "                    }\n"
        << "                } else {\n"
        << "                    if (value == nullptr) {\n"
        << "                        throw "
           "bioseq::postgres::support::row_codec_error{\n"
        << "                            std::string{row_name} + "
           "\" has a null C++ varlena pointer\"};\n"
        << "                    }\n"
        << "                    data_.set_raw(\n"
        << "                        attribute_number<p_member>(),\n"
        << "                        "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            "
           "bioseq::postgres::support::copy_pg_datum_to_memory_context(\n"
        << "                                PointerGetDatum(value),\n"
        << "                                "
           "field<p_member>::datum_copy_policy,\n"
        << "                                value_context)));\n"
        << "                }\n"
        << "            }\n"
        << "            return *this;\n"
        << "        }\n\n"
        << "        template<auto p_member>\n"
        << "            requires field<p_member>::nullable\n"
        << "        auto set_null() noexcept -> update& {\n"
        << "            data_.set_null(attribute_number<p_member>());\n"
        << "            return *this;\n"
        << "        }\n\n"
        << "        [[nodiscard]] auto modify(\n"
        << "                HeapTuple old_tuple,\n"
        << "                TupleDesc descriptor) -> HeapTuple {\n"
        << "            return data_.modify(\n"
        << "                old_tuple,\n"
        << "                descriptor,\n"
        << "                row_name,\n"
        << "                expected_descriptor);\n"
        << "        }\n\n"
        << "    private:\n"
        << "        bioseq::postgres::support::row_update_data<field_count> "
           "data_{};\n"
        << "    };\n\n"
        << "    template<auto p_member,\n"
        << "             bioseq::postgres::support::scan_comparison p_comparison =\n"
        << "                 bioseq::postgres::support::scan_comparison::equal>\n"
        << "        requires (!field<p_member>::by_reference\n"
        << "                  && !field<p_member>::nullable)\n"
        << "    [[nodiscard]] static auto scan_key(\n"
        << "            member_type<p_member> value) -> ScanKeyData {\n"
        << "        return bioseq::postgres::support::make_row_scan_key(\n"
        << "            attribute_number<p_member>(),\n"
        << "            {\n"
        << "                .strategy =\n"
        << "                    bioseq::postgres::support::row_scan_strategy(\n"
        << "                        p_comparison),\n"
        << "                .procedure =\n"
        << "                    "
           "field<p_member>::comparison_procedure(p_comparison),\n"
        << "                .datum = [value]() "
           "-> bioseq::postgres::support::pg_datum {\n"
        << "                    if constexpr (field<p_member>::oid_value) {\n"
        << "                        return "
           "bioseq::postgres::support::pg_datum::from_oid(value);\n"
        << "                    } else if constexpr "
           "(field<p_member>::timestamp_tz_value) {\n"
        << "                        return "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            TimestampTzGetDatum(value));\n"
        << "                    } else if constexpr "
           "(field<p_member>::xid8_value) {\n"
        << "                        return "
           "bioseq::postgres::support::pg_datum::from_raw(\n"
        << "                            FullTransactionIdGetDatum(value));\n"
        << "                    } else {\n"
        << "                        return "
           "bioseq::postgres::support::pg_datum::from(value);\n"
        << "                    }\n"
        << "                }(),\n"
        << "            });\n"
        << "    }\n"
        << "};\n\n";
    std::string rendered = output.str();
    validate_row_codec_text_lifetime_contract(
        rendered, text_field_count);
    return rendered;
}

/// Renders a module containing its deterministic set of row codecs.
[[nodiscard]] inline std::string render_row_codec_module(
    const rendered_row_codec_module& module,
    const std::vector<std::pair<
        const normalized_entity*,
        std::vector<const normalized_entity*>>>& rows) {
    std::ostringstream output;
    output
        << "module;\n\n"
        << "#include <bioseq/pg_compat.h>\n"
        << "#include \"utils/builtins.h\"\n"
        << "#include \"utils/timestamp.h\"\n"
        << "#include \"utils/xid8.h\"\n\n"
        << "#include <array>\n"
        << "#include <concepts>\n"
        << "#include <cstddef>\n"
        << "#include <cstdint>\n"
        << "#include <optional>\n"
        << "#include <string>\n"
        << "#include <string_view>\n"
        << "#include <type_traits>\n"
        << "#include <utility>\n\n"
        << "export module " << module.module_name << ";\n\n"
        << "export import " << module.source_module << ";\n"
        << "export import bioseq.postgres.support.sql_boundary;\n\n"
        << "export namespace " << module.cpp_namespace << " {\n\n";
    for (const auto& [row, columns] : rows) {
        output << render_row_codec(*row, columns);
    }
    output << "} // namespace " << module.cpp_namespace << "\n";
    return output.str();
}

}  // namespace detail

[[nodiscard]] inline std::vector<rendered_entity_handle_module>
render_entity_handle_modules(
        const sql_graph& graph,
        const std::vector<
            std::pair<std::string, std::string>>& configured_owners) {
    std::map<std::string, rendered_entity_handle_module, std::less<>>
        grouped;
    std::map<std::string, std::string, std::less<>> source_by_owner_key;
    std::map<std::string, std::string, std::less<>>
        configured_owner_by_module;
    std::set<std::string, std::less<>> configured_owner_keys;
    for (const auto& [owner_key, source_module] : configured_owners) {
        if (!configured_owner_by_module.emplace(
                source_module, owner_key).second
            || !configured_owner_keys.emplace(owner_key).second) {
            throw std::runtime_error(
                "configured entity-handle owner mapping is not one-to-one "
                "for '" + owner_key + "'");
        }
    }
    for (const normalized_entity& entity : graph.entities) {
        if (entity.source.kind != entity_kind::sequence) {
            continue;
        }
        if (!entity.source.active_condition.empty()) {
            throw std::runtime_error(
                "generated entity handle '" + entity.stable_identity
                + "' is not profile invariant");
        }
        if (entity.source.schema != "$extension"
            && entity.source.schema.empty()) {
            throw std::runtime_error(
                "generated entity handle '" + entity.stable_identity
                + "' has no schema binding");
        }
        const auto configured_owner = configured_owner_by_module.find(
            entity.source.source_module);
        if (configured_owner == configured_owner_by_module.end()) {
            throw std::runtime_error(
                "generated entity-handle source module '"
                + entity.source.source_module
                + "' has no configured owner mapping");
        }
        const std::string& owner_key = configured_owner->second;
        const auto [source_position, inserted] =
            source_by_owner_key.emplace(
                owner_key, entity.source.source_module);
        if (!inserted
            && source_position->second != entity.source.source_module) {
            throw std::runtime_error(
                "generated C++ owner key collision for '" + owner_key
                + "' between modules '" + source_position->second
                + "' and '" + entity.source.source_module + "'");
        }
        auto [position, module_inserted] = grouped.try_emplace(
            owner_key,
            rendered_entity_handle_module{
                .owner_key = owner_key,
                .source_module = entity.source.source_module,
                .module_name =
                    detail::entity_handle_module_name(owner_key),
                .cpp_namespace =
                    detail::entity_handle_cpp_namespace(owner_key),
            });
        static_cast<void>(module_inserted);
        position->second.handles.push_back(rendered_entity_handle{
            .entity_identity = entity.stable_identity,
            .installed_identity = entity.installed_identity,
            .leaf_name = entity.source.name,
            .schema_name = entity.source.schema,
            .schema_binding =
                entity.source.schema == "$extension"
                ? "extension"
                : "fixed",
            .expected_relkind = "RELKIND_SEQUENCE",
            .variable_name = entity.source.name,
        });
    }

    for (const auto& [owner_key, source_module] : configured_owners) {
        if (!grouped.contains(owner_key)) {
            throw std::runtime_error(
                "configured entity-handle owner '" + owner_key
                + "' has no generated sequence in module '"
                + source_module + "'");
        }
    }

    std::vector<rendered_entity_handle_module> result;
    result.reserve(grouped.size());
    for (auto& [owner_key, module] : grouped) {
        static_cast<void>(owner_key);
        std::ranges::sort(
            module.handles,
            {},
            &rendered_entity_handle::entity_identity);
        const auto duplicate = std::ranges::adjacent_find(
            module.handles,
            {},
            &rendered_entity_handle::variable_name);
        if (duplicate != module.handles.end()) {
            throw std::runtime_error(
                "generated entity-handle variable collision for '"
                + duplicate->variable_name + "' in " + module.module_name);
        }
        module.content = detail::render_entity_handle_module(module);
        result.push_back(std::move(module));
    }
    return result;
}

[[nodiscard]] inline std::vector<rendered_row_codec_module>
render_row_codec_modules(
        const sql_graph& graph,
        const std::vector<
            std::pair<std::string, std::string>>& configured_owners) {
    using row_group = std::vector<std::pair<
        const normalized_entity*,
        std::vector<const normalized_entity*>>>;
    std::map<std::string, std::string, std::less<>>
        configured_owner_by_module;
    std::set<std::string, std::less<>> configured_owner_keys;
    for (const auto& [owner_key, source_module] : configured_owners) {
        if (!configured_owner_by_module.emplace(
                source_module, owner_key).second
            || !configured_owner_keys.emplace(owner_key).second) {
            throw std::runtime_error(
                "configured row-codec owner mapping is not one-to-one "
                "for '" + owner_key + "'");
        }
    }
    std::map<std::string, row_group, std::less<>> rows_by_source;
    for (const normalized_entity& entity : graph.entities) {
        if ((entity.source.kind != entity_kind::table
             && entity.source.kind != entity_kind::type)
            || !entity.source.source_module.ends_with(".sql_rows")) {
            continue;
        }
        if (!entity.source.active_condition.empty()) {
            throw std::runtime_error(
                "generated row codec '" + entity.stable_identity
                + "' is not profile invariant");
        }
        std::vector<const normalized_entity*> columns;
        for (const normalized_entity& candidate : graph.entities) {
            if (candidate.source.kind == entity_kind::column
                && candidate.source.enclosing_cpp_name
                    == entity.source.cpp_qualified_name
                && std::ranges::contains(
                    candidate.dependency_identities,
                    entity.stable_identity)) {
                if (!candidate.source.active_condition.empty()) {
                    throw std::runtime_error(
                        "generated row-codec field '"
                        + candidate.stable_identity
                        + "' is not profile invariant");
                }
                columns.push_back(&candidate);
            }
        }
        rows_by_source[entity.source.source_module].push_back(
            {&entity, std::move(columns)});
    }

    std::vector<rendered_row_codec_module> result;
    result.reserve(rows_by_source.size());
    for (auto& [source_module, rows] : rows_by_source) {
        std::ranges::sort(
            rows,
            {},
            [](const auto& row) {
                return row.first->source.cpp_qualified_name;
            });
        const auto configured_owner =
            configured_owner_by_module.find(source_module);
        if (configured_owner == configured_owner_by_module.end()) {
            throw std::runtime_error(
                "generated row-codec source module '" + source_module
                + "' has no configured owner mapping");
        }
        const std::string& owner_key = configured_owner->second;
        rendered_row_codec_module module{
            .owner_key = owner_key,
            .source_module = source_module,
            .module_name =
                detail::row_codec_module_name(source_module),
            .cpp_namespace = detail::cpp_namespace_from_module(
                detail::row_codec_module_name(source_module)),
        };
        for (const auto& [row, columns] : rows) {
            static_cast<void>(columns);
            module.row_cpp_names.push_back(
                row->source.cpp_qualified_name);
        }
        module.content =
            detail::render_row_codec_module(module, rows);
        result.push_back(std::move(module));
    }
    for (const auto& [owner_key, source_module] : configured_owners) {
        if (!rows_by_source.contains(source_module)) {
            throw std::runtime_error(
                "configured row-codec owner '" + owner_key
                + "' has no generated row in module '"
                + source_module + "'");
        }
    }
    return result;
}

}  // namespace bioseq::codegen::sql
