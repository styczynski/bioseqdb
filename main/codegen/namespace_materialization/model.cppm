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

import bioseq.codegen.clang_structural;

export namespace bioseq::codegen::namespace_materialization {

inline constexpr std::uint32_t topology_schema_version = 1;
inline constexpr std::uint32_t artifact_schema_version = 3;
inline constexpr std::uint32_t publication_schema_version = 1;
inline constexpr std::string_view generator_version =
    "bioseq-namespace-materialization-v1";

enum class axis_role : std::uint8_t {
    identity,
    shard,
};

struct replacement_case {
    std::string label;
    std::string spelling;
    std::uint32_t ordinal{};
};

struct binding_record {
    clang_structural::declaration_record declaration;
    axis_role role{axis_role::identity};
    std::string group;
    std::vector<replacement_case> cases;
};

struct selection_clause {
    std::string group;
    std::vector<std::string> accepted_case_labels;
};

struct owned_declaration {
    clang_structural::declaration_record declaration;
    std::string entry_role;
    std::vector<selection_clause> clauses;
    bool generated_api_entry{};
};

struct materialization_region {
    clang_structural::module_record source_module;
    std::string configured_output_module_name;
    clang_structural::namespace_record boundary;
    std::optional<clang_structural::namespace_record> support_island;
    std::vector<binding_record> bindings;
    std::vector<owned_declaration> controlled_declarations;
};

struct axis_group {
    std::string name;
    axis_role role{axis_role::identity};
    std::vector<replacement_case> cases;
    std::vector<std::size_t> binding_indices;
};

struct axis_case_assignment {
    std::string group;
    axis_role role{axis_role::identity};
    std::string case_label;
    std::uint32_t case_ordinal{};
};

struct materialization_coordinate {
    std::uint32_t ordinal{};
    std::vector<axis_case_assignment> assignments;
    std::string identity_namespace;
    std::string module_name;
    std::filesystem::path relative_path;
    std::string topology_digest;
};

struct source_edit {
    std::size_t begin_byte{};
    std::size_t end_byte{};
    std::string replacement;
    std::string provenance_id;
    std::string purpose;
};

enum class validation_class : std::uint8_t {
    body_free,
    body_bearing,
};

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

/// Actions directly observed by the namespace-materialization generate
/// command. This ledger must never include later CMake module scans or
/// compiler invocations, nor infer actions performed by the earlier prepare
/// command merely because its authenticated outputs were validated.
struct frontend_action_ledger {
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

struct topology_plan {
    materialization_region region;
    std::vector<axis_group> groups;
    std::vector<materialization_coordinate> coordinates;
    std::vector<std::uint32_t> identity_representative_ordinals;
    std::string canonical_digest_input;
    std::string topology_digest;
};

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
    std::string clang_version;
    std::string compilation_signature;
    std::string compiler_module_inventory_sha256;
    std::string previous_manifest_identity;
    std::vector<clang_structural::translation_unit> declaration_probes;
    frontend_action_ledger frontend_actions;
    cmake_compilation_obligations compilation_obligations;
};

struct generation_result {
    std::vector<materialization_artifact> artifacts;
    std::string topology_manifest_json;
    std::string artifact_manifest_json;
    std::string manifest_identity;
};

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

struct publication_request {
    std::filesystem::path output_root;
    std::filesystem::path artifact_manifest;
    std::filesystem::path stamp;
    std::string artifact_manifest_json;
    std::vector<publication_artifact> artifacts;
};

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

enum class prepare_publication_checkpoint : std::uint8_t {
    after_environment_built,
    after_db_manifest_publish,
    before_completion_stamp,
};

using prepare_publication_checkpoint_callback =
    std::function<void(prepare_publication_checkpoint)>;

inline thread_local prepare_publication_checkpoint_callback
    prepare_publication_test_checkpoint_callback;

inline void set_prepare_publication_test_checkpoint(
    prepare_publication_checkpoint_callback callback) {
    prepare_publication_test_checkpoint_callback =
        std::move(callback);
}

inline void clear_prepare_publication_test_checkpoint() {
    prepare_publication_test_checkpoint_callback = {};
}

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
