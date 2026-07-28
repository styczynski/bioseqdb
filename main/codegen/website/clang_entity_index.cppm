module;

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:clang_entity_index;

import bioseq.codegen.clang_structural;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Clang structural/export index, module-graph side. The shared structural module
// owns the Clang pass and its compiler-neutral POD model. This partition projects
// that model into documented_entity / pg_entrypoint_candidate so entity_index and
// site_builder continue to see only website model types.
namespace bioseq::codegen::website {

/// @brief Result of the Clang structural pass over the reachable public units.
struct clang_structural_index {
    std::vector<documented_entity> entities;
    std::vector<pg_entrypoint_candidate> pg_entrypoints;
    std::string clang_version;
};

namespace {

namespace structural = ::bioseq::codegen::clang_structural;

[[nodiscard]] bool is_website_entity_kind(
    structural::declaration_kind kind) noexcept {
    switch (kind) {
        case structural::declaration_kind::namespace_:
        case structural::declaration_kind::record:
        case structural::declaration_kind::enum_:
        case structural::declaration_kind::enumerator:
        case structural::declaration_kind::alias:
        case structural::declaration_kind::concept_:
        case structural::declaration_kind::function:
        case structural::declaration_kind::variable:
            return true;
        case structural::declaration_kind::unknown:
        case structural::declaration_kind::parameter:
        case structural::declaration_kind::field:
            return false;
    }
    return false;
}

[[nodiscard]] entity_kind kind_from_structural(
    const structural::declaration_record& record) noexcept {
    switch (record.kind) {
        case structural::declaration_kind::namespace_:
            return entity_kind::namespace_;
        case structural::declaration_kind::record:
            switch (record.record_kind) {
                case structural::aggregate_kind::struct_:
                    return entity_kind::struct_;
                case structural::aggregate_kind::class_:
                    return entity_kind::class_;
                case structural::aggregate_kind::union_:
                    return entity_kind::union_;
            }
            return entity_kind::struct_;
        case structural::declaration_kind::enum_:
            return entity_kind::enum_;
        case structural::declaration_kind::enumerator:
            return entity_kind::enum_value;
        case structural::declaration_kind::alias:
            return entity_kind::type_alias;
        case structural::declaration_kind::concept_:
            return entity_kind::concept_;
        case structural::declaration_kind::variable:
            return entity_kind::variable;
        case structural::declaration_kind::function:
        case structural::declaration_kind::unknown:
        case structural::declaration_kind::parameter:
        case structural::declaration_kind::field:
            return entity_kind::function;
    }
    return entity_kind::function;
}

[[nodiscard]] documented_entity from_structural(
    const structural::declaration_record& record,
    const std::filesystem::path& source_path) {
    documented_entity entity;
    entity.domain = entity_domain::cpp;
    entity.kind = kind_from_structural(record);
    entity.qualified_name = record.qualified_name;
    entity.display_name = record.name;
    entity.normalized_signature =
        record.kind == structural::declaration_kind::function
        ? record.signature_text
        : record.qualified_name;
    if (record.kind == structural::declaration_kind::function) {
        entity.return_type = record.return_type;
    }
    if (!record.owning_module.empty()) {
        entity.module_name = record.owning_module;
    }
    source_span span;
    span.path = source_path;
    span.first_line = record.name_range.begin.line;
    span.first_column = record.name_range.begin.column;
    span.last_line = record.name_range.end.line;
    span.last_column = record.name_range.end.column;
    entity.declaration = std::move(span);
    entity.attributes.emplace("exported", "true");
    if (!record.member_kind.empty()) {
        entity.attributes.emplace("member_kind", record.member_kind);
    }
    for (const structural::parameter_record& parameter : record.parameters) {
        parameter_documentation info;
        info.name = parameter.name;
        info.type = parameter.cpp_type;
        if (parameter.has_default) {
            info.default_value = parameter.default_expression;
        }
        entity.parameters.push_back(std::move(info));
    }
    return entity;
}

[[nodiscard]] clang_structural_index from_structural_index(
    const structural::structural_index& structural_index) {
    clang_structural_index index;
    index.clang_version = structural_index.clang_version;
    std::set<std::string, std::less<>> seen;
    for (const structural::translation_unit& unit :
         structural_index.translation_units) {
        for (const structural::declaration_record& record :
             unit.declarations) {
            if (!record.exported || !is_website_entity_kind(record.kind)) {
                continue;
            }
            documented_entity entity =
                from_structural(record, unit.source_path);
            std::string key = entity.qualified_name + '\x1f'
                + entity.normalized_signature;
            if (seen.insert(std::move(key)).second) {
                index.entities.push_back(std::move(entity));
            }
        }
    }
    return index;
}

[[nodiscard]] module_graph_unit unit_from_structural(
    const structural::module_graph_unit& record) {
    module_graph_unit unit;
    unit.source_path = record.source_path;
    unit.provided_module = record.provided_module;
    unit.exported_imports = record.exported_imports;
    unit.internal_imports = record.internal_imports;
    return unit;
}

}  // namespace

/// @brief Reports the Clang version this generator is linked against.
/// @return The CLANG_VERSION_STRING for the §11.3 exact-version pin.
[[nodiscard]] std::string clang_api_version() {
    return structural::clang_api_version();
}

/// @brief Runs the structural pass over in-memory module source (test entry).
/// @param code A self-contained C++ module translation unit.
/// @param extra_args Compiler arguments (for example -std=c++26).
/// @return The exported/public entities the pass records.
[[nodiscard]] clang_structural_index parse_clang_structural_index_from_code(
    std::string_view code, const std::vector<std::string>& extra_args) {
    structural::structural_index index;
    index.translation_units.push_back(
        structural::scan_translation_unit_from_code(
            "unit.cppm", code, extra_args));
    index.clang_version = index.translation_units.front().clang_version;
    return from_structural_index(index);
}

/// @brief Runs the Clang 22 structural/export index over reachable interfaces.
/// @param reachable_units Source-relative public interface files to parse.
/// @param compile_database Path to the Clang-produced compile_commands.json.
/// @return The structural index of exported/public entities.
[[nodiscard]] clang_structural_index parse_clang_structural_index(
    const std::vector<std::filesystem::path>& reachable_units,
    const std::filesystem::path& compile_database) {
    return from_structural_index(
        structural::scan_translation_units_from_database(
            reachable_units, compile_database));
}

/// @brief Scans each reachable unit's provided module and import edges (§11.3).
/// @param reachable_units Module interface files to scan.
/// @param compile_database Path to the Clang-produced compile_commands.json.
/// @return One module_graph_unit per scanned interface unit.
[[nodiscard]] std::vector<module_graph_unit> scan_module_graph(
    const std::vector<std::filesystem::path>& reachable_units,
    const std::filesystem::path& compile_database) {
    const structural::module_graph graph =
        structural::scan_module_graph_from_database(
            reachable_units, compile_database);
    std::vector<module_graph_unit> result;
    result.reserve(graph.units.size());
    for (const structural::module_graph_unit& unit : graph.units) {
        result.push_back(unit_from_structural(unit));
    }
    return result;
}

/// @brief Scans one in-memory unit's provided module and import edges (tests).
/// @param code A self-contained C++ module translation unit.
/// @param extra_args Compiler arguments (for example -std=c++26).
/// @return The single scanned unit's module/edge record.
[[nodiscard]] module_graph_unit scan_module_graph_from_code(
    std::string_view code, const std::vector<std::string>& extra_args) {
    return unit_from_structural(
        structural::scan_module_graph_from_code(code, extra_args));
}

}  // namespace bioseq::codegen::website
