module;

#include <filesystem>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:cpp_reference_index;

import bioseq.codegen.structural_analysis;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Converts the shared Clang scanner results into website reference
// entries and module imports. Later website stages use these model types
// without depending on Clang objects.
namespace bioseq::codegen::website {

/// @brief Public C++ declarations collected by Clang for reference pages.
struct cpp_reference_index {
    std::vector<reference_entity> entities;
    std::vector<pg_entrypoint_candidate> pg_entrypoints;
    std::string clang_release;
};

namespace {

namespace structural = ::bioseq::codegen::structural_analysis;

[[nodiscard]] bool is_website_entity_kind(
    structural::declaration_kind kind) noexcept {
    switch (kind) {
        case structural::declaration_kind::namespace_declaration:
        case structural::declaration_kind::record:
        case structural::declaration_kind::enumeration:
        case structural::declaration_kind::enumerator:
        case structural::declaration_kind::alias:
        case structural::declaration_kind::concept_declaration:
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
        case structural::declaration_kind::namespace_declaration:
            return entity_kind::namespace_;
        case structural::declaration_kind::record:
            switch (record.record_kind) {
                case structural::aggregate_kind::structure:
                    return entity_kind::struct_;
                case structural::aggregate_kind::class_record:
                    return entity_kind::class_;
                case structural::aggregate_kind::union_record:
                    return entity_kind::union_;
            }
            return entity_kind::struct_;
        case structural::declaration_kind::enumeration:
            return entity_kind::enum_;
        case structural::declaration_kind::enumerator:
            return entity_kind::enum_value;
        case structural::declaration_kind::alias:
            return entity_kind::type_alias;
        case structural::declaration_kind::concept_declaration:
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

[[nodiscard]] reference_entity from_structural(
    const structural::declaration_record& record,
    const std::filesystem::path& source_path) {
    reference_entity entity;
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
        parameter_description info;
        info.name = parameter.name;
        info.type = parameter.cpp_type;
        if (parameter.has_default) {
            info.default_value = parameter.default_expression;
        }
        entity.parameters.push_back(std::move(info));
    }
    return entity;
}

[[nodiscard]] cpp_reference_index from_structural_index(
    const structural::structural_index& structural_index) {
    cpp_reference_index index;
    index.clang_release = structural_index.clang_release;
    std::set<std::string, std::less<>> seen;
    for (const structural::translation_unit& unit :
         structural_index.translation_units) {
        for (const structural::declaration_record& record :
             unit.declarations) {
            if (!record.exported || !is_website_entity_kind(record.kind)) {
                continue;
            }
            reference_entity entity =
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
    unit.source_path = record.source_identity;
    unit.provided_module = record.provided_module;
    unit.exported_imports = record.exported_imports;
    unit.internal_imports = record.internal_imports;
    return unit;
}

}  // namespace

/// @brief Reports the Clang release linked into this generator.
/// @return The version string used to check the configured compiler version.
[[nodiscard]] std::string linked_clang_release() {
    return structural::linked_clang_release();
}

/// @brief Collects public declarations from in-memory C++ source for tests.
/// @param code A self-contained C++ module translation unit.
/// @param extra_args Compiler arguments, such as -std=c++26.
/// @return Exported declarations used for reference pages.
[[nodiscard]] cpp_reference_index scan_cpp_reference_source(
    std::string_view code, const std::vector<std::string>& extra_args) {
    structural::structural_index index;
    index.translation_units.push_back(
        structural::scan_translation_unit_from_code(
            "unit.cppm", code, extra_args));
    index.clang_release = index.translation_units.front().clang_release;
    return from_structural_index(index);
}

/// @brief Collects public C++ declarations from the selected module interfaces.
/// @param reachable_units Public interface files to parse.
/// @param compile_database Path to the Clang compilation database.
/// @return Exported declarations used for reference pages.
[[nodiscard]] cpp_reference_index scan_cpp_reference_units(
    const std::vector<std::filesystem::path>& reachable_units,
    const std::filesystem::path& compile_database) {
    const structural::structural_index index =
        structural::scan_translation_units_from_database(
            reachable_units, compile_database);
    if (index.translation_units.size() != reachable_units.size()) {
        throw std::runtime_error(
            "website C++ reference omitted source units: requested "
            + std::to_string(reachable_units.size()) + ", parsed "
            + std::to_string(index.translation_units.size()));
    }
    return from_structural_index(index);
}

/// @brief Collects the module and import information for each selected interface.
/// @param reachable_units Module interface files to scan.
/// @param compile_database Path to the Clang compilation database.
/// @return One module_graph_unit per scanned interface.
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

/// @brief Collects module imports from in-memory source for tests.
/// @param code A self-contained C++ module translation unit.
/// @param extra_args Compiler arguments, such as -std=c++26.
/// @return The source module and its imports.
[[nodiscard]] module_graph_unit scan_module_graph_from_code(
    std::string_view code, const std::vector<std::string>& extra_args) {
    return unit_from_structural(
        structural::scan_module_graph_from_code(code, extra_args));
}

}  // namespace bioseq::codegen::website
