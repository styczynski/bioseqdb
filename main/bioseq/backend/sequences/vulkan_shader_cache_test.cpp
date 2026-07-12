#include "common_tests.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>
#include <vulkan/vulkan.h>

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.phase_c_abi;

namespace {

namespace gv = bioseq::gpu::vulkan;

bool shader_module_create_called = false;

VKAPI_ATTR VkResult VKAPI_CALL record_shader_module_create(
    VkDevice,
    const VkShaderModuleCreateInfo*,
    const VkAllocationCallbacks*,
    VkShaderModule*)
{
    shader_module_create_called = true;
    return VK_SUCCESS;
}

struct NoPublishCompileService {
    int calls = 0;

    [[nodiscard]] std::expected<void, std::string> ensure_variant_compiled(
        gv::ShaderVariantKey)
    {
        ++calls;
        return {};
    }
};

struct TempDir {
    std::filesystem::path path;

    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path()
             / ("bioseq_shader_cache_test_" + std::to_string(::getpid())
                + "_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

[[nodiscard]] bool contains_key(
    const std::vector<gv::ShaderVariantKey>& keys,
    gv::ShaderVariantKey needle)
{
    return std::find(keys.begin(), keys.end(), needle) != keys.end();
}

[[nodiscard]] bool is_hex_digest(std::string_view digest)
{
    return digest.size() == 16
        && std::all_of(digest.begin(), digest.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text)
{
    std::vector<std::byte> out(text.size());
    std::memcpy(out.data(), text.data(), text.size());
    return out;
}

// Test-only scalar oracles for the inactive ABI. They are deliberately local
// to this unit test and are not a host execution alternative to the GPU route.
[[nodiscard]] std::uint32_t grouped32_partition_validation_oracle(
    const gv::GpuGrouped32PartitionWire& partition,
    std::span<const gv::GpuGrouped32ControlWire> controls,
    std::span<const gv::GpuGrouped32PositionWire> positions)
{
    using Error = gv::GpuGrouped32ValidationError;
    const auto bit = [](Error error) {
        return static_cast<std::uint32_t>(error);
    };
    std::uint32_t status = 0u;
    const bool control_bounds = partition.control_offset <= controls.size()
        && partition.control_count
            <= controls.size() - partition.control_offset;
    const bool position_bounds = partition.position_offset <= positions.size()
        && partition.position_count
            <= positions.size() - partition.position_offset;
    if (!control_bounds || !position_bounds
        || partition.control_count == 0u
        || partition.position_count == 0u) {
        status |= bit(Error::descriptor_bounds);
    }
    if (partition.flags != 0u || partition.reserved0 != 0u
        || partition.reserved1 != 0u) {
        status |= bit(Error::descriptor_identity);
    }
    if (partition.segment_base_bp >= partition.segment_end_bp
        || partition.segment_end_bp - partition.segment_base_bp
            > std::uint64_t{gv::k_gpu_grouped32_max_local_bp} + 1u
        || partition.low_hash > partition.high_hash) {
        status |= bit(Error::segment_bounds);
    }
    if (!control_bounds || !position_bounds
        || partition.control_count == 0u
        || partition.position_count == 0u
        || partition.segment_base_bp >= partition.segment_end_bp) {
        return status;
    }

    std::uint32_t expected_position_offset = 0u;
    std::optional<std::uint32_t> previous_hash;
    for (std::uint32_t local_control = 0u;
         local_control < partition.control_count; ++local_control) {
        const auto& control = controls[partition.control_offset + local_control];
        if ((previous_hash && control.hash <= *previous_hash)
            || control.hash < partition.low_hash
            || control.hash > partition.high_hash) {
            status |= bit(Error::control_order);
        }
        if (control.position_count == 0u
            || control.position_offset != expected_position_offset
            || control.position_offset > partition.position_count
            || control.position_count
                > partition.position_count - control.position_offset) {
            status |= bit(Error::control_positions);
            continue;
        }
        previous_hash = control.hash;
        expected_position_offset =
            control.position_offset + control.position_count;
        std::optional<std::uint32_t> previous_position;
        for (std::uint32_t local_position = 0u;
             local_position < control.position_count; ++local_position) {
            const std::uint32_t encoded = positions[
                partition.position_offset + control.position_offset
                + local_position].local_bp_strand;
            if (std::uint64_t{encoded >> 1u}
                >= partition.segment_end_bp - partition.segment_base_bp) {
                status |= bit(Error::position_bounds);
            }
            if (previous_position && encoded <= *previous_position) {
                status |= bit(Error::position_order);
            }
            previous_position = encoded;
        }
    }
    if (!previous_hash
        || controls[partition.control_offset].hash != partition.low_hash
        || *previous_hash != partition.high_hash
        || expected_position_offset != partition.position_count) {
        status |= bit(Error::control_positions);
    }
    return status;
}

struct Grouped32GlobalPositionOracle {
    std::uint64_t bp = 0;
    std::uint32_t strand = 0;
};

[[nodiscard]] std::optional<Grouped32GlobalPositionOracle>
grouped32_globalize_oracle(
    const gv::GpuGrouped32PartitionWire& partition,
    const gv::GpuGrouped32PositionWire position)
{
    if (partition.segment_base_bp >= partition.segment_end_bp) {
        return std::nullopt;
    }
    const std::uint32_t local_bp = position.local_bp_strand >> 1u;
    if (std::uint64_t{local_bp}
        >= partition.segment_end_bp - partition.segment_base_bp) {
        return std::nullopt;
    }
    return Grouped32GlobalPositionOracle{
        .bp = partition.segment_base_bp + local_bp,
        .strand = position.local_bp_strand & 1u,
    };
}

[[nodiscard]] constexpr std::uint32_t grouped32_saturating_count_oracle(
    std::uint32_t current,
    std::uint32_t increment,
    std::uint32_t max_occ) noexcept
{
    const std::uint32_t ceiling =
        max_occ == (std::numeric_limits<std::uint32_t>::max)()
        ? max_occ : max_occ + 1u;
    if (current >= ceiling) return ceiling;
    return current + std::min(increment, ceiling - current);
}

[[nodiscard]] constexpr std::uint32_t grouped32_finalize_count_oracle(
    std::uint32_t global_count,
    std::uint32_t max_occ) noexcept
{
    return global_count <= max_occ ? global_count : 0u;
}

[[nodiscard]] std::uint32_t grouped32_directory_identity_oracle(
    std::span<const gv::GpuGrouped32PartitionWire> partitions)
{
    const auto bit = [](gv::GpuGrouped32ValidationError error) {
        return static_cast<std::uint32_t>(error);
    };
    std::uint32_t status = 0u;
    for (std::size_t ordinal = 0; ordinal < partitions.size(); ++ordinal) {
        const auto& partition = partitions[ordinal];
        if (partition.directory_ordinal != static_cast<std::uint32_t>(ordinal)
            || (ordinal != 0u
                && partition.partition_id
                    <= partitions[ordinal - 1u].partition_id)) {
            status |= bit(gv::GpuGrouped32ValidationError::descriptor_identity);
        }
    }
    return status;
}

[[nodiscard]] bool grouped32_query_encoding_oracle(
    const gv::GpuGrouped32QueryRecordWire& record,
    const gv::GpuGrouped32QueryMinimizerWire& minimizer,
    std::uint32_t kmer_size = 15u)
{
    const std::uint32_t q_last = minimizer.query_bp_strand >> 1u;
    return kmer_size == 15u && record.query_length <= 0x80000000u
        && record.query_length >= kmer_size && q_last < record.query_length
        && q_last + 1u >= kmer_size;
}

[[nodiscard]] constexpr bool grouped32_gate_oracle(
    const gv::GpuGrouped32ValidationStateWire& validation,
    const gv::GpuGrouped32QueryAdapterStateWire& adapter) noexcept
{
    using Completion = gv::GpuGrouped32ValidationCompletion;
    return validation.completion
            == static_cast<std::uint32_t>(Completion::accepted)
        && validation.status == 0u
        && adapter.completion
            == static_cast<std::uint32_t>(Completion::accepted)
        && adapter.status == 0u;
}

}  // namespace

TEST_CASE("DOC0341 shader variants enumerate the runtime AOT set",
          "[shader_cache][doc0341]") {
    gv::VulkanStaticVariantInputs inputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };

    std::vector<gv::ShaderVariantKey> keys;
    gv::for_each_static_shader_variant(inputs, [&](gv::ShaderVariantKey key) {
        INFO("variant stem: " << gv::cache_file_stem(key));
        CHECK(gv::valid_shader_variant_key(key));
        keys.push_back(key);
    });

    REQUIRE(keys.size() == 36);
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return gv::cache_file_stem(a) < gv::cache_file_stem(b);
    });
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    CHECK(gv::choose_chaining_variant(inputs) == gv::ChainingPipelineKey{32u, 32u});
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 32u, 32u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seed_lookup_anchors, 384u, 128u, 256u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_sketch_tile, 384u, 64u, 10u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_coordinate_vote_reduce, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_coordinate_vote_reduce_batch, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_persisted_directory_select, 384u, 128u, 1u)));
    CHECK_FALSE(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::align_extend_simple_cigar, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_extract_key_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_tiebreak_generic, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_global_compare_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::ShaderKind::seq_sort_prefix_check_dna4, 384u, 256u, 0u)));

    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::chain_tiled_dp_u64, 384u, 256u, 0u)));
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 128u)));
}

TEST_CASE("grouped32 ABI2 preseed variants are explicit and inactive",
          "[shader_cache][grouped32][abi2][aot]") {
    std::vector<gv::ShaderVariantKey> keys;
    gv::for_each_inactive_grouped32_shader_variant(
        [&](gv::ShaderVariantKey key) { keys.push_back(key); });
    REQUIRE(keys.size() == 8u);
    CHECK(is_hex_digest(gv::grouped32_inactive_shader_set_digest_hex()));

    gv::VulkanStaticVariantInputs inputs{
        .max_workgroup_invocations = 256u,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10u,
    };
    std::vector<gv::ShaderVariantKey> active;
    gv::for_each_static_shader_variant(
        inputs, [&](gv::ShaderVariantKey key) { active.push_back(key); });
    for (const auto& key : keys) {
        CHECK(gv::valid_shader_variant_key(key));
        CHECK_FALSE(contains_key(active, key));
    }
}

TEST_CASE("format-4 grouped32 ABI2 pins every device wire",
          "[shader_cache][grouped32][abi2]") {
    STATIC_REQUIRE(gv::k_gpu_grouped32_abi_version == 2u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ControlWire) == 12u);
    STATIC_REQUIRE(offsetof(gv::GpuGrouped32ControlWire, hash) == 0u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32ControlWire, position_offset) == 4u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32ControlWire, position_count) == 8u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32PositionWire) == 4u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32PartitionWire) == 64u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32PartitionWire, directory_ordinal) == 4u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32PartitionWire, segment_base_bp) == 40u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32PartitionWire, reserved1) == 60u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryMinimizerWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryRecordWire) == 48u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32OccurrenceStateWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryStateWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ValidationStateWire) == 32u);
    STATIC_REQUIRE(sizeof(
        gv::GpuGrouped32DispatchIndirectCommandWire) == 12u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32GlobalAnchorWire) == 24u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32AnchorRadixKeyWire) == 16u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32SortSegmentWire) == 24u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32FinalResultCigarWire) == 88u);
    STATIC_REQUIRE(offsetof(
        gv::GpuGrouped32FinalResultCigarWire, batch_join_ordinal) == 64u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32Abi2Push) == 72u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32SketchQueryPlanWire) == 48u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryAdapterStateWire) == 32u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32QueryAdapterPush) == 64u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32IndirectPreparePush) == 48u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32DeviceIdentity) == 32u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ResidentIdentity) == 104u);
    STATIC_REQUIRE(sizeof(gv::GpuGrouped32ExecutionIdentity) == 120u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        gv::GpuGrouped32PrefixAllocationStrategy::coarse_query_scan) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
        gv::GpuGrouped32PrefixAllocationStrategy::hierarchical_segmented_scan)
        == 2u);
    STATIC_REQUIRE(std::is_standard_layout_v<
        gv::GpuGrouped32PartitionWire>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
        gv::GpuGrouped32ValidatedResidentProof>);
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<
        gv::GpuGrouped32ExecutionProof>);

    CHECK(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::phase_c_grouped32_abi2) == 13u);
    CHECK(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::phase_c_grouped32_adapter_abi2) == 9u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_accumulate_occurrences) == 72u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_adapt_query_sketch) == 64u);
    CHECK(gv::push_constant_size(
        gv::ShaderKind::phase_c_grouped32_prepare_indirect) == 48u);
    CHECK_FALSE(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 1u)));
    CHECK(gv::valid_shader_variant_key(gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u)));
}

TEST_CASE("format-4 grouped32 ABI2 inactive kernels compile and reflect",
          "[shader_cache][grouped32][abi2][reflection]") {
    const std::array kinds{
        gv::ShaderKind::phase_c_grouped32_validate,
        gv::ShaderKind::phase_c_grouped32_accumulate_occurrences,
        gv::ShaderKind::phase_c_grouped32_finalize_max_occ,
        gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix,
        gv::ShaderKind::phase_c_grouped32_emit_global_anchors,
        gv::ShaderKind::phase_c_grouped32_build_sort_segments,
        gv::ShaderKind::phase_c_grouped32_adapt_query_sketch,
        gv::ShaderKind::phase_c_grouped32_prepare_indirect,
    };
    for (const gv::ShaderKind kind : kinds) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 2u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::valid_shader_variant_key(key));
        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        REQUIRE(gv::validate_spirv_layout(key, spirv));
        const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
        REQUIRE(msl);
        CHECK(msl->wg_x == 128u);
        CHECK(msl->wg_y == 1u);
        CHECK(msl->wg_z == 1u);
    }

    const auto validate_key = gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 128u, 2u);
    const auto validate_spirv =
        gv::compile_shader_spirv(validate_key.to_request());
    const auto layout = gv::scan_spirv_layout(validate_spirv);
    CHECK(layout.has_binding_array_stride(0u, 0u, 64u));
    CHECK(layout.has_binding_struct_offsets(
        0u, 0u, std::array<std::uint32_t, 14>{
            0u, 4u, 8u, 12u, 16u, 20u, 24u,
            28u, 32u, 36u, 40u, 48u, 56u, 60u}));
    CHECK_FALSE(layout.has_binding_array_stride(0u, 0u, 12u));
    CHECK(layout.has_binding_array_stride(0u, 1u, 12u));
    CHECK(layout.has_push_constant_offsets(
        std::array<std::uint32_t, 18>{
            0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u,
            36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u}));
    const auto wrong_workgroup_key = gv::make_shader_key(
        gv::ShaderKind::phase_c_grouped32_validate, 384u, 64u, 2u);
    CHECK_FALSE(gv::spirv_to_msl(
        validate_spirv, wrong_workgroup_key, gv::MslVersion{}));

    const std::string count_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_accumulate_occurrences);
    const std::string finalize_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_finalize_max_occ);
    CHECK(count_source.find(
        "control.position_count <= pc.max_occ") == std::string::npos);
    CHECK(count_source.find(
        "separate finalize kernel decides acceptance") != std::string::npos);
    CHECK(finalize_source.find(
        "total <= pc.max_occ ? total : 0u") != std::string::npos);
    const std::string prefix_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix);
    CHECK(prefix_source.find("atomicCompSwap") == std::string::npos);
    CHECK(prefix_source.find("one complete query") != std::string::npos);
    const std::string adapter_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_adapt_query_sketch);
    CHECK(adapter_source.find("packed_words") != std::string::npos);
    CHECK(adapter_source.find("total_minimizers") != std::string::npos);
    const std::string prepare_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_prepare_indirect);
    CHECK(prepare_source.find("INDIRECT_COMMAND_READ") != std::string::npos);
    const std::string validate_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_validate);
    CHECK(validate_source.find("grouped32_zero_command(G32_STAGE_RADIX)")
        != std::string::npos);
    CHECK(validate_source.find("microbenchmark selects")
        != std::string::npos);
}

TEST_CASE("format-4 local32 positions globalize beyond 4 Gbp on both strands",
          "[grouped32][abi2][oracle]") {
    constexpr std::uint64_t base = (std::uint64_t{1} << 32u) + 1234u;
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 0u,
        .position_segment = 2u,
        .control_offset = 0u,
        .control_count = 1u,
        .position_offset = 0u,
        .position_count = 2u,
        .low_hash = 7u,
        .high_hash = 7u,
        .segment_base_bp = base,
        .segment_end_bp = base + 100u,
    };
    const auto forward = grouped32_globalize_oracle(
        partition,
        gv::GpuGrouped32PositionWire{.local_bp_strand = 17u << 1u});
    const auto reverse = grouped32_globalize_oracle(
        partition,
        gv::GpuGrouped32PositionWire{
            .local_bp_strand = (18u << 1u) | 1u});
    REQUIRE(forward);
    REQUIRE(reverse);
    CHECK(forward->bp == base + 17u);
    CHECK(forward->strand == 0u);
    CHECK(reverse->bp == base + 18u);
    CHECK(reverse->strand == 1u);
    CHECK(forward->bp > (std::numeric_limits<std::uint32_t>::max)());
}

TEST_CASE("format-4 max-occ is finalized once from the target-global sum",
          "[grouped32][abi2][oracle][max-occ]") {
    constexpr std::uint32_t max_occ = 5u;
    constexpr std::array segment_counts{3u, 4u};
    STATIC_REQUIRE(segment_counts[0] <= max_occ);
    STATIC_REQUIRE(segment_counts[1] <= max_occ);

    std::uint32_t global = 0u;
    for (const std::uint32_t segment_count : segment_counts) {
        global = grouped32_saturating_count_oracle(
            global, segment_count, max_occ);
    }
    CHECK(global == max_occ + 1u);
    CHECK(grouped32_finalize_count_oracle(global, max_occ) == 0u);
}

TEST_CASE("format-4 structural oracle rejects corrupt offsets and counts",
          "[grouped32][abi2][oracle][validation]") {
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 0u,
        .position_segment = 0u,
        .control_offset = 0u,
        .control_count = 2u,
        .position_offset = 0u,
        .position_count = 3u,
        .low_hash = 10u,
        .high_hash = 20u,
        .segment_base_bp = 0u,
        .segment_end_bp = 100u,
    };
    const std::array valid_controls{
        gv::GpuGrouped32ControlWire{
            .hash = 10u, .position_offset = 0u, .position_count = 2u},
        gv::GpuGrouped32ControlWire{
            .hash = 20u, .position_offset = 2u, .position_count = 1u},
    };
    const std::array valid_positions{
        gv::GpuGrouped32PositionWire{.local_bp_strand = 4u},
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
        gv::GpuGrouped32PositionWire{.local_bp_strand = 12u},
    };
    CHECK(grouped32_partition_validation_oracle(
        partition, valid_controls, valid_positions) == 0u);

    auto corrupt_offset = valid_controls;
    corrupt_offset[1].position_offset = 1u;
    CHECK((grouped32_partition_validation_oracle(
        partition, corrupt_offset, valid_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::control_positions)) != 0u);

    auto corrupt_count = valid_controls;
    corrupt_count[1].position_count = 2u;
    CHECK((grouped32_partition_validation_oracle(
        partition, corrupt_count, valid_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::control_positions)) != 0u);

    auto corrupt_descriptor = partition;
    corrupt_descriptor.control_offset = 2u;
    CHECK((grouped32_partition_validation_oracle(
        corrupt_descriptor, valid_controls, valid_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::descriptor_bounds)) != 0u);
}

TEST_CASE("grouped32 directory binds gapped stable IDs to dense ordinals",
          "[grouped32][abi2][oracle][identity]") {
    std::array partitions{
        gv::GpuGrouped32PartitionWire{
            .partition_id = 3u, .directory_ordinal = 0u},
        gv::GpuGrouped32PartitionWire{
            .partition_id = 17u, .directory_ordinal = 1u},
    };
    CHECK(grouped32_directory_identity_oracle(partitions) == 0u);

    auto duplicate = partitions;
    duplicate[1].partition_id = 3u;
    CHECK(grouped32_directory_identity_oracle(duplicate) != 0u);
    auto wrong_ordinal = partitions;
    wrong_ordinal[1].directory_ordinal = 17u;
    CHECK(grouped32_directory_identity_oracle(wrong_ordinal) != 0u);
}

TEST_CASE("grouped32 rejects duplicate positions and malformed q_last",
          "[grouped32][abi2][oracle][adversarial]") {
    const gv::GpuGrouped32PartitionWire partition{
        .partition_id = 3u,
        .directory_ordinal = 0u,
        .position_segment = 0u,
        .control_offset = 0u,
        .control_count = 1u,
        .position_offset = 0u,
        .position_count = 2u,
        .low_hash = 9u,
        .high_hash = 9u,
        .segment_base_bp = 0u,
        .segment_end_bp = 100u,
    };
    const std::array controls{gv::GpuGrouped32ControlWire{
        .hash = 9u, .position_offset = 0u, .position_count = 2u}};
    const std::array duplicate_positions{
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
        gv::GpuGrouped32PositionWire{.local_bp_strand = 8u},
    };
    CHECK((grouped32_partition_validation_oracle(
        partition, controls, duplicate_positions)
        & static_cast<std::uint32_t>(
            gv::GpuGrouped32ValidationError::position_order)) != 0u);

    const gv::GpuGrouped32QueryRecordWire record{
        .minimizer_offset = 0u,
        .minimizer_count = 1u,
        .query_length = 100u,
        .query_state_index = 0u,
    };
    CHECK_FALSE(grouped32_query_encoding_oracle(
        record, gv::GpuGrouped32QueryMinimizerWire{
            .query_bp_strand = 13u << 1u}));
    CHECK(grouped32_query_encoding_oracle(
        record, gv::GpuGrouped32QueryMinimizerWire{
            .query_bp_strand = 14u << 1u}));
    auto over_31_bit_query = record;
    over_31_bit_query.query_length = 0x80000001u;
    CHECK_FALSE(grouped32_query_encoding_oracle(
        over_31_bit_query, gv::GpuGrouped32QueryMinimizerWire{
            .query_bp_strand = 14u << 1u}));
}

TEST_CASE("grouped32 validation gate remains closed until both tokens accept",
          "[grouped32][abi2][oracle][gate]") {
    using Completion = gv::GpuGrouped32ValidationCompletion;
    gv::GpuGrouped32ValidationStateWire validation{};
    gv::GpuGrouped32QueryAdapterStateWire adapter{};
    CHECK_FALSE(grouped32_gate_oracle(validation, adapter));
    validation.completion = static_cast<std::uint32_t>(Completion::accepted);
    CHECK_FALSE(grouped32_gate_oracle(validation, adapter));
    adapter.completion = static_cast<std::uint32_t>(Completion::accepted);
    CHECK(grouped32_gate_oracle(validation, adapter));
    validation.status = 1u;
    CHECK_FALSE(grouped32_gate_oracle(validation, adapter));

    constexpr std::array downstream{
        gv::ShaderKind::phase_c_grouped32_accumulate_occurrences,
        gv::ShaderKind::phase_c_grouped32_finalize_max_occ,
        gv::ShaderKind::phase_c_grouped32_allocate_anchor_prefix,
        gv::ShaderKind::phase_c_grouped32_build_sort_segments,
        gv::ShaderKind::phase_c_grouped32_emit_global_anchors,
    };
    for (const auto kind : downstream) {
        const std::string source = gv::shader_body_for_digest(kind);
        CHECK(source.find(
            "if (!grouped32_validation_gate_open()) return;")
            != std::string::npos);
    }
    const std::string validate_source = gv::shader_body_for_digest(
        gv::ShaderKind::phase_c_grouped32_validate);
    CHECK(validate_source.find("COMPUTE_SHADER write -> INDIRECT_COMMAND_READ")
        != std::string::npos);
}

TEST_CASE("grouped32 keeps non-dense u64 BatchJoin ordinals and poisons moves",
          "[grouped32][abi2][proof][identity]") {
    constexpr std::uint64_t first = (std::uint64_t{1} << 40u) + 7u;
    constexpr std::uint64_t second = (std::uint64_t{1} << 52u) + 9u;
    gv::GpuGrouped32QueryRecordWire record{};
    record.query_state_index = 0u;
    record.batch_join_ordinal = second;
    gv::GpuGrouped32SortSegmentWire segment{};
    segment.query_state_index = record.query_state_index;
    segment.batch_join_ordinal = record.batch_join_ordinal;
    gv::GpuGrouped32FinalResultCigarWire result{};
    result.batch_join_ordinal = segment.batch_join_ordinal;
    CHECK(first < record.batch_join_ordinal);
    CHECK(result.batch_join_ordinal == second);

    auto resident =
        gv::GpuGrouped32ValidatedResidentProof::rejected_test_token();
    auto moved_resident = std::move(resident);
    CHECK(resident.moved_from());
    CHECK_FALSE(resident.accepted());
    CHECK_FALSE(moved_resident.accepted());
    auto execution = gv::GpuGrouped32ExecutionProof::rejected_test_token();
    auto moved_execution = std::move(execution);
    CHECK(execution.moved_from());
    CHECK_FALSE(execution.accepted());
    CHECK_FALSE(moved_execution.accepted());

    gv::GpuGrouped32DeviceIdentity device{};
    device.device_uuid[0] = 1u;
    device.device_kind = gv::GpuGrouped32PhysicalDeviceKind::cpu;
    CHECK_FALSE(gv::gpu_grouped32_hardware_device(device));
    device.device_kind = gv::GpuGrouped32PhysicalDeviceKind::integrated_gpu;
    CHECK(gv::gpu_grouped32_hardware_device(device));
}

TEST_CASE("format-4 target-wide sort input crosses partition boundaries",
          "[grouped32][abi2][oracle][ordering]") {
    constexpr std::uint64_t high_base =
        (std::uint64_t{1} << 32u) + 1000u;
    struct TaggedAnchor {
        gv::GpuGrouped32GlobalAnchorWire anchor{};
        std::uint32_t source_partition = 0u;
    };
    std::vector<TaggedAnchor> anchors{
        TaggedAnchor{
            .anchor = gv::GpuGrouped32GlobalAnchorWire{
                .ref_position = high_base + 9u,
                .query_position = 30u,
                .is_reverse = 0u,
                .query_span = 15u,
            },
            .source_partition = 4u,
        },
        TaggedAnchor{
            .anchor = gv::GpuGrouped32GlobalAnchorWire{
                .ref_position = 77u,
                .query_position = 12u,
                .is_reverse = 0u,
                .query_span = 15u,
            },
            .source_partition = 1u,
        },
        TaggedAnchor{
            .anchor = gv::GpuGrouped32GlobalAnchorWire{
                .ref_position = 33u,
                .query_position = 8u,
                .is_reverse = 1u,
                .query_span = 15u,
            },
            .source_partition = 0u,
        },
    };
    std::sort(anchors.begin(), anchors.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.anchor.is_reverse != rhs.anchor.is_reverse) {
            return lhs.anchor.is_reverse < rhs.anchor.is_reverse;
        }
        if (lhs.anchor.ref_position != rhs.anchor.ref_position) {
            return lhs.anchor.ref_position < rhs.anchor.ref_position;
        }
        return lhs.anchor.query_position < rhs.anchor.query_position;
    });

    REQUIRE(anchors.size() == 3u);
    CHECK(anchors[0].source_partition == 1u);
    CHECK(anchors[1].source_partition == 4u);
    CHECK(anchors[2].source_partition == 0u);
    CHECK(anchors[1].anchor.ref_position >
          (std::numeric_limits<std::uint32_t>::max)());
}

TEST_CASE("DOC0371 persisted directory selector shader compiles with exact bindings",
          "[shader_cache][doc0371][persisted-directory]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::phase_c_persisted_directory_select,
        384u, 128u, 1u);
    REQUIRE(gv::valid_shader_variant_key(key));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto layout = gv::validate_spirv_layout(key, spirv);
    REQUIRE(layout.has_value());
}

TEST_CASE("Phase-B seed lookup AOT shader pins six-buffer key-emission ABI",
          "[shader_cache][phase-b][seed-sort]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::seed_lookup_anchors, 384u, 128u, 256u);
    REQUIRE(gv::valid_shader_variant_key(key));
    REQUIRE(gv::descriptor_binding_count(
        gv::DescriptorLayoutKind::seed_lookup) == 6u);
    REQUIRE(gv::push_constant_size(
        gv::ShaderKind::seed_lookup_anchors) == 44u);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
}

TEST_CASE("score-only affine extension has one provider-compatible AOT variant",
          "[shader_cache][alignment-extension][no-fallback]") {
    const auto key = gv::make_shader_key(
        gv::ShaderKind::align_extend_banded_affine, 384u, 1u, 512u);
    REQUIRE(gv::valid_shader_variant_key(key));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::ShaderKind::align_extend_banded_affine);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::ShaderWorkgroupSize{1u, 1u, 1u}));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
    const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 1u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("fixed radix workgroups have one authoritative cache ABI",
          "[shader_cache][workgroup-abi]") {
    const std::array kinds{
        gv::ShaderKind::phase_c_sort_radix_histogram,
        gv::ShaderKind::phase_c_sort_radix_scan,
        gv::ShaderKind::phase_c_sort_radix_scatter,
    };
    for (const gv::ShaderKind kind : kinds) {
        const gv::ShaderVariantKey key =
            gv::make_shader_key(kind, 384u, 1u, 1u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::valid_shader_variant_key(key));

        // The established key uses local_size=1 as a selector, while the
        // algorithm has one invocation per radix bin. Reflection, validation,
        // and archive selection must agree on the compiled 256-lane geometry.
        const gv::ShaderWorkgroupSize expected =
            gv::expected_shader_workgroup_size(key);
        REQUIRE(expected == (gv::ShaderWorkgroupSize{256u, 1u, 1u}));

        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        const auto msl = gv::spirv_to_msl(spirv, key, gv::MslVersion{});
        REQUIRE(msl);
        CHECK(msl->wg_x == expected.x);
        CHECK(msl->wg_y == expected.y);
        CHECK(msl->wg_z == expected.z);
    }
}

TEST_CASE("DOC0341 shader cache digest changes with variant ABI inputs",
          "[shader_cache][doc0341]") {
    gv::ShaderCacheBuildInputs inputs{};
    inputs.variants = gv::VulkanStaticVariantInputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };
    inputs.moltenvk_build = "unit-test-moltenvk";
    inputs.device_uuid = "unit-test-device";

    const std::string digest_a = gv::shader_cache_digest_hex(inputs);
    const std::string digest_b = gv::shader_cache_digest_hex(inputs);
    REQUIRE(is_hex_digest(digest_a));
    CHECK(digest_a == digest_b);

    inputs.variants.default_window_size = 11;
    const std::string digest_c = gv::shader_cache_digest_hex(inputs);
    REQUIRE(is_hex_digest(digest_c));
    CHECK(digest_c != digest_a);
}

TEST_CASE("DOC0341 shader cache meta and MoltenVK header payload are exact",
          "[shader_cache][doc0341]") {
    TempDir temp;
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::generate_units, 384u, 256u, 64u);
    constexpr std::string_view entry = "main0";

    gv::ShaderCacheMeta meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());

    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    auto meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size()));
    REQUIRE(meta_write);

    const std::vector<std::byte> metallib = bytes_of("not-a-real-metallib");
    auto metallib_write = gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size()));
    REQUIRE(metallib_write);

    // cache_entry_complete also requires a non-empty .metalar binary archive on Apple.
    const std::vector<std::byte> fake_archive = bytes_of("not-a-real-metalar");
    auto archive_write = gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(fake_archive.data(), fake_archive.size()));
    REQUIRE(archive_write);
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    std::string decoded_entry;
    const auto decoded_meta = gv::read_meta(
        gv::cache_meta_path(temp.path, key), &decoded_entry);
    REQUIRE(decoded_meta);
    CHECK(decoded_meta->key == key);
    CHECK(decoded_meta->wg_x == 256u);
    CHECK(decoded_entry == entry);

    // A cache entry with the right key but a different compiled workgroup ABI
    // is not reusable.  This is the local compatibility check complementing
    // the digest-level resource-binding ABI revision.
    gv::ShaderCacheMeta incompatible_meta = meta;
    incompatible_meta.wg_x = 128u;
    const std::vector<std::byte> incompatible_meta_bytes =
        gv::meta_to_bytes(incompatible_meta, entry);
    auto incompatible_meta_write = gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(
            incompatible_meta_bytes.data(), incompatible_meta_bytes.size()));
    REQUIRE(incompatible_meta_write);
    CHECK_FALSE(gv::cache_metallib_entry_complete(temp.path, key));
    CHECK_FALSE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    const auto loader_rejected = gv::load_cached_module(
        VK_NULL_HANDLE, temp.path, key, &record_shader_module_create);
    REQUIRE_FALSE(loader_rejected);
    CHECK(loader_rejected.error().find("meta workgroup mismatch")
          != std::string::npos);
    CHECK_FALSE(shader_module_create_called);

    NoPublishCompileService no_publish_service{};
    const auto post_compile_rejected = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        no_publish_service);
    REQUIRE_FALSE(post_compile_rejected);
    CHECK(post_compile_rejected.error().find("complete compatible cache entry")
          != std::string::npos);
    CHECK(post_compile_rejected.error().find(
              "metadata incompatible: meta workgroup mismatch")
          != std::string::npos);
    CHECK(no_publish_service.calls == 1);
    CHECK_FALSE(shader_module_create_called);

    const auto module_code = gv::make_moltenvk_metallib_module_code(
        gv::cache_metallib_path(temp.path, key));
    REQUIRE(module_code);
    CHECK(module_code->header_size_bytes == gv::moltenvk_msl_spirv_header_size());
    CHECK(module_code->code_size_bytes
          == gv::moltenvk_msl_spirv_header_size() + metallib.size());
    CHECK(reinterpret_cast<std::uintptr_t>(module_code->words.data())
          % alignof(std::uint32_t) == 0u);

    const auto* submitted =
        reinterpret_cast<const std::byte*>(module_code->words.data());
    REQUIRE(module_code->header_size_bytes >= sizeof(std::uint32_t));
    std::uint32_t magic = 0;
    std::memcpy(&magic, submitted, sizeof(magic));
    CHECK(magic == gv::moltenvk_msl_compiled_code_magic());
    CHECK(std::memcmp(
              submitted + module_code->header_size_bytes,
              metallib.data(),
              metallib.size()) == 0);
}

TEST_CASE("fixed radix cache accepts reflected workgroup and exact archive",
          "[shader_cache][workgroup-abi]") {
    TempDir temp;
    const gv::ShaderVariantKey key = gv::make_shader_key(
        gv::ShaderKind::phase_c_sort_radix_histogram, 384u, 1u, 1u);
    constexpr std::string_view entry = "main0";

    gv::ShaderCacheMeta meta{};
    meta.key = key;
    meta.wg_x = 256u;
    meta.wg_y = 1u;
    meta.wg_z = 1u;
    meta.entry_len = static_cast<std::uint32_t>(entry.size());
    const std::vector<std::byte> meta_bytes = gv::meta_to_bytes(meta, entry);
    REQUIRE(gv::durable_write_file(
        gv::cache_meta_path(temp.path, key),
        std::span<const std::byte>(meta_bytes.data(), meta_bytes.size())));

    const std::vector<std::byte> metallib = bytes_of("fixed-radix-metallib");
    REQUIRE(gv::durable_write_file(
        gv::cache_metallib_path(temp.path, key),
        std::span<const std::byte>(metallib.data(), metallib.size())));
    const std::vector<std::byte> archive = bytes_of("fixed-radix-metalar");
    REQUIRE(gv::durable_write_file(
        gv::cache_binary_archive_path(temp.path, key),
        std::span<const std::byte>(archive.data(), archive.size())));

    // Publication uses reflected threads, so a 256-thread shader needs only
    // the exact base archive; completeness must not invent a duplicate
    // `.max_threads_256.metalar` from the key's sentinel local_size.
    CHECK(gv::cache_binary_archive_paths(temp.path, key).size() == 1u);
    REQUIRE(gv::cache_entry_status(temp.path, key));
    REQUIRE(gv::cache_entry_complete(temp.path, key));

    shader_module_create_called = false;
    NoPublishCompileService should_not_compile{};
    const auto loaded = gv::load_module(
        VK_NULL_HANDLE,
        temp.path,
        key,
        &record_shader_module_create,
        should_not_compile);
    REQUIRE(loaded);
    CHECK(loaded->wg_x == 256u);
    CHECK(should_not_compile.calls == 0);
    CHECK(shader_module_create_called);
}
