module;

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.clang_structural;

export import :model;

export namespace bioseq::codegen::clang_structural {

/// Runs the Clang structural pass over one self-contained in-memory unit.
/// This is the contract-test entry point; production collection uses the exact
/// compile-database overload below.
[[nodiscard]] translation_unit scan_translation_unit_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = {});

/// Runs the authoritative Clang pass for the selected, already-configured
/// module units. The compile database supplies the exact target/profile flags.
[[nodiscard]] structural_index scan_translation_units_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Recovers provided-module and import edges with the same compile commands.
[[nodiscard]] module_graph scan_module_graph_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Contract-test entry point for module topology.
[[nodiscard]] module_graph_unit scan_module_graph_from_code(
    std::string_view source,
    const std::vector<std::string>& compiler_arguments);

/// Reports the linked Clang version used for structural collection.
[[nodiscard]] std::string clang_api_version();

/// Normalizes the selected raw CMake commands to the linked Clang profile,
/// builds the named-module closure into a content-keyed PCM root, and returns
/// the owned compilation database plus incremental-build evidence.
[[nodiscard]] prepared_compilation_environment
prepare_compilation_environment(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& raw_compile_database,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    const std::filesystem::path& pcm_root,
    std::string_view compiler_profile,
    std::string_view authority);

/// Verifies that an owned compile database and every mapped PCM still match
/// the PREPARE manifest, selected profile, authority, toolchain, and source
/// dependency closure before structural collection begins.
[[nodiscard]] prepared_compilation_validation
validate_prepared_compilation_environment(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    const std::filesystem::path& pcm_root,
    const std::filesystem::path& pcm_manifest,
    const std::filesystem::path& clang_binary,
    const std::filesystem::path& clang_scan_deps,
    const std::filesystem::path& llvm_config,
    const std::filesystem::path& compiler_module_inventory,
    std::string_view expected_clang_sha256,
    std::string_view expected_clang_scan_deps_sha256,
    std::string_view expected_llvm_config_sha256,
    std::string_view expected_compiler_module_inventory_sha256,
    std::string_view compiler_profile,
    std::string_view authority);

/// Source-only entry points for isolated collector contract tests and fixture
/// probes. Production collection must use the compile-database APIs above.
[[nodiscard]] translation_unit scan_translation_unit(
    const std::filesystem::path& source_path,
    std::string_view source);

[[nodiscard]] translation_unit scan_translation_unit(
    const std::filesystem::path& source_path);

}  // namespace bioseq::codegen::clang_structural
