/**
 * @file error_admission_test.cpp
 * @brief Exercises pipeline admission and ownership errors.
 */
#include <catch2/matchers/catch_matchers.hpp>

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.tests.unit.pipeline.pipe_support;

using namespace bioseq::tests::pipeline::pipe_support;

TEST_CASE("authorized Vulkan query batch moves storage before readback admission",
          "[component:bioseq][layer:unit][feature:pipeline]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t baseline = admission.reserved_bytes();
    using owned_type = bioseq::gpu::vulkan::
        vulkan_batch_sketch_result<budgeted_sketch_lifetime_probe>;
    STATIC_REQUIRE(!std::is_copy_constructible_v<owned_type>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<owned_type>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<owned_type>);

    const auto make_owned = [&admission](bool& observed) {
        constexpr std::size_t count = 4u;
        auto permit = admission.try_reserve(
            count * sizeof(budgeted_sketch_lifetime_probe) *
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        REQUIRE(permit.has_value());
        owned_type owned;
        owned.memory_retention =
            std::make_shared<bioseq::gpu::vulkan::vulkan_batch_sketch_memory_retention>();
        owned.host_readback_permit = std::move(*permit);
        owned.host_minimizers.reserve(count);
        const auto exact = owned.host_minimizers.capacity() *
                           sizeof(budgeted_sketch_lifetime_probe);
        REQUIRE(exact <= owned.host_readback_permit.bytes());
        owned.host_readback_permit.shrink_to(exact);
        owned.host_minimizers.emplace_back(
            &admission, admission.reserved_bytes(), &observed);
        return owned;
    };

    bool source_destroyed_with_permit = false;
    bool destination_destroyed_with_permit = false;
    std::weak_ptr<bioseq::gpu::vulkan::vulkan_batch_sketch_memory_retention> lifetime;
    {
        owned_type source = make_owned(source_destroyed_with_permit);
        const auto* original_data = source.host_minimizers.data();
        const auto exact = source.host_readback_permit.bytes();
        lifetime = source.memory_retention;
        owned_type destination = make_owned(destination_destroyed_with_permit);
        destination = std::move(source);
        REQUIRE(destination_destroyed_with_permit);
        REQUIRE_FALSE(source_destroyed_with_permit);
        REQUIRE(source.host_minimizers.capacity() == 0u);
        REQUIRE_FALSE(source.host_readback_permit);
        REQUIRE(destination.host_minimizers.data() == original_data);
        REQUIRE(admission.reserved_bytes() == baseline + exact);
        owned_type moved{std::move(destination)};
        REQUIRE(moved.host_minimizers.data() == original_data);
        REQUIRE_FALSE(destination.host_readback_permit);
        auto* self = std::addressof(moved);
        moved = std::move(*self);
        REQUIRE(moved.host_minimizers.data() == original_data);
        REQUIRE_FALSE(lifetime.expired());
    }
    REQUIRE(source_destroyed_with_permit);
    REQUIRE(lifetime.expired());
    REQUIRE(admission.reserved_bytes() == baseline);

    bool unwind_destroyed_with_permit = false;
    REQUIRE_THROWS_AS(([&make_owned, &unwind_destroyed_with_permit] {
        owned_type owned = make_owned(unwind_destroyed_with_permit);
        throw std::runtime_error{"exercise ordinary owner unwind"};
    }()), std::runtime_error);
    REQUIRE(unwind_destroyed_with_permit);
    REQUIRE(admission.reserved_bytes() == baseline);
}

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
    const auto make_owned = [&admission, baseline](bool& observed) {
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

    params.kmer_size = 0;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.kmer_size = 27;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.kmer_size = 15;

    params.window_size = 0;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.window_size = 257;
    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 4u, minimum));
    params.window_size = 10;

    REQUIRE_FALSE(fixed_vulkan_target_host_adoption_route_selected<
        source_direct_minimizer_fixture>(false, params, 257u, minimum));
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
