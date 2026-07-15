module;

#include "clang_structural_shim.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:clang_entity_index;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Clang 22 structural/export index (§11.3), module-graph side. The Clang pass
// itself lives in clang_structural_impl.cpp behind the plain clang_shim header
// (Clang headers are not GCC-16 module-clean). This partition owns the model
// result types and translates the shim's POD records into documented_entity /
// pg_entrypoint_candidate so entity_index and site_builder see only model types.
namespace bioseq::codegen::website {

/// @brief Result of the Clang structural pass over the reachable public units.
struct clang_structural_index {
    std::vector<documented_entity> entities;
    std::vector<pg_entrypoint_candidate> pg_entrypoints;
    std::string clang_version;
};

namespace {

[[nodiscard]] entity_kind kind_from_shim(std::string_view kind) {
    if (kind == "namespace") { return entity_kind::namespace_; }
    if (kind == "struct") { return entity_kind::struct_; }
    if (kind == "class") { return entity_kind::class_; }
    if (kind == "union") { return entity_kind::union_; }
    if (kind == "enum") { return entity_kind::enum_; }
    if (kind == "enum_value") { return entity_kind::enum_value; }
    if (kind == "concept") { return entity_kind::concept_; }
    if (kind == "type_alias") { return entity_kind::type_alias; }
    if (kind == "variable") { return entity_kind::variable; }
    return entity_kind::function;
}

[[nodiscard]] documented_entity from_shim(const clang_shim::entity& record) {
    documented_entity entity;
    entity.domain = entity_domain::cpp;
    entity.kind = kind_from_shim(record.kind);
    entity.qualified_name = record.qualified_name;
    entity.display_name = record.display_name;
    entity.normalized_signature = record.normalized_signature;
    if (record.has_return_type) {
        entity.return_type = record.return_type;
    }
    if (record.has_module) {
        entity.module_name = record.module_name;
    }
    if (record.has_location) {
        source_span span;
        span.path = record.path;
        span.first_line = record.line;
        span.first_column = record.column;
        span.last_line = record.line;
        span.last_column = record.column;
        entity.declaration = span;
    }
    entity.attributes.emplace("exported", "true");
    if (!record.member_kind.empty()) {
        entity.attributes.emplace("member_kind", record.member_kind);
    }
    for (const clang_shim::parameter& parameter : record.parameters) {
        parameter_documentation info;
        info.name = parameter.name;
        info.type = parameter.type;
        if (parameter.has_default) {
            info.default_value = parameter.default_value;
        }
        entity.parameters.push_back(std::move(info));
    }
    return entity;
}

[[nodiscard]] clang_structural_index from_shim_index(const clang_shim::index& shim) {
    clang_structural_index index;
    index.clang_version = shim.clang_version;
    for (const clang_shim::entity& record : shim.entities) {
        index.entities.push_back(from_shim(record));
    }
    for (const clang_shim::pg_entry& record : shim.pg_entrypoints) {
        pg_entrypoint_candidate candidate;
        candidate.symbol = record.symbol;
        candidate.normalized_signature = record.normalized_signature;
        candidate.owning_module = record.owning_module;
        candidate.declaration.path = record.path;
        candidate.declaration.first_line = record.line;
        candidate.declaration.last_line = record.line;
        index.pg_entrypoints.push_back(std::move(candidate));
    }
    return index;
}

}  // namespace

/// @brief Reports the Clang version this generator is linked against.
/// @return The CLANG_VERSION_STRING for the §11.3 exact-version pin.
[[nodiscard]] std::string clang_api_version() {
    return clang_shim::clang_version();
}

/// @brief Runs the structural pass over in-memory module source (test entry).
/// @param code A self-contained C++ module translation unit.
/// @param extra_args Compiler arguments (for example -std=c++26).
/// @return The exported/public entities the pass records.
[[nodiscard]] clang_structural_index parse_clang_structural_index_from_code(
    std::string_view code, const std::vector<std::string>& extra_args) {
    return from_shim_index(
        clang_shim::run_structural_from_code(std::string(code), extra_args));
}

/// @brief Runs the Clang 22 structural/export index over reachable interfaces.
/// @param reachable_units Source-relative public interface files to parse.
/// @param compile_database Path to the Clang-produced compile_commands.json.
/// @return The structural index of exported/public entities.
[[nodiscard]] clang_structural_index parse_clang_structural_index(
    const std::vector<std::filesystem::path>& reachable_units,
    const std::filesystem::path& compile_database) {
    std::vector<std::string> units;
    units.reserve(reachable_units.size());
    for (const std::filesystem::path& unit : reachable_units) {
        units.push_back(unit.string());
    }
    return from_shim_index(
        clang_shim::run_structural_from_db(units, compile_database.string()));
}

namespace {

[[nodiscard]] module_graph_unit unit_from_shim(const clang_shim::module_edges& edges) {
    module_graph_unit unit;
    unit.source_path = edges.source_path;
    unit.provided_module = edges.provided_module;
    unit.exported_imports = edges.exported_imports;
    unit.internal_imports = edges.plain_imports;
    return unit;
}

}  // namespace

/// @brief Scans each reachable unit's provided module and import edges (§11.3).
/// @param reachable_units Module interface files to scan.
/// @param compile_database Path to the Clang-produced compile_commands.json.
/// @return One module_graph_unit per scanned interface unit.
[[nodiscard]] std::vector<module_graph_unit> scan_module_graph(
    const std::vector<std::filesystem::path>& reachable_units,
    const std::filesystem::path& compile_database) {
    std::vector<std::string> units;
    units.reserve(reachable_units.size());
    for (const std::filesystem::path& unit : reachable_units) {
        units.push_back(unit.string());
    }
    const clang_shim::module_graph graph =
        clang_shim::scan_module_edges_from_db(units, compile_database.string());
    std::vector<module_graph_unit> result;
    result.reserve(graph.units.size());
    for (const clang_shim::module_edges& edges : graph.units) {
        result.push_back(unit_from_shim(edges));
    }
    return result;
}

/// @brief Scans one in-memory unit's provided module and import edges (tests).
/// @param code A self-contained C++ module translation unit.
/// @param extra_args Compiler arguments (for example -std=c++26).
/// @return The single scanned unit's module/edge record.
[[nodiscard]] module_graph_unit scan_module_graph_from_code(
    std::string_view code, const std::vector<std::string>& extra_args) {
    const clang_shim::module_graph graph =
        clang_shim::scan_module_edges_from_code(std::string(code), extra_args);
    if (graph.units.empty()) {
        return {};
    }
    return unit_from_shim(graph.units.front());
}

}  // namespace bioseq::codegen::website
