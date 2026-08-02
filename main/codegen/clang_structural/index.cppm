module;

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.clang_structural;

export import :model;

export namespace bioseq::codegen::clang_structural {

/// Performs the production syntax-only collection pass. This API uses Clang's
/// raw lexer but never creates a CompilerInstance, runs Sema, emits a BMI, or
/// asks PREPARE to compile the body-bearing source.
[[nodiscard]] translation_unit scan_token_slice(
    const std::filesystem::path& source_path,
    std::string_view source);

[[nodiscard]] translation_unit scan_token_slice(
    const std::filesystem::path& source_path);

/// Semantically validates a generated body-free declaration surrogate. The
/// call fails if a function body or default member initializer survives in the
/// probe and records declaration-probe provenance/linkage/module facts.
[[nodiscard]] translation_unit scan_declaration_probe_from_code(
    const std::filesystem::path& source_path,
    std::string_view body_free_source,
    const std::vector<std::string>& compiler_arguments,
    std::string_view compiler_profile = {});

/// Semantically validates a generated body-free declaration surrogate using
/// the prepared command for a distinct primary-interface signature donor.
/// The donor supplies compiler/module flags; its source argument is retargeted
/// to the probe and no BMI/object is emitted.
[[nodiscard]] translation_unit scan_declaration_probe_from_database(
    const std::filesystem::path& probe_source_path,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

/// In-memory form used before authenticated artifact publication. The virtual
/// probe path participates in diagnostics/module identity, but no probe file
/// is created outside the shared publisher.
[[nodiscard]] translation_unit scan_declaration_probe_from_database(
    const std::filesystem::path& probe_source_path,
    std::string_view body_free_source,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile = {},
    const std::filesystem::path& pcm_root = {});

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

/// Derives raw commands for canonical sources from one attached primary
/// interface, then delegates to ordinary PREPARE. Unlike the dependency-only
/// API below, every canonical source is a compiler root and receives an owned
/// content-keyed BMI in the isolated closure.
[[nodiscard]] prepared_compilation_environment
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

/// Prepares an attached primary donor plus the providers imported by a
/// canonical scan-only source. The canonical body-bearing input is tokenized,
/// is never selected as a PREPARE root, and is never passed to a compiler
/// action. The donor must be distinct from the canonical source.
[[nodiscard]] prepared_compilation_environment
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

/// Vector form for multi-source raw-token materialization policies. The union
/// donor/import-provider closure is prepared once; every canonical source
/// remains a raw-token-only input excluded from compiler roots and commands.
[[nodiscard]] prepared_compilation_environment
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

/// Reconstructs the exact donor-plus-import-provider root authority from raw
/// canonical tokens and either the configured raw database (PREPARE) or the
/// isolated owned database (validation).
[[nodiscard]] dependency_compilation_authority
derive_dependency_compilation_authority(
    const std::vector<std::filesystem::path>& canonical_source_paths,
    const std::filesystem::path& signature_source_path,
    const std::filesystem::path& compile_database,
    std::string_view compiler_profile);

/// Requires every derived dependency root command to carry the exact current
/// canonical token/import digest sealed by dependency-only PREPARE.
void validate_dependency_compilation_authority_seal(
    const dependency_compilation_authority& authority,
    const std::filesystem::path& compile_database);

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
