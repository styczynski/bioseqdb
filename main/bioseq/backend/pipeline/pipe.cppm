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

    template <class B>
    [[gnu::always_inline]] void on_batch(B&& batch) {
        self_.template process<Cep>(std::forward<B>(batch), sink_);
    }

    template <class B1, class B2>
    [[gnu::always_inline]] void on_batch_pair(B1&& first_batch, B2&& second_batch) {
        self_.template process_pair<Cep>(
            std::forward<B1>(first_batch), std::forward<B2>(second_batch), sink_);
    }

    template <class B, class IndexT>
    [[gnu::always_inline]] void on_batch_with_index(B&& batch, const IndexT& index_ref) {
        self_.template process_with_index<Cep>(
            std::forward<B>(batch), index_ref, sink_);
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
// The wire type between stages is `std::span<E>` directly.  The owning
// slab type below is used by stages internally to materialise kernel output
// into the per-pipeline chain arena so downstream spans remain valid.

/**
 * @brief Arena-backed owning slab.
 *
 * Allocates via `context::current().alloc_chain_array<E>(...)` so the
 * underlying storage is reclaimed at the surrounding `Arena::Chain` scope
 * unwind.  `push_back` grows geometrically; orphaned slabs land in the
 * arena and are freed in O(1) at scope exit.
 *
 * Two patterns are supported:
 *
 *   1. Trivially-copyable `E` (e.g. `Anchor`, `Chain`, `MinimizerRepr`):
 *      callers use `reserve(N)` + `push_back` or `reserve(N)` + write
 *      through `data()` + `resize(actual_n)`.  The arena scope reclaims
 *      the slab without per-element work; element destructors are no-op.
 *
 *   2. Non-trivially-destructible `E` (e.g. `AlignRegion` with its
 *      `std::vector<CigarOp>` member): callers MUST use
 *      `construct_at_back(args...)` to placement-new each element and the
 *      slab's own destructor walks `[0, len_)` calling `~E()` so heap-owned
 *      fields are released before the arena pointer-bump reclaims the slab.
 */
template <class E>
struct ArenaSlab {
    using element_type = E;

    E*               ptr_   = nullptr;
    std::size_t      len_   = 0;
    std::size_t      cap_   = 0;
    context::context* arena_ = nullptr;

    constexpr ArenaSlab() noexcept = default;
    constexpr ArenaSlab(E* data_ptr, std::size_t length, std::size_t capacity,
                        context::context* arena) noexcept
        : ptr_(data_ptr), len_(length), cap_(capacity), arena_(arena) {}

    // Non-copyable / move-only — destruction must run exactly once.
    ArenaSlab(const ArenaSlab&)            = delete;
    ArenaSlab& operator=(const ArenaSlab&) = delete;

    ArenaSlab(ArenaSlab&& other) noexcept
        : ptr_(other.ptr_), len_(other.len_), cap_(other.cap_), arena_(other.arena_)
    {
        other.ptr_   = nullptr;
        other.len_   = 0;
        other.cap_   = 0;
        other.arena_ = nullptr;
    }

    ArenaSlab& operator=(ArenaSlab&& other) noexcept {
        if (this != &other) {
            destroy_live_elements();
            ptr_   = other.ptr_;
            len_   = other.len_;
            cap_   = other.cap_;
            arena_ = other.arena_;
            other.ptr_   = nullptr;
            other.len_   = 0;
            other.cap_   = 0;
            other.arena_ = nullptr;
        }
        return *this;
    }

    ~ArenaSlab() noexcept {
        destroy_live_elements();
    }

    void reserve(std::size_t expected_capacity) {
        static_assert(std::is_trivially_copyable_v<E>,
            "ArenaSlab::reserve uses memcpy to relocate live elements during "
            "growth; only trivially-copyable element types are supported on "
            "this path.  Non-trivial types must size the slab once via the "
            "constructor and never call reserve a second time.");
        if (cap_ >= expected_capacity) return;
        auto allocated_span = arena_->template alloc_chain_array<E, alignof(E)>(expected_capacity);
        if (len_ > 0) std::memcpy(allocated_span.data(), ptr_, len_ * sizeof(E));
        ptr_ = allocated_span.data();
        cap_ = expected_capacity;
    }

    /**
     * @brief Allocate exactly @p capacity uninitialised slots in the arena.
     *
     * Use this once (not in a growth loop) for non-trivially-destructible
     * `E`.  Callers then `construct_at_back(args...)` per element; the
     * slab's destructor walks the live prefix and calls `~E()` on each.
     */
    void reserve_for_placement_new(std::size_t capacity) {
        DEBUG_ASSERT(cap_ == 0, "reserve_for_placement_new must be called on a fresh slab");
        if (capacity == 0) return;
        auto allocated_span = arena_->template alloc_chain_array<E, alignof(E)>(capacity);
        ptr_ = allocated_span.data();
        cap_ = capacity;
        len_ = 0;
    }

    /**
     * @brief Construct a new `E` in the next free slot, returning a reference.
     *
     * Caller must have called `reserve_for_placement_new(N)` beforehand and
     * must not exceed `N` constructions.  Used for non-trivial types so the
     * slab's destructor can call `~E()` on each live element.
     */
    template <class... Args>
    [[gnu::always_inline]] E& construct_at_back(Args&&... args) {
        DEBUG_ASSERT(len_ < cap_, "construct_at_back beyond reserved capacity");
        E* slot = new (ptr_ + len_) E(std::forward<Args>(args)...);
        ++len_;
        return *slot;
    }

    /**
     * @brief `push_back` overload restricted to trivially-copyable types.
     *
     * Geometric growth via `memcpy` of the live prefix; non-trivial types
     * must use the `reserve_for_placement_new` + `construct_at_back`
     * pattern instead so destructors are called.
     */
    void push_back(E element) {
        static_assert(std::is_trivially_copyable_v<E>,
            "ArenaSlab::push_back uses memcpy + trivial relocation during "
            "growth; non-trivially-copyable element types must use the "
            "reserve_for_placement_new + construct_at_back pattern.");
        if (len_ == cap_) {
            const std::size_t new_cap = cap_ ? cap_ * 2 : 64;
            auto allocated_span = arena_->template alloc_chain_array<E, alignof(E)>(new_cap);
            if (len_ > 0) std::memcpy(allocated_span.data(), ptr_, len_ * sizeof(E));
            ptr_ = allocated_span.data();
            cap_ = new_cap;
        }
        ptr_[len_++] = std::move(element);
    }

    void resize(std::size_t new_len) noexcept {
        DEBUG_ASSERT(new_len <= cap_, "ArenaSlab::resize beyond reserved capacity");
        len_ = new_len;
    }

    [[nodiscard]] E*          data() noexcept       { return ptr_; }
    [[nodiscard]] const E*    data() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size() const noexcept { return len_; }
    [[nodiscard]] std::span<E>       as_span()       noexcept { return {ptr_, len_}; }
    [[nodiscard]] std::span<const E> as_const_span() const noexcept { return {ptr_, len_}; }

private:
    void destroy_live_elements() noexcept {
        if constexpr (!std::is_trivially_destructible_v<E>) {
            if (ptr_ == nullptr) return;
            for (std::size_t reverse_idx = len_; reverse_idx > 0; --reverse_idx) {
                ptr_[reverse_idx - 1].~E();
            }
        }
        len_ = 0;
    }
};

/**
 * @brief Helper: make a fresh ArenaSlab bound to the current pipeline arena.
 */
template <class E>
[[nodiscard]] inline ArenaSlab<E> make_arena_slab() noexcept {
    return ArenaSlab<E>{nullptr, 0u, 0u, &context::current()};
}

// =============================================================================
// 11. Metadata surface (DOC0249 §11)
// =============================================================================

struct NamedProperty {
    std::string name;
    std::string value;
};

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

[[nodiscard]] constexpr std::size_t estimated_cigar_op_count(
    int32_t block_length) noexcept {
    const int32_t clamped = block_length > 0 ? block_length : 0;
    return static_cast<std::size_t>(clamped) + 16u;
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
 * Calls the chosen minimizer-extraction kernel selected at compile time via
 * `MinimizerAlgorithmType`.  The stage ignores its input batch and instead
 * sketches the pre-captured `query_seq_` (see DOC0249 §8.1).
 */
template <class MinimizerT,
          ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType MA,
          supports_compact_sequence_range QR>
struct MinimizerStage
    : StageBase<MinimizerStage<MinimizerT, MA, QR>, MinimizerT>
{
    static constexpr std::string_view stage_name = "MinimizerStage";

    const compact_sequence_range<QR>* query_seq_;
    int32_t runtime_w;
    int32_t runtime_k;

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]] void process(B&& /*ignored*/, Sink& sink) {
        const std::size_t window_size = Cep::generic_fallback
            ? static_cast<std::size_t>(runtime_w)
            : Cep::window_len;
        const std::size_t kmer_size = Cep::generic_fallback
            ? static_cast<std::size_t>(runtime_k)
            : Cep::kmer_len;

        sketch_collector_t<MinimizerT> collector;
        collector.reserve(detail::estimated_sketch_count(
            query_seq_->bp_size(), static_cast<int32_t>(window_size)));

        using ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType;
        if constexpr (MA == MinimizerAlgorithmType::SIMD_MINIMAP2_SLOW) {
            collect_minimizers_simd<minimap_int_hasher>(
                collector, *query_seq_,
                query_seq_->begin(), query_seq_->end(),
                window_size, kmer_size);
        } else if constexpr (MA == MinimizerAlgorithmType::TWO_STACK_NTHASH) {
            collect_minimizers_two_stack_nthash_simd<minimap_int_hasher>(
                collector, *query_seq_,
                query_seq_->begin(), query_seq_->end(),
                window_size, kmer_size);
        } else {
            static_assert(MA == MinimizerAlgorithmType::SCALAR,
                "Unhandled MinimizerAlgorithmType variant");
            collect_minimizers<minimap_int_hasher>(
                collector, *query_seq_,
                query_seq_->begin(), query_seq_->end(),
                window_size, kmer_size);
        }

        // Materialise minimizers in the arena slab so they outlive the
        // collector and downstream stages can hold span references to them.
        auto sketches_vec = std::move(collector).extract_sketches_data();
        auto sketches_slab = make_arena_slab<MinimizerT>();
        sketches_slab.reserve(sketches_vec.size());
        if (!sketches_vec.empty()) {
            std::memcpy(sketches_slab.data(), sketches_vec.data(),
                        sketches_vec.size() * sizeof(MinimizerT));
        }
        sketches_slab.resize(sketches_vec.size());
        sink.on_batch(std::span<const MinimizerT>{
            sketches_slab.data(), sketches_slab.size()});
    }

    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = std::string{
            ::bioseq::simd::HWY_NAMESPACE::minimizer_algorithm_name(MA)};
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
    [[gnu::always_inline]] void process(B&& sketches, Sink& sink) {
        sink.on_batch_with_index(std::forward<B>(sketches), *index_);
    }

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
 * Wraps the existing `collect_seed_hits` kernel; copies the kernel's returned
 * vector into an arena-backed slab so downstream chains can hold spans
 * without lifetime concerns.
 */
struct SeedHitStage : StageBase<SeedHitStage, ::bioseq::chaining::Anchor> {
    static constexpr std::string_view stage_name = "SeedHitStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    int32_t                                       query_len;

    template <is_constexpr_aligner_params Cep, class B, class IndexT, class Sink>
    [[gnu::always_inline]] void process_with_index(
        B&& sketches, const IndexT& target_index_ref, Sink& sink)
    {
        const int32_t kmer_size = Cep::generic_fallback
            ? params.kmer_size
            : static_cast<int32_t>(Cep::kmer_len);

        using MinimizerT = std::remove_const_t<
            typename std::remove_cvref_t<B>::value_type>;

        // Kernel now accepts std::span directly (DOC0249 M5).  The radix-sort
        // inside `collect_seed_hits` still emits its anchors into a
        // `std::vector<Anchor>`; we materialise that into an arena slab so
        // downstream stages can hold span references whose lifetime tracks
        // the per-query Arena::Chain scope opened in pipeline_run_impl_batch.
        auto anchors_vec = collect_seed_hits<MinimizerT>(
            std::span<const MinimizerT>{sketches.data(), sketches.size()},
            target_index_ref,
            query_len, params.max_occ, kmer_size);

        auto anchors_slab = make_arena_slab<Anchor>();
        anchors_slab.reserve(anchors_vec.size());
        if (!anchors_vec.empty()) {
            std::memcpy(anchors_slab.data(), anchors_vec.data(),
                        anchors_vec.size() * sizeof(Anchor));
        }
        anchors_slab.resize(anchors_vec.size());
        sink.on_batch(std::span<Anchor>{anchors_slab.data(), anchors_slab.size()});
    }

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
 * Dispatches to one of the three chain_anchors_* kernels via a compile-time
 * `ChainingAlgorithmType` parameter, then materialises the kernel's two
 * returned vectors into arena-backed slabs so downstream stages get
 * structural lifetime independent of inlining (see DOC0249 §8.4 rationale).
 */
template <::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType CA>
struct ChainStage : StageBase<ChainStage<CA>, Chain> {
    static constexpr std::string_view stage_name = "ChainStage";

    ChainingParams cp;

    template <is_constexpr_aligner_params Cep, class B, class Sink>
    [[gnu::always_inline]] void process(B&& anchors_in, Sink& sink) {
        using ::bioseq::simd::HWY_NAMESPACE::ChainingAlgorithmType;

        // chain_anchors_* now accept std::span<const Anchor> directly
        // (DOC0249 M6); no vector copy at the boundary.
        const std::span<const Anchor> anchors_span{
            anchors_in.data(), anchors_in.size()};

        std::vector<Chain>  chains_vec;
        std::vector<Anchor> reordered_vec;
        if constexpr (CA == ChainingAlgorithmType::SIMD_MM2) {
            auto pair = ::bioseq::simd::HWY_NAMESPACE::chain_anchors_simd(
                anchors_span, cp);
            chains_vec    = std::move(pair.first);
            reordered_vec = std::move(pair.second);
        } else if constexpr (CA == ChainingAlgorithmType::RMQ) {
            auto pair = ::bioseq::chaining::chain_anchors_rmq(anchors_span, cp);
            chains_vec    = std::move(pair.first);
            reordered_vec = std::move(pair.second);
        } else {
            static_assert(CA == ChainingAlgorithmType::SCALAR_MM2,
                "Unhandled ChainingAlgorithmType variant");
            auto pair = ::bioseq::chaining::chain_anchors_scalar(anchors_span, cp);
            chains_vec    = std::move(pair.first);
            reordered_vec = std::move(pair.second);
        }

        auto chains_slab    = make_arena_slab<Chain>();
        auto reordered_slab = make_arena_slab<Anchor>();
        chains_slab.reserve(chains_vec.size());
        reordered_slab.reserve(reordered_vec.size());
        if (!chains_vec.empty()) {
            std::memcpy(chains_slab.data(), chains_vec.data(),
                        chains_vec.size() * sizeof(Chain));
        }
        if (!reordered_vec.empty()) {
            std::memcpy(reordered_slab.data(), reordered_vec.data(),
                        reordered_vec.size() * sizeof(Anchor));
        }
        chains_slab.resize(chains_vec.size());
        reordered_slab.resize(reordered_vec.size());

        sink.on_batch_pair(
            std::span<Chain> {chains_slab.data(),    chains_slab.size()},
            std::span<Anchor>{reordered_slab.data(), reordered_slab.size()});
    }

    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        StageAlgorithmDescription algo;
        algo.name = std::string{
            ::bioseq::simd::HWY_NAMESPACE::chaining_algorithm_name(CA)};
        algo.parameters.push_back({"min_chain_score", std::to_string(cp.min_chain_score)});
        algo.parameters.push_back({"min_seed_count",  std::to_string(cp.min_seed_count)});
        algo.parameters.push_back({"max_chain_skip",  std::to_string(cp.max_chain_skip)});
        algo.parameters.push_back({"max_chain_iter",  std::to_string(cp.max_chain_iter)});
        algo.parameters.push_back({"bandwidth",       std::to_string(cp.bandwidth)});
        algo.parameters.push_back({"max_gap",         std::to_string(cp.max_gap)});
        desc.algorithm = std::move(algo);
        // Chain stage reserves both a chains slab and a reordered-anchors
        // slab; we surface the chain reserve (the smaller of the two; the
        // anchor reserve mirrors SeedHitStage's per-query estimate).
        desc.estimated_batch_size = detail::estimated_chain_count(
            /*anchor_count=*/0u, cp.min_seed_count) +
            static_cast<std::size_t>(cp.min_seed_count);
        return desc;
    }
};

/**
 * @brief Stage 4: chain → region conversion.
 *
 * Wraps the existing `generate_regions` kernel, which already builds a
 * std::vector<AlignRegion> exactly equal to chains.size().  The wrapper
 * passes the regions span downstream paired with the reordered anchors so
 * subsequent stages see a uniform (regions, anchors) pair shape.
 */
struct RegionStage : StageBase<RegionStage, AlignRegion> {
    static constexpr std::string_view stage_name = "RegionStage";

    int32_t query_len = 0;

    template <is_constexpr_aligner_params Cep, class CB, class AB, class Sink>
    [[gnu::always_inline]] void process_pair(CB&& chains, AB&& anchors, Sink& sink) {
        // generate_regions now accepts span inputs directly (DOC0249 M7).
        // The kernel sorts `chains` in place by score, so we pass a mutable
        // span; `anchors` is read-only.
        auto regions_vec = generate_regions(
            std::span<Chain>{chains.data(), chains.size()},
            std::span<const Anchor>{anchors.data(), anchors.size()},
            query_len);

        // `AlignRegion` owns a `std::vector<CigarOp>` so it is not
        // trivially-destructible.  Use the placement-new pattern so the
        // slab's destructor walks `[0, len_)` and calls `~AlignRegion()` on
        // each live element before the arena reclaims the storage; this
        // releases any heap-allocated CIGAR vectors a downstream stage
        // populated but did not consume (e.g. on an early-return path).
        auto regions_slab = make_arena_slab<AlignRegion>();
        regions_slab.reserve_for_placement_new(regions_vec.size());
        for (auto& region_source : regions_vec) {
            regions_slab.construct_at_back(std::move(region_source));
        }

        sink.on_batch_pair(
            std::span<AlignRegion>{regions_slab.data(), regions_slab.size()},
            std::forward<AB>(anchors));
    }

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
    [[gnu::always_inline]] void process_pair(RB&& regions, AB&& anchors, Sink& sink) {
        // Kernel now mutates in place through a span — no vector round-trip
        // (DOC0249 M8).
        classify_primary_secondary(
            std::span<AlignRegion>{regions.data(), regions.size()},
            mask_level, mask_len, sub_diff_value);
        sink.on_batch_pair(std::forward<RB>(regions), std::forward<AB>(anchors));
    }

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
    [[gnu::always_inline]] void process_pair(RB&& regions, AB&& anchors, Sink& sink) {
        // Kernel filters in place via a span; the new effective size is
        // returned so the downstream stage observes a shorter prefix
        // (DOC0249 M8 — `filter_secondary_regions_span`).
        const std::size_t new_size = filter_secondary_regions(
            std::span<AlignRegion>{regions.data(), regions.size()},
            pri_ratio, min_diff_value, best_n);
        sink.on_batch_pair(
            std::span<AlignRegion>{regions.data(), new_size},
            std::forward<AB>(anchors));
    }

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
    [[gnu::always_inline]] void process_pair(RB&& regions, AB&& anchors, Sink& sink) {
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
        sink.on_batch_pair(std::forward<RB>(regions), std::forward<AB>(anchors));
    }

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
 */
template <::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType BAA,
          supports_compact_sequence_range QR,
          supports_compact_sequence_range TR>
struct AlignStage : StageBase<AlignStage<BAA, QR, TR>, AlignRegion> {
    static constexpr std::string_view stage_name = "AlignStage";

    ::bioseq::simd::HWY_NAMESPACE::Minimap2Params params;
    const ScoringMatrix<QR>*                      sm;
    const compact_sequence_range<QR>*             query_seq_;
    const compact_sequence_range<TR>*             target_seq_;
    bool                                          enabled;

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]] void process_pair(RB&& regions, AB&& anchors, Sink& sink) {
        if (enabled) {
            align_regions<BAA>(
                std::span<AlignRegion>{regions.data(), regions.size()},
                std::span<const Anchor>{anchors.data(), anchors.size()},
                *query_seq_, *target_seq_, *sm, params);
        }
        sink.on_batch_pair(std::forward<RB>(regions), std::forward<AB>(anchors));
    }

    [[nodiscard]] StageDescription describe() const {
        StageDescription desc;
        desc.stage_name = std::string{stage_name};
        if (enabled) {
            StageAlgorithmDescription algo;
            algo.name = std::string{
                ::bioseq::simd::HWY_NAMESPACE::base_alignment_algorithm_name(BAA)};
            algo.parameters.push_back({"match_score",  std::to_string(params.match_score)});
            algo.parameters.push_back({"mismatch_pen", std::to_string(params.mismatch_pen)});
            algo.parameters.push_back({"gap_open1",    std::to_string(params.gap_open1)});
            algo.parameters.push_back({"gap_ext1",     std::to_string(params.gap_ext1)});
            using ::bioseq::simd::HWY_NAMESPACE::BaseAlignmentAlgorithmType;
            if constexpr (BAA == BaseAlignmentAlgorithmType::KSW2) {
                algo.parameters.push_back({"gap_open2", std::to_string(params.gap_open2)});
                algo.parameters.push_back({"gap_ext2",  std::to_string(params.gap_ext2)});
                algo.parameters.push_back({"zdrop",     std::to_string(params.zdrop)});
                algo.parameters.push_back({"end_bonus", std::to_string(params.end_bonus)});
            } else {
                algo.parameters.push_back({"block_min_size",
                    std::to_string(static_cast<int32_t>(params.block_min_size))});
                algo.parameters.push_back({"block_max_size",
                    std::to_string(static_cast<int32_t>(params.block_max_size))});
            }
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
    [[gnu::always_inline]] void process(RB&& regions, Sink& sink) {
        if (enabled) {
            compute_mapping_quality(
                std::span<AlignRegion>{regions.data(), regions.size()},
                params, 0);
        }
        sink.on_batch(std::forward<RB>(regions));
    }

    template <is_constexpr_aligner_params Cep, class RB, class AB, class Sink>
    [[gnu::always_inline]] void process_pair(RB&& regions, AB&& /*unused*/, Sink& sink) {
        process<Cep>(std::forward<RB>(regions), sink);
    }

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
    PipelineDescription d;
    d.stages.push_back(self_.describe());
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
        ChainStage<CA>{ {}, params.to_chaining_params() },
        RegionStage{ {}, query_len },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        DivergenceStage<MinimizerT>{
            {}, &query_minimizers, query_len, target_len, params.kmer_size,
            params.compute_divergence },
        AlignStage<BAA, QR, TR>{
            {}, params, &scoring_matrix, &query_seq, &target_seq, params.do_cigar },
        PrimarySecondaryStage{ {}, params.mask_level, params.mask_len, sub_diff(params) },
        SecondaryFilterStage{ {}, params.pri_ratio, min_diff(params), params.best_n },
        MapqStage{ {}, params, params.compute_mapq },
        SinkStage{ &output_collection, query_len, target_len }
    );

    if constexpr (EP == EntryPoint::FromSequence) {
        MinimizerStage<MinimizerT, MA, QR> head{
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
                collect_minimizers_simd<minimap_int_hasher>(
                    target_collector, target_seq,
                    target_seq.begin(), target_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH:
                collect_minimizers_two_stack_nthash_simd<minimap_int_hasher>(
                    target_collector, target_seq,
                    target_seq.begin(), target_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR:
            default:
                collect_minimizers<minimap_int_hasher>(
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
                collect_minimizers_simd<minimap_int_hasher>(
                    query_collector, query_seq,
                    query_seq.begin(), query_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::TWO_STACK_NTHASH:
                collect_minimizers_two_stack_nthash_simd<minimap_int_hasher>(
                    query_collector, query_seq,
                    query_seq.begin(), query_seq.end(),
                    window_len, kmer_len);
                break;
            case ::bioseq::simd::HWY_NAMESPACE::MinimizerAlgorithmType::SCALAR:
            default:
                collect_minimizers<minimap_int_hasher>(
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
