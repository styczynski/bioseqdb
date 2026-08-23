/**
 * @file pipeline/pipe_parts/stage_graph.cppm
 * @brief Compile-time stage protocol and composition for the alignment pipeline.
 */
module;

#include <libassert/assert.hpp>
#include <new>

export module bioseq.pipeline.pipe:stage_graph;

export import bioseq.pipeline.runtime_contract;

import std;
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
export import bioseq.pipeline.vulkan_decline;

export {

    namespace bioseq::simd::pipe {

    namespace prefilter = ::bioseq::prefilter;
    // Keep the canonical minimizer-index concepts visible to stage templates.
    using bioseq::indexing::minimizer_hashmap_fragment_index;
    using bioseq::indexing::minimizer_lookup_index;
    using bioseq::indexing::minimizer_seed_index;

    // Export exception aliases through the pipeline module so PG importers do not
    // redundantly import the Highway-heavy implementation modules.
    using exact_simd_runtime_error = ::bioseq::minimizers::exact_simd_runtime_error;
    using simd_chaining_runtime_error = ::bioseq::chaining::simd_chaining_runtime_error;
    using simd_chaining_status = ::bioseq::chaining::simd_chaining_status;

    /// Fail-closed error for the selected bounded ntHash route.  PostgreSQL-facing
    /// callers catch this concrete type before entering the longjmp ERROR boundary.
    /// no bounded ntHash rejection is allowed to escape the C++ frame or select another
    /// minimizer implementation.
    class bounded_nthash_runtime_error final : public std::runtime_error {
      public:
        /// Describe a fail-closed bounded-ntHash rejection.
        explicit bounded_nthash_runtime_error(std::string detail)
            : std::runtime_error(std::string{"bounded_nthash_simd minimizer execution rejected: "} +
                                 std::move(detail)) {}
    };

    // — pipeline lengths/positions use the 64-bit sequence_size_t.
    using bioseq::storage::sequence_size_t;

    using host_stage_clock = std::chrono::steady_clock;

    /// Sample the monotonic host-stage clock as a positive nanosecond value.
    [[nodiscard]] inline std::uint64_t host_stage_now_ns() noexcept {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             host_stage_clock::now().time_since_epoch())
                             .count();
        return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
    }

    /// Return a positive elapsed host-stage duration for one sampled start.
    [[nodiscard]] inline std::uint64_t
    elapsed_host_stage_ns(const std::uint64_t started_ns) noexcept {
        const std::uint64_t finished_ns = host_stage_now_ns();
        const std::uint64_t elapsed = finished_ns >= started_ns ? finished_ns - started_ns : 0u;
        // A stage that executed is a positive observation even on a platform whose
        // steady-clock tick is coarser than one nanosecond. The clamp can add at
        // most one nanosecond and keeps focused tests independent of clock
        // resolution without sleeping.
        return elapsed > 0 ? elapsed : 1u;
    }

    /// Report the structured decline emitted by a mandatory Vulkan sketch.
    [[noreturn]] inline void
    throw_vulkan_sketch_declined(std::string_view stage,
                                 const bioseq::gpu::vulkan::vulkan_sketch_decline& decline) {
        if (!decline.declined()) {
            throw_vulkan_declined(stage);
        }
        bioseq::gpu::vulkan::throw_vulkan_strict_decline(stage, decline);
    }

    // — the chain primitive is load-bearing on relaxed
    // template-template parameter matching (P0522R0) and C++20 class-type
    // NTTPs (carrying `auto...` packs that interleave enum NTTPs with
    // `std::size_t`).  Guard the build mode so a misconfigured build fails
    // loudly rather than producing subtly-wrong codegen.
    static_assert(__cplusplus >= 202302L, "bioseq pipeline templates require C++23");

    // Import types from the surrounding modules so the stage bodies can refer
    // to them by short name.
    using ::bioseq::chaining::anchor;
    using ::bioseq::chaining::arena_slab;
    using ::bioseq::chaining::chain;
    using ::bioseq::chaining::chain_anchors_rmq;
    using ::bioseq::chaining::chain_anchors_scalar;
    using ::bioseq::chaining::chaining_parameters;
    using ::bioseq::chaining::make_arena_slab;

    // =============================================================================
    // Compile-time parameter packs
    // =============================================================================

    /**
     * @brief Type-level parameter pack for a fully specialised pipeline instance.
     *
     * @tparam p_window_size           Minimizer window size (must be > 0).
     * @tparam p_kmer_size           p_kmer_size-mer length (must match the
     * minimizer_representation's KmerLen).
     * @tparam p_sequence_length_log2  ceil(log2(max(query_bp, target_bp))).  Selects the
     *                     `minimizer_representation<p_sequence_length_log2, p_kmer_size>` storage
     * bucket — same dispatch already happens at runtime in `minimap_port`.
     */
    template <std::size_t p_window_size,
              std::size_t p_kmer_size,
              std::size_t p_sequence_length_log2>
    struct static_aligner_parameters {
        static constexpr std::size_t window_len = p_window_size;
        static constexpr std::size_t kmer_len = p_kmer_size;
        static constexpr std::size_t sequence_len_log2 = p_sequence_length_log2;
        static constexpr bool generic_fallback = false;

        using minimizer_repr_t =
            ::bioseq::minimizers::minimizer_representation<p_sequence_length_log2, p_kmer_size>;

        [[nodiscard]] static constexpr std::string_view label() noexcept {
            return "specialised";
        }
    };

    /**
     * @brief Fallback parameter pack for non-canonical (p_window_size, p_kmer_size).
     *
     * Stages branch on `generic_fallback` to read window/kmer from runtime params
     * instead of the type-level constants.
     */
    template <std::size_t p_sequence_length_log2> struct fallback_aligner_parameters {
        static constexpr std::size_t window_len = 0;
        static constexpr std::size_t kmer_len = 0;
        static constexpr std::size_t sequence_len_log2 = p_sequence_length_log2;
        static constexpr bool generic_fallback = true;

        using minimizer_repr_t =
            ::bioseq::minimizers::minimizer_representation<p_sequence_length_log2, 15u>;

        [[nodiscard]] static constexpr std::string_view label() noexcept {
            return "fallback(generic w/k)";
        }
    };

    /// Recognize a compile-time aligner parameter bundle.
    template <class p_type>
    concept is_constexpr_aligner_params = requires {
        { p_type::window_len } -> std::convertible_to<std::size_t>;
        { p_type::kmer_len } -> std::convertible_to<std::size_t>;
        { p_type::sequence_len_log2 } -> std::convertible_to<std::size_t>;
        { p_type::generic_fallback } -> std::convertible_to<bool>;
        typename p_type::minimizer_repr_t;
        { p_type::label() } -> std::convertible_to<std::string_view>;
    };

    static_assert(is_constexpr_aligner_params<static_aligner_parameters<10u, 15u, 28u>>);
    static_assert(is_constexpr_aligner_params<fallback_aligner_parameters<28u>>);

    /**
     * @brief Pipeline-builder entry-point tag.
     *
     * `with_prebuilt_minimizers` — pipeline accepts a `std::span<const p_minimizer>`
     * directly. The index_stage is at the head.
     * `from_sequence` — pipeline accepts the query sequence range and a
     * `minimizer_stage` is front-loaded to sketch on the fly.
     */
    enum class pipeline_entry_point : std::uint8_t {
        with_prebuilt_minimizers,
        from_sequence,
    };

    // =============================================================================
    // Concepts and stage skeleton
    // =============================================================================

    namespace detail {
    template <class p_sink_type>
    concept is_bare_stage = requires { typename p_sink_type::bare_stage_marker; };
    } // namespace detail

    /**
     * @brief Base tag inherited by every concrete stage.
     *
     * Lets `make_pipeline` distinguish bare-stage arguments from the terminal
     * sink via the `bare_stage_marker` typedef.
     */
    template <class p_derived> struct bare_stage {
        using bare_stage_marker = void;
    };

    // Forward declaration so stage_base::attach can refer to stage.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    struct stage;

    // =============================================================================
    // — narrow minimizer-batch shape carrying an optional device span.
    // =============================================================================
    //
    // One additional stage-shape on top of the  matrix. It borrows the host
    // minimizers from the arena slab and owns the optional GPU device minimizer
    // handle through `vulkan_device_minimizer_span::owner`, so the all-Vulkan
    // sketch+seed path avoids a second query-minimizer upload. CPU minimizer
    // kernels keep using the existing `std::span<const p_minimizer>` path.
    template <class p_minimizer> struct minimizer_batch_view {
        using value_type = p_minimizer;

        std::span<const p_minimizer> host{};
        std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device{};

        /// Return the first host minimizer, or null for an empty batch.
        [[nodiscard]] const p_minimizer* data() const noexcept {
            return host.data();
        }
        /// Return the number of host minimizers.
        [[nodiscard]] std::size_t size() const noexcept {
            return host.size();
        }
        /// View the host minimizers without transferring device ownership.
        [[nodiscard]] std::span<const p_minimizer> as_span() const noexcept {
            return host;
        }
        /// Return the host-range begin iterator.
        [[nodiscard]] auto begin() const noexcept {
            return host.begin();
        }
        /// Return the host-range end iterator.
        [[nodiscard]] auto end() const noexcept {
            return host.end();
        }
    };

    template <class p_type> struct is_minimizer_batch_view : std::false_type {};
    template <class p_minimizer_type>
    struct is_minimizer_batch_view<minimizer_batch_view<p_minimizer_type>> : std::true_type {};
    template <class p_type>
    inline constexpr bool is_minimizer_batch_view_v =
        is_minimizer_batch_view<std::remove_cvref_t<p_type>>::value;

    // =============================================================================
    // — Concept-driven sink validation.
    // =============================================================================

    /**
     * @brief Single-batch sink concept.  Required by emit_token::emit_batch.
     *
     * Matched by every concrete sink that consumes a `std::span<p_element>` (most
     * pipeline tails).  Substitution failures inside `emit_token::emit_batch`
     * name the concept and the missing method.
     */
    template <class p_sink_type, class p_element>
    concept batch_sink = requires(p_sink_type& sink, std::span<p_element> batch) {
        { sink.on_batch(batch) } -> std::same_as<void>;
        { sink.on_completed() } -> std::same_as<void>;
    };

    /**
     * @brief Pair-batch sink concept.  Required by emit_token::emit_batch_pair.
     *
     * The pair shape is used by the chain → region → primary/secondary →
     * filter → divergence → align cascade where each stage forwards both the
     * regions span and the (read-only) anchors span downstream.
     */
    template <class p_sink_type, class p_first_element, class p_second_element>
    concept pair_batch_sink =
        requires(p_sink_type& sink, std::span<p_first_element> a, std::span<p_second_element> b) {
            { sink.on_batch_pair(a, b) } -> std::same_as<void>;
            { sink.on_completed() } -> std::same_as<void>;
        };

    /**
     * @brief Index-decorated batch sink concept.  Required by
     * emit_token::emit_batch_with_index.
     *
     * Used by index_stage to hand a sketches span downstream alongside a
     * (const-reference) target index.  seed_hit_stage is the canonical consumer.
     */
    template <class p_sink_type, class p_element, class p_index>
    concept indexed_batch_sink = requires(p_sink_type& sink,
                                          std::span<p_element> a,
                                          const p_index& idx,
                                          bioseq::indexing::gpu_index_cache_identity identity) {
        { sink.on_batch_with_index(a, idx, identity) } -> std::same_as<void>;
        { sink.on_completed() } -> std::same_as<void>;
    };

    /// — minimizer-batch sink (minimizer_stage → index_stage hand-off).
    template <class p_sink_type, class p_minimizer>
    concept minimizer_batch_sink =
        requires(p_sink_type& sink, minimizer_batch_view<p_minimizer> batch) {
            { sink.on_minimizer_batch(batch) } -> std::same_as<void>;
            { sink.on_completed() } -> std::same_as<void>;
        };

    /// — index-decorated minimizer-batch sink (index_stage →
    /// seed_hit_stage hand-off carrying the GPU index cache identity).
    template <class p_sink_type, class p_minimizer, class p_index>
    concept indexed_minimizer_batch_sink =
        requires(p_sink_type& sink,
                 minimizer_batch_view<p_minimizer> batch,
                 const p_index& idx,
                 bioseq::indexing::gpu_index_cache_identity identity) {
            { sink.on_minimizer_batch_with_index(batch, idx, identity) } -> std::same_as<void>;
            { sink.on_completed() } -> std::same_as<void>;
        };

    // =============================================================================
    // — Compile-time single-emission enforcement (emit_token /
    // emitted_token).
    // =============================================================================

    template <class p_sink> class emitted_token;

    /**
     * @brief Move-only, single-use, friend-constructible-only token granting
     * downstream-emission permission.  The `emit_batch*` family of
     * rvalue-qualified methods consumes the token (nulling its internal
     * pointer) and returns the friend-only-constructible `emitted_token<p_sink>`
     * stats.
     *
     * The token's only constructor is private. Only `stage<p_aligner_parameters, p_self, p_sink>`
     * is befriended, which means concrete-stage `process*<p_aligner_parameters>` bodies see
     * the token by rvalue reference but cannot manufacture one themselves —
     * the type system rules out skipping the emit (no way to construct the
     * required `emitted_token<p_sink>` return value).
     *
     * Single-emission enforcement:
     *   - At least once: HARD compile-time.  `process*<p_aligner_parameters>` must return
     *     `emitted_token<p_sink>`. The only way to construct one is to call
     *     `emit_token<p_sink>::emit_batch*`.
     *   - At most once: soft compile-time + runtime trip-wire.  `[[nodiscard]]`
     *     makes the ergonomic shape `return std::move(token).emit_batch*(...)`.
     *     emitting twice trips the `DEBUG_ASSERT(sink_ptr_ != nullptr)` in debug.
     */
    template <class p_sink> class emit_token {
      public:
        template <is_constexpr_aligner_params p_aligner_parameters,
                  class p_self,
                  class p_stage_sink>
        friend struct stage;

        emit_token() = delete;
        emit_token(const emit_token&) = delete;
        emit_token(emit_token&&) = delete;
        emit_token& operator=(const emit_token&) = delete;
        emit_token& operator=(emit_token&&) = delete;

        /// Emit one ordinary batch and consume this permission token.
        template <class p_element>
        [[nodiscard, gnu::always_inline]]
        emitted_token<p_sink> emit_batch(std::span<p_element> data) && {
            static_assert(batch_sink<p_sink, p_element>,
                          "emit_token::emit_batch: downstream sink does not implement "
                          "on_batch(std::span<p_element>). See stage-shape matrix.");
            DEBUG_ASSERT(sink_ptr_ != nullptr,
                         "emit_token consumed twice; the first emit_batch* call nulled the "
                         "sink pointer is no longer available.");
            sink_ptr_->on_batch(data);
            sink_ptr_ = nullptr;
            return emitted_token<p_sink>::make();
        }

        /// Emit one paired batch and consume this permission token.
        template <class p_first_element, class p_second_element>
        [[nodiscard, gnu::always_inline]]
        emitted_token<p_sink> emit_batch_pair(std::span<p_first_element> first_batch,
                                              std::span<p_second_element> second_batch) && {
            static_assert(
                pair_batch_sink<p_sink, p_first_element, p_second_element>,
                "emit_token::emit_batch_pair: downstream sink does not implement "
                "on_batch_pair(std::span<p_first_element>, std::span<p_second_element>).");
            DEBUG_ASSERT(sink_ptr_ != nullptr, "emit_token consumed twice.");
            sink_ptr_->on_batch_pair(first_batch, second_batch);
            sink_ptr_ = nullptr;
            return emitted_token<p_sink>::make();
        }

        /// Emit an index-decorated batch and consume this permission token.
        template <class p_element, class p_index>
        [[nodiscard, gnu::always_inline]]
        emitted_token<p_sink>
        emit_batch_with_index(std::span<p_element> data,
                              const p_index& index_ref,
                              bioseq::indexing::gpu_index_cache_identity identity) && {
            static_assert(
                indexed_batch_sink<p_sink, p_element, p_index>,
                "emit_token::emit_batch_with_index: downstream sink does not "
                "implement on_batch_with_index(std::span<p_element>, p_index const&, identity).");
            DEBUG_ASSERT(sink_ptr_ != nullptr, "emit_token consumed twice.");
            sink_ptr_->on_batch_with_index(data, index_ref, identity);
            sink_ptr_ = nullptr;
            return emitted_token<p_sink>::make();
        }

        // — narrow minimizer-batch emit (carries optional device span).
        /// Emit a host/device minimizer batch and consume this permission token.
        template <class p_minimizer>
        [[nodiscard, gnu::always_inline]]
        emitted_token<p_sink> emit_minimizer_batch(minimizer_batch_view<p_minimizer> batch) && {
            static_assert(minimizer_batch_sink<p_sink, p_minimizer>,
                          "emit_token::emit_minimizer_batch: downstream sink does not implement "
                          "on_minimizer_batch.");
            DEBUG_ASSERT(sink_ptr_ != nullptr, "emit_token consumed twice.");
            sink_ptr_->on_minimizer_batch(batch);
            sink_ptr_ = nullptr;
            return emitted_token<p_sink>::make();
        }

        /// Emit an index-decorated host/device batch and consume this token.
        template <class p_minimizer, class p_index>
        [[nodiscard, gnu::always_inline]]
        emitted_token<p_sink>
        emit_minimizer_batch_with_index(minimizer_batch_view<p_minimizer> batch,
                                        const p_index& index_ref,
                                        bioseq::indexing::gpu_index_cache_identity identity) && {
            static_assert(indexed_minimizer_batch_sink<p_sink, p_minimizer, p_index>,
                          "emit_token::emit_minimizer_batch_with_index: downstream sink does not "
                          "implement on_minimizer_batch_with_index.");
            DEBUG_ASSERT(sink_ptr_ != nullptr, "emit_token consumed twice.");
            sink_ptr_->on_minimizer_batch_with_index(batch, index_ref, identity);
            sink_ptr_ = nullptr;
            return emitted_token<p_sink>::make();
        }

      private:
        p_sink* sink_ptr_;
        explicit constexpr emit_token(p_sink& sink_ref) noexcept : sink_ptr_(&sink_ref) {}
    };

    /**
     * @brief Stats-of-emission token.  Returned by
     * `emit_token<p_sink>::emit_batch*`. Every concrete stage's `process*<p_aligner_parameters>`
     * must return one.  The only path to constructing one is the
     * friend-accessible `make()` factory.
     */
    template <class p_sink> class [[nodiscard]] emitted_token {
      public:
        emitted_token(const emitted_token&) = delete;
        emitted_token& operator=(const emitted_token&) = delete;
        emitted_token& operator=(emitted_token&&) = delete;

        /// Move constructor exists only for mandatory-copy-elision worst-case
        /// materialisation.  The canonical path is direct-return from
        /// emit_batch*, where C++17 copy elision skips it entirely.
        emitted_token(emitted_token&&) noexcept = default;

      private:
        friend class emit_token<p_sink>;
        /// Friend-only default constructor — the public surface is
        /// non-default-constructible because the only call site of this
        /// ctor lives behind the `make()` factory below.
        constexpr emitted_token() noexcept = default;
        [[nodiscard, gnu::always_inline]] static constexpr emitted_token make() noexcept {
            return emitted_token{};
        }
    };

    // =============================================================================
    // — Input-side kernel-to-arena adapter.
    // =============================================================================

    /**
     * @brief Drop-in `basic_sketch_collector<p_element>`-shaped adapter that routes the
     * kernel's `collect(...)` traffic into a caller-provided
     * `arena_slab<p_element>`.  Implements the minimal surface the kernels rely on
     * (`collect(p_element&&)`, `collect(const p_element&)`, `reserve(size_t)`).
     *
     * `arena_slab<p_element>::push_back` requires `std::is_trivially_copyable_v<p_element>` —
     * satisfied by every `minimizer_representation<p_bucket_width, p_kmer_size>`.  The existing
     * slab `arena_slab`'s `static_assert` covers the constraint and would fire if a non-trivial
     * minimizer type were ever introduced.
     */
    template <class p_element> struct sketch_collector_arena_adapter {
        using minimizer_type = p_element;

        ::bioseq::chaining::arena_slab<p_element>* slab_;

        /// Append one movable minimizer to the arena slab.
        [[gnu::always_inline]] void collect(p_element sketch) {
            slab_->push_back(std::move(sketch));
        }
        /// Append one borrowed minimizer to the arena slab.
        [[gnu::always_inline]] void collect(const p_element& sketch) {
            slab_->push_back(sketch);
        }
        /// Reserve space for the caller's conservative output estimate.
        [[gnu::always_inline]] void reserve(std::size_t expected_count) {
            slab_->reserve(expected_count);
        }
        /// Return the number of constructed minimizers.
        [[nodiscard, gnu::always_inline]] std::size_t size() const noexcept {
            return slab_->size();
        }

        /// Append a trivially copyable batch without per-record dispatch.
        [[nodiscard, gnu::always_inline]] bool
        append_records(const std::span<const p_element> records) {
            return slab_->append_trivial_records(records);
        }

        /// Append exact raw storage when its layout matches the minimizer type.
        template <class p_minimizer_type = p_element>
        [[nodiscard, gnu::always_inline]] bool
        append_raw_storage(const typename p_minimizer_type::storage_type* raw, std::size_t count)
            requires(std::same_as<p_minimizer_type, p_element> &&
                     requires { typename p_minimizer_type::storage_type; } &&
                     std::is_trivially_copyable_v<p_minimizer_type> &&
                     sizeof(p_minimizer_type) == sizeof(typename p_minimizer_type::storage_type))
        {
            return slab_->append_trivial_storage(
                std::span<const typename p_minimizer_type::storage_type>{raw, count});
        }
    };

    /**
     * @brief CRTP helper absorbing the boilerplate shared by every concrete stage.
     *
     * `p_derived` must define `static constexpr std::string_view stage_name`.  The
     * default `attach<p_aligner_parameters, p_sink>(...)` returns a bound
     * `stage<p_aligner_parameters, p_derived, p_sink>` value owning `*this` and the downstream
     * sink.
     */
    template <class p_derived, class p_output> struct stage_base : bare_stage<p_derived> {
        using out_element_type = p_output;

        /// Bind this stage to its downstream sink and parameter bundle.
        template <is_constexpr_aligner_params p_aligner_parameters, class p_sink>
        [[gnu::always_inline]] auto attach(p_sink sink) noexcept {
            return stage<p_aligner_parameters, p_derived, p_sink>{
                std::move(sink), std::move(static_cast<p_derived&>(*this))};
        }

        /// Complete a stage that has no terminal action of its own.
        template <class p_sink> [[gnu::always_inline]] void finalize(p_sink&) noexcept {
            /* default: no-op */
        }
    };

    /**
     * @brief Bound stage — owns its downstream sink and bare-stage config by value.
     *
     * `p_aligner_parameters` is fixed at `make_pipeline<p_aligner_parameters>(...)` time and
     * identical across every stage in a given pipeline instance.  The bound stage's `on_batch*`
     * members forward to the bare stage's `process*<p_aligner_parameters>` body. Both are
     * `[[gnu::always_inline]]` so the chain collapses to a straight-line sequence
     * of kernel calls at `-O3`.
     */
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    struct stage {
        using cep_t = p_aligner_parameters;

        p_sink sink_;
        p_self self_;

        /// Own one configured stage and its downstream sink.
        constexpr stage(p_sink sink_arg, p_self self_arg) noexcept
            : sink_(std::move(sink_arg)), self_(std::move(self_arg)) {}

        //: the bound stage constructs an `emit_token<p_sink>` on its
        // own stack and hands it to `process*<p_aligner_parameters>` by rvalue reference; the
        // concrete-stage body returns the (discarded) `emitted_token<p_sink>` stats
        // type whose construction is friend-private to `emit_token<p_sink>`.
        /// Forward one ordinary batch through the bound stage.
        template <class p_batch> [[gnu::always_inline]] void on_batch(p_batch&& batch) {
            (void)self_.template process<p_aligner_parameters>(std::forward<p_batch>(batch),
                                                               emit_token<p_sink>{sink_});
        }

        /// Forward one paired batch through the bound stage.
        template <class p_first_batch, class p_second_batch>
        [[gnu::always_inline]] void on_batch_pair(p_first_batch&& first_batch,
                                                  p_second_batch&& second_batch) {
            (void)self_.template process_pair<p_aligner_parameters>(
                std::forward<p_first_batch>(first_batch),
                std::forward<p_second_batch>(second_batch),
                emit_token<p_sink>{sink_});
        }

        /// Forward one index-decorated batch through the bound stage.
        template <class p_batch, class p_index>
        [[gnu::always_inline]] void
        on_batch_with_index(p_batch&& batch,
                            const p_index& index_ref,
                            bioseq::indexing::gpu_index_cache_identity identity) {
            (void)self_.template process_with_index<p_aligner_parameters>(
                std::forward<p_batch>(batch), index_ref, identity, emit_token<p_sink>{sink_});
        }

        // — forwarders for the narrow minimizer-batch shape.
        /// Forward one host/device minimizer batch through the bound stage.
        template <class p_minimizer>
        [[gnu::always_inline]] void on_minimizer_batch(minimizer_batch_view<p_minimizer> batch) {
            (void)self_.template process<p_aligner_parameters>(batch, emit_token<p_sink>{sink_});
        }

        /// Forward one index-decorated host/device batch through the bound stage.
        template <class p_minimizer, class p_index>
        [[gnu::always_inline]] void
        on_minimizer_batch_with_index(minimizer_batch_view<p_minimizer> batch,
                                      const p_index& index_ref,
                                      bioseq::indexing::gpu_index_cache_identity identity) {
            (void)self_.template process_with_index<p_aligner_parameters>(
                batch, index_ref, identity, emit_token<p_sink>{sink_});
        }

        /// Finalize this stage before completing its downstream sink.
        [[gnu::always_inline]] void on_completed() noexcept {
            self_.finalize(sink_);
            sink_.on_completed();
        }

        [[nodiscard]] pipeline_description describe() const;
    };

    /**
     * @brief Variadic pipeline composer — fixes `p_aligner_parameters` once for the whole chain.
     *
     * The base case (terminal sink) returns the sink unchanged. The recursive case
     * peels one bare stage off the front, builds the downstream pipeline from the
     * remaining arguments, and attaches.
     */
    /// Terminate pipeline composition at the supplied sink.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_sink>
    [[gnu::always_inline]] constexpr auto make_pipeline(p_sink sink) noexcept {
        static_assert(!detail::is_bare_stage<p_sink>,
                      "make_pipeline's last argument must be a terminal sink, "
                      "not a bare_stage.  Append your terminal sink at the end.");
        return sink;
    }

    /// Attach one stage to the recursively composed downstream pipeline.
    template <is_constexpr_aligner_params p_aligner_parameters,
              class p_first_stage,
              class... p_remaining_stages>
        requires detail::is_bare_stage<p_first_stage>
    [[gnu::always_inline]] constexpr auto make_pipeline(p_first_stage head,
                                                        p_remaining_stages... rest) noexcept {
        auto downstream = make_pipeline<p_aligner_parameters>(std::move(rest)...);
        return std::move(head).template attach<p_aligner_parameters>(std::move(downstream));
    }

    // =============================================================================
    // Batch storage — owning arena slab
    // =============================================================================
    //
    // `arena_slab<p_element>` is owned by `bioseq.chaining.core` so chaining,
    // seed, and region kernels share one caller-owned output contract. The using
    // declarations above expose the slab and its factory to stage bodies.

    } // namespace bioseq::simd::pipe

} // export
