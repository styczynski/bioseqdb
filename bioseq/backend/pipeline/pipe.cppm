/**
 * @file pipe.cppm
 * @brief Compile-time dispatched sequence-alignment pipeline.
 *
 * This module implements the compile-time dispatched stage graph used by the
 * scheduler-facing mapping entry points. It sits above kernel functions in
 * the target-neutral `bioseq::simd` namespace and provides:
 *
 *   - Compile-time parameter packs (`static_aligner_parameters<p_window_size,p_kmer_size,p_bucket_width>` /
 *     `fallback_aligner_parameters<p_bucket_width>`).
 *   - stage skeletons (`bare_stage<p_self>`, `stage_base<p_self,p_output>`,
 *     `stage<p_aligner_parameters,p_self,p_sink>`).
 *   - `make_pipeline<p_aligner_parameters>` variadic composer.
 *   - Concrete stages — every pipeline stage exposed as a stand-alone struct
 *     whose `process<p_aligner_parameters>` body forwards to the existing kernel function
 *     (collect_seed_hits, chain_anchors_*, generate_regions, etc.).
 *   - Top-level `dispatch_pipeline(...)` cascade that fixes (p_aligner_parameters, p_alignment_algorithm, p_chaining_algorithm,
 *     p_minimizer_algorithm, p_entry_point) at compile time before driving the pipeline.
 *   - `pipeline_description` metadata surface for the Postgres EXPLAIN
 *     renderer.
 *
 * The pipeline calls into the existing kernels directly. Stages adapt
 * span-based I/O to the kernel's `std::vector`
 * signatures by materialising the kernel's output into arena-backed slabs
 * after the kernel returns.  The wire type between stages is `std::span<p_element>`
 * (with mutating stages taking non-const elements and read-only stages
 * taking const).
 */
module;

#include <bioseq/pg_build_profile.h>

// The chain primitive (`pipe::detail::dispatch_chain`) relies
// on tree-inlining of [[gnu::always_inline]] recursive instantiations.  Both
// gcc and clang honour the attribute at -O1+, but downgrade silently at -O0.
// Build configs that compile this module without optimisation produce
// stack-spilled adaptor frames that defeat the point of the cascade; refuse
// to compile in that mode.
#if !defined(__OPTIMIZE__) && !defined(BIOSEQ_PACKAGE_O0_PROFILE)
# error "pipeline chain primitive requires -O1 or higher. " \
        "Use -Og or -O1 -g for debug builds of pipe.cppm; only the explicit " \
        "distribution package O0 profile may disable this guard."
#endif

#ifndef BIOSEQ_PIPELINE_FORCE_INLINE
#define BIOSEQ_PIPELINE_FORCE_INLINE 1
#endif

#if defined(BIOSEQ_PACKAGE_O0_PROFILE) && BIOSEQ_PIPELINE_FORCE_INLINE
# error "BIOSEQ_PACKAGE_O0_PROFILE requires BIOSEQ_PIPELINE_FORCE_INLINE=0"
#endif

#if BIOSEQ_PIPELINE_FORCE_INLINE
#define BIOSEQ_PIPELINE_INLINE_ATTR [[gnu::always_inline]]
#define BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR [[gnu::always_inline, gnu::flatten]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR [[nodiscard, gnu::always_inline]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR [[nodiscard, gnu::always_inline, gnu::flatten]]
#else
#define BIOSEQ_PIPELINE_INLINE_ATTR
#define BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR [[nodiscard]]
#define BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR [[nodiscard]]
#endif

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <algorithm>
#include <atomic>
#include <array>
#include <concepts>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>
#include <libassert/assert.hpp>

export module bioseq.pipeline.pipe;

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

// This target-independent execution value deliberately precedes Highway
// target toggling: these types describe
// a whole statement and must have one ABI regardless of the compiled simd
// target. algorithm_type-local statistics are mapped into this model at dispatch
// boundaries; no Highway namespace owns an alias of it.
export import bioseq.pipeline.stats;

export {

namespace bioseq::simd::pipe {

namespace prefilter = ::bioseq::prefilter;
// Keep the canonical minimizer-index concepts visible to stage templates.
using bioseq::indexing::minimizer_hashmap_fragment_index;
using bioseq::indexing::minimizer_lookup_index;
using bioseq::indexing::minimizer_seed_index;

// Export exception aliases through the pipeline module so PG importers do not
// redundantly import the Highway-heavy implementation modules.
using exact_simd_runtime_error =
    ::bioseq::minimizers::exact_simd_runtime_error;
using simd_chaining_runtime_error =
    ::bioseq::chaining::simd_chaining_runtime_error;
using simd_chaining_status =
    ::bioseq::chaining::simd_chaining_status;

/// Fail-closed error for the selected bounded ntHash route.  PostgreSQL-facing
/// callers catch this concrete type before entering the longjmp ERROR boundary;
/// no bounded ntHash rejection is allowed to escape the C++ frame or select another
/// minimizer implementation.
class bounded_nthash_runtime_error final : public std::runtime_error {
public:
    explicit bounded_nthash_runtime_error(std::string detail)
        : std::runtime_error(
              std::string{
                  "bounded_nthash_simd minimizer execution rejected: "}
              + std::move(detail)) {}
};

// — pipeline lengths/positions use the 64-bit sequence_size_t.
using bioseq::storage::sequence_size_t;

using host_stage_clock = std::chrono::steady_clock;

[[nodiscard]] inline std::uint64_t host_stage_now_ns() noexcept {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        host_stage_clock::now().time_since_epoch()).count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
}

[[nodiscard]] inline std::uint64_t elapsed_host_stage_ns(
        const std::uint64_t started_ns) noexcept {
    const std::uint64_t finished_ns = host_stage_now_ns();
    const std::uint64_t elapsed = finished_ns >= started_ns
        ? finished_ns - started_ns : 0u;
    // A stage that executed is a positive observation even on a platform whose
    // steady-clock tick is coarser than one nanosecond. The clamp can add at
    // most one nanosecond and keeps focused tests independent of clock
    // resolution without sleeping.
    return elapsed > 0 ? elapsed : 1u;
}

[[noreturn]] inline void throw_vulkan_declined(std::string_view stage) {
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    const std::string detail = !runtime.is_usable()
        ? fmt::format("runtime unavailable: {}", runtime.initialization_error())
        : runtime.features().software_device
            ? fmt::format(
                  "active Vulkan device '{}' is a CPU/software device "
                  "(VkPhysicalDeviceType={}); explicit GPU execution requires a hardware Vulkan device",
                  runtime.features().device_name.data(),
                  runtime.features().device_type)
            : "GPU path declined";
    throw bioseq::gpu::vulkan::vulkan_runtime_error(
        fmt::format("{} selected but Vulkan execution did not run ({})", stage, detail));
}

[[noreturn]] inline void throw_vulkan_sketch_declined(
    std::string_view stage,
    const bioseq::gpu::vulkan::vulkan_sketch_decline& decline)
{
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
static_assert(__cplusplus >= 202302L,
    "bioseq pipeline templates require C++23");

// Import types from the surrounding modules so the stage bodies can refer
// to them by short name.
using ::bioseq::chaining::anchor;
using ::bioseq::chaining::chain;
using ::bioseq::chaining::chaining_parameters;
using ::bioseq::chaining::arena_slab;
using ::bioseq::chaining::make_arena_slab;
using ::bioseq::chaining::chain_anchors_scalar;
using ::bioseq::chaining::chain_anchors_rmq;

// =============================================================================
// Compile-time parameter packs
// =============================================================================

/**
 * @brief Type-level parameter pack for a fully specialised pipeline instance.
 *
 * @tparam p_window_size           Minimizer window size (must be > 0).
 * @tparam p_kmer_size           p_kmer_size-mer length (must match the minimizer_representation's KmerLen).
 * @tparam p_sequence_length_log2  ceil(log2(max(query_bp, target_bp))).  Selects the
 *                     `minimizer_representation<p_sequence_length_log2, p_kmer_size>` storage bucket — same
 *                     dispatch already happens at runtime in `minimap_port`.
 */
template <std::size_t p_window_size, std::size_t p_kmer_size, std::size_t p_sequence_length_log2>
struct static_aligner_parameters {
    static constexpr std::size_t window_len        = p_window_size;
    static constexpr std::size_t kmer_len          = p_kmer_size;
    static constexpr std::size_t sequence_len_log2 = p_sequence_length_log2;
    static constexpr bool        generic_fallback  = false;

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
template <std::size_t p_sequence_length_log2>
struct fallback_aligner_parameters {
    static constexpr std::size_t window_len        = 0;
    static constexpr std::size_t kmer_len          = 0;
    static constexpr std::size_t sequence_len_log2 = p_sequence_length_log2;
    static constexpr bool        generic_fallback  = true;

    using minimizer_repr_t =
        ::bioseq::minimizers::minimizer_representation<p_sequence_length_log2, 15u>;

    [[nodiscard]] static constexpr std::string_view label() noexcept {
        return "fallback(generic w/k)";
    }
};

template <class p_type>
concept is_constexpr_aligner_params = requires {
    { p_type::window_len }        -> std::convertible_to<std::size_t>;
    { p_type::kmer_len }          -> std::convertible_to<std::size_t>;
    { p_type::sequence_len_log2 } -> std::convertible_to<std::size_t>;
    { p_type::generic_fallback }  -> std::convertible_to<bool>;
    typename p_type::minimizer_repr_t;
    { p_type::label() }           -> std::convertible_to<std::string_view>;
};

static_assert(is_constexpr_aligner_params<static_aligner_parameters<10u, 15u, 28u>>);
static_assert(is_constexpr_aligner_params<fallback_aligner_parameters<28u>>);

/**
 * @brief Pipeline-builder entry-point tag.
 *
 * `with_prebuilt_minimizers` — pipeline accepts a `std::span<const p_minimizer>`
 * directly; the index_stage is at the head.
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
}

/**
 * @brief Base tag inherited by every concrete stage.
 *
 * Lets `make_pipeline` distinguish bare-stage arguments from the terminal
 * sink via the `bare_stage_marker` typedef.
 */
template <class p_derived>
struct bare_stage {
    using bare_stage_marker = void;
};

// Forward declaration so stage_base::attach can refer to stage.
template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
struct stage;

// Metadata types (defined fully in; forward-declared here so describe()
// has a return type).
struct stage_description;
struct pipeline_description;

// =============================================================================
// — narrow minimizer-batch shape carrying an optional device span.
// =============================================================================
//
// One additional stage-shape on top of the  matrix. It borrows the host
// minimizers from the arena slab and owns the optional GPU device minimizer
// handle through `vulkan_device_minimizer_span::owner`, so the all-Vulkan
// sketch+seed path avoids a second query-minimizer upload. CPU minimizer
// kernels keep using the existing `std::span<const p_minimizer>` path.
template <class p_minimizer>
struct minimizer_batch_view {
    using value_type = p_minimizer;

    std::span<const p_minimizer> host{};
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device{};

    [[nodiscard]] const p_minimizer* data() const noexcept { return host.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return host.size(); }
    [[nodiscard]] std::span<const p_minimizer> as_span() const noexcept { return host; }
    [[nodiscard]] auto begin() const noexcept { return host.begin(); }
    [[nodiscard]] auto end() const noexcept { return host.end(); }
};

template <class p_type>
struct is_minimizer_batch_view : std::false_type {};
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
concept batch_sink = requires (p_sink_type& sink, std::span<p_element> batch) {
    { sink.on_batch(batch) } -> std::same_as<void>;
    { sink.on_completed() }  -> std::same_as<void>;
};

/**
 * @brief Pair-batch sink concept.  Required by emit_token::emit_batch_pair.
 *
 * The pair shape is used by the chain → region → primary/secondary →
 * filter → divergence → align cascade where each stage forwards both the
 * regions span and the (read-only) anchors span downstream.
 */
template <class p_sink_type, class p_first_element, class p_second_element>
concept pair_batch_sink = requires (p_sink_type& sink, std::span<p_first_element> a, std::span<p_second_element> b) {
    { sink.on_batch_pair(a, b) } -> std::same_as<void>;
    { sink.on_completed() }      -> std::same_as<void>;
};

/**
 * @brief Index-decorated batch sink concept.  Required by
 * emit_token::emit_batch_with_index.
 *
 * Used by index_stage to hand a sketches span downstream alongside a
 * (const-reference) target index.  seed_hit_stage is the canonical consumer.
 */
template <class p_sink_type, class p_element, class p_index>
concept indexed_batch_sink = requires (p_sink_type& sink, std::span<p_element> a, const p_index& idx,
                                     bioseq::indexing::gpu_index_cache_identity identity) {
    { sink.on_batch_with_index(a, idx, identity) } -> std::same_as<void>;
    { sink.on_completed() }                        -> std::same_as<void>;
};

/// — minimizer-batch sink (minimizer_stage → index_stage hand-off).
template <class p_sink_type, class p_minimizer>
concept minimizer_batch_sink = requires (p_sink_type& sink, minimizer_batch_view<p_minimizer> batch) {
    { sink.on_minimizer_batch(batch) } -> std::same_as<void>;
    { sink.on_completed() }            -> std::same_as<void>;
};

/// — index-decorated minimizer-batch sink (index_stage →
/// seed_hit_stage hand-off carrying the GPU index cache identity).
template <class p_sink_type, class p_minimizer, class p_index>
concept indexed_minimizer_batch_sink =
requires (p_sink_type& sink, minimizer_batch_view<p_minimizer> batch, const p_index& idx,
          bioseq::indexing::gpu_index_cache_identity identity) {
    { sink.on_minimizer_batch_with_index(batch, idx, identity) } -> std::same_as<void>;
    { sink.on_completed() }                                      -> std::same_as<void>;
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
 * The token's only constructor is private; only `stage<p_aligner_parameters, p_self, p_sink>`
 * is befriended, which means concrete-stage `process*<p_aligner_parameters>` bodies see
 * the token by rvalue reference but cannot manufacture one themselves —
 * the type system rules out skipping the emit (no way to construct the
 * required `emitted_token<p_sink>` return value).
 *
 * Single-emission enforcement:
 *   - At least once: HARD compile-time.  `process*<p_aligner_parameters>` must return
 *     `emitted_token<p_sink>`; the only way to construct one is to call
 *     `emit_token<p_sink>::emit_batch*`.
 *   - At most once: soft compile-time + runtime trip-wire.  `[[nodiscard]]`
 *     makes the ergonomic shape `return std::move(token).emit_batch*(...)`;
 *     emitting twice trips the `DEBUG_ASSERT(sink_ptr_ != nullptr)` in debug.
 */
template <class p_sink>
class emit_token {
public:
    template <is_constexpr_aligner_params p_aligner_parameters,
              class p_self,
              class p_stage_sink>
    friend struct stage;

    emit_token()                            = delete;
    emit_token(const emit_token&)            = delete;
    emit_token(emit_token&&)                 = delete;
    emit_token& operator=(const emit_token&) = delete;
    emit_token& operator=(emit_token&&)      = delete;

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

    template <class p_first_element, class p_second_element>
    [[nodiscard, gnu::always_inline]]
    emitted_token<p_sink> emit_batch_pair(std::span<p_first_element> first_batch,
                                       std::span<p_second_element> second_batch) && {
        static_assert(pair_batch_sink<p_sink, p_first_element, p_second_element>,
            "emit_token::emit_batch_pair: downstream sink does not implement "
            "on_batch_pair(std::span<p_first_element>, std::span<p_second_element>).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "emit_token consumed twice.");
        sink_ptr_->on_batch_pair(first_batch, second_batch);
        sink_ptr_ = nullptr;
        return emitted_token<p_sink>::make();
    }

    template <class p_element, class p_index>
    [[nodiscard, gnu::always_inline]]
    emitted_token<p_sink> emit_batch_with_index(
        std::span<p_element>  data,
        const p_index& index_ref,
        bioseq::indexing::gpu_index_cache_identity identity) && {
        static_assert(indexed_batch_sink<p_sink, p_element, p_index>,
            "emit_token::emit_batch_with_index: downstream sink does not "
            "implement on_batch_with_index(std::span<p_element>, p_index const&, identity).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "emit_token consumed twice.");
        sink_ptr_->on_batch_with_index(data, index_ref, identity);
        sink_ptr_ = nullptr;
        return emitted_token<p_sink>::make();
    }

    // — narrow minimizer-batch emit (carries optional device span).
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

    template <class p_minimizer, class p_index>
    [[nodiscard, gnu::always_inline]]
    emitted_token<p_sink> emit_minimizer_batch_with_index(
        minimizer_batch_view<p_minimizer> batch,
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
    explicit constexpr emit_token(p_sink& sink_ref) noexcept
        : sink_ptr_(&sink_ref) {}
};

/**
 * @brief Stats-of-emission token.  Returned by
 * `emit_token<p_sink>::emit_batch*`; every concrete stage's `process*<p_aligner_parameters>`
 * must return one.  The only path to constructing one is the
 * friend-accessible `make()` factory.
 */
template <class p_sink>
class [[nodiscard]] emitted_token {
public:
    emitted_token(const emitted_token&)             = delete;
    emitted_token& operator=(const emitted_token&)  = delete;
    emitted_token& operator=(emitted_token&&)       = delete;

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
 * @brief Drop-in `sketch_collector_t<p_element>`-shaped adapter that routes the
 * kernel's `collect(...)` traffic into a caller-provided
 * `arena_slab<p_element>`.  Implements the minimal surface the kernels rely on
 * (`collect(p_element&&)`, `collect(const p_element&)`, `reserve(size_t)`).
 *
 * `arena_slab<p_element>::push_back` requires `std::is_trivially_copyable_v<p_element>` —
 * satisfied by every `minimizer_representation<p_bucket_width, p_kmer_size>`.  The existing slab
 * `arena_slab`'s `static_assert` covers the constraint and
 * would fire if a non-trivial minimizer type were ever introduced.
 */
template <class p_element>
struct sketch_collector_arena_adapter {
    using minimizer_type = p_element;

    ::bioseq::chaining::arena_slab<p_element>* slab_;

    [[gnu::always_inline]] void collect(p_element sketch) {
        slab_->push_back(std::move(sketch));
    }
    [[gnu::always_inline]] void collect(const p_element& sketch) {
        slab_->push_back(sketch);
    }
    [[gnu::always_inline]] void reserve(std::size_t expected_count) {
        slab_->reserve(expected_count);
    }
    [[nodiscard, gnu::always_inline]] std::size_t size() const noexcept {
        return slab_->size();
    }

    [[nodiscard, gnu::always_inline]] bool append_records(
            const std::span<const p_element> records) {
        return slab_->append_trivial_records(records);
    }

    template<class p_minimizer_type = p_element>
    [[nodiscard, gnu::always_inline]] bool append_raw_storage(
        const typename p_minimizer_type::storage_type* raw,
        std::size_t count)
        requires (
            std::same_as<p_minimizer_type, p_element>
            && requires { typename p_minimizer_type::storage_type; }
            && std::is_trivially_copyable_v<p_minimizer_type>
            && sizeof(p_minimizer_type) == sizeof(typename p_minimizer_type::storage_type))
    {
        return slab_->append_trivial_storage(
            std::span<const typename p_minimizer_type::storage_type>{raw, count});
    }
};

/**
 * @brief CRTP helper absorbing the boilerplate shared by every concrete stage.
 *
 * `p_derived` must define `static constexpr std::string_view stage_name`.  The
 * default `attach<p_aligner_parameters, p_sink>(...)` returns a bound `stage<p_aligner_parameters, p_derived, p_sink>`
 * value owning `*this` and the downstream sink.
 */
template <class p_derived, class p_output>
struct stage_base : bare_stage<p_derived> {
    using out_element_type = p_output;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_sink>
    [[gnu::always_inline]] auto attach(p_sink sink) noexcept {
        return stage<p_aligner_parameters, p_derived, p_sink>{
            std::move(sink), std::move(static_cast<p_derived&>(*this))};
    }

    template <class p_sink>
    [[gnu::always_inline]] void finalize(p_sink&) noexcept {
        /* default: no-op */
    }
};

/**
 * @brief Bound stage — owns its downstream sink and bare-stage config by value.
 *
 * `p_aligner_parameters` is fixed at `make_pipeline<p_aligner_parameters>(...)` time and identical across every
 * stage in a given pipeline instance.  The bound stage's `on_batch*` members
 * forward to the bare stage's `process*<p_aligner_parameters>` body; both are
 * `[[gnu::always_inline]]` so the chain collapses to a straight-line sequence
 * of kernel calls at `-O3`.
 */
template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
struct stage {
    using cep_t = p_aligner_parameters;

    p_sink sink_;
    p_self self_;

    constexpr stage(p_sink sink_arg, p_self self_arg) noexcept
        : sink_(std::move(sink_arg)), self_(std::move(self_arg)) {}

    //: the bound stage constructs an `emit_token<p_sink>` on its
    // own stack and hands it to `process*<p_aligner_parameters>` by rvalue reference; the
    // concrete-stage body returns the (discarded) `emitted_token<p_sink>` stats
    // type whose construction is friend-private to `emit_token<p_sink>`.
    template <class p_batch>
    [[gnu::always_inline]] void on_batch(p_batch&& batch) {
        (void) self_.template process<p_aligner_parameters>(
            std::forward<p_batch>(batch), emit_token<p_sink>{sink_});
    }

    template <class p_first_batch, class p_second_batch>
    [[gnu::always_inline]] void on_batch_pair(p_first_batch&& first_batch, p_second_batch&& second_batch) {
        (void) self_.template process_pair<p_aligner_parameters>(
            std::forward<p_first_batch>(first_batch), std::forward<p_second_batch>(second_batch),
            emit_token<p_sink>{sink_});
    }

    template <class p_batch, class p_index>
    [[gnu::always_inline]] void on_batch_with_index(
        p_batch&& batch, const p_index& index_ref,
        bioseq::indexing::gpu_index_cache_identity identity) {
        (void) self_.template process_with_index<p_aligner_parameters>(
            std::forward<p_batch>(batch), index_ref, identity, emit_token<p_sink>{sink_});
    }

    // — forwarders for the narrow minimizer-batch shape.
    template <class p_minimizer>
    [[gnu::always_inline]] void on_minimizer_batch(minimizer_batch_view<p_minimizer> batch) {
        (void) self_.template process<p_aligner_parameters>(batch, emit_token<p_sink>{sink_});
    }

    template <class p_minimizer, class p_index>
    [[gnu::always_inline]] void on_minimizer_batch_with_index(
        minimizer_batch_view<p_minimizer> batch, const p_index& index_ref,
        bioseq::indexing::gpu_index_cache_identity identity) {
        (void) self_.template process_with_index<p_aligner_parameters>(
            batch, index_ref, identity, emit_token<p_sink>{sink_});
    }

    [[gnu::always_inline]] void on_completed() noexcept {
        self_.finalize(sink_);
        sink_.on_completed();
    }

    [[nodiscard]] pipeline_description describe() const;
};

/**
 * @brief Variadic pipeline composer — fixes `p_aligner_parameters` once for the whole chain.
 *
 * The base case (terminal sink) returns the sink unchanged; the recursive case
 * peels one bare stage off the front, builds the downstream pipeline from the
 * remaining arguments, and attaches.
 */
template <is_constexpr_aligner_params p_aligner_parameters, class p_sink>
[[gnu::always_inline]] constexpr auto make_pipeline(p_sink sink) noexcept {
    static_assert(!detail::is_bare_stage<p_sink>,
        "make_pipeline's last argument must be a terminal sink, "
        "not a bare_stage.  Append your terminal sink at the end.");
    return sink;
}

template <is_constexpr_aligner_params p_aligner_parameters, class p_first_stage, class... p_remaining_stages>
    requires detail::is_bare_stage<p_first_stage>
[[gnu::always_inline]] constexpr auto make_pipeline(p_first_stage head, p_remaining_stages... rest) noexcept {
    auto downstream = make_pipeline<p_aligner_parameters>(std::move(rest)...);
    return std::move(head).template attach<p_aligner_parameters>(std::move(downstream));
}

// =============================================================================
// Batch storage — owning arena slab
// =============================================================================
//
// `arena_slab<p_element>` itself lives in `bioseq.chaining.core` so the
// chaining kernels (chain_anchors_*) and the seed/region kernels in
// `minimap_port.cppm` can take it as an output parameter — see
// follow-up "output-side kernel-to-arena refactor".  The `using` declarations
// at the top of this namespace re-export both the slab type and the
// `make_arena_slab<p_element>()` helper so stage bodies refer to them by short name.

// =============================================================================
// Metadata surface — forward types for algorithm_traits
// =============================================================================
//
// The full `stage_description` / `pipeline_description` definitions live after
// the algorithm_traits block (they reference `named_property` too).  The
// `named_property` POD itself is the only piece the traits' `computed_properties_default()`
// signature needs, so we hoist it here ahead of the traits.

struct named_property {
    std::string name;
    std::string value;
};

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
template <auto p_algorithm>
inline constexpr bool no_kernel_for_v = false;

/// Primary template — unspecialised.  Substitution failure here produces a
/// readable diagnostic naming the offending enum value.
///
/// Specializations below cover the chaining, minimizer, and base-alignment
/// algorithm dimensions.
template <auto p_algorithm>
struct algorithm_traits {
    static_assert(no_kernel_for_v<p_algorithm>,
        "algorithm_traits<X>: no specialisation for this algorithm enum value. "
        "Declare one in pipe.cppm next to the chaining_algorithm / "
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
template <>
struct algorithm_traits<::bioseq::simd::chaining_algorithm::scalar_minimap2> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_stats = true;
        static constexpr bool emits_simd_chaining_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        [[gnu::always_inline]] inline static void call(
            std::span<const anchor> anchors,
            const chaining_parameters&   params,
            arena_slab<chain>&       chains_out,
            arena_slab<anchor>&      reordered_anchors_out,
            scalar_pipeline_execution_stats* stats)
        {
            ::bioseq::chaining::chain_anchors_scalar(
                anchors, params, chains_out, reordered_anchors_out);
            if (stats != nullptr && !anchors.empty()) {
                stats->record_scalar_chain(
                    static_cast<std::uint64_t>(anchors.size()));
            }
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::chaining_algorithm_name(
                ::bioseq::simd::chaining_algorithm::scalar_minimap2);
        }
        static void populate_runtime_params(
            const chaining_parameters&,
            std::vector<named_property>&) noexcept {}
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "false"},
                {"complexity", "O(n * max_iter)"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// simd DP chaining — `chain_anchors_simd` from
/// the selected fixed-target chaining table.
template <>
struct algorithm_traits<::bioseq::simd::chaining_algorithm::simd_minimap2> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_stats = false;
        static constexpr bool emits_simd_chaining_execution_stats = true;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::
                chaining_chain_anchors_simd;
        [[gnu::always_inline]] inline static void call(
            const ::bioseq::simd_dispatch::highway_kernel_table* highway,
            std::span<const anchor> anchors,
            const chaining_parameters&   params,
            arena_slab<chain>&       chains_out,
            arena_slab<anchor>&      reordered_anchors_out,
            ::bioseq::chaining::simd_chaining_execution_stats* stats)
        {
            if (highway == nullptr
                || !highway->has_kernel(highway_kernel_id)) {
                throw simd_chaining_runtime_error(
                    simd_chaining_status::unsupported_simd_target,
                    "selected Highway table has no chaining kernel");
            }
            ::bioseq::chaining::chain_anchors_simd(
                highway->chaining.chain_anchors_simd,
                anchors, params, chains_out, reordered_anchors_out,
                stats, /*aggregate_stats=*/true);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::chaining_algorithm_name(
                ::bioseq::simd::chaining_algorithm::simd_minimap2);
        }
        static void populate_runtime_params(
            const chaining_parameters& params,
            std::vector<named_property>& properties) {
            properties.push_back({
                "simd_chaining_policy",
                std::string{::bioseq::chaining::simd_chaining_policy_name(
                    params.simd_policy)}});
        }
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "true"},
                {"requires_zero_skip_penalty", "false"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// range_minimum_query tree-based chaining — `chain_anchors_rmq` from
/// `bioseq.chaining.rmq`.
template <>
struct algorithm_traits<::bioseq::simd::chaining_algorithm::range_minimum_query> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_stats = false;
        static constexpr bool emits_simd_chaining_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        [[gnu::always_inline]] inline static void call(
            std::span<const anchor> anchors,
            const chaining_parameters&   params,
            arena_slab<chain>&       chains_out,
            arena_slab<anchor>&      reordered_anchors_out)
        {
            ::bioseq::chaining::chain_anchors_rmq(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::chaining_algorithm_name(
                ::bioseq::simd::chaining_algorithm::range_minimum_query);
        }
        static void populate_runtime_params(
            const chaining_parameters&,
            std::vector<named_property>&) noexcept {}
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "false"},
                {"complexity", "O(n log n)"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Vulkan tiled-DP chaining — `chain_anchors_vulkan` from
/// `bioseq.chaining.vulkan`. G3SA-style tiled DP score generation on
/// Vulkan + bioseq CPU `backtrack_chains`; selected Vulkan execution is
/// mandatory and failures throw `vulkan_runtime_error`.
template <>
struct algorithm_traits<::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming> {
    struct kernel_type {
        static constexpr bool emits_scalar_chaining_execution_stats = false;
        static constexpr bool emits_simd_chaining_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = true;
        [[nodiscard, gnu::always_inline]] inline static bool call(
            std::span<const anchor> anchors,
            const chaining_parameters&   params,
            arena_slab<chain>&       chains_out,
            arena_slab<anchor>&      reordered_anchors_out)
        {
            return ::bioseq::chaining::chain_anchors_vulkan(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::chaining_algorithm_name(
                ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming);
        }
        static void populate_runtime_params(
            const chaining_parameters& params,
            std::vector<named_property>& props) noexcept
        {
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
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// ---------------------------------------------------------------------------
//  — Scanner-aware minimizer dispatch.
// For a plain compact_sequence_range or a no-fragment lazy wrapper, runs the
// requested in-RAM kernel (scalar / reference-simd / exact-simd) on contiguous storage
// (zero overhead — the no-fragment lazy forwards to its underlying range). For
// a fragmented lazy wrapper, streams fragments through the bit-identical scalar
// kernel (`collect_minimizers_view`) — see for why the scalar path
// is used instead of the simd lane-replay.
// ---------------------------------------------------------------------------
enum class minimizer_kernel_kind { scalar, simd, exact_simd };

template <sequence_alphabet p_alphabet>
[[nodiscard]] inline const ::bioseq::simd_dispatch::
    highway_alphabet_identity_entry&
resolved_highway_alphabet_identity(
    const ::bioseq::simd_dispatch::highway_kernel_table* table)
{
    const auto* identity = table == nullptr || !table->is_complete()
        ? nullptr : table->template resolve_alphabet_identity<p_alphabet>();
    if (identity == nullptr
        || table->resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "selected Highway table has no unique alphabet identity"};
    }
    return *identity;
}

template <sequence_alphabet p_alphabet, class p_descriptor>
[[nodiscard]] inline ::bioseq::minimizers::packed_sequence_kernel_view
minimizer_kernel_view(
    const p_descriptor& descriptor,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway)
{
    if (descriptor.logical_length > static_cast<sequence_size_t>(
            (std::numeric_limits<std::size_t>::max)())
        || descriptor.leading_symbol_offset
            > (std::numeric_limits<std::size_t>::max)()
                - static_cast<std::size_t>(descriptor.logical_length)) {
        throw std::overflow_error(
            "packed minimizer descriptor exceeds the neutral ABI");
    }
    const std::size_t begin = descriptor.leading_symbol_offset;
    const std::size_t length =
        static_cast<std::size_t>(descriptor.logical_length);
    return ::bioseq::minimizers::packed_sequence_kernel_view{
        .storage = static_cast<const std::byte*>(descriptor.units),
        .storage_units = descriptor.unit_count,
        .begin_symbol = begin,
        .end_symbol = begin + length,
        .alphabet_code = resolved_highway_alphabet_identity<p_alphabet>(
            highway).alphabet_code,
        .bits_per_symbol = descriptor.bits_per_symbol,
        .storage_unit_bytes = descriptor.storage_unit_width,
        .final_segment = true,
    };
}

/** Preserve any genuinely vectorized resident packing before materialising. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::optional<typename bioseq::storage::
    lazy_sequence_view<p_alphabet>::resident_packed_sequence>
try_minimizer_resident_packed(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    sequence_size_t from,
    sequence_size_t to)
{
    for (const std::uint8_t width :
         std::array<std::uint8_t, 4>{1u, 2u, 4u, 8u}) {
        auto descriptor = sequence.try_resident_packed(from, to, width);
        if (descriptor.has_value()
            && descriptor->leading_symbol_offset == 0u) {
            return descriptor;
        }
    }
    return std::nullopt;
}

/** Reference simd retains its established resident u32/u64 contract. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::optional<typename bioseq::storage::
    lazy_sequence_view<p_alphabet>::resident_packed_sequence>
try_word_minimizer_resident_packed(
    bioseq::storage::lazy_sequence_view<p_alphabet> sequence,
    sequence_size_t from,
    sequence_size_t to)
{
    for (const std::uint8_t width :
         std::array<std::uint8_t, 2>{8u, 4u}) {
        auto descriptor = sequence.try_resident_packed(from, to, width);
        if (descriptor.has_value()
            && descriptor->leading_symbol_offset == 0u) {
            return descriptor;
        }
    }
    return std::nullopt;
}

/**
 * Batch adapter from the caller-selected exact minimizer ABI to its collector.
 * Exceptions cannot cross the sink's noexcept commit
 * callback, so they are captured and rethrown immediately after the one
 * coarse fixed-target call returns.
 */
template <class p_minimizer, class p_collector>
class highway_minimizer_sink_state final {
public:
    explicit highway_minimizer_sink_state(p_collector& collector) noexcept
        : collector_(&collector) {}

    [[nodiscard]] ::bioseq::minimizers::minimizer_sink sink() noexcept {
        return {
            .state = this,
            .acquire = &acquire,
            .commit = &commit,
            .append_raw_u64 = &append_raw_u64,
        };
    }

    void validate(const ::bioseq::minimizers::minimizer_kernel_result& result) {
        if (exception_ != nullptr) {
            std::rethrow_exception(exception_);
        }
        // An acquired-but-uncommitted batch is always a transport protocol
        // violation.  It cannot be reclassified as a caller-owned allocation
        // failure because the target never completed the batch lifetime.
        if (is_acquired_) {
            throw std::runtime_error(
                "selected Highway minimizer sink left a batch uncommitted");
        }
        if (rejected_ || result.emitted != committed_) {
            if constexpr (requires (const p_collector& value) {
                { value.ok() } -> std::convertible_to<bool>;
            }) {
                // Admission-aware collectors deliberately expose their
                // sticky failure to the outer scheduler. Preserve the
                // established caller mapping (for example PostgreSQL OOM /
                // collector_growth_failed) instead of replacing it with a
                // generic transport exception. Protocol/layout failures keep
                // collector.ok()==true and still throw below.
                if (!static_cast<bool>(collector_->ok())) {
                    return;
                }
            }
            throw std::runtime_error(
                "selected Highway minimizer sink rejected a committed batch");
        }
    }

private:
    static constexpr std::size_t batch_capacity = 256u;
    static constexpr auto output_layout =
        ::bioseq::minimizers::k_minimizer_output_layout<p_minimizer>;

    static_assert(
        output_layout
        != ::bioseq::minimizers::minimizer_output_layout::unsupported);
    static_assert(std::is_standard_layout_v<p_minimizer>);
    static_assert(std::is_trivially_copyable_v<p_minimizer>);
    static_assert(std::is_trivially_destructible_v<p_minimizer>);

    [[nodiscard]] static ::bioseq::minimizers::minimizer_sink_batch_view
    acquire(void* opaque, std::size_t minimum) {
        auto& self = *static_cast<highway_minimizer_sink_state*>(opaque);
        if (self.rejected_ || self.is_acquired_ || minimum > batch_capacity) {
            self.rejected_ = true;
            return {};
        }
        self.is_acquired_ = true;
        return {
            .data = self.batch_.data(),
            .capacity = batch_capacity,
            .layout = output_layout,
            .record_size = sizeof(p_minimizer),
            .record_alignment = alignof(p_minimizer),
        };
    }

    static void commit(void* opaque, std::size_t initialized) noexcept {
        auto& self = *static_cast<highway_minimizer_sink_state*>(opaque);
        if (!self.is_acquired_ || initialized > batch_capacity) {
            self.rejected_ = true;
            return;
        }
        auto* records = std::launder(
            reinterpret_cast<p_minimizer*>(self.batch_.data()));
        const auto destroy_records = [&self, records, initialized]() noexcept {
            std::destroy_n(records, initialized);
            self.is_acquired_ = false;
        };
        if (self.rejected_) {
            destroy_records();
            return;
        }
        try {
            bool accepted = static_cast<bool>(self.collector_->append_records(
                std::span<const p_minimizer>{records, initialized}));
            if constexpr (requires (p_collector& value) {
                { value.ok() } -> std::convertible_to<bool>;
            }) {
                accepted = accepted
                    && static_cast<bool>(self.collector_->ok());
            }
            destroy_records();
            if (accepted) {
                self.committed_ += initialized;
            } else {
                self.rejected_ = true;
            }
        } catch (...) {
            destroy_records();
            self.exception_ = std::current_exception();
            self.rejected_ = true;
        }
    }

    [[nodiscard]] static bool append_raw_u64(
            void* opaque,
            ::bioseq::minimizers::minimizer_output_layout layout,
            const std::uint64_t* raw,
            std::size_t count) noexcept {
        auto& self = *static_cast<highway_minimizer_sink_state*>(opaque);
        constexpr auto expected_layout =
            ::bioseq::minimizers::k_minimizer_output_layout<p_minimizer>;
        if (self.rejected_ || self.is_acquired_ || layout != expected_layout
            || expected_layout
                == ::bioseq::minimizers::minimizer_output_layout::unsupported
            || (count != 0u && raw == nullptr)) {
            self.rejected_ = true;
            return false;
        }
        if constexpr (!::bioseq::minimizers::
                exact_simd_minimizer_representation<
                    std::remove_cv_t<p_minimizer>>) {
            // A raw-u64 batch is an exact representation fast path, never a
            // conversion path. In particular, do not truncate it into a
            // u32-backed minimizer_representation.
            self.rejected_ = true;
            return false;
        } else {
            try {
                bool accepted = true;
                if constexpr (requires (
                        p_collector& value,
                        const std::uint64_t* values,
                        std::size_t value_count) {
                    { value.append_raw_storage(values, value_count) }
                        -> std::convertible_to<bool>;
                }) {
                    accepted = static_cast<bool>(
                        self.collector_->append_raw_storage(raw, count));
                } else {
                    for (std::size_t i = 0u; i < count; ++i) {
                        self.collector_->collect(
                            p_minimizer::from_raw_storage(raw[i]));
                    }
                }
                if constexpr (requires (p_collector& value) {
                    { value.ok() } -> std::convertible_to<bool>;
                }) {
                    accepted = accepted
                        && static_cast<bool>(self.collector_->ok());
                }
                if (!accepted) {
                    self.rejected_ = true;
                    return false;
                }
                self.committed_ += count;
                return true;
            } catch (...) {
                self.exception_ = std::current_exception();
                self.rejected_ = true;
                return false;
            }
        }
    }

    p_collector* collector_ = nullptr;
    alignas(p_minimizer)
        std::array<std::byte, batch_capacity * sizeof(p_minimizer)> batch_;
    std::exception_ptr exception_{};
    std::size_t committed_ = 0u;
    bool is_acquired_ = false;
    bool rejected_ = false;
};

[[nodiscard]] inline ::bioseq::minimizers::minimizer_kernel_function
selected_minimizer_kernel(
    const ::bioseq::simd_dispatch::highway_kernel_table* table,
    ::bioseq::simd_dispatch::highway_kernel id)
{
    if (table == nullptr || !table->has_kernel(id)) {
        throw std::runtime_error(
            "selected Highway table does not contain the minimizer kernel");
    }
    switch (id) {
        case ::bioseq::simd_dispatch::highway_kernel::
                minimizer_minimap2_reference:
            return table->minimizers.minimap2_reference;
        case ::bioseq::simd_dispatch::highway_kernel::
                minimizer_two_stack_nthash:
            return table->minimizers.two_stack_nthash;
        case ::bioseq::simd_dispatch::highway_kernel::
                minimizer_bounded_nthash:
            return table->minimizers.bounded_nthash;
        default:
            break;
    }
    throw std::invalid_argument(
        "requested Highway kernel is not a minimizer kernel");
}

template <class p_minimizer, class p_collector>
[[nodiscard]] inline ::bioseq::minimizers::minimizer_kernel_result
invoke_selected_minimizer_kernel(
    const ::bioseq::simd_dispatch::highway_kernel_table* table,
    ::bioseq::simd_dispatch::highway_kernel id,
    ::bioseq::minimizers::minimizer_kernel_algorithm expected_algorithm,
    ::bioseq::minimizers::packed_sequence_kernel_view sequence,
    std::size_t window,
    std::size_t kmer,
    p_collector& collector,
    ::bioseq::minimizers::two_stack_route two_stack_route =
        ::bioseq::minimizers::two_stack_route::chronological_continuation)
{
    static_assert(std::constructible_from<
        std::remove_cv_t<p_minimizer>,
        std::uint64_t,
        std::uint64_t,
        std::uint64_t>);

    const auto kernel = selected_minimizer_kernel(table, id);
    if (kernel == nullptr) {
        throw std::runtime_error(
            "selected Highway minimizer table contains a null slot");
    }
    highway_minimizer_sink_state<p_minimizer, p_collector> sink_state{collector};
    ::bioseq::minimizers::minimizer_kernel_invocation invocation{
        .sequence = sequence,
        .window = window,
        .kmer = kmer,
        .output_layout =
            ::bioseq::minimizers::k_minimizer_output_layout<p_minimizer>,
        .output_hash_mask = static_cast<std::uint64_t>(
            std::remove_cv_t<p_minimizer>::hash_field_mask),
        .output_position_mask = static_cast<std::uint64_t>(
            std::remove_cv_t<p_minimizer>::pos_field_mask),
        .two_stack_route = two_stack_route,
        .sink = sink_state.sink(),
    };
    auto result = kernel(invocation);
    sink_state.validate(result);
    if (result.stats.algorithm != expected_algorithm) {
        throw std::runtime_error(
            "selected Highway minimizer returned the wrong family stats");
    }
    return result;
}

[[nodiscard]] constexpr two_stack_continuation_status
two_stack_transport_failure_status(
    const ::bioseq::minimizers::minimizer_kernel_result& result) noexcept
{
    const auto family = static_cast<two_stack_continuation_status>(
        result.family_status);
    if (family != two_stack_continuation_status::ok) {
        return family;
    }
    switch (result.status) {
        case ::bioseq::minimizers::minimizer_kernel_status::
                invalid_sequence_view:
            return two_stack_continuation_status::invalid_range;
        case ::bioseq::minimizers::minimizer_kernel_status::
                invalid_continuation:
            return two_stack_continuation_status::invalid_state;
        case ::bioseq::minimizers::minimizer_kernel_status::sink_rejected:
            return two_stack_continuation_status::collector_overflow;
        default:
            return two_stack_continuation_status::invalid_static_configuration;
    }
}

template <minimizer_kernel_kind p_kind, class p_minimizer,
          sequence_alphabet p_alphabet, class p_collector>
[[gnu::always_inline]] inline two_stack_execution_stats minimizer_dispatch(
    p_collector& c,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    std::size_t w,
    std::size_t k,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway)
{
    if constexpr (p_kind == minimizer_kernel_kind::exact_simd) {
        if (seq.bp_size() > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            throw exact_simd_runtime_error{
                two_stack_continuation_status::arithmetic_overflow,
                "query minimizer sketch"};
        }
        std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::
            resident_packed_sequence descriptor{};
        const auto native_resident = try_minimizer_resident_packed(
            seq, 0u, seq.bp_size());
        const bool borrowed_resident = native_resident.has_value();
        if (borrowed_resident) {
            descriptor = *native_resident;
        } else {
            const auto access = bioseq::storage::plan_sequence_access(
                seq,
                bioseq::storage::sequence_access_requirement::
                    contiguous_resident,
                0u,
                seq.bp_size(),
                4u);
            auto permit =
                bioseq::parallelism::memory_admission_controller::instance()
                    .try_reserve(access.admission_bytes);
            if (!permit) {
                throw exact_simd_runtime_error{
                    two_stack_continuation_status::non_contiguous_range,
                    "query minimizer sketch admission"};
            }
            owner.emplace(bioseq::storage::materialize_packed_sequence(
                seq, 0u, seq.bp_size(), 4u, std::move(*permit)));
            descriptor = owner->descriptor();
        }
        auto route = ::bioseq::minimizers::two_stack_route::
            chronological_continuation;
        if constexpr (std::same_as<p_alphabet, ::seq::dna4>
                      && exact_simd_minimizer_representation<p_minimizer>) {
            if (borrowed_resident
                && descriptor.storage_unit_width == sizeof(std::uint32_t)
                && w == 10u && k == 15u) [[likely]] {
                route = ::bioseq::minimizers::two_stack_route::resident_report76;
            }
        }
        const auto result = invoke_selected_minimizer_kernel<p_minimizer>(
            highway,
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_two_stack_nthash,
            ::bioseq::minimizers::minimizer_kernel_algorithm::two_stack_nthash,
            minimizer_kernel_view<p_alphabet>(descriptor, highway),
            w,
            k,
            c,
            route);
        if (!result.is_ok()) {
            throw exact_simd_runtime_error{
                two_stack_transport_failure_status(result),
                "query minimizer sketch"};
        }
        if (!result.stats.two_stack.is_internally_consistent(
                /*require_nonempty_vector_work=*/false)) {
            throw exact_simd_runtime_error{
                two_stack_continuation_status::invalid_state,
                "query minimizer sketch stats"};
        }
        return result.stats.two_stack;
    } else if constexpr (p_kind == minimizer_kernel_kind::simd) {
        if constexpr (bioseq::storage::lazy_sequence_view<p_alphabet>::
                          traits::logical_bits_per_symbol != 2u) {
            // Preserve the established reference-simd contract for alphabets
            // whose symbols are not DNA4's two-bit ranks. They use the scalar
            // minimizer route without consulting or materializing for a
            // Highway kernel.
            collect_minimizers_segment_scalar<p_minimizer>(
                c, seq, 0u, seq.bp_size(), w, k);
            return {};
        }
        const auto run_reference = [&](const auto& p_descriptor) {
            const auto result =
                invoke_selected_minimizer_kernel<p_minimizer>(
                    highway,
                    ::bioseq::simd_dispatch::highway_kernel::
                        minimizer_minimap2_reference,
                    ::bioseq::minimizers::minimizer_kernel_algorithm::
                        minimap2_reference,
                    minimizer_kernel_view<p_alphabet>(p_descriptor, highway),
                    w,
                    k,
                    c);
            if (!result.is_ok()
                || result.stats.reference.kernel_calls == 0u) {
                throw std::runtime_error(
                    "selected Highway reference minimizer declined");
            }
        };
        if (auto descriptor = try_word_minimizer_resident_packed(
                seq, 0u, seq.bp_size()); descriptor.has_value()) {
            run_reference(*descriptor);
            return {};
        }
        const auto access = bioseq::storage::plan_sequence_access(
            seq,
            bioseq::storage::sequence_access_requirement::
                contiguous_resident,
            0u,
            seq.bp_size(),
            sizeof(std::uint32_t));
        std::optional<
            bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::
            resident_packed_sequence descriptor{};
        if (access.route
                == bioseq::storage::sequence_access_route::borrow_resident
            && access.resident.has_value()) {
            descriptor = *access.resident;
        } else {
            auto permit = bioseq::parallelism::
                memory_admission_controller::instance().try_reserve(
                    access.admission_bytes);
            if (!permit) {
                throw std::bad_alloc{};
            }
            owner.emplace(bioseq::storage::materialize_packed_sequence(
                seq, 0u, seq.bp_size(), sizeof(std::uint32_t),
                std::move(*permit)));
            descriptor = owner->descriptor();
        }
        run_reference(descriptor);
    } else {
        collect_minimizers_segment_scalar<p_minimizer>(
            c, seq, 0u, seq.bp_size(), w, k);
    }
    return {};
}

template <minimizer_kernel_kind p_kind, std::size_t p_window_size, std::size_t p_kmer_size,
          class p_minimizer, sequence_alphabet p_alphabet, class p_collector>
[[gnu::always_inline]] inline two_stack_execution_stats
minimizer_dispatch_constexpr(
    p_collector& c,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    std::size_t w,
    std::size_t k,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway)
{
    if (w != p_window_size || k != p_kmer_size) {
        throw std::invalid_argument(
            "constexpr minimizer dispatch received mismatched p_window_size/p_kmer_size");
    }
    return minimizer_dispatch<p_kind, p_minimizer>(
        c, seq, p_window_size, p_kmer_size, highway);
}

//  — SUB-RANGE minimizer collection over storage units `[lo_unit, hi_unit)`.
// Used by `minimap_port_segmented` to sketch one target SEGMENT at a time,
// forwarding explicit begin/end iterators (advanced to the unit-aligned segment
// bounds) so positions come out LOCAL to `lo_unit*lpu`.
//
// IMPORTANT: this ALWAYS uses the bit-identical scalar `collect_minimizers_view`,
// even for contiguous (no-fragment) sequences. The simd two-stack kernel derives
// its iteration length from the WHOLE sequence (`calculate_iteration_offsets` over
// `seq.bp_size()`), not from `(end - begin)`, so handing it a sub-range start/end
// makes it read past the requested window — garbage minimizers + an out-of-bounds
// crash (observed: a 206 Mbp segment of a 500 Mbp target reported 99.7M minimizers
// — more than the WHOLE sequence — then SIGSEGV). The scalar view honours
// `seq_begin/seq_end` exactly and emits `seq_pos` from 0 at `seq_begin`, which is
// what position globalization and scalar/SIMD equivalence require. Scalar
// collection over contiguous memory is cheap because alignment dominates the
// per-segment wall clock. The shared scalar sub-range body lives in
// `collect_minimizers_segment_scalar` (bioseq.sequences.kmers). Under
// `vulkan_segment` the GPU segment sketch is called and throws on any decline.
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
/// * the resident vector_stream route restored from the report-76 fast path;
/// * the chronological continuation/B8 route used by persisted and fragmented
///   exact_simd index producers.
///
/// A query cannot select between them from its own storage shape.  Indexed
/// consumers must therefore require the continuation route explicitly; only a
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

[[nodiscard]] inline bool exact_simd_stats_matches_range_route(
    exact_simd_range_route route,
    const two_stack_execution_stats& stats) noexcept
{
    if (route == exact_simd_range_route::require_resident_vector_stream) {
        return stats.resident_vector_stream_kernel_calls > 0u;
    }
    // Check every resident-only field, rather than only the call count, so a
    // partially-populated or mixed stats also fails closed.
    return stats.resident_vector_stream_kernel_calls == 0u
        && stats.resident_vector_stream_single_calls == 0u
        && stats.resident_vector_stream_dual_calls == 0u
        && stats.resident_vector_stream_nonempty_work_calls == 0u
        && stats.resident_vector_stream_input_bp == 0u
        && stats.resident_vector_stream_minimizers_emitted == 0u;
}

/// Compare the biological selection sub-route actually attested by two
/// exact_simd operations. This is required even when both operations carry the
/// same high-level exact_simd backend label: resident vector_stream and
/// continuation/B8 stats are not lookup-compatible.
[[nodiscard]] inline bool exact_simd_stats_share_selection_route(
    const two_stack_execution_stats& lhs,
    const two_stack_execution_stats& rhs) noexcept
{
    if (!lhs.is_internally_consistent(/*require_nonempty_vector_work=*/false)
        || !rhs.is_internally_consistent(/*require_nonempty_vector_work=*/false)) {
        return false;
    }
    const bool lhs_resident =
        lhs.resident_vector_stream_kernel_calls > 0u;
    const bool rhs_resident =
        rhs.resident_vector_stream_kernel_calls > 0u;
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
        exact_simd_range_route::require_continuation) noexcept
{
    const bool scalar_absent = !stats.scalar_available
        && stats.scalar == scalar_minimizer_execution_stats{};
    const bool exact_absent = !stats.exact_simd_available
        && stats.exact_simd == two_stack_execution_stats{};
    const bool bounded_absent = !stats.bounded_nthash_available
        && stats.bounded_nthash == bounded_nthash_execution_stats{};

    switch (backend) {
        case minimizer_sketch_backend::exact_simd:
            return scalar_absent
                && bounded_absent
                && stats.exact_simd_available
                && stats.exact_simd.is_internally_consistent(require_nonempty_vector_work)
                && exact_simd_stats_matches_range_route(
                    exact_simd_route, stats.exact_simd);
        case minimizer_sketch_backend::bounded_nthash_simd:
            return scalar_absent
                && exact_absent
                && stats.bounded_nthash_available
                && stats.bounded_nthash.is_internally_consistent(require_nonempty_vector_work);
        case minimizer_sketch_backend::scalar:
            return stats.scalar_available
                && stats.scalar.is_coherent(require_nonempty_vector_work)
                && exact_absent
                && bounded_absent;
        case minimizer_sketch_backend::vulkan_segment:
            return false;
    }
    return false;
}

template <class p_minimizer, sequence_alphabet p_alphabet, class p_collector>
[[gnu::always_inline]] inline minimizer_range_execution_stats
minimizer_dispatch_range(
    p_collector& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> seq,
    sequence_size_t lo_bp,
    sequence_size_t hi_bp,
    std::size_t window_size,
    std::size_t kmer_size,
    minimizer_sketch_backend backend,
    exact_simd_range_route exact_simd_route =
        exact_simd_range_route::require_continuation,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway = nullptr,
    exact_simd_input_storage exact_simd_input_storage =
        exact_simd_input_storage::preserve_native_width)
{
    if (lo_bp > hi_bp || hi_bp > seq.bp_size()) {
        throw std::invalid_argument(
            "minimizer range has an invalid biological-coordinate shape");
    }
    if (backend == minimizer_sketch_backend::vulkan_segment) {
        throw std::invalid_argument(
            "Vulkan minimizer range requires an admitted GPU upload owner");
    }
    if (backend == minimizer_sketch_backend::bounded_nthash_simd) {
        if (window_size != 10u || kmer_size != 15u) {
            throw bounded_nthash_runtime_error(
                "bounded_nthash_simd currently requires p_window_size=10 and p_kmer_size=15; "
                "fallback is forbidden");
        }
        if constexpr (!std::same_as<p_alphabet, ::seq::dna4>) {
            throw bounded_nthash_runtime_error(
                "bounded_nthash_simd requires the DNA4 alphabet; fallback is "
                "forbidden");
        } else {
            auto run_bounded = [&](const auto& p_descriptor) {
                return invoke_selected_minimizer_kernel<p_minimizer>(
                    highway,
                    ::bioseq::simd_dispatch::highway_kernel::
                        minimizer_bounded_nthash,
                    ::bioseq::minimizers::minimizer_kernel_algorithm::
                        bounded_nthash,
                    minimizer_kernel_view<p_alphabet>(p_descriptor, highway),
                    window_size,
                    kmer_size,
                    collector);
            };
            ::bioseq::minimizers::minimizer_kernel_result result{};
            const auto access = bioseq::storage::plan_sequence_access(
                seq,
                bioseq::storage::sequence_access_requirement::
                    contiguous_resident,
                lo_bp,
                hi_bp,
                sizeof(std::uint32_t));
            if (access.route
                    == bioseq::storage::sequence_access_route::borrow_resident
                && access.resident.has_value()) {
                result = run_bounded(*access.resident);
            } else {
                bioseq::parallelism::memory_admission_failure failure{};
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(access.admission_bytes, &failure);
                if (!permit) {
                    throw bounded_nthash_runtime_error(fmt::format(
                        "packed-range admission failed: reason={} "
                        "requested_bytes={} reserved_bytes={} budget_bytes={}",
                        bioseq::parallelism::
                            memory_admission_failure_reason_name(
                                failure.reason),
                        failure.requested_bytes,
                        failure.reserved_bytes,
                        failure.budget_ceiling_bytes));
                }
                auto owner = bioseq::storage::materialize_packed_sequence(
                    seq, lo_bp, hi_bp, sizeof(std::uint32_t),
                    std::move(*permit));
                result = run_bounded(owner.descriptor());
            }
            if (!result.is_ok()
                || !result.stats.bounded_nthash.is_internally_consistent(
                    /*require_nonempty_vector_work=*/false)) {
                throw bounded_nthash_runtime_error(fmt::format(
                    "checked resident kernel declined without fallback: "
                    "status={} lo_bp={} hi_bp={} w={} k={}",
                    static_cast<unsigned>(result.status),
                    lo_bp, hi_bp, window_size, kmer_size));
            }
            return minimizer_range_execution_stats{
                .bounded_nthash = result.stats.bounded_nthash,
                .bounded_nthash_available = true,
            };
        }
    }
    if (backend == minimizer_sketch_backend::exact_simd) {
        const bool require_resident = exact_simd_route
            == exact_simd_range_route::require_resident_vector_stream;
        if (require_resident) {
            if constexpr (!std::same_as<p_alphabet, ::seq::dna4>
                          || !exact_simd_minimizer_representation<p_minimizer>) {
                throw std::invalid_argument(
                    "resident exact_simd vector_stream requires DNA4 and an "
                    "exact simd minimizer representation");
            } else {
                if (lo_bp != 0u || hi_bp != seq.bp_size()
                    || window_size != 10u || kmer_size != 15u) {
                    throw std::invalid_argument(
                        "resident exact_simd vector_stream requires the whole "
                        "p_window_size=10/p_kmer_size=15 range");
                }
            }
        }
        const sequence_size_t length = hi_bp - lo_bp;
        if (length > static_cast<sequence_size_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            throw exact_simd_runtime_error{
                two_stack_continuation_status::arithmetic_overflow,
                "target minimizer range sketch"};
        }
        std::optional<bioseq::storage::admitted_packed_sequence<p_alphabet>> owner;
        typename bioseq::storage::lazy_sequence_view<p_alphabet>::
            resident_packed_sequence descriptor{};
        const bool require_indexed_u32 = exact_simd_input_storage
            == exact_simd_input_storage::require_indexed_u32;
        const auto native_resident = require_resident || require_indexed_u32
            ? std::optional<typename bioseq::storage::
                lazy_sequence_view<p_alphabet>::resident_packed_sequence>{}
            : try_minimizer_resident_packed(seq, lo_bp, hi_bp);
        if (native_resident.has_value()) {
            descriptor = *native_resident;
        } else {
            const auto access = bioseq::storage::plan_sequence_access(
                seq,
                bioseq::storage::sequence_access_requirement::
                    contiguous_resident,
                lo_bp,
                hi_bp,
                4u);
            if (access.route
                    == bioseq::storage::sequence_access_route::borrow_resident
                && access.resident.has_value()) {
                descriptor = *access.resident;
            } else {
                auto permit =
                    bioseq::parallelism::memory_admission_controller::instance()
                        .try_reserve(access.admission_bytes);
                if (!permit) {
                    throw exact_simd_runtime_error{
                        two_stack_continuation_status::non_contiguous_range,
                        require_resident
                            ? "resident vector_stream admission"
                            : "target minimizer range sketch admission"};
                }
                owner.emplace(bioseq::storage::materialize_packed_sequence(
                    seq, lo_bp, hi_bp, 4u, std::move(*permit)));
                descriptor = owner->descriptor();
            }
        }
        const auto route = require_resident
            ? ::bioseq::minimizers::two_stack_route::resident_report76
            : ::bioseq::minimizers::two_stack_route::
                chronological_continuation;
        const auto result = invoke_selected_minimizer_kernel<p_minimizer>(
            highway,
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_two_stack_nthash,
            ::bioseq::minimizers::minimizer_kernel_algorithm::two_stack_nthash,
            minimizer_kernel_view<p_alphabet>(descriptor, highway),
            window_size,
            kmer_size,
            collector,
            route);
        if (!result.is_ok()) {
            throw exact_simd_runtime_error{
                two_stack_transport_failure_status(result),
                require_resident
                    ? "resident vector_stream minimizer sketch"
                    : "target minimizer range sketch"};
        }
        if (!result.stats.two_stack.is_internally_consistent(
                /*require_nonempty_vector_work=*/false)
            || !exact_simd_stats_matches_range_route(
                exact_simd_route, result.stats.two_stack)) {
            throw exact_simd_runtime_error{
                two_stack_continuation_status::invalid_state,
                "target minimizer range sketch stats"};
        }
        return minimizer_range_execution_stats{
            .exact_simd = result.stats.two_stack,
            .exact_simd_available = true,
        };
    }
    if (backend != minimizer_sketch_backend::scalar) {
        throw std::invalid_argument(
            "minimizer range received an unsupported backend");
    }
    collect_minimizers_segment_scalar<p_minimizer>(
        collector, seq, lo_bp, hi_bp, window_size, kmer_size);
    scalar_minimizer_execution_stats stats{};
    stats.record_input(hi_bp - lo_bp);
    return minimizer_range_execution_stats{
        .scalar = stats,
        .scalar_available = true,
    };
}

/// Dispatch one complete indexed-query sketch through the canonical facade.
template <class p_minimizer, sequence_alphabet p_alphabet, class p_collector>
[[gnu::always_inline]] inline minimizer_range_execution_stats
minimizer_dispatch_indexed_query_whole(
    p_collector& collector,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_sequence,
    std::size_t window_size,
    std::size_t kmer_size,
    minimizer_sketch_backend backend,
    const ::bioseq::simd_dispatch::highway_kernel_table* highway = nullptr) {
    return minimizer_dispatch_range<p_minimizer>(
        collector,
        query_sequence,
        0u,
        query_sequence.bp_size(),
        window_size,
        kmer_size,
        backend,
        exact_simd_range_route::require_continuation,
        highway,
        exact_simd_input_storage::require_indexed_u32);
}

// Resolve the biological target-sketch backend from the minimizer selector
// itself. Chaining and alignment choices must not silently substitute scalar
// sketch semantics for an explicitly selected exact_simd/bounded ntHash producer.
// Unsupported simd shapes fail in their checked drivers.
[[nodiscard]] inline minimizer_sketch_backend
target_index_sketch_backend_from_params(const minimap2_parameters& params) noexcept
{
    if (params.minimizer_algorithm
        == minimizer_algorithm::bounded_nthash_simd) {
        return minimizer_sketch_backend::bounded_nthash_simd;
    }
    if (params.vulkan_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm
            == minimizer_algorithm::vulkan_minimap_integer) {
        return minimizer_sketch_backend::vulkan_segment;
    }
    if (params.minimizer_algorithm
        == minimizer_algorithm::two_stack_nthash) {
        return minimizer_sketch_backend::exact_simd;
    }
    return minimizer_sketch_backend::scalar;
}

//  — segment GEOMETRY. The minimap_sketch_data position field is 28 bits
// (minimizer_representation<28,15>), so a segment's COLLECTED span — core + an overlap on
// each side — must stay within that field, or the global position handed back by
// `add_position_offset` would be silently masked by the packing ctor
// (`pos_v & pos_field_mask`, index.cppm) and corrupt the seed. We cap the core at
// 200 Mbp and each overlap at 30 Mbp; the static_assert ties both to the REAL
// field width so neither can be bumped past 2^28 without a compile error.
inline constexpr sequence_size_t k_segment_core_max_bp    = 200'000'000ull;  // core ceiling
inline constexpr sequence_size_t k_segment_overlap_max_bp =  30'000'000ull;  // per-side overlap ceiling
inline constexpr sequence_size_t k_segment_min_bp        =  32'000'000ull;  // floor (keep batches useful)
inline constexpr sequence_size_t k_vulkan_segment_min_bp   =   1'000'000ull;  // floor when GPU sketch is selected
// The 76M core and 96M collected-span bounds keep each admitted Vulkan sketch
// within the fixed position layout while limiting upload-window count.
inline constexpr sequence_size_t k_vulkan_segment_target_bp = 76'000'000ull;  // target core before query-overlap clamping
inline constexpr sequence_size_t k_vulkan_segment_collected_max_bp =
    96'000'000ull;
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
        bioseq::parallelism::memory_admission_controller::instance()
            .budget_ceiling();
    // allocatable_bytes() == UINT64_MAX when the mach/POSIX probe is unavailable;
    // dividing keeps it huge so the clamp yields k_segment_core_max_bp (the default).
    const std::uint64_t by_budget =
        budget / bioseq::parallelism::k_minimizer_index_peak_bytes_per_bp;
    const sequence_size_t want =
        (by_budget > k_segment_core_max_bp) ? k_segment_core_max_bp
                                        : static_cast<sequence_size_t>(by_budget);
    return want < k_segment_min_bp ? k_segment_min_bp : want;
}

[[nodiscard]] inline sequence_size_t segment_bp_from_budget_divided(
        unsigned divisor,
        sequence_size_t floor_bp = k_segment_min_bp) noexcept {
    const sequence_size_t base = segment_bp_from_budget();
    const sequence_size_t divided = divisor <= 1 ? base : base / divisor;
    return divided < floor_bp ? floor_bp : divided;
}

[[nodiscard]] inline sequence_size_t segment_overlap_bp(
        sequence_size_t query_len) noexcept {
    sequence_size_t overlap_bp = query_len + query_len / 4 + 100'000;
    if (overlap_bp > k_segment_overlap_max_bp) {
        overlap_bp = k_segment_overlap_max_bp;
    }
    return overlap_bp;
}

[[nodiscard]] inline sequence_size_t vulkan_segment_target_bp(
        sequence_size_t query_len) noexcept {
    const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
    const sequence_size_t expanded_overlap =
        overlap_bp > k_vulkan_segment_collected_max_bp / 2
            ? k_vulkan_segment_collected_max_bp
            : overlap_bp * 2;
    if (expanded_overlap + k_vulkan_segment_min_bp
        >= k_vulkan_segment_collected_max_bp) {
        return k_vulkan_segment_min_bp;
    }
    const sequence_size_t room =
        k_vulkan_segment_collected_max_bp - expanded_overlap;
    const sequence_size_t target = std::min(room, k_vulkan_segment_target_bp);
    return target < k_vulkan_segment_min_bp ? k_vulkan_segment_min_bp : target;
}

[[nodiscard]] inline unsigned
segment_geometry_divisor_for_backend(
        minimizer_sketch_backend backend,
        sequence_size_t query_len = 0) noexcept {
    if (backend != minimizer_sketch_backend::vulkan_segment) {
        return 1u;
    }
    const sequence_size_t base = segment_bp_from_budget();
    const sequence_size_t target = vulkan_segment_target_bp(query_len);
    const sequence_size_t divisor =
        (base + target - sequence_size_t{1}) / target;
    return static_cast<unsigned>(std::max<sequence_size_t>(1, divisor));
}

[[nodiscard]] inline sequence_size_t
segment_core_floor_for_backend(minimizer_sketch_backend backend) noexcept {
    return backend == minimizer_sketch_backend::vulkan_segment
        ? k_vulkan_segment_min_bp
        : k_segment_min_bp;
}

// — the segment geometry of `minimap_port_segmented`, extracted
// so `dispatch_buffer`'s stored-index assemble loop reuses the EXACT recompute
// geometry. NOT a pure function of its args: it reads the runtime budget via
// `segment_bp_from_budget()` exactly as recompute does. Each entry covers a
// segment's overlapped collection range `[cs_u, ce_u)` (in units) and its core
// claim band `[core_lo_bp, core_hi_bp)`.
[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range> segment_unit_ranges(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        unsigned divisor,
        sequence_size_t floor_bp) {
    std::vector<::bioseq::indexing::segment_unit_range> out;
    if (total_bp == 0 || lpu == 0) return out;
    const sequence_size_t total_units = (total_bp + lpu - 1) / lpu;
    const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
    const sequence_size_t ovl_units = (overlap_bp + lpu - 1) / lpu;
    sequence_size_t seg_units =
        segment_bp_from_budget_divided(divisor, floor_bp) / lpu;
    if (seg_units == 0) seg_units = 1;
    for (sequence_size_t lo_u = 0; lo_u < total_units; lo_u += seg_units) {
        const sequence_size_t hi_u = std::min(lo_u + seg_units, total_units);
        const sequence_size_t cs_u = (lo_u > ovl_units) ? (lo_u - ovl_units) : 0;
        const sequence_size_t ce_u = std::min(hi_u + ovl_units, total_units);
        const sequence_size_t core_lo_bp = lo_u * lpu;
        const sequence_size_t core_hi_bp = std::min(hi_u * lpu, total_bp);
        out.push_back(::bioseq::indexing::segment_unit_range{
            cs_u, ce_u, core_lo_bp, core_hi_bp});
    }
    return out;
}

[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu, unsigned divisor) {
    return segment_unit_ranges(total_bp, query_len, lpu, divisor, k_segment_min_bp);
}

[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu) {
    return segment_unit_ranges(total_bp, query_len, lpu, 1);
}

[[nodiscard]] inline std::vector<::bioseq::indexing::segment_unit_range> segment_unit_ranges_for_backend(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        minimizer_sketch_backend backend) {
    return segment_unit_ranges(
        total_bp,
        query_len,
        lpu,
        segment_geometry_divisor_for_backend(backend, query_len),
        segment_core_floor_for_backend(backend));
}

[[nodiscard]] inline std::pair<sequence_size_t, sequence_size_t>
validated_segment_bp_bounds(
        const ::bioseq::indexing::segment_unit_range& range,
        std::size_t letters_per_unit,
        sequence_size_t total_bp) {
    const sequence_size_t lpu =
        static_cast<sequence_size_t>(letters_per_unit);
    if (letters_per_unit == 0u
        || static_cast<std::size_t>(lpu) != letters_per_unit) {
        throw std::invalid_argument(
            "segment letters-per-unit is not representable");
    }
    constexpr sequence_size_t max_bp =
        (std::numeric_limits<sequence_size_t>::max)();
    if (range.cs_u > max_bp / lpu || range.ce_u > max_bp / lpu) {
        throw std::overflow_error(
            "segment packed-unit bounds overflow biological coordinates");
    }
    const sequence_size_t lo_bp = range.cs_u * lpu;
    const sequence_size_t hi_bp =
        std::min<sequence_size_t>(range.ce_u * lpu, total_bp);
    if (lo_bp > hi_bp || lo_bp > total_bp) {
        throw std::invalid_argument(
            "segment packed-unit bounds exceed the biological sequence");
    }
    return {lo_bp, hi_bp};
}

// ---------------------------------------------------------------------------
// minimizer_algorithm kernel traits
// ---------------------------------------------------------------------------
//
// The minimizer kernels are templated on the sketch-collector and the
// sequence range, both of which depend on the minimizer_stage's `p_minimizer`
// and `p_range` template parameters. The trait's `call` method is therefore a
// member template — the stage instantiates it with the correct types at the
// call site.

/// scalar minimizer extraction.
template <>
struct algorithm_traits<::bioseq::simd::minimizer_algorithm::scalar> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_stats = true;
        static constexpr bool emits_exact_simd_execution_stats = false;
        static constexpr bool emits_bounded_nthash_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        template <class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static scalar_minimizer_execution_stats call(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<minimizer_kernel_kind::scalar, p_minimizer>(
                collector, range, window_size, kmer_size,
                params.highway_kernels);
            scalar_minimizer_execution_stats stats{};
            stats.record_input(static_cast<std::uint64_t>(range.bp_size()));
            return stats;
        }

        template <std::size_t p_window_size, std::size_t p_kmer_size,
                  class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static scalar_minimizer_execution_stats call_constexpr(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range)
        {
            minimizer_dispatch_constexpr<minimizer_kernel_kind::scalar, p_window_size, p_kmer_size, p_minimizer>(
                collector, range, p_window_size, p_kmer_size, params.highway_kernels);
            scalar_minimizer_execution_stats stats{};
            stats.record_input(static_cast<std::uint64_t>(range.bp_size()));
            return stats;
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::minimizer_algorithm_name(
                ::bioseq::simd::minimizer_algorithm::scalar);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters&,
            std::vector<named_property>&) noexcept {}
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "false"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Reference simd minimizer extraction (single-stack).
template <>
struct algorithm_traits<::bioseq::simd::minimizer_algorithm::simd_minimap2_reference> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_stats = false;
        static constexpr bool emits_exact_simd_execution_stats = false;
        static constexpr bool emits_bounded_nthash_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_minimap2_reference;
        template <class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static void call(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<minimizer_kernel_kind::simd, p_minimizer>(
                collector, range, window_size, kmer_size,
                params.highway_kernels);
        }

        template <std::size_t p_window_size, std::size_t p_kmer_size,
                  class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static void call_constexpr(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range)
        {
            minimizer_dispatch_constexpr<minimizer_kernel_kind::simd, p_window_size, p_kmer_size, p_minimizer>(
                collector, range, p_window_size, p_kmer_size, params.highway_kernels);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::minimizer_algorithm_name(
                ::bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters&,
            std::vector<named_property>&) noexcept {}
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "true"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Two-stack nthash minimizer extraction (current default).
template <>
struct algorithm_traits<::bioseq::simd::minimizer_algorithm::two_stack_nthash> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_stats = false;
        static constexpr bool emits_exact_simd_execution_stats = true;
        static constexpr bool emits_bounded_nthash_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_two_stack_nthash;
        template <class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static two_stack_execution_stats call(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            return minimizer_dispatch<minimizer_kernel_kind::exact_simd, p_minimizer>(
                collector, range, window_size, kmer_size,
                params.highway_kernels);
        }

        template <std::size_t p_window_size, std::size_t p_kmer_size,
                  class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static two_stack_execution_stats call_constexpr(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range)
        {
            return minimizer_dispatch_constexpr<minimizer_kernel_kind::exact_simd, p_window_size, p_kmer_size, p_minimizer>(
                collector, range, p_window_size, p_kmer_size, params.highway_kernels);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::minimizer_algorithm_name(
                ::bioseq::simd::minimizer_algorithm::two_stack_nthash);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters&,
            std::vector<named_property>&) noexcept {}
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "true"},
                {"two_stack",  "true"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Bounded-memory unsigned-32-bit sum ntHash SIMD minimizer extraction. This is
/// deliberately distinct from two_stack_nthash/exact_simd: both the execution
/// stats and persisted scheme identity must match this selector.
template <>
struct algorithm_traits<::bioseq::simd::minimizer_algorithm::bounded_nthash_simd> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_stats = false;
        static constexpr bool emits_exact_simd_execution_stats = false;
        static constexpr bool emits_bounded_nthash_execution_stats = true;
        static constexpr bool emits_partial_vulkan_execution_stats = false;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::
                minimizer_bounded_nthash;

        template <class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static bounded_nthash_execution_stats call(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            const minimizer_range_execution_stats stats =
                minimizer_dispatch_range<p_minimizer>(
                    collector,
                    range,
                    0,
                    static_cast<sequence_size_t>(range.size()),
                    window_size,
                    kmer_size,
                    minimizer_sketch_backend::bounded_nthash_simd,
                    exact_simd_range_route::require_continuation,
                    params.highway_kernels);
            if (!stats.bounded_nthash_available) {
                throw bounded_nthash_runtime_error(
                    "bounded_nthash_simd leaf returned without bounded ntHash execution stats");
            }
            return stats.bounded_nthash;
        }

        template <std::size_t p_window_size, std::size_t p_kmer_size,
                  class p_minimizer, class p_collector, class p_range>
        [[gnu::always_inline]] static bounded_nthash_execution_stats call_constexpr(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range)
        {
            return call<p_minimizer>(params, collector, range, p_window_size, p_kmer_size);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::minimizer_algorithm_name(
                ::bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters&,
            std::vector<named_property>&) noexcept {}
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised", "true"},
                {"logical_streams", "8"},
                {"hash", "bounded-u32-nthash-r7-sum"},
                {"fallback", "forbidden"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Vulkan minimap-integer minimizer extraction. Reads packed
/// `compact_sequence_range` storage directly on the GPU. When this algorithm
/// is selected, Vulkan execution is mandatory and failures throw.
/// `call_result` exposes the resident device-minimizer buffer so the all-Vulkan
/// sketch+seed path reuses it without a second upload (`may_emit_device_minimizers`).
template <>
struct algorithm_traits<::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer> {
    struct kernel_type {
        static constexpr bool emits_scalar_minimizer_execution_stats = false;
        static constexpr bool emits_exact_simd_execution_stats = false;
        static constexpr bool emits_bounded_nthash_execution_stats = false;
        static constexpr bool emits_partial_vulkan_execution_stats = true;
        static constexpr bool may_emit_device_minimizers = true;

        template <class p_minimizer, class p_collector, class p_range>
        [[nodiscard, gnu::always_inline]] static
        bioseq::gpu::vulkan::vulkan_sketch_dispatch_token call(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            if (!params.vulkan_enabled) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto token =
                bioseq::gpu::vulkan::collect_minimizers_vulkan<p_minimizer>(
                    collector, range, window_size, kmer_size, &decline);
            if (!token.has_value() || !token->is_internally_consistent()) {
                throw_vulkan_sketch_declined(
                    "Vulkan minimizer sketch", decline);
            }
            return *token;
        }

        template <std::size_t p_window_size, std::size_t p_kmer_size,
                  class p_minimizer, class p_collector, class p_range>
        [[nodiscard, gnu::always_inline]] static
        bioseq::gpu::vulkan::vulkan_sketch_dispatch_token call_constexpr(
            const ::bioseq::simd::minimap2_parameters& params,
            p_collector& collector, p_range& range)
        {
            return call<p_minimizer>(params, collector, range, p_window_size, p_kmer_size);
        }

        template <class p_minimizer, class p_range>
        [[nodiscard]] static std::optional<
            bioseq::gpu::vulkan::vulkan_sketch_result<p_minimizer>>
        call_result(
            const ::bioseq::simd::minimap2_parameters& params,
            p_range& range, std::size_t window_size, std::size_t kmer_size,
            bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route
                host_ownership_route = bioseq::gpu::vulkan::
                    vulkan_sketch_host_ownership_route::collector_compatible)
        {
            if (!params.vulkan_enabled) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto result = bioseq::gpu::vulkan::collect_minimizers_vulkan_result<p_minimizer>(
                range,
                window_size,
                kmer_size,
                &decline,
                host_ownership_route);
            if (!result) {
                throw_vulkan_sketch_declined(
                    "Vulkan minimizer sketch", decline);
            }
            return result;
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::minimizer_algorithm_name(
                ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters& p,
            std::vector<named_property>& props) noexcept
        {
            props.push_back({"accelerator", "vulkan"});
            props.push_back({"vulkan_enabled", p.vulkan_enabled ? "true" : "false"});
        }
        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"hash", "minimap-int"},
                {"alphabet_dispatch", "runtime-shader-key"},
                {"specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

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
resolve_alignment_kernels(
    const ::bioseq::simd::minimap2_parameters& params)
{
    const auto* root = params.highway_kernels;
    const auto* identity = root == nullptr || !root->is_complete()
        ? nullptr : root->template resolve_alphabet_identity<p_alphabet>();
    if (identity == nullptr
        || params.alphabet_code != identity->alphabet_code
        || root->resolve_alphabet_identity(params.alphabet_code)
            != identity) {
        throw std::logic_error{
            "alignment dispatch requires an alphabet-matched Highway table"};
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
        if (entry.alphabet_code != identity->alphabet_code) continue;
        if (entry.typed_table == nullptr || resolved != nullptr) {
            throw std::logic_error{
                "selected Highway table has an invalid alignment alphabet entry"};
        }
        // The generated root reconciles this numeric slot with the concrete
        // type identity before the erased pointer can be cast.
        resolved = static_cast<
            const ::bioseq::simd::alignment_kernels<p_alphabet>*>(
                entry.typed_table);
    }
    if (resolved == nullptr
        || resolved->ksw2_z2 == nullptr
        || resolved->ksw2_d2 == nullptr
        || resolved->ksw2_batched == nullptr
        || resolved->block_prepare == nullptr
        || resolved->block_try_certified_gap == nullptr
        || resolved->block_align == nullptr) {
        throw std::logic_error{
            "selected Highway table has no complete typed alignment table"};
    }
    return *resolved;
}

/// Shared helper for ksw2 / Block Aligner / ksw2_batched — pushes the
/// four affine-scoring parameters that every base aligner consumes.
[[gnu::always_inline]] inline void push_base_affine_params(
    const ::bioseq::simd::minimap2_parameters& p,
    std::vector<named_property>& out)
{
    out.push_back({"match_score",  std::to_string(p.match_score)});
    out.push_back({"mismatch_pen", std::to_string(p.mismatch_pen)});
    out.push_back({"gap_open1",    std::to_string(p.gap_open1)});
    out.push_back({"gap_ext1",     std::to_string(p.gap_ext1)});
}

}  // namespace detail

template <>
struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2> {
    struct kernel_type {
        static constexpr bool emits_block_execution_stats = false;
        static constexpr bool emits_scalar_ksw2_execution_stats = true;
        static constexpr bool emits_ksw2_batched_execution_stats = false;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2;

        template <sequence_alphabet p_alphabet>
        [[gnu::always_inline]] static void call(
            std::span<alignment_region>                       regions,
            std::span<const ::bioseq::chaining::anchor>  anchors,
            bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
            bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
            const scoring_matrix<p_alphabet>&               scoring_matrix,
            const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
            const ::bioseq::simd::minimap2_parameters& params,
            block_aligner_execution_stats* forbidden_block_stats,
            scalar_pipeline_execution_stats* scalar_execution_stats)
        {
            if (forbidden_block_stats != nullptr) {
                block_aligner_forbidden_ksw2_fallback(*forbidden_block_stats);
            }
            ::bioseq::simd::align_regions<
                ::bioseq::simd::base_alignment_algorithm::ksw2>(
                regions, anchors, query_seq, target_seq, scoring_matrix,
                alignment_kernels, params,
                /*block_execution_stats=*/nullptr,
                scalar_execution_stats);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "ksw2";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters& p,
            std::vector<named_property>& out)
        {
            detail::push_base_affine_params(p, out);
            out.push_back({"gap_open2", std::to_string(p.gap_open2)});
            out.push_back({"gap_ext2",  std::to_string(p.gap_ext2)});
            out.push_back({"zdrop",     std::to_string(p.zdrop)});
            out.push_back({"end_bonus", std::to_string(p.end_bonus)});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised",        "intra-sequence striped"},
                {"single_affine_eq",  "data-dependent"},
                {"specialised",       p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

template <>
struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::block_aligner> {
    struct kernel_type {
        static constexpr bool emits_block_execution_stats = true;
        static constexpr bool emits_scalar_ksw2_execution_stats = false;
        static constexpr bool emits_ksw2_batched_execution_stats = false;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::
                alignment_block_aligner;

        template <sequence_alphabet p_alphabet>
        [[gnu::always_inline]] static void call(
            std::span<alignment_region>                       regions,
            std::span<const ::bioseq::chaining::anchor>  anchors,
            bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
            bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
            const scoring_matrix<p_alphabet>&               scoring_matrix,
            const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
            const ::bioseq::simd::minimap2_parameters& params,
            block_aligner_execution_stats* block_execution_stats,
            scalar_pipeline_execution_stats* /*scalar_execution_stats*/)
        {
            ::bioseq::simd::align_regions<
                ::bioseq::simd::base_alignment_algorithm::block_aligner>(
                regions, anchors, query_seq, target_seq, scoring_matrix,
                alignment_kernels, params,
                block_execution_stats);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "Block Aligner";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters& p,
            std::vector<named_property>& out)
        {
            detail::push_base_affine_params(p, out);
            out.push_back({"block_min_size",
                std::to_string(static_cast<int32_t>(p.block_min_size))});
            out.push_back({"block_max_size",
                std::to_string(static_cast<int32_t>(p.block_max_size))});
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised",       "block-affine"},
                {"single_affine_only", "true"},
                {"specialised",      p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// The ksw2_batched kernel is provided by `bioseq.alignment.ksw2_batched`.
template <>
struct algorithm_traits<::bioseq::simd::base_alignment_algorithm::ksw2_batched> {
    struct kernel_type {
        static constexpr bool emits_block_execution_stats = false;
        static constexpr bool emits_scalar_ksw2_execution_stats = false;
        static constexpr bool emits_ksw2_batched_execution_stats = true;
        static constexpr auto highway_kernel_id =
            ::bioseq::simd_dispatch::highway_kernel::
                alignment_ksw2_batched;

        /// The batched entry owns inter-region packing, but its long-fill and
        /// traceback paths execute the target-local ksw2 z2/d2 kernels as part
        /// of the same stage.  EXPLAIN therefore reports both coarse functions.
        [[nodiscard]] static constexpr
        ::bioseq::simd_dispatch::highway_kernel_mask
        highway_kernel_ids() noexcept {
            auto kernels = ::bioseq::simd_dispatch::highway_kernel_mask_for(
                ::bioseq::simd_dispatch::highway_kernel::
                    alignment_ksw2_batched);
            kernels |= ::bioseq::simd_dispatch::highway_kernel_mask_for(
                ::bioseq::simd_dispatch::highway_kernel::alignment_ksw2);
            return kernels;
        }

        template <sequence_alphabet p_alphabet>
        [[gnu::always_inline]] static void call(
            std::span<alignment_region>                       regions,
            std::span<const ::bioseq::chaining::anchor>  anchors,
            bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
            bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
            const scoring_matrix<p_alphabet>&               scoring_matrix,
            const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
            const ::bioseq::simd::minimap2_parameters& params,
            block_aligner_execution_stats* forbidden_block_stats,
            scalar_pipeline_execution_stats* /*scalar_execution_stats*/,
            ksw2_batched_execution_stats* execution_stats = nullptr)
        {
            if (forbidden_block_stats != nullptr) {
                block_aligner_forbidden_ksw2_fallback(*forbidden_block_stats);
            }
            alignment_kernels.align_ksw2_batched(
                regions, anchors, query_seq, target_seq, scoring_matrix, params,
                execution_stats);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "ksw2 (inter-region batched)";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::minimap2_parameters& p,
            std::vector<named_property>& out)
        {
            // Same runtime knobs as the intra-sequence ksw2 path; the
            // bucketing/batching is determined by the kernel from
            // params.max_gap, params.zdrop, and the per-fill geometry.
            algorithm_traits<
                ::bioseq::simd::base_alignment_algorithm::ksw2>::
                kernel_type::populate_runtime_params(p, out);
        }

        template <is_constexpr_aligner_params p_aligner_parameters>
        [[nodiscard]] static std::vector<named_property> static_properties() {
            return {
                {"vectorised",        "inter-sequence packed"},
                {"bucket_widths",     "[<=32, <=64, <=128, <=256, >256]"},
                {"long_fill_kernel",  "intra-sequence ksw2 (fallback)"},
                {"specialised",       p_aligner_parameters::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// =============================================================================
// Metadata surface — full definitions
// =============================================================================
//
// `named_property` was hoisted ahead of the algorithm_traits block so trait
// signatures can return `std::vector<named_property>`.  The remaining
// description types are defined here in their natural source order.

struct stage_algorithm_description {
    std::string                name;
    std::vector<named_property> parameters;
    std::vector<named_property> computed_properties;
    std::optional<std::string> extra_description;
    ::bioseq::simd_dispatch::highway_kernel_mask highway_kernels{};
};

struct stage_description {
    std::string                              stage_name;
    std::optional<stage_algorithm_description> algorithm;
    std::optional<std::string>               extra_description;

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
    std::optional<std::size_t>               estimated_batch_size;
};

struct pipeline_description {
    std::vector<stage_description> stages;
};

inline constexpr std::string_view query_sketch_stage_name =
    "QuerySketchStage";

/** Cold EXPLAIN-only query-sketch route; never a dispatch-chain dimension. */
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
[[nodiscard]] constexpr std::optional<
    ::bioseq::simd_dispatch::highway_kernel_mask>
planned_query_minimizer_highway_kernels(
        const ::bioseq::simd::minimap2_parameters& params,
        const query_sketch_route route) noexcept {
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    using p_kernel = ::bioseq::simd_dispatch::highway_kernel;
    if (route != query_sketch_route::recompute_capable
        && route != query_sketch_route::compatible_persisted_only) {
        return std::nullopt;
    }
    switch (params.minimizer_algorithm) {
        case algorithm_type::scalar:
        case algorithm_type::vulkan_minimap_integer:
            return ::bioseq::simd_dispatch::highway_kernel_mask{};
        case algorithm_type::simd_minimap2_reference:
            if (route
                    == query_sketch_route::compatible_persisted_only) {
                return ::bioseq::simd_dispatch::highway_kernel_mask{};
            }
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(
                p_kernel::minimizer_minimap2_reference);
        case algorithm_type::two_stack_nthash:
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(
                p_kernel::minimizer_two_stack_nthash);
        case algorithm_type::bounded_nthash_simd:
            if (params.window_size != 10
                || params.kmer_size != 15) {
                return std::nullopt;
            }
            return ::bioseq::simd_dispatch::highway_kernel_mask_for(
                p_kernel::minimizer_bounded_nthash);
    }
    return std::nullopt;
}

static_assert([]() consteval {
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
    const auto slow_persisted =
        planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::simd_minimap2_reference),
            route_type::compatible_persisted_only);
    const auto exact = planned_query_minimizer_highway_kernels(
        make_params(algorithm_type::two_stack_nthash),
        route_type::compatible_persisted_only);
    const auto bounded = planned_query_minimizer_highway_kernels(
        make_params(algorithm_type::bounded_nthash_simd),
        route_type::recompute_capable);
    const auto bounded_wrong_window = planned_query_minimizer_highway_kernels(
        make_params(algorithm_type::bounded_nthash_simd, 9, 15),
        route_type::recompute_capable);
    const auto bounded_wrong_kmer = planned_query_minimizer_highway_kernels(
        make_params(algorithm_type::bounded_nthash_simd, 10, 14),
        route_type::compatible_persisted_only);
    const auto scalar = planned_query_minimizer_highway_kernels(
        make_params(algorithm_type::scalar),
        route_type::recompute_capable);
    const auto vulkan = planned_query_minimizer_highway_kernels(
        make_params(algorithm_type::vulkan_minimap_integer),
        route_type::compatible_persisted_only);
    return slow.has_value()
        && *slow == ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::minimizer_minimap2_reference)
        && slow_persisted.has_value()
        && slow_persisted->none()
        && exact.has_value()
        && *exact == ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::minimizer_two_stack_nthash)
        && bounded.has_value()
        && *bounded == ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::minimizer_bounded_nthash)
        && !bounded_wrong_window.has_value()
        && !bounded_wrong_kmer.has_value()
        && scalar.has_value() && scalar->none()
        && vulkan.has_value() && vulkan->none()
        && !planned_query_minimizer_highway_kernels(
            make_params(algorithm_type::simd_minimap2_reference),
            static_cast<route_type>(0xffu)).has_value();
}(), "query-sketch Highway planning must remain route- and algorithm-exact");

/**
 * The describe cascade intentionally pins the executable pipeline at the
 * prebuilt-minimizer entry point, so its stage chain has no minimizer_stage.
 * Represent the query sketch as a neutral plan-only stage instead of making
 * the minimizer enum a describe-template axis. This stage is the canonical
 * owner of the query-sketch Highway functions whenever a description exists.
 */
[[nodiscard]] inline std::optional<stage_description>
describe_query_sketch_stage(
        const ::bioseq::simd::minimap2_parameters& params,
        const query_sketch_route route) {
    const auto kernels = planned_query_minimizer_highway_kernels(
        params, route);
    if (!kernels.has_value()) return std::nullopt;

    stage_description stage;
    stage.stage_name = std::string{query_sketch_stage_name};
    stage_algorithm_description algorithm;
    algorithm.name = ::bioseq::simd::minimizer_algorithm_name(
        params.minimizer_algorithm);
    algorithm.highway_kernels = *kernels;
    stage.algorithm = std::move(algorithm);
    return stage;
}

/** Number of semantic query-sketch owners in a structured description. */
[[nodiscard]] inline std::size_t query_sketch_stage_owner_count(
        const pipeline_description& description) noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(
        description.stages,
        [](const stage_description& stage) noexcept {
            return stage.stage_name == query_sketch_stage_name
                && stage.algorithm.has_value();
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
        return ::bioseq::simd_dispatch::highway_kernel_mask_for(
            p_kernel::highway_kernel_id);
    } else {
        return {};
    }
}

[[nodiscard]] constexpr std::size_t estimated_sketch_count(
    std::size_t bp_size, int32_t window_size) noexcept {
    if (window_size <= 0) return bp_size + 64u;
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
[[nodiscard]] constexpr std::size_t estimated_anchor_count(
    std::size_t sketch_count, std::size_t query_bp, std::size_t target_bp,
    int32_t kmer_size) noexcept
{
    const std::size_t shared_ceiling =
        std::min(query_bp, target_bp) > static_cast<std::size_t>(kmer_size)
            ? std::min(query_bp, target_bp) - static_cast<std::size_t>(kmer_size) + 1u
            : sketch_count;
    const std::size_t median_estimate = sketch_count * 2u;
    return std::min(median_estimate, shared_ceiling);
}

[[nodiscard]] constexpr std::size_t estimated_chain_count(
    std::size_t anchor_count, int32_t min_seed_count) noexcept {
    if (min_seed_count <= 0) return anchor_count;
    return anchor_count / static_cast<std::size_t>(min_seed_count) + 1u;
}

[[nodiscard]] constexpr std::size_t estimated_region_count(
    std::size_t chain_count, int32_t best_n) noexcept {
    return std::min(chain_count,
                    static_cast<std::size_t>(best_n) + 16u);
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
[[nodiscard]] constexpr std::size_t estimated_cigar_op_count(
    int32_t query_extent, int32_t ref_extent) noexcept {
    const int32_t q = query_extent > 0 ? query_extent : 0;
    const int32_t r = ref_extent   > 0 ? ref_extent   : 0;
    return static_cast<std::size_t>(q) + static_cast<std::size_t>(r) + 16u;
}

[[nodiscard]] constexpr std::size_t estimated_result_count(
    sequence_size_t query_len, sequence_size_t target_len,
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    const std::size_t bp_floor = std::max<std::size_t>(1u,
        static_cast<std::size_t>(std::min(query_len, target_len)) / 500u);
    return std::max(static_cast<std::size_t>(params.best_n + 4), bp_floor);
}

} // namespace detail

// =============================================================================
// Derived helpers
// =============================================================================

[[nodiscard]] constexpr int32_t sub_diff(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return params.match_score * 2 + params.mismatch_pen;
}

[[nodiscard]] constexpr int32_t min_diff(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return params.kmer_size * 2;
}

/**
 * @brief Thread-local cached scoring matrix keyed on (match, mismatch, ambi).
 *
 * Shared by the single-query and batch pipeline entry points.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard, gnu::always_inline]] inline const scoring_matrix<p_alphabet>&
thread_local_scoring_matrix(
    const ::bioseq::simd::minimap2_parameters& params)
{
    thread_local int8_t cached_match    = std::numeric_limits<int8_t>::min();
    thread_local int8_t cached_mismatch = std::numeric_limits<int8_t>::min();
    thread_local int8_t cached_ambi     = std::numeric_limits<int8_t>::min();
    thread_local scoring_matrix<p_alphabet> cached_matrix{};

    const int8_t want_match    = static_cast<int8_t>(params.match_score);
    const int8_t want_mismatch = static_cast<int8_t>(params.mismatch_pen);
    const int8_t want_ambi     = static_cast<int8_t>(params.sc_ambi);
    if (cached_match != want_match
        || cached_mismatch != want_mismatch
        || cached_ambi != want_ambi)
    {
        cached_matrix   = scoring_matrix<p_alphabet>::generate(
            want_match, want_mismatch, want_ambi);
        cached_match    = want_match;
        cached_mismatch = want_mismatch;
        cached_ambi     = want_ambi;
    }
    return cached_matrix;
}

// =============================================================================
// Concrete stages
// =============================================================================
//
// Each stage's `process*<p_aligner_parameters>(...)` body forwards to the existing kernel
// function in the target-neutral pipeline or its selected kernel table
// without renaming the kernel.  Wire-type between stages is `std::span<p_element>`
// directly; arena ownership is provided by `arena_slab<p_element>` materialised
// inside each stage before the downstream call.

/**
 * @brief Minimizer extraction stage.
 *
 * Templated on `p_kernel` — the algorithm-specific call surface exposed by
 * `algorithm_traits<minimizer_algorithm::X>::kernel_type`.
 * The stage body has zero `if constexpr` over enum
 * values; adding a new minimizer-extraction algorithm is a single trait
 * specialisation in the kernel's own module.
 *
 * The stage ignores its input batch and instead sketches the pre-captured
 * `query_seq_` pointer.
 */
template <class p_minimizer,
          class p_kernel,
          sequence_alphabet p_alphabet>
struct minimizer_stage
    : stage_base<minimizer_stage<p_minimizer, p_kernel, p_alphabet>, p_minimizer>
{
    static constexpr std::string_view stage_name = "minimizer_stage";

    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
    int32_t runtime_w;
    int32_t runtime_k;
    // — runtime flags so the Vulkan kernel can gate on
    // `vulkan_enabled` without touching runtime when Vulkan is off. CPU kernels
    // ignore it.
    ::bioseq::simd::minimap2_parameters params;
    match_result_collection* stats_dst = nullptr;
    match_result_collection* host_timing_dst = nullptr;

    // The kernel writes directly into the per-query arena slab via
    // `sketch_collector_arena_adapter`, avoiding an intermediate copy. The
    // specialised path calls `call_constexpr<p_window_size, p_kmer_size, p_minimizer>(...)` so
    // the (p_window_size, p_kmer_size) pair propagates as integer immediates through the
    // window-loop bounds and unrolls.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process(p_batch&& /*ignored*/, emit_token<p_sink>&& token) {
        const std::uint64_t host_stage_started = host_timing_dst == nullptr
            ? 0u : host_stage_now_ns();
        const std::size_t window_size = p_aligner_parameters::generic_fallback
            ? static_cast<std::size_t>(runtime_w)
            : p_aligner_parameters::window_len;
        const std::size_t kmer_size = p_aligner_parameters::generic_fallback
            ? static_cast<std::size_t>(runtime_k)
            : p_aligner_parameters::kmer_len;

        // Validate the zero-based position domain before any scalar, simd, or
        // Vulkan kernel can construct a masked minimizer_representation. A query shorter
        // than k is valid and naturally emits an empty minimizer batch.
        require_representable_minimizer_query_length<p_minimizer>(
            query_seq_.bp_size());

        // — device-minimizer path: only the Vulkan kernel exposes
        // `call_result`. When it returns a result, copy the host minimizers into
        // the arena slab and forward the resident device span as stage-local
        // metadata so seed_hit_stage can skip a second query-minimizer upload.
        if constexpr (requires { p_kernel::may_emit_device_minimizers; }) {
            if constexpr (p_kernel::may_emit_device_minimizers) {
                if (auto gpu_result =
                        p_kernel::template call_result<p_minimizer>(
                            params, query_seq_, window_size, kmer_size))
                {
                    if (stats_dst == nullptr
                        || !gpu_result->dispatch_token.is_internally_consistent()) {
                        bioseq_log<logging_level::error>(
                            "Vulkan minimizer stage lacks an operation token or stats sink");
                        std::abort();
                    }
                    partial_vulkan_execution_stats stats{};
                    stats.query_sketch_dispatches =
                        gpu_result->dispatch_token.completed_roundtrips;
                    stats_dst->record_partial_vulkan_execution_stats(stats);
                    auto device_slab = make_arena_slab<p_minimizer>();
                    device_slab.reserve(gpu_result->host_minimizers.size());
                    if (!gpu_result->host_minimizers.empty()) {
                        std::memcpy(device_slab.data(),
                                    gpu_result->host_minimizers.data(),
                                    gpu_result->host_minimizers.size() * sizeof(p_minimizer));
                    }
                    device_slab.resize(gpu_result->host_minimizers.size());
                    minimizer_batch_view<p_minimizer> batch{
                        .host = device_slab.as_const_span(),
                        .device = std::optional<
                            bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>{
                                gpu_result->device_span()},
                    };
                    // The reservation owns the vector capacity, so destroy
                    // that capacity before releasing its permit. `batch.device`
                    // independently retains the resident device permit.
                    std::vector<p_minimizer>{}.swap(
                        gpu_result->host_minimizers);
                    gpu_result->host_permit.reset();
                    return std::move(token).emit_minimizer_batch(batch);
                }
            }
        }

        auto sketches_slab = make_arena_slab<p_minimizer>();
        sketches_slab.reserve(detail::estimated_sketch_count(
            query_seq_.bp_size(), static_cast<int32_t>(window_size)));

        sketch_collector_arena_adapter<p_minimizer> collector{&sketches_slab};

        if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
            const auto dispatch_token = [&] {
                if constexpr (!p_aligner_parameters::generic_fallback) {
                    return p_kernel::template call_constexpr<
                        p_aligner_parameters::window_len, p_aligner_parameters::kmer_len, p_minimizer>(
                            params, collector, query_seq_);
                } else {
                    return p_kernel::template call<p_minimizer>(
                        params, collector, query_seq_,
                        window_size, kmer_size);
                }
            }();
            if (stats_dst == nullptr || !dispatch_token.is_internally_consistent()) {
                bioseq_log<logging_level::error>(
                    "Vulkan minimizer stage returned no accepted execution token");
                std::abort();
            }
            partial_vulkan_execution_stats stats{};
            stats.query_sketch_dispatches =
                dispatch_token.completed_roundtrips;
            stats_dst->record_partial_vulkan_execution_stats(stats);
        } else if constexpr (p_kernel::emits_scalar_minimizer_execution_stats) {
            scalar_minimizer_execution_stats stats{};
            if constexpr (!p_aligner_parameters::generic_fallback) {
                stats = p_kernel::template call_constexpr<
                    p_aligner_parameters::window_len, p_aligner_parameters::kmer_len, p_minimizer>(
                        params, collector, query_seq_);
            } else {
                stats = p_kernel::template call<p_minimizer>(
                    params, collector, query_seq_,
                    window_size, kmer_size);
            }
            if (stats_dst == nullptr || !stats.is_coherent()) {
                bioseq_log<logging_level::error>(
                    "scalar minimizer stage returned rejected stats");
                std::abort();
            }
            stats_dst->record_scalar_query_minimizer_execution_stats(stats);
        } else if constexpr (p_kernel::emits_exact_simd_execution_stats) {
            const auto stats = [&] {
                if constexpr (!p_aligner_parameters::generic_fallback) {
                    return p_kernel::template call_constexpr<
                        p_aligner_parameters::window_len, p_aligner_parameters::kmer_len, p_minimizer>(
                            params, collector, query_seq_);
                } else {
                    return p_kernel::template call<p_minimizer>(
                        params, collector, query_seq_,
                        window_size, kmer_size);
                }
            }();
            if (stats_dst == nullptr
                || !stats.is_internally_consistent(
                    /*require_nonempty_vector_work=*/false)) {
                bioseq_log<logging_level::error>(
                    "exact_simd minimizer stage returned rejected stats");
                std::abort();
            }
            stats_dst->record_exact_simd_query_execution_stats(stats);
        } else if constexpr (p_kernel::emits_bounded_nthash_execution_stats) {
            const auto stats = [&] {
                if constexpr (!p_aligner_parameters::generic_fallback) {
                    return p_kernel::template call_constexpr<
                        p_aligner_parameters::window_len, p_aligner_parameters::kmer_len, p_minimizer>(
                            params, collector, query_seq_);
                } else {
                    return p_kernel::template call<p_minimizer>(
                        params, collector, query_seq_,
                        window_size, kmer_size);
                }
            }();
            if (stats_dst == nullptr
                || !stats.is_internally_consistent(
                    /*require_nonempty_vector_work=*/false)) {
                bioseq_log<logging_level::error>(
                    "bounded_nthash_simd minimizer stage returned rejected stats");
                std::abort();
            }
            stats_dst->record_bounded_nthash_query_execution_stats(stats);
        } else if constexpr (!p_aligner_parameters::generic_fallback) {
            p_kernel::template call_constexpr<
                p_aligner_parameters::window_len, p_aligner_parameters::kmer_len, p_minimizer>(
                    params, collector, query_seq_);
        } else {
            p_kernel::template call<p_minimizer>(
                params, collector, query_seq_, window_size, kmer_size);
        }

        if (host_timing_dst != nullptr) {
            host_timing_dst->record_query_sketch_host_time(
                elapsed_host_stage_ns(host_stage_started));
        }
        return std::move(token).emit_batch(std::span<const p_minimizer>{
            sketches_slab.data(), sketches_slab.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        stage_algorithm_description algo;
        algo.name = std::string{p_kernel::algorithm_label()};
        p_kernel::populate_runtime_params({}, algo.parameters);
        algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
        algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
        desc.algorithm = std::move(algo);
        // Reserve target for the sketch arena slab (
        // "estimated_sketch_count").  `query_seq_` is a borrowed pointer; on
        // the describe path the proto is empty so bp_size() is zero — which
        // still yields the +64 safety margin from `estimated_sketch_count`.
        desc.estimated_batch_size = detail::estimated_sketch_count(
            query_seq_.bp_size(),
            runtime_w > 0 ? runtime_w : 10);
        return desc;
    }
};

/**
 * @brief Pass-through stage that decorates downstream with the target index.
 *
 * Converts the sink shape from `on_batch(span<p_minimizer>)` upstream to
 * `on_batch_with_index(span<p_minimizer>, p_index const&)` downstream.  Inlines
 * away at -O3 (zero runtime cost).
 */
template <class p_index>
struct index_stage : stage_base<index_stage<p_index>, p_index /* pass-through; downstream sees the same sketch span */> {
    static constexpr std::string_view stage_name = "index_stage";

    const p_index* index_;
    bioseq::indexing::gpu_index_cache_identity identity_{};

    template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process(p_batch&& sketches, emit_token<p_sink>&& token) {
        // /  — whole in-memory targets keep pointer
        // identity (`{}`); segmented recompute/assembly passes a nonzero
        // identity so a reused stack index object cannot alias another segment
        // in the GPU resident cache.
        if constexpr (is_minimizer_batch_view_v<p_batch>) {
            return std::move(token).emit_minimizer_batch_with_index(
                std::forward<p_batch>(sketches), *index_, identity_);
        } else {
            using element_type = std::remove_const_t<
                typename std::remove_cvref_t<p_batch>::value_type>;
            return std::move(token).emit_batch_with_index(
                std::span<const element_type>{sketches.data(), sketches.size()}, *index_, identity_);
        }
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        desc.extra_description = "target index lookup hand-off";
        return desc;
    }
};

enum class source_direct_host_authorization : std::uint8_t {
    none,
    canonical_empty,
    reflected_process_footprint,
    retained_exact_permit,
};

/// One operation-scoped ledger for the selected source-direct resident route.
/// It emits exactly one planning receipt after sort/count and exactly one
/// terminal receipt after cache/fence/arena cleanup (or unwind).  Checksums are
/// allocation-integrity observations only; biological parity remains a
/// test/benchmark property and is deliberately absent from these receipts.
struct direct_resident_operation_stats {
    std::uint64_t operation_identity = 0u;
    std::uint64_t minimizer_count = 0u;
    std::uint64_t distinct_count = 0u;
    std::uint64_t logical_control_bytes = 0u;
    std::uint64_t logical_position_bytes = 0u;
    std::uint64_t logical_resident_bytes = 0u;
    std::uint64_t control_requirement_bytes = 0u;
    std::uint64_t position_requirement_bytes = 0u;
    std::uint64_t control_allowance_bytes = 0u;
    std::uint64_t position_allowance_bytes = 0u;
    std::uint64_t conservative_allowance_bytes = 0u;
    std::uint64_t conservative_request_bytes = 0u;
    std::uint64_t reserved_before_bytes = 0u;
    std::uint64_t allocatable_before_bytes = 0u;
    std::uint64_t margin_bytes = 0u;
    std::uint64_t shortfall_bytes = 0u;
    std::uint64_t max_storage_buffer_range = 0u;
    std::uint64_t source_capacity_bytes = 0u;
    std::uint64_t source_reflected_bytes = 0u;
    std::uint64_t source_retained_permit_bytes = 0u;
    std::uint64_t source_releases = 0u;
    std::uint64_t source_released_bytes = 0u;
    std::uint64_t cache_preinsertions = 0u;
    std::uint64_t cache_required_lookups = 0u;
    std::uint64_t cache_required_hits = 0u;
    std::uint64_t mapped_prefix_passes = 0u;
    std::uint64_t mapped_prefix_lookups = 0u;
    std::uint64_t mapped_prefix_comparisons = 0u;
    std::uint64_t expected_seed_consumers = 0u;
    std::uint64_t completed_seed_consumers = 0u;
    std::uint64_t slab_capacity_before = 0u;
    std::uint64_t slab_requested_bytes = 0u;
    std::uint64_t slab_actual_bytes = 0u;
    std::uint64_t slab_retained_permit_bytes = 0u;
    std::uint64_t slab_permit_releases = 0u;
    std::uint64_t seed_reserved_bytes = 0u;
    std::uint64_t seed_planned_vulkan_bytes = 0u;
    std::uint64_t seed_actual_vulkan_bytes = 0u;
    std::uint64_t seed_roundtrips = 0u;
    std::uint64_t zero_query_bypasses = 0u;
    std::uint64_t zero_anchor_bypasses = 0u;
    std::uint64_t seed_fence_completions = 0u;
    std::uint64_t seed_dispatches = 0u;
    std::uint64_t cache_cleanup_releases = 0u;
    std::uint64_t resident_cleanup_bytes = 0u;
    std::uint64_t cpu_index_releases = 0u;
    std::uint64_t cpu_position_bytes_released = 0u;
    std::uint64_t planning_receipts = 0u;
    std::uint64_t terminal_receipts = 0u;
    bioseq::indexing::gpu_source_direct_materialization_stats materialization{};
    source_direct_host_authorization source_authority =
        source_direct_host_authorization::none;
    bool gpu_target_index_route_checked = false;
    bool gpu_target_index_route_compatible = false;
    bool source_authority_checked = false;
    bool has_valid_source_authorization = false;
    bool runtime_checked = false;
    bool runtime_available = false;
    bool hardware_checked = false;
    bool hardware_available = false;
    bool sorted_shape_checked = false;
    bool is_sorted_shape_accepted = false;
    bool identity_checked = false;
    bool is_identity_accepted = false;
    bool u32_shape_checked = false;
    bool is_u32_shape_accepted = false;
    bool storage_ranges_checked = false;
    bool is_control_range_accepted = false;
    bool is_position_range_accepted = false;
    bool mapping_plan_checked = false;
    bool is_mapping_plan_accepted = false;
    bool admission_snapshot_taken = false;
    bool canonical_empty = false;
    bool is_planning_accepted = false;
    bool route_committed = false;
    bool materialization_started = false;
    bool materialization_completed = false;
    bool resident_published = false;
    bool source_release_completed = false;
    bool source_released_after_publication = false;
    bool empty_bypass = false;
    bool cache_lookup_started = false;
    bool mapped_prefix_started = false;
    bool slab_admission_started = false;
    bool seed_dispatch_started = false;
    bool pipeline_started = false;
    bool pipeline_completed = false;
    bool planning_log_failed = false;
    bool terminal_success = false;

    [[nodiscard]] constexpr std::string_view route_name() const noexcept {
        return canonical_empty
            ? "empty_no_resident_zero_anchor"
            : "full_sorted_dense_direct_mapped";
    }

    [[nodiscard]] static constexpr std::string_view check_status(
        bool checked, bool accepted) noexcept
    {
        if (!checked) return "not_reached";
        return accepted ? "accepted" : "rejected";
    }

    [[nodiscard]] static constexpr std::string_view stage_status(
        bool checked, bool accepted, bool bypass) noexcept
    {
        return bypass ? "bypass" : check_status(checked, accepted);
    }

    [[nodiscard]] static constexpr std::string_view source_authority_name(
        source_direct_host_authorization authority) noexcept
    {
        switch (authority) {
            case source_direct_host_authorization::canonical_empty:
                return "canonical_empty";
            case source_direct_host_authorization::reflected_process_footprint:
                return "reflected_process_footprint";
            case source_direct_host_authorization::retained_exact_permit:
                return "retained_exact_permit";
            case source_direct_host_authorization::none:
                return "none";
        }
        return "unknown";
    }

    [[nodiscard]] static constexpr std::string_view resident_authority_name(
        bioseq::indexing::gpu_resident_accounting_authority authority) noexcept
    {
        switch (authority) {
            case bioseq::indexing::gpu_resident_accounting_authority::
                    reflected_process_footprint:
                return "reflected_process_footprint";
            case bioseq::indexing::gpu_resident_accounting_authority::
                    retained_exact_permit:
                return "retained_exact_permit";
            case bioseq::indexing::gpu_resident_accounting_authority::none:
                return "none";
        }
        return "unknown";
    }

    void log_planning(
        std::string_view decision, std::string_view reason) noexcept
    {
        if (planning_receipts != 0u) {
            planning_log_failed = true;
            return;
        }
        try {
            bioseq_log<logging_level::info>(
                "[gpu_source_direct_operation_planning] operation={} "
                "route={} "
                "source_authority={} source_authority_status={} "
                "gpu_target_index_route_status={} runtime_status={} device_status={} "
                "sorted_shape_status={} abi_identity_status={} "
                "u32_shape_status={} control_range_status={} "
                "position_range_status={} mapping_plan_status={} "
                "N={} D={} control_bytes={} position_bytes={} "
                "logical_bytes={} control_requirement_bytes={} "
                "position_requirement_bytes={} control_allowance_bytes={} "
                "position_allowance_bytes={} allowance_bytes={} "
                "conservative_request_bytes={} reserved_before_bytes={} "
                "allocatable_before_bytes={} margin_bytes={} "
                "shortfall_bytes={} max_storage_buffer_range={} "
                "canonical_empty_bypass={} gate_enabled=0 "
                "decision={} reason={}",
                operation_identity,
                route_name(),
                source_authority_name(source_authority),
                check_status(source_authority_checked,
                             has_valid_source_authorization),
                check_status(gpu_target_index_route_checked, gpu_target_index_route_compatible),
                check_status(runtime_checked, runtime_available),
                check_status(hardware_checked, hardware_available),
                check_status(sorted_shape_checked, is_sorted_shape_accepted),
                check_status(identity_checked, is_identity_accepted),
                check_status(u32_shape_checked, is_u32_shape_accepted),
                check_status(storage_ranges_checked,
                             is_control_range_accepted),
                check_status(storage_ranges_checked,
                             is_position_range_accepted),
                check_status(mapping_plan_checked,
                             is_mapping_plan_accepted),
                minimizer_count,
                distinct_count,
                logical_control_bytes,
                logical_position_bytes,
                logical_resident_bytes,
                control_requirement_bytes,
                position_requirement_bytes,
                control_allowance_bytes,
                position_allowance_bytes,
                conservative_allowance_bytes,
                conservative_request_bytes,
                reserved_before_bytes,
                allocatable_before_bytes,
                margin_bytes,
                shortfall_bytes,
                max_storage_buffer_range,
                canonical_empty ? 1 : 0,
                decision,
                reason);
            planning_receipts = 1u;
        } catch (...) {
            planning_log_failed = true;
        }
    }

    [[nodiscard]] bool terminal_success_invariants() const noexcept {
        const bool explicit_pipeline_bypass =
            expected_seed_consumers == 0u
            && completed_seed_consumers == 0u
            && (canonical_empty
                ? empty_bypass && zero_query_bypasses == 0u
                : zero_query_bypasses == 1u);
        const bool pipeline_exact = explicit_pipeline_bypass
            ? !pipeline_started && !pipeline_completed
            : pipeline_started && pipeline_completed;
        if (planning_receipts != 1u || planning_log_failed
            || !is_planning_accepted || !route_committed
            || !source_release_completed
            || !pipeline_exact
            || completed_seed_consumers != expected_seed_consumers
            || cpu_index_releases != 0u
            || cpu_position_bytes_released != 0u) {
            return false;
        }
        if (canonical_empty) {
            return empty_bypass
                && !materialization_started
                && !materialization_completed
                && !resident_published
                && minimizer_count == 0u
                && distinct_count == 0u
                && logical_control_bytes == 0u
                && logical_position_bytes == 0u
                && logical_resident_bytes == 0u
                && control_requirement_bytes == 0u
                && position_requirement_bytes == 0u
                && control_allowance_bytes == 0u
                && position_allowance_bytes == 0u
                && conservative_allowance_bytes == 0u
                && conservative_request_bytes == 0u
                && source_capacity_bytes == 0u
                && source_reflected_bytes == 0u
                && source_retained_permit_bytes == 0u
                && source_releases == 0u
                && source_released_bytes == 0u
                && !source_released_after_publication
                && cache_preinsertions == 0u
                && cache_required_lookups == 0u
                && cache_required_hits == 0u
                && mapped_prefix_passes == 0u
                && mapped_prefix_lookups == 0u
                && mapped_prefix_comparisons == 0u
                && slab_capacity_before == 0u
                && slab_requested_bytes == 0u
                && slab_actual_bytes == 0u
                && slab_retained_permit_bytes == 0u
                && slab_permit_releases == 0u
                && seed_reserved_bytes == 0u
                && seed_planned_vulkan_bytes == 0u
                && seed_actual_vulkan_bytes == 0u
                && seed_roundtrips == 0u
                && zero_anchor_bypasses == 0u
                && seed_fence_completions == 0u
                && seed_dispatches == 0u
                && !pipeline_started
                && cache_cleanup_releases == 0u
                && resident_cleanup_bytes == 0u;
        }
        if (!materialization_started || !materialization_completed
            || !resident_published || empty_bypass) {
            return false;
        }
        const std::uint64_t allocation_sum =
            bioseq::common::saturating_add(
                materialization.actual_slot_allocation_bytes,
                materialization.actual_position_allocation_bytes);
        const bool exact_materialization = allocation_sum
                == materialization.actual_allocation_bytes
            && allocation_sum
                != (std::numeric_limits<std::uint64_t>::max)()
            && materialization.logical_slot_bytes
                == logical_control_bytes
            && materialization.logical_position_bytes
                == logical_position_bytes
            && materialization.logical_resident_bytes
                == logical_resident_bytes
            && materialization.control_allocation_requirement_bytes
                == control_requirement_bytes
            && materialization.position_allocation_requirement_bytes
                == position_requirement_bytes
            && materialization.control_allocation_allowance_bytes
                == control_allowance_bytes
            && materialization.position_allocation_allowance_bytes
                == position_allowance_bytes
            && materialization.conservative_allowance_bytes
                == conservative_allowance_bytes
            && materialization.conservative_request_bytes
                == conservative_request_bytes
            && materialization.actual_slot_allocation_bytes
                == control_requirement_bytes
            && materialization.actual_position_allocation_bytes
                == position_requirement_bytes
            && materialization.actual_allocation_bytes
                == conservative_request_bytes
            && materialization.fully_touched_bytes
                == materialization.actual_allocation_bytes
            && materialization.permit_reconciliations == 1u;
        const bool reflected = materialization.authority
                == bioseq::indexing::gpu_resident_accounting_authority::
                    reflected_process_footprint
            && materialization.reflected_footprint_bytes
                >= materialization.actual_allocation_bytes
            && materialization.retained_permit_bytes == 0u;
        const bool retained = materialization.authority
                == bioseq::indexing::gpu_resident_accounting_authority::
                    retained_exact_permit
            && materialization.retained_permit_bytes
                == materialization.actual_allocation_bytes
            && materialization.reflected_footprint_bytes == 0u;
        const bool slab_exact = slab_capacity_before == 0u
            && slab_requested_bytes == slab_actual_bytes
            && slab_actual_bytes == slab_retained_permit_bytes
            && (slab_actual_bytes == 0u
                ? slab_permit_releases == 0u
                : slab_permit_releases == 1u);
        const bool zero_query = zero_query_bypasses == 1u;
        const bool seed_memory_exact = zero_query
            ? seed_reserved_bytes == 0u
                && seed_planned_vulkan_bytes == 0u
                && seed_actual_vulkan_bytes == 0u
            : slab_actual_bytes == 0u
            ? seed_reserved_bytes == 0u
                && seed_planned_vulkan_bytes == 0u
                && seed_actual_vulkan_bytes == 0u
            : seed_reserved_bytes != 0u
                && seed_planned_vulkan_bytes != 0u
                && seed_planned_vulkan_bytes
                    == seed_actual_vulkan_bytes;
        const bool zero_anchor = !zero_query
            && seed_roundtrips != 0u
            && zero_anchor_bypasses == seed_roundtrips;
        const bool lookup_exact = zero_query
            ? cache_required_hits == cache_required_lookups
                && cache_required_lookups == completed_seed_consumers
                && mapped_prefix_passes == 0u
            : cache_required_lookups >= 1u
                && cache_required_hits == cache_required_lookups
                && cache_required_lookups == completed_seed_consumers
                && mapped_prefix_passes
                    == 2u * cache_required_lookups;
        const bool seed_completion_exact = seed_roundtrips
                == cache_required_lookups
            && (zero_query
                ? slab_actual_bytes == 0u
                    && slab_requested_bytes == 0u
                    && zero_anchor_bypasses == 0u
                    && seed_dispatches == 0u
                    && seed_fence_completions == 0u
                : slab_actual_bytes == 0u
                ? zero_anchor_bypasses == seed_roundtrips
                    && seed_dispatches == 0u
                    && seed_fence_completions == 0u
                : zero_anchor_bypasses == 0u
                    && seed_dispatches == seed_roundtrips
                    && seed_fence_completions == seed_roundtrips);
        return exact_materialization
            && source_releases == 1u
            && source_released_bytes == source_capacity_bytes
            && source_released_after_publication
            && cache_preinsertions == 1u
            && lookup_exact
            && (reflected != retained)
            && slab_exact
            && seed_memory_exact
            && seed_completion_exact
            && cache_cleanup_releases == 1u
            && resident_cleanup_bytes
                == materialization.actual_allocation_bytes
            && materialization.source_checksum != 0u
            && materialization.source_checksum
                == materialization.mapped_checksum
            && materialization.complete_source_traversals == 1u
            && materialization.complete_mapped_traversals == 1u;
    }

    void log_terminal(
        std::string_view status,
        std::string_view reason) noexcept {
        if (terminal_receipts != 0u) return;
        const bool invariant_success = terminal_success_invariants();
        const bool accepted = status == "success" && invariant_success;
        const bool explicit_pipeline_bypass =
            expected_seed_consumers == 0u
            && completed_seed_consumers == 0u
            && (canonical_empty
                ? empty_bypass && zero_query_bypasses == 0u
                : zero_query_bypasses == 1u);
        const bool zero_anchor_bypass = seed_roundtrips != 0u
            && zero_anchor_bypasses == seed_roundtrips;
        const bool mapped_prefix_bypass = explicit_pipeline_bypass;
        const bool cleanup_checked = source_release_completed
            || cache_cleanup_releases != 0u
            || resident_cleanup_bytes != 0u;
        const bool is_cleanup_accepted = source_release_completed
            && (!resident_published
                || (cache_cleanup_releases == 1u
                    && resident_cleanup_bytes
                        == materialization.actual_allocation_bytes));
        try {
            bioseq_log<logging_level::info>(
            "[gpu_source_direct_operation_terminal] operation={} status={} "
            "reason={} route={} N={} D={} actual_control_allocation_bytes={} "
            "actual_position_allocation_bytes={} actual_allocation_bytes={} "
            "permit_reconciliations={} resident_authority={} "
            "reflected_bytes={} retained_permit_bytes={} fully_touched_bytes={} "
            "source_checksum={} mapped_checksum={} source_traversals={} "
            "mapped_traversals={} source_releases={} source_released_bytes={} "
            "source_released_after_publication={} cache_preinsertions={} "
            "required_lookups={} required_hits={} required_misses={} "
            "mapped_prefix_passes={} "
            "mapped_prefix_lookups={} mapped_prefix_comparisons={} "
            "slab_before={} slab_requested_bytes={} slab_actual_bytes={} "
            "slab_retained_permit_bytes={} slab_permit_releases={} "
            "seed_reserved_bytes={} seed_planned_vulkan_bytes={} "
            "seed_actual_vulkan_bytes={} "
            "expected_consumers={} completed_consumers={} seed_roundtrips={} "
            "seed_dispatches={} seed_fence_completions={} "
            "zero_query_bypasses={} zero_anchor_bypasses={} empty_bypass={} "
            "materialization_status={} publication_status={} "
            "cache_lookup_status={} mapped_prefix_status={} "
            "slab_admission_status={} seed_dispatch_status={} "
            "seed_fence_status={} cleanup_status={} pipeline_status={} "
            "pipeline_completed={} terminal_invariants={} "
            "cache_cleanup_releases={} resident_cleanup_bytes={}",
            operation_identity,
            accepted ? "success" : "failure",
            reason,
            route_name(),
            minimizer_count,
            distinct_count,
            materialization.actual_slot_allocation_bytes,
            materialization.actual_position_allocation_bytes,
            materialization.actual_allocation_bytes,
            materialization.permit_reconciliations,
            resident_authority_name(materialization.authority),
            materialization.reflected_footprint_bytes,
            materialization.retained_permit_bytes,
            materialization.fully_touched_bytes,
            materialization.source_checksum,
            materialization.mapped_checksum,
            materialization.complete_source_traversals,
            materialization.complete_mapped_traversals,
            source_releases,
            source_released_bytes,
            source_released_after_publication ? 1 : 0,
            cache_preinsertions,
            cache_required_lookups,
            cache_required_hits,
            cache_required_lookups >= cache_required_hits
                ? cache_required_lookups - cache_required_hits
                : 0u,
            mapped_prefix_passes,
            mapped_prefix_lookups,
            mapped_prefix_comparisons,
            slab_capacity_before,
            slab_requested_bytes,
            slab_actual_bytes,
            slab_retained_permit_bytes,
            slab_permit_releases,
            seed_reserved_bytes,
            seed_planned_vulkan_bytes,
            seed_actual_vulkan_bytes,
            expected_seed_consumers,
            completed_seed_consumers,
            seed_roundtrips,
            seed_dispatches,
            seed_fence_completions,
            zero_query_bypasses,
            zero_anchor_bypasses,
            empty_bypass ? 1 : 0,
            stage_status(
                materialization_started,
                materialization_completed,
                canonical_empty),
            stage_status(
                materialization_completed,
                resident_published,
                canonical_empty),
            stage_status(
                cache_lookup_started,
                cache_required_lookups != 0u
                    && cache_required_hits == cache_required_lookups,
                explicit_pipeline_bypass),
            stage_status(
                mapped_prefix_started,
                mapped_prefix_passes == 2u * cache_required_lookups,
                mapped_prefix_bypass),
            stage_status(
                slab_admission_started,
                slab_requested_bytes == slab_actual_bytes
                    && slab_actual_bytes == slab_retained_permit_bytes,
                explicit_pipeline_bypass || zero_anchor_bypass),
            stage_status(
                seed_dispatch_started,
                seed_dispatches != 0u
                    && seed_dispatches == seed_roundtrips,
                explicit_pipeline_bypass || zero_anchor_bypass),
            stage_status(
                seed_dispatch_started,
                seed_fence_completions == seed_roundtrips,
                explicit_pipeline_bypass || zero_anchor_bypass),
            check_status(cleanup_checked, is_cleanup_accepted),
            stage_status(
                pipeline_started,
                pipeline_completed,
                explicit_pipeline_bypass),
            pipeline_completed ? 1 : 0,
            invariant_success ? 1 : 0,
            cache_cleanup_releases,
            resident_cleanup_bytes);
            terminal_receipts = 1u;
            terminal_success = accepted;
        } catch (...) {
            terminal_success = false;
        }
    }
};

/// Last-resort receipt guard for failures before the wrapper's catch handler
/// can finish its unwind. Cleanup remains owned by the wrapper/PostSeed; this
/// guard only prevents a selected operation from disappearing without its two
/// operation-level receipts.
struct direct_resident_receipt_guard {
    direct_resident_operation_stats* ledger = nullptr;

    ~direct_resident_receipt_guard() noexcept {
        if (ledger == nullptr) return;
        if (ledger->planning_receipts == 0u) {
            ledger->log_planning("reject", "unwind_before_planning_complete");
        }
        if (ledger->terminal_receipts == 0u) {
            ledger->log_terminal("failure", "unhandled_operation_unwind");
        }
    }
};

/// Release authority for one operation-owned transient CPU target index.
/// Ordinary transient indexes carry only `index`; the fixed Vulkan handoff
/// also carries the exact permit that accounts for storage not already
/// represented by its one-shot process-footprint authority.
struct transient_target_index_release_authorization {
    bioseq::indexing::minimizer_hashmap_fragment_index* index = nullptr;
    bioseq::parallelism::memory_permit* permit = nullptr;
    std::uint64_t expected_permit_bytes = 0u;
    bool require_exact_permit_bytes = false;
    bioseq::indexing::gpu_minimizer_index_cache*
        operation_owned_gpu_cache = nullptr;
    direct_resident_operation_stats* direct_ledger = nullptr;

    [[nodiscard]] bool is_valid() const noexcept {
        return index != nullptr
            && (!require_exact_permit_bytes || permit != nullptr)
            && (permit == nullptr
                || static_cast<bool>(*permit)
                    && (!require_exact_permit_bytes
                        || permit->bytes() == expected_permit_bytes));
    }
};

/// One operation-owned lifetime boundary between seed lookup and chaining.
/// Borrowed persisted CPU indexes may use it only to retire an invocation-owned
/// GPU cache; external caches never receive release authority. Every successful
/// seed consumer signals exactly once after dropping its local resident handle;
/// the last consumer deallocates any transient CPU lookup index and clears any
/// internally-owned GPU cache before downstream chaining begins.
struct post_seed_owned_lifetime {
    struct release_receipt {
        std::uint64_t gpu_cache_evictions = 0;
        std::uint64_t gpu_resident_bytes_released = 0;
        std::uint64_t cpu_index_releases = 0;
        std::uint64_t cpu_position_bytes_released = 0;
        std::uint64_t cpu_index_permit_releases = 0;
        std::uint64_t cpu_index_permit_bytes_released = 0;
        bool cpu_index_storage_released_before_permit = false;
    };

    std::atomic<std::size_t> remaining_seed_users{0};
    bioseq::indexing::gpu_minimizer_index_cache* owned_gpu_cache = nullptr;
    transient_target_index_release_authorization owned_cpu_index{};
    bioseq::parallelism::memory_permit retained_seed_arena_permit{};

    post_seed_owned_lifetime(
            std::size_t seed_users,
            bioseq::indexing::gpu_minimizer_index_cache* gpu_cache,
            transient_target_index_release_authorization cpu_index) noexcept
        : remaining_seed_users(seed_users),
          owned_gpu_cache(gpu_cache),
          owned_cpu_index(cpu_index) {}

    ~post_seed_owned_lifetime() noexcept {
        if (owned_cpu_index.direct_ledger == nullptr) return;
        auto& ledger = *owned_cpu_index.direct_ledger;
        if (owned_gpu_cache != nullptr) {
            const std::uint64_t resident_bytes =
                owned_gpu_cache->resident_bytes();
            owned_gpu_cache->clear();
            owned_gpu_cache = nullptr;
            if (resident_bytes != 0u) {
                ledger.cache_cleanup_releases = 1u;
                ledger.resident_cleanup_bytes = resident_bytes;
            }
        }
        if (retained_seed_arena_permit) {
            ledger.slab_retained_permit_bytes =
                retained_seed_arena_permit.bytes();
            retained_seed_arena_permit.release();
            ledger.slab_permit_releases = 1u;
        }
        const bool users_completed =
            remaining_seed_users.load(std::memory_order_relaxed) == 0u;
        const bool accepted = users_completed
            && ledger.terminal_success_invariants();
        ledger.log_terminal(
            accepted ? "success" : "failure",
            accepted
                ? ledger.expected_seed_consumers == 0u
                    ? "completed_explicit_zero_consumer_bypass"
                    : "completed_after_last_seed_fence_and_arena_unwind"
                : users_completed
                    ? "route_specific_terminal_invariant_failed"
                    : "seed_user_not_completed");
    }

    post_seed_owned_lifetime(const post_seed_owned_lifetime&) = delete;
    post_seed_owned_lifetime& operator=(const post_seed_owned_lifetime&) = delete;

    [[nodiscard]] release_receipt complete_seed_user() {
        std::size_t previous =
            remaining_seed_users.load(std::memory_order_acquire);
        while (previous != 0u
               && !remaining_seed_users.compare_exchange_weak(
                   previous,
                   previous - 1u,
                   std::memory_order_acq_rel,
                   std::memory_order_acquire)) {
        }
        if (previous == 0u) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "post-seed lifetime user underflow"};
        }
        if (owned_cpu_index.direct_ledger != nullptr) {
            auto& ledger = *owned_cpu_index.direct_ledger;
            ledger.completed_seed_consumers =
                bioseq::common::saturating_add(
                    ledger.completed_seed_consumers, 1u);
        }
        if (previous != 1u) return {};

        release_receipt stats{};
        if (owned_gpu_cache != nullptr) {
            auto* const cache_to_release = owned_gpu_cache;
            owned_gpu_cache = nullptr;
            stats.gpu_resident_bytes_released =
                cache_to_release->resident_bytes();
            cache_to_release->clear();
            stats.gpu_cache_evictions =
                stats.gpu_resident_bytes_released != 0u ? 1u : 0u;
            if (owned_cpu_index.direct_ledger != nullptr) {
                auto& ledger = *owned_cpu_index.direct_ledger;
                ledger.cache_cleanup_releases =
                    stats.gpu_cache_evictions;
                ledger.resident_cleanup_bytes =
                    stats.gpu_resident_bytes_released;
            }
        }
        if (owned_cpu_index.index != nullptr) {
            if (!owned_cpu_index.is_valid()) {
                bioseq_log<logging_level::error>(
                    "post-seed lifetime has invalid CPU index release authority");
                std::abort();
            }
            const bool had_resident_storage =
                !owned_cpu_index.index->table_empty()
                || owned_cpu_index.index->positions.capacity() != 0u;
            stats.cpu_position_bytes_released =
                bioseq::common::saturating_mul(
                    owned_cpu_index.index->positions.capacity(),
                    sizeof(std::uint64_t));
            owned_cpu_index.index->release_storage();
            stats.cpu_index_releases = had_resident_storage ? 1u : 0u;
            if (owned_cpu_index.permit != nullptr) {
                if (!owned_cpu_index.index->table_empty()
                    || owned_cpu_index.index->positions.capacity() != 0u) {
                    bioseq_log<logging_level::error>(
                        "post-seed CPU index permit release preceded storage");
                    std::abort();
                }
                stats.cpu_index_storage_released_before_permit = true;
                stats.cpu_index_permit_bytes_released =
                    owned_cpu_index.permit->bytes();
                stats.cpu_index_permit_releases =
                    static_cast<bool>(*owned_cpu_index.permit) ? 1u : 0u;
                owned_cpu_index.permit->release();
            }
        }
        return stats;
    }
};

/**
 * @brief Seed-hit collection stage.
 *
 * Wraps the `collect_seed_hits` kernel, which writes its anchor output
 * directly into a caller-provided arena_slab. No `std::vector` is materialised
 * inside the stage, so the kernel's `push_back` traffic lands directly in the
 * pipeline arena.
 */
struct seed_hit_stage : stage_base<seed_hit_stage, ::bioseq::chaining::anchor> {
    static constexpr std::string_view stage_name = "seed_hit_stage";

    ::bioseq::simd::minimap2_parameters params;
    sequence_size_t                               query_len;
    // — batch/segment-wave-scoped GPU index cache. The outer
    // wave creates one cache, passes `&cache` here, and clears it at wave end.
    // Null on single-shot paths (then the GPU seed path uploads uncached).
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr;
    match_result_collection* stats_dst = nullptr;
    post_seed_owned_lifetime* post_seed_owned_lifetime = nullptr;
    match_result_collection* host_timing_dst = nullptr;

    // — host/device accessors for both the std::span CPU shape and
    // the minimizer_batch_view device shape.
    template <class p_minimizer>
    [[nodiscard, gnu::always_inline]] static std::span<const p_minimizer>
    sketch_span_of(const std::span<const p_minimizer>& s) noexcept { return s; }
    template <class p_minimizer>
    [[nodiscard, gnu::always_inline]] static std::span<const p_minimizer>
    sketch_span_of(const minimizer_batch_view<p_minimizer>& b) noexcept { return b.host; }

    template <class p_minimizer>
    [[nodiscard, gnu::always_inline]] static
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
    device_span_of(const std::span<const p_minimizer>&) noexcept { return std::nullopt; }
    template <class p_minimizer>
    [[nodiscard, gnu::always_inline]] static
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
    device_span_of(const minimizer_batch_view<p_minimizer>& b) noexcept { return b.device; }

    template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_index, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_with_index(
        p_batch&& sketches, const p_index& target_index_ref,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
        emit_token<p_sink>&& token)
    {
        const std::uint64_t host_stage_started = host_timing_dst == nullptr
            ? 0u : host_stage_now_ns();
        const int32_t kmer_size = p_aligner_parameters::generic_fallback
            ? params.kmer_size
            : static_cast<int32_t>(p_aligner_parameters::kmer_len);

        using p_minimizer = std::remove_const_t<
            typename std::remove_cvref_t<p_batch>::value_type>;

        auto sketch_span = sketch_span_of<p_minimizer>(sketches);
        auto device_span = device_span_of<p_minimizer>(sketches);
        direct_resident_operation_stats* const direct_ledger =
            post_seed_owned_lifetime == nullptr
            ? nullptr
            : post_seed_owned_lifetime->owned_cpu_index.direct_ledger;

        // — run the selected GPU seed-hit lookup before the CPU
        // path. If it declines, the selected Vulkan pipeline fails loudly.
        // Only the concrete minimizer_hashmap_fragment_index carries the GPU view.
        if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
            bool seed_gpu_declined = false;
            bioseq::seeding::vulkan_seed_lookup_decline seed_decline{
                .query_length = static_cast<std::uint64_t>(query_len),
                .query_minimizer_count = static_cast<std::uint64_t>(
                    sketch_span.size()),
                .kmer_size = kmer_size,
                .max_occ = params.max_occ,
                .fused_frontend = params.vulkan_fused_frontend_enabled,
            };
            if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                          bioseq::indexing::minimizer_hashmap_fragment_index>) {
                // Strict Vulkan selection owns its anchor allocation. A
                // prepared fragment view has no resident GPU index and reaches
                // the fail-closed decline below without allocating CPU output.
                auto anchors_slab = make_arena_slab<anchor>();
                if (gpu_index_identity.source_direct_sorted_dense
                    && gpu_index_identity.canonical_empty) {
                    if (!bioseq::indexing::
                            is_gpu_source_direct_identity_well_formed(
                                gpu_index_identity)
                        || !target_index_ref.table_empty()
                        || !target_index_ref.positions.empty()
                        || gpu_index_identity.table_size != 0u
                        || gpu_index_identity.position_count != 0u
                        || gpu_index_cache != nullptr
                        || direct_ledger == nullptr
                        || params.vulkan_fused_frontend_enabled) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                            "canonical empty source-direct route is invalid"};
                    }
                    direct_ledger->empty_bypass = true;
                    if (post_seed_owned_lifetime != nullptr
                        && post_seed_owned_lifetime->remaining_seed_users.load(
                               std::memory_order_acquire) != 0u) {
                        static_cast<void>(
                            post_seed_owned_lifetime->complete_seed_user());
                    }
                    return std::move(token).emit_batch(
                        std::span<anchor>{});
                }
                auto& runtime = bioseq::gpu::vulkan::runtime::instance();
                if (bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                    auto key = bioseq::indexing::make_gpu_index_cache_key(
                        target_index_ref, gpu_index_identity);
                    std::shared_ptr<const bioseq::indexing::
                        gpu_resident_minimizer_index> resident_index;
                    // Keep operation-originated materialization statistics
                    // separate from the seed-decline record. The seed lookup
                    // entry point intentionally resets its decline output at
                    // entry, while these statistics must survive a successful
                    // lookup so the benchmark records the one real upload and
                    // leaves later cache hits at zero.
                    bioseq::indexing::gpu_index_upload_decline
                        index_materialization{};
                    if (gpu_index_cache == nullptr) {
                        seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::index_cache_unavailable;
                    } else if (gpu_index_identity.
                            source_direct_sorted_dense) {
                        if (!bioseq::indexing::
                                is_gpu_source_direct_identity_well_formed(
                                    gpu_index_identity)
                            || gpu_index_identity.canonical_empty
                            || params.vulkan_fused_frontend_enabled
                            || direct_ledger == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "source-direct resident seed identity is invalid"};
                        }
                        direct_ledger->cache_lookup_started = true;
                        direct_ledger->cache_required_lookups =
                            bioseq::common::saturating_add(
                                direct_ledger->cache_required_lookups, 1u);
                        auto required =
                            gpu_index_cache->find_required_preinserted(
                                runtime, key);
                        if (required) {
                            resident_index = std::move(*required);
                            direct_ledger->cache_required_hits =
                                bioseq::common::saturating_add(
                                    direct_ledger->cache_required_hits, 1u);
                        } else {
                            seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::
                                    resident_index_unavailable;
                        }
                    } else {
                        resident_index = gpu_index_cache->get_or_upload(
                            runtime, target_index_ref, key,
                            params.vulkan_index_cache_enabled,
                            &index_materialization);
                        if (!resident_index) {
                            seed_decline.index_upload = index_materialization;
                            seed_decline.reason = index_materialization.is_declined()
                                ? bioseq::seeding::vulkan_seed_lookup_decline_reason::
                                        resident_index_upload_declined
                                : bioseq::seeding::vulkan_seed_lookup_decline_reason::
                                        resident_index_unavailable;
                        }
                    }
                    if (resident_index
                        && gpu_index_identity.source_direct_sorted_dense
                        && post_seed_owned_lifetime == nullptr) {
                        throw bioseq::gpu::vulkan::vulkan_runtime_error{
                            "source-direct seed route has no arena permit owner"};
                    }
                    if (direct_ledger != nullptr) {
                        if (post_seed_owned_lifetime->retained_seed_arena_permit) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "source-direct seed route retained a prior arena permit"};
                        }
                        if (stats_dst == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "source-direct seed stage has no execution-stats sink"};
                        }
                        direct_ledger->mapped_prefix_started = true;
                        direct_ledger->slab_capacity_before =
                            bioseq::common::saturating_add(
                                direct_ledger->slab_capacity_before,
                                static_cast<std::uint64_t>(
                                    anchors_slab.capacity()));
                    }
                    bioseq::seeding::vulkan_seed_lookup_completion
                        seed_completion{false};
                    if (resident_index) {
                        seed_completion = bioseq::seeding::collect_seed_hits_vulkan<p_minimizer>(
                                sketch_span,
                                device_span,
                                target_index_ref,
                                resident_index,
                                params.vulkan_enabled,
                                query_len,
                                params.max_occ,
                                kmer_size,
                                anchors_slab,
                                params.vulkan_fused_frontend_enabled,
                                &seed_decline);
                    }
                    if (direct_ledger != nullptr) {
                        direct_ledger->mapped_prefix_passes =
                            bioseq::common::saturating_add(
                                direct_ledger->mapped_prefix_passes,
                                seed_decline.mapped_prefix_authority_passes);
                        direct_ledger->mapped_prefix_lookups =
                            bioseq::common::saturating_add(
                                direct_ledger->mapped_prefix_lookups,
                                seed_decline.mapped_prefix_lookup_count);
                        direct_ledger->mapped_prefix_comparisons =
                            bioseq::common::saturating_add(
                                direct_ledger->mapped_prefix_comparisons,
                                seed_decline.mapped_prefix_comparison_count);
                        direct_ledger->slab_requested_bytes =
                            bioseq::common::saturating_add(
                                direct_ledger->slab_requested_bytes,
                                seed_decline.anchors_out_growth_bytes);
                        direct_ledger->slab_actual_bytes =
                            bioseq::common::saturating_add(
                                direct_ledger->slab_actual_bytes,
                                seed_decline.anchors_out_actual_growth_bytes);
                        direct_ledger->slab_retained_permit_bytes =
                            bioseq::common::saturating_add(
                                direct_ledger->slab_retained_permit_bytes,
                                seed_decline.
                                    anchors_out_retained_permit_bytes);
                        direct_ledger->seed_reserved_bytes =
                            bioseq::common::saturating_add(
                                direct_ledger->seed_reserved_bytes,
                                seed_decline.reserved_bytes);
                        direct_ledger->seed_planned_vulkan_bytes =
                            bioseq::common::saturating_add(
                                direct_ledger->seed_planned_vulkan_bytes,
                                seed_decline.
                                    planned_vulkan_allocation_bytes);
                        direct_ledger->seed_actual_vulkan_bytes =
                            bioseq::common::saturating_add(
                                direct_ledger->seed_actual_vulkan_bytes,
                                seed_decline.actual_vulkan_allocation_bytes);
                        direct_ledger->slab_admission_started =
                            seed_decline.exact_anchor_count != 0u;
                        direct_ledger->seed_dispatches =
                            bioseq::common::saturating_add(
                                direct_ledger->seed_dispatches,
                                seed_decline.seed_lookup_dispatches);
                        direct_ledger->seed_dispatch_started =
                            direct_ledger->seed_dispatch_started
                            || seed_decline.seed_lookup_dispatches != 0u;
                        if (seed_completion) {
                            direct_ledger->seed_roundtrips =
                                bioseq::common::saturating_add(
                                    direct_ledger->seed_roundtrips, 1u);
                            if (seed_decline.exact_anchor_count == 0u) {
                                direct_ledger->zero_anchor_bypasses =
                                    bioseq::common::saturating_add(
                                        direct_ledger->zero_anchor_bypasses,
                                        1u);
                            }
                            if (seed_decline.seed_lookup_dispatches != 0u) {
                                direct_ledger->seed_fence_completions =
                                    bioseq::common::saturating_add(
                                        direct_ledger->seed_fence_completions,
                                        1u);
                            }
                        }
                    }
                    if (seed_completion.retained_anchor_permit) {
                        if (post_seed_owned_lifetime == nullptr
                            || post_seed_owned_lifetime->
                                retained_seed_arena_permit) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "source-direct seed permit owner is invalid"};
                        }
                        post_seed_owned_lifetime->
                            retained_seed_arena_permit = std::move(
                                seed_completion.retained_anchor_permit);
                        if (seed_completion.retained_anchor_bytes
                            != post_seed_owned_lifetime->
                                retained_seed_arena_permit.bytes()) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "source-direct seed permit byte authority is invalid"};
                        }
                    }
                    if (resident_index && seed_completion) {
                        if (stats_dst == nullptr) {
                            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                                "Vulkan seed stage has no execution-stats sink"};
                        }
                        partial_vulkan_execution_stats stats{};
                        stats.seed_lookup_dispatches =
                            seed_decline.seed_lookup_dispatches;
                        stats.seed_open_addressed_lookup_dispatches =
                            seed_decline.open_addressed_lookup_dispatches;
                        stats.seed_sorted_dense_lookup_dispatches =
                            seed_decline.sorted_dense_lookup_dispatches;
                        stats.seed_bitonic_sort_dispatches =
                            seed_decline.sort_stats.bitonic_sort_dispatches;
                        stats.seed_radix_sort_operations =
                            seed_decline.sort_stats.radix_sort_operations;
                        stats.seed_radix_anchor_count =
                            seed_decline.sort_stats.radix_anchor_count;
                        stats.seed_radix_passes =
                            seed_decline.sort_stats.radix_passes;
                        stats.seed_radix_histogram_dispatches =
                            seed_decline.sort_stats.radix_histogram_dispatches;
                        stats.seed_radix_scan_dispatches =
                            seed_decline.sort_stats.radix_scan_dispatches;
                        stats.seed_radix_scatter_dispatches =
                            seed_decline.sort_stats.radix_scatter_dispatches;
                        stats.seed_host_sort_fallbacks =
                            seed_decline.sort_stats.host_sort_fallbacks;
                        if (seed_decline.canonical_anchor_order_checked) {
                            stats.seed_anchor_stats_operations = 1u;
                            stats.seed_canonical_anchor_count =
                                seed_decline.canonical_anchor_count;
                            stats.seed_canonical_anchor_order_checks = 1u;
                            stats.seed_canonical_anchor_order_accepts =
                                seed_decline.canonical_anchor_order_accepted
                                    ? 1u : 0u;
                            stats.seed_canonical_tie_normalization_runs =
                                seed_decline.
                                    canonical_tie_normalization_runs;
                            stats.seed_canonical_tie_normalization_anchors =
                                seed_decline.
                                    canonical_tie_normalization_anchors;
                            stats.seed_anchor_digest_lane0 =
                                seed_decline.canonical_anchor_digest_lo;
                            stats.seed_anchor_digest_lane1 =
                                seed_decline.canonical_anchor_digest_hi;
                        }
                        // `index_upload` is populated only by the operation
                        // that actually materialized a new resident index.
                        // cache_type hits leave it zeroed, so these counters never
                        // multiply one upload by the number of queries reusing
                        // it.
                        const auto& index_upload = index_materialization;
                        if (index_upload.slot_capacity != 0u) {
                            stats.gpu_index_materializations = 1u;
                            if (index_upload.representation
                                == bioseq::indexing::gpu_index_representation::
                                    open_addressed_70) {
                                stats.gpu_index_open_addressed_materializations = 1u;
                            } else {
                                stats.gpu_index_sorted_dense_materializations = 1u;
                            }
                            stats.gpu_index_unique_keys =
                                index_upload.unique_key_count;
                            stats.gpu_index_slot_capacity =
                                index_upload.slot_capacity;
                            stats.gpu_index_logical_resident_bytes =
                                index_upload.resident_bytes;
                            stats.gpu_index_actual_allocation_bytes =
                                index_upload.actual_allocation_bytes;
                            stats.gpu_index_fully_touched_bytes =
                                index_upload.fully_touched_bytes;
                            stats.gpu_index_staging_bytes =
                                index_upload.staging_bytes;
                            stats.gpu_index_permit_releases =
                                index_upload.is_materialization_permit_released ? 1u : 0u;
                            stats.gpu_index_process_footprint_reflections =
                                index_upload.is_process_footprint_reflected ? 1u : 0u;
                            stats.gpu_index_reflected_footprint_bytes =
                                index_upload.reflected_footprint_delta_bytes;
                            stats.gpu_index_max_load_ppm = index_upload.load_ppm;
                            stats.gpu_index_max_probe = index_upload.max_probe;
                        }
                        // `emit_batch` is synchronous: without dropping this
                        // local handle first, the multi-gigabyte resident index
                        // survives through chain_stage even if the cache is
                        // cleared.  Seed lookup has completed its Vulkan fence
                        // and copied anchors into `anchors_slab`, so neither
                        // index nor query-device storage is used downstream.
                        resident_index.reset();
                        device_span.reset();
                        if (post_seed_owned_lifetime != nullptr) {
                            const auto release =
                                post_seed_owned_lifetime->complete_seed_user();
                            stats.post_seed_owned_gpu_cache_evictions =
                                release.gpu_cache_evictions;
                            stats.post_seed_owned_gpu_resident_bytes_released =
                                release.gpu_resident_bytes_released;
                            stats.post_seed_owned_cpu_index_releases =
                                release.cpu_index_releases;
                            stats.post_seed_owned_cpu_position_bytes_released =
                                release.cpu_position_bytes_released;
                        }
                        stats_dst->record_partial_vulkan_execution_stats(stats);
                        return std::move(token).emit_batch(
                            std::span<anchor>{anchors_slab.data(), anchors_slab.size()});
                    }
                    seed_gpu_declined = true;
                } else {
                    seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::hardware_device_unavailable;
                    seed_gpu_declined = true;
                }
            } else {
                seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::resident_index_unavailable;
                seed_gpu_declined = true;
            }
            if (seed_gpu_declined) {
                if (!seed_decline.is_declined()) {
                    seed_decline.reason = bioseq::seeding::vulkan_seed_lookup_decline_reason::resident_index_unavailable;
                }
                bioseq::seeding::throw_vulkan_seed_lookup_strict_decline(
                    "Vulkan seed lookup", seed_decline);
            }
        }

        if (direct_ledger != nullptr) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct resident route cannot fall back to CPU seeding"};
        }

        match_result_collection* const cpu_seed_stats_dst = stats_dst;
        post_seed_owned_lifetime* const cpu_seed_owned_lifetime =
            post_seed_owned_lifetime;
        match_result_collection* const cpu_seed_host_timing_dst =
            host_timing_dst;
        const auto record_cpu_seed_completion =
            [cpu_seed_stats_dst, sketch_span, cpu_seed_owned_lifetime,
             cpu_seed_host_timing_dst, host_stage_started] {
            // A successful strict Vulkan lookup returned above, so this stats
            // can only describe a real CPU seed operation.
            if (cpu_seed_stats_dst == nullptr) {
                bioseq_log<logging_level::error>("CPU seed stage has no execution-stats sink");
                std::abort();
            }
            scalar_pipeline_execution_stats stats{};
            stats.record_seed_lookup(
                static_cast<std::uint64_t>(sketch_span.size()));
            cpu_seed_stats_dst->record_scalar_pipeline_execution_stats(stats);

            if (cpu_seed_owned_lifetime != nullptr) {
                // CPU-index retirement is a lifetime observation, not Vulkan
                // execution statistics. Prepared fragment batches are normally
                // externally owned and leave this pointer null.
                (void)cpu_seed_owned_lifetime->complete_seed_user();
            }
            if (cpu_seed_host_timing_dst != nullptr) {
                cpu_seed_host_timing_dst->record_seed_lookup_host_time(
                    elapsed_host_stage_ns(host_stage_started));
            }
        };

        if constexpr (bioseq::indexing::prepared_fragment_anchor_query_index<
                          std::remove_cvref_t<p_index>>) {
            static_assert(std::same_as<p_minimizer, minimap_sketch_data>,
                "prepared fragment anchors require canonical query minimizers");
            const std::span<const anchor> ordered_anchors =
                target_index_ref.ordered_fragment_seed_anchors(
                    std::span<const minimap_sketch_data>{
                        sketch_span.data(), sketch_span.size()},
                    query_len,
                    params.max_occ,
                    kmer_size);
            record_cpu_seed_completion();
            // The owning PreparedFragmentAnchorBatch outlives this synchronous
            // pipeline call. Emit its already-canonical span directly: no
            // arena_slab allocation, anchor copy, or global fragment sort.
            return std::move(token).emit_batch(ordered_anchors);
        } else {
            // Ordinary per-hash lookup retains the arena-producing kernel.
            auto anchors_slab = make_arena_slab<anchor>();
            anchors_slab.reserve(detail::estimated_anchor_count(
                sketch_span.size(),
                static_cast<std::size_t>(query_len),
                static_cast<std::size_t>(target_index_ref.target_length()),
                kmer_size));
            collect_seed_hits<p_minimizer>(
                sketch_span,
                target_index_ref,
                query_len, params.max_occ, kmer_size,
                anchors_slab);
            record_cpu_seed_completion();
            return std::move(token).emit_batch(
                std::span<anchor>{anchors_slab.data(), anchors_slab.size()});
        }
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        stage_algorithm_description algo;
        algo.name = "Seed hits + canonical CPU ordering";
        algo.parameters.push_back({"max_occ", std::to_string(params.max_occ)});
        algo.parameters.push_back({"query_len", std::to_string(query_len)});
        desc.algorithm = std::move(algo);
        // anchor reserve target — used by seed_hit_stage's slab (
        // "estimated_anchor_count").  The actual sketch count is unknown at
        // describe time so the reported figure uses query_len/window=10 as
        // the canonical median; the runtime reserve picks a tighter value
        // when the real sketches.size() is available.
        const std::size_t approx_sketch_count =
            static_cast<std::size_t>(query_len) / 10u + 64u;
        desc.estimated_batch_size = detail::estimated_anchor_count(
            approx_sketch_count,
            static_cast<std::size_t>(query_len),
            /*target_bp=*/static_cast<std::size_t>(query_len),
            params.kmer_size);
        return desc;
    }
};

/**
 * @brief Anchor chaining stage.
 *
 * Templated on `p_kernel` — the algorithm-specific call surface exposed by
 * `algorithm_traits<chaining_algorithm::X>::kernel_type`.
 * The stage body has zero `if constexpr` over enum
 * values — adding a new chaining algorithm is a single trait specialisation
 * in the kernel's own module.
 *
 * The chosen kernel writes its `(chains, reordered_anchors)` output directly
 * into caller-provided arena_slab parameters; no intermediate vector is
 * materialised inside the stage.
 */
template <class p_kernel>
struct chain_stage : stage_base<chain_stage<p_kernel>, chain> {
    static constexpr std::string_view stage_name = "chain_stage";

    chaining_parameters cp;
    sequence_size_t query_len = 0;
    match_result_collection* stats_dst = nullptr;
    match_result_collection* host_timing_dst = nullptr;
    const ::bioseq::simd_dispatch::highway_kernel_table* highway_kernels = nullptr;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process(p_batch&& anchors_in, emit_token<p_sink>&& token) {
        const std::uint64_t host_stage_started = host_timing_dst == nullptr
            ? 0u : host_stage_now_ns();
        chaining_parameters active_cp = cp;
        active_cp.query_len = query_len;
        const std::span<const anchor> anchors_span{
            anchors_in.data(), anchors_in.size()};

        auto chains_slab    = make_arena_slab<chain>();
        auto reordered_slab = make_arena_slab<anchor>();
        if (active_cp.coordinate_only) {
            chains_slab.reserve(1);
            // Reserve the full upstream bound before the Vulkan backend opens
            // its nested DP arena scope. A >65,536-anchor coordinate trace
            // must never grow this output slab inside that inner scope, whose
            // allocations are reclaimed before the stage emits downstream.
            reordered_slab.reserve(anchors_in.size());
        } else {
            chains_slab.reserve(detail::estimated_chain_count(
                anchors_in.size(), active_cp.min_seed_count));
            reordered_slab.reserve(anchors_in.size());
        }

        if constexpr (p_kernel::emits_scalar_chaining_execution_stats) {
            if (stats_dst == nullptr) {
                bioseq_log<logging_level::error>(
                    "scalar chaining stage has no execution-stats sink");
                std::abort();
            }
            scalar_pipeline_execution_stats stats{};
            p_kernel::call(
                anchors_span, active_cp, chains_slab, reordered_slab, &stats);
            /* This receipt is written after the kernel returns, so a zero-hit
             * negative is attested without claiming scalar score/backtrack
             * work that the kernel correctly skipped. */
            stats.record_scalar_chain_stage_completion(
                static_cast<std::uint64_t>(anchors_span.size()));
            stats_dst->record_scalar_pipeline_execution_stats(stats);
        } else if constexpr (p_kernel::emits_simd_chaining_execution_stats) {
            if (stats_dst == nullptr) {
                bioseq_log<logging_level::error>("simd chaining stage has no execution-stats sink");
                std::abort();
            }
            ::bioseq::chaining::simd_chaining_execution_stats stats{};
            p_kernel::call(
                highway_kernels, anchors_span, active_cp,
                chains_slab, reordered_slab, &stats);
            if (!stats.is_internally_consistent(
                    active_cp.simd_policy,
                    /*require_nonempty_vector_work=*/false)) {
                const auto status = stats.has_consistent_policy_stats(
                        active_cp.simd_policy)
                    ? simd_chaining_status::invalid_parameters
                    : simd_chaining_status::invalid_policy;
                throw simd_chaining_runtime_error(
                    status,
                    "selected policy does not match operation-originated "
                    "simd chaining stats");
            }
            stats_dst->record_simd_chaining_execution_stats(stats);
        } else if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
            if (stats_dst == nullptr) {
                bioseq_log<logging_level::error>(
                    "Vulkan chaining stage has no execution-stats sink");
                std::abort();
            }
            if (p_kernel::call(
                    anchors_span, active_cp, chains_slab, reordered_slab)) {
                partial_vulkan_execution_stats stats{};
                stats.chain_score_dispatches = 1;
                stats_dst->record_partial_vulkan_execution_stats(stats);
            }
        } else {
            p_kernel::call(anchors_span, active_cp, chains_slab, reordered_slab);
        }

        if (host_timing_dst != nullptr) {
            host_timing_dst->record_chain_score_host_time(
                elapsed_host_stage_ns(host_stage_started));
        }

        return std::move(token).emit_batch_pair(
            std::span<chain> {chains_slab.data(),    chains_slab.size()},
            std::span<anchor>{reordered_slab.data(), reordered_slab.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        stage_algorithm_description algo;
        algo.name = std::string{p_kernel::algorithm_label()};
        algo.parameters.push_back({"min_chain_score", std::to_string(cp.min_chain_score)});
        algo.parameters.push_back({"min_seed_count",  std::to_string(cp.min_seed_count)});
        algo.parameters.push_back({"max_chain_skip",  std::to_string(cp.max_chain_skip)});
        algo.parameters.push_back({"max_chain_iter",  std::to_string(cp.max_chain_iter)});
        algo.parameters.push_back({"bandwidth",       std::to_string(cp.bandwidth)});
        algo.parameters.push_back({"max_gap",         std::to_string(cp.max_gap)});
        // Let the selected kernel append its own runtime params (e.g. the
        // Vulkan backend's stage / vulkan_enabled markers) so the
        // EXPLAIN metadata is not dead code. No-op for the scalar/simd/range_minimum_query
        // kernels.
        p_kernel::populate_runtime_params(cp, algo.parameters);
        algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
        algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
        desc.algorithm = std::move(algo);
        // chain stage's two arena slabs are sized from the runtime
        // anchor count (which is unknown at describe time), so we leave
        // `estimated_batch_size` empty rather than fabricate a per-query
        // figure that doesn't reflect the actual reserve.  The
        // `[reserved: ...]` line in EXPLAIN is therefore omitted for
        // chain_stage; the runtime `chains_slab.reserve(...)` call still
        // sizes from `estimated_chain_count(anchors_in.size(), ...)`.
        return desc;
    }
};

/**
 * @brief Chain-to-region conversion stage.
 *
 * Wraps the `generate_regions` kernel, which writes its
 * `alignment_region`-per-chain output directly into a caller-provided
 * arena_slab via placement-new. `alignment_region` owns a `std::vector<cigar_operation>`,
 * so it is not trivially-destructible; the slab's destructor walks
 * `[0, len_)` and calls `~alignment_region()` on each live element before the
 * arena reclaims the storage.
 */
struct region_stage : stage_base<region_stage, alignment_region> {
    static constexpr std::string_view stage_name = "region_stage";

    sequence_size_t query_len = 0;
    match_result_collection* host_timing_dst = nullptr;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_chain_batch, class p_anchor_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_pair(p_chain_batch&& chains, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        if (host_timing_dst != nullptr) {
            host_timing_dst->begin_region_host_phase(host_stage_now_ns());
        }
        // `alignment_region` owns a `std::vector<cigar_operation>` so the slab uses the
        // placement-new pattern; no intermediate `std::vector` is needed.
        auto regions_slab = make_arena_slab<alignment_region>();
        generate_regions(
            std::span<chain>{chains.data(), chains.size()},
            std::span<const anchor>{anchors.data(), anchors.size()},
            query_len,
            regions_slab);

        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions_slab.data(), regions_slab.size()},
            std::span<const anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        stage_algorithm_description algo;
        algo.name = "Region generation";
        algo.parameters.push_back({"query_len", std::to_string(query_len)});
        desc.algorithm = std::move(algo);
        // One region per chain (exact); the actual chain count is only
        // known at runtime so the describe-time figure is left empty.  The
        // runtime reserve in process_pair sizes the slab from chains.size().
        return desc;
    }
};

/**
 * @brief In-place primary/secondary classification stage.
 *
 * Mutates the regions span via `classify_primary_secondary` (which takes
 * std::vector<alignment_region>& and modifies parent / score fields).  The
 * vector-wrapper here copies in and out; the touched region count is bounded
 * by best_n + 16 so the cost is negligible.
 */
struct primary_secondary_stage : stage_base<primary_secondary_stage, alignment_region> {
    static constexpr std::string_view stage_name = "primary_secondary_stage";

    float   mask_level;
    int32_t mask_len;
    int32_t sub_diff_value;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_pair(p_region_batch&& regions, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        // The kernel mutates in place through a span without a vector round-trip.
        classify_primary_secondary(
            std::span<alignment_region>{regions.data(), regions.size()},
            mask_level, mask_len, sub_diff_value);
        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions.data(), regions.size()},
            std::span<const anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        stage_algorithm_description algo;
        algo.name = "Classify primary/secondary";
        algo.parameters.push_back({"mask_level",
            std::to_string(static_cast<double>(mask_level))});
        algo.parameters.push_back({"mask_len", std::to_string(mask_len)});
        algo.parameters.push_back({"sub_diff", std::to_string(sub_diff_value)});
        desc.algorithm = std::move(algo);
        return desc;
    }
};

/**
 * @brief In-place secondary-region filtering stage.
 */
struct secondary_filter_stage : stage_base<secondary_filter_stage, alignment_region> {
    static constexpr std::string_view stage_name = "secondary_filter_stage";

    float   pri_ratio;
    int32_t min_diff_value;
    int32_t best_n;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_pair(p_region_batch&& regions, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        // The kernel filters in place via a span; the returned effective size
        // gives the downstream stage a shorter prefix.
        const std::size_t new_size = filter_secondary_regions(
            std::span<alignment_region>{regions.data(), regions.size()},
            pri_ratio, min_diff_value, best_n);
        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions.data(), new_size},
            std::span<const anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        stage_algorithm_description algo;
        algo.name = "Filter secondary regions";
        algo.parameters.push_back({"pri_ratio",
            std::to_string(static_cast<double>(pri_ratio))});
        algo.parameters.push_back({"min_diff", std::to_string(min_diff_value)});
        algo.parameters.push_back({"best_n",   std::to_string(best_n)});
        desc.algorithm = std::move(algo);
        return desc;
    }
};

/**
 * @brief Conditional divergence-estimation stage.
 */
template <class p_minimizer, class p_query_minimizers = std::vector<p_minimizer>>
struct divergence_stage : stage_base<divergence_stage<p_minimizer, p_query_minimizers>, alignment_region> {
    static constexpr std::string_view stage_name = "divergence_stage";

    const p_query_minimizers* query_minimizers_;
    sequence_size_t query_len;
    sequence_size_t target_len;
    int32_t runtime_kmer_size;
    bool    enabled;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_pair(p_region_batch&& regions, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        if (enabled) {
            const int32_t kmer_size = p_aligner_parameters::generic_fallback
                ? runtime_kmer_size
                : static_cast<int32_t>(p_aligner_parameters::kmer_len);
            estimate_divergence_from_seeds<p_minimizer>(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const p_minimizer>{
                    query_minimizers_->data(), query_minimizers_->size()},
                std::span<const anchor>{anchors.data(), anchors.size()},
                query_len, kmer_size, target_len);
        }
        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions.data(), regions.size()},
            std::span<const anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            stage_algorithm_description algo;
            algo.name = "Estimate divergence from seeds";
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

[[nodiscard]] inline ::bioseq::prefilter::resolved_prefilter_algorithm
resolve_prefilter_algorithm(
    ::bioseq::prefilter::prefilter_backend backend,
    std::span<const prefilter::prefilter_job> jobs) noexcept
{
    using backend_type = ::bioseq::prefilter::prefilter_backend;
    using algorithm_type = ::bioseq::prefilter::resolved_prefilter_algorithm;
    if (backend != backend_type::vulkan) {
        return algorithm_type::sneaky_snake;
    }
    for (const prefilter::prefilter_job& job : jobs) {
        const bool measured_length = job.query_length == job.target_length
            && (job.query_length == 32u || job.query_length == 64u);
        const bool measured_threshold =
            job.max_edits == 2u || job.max_edits == 4u;
        if (!measured_length || !measured_threshold) {
            return algorithm_type::sneaky_snake;
        }
    }
    return jobs.empty()
        ? algorithm_type::sneaky_snake
        : algorithm_type::gatekeeper;
}

/**
 * Query-scoped rank-prefilter lower-bound gate.  Only mandatory global
 * seed-to-seed fills are evaluated; local left/right extension windows are
 * intentionally excluded.  A region is removed only when the sum of proven
 * per-fill lower bounds exceeds the SQL predicate's maximum edit count.
 */
template <sequence_alphabet p_alphabet>
struct prefilter_stage
    : stage_base<prefilter_stage<p_alphabet>, alignment_region> {
    static constexpr std::string_view stage_name = "prefilter_stage";

    ::bioseq::simd::minimap2_parameters params;
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_;
    match_result_collection* stats_dst_ = nullptr;

    [[nodiscard]] static constexpr std::string_view backend_name(
        ::bioseq::prefilter::prefilter_backend backend) noexcept {
        using backend_type = ::bioseq::prefilter::prefilter_backend;
        switch (backend) {
            case backend_type::automatic: return "auto";
            case backend_type::scalar: return "scalar";
            case backend_type::simd: return "highway_simd";
            case backend_type::vulkan: return "vulkan";
        }
        return "unknown";
    }

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    emitted_token<p_sink> process_pair(
        p_region_batch&& regions, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        if (!params.prefilter_enabled || !params.compute_edit_distance
            || regions.size() == 0) {
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        static const auto* prefilter_region =
            bioseq::instrumentation::register_static_region("prefilter");
        auto prefilter_scope = bioseq::instrumentation::region(prefilter_region);

        std::vector<std::uint8_t> query_ranks;
        std::vector<std::uint8_t> target_ranks;
        std::vector<prefilter::prefilter_job> jobs;
        std::vector<std::size_t> job_regions;
        const std::uint64_t max_flat_offset =
            (std::numeric_limits<std::uint32_t>::max)();

        for (std::size_t region_index = 0;
             region_index < regions.size(); ++region_index) {
            const alignment_region& region = regions[region_index];
            (void)for_each_mandatory_alignment_gap(
                region,
                std::span<const anchor>{anchors.data(), anchors.size()},
                params,
                [this, &query_ranks, &target_ranks, &jobs, &job_regions,
                 &region, region_index, max_flat_offset](
                    const mandatory_alignment_gap_window& window) {
                    const std::uint64_t query_length =
                        window.query_end - window.query_start;
                    const std::uint64_t target_length =
                        window.target_end - window.target_start;
                    if (window.query_end > query_seq_.bp_size()
                        || window.target_end > target_seq_.bp_size()
                        || query_length > max_flat_offset
                        || target_length > max_flat_offset
                        || query_ranks.size() > max_flat_offset - query_length
                        || target_ranks.size() > max_flat_offset - target_length) {
                        /* Unsupported valid geometry is a conservative accept,
                         * never a hidden fallback or false rejection. */
                        return true;
                    }

                    const auto query_offset = static_cast<std::uint32_t>(
                        query_ranks.size());
                    const auto target_offset = static_cast<std::uint32_t>(
                        target_ranks.size());
                    query_ranks.resize(
                        query_ranks.size() + static_cast<std::size_t>(query_length));
                    target_ranks.resize(
                        target_ranks.size() + static_cast<std::size_t>(target_length));
                    std::span<std::uint8_t> query_destination{
                        query_ranks.data() + query_offset,
                        static_cast<std::size_t>(query_length)};
                    std::span<std::uint8_t> target_destination{
                        target_ranks.data() + target_offset,
                        static_cast<std::size_t>(target_length)};
                    if (region.is_reverse) {
                        query_seq_.template to_encoded_into<true, false>(
                            query_destination,
                            static_cast<std::size_t>(window.query_start),
                            static_cast<std::size_t>(window.query_end));
                    } else {
                        query_seq_.template to_encoded_into<false, false>(
                            query_destination,
                            static_cast<std::size_t>(window.query_start),
                            static_cast<std::size_t>(window.query_end));
                    }
                    target_seq_.template to_encoded_into<false, false>(
                        target_destination,
                        static_cast<std::size_t>(window.target_start),
                        static_cast<std::size_t>(window.target_end));
                    jobs.push_back(prefilter::prefilter_job{
                        .query_offset = query_offset,
                        .target_offset = target_offset,
                        .query_length = static_cast<std::uint32_t>(query_length),
                        .target_length = static_cast<std::uint32_t>(target_length),
                        .max_edits = params.prefilter_max_edits,
                    });
                    job_regions.push_back(region_index);
                    return true;
                });
        }

        if (jobs.empty()) {
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }

        const prefilter::prefilter_batch_input input{
            .query_ranks = query_ranks,
            .target_ranks = target_ranks,
            .jobs = jobs,
        };
        std::vector<prefilter::prefilter_result> results;
        using backend_type = ::bioseq::prefilter::prefilter_backend;
        using algorithm_type = ::bioseq::prefilter::resolved_prefilter_algorithm;
        const backend_type selected = params.prefilter_backend
            == backend_type::automatic
                ? backend_type::simd
                : params.prefilter_backend;
        const algorithm_type algorithm =
            resolve_prefilter_algorithm(selected, jobs);
        switch (selected) {
            case backend_type::scalar:
                results = sneaky_snake_scalar_batch(input);
                break;
            case backend_type::simd:
            case backend_type::automatic: {
                if (params.highway_kernels == nullptr
                    || !params.highway_kernels->has_kernel(
                        ::bioseq::simd_dispatch::highway_kernel::
                            prefilter_sneaky_snake_simd_batch)
                    || params.highway_kernels->common
                            .sneaky_snake_simd_batch == nullptr) {
                    throw std::logic_error{
                        "selected Highway table has no SneakySnake kernel"};
                }
                results = params.highway_kernels->common
                    .sneaky_snake_simd_batch(input, {});
                break;
            }
            case backend_type::vulkan: {
                if (algorithm == algorithm_type::gatekeeper) {
                    results = prefilter::run_gatekeeper_vulkan_batch(
                        query_ranks, target_ranks, jobs);
                } else {
                    results = prefilter::run_sneaky_snake_vulkan_batch(
                        query_ranks, target_ranks, jobs);
                }
                break;
            }
        }
        if (results.size() != jobs.size()) {
            throw std::runtime_error(
                "rank prefilter backend returned the wrong result count");
        }

        const std::size_t input_region_count = regions.size();
        std::uint64_t evaluated_jobs = 0u;
        std::uint64_t length_64_jobs = 0u;
        std::uint64_t length_difference_jobs = 0u;
        std::uint64_t unsupported_jobs = 0u;
        std::uint64_t rejected_jobs = 0u;
        std::uint64_t mask_rejection_candidates = 0u;
        std::uint64_t exact_guard_activations = 0u;
        std::uint64_t guard_confirmed_rejections = 0u;
        std::uint64_t hamming_exact_rejections = 0u;
        std::vector<std::uint64_t> lower_bounds(regions.size(), 0u);
        std::vector<bool> keep(regions.size(), true);
        for (std::size_t index = 0; index < results.size(); ++index) {
            const prefilter::prefilter_result& result = results[index];
            const std::size_t region_index = job_regions[index];
            switch (static_cast<prefilter::prefilter_status>(result.status)) {
                case prefilter::prefilter_status::success:
                    ++evaluated_jobs;
                    if (jobs[index].query_length == 64u
                        && jobs[index].target_length == 64u) {
                        ++length_64_jobs;
                    }
                    break;
                case prefilter::prefilter_status::conservative_unequal:
                    ++length_difference_jobs;
                    break;
                case prefilter::prefilter_status::unsupported_conservative:
                    ++unsupported_jobs;
                    break;
                default:
                    throw std::runtime_error(
                        "rank prefilter backend returned an unknown status");
            }
            if (algorithm == algorithm_type::gatekeeper) {
                mask_rejection_candidates +=
                    prefilter::is_gatekeeper_mask_rejection_candidate(result)
                        ? 1u : 0u;
                exact_guard_activations +=
                    prefilter::is_gatekeeper_exact_guard_activated(result)
                        ? 1u : 0u;
                guard_confirmed_rejections +=
                    prefilter::is_gatekeeper_guard_confirmed_rejection(result)
                        ? 1u : 0u;
                hamming_exact_rejections +=
                    prefilter::is_gatekeeper_hamming_exact_rejection(result)
                        ? 1u : 0u;
            }
            if (!prefilter::is_prefilter_accepted(result)) ++rejected_jobs;
            if (result.status == static_cast<std::uint32_t>(
                    prefilter::prefilter_status::unsupported_conservative)) continue;
            lower_bounds[region_index] =
                bioseq::common::saturating_add(
                    lower_bounds[region_index],
                    static_cast<std::uint64_t>(result.lower_bound));
            if (!prefilter::is_prefilter_accepted(result)
                || lower_bounds[region_index] > params.prefilter_max_edits) {
                keep[region_index] = false;
            }
        }

        std::size_t output_size = 0;
        for (std::size_t index = 0; index < regions.size(); ++index) {
            if (!keep[index]) continue;
            if (output_size != index) {
                regions.data()[output_size] = std::move(regions.data()[index]);
            }
            ++output_size;
        }
        if (stats_dst_ == nullptr) {
            throw std::logic_error(
                "rank prefilter stage has no execution-stats sink");
        }
        ::bioseq::prefilter::prefilter_execution_stats call_stats{};
        call_stats.record_batch(
            algorithm,
            selected,
            static_cast<std::uint64_t>(jobs.size()),
            evaluated_jobs,
            length_difference_jobs,
            unsupported_jobs,
            rejected_jobs,
            static_cast<std::uint64_t>(input_region_count - output_size),
            static_cast<std::uint64_t>(output_size),
            mask_rejection_candidates,
            exact_guard_activations,
            guard_confirmed_rejections,
            hamming_exact_rejections,
            length_64_jobs);
        stats_dst_->record_prefilter_execution_stats(call_stats);
        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions.data(), output_size},
            std::span<const anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        using backend_type = ::bioseq::prefilter::prefilter_backend;
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        if (params.prefilter_enabled) {
            stage_algorithm_description algorithm;
            algorithm.name = "Rank edit-distance prefilter";
            algorithm.parameters.push_back({
                "backend", std::string{backend_name(params.prefilter_backend)}});
            algorithm.parameters.push_back({
                "algorithm",
                params.prefilter_backend == backend_type::vulkan
                    ? "batch_resolved_gatekeeper_or_sneaky_snake"
                    : "sneaky_snake"});
            algorithm.parameters.push_back({
                "max_edits", std::to_string(params.prefilter_max_edits)});
            algorithm.parameters.push_back({
                "submaze_width",
                std::to_string(
                    prefilter::sneaky_snake_default_submaze_width)});
            algorithm.computed_properties.push_back({
                "domain", "mandatory seed-to-seed global fills"});
            algorithm.computed_properties.push_back({
                "survivor_check", "exact post-alignment edit distance"});
            algorithm.computed_properties.push_back({
                "gatekeeper_auto_domain", "length in {32,64}, p_element in {2,4}"});
            if (params.prefilter_backend == backend_type::simd
                || params.prefilter_backend == backend_type::automatic) {
                algorithm.highway_kernels = ::bioseq::simd_dispatch::
                    highway_kernel_mask_for(
                        ::bioseq::simd_dispatch::highway_kernel::
                            prefilter_sneaky_snake_simd_batch);
            }
            desc.algorithm = std::move(algorithm);
        }
        return desc;
    }
};

/**
 * Authoritative exact edit-distance predicate gate.
 *
 * This runs after traceback has populated `alignment_region::edit_distance` and
 * before the final primary/secondary and MAPQ pass.  Consequently a candidate
 * that does not satisfy the SQL predicate cannot influence the classification
 * or mapping-quality metadata of a candidate that does.  The earlier
 * rank-prefilter stage remains only a conservative work-avoidance gate.
 */
struct exact_edit_filter_stage
    : stage_base<exact_edit_filter_stage, alignment_region> {
    static constexpr std::string_view stage_name = "exact_edit_filter_stage";

    ::bioseq::simd::minimap2_parameters params;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    emitted_token<p_sink> process_pair(
        p_region_batch&& regions, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        if (!params.exact_edit_filter_enabled) {
            return std::move(token).emit_batch_pair(
                std::span<alignment_region>{regions.data(), regions.size()},
                std::span<const anchor>{anchors.data(), anchors.size()});
        }
        if (!params.compute_edit_distance) {
            throw std::logic_error(
                "exact edit filter requires exact edit-distance accounting");
        }

        const std::int64_t exclusive_upper_bound =
            params.exact_edit_filter_exclusive_upper_bound;
        std::size_t output_size = 0;
        for (std::size_t index = 0; index < regions.size(); ++index) {
            alignment_region& region = regions.data()[index];
            if (!region.has_alignment) {
                throw std::runtime_error(
                    "exact edit filter received a region without alignment traceback");
            }
            const bool keep = exclusive_upper_bound > 0
                && region.edit_distance < static_cast<std::uint64_t>(
                    exclusive_upper_bound);
            if (!keep) continue;

            if (output_size != index) {
                regions.data()[output_size] = std::move(region);
            }
            alignment_region& survivor = regions.data()[output_size];
            survivor.region_id = static_cast<std::int32_t>(output_size);
            survivor.parent = -1;
            survivor.secondary_score = 0;
            survivor.secondary_count = 0;
            survivor.secondary_dp_max = 0;
            survivor.mapq = 0;
            ++output_size;
        }

        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions.data(), output_size},
            std::span<const anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        if (params.exact_edit_filter_enabled) {
            stage_algorithm_description algorithm;
            algorithm.name = "Exact edit-distance predicate";
            algorithm.parameters.push_back({
                "exclusive_upper_bound",
                std::to_string(
                    params.exact_edit_filter_exclusive_upper_bound)});
            algorithm.computed_properties.push_back({
                "classification_domain", "exact predicate survivors"});
            desc.algorithm = std::move(algorithm);
        }
        return desc;
    }
};

/**
 * @brief Conditional base-level alignment stage.
 *
 * Algorithm dispatch occurs through `p_kernel = kernel_t<p_alignment_algorithm>`;
 * runtime parameters are populated by `p_kernel::populate_runtime_params(...)`.
 */
template <class p_kernel,
          sequence_alphabet p_alphabet>
struct align_stage : stage_base<align_stage<p_kernel, p_alphabet>, alignment_region> {
    static constexpr std::string_view stage_name = "align_stage";

    ::bioseq::simd::minimap2_parameters params;
    const scoring_matrix<p_alphabet>*                sm;
    const ::bioseq::simd::alignment_kernels<p_alphabet>* alignment_kernels_;
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_;
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_;
    bool                                          enabled;
    match_result_collection*                        stats_dst_;
    match_result_collection*                        host_timing_dst_;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_pair(p_region_batch&& regions, p_anchor_batch&& anchors, emit_token<p_sink>&& token) {
        if (host_timing_dst_ != nullptr) {
            host_timing_dst_->transition_region_to_alignment_result_host_phase(
                host_stage_now_ns(), enabled);
        }
        if (enabled) {
            if constexpr (p_kernel::emits_scalar_ksw2_execution_stats) {
                if (stats_dst_ == nullptr) {
                    bioseq_log<logging_level::error>(
                        "scalar ksw2 stage has no execution-stats sink");
                    std::abort();
                }
                scalar_pipeline_execution_stats call_stats{};
                p_kernel::template call<p_alphabet>(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, *alignment_kernels_, params,
                    /*forbidden_block_stats=*/nullptr,
                    &call_stats);
                stats_dst_->record_scalar_pipeline_execution_stats(
                    call_stats);
            } else if constexpr (
                    p_kernel::emits_ksw2_batched_execution_stats) {
                if (stats_dst_ == nullptr) {
                    bioseq_log<logging_level::error>(
                        "ksw2 batched stage has no execution-stats sink");
                    std::abort();
                }
                ksw2_batched_execution_stats call_stats{};
                p_kernel::template call<p_alphabet>(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, *alignment_kernels_, params,
                    /*forbidden_block_stats=*/nullptr,
                    /*scalar_execution_stats=*/nullptr,
                    &call_stats);
                if (!call_stats.is_coherent(
                        /*require_nonempty_work=*/false)) {
                    bioseq_log<logging_level::error>(
                        "ksw2 batched stage produced rejected execution stats");
                    std::abort();
                }
                stats_dst_->record_ksw2_batched_execution_stats(
                    call_stats);
            } else if constexpr (p_kernel::emits_block_execution_stats) {
                if (stats_dst_ == nullptr) {
                    bioseq_log<logging_level::error>(
                        "Block Aligner stage has no execution-stats sink");
                    std::abort();
                }
                block_aligner_execution_stats call_stats{};
                p_kernel::template call<p_alphabet>(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, *alignment_kernels_, params,
                    &call_stats,
                    /*scalar_execution_stats=*/nullptr);
                if (!call_stats.is_internally_consistent()) {
                    bioseq_log<logging_level::error>(
                        "Block Aligner stage produced a rejected execution stats");
                    std::abort();
                }
                stats_dst_->record_block_aligner_execution_stats(
                    call_stats);
            } else {
                p_kernel::template call<p_alphabet>(
                    std::span<alignment_region>{regions.data(), regions.size()},
                    std::span<const ::bioseq::chaining::anchor>{
                        anchors.data(), anchors.size()},
                    query_seq_, target_seq_, *sm, *alignment_kernels_, params,
                    /*forbidden_block_stats=*/nullptr,
                    /*scalar_execution_stats=*/nullptr);
            }
        }
        return std::move(token).emit_batch_pair(
            std::span<alignment_region>{regions.data(), regions.size()},
            std::span<const ::bioseq::chaining::anchor>{
                anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            stage_algorithm_description algo;
            algo.name = std::string{p_kernel::algorithm_label()};
            p_kernel::populate_runtime_params(params, algo.parameters);
            algo.computed_properties = p_kernel::template static_properties<p_aligner_parameters>();
            algo.highway_kernels = detail::stage_highway_kernels<p_kernel>();
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Conditional mapping-quality stage.
 *
 * Accepts a `(regions, anchors)` pair upstream but only uses the regions
 * span; emits the regions span downstream as a single batch.
 */
struct mapq_stage : stage_base<mapq_stage, alignment_region> {
    static constexpr std::string_view stage_name = "mapq_stage";

    ::bioseq::simd::minimap2_parameters params;
    bool                                          enabled;

    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process(p_region_batch&& regions, emit_token<p_sink>&& token) {
        if (enabled) {
            compute_mapping_quality(
                std::span<alignment_region>{regions.data(), regions.size()},
                params, 0);
        }
        return std::move(token).emit_batch(
            std::span<alignment_region>{regions.data(), regions.size()});
    }

    //: pair → single delegation.  The outer body has no
    // surviving `emit_token<p_sink>` after the inner call moves it, so a
    // second emit is structurally blocked.
    template <is_constexpr_aligner_params p_aligner_parameters, class p_region_batch, class p_anchor_batch, class p_sink>
    [[gnu::always_inline]]
    emitted_token<p_sink> process_pair(p_region_batch&& regions, p_anchor_batch&& /*unused*/, emit_token<p_sink>&& token) {
        return process<p_aligner_parameters>(std::forward<p_region_batch>(regions), std::move(token));
    }

    template <is_constexpr_aligner_params p_aligner_parameters>
    [[nodiscard]] stage_description describe() const {
        stage_description desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            stage_algorithm_description algo;
            algo.name = "Compute mapping quality (mm_set_mapq2)";
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Terminal sink: push converted MatchResults into the collection.
 *
 * Not a `bare_stage` — appears as the tail of `make_pipeline<p_aligner_parameters>(...)`.
 */
struct sink_stage {
    match_result_collection* dst_;
    sequence_size_t        query_len  = 0;
    sequence_size_t        target_len = 0;
    match_result_collection* host_timing_dst_ = nullptr;

    [[gnu::always_inline]] inline void on_batch(std::span<alignment_region> regions) {
        dst_->results.reserve(dst_->results.size() + regions.size());
        // `region_slot` is move-consumed into the converted match_result; the
        // slot is left in a valid-but-moved-from state and reclaimed by
        // whichever owner (arena slab or std::vector) holds the span.
        for (auto& region_slot : regions) {
            dst_->results.push_back(
                region_to_match_result(std::move(region_slot), query_len, target_len));
        }
        if (host_timing_dst_ != nullptr
            && host_timing_dst_->is_alignment_result_host_stage_active) {
            host_timing_dst_->finish_alignment_result_host_phase(
                host_stage_now_ns());
        }
    }

    [[gnu::always_inline]] inline void on_completed() noexcept {
        if (host_timing_dst_ != nullptr
            && host_timing_dst_->is_alignment_result_host_stage_active) {
            host_timing_dst_->finish_alignment_result_host_phase(
                host_stage_now_ns());
        }
    }
};

// =============================================================================
// Out-of-line pipeline-description walker
// =============================================================================

template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
[[nodiscard]] pipeline_description stage<p_aligner_parameters, p_self, p_sink>::describe() const {
    // Every concrete stage description uses the same parameter specialization
    // as the runtime path.
    pipeline_description d;
    d.stages.push_back(self_.template describe<p_aligner_parameters>());
    if constexpr (requires { sink_.describe(); }) {
        pipeline_description rest = sink_.describe();
        d.stages.insert(d.stages.end(),
            std::make_move_iterator(rest.stages.begin()),
            std::make_move_iterator(rest.stages.end()));
    }
    return d;
}

/**
 * @brief Injects the p_aligner_parameters computed properties as a leading entry in the
 * pipeline description so the EXPLAIN renderer sees w/k/p_bucket_width beside the first
 * stage.
 */
template <is_constexpr_aligner_params p_aligner_parameters, class p_self, class p_sink>
[[nodiscard]] pipeline_description describe_with_params(
    const stage<p_aligner_parameters, p_self, p_sink>& pipeline)
{
    auto d = pipeline.describe();
    if (!d.stages.empty()) {
        auto& first = d.stages.front();
        if (!first.algorithm.has_value()) {
            first.algorithm.emplace();
            first.algorithm->name = "pipeline configuration";
        }
        first.algorithm->computed_properties.push_back(
            {"cep.window_len", std::to_string(p_aligner_parameters::window_len)});
        first.algorithm->computed_properties.push_back(
            {"cep.kmer_len",   std::to_string(p_aligner_parameters::kmer_len)});
        first.algorithm->computed_properties.push_back(
            {"cep.sequence_len_log2", std::to_string(p_aligner_parameters::sequence_len_log2)});
        first.algorithm->computed_properties.push_back(
            {"cep.specialised", p_aligner_parameters::generic_fallback ? "fallback" : "specialised"});
    }
    return d;
}

// =============================================================================
// make_minimap_pipeline — single source of truth for the pipeline shape
// =============================================================================

[[nodiscard]] inline bool vulkan_components_requested(
    const ::bioseq::simd::minimap2_parameters& params) noexcept;

/**
 * @brief Build the full minimap pipeline value for one (p_aligner_parameters, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point).
 *
 * All template parameters are fixed by the caller (the dispatcher cascade).
 * The function is the single place both the run path and the describe path
 * consult to learn what stages are is_available — adding or removing a stage edits
 * only this function.
 */
template <is_constexpr_aligner_params p_aligner_parameters,
          ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
          ::bioseq::simd::chaining_algorithm      p_chaining_algorithm,
          ::bioseq::simd::minimizer_algorithm     p_minimizer_algorithm,
          pipeline_entry_point                                                p_entry_point,
          sequence_alphabet p_alphabet,
          class p_minimizer, class p_index,
          class p_query_minimizers = std::vector<p_minimizer>>
[[nodiscard, gnu::always_inline]] inline auto make_minimap_pipeline(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const p_query_minimizers&            query_minimizers,
    const p_index&                     target_index,
    const scoring_matrix<p_alphabet>&    scoring_matrix,
    const ::bioseq::simd::alignment_kernels<p_alphabet>& alignment_kernels,
    match_result_collection&            output_collection,
    const ::bioseq::simd::minimap2_parameters& params,
    sequence_size_t                   query_len,
    sequence_size_t                   target_len,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
    post_seed_owned_lifetime* post_seed_owned_lifetime = nullptr)
{
    match_result_collection* host_timing_dst =
        vulkan_components_requested(params) ? nullptr : &output_collection;
    match_result_collection* chaining_stats_dst = nullptr;
    if constexpr (p_chaining_algorithm
            == ::bioseq::simd::chaining_algorithm::scalar_minimap2
        || p_chaining_algorithm
            == ::bioseq::simd::chaining_algorithm::simd_minimap2
        || p_chaining_algorithm
            == ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming) {
        chaining_stats_dst = &output_collection;
    }
    match_result_collection* alignment_stats_dst = nullptr;
    if constexpr (kernel_t<p_alignment_algorithm>::emits_block_execution_stats
        || kernel_t<p_alignment_algorithm>::emits_scalar_ksw2_execution_stats
        || kernel_t<p_alignment_algorithm>::emits_ksw2_batched_execution_stats) {
        alignment_stats_dst = &output_collection;
    }
    auto tail = make_pipeline<p_aligner_parameters>(
        index_stage<p_index>{ {}, &target_index, gpu_index_identity },
        seed_hit_stage{ {}, params, query_len, gpu_index_cache,
                      &output_collection, post_seed_owned_lifetime,
                      host_timing_dst },
        chain_stage<kernel_t<p_chaining_algorithm>>{
            {}, params.make_chaining_parameters(), query_len,
            chaining_stats_dst, host_timing_dst, params.highway_kernels },
        region_stage{ {}, query_len, host_timing_dst },
        primary_secondary_stage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        secondary_filter_stage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        divergence_stage<p_minimizer, p_query_minimizers>{
            {}, &query_minimizers, query_len, target_len, params.kmer_size,
            params.compute_divergence },
        prefilter_stage<p_alphabet>{
            {}, params, query_seq, target_seq, &output_collection },
        align_stage<kernel_t<p_alignment_algorithm>, p_alphabet>{
            {}, params, &scoring_matrix, &alignment_kernels,
            query_seq, target_seq,
            params.do_cigar || params.compute_edit_distance,
            alignment_stats_dst, host_timing_dst },
        exact_edit_filter_stage{{}, params},
        primary_secondary_stage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        secondary_filter_stage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        mapq_stage{ {}, params, params.compute_mapq },
        sink_stage{ &output_collection, query_len, target_len, host_timing_dst }
    );

    if constexpr (p_entry_point == pipeline_entry_point::from_sequence) {
        minimizer_stage<p_minimizer, kernel_t<p_minimizer_algorithm>, p_alphabet> head{
            {}, query_seq, params.window_size, params.kmer_size, params,
            &output_collection, host_timing_dst };
        return head.template attach<p_aligner_parameters>(std::move(tail));
    } else {
        return tail;
    }
}

// =============================================================================
// pipeline_run_impl_batch — batched leaf dispatcher
// =============================================================================

/**
 * @brief Run the pipeline over N queries against one shared target index.
 *
 * Opens a single outer `Arena::chain` scope for the whole batch and a nested
 * inner scope per query so each query's slabs are reclaimed before the next
 * iteration's pipeline is built.  Per-query results are appended to
 * `out_per_query` in input order.
 */
/// Return whether the resolved statement asks for any full biological Vulkan
/// component. `vulkan_enabled` is deliberately absent: it is only the runtime
/// capability gate, and enabling that gate must not turn an otherwise CPU/simd
/// tuple into a full GPU-selected statement.  The rank prefilter is also absent:
/// it is an explicitly requested, self-contained accelerator inside the generic
/// pipeline and does not authorize GPU-to-CPU fallback for minimization,
/// chaining, or alignment. The cache toggle is likewise only policy for a
/// selected seed path.
[[nodiscard]] inline bool vulkan_components_requested(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return params.vulkan_seed_hits_enabled
        || params.vulkan_fused_frontend_enabled
        || params.vulkan_gpu_index_build_enabled
        || params.vulkan_extend_enabled
        || params.minimizer_algorithm ==
               ::bioseq::simd::minimizer_algorithm::
                   vulkan_minimap_integer
        || params.chaining_algorithm ==
               ::bioseq::simd::chaining_algorithm::
                   vulkan_tiled_dynamic_programming;
}

/// Return whether any Vulkan work can execute for this statement.  This is a
/// thread-placement/runtime-lifetime predicate, not biological route
/// admission: a standalone rank prefilter is allowed inside the generic
/// CPU/simd pipeline while `vulkan_components_requested()` remains false.
[[nodiscard]] inline bool vulkan_accelerator_requested(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return vulkan_components_requested(params)
        || (params.prefilter_enabled
            && params.prefilter_backend ==
               ::bioseq::prefilter::prefilter_backend::vulkan);
}

/// Exact statement shape that the current dedicated resident GPU route can
/// execute without entering any generic CPU/simd biological stage. A selected
/// tuple outside this shape must fail before generic pipeline construction.
[[nodiscard]] inline bool complete_all_gpu_tuple_selected(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && params.vulkan_fused_frontend_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::minimizer_algorithm::
                   vulkan_minimap_integer
        && params.chaining_algorithm ==
               ::bioseq::simd::chaining_algorithm::
                   vulkan_tiled_dynamic_programming
        && params.base_alignment_algorithm ==
               ::bioseq::simd::base_alignment_algorithm::
                   block_aligner
        && !params.compute_divergence
        && !params.compute_mapq
        && !params.compute_edit_distance
        && !params.exact_edit_filter_enabled
        && !params.prefilter_enabled
        && params.max_occ >= 0
        && (!params.do_cigar || params.vulkan_extend_enabled);
}

/// Validate the target-format and algorithm tuple admitted by the dedicated
/// persisted grouped-SoA GPU preparation route. The caller supplies the live
/// supported-alphabet pack size so a legitimate zero-based alphabet code is
/// distinguished from an out-of-range code.
[[nodiscard]] inline bool is_persisted_grouped_soa_gpu_preparation_tuple_valid(
    const std::uint8_t alphabet_code,
    const std::size_t supported_alphabet_count,
    const std::uint8_t storage_unit_byte_width,
    const ::bioseq::minimizers::minimizer_sketch_backend backend,
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return static_cast<std::size_t>(alphabet_code) < supported_alphabet_count
        && (storage_unit_byte_width == 1u
            || storage_unit_byte_width == 2u
            || storage_unit_byte_width == 4u
            || storage_unit_byte_width == 8u)
        && params.kmer_size == 15
        && params.window_size == 10
        && backend == ::bioseq::minimizers::minimizer_sketch_backend::
               vulkan_segment
        && complete_all_gpu_tuple_selected(params);
}

// The Vulkan frontend's GPU target-index build replaces the CPU
// `minimizer_hashmap_fragment_index` build for this exact (minimizer, chaining)
// leaf. runtime-gated; both flags plus the matching algorithm selectors must be
// set. The runtime device check is done at the call site.
[[nodiscard]] inline bool is_gpu_target_index_build_active(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_fused_frontend_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer
        && params.chaining_algorithm ==
               ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming;
}

[[nodiscard]] inline bool is_gpu_coordinate_only_execution_active(
    const ::bioseq::simd::minimap2_parameters& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && params.vulkan_fused_frontend_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer
        && params.chaining_algorithm ==
               ::bioseq::simd::chaining_algorithm::vulkan_tiled_dynamic_programming
        && !params.do_cigar
        && !params.compute_divergence
        && !params.compute_mapq
        && !params.compute_edit_distance
        && !params.exact_edit_filter_enabled
        && !params.prefilter_enabled
        && params.max_occ >= 0;
}

struct ordered_sketch_query_gate_storage {
    static constexpr unsigned gate_bits =
        bioseq::gpu::vulkan::k_gpu_query_gate_bits;

    ordered_sketch_query_gate_storage() = default;
    ordered_sketch_query_gate_storage(const ordered_sketch_query_gate_storage&) = delete;
    ordered_sketch_query_gate_storage& operator=(const ordered_sketch_query_gate_storage&) = delete;
    ordered_sketch_query_gate_storage(ordered_sketch_query_gate_storage&&) = delete;
    ordered_sketch_query_gate_storage& operator=(ordered_sketch_query_gate_storage&&) = delete;

    // The only mutable owner is consumed at freeze.  Afterwards every user has
    // a shared_ptr<const vector>; no mutable alias can alter a published gate.
    mutable std::shared_ptr<std::vector<std::uint32_t>> mutable_words{};
    mutable std::shared_ptr<const std::vector<std::uint32_t>> frozen_words{};
    mutable bool frozen = false;
    std::uint32_t mask = 0u;

    void ensure_allocated() {
        if (frozen) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error(
                "Stage C query gate mutation attempted after freeze");
        }
        if (mutable_words) return;
        const std::uint64_t bit_count = std::uint64_t{1} << gate_bits;
        mask = static_cast<std::uint32_t>(bit_count - 1u);
        mutable_words = std::make_shared<std::vector<std::uint32_t>>(
            static_cast<std::size_t>(bit_count / 32u), 0u);
    }

    template <class p_minimizer>
    void add(std::span<const p_minimizer> minimizers) {
        ensure_allocated();
        cached_identity.reset();
        for (const p_minimizer& minimizer : minimizers) {
            const std::uint64_t bit = minimizer.kmer() & mask;
            (*mutable_words)[static_cast<std::size_t>(bit >> 5u)] |=
                std::uint32_t{1} << (bit & 31u);
        }
    }

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint32_t>>
    freeze_words() const noexcept {
        if (!frozen) {
            frozen_words =
                std::shared_ptr<const std::vector<std::uint32_t>>{
                    std::move(mutable_words)};
            frozen = true;
        }
        return frozen_words;
    }

    [[nodiscard]] bioseq::gpu::vulkan::vulkan_sketch_query_gate
    vulkan_gate() const noexcept
    {
        const auto gate_identity = identity();
        const auto owner = freeze_words();
        return bioseq::gpu::vulkan::vulkan_sketch_query_gate{
            owner
                ? std::span<const std::uint32_t>{owner->data(), owner->size()}
                : std::span<const std::uint32_t>{},
            mask,
            bioseq::gpu::vulkan::vulkan_sketch_query_gate_identity{
                .enabled = gate_identity.enabled,
                .word_count = gate_identity.word_count,
                .mask = gate_identity.mask,
                .digest_lo = gate_identity.digest_lo,
                .digest_hi = gate_identity.digest_hi,
                .canonical_words = owner,
            },
            true};
    }

    struct identity {
        bool enabled = false;
        std::uint32_t word_count = 0;
        std::uint32_t mask = 0;
        std::uint64_t digest_lo = 0;
        std::uint64_t digest_hi = 0;
        std::uint64_t set_bits = 0;
    };
    mutable std::optional<identity> cached_identity{};

    /// Canonical 128-bit digest over little-endian u32 gate words. The cache is
    /// also bound once per wave, so this is identity metadata, not permission
    /// to share a filtered resident across unrelated waves.
    [[nodiscard]] identity identity() const noexcept {
        const auto owner = freeze_words();
        if (cached_identity) return *cached_identity;
        if (!owner) {
            cached_identity = identity{};
            return *cached_identity;
        }
        std::uint64_t lo = 1469598103934665603ULL;
        std::uint64_t hi = 0x9e3779b97f4a7c15ULL;
        std::uint64_t set_bits = 0u;
        for (const std::uint32_t word : *owner) {
            set_bits += std::popcount(word);
            for (unsigned shift = 0u; shift < 32u; shift += 8u) {
                const std::uint8_t byte = static_cast<std::uint8_t>(word >> shift);
                lo = (lo ^ byte) * 1099511628211ULL;
                hi ^= static_cast<std::uint64_t>(byte)
                    + 0x9e3779b97f4a7c15ULL + (hi << 6u) + (hi >> 2u);
                hi *= 0xbf58476d1ce4e5b9ULL;
            }
        }
        cached_identity = identity{
            .enabled = true,
            .word_count = static_cast<std::uint32_t>(owner->size()),
            .mask = mask,
            .digest_lo = lo,
            .digest_hi = hi,
            .set_bits = set_bits,
        };
        return *cached_identity;
    }

    [[nodiscard]] std::shared_ptr<const std::vector<std::uint32_t>>
    canonical_words() const noexcept {
        return freeze_words();
    }
};

inline void bind_gpu_query_gate_identity(
    bioseq::indexing::gpu_minimizer_index_cache* cache,
    const ordered_sketch_query_gate_storage& gate,
    const ordered_sketch_query_gate_storage::identity& gate_identity,
    bioseq::indexing::gpu_index_cache_identity& identity)
{
    identity.gate_enabled = gate_identity.enabled;
    identity.gate_word_count = gate_identity.word_count;
    identity.gate_mask = gate_identity.mask;
    identity.gate_digest_lo = gate_identity.digest_lo;
    identity.gate_digest_hi = gate_identity.digest_hi;
    identity.gate_canonical_words = gate.canonical_words();
    if (cache == nullptr) return;
    using cache_type = bioseq::indexing::gpu_minimizer_index_cache;
    auto bound = cache->bind_gate_once(cache_type::gate_identity{
        .enabled = gate_identity.enabled,
        .word_count = gate_identity.word_count,
        .mask = gate_identity.mask,
        .digest_lo = gate_identity.digest_lo,
        .digest_hi = gate_identity.digest_hi,
        .canonical_words = gate.canonical_words(),
    });
    if (!bound) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
            "Stage C GPU index cache gate binding rejected ({})",
            bound.error() == cache_type::gate_bind_error::identity_mismatch
                ? "identity_mismatch"
                : bound.error() == cache_type::gate_bind_error::invalid_identity
                    ? "invalid_identity" : "cache_not_empty"));
    }
}

/// Freeze a query gate behind its immutable canonical owner once per wave.
/// Segment loops reuse the returned value, so they neither alias mutable words
/// nor re-hash/copy the 8 MiB gate for every target window.
[[nodiscard]] inline bioseq::gpu::vulkan::vulkan_sketch_query_gate
freeze_gpu_query_gate(
    bioseq::gpu::vulkan::vulkan_sketch_query_gate gate)
{
    auto identity = gate.canonical_identity();
    if (!identity.is_well_formed()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "Stage C query gate has a malformed canonical identity");
    }
    bioseq::gpu::vulkan::vulkan_sketch_query_gate frozen{};
    frozen.mask = identity.mask;
    frozen.identity = std::move(identity);
    frozen.identity_valid = true;
    if (frozen.identity.canonical_words) {
        frozen.words = std::span<const std::uint32_t>{
            frozen.identity.canonical_words->data(),
            frozen.identity.canonical_words->size()};
    }
    return frozen;
}

/// Bind a segmented adoption key to the runtime build's authoritative frozen
/// gate identity, after proving it is exactly the gate supplied to the build.
/// Deriving a default/ungated key for a gated build is an identity mismatch.
inline void bind_verified_gpu_segment_gate_identity(
    bioseq::indexing::gpu_minimizer_index_cache_key& key,
    const bioseq::gpu::vulkan::gpu_index_gate_identity& expected,
    const bioseq::gpu::vulkan::gpu_index_gate_identity& built)
{
    if (!expected.is_well_formed() || !built.is_well_formed()
        || !built.is_exactly_equal_to(expected)) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error(
            "segmented GPU target-index gate identity mismatch");
    }
    key.gate_enabled = built.enabled;
    key.gate_word_count = built.word_count;
    key.gate_mask = built.mask;
    key.gate_digest_lo = built.digest_lo;
    key.gate_digest_hi = built.digest_hi;
    key.gate_canonical_words = built.canonical_words;
}

[[nodiscard]] inline match_result gpu_coordinate_to_match_result(
    const bioseq::gpu::vulkan::gpu_coordinate_result_wire& coord,
    sequence_size_t query_len,
    sequence_size_t target_len)
{
    return match_result{
        .query_len = query_len,
        .query_start = coord.query_start,
        .query_end = coord.query_end,
        .is_reverse = coord.is_reverse != 0u,
        .target_len = target_len,
        .target_start = coord.target_start,
        .target_end = coord.target_end,
        .num_matches = coord.match_length,
        .block_length = coord.block_length,
        .mapq = 0,
        .chain_score = coord.chain_score,
        .dp_score = 0,
        .dp_max_score = 0,
        .seed_count = coord.seed_count,
        .secondary_chain_score = 0,
        .divergence = -1.0f,
        .is_primary = true,
        .has_cigar = false,
        .cigar = {},
    };
}

// Batched execution shares one implementation for all alignment algorithms.
template <is_constexpr_aligner_params p_aligner_parameters,
          ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
          ::bioseq::simd::chaining_algorithm      p_chaining_algorithm,
          ::bioseq::simd::minimizer_algorithm     p_minimizer_algorithm,
          pipeline_entry_point                                                p_entry_point,
          sequence_alphabet p_alphabet,
          class p_minimizer, class p_index>
[[nodiscard]] std::vector<match_result_collection> pipeline_run_impl_batch(
    std::span<const bioseq::storage::lazy_sequence_view<p_alphabet>> query_seqs,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    std::span<const std::vector<p_minimizer>>    query_minimizers_per_query,
    const p_index&                               target_index,
    const ::bioseq::simd::minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* external_gpu_index_cache = nullptr,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    auto outer_chain_scope = context::current().push<context::arena::chain>();

    bioseq_log<logging_level::debug>("[bioseq] pipe: pipeline_run_impl_batch entry — p_aligner_parameters(p_bucket_width={},p_kmer_size={},p_window_size={},fallback={}) "
            "p_alignment_algorithm={} p_chaining_algorithm={} p_minimizer_algorithm={} p_entry_point={} queries={} target_bp={}",
            p_aligner_parameters::sequence_len_log2, p_aligner_parameters::kmer_len, p_aligner_parameters::window_len,
            p_aligner_parameters::generic_fallback ? 1 : 0,
            static_cast<int>(p_alignment_algorithm), static_cast<int>(p_chaining_algorithm), static_cast<int>(p_minimizer_algorithm),
            static_cast<int>(p_entry_point), query_seqs.size(), target_seq.bp_size());

    const sequence_size_t target_len = target_seq.bp_size();
    const auto& scoring_matrix = thread_local_scoring_matrix<p_alphabet>(params);
    // Resolve once for the entire alphabet-homogeneous query wave. Every
    // per-query pipeline borrows this immutable typed binding.
    const auto& alignment_kernels =
        detail::resolve_alignment_kernels<p_alphabet>(params);

    /// — one GPU index cache for the whole batch wave, shared
    // across queries that look up the same target index. When a Vulkan seed
    // path is selected, runtime/device failure is reported before scalar work
    // can masquerade as a GPU row.
    std::optional<bioseq::indexing::gpu_minimizer_index_cache> owned_gpu_index_cache;
    if (external_gpu_index_cache == nullptr
        && params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
        bioseq::gpu::vulkan::runtime& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
        std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        owned_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache_ptr =
        external_gpu_index_cache != nullptr
            ? external_gpu_index_cache
            : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

    /// — build the target minimizer index on the GPU and inject
    // it into the wave cache under the key the seed_hit_stage will compute, so the
    // CPU `minimizer_hashmap_fragment_index` build (skipped upstream in
    // `minimap2_port` for this leaf) is never paid. If the device build declines
    // this selected GPU path fails loudly.
    ordered_sketch_query_gate_storage gpu_query_gate;
    // A production target index is complete and query-independent.  Keep the
    // gate type for explicitly named direct-matching experiments, but publish
    // the ordinary/no-index build under the canonical disabled identity.
    const auto gpu_query_gate_identity = gpu_query_gate.identity();
    if (is_gpu_target_index_build_active(params)) {
        bind_gpu_query_gate_identity(
            gpu_index_cache_ptr, gpu_query_gate, gpu_query_gate_identity,
            gpu_index_identity);
    }

    const p_index* effective_index_ptr = &target_index;
    bool vulkan_target_index_built = false;
    bioseq::gpu::vulkan::gpu_index_build_stats
        vulkan_target_index_ownership{};
    if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                  bioseq::indexing::minimizer_hashmap_fragment_index>) {
        if (gpu_index_cache_ptr != nullptr && is_gpu_target_index_build_active(params)
            && allow_gpu_target_index_build && target_index.table_empty()
            && target_len > 0) {
            auto& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
            bool injected = false;
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<p_minimizer>(
                target_seq,
                static_cast<std::size_t>(params.window_size),
                static_cast<std::size_t>(params.kmer_size),
                gpu_query_gate.vulkan_gate(),
                &decline);
            if (!built) {
                throw_vulkan_sketch_declined(
                    "Stage C GPU target-index build", decline);
            }
            auto key = bioseq::indexing::make_gpu_index_cache_key(
                target_index, gpu_index_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(
                gpu_rt, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Stage C GPU target-index adoption rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(
                        adopted.error())));
            }
            auto resident = std::move(*adopted);
            vulkan_target_index_ownership = resident->ownership;
            resident->target_len = target_len;
            auto inserted = gpu_index_cache_ptr->insert_built(key, resident);
            if (!inserted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Stage C GPU target-index cache insertion rejected ({})",
                    static_cast<unsigned>(inserted.error())));
            }
            injected = true;
            vulkan_target_index_built = true;
            bioseq_log<logging_level::error>("[gpu_target_index] GPU index injected: slot_mask={} pos={} target_len={}",
                            resident->slot_mask, resident->position_count, target_len);
        }
    }
    const p_index& effective_index = *effective_index_ptr;

    // Empty proto for the from_sequence path's unused minimizers argument.
    static const std::vector<p_minimizer> empty_minimizers_proto{};

    auto run_one_query =
        [target_len, &query_seqs, &query_minimizers_per_query, target_seq,
         &effective_index, &scoring_matrix, &alignment_kernels, &params,
         gpu_index_cache_ptr,
         gpu_index_identity, vulkan_target_index_built,
         vulkan_target_index_ownership, gpu_query_gate_identity](std::size_t qi) {
        const auto query_seq = query_seqs[qi];
        const sequence_size_t query_len = query_seq.bp_size();

        auto per_query_chain_scope = context::current().push<context::arena::chain>();

        match_result_collection out;
        if (vulkan_target_index_built
            && is_ordered_target_build_owner_query(qi)) {
            partial_vulkan_execution_stats stats{};
            stats.target_sketch_dispatches = 1;
            stats.target_index_build_dispatches = 1;
            stats.gpu_index_count_admissions =
                vulkan_target_index_ownership.count_admissions;
            stats.gpu_index_main_peak_admissions =
                vulkan_target_index_ownership.main_peak_admissions;
            stats.gpu_index_resident_transfers =
                vulkan_target_index_ownership.resident_transfers;
            stats.gpu_index_adoption_rereserve_attempts =
                vulkan_target_index_ownership.adoption_rereserve_attempts;
            stats.gpu_index_adoption_successes =
                vulkan_target_index_ownership.adoption_successes;
            stats.gpu_index_owned_resident_bytes =
                vulkan_target_index_ownership.resident_bytes;
            stats.gpu_index_gate_enabled =
                vulkan_target_index_ownership.query_gate_enabled;
            stats.gpu_index_gate_word_count =
                vulkan_target_index_ownership.query_gate_word_count;
            stats.gpu_index_gate_mask =
                vulkan_target_index_ownership.query_gate_mask;
            stats.gpu_index_gate_set_bits = gpu_query_gate_identity.set_bits;
            stats.gpu_index_gate_digest_lo =
                vulkan_target_index_ownership.query_gate_digest_lo;
            stats.gpu_index_gate_digest_hi =
                vulkan_target_index_ownership.query_gate_digest_hi;
            stats.gpu_index_gate_count_dispatches =
                vulkan_target_index_ownership.gate_count_dispatches;
            stats.gpu_index_gated_emit_dispatches =
                vulkan_target_index_ownership.gated_emit_dispatches;
            stats.gpu_index_ungated_emit_dispatches =
                vulkan_target_index_ownership.ungated_emit_dispatches;
            stats.gpu_index_exact_count_dispatches =
                vulkan_target_index_ownership.exact_count_dispatches;
            stats.gpu_index_exact_count_prefix_scan_dispatches =
                vulkan_target_index_ownership.
                    exact_count_prefix_scan_dispatches;
            stats.gpu_index_exact_count_control_readbacks =
                vulkan_target_index_ownership.
                    exact_count_control_readbacks;
            stats.gpu_index_deterministic_tile_emit_dispatches =
                vulkan_target_index_ownership.
                    deterministic_tile_emit_dispatches;
            stats.gpu_index_atomic_append_dispatches =
                vulkan_target_index_ownership.atomic_append_dispatches;
            stats.gpu_index_emit_validation_control_readbacks =
                vulkan_target_index_ownership.
                    emit_validation_control_readbacks;
            stats.gpu_index_counted_minimizer_count =
                vulkan_target_index_ownership.counted_minimizer_count;
            stats.gpu_index_verified_emit_count =
                vulkan_target_index_ownership.verified_emit_count;
            stats.gpu_index_count_emit_parity_verified =
                vulkan_target_index_ownership.count_emit_parity_verified;
            stats.gpu_index_target_minimizer_capacity_estimate =
                vulkan_target_index_ownership.
                    target_minimizer_capacity_estimate;
            stats.gpu_index_capacity_estimate_underestimated =
                vulkan_target_index_ownership.
                    capacity_estimate_underestimated;
            stats.gpu_index_arena_trimmed_before_permit_shrink =
                vulkan_target_index_ownership.
                    arena_trimmed_before_permit_shrink;
            stats.gpu_index_retained_minimizer_count =
                vulkan_target_index_ownership.retained_minimizer_count;
            stats.gpu_index_distinct_minimizer_count =
                vulkan_target_index_ownership.distinct_minimizer_count;
            out.record_partial_vulkan_execution_stats(stats);
        }
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));

        const auto& mins_for_this_query = (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers)
            ? query_minimizers_per_query[qi]
            : empty_minimizers_proto;

        // Short-circuit empty query or target inputs before pipeline assembly.
        if (query_len == 0 || target_len == 0) {
            return out;
        }

        auto pipeline =
            make_minimap_pipeline<p_aligner_parameters, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point, p_alphabet,
                                  p_minimizer, p_index>(
            query_seq, target_seq, mins_for_this_query, effective_index,
            scoring_matrix, alignment_kernels, out, params,
            query_len, target_len,
            gpu_index_cache_ptr, gpu_index_identity);

        if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers) {
            pipeline.on_batch(std::span<const p_minimizer>{
                mins_for_this_query.data(), mins_for_this_query.size()});
        } else {
            // from_sequence: minimizer_stage's process ignores the input and
            // sketches the cached query_seq_ pointer instead; pass an empty
            // span of bytes as a placeholder.
            pipeline.on_batch(std::span<const std::byte>{});
        }
        pipeline.on_completed();
        return out;
    };

    std::vector<match_result_collection> out_per_query;
    const bool may_parallel_batch =
        !vulkan_accelerator_requested(params)
        && bioseq::parallelism::has_parallel_tasks(
            bioseq::parallelism::global_pool::instance().get_mode())
        && query_seqs.size()
            >= bioseq::parallelism::task_parallel_minimums::k_query_count
        && lazy_is_worker_shareable(target_seq)
        && std::ranges::all_of(query_seqs,
               [](const auto p_query) {
                   return lazy_is_worker_shareable(p_query);
               });

    if (may_parallel_batch) {
        out_per_query.resize(query_seqs.size());
        bioseq::parallelism::parallel_for_pipeline(
            0, query_seqs.size(), 1,
            bioseq::parallelism::task_parallel_minimums::k_query_count,
            bioseq::parallelism::task_parallel_region::query_batch,
            [&out_per_query, &run_one_query](std::size_t lo, std::size_t hi) {
                for (std::size_t qi = lo; qi < hi; ++qi) {
                    out_per_query[qi] = run_one_query(qi);
                }
            });
    } else {
        out_per_query.reserve(query_seqs.size());
        for (std::size_t qi = 0; qi < query_seqs.size(); ++qi) {
            out_per_query.push_back(run_one_query(qi));
        }
    }

    // — release the wave's resident GPU index handles.
    if (owned_gpu_index_cache) {
        owned_gpu_index_cache->clear();
    }
    return out_per_query;
}

/**
 * @brief Single-query convenience wrapper around `pipeline_run_impl_batch`.
 */
template <is_constexpr_aligner_params p_aligner_parameters,
          ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
          ::bioseq::simd::chaining_algorithm      p_chaining_algorithm,
          ::bioseq::simd::minimizer_algorithm     p_minimizer_algorithm,
          pipeline_entry_point                                                p_entry_point,
          sequence_alphabet p_alphabet,
          class p_minimizer, class p_index,
          class p_query_minimizers = std::vector<p_minimizer>>
[[nodiscard, gnu::always_inline]] inline match_result_collection pipeline_run_impl(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const p_query_minimizers&            query_minimizers,
    const p_index&                     target_index,
    const ::bioseq::simd::minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false,
    transient_target_index_release_authorization
        transient_target_index_owner = {})
{
    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    // The locally owned cache must outlive PostSeed's raw cache pointer, while
    // PostSeed itself must outlive every Arena::chain that backs an admitted
    // anchor slab. Reverse destruction therefore requires this exact order.
    std::optional<bioseq::indexing::gpu_minimizer_index_cache>
        owned_gpu_index_cache;
    // Declared before the Arena::chain scope so any exact retained anchor
    // permit is released only after that arena has physically unwound.
    std::optional<post_seed_owned_lifetime> post_seed_owned_lifetime;
    auto outer_chain_scope = context::current().push<context::arena::chain>();

    const sequence_size_t target_len = target_seq.bp_size();
    const sequence_size_t query_len = query_seq.bp_size();
    const auto& scoring_matrix = thread_local_scoring_matrix<p_alphabet>(params);
    const auto& alignment_kernels =
        detail::resolve_alignment_kernels<p_alphabet>(params);

    if (gpu_index_cache == nullptr
        && params.vulkan_enabled && params.vulkan_seed_hits_enabled
        && !(gpu_index_identity.source_direct_sorted_dense
            && gpu_index_identity.canonical_empty)) {
        bioseq::gpu::vulkan::runtime& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
        std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        owned_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache_ptr =
        gpu_index_cache != nullptr
            ? gpu_index_cache
            : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

    ordered_sketch_query_gate_storage gpu_query_gate;
    // Fair no-index semantics require the full query-independent target index.
    const auto gpu_query_gate_identity = gpu_query_gate.identity();
    if (is_gpu_target_index_build_active(params)) {
        bind_gpu_query_gate_identity(
            gpu_index_cache_ptr, gpu_query_gate, gpu_query_gate_identity,
            gpu_index_identity);
    }

    const p_index* effective_index_ptr = &target_index;
    bool vulkan_target_index_built = false;
    bioseq::gpu::vulkan::gpu_index_build_stats
        vulkan_target_index_ownership{};
    if constexpr (std::same_as<std::remove_cvref_t<p_index>,
                  bioseq::indexing::minimizer_hashmap_fragment_index>) {
        if (gpu_index_cache_ptr != nullptr && is_gpu_target_index_build_active(params)
            && allow_gpu_target_index_build && target_index.table_empty()
            && target_len > 0) {
            auto& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
            bool injected = false;
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<p_minimizer>(
                target_seq,
                static_cast<std::size_t>(params.window_size),
                static_cast<std::size_t>(params.kmer_size),
                gpu_query_gate.vulkan_gate(),
                &decline);
            if (!built) {
                throw_vulkan_sketch_declined(
                    "Stage C GPU target-index build", decline);
            }
            auto key = bioseq::indexing::make_gpu_index_cache_key(
                target_index, gpu_index_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(
                gpu_rt, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Stage C GPU target-index adoption rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(
                        adopted.error())));
            }
            auto resident = std::move(*adopted);
            vulkan_target_index_ownership = resident->ownership;
            resident->target_len = target_len;
            auto inserted = gpu_index_cache_ptr->insert_built(key, resident);
            if (!inserted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "Stage C GPU target-index cache insertion rejected ({})",
                    static_cast<unsigned>(inserted.error())));
            }
            injected = true;
            vulkan_target_index_built = true;
            bioseq_log<logging_level::error>("[gpu_target_index] GPU index injected: slot_mask={} pos={} target_len={}",
                            resident->slot_mask, resident->position_count, target_len);
        }
    }
    const p_index& effective_index = *effective_index_ptr;

    match_result_collection out;
    if (vulkan_target_index_built) {
        partial_vulkan_execution_stats stats{};
        stats.target_sketch_dispatches = 1;
        stats.target_index_build_dispatches = 1;
        stats.gpu_index_count_admissions =
            vulkan_target_index_ownership.count_admissions;
        stats.gpu_index_main_peak_admissions =
            vulkan_target_index_ownership.main_peak_admissions;
        stats.gpu_index_resident_transfers =
            vulkan_target_index_ownership.resident_transfers;
        stats.gpu_index_adoption_rereserve_attempts =
            vulkan_target_index_ownership.adoption_rereserve_attempts;
        stats.gpu_index_adoption_successes =
            vulkan_target_index_ownership.adoption_successes;
        stats.gpu_index_owned_resident_bytes =
            vulkan_target_index_ownership.resident_bytes;
        stats.gpu_index_gate_enabled =
            vulkan_target_index_ownership.query_gate_enabled;
        stats.gpu_index_gate_word_count =
            vulkan_target_index_ownership.query_gate_word_count;
        stats.gpu_index_gate_mask =
            vulkan_target_index_ownership.query_gate_mask;
        stats.gpu_index_gate_set_bits = gpu_query_gate_identity.set_bits;
        stats.gpu_index_gate_digest_lo =
            vulkan_target_index_ownership.query_gate_digest_lo;
        stats.gpu_index_gate_digest_hi =
            vulkan_target_index_ownership.query_gate_digest_hi;
        stats.gpu_index_gate_count_dispatches =
            vulkan_target_index_ownership.gate_count_dispatches;
        stats.gpu_index_gated_emit_dispatches =
            vulkan_target_index_ownership.gated_emit_dispatches;
        stats.gpu_index_ungated_emit_dispatches =
            vulkan_target_index_ownership.ungated_emit_dispatches;
        stats.gpu_index_exact_count_dispatches =
            vulkan_target_index_ownership.exact_count_dispatches;
        stats.gpu_index_exact_count_prefix_scan_dispatches =
            vulkan_target_index_ownership.exact_count_prefix_scan_dispatches;
        stats.gpu_index_exact_count_control_readbacks =
            vulkan_target_index_ownership.exact_count_control_readbacks;
        stats.gpu_index_deterministic_tile_emit_dispatches =
            vulkan_target_index_ownership.deterministic_tile_emit_dispatches;
        stats.gpu_index_atomic_append_dispatches =
            vulkan_target_index_ownership.atomic_append_dispatches;
        stats.gpu_index_emit_validation_control_readbacks =
            vulkan_target_index_ownership.emit_validation_control_readbacks;
        stats.gpu_index_counted_minimizer_count =
            vulkan_target_index_ownership.counted_minimizer_count;
        stats.gpu_index_verified_emit_count =
            vulkan_target_index_ownership.verified_emit_count;
        stats.gpu_index_count_emit_parity_verified =
            vulkan_target_index_ownership.count_emit_parity_verified;
        stats.gpu_index_target_minimizer_capacity_estimate =
            vulkan_target_index_ownership.target_minimizer_capacity_estimate;
        stats.gpu_index_capacity_estimate_underestimated =
            vulkan_target_index_ownership.capacity_estimate_underestimated;
        stats.gpu_index_arena_trimmed_before_permit_shrink =
            vulkan_target_index_ownership.
                arena_trimmed_before_permit_shrink;
        stats.gpu_index_retained_minimizer_count =
            vulkan_target_index_ownership.retained_minimizer_count;
        stats.gpu_index_distinct_minimizer_count =
            vulkan_target_index_ownership.distinct_minimizer_count;
        out.record_partial_vulkan_execution_stats(stats);
    }
    out.results.reserve(detail::estimated_result_count(query_len, target_len, params));
    direct_resident_operation_stats* const direct_ledger =
        transient_target_index_owner.direct_ledger;
    const bool direct_empty_bypass = direct_ledger != nullptr
        && gpu_index_identity.source_direct_sorted_dense
        && gpu_index_identity.canonical_empty;
    const bool early_zero_consumer_bypass =
        direct_empty_bypass || query_len == 0 || target_len == 0;
    const std::size_t actual_seed_consumers =
        early_zero_consumer_bypass ? 0u : 1u;
    if (direct_ledger != nullptr) {
        direct_ledger->expected_seed_consumers = actual_seed_consumers;
        if (direct_empty_bypass) {
            direct_ledger->empty_bypass = true;
        } else if (early_zero_consumer_bypass) {
            direct_ledger->zero_query_bypasses = 1u;
        }
    }
    if (owned_gpu_index_cache
        || transient_target_index_owner.index != nullptr
        || transient_target_index_owner.operation_owned_gpu_cache != nullptr
        || transient_target_index_owner.direct_ledger != nullptr) {
        // CPU-index release authority is supplied only by `minimap2_port`,
        // which owns that transient value. GPU-cache release authority is
        // independent: even a borrowed persisted CPU index may use and then
        // retire the cache constructed by this invocation. An external cache
        // is always borrowed and is never installed here.
        post_seed_owned_lifetime.emplace(
            actual_seed_consumers,
            owned_gpu_index_cache
                ? &*owned_gpu_index_cache
                : transient_target_index_owner.
                    operation_owned_gpu_cache,
            transient_target_index_owner);
    }

    if (early_zero_consumer_bypass) {
        return out;
    }

    if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers
                  && std::same_as<std::remove_cvref_t<p_index>,
                         bioseq::indexing::minimizer_hashmap_fragment_index>) {
        // The resident seed-to-coordinate kernel is only profitable once the
        // target index is genuinely resident/GPU-built. Using it on the current
        // vulkan_segment host-materialised sparse segment indexes repeats
        // submit/readback orchestration per segment and is slower than the
        // normal Vulkan seed+chain path ( 2026-06-25).
        if (is_gpu_coordinate_only_execution_active(params)
            && allow_gpu_target_index_build
            && effective_index.table_empty()) {
            auto& runtime = bioseq::gpu::vulkan::runtime::instance();
            if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
                throw_vulkan_declined("Stage C resident seed-to-coordinate");
            }
            auto key = bioseq::indexing::make_gpu_index_cache_key(
                effective_index, gpu_index_identity);
            auto resident_index = gpu_index_cache_ptr == nullptr
                ? nullptr
                : gpu_index_cache_ptr->get_or_upload(
                      runtime, effective_index, key,
                      params.vulkan_index_cache_enabled);
            if (!resident_index) {
                throw_vulkan_declined("Stage C resident seed-to-coordinate index");
            }

            std::span<const p_minimizer> sketch_span{
                query_minimizers.data(), query_minimizers.size()};
            std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>>
                device_span{};
            if constexpr (is_minimizer_batch_view_v<p_query_minimizers>) {
                device_span = query_minimizers.device;
            }
            const bioseq::gpu::vulkan::buffer* device_words =
                (device_span && device_span->words() != nullptr
                 && device_span->count() == sketch_span.size())
                    ? device_span->words()
                    : nullptr;
            bioseq::gpu::vulkan::gpu_seed_inputs inputs{
                .host_minimizer_bytes = device_words != nullptr
                    ? std::span<const std::byte>{}
                    : std::as_bytes(sketch_span),
                .device_minimizer_words = device_words,
                .slots = &resident_index->slots,
                .positions = &resident_index->positions,
                .target_len = resident_index->target_len,
                .slot_mask = resident_index->slot_mask,
                .position_count = resident_index->position_count,
                .minimizer_count = static_cast<std::uint32_t>(sketch_span.size()),
                .minimizer_word_kind =
                    static_cast<std::uint32_t>(
                        bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_word_kind),
                .minimizer_words32_per_record =
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_words32_per_minimizer,
                .hash_shift =
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_hash_shift,
                .pos_bits =
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_position_bits,
                .bucket_count = resident_index->bucket_count,
                .target_position_offset = resident_index->target_position_offset,
            };

            auto cp = params.make_chaining_parameters();
            std::int32_t max_dist_x = cp.max_gap;
            std::int32_t max_dist_y = cp.max_gap;
            if (max_dist_x < cp.bandwidth) max_dist_x = cp.bandwidth;
            if (max_dist_y < cp.bandwidth) max_dist_y = cp.bandwidth;
            const auto chain_key =
                bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
            const auto coord = runtime.run_seed_to_coordinate_roundtrip(
                inputs,
                query_len,
                target_len,
                static_cast<std::uint32_t>(params.kmer_size),
                params.max_occ,
                chain_key,
                max_dist_x,
                max_dist_y,
                cp.bandwidth,
                std::min<std::int32_t>(cp.max_chain_iter, 3000),
                cp.gap_penalty,
                cp.skip_penalty,
                cp.max_chain_skip,
                cp.min_chain_score,
                cp.min_seed_count,
                16u);
            if (coord.valid != 0u) {
                out.results.push_back(
                    gpu_coordinate_to_match_result(coord, query_len, target_len));
            }
            return out;
        }
    }

    if (direct_ledger != nullptr) {
        direct_ledger->pipeline_started = true;
    }
    auto per_query_chain_scope = context::current().push<context::arena::chain>();
    auto pipeline =
        make_minimap_pipeline<p_aligner_parameters, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point, p_alphabet,
                              p_minimizer, p_index, p_query_minimizers>(
        query_seq, target_seq, query_minimizers, effective_index,
        scoring_matrix, alignment_kernels, out, params,
        query_len, target_len,
        gpu_index_cache_ptr, gpu_index_identity,
        post_seed_owned_lifetime ? &*post_seed_owned_lifetime : nullptr);

    if constexpr (p_entry_point == pipeline_entry_point::with_prebuilt_minimizers) {
        pipeline.on_batch(query_minimizers);
    } else {
        pipeline.on_batch(std::span<const std::byte>{});
    }
    pipeline.on_completed();
    if (direct_ledger != nullptr) {
        // Seed completion alone is insufficient: this bit is set only after
        // both the batch and the terminal pipeline callback returned normally.
        direct_ledger->pipeline_completed = true;
    }
    return out;
}

// =============================================================================
//  /  — value_sequence (non-type parameter pack carrier)
// =============================================================================
//
// `value_sequence<p_type, p_values...>` lives in `bioseq.common.templating` (
//); re-exported here so the existing pipe.cppm body keeps the
// unqualified name. See

using ::bioseq::common::templating::value_sequence;

// =============================================================================
// — minimizer_layout-driven p_bucket_width bucket derivation
// =============================================================================
//
// The bucket list is computed at compile time from the per-p_kmer_size
// transitions in `minimizer_layout::storage_bits`,
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
/// `p_bucket_width` such that `2^p_bucket_width > max_bp - 1`, lifted up to `p_kmer_size` so the resulting
/// `minimizer_layout<p_bucket_width, p_kmer_size>` keeps `pos_bits >= kmer_size`.  At p_bucket_width=63 every p_kmer_size
/// resolves to a non-void storage_type (the 128-bit specialisation handles the
/// upper band), so a clamp on the upper end is sufficient.
[[nodiscard]] constexpr std::size_t round_up_seq_len_log2(
    std::size_t max_bp, std::size_t kmer_size) noexcept
{
    const std::size_t need = max_bp > 1u ? max_bp - 1u : 1u;
    const std::size_t lg   = static_cast<std::size_t>(std::bit_width(need));
    return std::clamp<std::size_t>(lg, kmer_size, 63u);
}

static_assert(round_up_seq_len_log2(0u,            15u) == 15u);
static_assert(round_up_seq_len_log2(1u,            15u) == 15u);
static_assert(round_up_seq_len_log2(std::size_t{1} << 28, 15u) == 28u);
static_assert(round_up_seq_len_log2((std::size_t{1} << 48) + 1u, 15u) == 49u);
static_assert(round_up_seq_len_log2(std::size_t{1} << 62, 15u) == 62u);

namespace detail {

/// Pack-expansion builder for a per-p_kmer_size table of `storage_bits` values keyed
/// by p_bucket_width ∈ [0, 64).  The layout type takes (p_bucket_width, p_kmer_size) as template parameters, so
/// the only way to convert a runtime p_bucket_width into a template argument is to
/// pre-compute every value of `minimizer_layout<p_bucket_width, p_kmer_size>::storage_bits` for p_bucket_width
/// in the legal range and index into the resulting `std::array`.  All work
/// happens at constant-evaluation time; the dispatcher path never enters
/// this helper.
template <std::size_t p_kmer_size, std::size_t... p_bucket_values>
[[nodiscard]] consteval auto make_storage_bits_table_impl(
    std::index_sequence<p_bucket_values...>) noexcept
{
    using ::bioseq::minimizers::minimizer_layout;
    return std::array<std::size_t, sizeof...(p_bucket_values)>{
        minimizer_layout<p_bucket_values, p_kmer_size>::storage_bits...
    };
}

template <std::size_t p_kmer_size>
[[nodiscard]] consteval auto make_storage_bits_table() noexcept {
    return make_storage_bits_table_impl<p_kmer_size>(std::make_index_sequence<64u>{});
}

/// Look up `minimizer_layout<p_bucket_width, p_kmer_size>::storage_bits` from the per-p_kmer_size table.  The
/// per-p_kmer_size clauses are a *dispatch table*, not a hand-tuned ladder — they exist
/// because the layout requires p_kmer_size as a *template* argument and the consteval
/// helper above can only enumerate p_bucket_width for a fixed compile-time p_kmer_size.  Every p_kmer_size
/// in `kmer_size_buckets` must have a matching clause here.
[[nodiscard]] consteval std::size_t storage_bits_for(
    std::size_t p_bucket_width, std::size_t p_kmer_size) noexcept
{
    if (p_kmer_size == 15u) return make_storage_bits_table<15u>()[p_bucket_width];
    if (p_kmer_size == 7u)  return make_storage_bits_table<7u>()[p_bucket_width];
    if (p_kmer_size == 27u) return make_storage_bits_table<27u>()[p_bucket_width];
    return 0u;
}

/// Compute the breakpoint p_bucket_width values for a given p_kmer_size.  For each p_bucket_width in [p_kmer_size, 63]:
///   - p_bucket_width is emitted if storage_bits_for(p_bucket_width, p_kmer_size) != storage_bits_for(p_bucket_width-1, p_kmer_size),
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
static_assert(compute_breakpoints_for<7u>().second  <= 6u);
static_assert(compute_breakpoints_for<27u>().second <= 6u);

/// Work-buffer type for `compute_global_l_buckets`.  Sized 32 (≤ 6
/// breakpoints/p_kmer_size × ≤ 5 kmer sizes = 30; rounded up to a power of two for
/// headroom).
using raw_buf = std::array<std::size_t, 32u>;

/// Free consteval helper — merges compute_breakpoints_for<p_kmer_size>()'s breakpoint
/// list into (raw, raw_n) using the lifted `append_unique_value` primitive.
template <std::size_t p_kmer_size>
consteval void merge_breakpoints_into(
    raw_buf& raw, std::size_t& raw_n) noexcept
{
    constexpr auto bp = compute_breakpoints_for<p_kmer_size>();
    for (std::size_t i = 0u; i < bp.second; ++i) {
        ::bioseq::common::templating::append_unique_value(raw, raw_n, bp.first[i]);
    }
}

/// Pack-expanded union over `kmer_size_buckets` indices.  Threads the
/// accumulator state through `merge_breakpoints_into` (no `[&]` capture)
/// and finishes with the lifted insertion-sort primitive — the
/// dispatcher's smallest-ge lookup needs sorted input.
template <std::size_t... p_indices>
[[nodiscard]] consteval auto compute_global_l_buckets_impl(
    std::index_sequence<p_indices...>) noexcept
{
    raw_buf raw{};
    std::size_t raw_n = 0u;
    (merge_breakpoints_into<kmer_size_buckets[p_indices]>(raw, raw_n), ...);
    ::bioseq::common::templating::sort_in_place_ascending(raw, raw_n);
    return std::pair{raw, raw_n};
}

[[nodiscard]] consteval auto compute_global_l_buckets() noexcept {
    return compute_global_l_buckets_impl(
        std::make_index_sequence<kmer_size_buckets.size()>{});
}

} // namespace detail

/// Materialised dispatcher p_bucket_width bucket list — the union of per-p_kmer_size breakpoints
/// computed at compile time.  The `_n` constant gives the live prefix size;
/// the array itself is sized 32 for headroom.
inline constexpr auto seq_len_log2_buckets_pair = detail::compute_global_l_buckets();
inline constexpr auto seq_len_log2_buckets      = seq_len_log2_buckets_pair.first;
inline constexpr std::size_t seq_len_log2_buckets_n =
    seq_len_log2_buckets_pair.second;
static_assert(seq_len_log2_buckets_n >= 1u,
    "seq_len_log2_buckets must be non-empty");

namespace detail {

/// Materialise `seq_len_log2_buckets` as a `value_sequence` the chain can
/// pack-expand.  Used as the value-list of `seq_len_log2_registry`.
template <std::size_t... p_indices>
[[nodiscard]] consteval auto make_seq_len_log2_buckets_seq(
    std::index_sequence<p_indices...>) noexcept
{
    return value_sequence<std::size_t, seq_len_log2_buckets[p_indices]...>{};
}

using seq_len_log2_buckets_seq = decltype(
    make_seq_len_log2_buckets_seq(
        std::make_index_sequence<seq_len_log2_buckets_n>{}));

/// Materialise `kmer_size_buckets` similarly.  Used as the value-list of
/// `kmer_size_registry`.
template <std::size_t... p_indices>
[[nodiscard]] consteval auto make_kmer_size_buckets_seq(
    std::index_sequence<p_indices...>) noexcept
{
    return value_sequence<std::size_t, kmer_size_buckets[p_indices]...>{};
}

using kmer_size_buckets_seq = decltype(
    make_kmer_size_buckets_seq(
        std::make_index_sequence<kmer_size_buckets.size()>{}));

// Per-p_kmer-size sequence-length breakpoint sequence.
//
// Materialise the p_kmer_size-specific breakpoint list (from `compute_breakpoints_for<p_kmer_size>()`)
// as a `value_sequence<std::size_t, ...>`.  Used by `dependent_bucket_step`'s
// p_kmer_size-aware `value_list_for<p_kmer_size>` to iterate only the p_bucket_width breakpoints relevant for
// that p_kmer_size — rather than the global union shape `seq_len_log2_buckets_seq` (which
// causes the explosion).
template <std::size_t p_kmer_size, std::size_t... p_indices>
[[nodiscard]] consteval auto make_per_k_breakpoints_seq_impl(
    std::index_sequence<p_indices...>) noexcept
{
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
static_assert(per_k_breakpoints_seq<7u>::size()  == compute_breakpoints_for<7u>().second);
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
// / — Low-level enum/value dispatch primitives
// =============================================================================
//
// The dispatch primitives live in `bioseq.common.templating` and are
// re-exported here for unqualified pipeline call sites.

using ::bioseq::common::templating::dispatch_exact_value_or_default;
using ::bioseq::common::templating::dispatch_exact_value_or_default_void;
using ::bioseq::common::templating::dispatch_smallest_not_less;

// =============================================================================
// / — algorithm_enum_values registry + per-dim selectors
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

template <class p_enum>
struct algorithm_enum_values;

template <>
struct algorithm_enum_values<::bioseq::simd::chaining_algorithm> {
    using target_t = ::bioseq::simd::chaining_algorithm;
    static constexpr target_t default_value = target_t::scalar_minimap2;
    using value_list = value_sequence<target_t,
        target_t::scalar_minimap2,
        target_t::simd_minimap2,
        target_t::range_minimum_query,
        target_t::vulkan_tiled_dynamic_programming>;

    template <class... p_arguments>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::minimap2_parameters& p,
        p_arguments&&...) noexcept
    {
        return p.chaining_algorithm;
    }
};

template <>
struct algorithm_enum_values<::bioseq::simd::base_alignment_algorithm> {
    using target_t = ::bioseq::simd::base_alignment_algorithm;
    static constexpr target_t default_value = target_t::block_aligner;
    using value_list = value_sequence<target_t,
        target_t::ksw2,
        target_t::block_aligner,
        target_t::ksw2_batched>;

    template <class... p_arguments>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::minimap2_parameters& p,
        p_arguments&&...) noexcept
    {
        return p.base_alignment_algorithm;
    }
};

template <>
struct algorithm_enum_values<::bioseq::simd::minimizer_algorithm> {
    using target_t = ::bioseq::simd::minimizer_algorithm;
    static constexpr target_t default_value = target_t::scalar;
    using value_list = value_sequence<target_t,
        target_t::scalar,
        target_t::simd_minimap2_reference,
        target_t::two_stack_nthash,
        target_t::vulkan_minimap_integer,
        target_t::bounded_nthash_simd>;

    template <class... p_arguments>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::minimap2_parameters& p,
        p_arguments&&...) noexcept
    {
        return p.minimizer_algorithm;
    }
};

namespace detail {

template <class p_enum, p_enum p_value>
[[nodiscard]] consteval bool trait_specialised_for() noexcept {
    return requires { typename algorithm_traits<p_value>::kernel_type; };
}

template <class p_enum, p_enum... p_values>
[[nodiscard]] consteval bool all_traits_specialised_for(
    value_sequence<p_enum, p_values...>) noexcept
{
    return (trait_specialised_for<p_enum, p_values>() && ...);
}

} // namespace detail

static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::chaining_algorithm>::value_list{}),
    "algorithm_enum_values<chaining_algorithm>::value_list contains a value "
    "without an algorithm_traits<>::kernel_type specialisation.  Add the trait "
    "specialisation alongside the enum value.");
static_assert(detail::all_traits_specialised_for(
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
concept has_bp_size = requires (const p_type& x) {
    { x.bp_size() } -> std::convertible_to<std::size_t>;
};

struct seq_len_log2_registry {
    using target_t = std::size_t;
    static constexpr std::size_t default_value = seq_len_log2_buckets[0];
    using value_list = seq_len_log2_buckets_seq;

    template <has_bp_size p_query_range, has_bp_size p_target_range, class... p_remaining_stages>
    [[nodiscard, gnu::always_inline]] static std::size_t read_field(
        const ::bioseq::simd::minimap2_parameters& params,
        const p_query_range& q, const p_target_range& t,
        p_remaining_stages&&...) noexcept
    {
        const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
        return round_up_seq_len_log2(
            max_bp, static_cast<std::size_t>(params.kmer_size));
    }
};

struct kmer_size_registry {
    using target_t = std::size_t;
    static constexpr std::size_t default_value = kmer_size_default;
    using value_list = kmer_size_buckets_seq;

    template <class... p_arguments>
    [[nodiscard, gnu::always_inline]] static constexpr std::size_t read_field(
        const ::bioseq::simd::minimap2_parameters& params,
        p_arguments&&...) noexcept
    {
        return static_cast<std::size_t>(params.kmer_size);
    }
};

/// Pinned-aware sequence-length registry.
///
/// Exposes `value_list_for<p_kmer_size, …>` and `read_field_for<p_kmer_size, …>` so the chain's
/// `dependent_bucket_step` can iterate only the p_bucket_width breakpoints for the previously-pinned p_kmer_size.
/// This collapses the 21-cell (p_bucket_width, p_kmer_size) Cartesian product (the global p_bucket_width union × p_kmer_size) into
/// the per-p_kmer_size breakpoint count: `3 + 4 + 2 = 9` cells (p_kmer_size=15: 3 buckets, p_kmer_size=7: 4, p_kmer_size=27: 2).
///
/// Place this step *after* `kmer_size_step` in the chain so `Pinned... = (p_kmer_size, …)`.
struct seq_len_log2_registry_per_k {
    using target_t = std::size_t;

    /// Anchored to p_kmer_size=15's lower bucket boundary (`compute_breakpoints_for<15>()[0]`).
    /// The chain only ever pins `default_value` when `active_v` is false; today every
    /// `dependent_bucket_step` is unconditionally active so this is dead code, but the
    /// chain primitive requires a value to thread through inactive steps.
    static constexpr std::size_t default_value = 15u;

    template <std::size_t p_kmer_size, auto... p_rest_pinned>
    using value_list_for = per_k_breakpoints_seq<p_kmer_size>;

    template <std::size_t p_pinned_kmer_size,
              has_bp_size p_query_range,
              has_bp_size p_target_range,
              class... p_remaining_stages>
    [[nodiscard, gnu::always_inline]] static std::size_t read_field_for(
        const ::bioseq::simd::minimap2_parameters& params,
        const p_query_range& q, const p_target_range& t,
        p_remaining_stages&&...) noexcept
    {
        const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
        return round_up_seq_len_log2(
            max_bp, static_cast<std::size_t>(params.kmer_size));
    }
};

} // namespace detail

// =============================================================================
// / / — dispatch_chain primitive and step adapters
// =============================================================================
//
// The chain primitive and step adapters live in
// `bioseq.common.templating::dispatch_chain.cppm`.
// They are re-exported here so the pipeline body keeps unqualified
// names. The per-pipeline alias supplies the `minimap2_parameters` slot so
// chain call sites do not need to repeat it.

namespace detail {

using ::bioseq::common::templating::enum_step;
using ::bioseq::common::templating::bucket_step;
using ::bioseq::common::templating::dependent_bucket_step;
using ::bioseq::common::templating::guarded_step;

using baa_step = enum_step<algorithm_enum_values<
    ::bioseq::simd::base_alignment_algorithm>>;
using ca_step  = enum_step<algorithm_enum_values<
    ::bioseq::simd::chaining_algorithm>>;
using ma_step  = enum_step<algorithm_enum_values<
    ::bioseq::simd::minimizer_algorithm>>;
using kmer_size_step    = enum_step<kmer_size_registry>;
using seq_len_log2_step = bucket_step<seq_len_log2_registry>;

/// The per-p_kmer_size seq_len_log2 step must follow `kmer_size_step`
/// in the chain so the pinned-p_kmer_size's per-p_kmer_size breakpoint list is iterated.
using seq_len_log2_step_per_k = dependent_bucket_step<seq_len_log2_registry_per_k>;

/// Per-pipeline chain alias for the request parameter type used here.
/// `p_result` and the leaf template stay as template
/// parameters because real call sites instantiate with different
/// return types (match_result_collection / pipeline_description / void).
template <class p_result,
          template <auto... p_values> class p_leaf,
          class... p_steps>
using dispatch_chain = ::bioseq::common::templating::dispatch_chain<
    p_result, p_leaf, ::bioseq::simd::minimap2_parameters, p_steps...>;

} // namespace detail

// =============================================================================
// — p_leaf types and traits aliases
// =============================================================================
//
// `pipeline_run_leaf` is the
// namespace-scope class template that bottoms out the runtime cascade.
// The specialized/fallback parameter branch lives inside the leaf body.
//
// `pipeline_run_traits::leaf` is the
// alias-template wrapper exposing the leaf as an NTTP-only `template
// <auto...> class` view — the shape `dispatch_chain` requires for its
// `template <auto...> class p_leaf` template-template parameter.

namespace detail {

template <pipeline_entry_point p_entry_point,
          sequence_alphabet p_alphabet,
          class p_minimizer, class p_index,
          std::size_t p_bucket_width, std::size_t p_kmer_size,
          ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
          ::bioseq::simd::chaining_algorithm      p_chaining_algorithm,
          ::bioseq::simd::minimizer_algorithm     p_minimizer_algorithm>
struct pipeline_run_leaf {
    static_assert(std::is_same_v<decltype(p_bucket_width), std::size_t>,
        "pipeline_run_leaf: p_bucket_width must be std::size_t (comes from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(p_kmer_size), std::size_t>,
        "pipeline_run_leaf: p_kmer_size must be std::size_t (comes from kmer_size_step).");
    static_assert(std::is_same_v<decltype(p_alignment_algorithm),
        ::bioseq::simd::base_alignment_algorithm>,
        "pipeline_run_leaf: p_alignment_algorithm must be base_alignment_algorithm (from baa_step).");
    static_assert(std::is_same_v<decltype(p_chaining_algorithm),
        ::bioseq::simd::chaining_algorithm>,
        "pipeline_run_leaf: p_chaining_algorithm must be chaining_algorithm (from ca_step).");
    static_assert(std::is_same_v<decltype(p_minimizer_algorithm),
        ::bioseq::simd::minimizer_algorithm>,
        "pipeline_run_leaf: p_minimizer_algorithm must be minimizer_algorithm (from ma_step).");

    template <class p_query_minimizers>
    BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR
    static match_result_collection call(
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
        bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
        const p_query_minimizers&            query_minimizers,
        const p_index&                     target_index,
        bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
        bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
        bool allow_gpu_target_index_build,
        transient_target_index_release_authorization
            transient_target_index_owner = {})
    {
        using specialized_parameters     = static_aligner_parameters<10u, p_kmer_size, p_bucket_width>;
        #if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS)
        using fallback_parameters = fallback_aligner_parameters<63>;
        #else
        using fallback_parameters = fallback_aligner_parameters<p_bucket_width>;
        #endif

        #if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION)
        // Both parameter specializations are available; the runtime condition
        // selects the matching specialization.
        if (params.window_size == 10
            && params.kmer_size == static_cast<int32_t>(p_kmer_size))
        {
            return pipeline_run_impl<specialized_parameters, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point, p_alphabet,
                                     p_minimizer, p_index, p_query_minimizers>(
                query_seq, target_seq, query_minimizers, target_index, params,
                gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build,
                transient_target_index_owner);
        }
        #endif
        return pipeline_run_impl<fallback_parameters, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point, p_alphabet,
                                 p_minimizer, p_index, p_query_minimizers>(
            query_seq, target_seq, query_minimizers, target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build,
            transient_target_index_owner);
    }
};

template <pipeline_entry_point p_entry_point,
          sequence_alphabet p_alphabet,
          class p_minimizer, class p_index>
struct pipeline_run_traits {
    template <auto p_kmer_size, auto p_bucket_width, auto p_alignment_algorithm, auto p_chaining_algorithm, auto p_minimizer_algorithm>
    using leaf = pipeline_run_leaf<
        p_entry_point, p_alphabet, p_minimizer, p_index, p_bucket_width, p_kmer_size, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm>;
};

/// Common leaf body shared by specialized and fallback parameter branches in
/// `describe_leaf::call`. Declared before
/// `describe_leaf` so its unqualified call inside `describe_leaf::call`
/// resolves at the point of template definition.
template <is_constexpr_aligner_params p_aligner_parameters,
          ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
          ::bioseq::simd::chaining_algorithm      p_chaining_algorithm,
          ::bioseq::simd::minimizer_algorithm     p_minimizer_algorithm,
          pipeline_entry_point                                                p_entry_point,
          sequence_alphabet p_alphabet>
[[nodiscard]] pipeline_description describe_pipeline_leaf(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const ::bioseq::simd::minimap2_parameters& params,
    sequence_size_t query_len, sequence_size_t target_len,
    const query_sketch_route query_sketch_route)
{
    using p_minimizer = typename p_aligner_parameters::minimizer_repr_t;
    const std::vector<p_minimizer> empty_minimizers{};
    minimizer_hashmap_fragment_index empty_index{};
    match_result_collection         ignored_out{};
    const auto& sm = thread_local_scoring_matrix<p_alphabet>(params);
    /* Description construction inspects selected stage traits and frozen
     * parameters; it never executes a kernel. Keep the runtime Highway table
     * out of this cold metadata path so plain EXPLAIN can describe an unbound
     * GUC snapshot before BatchProcessor performs the one allowed dispatch. */
    const ::bioseq::simd::alignment_kernels<p_alphabet>
        description_alignment_kernels{};

    auto pipeline =
        make_minimap_pipeline<p_aligner_parameters, p_alignment_algorithm, p_chaining_algorithm, p_minimizer_algorithm, p_entry_point, p_alphabet, p_minimizer,
                              minimizer_hashmap_fragment_index>(
        query_seq, target_seq, empty_minimizers, empty_index, sm,
        description_alignment_kernels, ignored_out, params,
        query_len, target_len);
    auto description = describe_with_params<p_aligner_parameters>(pipeline);
    if (complete_all_gpu_tuple_selected(params)) {
        /* The scheduler admits this exact tuple only to the dedicated
         * resident GPU route and returns before constructing the generic
         * CPU/simd pipeline. In particular, bounded CIGAR is produced by the
         * Vulkan extension, not by the selected block_aligner trait. Keep the
         * generic stage skeleton for EXPLAIN, but do not advertise any of its
         * bypassed Highway entry points as planned work. */
        for (auto& stage : description.stages) {
            if (stage.algorithm.has_value()) {
                stage.algorithm->highway_kernels.reset();
            }
        }
    }
    auto query_sketch = describe_query_sketch_stage(
        params, query_sketch_route);
    if (!query_sketch.has_value()) {
        throw std::invalid_argument{
            "query-sketch description received an unsupported algorithm or "
            "alphabet code"};
    }
    description.stages.insert(
        description.stages.begin(), std::move(*query_sketch));
    return description;
}

// Describe-side leaf — p_entry_point/p_minimizer_algorithm remain pinned so the structural stage skeleton
// does not multiply describe instantiations.  The exact query-minimizer
// Highway function is resolved neutrally from the frozen runtime enum and
// attached to the plan-only QuerySketchStage above.
template <sequence_alphabet p_alphabet,
          std::size_t p_bucket_width, std::size_t p_kmer_size,
          ::bioseq::simd::base_alignment_algorithm p_alignment_algorithm,
          ::bioseq::simd::chaining_algorithm      p_chaining_algorithm>
struct describe_leaf {
    static_assert(std::is_same_v<decltype(p_bucket_width), std::size_t>,
        "describe_leaf: p_bucket_width must be std::size_t (from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(p_kmer_size), std::size_t>,
        "describe_leaf: p_kmer_size must be std::size_t (from kmer_size_step).");
    static_assert(std::is_same_v<decltype(p_alignment_algorithm),
        ::bioseq::simd::base_alignment_algorithm>,
        "describe_leaf: p_alignment_algorithm must be base_alignment_algorithm (from baa_step).");
    static_assert(std::is_same_v<decltype(p_chaining_algorithm),
        ::bioseq::simd::chaining_algorithm>,
        "describe_leaf: p_chaining_algorithm must be chaining_algorithm (from ca_step).");

    BIOSEQ_PIPELINE_NODISCARD_INLINE_ATTR
    static pipeline_description call(
        const ::bioseq::simd::minimap2_parameters& params,
        bioseq::storage::lazy_sequence_view<p_alphabet> q,
        bioseq::storage::lazy_sequence_view<p_alphabet> t,
        const int32_t& query_len,
        const int32_t& target_len,
        const query_sketch_route& query_sketch_route)
    {
        using specialized_parameters     = static_aligner_parameters<10u, p_kmer_size, p_bucket_width>;
        #if !defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ADAPTIVE_LENGTHS)
        using fallback_parameters = fallback_aligner_parameters<63>;
        #else
        using fallback_parameters = fallback_aligner_parameters<p_bucket_width>;
        #endif

        constexpr auto describe_entry_point =
            pipeline_entry_point::with_prebuilt_minimizers;
        constexpr auto describe_minimizer_algorithm = algorithm_enum_values<
            ::bioseq::simd::minimizer_algorithm>::default_value;
        static_assert(describe_minimizer_algorithm ==
            ::bioseq::simd::minimizer_algorithm::scalar,
            "describe_leaf relies on p_minimizer_algorithm default = scalar.  Update both call "
            "sites if the p_minimizer_algorithm default ever changes.");

        #if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_FULL_SPECIALIZATION)
        if (params.window_size == 10
            && params.kmer_size == static_cast<int32_t>(p_kmer_size))
        {
            return describe_pipeline_leaf<specialized_parameters, p_alignment_algorithm, p_chaining_algorithm,
                describe_minimizer_algorithm, describe_entry_point, p_alphabet>(
                    q, t, params, query_len, target_len,
                    query_sketch_route);
        }
        #endif
        return describe_pipeline_leaf<fallback_parameters, p_alignment_algorithm, p_chaining_algorithm,
            describe_minimizer_algorithm, describe_entry_point, p_alphabet>(
                q, t, params, query_len, target_len,
                query_sketch_route);
    }
};

template <sequence_alphabet p_alphabet>
struct describe_traits {
    // The chain pins p_kmer_size before p_bucket_width.
    template <auto p_kmer_size, auto p_bucket_width, auto p_alignment_algorithm, auto p_chaining_algorithm>
    using leaf = describe_leaf<p_alphabet, p_bucket_width, p_kmer_size, p_alignment_algorithm, p_chaining_algorithm>;
};

} // namespace detail

// =============================================================================
// / — top-level runtime cascade entry
// =============================================================================
//
// The runtime cascade is one `dispatch_chain<...>::run<>(...)` over the
// bucket-width, k-mer-size, alignment, chaining, and minimizer dimensions.

template <pipeline_entry_point p_entry_point,
          sequence_alphabet p_alphabet,
          class p_minimizer, class p_index,
          class p_query_minimizers = std::vector<p_minimizer>>
BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR
match_result_collection dispatch_pipeline(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const p_query_minimizers&            query_minimizers,
    const p_index&                     target_index,
    const ::bioseq::simd::minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false,
    transient_target_index_release_authorization
        transient_target_index_owner = {})
{
    using traits_type = detail::pipeline_run_traits<p_entry_point, p_alphabet, p_minimizer, p_index>;
    // Pin p_kmer_size before p_bucket_width so only relevant breakpoints run.
    return detail::dispatch_chain<
        match_result_collection,
        traits_type::template leaf,
        detail::kmer_size_step,
        detail::seq_len_log2_step_per_k,
        detail::baa_step,
        detail::ca_step,
        detail::guarded_step<detail::ma_step,
            /*active=*/(p_entry_point != pipeline_entry_point::with_prebuilt_minimizers)>
    >::template run<>(
        params, query_seq, target_seq, query_minimizers, target_index,
        gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build,
        transient_target_index_owner);
}

// =============================================================================
// — describe_pipeline_for (EXPLAIN path)
// =============================================================================
//
// One chain instantiation over four steps (p_bucket_width, p_kmer_size, p_alignment_algorithm, p_chaining_algorithm). p_entry_point and p_minimizer_algorithm are
// pinned constants inside `describe_leaf`; algorithm-specific query-sketch
// metadata is added through the neutral enum-to-kernel mapping above.

template <sequence_alphabet p_alphabet>
BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR
pipeline_description describe_pipeline_for(
    const ::bioseq::simd::minimap2_parameters& params,
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq_proto,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq_proto,
    const query_sketch_route query_sketch_route =
        query_sketch_route::recompute_capable)
{
    const sequence_size_t query_len  = query_seq_proto.bp_size();
    const sequence_size_t target_len = target_seq_proto.bp_size();

    using traits_type = detail::describe_traits<p_alphabet>;
    // Pin p_kmer_size before the dependent p_bucket_width step.
    return detail::dispatch_chain<
        pipeline_description,
        traits_type::template leaf,
        detail::kmer_size_step,
        detail::seq_len_log2_step_per_k,
        detail::baa_step,
        detail::ca_step
    >::template run<>(
        params, query_seq_proto, target_seq_proto, query_len, target_len,
        query_sketch_route);
}

/**
 * Cold typed EXPLAIN entry. The owning metadata registry selects `p_alphabet`
 * and supplies its runtime code together, so this layer neither redispatches
 * nor maintains a second type/code map.
 */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline pipeline_description describe_pipeline_for(
    const ::bioseq::simd::minimap2_parameters& params,
    const std::uint8_t alphabet_code,
    const query_sketch_route query_sketch_route =
        query_sketch_route::recompute_capable)
{
    if (params.highway_kernels != nullptr) {
        const auto* identity = params.highway_kernels->is_complete()
            ? params.highway_kernels
                ->template resolve_alphabet_identity<p_alphabet>()
            : nullptr;
        if (identity == nullptr || identity->alphabet_code != alphabet_code
            || params.highway_kernels->resolve_alphabet_identity(
                   alphabet_code) != identity) {
            throw std::invalid_argument{
                "pipeline description received mismatched alphabet identity"};
        }
    }
    using sequence_type = sequence_custom_alphabet<p_alphabet, std::uint64_t>;
    compact_sequence_range<sequence_type> empty_query_proto{};
    compact_sequence_range<sequence_type> empty_target_proto{};
    auto empty_query_owner =
        bioseq::storage::make_resident_lazy_sequence(empty_query_proto);
    auto empty_target_owner =
        bioseq::storage::make_resident_lazy_sequence(empty_target_proto);
    auto typed_params = params;
    typed_params.alphabet_code = alphabet_code;
    return describe_pipeline_for(
        typed_params,
        bioseq::storage::make_lazy_sequence_view(empty_query_owner),
        bioseq::storage::make_lazy_sequence_view(empty_target_owner),
        query_sketch_route);
}

} // namespace bioseq::simd::pipe

// =============================================================================
// Public scheduler-facing entry points are target-neutral and receive
// the already-selected Highway table through minimap2_parameters.
// =============================================================================

namespace bioseq::simd {

// — relocated index name in scope for this namespace's entry points.
using bioseq::indexing::minimizer_hashmap_fragment_index;
using bioseq::indexing::minimizer_lookup_index;
using bioseq::indexing::minimizer_seed_index;

namespace pipe_entry_detail {

// Restrict fixed-allocation adoption to mmap-scale targets.  Within this route,
// a sparse result whose allocator pages were already resident retains its exact
// permit; query and ordinary small-target sketches keep the collector route.
inline constexpr std::uint64_t
    k_vulkan_fixed_host_adoption_target_minimum_bp = std::uint64_t{256} << 20u;

/// Select the fixed host-ownership transaction only for a large target sketch
/// whose complete execution tuple is accepted by the source-direct device ABI.
/// Query sketching and every incompatible target intentionally remain on the
/// established collector-compatible Vulkan route.
template <class p_minimizer>
[[nodiscard]] constexpr bool
fixed_vulkan_target_host_adoption_route_selected(
        bool is_gpu_target_index_build,
        const minimap2_parameters& params,
        std::uint32_t alphabet_size,
        std::uint64_t target_bp) noexcept {
    return !is_gpu_target_index_build
        && params.vulkan_enabled
        && params.vulkan_seed_hits_enabled
        && !params.vulkan_fused_frontend_enabled
        && !params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm
            == minimizer_algorithm::vulkan_minimap_integer
        && bioseq::indexing::gpu_source_direct_tuple_supported<p_minimizer>(
            alphabet_size,
            params.kmer_size,
            params.window_size)
        && target_bp >= k_vulkan_fixed_host_adoption_target_minimum_bp;
}

/// Accounting receipt carried with the fixed Vulkan target host allocation. The host
/// allocation has exactly one accounting authority: either its fully-touched
/// bytes were reconciled into process footprint and the permit was released,
/// or an exact permit remains attached to the allocation.  Empty output is the
/// footprint-authoritative zero-byte case.
template <class p_minimizer>
struct vulkan_sketch_host_adoption_receipt {
    const p_minimizer* source_data = nullptr;
    const p_minimizer* adopted_data = nullptr;
    std::size_t adopted_size = 0u;
    std::size_t adopted_capacity = 0u;
    std::size_t source_capacity_after_adoption = 0u;
    std::uint64_t host_capacity_bytes = 0u;
    std::uint64_t retained_host_permit_bytes = 0u;
    std::uint64_t host_footprint_reflected_delta_bytes = 0u;
    std::uint64_t device_allocation_bytes_released = 0u;
    bool is_host_permit_released_to_process_footprint = false;
    bool device_owner_available_before_adoption = false;
    bool device_owner_unique_before_adoption = false;
    bool device_released_before_return = false;
    bool arena_trimmed_before_adoption = false;
    bioseq::gpu::vulkan::vulkan_sketch_dispatch_token dispatch_token{};

    [[nodiscard]] bool is_zero_copy_adoption_valid() const noexcept {
        const std::uint64_t expected_capacity_bytes =
            bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(adopted_capacity),
                static_cast<std::uint64_t>(sizeof(p_minimizer)));
        const bool is_allocation_identity_accepted = adopted_size == 0u
            ? adopted_capacity == 0u && host_capacity_bytes == 0u
            : source_data == adopted_data
                && adopted_capacity == adopted_size
                && host_capacity_bytes == expected_capacity_bytes;
        const bool footprint_authority =
            is_host_permit_released_to_process_footprint
            && retained_host_permit_bytes == 0u
            && (host_capacity_bytes == 0u
                || host_footprint_reflected_delta_bytes
                    >= host_capacity_bytes);
        const bool retained_permit_authority =
            !is_host_permit_released_to_process_footprint
            && host_capacity_bytes != 0u
            && retained_host_permit_bytes == host_capacity_bytes;
        return is_allocation_identity_accepted
            && expected_capacity_bytes
                != (std::numeric_limits<std::uint64_t>::max)()
            && source_capacity_after_adoption == 0u
            && footprint_authority != retained_permit_authority
            && device_owner_available_before_adoption
            && device_owner_unique_before_adoption
            && device_released_before_return
            && arena_trimmed_before_adoption
            && dispatch_token.is_internally_consistent();
    }
};

/// Return-by-value lifetime owner for an adopted fixed Vulkan target sketch.
/// The permit is declared before the vector so normal reverse destruction frees
/// the allocation before returning its reservation.  Move assignment is
/// intentionally unavailable: memberwise assignment would release an existing
/// destination permit before freeing its destination vector.
template <class p_minimizer>
struct fixed_vulkan_target_host_owner {
    std::optional<bioseq::parallelism::memory_permit> host_permit;
    std::vector<p_minimizer> minimizers;
    vulkan_sketch_host_adoption_receipt<p_minimizer> adoption{};

    fixed_vulkan_target_host_owner() = default;
    fixed_vulkan_target_host_owner(const fixed_vulkan_target_host_owner&) = delete;
    fixed_vulkan_target_host_owner& operator=(
        const fixed_vulkan_target_host_owner&) = delete;
    fixed_vulkan_target_host_owner& operator=(
        fixed_vulkan_target_host_owner&&) = delete;

    fixed_vulkan_target_host_owner(
            fixed_vulkan_target_host_owner&& other) noexcept {
        // Swap the allocation while the source permit is still live, then move
        // its authority.  The destination is newly constructed and empty.
        minimizers.swap(other.minimizers);
        adoption = other.adoption;
        other.adoption = {};
        if (other.host_permit.has_value()) {
            host_permit.emplace(std::move(*other.host_permit));
        }
        other.host_permit.reset();
    }

    [[nodiscard]] bool has_valid_ownership_authorization() const noexcept {
        const bool valid_permit_available = host_permit.has_value()
            && static_cast<bool>(*host_permit);
        const bool footprint_authority =
            adoption.is_host_permit_released_to_process_footprint
            && !host_permit.has_value();
        const bool retained_permit_authority =
            !adoption.is_host_permit_released_to_process_footprint
            && valid_permit_available
            && host_permit->bytes() == adoption.host_capacity_bytes
            && host_permit->bytes()
                == adoption.retained_host_permit_bytes;
        const bool allocation_matches_receipt =
            minimizers.size() == adoption.adopted_size
            && minimizers.capacity() == adoption.adopted_capacity
            && (minimizers.empty()
                || minimizers.data() == adoption.adopted_data);
        return adoption.is_zero_copy_adoption_valid()
            && allocation_matches_receipt
            && footprint_authority != retained_permit_authority;
    }

    void release_consumed_host_permit() {
        if (minimizers.capacity() != 0u) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "fixed Vulkan host permit release preceded vector "
                "deallocation"};
        }
        host_permit.reset();
    }
};

template <class p_minimizer>
void release_fixed_vulkan_source_after_resident_publication(
    fixed_vulkan_target_host_owner<p_minimizer>& source,
    pipe::direct_resident_operation_stats& ledger,
    bool resident_published)
{
    const std::uint64_t released_bytes =
        bioseq::common::saturating_mul(
            source.minimizers.capacity(), sizeof(p_minimizer));
    {
        std::vector<p_minimizer> empty;
        source.minimizers.swap(empty);
    }
    if (source.host_permit.has_value()) {
        if (source.minimizers.capacity() != 0u) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct host permit release preceded source storage"};
        }
        source.host_permit.reset();
    }
    source.adoption = {};
    ledger.source_released_bytes = released_bytes;
    ledger.source_releases = released_bytes != 0u ? 1u : 0u;
    ledger.source_release_completed = true;
    ledger.source_released_after_publication = resident_published;
}

/// Exact accounting returned by the fixed Vulkan host-vector -> CPU index
/// handoff.  Keeping the transaction in one helper lets tests exercise the
/// same reserve/build/validate path used by `minimap2_port` instead of merely
/// reproducing its arithmetic.
struct vulkan_target_index_handoff_receipt {
    std::uint64_t admitted_position_headroom_bytes = 0u;
    std::uint64_t admitted_table_headroom_bytes = 0u;
    std::uint64_t admitted_build_bytes = 0u;
    std::uint64_t retained_position_bytes = 0u;
    std::uint64_t retained_table_bytes = 0u;
    std::uint64_t retained_index_bytes = 0u;
    std::uint64_t retained_index_permit_bytes = 0u;
    std::uint64_t transferred_position_footprint_bytes = 0u;
    std::uint64_t source_host_capacity_bytes_before_build = 0u;
    std::uint64_t source_footprint_reflected_bytes_before_build = 0u;
    std::uint64_t source_minimizer_count_before_build = 0u;
    std::uint64_t retained_position_count = 0u;
    std::uint64_t retained_position_initialized_bytes = 0u;
    std::uint64_t source_host_permit_bytes_before_build = 0u;
    std::uint64_t reserved_bytes_before_index_admission = 0u;
    std::uint64_t reserved_bytes_after_index_admission = 0u;
    bool source_host_permit_retained_during_build = false;
    bool source_footprint_authority_transferred_to_positions = false;
    bool every_retained_position_initialized = false;
    bool source_capacity_released = false;
    bool source_host_permit_cleared_after_consumption = false;
    bool source_adoption_cleared_after_consumption = false;

    [[nodiscard]] bool is_valid() const noexcept {
        return source_capacity_released
            && source_host_permit_cleared_after_consumption
            && source_adoption_cleared_after_consumption
            && retained_position_bytes
                <= admitted_position_headroom_bytes
            && retained_table_bytes <= admitted_table_headroom_bytes
            && retained_index_bytes
                == bioseq::common::saturating_add(
                    retained_position_bytes, retained_table_bytes)
            && retained_index_bytes
                == bioseq::common::saturating_add(
                    retained_index_permit_bytes,
                    transferred_position_footprint_bytes)
            && transferred_position_footprint_bytes
                <= retained_position_bytes
            && (transferred_position_footprint_bytes == 0u
                || transferred_position_footprint_bytes
                    == retained_position_bytes)
            && every_retained_position_initialized
            && retained_position_count
                == source_minimizer_count_before_build
            && retained_position_initialized_bytes
                == retained_position_bytes
            && retained_position_initialized_bytes
                == bioseq::common::saturating_mul(
                    retained_position_count,
                    static_cast<std::uint64_t>(sizeof(std::uint64_t)))
            && (!source_footprint_authority_transferred_to_positions
                ? transferred_position_footprint_bytes == 0u
                : transferred_position_footprint_bytes != 0u
                    && !source_host_permit_retained_during_build
                    && source_host_permit_bytes_before_build == 0u
                    && source_host_capacity_bytes_before_build
                        == transferred_position_footprint_bytes
                    && source_footprint_reflected_bytes_before_build
                        >= source_host_capacity_bytes_before_build);
    }
};

/// Owning return value for the strict fixed-Vulkan target-index handoff.  The
/// index permit precedes the index so reverse destruction releases table and
/// position storage first.  Move assignment is deleted to rule out releasing a
/// destination permit before its previous index storage is deallocated.
struct vulkan_admitted_target_index {
    std::optional<bioseq::parallelism::memory_permit> index_permit;
    minimizer_hashmap_fragment_index index;
    vulkan_target_index_handoff_receipt receipt{};

    vulkan_admitted_target_index() = default;
    vulkan_admitted_target_index(const vulkan_admitted_target_index&) = delete;
    vulkan_admitted_target_index& operator=(
        const vulkan_admitted_target_index&) = delete;
    vulkan_admitted_target_index& operator=(vulkan_admitted_target_index&&) = delete;

    vulkan_admitted_target_index(vulkan_admitted_target_index&& other) noexcept
        : index(std::move(other.index)),
          receipt(other.receipt) {
        // Move storage while the source reservation remains authoritative,
        // then transfer the reservation into this new empty owner.
        if (other.index_permit.has_value()) {
            index_permit.emplace(std::move(*other.index_permit));
        }
        other.index_permit.reset();
        other.receipt = {};
    }

    [[nodiscard]] bool is_valid() const noexcept {
        return receipt.is_valid()
            && index_permit.has_value()
            && static_cast<bool>(*index_permit)
            && index_permit->bytes()
                == receipt.retained_index_permit_bytes;
    }

    void release_storage_before_permit() noexcept {
        index.release_storage();
        index_permit.reset();
    }
};

template <class p_minimizer>
[[nodiscard]] vulkan_admitted_target_index
admit_and_build_fixed_vulkan_target_index(
    fixed_vulkan_target_host_owner<p_minimizer> target_host,
    sequence_size_t target_len)
{
    if (!target_host.has_valid_ownership_authorization()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan zero-copy target-index handoff lacks host ownership "
            "authority"};
    }
    const bool source_host_permit_retained_during_build =
        target_host.host_permit.has_value();
    const bool source_footprint_authority_before_build =
        target_host.adoption.is_host_permit_released_to_process_footprint
        && !target_host.host_permit.has_value();
    const std::uint64_t source_host_capacity_bytes_before_build =
        target_host.adoption.host_capacity_bytes;
    const std::uint64_t source_footprint_reflected_bytes_before_build =
        target_host.adoption.host_footprint_reflected_delta_bytes;
    const std::uint64_t source_minimizer_count_before_build =
        static_cast<std::uint64_t>(target_host.minimizers.size());
    const std::uint64_t source_host_permit_bytes_before_build =
        source_host_permit_retained_during_build
            ? target_host.host_permit->bytes()
            : 0u;
    const std::uint64_t position_payload_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(target_host.minimizers.size()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    // Empty sketches retain neither positions nor a table.  Do not turn the
    // generic allocator slop allowance into a fictitious 128 KiB reservation.
    const std::uint64_t admitted_position_headroom_bytes =
        position_payload_bytes == 0u
            ? 0u
            : bioseq::gpu::vulkan::
                vulkan_exact_host_allocation_admission_bytes(
                    position_payload_bytes);
    const bioseq::indexing::minimizer_hashmap_table_allocation_plan table_plan =
        bioseq::indexing::minimizer_hashmap_table_allocation_plan(
            static_cast<std::uint64_t>(target_host.minimizers.size()));
    const std::uint64_t admitted_table_headroom_bytes = !table_plan.valid
        ? (std::numeric_limits<std::uint64_t>::max)()
        : table_plan.allocation_bytes == 0u
            ? 0u
            : bioseq::gpu::vulkan::
                vulkan_exact_host_allocation_admission_bytes(
                    table_plan.allocation_bytes);
    const std::uint64_t build_peak_bytes =
        bioseq::common::saturating_add(
            admitted_position_headroom_bytes,
            admitted_table_headroom_bytes);
    if (position_payload_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || admitted_position_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || admitted_table_headroom_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || build_peak_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan zero-copy target-index handoff has invalid "
            "capacity accounting"};
    }
    vulkan_admitted_target_index admitted_index;
    auto& admission =
        bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t reserved_bytes_before_index_admission =
        admission.reserved_bytes();
    if (source_host_permit_retained_during_build) {
        // This deliberately counts the source reservation even if some reused
        // allocator pages are visible in RSS.  The conservative overlap can
        // deny a medium sparse result, but it never over-admits the real
        // host-vector + positions + one-shot FOA-table build peak.
        bioseq_log<logging_level::debug>(
            "[vulkan sketch] strict target-index handoff keeps sparse host "
            "permit through build: host={} index_peak={}",
            source_host_permit_bytes_before_build,
            build_peak_bytes);
    }
    bioseq::parallelism::memory_admission_failure failure{};
    auto permit = admission.try_reserve(build_peak_bytes, &failure);
    if (!permit.has_value()) {
        bioseq_log<logging_level::error>(
            "[vulkan sketch] strict target-index handoff admission "
            "denied: peak={} positions={} table={} requested={} "
            "reserved={} allocatable_after_reservations={}",
            build_peak_bytes,
            admitted_position_headroom_bytes,
            admitted_table_headroom_bytes,
            failure.requested_bytes,
            failure.reserved_bytes,
            failure.allocatable_after_reservations_bytes);
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan zero-copy target-index handoff admission denied"};
    }
    admitted_index.index_permit.emplace(std::move(*permit));
    const std::uint64_t reserved_bytes_after_index_admission =
        admission.reserved_bytes();
    admitted_index.index.build_consuming_full_table(
        std::move(target_host.minimizers), target_len);
    const std::uint64_t retained_position_bytes =
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(
                admitted_index.index.positions.capacity()),
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    const std::uint64_t retained_table_bytes =
        admitted_index.index.table_allocation_bytes();
    const std::uint64_t retained_position_count =
        static_cast<std::uint64_t>(admitted_index.index.positions.size());
    const std::uint64_t retained_position_initialized_bytes =
        bioseq::common::saturating_mul(
            retained_position_count,
            static_cast<std::uint64_t>(sizeof(std::uint64_t)));
    const std::uint64_t retained_index_bytes =
        bioseq::common::saturating_add(
            retained_position_bytes, retained_table_bytes);
    if (target_host.minimizers.capacity() != 0u
        || retained_position_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || retained_position_initialized_bytes
            == (std::numeric_limits<std::uint64_t>::max)()
        || retained_position_bytes > admitted_position_headroom_bytes
        || retained_position_count != source_minimizer_count_before_build
        || admitted_index.index.table_peak_allocation_bytes()
            > admitted_table_headroom_bytes
        || retained_table_bytes != table_plan.allocation_bytes
        || retained_index_bytes
            == (std::numeric_limits<std::uint64_t>::max)()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan zero-copy target-index build exceeded admitted "
            "host capacity"};
    }
    // The source allocation is gone now and only now may its retained permit
    // be returned.  On every earlier throw the by-value host owner unwinds its
    // vector before its permit.
    target_host.release_consumed_host_permit();
    const bool source_host_permit_cleared_after_consumption =
        !target_host.host_permit.has_value();
    // Exact replacement accounting is deliberately all-or-nothing.  The
    // footprint route may transfer its one-use authority only after every
    // position is initialized and the source allocation is gone, and only
    // when the replacement has exactly the same nonzero byte extent.  Any
    // mismatch discards the old authority and retains the full index permit.
    const bool exact_position_footprint_replacement =
        source_footprint_authority_before_build
        && source_host_capacity_bytes_before_build != 0u
        && source_footprint_reflected_bytes_before_build
            >= source_host_capacity_bytes_before_build
        && retained_position_count == source_minimizer_count_before_build
        && retained_position_initialized_bytes == retained_position_bytes
        && target_host.minimizers.capacity() == 0u
        && source_host_capacity_bytes_before_build
            == retained_position_bytes;
    const std::uint64_t transferred_position_footprint_bytes =
        exact_position_footprint_replacement
            ? retained_position_bytes
            : 0u;
    const std::uint64_t retained_index_permit_bytes =
        retained_index_bytes - transferred_position_footprint_bytes;
    bioseq_log<logging_level::info>(
        "[vulkan sketch] strict target-index replacement authority: "
        "source_authority={} source_capacity={} source_reflected={} "
        "source_permit={} source_count={} position_count={} "
        "position_initialized={} position_capacity={} exact_replacement={} "
        "transferred_positions={} retained_permit={} retained_total={}",
        source_footprint_authority_before_build
            ? "process_footprint"
            : source_host_permit_retained_during_build
                ? "retained_permit"
                : "invalid",
        source_host_capacity_bytes_before_build,
        source_footprint_reflected_bytes_before_build,
        source_host_permit_bytes_before_build,
        source_minimizer_count_before_build,
        retained_position_count,
        retained_position_initialized_bytes,
        retained_position_bytes,
        exact_position_footprint_replacement,
        transferred_position_footprint_bytes,
        retained_index_permit_bytes,
        retained_index_bytes);
    target_host.adoption = {};
    const bool source_adoption_cleared_after_consumption =
        target_host.adoption.adopted_size == 0u
        && target_host.adoption.host_capacity_bytes == 0u
        && !target_host.adoption.
            is_host_permit_released_to_process_footprint;
    admitted_index.index_permit->shrink_to(retained_index_permit_bytes);
    admitted_index.receipt = vulkan_target_index_handoff_receipt{
        .admitted_position_headroom_bytes =
            admitted_position_headroom_bytes,
        .admitted_table_headroom_bytes = admitted_table_headroom_bytes,
        .admitted_build_bytes = build_peak_bytes,
        .retained_position_bytes = retained_position_bytes,
        .retained_table_bytes = retained_table_bytes,
        .retained_index_bytes = retained_index_bytes,
        .retained_index_permit_bytes = retained_index_permit_bytes,
        .transferred_position_footprint_bytes =
            transferred_position_footprint_bytes,
        .source_host_capacity_bytes_before_build =
            source_host_capacity_bytes_before_build,
        .source_footprint_reflected_bytes_before_build =
            source_footprint_reflected_bytes_before_build,
        .source_minimizer_count_before_build =
            source_minimizer_count_before_build,
        .retained_position_count = retained_position_count,
        .retained_position_initialized_bytes =
            retained_position_initialized_bytes,
        .source_host_permit_bytes_before_build =
            source_host_permit_bytes_before_build,
        .reserved_bytes_before_index_admission =
            reserved_bytes_before_index_admission,
        .reserved_bytes_after_index_admission =
            reserved_bytes_after_index_admission,
        .source_host_permit_retained_during_build =
            source_host_permit_retained_during_build,
        .source_footprint_authority_transferred_to_positions =
            exact_position_footprint_replacement,
        .every_retained_position_initialized =
            retained_position_count == source_minimizer_count_before_build
            && retained_position_initialized_bytes
                == retained_position_bytes,
        .source_capacity_released =
            target_host.minimizers.capacity() == 0u,
        .source_host_permit_cleared_after_consumption =
            source_host_permit_cleared_after_consumption,
        .source_adoption_cleared_after_consumption =
            source_adoption_cleared_after_consumption,
    };
    if (!admitted_index.is_valid()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan zero-copy target-index owning receipt was rejected"};
    }
    return admitted_index;
}

} // namespace pipe_entry_detail

/**
 * @brief Run the minimap2 pipeline against a pre-built target index using a
 * caller-supplied query minimiser vector.
 *
 * Single-query convenience wrapper that hands off to
 * `pipe::dispatch_pipeline<p_entry_point::with_prebuilt_minimizers>(...)`.  All algorithm-
 * choice and bucket dispatch happens inside the pipe cascade — no
 * `(p_alignment_algorithm, p_chaining_algorithm)` switch at this layer.
 */
template <class p_minimizer,
          sequence_alphabet p_alphabet,
          ::bioseq::indexing::minimizer_seed_index p_index>
[[nodiscard]] match_result_collection minimap2_port_with_index(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const std::vector<p_minimizer>& query_minimizers,
    const p_index& prebuilt_target_index,
    const minimap2_parameters& params = minimap2_parameters{},
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("minimap2_port_with_index");
    auto _scope = bioseq::instrumentation::region(_desc);
    return ::bioseq::simd::pipe::dispatch_pipeline<
        ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
            p_alphabet, p_minimizer, p_index>(
            query_seq, target_seq, query_minimizers, prebuilt_target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
}

template <class p_minimizer,
          sequence_alphabet p_alphabet,
          ::bioseq::indexing::minimizer_seed_index p_index>
[[nodiscard]] match_result_collection minimap2_port_with_index(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const ::bioseq::simd::pipe::minimizer_batch_view<p_minimizer>& query_minimizers,
    const p_index& prebuilt_target_index,
    const minimap2_parameters& params = minimap2_parameters{},
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache = nullptr,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("minimap2_port_with_index");
    auto _scope = bioseq::instrumentation::region(_desc);
    return ::bioseq::simd::pipe::dispatch_pipeline<
        ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
            p_alphabet, p_minimizer, p_index>(
            query_seq, target_seq, query_minimizers, prebuilt_target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
}

/**
 * @brief Run the minimap2 pipeline against pre-built target minimizers.
 *
 * Builds a target index from `target_minimizers` and delegates to
 * `minimap2_port_with_index`.
 */
template <class p_minimizer,
          sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection minimap2_port(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const std::vector<p_minimizer>& query_minimizers,
    std::vector<p_minimizer> target_minimizers,
    const minimap2_parameters& params = minimap2_parameters{})
{
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("minimap2_port");
    auto _scope = bioseq::instrumentation::region(_desc);
    const sequence_size_t target_len = target_seq.bp_size();
    minimizer_hashmap_fragment_index target_index;
    // — for the Stage C leaf with the GPU index build active, leave
    // the CPU index empty; `pipeline_run_impl_batch` must build it on the
    // device. This is the line that removes the XXL_HIFI
    // `target_index_build` bottleneck from the GPU row.
    const bool is_gpu_target_index_build =
        ::bioseq::simd::pipe::is_gpu_target_index_build_active(params);
    if (!is_gpu_target_index_build) {
        target_index.build_consuming(
            std::move(target_minimizers), target_len);
    } else {
        std::vector<p_minimizer>{}.swap(target_minimizers);
    }
    static const auto* _with_index_desc =
        bioseq::instrumentation::register_static_region("minimap2_port_with_index");
    auto _with_index_scope = bioseq::instrumentation::region(_with_index_desc);
    return ::bioseq::simd::pipe::dispatch_pipeline<
        ::bioseq::simd::pipe::pipeline_entry_point::with_prebuilt_minimizers,
            p_alphabet, p_minimizer, minimizer_hashmap_fragment_index>(
                query_seq, target_seq, query_minimizers, target_index, params,
                nullptr, {}, is_gpu_target_index_build,
                ::bioseq::simd::pipe::
                    transient_target_index_release_authorization{
                        .index = &target_index});
}

namespace pipe_entry_detail {

/// Fixed Vulkan target handoff for the selected query-independent source-direct
/// sorted-dense resident.  The adopted source is radix-sorted in place, counted
/// exactly, streamed into final mapped buffers, published under an
/// operation-unique key, and only then released.  No CPU FOA table or CPU
/// positions vector is constructed on this route.
template <class p_minimizer,
          sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection
minimap2_port_with_fixed_vulkan_target_handoff(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const std::vector<p_minimizer>& query_minimizers,
    fixed_vulkan_target_host_owner<p_minimizer> target_host,
    const minimap2_parameters& params)
{
    pipe::direct_resident_operation_stats ledger{};
    ledger.operation_identity = bioseq::indexing::
        next_gpu_source_direct_operation_identity();
    pipe::direct_resident_receipt_guard receipt_guard{&ledger};
    ledger.source_capacity_bytes =
        target_host.adoption.host_capacity_bytes;
    ledger.source_reflected_bytes =
        target_host.adoption.host_footprint_reflected_delta_bytes;
    ledger.source_retained_permit_bytes =
        target_host.host_permit.has_value()
            ? target_host.host_permit->bytes() : 0u;
    ledger.source_authority = target_host.minimizers.empty()
        ? pipe::source_direct_host_authorization::canonical_empty
        : target_host.adoption.is_host_permit_released_to_process_footprint
            ? pipe::source_direct_host_authorization::
                reflected_process_footprint
            : pipe::source_direct_host_authorization::retained_exact_permit;

    const sequence_size_t target_len = target_seq.bp_size();

    std::optional<bioseq::indexing::gpu_minimizer_index_cache>
        operation_gpu_cache;
    std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index>
        construction_handle;
    minimizer_hashmap_fragment_index empty_target_index;
    empty_target_index.target_len = target_len;
    bioseq::indexing::gpu_index_cache_identity cache_identity{};
    bioseq::indexing::gpu_minimizer_index_cache_key cache_key{};

    try {
        ledger.gpu_target_index_route_checked = true;
        ledger.gpu_target_index_route_compatible =
            !::bioseq::simd::pipe::
                is_gpu_target_index_build_active(params)
            && params.vulkan_enabled
            && params.vulkan_seed_hits_enabled
            && !params.vulkan_fused_frontend_enabled
            && !params.vulkan_gpu_index_build_enabled
            && params.minimizer_algorithm
                == minimizer_algorithm::vulkan_minimap_integer;
        if (!ledger.gpu_target_index_route_compatible) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "fixed Vulkan host target handoff has an ineligible direct tuple"};
        }

        ledger.source_authority_checked = true;
        ledger.has_valid_source_authorization =
            target_host.has_valid_ownership_authorization();
        if (!ledger.has_valid_source_authorization) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct target handoff lacks source authority"};
        }

        ledger.runtime_checked = true;
        auto& runtime = bioseq::gpu::vulkan::runtime::instance();
        ledger.runtime_available = true;
        ledger.hardware_checked = true;
        ledger.hardware_available =
            bioseq::gpu::vulkan::hardware_device_available(runtime);
        if (!ledger.hardware_available) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct target handoff has no Vulkan device"};
        }

        static const auto* _desc =
            bioseq::instrumentation::register_static_region("minimap2_port");
        auto _scope = bioseq::instrumentation::region(_desc);
        minimizer_hashmap_fragment_index::sort_source_by_hash_in_place(
            target_host.minimizers);
        ledger.sorted_shape_checked = true;
        const auto inspected = bioseq::indexing::
            inspect_presorted_minimizer_source<p_minimizer>(
                std::span<const p_minimizer>{
                    target_host.minimizers.data(),
                    target_host.minimizers.size()});
        if (!inspected) {
            ledger.is_sorted_shape_accepted = false;
            ledger.log_planning("reject", "sorted_source_shape_invalid");
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct sorted source shape is invalid"};
        }
        ledger.is_sorted_shape_accepted = true;
        ledger.minimizer_count = inspected->minimizer_count;
        ledger.distinct_count = inspected->distinct_count;

        using target_alphabet_type = p_alphabet;
        ledger.identity_checked = true;
        cache_identity = bioseq::indexing::
            make_gpu_source_direct_sorted_dense_identity<p_minimizer>(
                runtime,
                ledger.operation_identity,
                ledger.minimizer_count,
                ledger.distinct_count,
                static_cast<std::uint32_t>(
                    seqan3::alphabet_size<target_alphabet_type>),
                static_cast<std::uint32_t>(params.kmer_size),
                static_cast<std::uint32_t>(params.window_size));
        ledger.is_identity_accepted = bioseq::indexing::
            is_gpu_source_direct_identity_well_formed(cache_identity);
        if (!ledger.is_identity_accepted) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct cache identity is ineligible"};
        }
        cache_key = bioseq::indexing::make_gpu_index_cache_key(
            empty_target_index, cache_identity);

        ledger.u32_shape_checked = true;
        ledger.is_u32_shape_accepted =
            ledger.minimizer_count
                <= (std::numeric_limits<std::uint32_t>::max)()
            && ledger.distinct_count
                <= (std::numeric_limits<std::uint32_t>::max)()
            && ((ledger.minimizer_count == 0u)
                == (ledger.distinct_count == 0u));
        if (!ledger.is_u32_shape_accepted) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct resident shape exceeds the device ABI"};
        }

        ledger.storage_ranges_checked = true;
        const auto preflight = bioseq::indexing::
            plan_gpu_source_direct_sorted_dense(
                runtime,
                ledger.minimizer_count,
                ledger.distinct_count);
        ledger.max_storage_buffer_range =
            runtime.features().max_storage_buffer_range;
        if (!preflight) {
            ledger.is_control_range_accepted = false;
            ledger.is_position_range_accepted = false;
            ledger.log_planning("reject", "shape_or_storage_range_rejected");
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct resident preflight rejected"};
        }
        ledger.is_control_range_accepted = true;
        ledger.is_position_range_accepted = true;
        ledger.canonical_empty = preflight->canonical_empty;
        ledger.logical_control_bytes = preflight->control_bytes;
        ledger.logical_position_bytes = preflight->position_bytes;
        ledger.logical_resident_bytes =
            preflight->logical_resident_bytes;
        ledger.control_requirement_bytes =
            preflight->control_allocation_requirement_bytes;
        ledger.position_requirement_bytes =
            preflight->position_allocation_requirement_bytes;
        ledger.control_allowance_bytes =
            preflight->control_allocation_allowance_bytes;
        ledger.position_allowance_bytes =
            preflight->position_allocation_allowance_bytes;
        ledger.conservative_allowance_bytes = preflight->allowance_bytes;
        ledger.conservative_request_bytes =
            preflight->conservative_request_bytes;
        ledger.mapping_plan_checked = true;
        ledger.is_mapping_plan_accepted = true;
        const auto snapshot = bioseq::parallelism::
            memory_admission_controller::instance().diagnostic_snapshot();
        ledger.admission_snapshot_taken = true;
        ledger.reserved_before_bytes = snapshot.reserved_bytes;
        ledger.allocatable_before_bytes =
            snapshot.allocatable_after_reservations_bytes;
        if (ledger.allocatable_before_bytes
            >= ledger.conservative_request_bytes) {
            ledger.margin_bytes = ledger.allocatable_before_bytes
                - ledger.conservative_request_bytes;
        } else {
            ledger.shortfall_bytes = ledger.conservative_request_bytes
                - ledger.allocatable_before_bytes;
        }
        ledger.is_planning_accepted = ledger.shortfall_bytes == 0u;
        ledger.log_planning(
            ledger.is_planning_accepted ? "proceed" : "reject",
            ledger.is_planning_accepted
                ? "exact_shape_and_range_preflight_passed"
                : "resident_memory_shortfall");
        if (!ledger.is_planning_accepted) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct resident planning admission rejected"};
        }
        ledger.route_committed = true;

        if (!ledger.canonical_empty) {
            ledger.materialization_started = true;
            bioseq::parallelism::memory_admission_failure admission_failure{};
            auto resident_permit = bioseq::parallelism::
                memory_admission_controller::instance().try_reserve(
                    ledger.conservative_request_bytes,
                    &admission_failure);
            if (!resident_permit) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident admission failed"};
            }
            auto materialized = bioseq::indexing::
                materialize_sorted_dense_from_presorted_minimizers(
                    runtime,
                    std::span<const p_minimizer>{
                        target_host.minimizers.data(),
                    target_host.minimizers.size()},
                    target_len,
                    *preflight,
                    cache_key,
                    *resident_permit);
            if (!materialized) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident materialization failed"};
            }
            ledger.materialization = materialized->stats;
            ledger.materialization_completed = true;
            construction_handle = std::move(materialized->resident);
            operation_gpu_cache.emplace(
                ledger.materialization.actual_allocation_bytes);
            auto inserted = operation_gpu_cache->insert_built(
                cache_key, construction_handle);
            if (!inserted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error{
                    "source-direct resident preinsert failed"};
            }
            ledger.cache_preinsertions = 1u;
            ledger.resident_published = true;
            construction_handle.reset();
        }

        release_fixed_vulkan_source_after_resident_publication(
            target_host, ledger, ledger.resident_published);

        static const auto* _with_index_desc =
            bioseq::instrumentation::register_static_region(
                "minimap2_port_with_index");
        auto _with_index_scope = bioseq::instrumentation::region(_with_index_desc);
        auto result = ::bioseq::simd::pipe::dispatch_pipeline<
            ::bioseq::simd::pipe::
                pipeline_entry_point::with_prebuilt_minimizers,
                p_alphabet, p_minimizer, minimizer_hashmap_fragment_index>(
                    query_seq,
                    target_seq,
                    query_minimizers,
                    empty_target_index,
                    params,
                    operation_gpu_cache
                        ? &*operation_gpu_cache : nullptr,
                    cache_identity,
                    /*allow_gpu_target_index_build=*/false,
                    ::bioseq::simd::pipe::
                        transient_target_index_release_authorization{
                            .operation_owned_gpu_cache =
                                operation_gpu_cache
                                    ? &*operation_gpu_cache : nullptr,
                            .direct_ledger = &ledger});
        if (ledger.terminal_receipts != 1u || !ledger.terminal_success) {
            throw bioseq::gpu::vulkan::vulkan_runtime_error{
                "source-direct operation terminal receipt rejected"};
        }
        partial_vulkan_execution_stats operation_stats{};
        operation_stats.gpu_index_source_direct_operations = 1u;
        operation_stats.gpu_index_source_direct_terminal_successes = 1u;
        operation_stats.gpu_index_source_direct_empty_bypasses =
            ledger.empty_bypass ? 1u : 0u;
        operation_stats.gpu_index_source_direct_zero_query_bypasses =
            ledger.zero_query_bypasses;
        operation_stats.gpu_index_source_direct_planning_receipts =
            ledger.planning_receipts;
        operation_stats.gpu_index_source_direct_terminal_receipts =
            ledger.terminal_receipts;
        operation_stats.gpu_index_source_direct_materializations =
            ledger.materialization_completed ? 1u : 0u;
        operation_stats.gpu_index_source_direct_resident_publications =
            ledger.resident_published ? 1u : 0u;
        operation_stats.gpu_index_source_direct_logical_control_bytes =
            ledger.logical_control_bytes;
        operation_stats.gpu_index_source_direct_logical_position_bytes =
            ledger.logical_position_bytes;
        operation_stats.gpu_index_source_direct_control_requirement_bytes =
            ledger.control_requirement_bytes;
        operation_stats.gpu_index_source_direct_position_requirement_bytes =
            ledger.position_requirement_bytes;
        operation_stats.gpu_index_source_direct_allocation_request_bytes =
            ledger.conservative_request_bytes;
        operation_stats.gpu_index_source_direct_actual_control_bytes =
            ledger.materialization.actual_slot_allocation_bytes;
        operation_stats.gpu_index_source_direct_actual_position_bytes =
            ledger.materialization.actual_position_allocation_bytes;
        operation_stats.gpu_index_source_direct_source_releases =
            ledger.source_releases;
        operation_stats.gpu_index_source_direct_source_released_bytes =
            ledger.source_released_bytes;
        operation_stats.gpu_index_source_direct_checksum_matches =
            ledger.materialization.source_checksum != 0u
                && ledger.materialization.source_checksum
                    == ledger.materialization.mapped_checksum
            ? 1u : 0u;
        operation_stats.gpu_index_source_direct_source_traversals =
            ledger.materialization.complete_source_traversals;
        operation_stats.gpu_index_source_direct_mapped_traversals =
            ledger.materialization.complete_mapped_traversals;
        operation_stats.gpu_index_source_direct_reflected_authorities =
            ledger.materialization.authority
                == bioseq::indexing::gpu_resident_accounting_authority::
                    reflected_process_footprint
            ? 1u : 0u;
        operation_stats.gpu_index_source_direct_retained_authorities =
            ledger.materialization.authority
                == bioseq::indexing::gpu_resident_accounting_authority::
                    retained_exact_permit
            ? 1u : 0u;
        operation_stats.
            gpu_index_source_direct_retained_resident_permit_bytes =
                ledger.materialization.retained_permit_bytes;
        operation_stats.gpu_index_source_direct_cache_preinsertions =
            ledger.cache_preinsertions;
        operation_stats.gpu_index_source_direct_expected_seed_consumers =
            ledger.expected_seed_consumers;
        operation_stats.gpu_index_source_direct_completed_seed_consumers =
            ledger.completed_seed_consumers;
        operation_stats.gpu_index_source_direct_required_lookups =
            ledger.cache_required_lookups;
        operation_stats.gpu_index_source_direct_required_hits =
            ledger.cache_required_hits;
        operation_stats.gpu_index_source_direct_required_misses =
            ledger.cache_required_lookups - ledger.cache_required_hits;
        operation_stats.gpu_index_source_direct_fallback_attempts = 0u;
        operation_stats.gpu_index_source_direct_mapped_prefix_passes =
            ledger.mapped_prefix_passes;
        operation_stats.gpu_index_source_direct_mapped_prefix_lookups =
            ledger.mapped_prefix_lookups;
        operation_stats.gpu_index_source_direct_mapped_prefix_comparisons =
            ledger.mapped_prefix_comparisons;
        operation_stats.gpu_index_source_direct_seed_slab_requested_bytes =
            ledger.slab_requested_bytes;
        operation_stats.
            gpu_index_source_direct_seed_slab_initial_capacity =
                ledger.slab_capacity_before;
        operation_stats.gpu_index_source_direct_seed_slab_actual_bytes =
            ledger.slab_actual_bytes;
        operation_stats.gpu_index_source_direct_seed_slab_retained_bytes =
            ledger.slab_retained_permit_bytes;
        operation_stats.
            gpu_index_source_direct_seed_slab_permit_releases =
                ledger.slab_permit_releases;
        operation_stats.gpu_index_source_direct_seed_reserved_bytes =
            ledger.seed_reserved_bytes;
        operation_stats.gpu_index_source_direct_seed_planned_vulkan_bytes =
            ledger.seed_planned_vulkan_bytes;
        operation_stats.gpu_index_source_direct_seed_actual_vulkan_bytes =
            ledger.seed_actual_vulkan_bytes;
        operation_stats.gpu_index_source_direct_seed_roundtrips =
            ledger.seed_roundtrips;
        operation_stats.gpu_index_source_direct_zero_anchor_bypasses =
            ledger.zero_anchor_bypasses;
        operation_stats.gpu_index_source_direct_seed_fence_completions =
            ledger.seed_fence_completions;
        operation_stats.gpu_index_source_direct_cache_cleanup_releases =
            ledger.cache_cleanup_releases;
        operation_stats.gpu_index_source_direct_resident_cleanup_bytes =
            ledger.resident_cleanup_bytes;
        operation_stats.gpu_index_source_direct_pipeline_completions =
            ledger.pipeline_completed ? 1u : 0u;
        if (!ledger.canonical_empty) {
            operation_stats.gpu_index_materializations = 1u;
            operation_stats.gpu_index_sorted_dense_materializations = 1u;
            operation_stats.gpu_index_unique_keys = ledger.distinct_count;
            operation_stats.gpu_index_slot_capacity = ledger.distinct_count;
            operation_stats.gpu_index_retained_minimizer_count =
                ledger.minimizer_count;
            operation_stats.gpu_index_distinct_minimizer_count =
                ledger.distinct_count;
            operation_stats.gpu_index_logical_resident_bytes =
                ledger.logical_resident_bytes;
            operation_stats.gpu_index_actual_allocation_bytes =
                ledger.materialization.actual_allocation_bytes;
            operation_stats.gpu_index_fully_touched_bytes =
                ledger.materialization.fully_touched_bytes;
            const bool reflected_authority =
                ledger.materialization.authority
                    == bioseq::indexing::gpu_resident_accounting_authority::
                        reflected_process_footprint;
            operation_stats.gpu_index_permit_releases =
                reflected_authority ? 1u : 0u;
            operation_stats.gpu_index_process_footprint_reflections =
                reflected_authority ? 1u : 0u;
            operation_stats.gpu_index_reflected_footprint_bytes =
                ledger.materialization.reflected_footprint_bytes;
            operation_stats.gpu_index_max_load_ppm = 1'000'000u;
            operation_stats.gpu_index_max_probe = 0u;
        }
        // Seed lookup dispatch and biological-anchor stats were already
        // recorded by seed_hit_stage. This operation stats deliberately leaves
        // those generic fields zero so aggregation cannot double count them.
        result.record_partial_vulkan_execution_stats(operation_stats);
        return result;
    } catch (...) {
        const std::exception_ptr pending = std::current_exception();
        bool cleanup_failed = false;
        construction_handle.reset();
        try {
            if (operation_gpu_cache) {
                const std::uint64_t resident_bytes =
                    operation_gpu_cache->resident_bytes();
                operation_gpu_cache->clear();
                if (resident_bytes != 0u) {
                    ledger.cache_cleanup_releases = 1u;
                    ledger.resident_cleanup_bytes = resident_bytes;
                }
            }
        } catch (...) {
            cleanup_failed = true;
        }
        if (!ledger.source_release_completed) {
            try {
                release_fixed_vulkan_source_after_resident_publication(
                    target_host, ledger, ledger.resident_published);
            } catch (...) {
                cleanup_failed = true;
            }
        }
        if (ledger.planning_receipts == 0u) {
            ledger.log_planning("reject", "not_reached_sort_or_count_failure");
        }
        ledger.log_terminal(
            "failure",
            cleanup_failed
                ? "operation_exception_cleanup_failed"
                : "operation_exception");
        std::rethrow_exception(pending);
    }
}

// =============================================================================
// / — sketch leaf and helpers
// =============================================================================
//
// Target and query minimizer sketching use these shared components:
//   - `sketch_leaf<p_minimizer, p_collector, p_range, p_minimizer_algorithm>` — the chain's
//     terminal leaf; pins p_minimizer_algorithm at compile time and invokes the trait's
//     kernel directly.
//   - `sketch_traits<p_minimizer, p_collector, p_range>::leaf<p_minimizer_algorithm>` — the
//     alias-template wrapper exposing the leaf as a `template <auto>
//     class`.
//   - `sketch_one<p_minimizer>(...)` — one `dispatch_chain` instantiation
//     over `ma_step`.
//   - `sketch_into_collector<p_minimizer>(...)` — packages the per-pass
//     register_static_region + reserve + sketch_one into one inline call.

/// One operation-originated sketch stats, independent of whether the
/// dispatcher selected scalar, exact_simd, or Vulkan. The Vulkan member carries
/// only a role-neutral sketch token here; the caller that knows whether this
/// invocation sketched a query or target consumes it into the corresponding
/// match_result_collection stage counter.
struct minimizer_sketch_execution_stats {
    scalar_minimizer_execution_stats scalar{};
    bool scalar_available = false;
    two_stack_execution_stats exact_simd{};
    bool exact_simd_available = false;
    bounded_nthash_execution_stats bounded_nthash{};
    bool bounded_nthash_available = false;
    partial_vulkan_execution_stats partial_vulkan{};

    [[nodiscard]] bool empty() const noexcept {
        return !scalar_available
            && scalar == scalar_minimizer_execution_stats{}
            && !exact_simd_available
            && exact_simd == two_stack_execution_stats{}
            && !bounded_nthash_available
            && bounded_nthash == bounded_nthash_execution_stats{}
            && partial_vulkan == partial_vulkan_execution_stats{};
    }
};

/// Transfer a fixed Vulkan host vector and whichever one accounting authority
/// it owns into a single return value.  Dense vectors normally arrive with a
/// footprint reconciliation and no permit.  Sparse non-empty vectors may carry
/// their exact permit because allocator-page reuse made the RSS delta
/// inconclusive.  Device ownership must be unique and is destroyed here before
/// any later CPU-index admission can begin.
template <class p_minimizer>
[[nodiscard]] fixed_vulkan_target_host_owner<p_minimizer>
adopt_fixed_vulkan_sketch_host_result(
    bioseq::gpu::vulkan::vulkan_sketch_result<p_minimizer> source)
{
    using bioseq::gpu::vulkan::vulkan_runtime_error;
    using bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route;
    using bioseq::common::saturating_mul;

    if (source.host_ownership_route
            != vulkan_sketch_host_ownership_route::fixed_zero_copy_adoption
        || !source.arena_trimmed_before_host_materialization
        || !source.dispatch_token.is_internally_consistent()) {
        throw vulkan_runtime_error{
            "Vulkan fixed host result lacks zero-copy ownership authority"};
    }
    const std::uint64_t host_capacity_bytes = saturating_mul(
        static_cast<std::uint64_t>(source.host_minimizers.capacity()),
        static_cast<std::uint64_t>(sizeof(p_minimizer)));
    const bool valid_retained_permit = source.host_permit.has_value()
        && static_cast<bool>(*source.host_permit)
        && source.host_permit->bytes() == host_capacity_bytes;
    const bool footprint_authority =
        source.is_host_permit_released_to_process_footprint
        && !source.host_permit.has_value()
        && (host_capacity_bytes == 0u
            || source.host_footprint_reflected_delta_bytes
                >= host_capacity_bytes);
    const bool retained_permit_authority =
        !source.is_host_permit_released_to_process_footprint
        && host_capacity_bytes != 0u
        && valid_retained_permit;
    const bool device_owner_available = source.device != nullptr;
    const bool device_owner_unique = device_owner_available
        && source.device.use_count() == 1u;
    if (host_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || source.host_minimizers.size()
            != source.host_minimizers.capacity()
        || footprint_authority == retained_permit_authority
        || !device_owner_unique) {
        throw vulkan_runtime_error{
            "Vulkan fixed host result has inconsistent host/device ownership "
            "authority"};
    }

    std::weak_ptr<bioseq::gpu::vulkan::
        vulkan_device_minimizer_storage<p_minimizer>> device_lifetime =
            source.device;
    vulkan_sketch_host_adoption_receipt<p_minimizer> receipt{
        .source_data = source.host_minimizers.data(),
        .adopted_size = source.host_minimizers.size(),
        .host_capacity_bytes = host_capacity_bytes,
        .retained_host_permit_bytes = retained_permit_authority
            ? source.host_permit->bytes()
            : 0u,
        .host_footprint_reflected_delta_bytes =
            source.host_footprint_reflected_delta_bytes,
        .device_allocation_bytes_released =
            source.device->words.allocation_bytes,
        .is_host_permit_released_to_process_footprint =
            source.is_host_permit_released_to_process_footprint,
        .device_owner_available_before_adoption = device_owner_available,
        .device_owner_unique_before_adoption = device_owner_unique,
        .arena_trimmed_before_adoption =
            source.arena_trimmed_before_host_materialization,
        .dispatch_token = source.dispatch_token,
    };
    fixed_vulkan_target_host_owner<p_minimizer> adopted;
    adopted.minimizers.swap(source.host_minimizers);
    if (source.host_permit.has_value()) {
        adopted.host_permit.emplace(std::move(*source.host_permit));
        source.host_permit.reset();
    }
    source.device.reset();
    receipt.adopted_data = adopted.minimizers.data();
    receipt.adopted_capacity = adopted.minimizers.capacity();
    receipt.source_capacity_after_adoption =
        source.host_minimizers.capacity();
    receipt.device_released_before_return = device_lifetime.expired();
    adopted.adoption = receipt;
    if (!adopted.has_valid_ownership_authorization()) {
        throw vulkan_runtime_error{
            "Vulkan fixed host result was not adopted zero-copy"};
    }
    return adopted;
}

template <class p_minimizer>
struct fixed_vulkan_target_sketch {
    fixed_vulkan_target_host_owner<p_minimizer> host;
    minimizer_sketch_execution_stats stats{};

    fixed_vulkan_target_sketch(
            fixed_vulkan_target_host_owner<p_minimizer> adopted_host,
            minimizer_sketch_execution_stats execution_stats) noexcept
        : host(std::move(adopted_host)), stats(execution_stats) {}
    fixed_vulkan_target_sketch(const fixed_vulkan_target_sketch&) = delete;
    fixed_vulkan_target_sketch& operator=(const fixed_vulkan_target_sketch&) = delete;
    fixed_vulkan_target_sketch& operator=(fixed_vulkan_target_sketch&&) = delete;
    fixed_vulkan_target_sketch(fixed_vulkan_target_sketch&& other) noexcept
        : host(std::move(other.host)), stats(other.stats) {}
};

template <class p_minimizer, class p_range>
[[nodiscard]] fixed_vulkan_target_sketch<p_minimizer>
sketch_vulkan_into_adopted_host_vector(
    p_range& range,
    const std::size_t& window_len,
    const std::size_t& kmer_len,
    const ::bioseq::simd::minimap2_parameters& params)
{
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("mm_sketch");
    auto _scope = bioseq::instrumentation::region(_desc);
    using p_kernel = typename ::bioseq::simd::pipe::
        algorithm_traits<minimizer_algorithm::vulkan_minimap_integer>::
            kernel_type;
    auto source = p_kernel::template call_result<p_minimizer>(
        params,
        range,
        window_len,
        kmer_len,
        bioseq::gpu::vulkan::vulkan_sketch_host_ownership_route::
            fixed_zero_copy_adoption);
    if (!source.has_value()) {
        throw bioseq::gpu::vulkan::vulkan_runtime_error{
            "Vulkan fixed host sketch returned no result"};
    }
    auto adopted = adopt_fixed_vulkan_sketch_host_result(
        std::move(*source));
    minimizer_sketch_execution_stats stats{};
    stats.partial_vulkan.sketch_dispatches =
        adopted.adoption.dispatch_token.completed_roundtrips;
    return fixed_vulkan_target_sketch<p_minimizer>{
        std::move(adopted), stats};
}

[[nodiscard]] inline bool minimizer_sketch_stats_matches_selected_route(
    ::bioseq::simd::minimizer_algorithm selected,
    const minimizer_sketch_execution_stats& stats) noexcept
{
    if (selected
        == ::bioseq::simd::minimizer_algorithm::scalar) {
        return stats.scalar_available
            && stats.scalar.is_coherent()
            && !stats.exact_simd_available
            && stats.exact_simd == two_stack_execution_stats{}
            && !stats.bounded_nthash_available
            && stats.bounded_nthash == bounded_nthash_execution_stats{}
            && stats.partial_vulkan == partial_vulkan_execution_stats{};
    }
    if (selected
        == ::bioseq::simd::minimizer_algorithm::two_stack_nthash) {
        return !stats.scalar_available
            && stats.scalar == scalar_minimizer_execution_stats{}
            && stats.exact_simd_available
            && stats.exact_simd.is_internally_consistent(
                /*require_nonempty_vector_work=*/false)
            && !stats.bounded_nthash_available
            && stats.bounded_nthash == bounded_nthash_execution_stats{}
            && stats.partial_vulkan == partial_vulkan_execution_stats{};
    }
    if (selected
        == ::bioseq::simd::minimizer_algorithm::bounded_nthash_simd) {
        return !stats.scalar_available
            && stats.scalar == scalar_minimizer_execution_stats{}
            && !stats.exact_simd_available
            && stats.exact_simd == two_stack_execution_stats{}
            && stats.bounded_nthash_available
            && stats.bounded_nthash.is_internally_consistent(
                /*require_nonempty_vector_work=*/false)
            && stats.partial_vulkan == partial_vulkan_execution_stats{};
    }
    if (selected
        == ::bioseq::simd::minimizer_algorithm::vulkan_minimap_integer) {
        return !stats.scalar_available
            && stats.scalar == scalar_minimizer_execution_stats{}
            && !stats.exact_simd_available
            && stats.exact_simd == two_stack_execution_stats{}
            && !stats.bounded_nthash_available
            && stats.bounded_nthash == bounded_nthash_execution_stats{}
            && stats.partial_vulkan.has_valid_exact_sketch_token();
    }
    return stats.is_empty();
}

enum class minimizer_sketch_role : std::uint8_t {
    query,
    target,
};

/// Consume one already-validated operation stats into its call-site role.
/// This function never consults the algorithm selector and never manufactures
/// counters from route identity: only counters carried by the returned stats
/// are transferred.
inline void record_minimizer_sketch_execution_stats(
    match_result_collection& destination,
    const minimizer_sketch_execution_stats& stats,
    minimizer_sketch_role role) noexcept
{
    if (stats.scalar_available) {
        if (role == minimizer_sketch_role::query) {
            destination.record_scalar_query_minimizer_execution_stats(
                stats.scalar);
        } else {
            destination.record_scalar_target_minimizer_execution_stats(
                stats.scalar);
        }
    }
    if (stats.exact_simd_available) {
        if (role == minimizer_sketch_role::query) {
            destination.record_exact_simd_query_execution_stats(
                stats.exact_simd);
        } else {
            destination.record_exact_simd_target_execution_stats(
                stats.exact_simd);
        }
    }
    if (stats.bounded_nthash_available) {
        if (role == minimizer_sketch_role::query) {
            destination.record_bounded_nthash_query_execution_stats(
                stats.bounded_nthash);
        } else {
            destination.record_bounded_nthash_target_execution_stats(
                stats.bounded_nthash);
        }
    }
    if (stats.partial_vulkan.has_valid_exact_sketch_token()) {
        partial_vulkan_execution_stats routed{};
        if (role == minimizer_sketch_role::query) {
            routed.query_sketch_dispatches =
                stats.partial_vulkan.sketch_dispatches;
        } else {
            routed.target_sketch_dispatches =
                stats.partial_vulkan.sketch_dispatches;
        }
        destination.record_partial_vulkan_execution_stats(routed);
    }
}

template <class p_minimizer,
          class p_collector, class p_range,
          ::bioseq::simd::minimizer_algorithm p_minimizer_algorithm>
struct sketch_leaf {
    static_assert(std::is_same_v<decltype(p_minimizer_algorithm),
        ::bioseq::simd::minimizer_algorithm>,
        "sketch_leaf: p_minimizer_algorithm must be minimizer_algorithm (from ma_step).");

    BIOSEQ_PIPELINE_INLINE_ATTR
    static minimizer_sketch_execution_stats call(
        const ::bioseq::simd::minimap2_parameters& params,
        p_collector& collector, p_range& range,
        const std::size_t& window_len, const std::size_t& kmer_len)
    {
        using p_kernel = typename ::bioseq::simd::pipe::
            algorithm_traits<p_minimizer_algorithm>::kernel_type;
        if constexpr (p_kernel::emits_scalar_minimizer_execution_stats) {
            return {
                .scalar = p_kernel::template call<p_minimizer>(
                    params, collector, range, window_len, kmer_len),
                .scalar_available = true,
            };
        } else if constexpr (p_kernel::emits_exact_simd_execution_stats) {
            return {
                .exact_simd = p_kernel::template call<p_minimizer>(
                    params, collector, range, window_len, kmer_len),
                .exact_simd_available = true,
            };
        } else if constexpr (p_kernel::emits_bounded_nthash_execution_stats) {
            return {
                .bounded_nthash = p_kernel::template call<p_minimizer>(
                    params, collector, range, window_len, kmer_len),
                .bounded_nthash_available = true,
            };
        } else if constexpr (p_kernel::emits_partial_vulkan_execution_stats) {
            const auto token = p_kernel::template call<p_minimizer>(
                params, collector, range, window_len, kmer_len);
            if (!token.is_internally_consistent()) {
                ::bioseq::simd::pipe::throw_vulkan_declined(
                    "Vulkan minimizer sketch execution token");
            }
            minimizer_sketch_execution_stats stats{};
            stats.partial_vulkan.sketch_dispatches =
                token.completed_roundtrips;
            return stats;
        } else {
            p_kernel::template call<p_minimizer>(
                params, collector, range, window_len, kmer_len);
            return {};
        }
    }
};

template <class p_minimizer, class p_collector, class p_range>
struct sketch_traits {
    template <auto p_minimizer_algorithm>
    using leaf = sketch_leaf<p_minimizer, p_collector, p_range, p_minimizer_algorithm>;
};

/// One-step chain over the p_minimizer_algorithm dimension. The chain pins
/// p_minimizer_algorithm at compile time and the
/// leaf dispatches via the trait's kernel.
template <class p_minimizer, class p_collector, class p_range>
BIOSEQ_PIPELINE_INLINE_FLATTEN_ATTR
minimizer_sketch_execution_stats sketch_one(
    p_collector& collector, p_range& range,
    const std::size_t& window_len, const std::size_t& kmer_len,
    const ::bioseq::simd::minimap2_parameters& params)
{
    using traits_type = sketch_traits<p_minimizer, p_collector, p_range>;
    return ::bioseq::simd::pipe::detail::dispatch_chain<
        minimizer_sketch_execution_stats,
        traits_type::template leaf,
        ::bioseq::simd::pipe::detail::ma_step
    >::template run<>(params, collector, range, window_len, kmer_len);
}

/// Packages the per-pass instrumentation scope, collector reservation, and
/// sketch dispatch into one inline call.
template <class p_minimizer, class p_range>
BIOSEQ_PIPELINE_INLINE_ATTR
minimizer_sketch_execution_stats sketch_into_collector(
    sketch_collector_t<p_minimizer>& collector,
    p_range& range,
    const std::size_t& window_len, const std::size_t& kmer_len,
    const ::bioseq::simd::minimap2_parameters& params)
{
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("mm_sketch");
    auto _scope = bioseq::instrumentation::region(_desc);
    collector.reserve(
        range.bp_size() / std::max<std::size_t>(window_len, 1) + 64);
    return sketch_one<p_minimizer>(
        collector, range, window_len, kmer_len, params);
}

/**
 * @brief Per-bucket helper for the from-sequence `minimap_port` entry.
 *
 * Owns one `Arena::chain` scope around the target minimiser sketching + index
 * build.  Sketching is dispatched via `sketch_into_collector<p_minimizer>` —
 * the per-pass switch on `params.minimizer_algorithm` is now expressed as a
 * single-step `dispatch_chain` instantiation inside `sketch_one`.  Once the
 * target index is built, the function delegates to `minimap2_port_with_index`
 * so the per-query minimizer_stage inside `pipe::dispatch_pipeline` handles
 * query sketching.
 */
template <class p_minimizer,
          sequence_alphabet p_alphabet>
[[nodiscard]] match_result_collection minimap_port_with_repr(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const minimap2_parameters& params)
{
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("minimap_port");
    auto _scope = bioseq::instrumentation::region(_desc);

    std::optional<context::context> owned_context;
    std::optional<context::installed_context_scope> context_install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        context_install.emplace(*owned_context);
    }

    const std::size_t window_len = static_cast<std::size_t>(params.window_size);
    const std::size_t kmer_len   = static_cast<std::size_t>(params.kmer_size);

    // — skip the host target sketch for the Stage C GPU-index leaf;
    // the device index build sketches the target itself. If that selected GPU
    // path cannot run, the pipeline throws instead of rebuilding on CPU.
    // The fixed Vulkan host vector carries either footprint authority or its
    // exact retained permit. Its dedicated handoff owns the later CPU-index
    // permit locally and destroys the nested index before releasing it.
    std::vector<p_minimizer> target_minimizers;
    std::optional<fixed_vulkan_target_sketch<p_minimizer>> fixed_target_sketch;
    minimizer_sketch_execution_stats target_sketch_stats{};
    const bool is_gpu_target_index_build =
        ::bioseq::simd::pipe::is_gpu_target_index_build_active(params);
    using target_alphabet_type = p_alphabet;
    const bool fixed_target_zero_copy_route =
        fixed_vulkan_target_host_adoption_route_selected<p_minimizer>(
            is_gpu_target_index_build,
            params,
            static_cast<std::uint32_t>(
                seqan3::alphabet_size<target_alphabet_type>),
            static_cast<std::uint64_t>(target_seq.bp_size()));
    if (!is_gpu_target_index_build) {
        if (fixed_target_zero_copy_route) {
            fixed_target_sketch.emplace(
                sketch_vulkan_into_adopted_host_vector<p_minimizer>(
                    target_seq, window_len, kmer_len, params));
            target_sketch_stats = fixed_target_sketch->stats;
        } else {
            sketch_collector_t<p_minimizer> target_collector;
            target_sketch_stats = sketch_into_collector<p_minimizer>(
                target_collector, target_seq, window_len, kmer_len, params);
            target_minimizers =
                std::move(target_collector).extract_sketches_data();
        }
    }

    const std::uint64_t query_sketch_started_ns =
        pipe::host_stage_now_ns();
    // Query results are intentionally left on the collector-compatible route.
    // Small/repeated allocations can reuse already-resident allocator pages,
    // so a footprint-delta release is not reliable authority for them.
    sketch_collector_t<p_minimizer> query_collector;
    const minimizer_sketch_execution_stats query_sketch_stats =
        sketch_into_collector<p_minimizer>(
            query_collector, query_seq, window_len, kmer_len, params);
    const auto query_minimizers =
        std::move(query_collector).extract_sketches_data();
    const std::uint64_t query_sketch_host_wall_ns =
        pipe::elapsed_host_stage_ns(query_sketch_started_ns);
    const bool query_route_valid =
        minimizer_sketch_stats_matches_selected_route(
            params.minimizer_algorithm, query_sketch_stats);
    const bool target_route_valid = is_gpu_target_index_build
        || minimizer_sketch_stats_matches_selected_route(
            params.minimizer_algorithm, target_sketch_stats);
    const bool exact_simd_pair_route_compatible = is_gpu_target_index_build
        || params.minimizer_algorithm
            != minimizer_algorithm::two_stack_nthash
        || (query_sketch_stats.exact_simd_available
            && target_sketch_stats.exact_simd_available
            && pipe::exact_simd_stats_share_selection_route(
                query_sketch_stats.exact_simd,
                target_sketch_stats.exact_simd));
    if (!query_route_valid
        || !target_route_valid
        || !exact_simd_pair_route_compatible) {
        bioseq_log<logging_level::error>(
            "Selected minimizer route returned mismatched execution stats or "
            "incompatible exact_simd selection schemes");
        std::abort();
    }

    auto owned_target_minimizers = std::move(target_minimizers);
    std::vector<p_minimizer>{}.swap(target_minimizers);
    match_result_collection result = fixed_target_zero_copy_route
        ? minimap2_port_with_fixed_vulkan_target_handoff<
              p_minimizer, p_alphabet>(
                  query_seq,
                  target_seq,
                  query_minimizers,
                  std::move(fixed_target_sketch->host),
                  params)
        : minimap2_port<p_minimizer, p_alphabet>(
              query_seq,
              target_seq,
              query_minimizers,
              std::move(owned_target_minimizers),
              params);
    result.record_query_sketch_host_time(query_sketch_host_wall_ns);
    record_minimizer_sketch_execution_stats(
        result, query_sketch_stats, minimizer_sketch_role::query);
    if (!is_gpu_target_index_build) {
        record_minimizer_sketch_execution_stats(
            result, target_sketch_stats, minimizer_sketch_role::target);
    }
    return result;
}

// =============================================================================
// — from-sequence cascade leaf and traits
// =============================================================================
//
// `minimap_port` uses one `dispatch_chain<...>::run<>(...)` over
// two steps (p_bucket_width, p_kmer_size).  The leaf below pins `(p_bucket_width, p_kmer_size)` into the
// `minimizer_representation<p_bucket_width, p_kmer_size>` storage type and
// delegates to `minimap_port_with_repr<p_minimizer>`.

template <sequence_alphabet p_alphabet,
          std::size_t p_bucket_width, std::size_t p_kmer_size>
struct minimap_port_repr_leaf {
    static_assert(std::is_same_v<decltype(p_bucket_width), std::size_t>,
        "minimap_port_repr_leaf: p_bucket_width must be std::size_t (from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(p_kmer_size), std::size_t>,
        "minimap_port_repr_leaf: p_kmer_size must be std::size_t (from kmer_size_step).");

    BIOSEQ_PIPELINE_INLINE_ATTR
    static match_result_collection call(
        const minimap2_parameters& params,
        bioseq::storage::lazy_sequence_view<p_alphabet> q,
        bioseq::storage::lazy_sequence_view<p_alphabet> t)
    {
        using p_minimizer = ::bioseq::minimizers::minimizer_representation<p_bucket_width, p_kmer_size>;
        return minimap_port_with_repr<p_minimizer, p_alphabet>(
            q, t, params);
    }
};

template <sequence_alphabet p_alphabet>
struct minimap_port_traits {
    template <auto p_kmer_size, auto p_bucket_width>
    using leaf = minimap_port_repr_leaf<p_alphabet, p_bucket_width, p_kmer_size>;
};

} // namespace pipe_entry_detail

/**
 * @brief Top-level minimap2 entry for raw query and target sequences.
 *
 * One `dispatch_chain<...>::run<>(...)` line over `seq_len_log2_step` +
 * `kmer_size_step`. Each pinned
 * `(p_bucket_width, p_kmer_size)` selects a `minimizer_representation<p_bucket_width, p_kmer_size>` and forwards to
 * `pipe_entry_detail::minimap_port_with_repr<p_minimizer>` which sketches
 * both target and query minimisers, builds the target index, and delegates
 * to `minimap2_port_with_index` (which in turn calls
 * `pipe::dispatch_pipeline<p_entry_point::with_prebuilt_minimizers>`).
 */
template <sequence_alphabet p_alphabet>
BIOSEQ_PIPELINE_NODISCARD_INLINE_FLATTEN_ATTR
match_result_collection minimap_port(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const minimap2_parameters& params = minimap2_parameters{})
{
    using traits_type = pipe_entry_detail::minimap_port_traits<p_alphabet>;
    // Pin p_kmer_size before the dependent p_bucket_width step.
    return ::bioseq::simd::pipe::detail::dispatch_chain<
        match_result_collection,
        traits_type::template leaf,
        ::bioseq::simd::pipe::detail::kmer_size_step,
        ::bioseq::simd::pipe::detail::seq_len_log2_step_per_k
    >::template run<>(params, query_seq, target_seq);
}

/**
 * @brief Memory-bounded recompute entry for arbitrarily large targets on a
 * fixed-memory host.
 *
 * The non-segmented `minimap_port` builds one minimizer index over the whole
 * target (~6 bytes/bp at peak). This entry indexes the target in
 * admission-sized segments (at most 200 Mbp core plus overlap), pinning the 8-byte
 * `minimap_sketch_data` minimizer (segment-local positions fit its 28-bit field).
 * Each segment index is globalized with `add_position_offset` and run
 * through `minimap2_port_with_index` against the full target, so
 * chaining + DP alignment see correct global coordinates and stream the real
 * target bytes. A result is kept only when its `target_start` lands in the
 * segment's CORE — with overlap ≥ the longest match span, every alignment is
 * fully indexed by exactly one segment, so there are no duplicates and no
 * boundary-split partials. Per-segment index memory is freed each iteration, so
 * the live footprint is ~one segment (~1.5 GB) regardless of target size.
 *
 * Limitation (documented): `max_occ` high-frequency-seed filtering is applied
 * per-segment, so a k-mer whose genome-wide occurrences are split across
 * segments can pass the filter in each. For low-repeat targets (the sample
 * generator's near-random sequences) occurrences are ~uniform and well under
 * `max_occ`, so anchors match the non-segmented path; a global pre-count would
 * restore exact semantics for highly repetitive references (future work).
 */
template <class p_minimizer>
struct segment_fragment_emission {
    std::size_t fragment_no = 0;
    sequence_size_t fragment_len_bp = 0;
    std::vector<p_minimizer> minimizers{};
};

template <class p_minimizer>
[[nodiscard]] std::vector<segment_fragment_emission<p_minimizer>>
collect_fragment_emissions_for_segment(
        const ::bioseq::indexing::segment_unit_range& range,
        std::span<const p_minimizer> segment_minimizers,
        sequence_size_t total_bp,
        sequence_size_t frag_bp,
        std::size_t lpu) {
    const sequence_size_t emit_base_bp =
        static_cast<sequence_size_t>(range.cs_u) * lpu;
    const std::size_t frag_lo =
        static_cast<std::size_t>(range.core_lo_bp / frag_bp);
    const std::size_t frag_hi =
        static_cast<std::size_t>((range.core_hi_bp + frag_bp - 1) / frag_bp);

    std::vector<segment_fragment_emission<p_minimizer>> out;
    out.reserve(frag_hi > frag_lo ? frag_hi - frag_lo : 0);
    for (std::size_t fragment_no = frag_lo; fragment_no < frag_hi; ++fragment_no) {
        const sequence_size_t fragment_lo_bp =
            static_cast<sequence_size_t>(fragment_no) * frag_bp;
        const sequence_size_t fragment_hi_bp =
            std::min(fragment_lo_bp + frag_bp, total_bp);
        const sequence_size_t claim_lo =
            std::max(fragment_lo_bp, range.core_lo_bp);
        const sequence_size_t claim_hi =
            std::min(fragment_hi_bp, range.core_hi_bp);
        if (claim_lo >= claim_hi) {
            continue;
        }

        segment_fragment_emission<p_minimizer> emission{};
        emission.fragment_no = fragment_no;
        emission.fragment_len_bp = fragment_hi_bp - fragment_lo_bp;
        for (const p_minimizer& minimizer : segment_minimizers) {
            const sequence_size_t global_pos =
                emit_base_bp
                + static_cast<sequence_size_t>(minimizer.last_pos());
            if (global_pos >= claim_lo && global_pos < claim_hi) {
                emission.minimizers.emplace_back(
                    minimizer.kmer(),
                    static_cast<std::uint64_t>(global_pos - fragment_lo_bp),
                    minimizer.strand());
            }
        }
        out.push_back(std::move(emission));
    }
    return out;
}

template <class p_minimizer>
struct pending_fragment_build {
    sequence_size_t fragment_len_bp = 0;
    std::vector<p_minimizer> minimizers{};
};

template <class p_fragment_storage, class p_minimizer>
class fragment_persist_accumulator {
public:
    fragment_persist_accumulator(
            p_fragment_storage& storage,
            sequence_size_t total_bp,
            sequence_size_t frag_bp)
        : storage_(storage),
          total_bp_(total_bp),
          frag_bp_(frag_bp),
          nfrag_(static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp)) {}

    void ingest(std::span<const segment_fragment_emission<p_minimizer>> emissions) {
        for (const segment_fragment_emission<p_minimizer>& emission : emissions) {
            pending_fragment_build<p_minimizer>& pending =
                pending_[emission.fragment_no];
            pending.fragment_len_bp = emission.fragment_len_bp;
            pending.minimizers.insert(
                pending.minimizers.end(),
                emission.minimizers.begin(),
                emission.minimizers.end());
        }
    }

    void flush_ready(sequence_size_t processed_core_hi_bp) {
        while (next_flush_fragment_ < nfrag_) {
            if (fragment_hi_bp(next_flush_fragment_) > processed_core_hi_bp) {
                break;
            }
            flush_one(next_flush_fragment_++);
        }
    }

    void finish() {
        while (next_flush_fragment_ < nfrag_) {
            flush_one(next_flush_fragment_++);
        }

        const std::int64_t manifest = static_cast<std::int64_t>(nfrag_);
        storage_.resize(
            bioseq::indexing::k_directory_sub_key,
            sizeof(std::int64_t));
        storage_.template write_to_each<std::int64_t>(
            bioseq::indexing::k_directory_sub_key,
            [manifest](
                    std::size_t chunk_no,
                    std::int64_t* manifest_buf,
                    std::size_t) {
                manifest_buf[0] = manifest;
                return chunk_no;
            });
    }

private:
    [[nodiscard]] sequence_size_t fragment_lo_bp(std::size_t fragment_no) const {
        return static_cast<sequence_size_t>(fragment_no) * frag_bp_;
    }

    [[nodiscard]] sequence_size_t fragment_hi_bp(std::size_t fragment_no) const {
        return std::min(fragment_lo_bp(fragment_no) + frag_bp_, total_bp_);
    }

    void flush_one(std::size_t fragment_no) {
        pending_fragment_build<p_minimizer> pending{};
        if (auto it = pending_.find(fragment_no); it != pending_.end()) {
            pending = std::move(it->second);
            pending_.erase(it);
        }
        if (pending.fragment_len_bp == 0) {
            pending.fragment_len_bp =
                fragment_hi_bp(fragment_no) - fragment_lo_bp(fragment_no);
        }

        minimizer_hashmap_fragment_index frag_idx;
        frag_idx.build(pending.minimizers, pending.fragment_len_bp);

        std::vector<char> blob(frag_idx.serialize_binary_size());
        frag_idx.serialize_to_binary(blob.data());

        const auto sub_key =
            static_cast<bioseq::indexing::index_sub_key_t>(fragment_no);
        storage_.resize(sub_key, blob.size());
        storage_.template write_to_each<char>(
            sub_key,
            [&blob](std::size_t chunk_no, char* buf, std::size_t cap_bytes) {
                const std::size_t offset =
                    chunk_no * bioseq::indexing::k_max_index_chunk_size_bytes;
                const std::size_t n =
                    offset >= blob.size()
                        ? 0
                        : std::min(cap_bytes, blob.size() - offset);
                if (n != 0) {
                    std::memcpy(buf, blob.data() + offset, n);
                }
                return chunk_no;
            });
    }

    p_fragment_storage& storage_;
    sequence_size_t total_bp_ = 0;
    sequence_size_t frag_bp_ = 0;
    std::size_t nfrag_ = 0;
    std::size_t next_flush_fragment_ = 0;
    std::map<std::size_t, pending_fragment_build<p_minimizer>> pending_{};
};

// =============================================================================
// — query sketch for persistent-index bucket selection.
// =============================================================================
//
// Holds the query minimizers produced for persistent-index bucket filtering,
// keeping the optional GPU host-result permit and device span alive for the
// caller's lifetime. The host vector feeds the assembly/filter helpers; the
// Vulkan branch sketches the query on the GPU so the segmented
// path's seed lookup consumes byte-identical minimizers.
template <class p_minimizer>
struct budgeted_sketch_vector {
    // Reverse destruction is the ownership contract: host capacity and the
    // device lifetime anchor disappear before either reservation can release.
    std::optional<bioseq::parallelism::memory_permit> permit;
    std::optional<bioseq::gpu::vulkan::vulkan_device_minimizer_span<p_minimizer>> device;
    std::vector<p_minimizer> minimizers;
    scalar_minimizer_execution_stats scalar_execution_stats{};
    bool scalar_execution_stats_available = false;
    two_stack_execution_stats exact_simd_execution_stats{};
    bool exact_simd_execution_stats_available = false;
    bounded_nthash_execution_stats bounded_nthash_execution_stats{};
    bool bounded_nthash_execution_stats_available = false;

    budgeted_sketch_vector() = default;
    budgeted_sketch_vector(const budgeted_sketch_vector&) = delete;
    budgeted_sketch_vector& operator=(const budgeted_sketch_vector&) = delete;

    budgeted_sketch_vector(budgeted_sketch_vector&& other) noexcept {
        steal_from_(std::move(other));
    }

    budgeted_sketch_vector& operator=(budgeted_sketch_vector&& other) noexcept {
        if (this != &other) {
            release_owned_memory_before_permit();
            steal_from_(std::move(other));
        }
        return *this;
    }

    void release_owned_memory_before_permit() noexcept {
        {
            std::vector<p_minimizer> empty;
            minimizers.swap(empty);
        }
        device.reset();
        permit.reset();
    }

    [[nodiscard]] std::span<const p_minimizer> as_span() const noexcept {
        return minimizers;
    }

private:
    void steal_from_(budgeted_sketch_vector&& other) noexcept {
        // Transfer allocations while all source admission anchors remain live,
        // then transfer device and host permits. Swap proves the source has no
        // residual vector capacity after the permit move.
        minimizers.swap(other.minimizers);
        device = std::move(other.device);
        other.device.reset();
        permit = std::move(other.permit);
        other.permit.reset();
        scalar_execution_stats = other.scalar_execution_stats;
        scalar_execution_stats_available =
            std::exchange(other.scalar_execution_stats_available, false);
        exact_simd_execution_stats = other.exact_simd_execution_stats;
        exact_simd_execution_stats_available =
            std::exchange(other.exact_simd_execution_stats_available, false);
        bounded_nthash_execution_stats = other.bounded_nthash_execution_stats;
        bounded_nthash_execution_stats_available =
            std::exchange(other.bounded_nthash_execution_stats_available, false);
    }
};

template <class p_minimizer, sequence_alphabet p_alphabet>
[[nodiscard]] budgeted_sketch_vector<p_minimizer> sketch_query_for_index_selection(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    const minimap2_parameters& params,
    std::size_t w,
    std::size_t k)
{
    require_representable_minimizer_query_length<p_minimizer>(
        query_seq.bp_size());
    if (params.minimizer_algorithm == minimizer_algorithm::vulkan_minimap_integer)
    {
        if (!params.vulkan_enabled) {
            pipe::throw_vulkan_declined("Vulkan query minimizer sketch");
        }
        bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
        if (auto gpu_sketch =
                bioseq::gpu::vulkan::collect_minimizers_vulkan_result<p_minimizer>(
                    query_seq,
                    w,
                    k,
                    &decline))
        {
            budgeted_sketch_vector<p_minimizer> result;
            result.minimizers.swap(gpu_sketch->host_minimizers);
            result.device = gpu_sketch->device_span();
            result.permit = std::move(gpu_sketch->host_permit);
            return result;
        }
        pipe::throw_vulkan_sketch_declined(
            "Vulkan query minimizer sketch", decline);
    }

    sketch_collector_t<p_minimizer> query_collector;
    query_collector.reserve(query_seq.bp_size() / std::max<std::size_t>(w, 1) + 64);
    // Vulkan query sketching returned above. For the existing all-simd tuple,
    // use the same exact_simd continuation backend as target preparation;
    // every other non-GPU tuple retains its explicitly scalar query backend.
    const minimizer_sketch_backend cpu_query_backend =
        pipe::target_index_sketch_backend_from_params(params);
    // The SQL/bench dna4 query representation is packed in u64 units, while
    // the exact K15/W10/B8 continuation kernel consumes u32 words on NEON.
    // Reinterpret a whole resident query as u32 without copying so the same
    // persisted-index-compatible continuation scheme executes eight
    // chronological candidates per block.  This is intentionally *not* the
    // report-76 resident vector_stream selection route: its minimizer scheme
    // differs from the persisted target index and remains forbidden here.
    // The u32 view changes only the decoder lane width; scalar-oracle
    // differential tests freeze the emitted raw minimizer sequence. Native
    // little-endian word order is part of this zero-copy split; other targets
    // retain the existing generic-u64 continuation.
    const pipe::minimizer_range_execution_stats execution_stats =
        pipe::minimizer_dispatch_indexed_query_whole<p_minimizer>(
            query_collector,
            query_seq,
            w,
            k,
            cpu_query_backend,
            params.highway_kernels);
    if (!pipe::minimizer_range_stats_matches_backend(
            cpu_query_backend,
            execution_stats,
            /*require_nonempty_vector_work=*/false,
            pipe::exact_simd_range_route::require_continuation)) {
        throw std::runtime_error(
            "indexed query minimizer sketch did not execute the required "
            "persisted-index-compatible route");
    }
    budgeted_sketch_vector<p_minimizer> result;
    auto minimizers = std::move(query_collector).extract_sketches_data();
    result.minimizers.swap(minimizers);
    result.scalar_execution_stats = execution_stats.scalar;
    result.scalar_execution_stats_available =
        execution_stats.scalar_available;
    result.exact_simd_execution_stats = execution_stats.exact_simd;
    result.exact_simd_execution_stats_available =
        execution_stats.exact_simd_available;
    result.bounded_nthash_execution_stats = execution_stats.bounded_nthash;
    result.bounded_nthash_execution_stats_available =
        execution_stats.bounded_nthash_available;
    return result;
}

template <sequence_alphabet p_alphabet,
          class p_fragment_index_storage = std::nullptr_t>
[[nodiscard]] match_result_collection minimap_port_segmented(
    bioseq::storage::lazy_sequence_view<p_alphabet> query_seq,
    bioseq::storage::lazy_sequence_view<p_alphabet> target_seq,
    const minimap2_parameters& params = minimap2_parameters{},
    p_fragment_index_storage* fragment_index_out = nullptr)
{
    using p_minimizer = minimap_sketch_data;
    static const auto* _desc =
        bioseq::instrumentation::register_static_region("minimap_port_segmented");
    auto _scope = bioseq::instrumentation::region(_desc);

    const std::size_t w = static_cast<std::size_t>(params.window_size);
    const std::size_t k = static_cast<std::size_t>(params.kmer_size);
    const sequence_size_t query_len  = query_seq.bp_size();
    const sequence_size_t target_len = target_seq.bp_size();
    if (query_len == 0 || target_len == 0) {
        return match_result_collection{};
    }

    //  Patch D — resolve the target-index sketch backend once; every
    // target `minimizer_dispatch_range` call below passes it explicitly so no
    // target seam can silently fall back to scalar.
    const minimizer_sketch_backend target_sketch_backend =
        pipe::target_index_sketch_backend_from_params(params);

    std::optional<bioseq::indexing::gpu_minimizer_index_cache> segmented_gpu_index_cache;
    if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
        bioseq::gpu::vulkan::runtime& gpu_rt = bioseq::gpu::vulkan::runtime::instance();
        std::uint64_t limit = bioseq::gpu::vulkan::hardware_device_available(gpu_rt)
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        segmented_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::gpu_minimizer_index_cache* segmented_gpu_index_cache_ptr =
        segmented_gpu_index_cache ? &*segmented_gpu_index_cache : nullptr;
    auto segment_gpu_identity = [](std::size_t segment_index) noexcept {
        return bioseq::indexing::gpu_index_cache_identity{
            .assembly_identity = 0x9e3779b97f4a7c15ULL
                ^ (static_cast<std::uint64_t>(segment_index) + 1u),
        };
    };

    // (1) Sketch the query ONCE as minimap_sketch_data (query <= 2^28 bp; its
    //     positions fit the 28-bit field). Same kmer hashing as the target, so
    //     `collect_seed_hits` matches identically. — the helper
    //     sketches on the GPU when vulkan_minimap_integer is selected (byte-identical
    //     minimizers) and otherwise uses the scalar CPU sketch.
    auto query_sketch =
        sketch_query_for_index_selection<p_minimizer>(query_seq, params, w, k);
    const pipe::minimizer_batch_view<p_minimizer> query_minimizers{
        .host = std::span<const p_minimizer>{
            query_sketch.minimizers.data(), query_sketch.minimizers.size()},
        .device = query_sketch.device,
    };
    const bool exact_simd_segmented_route =
        target_sketch_backend == minimizer_sketch_backend::exact_simd;
    const bool bounded_nthash_segmented_route =
        target_sketch_backend == minimizer_sketch_backend::bounded_nthash_simd;
    const bool scalar_segmented_route =
        target_sketch_backend == minimizer_sketch_backend::scalar;
    auto record_target_range_stats =
        [exact_simd_segmented_route, bounded_nthash_segmented_route,
         scalar_segmented_route](
                match_result_collection& collection,
                const pipe::minimizer_range_execution_stats& range_stats) {
            if (exact_simd_segmented_route) {
                if (!range_stats.exact_simd_available
                    || !range_stats.exact_simd.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false)
                    || range_stats.bounded_nthash_available
                    || range_stats.scalar_available) {
                    bioseq_log<logging_level::error>(
                        "exact_simd segmented target range lacks actual stats");
                    std::abort();
                }
                collection.record_exact_simd_target_execution_stats(
                    range_stats.exact_simd);
            } else if (bounded_nthash_segmented_route) {
                if (!range_stats.bounded_nthash_available
                    || !range_stats.bounded_nthash.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false)
                    || range_stats.exact_simd_available
                    || range_stats.scalar_available) {
                    bioseq_log<logging_level::error>(
                        "bounded_nthash_simd segmented target range lacks actual stats");
                    std::abort();
                }
                collection.record_bounded_nthash_target_execution_stats(
                    range_stats.bounded_nthash);
            } else if (scalar_segmented_route) {
                if (!range_stats.scalar_available
                    || !range_stats.scalar.is_coherent(
                        /*require_nonempty_work=*/false)
                    || range_stats.exact_simd_available
                    || range_stats.bounded_nthash_available) {
                    bioseq_log<logging_level::error>(
                        "scalar segmented target range lacks actual stats");
                    std::abort();
                }
                collection.record_scalar_target_minimizer_execution_stats(
                    range_stats.scalar);
            } else if (range_stats.exact_simd_available
                       || range_stats.bounded_nthash_available
                       || range_stats.scalar_available) {
                bioseq_log<logging_level::error>(
                    "scalar/Vulkan segmented target range emitted CPU simd sketch stats");
                std::abort();
            }
        };
    auto finalize_segmented_stats =
        [&query_sketch, exact_simd_segmented_route,
         bounded_nthash_segmented_route, scalar_segmented_route](
                match_result_collection collection) {
            if (exact_simd_segmented_route) {
                if (!query_sketch.exact_simd_execution_stats_available
                    || !query_sketch.exact_simd_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false)
                    || query_sketch.bounded_nthash_execution_stats_available
                    || query_sketch.scalar_execution_stats_available
                    || !collection.exact_simd_target_execution_stats_available
                    || !collection.exact_simd_target_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false)
                    || collection.bounded_nthash_target_execution_stats_available) {
                    bioseq_log<logging_level::error>(
                        "exact_simd segmented query/target stats coverage is incomplete");
                    std::abort();
                }
                collection.record_exact_simd_query_execution_stats(
                    query_sketch.exact_simd_execution_stats);
            } else if (bounded_nthash_segmented_route) {
                if (!query_sketch.bounded_nthash_execution_stats_available
                    || !query_sketch.bounded_nthash_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false)
                    || query_sketch.exact_simd_execution_stats_available
                    || query_sketch.scalar_execution_stats_available
                    || !collection.bounded_nthash_target_execution_stats_available
                    || !collection.bounded_nthash_target_execution_stats.is_internally_consistent(
                        /*require_nonempty_vector_work=*/false)
                    || collection.exact_simd_target_execution_stats_available) {
                    bioseq_log<logging_level::error>(
                        "bounded_nthash_simd segmented query/target stats coverage is incomplete");
                    std::abort();
                }
                collection.record_bounded_nthash_query_execution_stats(
                    query_sketch.bounded_nthash_execution_stats);
            } else if (scalar_segmented_route) {
                if (!query_sketch.scalar_execution_stats_available
                    || !query_sketch.scalar_execution_stats.is_coherent()
                    || query_sketch.exact_simd_execution_stats_available
                    || query_sketch.bounded_nthash_execution_stats_available
                    || !collection.scalar_pipeline_execution_stats_available
                    || !collection.scalar_pipeline_execution_stats
                        .target_minimizers.is_coherent()
                    || collection.exact_simd_target_execution_stats_available
                    || collection.bounded_nthash_target_execution_stats_available) {
                    bioseq_log<logging_level::error>(
                        "scalar segmented query/target stats coverage is incomplete");
                    std::abort();
                }
                collection.record_scalar_query_minimizer_execution_stats(
                    query_sketch.scalar_execution_stats);
            } else if (query_sketch.exact_simd_execution_stats_available
                       || query_sketch.bounded_nthash_execution_stats_available
                       || query_sketch.scalar_execution_stats_available
                       || collection.exact_simd_target_execution_stats_available
                       || collection.bounded_nthash_target_execution_stats_available
                       || (collection.scalar_pipeline_execution_stats_available
                           && collection.scalar_pipeline_execution_stats
                               .target_minimizers.calls != 0)) {
                bioseq_log<logging_level::error>(
                    "scalar/Vulkan segmented route emitted CPU simd sketch stats");
                std::abort();
            }
            return collection;
        };
    boost::unordered_flat_set<std::uint64_t> query_kmer_filter;
    query_kmer_filter.reserve(query_minimizers.size());
    for (const p_minimizer& minimizer : query_minimizers) {
        query_kmer_filter.insert(minimizer.kmer());
    }
    struct query_kmer_gate {
        std::vector<std::uint32_t> words;
        std::uint64_t mask = 0u;
        bool exact = false;

        void build(
            const boost::unordered_flat_set<std::uint64_t>& filter,
            std::size_t /*kmer_size*/) {
            const unsigned gate_bits = 26u;
            exact = false;
            if (filter.empty()) {
                words.clear();
                mask = 0u;
                return;
            }
            const std::uint64_t bit_count = std::uint64_t{1} << gate_bits;
            mask = bit_count - 1u;
            words.assign(static_cast<std::size_t>(bit_count / 32u), 0u);
            for (const std::uint64_t kmer : filter) {
                const std::uint64_t bit = kmer & mask;
                words[static_cast<std::size_t>(bit >> 5u)] |=
                    std::uint32_t{1} << (bit & 31u);
            }
        }

        [[nodiscard, gnu::always_inline]] bool maybe_contains(
            std::uint64_t kmer) const noexcept
        {
            if (words.empty()) return true;
            const std::uint64_t bit = kmer & mask;
            return (words[static_cast<std::size_t>(bit >> 5u)]
                    & (std::uint32_t{1} << (bit & 31u))) != 0u;
        }

        [[nodiscard, gnu::always_inline]] bool accepts_exactly(
            std::uint64_t /*kmer*/) const noexcept
        {
            return false;
        }

        [[nodiscard]] bioseq::gpu::vulkan::vulkan_sketch_query_gate
        vulkan_gate() const noexcept
        {
            return bioseq::gpu::vulkan::vulkan_sketch_query_gate{
                std::span<const std::uint32_t>{words.data(), words.size()},
                static_cast<std::uint32_t>(mask)};
        }
    };
    query_kmer_gate query_kmer_gate;
    if (target_sketch_backend == minimizer_sketch_backend::vulkan_segment) {
        query_kmer_gate.build(query_kmer_filter, k);
    }
    using filtered_minimizer_pair = std::pair<std::uint64_t, std::uint64_t>;
    auto build_query_filtered_segment_index =
        [&query_kmer_filter, &query_kmer_gate, target_len](
                std::span<const p_minimizer> minimizers,
                minimizer_hashmap_fragment_index& index,
                std::vector<filtered_minimizer_pair>& filtered_pairs,
                bool already_query_filtered = false) -> std::size_t {
        filtered_pairs.clear();
        const std::size_t filter_size = query_kmer_filter.size();
        const std::size_t reserve_base =
            filter_size > (std::numeric_limits<std::size_t>::max)() - 64u
                ? filter_size
                : filter_size + 64u;
        const std::size_t reserve_hint = std::min(minimizers.size(), reserve_base);
        if (filtered_pairs.capacity() < reserve_hint) {
            filtered_pairs.reserve(reserve_hint);
        }
        for (const p_minimizer& minimizer : minimizers) {
            const std::uint64_t kmer = minimizer.kmer();
            if (already_query_filtered
                || query_kmer_gate.accepts_exactly(kmer)
                || (query_kmer_gate.maybe_contains(kmer)
                    && query_kmer_filter.find(kmer) != query_kmer_filter.end())) {
                filtered_pairs.emplace_back(kmer, minimizer.pos_strand_encoded());
            }
        }
        index.build_from_pairs(
            std::span<filtered_minimizer_pair>{
                filtered_pairs.data(), filtered_pairs.size()},
            target_len);
        return filtered_pairs.size();
    };
    using query_kmer_filter_type = decltype(query_kmer_filter);
    struct query_filtered_pair_collector {
        using minimizer_type = p_minimizer;

        const query_kmer_filter_type* filter = nullptr;
        const query_kmer_gate* gate = nullptr;
        std::uint64_t position_offset = 0u;
        std::vector<filtered_minimizer_pair> pairs;

        void reserve(std::size_t expected_count) {
            const std::size_t filtered_hint = filter == nullptr
                ? expected_count
                : std::min<std::size_t>(expected_count, filter->size() + 64u);
            pairs.reserve(filtered_hint);
        }
        void clear() { pairs.clear(); }
        [[gnu::always_inline]] void collect(p_minimizer&& sketch) {
            collect(static_cast<const p_minimizer&>(sketch));
        }
        [[gnu::always_inline]] void collect(const p_minimizer& sketch) {
            const std::uint64_t kmer = sketch.kmer();
            if (filter == nullptr
                || ((gate == nullptr || gate->maybe_contains(kmer))
                    && filter->find(kmer) != filter->end())) {
                const std::uint64_t encoded =
                    ((position_offset + sketch.last_pos()) << 1u)
                    | static_cast<std::uint64_t>(sketch.strand());
                pairs.emplace_back(kmer, encoded);
            }
        }
        [[nodiscard]] bool append_raw_storage(
            const typename p_minimizer::storage_type* raw,
            std::size_t count) {
            if (count > pairs.max_size() - pairs.size()) {
                return false;
            }
            for (std::size_t index = 0; index < count; ++index) {
                collect(p_minimizer::from_raw_storage(raw[index]));
            }
            return true;
        }
    };

    // (2) Segment the target. Overlap >= longest target match span so a match
    //     starting in a segment core is fully indexed within that segment.
    constexpr std::size_t lpu = 1u;
    const sequence_size_t overlap_bp = pipe::segment_overlap_bp(query_len);

    auto run_serial_segmented =
        [target_len, query_len, lpu, w, k, &target_seq, &query_seq,
         &query_minimizers, &query_kmer_filter, &query_kmer_gate, &params, fragment_index_out,
         target_sketch_backend, segmented_gpu_index_cache_ptr,
         &segment_gpu_identity, &build_query_filtered_segment_index,
         &record_target_range_stats,
         &finalize_segmented_stats]() -> match_result_collection {
        match_result_collection merged_collection;
        std::vector<match_result>& merged = merged_collection.results;
        sketch_collector_t<p_minimizer> seg_collector;
        minimizer_hashmap_fragment_index seg_index;
        std::vector<filtered_minimizer_pair> filtered_pairs;
        const auto ranges =
            pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
        constexpr sequence_size_t frag_bp = 32768u;

        bioseq_log_lazy<logging_level::debug>([&] {
            bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] serial target_len={} query_len={} segments={} "
                            "| footprint={}MB avail={}MB",
                    target_len, query_len, ranges.size(),
                    bioseq::common::memory::process_footprint_bytes() >> 20,
                    bioseq::common::memory::system_available_bytes() >> 20);
        });

        if constexpr (!std::is_same_v<p_fragment_index_storage, std::nullptr_t>) {
            if (fragment_index_out != nullptr) {
                fragment_persist_accumulator<p_fragment_index_storage, p_minimizer> acc{
                    *fragment_index_out, target_len, frag_bp};
                for (std::size_t segment_index = 0;
                     segment_index < ranges.size();
                     ++segment_index) {
                    const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                    const auto [segment_lo_bp, segment_hi_bp] =
                        pipe::validated_segment_bp_bounds(
                            range, lpu, target_len);
                    seg_collector.clear();
                    seg_collector.reserve(
                        (range.ce_u - range.cs_u) * lpu
                            / std::max<std::size_t>(w, 1) + 64);
                    const auto range_stats =
                        pipe::minimizer_dispatch_range<p_minimizer>(
                        seg_collector, target_seq,
                        segment_lo_bp, segment_hi_bp, w, k,
                        target_sketch_backend,
                        pipe::exact_simd_range_route::require_continuation,
                        params.highway_kernels);
                    record_target_range_stats(merged_collection, range_stats);
                    const std::vector<p_minimizer>& seg_minimizers =
                        seg_collector.extract_sketches_data();

                    auto emissions = collect_fragment_emissions_for_segment(
                        range,
                        std::span<const p_minimizer>(
                            seg_minimizers.data(), seg_minimizers.size()),
                        target_len,
                        frag_bp,
                        lpu);
                    acc.ingest(std::span<const segment_fragment_emission<p_minimizer>>(
                        emissions.data(), emissions.size()));
                    acc.flush_ready(range.core_hi_bp);

                    build_query_filtered_segment_index(
                        std::span<const p_minimizer>{
                            seg_minimizers.data(), seg_minimizers.size()},
                        seg_index,
                        filtered_pairs);
                    seg_index.add_position_offset(segment_lo_bp);

                    match_result_collection seg_res = minimap2_port_with_index(
                        query_seq, target_seq, query_minimizers, seg_index, params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                    merged_collection.combine_execution_stats(seg_res);
                    for (auto& result : seg_res.results) {
                        if (result.target_start >= range.core_lo_bp
                            && result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                }
                acc.finish();
                return finalize_segmented_stats(std::move(merged_collection));
            }
        }

        query_filtered_pair_collector filtered_pair_collector{
            &query_kmer_filter, &query_kmer_gate};
        for (std::size_t segment_index = 0;
             segment_index < ranges.size();
             ++segment_index) {
            const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target_len);
            bioseq::indexing::gpu_index_cache_identity gpu_identity =
                segment_gpu_identity(segment_index);
            std::size_t seg_result_count = 0;
            if (target_sketch_backend == minimizer_sketch_backend::vulkan_segment) {
                filtered_pair_collector.position_offset = segment_lo_bp;
                filtered_pair_collector.clear();
                filtered_pair_collector.reserve(
                    (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                bioseq::gpu::vulkan::collect_minimizers_vulkan_segment_or_throw<p_minimizer>(
                    filtered_pair_collector,
                    target_seq,
                    segment_lo_bp,
                    segment_hi_bp,
                    w,
                    k,
                    query_kmer_gate.vulkan_gate());
                // Safe with the approximate device gate because the collector
                // still exact-confirms each emitted kmer against query_kmer_filter.
                seg_index.build_from_pairs(
                    std::span<filtered_minimizer_pair>{
                        filtered_pair_collector.pairs.data(),
                        filtered_pair_collector.pairs.size()},
                    target_len);

                match_result_collection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, seg_index, params,
                    segmented_gpu_index_cache_ptr,
                    gpu_identity);
                merged_collection.combine_execution_stats(seg_res);
                seg_result_count = seg_res.results.size();
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            } else {
                seg_collector.clear();
                seg_collector.reserve(
                    (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                const auto range_stats =
                    pipe::minimizer_dispatch_range<p_minimizer>(
                    seg_collector, target_seq,
                    segment_lo_bp, segment_hi_bp, w, k,
                    target_sketch_backend,
                    pipe::exact_simd_range_route::require_continuation,
                    params.highway_kernels);
                record_target_range_stats(merged_collection, range_stats);
                const std::vector<p_minimizer>& seg_minimizers =
                    seg_collector.extract_sketches_data();
                build_query_filtered_segment_index(
                    std::span<const p_minimizer>{seg_minimizers.data(), seg_minimizers.size()},
                    seg_index,
                    filtered_pairs);
                seg_index.add_position_offset(segment_lo_bp);

                match_result_collection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, seg_index, params,
                    segmented_gpu_index_cache_ptr,
                    gpu_identity);
                merged_collection.combine_execution_stats(seg_res);
                seg_result_count = seg_res.results.size();
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            }
            bioseq_log_lazy<logging_level::debug>([&] {
                bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] segment core [{},{}) done, kept {} of {} "
                                "(merged {}) | footprint={}MB avail={}MB",
                        range.core_lo_bp, range.core_hi_bp,
                        merged.size(), seg_result_count, merged.size(),
                        bioseq::common::memory::process_footprint_bytes() >> 20,
                        bioseq::common::memory::system_available_bytes() >> 20);
            });
        }
        return finalize_segmented_stats(std::move(merged_collection));
    };

    auto run_vulkan_segment_coordinate_batch = [&]() -> match_result_collection {
        auto& runtime = bioseq::gpu::vulkan::runtime::instance();
        if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
            pipe::throw_vulkan_declined("vulkan_segment resident coordinate batch");
        }
        const auto ranges =
            pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
        std::span<const p_minimizer> sketch_span{
            query_minimizers.data(), query_minimizers.size()};
        const bioseq::gpu::vulkan::buffer* device_words =
            (query_minimizers.device && query_minimizers.device->words() != nullptr
             && query_minimizers.device->count() == sketch_span.size())
                ? query_minimizers.device->words()
                : nullptr;

        std::vector<std::shared_ptr<bioseq::indexing::gpu_resident_minimizer_index>>
            residents;
        std::vector<bioseq::gpu::vulkan::gpu_seed_coordinate_job> jobs;
        std::vector<std::size_t> job_segment_indices;
        residents.reserve(ranges.size());
        jobs.reserve(ranges.size());
        job_segment_indices.reserve(ranges.size());

        const auto frozen_query_gate = pipe::freeze_gpu_query_gate(
            query_kmer_gate.vulkan_gate());
        const auto frozen_gate_identity =
            frozen_query_gate.canonical_identity();

        for (std::size_t segment_index = 0;
             segment_index < ranges.size();
             ++segment_index) {
            const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
            const auto [segment_lo_bp, segment_hi_bp] =
                pipe::validated_segment_bp_bounds(range, lpu, target_len);
            bioseq::gpu::vulkan::vulkan_sketch_decline decline{};
            auto built = bioseq::gpu::vulkan::build_gpu_segment_index_on_device<p_minimizer>(
                target_seq,
                segment_lo_bp,
                segment_hi_bp,
                w,
                k,
                frozen_query_gate,
                &decline);
            if (!built) {
                pipe::throw_vulkan_sketch_declined(
                    "vulkan_segment GPU resident index build", decline);
            }
            if (!built->valid || built->position_count == 0u || built->occupied_slots == 0u) {
                continue;
            }

            auto identity = segment_gpu_identity(segment_index);
            bioseq::indexing::gpu_minimizer_index_cache_key key{
                .source_id = 0u,
                .caller_identity = identity.assembly_identity,
                .target_len = built->target_len,
                .table_size = built->occupied_slots,
                .position_count = built->position_count,
            };
            pipe::bind_verified_gpu_segment_gate_identity(
                key, frozen_gate_identity, built->gate_identity);
            auto adopted = bioseq::indexing::adopt_gpu_built_index(
                runtime, std::move(*built), key);
            if (!adopted) {
                throw bioseq::gpu::vulkan::vulkan_runtime_error(fmt::format(
                    "vulkan_segment GPU resident index adoption rejected ({})",
                    bioseq::indexing::gpu_built_index_adoption_error_name(
                        adopted.error())));
            }
            auto resident = std::move(*adopted);
            resident->target_position_offset = segment_lo_bp;
            residents.push_back(resident);
            jobs.push_back(bioseq::gpu::vulkan::gpu_seed_coordinate_job{
                .slots = &resident->slots,
                .positions = &resident->positions,
                .target_len = resident->target_len,
                .slot_mask = resident->slot_mask,
                .position_count = resident->position_count,
                .bucket_count = resident->bucket_count,
                .target_position_offset = resident->target_position_offset,
            });
            job_segment_indices.push_back(segment_index);
        }
        if (jobs.empty()) {
            return match_result_collection{};
        }

        auto cp = params.make_chaining_parameters();
        std::int32_t max_dist_x = cp.max_gap;
        std::int32_t max_dist_y = cp.max_gap;
        if (max_dist_x < cp.bandwidth) max_dist_x = cp.bandwidth;
        if (max_dist_y < cp.bandwidth) max_dist_y = cp.bandwidth;
        const auto chain_key =
            bioseq::chaining::choose_vulkan_chaining_pipeline(runtime.features());
        bioseq::gpu::vulkan::gpu_seed_query_inputs query_inputs{
            .host_minimizer_bytes = device_words != nullptr
                ? std::span<const std::byte>{}
                : std::as_bytes(sketch_span),
            .device_minimizer_words = device_words,
            .minimizer_count = static_cast<std::uint32_t>(sketch_span.size()),
            .minimizer_word_kind =
                static_cast<std::uint32_t>(
                    bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_word_kind),
            .minimizer_words32_per_record =
                bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_words32_per_minimizer,
            .hash_shift =
                bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_hash_shift,
            .pos_bits =
                bioseq::gpu::vulkan::minimizer_wire_traits<p_minimizer>::k_position_bits,
        };
        const auto coords = runtime.run_seed_to_coordinate_batch_roundtrip(
            query_inputs,
            std::span<const bioseq::gpu::vulkan::gpu_seed_coordinate_job>{
                jobs.data(), jobs.size()},
            query_len,
            static_cast<std::uint32_t>(params.kmer_size),
            params.max_occ,
            bioseq::gpu::vulkan::gpu_seed_coordinate_algorithm::
                vulkan_tiled_dp,
            chain_key,
            max_dist_x,
            max_dist_y,
            cp.bandwidth,
            std::min<std::int32_t>(cp.max_chain_iter, 3000),
            cp.gap_penalty,
            cp.skip_penalty,
            cp.max_chain_skip,
            cp.min_chain_score,
            cp.min_seed_count,
            16u);

        std::vector<match_result> merged;
        merged.reserve(coords.size());
        for (std::size_t job_index = 0; job_index < coords.size(); ++job_index) {
            const auto& coord = coords[job_index];
            if (coord.valid == 0u) continue;
            const ::bioseq::indexing::segment_unit_range& range =
                ranges[job_segment_indices[job_index]];
            if (coord.target_start >= range.core_lo_bp
                && coord.target_start < range.core_hi_bp) {
                merged.push_back(
                    pipe::gpu_coordinate_to_match_result(coord, query_len, target_len));
            }
        }
        return match_result_collection{std::move(merged)};
    };

    if (target_sketch_backend == minimizer_sketch_backend::vulkan_segment
        && pipe::is_gpu_coordinate_only_execution_active(params)
        && fragment_index_out == nullptr) {
        return finalize_segmented_stats(
            run_vulkan_segment_coordinate_batch());
    }

    auto& pool = bioseq::parallelism::global_pool::instance();
    if (pipe::vulkan_accelerator_requested(params)
        || !bioseq::parallelism::has_parallel_tasks(pool.get_mode())
        || pool.maximum_concurrency() <= 1
        || target_sketch_backend == minimizer_sketch_backend::vulkan_segment
        || !lazy_is_worker_shareable(query_seq)
        || !lazy_is_worker_shareable(target_seq)) {
        return run_serial_segmented();
    }

    struct segment_wave_admission {
        unsigned fanout = 1;
        unsigned geometry_divisor = 1;
        sequence_size_t segment_bp = 0;
        std::size_t estimated_segments = 0;
        std::size_t estimated_waves = 0;
        std::uint64_t peak_per_segment = 0;
        bioseq::parallelism::memory_permit permit;
        bool is_admitted = false;
    };

    constexpr std::uint64_t context_reserve = std::uint64_t{8} << 20;
    auto collected_bp_upper_bound =
        [target_len, overlap_bp](sequence_size_t core_bp) -> std::uint64_t {
        const std::uint64_t core = static_cast<std::uint64_t>(core_bp);
        const std::uint64_t overlap = static_cast<std::uint64_t>(overlap_bp);
        const std::uint64_t expanded =
            bioseq::common::saturating_mul(2, overlap);
        const std::uint64_t collected =
            core > (std::numeric_limits<std::uint64_t>::max)() - expanded
                ? (std::numeric_limits<std::uint64_t>::max)()
                : core + expanded;
        return std::min<std::uint64_t>(
            collected, static_cast<std::uint64_t>(target_len));
    };

    auto peak_for_segment_core_bp =
        [&collected_bp_upper_bound](sequence_size_t core_bp) {
        return bioseq::parallelism::minimizer_index_peak_bytes_for_bp(
            collected_bp_upper_bound(core_bp)) + context_reserve;
    };

    struct segment_wave_candidate {
        unsigned fanout = 1;
        unsigned geometry_divisor = 1;
        sequence_size_t segment_bp = 0;
        std::size_t estimated_segments = 0;
        std::size_t estimated_waves = 0;
        std::uint64_t peak_per_segment = 0;
        std::uint64_t reserve_bytes = 0;
        std::uint64_t estimated_work_bp = 0;
    };

    auto try_admit_segment_geometry =
        [&pool, target_len, &collected_bp_upper_bound,
         &peak_for_segment_core_bp]() -> segment_wave_admission {
        auto& admission =
            bioseq::parallelism::memory_admission_controller::instance();
        const unsigned max_fanout = pool.maximum_concurrency();
        if (max_fanout <= 1) {
            return segment_wave_admission{};
        }

        const std::uint64_t allocatable =
            admission.allocatable_after_reservations();
        std::vector<segment_wave_candidate> candidates;
        candidates.reserve(static_cast<std::size_t>(max_fanout));

        for (unsigned fanout = max_fanout; fanout > 1; --fanout) {
            const unsigned geometry_divisor = fanout;
            const sequence_size_t bp =
                pipe::segment_bp_from_budget_divided(geometry_divisor);
            const std::uint64_t collected_bp = collected_bp_upper_bound(bp);
            const std::size_t estimated_segments =
                static_cast<std::size_t>(
                    (target_len + bp - 1) / std::max<sequence_size_t>(bp, 1));
            const std::uint64_t peak = peak_for_segment_core_bp(bp);

            const std::uint64_t reserve_bytes =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(fanout), peak);
            if (reserve_bytes > allocatable) {
                continue;
            }

            const std::size_t estimated_waves =
                (estimated_segments + fanout - 1) / fanout;
            const std::uint64_t estimated_work =
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(estimated_waves),
                    collected_bp);
            candidates.push_back(segment_wave_candidate{
                fanout,
                geometry_divisor,
                bp,
                estimated_segments,
                estimated_waves,
                peak,
                reserve_bytes,
                estimated_work});
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const segment_wave_candidate& lhs, const segment_wave_candidate& rhs) {
                if (lhs.estimated_work_bp != rhs.estimated_work_bp) {
                    return lhs.estimated_work_bp < rhs.estimated_work_bp;
                }
                if (lhs.estimated_segments != rhs.estimated_segments) {
                    return lhs.estimated_segments < rhs.estimated_segments;
                }
                if (lhs.fanout != rhs.fanout) {
                    return lhs.fanout > rhs.fanout;
                }
                return lhs.geometry_divisor < rhs.geometry_divisor;
            });

        for (const segment_wave_candidate& candidate : candidates) {
            if (auto permit = admission.try_reserve(candidate.reserve_bytes)) {
                return segment_wave_admission{
                    candidate.fanout,
                    candidate.geometry_divisor,
                    candidate.segment_bp,
                    candidate.estimated_segments,
                    candidate.estimated_waves,
                    candidate.peak_per_segment,
                    std::move(*permit),
                    true};
            }
        }

        return segment_wave_admission{};
    };

    auto first_wave = try_admit_segment_geometry();
    if (!first_wave.is_admitted || first_wave.fanout <= 1) {
        return run_serial_segmented();
    }

    bioseq_log_lazy<logging_level::debug>([&] {
        bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] admitted parallel geometry fanout={} "
                        "divisor={} segment_bp={} estimated_segments={} "
                        "estimated_waves={} peak_per_segment={}MB reserve={}MB "
                        "| footprint={}MB avail={}MB",
                first_wave.fanout, first_wave.geometry_divisor,
                first_wave.segment_bp, first_wave.estimated_segments,
                first_wave.estimated_waves, first_wave.peak_per_segment >> 20,
                first_wave.permit.bytes() >> 20,
                bioseq::common::memory::process_footprint_bytes() >> 20,
                bioseq::common::memory::system_available_bytes() >> 20);
    });

    const unsigned geometry_divisor = first_wave.geometry_divisor;
    auto ranges = pipe::segment_unit_ranges(target_len, query_len, lpu, geometry_divisor);
    std::vector<match_result_collection> seg_out(ranges.size());
    bioseq::parallelism::task_parallel_scope scope(
        bioseq::parallelism::task_parallel_region::segment_recompute);
    if (!scope.acquired()) {
        return run_serial_segmented();
    }

    if constexpr (!std::is_same_v<p_fragment_index_storage, std::nullptr_t>) {
        if (fragment_index_out != nullptr) {
            constexpr sequence_size_t frag_bp = 32768u;
            fragment_persist_accumulator<p_fragment_index_storage, p_minimizer> acc{
                *fragment_index_out, target_len, frag_bp};
            match_result_collection merged_collection;
            std::vector<match_result>& merged = merged_collection.results;

            auto process_segment_serial =
                [&ranges, &target_seq, &query_seq, &query_minimizers, &params,
                 &acc, &merged_collection, &merged, target_len, frag_bp, w, k, lpu,
                 target_sketch_backend, segmented_gpu_index_cache_ptr,
                 &segment_gpu_identity, &record_target_range_stats](
                        std::size_t segment_index) {
                sketch_collector_t<p_minimizer> collector;
                minimizer_hashmap_fragment_index index;
                const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                const auto [segment_lo_bp, segment_hi_bp] =
                    pipe::validated_segment_bp_bounds(
                        range, lpu, target_len);
                collector.reserve(
                    (range.ce_u - range.cs_u) * lpu
                        / std::max<std::size_t>(w, 1) + 64);
                const auto range_stats =
                    pipe::minimizer_dispatch_range<p_minimizer>(
                    collector, target_seq,
                    segment_lo_bp, segment_hi_bp, w, k,
                    target_sketch_backend,
                    pipe::exact_simd_range_route::require_continuation,
                    params.highway_kernels);
                record_target_range_stats(merged_collection, range_stats);
                const std::vector<p_minimizer>& minimizers =
                    collector.extract_sketches_data();
                auto emissions = collect_fragment_emissions_for_segment(
                    range,
                    std::span<const p_minimizer>(minimizers.data(), minimizers.size()),
                    target_len,
                    frag_bp,
                    lpu);
                acc.ingest(std::span<const segment_fragment_emission<p_minimizer>>(
                    emissions.data(), emissions.size()));
                acc.flush_ready(range.core_hi_bp);

                index.build(minimizers, target_len);
                index.add_position_offset(segment_lo_bp);
                match_result_collection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, index, params,
                    segmented_gpu_index_cache_ptr,
                    segment_gpu_identity(segment_index));
                merged_collection.combine_execution_stats(seg_res);
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            };

            for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
                const std::size_t wave_count = std::min<std::size_t>(
                    first_wave.fanout, ranges.size() - wave_lo);
                bioseq::parallelism::memory_permit wave_permit;
                if (wave_lo == 0) {
                    wave_permit = std::move(first_wave.permit);
                } else if (auto permit =
                        bioseq::parallelism::memory_admission_controller::instance()
                            .try_reserve(
                                bioseq::common::saturating_mul(
                                    static_cast<std::uint64_t>(wave_count),
                                    first_wave.peak_per_segment))) {
                    wave_permit = std::move(*permit);
                }
                if (!wave_permit) {
                    for (std::size_t segment_index = wave_lo;
                         segment_index < ranges.size();
                         ++segment_index) {
                        process_segment_serial(segment_index);
                    }
                    wave_lo = ranges.size();
                    break;
                }

                const std::size_t current_wave_lo = wave_lo;
                const std::size_t wave_hi = wave_lo + wave_count;
                std::vector<match_result_collection> wave_seg_out(wave_count);
                std::vector<std::vector<segment_fragment_emission<p_minimizer>>>
                    wave_frag_out(wave_count);
                pool.parallel_for_with_context_tracked(
                    wave_lo, wave_hi, 1,
                    bioseq::parallelism::range_body{
                        [&ranges, &wave_seg_out, &wave_frag_out,
                         &target_seq, &query_seq, &query_minimizers, &params,
                         current_wave_lo, target_len, frag_bp, w, k, lpu,
                         target_sketch_backend, segmented_gpu_index_cache_ptr,
                         &segment_gpu_identity, &record_target_range_stats](
                                std::size_t lo, std::size_t hi) {
                        sketch_collector_t<p_minimizer> collector;
                        minimizer_hashmap_fragment_index index;
                        for (std::size_t segment_index = lo;
                             segment_index < hi;
                             ++segment_index) {
                            const ::bioseq::indexing::segment_unit_range& range =
                                ranges[segment_index];
                            const auto [segment_lo_bp, segment_hi_bp] =
                                pipe::validated_segment_bp_bounds(
                                    range, lpu, target_len);
                            const std::size_t rel = segment_index - current_wave_lo;
                            collector.clear();
                            collector.reserve(
                                (range.ce_u - range.cs_u) * lpu
                                    / std::max<std::size_t>(w, 1) + 64);
                            const auto range_stats =
                                pipe::minimizer_dispatch_range<p_minimizer>(
                                collector, target_seq,
                                segment_lo_bp, segment_hi_bp, w, k,
                                target_sketch_backend,
                                pipe::exact_simd_range_route::require_continuation,
                                params.highway_kernels);
                            const std::vector<p_minimizer>& minimizers =
                                collector.extract_sketches_data();
                            wave_frag_out[rel] =
                                collect_fragment_emissions_for_segment(
                                    range,
                                    std::span<const p_minimizer>(
                                        minimizers.data(), minimizers.size()),
                                    target_len,
                                    frag_bp,
                                    lpu);
                            index.build(minimizers, target_len);
                            index.add_position_offset(segment_lo_bp);
                            wave_seg_out[rel] = minimap2_port_with_index(
                                query_seq,
                                target_seq,
                                query_minimizers,
                                index,
                                params,
                                segmented_gpu_index_cache_ptr,
                                segment_gpu_identity(segment_index));
                            record_target_range_stats(
                                wave_seg_out[rel], range_stats);
                        }
                    }},
                    bioseq::parallelism::current_tracked_job_observer());

                for (std::size_t rel = 0; rel < wave_count; ++rel) {
                    const std::size_t segment_index = current_wave_lo + rel;
                    const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                    acc.ingest(std::span<const segment_fragment_emission<p_minimizer>>(
                        wave_frag_out[rel].data(), wave_frag_out[rel].size()));
                    acc.flush_ready(range.core_hi_bp);
                    merged_collection.combine_execution_stats(
                        wave_seg_out[rel]);
                    for (auto& result : wave_seg_out[rel].results) {
                        if (result.target_start >= range.core_lo_bp
                            && result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                }
                wave_lo = wave_hi;
            }

            acc.finish();
            return finalize_segmented_stats(std::move(merged_collection));
        }
    }

    std::vector<std::size_t> segment_minimizer_counts(ranges.size());
    std::vector<std::size_t> segment_filtered_counts(ranges.size());

    auto process_segment_serial =
        [&ranges, &seg_out, &target_seq, &query_seq, &query_minimizers,
         &segment_minimizer_counts, &segment_filtered_counts,
         &build_query_filtered_segment_index, &params, target_len,
         w, k, lpu, target_sketch_backend, segmented_gpu_index_cache_ptr,
         &segment_gpu_identity,
         &record_target_range_stats](std::size_t segment_index) {
        sketch_collector_t<p_minimizer> collector;
        minimizer_hashmap_fragment_index index;
        std::vector<filtered_minimizer_pair> filtered_pairs;
        const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
        const auto [segment_lo_bp, segment_hi_bp] =
            pipe::validated_segment_bp_bounds(range, lpu, target_len);
        collector.reserve(
            (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
        const auto range_stats =
            pipe::minimizer_dispatch_range<p_minimizer>(
            collector, target_seq,
            segment_lo_bp, segment_hi_bp, w, k,
            target_sketch_backend,
            pipe::exact_simd_range_route::require_continuation,
            params.highway_kernels);
        const std::vector<p_minimizer>& minimizers =
            collector.extract_sketches_data();
        segment_minimizer_counts[segment_index] = minimizers.size();
        segment_filtered_counts[segment_index] =
            build_query_filtered_segment_index(
                std::span<const p_minimizer>{minimizers.data(), minimizers.size()},
                index,
                filtered_pairs);
        index.add_position_offset(segment_lo_bp);
        seg_out[segment_index] = minimap2_port_with_index(
            query_seq, target_seq, query_minimizers, index, params,
            segmented_gpu_index_cache_ptr,
            segment_gpu_identity(segment_index));
        record_target_range_stats(seg_out[segment_index], range_stats);
        bioseq_log_lazy<logging_level::debug>([&] {
            bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] adaptive serial segment core [{},{}) "
                            "produced {} results | footprint={}MB avail={}MB",
                    range.core_lo_bp, range.core_hi_bp,
                    seg_out[segment_index].results.size(),
                    bioseq::common::memory::process_footprint_bytes() >> 20,
                    bioseq::common::memory::system_available_bytes() >> 20);
        });
    };

    for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
        const std::size_t requested_wave_count = std::min<std::size_t>(
            first_wave.fanout, ranges.size() - wave_lo);
        std::size_t wave_count = requested_wave_count;
        bioseq::parallelism::memory_permit wave_permit;
        if (wave_lo == 0) {
            wave_permit = std::move(first_wave.permit);
            wave_permit.shrink_to(
                bioseq::common::saturating_mul(
                    static_cast<std::uint64_t>(wave_count),
                    first_wave.peak_per_segment));
        } else {
            for (std::size_t candidate = requested_wave_count; candidate > 1; --candidate) {
                if (auto permit =
                        bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                            bioseq::common::saturating_mul(
                                static_cast<std::uint64_t>(candidate),
                                first_wave.peak_per_segment))) {
                    wave_count = candidate;
                    wave_permit = std::move(*permit);
                    break;
                }
            }
        }
        if (!wave_permit) {
            bioseq_log_lazy<logging_level::debug>([&] {
                bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] wave admission failed at segment {} "
                                "of {}; processing one segment serially | footprint={}MB "
                                "avail={}MB",
                        wave_lo, ranges.size(),
                        bioseq::common::memory::process_footprint_bytes() >> 20,
                        bioseq::common::memory::system_available_bytes() >> 20);
            });
            process_segment_serial(wave_lo);
            ++wave_lo;
            continue;
        }
        if (wave_count != requested_wave_count) {
            bioseq_log_lazy<logging_level::debug>([&] {
                bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] wave fanout reduced from {} to {} "
                                "at segment {} of {} | footprint={}MB avail={}MB",
                        requested_wave_count, wave_count, wave_lo, ranges.size(),
                        bioseq::common::memory::process_footprint_bytes() >> 20,
                        bioseq::common::memory::system_available_bytes() >> 20);
            });
        }

        const std::size_t wave_hi = wave_lo + wave_count;
        pool.parallel_for_with_context_tracked(
            wave_lo, wave_hi, 1,
            bioseq::parallelism::range_body{
                [&ranges, &seg_out, &target_seq, &query_seq, &query_minimizers,
                 &segment_minimizer_counts, &segment_filtered_counts,
                 &build_query_filtered_segment_index, &params, target_len,
                 w, k, lpu, target_sketch_backend, segmented_gpu_index_cache_ptr,
                 &segment_gpu_identity,
                 &record_target_range_stats](std::size_t lo, std::size_t hi) {
                sketch_collector_t<p_minimizer> collector;
                minimizer_hashmap_fragment_index index;
                std::vector<filtered_minimizer_pair> filtered_pairs;
                for (std::size_t segment_index = lo; segment_index < hi; ++segment_index) {
                    const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
                    const auto [segment_lo_bp, segment_hi_bp] =
                        pipe::validated_segment_bp_bounds(
                            range, lpu, target_len);
                    collector.clear();
                    collector.reserve(
                        (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                    const auto range_stats =
                        pipe::minimizer_dispatch_range<p_minimizer>(
                        collector, target_seq,
                        segment_lo_bp, segment_hi_bp, w, k,
                        target_sketch_backend,
                        pipe::exact_simd_range_route::require_continuation,
                        params.highway_kernels);
                    const std::vector<p_minimizer>& minimizers =
                        collector.extract_sketches_data();
                    segment_minimizer_counts[segment_index] = minimizers.size();
                    segment_filtered_counts[segment_index] =
                        build_query_filtered_segment_index(
                            std::span<const p_minimizer>{
                                minimizers.data(), minimizers.size()},
                            index,
                            filtered_pairs);
                    index.add_position_offset(segment_lo_bp);
                    seg_out[segment_index] = minimap2_port_with_index(
                        query_seq, target_seq, query_minimizers, index, params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                    record_target_range_stats(
                        seg_out[segment_index], range_stats);
                }
            }},
            bioseq::parallelism::current_tracked_job_observer());
        wave_lo = wave_hi;
    }

    std::size_t total_segment_minimizers = 0;
    std::size_t total_filtered_minimizers = 0;
    for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
        total_segment_minimizers += segment_minimizer_counts[segment_index];
        total_filtered_minimizers += segment_filtered_counts[segment_index];
    }
    bioseq_log_lazy<logging_level::debug>([&] {
        bioseq_log_progress<logging_level::debug>("[minimap_port_segmented] query-filtered transient index kept {} "
                        "of {} segment minimizers across {} segments | footprint={}MB "
                        "avail={}MB",
                total_filtered_minimizers, total_segment_minimizers, ranges.size(),
                bioseq::common::memory::process_footprint_bytes() >> 20,
                bioseq::common::memory::system_available_bytes() >> 20);
    });

    match_result_collection merged_collection;
    std::vector<match_result>& merged = merged_collection.results;
    for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
        const ::bioseq::indexing::segment_unit_range& range = ranges[segment_index];
        merged_collection.combine_execution_stats(seg_out[segment_index]);
        for (auto& result : seg_out[segment_index].results) {
            if (result.target_start >= range.core_lo_bp
                && result.target_start < range.core_hi_bp) {
                merged.push_back(std::move(result));
            }
        }
    }
    return finalize_segmented_stats(std::move(merged_collection));
}

} // namespace bioseq::simd

} // export
