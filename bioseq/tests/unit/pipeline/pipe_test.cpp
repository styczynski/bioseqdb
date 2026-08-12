// Tests for the templated pipeline API.
//
// Static-assert-only cases exercise the emit_token/emitted_token trait surface
// for single-emission invariants and sink concepts. The runtime
// cases drive a probe sink through a canonical pipeline to verify that every
// process*<Cep> body emits exactly once.
//
// These tests are compile-only in the static-assert sections (compile fails ⇒
// test fails); the runtime SECTIONs use Catch2 REQUIRE assertions and run as
// part of the bioseq unit-test target.

#include <tests/support/common.hpp>

#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.pipeline.pipe;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.chaining.simd;
import bioseq.chaining.core;
import bioseq.chaining.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.parallelism.memory_admission;
import bioseq.sequences.cigar;
import bioseq.storage.size;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.gpu.vulkan.runtime;

namespace {

[[nodiscard]] const ::bioseq::simd_dispatch::highway_kernel_table*
fixed_highway_kernels() {
    return &::bioseq::simd_dispatch::highway_test_kernel_table();
}

[[nodiscard]] ::bioseq::simd_dispatch::highway_dispatch_plan
fixed_highway_dispatch_plan() {
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    return {
        .compiled_targets = binding.target,
        .cpu_supported_targets = binding.target,
        .runnable_targets = binding.target,
        .planned_target = binding.target,
    };
}

template <sequence_alphabet p_alphabet = ::seq::dna4>
void bind_fixed_highway(::bioseq::simd::minimap2_parameters& params) {
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    const auto* identity = binding.kernels == nullptr
        || !binding.kernels->is_complete()
        ? nullptr
        : binding.kernels->template resolve_alphabet_identity<p_alphabet>();
    if (identity == nullptr
        || binding.kernels->resolve_alphabet_identity(
               identity->alphabet_code) != identity) {
        throw std::logic_error{
            "generated fixed-target test binding lacks alphabet identity"};
    }
    params.highway_kernels = binding.kernels;
    params.highway_executed_target = binding.target;
    params.highway_dispatch = fixed_highway_dispatch_plan();
    params.alphabet_code = identity->alphabet_code;
}

struct raw_sketch_append_fixture {
    using storage_type = std::uint64_t;
    std::uint64_t value = 0;
};
static_assert(std::is_trivially_copyable_v<raw_sketch_append_fixture>);
static_assert(sizeof(raw_sketch_append_fixture)
              == sizeof(raw_sketch_append_fixture::storage_type));

struct alignas(8) source_direct_minimizer_fixture {
    struct layout_type {
        static constexpr std::uint32_t k_position_bits = 33u;
        static constexpr std::uint32_t k_hash_bits = 30u;
    };
    std::uint64_t value = 0u;
};
static_assert(sizeof(source_direct_minimizer_fixture) == 8u);
static_assert(alignof(source_direct_minimizer_fixture) == 8u);

struct alignas(8) incompatible_source_direct_minimizer_fixture {
    struct layout_type {
        static constexpr std::uint32_t k_position_bits = 32u;
        static constexpr std::uint32_t k_hash_bits = 31u;
    };
    std::uint64_t value = 0u;
};
static_assert(sizeof(incompatible_source_direct_minimizer_fixture) == 8u);
static_assert(alignof(incompatible_source_direct_minimizer_fixture) == 8u);

struct budgeted_sketch_lifetime_probe {
    using storage_type = std::uint64_t;
    struct layout_type {
        static constexpr std::uint32_t k_position_bits = 28u;
        static constexpr std::uint32_t k_hash_bits = 35u;
    };
    static constexpr std::uint32_t k_hash_shift = 29u;
    static constexpr std::uint64_t k_position_field_mask = (std::uint64_t{1} << 28u) - 1u;

    bioseq::parallelism::memory_admission_controller* admission = nullptr;
    std::uint64_t expected_reserved = 0;
    bool* destroyed_with_live_permit = nullptr;
    bool active = true;

    [[nodiscard]] std::uint64_t raw_value() const noexcept { return 0u; }
    budgeted_sketch_lifetime_probe() = default;
    budgeted_sketch_lifetime_probe(
            bioseq::parallelism::memory_admission_controller* owner,
            std::uint64_t expected,
            bool* observed) noexcept
        : admission(owner),
          expected_reserved(expected),
          destroyed_with_live_permit(observed)
    {}
    budgeted_sketch_lifetime_probe(budgeted_sketch_lifetime_probe&& other) noexcept
        : admission(other.admission),
          expected_reserved(other.expected_reserved),
          destroyed_with_live_permit(other.destroyed_with_live_permit),
          active(std::exchange(other.active, false))
    {}
    budgeted_sketch_lifetime_probe(const budgeted_sketch_lifetime_probe&) = delete;
    budgeted_sketch_lifetime_probe& operator=(const budgeted_sketch_lifetime_probe&) = delete;
    budgeted_sketch_lifetime_probe& operator=(budgeted_sketch_lifetime_probe&&) = delete;
    ~budgeted_sketch_lifetime_probe() {
        if (active) {
            *destroyed_with_live_permit =
                admission->reserved_bytes() == expected_reserved;
        }
    }
};

TEST_CASE("budgeted_sketch_vector transfers and destroys host capacity before admission",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 20)));

    using owned_type = ::bioseq::simd::
        budgeted_sketch_vector<budgeted_sketch_lifetime_probe>;
    const auto make_owned = [&](bool& observed) {
        constexpr std::size_t count = 4u;
        const std::uint64_t headroom =
            count * sizeof(budgeted_sketch_lifetime_probe)
            * bioseq::parallelism::k_vector_preallocation_headroom_factor;
        auto permit = admission.try_reserve(headroom);
        REQUIRE(permit.has_value());
        owned_type owned;
        owned.permit = std::move(*permit);
        owned.minimizers.reserve(count);
        const std::uint64_t exact =
            owned.minimizers.capacity() * sizeof(budgeted_sketch_lifetime_probe);
        REQUIRE(exact <= owned.permit->bytes());
        owned.permit->shrink_to(exact);
        owned.minimizers.emplace_back(
            &admission, baseline + exact, &observed);
        return owned;
    };

    bool implicit_observed = false;
    {
        owned_type owned = make_owned(implicit_observed);
        owned_type moved{std::move(owned)};
        REQUIRE(owned.minimizers.capacity() == 0u);
        REQUIRE_FALSE(owned.permit.has_value());
        REQUIRE(moved.minimizers.size() == 1u);
    }
    REQUIRE(implicit_observed);
    REQUIRE(admission.reserved_bytes() == baseline);

    bool explicit_observed = false;
    {
        owned_type owned = make_owned(explicit_observed);
        owned.release_owned_memory_before_permit();
        REQUIRE(owned.minimizers.capacity() == 0u);
        REQUIRE_FALSE(owned.permit.has_value());
    }
    REQUIRE(explicit_observed);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("fixed Vulkan target sketch adopts host allocation and releases device before index",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<sketch_type>;
    using device_type = bioseq::gpu::vulkan::vulkan_device_minimizer_storage<sketch_type>;

    source_type source;
    source.host_minimizers = {
        sketch_type{/*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u},
        sketch_type{/*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u},
    };
    const sketch_type* const original_data = source.host_minimizers.data();
    const std::size_t original_capacity =
        source.host_minimizers.capacity();
    auto device = std::make_shared<device_type>();
    std::weak_ptr<device_type> device_lifetime = device;
    source.device = std::move(device);
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes =
        original_capacity * sizeof(sketch_type);
    source.arena_trimmed_before_host_materialization = true;

    using owner_type = ::bioseq::simd::pipe_entry_detail::
        fixed_vulkan_target_host_owner<sketch_type>;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<owner_type>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<owner_type>);
    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    const auto& receipt = adopted.adoption;

    REQUIRE(receipt.is_zero_copy_adoption_valid());
    REQUIRE(adopted.has_valid_ownership_authorization());
    REQUIRE(receipt.is_host_permit_released_to_process_footprint);
    REQUIRE(receipt.retained_host_permit_bytes == 0u);
    REQUIRE(receipt.device_owner_available_before_adoption);
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
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace ::bioseq::simd;
    using pipe_entry_detail::
        fixed_vulkan_target_host_adoption_route_selected;
    constexpr std::uint64_t minimum = pipe_entry_detail::
        k_vulkan_fixed_host_adoption_target_minimum_bp;
    // Route selection is intentionally independent of emitted-vector size.
    // An ambiguity-heavy dna5 target can be >=256 MiB yet emit only this
    // representative small nonzero payload, which must retain its exact permit
    // if RSS reconciliation is inconclusive.
    constexpr std::uint64_t sparse_dna5_emitted_bytes =
        2u * sizeof(minimap_sketch_data);
    minimap2_parameters params{};
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = false;
    params.vulkan_gpu_index_build_enabled = false;
    params.minimizer_algorithm =
        minimizer_algorithm::vulkan_minimap_integer;

    STATIC_REQUIRE(minimum == (std::uint64_t{256} << 20u));
    STATIC_REQUIRE(sparse_dna5_emitted_bytes < (std::uint64_t{64} << 10u));
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(
        /*is_gpu_target_index_build=*/false,
        params,
        /*alphabet_size=*/4u,
        minimum - 1u));
    REQUIRE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(
        /*is_gpu_target_index_build=*/false,
        params,
        /*alphabet_size=*/4u,
        minimum));
    REQUIRE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(
        /*is_gpu_target_index_build=*/false,
        params,
        /*alphabet_size=*/5u,
        minimum));
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(
        /*is_gpu_target_index_build=*/true,
        params,
        /*alphabet_size=*/4u,
        minimum));

    params.vulkan_enabled = false;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(
        /*is_gpu_target_index_build=*/false,
        params,
        /*alphabet_size=*/4u,
        minimum));
    params.vulkan_enabled = true;

    params.vulkan_seed_hits_enabled = false;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.vulkan_seed_hits_enabled = true;

    params.vulkan_fused_frontend_enabled = true;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.vulkan_fused_frontend_enabled = false;

    params.vulkan_gpu_index_build_enabled = true;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.vulkan_gpu_index_build_enabled = false;

    params.minimizer_algorithm = minimizer_algorithm::two_stack_nthash;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.minimizer_algorithm =
        minimizer_algorithm::vulkan_minimap_integer;

    params.kmer_size = 7;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.kmer_size = 27;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.kmer_size = 15;

    params.window_size = 9;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.window_size = 11;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.window_size = 10;

    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 20u, minimum));
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        incompatible_source_direct_minimizer_fixture>(
            false, params, 4u, minimum));
}

TEST_CASE("empty fixed Vulkan target result adopts zero bytes and releases device",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<sketch_type>;
    using device_type = bioseq::gpu::vulkan::vulkan_device_minimizer_storage<sketch_type>;

    source_type source;
    auto device = std::make_shared<device_type>();
    std::weak_ptr<device_type> device_lifetime = device;
    source.device = std::move(device);
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    const auto& receipt = adopted.adoption;

    REQUIRE(receipt.is_zero_copy_adoption_valid());
    REQUIRE(adopted.has_valid_ownership_authorization());
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
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<sketch_type>;
    using device_type = bioseq::gpu::vulkan::vulkan_device_minimizer_storage<sketch_type>;

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    source_type source;
    source.device = std::make_shared<device_type>();
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.arena_trimmed_before_host_materialization = true;
    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    auto admitted = ::bioseq::simd::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_length=*/1'000u);
    const auto& receipt = admitted.receipt;

    REQUIRE(admitted.is_valid());
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
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<sketch_type>;
    using device_type = bioseq::gpu::vulkan::vulkan_device_minimizer_storage<sketch_type>;
    using authority_type = ::bioseq::simd::pipe::
        transient_target_index_release_authorization;

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t cap = 0u;
        ~restore_cap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::common::saturating_add(
            baseline, std::uint64_t{64} << 20u)));

    source_type source;
    source.host_minimizers.reserve(2u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u);
    REQUIRE(source.host_minimizers.size()
            == source.host_minimizers.capacity());
    const std::uint64_t source_host_bytes =
        source.host_minimizers.capacity() * sizeof(sketch_type);
    source.device = std::make_shared<device_type>();
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes = source_host_bytes;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    REQUIRE(adopted.has_valid_ownership_authorization());
    auto admitted = ::bioseq::simd::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_length=*/1'000u);
    const auto& receipt = admitted.receipt;

    REQUIRE(admitted.is_valid());
    REQUIRE(adopted.minimizers.capacity() == 0u);
    REQUIRE(adopted.adoption.adopted_size == 0u);
    REQUIRE(adopted.adoption.host_capacity_bytes == 0u);
    REQUIRE_FALSE(
        adopted.adoption.is_host_permit_released_to_process_footprint);
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

    authority_type authority{
        .index = &admitted.index,
        .permit = &*admitted.index_permit,
        .expected_permit_bytes = receipt.retained_index_permit_bytes,
        .require_exact_permit_bytes = true,
    };
    REQUIRE(authority.is_valid());
    authority_type mismatched_authority = authority;
    ++mismatched_authority.expected_permit_bytes;
    REQUIRE_FALSE(mismatched_authority.is_valid());
    ::bioseq::simd::pipe::post_seed_owned_lifetime lifetime{
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
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using wide_sketch_type =
        ::bioseq::simd::minimizer_representation<40u, 15u>;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<wide_sketch_type>;
    using device_type =
        bioseq::gpu::vulkan::vulkan_device_minimizer_storage<wide_sketch_type>;
    STATIC_REQUIRE(sizeof(wide_sketch_type) == 2u * sizeof(std::uint64_t));

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t cap = 0u;
        ~restore_cap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::common::saturating_add(
            baseline, std::uint64_t{64} << 20u)));

    source_type source;
    source.host_minimizers.reserve(2u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u);
    REQUIRE(source.host_minimizers.size()
            == source.host_minimizers.capacity());
    const std::uint64_t source_host_bytes =
        source.host_minimizers.capacity() * sizeof(wide_sketch_type);
    source.device = std::make_shared<device_type>();
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes = source_host_bytes;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    auto admitted = ::bioseq::simd::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_length=*/1'000u);
    const auto& receipt = admitted.receipt;

    REQUIRE(admitted.is_valid());
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
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<sketch_type>;
    using device_type = bioseq::gpu::vulkan::vulkan_device_minimizer_storage<sketch_type>;
    using index_owner_type = ::bioseq::simd::pipe_entry_detail::
        vulkan_admitted_target_index;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<index_owner_type>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<index_owner_type>);

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t cap = 0u;
        ~restore_cap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::common::saturating_add(
            baseline, std::uint64_t{64} << 20u)));

    constexpr std::size_t minimizer_count = 2u;
    const std::uint64_t planned_host_bytes =
        minimizer_count * sizeof(sketch_type);
    auto host_permit = admission.try_reserve(planned_host_bytes);
    REQUIRE(host_permit.has_value());
    source_type source;
    source.host_permit.emplace(std::move(*host_permit));
    source.host_minimizers.reserve(minimizer_count);
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.host_minimizers.emplace_back(
        /*kmer*/ 9u, /*pos*/ 13u, /*strand*/ 1u);
    const std::uint64_t exact_host_bytes =
        source.host_minimizers.capacity() * sizeof(sketch_type);
    REQUIRE(exact_host_bytes <= planned_host_bytes);
    source.host_permit->shrink_to(exact_host_bytes);
    const sketch_type* const original_data = source.host_minimizers.data();
    source.device = std::make_shared<device_type>();
    std::weak_ptr<device_type> device_lifetime = source.device;
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = false;
    source.host_footprint_reflected_delta_bytes = 0u;
    source.arena_trimmed_before_host_materialization = true;

    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(source));
    REQUIRE(adopted.has_valid_ownership_authorization());
    REQUIRE(adopted.minimizers.data() == original_data);
    REQUIRE(adopted.host_permit.has_value());
    REQUIRE(adopted.host_permit->bytes() == exact_host_bytes);
    REQUIRE(adopted.adoption.retained_host_permit_bytes == exact_host_bytes);
    REQUIRE_FALSE(
        adopted.adoption.is_host_permit_released_to_process_footprint);
    REQUIRE(device_lifetime.expired());
    REQUIRE(admission.reserved_bytes() == baseline + exact_host_bytes);

    auto admitted = ::bioseq::simd::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), /*target_length=*/1'000u);
    const auto& receipt = admitted.receipt;
    REQUIRE(admitted.is_valid());
    REQUIRE(adopted.minimizers.capacity() == 0u);
    REQUIRE_FALSE(adopted.host_permit.has_value());
    REQUIRE(adopted.adoption.adopted_size == 0u);
    REQUIRE(adopted.adoption.host_capacity_bytes == 0u);
    REQUIRE_FALSE(
        adopted.adoption.is_host_permit_released_to_process_footprint);
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
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using source_type = bioseq::gpu::vulkan::vulkan_sketch_result<sketch_type>;
    using device_type = bioseq::gpu::vulkan::vulkan_device_minimizer_storage<sketch_type>;

    source_type source;
    source.host_minimizers.emplace_back(
        /*kmer*/ 7u, /*pos*/ 11u, /*strand*/ 0u);
    source.device = std::make_shared<device_type>();
    const auto device_alias = source.device;
    source.dispatch_token = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token::
        completed_collect_minimizers_roundtrip();
    source.host_ownership_route = bioseq::gpu::vulkan::
        vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption;
    source.host_permit_released_to_process_footprint = true;
    source.host_footprint_reflected_delta_bytes = sizeof(sketch_type);
    source.arena_trimmed_before_host_materialization = true;

    REQUIRE_THROWS_AS(
        ::bioseq::simd::pipe_entry_detail::
            adopt_fixed_vulkan_sketch_host_result(std::move(source)),
        bioseq::gpu::vulkan::vulkan_runtime_error);
    REQUIRE(device_alias != nullptr);
}

TEST_CASE("real sparse dna5 Vulkan target adopts either exact authority and builds admitted index",
          "[component:bioseq][layer:unit][feature:pipeline][resource:gpu]") {
    namespace vk = bioseq::gpu::vulkan;
    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("hardware Vulkan unavailable; real fixed-route handoff skipped");
        return;
    }

    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using packed_sequence_type =
        sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    constexpr std::size_t letter_count = 1'000'000u;
    constexpr std::size_t window_size = 10u;
    constexpr std::size_t kmer_size = 15u;

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t pipeline_cache_metadata_baseline =
        runtime.pipeline_cache_metadata_reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t cap = 0u;
        ~restore_cap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::common::saturating_add(
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
    auto sequence = compact_sequence_range<packed_sequence_type>(letters);
    auto sequence_owner = bioseq::storage::make_resident_lazy_sequence(sequence);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(sequence_owner);

    auto result = vk::collect_minimizers_vulkan_result<sketch_type>(
        sequence_view,
        window_size,
        kmer_size,
        nullptr,
        vk::vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption);
    REQUIRE(result.has_value());
    REQUIRE(result->dispatch_token.is_internally_consistent());
    REQUIRE(result->host_ownership_route
            == vk::vulkan_sketch_host_ownership_route::
                fixed_zero_copy_adoption);
    REQUIRE(result->arena_trimmed_before_host_materialization);
    REQUIRE_FALSE(result->host_minimizers.empty());
    REQUIRE(result->host_minimizers.size() < 10'000u);
    const std::uint64_t host_capacity_bytes =
        result->host_minimizers.capacity() * sizeof(sketch_type);
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
    for (const sketch_type& minimizer : result->host_minimizers) {
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
    const sketch_type* const original_data = result->host_minimizers.data();
    std::weak_ptr<vk::vulkan_device_minimizer_storage<sketch_type>> device_lifetime =
        result->device;

    auto adopted = ::bioseq::simd::pipe_entry_detail::
        adopt_fixed_vulkan_sketch_host_result(std::move(*result));
    REQUIRE(adopted.adoption.is_zero_copy_adoption_valid());
    REQUIRE(adopted.has_valid_ownership_authorization());
    REQUIRE(adopted.minimizers.data() == original_data);
    REQUIRE(device_lifetime.expired());
    // Moving an optional leaves its source engagement unspecified.  If the
    // retained-permit case leaves an engaged source, its memory permit must be
    // moved-from and therefore own neither a controller nor any bytes.
    if (result->host_permit.has_value()) {
        REQUIRE_FALSE(static_cast<bool>(*result->host_permit));
        REQUIRE(result->host_permit->bytes() == 0u);
    }
    REQUIRE(result->host_minimizers.capacity() == 0u);

    auto admitted = ::bioseq::simd::pipe_entry_detail::
        admit_and_build_fixed_vulkan_target_index(
            std::move(adopted), letter_count);
    const auto& handoff = admitted.receipt;
    REQUIRE(admitted.is_valid());
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
    // process-lifetime metadata has its own explicit memory permit and is not
    // part of the adopted host vector or admitted index. Verify that the only
    // reservation surviving this transaction is precisely that cache growth;
    // an unreturned host/device/index permit would make the equality fail.
    const std::uint64_t pipeline_cache_metadata_after =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(pipeline_cache_metadata_after
            >= pipeline_cache_metadata_baseline);
    REQUIRE(admission.reserved_bytes()
            == bioseq::common::saturating_add(
                baseline,
                pipeline_cache_metadata_after
                    - pipeline_cache_metadata_baseline));
}

TEST_CASE("raw sketch batches grow geometrically and nested query scopes release only their tail",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    constexpr std::size_t initial_capacity = 140'000; // > 1 MiB as uint64_t.
    constexpr std::size_t batch_size = 4'096;
    constexpr std::size_t batch_count = 64;
    constexpr std::size_t emitted_count = batch_size * batch_count;

    context::context ctx;
    context::installed_context_scope installed{ctx};
    REQUIRE(ctx.chain_heap_fallback_bytes() == 0);

    std::vector<raw_sketch_append_fixture> survivor;
    {
        auto outer_scope = ctx.push<context::arena::chain>();
        auto outer_slab =
            ::bioseq::chaining::make_arena_slab<raw_sketch_append_fixture>();
        outer_slab.reserve(initial_capacity);
        outer_slab.resize(1);
        outer_slab.data()[0].value = 0xfeedfacecafebeefULL;
        const std::size_t outer_bytes =
            initial_capacity * sizeof(raw_sketch_append_fixture);
        REQUIRE(ctx.chain_heap_fallback_bytes() == outer_bytes);

        // Repeat the faithful under-estimated raw-batch append shape. Before
        // geometric growth this performed one exact allocation and full-prefix
        // copy for every batch after `initial_capacity` was crossed.
        for (std::size_t round = 0; round < 3; ++round) {
            {
                auto query_scope = ctx.push<context::arena::chain>();
                auto sketches =
                    ::bioseq::chaining::make_arena_slab<raw_sketch_append_fixture>();
                sketches.reserve(initial_capacity);
                ::bioseq::simd::pipe::
                    sketch_collector_arena_adapter<raw_sketch_append_fixture>
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
                    * sizeof(raw_sketch_append_fixture);
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

using bioseq::pipeline_stats::executed_backend;
using bioseq::pipeline_stats::execution_engine;
using bioseq::pipeline_stats::execution_environment_snapshot;
using bioseq::pipeline_stats::optional_observation_bool;
using bioseq::pipeline_stats::pipeline_cache_policy;
using bioseq::pipeline_stats::pipeline_execution_stats;
using bioseq::pipeline_stats::pipeline_job_execution_stats;
using bioseq::pipeline_stats::pipeline_stage;
using bioseq::pipeline_stats::simd_alignment_algorithm_identity;
using bioseq::pipeline_stats::simd_chaining_policy_identity;
using bioseq::pipeline_stats::target_index_source;

[[nodiscard]] execution_environment_snapshot make_cpu_environment(
    std::uint32_t workers = 2) {
    execution_environment_snapshot environment{};
    environment.effective_cpu_workers = workers;
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    environment.record_highway_target(
        fixed_highway_dispatch_plan(), binding.target);
    return environment;
}

[[nodiscard]] execution_environment_snapshot make_gpu_environment(
    std::uint32_t workers = 2) {
    execution_environment_snapshot environment{};
    environment.effective_cpu_workers = workers;
    environment.is_gpu_environment_available = true;
    environment.gpu_device_uuid[0] = 0x42;
    environment.gpu_driver_identity[0] = 'v';
    environment.gpu_driver_identity[1] = 'k';
    environment.gpu_pipeline_cache_policy =
        pipeline_cache_policy::immutable_preseed_all_hit;
    environment.gpu_pipeline_cache_seed_sha256[0] = 0x91;
    environment.gpu_pipeline_cache_seed_size = 64;
    environment.gpu_pipeline_cache_manifest_checksum = 0x1234;
    environment.gpu_pipeline_cache_hit = optional_observation_bool::true_value;
    const auto& binding =
        ::bioseq::simd_dispatch::highway_test_target_binding();
    environment.record_highway_target(
        fixed_highway_dispatch_plan(), binding.target);
    return environment;
}

[[nodiscard]] pipeline_job_execution_stats make_attested_job(
    executed_backend backend,
    target_index_source source) {
    pipeline_job_execution_stats job{};
    job.requested_backend = backend;
    job.actual_backend = backend;
    if (backend == executed_backend::simd_cpu) {
        job.requested_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        job.observed_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        job.requested_simd_alignment_algorithm =
            simd_alignment_algorithm_identity::block_aligner;
        job.observed_simd_alignment_algorithm =
            simd_alignment_algorithm_identity::block_aligner;
    }
    job.requested_target_index_source = source;
    job.actual_target_index_source = source;
    job.environment = backend == executed_backend::vulkan_gpu
        ? make_gpu_environment()
        : make_cpu_environment();
    if (source == target_index_source::persisted) {
        job.persisted_generation_id = 17;
        job.persisted_source_identity = 0xabc;
    }
    for (std::size_t i = 0; i < bioseq::pipeline_stats::k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        const execution_engine engine = bioseq::pipeline_stats::expected_execution_engine(
            backend, source, stage);
        if (engine == execution_engine::count) continue;
        auto& coverage = job.stages.coverage[i];
        coverage.eligible_jobs = 1;
        coverage.executed_jobs[bioseq::pipeline_stats::engine_index(engine)] = 1;
    }
    return job;
}

[[nodiscard]] pipeline_job_execution_stats make_zero_match_job(
    executed_backend backend,
    target_index_source source) {
    auto job = make_attested_job(backend, source);
    for (std::size_t i = 0; i < bioseq::pipeline_stats::k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        if (!bioseq::pipeline_stats::is_front_end_eligibility_required(source, stage)) {
            job.stages.coverage[i] = {};
        }
    }
    return job;
}

} // namespace

TEST_CASE("canonical pipeline stages have stable ordinals and names",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::pipeline_stats::pipeline_stage_name;
    static_assert(static_cast<std::uint8_t>(pipeline_stage::query_sketch) == 0);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::target_index_build) == 2);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::chain_backtrack) == 8);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::target_window_io) == 14);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::right_extension) == 19);
    static_assert(static_cast<std::uint8_t>(pipeline_stage::count) == 20);
    static_assert(bioseq::pipeline_stats::k_pipeline_stage_count == 20);
    static_assert(std::is_trivially_copyable_v<
        bioseq::pipeline_stats::pipeline_execution_stats>);
    static_assert(std::is_standard_layout_v<
        bioseq::pipeline_stats::pipeline_execution_stats>);

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
        REQUIRE(pipeline_stage_name(static_cast<pipeline_stage>(i)) == expected_names[i]);
    }
}

TEST_CASE("unbound pipeline description owns exact Highway kernels by stage",
          "[component:bioseq][layer:unit][feature:planner][resource:highway]") {
    using bioseq::simd::base_alignment_algorithm;
    using bioseq::simd::chaining_algorithm;
    using bioseq::simd::minimap2_parameters;
    using bioseq::simd::minimizer_algorithm;
    using bioseq::simd::pipe::query_sketch_route;
    using bioseq::simd_dispatch::highway_kernel;
    using bioseq::simd_dispatch::highway_kernel_mask_for;

    minimap2_parameters params{};
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* alphabet_identity = table.is_complete()
        ? table.resolve_alphabet_identity<::seq::dna4>() : nullptr;
    REQUIRE(alphabet_identity != nullptr);
    params.alphabet_code = alphabet_identity->alphabet_code;
    params.minimizer_algorithm =
        minimizer_algorithm::bounded_nthash_simd;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.prefilter_enabled = true;
    params.prefilter_backend =
        bioseq::prefilter::prefilter_backend::simd;
    params.base_alignment_algorithm =
        base_alignment_algorithm::ksw2_batched;
    params.do_cigar = true;
    REQUIRE(params.highway_kernels == nullptr);

    const auto description = bioseq::simd::pipe::describe_pipeline_for<
        ::seq::dna4>(
        params, params.alphabet_code,
        query_sketch_route::recompute_capable);
    REQUIRE(bioseq::simd::pipe::query_sketch_stage_owner_count(
                description) == 1u);

    const auto stage_kernels = [&description](const std::string_view name) {
        const auto found = std::ranges::find(
            description.stages, name,
            &bioseq::simd::pipe::stage_description::stage_name);
        REQUIRE(found != description.stages.end());
        REQUIRE(found->algorithm.has_value());
        return found->algorithm->highway_kernels;
    };
    REQUIRE(stage_kernels("QuerySketchStage") == highway_kernel_mask_for(
        highway_kernel::minimizer_bounded_nthash));
    REQUIRE(stage_kernels("chain_stage") == highway_kernel_mask_for(
        highway_kernel::chaining_chain_anchors_simd));
    REQUIRE(stage_kernels("prefilter_stage") == highway_kernel_mask_for(
        highway_kernel::prefilter_sneaky_snake_simd_batch));
    auto alignment_kernels = highway_kernel_mask_for(
        highway_kernel::alignment_ksw2_batched);
    alignment_kernels |= highway_kernel_mask_for(
        highway_kernel::alignment_ksw2);
    REQUIRE(stage_kernels("align_stage") == alignment_kernels);
}

TEST_CASE("all-GPU bounded CIGAR description advertises no Highway kernel",
          "[component:bioseq][layer:unit][feature:planner][resource:highway]") {
    using bioseq::simd::base_alignment_algorithm;
    using bioseq::simd::chaining_algorithm;
    using bioseq::simd::minimap2_parameters;
    using bioseq::simd::minimizer_algorithm;
    using bioseq::simd::pipe::query_sketch_route;

    minimap2_parameters params{};
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* alphabet_identity = table.is_complete()
        ? table.resolve_alphabet_identity<::seq::dna4>() : nullptr;
    REQUIRE(alphabet_identity != nullptr);
    params.alphabet_code = alphabet_identity->alphabet_code;
    params.kmer_size = 15;
    params.window_size = 10;
    params.minimizer_algorithm =
        minimizer_algorithm::vulkan_minimap_integer;
    params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    params.base_alignment_algorithm =
        base_alignment_algorithm::block_aligner;
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.do_cigar = true;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.compute_edit_distance = false;
    params.exact_edit_filter_enabled = false;
    params.prefilter_enabled = false;
    params.max_occ = 500;

    REQUIRE(bioseq::simd::pipe::complete_all_gpu_tuple_selected(params));
    REQUIRE(params.highway_kernels == nullptr);

    const auto description = bioseq::simd::pipe::describe_pipeline_for<
        ::seq::dna4>(
        params, params.alphabet_code,
        query_sketch_route::recompute_capable);
    const auto align_stage = std::ranges::find(
        description.stages, "align_stage",
        &bioseq::simd::pipe::stage_description::stage_name);
    REQUIRE(align_stage != description.stages.end());
    REQUIRE(align_stage->algorithm.has_value());
    REQUIRE(align_stage->algorithm->highway_kernels.none());
    REQUIRE(std::ranges::all_of(
        description.stages,
        [](const bioseq::simd::pipe::stage_description& stage) {
            return !stage.algorithm.has_value()
                || stage.algorithm->highway_kernels.none();
        }));
}

TEST_CASE("persisted grouped-SoA publication rejects burned fully-consumed stats",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    using counts_type = persisted_grouped_soa_coordinate_publication_stats;

    constexpr counts_type accepted{
        .expected_jobs = 2,
        .validated_jobs = 2,
        .consumed_jobs = 2,
        .accepted_jobs = 2,
        .burned_jobs = 0,
        .pending_jobs = 0,
        .staged_jobs = 2,
    };
    static_assert(is_persisted_grouped_soa_coordinate_publication_consistent(
        accepted));
    REQUIRE(is_persisted_grouped_soa_coordinate_publication_consistent(accepted));

    /* This fail-open fixture has consume() mark both records
     * consumed even though the second expectation mismatched and burned. */
    constexpr counts_type burned_but_fully_consumed{
        .expected_jobs = 2,
        .validated_jobs = 2,
        .consumed_jobs = 2,
        .accepted_jobs = 1,
        .burned_jobs = 1,
        .pending_jobs = 0,
        .staged_jobs = 1,
    };
    static_assert(!is_persisted_grouped_soa_coordinate_publication_consistent(
        burned_but_fully_consumed));
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_publication_consistent(
        burned_but_fully_consumed));

    const auto rejected = [](counts_type counts) {
        REQUIRE_FALSE(
            is_persisted_grouped_soa_coordinate_publication_consistent(counts));
    };
    auto malformed = accepted;
    malformed.expected_jobs = 0;
    rejected(malformed);
    malformed = accepted;
    malformed.validated_jobs = 1;
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

TEST_CASE("persisted grouped-SoA environment binding maps without inference",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    namespace gv = bioseq::gpu::vulkan;
    using namespace bioseq::pipeline_stats;
    gv::runtime_gpu_execution_environment_snapshot binding{};
    binding.gpu_environment_present = true;
    binding.gpu_device_uuid[0] = 0x61u;
    binding.gpu_driver_identity[0] = 'v';
    binding.gpu_driver_identity[1] = 'k';
    binding.cache_policy = gv::runtime_pipeline_cache_policy::
        immutable_preseed_all_hit;
    binding.seed_sha256[0] = 0xa5u;
    binding.seed_size = 4096u;
    binding.manifest_checksum = 0x12345678u;
    binding.pipeline_cache_all_hit = true;
    REQUIRE(binding.is_internally_consistent());
    static_assert(noexcept(make_persisted_grouped_soa_gpu_environment_snapshot(
        binding, 4u)));

    const auto mapped = make_persisted_grouped_soa_gpu_environment_snapshot(
        binding, 4u);
    REQUIRE(mapped.has_value());
    REQUIRE(mapped->effective_cpu_workers == 4u);
    REQUIRE(mapped->is_gpu_environment_available);
    REQUIRE(mapped->gpu_device_uuid == binding.gpu_device_uuid);
    REQUIRE(mapped->gpu_driver_identity == binding.gpu_driver_identity);
    REQUIRE(mapped->gpu_pipeline_cache_policy
            == pipeline_cache_policy::immutable_preseed_all_hit);
    REQUIRE(mapped->gpu_pipeline_cache_seed_sha256 == binding.seed_sha256);
    REQUIRE(mapped->gpu_pipeline_cache_seed_size == binding.seed_size);
    REQUIRE(mapped->gpu_pipeline_cache_manifest_checksum
            == binding.manifest_checksum);
    REQUIRE(mapped->gpu_pipeline_cache_hit == optional_observation_bool::true_value);
    REQUIRE(is_environment_valid_for_backend(
        *mapped, executed_backend::vulkan_gpu));
    REQUIRE(is_persisted_grouped_soa_gpu_environment_consistent(
        binding, binding));
    auto drifted_binding = binding;
    drifted_binding.gpu_driver_identity[1] = 'x';
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_environment_consistent(
        drifted_binding, binding));

    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        binding, 0u).has_value());
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        binding,
        static_cast<std::size_t>(k_sql_integer_value_max) + 1u).has_value());
    auto invalid = binding;
    invalid.gpu_device_uuid = {};
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.cache_policy = gv::runtime_pipeline_cache_policy::invalid;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.pipeline_cache_all_hit = false;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());
    invalid = binding;
    invalid.seed_size = k_sql_bigint_value_max + 1u;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_environment_snapshot(
        invalid, 4u).has_value());

    gv::runtime_gpu_cache_metrics_audit cache_metrics{
        .token_identity = 0x999u,
        .gpu_pipeline_cache_loads = 1u,
        .gpu_pipeline_cache_seed_bytes = binding.seed_size,
        .gpu_pipeline_cache_load_ns = 37u,
        .gpu_pipeline_creates = 11u,
        .gpu_pipeline_creation_ns = 91u,
        .saturated = false,
    };
    static_assert(noexcept(
        make_persisted_grouped_soa_gpu_cache_metrics_delta(cache_metrics)));
    const auto cache_delta =
        make_persisted_grouped_soa_gpu_cache_metrics_delta(cache_metrics);
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
    REQUIRE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        cache_metrics, cache_metrics_preview));
    auto out_of_bounds = cache_metrics;
    out_of_bounds.gpu_pipeline_creates = 16u;
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        out_of_bounds, cache_metrics_preview));
    auto wrong_token = cache_metrics;
    wrong_token.token_identity += 1u;
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        wrong_token, cache_metrics_preview));
    const gv::runtime_gpu_cache_metrics_audit already_reported{
        .token_identity = 0x1000u,
    };
    REQUIRE(already_reported.is_internally_consistent());
    const auto zero_delta =
        make_persisted_grouped_soa_gpu_cache_metrics_delta(already_reported);
    REQUIRE(zero_delta.has_value());
    REQUIRE(zero_delta->gpu_pipeline_cache_loads == 0u);
    REQUIRE(zero_delta->gpu_pipeline_creates == 0u);
    const gv::runtime_gpu_cache_metrics_audit zero_for_same_operation{
        .token_identity = cache_metrics.token_identity,
    };
    REQUIRE(is_persisted_grouped_soa_gpu_cache_metrics_bounded_by(
        zero_for_same_operation, cache_metrics_preview));
    cache_metrics.saturated = true;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_cache_metrics_delta(
        cache_metrics).has_value());
    cache_metrics.saturated = false;
    cache_metrics.token_identity = 0u;
    REQUIRE_FALSE(make_persisted_grouped_soa_gpu_cache_metrics_delta(
        cache_metrics).has_value());
}

TEST_CASE("persisted grouped-SoA prospective publication is exact across waves",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto make_wave = [](const std::size_t jobs,
                              const std::uint64_t first_query) {
        pipeline_execution_stats wave{};
        for (std::size_t i = 0; i < jobs; ++i) {
            auto job = make_attested_job(
                executed_backend::vulkan_gpu,
                target_index_source::persisted);
            job.logical_target_row = 73;
            job.query_ordinal = first_query + i;
            wave.merge_job(job);
        }
        return wave;
    };

    for (const std::size_t jobs : {std::size_t{1}, std::size_t{8}}) {
        CAPTURE(jobs);
        const pipeline_execution_stats before{};
        const auto staged = make_wave(jobs, 100);
        auto prospective = before;
        prospective.merge(staged);
        REQUIRE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
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
    pipeline_execution_stats cache_metrics_prospective{};
    cache_metrics_prospective.merge(staged_with_cache_metrics);
    REQUIRE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        pipeline_execution_stats{},
        staged_with_cache_metrics,
        cache_metrics_prospective,
        1u));

    const auto before = make_wave(1, 200);
    const auto staged = make_wave(1, 201);
    auto prospective = before;
    prospective.merge(staged);
    REQUIRE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, staged, prospective, 1));

    auto drifted = staged;
    drifted.environment.effective_cpu_workers += 1;
    auto drifted_prospective = before;
    drifted_prospective.merge(drifted);
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, drifted, drifted_prospective, 1));

    auto wrong_generation = staged;
    wrong_generation.persisted_generation_id += 1;
    auto generation_prospective = before;
    generation_prospective.merge(wrong_generation);
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, wrong_generation, generation_prospective, 1));

    auto unattested = staged;
    unattested.fully_attested_alignment_jobs = 0;
    unattested.unattested_alignment_jobs = 1;
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, unattested, prospective, 1));
    REQUIRE_FALSE(is_persisted_grouped_soa_coordinate_prospective_stats_consistent(
        before, staged, prospective, 0));
}

TEST_CASE("persisted GPU SQL accumulation is admitted, causal, and immutable across waves",
          "[component:bioseq][layer:unit][feature:sql_api]") {
    using namespace bioseq::pipeline_stats;
    static_assert(!std::is_copy_constructible_v<
        persisted_gpu_operation_accumulator>);
    static_assert(std::is_nothrow_move_constructible_v<
        persisted_gpu_operation_accumulator>);
    static_assert(noexcept(std::declval<
        persisted_gpu_operation_chunk&>().publish_cache_metrics(
            std::declval<const persisted_gpu_cache_metrics&>())));

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0u;
        ~restore_cap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 23u)));
    constexpr std::uint64_t two_job_chunk_retained_bytes =
        sizeof(persisted_gpu_operation_chunk)
        + 2u * sizeof(persisted_gpu_job_stats);

    REQUIRE(persisted_gpu_operation_chunk::prepare(0u) == nullptr);
    REQUIRE(persisted_gpu_operation_chunk::prepare(
        static_cast<std::size_t>(
            k_persisted_gpu_operation_capacity + 1u)) == nullptr);

    const auto make_chunk = [](
            const std::uint64_t shape_operation_identity,
            const std::uint64_t query_operation_identity,
            const std::uint64_t first_logical_query) {
        auto chunk = persisted_gpu_operation_chunk::prepare(2u);
        if (chunk == nullptr) return chunk;
        persisted_gpu_environment_snapshot environment{};
        environment.device_uuid[0] = 0x41u;
        environment.api_version = 0x00402000u;
        environment.driver_version = 0x01020304u;
        environment.vendor_id = 0x106bu;
        environment.device_id = 0x0011u;
        environment.pipeline_cache_uuid[0] = 0x52u;
        environment.seed_sha256[0] = 0x63u;
        environment.seed_size = 4096u;
        environment.manifest_checksum = 0x7777u;
        persisted_gpu_cache_validation_result qualification{};
        qualification.qualification_wire_revision = 2u;
        qualification.identity = 0x7171u;
        qualification.archive_count = 2u;
        qualification.archive_bytes = 4096u;
        qualification.archive_checksum = 0x7272u;
        qualification.receipt_count = 2u;
        qualification.hash_passes = 1u;
        qualification.hashed_bytes = 4096u;
        qualification.private_files_created = 2u;
        qualification.private_files_unlinked = 2u;
        qualification.helper_wire_revision = 2u;
        constexpr std::string_view helper_build =
            "bioseq_gpu_archive_builder";
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
        REQUIRE(qualification.is_internally_consistent());
        chunk->operation() = persisted_gpu_operation_stats{
            .operation_identity = shape_operation_identity,
            .publication = {
                .expected_jobs = 2u,
                .validated_jobs = 2u,
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
            job = persisted_gpu_job_stats{
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
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 10u, 0x5010u},
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 11u, 0x5011u},
                    persisted_gpu_submission_stats{
                        shape_operation_identity, 12u, 0x5012u},
                    persisted_gpu_submission_stats{
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
                    .is_valid = true,
                },
            };
        }
        return chunk;
    };
    const auto publish_final_cache = [](
            persisted_gpu_operation_chunk& chunk) {
        const persisted_gpu_cache_metrics final_metrics{
            .token_identity = chunk.operation().operation_identity,
        };
        REQUIRE(final_metrics.is_internally_consistent());
        chunk.publish_cache_metrics(final_metrics);
        REQUIRE(chunk.has_published_cache_metrics());
        REQUIRE(chunk.is_sealed());
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
        qualification.is_saturated = true;
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
        persisted_gpu_operation_accumulator state{};
        auto first = make_chunk(0x1000u, 0x2000u, 30u);
        REQUIRE(first != nullptr);
        REQUIRE(first->seal());
        auto first_append = state.prepare_append(first);
        REQUIRE(first_append.is_valid());
        publish_final_cache(*first);
        state.commit_append(std::move(first_append), std::move(first));

        /* A fully drained rescan may repeat the logical rows in a new
         * structured operation domain. */
        auto second = make_chunk(0x1001u, 0x2001u, 30u);
        REQUIRE(second != nullptr);
        REQUIRE(second->seal());
        auto second_append = state.prepare_append(second);
        REQUIRE(second_append.is_valid());
        publish_final_cache(*second);
        state.commit_append(std::move(second_append), std::move(second));
        state.freeze();
        REQUIRE(state.is_complete());
        REQUIRE(state.is_internally_consistent());
        REQUIRE(state.operation_count() == 2u);
        REQUIRE(state.job_count() == 4u);

        persisted_gpu_operation_accumulator frozen{std::move(state)};
        REQUIRE(frozen.is_complete());
        std::vector<std::uint64_t> first_read;
        std::vector<std::uint64_t> second_read;
        for (const persisted_gpu_operation_chunk* chunk = frozen.head();
             chunk != nullptr; chunk = chunk->next()) {
            first_read.push_back(chunk->operation().operation_identity);
        }
        for (const persisted_gpu_operation_chunk* chunk = frozen.head();
             chunk != nullptr; chunk = chunk->next()) {
            second_read.push_back(chunk->operation().operation_identity);
        }
        REQUIRE(first_read == std::vector<std::uint64_t>{0x1000u, 0x1001u});
        REQUIRE(second_read == first_read);
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    const auto require_cross_operation_rejection = [&make_chunk,
            &publish_final_cache](const auto& mutate) {
        persisted_gpu_operation_accumulator state{};
        auto first = make_chunk(0x1100u, 0x2100u, 40u);
        REQUIRE(first != nullptr);
        REQUIRE(first->seal());
        auto first_append = state.prepare_append(first);
        REQUIRE(first_append.is_valid());
        publish_final_cache(*first);
        state.commit_append(std::move(first_append), std::move(first));

        auto second = make_chunk(0x1101u, 0x2101u, 41u);
        REQUIRE(second != nullptr);
        mutate(second->operation());
        REQUIRE(second->seal());
        REQUIRE_FALSE(state.prepare_append(second).is_valid());
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

TEST_CASE("measured modes match the literal exclusive-engine matrix",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    using execution_engine_type = execution_engine;
    constexpr execution_engine_type forbidden = execution_engine_type::count;
    constexpr std::array<execution_engine_type, 20> simd_recomputed{
        execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::scalar_cpu, forbidden, forbidden,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::scalar_cpu,
        execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::host_io,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu,
    };
    constexpr std::array<execution_engine_type, 20> simd_persisted{
        execution_engine_type::simd_cpu, forbidden, forbidden, execution_engine_type::host_io, execution_engine_type::scalar_cpu,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::scalar_cpu,
        execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::scalar_cpu, execution_engine_type::host_io,
        forbidden, execution_engine_type::scalar_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu, execution_engine_type::simd_cpu,
    };
    constexpr std::array<execution_engine_type, 20> gpu_recomputed{
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, forbidden, forbidden,
        forbidden, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::host_io,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
    };
    constexpr std::array<execution_engine_type, 20> gpu_persisted{
        execution_engine_type::vulkan_gpu, forbidden, forbidden, execution_engine_type::host_io, execution_engine_type::vulkan_gpu,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::host_io,
        execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu, execution_engine_type::vulkan_gpu,
    };
    const auto require_matrix = [](
        executed_backend backend,
        target_index_source source,
        const std::array<execution_engine_type, 20>& expected) {
        for (std::size_t i = 0; i < expected.size(); ++i) {
            REQUIRE(expected_execution_engine(
                        backend, source, static_cast<pipeline_stage>(i))
                    == expected[i]);
        }
    };
    require_matrix(
        executed_backend::simd_cpu, target_index_source::recomputed, simd_recomputed);
    require_matrix(
        executed_backend::simd_cpu, target_index_source::persisted, simd_persisted);
    require_matrix(
        executed_backend::vulkan_gpu, target_index_source::recomputed, gpu_recomputed);
    require_matrix(
        executed_backend::vulkan_gpu, target_index_source::persisted, gpu_persisted);

    constexpr std::array<bool, 20> recomputed_required{
        true, true, true, false, false, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false,
    };
    constexpr std::array<bool, 20> persisted_required{
        true, false, false, true, true, false, false, false, false, false,
        false, false, false, false, false, false, false, false, false, false,
    };
    for (std::size_t i = 0; i < k_pipeline_stage_count; ++i) {
        const auto stage = static_cast<pipeline_stage>(i);
        REQUIRE(is_front_end_eligibility_required(
                    target_index_source::recomputed, stage)
                == recomputed_required[i]);
        REQUIRE(is_front_end_eligibility_required(
                    target_index_source::persisted, stage)
                == persisted_required[i]);
    }
}

TEST_CASE("stats merges saturate and identities fail closed",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    stage_coverage coverage{};
    coverage.eligible_jobs = k_sql_bigint_value_max;
    coverage.executed_jobs[engine_index(execution_engine::simd_cpu)] =
        k_sql_bigint_value_max;
    stage_coverage increment{};
    increment.eligible_jobs = 1;
    increment.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE(coverage.merge(increment));
    REQUIRE(coverage.eligible_jobs == k_sql_bigint_value_max);
    REQUIRE(coverage.executed_jobs[engine_index(execution_engine::simd_cpu)]
            == k_sql_bigint_value_max);

    std::uint64_t exact_boundary = k_sql_bigint_value_max - 1;
    REQUIRE_FALSE(merge_counter(exact_boundary, 1));
    REQUIRE(exact_boundary == k_sql_bigint_value_max);
    std::uint64_t oversized = (std::numeric_limits<std::uint64_t>::max)();
    REQUIRE(merge_counter(oversized, 0));
    REQUIRE(oversized == k_sql_bigint_value_max);

    pipeline_operational_stats stage_timing_delta{};
    stage_timing_delta.timing[
        stage_index(pipeline_stage::query_sketch)].host_wall_ns = 7u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::seed_lookup)].host_wall_ns = 11u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::chain_score)].host_wall_ns = 13u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::region_generation)].host_wall_ns = 17u;
    stage_timing_delta.timing[
        stage_index(pipeline_stage::left_extension)].host_wall_ns = 19u;
    pipeline_operational_stats stage_timing_aggregate{};
    stage_timing_aggregate.merge(stage_timing_delta);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::query_sketch)].host_wall_ns == 7u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::seed_lookup)].host_wall_ns == 11u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::chain_score)].host_wall_ns == 13u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::region_generation)].host_wall_ns == 17u);
    REQUIRE(stage_timing_aggregate.timing[
        stage_index(pipeline_stage::left_extension)].host_wall_ns == 19u);
    REQUIRE_FALSE(stage_timing_aggregate.are_stats_saturated);

    pipeline_operational_stats operational{};
    operational.target_rows = k_sql_bigint_value_max;
    pipeline_operational_stats operational_increment{};
    operational_increment.target_rows = 1;
    operational.merge(operational_increment);
    REQUIRE(operational.target_rows == k_sql_bigint_value_max);
    REQUIRE(operational.are_stats_saturated);

    REQUIRE(merge_backend_identity(executed_backend::none, executed_backend::simd_cpu)
            == executed_backend::simd_cpu);
    REQUIRE(merge_backend_identity(executed_backend::simd_cpu, executed_backend::simd_cpu)
            == executed_backend::simd_cpu);
    REQUIRE(merge_backend_identity(executed_backend::simd_cpu, executed_backend::vulkan_gpu)
            == executed_backend::mixed_invalid);
    REQUIRE(merge_target_index_identity(target_index_source::recomputed,
                                        target_index_source::persisted)
            == target_index_source::mixed_invalid);
    REQUIRE(merge_simd_chaining_policy_identity(
                simd_chaining_policy_identity::none,
                simd_chaining_policy_identity::bounded_batch_approximate)
            == simd_chaining_policy_identity::bounded_batch_approximate);
    REQUIRE(merge_simd_chaining_policy_identity(
                simd_chaining_policy_identity::exact_visited,
                simd_chaining_policy_identity::bounded_batch_approximate)
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(merge_simd_alignment_algorithm_identity(
                simd_alignment_algorithm_identity::none,
                simd_alignment_algorithm_identity::block_aligner)
            == simd_alignment_algorithm_identity::block_aligner);
    REQUIRE(merge_simd_alignment_algorithm_identity(
                simd_alignment_algorithm_identity::block_aligner,
                simd_alignment_algorithm_identity::mixed_invalid)
            == simd_alignment_algorithm_identity::mixed_invalid);

    execution_environment_snapshot merged{};
    const execution_environment_snapshot cpu = make_cpu_environment(3);
    merged.merge(cpu);
    REQUIRE_FALSE(merged.is_mixed_invalid);
    merged.merge(cpu);
    REQUIRE_FALSE(merged.is_mixed_invalid);
    merged.merge(make_cpu_environment(4));
    REQUIRE(merged.is_mixed_invalid);

    execution_environment_snapshot missing{};
    missing.merge(execution_environment_snapshot{});
    REQUIRE(missing.is_mixed_invalid);
}

TEST_CASE("Highway execution stats merges preserve exact target observations",
          "[component:bioseq][layer:unit][feature:pipeline][resource:highway]") {
    constexpr std::int64_t target_a = std::int64_t{1} << 20u;
    constexpr std::int64_t target_b = std::int64_t{1} << 21u;
    constexpr std::int64_t both_targets = target_a | target_b;

    const auto make_plan = [](
            const std::int64_t compiled,
            const std::int64_t supported,
            const std::int64_t planned) {
        return ::bioseq::simd_dispatch::highway_dispatch_plan{
            .compiled_targets = compiled,
            .cpu_supported_targets = supported,
            .runnable_targets = compiled & supported,
            .planned_target = planned,
        };
    };
    const auto plan_a = make_plan(both_targets, both_targets, target_a);
    const auto plan_b = make_plan(both_targets, both_targets, target_b);
    const auto plan_a_only = make_plan(target_a, target_a, target_a);

    const auto make_stats = [](
            const ::bioseq::simd_dispatch::highway_dispatch_plan& plan,
            const std::int64_t executed_target,
            const std::uint64_t target_row) {
        auto job = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        job.logical_target_row = target_row;
        job.query_ordinal = 0;
        job.environment = {};
        job.environment.effective_cpu_workers = 2;
        job.environment.record_highway_target(plan, executed_target);
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    SECTION("the canonical empty aggregate is a two-sided identity") {
        const auto stats = make_stats(plan_a, target_a, 1);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(stats));

        pipeline_execution_stats empty_on_left{};
        empty_on_left.merge(stats);
        REQUIRE(empty_on_left.environment.highway_compiled_targets
                == both_targets);
        REQUIRE(empty_on_left.environment.highway_cpu_supported_targets
                == both_targets);
        REQUIRE(empty_on_left.environment.highway_runnable_targets
                == both_targets);
        REQUIRE(empty_on_left.environment.highway_executed_target
                == target_a);
        REQUIRE_FALSE(
            empty_on_left.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(empty_on_left.environment.is_mixed_invalid);
        REQUIRE(empty_on_left.attempted_alignment_jobs == 1);
        REQUIRE(empty_on_left.fully_attested_alignment_jobs == 1);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(
            empty_on_left));

        auto empty_on_right = stats;
        empty_on_right.merge(pipeline_execution_stats{});
        REQUIRE(empty_on_right.environment.highway_compiled_targets
                == both_targets);
        REQUIRE(empty_on_right.environment.highway_executed_target
                == target_a);
        REQUIRE_FALSE(
            empty_on_right.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(empty_on_right.environment.is_mixed_invalid);
        REQUIRE(empty_on_right.attempted_alignment_jobs == 1);
        REQUIRE(empty_on_right.fully_attested_alignment_jobs == 1);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(
            empty_on_right));
    }

    SECTION("equal masks and executed targets remain singular") {
        auto merged = make_stats(plan_a, target_a, 2);
        merged.merge(make_stats(plan_a, target_a, 3));
        REQUIRE(merged.environment.highway_compiled_targets == both_targets);
        REQUIRE(merged.environment.highway_cpu_supported_targets
                == both_targets);
        REQUIRE(merged.environment.highway_runnable_targets == both_targets);
        REQUIRE(merged.environment.highway_executed_target == target_a);
        REQUIRE_FALSE(merged.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(merged.environment.is_mixed_invalid);
        REQUIRE(merged.attempted_alignment_jobs == 2);
        REQUIRE(merged.fully_attested_alignment_jobs == 2);
        REQUIRE(merged.unattested_alignment_jobs == 0);
        REQUIRE(pipeline_execution_stats::can_retain_full_attestation(merged));
    }

    SECTION("different targets become mixed and mixed is permanent") {
        auto mixed = make_stats(plan_a, target_a, 4);
        mixed.merge(make_stats(plan_b, target_b, 5));
        REQUIRE(mixed.environment.highway_compiled_targets == both_targets);
        REQUIRE(mixed.environment.highway_runnable_targets == both_targets);
        REQUIRE(mixed.environment.highway_executed_target == 0);
        REQUIRE(mixed.environment.is_highway_executed_target_mixed);
        REQUIRE_FALSE(mixed.environment.is_mixed_invalid);
        REQUIRE(mixed.attempted_alignment_jobs == 2);
        REQUIRE(mixed.fully_attested_alignment_jobs == 0);
        REQUIRE(mixed.unattested_alignment_jobs == 2);
        REQUIRE(mixed.first_unattested_target_row == 4);
        REQUIRE(mixed.first_unattested_query_ordinal == 0);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            mixed));

        mixed.merge(make_stats(plan_a, target_a, 6));
        REQUIRE(mixed.environment.highway_executed_target == 0);
        REQUIRE(mixed.environment.is_highway_executed_target_mixed);
        REQUIRE(mixed.environment.is_mixed_invalid);
        REQUIRE(mixed.attempted_alignment_jobs == 3);
        REQUIRE(mixed.fully_attested_alignment_jobs == 0);
        REQUIRE(mixed.unattested_alignment_jobs == 3);
    }

    SECTION("mask drift is an invariant failure") {
        auto drift = make_stats(plan_a, target_a, 7);
        drift.merge(make_stats(plan_a_only, target_a, 8));
        REQUIRE(drift.environment.is_mixed_invalid);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            drift));
        REQUIRE(drift.attempted_alignment_jobs == 2);
        REQUIRE(drift.fully_attested_alignment_jobs == 0);
        REQUIRE(drift.unattested_alignment_jobs == 2);
    }

    SECTION("a malformed recorded target is never attested") {
        const auto malformed = make_stats(plan_a, target_b, 9);
        REQUIRE(malformed.environment.is_mixed_invalid);
        REQUIRE(malformed.environment.highway_executed_target == 0);
        REQUIRE_FALSE(execution_environment_snapshot::has_valid_highway_fields(
            malformed.environment));
        REQUIRE(malformed.attempted_alignment_jobs == 1);
        REQUIRE(malformed.fully_attested_alignment_jobs == 0);
        REQUIRE(malformed.unattested_alignment_jobs == 1);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
    }
}

TEST_CASE("pipeline stats retain policy identity and distinct vector activity",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    auto bounded = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    REQUIRE(is_job_execution_stats_valid(bounded));

    auto mismatched = bounded;
    mismatched.observed_simd_chaining_policy =
        simd_chaining_policy_identity::exact_visited;
    REQUIRE_FALSE(is_job_execution_stats_valid(mismatched));

    auto missing_block_identity = bounded;
    missing_block_identity.observed_simd_alignment_algorithm =
        simd_alignment_algorithm_identity::none;
    REQUIRE_FALSE(is_job_execution_stats_valid(missing_block_identity));

    auto scalar_with_leaked_policy = make_attested_job(
        executed_backend::scalar_cpu, target_index_source::recomputed);
    scalar_with_leaked_policy.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    scalar_with_leaked_policy.observed_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    REQUIRE_FALSE(is_job_execution_stats_valid(scalar_with_leaked_policy));

    pipeline_execution_stats aggregate{};
    aggregate.merge_job(bounded);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 1);
    REQUIRE(aggregate.requested_simd_chaining_policy
            == simd_chaining_policy_identity::bounded_batch_approximate);
    REQUIRE(aggregate.observed_simd_chaining_policy
            == simd_chaining_policy_identity::bounded_batch_approximate);
    REQUIRE(aggregate.requested_simd_alignment_algorithm
            == simd_alignment_algorithm_identity::block_aligner);
    REQUIRE(aggregate.observed_simd_alignment_algorithm
            == simd_alignment_algorithm_identity::block_aligner);

    auto exact = bounded;
    exact.logical_target_row = 1;
    exact.requested_simd_chaining_policy =
        simd_chaining_policy_identity::exact_visited;
    exact.observed_simd_chaining_policy =
        simd_chaining_policy_identity::exact_visited;
    REQUIRE(is_job_execution_stats_valid(exact));
    aggregate.merge_job(exact);
    REQUIRE(aggregate.requested_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(aggregate.observed_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(aggregate.attempted_alignment_jobs == 2);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 2);

    pipeline_execution_stats bounded_group{};
    bounded_group.merge_job(bounded);
    pipeline_execution_stats exact_group{};
    exact_group.merge_job(exact);
    bounded_group.merge(exact_group);
    REQUIRE(bounded_group.requested_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(bounded_group.observed_simd_chaining_policy
            == simd_chaining_policy_identity::mixed_invalid);
    REQUIRE(bounded_group.fully_attested_alignment_jobs == 0);
    REQUIRE(bounded_group.unattested_alignment_jobs == 2);

    pipeline_operational_stats continuation{};
    continuation.nthash_vector_batches = 7;
    continuation.simd_chaining_exact_policy_calls = 1;
    pipeline_operational_stats resident{};
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

    pipeline_operational_stats resident_only{};
    resident_only.nthash_resident_vector_stream_kernel_calls = 1;
    REQUIRE(resident_only.has_nthash_vector_activity());
    REQUIRE(resident_only.nthash_vector_batches == 0);

    block_aligner_stage_stats first_block{};
    first_block.is_available = true;
    first_block.is_accepted = true;
    first_block.executed_calls = 2;
    first_block.nonempty_calls = 2;
    first_block.nonempty_calls_with_simd = 2;
    first_block.simd_dp_vector_batches = 9;
    first_block.simd_dp_cells = 144;
    block_aligner_stage_stats second_block = first_block;
    second_block.executed_calls = 1;
    second_block.nonempty_calls = 1;
    second_block.nonempty_calls_with_simd = 1;
    second_block.simd_dp_vector_batches = 4;
    second_block.simd_dp_cells = 64;
    REQUIRE_FALSE(first_block.merge(second_block));
    REQUIRE(first_block.is_available);
    REQUIRE(first_block.is_accepted);
    REQUIRE(first_block.executed_calls == 3);
    REQUIRE(first_block.nonempty_calls == 3);
    REQUIRE(first_block.nonempty_calls_with_simd == 3);
    REQUIRE(first_block.simd_dp_vector_batches == 13);
    REQUIRE(first_block.simd_dp_cells == 208);
    REQUIRE(first_block.forbidden_ksw2_calls == 0);
    REQUIRE(first_block.forbidden_scalar_biological_fallbacks == 0);
    REQUIRE_FALSE(first_block.is_saturated);

    block_aligner_stage_stats forbidden_block{};
    forbidden_block.is_available = true;
    forbidden_block.is_accepted = false;
    forbidden_block.forbidden_ksw2_calls = 1;
    REQUIRE_FALSE(first_block.merge(forbidden_block));
    REQUIRE_FALSE(first_block.is_accepted);
    REQUIRE(first_block.forbidden_ksw2_calls == 1);

    for (const bool saturated_policy : {false, true}) {
        pipeline_execution_stats rejected{};
        rejected.merge_job(bounded);
        pipeline_operational_stats invalid_policy{};
        invalid_policy.simd_chaining_policy_rejections =
            saturated_policy ? 0 : 1;
        invalid_policy.are_simd_chaining_policy_stats_saturated =
            saturated_policy;
        rejected.merge_operational(invalid_policy);
        REQUIRE(rejected.fully_attested_alignment_jobs == 0);
        REQUIRE(rejected.unattested_alignment_jobs == 1);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            rejected));
    }
}

TEST_CASE("prepared CRC identities preserve all u64 bits and publish once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    constexpr std::array<std::uint64_t, 4> identities{
        0,
        k_sql_bigint_value_max,
        std::uint64_t{1} << 63,
        (std::numeric_limits<std::uint64_t>::max)(),
    };

    for (const std::uint64_t identity : identities) {
        pipeline_operational_stats aggregate{};
        pipeline_operational_stats publication{};
        publication.cpu_prepared_key_set_checksum = identity;
        publication.cpu_prepared_key_set_checksum_available = true;
        publication.cpu_prepared_build_set_checksum = identity;
        publication.cpu_prepared_build_set_checksum_available = true;
        aggregate.merge(publication);

        REQUIRE(aggregate.cpu_prepared_key_set_checksum_available);
        REQUIRE(aggregate.cpu_prepared_key_set_checksum == identity);
        REQUIRE(aggregate.cpu_prepared_build_set_checksum_available);
        REQUIRE(aggregate.cpu_prepared_build_set_checksum == identity);
        REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
        REQUIRE_FALSE(aggregate.are_stats_saturated);
    }

    pipeline_operational_stats duplicate{};
    pipeline_operational_stats first_publication{};
    first_publication.cpu_prepared_key_set_checksum = std::uint64_t{1} << 63;
    first_publication.cpu_prepared_key_set_checksum_available = true;
    duplicate.merge(first_publication);
    duplicate.merge(first_publication);
    REQUIRE(duplicate.has_operational_identity_conflict);
    REQUIRE(duplicate.cpu_prepared_key_set_checksum == (std::uint64_t{1} << 63));
    REQUIRE_FALSE(duplicate.are_stats_saturated);

    pipeline_operational_stats mismatch{};
    pipeline_operational_stats low{};
    low.cpu_prepared_build_set_checksum = 7;
    low.cpu_prepared_build_set_checksum_available = true;
    pipeline_operational_stats high{};
    high.cpu_prepared_build_set_checksum =
        (std::numeric_limits<std::uint64_t>::max)();
    high.cpu_prepared_build_set_checksum_available = true;
    mismatch.merge(low);
    mismatch.merge(high);
    REQUIRE(mismatch.has_operational_identity_conflict);
    REQUIRE(mismatch.cpu_prepared_build_set_checksum == 7);
    REQUIRE_FALSE(mismatch.are_stats_saturated);
}

TEST_CASE("job validation enforces the exclusive backend/source matrix",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    const auto simd_recomputed = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    const auto gpu_persisted = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    REQUIRE(is_job_execution_stats_valid(simd_recomputed));
    REQUIRE(is_job_execution_stats_valid(gpu_persisted));

    auto wrong_engine = simd_recomputed;
    auto& query_sketch =
        wrong_engine.stages.coverage[stage_index(pipeline_stage::query_sketch)];
    query_sketch.executed_jobs[engine_index(execution_engine::simd_cpu)] = 0;
    query_sketch.executed_jobs[engine_index(execution_engine::scalar_cpu)] = 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(wrong_engine));

    auto fallback = gpu_persisted;
    fallback.stages.coverage[stage_index(pipeline_stage::chain_score)].fallback_jobs = 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(fallback));

    auto forbidden_stage = make_attested_job(
        executed_backend::simd_cpu, target_index_source::persisted);
    auto& rebuilt_target =
        forbidden_stage.stages.coverage[stage_index(pipeline_stage::target_sketch)];
    rebuilt_target.eligible_jobs = 1;
    rebuilt_target.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(forbidden_stage));

    auto missing_identity = gpu_persisted;
    missing_identity.persisted_generation_id = 0;
    REQUIRE_FALSE(is_job_execution_stats_valid(missing_identity));

    auto repeated_required_stage = simd_recomputed;
    auto& repeated_query = repeated_required_stage.stages.coverage[
        stage_index(pipeline_stage::query_sketch)];
    repeated_query.eligible_jobs = 2;
    repeated_query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 2;
    REQUIRE_FALSE(is_job_execution_stats_valid(repeated_required_stage));

    auto missing_required_stage = simd_recomputed;
    missing_required_stage.stages.coverage[
        stage_index(pipeline_stage::query_sketch)] = {};
    REQUIRE_FALSE(is_job_execution_stats_valid(missing_required_stage));

    auto entirely_unattributed = simd_recomputed;
    entirely_unattributed.stages = {};
    REQUIRE_FALSE(is_job_execution_stats_valid(entirely_unattributed));

    auto no_extension = simd_recomputed;
    no_extension.stages.coverage[stage_index(pipeline_stage::gap_extension)] = {};
    REQUIRE(is_job_execution_stats_valid(no_extension));
}

TEST_CASE("zero-match jobs retain front-end stats for every measured mode",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::simd_cpu, target_index_source::recomputed)));
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::simd_cpu, target_index_source::persisted)));
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::vulkan_gpu, target_index_source::recomputed)));
    REQUIRE(is_job_execution_stats_valid(make_zero_match_job(
        executed_backend::vulkan_gpu, target_index_source::persisted)));
}

TEST_CASE("SQL-bound stats values reject max plus one without clamping",
          "[component:bioseq][layer:unit][feature:sql_api]") {
    using namespace bioseq::pipeline_stats;
    auto boundary = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    boundary.environment.effective_cpu_workers = k_sql_integer_value_max;
    boundary.environment.gpu_pipeline_cache_seed_size = k_sql_bigint_value_max;
    boundary.persisted_generation_id = k_sql_bigint_value_max;
    boundary.logical_target_row = k_sql_bigint_value_max;
    boundary.query_ordinal = k_sql_bigint_value_max;
    REQUIRE(is_job_execution_stats_valid(boundary));

    auto workers_overflow = boundary;
    workers_overflow.environment.effective_cpu_workers = k_sql_integer_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(workers_overflow));

    auto seed_size_overflow = boundary;
    seed_size_overflow.environment.gpu_pipeline_cache_seed_size =
        k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(seed_size_overflow));

    auto generation_overflow = boundary;
    generation_overflow.persisted_generation_id = k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(generation_overflow));

    auto target_row_overflow = boundary;
    target_row_overflow.logical_target_row = k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(target_row_overflow));

    auto query_ordinal_overflow = boundary;
    query_ordinal_overflow.query_ordinal = k_sql_bigint_value_max + 1;
    REQUIRE_FALSE(is_job_execution_stats_valid(query_ordinal_overflow));
}

TEST_CASE("GPU environment requires canonical identity and policy-hit pairs",
          "[component:bioseq][layer:unit][feature:pipeline]") {
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
    static_assert(is_canonical_nonempty_driver_identity(constexpr_valid_four_byte));
    static_assert(!is_canonical_nonempty_driver_identity(constexpr_invalid_surrogate));

    std::array<char, 128> valid_utf8{};
    valid_utf8[0] = static_cast<char>(0xc3);
    valid_utf8[1] = static_cast<char>(0xa9);
    REQUIRE(is_canonical_nonempty_driver_identity(valid_utf8));

    std::array<char, 128> valid_three_byte{};
    valid_three_byte[0] = static_cast<char>(0xe2);
    valid_three_byte[1] = static_cast<char>(0x82);
    valid_three_byte[2] = static_cast<char>(0xac);
    REQUIRE(is_canonical_nonempty_driver_identity(valid_three_byte));

    std::array<char, 128> valid_four_byte{};
    valid_four_byte[0] = static_cast<char>(0xf0);
    valid_four_byte[1] = static_cast<char>(0x9f);
    valid_four_byte[2] = static_cast<char>(0x98);
    valid_four_byte[3] = static_cast<char>(0x80);
    REQUIRE(is_canonical_nonempty_driver_identity(valid_four_byte));

    std::array<char, 128> stray_continuation{};
    stray_continuation[0] = static_cast<char>(0x80);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(stray_continuation));

    std::array<char, 128> invalid_continuation{};
    invalid_continuation[0] = static_cast<char>(0xc3);
    invalid_continuation[1] = '(';
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(invalid_continuation));

    std::array<char, 128> overlong{};
    overlong[0] = static_cast<char>(0xc0);
    overlong[1] = static_cast<char>(0xaf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(overlong));

    std::array<char, 128> c1_overlong{};
    c1_overlong[0] = static_cast<char>(0xc1);
    c1_overlong[1] = static_cast<char>(0xbf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(c1_overlong));

    std::array<char, 128> e0_overlong{};
    e0_overlong[0] = static_cast<char>(0xe0);
    e0_overlong[1] = static_cast<char>(0x9f);
    e0_overlong[2] = static_cast<char>(0xbf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(e0_overlong));

    std::array<char, 128> surrogate{};
    surrogate[0] = static_cast<char>(0xed);
    surrogate[1] = static_cast<char>(0xa0);
    surrogate[2] = static_cast<char>(0x80);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(surrogate));

    std::array<char, 128> above_unicode_max{};
    above_unicode_max[0] = static_cast<char>(0xf4);
    above_unicode_max[1] = static_cast<char>(0x90);
    above_unicode_max[2] = static_cast<char>(0x80);
    above_unicode_max[3] = static_cast<char>(0x80);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(above_unicode_max));

    std::array<char, 128> f0_overlong{};
    f0_overlong[0] = static_cast<char>(0xf0);
    f0_overlong[1] = static_cast<char>(0x8f);
    f0_overlong[2] = static_cast<char>(0xbf);
    f0_overlong[3] = static_cast<char>(0xbf);
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(f0_overlong));

    std::array<char, 128> nonzero_padding{};
    nonzero_padding[0] = 'v';
    nonzero_padding[2] = 'x';
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(nonzero_padding));

    std::array<char, 128> unterminated{};
    unterminated.fill('x');
    REQUIRE_FALSE(is_canonical_nonempty_driver_identity(unterminated));

    auto immutable_without_seed = make_gpu_environment();
    immutable_without_seed.gpu_pipeline_cache_seed_size = 0;

    auto immutable_miss = make_gpu_environment();
    immutable_miss.gpu_pipeline_cache_hit = optional_observation_bool::false_value;

    auto isolated_hit = make_gpu_environment();
    isolated_hit.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;

    auto zero_manifest = make_gpu_environment();
    zero_manifest.gpu_pipeline_cache_manifest_checksum = 0;

    auto zero_uuid = make_gpu_environment();
    zero_uuid.gpu_device_uuid = {};

    auto zero_seed_sha = make_gpu_environment();
    zero_seed_sha.gpu_pipeline_cache_seed_sha256 = {};

    auto not_applicable_policy = make_gpu_environment();
    not_applicable_policy.gpu_pipeline_cache_policy =
        pipeline_cache_policy::not_applicable;

    auto mixed_policy = make_gpu_environment();
    mixed_policy.gpu_pipeline_cache_policy =
        pipeline_cache_policy::mixed_invalid;

    auto immutable_unset_hit = make_gpu_environment();
    immutable_unset_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::not_applicable;

    auto immutable_mixed_hit = make_gpu_environment();
    immutable_mixed_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::mixed_invalid;

    auto isolated_unset_hit = make_gpu_environment();
    isolated_unset_hit.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;
    isolated_unset_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::not_applicable;

    auto isolated_mixed_hit = make_gpu_environment();
    isolated_mixed_hit.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;
    isolated_mixed_hit.gpu_pipeline_cache_hit =
        optional_observation_bool::mixed_invalid;

    struct invalid_environment_case {
        std::string_view name;
        execution_environment_snapshot environment;
    };
    const auto invalid_environments = std::to_array<invalid_environment_case>({
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
        REQUIRE_FALSE(is_environment_valid_for_backend(
            test.environment, executed_backend::vulkan_gpu));
    }

    REQUIRE(is_environment_valid_for_backend(
        make_gpu_environment(), executed_backend::vulkan_gpu));

    auto isolated_empty_seed = immutable_without_seed;
    isolated_empty_seed.gpu_pipeline_cache_policy =
        pipeline_cache_policy::isolated_miss_diagnostic;
    isolated_empty_seed.gpu_pipeline_cache_hit = optional_observation_bool::false_value;
    REQUIRE(is_environment_valid_for_backend(
        isolated_empty_seed, executed_backend::vulkan_gpu));
}

TEST_CASE("aggregate merge rejects persisted and environment identity drift",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats aggregate{};
    auto first = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    first.logical_target_row = 11;
    first.query_ordinal = 7;
    aggregate.merge_job(first);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 1);
    REQUIRE_FALSE(aggregate.has_mixed_persisted_identity);

    auto changed_generation = first;
    changed_generation.persisted_generation_id = 18;
    changed_generation.logical_target_row = 12;
    aggregate.merge_job(changed_generation);
    REQUIRE(aggregate.has_mixed_persisted_identity);
    REQUIRE(aggregate.attempted_alignment_jobs == 2);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 2);
    REQUIRE(aggregate.first_unattested_target_row == 11);
    REQUIRE(aggregate.first_unattested_query_ordinal == 7);

    auto changed_environment = first;
    changed_environment.environment.effective_cpu_workers = 3;
    changed_environment.logical_target_row = 13;
    aggregate.merge_job(changed_environment);
    REQUIRE(aggregate.environment.is_mixed_invalid);
    REQUIRE(aggregate.attempted_alignment_jobs == 3);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(aggregate.unattested_alignment_jobs == 3);
    REQUIRE(aggregate.first_unattested_target_row == 11);
    REQUIRE(aggregate.first_unattested_query_ordinal == 7);
}

TEST_CASE("merge_job and aggregate merge have identical drift semantics",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_same_counts_and_coordinates = [](
            const pipeline_execution_stats& direct,
            const pipeline_execution_stats& grouped) {
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
            executed_backend::vulkan_gpu, target_index_source::persisted);
        first.logical_target_row = 41;
        first.query_ordinal = 5;
        auto second = first;
        second.logical_target_row = 42;
        second.query_ordinal = 6;
        second.persisted_generation_id = 18;

        pipeline_execution_stats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == executed_backend::vulkan_gpu);
        REQUIRE(direct.actual_backend == executed_backend::vulkan_gpu);
        REQUIRE(direct.has_mixed_persisted_identity);
        REQUIRE(grouped.has_mixed_persisted_identity);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 41);
        REQUIRE(direct.first_unattested_query_ordinal == 5);
    }

    SECTION("environment drift") {
        auto first = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        first.logical_target_row = 51;
        first.query_ordinal = 7;
        auto second = first;
        second.logical_target_row = 52;
        second.query_ordinal = 8;
        second.environment.effective_cpu_workers = 3;

        pipeline_execution_stats direct{};
        direct.merge_job(first);
        direct.merge_job(second);
        auto grouped = aggregate_job(first);
        grouped.merge(aggregate_job(second));

        require_same_counts_and_coordinates(direct, grouped);
        REQUIRE(direct.requested_backend == executed_backend::simd_cpu);
        REQUIRE(direct.actual_backend == executed_backend::simd_cpu);
        REQUIRE(direct.environment.is_mixed_invalid);
        REQUIRE(grouped.environment.is_mixed_invalid);
        REQUIRE(direct.attempted_alignment_jobs == 2);
        REQUIRE(direct.fully_attested_alignment_jobs == 0);
        REQUIRE(direct.unattested_alignment_jobs == 2);
        REQUIRE(direct.first_unattested_target_row == 51);
        REQUIRE(direct.first_unattested_query_ordinal == 7);
    }
}

TEST_CASE("aggregate merge cannot launder absent environments",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto missing_environment_operand = [] {
        pipeline_execution_stats stats{};
        stats.requested_backend = executed_backend::simd_cpu;
        stats.actual_backend = executed_backend::simd_cpu;
        stats.requested_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        stats.observed_simd_chaining_policy =
            simd_chaining_policy_identity::bounded_batch_approximate;
        stats.requested_target_index_source = target_index_source::recomputed;
        stats.actual_target_index_source = target_index_source::recomputed;
        stats.attempted_alignment_jobs = 1;
        stats.fully_attested_alignment_jobs = 1;
        stats.first_attempted_target_row = 91;
        stats.first_attempted_query_ordinal = 4;
        return stats;
    };
    const auto valid_operand = [] {
        pipeline_execution_stats stats{};
        stats.merge_job(make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed));
        return stats;
    };

    auto missing_left = missing_environment_operand();
    missing_left.merge(valid_operand());
    REQUIRE(missing_left.environment.is_mixed_invalid);
    REQUIRE(missing_left.attempted_alignment_jobs == 2);
    REQUIRE(missing_left.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_left.unattested_alignment_jobs == 2);
    REQUIRE(missing_left.first_unattested_target_row == 91);
    REQUIRE(missing_left.first_unattested_query_ordinal == 4);

    auto missing_right = valid_operand();
    missing_right.merge(missing_environment_operand());
    REQUIRE(missing_right.environment.is_mixed_invalid);
    REQUIRE(missing_right.attempted_alignment_jobs == 2);
    REQUIRE(missing_right.fully_attested_alignment_jobs == 0);
    REQUIRE(missing_right.unattested_alignment_jobs == 2);
    REQUIRE(missing_right.first_unattested_target_row == 0);
    REQUIRE(missing_right.first_unattested_query_ordinal == 0);

    pipeline_execution_stats zero_job_nonempty{};
    zero_job_nonempty.requested_backend = executed_backend::simd_cpu;
    zero_job_nonempty.actual_backend = executed_backend::simd_cpu;
    zero_job_nonempty.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    zero_job_nonempty.observed_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    zero_job_nonempty.requested_target_index_source = target_index_source::recomputed;
    zero_job_nonempty.actual_target_index_source = target_index_source::recomputed;
    zero_job_nonempty.environment = make_cpu_environment();
    pipeline_execution_stats zero_job_aggregate{};
    zero_job_aggregate.merge(zero_job_nonempty);
    REQUIRE(zero_job_aggregate.environment.is_mixed_invalid);
    REQUIRE(zero_job_aggregate.attempted_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.fully_attested_alignment_jobs == 0);
    REQUIRE(zero_job_aggregate.unattested_alignment_jobs == 0);
}

TEST_CASE("exact-empty stats checks every stage and operational member",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const pipeline_execution_stats empty{};
    REQUIRE(pipeline_execution_stats::is_exact_empty_stats_operand(empty));

    SECTION("stage coverage is observations even with no attempted jobs") {
        auto injected = empty;
        injected.stages.coverage[
            stage_index(pipeline_stage::query_sketch)].eligible_jobs = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("stage timing is operational observations") {
        auto injected = empty;
        injected.operational.timing[
            stage_index(pipeline_stage::query_sketch)].host_wall_ns = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("an ordinary operational counter is observations") {
        auto injected = empty;
        injected.operational.target_rows = 1;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }

    SECTION("a zero-valued report-once identity still has a presence bit") {
        auto injected = empty;
        injected.operational.cpu_prepared_key_set_checksum_available = true;
        REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
            injected));
    }
}

TEST_CASE("zero-job stage observations cannot launder a complete aggregate",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 71;
    job.query_ordinal = 8;
    pipeline_execution_stats complete{};
    complete.merge_job(job);
    REQUIRE(pipeline_execution_stats::can_retain_full_attestation(complete));

    pipeline_execution_stats injected{};
    auto& query = injected.stages.coverage[
        stage_index(pipeline_stage::query_sketch)];
    query.eligible_jobs = 1;
    query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
    REQUIRE_FALSE(pipeline_execution_stats::is_exact_empty_stats_operand(
        injected));

    const auto require_rejected = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 71);
        REQUIRE(stats.first_unattested_query_ordinal == 8);
        REQUIRE_FALSE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            stats));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            stats));
    };

    auto injected_on_right = complete;
    injected_on_right.merge(injected);
    require_rejected(injected_on_right);

    auto injected_on_left = injected;
    injected_on_left.merge(complete);
    require_rejected(injected_on_left);
}

TEST_CASE("aggregate retention rejects malformed stage matrices",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 81;
    job.query_ordinal = 6;
    pipeline_execution_stats complete{};
    complete.merge_job(job);
    REQUIRE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
        complete));
    REQUIRE(pipeline_execution_stats::can_retain_full_attestation(complete));

    const auto require_malformed_stage_rejected = [](
            const pipeline_execution_stats& malformed) {
        REQUIRE_FALSE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            malformed));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
        pipeline_execution_stats merged{};
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
            stage_index(pipeline_stage::query_sketch)] = {};
        require_malformed_stage_rejected(malformed);
    }

    SECTION("stage total exceeds the attempted-job total") {
        auto malformed = complete;
        auto& query = malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)];
        query.eligible_jobs = 2;
        query.executed_jobs[engine_index(execution_engine::simd_cpu)] = 2;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a forbidden engine lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)]
            .executed_jobs[engine_index(execution_engine::scalar_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a fallback lane carries work") {
        auto malformed = complete;
        malformed.stages.coverage[
            stage_index(pipeline_stage::query_sketch)].fallback_jobs = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a mode-forbidden stage carries work") {
        auto malformed = complete;
        auto& upload = malformed.stages.coverage[
            stage_index(pipeline_stage::device_index_upload)];
        upload.eligible_jobs = 1;
        upload.executed_jobs[engine_index(execution_engine::simd_cpu)] = 1;
        require_malformed_stage_rejected(malformed);
    }

    SECTION("a saturated aggregate cannot retain stats") {
        auto malformed = complete;
        malformed.operational.are_stats_saturated = true;
        REQUIRE(pipeline_execution_stats::is_aggregate_stage_coverage_valid(
            malformed));
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            malformed));
        pipeline_execution_stats merged{};
        merged.merge(malformed);
        REQUIRE(merged.fully_attested_alignment_jobs == 0);
        REQUIRE(merged.unattested_alignment_jobs == 1);
        REQUIRE(merged.first_unattested_target_row == 81);
        REQUIRE(merged.first_unattested_query_ordinal == 6);
    }
}

TEST_CASE("backend declines and fallbacks always revoke attestation",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 91;
    job.query_ordinal = 7;

    const auto require_revoked = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 91);
        REQUIRE(stats.first_unattested_query_ordinal == 7);
        REQUIRE_FALSE(pipeline_execution_stats::can_retain_full_attestation(
            stats));
    };

    for (const bool is_fallback : {false, true}) {
        pipeline_operational_stats observations{};
        if (is_fallback) {
            observations.backend_fallbacks = 1;
        } else {
            observations.backend_declines = 1;
        }

        pipeline_execution_stats operational_after_job{};
        operational_after_job.merge_job(job);
        operational_after_job.merge_operational(observations);
        require_revoked(operational_after_job);

        pipeline_execution_stats operational_before_job{};
        operational_before_job.merge_operational(observations);
        operational_before_job.merge_job(job);
        require_revoked(operational_before_job);

        pipeline_execution_stats operational_only{};
        operational_only.merge_operational(observations);
        pipeline_execution_stats grouped_after_job{};
        grouped_after_job.merge_job(job);
        grouped_after_job.merge(operational_only);
        require_revoked(grouped_after_job);

        auto grouped_before_job = operational_only;
        pipeline_execution_stats job_only{};
        job_only.merge_job(job);
        grouped_before_job.merge(job_only);
        require_revoked(grouped_before_job);
    }
}

TEST_CASE("mixed CPU diagnostics retain environment without becoming stats",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_job_execution_stats job{};
    job.requested_backend = executed_backend::mixed_invalid;
    job.actual_backend = executed_backend::mixed_invalid;
    job.requested_target_index_source = target_index_source::recomputed;
    job.actual_target_index_source = target_index_source::recomputed;
    job.environment = make_cpu_environment();

    pipeline_execution_stats node{};
    node.merge_job(job);
    REQUIRE(node.attempted_alignment_jobs == 1);
    REQUIRE(node.fully_attested_alignment_jobs == 0);
    REQUIRE(node.unattested_alignment_jobs == 1);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(node));

    pipeline_execution_stats statement{};
    statement.merge(node);
    REQUIRE(statement.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 1);
    REQUIRE_FALSE(statement.environment.is_mixed_invalid);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(statement));

    pipeline_execution_stats valid_sibling{};
    auto valid_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    valid_job.logical_target_row = 12;
    valid_job.query_ordinal = 3;
    valid_sibling.merge_job(valid_job);
    statement.merge(valid_sibling);
    REQUIRE(statement.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(statement.fully_attested_alignment_jobs == 0);
    REQUIRE(statement.unattested_alignment_jobs == 2);
    REQUIRE(pipeline_execution_stats::is_diagnostic_environment_valid(statement));

    auto gpu_work_with_cpu_environment = node;
    gpu_work_with_cpu_environment.operational.gpu_seed_dispatches = 1;
    REQUIRE_FALSE(pipeline_execution_stats::is_diagnostic_environment_valid(
        gpu_work_with_cpu_environment));
    pipeline_execution_stats rejected_statement{};
    rejected_statement.merge(gpu_work_with_cpu_environment);
    REQUIRE(rejected_statement.environment.is_mixed_invalid);
}

TEST_CASE("aggregate merge is associative under environment drift",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto first_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
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
            const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.requested_target_index_source
                == target_index_source::recomputed);
        REQUIRE(stats.actual_target_index_source
                == target_index_source::recomputed);
        REQUIRE(stats.environment.is_mixed_invalid);
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

TEST_CASE("aggregate merge is associative at SQL bigint saturation",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](std::uint64_t target_row) {
        auto job = make_attested_job(
            executed_backend::simd_cpu, target_index_source::recomputed);
        job.logical_target_row = target_row;
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };

    auto near_limit = aggregate_job(10);
    near_limit.attempted_alignment_jobs = k_sql_bigint_value_max;
    near_limit.fully_attested_alignment_jobs = k_sql_bigint_value_max;
    auto second = aggregate_job(20);
    auto third = aggregate_job(30);

    auto left_grouped = near_limit;
    left_grouped.merge(second);
    left_grouped.merge(third);

    auto right_tail = second;
    right_tail.merge(third);
    auto right_grouped = near_limit;
    right_grouped.merge(right_tail);

    const auto require_saturated = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.attempted_alignment_jobs == k_sql_bigint_value_max);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == k_sql_bigint_value_max);
        REQUIRE(stats.first_unattested_target_row == 10);
        REQUIRE(stats.first_unattested_query_ordinal == 0);
        REQUIRE(stats.operational.are_stats_saturated);
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

TEST_CASE("operational saturation is fail closed independent of ordering",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    auto job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    job.logical_target_row = 61;
    job.query_ordinal = 9;
    const auto saturate_operational = [](pipeline_execution_stats& stats) {
        pipeline_operational_stats at_limit{};
        at_limit.target_rows = k_sql_bigint_value_max;
        pipeline_operational_stats overflow{};
        overflow.target_rows = 1;
        stats.merge_operational(at_limit);
        stats.merge_operational(overflow);
    };

    pipeline_execution_stats operational_after_job{};
    operational_after_job.merge_job(job);
    saturate_operational(operational_after_job);

    pipeline_execution_stats operational_before_job{};
    saturate_operational(operational_before_job);
    operational_before_job.merge_job(job);

    pipeline_execution_stats operational_only{};
    saturate_operational(operational_only);
    pipeline_execution_stats grouped{};
    grouped.merge_job(job);
    grouped.merge(operational_only);

    const auto require_fail_closed = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.requested_backend == executed_backend::simd_cpu);
        REQUIRE(stats.actual_backend == executed_backend::simd_cpu);
        REQUIRE(stats.attempted_alignment_jobs == 1);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 1);
        REQUIRE(stats.first_unattested_target_row == 61);
        REQUIRE(stats.first_unattested_query_ordinal == 9);
        REQUIRE(stats.operational.target_rows == k_sql_bigint_value_max);
        REQUIRE(stats.operational.are_stats_saturated);
    };
    require_fail_closed(operational_after_job);
    require_fail_closed(operational_before_job);
    require_fail_closed(grouped);
}

TEST_CASE("cold-index operational telemetry merges and saturates through one authority",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats aggregate{};
    pipeline_operational_stats first{};
    first.cold_index_sidecar_heap_opens = 2;
    first.cold_index_sidecar_index_opens = 2;
    first.cold_index_lock_acquires = 7;
    first.cold_index_lock_releases = 6;
    first.cold_index_payload_bytes_delivered = 4096;
    first.cold_index_payload_bytes_scanned = 6144;
    first.cold_index_payload_rows_scanned = 3;
    first.cold_index_payload_rows_skipped = 1;
    first.cold_index_shared_blks_read = 3;
    first.grouped_soa_reader_capacity_accounting_passes = 5;
    first.grouped_soa_reader_global_position_storage_preparations = 1;
    first.grouped_soa_reader_global_position_storage_allocations = 1;
    first.grouped_soa_reader_retained_position_count = 37;
    first.grouped_soa_reader_retained_position_capacity = 37;
    first.grouped_soa_reader_directory_capacity_bytes = 10;
    first.grouped_soa_reader_query_hash_capacity_bytes = 7;
    first.grouped_soa_reader_query_record_capacity_bytes = 100;
    first.grouped_soa_reader_ordinal_route_capacity_bytes = 20;
    first.grouped_soa_reader_selected_extent_capacity_bytes = 30;
    first.grouped_soa_reader_current_selected_control_capacity_bytes = 11;
    first.grouped_soa_reader_global_position_capacity_bytes = 296;
    first.grouped_soa_reader_control_payload_capacity_bytes = 13;
    first.grouped_soa_reader_owned_capacity_bytes = 487;
    first.grouped_soa_reader_admitted_bytes = 487;
    first.grouped_soa_reader_exact_position_image_checks = 1;
    first.grouped_soa_reader_retained_layout_checks = 1;
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
    REQUIRE(aggregate.grouped_soa_reader_capacity_accounting_passes == 5);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_preparations == 1);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_allocations == 1);
    REQUIRE(aggregate.grouped_soa_reader_position_append_growth_reservations == 0);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_count == 37);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity == 37);
    REQUIRE(aggregate.grouped_soa_reader_query_hash_capacity_bytes == 7);
    REQUIRE(aggregate.grouped_soa_reader_current_selected_control_capacity_bytes
            == 11);
    REQUIRE(aggregate.grouped_soa_reader_global_position_capacity_bytes == 296);
    REQUIRE(aggregate.grouped_soa_reader_control_payload_capacity_bytes == 13);
    REQUIRE(aggregate.grouped_soa_reader_owned_capacity_bytes == 487);
    REQUIRE(aggregate.grouped_soa_reader_admitted_bytes == 487);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_checks == 1);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_failures == 0);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_checks == 1);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_failures == 0);
    REQUIRE_FALSE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats second{};
    second.grouped_soa_reader_capacity_accounting_passes = 4;
    second.grouped_soa_reader_global_position_storage_preparations = 1;
    second.grouped_soa_reader_global_position_storage_allocations = 1;
    second.grouped_soa_reader_position_append_growth_reservations = 2;
    second.grouped_soa_reader_retained_position_count = 11;
    second.grouped_soa_reader_retained_position_capacity = 11;
    second.grouped_soa_reader_directory_capacity_bytes = 5;
    second.grouped_soa_reader_query_hash_capacity_bytes = 3;
    second.grouped_soa_reader_query_record_capacity_bytes = 40;
    second.grouped_soa_reader_ordinal_route_capacity_bytes = 8;
    second.grouped_soa_reader_selected_extent_capacity_bytes = 12;
    second.grouped_soa_reader_current_selected_control_capacity_bytes = 5;
    second.grouped_soa_reader_global_position_capacity_bytes = 87;
    second.grouped_soa_reader_control_payload_capacity_bytes = 7;
    second.grouped_soa_reader_owned_capacity_bytes = 167;
    second.grouped_soa_reader_admitted_bytes = 168;
    second.grouped_soa_reader_exact_position_image_checks = 1;
    second.grouped_soa_reader_exact_position_image_failures = 1;
    second.grouped_soa_reader_retained_layout_checks = 1;
    second.grouped_soa_reader_retained_layout_failures = 1;
    second.are_grouped_soa_reader_memory_stats_saturated = true;
    aggregate.merge(second);

    REQUIRE(aggregate.grouped_soa_reader_capacity_accounting_passes == 9);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_preparations == 2);
    REQUIRE(aggregate.grouped_soa_reader_global_position_storage_allocations == 2);
    REQUIRE(aggregate.grouped_soa_reader_position_append_growth_reservations == 2);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_count == 48);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity == 48);
    REQUIRE(aggregate.grouped_soa_reader_query_hash_capacity_bytes == 10);
    REQUIRE(aggregate.grouped_soa_reader_current_selected_control_capacity_bytes
            == 16);
    REQUIRE(aggregate.grouped_soa_reader_global_position_capacity_bytes == 383);
    REQUIRE(aggregate.grouped_soa_reader_control_payload_capacity_bytes == 20);
    REQUIRE(aggregate.grouped_soa_reader_owned_capacity_bytes == 654);
    REQUIRE(aggregate.grouped_soa_reader_admitted_bytes == 655);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_checks == 2);
    REQUIRE(aggregate.grouped_soa_reader_exact_position_image_failures == 1);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_checks == 2);
    REQUIRE(aggregate.grouped_soa_reader_retained_layout_failures == 1);
    REQUIRE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats at_limit{};
    at_limit.cold_index_payload_bytes_scanned = k_sql_bigint_value_max;
    at_limit.grouped_soa_reader_retained_position_capacity = k_sql_bigint_value_max;
    pipeline_operational_stats overflow{};
    overflow.cold_index_payload_bytes_scanned = 1;
    overflow.grouped_soa_reader_retained_position_capacity = 1;
    aggregate.merge(at_limit);
    aggregate.merge(overflow);

    REQUIRE(aggregate.cold_index_payload_bytes_scanned
            == k_sql_bigint_value_max);
    REQUIRE(aggregate.grouped_soa_reader_retained_position_capacity
            == k_sql_bigint_value_max);
    REQUIRE(aggregate.are_stats_saturated);

    using capacity_member_type =
        std::uint64_t pipeline_operational_stats::*;
    const auto require_independent_saturation = [](capacity_member_type member) {
        pipeline_operational_stats exact_limit{};
        exact_limit.*member = k_sql_bigint_value_max;
        pipeline_operational_stats independently_merged{};
        independently_merged.merge(exact_limit);
        REQUIRE((independently_merged.*member) == k_sql_bigint_value_max);
        REQUIRE_FALSE(independently_merged.are_stats_saturated);

        pipeline_operational_stats plus_one{};
        plus_one.*member = 1;
        independently_merged.merge(plus_one);
        REQUIRE((independently_merged.*member) == k_sql_bigint_value_max);
        REQUIRE(independently_merged.are_stats_saturated);
    };
    require_independent_saturation(
        &pipeline_operational_stats::grouped_soa_reader_query_hash_capacity_bytes);
    require_independent_saturation(
        &pipeline_operational_stats::
            grouped_soa_reader_current_selected_control_capacity_bytes);
    require_independent_saturation(
        &pipeline_operational_stats::grouped_soa_reader_control_payload_capacity_bytes);
}

TEST_CASE("fragment-native query observations preserves additive and peak merge semantics",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::indexing::prepared_fragment_anchor_batch_stats;
    using bioseq::pipeline_stats::pipeline_operational_stats;

    prepared_fragment_anchor_batch_stats first{};
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

    prepared_fragment_anchor_batch_stats second{};
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

    pipeline_operational_stats aggregate{};
    pipeline_operational_stats delta{};
    delta.fragment_native_query_stats = first;
    aggregate.merge(delta);
    REQUIRE(aggregate.fragment_native_query_stats == first);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats overflow{};
    overflow.fragment_native_query_stats.waves =
        (std::numeric_limits<std::uint64_t>::max)();
    overflow.fragment_native_query_stats.query_occurrences =
        (std::numeric_limits<std::uint64_t>::max)();
    aggregate.merge(overflow);
    REQUIRE(aggregate.fragment_native_query_stats.waves
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_stats.query_occurrences
        == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(aggregate.fragment_native_query_stats.saturated);
    REQUIRE(aggregate.are_stats_saturated);
}

TEST_CASE("grouped-SoA provider observations preserves stable codes and checked counters",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats first{};
    auto& first_stats = first.grouped_soa_provider;
    first_stats.record_provider_configured(1u);
    first_stats.record_provider_captured(1u);
    first_stats.record_provider_persisted(1u);
    first_stats.record_provider_opened(1u);
    first_stats.record_mapped_sink_preallocation_result(
        grouped_soa_operation_result::succeeded);
    first_stats.record_lazy_state_layout(
        grouped_soa_state_layout::compact_aligned_planes);
    first_stats.record_madvise(
        grouped_soa_mapping_advice::random,
        grouped_soa_operation_result::succeeded);
    first_stats.record_counter(first_stats.provider_head_scans, 1u);
    first_stats.record_counter(first_stats.provider_head_rows, 1u);
    first_stats.record_counter(first_stats.mapped_sink_calls, 1u);
    first_stats.record_counter(first_stats.mapped_sink_bytes, 100u);
    first_stats.record_counter(first_stats.mapping_bytes, 400u);
    first_stats.record_file_action_high_water(2u);
    REQUIRE(first_stats.provider_identity_complete_and_consistent());

    pipeline_operational_stats second{};
    auto& second_stats = second.grouped_soa_provider;
    second_stats.record_provider_configured(1u);
    second_stats.record_provider_captured(1u);
    second_stats.record_provider_persisted(1u);
    second_stats.record_provider_opened(1u);
    second_stats.record_mapped_sink_preallocation_result(
        grouped_soa_operation_result::succeeded);
    second_stats.record_lazy_state_layout(
        grouped_soa_state_layout::compact_aligned_planes);
    second_stats.record_madvise(
        grouped_soa_mapping_advice::random,
        grouped_soa_operation_result::succeeded);
    second_stats.record_counter(second_stats.provider_head_scans, 1u);
    second_stats.record_counter(second_stats.provider_head_rows, 1u);
    second_stats.record_counter(second_stats.mapped_sink_calls, 1u);
    second_stats.record_counter(second_stats.mapped_sink_bytes, 200u);
    second_stats.record_counter(second_stats.mapping_bytes, 800u);
    second_stats.record_file_action_high_water(3u);

    pipeline_operational_stats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& observations = aggregate.grouped_soa_provider;
    REQUIRE(observations.provider_identity_complete_and_consistent());
    REQUIRE(observations.provider_configured == 1u);
    REQUIRE(observations.provider_captured == 1u);
    REQUIRE(observations.provider_persisted == 1u);
    REQUIRE(observations.provider_opened == 1u);
    REQUIRE(observations.provider_head_scans == 2u);
    REQUIRE(observations.provider_head_rows == 2u);
    REQUIRE(observations.mapped_sink_calls == 2u);
    REQUIRE(observations.mapped_sink_bytes == 300u);
    REQUIRE(observations.mapping_bytes == 1'200u);
    REQUIRE(observations.file_action_high_water == 3u);
    REQUIRE(observations.mapped_sink_preallocation_result
        == static_cast<std::uint64_t>(
            grouped_soa_operation_result::succeeded));
    REQUIRE(observations.lazy_state_layout
        == static_cast<std::uint64_t>(
            grouped_soa_state_layout::compact_aligned_planes));
    REQUIRE(observations.madvise_choice
        == static_cast<std::uint64_t>(grouped_soa_mapping_advice::random));
    REQUIRE(observations.madvise_result
        == static_cast<std::uint64_t>(
            grouped_soa_operation_result::succeeded));
    REQUIRE_FALSE(observations.has_stable_value_conflict);
    REQUIRE_FALSE(observations.is_saturated);
    REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
    REQUIRE_FALSE(aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats conflicting{};
    conflicting.grouped_soa_provider.record_provider_opened(0u);
    aggregate.merge(conflicting);
    REQUIRE(aggregate.has_operational_identity_conflict);
    REQUIRE(aggregate.grouped_soa_provider.has_stable_value_conflict);
    REQUIRE_FALSE(
        aggregate.grouped_soa_provider.provider_identity_complete_and_consistent());

    pipeline_operational_stats exact_limit{};
    exact_limit.grouped_soa_provider.record_counter(
        exact_limit.grouped_soa_provider.mapping_bytes,
        k_sql_bigint_value_max);
    pipeline_operational_stats saturated_aggregate{};
    saturated_aggregate.merge(exact_limit);
    REQUIRE(saturated_aggregate.grouped_soa_provider.mapping_bytes
        == k_sql_bigint_value_max);
    REQUIRE_FALSE(saturated_aggregate.are_stats_saturated);

    pipeline_operational_stats plus_one{};
    plus_one.grouped_soa_provider.record_counter(
        plus_one.grouped_soa_provider.mapping_bytes, 1u);
    saturated_aggregate.merge(plus_one);
    REQUIRE(saturated_aggregate.grouped_soa_provider.mapping_bytes
        == k_sql_bigint_value_max);
    REQUIRE(saturated_aggregate.grouped_soa_provider.is_saturated);
    REQUIRE(saturated_aggregate.are_grouped_soa_reader_memory_stats_saturated);
    REQUIRE(saturated_aggregate.are_stats_saturated);
}

TEST_CASE("hash-major reader receipts preserve max peaks and owners emit once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;

    pipeline_operational_stats first{};
    auto& first_hash_major = first.hash_major_index_reader;
    first_hash_major.layout_revision = 6;
    first_hash_major.sessions = 1;
    first_hash_major.identity_available = true;
    first_hash_major.generation_id = 71;
    first_hash_major.source_identity[0] = std::byte{0x11};
    first_hash_major.source_content_crc64 = 91;
    first_hash_major.storage_contract_digest[0] = std::byte{0x22};
    first_hash_major.directory_rows = 2;
    first_hash_major.posting_ranges_delivered = 3;
    first_hash_major.posting_range_crc_receipts = 1;
    first_hash_major.posting_semantic_receipts = 3;
    first_hash_major.directory_peak_bytes = 100;
    first_hash_major.control_peak_bytes = 300;
    first_hash_major.posting_peak_bytes = 200;
    first_hash_major.peak_admitted_bytes = 300;
    first_hash_major.owner_bytes_available = true;
    first_hash_major.retained_owner_bytes = 80;
    first_hash_major.admitted_bytes = 80;

    pipeline_operational_stats second = first;
    auto& second_hash_major = second.hash_major_index_reader;
    second_hash_major.directory_rows = 4;
    second_hash_major.posting_ranges_delivered = 5;
    second_hash_major.posting_range_crc_receipts = 2;
    second_hash_major.posting_semantic_receipts = 5;
    second_hash_major.directory_peak_bytes = 500;
    second_hash_major.control_peak_bytes = 250;
    second_hash_major.posting_peak_bytes = 450;
    second_hash_major.peak_admitted_bytes = 500;

    pipeline_operational_stats aggregate{};
    aggregate.merge(first);
    aggregate.merge(second);
    const auto& merged = aggregate.hash_major_index_reader;
    REQUIRE(merged.sessions == 2);
    REQUIRE(merged.directory_rows == 6);
    REQUIRE(merged.posting_ranges_delivered == 8);
    REQUIRE(merged.posting_range_crc_receipts == 3);
    REQUIRE(merged.posting_semantic_receipts == 8);
    REQUIRE(merged.directory_peak_bytes == 500);
    REQUIRE(merged.control_peak_bytes == 300);
    REQUIRE(merged.posting_peak_bytes == 450);
    REQUIRE(merged.peak_admitted_bytes == 500);
    REQUIRE(merged.retained_owner_bytes == 80);
    REQUIRE(merged.admitted_bytes == 80);
    REQUIRE_FALSE(aggregate.has_operational_identity_conflict);
    REQUIRE_FALSE(aggregate.are_stats_saturated);

    pipeline_operational_stats mismatched_owner = first;
    mismatched_owner.hash_major_index_reader.retained_owner_bytes = 81;
    aggregate.merge(mismatched_owner);
    REQUIRE(aggregate.has_operational_identity_conflict);

    pipeline_operational_stats at_limit{};
    at_limit.hash_major_index_reader.directory_rows =
        k_sql_bigint_value_max;
    pipeline_operational_stats overflow{};
    overflow.hash_major_index_reader.directory_rows = 1;
    at_limit.merge(overflow);
    REQUIRE(
        at_limit.hash_major_index_reader.directory_rows
        == k_sql_bigint_value_max);
    REQUIRE(at_limit.hash_major_index_reader.is_saturated);
    REQUIRE(at_limit.are_stats_saturated);
}

TEST_CASE("aggregate environment drift reclassifies both job sets",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto aggregate_job = [](const pipeline_job_execution_stats& job) {
        pipeline_execution_stats stats{};
        stats.merge_job(job);
        return stats;
    };
    const auto require_reclassified = [](const pipeline_execution_stats& stats) {
        REQUIRE(stats.environment.is_mixed_invalid);
        REQUIRE(stats.attempted_alignment_jobs == 2);
        REQUIRE(stats.fully_attested_alignment_jobs == 0);
        REQUIRE(stats.unattested_alignment_jobs == 2);
    };

    auto worker_left_job = make_attested_job(
        executed_backend::simd_cpu, target_index_source::recomputed);
    auto worker_right_job = worker_left_job;
    worker_right_job.environment.effective_cpu_workers = 3;
    auto worker_left = aggregate_job(worker_left_job);
    worker_left.merge(aggregate_job(worker_right_job));
    require_reclassified(worker_left);

    auto device_left_job = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
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

TEST_CASE("per-job coverage does not multiply operational events",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats aggregate{};
    auto job = make_attested_job(
        executed_backend::vulkan_gpu, target_index_source::persisted);
    for (std::uint64_t i = 0; i < 4096; ++i) {
        job.logical_target_row = i;
        job.query_ordinal = i;
        aggregate.merge_job(job);
    }
    REQUIRE(aggregate.operational.target_rows == 0);

    pipeline_operational_stats once{};
    once.target_rows = 1;
    once.gpu_queue_submits = 1;
    once.gpu_persisted_resident_uploads = 1;
    aggregate.merge_operational(once);

    REQUIRE(aggregate.attempted_alignment_jobs == 4096);
    REQUIRE(aggregate.fully_attested_alignment_jobs == 4096);
    REQUIRE(aggregate.unattested_alignment_jobs == 0);
    REQUIRE(aggregate.first_unattested_target_row == k_unset_job_ordinal);
    REQUIRE(aggregate.stages.coverage[stage_index(pipeline_stage::query_sketch)].eligible_jobs
            == 4096);
    REQUIRE(aggregate.stages.coverage[stage_index(pipeline_stage::query_sketch)]
                .executed_jobs[engine_index(execution_engine::vulkan_gpu)]
            == 4096);
    REQUIRE(aggregate.operational.gpu_queue_submits == 1);
    REQUIRE(aggregate.operational.gpu_persisted_resident_uploads == 1);
    REQUIRE(aggregate.operational.target_rows == 1);
    REQUIRE_FALSE(aggregate.operational.are_stats_saturated);
}

TEST_CASE("dispatch primary and extra-query aggregates merge once",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    const auto incomplete_invocation = [](
            const std::uint64_t query_ordinal,
            const std::uint64_t seed_dispatches,
            const bool primary_invocation) {
        pipeline_execution_stats invocation{};
        pipeline_job_execution_stats job{};
        job.requested_backend = executed_backend::vulkan_gpu;
        job.actual_backend = executed_backend::mixed_invalid;
        job.requested_target_index_source = target_index_source::recomputed;
        job.actual_target_index_source = target_index_source::recomputed;
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

    pipeline_execution_stats completed_buffer{};
    completed_buffer.merge(incomplete_invocation(0, 1, true));
    completed_buffer.merge(incomplete_invocation(1, 2, false));

    REQUIRE(completed_buffer.requested_backend == executed_backend::vulkan_gpu);
    REQUIRE(completed_buffer.actual_backend == executed_backend::mixed_invalid);
    REQUIRE(completed_buffer.requested_target_index_source
            == target_index_source::recomputed);
    REQUIRE(completed_buffer.actual_target_index_source
            == target_index_source::recomputed);
    REQUIRE(completed_buffer.attempted_alignment_jobs == 2);
    REQUIRE(completed_buffer.fully_attested_alignment_jobs == 0);
    REQUIRE(completed_buffer.unattested_alignment_jobs == 2);
    REQUIRE(completed_buffer.first_unattested_target_row == 41);
    REQUIRE(completed_buffer.first_unattested_query_ordinal == 0);
    REQUIRE(completed_buffer.operational.target_rows == 1);
    REQUIRE(completed_buffer.operational.persisted_index_rows == 0);
    REQUIRE(completed_buffer.operational.recomputed_target_index_rows == 1);
    REQUIRE(completed_buffer.operational.gpu_seed_dispatches == 3);
    REQUIRE(completed_buffer.environment.is_mixed_invalid);
    REQUIRE_FALSE(completed_buffer.operational.are_stats_saturated);
}

TEST_CASE("persisted request cannot be laundered by recompute outcome",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using namespace bioseq::pipeline_stats;
    pipeline_execution_stats invocation{};
    pipeline_job_execution_stats job{};
    job.requested_backend = executed_backend::simd_cpu;
    job.actual_backend = executed_backend::mixed_invalid;
    job.requested_simd_chaining_policy =
        simd_chaining_policy_identity::bounded_batch_approximate;
    job.observed_simd_chaining_policy = simd_chaining_policy_identity::none;
    job.requested_target_index_source = target_index_source::persisted;
    job.actual_target_index_source = target_index_source::recomputed;
    job.logical_target_row = 19;
    job.query_ordinal = 3;
    invocation.merge_job(job);

    pipeline_operational_stats operational{};
    operational.target_rows = 1;
    operational.missing_persisted_index_rows = 1;
    operational.recomputed_target_index_rows = 1;
    invocation.merge_operational(operational);

    REQUIRE(invocation.requested_target_index_source
            == target_index_source::persisted);
    REQUIRE(invocation.actual_target_index_source
            == target_index_source::recomputed);
    REQUIRE(invocation.attempted_alignment_jobs == 1);
    REQUIRE(invocation.fully_attested_alignment_jobs == 0);
    REQUIRE(invocation.unattested_alignment_jobs == 1);
    REQUIRE(invocation.first_unattested_target_row == 19);
    REQUIRE(invocation.first_unattested_query_ordinal == 3);
    REQUIRE(invocation.operational.target_rows == 1);
    REQUIRE(invocation.operational.persisted_index_rows == 0);
    REQUIRE(invocation.operational.missing_persisted_index_rows == 1);
    REQUIRE(invocation.operational.recomputed_target_index_rows == 1);
    REQUIRE_FALSE(invocation.has_mixed_persisted_identity);
}

TEST_CASE("logical target identity survives payload coalescing",
          "[component:bioseq][layer:unit][feature:pipeline]") {
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


namespace bioseq::simd::pipe {

// ============================================================================
// Probe sink that counts on_batch invocations.
// ============================================================================

struct probe_batch_sink {
    int* on_batch_count;
    int* on_completed_count;
    [[gnu::always_inline]] void on_batch(std::span<alignment_region> /*regions*/) {
        ++*on_batch_count;
    }
    [[gnu::always_inline]] void on_completed() noexcept { ++*on_completed_count; }
};

struct exact_filter_probe_sink {
    std::vector<alignment_region>* observed;
    int* on_batch_count;

    void on_batch_pair(
        std::span<alignment_region> regions,
        std::span<const ::bioseq::chaining::anchor> /*anchors*/) {
        observed->assign(regions.begin(), regions.end());
        ++*on_batch_count;
    }

    void on_completed() noexcept {}
};

struct prefilter_probe_sink {
    std::vector<alignment_region>* observed;
    int* on_batch_count;

    void on_batch_pair(
        std::span<alignment_region> regions,
        std::span<const ::bioseq::chaining::anchor> /*anchors*/) {
        observed->assign(regions.begin(), regions.end());
        ++*on_batch_count;
    }

    void on_completed() noexcept {}
};

TEST_CASE("emit_token trait surface", "[component:bioseq][layer:unit][feature:pipeline]") {
    SECTION("EmitTokenTraits") {
        static_assert(!std::is_copy_constructible_v<emit_token<sink_stage>>);
        static_assert(!std::is_move_constructible_v<emit_token<sink_stage>>);
        static_assert(!std::is_default_constructible_v<emit_token<sink_stage>>);
        SUCCEED();
    }
    SECTION("EmittedTokenTraits") {
        static_assert(!std::is_default_constructible_v<emitted_token<sink_stage>>);
        static_assert(!std::is_copy_constructible_v<emitted_token<sink_stage>>);
        SUCCEED();
    }
    SECTION("EmitMethodReturnType") {
        // emit_token::emit_batch returns emitted_token<Sink>.
        // The expression is evaluated only inside `std::declval`, so no
        // emit_token instance is actually constructed (it is non-default-
        // and non-move-constructible).
        static_assert(std::is_same_v<
            decltype(std::declval<emit_token<sink_stage>>().emit_batch(
                std::declval<std::span<alignment_region>>())),
            emitted_token<sink_stage>>);
        SUCCEED();
    }
    SECTION("SinkConceptHolds") {
        static_assert(batch_sink<sink_stage, alignment_region>);
        static_assert(!batch_sink<sink_stage, ::bioseq::chaining::anchor>);
        SUCCEED();
    }
    SECTION("ProbeBatchSinkConceptHolds") {
        static_assert(batch_sink<probe_batch_sink, alignment_region>);
        SUCCEED();
    }
}

TEST_CASE("mapq_stage emits exactly one batch per upstream batch",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    probe_batch_sink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::minimap2_parameters params;
    params.compute_mapq = false;
    mapq_stage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<cep_type>(probe);

    std::vector<alignment_region> regions(1);
    bound.on_batch(std::span<alignment_region>{regions.data(), regions.size()});
    REQUIRE(on_batch_count == 1);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

TEST_CASE("mapq_stage::process_pair delegates exactly once",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    probe_batch_sink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::minimap2_parameters params;
    mapq_stage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<cep_type>(probe);

    std::vector<alignment_region> regions(2);
    std::vector<::bioseq::chaining::anchor> anchors(2);
    bound.on_batch_pair(
        std::span<alignment_region>{regions.data(), regions.size()},
        std::span<::bioseq::chaining::anchor>{anchors.data(), anchors.size()});
    REQUIRE(on_batch_count == 1);
}

TEST_CASE("exact_edit_filter_stage applies strict bounds and resets classification state",
          "[component:bioseq][layer:unit][feature:alignment]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    ::bioseq::simd::minimap2_parameters params;
    params.compute_edit_distance = true;
    params.exact_edit_filter_enabled = true;
    params.exact_edit_filter_exclusive_upper_bound = 5;

    std::vector<alignment_region> observed;
    int on_batch_count = 0;
    exact_filter_probe_sink probe{&observed, &on_batch_count};
    exact_edit_filter_stage stage{{}, params};
    auto bound = stage.template attach<cep_type>(probe);

    std::vector<alignment_region> regions(4);
    for (std::size_t index = 0; index < regions.size(); ++index) {
        alignment_region& region = regions[index];
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
    std::vector<::bioseq::chaining::anchor> anchors;

    bound.on_batch_pair(
        std::span<alignment_region>{regions},
        std::span<::bioseq::chaining::anchor>{anchors});

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

TEST_CASE("exact_edit_filter_stage rejects nonpositive bounds and missing traceback",
          "[component:bioseq][layer:unit][feature:alignment]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    std::vector<::bioseq::chaining::anchor> anchors;

    SECTION("nonpositive bound rejects every exact alignment") {
        ::bioseq::simd::minimap2_parameters params;
        params.compute_edit_distance = true;
        params.exact_edit_filter_enabled = true;
        params.exact_edit_filter_exclusive_upper_bound = 0;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        exact_filter_probe_sink probe{&observed, &on_batch_count};
        exact_edit_filter_stage stage{{}, params};
        auto bound = stage.template attach<cep_type>(probe);
        std::vector<alignment_region> regions(1);
        regions[0].has_alignment = true;
        regions[0].edit_distance = 0;

        bound.on_batch_pair(
            std::span<alignment_region>{regions},
            std::span<::bioseq::chaining::anchor>{anchors});
        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.empty());
    }

    SECTION("missing traceback fails loudly") {
        ::bioseq::simd::minimap2_parameters params;
        params.compute_edit_distance = true;
        params.exact_edit_filter_enabled = true;
        params.exact_edit_filter_exclusive_upper_bound = 1;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        exact_filter_probe_sink probe{&observed, &on_batch_count};
        exact_edit_filter_stage stage{{}, params};
        auto bound = stage.template attach<cep_type>(probe);
        std::vector<alignment_region> regions(1);
        regions[0].has_alignment = false;

        REQUIRE_THROWS_WITH(
            bound.on_batch_pair(
                std::span<alignment_region>{regions},
                std::span<::bioseq::chaining::anchor>{anchors}),
            "exact edit filter received a region without alignment traceback");
        REQUIRE(on_batch_count == 0);
    }
}

// `SingleEmissionInvariant`.
//
// Drives N=8 invocations through the bound mapq_stage with a probe sink
// and asserts the probe records exactly 8 on_batch invocations.  This
// mirrors the spec's intent of "every process*<Cep> emits exactly one
// on_batch* per upstream on_batch*" — the bound stage is the unit of
// stage-shape composition, so verifying the contract on one bound
// stage exercises the contract shared by every stage in the pipeline (every
// `process*<Cep>` body uses the same emit_token/emitted_token machinery).
TEST_CASE("SingleEmissionInvariant — N=8 queries one emission each",
          "[component:bioseq][layer:unit][feature:pipeline]")
{
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    int on_batch_count = 0;
    int on_completed_count = 0;
    probe_batch_sink probe{&on_batch_count, &on_completed_count};

    ::bioseq::simd::minimap2_parameters params;
    mapq_stage stage{{}, params, /*enabled=*/false};
    auto bound = stage.template attach<cep_type>(probe);

    constexpr int num_queries = 8;
    std::vector<alignment_region> regions(1);
    for (int qi = 0; qi < num_queries; ++qi) {
        bound.on_batch(std::span<alignment_region>{regions.data(), regions.size()});
    }
    REQUIRE(on_batch_count == num_queries);
    bound.on_completed();
    REQUIRE(on_completed_count == 1);
}

// `KSW2BatchedParity` checks whether the trait surface for
// `BAA = KSW2` and `BAA = KSW2_BATCHED` agrees on the algorithm-label
// and runtime-parameter row count.  A full end-to-end parity check
// against `minimap2_port_with_index_batch` would require live
// sequences; the trait surface check verifies the structural contract.
TEST_CASE("KSW2BatchedParity — trait surface matches KSW2",
          "[component:bioseq][layer:unit][feature:alignment]")
{
    using ksw2_kernel_type     = kernel_t<::bioseq::simd::
                                  base_alignment_algorithm::ksw2>;
    using batched_kernel_type  = kernel_t<::bioseq::simd::
                                  base_alignment_algorithm::ksw2_batched>;

    ::bioseq::simd::minimap2_parameters params;
    std::vector<named_property> ksw2_params;
    std::vector<named_property> batched_params;
    ksw2_kernel_type::populate_runtime_params(params, ksw2_params);
    batched_kernel_type::populate_runtime_params(params, batched_params);
    REQUIRE(ksw2_params.size() == batched_params.size());
    for (std::size_t i = 0; i < ksw2_params.size(); ++i) {
        REQUIRE(ksw2_params[i].name == batched_params[i].name);
        REQUIRE(ksw2_params[i].value == batched_params[i].value);
    }
    REQUIRE(std::string{ksw2_kernel_type::algorithm_label()} == "ksw2");
    REQUIRE(std::string{batched_kernel_type::algorithm_label()}
            == "ksw2 (inter-region batched)");
    static_assert(!ksw2_kernel_type::emits_ksw2_batched_execution_stats);
    static_assert(batched_kernel_type::emits_ksw2_batched_execution_stats);
}

TEST_CASE("KSW2 batched stats rejects zero and tampered observations",
          "[component:bioseq][layer:unit][feature:alignment]") {
    using ::bioseq::simd::ksw2_batched_execution_stats;
    using ::bioseq::simd::match_result_collection;

    ksw2_batched_execution_stats empty{};
    REQUIRE_FALSE(empty.is_coherent(false));
    REQUIRE_FALSE(empty.is_coherent(true));

    ksw2_batched_execution_stats valid{};
    valid.record_invocation(1u);
    valid.record_collected_fills(2u);
    valid.record_short_bucket_dispatch(1u);
    valid.record_nonempty_fill(32u, 31u, false);
    valid.record_nonempty_fill(300u, 301u, true);
    REQUIRE(valid.is_coherent(true));

    auto tampered_lane_count = valid;
    ++tampered_lane_count.short_bucket_lanes;
    REQUIRE_FALSE(tampered_lane_count.is_coherent(true));

    auto dropped_fill = valid;
    ++dropped_fill.fills_collected;
    REQUIRE_FALSE(dropped_fill.is_coherent(true));

    auto tampered_area = valid;
    tampered_area.input_cell_area = 0u;
    REQUIRE_FALSE(tampered_area.is_coherent(true));

    auto impossible_dispatch_count = valid;
    impossible_dispatch_count.short_bucket_dispatches =
        impossible_dispatch_count.short_bucket_lanes + 1u;
    REQUIRE_FALSE(impossible_dispatch_count.is_coherent(true));

    auto impossible_cell_area = valid;
    impossible_cell_area.input_cell_area = 1u;
    REQUIRE_FALSE(impossible_cell_area.is_coherent(true));

    auto saturated = valid;
    saturated.saturated = true;
    REQUIRE_FALSE(saturated.is_coherent(true));

    ksw2_batched_execution_stats combined{};
    combined.merge(valid);
    combined.merge(valid);
    REQUIRE(combined.is_coherent(true));
    REQUIRE(combined.alignment_region_invocations == 2u);
    REQUIRE(combined.nonempty_fill_calls == 4u);

    match_result_collection first;
    first.record_ksw2_batched_execution_stats(valid);
    first.record_query_sketch_host_time(7u);
    first.record_seed_lookup_host_time(11u);
    first.record_chain_score_host_time(13u);
    first.record_region_generation_host_time(17u);
    first.record_alignment_result_host_time(19u);
    match_result_collection second;
    second.record_ksw2_batched_execution_stats(valid);
    second.record_query_sketch_host_time(11u);
    second.record_seed_lookup_host_time(19u);
    second.record_chain_score_host_time(23u);
    second.record_region_generation_host_time(29u);
    second.record_alignment_result_host_time(31u);
    first.combine_execution_stats(second);
    REQUIRE(first.is_ksw2_batched_execution_consistent(true));
    REQUIRE(first.ksw2_batched_stats.alignment_region_invocations
            == 2u);
    REQUIRE(first.query_sketch_host_wall_ns == 18u);
    REQUIRE(first.seed_lookup_host_wall_ns == 30u);
    REQUIRE(first.chain_score_host_wall_ns == 36u);
    REQUIRE(first.region_generation_host_wall_ns == 46u);
    REQUIRE(first.alignment_result_host_wall_ns == 50u);
    REQUIRE_FALSE(first.is_host_stage_timing_saturated);
}

TEST_CASE("Host timing region handoff is non-overlapping and mergeable",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;

    match_result_collection collection;
    collection.begin_region_host_stage(100u);
    collection.transition_region_to_alignment_result_host_stage(
        130u, /*alignment_enabled=*/true);
    collection.finish_alignment_result_host_stage(180u);

    REQUIRE(collection.region_generation_host_wall_ns == 30u);
    REQUIRE(collection.alignment_result_host_wall_ns == 50u);
    REQUIRE(collection.region_generation_host_wall_ns
            + collection.alignment_result_host_wall_ns == 80u);
    REQUIRE_FALSE(collection.is_region_host_stage_active);
    REQUIRE_FALSE(collection.is_alignment_result_host_stage_active);
    REQUIRE_FALSE(collection.is_host_stage_timing_saturated);

    match_result_collection saturated;
    saturated.record_alignment_result_host_time(
        (std::numeric_limits<std::uint64_t>::max)());
    saturated.record_alignment_result_host_time(1u);
    REQUIRE(saturated.alignment_result_host_wall_ns
            == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(saturated.is_host_stage_timing_saturated);
}

// =============================================================================
// Chain primitive, registry, and leaf tests.
// =============================================================================

namespace dispatch_test_support {

using base_alignment_algorithm_type = ::bioseq::simd::base_alignment_algorithm;
using chaining_algorithm_type  = ::bioseq::simd::chaining_algorithm;
using minimizer_algorithm_type  = ::bioseq::simd::minimizer_algorithm;

/// Probe callable for `dispatch_exact_value_or_default` tests. Stateless POD with a
/// templated `operator()` whose body forwards the matched IC<T, V> into a
/// caller-provided reference slot.
template <class p_t>
struct value_capture_callable {
    p_t* sink;
    template <p_t p_v, class... p_args>
    [[gnu::always_inline]] p_t operator()(
        std::integral_constant<p_t, p_v>, p_args&&...) const
    {
        *sink = p_v;
        return p_v;
    }
};

}  // namespace dispatch_test_support

TEST_CASE("dispatch_exact_value_or_default routes to the matching template branch",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    SECTION("chaining_algorithm — every enum value routes correctly") {
        using registry_type = algorithm_enum_values<chaining_algorithm_type>;
        for (chaining_algorithm_type v : {chaining_algorithm_type::scalar_minimap2, chaining_algorithm_type::simd_minimap2, chaining_algorithm_type::range_minimum_query, chaining_algorithm_type::vulkan_tiled_dynamic_programming}) {
            chaining_algorithm_type captured = static_cast<chaining_algorithm_type>(0xFF);
            dispatch_test_support::value_capture_callable<chaining_algorithm_type> cb{&captured};
            const chaining_algorithm_type result = dispatch_exact_value_or_default<chaining_algorithm_type, chaining_algorithm_type, registry_type::default_value>(
                registry_type::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
    SECTION("base_alignment_algorithm — every enum value routes correctly") {
        using registry_type = algorithm_enum_values<base_alignment_algorithm_type>;
        for (base_alignment_algorithm_type v : {base_alignment_algorithm_type::ksw2, base_alignment_algorithm_type::block_aligner, base_alignment_algorithm_type::ksw2_batched}) {
            base_alignment_algorithm_type captured = static_cast<base_alignment_algorithm_type>(0xFF);
            dispatch_test_support::value_capture_callable<base_alignment_algorithm_type> cb{&captured};
            const base_alignment_algorithm_type result = dispatch_exact_value_or_default<base_alignment_algorithm_type, base_alignment_algorithm_type, registry_type::default_value>(
                registry_type::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
    SECTION("minimizer_algorithm — every enum value routes correctly") {
        using registry_type = algorithm_enum_values<minimizer_algorithm_type>;
        for (minimizer_algorithm_type v : {minimizer_algorithm_type::scalar,
                     minimizer_algorithm_type::simd_minimap2_reference,
                     minimizer_algorithm_type::two_stack_nthash,
                     minimizer_algorithm_type::vulkan_minimap_integer,
                     minimizer_algorithm_type::bounded_nthash_simd}) {
            minimizer_algorithm_type captured = static_cast<minimizer_algorithm_type>(0xFF);
            dispatch_test_support::value_capture_callable<minimizer_algorithm_type> cb{&captured};
            const minimizer_algorithm_type result = dispatch_exact_value_or_default<minimizer_algorithm_type, minimizer_algorithm_type, registry_type::default_value>(
                registry_type::value_list{}, v, cb);
            REQUIRE(captured == v);
            REQUIRE(result == v);
        }
    }
}

TEST_CASE("Exact SIMD chaining metadata permits canonical nonzero skip penalty",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using chaining_algorithm_type = ::bioseq::simd::chaining_algorithm;
    using kernel_type = algorithm_traits<chaining_algorithm_type::simd_minimap2>::kernel_type;
    using cep_type = static_aligner_parameters<10, 15, 28>;
    const auto properties = kernel_type::template static_properties<cep_type>();
    const auto property = std::ranges::find_if(
        properties,
        [](const named_property& candidate) {
            return candidate.name == "requires_zero_skip_penalty";
        });
    REQUIRE(property != properties.end());
    REQUIRE(property->value == "false");
}

TEST_CASE("scalar chaining trait keeps an empty negative free of score work",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using ::bioseq::chaining::anchor;
    using ::bioseq::chaining::chain;
    using ::bioseq::chaining::chaining_parameters;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::scalar_pipeline_execution_stats;
    using kernel_type =
        algorithm_traits<chaining_algorithm::scalar_minimap2>::kernel_type;

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto chains = ::bioseq::chaining::make_arena_slab<chain>();
    auto reordered = ::bioseq::chaining::make_arena_slab<anchor>();
    scalar_pipeline_execution_stats stats{};
    kernel_type::call(
        std::span<const anchor>{}, chaining_parameters{}, chains, reordered,
        &stats);

    REQUIRE(chains.size() == 0u);
    REQUIRE(reordered.size() == 0u);
    REQUIRE(stats.chain_score_calls == 0u);
    REQUIRE(stats.chain_backtrack_calls == 0u);
    REQUIRE(stats.chain_anchor_inputs == 0u);
    REQUIRE(stats.chain_stage_completion_calls == 0u);
    REQUIRE(stats.chain_stage_completed_anchor_inputs == 0u);
}

TEST_CASE("SIMD chaining policy is exact by default and explicit in stats metadata",
          "[component:bioseq][layer:unit][feature:chaining]") {
    using ::bioseq::chaining::simd_chaining_policy;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::chaining::simd_chaining_execution_stats;
    using kernel_type = algorithm_traits<chaining_algorithm::simd_minimap2>::kernel_type;

    minimap2_parameters params{};
    REQUIRE(params.simd_chaining_policy
        == simd_chaining_policy::exact_visited);
    REQUIRE(params.make_chaining_parameters().simd_policy
        == simd_chaining_policy::exact_visited);

    params.simd_chaining_policy =
        simd_chaining_policy::bounded_batch_approximate;
    const auto bounded_params = params.make_chaining_parameters();
    REQUIRE(bounded_params.simd_policy
        == simd_chaining_policy::bounded_batch_approximate);

    std::vector<named_property> runtime_properties;
    kernel_type::populate_runtime_params(bounded_params, runtime_properties);
    const auto policy_property = std::ranges::find_if(
        runtime_properties,
        [](const named_property& candidate) {
            return candidate.name == "simd_chaining_policy";
        });
    REQUIRE(policy_property != runtime_properties.end());
    REQUIRE(policy_property->value == "bounded_batch_approximate");

    simd_chaining_execution_stats exact{};
    exact.policy.exact_visited_calls = 1u;
    match_result_collection collection;
    collection.record_simd_chaining_execution_stats(exact);
    REQUIRE(collection.is_simd_chaining_execution_consistent(
        simd_chaining_policy::exact_visited,
        /*require_nonempty_vector_work=*/false));
    REQUIRE_FALSE(collection.is_simd_chaining_execution_consistent(
        simd_chaining_policy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/false));

    simd_chaining_execution_stats bounded{};
    bounded.policy.bounded_batch_approximate_calls = 1u;
    match_result_collection mixed;
    mixed.record_simd_chaining_execution_stats(exact);
    mixed.record_simd_chaining_execution_stats(bounded);
    REQUIRE_FALSE(mixed.is_simd_chaining_execution_consistent(
        simd_chaining_policy::exact_visited,
        /*require_nonempty_vector_work=*/false));
    REQUIRE_FALSE(mixed.is_simd_chaining_execution_consistent(
        simd_chaining_policy::bounded_batch_approximate,
        /*require_nonempty_vector_work=*/false));
}

TEST_CASE("scalar pipeline stats is operation-local and rejects mixed routes",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::partial_vulkan_execution_stats;
    using ::bioseq::simd::scalar_ksw2_stage;
    using ::bioseq::simd::scalar_minimizer_execution_stats;
    using ::bioseq::simd::scalar_pipeline_execution_stats;

    scalar_minimizer_execution_stats query{};
    query.record_input(5'000u);
    scalar_minimizer_execution_stats target{};
    target.record_input(2'000'000u);

    scalar_pipeline_execution_stats recomputed{};
    recomputed.query_minimizers = query;
    recomputed.target_minimizers = target;
    recomputed.record_seed_lookup(400u);
    recomputed.record_scalar_chain(1'200u);
    recomputed.record_scalar_chain_stage_completion(1'200u);
    recomputed.record_ksw2_call(
        scalar_ksw2_stage::left, 100u, 120u);
    recomputed.record_ksw2_call(
        scalar_ksw2_stage::gap, 80u, 90u);

    match_result_collection complete;
    complete.record_scalar_pipeline_execution_stats(recomputed);
    REQUIRE(complete.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));
    REQUIRE_FALSE(complete.is_all_simd_execution_consistent(
        ::bioseq::simd::minimizer_algorithm::
            two_stack_nthash,
        ::bioseq::chaining::simd_chaining_policy::exact_visited,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));

    scalar_pipeline_execution_stats persisted = recomputed;
    persisted.target_minimizers = {};
    match_result_collection persisted_collection;
    persisted_collection.record_scalar_pipeline_execution_stats(persisted);
    REQUIRE(persisted_collection.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/false,
        /*require_alignment=*/true));
    REQUIRE_FALSE(persisted_collection.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    match_result_collection no_alignment;
    scalar_pipeline_execution_stats coordinate_only = persisted;
    coordinate_only.ksw2_left_calls = 0;
    coordinate_only.ksw2_gap_calls = 0;
    coordinate_only.ksw2_right_calls = 0;
    coordinate_only.ksw2_input_cell_area = 0;
    no_alignment.record_scalar_pipeline_execution_stats(coordinate_only);
    REQUIRE(no_alignment.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/false,
        /*require_alignment=*/false));
    REQUIRE_FALSE(no_alignment.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/false,
        /*require_alignment=*/true));

    /* A real zero-anchor scalar chain is an attested negative operation.  Its
     * successful stage receipt is distinct from score/backtrack work, and it
     * cannot waive a selected KSW2 alignment requirement. */
    scalar_pipeline_execution_stats zero_anchor = recomputed;
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
    match_result_collection zero_anchor_negative;
    zero_anchor_negative.record_scalar_pipeline_execution_stats(zero_anchor);
    REQUIRE(zero_anchor_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));
    REQUIRE_FALSE(zero_anchor_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    scalar_pipeline_execution_stats impossible_empty_chain = zero_anchor;
    impossible_empty_chain.chain_score_calls = 1;
    impossible_empty_chain.chain_backtrack_calls = 1;
    match_result_collection impossible_negative;
    impossible_negative.record_scalar_pipeline_execution_stats(
        impossible_empty_chain);
    REQUIRE_FALSE(impossible_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));

    scalar_pipeline_execution_stats missing_chain_receipt = zero_anchor;
    missing_chain_receipt.chain_stage_completion_calls = 0;
    match_result_collection incomplete_negative;
    incomplete_negative.record_scalar_pipeline_execution_stats(
        missing_chain_receipt);
    REQUIRE_FALSE(incomplete_negative.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/false));

    match_result_collection mixed = complete;
    mixed.exact_simd_query_execution_stats_available = true;
    REQUIRE_FALSE(mixed.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    match_result_collection gpu_mixed = complete;
    partial_vulkan_execution_stats gpu{};
    gpu.seed_lookup_dispatches = 1u;
    gpu_mixed.record_partial_vulkan_execution_stats(gpu);
    REQUIRE_FALSE(gpu_mixed.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));

    match_result_collection saturated = complete;
    saturated.scalar_pipeline_stats.is_saturated = true;
    REQUIRE_FALSE(saturated.is_all_scalar_execution_consistent(
        /*require_target_sketch=*/true,
        /*require_alignment=*/true));
}

TEST_CASE("exact SIMD collection stats merge valid observations and reject tampering",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::two_stack_execution_stats;

    const auto actual_range_stats = [](std::uint64_t bp) {
        two_stack_execution_stats stats{};
        stats.bp_consumed = bp;
        stats.fragment_symbols_staged = bp;
        stats.packed_decode_lanes = bp;
        stats.generic_u64_kernel_calls = 1;
        stats.event_generation_batches = 1;
        stats.stable_compress_batches = 1;
        if (bp != 0) {
            stats.vector_batches = 1;
            stats.vector_lanes_visited = bp;
            stats.generic_u64_candidate_blocks = 1;
            stats.generic_u64_lane_visits = bp;
            stats.consecutive_kmer_batches = 1;
            stats.canonical_scramble_batches = 1;
            stats.window_reduction_batches = 1;
            stats.event_generation_batches = 2;
            stats.stable_compress_batches = 2;
            stats.tie_matrix_event_batches = 1;
            stats.row_transpose_batches = 1;
            stats.row_vector_stores = 1;
            stats.packed_decode_batches = 1;
        }
        return stats;
    };

    match_result_collection first;
    first.record_exact_simd_target_execution_stats(
        actual_range_stats(17));
    match_result_collection second;
    second.record_exact_simd_target_execution_stats(
        actual_range_stats(23));
    first.combine_execution_stats(second);
    REQUIRE(first.exact_simd_target_execution_stats_available);
    REQUIRE(first.exact_simd_target_execution_stats.bp_consumed == 40);
    REQUIRE(first.exact_simd_target_execution_stats.packed_decode_lanes == 40);
    REQUIRE(first.is_exact_simd_target_execution_consistent(
        /*require_nonempty_vector_work=*/true));

    match_result_collection missing;
    REQUIRE_FALSE(missing.is_exact_simd_target_execution_consistent(false));

    match_result_collection manufactured_empty;
    manufactured_empty.record_exact_simd_query_execution_stats(
        two_stack_execution_stats{});
    REQUIRE_FALSE(
        manufactured_empty.is_exact_simd_query_execution_consistent(false));

    match_result_collection tampered;
    auto forbidden = actual_range_stats(8);
    forbidden.scalar_hash_symbols = 1;
    tampered.record_exact_simd_target_execution_stats(forbidden);
    REQUIRE_FALSE(tampered.is_exact_simd_target_execution_consistent(false));

    // An actual zero-bp invocation is valid route observations for ordinary
    // diagnostics, but it cannot satisfy the strict positive-work gate.
    match_result_collection empty_actual;
    empty_actual.record_exact_simd_query_execution_stats(
        actual_range_stats(0));
    REQUIRE(empty_actual.is_exact_simd_query_execution_consistent(false));
    REQUIRE_FALSE(empty_actual.is_exact_simd_query_execution_consistent(true));

    auto mixed_routes = actual_range_stats(11);
    mixed_routes.kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 1;
    mixed_routes.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks =
        mixed_routes.generic_u64_candidate_blocks;
    mixed_routes.kmer_15_window_10_byte_8_uint32_hash_lane_visits =
        mixed_routes.generic_u64_lane_visits;
    mixed_routes.generic_u64_candidate_blocks = 0;
    mixed_routes.generic_u64_lane_visits = 0;
    match_result_collection mixed;
    mixed.record_exact_simd_query_execution_stats(mixed_routes);
    REQUIRE_FALSE(mixed.is_exact_simd_query_execution_consistent(false));

    auto missing_packed22 = actual_range_stats(11);
    missing_packed22.kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 1;
    missing_packed22.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks =
        missing_packed22.generic_u64_candidate_blocks;
    missing_packed22.kmer_15_window_10_byte_8_uint32_hash_lane_visits =
        missing_packed22.generic_u64_lane_visits;
    missing_packed22.generic_u64_kernel_calls = 0;
    missing_packed22.generic_u64_candidate_blocks = 0;
    missing_packed22.generic_u64_lane_visits = 0;
    match_result_collection missing_packed;
    missing_packed.record_exact_simd_query_execution_stats(
        missing_packed22);
    REQUIRE_FALSE(
        missing_packed.is_exact_simd_query_execution_consistent(false));

    auto packed22 = missing_packed22;
    packed22.kmer_15_window_10_byte_8_packed_22_groups =
        packed22.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks;
    packed22.kmer_15_window_10_byte_8_packed_22_lane_visits =
        packed22.kmer_15_window_10_byte_8_uint32_hash_lane_visits;
    match_result_collection packed;
    packed.record_exact_simd_query_execution_stats(packed22);
    REQUIRE(packed.is_exact_simd_query_execution_consistent(false));

    ++packed22.kmer_15_window_10_byte_8_packed_22_lane_visits;
    match_result_collection tampered_packed;
    tampered_packed.record_exact_simd_query_execution_stats(packed22);
    REQUIRE_FALSE(
        tampered_packed.is_exact_simd_query_execution_consistent(false));
}

TEST_CASE("partial Vulkan stats requires the exact positive stage mask",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::partial_vulkan_execution_stats;

    match_result_collection tiled_chain;
    partial_vulkan_execution_stats chain_only{};
    chain_only.chain_score_dispatches = 1;
    tiled_chain.record_partial_vulkan_execution_stats(chain_only);
    REQUIRE(tiled_chain.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    false, false, false, false, true));

    match_result_collection ordered_sketch;
    partial_vulkan_execution_stats frontend{};
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
    ordered_sketch.record_partial_vulkan_execution_stats(frontend);
    REQUIRE(ordered_sketch.partial_vulkan_stats
                .is_exact_stage_mask_consistent(true, true, true, true, true));
    REQUIRE_FALSE(ordered_sketch.partial_vulkan_stats
                      .is_exact_stage_mask_consistent(
                          true, true, false, true, true));
    auto tampered_frontend = frontend;
    tampered_frontend.gpu_index_atomic_append_dispatches = 1u;
    REQUIRE_FALSE(tampered_frontend.is_exact_stage_mask_consistent(
        true, true, true, true, true));
    tampered_frontend = frontend;
    tampered_frontend.gpu_index_exact_count_prefix_scan_dispatches = 2u;
    REQUIRE_FALSE(tampered_frontend.is_exact_stage_mask_consistent(
        true, true, true, true, true));

    partial_vulkan_execution_stats source_direct{};
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
    REQUIRE(source_direct.is_exact_source_direct_stage_mask_consistent(
        true, true, true, true));
    REQUIRE_FALSE(source_direct.is_exact_stage_mask_consistent(
        true, true, false, true, true));
    source_direct.gpu_index_gate_enabled = 1u;
    REQUIRE_FALSE(source_direct.is_exact_source_direct_stage_mask_consistent(
        true, true, true, true));

    match_result_collection combined;
    combined.combine_execution_stats(tiled_chain);
    REQUIRE(combined.has_any_algorithm_execution_stats());
}

TEST_CASE("ordered sketch batch target-build stats is not inflated by query count",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::partial_vulkan_execution_stats;
    using ::bioseq::simd::
        is_ordered_target_build_owner_query;

    partial_vulkan_execution_stats aggregate{};
    for (std::size_t query_index = 0; query_index < 3u; ++query_index) {
        partial_vulkan_execution_stats per_query{};
        per_query.query_sketch_dispatches = 1u;
        per_query.seed_lookup_dispatches = 1u;
        per_query.chain_score_dispatches = 1u;
        if (is_ordered_target_build_owner_query(query_index)) {
            per_query.target_sketch_dispatches = 1u;
            per_query.target_index_build_dispatches = 1u;
            per_query.gpu_index_count_admissions = 1u;
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
        aggregate.merge(per_query);
    }

    REQUIRE(aggregate.query_sketch_dispatches == 3u);
    REQUIRE(aggregate.seed_lookup_dispatches == 3u);
    REQUIRE(aggregate.chain_score_dispatches == 3u);
    REQUIRE(aggregate.target_sketch_dispatches == 1u);
    REQUIRE(aggregate.target_index_build_dispatches == 1u);
    REQUIRE(aggregate.gpu_index_count_admissions == 1u);
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

TEST_CASE("ordered sketch query gate becomes immutable after publication",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_type =
        ::bioseq::simd::minimizer_representation<15u, 15u>;
    using storage_type = ::bioseq::simd::pipe::
        ordered_sketch_query_gate_storage;

    storage_type storage;
    const std::array<minimizer_type, 1> minimizers{
        minimizer_type{17u, 0u, 0u}};
    storage.add(std::span<const minimizer_type>{minimizers});
    const auto identity = storage.canonical_identity();
    const auto gate = storage.vulkan_gate();

    REQUIRE(identity.enabled);
    REQUIRE(identity.word_count
            == bioseq::gpu::vulkan::k_gpu_query_gate_word_count);
    REQUIRE(identity.mask == bioseq::gpu::vulkan::k_gpu_query_gate_mask);
    REQUIRE(gate.identity_valid);
    REQUIRE(gate.words.data() == gate.identity.canonical_words->data());
    REQUIRE(gate.canonical_identity().is_exactly_equal_to(gate.identity));
    REQUIRE_THROWS_AS(
        storage.add(std::span<const minimizer_type>{minimizers}),
        bioseq::gpu::vulkan::vulkan_runtime_error);
}

TEST_CASE("segmented GPU adoption key preserves the exact frozen gate",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using gate_identity_type = bioseq::gpu::vulkan::gpu_index_gate_identity;
    using key_type = bioseq::indexing::gpu_minimizer_index_cache_key;
    using ::bioseq::simd::pipe::
        bind_verified_gpu_segment_gate_identity;
    using ::bioseq::simd::pipe::freeze_gpu_query_gate;

    std::vector<std::uint32_t> mutable_words(
        bioseq::gpu::vulkan::k_gpu_query_gate_word_count, 0u);
    mutable_words.front() = 1u;
    const auto frozen_gate = freeze_gpu_query_gate(
        bioseq::gpu::vulkan::vulkan_sketch_query_gate{
            std::span<const std::uint32_t>{mutable_words},
            bioseq::gpu::vulkan::k_gpu_query_gate_mask});
    const gate_identity_type expected = frozen_gate.canonical_identity();
    REQUIRE(expected.canonical_words);
    REQUIRE(frozen_gate.words.data() == expected.canonical_words->data());
    REQUIRE(frozen_gate.words.data() != mutable_words.data());

    std::size_t segment_bind_calls = 0u;
    key_type key{};
    for (std::size_t segment = 0; segment < 3u; ++segment) {
        const gate_identity_type built = frozen_gate.canonical_identity();
        REQUIRE(built.canonical_words.get()
                == expected.canonical_words.get());
        key = key_type{
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
        bioseq::gpu::vulkan::vulkan_runtime_error);
}

TEST_CASE("Vulkan sketch dispatch token requires an operation-completed roundtrip",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using bioseq::gpu::vulkan::vulkan_sketch_dispatch_route;
    using bioseq::gpu::vulkan::vulkan_sketch_dispatch_token;

    REQUIRE_FALSE(vulkan_sketch_dispatch_token{}.is_internally_consistent());

    const auto completed =
        vulkan_sketch_dispatch_token::completed_collect_minimizers_roundtrip();
    REQUIRE(completed.is_internally_consistent());

    auto no_roundtrip = completed;
    no_roundtrip.completed_roundtrips = 0;
    REQUIRE_FALSE(no_roundtrip.is_internally_consistent());

    auto wrong_count = completed;
    wrong_count.completed_roundtrips = 2;
    REQUIRE_FALSE(wrong_count.is_internally_consistent());

    auto wrong_route = completed;
    wrong_route.route = vulkan_sketch_dispatch_route::none;
    REQUIRE_FALSE(wrong_route.is_internally_consistent());
}

TEST_CASE("Vulkan sketch stats rejects an absent or default operation token",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::match_result_collection;
    using stats = ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_role;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_stats_matches_selected_route;
    using ::bioseq::simd::pipe_entry_detail::
        record_minimizer_sketch_execution_stats;

    const stats absent{};
    REQUIRE(absent.empty());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, absent));

    stats already_promoted_without_token{};
    already_promoted_without_token.partial_vulkan.query_sketch_dispatches = 1;
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, already_promoted_without_token));

    stats metadata_without_token{};
    metadata_without_token.partial_vulkan.gpu_index_unique_keys = 1;
    REQUIRE_FALSE(metadata_without_token.partial_vulkan.has_any_dispatch());
    REQUIRE_FALSE(metadata_without_token.empty());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::scalar, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::two_stack_nthash, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::bounded_nthash_simd, metadata_without_token));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, metadata_without_token));

    match_result_collection destination;
    record_minimizer_sketch_execution_stats(
        destination, absent, minimizer_sketch_role::query);
    record_minimizer_sketch_execution_stats(
        destination, already_promoted_without_token,
        minimizer_sketch_role::query);
    record_minimizer_sketch_execution_stats(
        destination, metadata_without_token, minimizer_sketch_role::query);
    REQUIRE_FALSE(destination.partial_vulkan_stats
                      .has_any_dispatch());
}

TEST_CASE("Vulkan sketch stats cannot promote a token across a tampered route",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::match_result_collection;
    using stats = ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_role;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_stats_matches_selected_route;
    using ::bioseq::simd::pipe_entry_detail::
        record_minimizer_sketch_execution_stats;

    using vulkan_token_type = bioseq::gpu::vulkan::vulkan_sketch_dispatch_token;
    const auto token = vulkan_token_type::completed_collect_minimizers_roundtrip();
    stats vulkan_sketch{};
    vulkan_sketch.partial_vulkan.sketch_dispatches =
        token.completed_roundtrips;
    REQUIRE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, vulkan_sketch));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::scalar, vulkan_sketch));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::two_stack_nthash, vulkan_sketch));

    stats mixed_route = vulkan_sketch;
    mixed_route.exact_simd_available = true;
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, mixed_route));

    stats token_with_metadata = vulkan_sketch;
    token_with_metadata.partial_vulkan.gpu_index_unique_keys = 1;
    REQUIRE_FALSE(token_with_metadata.partial_vulkan
                      .is_exact_sketch_token_consistent());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, token_with_metadata));

    stats repeated_roundtrip = vulkan_sketch;
    repeated_roundtrip.partial_vulkan.sketch_dispatches = 2;
    REQUIRE_FALSE(repeated_roundtrip.partial_vulkan
                      .is_exact_sketch_token_consistent());
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, repeated_roundtrip));

    match_result_collection repeated_roundtrip_destination;
    record_minimizer_sketch_execution_stats(
        repeated_roundtrip_destination, repeated_roundtrip,
        minimizer_sketch_role::query);
    REQUIRE(repeated_roundtrip_destination.partial_vulkan_stats
                == ::bioseq::simd::
                    partial_vulkan_execution_stats{});

    match_result_collection tampered_destination;
    record_minimizer_sketch_execution_stats(
        tampered_destination, token_with_metadata,
        minimizer_sketch_role::query);
    REQUIRE(tampered_destination.partial_vulkan_stats
                == ::bioseq::simd::
                    partial_vulkan_execution_stats{});

    match_result_collection query_destination;
    record_minimizer_sketch_execution_stats(
        query_destination, vulkan_sketch, minimizer_sketch_role::query);
    REQUIRE(query_destination.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    true, false, false, false, false));

    match_result_collection target_destination;
    record_minimizer_sketch_execution_stats(
        target_destination, vulkan_sketch, minimizer_sketch_role::target);
    REQUIRE(target_destination.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    false, true, false, false, false));
}

TEST_CASE("bounded ntHash sketch stats is distinct and cannot be promoted across routes",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using minimizer_algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using backend_type = ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::minimap2_parameters;
    using bounded_stats_type = ::bioseq::simd::
        bounded_nthash_execution_stats;
    using range_stats_type = ::bioseq::simd::pipe::
        minimizer_range_execution_stats;
    using stats = ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_role;
    using ::bioseq::simd::pipe_entry_detail::
        minimizer_sketch_stats_matches_selected_route;
    using ::bioseq::simd::pipe_entry_detail::
        record_minimizer_sketch_execution_stats;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;
    using ::bioseq::simd::pipe::
        target_index_sketch_backend_from_params;

    bounded_stats_type operation{};
    operation.scheme_id = ::bioseq::simd::
        k_bounded_nthash_scheme_id;
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
    REQUIRE(operation.is_internally_consistent(true));

    stats bounded{};
    bounded.bounded_nthash = operation;
    bounded.bounded_nthash_available = true;
    REQUIRE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::bounded_nthash_simd, bounded));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::scalar, bounded));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::two_stack_nthash, bounded));
    REQUIRE_FALSE(minimizer_sketch_stats_matches_selected_route(
        minimizer_algorithm_type::vulkan_minimap_integer, bounded));

    match_result_collection destination;
    record_minimizer_sketch_execution_stats(
        destination, bounded, minimizer_sketch_role::query);
    record_minimizer_sketch_execution_stats(
        destination, bounded, minimizer_sketch_role::target);
    REQUIRE(destination.is_bounded_nthash_query_execution_consistent(true));
    REQUIRE(destination.is_bounded_nthash_target_execution_consistent(true));
    REQUIRE_FALSE(destination.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(destination.partial_vulkan_stats.has_any_dispatch());

    range_stats_type bounded_range{};
    bounded_range.bounded_nthash = operation;
    bounded_range.bounded_nthash_available = true;
    REQUIRE(minimizer_range_stats_matches_backend(
        backend_type::bounded_nthash_simd,
        bounded_range,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::exact_simd, bounded_range));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::scalar, bounded_range));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::vulkan_segment, bounded_range));

    auto hidden_exact_payload = bounded_range;
    hidden_exact_payload.exact_simd.bp_consumed = 1;
    REQUIRE_FALSE(hidden_exact_payload.exact_simd_available);
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::bounded_nthash_simd, hidden_exact_payload));

    auto cleared_bounded_flag = bounded_range;
    cleared_bounded_flag.bounded_nthash_available = false;
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::bounded_nthash_simd, cleared_bounded_flag));

    range_stats_type scalar_range{};
    scalar_range.scalar.record_input(/*symbols=*/128u);
    scalar_range.scalar_available = true;
    REQUIRE(minimizer_range_stats_matches_backend(
        backend_type::scalar, scalar_range));
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::vulkan_segment, scalar_range));
    scalar_range.bounded_nthash.scheme_id = 1;
    REQUIRE_FALSE(scalar_range.bounded_nthash_available);
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::scalar, scalar_range));

    range_stats_type scalar_presence_only{};
    scalar_presence_only.scalar_available = true;
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        backend_type::scalar, scalar_presence_only));

    minimap2_parameters params{};
    params.minimizer_algorithm = minimizer_algorithm_type::bounded_nthash_simd;
    REQUIRE(target_index_sketch_backend_from_params(params)
        == backend_type::bounded_nthash_simd);
    params.minimizer_algorithm = minimizer_algorithm_type::two_stack_nthash;
    params.chaining_algorithm =
        ::bioseq::simd::chaining_algorithm::scalar_minimap2;
    params.base_alignment_algorithm =
        ::bioseq::simd::base_alignment_algorithm::ksw2;
    REQUIRE(target_index_sketch_backend_from_params(params)
        == backend_type::exact_simd);
}

TEST_CASE("dispatch_exact_value_or_default falls back to DefaultValue",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    SECTION("chaining_algorithm — out-of-range routes to scalar_minimap2") {
        using registry_type = algorithm_enum_values<chaining_algorithm_type>;
        chaining_algorithm_type captured = static_cast<chaining_algorithm_type>(0x00);
        dispatch_test_support::value_capture_callable<chaining_algorithm_type> cb{&captured};
        dispatch_exact_value_or_default<chaining_algorithm_type, chaining_algorithm_type, registry_type::default_value>(
            registry_type::value_list{}, static_cast<chaining_algorithm_type>(0xFF), cb);
        REQUIRE(captured == chaining_algorithm_type::scalar_minimap2);
    }
    SECTION("base_alignment_algorithm — out-of-range routes to block_aligner") {
        using registry_type = algorithm_enum_values<base_alignment_algorithm_type>;
        base_alignment_algorithm_type captured = static_cast<base_alignment_algorithm_type>(0x00);
        dispatch_test_support::value_capture_callable<base_alignment_algorithm_type> cb{&captured};
        dispatch_exact_value_or_default<base_alignment_algorithm_type, base_alignment_algorithm_type, registry_type::default_value>(
            registry_type::value_list{}, static_cast<base_alignment_algorithm_type>(0xFF), cb);
        REQUIRE(captured == base_alignment_algorithm_type::block_aligner);
    }
    SECTION("minimizer_algorithm — out-of-range routes to SCALAR") {
        using registry_type = algorithm_enum_values<minimizer_algorithm_type>;
        minimizer_algorithm_type captured = static_cast<minimizer_algorithm_type>(0x00);
        dispatch_test_support::value_capture_callable<minimizer_algorithm_type> cb{&captured};
        dispatch_exact_value_or_default<minimizer_algorithm_type, minimizer_algorithm_type, registry_type::default_value>(
            registry_type::value_list{}, static_cast<minimizer_algorithm_type>(0xFF), cb);
        REQUIRE(captured == minimizer_algorithm_type::scalar);
    }
}

TEST_CASE("dispatch_smallest_not_less picks the right bucket",
          "[component:bioseq][layer:unit][feature:planner]")
{
    // The bucket list is the union of `compute_breakpoints_for<K>()` across
    // every K in `kmer_size_buckets`. The extended-k-mer capability selects
    // {15, 7, 27}, whose union is L = {7, 15, 17, 27, 33, 49, 63}.
    // Baseline profiles select {15}, collapsing the union to K=15's per-K
    // breakpoints. Gate the table so both specialization domains are covered.
    if constexpr (kmer_size_buckets.size() >= 3u) {
        static constexpr std::array<std::pair<std::size_t, std::size_t>, 15u> cases{{
            { 1u,   7u}, { 7u,   7u}, { 8u,  15u}, {15u, 15u}, {16u, 17u},
            {17u,  17u}, {18u,  27u}, {27u,  27u}, {28u, 33u}, {33u, 33u},
            {34u,  49u}, {49u,  49u}, {50u,  63u}, {63u, 63u}, {64u, 63u},
        }};
        for (const auto& [want, expected] : cases) {
            std::size_t captured = 0u;
            dispatch_test_support::value_capture_callable<std::size_t> cb{&captured};
            dispatch_smallest_not_less<std::size_t, std::size_t>(
                detail::seq_len_log2_buckets_seq{}, want, cb);
            REQUIRE(captured == expected);
        }
    } else {
        // Reduced-branching mode (K = {15}). Reuse the K=15 breakpoints:
        // dispatch_smallest_not_less picks the smallest L breakpoint >= want.
        // Verify the contract holds for a few representative `want` values
        // — the exact breakpoint set is materialised by
        // `compute_breakpoints_for<15>()` so we walk the same sequence.
        constexpr auto bp = detail::compute_breakpoints_for<15u>();
        REQUIRE(bp.second > 0u);
        for (std::size_t i = 0u; i < bp.second; ++i) {
            const std::size_t want = bp.first[i];
            std::size_t captured = 0u;
            dispatch_test_support::value_capture_callable<std::size_t> cb{&captured};
            dispatch_smallest_not_less<std::size_t, std::size_t>(
                detail::seq_len_log2_buckets_seq{}, want, cb);
            REQUIRE(captured >= want);
        }
    }
}

TEST_CASE("round_up_seq_len_log2 produces canonical L",
          "[component:bioseq][layer:unit][feature:planner]")
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

TEST_CASE("kmer_size_default matches buckets[0]",
          "[component:bioseq][layer:unit][feature:planner]")
{
    static_assert(kmer_size_default == 15u);
    static_assert(kmer_size_default == kmer_size_buckets[0]);
    SUCCEED();
}

TEST_CASE("algorithm_enum_values registry round-trips through algorithm_traits",
          "[component:bioseq][layer:unit][feature:planner]")
{
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::chaining_algorithm>::value_list{}));
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::base_alignment_algorithm>::value_list{}));
    static_assert(detail::all_traits_specialised_for(
        algorithm_enum_values<
            ::bioseq::simd::minimizer_algorithm>::value_list{}));
    SUCCEED();
}

namespace dispatch_test_support {

/// Probe leaf for the BAA × CA × MA chain order test.  Three NTTPs are
/// pinned; the leaf's `call` writes the (BAA, CA, MA) triple into a
/// caller-provided slot reachable through the trailing args.
template <auto p_baa_pin, auto p_ca_pin, auto p_ma_pin>
struct bca_probe_leaf {
    [[gnu::always_inline]] static int call(
        const ::bioseq::simd::minimap2_parameters& /*params*/,
        base_alignment_algorithm_type& baa_out, chaining_algorithm_type& ca_out, minimizer_algorithm_type& ma_out)
    {
        baa_out = p_baa_pin;
        ca_out  = p_ca_pin;
        ma_out  = p_ma_pin;
        return 1;
    }
};

struct bca_probe_traits {
    template <auto p_baa_pin, auto p_ca_pin, auto p_ma_pin>
    using leaf = bca_probe_leaf<p_baa_pin, p_ca_pin, p_ma_pin>;
};

/// Probe leaf for the heterogeneous (size_t L, size_t K, BAA) chain test.
template <auto p_l_pin, auto p_k_pin, auto p_baa_pin>
struct lkb_probe_leaf {
    [[gnu::always_inline]] static int call(
        const ::bioseq::simd::minimap2_parameters& /*params*/,
        const compact_sequence_range<std::vector<seq::dna4>>& /*q*/,
        const compact_sequence_range<std::vector<seq::dna4>>& /*t*/,
        std::size_t& l_out, std::size_t& k_out, base_alignment_algorithm_type& baa_out)
    {
        l_out   = p_l_pin;
        k_out   = p_k_pin;
        baa_out = p_baa_pin;
        return 1;
    }
};

struct lkb_probe_traits {
    template <auto p_l_pin, auto p_k_pin, auto p_baa_pin>
    using leaf = lkb_probe_leaf<p_l_pin, p_k_pin, p_baa_pin>;
};

/// Void-returning probe leaf for the MA-only chain test.
template <auto p_ma_pin>
struct void_probe_leaf {
    [[gnu::always_inline]] static void call(
        const ::bioseq::simd::minimap2_parameters& /*params*/,
        minimizer_algorithm_type& ma_out)
    {
        ma_out = p_ma_pin;
    }
};

struct void_probe_traits {
    template <auto p_ma_pin>
    using leaf = void_probe_leaf<p_ma_pin>;
};

}  // namespace dispatch_test_support

TEST_CASE("dispatch_chain pins NTTPs in chain order",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;
    using dispatch_test_support::bca_probe_traits;

    ::bioseq::simd::minimap2_parameters params;

    constexpr std::array<base_alignment_algorithm_type, 3> baas = {
        base_alignment_algorithm_type::ksw2, base_alignment_algorithm_type::block_aligner, base_alignment_algorithm_type::ksw2_batched,
    };
    constexpr std::array<chaining_algorithm_type, 4> cas = {
        chaining_algorithm_type::scalar_minimap2, chaining_algorithm_type::simd_minimap2, chaining_algorithm_type::range_minimum_query, chaining_algorithm_type::vulkan_tiled_dynamic_programming,
    };
    constexpr std::array<minimizer_algorithm_type, 3> mas = {
        minimizer_algorithm_type::scalar, minimizer_algorithm_type::simd_minimap2_reference, minimizer_algorithm_type::two_stack_nthash,
    };

    for (base_alignment_algorithm_type baa_v : baas) for (chaining_algorithm_type ca_v : cas) for (minimizer_algorithm_type ma_v : mas) {
        params.base_alignment_algorithm = baa_v;
        params.chaining_algorithm       = ca_v;
        params.minimizer_algorithm      = ma_v;
        base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
        chaining_algorithm_type  ca_out  = static_cast<chaining_algorithm_type>(0xFF);
        minimizer_algorithm_type  ma_out  = static_cast<minimizer_algorithm_type>(0xFF);
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

TEST_CASE("dispatch_chain heterogeneous NTTPs (L, K, BAA)",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::lkb_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    // Build two dummy sequences of differing sizes so seq_len_log2_step
    // picks a non-trivial L.
    std::vector<seq::dna4> q_data(1u << 10);
    std::vector<seq::dna4> t_data(1u << 14);
    compact_sequence_range<std::vector<seq::dna4>> q{q_data};
    compact_sequence_range<std::vector<seq::dna4>> t{t_data};

    SECTION("K=15, BAA=KSW2 — picks the L bucket from max_bp") {
        params.kmer_size                = 15;
        params.base_alignment_algorithm = base_alignment_algorithm_type::ksw2;
        std::size_t l_out = 0u, k_out = 0u;
        base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
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
        REQUIRE(baa_out == base_alignment_algorithm_type::ksw2);
    }
    // Gated by `kmer_size_buckets.size()`: only run when the extended-k-mer
    // capability compiles the K=7 dispatch branch. Baseline profiles contain
    // only {15u}, so their smallest-ge fallback is K=15.
    if constexpr (kmer_size_buckets.size() >= 3u) {
        SECTION("K=7 — picks the K=7 bucket and L=7 from short sequences") {
            std::vector<seq::dna4> q_small(64);
            std::vector<seq::dna4> t_small(64);
            compact_sequence_range<std::vector<seq::dna4>> q_sm{q_small};
            compact_sequence_range<std::vector<seq::dna4>> t_sm{t_small};
            params.kmer_size                = 7;
            params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
            std::size_t l_out = 0u, k_out = 0u;
            base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
            detail::dispatch_chain<
                int,
                lkb_probe_traits::template leaf,
                detail::seq_len_log2_step,
                detail::kmer_size_step,
                detail::baa_step
            >::template run<>(params, q_sm, t_sm, l_out, k_out, baa_out);
            REQUIRE(k_out == 7u);
            REQUIRE(l_out == 7u);
            REQUIRE(baa_out == base_alignment_algorithm_type::block_aligner);
        }
    }
}

TEST_CASE("dispatch_chain skips inactive steps via guarded_step",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;
    using dispatch_test_support::bca_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    // Inject an out-of-range MA value to confirm the chain never reads
    // params.minimizer_algorithm — guarded_step short-circuits to
    // ma_step::default_value (SCALAR) when active=false.
    params.minimizer_algorithm      = static_cast<minimizer_algorithm_type>(0xFF);
    params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
    params.chaining_algorithm       = chaining_algorithm_type::scalar_minimap2;

    base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
    chaining_algorithm_type  ca_out  = static_cast<chaining_algorithm_type>(0xFF);
    minimizer_algorithm_type  ma_out  = static_cast<minimizer_algorithm_type>(0xFF);
    detail::dispatch_chain<
        int,
        bca_probe_traits::template leaf,
        detail::baa_step,
        detail::ca_step,
        detail::guarded_step<detail::ma_step, /*active=*/false>
    >::template run<>(params, baa_out, ca_out, ma_out);
    REQUIRE(baa_out == base_alignment_algorithm_type::block_aligner);
    REQUIRE(ca_out  == chaining_algorithm_type::scalar_minimap2);
    REQUIRE(ma_out  == minimizer_algorithm_type::scalar);
}

TEST_CASE("dispatch_chain handles void return",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::minimizer_algorithm_type;
    using dispatch_test_support::void_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    for (minimizer_algorithm_type v : {minimizer_algorithm_type::scalar, minimizer_algorithm_type::simd_minimap2_reference, minimizer_algorithm_type::two_stack_nthash}) {
        params.minimizer_algorithm = v;
        minimizer_algorithm_type captured = static_cast<minimizer_algorithm_type>(0xFF);
        detail::dispatch_chain<
            void,
            void_probe_traits::template leaf,
            detail::ma_step
        >::template run<>(params, captured);
        REQUIRE(captured == v);
    }
}

namespace dispatch_test_support {

/// Records which Cep branch was selected inside `pipeline_run_leaf::call`.
inline thread_local int g_last_recorded_w = 0;
inline thread_local int g_last_recorded_k = 0;

}  // namespace dispatch_test_support

TEST_CASE("pipeline_run_leaf::call routes Cep on (W, K) runtime match",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    SECTION("(W=10, K=15) — selects the specialised Cep") {
        // Both branches use the same fully-instantiated pipeline body —
        // we cannot directly observe which Cep was picked without a probe
        // hook inside pipeline_run_impl.  Instead, we exercise the leaf's
        // (W, K) gate behaviour and confirm the call completes without
        // an out-of-bounds access by passing zero-sized sequences and
        // empty minimizers.
        ::bioseq::simd::minimap2_parameters params;
        bind_fixed_highway(params);
        params.window_size = 10;
        params.kmer_size   = 15;
        params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
        params.chaining_algorithm       = chaining_algorithm_type::scalar_minimap2;
        params.minimizer_algorithm      = minimizer_algorithm_type::scalar;
        std::vector<seq::dna4> q_data(64);
        std::vector<seq::dna4> t_data(64);
        compact_sequence_range<std::vector<seq::dna4>> q{q_data};
        compact_sequence_range<std::vector<seq::dna4>> t{t_data};
        auto q_owner = bioseq::storage::make_resident_lazy_sequence(q);
        auto t_owner = bioseq::storage::make_resident_lazy_sequence(t);
        const auto q_view = bioseq::storage::make_lazy_sequence_view(q_owner);
        const auto t_view = bioseq::storage::make_lazy_sequence_view(t_owner);
        using minimizer_type =
            ::bioseq::simd::minimizer_representation<15u, 15u>;
        const std::vector<minimizer_type> mins{};
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(mins, t.bp_size());
        // The leaf's W==10 && K==15 branch picks SpecCep.
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 15u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                params, q_view, t_view, mins, idx,
                nullptr, ::bioseq::indexing::gpu_index_cache_identity{}, false);
        REQUIRE(result.size() == 0u);
    }
    SECTION("(W=5, K=15) — selects the fallback Cep") {
        ::bioseq::simd::minimap2_parameters params;
        bind_fixed_highway(params);
        params.window_size = 5;
        params.kmer_size   = 15;
        params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;
        params.chaining_algorithm       = chaining_algorithm_type::scalar_minimap2;
        params.minimizer_algorithm      = minimizer_algorithm_type::scalar;
        std::vector<seq::dna4> q_data(64);
        std::vector<seq::dna4> t_data(64);
        compact_sequence_range<std::vector<seq::dna4>> q{q_data};
        compact_sequence_range<std::vector<seq::dna4>> t{t_data};
        auto q_owner = bioseq::storage::make_resident_lazy_sequence(q);
        auto t_owner = bioseq::storage::make_resident_lazy_sequence(t);
        const auto q_view = bioseq::storage::make_lazy_sequence_view(q_owner);
        const auto t_view = bioseq::storage::make_lazy_sequence_view(t_owner);
        using minimizer_type =
            ::bioseq::simd::minimizer_representation<15u, 15u>;
        const std::vector<minimizer_type> mins{};
        ::bioseq::indexing::minimizer_hashmap_fragment_index idx;
        idx.build(mins, t.bp_size());
        auto result = detail::pipeline_run_leaf<
            pipeline_entry_point::with_prebuilt_minimizers,
            seq::dna4,
            minimizer_type, ::bioseq::indexing::minimizer_hashmap_fragment_index,
            15u, 15u,
            base_alignment_algorithm_type::block_aligner, chaining_algorithm_type::scalar_minimap2, minimizer_algorithm_type::scalar>::call(
                params, q_view, t_view, mins, idx,
                nullptr, ::bioseq::indexing::gpu_index_cache_identity{}, false);
        REQUIRE(result.size() == 0u);
    }
}

TEST_CASE("non-canonical kmer_size resolves to the right L bucket",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::lkb_probe_traits;

    ::bioseq::simd::minimap2_parameters params;
    params.base_alignment_algorithm = base_alignment_algorithm_type::block_aligner;

    for (int32_t k : {int32_t{11}, int32_t{21}}) {
        params.kmer_size = k;
        SECTION("max_bp = 1<<10") {
            std::vector<seq::dna4> qd(1u << 10);
            std::vector<seq::dna4> td(1u << 10);
            compact_sequence_range<std::vector<seq::dna4>> q{qd};
            compact_sequence_range<std::vector<seq::dna4>> t{td};
            std::size_t l_out = 0u, k_out = 0u;
            base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
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
            // (union over kmer_size_buckets) breakpoint list. With the
            // extended-k-mer capability that list is
            // {7, 15, 17, 27, 33, 49, 63}: k=11 → L=15;  k=21 → L=27.
            // Baseline profiles include only K=15, so the list collapses to
            // K=15's per-K breakpoints
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
            base_alignment_algorithm_type baa_out = static_cast<base_alignment_algorithm_type>(0xFF);
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

namespace dispatch_test_support {

template <class... p_args>
constexpr bool chain_accepts_v = requires {
    ::bioseq::simd::pipe::detail::dispatch_chain<
        int, bca_probe_traits::template leaf,
        ::bioseq::simd::pipe::detail::baa_step,
        ::bioseq::simd::pipe::detail::ca_step,
        ::bioseq::simd::pipe::detail::ma_step
    >::template run<>(
        std::declval<const ::bioseq::simd::minimap2_parameters&>(),
        std::declval<p_args>()...);
};

}  // namespace dispatch_test_support

TEST_CASE("dispatch_chain rejects rvalue args at compile time",
          "[component:bioseq][layer:unit][feature:planner]")
{
    using dispatch_test_support::base_alignment_algorithm_type;
    using dispatch_test_support::chaining_algorithm_type;
    using dispatch_test_support::minimizer_algorithm_type;

    // lvalue references are accepted.
    static_assert(dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type&, chaining_algorithm_type&, minimizer_algorithm_type&>);
    static_assert(dispatch_test_support::chain_accepts_v<const base_alignment_algorithm_type&, const chaining_algorithm_type&, const minimizer_algorithm_type&>);
    // By-value (deduced as non-reference type) is rejected.
    static_assert(!dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type, chaining_algorithm_type, minimizer_algorithm_type>);
    // rvalue ref is rejected.
    static_assert(!dispatch_test_support::chain_accepts_v<base_alignment_algorithm_type&&, chaining_algorithm_type&&, minimizer_algorithm_type&&>);
    SUCCEED();
}

// ============================================================================
// vulkan_minimap_integer end-to-end biological parity.
// ============================================================================
//
// The full Vulkan front-end (GPU minimizers + GPU seed-hit lookup) must produce
// the same complete ordered match_result/CIGAR stream as the CPU path. When a
// Vulkan algorithm is selected, disabling Vulkan is an error; fallback
// selection must happen before entering the pipe.
namespace vulkan_pipeline_test_support {

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

[[nodiscard]] match_result_collection run(
    std::vector<seq::dna4> qd, std::vector<seq::dna4> td,
    const ::bioseq::simd::minimap2_parameters& params)
{
    range_t q_range(qd);
    range_t t_range(td);
    auto lazy_q = bioseq::storage::make_resident_lazy_sequence(q_range);
    auto lazy_t = bioseq::storage::make_resident_lazy_sequence(t_range);
    const auto q_view = bioseq::storage::make_lazy_sequence_view(lazy_q);
    const auto t_view = bioseq::storage::make_lazy_sequence_view(lazy_t);
    auto fixed_params = params;
    bind_fixed_highway(fixed_params);
    return ::bioseq::simd::minimap_port(q_view, t_view, fixed_params);
}

[[nodiscard]] bool exact_biological_results_equal(
    const match_result_collection& left,
    const match_result_collection& right) noexcept
{
    if (left.results.size() != right.results.size()) return false;
    for (std::size_t index = 0u; index < left.results.size(); ++index) {
        const match_result& lhs = left.results[index];
        const match_result& rhs = right.results[index];
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

}  // namespace vulkan_pipeline_test_support

TEST_CASE("Prefilter stats aggregation preserves runtime identity and fails closed on overflow",
          "[component:bioseq][layer:unit][feature:prefilter]") {
    using bioseq::pipeline_stats::pipeline_execution_stats;
    using bioseq::pipeline_stats::pipeline_operational_stats;
    using bioseq::prefilter::prefilter_backend;
    using bioseq::prefilter::prefilter_execution_stats;
    using bioseq::prefilter::resolved_prefilter_algorithm;

    pipeline_execution_stats aggregate{};
    for (std::uint64_t index = 0u; index < 3u; ++index) {
        pipeline_operational_stats batch{};
        prefilter_execution_stats stats{};
        const bool rejected = index == 2u;
        stats.record_batch(
            resolved_prefilter_algorithm::gatekeeper,
            prefilter_backend::vulkan,
            1u, 1u, 0u, 0u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            rejected ? 0u : 1u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            rejected ? 1u : 0u,
            0u,
            1u);
        batch.record_prefilter_execution_stats(stats);
        aggregate.merge_operational(batch);
    }

    REQUIRE(aggregate.operational.prefilter_execution_stats_available);
    const auto& stats = aggregate.operational.prefilter_stats;
    CHECK(stats.dispatches == 3u);
    CHECK(stats.gatekeeper_dispatches == 3u);
    CHECK(stats.vulkan_dispatches == 3u);
    CHECK(stats.jobs == 3u);
    CHECK(stats.evaluated_jobs == 3u);
    CHECK(stats.length_64_jobs == 3u);
    CHECK(stats.rejected_jobs == 1u);
    CHECK(stats.rejected_regions == 1u);
    CHECK(stats.surviving_regions == 2u);
    CHECK(stats.mask_rejection_candidates == 1u);
    CHECK(stats.exact_guard_activations == 1u);
    CHECK(stats.guard_confirmed_rejections == 1u);
    CHECK(stats.hamming_exact_rejections == 0u);
    CHECK(stats.is_internally_consistent());
    CHECK(std::string_view{stats.resolved_algorithm_name()} == "gatekeeper");
    CHECK(std::string_view{stats.resolved_backend_name()} == "vulkan");
    CHECK_FALSE(aggregate.operational.are_stats_saturated);

    pipeline_operational_stats valid_then_malformed{};
    prefilter_execution_stats valid_single_batch{};
    valid_single_batch.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    valid_then_malformed.record_prefilter_execution_stats(
        valid_single_batch);
    REQUIRE(valid_then_malformed.prefilter_stats
                .is_internally_consistent());
    valid_then_malformed.record_prefilter_execution_stats(
        prefilter_execution_stats{});
    CHECK(valid_then_malformed.prefilter_stats.is_saturated);
    CHECK_FALSE(valid_then_malformed.prefilter_stats
                    .is_internally_consistent());
    CHECK(valid_then_malformed.are_stats_saturated);

    pipeline_execution_stats at_limit{};
    pipeline_operational_stats maximum_batch{};
    prefilter_execution_stats maximum{};
    maximum.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        (std::numeric_limits<std::uint64_t>::max)(),
        (std::numeric_limits<std::uint64_t>::max)(),
        0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u,
        (std::numeric_limits<std::uint64_t>::max)());
    maximum_batch.record_prefilter_execution_stats(maximum);
    at_limit.merge_operational(maximum_batch);
    pipeline_execution_stats increment{};
    pipeline_operational_stats increment_batch{};
    prefilter_execution_stats one_more{};
    one_more.record_batch(
        resolved_prefilter_algorithm::gatekeeper,
        prefilter_backend::vulkan,
        1u, 1u, 0u, 0u, 0u, 0u, 1u,
        0u, 0u, 0u, 0u, 1u);
    increment_batch.record_prefilter_execution_stats(one_more);
    increment.merge_operational(increment_batch);
    at_limit.merge(increment);
    CHECK(at_limit.operational.prefilter_stats.is_saturated);
    CHECK_FALSE(at_limit.operational.prefilter_stats
                    .is_internally_consistent());
    CHECK(at_limit.operational.are_stats_saturated);
}

TEST_CASE("Prefilter algorithm resolver admits only the measured Vulkan matrix",
          "[component:bioseq][layer:unit][feature:prefilter]") {
    using bioseq::prefilter::prefilter_backend;
    using bioseq::prefilter::resolved_prefilter_algorithm;
    using bioseq::prefilter::prefilter_job;

    const std::array<prefilter_job, 2u> measured{
        prefilter_job{.query_length = 32u, .target_length = 32u,
                     .max_edits = 2u},
        prefilter_job{.query_length = 64u, .target_length = 64u,
                     .max_edits = 4u},
    };
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::vulkan, measured)
        == resolved_prefilter_algorithm::gatekeeper);
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::simd, measured)
        == resolved_prefilter_algorithm::sneaky_snake);

    auto outside = measured;
    outside[0].query_length = 48u;
    outside[0].target_length = 48u;
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::vulkan, outside)
        == resolved_prefilter_algorithm::sneaky_snake);
    CHECK(resolve_prefilter_algorithm(
        prefilter_backend::vulkan,
        std::span<const prefilter_job>{})
        == resolved_prefilter_algorithm::sneaky_snake);
}

TEST_CASE("prefilter_stage composes reverse, unequal, aggregate, and stable routes",
          "[component:bioseq][layer:unit][feature:prefilter]") {
    using bioseq::prefilter::prefilter_backend;
    using cep_type = static_aligner_parameters<10u, 15u, 28u>;
    using ::bioseq::chaining::anchor;
    using vulkan_pipeline_test_support::dna4_u64;
    using vulkan_pipeline_test_support::range_t;

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
        return anchor{
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
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_target);

    const std::vector<anchor> anchors{
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

    for (const prefilter_backend backend : {
             prefilter_backend::scalar,
             prefilter_backend::simd}) {
        CAPTURE(static_cast<unsigned>(backend));
        minimap2_parameters params;
        bind_fixed_highway(params);
        params.prefilter_enabled = true;
        params.compute_edit_distance = true;
        params.prefilter_backend = backend;
        params.prefilter_max_edits = 1u;
        params.min_ksw_len = 1;

        std::vector<alignment_region> regions(4u);
        regions[0].anchor_start = 0;
        regions[0].seed_count = 2;
        regions[1].anchor_start = 2;
        regions[1].seed_count = 3;
        regions[2].anchor_start = 5;
        regions[2].seed_count = 2;
        regions[3].anchor_start = 7;
        regions[3].seed_count = 2;
        regions[3].is_reverse = true;

        match_result_collection stats;
        std::vector<alignment_region> observed;
        int on_batch_count = 0;
        prefilter_probe_sink probe{&observed, &on_batch_count};
        prefilter_stage<seq::dna4> stage{
            {}, params, query_view, target_view, &stats};
        auto bound = stage.template attach<cep_type>(probe);
        bound.on_batch_pair(
            std::span<alignment_region>{regions},
            std::span<const anchor>{anchors});

        REQUIRE(on_batch_count == 1);
        REQUIRE(observed.size() == 2u);
        CHECK(observed[0].anchor_start == 0);
        CHECK(observed[1].anchor_start == 7);
        REQUIRE(stats.prefilter_execution_stats_available);
        const auto& route = stats.prefilter_stats;
        CHECK(route.dispatches == 1u);
        CHECK(route.sneaky_snake_dispatches == 1u);
        CHECK(route.gatekeeper_dispatches == 0u);
        CHECK(route.scalar_dispatches
              == (backend == prefilter_backend::scalar ? 1u : 0u));
        CHECK(route.simd_dispatches
              == (backend == prefilter_backend::simd ? 1u : 0u));
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
        CHECK_FALSE(route.is_saturated);
    }
}

#ifdef BIOSEQ_TEST_INDEXES
template <class p_collector>
[[nodiscard]] std::vector<std::uint64_t>
normalized_raw_minimizers(const p_collector& collector) {
    std::vector<std::uint64_t> records;
    const auto& sketches = collector.extract_sketches_data();
    records.reserve(sketches.size());
    for (const auto& sketch : sketches) {
        records.push_back(static_cast<std::uint64_t>(sketch.raw_value()));
    }
    std::ranges::sort(records);
    const auto unique_end = std::unique(records.begin(), records.end());
    records.resize(static_cast<std::size_t>(unique_end - records.begin()));
    return records;
}

template <sequence_alphabet p_alphabet, class p_storage_unit>
void check_reference_minimizer_topology_case() {
    using namespace ::bioseq::simd;
    using sequence_type = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = basic_sketch_collector<minimap_sketch_data>;
    using pipe::minimizer_kernel_kind;

    std::vector<p_alphabet> data(4099u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        std::uint8_t rank = static_cast<std::uint8_t>(index & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 97u == 41u
                ? 3u
                : std::array<std::uint8_t, 4>{0u, 1u, 2u, 4u}[index & 3u];
        }
        data[index].assign_rank(rank);
    }
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    constexpr bool is_dna5 = std::same_as<p_alphabet, seq::dna5>;
    const auto* table = is_dna5 ? nullptr : fixed_highway_kernels();

    collector_type selected_collector;
    const auto runtime_stats = pipe::minimizer_dispatch<
        minimizer_kernel_kind::simd, minimap_sketch_data>(
            selected_collector, view, 10u, 15u, table);
    collector_type specialized_collector;
    const auto constexpr_stats = pipe::minimizer_dispatch_constexpr<
        minimizer_kernel_kind::simd, 10u, 15u, minimap_sketch_data>(
            specialized_collector, view, 10u, 15u, table);
    collector_type scalar_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::scalar, minimap_sketch_data>(
            scalar_collector, view, 10u, 15u, table);

    const auto selected = normalized_raw_minimizers(selected_collector);
    const auto specialized =
        normalized_raw_minimizers(specialized_collector);
    const auto scalar = normalized_raw_minimizers(scalar_collector);
    CAPTURE(sizeof(p_storage_unit), is_dna5);
    REQUIRE(runtime_stats == two_stack_execution_stats{});
    REQUIRE(constexpr_stats == two_stack_execution_stats{});
    REQUIRE_FALSE(selected.empty());
    REQUIRE(selected == scalar);
    REQUIRE(specialized == scalar);
    if constexpr (is_dna5) {
        const auto& runtime_records =
            selected_collector.extract_sketches_data();
        const auto& constexpr_records =
            specialized_collector.extract_sketches_data();
        const auto& scalar_records =
            scalar_collector.extract_sketches_data();
        REQUIRE(runtime_records.size() == scalar_records.size());
        REQUIRE(constexpr_records.size() == scalar_records.size());
        for (std::size_t index = 0u; index < scalar_records.size(); ++index) {
            CAPTURE(index);
            REQUIRE(runtime_records[index].raw_value()
                == scalar_records[index].raw_value());
            REQUIRE(constexpr_records[index].raw_value()
                == scalar_records[index].raw_value());
        }
    }
}

template <sequence_alphabet p_alphabet, class p_storage_unit>
void check_two_stack_minimizer_topology_case() {
    using namespace ::bioseq::simd;
    using sequence_type = sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = basic_sketch_collector<minimap_sketch_data>;
    using pipe::minimizer_kernel_kind;

    std::vector<p_alphabet> data(4099u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        std::uint8_t rank = static_cast<std::uint8_t>(index & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 101u == 53u
                ? 3u
                : std::array<std::uint8_t, 4>{0u, 1u, 2u, 4u}[index & 3u];
        }
        data[index].assign_rank(rank);
    }
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);

    collector_type selected_collector;
    const auto stats = pipe::minimizer_dispatch<
        minimizer_kernel_kind::exact_simd, minimap_sketch_data>(
            selected_collector,
            view,
            11u,
            15u,
            fixed_highway_kernels());
    REQUIRE(stats.is_internally_consistent(false));

    const auto& selected = selected_collector.extract_sketches_data();
    constexpr bool is_dna5 = std::same_as<p_alphabet, seq::dna5>;
    CAPTURE(sizeof(p_storage_unit), is_dna5);
    REQUIRE(stats.minimizers_emitted == selected.size());
    if constexpr (is_dna5) {
        const auto selected_pairs = selected_collector.extract_pairs();
        const auto baseline_pairs =
            minimap2_sketch_baseline_pairs(resident, 11u, 15u);
        REQUIRE_FALSE(selected_pairs.empty());
        REQUIRE(selected_pairs == baseline_pairs);
    } else {
        collector_type scalar_collector;
        (void)pipe::minimizer_dispatch<
            minimizer_kernel_kind::scalar, minimap_sketch_data>(
                scalar_collector,
                view,
                11u,
                15u,
                fixed_highway_kernels());
        const auto& scalar = scalar_collector.extract_sketches_data();
        REQUIRE(selected.size() == scalar.size());
        for (std::size_t index = 0u; index < selected.size(); ++index) {
            CAPTURE(index);
            REQUIRE(selected[index].raw_value() == scalar[index].raw_value());
        }
    }
}

TEST_CASE("reference SIMD pipe runtime and constexpr dispatch use fixed target table",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using namespace ::bioseq::simd;
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using collector_type = basic_sketch_collector<minimap_sketch_data>;
    using pipe::minimizer_kernel_kind;

    auto data = vulkan_pipeline_test_support::make_dna4(4096u, 0x51AD51ADu);
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto* table = fixed_highway_kernels();
    REQUIRE(table->has_kernel(
        ::bioseq::simd_dispatch::highway_kernel::
            minimizer_minimap2_reference));

    collector_type runtime_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::simd, minimap_sketch_data>(
            runtime_collector, view, 10u, 15u, table);

    collector_type constexpr_collector;
    (void)pipe::minimizer_dispatch_constexpr<
        minimizer_kernel_kind::simd, 10u, 15u, minimap_sketch_data>(
            constexpr_collector, view, 10u, 15u, table);

    const auto& runtime = runtime_collector.extract_sketches_data();
    const auto& specialized = constexpr_collector.extract_sketches_data();
    REQUIRE(runtime.size() == specialized.size());
    for (std::size_t index = 0u; index < runtime.size(); ++index) {
        CAPTURE(index);
        REQUIRE(runtime[index].raw_value() == specialized[index].raw_value());
    }

    collector_type scalar_collector;
    (void)pipe::minimizer_dispatch<
        minimizer_kernel_kind::scalar, minimap_sketch_data>(
            scalar_collector, view, 10u, 15u, table);
    const auto normalized_runtime =
        normalized_raw_minimizers(runtime_collector);
    const auto normalized_scalar =
        normalized_raw_minimizers(scalar_collector);
    REQUIRE_FALSE(normalized_runtime.empty());
    REQUIRE(normalized_runtime == normalized_scalar);
}

TEST_CASE("neutral minimizer sink projects active K15 layouts without truncation",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using namespace ::bioseq::simd;
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using output_type = minimizer_representation<33u, 15u>;
    using collector_type = basic_sketch_collector<output_type>;
    using pipe::minimizer_kernel_kind;

    auto data = vulkan_pipeline_test_support::make_dna4(32'769u, 0x15AB1C0Du);
    range_type resident(data);
    auto lazy = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto* table = fixed_highway_kernels();

    const auto require_raw_parity = [](const collector_type& selected,
                                       const collector_type& scalar) {
        const auto& selected_records = selected.extract_sketches_data();
        const auto& scalar_records = scalar.extract_sketches_data();
        REQUIRE_FALSE(selected_records.empty());
        REQUIRE(selected_records.size() == scalar_records.size());
        for (std::size_t index = 0u; index < selected_records.size(); ++index) {
            CAPTURE(index);
            REQUIRE(selected_records[index].raw_value()
                    == scalar_records[index].raw_value());
        }
    };

    SECTION("reference SIMD preserves the normalized K15 record set") {
        collector_type reference_collector;
        (void)pipe::minimizer_dispatch<minimizer_kernel_kind::simd, output_type>(
            reference_collector, view, 10u, 15u, table);
        collector_type reference_scalar_collector;
        (void)pipe::minimizer_dispatch<minimizer_kernel_kind::scalar, output_type>(
            reference_scalar_collector, view, 10u, 15u, table);
        const auto reference =
            normalized_raw_minimizers(reference_collector);
        const auto scalar =
            normalized_raw_minimizers(reference_scalar_collector);
        REQUIRE_FALSE(reference.empty());
        REQUIRE(reference == scalar);
    }

    SECTION("exact SIMD preserves raw K15 record identity") {
        collector_type two_stack_collector;
        const auto two_stack_stats =
            pipe::minimizer_dispatch<minimizer_kernel_kind::exact_simd, output_type>(
                two_stack_collector, view, 10u, 15u, table);
        REQUIRE(two_stack_stats.is_internally_consistent(true));
        collector_type two_stack_scalar_collector;
        (void)pipe::minimizer_dispatch<minimizer_kernel_kind::scalar, output_type>(
            two_stack_scalar_collector, view, 10u, 15u, table);
        require_raw_parity(
            two_stack_collector, two_stack_scalar_collector);
    }
}

TEST_CASE("neutral minimizer batches retain the caller's exact wide representation",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using namespace ::bioseq::simd;
    using output_type = minimizer_representation<63u, 15u>;
    using collector_type = basic_sketch_collector<output_type>;

    STATIC_REQUIRE(sizeof(output_type) == 2u * sizeof(std::uint64_t));
    STATIC_REQUIRE(output_type::k_hash_field_mask
                   > minimap_sketch_data::k_hash_field_mask);
    STATIC_REQUIRE(output_type::k_position_field_mask
                   > minimap_sketch_data::k_position_field_mask);

    constexpr std::array<output_type, 3u> expected{
        output_type{
            (std::uint64_t{1u} << 40u) | 0x12345u,
            (std::uint64_t{1u} << 28u) | 17u,
            1u},
        output_type{
            (std::uint64_t{1u} << 41u) | 0x23456u,
            (std::uint64_t{1u} << 28u) | 29u,
            0u},
        output_type{
            (std::uint64_t{1u} << 42u) | 0x34567u,
            (std::uint64_t{1u} << 28u) | 43u,
            1u},
    };
    STATIC_REQUIRE(expected[0].kmer()
                   > minimap_sketch_data::k_hash_field_mask);
    STATIC_REQUIRE(expected[0].last_pos()
                   > minimap_sketch_data::k_position_field_mask);

    collector_type collector;
    pipe::highway_minimizer_sink_state<output_type, collector_type> adapter{
        collector};
    auto sink = adapter.sink();
    REQUIRE(sink.acquire != nullptr);
    REQUIRE(sink.commit != nullptr);

    auto first_batch = sink.acquire(sink.state, 2u);
    REQUIRE(first_batch.data != nullptr);
    REQUIRE(first_batch.capacity >= 2u);
    REQUIRE(first_batch.layout
            == ::bioseq::minimizers::k_minimizer_output_layout<output_type>);
    REQUIRE(first_batch.record_size == sizeof(output_type));
    REQUIRE(first_batch.record_alignment == alignof(output_type));
    auto* first_records = static_cast<output_type*>(first_batch.data);
    std::construct_at(first_records, expected[0]);
    std::construct_at(first_records + 1u, expected[1]);
    sink.commit(sink.state, 2u);

    auto second_batch = sink.acquire(sink.state, 1u);
    REQUIRE(second_batch.data != nullptr);
    REQUIRE(second_batch.capacity >= 1u);
    auto* second_records = static_cast<output_type*>(second_batch.data);
    std::construct_at(second_records, expected[2]);
    sink.commit(sink.state, 1u);

    ::bioseq::minimizers::minimizer_kernel_result result{};
    result.emitted = expected.size();
    REQUIRE_NOTHROW(adapter.validate(result));

    const auto& records = collector.extract_sketches_data();
    REQUIRE(records.size() == expected.size());
    for (std::size_t index = 0u; index < records.size(); ++index) {
        CAPTURE(index);
        REQUIRE(records[index].kmer() == expected[index].kmer());
        REQUIRE(records[index].last_pos() == expected[index].last_pos());
        REQUIRE(records[index].strand() == expected[index].strand());
        REQUIRE(records[index].span() == 15u);
    }
}

TEST_CASE("production minimizer sink keeps protocol faults distinct from collector admission",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using output_type = ::bioseq::simd::minimap_sketch_data;

    struct controlled_collector final {
        bool accept_batches = true;
        bool healthy = true;
        bool throw_from_batch = false;

        [[nodiscard]] bool append_records(
                std::span<const output_type>) {
            if (throw_from_batch) {
                throw std::runtime_error{"production sink callback failure"};
            }
            if (!accept_batches) {
                healthy = false;
                return false;
            }
            return true;
        }

        void collect(const output_type&) noexcept {}
        void collect(output_type&&) noexcept {}

        [[nodiscard]] bool is_ok() const noexcept { return healthy; }
    };

    constexpr output_type record{0x12345u, 17u, 1u};

    SECTION("sticky collector failure remains available to the outer scheduler") {
        controlled_collector collector;
        collector.accept_batches = false;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        auto sink = adapter.sink();
        auto batch = sink.acquire(sink.state, 1u);
        REQUIRE(batch.data != nullptr);
        std::construct_at(static_cast<output_type*>(batch.data), record);
        sink.commit(sink.state, 1u);

        ::bioseq::minimizers::minimizer_kernel_result result{};
        result.emitted = 1u;
        REQUIRE_FALSE(collector.is_ok());
        REQUIRE_NOTHROW(adapter.validate(result));
    }

    SECTION("healthy count mismatch remains a transport error") {
        controlled_collector collector;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        ::bioseq::minimizers::minimizer_kernel_result result{};
        result.emitted = 1u;
        REQUIRE_THROWS_WITH(
            adapter.validate(result),
            "selected Highway minimizer sink rejected a committed batch");
    }

    SECTION("acquire without commit fails closed even at a zero count") {
        controlled_collector collector;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        auto sink = adapter.sink();
        REQUIRE(sink.acquire(sink.state, 1u).data != nullptr);
        ::bioseq::minimizers::minimizer_kernel_result result{};
        REQUIRE_THROWS_WITH(
            adapter.validate(result),
            "selected Highway minimizer sink left a batch uncommitted");
    }

    SECTION("callback exception is rethrown after the noexcept commit boundary") {
        controlled_collector collector;
        collector.throw_from_batch = true;
        ::bioseq::simd::pipe::highway_minimizer_sink_state<
            output_type, controlled_collector> adapter{collector};
        auto sink = adapter.sink();
        auto batch = sink.acquire(sink.state, 1u);
        REQUIRE(batch.data != nullptr);
        std::construct_at(static_cast<output_type*>(batch.data), record);
        sink.commit(sink.state, 1u);

        ::bioseq::minimizers::minimizer_kernel_result result{};
        result.emitted = 1u;
        REQUIRE_THROWS_WITH(
            adapter.validate(result), "production sink callback failure");
    }
}

TEST_CASE("reference SIMD preserves DNA4 topology and scalar-routes DNA5",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    check_reference_minimizer_topology_case<seq::dna4, std::uint32_t>();
    check_reference_minimizer_topology_case<seq::dna4, std::uint64_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint8_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint16_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint32_t>();
    check_reference_minimizer_topology_case<seq::dna5, std::uint64_t>();
}

TEST_CASE("source-owned two-stack topology closes DNA4/DNA5 by four widths",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint8_t>();
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint16_t>();
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint32_t>();
    check_two_stack_minimizer_topology_case<seq::dna4, std::uint64_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint8_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint16_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint32_t>();
    check_two_stack_minimizer_topology_case<seq::dna5, std::uint64_t>();
}
#endif

TEST_CASE("exact SIMD pipe admits whole non-u32 ranges into resident u32 vector_stream",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using range_type = compact_sequence_range<sequence_type>;
    using non_u32_sequence_type =
        sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using non_u32_range_type = compact_sequence_range<non_u32_sequence_type>;
    using minimizer_type =
        ::bioseq::simd::minimizer_representation<33u, 15u>;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::exact_simd_range_route;
    using ::bioseq::simd::pipe::minimizer_kernel_kind;
    using ::bioseq::simd::pipe::minimizer_dispatch_constexpr;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;
    using ::bioseq::simd::pipe::
        exact_simd_stats_share_selection_route;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;

    auto data = vulkan_pipeline_test_support::make_dna4(4096u, 0x51A1D5EEDu);
    range_type resident(data);
    auto lazy_resident = bioseq::storage::make_resident_lazy_sequence(resident);
    const auto resident_view =
        bioseq::storage::make_lazy_sequence_view(lazy_resident);
    const auto* table = fixed_highway_kernels();

    collector_type whole_collector;
    ::bioseq::simd::two_stack_execution_stats whole_stats;
    try {
        whole_stats = minimizer_dispatch_constexpr<
            minimizer_kernel_kind::exact_simd, 10u, 15u, minimizer_type>(
                whole_collector, resident_view, 10u, 15u, table);
    } catch (const ::bioseq::simd::exact_simd_runtime_error& error) {
        REQUIRE(error.status() == ::bioseq::simd::
            two_stack_continuation_status::unsupported_vector_shape);
        REQUIRE(whole_collector.extract_sketches_data().empty());
        SKIP("generated fixed target lacks the required vector shape");
    }
    REQUIRE(whole_stats.is_internally_consistent(true));
    REQUIRE(whole_stats.resident_vector_stream_kernel_calls == 1u);
    REQUIRE(whole_stats.resident_vector_stream_dual_calls == 1u);
    REQUIRE(whole_stats.resident_vector_stream_single_calls == 0u);
    REQUIRE(whole_stats.generic_u64_kernel_calls == 0u);
    REQUIRE(whole_stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0u);

    // Range dispatch is also used by indexed query preparation. Resident
    // vector_stream is therefore an explicit paired/no-index opt-in, never a
    // decision inferred from the query's storage shape alone.
    collector_type paired_resident_collector;
    const auto paired_resident_stats =
        minimizer_dispatch_range<minimizer_type>(
            paired_resident_collector,
            resident_view,
            0u,
            resident.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_resident_vector_stream,
            table);
    REQUIRE(paired_resident_stats.exact_simd_available);
    REQUIRE(paired_resident_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(paired_resident_stats.exact_simd
        .resident_vector_stream_kernel_calls == 1u);
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        paired_resident_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_resident_vector_stream));

    // Resident vector_stream is an exact-u32 topology. A different resident
    // width is therefore admitted into an owned u32 region before the same
    // The fixed-target route executes; source width is never relabelled in place.
    non_u32_range_type non_u32_resident(data);
    auto lazy_non_u32 = bioseq::storage::make_resident_lazy_sequence(non_u32_resident);
    const auto non_u32_view =
        bioseq::storage::make_lazy_sequence_view(lazy_non_u32);
    const auto non_u32_access = bioseq::storage::plan_sequence_access(
        non_u32_view,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        non_u32_view.bp_size(),
        sizeof(std::uint32_t));
    REQUIRE(non_u32_access.route
        == bioseq::storage::sequence_access_route::materialize_admitted);
    REQUIRE_FALSE(non_u32_access.resident.has_value());
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t admission_before = admission.reserved_bytes();
    collector_type admitted_non_u32_collector;
    const auto admitted_non_u32_stats =
        minimizer_dispatch_range<minimizer_type>(
            admitted_non_u32_collector,
            non_u32_view,
            0u,
            non_u32_view.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_resident_vector_stream,
            table);
    REQUIRE(admission.reserved_bytes() == admission_before);
    REQUIRE(admitted_non_u32_stats.exact_simd_available);
    REQUIRE(admitted_non_u32_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(admitted_non_u32_stats.exact_simd
        .resident_vector_stream_kernel_calls == 1u);
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        admitted_non_u32_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_resident_vector_stream));
    const auto& admitted_records =
        admitted_non_u32_collector.extract_sketches_data();
    const auto& resident_records =
        paired_resident_collector.extract_sketches_data();
    REQUIRE(admitted_records.size() == resident_records.size());
    for (std::size_t index = 0u; index < resident_records.size(); ++index) {
        CAPTURE(index);
        REQUIRE(admitted_records[index].raw_value()
            == resident_records[index].raw_value());
    }

    // This is the fail-closed resident-stats boundary: resident stats cannot
    // be relabelled as the continuation/B8 scheme used by persisted indexes.
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        paired_resident_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_continuation));

    collector_type persisted_compatible_collector;
    const auto persisted_compatible_stats =
        minimizer_dispatch_range<minimizer_type>(
            persisted_compatible_collector,
            resident_view,
            0u,
            resident.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_continuation,
            table);
    REQUIRE(persisted_compatible_stats.exact_simd_available);
    REQUIRE(persisted_compatible_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(persisted_compatible_stats.exact_simd
        .resident_vector_stream_kernel_calls == 0u);
    REQUIRE((persisted_compatible_stats.exact_simd
            .generic_u64_kernel_calls > 0u
        || persisted_compatible_stats.exact_simd
            .kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0u));
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        persisted_compatible_stats,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_continuation));
    REQUIRE_FALSE(exact_simd_stats_share_selection_route(
        paired_resident_stats.exact_simd,
        persisted_compatible_stats.exact_simd));
    REQUIRE(exact_simd_stats_share_selection_route(
        paired_resident_stats.exact_simd,
        whole_stats));

    collector_type rejected_resident_subrange_collector;
    REQUIRE_THROWS_AS(
        minimizer_dispatch_range<minimizer_type>(
            rejected_resident_subrange_collector,
            resident_view,
            0u,
            resident.bp_size() - 1u,
            10u,
            15u,
            minimizer_sketch_backend::exact_simd,
            exact_simd_range_route::require_resident_vector_stream,
            table),
        std::invalid_argument);
    REQUIRE(rejected_resident_subrange_collector
        .extract_sketches_data().empty());

    collector_type subrange_collector;
    const auto subrange_stats = minimizer_dispatch_range<minimizer_type>(
        subrange_collector,
        resident_view,
        0u,
        resident.bp_size() - 1u,
        10u,
        15u,
        minimizer_sketch_backend::exact_simd,
        exact_simd_range_route::require_continuation,
        table);
    REQUIRE(subrange_stats.exact_simd_available);
    REQUIRE(subrange_stats.exact_simd.is_internally_consistent(true));
    REQUIRE(subrange_stats.exact_simd
        .resident_vector_stream_kernel_calls == 0u);
    REQUIRE((subrange_stats.exact_simd.generic_u64_kernel_calls > 0u
        || subrange_stats.exact_simd
            .kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0u));
}

TEST_CASE("Indexed dna4 u64 queries use admitted typed u32 B8 continuation with scalar-identical records",
          "[component:bioseq][layer:unit][feature:seeding][resource:highway]") {
    using namespace ::bioseq::simd;
    using query_sequence = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
    using query_range = compact_sequence_range<query_sequence>;

    minimap2_parameters params;
    bind_fixed_highway(params);
    params.minimizer_algorithm = minimizer_algorithm::two_stack_nthash;

    const auto assert_case = [&params](std::vector<seq::dna4> data,
                                      std::string_view fixture) {
        CAPTURE(fixture, data.size());
        query_range resident(data);
        auto lazy_query = bioseq::storage::make_resident_lazy_sequence(resident);
        const auto query_view =
            bioseq::storage::make_lazy_sequence_view(lazy_query);
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
            == bioseq::storage::packed_sequence_admission_bytes<seq::dna4>(
                data.size(), sizeof(std::uint32_t)));
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        const std::uint64_t admission_baseline = admission.reserved_bytes();
        {
            auto permit = admission.try_reserve(access.admission_bytes);
            REQUIRE(permit.has_value());
            auto typed_owner = bioseq::storage::materialize_packed_sequence(
                query_view,
                0u,
                query_view.bp_size(),
                sizeof(std::uint32_t),
                std::move(*permit));
            REQUIRE(admission.reserved_bytes()
                == admission_baseline + access.admission_bytes);
            REQUIRE(typed_owner.descriptor().storage_unit_width
                == sizeof(std::uint32_t));
            REQUIRE(typed_owner.descriptor().unit_count
                == (data.size() + 15u) / 16u);
            REQUIRE(typed_owner.descriptor().units != native->units);
        }
        REQUIRE(admission.reserved_bytes() == admission_baseline);
        auto fast = sketch_query_for_index_selection<minimap_sketch_data>(
            query_view, params, 10u, 15u);
        REQUIRE(admission.reserved_bytes() == admission_baseline);

        basic_sketch_collector<minimap_sketch_data> scalar_collector;
        collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
            scalar_collector,
            resident,
            resident.begin(),
            resident.end(),
            10u,
            15u);
        const auto scalar =
            std::move(scalar_collector).extract_sketches_data();

        REQUIRE(fast.exact_simd_execution_stats_available);
        const two_stack_execution_stats& stats =
            fast.exact_simd_execution_stats;
        REQUIRE(stats.is_internally_consistent(/*require_nonempty_vector_work=*/true));
        REQUIRE(stats.bp_consumed == data.size());
        REQUIRE(stats.resident_vector_stream_kernel_calls == 0u);
        REQUIRE(stats.resident_vector_stream_single_calls == 0u);
        REQUIRE(stats.resident_vector_stream_dual_calls == 0u);
        REQUIRE(stats.generic_u64_kernel_calls == 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 1u);
        REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_candidate_blocks > 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_uint32_hash_lane_visits > 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_packed_22_groups > 0u);
        REQUIRE(stats.kmer_15_window_10_byte_8_packed_22_lane_visits > 0u);
        REQUIRE(fast.minimizers.size() == scalar.size());
        for (std::size_t i = 0; i < scalar.size(); ++i) {
            CAPTURE(i);
            REQUIRE(fast.minimizers[i].raw_value()
                == scalar[i].raw_value());
        }
    };

    assert_case(
        vulkan_pipeline_test_support::make_dna4(4'123u, 0xB8D0A57u),
        "non-periodic tail");
    std::vector<seq::dna4> repeated(4'123u);
    seq::dna4 adenine;
    adenine.assign_rank(
        static_cast<decltype(adenine.to_rank())>(0u));
    std::fill(repeated.begin(), repeated.end(), adenine);
    assert_case(std::move(repeated), "equal-hash tie windows");
}

TEST_CASE("KSW2 batched pipeline returns operation-originated nonempty stats",
          "[component:bioseq][layer:unit][feature:alignment][resource:highway]") {
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;

    auto target = vulkan_pipeline_test_support::make_dna4(4'000u, 0xB47C4EDu);
    constexpr std::size_t truth_start = 700u;
    std::vector<seq::dna4> query(
        target.begin() + truth_start,
        target.begin() + truth_start + 900u);

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.minimizer_algorithm =
        minimizer_algorithm::two_stack_nthash;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.base_alignment_algorithm =
        base_alignment_algorithm::ksw2_batched;
    params.do_cigar = true;

    const auto result = vulkan_pipeline_test_support::run(query, target, params);
    REQUIRE(result.is_exact_simd_query_execution_consistent(true));
    REQUIRE(result.is_exact_simd_target_execution_consistent(true));
    REQUIRE(result.is_simd_chaining_execution_consistent(
        params.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(result.is_ksw2_batched_execution_consistent(true));
    REQUIRE_FALSE(result.block_aligner_execution_stats_available);
    REQUIRE(result.scalar_pipeline_execution_stats_available);
    REQUIRE(result.scalar_pipeline_stats.seed_lookup_calls > 0u);
    REQUIRE(result.scalar_pipeline_stats.seed_query_minimizers > 0u);
    REQUIRE(result.scalar_pipeline_stats.query_minimizers
            == ::bioseq::simd::scalar_minimizer_execution_stats{});
    REQUIRE(result.scalar_pipeline_stats.target_minimizers
            == ::bioseq::simd::scalar_minimizer_execution_stats{});
    REQUIRE(result.query_sketch_host_wall_ns > 0u);
    REQUIRE(result.seed_lookup_host_wall_ns > 0u);
    REQUIRE(result.chain_score_host_wall_ns > 0u);
    REQUIRE(result.region_generation_host_wall_ns > 0u);
    REQUIRE(result.alignment_result_host_wall_ns > 0u);
    REQUIRE_FALSE(result.is_region_host_stage_active);
    REQUIRE_FALSE(result.is_alignment_result_host_stage_active);
    REQUIRE_FALSE(result.is_host_stage_timing_saturated);
    REQUIRE(result.scalar_pipeline_stats.chain_score_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.chain_backtrack_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.ksw2_left_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.ksw2_gap_calls == 0u);
    REQUIRE(result.scalar_pipeline_stats.ksw2_right_calls == 0u);

    auto missing = result;
    missing.ksw2_batched_execution_stats_available = false;
    REQUIRE_FALSE(missing.is_ksw2_batched_execution_consistent(true));

    auto tampered = result;
    ++tampered.ksw2_batched_stats.long_fill_calls;
    REQUIRE_FALSE(tampered.is_ksw2_batched_execution_consistent(true));
}

template <sequence_alphabet p_alphabet, class p_storage_unit>
void check_fragmented_bounded_nthash_range_materialization() {
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::bounded_nthash_runtime_error;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;
    using packed_sequence =
        sequence_custom_alphabet<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<packed_sequence>;
    using chunked_scanner = bioseq::storage::chunked_span_sequence_scanner<
        p_storage_unit, 2>;
    using minimizer_type =
        ::bioseq::simd::minimap_sketch_data;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;

    std::vector<p_alphabet> data(4'123u);
    std::uint64_t state = 0xF12A6E57u + sizeof(p_storage_unit);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::uint8_t rank = static_cast<std::uint8_t>((state >> 33u) & 3u);
        if constexpr (std::same_as<p_alphabet, seq::dna5>) {
            rank = index % 113u == 61u
                ? 3u
                : std::array<std::uint8_t, 4>{0u, 1u, 2u, 4u}[rank];
        }
        data[index].assign_rank(rank);
    }
    range_type range(data);
    auto resident_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto resident_view =
        bioseq::storage::make_lazy_sequence_view(resident_owner);
    REQUIRE(range.size() > 12u);
    REQUIRE(range.padding() != 0u);
    using sequence_size_type = bioseq::storage::sequence_size_t;
    const auto check_slice = [&](sequence_size_type lo_unit,
                                 sequence_size_type hi_unit) {
        CAPTURE(lo_unit, hi_unit, sizeof(p_storage_unit));
        constexpr sequence_size_type letters_per_unit =
            range_type::traits::letters_per_unit;
        const sequence_size_type lo_bp = lo_unit * letters_per_unit;
        const sequence_size_type hi_bp = std::min(
            hi_unit * letters_per_unit,
            static_cast<sequence_size_type>(range.bp_size()));
        collector_type resident_collector;
        const auto resident_stats = minimizer_dispatch_range<minimizer_type>(
            resident_collector,
            resident_view,
            lo_bp,
            hi_bp,
            10u,
            15u,
            minimizer_sketch_backend::bounded_nthash_simd,
            exact_simd_range_route::require_continuation,
            fixed_highway_kernels());
        REQUIRE(minimizer_range_stats_matches_backend(
            minimizer_sketch_backend::bounded_nthash_simd,
            resident_stats,
            /*require_nonempty_vector_work=*/true));

        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        const std::uint64_t reserved_before = admission.reserved_bytes();
        collector_type fragmented_collector;
        {
            bioseq::storage::lazy_sequence<
                packed_sequence, chunked_scanner> fragmented{
                    chunked_scanner::from_range(range)};
            const auto fragmented_view =
                bioseq::storage::make_lazy_sequence_view(fragmented);
            const auto fragmented_stats =
                minimizer_dispatch_range<minimizer_type>(
                    fragmented_collector,
                    fragmented_view,
                    lo_bp,
                    hi_bp,
                    10u,
                    15u,
                    minimizer_sketch_backend::bounded_nthash_simd,
                    exact_simd_range_route::require_continuation,
                    fixed_highway_kernels());
            REQUIRE(minimizer_range_stats_matches_backend(
                minimizer_sketch_backend::bounded_nthash_simd,
                fragmented_stats,
                /*require_nonempty_vector_work=*/true));
            /* Bounded consumes canonical, genuinely typed u32 storage. A u64
             * source is admission-materialized rather than reinterpreted, so
             * the execution stats records the kernel input width—not the
             * facade's original resident width. */
            REQUIRE(fragmented_stats.bounded_nthash.source_storage_unit_bytes
                    == sizeof(std::uint32_t));
            REQUIRE(fragmented_stats.bounded_nthash
                    == resident_stats.bounded_nthash);

            collector_type rejected_collector;
            REQUIRE_THROWS_AS(
                minimizer_dispatch_range<minimizer_type>(
                    rejected_collector,
                    fragmented_view,
                    lo_bp,
                    hi_bp,
                    9u,
                    15u,
                    minimizer_sketch_backend::bounded_nthash_simd,
                    exact_simd_range_route::require_continuation,
                    fixed_highway_kernels()),
                bounded_nthash_runtime_error);
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

template <class p_storage_unit>
void check_bounded_nthash_rejects_dna5_coordinate() {
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::bounded_nthash_runtime_error;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;
    using packed_sequence =
        sequence_custom_alphabet<seq::dna5, p_storage_unit>;
    using range_type = compact_sequence_range<packed_sequence>;
    using chunked_scanner = bioseq::storage::chunked_span_sequence_scanner<
        p_storage_unit, 2>;
    using minimizer_type = ::bioseq::simd::minimap_sketch_data;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;

    std::vector<seq::dna5> data(4'123u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
    }
    range_type range(data);
    auto resident_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto resident_view =
        bioseq::storage::make_lazy_sequence_view(resident_owner);
    collector_type resident_collector;
    REQUIRE_THROWS_AS(
        minimizer_dispatch_range<minimizer_type>(
            resident_collector,
            resident_view,
            0u,
            resident_view.bp_size(),
            10u,
            15u,
            minimizer_sketch_backend::bounded_nthash_simd,
            exact_simd_range_route::require_continuation,
            fixed_highway_kernels()),
        bounded_nthash_runtime_error);
    REQUIRE(resident_collector.extract_sketches_data().empty());

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    collector_type fragmented_collector;
    {
        bioseq::storage::lazy_sequence<
            packed_sequence, chunked_scanner> fragmented{
                chunked_scanner::from_range(range)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);
        REQUIRE_THROWS_AS(
            minimizer_dispatch_range<minimizer_type>(
                fragmented_collector,
                fragmented_view,
                0u,
                fragmented_view.bp_size(),
                10u,
                15u,
                minimizer_sketch_backend::bounded_nthash_simd,
                exact_simd_range_route::require_continuation,
                fixed_highway_kernels()),
            bounded_nthash_runtime_error);
    }
    REQUIRE(fragmented_collector.extract_sketches_data().empty());
    REQUIRE(admission.reserved_bytes() == reserved_before);
}

TEST_CASE("bounded ntHash materializes DNA4 slices and rejects every DNA5 width",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint8_t>();
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint16_t>();
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint32_t>();
    check_fragmented_bounded_nthash_range_materialization<seq::dna4, std::uint64_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint8_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint16_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint32_t>();
    check_bounded_nthash_rejects_dna5_coordinate<std::uint64_t>();
}

enum class injected_scanner_leaf {
    row_read,
    palloc,
    interrupt,
};

class injected_scanner_leaf_error final : public std::runtime_error {
public:
    explicit injected_scanner_leaf_error(injected_scanner_leaf point)
        : std::runtime_error(point == injected_scanner_leaf::row_read
              ? "injected scanner row-read failure"
              : point == injected_scanner_leaf::palloc
                  ? "injected scanner palloc failure"
                  : "injected scanner interrupt failure")
    {}
};

template <class p_storage_unit>
struct injected_failure_scanner final {
    using storage_unit = p_storage_unit;
    using base_container = std::span<const storage_unit>;
    static constexpr std::size_t fragment_storage_units = 2u;

    base_container backing{};
    bioseq::storage::sequence_size_t logical_bp_size = 0;
    std::size_t padding = 0;
    injected_scanner_leaf point = injected_scanner_leaf::row_read;
    std::size_t* reads = nullptr;

    [[nodiscard]] static constexpr bool is_fragmented() noexcept {
        return true;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t bp_size() const noexcept {
        return logical_bp_size;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t storage_unit_count() const noexcept {
        return static_cast<bioseq::storage::sequence_size_t>(backing.size());
    }
    [[nodiscard]] bioseq::storage::sequence_size_t storage_byte_count() const noexcept {
        return static_cast<bioseq::storage::sequence_size_t>(
            backing.size() * sizeof(storage_unit));
    }
    [[nodiscard]] std::size_t fragment_count() const noexcept {
        return (backing.size() + fragment_storage_units - 1u)
            / fragment_storage_units;
    }
    [[nodiscard]] bioseq::storage::sequence_size_t
    estimated_bp_size() const noexcept {
        return logical_bp_size;
    }
    [[nodiscard]] std::size_t trailing_padding() const noexcept {
        return padding;
    }
    [[nodiscard]] base_container load_fragment(std::size_t) const {
        if (reads != nullptr) {
            ++*reads;
        }
        throw injected_scanner_leaf_error{point};
    }
    void release_fragment(std::size_t) const noexcept {}
};

static_assert(bioseq::storage::fragmented_sequence_scanner<
    injected_failure_scanner<std::uint64_t>>);

TEST_CASE("fragmented bounded ntHash unwinds its vector and memory permit for every scanner PostgreSQL leaf failure",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using storage_unit_type = std::uint64_t;
    using packed_sequence = sequence_custom_alphabet<seq::dna4, storage_unit_type>;
    using range_type = compact_sequence_range<packed_sequence>;
    using scanner_type = injected_failure_scanner<storage_unit_type>;
    using lazy_type = bioseq::storage::lazy_sequence<
        packed_sequence, scanner_type>;
    using minimizer_type =
        ::bioseq::simd::minimap_sketch_data;
    using collector_type =
        ::bioseq::simd::basic_sketch_collector<minimizer_type>;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::minimizer_dispatch_range;

    auto bases = vulkan_pipeline_test_support::make_dna4(4'123, 0x51A9E5u);
    range_type range(bases);
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();

    for (const injected_scanner_leaf point : {
             injected_scanner_leaf::row_read,
             injected_scanner_leaf::palloc,
             injected_scanner_leaf::interrupt}) {
        CAPTURE(static_cast<unsigned>(point));
        std::size_t reads = 0;
        scanner_type scanner{
            .backing = std::span<const storage_unit_type>{
                range.read_cptr(range.cbegin()),
                static_cast<std::size_t>(range.size())},
            .logical_bp_size = static_cast<bioseq::storage::sequence_size_t>(
                range.bp_size()),
            .padding = static_cast<std::size_t>(range.padding()),
            .point = point,
            .reads = &reads,
        };
        lazy_type fragmented{std::move(scanner)};
        const auto fragmented_view =
            bioseq::storage::make_lazy_sequence_view(fragmented);
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
                minimizer_sketch_backend::bounded_nthash_simd,
                exact_simd_range_route::require_continuation,
                fixed_highway_kernels()),
            injected_scanner_leaf_error);
        REQUIRE(reads == 1u);
        REQUIRE(admission.reserved_bytes() == reserved_before);
    }
}

TEST_CASE("bounded ntHash SIMD pipe executes distinct sketch stats without fallback",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::base_alignment_algorithm;

    auto target = vulkan_pipeline_test_support::make_dna4(4000, 0x51A7C0DEu);
    constexpr std::size_t truth_start = 700;
    std::vector<seq::dna4> query(
        target.begin() + truth_start,
        target.begin() + truth_start + 900);

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.minimizer_algorithm =
        minimizer_algorithm::bounded_nthash_simd;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.base_alignment_algorithm =
        base_alignment_algorithm::block_aligner;
    params.do_cigar = true;

    const auto result = vulkan_pipeline_test_support::run(query, target, params);
    REQUIRE(result.is_bounded_nthash_query_execution_consistent(true));
    REQUIRE(result.is_bounded_nthash_target_execution_consistent(true));
    REQUIRE(result.is_simd_chaining_execution_consistent(
        params.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(result.is_block_aligner_execution_consistent(true));
    REQUIRE(result.scalar_pipeline_execution_stats_available);
    REQUIRE(result.scalar_pipeline_stats.seed_lookup_calls > 0u);
    REQUIRE(result.scalar_pipeline_stats.seed_query_minimizers > 0u);
    REQUIRE(result.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.is_all_simd_execution_consistent(
        minimizer_algorithm::two_stack_nthash,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto mixed_stats = result;
    mixed_stats.exact_simd_query_execution_stats_available = true;
    REQUIRE_FALSE(mixed_stats.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto missing_cpu_seed_stats = result;
    missing_cpu_seed_stats.scalar_pipeline_stats = {};
    missing_cpu_seed_stats.scalar_pipeline_execution_stats_available = false;
    REQUIRE_FALSE(missing_cpu_seed_stats.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto presence_only_cpu_seed_stats = missing_cpu_seed_stats;
    presence_only_cpu_seed_stats.scalar_pipeline_execution_stats_available = true;
    REQUIRE_FALSE(presence_only_cpu_seed_stats.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto hidden_scalar_chain = result;
    hidden_scalar_chain.scalar_pipeline_stats
        .record_scalar_chain(1u);
    REQUIRE_FALSE(hidden_scalar_chain.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    auto saturated_cpu_seed = result;
    saturated_cpu_seed.scalar_pipeline_stats.is_saturated = true;
    REQUIRE_FALSE(saturated_cpu_seed.is_all_simd_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        params.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(result.exact_simd_target_execution_stats_available);
    REQUIRE_FALSE(result.partial_vulkan_stats.has_any_dispatch());
    REQUIRE(result.partial_vulkan_stats
            == ::bioseq::simd::partial_vulkan_execution_stats{});

    auto vulkan_dispatch_tamper = result;
    vulkan_dispatch_tamper.partial_vulkan_stats
        .query_sketch_dispatches = 1;
    REQUIRE_FALSE(vulkan_dispatch_tamper
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto vulkan_metadata_tamper = result;
    vulkan_metadata_tamper.partial_vulkan_stats
        .gpu_index_unique_keys = 1;
    REQUIRE_FALSE(vulkan_metadata_tamper.partial_vulkan_stats
        .has_any_dispatch());
    REQUIRE_FALSE(vulkan_metadata_tamper
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto hidden_opposite_payload = result;
    hidden_opposite_payload.exact_simd_query_execution_stats.bp_consumed = 1;
    REQUIRE_FALSE(hidden_opposite_payload
        .exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(hidden_opposite_payload
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/true,
            /*require_nonempty_vector_work=*/true));

    auto persisted_stats = result;
    persisted_stats.bounded_nthash_target_execution_stats = {};
    persisted_stats.bounded_nthash_target_execution_stats_available = false;
    REQUIRE(persisted_stats.is_selected_simd_sketch_execution_consistent(
        minimizer_algorithm::bounded_nthash_simd,
        /*require_target_sketch=*/false,
        /*require_nonempty_vector_work=*/true));

    auto hidden_persisted_target_payload = persisted_stats;
    hidden_persisted_target_payload.bounded_nthash_target_execution_stats =
        result.bounded_nthash_target_execution_stats;
    REQUIRE_FALSE(hidden_persisted_target_payload
        .bounded_nthash_target_execution_stats_available);
    REQUIRE_FALSE(hidden_persisted_target_payload
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
            /*require_target_sketch=*/false,
            /*require_nonempty_vector_work=*/true));

    auto target_presence_only = persisted_stats;
    target_presence_only.bounded_nthash_target_execution_stats_available = true;
    REQUIRE_FALSE(target_presence_only
        .is_selected_simd_sketch_execution_consistent(
            minimizer_algorithm::bounded_nthash_simd,
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

TEST_CASE("bounded ntHash segmented pipe globalizes local coordinates without fallback",
          "[component:bioseq][layer:unit][feature:minimizer][resource:highway]") {
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::base_alignment_algorithm;

    // Force the production geometry to its 32-Mbp floor, then place the truth
    // strictly inside the second core. This exercises two real target-range
    // sketches without introducing a test-only segmentation implementation.
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t value;
        ~restore_cap() { controller.set_index_memory_cap_bytes(value); }
    } restore_cap{admission, previous_cap};
    admission.set_index_memory_cap_bytes(192'000'000u);

    constexpr std::size_t target_length = 33'000'000;
    constexpr std::size_t truth_start = 32'250'000;
    constexpr std::size_t query_length = 40'000;
    auto target = vulkan_pipeline_test_support::make_dna4(
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

    vulkan_pipeline_test_support::range_t query_range(query);
    vulkan_pipeline_test_support::range_t target_range(target);
    auto lazy_query = bioseq::storage::make_resident_lazy_sequence(query_range);
    auto lazy_target = bioseq::storage::make_resident_lazy_sequence(target_range);
    const auto query_view =
        bioseq::storage::make_lazy_sequence_view(lazy_query);
    const auto target_view =
        bioseq::storage::make_lazy_sequence_view(lazy_target);

    minimap2_parameters params{};
    bind_fixed_highway(params);
    params.minimizer_algorithm =
        minimizer_algorithm::bounded_nthash_simd;
    params.chaining_algorithm = chaining_algorithm::simd_minimap2;
    params.base_alignment_algorithm = base_alignment_algorithm::ksw2;
    params.do_cigar = false;

    const auto result = ::bioseq::simd::
        minimap_port_segmented(query_view, target_view, params);
    REQUIRE(result.is_bounded_nthash_query_execution_consistent(true));
    REQUIRE(result.is_bounded_nthash_target_execution_consistent(true));
    REQUIRE(result.bounded_nthash_target_execution_stats.segments_combined >= 2);
    REQUIRE(result.is_simd_chaining_execution_consistent(
        params.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE_FALSE(result.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(result.exact_simd_target_execution_stats_available);
    REQUIRE_FALSE(result.partial_vulkan_stats.has_any_dispatch());

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
          "[component:bioseq][layer:unit][feature:planner]") {
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::pipe::
        complete_all_gpu_tuple_selected;
    using ::bioseq::simd::pipe::vulkan_accelerator_requested;
    using ::bioseq::simd::pipe::vulkan_components_requested;

    minimap2_parameters params{};
    params.vulkan_enabled = true;
    params.vulkan_index_cache_enabled = true;
    REQUIRE_FALSE(vulkan_components_requested(params));
    REQUIRE_FALSE(vulkan_accelerator_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));

    params.prefilter_enabled = true;
    params.prefilter_backend =
        bioseq::prefilter::prefilter_backend::vulkan;
    REQUIRE_FALSE(vulkan_components_requested(params));
    REQUIRE(vulkan_accelerator_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
    params.prefilter_enabled = false;

    params.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    REQUIRE(vulkan_components_requested(params));
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));

    params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
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
        ::bioseq::simd::base_alignment_algorithm::ksw2;
    REQUIRE_FALSE(complete_all_gpu_tuple_selected(params));
}

TEST_CASE("persisted grouped-SoA GPU preparation admits zero-based supported alphabets",
          "[component:bioseq][layer:unit][feature:planner]") {
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::pipe::
        is_persisted_grouped_soa_gpu_preparation_tuple_valid;

    constexpr std::size_t supported_alphabet_count = 2u;
    constexpr std::uint8_t dna4_code = 0u;
    constexpr std::uint8_t dna5_code = 1u;

    minimap2_parameters params{};
    params.vulkan_enabled = true;
    params.vulkan_seed_hits_enabled = true;
    params.vulkan_fused_frontend_enabled = true;
    params.vulkan_gpu_index_build_enabled = true;
    params.vulkan_extend_enabled = true;
    params.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    params.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    params.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
    params.compute_divergence = false;
    params.compute_mapq = false;
    params.max_occ = 500;

    const auto admitted = [&](const std::uint8_t alphabet_code,
                              const std::uint8_t byte_width,
                              const minimizer_sketch_backend backend =
                                  minimizer_sketch_backend::vulkan_segment) {
        return is_persisted_grouped_soa_gpu_preparation_tuple_valid(
            alphabet_code, supported_alphabet_count, byte_width,
            backend, params);
    };

    REQUIRE(admitted(dna4_code, 1u));
    REQUIRE(admitted(dna4_code, 2u));
    REQUIRE(admitted(dna5_code, 4u));
    REQUIRE(admitted(dna5_code, 8u));
    REQUIRE_FALSE(is_persisted_grouped_soa_gpu_preparation_tuple_valid(
        dna4_code, 0u, 1u, minimizer_sketch_backend::vulkan_segment,
        params));
    REQUIRE_FALSE(admitted(
        static_cast<std::uint8_t>(supported_alphabet_count), 1u));
    REQUIRE_FALSE(admitted((std::numeric_limits<std::uint8_t>::max)(), 1u));
    REQUIRE_FALSE(admitted(dna4_code, 0u));
    REQUIRE_FALSE(admitted(dna4_code, 3u));
    REQUIRE_FALSE(admitted(dna4_code, 16u));
    REQUIRE_FALSE(admitted(
        dna4_code, 1u, minimizer_sketch_backend::scalar));

    params.vulkan_enabled = false;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
    params.vulkan_enabled = true;

    params.kmer_size = 14;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
    params.kmer_size = 15;
    params.window_size = 9;
    REQUIRE_FALSE(admitted(dna4_code, 1u));
}

TEST_CASE("bounded Vulkan pipe matches complete CPU biological results",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::minimizer_sketch_backend;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::pipe::
        minimizer_range_execution_stats;
    using ::bioseq::simd::pipe::exact_simd_range_route;
    using ::bioseq::simd::pipe::
        minimizer_range_stats_matches_backend;

    auto target = vulkan_pipeline_test_support::make_dna4(4000, 0xC0FFEEu);
    // Query = a forward sub-region of the target (a guaranteed match).
    std::vector<seq::dna4> query(target.begin() + 500, target.begin() + 1300);

    minimap2_parameters cpu;
    cpu.minimizer_algorithm = minimizer_algorithm::two_stack_nthash;
    cpu.chaining_algorithm = chaining_algorithm::simd_minimap2;
    cpu.base_alignment_algorithm = base_alignment_algorithm::block_aligner;
    const auto cpu_res = vulkan_pipeline_test_support::run(query, target, cpu);
    REQUIRE(cpu.do_cigar);
    REQUIRE(cpu_res.is_exact_simd_query_execution_consistent(true));
    REQUIRE(cpu_res.is_exact_simd_target_execution_consistent(true));
    REQUIRE(cpu_res.is_simd_chaining_execution_consistent(
        cpu.simd_chaining_policy,
        /*require_nonempty_vector_work=*/true));
    REQUIRE(cpu_res.is_block_aligner_execution_consistent(
        /*require_nonempty_simd_work=*/true));
    REQUIRE(cpu_res.scalar_pipeline_execution_stats_available);
    REQUIRE(cpu_res.scalar_pipeline_stats.seed_lookup_calls > 0u);
    REQUIRE(cpu_res.scalar_pipeline_stats.seed_query_minimizers > 0u);
    REQUIRE(cpu_res.partial_vulkan_stats
            == ::bioseq::simd::partial_vulkan_execution_stats{});
    REQUIRE(cpu_res.is_all_simd_execution_consistent(
        minimizer_algorithm::two_stack_nthash,
        cpu.simd_chaining_policy,
        /*require_target_sketch=*/true,
        /*require_nonempty_vector_work=*/true));

    minimizer_range_execution_stats exact_range{};
    exact_range.exact_simd = cpu_res.exact_simd_query_execution_stats;
    exact_range.exact_simd_available = true;
    // This parity fixture deliberately uses dna4/u64 (`vulkan_pipeline_test_support::range_t`).
    // The resident vector_stream route is restricted to dna4/u32,
    // so the authentic exact SIMD stats here is the chronological continuation
    // route.  Resident routing has its own u32 test above; relabelling this u64
    // stats as resident would weaken the persisted-scheme boundary.
    REQUIRE(exact_range.exact_simd.resident_vector_stream_kernel_calls == 0u);
    REQUIRE((exact_range.exact_simd.generic_u64_kernel_calls > 0u
        || exact_range.exact_simd.kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0u));
    REQUIRE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        exact_range,
        /*require_nonempty_vector_work=*/true,
        exact_simd_range_route::require_continuation));
    auto hidden_bounded_payload = exact_range;
    hidden_bounded_payload.bounded_nthash.scheme_id = 1;
    REQUIRE_FALSE(hidden_bounded_payload.bounded_nthash_available);
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        hidden_bounded_payload,
        /*require_nonempty_vector_work=*/false,
        exact_simd_range_route::require_continuation));
    auto bounded_presence_only = exact_range;
    bounded_presence_only.bounded_nthash_available = true;
    REQUIRE_FALSE(minimizer_range_stats_matches_backend(
        minimizer_sketch_backend::exact_simd,
        bounded_presence_only,
        /*require_nonempty_vector_work=*/false,
        exact_simd_range_route::require_continuation));

    minimap2_parameters gpu = cpu;
    gpu.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    gpu.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    gpu.vulkan_enabled = true;
    gpu.vulkan_seed_hits_enabled = true;
    if (!::bioseq::chaining::is_vulkan_chaining_supported()) {
        REQUIRE_THROWS_AS(
            vulkan_pipeline_test_support::run(query, target, gpu),
            bioseq::gpu::vulkan::vulkan_runtime_error);
        return;
    }
    const auto gpu_res = vulkan_pipeline_test_support::run(query, target, gpu);

    REQUIRE_FALSE(cpu_res.results.empty());
    REQUIRE_FALSE(gpu_res.results.empty());
    REQUIRE(gpu.do_cigar);
    REQUIRE_FALSE(gpu_res.exact_simd_query_execution_stats_available);
    REQUIRE_FALSE(gpu_res.exact_simd_target_execution_stats_available);
    REQUIRE_FALSE(gpu_res.simd_chaining_execution_stats_available);
    REQUIRE_FALSE(gpu_res.scalar_pipeline_execution_stats_available);
    REQUIRE(gpu_res.scalar_pipeline_stats
            == ::bioseq::simd::scalar_pipeline_execution_stats{});
    REQUIRE(gpu_res.partial_vulkan_stats
                .is_exact_stage_mask_consistent(
                    /*query_sketch=*/true,
                    /*target_sketch=*/true,
                    /*target_index_build=*/false,
                    /*seed_lookup=*/true,
                    /*chain_score=*/true));
    REQUIRE(gpu_res.is_block_aligner_execution_consistent(
        /*require_nonempty_simd_work=*/true));
    REQUIRE(vulkan_pipeline_test_support::exact_biological_results_equal(cpu_res, gpu_res));
    REQUIRE(cpu_res.biological_identity()
            == gpu_res.biological_identity());
}

TEST_CASE("vulkan_minimap_integer with vulkan_enabled=false fails",
          "[component:bioseq][layer:unit][feature:configuration]") {
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using ::bioseq::simd::chaining_algorithm;

    auto target = vulkan_pipeline_test_support::make_dna4(3000, 0x1234567u);
    std::vector<seq::dna4> query(target.begin() + 200, target.begin() + 900);

    minimap2_parameters disabled;
    disabled.minimizer_algorithm = minimizer_algorithm::vulkan_minimap_integer;
    disabled.chaining_algorithm = chaining_algorithm::vulkan_tiled_dynamic_programming;
    disabled.vulkan_enabled = false;

    REQUIRE_THROWS_AS(
        vulkan_pipeline_test_support::run(query, target, disabled),
        bioseq::gpu::vulkan::vulkan_runtime_error);
}

namespace resident_pipeline_test_support {

using operation_state_type = ::bioseq::simd::pipe::
    direct_resident_operation_stats;
using source_authority_type = ::bioseq::simd::pipe::
    source_direct_host_authorization;
using resident_authority_type = bioseq::indexing::
    gpu_resident_accounting_authority;

[[nodiscard]] operation_state_type full_resident_state(resident_authority_type authority) {
    operation_state_type state{};
    state.operation_identity = 17u;
    state.minimizer_count = 53u;
    state.distinct_count = 17u;
    state.logical_control_bytes = 272u;
    state.logical_position_bytes = 424u;
    state.logical_resident_bytes = 696u;
    state.control_requirement_bytes = 1'024u;
    state.position_requirement_bytes = 3'072u;
    state.control_allowance_bytes = 1'024u;
    state.position_allowance_bytes = 3'072u;
    state.conservative_allowance_bytes = 4'096u;
    state.conservative_request_bytes = 4'096u;
    state.source_capacity_bytes = 512u;
    state.source_releases = 1u;
    state.source_released_bytes = 512u;
    state.cache_preinsertions = 1u;
    state.cache_required_lookups = 1u;
    state.cache_required_hits = 1u;
    state.mapped_prefix_passes = 2u;
    state.mapped_prefix_lookups = 20u;
    state.mapped_prefix_comparisons = 80u;
    state.expected_seed_consumers = 1u;
    state.completed_seed_consumers = 1u;
    state.slab_requested_bytes = 1'024u;
    state.slab_actual_bytes = 1'024u;
    state.slab_retained_permit_bytes = 1'024u;
    state.slab_permit_releases = 1u;
    state.seed_reserved_bytes = 4'096u;
    state.seed_planned_vulkan_bytes = 2'048u;
    state.seed_actual_vulkan_bytes = 2'048u;
    state.seed_roundtrips = 1u;
    state.seed_dispatches = 1u;
    state.seed_fence_completions = 1u;
    state.cache_cleanup_releases = 1u;
    state.resident_cleanup_bytes = 4'096u;
    state.materialization = {
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
            authority == resident_authority_type::reflected_process_footprint
                ? 4'096u : 0u,
        .retained_permit_bytes =
            authority == resident_authority_type::retained_exact_permit
                ? 4'096u : 0u,
        .permit_reconciliations = 1u,
        .complete_source_traversals = 1u,
        .complete_mapped_traversals = 1u,
        .authority = authority,
    };
    state.source_authority =
        authority == resident_authority_type::reflected_process_footprint
            ? source_authority_type::reflected_process_footprint
            : source_authority_type::retained_exact_permit;
    state.gpu_target_index_route_checked = true;
    state.gpu_target_index_route_compatible = true;
    state.source_authority_checked = true;
    state.has_valid_source_authorization = true;
    state.runtime_checked = true;
    state.runtime_available = true;
    state.hardware_checked = true;
    state.hardware_available = true;
    state.sorted_shape_checked = true;
    state.is_sorted_shape_accepted = true;
    state.identity_checked = true;
    state.is_identity_accepted = true;
    state.u32_shape_checked = true;
    state.is_u32_shape_accepted = true;
    state.storage_ranges_checked = true;
    state.is_control_range_accepted = true;
    state.is_position_range_accepted = true;
    state.mapping_plan_checked = true;
    state.is_mapping_plan_accepted = true;
    state.admission_snapshot_taken = true;
    state.is_planning_accepted = true;
    state.route_committed = true;
    state.materialization_started = true;
    state.materialization_completed = true;
    state.resident_published = true;
    state.source_release_completed = true;
    state.source_released_after_publication = true;
    state.cache_lookup_started = true;
    state.mapped_prefix_started = true;
    state.slab_admission_started = true;
    state.seed_dispatch_started = true;
    state.pipeline_started = true;
    state.pipeline_completed = true;
    state.log_planning("proceed", "focused_test_fixture");
    return state;
}

[[nodiscard]] operation_state_type canonical_empty_state() {
    operation_state_type state{};
    state.operation_identity = 18u;
    state.source_authority = source_authority_type::canonical_empty;
    state.gpu_target_index_route_checked = true;
    state.gpu_target_index_route_compatible = true;
    state.source_authority_checked = true;
    state.has_valid_source_authorization = true;
    state.runtime_checked = true;
    state.runtime_available = true;
    state.hardware_checked = true;
    state.hardware_available = true;
    state.sorted_shape_checked = true;
    state.is_sorted_shape_accepted = true;
    state.identity_checked = true;
    state.is_identity_accepted = true;
    state.u32_shape_checked = true;
    state.is_u32_shape_accepted = true;
    state.storage_ranges_checked = true;
    state.is_control_range_accepted = true;
    state.is_position_range_accepted = true;
    state.mapping_plan_checked = true;
    state.is_mapping_plan_accepted = true;
    state.admission_snapshot_taken = true;
    state.canonical_empty = true;
    state.is_planning_accepted = true;
    state.route_committed = true;
    state.source_release_completed = true;
    state.empty_bypass = true;
    state.log_planning("proceed", "focused_empty_fixture");
    return state;
}

}  // namespace resident_pipeline_test_support

TEST_CASE("source-direct operation emits an exact two-receipt transaction",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    using resident_pipeline_test_support::operation_state_type;
    using resident_pipeline_test_support::resident_authority_type;

    SECTION("reflected authority") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::reflected_process_footprint);
        REQUIRE(state.route_name() == "full_sorted_dense_direct_mapped");
        REQUIRE(state.planning_receipts == 1u);
        REQUIRE(state.terminal_success_invariants());
        state.log_terminal("success", "focused_success");
        REQUIRE(state.terminal_receipts == 1u);
        REQUIRE(state.terminal_success);
        state.log_terminal("failure", "must_not_duplicate");
        REQUIRE(state.terminal_receipts == 1u);
        REQUIRE(state.terminal_success);
    }

    SECTION("retained exact permit authority") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::retained_exact_permit);
        REQUIRE(state.terminal_success_invariants());
    }

    SECTION("zero anchors still complete two mapped-prefix passes") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::retained_exact_permit);
        state.slab_requested_bytes = 0u;
        state.slab_actual_bytes = 0u;
        state.slab_retained_permit_bytes = 0u;
        state.slab_permit_releases = 0u;
        state.seed_reserved_bytes = 0u;
        state.seed_planned_vulkan_bytes = 0u;
        state.seed_actual_vulkan_bytes = 0u;
        state.zero_anchor_bypasses = 1u;
        state.seed_dispatches = 0u;
        state.seed_fence_completions = 0u;
        REQUIRE(state.mapped_prefix_passes == 2u);
        REQUIRE(state.terminal_success_invariants());
        --state.mapped_prefix_passes;
        REQUIRE_FALSE(state.terminal_success_invariants());
    }

    SECTION("zero query is an explicit post-publication pipeline bypass") {
        auto state = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::reflected_process_footprint);
        state.expected_seed_consumers = 0u;
        state.completed_seed_consumers = 0u;
        state.cache_required_lookups = 0u;
        state.cache_required_hits = 0u;
        state.mapped_prefix_passes = 0u;
        state.mapped_prefix_lookups = 0u;
        state.mapped_prefix_comparisons = 0u;
        state.slab_requested_bytes = 0u;
        state.slab_actual_bytes = 0u;
        state.slab_retained_permit_bytes = 0u;
        state.slab_permit_releases = 0u;
        state.seed_reserved_bytes = 0u;
        state.seed_planned_vulkan_bytes = 0u;
        state.seed_actual_vulkan_bytes = 0u;
        state.seed_roundtrips = 0u;
        state.seed_dispatches = 0u;
        state.seed_fence_completions = 0u;
        state.zero_query_bypasses = 1u;
        state.cache_lookup_started = false;
        state.mapped_prefix_started = false;
        state.slab_admission_started = false;
        state.seed_dispatch_started = false;
        state.pipeline_started = false;
        state.pipeline_completed = false;
        REQUIRE(state.terminal_success_invariants());
    }

    SECTION("canonical empty publishes neither resident nor fake cleanup") {
        auto state = resident_pipeline_test_support::canonical_empty_state();
        REQUIRE(state.route_name() == "empty_no_resident_zero_anchor");
        REQUIRE(state.terminal_success_invariants());
        state.resident_published = true;
        REQUIRE_FALSE(state.terminal_success_invariants());
    }

    SECTION("failure and not-reached unwind remain exactly two receipts") {
        auto invalid = resident_pipeline_test_support::full_resident_state(
            resident_authority_type::reflected_process_footprint);
        --invalid.materialization.fully_touched_bytes;
        REQUIRE_FALSE(invalid.terminal_success_invariants());
        invalid.log_terminal("success", "injected_touch_failure");
        REQUIRE(invalid.terminal_receipts == 1u);
        REQUIRE_FALSE(invalid.terminal_success);

        operation_state_type not_reached{};
        not_reached.operation_identity = 19u;
        {
            ::bioseq::simd::pipe::
                direct_resident_receipt_guard guard{&not_reached};
        }
        REQUIRE(not_reached.planning_receipts == 1u);
        REQUIRE(not_reached.terminal_receipts == 1u);
        REQUIRE_FALSE(not_reached.terminal_success);
    }
}

TEST_CASE("bounded real-GPU prepared resident matches independent ordered sketch",
          "[component:bioseq][layer:unit][feature:pipeline][resource:gpu]") {
    namespace vk = bioseq::gpu::vulkan;
    using ::bioseq::simd::base_alignment_algorithm;
    using ::bioseq::simd::chaining_algorithm;
    using ::bioseq::simd::match_result_collection;
    using ::bioseq::simd::minimap2_parameters;
    using ::bioseq::simd::minimizer_algorithm;
    using direct_minimizer_type =
        ::bioseq::simd::minimizer_representation<33u, 15u>;

    auto& runtime = vk::runtime::instance();
    if (!vk::hardware_device_available(runtime)) {
        SUCCEED("hardware Vulkan unavailable; prepared-resident parity skipped");
        return;
    }

    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    const std::uint64_t pipeline_cache_metadata_baseline =
        runtime.pipeline_cache_metadata_reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t cap = 0u;
        ~restore_cap() { controller.set_index_memory_cap_bytes(cap); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap,
        bioseq::common::saturating_add(
            baseline, std::uint64_t{512} << 20u)));

    match_result_collection direct_results;
    match_result_collection ordered_results;
    match_result_collection empty_minimizer_results;
    {
        auto target_letters =
            vulkan_pipeline_test_support::make_dna4(20'000u, 0x0384c0ffeeULL);
        std::vector<seq::dna4> query_letters(
            target_letters.begin() + 5'000,
            target_letters.begin() + 6'500);
        vulkan_pipeline_test_support::range_t query_range(query_letters);
        vulkan_pipeline_test_support::range_t target_range(target_letters);
        auto gpu_query_owner =
            bioseq::storage::make_resident_lazy_sequence(query_range);
        auto gpu_target_owner =
            bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto gpu_query_view =
            bioseq::storage::make_lazy_sequence_view(gpu_query_owner);
        const auto gpu_target_view =
            bioseq::storage::make_lazy_sequence_view(gpu_target_owner);

        auto target_sketch =
            vk::collect_minimizers_vulkan_result<direct_minimizer_type>(
                gpu_target_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u,
                nullptr,
                vk::vulkan_sketch_host_ownership_route::
                    fixed_zero_copy_adoption);
        REQUIRE(target_sketch.has_value());
        REQUIRE(target_sketch->dispatch_token.is_internally_consistent());
        REQUIRE_FALSE(target_sketch->host_minimizers.empty());
        auto target_owner =
            ::bioseq::simd::pipe_entry_detail::
                adopt_fixed_vulkan_sketch_host_result(
                    std::move(*target_sketch));
        REQUIRE(target_owner.has_valid_ownership_authorization());

        auto query_sketch =
            vk::collect_minimizers_vulkan_result<direct_minimizer_type>(
                gpu_query_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u);
        REQUIRE(query_sketch.has_value());
        REQUIRE(query_sketch->dispatch_token.is_internally_consistent());
        REQUIRE_FALSE(query_sketch->host_minimizers.empty());

        minimap2_parameters direct_params{};
        bind_fixed_highway(direct_params);
        direct_params.chaining_algorithm =
            chaining_algorithm::vulkan_tiled_dynamic_programming;
        direct_params.base_alignment_algorithm =
            base_alignment_algorithm::block_aligner;
        direct_params.minimizer_algorithm =
            minimizer_algorithm::vulkan_minimap_integer;
        direct_params.vulkan_enabled = true;
        direct_params.vulkan_seed_hits_enabled = true;
        direct_params.vulkan_index_cache_enabled = true;
        direct_params.vulkan_fused_frontend_enabled = false;
        direct_params.vulkan_gpu_index_build_enabled = false;
        direct_params.vulkan_extend_enabled = false;
        direct_params.do_cigar = true;

        auto direct_lazy_query =
            bioseq::storage::make_resident_lazy_sequence(query_range);
        auto direct_lazy_target =
            bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto direct_query_view =
            bioseq::storage::make_lazy_sequence_view(direct_lazy_query);
        const auto direct_target_view =
            bioseq::storage::make_lazy_sequence_view(direct_lazy_target);
        direct_results =
            ::bioseq::simd::pipe_entry_detail::
                minimap2_port_with_fixed_vulkan_target_handoff(
                    direct_query_view,
                    direct_target_view,
                    query_sketch->host_minimizers,
                    std::move(target_owner),
                    direct_params);
        query_sketch.reset();
        target_sketch.reset();

        minimap2_parameters ordered_params = direct_params;
        ordered_params.vulkan_fused_frontend_enabled = true;
        ordered_params.vulkan_gpu_index_build_enabled = true;
        auto ordered_lazy_query =
            bioseq::storage::make_resident_lazy_sequence(query_range);
        auto ordered_lazy_target =
            bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto ordered_query_view =
            bioseq::storage::make_lazy_sequence_view(ordered_lazy_query);
        const auto ordered_target_view =
            bioseq::storage::make_lazy_sequence_view(ordered_lazy_target);
        ordered_results =
            ::bioseq::simd::minimap_port(
                ordered_query_view,
                ordered_target_view,
                ordered_params);

        // A nonempty query shorter than k has no minimizers, but the already
        // materialized source-direct resident still owns the two required
        // mapped-payload authority passes. Rebuild the small bounded resident
        // and exercise the production wrapper so the generic early-empty seed
        // shortcut cannot regress over this case.
        std::vector<seq::dna4> short_query_letters(
            target_letters.begin(), target_letters.begin() + 8);
        vulkan_pipeline_test_support::range_t short_query_range(short_query_letters);
        auto short_query_owner =
            bioseq::storage::make_resident_lazy_sequence(short_query_range);
        const auto short_query_view =
            bioseq::storage::make_lazy_sequence_view(short_query_owner);
        auto empty_query_sketch =
            vk::collect_minimizers_vulkan_result<direct_minimizer_type>(
                short_query_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u);
        REQUIRE(empty_query_sketch.has_value());
        REQUIRE(empty_query_sketch->dispatch_token.is_internally_consistent());
        REQUIRE(empty_query_sketch->host_minimizers.empty());
        auto second_target_sketch =
            vk::collect_minimizers_vulkan_result<direct_minimizer_type>(
                gpu_target_view,
                /*window_size=*/10u,
                /*kmer_size=*/15u,
                nullptr,
                vk::vulkan_sketch_host_ownership_route::
                    fixed_zero_copy_adoption);
        REQUIRE(second_target_sketch.has_value());
        REQUIRE(second_target_sketch->dispatch_token.is_internally_consistent());
        auto second_target_owner =
            ::bioseq::simd::pipe_entry_detail::
                adopt_fixed_vulkan_sketch_host_result(
                    std::move(*second_target_sketch));
        auto empty_lazy_query =
            bioseq::storage::make_resident_lazy_sequence(short_query_range);
        auto empty_lazy_target =
            bioseq::storage::make_resident_lazy_sequence(target_range);
        const auto empty_query_view =
            bioseq::storage::make_lazy_sequence_view(empty_lazy_query);
        const auto empty_target_view =
            bioseq::storage::make_lazy_sequence_view(empty_lazy_target);
        empty_minimizer_results =
            ::bioseq::simd::pipe_entry_detail::
                minimap2_port_with_fixed_vulkan_target_handoff(
                    empty_query_view,
                    empty_target_view,
                    empty_query_sketch->host_minimizers,
                    std::move(second_target_owner),
                    direct_params);
    }

    REQUIRE_FALSE(direct_results.results.empty());
    REQUIRE_FALSE(ordered_results.results.empty());
    const auto& direct = direct_results.partial_vulkan_stats;
    const auto& ordered_sketch = ordered_results.partial_vulkan_stats;
    REQUIRE_FALSE(direct.is_saturated);
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

    REQUIRE(direct.seed_anchor_stats_operations
            == direct.gpu_index_source_direct_seed_roundtrips);
    REQUIRE(direct.seed_canonical_anchor_order_checks
            == direct.seed_anchor_stats_operations);
    REQUIRE(direct.seed_canonical_anchor_order_accepts
            == direct.seed_canonical_anchor_order_checks);
    REQUIRE(direct.seed_canonical_anchor_count
            == ordered_sketch.seed_canonical_anchor_count);
    REQUIRE(direct.seed_anchor_digest_lane0
            == ordered_sketch.seed_anchor_digest_lane0);
    REQUIRE(direct.seed_anchor_digest_lane1
            == ordered_sketch.seed_anchor_digest_lane1);
    REQUIRE(vulkan_pipeline_test_support::exact_biological_results_equal(
        direct_results, ordered_results));
    REQUIRE(direct_results.biological_identity()
            == ordered_results.biological_identity());

    REQUIRE(empty_minimizer_results.results.empty());
    const auto& empty_minimizer =
        empty_minimizer_results.partial_vulkan_stats;
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
    REQUIRE(empty_minimizer.seed_anchor_stats_operations == 1u);
    REQUIRE(empty_minimizer.seed_canonical_anchor_count == 0u);
    REQUIRE(empty_minimizer.seed_anchor_digest_lane0
            == vk::k_seed_anchor_digest_seed_low);
    REQUIRE(empty_minimizer.seed_anchor_digest_lane1
            == vk::k_seed_anchor_digest_seed_high);
    REQUIRE(empty_minimizer.gpu_index_source_direct_pipeline_completions == 1u);

    const std::uint64_t pipeline_cache_metadata_after =
        runtime.pipeline_cache_metadata_reserved_bytes();
    REQUIRE(pipeline_cache_metadata_after
            >= pipeline_cache_metadata_baseline);
    REQUIRE(admission.reserved_bytes()
            == bioseq::common::saturating_add(
                baseline,
                pipeline_cache_metadata_after
                    - pipeline_cache_metadata_baseline));
}

TEST_CASE("post-seed owned lifetime releases only after the final seed user",
          "[component:bioseq][layer:unit][feature:seeding]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    std::vector<sketch_type> sketches{
        sketch_type{/*kmer*/ 7, /*pos*/ 1, /*strand*/ 0},
        sketch_type{/*kmer*/ 7, /*pos*/ 3, /*strand*/ 1},
        sketch_type{/*kmer*/ 9, /*pos*/ 5, /*strand*/ 0},
    };
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build(sketches, 100);
    const std::uint64_t position_capacity_bytes =
        index.positions.capacity() * sizeof(std::uint64_t);
    REQUIRE(position_capacity_bytes != 0u);

    post_seed_owned_lifetime lifetime{
        /*seed_users=*/2u,
        /*owned_gpu_cache=*/nullptr,
        transient_target_index_release_authorization{
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
          "[component:bioseq][layer:unit][feature:seeding]") {
    using sketch_type = ::bioseq::simd::minimap_sketch_data;
    using ::bioseq::indexing::minimizer_lookup_spans;
    using ::bioseq::indexing::minimizer_position_span;
    struct segmented_seed_index {
        std::array<std::uint64_t, 2> first{
            (std::uint64_t{100} << 1u),
            (std::uint64_t{200} << 1u) | 1u,
        };
        std::array<std::uint64_t, 1> second{
            (std::uint64_t{300} << 1u),
        };
        std::array<minimizer_position_span, 2> spans{
            minimizer_position_span{first},
            minimizer_position_span{second},
        };
        bool positions_available = true;
        std::uint64_t reported_count = 3;

        [[nodiscard]] minimizer_lookup_spans lookup_spans(
                std::uint64_t hash) const noexcept {
            if (hash != 7u) {
                return minimizer_lookup_spans::segmented(
                    {}, /*global_occurrence_count=*/0);
            }
            if (!positions_available) {
                return minimizer_lookup_spans::positions_unavailable(
                    reported_count);
            }
            return minimizer_lookup_spans::segmented(
                spans, reported_count);
        }

        [[nodiscard]] bioseq::storage::sequence_size_t target_length()
                const noexcept {
            return 1'000;
        }
    };
    STATIC_REQUIRE(::bioseq::indexing::minimizer_lookup_index<
        segmented_seed_index>);

    const std::array<sketch_type, 1> query{
        sketch_type{/*kmer=*/7, /*pos=*/10, /*strand=*/0},
    };

    SECTION("all spans emit when their global count is admitted") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::anchor>();
        segmented_seed_index index;
        ::bioseq::simd::collect_seed_hits(
            std::span<const sketch_type>{query},
            index,
            /*query_length=*/100,
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
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::anchor>();
        segmented_seed_index index;
        ::bioseq::simd::collect_seed_hits(
            std::span<const sketch_type>{query},
            index,
            /*query_length=*/100,
            /*max_occ=*/2,
            /*kmer_size=*/15,
            anchors);
        REQUIRE(anchors.size() == 0);
    }

    SECTION("global pruning does not require deliberately unloaded positions") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::anchor>();
        segmented_seed_index index;
        index.positions_available = false;
        ::bioseq::simd::collect_seed_hits(
            std::span<const sketch_type>{query},
            index,
            /*query_length=*/100,
            /*max_occ=*/2,
            /*kmer_size=*/15,
            anchors);
        REQUIRE(anchors.size() == 0);
    }

    SECTION("a surviving hash without positions fails closed") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::anchor>();
        segmented_seed_index index;
        index.positions_available = false;
        REQUIRE_THROWS_AS(
            ::bioseq::simd::collect_seed_hits(
                std::span<const sketch_type>{query},
                index,
                /*query_length=*/100,
                /*max_occ=*/3,
                /*kmer_size=*/15,
                anchors),
            ::bioseq::indexing::persisted_minimizer_lookup_error);
    }

    SECTION("an admitted malformed global count fails closed") {
        context::context ctx;
        context::installed_context_scope installed{ctx};
        auto scope = ctx.push<context::arena::chain>();
        auto anchors = ::bioseq::chaining::make_arena_slab<
            ::bioseq::chaining::anchor>();
        segmented_seed_index index;
        index.reported_count = 2;
        REQUIRE_THROWS_AS(
            ::bioseq::simd::collect_seed_hits(
                std::span<const sketch_type>{query},
                index,
                /*query_length=*/100,
                /*max_occ=*/2,
                /*kmer_size=*/15,
                anchors),
            ::bioseq::indexing::persisted_minimizer_lookup_error);
        REQUIRE(anchors.size() == 0);
    }
}

TEST_CASE("Vulkan chaining mapped plan reports exact GPU and CPU scratch peaks",
          "[component:bioseq][layer:unit][feature:chaining]") {
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
    REQUIRE(plan.gpu_execution_bytes == 55'266'544u);
    REQUIRE(plan.host_backtracking_bytes == 61'094'040u);
    REQUIRE(plan.total_bytes == 61'094'040u);

    bioseq::parallelism::memory_admission_failure failure{
        .reason = bioseq::parallelism::memory_admission_failure_reason::system_pressure,
        .requested_bytes = plan.total_bytes,
        .reserved_bytes = 17u,
        .budget_ceiling_bytes = 19u,
        .allocatable_after_reservations_bytes = 2u,
        .process_footprint_bytes = 23u,
        .system_available_bytes = 29u,
        .system_total_bytes = 31u,
        .process_soft_cap_bytes = 37u,
    };
    const ::bioseq::chaining::vulkan_chaining_admission_error error{
        plan, failure};
    REQUIRE(error.plan.total_bytes == plan.total_bytes);
    REQUIRE(error.admission.reason
            == bioseq::parallelism::memory_admission_failure_reason::system_pressure);
    REQUIRE(std::string_view{error.what()}.find("admission_reason=system_pressure")
            != std::string_view::npos);
    REQUIRE(std::string_view{error.what()}.find("range_count=32035")
            != std::string_view::npos);
}

TEST_CASE("Vulkan chaining shape preflight exactly matches in-place range compaction",
          "[component:bioseq][layer:unit][feature:chaining]") {
    constexpr std::size_t anchor_count =
        ::bioseq::chaining::vulkan_chaining_detail::
            k_gpu_chain_compact_range_threshold + 2u;
    std::vector<::bioseq::chaining::anchor> anchors(anchor_count);
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        anchors[i].ref_position = i;
        anchors[i].query_position = i;
        anchors[i].query_span = 15u;
    }

    const auto shape =
        ::bioseq::chaining::vulkan_chaining_detail::
            analyze_gpu_chaining_shape(
                anchors, /*max_ref_dist=*/10u);
    REQUIRE(shape.range_count == 1u);
    REQUIRE(shape.compaction_required);
    REQUIRE(shape.device_anchor_count < anchors.size());

    context::context ctx;
    context::installed_context_scope installed{ctx};
    auto scope = ctx.push<context::arena::chain>();
    auto ranges = ::bioseq::chaining::vulkan_chaining_detail::
        build_gpu_chain_ranges(
            anchors, /*max_ref_dist=*/10u, shape.range_count);
    auto compacted = ::bioseq::chaining::make_arena_slab<
        ::bioseq::chaining::anchor>();
    REQUIRE(::bioseq::chaining::vulkan_chaining_detail::
        compact_long_gpu_chain_ranges(
            anchors, ranges, compacted, ranges));
    REQUIRE(compacted.size() == shape.device_anchor_count);
    REQUIRE(ranges.size() == shape.range_count);
    REQUIRE(ranges[0].begin == 0u);
    REQUIRE(ranges[0].length == shape.device_anchor_count);
    REQUIRE(compacted.data()[0].ref_position == anchors.front().ref_position);
    REQUIRE(compacted.data()[compacted.size() - 1u].ref_position
            == anchors.back().ref_position);
}

TEST_CASE("match_result biological fingerprints cover every result and CIGAR field",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    match_result result{
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
            cigar_operation{.length = 10u, .opcode = cigar_match_opcode},
            cigar_operation{.length = 2u, .opcode = cigar_insertion_opcode},
            cigar_operation{.length = 82u, .opcode = cigar_match_opcode},
        },
        .edit_distance = 2u,
    };
    match_result_collection lhs{std::vector<match_result>{result}};
    match_result_collection rhs{std::vector<match_result>{result}};

    const auto expected = lhs.biological_identity();
    REQUIRE(expected.is_computed);
    REQUIRE_FALSE(expected.is_saturated);
    REQUIRE(expected.collection_count == 1u);
    REQUIRE(expected.match_result_count == 1u);
    REQUIRE(expected.cigar_result_count == 1u);
    REQUIRE(expected.cigar_operation_count == 3u);
    REQUIRE(expected == rhs.biological_identity());

    enum class result_field : std::uint8_t {
        query_length,
        query_start,
        query_end,
        is_reverse,
        target_length,
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
    struct result_field_mutation {
        result_field field;
        const char* name;
        bool affects_cigar_digest;
    };
    constexpr std::array<result_field_mutation, 19u> result_mutations{{
        {result_field::query_length, "query_length", false},
        {result_field::query_start, "query_start", false},
        {result_field::query_end, "query_end", false},
        {result_field::is_reverse, "is_reverse", false},
        {result_field::target_length, "target_length", false},
        {result_field::target_start, "target_start", false},
        {result_field::target_end, "target_end", false},
        {result_field::num_matches, "num_matches", false},
        {result_field::block_length, "block_length", false},
        {result_field::mapq, "mapq", false},
        {result_field::chain_score, "chain_score", false},
        {result_field::dp_score, "dp_score", false},
        {result_field::dp_max_score, "dp_max_score", false},
        {result_field::seed_count, "seed_count", false},
        {result_field::secondary_chain_score,
         "secondary_chain_score", false},
        {result_field::divergence, "divergence", false},
        {result_field::is_primary, "is_primary", false},
        {result_field::has_cigar, "has_cigar", true},
        {result_field::edit_distance, "edit_distance", false},
    }};
    for (const auto& mutation : result_mutations) {
        auto mutated = result;
        switch (mutation.field) {
            case result_field::query_length: ++mutated.query_len; break;
            case result_field::query_start: ++mutated.query_start; break;
            case result_field::query_end: ++mutated.query_end; break;
            case result_field::is_reverse:
                mutated.is_reverse = !mutated.is_reverse;
                break;
            case result_field::target_length: ++mutated.target_len; break;
            case result_field::target_start: ++mutated.target_start; break;
            case result_field::target_end: ++mutated.target_end; break;
            case result_field::num_matches: ++mutated.num_matches; break;
            case result_field::block_length: ++mutated.block_length; break;
            case result_field::mapq: ++mutated.mapq; break;
            case result_field::chain_score: ++mutated.chain_score; break;
            case result_field::dp_score: ++mutated.dp_score; break;
            case result_field::dp_max_score: ++mutated.dp_max_score; break;
            case result_field::seed_count: ++mutated.seed_count; break;
            case result_field::secondary_chain_score:
                ++mutated.secondary_chain_score;
                break;
            case result_field::divergence:
                mutated.divergence = 0.25f;
                break;
            case result_field::is_primary:
                mutated.is_primary = !mutated.is_primary;
                break;
            case result_field::has_cigar:
                mutated.has_cigar = !mutated.has_cigar;
                break;
            case result_field::edit_distance:
                ++mutated.edit_distance;
                break;
        }
        INFO("mutated match_result field: " << mutation.name);
        rhs.results = {std::move(mutated)};
        const auto fingerprint = rhs.biological_identity();
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

    enum class cigar_field : std::uint8_t { length, opcode, vector_shape };
    struct cigar_field_mutation {
        cigar_field field;
        const char* name;
    };
    constexpr std::array<cigar_field_mutation, 3u> cigar_mutations{{
        {cigar_field::length, "cigar.length"},
        {cigar_field::opcode, "cigar.opcode"},
        {cigar_field::vector_shape, "cigar vector shape"},
    }};
    for (const auto& mutation : cigar_mutations) {
        auto mutated = result;
        switch (mutation.field) {
            case cigar_field::length:
                ++mutated.cigar[1].length;
                break;
            case cigar_field::opcode:
                mutated.cigar[1].opcode = cigar_deletion_opcode;
                break;
            case cigar_field::vector_shape:
                mutated.cigar.push_back(
                    cigar_operation{.length = 1u, .opcode = cigar_deletion_opcode});
                break;
        }
        INFO("mutated CIGAR field: " << mutation.name);
        rhs.results = {std::move(mutated)};
        const auto fingerprint = rhs.biological_identity();
        REQUIRE(fingerprint.match_digest_lane0
                != expected.match_digest_lane0);
        REQUIRE(fingerprint.match_digest_lane1
                != expected.match_digest_lane1);
        REQUIRE(fingerprint.cigar_digest_lane0
                != expected.cigar_digest_lane0);
        REQUIRE(fingerprint.cigar_digest_lane1
                != expected.cigar_digest_lane1);
    }

    match_result_collection empty_a;
    match_result_collection empty_b;
    const auto empty_fingerprint = empty_a.biological_identity();
    REQUIRE(empty_fingerprint.is_computed);
    REQUIRE_FALSE(empty_fingerprint.is_saturated);
    REQUIRE(empty_fingerprint.collection_count == 1u);
    REQUIRE(empty_fingerprint.match_result_count == 0u);
    REQUIRE(empty_fingerprint.cigar_result_count == 0u);
    REQUIRE(empty_fingerprint.cigar_operation_count == 0u);
    REQUIRE(empty_fingerprint == empty_b.biological_identity());

    auto second = result;
    ++second.query_start;
    ++second.cigar.front().length;
    match_result_collection ordered{
        std::vector<match_result>{result, second}};
    match_result_collection reversed{
        std::vector<match_result>{second, result}};
    const auto ordered_fingerprint = ordered.biological_identity();
    const auto reversed_fingerprint = reversed.biological_identity();
    REQUIRE(ordered_fingerprint.match_digest_lane0
            != reversed_fingerprint.match_digest_lane0);
    REQUIRE(ordered_fingerprint.match_digest_lane1
            != reversed_fingerprint.match_digest_lane1);
    REQUIRE(ordered_fingerprint.cigar_digest_lane0
            != reversed_fingerprint.cigar_digest_lane0);
    REQUIRE(ordered_fingerprint.cigar_digest_lane1
            != reversed_fingerprint.cigar_digest_lane1);

    match_result_set_identity aggregate{};
    aggregate.merge(expected);
    REQUIRE(aggregate == expected);
    aggregate.merge(match_result_set_identity{});
    REQUIRE_FALSE(aggregate.is_computed);
    REQUIRE(aggregate.is_saturated);
    aggregate.merge(expected);
    REQUIRE_FALSE(aggregate.is_computed);
    REQUIRE(aggregate.is_saturated);
}

}  // namespace bioseq::simd::pipe
