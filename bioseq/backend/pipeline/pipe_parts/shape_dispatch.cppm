/**
 * @file pipeline/pipe_parts/shape_dispatch.cppm
 * @brief Maps runtime settings to compiled pipeline and minimizer record types.
 */
export module bioseq.pipeline.pipe:shape_dispatch;

import std;
import :stage_graph;
import :minimizer_execution;
import :execution;
import bioseq.common.logging;
import bioseq.common.templating;
import bioseq.common.seqan_adapter;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;
import bioseq.highway.dispatch_contract;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.minimizer_layout_geometry;
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

    // Make the shared compile-time value list available to pipeline dispatch.

    using ::bioseq::common::templating::value_sequence;

    // Choose position-width buckets where the minimizer record changes size.
    // Add supported k values to kmer_size_buckets and storage_bits_for together.

    /// K-mer sizes with compiled dispatch entries. The first is the template default.
    /// Infer the array length from its entries so a smaller build profile cannot
    /// leave zero-filled slots that would instantiate an invalid k=0 layout.
    inline constexpr auto kmer_size_buckets = std::to_array<std::size_t>({
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
        15u
#else
        15u, 7u, 27u
#endif
    });

    /// Calculate the position-bit requirement for max_bp bases, capped at 63
    /// and raised to at least kmer_size for the layout's bucket range.
    /// Callers must supply a kmer_size no greater than 63.
    [[nodiscard]] constexpr std::size_t round_up_seq_len_log2(std::size_t max_bp,
                                                              std::size_t kmer_size) noexcept {
        // Saturate this legacy size helper. Registered-layout callers check the
        // original sequence length before using a layout.
        const std::size_t lg = ::bioseq::minimizers::minimizer_position_requirement(
            std::min<std::uint64_t>(max_bp, std::uint64_t{1u} << 63u));
        return std::clamp<std::size_t>(lg, kmer_size, 63u);
    }

    static_assert(round_up_seq_len_log2(0u, 15u) == 15u);
    static_assert(round_up_seq_len_log2(1u, 15u) == 15u);
    static_assert(round_up_seq_len_log2(std::size_t{1} << 28, 15u) == 28u);
    static_assert(round_up_seq_len_log2((std::size_t{1} << 48) + 1u, 15u) == 49u);
    static_assert(round_up_seq_len_log2(std::size_t{1} << 62, 15u) == 62u);

    namespace detail {

    /// Build a compile-time table of record sizes for one k-mer size and the
    /// supplied position widths. Each entry instantiates its minimizer layout.
    template <std::size_t p_kmer_size, std::size_t... p_bucket_values>
    [[nodiscard]] consteval auto
    make_storage_bits_table_impl(std::index_sequence<p_bucket_values...>) noexcept {
        using ::bioseq::minimizers::minimizer_layout;
        return std::array<std::size_t, sizeof...(p_bucket_values)>{
            minimizer_layout<p_bucket_values, p_kmer_size>::k_storage_bits...};
    }

    template <std::size_t p_kmer_size>
    [[nodiscard]] consteval auto make_storage_bits_table() noexcept {
        return make_storage_bits_table_impl<p_kmer_size>(std::make_index_sequence<64u>{});
    }

    /// Return the record size for a supported k-mer size, or zero otherwise.
    /// Keep this list consistent with kmer_size_buckets.
    [[nodiscard]] consteval std::size_t storage_bits_for(std::size_t p_bucket_width,
                                                         std::size_t p_kmer_size) noexcept {
        if (p_kmer_size == 15u || p_kmer_size == 20u ||
            p_kmer_size == 7u || p_kmer_size == 27u) {
            return ::bioseq::minimizers::minimizer_record_storage_bits(
                p_bucket_width, p_kmer_size);
        }
        return 0u;
    }

    /// Find position-width buckets for one k-mer size. Keep the lower bound,
    /// the last width before each increase in record size, and the final
    /// 63-bit bound. These buckets cover the same storage choices with fewer
    /// template instantiations.
    template <std::size_t p_kmer_size>
    [[nodiscard]] consteval auto compute_breakpoints_for() noexcept {
        std::array<std::size_t, 6u> out{};
        std::size_t out_n = 0u;
        std::size_t last_bits = 0u;
        for (std::size_t p_bucket_width = p_kmer_size; p_bucket_width <= 63u; ++p_bucket_width) {
            const std::size_t bits = storage_bits_for(p_bucket_width, p_kmer_size);
            if (bits != last_bits) {
                out[out_n++] = (last_bits == 0u) ? p_bucket_width : p_bucket_width - 1u;
                last_bits = bits;
            }
        }
        out[out_n++] = 63u;
        return std::pair{out, out_n};
    }

    static_assert(compute_breakpoints_for<15u>().second <= 6u);
    static_assert(compute_breakpoints_for<7u>().second <= 6u);
    static_assert(compute_breakpoints_for<27u>().second <= 6u);

    /// Scratch array for merging position-width buckets from the supported k values.
    using raw_buf = std::array<std::size_t, 32u>;

    /// Append this k-mer size's breakpoints, skipping values already present.
    template <std::size_t p_kmer_size>
    consteval void merge_breakpoints_into(raw_buf& raw, std::size_t& raw_n) noexcept {
        constexpr auto bp = compute_breakpoints_for<p_kmer_size>();
        for (std::size_t i = 0u; i < bp.second; ++i) {
            ::bioseq::common::templating::append_unique_value(raw, raw_n, bp.first[i]);
        }
    }

    /// Combine and sort the position-width buckets for all compiled k values.
    /// Sorting lets runtime dispatch choose the smallest sufficient bucket.
    template <std::size_t... p_indices>
    [[nodiscard]] consteval auto
    compute_global_l_buckets_impl(std::index_sequence<p_indices...>) noexcept {
        raw_buf raw{};
        std::size_t raw_n = 0u;
        (merge_breakpoints_into<kmer_size_buckets[p_indices]>(raw, raw_n), ...);
        ::bioseq::common::templating::sort_in_place_ascending(raw, raw_n);
        return std::pair{raw, raw_n};
    }

    [[nodiscard]] consteval auto compute_global_l_buckets() noexcept {
        return compute_global_l_buckets_impl(std::make_index_sequence<kmer_size_buckets.size()>{});
    }

    } // namespace detail

    /// Combined position-width buckets and the number of populated entries.
    /// The array has spare capacity, so use only its first _n entries.
    inline constexpr auto seq_len_log2_buckets_pair = detail::compute_global_l_buckets();
    inline constexpr auto seq_len_log2_buckets = seq_len_log2_buckets_pair.first;
    inline constexpr std::size_t seq_len_log2_buckets_n = seq_len_log2_buckets_pair.second;
    static_assert(seq_len_log2_buckets_n >= 1u, "seq_len_log2_buckets must be non-empty");

    namespace detail {

    /// Turn the populated position-width buckets into template arguments.
    template <std::size_t... p_indices>
    [[nodiscard]] consteval auto
    make_seq_len_log2_buckets_seq(std::index_sequence<p_indices...>) noexcept {
        return value_sequence<std::size_t, seq_len_log2_buckets[p_indices]...>{};
    }

    using seq_len_log2_buckets_seq =
        decltype(make_seq_len_log2_buckets_seq(std::make_index_sequence<seq_len_log2_buckets_n>{}));

    /// Turn the compiled k-mer sizes into template arguments.
    template <std::size_t... p_indices>
    [[nodiscard]] consteval auto
    make_kmer_size_buckets_seq(std::index_sequence<p_indices...>) noexcept {
        return value_sequence<std::size_t, kmer_size_buckets[p_indices]...>{};
    }

    using kmer_size_buckets_seq =
        decltype(make_kmer_size_buckets_seq(std::make_index_sequence<kmer_size_buckets.size()>{}));

    // Build the position-width list for one selected k. Dispatching only its
    // relevant widths avoids compiling every combination in the global list.
    template <std::size_t p_kmer_size, std::size_t... p_indices>
    [[nodiscard]] consteval auto
    make_per_k_breakpoints_seq_impl(std::index_sequence<p_indices...>) noexcept {
        constexpr auto bp = compute_breakpoints_for<p_kmer_size>();
        return value_sequence<std::size_t, bp.first[p_indices]...>{};
    }

    template <std::size_t p_kmer_size>
    [[nodiscard]] consteval auto make_per_k_breakpoints_seq() noexcept {
        constexpr auto bp = compute_breakpoints_for<p_kmer_size>();
        return make_per_k_breakpoints_seq_impl<p_kmer_size>(std::make_index_sequence<bp.second>{});
    }

    template <std::size_t p_kmer_size>
    using per_k_breakpoints_seq = decltype(make_per_k_breakpoints_seq<p_kmer_size>());

    static_assert(per_k_breakpoints_seq<15u>::size() == compute_breakpoints_for<15u>().second);
    static_assert(per_k_breakpoints_seq<7u>::size() == compute_breakpoints_for<7u>().second);
    static_assert(per_k_breakpoints_seq<27u>::size() == compute_breakpoints_for<27u>().second);

    } // namespace detail

    /// Template k value used when the requested k has no compiled entry.
    /// The generic pipeline still reads the requested k from runtime parameters.
    inline constexpr std::size_t kmer_size_default = 15u;
    static_assert(kmer_size_default == kmer_size_buckets[0],
                  "kmer_size_default must match the leading entry of kmer_size_buckets — "
                  "the chain's default-value path expects them in sync.");

    // Reuse the shared exact-value and size-bucket dispatch helpers.

    using ::bioseq::common::templating::dispatch_exact_value_or_default;
    using ::bioseq::common::templating::dispatch_exact_value_or_default_void;
    using ::bioseq::common::templating::dispatch_smallest_not_less;

    // Each registry lists its compiled values and default, and reads the
    // corresponding runtime setting. Dispatch steps use that information to
    // choose a template instantiation.

    template <class p_enum> struct algorithm_enum_values;

    template <> struct algorithm_enum_values<::bioseq::simd::chaining_algorithm> {
        using target_t = ::bioseq::simd::chaining_algorithm;
        static constexpr target_t default_value = target_t::scalar_minimap2;
        using value_list = value_sequence<target_t,
                                          target_t::scalar_minimap2,
                                          target_t::simd_minimap2,
                                          target_t::range_minimum_query,
                                          target_t::vulkan_tiled_dynamic_programming>;

        template <class... p_arguments>
        [[nodiscard, gnu::always_inline]] static constexpr target_t
        read_field(const ::bioseq::simd::minimap2_parameters& p, p_arguments&&...) noexcept {
            return p.chaining_algorithm;
        }
    };

    template <> struct algorithm_enum_values<::bioseq::simd::base_alignment_algorithm> {
        using target_t = ::bioseq::simd::base_alignment_algorithm;
        static constexpr target_t default_value = target_t::block_aligner;
        using value_list = value_sequence<target_t,
                                          target_t::ksw2,
                                          target_t::block_aligner,
                                          target_t::ksw2_batched>;

        template <class... p_arguments>
        [[nodiscard, gnu::always_inline]] static constexpr target_t
        read_field(const ::bioseq::simd::minimap2_parameters& p, p_arguments&&...) noexcept {
            return p.base_alignment_algorithm;
        }
    };

    template <> struct algorithm_enum_values<::bioseq::simd::minimizer_algorithm> {
        using target_t = ::bioseq::simd::minimizer_algorithm;
        static constexpr target_t default_value = target_t::scalar;
        using value_list = value_sequence<target_t,
                                          target_t::scalar,
                                          target_t::simd_minimap2_reference,
                                          target_t::two_stack_nthash,
                                          target_t::vulkan_minimap_integer,
                                          target_t::bounded_nthash_simd>;

        template <class... p_arguments>
        [[nodiscard, gnu::always_inline]] static constexpr target_t
        read_field(const ::bioseq::simd::minimap2_parameters& p, p_arguments&&...) noexcept {
            return p.minimizer_algorithm;
        }
    };

    namespace detail {

    template <class p_enum, p_enum p_value>
    [[nodiscard]] consteval bool trait_specialised_for() noexcept {
        return requires { typename algorithm_traits<p_value>::kernel_type; };
    }

    template <class p_enum, p_enum... p_values>
    [[nodiscard]] consteval bool
    all_traits_specialised_for(value_sequence<p_enum, p_values...>) noexcept {
        return (trait_specialised_for<p_enum, p_values>() && ...);
    }

    } // namespace detail

    static_assert(detail::all_traits_specialised_for(
                      algorithm_enum_values<::bioseq::simd::chaining_algorithm>::value_list{}),
                  "algorithm_enum_values<chaining_algorithm>::value_list contains a value "
                  "without an algorithm_traits<>::kernel_type specialisation.  Add the trait "
                  "specialisation alongside the enum value.");
    static_assert(
        detail::all_traits_specialised_for(
            algorithm_enum_values<::bioseq::simd::base_alignment_algorithm>::value_list{}),
        "algorithm_enum_values<base_alignment_algorithm>::value_list contains a "
        "value without an algorithm_traits<>::kernel_type specialisation.");
    static_assert(detail::all_traits_specialised_for(
                      algorithm_enum_values<::bioseq::simd::minimizer_algorithm>::value_list{}),
                  "algorithm_enum_values<minimizer_algorithm>::value_list contains a "
                  "value without an algorithm_traits<>::kernel_type specialisation.");

    namespace detail {

    /// Require a sequence length convertible to size_t. This catches dispatch
    /// calls that omit the query or target range where length selection needs it.
    template <class p_type>
    concept has_bp_size = requires(const p_type& x) {
        { x.bp_size() } -> std::convertible_to<std::size_t>;
    };

    struct seq_len_log2_registry {
        using target_t = std::size_t;
        static constexpr std::size_t default_value = seq_len_log2_buckets[0];
        using value_list = seq_len_log2_buckets_seq;

        template <has_bp_size p_query_range,
                  has_bp_size p_target_range,
                  class... p_remaining_stages>
        [[nodiscard, gnu::always_inline]] static std::size_t
        read_field(const ::bioseq::simd::minimap2_parameters& params,
                   const p_query_range& q,
                   const p_target_range& t,
                   p_remaining_stages&&...) noexcept {
            const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
            return round_up_seq_len_log2(max_bp, static_cast<std::size_t>(params.kmer_size));
        }
    };

    struct kmer_size_registry {
        using target_t = std::size_t;
        static constexpr std::size_t default_value = kmer_size_default;
        using value_list = kmer_size_buckets_seq;

        template <class... p_arguments>
        [[nodiscard, gnu::always_inline]] static constexpr std::size_t
        read_field(const ::bioseq::simd::minimap2_parameters& params, p_arguments&&...) noexcept {
            return static_cast<std::size_t>(params.kmer_size);
        }
    };

    /// Select position-width buckets for the k value chosen by an earlier step.
    /// This reduces the number of compiled k/width combinations. Place this
    /// registry's step after kmer_size_step so that k is already available.
    struct seq_len_log2_registry_per_k {
        using target_t = std::size_t;

        /// Default required by the dispatch-step interface. This step is always active.
        static constexpr std::size_t default_value = 15u;

        template <std::size_t p_kmer_size, auto... p_rest_pinned>
        using value_list_for = per_k_breakpoints_seq<p_kmer_size>;

        template <std::size_t p_pinned_kmer_size,
                  has_bp_size p_query_range,
                  has_bp_size p_target_range,
                  class... p_remaining_stages>
        [[nodiscard, gnu::always_inline]] static std::size_t
        read_field_for(const ::bioseq::simd::minimap2_parameters& params,
                       const p_query_range& q,
                       const p_target_range& t,
                       p_remaining_stages&&...) noexcept {
            const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
            return round_up_seq_len_log2(max_bp, static_cast<std::size_t>(params.kmer_size));
        }
    };

    } // namespace detail

    // Pipeline aliases reuse the shared dispatch chain and step adapters.
    // They supply minimap2_parameters as the runtime settings type.

    namespace detail {

    using ::bioseq::common::templating::bucket_step;
    using ::bioseq::common::templating::dependent_bucket_step;
    using ::bioseq::common::templating::enum_step;
    using ::bioseq::common::templating::guarded_step;

    using baa_step = enum_step<algorithm_enum_values<::bioseq::simd::base_alignment_algorithm>>;
    using ca_step = enum_step<algorithm_enum_values<::bioseq::simd::chaining_algorithm>>;
    using ma_step = enum_step<algorithm_enum_values<::bioseq::simd::minimizer_algorithm>>;
    using kmer_size_step = enum_step<kmer_size_registry>;
    using seq_len_log2_step = bucket_step<seq_len_log2_registry>;

    /// Place this after kmer_size_step to select length buckets for the chosen k.
    using seq_len_log2_step_per_k = dependent_bucket_step<seq_len_log2_registry_per_k>;

    /// A dispatch chain using minimap2_parameters. The caller supplies the final
    /// operation, its result type, and the sequence of selection steps.
    template <class p_result, template <auto... p_values> class p_leaf, class... p_steps>
    using dispatch_chain = ::bioseq::common::templating::
        dispatch_chain<p_result, p_leaf, ::bioseq::simd::minimap2_parameters, p_steps...>;

    } // namespace detail

    // pipeline_run_leaf and pipeline_run_traits in pipe.cppm connect these
    // choices to the specialized or generic pipeline implementation.

    } // namespace bioseq::simd::pipe

} // export
