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

#include <hwy/highway.h>

#include "common.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq_sequences.pipe;

import mlogging;
import bioseq_sequences;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers.minimap_int;
import bioseq_sequences.kmers.two_stacks;
import bioseq_cigar;
import alignment_hwy;
import alignment_hwy_utils;
import alignment_hwy_lane_helpers;
import alignment_hwy_ksw2_batched;
import block_aligner_hwy;
import bioseq_sequences.chaining.utils;
import bioseq_sequences.chaining.scalar;
import bioseq_sequences.chaining.simd;
import bioseq_sequences.chaining.rmq;
import bioseq_sequences.minimap_port;
import context;
import bioseq_instr;

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE::pipe {

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
concept IndexedBatchSink = requires (S& sink, std::span<E> a, const IndexT& idx) {
    { sink.on_batch_with_index(a, idx) } -> std::same_as<void>;
    { sink.on_completed() }              -> std::same_as<void>;
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
    EmittedToken<Sink> emit_batch_with_index(std::span<E>  data,
                                             const IndexT& index_ref) && {
        static_assert(IndexedBatchSink<Sink, E, IndexT>,
            "EmitToken::emit_batch_with_index: downstream sink does not "
            "implement on_batch_with_index(std::span<E>, IndexT const&).");
        DEBUG_ASSERT(sink_ptr_ != nullptr, "EmitToken consumed twice.");
        sink_ptr_->on_batch_with_index(data, index_ref);
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
    [[gnu::always_inline]] void on_batch_with_index(B&& batch, const IndexT& index_ref) {
        (void) self_.template process_with_index<Cep>(
            std::forward<B>(batch), index_ref, EmitToken<Sink>{sink_});
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
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
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
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
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
            const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params&,
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
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers<
                minimap_int_hasher, MinimizerT>(
                collector, range, range.begin(), range.end(),
                window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            Collector& collector, Range& range)
        {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_constexpr<
                W, K, minimap_int_hasher, MinimizerT>(
                collector, range, range.begin(), range.end());
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
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_simd<
                minimap_int_hasher, MinimizerT>(
                collector, range, range.begin(), range.end(),
                window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            Collector& collector, Range& range)
        {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_simd_constexpr<
                W, K, minimap_int_hasher, MinimizerT>(
                collector, range, range.begin(), range.end());
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
            Collector& collector, Range& range,
            std::size_t window_size, std::size_t kmer_size)
        {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_two_stack_nthash_simd<
                minimap_int_hasher, MinimizerT>(
                collector, range, range.begin(), range.end(),
                window_size, kmer_size);
        }

        template <std::size_t W, std::size_t K,
                  class MinimizerT, class Collector, class Range>
        [[gnu::always_inline]] static void call_constexpr(
            Collector& collector, Range& range)
        {
            ::bioseq::simd::HWY_NAMESPACE::collect_minimizers_two_stack_nthash_simd_constexpr<
                W, K, minimap_int_hasher, MinimizerT>(
                collector, range, range.begin(), range.end());
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
                  supports_compact_sequence_range TR>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            const compact_sequence_range<QR>&            query_seq,
            const compact_sequence_range<TR>&            target_seq,
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
                  supports_compact_sequence_range TR>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            const compact_sequence_range<QR>&            query_seq,
            const compact_sequence_range<TR>&            target_seq,
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
                  supports_compact_sequence_range TR>
        [[gnu::always_inline]] static void call(
            std::span<AlignRegion>                       regions,
            std::span<const ::bioseq::chaining::Anchor>  anchors,
            const compact_sequence_range<QR>&            query_seq,
            const compact_sequence_range<TR>&            target_seq,
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
    int32_t query_len, int32_t target_len,
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
          supports_compact_sequence_range QR>
struct MinimizerStage
    : StageBase<MinimizerStage<MinimizerT, Kernel, QR>, MinimizerT>
{
    static constexpr std::string_view stage_name = "MinimizerStage";

    const compact_sequence_range<QR>* query_seq_;
    int32_t runtime_w;
    int32_t runtime_k;

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

        auto sketches_slab = make_arena_slab<MinimizerT>();
        sketches_slab.reserve(detail::estimated_sketch_count(
            query_seq_->bp_size(), static_cast<int32_t>(window_size)));

        sketch_collector_arena_adapter<MinimizerT> collector{&sketches_slab};

        if constexpr (!Cep::generic_fallback) {
            Kernel::template call_constexpr<Cep::window_len, Cep::kmer_len, MinimizerT>(
                collector, *query_seq_);
        } else {
            Kernel::template call<MinimizerT>(
                collector, *query_seq_, window_size, kmer_size);
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

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process(B&& sketches, EmitToken<Sink>&& token) {
        using ElemT = std::remove_const_t<
            typename std::remove_cvref_t<B>::value_type>;
        return std::move(token).emit_batch_with_index(
            std::span<const ElemT>{sketches.data(), sketches.size()}, *index_);
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
    int32_t                                       query_len;

    template <is_constexpr_aligner_params Cep, class B, class IndexT, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_with_index(
        B&& sketches, const IndexT& target_index_ref, EmitToken<Sink>&& token)
    {
        const int32_t kmer_size = Cep::generic_fallback
            ? params.kmer_size
            : static_cast<int32_t>(Cep::kmer_len);

        using MinimizerT = std::remove_const_t<
            typename std::remove_cvref_t<B>::value_type>;

        // Pre-size the slab from the tight per-query estimate
        // (DOC0249 §21.4 `estimated_anchor_count`); growth past the estimate
        // doubles the slab inside the same Arena::Chain scope.
        auto anchors_slab = make_arena_slab<Anchor>();
        anchors_slab.reserve(detail::estimated_anchor_count(
            sketches.size(),
            static_cast<std::size_t>(query_len),
            static_cast<std::size_t>(target_index_ref.target_length()),
            kmer_size));

        collect_seed_hits<MinimizerT>(
            std::span<const MinimizerT>{sketches.data(), sketches.size()},
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

    int32_t query_len = 0;

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
    int32_t query_len;
    int32_t target_len;
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
          supports_compact_sequence_range TR>
struct AlignStage : StageBase<AlignStage<Kernel, QR, TR>, AlignRegion> {
    static constexpr std::string_view stage_name = "AlignStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    const ScoringMatrix<QR>*                      sm;
    const compact_sequence_range<QR>*             query_seq_;
    const compact_sequence_range<TR>*             target_seq_;
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
    int32_t                query_len  = 0;
    int32_t                target_len = 0;

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
          class MinimizerT, class IndexT>
[[nodiscard, gnu::always_inline]] inline auto make_minimap_pipeline(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ScoringMatrix<QR>&          scoring_matrix,
    MatchResultCollection&            output_collection,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    int32_t                           query_len,
    int32_t                           target_len)
{
    auto tail = make_pipeline<Cep>(
        IndexStage<IndexT>{ {}, &target_index },
        SeedHitStage{ {}, params, query_len },
        ChainStage<kernel_t<CA>>{ {}, params.to_chaining_params() },
        RegionStage{ {}, query_len },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        DivergenceStage<MinimizerT>{
            {}, &query_minimizers, query_len, target_len, params.kmer_size,
            params.compute_divergence },
        AlignStage<kernel_t<BAA>, QR, TR>{
            {}, params, &scoring_matrix, &query_seq, &target_seq, params.do_cigar },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        MapqStage{ {}, params, params.compute_mapq },
        SinkStage{ &output_collection, query_len, target_len }
    );

    if constexpr (EP == EntryPoint::FromSequence) {
        MinimizerStage<MinimizerT, kernel_t<MA>, QR> head{
            {}, &query_seq, params.window_size, params.kmer_size };
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
// Forward declaration so `pipeline_run_impl_batch` can delegate to the
// cross-query deferred-align path when the runtime flag is set.
template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] std::vector<MatchResultCollection>
pipeline_run_impl_batch_deferred_align(
    std::span<const compact_sequence_range<QR>> query_seqs,
    const compact_sequence_range<TR>&           target_seq,
    std::span<const std::vector<MinimizerT>>    query_minimizers_per_query,
    const IndexT&                               target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params);

template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] std::vector<MatchResultCollection> pipeline_run_impl_batch(
    std::span<const compact_sequence_range<QR>> query_seqs,
    const compact_sequence_range<TR>&           target_seq,
    std::span<const std::vector<MinimizerT>>    query_minimizers_per_query,
    const IndexT&                               target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    // DOC0254 §11.10 — dispatcher routes `KSW2_BATCHED &&
    // params.cross_query_batched` to the deferred-align scheduling shape.
    // The branch is gated by a trait-level constant
    // (`kernel_t<BAA>::supports_cross_query_batched`) so it never
    // materialises an `if constexpr (BAA == X)` ladder (R10).
    if constexpr (kernel_t<BAA>::supports_cross_query_batched) {
        if (params.cross_query_batched) {
            return pipeline_run_impl_batch_deferred_align<
                Cep, BAA, CA, MA, EP, QR, TR, MinimizerT, IndexT>(
                    query_seqs, target_seq, query_minimizers_per_query,
                    target_index, params);
        }
    }

    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    auto outer_chain_scope = context::current().push<context::Arena::Chain>();

    JustLog("[bioseq] pipe: pipeline_run_impl_batch entry — Cep(L={},K={},W={},fallback={}) "
            "BAA={} CA={} MA={} EP={} queries={} target_bp={}",
            Cep::sequence_len_log2, Cep::kmer_len, Cep::window_len,
            Cep::generic_fallback ? 1 : 0,
            static_cast<int>(BAA), static_cast<int>(CA), static_cast<int>(MA),
            static_cast<int>(EP), query_seqs.size(), target_seq.bp_size());

    std::vector<MatchResultCollection> out_per_query;
    out_per_query.reserve(query_seqs.size());

    const int32_t target_len = static_cast<int32_t>(target_seq.bp_size());
    const auto& scoring_matrix = thread_local_scoring_matrix<QR>(params);

    // Empty proto for the FromSequence path's unused minimizers argument.
    static const std::vector<MinimizerT> empty_minimizers_proto{};

    for (std::size_t qi = 0; qi < query_seqs.size(); ++qi) {
        const auto&   query_seq = query_seqs[qi];
        const int32_t query_len = static_cast<int32_t>(query_seq.bp_size());

        auto per_query_chain_scope = context::current().push<context::Arena::Chain>();

        MatchResultCollection out;
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));

        const auto& mins_for_this_query = (EP == EntryPoint::WithPrebuiltMinimizers)
            ? query_minimizers_per_query[qi]
            : empty_minimizers_proto;

        // Short-circuit early-empty cases matching the legacy entry's
        // returns at `minimap_port.cppm:2313` and :2325.
        if (query_len == 0 || target_len == 0) {
            out_per_query.push_back(std::move(out));
            continue;
        }

        auto pipeline = make_minimap_pipeline<Cep, BAA, CA, MA, EP, QR, TR>(
            query_seq, target_seq, mins_for_this_query, target_index,
            scoring_matrix, out, params, query_len, target_len);

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
        out_per_query.push_back(std::move(out));
    }

    return out_per_query;
}

// =============================================================================
// DOC0254 §11.10 — Cross-query deferred-align scheduling shape.
//
// Pipeline implementation parallel to `pipeline_run_impl_batch` selected when
// `BAA == KSW2_BATCHED && params.cross_query_batched`.
//
// The deferred variant opens one outer Arena::Chain scope around the whole
// batch and processes every query under it.  Per-query inner scopes still
// open/close, but the outer scope holds whatever cross-query bucket state
// `align_regions_batched_cross_query` chooses to accumulate (today: none —
// the per-query bucketing already saturates the bucket fill on
// VERY_LONG inputs per §11.10).  The pipeline layout is otherwise
// identical to the in-spec `pipeline_run_impl_batch` shape; the
// implementation is opt-in via `params.cross_query_batched`.
// =============================================================================

template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType     MA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] std::vector<MatchResultCollection>
pipeline_run_impl_batch_deferred_align(
    std::span<const compact_sequence_range<QR>> query_seqs,
    const compact_sequence_range<TR>&           target_seq,
    std::span<const std::vector<MinimizerT>>    query_minimizers_per_query,
    const IndexT&                               target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    std::optional<context::context>                 owned_context;
    std::optional<context::installed_context_scope> install;
    if (context::current_or_null() == nullptr) {
        owned_context.emplace();
        install.emplace(*owned_context);
    }
    auto outer_chain_scope = context::current().push<context::Arena::Chain>();
    auto batch_fill_scope  = context::current().push<context::Arena::Chain>();

    JustLog("[bioseq] pipe: pipeline_run_impl_batch_deferred_align entry — "
            "Cep(L={},K={},W={},fallback={}) BAA={} CA={} MA={} EP={} "
            "queries={} target_bp={} cross_query_batched=1",
            Cep::sequence_len_log2, Cep::kmer_len, Cep::window_len,
            Cep::generic_fallback ? 1 : 0,
            static_cast<int>(BAA), static_cast<int>(CA), static_cast<int>(MA),
            static_cast<int>(EP), query_seqs.size(), target_seq.bp_size());

    std::vector<MatchResultCollection> out_per_query;
    out_per_query.reserve(query_seqs.size());

    const int32_t target_len = static_cast<int32_t>(target_seq.bp_size());
    const auto& scoring_matrix = thread_local_scoring_matrix<QR>(params);

    static const std::vector<MinimizerT> empty_minimizers_proto{};

    for (std::size_t qi = 0; qi < query_seqs.size(); ++qi) {
        const auto&   query_seq = query_seqs[qi];
        const int32_t query_len = static_cast<int32_t>(query_seq.bp_size());

        auto per_query_chain_scope = context::current().push<context::Arena::Chain>();

        MatchResultCollection out;
        out.results.reserve(detail::estimated_result_count(query_len, target_len, params));

        const auto& mins_for_this_query = (EP == EntryPoint::WithPrebuiltMinimizers)
            ? query_minimizers_per_query[qi]
            : empty_minimizers_proto;

        if (query_len == 0 || target_len == 0) {
            out_per_query.push_back(std::move(out));
            continue;
        }

        auto pipeline = make_minimap_pipeline<Cep, BAA, CA, MA, EP, QR, TR>(
            query_seq, target_seq, mins_for_this_query, target_index,
            scoring_matrix, out, params, query_len, target_len);

        if constexpr (EP == EntryPoint::WithPrebuiltMinimizers) {
            pipeline.on_batch(std::span<const MinimizerT>{
                mins_for_this_query.data(), mins_for_this_query.size()});
        } else {
            pipeline.on_batch(std::span<const std::byte>{});
        }
        pipeline.on_completed();
        out_per_query.push_back(std::move(out));
    }

    return out_per_query;
}

// =============================================================================
// DOC0254 §11.10 — AlignCollectStage + align_regions_batched_cross_query.
//
// The deferred-align scheduling shape captures (regions, anchors,
// query_seq*) per query into a `PerQueryAlignState<QR, MinimizerT>`
// slot via `AlignCollectStage`, then `align_regions_batched_cross_query`
// folds the per-query fill lists into one big slab, buckets across all
// queries' fills, runs the per-bucket DP, and stitches per-region CIGAR
// back into each query's regions before the Phase C stage chain
// (primary/secondary/filter/mapq/sink) runs.
//
// Today's runtime delegates to the per-query `align_regions_batched`
// inside the regular pipeline; the structural per-query slot remains
// available for the inner-loop substitution (fused inter-sequence SIMD
// DP) that a follow-up may introduce.  Building this two-phase shape
// here keeps the deferred path independent of the per-query path.
// =============================================================================

template <supports_compact_sequence_range QR, class MinimizerT>
struct PerQueryAlignState {
    const compact_sequence_range<QR>* query_seq = nullptr;
    int32_t                           query_len = 0;
    ArenaSlab<AlignRegion>            regions{nullptr, 0u, 0u, nullptr};
    ArenaSlab<::bioseq::chaining::Anchor> anchors{nullptr, 0u, 0u, nullptr};
    MatchResultCollection             out{};
};

/**
 * @brief Phase-A terminal stage for the deferred-align pipeline.
 *
 * Records the upstream `(regions, anchors)` pair plus the per-query
 * query_seq pointer into a `PerQueryAlignState` slot and emits no
 * further batches; the downstream sink (a no-op) absorbs an empty
 * emission so the pipeline composition stays uniform.  Used only by
 * `pipeline_run_impl_batch_deferred_align`.
 */
template <supports_compact_sequence_range QR, class MinimizerT>
struct AlignCollectStage
    : StageBase<AlignCollectStage<QR, MinimizerT>, AlignRegion>
{
    static constexpr std::string_view stage_name = "AlignCollectStage";

    PerQueryAlignState<QR, MinimizerT>* slot;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]]
    EmittedToken<Sink> process_pair(RB&& regions, AB&& anchors,
                                    EmitToken<Sink>&& token)
    {
        // Move-in the regions / anchors slabs to the per-query slot via
        // span-copy: the caller owns the arena scope so the spans are
        // safe through the deferred-align Phase B/C steps.
        const std::size_t n_r = regions.size();
        const std::size_t n_a = anchors.size();
        slot->regions.reserve_for_placement_new(n_r);
        for (std::size_t i = 0; i < n_r; ++i) {
            slot->regions.construct_at_back(std::move(regions.data()[i]));
        }
        slot->anchors.reserve(n_a);
        for (std::size_t i = 0; i < n_a; ++i) {
            slot->anchors.push_back(anchors.data()[i]);
        }
        return std::move(token).emit_batch_pair(
            std::span<AlignRegion>{slot->regions.data(), slot->regions.size()},
            std::span<const ::bioseq::chaining::Anchor>{
                slot->anchors.data(), slot->anchors.size()});
    }

    template <is_constexpr_aligner_params Cep>
    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = "Align-collect (deferred batching)";
        desc.algorithm = std::move(algo);
        return desc;
    }
};

/**
 * @brief Phase B of the deferred-align scheduling shape — folds every
 *        per-query fill list into one big slab, buckets across queries,
 *        runs the per-bucket DP, and stitches the per-fill CIGAR back
 *        into each query's regions.
 *
 * The current implementation iterates per-query through
 * `align_regions_batched` — which buckets that query's fills and runs
 * the per-bucket DP — keeping the per-query slabs alive across the
 * call.  Substituting an explicit cross-query bucket accumulator is a
 * local change to this function and does not affect the surrounding
 * dispatcher or `AlignCollectStage`.
 */
template <supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT>
void align_regions_batched_cross_query(
    std::span<PerQueryAlignState<QR, MinimizerT>> per_query,
    const compact_sequence_range<TR>&             target_seq,
    const ScoringMatrix<QR>&                      scoring_matrix,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    for (auto& q : per_query) {
        if (q.query_seq == nullptr || q.regions.size() == 0) continue;
        align_regions_batched<QR, TR>(
            std::span<AlignRegion>{q.regions.data(), q.regions.size()},
            std::span<const ::bioseq::chaining::Anchor>{
                q.anchors.data(), q.anchors.size()},
            *q.query_seq, target_seq, scoring_matrix, params);
    }
}

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
          class MinimizerT, class IndexT>
[[nodiscard, gnu::always_inline]] inline MatchResultCollection pipeline_run_impl(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    const compact_sequence_range<QR>* one_query = &query_seq;
    const std::vector<MinimizerT>*    one_mins  = &query_minimizers;
    auto results = pipeline_run_impl_batch<Cep, BAA, CA, MA, EP, QR, TR, MinimizerT, IndexT>(
        std::span<const compact_sequence_range<QR>>{one_query, 1},
        target_seq,
        std::span<const std::vector<MinimizerT>>{one_mins, 1},
        target_index, params);
    return std::move(results.front());
}

// =============================================================================
// 9.3 Inner dispatch — algorithm cascade (DOC0249 §9.3)
// =============================================================================

namespace detail {

template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType      CA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] MatchResultCollection dispatch_on_chain_and_mini(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
    if constexpr (EP == EntryPoint::WithPrebuiltMinimizers) {
        // MinimizerAlgorithmType is irrelevant on the pre-built path — pick
        // any value and let the unused MinimizerStage fall away.
        return pipeline_run_impl<Cep, BAA, CA,
            MinimizerAlgorithmType::SCALAR, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
    } else {
        switch (params.minimizer_algorithm) {
            case MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW:
                return pipeline_run_impl<Cep, BAA, CA,
                    MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW, EP, QR, TR>(
                        query_seq, target_seq, query_minimizers, target_index, params);
            case MinimizerAlgorithmType::TWO_STACK_NTHASH:
                return pipeline_run_impl<Cep, BAA, CA,
                    MinimizerAlgorithmType::TWO_STACK_NTHASH, EP, QR, TR>(
                        query_seq, target_seq, query_minimizers, target_index, params);
            case MinimizerAlgorithmType::SCALAR:
            default:
                return pipeline_run_impl<Cep, BAA, CA,
                    MinimizerAlgorithmType::SCALAR, EP, QR, TR>(
                        query_seq, target_seq, query_minimizers, target_index, params);
        }
    }
}

template <is_constexpr_aligner_params Cep,
          ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] MatchResultCollection dispatch_on_chain(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    switch (params.chaining_algorithm) {
        case ChainingAlgorithmType::SIMD_MM2:
            return dispatch_on_chain_and_mini<Cep, BAA,
                ChainingAlgorithmType::SIMD_MM2, EP, QR, TR>(
                    query_seq, target_seq, query_minimizers, target_index, params);
        case ChainingAlgorithmType::RMQ:
            return dispatch_on_chain_and_mini<Cep, BAA,
                ChainingAlgorithmType::RMQ, EP, QR, TR>(
                    query_seq, target_seq, query_minimizers, target_index, params);
        case ChainingAlgorithmType::SCALAR_MM2:
        default:
            return dispatch_on_chain_and_mini<Cep, BAA,
                ChainingAlgorithmType::SCALAR_MM2, EP, QR, TR>(
                    query_seq, target_seq, query_minimizers, target_index, params);
    }
}

template <is_constexpr_aligner_params Cep,
          EntryPoint                                                EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] MatchResultCollection dispatch_on_algorithms(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    switch (params.base_alignment_algorithm) {
        case BaseAlignmentAlgorithmType::KSW2:
            return dispatch_on_chain<Cep, BaseAlignmentAlgorithmType::KSW2, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        case BaseAlignmentAlgorithmType::KSW2_BATCHED:
            return dispatch_on_chain<Cep, BaseAlignmentAlgorithmType::KSW2_BATCHED, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        case BaseAlignmentAlgorithmType::BLOCK_ALIGNER:
        default:
            return dispatch_on_chain<Cep, BaseAlignmentAlgorithmType::BLOCK_ALIGNER, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
    }
}

template <std::size_t L, std::size_t K,
          EntryPoint EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] MatchResultCollection dispatch_on_specialisation(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    using SpecialisedCep = ConstexprAlignerParams<10u, K, L>;
    using FallbackCep    = ConstexprAlignerFallbackParams<L>;
    if (params.window_size == 10
        && params.kmer_size == static_cast<int32_t>(K))
    {
        return dispatch_on_algorithms<SpecialisedCep, EP, QR, TR>(
            query_seq, target_seq, query_minimizers, target_index, params);
    }
    return dispatch_on_algorithms<FallbackCep, EP, QR, TR>(
        query_seq, target_seq, query_minimizers, target_index, params);
}

} // namespace detail

/**
 * @brief Top-level dispatch ladder — selects (L, K) bucket from runtime
 * sequence sizes and `params.kmer_size`, then fans out through the algorithm
 * cascade (DOC0249 §9.2).
 *
 * Mirrors the existing ladder at `minimap_port.cppm:2604-2699` exactly but
 * now produces `(Cep, BAA, CA, MA, EP)` template-resolved pipeline runs at
 * each leaf instead of recursing into the legacy `minimap_port_with_repr`
 * → `minimap2_port_with_index_impl` chain.
 */
template <EntryPoint EP,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR,
          class MinimizerT, class IndexT>
[[nodiscard]] MatchResultCollection dispatch_pipeline(
    const compact_sequence_range<QR>& query_seq,
    const compact_sequence_range<TR>& target_seq,
    const std::vector<MinimizerT>&    query_minimizers,
    const IndexT&                     target_index,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params)
{
    const std::size_t max_bp = std::max(query_seq.bp_size(), target_seq.bp_size());

    if (params.kmer_size == 15) {
        if (max_bp <= (std::size_t{1} << 16))
            return detail::dispatch_on_specialisation<16, 15, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 20))
            return detail::dispatch_on_specialisation<20, 15, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 24))
            return detail::dispatch_on_specialisation<24, 15, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 28))
            return detail::dispatch_on_specialisation<28, 15, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 32))
            return detail::dispatch_on_specialisation<32, 15, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 48))
            return detail::dispatch_on_specialisation<48, 15, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        return detail::dispatch_on_specialisation<63, 15, EP, QR, TR>(
            query_seq, target_seq, query_minimizers, target_index, params);
    }
    if (params.kmer_size == 7) {
        if (max_bp <= (std::size_t{1} << 15))
            return detail::dispatch_on_specialisation<15, 7, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 28))
            return detail::dispatch_on_specialisation<28, 7, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 48))
            return detail::dispatch_on_specialisation<48, 7, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        return detail::dispatch_on_specialisation<63, 7, EP, QR, TR>(
            query_seq, target_seq, query_minimizers, target_index, params);
    }
    if (params.kmer_size == 27) {
        if (max_bp <= (std::size_t{1} << 28))
            return detail::dispatch_on_specialisation<28, 27, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        if (max_bp <= (std::size_t{1} << 48))
            return detail::dispatch_on_specialisation<48, 27, EP, QR, TR>(
                query_seq, target_seq, query_minimizers, target_index, params);
        return detail::dispatch_on_specialisation<63, 27, EP, QR, TR>(
            query_seq, target_seq, query_minimizers, target_index, params);
    }
    // Non-canonical K — pick the closest L and let the fallback Cep cover it.
    return detail::dispatch_on_specialisation<28, 15, EP, QR, TR>(
        query_seq, target_seq, query_minimizers, target_index, params);
}

// =============================================================================
// 12.2 describe_pipeline_for — describe path used by EXPLAIN
// =============================================================================

namespace detail {

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
    int32_t query_len, int32_t target_len)
{
    using MinimizerT = typename Cep::minimizer_repr_t;
    const std::vector<MinimizerT> empty_minimizers{};
    TargetIndex                   empty_index{};
    MatchResultCollection         ignored_out{};
    const auto& sm = thread_local_scoring_matrix<QR>(params);

    auto pipeline = make_minimap_pipeline<Cep, BAA, CA, MA, EP, QR, TR>(
        query_seq, target_seq, empty_minimizers, empty_index, sm, ignored_out,
        params, query_len, target_len);
    return describe_with_params<Cep>(pipeline);
}

template <is_constexpr_aligner_params CepHere,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR>
[[nodiscard]] PipelineDescription describe_pipeline_dispatch_inner(
    const compact_sequence_range<QR>& query_seq_proto,
    const compact_sequence_range<TR>& target_seq_proto,
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    int32_t query_len, int32_t target_len)
{
    using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;
    using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;

    switch (params.base_alignment_algorithm) {
        case BaseAlignmentAlgorithmType::KSW2:
            switch (params.chaining_algorithm) {
                case ChainingAlgorithmType::SIMD_MM2:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::KSW2,
                        ChainingAlgorithmType::SIMD_MM2,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
                case ChainingAlgorithmType::RMQ:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::KSW2,
                        ChainingAlgorithmType::RMQ,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
                case ChainingAlgorithmType::SCALAR_MM2:
                default:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::KSW2,
                        ChainingAlgorithmType::SCALAR_MM2,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
            }
        case BaseAlignmentAlgorithmType::KSW2_BATCHED:
            switch (params.chaining_algorithm) {
                case ChainingAlgorithmType::SIMD_MM2:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::KSW2_BATCHED,
                        ChainingAlgorithmType::SIMD_MM2,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
                case ChainingAlgorithmType::RMQ:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::KSW2_BATCHED,
                        ChainingAlgorithmType::RMQ,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
                case ChainingAlgorithmType::SCALAR_MM2:
                default:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::KSW2_BATCHED,
                        ChainingAlgorithmType::SCALAR_MM2,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
            }
        case BaseAlignmentAlgorithmType::BLOCK_ALIGNER:
        default:
            switch (params.chaining_algorithm) {
                case ChainingAlgorithmType::SIMD_MM2:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::BLOCK_ALIGNER,
                        ChainingAlgorithmType::SIMD_MM2,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
                case ChainingAlgorithmType::RMQ:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::BLOCK_ALIGNER,
                        ChainingAlgorithmType::RMQ,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
                case ChainingAlgorithmType::SCALAR_MM2:
                default:
                    return describe_pipeline_leaf<CepHere,
                        BaseAlignmentAlgorithmType::BLOCK_ALIGNER,
                        ChainingAlgorithmType::SCALAR_MM2,
                        MinimizerAlgorithmType::SCALAR,
                        EntryPoint::WithPrebuiltMinimizers, QR, TR>(
                            query_seq_proto, target_seq_proto, params, query_len, target_len);
            }
    }
}

} // namespace detail

/**
 * @brief Build a `PipelineDescription` matching the pipeline that would run
 * for the given (params, sequence sizes).
 *
 * Called once per Postgres scan from `BeginCustomScan` (cold path).  Walks
 * the same dispatch ladder as `dispatch_pipeline` but invokes the
 * `describe()` chain instead of `on_batch(...)`.  Constructs dummy
 * (empty-minimizers, empty-index) per-stage configuration; safe because every
 * stage's `describe()` consults only its by-value config fields (DOC0249
 * §6.1 invariant).
 *
 * Cep bucket is fixed to L=28 here because EXPLAIN output is identical
 * regardless of the L bucket — Cep.window_len / Cep.kmer_len are what the
 * renderer surfaces and those depend on the specialised-vs-fallback choice,
 * not on L.
 */
template <supports_compact_sequence_range QR,
          supports_compact_sequence_range TR>
[[nodiscard]] PipelineDescription describe_pipeline_for(
    const ::bioseq::simd::HWY_NAMESPACE::Minimap2Params& params,
    const compact_sequence_range<QR>& query_seq_proto,
    const compact_sequence_range<TR>& target_seq_proto)
{
    using Cep         = ConstexprAlignerParams<10u, 15u, 28u>;
    using FallbackCep = ConstexprAlignerFallbackParams<28u>;

    const int32_t query_len  = static_cast<int32_t>(query_seq_proto.bp_size());
    const int32_t target_len = static_cast<int32_t>(target_seq_proto.bp_size());

    const bool specialised =
        params.window_size == 10 && params.kmer_size == 15;
    if (specialised) {
        return detail::describe_pipeline_dispatch_inner<Cep, QR, TR>(
            query_seq_proto, target_seq_proto, params, query_len, target_len);
    }
    return detail::describe_pipeline_dispatch_inner<FallbackCep, QR, TR>(
        query_seq_proto, target_seq_proto, params, query_len, target_len);
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
          ::bioseq::simd::HWY_NAMESPACE::MinimizerIndex IndexT>
[[nodiscard]] MatchResultCollection minimap2_port_with_index(
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    const IndexT& prebuilt_target_index,
    const Minimap2Params& params = Minimap2Params{})
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port_with_index");
    auto _scope = bioseq::instr::region(_desc);
    return ::bioseq::simd::HWY_NAMESPACE::pipe::dispatch_pipeline<
        ::bioseq::simd::HWY_NAMESPACE::pipe::EntryPoint::WithPrebuiltMinimizers,
        QueryRange, TargetRange, MinimizerT, IndexT>(
            query_seq, target_seq, query_minimizers, prebuilt_target_index, params);
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
          supports_compact_sequence_range TargetRange>
[[nodiscard]] MatchResultCollection minimap2_port(
    const compact_sequence_range<QueryRange>& query_seq,
    const compact_sequence_range<TargetRange>& target_seq,
    const std::vector<MinimizerT>& query_minimizers,
    const std::vector<MinimizerT>& target_minimizers,
    const Minimap2Params& params = Minimap2Params{})
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap2_port");
    auto _scope = bioseq::instr::region(_desc);
    const int32_t target_len = static_cast<int32_t>(target_seq.bp_size());
    TargetIndex target_index;
    target_index.build(target_minimizers, target_len);
    return minimap2_port_with_index<MinimizerT, QueryRange, TargetRange, TargetIndex>(
        query_seq, target_seq, query_minimizers, target_index, params);
}

namespace pipe_entry_detail {

/**
 * @brief Per-bucket helper for the from-sequence `minimap_port` entry.
 *
 * Owns one `Arena::Chain` scope around the target minimiser sketching + index
 * build.  Sketching uses the chosen `MinimizerAlgorithmType` selected by a
 * runtime switch (mirrors the legacy `minimap_port_with_repr` body).  Once
 * the target index is built, the function delegates to
 * `minimap2_port_with_index` so the per-query MinimizerStage inside
 * `pipe::dispatch_pipeline` handles query sketching.
 */
template <class MinimizerT,
          supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange>
[[nodiscard]] MatchResultCollection minimap_port_with_repr(
    compact_sequence_range<QueryRange>& query_seq,
    compact_sequence_range<TargetRange>& target_seq,
    const Minimap2Params& params)
{
    static const auto* _desc =
        bioseq::instr::register_region_static("minimap_port");
    auto _scope = bioseq::instr::region(_desc);

    context::context pipeline_context;
    context::installed_context_scope context_install(pipeline_context);

    const std::size_t window_len = static_cast<std::size_t>(params.window_size);
    const std::size_t kmer_len   = static_cast<std::size_t>(params.kmer_size);

    sketch_collector_t<MinimizerT> target_collector;
    {
        static const auto* _mm_sketch_target_desc =
            bioseq::instr::register_region_static("mm_sketch");
        auto _mm_sketch_target = bioseq::instr::region(_mm_sketch_target_desc);
        target_collector.reserve(
            target_seq.bp_size() / std::max<std::size_t>(window_len, 1) + 64);
        switch (params.minimizer_algorithm) {
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW:
                collect_minimizers_simd<minimap_int_hasher, MinimizerT>(
                    target_collector, target_seq,
                    target_seq.begin(), target_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH:
                collect_minimizers_two_stack_nthash_simd<minimap_int_hasher, MinimizerT>(
                    target_collector, target_seq,
                    target_seq.begin(), target_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR:
            default:
                collect_minimizers<minimap_int_hasher, MinimizerT>(
                    target_collector, target_seq,
                    target_seq.begin(), target_seq.end(),
                    window_len, kmer_len);
                break;
        }
    }
    const auto target_minimizers = std::move(target_collector).extract_sketches_data();

    sketch_collector_t<MinimizerT> query_collector;
    {
        static const auto* _mm_sketch_query_desc =
            bioseq::instr::register_region_static("mm_sketch");
        auto _mm_sketch_query = bioseq::instr::region(_mm_sketch_query_desc);
        query_collector.reserve(
            query_seq.bp_size() / std::max<std::size_t>(window_len, 1) + 64);
        switch (params.minimizer_algorithm) {
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW:
                collect_minimizers_simd<minimap_int_hasher, MinimizerT>(
                    query_collector, query_seq,
                    query_seq.begin(), query_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH:
                collect_minimizers_two_stack_nthash_simd<minimap_int_hasher, MinimizerT>(
                    query_collector, query_seq,
                    query_seq.begin(), query_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR:
            default:
                collect_minimizers<minimap_int_hasher, MinimizerT>(
                    query_collector, query_seq,
                    query_seq.begin(), query_seq.end(),
                    window_len, kmer_len);
                break;
        }
    }
    const auto query_minimizers = std::move(query_collector).extract_sketches_data();

    return minimap2_port<MinimizerT, QueryRange, TargetRange>(
        query_seq, target_seq, query_minimizers, target_minimizers, params);
}

} // namespace pipe_entry_detail

/**
 * @brief Top-level minimap2 entry — raw query/target sequences only
 * (DOC0249 §9.7 / legacy `minimap_port`).
 *
 * Picks a `MinimizerRepr<L, K>` bucket from `max(query_bp, target_bp)` and
 * `params.kmer_size`, then forwards to the per-bucket
 * `pipe_entry_detail::minimap_port_with_repr`.  Each per-bucket helper
 * sketches both target and query minimisers, builds the target index, and
 * delegates to `minimap2_port_with_index` — which in turn calls
 * `pipe::dispatch_pipeline<EP::WithPrebuiltMinimizers>`.
 *
 * The L-bucket switch mirrors the legacy ladder at
 * `minimap_port.cppm:2604-2699` exactly.
 */
template <supports_compact_sequence_range QueryRange,
          supports_compact_sequence_range TargetRange>
[[nodiscard]] MatchResultCollection minimap_port(
    compact_sequence_range<QueryRange>& query_seq,
    compact_sequence_range<TargetRange>& target_seq,
    const Minimap2Params& params = Minimap2Params{})
{
    const std::size_t max_len =
        std::max(query_seq.bp_size(), target_seq.bp_size());
    const int32_t kmer_len = params.kmer_size;

    if (kmer_len == 15) {
        if (max_len <= (std::size_t{1} << 16))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<16u, 15u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 20))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<20u, 15u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 24))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<24u, 15u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 28))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<28u, 15u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 32))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<32u, 15u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 48))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<48u, 15u>>(
                query_seq, target_seq, params);
        return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<63u, 15u>>(
            query_seq, target_seq, params);
    }
    if (kmer_len == 7) {
        if (max_len <= (std::size_t{1} << 15))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<15u, 7u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 28))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<28u, 7u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 48))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<48u, 7u>>(
                query_seq, target_seq, params);
        return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<63u, 7u>>(
            query_seq, target_seq, params);
    }
    if (kmer_len == 27) {
        if (max_len <= (std::size_t{1} << 28))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<28u, 27u>>(
                query_seq, target_seq, params);
        if (max_len <= (std::size_t{1} << 48))
            return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<48u, 27u>>(
                query_seq, target_seq, params);
        return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<63u, 27u>>(
            query_seq, target_seq, params);
    }
    if (max_len <= (std::size_t{1} << 28))
        return pipe_entry_detail::minimap_port_with_repr<MinimapSketchData>(
            query_seq, target_seq, params);
    return pipe_entry_detail::minimap_port_with_repr<MinimizerRepr<63u, 15u>>(
        query_seq, target_seq, params);
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

} // export
