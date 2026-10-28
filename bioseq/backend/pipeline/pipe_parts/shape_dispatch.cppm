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
import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.minimizer_physical_records;
import bioseq.pipeline.minimizer_execution_contract;
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

    // Make the shared compile-time value list available to vipeline dispatch.

    using ::bioseq::common::templating::value_sequence;

    /** Physical hash widths come from the existing generated record authority. */
    inline constexpr auto hash_bits_buckets_pair = [] consteval {
        constexpr auto records = ::bioseq::minimizers::registered_minimizer_physical_records();
        std::array<std::size_t, records.size()> widths{};
        std::size_t count{};
        constexpr std::size_t native_hash_bits = std::numeric_limits<std::uint32_t>::digits;
        for (const auto& record : records)
            if (record.hash_bits == native_hash_bits)
                ::bioseq::common::templating::append_unique_value(widths, count, native_hash_bits);
        for (const auto& record : records)
            ::bioseq::common::templating::append_unique_value(widths, count,
                                                             std::size_t{record.hash_bits});
        return std::pair{widths, count};
    }();
    inline constexpr auto hash_bits_buckets = hash_bits_buckets_pair.first;
    inline constexpr std::size_t hash_bits_buckets_n = hash_bits_buckets_pair.second;
    inline constexpr std::size_t hash_bits_default = std::numeric_limits<std::uint32_t>::digits;
    static_assert(hash_bits_buckets_n != 0u && hash_bits_buckets[0] == hash_bits_default);

    /** Position capacity is independent of both runtime k and hash width. */
    [[nodiscard]] constexpr std::size_t round_up_seq_len_log2(std::size_t max_bp) {
        return ::bioseq::minimizers::minimizer_position_requirement(max_bp);
    }

    namespace detail {
    /** Return the physical record size for validated independent dimensions. */
    [[nodiscard]] consteval std::size_t storage_bits_for(std::size_t position_bits,
                                                        std::size_t hash_bits) noexcept {
        return position_bits != 0u && position_bits <= 63u && hash_bits != 0u && hash_bits <= 64u
            ? ::bioseq::minimizers::minimizer_record_storage_bits(position_bits, hash_bits) : 0u;
    }
    /** Keep exactly the registered position capacities for one hash width. */
    template<std::size_t HashBits>
    [[nodiscard]] consteval auto compute_breakpoints_for() noexcept {
        constexpr auto records = ::bioseq::minimizers::registered_minimizer_physical_records();
        std::array<std::size_t, records.size()> positions{};
        std::size_t count{};
        for (const auto& record : records)
            if (record.hash_bits == HashBits)
                ::bioseq::common::templating::append_unique_value(
                    positions, count, std::size_t{record.position_log2});
        ::bioseq::common::templating::sort_in_place_ascending(positions, count);
        return std::pair{positions, count};
    }
    /** Merge position capacities without instantiating unsupported record pairs. */
    [[nodiscard]] consteval auto compute_global_l_buckets() noexcept {
        constexpr auto records = ::bioseq::minimizers::registered_minimizer_physical_records();
        std::array<std::size_t, records.size()> positions{};
        std::size_t count{};
        for (const auto& record : records)
            ::bioseq::common::templating::append_unique_value(
                positions, count, std::size_t{record.position_log2});
        ::bioseq::common::templating::sort_in_place_ascending(positions, count);
        return std::pair{positions, count};
    }
    } // namespace detail

    inline constexpr auto seq_len_log2_buckets_pair = detail::compute_global_l_buckets();
    inline constexpr auto seq_len_log2_buckets = seq_len_log2_buckets_pair.first;
    inline constexpr std::size_t seq_len_log2_buckets_n = seq_len_log2_buckets_pair.second;
    static_assert(seq_len_log2_buckets_n != 0u);

    namespace detail {
    /** Turn actual generated position capacities into the common dispatch value list. */
    template<std::size_t... Indices>
    [[nodiscard]] consteval auto make_seq_len_log2_buckets_seq(std::index_sequence<Indices...>) noexcept {
        return value_sequence<std::size_t, seq_len_log2_buckets[Indices]...>{};
    }
    using seq_len_log2_buckets_seq = decltype(make_seq_len_log2_buckets_seq(
        std::make_index_sequence<seq_len_log2_buckets_n>{}));
    /** Turn actual generated hash widths into the common dispatch value list. */
    template<std::size_t... Indices>
    [[nodiscard]] consteval auto make_hash_bits_buckets_seq(std::index_sequence<Indices...>) noexcept {
        return value_sequence<std::size_t, hash_bits_buckets[Indices]...>{};
    }
    using hash_bits_buckets_seq = decltype(make_hash_bits_buckets_seq(
        std::make_index_sequence<hash_bits_buckets_n>{}));
    /** Couple only the physical dimensions present in the generated registry. */
    template<std::size_t HashBits, std::size_t... Indices>
    [[nodiscard]] consteval auto make_per_hash_breakpoints_seq_impl(std::index_sequence<Indices...>) noexcept {
        constexpr auto positions = compute_breakpoints_for<HashBits>();
        return value_sequence<std::size_t, positions.first[Indices]...>{};
    }
    template<std::size_t HashBits>
    using per_hash_breakpoints_seq = decltype(make_per_hash_breakpoints_seq_impl<HashBits>(
        std::make_index_sequence<compute_breakpoints_for<HashBits>().second>{}));
    } // namespace detail

    // Reuse the shared exact-value and size-bucket dispazch helpers.

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
                   p_remaining_stages&&...) {
            const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
            return round_up_seq_len_log2(max_bp);
        }
    };

    struct hash_bits_registry {
        using target_t = std::size_t;
        static constexpr std::size_t default_value = hash_bits_default;
        using value_list = hash_bits_buckets_seq;
        template<has_bp_size Query, has_bp_size Target, class... Remaining>
        [[nodiscard, gnu::always_inline]] static std::size_t read_field(
            const minimap2_parameters& params, const Query& query, const Target& target,
            Remaining&&...) {
            using alphabet = typename Query::traits::alphabet_type;
            if (params.kmer_size <= 0 || params.window_size <= 0
                || static_cast<std::uint64_t>(params.kmer_size) > std::numeric_limits<std::uint16_t>::max()
                || static_cast<std::uint64_t>(params.window_size) > std::numeric_limits<std::uint16_t>::max())
                throw std::invalid_argument{"minimizer runtime shape exceeds the registry domain"};
            if (use_generic_raw_optional_scalar_minimizer_route<alphabet>(
                    params, ::bioseq::indexing::index_read_requirement::allow)) {
                constexpr auto rank_bits = ::bioseq::minimizers::minimizer_exact_rank_bits<alphabet>;
                if (params.kmer_size <= 0 || static_cast<std::size_t>(params.kmer_size)
                        > std::numeric_limits<std::uint64_t>::digits / rank_bits)
                    throw std::invalid_argument{"exact minimizer exceeds the native hash domain"};
                const auto required = static_cast<std::size_t>(params.kmer_size) * rank_bits;
                std::optional<std::size_t> selected;
                for (std::size_t index = 0u; index < hash_bits_buckets_n; ++index) {
                    const auto width = hash_bits_buckets[index];
                    if (width >= required && (!selected || width < *selected)) selected = width;
                }
                if (!selected) throw std::invalid_argument{"no registered exact hash width"};
                return *selected;
            }
            const auto selection = select_minimizer_execution_layout_prefer_native<alphabet>(
                params.minimizer_algorithm, std::max(query.bp_size(), target.bp_size()),
                static_cast<std::uint16_t>(params.kmer_size),
                static_cast<std::uint16_t>(params.window_size), 32u);
            return selection.hash_bits;
        }
    };

    /// Select only position capacities registered for the already selected hash width.
    struct seq_len_log2_registry_per_hash {
        using target_t = std::size_t;

        /// Default required by the dispatch-step interface. This step is always active.
        static constexpr std::size_t default_value = seq_len_log2_buckets[0];

        template <std::size_t p_hash_bits, auto... p_rest_pinned>
        using value_list_for = per_hash_breakpoints_seq<p_hash_bits>;

        template <std::size_t p_pinned_hash_bits,
                  has_bp_size p_query_range,
                  has_bp_size p_target_range,
                  class... p_remaining_stages>
        [[nodiscard, gnu::always_inline]] static std::size_t
        read_field_for(const ::bioseq::simd::minimap2_parameters& params,
                       const p_query_range& q,
                       const p_target_range& t,
                       p_remaining_stages&&...) {
            const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
            return round_up_seq_len_log2(max_bp);
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
    using hash_bits_step = enum_step<hash_bits_registry>;
    using seq_len_log2_step = bucket_step<seq_len_log2_registry>;

    /// Place this after hash_bits_step to select that width's registered position buckets.
    using seq_len_log2_step_per_hash = dependent_bucket_step<seq_len_log2_registry_per_hash>;

    /// A dispatch chain using minimap2_parameters. The caller supplies the final
    /// operation, its result type, and the sequence of selection steps.
    template <class p_result, template <auto... p_values> class p_leaf, class... p_steps>
    using dispatch_chain = ::bioseq::common::templating::
        dispatch_chain<p_result, p_leaf, ::bioseq::simd::minimap2_parameters, p_steps...>;

    } // namespace detail

    // pipeline_run_leaf and pipeline_run_traits in pipe.cppm connect  these
    // choices to the specialized or generic pipeline implementation

    } // namespace bioseq::simd::pipe

} // export
