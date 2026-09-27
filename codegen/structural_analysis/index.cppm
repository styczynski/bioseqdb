module;

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.structural_analysis;

export import :model;

export namespace bioseq::codegen::structural_analysis {

/// Returns a repository-relative source path using forward slashes.
/// Absolute inputs are shortened at a recognized repository directory.
/// Rejects parent traversal and unsupported source prefixes.
[[nodiscard]] std::string repository_source_identity(
    const std::filesystem::path& source_path);

/// Reads source tokens and declarations without compiling function bodies.
/// Uses Clang's raw lexer without a preprocessor or semantic analysis, and
/// produces no precompiled module.
[[nodiscard]] translation_unit scan_token_slice(
    const std::filesystem::path& source_path,
    std::string_view source);

[[nodiscard]] translation_unit scan_token_slice(
    const std::filesystem::path& source_path);

/// Uses Clang to check a generated declaration-only source.
/// Rejects remaining function bodies or default member initializers, then
/// records declaration identities, linkage, and module information.
[[nodiscard]] translation_unit scan_declaration_probe_from_code(
    const std::filesystem::path& source_path,
    std::string_view body_free_source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = {});

/// Checks a declaration-only source using compiler flags from a separate
/// prepared primary module. Retargets that module's command to the probe
/// without writing a precompiled module or object file.
[[nodiscard]] translation_unit scan_declaration_probe_from_database(
    const std::filesystem::path& probe_source_path,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Checks declaration-only text in memory before the generated file is published.
/// The virtual path supplies its source identity and diagnostic filename.
[[nodiscard]] translation_unit scan_declaration_probe_from_database(
    const std::filesystem::path& probe_source_path,
    std::string_view body_free_source,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Runs Clang on a self-contained source string for contract tests.
/// Production scans use the compilation-database overload below.
[[nodiscard]] translation_unit scan_translation_unit_from_code(
    const std::filesystem::path& source_path,
    std::string_view source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = {});

/// Collects declarations from selected modules using their configured compiler commands.
[[nodiscard]] structural_index scan_translation_units_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Collects declarations and active module imports from the same Clang AST
/// for each selected source.
[[nodiscard]] structural_module_graph_scan
scan_translation_units_and_module_graph_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Collects provided modules and active imports using the configured compiler commands.
[[nodiscard]] module_graph scan_module_graph_from_database(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// Collects a module's imports from in-memory source for contract tests.
[[nodiscard]] module_graph_unit scan_module_graph_from_code(
    std::string_view source,
    const std::vector<std::string>& compiler_arguments);

/// Returns the linked Clang release used for structural collection.
[[nodiscard]] std::string linked_clang_release();

/// Prepares selected CMake compiler commands for the linked Clang version
/// and builds the selected modules and their dependencies. Returns a separate
/// compilation database, cached precompiled modules, and dependency records.
[[nodiscard]] dependency_compilation_environment
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

/// Copies compiler settings from a primary module to the requested sources,
/// then prepares each source and its dependencies. Every requested source
/// is compiled into a cached module file.
[[nodiscard]] dependency_compilation_environment
prepare_compilation_environment(
    const std::vector<std::filesystem::path>& source_paths,
    const std::filesystem::path& signature_source_path,
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

/// Prepares a separate signature module and the modules imported by the
/// source being generated. That source is read as tokens only and must
/// differ from the signature module.
[[nodiscard]] dependency_compilation_environment
prepare_dependency_compilation_environment(
    const std::filesystem::path& canonical_source_path,
    const std::filesystem::path& signature_source_path,
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

/// Prepares shared dependencies for several sources in one pass.
/// Reads the original sources as tokens and compiles only the separate
/// signature module and imported module providers.
[[nodiscard]] dependency_compilation_environment
prepare_dependency_compilation_environment(
    const std::vector<std::filesystem::path>& canonical_source_paths,
    const std::filesystem::path& signature_source_path,
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

/// Finds the signature module and imported providers that must be compiled.
/// Uses source tokens with either the original or prepared compilation database.
[[nodiscard]] dependency_compilation_authority
derive_dependency_compilation_authority(
    const std::vector<std::filesystem::path>& canonical_source_paths,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile);

/// Adds the source-token and import hash to the PREPARE authority string.
[[nodiscard]] std::string dependency_compilation_manifest_authority(
    std::string_view base_authority,
    const dependency_compilation_authority& authority);

/// Checks that the prepared database matches the required dependency roots.
/// Pass dependency_compilation_manifest_authority to manifest validation
/// as well to detect changed source tokens or imports.
void validate_dependency_compilation_roots(
    const dependency_compilation_authority& authority,
    const std::filesystem::path& compile_database);

/// Renders a hashed record of the scans and compilations observed during PREPARE.
/// This diagnostic trace is excluded from generated-file and compilation identities.
[[nodiscard]] std::string render_frontend_action_trace(
    const dependency_compilation_environment& environment);

/// Checks the PREPARE trace against the verified module files.
/// Each original source must have one token scan and no compiler analysis.
[[nodiscard]] frontend_trace_validation
validate_frontend_action_trace(
    const std::filesystem::path& action_trace,
    const dependency_compilation_authority& dependency_authority,
    const dependency_compilation_validation& environment);

/// Checks prepared compiler commands and module files against their manifest.
/// The build configuration, tool hashes, and source dependencies must still match.
[[nodiscard]] dependency_compilation_validation
validate_dependency_compilation_environment(
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

/// Reads source directly for isolated collector tests and fixture checks.
/// Production collection uses the compilation-database APIs above.
[[nodiscard]] translation_unit scan_translation_unit(
    const std::filesystem::path& source_path,
    std::string_view source);

[[nodiscard]] translation_unit scan_translation_unit(
    const std::filesystem::path& source_path);

}  // namespace bioseq::codegen::structural_analysis
