/**
 * @file pipeline/pipe_parts/minimizer_projection.cppm
 * @brief Connects alignment algorithms to stages and describes planned sketch work.
 */
module;

#include <new>

export module bioseq.pipeline.pipe:minimizer_projection;

import std;
export import bioseq.pipeline.query_sketch_plan_contract;
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

    // Each alignment trait supplies the kernel call and its report fields.
    // align_stage uses that common interface for the selected algorithm.

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
            // The generated table has checked that this alphabet entry carries
            // the matching C++ table type before we cast its pointer.
            resolved = static_cast<const ::bioseq::simd::alignment_kernels<p_alphabet>*>(
                entry.typed_table);
        }
        if (resolved == nullptr || !::bioseq::simd::is_alignment_kernel_table_complete(*resolved)) {
            throw std::logic_error{"selected Highway table has no complete typed alignment table"};
        }
        return *resolved;
    }

    /// Add the match, mismatch, and first affine-gap scores shared by the aligners.
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
                 scalar_pipeline_execution_stats* scalar_execution_stats,
                 bioseq::parallelism::worker_context_factory worker_factory = {}) {
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
                    scalar_execution_stats,
                    worker_factory);
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
                 scalar_pipeline_execution_stats* /*scalar_execution_stats*/,
                 bioseq::parallelism::worker_context_factory worker_factory = {}) {
                ::bioseq::simd::align_regions<
                    ::bioseq::simd::base_alignment_algorithm::block_aligner>(regions,
                                                                             anchors,
                                                                             query_seq,
                                                                             target_seq,
                                                                             scoring_matrix,
                                                                             alignment_kernels,
                                                                             params,
                                                                             block_execution_stats,
                                                                             nullptr,
                                                                             worker_factory);
            }

            [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
                return "Block Aligner";
            }

            static void populate_runtime_params(const ::bioseq::simd::minimap2_parameters& p,
                                                std::vector<named_property>& out) {
                detail::push_base_affine_params(p, out);
                out.push_back(
                    {"block_min_size", std::to_string(static_cast<std::int32_t>(p.block_min_size))});
                out.push_back(
                    {"block_max_size", std::to_string(static_cast<std::int32_t>(p.block_max_size))});
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

    // The batched driver comes from bioseq.alignment.ksw2_batched.
    template <> struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2_batched> {
        struct kernel_type {
            static constexpr bool emits_block_execution_stats = false;
            static constexpr bool emits_scalar_ksw2_execution_stats = false;
            static constexpr bool emits_ksw2_batched_execution_stats = true;
            static constexpr auto highway_kernel_id =
                ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2_batched;

            /// Report both the batched driver and the KSW2 kernel it uses for fills.
            /// EXPLAIN needs both entries to describe this stage's CPU dispatch.
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
                // Batched alignment uses the same scoring and stopping parameters as
                // KSW2. The driver groups fills by their geometry and orientation.
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

    /**
     * Describe query sketching separately because the described pipeline starts
     * with prebuilt minimizers. This extra stage records the chosen algorithm
     * and its Highway kernels for EXPLAIN without adding another template
     * parameter to every pipeline description.
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

    // Estimates used for initial capacities and pipeline descriptions.

    namespace detail {

    /** Return the Highway function set declared by the selected stage kernel. */
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
                                                               std::int32_t window_size) noexcept {
        if (window_size <= 0)
            return bp_size + 64u;
        return bp_size / static_cast<std::size_t>(window_size) + 64u;
    }

    /**
     * @brief Estimate an initial anchor capacity.
     *
     * Scale from the query sketch count and reduce the estimate for short
     * sequences. Repeated seed matches can exceed this estimate, so the slab
     * can grow as hits arrive.
     */
    [[nodiscard]] constexpr std::size_t estimated_anchor_count(std::size_t sketch_count,
                                                               std::size_t query_bp,
                                                               std::size_t target_bp,
                                                               std::int32_t kmer_size) noexcept {
        const std::size_t shared_ceiling =
            std::min(query_bp, target_bp) > static_cast<std::size_t>(kmer_size)
                ? std::min(query_bp, target_bp) - static_cast<std::size_t>(kmer_size) + 1u
                : sketch_count;
        const std::size_t median_estimate = sketch_count * 2u;
        return std::min(median_estimate, shared_ceiling);
    }

    [[nodiscard]] constexpr std::size_t estimated_chain_count(std::size_t anchor_count,
                                                              std::int32_t min_seed_count) noexcept {
        if (min_seed_count <= 0)
            return anchor_count;
        return anchor_count / static_cast<std::size_t>(min_seed_count) + 1u;
    }

    [[nodiscard]] constexpr std::size_t estimated_region_count(std::size_t chain_count,
                                                               std::int32_t best_n) noexcept {
        return std::min(chain_count, static_cast<std::size_t>(best_n) + 16u);
    }

    /**
     * @brief Estimate CIGAR storage from query and reference extents.
     *
     * Include any alignment extensions in the supplied extents. This helper
     * returns a size estimate and does not reserve memory.
     */
    [[nodiscard]] constexpr std::size_t estimated_cigar_op_count(std::int32_t query_extent,
                                                                 std::int32_t ref_extent) noexcept {
        const std::int32_t q = query_extent > 0 ? query_extent : 0;
        const std::int32_t r = ref_extent > 0 ? ref_extent : 0;
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

    // Score differences and the reusable scoring matrix.

    [[nodiscard]] constexpr std::int32_t
    sub_diff(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.match_score * 2 + params.mismatch_pen;
    }

    [[nodiscard]] constexpr std::int32_t
    min_diff(const ::bioseq::simd::minimap2_parameters& params) noexcept {
        return params.kmer_size * 2;
    }

    /**
     * @brief Reuse this thread's scoring matrix while its scores stay unchanged.
     *
     * Each alphabet has its own cache. Rebuild when the match, mismatch, or
     * ambiguous-base score changes after conversion to the matrix's byte type.
     */
    template <sequence_alphabet p_alphabet>
    [[nodiscard, gnu::always_inline]] inline const scoring_matrix<p_alphabet>&
    thread_local_scoring_matrix(const ::bioseq::simd::minimap2_parameters& params) {
        thread_local std::int8_t cached_match = std::numeric_limits<std::int8_t>::min();
        thread_local std::int8_t cached_mismatch = std::numeric_limits<std::int8_t>::min();
        thread_local std::int8_t cached_ambi = std::numeric_limits<std::int8_t>::min();
        thread_local scoring_matrix<p_alphabet> cached_matrix{};

        const std::int8_t want_match = static_cast<std::int8_t>(params.match_score);
        const std::int8_t want_mismatch = static_cast<std::int8_t>(params.mismatch_pen);
        const std::int8_t want_ambi = static_cast<std::int8_t>(params.sc_ambi);
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
