module;

#include <algorithm>
#include <array>
#include <bit>
#include <set>
#include <span>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.highway:model;

import bioseq.sequences.kmers.minimizer_layout_geometry;
import bioseq.codegen.structural_analysis;

export namespace bioseq::codegen::highway {

inline constexpr std::uint32_t policy_evidence_schema_revision = 1;
inline constexpr std::string_view publication_policy = "highway";
inline constexpr std::string_view publication_name = "highway";

struct target_spec {
    std::string platform;
    std::string slug;
    std::string token;
    std::string target_namespace;
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

/** Template for a C++ implementation file used only by unit tests. */
struct probe_spec {
    std::string key;
    std::filesystem::path path;
};

enum class minimizer_registry_profile : std::uint8_t {
    baseline,
    extended,
};

/** A minimizer record layout shared by algorithms that can use it. */
struct minimizer_physical_record_spec {
    std::uint16_t id{};
    std::string key;
    std::string representation_type;
    std::string hash_output_width;
    std::uint8_t position_log2{};
    std::uint8_t hash_bits{};
    std::uint8_t storage_bits{};
    std::uint8_t available_hash_bits{};

    [[nodiscard]] bool operator==(
        const minimizer_physical_record_spec&) const = default;
};

/** A minimizer implementation compiled for each Highway target. */
struct minimizer_highway_owner_spec {
    std::uint16_t id{};
    std::string owner_key;
    std::string algorithm;
    std::string source_key;
    std::string record_key;
    std::vector<std::string> storage_types;
    std::string alphabet_cpp_type;
    std::uint16_t alphabet_id{};
    std::string export_base;

    [[nodiscard]] bool operator==(
        const minimizer_highway_owner_spec&) const = default;
};

/** Maps supported runtime settings to a record layout and kernel entry point. */
struct minimizer_runtime_profile_spec {
    std::string requested_algorithm;
    std::string effective_algorithm;
    std::string alphabet_cpp_type;
    std::uint16_t alphabet_id{};
    std::uint16_t runtime_kmer_size{};
    std::uint16_t runtime_window_size{};
    std::uint8_t minimum_position_log2{};
    std::uint8_t maximum_position_log2{};
    std::string packed_storage;
    std::string record_key;
    std::string hash_width;
    std::string dispatch_kind;
    std::string highway_owner_key;
    std::string export_symbol;

    [[nodiscard]] bool operator==(
        const minimizer_runtime_profile_spec&) const = default;
};

/** Registered minimizer layouts, compiled implementations and runtime choices. */
struct minimizer_registry_spec {
    minimizer_registry_profile profile{minimizer_registry_profile::baseline};
    std::vector<structural_analysis::alphabet_manifest_entry> alphabets;
    std::vector<minimizer_physical_record_spec> records;
    std::vector<minimizer_highway_owner_spec> highway_owners;
    std::vector<minimizer_runtime_profile_spec> runtime_profiles;

    [[nodiscard]] bool operator==(
        const minimizer_registry_spec&) const = default;
};

[[nodiscard]] inline constexpr std::string_view minimizer_registry_profile_name(
    const minimizer_registry_profile profile) noexcept {
    return profile == minimizer_registry_profile::extended
        ? "extended"
        : "baseline";
}

[[nodiscard]] inline minimizer_registry_profile parse_minimizer_registry_profile(
    const std::string_view name) {
    if (name == "baseline") return minimizer_registry_profile::baseline;
    if (name == "extended") return minimizer_registry_profile::extended;
    throw std::invalid_argument(
        "unknown minimizer registry profile '" + std::string{name} + "'");
}

/** Build physical hash-capacity records and alphabet owners without a k/w template axis. */
[[nodiscard]] inline minimizer_registry_spec make_minimizer_registry(
    const minimizer_registry_profile profile,
    const std::span<const structural_analysis::alphabet_manifest_entry> alphabets) {
    structural_analysis::validate_biological_alphabet_manifest(alphabets);
    minimizer_registry_spec result{.profile = profile, .alphabets = {alphabets.begin(), alphabets.end()}};
    const auto add_record = [&result](const std::uint8_t position_bits, const std::uint8_t hash_bits) {
        if (std::ranges::any_of(result.records, [position_bits, hash_bits](const auto& record) {
                return record.position_log2 == position_bits && record.hash_bits == hash_bits;
            })) return;
        const std::string suffix = "l" + std::to_string(position_bits) + "_h" + std::to_string(hash_bits);
        result.records.push_back(minimizer_physical_record_spec{
            .id = static_cast<std::uint16_t>(result.records.size() + 1u),
            .key = "minimizer_record_" + suffix,
            .representation_type = "::bioseq::minimizers::minimizer_representation<" + std::to_string(position_bits) + "u," + std::to_string(hash_bits) + "u>",
            .hash_output_width = "u" + std::to_string(std::bit_ceil(static_cast<unsigned>(hash_bits))),
            .position_log2 = position_bits, .hash_bits = hash_bits,
            .storage_bits = static_cast<std::uint8_t>(::bioseq::minimizers::minimizer_record_storage_bits(position_bits, hash_bits)),
            .available_hash_bits = hash_bits,
        });
    };
    // Retain the measured 40-bit exact-hash capacity (the previous k=20 route).
    // These physical capacities are independent of alphabet identity and runtime k.
    for (const std::uint8_t hash_bits : std::array<std::uint8_t, 5>{14u, 16u, 32u, 40u, 64u}) {
        for (const std::uint8_t word_bits : std::array<std::uint8_t, 2>{32u, 64u}) {
            if (word_bits > hash_bits + 1u) add_record(static_cast<std::uint8_t>(word_bits - hash_bits - 1u), hash_bits);
        }
        add_record(63u, hash_bits);
    }
    // Retain the measured short/default native-hash position shapes, not their old k-mer axis.
    add_record(15u, 32u);
    add_record(28u, 32u);
    const std::array<std::string, 2> storage_types{"u32", "u64"};
    const auto add_profile = [&result](const structural_analysis::alphabet_manifest_entry& alphabet,
                                      const minimizer_physical_record_spec& record,
                                      const std::string_view algorithm,
                                      const std::string_view kind,
                                      const std::uint16_t maximum_k,
                                      const std::uint8_t minimum_position,
                                      const minimizer_highway_owner_spec* owner) {
        for (const std::string_view storage : {"u32", "u64"}) {
            result.runtime_profiles.push_back(minimizer_runtime_profile_spec{
                .requested_algorithm = std::string{algorithm}, .effective_algorithm = std::string{algorithm},
                .alphabet_cpp_type = alphabet.qualified_cpp_type, .alphabet_id = alphabet.ordinal,
                .runtime_kmer_size = maximum_k, .runtime_window_size = 0u,
                .minimum_position_log2 = minimum_position, .maximum_position_log2 = record.position_log2,
                .packed_storage = std::string{storage}, .record_key = record.key,
                .hash_width = record.hash_output_width, .dispatch_kind = std::string{kind},
                .highway_owner_key = owner == nullptr ? std::string{} : owner->owner_key,
                .export_symbol = owner == nullptr ? std::string{} : owner->export_base + "_" + std::string{storage},
            });
        }
    };
    for (const auto& alphabet : alphabets) {
        for (const auto& record : result.records) {
            const std::string suffix = "l" + std::to_string(record.position_log2) + "_h" + std::to_string(record.hash_bits) + "_" + alphabet.literal_name;
            std::uint8_t minimum_position{};
            // Disjoint position bands within one hash-width prevent order-dependent profile selection.
            for (const auto& previous : result.records) {
                if (previous.hash_bits == record.hash_bits && previous.position_log2 < record.position_log2) {
                    minimum_position = std::max(minimum_position, static_cast<std::uint8_t>(previous.position_log2 + 1u));
                }
            }
            result.highway_owners.push_back(minimizer_highway_owner_spec{
                .id = static_cast<std::uint16_t>(result.highway_owners.size() + 1u),
                .owner_key = "minimap2_reference_" + suffix, .algorithm = "minimap2_reference",
                .source_key = "minimizer_minimap2_reference", .record_key = record.key,
                .storage_types = {storage_types.begin(), storage_types.end()},
                .alphabet_cpp_type = alphabet.qualified_cpp_type, .alphabet_id = alphabet.ordinal,
                .export_base = "minimap2_reference_repr_" + suffix,
            });
            const auto maximum_k = static_cast<std::uint16_t>(std::min<unsigned>(31u, record.hash_bits / alphabet.exact_seed_rank_bits));
            add_profile(alphabet, record, "simd_minimap2_reference", "highway_export", maximum_k, minimum_position, &result.highway_owners.back());
            add_profile(alphabet, record, "scalar", "scalar_collect_minimizers", maximum_k, minimum_position, nullptr);
            if (record.storage_bits >= 64u) {
                std::uint8_t two_stack_minimum_position{};
                for (const auto& previous : result.records) {
                    if (previous.hash_bits == record.hash_bits && previous.storage_bits >= 64u
                        && previous.position_log2 < record.position_log2) {
                        two_stack_minimum_position = std::max(two_stack_minimum_position, static_cast<std::uint8_t>(previous.position_log2 + 1u));
                    }
                }
                add_profile(alphabet, record, "two_stack_nthash", "highway_slot", maximum_k, two_stack_minimum_position, nullptr);
            }
            if (record.hash_bits == 32u) {
                add_profile(alphabet, record, "vulkan_minimap_integer", "vulkan_minimap_integer", 0u, minimum_position, nullptr);
            }
            if (record.hash_bits == 32u) {
                result.highway_owners.push_back(minimizer_highway_owner_spec{
                    .id = static_cast<std::uint16_t>(result.highway_owners.size() + 1u),
                    .owner_key = "bounded_nthash_" + suffix, .algorithm = "bounded_nthash",
                    .source_key = "minimizer_bounded_nthash", .record_key = record.key,
                    .storage_types = {storage_types.begin(), storage_types.end()},
                    .alphabet_cpp_type = alphabet.qualified_cpp_type, .alphabet_id = alphabet.ordinal,
                    .export_base = "bounded_nthash_repr_" + suffix,
                });
                add_profile(alphabet, record, "bounded_nthash_simd", "highway_export", 0u, minimum_position, &result.highway_owners.back());
            }
        }
    }
    return result;
}

struct minimizer_runtime_profile_capabilities final {
    std::uint16_t minimum_runtime_kmer_size{};
    std::uint16_t maximum_runtime_kmer_size{};
    std::uint16_t minimum_runtime_window_size{};
    std::uint16_t maximum_runtime_window_size{};
};

/** Finds the supported k-mer and window sizes for one record binding. */
[[nodiscard]] inline minimizer_runtime_profile_capabilities
minimizer_runtime_profile_capabilities_for(
    const minimizer_registry_spec& registry,
    const minimizer_runtime_profile_spec& binding) {
    if (binding.requested_algorithm == "bounded_nthash_simd" || binding.requested_algorithm == "vulkan_minimap_integer") {
        return {1u, 31u, 1u, 64u};
    }
    const auto alphabet = std::ranges::find_if(registry.alphabets, [&binding](const auto& candidate) {
        return candidate.ordinal == binding.alphabet_id && candidate.qualified_cpp_type == binding.alphabet_cpp_type;
    });
    const auto record = std::ranges::find_if(registry.records, [&binding](const auto& candidate) {
        return candidate.key == binding.record_key;
    });
    if (alphabet == registry.alphabets.end() || record == registry.records.end() || alphabet->exact_seed_rank_bits == 0u) {
        throw std::logic_error("runtime minimizer profile has unresolved alphabet or physical record");
    }
    std::uint8_t previous_hash_bits{};
    for (const auto& candidate : registry.records) {
        if (candidate.hash_bits < record->hash_bits) previous_hash_bits = std::max(previous_hash_bits, candidate.hash_bits);
    }
    const auto minimum_k = static_cast<std::uint16_t>(previous_hash_bits / alphabet->exact_seed_rank_bits + 1u);
    const auto maximum_k = static_cast<std::uint16_t>(std::min<unsigned>(31u, record->hash_bits / alphabet->exact_seed_rank_bits));
    return {minimum_k, maximum_k, 1u, 256u};
}

/** A named external topology manifest supplied during CMake configuration. */
struct domain_topology_manifest_input {
    std::string name;
    std::filesystem::path path;
};

/** Verified identity and hashes of an external topology manifest. */
struct domain_topology_provenance {
    std::string name;
    std::filesystem::path path;
    std::string topology_digest;
    std::string content_sha256;
};

/** Topology identity recorded in both target and source manifests. */
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
    std::vector<probe_spec> unit_probes;
    std::optional<minimizer_registry_spec> minimizer_registry;
    std::filesystem::path minimizer_registry_manifest_path;
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

struct minimizer_registry_command_line {
    std::filesystem::path registry_source;
    std::filesystem::path seed_policy_source;
    std::filesystem::path pipeline_owner_source;
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
    std::filesystem::path depfile;
    minimizer_registry_profile profile{minimizer_registry_profile::baseline};
    std::filesystem::path records_output;
    std::filesystem::path visitor_output;
    std::filesystem::path pipeline_entries_output;
    std::filesystem::path manifest_output;
    std::filesystem::path stamp;
};

struct callable_insertion {
    std::size_t body_relative_offset{};
    std::string qualified_name;
    std::string kind;
    std::string lambda_preceding_token;
    std::string lambda_following_token;
    std::size_t lambda_following_relative_offset{};
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
    kernel_instantiation,
    dispatch_registry_partition,
    dispatch_inventory_implementation,
    test_support,
    unit_probe_implementation,
    minimizer_representation_instantiation,
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
    std::string clang_release;
    std::string compilation_signature;
    std::vector<domain_topology_provenance> domain_topologies;
};

struct generation_result {
    std::vector<generated_artifact> artifacts;
    std::string policy_evidence_json;
    std::string publication_topology_digest;
};

}  // namespace bioseq::codegen::highway
