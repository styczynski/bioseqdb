/** @file Copyable PostgreSQL plan data for the export custom executor. */
module;
#include <bioseq/pg_compat.h>
extern "C" {
#include "nodes/makefuncs.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
}
export module bioseq.postgres.storage.io.exporting.plan_contract;
import std;
import bioseq.io.record_schema;
import bioseq.postgres.support;
import bioseq.postgres.storage.io.exporting.query_binding;

export namespace bioseq::postgres::storage::io::exporting {
/** Versioned custom_private fields are nodes, never process-local pointers. */
enum class export_plan_field : int {
    version, function_oid, destination, mappings, options, query_text,
    columns, visible_columns, match_owner, count
};
inline constexpr int export_plan_codec_version = 1;

/** Encode each checked schema entry into copyObject-compatible lists. */
[[nodiscard]] inline List* encode_export_columns(const std::vector<export_column_binding>& columns) {
    List* encoded = NIL;
    for (const auto& column : columns) {
        List* fields = NIL;
        fields = lappend(fields, makeInteger(column.result_number));
        fields = lappend(fields, makeString(pstrdup(column.column.name.c_str())));
        fields = lappend(fields, makeString(pstrdup(column.column.qualified_type.c_str())));
        fields = lappend(fields, makeInteger(static_cast<int>(column.column.kind)));
        fields = lappend(fields, makeInteger(column.column.alphabet_code ? *column.column.alphabet_code : -1));
        fields = lappend(fields, makeInteger(column.column.nullable));
        fields = lappend(fields, makeInteger(column.column.single_column_primary_key));
        fields = lappend(fields, makeInteger(column.role ? static_cast<int>(*column.role) : -1));
        fields = lappend(fields, makeInteger(column.private_companion));
        fields = lappend(fields, makeInteger(static_cast<int>(column.internal_fields)));
        fields = lappend(fields, makeInteger(static_cast<int>(column.coordinate_basis)));
        const std::string tag = column.auxiliary_tag
            ? std::string{column.auxiliary_tag->data(), column.auxiliary_tag->size()} : std::string{};
        fields = lappend(fields, makeString(pstrdup(tag.c_str())));
        fields = lappend(fields, makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
            ObjectIdGetDatum(column.source_relation), false, true));
        fields = lappend(fields, makeInteger(column.source_attribute));
        fields = lappend(fields, makeConst(OIDOID, -1, InvalidOid, sizeof(Oid),
            ObjectIdGetDatum(column.helper_function), false, true));
        encoded = lappend(encoded, fields);
    }
    return encoded;
}

/** Reject malformed private nodes before any child data or file is accessed. */
[[nodiscard]] inline Node* export_private_node(const CustomScan& scan, export_plan_field field) {
    if (list_length(scan.custom_private) != static_cast<int>(export_plan_field::count))
        export_binding_error("corrupt BioseqExport plan field count");
    auto* version = static_cast<Node*>(linitial(scan.custom_private));
    if (!IsA(version, Integer) || intVal(version) != export_plan_codec_version)
        export_binding_error("unsupported BioseqExport plan version");
    return static_cast<Node*>(list_nth(scan.custom_private, static_cast<int>(field)));
}

/** Decode schema once per execution; no PostgreSQL nodes enter worker records. */
[[nodiscard]] inline std::vector<export_column_binding> decode_export_columns(const CustomScan& scan) {
    auto* node = export_private_node(scan, export_plan_field::columns);
    if (node && !IsA(node, List)) export_binding_error("invalid BioseqExport column descriptors");
    auto* entries = reinterpret_cast<List*>(node);
    std::vector<export_column_binding> result;
    result.reserve(list_length(entries));
    for (auto* cell = list_head(entries); cell; cell = lnext(entries, cell)) {
        auto* row = static_cast<List*>(lfirst(cell));
        if (!row || !IsA(row, List) || list_length(row) != 15)
            export_binding_error("invalid BioseqExport column descriptor width");
        const auto integer = [row](int index) {
            auto* field = static_cast<Node*>(list_nth(row, index));
            if (!IsA(field, Integer)) export_binding_error("invalid BioseqExport integer descriptor");
            return intVal(field);
        };
        const auto text = [row](int index) -> const char* {
            auto* field = static_cast<Node*>(list_nth(row, index));
            if (!IsA(field, String)) export_binding_error("invalid BioseqExport text descriptor");
            return strVal(field);
        };
        export_column_binding binding;
        const auto attribute = integer(0);
        if (attribute <= 0 || attribute > std::numeric_limits<AttrNumber>::max())
            export_binding_error("invalid BioseqExport output attribute");
        binding.result_number = static_cast<AttrNumber>(attribute);
        binding.column.ordinal = result.size();
        binding.column.name = text(1);
        binding.column.qualified_type = text(2);
        const auto kind = integer(3);
        if (kind < 0 || kind > static_cast<int>(column_value_kind::scalar_text))
            export_binding_error("invalid BioseqExport column category");
        binding.column.kind = static_cast<column_value_kind>(kind);
        const auto alphabet = integer(4);
        if (alphabet < -1 || alphabet > 255) export_binding_error("invalid BioseqExport alphabet");
        if (alphabet >= 0) binding.column.alphabet_code = static_cast<std::uint8_t>(alphabet);
        binding.column.nullable = integer(5) != 0;
        binding.column.single_column_primary_key = integer(6) != 0;
        const auto role = integer(7);
        if (role < -1 || role > static_cast<int>(record_field_role::omit))
            export_binding_error("invalid BioseqExport role");
        if (role >= 0) binding.role = static_cast<record_field_role>(role);
        binding.private_companion = integer(8) != 0;
        const auto internal = integer(9);
        if (internal < 0 || static_cast<std::uint32_t>(internal) >= (std::uint32_t{1} << (static_cast<unsigned>(export_internal_field::pair_mapping_quality) + 1u)) || (internal & 1) != 0)
            export_binding_error("invalid BioseqExport private field");
        binding.internal_fields = static_cast<std::uint32_t>(internal);
        const auto basis = integer(10);
        if (basis < 0 || basis > static_cast<int>(bioseq::io::coordinate_basis::sam_one_based))
            export_binding_error("invalid BioseqExport coordinate basis");
        binding.coordinate_basis = static_cast<bioseq::io::coordinate_basis>(basis);
        const std::string_view tag = text(11);
        if (!tag.empty()) {
            if (tag.size() != 2) export_binding_error("invalid BioseqExport auxiliary tag descriptor");
            binding.auxiliary_tag = std::array<char, 2>{tag[0], tag[1]};
        }
        auto* relation = static_cast<Node*>(list_nth(row, 12));
        if (!IsA(relation, Const) || reinterpret_cast<Const*>(relation)->consttype != OIDOID)
            export_binding_error("invalid BioseqExport source relation descriptor");
        binding.source_relation = DatumGetObjectId(reinterpret_cast<Const*>(relation)->constvalue);
        const auto source_attribute = integer(13);
        if (source_attribute < 0 || source_attribute > std::numeric_limits<AttrNumber>::max())
            export_binding_error("invalid BioseqExport source attribute descriptor");
        binding.source_attribute = static_cast<AttrNumber>(source_attribute);
        auto* helper = static_cast<Node*>(list_nth(row, 14));
        if (!IsA(helper, Const) || reinterpret_cast<Const*>(helper)->consttype != OIDOID)
            export_binding_error("invalid BioseqExport helper descriptor");
        binding.helper_function = DatumGetObjectId(reinterpret_cast<Const*>(helper)->constvalue);
        result.push_back(std::move(binding));
    }
    return result;
}

/** Return a checked typed Const from the shared codec. */
[[nodiscard]] inline const Const& export_private_const(const CustomScan& scan,
    export_plan_field field, Oid expected_type) {
    auto* node = export_private_node(scan, field);
    if (!IsA(node, Const)) export_binding_error("invalid BioseqExport constant");
    const auto& constant = *reinterpret_cast<Const*>(node);
    if (constant.consttype != expected_type) export_binding_error("invalid BioseqExport constant type");
    return constant;
}
} // namespace bioseq::postgres::storage::io::exporting
