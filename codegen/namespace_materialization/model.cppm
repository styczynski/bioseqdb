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

/// Revisions of the stable JSON formats emitted by this generator.
inline constexpr std::uint32_t topology_format_revision = 1;
inline constexpr std::uint32_t artifact_format_revision = 3;
inline constexpr std::uint32_t publication_format_revision = 1;

/// Stable identity written into generated source and metadata.
inline constexpr std::string_view materialization_generator_identity =
    "bioseq-namespace-materialization-v1";

/// Role played by one independent materialization axis.
enum class axis_role : std::uint8_t {
    identity,
    shard,
    external,
};

/// One closed replacement spelling accepted for an axis.
struct replacement_case {
    std::string label;
    std::string spelling;
    std::uint32_t ordinal{};
};

/// Compiler-derived declaration bound to a materialization axis.
struct binding_record {
    structural_analysis::declaration_record declaration;
    axis_role role{axis_role::identity};
    std::string group;
    std::vector<replacement_case> cases;
};

/// Cases that select a controlled declaration for one axis group.
struct selection_clause {
    std::string group;
    std::vector<std::string> accepted_case_labels;
};

/// Declaration owned by the generated module topology.
struct owned_declaration {
    structural_analysis::declaration_record declaration;
    std::string entry_role;
    std::vector<selection_clause> clauses;
    bool generated_api_entry{};
};

/// Authored namespace boundary and declarations controlled by generation.
struct materialization_region {
    structural_analysis::module_record source_module;
    std::string configured_output_module_name;
    structural_analysis::namespace_record boundary;
    std::optional<structural_analysis::namespace_record> support_island;
    std::vector<binding_record> bindings;
    std::vector<owned_declaration> controlled_declarations;
};

/// Zipped bindings that form one logical materialization axis.
struct axis_group {
    std::string name;
    axis_role role{axis_role::identity};
    std::vector<replacement_case> cases;
    std::vector<std::size_t> binding_indices;
};

/// Selected case for one axis at one logical coordinate.
struct axis_case_assignment {
    std::string group;
    axis_role role{axis_role::identity};
    std::string case_label;
    std::uint32_t case_ordinal{};
};

/// Stable identity and output location for one materialized specialization.
struct materialization_coordinate {
    std::uint32_t ordinal{};
    std::vector<axis_case_assignment> assignments;
    std::string identity_namespace;
    std::string module_name;
    std::filesystem::path relative_path;
    std::string topology_digest;
};

/// Exact byte replacement against an authenticated source slice.
struct source_edit {
    std::size_t begin_byte{};
    std::size_t end_byte{};
    std::string replacement;
    std::string source_identity;
    std::string purpose;
};

/// A coordinate belongs either to the explicit identity/shard product or to
/// one policy-supplied external axis. The explicit grammar intentionally does
/// not expose the external role; Highway is its sole policy adapter.
void validate_coordinate_axis_domain(
    const materialization_coordinate& coordinate);

/// Applies policy-supplied edits against one authenticated source slice.
/// Both explicit-axis materialization and Highway rendering use this owner.
[[nodiscard]] std::string apply_exact_edits(
    std::string source,
    std::vector<source_edit> edits,
    std::size_t source_origin = 0u);

/// Whether validation may compile a generated artifact body.
enum class validation_class : std::uint8_t {
    body_free,
    body_bearing,
};

/// One generated module or metadata artifact.
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

/// Frontend actions directly observed by the generate command.
///
/// Counts exclude later CMake scans and compiler invocations and do not infer
/// work performed by the earlier prepare command.
struct frontend_action_counts {
    std::uint32_t dependency_preparation_validations{};
    std::uint32_t canonical_token_slice_scans{};
    std::uint32_t signature_donor_token_slice_scans{};
    std::uint32_t body_free_declaration_probe_scans{};
    std::uint32_t artifact_render_passes{};
    std::uint32_t body_bearing_compiler_invocations{};
};

/// Generated module units CMake is subsequently obligated to scan and
/// compile. These are topology-derived obligations, not observed invocations.
struct cmake_compilation_obligations {
    std::uint32_t api_partition_units{};
    std::uint32_t selector_partition_units{};
    std::uint32_t body_bearing_implementation_units{};
};

/// Closed logical topology derived from the authored source.
struct topology_plan {
    materialization_region region;
    std::vector<axis_group> groups;
    std::vector<materialization_coordinate> coordinates;
    std::vector<std::uint32_t> identity_representative_ordinals;
    std::string canonical_digest_input;
    std::string topology_digest;
};

/// Complete authenticated input to rendering and publication.
struct generation_plan {
    topology_plan topology;
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

/// Generated artifacts and their sealed metadata.
struct generation_result {
    std::vector<materialization_artifact> artifacts;
    std::string topology_manifest_json;
    std::string artifact_manifest_json;
    std::string manifest_identity;
};

/// Closed ownership class used by policy-neutral publication.
enum class publication_artifact_class : std::uint8_t {
    production_cppm,
    metadata_json,
};

/// A policy-neutral publication record. Both explicit-axis and Highway
/// frontends submit their complete desired set through this one API.
struct publication_artifact {
    std::filesystem::path relative_path;
    std::string content;
    std::string content_sha256;
    publication_artifact_class artifact_class{
        publication_artifact_class::production_cppm};
};

/// Inputs to the one policy-neutral, self-sealed ownership envelope.
/// `policy_evidence_json` is an already policy-validated JSON object. The
/// publisher authenticates it as part of the envelope bytes but deliberately
/// does not interpret it when authorizing stale cleanup.
struct publication_manifest_request {
    std::filesystem::path output_root;
    std::string policy;
    std::string name;
    std::string topology_digest;
    std::string prior_manifest_identity;
    std::string policy_evidence_json;
    std::vector<publication_artifact> artifacts;
};

/// Atomic publication request for artifacts and their ownership envelope.
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

/// Test-observable checkpoints in the atomic prepare publication sequence.
enum class prepare_publication_checkpoint : std::uint8_t {
    after_environment_built,
    after_db_manifest_publish,
    before_completion_stamp,
};

/// Observer invoked at a prepare-publication checkpoint.
using prepare_publication_checkpoint_callback =
    std::function<void(prepare_publication_checkpoint)>;

/// Optional observer installed only by the in-module contract partition.
inline thread_local prepare_publication_checkpoint_callback
    prepare_publication_test_checkpoint_callback;

/// Installs a contract observer for prepare-publication checkpoints.
inline void set_prepare_publication_test_checkpoint(
    prepare_publication_checkpoint_callback callback) {
    prepare_publication_test_checkpoint_callback =
        std::move(callback);
}

/// Removes the current contract observer.
inline void clear_prepare_publication_test_checkpoint() {
    prepare_publication_test_checkpoint_callback = {};
}

/// Notifies the contract observer when one is installed.
inline void invoke_prepare_publication_test_checkpoint(
    prepare_publication_checkpoint checkpoint) {
    if (prepare_publication_test_checkpoint_callback) {
        prepare_publication_test_checkpoint_callback(checkpoint);
    }
}

/// Contract-only bridge implemented by the primary module after its contract
/// partition is imported. Production callers use `run`.
[[nodiscard]] int execute_prepare_contract(
    prepare_command_line command);

/// Parsed arguments for namespace materialization generation.
struct command_line {
    std::string name;
    std::filesystem::path source;
    std::filesystem::path repository_root;
    std::string output_module;
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
