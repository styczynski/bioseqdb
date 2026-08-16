/**
 * @file source_direct_test.cpp
 * @brief Tests source-direct GPU-index admission, fill, and cache restrictions.
 */

#include <cstring>

#include <vulkan/vulkan_core.h>

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.indexing.storage;
import bioseq.indexing.type;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.gpu.vulkan.abi;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;

import bioseq.tests.unit.indexing.gpu_minimizer_index_support;

using minimap_sketch_data = bioseq::indexing::minimap_sketch_data;
using bioseq::indexing::minimizer_hashmap_fragment_index;
namespace idx_ns = bioseq::indexing;
using namespace bioseq::tests::unit::indexing::gpu_minimizer_index_test_support;

namespace {

TEST_CASE("vulkan source-direct preflight uses exact allocation requirements",
          "[component:bioseq][layer:unit][feature:index]") {
    const auto plan = idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
        /*minimizer_count=*/5u,
        /*distinct_count=*/3u,
        /*max_storage_buffer_range=*/4096u,
        /*control_allocation_requirement_bytes=*/64u,
        /*position_allocation_requirement_bytes=*/80u);
    REQUIRE(plan.has_value());
    REQUIRE(plan->control_bytes == 48u);
    REQUIRE(plan->position_bytes == 40u);
    REQUIRE(plan->logical_resident_bytes == 88u);
    REQUIRE(plan->control_allocation_allowance_bytes == 16u);
    REQUIRE(plan->position_allocation_allowance_bytes == 40u);
    REQUIRE(plan->allocation_requirement_bytes == 144u);
    REQUIRE(plan->conservative_request_bytes == 144u);
    REQUIRE(idx_ns::gpu_source_direct_preflight_fits(*plan, 144u));
    REQUIRE_FALSE(idx_ns::gpu_source_direct_preflight_fits(*plan, 143u));

    const auto empty =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            0u, 0u, 4096u, 0u, 0u);
    REQUIRE(empty.has_value());
    REQUIRE(empty->canonical_empty);
    REQUIRE(idx_ns::gpu_source_direct_preflight_fits(*empty, 0u));

    const auto undersized =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            5u, 3u, 4096u, 47u, 40u);
    REQUIRE_FALSE(undersized.has_value());
    REQUIRE(undersized.error()
            == idx_ns::gpu_source_direct_materialization_error::
                device_abi_overflow);

    for (const auto [n, d] : {
            std::pair<std::uint64_t, std::uint64_t>{0u, 1u},
            std::pair<std::uint64_t, std::uint64_t>{1u, 0u},
            std::pair<std::uint64_t, std::uint64_t>{2u, 3u}}) {
        const auto invalid_shape =
            idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
                n, d, 4096u, 64u, 64u);
        REQUIRE_FALSE(invalid_shape.has_value());
        REQUIRE(invalid_shape.error()
                == idx_ns::gpu_source_direct_materialization_error::invalid_shape);
    }
    const auto invalid_empty_requirement =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            0u, 0u, 4096u, 1u, 0u);
    REQUIRE_FALSE(invalid_empty_requirement.has_value());
    REQUIRE(invalid_empty_requirement.error()
            == idx_ns::gpu_source_direct_materialization_error::invalid_shape);

    constexpr std::uint64_t u32_max =
        (std::numeric_limits<std::uint32_t>::max)();
    const auto range_overflow =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            u32_max + 1u, 1u,
            (std::numeric_limits<std::uint64_t>::max)(), 16u, 8u);
    REQUIRE_FALSE(range_overflow.has_value());
    REQUIRE(range_overflow.error()
            == idx_ns::gpu_source_direct_materialization_error::
                device_abi_overflow);

    const auto range_equality =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            2u, 1u, /*max_storage_buffer_range=*/16u, 16u, 16u);
    REQUIRE(range_equality.has_value());
    const auto range_one_over =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            2u, 1u, /*max_storage_buffer_range=*/15u, 16u, 16u);
    REQUIRE_FALSE(range_one_over.has_value());
    REQUIRE(range_one_over.error()
            == idx_ns::gpu_source_direct_materialization_error::
                storage_buffer_range_exceeded);

    constexpr std::uint64_t xxl_n = 93'308'896u;
    constexpr std::uint64_t xxl_d = 70'249'364u;
    constexpr std::uint64_t xxl_control = 1'123'989'824u;
    constexpr std::uint64_t xxl_positions = 746'471'168u;
    constexpr std::uint64_t allowance = 65'536u;
    const auto xxl =
        idx_ns::plan_gpu_source_direct_sorted_dense_with_requirements(
            xxl_n, xxl_d, 4'294'967'295u,
            xxl_control + allowance,
            xxl_positions + allowance);
    REQUIRE(xxl.has_value());
    REQUIRE(xxl->control_bytes == xxl_control);
    REQUIRE(xxl->position_bytes == xxl_positions);
    REQUIRE(xxl->logical_resident_bytes == 1'870'460'992u);
    REQUIRE(xxl->allowance_bytes == 131'072u);
    REQUIRE(xxl->conservative_request_bytes == 1'870'592'064u);

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    bioseq::parallelism::memory_permit no_permit{};
    const auto ineligible =
        idx_ns::materialize_sorted_dense_from_presorted_minimizers(
            runtime,
            std::span<const bioseq::minimizers::
                minimizer_representation<28u, 30u>>{},
            /*target_len=*/0u,
            idx_ns::gpu_source_direct_preflight{},
            idx_ns::gpu_minimizer_index_cache_key{},
            no_permit);
    REQUIRE_FALSE(ineligible.has_value());
    REQUIRE(ineligible.error().error
            == idx_ns::gpu_source_direct_materialization_error::
                representation_ineligible);
}

TEST_CASE("vulkan source-direct fill consumes each source position once",
          "[component:bioseq][layer:unit][feature:index]") {
    struct single_read_minimizer {
        std::uint64_t hash = 0u;
        std::uint64_t encoded = 0u;
        mutable std::uint32_t reads = 0u;

        [[nodiscard]] std::uint64_t kmer() const noexcept { return hash; }
        [[nodiscard]] std::uint64_t pos_strand_encoded() const {
            if (++reads != 1u) throw std::runtime_error("source reread");
            return encoded;
        }
    };
    std::vector<single_read_minimizer> source{
        {.hash = 10u, .encoded = 4u},
        {.hash = 10u, .encoded = 7u},
        {.hash = 20u, .encoded = 12u},
    };
    std::array<idx_ns::gpu_minimizer_slot, 2u> controls{};
    std::array<std::uint64_t, 3u> positions{};
    const auto fill_stats =
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const single_read_minimizer>{source},
            /*target_len=*/100u,
            idx_ns::gpu_source_direct_sorted_shape{
                .minimizer_count = source.size(),
                .distinct_count = controls.size(),
            },
            std::span{controls},
            std::span{positions});
    REQUIRE(fill_stats.source_checksum == fill_stats.mapped_checksum);
    REQUIRE(fill_stats.complete_source_traversals == 1u);
    REQUIRE(fill_stats.complete_mapped_traversals == 1u);
    REQUIRE(std::all_of(source.begin(), source.end(),
        [](const single_read_minimizer& minimizer) {
            return minimizer.reads == 1u;
        }));

    REQUIRE(controls[0].hash == 10u);
    REQUIRE(controls[0].pos_offset == 0u);
    REQUIRE(controls[0].pos_count == 2u);
    REQUIRE(controls[1].hash == 20u);
    REQUIRE(controls[1].pos_offset == 2u);
    REQUIRE(controls[1].pos_count == 1u);
    REQUIRE((positions == std::array<std::uint64_t, 3u>{4u, 7u, 12u}));

    std::vector<std::pair<std::uint64_t, std::uint64_t>> oracle_pairs{
        {10u, 4u}, {10u, 7u}, {20u, 12u}};
    minimizer_hashmap_fragment_index oracle;
    oracle.build_from_pairs(oracle_pairs, 100u);
    for (std::size_t control_index = 0u;
         control_index < controls.size(); ++control_index) {
        const auto& control = controls[control_index];
        const auto expected = oracle.lookup(control.hash);
        REQUIRE(expected.size() == control.pos_count);
        REQUIRE(std::equal(
            expected.begin(), expected.end(),
            positions.begin() + control.pos_offset));
    }
    REQUIRE(oracle.lookup(30u).empty());

    std::vector<single_read_minimizer> unsorted{
        {.hash = 20u, .encoded = 12u},
        {.hash = 10u, .encoded = 4u},
    };
    const auto rejected_shape = idx_ns::inspect_presorted_minimizer_source(
        std::span<const single_read_minimizer>{unsorted});
    REQUIRE_FALSE(rejected_shape.has_value());
    REQUIRE(rejected_shape.error()
            == idx_ns::gpu_source_direct_materialization_error::invalid_shape);

    constexpr std::uint64_t large_position =
        (std::uint64_t{1} << 62u) - 1u;
    std::vector<single_read_minimizer> boundary_source{
        {.hash = 99u, .encoded = (large_position << 1u) | 1u},
    };
    std::array<idx_ns::gpu_minimizer_slot, 1u> boundary_control{};
    std::array<std::uint64_t, 1u> boundary_position{};
    const auto boundary =
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const single_read_minimizer>{boundary_source},
            /*target_len=*/large_position + 1u,
            idx_ns::gpu_source_direct_sorted_shape{
                .minimizer_count = 1u,
                .distinct_count = 1u,
            },
            std::span{boundary_control},
            std::span{boundary_position});
    REQUIRE(boundary.source_checksum == boundary.mapped_checksum);
    REQUIRE(boundary_position[0] == (large_position << 1u) + 1u);

    std::vector<single_read_minimizer> out_of_range{
        {.hash = 1u, .encoded = (100u << 1u)},
    };
    std::array<idx_ns::gpu_minimizer_slot, 1u> rejected_control{};
    std::array<std::uint64_t, 1u> rejected_position{};
    REQUIRE_THROWS_AS(
        idx_ns::direct_fill_gpu_minimizer_index_from_presorted_source(
            std::span<const single_read_minimizer>{out_of_range},
            /*target_len=*/100u,
            idx_ns::gpu_source_direct_sorted_shape{
                .minimizer_count = 1u,
                .distinct_count = 1u,
            },
            std::span{rejected_control},
            std::span{rejected_position}),
        idx_ns::gpu_index_direct_fill_invariant);
}

TEST_CASE("vulkan source-direct cache keys cannot upload or replace",
          "[component:bioseq][layer:unit][feature:index]") {
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{0u};
    idx_ns::gpu_index_cache_identity identity{
        .table_size = 2u,
        .position_count = 3u,
        .operation_identity = 0x1234u,
        .alphabet_size = 4u,
        .kmer_size = 15u,
        .window_size = 10u,
        .minimizer_size = 8u,
        .minimizer_alignment = 8u,
        .minimizer_hash_bits = 30u,
        .minimizer_position_bits = 33u,
        .sorted_dense_abi_revision =
            idx_ns::k_minimizer_sorted_dense_gpu_abi_revision,
        .representation = idx_ns::gpu_index_representation::
            source_direct_sorted_dense,
        .source_direct_sorted_dense = true,
        .canonical_empty = false,
    };
    identity.device_uuid[0] = 1u;
    REQUIRE(idx_ns::gpu_source_direct_identity_well_formed(identity));
    const auto rejects_identity = [&identity](auto mutate) {
        auto candidate = identity;
        mutate(candidate);
        REQUIRE_FALSE(
            idx_ns::gpu_source_direct_identity_well_formed(candidate));
    };
    rejects_identity([](auto& value) { value.operation_identity = 0u; });
    rejects_identity([](auto& value) { value.table_size = 0u; });
    rejects_identity([](auto& value) { value.position_count = 0u; });
    rejects_identity([](auto& value) { value.table_size = 4u; });
    rejects_identity([](auto& value) { value.alphabet_size = 3u; });
    rejects_identity([](auto& value) { value.kmer_size = 14u; });
    rejects_identity([](auto& value) { value.window_size = 9u; });
    rejects_identity([](auto& value) { value.minimizer_size = 16u; });
    rejects_identity([](auto& value) { value.minimizer_alignment = 16u; });
    rejects_identity([](auto& value) { value.minimizer_hash_bits = 29u; });
    rejects_identity([](auto& value) { value.minimizer_position_bits = 32u; });
    rejects_identity([](auto& value) { ++value.sorted_dense_abi_revision; });
    rejects_identity([](auto& value) {
        value.representation = idx_ns::gpu_index_representation::
            open_addressed_70;
    });
    rejects_identity([](auto& value) { value.device_uuid = {}; });
    rejects_identity([](auto& value) {
        value.source_direct_sorted_dense = false;
    });
    rejects_identity([](auto& value) { value.canonical_empty = true; });
    rejects_identity([](auto& value) { value.gate_enabled = true; });

    minimizer_hashmap_fragment_index placeholder;
    placeholder.target_len = 100u;
    const auto key = idx_ns::make_gpu_index_cache_key(
        placeholder, identity);
    const auto handle =
        std::make_shared<idx_ns::gpu_resident_minimizer_index>();
    handle->key = key;
    handle->gate_identity = idx_ns::gpu_index_gate_identity_from_key(key);
    REQUIRE(cache.insert_built(key, handle).has_value());
    const auto duplicate = cache.insert_built(key, handle);
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error() == cache_type::built_insert_error::duplicate_direct_key);

    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    const auto cpu_index = build_demo_index();
    REQUIRE_THROWS_AS(
        cache.get_or_upload(
            runtime, cpu_index, key, /*cache_enabled=*/true),
        bioseq::gpu::vulkan::vulkan_runtime_error);

    auto zero_uuid_key = key;
    zero_uuid_key.device_uuid = {};
    const auto invalid_lookup = cache.find_required_preinserted(
        runtime, zero_uuid_key);
    REQUIRE_FALSE(invalid_lookup.has_value());
    REQUIRE(invalid_lookup.error()
            == cache_type::required_preinsert_lookup_error::invalid_key);

    auto runtime_identity = identity;
    runtime_identity.operation_identity = 0x5678u;
    runtime_identity.device_uuid = runtime.features().device_uuid;
    if (idx_ns::gpu_source_direct_identity_well_formed(runtime_identity)) {
        cache_type required_cache{0u};
        const auto runtime_key = idx_ns::make_gpu_index_cache_key(
            placeholder, runtime_identity);
        const auto miss = required_cache.find_required_preinserted(
            runtime, runtime_key);
        REQUIRE_FALSE(miss.has_value());
        REQUIRE(miss.error() == cache_type::required_preinsert_lookup_error::miss);
        REQUIRE(required_cache.resident_bytes() == 0u);

        const auto corrupt =
            std::make_shared<idx_ns::gpu_resident_minimizer_index>();
        corrupt->key = runtime_key;
        corrupt->gate_identity =
            idx_ns::gpu_index_gate_identity_from_key(runtime_key);
        REQUIRE(required_cache.insert_built(runtime_key, corrupt).has_value());
        const auto rejected = required_cache.find_required_preinserted(
            runtime, runtime_key);
        REQUIRE_FALSE(rejected.has_value());
        REQUIRE(rejected.error()
                == cache_type::required_preinsert_lookup_error::resident_mismatch);
    }
}

} // namespace
