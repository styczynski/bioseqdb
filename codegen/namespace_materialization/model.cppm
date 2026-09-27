module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.namespace_materialization:model;

import bioseq.codegen.structural_analysis;

export namespace bioseq::codegen::namespace_materialization {

/// Versions of the JSON formats written by this generator.
inline constexpr std::uint32_t topology_schema_revision = 2;
inline constexpr std::uint32_t artifact_schema_revision = 4;
inline constexpr std::uint32_t publication_schema_revision = 1;

/// Generator name written into source files and metadata.
inline constexpr std::string_view materialization_generator_identity =
    "bioseq-namespace-materialization";

/// How a  choice affects generated code: specialization identity, source split or external target.
enum class axis_role : std::uint8_t {
    identity,
    shard,
    external,
};

/// Chooses generated selectors or implementation files for existing declarations.
enum class emission_policy : std::uint8_t {
    selector_api,
    implementation_owners,
};

/// Returns the emission-policy name stored in manifests.
[[nodiscard]] inline constexpr std::string_view spell_emission_policy(
    emission_policy policy) noexcept {
    switch (policy) {
        case emission_policy::selector_api: return "selector_api";
        case emission_policy::implementation_owners:
            return "implementation_owners";
    }
    return {};
}

/// A named replacement value for one generation choice.
struct replacement_case {
    std::string label;
    std::string spelling;
    std::uint32_t ordinal{};
};

/// A source declaration and the replacement cases ahat can be generated for it.
struct binding_record {
    structural_analysis::declaration_record declaration;
    axis_role role{axis_role::identity};
    std::string group;
    std::vector<replacement_case> cases;
};

/// Cases in one group for which a declaration should be included.
struct selection_clause {
    std::string group;
    std::vector<std::string> accepted_case_labels;
};

/// A declaration selected for generated files by its case conditions.
struct owned_declaration {
    structural_analysis::declaration_record declaration;
    std::string entry_role;
    std::vector<selection_clause> clauses;
    bool generated_api_entry{};
};

/// The source namespace to copy  and the declarations controlled by its annotations.
struct materialization_region {
    structural_analysis::module_record source_module;
    std::string configured_output_module_name;
    emission_policy policy{emission_policy::selector_api};
    structural_analysis::namespace_record boundary;
    std::optional<structural_analysis::namespace_record> support_island;
    std::vector<binding_record> bindings;
    std::vector<owned_declaration> controlled_declarations;
};

/// Bindings that select the same case label together, in source order.
struct axis_group {
    std::string name;
    axis_role role{axis_role::identity};
    std::vector<replacement_case> cases;
    std::vector<std::size_t> binding_indices;
};

/// The case chosen for one group in a generated specialization.
struct axis_case_assignment {
    std::string group;
    axis_role role{axis_role::identity};
    std::string case_label;
    std::uint32_t case_ordinal{};
};

/// Case choices, namespace and output path for one generated specialization
struct materialization_coordinate {
    std::uint32_t ordinal{};
    std::vector<axis_case_assignment> assignments;
    std::string identity_namespace;
    std::string module_name;
    std::filesystem::path relative_path;
    std::string topology_digest;
};

/// A replacement of a byte range in the source text.
struct source_edit {
    std::size_t begin_byte{};
    std::size_t end_byte{};
    std::string replacement;
    std::string source_identity;
    std::string purpose;
};

/// Checks that a specialization uses identity/shard choices or one external
/// target choice. Highway supplies the external target through its generator
/// adapter. Source annotations accept  identity and shard roles.
void validate_coordinate_axis_domain(
    const materialization_coordinate& coordinate);

/// Applies byte-range edits after checking that they fit the source slice
/// and do not overlap. Namespace and Highway generators share this function.
[[nodiscard]] std::string apply_exact_edits(
    std::string source,
    std::vector<source_edit> edits,
    std::size_t source_origin = 0u);

/// Records whether an artifact contains implementation bodies.
enum class validation_class : std::uint8_t {
    body_free,
    body_bearing,
};

/// A generated C++ module or metadata file, with its contents and hash.
struct materialization_artifact {
    enum class kind : std::uint8_t {
        declaration_probe,
        api_partition,
        implementation_unit,
        selector_partition,
        manifest,
    };

    kind artifact_kind{kind::implementation_unit};
    std::filesystem::path relative_path;
    std::string module_name;
    std::vector<std::uint32_t> logical_coordinate_ordinals;
    validation_class validation{validation_class::body_bearing};
    std::string content;
    std::string content_sha256;
};

/// Compiler and rendering work recorded for generation.
///
/// Scan and render counts describe the generate command. The count of uompiler
/// actions on implementation bodies comes from the verified prepare  trace.
/// CMake's later scans and compilations are recorded separately.
struct frontend_action_counts {
    std::uint32_t dependency_preparation_validations{};
    std::uint32_t canonical_token_slice_scans{};
    std::uint32_t signature_donor_token_slice_scans{};
    std::uint32_t body_free_declaration_probe_scans{};
    std::uint32_t artifact_render_passes{};
    std::uint32_t body_bearing_compiler_invocations{};
};

/// Generated module files that CMake must, scan and compile. These counts
/// describe required build work, without claiming that it has already run.
struct cmake_compilation_obligations {
    std::uint32_t api_partition_units{};
    std::uint32_t selector_partition_units{};
    std::uint32_t body_bearing_implementation_units{};
};

/// The generated combinations of source choices and their shared topology hash.
struct topology_plan {
    materialization_region region;
    std::vector<axis_group> groups;
    std::vector<materialization_coordinate> coordinates;
    std::vector<std::uint32_t> identity_representative_ordinals;
    std::string canonical_digest_input;
    std::string topology_digest;
};

/// Source, compiler checks and selected specializations used to generate output.
struct generation_plan {
    topology_plan topology;
    emission_policy policy{emission_policy::selector_api};
    std::filesystem::path repository_root;
    std::filesystem::path source_path;
    std::filesystem::path output_root;
    std::string source_text;
    std::string source_sha256;
    std::string name;
    std::string profile;
    std::vector<std::uint32_t> enabled_ordinals;
    std::string clang_release;
    std::string compilation_signature;
    std::string compiler_module_inventory_sha256;
    std::string previous_manifest_identity;
    std::vector<structural_analysis::translation_unit> declaration_probes;
    frontend_action_counts frontend_actions;
    cmake_compilation_obligations compilation_obligations;
};

/// Generated files and the manifests that record their identities.
struct generation_result {
    std::vector<materialization_artifact> artifacts;
    std::string topology_manifest_json;
    std::string artifact_manifest_json;
    std::string manifest_identity;
};

/// File types accepted by the shared output publisher.
enum class publication_artifact_class : std::uint8_t {
    production_cppm,
    metadata_json,
};

/// A file to write through the shared publisher. Namespace and Highway
/// generators submit their full output lists using these records.
struct publication_artifact {
    std::filesystem::path relative_path;
    std::string content;
    std::string content_sha256;
    publication_artifact_class artifact_class{
        publication_artifact_class::production_cppm};
};

/// Inputs for the output-ownership manifest  and its integrity hash
/// The generator validates policy_evidence_json before passing it here. The
/// publisher includes that object in the hash, while file ownership records
/// determine which obsolete files it may remove.
struct publication_manifest_request {
    std::filesystem::path output_root;
    std::string policy;
    std::string name;
    std::string topology_digest;
    std::string prior_manifest_identity;
    std::string policy_evidence_json;
    std::vector<publication_artifact> artifacts;
};

/// Files and ownership metadata to publish through the recoverable reame protocol.
struct publication_request {
    std::filesystem::path output_root;
    std::filesystem::path artifact_manifest;
    std::filesystem::path stamp;
    std::string artifact_manifest_json;
    std::vector<publication_artifact> artifacts;
};

/// Parsed arguments for dependency preparation.
struct prepare_command_line {
    std::filesystem::path source;
    std::filesystem::path signature_source;
    std::filesystem::path raw_compile_database;
    std::filesystem::path clang_binary;
    std::filesystem::path clang_scan_deps;
    std::filesystem::path llvm_config;
    std::filesystem::path compiler_module_inventory;
    std::string clang_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::filesystem::path pcm_root;
    std::filesystem::path compile_database_output;
    std::filesystem::path pcm_manifest;
    std::filesystem::path action_trace;
    std::filesystem::path depfile;
    std::filesystem::path stamp;
};

/// Test checkpoints while prepared compiler files are being published.
enum class prepare_publication_checkpoint : std::uint8_t {
    after_environment_built,
    after_db_manifest_publish,
    before_completion_stamp,
};

/// Callback invoked at a prepare-publication test checkpoint.
using prepare_publication_checkpoint_callback =
    std::function<void(prepare_publication_checkpoint)>;

/// Optional checkpoint callback used by the generator's contract tests.
inline thread_local prepare_publication_checkpoint_callback
    prepare_publication_test_checkpoint_callback;

/// Sets the callback for prepare-publication  test checkpoints.
inline void set_prepare_publication_test_checkpoint(
    prepare_publication_checkpoint_callback callback) {
    prepare_publication_test_checkpoint_callback =
        std::move(callback);
}

/// Clears the prepare-publication test callback.
inline void clear_prepare_publication_test_checkpoint() {
    prepare_publication_test_checkpoint_callback = {};
}

/// Calls the prepare-publication test callback if one is installed.
inline void invoke_prepare_publication_test_checkpoint(
    prepare_publication_checkpoint checkpoint) {
    if (prepare_publication_test_checkpoint_callback) {
        prepare_publication_test_checkpoint_callback(checkpoint);
    }
}

/// Runs preparation for in-module contract tests. Normal command-line
/// callers enter through run().
[[nodiscard]] int execute_prepare_contract(
    prepare_command_line command);

/// Parsed arguments for generating namespace specializations.
struct command_line {
    std::string name;
    std::filesystem::path source;
    std::filesystem::path repository_root;
    std::string output_module;
    emission_policy policy{emission_policy::selector_api};
    std::string profile;
    std::filesystem::path configure_topology_manifest;
    std::string expected_topology_digest;
    std::vector<std::uint32_t> enabled_ordinals;
    std::filesystem::path signature_source;
    std::filesystem::path compile_database;
    std::filesystem::path pcm_manifest;
    std::filesystem::path prepare_action_trace;
    std::filesystem::path prepare_stamp;
    std::filesystem::path pcm_root;
    std::filesystem::path clang_binary;
    std::filesystem::path clang_scan_deps;
    std::filesystem::path llvm_config;
    std::filesystem::path compiler_module_inventory;
    std::string clang_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::filesystem::path output_root;
    std::filesystem::path topology_manifest;
    std::filesystem::path artifact_manifest;
    std::filesystem::path depfile;
    std::filesystem::path stamp;
};

}  // namespace bioseq::codegen::namespace_materialization
