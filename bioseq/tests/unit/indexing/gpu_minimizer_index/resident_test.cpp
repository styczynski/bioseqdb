/**
 * @file resident_test.cpp
 * @brief Tests resident GPU-index upload, adoption, and publication ownership.
 */

#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <vulkan/vulkan_core.h>

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

TEST_CASE("vulkan index: resident upload + cache reuse (device path)",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    auto idx = build_demo_index();
    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{64} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(idx, idx_ns::gpu_index_cache_identity{});

    idx_ns::gpu_index_upload_decline diagnostic{};
    auto handle1 = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &diagnostic);
    ASSERT(handle1 != nullptr, "resident upload returned nullptr on a usable device", 0);
    ASSERT(handle1->target_len == idx.target_len, "resident target_len mismatch", 0);
    ASSERT(handle1->position_count == idx.positions.size(),
           "resident position_count mismatch", 0);
    REQUIRE(diagnostic.reason == idx_ns::gpu_index_upload_decline_reason::none);
    REQUIRE(diagnostic.unique_key_count == idx.table_size());
    REQUIRE(diagnostic.slot_capacity == idx_ns::gpu_slot_capacity(idx.table_size()));
    REQUIRE(diagnostic.load_ppm <= 700'000u);
    REQUIRE(diagnostic.max_probe == handle1->max_probe);
    REQUIRE(diagnostic.staging_bytes == 0u);
    REQUIRE(diagnostic.upload_peak_bytes == 0u);
    REQUIRE(handle1->representation
            == idx_ns::gpu_index_representation::open_addressed_70);
    REQUIRE(handle1->unique_key_count == idx.table_size());
    REQUIRE(handle1->slot_capacity == diagnostic.slot_capacity);
    REQUIRE(handle1->materialization.admitted_bytes
            == handle1->slots.allocation_bytes
                + handle1->positions.allocation_bytes);
    REQUIRE(handle1->materialization.allocation_snapshot.actual_allocation_bytes
            == handle1->materialization.admitted_bytes);
    REQUIRE(diagnostic.actual_allocation_bytes
            == handle1->materialization.allocation_snapshot.actual_allocation_bytes);
    REQUIRE(diagnostic.fully_touched_bytes
            == handle1->materialization.allocation_snapshot.fully_touched_bytes);
    REQUIRE(diagnostic.reflected_footprint_delta_bytes
            == handle1->materialization.reflected_delta_bytes);
    REQUIRE(diagnostic.materialization_permit_released
            == handle1->materialization.is_permit_released);
    REQUIRE(diagnostic.process_footprint_reflected
            == handle1->materialization.allocation_snapshot.is_process_footprint_reflected);
    if (handle1->materialization.is_permit_released) {
        REQUIRE(handle1->permit.bytes() == 0u);
        REQUIRE(handle1->materialization.reflected_delta_bytes
                >= handle1->materialization.admitted_bytes);
    } else {
        REQUIRE(handle1->permit.bytes()
                == handle1->materialization.admitted_bytes);
    }

    idx_ns::gpu_index_upload_decline cache_hit_diagnostic{};
    auto handle2 = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &cache_hit_diagnostic);
    ASSERT(handle2.get() == handle1.get(),
           "cache must return the same resident handle for a repeated key", 0);
    REQUIRE(cache_hit_diagnostic.reason
            == idx_ns::gpu_index_upload_decline_reason::none);
    REQUIRE(cache_hit_diagnostic.slot_capacity == 0u);
    REQUIRE(cache_hit_diagnostic.actual_allocation_bytes == 0u);

    cache.clear();
}

TEST_CASE("vulkan index: admission cliff selects and attests sorted-dense device layout",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    constexpr std::uint64_t distinct = 100'000u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(distinct);
    for (std::uint64_t i = 0u; i < distinct; ++i) {
        pairs.emplace_back(i * 0x9e3779b97f4a7c15ULL, i << 1u);
    }
    minimizer_hashmap_fragment_index idx;
    idx.build_from_pairs(pairs, 1'000'000u);
    const auto open = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::gpu_index_representation::open_addressed_70);
    const auto dense = idx_ns::plan_gpu_index_materialization(
        idx.table_size(), idx.positions.size(),
        idx_ns::gpu_index_representation::source_direct_sorted_dense);
    REQUIRE(dense.resident_bytes + (std::uint64_t{1} << 20)
            < open.resident_bytes);

    index_memory_cap_scope compact_only_cap{
        dense.resident_bytes + (std::uint64_t{1} << 20)};
    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{16} << 20};
    const auto key = idx_ns::make_gpu_index_cache_key(
        idx, idx_ns::gpu_index_cache_identity{});
    idx_ns::gpu_index_upload_decline diagnostic{};
    auto handle = cache.get_or_upload(
        runtime, idx, key, /*cache_enabled=*/true, &diagnostic);

    REQUIRE(handle != nullptr);
    REQUIRE(diagnostic.reason == idx_ns::gpu_index_upload_decline_reason::none);
    REQUIRE(diagnostic.representation
            == idx_ns::gpu_index_representation::source_direct_sorted_dense);
    REQUIRE(diagnostic.resident_bytes == dense.resident_bytes);
    REQUIRE(diagnostic.slot_capacity == distinct);
    REQUIRE(diagnostic.load_ppm == 1'000'000u);
    REQUIRE(diagnostic.max_probe == 0u);
    REQUIRE(diagnostic.staging_bytes == 0u);
    REQUIRE(handle->representation
            == idx_ns::gpu_index_representation::source_direct_sorted_dense);
    REQUIRE(handle->slot_mask == 0u);
    REQUIRE(handle->bucket_count == distinct);
    REQUIRE(handle->unique_key_count == distinct);
    REQUIRE(handle->slot_capacity == distinct);

    const auto mapped_slots = std::span<const idx_ns::gpu_minimizer_slot>{
        static_cast<const idx_ns::gpu_minimizer_slot*>(handle->slots.mapped),
        static_cast<std::size_t>(handle->bucket_count)};
    for (const std::uint64_t ordinal : {0u, 1u, 50'000u, 99'999u}) {
        const std::uint64_t hash = ordinal * 0x9e3779b97f4a7c15ULL;
        const std::uint32_t slot = host_find_sorted_dense_slot(
            mapped_slots, hash);
        REQUIRE(slot != 0xffffffffu);
        REQUIRE(mapped_slots[slot].pos_count == 1u);
    }
    cache.clear();
}

TEST_CASE("vulkan index: GPU-built adoption transfers retained ownership without re-reserve",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t before = admission.reserved_bytes();
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    const std::uint64_t with_built = admission.reserved_bytes();
    REQUIRE(with_built > before);
    index_memory_cap_scope tiny_cap{1u};
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->ownership.resident_transfers == 1u);
    REQUIRE((*adopted)->ownership.adoption_rereserve_attempts == 0u);
    REQUIRE((*adopted)->ownership.adoption_successes == 1u);
    REQUIRE(admission.reserved_bytes() == with_built);
    REQUIRE_FALSE(built.valid);
    REQUIRE_FALSE(built.slots.is_valid());
    REQUIRE_FALSE(built.positions.is_valid());
    adopted->reset();
    REQUIRE(admission.reserved_bytes() == before);
}

TEST_CASE("vulkan index: GPU-built move assignment releases destination after buffers",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<
        bioseq::gpu::vulkan::gpu_built_index>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<
        bioseq::gpu::vulkan::gpu_built_index>);
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t before = admission.reserved_bytes();
    {
        auto destination = make_adoptable_gpu_built_index(
            runtime, /*gate_enabled=*/false);
        const std::uint64_t one_resident = destination.ownership.resident_bytes;
        auto source = make_adoptable_gpu_built_index(
            runtime, /*gate_enabled=*/false);
        REQUIRE(admission.reserved_bytes() == before + 2u * one_resident);
        destination = std::move(source);
        REQUIRE(destination.valid);
        REQUIRE_FALSE(source.valid);
        REQUIRE_FALSE(source.slots.is_valid());
        REQUIRE_FALSE(source.positions.is_valid());
        REQUIRE_FALSE(static_cast<bool>(source.permit));
        REQUIRE(admission.reserved_bytes() == before + one_resident);
    }
    REQUIRE(admission.reserved_bytes() == before);
}

TEST_CASE("vulkan index: adoption rejects inconsistent stats without consuming source",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    built.materialization.allocation_snapshot.fully_touched_bytes--;
    built.ownership.materialization_fully_touched_bytes--;
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error()
            == idx_ns::gpu_built_index_adoption_error::stats_mismatch);
    REQUIRE(built.valid);
    REQUIRE(built.slots.is_valid());
    REQUIRE(built.positions.is_valid());
    REQUIRE(static_cast<bool>(built.permit));

    built.materialization.allocation_snapshot.fully_touched_bytes++;
    built.ownership.materialization_fully_touched_bytes++;
    built.occupied_slots = built.position_count + 1u;
    built.ownership.distinct_minimizer_count = built.occupied_slots;
    adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error() == idx_ns::gpu_built_index_adoption_error::invalid_build);
    REQUIRE(built.valid);
    REQUIRE(built.slots.is_valid());
    REQUIRE(built.positions.is_valid());

    built.occupied_slots = built.position_count;
    built.ownership.distinct_minimizer_count = built.occupied_slots;
    built.target_len = built.position_count - 1u;
    adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error() == idx_ns::gpu_built_index_adoption_error::invalid_build);
    REQUIRE(built.valid);
}

TEST_CASE("vulkan index: adoption rejects tampered deterministic target-build stats",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::uint32_t stats_field_count = 12u;
    for (std::uint32_t field = 0u; field < stats_field_count; ++field) {
        auto built = make_adoptable_gpu_built_index(
            runtime, /*gate_enabled=*/false);
        switch (field) {
            case 0u: built.ownership.exact_count_dispatches = 0u; break;
            case 1u:
                built.ownership.exact_count_prefix_scan_dispatches = 2u;
                break;
            case 2u:
                built.ownership.exact_count_control_readbacks = 0u;
                break;
            case 3u:
                built.ownership.deterministic_tile_emit_dispatches = 0u;
                break;
            case 4u: built.ownership.atomic_append_dispatches = 1u; break;
            case 5u:
                built.ownership.emit_validation_control_readbacks = 0u;
                break;
            case 6u: built.ownership.counted_minimizer_count = 1u; break;
            case 7u: built.ownership.verified_emit_count = 1u; break;
            case 8u: built.ownership.count_emit_parity_verified = 0u; break;
            case 9u:
                built.ownership.arena_trimmed_before_permit_shrink = 0u;
                break;
            case 10u:
                built.ownership.target_minimizer_capacity_estimate = 0u;
                break;
            case 11u:
                built.ownership.capacity_estimate_underestimated = 1u;
                break;
            default: FAIL("unreachable stats field");
        }
        INFO("tampered stats field ordinal=" << field);
        auto adopted = idx_ns::adopt_gpu_built_index(
            runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
        REQUIRE_FALSE(adopted.has_value());
        REQUIRE(adopted.error()
                == idx_ns::gpu_built_index_adoption_error::stats_mismatch);
        REQUIRE(built.valid);
        REQUIRE(built.slots.is_valid());
        REQUIRE(built.positions.is_valid());
    }
}

TEST_CASE("vulkan index: ungated T-zero sentinel adopts without an emit dispatch",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false, /*empty=*/true);
    REQUIRE(built.ownership.ungated_emit_dispatches == 0u);
    REQUIRE(built.ownership.deterministic_tile_emit_dispatches == 0u);
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), idx_ns::gpu_minimizer_index_cache_key{});
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
    REQUIRE((*adopted)->ownership.counted_minimizer_count == 0u);
    REQUIRE((*adopted)->ownership.verified_emit_count == 0u);
}

TEST_CASE("vulkan index: released materialization and gated T-zero sentinel adopt",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true, /*empty=*/true);
    const auto bytes = built.ownership.resident_bytes;
    built.permit.release();
    built.materialization.is_permit_released = true;
    built.materialization.allocation_snapshot.is_process_footprint_reflected = true;
    built.materialization.reflected_delta_bytes = bytes;
    built.ownership.materialization_permit_released = 1u;
    built.ownership.materialization_process_footprint_reflected = 1u;
    built.ownership.materialization_reflected_delta_bytes = bytes;
    const auto gate_identity = built.gate_identity;
    idx_ns::gpu_minimizer_index_cache_key gated_key{};
    gated_key.gate_enabled = true;
    gated_key.gate_word_count = gate_identity.word_count;
    gated_key.gate_mask = gate_identity.mask;
    gated_key.gate_digest_lo = 11u;
    gated_key.gate_digest_hi = 12u;
    gated_key.gate_canonical_words = gate_identity.canonical_words;
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), gated_key);
    REQUIRE(adopted.has_value());
    REQUIRE((*adopted)->position_count == 0u);
    REQUIRE((*adopted)->bucket_count == 0u);
    REQUIRE((*adopted)->ownership.gate_count_dispatches == 1u);
    REQUIRE((*adopted)->ownership.gated_emit_dispatches == 0u);
    REQUIRE((*adopted)->ownership.ungated_emit_dispatches == 0u);
}

TEST_CASE("vulkan index: Gate-A build cannot adopt under forced-collision Gate-B key",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true);
    auto colliding_words = std::make_shared<std::vector<std::uint32_t>>(
        *built.gate_identity.canonical_words);
    (*colliding_words)[7] ^= 1u;
    idx_ns::gpu_minimizer_index_cache_key colliding_key{
        .gate_enabled = true,
        .gate_word_count = built.gate_identity.word_count,
        .gate_mask = built.gate_identity.mask,
        .gate_digest_lo = built.gate_identity.digest_lo,
        .gate_digest_hi = built.gate_identity.digest_hi,
        .gate_canonical_words = std::move(colliding_words),
    };
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), colliding_key);
    REQUIRE_FALSE(adopted.has_value());
    REQUIRE(adopted.error()
            == idx_ns::gpu_built_index_adoption_error::stats_mismatch);
    REQUIRE(built.valid);
    REQUIRE(built.slots.is_valid());
    REQUIRE(built.positions.is_valid());
    REQUIRE(static_cast<bool>(built.permit));
}

TEST_CASE("vulkan index: Gate-B-bound cache rejects adopted Gate-A resident on digest collision",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/true);
    const auto gate_a = built.gate_identity;
    idx_ns::gpu_minimizer_index_cache_key key_a{
        .gate_enabled = true,
        .gate_word_count = gate_a.word_count,
        .gate_mask = gate_a.mask,
        .gate_digest_lo = gate_a.digest_lo,
        .gate_digest_hi = gate_a.digest_hi,
        .gate_canonical_words = gate_a.canonical_words,
    };
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), key_a);
    REQUIRE(adopted.has_value());
    auto resident_a = std::move(*adopted);

    auto words_b_mut = std::make_shared<std::vector<std::uint32_t>>(
        *gate_a.canonical_words);
    (*words_b_mut)[9] ^= 1u;
    const std::shared_ptr<const std::vector<std::uint32_t>> words_b =
        std::move(words_b_mut);
    using cache_type = idx_ns::gpu_minimizer_index_cache;
    cache_type cache{std::uint64_t{64} << 20};
    REQUIRE(cache.bind_gate_once(cache_type::gate_identity{
        .enabled = true,
        .word_count = gate_a.word_count,
        .mask = gate_a.mask,
        .digest_lo = gate_a.digest_lo,
        .digest_hi = gate_a.digest_hi,
        .canonical_words = words_b,
    }).has_value());
    auto key_b = key_a;
    key_b.gate_canonical_words = words_b;
    const auto inserted = cache.insert_built(key_b, resident_a);
    REQUIRE_FALSE(inserted.has_value());
    REQUIRE(inserted.error() == cache_type::built_insert_error::gate_identity_mismatch);
}

TEST_CASE("vulkan index: cache cannot publish a resident under another target key",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    auto built = make_adoptable_gpu_built_index(
        runtime, /*gate_enabled=*/false);
    idx_ns::gpu_minimizer_index_cache_key key_a{.source_id = 101u};
    auto adopted = idx_ns::adopt_gpu_built_index(
        runtime, std::move(built), key_a);
    REQUIRE(adopted.has_value());
    auto resident = std::move(*adopted);
    auto key_b = key_a;
    key_b.source_id = 202u;
    idx_ns::gpu_minimizer_index_cache cache{std::uint64_t{64} << 20};
    const auto inserted = cache.insert_built(key_b, resident);
    REQUIRE_FALSE(inserted.has_value());
    REQUIRE(inserted.error()
            == idx_ns::gpu_minimizer_index_cache::built_insert_error::
                resident_key_mismatch);
}

TEST_CASE("grouped-SoA cold resident preserves raw SoA and local positions",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    STATIC_REQUIRE_FALSE(
        idx_ns::gpu_grouped_soa_resident_target::permits_host_decode_or_rebuild());
    STATIC_REQUIRE_FALSE(
        idx_ns::gpu_grouped_soa_resident_target::qualifies_as_all_gpu_execution());
    const auto fixture = make_grouped_soa_gpu_resident_fixture();
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    constexpr std::uint64_t cold_load_identity = 0xC01DCAC4EULL;
    auto ingest = idx_ns::gpu_grouped_soa_generation_ingest::begin(
        runtime, fixture.head, fixture.manifest,
        grouped_soa_gpu_expected_identity(), fixture.directory,
        cold_load_identity);
    REQUIRE(ingest.has_value());
    const auto requests = ingest->requests();
    REQUIRE(requests.size() == 4u);
    REQUIRE(requests[0].sub_key == fixture.entries[0].sub_key);
    REQUIRE(requests[1].sub_key == fixture.entries[1].sub_key);
    REQUIRE(requests[2].sub_key
        == *idx_ns::derive_minimizer_position_extent_sub_key(
            fixture.entries[0].sub_key));
    REQUIRE(requests[3].sub_key
        == *idx_ns::derive_minimizer_position_extent_sub_key(
            fixture.entries[1].sub_key));

    for (std::size_t index = 0u; index < requests.size(); ++index) {
        const bool position = index >= fixture.entries.size();
        const std::size_t ordinal = position
            ? index - fixture.entries.size() : index;
        const auto& bytes = position
            ? fixture.position_bytes[ordinal]
            : fixture.control_bytes[ordinal];
        const idx_ns::index_chunk_address address{
                .generation_id = requests[index].generation_id,
                .position_segment = requests[index].position_segment,
                .sub_key = requests[index].sub_key,
                .chunk_no = requests[index].first_chunk_no,
            };
        auto receipt = idx_ns::validate_minimizer_generation_row_receipt(
            idx_ns::minimizer_generation_row_view{
                .address = address,
                .bytes = bytes,
                .row_checksum = idx_ns::crc64_index_chunk_row(
                    address.generation_id, address.position_segment,
                    address.sub_key, address.chunk_no,
                    static_cast<std::uint32_t>(bytes.size()), bytes),
            });
        REQUIRE(receipt.has_value());
        REQUIRE(ingest->accept_validated_row(*receipt).has_value());
    }
    const auto finished = ingest->finish();
    REQUIRE(finished.has_value());
    const auto& resident = **finished;
    REQUIRE(resident.validated());
    REQUIRE(resident.identity().generation_id
        == fixture.manifest.generation_id);
    REQUIRE(resident.identity().source_identity
        == fixture.manifest.source_identity);
    REQUIRE(resident.identity().source_content_checksum
        == fixture.manifest.source_content_checksum);
    REQUIRE(resident.identity().device_uuid
        == runtime.features().device_uuid);
    REQUIRE(resident.identity().cold_load_identity == cold_load_identity);
    REQUIRE(resident.identity().gpu_abi_revision
        == idx_ns::k_grouped_soa_gpu_abi_revision);
    REQUIRE(resident.control_count() == fixture.manifest.control_count);
    REQUIRE(resident.position_count() == fixture.manifest.position_count);
    REQUIRE(resident.descriptor_count() == fixture.entries.size());
    REQUIRE(resident.directory_bytes()
        == fixture.entries.size()
            * idx_ns::grouped_soa_bucket_directory_entry::encoded_bytes);

    std::vector<std::byte> expected_controls;
    std::vector<std::byte> expected_positions;
    for (const auto& bytes : fixture.control_bytes) {
        expected_controls.insert(
            expected_controls.end(), bytes.begin(), bytes.end());
    }
    for (const auto& bytes : fixture.position_bytes) {
        expected_positions.insert(
            expected_positions.end(), bytes.begin(), bytes.end());
    }
    const std::span<const std::byte> resident_controls{
        static_cast<const std::byte*>(resident.controls().mapped),
        expected_controls.size()};
    const std::span<const std::byte> resident_positions{
        static_cast<const std::byte*>(resident.positions().mapped),
        expected_positions.size()};
    REQUIRE(std::ranges::equal(resident_controls, expected_controls));
    REQUIRE(std::ranges::equal(resident_positions, expected_positions));

    const auto* descriptors = static_cast<const
        bioseq::gpu::vulkan::gpu_grouped_soa_resident_partition_descriptor*>(
            resident.descriptors().mapped);
    REQUIRE(descriptors != nullptr);
    REQUIRE(descriptors[0].flags
        == bioseq::gpu::vulkan::k_gpu_grouped_soa_partition_required_flags);
    REQUIRE(descriptors[0].hash_byte_offset == 0u);
    REQUIRE(descriptors[0].prefix_byte_offset == 8u);
    REQUIRE(descriptors[0].count_byte_offset == 16u);
    REQUIRE(descriptors[0].position_byte_offset == 0u);
    REQUIRE(descriptors[0].control_count == 2u);
    REQUIRE(descriptors[0].position_count == 3u);
    REQUIRE(descriptors[0].hash_width == 4u);
    REQUIRE(descriptors[0].offset_width == 2u);
    REQUIRE(descriptors[0].count_width == 2u);
    REQUIRE(descriptors[0].position_width == 2u);
    REQUIRE(descriptors[0].segment_base_bp == 0u);
    REQUIRE(descriptors[0].segment_end_bp == 32u);
    REQUIRE(descriptors[1].hash_byte_offset
        == fixture.control_bytes[0].size());
    REQUIRE(descriptors[1].position_byte_offset
        == fixture.position_bytes[0].size());
    REQUIRE(descriptors[1].segment_base_bp == 32u);
    REQUIRE(descriptors[1].segment_end_bp == 64u);

    const std::uint64_t actual_allocations =
        resident.controls().allocation_bytes
        + resident.positions().allocation_bytes
        + resident.descriptors().allocation_bytes;
    REQUIRE(resident.admitted_device_bytes() == actual_allocations);
    const auto& stats = resident.cold_load_stats();
    REQUIRE(stats.cold_cache_misses == 1u);
    REQUIRE(stats.full_generations_ingested == 1u);
    REQUIRE(stats.control_bytes_copied == expected_controls.size());
    REQUIRE(stats.position_bytes_copied == expected_positions.size());
    REQUIRE(stats.descriptor_bytes_copied
        == fixture.entries.size()
            * sizeof(bioseq::gpu::vulkan::
                gpu_grouped_soa_resident_partition_descriptor));
    REQUIRE(stats.device_allocation_bytes == actual_allocations);
    REQUIRE(stats.admitted_device_bytes == actual_allocations);
    REQUIRE(stats.validated_receipt_rows == requests.size());
    REQUIRE(stats.raw_rehashed_rows == 0u);
    REQUIRE(stats.payload_hash_byte_visits == 0u);
    REQUIRE_FALSE(stats.has_forbidden_work());
}

TEST_CASE("grouped-SoA cold resident fails closed before publication",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    const auto fixture = make_grouped_soa_gpu_resident_fixture();
    auto& runtime = ::bioseq::gpu::vulkan::runtime::instance();
    if (!::bioseq::gpu::vulkan::hardware_device_available(runtime)) return;

    const auto missing_cold_identity =
        idx_ns::gpu_grouped_soa_generation_ingest::begin(
            runtime, fixture.head, fixture.manifest,
            grouped_soa_gpu_expected_identity(), fixture.directory, 0u);
    REQUIRE_FALSE(missing_cold_identity.has_value());
    REQUIRE(missing_cold_identity.error()
        == idx_ns::gpu_grouped_soa_ingest_error::invalid_identity);

    auto wrong_cap = grouped_soa_gpu_expected_identity();
    wrong_cap.max_occ_cap = 11u;
    const auto relabelled = idx_ns::gpu_grouped_soa_generation_ingest::begin(
        runtime, fixture.head, fixture.manifest, wrong_cap,
        fixture.directory, 1u);
    REQUIRE_FALSE(relabelled.has_value());
    REQUIRE(relabelled.error()
        == idx_ns::gpu_grouped_soa_ingest_error::metadata_mismatch);

    auto ingest = idx_ns::gpu_grouped_soa_generation_ingest::begin(
        runtime, fixture.head, fixture.manifest,
        grouped_soa_gpu_expected_identity(), fixture.directory, 2u);
    REQUIRE(ingest.has_value());
    auto corrupt = fixture.control_bytes[0];
    corrupt.front() ^= std::byte{1};
    const auto& request = ingest->requests().front();
    const auto accepted = ingest->accept_raw_chunk_for_test(
        idx_ns::index_chunk_address{
            .generation_id = request.generation_id,
            .position_segment = request.position_segment,
            .sub_key = request.sub_key,
            .chunk_no = request.first_chunk_no,
        },
        corrupt);
    REQUIRE_FALSE(accepted.has_value());
    REQUIRE(accepted.error()
        == idx_ns::gpu_grouped_soa_ingest_error::checksum);
    const auto unpublished = ingest->finish();
    REQUIRE_FALSE(unpublished.has_value());
    REQUIRE(unpublished.error()
        == idx_ns::gpu_grouped_soa_ingest_error::checksum);
}

} // namespace
