module;

#include <filesystem>
#include <string>
#include <vector>

module bioseq.codegen.website:clang_entity_index;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Clang 22 structural/export index (§11.3). The full LibTooling pass — which
// links Clang 22 and consumes the main compilation database/BMI graph — is
// added in Phase 6; this partition currently owns the source-neutral result
// types and the merge entrypoint so entity_index and site_builder compile
// against a stable contract. Clang/AST headers will live in this unit's global
// module fragment when the pass is implemented.
namespace bioseq::codegen::website {

/// @brief Result of the Clang structural pass over the reachable public units.
struct clang_structural_index {
    std::vector<documented_entity> entities;
    std::vector<pg_entrypoint_candidate> pg_entrypoints;
    std::string clang_version;
};

/// @brief Runs the Clang 22 structural/export index over reachable interfaces.
/// @param reachable_units Source-relative public interface files to parse.
/// @param compile_database Path to the Clang-produced compile_commands.json.
/// @return The structural index; currently empty pending the Phase 6 pass.
[[nodiscard]] clang_structural_index parse_clang_structural_index(
    const std::vector<std::filesystem::path>& reachable_units,
    const std::filesystem::path& compile_database) {
    clang_structural_index index;
    (void)reachable_units;
    (void)compile_database;
    return index;
}

}  // namespace bioseq::codegen::website
