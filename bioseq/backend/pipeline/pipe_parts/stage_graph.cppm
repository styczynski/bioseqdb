/**
 * @file pipeline/pipe_parts/stage_graph.cppm
 * @brief Connects alignment stages and passes batches between them.
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
    // Make the shared index types and requirements available to stages.
    using bioseq::indexing::minimizer_hashmap_fragment_index;
    using bioseq::indexing::minimizer_lookup_index;
    using bioseq::indexing::minimizer_seed_index;

    // Export these error types so PostgreSQL callers can catch them through
    // the pipeline module without importing the SIMD implementations.
    using exact_simd_runtime_error = ::bioseq::minimizers::exact_simd_runtime_error;
    using simd_chaining_runtime_error = ::bioseq::chaining::simd_chaining_runtime_error;
    using simd_chaining_status = ::bioseq::chaining::simd_chaining_status;

    /// Reports a rejected bounded-ntHash request. PostgreSQL callers catch this
    /// C++ exception before raising ERROR, so C++ cleanup can finish first.
    class bounded_nthash_runtime_error final : public std::runtime_error {
      public:
        /// Add the bounded-ntHash algorithm name to the rejection details.
        explicit bounded_nthash_runtime_error(std::string detail)
            : std::runtime_error(std::string{"bounded_nthash_simd minimizer execution rejected: "} +
                                 std::move(detail)) {}
    };

    // Store pipeline lengths and positions in the 64-bit sequence_size_t.
    using bioseq::storage::sequence_size_t;

    using host_stage_clock = std::chrono::steady_clock;

    /// Read the steady clock in nanoseconds, with a minimum value of one.
    [[nodiscard]] inline std::uint64_t host_stage_now_ns() noexcept {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             host_stage_clock::now().time_since_epoch())
                             .count();
        return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
    }

    /// Measure elapsed nanoseconds since started_ns, with a minimum of one.
    [[nodiscard]] inline std::uint64_t
    elapsed_host_stage_ns(const std::uint64_t started_ns) noexcept {
        const std::uint64_t finished_ns = host_stage_now_ns();
        const std::uint64_t elapsed = finished_ns >= started_ns ? finished_ns - started_ns : 0u;
        // Use one nanosecond when the clock cannot distinguish the two samples,
        // so a completed stage still has a positive recorded duration.
        return elapsed > 0 ? elapsed : 1u;
    }

    /// Throw the reported Vulkan sketch error, or a general decline if none is set.
    [[noreturn]] inline void
    throw_vulkan_sketch_declined(std::string_view stage,
                                 const bioseq::gpu::vulkan::vulkan_sketch_decline& decline) {
        if (!decline.declined()) {
            throw_vulkan_declined(stage);
        }
        bioseq::gpu::vulkan::throw_vulkan_strict_decline(stage, decline);
    }

    // Require the C++23 build mode used by these pipeline templates.
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

    // Parameters shared by the stages of a compiled pipeline.

    /**
     * @brief Fix sketch parameters and record storage for a compiled pipeline.
     *
     * @tparam p_window_size Positive minimizer window size.
     * @tparam p_runtime_kmer_size K-mer length used by the algorithm.
     * @tparam p_sequence_length_log2 Selected position-width bucket.
     * @tparam p_physical_kmer_size K used to choose the record format. A
     *         fixed-32-bit hash can use the K15 format while sketching k=20.
     */
    template <std::size_t p_window_size,
              std::size_t p_runtime_kmer_size,
              std::size_t p_sequence_length_log2,
              std::size_t p_physical_kmer_size = p_runtime_kmer_size>
    struct static_aligner_parameters {
        static constexpr std::size_t window_len = p_window_size;
        static constexpr std::size_t kmer_len = p_runtime_kmer_size;
        static constexpr std::size_t physical_kmer_len = p_physical_kmer_size;
        static constexpr std::size_t sequence_len_log2 = p_sequence_length_log2;
        static constexpr bool generic_fallback = false;

        using minimizer_repr_t =
            ::bioseq::minimizers::minimizer_representation<p_sequence_length_log2,
                                                         p_physical_kmer_size>;

        [[nodiscard]] static constexpr std::string_view label() noexcept {
            return "specialised";
        }
    };

    /**
     * @brief Select record storage while reading k and w at runtime.
     *
     * generic_fallback tells stages to use the request's k-mer and window
     * sizes. The zero constants here are placeholders.
     */
    template <std::size_t p_sequence_length_log2,
              std::size_t p_physical_kmer_size = 15u>
    struct fallback_aligner_parameters {
        static constexpr std::size_t window_len = 0;
        static constexpr std::size_t kmer_len = 0;
        static constexpr std::size_t physical_kmer_len = p_physical_kmer_size;
        static constexpr std::size_t sequence_len_log2 = p_sequence_length_log2;
        static constexpr bool generic_fallback = true;

        using minimizer_repr_t =
            ::bioseq::minimizers::minimizer_representation<p_sequence_length_log2,
                                                         p_physical_kmer_size>;

        [[nodiscard]] static constexpr std::string_view label() noexcept {
            return "fallback(generic w/k)";
        }
    };

    /// Require the parameter fields and minimizer type expected by pipeline stages.
    template <class p_type>
    concept is_constexpr_aligner_params = requires {
        { p_type::window_len } -> std::convertible_to<std::size_t>;
        { p_type::kmer_len } -> std::convertible_to<std::size_t>;
        { p_type::physical_kmer_len } -> std::convertible_to<std::size_t>;
        { p_type::sequence_len_log2 } -> std::convertible_to<std::size_t>;
        { p_type::generic_fallback } -> std::convertible_to<bool>;
        typename p_type::minimizer_repr_t;
        { p_type::label() } -> std::convertible_to<std::string_view>;
    };

    static_assert(is_constexpr_aligner_params<static_aligner_parameters<10u, 15u, 28u>>);
    static_assert(is_constexpr_aligner_params<fallback_aligner_parameters<28u>>);
    static_assert(std::same_as<
        static_aligner_parameters<15u, 20u, 28u, 15u>::minimizer_repr_t,
        ::bioseq::minimizers::minimizer_representation<28u, 15u>>);
    static_assert(static_aligner_parameters<15u, 20u, 28u, 15u>::kmer_len == 20u);
    static_assert(std::same_as<
        fallback_aligner_parameters<23u, 20u>::minimizer_repr_t,
        ::bioseq::minimizers::minimizer_representation<23u, 20u>>);

    /**
     * @brief Choose whether the pipeline receives a sketch or a sequence.
     *
     * with_prebuilt_minimizers starts at index lookup with an owned minimizer
     * batch. from_sequence adds a minimizer stage to sketch the query first.
     */
    enum class pipeline_entry_point : std::uint8_t {
        with_prebuilt_minimizers,
        from_sequence,
    };

    // Stage interfaces and composition helpers.

    namespace detail {
    template <class p_sink_type>
    concept is_bare_stage = requires { typename p_sink_type::bare_stage_marker; };
    } // namespace detail

    /**
     * @brief Marks a stage that has not yet been attached to a downstream sink.
     *
     * make_pipeline uses this marker to distinguish stages from the final sink.
     */
    template <class p_derived> struct bare_stage {
        using bare_stage_marker = void;
    };

    // Forward declaration so stage_base::attach can refer to stage.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    struct stage;

    /// Keeps a checked range of host minimizers and an optional device copy.
    /// Copies share the host vector and device owner, so passing a batch through
    /// stages keeps its backing storage alive. Spans returned by as_span borrow
    /// from that host vector.
    template <class p_minimizer> class minimizer_batch_view {
        std::shared_ptr<const std::vector<p_minimizer>> host_owner_;
        std::size_t offset_;
        std::size_t count_;

        minimizer_batch_view(
            std::shared_ptr<const std::vector<p_minimizer>> owner,
            const std::size_t offset,
            const std::size_t count,
            std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device_span)
            : host_owner_(std::move(owner)), offset_(offset), count_(count),
              device(std::move(device_span)) {}

      public:
        using value_type = p_minimizer;

        std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device{};

        /// Share an in-bounds vector range without copying its minimizers.
        [[nodiscard]] static minimizer_batch_view from_shared_host(
            std::shared_ptr<const std::vector<p_minimizer>> owner,
            const std::size_t offset,
            const std::size_t count,
            std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
                device_span = std::nullopt) {
            if (!owner || offset > owner->size() || count > owner->size() - offset) {
                throw std::invalid_argument("minimizer batch requires an owned in-bounds host range");
            }
            return minimizer_batch_view{
                std::move(owner), offset, count, std::move(device_span)};
        }

        /// Check whether this batch still keeps its host vector alive.
        [[nodiscard]] bool has_host_owner() const noexcept {
            return static_cast<bool>(host_owner_);
        }
        /// Return the host range's data pointer. Use size() before dereferencing it.
        [[nodiscard]] const p_minimizer* data() const noexcept {
            return as_span().data();
        }
        /// Return the number of host minimizers.
        [[nodiscard]] std::size_t size() const noexcept {
            return as_span().size();
        }
        /// Borrow the host range while this batch keeps its storage alive.
        [[nodiscard]] std::span<const p_minimizer> as_span() const noexcept {
            return host_owner_
                ? std::span<const p_minimizer>{*host_owner_}.subspan(offset_, count_)
                : std::span<const p_minimizer>{};
        }
        /// Return the beginning of the host range.
        [[nodiscard]] auto begin() const noexcept {
            return as_span().begin();
        }
        /// Return the end of the host range.
        [[nodiscard]] auto end() const noexcept {
            return as_span().end();
        }
    };

    template <class p_type> struct is_minimizer_batch_view : std::false_type {};
    template <class p_minimizer_type>
    struct is_minimizer_batch_view<minimizer_batch_view<p_minimizer_type>> : std::true_type {};
    template <class p_type>
    inline constexpr bool is_minimizer_batch_view_v =
        is_minimizer_batch_view<std::remove_cvref_t<p_type>>::value;

    // Check that downstream sinks accept each kind of batch.

    /**
     * @brief Require a sink that accepts one span and a completion callback.
     * emit_token::emit_batch checks this before calling the downstream sink.
     */
    template <class p_sink_type, class p_element>
    concept batch_sink = requires(p_sink_type& sink, std::span<p_element> batch) {
        { sink.on_batch(batch) } -> std::same_as<void>;
        { sink.on_completed() } -> std::same_as<void>;
    };

    /**
     * @brief Require a sink that accepts two spans together.
     * Chaining and alignment stages use this to pass regions alongside their
     * anchor data. The sink must also accept a completion callback.
     */
    template <class p_sink_type, class p_first_element, class p_second_element>
    concept pair_batch_sink =
        requires(p_sink_type& sink, std::span<p_first_element> a, std::span<p_second_element> b) {
            { sink.on_batch_pair(a, b) } -> std::same_as<void>;
            { sink.on_completed() } -> std::same_as<void>;
        };

    /**
     * @brief Require a sink that accepts a span with a target index.
     * The call also carries the GPU index-cache identity. Completion uses
     * the same callback as other batch sinks.
     */
    template <class p_sink_type, class p_element, class p_index>
    concept indexed_batch_sink = requires(p_sink_type& sink,
                                          std::span<p_element> a,
                                          const p_index& idx,
                                          bioseq::indexing::gpu_index_cache_identity identity) {
        { sink.on_batch_with_index(a, idx, identity) } -> std::same_as<void>;
        { sink.on_completed() } -> std::same_as<void>;
    };

    /// Accept an owned minimizer batch from the sketch stage, then completion.
    template <class p_sink_type, class p_minimizer>
    concept minimizer_batch_sink =
        requires(p_sink_type& sink, minimizer_batch_view<p_minimizer> batch) {
            { sink.on_minimizer_batch(batch) } -> std::same_as<void>;
            { sink.on_completed() } -> std::same_as<void>;
        };

    /// Accept an owned minimizer batch with a target index and GPU cache identity.
    /// The sink must also handle completion.
    template <class p_sink_type, class p_minimizer, class p_index>
    concept indexed_minimizer_batch_sink =
        requires(p_sink_type& sink,
                 minimizer_batch_view<p_minimizer> batch,
                 const p_index& idx,
                 bioseq::indexing::gpu_index_cache_identity identity) {
            { sink.on_minimizer_batch_with_index(batch, idx, identity) } -> std::same_as<void>;
            { sink.on_completed() } -> std::same_as<void>;
        };

    // Tokens track whether a stage forwarded its output.

    template <class p_sink> class emitted_token;

    /**
     * @brief Lets a stage forward one batch to its downstream sink.
     *
     * Only stage can construct this token. Its process function receives an
     * rvalue reference and returns the emitted_token produced by an emit call.
     * The emit call clears the sink pointer, and debug assertions catch reuse.
     * The token itself can be neither copied nor moved.
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

        /// Forward one span and mark this token as used.
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

        /// Forward two spans together and mark this token as used.
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

        /// Forward a span with its target index and GPU cache identity.
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

        // Minimizer batches can keep an optional device buffer.
        /// Forward an owned host/device sketch and mark this token as used.
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

        /// Forward an owned sketch with its target index and GPU cache identity.
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
     * @brief Return value showing that a stage forwarded a batch.
     * Only emit_token can construct it. A stage's process function returns
     * this value after calling one of the emit methods.
     */
    template <class p_sink> class [[nodiscard]] emitted_token {
      public:
        emitted_token(const emitted_token&) = delete;
        emitted_token& operator=(const emitted_token&) = delete;
        emitted_token& operator=(emitted_token&&) = delete;

        /// Allow a completed emission result to be returned by move when needed.
        emitted_token(emitted_token&&) noexcept = default;

      private:
        friend class emit_token<p_sink>;
        /// Create an emission result only through emit_token's private access.
        constexpr emitted_token() noexcept = default;
        [[nodiscard, gnu::always_inline]] static constexpr emitted_token make() noexcept {
            return emitted_token{};
        }
    };

    // Collect kernel output directly into an arena slab.

    /**
     * @brief Write minimizers directly into the caller's chaining arena slab.
     *
     * Implements the collector methods used by sketch kernels, including
     * batch appends. Elements must be trivially copyable because the slab
     * copies their storage when it grows.
     */
    template <class p_element> struct sketch_collector_arena_adapter {
        using minimizer_type = p_element;

        ::bioseq::chaining::arena_slab<p_element>* slab_;

        /// Append a minimizer value to the slab.
        [[gnu::always_inline]] void collect(p_element sketch) {
            slab_->push_back(std::move(sketch));
        }
        /// Copy a minimizer into the slab.
        [[gnu::always_inline]] void collect(const p_element& sketch) {
            slab_->push_back(sketch);
        }
        /// Reserve space for at least expected_count minimizers.
        [[gnu::always_inline]] void reserve(std::size_t expected_count) {
            slab_->reserve(expected_count);
        }
        /// Return the number of constructed minimizers.
        [[nodiscard, gnu::always_inline]] std::size_t size() const noexcept {
            return slab_->size();
        }

        /// Append a batch by copying its records together.
        [[nodiscard, gnu::always_inline]] bool
        append_records(const std::span<const p_element> records) {
            return slab_->append_trivial_records(records);
        }

        /// Append raw record words when their size matches the minimizer type.
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
     * @brief Adds attachment and default completion handling to a stage.
     *
     * The derived stage supplies stage_name and its process methods. attach
     * moves it into a bound stage together with the downstream sink.
     */
    template <class p_derived, class p_output> struct stage_base : bare_stage<p_derived> {
        using out_element_type = p_output;

        /// Move this stage and its downstream sink into a bound pipeline stage.
        template <is_constexpr_aligner_params p_aligner_parameters, class p_sink>
        [[gnu::always_inline]] auto attach(p_sink sink) noexcept {
            return stage<p_aligner_parameters, p_derived, p_sink>{
                std::move(sink), std::move(static_cast<p_derived&>(*this))};
        }

        /// Default completion hook for stages with no final work.
        template <class p_sink> [[gnu::always_inline]] void finalize(p_sink&) noexcept {
            /* default: no-op */
        }
    };

    /**
     * @brief Owns a configured stage and the sink that receives its output.
     *
     * Every stage in the pipeline shares p_aligner_parameters. Batch callbacks
     * call the configured stage's process methods, which forward results through
     * an emit_token. Template composition lets the compiler inline these calls.
     */
    template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
    struct stage {
        using cep_t = p_aligner_parameters;

        p_sink sink_;
        p_self self_;

        /// Take ownership of a configured stage and its downstream sink.
        constexpr stage(p_sink sink_arg, p_self self_arg) noexcept
            : sink_(std::move(sink_arg)), self_(std::move(self_arg)) {}

        // Give each process call a fresh token, then discard the returned
        // marker that records the forwarded output.
        /// Pass one batch to this stage's process method.
        template <class p_batch> [[gnu::always_inline]] void on_batch(p_batch&& batch) {
            (void)self_.template process<p_aligner_parameters>(std::forward<p_batch>(batch),
                                                               emit_token<p_sink>{sink_});
        }

        /// Pass two related batches to this stage's process_pair method.
        template <class p_first_batch, class p_second_batch>
        [[gnu::always_inline]] void on_batch_pair(p_first_batch&& first_batch,
                                                  p_second_batch&& second_batch) {
            (void)self_.template process_pair<p_aligner_parameters>(
                std::forward<p_first_batch>(first_batch),
                std::forward<p_second_batch>(second_batch),
                emit_token<p_sink>{sink_});
        }

        /// Process a batch with its target index and GPU cache identity.
        template <class p_batch, class p_index>
        [[gnu::always_inline]] void
        on_batch_with_index(p_batch&& batch,
                            const p_index& index_ref,
                            bioseq::indexing::gpu_index_cache_identity identity) {
            (void)self_.template process_with_index<p_aligner_parameters>(
                std::forward<p_batch>(batch), index_ref, identity, emit_token<p_sink>{sink_});
        }

        // Forward minimizer batches while keeping their backing storage alive.
        /// Pass an owned host/device sketch to this stage.
        template <class p_minimizer>
        [[gnu::always_inline]] void on_minimizer_batch(minimizer_batch_view<p_minimizer> batch) {
            (void)self_.template process<p_aligner_parameters>(batch, emit_token<p_sink>{sink_});
        }

        /// Process an owned sketch with its target index and GPU cache identity.
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
     * @brief Connect stages in order, using one parameter type throughout.
     *
     * The final argument is the result sink. Build the downstream chain first,
     * then attach each preceding stage to it.
     */
    /// Finish composition at the terminal sink. A stage cannot be the last argument.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_sink>
    [[gnu::always_inline]] constexpr auto make_pipeline(p_sink sink) noexcept {
        static_assert(!detail::is_bare_stage<p_sink>,
                      "make_pipeline's last argument must be a terminal sink, "
                      "not a bare_stage.  Append your terminal sink at the end.");
        return sink;
    }

    /// Attach this stage to the pipeline built from the remaining arguments.
    template <is_constexpr_aligner_params p_aligner_parameters,
              class p_first_stage,
              class... p_remaining_stages>
        requires detail::is_bare_stage<p_first_stage>
    [[gnu::always_inline]] constexpr auto make_pipeline(p_first_stage head,
                                                        p_remaining_stages... rest) noexcept {
        auto downstream = make_pipeline<p_aligner_parameters>(std::move(rest)...);
        return std::move(head).template attach<p_aligner_parameters>(std::move(downstream));
    }

    // Stages share arena_slab from bioseq.chaining.core for seed, chain, and
    // region output. The aliases above expose it and its factory here.

    } // namespace bioseq::simd::pipe

} // export
