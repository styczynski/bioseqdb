/**
 * @file pipeline/pipe_parts/shape_dispatch.cppm
 * @brief Compile-time shape buckets and dispatch-chain registries.
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

    // =============================================================================
    // value_sequence non-type parameter-pack carrier
    // =============================================================================
    // Re-export the shared carrier so pipeline call sites retain its unqualified
    // name.

    using ::bioseq::common::templating::value_sequence;

    // =============================================================================
    // minimizer_layout-driven p_bucket_width derivation
    // =============================================================================
    //
    // The bucket list is computed at compile time from the per-p_kmer_size
    // transitions in `minimizer_layout::k_storage_bits`,
    // so adding a new p_kmer_size only requires editing `kmer_size_buckets` and the
    // per-p_kmer_size clause in `storage_bits_for`.

    /// Canonical k-mer values the bench/postgres paths instantiate.  The leading
    /// entry is the default for non-canonical kmer_size — see kmer_size_default.
    ///
    /// The array's size must
    /// match its element count, not be hard-coded to 3. A `std::array<size_t, 3>`
    /// with only `{15u}` initialised silently zeroes the trailing two slots, and
    /// the consteval dispatch table then enumerates p_kmer_size=0, which instantiates
    /// `minimizer_representation<63, 0>` whose `hash_shift` equals `sizeof(storage_type)*8` and
    /// trips an undefined-shift constexpr error in `index.cppm:151`. `std::to_array`
    /// deduces the array size from the initialiser so the two stay in lockstep.
    inline constexpr auto kmer_size_buckets = std::to_array<std::size_t>({
#if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_EXTENDED_KMER_COORDINATES)
        15u
#else
        15u, 7u, 27u
#endif
    });

    /// `bit_width(max(max_bp - 1, 1))` clamped into `[p_kmer_size, 63]` — the smallest
    /// `p_bucket_width` such that `2^p_bucket_width > max_bp - 1`, lifted up to `p_kmer_size` so
    /// the resulting `minimizer_layout<p_bucket_width, p_kmer_size>` keeps `pos_bits >= kmer_size`.
    /// At p_bucket_width=63 every p_kmer_size resolves to a non-void storage_type (the 128-bit
    /// specialisation handles the upper band), so a clamp on the upper end is sufficient.
    [[nodiscard]] constexpr std::size_t round_up_seq_len_log2(std::size_t max_bp,
                                                              std::size_t kmer_size) noexcept {
        // Preserve the legacy helper's saturating contract. New physical-layout
        // selection checks the original extent before reaching this fallback.
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

    /// Pack-expansion builder for a per-p_kmer_size table of `k_storage_bits` values keyed
    /// by p_bucket_width ∈ [0, 64).  The layout type takes (p_bucket_width, p_kmer_size) as
    /// template parameters, so the only way to convert a runtime p_bucket_width into a template
    /// argument is to pre-compute every value of `minimizer_layout<p_bucket_width,
    /// p_kmer_size>::k_storage_bits` for p_bucket_width in the legal range and index into the
    /// resulting `std::array`. All work happens at constant-evaluation time. The dispatcher path
    /// never enters this helper.
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

    /// Look up `minimizer_layout<p_bucket_width, p_kmer_size>::k_storage_bits` from the
    /// per-p_kmer_size table.  The per-p_kmer_size clauses are a *dispatch table*, not a hand-tuned
    /// ladder — they exist because the layout requires p_kmer_size as a *template* argument and the
    /// consteval helper above can only enumerate p_bucket_width for a fixed compile-time
    /// p_kmer_size.  Every p_kmer_size in `kmer_size_buckets` must have a matching clause here.
    [[nodiscard]] consteval std::size_t storage_bits_for(std::size_t p_bucket_width,
                                                         std::size_t p_kmer_size) noexcept {
        if (p_kmer_size == 15u || p_kmer_size == 20u ||
            p_kmer_size == 7u || p_kmer_size == 27u) {
            return ::bioseq::minimizers::minimizer_record_storage_bits(
                p_bucket_width, p_kmer_size);
        }
        return 0u;
    }

    /// Compute the breakpoint p_bucket_width values for a given p_kmer_size.  For each
    /// p_bucket_width in [p_kmer_size, 63]:
    ///   - p_bucket_width is emitted if storage_bits_for(p_bucket_width, p_kmer_size) !=
    ///   storage_bits_for(p_bucket_width-1, p_kmer_size),
    ///     recorded as the largest p_bucket_width of the previous band (i.e. p_bucket_width-1).
    ///   - p_bucket_width = p_kmer_size is emitted as the lower anchor.
    ///   - p_bucket_width = 63 is always emitted as the terminal anchor.
    /// Max 6 entries possible (initial + up to 3 storage transitions + terminal +
    /// headroom).
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

    /// Work-buffer type for `compute_global_l_buckets`.  Sized 32 (≤ 6
    /// breakpoints/p_kmer_size × ≤ 5 kmer sizes = 30, rounded up to a power of two for
    /// headroom).
    using raw_buf = std::array<std::size_t, 32u>;

    /// Free consteval helper — merges compute_breakpoints_for<p_kmer_size>()'s breakpoint
    /// list into (raw, raw_n) using the lifted `append_unique_value` primitive.
    template <std::size_t p_kmer_size>
    consteval void merge_breakpoints_into(raw_buf& raw, std::size_t& raw_n) noexcept {
        constexpr auto bp = compute_breakpoints_for<p_kmer_size>();
        for (std::size_t i = 0u; i < bp.second; ++i) {
            ::bioseq::common::templating::append_unique_value(raw, raw_n, bp.first[i]);
        }
    }

    /// Pack-expanded union over `kmer_size_buckets` indices.  Threads the
    /// accumulator state through `merge_breakpoints_into` without implicit captures.
    /// and finishes with the lifted insertion-sort primitive — the
    /// dispatcher's smallest-ge lookup needs sorted input.
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

    /// Materialised dispatcher p_bucket_width bucket list — the union of per-p_kmer_size
    /// breakpoints computed at compile time. The `_n` constant gives the live prefix size. The
    /// array itself is sized 32 for headroom.
    inline constexpr auto seq_len_log2_buckets_pair = detail::compute_global_l_buckets();
    inline constexpr auto seq_len_log2_buckets = seq_len_log2_buckets_pair.first;
    inline constexpr std::size_t seq_len_log2_buckets_n = seq_len_log2_buckets_pair.second;
    static_assert(seq_len_log2_buckets_n >= 1u, "seq_len_log2_buckets must be non-empty");

    namespace detail {

    /// Materialise `seq_len_log2_buckets` as a `value_sequence` the chain can
    /// pack-expand.  Used as the value-list of `seq_len_log2_registry`.
    template <std::size_t... p_indices>
    [[nodiscard]] consteval auto
    make_seq_len_log2_buckets_seq(std::index_sequence<p_indices...>) noexcept {
        return value_sequence<std::size_t, seq_len_log2_buckets[p_indices]...>{};
    }

    using seq_len_log2_buckets_seq =
        decltype(make_seq_len_log2_buckets_seq(std::make_index_sequence<seq_len_log2_buckets_n>{}));

    /// Materialise `kmer_size_buckets` similarly.  Used as the value-list of
    /// `kmer_size_registry`.
    template <std::size_t... p_indices>
    [[nodiscard]] consteval auto
    make_kmer_size_buckets_seq(std::index_sequence<p_indices...>) noexcept {
        return value_sequence<std::size_t, kmer_size_buckets[p_indices]...>{};
    }

    using kmer_size_buckets_seq =
        decltype(make_kmer_size_buckets_seq(std::make_index_sequence<kmer_size_buckets.size()>{}));

    // Per-p_kmer-size sequence-length breakpoint sequence.
    //
    // Materialise the p_kmer_size-specific breakpoint list (from
    // `compute_breakpoints_for<p_kmer_size>()`) as a `value_sequence<std::size_t, ...>`.  Used by
    // `dependent_bucket_step`'s p_kmer_size-aware `value_list_for<p_kmer_size>` to iterate only the
    // p_bucket_width breakpoints relevant for that p_kmer_size — rather than the global union shape
    // `seq_len_log2_buckets_seq` (which causes the explosion).
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

    /// Canonical k-mer default for any non-canonical
    /// `params.kmer_size` value.  The chain's `kmer_size_step` pins this when
    /// the runtime field is outside `kmer_size_buckets`.
    inline constexpr std::size_t kmer_size_default = 15u;
    static_assert(kmer_size_default == kmer_size_buckets[0],
                  "kmer_size_default must match the leading entry of kmer_size_buckets — "
                  "the chain's default-value path expects them in sync.");

    // =============================================================================
    // Low-level enum/value dispatch primitives
    // =============================================================================
    //
    // The dispatch primitives live in `bioseq.common.templating` and are
    // re-exported here for unqualified pipeline call sites.

    using ::bioseq::common::templating::dispatch_exact_value_or_default;
    using ::bioseq::common::templating::dispatch_exact_value_or_default_void;
    using ::bioseq::common::templating::dispatch_smallest_not_less;

    // =============================================================================
    // algorithm_enum_values registry and per-dimension selectors
    // =============================================================================
    //
    // Each registry exposes:
    //   - `target_t`        — the NTTP type the dispatcher pins.
    //   - `default_value`   — pinned when the runtime field is outside the pack.
    //   - `value_list`      — `std::integer_sequence<target_t, ...>` of enum/size_t
    //                         values the dispatcher iterates.
    //   - `read_field(p, args...)` — extracts the target_t value from the runtime
    //                         parameter pack (singular method name shared by every
    //                         step adapter; the implementation may literally read a
    //                         field or compute a derived value).
    //
    // Adding a new dispatched dimension is one new registry plus one `enum_step` /
    // `bucket_step` alias.

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

    /// A type satisfies `has_bp_size` when it exposes a `bp_size()` member
    /// returning an unsigned integer.  Constrains `seq_len_log2_registry::read_field`'s
    /// first two args so a chain composing `seq_len_log2_step` into a call site
    /// lacking query and target ranges in those slots produces a localized diagnostic
    /// at the chain instantiation point.
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

    /// Pinned-aware sequence-length registry.
    ///
    /// Exposes `value_list_for<p_kmer_size, …>` and `read_field_for<p_kmer_size, …>` so the chain's
    /// `dependent_bucket_step` can iterate only the p_bucket_width breakpoints for the
    /// previously-pinned p_kmer_size. This collapses the 21-cell (p_bucket_width, p_kmer_size)
    /// Cartesian product (the global p_bucket_width union × p_kmer_size) into the per-p_kmer_size
    /// breakpoint count: `3 + 4 + 2 = 9` cells (p_kmer_size=15: 3 buckets, p_kmer_size=7: 4,
    /// p_kmer_size=27: 2).
    ///
    /// Place this step *after* `kmer_size_step` in the chain so `Pinned... = (p_kmer_size, …)`.
    struct seq_len_log2_registry_per_k {
        using target_t = std::size_t;

        /// Anchored to p_kmer_size=15's lower bucket boundary (`compute_breakpoints_for<15>()[0]`).
        /// The chain only pins `default_value` when `active_v` is false. Every
        /// `dependent_bucket_step` is unconditionally active, but the
        /// chain primitive requires a value to thread through inactive steps.
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

    // =============================================================================
    // dispatch_chain primitive and step adapters
    // =============================================================================
    //
    // The chain primitive and step adapters live in
    // `bioseq.common.templating::dispatch_chain.cppm`.
    // They are re-exported here so the pipeline body keeps unqualified
    // names. The per-pipeline alias supplies the `minimap2_parameters` slot so
    // chain call sites do not need to repeat it.

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

    /// The per-p_kmer_size seq_len_log2 step must follow `kmer_size_step`
    /// in the chain so the pinned-p_kmer_size's per-p_kmer_size breakpoint list is iterated.
    using seq_len_log2_step_per_k = dependent_bucket_step<seq_len_log2_registry_per_k>;

    /// Per-pipeline chain alias for the request parameter type used here.
    /// `p_result` and the leaf template stay as template
    /// parameters because real call sites instantiate with different
    /// return types (match_result_collection / pipeline_description / void).
    template <class p_result, template <auto... p_values> class p_leaf, class... p_steps>
    using dispatch_chain = ::bioseq::common::templating::
        dispatch_chain<p_result, p_leaf, ::bioseq::simd::minimap2_parameters, p_steps...>;

    } // namespace detail

    // =============================================================================
    // Leaf types and traits aliases
    // =============================================================================
    // `pipeline_run_leaf` terminates the runtime cascade and selects the
    // specialized or fallback parameter branch. `pipeline_run_traits::leaf`
    // exposes it in the NTTP-only form required by `dispatch_chain`.

    } // namespace bioseq::simd::pipe

} // export
