/**
 * @file pipe.cppm
 * @brief Templated minimap_port pipeline API v2 (DOC0249).
 *
 * This module implements the constexpr-dispatched stage pipeline described in
 * `TEMPLATED_MINIMAP_PORT_PIPELINE_API_V2_CONSTEXPR_DISPATCHED_STAGES_AND_METADATA__DOC0249.md`.
 * It is a parallel API layer that sits above the existing kernel functions in
 * `bioseq::simd::N_NEON` and provides:
 *
 *   - Compile-time parameter packs (`ConstexprAlignerParams<W,K,L>` /
 *     `ConstexprAlignerFallbackParams<L>`).
 *   - Stage skeletons (`BareStage<Self>`, `StageBase<Self,Out>`,
 *     `Stage<Cep,Self,Sink>`).
 *   - `make_pipeline<Cep>` variadic composer.
 *   - `BatchT<E,StorageKind>` storage wrappers backed by `std::span` or the
 *     per-pipeline arena context.
 *   - Concrete stages — every pipeline phase exposed as a stand-alone struct
 *     whose `process<Cep>` body forwards to the existing kernel function
 *     (collect_seed_hits, chain_anchors_*, generate_regions, etc.).
 *   - Top-level `dispatch_pipeline(...)` cascade that fixes (Cep, BAA, CA,
 *     MA, EP) at compile time before driving the pipeline.
 *   - `PipelineDescription` metadata surface for the Postgres EXPLAIN
 *     renderer.
 *
 * The pipeline calls into the existing kernels directly; no kernel rename is
 * performed here.  Stages adapt span-based I/O to the kernel's `std::vector`
 * signatures by materialising the kernel's output into arena-backed slabs
 * after the kernel returns.  The wire type between stages is `std::span<E>`
 * (with mutating stages taking non-const elements and read-only stages
 * taking const).
 */
module;

// FIXME: This should be put inside some build-time config
/**
Special value that controls template instantiation explosion:
level 0: one kmer bucket length, always assume int64 L of sequence
level 1: one kmer bucket length
level 2: default kmer bucket lengths
level 3: allow SpecCep specialisations

General rule:
- Lower level means faster build, smaller BMI
- Higher level means exponentially slower build, larger executable binary size, but more efficient runtime code execution

For testing/benchmarking faster iteration you can use level 1, level 3 requires massive hour-long builds
*/
#define PIPELINE_TEMPLATE_BRANCHING_LEVEL 0

// DOC0256 §R17 — the chain primitive (`pipe::detail::dispatch_chain`) relies
// on tree-inlining of [[gnu::always_inline]] recursive instantiations.  Both
// gcc and clang honour the attribute at -O1+, but downgrade silently at -O0.
// Build configs that compile this module without optimisation produce
// stack-spilled adaptor frames that defeat the point of the cascade; refuse
// to compile in that mode.
#if !defined(__OPTIMIZE__)
# error "DOC0256 chain primitive requires -O1 or higher (see DOC0256 §R17). " \
        "Use -Og or -O1 -g for debug builds of pipe.cppm."
#endif

#include <hwy/highway.h>

#include "common.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <concepts>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>

export module bioseq.sequences.pipe;

import bioseq.common.mlogging;
import bioseq.common.templating;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.two_stacks;
import bioseq.sequences.cigar;
import bioseq.alignment;
import bioseq.alignment.utils;
import bioseq.alignment.lane_helpers;
import bioseq.alignment.ksw2_batched;
import bioseq.alignment.block_aligner;
import bioseq.sequences.chaining.utils;
import bioseq.sequences.chaining.scalar;
import bioseq.sequences.chaining.simd;
import bioseq.sequences.chaining.rmq;
import bioseq.sequences.chaining.vulkan;
import bioseq.sequences.minimap_port;
import bioseq.common.context;
import bioseq.instr;
import bioseq.storage.scanner;
import bioseq.storage.lazy;
import bioseq.storage.size;   // DOC0263 §10 — sequence_size_t for the C1 truncation audit
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;   // DOC0279 §9 — relocated MinimizerHashmapFragmentIndex (was TargetIndex)
import bioseq.indexing.minimizer_index;     // DOC0279 §7.1 — relocated MinimizerIndex concept
import bioseq.gpu.vulkan.runtime;           // Runtime::instance() for GPU sketch/seed paths
import bioseq.sequences.kmers.vulkan;       // DOC0351 Phase C — Vulkan minimizer sketch + device span
import bioseq.indexing.minimizer_vulkan;    // DOC0307 Phase B — GPU-resident index view + cache
import bioseq.sequences.seeding.vulkan;     // DOC0307 Phase B — Vulkan seed-hit lookup
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE::pipe {
// DOC0279 §9 — bring the relocated index into this namespace so the prior
// `TargetIndex` spellings resolve to `MinimizerHashmapFragmentIndex`.
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerIndex;

// DOC0263 §10.1 — pipeline lengths/positions use the 64-bit sequence_size_t.
using bioseq::storage::sequence_size_t;

[[noreturn]] inline void throw_vulkan_declined(std::string_view stage) {
    auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
    const std::string detail = runtime.usable()
        ? "GPU path declined"
        : fmt::format("runtime unavailable: {}", runtime.initialization_error());
    throw bioseq::gpu::vulkan::VulkanRuntimeError(
        fmt::format("{} selected but Vulkan execution did not run ({})", stage, detail));
}

// DOC0256 §2.1 — the chain primitive is load-bearing on relaxed
// template-template parameter matching (P0522R0) and C++20 class-type
// NTTPs (carrying `auto...` packs that interleave enum NTTPs with
// `std::size_t`).  Guard the build mode so a misconfigured build fails
// loudly rather than producing subtly-wrong codegen.
static_assert(__cplusplus >= 202302L,
    "DOC0256 requires C++23 — see DOC0256 §2.1.");

// Import types from the surrounding modules so the stage bodies can refer
// to them by short name.
using ::bioseq::chaining::Anchor;
using ::bioseq::chaining::Chain;
using ::bioseq::chaining::ChainingParams;
using ::bioseq::chaining::ArenaSlab;
using ::bioseq::chaining::make_arena_slab;
using ::bioseq::chaining::chain_anchors_scalar;
using ::bioseq::chaining::chain_anchors_rmq;

// =============================================================================
// 4. Compile-time parameter packs (DOC0249 §4)
// =============================================================================

/**
 * @brief Type-level parameter pack for a fully specialised pipeline instance.
 *
 * @tparam W           Minimizer window size (must be > 0).
 * @tparam K           K-mer length (must match the MinimizerRepr's KmerLen).
 * @tparam SeqLenLog2  ceil(log2(max(query_bp, target_bp))).  Selects the
 *                     `MinimizerRepr<SeqLenLog2, K>` storage bucket — same
 *                     dispatch already happens at runtime in `minimap_port`.
 */
template <std::size_t W, std::size_t K, std::size_t SeqLenLog2>
struct ConstexprAlignerParams {
    static constexpr std::size_t window_len        = W;
    static constexpr std::size_t kmer_len          = K;
    static constexpr std::size_t sequence_len_log2 = SeqLenLog2;
    static constexpr bool        generic_fallback  = false;

    using minimizer_repr_t = ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<SeqLenLog2, K>;

    [[nodiscard]] static constexpr std::string_view label() noexcept {
        return "specialised";
    }
};

/**
 * @brief Fallback parameter pack for non-canonical (W, K).
 *
 * Stages branch on `generic_fallback` to read window/kmer from runtime params
 * instead of the type-level constants.
 */
template <std::size_t SeqLenLog2>
struct ConstexprAlignerFallbackParams {
    static constexpr std::size_t window_len        = 0;
    static constexpr std::size_t kmer_len          = 0;
    static constexpr std::size_t sequence_len_log2 = SeqLenLog2;
    static constexpr bool        generic_fallback  = true;

    using minimizer_repr_t = ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<SeqLenLog2, 15u>;

    [[nodiscard]] static constexpr std::string_view label() noexcept {
        return "fallback(generic w/k)";
    }
};

template <class T>
concept is_constexpr_aligner_params = requires {
    { T::window_len }        -> std::convertible_to<std::size_t>;
    { T::kmer_len }          -> std::convertible_to<std::size_t>;
    { T::sequence_len_log2 } -> std::convertible_to<std::size_t>;
    { T::generic_fallback }  -> std::convertible_to<bool>;
    typename T::minimizer_repr_t;
    { T::label() }           -> std::convertible_to<std::string_view>;
};

static_assert(is_constexpr_aligner_params<ConstexprAlignerParams<10u, 15u, 28u>>);
static_assert(is_constexpr_aligner_params<ConstexprAlignerFallbackParams<28u>>);

/**
 * @brief Pipeline-builder entry-point tag (DOC0249 §9.4).
 *
 * `WithPrebuiltMinimizers` — pipeline accepts a `std::span<const MinimizerT>`
 * directly; the IndexStage is at the head.
 * `FromSequence` — pipeline accepts the query sequence range and a
 * `MinimizerStage` is front-loaded to sketch on the fly.
 */
enum class EntryPoint : std::uint8_t {
    WithPrebuiltMinimizers,
    FromSequence,
};

// =============================================================================
// 5/6. Concepts and Stage skeleton (DOC0249 §5, §6)
// =============================================================================

namespace detail {
    template <class S>
    concept IsBareStage = requires { typename S::bare_stage_marker; };
}

/**
 * @brief Base tag inherited by every concrete stage.
 *
 * Lets `make_pipeline` distinguish bare-stage arguments from the terminal
 * sink via the `bare_stage_marker` typedef.
 */
template <class Derived>
struct BareStage {
    using bare_stage_marker = void;
};

// Forward declaration so StageBase::attach can refer to Stage.
template <is_constexpr_aligner_params Cep, class Self, class Sink>
struct Stage;

// Metadata types (defined fully in §11; forward-declared here so describe()
// has a return type).
struct StageDescription;
struct PipelineDescription;

// =============================================================================
// DOC0307 §5.2 — narrow minimizer-batch shape carrying an optional device span.
// =============================================================================
//
// One additional stage-shape on top of the DOC0254 matrix. It borrows the host
// minimizers from the arena slab and owns the optional GPU device minimizer
// handle through `VulkanDeviceMinimizerSpan::owner`, so the all-Vulkan
// sketch+seed path avoids a second query-minimizer upload. CPU minimizer
// kernels keep using the existing `std::span<const MinimizerT>` path.
template <class MinimizerT>
struct MinimizerBatchView {
    using value_type = MinimizerT;

    std::span<const MinimizerT> host{};
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>> device{};

    [[nodiscard]] const MinimizerT* data() const noexcept { return host.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return host.size(); }
    [[nodiscard]] std::span<const MinimizerT> as_span() const noexcept { return host; }
};

template <class T>
struct is_minimizer_batch_view : std::false_type {};
template <class M>
struct is_minimizer_batch_view<MinimizerBatchView<M>> : std::true_type {};
template <class T>
inline constexpr bool is_minimizer_batch_view_v =
    is_minimizer_batch_view<std::remove_cvref_t<T>>::value;

// =============================================================================
// DOC0254 §7 — Concept-driven sink validation.
// =============================================================================

/**
 * @brief Single-batch sink concept.  Required by EmitToken::emit_batch.
 *
 * Matched by every concrete sink that consumes a `std::span<E>` (most
 * pipeline tails).  Substitution failures inside `EmitToken::emit_batch`
 * name the concept and the missing method.
 */
template <class S, class E>
concept BatchSink = requires (S& sink, std::span<E> batch) {
    { sink.on_batch(batch) } -> std::same_as<void>;
    { sink.on_completed() }  -> std::same_as<void>;
};

/**
 * @brief Pair-batch sink concept.  Required by EmitToken::emit_batch_pair.
 *
 * The pair shape is used by the chain → region → primary/secondary →
 * filter → divergence → align cascade where each stage forwards both the
 * regions span and the (read-only) anchors span downstream.
 */
template <class S, class E1, class E2>
concept PairBatchSink = requires (S& sink, std::span<E1> a, std::span<E2> b) {
    { sink.on_batch_pair(a, b) } -> std::same_as<void>;
    { sink.on_completed() }      -> std::same_as<void>;
};

/**
 * @brief Index-decorated batch sink concept.  Required by
 * EmitToken::emit_batch_with_index.
 *
 * Used by IndexStage to hand a sketches span downstream alongside a
 * (const-reference) target index.  SeedHitStage is the canonical consumer.
 */
template <class S, class E, class IndexT>
concept IndexedBatchSink = requires (S& sink, std::span<E> a, const IndexT& idx,
                                     bioseq::indexing::GpuIndexCacheIdentity identity) {
    { sink.on_batch_with_index(a, idx, identity) } -> std::same_as<void>;
    { sink.on_completed() }                        -> std::same_as<void>;
};

/// DOC0307 §5.2 — minimizer-batch sink (MinimizerStage → IndexStage hand-off).
template <class S, class MinimizerT>
concept MinimizerBatchSink = requires (S& sink, MinimizerBatchView<MinimizerT> batch) {
    { sink.on_minimizer_batch(batch) } -> std::same_as<void>;
    { sink.on_completed() }            -> std::same_as<void>;
};

/// DOC0307 §5.2 — index-decorated minimizer-batch sink (IndexStage →
/// SeedHitStage hand-off carrying the GPU index cache identity).
template <class S, class MinimizerT, class IndexT>
concept IndexedMinimizerBatchSink =
requires (S& sink, MinimizerBatchView<MinimizerT> batch, const IndexT& idx,
          bioseq::indexing::GpuIndexCacheIdentity identity) {
    { sink.on_minimizer_batch_with_index(batch, idx, identity) } -> std::same_as<void>;
    { sink.on_completed() }                                      -> std::same_as<void>;
};

// =============================================================================
// DOC0254 §4 — Compile-time single-emission enforcement (EmitToken /
// EmittedToken).
// =============================================================================

template <class Sink> class EmittedToken;

/**
 * @brief Move-only, single-use, friend-constructible-only token granting
 * downstream-emission permission.  The `emit_batch*` family of
 * rvalue-qualified methods consumes the token (nulling its internal
 * pointer) and returns the friend-only-constructible `EmittedToken<Sink>`
 * proof.
 *
 * The token's only constructor is private; only `Stage<Cep, Self, Sink>`
 * is befriended, which means concrete-stage `process*<Cep>` bodies see
 * the token by rvalue reference but cannot manufacture one themselves —
 * the type system rules out skipping the emit (no way to construct the
 * required `EmittedToken<Sink>` return value).
 *
 * Single-emission enforcement (DOC0254 R7):
 *   - At least once: HARD compile-time.  `process*<Cep>` must return
 *     `EmittedToken<Sink>`; the only way to construct one is to call
 *     `EmitToken<Sink>::emit_batch*`.
 *   - At most once: soft compile-time + runtime trip-wire.  `[[nodiscard]]`
 *     makes the ergonomic shape `return std::move(token).emit_batch*(...)`;
 *     emitting twice trips the `DEBUG_ASSERT(sink_ptr_ != nullptr)` in debug.
 */
template <class Sink>
class EmitToken {
public:
    template <is_constexpr_aligner_params, class, class> friend struct Stage;

    EmitToken()                            = delete;
    EmitToken(const EmitToken&)            = delete;
    EmitToken(EmitToken&&)                 = delete;
    EmitToken& operator=(const EmitToken&) = delete;
    EmitToken& operator=(EmitToken&&)      = delete;

    template <class E>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_batch(std::span<E> data) && {
        static_assert(BatchSink<Sink, E>,
            "EmitToken::emit_batch: downstream sink does not implement "
            "on_batch(std::span<E>). See DOC0249 §8.12 stage-shape matrix.");
        DEBUG_ASSERT(sink_ptr_ != nullptr,
            "EmitToken consumed twice; the first emit_batch* call nulled the "
            "sink pointer (see R7 in DOC0254 §3).");
        sink_ptr_->on_batch(data);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    template <class E1, class E2>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_batch_pair(std::span<E1> first_batch,
                                       std::span<E2> second_batch) && {
        static_assert(PairBatchSink<Sink, E1, E2>,
            "EmitToken::emit_batch_pair: downstream sink does not implement "
            "on_batch_pair(std::span<E1>, std::span<E2>).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_batch_pair(first_batch, second_batch);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    template <class E, class IndexT>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_batch_with_index(
        std::span<E>  data,
        const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) && {
        static_assert(IndexedBatchSink<Sink, E, IndexT>,
            "EmitToken::emit_batch_with_index: downstream sink does not "
            "implement on_batch_with_index(std::span<E>, IndexT const&, identity).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_batch_with_index(data, index_ref, identity);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    // DOC0307 §5.2 — narrow minimizer-batch emit (carries optional device span).
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_minimizer_batch(MinimizerBatchView<MinimizerT> batch) && {
        static_assert(MinimizerBatchSink<Sink, MinimizerT>,
            "EmitToken::emit_minimizer_batch: downstream sink does not implement "
            "on_minimizer_batch.");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_minimizer_batch(batch);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

    template <class MinimizerT, class IndexT>
    [[nodiscard, gnu::always_inline]]
    EmittedToken<Sink> emit_minimizer_batch_with_index(
        MinimizerBatchView<MinimizerT> batch,
        const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) && {
        static_assert(IndexedMinimizerBatchSink<Sink, MinimizerT, IndexT>,
            "EmitToken::emit_minimizer_batch_with_index: downstream sink does not "
            "implement on_minimizer_batch_with_index.");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_minimizer_batch_with_index(batch, index_ref, identity);
        sink_ptr_ = nullptr;
        return EmittedToken<Sink>::make();
    }

private:
    Sink* sink_ptr_;
    explicit constexpr EmitToken(Sink& sink_ref) noexcept
        : sink_ptr_(&sink_ref) {}
};

/**
 * @brief Proof-of-emission token.  Returned by
 * `EmitToken<Sink>::emit_batch*`; every concrete stage's `process*<Cep>`
 * must return one.  The only path to constructing one is the
 * friend-accessible `make()` factory.
 */
template <class Sink>
class [[nodiscard]] EmittedToken {
public:
    EmittedToken(const EmittedToken&)             = delete;
    EmittedToken& operator=(const EmittedToken&)  = delete;
    EmittedToken& operator=(EmittedToken&&)       = delete;

    /// Move constructor exists only for mandatory-copy-elision worst-case
    /// materialisation.  The canonical path is direct-return from
    /// emit_batch*, where C++17 copy elision skips it entirely.
    EmittedToken(EmittedToken&&) noexcept = default;

private:
    friend class EmitToken<Sink>;
    /// Friend-only default constructor — the public surface is
    /// non-default-constructible because the only call site of this
    /// ctor lives behind the `make()` factory below.
    constexpr EmittedToken() noexcept = default;
    [[nodiscard, gnu::always_inline]] static constexpr EmittedToken make() noexcept {
        return EmittedToken{};
    }
};

// =============================================================================
// DOC0254 §5 — Input-side kernel-to-arena adapter.
// =============================================================================

/**
 * @brief Drop-in `sketch_collector_t<E>`-shaped adapter that routes the
 * kernel's `collect(...)` traffic into a caller-provided
 * `ArenaSlab<E>`.  Implements the minimal surface the kernels rely on
 * (`collect(E&&)`, `collect(const E&)`, `reserve(size_t)`).
 *
 * `ArenaSlab<E>::push_back` requires `std::is_trivially_copyable_v<E>` —
 * satisfied by every `MinimizerRepr<L, K>`.  The existing slab
 * `static_assert` (`chaining_utils.cppm:236`) covers the constraint and
 * would fire if a non-trivial minimizer type were ever introduced.
 */
template <class E>
struct sketch_collector_arena_adapter {
    ::bioseq::chaining::ArenaSlab<E>* slab_;

    [[gnu::always_inline]] void collect(E sketch) {
        slab_->push_back(std::move(sketch));
    }
    [[gnu::always_inline]] void collect(const E& sketch) {
        slab_->push_back(sketch);
    }
    [[gnu::always_inline]] void reserve(std::size_t expected_count) {
        slab_->reserve(expected_count);
    }
};

/**
 * @brief CRTP helper absorbing the boilerplate shared by every concrete stage.
 *
 * `Derived` must define `static constexpr std::string_view stage_name`.  The
 * default `attach<Cep, Sink>(...)` returns a bound `Stage<Cep, Derived, Sink>`
 * value owning `*this` and the downstream sink.
 */
template <class Derived, class Out>
struct StageBase : BareStage<Derived> {
    using out_element_type = Out;

    template <is_constexpr_aligner_params Cep, class Sink>
    [[gnu::always_inline]] auto attach(Sink sink) noexcept {
        return Stage<Cep, Derived, Sink>{
            std::move(sink), std::move(static_cast<Derived&>(*this))};
    }

    template <class Sink>
    [[gnu::always_inline]] void finalize(Sink&) noexcept {
        /* default: no-op */
    }
};

/**
 * @brief Bound stage — owns its downstream sink and bare-stage config by value.
 *
 * `Cep` is fixed at `make_pipeline<Cep>(...)` time and identical across every
 * stage in a given pipeline instance.  The bound stage's `on_batch*` members
 * forward to the bare stage's `process*<Cep>` body; both are
 * `[[gnu::always_inline]]` so the chain collapses to a straight-line sequence
 * of kernel calls at `-O3`.
 */
template <is_constexpr_aligner_params Cep, class Self, class Sink>
struct Stage {
    using cep_t = Cep;

    Sink sink_;
    Self self_;

    constexpr Stage(Sink sink_arg, Self self_arg) noexcept
        : sink_(std::move(sink_arg)), self_(std::move(self_arg)) {}

    // DOC0254 §4.2: the bound stage constructs an `EmitToken<Sink>` on its
    // own stack and hands it to `process*<Cep>` by rvalue reference; the
    // concrete-stage body returns the (discarded) `EmittedToken<Sink>` proof
    // type whose construction is friend-private to `EmitToken<Sink>`.
    template <class B>
    [[gnu::always_inline]] void on_batch(B&& batch) {
        (void) self_.template process<Cep>(
            std::forward<B>(batch), EmitToken<Sink>{sink_});
    }

    template <class B1, class B2>
    [[gnu::always_inline]] void on_batch_pair(B1&& first_batch, B2&& second_batch) {
        (void) self_.template process_pair<Cep>(
            std::forward<B1>(first_batch), std::forward<B2>(second_batch),
            EmitToken<Sink>{sink_});
    }

    template <class B, class IndexT>
    [[gnu::always_inline]] void on_batch_with_index(
        B&& batch, const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) {
        (void) self_.template process_with_index<Cep>(
            std::forward<B>(batch), index_ref, identity, EmitToken<Sink>{sink_});
    }

    // DOC0307 §5.2 — forwarders for the narrow minimizer-batch shape.
    template <class MinimizerT>
    [[gnu::always_inline]] void on_minimizer_batch(MinimizerBatchView<MinimizerT> batch) {
        (void) self_.template process<Cep>(batch, EmitToken<Sink>{sink_});
    }

    template <class MinimizerT, class IndexT>
    [[gnu::always_inline]] void on_minimizer_batch_with_index(
        MinimizerBatchView<MinimizerT> batch, const IndexT& index_ref,
        bioseq::indexing::GpuIndexCacheIdentity identity) {
        (void) self_.template process_with_index<Cep>(
            batch, index_ref, identity, EmitToken<Sink>{sink_});
    }

    [[gnu::always_inline]] void on_completed() noexcept {
        self_.finalize(sink_);
        sink_.on_completed();
    }

    [[nodiscard]] PipelineDescription describe() const;
};

/**
 * @brief Variadic pipeline composer — fixes `Cep` once for the whole chain.
 *
 * The base case (terminal sink) returns the sink unchanged; the recursive case
 * peels one bare stage off the front, builds the downstream pipeline from the
 * remaining arguments, and attaches.
 */
template <is_constexpr_aligner_params Cep, class Sink>
[[gnu::always_inline]] constexpr auto make_pipeline(Sink sink) noexcept {
    static_assert(!detail::IsBareStage<Sink>,
        "make_pipeline's last argument must be a terminal sink, "
        "not a BareStage.  Append your terminal sink at the end.");
    return sink;
}

template <is_constexpr_aligner_params Cep, class Bare0, class... Rest>
    requires detail::IsBareStage<Bare0>
[[gnu::always_inline]] constexpr auto make_pipeline(Bare0 head, Rest... rest) noexcept {
    auto downstream = make_pipeline<Cep>(std::move(rest)...);
    return std::move(head).template attach<Cep>(std::move(downstream));
}

// =============================================================================
// 7. Batch storage — owning arena slab (DOC0249 §7)
// =============================================================================
//
// `ArenaSlab<E>` itself lives in `bioseq_sequences.chaining.utils` so the
// chaining kernels (chain_anchors_*) and the seed/region kernels in
// `minimap_port.cppm` can take it as an output parameter — see DOC0249 §13
// follow-up "output-side kernel-to-arena refactor".  The `using` declarations
// at the top of this namespace re-export both the slab type and the
// `make_arena_slab<E>()` helper so stage bodies refer to them by short name.

// =============================================================================
// 11. Metadata surface (DOC0249 §11) — forward types for algorithm_traits
// =============================================================================
//
// The full `StageDescription` / `PipelineDescription` definitions live after
// the algorithm_traits block (they reference `NamedProperty` too).  The
// `NamedProperty` POD itself is the only piece the traits' `computed_properties_default()`
// signature needs, so we hoist it here ahead of the traits.

struct NamedProperty {
    std::string name;
    std::string value;
};

// =============================================================================
// 8.0 Kernel-trait pattern (DOC0249 §8.0, R5)
// =============================================================================
//
// `algorithm_traits<AlgoEnumValue>` maps an algorithm enum value to
//   - a `kernel_type` struct exposing the call surface used by the stage,
//   - a `static constexpr std::string_view algorithm_label()` for EXPLAIN,
//   - a `static std::vector<NamedProperty> computed_properties_default()`
//     for the EXPLAIN renderer's `[name: value]` properties.
//
// Stages take the `kernel_type` as a template argument (via `kernel_t<X>`)
// and call `Kernel::call(...)` once — no `if constexpr` ladders inside the
// stage body.  Adding a new algorithm is a single trait specialisation in
// the kernel's own module.

/// Dependent-false helper so the primary template's `static_assert` fires
/// only on instantiation (not at point of definition).
template <auto>
inline constexpr bool no_kernel_for_v = false;

/// Primary template — unspecialised.  Substitution failure here produces a
/// readable diagnostic naming the offending enum value.
///
/// Specialisations live below for `ChainingAlgorithmType` and
/// `MinimizerAlgorithmType`; `BaseAlignmentAlgorithmType` is still dispatched
/// inside `AlignStage::process_pair` via `align_regions<BAA>` because that
/// kernel's signature is templated on both `BAA` and the per-stage sequence
/// ranges `(QR, TR)` — a trait specialisation would still need to forward
/// `QR`/`TR`, so the direct call is preferred for clarity.  A future
/// iteration can move `AlignStage` to the trait pattern once the kernel
/// surface is refactored to take a uniform-shape output adapter.
template <auto AlgoEnumValue>
struct algorithm_traits {
    static_assert(no_kernel_for_v<AlgoEnumValue>,
        "algorithm_traits<X>: no specialisation for this algorithm enum value. "
        "Declare one in pipe.cppm next to the ChainingAlgorithmType / "
        "MinimizerAlgorithmType blocks.");
};

/// Helper alias — `kernel_t<X>` gives the kernel type directly.
template <auto AlgoEnumValue>
using kernel_t = typename algorithm_traits<AlgoEnumValue>::kernel_type;

// ---------------------------------------------------------------------------
// 8.0.1 ChainingAlgorithmType kernel traits
// ---------------------------------------------------------------------------

/// Scalar DP chaining — `chain_anchors_scalar` from
/// `bioseq_sequences.chaining.scalar`.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SCALAR_MM2> {
    struct kernel_type {
        [[gnu::always_inline]] static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out)
        {
            ::bioseq::chaining::chain_anchors_scalar(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SCALAR_MM2);
        }
        static void populate_runtime_params(
            const ChainingParams&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "false"},
                {"complexity", "O(n * max_iter)"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// SIMD DP chaining — `chain_anchors_simd` from
/// `bioseq_sequences.chaining.simd` (lives in `bioseq::simd::HWY_NAMESPACE`).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SIMD_MM2> {
    struct kernel_type {
        [[gnu::always_inline]] static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out)
        {
            ::bioseq::simd::HWY_NAMESPACE::chain_anchors_simd(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::SIMD_MM2);
        }
        static void populate_runtime_params(
            const ChainingParams&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"requires_zero_skip_penalty", "true"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// RMQ tree-based chaining — `chain_anchors_rmq` from
/// `bioseq_sequences.chaining.rmq`.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::RMQ> {
    struct kernel_type {
        [[gnu::always_inline]] static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out)
        {
            ::bioseq::chaining::chain_anchors_rmq(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::RMQ);
        }
        static void populate_runtime_params(
            const ChainingParams&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "false"},
                {"complexity", "O(n log n)"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Vulkan tiled-DP chaining (DOC0304 Phase A) — `chain_anchors_vulkan` from
/// `bioseq_sequences.chaining.vulkan`. G3SA-style tiled DP score generation on
/// Vulkan + bioseq CPU `backtrack_chains`; selected Vulkan execution is
/// mandatory and failures throw `VulkanRuntimeError`.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP> {
    struct kernel_type {
        [[gnu::always_inline]] static void call(
            std::span<const Anchor> anchors,
            const ChainingParams&   params,
            ArenaSlab<Chain>&       chains_out,
            ArenaSlab<Anchor>&      reordered_anchors_out)
        {
            ::bioseq::chaining::chain_anchors_vulkan(
                anchors, params, chains_out, reordered_anchors_out);
        }
        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP);
        }
        static void populate_runtime_params(
            const ChainingParams& params,
            std::vector<NamedProperty>& props) noexcept
        {
            props.push_back({"phase", "A"});
            props.push_back({"fallback", "none"});
            props.push_back({"uses_max_chain_skip", "false"});
            props.push_back({"vulkan_enabled", params.vulkan_enabled ? "true" : "false"});
        }
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"accelerator", "vulkan"},
                {"score_generation", "g3sa-tiled-dp"},
                {"backtrack", "bioseq-cpu"},
                {"uses_max_chain_skip", "false"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// ---------------------------------------------------------------------------
// DOC0263 — Scanner-aware minimizer dispatch.
// For a plain compact_sequence_range or a no-fragment lazy wrapper, runs the
// requested in-RAM kernel (scalar / simd / two-stack) on contiguous storage
// (zero overhead — the no-fragment lazy forwards to its underlying range). For
// a fragmented lazy wrapper, streams fragments through the bit-identical scalar
// kernel (`collect_minimizers_view`) — see DOC0263 §8.4 for why the scalar path
// is used instead of the SIMD lane-replay.
// ---------------------------------------------------------------------------
enum class MinKernelKind { Scalar, Simd, TwoStack };

// DOC0286 §10.2 — the SIMD minimizer kernels (single-stack and two-stack
// nthash) are 2-bit specialisations (dna4 + uint64); a 3-bit alphabet such as
// dna5 has no SIMD lane layout and the two-stack kernel `DEBUG_ASSERT(false)`s
// for it. Route any non-2-bit alphabet to the bit-identical, nt4-correct
// scalar kernel regardless of the requested SIMD `Kind` — this is the
// "must be routed to the scalar fallback" the two-stack kernel expects, and
// it makes the forced `minimizer_algorithm=two_stack_nthash` GUC dna5-safe.
template <MinKernelKind Kind, class Seq>
inline constexpr MinKernelKind effective_min_kernel() {
    using seq_traits = typename std::remove_cvref_t<Seq>::traits;
    return (Kind != MinKernelKind::Scalar && seq_traits::bits_per_char != 2)
               ? MinKernelKind::Scalar
               : Kind;
}

template <MinKernelKind Kind, class MinimizerT, class Seq, class Collector>
[[gnu::always_inline]] inline void run_minimizer_inram(
    Collector& c, Seq& seq, std::size_t w, std::size_t k)
{
    // The legacy minimizer kernels take a non-const compact_sequence_range& but
    // only ever read it;  here may be a const& (e.g. lazy.underlying()).
    auto& m = const_cast<std::remove_cvref_t<Seq>&>(seq);
    constexpr MinKernelKind EffKind = effective_min_kernel<Kind, Seq>();
    if constexpr (EffKind == MinKernelKind::Scalar) {
        ::bioseq::simd::HWY_NAMESPACE::collect_minimizers<minimap_int_hasher, MinimizerT>(
            c, m, m.begin(), m.end(), w, k);
    } else if constexpr (EffKind == MinKernelKind::Simd) {
        ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_simd<minimap_int_hasher, MinimizerT>(
            c, m, m.begin(), m.end(), w, k);
    } else {
        ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_two_stack_nthash_simd<minimap_int_hasher, MinimizerT>(
            c, m, m.begin(), m.end(), w, k);
    }
}

template <MinKernelKind Kind, std::size_t W, std::size_t K, class MinimizerT, class Seq, class Collector>
[[gnu::always_inline]] inline void run_minimizer_inram_constexpr(Collector& c, Seq& seq)
{
    auto& m = const_cast<std::remove_cvref_t<Seq>&>(seq);
    constexpr MinKernelKind EffKind = effective_min_kernel<Kind, Seq>();
    if constexpr (EffKind == MinKernelKind::Scalar) {
        ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_constexpr<W, K, minimap_int_hasher, MinimizerT>(
            c, m, m.begin(), m.end());
    } else if constexpr (EffKind == MinKernelKind::Simd) {
        ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_simd_constexpr<W, K, minimap_int_hasher, MinimizerT>(
            c, m, m.begin(), m.end());
    } else {
        ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_two_stack_nthash_simd_constexpr<W, K, minimap_int_hasher, MinimizerT>(
            c, m, m.begin(), m.end());
    }
}

template <MinKernelKind Kind, class MinimizerT, class Seq, class Collector>
[[gnu::always_inline]] inline void minimizer_dispatch(
    Collector& c, Seq& seq, std::size_t w, std::size_t k)
{
    if constexpr (requires { typename Seq::scanner_type; }) {
        if constexpr (!Seq::scanner_type::supports_fragments()) {
            run_minimizer_inram<Kind, MinimizerT>(c, seq.underlying(), w, k);
        } else {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_view<minimap_int_hasher, MinimizerT>(
                c, seq, seq.begin(), seq.end(), w, k);
        }
    } else {
        run_minimizer_inram<Kind, MinimizerT>(c, seq, w, k);
    }
}

template <MinKernelKind Kind, std::size_t W, std::size_t K, class MinimizerT, class Seq, class Collector>
[[gnu::always_inline]] inline void minimizer_dispatch_constexpr(Collector& c, Seq& seq, std::size_t w, std::size_t k)
{
    if constexpr (requires { typename Seq::scanner_type; }) {
        if constexpr (!Seq::scanner_type::supports_fragments()) {
            run_minimizer_inram_constexpr<Kind, W, K, MinimizerT>(c, seq.underlying());
        } else {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_view<minimap_int_hasher, MinimizerT>(
                c, seq, seq.begin(), seq.end(), w, k);
        }
    } else {
        run_minimizer_inram_constexpr<Kind, W, K, MinimizerT>(c, seq);
    }
}

// DOC0276 — SUB-RANGE minimizer collection over storage units `[lo_unit, hi_unit)`.
// Used by `minimap_port_segmented` to sketch one target SEGMENT at a time,
// forwarding explicit begin/end iterators (advanced to the unit-aligned segment
// bounds) so positions come out LOCAL to `lo_unit*lpu`.
//
// IMPORTANT: this ALWAYS uses the bit-identical SCALAR `collect_minimizers_view`,
// even for contiguous (no-fragment) sequences. The SIMD two-stack kernel derives
// its iteration length from the WHOLE sequence (`calculate_iteration_offsets` over
// `seq.bp_size()`), not from `(end - begin)`, so handing it a sub-range start/end
// makes it read past the requested window — garbage minimizers + an out-of-bounds
// crash (observed: a 206 Mbp segment of a 500 Mbp target reported 99.7M minimizers
// — more than the WHOLE sequence — then SIGSEGV). The scalar view honours
// `seq_begin/seq_end` exactly and emits `seq_pos` from 0 at `seq_begin`, which is
// what the globalisation (`TargetIndex::add_position_offset`) and the scalar/SIMD
// parity both rely on. Scalar collection over contiguous memory is cheap (no I/O;
// alignment dominates the per-segment wall-clock), so this is correct AND fast.
// DOC0345 Patch D — no default backend. The scalar sub-range body now lives in
// `collect_minimizers_segment_scalar` (bioseq.sequences.kmers) so pipe and the
// persisted builders share one (bug-carrying) sub-range contract. Under
// `VulkanSegment` the GPU segment sketch is called and throws on any decline.
template <class MinimizerT, class Seq, class Collector>
[[gnu::always_inline]] inline void minimizer_dispatch_range(
    Collector& collector,
    Seq& seq,
    sequence_size_t lo_unit,
    sequence_size_t hi_unit,
    std::size_t window_size,
    std::size_t kmer_size,
    MinimizerSketchBackend backend)
{
    if (backend == MinimizerSketchBackend::VulkanSegment) {
        bioseq::gpu::vulkan::collect_minimizers_vulkan_segment_or_throw<MinimizerT>(
            collector,
            seq,
            static_cast<std::size_t>(lo_unit),
            static_cast<std::size_t>(hi_unit),
            window_size,
            kmer_size);
        return;
    }

    collect_minimizers_segment_scalar<MinimizerT>(
        collector,
        seq,
        lo_unit,
        hi_unit,
        window_size,
        kmer_size);
}

// DOC0345 Patch D — derive the target-index sketch backend from the resolved
// params. Vulkan only when all three high-level gates are on; otherwise scalar.
[[nodiscard]] inline MinimizerSketchBackend
target_index_sketch_backend_from_params(const Minimap2Params& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm == MinimizerAlgorithmType::VULKAN_MINIMAP_INT
            ? MinimizerSketchBackend::VulkanSegment
            : MinimizerSketchBackend::Scalar;
}

// DOC0276 — segment GEOMETRY. The MinimapSketchData position field is 28 bits
// (MinimizerRepr<28,15>), so a segment's COLLECTED span — core + an overlap on
// each side — must stay within that field, or the global position handed back by
// `add_position_offset` would be silently masked by the packing ctor
// (`pos_v & pos_field_mask`, index.cppm) and corrupt the seed. We cap the core at
// 200 Mbp and each overlap at 30 Mbp; the static_assert ties both to the REAL
// field width so neither can be bumped past 2^28 without a compile error.
inline constexpr sequence_size_t kSegmentCoreMaxBp    = 200'000'000ull;  // core ceiling
inline constexpr sequence_size_t kSegmentOverlapMaxBp =  30'000'000ull;  // per-side overlap ceiling
inline constexpr sequence_size_t kSegmentMinBp        =  32'000'000ull;  // floor (keep batches useful)
inline constexpr sequence_size_t kVulkanSegmentMinBp   =   1'000'000ull;  // floor when GPU sketch is selected
// DOC0353 continuation: 76M/96M is the best measured consolidation point. The
// earlier 100M/128M experiment made the inner sketch admission shrink and exposed
// a multi-window correctness bug; 96M keeps the collected span below that failed
// shape while cutting the 300M probe from ~44 GPU windows to ~6 when
// memory admission accepts a one-window sketch.
inline constexpr sequence_size_t kVulkanSegmentTargetBp = 76'000'000ull;  // target core before query-overlap clamping
inline constexpr sequence_size_t kVulkanSegmentCollectedMaxBp =
    96'000'000ull;
static_assert(kSegmentCoreMaxBp + 2 * kSegmentOverlapMaxBp <=
                  static_cast<sequence_size_t>(MinimapSketchData::pos_field_mask),
              "segment collected span (core + 2*overlap) must fit "
              "MinimapSketchData's 28-bit position field");

// Choose a target-index SEGMENT size (bp) from the live memory budget. A full
// minimizer index over an N-bp target costs ~6 B/bp at PEAK (collector vector
// 8 B + flat_map bucket ~16 B/distinct + positions 8 B, all × density ~0.19).
// On a 16 GB box a 6 Gbp full index (~37 GB) swaps; capping a segment to the
// budget keeps the live index ~one segment, clamped to [kSegmentMinBp,
// kSegmentCoreMaxBp].
[[nodiscard]] inline sequence_size_t segment_bp_from_budget() noexcept {
    const std::uint64_t budget = bioseq::memory::default_budget().allocatable_bytes();
    // allocatable_bytes() == UINT64_MAX when the mach/POSIX probe is unavailable;
    // dividing keeps it huge so the clamp yields kSegmentCoreMaxBp (the default).
    const std::uint64_t by_budget =
        budget / bioseq::parallelism::kMinimizerIndexPeakBytesPerBp;
    const sequence_size_t want =
        (by_budget > kSegmentCoreMaxBp) ? kSegmentCoreMaxBp
                                        : static_cast<sequence_size_t>(by_budget);
    return want < kSegmentMinBp ? kSegmentMinBp : want;
}

[[nodiscard]] inline sequence_size_t segment_bp_from_budget_divided(
        unsigned divisor,
        sequence_size_t floor_bp = kSegmentMinBp) noexcept {
    const sequence_size_t base = segment_bp_from_budget();
    const sequence_size_t divided = divisor <= 1 ? base : base / divisor;
    return divided < floor_bp ? floor_bp : divided;
}

[[nodiscard]] inline sequence_size_t segment_overlap_bp(
        sequence_size_t query_len) noexcept {
    sequence_size_t overlap_bp = query_len + query_len / 4 + 100'000;
    if (overlap_bp > kSegmentOverlapMaxBp) {
        overlap_bp = kSegmentOverlapMaxBp;
    }
    return overlap_bp;
}

[[nodiscard]] inline sequence_size_t vulkan_segment_target_bp(
        sequence_size_t query_len) noexcept {
    const sequence_size_t overlap_bp = segment_overlap_bp(query_len);
    const sequence_size_t expanded_overlap =
        overlap_bp > kVulkanSegmentCollectedMaxBp / 2
            ? kVulkanSegmentCollectedMaxBp
            : overlap_bp * 2;
    if (expanded_overlap + kVulkanSegmentMinBp
        >= kVulkanSegmentCollectedMaxBp) {
        return kVulkanSegmentMinBp;
    }
    const sequence_size_t room =
        kVulkanSegmentCollectedMaxBp - expanded_overlap;
    const sequence_size_t target = std::min(room, kVulkanSegmentTargetBp);
    return target < kVulkanSegmentMinBp ? kVulkanSegmentMinBp : target;
}

[[nodiscard]] inline unsigned
segment_geometry_divisor_for_backend(
        MinimizerSketchBackend backend,
        sequence_size_t query_len = 0) noexcept {
    if (backend != MinimizerSketchBackend::VulkanSegment) {
        return 1u;
    }
    const sequence_size_t base = segment_bp_from_budget();
    const sequence_size_t target = vulkan_segment_target_bp(query_len);
    const sequence_size_t divisor =
        (base + target - sequence_size_t{1}) / target;
    return static_cast<unsigned>(std::max<sequence_size_t>(1, divisor));
}

[[nodiscard]] inline sequence_size_t
segment_core_floor_for_backend(MinimizerSketchBackend backend) noexcept {
    return backend == MinimizerSketchBackend::VulkanSegment
        ? kVulkanSegmentMinBp
        : kSegmentMinBp;
}

// DOC0279 §11.3a — the segment geometry of `minimap_port_segmented`, extracted
// so `dispatch_buffer`'s stored-index assemble loop reuses the EXACT recompute
// geometry. NOT a pure function of its args: it reads the runtime budget via
// `segment_bp_from_budget()` exactly as recompute does. Each entry covers a
// segment's overlapped collection range `[cs_u, ce_u)` (in units) and its core
// claim band `[core_lo_bp, core_hi_bp)`.
struct SegmentUnitRange {
    sequence_size_t cs_u;          ///< collection start (units, overlap-included)
    sequence_size_t ce_u;          ///< collection end   (units, overlap-included)
    sequence_size_t core_lo_bp;    ///< core claim lower bound (bp)
    sequence_size_t core_hi_bp;    ///< core claim upper bound (bp, clamped to total_bp)
};
[[nodiscard]] inline std::vector<SegmentUnitRange> segment_unit_ranges(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        unsigned divisor,
        sequence_size_t floor_bp) {
    std::vector<SegmentUnitRange> out;
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
        out.push_back(SegmentUnitRange{cs_u, ce_u, core_lo_bp, core_hi_bp});
    }
    return out;
}

[[nodiscard]] inline std::vector<SegmentUnitRange> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu, unsigned divisor) {
    return segment_unit_ranges(total_bp, query_len, lpu, divisor, kSegmentMinBp);
}

[[nodiscard]] inline std::vector<SegmentUnitRange> segment_unit_ranges(
        sequence_size_t total_bp, sequence_size_t query_len, std::size_t lpu) {
    return segment_unit_ranges(total_bp, query_len, lpu, 1);
}

[[nodiscard]] inline std::vector<SegmentUnitRange> segment_unit_ranges_for_backend(
        sequence_size_t total_bp,
        sequence_size_t query_len,
        std::size_t lpu,
        MinimizerSketchBackend backend) {
    return segment_unit_ranges(
        total_bp,
        query_len,
        lpu,
        segment_geometry_divisor_for_backend(backend, query_len),
        segment_core_floor_for_backend(backend));
}

// ---------------------------------------------------------------------------
// 8.0.2 MinimizerAlgorithmType kernel traits
// ---------------------------------------------------------------------------
//
// The minimizer kernels are templated on the sketch-collector and the
// sequence range, both of which depend on the MinimizerStage's `MinimizerT`
// and `QR` template parameters.  The trait's `call` method is therefore a
// member template — the stage instantiates it with the correct types at the
// call site.

/// Scalar minimizer extraction.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR> {
    struct kernel_type {
        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<MinKernelKind::Scalar, MinimizerT>(
                collector, range, window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range)
        {
            minimizer_dispatch_constexpr<MinKernelKind::Scalar, W, K, MinimizerT>(
                collector, range, W, K);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "false"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Reference SIMD minimizer extraction (single-stack).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW> {
    struct kernel_type {
        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<MinKernelKind::Simd, MinimizerT>(
                collector, range, window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range)
        {
            minimizer_dispatch_constexpr<MinKernelKind::Simd, W, K, MinimizerT>(
                collector, range, W, K);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// Two-stack nthash minimizer extraction (current default).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH> {
    struct kernel_type {
        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            minimizer_dispatch<MinKernelKind::TwoStack, MinimizerT>(
                collector, range, window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& /*params*/,
            Collector& collector, Range& range)
        {
            minimizer_dispatch_constexpr<MinKernelKind::TwoStack, W, K, MinimizerT>(
                collector, range, W, K);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
            std::vector<NamedProperty>&) noexcept {}
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised", "true"},
                {"two_stack",  "true"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

/// DOC0351 Phase C — Vulkan minimap-int minimizer extraction. Reads packed
/// `compact_sequence_range` storage directly on the GPU. When this algorithm
/// is selected, Vulkan execution is mandatory and failures throw.
/// `call_result` exposes the resident device-minimizer buffer so the all-Vulkan
/// sketch+seed path reuses it without a second upload (`may_emit_device_minimizers`).
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT> {
    struct kernel_type {
        static constexpr bool may_emit_device_minimizers = true;

        template <class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            if (!params.vulkan_enabled) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            if (!bioseq::gpu::vulkan::collect_minimizers_vulkan<MinimizerT>(
                    collector, range, window_size, kmer_size))
            {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Collector& collector, Range& range)
        {
            call<MinimizerT>(params, collector, range, W, K);
        }

        template <class MinimizerT, class Range>
        [[nodiscard]] static std::optional<
            bioseq::gpu::vulkan::VulkanSketchResult<MinimizerT>>
        call_result(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
            Range& range, std::size_t window_size, std::size_t kmer_size)
        {
            if (!params.vulkan_enabled) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            auto result = bioseq::gpu::vulkan::collect_minimizers_vulkan_result<MinimizerT>(
                range, window_size, kmer_size);
            if (!result) {
                throw_vulkan_declined("Vulkan minimizer sketch");
            }
            return result;
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(
                ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT);
        }
        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& props) noexcept
        {
            props.push_back({"accelerator", "vulkan"});
            props.push_back({"vulkan_enabled", p.vulkan_enabled ? "true" : "false"});
        }
        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"hash", "minimap-int"},
                {"alphabet_dispatch", "runtime-shader-key"},
                {"specialised", Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// ---------------------------------------------------------------------------
// 8.0.3 BaseAlignmentAlgorithmType kernel traits (DOC0254 §6.2)
// ---------------------------------------------------------------------------
//
// The trait pattern is now universal across every algorithm dimension
// (DOC0254 R10).  AlignStage<Kernel, QR, TR> calls `Kernel::template
// call<QR, TR>(...)` once — the per-algorithm dispatch lives entirely in
// the trait specialisations below.

namespace detail {

/// Shared helper for KSW2 / Block Aligner / KSW2_BATCHED — pushes the
/// four affine-scoring parameters that every base aligner consumes.
[[gnu::always_inline]] inline void push_base_affine_params(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
    std::vector<NamedProperty>& out)
{
    out.push_back({"match_score",  std::to_string(p.match_score)});
    out.push_back({"mismatch_pen", std::to_string(p.mismatch_pen)});
    out.push_back({"gap_open1",    std::to_string(p.gap_open1)});
    out.push_back({"gap_ext1",     std::to_string(p.gap_ext1)});
}

}  // namespace detail

template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2> {
    struct kernel_type {
        /// DOC0254 R10: the dispatcher consults this constant to route the
        /// cross-query deferred-align scheduling shape instead of an
        /// `if constexpr (BAA == ...)` ladder.
        static constexpr bool supports_cross_query_batched = false;

        template <supports_compact_sequence_range QR,
                  supports_compact_sequence_range TR,
                  class Scanner = bioseq::storage::sequence_scanner_none<
                      typename compact_sequence_range<QR>::traits::storage_unit>>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            const bioseq::storage::lazy_compact_sequence<QR, Scanner>&            query_seq,
            const bioseq::storage::lazy_compact_sequence<TR, Scanner>&            target_seq,
            const ScoringMatrix<QR>&                     scoring_matrix,
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
        {
            ::bioseq::simd::HWY_NAMESPACE::align_regions<
                ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2>(
                regions, anchors, query_seq, target_seq, scoring_matrix, params);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "KSW2";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& out)
        {
            detail::push_base_affine_params(p, out);
            out.push_back({"gap_open2", std::to_string(p.gap_open2)});
            out.push_back({"gap_ext2",  std::to_string(p.gap_ext2)});
            out.push_back({"zdrop",     std::to_string(p.zdrop)});
            out.push_back({"end_bonus", std::to_string(p.end_bonus)});
        }

        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised",        "intra-sequence striped"},
                {"single_affine_eq",  "data-dependent"},
                {"specialised",       Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::BLOCK_ALIGNER> {
    struct kernel_type {
        static constexpr bool supports_cross_query_batched = false;

        template <supports_compact_sequence_range QR,
                  supports_compact_sequence_range TR,
                  class Scanner = bioseq::storage::sequence_scanner_none<
                      typename compact_sequence_range<QR>::traits::storage_unit>>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            const bioseq::storage::lazy_compact_sequence<QR, Scanner>&            query_seq,
            const bioseq::storage::lazy_compact_sequence<TR, Scanner>&            target_seq,
            const ScoringMatrix<QR>&                     scoring_matrix,
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
        {
            ::bioseq::simd::HWY_NAMESPACE::align_regions<
                ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::BLOCK_ALIGNER>(
                regions, anchors, query_seq, target_seq, scoring_matrix, params);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "Block Aligner";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& out)
        {
            detail::push_base_affine_params(p, out);
            out.push_back({"block_min_size",
                std::to_string(static_cast<int32_t>(p.block_min_size))});
            out.push_back({"block_max_size",
                std::to_string(static_cast<int32_t>(p.block_max_size))});
        }

        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised",       "block-affine"},
                {"single_affine_only", "true"},
                {"specialised",      Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// `KSW2_BATCHED` — DOC0254 §11.  The kernel ships in
// `alignment_hwy/ksw2_batched.cppm`; this trait forwards through a free
// function `align_regions_batched<QR, TR>(...)` that is declared in this
// module via the `bioseq_sequences.ksw2_batched` import.
template <>
struct algorithm_traits<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2_BATCHED> {
    struct kernel_type {
        /// DOC0254 §11.10 — the only kernel whose dispatcher branch
        /// supports cross-query batching.  Setting this constant true on
        /// the KSW2_BATCHED specialisation only is how
        /// `pipeline_run_impl_batch` selects the deferred-align variant
        /// without an enum-value `if constexpr` ladder.
        static constexpr bool supports_cross_query_batched = true;

        template <supports_compact_sequence_range QR,
                  supports_compact_sequence_range TR,
                  class Scanner = bioseq::storage::sequence_scanner_none<
                      typename compact_sequence_range<QR>::traits::storage_unit>>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            const bioseq::storage::lazy_compact_sequence<QR, Scanner>&            query_seq,
            const bioseq::storage::lazy_compact_sequence<TR, Scanner>&            target_seq,
            const ScoringMatrix<QR>&                     scoring_matrix,
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
        {
            ::bioseq::simd::HWY_NAMESPACE::align_regions_batched<QR, TR>(
                regions, anchors, query_seq, target_seq, scoring_matrix, params);
        }

        [[nodiscard]] static constexpr std::string_view algorithm_label() noexcept {
            return "KSW2 (inter-region batched)";
        }

        static void populate_runtime_params(
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
            std::vector<NamedProperty>& out)
        {
            // Same runtime knobs as the intra-sequence KSW2 path; the
            // bucketing/batching is determined by the kernel from
            // params.max_gap, params.zdrop, and the per-fill geometry.
            algorithm_traits<
                ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType::KSW2>::
                kernel_type::populate_runtime_params(p, out);
        }

        template <is_constexpr_aligner_params Cep>
        [[nodiscard]] static std::vector<NamedProperty> static_properties() {
            return {
                {"vectorised",        "inter-sequence packed"},
                {"bucket_widths",     "[<=32, <=64, <=128, <=256, >256]"},
                {"long_fill_kernel",  "intra-sequence KSW2 (fallback)"},
                {"specialised",       Cep::generic_fallback ? "fallback" : "specialised"},
            };
        }
    };
};

// =============================================================================
// 11. Metadata surface (DOC0249 §11) — full definitions
// =============================================================================
//
// `NamedProperty` was hoisted ahead of the algorithm_traits block so trait
// signatures can return `std::vector<NamedProperty>`.  The remaining
// description types are defined here in their natural source order.

struct StageAlgorithmDescription {
    std::string                name;
    std::vector<NamedProperty> parameters;
    std::vector<NamedProperty> computed_properties;
    std::optional<std::string> extra_description;
};

struct StageDescription {
    std::string                              stage_name;
    std::optional<StageAlgorithmDescription> algorithm;
    std::optional<std::string>               extra_description;

    /**
     * @brief Pre-allocation upper bound the stage reserves before driving its
     * kernel (DOC0249 §21 — "reserve correctness").
     *
     * Populated by every stage that opens an `ArenaSlab<E>` of bounded size
     * (MinimizerStage, SeedHitStage, ChainStage, RegionStage).  Set to
     * `std::nullopt` for stages that mutate in-place or whose output size
     * is `0` (IndexStage, SinkStage, etc.).  The EXPLAIN renderer surfaces
     * this as a per-stage `[reserved: N]` line so a reader can verify
     * `actual ≤ reserved` against the runtime instrumentation log.
     */
    std::optional<std::size_t>               estimated_batch_size;
};

struct PipelineDescription {
    std::vector<StageDescription> stages;
};

// =============================================================================
// 21. Output size estimates (DOC0249 §21)
// =============================================================================

namespace detail {

[[nodiscard]] constexpr std::size_t estimated_sketch_count(
    std::size_t bp_size, int32_t window_size) noexcept {
    if (window_size <= 0) return bp_size + 64u;
    return bp_size / static_cast<std::size_t>(window_size) + 64u;
}

/**
 * @brief Reserve-time anchor estimate (DOC0249 §21.3).
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
 * DOC0254 §10.3 unifies the helper signature so it can model the post-fill
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
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    const std::size_t bp_floor = std::max<std::size_t>(1u,
        static_cast<std::size_t>(std::min(query_len, target_len)) / 500u);
    return std::max(static_cast<std::size_t>(params.best_n + 4), bp_floor);
}

} // namespace detail

// =============================================================================
// 9.6 Derived helpers (DOC0249 §9.6)
// =============================================================================

[[nodiscard]] constexpr int32_t sub_diff(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.match_score * 2 + params.mismatch_pen;
}

[[nodiscard]] constexpr int32_t min_diff(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.kmer_size * 2;
}

/**
 * @brief Thread-local cached scoring matrix keyed on (match, mismatch, ambi).
 *
 * Mirrors the cache previously inlined at `minimap_port.cppm:2392-2408`
 * inside `minimap2_port_with_index_impl`.  Hoisted to a free function so
 * both the legacy entry point and `pipeline_run_impl_batch` can share it.
 */
template <supports_compact_sequence_range QR>
[[nodiscard, gnu::always_inline]] inline const ScoringMatrix<QR>&
thread_local_scoring_matrix(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    thread_local int8_t cached_match    = std::numeric_limits<int8_t>::min();
    thread_local int8_t cached_mismatch = std::numeric_limits<int8_t>::min();
    thread_local int8_t cached_ambi     = std::numeric_limits<int8_t>::min();
    thread_local ScoringMatrix<QR> cached_matrix{};

    const int8_t want_match    = static_cast<int8_t>(params.match_score);
    const int8_t want_mismatch = static_cast<int8_t>(params.mismatch_pen);
    const int8_t want_ambi     = static_cast<int8_t>(params.sc_ambi);
    if (cached_match != want_match
        || cached_mismatch != want_mismatch
        || cached_ambi != want_ambi)
    {
        cached_matrix   = ScoringMatrix<QR>::generate(want_match, want_mismatch, want_ambi);
        cached_match    = want_match;
        cached_mismatch = want_mismatch;
        cached_ambi     = want_ambi;
    }
    return cached_matrix;
}

// =============================================================================
// 8. Concrete stages (DOC0249 §8)
// =============================================================================
//
// Each stage's `process*<Cep>(...)` body forwards to the existing kernel
// function in `bioseq::simd::HWY_NAMESPACE` (or the chaining/cigar modules)
// without renaming the kernel.  Wire-type between stages is `std::span<E>`
// directly; arena ownership is provided by `ArenaSlab<E>` materialised
// inside each stage before the downstream call.

/**
 * @brief Stage 0: minimizer extraction.
 *
 * Templated on `Kernel` — the algorithm-specific call surface exposed by
 * `algorithm_traits<MinimizerAlgorithmType::X>::kernel_type` (DOC0249 §8.0,
 * R5 trait pattern).  The stage body has zero `if constexpr` over enum
 * values; adding a new minimizer-extraction algorithm is a single trait
 * specialisation in the kernel's own module.
 *
 * The stage ignores its input batch and instead sketches the pre-captured
 * `query_seq_` pointer (DOC0249 §8.1).
 */
template <class MinimizerT,
          class Kernel,
          supports_compact_sequence_range QR,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
struct MinimizerStage
    : StageBase<MinimizerStage<MinimizerT, Kernel, QR, Scanner>, MinimizerT>
{
    static constexpr std::string_view stage_name = "MinimizerStage";

    const bioseq::storage::lazy_compact_sequence<QR, Scanner>* query_seq_;
    int32_t runtime_w;
    int32_t runtime_k;
    // DOC0307 §5.1 — runtime flags so the Vulkan kernel can gate on
    // `vulkan_enabled` without touching Runtime when Vulkan is off. CPU kernels
    // ignore it.
    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;

    // DOC0254 §5.4 / §8.4: the kernel writes directly into the per-query
    // arena slab via `sketch_collector_arena_adapter`, eliminating the
    // sketches→vector→slab memcpy that the previous shape required.  The
    // specialised path calls `call_constexpr<W, K, MinimizerT>(...)` so
    // the (W, K) pair propagates as integer immediates through the
    // window-loop bounds and unrolls.
    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& /*ignored*/, EmitToken<Sink>&& token) {
        const std::size_t window_size = Cep::generic_fallback
            ? static_cast<std::size_t>(runtime_w)
            : Cep::window_len;
        const std::size_t kmer_size = Cep::generic_fallback
            ? static_cast<std::size_t>(runtime_k)
            : Cep::kmer_len;

        // DOC0307 §5.1 — device-minimizer path: only the Vulkan kernel exposes
        // `call_result`. When it returns a result, copy the host minimizers into
        // the arena slab and forward the resident device span as stage-local
        // metadata so SeedHitStage can skip a second query-minimizer upload.
        if constexpr (requires { Kernel::may_emit_device_minimizers; }) {
            if constexpr (Kernel::may_emit_device_minimizers) {
                if (auto gpu_result =
                        Kernel::template call_result<MinimizerT>(
                            params, *query_seq_, window_size, kmer_size))
                {
                    auto device_slab = make_arena_slab<MinimizerT>();
                    device_slab.reserve(gpu_result->host_minimizers.size());
                    if (!gpu_result->host_minimizers.empty()) {
                        std::memcpy(device_slab.data(),
                                    gpu_result->host_minimizers.data(),
                                    gpu_result->host_minimizers.size() * sizeof(MinimizerT));
                    }
                    device_slab.resize(gpu_result->host_minimizers.size());
                    MinimizerBatchView<MinimizerT> batch{
                        .host = device_slab.as_const_span(),
                        .device = std::optional<
                            bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>{
                                gpu_result->device_span()},
                    };
                    // Host-result permit released now that the slab owns the
                    // copy; `batch.device` retains the resident permit (§6.1).
                    gpu_result->host_permit.reset();
                    return std::move(token).emit_minimizer_batch(batch);
                }
            }
        }

        auto sketches_slab = make_arena_slab<MinimizerT>();
        sketches_slab.reserve(detail::estimated_sketch_count(
            query_seq_->bp_size(), static_cast<int32_t>(window_size)));

        sketch_collector_arena_adapter<MinimizerT> collector{&sketches_slab};

        if constexpr (!Cep::generic_fallback) {
            Kernel::template call_constexpr<Cep::window_len, Cep::kmer_len, MinimizerT>(
                params, collector, *query_seq_);
        } else {
            Kernel::template call<MinimizerT>(
                params, collector, *query_seq_, window_size, kmer_size);
        }

        return std::move(token).emit_batch(std::span<const MinimizerT>{
            sketches_slab.data(), sketches_slab.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = std::string{Kernel::algorithm_label()};
        Kernel::populate_runtime_params({}, algo.parameters);
        algo.computed_properties = Kernel::template static_properties<Cep>();
        desc.algorithm = std::move(algo);
        // Reserve target for the sketch arena slab (DOC0249 §21.4
        // "estimated_sketch_count").  `query_seq_` is a borrowed pointer; on
        // the describe path the proto is empty so bp_size() is zero — which
        // still yields the +64 safety margin from `estimated_sketch_count`.
        desc.estimated_batch_size = detail::estimated_sketch_count(
            query_seq_ != nullptr ? query_seq_->bp_size() : 0u,
            runtime_w > 0 ? runtime_w : 10);
        return desc;
    }
};

/**
 * @brief Stage 1: pass-through that decorates downstream with TargetIndex&.
 *
 * Converts the sink shape from `on_batch(span<MinimizerT>)` upstream to
 * `on_batch_with_index(span<MinimizerT>, IndexT const&)` downstream.  Inlines
 * away at -O3 (zero runtime cost).
 */
template <class IndexT>
struct IndexStage : StageBase<IndexStage<IndexT>, IndexT /* pass-through; downstream sees the same sketch span */> {
    static constexpr std::string_view stage_name = "IndexStage";

    const IndexT* index_;
    bioseq::indexing::GpuIndexCacheIdentity identity_{};

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& sketches, EmitToken<Sink>&& token) {
        // DOC0307 §5.2 / DOC0351 — whole in-memory targets keep pointer
        // identity (`{}`); segmented recompute/assembly passes a nonzero
        // identity so a reused stack index object cannot alias another segment
        // in the GPU resident cache.
        if constexpr (is_minimizer_batch_view_v<B>) {
            return std::move(token).emit_minimizer_batch_with_index(
                std::forward<B>(sketches), *index_, identity_);
        } else {
            using ElemT = std::remove_const_t<
                typename std::remove_cvref_t<B>::value_type>;
            return std::move(token).emit_batch_with_index(
                std::span<const ElemT>{sketches.data(), sketches.size()}, *index_, identity_);
        }
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        desc.extra_description = "target index lookup hand-off";
        return desc;
    }
};

/**
 * @brief Stage 2: seed-hit collection.
 *
 * Wraps the `collect_seed_hits` kernel, which writes its anchor output
 * directly into a caller-provided ArenaSlab (DOC0249 §13 follow-up:
 * output-side kernel-to-arena refactor).  No `std::vector` is materialised
 * inside the stage — the per-stage memcpy that the previous shape required
 * is eliminated, so the kernel's `push_back` traffic lands straight in the
 * pipeline arena.
 */
struct SeedHitStage : StageBase<SeedHitStage, ::bioseq::chaining::Anchor> {
    static constexpr std::string_view stage_name = "SeedHitStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    sequence_size_t                               query_len;
    // DOC0307 §5.2 — batch/segment-wave-scoped GPU index cache. The outer
    // wave creates one cache, passes `&cache` here, and clears it at wave end.
    // Null on single-shot paths (then the GPU seed path uploads uncached).
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr;

    // DOC0307 §5.2 — host/device accessors for both the std::span CPU shape and
    // the MinimizerBatchView device shape.
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static std::span<const MinimizerT>
    sketch_span_of(const std::span<const MinimizerT>& s) noexcept { return s; }
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static std::span<const MinimizerT>
    sketch_span_of(const MinimizerBatchView<MinimizerT>& b) noexcept { return b.host; }

    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>
    device_span_of(const std::span<const MinimizerT>&) noexcept { return std::nullopt; }
    template <class MinimizerT>
    [[nodiscard, gnu::always_inline]] static
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>>
    device_span_of(const MinimizerBatchView<MinimizerT>& b) noexcept { return b.device; }

    template <is_constexpr_aligner_params Cep, class B, class IndexT, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_with_index(
        B&& sketches, const IndexT& target_index_ref,
        bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity,
        EmitToken<Sink>&& token)
    {
        const int32_t kmer_size = Cep::generic_fallback
            ? params.kmer_size
            : static_cast<int32_t>(Cep::kmer_len);

        using MinimizerT = std::remove_const_t<
            typename std::remove_cvref_t<B>::value_type>;

        auto sketch_span = sketch_span_of<MinimizerT>(sketches);
        auto device_span = device_span_of<MinimizerT>(sketches);

        // Pre-size the slab from the tight per-query estimate
        // (DOC0249 §21.4 `estimated_anchor_count`); growth past the estimate
        // doubles the slab inside the same Arena::Chain scope.
        auto anchors_slab = make_arena_slab<Anchor>();
        anchors_slab.reserve(detail::estimated_anchor_count(
            sketch_span.size(),
            static_cast<std::size_t>(query_len),
            static_cast<std::size_t>(target_index_ref.target_length()),
            kmer_size));

        // DOC0307 §5.2 — run the selected GPU seed-hit lookup before the CPU
        // path. If it declines, the selected Vulkan pipeline fails loudly.
        // Only the concrete MinimizerHashmapFragmentIndex carries the GPU view.
        if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
            bool seed_gpu_declined = false;
            if constexpr (std::same_as<std::remove_cvref_t<IndexT>,
                          bioseq::indexing::MinimizerHashmapFragmentIndex>) {
                auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
                if (runtime.usable()) {
                    auto key = bioseq::indexing::make_gpu_index_cache_key(
                        target_index_ref, gpu_index_identity);
                    auto resident_index = gpu_index_cache == nullptr
                        ? nullptr
                        : gpu_index_cache->get_or_upload(
                              runtime, target_index_ref, key,
                              params.vulkan_index_cache_enabled);
                    if (bioseq::gpu::vulkan::collect_seed_hits_vulkan<MinimizerT>(
                            sketch_span,
                            device_span,
                            target_index_ref,
                            resident_index,
                            params.vulkan_enabled,
                            query_len,
                            params.max_occ,
                            kmer_size,
                            anchors_slab,
                            params.vulkan_fused_frontend_enabled))
                    {
                        return std::move(token).emit_batch(
                            std::span<Anchor>{anchors_slab.data(), anchors_slab.size()});
                    }
                    seed_gpu_declined = true;
                } else {
                    seed_gpu_declined = true;
                }
            } else {
                seed_gpu_declined = true;
            }
            if (seed_gpu_declined) {
                throw_vulkan_declined("Vulkan seed lookup");
            }
        }

        collect_seed_hits<MinimizerT>(
            sketch_span,
            target_index_ref,
            query_len, params.max_occ, kmer_size,
            anchors_slab);

        return std::move(token).emit_batch(
            std::span<Anchor>{anchors_slab.data(), anchors_slab.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = "Seed hits + radix sort";
        algo.parameters.push_back({"max_occ", std::to_string(params.max_occ)});
        algo.parameters.push_back({"query_len", std::to_string(query_len)});
        desc.algorithm = std::move(algo);
        // Anchor reserve target — used by SeedHitStage's slab (DOC0249 §21.4
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
 * @brief Stage 3: anchor chaining.
 *
 * Templated on `Kernel` — the algorithm-specific call surface exposed by
 * `algorithm_traits<ChainingAlgorithmType::X>::kernel_type` (DOC0249 §8.0,
 * R5 trait pattern).  The stage body has zero `if constexpr` over enum
 * values — adding a new chaining algorithm is a single trait specialisation
 * in the kernel's own module.
 *
 * The chosen kernel writes its `(chains, reordered_anchors)` output directly
 * into caller-provided ArenaSlab parameters (DOC0249 §13 follow-up:
 * output-side kernel-to-arena refactor); no intermediate vector is
 * materialised inside the stage.
 */
template <class Kernel>
struct ChainStage : StageBase<ChainStage<Kernel>, Chain> {
    static constexpr std::string_view stage_name = "ChainStage";

    ChainingParams cp;

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& anchors_in, EmitToken<Sink>&& token) {
        const std::span<const Anchor> anchors_span{
            anchors_in.data(), anchors_in.size()};

        auto chains_slab    = make_arena_slab<Chain>();
        auto reordered_slab = make_arena_slab<Anchor>();
        chains_slab.reserve(detail::estimated_chain_count(
            anchors_in.size(), cp.min_seed_count));
        reordered_slab.reserve(anchors_in.size());

        Kernel::call(anchors_span, cp, chains_slab, reordered_slab);

        return std::move(token).emit_batch_pair(
            std::span<Chain> {chains_slab.data(),    chains_slab.size()},
            std::span<Anchor>{reordered_slab.data(), reordered_slab.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = std::string{Kernel::algorithm_label()};
        algo.parameters.push_back({"min_chain_score", std::to_string(cp.min_chain_score)});
        algo.parameters.push_back({"min_seed_count",  std::to_string(cp.min_seed_count)});
        algo.parameters.push_back({"max_chain_skip",  std::to_string(cp.max_chain_skip)});
        algo.parameters.push_back({"max_chain_iter",  std::to_string(cp.max_chain_iter)});
        algo.parameters.push_back({"bandwidth",       std::to_string(cp.bandwidth)});
        algo.parameters.push_back({"max_gap",         std::to_string(cp.max_gap)});
        // Let the selected kernel append its own runtime params (e.g. the
        // Vulkan backend's phase / vulkan_enabled markers) so the
        // EXPLAIN metadata is not dead code. No-op for the scalar/SIMD/RMQ
        // kernels (DOC0304 §7.2).
        Kernel::populate_runtime_params(cp, algo.parameters);
        algo.computed_properties = Kernel::template static_properties<Cep>();
        desc.algorithm = std::move(algo);
        // Chain stage's two arena slabs are sized from the runtime
        // anchor count (which is unknown at describe time), so we leave
        // `estimated_batch_size` empty rather than fabricate a per-query
        // figure that doesn't reflect the actual reserve.  The
        // `[reserved: ...]` line in EXPLAIN is therefore omitted for
        // ChainStage; the runtime `chains_slab.reserve(...)` call still
        // sizes from `estimated_chain_count(anchors_in.size(), ...)`.
        return desc;
    }
};

/**
 * @brief Stage 4: chain → region conversion.
 *
 * Wraps the `generate_regions` kernel, which writes its
 * `AlignRegion`-per-chain output directly into a caller-provided
 * ArenaSlab via placement-new (DOC0249 §13 follow-up: output-side
 * kernel-to-arena refactor).  `AlignRegion` owns a `std::vector<CigarOp>`,
 * so it is not trivially-destructible; the slab's destructor walks
 * `[0, len_)` and calls `~AlignRegion()` on each live element before the
 * arena reclaims the storage.
 */
struct RegionStage : StageBase<RegionStage, AlignRegion> {
    static constexpr std::string_view stage_name = "RegionStage";

    sequence_size_t query_len = 0;

    template <is_constexpr_aligner_params Cep, class CB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(CB&& chains, AB&& anchors, EmitToken<Sink>&& token) {
        // `AlignRegion` owns a `std::vector<CigarOp>` so the slab uses the
        // placement-new pattern; no intermediate `std::vector` is needed.
        auto regions_slab = make_arena_slab<AlignRegion>();
        generate_regions(
            std::span<Chain>{chains.data(), chains.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()},
            query_len,
            regions_slab);

        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions_slab.data(), regions_slab.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
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
 * @brief Stage 5: classify primary/secondary in place.
 *
 * Mutates the regions span via `classify_primary_secondary` (which takes
 * std::vector<AlignRegion>& and modifies parent / score fields).  The
 * vector-wrapper here copies in and out; the touched region count is bounded
 * by best_n + 16 so the cost is negligible.
 */
struct PrimarySecondaryStage : StageBase<PrimarySecondaryStage, AlignRegion> {
    static constexpr std::string_view stage_name = "PrimarySecondaryStage";

    float   mask_level;
    int32_t mask_len;
    int32_t sub_diff_value;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        // Kernel now mutates in place through a span — no vector round-trip
        // (DOC0249 M8).
        classify_primary_secondary(
            std::span<AlignRegion>{regions.data(), regions.size()},
            mask_level, mask_len, sub_diff_value);
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
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
 * @brief Stage 6: filter secondary regions, shrinking the span in place.
 */
struct SecondaryFilterStage : StageBase<SecondaryFilterStage, AlignRegion> {
    static constexpr std::string_view stage_name = "SecondaryFilterStage";

    float   pri_ratio;
    int32_t min_diff_value;
    int32_t best_n;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        // Kernel filters in place via a span; the new effective size is
        // returned so the downstream stage observes a shorter prefix
        // (DOC0249 M8 — `filter_secondary_regions_span`).
        const std::size_t new_size = filter_secondary_regions(
            std::span<AlignRegion>{regions.data(), regions.size()},
            pri_ratio, min_diff_value, best_n);
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), new_size},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
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
 * @brief Stage 7: estimate divergence (conditional).
 */
template <class MinimizerT>
struct DivergenceStage : StageBase<DivergenceStage<MinimizerT>, AlignRegion> {
    static constexpr std::string_view stage_name = "DivergenceStage";

    const std::vector<MinimizerT>* query_minimizers_;
    sequence_size_t query_len;
    sequence_size_t target_len;
    int32_t runtime_kmer_size;
    bool    enabled;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        if (enabled) {
            const int32_t kmer_size = Cep::generic_fallback
                ? runtime_kmer_size
                : static_cast<int32_t>(Cep::kmer_len);
            estimate_divergence_from_seeds<MinimizerT>(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const MinimizerT>{
                    query_minimizers_->data(), query_minimizers_->size()},
                std::span<const Anchor>{anchors.data(), anchors.size()},
                query_len, kmer_size, target_len);
        }
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = "Estimate divergence from seeds";
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Stage 8: base-level alignment via the chosen aligner (conditional).
 *
 * DOC0254 §6.3 — reparameterised on `Kernel = kernel_t<BAA>`.  Algorithm
 * dispatch happens exclusively via the trait; the `if constexpr (BAA == ...)`
 * ladder inside `describe()` is gone — its rows now live in
 * `Kernel::populate_runtime_params(...)`.
 */
template <class Kernel,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
struct AlignStage : StageBase<AlignStage<Kernel, QR, TR, Scanner>, AlignRegion> {
    static constexpr std::string_view stage_name = "AlignStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    const ScoringMatrix<QR>*                      sm;
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>*             query_seq_;
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>*             target_seq_;
    bool                                          enabled;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors, EmitToken<Sink>&& token) {
        if (enabled) {
            Kernel::template call<QR, TR>(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const ::bioseq::chaining::Anchor>{
                    anchors.data(), anchors.size()},
                *query_seq_, *target_seq_, *sm, params);
        }
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{regions.data(), regions.size()},
            std::span<const ::bioseq::chaining::Anchor>{
                anchors.data(), anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = std::string{Kernel::algorithm_label()};
            Kernel::populate_runtime_params(params, algo.parameters);
            algo.computed_properties = Kernel::template static_properties<Cep>();
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Stage 9: compute MAPQ (conditional).
 *
 * Accepts a `(regions, anchors)` pair upstream but only uses the regions
 * span; emits the regions span downstream as a single batch (DOC0249 §8.10).
 */
struct MapqStage : StageBase<MapqStage, AlignRegion> {
    static constexpr std::string_view stage_name = "MapqStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    bool                                          enabled;

    template <is_constexpr_aligner_params Cep, class RB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(RB&& regions, EmitToken<Sink>&& token) {
        if (enabled) {
            compute_mapping_quality(
                std::span<AlignRegion>{regions.data(), regions.size()},
                params, 0);
        }
        return std::move(token).emit_batch(
            std::span<AlignRegion>{regions.data(), regions.size()});
    }

    // DOC0254 §4.3: pair → single delegation.  The outer body has no
    // surviving `EmitToken<Sink>` after the inner call moves it, so a
    // second emit is structurally blocked.
    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& /*unused*/, EmitToken<Sink>&& token) {
        return process<Cep>(std::forward<RB>(regions), std::move(token));
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = "Compute mapping quality (mm_set_mapq2)";
            desc.algorithm = std::move(algo);
        }
        return desc;
    }
};

/**
 * @brief Terminal sink: push converted MatchResults into the collection.
 *
 * Not a `BareStage` — appears as the tail of `make_pipeline<Cep>(...)`.
 */
struct SinkStage {
    MatchResultCollection* dst_;
    sequence_size_t        query_len  = 0;
    sequence_size_t        target_len = 0;

    [[gnu::always_inline]] void on_batch(std::span<AlignRegion> regions) {
        dst_->results.reserve(dst_->results.size() + regions.size());
        // `region_slot` is move-consumed into the converted MatchResult; the
        // slot is left in a valid-but-moved-from state and reclaimed by
        // whichever owner (arena slab or std::vector) holds the span.
        for (auto& region_slot : regions) {
            dst_->results.push_back(
                region_to_match_result(std::move(region_slot), query_len, target_len));
        }
    }

    [[gnu::always_inline]] void on_completed() noexcept { /* no-op */ }
};

// =============================================================================
// 11.2 describe() walker — out-of-line definition (Stage forward-declared above)
// =============================================================================

template <is_constexpr_aligner_params Cep, class Self, class Sink>
[[nodiscard]] PipelineDescription Stage<Cep, Self, Sink>::describe() const {
    // DOC0254 §9.2 / R11: every concrete stage's `describe()` is templated on
    // Cep so describe-time Cep-sensitive output always matches the runtime
    // path's behaviour.  No `describe_representative_cep_t` alias.
    PipelineDescription d;
    d.stages.push_back(self_.template describe<Cep>());
    if constexpr (requires { sink_.describe(); }) {
        PipelineDescription rest = sink_.describe();
        d.stages.insert(d.stages.end(),
            std::make_move_iterator(rest.stages.begin()),
            std::make_move_iterator(rest.stages.end()));
    }
    return d;
}

/**
 * @brief Injects the Cep computed properties as a leading entry in the
 * pipeline description so the EXPLAIN renderer sees w/k/L beside the first
 * stage.
 */
template <is_constexpr_aligner_params Cep, class Self, class Sink>
[[nodiscard]] PipelineDescription describe_with_params(
    const Stage<Cep, Self, Sink>& pipeline)
{
    auto d = pipeline.describe();
    if (!d.stages.empty()) {
        auto& first = d.stages.front();
        if (!first.algorithm.has_value()) {
            first.algorithm.emplace();
            first.algorithm->name = "pipeline configuration";
        }
        first.algorithm->computed_properties.push_back(
            {"cep.window_len", std::to_string(Cep::window_len)});
        first.algorithm->computed_properties.push_back(
            {"cep.kmer_len",   std::to_string(Cep::kmer_len)});
        first.algorithm->computed_properties.push_back(
            {"cep.sequence_len_log2", std::to_string(Cep::sequence_len_log2)});
        first.algorithm->computed_properties.push_back(
            {"cep.specialised", Cep::generic_fallback ? "fallback" : "specialised"});
    }
    return d;
}

// =============================================================================
// 9.4 make_minimap_pipeline — single source of truth for the pipeline shape
// =============================================================================

/**
 * @brief Build the full minimap pipeline value for one (Cep, BAA, CA, MA, EP).
 *
 * All template parameters are fixed by the caller (the dispatcher cascade).
 * The function is the single place both the run path and the describe path
 * consult to learn what stages are present — adding or removing a stage edits
 * only this function (DOC0249 §9.4).
 */
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
[[nodiscard, gnu::always_inline]] inline auto make_minimap_pipeline(
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ScoringMatrix<QR>&          scoring_matrix,
    MatchResultCollection&            output_collection,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    sequence_size_t                   query_len,
    sequence_size_t                   target_len,
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {})
{
    auto tail = make_pipeline<Cep>(
        IndexStage<IndexT>{ {}, &target_index, gpu_index_identity },
        SeedHitStage{ {}, params, query_len, gpu_index_cache },
        ChainStage<kernel_t<CA>>{ {}, params.to_chaining_params() },
        RegionStage{ {}, query_len },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        DivergenceStage<MinimizerT>{
            {}, &query_minimizers, query_len, target_len, params.kmer_size,
            params.compute_divergence },
        AlignStage<kernel_t<BAA>, QR, TR, Scanner>{
            {}, params, &scoring_matrix, &query_seq, &target_seq, params.do_cigar },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        MapqStage{ {}, params, params.compute_mapq },
        SinkStage{ &output_collection, query_len, target_len }
    );

    if constexpr (EP == EntryPoint::FromSequence) {
        MinimizerStage<MinimizerT, kernel_t<MA>, QR, Scanner> head{
            {}, &query_seq, params.window_size, params.kmer_size, params };
        return head.template attach<Cep>(std::move(tail));
    } else {
        return tail;
    }
}

// =============================================================================
// 20.4 pipeline_run_impl_batch — batched leaf dispatcher
// =============================================================================

/**
 * @brief Run the pipeline over N queries against one shared target index.
 *
 * Opens a single outer `Arena::Chain` scope for the whole batch and a nested
 * inner scope per query so each query's slabs are reclaimed before the next
 * iteration's pipeline is built.  Per-query results are appended to
 * `out_per_query` in input order.
 */
// DOC0310 §16 — the Phase C frontend's GPU target-index build replaces the CPU
// `MinimizerHashmapFragmentIndex` build for this exact (minimizer, chaining)
// leaf. Runtime-gated; both flags plus the matching algorithm selectors must be
// set. The runtime device check is done at the call site.
[[nodiscard]] inline bool phase_c_index_build_active(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params) noexcept
{
    return params.vulkan_enabled
        && params.vulkan_fused_frontend_enabled
        && params.vulkan_gpu_index_build_enabled
        && params.minimizer_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::VULKAN_MINIMAP_INT
        && params.chaining_algorithm ==
               ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType::VULKAN_TILED_DP;
}

// DOC0271 Phase 1 §5.3 — the deferred-align parallel pipeline template
// (`pipeline_run_impl_batch_deferred_align`) and its supporting types
// (`PerQueryAlignState`, `AlignCollectStage`, `align_regions_batched_cross_query`)
// were removed. The two function bodies were 98% identical and LTO already
// DCE'd the deferred body in every linked binary (no production caller sets
// `params.cross_query_batched = true`), so the post-LTO `__TEXT` is unchanged.
// The pre-LTO IR cost — paid in pipe.cppm's TU compile RSS — is what motivates
// the deletion: every `BAA = KSW2_BATCHED` leaf no longer instantiates a
// duplicate ~6 KB body. See DOC0271 §4.3 + §5.3 Option B for the rationale.
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
[[nodiscard]] std::vector<MatchResultCollection> pipeline_run_impl_batch(
    std::span<const bioseq::storage::lazy_compact_sequence<QR, Scanner>> query_seqs,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>&           target_seq,
    std::span<const std::vector<MinimizerT>>    query_minimizers_per_query,
    const IndexT&                               target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::indexing::GpuMinimizerIndexCache* external_gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    auto outer_chain_scope = context::current().push<context::Arena::Chain>();

    bioseq_log<DBG>("[bioseq] pipe: pipeline_run_impl_batch entry — Cep(L={},K={},W={},fallback={}) "
            "BAA={} CA={} MA={} EP={} queries={} target_bp={}",
            Cep::sequence_len_log2, Cep::kmer_len, Cep::window_len,
            Cep::generic_fallback ? 1 : 0,
            static_cast<int>(BAA), static_cast<int>(CA), static_cast<int>(MA),
            static_cast<int>(EP), query_seqs.size(), target_seq.bp_size());

    const sequence_size_t target_len = target_seq.bp_size();
    const auto& scoring_matrix = thread_local_scoring_matrix<QR>(params);

    // DOC0307 §4.4/§5.2 — one GPU index cache for the whole batch wave, shared
    // across queries that look up the same target index. When a Vulkan seed
    // path is selected, runtime/device failure is reported before scalar work
    // can masquerade as a GPU row.
    std::optional<bioseq::indexing::GpuMinimizerIndexCache> owned_gpu_index_cache;
    if (external_gpu_index_cache == nullptr
        && params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
        bioseq::gpu::vulkan::Runtime& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
        std::uint64_t limit = gpu_rt.usable()
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        owned_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache_ptr =
        external_gpu_index_cache != nullptr
            ? external_gpu_index_cache
            : (owned_gpu_index_cache ? &*owned_gpu_index_cache : nullptr);

    // DOC0310 §12/§16 — build the target minimizer index on the GPU and inject
    // it into the wave cache under the key the SeedHitStage will compute, so the
    // CPU `MinimizerHashmapFragmentIndex` build (skipped upstream in
    // `minimap2_port` for this leaf) is never paid. If the device build declines
    // this selected GPU path fails loudly.
    const IndexT* effective_index_ptr = &target_index;
    if constexpr (std::same_as<std::remove_cvref_t<IndexT>,
                  bioseq::indexing::MinimizerHashmapFragmentIndex>) {
        if (gpu_index_cache_ptr != nullptr && phase_c_index_build_active(params)
            && allow_gpu_target_index_build && target_index.table.empty()
            && target_len > 0) {
            auto& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
            bool injected = false;
            if (gpu_rt.usable()) {
                auto built = bioseq::gpu::vulkan::build_gpu_index_on_device<MinimizerT>(
                    target_seq, static_cast<std::size_t>(params.window_size),
                    static_cast<std::size_t>(params.kmer_size));
                if (built) {
                    auto key = bioseq::indexing::make_gpu_index_cache_key(
                        target_index, gpu_index_identity);
                    auto resident = bioseq::indexing::adopt_gpu_built_index(
                        gpu_rt, std::move(*built), key);
                    if (resident) {
                        resident->target_len = target_len;
                        gpu_index_cache_ptr->insert_built(key, resident);
                        injected = true;
                        bioseq_log<ERR>("[phase_c] GPU index injected: slot_mask={} pos={} target_len={}",
                                        resident->slot_mask, resident->position_count, target_len);
                    }
                } else {
                    bioseq_log<ERR>("[phase_c] build_gpu_index_on_device declined (nullopt) for target_len={}", target_len);
                }
            } else {
                bioseq_log<ERR>("[phase_c] runtime not usable for GPU index build");
            }
            if (!injected) {
                throw_vulkan_declined("Phase C GPU target-index build");
            }
        }
    }
    const IndexT& effective_index = *effective_index_ptr;

    // Empty proto for the FromSequence path's unused minimizers argument.
    static const std::vector<MinimizerT> empty_minimizers_proto{};

    auto run_one_query =
        [target_len, &query_seqs, &query_minimizers_per_query, &target_seq,
         &effective_index, &scoring_matrix, &params, gpu_index_cache_ptr,
         gpu_index_identity](std::size_t qi) {
        const auto&   query_seq = query_seqs[qi];
        const sequence_size_t query_len = query_seq.bp_size();

        auto per_query_chain_scope = context::current().push<context::Arena::Chain>();

        MatchResultCollection out;
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));

        const auto& mins_for_this_query = (EP == EntryPoint::WithPrebuiltMinimizers)
            ? query_minimizers_per_query[qi]
            : empty_minimizers_proto;

        // Short-circuit early-empty cases matching the legacy entry's
        // returns at `minimap_port.cppm:2313` and :2325.
        if (query_len == 0 || target_len == 0) {
            return out;
        }

        auto pipeline = make_minimap_pipeline<Cep, BAA, CA, MA, EP, QR, TR>(
            query_seq, target_seq, mins_for_this_query, effective_index,
            scoring_matrix, out, params, query_len, target_len,
            gpu_index_cache_ptr, gpu_index_identity);

        if constexpr (EP == EntryPoint::WithPrebuiltMinimizers) {
            pipeline.on_batch(std::span<const MinimizerT>{
                mins_for_this_query.data(), mins_for_this_query.size()});
        } else {
            // FromSequence: MinimizerStage's process ignores the input and
            // sketches the cached query_seq_ pointer instead; pass an empty
            // span of bytes as a placeholder.
            pipeline.on_batch(std::span<const std::byte>{});
        }
        pipeline.on_completed();
        return out;
    };

    std::vector<MatchResultCollection> out_per_query;
    const bool may_parallel_batch =
        bioseq::parallelism::task_parallel(
            bioseq::parallelism::GlobalPool::instance().mode())
        && query_seqs.size()
            >= bioseq::parallelism::TaskParallelMinTriggers::MIN_QUERY_COUNT
        && bioseq::storage::lazy_is_worker_shareable(target_seq);

    if (may_parallel_batch) {
        out_per_query.resize(query_seqs.size());
        bioseq::parallelism::parallel_for_pipeline(
            0, query_seqs.size(), 1,
            bioseq::parallelism::TaskParallelMinTriggers::MIN_QUERY_COUNT,
            bioseq::parallelism::TaskParallelRegion::query_batch,
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

    // DOC0307 §4.4 — release the wave's resident GPU index handles.
    if (owned_gpu_index_cache) {
        owned_gpu_index_cache->clear();
    }
    return out_per_query;
}

// DOC0271 Phase 1 §5.3 — the entire cross-query deferred-align scheduling shape
// (including `pipeline_run_impl_batch_deferred_align`, `PerQueryAlignState`,
// `AlignCollectStage`, and `align_regions_batched_cross_query`) was deleted.
// Its only structural difference from `pipeline_run_impl_batch` was one extra
// `Arena::Chain` push for a not-yet-implemented cross-query fill accumulator;
// the cross-query helper itself was a pass-through over per-query
// `align_regions_batched<…>` calls. Restoring the shape later means re-adding
// the four declarations above; the runtime semantics are now `cross_query_batched`-
// flag-independent.

/**
 * @brief Single-query convenience wrapper around `pipeline_run_impl_batch`.
 */
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
[[nodiscard, gnu::always_inline]] inline MatchResultCollection pipeline_run_impl(
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>* one_query = &query_seq;
    const std::vector<MinimizerT>*    one_mins  = &query_minimizers;
    auto results = pipeline_run_impl_batch<Cep, BAA, CA, MA, EP, QR, TR, MinimizerT, IndexT, Scanner>(
        std::span<const bioseq::storage::lazy_compact_sequence<QR, Scanner>>{one_query, 1},
        target_seq,
        std::span<const std::vector<MinimizerT>>{one_mins, 1},
        target_index, params, gpu_index_cache, gpu_index_identity,
        allow_gpu_target_index_build);
    return std::move(results.front());
}

// =============================================================================
// DOC0256 / DOC0261 — value_sequence (non-type parameter pack carrier)
// =============================================================================
//
// `value_sequence<T, Vs...>` lives in `bioseq_common.templating` (DOC0261
// §4.3); re-exported here so the existing pipe.cppm body keeps the
// unqualified name. See DOC0261 §12.4.

using ::bioseq::common::templating::value_sequence;

// =============================================================================
// DOC0256 §4 — MinimizerLayout-driven L bucket derivation
// =============================================================================
//
// Replaces the hand-coded `(K, L)` ladders that used to live inside
// `dispatch_pipeline` and `minimap_port`.  The bucket list is computed at
// compile time from the per-K transitions in `MinimizerLayout::storage_bits`,
// so adding a new K only requires editing `kmer_size_buckets` and the
// per-K clause in `storage_bits_for`.

/// Canonical k-mer values the bench/postgres paths instantiate.  The leading
/// entry is the default for non-canonical kmer_size — see kmer_size_default.
///
/// DOC0271 Phase 1 / PIPELINE_TEMPLATE_BRANCHING_LEVEL — the array's SIZE must
/// match its element count, not be hard-coded to 3. A `std::array<size_t, 3>`
/// with only `{15u}` initialised silently zeroes the trailing two slots, and
/// the consteval dispatch table then enumerates K=0, which instantiates
/// `MinimizerRepr<63, 0>` whose `hash_shift` equals `sizeof(storage_t)*8` and
/// trips an undefined-shift constexpr error in `index.cppm:151`. `std::to_array`
/// deduces the array size from the initialiser so the two stay in lockstep.
inline constexpr auto kmer_size_buckets = std::to_array<std::size_t>({
    #if PIPELINE_TEMPLATE_BRANCHING_LEVEL <= 1
    15u
    #else
    15u, 7u, 27u
    #endif
});

/// `bit_width(max(max_bp - 1, 1))` clamped into `[K, 63]` — the smallest
/// `L` such that `2^L > max_bp - 1`, lifted up to `K` so the resulting
/// `MinimizerLayout<L, K>` keeps `pos_bits >= kmer_size`.  At L=63 every K
/// resolves to a non-void storage_t (the 128-bit specialisation handles the
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

/// Pack-expansion builder for a per-K table of `storage_bits` values keyed
/// by L ∈ [0, 64).  The layout type takes (L, K) as template parameters, so
/// the only way to convert a runtime L into a template argument is to
/// pre-compute every value of `MinimizerLayout<L, K>::storage_bits` for L
/// in the legal range and index into the resulting `std::array`.  All work
/// happens at constant-evaluation time; the dispatcher path never enters
/// this helper.
template <std::size_t K, std::size_t... Ls>
[[nodiscard]] consteval auto make_storage_bits_table_impl(
    std::index_sequence<Ls...>) noexcept
{
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerLayout;
    return std::array<std::size_t, sizeof...(Ls)>{
        MinimizerLayout<Ls, K>::storage_bits...
    };
}

template <std::size_t K>
[[nodiscard]] consteval auto make_storage_bits_table() noexcept {
    return make_storage_bits_table_impl<K>(std::make_index_sequence<64u>{});
}

/// Look up `MinimizerLayout<L, K>::storage_bits` from the per-K table.  The
/// per-K clauses are a *dispatch table*, not a hand-tuned ladder — they exist
/// because the layout requires K as a *template* argument and the consteval
/// helper above can only enumerate L for a fixed compile-time K.  Every K
/// in `kmer_size_buckets` must have a matching clause here.
[[nodiscard]] consteval std::size_t storage_bits_for(
    std::size_t L, std::size_t K) noexcept
{
    if (K == 15u) return make_storage_bits_table<15u>()[L];
    if (K == 7u)  return make_storage_bits_table<7u>()[L];
    if (K == 27u) return make_storage_bits_table<27u>()[L];
    return 0u;
}

/// Compute the breakpoint L values for a given K.  For each L in [K, 63]:
///   - L is emitted if storage_bits_for(L, K) != storage_bits_for(L-1, K),
///     recorded as the largest L of the previous band (i.e. L-1).
///   - L = K is emitted as the lower anchor.
///   - L = 63 is always emitted as the terminal anchor.
/// Max 6 entries possible (initial + up to 3 storage transitions + terminal +
/// headroom).
template <std::size_t K>
[[nodiscard]] consteval auto compute_breakpoints_for() noexcept {
    std::array<std::size_t, 6u> out{};
    std::size_t out_n = 0u;
    std::size_t last_bits = 0u;
    for (std::size_t L = K; L <= 63u; ++L) {
        const std::size_t bits = storage_bits_for(L, K);
        if (bits != last_bits) {
            out[out_n++] = (last_bits == 0u) ? L : L - 1u;
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
/// breakpoints/K × ≤ 5 kmer sizes = 30; rounded up to a power of two for
/// headroom).
using raw_buf = std::array<std::size_t, 32u>;

/// Free consteval helper — merges compute_breakpoints_for<K>()'s breakpoint
/// list into (raw, raw_n) using the lifted `append_unique_value` primitive.
template <std::size_t K>
consteval void merge_breakpoints_into(
    raw_buf& raw, std::size_t& raw_n) noexcept
{
    constexpr auto bp = compute_breakpoints_for<K>();
    for (std::size_t i = 0u; i < bp.second; ++i) {
        ::bioseq::common::templating::append_unique_value(raw, raw_n, bp.first[i]);
    }
}

/// Pack-expanded union over `kmer_size_buckets` indices.  Threads the
/// accumulator state through `merge_breakpoints_into` (no `[&]` capture)
/// and finishes with the lifted insertion-sort primitive — the
/// dispatcher's smallest-ge lookup needs sorted input.
template <std::size_t... Is>
[[nodiscard]] consteval auto compute_global_l_buckets_impl(
    std::index_sequence<Is...>) noexcept
{
    raw_buf raw{};
    std::size_t raw_n = 0u;
    (merge_breakpoints_into<kmer_size_buckets[Is]>(raw, raw_n), ...);
    ::bioseq::common::templating::sort_in_place_ascending(raw, raw_n);
    return std::pair{raw, raw_n};
}

[[nodiscard]] consteval auto compute_global_l_buckets() noexcept {
    return compute_global_l_buckets_impl(
        std::make_index_sequence<kmer_size_buckets.size()>{});
}

} // namespace detail

/// Materialised dispatcher L bucket list — the union of per-K breakpoints
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
template <std::size_t... Is>
[[nodiscard]] consteval auto make_seq_len_log2_buckets_seq(
    std::index_sequence<Is...>) noexcept
{
    return value_sequence<std::size_t, seq_len_log2_buckets[Is]...>{};
}

using seq_len_log2_buckets_seq = decltype(
    make_seq_len_log2_buckets_seq(
        std::make_index_sequence<seq_len_log2_buckets_n>{}));

/// Materialise `kmer_size_buckets` similarly.  Used as the value-list of
/// `kmer_size_registry`.
template <std::size_t... Is>
[[nodiscard]] consteval auto make_kmer_size_buckets_seq(
    std::index_sequence<Is...>) noexcept
{
    return value_sequence<std::size_t, kmer_size_buckets[Is]...>{};
}

using kmer_size_buckets_seq = decltype(
    make_kmer_size_buckets_seq(
        std::make_index_sequence<kmer_size_buckets.size()>{}));

// DOC0271 Phase 1 §5.1 — per-K seq_len_log2 breakpoint sequence.
//
// Materialise the K-specific breakpoint list (from `compute_breakpoints_for<K>()`)
// as a `value_sequence<std::size_t, ...>`.  Used by `bucket_step_pinned`'s
// K-aware `value_list_for<K>` to iterate only the L breakpoints relevant for
// that K — rather than the global union shape `seq_len_log2_buckets_seq` (which
// causes the §3 explosion).
template <std::size_t K, std::size_t... Is>
[[nodiscard]] consteval auto make_per_k_breakpoints_seq_impl(
    std::index_sequence<Is...>) noexcept
{
    constexpr auto bp = compute_breakpoints_for<K>();
    return value_sequence<std::size_t, bp.first[Is]...>{};
}

template <std::size_t K>
[[nodiscard]] consteval auto make_per_k_breakpoints_seq() noexcept {
    constexpr auto bp = compute_breakpoints_for<K>();
    return make_per_k_breakpoints_seq_impl<K>(std::make_index_sequence<bp.second>{});
}

template <std::size_t K>
using per_k_breakpoints_seq = decltype(make_per_k_breakpoints_seq<K>());

static_assert(per_k_breakpoints_seq<15u>::size() == compute_breakpoints_for<15u>().second);
static_assert(per_k_breakpoints_seq<7u>::size()  == compute_breakpoints_for<7u>().second);
static_assert(per_k_breakpoints_seq<27u>::size() == compute_breakpoints_for<27u>().second);

} // namespace detail

/// Canonical k-mer default — the historical fallback for any non-canonical
/// `params.kmer_size` value.  The chain's `kmer_size_step` pins this when
/// the runtime field is outside `kmer_size_buckets`.
inline constexpr std::size_t kmer_size_default = 15u;
static_assert(kmer_size_default == kmer_size_buckets[0],
    "kmer_size_default must match the leading entry of kmer_size_buckets — "
    "the chain's default-value path expects them in sync.");

// =============================================================================
// DOC0256 §5.1–5.3 / DOC0261 §4.4 — Low-level enum/value dispatch primitives
// =============================================================================
//
// The dispatch primitives now live in `bioseq_common.templating`
// (DOC0261). They are re-exported under the existing names so this
// module's call sites stay unchanged. Codegen is identical to the
// previous in-place definition — every helper carries
// `[[gnu::always_inline]]`.

using ::bioseq::common::templating::enum_dispatch_pack;
using ::bioseq::common::templating::enum_dispatch_pack_void;
using ::bioseq::common::templating::value_dispatch_smallest_ge;
using ::bioseq::common::templating::value_dispatch_exact_with_default;

// =============================================================================
// DOC0256 §5.4 / §5.9 — algorithm_enum_values registry + per-dim selectors
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

template <class Enum>
struct algorithm_enum_values;

template <>
struct algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType> {
    using target_t = ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using enum_t   = target_t;  // legacy alias.
    static constexpr target_t default_value = target_t::SCALAR_MM2;
    using value_list = value_sequence<target_t,
        target_t::SCALAR_MM2,
        target_t::SIMD_MM2,
        target_t::RMQ,
        target_t::VULKAN_TILED_DP>;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
        Args&&...) noexcept
    {
        return p.chaining_algorithm;
    }
};

template <>
struct algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType> {
    using target_t = ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using enum_t   = target_t;
    static constexpr target_t default_value = target_t::BLOCK_ALIGNER;
    using value_list = value_sequence<target_t,
        target_t::KSW2,
        target_t::BLOCK_ALIGNER,
        target_t::KSW2_BATCHED>;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
        Args&&...) noexcept
    {
        return p.base_alignment_algorithm;
    }
};

template <>
struct algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType> {
    using target_t = ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    using enum_t   = target_t;
    static constexpr target_t default_value = target_t::SCALAR;
    using value_list = value_sequence<target_t,
        target_t::SCALAR,
        target_t::SIMD_MINIMAP2_SLOW,
        target_t::TWO_STACK_NTHASH,
        target_t::VULKAN_MINIMAP_INT>;

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr target_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& p,
        Args&&...) noexcept
    {
        return p.minimizer_algorithm;
    }
};

namespace detail {

template <class Enum, Enum V>
[[nodiscard]] consteval bool trait_specialised_for() noexcept {
    return requires { typename algorithm_traits<V>::kernel_type; };
}

template <class Enum, Enum... Vs>
[[nodiscard]] consteval bool all_traits_specialised_for(
    value_sequence<Enum, Vs...>) noexcept
{
    return (trait_specialised_for<Enum, Vs>() && ...);
}

} // namespace detail

static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>::value_list{}),
    "algorithm_enum_values<ChainingAlgorithmType>::value_list contains a value "
    "without an algorithm_traits<>::kernel_type specialisation.  Add the trait "
    "specialisation alongside the enum value.");
static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>::value_list{}),
    "algorithm_enum_values<BaseAlignmentAlgorithmType>::value_list contains a "
    "value without an algorithm_traits<>::kernel_type specialisation.");
static_assert(detail::all_traits_specialised_for(
    algorithm_enum_values<::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>::value_list{}),
    "algorithm_enum_values<MinimizerAlgorithmType>::value_list contains a "
    "value without an algorithm_traits<>::kernel_type specialisation.");

namespace detail {

/// A type satisfies `has_bp_size` when it exposes a `bp_size()` member
/// returning an unsigned integer.  Constrains `seq_len_log2_registry::read_field`'s
/// first two args so a chain composing `seq_len_log2_step` into a call site
/// lacking `(QR&, TR&)` in those slots produces a localised diagnostic
/// at the chain instantiation point.
template <class T>
concept has_bp_size = requires (const T& x) {
    { x.bp_size() } -> std::convertible_to<std::size_t>;
};

struct seq_len_log2_registry {
    using target_t = std::size_t;
    static constexpr std::size_t default_value = seq_len_log2_buckets[0];
    using value_list = seq_len_log2_buckets_seq;

    template <has_bp_size QR_arg, has_bp_size TR_arg, class... Rest>
    [[nodiscard, gnu::always_inline]] static std::size_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        const QR_arg& q, const TR_arg& t,
        Rest&&...) noexcept
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

    template <class... Args>
    [[nodiscard, gnu::always_inline]] static constexpr std::size_t read_field(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        Args&&...) noexcept
    {
        return static_cast<std::size_t>(params.kmer_size);
    }
};

/// DOC0271 Phase 1 §5.1 — pinned-aware seq_len_log2 registry.
///
/// Exposes `value_list_for<K, …>` and `read_field_for<K, …>` so the chain's
/// `bucket_step_pinned` can iterate only the L breakpoints for the previously-pinned K.
/// This collapses the 21-cell (L, K) Cartesian product (the global L union × K) into
/// the per-K breakpoint count: `3 + 4 + 2 = 9` cells (K=15: 3 buckets, K=7: 4, K=27: 2).
///
/// Place this step *after* `kmer_size_step` in the chain so `Pinned... = (K, …)`.
struct seq_len_log2_registry_per_k {
    using target_t = std::size_t;

    /// Anchored to K=15's lower bucket boundary (`compute_breakpoints_for<15>()[0]`).
    /// The chain only ever pins `default_value` when `active_v` is false; today every
    /// `bucket_step_pinned` is unconditionally active so this is dead code, but the
    /// chain primitive requires a value to thread through inactive steps.
    static constexpr std::size_t default_value = 15u;

    template <std::size_t K, auto... /*RestPinned*/>
    using value_list_for = per_k_breakpoints_seq<K>;

    template <std::size_t /*K_pinned*/, has_bp_size QR_arg, has_bp_size TR_arg, class... Rest>
    [[nodiscard, gnu::always_inline]] static std::size_t read_field_for(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        const QR_arg& q, const TR_arg& t,
        Rest&&...) noexcept
    {
        const std::size_t max_bp = std::max(q.bp_size(), t.bp_size());
        return round_up_seq_len_log2(
            max_bp, static_cast<std::size_t>(params.kmer_size));
    }
};

} // namespace detail

// =============================================================================
// DOC0256 §5.7 / §5.8 / DOC0261 §4.6 — dispatch_chain primitive and step adapters
// =============================================================================
//
// The chain primitive and the three step adapters now live in
// `bioseq_common.templating::dispatch_chain.cppm` (DOC0261 §4.6).
// They are re-exported here so the pipeline body keeps unqualified
// names. The per-pipeline aliases below pin `Params = Minimap2Params`;
// `pipeline_dispatch_chain` is the local alias that supplies the
// `Minimap2Params` slot so chain call sites do not need to repeat it.

namespace detail {

using ::bioseq::common::templating::enum_step;
using ::bioseq::common::templating::bucket_step;
using ::bioseq::common::templating::bucket_step_pinned;
using ::bioseq::common::templating::guarded_step;

using baa_step = enum_step<algorithm_enum_values<
    ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>>;
using ca_step  = enum_step<algorithm_enum_values<
    ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>>;
using ma_step  = enum_step<algorithm_enum_values<
    ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>>;
using kmer_size_step    = enum_step<kmer_size_registry>;
using seq_len_log2_step = bucket_step<seq_len_log2_registry>;

/// DOC0271 Phase 1 §5.1 — per-K seq_len_log2 step. Must follow `kmer_size_step`
/// in the chain so the pinned-K's per-K breakpoint list is iterated.
using seq_len_log2_step_per_k = bucket_step_pinned<seq_len_log2_registry_per_k>;

/// Per-pipeline chain alias — pins `Params` to the only param type
/// pipe.cppm ever uses. `R` and the leaf template stay as template
/// parameters because real call sites instantiate with different
/// return types (MatchResultCollection / PipelineDescription / void).
template <class R, template <auto...> class Leaf, class... Steps>
using dispatch_chain = ::bioseq::common::templating::dispatch_chain<
    R, Leaf, ::bioseq::simd::HWY_NAMESPACE::Minimap2Params, Steps...>;

} // namespace detail

// =============================================================================
// DOC0256 §5.10 — Leaf types and traits aliases
// =============================================================================
//
// `pipeline_run_leaf<EP, QR, TR, MT, IT, L, K, BAA, CA, MA>` is the
// namespace-scope class template that bottoms out the runtime cascade.
// The Cep (Specialised vs Fallback) branch lifted from the deleted
// `dispatch_on_specialisation` lives inside the leaf body.
//
// `pipeline_run_traits<EP, QR, TR, MT, IT>::leaf<L, K, BAA, CA, MA>` is the
// alias-template wrapper exposing the leaf as an NTTP-only `template
// <auto...> class` view — the shape `dispatch_chain` requires for its
// `template <auto...> class Leaf` template-template parameter.

namespace detail {

template <EntryPoint EP_v,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT,
          std::size_t L, std::size_t K,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
struct pipeline_run_leaf {
    static_assert(std::is_same_v<decltype(L), std::size_t>,
        "pipeline_run_leaf: L must be std::size_t (comes from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(K), std::size_t>,
        "pipeline_run_leaf: K must be std::size_t (comes from kmer_size_step).");
    static_assert(std::is_same_v<decltype(BAA),
        ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>,
        "pipeline_run_leaf: BAA must be BaseAlignmentAlgorithmType (from baa_step).");
    static_assert(std::is_same_v<decltype(CA),
        ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>,
        "pipeline_run_leaf: CA must be ChainingAlgorithmType (from ca_step).");
    static_assert(std::is_same_v<decltype(MA),
        ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>,
        "pipeline_run_leaf: MA must be MinimizerAlgorithmType (from ma_step).");

    [[nodiscard, gnu::always_inline]] static MatchResultCollection call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        const bioseq::storage::lazy_compact_sequence<QR, Scanner>& query_seq,
        const bioseq::storage::lazy_compact_sequence<TR, Scanner>& target_seq,
        const std::vector<MinimizerT>&    query_minimizers,
        const IndexT&                     target_index,
        bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache,
        bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity,
        bool allow_gpu_target_index_build)
    {
        using SpecCep     = ConstexprAlignerParams<10u, K, L>;
        #if PIPELINE_TEMPLATE_BRANCHING_LEVEL == 0
        using FallbackCep = ConstexprAlignerFallbackParams<63>;
        #else
        using FallbackCep = ConstexprAlignerFallbackParams<L>;
        #endif

        #if PIPELINE_TEMPLATE_BRANCHING_LEVEL >= 3
        // Cep selection — lifted from the deleted `dispatch_on_specialisation`.
        // Both Cep instantiations are present in the binary; the runtime
        // `if` picks one.  Compile-time impact: identical to the v0 body.
        if (params.window_size == 10
            && params.kmer_size == static_cast<int32_t>(K))
        {
            return pipeline_run_impl<SpecCep, BAA, CA, MA, EP_v, QR, TR, MinimizerT, IndexT, Scanner>(
                query_seq, target_seq, query_minimizers, target_index, params,
                gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
        }
        #endif
        return pipeline_run_impl<FallbackCep, BAA, CA, MA, EP_v, QR, TR, MinimizerT, IndexT, Scanner>(
            query_seq, target_seq, query_minimizers, target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
    }
};

template <EntryPoint EP_v,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
struct pipeline_run_traits {
    template <auto K, auto L, auto BAA, auto CA, auto MA>
    using leaf = pipeline_run_leaf<
        EP_v, QR, TR, MinimizerT, IndexT,
        L, K, BAA, CA, MA, Scanner>;
};

/// Common leaf body shared by both Spec and Fallback Cep branches inside
/// `describe_leaf::call`.  Identical to the legacy `describe_pipeline_leaf`
/// from the v0 design — kept under that name so the §7.1 chain instantiation
/// can be expressed without renaming the legacy entry.  Declared before
/// `describe_leaf` so its unqualified call inside `describe_leaf::call`
/// resolves at the point of template definition.
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR>
[[nodiscard]] PipelineDescription describe_pipeline_leaf(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    sequence_size_t query_len, sequence_size_t target_len)
{
    using MinimizerT = typename Cep::minimizer_repr_t;
    const std::vector<MinimizerT> empty_minimizers{};
    MinimizerHashmapFragmentIndex empty_index{};
    MatchResultCollection         ignored_out{};
    const auto& sm = thread_local_scoring_matrix<QR>(params);

    auto describe_lazy_q = bioseq::storage::make_lazy_in_memory(query_seq);
    auto describe_lazy_t = bioseq::storage::make_lazy_in_memory(target_seq);
    auto pipeline = make_minimap_pipeline<Cep, BAA, CA, MA, EP, QR, TR>(
        describe_lazy_q, describe_lazy_t, empty_minimizers, empty_index, sm, ignored_out,
        params, query_len, target_len);
    return describe_with_params<Cep>(pipeline);
}

// Describe-side leaf — see DOC0256 §5.10.3 for the rationale on why
// EP/MA are pinned constants here instead of chain-dispatched dimensions.
template <supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          std::size_t L, std::size_t K,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA>
struct describe_leaf {
    static_assert(std::is_same_v<decltype(L), std::size_t>,
        "describe_leaf: L must be std::size_t (from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(K), std::size_t>,
        "describe_leaf: K must be std::size_t (from kmer_size_step).");
    static_assert(std::is_same_v<decltype(BAA),
        ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType>,
        "describe_leaf: BAA must be BaseAlignmentAlgorithmType (from baa_step).");
    static_assert(std::is_same_v<decltype(CA),
        ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType>,
        "describe_leaf: CA must be ChainingAlgorithmType (from ca_step).");

    [[nodiscard, gnu::always_inline]] static PipelineDescription call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        const compact_sequence_range<QR>& q,
        const compact_sequence_range<TR>& t,
        const int32_t& query_len,
        const int32_t& target_len)
    {
        using SpecCep     = ConstexprAlignerParams<10u, K, L>;
        #if PIPELINE_TEMPLATE_BRANCHING_LEVEL == 0
        using FallbackCep = ConstexprAlignerFallbackParams<63>;
        #else
        using FallbackCep = ConstexprAlignerFallbackParams<L>;
        #endif

        constexpr auto EP_describe = EntryPoint::WithPrebuiltMinimizers;
        constexpr auto MA_describe = algorithm_enum_values<
            ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>::default_value;
        static_assert(MA_describe ==
            ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR,
            "describe_leaf relies on MA default = SCALAR.  Update both call "
            "sites if the MA default ever changes.");

        #if PIPELINE_TEMPLATE_BRANCHING_LEVEL >= 3
        if (params.window_size == 10
            && params.kmer_size == static_cast<int32_t>(K))
        {
            return describe_pipeline_leaf<SpecCep, BAA, CA,
                MA_describe, EP_describe, QR, TR>(
                    q, t, params, query_len, target_len);
        }
        #endif
        return describe_pipeline_leaf<FallbackCep, BAA, CA,
            MA_describe, EP_describe, QR, TR>(
                q, t, params, query_len, target_len);
    }
};

template <supports_compact_sequence_range QR,
          supports_compact_sequence_range TR>
struct describe_traits {
    // DOC0271 Phase 1 §5.1: chain pins K before L (see pipeline_run_traits).
    template <auto K, auto L, auto BAA, auto CA>
    using leaf = describe_leaf<QR, TR, L, K, BAA, CA>;
};

} // namespace detail

// =============================================================================
// DOC0256 §4.4 / §6.1 — top-level runtime cascade entry
// =============================================================================
//
// Replaces every per-level dispatcher function (`dispatch_on_chain`,
// `dispatch_on_chain_and_mini`, `dispatch_on_algorithms`,
// `dispatch_on_specialisation`) and the hand-coded `(K, L)` ladder.  The
// body is a single `dispatch_chain<...>::run<>(...)` line over five steps
// (L, K, BAA, CA, MA) — see DOC0256 §R16.

template <EntryPoint EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QR>::traits::storage_unit>>
[[nodiscard, gnu::always_inline, gnu::flatten]]
MatchResultCollection dispatch_pipeline(
    const bioseq::storage::lazy_compact_sequence<QR, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TR, Scanner>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    using Traits = detail::pipeline_run_traits<EP, QR, TR, MinimizerT, IndexT, Scanner>;
    // DOC0271 Phase 1 §5.1 — K-then-L chain order so the L step (per-K) can
    // iterate only the breakpoints relevant for the pinned K.
    return detail::dispatch_chain<
        MatchResultCollection,
        Traits::template leaf,
        detail::kmer_size_step,
        detail::seq_len_log2_step_per_k,
        detail::baa_step,
        detail::ca_step,
        detail::guarded_step<detail::ma_step,
            /*active=*/(EP != EntryPoint::WithPrebuiltMinimizers)>
    >::template run<>(
        params, query_seq, target_seq, query_minimizers, target_index,
        gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
}

// =============================================================================
// DOC0256 §7.1 — describe_pipeline_for (EXPLAIN path)
// =============================================================================
//
// One chain instantiation over four steps (L, K, BAA, CA).  EP and MA are
// pinned constants inside `describe_leaf` (§5.10.3) — describe output is
// invariant in them.

template <supports_compact_sequence_range QR,
          supports_compact_sequence_range TR>
[[nodiscard, gnu::always_inline, gnu::flatten]]
PipelineDescription describe_pipeline_for(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    const compact_sequence_range<QR>& query_seq_proto,
    const compact_sequence_range<TR>& target_seq_proto)
{
    const sequence_size_t query_len  = query_seq_proto.bp_size();
    const sequence_size_t target_len = target_seq_proto.bp_size();

    using Traits = detail::describe_traits<QR, TR>;
    // DOC0271 Phase 1 §5.1 — K-then-L chain order (per-K L step).
    return detail::dispatch_chain<
        PipelineDescription,
        Traits::template leaf,
        detail::kmer_size_step,
        detail::seq_len_log2_step_per_k,
        detail::baa_step,
        detail::ca_step
    >::template run<>(
        params, query_seq_proto, target_seq_proto, query_len, target_len);
}

} // namespace bioseq::simd::HWY_NAMESPACE::pipe

// =============================================================================
// 9.7 Public entry points (DOC0249 §9.7) — relocated from minimap_port.cppm so
// they can delegate to `pipe::dispatch_pipeline` without forming a circular
// import.  Live in `bioseq::simd::HWY_NAMESPACE` so callers' fully-qualified
// names (`bioseq::simd::N_NEON::minimap_port(...)`,
// `bioseq::simd::N_NEON::minimap2_port_with_index(...)`) are unchanged.
// =============================================================================

namespace bioseq::simd::HWY_NAMESPACE {

// DOC0279 §9 — relocated index name in scope for this namespace's entry points.
using bioseq::indexing::MinimizerHashmapFragmentIndex;
using bioseq::indexing::MinimizerIndex;

/**
 * @brief Run the minimap2 pipeline against a pre-built target index using a
 * caller-supplied query minimiser vector (DOC0249 §9.7).
 *
 * Single-query convenience wrapper that hands off to
 * `pipe::dispatch_pipeline<EP::WithPrebuiltMinimizers>(...)`.  All algorithm-
 * choice and bucket dispatch happens inside the pipe cascade — no
 * `(BAA, CA)` switch at this layer.
 */
template <class MinimizerT,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          ::bioseq::indexing::MinimizerIndex IndexT,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] MatchResultCollection minimap2_port_with_index(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    const IndexT& prebuilt_target_index,
    const Minimap2Params& params = Minimap2Params{},
    bioseq::indexing::GpuMinimizerIndexCache* gpu_index_cache = nullptr,
    bioseq::indexing::GpuIndexCacheIdentity gpu_index_identity = {},
    bool allow_gpu_target_index_build = false)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port_with_index");
    auto _scope = bioseq::instr::region(_desc);
    return ::bioseq::simd::HWY_NAMESPACE::pipe::dispatch_pipeline<
        ::bioseq::simd::HWY_NAMESPACE::pipe::EntryPoint::WithPrebuiltMinimizers,
            QueryRange, TargetRange, MinimizerT, IndexT, Scanner>(
            query_seq, target_seq, query_minimizers, prebuilt_target_index, params,
            gpu_index_cache, gpu_index_identity, allow_gpu_target_index_build);
}

/**
 * @brief Run the minimap2 pipeline against pre-built target minimizers
 * (DOC0249 §9.7 / legacy minimap2_port shape).
 *
 * Builds a TargetIndex from `target_minimizers` and delegates to
 * `minimap2_port_with_index`.
 */
template <class MinimizerT,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] MatchResultCollection minimap2_port(
    const bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    const bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    const std::vector<MinimizerT>& target_minimizers,
    const Minimap2Params& params = Minimap2Params{})
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port");
    auto _scope = bioseq::instr::region(_desc);
    const sequence_size_t target_len = target_seq.bp_size();
    MinimizerHashmapFragmentIndex target_index;
    // DOC0310 §16 — for the Phase C leaf with the GPU index build active, leave
    // the CPU index empty; `pipeline_run_impl_batch` must build it on the
    // device. This is the line that removes the XXL_HIFI
    // `target_index_build` bottleneck from the GPU row.
    const bool phase_c_gpu_index =
        ::bioseq::simd::HWY_NAMESPACE::pipe::phase_c_index_build_active(params);
    if (!phase_c_gpu_index) {
        target_index.build(target_minimizers, target_len);
    }
    return minimap2_port_with_index<MinimizerT, QueryRange, TargetRange, MinimizerHashmapFragmentIndex, Scanner>(
        query_seq, target_seq, query_minimizers, target_index, params,
        nullptr, {}, phase_c_gpu_index);
}

namespace pipe_entry_detail {

// =============================================================================
// DOC0256 §5.10.4 / §6.3 — sketch leaf and helpers
// =============================================================================
//
// The two minimizer sketching switches (target pass / query pass) in the
// legacy `minimap_port_with_repr` body collapse into:
//   - `sketch_leaf<MinimizerT, Collector, Range, MA>` — the chain's
//     terminal leaf; pins MA at compile time and invokes the trait's
//     kernel directly.
//   - `sketch_traits<MinimizerT, Collector, Range>::leaf<MA>` — the
//     alias-template wrapper exposing the leaf as a `template <auto>
//     class`.
//   - `sketch_one<MinimizerT>(...)` — one `dispatch_chain` instantiation
//     over `ma_step`.
//   - `sketch_into_collector<MinimizerT>(...)` — packages the per-pass
//     register_region_static + reserve + sketch_one into one inline call.

template <class MinimizerT,
          class Collector, class Range,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType MA>
struct sketch_leaf {
    static_assert(std::is_same_v<decltype(MA),
        ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType>,
        "sketch_leaf: MA must be MinimizerAlgorithmType (from ma_step).");

    [[gnu::always_inline]] static void call(
        const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
        Collector& collector, Range& range,
        const std::size_t& window_len, const std::size_t& kmer_len)
    {
        using Kernel = typename ::bioseq::simd::HWY_NAMESPACE::pipe::
            algorithm_traits<MA>::kernel_type;
        Kernel::template call<MinimizerT>(
            params, collector, range, window_len, kmer_len);
    }
};

template <class MinimizerT, class Collector, class Range>
struct sketch_traits {
    template <auto MA>
    using leaf = sketch_leaf<MinimizerT, Collector, Range, MA>;
};

/// One-step chain over the MA dimension — replaces the legacy switch on
/// `params.minimizer_algorithm`.  The chain pins MA at compile time and the
/// leaf dispatches via the trait's kernel.
template <class MinimizerT, class Collector, class Range>
[[gnu::always_inline, gnu::flatten]] void sketch_one(
    Collector& collector, Range& range,
    const std::size_t& window_len, const std::size_t& kmer_len,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    using Traits = sketch_traits<MinimizerT, Collector, Range>;
    ::bioseq::simd::HWY_NAMESPACE::pipe::detail::dispatch_chain<
        void,
        Traits::template leaf,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::ma_step
    >::template run<>(params, collector, range, window_len, kmer_len);
}

/// Packages the per-pass instrumentation scope + collector reserve +
/// sketch_one into one inline call.  Replaces the ~22-line block-scope
/// boilerplate repeated for the target and query passes in the legacy
/// `minimap_port_with_repr` body.
template <class MinimizerT, class Range>
[[gnu::always_inline]] void sketch_into_collector(
    sketch_collector_t<MinimizerT>& collector,
    Range& range,
    const std::size_t& window_len, const std::size_t& kmer_len,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("mm_sketch");
    auto _scope = bioseq::instr::region(_desc);
    collector.reserve(
        range.bp_size() / std::max<std::size_t>(window_len, 1) + 64);
    sketch_one<MinimizerT>(collector, range, window_len, kmer_len, params);
}

/**
 * @brief Per-bucket helper for the from-sequence `minimap_port` entry.
 *
 * Owns one `Arena::Chain` scope around the target minimiser sketching + index
 * build.  Sketching is dispatched via `sketch_into_collector<MinimizerT>` —
 * the per-pass switch on `params.minimizer_algorithm` is now expressed as a
 * single-step `dispatch_chain` instantiation inside `sketch_one`.  Once the
 * target index is built, the function delegates to `minimap2_port_with_index`
 * so the per-query MinimizerStage inside `pipe::dispatch_pipeline` handles
 * query sketching.
 */
template <class MinimizerT,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard]] MatchResultCollection minimap_port_with_repr(
    bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap_port");
    auto _scope = bioseq::instr::region(_desc);

    std::optional<context::context> owned_context;
    std::optional<context::installed_context_scope> context_install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        context_install.emplace(*owned_context);
    }

    const std::size_t window_len = static_cast<std::size_t>(params.window_size);
    const std::size_t kmer_len   = static_cast<std::size_t>(params.kmer_size);

    // DOC0310 §16 — skip the host target sketch for the Phase C GPU-index leaf;
    // the device index build sketches the target itself. If that selected GPU
    // path cannot run, the pipeline throws instead of rebuilding on CPU.
    std::vector<MinimizerT> target_minimizers;
    const bool phase_c_gpu_index =
        ::bioseq::simd::HWY_NAMESPACE::pipe::phase_c_index_build_active(params);
    if (!phase_c_gpu_index) {
        sketch_collector_t<MinimizerT> target_collector;
        sketch_into_collector<MinimizerT>(
            target_collector, target_seq, window_len, kmer_len, params);
        target_minimizers = std::move(target_collector).extract_sketches_data();
    }

    sketch_collector_t<MinimizerT> query_collector;
    sketch_into_collector<MinimizerT>(
        query_collector, query_seq, window_len, kmer_len, params);
    const auto query_minimizers = std::move(query_collector).extract_sketches_data();

    return minimap2_port<MinimizerT, QueryRange, TargetRange, Scanner>(
        query_seq, target_seq, query_minimizers, target_minimizers, params);
}

// =============================================================================
// DOC0256 §4.6 — from-sequence cascade leaf and traits
// =============================================================================
//
// `minimap_port` now collapses to one `dispatch_chain<...>::run<>(...)` over
// two steps (L, K).  The leaf below pins `(L, K)` into the
// `MinimizerRepr<L, K>` storage type and delegates to the existing
// `minimap_port_with_repr<MinimizerT>` body unchanged.

template <supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          std::size_t L, std::size_t K,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
struct minimap_port_repr_leaf {
    static_assert(std::is_same_v<decltype(L), std::size_t>,
        "minimap_port_repr_leaf: L must be std::size_t (from seq_len_log2_step).");
    static_assert(std::is_same_v<decltype(K), std::size_t>,
        "minimap_port_repr_leaf: K must be std::size_t (from kmer_size_step).");

    [[gnu::always_inline]] static MatchResultCollection call(
        const Minimap2Params& params,
        bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>&  q,
        bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& t)
    {
        using MinimizerT = ::bioseq::simd::HWY_NAMESPACE::MinimizerRepr<L, K>;
        return minimap_port_with_repr<MinimizerT, QueryRange, TargetRange, Scanner>(
            q, t, params);
    }
};

template <supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
struct minimap_port_traits {
    template <auto K, auto L>
    using leaf = minimap_port_repr_leaf<QueryRange, TargetRange, L, K, Scanner>;
};

} // namespace pipe_entry_detail

/**
 * @brief Top-level minimap2 entry — raw query/target sequences only
 * (DOC0249 §9.7 / legacy `minimap_port`; DOC0256 §4.6 chain rewrite).
 *
 * One `dispatch_chain<...>::run<>(...)` line over `seq_len_log2_step` +
 * `kmer_size_step` — replaces the hand-coded `(K, L)` ladder.  Each pinned
 * `(L, K)` selects a `MinimizerRepr<L, K>` and forwards to
 * `pipe_entry_detail::minimap_port_with_repr<MinimizerT>` which sketches
 * both target and query minimisers, builds the target index, and delegates
 * to `minimap2_port_with_index` (which in turn calls
 * `pipe::dispatch_pipeline<EP::WithPrebuiltMinimizers>`).
 */
template <supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>>
[[nodiscard, gnu::always_inline, gnu::flatten]]
MatchResultCollection minimap_port(
    bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const Minimap2Params& params = Minimap2Params{})
{
    using Traits = pipe_entry_detail::minimap_port_traits<QueryRange, TargetRange, Scanner>;
    // DOC0271 Phase 1 §5.1 — K-then-L chain order (per-K L step).
    return ::bioseq::simd::HWY_NAMESPACE::pipe::detail::dispatch_chain<
        MatchResultCollection,
        Traits::template leaf,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::kmer_size_step,
        ::bioseq::simd::HWY_NAMESPACE::pipe::detail::seq_len_log2_step_per_k
    >::template run<>(params, query_seq, target_seq);
}

/**
 * @brief DOC0276 — memory-bounded recompute entry. Drop-in replacement for
 * `minimap_port` on the production paths, sized for arbitrarily large targets
 * on a fixed-RAM host.
 *
 * The non-segmented `minimap_port` builds ONE minimizer index over the whole
 * target (~6 B/bp at peak → ~37 GB for 6 Gbp), which swaps a 16 GB box to death.
 * This entry instead indexes the target in budget-sized SEGMENTS (≤200 Mbp core
 * + overlap, sized by `bioseq::memory::default_budget()`), pinning the 8-byte
 * `MinimapSketchData` minimizer (segment-local positions fit its 28-bit field).
 * Each segment index is globalized (`TargetIndex::add_position_offset`) and run
 * through the UNCHANGED `minimap2_port_with_index` against the FULL target, so
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
template <class MinimizerT>
struct SegmentFragmentEmission {
    std::size_t fragment_no = 0;
    sequence_size_t fragment_len_bp = 0;
    std::vector<MinimizerT> minimizers{};
};

template <class MinimizerT>
[[nodiscard]] std::vector<SegmentFragmentEmission<MinimizerT>>
collect_fragment_emissions_for_segment(
        const pipe::SegmentUnitRange& range,
        std::span<const MinimizerT> segment_minimizers,
        sequence_size_t total_bp,
        sequence_size_t frag_bp,
        std::size_t lpu) {
    const sequence_size_t emit_base_bp =
        static_cast<sequence_size_t>(range.cs_u) * lpu;
    const std::size_t frag_lo =
        static_cast<std::size_t>(range.core_lo_bp / frag_bp);
    const std::size_t frag_hi =
        static_cast<std::size_t>((range.core_hi_bp + frag_bp - 1) / frag_bp);

    std::vector<SegmentFragmentEmission<MinimizerT>> out;
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

        SegmentFragmentEmission<MinimizerT> emission{};
        emission.fragment_no = fragment_no;
        emission.fragment_len_bp = fragment_hi_bp - fragment_lo_bp;
        for (const MinimizerT& minimizer : segment_minimizers) {
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

template <class MinimizerT>
struct PendingFragmentBuild {
    sequence_size_t fragment_len_bp = 0;
    std::vector<MinimizerT> minimizers{};
};

template <class FragmentStorage, class MinimizerT>
class FragmentPersistAccumulator {
public:
    FragmentPersistAccumulator(
            FragmentStorage& storage,
            sequence_size_t total_bp,
            sequence_size_t frag_bp)
        : storage_(storage),
          total_bp_(total_bp),
          frag_bp_(frag_bp),
          nfrag_(static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp)) {}

    void ingest(std::span<const SegmentFragmentEmission<MinimizerT>> emissions) {
        for (const SegmentFragmentEmission<MinimizerT>& emission : emissions) {
            PendingFragmentBuild<MinimizerT>& pending =
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
            bioseq::indexing::DIRECTORY_SUB_KEY,
            sizeof(std::int64_t));
        storage_.template write_to_each<std::int64_t>(
            bioseq::indexing::DIRECTORY_SUB_KEY,
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
        PendingFragmentBuild<MinimizerT> pending{};
        if (auto it = pending_.find(fragment_no); it != pending_.end()) {
            pending = std::move(it->second);
            pending_.erase(it);
        }
        if (pending.fragment_len_bp == 0) {
            pending.fragment_len_bp =
                fragment_hi_bp(fragment_no) - fragment_lo_bp(fragment_no);
        }

        MinimizerHashmapFragmentIndex frag_idx;
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
                    chunk_no * bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES;
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

    FragmentStorage& storage_;
    sequence_size_t total_bp_ = 0;
    sequence_size_t frag_bp_ = 0;
    std::size_t nfrag_ = 0;
    std::size_t next_flush_fragment_ = 0;
    std::map<std::size_t, PendingFragmentBuild<MinimizerT>> pending_{};
};

// =============================================================================
// DOC0307 §5.3 — query sketch for persistent-index bucket selection.
// =============================================================================
//
// Holds the query minimizers produced for persistent-index bucket filtering,
// keeping the optional GPU host-result permit and device span alive for the
// caller's lifetime. The host vector feeds the existing assembly/filter helpers
// unchanged; the Vulkan branch sketches the query on the GPU so the segmented
// path's seed lookup consumes byte-identical minimizers.
template <class MinimizerT>
struct BudgetedSketchVector {
    std::vector<MinimizerT> minimizers;
    std::optional<bioseq::parallelism::MemoryPermit> permit;
    std::optional<bioseq::gpu::vulkan::VulkanDeviceMinimizerSpan<MinimizerT>> device;

    [[nodiscard]] std::span<const MinimizerT> as_span() const noexcept {
        return minimizers;
    }
};

template <class MinimizerT, class QuerySeq>
[[nodiscard]] BudgetedSketchVector<MinimizerT> sketch_query_for_index_selection(
    QuerySeq& query_seq, const Minimap2Params& params, std::size_t w, std::size_t k)
{
    if (params.minimizer_algorithm == MinimizerAlgorithmType::VULKAN_MINIMAP_INT)
    {
        if (!params.vulkan_enabled) {
            pipe::throw_vulkan_declined("Vulkan query minimizer sketch");
        }
        if (auto gpu_sketch =
                bioseq::gpu::vulkan::collect_minimizers_vulkan_result<MinimizerT>(
                    query_seq,
                    w,
                    k))
        {
            return BudgetedSketchVector<MinimizerT>{
                .minimizers = std::move(gpu_sketch->host_minimizers),
                .permit = std::move(gpu_sketch->host_permit),
                .device = gpu_sketch->device_span(),
            };
        }
        pipe::throw_vulkan_declined("Vulkan query minimizer sketch");
    }

    sketch_collector_t<MinimizerT> query_collector;
    query_collector.reserve(query_seq.bp_size() / std::max<std::size_t>(w, 1) + 64);
    // DOC0345 Patch D — query sketch is explicitly Scalar; it must never inherit
    // the target Vulkan backend. (When minimizer_algorithm==VULKAN_MINIMAP_INT the
    // caller routes the query through the GPU whole-sequence path before reaching here.)
    pipe::minimizer_dispatch_range<MinimizerT>(
        query_collector,
        query_seq,
        0,
        (query_seq.bp_size() + QuerySeq::traits::letters_per_unit - 1)
            / QuerySeq::traits::letters_per_unit,
        w,
        k,
        MinimizerSketchBackend::Scalar);
    return BudgetedSketchVector<MinimizerT>{
        .minimizers = std::move(query_collector).extract_sketches_data(),
        .permit = std::nullopt,
        .device = std::nullopt,
    };
}

template <supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange,
          class Scanner = bioseq::storage::sequence_scanner_none<
              typename compact_sequence_range<QueryRange>::traits::storage_unit>,
          class FragmentIndexStorage = std::nullptr_t>
[[nodiscard]] MatchResultCollection minimap_port_segmented(
    bioseq::storage::lazy_compact_sequence<QueryRange, Scanner>& query_seq,
    bioseq::storage::lazy_compact_sequence<TargetRange, Scanner>& target_seq,
    const Minimap2Params& params = Minimap2Params{},
    FragmentIndexStorage* fragment_index_out = nullptr)
{
    using MinimizerT = MinimapSketchData;
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap_port_segmented");
    auto _scope = bioseq::instr::region(_desc);

    const std::size_t w = static_cast<std::size_t>(params.window_size);
    const std::size_t k = static_cast<std::size_t>(params.kmer_size);
    const sequence_size_t query_len  = query_seq.bp_size();
    const sequence_size_t target_len = target_seq.bp_size();
    if (query_len == 0 || target_len == 0) {
        return MatchResultCollection{};
    }

    // DOC0345 Patch D — resolve the target-index sketch backend once; every
    // target `minimizer_dispatch_range` call below passes it explicitly so no
    // target seam can silently fall back to scalar.
    const MinimizerSketchBackend target_sketch_backend =
        pipe::target_index_sketch_backend_from_params(params);

    std::optional<bioseq::indexing::GpuMinimizerIndexCache> segmented_gpu_index_cache;
    if (params.vulkan_enabled && params.vulkan_seed_hits_enabled) {
        bioseq::gpu::vulkan::Runtime& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
        std::uint64_t limit = gpu_rt.usable()
            ? gpu_rt.features().device_local_bytes / 2u
            : (std::uint64_t{512} << 20);
        if (limit < (std::uint64_t{512} << 20)) limit = std::uint64_t{512} << 20;
        if (limit > (std::uint64_t{4} << 30)) limit = std::uint64_t{4} << 30;
        segmented_gpu_index_cache.emplace(limit);
    }
    bioseq::indexing::GpuMinimizerIndexCache* segmented_gpu_index_cache_ptr =
        segmented_gpu_index_cache ? &*segmented_gpu_index_cache : nullptr;
    auto segment_gpu_identity = [](std::size_t segment_index) noexcept {
        return bioseq::indexing::GpuIndexCacheIdentity{
            .assembly_identity = 0x9e3779b97f4a7c15ULL
                ^ (static_cast<std::uint64_t>(segment_index) + 1u),
        };
    };

    // (1) Sketch the query ONCE as MinimapSketchData (query < 2^28 bp; its
    //     positions fit the 28-bit field). Same kmer hashing as the target, so
    //     `collect_seed_hits` matches identically. DOC0307 §5.3 — the helper
    //     sketches on the GPU when VULKAN_MINIMAP_INT is selected (byte-identical
    //     minimizers) and otherwise uses the scalar CPU sketch.
    auto query_sketch =
        sketch_query_for_index_selection<MinimizerT>(query_seq, params, w, k);
    const std::vector<MinimizerT>& query_minimizers = query_sketch.minimizers;
    boost::unordered_flat_set<std::uint64_t> query_kmer_filter;
    query_kmer_filter.reserve(query_minimizers.size());
    for (const MinimizerT& minimizer : query_minimizers) {
        query_kmer_filter.insert(minimizer.kmer());
    }
    struct QueryKmerGate {
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

        [[nodiscard]] bioseq::gpu::vulkan::VulkanSketchQueryGate
        vulkan_gate() const noexcept
        {
            return bioseq::gpu::vulkan::VulkanSketchQueryGate{
                std::span<const std::uint32_t>{words.data(), words.size()},
                static_cast<std::uint32_t>(mask)};
        }
    };
    QueryKmerGate query_kmer_gate;
    if (target_sketch_backend == MinimizerSketchBackend::VulkanSegment) {
        query_kmer_gate.build(query_kmer_filter, k);
    }
    using FilteredMinimizerPair = std::pair<std::uint64_t, std::uint64_t>;
    auto build_query_filtered_segment_index =
        [&query_kmer_filter, &query_kmer_gate, target_len](
                std::span<const MinimizerT> minimizers,
                MinimizerHashmapFragmentIndex& index,
                std::vector<FilteredMinimizerPair>& filtered_pairs,
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
        for (const MinimizerT& minimizer : minimizers) {
            const std::uint64_t kmer = minimizer.kmer();
            if (already_query_filtered
                || query_kmer_gate.accepts_exactly(kmer)
                || (query_kmer_gate.maybe_contains(kmer)
                    && query_kmer_filter.find(kmer) != query_kmer_filter.end())) {
                filtered_pairs.emplace_back(kmer, minimizer.pos_strand_encoded());
            }
        }
        index.build_from_pairs(
            std::span<FilteredMinimizerPair>{
                filtered_pairs.data(), filtered_pairs.size()},
            target_len);
        return filtered_pairs.size();
    };
    using QueryKmerFilter = decltype(query_kmer_filter);
    struct QueryFilteredSketchCollector {
        const QueryKmerFilter* filter = nullptr;
        const QueryKmerGate* gate = nullptr;
        std::vector<MinimizerT> sketches;

        void reserve(std::size_t expected_count) {
            const std::size_t filtered_hint = filter == nullptr
                ? expected_count
                : std::min<std::size_t>(expected_count, filter->size() + 64u);
            sketches.reserve(filtered_hint);
        }
        void clear() { sketches.clear(); }
        [[gnu::always_inline]] void collect(MinimizerT&& sketch) {
            if (filter == nullptr) {
                sketches.push_back(std::move(sketch));
                return;
            }
            const std::uint64_t kmer = sketch.kmer();
            if ((gate == nullptr || gate->maybe_contains(kmer))
                && filter->find(kmer) != filter->end()) {
                sketches.push_back(std::move(sketch));
            }
        }
        [[gnu::always_inline]] void collect(const MinimizerT& sketch) {
            if (filter == nullptr) {
                sketches.push_back(sketch);
                return;
            }
            const std::uint64_t kmer = sketch.kmer();
            if ((gate == nullptr || gate->maybe_contains(kmer))
                && filter->find(kmer) != filter->end()) {
                sketches.push_back(sketch);
            }
        }
        [[nodiscard]] const std::vector<MinimizerT>& extract_sketches_data() const & {
            return sketches;
        }
    };
    struct QueryFilteredPairCollector {
        const QueryKmerFilter* filter = nullptr;
        const QueryKmerGate* gate = nullptr;
        std::uint64_t position_offset = 0u;
        std::vector<FilteredMinimizerPair> pairs;

        void reserve(std::size_t expected_count) {
            const std::size_t filtered_hint = filter == nullptr
                ? expected_count
                : std::min<std::size_t>(expected_count, filter->size() + 64u);
            pairs.reserve(filtered_hint);
        }
        void clear() { pairs.clear(); }
        [[gnu::always_inline]] void collect(MinimizerT&& sketch) {
            collect(static_cast<const MinimizerT&>(sketch));
        }
        [[gnu::always_inline]] void collect(const MinimizerT& sketch) {
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
    };

    // (2) Segment the target. Overlap >= longest target match span so a match
    //     starting in a segment core is fully indexed within that segment.
    constexpr std::size_t lpu =
        compact_sequence_range<TargetRange>::traits::letters_per_unit;
    const sequence_size_t overlap_bp = pipe::segment_overlap_bp(query_len);

    auto run_serial_segmented =
        [target_len, query_len, lpu, w, k, &target_seq, &query_seq,
         &query_minimizers, &query_kmer_filter, &query_kmer_gate, &params, fragment_index_out,
         target_sketch_backend, segmented_gpu_index_cache_ptr,
         &segment_gpu_identity, &build_query_filtered_segment_index]() -> MatchResultCollection {
        std::vector<MatchResult> merged;
        sketch_collector_t<MinimizerT> seg_collector;
        MinimizerHashmapFragmentIndex seg_index;
        std::vector<FilteredMinimizerPair> filtered_pairs;
        const auto ranges =
            pipe::segment_unit_ranges_for_backend(
                target_len, query_len, lpu, target_sketch_backend);
        constexpr std::size_t frag_units = Scanner::fragment_storage_units;
        const sequence_size_t frag_bp =
            static_cast<sequence_size_t>(frag_units) * lpu;

        bioseq_log_lazy<DBG>([&] {
            bioseq_log_progress<DBG>("[minimap_port_segmented] serial target_len={} query_len={} segments={} "
                            "| footprint={}MB avail={}MB",
                    target_len, query_len, ranges.size(),
                    bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                    bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
        });

        if constexpr (!std::is_same_v<FragmentIndexStorage, std::nullptr_t>) {
            if (fragment_index_out != nullptr) {
                FragmentPersistAccumulator<FragmentIndexStorage, MinimizerT> acc{
                    *fragment_index_out, target_len, frag_bp};
                for (std::size_t segment_index = 0;
                     segment_index < ranges.size();
                     ++segment_index) {
                    const pipe::SegmentUnitRange& range = ranges[segment_index];
                    seg_collector.clear();
                    seg_collector.reserve(
                        (range.ce_u - range.cs_u) * lpu
                            / std::max<std::size_t>(w, 1) + 64);
                    pipe::minimizer_dispatch_range<MinimizerT>(
                        seg_collector, target_seq, range.cs_u, range.ce_u, w, k,
                        target_sketch_backend);
                    const std::vector<MinimizerT>& seg_minimizers =
                        seg_collector.extract_sketches_data();

                    auto emissions = collect_fragment_emissions_for_segment(
                        range,
                        std::span<const MinimizerT>(
                            seg_minimizers.data(), seg_minimizers.size()),
                        target_len,
                        frag_bp,
                        lpu);
                    acc.ingest(std::span<const SegmentFragmentEmission<MinimizerT>>(
                        emissions.data(), emissions.size()));
                    acc.flush_ready(range.core_hi_bp);

                    build_query_filtered_segment_index(
                        std::span<const MinimizerT>{
                            seg_minimizers.data(), seg_minimizers.size()},
                        seg_index,
                        filtered_pairs);
                    seg_index.add_position_offset(range.cs_u * lpu);

                    MatchResultCollection seg_res = minimap2_port_with_index(
                        query_seq, target_seq, query_minimizers, seg_index, params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                    for (auto& result : seg_res.results) {
                        if (result.target_start >= range.core_lo_bp
                            && result.target_start < range.core_hi_bp) {
                            merged.push_back(std::move(result));
                        }
                    }
                }
                acc.finish();
                return MatchResultCollection{std::move(merged)};
            }
        }

        QueryFilteredPairCollector filtered_pair_collector{
            &query_kmer_filter, &query_kmer_gate};
        for (std::size_t segment_index = 0;
             segment_index < ranges.size();
             ++segment_index) {
            const pipe::SegmentUnitRange& range = ranges[segment_index];
            bioseq::indexing::GpuIndexCacheIdentity gpu_identity =
                segment_gpu_identity(segment_index);
            std::size_t seg_result_count = 0;
            if (target_sketch_backend == MinimizerSketchBackend::VulkanSegment) {
                filtered_pair_collector.position_offset = range.cs_u * lpu;
                filtered_pair_collector.clear();
                filtered_pair_collector.reserve(
                    (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                bioseq::gpu::vulkan::collect_minimizers_vulkan_segment_or_throw<MinimizerT>(
                    filtered_pair_collector,
                    target_seq,
                    static_cast<std::size_t>(range.cs_u),
                    static_cast<std::size_t>(range.ce_u),
                    w,
                    k,
                    query_kmer_gate.vulkan_gate());
                // Safe with the approximate device gate because the collector
                // still exact-confirms each emitted kmer against query_kmer_filter.
                std::optional<bioseq::indexing::GpuMinimizerIndexCacheKey>
                    direct_gpu_key;
                if (segmented_gpu_index_cache_ptr != nullptr
                    && params.vulkan_seed_hits_enabled
                    && params.vulkan_fused_frontend_enabled) {
                    seg_index.table.clear();
                    seg_index.positions.clear();
                    seg_index.target_len = target_len;
                    auto upload =
                        bioseq::indexing::build_gpu_minimizer_upload_from_pairs(
                            std::span<FilteredMinimizerPair>{
                                filtered_pair_collector.pairs.data(),
                                filtered_pair_collector.pairs.size()},
                            target_len);
                    gpu_identity.table_size = upload.table_size;
                    gpu_identity.position_count = upload.positions.size();
                    auto key = bioseq::indexing::make_gpu_index_cache_key(
                        seg_index, gpu_identity);
                    auto& gpu_rt = bioseq::gpu::vulkan::Runtime::instance();
                    auto resident = segmented_gpu_index_cache_ptr->insert_upload(
                        gpu_rt, std::move(upload), key, true);
                    if (!resident) {
                        pipe::throw_vulkan_declined(
                            "Vulkan direct segment index upload");
                    }
                    direct_gpu_key = key;
                } else {
                    seg_index.build_from_pairs(
                        std::span<FilteredMinimizerPair>{
                            filtered_pair_collector.pairs.data(),
                            filtered_pair_collector.pairs.size()},
                        target_len);
                }

                MatchResultCollection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, seg_index, params,
                    segmented_gpu_index_cache_ptr,
                    gpu_identity);
                if (direct_gpu_key) {
                    segmented_gpu_index_cache_ptr->erase(*direct_gpu_key);
                }
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
                pipe::minimizer_dispatch_range<MinimizerT>(
                    seg_collector, target_seq, range.cs_u, range.ce_u, w, k,
                    target_sketch_backend);
                const std::vector<MinimizerT>& seg_minimizers =
                    seg_collector.extract_sketches_data();
                build_query_filtered_segment_index(
                    std::span<const MinimizerT>{seg_minimizers.data(), seg_minimizers.size()},
                    seg_index,
                    filtered_pairs);
                seg_index.add_position_offset(range.cs_u * lpu);

                MatchResultCollection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, seg_index, params,
                    segmented_gpu_index_cache_ptr,
                    gpu_identity);
                seg_result_count = seg_res.results.size();
                for (auto& result : seg_res.results) {
                    if (result.target_start >= range.core_lo_bp
                        && result.target_start < range.core_hi_bp) {
                        merged.push_back(std::move(result));
                    }
                }
            }
            bioseq_log_lazy<DBG>([&] {
                bioseq_log_progress<DBG>("[minimap_port_segmented] segment core [{},{}) done, kept {} of {} "
                                "(merged {}) | footprint={}MB avail={}MB",
                        range.core_lo_bp, range.core_hi_bp,
                        merged.size(), seg_result_count, merged.size(),
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            });
        }
        return MatchResultCollection{std::move(merged)};
    };

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    if (!bioseq::parallelism::task_parallel(pool.mode())
        || pool.max_concurrency() <= 1
        || target_sketch_backend == MinimizerSketchBackend::VulkanSegment
        || !bioseq::storage::lazy_is_worker_shareable(query_seq)
        || !bioseq::storage::lazy_is_worker_shareable(target_seq)) {
        return run_serial_segmented();
    }

    struct SegmentWaveAdmission {
        unsigned fanout = 1;
        unsigned geometry_divisor = 1;
        sequence_size_t segment_bp = 0;
        std::size_t estimated_segments = 0;
        std::size_t estimated_waves = 0;
        std::uint64_t peak_per_segment = 0;
        bioseq::parallelism::MemoryPermit permit;
        bool admitted = false;
    };

    constexpr std::uint64_t context_reserve = std::uint64_t{8} << 20;
    auto collected_bp_upper_bound =
        [target_len, overlap_bp](sequence_size_t core_bp) -> std::uint64_t {
        const std::uint64_t core = static_cast<std::uint64_t>(core_bp);
        const std::uint64_t overlap = static_cast<std::uint64_t>(overlap_bp);
        const std::uint64_t expanded =
            bioseq::parallelism::saturating_mul(2, overlap);
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

    struct SegmentWaveCandidate {
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
         &peak_for_segment_core_bp]() -> SegmentWaveAdmission {
        auto& admission =
            bioseq::parallelism::MemoryAdmissionController::instance();
        const unsigned max_fanout = pool.max_concurrency();
        if (max_fanout <= 1) {
            return SegmentWaveAdmission{};
        }

        const std::uint64_t allocatable =
            admission.allocatable_after_reservations();
        std::vector<SegmentWaveCandidate> candidates;
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
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(fanout), peak);
            if (reserve_bytes > allocatable) {
                continue;
            }

            const std::size_t estimated_waves =
                (estimated_segments + fanout - 1) / fanout;
            const std::uint64_t estimated_work =
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(estimated_waves),
                    collected_bp);
            candidates.push_back(SegmentWaveCandidate{
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
            [](const SegmentWaveCandidate& lhs, const SegmentWaveCandidate& rhs) {
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

        for (const SegmentWaveCandidate& candidate : candidates) {
            if (auto permit = admission.try_reserve(candidate.reserve_bytes)) {
                return SegmentWaveAdmission{
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

        return SegmentWaveAdmission{};
    };

    auto first_wave = try_admit_segment_geometry();
    if (!first_wave.admitted || first_wave.fanout <= 1) {
        return run_serial_segmented();
    }

    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port_segmented] admitted parallel geometry fanout={} "
                        "divisor={} segment_bp={} estimated_segments={} "
                        "estimated_waves={} peak_per_segment={}MB reserve={}MB "
                        "| footprint={}MB avail={}MB",
                first_wave.fanout, first_wave.geometry_divisor,
                first_wave.segment_bp, first_wave.estimated_segments,
                first_wave.estimated_waves, first_wave.peak_per_segment >> 20,
                first_wave.permit.bytes() >> 20,
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });

    const unsigned geometry_divisor = first_wave.geometry_divisor;
    auto ranges = pipe::segment_unit_ranges(target_len, query_len, lpu, geometry_divisor);
    std::vector<MatchResultCollection> seg_out(ranges.size());
    bioseq::parallelism::TaskParallelScope scope(
        bioseq::parallelism::TaskParallelRegion::segment_recompute);
    if (!scope.acquired()) {
        return run_serial_segmented();
    }

    if constexpr (!std::is_same_v<FragmentIndexStorage, std::nullptr_t>) {
        if (fragment_index_out != nullptr) {
            constexpr std::size_t frag_units = Scanner::fragment_storage_units;
            const sequence_size_t frag_bp =
                static_cast<sequence_size_t>(frag_units) * lpu;
            FragmentPersistAccumulator<FragmentIndexStorage, MinimizerT> acc{
                *fragment_index_out, target_len, frag_bp};
            std::vector<MatchResult> merged;

            auto process_segment_serial =
                [&ranges, &target_seq, &query_seq, &query_minimizers, &params,
                 &acc, &merged, target_len, frag_bp, w, k, lpu,
                 target_sketch_backend, segmented_gpu_index_cache_ptr,
                 &segment_gpu_identity](
                        std::size_t segment_index) {
                sketch_collector_t<MinimizerT> collector;
                MinimizerHashmapFragmentIndex index;
                const pipe::SegmentUnitRange& range = ranges[segment_index];
                collector.reserve(
                    (range.ce_u - range.cs_u) * lpu
                        / std::max<std::size_t>(w, 1) + 64);
                pipe::minimizer_dispatch_range<MinimizerT>(
                    collector, target_seq, range.cs_u, range.ce_u, w, k,
                    target_sketch_backend);
                const std::vector<MinimizerT>& minimizers =
                    collector.extract_sketches_data();
                auto emissions = collect_fragment_emissions_for_segment(
                    range,
                    std::span<const MinimizerT>(minimizers.data(), minimizers.size()),
                    target_len,
                    frag_bp,
                    lpu);
                acc.ingest(std::span<const SegmentFragmentEmission<MinimizerT>>(
                    emissions.data(), emissions.size()));
                acc.flush_ready(range.core_hi_bp);

                index.build(minimizers, target_len);
                index.add_position_offset(range.cs_u * lpu);
                MatchResultCollection seg_res = minimap2_port_with_index(
                    query_seq, target_seq, query_minimizers, index, params,
                    segmented_gpu_index_cache_ptr,
                    segment_gpu_identity(segment_index));
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
                bioseq::parallelism::MemoryPermit wave_permit;
                if (wave_lo == 0) {
                    wave_permit = std::move(first_wave.permit);
                } else if (auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance()
                            .try_reserve(
                                bioseq::parallelism::saturating_mul(
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
                std::vector<MatchResultCollection> wave_seg_out(wave_count);
                std::vector<std::vector<SegmentFragmentEmission<MinimizerT>>>
                    wave_frag_out(wave_count);
                pool.parallel_for_with_context(
                    wave_lo, wave_hi, 1,
                    bioseq::parallelism::range_body{
                        [&ranges, &wave_seg_out, &wave_frag_out,
                         &target_seq, &query_seq, &query_minimizers, &params,
                         current_wave_lo, target_len, frag_bp, w, k, lpu,
                         target_sketch_backend, segmented_gpu_index_cache_ptr,
                         &segment_gpu_identity](
                                std::size_t lo, std::size_t hi) {
                        sketch_collector_t<MinimizerT> collector;
                        MinimizerHashmapFragmentIndex index;
                        for (std::size_t segment_index = lo;
                             segment_index < hi;
                             ++segment_index) {
                            const pipe::SegmentUnitRange& range =
                                ranges[segment_index];
                            const std::size_t rel = segment_index - current_wave_lo;
                            collector.clear();
                            collector.reserve(
                                (range.ce_u - range.cs_u) * lpu
                                    / std::max<std::size_t>(w, 1) + 64);
                            pipe::minimizer_dispatch_range<MinimizerT>(
                                collector, target_seq, range.cs_u, range.ce_u, w, k,
                                target_sketch_backend);
                            const std::vector<MinimizerT>& minimizers =
                                collector.extract_sketches_data();
                            wave_frag_out[rel] =
                                collect_fragment_emissions_for_segment(
                                    range,
                                    std::span<const MinimizerT>(
                                        minimizers.data(), minimizers.size()),
                                    target_len,
                                    frag_bp,
                                    lpu);
                            index.build(minimizers, target_len);
                            index.add_position_offset(range.cs_u * lpu);
                            wave_seg_out[rel] = minimap2_port_with_index(
                                query_seq,
                                target_seq,
                                query_minimizers,
                                index,
                                params,
                                segmented_gpu_index_cache_ptr,
                                segment_gpu_identity(segment_index));
                        }
                    }});

                for (std::size_t rel = 0; rel < wave_count; ++rel) {
                    const std::size_t segment_index = current_wave_lo + rel;
                    const pipe::SegmentUnitRange& range = ranges[segment_index];
                    acc.ingest(std::span<const SegmentFragmentEmission<MinimizerT>>(
                        wave_frag_out[rel].data(), wave_frag_out[rel].size()));
                    acc.flush_ready(range.core_hi_bp);
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
            return MatchResultCollection{std::move(merged)};
        }
    }

    std::vector<std::size_t> segment_minimizer_counts(ranges.size());
    std::vector<std::size_t> segment_filtered_counts(ranges.size());

    auto process_segment_serial =
        [&ranges, &seg_out, &target_seq, &query_seq, &query_minimizers,
         &segment_minimizer_counts, &segment_filtered_counts,
         &build_query_filtered_segment_index, &params, target_len,
         w, k, lpu, target_sketch_backend, segmented_gpu_index_cache_ptr,
         &segment_gpu_identity](std::size_t segment_index) {
        sketch_collector_t<MinimizerT> collector;
        MinimizerHashmapFragmentIndex index;
        std::vector<FilteredMinimizerPair> filtered_pairs;
        const pipe::SegmentUnitRange& range = ranges[segment_index];
        collector.reserve(
            (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
        pipe::minimizer_dispatch_range<MinimizerT>(
            collector, target_seq, range.cs_u, range.ce_u, w, k,
            target_sketch_backend);
        const std::vector<MinimizerT>& minimizers =
            collector.extract_sketches_data();
        segment_minimizer_counts[segment_index] = minimizers.size();
        segment_filtered_counts[segment_index] =
            build_query_filtered_segment_index(
                std::span<const MinimizerT>{minimizers.data(), minimizers.size()},
                index,
                filtered_pairs);
        index.add_position_offset(range.cs_u * lpu);
        seg_out[segment_index] = minimap2_port_with_index(
            query_seq, target_seq, query_minimizers, index, params,
            segmented_gpu_index_cache_ptr,
            segment_gpu_identity(segment_index));
        bioseq_log_lazy<DBG>([&] {
            bioseq_log_progress<DBG>("[minimap_port_segmented] adaptive serial segment core [{},{}) "
                            "produced {} results | footprint={}MB avail={}MB",
                    range.core_lo_bp, range.core_hi_bp,
                    seg_out[segment_index].results.size(),
                    bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                    bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
        });
    };

    for (std::size_t wave_lo = 0; wave_lo < ranges.size();) {
        const std::size_t requested_wave_count = std::min<std::size_t>(
            first_wave.fanout, ranges.size() - wave_lo);
        std::size_t wave_count = requested_wave_count;
        bioseq::parallelism::MemoryPermit wave_permit;
        if (wave_lo == 0) {
            wave_permit = std::move(first_wave.permit);
            wave_permit.shrink_to(
                bioseq::parallelism::saturating_mul(
                    static_cast<std::uint64_t>(wave_count),
                    first_wave.peak_per_segment));
        } else {
            for (std::size_t candidate = requested_wave_count; candidate > 1; --candidate) {
                if (auto permit =
                        bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
                            bioseq::parallelism::saturating_mul(
                                static_cast<std::uint64_t>(candidate),
                                first_wave.peak_per_segment))) {
                    wave_count = candidate;
                    wave_permit = std::move(*permit);
                    break;
                }
            }
        }
        if (!wave_permit) {
            bioseq_log_lazy<DBG>([&] {
                bioseq_log_progress<DBG>("[minimap_port_segmented] wave admission failed at segment {} "
                                "of {}; processing one segment serially | footprint={}MB "
                                "avail={}MB",
                        wave_lo, ranges.size(),
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            });
            process_segment_serial(wave_lo);
            ++wave_lo;
            continue;
        }
        if (wave_count != requested_wave_count) {
            bioseq_log_lazy<DBG>([&] {
                bioseq_log_progress<DBG>("[minimap_port_segmented] wave fanout reduced from {} to {} "
                                "at segment {} of {} | footprint={}MB avail={}MB",
                        requested_wave_count, wave_count, wave_lo, ranges.size(),
                        bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                        bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
            });
        }

        const std::size_t wave_hi = wave_lo + wave_count;
        pool.parallel_for_with_context(
            wave_lo, wave_hi, 1,
            bioseq::parallelism::range_body{
                [&ranges, &seg_out, &target_seq, &query_seq, &query_minimizers,
                 &segment_minimizer_counts, &segment_filtered_counts,
                 &build_query_filtered_segment_index, &params, target_len,
                 w, k, lpu, target_sketch_backend, segmented_gpu_index_cache_ptr,
                 &segment_gpu_identity](std::size_t lo, std::size_t hi) {
                sketch_collector_t<MinimizerT> collector;
                MinimizerHashmapFragmentIndex index;
                std::vector<FilteredMinimizerPair> filtered_pairs;
                for (std::size_t segment_index = lo; segment_index < hi; ++segment_index) {
                    const pipe::SegmentUnitRange& range = ranges[segment_index];
                    collector.clear();
                    collector.reserve(
                        (range.ce_u - range.cs_u) * lpu / std::max<std::size_t>(w, 1) + 64);
                    pipe::minimizer_dispatch_range<MinimizerT>(
                        collector, target_seq, range.cs_u, range.ce_u, w, k,
                        target_sketch_backend);
                    const std::vector<MinimizerT>& minimizers =
                        collector.extract_sketches_data();
                    segment_minimizer_counts[segment_index] = minimizers.size();
                    segment_filtered_counts[segment_index] =
                        build_query_filtered_segment_index(
                            std::span<const MinimizerT>{
                                minimizers.data(), minimizers.size()},
                            index,
                            filtered_pairs);
                    index.add_position_offset(range.cs_u * lpu);
                    seg_out[segment_index] = minimap2_port_with_index(
                        query_seq, target_seq, query_minimizers, index, params,
                        segmented_gpu_index_cache_ptr,
                        segment_gpu_identity(segment_index));
                }
            }});
        wave_lo = wave_hi;
    }

    std::size_t total_segment_minimizers = 0;
    std::size_t total_filtered_minimizers = 0;
    for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
        total_segment_minimizers += segment_minimizer_counts[segment_index];
        total_filtered_minimizers += segment_filtered_counts[segment_index];
    }
    bioseq_log_lazy<DBG>([&] {
        bioseq_log_progress<DBG>("[minimap_port_segmented] query-filtered transient index kept {} "
                        "of {} segment minimizers across {} segments | footprint={}MB "
                        "avail={}MB",
                total_filtered_minimizers, total_segment_minimizers, ranges.size(),
                bioseq::instr::detail::sysinfo::process_footprint_bytes() >> 20,
                bioseq::instr::detail::sysinfo::system_available_bytes() >> 20);
    });

    std::vector<MatchResult> merged;
    for (std::size_t segment_index = 0; segment_index < ranges.size(); ++segment_index) {
        const pipe::SegmentUnitRange& range = ranges[segment_index];
        for (auto& result : seg_out[segment_index].results) {
            if (result.target_start >= range.core_lo_bp
                && result.target_start < range.core_hi_bp) {
                merged.push_back(std::move(result));
            }
        }
    }
    return MatchResultCollection{std::move(merged)};
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

} // export
