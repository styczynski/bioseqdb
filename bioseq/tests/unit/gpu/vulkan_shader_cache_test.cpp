/**
 * @file vulkan_shader_cache_test.cpp
 * @brief Tests shader variants, seed-job ABI, and cache-key construction.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <tests/support/common.hpp>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <vulkan/vulkan.h>

import bioseq.gpu.vulkan.shader_cache;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.gpu.vulkan.shader_layout;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.prefilter.types;

import bioseq.tests.unit.gpu.vulkan_shader_cache_support;

namespace gv = bioseq::gpu::vulkan;
namespace pf = bioseq::prefilter;
using namespace bioseq::tests::unit::gpu::vulkan_shader_cache_test_support;

TEST_CASE("shader variants enumerate the runtime AOT set",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::vulkan_static_variant_inputs inputs{
        .max_workgroup_invocations = 256,
        .max_shared_memory_bytes = 32u * 1024u,
        .default_window_size = 10,
    };

    std::vector<gv::shader_variant_key> keys;
    gv::for_each_static_shader_variant(inputs, [&keys](gv::shader_variant_key key) {
        INFO("variant stem: " << gv::cache_file_stem(key));
        CHECK(gv::is_shader_variant_key_valid(key));
        keys.push_back(key);
    });

    REQUIRE(keys.size() == 43);
    auto sorted = keys;
    std::sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) {
        return gv::cache_file_stem(left) < gv::cache_file_stem(right);
    });
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    CHECK(gv::choose_chaining_variant(inputs) == gv::chaining_pipeline_key{32u, 32u});
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 32u, 32u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seed_lookup_anchors, 384u, 128u, 256u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::ordered_sketch_tile, 384u, 64u, 10u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::coordinate_vote_reduce, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::coordinate_vote_reduce_batch, 384u, 128u, 1u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::grouped_soa_seed_count, 384u, 128u, 3u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::grouped_soa_seed_emit, 384u, 128u, 3u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::align_extend_banded_affine, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::align_extend_simple_cigar, 384u, 1u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::align_extend_simple_cigar, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 64u, 512u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l32_e2, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l32_e4, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l64_e2, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch_l64_e4, 384u, 64u, 128u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 64u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_extract_key_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_tiebreak_generic, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_global_compare_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_dense_scatter_dna4, 384u, 256u, 0u)));
    CHECK(contains_key(keys, gv::make_shader_key(
        gv::shader_kind::seq_sort_prefix_check_dna4, 384u, 256u, 0u)));

    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::chain_tiled_dp_u64, 384u, 256u, 0u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::generate_units, 384u, 256u, 128u)));
}

TEST_CASE("grouped-SoA current ABI direct seed shaders pin raw SoA and local-position ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(gv::k_gpu_grouped_soa_abi_revision == 3u);
    STATIC_REQUIRE(gv::k_gpu_grouped_soa_partition_required_flags == 7u);
    STATIC_REQUIRE(sizeof(
        gv::gpu_grouped_soa_resident_partition_descriptor) == 104u);
    STATIC_REQUIRE(alignof(
        gv::gpu_grouped_soa_resident_partition_descriptor) == 8u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, hash_byte_offset) == 16u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, control_count) == 48u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, low_hash) == 72u);
    STATIC_REQUIRE(offsetof(
        gv::gpu_grouped_soa_resident_partition_descriptor, segment_end_bp) == 96u);
    STATIC_REQUIRE(sizeof(gv::gpu_grouped_soa_seed_push) == 72u);
    STATIC_REQUIRE(offsetof(gv::gpu_grouped_soa_seed_push, status_index) == 68u);
    STATIC_REQUIRE(gv::encode_gpu_grouped_soa_seed_status(
        gv::gpu_grouped_soa_seed_status::contract) == 1u);
    STATIC_REQUIRE(gv::encode_gpu_grouped_soa_seed_status(
        gv::gpu_grouped_soa_seed_status::emission) == 16u);
    STATIC_REQUIRE(std::is_standard_layout_v<
        gv::gpu_grouped_soa_resident_partition_descriptor>);
    constexpr gv::gpu_grouped_soa_resident_partition_descriptor descriptor{
        .partition_id = 3u,
        .directory_ordinal = 0u,
        .position_segment = 0u,
        .flags = gv::k_gpu_grouped_soa_partition_required_flags,
        .hash_byte_offset = 0u,
        .prefix_byte_offset = 4u,
        .count_byte_offset = 12u,
        .position_byte_offset = 0u,
        .control_count = 2u,
        .position_count = 3u,
        .hash_width = 2u,
        .offset_width = 2u,
        .count_width = 2u,
        .position_width = 2u,
        .low_hash = 7u,
        .high_hash = 11u,
        .segment_base_bp = 100u,
        .segment_end_bp = 200u,
    };
    STATIC_REQUIRE(gv::is_gpu_grouped_soa_partition_descriptor_valid(
        descriptor, 16u, 6u));
    constexpr auto invalid_descriptor = [](auto value) {
        value.position_width = 1u;
        return value;
    }(descriptor);
    STATIC_REQUIRE_FALSE(gv::is_gpu_grouped_soa_partition_descriptor_valid(
        invalid_descriptor, 16u, 6u));

    CHECK(gv::descriptor_binding_count(
        gv::descriptor_layout_kind::grouped_soa_seed) == 9u);
    CHECK(gv::descriptor_layout_for(
        gv::shader_kind::grouped_soa_seed_count)
        == gv::descriptor_layout_kind::grouped_soa_seed);
    CHECK(gv::descriptor_layout_for(
        gv::shader_kind::grouped_soa_seed_emit)
        == gv::descriptor_layout_kind::grouped_soa_seed);
    CHECK(gv::push_constant_size(
        gv::shader_kind::grouped_soa_seed_count) == 72u);
    CHECK(gv::push_constant_size(
        gv::shader_kind::grouped_soa_seed_emit) == 72u);

    const std::array kinds{
        gv::shader_kind::grouped_soa_seed_count,
        gv::shader_kind::grouped_soa_seed_emit,
    };
    std::vector<gv::shader_variant_key> static_keys;
    gv::for_each_static_shader_variant(
        gv::vulkan_static_variant_inputs{},
        [&static_keys](gv::shader_variant_key key) { static_keys.push_back(key); });
    for (const gv::shader_kind kind : kinds) {
        const auto key = gv::make_shader_key(kind, 384u, 128u, 3u);
        CHECK(gv::checked_shader_kind(key.kind) == kind);
        CHECK(gv::is_shader_variant_key_valid(key));
        CHECK_FALSE(gv::is_shader_variant_key_valid(
            gv::make_shader_key(kind, 384u, 128u, 1u)));
        CHECK(std::count(static_keys.begin(), static_keys.end(), key) == 1);
        const gv::shader_compile_request request{
            .kind = kind,
            .tile_size = 384u,
            .local_size = 128u,
            .max_span = 3u,
        };
        const auto spirv = gv::compile_shader_spirv(request);
        REQUIRE_FALSE(spirv.empty());
        CHECK(spirv_opcode_count(spirv, 241u) > 0u);  // OpAtomicOr status.
        const auto layout = gv::scan_spirv_layout(spirv);
        CHECK(layout.has_binding_array_stride(0u, 1u, 104u));
        CHECK(layout.has_binding_struct_offsets(
            0u, 1u, std::array<std::uint32_t, 18>{
                0u, 4u, 8u, 12u, 16u, 24u, 32u, 40u, 48u,
                52u, 56u, 60u, 64u, 68u, 72u, 80u, 88u, 96u}));
        CHECK(layout.has_push_constant_offsets(
            std::array<std::uint32_t, 18>{
                0u, 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u,
                36u, 40u, 44u, 48u, 52u, 56u, 60u, 64u, 68u}));
    }
}

[[nodiscard]] gv::persisted_grouped_soa_coordinate_job_audit
make_grouped_soa_coordinate_hit_audit(
    const std::size_t problem_index,
    const std::uint64_t operation_identity)
{
    static gv::buffer resident_descriptors;
    static gv::buffer resident_controls;
    static gv::buffer resident_positions;
    static gv::buffer query_minimizers;
    const auto identity = [](const gv::buffer* object,
                             const std::uint64_t token,
                             const std::uint64_t bytes) {
        return gv::runtime_buffer_identity{
            .object = object,
            .owner = reinterpret_cast<const gv::runtime*>(1u),
            .buffer_handle = token,
            .memory_handle = token + 100u,
            .bytes = bytes,
            .allocation_bytes = bytes,
        };
    };
    gv::persisted_grouped_soa_coordinate_job_audit audit{};
    audit.operation_identity = operation_identity;
    audit.query_authority_identity = 200u;
    audit.resident_descriptors = identity(
        &resident_descriptors, 1u, 104u);
    audit.resident_controls = identity(&resident_controls, 2u, 64u);
    audit.resident_positions = identity(&resident_positions, 3u, 64u);
    audit.query_minimizer_buffer = identity(&query_minimizers, 4u, 64u);
    audit.device_uuid[0] = 1u;
    audit.descriptor_bytes = 104u;
    audit.control_bytes = 64u;
    audit.position_bytes = 64u;
    audit.position_count = 8u;
    audit.max_occ_cap = 500u;
    audit.partition_count = 1u;
    audit.abi_revision = gv::k_gpu_grouped_soa_abi_revision;
    audit.generation_id = 300u;
    audit.source_identity = 400u;
    audit.cold_load_identity = 500u;
    audit.target_len = 10'000u;
    audit.problem_index = problem_index;
    audit.query_index = static_cast<std::uint32_t>(problem_index);
    audit.logical_target_row = 600u + problem_index;
    audit.logical_query_ordinal = 700u + problem_index;
    audit.query_record = gv::gpu_seed_query_record_wire{
        .minimizer_offset = static_cast<std::uint32_t>(problem_index * 4u),
        .minimizer_count = 4u,
        .query_len = 120u,
        .flags = 0u,
    };
    audit.result = gv::gpu_coordinate_result_wire{
        .query_start = 1u,
        .query_end = 119u,
        .target_start = 100u + problem_index,
        .target_end = 218u + problem_index,
        .match_length = 118u,
        .block_length = 118u,
        .chain_score = 42,
        .seed_count = 7u,
        .is_reverse = 0u,
        .is_valid = 1u,
    };
    audit.configuration =
        gv::make_persisted_grouped_soa_coordinate_config_identity(
            15u,
            500,
            gv::gpu_seed_coordinate_algorithm::vulkan_tiled_dp,
            gv::chaining_pipeline_key{
                .tile_size = 384u,
                .local_size = 128u,
            },
            5'000,
            5'000,
            500,
            3'000,
            0.01F,
            0.005F,
            25,
            40,
            3,
            16u);
    audit.query_tile_count = 2u;
    audit.query_count_dispatches = 1u;
    audit.query_scan_dispatches = 3u;
    audit.query_emit_dispatches = 1u;
    audit.query_completed_roundtrips = 2u;
    audit.direct_count_dispatches = 1u;
    audit.direct_scan_dispatches = 3u;
    audit.direct_emit_dispatches = 1u;
    audit.anchor_count = 7u;
    audit.sort_operations = 1u;
    audit.range_build_dispatches = 1u;
    audit.range_count = 1u;
    audit.chain_dispatches = 1u;
    audit.chain_backtrack_dispatches = 1u;
    audit.coordinate_dispatches = 1u;
    const std::uint64_t receipt_base = 1'000u + problem_index * 10u;
    audit.count_submit_wait_identity = receipt_base + 1u;
    audit.emit_submit_wait_identity = receipt_base + 2u;
    audit.sort_range_submit_wait_identity = receipt_base + 3u;
    audit.chain_coordinate_submit_wait_identity = receipt_base + 4u;
    audit.count_status_checks = 1u;
    audit.emit_status_checks = 1u;
    audit.result_readbacks = 1u;
    return audit;
}

[[nodiscard]] gv::persisted_grouped_soa_coordinate_job_expectation
expectation_for_grouped_soa_coordinate_audit(
    const gv::persisted_grouped_soa_coordinate_job_audit& audit)
{
    return gv::persisted_grouped_soa_coordinate_job_expectation{
        .problem_index = audit.problem_index,
        .query_authority_identity = audit.query_authority_identity,
        .resident_descriptors = audit.resident_descriptors,
        .resident_controls = audit.resident_controls,
        .resident_positions = audit.resident_positions,
        .query_minimizer_buffer = audit.query_minimizer_buffer,
        .device_uuid = audit.device_uuid,
        .descriptor_bytes = audit.descriptor_bytes,
        .control_bytes = audit.control_bytes,
        .position_bytes = audit.position_bytes,
        .position_count = audit.position_count,
        .max_occ_cap = audit.max_occ_cap,
        .partition_count = audit.partition_count,
        .abi_revision = audit.abi_revision,
        .generation_id = audit.generation_id,
        .source_identity = audit.source_identity,
        .cold_load_identity = audit.cold_load_identity,
        .target_len = audit.target_len,
        .query_index = audit.query_index,
        .logical_target_row = audit.logical_target_row,
        .logical_query_ordinal = audit.logical_query_ordinal,
        .query_record = audit.query_record,
        .result = audit.result,
        .configuration = audit.configuration,
    };
}

TEST_CASE("seed jobs require strict XOR resident representation",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::buffer source_slots;
    gv::buffer source_positions;
    gv::gpu_grouped_soa_resident_view grouped_soa_view{};

    const gv::gpu_seed_coordinate_job source_direct{
        .slots = &source_slots,
        .positions = &source_positions,
        .target_len = 100u,
    };
    CHECK(gv::classify_gpu_seed_index_representation(source_direct)
          == gv::gpu_seed_index_representation::source_direct_sorted_dense);

    const gv::gpu_seed_coordinate_job grouped_soa{
        .target_len = 100u,
        .grouped_soa = &grouped_soa_view,
    };
    CHECK(gv::classify_gpu_seed_index_representation(grouped_soa)
          == gv::gpu_seed_index_representation::grouped_soa);

    const gv::gpu_seed_coordinate_job ambiguous{
        .slots = &source_slots,
        .positions = &source_positions,
        .target_len = 100u,
        .grouped_soa = &grouped_soa_view,
    };
    CHECK(gv::classify_gpu_seed_index_representation(ambiguous)
          == gv::gpu_seed_index_representation::invalid);

    const gv::gpu_seed_coordinate_job grouped_soa_with_source_metadata{
        .target_len = 100u,
        .position_count = 1u,
        .grouped_soa = &grouped_soa_view,
    };
    CHECK(gv::classify_gpu_seed_index_representation(
              grouped_soa_with_source_metadata)
          == gv::gpu_seed_index_representation::invalid);

    const gv::gpu_seed_coordinate_job incomplete_source{
        .slots = &source_slots,
        .target_len = 100u,
    };
    CHECK(gv::classify_gpu_seed_index_representation(incomplete_source)
          == gv::gpu_seed_index_representation::invalid);
}

TEST_CASE("persisted grouped-SoA coordinate receipt is strict and one-shot",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::device_query_sketch_authorization>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::device_query_sketch_authorization>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::persisted_grouped_soa_coordinate_operation_receipt>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
                   gv::persisted_grouped_soa_coordinate_operation_receipt>);
    STATIC_REQUIRE(!std::is_copy_constructible_v<
                   gv::persisted_grouped_soa_coordinate_job_receipt>);

    auto hit = make_grouped_soa_coordinate_hit_audit(0u, 100u);
    REQUIRE(hit.is_internally_consistent());
    const auto as_zero_minimizer_query = [](auto audit) {
        audit.query_record.minimizer_count = 0u;
        audit.result = {};
        audit.direct_count_dispatches = 0u;
        audit.direct_scan_dispatches = 0u;
        audit.direct_emit_dispatches = 0u;
        audit.anchor_count = 0u;
        audit.sort_operations = 0u;
        audit.range_build_dispatches = 0u;
        audit.range_count = 0u;
        audit.chain_dispatches = 0u;
        audit.chain_backtrack_dispatches = 0u;
        audit.coordinate_dispatches = 0u;
        audit.count_submit_wait_identity = 0u;
        audit.emit_submit_wait_identity = 0u;
        audit.sort_range_submit_wait_identity = 0u;
        audit.chain_coordinate_submit_wait_identity = 0u;
        audit.count_status_checks = 0u;
        audit.emit_status_checks = 0u;
        audit.result_readbacks = 0u;
        return audit;
    };

    SECTION("every mandatory dispatch and completion is required") {
        auto missing = hit;
        missing.query_count_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.query_scan_dispatches = 2u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.query_emit_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.query_completed_roundtrips = 1u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.direct_count_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.direct_scan_dispatches = 2u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.direct_emit_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.sort_operations = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.range_build_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.chain_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.chain_backtrack_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.coordinate_dispatches = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.result_readbacks = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.count_status_checks = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
        missing = hit;
        missing.emit_status_checks = 0u;
        CHECK_FALSE(missing.is_internally_consistent());
    }

    SECTION("tamper and forbidden-route counters fail closed") {
        auto tampered = hit;
        tampered.status_or = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.coordinate_vote_dispatches = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.cpu_or_simd_biological_calls = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.recompute_calls = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.fallback_calls = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.query_record.flags = 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.query_authority_identity = 0u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.resident_controls = {};
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.resident_controls.buffer_handle =
            hit.resident_descriptors.buffer_handle;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.resident_controls.memory_handle =
            hit.resident_descriptors.memory_handle;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.descriptor_bytes =
            hit.resident_descriptors.bytes + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.partition_count += 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.control_bytes = hit.resident_controls.bytes + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.position_bytes = hit.resident_positions.bytes + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.device_uuid = {};
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.configuration.coordinate_algorithm =
            gv::gpu_seed_coordinate_algorithm::coordinate_vote;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.configuration.probe_limit = 0u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.configuration.max_occ =
            static_cast<std::int32_t>(hit.max_occ_cap + 1u);
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.chain_coordinate_submit_wait_identity =
            hit.count_submit_wait_identity;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.is_valid = 2u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.is_reverse = 9u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.query_end = tampered.query_record.query_len + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.target_start = tampered.result.target_end;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.seed_count = -1;
        CHECK_FALSE(tampered.is_internally_consistent());
        tampered = hit;
        tampered.result.match_length = tampered.result.block_length + 1u;
        CHECK_FALSE(tampered.is_internally_consistent());
    }

    SECTION("data-dependent no-hit stages remain ineligible") {
        const auto zero_query = as_zero_minimizer_query(hit);
        CHECK_FALSE(zero_query.is_internally_consistent());
        auto zero_tile_query = zero_query;
        zero_tile_query.query_tile_count = 0u;
        CHECK_FALSE(zero_tile_query.is_internally_consistent());

        auto no_anchor = hit;
        no_anchor.result = {};
        no_anchor.direct_emit_dispatches = 0u;
        no_anchor.anchor_count = 0u;
        no_anchor.sort_operations = 0u;
        no_anchor.range_build_dispatches = 0u;
        no_anchor.range_count = 0u;
        no_anchor.chain_dispatches = 0u;
        no_anchor.chain_backtrack_dispatches = 0u;
        no_anchor.coordinate_dispatches = 0u;
        no_anchor.emit_submit_wait_identity = 0u;
        no_anchor.sort_range_submit_wait_identity = 0u;
        no_anchor.chain_coordinate_submit_wait_identity = 0u;
        no_anchor.emit_status_checks = 0u;
        no_anchor.result_readbacks = 0u;
        CHECK(no_anchor.is_internally_consistent());

        auto no_range = hit;
        no_range.result = {};
        no_range.range_count = 0u;
        no_range.chain_dispatches = 0u;
        no_range.chain_backtrack_dispatches = 0u;
        no_range.coordinate_dispatches = 0u;
        no_range.chain_coordinate_submit_wait_identity = 0u;
        no_range.result_readbacks = 0u;
        CHECK(no_range.is_internally_consistent());
    }

    SECTION("two logical jobs move and consume exactly once") {
        std::array audits{
            make_grouped_soa_coordinate_hit_audit(0u, 900u),
            make_grouped_soa_coordinate_hit_audit(1u, 900u),
        };
        std::array expectations{
            expectation_for_grouped_soa_coordinate_audit(audits[0]),
            expectation_for_grouped_soa_coordinate_audit(audits[1]),
        };
        CHECK(gv::persisted_grouped_soa_coordinate_operation_receipt::
              self_test_one_shot_contract(audits, expectations));

        auto unsupported_zero_audits = audits;
        unsupported_zero_audits[1] =
            as_zero_minimizer_query(unsupported_zero_audits[1]);
        std::array unsupported_zero_expectations{
            expectation_for_grouped_soa_coordinate_audit(
                unsupported_zero_audits[0]),
            expectation_for_grouped_soa_coordinate_audit(
                unsupported_zero_audits[1]),
        };
        CHECK_FALSE(gv::persisted_grouped_soa_coordinate_operation_receipt::
                    self_test_one_shot_contract(
                        unsupported_zero_audits,
                        unsupported_zero_expectations));

        const auto config_tamper_fails = [&audits, &expectations](auto mutate) {
            auto tampered = expectations;
            mutate(tampered[0].configuration);
            return !gv::persisted_grouped_soa_coordinate_operation_receipt::
                self_test_one_shot_contract(audits, tampered);
        };
        CHECK(config_tamper_fails([](auto& config) {
            config.kmer_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_occ += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.coordinate_algorithm =
                gv::gpu_seed_coordinate_algorithm::coordinate_vote;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.chain_key.tile_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.chain_key.local_size += 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_dist_x += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_dist_y += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.bandwidth += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_iter += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.gap_penalty_bits ^= 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.skip_penalty_bits ^= 1u;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.max_chain_skip += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.min_chain_score += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.min_seed_count += 1;
        }));
        CHECK(config_tamper_fails([](auto& config) {
            config.probe_limit += 1u;
        }));

        expectations[0].result.target_end += 1u;
        CHECK_FALSE(gv::persisted_grouped_soa_coordinate_operation_receipt::
                    self_test_one_shot_contract(audits, expectations));
    }
}

TEST_CASE("seed lookup AOT shader pins six-buffer key-emission ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto key = gv::make_shader_key(
        gv::shader_kind::seed_lookup_anchors, 384u, 128u, 256u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_kind::seed_lookup) == 6u);
    REQUIRE(gv::push_constant_size(
        gv::shader_kind::seed_lookup_anchors) == 44u);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
}

TEST_CASE("score-only affine extension has one provider-contract AOT variant",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const auto key = gv::make_shader_key(
        gv::shader_kind::align_extend_banded_affine, 384u, 1u, 512u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::shader_kind::align_extend_banded_affine);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::shader_workgroup_size{1u, 1u, 1u}));
    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    REQUIRE(gv::validate_spirv_layout(key, spirv));
    const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 1u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("SneakySnake rank batch has an exact provider ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(gv::k_gpu_sneaky_snake_submaze_length == 512u);
    STATIC_REQUIRE(gv::k_gpu_sneaky_snake_max_edits == 31u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(pf::prefilter_status::success)
                   == 0u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
                       pf::prefilter_status::conservative_unequal) == 1u);
    STATIC_REQUIRE(static_cast<std::uint32_t>(
                       pf::prefilter_status::unsupported_conservative) == 2u);
    STATIC_REQUIRE(sizeof(pf::prefilter_job) == 20u);
    STATIC_REQUIRE(alignof(pf::prefilter_job) == 4u);
    STATIC_REQUIRE(offsetof(pf::prefilter_job, max_edits) == 16u);
    STATIC_REQUIRE(sizeof(pf::prefilter_result) == 16u);
    STATIC_REQUIRE(alignof(pf::prefilter_result) == 4u);
    STATIC_REQUIRE(offsetof(pf::prefilter_result, is_accepted) == 8u);
    STATIC_REQUIRE(sizeof(gv::gpu_prefilter_push) == 16u);
    STATIC_REQUIRE(offsetof(gv::gpu_prefilter_push, mode) == 4u);
    STATIC_REQUIRE(offsetof(gv::gpu_prefilter_push, query_rank_count) == 8u);
    STATIC_REQUIRE(offsetof(gv::gpu_prefilter_push, target_rank_count) == 12u);

    const auto key = gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 64u, 512u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 32u, 512u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::sneaky_snake_rank_batch, 384u, 64u, 511u)));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::shader_kind::sneaky_snake_rank_batch);
    REQUIRE(gv::descriptor_layout_for(key.to_request().kind)
            == gv::descriptor_layout_kind::align_extension);
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_for(key.to_request().kind)) == 4u);
    REQUIRE(gv::push_constant_size(key.to_request().kind) == 16u);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::shader_workgroup_size{64u, 1u, 1u}));

    const std::string body = gv::shader_body_for_digest(
        gv::shader_kind::sneaky_snake_rank_batch);
    CHECK(body.find("k_maximum_supported_edits = 31u") != std::string::npos);
    CHECK(body.find("uint query_rank_count;") != std::string::npos);
    CHECK(body.find("uint target_rank_count;") != std::string::npos);
    CHECK(body.find("prefilter_job_is_in_bounds(job)") != std::string::npos);
    CHECK(body.find("subgroup") == std::string::npos);
    CHECK(body.find("atomic") == std::string::npos);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto scan = gv::scan_spirv_layout(spirv);
    CHECK(scan.has_binding_array_stride(0u, 0u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 1u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 2u, 20u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 2u,
        std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
    CHECK(scan.has_binding_array_stride(0u, 3u, 16u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 3u,
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    CHECK(scan.has_push_constant_offsets(
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    REQUIRE(gv::validate_spirv_layout(key, spirv));

    const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 64u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("GateKeeper generic and production shaders share one bounds-aware ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    STATIC_REQUIRE(gv::k_gpu_gatekeeper_max_length == 128u);
    STATIC_REQUIRE(gv::k_gpu_gatekeeper_max_edits == 8u);
    STATIC_REQUIRE(sizeof(pf::prefilter_job) == 20u);
    STATIC_REQUIRE(sizeof(pf::prefilter_result) == 16u);
    STATIC_REQUIRE(sizeof(gv::gpu_prefilter_push) == 16u);

    const auto key = gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 64u, 128u);
    REQUIRE(gv::is_shader_variant_key_valid(key));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 32u, 128u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 384u, 64u, 127u)));
    CHECK_FALSE(gv::is_shader_variant_key_valid(gv::make_shader_key(
        gv::shader_kind::gatekeeper_rank_batch, 383u, 64u, 128u)));
    REQUIRE(gv::checked_shader_kind(key.kind)
            == gv::shader_kind::gatekeeper_rank_batch);
    REQUIRE(gv::descriptor_layout_for(key.to_request().kind)
            == gv::descriptor_layout_kind::align_extension);
    REQUIRE(gv::descriptor_binding_count(
        gv::descriptor_layout_for(key.to_request().kind)) == 4u);
    REQUIRE(gv::push_constant_size(key.to_request().kind) == 16u);
    REQUIRE(gv::expected_shader_workgroup_size(key)
            == (gv::shader_workgroup_size{64u, 1u, 1u}));

    struct production_route_expectation {
        gv::shader_kind kind;
        std::uint32_t length;
        std::uint32_t max_edits;
        std::size_t replay_index;
    };
    const std::array production_routes{
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l32_e2, 32u, 2u, 3u},
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l32_e4, 32u, 4u, 4u},
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l64_e2, 64u, 2u, 5u},
        production_route_expectation{
            gv::shader_kind::gatekeeper_rank_batch_l64_e4, 64u, 4u, 6u},
    };

    std::array<bool, gv::k_rank_prefilter_replay_route_count>
        observed_replay_indices{};
    const auto check_replay_route = [
        &observed_replay_indices](
            gv::shader_kind kind,
            std::uint32_t push_mode,
            std::size_t expected_index) {
        const std::optional<std::size_t> replay_index =
            gv::rank_prefilter_replay_index(kind, push_mode);
        REQUIRE(replay_index);
        REQUIRE(*replay_index < observed_replay_indices.size());
        CHECK(*replay_index == expected_index);
        CHECK_FALSE(observed_replay_indices[*replay_index]);
        observed_replay_indices[*replay_index] = true;
    };
    check_replay_route(
        gv::shader_kind::sneaky_snake_rank_batch, 1u, 0u);
    check_replay_route(
        gv::shader_kind::sneaky_snake_rank_batch, 8u, 1u);
    check_replay_route(
        gv::shader_kind::gatekeeper_rank_batch, 0u, 2u);

    const gv::gatekeeper_rank_batch_route empty_route =
        gv::resolve_gatekeeper_rank_batch_route(
            std::span<const pf::prefilter_job>{});
    CHECK(empty_route.kind == gv::shader_kind::gatekeeper_rank_batch);
    CHECK(empty_route.replay_index == 2u);

    std::vector<std::string> rank_prefilter_cache_stems{
        gv::cache_file_stem(key)};
    for (const production_route_expectation& production_route
         : production_routes) {
        const gv::shader_kind production_kind = production_route.kind;
        const auto production_key = gv::make_shader_key(
            production_kind, 384u, 64u, 128u);
        const std::string production_cache_stem =
            gv::cache_file_stem(production_key);
        CAPTURE(production_cache_stem);
        CHECK(std::find(
            rank_prefilter_cache_stems.begin(),
            rank_prefilter_cache_stems.end(),
            production_cache_stem) == rank_prefilter_cache_stems.end());
        rank_prefilter_cache_stems.push_back(production_cache_stem);
        REQUIRE(gv::is_shader_variant_key_valid(production_key));
        REQUIRE(gv::descriptor_layout_for(production_kind)
                == gv::descriptor_layout_kind::align_extension);
        REQUIRE(gv::push_constant_size(production_kind) == 16u);
        check_replay_route(
            production_kind, 0u, production_route.replay_index);

        const std::array production_jobs{
            pf::prefilter_job{
                .query_offset = 0u,
                .target_offset = 0u,
                .query_length = production_route.length,
                .target_length = production_route.length,
                .max_edits = production_route.max_edits,
            },
            pf::prefilter_job{
                .query_offset = production_route.length,
                .target_offset = production_route.length,
                .query_length = production_route.length,
                .target_length = production_route.length,
                .max_edits = production_route.max_edits,
            },
        };
        const gv::gatekeeper_rank_batch_route resolved_route =
            gv::resolve_gatekeeper_rank_batch_route(production_jobs);
        CHECK(resolved_route.kind == production_kind);
        CHECK(resolved_route.replay_index == production_route.replay_index);

        const std::string production_body =
            gv::shader_body_for_digest(production_kind);
        CHECK(production_body.find(
            "#define gatekeeper_specialized_length "
            + std::to_string(production_route.length))
              != std::string::npos);
        CHECK(production_body.find(
            "#define gatekeeper_specialized_edits "
            + std::to_string(production_route.max_edits))
              != std::string::npos);

        const auto production_spirv =
            gv::compile_shader_spirv(production_key.to_request());
        REQUIRE_FALSE(production_spirv.empty());
        const auto production_scan =
            gv::scan_spirv_layout(production_spirv);
        CHECK(production_scan.has_binding_array_stride(0u, 0u, 4u));
        CHECK(production_scan.has_binding_array_stride(0u, 1u, 4u));
        CHECK(production_scan.has_binding_array_stride(0u, 2u, 20u));
        CHECK(production_scan.has_binding_struct_offsets(
            0u, 2u,
            std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
        CHECK(production_scan.has_binding_array_stride(0u, 3u, 16u));
        CHECK(production_scan.has_binding_struct_offsets(
            0u, 3u,
            std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
        CHECK(production_scan.has_push_constant_offsets(
            std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
        REQUIRE(gv::validate_spirv_layout(
            production_key, production_spirv));

        const auto production_msl = gv::spirv_to_msl(
            production_spirv, production_key, gv::msl_version{});
        REQUIRE(production_msl);
        CHECK(production_msl->wg_x == 64u);
        CHECK(production_msl->wg_y == 1u);
        CHECK(production_msl->wg_z == 1u);
    }
    CHECK(std::all_of(
        observed_replay_indices.begin(),
        observed_replay_indices.end(),
        [](bool observed) { return observed; }));
    CHECK_FALSE(gv::rank_prefilter_replay_index(
        gv::shader_kind::sneaky_snake_rank_batch, 0u));
    CHECK_FALSE(gv::rank_prefilter_replay_index(
        gv::shader_kind::gatekeeper_rank_batch, 1u));

    const std::array mixed_production_jobs{
        pf::prefilter_job{
            .query_offset = 0u,
            .target_offset = 0u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 2u,
        },
        pf::prefilter_job{
            .query_offset = 32u,
            .target_offset = 32u,
            .query_length = 64u,
            .target_length = 64u,
            .max_edits = 2u,
        },
    };
    const gv::gatekeeper_rank_batch_route mixed_route =
        gv::resolve_gatekeeper_rank_batch_route(mixed_production_jobs);
    CHECK(mixed_route.kind == gv::shader_kind::gatekeeper_rank_batch);
    CHECK(mixed_route.replay_index == 2u);

    const std::array unaligned_production_job{
        pf::prefilter_job{
            .query_offset = 1u,
            .target_offset = 0u,
            .query_length = 32u,
            .target_length = 32u,
            .max_edits = 2u,
        },
    };
    const gv::gatekeeper_rank_batch_route unaligned_route =
        gv::resolve_gatekeeper_rank_batch_route(unaligned_production_job);
    CHECK(unaligned_route.kind == gv::shader_kind::gatekeeper_rank_batch);
    CHECK(unaligned_route.replay_index == 2u);

    const std::string body = gv::shader_body_for_digest(
        gv::shader_kind::gatekeeper_rank_batch);
    CHECK(body.find("k_maximum_gatekeeper_length = 128u")
          != std::string::npos);
    CHECK(body.find("k_maximum_gatekeeper_edits = 8u")
          != std::string::npos);
    CHECK(body.find("prefilter_job_is_in_bounds(job)")
          != std::string::npos);
    CHECK(body.find("set_mask_bit(shifted_mask, shift - 1u)")
          != std::string::npos);
    CHECK(body.find("set_mask_bit(shifted_mask, length - shift)")
          != std::string::npos);
    CHECK(body.find("exact_edit_distance_is_above_threshold(job)")
          != std::string::npos);
    CHECK(body.find("k_hamming_exact_rejection_mask")
          != std::string::npos);
    CHECK(body.find("subgroup") == std::string::npos);
    CHECK(body.find("atomic") == std::string::npos);

    const auto spirv = gv::compile_shader_spirv(key.to_request());
    REQUIRE_FALSE(spirv.empty());
    const auto scan = gv::scan_spirv_layout(spirv);
    CHECK(scan.has_binding_array_stride(0u, 0u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 1u, 4u));
    CHECK(scan.has_binding_array_stride(0u, 2u, 20u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 2u,
        std::array<std::uint32_t, 5>{0u, 4u, 8u, 12u, 16u}));
    CHECK(scan.has_binding_array_stride(0u, 3u, 16u));
    CHECK(scan.has_binding_struct_offsets(
        0u, 3u,
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    CHECK(scan.has_push_constant_offsets(
        std::array<std::uint32_t, 4>{0u, 4u, 8u, 12u}));
    REQUIRE(gv::validate_spirv_layout(key, spirv));

    const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
    REQUIRE(msl);
    CHECK(msl->wg_x == 64u);
    CHECK(msl->wg_y == 1u);
    CHECK(msl->wg_z == 1u);
}

TEST_CASE("fixed radix workgroups have one authoritative cache ABI",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    const std::array kinds{
        gv::shader_kind::radix_sort_histogram,
        gv::shader_kind::radix_sort_scan,
        gv::shader_kind::radix_sort_scatter,
    };
    for (const gv::shader_kind kind : kinds) {
        const gv::shader_variant_key key =
            gv::make_shader_key(kind, 384u, 1u, 1u);
        CAPTURE(gv::cache_file_stem(key));
        REQUIRE(gv::is_shader_variant_key_valid(key));

        // The established key uses local_size=1 as a selector, while the
        // algorithm has one invocation per radix bin. Reflection, validation,
        // and archive selection must agree on the compiled 256-lane geometry.
        const gv::shader_workgroup_size expected =
            gv::expected_shader_workgroup_size(key);
        REQUIRE(expected == (gv::shader_workgroup_size{256u, 1u, 1u}));

        const auto spirv = gv::compile_shader_spirv(key.to_request());
        REQUIRE_FALSE(spirv.empty());
        const auto msl = gv::spirv_to_msl(spirv, key, gv::msl_version{});
        REQUIRE(msl);
        CHECK(msl->wg_x == expected.x);
        CHECK(msl->wg_y == expected.y);
        CHECK(msl->wg_z == expected.z);
    }
}

TEST_CASE("shader cache digest changes with variant ABI inputs",
          "[component:bioseq][layer:unit][feature:gpu_runtime]") {
    gv::shader_cache_build_inputs inputs{};
    inputs.variants = gv::vulkan_static_variant_inputs{
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
