/**
 * @file pipeline/pipe_parts/minimizer_projection.cppm
 * @brief Projects algorithm choices, receipts, and stage statistics.
 */
export module bioseq.pipeline.pipe:minimizer_projection;

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
import :minimizer_request_admission;
import :minimizer_scalar_highway;
import :minimizer_vulkan;

export {

    namespace bioseq::simd::pipe {

    // ---------------------------------------------------------------------------
    // base_alignment_algorithm kernel traits
    // ---------------------------------------------------------------------------
    //
    // The trait pattern is shared across every algorithm dimension.
    // `align_stage<p_kernel, p_alphabet>` calls `p_kernel::call<p_alphabet>(...)`
    // once; per-algorithm dispatch lives entirely in
    // the trait specialisations below.

    namespace detail {

    template <sequence_alphabet p_alphabet>
    [[nodiscard]] const ::bioseq::simd::alignment_kernels<p_alphabet>&
    resolve_alignment_kernels(const ::bioseq::simd::minimap2_parameters& params) {
        const auto* root = params.highway_kernels;
        const auto* identity = root == nullptr || !root->is_complete()
                                   ? nullptr
                                   : root->template resolve_alphabet_identity<p_alphabet>();
        if (identity == nullptr || params.alphabet_code != identity->alphabet_code ||
            root->resolve_alphabet_identity(params.alphabet_code) != identity) {
            throw std::logic_error{"alignment dispatch requires an alphabet-matched Highway table"};
        }

        auto required = ::bioseq::simd_dispatch::highway_kernel_mask_for(
            ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2);
        required |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
            ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched);
        required |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
            ::bioseq::simd_dispatch::highway_kernel::alignment_block_aligner);
        if (!root->has_all_kernels(required)) {
            throw std::logic_error{
                "selected Highway table has is_incomplete alignment availability"};
        }

        const ::bioseq::simd::alignment_kernels<p_alphabet>* resolved = nullptr;
        for (const auto& entry : root->alignment_by_alphabet) {
            if (entry.alphabet_code != identity->alphabet_code)
                continue;
            if (entry.typed_table == nullptr || resolved != nullptr) {
                throw std::logic_error{
                    "selected Highway table has an invalid alignment alphabet entry"};
            }
            // The generated root reconciles this numeric slot with the concrete
            // type identity before the erased pointer can be cast.
            resolved = static_cast<const ::bioseq::simd::alignment_kernels<p_alphabet>*>(
                entry.typed_table);
        }
        if (resolved == nullptr || !::bioseq::simd::is_alignment_kernel_table_complete(*resolved)) {
            throw std::logic_error{"selected Highway table has no complete typed alignment table"};
        }
        return *resolved;
    }

    /// Shared helper for ksw2 / Block Aligner / ksw2_batched — pushes the
    /// four affine-scoring parameters that every base aligner consumes.
    [[gnu::always_inline]] inline void
    push_base_affine_params(const ::bioseq::simd::minimap2_parameters& p,
                            std::vector<named_property>& out) {
        out.push_back({"match_score", std::to_string(p.match_score)});
        out.push_back({"mismatch_pen", std::to_string(p.mismatch_pen)});
        out.push_back({"gap_open1", std::to_string(p.gap_open1)});
        out.push_back({"gap_ext1", std::to_string(p.gap_ext1)});
    }

    } // namespace detail

    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = false;
            static constexpr bool emits_scalar_ksw2_execution_stats = true;
            static constexpr bool emits_ksw2_batched_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2;

            template <sequence_alphabet p_alphabet>
            [[gnu::always_inline]] static void
            call(std::span<alignment_region> regions,
                 std::span<const ::bioseq::chaining::anchor> anchors,
                 bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const scoring_matrix<p_alphabet>& scoring_matrix,
                 const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                 const ::bioseq::simd::minimap2_parameters& params,
                 block_aligner_execution_stats* forbidden_block_stats,
                 scalar_pipeline_execution_stats* scalar_execution_stats) {
                if (forbidden_block_stats != nullptr) {
                    abort_forbidden_ksw2_fallback(*forbidden_block_stats);
                }
                ::bioseq::simd::align_regions<::bioseq::simd::base_alignment_algorithm::ksw2>(
                    regions,
                    anchors,
                    query_seq,
                    target_seq,
                    scoring_matrix,
                    alignment_kernels,
                    params,
                    /*block_execution_stats=*/nullptr,
                    scalar_execution_stats);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "ksw2";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                detail::push_base_affine_params(p, out);
                out.push_back({"gap_open2", std::to_string(p.gap_open2)});
                out.push_back({"gap_ext2", std::to_string(p.gap_ext2)});
                out.push_back({"zdrop", std::to_string(p.zdrop)});
                out.push_back({"end_bonus", std::to_string(p.end_bonus)});
            }

            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "intra-sequence striped"},
                    {"single_affine_eq", "data-dependent"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::block_aligner> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = true;
            static constexpr bool emits_scalar_ksw2_execution_stats = false;
            static constexpr bool emits_ksw2_batched_execution_stats = false;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_block_aligner;

            template <sequence_alphabet p_alphabet>
            [[gnu::always_inline]] static void
            call(std::span<alignment_region> regions,
                 std::span<const ::bioseq::chaining::anchor> anchors,
                 bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const scoring_matrix<p_alphabet>& scoring_matrix,
                 const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                 const ::bioseq::simd::minimap2_parameters& params,
                 block_aligner_execution_stats* block_execution_stats,
                 scalar_pipeline_execution_stats* /*scalar_execution_stats*/) {
                ::bioseq::simd::align_regions<
                    ::bioseq::simd::base_alignment_algorithm::block_aligner>(regions,
                                                                             anchors,
                                                                             query_seq,
                                                                             target_seq,
                                                                             scoring_matrix,
                                                                             alignment_kernels,
                                                                             params,
                                                                             block_execution_stats);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "Block Aligner";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                detail::push_base_affine_params(p, out);
                out.push_back(
                    {"block_min_size", std::to_string(static_cast<int32_t>(p.block_min_size))});
                out.push_back(
                    {"block_max_size", std::to_string(static_cast<int32_t>(p.block_max_size))});
            }

            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "block-affine"},
                    {"single_affine_only", "true"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // The ksw2_batched kernel is provided by `bioseq.alignment.ksw2_batched`.
    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2_batched> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = false;
            static constexpr bool emits_scalar_ksw2_execution_stats = false;
            static constexpr bool emits_ksw2_batched_execution_stats = true;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched;

            /// The batched entry owns inter-region packing, but its long-fill and
            /// traceback paths execute the target-local ksw2 z2/d2 kernels as part
            /// of the same stage.  EXPLAIN therefore reports both coarse functions.
            [[nodiscard]] static constexpr ::bioseq::simd_dispatch::highway_kernel_mask
            highway_kernel_ids() noexcept {
                auto kernels = ::bioseq::simd_dispatch::highway_kernel_mask_for(
                    ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched);
                kernels |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
                    ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2);
                return kernels;
            }

            template <sequence_alphabet p_alphabet>
            [[gnu::always_inline]] static void
            call(std::span<alignment_region> regions,
                 std::span<const ::bioseq::chaining::anchor> anchors,
                 bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
                 bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
                 const scoring_matrix<p_alphabet>& scoring_matrix,
                 const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
                 const ::bioseq::simd::minimap2_parameters& params,
                 block_aligner_execution_stats* forbidden_block_stats,
                 scalar_pipeline_execution_stats* /*scalar_execution_stats*/,
                 ksw2_batched_execution_stats* execution_stats = nullptr) {
                if (forbidden_block_stats != nullptr) {
                    abort_forbidden_ksw2_fallback(*forbidden_block_stats);
                }
                alignment_kernels.align_ksw2_batched(regions,
                                                     anchors,
                                                     query_seq,
                                                     target_seq,
                                                     scoring_matrix,
                                                     params,
                                                     execution_stats);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "ksw2 (inter-region batched)";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                // Same runtime knobs as the intra-sequence ksw2 path; the
                // bucketing/batching is determined by the kernel from
                // params.max_gap, params.zdrop, and the per-fill geometry.
                algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2>::kernel_type::
                    populate_runtime_params(p, out);
            }

            template <is_constexpr_aligner_params p_aligner_parameters>
            [[nodiscard]] static std::vector<named_property> static_properties() {
                return {
                    {"vectorised", "inter-sequence packed"},
                    {"bucket_widths", "[<=32, <=64, <=128, <=256, >256]"},
                    {"long_fill_kernel", "intra-sequence ksw2 (fallback)"},
                    {"specialised",
                     p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
                };
            }
        };
    };

    // =============================================================================
    // Metadata surface — full definitions
    // =============================================================================
    //
    // Full description types follow the algorithm traits that provide their
    // computed properties.

    struct stage_algorithm_description {
        std::string name;
        std::vector<named_property> parameters;
        std::vector<named_property> computed_properties;
        std::optional<std::string> extra_description;
        ::bioseq::simd_dispatch::highway_kernel_mask highway_kernels{};
    };

    struct stage_description {
        std::string stage_name;
        std::optional<stage_algorithm_description> algorithm;
        std::optional<std::string> extra_description;

        /**
         * @brief Pre-allocation upper bound the stage reserves before driving its
         * kernel ( — "reserve correctness").
         *
         * Populated by every stage that opens an `arena_slab<p_element>` of bounded size
         * (minimizer_stage, seed_hit_stage, chain_stage, region_stage).  Set to
         * `std::nullopt` for stages that mutate in-place or whose output size
         * is `0` (index_stage, sink_stage, etc.).  The EXPLAIN renderer surfaces
         * this as a per-stage `[reserved: N]` line so a reader can verify
         * `actual ≤ reserved` against the runtime instrumentation log.
         */
        std::optional<std::size_t> estimated_batch_size;
    };

    struct pipeline_description {
        std::vector<stage_description> stages;
    };

    inline constexpr std::string_view query_sketch_stage_name = "QuerySketchStage";

    /** Cold EXPLAIN-only query-sketch route. Never a dispatch-chain dimension. */
    enum class query_sketch_route : std::uint8_t {
        recompute_capable,
        compatible_persisted_only,
    };

    /**
     * Resolve the query-sketch Highway function without adding the minimizer
     * algorithm as another describe-template dimension.  scalar and Vulkan
     * sketching are concrete non-Highway routes and therefore map to an empty
     * set. p_alphabet validity is established by the caller's authoritative typed
     * dispatch before this code-independent algorithm/route planner runs.
     * Compatible-persisted query preparation performs no Highway work. bounded ntHash
     * ntHash retains its W10/K15 gate. Unknown route or algorithm values remain
     * fail-closed rather than being silently advertised as scalar.
     */
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
            if (params.window_size != 10 || params.kmer_size != 15) {
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
            const auto bounded_wrong_window = planned_query_minimizer_highway_kernels(
                make_params(algorithm_type::bounded_nthash_simd, 9, 15),
                route_type::recompute_capable);
            const auto bounded_wrong_kmer = planned_query_minimizer_highway_kernels(
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
                   !bounded_wrong_window.has_value() && !bounded_wrong_kmer.has_value() &&
                   scalar.has_value() && scalar->none() && vulkan.has_value() && vulkan->none() &&
                   !planned_query_minimizer_highway_kernels(
                        make_params(algorithm_type::simd_minimap2_reference),
                        static_cast<route_type>(0xffu))
                        .has_value();
        }(),
        "query-sketch Highway planning must remain route- and algorithm-exact");

    /**
     * The describe cascade intentionally pins the executable pipeline at the
     * prebuilt-minimizer entry point, so its stage chain has no minimizer_stage.
     * Represent the query sketch as a neutral plan-only stage instead of making
     * the minimizer enum a describe-template axis. This stage is the canonical
     * owner of the query-sketch Highway functions whenever a description exists.
     */
    [[nodiscard]] inline std::optional<stage_description>
    describe_query_sketch_stage(const ::bioseq::simd::minimap2_parameters& params,
                                const query_sketch_route route) {
        const auto kernels = planned_query_minimizer_highway_kernels(params, route);
        if (!kernels.has_value())
            return std::nullopt;

        stage_description stage;
        stage.stage_name = std::string{query_sketch_stage_name};
        stage_algorithm_description algorithm;
        algorithm.name = ::bioseq::simd::minimizer_algorithm_name(params.minimizer_algorithm);
        algorithm.highway_kernels = *kernels;
        stage.algorithm = std::move(algorithm);
        return stage;
    }

    /** Number of semantic query-sketch owners in a structured description. */
    [[nodiscard]] inline std::size_t
    query_sketch_stage_owner_count(const pipeline_description& description) noexcept {
        return static_cast<std::size_t>(
            std::ranges::count_if(description.stages, [](const stage_description& stage) noexcept {
                return stage.stage_name == query_sketch_stage_name && stage.algorithm.has_value();
            }));
    }

    // =============================================================================
    // Output size estimates
    // =============================================================================

    namespace detail {

    /** Exact coarse Highway functions executed by one selected stage kernel. */
    template <class p_kernel>
    [[nodiscard]] constexpr ::bioseq::simd_dispatch::highway_kernel_mask
    stage_highway_kernels() noexcept {
        if constexpr (requires { p_kernel::highway_kernel_ids(); }) {
            return p_kernel::highway_kernel_ids();
        } else if constexpr (requires { p_kernel::highway_kernel_id; }) {
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(p_kernel::highway_kernel_id);
        } else {
            return {};
        }
    }

    [[nodiscard]] constexpr std::size_t estimated_sketch_count(std::size_t bp_size,
                                                               int32_t window_size) noexcept {
        if (window_size <= 0)
            return bp_size + 64u;
        return bp_size / static_cast<std::size_t>(window_size) + 64u;
    }

    /**
     * @brief Reserve-time anchor estimate.
     *
     * Uses today's median heuristic (2× sketch_count) but clamps by the
     * physical shared-k-mer ceiling so very-short query/target pairs don't
     * over-allocate.  The slab grows geometrically when the kernel produces
     * more anchors than estimated.
     */
    [[nodiscard]] constexpr std::size_t estimated_anchor_count(std::size_t sketch_count,
                                                               std::size_t query_bp,
                                                               std::size_t target_bp,
                                                               int32_t kmer_size) noexcept {
        const std::size_t shared_ceiling =
            std::min(query_bp, target_bp) > static_cast<std::size_t>(kmer_size)
                ? std::min(query_bp, target_bp) - static_cast<std::size_t>(kmer_size) + 1u
                : sketch_count;
        const std::size_t median_estimate = sketch_count * 2u;
        return std::min(median_estimate, shared_ceiling);
    }

    [[nodiscard]] constexpr std::size_t estimated_chain_count(std::size_t anchor_count,
                                                              int32_t min_seed_count) noexcept {
        if (min_seed_count <= 0)
            return anchor_count;
        return anchor_count / static_cast<std::size_t>(min_seed_count) + 1u;
    }

    [[nodiscard]] constexpr std::size_t estimated_region_count(std::size_t chain_count,
                                                               int32_t best_n) noexcept {
        return std::min(chain_count, static_cast<std::size_t>(best_n) + 16u);
    }

    /**
     * @brief Upper-bound estimate of the per-region CIGAR op count.
     *
     * unifies the helper signature so it can model the post-fill
     * outer extents instead of `region.block_length` (which excludes the
     * left/right kernel extensions).  Describe-time callers pass
     * `(block_length, block_length)` as a coarse approximation — the value is
     * only a render hint, never an actual reservation.
     */
    [[nodiscard]] constexpr std::size_t estimated_cigar_op_count(int32_t query_extent,
                                                                 int32_t ref_extent) noexcept {
        const int32_t q = query_extent > 0 ? query_extent : 0;
        const int32_t r = ref_extent > 0 ? ref_extent : 0;
        return static_cast<std::size_t>(q) + static_cast<std::size_t>(r) + 16u;
    }

    [[nodiscard]] constexpr std::size_t
    estimated_result_count(sequence_size_t query_len,
                           sequence_size_t target_len,
                           const ::bioseq::simd::minimap2_parameters& params) noexcept {
        const std::size_t bp_floor = std::max<std::size_t>(
            1u, static_cast<std::size_t>(std::min(query_len, target_len)) / 500u);
        return std::max(static_cast<std::size_t>(params.best_n + 4), bp_floor);
    }

    } // namespace detail

    // =============================================================================
    // Derived helpers
    // =============================================================================

    [[nodiscard]] constexpr int32_t
    sub_diff(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.match_score * 2 + params.mismatch_pen;
    }

    [[nodiscard]] constexpr int32_t
    min_diff(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.kmer_size * 2;
    }

    /**
     * @brief Thread-local cached scoring matrix keyed on (match, mismatch, ambi).
     *
     * Shared by the single-query and batch pipeline entry points.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard, gnu::always_inline]] inline const scoring_matrix<p_alphabet>&
    thread_local_scoring_matrix(const ::bioseq::simd::minimap2_parameters& params) {
        thread_local int8_t cached_match = std::numeric_limits<int8_t>::min();
        thread_local int8_t cached_mismatch = std::numeric_limits<int8_t>::min();
        thread_local int8_t cached_ambi = std::numeric_limits<int8_t>::min();
        thread_local scoring_matrix<p_alphabet> cached_matrix{};

        const int8_t want_match = static_cast<int8_t>(params.match_score);
        const int8_t want_mismatch = static_cast<int8_t>(params.mismatch_pen);
        const int8_t want_ambi = static_cast<int8_t>(params.sc_ambi);
        if (cached_match != want_match || cached_mismatch != want_mismatch ||
            cached_ambi != want_ambi) {
            cached_matrix =
                scoring_matrix<p_alphabet>::generate(want_match, want_mismatch, want_ambi);
            cached_match = want_match;
            cached_mismatch = want_mismatch;
            cached_ambi = want_ambi;
        }
        return cached_matrix;
    }

    } // namespace bioseq::simd::pipe

} // export
