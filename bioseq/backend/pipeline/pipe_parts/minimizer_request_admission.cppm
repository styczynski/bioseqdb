/**
 * @file pipeline/pipe_parts/minimizer_request_admission.cppm
 * @brief Selects chaining kernels and sizgs target segments from the memory budget.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:minimizer_request_admission;

import std;
import :stage_graph;
import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.two_stack_minimizers;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.chaining.core;
import bioseq.chaining.scalar;
import bioseq.chaining.simd;
import bioseq.chaining.rmq;
import bioseq.chaining.vulkan;
import bioseq.pipeline.minimap_port;
import bioseq.pipeline.stats;
import bioseq.prefilter.types;
import bioseq.prefilter.sneaky_snake;
import bioseq.prefilter.sneaky_snake.vulkan;
import bioseq.prefilter.gatekeeper;
import bioseq.prefilter.gatekeeper.vulkan;
import bioseq.common.context;
import bioseq.instrumentation;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_index;
import bioseq.gpu.vulkan.runtime;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.indexing.gpu_minimizer_index;
import bioseq.seeding.vulkan;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
export import bioseq.pipeline.minimizer_execution_contract;

export {

    namespace bioseq::simd::pipe {

    // algorithm_traits maps an algorithm choice to the kernel interface used
    // by its stage. Each specialization also supplies names and parameters
    // for EXPLAIN. kernel_t selects that kernel type at compile, time

    /// Defer the missing-kernel assertion until an algorithm  is instantiated.
    template <auto p_algorithm> inline constexpr bool no_kernel_for_v = false;

    /// Reject algorithm choices with no trait specialization. Chaining traits
    /// are defined here, and sketch and alignment traits live in their modules.
    template <auto p_algorithm> struct algorithm_traits {
        static_assert(no_kernel_for_v<p_algorithm>,
                      "algorithm_traits<X>: no specialisation for this algorithm enum value. "
                      "Declare one in pipeline/pipe.cppm next to the chaining_algorithm / "
                      "minimizer_algorithm blocks.");
    };

    /// Select the kernel type for one algorithm.
    template <auto p_algorithm>
    using kernel_t = typename algorithm_traits<p_algorithm>::kernel_type;

    // Chaining kernels and their EXPLAIN properties.

    /// Run scalar dynamic-programming chaining and record completed anchor work.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::scalar_minimap2> {
        struct kernel_type {
            [[nodiscard]] static constexpr auto storage_requirements(
                std::size_t anchor_count, const chaining_parameters& params) noexcept {
                return ::bioseq::chaining::derive_scalar_chaining_storage_plan(anchor_count, params);
            }
            static constexpr bool emits_scalar_chaining_execution_stats = true;
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            [[gnu::always_inline]] inline static void
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out,
                 scalar_pipeline_execution_stats* stats) {
                ::bioseq::chaining::chain_anchors_scalar(
                    anchors, params, chains_out, reordered_anchors_out);
                if (stats != nullptr && !anchors.empty()) {
                    stats->record_scalar_chain(static_cast<std::uint64_t>(anchors.size()));
                }
            }
            [[nodiscard]] static constexpr chaining_algorithm
            completed_chaining_algorithm_identity() noexcept {
                return chaining_algorithm::scalar_minimap2;
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::scalar_minimap2);
            }
            static void populate_runtime_params(const chaining_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "false"},
                    {"complexity", "O(n * max_iter)"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Run SIMD chaining through the Highway table selected for this CPU.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::simd_minimap2> {
        struct kernel_type {
            [[nodiscard]] static constexpr auto storage_requirements(
                std::size_t anchor_count, const chaining_parameters& params) noexcept {
                return ::bioseq::chaining::derive_simd_chaining_storage_plan(anchor_count, params);
            }
            static constexpr bool emits_scalar_chaining_execution_stats = false;
            static constexpr bool emits_simd_chaining_execution_stats = true;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::chaining_chain_anchors_simd;
            [[gnu::always_inline]] inline static void
            call(const ::bioseq::simd_dispatch::highway_kernel_table* highway,
                 std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out,
                 ::bioseq::chaining::simd_chaining_execution_stats* stats) {
                if (highway == nullptr || !highway->has_kernel(highway_kernel_id)) {
                    throw simd_chaining_runtime_error(
                        simd_chaining_status::unsupported_simd_target,
                        "selected Highway table has no chaining kernel");
                }
                ::bioseq::chaining::chain_anchors_simd(highway->chaining.chain_anchors_simd,
                                                       anchors,
                                                       params,
                                                       chains_out,
                                                       reordered_anchors_out,
                                                       stats,
                                                       /*aggregate_stats=*/true);
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::simd_minimap2);
            }
            static void populate_runtime_params(const chaining_parameters& params,
                                                std::vector<named_property>& properties) {
                properties.push_back({"simd_chaining_policy",
                                      std::string{::bioseq::chaining::simd_chaining_policy_name(
                                          params.simd_policy)}});
            }
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "true"},
                    {"requires_zero_skip_penalty", "false"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Run tree-based range-minimum-query chaining.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::range_minimum_query> {
        struct kernel_type {
            [[nodiscard]] static constexpr auto storage_requirements(
                std::size_t anchor_count, const chaining_parameters& params) noexcept {
                return ::bioseq::chaining::derive_rmq_chaining_storage_plan(anchor_count, params);
            }
            // Record RMQ counters in benchmark and index-test builds. Ordinary
            // builds call this kernel without a statistics  destination.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            static constexpr bool emits_scalar_chaining_execution_stats = true;
#else
            static constexpr bool emits_scalar_chaining_execution_stats = false;
#endif
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            [[gnu::always_inline]] inline static void
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                 , scalar_pipeline_execution_stats* stats
#endif
                 ) {
                ::bioseq::chaining::chain_anchors_rmq(
                    anchors, params, chains_out, reordered_anchors_out);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
                if (stats != nullptr && !anchors.empty()) {
                    stats->record_scalar_chain(static_cast<std::uint64_t>(anchors.size()));
                }
#endif
            }
            [[nodiscard]] static constexpr chaining_algorithm
            completed_chaining_algorithm_identity() noexcept {
                return chaining_algorithm::range_minimum_query;
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::range_minimum_query);
            }
            static void populate_runtime_params(const chaining_parameters&,
                                                std::vector<named_property>&) noexcept {}
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "false"},
                    {"complexity", "O(n log n)"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    /// Score chain links on Vulkan, then backtrack the returned links on the CPU.
    /// A selected GPU operation throws if dispatch or validation fails. Empty
    /// inpu can finish without a GPU dispatch.
    template <>
    struct algorithm_traits<::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming> {
        struct kernel_type {
            static constexpr bool emits_scalar_chaining_execution_stats = false;
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = true;
            [[nodiscard, gnu::always_inline]] inline static bool
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out,
                 ::bioseq::parallelism::operation_memory_retention* attempt_retention = nullptr
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                 , ::bioseq::gpu::vulkan::runtime_pipeline_operation_token* completed_operation = nullptr
#endif
                 ) {
                return ::bioseq::chaining::chain_anchors_vulkan(
                    anchors, params, chains_out, reordered_anchors_out, attempt_retention
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
                    , completed_operation
#endif
                    );
            }
            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return ::bioseq::simd::chaining_algorithm_name(
                    ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming);
            }
            static void populate_runtime_params(const chaining_parameters& params,
                                                std::vector<named_property>& props) noexcept {
                props.push_back({"stage", "A"});
                props.push_back({"fallback", "none"});
                props.push_back({"uses_max_chain_skip", "false"});
                props.push_back({"vulkan_enabled", params.vulkan_enabled ? "true" : "false"});
            }
            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"accelerator", "vulkan"},
                    {"score_generation", "g3sa-tiled-dp"},
                    {"backtrack", "bioseq-cpu"},
                    {"uses_max_chain_skip", "false"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // A segment sketch stores local positions in a 28-bit field. Its core
    // and both overlaps must fit together, or packing would truncate a seed
    // position. The assertion below checks the configured CPU limits.
    inline constexpr sequence_size_t k_segment_core_max_bp = 200'000'000ull; // Maximum core length.
    inline constexpr sequence_size_t k_segment_overlap_max_bp =
        30'000'000ull; // Maximum overlap on either  side.
    inline constexpr sequence_size_t k_segment_min_bp =
        32'000'000ull; // Minimum CPU core length.
    inline constexpr sequence_size_t k_vulkan_segment_min_bp =
        1'000'000ull; // Minimum Vulkan core length.
    // Limit Vulkan's collected span as well as its core. Larger query
    // overlaps reduce the core so the whole uploaded segment still fits.
    inline constexpr sequence_size_t k_vulkan_segment_target_bp =
        76'000'000ull; // Preferred core size before allowing for query overlap
    inline constexpr sequence_size_t k_vulkan_segment_collected_max_bp = 96'000'000ull;
    static_assert(k_segment_core_max_bp + 2 * k_segment_overlap_max_bp <=
                      k_max_minimizer_sequence_length<minimap_sketch_data>,
                  "segment collected span (core + 2*overlap) must fit "
                  "minimap_sketch_data's 28-bit position field");

    // Estimate a target segment size from the current memory ceiling. The
    // peak-index bytes-per-base estimate keeps segments manageable, while
    // later reservations decide whether the actual allocations fit. Clamp
    // to the configured minimum and maximum  core sizes.
    [[nodiscard]] inline sequence_size_t segment_bp_from_budget() noexcept {
        // Use the same process/system ceiling and optional index cap as reservations.
        const std::uint64_t budget =
            bioseq::parallelism::memory_admission_controller::instance().budget_ceiling();
        // An unknown ceiling stays large enough to, select the maximum core size.
        const std::uint64_t by_budget =
            budget / bioseq::parallelism::k_minimizer_index_peak_bytes_per_bp;
        const sequence_size_t want = (by_budget > k_segment_core_max_bp)
                                         ? k_segment_core_max_bp
                                         : static_cast<sequence_size_t>(by_budget);
        return want < k_segment_min_bp ? k_segment_min_bp : want;
    }

    [[nodiscard]] inline sequence_size_t
    segment_bp_from_budget_divided(unsigned divisor,
                                   sequence_size_t floor_bp = k_segment_min_bp) noexcept {
        const sequence_size_t base = segment_bp_from_budget();
        const sequence_size_t divided = divisor <= 1 ? base : base / divisor;
        return divided < floor_bp ? floor_bp : divided;
    }

    [[nodiscard]] inline sequence_size_t segment_overlap_bp(sequence_size_t query_len) noexcept {
        sequence_size_t overlap_bp = query_len + query_len / 4 + 100'000;
        if (overlap_bp > k_segment_overlap_max_bp) {
            overlap_bp = k_segment_overlap_max_bp;
        }
        return overlap_bp;
    }

    [[nodiscard]] inline sequence_size_t
    vulkan_segment_target_bp(sequence_size_t query_len) noexcept {
        const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
        const sequence_size_t expanded_overlap = overlap_bp > k_vulkan_segment_collected_max_bp / 2
                                                     ? k_vulkan_segment_collected_max_bp
                                                     : overlap_bp * 2;
        if (expanded_overlap + k_vulkan_segment_min_bp >= k_vulkan_segment_collected_max_bp) {
            return k_vulkan_segment_min_bp;
        }
        const sequence_size_t room = k_vulkan_segment_collected_max_bp - expanded_overlap;
        const sequence_size_t target = std::min(room, k_vulkan_segment_target_bp);
        return target < k_vulkan_segment_min_bp ? k_vulkan_segment_min_bp : target;
    }

    [[nodiscard]] inline unsigned
    segment_geometry_divisor_for_backend(minimizer_sketch_backend backend,
                                         sequence_size_t query_len = 0) noexcept {
        if (backend != minimizer_sketch_backend::vulkan_segment) {
            return 1u;
        }
        const sequence_size_t base = segment_bp_from_budget();
        const sequence_size_t target = vulkan_segment_target_bp(query_len);
        const sequence_size_t divisor = (base + target - sequence_size_t{1}) / target;
        return static_cast<unsigned>(std::max<sequence_size_t>(1, divisor));
    }

    [[nodiscard]] inline sequence_size_t
    segment_core_floor_for_backend(minimizer_sketch_backend backend) noexcept {
        return backend == minimizer_sketch_backend::vulkan_segment ? k_vulkan_segment_min_bp
                                                                   : k_segment_min_bp;
    }

    // Build the vame segment ranges for recomputation and stored-index reads.
    // Geometry depends on the live memory budget as well as these arguments.
    // Each entry has an overlapping collection range in packed units and a
    // non-overlapping core range in bases.
    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
    segment_unit_ranges(sequence_size_t total_bp,
                        sequence_size_t query_len,
                        std::size_t lpu,
                        unsigned divisor,
                        sequence_size_t floor_bp) {
        std::vector<::bioseq::indexing::segment_unit_range> out;
        if (total_bp == 0 || lpu == 0)
            return out;
        const sequence_size_t total_units = (total_bp + lpu - 1) / lpu;
        const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
        const sequence_size_t ovl_units = (overlap_bp + lpu - 1) / lpu;
        sequence_size_t seg_units = segment_bp_from_budget_divided(divisor, floor_bp) / lpu;
        if (seg_units == 0)
            seg_units = 1;
        for (sequence_size_t lo_u = 0; lo_u < total_units; lo_u += seg_units) {
            const sequence_size_t hi_u = std::min(lo_u + seg_units, total_units);
            const sequence_size_t cs_u = (lo_u > ovl_units) ? (lo_u - ovl_units) : 0;
            const sequence_size_t ce_u = std::min(hi_u + ovl_units, total_units);
            const sequence_size_t core_lo_bp = lo_u * lpu;
            const sequence_size_t core_hi_bp = std::min(hi_u * lpu, total_bp);
            out.push_back(
                ::bioseq::indexing::segment_unit_range{cs_u, ce_u, core_lo_bp, core_hi_bp});
        }
        return out;
    }

    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu, unsigned divisor) {
        return segment_unit_ranges(total_bp, query_len, lpu, divisor, k_segment_min_bp);
    }

    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
    segment_unit_ranges(sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu) {
        return segment_unit_ranges(total_bp, query_len, lpu, 1);
    }

    [[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range>
    segment_unit_ranges_for_backend(sequence_size_t total_bp,
                                    sequence_size_t query_len,
                                    std::size_t lpu,
                                    minimizer_sketch_backend backend) {
        return segment_unit_ranges(total_bp,
                                   query_len,
                                   lpu,
                                   segment_geometry_divisor_for_backend(backend, query_len),
                                   segment_core_floor_for_backend(backend));
    }

    [[nodiscard]] inline std::pair<sequence_size_t, sequence_size_t>
    validated_segment_bp_bounds(const ::bioseq::indexing::segment_unit_range& range,
                                std::size_t letters_per_unit,
                                sequence_size_t total_bp) {
        const sequence_size_t lpu = static_cast<sequence_size_t>(letters_per_unit);
        if (letters_per_unit == 0u || static_cast<std::size_t>(lpu) != letters_per_unit) {
            throw std::invalid_argument("segment letters-per-unit is not representable");
        }
        constexpr sequence_size_t max_bp = (std::numeric_limits<sequence_size_t>::max)();
        if (range.cs_u > max_bp / lpu || range.ce_u > max_bp / lpu) {
            throw std::overflow_error("segment packed-unit bounds overflow biological coordinates");
        }
        const sequence_size_t lo_bp = range.cs_u * lpu;
        const sequence_size_t hi_bp = std::min<sequence_size_t>(range.ce_u * lpu, total_bp);
        if (lo_bp > hi_bp || lo_bp > total_bp) {
            throw std::invalid_argument(
                "segment packed-unit bounds exceed the biological sequence");
        }
        return {lo_bp, hi_bp};
    }

    } // namespace bioseq::simd::pipe

} // export
