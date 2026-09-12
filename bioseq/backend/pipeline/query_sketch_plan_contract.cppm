/**
 * @file
 * @brief Target-neutral query-sketch Highway planning used by execution and EXPLAIN.
 */
export module bioseq.pipeline.query_sketch_plan_contract;

import std;
import bioseq.highway.dispatch_contract;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.pipeline.minimap_parameters_contract;
import bioseq.pipeline.runtime_contract;

export namespace bioseq::simd::pipe {

/** Resolve the exact query-sketch Highway function set for one route. */
[[nodiscard]] constexpr std::optional<::bioseq::simd_dispatch::highway_kernel_mask>
planned_query_minimizer_highway_kernels(const ::bioseq::simd::minimap2_parameters& params,
                                        const query_sketch_route route) noexcept {
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using p_kernel = ::bioseq::simd_dispatch::highway_kernel;
    if (route != query_sketch_route::recompute_capable &&
        route != query_sketch_route::compatible_persisted_only) {
        return std::nullopt;
    }
    switch (params.minimizer_algorithm) {
    case algorithm_type::scalar:
    case algorithm_type::vulkan_minimap_integer:
        return ::bioseq::simd_dispatch::highway_kernel_mask{};
    case algorithm_type::simd_minimap2_reference:
        if (route == query_sketch_route::compatible_persisted_only) {
            return ::bioseq::simd_dispatch::highway_kernel_mask{};
        }
        return ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::minimizer_minimap2_reference);
    case algorithm_type::two_stack_nthash:
        return ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::minimizer_two_stack_nthash);
    case algorithm_type::bounded_nthash_simd:
        if (params.kmer_size <= 0 || params.window_size <= 0 ||
            static_cast<std::size_t>(params.kmer_size) >
                ::bioseq::minimizers::k_max_exact_two_stack_kmer ||
            static_cast<std::size_t>(params.window_size) >
                ::bioseq::minimizers::k_bounded_nthash_max_window) {
            return std::nullopt;
        }
        return ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::minimizer_bounded_nthash);
    }
    return std::nullopt;
}

static_assert(
    []() consteval {
        using algorithm_type = ::bioseq::simd::minimizer_algorithm;
        using p_kernel = ::bioseq::simd_dispatch::highway_kernel;
        using route_type = query_sketch_route;
        const auto make_params = [](const algorithm_type algorithm,
                                    const std::int32_t window_size = 10,
                                    const std::int32_t kmer_size = 15) consteval {
            ::bioseq::simd::minimap2_parameters params{};
            params.minimizer_algorithm = algorithm;
            params.window_size = window_size;
            params.kmer_size = kmer_size;
            return params;
        };
        const auto slow = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::simd_minimap2_reference),
            route_type::recompute_capable);
        const auto slow_persisted = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::simd_minimap2_reference),
            route_type::compatible_persisted_only);
        const auto exact = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::two_stack_nthash),
            route_type::compatible_persisted_only);
        const auto bounded = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::bounded_nthash_simd), route_type::recompute_capable);
        const auto bounded_generic_window = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::bounded_nthash_simd, 9, 15),
            route_type::recompute_capable);
        const auto bounded_k20 = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::bounded_nthash_simd, 15, 20),
            route_type::recompute_capable);
        const auto bounded_generic_kmer = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::bounded_nthash_simd, 10, 14),
            route_type::compatible_persisted_only);
        const auto scalar = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::scalar), route_type::recompute_capable);
        const auto vulkan = planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::vulkan_minimap_integer),
            route_type::compatible_persisted_only);
        return slow.has_value() &&
               *slow == ::bioseq::simd_dispatch::highway_kernel_mask_for(
                            p_kernel::minimizer_minimap2_reference) &&
               slow_persisted.has_value() && slow_persisted->none() && exact.has_value() &&
               *exact == ::bioseq::simd_dispatch::highway_kernel_mask_for(
                             p_kernel::minimizer_two_stack_nthash) &&
               bounded.has_value() &&
               *bounded == ::bioseq::simd_dispatch::highway_kernel_mask_for(
                               p_kernel::minimizer_bounded_nthash) &&
               bounded_k20.has_value() && *bounded_k20 == *bounded &&
               bounded_generic_window.has_value() && *bounded_generic_window == *bounded &&
               bounded_generic_kmer.has_value() && *bounded_generic_kmer == *bounded &&
               !planned_query_minimizer_highway_kernels(
                    make_params(algorithm_type::bounded_nthash_simd, 0, 15),
                    route_type::recompute_capable).has_value() &&
               !planned_query_minimizer_highway_kernels(
                    make_params(algorithm_type::bounded_nthash_simd, 10, 0),
                    route_type::compatible_persisted_only).has_value() &&
               scalar.has_value() && scalar->none() && vulkan.has_value() && vulkan->none() &&
               !planned_query_minimizer_highway_kernels(
                    make_params(algorithm_type::simd_minimap2_reference),
                    static_cast<route_type>(0xffu))
                    .has_value();
    }(),
    "query-sketch Highway planning must remain route- and algorithm-exact");

/** Number of semantic query-sketch owners in a structured description. */
[[nodiscard]] inline std::size_t
query_sketch_stage_owner_count(const pipeline_description& description) noexcept {
    return static_cast<std::size_t>(
        std::ranges::count_if(description.stages, [](const stage_description& stage) noexcept {
            return stage.stage_name == query_sketch_stage_name && stage.algorithm.has_value();
        }));
}

} // namespace bioseq::simd::pipe
