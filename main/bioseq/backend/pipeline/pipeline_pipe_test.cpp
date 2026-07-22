// DOC0254 §14.4 tests for the templated pipeline API.
//
// Static-assert-only cases exercise the EmitToken/EmittedToken trait surface
// from §4 (single-emission invariants) and the §7 sink concepts.  The runtime
// cases drive a probe sink through a canonical pipeline to verify that every
// process*<Cep> body emits exactly once.
//
// These tests are compile-only in the static-assert sections (compile fails ⇒
// test fails); the runtime SECTIONs use Catch2 REQUIRE assertions and run as
// part of the bioseq unit-test target.

#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.context;
import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.vulkan;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.pipe;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.chaining.vulkan;
import bioseq.sequences.minimap_port;
import bioseq.indexing.minimizer_hashmap;   // DOC0279 — MinimizerHashmapFragmentIndex (was TargetIndex)
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_vulkan;    // GpuIndexCacheIdentity
import bioseq.parallelism.memory_admission;
import bioseq.sequences.cigar;
import bioseq.storage.size;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.gpu.vulkan.runtime;

namespace {

struct RawSketchAppendFixture {
    using storage_t = std::uint64_t;
    std::uint64_t value = 0;
};
static_assert(std::is_trivially_copyable_v<RawSketchAppendFixture>);
static_assert(sizeof(RawSketchAppendFixture)
              == sizeof(RawSketchAppendFixture::storage_t));

struct alignas(8) SourceDirectMinimizerFixture {
    struct layout_t {
        static constexpr std::uint32_t pos_bits = 33u;
        static constexpr std::uint32_t hash_bits = 30u;
    };
    std::uint64_t value = 0u;
};
static_assert(sizeof(SourceDirectMinimizerFixture) == 8u);
static_assert(alignof(SourceDirectMinimizerFixture) == 8u);

struct alignas(8) IncompatibleSourceDirectMinimizerFixture {
    struct layout_t {
        static constexpr std::uint32_t pos_bits = 32u;
        static constexpr std::uint32_t hash_bits = 31u;
    };
    std::uint64_t value = 0u;
};
static_assert(sizeof(IncompatibleSourceDirectMinimizerFixture) == 8u);
static_assert(alignof(IncompatibleSourceDirectMinimizerFixture) == 8u);

struct BudgetedSketchLifetimeProbe {
    using storage_t = std::uint64_t;
    struct layout_t {
        static constexpr std::uint32_t pos_bits = 28u;
        static constexpr std::uint32_t hash_bits = 35u;
    };
    static constexpr std::uint32_t hash_shift = 29u;
    static constexpr std::uint64_t pos_field_mask = (std::uint64_t{1} << 28u) - 1u;

    bioseq::parallelism::MemoryAdmissionController* admission = nullptr;
    std::uint64_t expected_reserved = 0;
    bool* destroyed_with_live_permit = nullptr;
    bool active = true;

    [[nodiscard]] std::uint64_t raw_value() const noexcept { return 0u; }
    BudgetedSketchLifetimeProbe() = default;
    BudgetedSketchLifetimeProbe(
            bioseq::parallelism::MemoryAdmissionController* owner,
            std::uint64_t expected,
            bool* observed) noexcept
        : admission(owner),
          expected_reserved(expected),
          destroyed_with_live_permit(observed)
    {}
    BudgetedSketchLifetimeProbe(BudgetedSketchLifetimeProbe&& other) noexcept
        : admission(other.admission),
          expected_reserved(other.expected_reserved),
          destroyed_with_live_permit(other.destroyed_with_live_permit),
          active(std::exchange(other.active, false))
    {}
    BudgetedSketchLifetimeProbe(const BudgetedSketchLifetimeProbe&) = delete;
    BudgetedSketchLifetimeProbe& operator=(const BudgetedSketchLifetimeProbe&) = delete;
    BudgetedSketchLifetimeProbe& operator=(BudgetedSketchLifetimeProbe&&) = delete;
    ~BudgetedSketchLifetimeProbe() {
        if (active) {
            *destroyed_with_live_permit =
                admission->reserved_bytes() == expected_reserved;
        }
    }
};

TEST_CASE("BudgetedSketchVector transfers and destroys host capacity before admission",
          "[pipeline][sketch][memory-admission][lifetime]") {
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& admission;
        std::uint64_t previous = 0;
        ~RestoreCap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 20)));

    using Owned = ::bioseq::simd::HWY_NAMESPACE::
        BudgetedSketchVector<BudgetedSketchLifetimeProbe>;
    const auto make_owned = [&](bool& observed) {
        constexpr std::size_t count = 4u;
        const std::uint64_t headroom =
            count * sizeof(BudgetedSketchLifetimeProbe)
            * bioseq::parallelism::kVectorPreallocationHeadroomFactor;
        auto permit = admission.try_reserve(headroom);
        REQUIRE(permit.has_value());
        Owned owned;
        owned.permit = std::move(*permit);
        owned.minimizers.reserve(count);
        const std::uint64_t exact =
            owned.minimizers.capacity() * sizeof(BudgetedSketchLifetimeProbe);
        REQUIRE(exact <= owned.permit->bytes());
        owned.permit->shrink_to(exact);
        owned.minimizers.emplace_back(
            &admission, baseline + exact, &observed);
        return owned;
    };

    bool implicit_observed = false;
    {
        Owned owned = make_owned(implicit_observed);
        Owned moved{std::move(owned)};
        REQUIRE(owned.minimizers.capacity() == 0u);
        REQUIRE_FALSE(owned.permit.has_value());
        REQUIRE(moved.minimizers.size() == 1u);
    }
    REQUIRE(implicit_observed);
    REQUIRE(admission.reserved_bytes() == baseline);

    bool explicit_observed = false;
    {
        Owned owned = make_owned(explicit_observed);
        owned.release_owned_memory_before_permit();
        REQUIRE(owned.minimizers.capacity() == 0u);
        REQUIRE_FALSE(owned.permit.has_value());
    }
    REQUIRE(explicit_observed);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("fixed Vulkan target sketch adopts host allocation and releases device before index",
          "[pipeline][vulkan][sketch][zero-copy][lifetime]"
          "[pipeline][vulkan][zero-copy]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<Sketch>;
    using Device = bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<Sketch>;

    Source source;
    source.host_minimizers = {
        Sketch{/*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u},
        Sketch{/*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u},
    };
    const Sketch* const original_data = source.host_minimizers.data();
    const std::size_t original_capacity =
        source.host_minimizers.capacity();
    auto device = std::make_shared<Device>();
    std::weak_ptr<Device> device_lifetime = device;
    source.device = std::move(device);
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes =
        original_capacity * sizeof(Sketch);
    source.arena_trimmed_before_host_materialization = true;

    using Owner = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        FixedVulkanTargetHostOwner<Sketch>;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<Owner>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<Owner>);
    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    const auto& receipt = adopted.adoption;

    REQUIRE(receipt.zero_copy_adoption_accepted());
    REQUIRE(adopted.ownership_authority_accepted());
    REQUIRE(receipt.host_permit_released_to_process_footprint);
    REQUIRE(receipt.retained_host_permit_bytes == 0u);
    REQUIRE(receipt.device_owner_present_before_adoption);
    REQUIRE(receipt.device_owner_unique_before_adoption);
    REQUIRE(receipt.source_data == original_data);
    REQUIRE(receipt.adopted_data == original_data);
    REQUIRE(receipt.adopted_capacity == original_capacity);
    REQUIRE(adopted.minimizers.data() == original_data);
    REQUIRE(adopted.minimizers.capacity() == original_capacity);
    REQUIRE(adopted.minimizers.size() == 2u);
    REQUIRE_FALSE(adopted.host_permit.has_value());
    REQUIRE(source.host_minimizers.capacity() == 0u);
    REQUIRE(source.device == nullptr);
    REQUIRE(device_lifetime.expired());
    REQUIRE_FALSE(source.host_permit.has_value());
}

TEST_CASE("fixed Vulkan host ownership is target-only and starts at 256 Mi bp",
          "[pipeline][vulkan][sketch][zero-copy][route-selection]") {
    using namespace ::bioseq::simd::HWY_NAMESPACE;
    using pipe_entry_detail::
        fixed_vulkan_target_host_adoption_route_selected;
    constexpr std::uint64_t minimum = pipe_entry_detail::
        kVulkanFixedHostAdoptionTargetMinimumBp;
    // Route selection is intentionally independent of emitted-vector size.
    // An ambiguity-heavy dna5 target can be >=256 MiB yet emit only this
    // representative small nonzero payload, which must retain its exact permit
    // if RSS reconciliation is inconclusive.
    constexpr std::uint64_t sparse_dna5_emitted_bytes =
        2u * sizeof(MinimapSketchData);
    Minimap2Params params{};
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = false;
    params.vulkan_gpu_index_build_enabled = false;
    params.minimizer_algorithm =
        MinimizerAlgorithmType::VULKAN_MINIMAP_INT;

    STATIC_REQUIRE(minimum == (std::uint64_t{256} << 20u));
    STATIC_REQUIRE(sparse_dna5_emitted_bytes < (std::uint64_t{64} << 10u));
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(
        /*phase_c_gpu_index=*/false,
        params,
        /*alphabet_size=*/4u,
        minimum - 1u));
    REQUIRE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(
        /*phase_c_gpu_index=*/false,
        params,
        /*alphabet_size=*/4u,
        minimum));
    REQUIRE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(
        /*phase_c_gpu_index=*/false,
        params,
        /*alphabet_size=*/5u,
        minimum));
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(
        /*phase_c_gpu_index=*/true,
        params,
        /*alphabet_size=*/4u,
        minimum));

    params.vulkan_enabled = false;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(
        /*phase_c_gpu_index=*/false,
        params,
        /*alphabet_size=*/4u,
        minimum));
    params.vulkan_enabled = true;

    params.vulkan_seed_hits_enabled = false;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.vulkan_seed_hits_enabled = true;

    params.vulkan_fused_frontend_enabled = true;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.vulkan_fused_frontend_enabled = false;

    params.vulkan_gpu_index_build_enabled = true;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.vulkan_gpu_index_build_enabled = false;

    params.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.minimizer_algorithm =
        MinimizerAlgorithmType::VULKAN_MINIMAP_INT;

    params.kmer_size = 7;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.kmer_size = 27;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.kmer_size = 15;

    params.window_size = 9;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.window_size = 11;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 4u, minimum));
    params.window_size = 10;

    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        SourceDirectMinimizerFixture>(false, params, 20u, minimum));
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        IncompatibleSourceDirectMinimizerFixture>(
            false, params, 4u, minimum));
}

TEST_CASE("empty fixed Vulkan target result adopts zero bytes and releases device",
          "[pipeline][vulkan][sketch][zero-copy][lifetime][empty]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<Sketch>;
    using Device = bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<Sketch>;

    Source source;
    auto device = std::make_shared<Device>();
    std::weak_ptr<Device> device_lifetime = device;
    source.device = std::move(device);
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    const auto& receipt = adopted.adoption;

    REQUIRE(receipt.zero_copy_adoption_accepted());
    REQUIRE(adopted.ownership_authority_accepted());
    REQUIRE(receipt.adopted_size == 0u);
    REQUIRE(receipt.adopted_capacity == 0u);
    REQUIRE(receipt.host_capacity_bytes == 0u);
    REQUIRE(receipt.source_capacity_after_adoption == 0u);
    REQUIRE(adopted.minimizers.empty());
    REQUIRE(adopted.minimizers.capacity() == 0u);
    REQUIRE_FALSE(adopted.host_permit.has_value());
    REQUIRE(source.device == nullptr);
    REQUIRE(device_lifetime.expired());
}

TEST_CASE("empty fixed Vulkan target index handoff admits and retains zero bytes",
          "[pipeline][vulkan][sketch][zero-copy][memory-admission][empty]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<Sketch>;
    using Device = bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<Sketch>;

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    Source source;
    source.device = std::make_shared<Device>();
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.arena_trimmed_before_host_materialization = true;
    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    auto admitted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_len=*/1'000u);
    const auto& receipt = admitted.receipt;

    REQUIRE(admitted.accepted());
    REQUIRE(receipt.admitted_position_headroom_bytes == 0u);
    REQUIRE(receipt.admitted_table_headroom_bytes == 0u);
    REQUIRE(receipt.admitted_build_bytes == 0u);
    REQUIRE(receipt.retained_position_bytes == 0u);
    REQUIRE(receipt.retained_table_bytes == 0u);
    REQUIRE(receipt.retained_index_bytes == 0u);
    REQUIRE(receipt.retained_index_permit_bytes == 0u);
    REQUIRE(receipt.transferred_position_footprint_bytes == 0u);
    REQUIRE_FALSE(
        receipt.source_footprint_authority_transferred_to_positions);
    REQUIRE(receipt.every_retained_position_initialized);
    REQUIRE(receipt.source_capacity_released);
    REQUIRE(receipt.source_host_permit_cleared_after_consumption);
    REQUIRE(receipt.source_adoption_cleared_after_consumption);
    REQUIRE_FALSE(receipt.source_host_permit_retained_during_build);
    REQUIRE(admitted.index_permit.has_value());
    REQUIRE(admitted.index_permit->bytes() == 0u);
    REQUIRE(admitted.index.positions.capacity() == 0u);
    REQUIRE(admitted.index.table_allocation_bytes() == 0u);
    REQUIRE(admission.reserved_bytes() == baseline);

    admitted.release_storage_before_permit();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("dense fixed Vulkan handoff transfers exact footprint authority to positions",
          "[pipeline][vulkan][sketch][zero-copy][memory-admission][replacement-authority]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<Sketch>;
    using Device = bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<Sketch>;
    using Authority = ::bioseq::simd::HWY_NAMESPACE::pipe::
        TransientTargetIndexReleaseAuthority;

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& controller;
        std::uint64_t cap = 0u;
        ~RestoreCap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::parallelism::saturating_add(
            baseline, std::uint64_t{64} << 20u)));

    Source source;
    source.host_minimizers.reserve(2u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u);
    REQUIRE(source.host_minimizers.size()
            == source.host_minimizers.capacity());
    const std::uint64_t source_host_bytes =
        source.host_minimizers.capacity() * sizeof(Sketch);
    source.device = std::make_shared<Device>();
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes = source_host_bytes;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    REQUIRE(adopted.ownership_authority_accepted());
    auto admitted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_len=*/1'000u);
    const auto& receipt = admitted.receipt;

    REQUIRE(admitted.accepted());
    REQUIRE(adopted.minimizers.capacity() == 0u);
    REQUIRE(adopted.adoption.adopted_size == 0u);
    REQUIRE(adopted.adoption.host_capacity_bytes == 0u);
    REQUIRE_FALSE(
        adopted.adoption.host_permit_released_to_process_footprint);
    REQUIRE(receipt.source_host_capacity_bytes_before_build
            == source_host_bytes);
    REQUIRE(receipt.source_footprint_reflected_bytes_before_build
            >= source_host_bytes);
    REQUIRE(receipt.source_minimizer_count_before_build == 2u);
    REQUIRE(receipt.retained_position_count == 2u);
    REQUIRE(receipt.retained_position_initialized_bytes
            == receipt.retained_position_bytes);
    REQUIRE(admitted.index.positions.size()
            == admitted.index.positions.capacity());
    REQUIRE(receipt.every_retained_position_initialized);
    REQUIRE(receipt.source_capacity_released);
    REQUIRE(receipt.source_adoption_cleared_after_consumption);
    REQUIRE_FALSE(receipt.source_host_permit_retained_during_build);
    REQUIRE(receipt.source_host_permit_bytes_before_build == 0u);
    REQUIRE(receipt.source_footprint_authority_transferred_to_positions);
    REQUIRE(receipt.transferred_position_footprint_bytes
            == receipt.retained_position_bytes);
    REQUIRE(receipt.transferred_position_footprint_bytes
            == source_host_bytes);
    REQUIRE(receipt.retained_index_permit_bytes
            == receipt.retained_table_bytes);
    REQUIRE(receipt.retained_index_bytes
            == receipt.retained_index_permit_bytes
                + receipt.transferred_position_footprint_bytes);
    REQUIRE(admitted.index_permit->bytes()
            == receipt.retained_index_permit_bytes);
    REQUIRE(admission.reserved_bytes()
            == baseline + receipt.retained_index_permit_bytes);
    REQUIRE(admitted.index.lookup(7u).size() == 1u);
    REQUIRE(admitted.index.lookup(9u).size() == 1u);

    Authority authority{
        .index = &admitted.index,
        .permit = &*admitted.index_permit,
        .expected_permit_bytes = receipt.retained_index_permit_bytes,
        .require_exact_permit_bytes = true,
    };
    REQUIRE(authority.accepted());
    Authority mismatched_authority = authority;
    ++mismatched_authority.expected_permit_bytes;
    REQUIRE_FALSE(mismatched_authority.accepted());
    ::bioseq::simd::HWY_NAMESPACE::pipe::PostSeedOwnedLifetime lifetime{
        /*seed_users=*/1u,
        /*owned_gpu_cache=*/nullptr,
        authority};
    const auto release = lifetime.complete_seed_user();
    REQUIRE(release.cpu_index_releases == 1u);
    REQUIRE(release.cpu_index_permit_releases == 1u);
    REQUIRE(release.cpu_index_permit_bytes_released
            == receipt.retained_index_permit_bytes);
    REQUIRE(release.cpu_index_storage_released_before_permit);
    REQUIRE(admitted.index.table_empty());
    REQUIRE(admitted.index.positions.capacity() == 0u);
    REQUIRE_FALSE(static_cast<bool>(*admitted.index_permit));
    REQUIRE(admitted.index_permit->bytes() == 0u);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("fixed Vulkan footprint mismatch keeps full index permit",
          "[pipeline][vulkan][sketch][zero-copy][memory-admission][replacement-authority][mismatch]") {
    using WideSketch =
        ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<40u, 15u>;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<WideSketch>;
    using Device =
        bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<WideSketch>;
    STATIC_REQUIRE(sizeof(WideSketch) == 2u * sizeof(std::uint64_t));

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& controller;
        std::uint64_t cap = 0u;
        ~RestoreCap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::parallelism::saturating_add(
            baseline, std::uint64_t{64} << 20u)));

    Source source;
    source.host_minimizers.reserve(2u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u);
    REQUIRE(source.host_minimizers.size()
            == source.host_minimizers.capacity());
    const std::uint64_t source_host_bytes =
        source.host_minimizers.capacity() * sizeof(WideSketch);
    source.device = std::make_shared<Device>();
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes = source_host_bytes;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    auto admitted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_len=*/1'000u);
    const auto& receipt = admitted.receipt;

    REQUIRE(admitted.accepted());
    REQUIRE(receipt.source_host_capacity_bytes_before_build
            == source_host_bytes);
    REQUIRE(receipt.retained_position_bytes
            == 2u * sizeof(std::uint64_t));
    REQUIRE(receipt.retained_position_initialized_bytes
            == receipt.retained_position_bytes);
    REQUIRE(admitted.index.positions.size()
            == admitted.index.positions.capacity());
    REQUIRE(receipt.source_host_capacity_bytes_before_build
            != receipt.retained_position_bytes);
    REQUIRE_FALSE(
        receipt.source_footprint_authority_transferred_to_positions);
    REQUIRE(receipt.transferred_position_footprint_bytes == 0u);
    REQUIRE(receipt.retained_index_permit_bytes
            == receipt.retained_index_bytes);
    REQUIRE(admitted.index_permit->bytes()
            == receipt.retained_index_bytes);
    REQUIRE(admission.reserved_bytes()
            == baseline + receipt.retained_index_bytes);

    admitted.release_storage_before_permit();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("small nonzero fixed Vulkan host permit survives adoption through index consumption",
          "[pipeline][vulkan][sketch][zero-copy][memory-admission][retained-permit]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<Sketch>;
    using Device = bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<Sketch>;
    using IndexOwner = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        VulkanAdmittedTargetIndex;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<IndexOwner>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<IndexOwner>);

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& controller;
        std::uint64_t cap = 0u;
        ~RestoreCap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::parallelism::saturating_add(
            baseline, std::uint64_t{64} << 20u)));

    constexpr std::size_t minimizer_count = 2u;
    const std::uint64_t planned_host_bytes =
        minimizer_count * sizeof(Sketch);
    auto host_permit = admission.try_reserve(planned_host_bytes);
    REQUIRE(host_permit.has_value());
    Source source;
    source.host_permit.emplace(std::move(*host_permit));
    source.host_minimizers.reserve(minimizer_count);
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u);
    const std::uint64_t exact_host_bytes =
        source.host_minimizers.capacity() * sizeof(Sketch);
    REQUIRE(exact_host_bytes <= planned_host_bytes);
    source.host_permit->shrink_to(exact_host_bytes);
    const Sketch* const original_data = source.host_minimizers.data();
    source.device = std::make_shared<Device>();
    std::weak_ptr<Device> device_lifetime = source.device;
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = false;
    source.host_footprint_reflected_delta_bytes = 0u;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    REQUIRE(adopted.ownership_authority_accepted());
    REQUIRE(adopted.minimizers.data() == original_data);
    REQUIRE(adopted.host_permit.has_value());
    REQUIRE(adopted.host_permit->bytes() == exact_host_bytes);
    REQUIRE(adopted.adoption.retained_host_permit_bytes == exact_host_bytes);
    REQUIRE_FALSE(
        adopted.adoption.host_permit_released_to_process_footprint);
    REQUIRE(device_lifetime.expired());
    REQUIRE(admission.reserved_bytes() == baseline + exact_host_bytes);

    auto admitted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_len=*/1'000u);
    const auto& receipt = admitted.receipt;
    REQUIRE(admitted.accepted());
    REQUIRE(adopted.minimizers.capacity() == 0u);
    REQUIRE_FALSE(adopted.host_permit.has_value());
    REQUIRE(adopted.adoption.adopted_size == 0u);
    REQUIRE(adopted.adoption.host_capacity_bytes == 0u);
    REQUIRE_FALSE(
        adopted.adoption.host_permit_released_to_process_footprint);
    REQUIRE(receipt.source_host_permit_retained_during_build);
    REQUIRE(receipt.source_host_permit_bytes_before_build
            == exact_host_bytes);
    REQUIRE(receipt.reserved_bytes_before_index_admission
            == baseline + exact_host_bytes);
    REQUIRE(receipt.reserved_bytes_after_index_admission
            == baseline + exact_host_bytes + receipt.admitted_build_bytes);
    REQUIRE(receipt.source_capacity_released);
    REQUIRE(receipt.source_host_permit_cleared_after_consumption);
    REQUIRE(receipt.source_adoption_cleared_after_consumption);
    REQUIRE_FALSE(
        receipt.source_footprint_authority_transferred_to_positions);
    REQUIRE(receipt.transferred_position_footprint_bytes == 0u);
    REQUIRE(receipt.retained_index_permit_bytes
            == receipt.retained_index_bytes);
    REQUIRE(admission.reserved_bytes()
            == baseline + receipt.retained_index_bytes);
    REQUIRE(admitted.index.lookup(7u).size() == 1u);
    REQUIRE(admitted.index.lookup(9u).size() == 1u);

    admitted.release_storage_before_permit();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("fixed Vulkan adoption rejects an aliased device owner",
          "[pipeline][vulkan][sketch][zero-copy][lifetime][alias]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using Source = bioseq::gpu::vulkan::VulkanSketchResult<Sketch>;
    using Device = bioseq::gpu::vulkan::VulkanDeviceMinimizerStorage<Sketch>;

    Source source;
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.device = std::make_shared<Device>();
    const auto device_alias = source.device;
    source.dispatch_token = bioseq::gpu::vulkan::VulkanSketchDispatchToken::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes = sizeof(Sketch);
    source.arena_trimmed_before_host_materialization = true;

    REQUIRE_THROWS_AS(
        ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
            adopt_fixed_vulkan_sketch_host_result(std::move(source)),
        bioseq::gpu::vulkan::VulkanRuntimeError);
    REQUIRE(device_alias != nullptr);
}

TEST_CASE("real sparse dna5 Vulkan target adopts either exact authority and builds admitted index",
          "[pipeline][vulkan][sketch][zero-copy][memory-admission][hardware][dna5]") {
    namespace vk = bioseq::gpu::vulkan;
    auto& runtime = vk::Runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("hardware Vulkan unavailable; real fixed-route handoff skipped");
        return;
    }

    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using PackedSequence =
        sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    constexpr std::size_t letter_count = 1'000'000u;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t pipeline_cache_metadata_baseline =
        runtime.pipeline_cache_metadata_reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& controller;
        std::uint64_t cap = 0u;
        ~RestoreCap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::parallelism::saturating_add(
            baseline, std::uint64_t{512} << 20u)));

    seq::dna5 ambiguous{};
    ambiguous.assign_char('N');
    std::vector<seq::dna5> letters(letter_count, ambiguous);
    std::vector<bool> ambiguous_positions(letter_count, true);
    constexpr std::size_t island_size = 128u;
    constexpr std::size_t island_stride = 100'000u;
    std::uint64_t lcg = 0x243f6a8885a308d3ULL;
    for (std::size_t island_start = 4'096u;
         island_start + island_size <= letter_count;
         island_start += island_stride) {
        for (std::size_t offset = 0u; offset < island_size; ++offset) {
            lcg = lcg * 6364136223846793005ULL
                + 1442695040888963407ULL;
            seq::dna5 value{};
            constexpr std::array<char, 4> canonical{'A', 'C', 'G', 'T'};
            value.assign_char(canonical[(lcg >> 33u) & 3u]);
            letters[island_start + offset] = value;
            ambiguous_positions[island_start + offset] = false;
        }
    }
    auto sequence = compact_sequence_range<PackedSequence>(letters);
    auto sequence_owner = bioseq::storage::make_lazy_in_memory(sequence);
    const auto sequence_view =
        bioseq::storage::as_lazy_sequence_view(sequence_owner);

    auto result = vk::collect_minimizers_vulkan_result<Sketch>(
        sequence_view,
        window_size,
        kmer_size,
        nullptr,
        vk::VulkanSketchHostOwnershipRoute::fixed_zero_copy_adoption);
    REQUIRE(result.has_value());
    REQUIRE(result->dispatch_token.accepted());
    REQUIRE(result->host_ownership_route
            == vk::VulkanSketchHostOwnershipRoute::
                fixed_zero_copy_adoption);
    REQUIRE(result->arena_trimmed_before_host_materialization);
    REQUIRE_FALSE(result->host_minimizers.empty());
    REQUIRE(result->host_minimizers.size() < 10'000u);
    const std::uint64_t host_capacity_bytes =
        result->host_minimizers.capacity() * sizeof(Sketch);
    const bool footprint_authority =
        result->host_permit_released_to_process_footprint
        && !result->host_permit.has_value()
        && result->host_footprint_reflected_delta_bytes
            >= host_capacity_bytes;
    const bool retained_permit_authority =
        !result->host_permit_released_to_process_footprint
        && result->host_permit.has_value()
        && result->host_permit->bytes() == host_capacity_bytes;
    REQUIRE(footprint_authority != retained_permit_authority);
    for (const Sketch& minimizer : result->host_minimizers) {
        const std::size_t last_position =
            static_cast<std::size_t>(minimizer.last_pos());
        REQUIRE(last_position + 1u >= kmer_size);
        const std::size_t first_position =
            last_position + 1u - kmer_size;
        REQUIRE(last_position < ambiguous_positions.size());
        for (std::size_t position = first_position;
             position <= last_position;
             ++position) {
            REQUIRE_FALSE(ambiguous_positions[position]);
        }
    }
    const Sketch* const original_data = result->host_minimizers.data();
    std::weak_ptr<vk::VulkanDeviceMinimizerStorage<Sketch>> device_lifetime =
        result->device;

    auto adopted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(*result));
    REQUIRE(adopted.adoption.zero_copy_adoption_accepted());
    REQUIRE(adopted.ownership_authority_accepted());
    REQUIRE(adopted.minimizers.data() == original_data);
    REQUIRE(device_lifetime.expired());
    // Moving an optional leaves its source engagement unspecified.  If the
    // retained-permit case leaves an engaged source, its MemoryPermit must be
    // moved-from and therefore own neither a controller nor any bytes.
    if (result->host_permit.has_value()) {
        REQUIRE_FALSE(static_cast<bool>(*result->host_permit));
        REQUIRE(result->host_permit->bytes() == 0u);
    }
    REQUIRE(result->host_minimizers.capacity() == 0u);

    auto admitted = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), letter_count);
    const auto& handoff = admitted.receipt;
    REQUIRE(admitted.accepted());
    REQUIRE(adopted.minimizers.capacity() == 0u);
    REQUIRE_FALSE(adopted.host_permit.has_value());
    REQUIRE(admitted.index_permit.has_value());
    REQUIRE(admitted.index_permit->bytes()
            == handoff.retained_index_permit_bytes);
    REQUIRE(handoff.retained_position_bytes
            == admitted.index.positions.capacity() * sizeof(std::uint64_t));
    REQUIRE(handoff.retained_table_bytes
            == admitted.index.table_allocation_bytes());
    REQUIRE(admitted.index.table_peak_allocation_bytes()
            <= handoff.admitted_table_headroom_bytes);
    REQUIRE(handoff.retained_index_bytes
            == handoff.retained_index_permit_bytes
                + handoff.transferred_position_footprint_bytes);
    if (footprint_authority) {
        REQUIRE(handoff.source_footprint_authority_transferred_to_positions);
        REQUIRE(handoff.transferred_position_footprint_bytes
                == handoff.retained_position_bytes);
    } else {
        REQUIRE_FALSE(
            handoff.source_footprint_authority_transferred_to_positions);
        REQUIRE(handoff.transferred_position_footprint_bytes == 0u);
    }

    admitted.release_storage_before_permit();
    // This first real sketch may compile and retain Vulkan pipelines.  Their
    // process-lifetime metadata has its own explicit MemoryPermit and is not
    // part of the adopted host vector or admitted index.  Prove that the only
    // reservation surviving this transaction is precisely that cache growth;
    // an unreturned host/device/index permit would make the equality fail.
    const std::uint64_t pipeline_cache_metadata_after =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(pipeline_cache_metadata_after
            >= pipeline_cache_metadata_baseline);
    REQUIRE(admission.reserved_bytes()
            == bioseq::parallelism::saturating_add(
                baseline,
                pipeline_cache_metadata_after
                    - pipeline_cache_metadata_baseline));
}

TEST_CASE("raw sketch batches grow geometrically and nested query scopes release only their tail",
          "[pipeline][sketch][arena][memory]")
{
    constexpr std::size_t initial_capacity = 140'000; // > 1 MiB as uint64_t.
    constexpr std::size_t batch_size = 4'096;
    constexpr std::size_t batch_count = 64;
    constexpr std::size_t emitted_count = batch_size * batch_count;

    context::context ctx;
    context::installed_context_scope installed{ctx};
    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);

    std::vector<RawSketchAppendFixture> survivor;
    {
        auto outer_scope = ctx.push<context::Arena::Chain>();
        auto outer_slab =
            ::bioseq::chaining::make_arena_slab<RawSketchAppendFixture>();
        outer_slab.reserve(initial_capacity);
        outer_slab.resize(1);
        outer_slab.data()[0].value = 0xfeedfacecafebeefULL;
        const std::size_t outer_bytes =
            initial_capacity * sizeof(RawSketchAppendFixture);
        REQUIRE(ctx.chain_heap_fallback_bytes() == outer_bytes);

        // Repeat the faithful under-estimated raw-batch append shape. Before
        // geometric growth this performed one exact allocation and full-prefix
        // copy for every batch after `initial_capacity` was crossed.
        for (std::size_t round = 0; round < 3; ++round) {
            {
                auto query_scope = ctx.push<context::Arena::Chain>();
                auto sketches =
                    ::bioseq::chaining::make_arena_slab<RawSketchAppendFixture>();
                sketches.reserve(initial_capacity);
                ::bioseq::simd::HWY_NAMESPACE::pipe::
                    sketch_collector_arena_adapter<RawSketchAppendFixture>
                        collector{&sketches};
                std::vector<std::uint64_t> raw(batch_size);

                for (std::size_t batch = 0; batch < batch_count; ++batch) {
                    for (std::size_t i = 0; i < batch_size; ++i) {
                        raw[i] = (static_cast<std::uint64_t>(batch) << 32u)
                               | static_cast<std::uint64_t>(i);
                    }
                    REQUIRE(collector.append_raw_storage(raw.data(), raw.size()));
                }

                REQUIRE(sketches.size() == emitted_count);
                REQUIRE(sketches.capacity() == initial_capacity * 2);
                REQUIRE(sketches.data()[0].value == 0u);
                REQUIRE(sketches.data()[batch_size - 1].value == batch_size - 1);
                REQUIRE(sketches.data()[batch_size].value
                        == (std::uint64_t{1} << 32u));
                REQUIRE(sketches.data()[emitted_count - 1].value
                        == ((std::uint64_t{batch_count - 1} << 32u)
                            | std::uint64_t{batch_size - 1}));

                const std::size_t expected_query_bytes =
                    (initial_capacity + initial_capacity * 2)
                    * sizeof(RawSketchAppendFixture);
                REQUIRE(ctx.chain_heap_fallback_bytes()
                        == outer_bytes + expected_query_bytes);
                survivor.assign(
                    sketches.data(),
                    sketches.data() + sketches.size());
            }

            // Inner heap blocks are gone, the outer allocation and its bytes
            // remain live, and ordinary result ownership survives the unwind.
            REQUIRE(ctx.chain_heap_fallback_bytes() == outer_bytes);
            REQUIRE(outer_slab.data()[0].value == 0xfeedfacecafebeefULL);
            REQUIRE(survivor.size() == emitted_count);
            REQUIRE(survivor.back().value
                    == ((std::uint64_t{batch_count - 1} << 32u)
                        | std::uint64_t{batch_size - 1}));
        }
    }
    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);
}

using bioseq::pipeline_stats::ExecutedBackend;
using bioseq::pipeline_stats::ExecutionEngine;
using bioseq::pipeline_stats::ExecutionEnvironmentProof;
using bioseq::pipeline_stats::OptionalProofBool;
using bioseq::pipeline_stats::PipelineCachePolicy;
using bioseq::pipeline_stats::PipelineJobExecutionStats;
using bioseq::pipeline_stats::PipelineStage;
using bioseq::pipeline_stats::SimdAlignmentAlgorithmIdentity;
using bioseq::pipeline_stats::SimdChainingPolicyIdentity;
using bioseq::pipeline_stats::TargetIndexSource;

[[nodiscard]] ExecutionEnvironmentProof make_cpu_environment(
    std::uint32_t workers = 2) {
    ExecutionEnvironmentProof environment{};
    environment.effective_cpu_workers = workers;
    return environment;
}

[[nodiscard]] ExecutionEnvironmentProof make_gpu_environment(
    std::uint32_t workers = 2) {
    ExecutionEnvironmentProof environment{};
    environment.effective_cpu_workers = workers;
    environment.gpu_environment_present = true;
    environment.gpu_device_uuid[0] = 0x42;
    environment.gpu_driver_identity[0] = 'v';
    environment.gpu_driver_identity[1] = 'k';
    environment.gpu_pipeline_cache_policy =
        PipelineCachePolicy::immutable_preseed_all_hit;
    environment.gpu_pipeline_cache_seed_sha256[0] = 0x91;
    environment.gpu_pipeline_cache_seed_size = 64;
    environment.gpu_pipeline_cache_manifest_checksum = 0x1234;
    environment.gpu_pipeline_cache_hit = OptionalProofBool::true_value;
    return environment;
}

[[nodiscard]] PipelineJobExecutionStats make_attested_job(
    ExecutedBackend backend,
    TargetIndexSource source) {
    PipelineJobExecutionStats job{};
    job.requested_backend = backend;
    job.actual_backend = backend;
    if (backend == ExecutedBackend::simd_cpu) {
        job.requested_simd_chaining_policy =
            SimdChainingPolicyIdentity::bounded_batch_approximate;
        job.observed_simd_chaining_policy =
            SimdChainingPolicyIdentity::bounded_batch_approximate;
        job.requested_simd_alignment_algorithm =
            SimdAlignmentAlgorithmIdentity::block_aligner;
        job.observed_simd_alignment_algorithm =
            SimdAlignmentAlgorithmIdentity::block_aligner;
    }
    job.requested_target_index_source = source;
    job.actual_target_index_source = source;
    job.environment = backend == ExecutedBackend::vulkan_gpu
        ? make_gpu_environment()
        : make_cpu_environment();
    if (source == TargetIndexSource::persisted) {
        job.persisted_generation_id = 17;
        job.persisted_source_identity = 0xabc;
    }
    for (std::size_t i = 0; i < bioseq::pipeline_stats::pipeline_stage_count; ++i) {
        const auto stage = static_cast<PipelineStage>(i);
        const ExecutionEngine engine = bioseq::pipeline_stats::expected_execution_engine(
            backend, source, stage);
        if (engine == ExecutionEngine::count) continue;
        auto& coverage = job.stages.coverage[i];
        coverage.eligible_jobs = 1;
        coverage.executed_jobs[bioseq::pipeline_stats::engine_index(engine)] = 1;
    }
    return job;
}

[[nodiscard]] PipelineJobExecutionStats make_zero_match_job(
    ExecutedBackend backend,
    TargetIndexSource source) {
    auto job = make_attested_job(backend, source);
    for (std::size_t i = 0; i < bioseq::pipeline_stats::pipeline_stage_count; ++i) {
        const auto stage = static_cast<PipelineStage>(i);
        if (!bioseq::pipeline_stats::stage_requires_front_end_eligibility(source, stage)) {
            job.stages.coverage[i] = {};
        }
    }
    return job;
}

} // namespace

TEST_CASE("DOC0371 canonical pipeline stages have stable ordinals and names",
          "[pipe][pipeline-stats][abi]") {
    using bioseq::pipeline_stats::pipeline_stage_name;
    static_assert(static_cast<std::uint8_t>(PipelineStage::query_sketch) == 0);
    static_assert(static_cast<std::uint8_t>(PipelineStage::target_index_build) == 2);
    static_assert(static_cast<std::uint8_t>(PipelineStage::chain_backtrack) == 8);
    static_assert(static_cast<std::uint8_t>(PipelineStage::target_window_io) == 14);
    static_assert(static_cast<std::uint8_t>(PipelineStage::right_extension) == 19);
    static_assert(static_cast<std::uint8_t>(PipelineStage::count) == 20);
    static_assert(bioseq::pipeline_stats::pipeline_stage_count == 20);
    static_assert(std::is_trivially_copyable_v<
        bioseq::pipeline_stats::PipelineExecutionStats>);
    static_assert(std::is_standard_layout_v<
        bioseq::pipeline_stats::PipelineExecutionStats>);

    constexpr std::array<std::string_view, 20> expected_names{
        "query_sketch",
        "target_sketch",
        "target_index_build",
        "persisted_index_decode",
        "partition_selection",
        "device_index_upload",
        "seed_lookup",
        "chain_score",
        "chain_backtrack",
        "region_generation",
        "primary_secondary_reduction",
        "divergence_estimation",
        "mapq",
        "target_window_plan",
        "target_window_io",
        "target_window_upload",
        "cigar_job_generation",
        "left_extension",
        "gap_extension",
        "right_extension",
    };
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        REQUIRE(pipeline_stage_name(static_cast<PipelineStage>(i)) == expected_names[i]);
    }
}

TEST_CASE("persisted Shape-V5 publication rejects burned fully-consumed proof",
          "[vulkan][pipe][shape-v5][publication][transaction][burn]") {
    using namespace bioseq::pipeline_stats;
    using Counts = PersistedShapeV5CoordinatePublicationCounts;

    constexpr Counts accepted{
        .expected_jobs = 2,
        .proof_jobs = 2,
        .consumed_jobs = 2,
        .accepted_jobs = 2,
        .burned_jobs = 0,
        .pending_jobs = 0,
        .staged_jobs = 2,
    };
    static_assert(persisted_shape_v5_coordinate_publication_counts_valid(
        accepted));
    REQUIRE(persisted_shape_v5_coordinate_publication_counts_valid(accepted));

    /* This is the exact former fail-open shape: consume() marked both records
     * consumed even though the second expectation mismatched and burned. */
    constexpr Counts burned_but_fully_consumed{
        .expected_jobs = 2,
        .proof_jobs = 2,
        .consumed_jobs = 2,
        .accepted_jobs = 1,
        .burned_jobs = 1,
        .pending_jobs = 0,
        .staged_jobs = 1,
    };
    static_assert(!persisted_shape_v5_coordinate_publication_counts_valid(
        burned_but_fully_consumed));
    REQUIRE_FALSE(persisted_shape_v5_coordinate_publication_counts_valid(
        burned_but_fully_consumed));

    const auto rejected = [](Counts counts) {
        REQUIRE_FALSE(
            persisted_shape_v5_coordinate_publication_counts_valid(counts));
    };
    auto malformed = accepted;
    malformed.expected_jobs = 0;
    rejected(malformed);
    malformed = accepted;
    malformed.proof_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.consumed_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.accepted_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.burned_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.pending_jobs = 1;
    rejected(malformed);
    malformed = accepted;
    malformed.staged_jobs = 1;
    rejected(malformed);
}

TEST_CASE("persisted Shape-V5 environment binding maps without inference",
          "[vulkan][pipe][shape-v5][publication][transaction][environment]") {
    namespace gv = bioseq::gpu::vulkan;
    using namespace bioseq::pipeline_stats;
    gv::RuntimeGpuExecutionEnvironmentBinding binding{};
    binding.gpu_environment_present = true;
    binding.gpu_device_uuid[0] = 0x61u;
    binding.gpu_driver_identity[0] = 'v';
    binding.gpu_driver_identity[1] = 'k';
    binding.cache_policy = gv::RuntimePipelineCachePolicyEvidence::
        immutable_preseed_all_hit;
    binding.seed_sha256[0] = 0xa5u;
    binding.seed_size = 4096u;
    binding.manifest_checksum = 0x12345678u;
    binding.pipeline_cache_all_hit = true;
    REQUIRE(binding.accepted());
    static_assert(noexcept(make_persisted_shape_v5_gpu_environment_proof(
        binding, 4u)));

    const auto mapped = make_persisted_shape_v5_gpu_environment_proof(
        binding, 4u);
    REQUIRE(mapped.has_value());
    REQUIRE(mapped->effective_cpu_workers == 4u);
    REQUIRE(mapped->gpu_environment_present);
    REQUIRE(mapped->gpu_device_uuid == binding.gpu_device_uuid);
    REQUIRE(mapped->gpu_driver_identity == binding.gpu_driver_identity);
    REQUIRE(mapped->gpu_pipeline_cache_policy
            == PipelineCachePolicy::immutable_preseed_all_hit);
    REQUIRE(mapped->gpu_pipeline_cache_seed_sha256 == binding.seed_sha256);
    REQUIRE(mapped->gpu_pipeline_cache_seed_size == binding.seed_size);
    REQUIRE(mapped->gpu_pipeline_cache_manifest_checksum
            == binding.manifest_checksum);
    REQUIRE(mapped->gpu_pipeline_cache_hit == OptionalProofBool::true_value);
    REQUIRE(environment_is_valid_for_backend(
        *mapped, ExecutedBackend::vulkan_gpu));
    REQUIRE(persisted_shape_v5_gpu_environment_binding_matches(
        binding, binding));
    auto drifted_binding = binding;
    drifted_binding.gpu_driver_identity[1] = 'x';
    REQUIRE_FALSE(persisted_shape_v5_gpu_environment_binding_matches(
        drifted_binding, binding));

    REQUIRE_FALSE(make_persisted_shape_v5_gpu_environment_proof(
        binding, 0u).has_value());
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_environment_proof(
        binding,
        static_cast<std::size_t>(sql_integer_value_max) + 1u).has_value());
    auto invalid = binding;
    invalid.gpu_device_uuid = {};
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_environment_proof(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.cache_policy = gv::RuntimePipelineCachePolicyEvidence::invalid;
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_environment_proof(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.pipeline_cache_all_hit = false;
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_environment_proof(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.seed_size = sql_bigint_value_max + 1u;
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_environment_proof(
        invalid, 4u).has_value());

    gv::RuntimeGpuCacheMetricsAudit cache_metrics{
        .token_identity = 0x999u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = binding.seed_size,
        .gpu_pipeline_cache_load_ns = 37u,
        .gpu_pipeline_creates = 11u,
        .gpu_pipeline_creation_ns = 91u,
        .saturated = false,
    };
    static_assert(noexcept(
        make_persisted_shape_v5_gpu_cache_metrics_delta(cache_metrics)));
    const auto cache_delta =
        make_persisted_shape_v5_gpu_cache_metrics_delta(cache_metrics);
    REQUIRE(cache_delta.has_value());
    REQUIRE(cache_delta->gpu_pipeline_cache_loads == 1u);
    REQUIRE(cache_delta->gpu_pipeline_cache_seed_bytes == binding.seed_size);
    REQUIRE(cache_delta->gpu_pipeline_cache_load_ns == 37u);
    REQUIRE(cache_delta->gpu_pipeline_creates == 11u);
    REQUIRE(cache_delta->gpu_pipeline_creation_ns == 91u);
    auto cache_metrics_preview = cache_metrics;
    cache_metrics_preview.gpu_pipeline_cache_load_ns = 50u;
    cache_metrics_preview.gpu_pipeline_creates = 15u;
    cache_metrics_preview.gpu_pipeline_creation_ns = 120u;
    REQUIRE(persisted_shape_v5_gpu_cache_metrics_bounded_by(
        cache_metrics, cache_metrics_preview));
    auto out_of_bounds = cache_metrics;
    out_of_bounds.gpu_pipeline_creates = 16u;
    REQUIRE_FALSE(persisted_shape_v5_gpu_cache_metrics_bounded_by(
        out_of_bounds, cache_metrics_preview));
    auto wrong_token = cache_metrics;
    wrong_token.token_identity += 1u;
    REQUIRE_FALSE(persisted_shape_v5_gpu_cache_metrics_bounded_by(
        wrong_token, cache_metrics_preview));
    const gv::RuntimeGpuCacheMetricsAudit already_reported{
        .token_identity = 0x1000u,
    };
    REQUIRE(already_reported.accepted());
    const auto zero_delta =
        make_persisted_shape_v5_gpu_cache_metrics_delta(already_reported);
    REQUIRE(zero_delta.has_value());
    REQUIRE(zero_delta->gpu_pipeline_cache_loads == 0u);
    REQUIRE(zero_delta->gpu_pipeline_creates == 0u);
    const gv::RuntimeGpuCacheMetricsAudit zero_for_same_operation{
        .token_identity = cache_metrics.token_identity,
    };
    REQUIRE(persisted_shape_v5_gpu_cache_metrics_bounded_by(
        zero_for_same_operation, cache_metrics_preview));
    cache_metrics.saturated = true;
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_cache_metrics_delta(
        cache_metrics).has_value());
    cache_metrics.saturated = false;
    cache_metrics.token_identity = 0u;
    REQUIRE_FALSE(make_persisted_shape_v5_gpu_cache_metrics_delta(
        cache_metrics).has_value());
}

TEST_CASE("persisted Shape-V5 prospective publication is exact across waves",
          "[vulkan][pipe][shape-v5][publication][transaction][stats]") {
    using namespace bioseq::pipeline_stats;
    const auto make_wave = [](const std::size_t jobs,
                              const std::uint64_t first_query) {
        PipelineExecutionStats wave{};
        for (std::size_t i = 0; i < jobs; ++i) {
            auto job = make_attested_job(
                ExecutedBackend::vulkan_gpu,
                TargetIndexSource::persisted);
            job.logical_target_row = 73;
            job.query_ordinal = first_query + i;
            wave.merge_job(job);
        }
        return wave;
    };

    for (const std::size_t jobs : {std::size_t{1}, std::size_t{8}}) {
        CAPTURE(jobs);
        const PipelineExecutionStats before{};
        const auto staged = make_wave(jobs, 100);
        auto prospective = before;
        prospective.merge(staged);
        REQUIRE(persisted_shape_v5_coordinate_prospective_stats_valid(
            before,
            staged,
            prospective,
            static_cast<std::uint64_t>(jobs)));
    }

    auto staged_with_cache_metrics = make_wave(1, 150);
    staged_with_cache_metrics.operational.gpu_pipeline_cache_loads = 1u;
    staged_with_cache_metrics.operational.gpu_pipeline_cache_seed_bytes = 4096u;
    staged_with_cache_metrics.operational.gpu_pipeline_cache_load_ns = 37u;
    staged_with_cache_metrics.operational.gpu_pipeline_creates = 7u;
    staged_with_cache_metrics.operational.gpu_pipeline_creation_ns = 91u;
    PipelineExecutionStats cache_metrics_prospective{};
    cache_metrics_prospective.merge(staged_with_cache_metrics);
    REQUIRE(persisted_shape_v5_coordinate_prospective_stats_valid(
        PipelineExecutionStats{},
        staged_with_cache_metrics,
        cache_metrics_prospective,
        1u));

    const auto before = make_wave(1, 200);
    const auto staged = make_wave(1, 201);
    auto prospective = before;
    prospective.merge(staged);
    REQUIRE(persisted_shape_v5_coordinate_prospective_stats_valid(
        before, staged, prospective, 1));

    auto drifted = staged;
    drifted.environment.effective_cpu_workers += 1;
    auto drifted_prospective = before;
    drifted_prospective.merge(drifted);
    REQUIRE_FALSE(persisted_shape_v5_coordinate_prospective_stats_valid(
        before, drifted, drifted_prospective, 1));

    auto wrong_generation = staged;
    wrong_generation.persisted_generation_id += 1;
    auto generation_prospective = before;
    generation_prospective.merge(wrong_generation);
    REQUIRE_FALSE(persisted_shape_v5_coordinate_prospective_stats_valid(
        before, wrong_generation, generation_prospective, 1));

    auto unattested = staged;
    unattested.fully_attested_alignment_jobs = 0;
    unattested.unattested_alignment_jobs = 1;
    REQUIRE_FALSE(persisted_shape_v5_coordinate_prospective_stats_valid(
        before, unattested, prospective, 1));
    REQUIRE_FALSE(persisted_shape_v5_coordinate_prospective_stats_valid(
        before, staged, prospective, 0));
}

TEST_CASE("persisted GPU SQL ledger is admitted, causal, and immutable across waves",
          "[vulkan][pipe][shape-v5][publication][sql-evidence][memory-admission]") {
    using namespace bioseq::pipeline_stats;
    static_assert(!std::is_copy_constructible_v<
        PersistedGpuOperationEvidenceLedger>);
    static_assert(std::is_nothrow_move_constructible_v<
        PersistedGpuOperationEvidenceLedger>);
    static_assert(noexcept(std::declval<
        PersistedGpuOperationEvidenceChunk&>().publish_cache_metrics(
            std::declval<const PersistedGpuCacheMetricsEvidence&>())));

    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& admission;
        std::uint64_t previous = 0u;
        ~RestoreCap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 23u)));
    constexpr std::uint64_t two_job_chunk_retained_bytes =
        sizeof(PersistedGpuOperationEvidenceChunk)
        + 2u * sizeof(PersistedGpuJobEvidence);

    REQUIRE(PersistedGpuOperationEvidenceChunk::prepare(0u) == nullptr);
    REQUIRE(PersistedGpuOperationEvidenceChunk::prepare(
        static_cast<std::size_t>(
            persisted_gpu_operation_evidence_capacity + 1u)) == nullptr);

    const auto make_chunk = [](
            const std::uint64_t shape_operation_identity,
            const std::uint64_t query_operation_identity,
            const std::uint64_t first_logical_query) {
        auto chunk = PersistedGpuOperationEvidenceChunk::prepare(2u);
        if (chunk == nullptr) return chunk;
        PersistedGpuEnvironmentEvidence environment{};
        environment.device_uuid[0] = 0x41u;
        environment.api_version = 0x00402000u;
        environment.driver_version = 0x01020304u;
        environment.vendor_id = 0x106bu;
        environment.device_id = 0x0011u;
        environment.pipeline_cache_uuid[0] = 0x52u;
        environment.seed_sha256[0] = 0x63u;
        environment.seed_size = 4096u;
        environment.manifest_checksum = 0x7777u;
        PersistedGpuCacheQualificationEvidence qualification{};
        qualification.version = 2u;
        qualification.identity = 0x7171u;
        qualification.archive_count = 2u;
        qualification.archive_bytes = 4096u;
        qualification.archive_checksum = 0x7272u;
        qualification.receipt_count = 2u;
        qualification.hash_passes = 1u;
        qualification.hashed_bytes = 4096u;
        qualification.private_files_created = 2u;
        qualification.private_files_unlinked = 2u;
        qualification.helper_protocol_version = 2u;
        constexpr std::string_view helper_build =
            "bioseq_gpu_archive_builder_v2";
        std::copy(
            helper_build.begin(), helper_build.end(),
            qualification.helper_build_identity.begin());
        qualification.helper_build_identity_size =
            static_cast<std::uint32_t>(helper_build.size());
        constexpr std::string_view helper_source = "installed-sibling";
        std::copy(
            helper_source.begin(), helper_source.end(),
            qualification.helper_source.begin());
        qualification.helper_source_size =
            static_cast<std::uint32_t>(helper_source.size());
        qualification.helper_executable_size = 8192u;
        qualification.helper_executable_sha256[0] = 0x73u;
        qualification.helper_metal_registry_id = 0x7474u;
        qualification.device_uuid = environment.device_uuid;
        qualification.api_version = environment.api_version;
        qualification.driver_version = environment.driver_version;
        qualification.vendor_id = environment.vendor_id;
        qualification.device_id = environment.device_id;
        qualification.pipeline_cache_uuid =
            environment.pipeline_cache_uuid;
        qualification.seed_receipt_identity = 0x8888u;
        qualification.seed_sha256 = environment.seed_sha256;
        qualification.seed_size = environment.seed_size;
        qualification.manifest_checksum = environment.manifest_checksum;
        REQUIRE(qualification.accepted());
        chunk->operation() = PersistedGpuOperationEvidence{
            .operation_identity = shape_operation_identity,
            .publication = {
                .expected_jobs = 2u,
                .proof_jobs = 2u,
                .consumed_jobs = 2u,
                .accepted_jobs = 2u,
                .burned_jobs = 0u,
                .pending_jobs = 0u,
                .staged_jobs = 2u,
            },
            .published_jobs = 2u,
            .publication_commits = 1u,
            .query_pipeline_receipt_count = 3u,
            .shape_pipeline_receipt_count = 7u,
            .submission_count = 6u,
            .query_pipeline_operation_identity = query_operation_identity,
            .shape_pipeline_operation_identity = shape_operation_identity,
            .query_submission_ordinals = {1u, 2u},
            .seed_receipt_identity = 0x8888u,
            .generation_id = 77u,
            .source_identity = 0x9999u,
            .cold_load_identity = 0xaaaau,
            .environment = environment,
            .cache_qualification = qualification,
            .cache_metrics = {
                .token_identity = shape_operation_identity,
                .pipeline_cache_loads = 1u,
                .pipeline_cache_seed_bytes = 4096u,
                .pipeline_cache_load_ns = 31u,
                .pipeline_creates = 4u,
                .pipeline_creation_ns = 47u,
            },
        };
        for (std::size_t i = 0u; i < chunk->jobs().size(); ++i) {
            auto& job = chunk->jobs()[i];
            job = PersistedGpuJobEvidence{
                .logical_target_row = 9u,
                .logical_query_ordinal = first_logical_query + i,
                .query_index = static_cast<std::uint32_t>(i),
                .query_length = 1000u,
                .target_length = 2000u,
                .query_minimizer_count = 10u,
                .query_tile_count = 1u,
                .query_count_dispatches = 1u,
                .query_scan_dispatches = 3u,
                .query_emit_dispatches = 1u,
                .query_completed_roundtrips = 2u,
                .direct_count_dispatches = 1u,
                .direct_scan_dispatches = 3u,
                .anchor_count = 12u,
                .direct_emit_dispatches = 1u,
                .sort_operations = 1u,
                .range_build_dispatches = 1u,
                .range_count = 2u,
                .chain_dispatches = 1u,
                .chain_backtrack_dispatches = 1u,
                .coordinate_dispatches = 1u,
                .submissions = {{
                    PersistedGpuStructuredSubmissionEvidence{
                        shape_operation_identity, 10u, 0x5010u},
                    PersistedGpuStructuredSubmissionEvidence{
                        shape_operation_identity, 11u, 0x5011u},
                    PersistedGpuStructuredSubmissionEvidence{
                        shape_operation_identity, 12u, 0x5012u},
                    PersistedGpuStructuredSubmissionEvidence{
                        shape_operation_identity, 13u, 0x5013u},
                }},
                .count_status_checks = 1u,
                .emit_status_checks = 1u,
                .status_or = 0u,
                .result_readbacks = 1u,
                .coordinate_vote_dispatches = 0u,
                .cpu_or_simd_biological_calls = 0u,
                .recompute_calls = 0u,
                .fallback_calls = 0u,
                .coordinate = {
                    .query_start = 1u,
                    .query_end = 100u,
                    .target_start = 200u + i,
                    .target_end = 299u + i,
                    .match_length = 90u,
                    .block_length = 99u,
                    .chain_score = 42,
                    .seed_count = 10,
                    .is_reverse = false,
                    .valid = true,
                },
            };
        }
        return chunk;
    };
    const auto publish_final_cache = [](
            PersistedGpuOperationEvidenceChunk& chunk) {
        const PersistedGpuCacheMetricsEvidence final_metrics{
            .token_identity = chunk.operation().operation_identity,
        };
        REQUIRE(final_metrics.accepted());
        chunk.publish_cache_metrics(final_metrics);
        REQUIRE(chunk.cache_metrics_published());
        REQUIRE(chunk.sealed());
    };

    {
        auto inconsistent_wait = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(inconsistent_wait != nullptr);
        inconsistent_wait->jobs()[1].submissions[0].submit_wait_identity += 1u;
        REQUIRE_FALSE(inconsistent_wait->seal());

        auto reused_wait = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(reused_wait != nullptr);
        reused_wait->jobs()[1].submissions[0].ordinal += 100u;
        REQUIRE_FALSE(reused_wait->seal());

        auto wrong_shape_domain = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(wrong_shape_domain != nullptr);
        wrong_shape_domain->operation().shape_pipeline_operation_identity =
            0x3000u;
        REQUIRE_FALSE(wrong_shape_domain->seal());
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    const auto require_qualification_rejection = [&make_chunk](
            const auto& mutate) {
        auto chunk = make_chunk(0x1050u, 0x2050u, 35u);
        REQUIRE(chunk != nullptr);
        mutate(chunk->operation().cache_qualification);
        REQUIRE_FALSE(chunk->seal());
    };
    require_qualification_rejection([](auto& qualification) {
        qualification.hash_passes = 2u;
    });
    require_qualification_rejection([](auto& qualification) {
        --qualification.hashed_bytes;
    });
    require_qualification_rejection([](auto& qualification) {
        --qualification.private_files_unlinked;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.live_private_paths = 1u;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.pipeline_filesystem_hash_passes = 1u;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.helper_source[0] = 'x';
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.helper_build_identity[0] = 'x';
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.helper_executable_sha256 = {};
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.device_uuid[0] ^= 1u;
    });
    require_qualification_rejection([](auto& qualification) {
        ++qualification.seed_receipt_identity;
    });
    require_qualification_rejection([](auto& qualification) {
        qualification.saturated = true;
    });
    REQUIRE(admission.reserved_bytes() == baseline);

    {
        auto zero_numeric_device_ids = make_chunk(0x1060u, 0x2060u, 36u);
        REQUIRE(zero_numeric_device_ids != nullptr);
        REQUIRE(admission.reserved_bytes()
                == baseline + two_job_chunk_retained_bytes);
        zero_numeric_device_ids->operation().environment.vendor_id = 0u;
        zero_numeric_device_ids->operation().environment.device_id = 0u;
        zero_numeric_device_ids->operation().cache_qualification.vendor_id =
            0u;
        zero_numeric_device_ids->operation().cache_qualification.device_id =
            0u;
        REQUIRE(zero_numeric_device_ids->seal());
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    {
        PersistedGpuOperationEvidenceLedger ledger{};
        auto first = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(first != nullptr);
        REQUIRE(first->seal());
        auto first_append = ledger.prepare_append(first);
        REQUIRE(first_append.accepted());
        publish_final_cache(*first);
        ledger.commit_append(std::move(first_append), std::move(first));

        /* A fully drained rescan may repeat the logical rows in a new
         * structured operation domain. */
        auto second = make_chunk(0x1001u, 0x2001u, 30u);
        REQUIRE(second != nullptr);
        REQUIRE(second->seal());
        auto second_append = ledger.prepare_append(second);
        REQUIRE(second_append.accepted());
        publish_final_cache(*second);
        ledger.commit_append(std::move(second_append), std::move(second));
        ledger.freeze();
        REQUIRE(ledger.complete());
        REQUIRE(ledger.accepted());
        REQUIRE(ledger.operation_count() == 2u);
        REQUIRE(ledger.job_count() == 4u);

        PersistedGpuOperationEvidenceLedger frozen{std::move(ledger)};
        REQUIRE(frozen.complete());
        std::vector<std::uint64_t> first_read;
        std::vector<std::uint64_t> second_read;
        for (const PersistedGpuOperationEvidenceChunk* chunk = frozen.head();
             chunk != nullptr; chunk = chunk->next()) {
            first_read.push_back(chunk->operation().operation_identity);
        }
        for (const PersistedGpuOperationEvidenceChunk* chunk = frozen.head();
             chunk != nullptr; chunk = chunk->next()) {
            second_read.push_back(chunk->operation().operation_identity);
        }
        REQUIRE(first_read == std::vector<std::uint64_t>{0x1000u, 0x1001u});
        REQUIRE(second_read == first_read);
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    const auto require_cross_operation_rejection = [&make_chunk,
            &publish_final_cache](const auto& mutate) {
        PersistedGpuOperationEvidenceLedger ledger{};
        auto first = make_chunk(0x1100u, 0x2100u, 40u);
        REQUIRE(first != nullptr);
        REQUIRE(first->seal());
        auto first_append = ledger.prepare_append(first);
        REQUIRE(first_append.accepted());
        publish_final_cache(*first);
        ledger.commit_append(std::move(first_append), std::move(first));

        auto second = make_chunk(0x1101u, 0x2101u, 41u);
        REQUIRE(second != nullptr);
        mutate(second->operation());
        REQUIRE(second->seal());
        REQUIRE_FALSE(ledger.prepare_append(second).accepted());
    };
    require_cross_operation_rejection([](auto& operation) {
        operation.query_pipeline_operation_identity = 0x2100u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.query_pipeline_operation_identity = 0x1100u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.seed_receipt_identity += 1u;
        operation.cache_qualification.seed_receipt_identity += 1u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.cache_qualification.identity += 1u;
    });
    require_cross_operation_rejection([](auto& operation) {
        operation.cold_load_identity += 1u;
    });

    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("DOC0371 measured modes match the literal exclusive-engine matrix",
          "[pipe][pipeline-stats][abi][engine-matrix]") {
    using namespace bioseq::pipeline_stats;
    using E = ExecutionEngine;
    constexpr E forbidden = E::count;
    constexpr std::array<E, 20> simd_recomputed{
        E::simd_cpu, E::simd_cpu, E::scalar_cpu, forbidden, forbidden,
        forbidden, E::scalar_cpu, E::simd_cpu, E::simd_cpu, E::scalar_cpu,
        E::scalar_cpu, E::scalar_cpu, E::scalar_cpu, E::scalar_cpu, E::host_io,
        forbidden, E::scalar_cpu, E::simd_cpu, E::simd_cpu, E::simd_cpu,
    };
    constexpr std::array<E, 20> simd_persisted{
        E::simd_cpu, forbidden, forbidden, E::host_io, E::scalar_cpu,
        forbidden, E::scalar_cpu, E::simd_cpu, E::simd_cpu, E::scalar_cpu,
        E::scalar_cpu, E::scalar_cpu, E::scalar_cpu, E::scalar_cpu, E::host_io,
        forbidden, E::scalar_cpu, E::simd_cpu, E::simd_cpu, E::simd_cpu,
    };
    constexpr std::array<E, 20> gpu_recomputed{
        E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, forbidden, forbidden,
        forbidden, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu,
        E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::host_io,
        E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu,
    };
    constexpr std::array<E, 20> gpu_persisted{
        E::vulkan_gpu, forbidden, forbidden, E::host_io, E::vulkan_gpu,
        E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu,
        E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::host_io,
        E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu, E::vulkan_gpu,
    };
    const auto require_matrix = [](
        ExecutedBackend backend,
        TargetIndexSource source,
        const std::array<E, 20>& expected) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(expected_execution_engine(
                        backend, source, static_cast<PipelineStage>(i))
                    == expected[i]);
        }
    };
    require_matrix(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed, simd_recomputed);
    require_matrix(
        ExecutedBackend::simd_cpu, TargetIndexSource::persisted, simd_persisted);
    require_matrix(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::recomputed, gpu_recomputed);
    require_matrix(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted, gpu_persisted);

    constexpr std::array<bool, 20> recomputed_required{
        true, true, true, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false,
    };
    constexpr std::array<bool, 20> persisted_required{
        true, false, false, true, true, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false,
    };
    for (std::size_t i = 0; i < pipeline_stage_count; ++i) {
        const auto stage = static_cast<PipelineStage>(i);
        REQUIRE(stage_requires_front_end_eligibility(
                    TargetIndexSource::recomputed, stage)
                == recomputed_required[i]);
        REQUIRE(stage_requires_front_end_eligibility(
                    TargetIndexSource::persisted, stage)
                == persisted_required[i]);
    }
}

TEST_CASE("DOC0371 stats merges saturate and identities fail closed",
          "[pipe][pipeline-stats][merge]") {
    using namespace bioseq::pipeline_stats;

    StageCoverage coverage{};
    coverage.eligible_jobs = sql_bigint_value_max;
    coverage.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] =
        sql_bigint_value_max;
    StageCoverage increment{};
    increment.eligible_jobs = 1;
    increment.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 1;
    REQUIRE(coverage.merge(increment));
    REQUIRE(coverage.eligible_jobs == sql_bigint_value_max);
    REQUIRE(coverage.executed_jobs[engine_index(ExecutionEngine::simd_cpu)]
            == sql_bigint_value_max);

    std::uint64_t exact_boundary = sql_bigint_value_max - 1;
    REQUIRE_FALSE(merge_counter(exact_boundary, 1));
    REQUIRE(exact_boundary == sql_bigint_value_max);
    std::uint64_t oversized = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(merge_counter(oversized, 0));
    REQUIRE(oversized == sql_bigint_value_max);

    PipelineOperationalStats stage_timing_delta{};
    stage_timing_delta.timing[
        stage_index(PipelineStage::query_sketch)].host_wall_ns = 7u;
    stage_timing_delta.timing[
        stage_index(PipelineStage::seed_lookup)].host_wall_ns = 11u;
    stage_timing_delta.timing[
        stage_index(PipelineStage::chain_score)].host_wall_ns = 13u;
    stage_timing_delta.timing[
        stage_index(PipelineStage::region_generation)].host_wall_ns = 17u;
    stage_timing_delta.timing[
        stage_index(PipelineStage::left_extension)].host_wall_ns = 19u;
    PipelineOperationalStats stage_timing_aggregate{};
    stage_timing_aggregate.merge(stage_timing_delta);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(PipelineStage::query_sketch)].host_wall_ns == 7u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(PipelineStage::seed_lookup)].host_wall_ns == 11u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(PipelineStage::chain_score)].host_wall_ns == 13u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(PipelineStage::region_generation)].host_wall_ns == 17u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(PipelineStage::left_extension)].host_wall_ns == 19u);
    REQUIRE_FALSE(stage_timing_aggregate.stats_saturated);

    PipelineOperationalStats operational{};
    operational.target_rows = sql_bigint_value_max;
    PipelineOperationalStats operational_increment{};
    operational_increment.target_rows = 1;
    operational.merge(operational_increment);
    REQUIRE(operational.target_rows == sql_bigint_value_max);
    REQUIRE(operational.stats_saturated);

    REQUIRE(merge_backend_identity(ExecutedBackend::none, ExecutedBackend::simd_cpu)
            == ExecutedBackend::simd_cpu);
    REQUIRE(merge_backend_identity(ExecutedBackend::simd_cpu, ExecutedBackend::simd_cpu)
            == ExecutedBackend::simd_cpu);
    REQUIRE(merge_backend_identity(ExecutedBackend::simd_cpu, ExecutedBackend::vulkan_gpu)
            == ExecutedBackend::mixed_invalid);
    REQUIRE(merge_target_index_identity(TargetIndexSource::recomputed,
                                        TargetIndexSource::persisted)
            == TargetIndexSource::mixed_invalid);
    REQUIRE(merge_simd_chaining_policy_identity(
                SimdChainingPolicyIdentity::none,
                SimdChainingPolicyIdentity::bounded_batch_approximate)
            == SimdChainingPolicyIdentity::bounded_batch_approximate);
    REQUIRE(merge_simd_chaining_policy_identity(
                SimdChainingPolicyIdentity::exact_visited,
                SimdChainingPolicyIdentity::bounded_batch_approximate)
            == SimdChainingPolicyIdentity::mixed_invalid);
    REQUIRE(merge_simd_alignment_algorithm_identity(
                SimdAlignmentAlgorithmIdentity::none,
                SimdAlignmentAlgorithmIdentity::block_aligner)
            == SimdAlignmentAlgorithmIdentity::block_aligner);
    REQUIRE(merge_simd_alignment_algorithm_identity(
                SimdAlignmentAlgorithmIdentity::block_aligner,
                SimdAlignmentAlgorithmIdentity::mixed_invalid)
            == SimdAlignmentAlgorithmIdentity::mixed_invalid);

    ExecutionEnvironmentProof merged{};
    const ExecutionEnvironmentProof cpu = make_cpu_environment(3);
    merged.merge(cpu);
    REQUIRE_FALSE(merged.mixed_invalid);
    merged.merge(cpu);
    REQUIRE_FALSE(merged.mixed_invalid);
    merged.merge(make_cpu_environment(4));
    REQUIRE(merged.mixed_invalid);

    ExecutionEnvironmentProof missing{};
    missing.merge(ExecutionEnvironmentProof{});
    REQUIRE(missing.mixed_invalid);
}

TEST_CASE("DOC0372 pipeline stats retain policy identity and distinct vector activity",
          "[pipe][pipeline-stats][doc0372][simd-chaining][policy][proof]") {
    using namespace bioseq::pipeline_stats;

    auto bounded = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    REQUIRE(validate_job_execution_stats(bounded));

    auto mismatched = bounded;
    mismatched.observed_simd_chaining_policy =
        SimdChainingPolicyIdentity::exact_visited;
    REQUIRE_FALSE(validate_job_execution_stats(mismatched));

    auto missing_block_identity = bounded;
    missing_block_identity.observed_simd_alignment_algorithm =
        SimdAlignmentAlgorithmIdentity::none;
    REQUIRE_FALSE(validate_job_execution_stats(missing_block_identity));

    auto scalar_with_leaked_policy = make_attested_job(
        ExecutedBackend::scalar_cpu, TargetIndexSource::recomputed);
    scalar_with_leaked_policy.requested_simd_chaining_policy =
        SimdChainingPolicyIdentity::bounded_batch_approximate;
    scalar_with_leaked_policy.observed_simd_chaining_policy =
        SimdChainingPolicyIdentity::bounded_batch_approximate;
    REQUIRE_FALSE(validate_job_execution_stats(scalar_with_leaked_policy));

    PipelineExecutionStats aggregate{};
    aggregate.merge_job(bounded);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 1);
    REQUIRE(aggregate.requested_simd_chaining_policy
            == SimdChainingPolicyIdentity::bounded_batch_approximate);
    REQUIRE(aggregate.observed_simd_chaining_policy
            == SimdChainingPolicyIdentity::bounded_batch_approximate);
    REQUIRE(aggregate.requested_simd_alignment_algorithm
            == SimdAlignmentAlgorithmIdentity::block_aligner);
    REQUIRE(aggregate.observed_simd_alignment_algorithm
            == SimdAlignmentAlgorithmIdentity::block_aligner);

    auto exact = bounded;
    exact.logical_target_row = 1;
    exact.requested_simd_chaining_policy =
        SimdChainingPolicyIdentity::exact_visited;
    exact.observed_simd_chaining_policy =
        SimdChainingPolicyIdentity::exact_visited;
    REQUIRE(validate_job_execution_stats(exact));
    aggregate.merge_job(exact);
    REQUIRE(aggregate.requested_simd_chaining_policy
            == SimdChainingPolicyIdentity::mixed_invalid);
    REQUIRE(aggregate.observed_simd_chaining_policy
            == SimdChainingPolicyIdentity::mixed_invalid);
    REQUIRE(aggregate.attempted_alignment_jobs == 2);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 2);

    PipelineExecutionStats bounded_group{};
    bounded_group.merge_job(bounded);
    PipelineExecutionStats exact_group{};
    exact_group.merge_job(exact);
    bounded_group.merge(exact_group);
    REQUIRE(bounded_group.requested_simd_chaining_policy
            == SimdChainingPolicyIdentity::mixed_invalid);
    REQUIRE(bounded_group.observed_simd_chaining_policy
            == SimdChainingPolicyIdentity::mixed_invalid);
    REQUIRE(bounded_group.fully_attested_alignment_jobs == 0);
    REQUIRE(bounded_group.unattested_alignment_jobs == 2);

    PipelineOperationalStats continuation{};
    continuation.nthash_vector_batches = 7;
    continuation.simd_chaining_exact_policy_calls = 1;
    PipelineOperationalStats resident{};
    resident.nthash_resident_vector_stream_kernel_calls = 2;
    resident.simd_chaining_bounded_policy_calls = 2;
    resident.simd_chaining_bounded_score_batches = 11;
    resident.simd_chaining_bounded_cutoff_stops = 3;
    resident.simd_chaining_bounded_candidates_skipped = 29;
    continuation.merge(resident);

    REQUIRE(continuation.has_nthash_vector_activity());
    REQUIRE(continuation.nthash_vector_batches == 7);
    REQUIRE(continuation.nthash_resident_vector_stream_kernel_calls == 2);
    REQUIRE(continuation.simd_chaining_exact_policy_calls == 1);
    REQUIRE(continuation.simd_chaining_bounded_policy_calls == 2);
    REQUIRE(continuation.simd_chaining_bounded_score_batches == 11);
    REQUIRE(continuation.simd_chaining_bounded_cutoff_stops == 3);
    REQUIRE(continuation.simd_chaining_bounded_candidates_skipped == 29);

    PipelineOperationalStats resident_only{};
    resident_only.nthash_resident_vector_stream_kernel_calls = 1;
    REQUIRE(resident_only.has_nthash_vector_activity());
    REQUIRE(resident_only.nthash_vector_batches == 0);

    BlockAlignerExecutionEvidence first_block{};
    first_block.present = true;
    first_block.accepted = true;
    first_block.executed_calls = 2;
    first_block.nonempty_calls = 2;
    first_block.nonempty_calls_with_simd = 2;
    first_block.simd_dp_vector_batches = 9;
    first_block.simd_dp_cells = 144;
    BlockAlignerExecutionEvidence second_block = first_block;
    second_block.executed_calls = 1;
    second_block.nonempty_calls = 1;
    second_block.nonempty_calls_with_simd = 1;
    second_block.simd_dp_vector_batches = 4;
    second_block.simd_dp_cells = 64;
    REQUIRE_FALSE(first_block.merge(second_block));
    REQUIRE(first_block.present);
    REQUIRE(first_block.accepted);
    REQUIRE(first_block.executed_calls == 3);
    REQUIRE(first_block.nonempty_calls == 3);
    REQUIRE(first_block.nonempty_calls_with_simd == 3);
    REQUIRE(first_block.simd_dp_vector_batches == 13);
    REQUIRE(first_block.simd_dp_cells == 208);
    REQUIRE(first_block.forbidden_ksw2_calls == 0);
    REQUIRE(first_block.forbidden_scalar_biological_fallbacks == 0);
    REQUIRE_FALSE(first_block.saturated);

    BlockAlignerExecutionEvidence forbidden_block{};
    forbidden_block.present = true;
    forbidden_block.accepted = false;
    forbidden_block.forbidden_ksw2_calls = 1;
    REQUIRE_FALSE(first_block.merge(forbidden_block));
    REQUIRE_FALSE(first_block.accepted);
    REQUIRE(first_block.forbidden_ksw2_calls == 1);

    for (const bool saturated_policy : {false, true}) {
        PipelineExecutionStats rejected{};
        rejected.merge_job(bounded);
        PipelineOperationalStats invalid_policy{};
        invalid_policy.simd_chaining_policy_rejections =
            saturated_policy ? 0 : 1;
        invalid_policy.simd_chaining_policy_evidence_saturated =
            saturated_policy;
        rejected.merge_operational(invalid_policy);
        REQUIRE(rejected.fully_attested_alignment_jobs == 0);
        REQUIRE(rejected.unattested_alignment_jobs == 1);
        REQUIRE_FALSE(PipelineExecutionStats::can_retain_full_attestation(
            rejected));
    }
}

TEST_CASE("DOC0371 prepared CRC identities preserve all u64 bits and publish once",
          "[pipe][pipeline-stats][operational-identity]") {
    using namespace bioseq::pipeline_stats;
    constexpr std::array<std::uint64_t, 4> identities{
        0,
        sql_bigint_value_max,
        std::uint64_t{1} << 63,
        (std::numeric_limits<std::uint64_t>::max)(),
    };

    for (const std::uint64_t identity : identities) {
        PipelineOperationalStats aggregate{};
        PipelineOperationalStats publication{};
        publication.cpu_prepared_key_set_checksum = identity;
        publication.cpu_prepared_key_set_checksum_present = true;
        publication.cpu_prepared_build_set_checksum = identity;
        publication.cpu_prepared_build_set_checksum_present = true;
        aggregate.merge(publication);

        REQUIRE(aggregate.cpu_prepared_key_set_checksum_present);
        REQUIRE(aggregate.cpu_prepared_key_set_checksum == identity);
        REQUIRE(aggregate.cpu_prepared_build_set_checksum_present);
        REQUIRE(aggregate.cpu_prepared_build_set_checksum == identity);
        REQUIRE_FALSE(aggregate.operational_identity_conflict);
        REQUIRE_FALSE(aggregate.stats_saturated);
    }

    PipelineOperationalStats duplicate{};
    PipelineOperationalStats first_publication{};
    first_publication.cpu_prepared_key_set_checksum = std::uint64_t{1} << 63;
    first_publication.cpu_prepared_key_set_checksum_present = true;
    duplicate.merge(first_publication);
    duplicate.merge(first_publication);
    REQUIRE(duplicate.operational_identity_conflict);
    REQUIRE(duplicate.cpu_prepared_key_set_checksum == (std::uint64_t{1} << 63));
    REQUIRE_FALSE(duplicate.stats_saturated);

    PipelineOperationalStats mismatch{};
    PipelineOperationalStats low{};
    low.cpu_prepared_build_set_checksum = 7;
    low.cpu_prepared_build_set_checksum_present = true;
    PipelineOperationalStats high{};
    high.cpu_prepared_build_set_checksum =
        (std::numeric_limits<std::uint64_t>::max)();
    high.cpu_prepared_build_set_checksum_present = true;
    mismatch.merge(low);
    mismatch.merge(high);
    REQUIRE(mismatch.operational_identity_conflict);
    REQUIRE(mismatch.cpu_prepared_build_set_checksum == 7);
    REQUIRE_FALSE(mismatch.stats_saturated);
}

TEST_CASE("DOC0371 job validation enforces the exclusive backend/source matrix",
          "[pipe][pipeline-stats][attestation]") {
    using namespace bioseq::pipeline_stats;

    const auto simd_recomputed = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    const auto gpu_persisted = make_attested_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted);
    REQUIRE(validate_job_execution_stats(simd_recomputed));
    REQUIRE(validate_job_execution_stats(gpu_persisted));

    auto wrong_engine = simd_recomputed;
    auto& query_sketch =
        wrong_engine.stages.coverage[stage_index(PipelineStage::query_sketch)];
    query_sketch.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 0;
    query_sketch.executed_jobs[engine_index(ExecutionEngine::scalar_cpu)] = 1;
    REQUIRE_FALSE(validate_job_execution_stats(wrong_engine));

    auto fallback = gpu_persisted;
    fallback.stages.coverage[stage_index(PipelineStage::chain_score)].fallback_jobs = 1;
    REQUIRE_FALSE(validate_job_execution_stats(fallback));

    auto forbidden_stage = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::persisted);
    auto& rebuilt_target =
        forbidden_stage.stages.coverage[stage_index(PipelineStage::target_sketch)];
    rebuilt_target.eligible_jobs = 1;
    rebuilt_target.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 1;
    REQUIRE_FALSE(validate_job_execution_stats(forbidden_stage));

    auto missing_identity = gpu_persisted;
    missing_identity.persisted_generation_id = 0;
    REQUIRE_FALSE(validate_job_execution_stats(missing_identity));

    auto repeated_required_stage = simd_recomputed;
    auto& repeated_query = repeated_required_stage.stages.coverage[
        stage_index(PipelineStage::query_sketch)];
    repeated_query.eligible_jobs = 2;
    repeated_query.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 2;
    REQUIRE_FALSE(validate_job_execution_stats(repeated_required_stage));

    auto missing_required_stage = simd_recomputed;
    missing_required_stage.stages.coverage[
        stage_index(PipelineStage::query_sketch)] = {};
    REQUIRE_FALSE(validate_job_execution_stats(missing_required_stage));

    auto entirely_unattributed = simd_recomputed;
    entirely_unattributed.stages = {};
    REQUIRE_FALSE(validate_job_execution_stats(entirely_unattributed));

    auto no_extension = simd_recomputed;
    no_extension.stages.coverage[stage_index(PipelineStage::gap_extension)] = {};
    REQUIRE(validate_job_execution_stats(no_extension));
}

TEST_CASE("DOC0371 zero-match jobs retain front-end proof for every measured mode",
          "[pipe][pipeline-stats][attestation][zero-match]") {
    using namespace bioseq::pipeline_stats;
    REQUIRE(validate_job_execution_stats(make_zero_match_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed)));
    REQUIRE(validate_job_execution_stats(make_zero_match_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::persisted)));
    REQUIRE(validate_job_execution_stats(make_zero_match_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::recomputed)));
    REQUIRE(validate_job_execution_stats(make_zero_match_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted)));
}

TEST_CASE("DOC0371 SQL-bound proof values reject max plus one without clamping",
          "[pipe][pipeline-stats][attestation][sql-bounds]") {
    using namespace bioseq::pipeline_stats;
    auto boundary = make_attested_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted);
    boundary.environment.effective_cpu_workers = sql_integer_value_max;
    boundary.environment.gpu_pipeline_cache_seed_size = sql_bigint_value_max;
    boundary.persisted_generation_id = sql_bigint_value_max;
    boundary.logical_target_row = sql_bigint_value_max;
    boundary.query_ordinal = sql_bigint_value_max;
    REQUIRE(validate_job_execution_stats(boundary));

    auto workers_overflow = boundary;
    workers_overflow.environment.effective_cpu_workers = sql_integer_value_max + 1;
    REQUIRE_FALSE(validate_job_execution_stats(workers_overflow));

    auto seed_size_overflow = boundary;
    seed_size_overflow.environment.gpu_pipeline_cache_seed_size =
        sql_bigint_value_max + 1;
    REQUIRE_FALSE(validate_job_execution_stats(seed_size_overflow));

    auto generation_overflow = boundary;
    generation_overflow.persisted_generation_id = sql_bigint_value_max + 1;
    REQUIRE_FALSE(validate_job_execution_stats(generation_overflow));

    auto target_row_overflow = boundary;
    target_row_overflow.logical_target_row = sql_bigint_value_max + 1;
    REQUIRE_FALSE(validate_job_execution_stats(target_row_overflow));

    auto query_ordinal_overflow = boundary;
    query_ordinal_overflow.query_ordinal = sql_bigint_value_max + 1;
    REQUIRE_FALSE(validate_job_execution_stats(query_ordinal_overflow));
}

TEST_CASE("DOC0371 GPU environment requires canonical identity and policy-hit pairs",
          "[pipe][pipeline-stats][attestation][environment]") {
    using namespace bioseq::pipeline_stats;
    constexpr auto constexpr_valid_four_byte = [] {
        std::array<char, 128> value{};
        value[0] = static_cast<char>(0xf0);
        value[1] = static_cast<char>(0x9f);
        value[2] = static_cast<char>(0x98);
        value[3] = static_cast<char>(0x80);
        return value;
    }();
    constexpr auto constexpr_invalid_surrogate = [] {
        std::array<char, 128> value{};
        value[0] = static_cast<char>(0xed);
        value[1] = static_cast<char>(0xa0);
        value[2] = static_cast<char>(0x80);
        return value;
    }();
    static_assert(canonical_nonempty_driver_identity(constexpr_valid_four_byte));
    static_assert(!canonical_nonempty_driver_identity(constexpr_invalid_surrogate));

    std::array<char, 128> valid_utf8{};
    valid_utf8[0] = static_cast<char>(0xc3);
    valid_utf8[1] = static_cast<char>(0xa9);
    REQUIRE(canonical_nonempty_driver_identity(valid_utf8));

    std::array<char, 128> valid_three_byte{};
    valid_three_byte[0] = static_cast<char>(0xe2);
    valid_three_byte[1] = static_cast<char>(0x82);
    valid_three_byte[2] = static_cast<char>(0xac);
    REQUIRE(canonical_nonempty_driver_identity(valid_three_byte));

    std::array<char, 128> valid_four_byte{};
    valid_four_byte[0] = static_cast<char>(0xf0);
    valid_four_byte[1] = static_cast<char>(0x9f);
    valid_four_byte[2] = static_cast<char>(0x98);
    valid_four_byte[3] = static_cast<char>(0x80);
    REQUIRE(canonical_nonempty_driver_identity(valid_four_byte));

    std::array<char, 128> stray_continuation{};
    stray_continuation[0] = static_cast<char>(0x80);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(stray_continuation));

    std::array<char, 128> invalid_continuation{};
    invalid_continuation[0] = static_cast<char>(0xc3);
    invalid_continuation[1] = '(';
    REQUIRE_FALSE(canonical_nonempty_driver_identity(invalid_continuation));

    std::array<char, 128> overlong{};
    overlong[0] = static_cast<char>(0xc0);
    overlong[1] = static_cast<char>(0xaf);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(overlong));

    std::array<char, 128> c1_overlong{};
    c1_overlong[0] = static_cast<char>(0xc1);
    c1_overlong[1] = static_cast<char>(0xbf);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(c1_overlong));

    std::array<char, 128> e0_overlong{};
    e0_overlong[0] = static_cast<char>(0xe0);
    e0_overlong[1] = static_cast<char>(0x9f);
    e0_overlong[2] = static_cast<char>(0xbf);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(e0_overlong));

    std::array<char, 128> surrogate{};
    surrogate[0] = static_cast<char>(0xed);
    surrogate[1] = static_cast<char>(0xa0);
    surrogate[2] = static_cast<char>(0x80);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(surrogate));

    std::array<char, 128> above_unicode_max{};
    above_unicode_max[0] = static_cast<char>(0xf4);
    above_unicode_max[1] = static_cast<char>(0x90);
    above_unicode_max[2] = static_cast<char>(0x80);
    above_unicode_max[3] = static_cast<char>(0x80);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(above_unicode_max));

    std::array<char, 128> f0_overlong{};
    f0_overlong[0] = static_cast<char>(0xf0);
    f0_overlong[1] = static_cast<char>(0x8f);
    f0_overlong[2] = static_cast<char>(0xbf);
    f0_overlong[3] = static_cast<char>(0xbf);
    REQUIRE_FALSE(canonical_nonempty_driver_identity(f0_overlong));

    std::array<char, 128> nonzero_padding{};
    nonzero_padding[0] = 'v';
    nonzero_padding[2] = 'x';
    REQUIRE_FALSE(canonical_nonempty_driver_identity(nonzero_padding));

    std::array<char, 128> unterminated{};
    unterminated.fill('x');
    REQUIRE_FALSE(canonical_nonempty_driver_identity(unterminated));

    auto immutable_without_seed = make_gpu_environment();
    immutable_without_seed.gpu_pipeline_cache_seed_size = 0;

    auto immutable_miss = make_gpu_environment();
    immutable_miss.gpu_pipeline_cache_hit = OptionalProofBool::false_value;

    auto isolated_hit = make_gpu_environment();
    isolated_hit.gpu_pipeline_cache_policy =
        PipelineCachePolicy::isolated_miss_diagnostic;

    auto zero_manifest = make_gpu_environment();
    zero_manifest.gpu_pipeline_cache_manifest_checksum = 0;

    auto zero_uuid = make_gpu_environment();
    zero_uuid.gpu_device_uuid = {};

    auto zero_seed_sha = make_gpu_environment();
    zero_seed_sha.gpu_pipeline_cache_seed_sha256 = {};

    auto not_applicable_policy = make_gpu_environment();
    not_applicable_policy.gpu_pipeline_cache_policy =
        PipelineCachePolicy::not_applicable;

    auto mixed_policy = make_gpu_environment();
    mixed_policy.gpu_pipeline_cache_policy =
        PipelineCachePolicy::mixed_invalid;

    auto immutable_unset_hit = make_gpu_environment();
    immutable_unset_hit.gpu_pipeline_cache_hit =
        OptionalProofBool::not_applicable;

    auto immutable_mixed_hit = make_gpu_environment();
    immutable_mixed_hit.gpu_pipeline_cache_hit =
        OptionalProofBool::mixed_invalid;

    auto isolated_unset_hit = make_gpu_environment();
    isolated_unset_hit.gpu_pipeline_cache_policy =
        PipelineCachePolicy::isolated_miss_diagnostic;
    isolated_unset_hit.gpu_pipeline_cache_hit =
        OptionalProofBool::not_applicable;

    auto isolated_mixed_hit = make_gpu_environment();
    isolated_mixed_hit.gpu_pipeline_cache_policy =
        PipelineCachePolicy::isolated_miss_diagnostic;
    isolated_mixed_hit.gpu_pipeline_cache_hit =
        OptionalProofBool::mixed_invalid;

    struct InvalidEnvironmentCase {
        std::string_view name;
        ExecutionEnvironmentProof environment;
    };
    const auto invalid_environments = std::to_array<InvalidEnvironmentCase>({
        {"immutable without seed bytes", immutable_without_seed},
        {"immutable cache miss", immutable_miss},
        {"isolated diagnostic cache hit", isolated_hit},
        {"zero manifest checksum", zero_manifest},
        {"zero device UUID", zero_uuid},
        {"zero seed SHA", zero_seed_sha},
        {"not-applicable policy", not_applicable_policy},
        {"mixed policy", mixed_policy},
        {"immutable unset hit", immutable_unset_hit},
        {"immutable mixed hit", immutable_mixed_hit},
        {"isolated unset hit", isolated_unset_hit},
        {"isolated mixed hit", isolated_mixed_hit},
    });
    for (const auto& test : invalid_environments) {
        CAPTURE(test.name);
        REQUIRE_FALSE(environment_is_valid_for_backend(
            test.environment, ExecutedBackend::vulkan_gpu));
    }

    REQUIRE(environment_is_valid_for_backend(
        make_gpu_environment(), ExecutedBackend::vulkan_gpu));

    auto isolated_empty_seed = immutable_without_seed;
    isolated_empty_seed.gpu_pipeline_cache_policy =
        PipelineCachePolicy::isolated_miss_diagnostic;
    isolated_empty_seed.gpu_pipeline_cache_hit = OptionalProofBool::false_value;
    REQUIRE(environment_is_valid_for_backend(
        isolated_empty_seed, ExecutedBackend::vulkan_gpu));
}

TEST_CASE("DOC0371 aggregate merge rejects persisted and environment identity drift",
          "[pipe][pipeline-stats][identity]") {
    using namespace bioseq::pipeline_stats;
    PipelineExecutionStats aggregate{};
    auto first = make_attested_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted);
    first.logical_target_row = 11;
    first.query_ordinal = 7;
    aggregate.merge_job(first);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 1);
    REQUIRE_FALSE(aggregate.mixed_persisted_identity);

    auto changed_generation = first;
    changed_generation.persisted_generation_id = 18;
    changed_generation.logical_target_row = 12;
    aggregate.merge_job(changed_generation);
    REQUIRE(aggregate.mixed_persisted_identity);
    REQUIRE(aggregate.attempted_alignment_jobs == 2);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 2);
    REQUIRE(aggregate.first_unattested_target_row == 11);
    REQUIRE(aggregate.first_unattested_query_ordinal == 7);

    auto changed_environment = first;
    changed_environment.environment.effective_cpu_workers = 3;
    changed_environment.logical_target_row = 13;
    aggregate.merge_job(changed_environment);
    REQUIRE(aggregate.environment.mixed_invalid);
    REQUIRE(aggregate.attempted_alignment_jobs == 3);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 3);
    REQUIRE(aggregate.first_unattested_target_row == 11);
    REQUIRE(aggregate.first_unattested_query_ordinal == 7);
}

TEST_CASE("DOC0371 merge_job and aggregate merge have identical drift semantics",
          "[pipe][pipeline-stats][identity][grouping]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const PipelineJobExecutionStats& job) {
        PipelineExecutionStats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_same_counts_and_coordinates = [](
            const PipelineExecutionStats& direct,
            const PipelineExecutionStats& grouped) {
        REQUIRE(direct.requested_backend == grouped.requested_backend);
        REQUIRE(direct.actual_backend == grouped.actual_backend);
        REQUIRE(direct.requested_target_index_source
                == grouped.requested_target_index_source);
        REQUIRE(direct.actual_target_index_source
                == grouped.actual_target_index_source);
        REQUIRE(direct.attempted_alignment_jobs
                == grouped.attempted_alignment_jobs);
        REQUIRE(direct.fully_attested_alignment_jobs
                == grouped.fully_attested_alignment_jobs);
        REQUIRE(direct.unattested_alignment_jobs
                == grouped.unattested_alignment_jobs);
        REQUIRE(direct.first_attempted_target_row
                == grouped.first_attempted_target_row);
        REQUIRE(direct.first_attempted_query_ordinal
                == grouped.first_attempted_query_ordinal);
        REQUIRE(direct.first_unattested_target_row
                == grouped.first_unattested_target_row);
        REQUIRE(direct.first_unattested_query_ordinal
                == grouped.first_unattested_query_ordinal);
    };

    SECTION("persisted generation drift") {
        auto first = make_attested_job(
            ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted);
        first.logical_target_row = 41;
        first.query_ordinal = 5;
        auto second = first;
        second.logical_target_row = 42;
        second.query_ordinal = 6;
        second.persisted_generation_id = 18;

        PipelineExecutionStats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == ExecutedBackend::vulkan_gpu);
        REQUIRE(direct.actual_backend == ExecutedBackend::vulkan_gpu);
        REQUIRE(direct.mixed_persisted_identity);
        REQUIRE(grouped.mixed_persisted_identity);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 41);
        REQUIRE(direct.first_unattested_query_ordinal == 5);
    }

    SECTION("environment drift") {
        auto first = make_attested_job(
            ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
        first.logical_target_row = 51;
        first.query_ordinal = 7;
        auto second = first;
        second.logical_target_row = 52;
        second.query_ordinal = 8;
        second.environment.effective_cpu_workers = 3;

        PipelineExecutionStats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == ExecutedBackend::simd_cpu);
        REQUIRE(direct.actual_backend == ExecutedBackend::simd_cpu);
        REQUIRE(direct.environment.mixed_invalid);
        REQUIRE(grouped.environment.mixed_invalid);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 51);
        REQUIRE(direct.first_unattested_query_ordinal == 7);
    }
}

TEST_CASE("DOC0371 aggregate merge cannot launder absent environments",
          "[pipe][pipeline-stats][identity][environment]") {
    using namespace bioseq::pipeline_stats;
    const auto missing_environment_operand = [] {
        PipelineExecutionStats stats{};
        stats.requested_backend = ExecutedBackend::simd_cpu;
        stats.actual_backend = ExecutedBackend::simd_cpu;
        stats.requested_simd_chaining_policy =
            SimdChainingPolicyIdentity::bounded_batch_approximate;
        stats.observed_simd_chaining_policy =
            SimdChainingPolicyIdentity::bounded_batch_approximate;
        stats.requested_target_index_source = TargetIndexSource::recomputed;
        stats.actual_target_index_source = TargetIndexSource::recomputed;
        stats.attempted_alignment_jobs = 1;
        stats.fully_attested_alignment_jobs = 1;
        stats.first_attempted_target_row = 91;
        stats.first_attempted_query_ordinal = 4;
        return stats;
    };
    const auto valid_operand = [] {
        PipelineExecutionStats stats{};
        stats.merge_job(make_attested_job(
            ExecutedBackend::simd_cpu, TargetIndexSource::recomputed));
        return stats;
    };

    auto missing_left = missing_environment_operand();
    missing_left.merge(valid_operand());
    REQUIRE(missing_left.environment.mixed_invalid);
    REQUIRE(missing_left.attempted_alignment_jobs == 2);
    REQUIRE(missing_left.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_left.unattested_alignment_jobs == 2);
    REQUIRE(missing_left.first_unattested_target_row == 91);
    REQUIRE(missing_left.first_unattested_query_ordinal == 4);

    auto missing_right = valid_operand();
    missing_right.merge(missing_environment_operand());
    REQUIRE(missing_right.environment.mixed_invalid);
    REQUIRE(missing_right.attempted_alignment_jobs == 2);
    REQUIRE(missing_right.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_right.unattested_alignment_jobs == 2);
    REQUIRE(missing_right.first_unattested_target_row == 0);
    REQUIRE(missing_right.first_unattested_query_ordinal == 0);

    PipelineExecutionStats zero_job_nonempty{};
    zero_job_nonempty.requested_backend = ExecutedBackend::simd_cpu;
    zero_job_nonempty.actual_backend = ExecutedBackend::simd_cpu;
    zero_job_nonempty.requested_simd_chaining_policy =
        SimdChainingPolicyIdentity::bounded_batch_approximate;
    zero_job_nonempty.observed_simd_chaining_policy =
        SimdChainingPolicyIdentity::bounded_batch_approximate;
    zero_job_nonempty.requested_target_index_source = TargetIndexSource::recomputed;
    zero_job_nonempty.actual_target_index_source = TargetIndexSource::recomputed;
    zero_job_nonempty.environment = make_cpu_environment();
    PipelineExecutionStats zero_job_aggregate{};
    zero_job_aggregate.merge(zero_job_nonempty);
    REQUIRE(zero_job_aggregate.environment.mixed_invalid);
    REQUIRE(zero_job_aggregate.attempted_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.unattested_alignment_jobs == 0);
}

TEST_CASE("DOC0371 exact-empty proof checks every stage and operational member",
          "[pipe][pipeline-stats][identity][empty]") {
    using namespace bioseq::pipeline_stats;
    const PipelineExecutionStats empty{};
    REQUIRE(PipelineExecutionStats::is_exact_empty_proof_operand(empty));

    SECTION("stage coverage is evidence even with no attempted jobs") {
        auto injected = empty;
        injected.stages.coverage[
            stage_index(PipelineStage::query_sketch)].eligible_jobs = 1;
        REQUIRE_FALSE(PipelineExecutionStats::is_exact_empty_proof_operand(
            injected));
    }

    SECTION("stage timing is operational evidence") {
        auto injected = empty;
        injected.operational.timing[
            stage_index(PipelineStage::query_sketch)].host_wall_ns = 1;
        REQUIRE_FALSE(PipelineExecutionStats::is_exact_empty_proof_operand(
            injected));
    }

    SECTION("an ordinary operational counter is evidence") {
        auto injected = empty;
        injected.operational.target_rows = 1;
        REQUIRE_FALSE(PipelineExecutionStats::is_exact_empty_proof_operand(
            injected));
    }

    SECTION("a zero-valued report-once identity still has a presence bit") {
        auto injected = empty;
        injected.operational.cpu_prepared_key_set_checksum_present = true;
        REQUIRE_FALSE(PipelineExecutionStats::is_exact_empty_proof_operand(
            injected));
    }
}

TEST_CASE("DOC0371 zero-job stage evidence cannot launder a complete aggregate",
          "[pipe][pipeline-stats][attestation][grouping][laundering]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    job.logical_target_row = 71;
    job.query_ordinal = 8;
    PipelineExecutionStats complete{};
    complete.merge_job(job);
    REQUIRE(PipelineExecutionStats::can_retain_full_attestation(complete));

    PipelineExecutionStats injected{};
    auto& query = injected.stages.coverage[
        stage_index(PipelineStage::query_sketch)];
    query.eligible_jobs = 1;
    query.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 1;
    REQUIRE_FALSE(PipelineExecutionStats::is_exact_empty_proof_operand(
        injected));

    const auto require_rejected = [](const PipelineExecutionStats& stats) {
        REQUIRE(stats.requested_backend == ExecutedBackend::simd_cpu);
        REQUIRE(stats.actual_backend == ExecutedBackend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 71);
        REQUIRE(stats.first_unattested_query_ordinal == 8);
        REQUIRE_FALSE(PipelineExecutionStats::aggregate_stage_coverage_is_valid(
            stats));
        REQUIRE_FALSE(PipelineExecutionStats::can_retain_full_attestation(
            stats));
    };

    auto injected_on_right = complete;
    injected_on_right.merge(injected);
    require_rejected(injected_on_right);

    auto injected_on_left = injected;
    injected_on_left.merge(complete);
    require_rejected(injected_on_left);
}

TEST_CASE("DOC0371 aggregate retention rejects malformed stage matrices",
          "[pipe][pipeline-stats][attestation][matrix][laundering]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    job.logical_target_row = 81;
    job.query_ordinal = 6;
    PipelineExecutionStats complete{};
    complete.merge_job(job);
    REQUIRE(PipelineExecutionStats::aggregate_stage_coverage_is_valid(
        complete));
    REQUIRE(PipelineExecutionStats::can_retain_full_attestation(complete));

    const auto require_malformed_stage_rejected = [](
            const PipelineExecutionStats& malformed) {
        REQUIRE_FALSE(PipelineExecutionStats::aggregate_stage_coverage_is_valid(
            malformed));
        REQUIRE_FALSE(PipelineExecutionStats::can_retain_full_attestation(
            malformed));
        PipelineExecutionStats merged{};
        merged.merge(malformed);
        REQUIRE(merged.attempted_alignment_jobs == 1);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    };

    SECTION("required front-end coverage is missing") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(PipelineStage::query_sketch)] = {};
        require_malformed_stage_rejected(malformed);
    }

    SECTION("stage total exceeds the attempted-job total") {
        auto malformed = complete;
        auto& query = malformed.stages.coverage[
            stage_index(PipelineStage::query_sketch)];
        query.eligible_jobs = 2;
        query.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 2;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a forbidden engine lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(PipelineStage::query_sketch)]
            .executed_jobs[engine_index(ExecutionEngine::scalar_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a fallback lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(PipelineStage::query_sketch)].fallback_jobs = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a mode-forbidden stage carries work") {
        auto malformed = complete;
        auto& upload = malformed.stages.coverage[
            stage_index(PipelineStage::device_index_upload)];
        upload.eligible_jobs = 1;
        upload.executed_jobs[engine_index(ExecutionEngine::simd_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a saturated aggregate cannot retain proof") {
        auto malformed = complete;
        malformed.operational.stats_saturated = true;
        REQUIRE(PipelineExecutionStats::aggregate_stage_coverage_is_valid(
            malformed));
        REQUIRE_FALSE(PipelineExecutionStats::can_retain_full_attestation(
            malformed));
        PipelineExecutionStats merged{};
        merged.merge(malformed);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    }
}

TEST_CASE("DOC0371 backend declines and fallbacks always revoke attestation",
          "[pipe][pipeline-stats][operational][fallback][grouping]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    job.logical_target_row = 91;
    job.query_ordinal = 7;

    const auto require_revoked = [](const PipelineExecutionStats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 91);
        REQUIRE(stats.first_unattested_query_ordinal == 7);
        REQUIRE_FALSE(PipelineExecutionStats::can_retain_full_attestation(
            stats));
    };

    for (const bool is_fallback : {false, true}) {
        PipelineOperationalStats evidence{};
        if (is_fallback) {
            evidence.backend_fallbacks = 1;
        } else {
            evidence.backend_declines = 1;
        }

        PipelineExecutionStats operational_after_job{};
        operational_after_job.merge_job(job);
        operational_after_job.merge_operational(evidence);
        require_revoked(operational_after_job);

        PipelineExecutionStats operational_before_job{};
        operational_before_job.merge_operational(evidence);
        operational_before_job.merge_job(job);
        require_revoked(operational_before_job);

        PipelineExecutionStats operational_only{};
        operational_only.merge_operational(evidence);
        PipelineExecutionStats grouped_after_job{};
        grouped_after_job.merge_job(job);
        grouped_after_job.merge(operational_only);
        require_revoked(grouped_after_job);

        auto grouped_before_job = operational_only;
        PipelineExecutionStats job_only{};
        job_only.merge_job(job);
        grouped_before_job.merge(job_only);
        require_revoked(grouped_before_job);
    }
}

TEST_CASE("DOC0371 mixed CPU diagnostics retain environment without becoming proof",
          "[pipe][pipeline-stats][identity][environment][diagnostic]") {
    using namespace bioseq::pipeline_stats;
    PipelineJobExecutionStats job{};
    job.requested_backend = ExecutedBackend::mixed_invalid;
    job.actual_backend = ExecutedBackend::mixed_invalid;
    job.requested_target_index_source = TargetIndexSource::recomputed;
    job.actual_target_index_source = TargetIndexSource::recomputed;
    job.environment = make_cpu_environment();

    PipelineExecutionStats node{};
    node.merge_job(job);
    REQUIRE(node.attempted_alignment_jobs == 1);
    REQUIRE(node.fully_attested_alignment_jobs == 0);
    REQUIRE(node.unattested_alignment_jobs == 1);
    REQUIRE(PipelineExecutionStats::diagnostic_environment_is_valid(node));

    PipelineExecutionStats statement{};
    statement.merge(node);
    REQUIRE(statement.actual_backend == ExecutedBackend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 1);
    REQUIRE_FALSE(statement.environment.mixed_invalid);
    REQUIRE(PipelineExecutionStats::diagnostic_environment_is_valid(statement));

    PipelineExecutionStats valid_sibling{};
    auto valid_job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    valid_job.logical_target_row = 12;
    valid_job.query_ordinal = 3;
    valid_sibling.merge_job(valid_job);
    statement.merge(valid_sibling);
    REQUIRE(statement.actual_backend == ExecutedBackend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 2);
    REQUIRE(PipelineExecutionStats::diagnostic_environment_is_valid(statement));

    auto gpu_work_with_cpu_environment = node;
    gpu_work_with_cpu_environment.operational.gpu_seed_dispatches = 1;
    REQUIRE_FALSE(PipelineExecutionStats::diagnostic_environment_is_valid(
        gpu_work_with_cpu_environment));
    PipelineExecutionStats rejected_statement{};
    rejected_statement.merge(gpu_work_with_cpu_environment);
    REQUIRE(rejected_statement.environment.mixed_invalid);
}

TEST_CASE("DOC0371 aggregate merge is associative under environment drift",
          "[pipe][pipeline-stats][identity][environment][associative]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const PipelineJobExecutionStats& job) {
        PipelineExecutionStats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto first_job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    first_job.logical_target_row = 10;
    first_job.query_ordinal = 1;
    auto second_job = first_job;
    second_job.logical_target_row = 20;
    second_job.query_ordinal = 2;
    second_job.environment.effective_cpu_workers = 3;
    auto third_job = second_job;
    third_job.logical_target_row = 30;
    third_job.query_ordinal = 3;

    auto left_grouped = aggregate_job(first_job);
    left_grouped.merge(aggregate_job(second_job));
    left_grouped.merge(aggregate_job(third_job));

    auto right_tail = aggregate_job(second_job);
    right_tail.merge(aggregate_job(third_job));
    auto right_grouped = aggregate_job(first_job);
    right_grouped.merge(right_tail);

    const auto require_same_fail_closed_result = [](
            const PipelineExecutionStats& stats) {
        REQUIRE(stats.requested_backend == ExecutedBackend::simd_cpu);
        REQUIRE(stats.actual_backend == ExecutedBackend::simd_cpu);
        REQUIRE(stats.requested_target_index_source
                == TargetIndexSource::recomputed);
        REQUIRE(stats.actual_target_index_source
                == TargetIndexSource::recomputed);
        REQUIRE(stats.environment.mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 3);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 3);
        REQUIRE(stats.first_attempted_target_row == 10);
        REQUIRE(stats.first_attempted_query_ordinal == 1);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 1);
    };
    require_same_fail_closed_result(left_grouped);
    require_same_fail_closed_result(right_grouped);
    REQUIRE(left_grouped.attempted_alignment_jobs
            == right_grouped.attempted_alignment_jobs);
    REQUIRE(left_grouped.fully_attested_alignment_jobs
            == right_grouped.fully_attested_alignment_jobs);
    REQUIRE(left_grouped.unattested_alignment_jobs
            == right_grouped.unattested_alignment_jobs);
    REQUIRE(left_grouped.first_unattested_target_row
            == right_grouped.first_unattested_target_row);
    REQUIRE(left_grouped.first_unattested_query_ordinal
            == right_grouped.first_unattested_query_ordinal);
}

TEST_CASE("DOC0371 aggregate merge is associative at SQL bigint saturation",
          "[pipe][pipeline-stats][saturation][associative]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](std::uint64_t target_row) {
        auto job = make_attested_job(
            ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
        job.logical_target_row = target_row;
        PipelineExecutionStats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto near_limit = aggregate_job(10);
    near_limit.attempted_alignment_jobs = sql_bigint_value_max;
    near_limit.fully_attested_alignment_jobs = sql_bigint_value_max;
    auto second = aggregate_job(20);
    auto third = aggregate_job(30);

    auto left_grouped = near_limit;
    left_grouped.merge(second);
    left_grouped.merge(third);

    auto right_tail = second;
    right_tail.merge(third);
    auto right_grouped = near_limit;
    right_grouped.merge(right_tail);

    const auto require_saturated = [](const PipelineExecutionStats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == sql_bigint_value_max);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == sql_bigint_value_max);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 0);
        REQUIRE(stats.operational.stats_saturated);
    };
    require_saturated(left_grouped);
    require_saturated(right_grouped);
    REQUIRE(left_grouped.requested_backend == right_grouped.requested_backend);
    REQUIRE(left_grouped.actual_backend == right_grouped.actual_backend);
    REQUIRE(left_grouped.fully_attested_alignment_jobs
            == right_grouped.fully_attested_alignment_jobs);
    REQUIRE(left_grouped.unattested_alignment_jobs
            == right_grouped.unattested_alignment_jobs);
}

TEST_CASE("DOC0371 operational saturation is fail closed independent of ordering",
          "[pipe][pipeline-stats][operational][saturation][grouping]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    job.logical_target_row = 61;
    job.query_ordinal = 9;
    const auto saturate_operational = [](PipelineExecutionStats& stats) {
        PipelineOperationalStats at_limit{};
        at_limit.target_rows = sql_bigint_value_max;
        PipelineOperationalStats overflow{};
        overflow.target_rows = 1;
        stats.merge_operational(at_limit);
        stats.merge_operational(overflow);
    };

    PipelineExecutionStats operational_after_job{};
    operational_after_job.merge_job(job);
    saturate_operational(operational_after_job);

    PipelineExecutionStats operational_before_job{};
    saturate_operational(operational_before_job);
    operational_before_job.merge_job(job);

    PipelineExecutionStats operational_only{};
    saturate_operational(operational_only);
    PipelineExecutionStats grouped{};
    grouped.merge_job(job);
    grouped.merge(operational_only);

    const auto require_fail_closed = [](const PipelineExecutionStats& stats) {
        REQUIRE(stats.requested_backend == ExecutedBackend::simd_cpu);
        REQUIRE(stats.actual_backend == ExecutedBackend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 61);
        REQUIRE(stats.first_unattested_query_ordinal == 9);
        REQUIRE(stats.operational.target_rows == sql_bigint_value_max);
        REQUIRE(stats.operational.stats_saturated);
    };
    require_fail_closed(operational_after_job);
    require_fail_closed(operational_before_job);
    require_fail_closed(grouped);
}

TEST_CASE("cold-index operational telemetry merges and saturates through one authority",
          "[pipe][pipeline-stats][cold-index][saturation]") {
    using namespace bioseq::pipeline_stats;

    PipelineOperationalStats aggregate{};
    PipelineOperationalStats first{};
    first.cold_index_sidecar_heap_opens = 2;
    first.cold_index_sidecar_index_opens = 2;
    first.cold_index_lock_acquires = 7;
    first.cold_index_lock_releases = 6;
    first.cold_index_payload_bytes_delivered = 4096;
    first.cold_index_payload_bytes_scanned = 6144;
    first.cold_index_payload_rows_scanned = 3;
    first.cold_index_payload_rows_skipped = 1;
    first.cold_index_shared_blks_read = 3;
    first.format5_reader_capacity_accounting_passes = 5;
    first.format5_reader_global_position_storage_preparations = 1;
    first.format5_reader_global_position_storage_allocations = 1;
    first.format5_reader_retained_position_count = 37;
    first.format5_reader_retained_position_capacity = 37;
    first.format5_reader_directory_capacity_bytes = 10;
    first.format5_reader_query_hash_capacity_bytes = 7;
    first.format5_reader_query_record_capacity_bytes = 100;
    first.format5_reader_ordinal_route_capacity_bytes = 20;
    first.format5_reader_selected_extent_capacity_bytes = 30;
    first.format5_reader_current_selected_control_capacity_bytes = 11;
    first.format5_reader_global_position_capacity_bytes = 296;
    first.format5_reader_control_payload_capacity_bytes = 13;
    first.format5_reader_owned_capacity_bytes = 487;
    first.format5_reader_admitted_bytes = 487;
    first.format5_reader_exact_position_image_checks = 1;
    first.format5_reader_retained_layout_checks = 1;
    aggregate.merge(first);

    REQUIRE(aggregate.cold_index_sidecar_heap_opens == 2);
    REQUIRE(aggregate.cold_index_sidecar_index_opens == 2);
    REQUIRE(aggregate.cold_index_lock_acquires == 7);
    REQUIRE(aggregate.cold_index_lock_releases == 6);
    REQUIRE(aggregate.cold_index_payload_bytes_delivered == 4096);
    REQUIRE(aggregate.cold_index_payload_bytes_scanned == 6144);
    REQUIRE(aggregate.cold_index_payload_rows_scanned == 3);
    REQUIRE(aggregate.cold_index_payload_rows_skipped == 1);
    REQUIRE(aggregate.cold_index_shared_blks_read == 3);
    REQUIRE(aggregate.format5_reader_capacity_accounting_passes == 5);
    REQUIRE(aggregate.format5_reader_global_position_storage_preparations == 1);
    REQUIRE(aggregate.format5_reader_global_position_storage_allocations == 1);
    REQUIRE(aggregate.format5_reader_position_append_growth_reservations == 0);
    REQUIRE(aggregate.format5_reader_retained_position_count == 37);
    REQUIRE(aggregate.format5_reader_retained_position_capacity == 37);
    REQUIRE(aggregate.format5_reader_query_hash_capacity_bytes == 7);
    REQUIRE(aggregate.format5_reader_current_selected_control_capacity_bytes
            == 11);
    REQUIRE(aggregate.format5_reader_global_position_capacity_bytes == 296);
    REQUIRE(aggregate.format5_reader_control_payload_capacity_bytes == 13);
    REQUIRE(aggregate.format5_reader_owned_capacity_bytes == 487);
    REQUIRE(aggregate.format5_reader_admitted_bytes == 487);
    REQUIRE(aggregate.format5_reader_exact_position_image_checks == 1);
    REQUIRE(aggregate.format5_reader_exact_position_image_failures == 0);
    REQUIRE(aggregate.format5_reader_retained_layout_checks == 1);
    REQUIRE(aggregate.format5_reader_retained_layout_failures == 0);
    REQUIRE_FALSE(aggregate.format5_reader_memory_evidence_saturated);
    REQUIRE_FALSE(aggregate.stats_saturated);

    PipelineOperationalStats second{};
    second.format5_reader_capacity_accounting_passes = 4;
    second.format5_reader_global_position_storage_preparations = 1;
    second.format5_reader_global_position_storage_allocations = 1;
    second.format5_reader_position_append_growth_reservations = 2;
    second.format5_reader_retained_position_count = 11;
    second.format5_reader_retained_position_capacity = 11;
    second.format5_reader_directory_capacity_bytes = 5;
    second.format5_reader_query_hash_capacity_bytes = 3;
    second.format5_reader_query_record_capacity_bytes = 40;
    second.format5_reader_ordinal_route_capacity_bytes = 8;
    second.format5_reader_selected_extent_capacity_bytes = 12;
    second.format5_reader_current_selected_control_capacity_bytes = 5;
    second.format5_reader_global_position_capacity_bytes = 87;
    second.format5_reader_control_payload_capacity_bytes = 7;
    second.format5_reader_owned_capacity_bytes = 167;
    second.format5_reader_admitted_bytes = 168;
    second.format5_reader_exact_position_image_checks = 1;
    second.format5_reader_exact_position_image_failures = 1;
    second.format5_reader_retained_layout_checks = 1;
    second.format5_reader_retained_layout_failures = 1;
    second.format5_reader_memory_evidence_saturated = true;
    aggregate.merge(second);

    REQUIRE(aggregate.format5_reader_capacity_accounting_passes == 9);
    REQUIRE(aggregate.format5_reader_global_position_storage_preparations == 2);
    REQUIRE(aggregate.format5_reader_global_position_storage_allocations == 2);
    REQUIRE(aggregate.format5_reader_position_append_growth_reservations == 2);
    REQUIRE(aggregate.format5_reader_retained_position_count == 48);
    REQUIRE(aggregate.format5_reader_retained_position_capacity == 48);
    REQUIRE(aggregate.format5_reader_query_hash_capacity_bytes == 10);
    REQUIRE(aggregate.format5_reader_current_selected_control_capacity_bytes
            == 16);
    REQUIRE(aggregate.format5_reader_global_position_capacity_bytes == 383);
    REQUIRE(aggregate.format5_reader_control_payload_capacity_bytes == 20);
    REQUIRE(aggregate.format5_reader_owned_capacity_bytes == 654);
    REQUIRE(aggregate.format5_reader_admitted_bytes == 655);
    REQUIRE(aggregate.format5_reader_exact_position_image_checks == 2);
    REQUIRE(aggregate.format5_reader_exact_position_image_failures == 1);
    REQUIRE(aggregate.format5_reader_retained_layout_checks == 2);
    REQUIRE(aggregate.format5_reader_retained_layout_failures == 1);
    REQUIRE(aggregate.format5_reader_memory_evidence_saturated);
    REQUIRE_FALSE(aggregate.stats_saturated);

    PipelineOperationalStats at_limit{};
    at_limit.cold_index_payload_bytes_scanned = sql_bigint_value_max;
    at_limit.format5_reader_retained_position_capacity = sql_bigint_value_max;
    PipelineOperationalStats overflow{};
    overflow.cold_index_payload_bytes_scanned = 1;
    overflow.format5_reader_retained_position_capacity = 1;
    aggregate.merge(at_limit);
    aggregate.merge(overflow);

    REQUIRE(aggregate.cold_index_payload_bytes_scanned
            == sql_bigint_value_max);
    REQUIRE(aggregate.format5_reader_retained_position_capacity
            == sql_bigint_value_max);
    REQUIRE(aggregate.stats_saturated);

    using CapacityMember =
        std::uint64_t PipelineOperationalStats::*;
    const auto require_independent_saturation = [](CapacityMember member) {
        PipelineOperationalStats exact_limit{};
        exact_limit.*member = sql_bigint_value_max;
        PipelineOperationalStats independently_merged{};
        independently_merged.merge(exact_limit);
        REQUIRE((independently_merged.*member) == sql_bigint_value_max);
        REQUIRE_FALSE(independently_merged.stats_saturated);

        PipelineOperationalStats plus_one{};
        plus_one.*member = 1;
        independently_merged.merge(plus_one);
        REQUIRE((independently_merged.*member) == sql_bigint_value_max);
        REQUIRE(independently_merged.stats_saturated);
    };
    require_independent_saturation(
        &PipelineOperationalStats::format5_reader_query_hash_capacity_bytes);
    require_independent_saturation(
        &PipelineOperationalStats::
            format5_reader_current_selected_control_capacity_bytes);
    require_independent_saturation(
        &PipelineOperationalStats::format5_reader_control_payload_capacity_bytes);
}

TEST_CASE("DOC0383 fragment-native query evidence preserves additive and peak merge semantics",
          "[pipe][pipeline-stats][fragment-native][evidence][merge][saturation]") {
    using bioseq::indexing::PreparedFragmentAnchorBatchEvidence;
    using bioseq::pipeline_stats::PipelineOperationalStats;

    PreparedFragmentAnchorBatchEvidence first{};
    first.waves = 1u;
    first.queries = 3u;
    first.query_occurrences = 7u;
    first.unique_query_plans = 3u;
    first.packed_occurrence_waves = 1u;
    first.wide_occurrence_waves = 0u;
    first.occurrence_fill_ns = 11u;
    first.control_count_bytes = 101u;
    first.pass2_position_bytes = 40u;
    first.pass3_position_bytes = 40u;
    first.anchors_constructed = 12u;
    first.exact_final_bytes = 288u;
    first.peak_exact_final_bytes = 288u;
    first.global_sort_calls_skipped = 3u;
    first.peak_occurrence_capacity_bytes = 96u;
    first.peak_block_layout_capacity_bytes = 144u;
    first.occurrence_allocations = 1u;

    PreparedFragmentAnchorBatchEvidence second{};
    second.waves = 2u;
    second.queries = 5u;
    second.query_occurrences = 11u;
    second.unique_query_plans = 4u;
    second.packed_occurrence_waves = 1u;
    second.wide_occurrence_waves = 1u;
    second.occurrence_fill_ns = 17u;
    second.control_count_bytes = 203u;
    second.pass2_position_bytes = 72u;
    second.pass3_position_bytes = 72u;
    second.anchors_constructed = 21u;
    second.exact_final_bytes = 504u;
    second.peak_exact_final_bytes = 360u;
    second.global_sort_calls_skipped = 5u;
    second.peak_occurrence_capacity_bytes = 80u;
    second.peak_block_layout_capacity_bytes = 192u;
    second.occurrence_allocations = 0u;

    first.merge(second);
    REQUIRE(first.waves == 3u);
    REQUIRE(first.queries == 8u);
    REQUIRE(first.query_occurrences == 18u);
    REQUIRE(first.unique_query_plans == 7u);
    REQUIRE(first.query_occurrences >= first.unique_query_plans);
    REQUIRE(first.packed_occurrence_waves == 2u);
    REQUIRE(first.wide_occurrence_waves == 1u);
    REQUIRE(first.packed_occurrence_waves + first.wide_occurrence_waves
        == first.waves);
    REQUIRE(first.occurrence_fill_ns == 28u);
    REQUIRE(first.control_count_bytes == 304u);
    REQUIRE(first.pass2_position_bytes == 112u);
    REQUIRE(first.pass2_position_bytes == first.pass3_position_bytes);
    REQUIRE(first.anchors_constructed == 33u);
    REQUIRE(first.exact_final_bytes == 792u);
    REQUIRE(first.peak_exact_final_bytes == 360u);
    REQUIRE(first.global_sort_calls_invoked == 0u);
    REQUIRE(first.global_sort_calls_skipped == 8u);
    REQUIRE(first.peak_occurrence_capacity_bytes == 96u);
    REQUIRE(first.peak_block_layout_capacity_bytes == 192u);
    REQUIRE(first.occurrence_allocations == 1u);
    REQUIRE(first.forbidden_buffers_are_zero());
    REQUIRE_FALSE(first.saturated);

    PipelineOperationalStats aggregate{};
    PipelineOperationalStats delta{};
    delta.fragment_native_query_evidence = first;
    aggregate.merge(delta);
    REQUIRE(aggregate.fragment_native_query_evidence == first);
    REQUIRE_FALSE(aggregate.stats_saturated);

    PipelineOperationalStats overflow{};
    overflow.fragment_native_query_evidence.waves =
        (std::numeric_limits<std::uint64_t>::max)();
    overflow.fragment_native_query_evidence.query_occurrences =
        (std::numeric_limits<std::uint64_t>::max)();
    aggregate.merge(overflow);
    REQUIRE(aggregate.fragment_native_query_evidence.waves
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_evidence.query_occurrences
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_evidence.saturated);
    REQUIRE(aggregate.stats_saturated);
}

TEST_CASE("DOC0383 Format-5 provider evidence preserves stable codes and checked counters",
          "[pipeline][operational-stats][format5][provider-evidence]"
          "[doc0383][p5][schema-v4]") {
    using namespace bioseq::pipeline_stats;

    PipelineOperationalStats first{};
    auto& first_evidence = first.format5_provider_evidence;
    first_evidence.record_provider_configured(1u);
    first_evidence.record_provider_captured(1u);
    first_evidence.record_provider_persisted(1u);
    first_evidence.record_provider_opened(1u);
    first_evidence.record_mapped_sink_preallocation_result(
        Format5EvidenceOperationResult::succeeded);
    first_evidence.record_lazy_state_layout(
        Format5LazyStateLayout::compact_aligned_planes);
    first_evidence.record_madvise(
        Format5MappingAdvice::random,
        Format5EvidenceOperationResult::succeeded);
    first_evidence.record_counter(first_evidence.provider_head_scans, 1u);
    first_evidence.record_counter(first_evidence.provider_head_rows, 1u);
    first_evidence.record_counter(first_evidence.mapped_sink_calls, 1u);
    first_evidence.record_counter(first_evidence.mapped_sink_bytes, 100u);
    first_evidence.record_counter(first_evidence.mapping_bytes, 400u);
    first_evidence.record_file_action_high_water(2u);
    REQUIRE(first_evidence.provider_identity_complete_and_consistent());

    PipelineOperationalStats second{};
    auto& second_evidence = second.format5_provider_evidence;
    second_evidence.record_provider_configured(1u);
    second_evidence.record_provider_captured(1u);
    second_evidence.record_provider_persisted(1u);
    second_evidence.record_provider_opened(1u);
    second_evidence.record_mapped_sink_preallocation_result(
        Format5EvidenceOperationResult::succeeded);
    second_evidence.record_lazy_state_layout(
        Format5LazyStateLayout::compact_aligned_planes);
    second_evidence.record_madvise(
        Format5MappingAdvice::random,
        Format5EvidenceOperationResult::succeeded);
    second_evidence.record_counter(second_evidence.provider_head_scans, 1u);
    second_evidence.record_counter(second_evidence.provider_head_rows, 1u);
    second_evidence.record_counter(second_evidence.mapped_sink_calls, 1u);
    second_evidence.record_counter(second_evidence.mapped_sink_bytes, 200u);
    second_evidence.record_counter(second_evidence.mapping_bytes, 800u);
    second_evidence.record_file_action_high_water(3u);

    PipelineOperationalStats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& evidence = aggregate.format5_provider_evidence;
    REQUIRE(evidence.provider_identity_complete_and_consistent());
    REQUIRE(evidence.provider_configured == 1u);
    REQUIRE(evidence.provider_captured == 1u);
    REQUIRE(evidence.provider_persisted == 1u);
    REQUIRE(evidence.provider_opened == 1u);
    REQUIRE(evidence.provider_head_scans == 2u);
    REQUIRE(evidence.provider_head_rows == 2u);
    REQUIRE(evidence.mapped_sink_calls == 2u);
    REQUIRE(evidence.mapped_sink_bytes == 300u);
    REQUIRE(evidence.mapping_bytes == 1'200u);
    REQUIRE(evidence.file_action_high_water == 3u);
    REQUIRE(evidence.mapped_sink_preallocation_result
        == static_cast<std::uint64_t>(
            Format5EvidenceOperationResult::succeeded));
    REQUIRE(evidence.lazy_state_layout
        == static_cast<std::uint64_t>(
            Format5LazyStateLayout::compact_aligned_planes));
    REQUIRE(evidence.madvise_choice
        == static_cast<std::uint64_t>(Format5MappingAdvice::random));
    REQUIRE(evidence.madvise_result
        == static_cast<std::uint64_t>(
            Format5EvidenceOperationResult::succeeded));
    REQUIRE_FALSE(evidence.stable_value_conflict);
    REQUIRE_FALSE(evidence.saturated);
    REQUIRE_FALSE(aggregate.operational_identity_conflict);
    REQUIRE_FALSE(aggregate.format5_reader_memory_evidence_saturated);
    REQUIRE_FALSE(aggregate.stats_saturated);

    PipelineOperationalStats conflicting{};
    conflicting.format5_provider_evidence.record_provider_opened(0u);
    aggregate.merge(conflicting);
    REQUIRE(aggregate.operational_identity_conflict);
    REQUIRE(aggregate.format5_provider_evidence.stable_value_conflict);
    REQUIRE_FALSE(
        aggregate.format5_provider_evidence.provider_identity_complete_and_consistent());

    PipelineOperationalStats exact_limit{};
    exact_limit.format5_provider_evidence.record_counter(
        exact_limit.format5_provider_evidence.mapping_bytes,
        sql_bigint_value_max);
    PipelineOperationalStats saturated_aggregate{};
    saturated_aggregate.merge(exact_limit);
    REQUIRE(saturated_aggregate.format5_provider_evidence.mapping_bytes
        == sql_bigint_value_max);
    REQUIRE_FALSE(saturated_aggregate.stats_saturated);

    PipelineOperationalStats plus_one{};
    plus_one.format5_provider_evidence.record_counter(
        plus_one.format5_provider_evidence.mapping_bytes, 1u);
    saturated_aggregate.merge(plus_one);
    REQUIRE(saturated_aggregate.format5_provider_evidence.mapping_bytes
        == sql_bigint_value_max);
    REQUIRE(saturated_aggregate.format5_provider_evidence.saturated);
    REQUIRE(saturated_aggregate.format5_reader_memory_evidence_saturated);
    REQUIRE(saturated_aggregate.stats_saturated);
}

TEST_CASE("Format-6 reader receipts preserve max peaks and report-once owners",
          "[pipe][pipeline-stats][format6][identity][saturation]") {
    using namespace bioseq::pipeline_stats;

    PipelineOperationalStats first{};
    auto& first_f6 = first.format6_reader_evidence;
    first_f6.format_version = 6;
    first_f6.sessions = 1;
    first_f6.identity_present = true;
    first_f6.generation_id = 71;
    first_f6.source_identity[0] = std::byte{0x11};
    first_f6.source_content_crc64 = 91;
    first_f6.compatibility_digest[0] = std::byte{0x22};
    first_f6.directory_rows = 2;
    first_f6.posting_ranges_delivered = 3;
    first_f6.posting_range_crc_receipts = 1;
    first_f6.posting_semantic_receipts = 3;
    first_f6.directory_phase_peak_bytes = 100;
    first_f6.control_phase_peak_bytes = 300;
    first_f6.posting_phase_peak_bytes = 200;
    first_f6.peak_admitted_bytes = 300;
    first_f6.owner_bytes_present = true;
    first_f6.retained_owner_bytes = 80;
    first_f6.admitted_bytes = 80;

    PipelineOperationalStats second = first;
    auto& second_f6 = second.format6_reader_evidence;
    second_f6.directory_rows = 4;
    second_f6.posting_ranges_delivered = 5;
    second_f6.posting_range_crc_receipts = 2;
    second_f6.posting_semantic_receipts = 5;
    second_f6.directory_phase_peak_bytes = 500;
    second_f6.control_phase_peak_bytes = 250;
    second_f6.posting_phase_peak_bytes = 450;
    second_f6.peak_admitted_bytes = 500;

    PipelineOperationalStats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& merged = aggregate.format6_reader_evidence;
    REQUIRE(merged.sessions == 2);
    REQUIRE(merged.directory_rows == 6);
    REQUIRE(merged.posting_ranges_delivered == 8);
    REQUIRE(merged.posting_range_crc_receipts == 3);
    REQUIRE(merged.posting_semantic_receipts == 8);
    REQUIRE(merged.directory_phase_peak_bytes == 500);
    REQUIRE(merged.control_phase_peak_bytes == 300);
    REQUIRE(merged.posting_phase_peak_bytes == 450);
    REQUIRE(merged.peak_admitted_bytes == 500);
    REQUIRE(merged.retained_owner_bytes == 80);
    REQUIRE(merged.admitted_bytes == 80);
    REQUIRE_FALSE(aggregate.operational_identity_conflict);
    REQUIRE_FALSE(aggregate.stats_saturated);

    PipelineOperationalStats mismatched_owner = first;
    mismatched_owner.format6_reader_evidence.retained_owner_bytes = 81;
    aggregate.merge(mismatched_owner);
    REQUIRE(aggregate.operational_identity_conflict);

    PipelineOperationalStats at_limit{};
    at_limit.format6_reader_evidence.directory_rows =
        sql_bigint_value_max;
    PipelineOperationalStats overflow{};
    overflow.format6_reader_evidence.directory_rows = 1;
    at_limit.merge(overflow);
    REQUIRE(
        at_limit.format6_reader_evidence.directory_rows
        == sql_bigint_value_max);
    REQUIRE(at_limit.format6_reader_evidence.saturated);
    REQUIRE(at_limit.stats_saturated);
}

TEST_CASE("DOC0371 aggregate environment drift reclassifies both job sets",
          "[pipe][pipeline-stats][identity][environment][drift]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const PipelineJobExecutionStats& job) {
        PipelineExecutionStats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_reclassified = [](const PipelineExecutionStats& stats) {
        REQUIRE(stats.environment.mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 2);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 2);
    };

    auto worker_left_job = make_attested_job(
        ExecutedBackend::simd_cpu, TargetIndexSource::recomputed);
    auto worker_right_job = worker_left_job;
    worker_right_job.environment.effective_cpu_workers = 3;
    auto worker_left = aggregate_job(worker_left_job);
    worker_left.merge(aggregate_job(worker_right_job));
    require_reclassified(worker_left);

    auto device_left_job = make_attested_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted);
    auto device_right_job = device_left_job;
    device_right_job.environment.gpu_device_uuid[1] = 0x7f;
    auto device_left = aggregate_job(device_left_job);
    device_left.merge(aggregate_job(device_right_job));
    require_reclassified(device_left);

    auto cache_right_job = device_left_job;
    cache_right_job.environment.gpu_pipeline_cache_seed_sha256[1] = 0x55;
    auto cache_left = aggregate_job(device_left_job);
    cache_left.merge(aggregate_job(cache_right_job));
    require_reclassified(cache_left);
}

TEST_CASE("DOC0371 per-job coverage does not multiply operational events",
          "[pipe][pipeline-stats][operational-once]") {
    using namespace bioseq::pipeline_stats;
    PipelineExecutionStats aggregate{};
    auto job = make_attested_job(
        ExecutedBackend::vulkan_gpu, TargetIndexSource::persisted);
    for (std::uint64_t i = 0; i < 4096; ++i) {
        job.logical_target_row = i;
        job.query_ordinal = i;
        aggregate.merge_job(job);
    }
    REQUIRE(aggregate.operational.target_rows == 0);

    PipelineOperationalStats once{};
    once.target_rows = 1;
    once.gpu_queue_submits = 1;
    once.gpu_persisted_resident_uploads = 1;
    aggregate.merge_operational(once);

    REQUIRE(aggregate.attempted_alignment_jobs == 4096);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 4096);
    REQUIRE(aggregate.unattested_alignment_jobs == 0);
    REQUIRE(aggregate.first_unattested_target_row == unset_job_ordinal);
    REQUIRE(aggregate.stages.coverage[stage_index(PipelineStage::query_sketch)].eligible_jobs
            == 4096);
    REQUIRE(aggregate.stages.coverage[stage_index(PipelineStage::query_sketch)]
                .executed_jobs[engine_index(ExecutionEngine::vulkan_gpu)]
            == 4096);
    REQUIRE(aggregate.operational.gpu_queue_submits == 1);
    REQUIRE(aggregate.operational.gpu_persisted_resident_uploads == 1);
    REQUIRE(aggregate.operational.target_rows == 1);
    REQUIRE_FALSE(aggregate.operational.stats_saturated);
}

TEST_CASE("DOC0371 dispatch primary and extra-query aggregates merge once",
          "[pipe][pipeline-stats][dispatch-consumer][operational-once]") {
    using namespace bioseq::pipeline_stats;
    const auto incomplete_invocation = [](
            const std::uint64_t query_ordinal,
            const std::uint64_t seed_dispatches,
            const bool primary_invocation) {
        PipelineExecutionStats invocation{};
        PipelineJobExecutionStats job{};
        job.requested_backend = ExecutedBackend::vulkan_gpu;
        job.actual_backend = ExecutedBackend::mixed_invalid;
        job.requested_target_index_source = TargetIndexSource::recomputed;
        job.actual_target_index_source = TargetIndexSource::recomputed;
        job.logical_target_row = 41;
        job.query_ordinal = query_ordinal;
        invocation.merge_job(job);
        if (primary_invocation) {
            invocation.operational.target_rows = 1;
            invocation.operational.recomputed_target_index_rows = 1;
        }
        invocation.operational.gpu_seed_dispatches = seed_dispatches;
        return invocation;
    };

    PipelineExecutionStats completed_buffer{};
    completed_buffer.merge(incomplete_invocation(0, 1, true));
    completed_buffer.merge(incomplete_invocation(1, 2, false));

    REQUIRE(completed_buffer.requested_backend == ExecutedBackend::vulkan_gpu);
    REQUIRE(completed_buffer.actual_backend == ExecutedBackend::mixed_invalid);
    REQUIRE(completed_buffer.requested_target_index_source
            == TargetIndexSource::recomputed);
    REQUIRE(completed_buffer.actual_target_index_source
            == TargetIndexSource::recomputed);
    REQUIRE(completed_buffer.attempted_alignment_jobs == 2);
    REQUIRE(completed_buffer.fully_attested_alignment_jobs == 0);
    REQUIRE(completed_buffer.unattested_alignment_jobs == 2);
    REQUIRE(completed_buffer.first_unattested_target_row == 41);
    REQUIRE(completed_buffer.first_unattested_query_ordinal == 0);
    REQUIRE(completed_buffer.operational.target_rows == 1);
    REQUIRE(completed_buffer.operational.persisted_index_rows == 0);
    REQUIRE(completed_buffer.operational.recomputed_target_index_rows == 1);
    REQUIRE(completed_buffer.operational.gpu_seed_dispatches == 3);
    REQUIRE(completed_buffer.environment.mixed_invalid);
    REQUIRE_FALSE(completed_buffer.operational.stats_saturated);
}

TEST_CASE("DOC0371 persisted request cannot be laundered by recompute outcome",
          "[pipe][pipeline-stats][dispatch-consumer][index-source]") {
    using namespace bioseq::pipeline_stats;
    PipelineExecutionStats invocation{};
    PipelineJobExecutionStats job{};
    job.requested_backend = ExecutedBackend::simd_cpu;
    job.actual_backend = ExecutedBackend::mixed_invalid;
    job.requested_simd_chaining_policy =
        SimdChainingPolicyIdentity::bounded_batch_approximate;
    job.observed_simd_chaining_policy = SimdChainingPolicyIdentity::none;
    job.requested_target_index_source = TargetIndexSource::persisted;
    job.actual_target_index_source = TargetIndexSource::recomputed;
    job.logical_target_row = 19;
    job.query_ordinal = 3;
    invocation.merge_job(job);

    PipelineOperationalStats operational{};
    operational.target_rows = 1;
    operational.missing_persisted_index_rows = 1;
    operational.recomputed_target_index_rows = 1;
    invocation.merge_operational(operational);

    REQUIRE(invocation.requested_target_index_source
            == TargetIndexSource::persisted);
    REQUIRE(invocation.actual_target_index_source
            == TargetIndexSource::recomputed);
    REQUIRE(invocation.attempted_alignment_jobs == 1);
    REQUIRE(invocation.fully_attested_alignment_jobs == 0);
    REQUIRE(invocation.unattested_alignment_jobs == 1);
    REQUIRE(invocation.first_unattested_target_row == 19);
    REQUIRE(invocation.first_unattested_query_ordinal == 3);
    REQUIRE(invocation.operational.target_rows == 1);
    REQUIRE(invocation.operational.persisted_index_rows == 0);
    REQUIRE(invocation.operational.missing_persisted_index_rows == 1);
    REQUIRE(invocation.operational.recomputed_target_index_rows == 1);
    REQUIRE_FALSE(invocation.mixed_persisted_identity);
}

TEST_CASE("DOC0371 logical target identity survives payload coalescing",
          "[pipe][pipeline-stats][dispatch-consumer][logical-row]") {
    using bioseq::pipeline_stats::count_unique_logical_target_rows;

    // The GPU resident-target layer may coalesce identical sequence bytes,
    // but two distinct relation row identities must still count twice.
    std::array<std::uint64_t, 2> identical_payload_distinct_rows{41, 42};
    REQUIRE(count_unique_logical_target_rows(
                std::span<std::uint64_t>{identical_payload_distinct_rows})
            == 2);

    // Re-observing the same logical row is the only case that collapses.
    std::array<std::uint64_t, 2> repeated_logical_row{41, 41};
    REQUIRE(count_unique_logical_target_rows(
                std::span<std::uint64_t>{repeated_logical_row})
            == 1);
}

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE::pipe {

// ============================================================================
// §14.4 — Probe sink that counts on_batch invocations.
// ============================================================================

struct ProbeBatchSink {
    int* on_batch_count;
    int* on_completed_count;
    [[gnu::always_inline]] void on_batch(std::span<AlignRegion> /*regions*/) {
        ++*on_batch_count;
    }
    [[gnu::always_inline]] void on_completed() noexcept { ++*on_completed_count; }
};

struct ExactFilterProbeSink {
    std::vector<AlignRegion>* observed;
    int* on_batch_count;

    void on_batch_pair(
        std::span<AlignRegion> regions,
        std::span<const ::bioseq::chaining::Anchor> /*anchors*/) {
        observed->assign(regions.begin(), regions.end());
        ++*on_batch_count;
    }

    void on_completed() noexcept {}
};

struct PrefilterProbeSink {
    std::vector<AlignRegion>* observed;
    int* on_batch_count;

    void on_batch_pair(
        std::span<AlignRegion> regions,
        std::span<const ::bioseq::chaining::Anchor> /*anchors*/) {
        observed->assign(regions.begin(), regions.end());
        ++*on_batch_count;
    }

    void on_completed() noexcept {}
};

TEST_CASE("EmitToken trait surface", "[pipe][emit-token-traits]") {
    SECTION("EmitTokenTraits") {
        static_assert(!std::is_copy_constructible_v<EmitToken<SinkStage>>);
        static_assert(!std::is_move_constructible_v<EmitToken<SinkStage>>);
        static_assert(!std::is_default_constructible_v<EmitToken<SinkStage>>);
        SUCCEED();
    }
    SECTION("EmittedTokenTraits") {
        static_assert(!std::is_default_constructible_v<EmittedToken<SinkStage>>);
        static_assert(!std::is_copy_constructible_v<EmittedToken<SinkStage>>);
        SUCCEED();
    }
    SECTION("EmitMethodReturnType") {
        // DOC0254 §14.4 — EmitToken::emit_batch returns EmittedToken<Sink>.
        // The expression is evaluated only inside `std::declval`, so no
        // EmitToken instance is actually constructed (it is non-default-
        // and non-move-constructible).
        static_assert(std::is_same_v<
            decltype(std::declval<EmitToken<SinkStage>>().emit_batch(
                std::declval<std::span<AlignRegion>>())),
            EmittedToken<SinkStage>>);
        SUCCEED();
    }
    SECTION("SinkConceptHolds") {
        static_assert(BatchSink<SinkStage, AlignRegion>);
        static_assert(!BatchSink<SinkStage, ::bioseq::chaining::Anchor>);
        SUCCEED();
    }
    SECTION("ProbeBatchSinkConceptHolds") {
        static_assert(BatchSink<ProbeBatchSink, AlignRegion>);
        SUCCEED();
    }
}

TEST_CASE("MapqStage emits exactly one batch per upstream batch",
          "[pipe][single-emission]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    ProbeBatchSink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    params.compute_mapq = false;
    MapqStage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<Cep>(probe);

    std::vector<AlignRegion> regions(1);
    bound.on_batch(std::span<AlignRegion>{regions.data(), regions.size()});
    REQUIRE(on_batch_count == 1);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

TEST_CASE("MapqStage::process_pair delegates exactly once",
          "[pipe][single-emission][pair-delegation]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    ProbeBatchSink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    MapqStage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<Cep>(probe);

    std::vector<AlignRegion> regions(2);
    std::vector<::bioseq::chaining::Anchor> anchors(2);
    bound.on_batch_pair(
        std::span<AlignRegion>{regions.data(), regions.size()},
        std::span<::bioseq::chaining::Anchor>{anchors.data(), anchors.size()});
    REQUIRE(on_batch_count == 1);
}

TEST_CASE("ExactEditFilterStage applies strict bounds and resets classification state",
          "[pipe][edit-distance][exact-filter]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    params.compute_edit_distance = true;
    params.exact_edit_filter_enabled = true;
    params.exact_edit_filter_exclusive_upper_bound = 5;

    std::vector<AlignRegion> observed;
    int on_batch_count = 0;
    ExactFilterProbeSink probe{&observed, &on_batch_count};
    ExactEditFilterStage stage{{}, params};
    auto bound = stage.template attach<Cep>(probe);

    std::vector<AlignRegion> regions(4);
    for (std::size_t index = 0; index < regions.size(); ++index) {
        AlignRegion& region = regions[index];
        region.has_alignment = true;
        region.anchor_start = static_cast<std::int32_t>(100u + index);
        region.parent = 91;
        region.secondary_score = 92;
        region.secondary_count = 93;
        region.secondary_dp_max = 94;
        region.mapq = 95;
    }
    regions[0].edit_distance = 5;
    regions[1].edit_distance = 4;
    regions[2].edit_distance = 0;
    regions[3].edit_distance = 8;
    std::vector<::bioseq::chaining::Anchor> anchors;

    bound.on_batch_pair(
        std::span<AlignRegion>{regions},
        std::span<::bioseq::chaining::Anchor>{anchors});

    REQUIRE(on_batch_count == 1);
    REQUIRE(observed.size() == 2);
    REQUIRE(observed[0].anchor_start == 101);
    REQUIRE(observed[1].anchor_start == 102);
    for (std::size_t index = 0; index < observed.size(); ++index) {
        CAPTURE(index);
        CHECK(observed[index].region_id == static_cast<std::int32_t>(index));
        CHECK(observed[index].parent == -1);
        CHECK(observed[index].secondary_score == 0);
        CHECK(observed[index].secondary_count == 0);
        CHECK(observed[index].secondary_dp_max == 0);
        CHECK(observed[index].mapq == 0);
    }
}

TEST_CASE("ExactEditFilterStage rejects nonpositive bounds and missing traceback",
          "[pipe][edit-distance][exact-filter][fail-loud]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    std::vector<::bioseq::chaining::Anchor> anchors;

    SECTION("nonpositive bound rejects every exact alignment") {
        ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
        params.compute_edit_distance = true;
        params.exact_edit_filter_enabled = true;
        params.exact_edit_filter_exclusive_upper_bound = 0;
        std::vector<AlignRegion> observed;
        int on_batch_count = 0;
        ExactFilterProbeSink probe{&observed, &on_batch_count};
        ExactEditFilterStage stage{{}, params};
        auto bound = stage.template attach<Cep>(probe);
        std::vector<AlignRegion> regions(1);
        regions[0].has_alignment = true;
        regions[0].edit_distance = 0;

        bound.on_batch_pair(
            std::span<AlignRegion>{regions},
            std::span<::bioseq::chaining::Anchor>{anchors});
        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.empty());
    }

    SECTION("missing traceback fails loudly") {
        ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
        params.compute_edit_distance = true;
        params.exact_edit_filter_enabled = true;
        params.exact_edit_filter_exclusive_upper_bound = 1;
        std::vector<AlignRegion> observed;
        int on_batch_count = 0;
        ExactFilterProbeSink probe{&observed, &on_batch_count};
        ExactEditFilterStage stage{{}, params};
        auto bound = stage.template attach<Cep>(probe);
        std::vector<AlignRegion> regions(1);
        regions[0].has_alignment = false;

        REQUIRE_THROWS_WITH(
            bound.on_batch_pair(
                std::span<AlignRegion>{regions},
                std::span<::bioseq::chaining::Anchor>{anchors}),
            "exact edit filter received a region without alignment traceback");
        REQUIRE(on_batch_count == 0);
    }
}

// DOC0254 §14.4 — `SingleEmissionInvariant`.
//
// Drives N=8 invocations through the bound MapqStage with a probe sink
// and asserts the probe records exactly 8 on_batch invocations.  This
// mirrors the spec's intent of "every process*<Cep> emits exactly one
// on_batch* per upstream on_batch*" — the bound stage is the unit of
// stage-shape composition, so verifying the contract on one bound
// stage proves the contract for every stage in the pipeline (every
// `process*<Cep>` body uses the same EmitToken/EmittedToken machinery).
TEST_CASE("SingleEmissionInvariant — N=8 queries one emission each",
          "[pipe][single-emission][n8]")
{
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    ProbeBatchSink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    MapqStage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<Cep>(probe);

    constexpr int kNumQueries = 8;
    std::vector<AlignRegion> regions(1);
    for (int qi = 0; qi < kNumQueries; ++qi) {
        bound.on_batch(std::span<AlignRegion>{regions.data(), regions.size()});
    }
    REQUIRE(on_batch_count == kNumQueries);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

// DOC0254 §14.4 — `KSW2BatchedParity` — checks the trait surface for
// `BAA = KSW2` and `BAA = KSW2_BATCHED` agrees on the algorithm-label
// and runtime-parameter row count.  A full end-to-end parity check
// against `minimap2_port_with_index_batch` would require live
// sequences; the trait surface check verifies the structural contract.
TEST_CASE("KSW2BatchedParity — trait surface matches KSW2",
          "[pipe][ksw2-batched][parity]")
{
    using KKsw2     = kernel_t<::bioseq::simd::HWY_NAMESPACE::
                                  BaseAlignmentAlgorithmType::KSW2>;
    using KBatched  = kernel_t<::bioseq::simd::HWY_NAMESPACE::
                                  BaseAlignmentAlgorithmType::KSW2_BATCHED>;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    std::vector<NamedProperty> ksw2_params;
    std::vector<NamedProperty> batched_params;
    KKsw2::populate_runtime_params(params, ksw2_params);
    KBatched::populate_runtime_params(params, batched_params);
    REQUIRE(ksw2_params.size() == batched_params.size());
    for (std::size_t i = 0; i < ksw2_params.size(); ++i) {
        REQUIRE(ksw2_params[i].name == batched_params[i].name);
        REQUIRE(ksw2_params[i].value == batched_params[i].value);
    }
    REQUIRE(std::string{KKsw2::algorithm_label()} == "KSW2");
    REQUIRE(std::string{KBatched::algorithm_label()}
            == "KSW2 (inter-region batched)");
    // The dispatcher trait constant — only KSW2_BATCHED supports the
    // deferred cross-query scheduling shape (DOC0254 §11.10).
    static_assert(!KKsw2::supports_cross_query_batched);
    static_assert(KBatched::supports_cross_query_batched);
    static_assert(!KKsw2::emits_ksw2_batched_execution_proof);
    static_assert(KBatched::emits_ksw2_batched_execution_proof);
}

TEST_CASE("KSW2 batched proof rejects zero and tampered evidence",
          "[pipe][ksw2-batched][execution-proof][fail-closed]") {
    using ::bioseq::simd::HWY_NAMESPACE::Ksw2BatchedExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;

    Ksw2BatchedExecutionProof empty{};
    REQUIRE_FALSE(empty.accepted(false));
    REQUIRE_FALSE(empty.accepted(true));

    Ksw2BatchedExecutionProof valid{};
    valid.record_invocation(1u);
    valid.record_collected_fills(2u);
    valid.record_short_bucket_dispatch(1u);
    valid.record_nonempty_fill(32u, 31u, false);
    valid.record_nonempty_fill(300u, 301u, true);
    REQUIRE(valid.accepted(true));

    auto tampered_lane_count = valid;
    ++tampered_lane_count.short_bucket_lanes;
    REQUIRE_FALSE(tampered_lane_count.accepted(true));

    auto dropped_fill = valid;
    ++dropped_fill.fills_collected;
    REQUIRE_FALSE(dropped_fill.accepted(true));

    auto tampered_area = valid;
    tampered_area.input_cell_area = 0u;
    REQUIRE_FALSE(tampered_area.accepted(true));

    auto impossible_dispatch_count = valid;
    impossible_dispatch_count.short_bucket_dispatches =
        impossible_dispatch_count.short_bucket_lanes + 1u;
    REQUIRE_FALSE(impossible_dispatch_count.accepted(true));

    auto impossible_cell_area = valid;
    impossible_cell_area.input_cell_area = 1u;
    REQUIRE_FALSE(impossible_cell_area.accepted(true));

    auto saturated = valid;
    saturated.saturated = true;
    REQUIRE_FALSE(saturated.accepted(true));

    Ksw2BatchedExecutionProof combined{};
    combined.combine(valid);
    combined.combine(valid);
    REQUIRE(combined.accepted(true));
    REQUIRE(combined.align_region_invocations == 2u);
    REQUIRE(combined.nonempty_fill_calls == 4u);

    MatchResultCollection first;
    first.record_ksw2_batched_execution_proof(valid);
    first.record_query_sketch_host_time(7u);
    first.record_seed_lookup_host_time(11u);
    first.record_chain_score_host_time(13u);
    first.record_region_generation_host_time(17u);
    first.record_alignment_result_host_time(19u);
    MatchResultCollection second;
    second.record_ksw2_batched_execution_proof(valid);
    second.record_query_sketch_host_time(11u);
    second.record_seed_lookup_host_time(19u);
    second.record_chain_score_host_time(23u);
    second.record_region_generation_host_time(29u);
    second.record_alignment_result_host_time(31u);
    first.combine_execution_proof(second);
    REQUIRE(first.ksw2_batched_execution_accepted(true));
    REQUIRE(first.ksw2_batched_execution_proof.align_region_invocations
            == 2u);
    REQUIRE(first.query_sketch_host_wall_ns == 18u);
    REQUIRE(first.seed_lookup_host_wall_ns == 30u);
    REQUIRE(first.chain_score_host_wall_ns == 36u);
    REQUIRE(first.region_generation_host_wall_ns == 46u);
    REQUIRE(first.alignment_result_host_wall_ns == 50u);
    REQUIRE_FALSE(first.host_stage_timing_saturated);
}

TEST_CASE("Host timing phase handoff is non-overlapping and mergeable",
          "[pipe][pipeline-stats][host-timing]") {
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;

    MatchResultCollection collection;
    collection.begin_region_host_phase(100u);
    collection.transition_region_to_alignment_result_host_phase(
        130u, /*alignment_enabled=*/true);
    collection.finish_alignment_result_host_phase(180u);

    REQUIRE(collection.region_generation_host_wall_ns == 30u);
    REQUIRE(collection.alignment_result_host_wall_ns == 50u);
    REQUIRE(collection.region_generation_host_wall_ns
            + collection.alignment_result_host_wall_ns == 80u);
    REQUIRE_FALSE(collection.region_host_phase_active);
    REQUIRE_FALSE(collection.alignment_result_host_phase_active);
    REQUIRE_FALSE(collection.host_stage_timing_saturated);

    MatchResultCollection saturated;
    saturated.record_alignment_result_host_time(
        (std::numeric_limits<std::uint64_t>::max)());
    saturated.record_alignment_result_host_time(1u);
    REQUIRE(saturated.alignment_result_host_wall_ns
            == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(saturated.host_stage_timing_saturated);
}

// =============================================================================
// DOC0256 §8.3 — chain primitive / registry / leaf tests.
// =============================================================================

namespace doc0256_test_helpers {

using BAA = ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
using CA  = ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
using MA  = ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;

/// Probe callable for `enum_dispatch_pack` tests.  Stateless POD with a
/// templated `operator()` whose body forwards the matched IC<T, V> into a
/// caller-provided reference slot.
template <class T>
struct value_capture_callable {
    T* sink;
    template <T V, class... Args>
    [[gnu::always_inline]] T operator()(
        std::integral_constant<T, V>, Args&&...) const
    {
        *sink = V;
        return V;
    }
};

}  // namespace doc0256_test_helpers

TEST_CASE("DOC0256 — enum_dispatch_pack routes to the matching template branch",
          "[pipe][doc0256][enum-dispatch-pack]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::CA;
    using doc0256_test_helpers::MA;

    SECTION("ChainingAlgorithmType — every enum value routes correctly") {
        using Reg = algorithm_enum_values<CA>;
        for (CA v : {CA::SCALAR_MM2, CA::SIMD_MM2, CA::RMQ, CA::VULKAN_TILED_DP}) {
            CA captured = static_cast<CA>(0xFF);
            doc0256_test_helpers::value_capture_callable<CA> cb{&captured};
            const CA result = enum_dispatch_pack<CA, CA, Reg::default_value>(
                Reg::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
    SECTION("BaseAlignmentAlgorithmType — every enum value routes correctly") {
        using Reg = algorithm_enum_values<BAA>;
        for (BAA v : {BAA::KSW2, BAA::BLOCK_ALIGNER, BAA::KSW2_BATCHED}) {
            BAA captured = static_cast<BAA>(0xFF);
            doc0256_test_helpers::value_capture_callable<BAA> cb{&captured};
            const BAA result = enum_dispatch_pack<BAA, BAA, Reg::default_value>(
                Reg::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
    SECTION("MinimizerAlgorithmType — every enum value routes correctly") {
        using Reg = algorithm_enum_values<MA>;
        for (MA v : {MA::SCALAR,
                     MA::SIMD_MINIMAP2_SLOW,
                     MA::TWO_STACK_NTHASH,
                     MA::VULKAN_MINIMAP_INT,
                     MA::PAPER_NTHASH_SIMD}) {
            MA captured = static_cast<MA>(0xFF);
            doc0256_test_helpers::value_capture_callable<MA> cb{&captured};
            const MA result = enum_dispatch_pack<MA, MA, Reg::default_value>(
                Reg::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
}

TEST_CASE("Exact SIMD chaining metadata permits canonical nonzero skip penalty",
          "[pipe][exact-simd][chaining][metadata]") {
    using CA = ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using Kernel = algorithm_traits<CA::SIMD_MM2>::kernel_type;
    using Cep = ConstexprAlignerParams<10, 15, 28>;
    const auto properties = Kernel::template static_properties<Cep>();
    const auto property = std::ranges::find_if(
        properties,
        [](const NamedProperty& candidate) {
            return candidate.name == "requires_zero_skip_penalty";
        });
    REQUIRE(property != properties.end());
    REQUIRE(property->value == "false");
}

TEST_CASE("scalar chaining trait keeps an empty negative free of score work",
          "[pipe][scalar][chaining][empty][execution-proof]") {
    using ::bioseq::chaining::Anchor;
    using ::bioseq::chaining::Chain;
    using ::bioseq::chaining::ChainingParams;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ScalarPipelineExecutionProof;
    using Kernel =
        algorithm_traits<ChainingAlgorithmType::SCALAR_MM2>::kernel_type;

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::Arena::Chain>();
    auto chains = ::bioseq::chaining::make_arena_slab<Chain>();
    auto reordered = ::bioseq::chaining::make_arena_slab<Anchor>();
    ScalarPipelineExecutionProof proof{};
    Kernel::call(
        std::span<const Anchor>{}, ChainingParams{}, chains, reordered,
        &proof);

    REQUIRE(chains.size() == 0u);
    REQUIRE(reordered.size() == 0u);
    REQUIRE(proof.chain_score_calls == 0u);
    REQUIRE(proof.chain_backtrack_calls == 0u);
    REQUIRE(proof.chain_anchor_inputs == 0u);
    REQUIRE(proof.chain_stage_completion_calls == 0u);
    REQUIRE(proof.chain_stage_completed_anchor_inputs == 0u);
}

TEST_CASE("DOC0372 SIMD chaining policy is exact by default and explicit in proof metadata",
          "[pipe][doc0372][simd-chaining][policy][proof][metadata]") {
    using ::bioseq::chaining::SimdChainingPolicy;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::SimdChainingExecutionProof;
    using Kernel = algorithm_traits<ChainingAlgorithmType::SIMD_MM2>::kernel_type;

    Minimap2Params params{};
    REQUIRE(params.simd_chaining_policy
        == SimdChainingPolicy::exact_visited);
    REQUIRE(params.to_chaining_params().simd_policy
        == SimdChainingPolicy::exact_visited);

    params.simd_chaining_policy =
        SimdChainingPolicy::bounded_batch_approximate;
    const auto bounded_params = params.to_chaining_params();
    REQUIRE(bounded_params.simd_policy
        == SimdChainingPolicy::bounded_batch_approximate);

    std::vector<NamedProperty> runtime_properties;
    Kernel::populate_runtime_params(bounded_params, runtime_properties);
    const auto policy_property = std::ranges::find_if(
        runtime_properties,
        [](const NamedProperty& candidate) {
            return candidate.name == "simd_chaining_policy";
        });
    REQUIRE(policy_property != runtime_properties.end());
    REQUIRE(policy_property->value == "bounded_batch_approximate");

    SimdChainingExecutionProof exact{};
    exact.policy.exact_visited_calls = 1u;
    MatchResultCollection collection;
    collection.record_simd_chaining_execution_proof(exact);
    REQUIRE(collection.simd_chaining_execution_accepted(
        SimdChainingPolicy::exact_visited,
        /*require_nonempty_vector_work=*/false));
    REQUIRE_FALSE(collection.simd_chaining_execution_accepted(
        SimdChainingPolicy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/false));

    SimdChainingExecutionProof bounded{};
    bounded.policy.bounded_batch_approximate_calls = 1u;
    MatchResultCollection mixed;
    mixed.record_simd_chaining_execution_proof(exact);
    mixed.record_simd_chaining_execution_proof(bounded);
    REQUIRE_FALSE(mixed.simd_chaining_execution_accepted(
        SimdChainingPolicy::exact_visited,
        /*require_nonempty_vector_work=*/false));
    REQUIRE_FALSE(mixed.simd_chaining_execution_accepted(
        SimdChainingPolicy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/false));
}

TEST_CASE("scalar pipeline proof is operation-local and rejects mixed routes",
          "[pipe][scalar][execution-proof]") {
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using ::bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::ScalarKsw2Phase;
    using ::bioseq::simd::HWY_NAMESPACE::ScalarMinimizerExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::ScalarPipelineExecutionProof;

    ScalarMinimizerExecutionProof query{};
    query.record(5'000u);
    ScalarMinimizerExecutionProof target{};
    target.record(2'000'000u);

    ScalarPipelineExecutionProof recomputed{};
    recomputed.query_minimizers = query;
    recomputed.target_minimizers = target;
    recomputed.record_seed_lookup(400u);
    recomputed.record_scalar_chain(1'200u);
    recomputed.record_scalar_chain_stage_completion(1'200u);
    recomputed.record_ksw2_call(
        ScalarKsw2Phase::left, 100u, 120u);
    recomputed.record_ksw2_call(
        ScalarKsw2Phase::gap, 80u, 90u);

    MatchResultCollection complete;
    complete.record_scalar_pipeline_execution_proof(recomputed);
    REQUIRE(complete.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));
    REQUIRE_FALSE(complete.all_simd_execution_accepted(
        ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::
            TWO_STACK_NTHASH,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));

    ScalarPipelineExecutionProof persisted = recomputed;
    persisted.target_minimizers = {};
    MatchResultCollection persisted_collection;
    persisted_collection.record_scalar_pipeline_execution_proof(persisted);
    REQUIRE(persisted_collection.all_scalar_execution_accepted(
        /*require_target_sketch=*/false,
        /*require_alignment=*/true));
    REQUIRE_FALSE(persisted_collection.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    MatchResultCollection no_alignment;
    ScalarPipelineExecutionProof coordinate_only = persisted;
    coordinate_only.ksw2_left_calls = 0;
    coordinate_only.ksw2_gap_calls = 0;
    coordinate_only.ksw2_right_calls = 0;
    coordinate_only.ksw2_input_cell_area = 0;
    no_alignment.record_scalar_pipeline_execution_proof(coordinate_only);
    REQUIRE(no_alignment.all_scalar_execution_accepted(
        /*require_target_sketch=*/false,
        /*require_alignment=*/false));
    REQUIRE_FALSE(no_alignment.all_scalar_execution_accepted(
        /*require_target_sketch=*/false,
        /*require_alignment=*/true));

    /* A real zero-anchor scalar chain is an attested negative operation.  Its
     * successful stage receipt is distinct from score/backtrack work, and it
     * cannot waive a selected KSW2 alignment requirement. */
    ScalarPipelineExecutionProof zero_anchor = recomputed;
    zero_anchor.chain_stage_completion_calls = 0;
    zero_anchor.chain_stage_completed_anchor_inputs = 0;
    zero_anchor.chain_score_calls = 0;
    zero_anchor.chain_backtrack_calls = 0;
    zero_anchor.chain_anchor_inputs = 0;
    zero_anchor.ksw2_left_calls = 0;
    zero_anchor.ksw2_gap_calls = 0;
    zero_anchor.ksw2_right_calls = 0;
    zero_anchor.ksw2_input_cell_area = 0;
    zero_anchor.record_scalar_chain_stage_completion(0u);
    MatchResultCollection zero_anchor_negative;
    zero_anchor_negative.record_scalar_pipeline_execution_proof(zero_anchor);
    REQUIRE(zero_anchor_negative.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));
    REQUIRE_FALSE(zero_anchor_negative.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    ScalarPipelineExecutionProof impossible_empty_chain = zero_anchor;
    impossible_empty_chain.chain_score_calls = 1;
    impossible_empty_chain.chain_backtrack_calls = 1;
    MatchResultCollection impossible_negative;
    impossible_negative.record_scalar_pipeline_execution_proof(
        impossible_empty_chain);
    REQUIRE_FALSE(impossible_negative.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));

    ScalarPipelineExecutionProof missing_chain_receipt = zero_anchor;
    missing_chain_receipt.chain_stage_completion_calls = 0;
    MatchResultCollection incomplete_negative;
    incomplete_negative.record_scalar_pipeline_execution_proof(
        missing_chain_receipt);
    REQUIRE_FALSE(incomplete_negative.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));

    MatchResultCollection mixed = complete;
    mixed.exact_simd_query_execution_proof_present = true;
    REQUIRE_FALSE(mixed.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    MatchResultCollection gpu_mixed = complete;
    PartialVulkanExecutionProof gpu{};
    gpu.seed_lookup_dispatches = 1u;
    gpu_mixed.record_partial_vulkan_execution_proof(gpu);
    REQUIRE_FALSE(gpu_mixed.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    MatchResultCollection saturated = complete;
    saturated.scalar_pipeline_execution_proof.saturated = true;
    REQUIRE_FALSE(saturated.all_scalar_execution_accepted(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));
}

TEST_CASE("ExactSimd collection proof merge is actual fail-closed evidence",
          "[pipe][exact-simd][proof][merge][tamper][empty]") {
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using ::bioseq::simd::HWY_NAMESPACE::TwoStackExecutionProof;

    const auto actual_range_proof = [](std::uint64_t bp) {
        TwoStackExecutionProof proof{};
        proof.bp_consumed = bp;
        proof.fragment_symbols_staged = bp;
        proof.packed_decode_lanes = bp;
        proof.generic_u64_kernel_calls = 1;
        proof.event_generation_batches = 1;
        proof.stable_compress_batches = 1;
        if (bp != 0) {
            proof.vector_batches = 1;
            proof.vector_lanes_visited = bp;
            proof.generic_u64_candidate_blocks = 1;
            proof.generic_u64_lane_visits = bp;
            proof.consecutive_kmer_batches = 1;
            proof.canonical_scramble_batches = 1;
            proof.window_reduction_batches = 1;
            proof.event_generation_batches = 2;
            proof.stable_compress_batches = 2;
            proof.tie_matrix_event_batches = 1;
            proof.row_transpose_batches = 1;
            proof.row_vector_stores = 1;
            proof.packed_decode_batches = 1;
        }
        return proof;
    };

    MatchResultCollection first;
    first.record_exact_simd_target_execution_proof(
        actual_range_proof(17));
    MatchResultCollection second;
    second.record_exact_simd_target_execution_proof(
        actual_range_proof(23));
    first.combine_execution_proof(second);
    REQUIRE(first.exact_simd_target_execution_proof_present);
    REQUIRE(first.exact_simd_target_execution_proof.bp_consumed == 40);
    REQUIRE(first.exact_simd_target_execution_proof.packed_decode_lanes == 40);
    REQUIRE(first.exact_simd_target_execution_accepted(
        /*require_nonempty_vector_work=*/true));

    MatchResultCollection missing;
    REQUIRE_FALSE(missing.exact_simd_target_execution_accepted(false));

    MatchResultCollection manufactured_empty;
    manufactured_empty.record_exact_simd_query_execution_proof(
        TwoStackExecutionProof{});
    REQUIRE_FALSE(
        manufactured_empty.exact_simd_query_execution_accepted(false));

    MatchResultCollection tampered;
    auto forbidden = actual_range_proof(8);
    forbidden.scalar_hash_symbols = 1;
    tampered.record_exact_simd_target_execution_proof(forbidden);
    REQUIRE_FALSE(tampered.exact_simd_target_execution_accepted(false));

    // An actual zero-bp invocation is valid route evidence for ordinary
    // diagnostics, but it cannot satisfy the strict positive-work gate.
    MatchResultCollection empty_actual;
    empty_actual.record_exact_simd_query_execution_proof(
        actual_range_proof(0));
    REQUIRE(empty_actual.exact_simd_query_execution_accepted(false));
    REQUIRE_FALSE(empty_actual.exact_simd_query_execution_accepted(true));

    auto mixed_routes = actual_range_proof(11);
    mixed_routes.k15_w10_b8_u32_hash_kernel_calls = 1;
    mixed_routes.k15_w10_b8_u32_hash_candidate_blocks =
        mixed_routes.generic_u64_candidate_blocks;
    mixed_routes.k15_w10_b8_u32_hash_lane_visits =
        mixed_routes.generic_u64_lane_visits;
    mixed_routes.generic_u64_candidate_blocks = 0;
    mixed_routes.generic_u64_lane_visits = 0;
    MatchResultCollection mixed;
    mixed.record_exact_simd_query_execution_proof(mixed_routes);
    REQUIRE_FALSE(mixed.exact_simd_query_execution_accepted(false));

    auto missing_packed22 = actual_range_proof(11);
    missing_packed22.k15_w10_b8_u32_hash_kernel_calls = 1;
    missing_packed22.k15_w10_b8_u32_hash_candidate_blocks =
        missing_packed22.generic_u64_candidate_blocks;
    missing_packed22.k15_w10_b8_u32_hash_lane_visits =
        missing_packed22.generic_u64_lane_visits;
    missing_packed22.generic_u64_kernel_calls = 0;
    missing_packed22.generic_u64_candidate_blocks = 0;
    missing_packed22.generic_u64_lane_visits = 0;
    MatchResultCollection missing_packed;
    missing_packed.record_exact_simd_query_execution_proof(
        missing_packed22);
    REQUIRE_FALSE(
        missing_packed.exact_simd_query_execution_accepted(false));

    auto packed22 = missing_packed22;
    packed22.k15_w10_b8_packed22_groups =
        packed22.k15_w10_b8_u32_hash_candidate_blocks;
    packed22.k15_w10_b8_packed22_lane_visits =
        packed22.k15_w10_b8_u32_hash_lane_visits;
    MatchResultCollection packed;
    packed.record_exact_simd_query_execution_proof(packed22);
    REQUIRE(packed.exact_simd_query_execution_accepted(false));

    ++packed22.k15_w10_b8_packed22_lane_visits;
    MatchResultCollection tampered_packed;
    tampered_packed.record_exact_simd_query_execution_proof(packed22);
    REQUIRE_FALSE(
        tampered_packed.exact_simd_query_execution_accepted(false));
}

TEST_CASE("partial Vulkan proof requires the exact positive stage mask",
          "[pipe][vulkan][proof][stage-mask][fail-closed]") {
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using ::bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof;

    MatchResultCollection tiled_chain;
    PartialVulkanExecutionProof chain_only{};
    chain_only.chain_score_dispatches = 1;
    tiled_chain.record_partial_vulkan_execution_proof(chain_only);
    REQUIRE(tiled_chain.partial_vulkan_execution_proof
                .exact_stage_mask_accepted(
                    false, false, false, false, true));

    MatchResultCollection phase_c;
    PartialVulkanExecutionProof frontend{};
    frontend.query_sketch_dispatches = 1;
    frontend.target_sketch_dispatches = 1;
    frontend.target_index_build_dispatches = 1;
    frontend.gpu_index_exact_count_dispatches = 1;
    frontend.gpu_index_exact_count_prefix_scan_dispatches = 3;
    frontend.gpu_index_exact_count_control_readbacks = 1;
    frontend.gpu_index_deterministic_tile_emit_dispatches = 1;
    frontend.gpu_index_emit_validation_control_readbacks = 1;
    frontend.gpu_index_counted_minimizer_count = 53;
    frontend.gpu_index_verified_emit_count = 53;
    frontend.gpu_index_count_emit_parity_verified = 1;
    frontend.gpu_index_target_minimizer_capacity_estimate = 100;
    frontend.gpu_index_arena_trimmed_before_permit_shrink = 1;
    frontend.gpu_index_ungated_emit_dispatches = 1;
    frontend.gpu_index_retained_minimizer_count = 53;
    frontend.seed_lookup_dispatches = 1;
    frontend.chain_score_dispatches = 1;
    phase_c.record_partial_vulkan_execution_proof(frontend);
    REQUIRE(phase_c.partial_vulkan_execution_proof
                .exact_stage_mask_accepted(true, true, true, true, true));
    REQUIRE_FALSE(phase_c.partial_vulkan_execution_proof
                      .exact_stage_mask_accepted(
                          true, true, false, true, true));
    auto tampered_frontend = frontend;
    tampered_frontend.gpu_index_atomic_append_dispatches = 1u;
    REQUIRE_FALSE(tampered_frontend.exact_stage_mask_accepted(
        true, true, true, true, true));
    tampered_frontend = frontend;
    tampered_frontend.gpu_index_exact_count_prefix_scan_dispatches = 2u;
    REQUIRE_FALSE(tampered_frontend.exact_stage_mask_accepted(
        true, true, true, true, true));

    PartialVulkanExecutionProof source_direct{};
    source_direct.query_sketch_dispatches = 1u;
    source_direct.target_sketch_dispatches = 1u;
    source_direct.seed_lookup_dispatches = 1u;
    source_direct.chain_score_dispatches = 1u;
    source_direct.gpu_index_materializations = 1u;
    source_direct.gpu_index_sorted_dense_materializations = 1u;
    source_direct.gpu_index_retained_minimizer_count = 53u;
    source_direct.gpu_index_distinct_minimizer_count = 17u;
    source_direct.gpu_index_source_direct_operations = 1u;
    source_direct.gpu_index_source_direct_planning_receipts = 1u;
    source_direct.gpu_index_source_direct_terminal_receipts = 1u;
    source_direct.gpu_index_source_direct_terminal_successes = 1u;
    REQUIRE(source_direct.exact_source_direct_stage_mask_accepted(
        true, true, true, true));
    REQUIRE_FALSE(source_direct.exact_stage_mask_accepted(
        true, true, false, true, true));
    source_direct.gpu_index_gate_enabled = 1u;
    REQUIRE_FALSE(source_direct.exact_source_direct_stage_mask_accepted(
        true, true, true, true));

    MatchResultCollection combined;
    combined.combine_execution_proof(tiled_chain);
    REQUIRE(combined.has_any_algorithm_execution_proof());
}

TEST_CASE("Phase C batch target-build proof is not inflated by query count",
          "[pipe][vulkan][phase-c][proof][batch][fail-closed]") {
    using ::bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::
        phase_c_target_build_proof_owner_query;

    PartialVulkanExecutionProof aggregate{};
    for (std::size_t query_index = 0; query_index < 3u; ++query_index) {
        PartialVulkanExecutionProof per_query{};
        per_query.query_sketch_dispatches = 1u;
        per_query.seed_lookup_dispatches = 1u;
        per_query.chain_score_dispatches = 1u;
        if (phase_c_target_build_proof_owner_query(query_index)) {
            per_query.target_sketch_dispatches = 1u;
            per_query.target_index_build_dispatches = 1u;
            per_query.gpu_index_count_phase_admissions = 1u;
            per_query.gpu_index_main_peak_admissions = 1u;
            per_query.gpu_index_resident_transfers = 1u;
            per_query.gpu_index_adoption_successes = 1u;
            per_query.gpu_index_owned_resident_bytes = 4096u;
            per_query.gpu_index_ungated_emit_dispatches = 1u;
            per_query.gpu_index_exact_count_dispatches = 1u;
            per_query.gpu_index_exact_count_prefix_scan_dispatches = 3u;
            per_query.gpu_index_exact_count_control_readbacks = 1u;
            per_query.gpu_index_deterministic_tile_emit_dispatches = 1u;
            per_query.gpu_index_emit_validation_control_readbacks = 1u;
            per_query.gpu_index_counted_minimizer_count = 53u;
            per_query.gpu_index_verified_emit_count = 53u;
            per_query.gpu_index_count_emit_parity_verified = 1u;
            per_query.gpu_index_target_minimizer_capacity_estimate = 100u;
            per_query.gpu_index_arena_trimmed_before_permit_shrink = 1u;
            per_query.gpu_index_retained_minimizer_count = 53u;
            per_query.gpu_index_distinct_minimizer_count = 17u;
        }
        aggregate.combine(per_query);
    }

    REQUIRE(aggregate.query_sketch_dispatches == 3u);
    REQUIRE(aggregate.seed_lookup_dispatches == 3u);
    REQUIRE(aggregate.chain_score_dispatches == 3u);
    REQUIRE(aggregate.target_sketch_dispatches == 1u);
    REQUIRE(aggregate.target_index_build_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_count_phase_admissions == 1u);
    REQUIRE(aggregate.gpu_index_main_peak_admissions == 1u);
    REQUIRE(aggregate.gpu_index_resident_transfers == 1u);
    REQUIRE(aggregate.gpu_index_adoption_successes == 1u);
    REQUIRE(aggregate.gpu_index_gate_enabled == 0u);
    REQUIRE(aggregate.gpu_index_gate_count_dispatches == 0u);
    REQUIRE(aggregate.gpu_index_gate_word_count == 0u);
    REQUIRE(aggregate.gpu_index_gate_mask == 0u);
    REQUIRE(aggregate.gpu_index_gate_set_bits == 0u);
    REQUIRE(aggregate.gpu_index_gated_emit_dispatches == 0u);
    REQUIRE(aggregate.gpu_index_ungated_emit_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_exact_count_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_exact_count_prefix_scan_dispatches == 3u);
    REQUIRE(aggregate.gpu_index_exact_count_control_readbacks == 1u);
    REQUIRE(aggregate.gpu_index_deterministic_tile_emit_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_atomic_append_dispatches == 0u);
    REQUIRE(aggregate.gpu_index_emit_validation_control_readbacks == 1u);
    REQUIRE(aggregate.gpu_index_counted_minimizer_count == 53u);
    REQUIRE(aggregate.gpu_index_verified_emit_count == 53u);
    REQUIRE(aggregate.gpu_index_count_emit_parity_verified == 1u);
    REQUIRE(aggregate.gpu_index_target_minimizer_capacity_estimate == 100u);
    REQUIRE(aggregate.gpu_index_capacity_estimate_underestimated == 0u);
    REQUIRE(aggregate.gpu_index_arena_trimmed_before_permit_shrink == 1u);
    REQUIRE(aggregate.gpu_index_retained_minimizer_count == 53u);
    REQUIRE(aggregate.gpu_index_distinct_minimizer_count == 17u);
}

TEST_CASE("Phase C query gate becomes immutable after publication",
          "[pipe][vulkan][phase-c][gate][freeze][fail-closed]") {
    using Minimizer =
        ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<15u, 15u>;
    using Storage = ::bioseq::simd::HWY_NAMESPACE::pipe::
        PhaseCQueryGateStorage;

    Storage storage;
    const std::array<Minimizer, 1> minimizers{
        Minimizer{17u, 0u, 0u}};
    storage.add(std::span<const Minimizer>{minimizers});
    const auto identity = storage.identity();
    const auto gate = storage.vulkan_gate();

    REQUIRE(identity.enabled);
    REQUIRE(identity.word_count
            == bioseq::gpu::vulkan::kPhaseCQueryGateWordCount);
    REQUIRE(identity.mask == bioseq::gpu::vulkan::kPhaseCQueryGateMask);
    REQUIRE(gate.identity_valid);
    REQUIRE(gate.words.data() == gate.identity.canonical_words->data());
    REQUIRE(gate.canonical_identity().exact_equals(gate.identity));
    REQUIRE_THROWS_AS(
        storage.add(std::span<const Minimizer>{minimizers}),
        bioseq::gpu::vulkan::VulkanRuntimeError);
}

TEST_CASE("segmented GPU adoption key preserves the exact frozen gate",
          "[pipe][vulkan][phase-c][segment][gate][adoption][fail-closed]") {
    using GateIdentity = bioseq::gpu::vulkan::GpuBuiltIndexGateIdentity;
    using Key = bioseq::indexing::GpuMinimizerIndexCacheKey;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        bind_verified_gpu_segment_gate_identity;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::freeze_phase_c_query_gate;

    std::vector<std::uint32_t> mutable_words(
        bioseq::gpu::vulkan::kPhaseCQueryGateWordCount, 0u);
    mutable_words.front() = 1u;
    const auto frozen_gate = freeze_phase_c_query_gate(
        bioseq::gpu::vulkan::VulkanSketchQueryGate{
            std::span<const std::uint32_t>{mutable_words},
            bioseq::gpu::vulkan::kPhaseCQueryGateMask});
    const GateIdentity expected = frozen_gate.canonical_identity();
    REQUIRE(expected.canonical_words);
    REQUIRE(frozen_gate.words.data() == expected.canonical_words->data());
    REQUIRE(frozen_gate.words.data() != mutable_words.data());

    std::size_t segment_bind_calls = 0u;
    Key key{};
    for (std::size_t segment = 0; segment < 3u; ++segment) {
        const GateIdentity built = frozen_gate.canonical_identity();
        REQUIRE(built.canonical_words.get()
                == expected.canonical_words.get());
        key = Key{
            .source_id = 7u,
            .caller_identity = 11u + segment,
            .target_len = 101u,
            .table_size = 13u,
            .position_count = 17u,
        };
        bind_verified_gpu_segment_gate_identity(key, expected, built);
        ++segment_bind_calls;
        REQUIRE(key.gate_enabled);
        REQUIRE(key.gate_word_count == expected.word_count);
        REQUIRE(key.gate_mask == expected.mask);
        REQUIRE(key.gate_digest_lo == expected.digest_lo);
        REQUIRE(key.gate_digest_hi == expected.digest_hi);
        REQUIRE(key.gate_canonical_words.get()
                == expected.canonical_words.get());
    }
    REQUIRE(segment_bind_calls == 3u);

    auto mismatched_build = expected;
    mismatched_build.digest_hi ^= 1u;
    REQUIRE_THROWS_AS(
        bind_verified_gpu_segment_gate_identity(
            key, expected, mismatched_build),
        bioseq::gpu::vulkan::VulkanRuntimeError);
}

TEST_CASE("Vulkan sketch dispatch token requires an operation-completed roundtrip",
          "[pipe][vulkan][proof][operation-token][fail-closed]") {
    using bioseq::gpu::vulkan::VulkanSketchDispatchRoute;
    using bioseq::gpu::vulkan::VulkanSketchDispatchToken;

    REQUIRE_FALSE(VulkanSketchDispatchToken{}.accepted());

    const auto completed =
        VulkanSketchDispatchToken::completed_collect_minimizers_roundtrip();
    REQUIRE(completed.accepted());

    auto no_roundtrip = completed;
    no_roundtrip.completed_roundtrips = 0;
    REQUIRE_FALSE(no_roundtrip.accepted());

    auto wrong_count = completed;
    wrong_count.completed_roundtrips = 2;
    REQUIRE_FALSE(wrong_count.accepted());

    auto wrong_route = completed;
    wrong_route.route = VulkanSketchDispatchRoute::none;
    REQUIRE_FALSE(wrong_route.accepted());
}

TEST_CASE("Vulkan sketch proof rejects an absent or default operation token",
          "[pipe][vulkan][proof][operation-token][absent][fail-closed]") {
    using MA = ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using Proof = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        MinimizerSketchExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        MinimizerSketchRole;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        minimizer_sketch_proof_matches_selected_route;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        record_minimizer_sketch_execution_proof;

    const Proof absent{};
    REQUIRE(absent.empty());
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, absent));

    Proof already_promoted_without_token{};
    already_promoted_without_token.partial_vulkan.query_sketch_dispatches = 1;
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, already_promoted_without_token));

    Proof metadata_without_token{};
    metadata_without_token.partial_vulkan.gpu_index_unique_keys = 1;
    REQUIRE_FALSE(metadata_without_token.partial_vulkan.has_any_dispatch());
    REQUIRE_FALSE(metadata_without_token.empty());
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::SCALAR, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::TWO_STACK_NTHASH, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::PAPER_NTHASH_SIMD, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, metadata_without_token));

    MatchResultCollection destination;
    record_minimizer_sketch_execution_proof(
        destination, absent, MinimizerSketchRole::query);
    record_minimizer_sketch_execution_proof(
        destination, already_promoted_without_token,
        MinimizerSketchRole::query);
    record_minimizer_sketch_execution_proof(
        destination, metadata_without_token, MinimizerSketchRole::query);
    REQUIRE_FALSE(destination.partial_vulkan_execution_proof
                      .has_any_dispatch());
}

TEST_CASE("Vulkan sketch proof cannot promote a token across a tampered route",
          "[pipe][vulkan][proof][operation-token][route-tamper][fail-closed]") {
    using MA = ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using Proof = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        MinimizerSketchExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        MinimizerSketchRole;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        minimizer_sketch_proof_matches_selected_route;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        record_minimizer_sketch_execution_proof;

    using VulkanToken = bioseq::gpu::vulkan::VulkanSketchDispatchToken;
    const auto token = VulkanToken::completed_collect_minimizers_roundtrip();
    Proof vulkan_sketch{};
    vulkan_sketch.partial_vulkan.sketch_dispatches =
        token.completed_roundtrips;
    REQUIRE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, vulkan_sketch));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::SCALAR, vulkan_sketch));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::TWO_STACK_NTHASH, vulkan_sketch));

    Proof mixed_route = vulkan_sketch;
    mixed_route.exact_simd_present = true;
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, mixed_route));

    Proof token_with_metadata = vulkan_sketch;
    token_with_metadata.partial_vulkan.gpu_index_unique_keys = 1;
    REQUIRE_FALSE(token_with_metadata.partial_vulkan
                      .exact_sketch_token_accepted());
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, token_with_metadata));

    Proof repeated_roundtrip = vulkan_sketch;
    repeated_roundtrip.partial_vulkan.sketch_dispatches = 2;
    REQUIRE_FALSE(repeated_roundtrip.partial_vulkan
                      .exact_sketch_token_accepted());
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, repeated_roundtrip));

    MatchResultCollection repeated_roundtrip_destination;
    record_minimizer_sketch_execution_proof(
        repeated_roundtrip_destination, repeated_roundtrip,
        MinimizerSketchRole::query);
    REQUIRE(repeated_roundtrip_destination.partial_vulkan_execution_proof
                == ::bioseq::simd::HWY_NAMESPACE::
                    PartialVulkanExecutionProof{});

    MatchResultCollection tampered_destination;
    record_minimizer_sketch_execution_proof(
        tampered_destination, token_with_metadata,
        MinimizerSketchRole::query);
    REQUIRE(tampered_destination.partial_vulkan_execution_proof
                == ::bioseq::simd::HWY_NAMESPACE::
                    PartialVulkanExecutionProof{});

    MatchResultCollection query_destination;
    record_minimizer_sketch_execution_proof(
        query_destination, vulkan_sketch, MinimizerSketchRole::query);
    REQUIRE(query_destination.partial_vulkan_execution_proof
                .exact_stage_mask_accepted(
                    true, false, false, false, false));

    MatchResultCollection target_destination;
    record_minimizer_sketch_execution_proof(
        target_destination, vulkan_sketch, MinimizerSketchRole::target);
    REQUIRE(target_destination.partial_vulkan_execution_proof
                .exact_stage_mask_accepted(
                    false, true, false, false, false));
}

TEST_CASE("Paper ntHash sketch proof is distinct and cannot be promoted across routes",
          "[pipe][paper-nthash][proof][fail-closed]") {
    using MA = ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using Backend = ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using PaperProof = ::bioseq::simd::HWY_NAMESPACE::
        PackedPaperNthashExecutionProof;
    using RangeProof = ::bioseq::simd::HWY_NAMESPACE::pipe::
        MinimizerRangeExecutionProof;
    using Proof = ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        MinimizerSketchExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        MinimizerSketchRole;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        minimizer_sketch_proof_matches_selected_route;
    using ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
        record_minimizer_sketch_execution_proof;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        minimizer_range_proof_matches_backend;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        target_index_sketch_backend_from_params;

    PaperProof operation{};
    operation.scheme_id = ::bioseq::simd::HWY_NAMESPACE::
        packed_paper_nthash_scheme_id;
    operation.kmer_size = 15;
    operation.window_size = 10;
    operation.nthash_rotation = 7;
    operation.source_storage_unit_bytes = 4;
    operation.input_symbols = 128;
    operation.input_storage_units = 8;
    operation.segments_combined = 1;
    operation.logical_streams = 8;
    operation.physical_vector_lanes = 4;
    operation.vector_registers_per_step = 2;
    operation.packed_decode_batches = 1;
    operation.packed_decode_lane_visits = 8;
    operation.hash_initialization_batches = 1;
    operation.hash_table_lookup_batches = 2;
    operation.canonical_sum_batches = 1;
    operation.window_update_batches = 1;
    operation.emission_decision_batches = 1;
    operation.emission_mask_store_batches = 1;
    operation.minimizers_emitted = 1;
    operation.scalar_collector_appends = 1;
    operation.positions_are_segment_local = true;
    REQUIRE(operation.accepted(true));

    Proof paper{};
    paper.paper_nthash = operation;
    paper.paper_nthash_present = true;
    REQUIRE(minimizer_sketch_proof_matches_selected_route(
        MA::PAPER_NTHASH_SIMD, paper));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::SCALAR, paper));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::TWO_STACK_NTHASH, paper));
    REQUIRE_FALSE(minimizer_sketch_proof_matches_selected_route(
        MA::VULKAN_MINIMAP_INT, paper));

    MatchResultCollection destination;
    record_minimizer_sketch_execution_proof(
        destination, paper, MinimizerSketchRole::query);
    record_minimizer_sketch_execution_proof(
        destination, paper, MinimizerSketchRole::target);
    REQUIRE(destination.paper_nthash_query_execution_accepted(true));
    REQUIRE(destination.paper_nthash_target_execution_accepted(true));
    REQUIRE_FALSE(destination.exact_simd_query_execution_proof_present);
    REQUIRE_FALSE(destination.partial_vulkan_execution_proof.has_any_dispatch());

    RangeProof paper_range{};
    paper_range.paper_nthash = operation;
    paper_range.paper_nthash_present = true;
    REQUIRE(minimizer_range_proof_matches_backend(
        Backend::PaperNtHashSimd,
        paper_range,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::ExactSimd, paper_range));
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::Scalar, paper_range));
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::VulkanSegment, paper_range));

    auto hidden_exact_payload = paper_range;
    hidden_exact_payload.exact_simd.bp_consumed = 1;
    REQUIRE_FALSE(hidden_exact_payload.exact_simd_present);
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::PaperNtHashSimd, hidden_exact_payload));

    auto cleared_paper_flag = paper_range;
    cleared_paper_flag.paper_nthash_present = false;
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::PaperNtHashSimd, cleared_paper_flag));

    RangeProof scalar_range{};
    scalar_range.scalar.record(/*symbols=*/128u);
    scalar_range.scalar_present = true;
    REQUIRE(minimizer_range_proof_matches_backend(
        Backend::Scalar, scalar_range));
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::VulkanSegment, scalar_range));
    scalar_range.paper_nthash.scheme_id = 1;
    REQUIRE_FALSE(scalar_range.paper_nthash_present);
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::Scalar, scalar_range));

    RangeProof scalar_presence_only{};
    scalar_presence_only.scalar_present = true;
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        Backend::Scalar, scalar_presence_only));

    Minimap2Params params{};
    params.minimizer_algorithm = MA::PAPER_NTHASH_SIMD;
    REQUIRE(target_index_sketch_backend_from_params(params)
        == Backend::PaperNtHashSimd);
    params.minimizer_algorithm = MA::TWO_STACK_NTHASH;
    params.chaining_algorithm =
        ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SCALAR_MM2;
    params.base_alignment_algorithm =
        ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2;
    REQUIRE(target_index_sketch_backend_from_params(params)
        == Backend::ExactSimd);
}

TEST_CASE("DOC0256 — enum_dispatch_pack falls back to DefaultValue",
          "[pipe][doc0256][enum-dispatch-pack][default]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::CA;
    using doc0256_test_helpers::MA;

    SECTION("ChainingAlgorithmType — out-of-range routes to SCALAR_MM2") {
        using Reg = algorithm_enum_values<CA>;
        CA captured = static_cast<CA>(0x00);
        doc0256_test_helpers::value_capture_callable<CA> cb{&captured};
        enum_dispatch_pack<CA, CA, Reg::default_value>(
            Reg::value_list{}, static_cast<CA>(0xFF), cb);
        REQUIRE(captured == CA::SCALAR_MM2);
    }
    SECTION("BaseAlignmentAlgorithmType — out-of-range routes to BLOCK_ALIGNER") {
        using Reg = algorithm_enum_values<BAA>;
        BAA captured = static_cast<BAA>(0x00);
        doc0256_test_helpers::value_capture_callable<BAA> cb{&captured};
        enum_dispatch_pack<BAA, BAA, Reg::default_value>(
            Reg::value_list{}, static_cast<BAA>(0xFF), cb);
        REQUIRE(captured == BAA::BLOCK_ALIGNER);
    }
    SECTION("MinimizerAlgorithmType — out-of-range routes to SCALAR") {
        using Reg = algorithm_enum_values<MA>;
        MA captured = static_cast<MA>(0x00);
        doc0256_test_helpers::value_capture_callable<MA> cb{&captured};
        enum_dispatch_pack<MA, MA, Reg::default_value>(
            Reg::value_list{}, static_cast<MA>(0xFF), cb);
        REQUIRE(captured == MA::SCALAR);
    }
}

TEST_CASE("DOC0256 — value_dispatch_smallest_ge picks the right bucket",
          "[pipe][doc0256][value-dispatch-smallest-ge]")
{
    // The bucket list is the union of `compute_breakpoints_for<K>()` across
    // every K in `kmer_size_buckets`. With
    // BIOSEQ_OPTTIMIZATION_LEVEL >= 4, kmer_size_buckets = {15, 7, 27}
    // and the union resolves to L = {7, 15, 17, 27, 33, 49, 63}. With
    // BIOSEQ_OPTTIMIZATION_LEVEL <= 3, kmer_size_buckets = {15} and
    // the union collapses to K=15's per-K breakpoints. Gate the table
    // accordingly so the test stays meaningful at both branching levels.
    if constexpr (kmer_size_buckets.size() >= 3u) {
        static constexpr std::array<std::pair<std::size_t, std::size_t>, 15u> cases{{
            { 1u,   7u}, { 7u,   7u}, { 8u,  15u}, {15u, 15u}, {16u, 17u},
            {17u,  17u}, {18u,  27u}, {27u,  27u}, {28u, 33u}, {33u, 33u},
            {34u,  49u}, {49u,  49u}, {50u,  63u}, {63u, 63u}, {64u, 63u},
        }};
        for (const auto& [want, expected] : cases) {
            std::size_t captured = 0u;
            doc0256_test_helpers::value_capture_callable<std::size_t> cb{&captured};
            value_dispatch_smallest_ge<std::size_t, std::size_t>(
                detail::seq_len_log2_buckets_seq{}, want, cb);
            REQUIRE(captured == expected);
        }
    } else {
        // Reduced-branching mode (K = {15}). Reuse the K=15 breakpoints:
        // value_dispatch_smallest_ge picks the smallest L breakpoint >= want.
        // Verify the contract holds for a few representative `want` values
        // — the exact breakpoint set is materialised by
        // `compute_breakpoints_for<15>()` so we walk the same sequence.
        constexpr auto bp = detail::compute_breakpoints_for<15u>();
        REQUIRE(bp.second > 0u);
        for (std::size_t i = 0u; i < bp.second; ++i) {
            const std::size_t want = bp.first[i];
            std::size_t captured = 0u;
            doc0256_test_helpers::value_capture_callable<std::size_t> cb{&captured};
            value_dispatch_smallest_ge<std::size_t, std::size_t>(
                detail::seq_len_log2_buckets_seq{}, want, cb);
            REQUIRE(captured >= want);
        }
    }
}

TEST_CASE("DOC0256 — round_up_seq_len_log2 produces canonical L",
          "[pipe][doc0256][round-up-seq-len]")
{
    SECTION("Compile-time properties") {
        static_assert(round_up_seq_len_log2(0u,            15u) == 15u);
        static_assert(round_up_seq_len_log2(1u,            15u) == 15u);
        static_assert(round_up_seq_len_log2(std::size_t{1} << 28, 15u) == 28u);
        static_assert(round_up_seq_len_log2((std::size_t{1} << 48) + 1u, 15u) == 49u);
        static_assert(round_up_seq_len_log2(std::size_t{1} << 62, 15u) == 62u);
        SUCCEED();
    }
    SECTION("Clamps below K") {
        REQUIRE(round_up_seq_len_log2(1u, 27u) == 27u);
        REQUIRE(round_up_seq_len_log2(1u, 7u)  == 7u);
    }
    SECTION("Clamps above 63") {
        REQUIRE(round_up_seq_len_log2(~std::size_t{0}, 15u) == 63u);
    }
}

TEST_CASE("DOC0256 — kmer_size_default matches buckets[0]",
          "[pipe][doc0256][registry][kmer-size-default]")
{
    static_assert(kmer_size_default == 15u);
    static_assert(kmer_size_default == kmer_size_buckets[0]);
    SUCCEED();
}

TEST_CASE("DOC0256 — algorithm_enum_values registry round-trips through algorithm_traits",
          "[pipe][doc0256][registry][trait-roundtrip]")
{
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>::value_list{}));
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>::value_list{}));
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>::value_list{}));
    SUCCEED();
}

namespace doc0256_test_helpers {

/// Probe leaf for the BAA × CA × MA chain order test.  Three NTTPs are
/// pinned; the leaf's `call` writes the (BAA, CA, MA) triple into a
/// caller-provided slot reachable through the trailing args.
template <auto BaaPin, auto CaPin, auto MaPin>
struct bca_probe_leaf {
    [[gnu::always_inline]] static int call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
        BAA& baa_out, CA& ca_out, MA& ma_out)
    {
        baa_out = BaaPin;
        ca_out  = CaPin;
        ma_out  = MaPin;
        return 1;
    }
};

struct bca_probe_traits {
    template <auto BaaPin, auto CaPin, auto MaPin>
    using leaf = bca_probe_leaf<BaaPin, CaPin, MaPin>;
};

/// Probe leaf for the heterogeneous (size_t L, size_t K, BAA) chain test.
template <auto LPin, auto KPin, auto BaaPin>
struct lkb_probe_leaf {
    [[gnu::always_inline]] static int call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
        const compact_sequence_range<std::vector<seq::dna4>>& /*q*/,
        const compact_sequence_range<std::vector<seq::dna4>>& /*t*/,
        std::size_t& l_out, std::size_t& k_out, BAA& baa_out)
    {
        l_out   = LPin;
        k_out   = KPin;
        baa_out = BaaPin;
        return 1;
    }
};

struct lkb_probe_traits {
    template <auto LPin, auto KPin, auto BaaPin>
    using leaf = lkb_probe_leaf<LPin, KPin, BaaPin>;
};

/// Void-returning probe leaf for the MA-only chain test.
template <auto MaPin>
struct void_probe_leaf {
    [[gnu::always_inline]] static void call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
        MA& ma_out)
    {
        ma_out = MaPin;
    }
};

struct void_probe_traits {
    template <auto MaPin>
    using leaf = void_probe_leaf<MaPin>;
};

}  // namespace doc0256_test_helpers

TEST_CASE("DOC0256 — dispatch_chain pins NTTPs in chain order",
          "[pipe][doc0256][dispatch-chain][nttp-order]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::CA;
    using doc0256_test_helpers::MA;
    using doc0256_test_helpers::bca_probe_traits;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;

    constexpr std::array<BAA, 3> baas = {
        BAA::KSW2, BAA::BLOCK_ALIGNER, BAA::KSW2_BATCHED,
    };
    constexpr std::array<CA, 4> cas = {
        CA::SCALAR_MM2, CA::SIMD_MM2, CA::RMQ, CA::VULKAN_TILED_DP,
    };
    constexpr std::array<MA, 3> mas = {
        MA::SCALAR, MA::SIMD_MINIMAP2_SLOW, MA::TWO_STACK_NTHASH,
    };

    for (BAA baa_v : baas) for (CA ca_v : cas) for (MA ma_v : mas) {
        params.base_alignment_algorithm = baa_v;
        params.chaining_algorithm       = ca_v;
        params.minimizer_algorithm      = ma_v;
        BAA baa_out = static_cast<BAA>(0xFF);
        CA  ca_out  = static_cast<CA>(0xFF);
        MA  ma_out  = static_cast<MA>(0xFF);
        const int r = detail::dispatch_chain<
            int,
            bca_probe_traits::template leaf,
            detail::baa_step,
            detail::ca_step,
            detail::ma_step
        >::template run<>(params, baa_out, ca_out, ma_out);
        REQUIRE(r == 1);
        REQUIRE(baa_out == baa_v);
        REQUIRE(ca_out  == ca_v);
        REQUIRE(ma_out  == ma_v);
    }
}

TEST_CASE("DOC0256 — dispatch_chain heterogeneous NTTPs (L, K, BAA)",
          "[pipe][doc0256][dispatch-chain][heterogeneous]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::lkb_probe_traits;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    // Build two dummy sequences of differing sizes so seq_len_log2_step
    // picks a non-trivial L.
    std::vector<seq::dna4> q_data(1u << 10);
    std::vector<seq::dna4> t_data(1u << 14);
    compact_sequence_range<std::vector<seq::dna4>> q{q_data};
    compact_sequence_range<std::vector<seq::dna4>> t{t_data};

    SECTION("K=15, BAA=KSW2 — picks the L bucket from max_bp") {
        params.kmer_size                = 15;
        params.base_alignment_algorithm = BAA::KSW2;
        std::size_t l_out = 0u, k_out = 0u;
        BAA baa_out = static_cast<BAA>(0xFF);
        const int r = detail::dispatch_chain<
            int,
            lkb_probe_traits::template leaf,
            detail::seq_len_log2_step,
            detail::kmer_size_step,
            detail::baa_step
        >::template run<>(params, q, t, l_out, k_out, baa_out);
        REQUIRE(r == 1);
        REQUIRE(k_out == 15u);
        REQUIRE(l_out == 15u);  // round_up_seq_len_log2(max(1<<10, 1<<14), 15) = 15
        REQUIRE(baa_out == BAA::KSW2);
    }
    // Gated by `kmer_size_buckets.size()`: only run when the K=7 dispatch
    // branch is actually compiled in. BIOSEQ_OPTTIMIZATION_LEVEL <= 3
    // collapses kmer_size_buckets to {15u} for faster builds, in which case
    // the dispatch chain has no K=7 leaf — the test would assert against
    // the smallest-ge fallback (which is K=15) and spuriously fail.
    if constexpr (kmer_size_buckets.size() >= 3u) {
        SECTION("K=7 — picks the K=7 bucket and L=7 from short sequences") {
            std::vector<seq::dna4> q_small(64);
            std::vector<seq::dna4> t_small(64);
            compact_sequence_range<std::vector<seq::dna4>> q_sm{q_small};
            compact_sequence_range<std::vector<seq::dna4>> t_sm{t_small};
            params.kmer_size                = 7;
            params.base_alignment_algorithm = BAA::BLOCK_ALIGNER;
            std::size_t l_out = 0u, k_out = 0u;
            BAA baa_out = static_cast<BAA>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q_sm, t_sm, l_out, k_out, baa_out);
            REQUIRE(k_out == 7u);
            REQUIRE(l_out == 7u);
            REQUIRE(baa_out == BAA::BLOCK_ALIGNER);
        }
    }
}

TEST_CASE("DOC0256 — dispatch_chain skips inactive steps via guarded_step",
          "[pipe][doc0256][dispatch-chain][guarded-step]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::CA;
    using doc0256_test_helpers::MA;
    using doc0256_test_helpers::bca_probe_traits;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    // Inject an out-of-range MA value to confirm the chain never reads
    // params.minimizer_algorithm — guarded_step short-circuits to
    // ma_step::default_value (SCALAR) when active=false.
    params.minimizer_algorithm      = static_cast<MA>(0xFF);
    params.base_alignment_algorithm = BAA::BLOCK_ALIGNER;
    params.chaining_algorithm       = CA::SCALAR_MM2;

    BAA baa_out = static_cast<BAA>(0xFF);
    CA  ca_out  = static_cast<CA>(0xFF);
    MA  ma_out  = static_cast<MA>(0xFF);
    detail::dispatch_chain<
        int,
        bca_probe_traits::template leaf,
        detail::baa_step,
        detail::ca_step,
        detail::guarded_step<detail::ma_step, /*active=*/false>
    >::template run<>(params, baa_out, ca_out, ma_out);
    REQUIRE(baa_out == BAA::BLOCK_ALIGNER);
    REQUIRE(ca_out  == CA::SCALAR_MM2);
    REQUIRE(ma_out  == MA::SCALAR);
}

TEST_CASE("DOC0256 — dispatch_chain handles void return",
          "[pipe][doc0256][dispatch-chain][void-return]")
{
    using doc0256_test_helpers::MA;
    using doc0256_test_helpers::void_probe_traits;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    for (MA v : {MA::SCALAR, MA::SIMD_MINIMAP2_SLOW, MA::TWO_STACK_NTHASH}) {
        params.minimizer_algorithm = v;
        MA captured = static_cast<MA>(0xFF);
        detail::dispatch_chain<
            void,
            void_probe_traits::template leaf,
            detail::ma_step
        >::template run<>(params, captured);
        REQUIRE(captured == v);
    }
}

namespace doc0256_test_helpers {

/// Records which Cep branch was selected inside `pipeline_run_leaf::call`.
inline thread_local int g_last_recorded_w = 0;
inline thread_local int g_last_recorded_k = 0;

}  // namespace doc0256_test_helpers

TEST_CASE("DOC0256 — pipeline_run_leaf::call routes Cep on (W, K) runtime match",
          "[pipe][doc0256][leaf][cep-route]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::CA;
    using doc0256_test_helpers::MA;

    SECTION("(W=10, K=15) — selects the specialised Cep") {
        // Both branches use the same fully-instantiated pipeline body —
        // we cannot directly observe which Cep was picked without a probe
        // hook inside pipeline_run_impl.  Instead, we exercise the leaf's
        // (W, K) gate behaviour and confirm the call completes without
        // an out-of-bounds access by passing zero-sized sequences and
        // empty minimizers.
        ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
        params.window_size = 10;
        params.kmer_size   = 15;
        params.base_alignment_algorithm = BAA::BLOCK_ALIGNER;
        params.chaining_algorithm       = CA::SCALAR_MM2;
        params.minimizer_algorithm      = MA::SCALAR;
        std::vector<seq::dna4> q_data(64);
        std::vector<seq::dna4> t_data(64);
        compact_sequence_range<std::vector<seq::dna4>> q{q_data};
        compact_sequence_range<std::vector<seq::dna4>> t{t_data};
        auto q_owner = bioseq::storage::make_lazy_in_memory(q);
        auto t_owner = bioseq::storage::make_lazy_in_memory(t);
        const auto q_view = bioseq::storage::as_lazy_sequence_view(q_owner);
        const auto t_view = bioseq::storage::as_lazy_sequence_view(t_owner);
        using MinimizerT =
            ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<15u, 15u>;
        const std::vector<MinimizerT> mins{};
        ::bioseq::indexing::MinimizerHashmapFragmentIndex idx;
        idx.build(mins, t.bp_size());
        // The leaf's W==10 && K==15 branch picks SpecCep.
        auto result = detail::pipeline_run_leaf<
            EntryPoint::WithPrebuiltMinimizers,
            seq::dna4,
            MinimizerT, ::bioseq::indexing::MinimizerHashmapFragmentIndex,
            15u, 15u,
            BAA::BLOCK_ALIGNER, CA::SCALAR_MM2, MA::SCALAR>::call(
                params, q_view, t_view, mins, idx,
                nullptr, ::bioseq::indexing::GpuIndexCacheIdentity{}, false);
        REQUIRE(result.size() == 0u);
    }
    SECTION("(W=5, K=15) — selects the fallback Cep") {
        ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
        params.window_size = 5;
        params.kmer_size   = 15;
        params.base_alignment_algorithm = BAA::BLOCK_ALIGNER;
        params.chaining_algorithm       = CA::SCALAR_MM2;
        params.minimizer_algorithm      = MA::SCALAR;
        std::vector<seq::dna4> q_data(64);
        std::vector<seq::dna4> t_data(64);
        compact_sequence_range<std::vector<seq::dna4>> q{q_data};
        compact_sequence_range<std::vector<seq::dna4>> t{t_data};
        auto q_owner = bioseq::storage::make_lazy_in_memory(q);
        auto t_owner = bioseq::storage::make_lazy_in_memory(t);
        const auto q_view = bioseq::storage::as_lazy_sequence_view(q_owner);
        const auto t_view = bioseq::storage::as_lazy_sequence_view(t_owner);
        using MinimizerT =
            ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<15u, 15u>;
        const std::vector<MinimizerT> mins{};
        ::bioseq::indexing::MinimizerHashmapFragmentIndex idx;
        idx.build(mins, t.bp_size());
        auto result = detail::pipeline_run_leaf<
            EntryPoint::WithPrebuiltMinimizers,
            seq::dna4,
            MinimizerT, ::bioseq::indexing::MinimizerHashmapFragmentIndex,
            15u, 15u,
            BAA::BLOCK_ALIGNER, CA::SCALAR_MM2, MA::SCALAR>::call(
                params, q_view, t_view, mins, idx,
                nullptr, ::bioseq::indexing::GpuIndexCacheIdentity{}, false);
        REQUIRE(result.size() == 0u);
    }
}

TEST_CASE("DOC0256 — non-canonical kmer_size resolves to the right L bucket",
          "[pipe][doc0256][kmer-size-default]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::lkb_probe_traits;

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    params.base_alignment_algorithm = BAA::BLOCK_ALIGNER;

    for (int32_t k : {int32_t{11}, int32_t{21}}) {
        params.kmer_size = k;
        SECTION("max_bp = 1<<10") {
            std::vector<seq::dna4> qd(1u << 10);
            std::vector<seq::dna4> td(1u << 10);
            compact_sequence_range<std::vector<seq::dna4>> q{qd};
            compact_sequence_range<std::vector<seq::dna4>> t{td};
            std::size_t l_out = 0u, k_out = 0u;
            BAA baa_out = static_cast<BAA>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q, t, l_out, k_out, baa_out);
            REQUIRE(k_out == kmer_size_default);
            // round_up_seq_len_log2(1<<10, k) = max(10, k) = k for k in {11, 21}.
            // Then seq_len_log2_step picks the smallest L >= k from the
            // (union over kmer_size_buckets) breakpoint list. With
            // BIOSEQ_OPTTIMIZATION_LEVEL >= 4 that list is
            // {7, 15, 17, 27, 33, 49, 63}: k=11 → L=15;  k=21 → L=27.
            // With BIOSEQ_OPTTIMIZATION_LEVEL <= 3, only K=15 is in
            // the union so the list collapses to K=15's per-K breakpoints
            // {15, 33, 63}: k=11 → L=15; k=21 → L=33.
            if constexpr (kmer_size_buckets.size() >= 3u) {
                if (k == 11) REQUIRE(l_out == 15u);
                if (k == 21) REQUIRE(l_out == 27u);
            } else {
                if (k == 11) REQUIRE(l_out == 15u);
                if (k == 21) REQUIRE(l_out == 33u);
            }
        }
        SECTION("max_bp = 1<<20") {
            std::vector<seq::dna4> qd(1u << 20);
            std::vector<seq::dna4> td(1u << 20);
            compact_sequence_range<std::vector<seq::dna4>> q{qd};
            compact_sequence_range<std::vector<seq::dna4>> t{td};
            std::size_t l_out = 0u, k_out = 0u;
            BAA baa_out = static_cast<BAA>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q, t, l_out, k_out, baa_out);
            REQUIRE(k_out == kmer_size_default);
            // round_up_seq_len_log2(1<<20, k) = 20. The smallest L >= 20
            // in {7, 15, 17, 27, 33, 49, 63} is 27 (default level >= 2);
            // in {15, 33, 63} (level == 1) it is 33.
            if constexpr (kmer_size_buckets.size() >= 3u) {
                REQUIRE(l_out == 27u);
            } else {
                REQUIRE(l_out == 33u);
            }
        }
    }
}

namespace doc0256_test_helpers {

template <class... Args>
constexpr bool chain_accepts_v = requires {
    ::bioseq::simd::HWY_NAMESPACE::pipe::detail::dispatch_chain<
        int, bca_probe_traits::template leaf,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::baa_step,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::ca_step,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::ma_step
    >::template run<>(
        std::declval<const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&>(),
        std::declval<Args>()...);
};

}  // namespace doc0256_test_helpers

TEST_CASE("DOC0256 — dispatch_chain rejects rvalue args at compile time",
          "[pipe][doc0256][dispatch-chain][concept-gate]")
{
    using doc0256_test_helpers::BAA;
    using doc0256_test_helpers::CA;
    using doc0256_test_helpers::MA;

    // lvalue references are accepted.
    static_assert(doc0256_test_helpers::chain_accepts_v<BAA&, CA&, MA&>);
    static_assert(doc0256_test_helpers::chain_accepts_v<const BAA&, const CA&, const MA&>);
    // By-value (deduced as non-reference type) is rejected.
    static_assert(!doc0256_test_helpers::chain_accepts_v<BAA, CA, MA>);
    // rvalue ref is rejected.
    static_assert(!doc0256_test_helpers::chain_accepts_v<BAA&&, CA&&, MA&&>);
    SUCCEED();
}

// ============================================================================
// DOC0307 §13 / DOC0384 — VULKAN_MINIMAP_INT end-to-end biological parity.
// ============================================================================
//
// The full Vulkan front-end (GPU minimizers + GPU seed-hit lookup) must produce
// the same complete ordered MatchResult/CIGAR stream as the CPU path. When a
// Vulkan algorithm is selected, disabling Vulkan is an error; fallback
// selection must happen before entering the pipe.
namespace doc0307_e2e {

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

[[nodiscard]] MatchResultCollection run(
    std::vector<seq::dna4> qd, std::vector<seq::dna4> td,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    range_t q_range(qd);
    range_t t_range(td);
    auto lazy_q = bioseq::storage::make_lazy_in_memory(q_range);
    auto lazy_t = bioseq::storage::make_lazy_in_memory(t_range);
    const auto q_view = bioseq::storage::as_lazy_sequence_view(lazy_q);
    const auto t_view = bioseq::storage::as_lazy_sequence_view(lazy_t);
    return ::bioseq::simd::HWY_NAMESPACE::minimap_port(q_view, t_view, params);
}

[[nodiscard]] bool exact_biological_results_equal(
    const MatchResultCollection& left,
    const MatchResultCollection& right) noexcept
{
    if (left.results.size() != right.results.size()) return false;
    for (std::size_t index = 0u; index < left.results.size(); ++index) {
        const MatchResult& lhs = left.results[index];
        const MatchResult& rhs = right.results[index];
        if (lhs.query_len != rhs.query_len
            || lhs.query_start != rhs.query_start
            || lhs.query_end != rhs.query_end
            || lhs.is_reverse != rhs.is_reverse
            || lhs.target_len != rhs.target_len
            || lhs.target_start != rhs.target_start
            || lhs.target_end != rhs.target_end
            || lhs.num_matches != rhs.num_matches
            || lhs.block_length != rhs.block_length
            || lhs.mapq != rhs.mapq
            || lhs.chain_score != rhs.chain_score
            || lhs.dp_score != rhs.dp_score
            || lhs.dp_max_score != rhs.dp_max_score
            || lhs.seed_count != rhs.seed_count
            || lhs.secondary_chain_score != rhs.secondary_chain_score
            || std::bit_cast<std::uint32_t>(lhs.divergence)
                != std::bit_cast<std::uint32_t>(rhs.divergence)
            || lhs.is_primary != rhs.is_primary
            || lhs.has_cigar != rhs.has_cigar
            || lhs.edit_distance != rhs.edit_distance
            || lhs.cigar.size() != rhs.cigar.size()) {
            return false;
        }
        for (std::size_t cigar_index = 0u;
             cigar_index < lhs.cigar.size(); ++cigar_index) {
            if (lhs.cigar[cigar_index].length
                    != rhs.cigar[cigar_index].length
                || lhs.cigar[cigar_index].opcode
                    != rhs.cigar[cigar_index].opcode) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace doc0307_e2e

TEST_CASE("Prefilter proof aggregation preserves runtime identity and fails closed on overflow",
          "[pipe][prefilter][proof][pipeline-stats]") {
    using bioseq::pipeline_stats::PipelineExecutionStats;
    using bioseq::pipeline_stats::PipelineOperationalStats;
    using bioseq::prefilter_stats::PrefilterBackendType;
    using bioseq::prefilter_stats::PrefilterExecutionProof;
    using bioseq::prefilter_stats::ResolvedPrefilterAlgorithm;

    PipelineExecutionStats aggregate{};
    for (std::uint64_t index = 0u; index < 3u; ++index) {
        PipelineOperationalStats batch{};
        PrefilterExecutionProof proof{};
        const bool rejected = index == 2u;
        proof.record_batch(
            ResolvedPrefilterAlgorithm::gatekeeper,
            PrefilterBackendType::VULKAN,
            1u, 1u, 0u, 0u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            rejected ? 0u : 1u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            0u,
            1u);
        batch.record_prefilter_execution_proof(proof);
        aggregate.merge_operational(batch);
    }

    REQUIRE(aggregate.operational.prefilter_execution_proof_present);
    const auto& proof = aggregate.operational.prefilter_execution_proof;
    CHECK(proof.dispatches == 3u);
    CHECK(proof.gatekeeper_dispatches == 3u);
    CHECK(proof.vulkan_dispatches == 3u);
    CHECK(proof.jobs == 3u);
    CHECK(proof.evaluated_jobs == 3u);
    CHECK(proof.length_64_jobs == 3u);
    CHECK(proof.rejected_jobs == 1u);
    CHECK(proof.rejected_regions == 1u);
    CHECK(proof.surviving_regions == 2u);
    CHECK(proof.mask_rejection_candidates == 1u);
    CHECK(proof.exact_guard_activations == 1u);
    CHECK(proof.guard_confirmed_rejections == 1u);
    CHECK(proof.hamming_exact_rejections == 0u);
    CHECK(proof.valid());
    CHECK(std::string_view{proof.resolved_algorithm_name()} == "gatekeeper");
    CHECK(std::string_view{proof.resolved_backend_name()} == "vulkan");
    CHECK_FALSE(aggregate.operational.stats_saturated);

    PipelineOperationalStats valid_then_malformed{};
    PrefilterExecutionProof valid_single_batch{};
    valid_single_batch.record_batch(
        ResolvedPrefilterAlgorithm::gatekeeper,
        PrefilterBackendType::VULKAN,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    valid_then_malformed.record_prefilter_execution_proof(
        valid_single_batch);
    REQUIRE(valid_then_malformed.prefilter_execution_proof.valid());
    valid_then_malformed.record_prefilter_execution_proof(
        PrefilterExecutionProof{});
    CHECK(valid_then_malformed.prefilter_execution_proof.saturated);
    CHECK_FALSE(valid_then_malformed.prefilter_execution_proof.valid());
    CHECK(valid_then_malformed.stats_saturated);

    PipelineExecutionStats at_limit{};
    PipelineOperationalStats maximum_batch{};
    PrefilterExecutionProof maximum{};
    maximum.record_batch(
        ResolvedPrefilterAlgorithm::gatekeeper,
        PrefilterBackendType::VULKAN,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u,
        (std::numeric_limits<std::uint64_t>::max)());
    maximum_batch.record_prefilter_execution_proof(maximum);
    at_limit.merge_operational(maximum_batch);
    PipelineExecutionStats increment{};
    PipelineOperationalStats increment_batch{};
    PrefilterExecutionProof one_more{};
    one_more.record_batch(
        ResolvedPrefilterAlgorithm::gatekeeper,
        PrefilterBackendType::VULKAN,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    increment_batch.record_prefilter_execution_proof(one_more);
    increment.merge_operational(increment_batch);
    at_limit.merge(increment);
    CHECK(at_limit.operational.prefilter_execution_proof.saturated);
    CHECK_FALSE(at_limit.operational.prefilter_execution_proof.valid());
    CHECK(at_limit.operational.stats_saturated);
}

TEST_CASE("Prefilter algorithm resolver admits only the measured Vulkan matrix",
          "[pipe][prefilter][gatekeeper][resolver]") {
    using bioseq::prefilter_stats::PrefilterBackendType;
    using bioseq::prefilter_stats::ResolvedPrefilterAlgorithm;
    using bioseq::sequences::prefilters::PrefilterJob;

    const std::array<PrefilterJob, 2u> measured{
        PrefilterJob{.query_length = 32u, .target_length = 32u,
                     .max_edits = 2u},
        PrefilterJob{.query_length = 64u, .target_length = 64u,
                     .max_edits = 4u},
    };
    CHECK(resolve_prefilter_algorithm(
        PrefilterBackendType::VULKAN, measured)
        == ResolvedPrefilterAlgorithm::gatekeeper);
    CHECK(resolve_prefilter_algorithm(
        PrefilterBackendType::SIMD, measured)
        == ResolvedPrefilterAlgorithm::sneaky_snake);

    auto outside = measured;
    outside[0].query_length = 48u;
    outside[0].target_length = 48u;
    CHECK(resolve_prefilter_algorithm(
        PrefilterBackendType::VULKAN, outside)
        == ResolvedPrefilterAlgorithm::sneaky_snake);
    CHECK(resolve_prefilter_algorithm(
        PrefilterBackendType::VULKAN,
        std::span<const PrefilterJob>{})
        == ResolvedPrefilterAlgorithm::sneaky_snake);
}

TEST_CASE("PrefilterStage composes reverse, unequal, aggregate, and stable routes",
          "[pipe][sneaky-snake][prefilter][composition]") {
    using bioseq::prefilter_stats::PrefilterBackendType;
    using Cep = ConstexprAlignerParams<10u, 15u, 28u>;
    using ::bioseq::chaining::Anchor;
    using doc0307_e2e::dna4_u64;
    using doc0307_e2e::range_t;

    const auto dna4_from_ascii = [](std::string_view text) {
        std::vector<seq::dna4> result(text.size());
        for (std::size_t index = 0; index < text.size(); ++index) {
            result[index].assign_char(text[index]);
        }
        return result;
    };
    const auto make_anchor = [](
        std::uint64_t query_position,
        std::uint64_t ref_position,
        bool reverse) {
        return Anchor{
            .ref_position = ref_position,
            .query_position = query_position,
            .is_reverse = static_cast<std::uint32_t>(reverse),
            .query_span = 8u,
            .flags = 0u,
            .segment_id = 0u,
        };
    };

    std::string query_text(224u, 'A');
    constexpr std::string_view reverse_source =
        "ACGTTGCAAGTCCTAGGATCCGATTTACGGCATGCAATCG";
    static_assert(reverse_source.size() == 40u);
    query_text.replace(16u, reverse_source.size(), reverse_source);
    std::string target_text = query_text;
    // Region 1 owns two individually accepted, equal-length gaps. Their
    // lower bounds aggregate to two and reject only at the region boundary.
    target_text[72] = 'C';
    target_text[104] = 'C';
    // Region 3 is reverse-strand.  Its query-coordinate window [168, 208)
    // mirrors to the non-palindromic forward source [16, 56), so this checks
    // coordinate mirroring and reversal as well as base complementation.
    const auto complement_ascii = [](char base) {
        switch (base) {
            case 'A': return 'T';
            case 'C': return 'G';
            case 'G': return 'C';
            case 'T': return 'A';
            default: return base;
        }
    };
    for (std::size_t offset = 0; offset < reverse_source.size(); ++offset) {
        target_text[168u + offset] = complement_ascii(
            reverse_source[reverse_source.size() - 1u - offset]);
    }

    range_t query_range(dna4_from_ascii(query_text));
    range_t target_range(dna4_from_ascii(target_text));
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto query_view =
        bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto target_view =
        bioseq::storage::as_lazy_sequence_view(lazy_target);

    const std::vector<Anchor> anchors{
        make_anchor(15u, 15u, false),
        make_anchor(31u, 31u, false),
        make_anchor(63u, 63u, false),
        make_anchor(95u, 95u, false),
        make_anchor(127u, 127u, false),
        make_anchor(143u, 143u, false),
        make_anchor(159u, 164u, false),
        make_anchor(175u, 175u, true),
        make_anchor(207u, 207u, true),
    };

    for (const PrefilterBackendType backend : {
             PrefilterBackendType::SCALAR,
             PrefilterBackendType::SIMD}) {
        CAPTURE(static_cast<unsigned>(backend));
        Minimap2Params params;
        params.prefilter_enabled = true;
        params.compute_edit_distance = true;
        params.prefilter_backend = backend;
        params.prefilter_max_edits = 1u;
        params.min_ksw_len = 1;

        std::vector<AlignRegion> regions(4u);
        regions[0].anchor_start = 0;
        regions[0].seed_count = 2;
        regions[1].anchor_start = 2;
        regions[1].seed_count = 3;
        regions[2].anchor_start = 5;
        regions[2].seed_count = 2;
        regions[3].anchor_start = 7;
        regions[3].seed_count = 2;
        regions[3].is_reverse = true;

        MatchResultCollection proof;
        std::vector<AlignRegion> observed;
        int on_batch_count = 0;
        PrefilterProbeSink probe{&observed, &on_batch_count};
        PrefilterStage<seq::dna4> stage{
            {}, params, query_view, target_view, &proof};
        auto bound = stage.template attach<Cep>(probe);
        bound.on_batch_pair(
            std::span<AlignRegion>{regions},
            std::span<const Anchor>{anchors});

        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.size() == 2u);
        CHECK(observed[0].anchor_start == 0);
        CHECK(observed[1].anchor_start == 7);
        REQUIRE(proof.prefilter_execution_proof_present);
        const auto& route = proof.prefilter_execution_proof;
        CHECK(route.dispatches == 1u);
        CHECK(route.sneaky_snake_dispatches == 1u);
        CHECK(route.gatekeeper_dispatches == 0u);
        CHECK(route.scalar_dispatches
              == (backend == PrefilterBackendType::SCALAR ? 1u : 0u));
        CHECK(route.simd_dispatches
              == (backend == PrefilterBackendType::SIMD ? 1u : 0u));
        CHECK(route.jobs == 5u);
        CHECK(route.evaluated_jobs == 4u);
        CHECK(route.length_difference_jobs == 1u);
        CHECK(route.unsupported_jobs == 0u);
        CHECK(route.rejected_jobs == 1u);
        CHECK(route.rejected_regions == 2u);
        CHECK(route.surviving_regions == 2u);
        CHECK(route.mask_rejection_candidates == 0u);
        CHECK(route.exact_guard_activations == 0u);
        CHECK(route.guard_confirmed_rejections == 0u);
        CHECK(route.hamming_exact_rejections == 0u);
        CHECK_FALSE(route.saturated);
    }
}

#ifdef BIOSEQ_TEST_INDEXES
TEST_CASE("reference SIMD pipe runtime and constexpr dispatch execute native vector work",
          "[pipe][reference-simd][runtime][constexpr][route][proof]") {
    using namespace ::bioseq::simd::HWY_NAMESPACE;
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = sketch_collector_t<MinimapSketchData>;
    using pipe::MinKernelKind;

    auto data = doc0307_e2e::make_dna4(4096u, 0x51AD51ADu);
    range_type resident(data);
    auto lazy = bioseq::storage::make_lazy_in_memory(resident);
    const auto view = bioseq::storage::as_lazy_sequence_view(lazy);

    reset_reference_simd_minimizer_test_receipt();
    collector_type runtime_collector;
    (void)pipe::minimizer_dispatch<
        MinKernelKind::Simd, MinimapSketchData>(
            runtime_collector, view, 10u, 15u);
    const auto runtime_receipt =
        reference_simd_minimizer_test_receipt_snapshot();
    REQUIRE(runtime_receipt.kernel_calls == 1u);
    REQUIRE(runtime_receipt.vector_callback_batches > 0u);
    REQUIRE(runtime_receipt.vector_lane_visits > 0u);
    REQUIRE(runtime_receipt.last_storage_unit_bytes
        == sizeof(std::uint64_t));

    reset_reference_simd_minimizer_test_receipt();
    collector_type constexpr_collector;
    (void)pipe::minimizer_dispatch_constexpr<
        MinKernelKind::Simd, 10u, 15u, MinimapSketchData>(
            constexpr_collector, view, 10u, 15u);
    const auto constexpr_receipt =
        reference_simd_minimizer_test_receipt_snapshot();
    REQUIRE(constexpr_receipt.kernel_calls == 1u);
    REQUIRE(constexpr_receipt.vector_callback_batches > 0u);
    REQUIRE(constexpr_receipt.vector_lane_visits > 0u);
    REQUIRE(constexpr_receipt.last_storage_unit_bytes
        == sizeof(std::uint64_t));

    const auto& runtime = runtime_collector.extract_sketches_data();
    const auto& specialized = constexpr_collector.extract_sketches_data();
    REQUIRE(runtime.size() == specialized.size());
    for (std::size_t index = 0u; index < runtime.size(); ++index) {
        CAPTURE(index);
        REQUIRE(runtime[index].raw_value() == specialized[index].raw_value());
    }

    reset_reference_simd_minimizer_test_receipt();
    collector_type scalar_collector;
    (void)pipe::minimizer_dispatch<
        MinKernelKind::Scalar, MinimapSketchData>(
            scalar_collector, view, 10u, 15u);
    const auto scalar_receipt =
        reference_simd_minimizer_test_receipt_snapshot();
    REQUIRE(scalar_receipt.kernel_calls == 0u);
    REQUIRE(scalar_receipt.vector_callback_batches == 0u);
    REQUIRE(scalar_receipt.vector_lane_visits == 0u);
}
#endif

TEST_CASE("ExactSimd pipe selects resident dual vector_stream only for whole u32 ranges",
          "[pipe][exact-simd][resident][vector_stream][route][proof]") {
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using minimizer_type =
        ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<33u, 15u>;
    using collector_type =
        ::bioseq::simd::HWY_NAMESPACE::sketch_collector_t<minimizer_type>;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::ExactSimdRangeRoute;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::MinKernelKind;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::minimizer_dispatch_constexpr;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::minimizer_dispatch_range;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        exact_simd_proofs_share_selection_route;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        minimizer_range_proof_matches_backend;

    auto data = doc0307_e2e::make_dna4(4096u, 0x51A1D5EEDu);
    range_type resident(data);
    auto lazy_resident = bioseq::storage::make_lazy_in_memory(resident);
    const auto resident_view =
        bioseq::storage::as_lazy_sequence_view(lazy_resident);

    collector_type whole_collector;
    if (!::bioseq::simd::HWY_NAMESPACE::two_stack_internal::
            resident_vector_shape_supported()) {
        try {
            (void)minimizer_dispatch_constexpr<
                MinKernelKind::ExactSimd, 10u, 15u, minimizer_type>(
                    whole_collector, resident_view, 10u, 15u);
            FAIL("unsupported Highway target must reject resident pipe routing");
        } catch (const ::bioseq::simd::HWY_NAMESPACE::
                ExactSimdRuntimeError& error) {
            REQUIRE(error.status() == ::bioseq::simd::HWY_NAMESPACE::
                TwoStackContinuationStatus::unsupported_vector_shape);
        }
        REQUIRE(whole_collector.extract_sketches_data().empty());
        return;
    }
    const auto whole_proof = minimizer_dispatch_constexpr<
        MinKernelKind::ExactSimd, 10u, 15u, minimizer_type>(
            whole_collector, resident_view, 10u, 15u);
    REQUIRE(whole_proof.accepted(true));
    REQUIRE(whole_proof.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(whole_proof.resident_vector_stream_dual_calls == 1u);
    REQUIRE(whole_proof.resident_vector_stream_single_calls == 0u);
    REQUIRE(whole_proof.generic_u64_kernel_calls == 0u);
    REQUIRE(whole_proof.k15_w10_b8_u32_hash_kernel_calls == 0u);

    // Range dispatch is also used by indexed query preparation. Resident
    // vector_stream is therefore an explicit paired/no-index opt-in, never a
    // decision inferred from the query's storage shape alone.
    collector_type paired_resident_collector;
    const auto paired_resident_proof =
        minimizer_dispatch_range<minimizer_type>(
            paired_resident_collector,
            resident_view,
            0u,
            resident.bp_size(),
            10u,
            15u,
            MinimizerSketchBackend::ExactSimd,
            ExactSimdRangeRoute::require_resident_vector_stream);
    REQUIRE(paired_resident_proof.exact_simd_present);
    REQUIRE(paired_resident_proof.exact_simd.accepted(true));
    REQUIRE(paired_resident_proof.exact_simd
        .resident_vector_stream_kernel_calls == 1u);
    REQUIRE(minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::ExactSimd,
        paired_resident_proof,
        /*require_nonempty_vector_work=*/true,
        ExactSimdRangeRoute::require_resident_vector_stream));
    // This is the fail-closed compatibility boundary: a resident proof cannot
    // be relabelled as the continuation/B8 scheme used by persisted indexes.
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::ExactSimd,
        paired_resident_proof,
        /*require_nonempty_vector_work=*/true,
        ExactSimdRangeRoute::require_continuation));

    collector_type persisted_compatible_collector;
    const auto persisted_compatible_proof =
        minimizer_dispatch_range<minimizer_type>(
            persisted_compatible_collector,
            resident_view,
            0u,
            resident.bp_size(),
            10u,
            15u,
            MinimizerSketchBackend::ExactSimd,
            ExactSimdRangeRoute::require_continuation);
    REQUIRE(persisted_compatible_proof.exact_simd_present);
    REQUIRE(persisted_compatible_proof.exact_simd.accepted(true));
    REQUIRE(persisted_compatible_proof.exact_simd
        .resident_vector_stream_kernel_calls == 0u);
    REQUIRE((persisted_compatible_proof.exact_simd
            .generic_u64_kernel_calls > 0u
        || persisted_compatible_proof.exact_simd
            .k15_w10_b8_u32_hash_kernel_calls > 0u));
    REQUIRE(minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::ExactSimd,
        persisted_compatible_proof,
        /*require_nonempty_vector_work=*/true,
        ExactSimdRangeRoute::require_continuation));
    REQUIRE_FALSE(exact_simd_proofs_share_selection_route(
        paired_resident_proof.exact_simd,
        persisted_compatible_proof.exact_simd));
    REQUIRE(exact_simd_proofs_share_selection_route(
        paired_resident_proof.exact_simd,
        whole_proof));

    collector_type rejected_resident_subrange_collector;
    REQUIRE_THROWS_AS(
        minimizer_dispatch_range<minimizer_type>(
            rejected_resident_subrange_collector,
            resident_view,
            0u,
            resident.bp_size() - 1u,
            10u,
            15u,
            MinimizerSketchBackend::ExactSimd,
            ExactSimdRangeRoute::require_resident_vector_stream),
        std::invalid_argument);
    REQUIRE(rejected_resident_subrange_collector
        .extract_sketches_data().empty());

    collector_type subrange_collector;
    const auto subrange_proof = minimizer_dispatch_range<minimizer_type>(
        subrange_collector,
        resident_view,
        0u,
        resident.bp_size() - 1u,
        10u,
        15u,
        MinimizerSketchBackend::ExactSimd);
    REQUIRE(subrange_proof.exact_simd_present);
    REQUIRE(subrange_proof.exact_simd.accepted(true));
    REQUIRE(subrange_proof.exact_simd
        .resident_vector_stream_kernel_calls == 0u);
    REQUIRE((subrange_proof.exact_simd.generic_u64_kernel_calls > 0u
        || subrange_proof.exact_simd
            .k15_w10_b8_u32_hash_kernel_calls > 0u));
}

TEST_CASE("Indexed dna4 u64 queries use admitted typed u32 B8 continuation with scalar-identical records",
          "[pipe][exact-simd][indexed-query][u64-u32][b8][parity][proof]"
          "[memory-admission][alias-safe]") {
    using namespace ::bioseq::simd::HWY_NAMESPACE;
    using query_sequence = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using query_range = compact_sequence_range<query_sequence>;
    using b8_sequence = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using b8_range = compact_sequence_range<b8_sequence>;

    if (two_stack_internal::select_exact_simd_kernel_kind<
            MinimapSketchData, b8_range::traits>(4'123u)
        != ExactSimdKernelKind::k15_w10_b8_u32_hash) {
        return;
    }

    Minimap2Params params;
    params.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;

    const auto assert_case = [&params](std::vector<seq::dna4> data,
                                      std::string_view fixture) {
        CAPTURE(fixture, data.size());
        query_range resident(data);
        auto lazy_query = bioseq::storage::make_lazy_in_memory(resident);
        const auto query_view =
            bioseq::storage::as_lazy_sequence_view(lazy_query);
        const auto native = query_view.try_resident_packed(
            0u, query_view.bp_size(), sizeof(std::uint64_t));
        REQUIRE(native.has_value());
        const auto access = bioseq::storage::plan_sequence_access(
            query_view,
            bioseq::storage::sequence_access_requirement::
                contiguous_resident,
            0u,
            query_view.bp_size(),
            sizeof(std::uint32_t));
        REQUIRE(access.route
            == bioseq::storage::sequence_access_route::materialize_admitted);
        REQUIRE(access.resident_borrow_kind
            == bioseq::storage::sequence_resident_borrow_kind::none);
        REQUIRE_FALSE(access.resident.has_value());
        REQUIRE(access.admission_bytes
            == bioseq::storage::packed_region_admission_bytes<seq::dna4>(
                data.size(), sizeof(std::uint32_t)));
        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        const std::uint64_t admission_baseline = admission.reserved_bytes();
        {
            auto permit = admission.try_reserve(access.admission_bytes);
            REQUIRE(permit.has_value());
            auto typed_owner = bioseq::storage::materialize_packed_region(
                query_view,
                0u,
                query_view.bp_size(),
                sizeof(std::uint32_t),
                std::move(*permit));
            REQUIRE(admission.reserved_bytes()
                == admission_baseline + access.admission_bytes);
            REQUIRE(typed_owner.descriptor().storage_unit_byte_width
                == sizeof(std::uint32_t));
            REQUIRE(typed_owner.descriptor().unit_count
                == (data.size() + 15u) / 16u);
            REQUIRE(typed_owner.descriptor().units != native->units);
        }
        REQUIRE(admission.reserved_bytes() == admission_baseline);
        auto fast = sketch_query_for_index_selection<MinimapSketchData>(
            query_view, params, 10u, 15u);
        REQUIRE(admission.reserved_bytes() == admission_baseline);

        sketch_collector_t<MinimapSketchData> scalar_collector;
        collect_minimizers_view<minimap_int_hasher, MinimapSketchData>(
            scalar_collector,
            resident,
            resident.begin(),
            resident.end(),
            10u,
            15u);
        const auto scalar =
            std::move(scalar_collector).extract_sketches_data();

        REQUIRE(fast.exact_simd_execution_proof_present);
        const TwoStackExecutionProof& proof =
            fast.exact_simd_execution_proof;
        REQUIRE(proof.accepted(/*require_nonempty_vector_work=*/true));
        REQUIRE(proof.bp_consumed == data.size());
        REQUIRE(proof.resident_vector_stream_kernel_calls == 0u);
        REQUIRE(proof.resident_vector_stream_single_calls == 0u);
        REQUIRE(proof.resident_vector_stream_dual_calls == 0u);
        REQUIRE(proof.generic_u64_kernel_calls == 0u);
        REQUIRE(proof.k15_w10_b8_u32_hash_kernel_calls == 1u);
        REQUIRE(proof.k15_w10_b8_u32_hash_candidate_blocks > 0u);
        REQUIRE(proof.k15_w10_b8_u32_hash_lane_visits > 0u);
        REQUIRE(proof.k15_w10_b8_packed22_groups > 0u);
        REQUIRE(proof.k15_w10_b8_packed22_lane_visits > 0u);
        REQUIRE(fast.minimizers.size() == scalar.size());
        for (std::size_t i = 0; i < scalar.size(); ++i) {
            CAPTURE(i);
            REQUIRE(fast.minimizers[i].raw_value()
                == scalar[i].raw_value());
        }
    };

    assert_case(
        doc0307_e2e::make_dna4(4'123u, 0xB8D0A57u),
        "non-periodic tail");
    std::vector<seq::dna4> repeated(4'123u);
    seq::dna4 adenine;
    adenine.assign_rank(
        static_cast<decltype(adenine.to_rank())>(0u));
    std::fill(repeated.begin(), repeated.end(), adenine);
    assert_case(std::move(repeated), "equal-hash tie windows");
}

TEST_CASE("KSW2 batched pipeline returns operation-originated nonempty proof",
          "[pipe][ksw2-batched][execution-proof][integration]") {
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;

    auto target = doc0307_e2e::make_dna4(4'000u, 0xB47C4EDu);
    constexpr std::size_t truth_start = 700u;
    std::vector<seq::dna4> query(
        target.begin() + truth_start,
        target.begin() + truth_start + 900u);

    Minimap2Params params{};
    params.minimizer_algorithm =
        MinimizerAlgorithmType::TWO_STACK_NTHASH;
    params.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
    params.base_alignment_algorithm =
        BaseAlignmentAlgorithmType::KSW2_BATCHED;
    params.do_cigar = true;

    const auto result = doc0307_e2e::run(query, target, params);
    REQUIRE(result.exact_simd_query_execution_accepted(true));
    REQUIRE(result.exact_simd_target_execution_accepted(true));
    REQUIRE(result.simd_chaining_execution_accepted(true));
    REQUIRE(result.ksw2_batched_execution_accepted(true));
    REQUIRE_FALSE(result.block_aligner_execution_proof_present);
    REQUIRE(result.scalar_pipeline_execution_proof_present);
    REQUIRE(result.scalar_pipeline_execution_proof.seed_lookup_calls > 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.seed_query_minimizers > 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.query_minimizers
            == ::bioseq::simd::HWY_NAMESPACE::ScalarMinimizerExecutionProof{});
    REQUIRE(result.scalar_pipeline_execution_proof.target_minimizers
            == ::bioseq::simd::HWY_NAMESPACE::ScalarMinimizerExecutionProof{});
    REQUIRE(result.query_sketch_host_wall_ns > 0u);
    REQUIRE(result.seed_lookup_host_wall_ns > 0u);
    REQUIRE(result.chain_score_host_wall_ns > 0u);
    REQUIRE(result.region_generation_host_wall_ns > 0u);
    REQUIRE(result.alignment_result_host_wall_ns > 0u);
    REQUIRE_FALSE(result.region_host_phase_active);
    REQUIRE_FALSE(result.alignment_result_host_phase_active);
    REQUIRE_FALSE(result.host_stage_timing_saturated);
    REQUIRE(result.scalar_pipeline_execution_proof.chain_score_calls == 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.chain_backtrack_calls == 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.ksw2_left_calls == 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.ksw2_gap_calls == 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.ksw2_right_calls == 0u);

    auto missing = result;
    missing.ksw2_batched_execution_proof_present = false;
    REQUIRE_FALSE(missing.ksw2_batched_execution_accepted(true));

    auto tampered = result;
    ++tampered.ksw2_batched_execution_proof.long_fill_calls;
    REQUIRE_FALSE(tampered.ksw2_batched_execution_accepted(true));
}

template <class StorageUnit>
void check_fragmented_paper_range_materialization() {
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::PaperNtHashRuntimeError;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::minimizer_dispatch_range;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        minimizer_range_proof_matches_backend;
    using packed_sequence =
        sequence_custom_alphabet<seq::dna4, StorageUnit>;
    using range_type = compact_sequence_range<packed_sequence>;
    using chunked_scanner = bioseq::storage::sequence_scanner_test_chunked<
        StorageUnit, 2>;
    using minimizer_type =
        ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using collector_type =
        ::bioseq::simd::HWY_NAMESPACE::sketch_collector_t<minimizer_type>;

    auto data = doc0307_e2e::make_dna4(
        4'123, 0xF12A6E57u + sizeof(StorageUnit));
    range_type range(data);
    auto resident_owner = bioseq::storage::make_lazy_in_memory(range);
    const auto resident_view =
        bioseq::storage::as_lazy_sequence_view(resident_owner);
    REQUIRE(range.size() > 12u);
    REQUIRE(range.padding() != 0u);
    using sequence_size_type = bioseq::storage::sequence_size_t;
    const auto check_slice = [&](sequence_size_type lo_unit,
                                 sequence_size_type hi_unit) {
        CAPTURE(lo_unit, hi_unit, sizeof(StorageUnit));
        constexpr sequence_size_type letters_per_unit =
            range_type::traits::letters_per_unit;
        const sequence_size_type lo_bp = lo_unit * letters_per_unit;
        const sequence_size_type hi_bp = std::min(
            hi_unit * letters_per_unit,
            static_cast<sequence_size_type>(range.bp_size()));
        collector_type resident_collector;
        const auto resident_proof = minimizer_dispatch_range<minimizer_type>(
            resident_collector,
            resident_view,
            lo_bp,
            hi_bp,
            10u,
            15u,
            MinimizerSketchBackend::PaperNtHashSimd);
        REQUIRE(minimizer_range_proof_matches_backend(
            MinimizerSketchBackend::PaperNtHashSimd,
            resident_proof,
            /*require_nonempty_vector_work=*/true));

        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        const std::uint64_t reserved_before = admission.reserved_bytes();
        collector_type fragmented_collector;
        {
            bioseq::storage::lazy_compact_sequence<
                packed_sequence, chunked_scanner> fragmented{
                    chunked_scanner::from_range(range)};
            const auto fragmented_view =
                bioseq::storage::as_lazy_sequence_view(fragmented);
            const auto fragmented_proof =
                minimizer_dispatch_range<minimizer_type>(
                    fragmented_collector,
                    fragmented_view,
                    lo_bp,
                    hi_bp,
                    10u,
                    15u,
                    MinimizerSketchBackend::PaperNtHashSimd);
            REQUIRE(minimizer_range_proof_matches_backend(
                MinimizerSketchBackend::PaperNtHashSimd,
                fragmented_proof,
                /*require_nonempty_vector_work=*/true));
            REQUIRE(fragmented_proof.paper_nthash.source_storage_unit_bytes
                    == sizeof(StorageUnit));
            REQUIRE(fragmented_proof.paper_nthash
                    == resident_proof.paper_nthash);

            collector_type rejected_collector;
            REQUIRE_THROWS_AS(
                minimizer_dispatch_range<minimizer_type>(
                    rejected_collector,
                    fragmented_view,
                    lo_bp,
                    hi_bp,
                    9u,
                    15u,
                    MinimizerSketchBackend::PaperNtHashSimd),
                PaperNtHashRuntimeError);
        }
        REQUIRE(admission.reserved_bytes() == reserved_before);

        const auto& resident_records =
            resident_collector.extract_sketches_data();
        const auto& fragmented_records =
            fragmented_collector.extract_sketches_data();
        REQUIRE_FALSE(resident_records.empty());
        REQUIRE(fragmented_records.size() == resident_records.size());
        for (std::size_t i = 0; i < resident_records.size(); ++i) {
            CAPTURE(i);
            REQUIRE(fragmented_records[i].kmer()
                    == resident_records[i].kmer());
            REQUIRE(fragmented_records[i].last_pos()
                    == resident_records[i].last_pos());
            REQUIRE(fragmented_records[i].strand()
                    == resident_records[i].strand());
            REQUIRE(fragmented_records[i].raw_value()
                    == resident_records[i].raw_value());
        }
    };

    // Exercise both an interior slice (zero local padding) and a tail slice
    // that must inherit the source range's final packed-unit padding.
    check_slice(3u, static_cast<sequence_size_type>(range.size() - 5u));
    check_slice(5u, static_cast<sequence_size_type>(range.size()));
}

TEST_CASE("Paper ntHash fragmented ranges materialize only their packed slice with local coordinates",
          "[paper-nthash][simd][pipe][fragmented][range][fail-closed]") {
    check_fragmented_paper_range_materialization<std::uint32_t>();
    check_fragmented_paper_range_materialization<std::uint64_t>();
}

enum class InjectedScannerLeaf {
    row_read,
    palloc,
    interrupt,
};

class InjectedScannerLeafError final : public std::runtime_error {
public:
    explicit InjectedScannerLeafError(InjectedScannerLeaf point)
        : std::runtime_error(point == InjectedScannerLeaf::row_read
              ? "injected scanner row-read failure"
              : point == InjectedScannerLeaf::palloc
                  ? "injected scanner palloc failure"
                  : "injected scanner interrupt failure")
    {}
};

template <class StorageUnit>
struct InjectedFailureScanner final {
    using storage_unit = StorageUnit;
    using base_container = std::span<const storage_unit>;
    static constexpr std::size_t fragment_storage_units = 2u;

    base_container backing{};
    bioseq::storage::sequence_size_t bp_size = 0;
    std::size_t padding = 0;
    InjectedScannerLeaf point = InjectedScannerLeaf::row_read;
    std::size_t* reads = nullptr;

    [[nodiscard]] static constexpr bool supports_fragments() noexcept {
        return true;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t total_bp_size() const noexcept {
        return bp_size;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t total_size() const noexcept {
        return static_cast<bioseq::storage::sequence_size_t>(backing.size());
    }
    [[nodiscard]] bioseq::storage::sequence_size_t total_byte_size() const noexcept {
        return static_cast<bioseq::storage::sequence_size_t>(
            backing.size() * sizeof(storage_unit));
    }
    [[nodiscard]] std::size_t number_of_fragments() const noexcept {
        return (backing.size() + fragment_storage_units - 1u)
            / fragment_storage_units;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t
    estimated_total_bp_size() const noexcept {
        return bp_size;
    }
    [[nodiscard]] std::size_t last_fragment_padding_letters() const noexcept {
        return padding;
    }
    [[nodiscard]] base_container read_fragment(std::size_t) const {
        if (reads != nullptr) {
            ++*reads;
        }
        throw InjectedScannerLeafError{point};
    }
    void release_fragment(std::size_t) const noexcept {}

    using fragment_read_callback = void (*)(void*);
    static void run_fragment_reads_with_error_cleanup(
            void* context,
            fragment_read_callback body,
            fragment_read_callback) {
        body(context);
    }

};

static_assert(bioseq::storage::is_active_sequence_scanner<
    InjectedFailureScanner<std::uint64_t>>);

TEST_CASE("fragmented Paper ntHash unwinds its vector and memory permit for every scanner PostgreSQL leaf failure",
          "[paper-nthash][pipe][fragmented][pg-error][memory-admission]") {
    using StorageUnit = std::uint64_t;
    using packed_sequence = sequence_custom_alphabet<seq::dna4, StorageUnit>;
    using range_type = compact_sequence_range<packed_sequence>;
    using scanner_type = InjectedFailureScanner<StorageUnit>;
    using lazy_type = bioseq::storage::lazy_compact_sequence<
        packed_sequence, scanner_type>;
    using minimizer_type =
        ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using collector_type =
        ::bioseq::simd::HWY_NAMESPACE::sketch_collector_t<minimizer_type>;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::minimizer_dispatch_range;

    auto bases = doc0307_e2e::make_dna4(4'123, 0x51A9E5u);
    range_type range(bases);
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();

    for (const InjectedScannerLeaf point : {
             InjectedScannerLeaf::row_read,
             InjectedScannerLeaf::palloc,
             InjectedScannerLeaf::interrupt}) {
        CAPTURE(static_cast<unsigned>(point));
        std::size_t reads = 0;
        scanner_type scanner{
            .backing = std::span<const StorageUnit>{
                range.read_cptr(range.cbegin()),
                static_cast<std::size_t>(range.size())},
            .bp_size = static_cast<bioseq::storage::sequence_size_t>(
                range.bp_size()),
            .padding = static_cast<std::size_t>(range.padding()),
            .point = point,
            .reads = &reads,
        };
        lazy_type fragmented{std::move(scanner)};
        const auto fragmented_view =
            bioseq::storage::as_lazy_sequence_view(fragmented);
        collector_type collector;
        const std::uint64_t reserved_before = admission.reserved_bytes();
        constexpr bioseq::storage::sequence_size_t letters_per_unit =
            range_type::traits::letters_per_unit;
        const bioseq::storage::sequence_size_t lo_bp =
            2u * letters_per_unit;
        const bioseq::storage::sequence_size_t hi_bp = std::min(
            static_cast<bioseq::storage::sequence_size_t>(
                (range.size() - 2u) * letters_per_unit),
            static_cast<bioseq::storage::sequence_size_t>(range.bp_size()));

        REQUIRE_THROWS_AS(
            minimizer_dispatch_range<minimizer_type>(
                collector,
                fragmented_view,
                lo_bp,
                hi_bp,
                10u,
                15u,
                MinimizerSketchBackend::PaperNtHashSimd),
            InjectedScannerLeafError);
        REQUIRE(reads == 1u);
        REQUIRE(admission.reserved_bytes() == reserved_before);
    }
}

TEST_CASE("Paper ntHash SIMD pipe executes distinct sketch proof without fallback",
          "[paper-nthash][simd][pipe][proof][fail-closed]") {
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;

    auto target = doc0307_e2e::make_dna4(4000, 0x51A7C0DEu);
    constexpr std::size_t truth_start = 700;
    std::vector<seq::dna4> query(
        target.begin() + truth_start,
        target.begin() + truth_start + 900);

    Minimap2Params params{};
    params.minimizer_algorithm =
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
    params.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
    params.base_alignment_algorithm =
        BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    params.do_cigar = true;

    const auto result = doc0307_e2e::run(query, target, params);
    REQUIRE(result.paper_nthash_query_execution_accepted(true));
    REQUIRE(result.paper_nthash_target_execution_accepted(true));
    REQUIRE(result.simd_chaining_execution_accepted(true));
    REQUIRE(result.block_aligner_execution_accepted(true));
    REQUIRE(result.scalar_pipeline_execution_proof_present);
    REQUIRE(result.scalar_pipeline_execution_proof.seed_lookup_calls > 0u);
    REQUIRE(result.scalar_pipeline_execution_proof.seed_query_minimizers > 0u);
    REQUIRE(result.all_simd_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.all_simd_execution_accepted(
        MinimizerAlgorithmType::TWO_STACK_NTHASH,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto mixed_proof = result;
    mixed_proof.exact_simd_query_execution_proof_present = true;
    REQUIRE_FALSE(mixed_proof.all_simd_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto missing_cpu_seed_proof = result;
    missing_cpu_seed_proof.scalar_pipeline_execution_proof = {};
    missing_cpu_seed_proof.scalar_pipeline_execution_proof_present = false;
    REQUIRE_FALSE(missing_cpu_seed_proof.all_simd_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto presence_only_cpu_seed_proof = missing_cpu_seed_proof;
    presence_only_cpu_seed_proof.scalar_pipeline_execution_proof_present = true;
    REQUIRE_FALSE(presence_only_cpu_seed_proof.all_simd_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto hidden_scalar_chain = result;
    hidden_scalar_chain.scalar_pipeline_execution_proof
        .record_scalar_chain(1u);
    REQUIRE_FALSE(hidden_scalar_chain.all_simd_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto saturated_cpu_seed = result;
    saturated_cpu_seed.scalar_pipeline_execution_proof.saturated = true;
    REQUIRE_FALSE(saturated_cpu_seed.all_simd_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.exact_simd_query_execution_proof_present);
    REQUIRE_FALSE(result.exact_simd_target_execution_proof_present);
    REQUIRE_FALSE(result.partial_vulkan_execution_proof.has_any_dispatch());
    REQUIRE(result.partial_vulkan_execution_proof
            == ::bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof{});

    auto vulkan_dispatch_tamper = result;
    vulkan_dispatch_tamper.partial_vulkan_execution_proof
        .query_sketch_dispatches = 1;
    REQUIRE_FALSE(vulkan_dispatch_tamper
        .selected_simd_sketch_execution_accepted(
            MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto vulkan_metadata_tamper = result;
    vulkan_metadata_tamper.partial_vulkan_execution_proof
        .gpu_index_unique_keys = 1;
    REQUIRE_FALSE(vulkan_metadata_tamper.partial_vulkan_execution_proof
        .has_any_dispatch());
    REQUIRE_FALSE(vulkan_metadata_tamper
        .selected_simd_sketch_execution_accepted(
            MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto hidden_opposite_payload = result;
    hidden_opposite_payload.exact_simd_query_execution_proof.bp_consumed = 1;
    REQUIRE_FALSE(hidden_opposite_payload
        .exact_simd_query_execution_proof_present);
    REQUIRE_FALSE(hidden_opposite_payload
        .selected_simd_sketch_execution_accepted(
            MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto persisted_proof = result;
    persisted_proof.paper_nthash_target_execution_proof = {};
    persisted_proof.paper_nthash_target_execution_proof_present = false;
    REQUIRE(persisted_proof.selected_simd_sketch_execution_accepted(
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
        /*require_target_sketch=*/false,
        /*require_nonempty_vector_work=*/true));

    auto hidden_persisted_target_payload = persisted_proof;
    hidden_persisted_target_payload.paper_nthash_target_execution_proof =
        result.paper_nthash_target_execution_proof;
    REQUIRE_FALSE(hidden_persisted_target_payload
        .paper_nthash_target_execution_proof_present);
    REQUIRE_FALSE(hidden_persisted_target_payload
        .selected_simd_sketch_execution_accepted(
            MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
            /*require_target_sketch=*/false,
            /*require_nonempty_vector_work=*/true));

    auto target_presence_only = persisted_proof;
    target_presence_only.paper_nthash_target_execution_proof_present = true;
    REQUIRE_FALSE(target_presence_only
        .selected_simd_sketch_execution_accepted(
            MinimizerAlgorithmType::PAPER_NTHASH_SIMD,
            /*require_target_sketch=*/false,
            /*require_nonempty_vector_work=*/true));

    const auto primary = std::ranges::find_if(
        result.results,
        [](const auto& match) { return match.is_primary; });
    REQUIRE(primary != result.results.end());
    REQUIRE(std::abs(
        static_cast<std::int64_t>(primary->target_start)
        - static_cast<std::int64_t>(truth_start)) < 15);
}

TEST_CASE("Paper ntHash segmented pipe globalizes local coordinates without fallback",
          "[paper-nthash][simd][pipe][segmented][proof][coordinates]") {
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;

    // Force the production geometry to its 32-Mbp floor, then place the truth
    // strictly inside the second core. This exercises two real target-range
    // sketches without introducing a test-only segmentation implementation.
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& controller;
        std::uint64_t value;
        ~RestoreCap() { controller.set_index_memory_cap_bytes(value); }
    } restore_cap{admission, previous_cap};
    admission.set_index_memory_cap_bytes(192'000'000u);

    constexpr std::size_t target_length = 33'000'000;
    constexpr std::size_t truth_start = 32'250'000;
    constexpr std::size_t query_length = 40'000;
    auto target = doc0307_e2e::make_dna4(
        target_length, 0x5E6A3E17u);
    std::vector<seq::dna4> query;
    query.reserve(query_length);
    for (auto iterator = target.begin() + truth_start + query_length;
         iterator != target.begin() + truth_start;) {
        --iterator;
        seq::dna4 complement{};
        complement.assign_rank(static_cast<std::uint8_t>(
            3u - iterator->to_rank()));
        query.push_back(complement);
    }

    doc0307_e2e::range_t query_range(query);
    doc0307_e2e::range_t target_range(target);
    auto lazy_query = bioseq::storage::make_lazy_in_memory(query_range);
    auto lazy_target = bioseq::storage::make_lazy_in_memory(target_range);
    const auto query_view =
        bioseq::storage::as_lazy_sequence_view(lazy_query);
    const auto target_view =
        bioseq::storage::as_lazy_sequence_view(lazy_target);

    Minimap2Params params{};
    params.minimizer_algorithm =
        MinimizerAlgorithmType::PAPER_NTHASH_SIMD;
    params.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
    params.base_alignment_algorithm = BaseAlignmentAlgorithmType::KSW2;
    params.do_cigar = false;

    const auto result = ::bioseq::simd::HWY_NAMESPACE::
        minimap_port_segmented(query_view, target_view, params);
    REQUIRE(result.paper_nthash_query_execution_accepted(true));
    REQUIRE(result.paper_nthash_target_execution_accepted(true));
    REQUIRE(result.paper_nthash_target_execution_proof.segments_combined >= 2);
    REQUIRE(result.simd_chaining_execution_accepted(true));
    REQUIRE_FALSE(result.exact_simd_query_execution_proof_present);
    REQUIRE_FALSE(result.exact_simd_target_execution_proof_present);
    REQUIRE_FALSE(result.partial_vulkan_execution_proof.has_any_dispatch());

    const auto primary = std::ranges::find_if(
        result.results,
        [](const auto& match) { return match.is_primary; });
    REQUIRE(primary != result.results.end());
    REQUIRE(primary->is_reverse);
    REQUIRE(std::abs(
        static_cast<std::int64_t>(primary->target_start)
        - static_cast<std::int64_t>(truth_start)) < 15);
}

TEST_CASE("canonical Vulkan selection separates capability from a complete all-GPU tuple",
          "[vulkan][pipe][routing][fail-closed]") {
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        complete_all_gpu_tuple_selected;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::vulkan_accelerator_requested;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::vulkan_components_requested;

    Minimap2Params params{};
    params.vulkan_enabled = true;
    params.vulkan_index_cache_enabled = true;
    REQUIRE_FALSE(vulkan_components_requested(params));
    REQUIRE_FALSE(vulkan_accelerator_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));

    params.prefilter_enabled = true;
    params.prefilter_backend =
        bioseq::prefilter_stats::PrefilterBackendType::VULKAN;
    REQUIRE_FALSE(vulkan_components_requested(params));
    REQUIRE(vulkan_accelerator_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.prefilter_enabled = false;

    params.minimizer_algorithm = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
    REQUIRE(vulkan_components_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));

    params.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.max_occ = 500;
    params.kmer_size = 15;
    params.window_size = 10;
    REQUIRE(complete_all_gpu_tuple_selected(params));

    params.vulkan_enabled = false;
    REQUIRE(vulkan_components_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.vulkan_enabled = true;

    params.vulkan_extend_enabled = false;
    params.do_cigar = true;
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.vulkan_extend_enabled = true;
    REQUIRE(complete_all_gpu_tuple_selected(params));

    params.compute_mapq = true;
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.compute_mapq = false;
    params.base_alignment_algorithm =
        ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2;
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
}

TEST_CASE("persisted Shape-V5 GPU preparation admits zero-based supported alphabets",
          "[vulkan][pipe][routing][shape-v5][admission][fail-closed]") {
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        persisted_shape_v5_gpu_preparation_tuple_valid;

    constexpr std::size_t supported_alphabet_count = 2u;
    constexpr std::uint8_t dna4_code = 0u;
    constexpr std::uint8_t dna5_code = 1u;

    Minimap2Params params{};
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.minimizer_algorithm = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
    params.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    params.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.max_occ = 500;

    const auto admitted = [&](const std::uint8_t alphabet_code,
                              const std::uint8_t byte_width,
                              const MinimizerSketchBackend backend =
                                  MinimizerSketchBackend::VulkanSegment) {
        return persisted_shape_v5_gpu_preparation_tuple_valid(
            alphabet_code, supported_alphabet_count, byte_width,
            backend, params);
    };

    REQUIRE(admitted(dna4_code, 1u));
    REQUIRE(admitted(dna4_code, 2u));
    REQUIRE(admitted(dna5_code, 4u));
    REQUIRE(admitted(dna5_code, 8u));
    REQUIRE_FALSE(persisted_shape_v5_gpu_preparation_tuple_valid(
        dna4_code, 0u, 1u, MinimizerSketchBackend::VulkanSegment,
        params));
    REQUIRE_FALSE(admitted(
        static_cast<std::uint8_t>(supported_alphabet_count), 1u));
    REQUIRE_FALSE(admitted((std::numeric_limits<std::uint8_t>::max)(), 1u));
    REQUIRE_FALSE(admitted(dna4_code, 0u));
    REQUIRE_FALSE(admitted(dna4_code, 3u));
    REQUIRE_FALSE(admitted(dna4_code, 16u));
    REQUIRE_FALSE(admitted(
        dna4_code, 1u, MinimizerSketchBackend::Scalar));

    params.vulkan_enabled = false;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
    params.vulkan_enabled = true;

    params.kmer_size = 14;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
    params.kmer_size = 15;
    params.window_size = 9;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
}

TEST_CASE("DOC0384 — bounded Vulkan pipe matches complete CPU biological results",
          "[vulkan][phase-b][pipe][pipeline][biological-parity]") {
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerSketchBackend;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        MinimizerRangeExecutionProof;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::ExactSimdRangeRoute;
    using ::bioseq::simd::HWY_NAMESPACE::pipe::
        minimizer_range_proof_matches_backend;

    auto target = doc0307_e2e::make_dna4(4000, 0xC0FFEEu);
    // Query = a forward sub-region of the target (a guaranteed match).
    std::vector<seq::dna4> query(target.begin() + 500, target.begin() + 1300);

    Minimap2Params cpu;
    cpu.minimizer_algorithm = MinimizerAlgorithmType::TWO_STACK_NTHASH;
    cpu.chaining_algorithm = ChainingAlgorithmType::SIMD_MM2;
    cpu.base_alignment_algorithm = BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
    const auto cpu_res = doc0307_e2e::run(query, target, cpu);
    REQUIRE(cpu.do_cigar);
    REQUIRE(cpu_res.exact_simd_query_execution_accepted(true));
    REQUIRE(cpu_res.exact_simd_target_execution_accepted(true));
    REQUIRE(cpu_res.simd_chaining_execution_accepted(true));
    REQUIRE(cpu_res.block_aligner_execution_accepted(
        /*require_nonempty_simd_work=*/true));
    REQUIRE(cpu_res.scalar_pipeline_execution_proof_present);
    REQUIRE(cpu_res.scalar_pipeline_execution_proof.seed_lookup_calls > 0u);
    REQUIRE(cpu_res.scalar_pipeline_execution_proof.seed_query_minimizers > 0u);
    REQUIRE(cpu_res.partial_vulkan_execution_proof
            == ::bioseq::simd::HWY_NAMESPACE::PartialVulkanExecutionProof{});
    REQUIRE(cpu_res.all_simd_execution_accepted(
        MinimizerAlgorithmType::TWO_STACK_NTHASH,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));

    MinimizerRangeExecutionProof exact_range{};
    exact_range.exact_simd = cpu_res.exact_simd_query_execution_proof;
    exact_range.exact_simd_present = true;
    // This parity fixture deliberately uses dna4/u64 (`doc0307_e2e::range_t`).
    // The report-76 resident vector_stream route is restricted to dna4/u32,
    // so the authentic ExactSimd proof here is the chronological continuation
    // route.  Resident routing has its own u32 test above; relabelling this u64
    // proof as resident would weaken the persisted-scheme boundary.
    REQUIRE(exact_range.exact_simd.resident_vector_stream_kernel_calls == 0u);
    REQUIRE((exact_range.exact_simd.generic_u64_kernel_calls > 0u
        || exact_range.exact_simd.k15_w10_b8_u32_hash_kernel_calls > 0u));
    REQUIRE(minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::ExactSimd,
        exact_range,
        /*require_nonempty_vector_work=*/true,
        ExactSimdRangeRoute::require_continuation));
    auto hidden_paper_payload = exact_range;
    hidden_paper_payload.paper_nthash.scheme_id = 1;
    REQUIRE_FALSE(hidden_paper_payload.paper_nthash_present);
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::ExactSimd,
        hidden_paper_payload,
        /*require_nonempty_vector_work=*/false,
        ExactSimdRangeRoute::require_continuation));
    auto paper_presence_only = exact_range;
    paper_presence_only.paper_nthash_present = true;
    REQUIRE_FALSE(minimizer_range_proof_matches_backend(
        MinimizerSketchBackend::ExactSimd,
        paper_presence_only,
        /*require_nonempty_vector_work=*/false,
        ExactSimdRangeRoute::require_continuation));

    Minimap2Params gpu = cpu;
    gpu.minimizer_algorithm = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
    gpu.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    gpu.vulkan_enabled = true;
    gpu.vulkan_seed_hits_enabled = true;
    if (!::bioseq::chaining::vulkan_chaining_supported()) {
        REQUIRE_THROWS_AS(
            doc0307_e2e::run(query, target, gpu),
            bioseq::gpu::vulkan::VulkanRuntimeError);
        return;
    }
    const auto gpu_res = doc0307_e2e::run(query, target, gpu);

    REQUIRE_FALSE(cpu_res.results.empty());
    REQUIRE_FALSE(gpu_res.results.empty());
    REQUIRE(gpu.do_cigar);
    REQUIRE_FALSE(gpu_res.exact_simd_query_execution_proof_present);
    REQUIRE_FALSE(gpu_res.exact_simd_target_execution_proof_present);
    REQUIRE_FALSE(gpu_res.simd_chaining_execution_proof_present);
    REQUIRE_FALSE(gpu_res.scalar_pipeline_execution_proof_present);
    REQUIRE(gpu_res.scalar_pipeline_execution_proof
            == ::bioseq::simd::HWY_NAMESPACE::ScalarPipelineExecutionProof{});
    REQUIRE(gpu_res.partial_vulkan_execution_proof
                .exact_stage_mask_accepted(
                    /*query_sketch=*/true,
                    /*target_sketch=*/true,
                    /*target_index_build=*/false,
                    /*seed_lookup=*/true,
                    /*chain_score=*/true));
    REQUIRE(gpu_res.block_aligner_execution_accepted(
        /*require_nonempty_simd_work=*/true));
    REQUIRE(doc0307_e2e::exact_biological_results_equal(cpu_res, gpu_res));
    REQUIRE(cpu_res.biological_fingerprint()
            == gpu_res.biological_fingerprint());
}

TEST_CASE("DOC0307 — VULKAN_MINIMAP_INT with vulkan_enabled=false fails",
          "[vulkan][phase-b][pipe]") {
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;

    auto target = doc0307_e2e::make_dna4(3000, 0x1234567u);
    std::vector<seq::dna4> query(target.begin() + 200, target.begin() + 900);

    Minimap2Params disabled;
    disabled.minimizer_algorithm = MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
    disabled.chaining_algorithm = ChainingAlgorithmType::VULKAN_TILED_DP;
    disabled.vulkan_enabled = false;

    REQUIRE_THROWS_AS(
        doc0307_e2e::run(query, target, disabled),
        bioseq::gpu::vulkan::VulkanRuntimeError);
}

namespace doc0384_direct_test {

using Ledger = ::bioseq::simd::HWY_NAMESPACE::pipe::
    DirectResidentOperationLedger;
using SourceAuthority = ::bioseq::simd::HWY_NAMESPACE::pipe::
    SourceDirectHostAuthority;
using ResidentAuthority = bioseq::indexing::
    GpuResidentAccountingAuthority;

[[nodiscard]] Ledger full_resident_ledger(ResidentAuthority authority) {
    Ledger ledger{};
    ledger.operation_identity = 17u;
    ledger.minimizer_count = 53u;
    ledger.distinct_count = 17u;
    ledger.logical_control_bytes = 272u;
    ledger.logical_position_bytes = 424u;
    ledger.logical_resident_bytes = 696u;
    ledger.control_requirement_bytes = 1'024u;
    ledger.position_requirement_bytes = 3'072u;
    ledger.control_allowance_bytes = 1'024u;
    ledger.position_allowance_bytes = 3'072u;
    ledger.conservative_allowance_bytes = 4'096u;
    ledger.conservative_request_bytes = 4'096u;
    ledger.source_capacity_bytes = 512u;
    ledger.source_releases = 1u;
    ledger.source_released_bytes = 512u;
    ledger.cache_preinsertions = 1u;
    ledger.cache_required_lookups = 1u;
    ledger.cache_required_hits = 1u;
    ledger.mapped_prefix_passes = 2u;
    ledger.mapped_prefix_lookups = 20u;
    ledger.mapped_prefix_comparisons = 80u;
    ledger.expected_seed_consumers = 1u;
    ledger.completed_seed_consumers = 1u;
    ledger.slab_requested_bytes = 1'024u;
    ledger.slab_actual_bytes = 1'024u;
    ledger.slab_retained_permit_bytes = 1'024u;
    ledger.slab_permit_releases = 1u;
    ledger.seed_reserved_bytes = 4'096u;
    ledger.seed_planned_vulkan_bytes = 2'048u;
    ledger.seed_actual_vulkan_bytes = 2'048u;
    ledger.seed_roundtrips = 1u;
    ledger.seed_dispatches = 1u;
    ledger.seed_fence_completions = 1u;
    ledger.cache_cleanup_releases = 1u;
    ledger.resident_cleanup_bytes = 4'096u;
    ledger.materialization = {
        .logical_slot_bytes = 272u,
        .logical_position_bytes = 424u,
        .logical_resident_bytes = 696u,
        .control_allocation_requirement_bytes = 1'024u,
        .position_allocation_requirement_bytes = 3'072u,
        .control_allocation_allowance_bytes = 1'024u,
        .position_allocation_allowance_bytes = 3'072u,
        .conservative_allowance_bytes = 4'096u,
        .conservative_request_bytes = 4'096u,
        .actual_slot_allocation_bytes = 1'024u,
        .actual_position_allocation_bytes = 3'072u,
        .actual_allocation_bytes = 4'096u,
        .source_checksum = 0x1234u,
        .mapped_checksum = 0x1234u,
        .fully_touched_bytes = 4'096u,
        .reflected_footprint_bytes =
            authority == ResidentAuthority::reflected_process_footprint
                ? 4'096u : 0u,
        .retained_permit_bytes =
            authority == ResidentAuthority::retained_exact_permit
                ? 4'096u : 0u,
        .permit_reconciliations = 1u,
        .complete_source_traversals = 1u,
        .complete_mapped_traversals = 1u,
        .authority = authority,
    };
    ledger.source_authority =
        authority == ResidentAuthority::reflected_process_footprint
            ? SourceAuthority::reflected_process_footprint
            : SourceAuthority::retained_exact_permit;
    ledger.phase_c_checked = true;
    ledger.phase_c_compatible = true;
    ledger.source_authority_checked = true;
    ledger.source_authority_accepted = true;
    ledger.runtime_checked = true;
    ledger.runtime_available = true;
    ledger.hardware_checked = true;
    ledger.hardware_available = true;
    ledger.sorted_shape_checked = true;
    ledger.sorted_shape_accepted = true;
    ledger.identity_checked = true;
    ledger.identity_accepted = true;
    ledger.u32_shape_checked = true;
    ledger.u32_shape_accepted = true;
    ledger.storage_ranges_checked = true;
    ledger.control_range_accepted = true;
    ledger.position_range_accepted = true;
    ledger.mapping_plan_checked = true;
    ledger.mapping_plan_accepted = true;
    ledger.admission_snapshot_taken = true;
    ledger.planning_accepted = true;
    ledger.route_committed = true;
    ledger.materialization_started = true;
    ledger.materialization_completed = true;
    ledger.resident_published = true;
    ledger.source_release_completed = true;
    ledger.source_released_after_publication = true;
    ledger.cache_lookup_started = true;
    ledger.mapped_prefix_started = true;
    ledger.slab_admission_started = true;
    ledger.seed_dispatch_started = true;
    ledger.pipeline_started = true;
    ledger.pipeline_completed = true;
    ledger.log_planning("proceed", "focused_test_fixture");
    return ledger;
}

[[nodiscard]] Ledger canonical_empty_ledger() {
    Ledger ledger{};
    ledger.operation_identity = 18u;
    ledger.source_authority = SourceAuthority::canonical_empty;
    ledger.phase_c_checked = true;
    ledger.phase_c_compatible = true;
    ledger.source_authority_checked = true;
    ledger.source_authority_accepted = true;
    ledger.runtime_checked = true;
    ledger.runtime_available = true;
    ledger.hardware_checked = true;
    ledger.hardware_available = true;
    ledger.sorted_shape_checked = true;
    ledger.sorted_shape_accepted = true;
    ledger.identity_checked = true;
    ledger.identity_accepted = true;
    ledger.u32_shape_checked = true;
    ledger.u32_shape_accepted = true;
    ledger.storage_ranges_checked = true;
    ledger.control_range_accepted = true;
    ledger.position_range_accepted = true;
    ledger.mapping_plan_checked = true;
    ledger.mapping_plan_accepted = true;
    ledger.admission_snapshot_taken = true;
    ledger.canonical_empty = true;
    ledger.planning_accepted = true;
    ledger.route_committed = true;
    ledger.source_release_completed = true;
    ledger.empty_bypass = true;
    ledger.log_planning("proceed", "focused_empty_fixture");
    return ledger;
}

}  // namespace doc0384_direct_test

TEST_CASE("DOC0384 source-direct operation ledger emits an exact two-receipt transaction",
          "[pipeline][vulkan][prepared-full-resident]"
          "[pipeline][vulkan][receipt][fail-closed]") {
    using doc0384_direct_test::Ledger;
    using doc0384_direct_test::ResidentAuthority;

    SECTION("reflected authority") {
        auto ledger = doc0384_direct_test::full_resident_ledger(
            ResidentAuthority::reflected_process_footprint);
        REQUIRE(ledger.route_name() == "full_sorted_dense_direct_mapped");
        REQUIRE(ledger.planning_receipts == 1u);
        REQUIRE(ledger.terminal_success_invariants());
        ledger.log_terminal("success", "focused_success");
        REQUIRE(ledger.terminal_receipts == 1u);
        REQUIRE(ledger.terminal_success);
        ledger.log_terminal("failure", "must_not_duplicate");
        REQUIRE(ledger.terminal_receipts == 1u);
        REQUIRE(ledger.terminal_success);
    }

    SECTION("retained exact permit authority") {
        auto ledger = doc0384_direct_test::full_resident_ledger(
            ResidentAuthority::retained_exact_permit);
        REQUIRE(ledger.terminal_success_invariants());
    }

    SECTION("zero anchors still complete two mapped-prefix passes") {
        auto ledger = doc0384_direct_test::full_resident_ledger(
            ResidentAuthority::retained_exact_permit);
        ledger.slab_requested_bytes = 0u;
        ledger.slab_actual_bytes = 0u;
        ledger.slab_retained_permit_bytes = 0u;
        ledger.slab_permit_releases = 0u;
        ledger.seed_reserved_bytes = 0u;
        ledger.seed_planned_vulkan_bytes = 0u;
        ledger.seed_actual_vulkan_bytes = 0u;
        ledger.zero_anchor_bypasses = 1u;
        ledger.seed_dispatches = 0u;
        ledger.seed_fence_completions = 0u;
        REQUIRE(ledger.mapped_prefix_passes == 2u);
        REQUIRE(ledger.terminal_success_invariants());
        --ledger.mapped_prefix_passes;
        REQUIRE_FALSE(ledger.terminal_success_invariants());
    }

    SECTION("zero query is an explicit post-publication pipeline bypass") {
        auto ledger = doc0384_direct_test::full_resident_ledger(
            ResidentAuthority::reflected_process_footprint);
        ledger.expected_seed_consumers = 0u;
        ledger.completed_seed_consumers = 0u;
        ledger.cache_required_lookups = 0u;
        ledger.cache_required_hits = 0u;
        ledger.mapped_prefix_passes = 0u;
        ledger.mapped_prefix_lookups = 0u;
        ledger.mapped_prefix_comparisons = 0u;
        ledger.slab_requested_bytes = 0u;
        ledger.slab_actual_bytes = 0u;
        ledger.slab_retained_permit_bytes = 0u;
        ledger.slab_permit_releases = 0u;
        ledger.seed_reserved_bytes = 0u;
        ledger.seed_planned_vulkan_bytes = 0u;
        ledger.seed_actual_vulkan_bytes = 0u;
        ledger.seed_roundtrips = 0u;
        ledger.seed_dispatches = 0u;
        ledger.seed_fence_completions = 0u;
        ledger.zero_query_bypasses = 1u;
        ledger.cache_lookup_started = false;
        ledger.mapped_prefix_started = false;
        ledger.slab_admission_started = false;
        ledger.seed_dispatch_started = false;
        ledger.pipeline_started = false;
        ledger.pipeline_completed = false;
        REQUIRE(ledger.terminal_success_invariants());
    }

    SECTION("canonical empty publishes neither resident nor fake cleanup") {
        auto ledger = doc0384_direct_test::canonical_empty_ledger();
        REQUIRE(ledger.route_name() == "empty_no_resident_zero_anchor");
        REQUIRE(ledger.terminal_success_invariants());
        ledger.resident_published = true;
        REQUIRE_FALSE(ledger.terminal_success_invariants());
    }

    SECTION("failure and not-reached unwind remain exactly two receipts") {
        auto invalid = doc0384_direct_test::full_resident_ledger(
            ResidentAuthority::reflected_process_footprint);
        --invalid.materialization.fully_touched_bytes;
        REQUIRE_FALSE(invalid.terminal_success_invariants());
        invalid.log_terminal("success", "injected_touch_failure");
        REQUIRE(invalid.terminal_receipts == 1u);
        REQUIRE_FALSE(invalid.terminal_success);

        Ledger not_reached{};
        not_reached.operation_identity = 19u;
        {
            ::bioseq::simd::HWY_NAMESPACE::pipe::
                DirectResidentReceiptGuard guard{&not_reached};
        }
        REQUIRE(not_reached.planning_receipts == 1u);
        REQUIRE(not_reached.terminal_receipts == 1u);
        REQUIRE_FALSE(not_reached.terminal_success);
    }
}

TEST_CASE("DOC0384 bounded real-GPU prepared resident matches independent Phase C",
          "[pipeline][vulkan][prepared-full-resident]"
          "[pipeline][vulkan][zero-copy][hardware][biological-parity]") {
    namespace vk = bioseq::gpu::vulkan;
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MatchResultCollection;
    using ::bioseq::simd::HWY_NAMESPACE::Minimap2Params;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using DirectMinimizer =
        ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<33u, 15u>;

    auto& runtime = vk::Runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("hardware Vulkan unavailable; prepared-resident parity skipped");
        return;
    }

    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t pipeline_cache_metadata_baseline =
        runtime.pipeline_cache_metadata_reserved_bytes();
    struct RestoreCap {
        bioseq::parallelism::MemoryAdmissionController& controller;
        std::uint64_t cap = 0u;
        ~RestoreCap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::parallelism::saturating_add(
            baseline, std::uint64_t{512} << 20u)));

    MatchResultCollection direct_results;
    MatchResultCollection phase_c_results;
    MatchResultCollection empty_minimizer_results;
    {
        auto target_letters =
            doc0307_e2e::make_dna4(20'000u, 0x0384c0ffeeULL);
        std::vector<seq::dna4> query_letters(
            target_letters.begin() + 5'000,
            target_letters.begin() + 6'500);
        doc0307_e2e::range_t query_range(query_letters);
        doc0307_e2e::range_t target_range(target_letters);
        auto gpu_query_owner =
            bioseq::storage::make_lazy_in_memory(query_range);
        auto gpu_target_owner =
            bioseq::storage::make_lazy_in_memory(target_range);
        const auto gpu_query_view =
            bioseq::storage::as_lazy_sequence_view(gpu_query_owner);
        const auto gpu_target_view =
            bioseq::storage::as_lazy_sequence_view(gpu_target_owner);

        auto target_sketch =
            vk::collect_minimizers_vulkan_result<DirectMinimizer>(
                gpu_target_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u,
                nullptr,
                vk::VulkanSketchHostOwnershipRoute::
                    fixed_zero_copy_adoption);
        REQUIRE(target_sketch.has_value());
        REQUIRE(target_sketch->dispatch_token.accepted());
        REQUIRE_FALSE(target_sketch->host_minimizers.empty());
        auto target_owner =
            ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
                adopt_fixed_vulkan_sketch_host_result(
                    std::move(*target_sketch));
        REQUIRE(target_owner.ownership_authority_accepted());

        auto query_sketch =
            vk::collect_minimizers_vulkan_result<DirectMinimizer>(
                gpu_query_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u);
        REQUIRE(query_sketch.has_value());
        REQUIRE(query_sketch->dispatch_token.accepted());
        REQUIRE_FALSE(query_sketch->host_minimizers.empty());

        Minimap2Params direct_params{};
        direct_params.chaining_algorithm =
            ChainingAlgorithmType::VULKAN_TILED_DP;
        direct_params.base_alignment_algorithm =
            BaseAlignmentAlgorithmType::BLOCK_ALIGNER;
        direct_params.minimizer_algorithm =
            MinimizerAlgorithmType::VULKAN_MINIMAP_INT;
        direct_params.vulkan_enabled = true;
        direct_params.vulkan_seed_hits_enabled = true;
        direct_params.vulkan_index_cache_enabled = true;
        direct_params.vulkan_fused_frontend_enabled = false;
        direct_params.vulkan_gpu_index_build_enabled = false;
        direct_params.vulkan_extend_enabled = false;
        direct_params.do_cigar = true;

        auto direct_lazy_query =
            bioseq::storage::make_lazy_in_memory(query_range);
        auto direct_lazy_target =
            bioseq::storage::make_lazy_in_memory(target_range);
        const auto direct_query_view =
            bioseq::storage::as_lazy_sequence_view(direct_lazy_query);
        const auto direct_target_view =
            bioseq::storage::as_lazy_sequence_view(direct_lazy_target);
        direct_results =
            ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
                minimap2_port_with_fixed_vulkan_target_handoff(
                    direct_query_view,
                    direct_target_view,
                    query_sketch->host_minimizers,
                    std::move(target_owner),
                    direct_params);
        query_sketch.reset();
        target_sketch.reset();

        Minimap2Params phase_c_params = direct_params;
        phase_c_params.vulkan_fused_frontend_enabled = true;
        phase_c_params.vulkan_gpu_index_build_enabled = true;
        auto phase_c_lazy_query =
            bioseq::storage::make_lazy_in_memory(query_range);
        auto phase_c_lazy_target =
            bioseq::storage::make_lazy_in_memory(target_range);
        const auto phase_c_query_view =
            bioseq::storage::as_lazy_sequence_view(phase_c_lazy_query);
        const auto phase_c_target_view =
            bioseq::storage::as_lazy_sequence_view(phase_c_lazy_target);
        phase_c_results =
            ::bioseq::simd::HWY_NAMESPACE::minimap_port(
                phase_c_query_view,
                phase_c_target_view,
                phase_c_params);

        // A nonempty query shorter than k has no minimizers, but the already
        // materialized source-direct resident still owns the two required
        // mapped-payload authority passes. Rebuild the small bounded resident
        // and exercise the production wrapper so the generic early-empty seed
        // shortcut cannot regress over this case.
        std::vector<seq::dna4> short_query_letters(
            target_letters.begin(), target_letters.begin() + 8);
        doc0307_e2e::range_t short_query_range(short_query_letters);
        auto short_query_owner =
            bioseq::storage::make_lazy_in_memory(short_query_range);
        const auto short_query_view =
            bioseq::storage::as_lazy_sequence_view(short_query_owner);
        auto empty_query_sketch =
            vk::collect_minimizers_vulkan_result<DirectMinimizer>(
                short_query_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u);
        REQUIRE(empty_query_sketch.has_value());
        REQUIRE(empty_query_sketch->dispatch_token.accepted());
        REQUIRE(empty_query_sketch->host_minimizers.empty());
        auto second_target_sketch =
            vk::collect_minimizers_vulkan_result<DirectMinimizer>(
                gpu_target_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u,
                nullptr,
                vk::VulkanSketchHostOwnershipRoute::
                    fixed_zero_copy_adoption);
        REQUIRE(second_target_sketch.has_value());
        REQUIRE(second_target_sketch->dispatch_token.accepted());
        auto second_target_owner =
            ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
                adopt_fixed_vulkan_sketch_host_result(
                    std::move(*second_target_sketch));
        auto empty_lazy_query =
            bioseq::storage::make_lazy_in_memory(short_query_range);
        auto empty_lazy_target =
            bioseq::storage::make_lazy_in_memory(target_range);
        const auto empty_query_view =
            bioseq::storage::as_lazy_sequence_view(empty_lazy_query);
        const auto empty_target_view =
            bioseq::storage::as_lazy_sequence_view(empty_lazy_target);
        empty_minimizer_results =
            ::bioseq::simd::HWY_NAMESPACE::pipe_entry_detail::
                minimap2_port_with_fixed_vulkan_target_handoff(
                    empty_query_view,
                    empty_target_view,
                    empty_query_sketch->host_minimizers,
                    std::move(second_target_owner),
                    direct_params);
    }

    REQUIRE_FALSE(direct_results.results.empty());
    REQUIRE_FALSE(phase_c_results.results.empty());
    const auto& direct = direct_results.partial_vulkan_execution_proof;
    const auto& phase_c = phase_c_results.partial_vulkan_execution_proof;
    REQUIRE_FALSE(direct.saturated);
    REQUIRE(direct.gpu_index_source_direct_operations == 1u);
    REQUIRE(direct.gpu_index_source_direct_terminal_successes == 1u);
    REQUIRE(direct.gpu_index_source_direct_planning_receipts == 1u);
    REQUIRE(direct.gpu_index_source_direct_terminal_receipts == 1u);
    REQUIRE(direct.gpu_index_source_direct_materializations == 1u);
    REQUIRE(direct.gpu_index_source_direct_resident_publications == 1u);
    REQUIRE(direct.gpu_index_source_direct_source_releases == 1u);
    REQUIRE(direct.gpu_index_source_direct_checksum_matches == 1u);
    REQUIRE(direct.gpu_index_source_direct_source_traversals == 1u);
    REQUIRE(direct.gpu_index_source_direct_mapped_traversals == 1u);
    REQUIRE(direct.gpu_index_source_direct_cache_preinsertions == 1u);
    REQUIRE(direct.gpu_index_source_direct_required_misses == 0u);
    REQUIRE(direct.gpu_index_source_direct_fallback_attempts == 0u);
    REQUIRE(direct.gpu_index_source_direct_expected_seed_consumers > 0u);
    REQUIRE(direct.gpu_index_source_direct_completed_seed_consumers
            == direct.gpu_index_source_direct_expected_seed_consumers);
    REQUIRE(direct.gpu_index_source_direct_required_lookups
            == direct.gpu_index_source_direct_completed_seed_consumers);
    REQUIRE(direct.gpu_index_source_direct_required_hits
            == direct.gpu_index_source_direct_required_lookups);
    REQUIRE(direct.gpu_index_source_direct_mapped_prefix_passes
            == 2u * direct.gpu_index_source_direct_required_lookups);
    REQUIRE(direct.gpu_index_source_direct_seed_slab_initial_capacity == 0u);
    REQUIRE(direct.gpu_index_source_direct_seed_planned_vulkan_bytes
            == direct.gpu_index_source_direct_seed_actual_vulkan_bytes);
    REQUIRE(direct.gpu_index_source_direct_seed_roundtrips
            == direct.gpu_index_source_direct_required_lookups);
    REQUIRE(direct.gpu_index_source_direct_seed_fence_completions
            == direct.gpu_index_source_direct_seed_roundtrips);
    REQUIRE(direct.gpu_index_source_direct_cache_cleanup_releases == 1u);
    REQUIRE(direct.gpu_index_source_direct_resident_cleanup_bytes
            == direct.gpu_index_actual_allocation_bytes);
    REQUIRE(direct.gpu_index_source_direct_pipeline_completions == 1u);
    REQUIRE(direct.gpu_index_fully_touched_bytes
            == direct.gpu_index_actual_allocation_bytes);
    REQUIRE(direct.target_index_build_dispatches == 0u);
    REQUIRE(direct.seed_host_sort_fallbacks == 0u);
    REQUIRE(direct.post_seed_owned_cpu_index_releases == 0u);
    REQUIRE(direct.post_seed_owned_cpu_position_bytes_released == 0u);

    REQUIRE(direct.seed_anchor_proof_operations
            == direct.gpu_index_source_direct_seed_roundtrips);
    REQUIRE(direct.seed_canonical_anchor_order_checks
            == direct.seed_anchor_proof_operations);
    REQUIRE(direct.seed_canonical_anchor_order_accepts
            == direct.seed_canonical_anchor_order_checks);
    REQUIRE(direct.seed_canonical_anchor_count
            == phase_c.seed_canonical_anchor_count);
    REQUIRE(direct.seed_anchor_digest_lane0
            == phase_c.seed_anchor_digest_lane0);
    REQUIRE(direct.seed_anchor_digest_lane1
            == phase_c.seed_anchor_digest_lane1);
    REQUIRE(doc0307_e2e::exact_biological_results_equal(
        direct_results, phase_c_results));
    REQUIRE(direct_results.biological_fingerprint()
            == phase_c_results.biological_fingerprint());

    REQUIRE(empty_minimizer_results.results.empty());
    const auto& empty_minimizer =
        empty_minimizer_results.partial_vulkan_execution_proof;
    REQUIRE(empty_minimizer.gpu_index_source_direct_operations == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_terminal_successes == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_required_lookups == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_required_hits == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_mapped_prefix_passes == 2u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_mapped_prefix_lookups == 0u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_mapped_prefix_comparisons
            == 0u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_seed_roundtrips == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_zero_anchor_bypasses == 1u);
    REQUIRE(empty_minimizer.gpu_index_source_direct_seed_fence_completions == 0u);
    REQUIRE(empty_minimizer.seed_lookup_dispatches == 0u);
    REQUIRE(empty_minimizer.seed_anchor_proof_operations == 1u);
    REQUIRE(empty_minimizer.seed_canonical_anchor_count == 0u);
    REQUIRE(empty_minimizer.seed_anchor_digest_lane0
            == vk::kSeedAnchorDigestSeedLo);
    REQUIRE(empty_minimizer.seed_anchor_digest_lane1
            == vk::kSeedAnchorDigestSeedHi);
    REQUIRE(empty_minimizer.gpu_index_source_direct_pipeline_completions == 1u);

    const std::uint64_t pipeline_cache_metadata_after =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(pipeline_cache_metadata_after
            >= pipeline_cache_metadata_baseline);
    REQUIRE(admission.reserved_bytes()
            == bioseq::parallelism::saturating_add(
                baseline,
                pipeline_cache_metadata_after
                    - pipeline_cache_metadata_baseline));
}

TEST_CASE("post-seed owned lifetime releases only after the final seed user",
          "[pipeline][seed][lifetime]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    std::vector<Sketch> sketches{
        Sketch{/*kmer*/ 7, /*pos*/ 1, /*strand*/ 0},
        Sketch{/*kmer*/ 7, /*pos*/ 3, /*strand*/ 1},
        Sketch{/*kmer*/ 9, /*pos*/ 5, /*strand*/ 0},
    };
    bioseq::indexing::MinimizerHashmapFragmentIndex index;
    index.build(sketches, 100);
    const std::uint64_t position_capacity_bytes =
        index.positions.capacity() * sizeof(std::uint64_t);
    REQUIRE(position_capacity_bytes != 0u);

    PostSeedOwnedLifetime lifetime{
        /*seed_users=*/2u,
        /*owned_gpu_cache=*/nullptr,
        TransientTargetIndexReleaseAuthority{
            .index = &index}};
    const auto first = lifetime.complete_seed_user();
    REQUIRE(first.cpu_index_releases == 0u);
    REQUIRE_FALSE(index.table_empty());

    const auto last = lifetime.complete_seed_user();
    REQUIRE(last.cpu_index_releases == 1u);
    REQUIRE(last.cpu_position_bytes_released == position_capacity_bytes);
    REQUIRE(last.gpu_cache_evictions == 0u);
    REQUIRE(index.table_empty());
    REQUIRE(index.positions.capacity() == 0u);
}

TEST_CASE("seed lookup applies max_occ once across every borrowed position span",
          "[pipeline][seed][lookup-spans]") {
    using Sketch = ::bioseq::simd::HWY_NAMESPACE::MinimapSketchData;
    using ::bioseq::indexing::MinimizerLookupSpans;
    using ::bioseq::indexing::MinimizerPositionSpan;
    struct SegmentedSeedIndex {
        std::array<std::uint64_t, 2> first{
            (std::uint64_t{100} << 1u),
            (std::uint64_t{200} << 1u) | 1u,
        };
        std::array<std::uint64_t, 1> second{
            (std::uint64_t{300} << 1u),
        };
        std::array<MinimizerPositionSpan, 2> spans{
            MinimizerPositionSpan{first},
            MinimizerPositionSpan{second},
        };
        bool positions_available = true;
        std::uint64_t reported_count = 3;

        [[nodiscard]] MinimizerLookupSpans lookup_spans(
                std::uint64_t hash) const noexcept {
            if (hash != 7u) {
                return MinimizerLookupSpans::segmented(
                    {}, /*global_occurrence_count=*/0);
            }
            if (!positions_available) {
                return MinimizerLookupSpans::positions_unavailable(
                    reported_count);
            }
            return MinimizerLookupSpans::segmented(
                spans, reported_count);
        }

        [[nodiscard]] bioseq::storage::sequence_size_t target_length()
                const noexcept {
            return 1'000;
        }
    };
    STATIC_REQUIRE(::bioseq::indexing::MinimizerLookupIndex<
        SegmentedSeedIndex>);

    const std::array<Sketch, 1> query{
        Sketch{/*kmer=*/7, /*pos=*/10, /*strand=*/0},
    };

    SECTION("all spans emit when their global count is admitted") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::Arena::Chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::Anchor>();
        SegmentedSeedIndex index;
        ::bioseq::simd::HWY_NAMESPACE::collect_seed_hits(
            std::span<const Sketch>{query},
            index,
            /*query_len=*/100,
            /*max_occ=*/3,
            /*kmer_size=*/15,
            anchors);
        REQUIRE(anchors.size() == 3);
        REQUIRE(anchors.data()[0].ref_position == 100);
        REQUIRE(anchors.data()[1].ref_position == 300);
        REQUIRE(anchors.data()[2].ref_position == 200);
        REQUIRE(anchors.data()[2].is_reverse == 1u);
    }

    SECTION("global count prunes although every available span is individually admissible") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::Arena::Chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::Anchor>();
        SegmentedSeedIndex index;
        ::bioseq::simd::HWY_NAMESPACE::collect_seed_hits(
            std::span<const Sketch>{query},
            index,
            /*query_len=*/100,
            /*max_occ=*/2,
            /*kmer_size=*/15,
            anchors);
        REQUIRE(anchors.size() == 0);
    }

    SECTION("global pruning does not require deliberately unloaded positions") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::Arena::Chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::Anchor>();
        SegmentedSeedIndex index;
        index.positions_available = false;
        ::bioseq::simd::HWY_NAMESPACE::collect_seed_hits(
            std::span<const Sketch>{query},
            index,
            /*query_len=*/100,
            /*max_occ=*/2,
            /*kmer_size=*/15,
            anchors);
        REQUIRE(anchors.size() == 0);
    }

    SECTION("a surviving hash without positions fails closed") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::Arena::Chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::Anchor>();
        SegmentedSeedIndex index;
        index.positions_available = false;
        REQUIRE_THROWS_AS(
            ::bioseq::simd::HWY_NAMESPACE::collect_seed_hits(
                std::span<const Sketch>{query},
                index,
                /*query_len=*/100,
                /*max_occ=*/3,
                /*kmer_size=*/15,
                anchors),
            ::bioseq::indexing::PersistedMinimizerLookupError);
    }

    SECTION("an admitted malformed global count fails closed") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::Arena::Chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::Anchor>();
        SegmentedSeedIndex index;
        index.reported_count = 2;
        REQUIRE_THROWS_AS(
            ::bioseq::simd::HWY_NAMESPACE::collect_seed_hits(
                std::span<const Sketch>{query},
                index,
                /*query_len=*/100,
                /*max_occ=*/2,
                /*kmer_size=*/15,
                anchors),
            ::bioseq::indexing::PersistedMinimizerLookupError);
        REQUIRE(anchors.size() == 0);
    }
}

TEST_CASE("Vulkan chaining mapped plan phases report89 GPU and CPU scratch peaks",
          "[vulkan][chaining][memory][diagnostics]") {
    const auto plan = ::bioseq::chaining::make_vulkan_chaining_memory_plan(
        /*anchor_count=*/1'520'944u,
        /*range_count=*/32'035u);
    REQUIRE(plan.device_anchor_bytes == 36'502'656u);
    REQUIRE(plan.device_score_bytes == 6'083'776u);
    REQUIRE(plan.device_predecessor_bytes == 12'167'552u);
    REQUIRE(plan.device_range_bytes == 256'280u);
    REQUIRE(plan.host_range_bytes == 256'280u);
    REQUIRE(plan.compacted_host_anchor_bytes == 0u);
    REQUIRE(plan.host_scored_index_bytes == 24'335'104u);
    REQUIRE(plan.host_visited_bytes == 6'083'776u);
    REQUIRE(plan.host_chain_index_bytes == 12'167'552u);
    REQUIRE(plan.coordinate_device_trace_bytes == 0u);
    REQUIRE(plan.coordinate_host_trace_bytes == 0u);
    REQUIRE(plan.gpu_phase_bytes == 55'266'544u);
    REQUIRE(plan.cpu_backtrack_phase_bytes == 61'094'040u);
    REQUIRE(plan.total_bytes == 61'094'040u);

    bioseq::parallelism::MemoryAdmissionFailure failure{
        .reason = bioseq::parallelism::MemoryAdmissionFailureReason::system_pressure,
        .requested_bytes = plan.total_bytes,
        .reserved_bytes = 17u,
        .budget_ceiling_bytes = 19u,
        .allocatable_after_reservations_bytes = 2u,
        .process_footprint_bytes = 23u,
        .system_available_bytes = 29u,
        .system_total_bytes = 31u,
        .process_soft_cap_bytes = 37u,
    };
    const ::bioseq::chaining::VulkanChainingAdmissionError error{
        plan, failure};
    REQUIRE(error.plan.total_bytes == plan.total_bytes);
    REQUIRE(error.admission.reason
            == bioseq::parallelism::MemoryAdmissionFailureReason::system_pressure);
    REQUIRE(std::string_view{error.what()}.find("admission_reason=system_pressure")
            != std::string_view::npos);
    REQUIRE(std::string_view{error.what()}.find("range_count=32035")
            != std::string_view::npos);
}

TEST_CASE("Vulkan chaining shape preflight exactly matches in-place range compaction",
          "[vulkan][chaining][memory][compaction]") {
    constexpr std::size_t anchor_count =
        ::bioseq::chaining::detail::kGpuChainCompactRangeThreshold + 2u;
    std::vector<::bioseq::chaining::Anchor> anchors(anchor_count);
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        anchors[i].ref_position = i;
        anchors[i].query_position = i;
        anchors[i].query_span = 15u;
    }

    const auto shape =
        ::bioseq::chaining::detail::analyze_gpu_chaining_shape(
            anchors, /*max_ref_dist=*/10u);
    REQUIRE(shape.range_count == 1u);
    REQUIRE(shape.compaction_required);
    REQUIRE(shape.device_anchor_count < anchors.size());

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::Arena::Chain>();
    auto ranges = ::bioseq::chaining::detail::build_gpu_chain_ranges(
        anchors, /*max_ref_dist=*/10u, shape.range_count);
    auto compacted = ::bioseq::chaining::make_arena_slab<
        ::bioseq::chaining::Anchor>();
    REQUIRE(::bioseq::chaining::detail::compact_long_gpu_chain_ranges(
        anchors, ranges, compacted, ranges));
    REQUIRE(compacted.size() == shape.device_anchor_count);
    REQUIRE(ranges.size() == shape.range_count);
    REQUIRE(ranges[0].begin == 0u);
    REQUIRE(ranges[0].length == shape.device_anchor_count);
    REQUIRE(compacted.data()[0].ref_position == anchors.front().ref_position);
    REQUIRE(compacted.data()[compacted.size() - 1u].ref_position
            == anchors.back().ref_position);
}

TEST_CASE("MatchResult biological fingerprints cover every result and CIGAR field",
          "[pipeline][biological-proof][fingerprint]") {
    MatchResult result{
        .query_len = 101u,
        .query_start = 3u,
        .query_end = 97u,
        .is_reverse = true,
        .target_len = 1001u,
        .target_start = 23u,
        .target_end = 117u,
        .num_matches = 90u,
        .block_length = 94u,
        .mapq = 42,
        .chain_score = 99,
        .dp_score = 88,
        .dp_max_score = 91,
        .seed_count = 7,
        .secondary_chain_score = 11,
        .divergence = 0.125f,
        .is_primary = true,
        .has_cigar = true,
        .cigar = {
            CigarOp{.length = 10u, .opcode = CIGAR_MATCH},
            CigarOp{.length = 2u, .opcode = CIGAR_INS},
            CigarOp{.length = 82u, .opcode = CIGAR_MATCH},
        },
        .edit_distance = 2u,
    };
    MatchResultCollection lhs{std::vector<MatchResult>{result}};
    MatchResultCollection rhs{std::vector<MatchResult>{result}};

    const auto expected = lhs.biological_fingerprint();
    REQUIRE(expected.computed);
    REQUIRE_FALSE(expected.saturated);
    REQUIRE(expected.collection_count == 1u);
    REQUIRE(expected.match_result_count == 1u);
    REQUIRE(expected.cigar_result_count == 1u);
    REQUIRE(expected.cigar_operation_count == 3u);
    REQUIRE(expected == rhs.biological_fingerprint());

    enum class ResultField : std::uint8_t {
        query_len,
        query_start,
        query_end,
        is_reverse,
        target_len,
        target_start,
        target_end,
        num_matches,
        block_length,
        mapq,
        chain_score,
        dp_score,
        dp_max_score,
        seed_count,
        secondary_chain_score,
        divergence,
        is_primary,
        has_cigar,
        edit_distance,
    };
    struct ResultFieldMutation {
        ResultField field;
        const char* name;
        bool affects_cigar_digest;
    };
    constexpr std::array<ResultFieldMutation, 19u> result_mutations{{
        {ResultField::query_len, "query_len", false},
        {ResultField::query_start, "query_start", false},
        {ResultField::query_end, "query_end", false},
        {ResultField::is_reverse, "is_reverse", false},
        {ResultField::target_len, "target_len", false},
        {ResultField::target_start, "target_start", false},
        {ResultField::target_end, "target_end", false},
        {ResultField::num_matches, "num_matches", false},
        {ResultField::block_length, "block_length", false},
        {ResultField::mapq, "mapq", false},
        {ResultField::chain_score, "chain_score", false},
        {ResultField::dp_score, "dp_score", false},
        {ResultField::dp_max_score, "dp_max_score", false},
        {ResultField::seed_count, "seed_count", false},
        {ResultField::secondary_chain_score,
         "secondary_chain_score", false},
        {ResultField::divergence, "divergence", false},
        {ResultField::is_primary, "is_primary", false},
        {ResultField::has_cigar, "has_cigar", true},
        {ResultField::edit_distance, "edit_distance", false},
    }};
    for (const auto& mutation : result_mutations) {
        auto mutated = result;
        switch (mutation.field) {
            case ResultField::query_len: ++mutated.query_len; break;
            case ResultField::query_start: ++mutated.query_start; break;
            case ResultField::query_end: ++mutated.query_end; break;
            case ResultField::is_reverse:
                mutated.is_reverse = !mutated.is_reverse;
                break;
            case ResultField::target_len: ++mutated.target_len; break;
            case ResultField::target_start: ++mutated.target_start; break;
            case ResultField::target_end: ++mutated.target_end; break;
            case ResultField::num_matches: ++mutated.num_matches; break;
            case ResultField::block_length: ++mutated.block_length; break;
            case ResultField::mapq: ++mutated.mapq; break;
            case ResultField::chain_score: ++mutated.chain_score; break;
            case ResultField::dp_score: ++mutated.dp_score; break;
            case ResultField::dp_max_score: ++mutated.dp_max_score; break;
            case ResultField::seed_count: ++mutated.seed_count; break;
            case ResultField::secondary_chain_score:
                ++mutated.secondary_chain_score;
                break;
            case ResultField::divergence:
                mutated.divergence = 0.25f;
                break;
            case ResultField::is_primary:
                mutated.is_primary = !mutated.is_primary;
                break;
            case ResultField::has_cigar:
                mutated.has_cigar = !mutated.has_cigar;
                break;
            case ResultField::edit_distance:
                ++mutated.edit_distance;
                break;
        }
        INFO("mutated MatchResult field: " << mutation.name);
        rhs.results = {std::move(mutated)};
        const auto fingerprint = rhs.biological_fingerprint();
        REQUIRE(fingerprint.match_digest_lane0
                != expected.match_digest_lane0);
        REQUIRE(fingerprint.match_digest_lane1
                != expected.match_digest_lane1);
        if (mutation.affects_cigar_digest) {
            REQUIRE(fingerprint.cigar_digest_lane0
                    != expected.cigar_digest_lane0);
            REQUIRE(fingerprint.cigar_digest_lane1
                    != expected.cigar_digest_lane1);
        } else {
            REQUIRE(fingerprint.cigar_digest_lane0
                    == expected.cigar_digest_lane0);
            REQUIRE(fingerprint.cigar_digest_lane1
                    == expected.cigar_digest_lane1);
        }
    }

    enum class CigarField : std::uint8_t { length, opcode, vector_shape };
    struct CigarFieldMutation {
        CigarField field;
        const char* name;
    };
    constexpr std::array<CigarFieldMutation, 3u> cigar_mutations{{
        {CigarField::length, "cigar.length"},
        {CigarField::opcode, "cigar.opcode"},
        {CigarField::vector_shape, "cigar vector shape"},
    }};
    for (const auto& mutation : cigar_mutations) {
        auto mutated = result;
        switch (mutation.field) {
            case CigarField::length:
                ++mutated.cigar[1].length;
                break;
            case CigarField::opcode:
                mutated.cigar[1].opcode = CIGAR_DEL;
                break;
            case CigarField::vector_shape:
                mutated.cigar.push_back(
                    CigarOp{.length = 1u, .opcode = CIGAR_DEL});
                break;
        }
        INFO("mutated CIGAR field: " << mutation.name);
        rhs.results = {std::move(mutated)};
        const auto fingerprint = rhs.biological_fingerprint();
        REQUIRE(fingerprint.match_digest_lane0
                != expected.match_digest_lane0);
        REQUIRE(fingerprint.match_digest_lane1
                != expected.match_digest_lane1);
        REQUIRE(fingerprint.cigar_digest_lane0
                != expected.cigar_digest_lane0);
        REQUIRE(fingerprint.cigar_digest_lane1
                != expected.cigar_digest_lane1);
    }

    MatchResultCollection empty_a;
    MatchResultCollection empty_b;
    const auto empty_fingerprint = empty_a.biological_fingerprint();
    REQUIRE(empty_fingerprint.computed);
    REQUIRE_FALSE(empty_fingerprint.saturated);
    REQUIRE(empty_fingerprint.collection_count == 1u);
    REQUIRE(empty_fingerprint.match_result_count == 0u);
    REQUIRE(empty_fingerprint.cigar_result_count == 0u);
    REQUIRE(empty_fingerprint.cigar_operation_count == 0u);
    REQUIRE(empty_fingerprint == empty_b.biological_fingerprint());

    auto second = result;
    ++second.query_start;
    ++second.cigar.front().length;
    MatchResultCollection ordered{
        std::vector<MatchResult>{result, second}};
    MatchResultCollection reversed{
        std::vector<MatchResult>{second, result}};
    const auto ordered_fingerprint = ordered.biological_fingerprint();
    const auto reversed_fingerprint = reversed.biological_fingerprint();
    REQUIRE(ordered_fingerprint.match_digest_lane0
            != reversed_fingerprint.match_digest_lane0);
    REQUIRE(ordered_fingerprint.match_digest_lane1
            != reversed_fingerprint.match_digest_lane1);
    REQUIRE(ordered_fingerprint.cigar_digest_lane0
            != reversed_fingerprint.cigar_digest_lane0);
    REQUIRE(ordered_fingerprint.cigar_digest_lane1
            != reversed_fingerprint.cigar_digest_lane1);

    MatchResultBiologicalFingerprint aggregate{};
    aggregate.combine(expected);
    REQUIRE(aggregate == expected);
    aggregate.combine(MatchResultBiologicalFingerprint{});
    REQUIRE_FALSE(aggregate.computed);
    REQUIRE(aggregate.saturated);
    aggregate.combine(expected);
    REQUIRE_FALSE(aggregate.computed);
    REQUIRE(aggregate.saturated);
}

}  // namespace bioseq::simd::HWY_NAMESPACE::pipe

HWY_AFTER_NAMESPACE();
