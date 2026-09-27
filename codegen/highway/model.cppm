module;

#include <algorithm>
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
    std::uint8_t physical_kmer_size{};
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

/** An alphabet supported by the generic minimap2-reference adapter. */
struct generic_reference_alphabet_spec final {
    std::string_view cpp_type;
    std::uint16_t id;
    std::string_view symbol_suffix;
    std::uint8_t state_bits_per_rank;
};

/** Append new generic reference alphabets here to preserve existing IDs. */
inline constexpr generic_reference_alphabet_spec
    generic_reference_alphabets[]{
        {"::seq::dna5", 1u, "dna5", 3u},
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

/** Builds the minimizer registry as data, without instantiating record templates. */
[[nodiscard]] inline minimizer_registry_spec make_minimizer_registry(
    const minimizer_registry_profile profile) {
    minimizer_registry_spec result{.profile = profile};
    const auto add_record = [&result](
                                const std::uint16_t id,
                                const std::string_view key,
                                const std::uint8_t position_log2,
                                const std::uint8_t physical_kmer_size,
                                const std::string_view hash_width) {
        const auto storage_bits = static_cast<std::uint8_t>(
            ::bioseq::minimizers::minimizer_record_storage_bits(
                position_log2, physical_kmer_size));
        const auto available_hash_bits = static_cast<std::uint8_t>(
            storage_bits == 128u
                ? 128u - 1u - position_log2 - 8u
                : storage_bits - 1u - position_log2);
        result.records.push_back(minimizer_physical_record_spec{
            .id = id,
            .key = std::string{key},
            .representation_type =
                "::bioseq::minimizers::minimizer_representation<"
                + std::to_string(position_log2) + "u,"
                + std::to_string(physical_kmer_size) + "u>",
            .hash_output_width = std::string{hash_width},
            .position_log2 = position_log2,
            .physical_kmer_size = physical_kmer_size,
            .storage_bits = storage_bits,
            .available_hash_bits = available_hash_bits,
        });
    };

    add_record(1u, "minimizer_record_l15_k15", 15u, 15u, "u32");
    add_record(2u, "minimizer_record_l28_k15", 28u, 15u, "u32");
    add_record(3u, "minimizer_record_l33_k15", 33u, 15u, "u32");
    add_record(4u, "minimizer_record_l63_k15", 63u, 15u, "u32");
    add_record(5u, "minimizer_record_l20_k20", 20u, 20u, "u64");
    add_record(6u, "minimizer_record_l23_k20", 23u, 20u, "u64");
    add_record(7u, "minimizer_record_l63_k20", 63u, 20u, "u64");
    if (profile == minimizer_registry_profile::extended) {
        add_record(8u, "minimizer_record_l7_k7", 7u, 7u, "u16");
        add_record(9u, "minimizer_record_l17_k7", 17u, 7u, "u16");
        add_record(10u, "minimizer_record_l49_k7", 49u, 7u, "u16");
        add_record(11u, "minimizer_record_l63_k7", 63u, 7u, "u16");
        add_record(12u, "minimizer_record_l27_k27", 27u, 27u, "u64");
        add_record(13u, "minimizer_record_l63_k27", 63u, 27u, "u64");
    }

    const auto add_owner = [&result](
                               const std::uint16_t id,
                               const std::string_view owner_key,
                               const std::string_view algorithm,
                               const std::string_view source_key,
                               const std::string_view record_key,
                               std::vector<std::string> storages,
                               const std::string_view alphabet_cpp_type,
                               const std::uint16_t alphabet_id,
                               const std::string_view export_base) {
        result.highway_owners.push_back(minimizer_highway_owner_spec{
            .id = id,
            .owner_key = std::string{owner_key},
            .algorithm = std::string{algorithm},
            .source_key = std::string{source_key},
            .record_key = std::string{record_key},
            .storage_types = std::move(storages),
            .alphabet_cpp_type = std::string{alphabet_cpp_type},
            .alphabet_id = alphabet_id,
            .export_base = std::string{export_base},
        });
    };
    const std::vector<std::string> both_storages{"u32", "u64"};
    // DNA4 implementations have fixed IDs and exported names. Alphabets in
    // generic_reference_alphabets add reference implementations through the
    // adapter that stores alphabet ranks in state lanes.
    add_owner(1u, "minimap2_reference_l15_k15", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l15_k15",
              both_storages, "::seq::dna4", 0u,
              "minimap2_reference_repr_l15_k15");
    add_owner(2u, "minimap2_reference_l28_k15", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l28_k15",
              both_storages, "::seq::dna4", 0u,
              "minimap2_reference_repr_l28_k15");
    add_owner(3u, "minimap2_reference_l33_k15", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l33_k15",
              both_storages, "::seq::dna4", 0u,
              "minimap2_reference_repr_l33_k15");
    add_owner(4u, "minimap2_reference_l63_k15", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l63_k15",
              both_storages, "::seq::dna4", 0u,
              "minimap2_reference_repr_l63_k15");
    add_owner(5u, "bounded_nthash_l15_k15", "bounded_nthash",
              "minimizer_bounded_nthash", "minimizer_record_l15_k15",
              both_storages, "::seq::dna4", 0u,
              "bounded_nthash_repr_l15_k15");
    add_owner(6u, "bounded_nthash_l28_k15", "bounded_nthash",
              "minimizer_bounded_nthash", "minimizer_record_l28_k15",
              both_storages, "::seq::dna4", 0u,
              "bounded_nthash_repr_l28_k15");
    add_owner(7u, "bounded_nthash_l63_k15", "bounded_nthash",
              "minimizer_bounded_nthash", "minimizer_record_l63_k15",
              both_storages, "::seq::dna4", 0u,
              "bounded_nthash_repr_l63_k15");
    add_owner(8u, "bounded_nthash_l15_dna5", "bounded_nthash",
              "minimizer_bounded_nthash", "minimizer_record_l15_k15",
              both_storages, "::seq::dna5", 1u,
              "bounded_nthash_repr_l15_dna5");
    add_owner(9u, "bounded_nthash_l28_dna5", "bounded_nthash",
              "minimizer_bounded_nthash", "minimizer_record_l28_k15",
              both_storages, "::seq::dna5", 1u,
              "bounded_nthash_repr_l28_dna5");
    add_owner(10u, "bounded_nthash_l63_dna5", "bounded_nthash",
              "minimizer_bounded_nthash", "minimizer_record_l63_k15",
              both_storages, "::seq::dna5", 1u,
              "bounded_nthash_repr_l63_dna5");
    add_owner(11u, "minimap2_reference_l20_k20", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l20_k20",
              {"u64"}, "::seq::dna4", 0u,
              "minimap2_reference_repr_l20_k20");
    add_owner(12u, "minimap2_reference_l23_k20", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l23_k20",
              {"u64"}, "::seq::dna4", 0u,
              "minimap2_reference_repr_l23_k20");
    add_owner(13u, "minimap2_reference_l63_k20", "minimap2_reference",
              "minimizer_minimap2_reference", "minimizer_record_l63_k20",
              {"u64"}, "::seq::dna4", 0u,
              "minimap2_reference_repr_l63_k20");
    if (profile == minimizer_registry_profile::extended) {
        add_owner(14u, "minimap2_reference_l7_k7", "minimap2_reference",
                  "minimizer_minimap2_reference", "minimizer_record_l7_k7",
                  both_storages, "::seq::dna4", 0u,
                  "minimap2_reference_repr_l7_k7");
        add_owner(15u, "minimap2_reference_l17_k7", "minimap2_reference",
                  "minimizer_minimap2_reference", "minimizer_record_l17_k7",
                  both_storages, "::seq::dna4", 0u,
                  "minimap2_reference_repr_l17_k7");
        add_owner(16u, "minimap2_reference_l49_k7", "minimap2_reference",
                  "minimizer_minimap2_reference", "minimizer_record_l49_k7",
                  both_storages, "::seq::dna4", 0u,
                  "minimap2_reference_repr_l49_k7");
        add_owner(17u, "minimap2_reference_l63_k7", "minimap2_reference",
                  "minimizer_minimap2_reference", "minimizer_record_l63_k7",
                  both_storages, "::seq::dna4", 0u,
                  "minimap2_reference_repr_l63_k7");
        add_owner(18u, "minimap2_reference_l27_k27", "minimap2_reference",
                  "minimizer_minimap2_reference", "minimizer_record_l27_k27",
                  {"u64"}, "::seq::dna4", 0u,
                  "minimap2_reference_repr_l27_k27");
        add_owner(19u, "minimap2_reference_l63_k27", "minimap2_reference",
                  "minimizer_minimap2_reference", "minimizer_record_l63_k27",
                  {"u64"}, "::seq::dna4", 0u,
                  "minimap2_reference_repr_l63_k27");
    }
    const std::size_t legacy_owner_count = result.highway_owners.size();
    std::uint16_t next_generic_owner_id = 20u;
    for (const generic_reference_alphabet_spec& alphabet :
         generic_reference_alphabets) {
        if (alphabet.id == 0u || alphabet.symbol_suffix.empty()
            || alphabet.state_bits_per_rank == 0u
            || alphabet.state_bits_per_rank > 64u) {
            throw std::logic_error(
                "invalid generic minimap2-reference alphabet registration");
        }
        for (std::size_t index = 0u; index < legacy_owner_count; ++index) {
            // Copy before add_owner can reallocate the owner vector.
            const minimizer_highway_owner_spec legacy =
                result.highway_owners[index];
            if (legacy.algorithm != "minimap2_reference"
                || legacy.alphabet_id != 0u) {
                continue;
            }
            add_owner(
                next_generic_owner_id++,
                legacy.owner_key + "_" + std::string{alphabet.symbol_suffix},
                legacy.algorithm, legacy.source_key, legacy.record_key,
                both_storages, alphabet.cpp_type, alphabet.id,
                legacy.export_base + "_" +
                    std::string{alphabet.symbol_suffix});
        }
    }

    const auto owner = [&result](const std::string_view key)
        -> const minimizer_highway_owner_spec& {
        for (const auto& candidate : result.highway_owners) {
            if (candidate.owner_key == key) return candidate;
        }
        throw std::logic_error("missing minimizer owner '" + std::string{key} + "'");
    };
    const auto add_profile = [&result](
                                 const std::string_view requested,
                                 const std::string_view effective,
                                 const std::string_view alphabet_cpp_type,
                                 const std::uint16_t alphabet_id,
                                 const std::uint16_t runtime_k,
                                 const std::uint16_t runtime_w,
                                 const std::uint8_t minimum_l,
                                 const std::uint8_t maximum_l,
                                 const std::string_view storage,
                                 const std::string_view record_key,
                                 const std::string_view hash_width,
                                 const std::string_view dispatch_kind,
                                 const std::string_view owner_key = {},
                                 const std::string_view export_symbol = {}) {
        result.runtime_profiles.push_back(minimizer_runtime_profile_spec{
            .requested_algorithm = std::string{requested},
            .effective_algorithm = std::string{effective},
            .alphabet_cpp_type = std::string{alphabet_cpp_type},
            .alphabet_id = alphabet_id,
            .runtime_kmer_size = runtime_k,
            .runtime_window_size = runtime_w,
            .minimum_position_log2 = minimum_l,
            .maximum_position_log2 = maximum_l,
            .packed_storage = std::string{storage},
            .record_key = std::string{record_key},
            .hash_width = std::string{hash_width},
            .dispatch_kind = std::string{dispatch_kind},
            .highway_owner_key = std::string{owner_key},
            .export_symbol = std::string{export_symbol},
        });
    };
    const auto add_export_profiles = [&add_profile](
                                         const minimizer_highway_owner_spec& export_owner,
                                         const std::uint16_t runtime_k,
                                         const std::uint16_t runtime_w,
                                         const std::uint8_t minimum_l,
                                         const std::uint8_t maximum_l,
                                         const std::string_view hash_width,
                                         const std::string_view requested) {
        for (const std::string& storage : export_owner.storage_types) {
            add_profile(
                requested, requested, export_owner.alphabet_cpp_type,
                export_owner.alphabet_id, runtime_k, runtime_w,
                minimum_l, maximum_l, storage, export_owner.record_key,
                hash_width, "highway_export", export_owner.owner_key,
                export_owner.export_base + "_" + storage);
        }
    };

    struct band_spec final {
        std::uint8_t minimum_l;
        std::uint8_t maximum_l;
        std::string_view record_key;
        std::string_view dna4_bounded_owner;
        std::string_view dna5_bounded_owner;
    };
    static constexpr band_spec fixed_bands[]{
        {0u, 15u, "minimizer_record_l15_k15", "bounded_nthash_l15_k15", "bounded_nthash_l15_dna5"},
        {16u, 28u, "minimizer_record_l28_k15", "bounded_nthash_l28_k15", "bounded_nthash_l28_dna5"},
        {29u, 63u, "minimizer_record_l63_k15", "bounded_nthash_l63_k15", "bounded_nthash_l63_dna5"},
    };
    for (const auto& band : fixed_bands) {
        // Zero K/W values mark the bounded ntHash profile. The generated code
        // replaces them with inclusive limits and checks requests with the shared
        // shape predicate.
        add_export_profiles(owner(band.dna4_bounded_owner), 0u, 0u,
                            band.minimum_l, band.maximum_l, "u32",
                            "bounded_nthash_simd");
        add_export_profiles(owner(band.dna5_bounded_owner), 0u, 0u,
                            band.minimum_l, band.maximum_l, "u32",
                            "bounded_nthash_simd");
    }
    const auto add_two_stack_slice = [&add_profile](
                                         const std::string_view record_key,
                                         const std::uint8_t minimum_l,
                                         const std::uint8_t maximum_l,
                                         const std::uint16_t maximum_runtime_k,
                                         const std::string_view hash_width) {
        for (const auto [alphabet_cpp_type, alphabet_id] :
             {std::pair{std::string_view{"::seq::dna4"}, std::uint16_t{0u}},
              std::pair{std::string_view{"::seq::dna5"}, std::uint16_t{1u}}}) {
            for (const std::string_view storage : {"u32", "u64"}) {
                add_profile(
                    "two_stack_nthash", "two_stack_nthash",
                    alphabet_cpp_type, alphabet_id, maximum_runtime_k, 0u,
                    minimum_l, maximum_l, storage, record_key, hash_width,
                    "highway_slot");
            }
        }
    };
    // Two-stack hashes need two bits per base in the record. Put compact
    // layouts first so the first matching profile uses them. Larger split
    // records handle requests that do not fit those layouts.
    add_two_stack_slice("minimizer_record_l15_k15", 0u, 15u, 8u, "u16");
    add_two_stack_slice("minimizer_record_l15_k15", 0u, 15u, 16u, "u32");
    add_two_stack_slice("minimizer_record_l15_k15", 0u, 15u, 24u, "u64");
    add_two_stack_slice("minimizer_record_l28_k15", 16u, 28u, 8u, "u16");
    add_two_stack_slice("minimizer_record_l28_k15", 16u, 28u, 16u, "u32");
    add_two_stack_slice("minimizer_record_l28_k15", 16u, 28u, 17u, "u64");
    add_two_stack_slice("minimizer_record_l33_k15", 29u, 33u, 8u, "u16");
    add_two_stack_slice("minimizer_record_l33_k15", 29u, 33u, 15u, "u32");
    add_two_stack_slice("minimizer_record_l20_k20", 0u, 20u, 21u, "u64");
    add_two_stack_slice("minimizer_record_l23_k20", 0u, 23u, 20u, "u64");
    add_two_stack_slice("minimizer_record_l63_k15", 0u, 63u, 8u, "u16");
    add_two_stack_slice("minimizer_record_l63_k15", 0u, 63u, 16u, "u32");
    add_two_stack_slice("minimizer_record_l63_k20", 0u, 63u, 28u, "u64");

    struct exact_band_spec final {
        std::uint8_t minimum_l;
        std::uint8_t maximum_l;
        std::string_view record_key;
        std::string_view owner_key;
        std::uint16_t maximum_runtime_k;
        std::string_view hash_width;
    };
    static constexpr exact_band_spec k15_bands[]{
        {0u, 15u, "minimizer_record_l15_k15", "minimap2_reference_l15_k15", 8u, "u16"},
        {0u, 15u, "minimizer_record_l15_k15", "minimap2_reference_l15_k15", 15u, "u32"},
        {16u, 28u, "minimizer_record_l28_k15", "minimap2_reference_l28_k15", 8u, "u16"},
        {16u, 28u, "minimizer_record_l28_k15", "minimap2_reference_l28_k15", 15u, "u32"},
        {29u, 33u, "minimizer_record_l33_k15", "minimap2_reference_l33_k15", 8u, "u16"},
        {29u, 33u, "minimizer_record_l33_k15", "minimap2_reference_l33_k15", 15u, "u32"},
        {34u, 63u, "minimizer_record_l63_k15", "minimap2_reference_l63_k15", 8u, "u16"},
        {34u, 63u, "minimizer_record_l63_k15", "minimap2_reference_l63_k15", 15u, "u32"},
    };
    static constexpr exact_band_spec k20_bands[]{
        {0u, 20u, "minimizer_record_l20_k20", "minimap2_reference_l20_k20", 16u, "u32"},
        {0u, 20u, "minimizer_record_l20_k20", "minimap2_reference_l20_k20", 31u, "u64"},
        {21u, 23u, "minimizer_record_l23_k20", "minimap2_reference_l23_k20", 16u, "u32"},
        {21u, 23u, "minimizer_record_l23_k20", "minimap2_reference_l23_k20", 31u, "u64"},
        {0u, 63u, "minimizer_record_l63_k20", "minimap2_reference_l63_k20", 16u, "u32"},
        {0u, 63u, "minimizer_record_l63_k20", "minimap2_reference_l63_k20", 31u, "u64"},
    };
    const auto add_exact_band_profiles = [&add_export_profiles, &add_profile, &owner](
                                             const exact_band_spec& band) {
        add_export_profiles(owner(band.owner_key), band.maximum_runtime_k, 0u,
                            band.minimum_l, band.maximum_l, band.hash_width,
                            "simd_minimap2_reference");
        for (const generic_reference_alphabet_spec& alphabet :
             generic_reference_alphabets) {
            const auto state_capacity = static_cast<std::uint16_t>(
                64u / alphabet.state_bits_per_rank);
            const auto generic_maximum_k = std::min<std::uint16_t>(
                band.maximum_runtime_k,
                std::min<std::uint16_t>(state_capacity, 31u));
            const std::string generic_owner_key =
                std::string{band.owner_key} + "_" +
                std::string{alphabet.symbol_suffix};
            add_export_profiles(owner(generic_owner_key), generic_maximum_k,
                                0u, band.minimum_l, band.maximum_l,
                                band.hash_width, "simd_minimap2_reference");
        }
        for (const auto [alphabet_cpp_type, alphabet_id] :
             {std::pair{std::string_view{"::seq::dna4"}, std::uint16_t{0u}},
              std::pair{std::string_view{"::seq::dna5"}, std::uint16_t{1u}}}) {
            for (const std::string_view storage : {"u32", "u64"}) {
                add_profile("scalar", "scalar", alphabet_cpp_type,
                            alphabet_id, band.maximum_runtime_k, 0u,
                            band.minimum_l, band.maximum_l, storage,
                            band.record_key, band.hash_width,
                            "scalar_collect_minimizers");
                add_profile(
                    "vulkan_minimap_integer", "vulkan_minimap_integer",
                    alphabet_cpp_type, alphabet_id,
                    band.maximum_runtime_k, 0u,
                    band.minimum_l, band.maximum_l, storage,
                    band.record_key, band.hash_width,
                    "vulkan_minimap_integer");
            }
        }
    };
    for (const auto& band : k15_bands) {
        add_exact_band_profiles(band);
    }
    for (const auto& band : k20_bands) {
        add_exact_band_profiles(band);
    }

    if (profile == minimizer_registry_profile::extended) {
        static constexpr exact_band_spec k7_bands[]{
            {0u, 7u, "minimizer_record_l7_k7", "minimap2_reference_l7_k7", 7u, "u16"},
            {8u, 17u, "minimizer_record_l17_k7", "minimap2_reference_l17_k7", 7u, "u16"},
            {18u, 49u, "minimizer_record_l49_k7", "minimap2_reference_l49_k7", 7u, "u16"},
            {50u, 63u, "minimizer_record_l63_k7", "minimap2_reference_l63_k7", 7u, "u16"},
        };
        static constexpr exact_band_spec k27_bands[]{
            {0u, 27u, "minimizer_record_l27_k27", "minimap2_reference_l27_k27", 31u, "u64"},
            {28u, 63u, "minimizer_record_l63_k27", "minimap2_reference_l63_k27", 31u, "u64"},
        };
        for (const auto& band : k7_bands) {
            add_exact_band_profiles(band);
        }
        for (const auto& band : k27_bands) {
            add_exact_band_profiles(band);
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
    if (binding.requested_algorithm == "bounded_nthash_simd") {
        return {1u, 31u, 1u, 64u};
    }
    if (binding.requested_algorithm == "two_stack_nthash") {
        const minimizer_physical_record_spec* record = nullptr;
        for (const auto& candidate : registry.records) {
            if (candidate.key == binding.record_key) record = &candidate;
        }
        if (record == nullptr
            || (record->storage_bits != 64u
                && record->storage_bits != 128u)) return {};
        const auto usable_hash_bits = static_cast<std::uint16_t>(
            record->available_hash_bits > 56u
                ? 56u
                : record->available_hash_bits);
        const auto record_maximum_k = static_cast<std::uint16_t>(
            usable_hash_bits / 2u);
        const auto maximum_k = record_maximum_k < binding.runtime_kmer_size
            ? record_maximum_k
            : binding.runtime_kmer_size;
        if (binding.hash_width == "u16") {
            return {1u, maximum_k, 1u, 256u};
        }
        if (binding.hash_width == "u32") {
            return {9u, maximum_k, 1u, 256u};
        }
        return {17u, maximum_k, 1u, 256u};
    }

    const minimizer_physical_record_spec* record = nullptr;
    std::uint16_t minimum_k = 1u;
    for (const auto& candidate : registry.records) {
        if (candidate.key == binding.record_key) record = &candidate;
    }
    if (record == nullptr) {
        throw std::logic_error(
            "runtime profile references missing minimizer record '" +
            binding.record_key + "'");
    }
    for (const auto& candidate : registry.records) {
        if (candidate.physical_kmer_size < record->physical_kmer_size) {
            const auto next = static_cast<std::uint16_t>(
                candidate.physical_kmer_size + 1u);
            if (next > minimum_k) minimum_k = next;
        }
    }

    std::uint16_t hash_minimum_k = 1u;
    std::uint16_t hash_maximum_k = 0u;
    if (binding.hash_width == "u16") {
        hash_maximum_k = 8u;
    } else if (binding.hash_width == "u32") {
        hash_minimum_k = 9u;
        hash_maximum_k = 16u;
    } else if (binding.hash_width == "u64") {
        hash_minimum_k = 17u;
        hash_maximum_k = 31u;
    }
    if (hash_minimum_k > minimum_k) minimum_k = hash_minimum_k;

    auto maximum_k = binding.runtime_kmer_size;
    for (const auto& candidate : registry.records) {
        if (candidate.physical_kmer_size > record->physical_kmer_size) {
            const auto bucket_maximum =
                static_cast<std::uint16_t>(record->physical_kmer_size);
            if (bucket_maximum < maximum_k) maximum_k = bucket_maximum;
            break;
        }
    }
    if (hash_maximum_k < maximum_k) maximum_k = hash_maximum_k;
    const auto physical_hash_bits = static_cast<std::uint16_t>(
        record->storage_bits == 128u && record->available_hash_bits > 56u
            ? 56u
            : record->available_hash_bits);
    const auto representation_maximum_k = static_cast<std::uint16_t>(
        physical_hash_bits / 2u);
    if (representation_maximum_k < maximum_k) {
        maximum_k = representation_maximum_k;
    }
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
    minimizer_registry_profile profile{minimizer_registry_profile::baseline};
    std::filesystem::path records_output;
    std::filesystem::path visitor_output;
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
