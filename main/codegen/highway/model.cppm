module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

export module bioseq.codegen.highway:model;

export namespace bioseq::codegen::highway {

inline constexpr std::uint32_t manifest_schema_version = 1;

struct target_spec {
    std::string platform;
    std::string slug;
    std::string token;
    std::string name_space;
    std::uint64_t bit{};
};

struct source_spec {
    std::string key;
    std::filesystem::path path;
    std::string module_name;
    std::string partition;
    std::string family;
    std::vector<std::string> dependencies;
};

struct slot_spec {
    std::string owner_key;
    std::string family_field;
    std::string slot_field;
    std::string expression;
};

struct required_slot_spec {
    std::string family_field;
    std::string slot_field;
};

/** Ordered external domain topology supplied by configure-time authority. */
struct domain_topology_manifest_input {
    std::string name;
    std::filesystem::path path;
};

/** Validated external-domain provenance; never a Highway target axis. */
struct domain_topology_provenance {
    std::string name;
    std::filesystem::path path;
    std::string topology_digest;
    std::string content_sha256;
};

/** Configure-sealed expectation repeated in target and source manifests. */
using domain_topology_expectation = domain_topology_provenance;

struct target_manifest {
    std::string policy_digest;
    std::vector<std::pair<std::string, std::string>> policy_definitions;
    std::string highway_version;
    std::string highway_digest;
    std::string static_target_token;
    std::vector<target_spec> targets;
    std::vector<domain_topology_expectation> domain_topologies;
};

struct source_manifest {
    std::string contract_module;
    std::string dispatch_module;
    bool require_complete_table{};
    std::vector<source_spec> sources;
    std::vector<slot_spec> slots;
    std::vector<required_slot_spec> required_slots;
    std::vector<domain_topology_expectation> domain_topologies;
};

struct command_line {
    std::filesystem::path target_manifest_path;
    std::filesystem::path source_manifest_path;
    std::filesystem::path compile_database;
    std::filesystem::path pcm_manifest;
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
    std::filesystem::path artifact_manifest;
    std::filesystem::path stamp;
    std::vector<domain_topology_manifest_input> domain_topology_manifests;
};

struct prepare_command_line {
    std::filesystem::path source_manifest_path;
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
    std::filesystem::path depfile;
    std::filesystem::path stamp;
    std::vector<domain_topology_manifest_input> domain_topology_manifests;
};

struct callable_insertion {
    std::size_t body_relative_offset{};
    std::string qualified_name;
    std::string kind;
};

struct extracted_source {
    source_spec source;
    std::string enclosing_namespace;
    std::string body;
    std::size_t body_first_line{1};
    std::string source_sha256;
    std::vector<std::string> neutral_imports;
    std::vector<callable_insertion> callables;
};

enum class artifact_kind : std::uint8_t {
    primary,
    binder,
    partition,
    dispatch_registry_partition,
    dispatch_inventory_implementation,
    test_support,
};

struct generated_artifact {
    std::filesystem::path relative_path;
    std::string module_name;
    std::string target_token;
    std::string source_key;
    artifact_kind kind{artifact_kind::partition};
    std::string content;
};

struct generation_plan {
    target_manifest target_inventory;
    source_manifest source_inventory;
    std::vector<extracted_source> extracted_sources;
    std::string clang_version;
    std::string compilation_signature;
    std::vector<domain_topology_provenance> domain_topologies;
};

struct generation_result {
    std::vector<generated_artifact> artifacts;
    std::string artifact_manifest_json;
};

}  // namespace bioseq::codegen::highway
