/**
 * @file pipeline/pipe_parts/minimizer_request_admission.cppm
 * @brief Defines minimizer requests, admission rules, and segment geometry.
 */
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

export {

    namespace bioseq::simd::pipe {

    // =============================================================================
    // Kernel-trait pattern
    // =============================================================================
    //
    // `algorithm_traits<p_algorithm>` maps an algorithm enum value to
    //   - a `kernel_type` struct exposing the call surface used by the stage,
    //   - a `static constexpr std::string_view algorithm_label()` for EXPLAIN,
    //   - a `static std::vector<named_property> computed_properties_default()`
    //     for the EXPLAIN renderer's `[name: value]` properties.
    //
    // Stages take the `kernel_type` as a template argument (via `kernel_t<X>`)
    // and call `p_kernel::call(...)` once — no `if constexpr` ladders inside the
    // stage body.  Adding a new algorithm is a single trait specialisation in
    // the kernel's own module.

    /// Dependent-false helper so the primary template's `static_assert` fires
    /// only on instantiation (not at point of definition).
    template <auto p_algorithm> inline constexpr bool no_kernel_for_v = false;

    /// Primary template — unspecialised.  Substitution failure here produces a
    /// readable diagnostic naming the offending enum value.
    ///
    /// Specializations below cover the chaining, minimizer, and base-alignment
    /// algorithm dimensions.
    template <auto p_algorithm> struct algorithm_traits {
        static_assert(no_kernel_for_v<p_algorithm>,
                      "algorithm_traits<X>: no specialisation for this algorithm enum value. "
                      "Declare one in pipeline/pipe.cppm next to the chaining_algorithm / "
                      "minimizer_algorithm blocks.");
    };

    /// Helper alias — `kernel_t<X>` gives the kernel type directly.
    template <auto p_algorithm>
    using kernel_t = typename algorithm_traits<p_algorithm>::kernel_type;

    // ---------------------------------------------------------------------------
    // chaining_algorithm kernel traits
    // ---------------------------------------------------------------------------

    /// scalar DP chaining — `chain_anchors_scalar` from
    /// `bioseq.chaining.scalar`.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::scalar_minimap2> {
        struct kernel_type {
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

    /// simd DP chaining — `chain_anchors_simd` from
    /// the selected fixed-target chaining table.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::simd_minimap2> {
        struct kernel_type {
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

    /// range_minimum_query tree-based chaining — `chain_anchors_rmq` from
    /// `bioseq.chaining.rmq`.
    template <> struct algorithm_traits<::bioseq::simd::chaining_algorithm::range_minimum_query> {
        struct kernel_type {
            static constexpr bool emits_scalar_chaining_execution_stats = false;
            static constexpr bool emits_simd_chaining_execution_stats = false;
            static constexpr bool emits_partial_vulkan_execution_stats = false;
            [[gnu::always_inline]] inline static void
            call(std::span<const anchor> anchors,
                 const chaining_parameters& params,
                 arena_slab<chain>& chains_out,
                 arena_slab<anchor>& reordered_anchors_out) {
                ::bioseq::chaining::chain_anchors_rmq(
                    anchors, params, chains_out, reordered_anchors_out);
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

    /// Vulkan tiled-DP chaining — `chain_anchors_vulkan` from
    /// `bioseq.chaining.vulkan`. G3SA-style tiled DP score generation on
    /// Vulkan + bioseq CPU `backtrack_chains`. Selected Vulkan execution is
    /// mandatory and failures throw `vulkan_runtime_error`.
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
                 arena_slab<anchor>& reordered_anchors_out) {
                return ::bioseq::chaining::chain_anchors_vulkan(
                    anchors, params, chains_out, reordered_anchors_out);
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

    struct minimizer_range_execution_stats {
        scalar_minimizer_execution_stats scalar{};
        bool scalar_available = false;
        two_stack_execution_stats exact_simd{};
        bool exact_simd_available = false;
        bounded_nthash_execution_stats bounded_nthash{};
        bool bounded_nthash_available = false;
    };

    /// exact_simd has two independently valid, but biologically incompatible,
    /// minimizer-selection routes for the p_window_size=10/p_kmer_size=15 dna4/u32 shape:
    ///
    /// * the resident vector_stream route.
    /// * the chronological continuation/B8 route used by persisted and fragmented
    ///   exact_simd index producers.
    ///
    /// A query cannot select between them from its own storage shape.  Indexed
    /// consumers must therefore require the continuation route explicitly. Only a
    /// caller that owns both full-resident sides of one transient/no-index
    /// operation may opt into the resident route.
    enum class exact_simd_range_route : std::uint8_t {
        require_continuation,
        require_resident_vector_stream,
    };

    /// Preserve native-width topology by default. Persisted indexed-query
    /// preparation is the narrow exception: its continuation/B8 storage
    /// contract requires an exact u32 input packing.
    enum class exact_simd_input_storage : std::uint8_t {
        preserve_native_width,
        require_indexed_u32,
    };

    [[nodiscard]] inline bool
    exact_simd_stats_matches_range_route(exact_simd_range_route route,
                                         const two_stack_execution_stats& stats) noexcept {
        if (route == exact_simd_range_route::require_resident_vector_stream) {
            return stats.resident_vector_stream_kernel_calls > 0u;
        }
        // Check every resident-only field, rather than only the call count, so a
        // partially-populated or mixed stats also fails closed.
        return stats.resident_vector_stream_kernel_calls == 0u &&
               stats.resident_vector_stream_single_calls == 0u &&
               stats.resident_vector_stream_dual_calls == 0u &&
               stats.resident_vector_stream_nonempty_work_calls == 0u &&
               stats.resident_vector_stream_input_bp == 0u &&
               stats.resident_vector_stream_minimizers_emitted == 0u;
    }

    /// Compare the biological selection sub-route actually attested by two
    /// exact_simd operations. This is required even when both operations carry the
    /// same high-level exact_simd backend label: resident vector_stream and
    /// continuation/B8 stats are not lookup-compatible.
    [[nodiscard]] inline bool
    exact_simd_stats_share_selection_route(const two_stack_execution_stats& lhs,
                                           const two_stack_execution_stats& rhs) noexcept {
        if (!lhs.is_internally_consistent(/*require_nonempty_vector_work=*/false) ||
            !rhs.is_internally_consistent(/*require_nonempty_vector_work=*/false)) {
            return false;
        }
        const bool lhs_resident = lhs.resident_vector_stream_kernel_calls > 0u;
        const bool rhs_resident = rhs.resident_vector_stream_kernel_calls > 0u;
        return lhs_resident == rhs_resident;
    }

    /// Validate the complete range-stats payload against the selected CPU sketch
    /// backend.  Presence bits are not sufficient validation on their own: a
    /// non-default stats object with a cleared bit is rejected as hidden
    /// cross-route stats object. Vulkan is deliberately not accepted here because
    /// this stats type carries only CPU/simd operation stats.
    [[nodiscard]] inline bool minimizer_range_stats_matches_backend(
        minimizer_sketch_backend backend,
        const minimizer_range_execution_stats& stats,
        bool require_nonempty_vector_work = false,
        exact_simd_range_route exact_simd_route =
            exact_simd_range_route::require_continuation) noexcept {
        const bool scalar_absent =
            !stats.scalar_available && stats.scalar == scalar_minimizer_execution_stats{};
        const bool exact_absent =
            !stats.exact_simd_available && stats.exact_simd == two_stack_execution_stats{};
        const bool bounded_absent = !stats.bounded_nthash_available &&
                                    stats.bounded_nthash == bounded_nthash_execution_stats{};

        switch (backend) {
        case minimizer_sketch_backend::exact_simd:
            return scalar_absent && bounded_absent && stats.exact_simd_available &&
                   stats.exact_simd.is_internally_consistent(require_nonempty_vector_work) &&
                   exact_simd_stats_matches_range_route(exact_simd_route, stats.exact_simd);
        case minimizer_sketch_backend::bounded_nthash_simd:
            return scalar_absent && exact_absent && stats.bounded_nthash_available &&
                   stats.bounded_nthash.is_internally_consistent(require_nonempty_vector_work);
        case minimizer_sketch_backend::scalar:
            return stats.scalar_available &&
                   stats.scalar.is_coherent(require_nonempty_vector_work) && exact_absent &&
                   bounded_absent;
        case minimizer_sketch_backend::vulkan_segment:
            return false;
        }
        return false;
    }

    // Resolve the biological target-sketch backend from the minimizer selector
    // itself. Chaining and alignment choices must not silently substitute scalar
    // sketch semantics for an explicitly selected exact_simd/bounded ntHash producer.
    // Unsupported simd shapes fail in their checked drivers.
    [[nodiscard]] inline minimizer_sketch_backend
    target_index_sketch_backend_from_params(const minimap2_parameters& params) noexcept {
        if (params.minimizer_algorithm == minimizer_algorithm::bounded_nthash_simd) {
            return minimizer_sketch_backend::bounded_nthash_simd;
        }
        if (params.vulkan_enabled && params.vulkan_gpu_index_build_enabled &&
            params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer) {
            return minimizer_sketch_backend::vulkan_segment;
        }
        if (params.minimizer_algorithm == minimizer_algorithm::two_stack_nthash) {
            return minimizer_sketch_backend::exact_simd;
        }
        return minimizer_sketch_backend::scalar;
    }

    // Segment geometry. The minimap_sketch_data position field is 28 bits
    // (minimizer_representation<28,15>), so a segment's COLLECTED span — core + an overlap on
    // each side — must stay within that field, or the global position handed back by
    // `add_position_offset` would be silently masked by the packing ctor
    // (`pos_v & pos_field_mask`, index.cppm) and corrupt the seed. We cap the core at
    // 200 Mbp and each overlap at 30 Mbp; the static_assert ties both to the REAL
    // field width so neither can be bumped past 2^28 without a compile error.
    inline constexpr sequence_size_t k_segment_core_max_bp = 200'000'000ull; // core ceiling
    inline constexpr sequence_size_t k_segment_overlap_max_bp =
        30'000'000ull; // per-side overlap ceiling
    inline constexpr sequence_size_t k_segment_min_bp =
        32'000'000ull; // floor (keep batches useful)
    inline constexpr sequence_size_t k_vulkan_segment_min_bp =
        1'000'000ull; // floor when GPU sketch is selected
    // The 76M core and 96M collected-span bounds keep each admitted Vulkan sketch
    // within the fixed position layout while limiting upload-window count.
    inline constexpr sequence_size_t k_vulkan_segment_target_bp =
        76'000'000ull; // target core before query-overlap clamping
    inline constexpr sequence_size_t k_vulkan_segment_collected_max_bp = 96'000'000ull;
    static_assert(k_segment_core_max_bp + 2 * k_segment_overlap_max_bp <=
                      k_max_minimizer_sequence_length<minimap_sketch_data>,
                  "segment collected span (core + 2*overlap) must fit "
                  "minimap_sketch_data's 28-bit position field");

    // Choose a target-index SEGMENT size (bp) from the live memory budget. A full
    // minimizer index over an N-bp target costs ~6 bytes/bp at peak (collector vector
    // 8 bytes + flat_map bucket ~16 bytes/distinct + positions 8 bytes, all × density ~0.19).
    // On a 16 GB box a 6 Gbp full index (~37 GB) swaps; capping a segment to the
    // budget keeps the live index ~one segment, clamped to [k_segment_min_bp,
    // k_segment_core_max_bp].
    [[nodiscard]] inline sequence_size_t segment_bp_from_budget() noexcept {
        // Respect both the live process/system ceiling and the explicit index cap.
        // Use the same effective ceiling as admission so segment geometry and the
        // maximum legal index cannot disagree.
        const std::uint64_t budget =
            bioseq::parallelism::memory_admission_controller::instance().budget_ceiling();
        // allocatable_bytes() == UINT64_MAX when the mach/POSIX probe is unavailable;
        // dividing keeps it huge so the clamp yields k_segment_core_max_bp (the default).
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

    // Share `minimap_port_segmented` geometry with `dispatch_buffer`'s
    // stored-index assembly loop so it reuses the exact recompute
    // geometry. NOT a pure function of its args: it reads the runtime budget via
    // `segment_bp_from_budget()` exactly as recompute does. Each entry covers a
    // segment's overlapped collection range `[cs_u, ce_u)` (in units) and its core
    // claim band `[core_lo_bp, core_hi_bp)`.
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
