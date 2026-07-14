/**
 * @file chaining_utils.cppm
 * @brief Shared types, scoring functions, and backtracking for all chaining algorithms.
 *
 * Single source of truth for:
 *   - Anchor, Chain, ChainingParams types
 *   - fast_log2 scalar approximation
 *   - compute_chaining_score (full, for scalar/SIMD scalar fallback)
 *   - compute_chaining_score_simple (simplified, for RMQ path)
 *   - backtrack_chains (shared by all three chaining algorithms)
 *
 * All three chaining modules (scalar, SIMD, RMQ) import this module.
 * minimap_port.cppm also imports this for Anchor/Chain types.
 */
module;

#include "common.h"

#include <cstdint>
#include <cstring>
#include <cmath>
#include <bit>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <algorithm>
#include <limits>

export module bioseq.sequences.chaining.utils;

import bioseq.common.mlogging;
import bioseq.common.context;

export {
namespace bioseq::chaining {

// ============================================================================
// Anchor and Chain types — moved from minimap_port.cppm
// ============================================================================

/// Anchor for chaining. Equivalent to mm128_t in collect_seed_hits output.
///
/// **FLAT 24-byte layout** — no nested `ref`/`query` sub-objects. Fields are
/// accessed directly (`anchor.ref_position`, `anchor.query_position`,
/// `anchor.is_reverse`, `anchor.query_span`, …). DOC0263 §10.2 / consolidated-
/// review C1 required the 64-bit position widening (the *correctness* fix); the
/// minimap2 mm128_t's `reference_id` field is removed entirely since this
/// codebase only ever has a single target (every write was `target_hit >> 32`
/// which is structurally 0 for our index packing — see
/// `minimap_port.cppm` `TargetIndex` doc: "Each stored position encodes:
/// (rid << 32) | (pos << 1) | strand. For single-target, rid = 0."). That
/// reclaims 31 bits per anchor, which combined with the layout flattening
/// brings the anchor from the pre-Phase-B 16 B → **24 B** (1.5×, not 2×) so
/// XXL_HIFI cache pressure stays close to the pre-Phase-B baseline.
///
/// The packed mm128-style sort key the chaining DP consumes is recomputed on
/// demand by `ref_bits()` / `query_bits()` accessors; with `reference_id`
/// hard-zero, `ref_bits() == (is_reverse << 63) | (ref_position & ((1<<63)-1))`
/// (DOC0275: the full position, not just the low 32 bits — see `ref_bits()`).
/// Same-chromosome position deltas in the hottest scoring loop read
/// `ref_position` / `query_position` directly (no key recompute), so the
/// precise gap math has always been full-64-bit; `ref_bits()` is the sort key
/// plus the coarse window/distance heuristics, now also full-position so a
/// reference > 2^32 (the 6 Gbp smoke case) neither mis-sorts nor mis-prunes.
///
/// `collect_seed_hits` writes every field it later reads (no default `= 0`),
/// preserving the dead-store elision from proposal-2 §N3.
struct Anchor {
    std::uint64_t ref_position;        ///< full 64-bit reference position (C1)
    std::uint64_t query_position;      ///< full 64-bit query position (C1)
    std::uint32_t is_reverse;          ///< reverse strand flag (0/1)
    std::uint8_t  query_span;          ///< query k-mer span
    std::uint8_t  flags;               ///< flags
    std::uint16_t segment_id;          ///< segment ID

    /// Packed mm128 reference word: is_reverse<<63 | (ref_position & ((1<<63)-1)).
    /// Used as the radix sort key and for the coarse window/distance heuristics
    /// in the chaining DP (the precise gap math reads `ref_position` directly).
    /// `reference_id` was always 0 in this codebase (single-target) and has been
    /// removed, so the old mm128 "rid" slot (bits 32..62) is free to carry
    /// position bits 32..62.
    ///
    /// DOC0275: this keeps the FULL reference position (bits 0..62) instead of
    /// truncating to the low 32 (`static_cast<std::uint32_t>`). The value is
    /// IDENTICAL to the old expression for every position < 2^32 (so every
    /// existing unit/bench result is unchanged — the index packing produced
    /// only sub-2^32 positions before the multi-Gbp smoke tests), but it no
    /// longer wraps the sort key / distance heuristics for references larger
    /// than UINT32_MAX (~4.29 Gbp, the 6 Gbp smoke case): two anchors 5 Gbp
    /// apart would otherwise compare as if ~0.7 Gbp apart. is_reverse stays the
    /// MSB so forward anchors still sort before reverse ones.
    [[nodiscard, gnu::always_inline]] inline std::uint64_t ref_bits() const noexcept {
        return (static_cast<std::uint64_t>(is_reverse) << 63)
             | (ref_position & ((static_cast<std::uint64_t>(1) << 63) - 1));
    }
    /// Packed mm128 query word: segment_id<<48 | flags<<40 | query_span<<32 | (query_position & 0xffffffff).
    [[nodiscard, gnu::always_inline]] inline std::uint64_t query_bits() const noexcept {
        return (static_cast<std::uint64_t>(segment_id) << 48)
             | (static_cast<std::uint64_t>(flags) << 40)
             | (static_cast<std::uint64_t>(query_span) << 32)
             | static_cast<std::uint32_t>(query_position);
    }
    /// Chromosome grouping key (was `ref_bits() >> 32` = is_reverse<<31 since
    /// reference_id ≡ 0). The window-advancement / pruning hot path uses this
    /// instead of `ref_bits() >> 32` to halve the per-iteration work.
    [[nodiscard, gnu::always_inline]] inline std::uint64_t ref_chromosome_key() const noexcept {
        return static_cast<std::uint64_t>(is_reverse) << 31;
    }
};

// Pin the packing. DOC0263 §2 accepted "anchor memory doubles" (16 → ≤32 B); the
// flat layout + reference_id removal lands at 24 B, 1.5× — the tightest the
// C1-widened anchor permits with naturally-aligned 64-bit positions. The
// per-anchor 8 B saved vs round-1's 32 B compounds to ~24 MB L2/L3 pressure
// delta on XXL_HIFI's ~3M anchors / pair.
static_assert(sizeof(Anchor) == 24, "Anchor must be 24 B (2 × uint64 positions + 4 B is_reverse + 4 B (span+flags+segment_id))");

/// One chain of anchors, with metadata.
///
/// Equivalent to one entry in the u[] array after chaining.
struct Chain {
    int32_t score        = 0;    ///< chain score (upper 32 bits of u[])
    int32_t anchor_count = 0;    ///< anchor count (lower 32 bits of u[])
    int32_t anchor_start = 0;    ///< start index in the reordered anchor array
};

// ============================================================================
// Arena-backed owning slab — pipeline batch storage (DOC0249 §7)
// ============================================================================

/// Arena-backed owning batch storage shared by the pipeline stages and the
/// kernels they drive (DOC0249 §7 + §13 follow-up: output-side kernel-to-arena
/// refactor).
///
/// Allocates via `context::current().alloc_chain_array<E>(...)` so the
/// underlying storage is reclaimed at the surrounding `Arena::Chain` scope
/// unwind.  `push_back` grows geometrically; orphaned slabs land in the
/// arena and are freed in O(1) at scope exit.
///
/// Two patterns are supported:
///
///   1. Trivially-copyable `E` (e.g. `Anchor`, `Chain`, `MinimizerRepr`):
///      callers use `reserve(N)` + `push_back` or `reserve(N)` + write
///      through `data()` + `resize(actual_n)`.  The arena scope reclaims
///      the slab without per-element work; element destructors are no-op.
///
///   2. Non-trivially-destructible `E` (e.g. `AlignRegion` with its
///      `std::vector<CigarOp>` member): callers MUST use
///      `construct_at_back(args...)` to placement-new each element and the
///      slab's own destructor walks `[0, len_)` calling `~E()` so heap-owned
///      fields are released before the arena pointer-bump reclaims the slab.
///
/// Lives in `bioseq::chaining` because the chaining kernels are the primary
/// producers (`backtrack_chains`, `chain_anchors_*`) and the pipeline stages
/// (`bioseq::simd::HWY_NAMESPACE::pipe`) import this module already.
template <class E>
struct ArenaSlab {
    using element_type = E;

    E*                ptr_   = nullptr;
    std::size_t       len_   = 0;
    std::size_t       cap_   = 0;
    context::context* arena_ = nullptr;

    constexpr ArenaSlab() noexcept = default;
    constexpr ArenaSlab(E* data_ptr, std::size_t length, std::size_t capacity,
                        context::context* arena) noexcept
        : ptr_(data_ptr), len_(length), cap_(capacity), arena_(arena) {}

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
            "this path.  Non-trivial types must size the slab once via "
            "reserve_for_placement_new() and never call reserve a second time.");
        constexpr std::size_t max_elements =
            (std::numeric_limits<std::size_t>::max)() / sizeof(E);
        if (expected_capacity > max_elements) {
            throw std::length_error{"ArenaSlab capacity exceeds addressable bytes"};
        }
        if (cap_ >= expected_capacity) return;
        auto allocated_span = arena_->template alloc_chain_array<E, alignof(E)>(expected_capacity);
        if (len_ > 0) std::memcpy(allocated_span.data(), ptr_, len_ * sizeof(E));
        ptr_ = allocated_span.data();
        cap_ = expected_capacity;
    }

    /// Grow to at least @p required_capacity using an overflow-safe geometric
    /// policy.  This is the append-loop form of reserve: arena allocations
    /// cannot resize in place, so exact `old_size + batch_size` growth would
    /// retain and recopy every superseded prefix until scope unwind.
    void reserve_geometric(std::size_t required_capacity) {
        static_assert(std::is_trivially_copyable_v<E>,
            "ArenaSlab::reserve_geometric relocates live elements with memcpy");
        if (cap_ >= required_capacity) return;

        constexpr std::size_t max_capacity =
            (std::numeric_limits<std::size_t>::max)() / sizeof(E);
        if (required_capacity > max_capacity) {
            throw std::length_error{"ArenaSlab capacity exceeds addressable bytes"};
        }
        std::size_t new_capacity = cap_ == 0 ? std::size_t{64} : cap_;
        while (new_capacity < required_capacity) {
            if (new_capacity > max_capacity / 2) {
                new_capacity = required_capacity;
                break;
            }
            new_capacity *= 2;
        }
        reserve(new_capacity);
    }

    /// Allocate exactly @p capacity uninitialised slots in the arena.
    ///
    /// Use this once (not in a growth loop) for non-trivially-destructible
    /// `E`.  Callers then `construct_at_back(args...)` per element; the
    /// slab's destructor walks the live prefix and calls `~E()` on each.
    void reserve_for_placement_new(std::size_t capacity) {
        DEBUG_ASSERT(cap_ == 0, "reserve_for_placement_new must be called on a fresh slab");
        if (capacity == 0) return;
        auto allocated_span = arena_->template alloc_chain_array<E, alignof(E)>(capacity);
        ptr_ = allocated_span.data();
        cap_ = capacity;
        len_ = 0;
    }

    /// Construct a new `E` in the next free slot, returning a reference.
    ///
    /// Caller must have called `reserve_for_placement_new(N)` beforehand and
    /// must not exceed `N` constructions.  Used for non-trivial types so the
    /// slab's destructor can call `~E()` on each live element.
    template <class... Args>
    [[gnu::always_inline]] E& construct_at_back(Args&&... args) {
        DEBUG_ASSERT(len_ < cap_, "construct_at_back beyond reserved capacity");
        E* slot = new (ptr_ + len_) E(std::forward<Args>(args)...);
        ++len_;
        return *slot;
    }

    /// `push_back` overload restricted to trivially-copyable types.
    ///
    /// Geometric growth via `memcpy` of the live prefix; non-trivial types
    /// must use the `reserve_for_placement_new` + `construct_at_back`
    /// pattern instead so destructors are called.
    void push_back(E element) {
        static_assert(std::is_trivially_copyable_v<E>,
            "ArenaSlab::push_back uses memcpy + trivial relocation during "
            "growth; non-trivially-copyable element types must use the "
            "reserve_for_placement_new + construct_at_back pattern.");
        if (len_ == cap_) {
            constexpr std::size_t max_elements =
                (std::numeric_limits<std::size_t>::max)() / sizeof(E);
            if (len_ == max_elements) {
                throw std::length_error{"ArenaSlab size exceeds addressable bytes"};
            }
            reserve_geometric(len_ + 1);
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
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
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

/// Helper: make a fresh ArenaSlab bound to the current pipeline arena context.
template <class E>
[[nodiscard]] inline ArenaSlab<E> make_arena_slab() noexcept {
    return ArenaSlab<E>{nullptr, 0u, 0u, &context::current()};
}

// ============================================================================
// Chaining parameters
// ============================================================================

/// Algorithm selection for chaining.
enum class ChainingMode : uint8_t {
    SCALAR_DP,   ///< Original O(n * max_iter) scalar DP
    SIMD_DP,     ///< SIMD-vectorized DP (same algorithm, faster inner loop)
    RMQ,         ///< AVL+RMQ tree-based O(n log n)
};

/// Forward-search semantics used by the authentic SIMD chaining backend.
///
/// The exact policy preserves minimap2's visited-generation/max-skip control.
/// The bounded policy is an explicitly selected throughput policy: it scores
/// predecessor batches with Highway and may stop after a bounded run of
/// non-improving batches.  Neither value permits a scalar implementation.
enum class SimdChainingPolicy : std::uint8_t {
    exact_visited = 0,
    bounded_batch_approximate = 1,
};

/// Stable diagnostic spelling for a selected SIMD chaining policy.
[[nodiscard]] constexpr std::string_view simd_chaining_policy_name(
    SimdChainingPolicy policy) noexcept
{
    switch (policy) {
        case SimdChainingPolicy::exact_visited:
            return "exact_visited";
        case SimdChainingPolicy::bounded_batch_approximate:
            return "bounded_batch_approximate";
    }
    return "unknown";
}

/// Parameters controlling anchor chaining behavior.
///
/// Extracted from Minimap2Params -- only the chaining-relevant subset.
/// The full Minimap2Params in minimap_port.cppm derives these via to_chaining_params().
struct ChainingParams {
    int32_t kmer_size         = 15;
    int32_t max_gap           = 5000;
    int32_t bandwidth         = 500;
    int32_t max_chain_skip    = 25;
    int32_t max_chain_iter    = 5000;
    int32_t min_seed_count    = 3;
    int32_t min_chain_score   = 40;
    float   chain_gap_scale   = 0.8f;
    float   chain_skip_scale  = 0.0f;

    ChainingMode chaining_mode = ChainingMode::SIMD_DP;

    /// Named SIMD forward-search policy. Exact is the permanent default and
    /// oracle; accelerated execution must always be requested explicitly.
    SimdChainingPolicy simd_policy = SimdChainingPolicy::exact_visited;

    /// Enable Vulkan execution for the VULKAN_TILED_DP chaining backend. When
    /// false and the Vulkan backend is selected, the backend raises an error.
    /// Algorithm fallback must be selected before entering the pipeline.
    bool    vulkan_enabled    = true;

    /// True when downstream stages only need a mapped coordinate/strand result,
    /// not the full secondary list or base-alignment metadata. The scalar/SIMD
    /// chaining backends ignore this hint; the Vulkan backend uses it to keep the
    /// selected GPU coordinate path from spending CPU time backtracking every
    /// chain when SQL did not request CIGAR, divergence, or MAPQ.
    bool    coordinate_only    = false;
    std::uint64_t query_len    = 0;

    /// Derived penalty values. Call prepare() after setting scale values.
    float   gap_penalty       = 0.0f;   ///< chain_gap_scale * 0.01 * kmer_size
    float   skip_penalty      = 0.0f;   ///< chain_skip_scale * 0.01 * kmer_size

    /// Compute derived values from scale parameters.
    void prepare() {
        gap_penalty  = chain_gap_scale  * 0.01f * static_cast<float>(kmer_size);
        skip_penalty = chain_skip_scale * 0.01f * static_cast<float>(kmer_size);
    }
};

// ============================================================================
// Fast log2 approximation — matches minimap2's mg_log2 (mmpriv.h:139-147)
// ============================================================================

/// Fast log2 approximation using IEEE 754 float bit tricks.
///
/// Accurate to ~3 significant figures for input >= 2.
/// Matches minimap2 mg_log2() used in gap penalty computation.
///
/// @param input  Float value (must be >= 2 for accuracy)
/// @return Approximate log2(input)
[[nodiscard]] inline float fast_log2(float input) {
    uint32_t float_bits = std::bit_cast<uint32_t>(input);
    float log_result = static_cast<float>(((float_bits >> 23) & 255) - 128);
    float_bits &= ~(255u << 23);
    float_bits += 127u << 23;
    const float mantissa_approx = std::bit_cast<float>(float_bits);
    log_result += (-0.34484843f * mantissa_approx + 2.02466578f) * mantissa_approx - 0.67487759f;
    return log_result;
}

// ============================================================================
// Chaining score functions
// ============================================================================

/// Compute chaining score between two anchors.
///
/// Equivalent to comput_sc() in lchain.c:115-140.
/// Score = min(predecessor_span, gap_length) - gap_penalties.
///
/// ASSUMPTION: Both anchors must be on the same chromosome. The window-start
/// advancement loop in the DP outer loop filters cross-chromosome pairs before
/// they reach this function. The distances are computed as 64-bit position
/// subtractions narrowed to the bounded int32 gap (NOT a truncation of the
/// absolute positions, which are full 64-bit per DOC0263 C1); a cross-chromosome
/// pair would produce a garbage gap, hence the same-chromosome precondition.
///
/// @param current_anchor      Anchor i (the one we are scoring)
/// @param predecessor_anchor  Anchor j (candidate predecessor)
/// @param max_ref_dist        Maximum reference distance (max_dist_x)
/// @param max_query_dist      Maximum query distance (max_dist_y)
/// @param chain_bandwidth     Bandwidth for diagonal deviation
/// @param gap_penalty         Precomputed: chain_gap_scale * 0.01 * kmer_size
/// @param skip_penalty        Precomputed: chain_skip_scale * 0.01 * kmer_size
/// @return Score, or INT32_MIN if the pair is invalid
[[nodiscard]] inline int32_t compute_chaining_score(
    const Anchor& current_anchor, const Anchor& predecessor_anchor,
    int32_t max_ref_dist, int32_t max_query_dist, int32_t chain_bandwidth,
    float gap_penalty, float skip_penalty)
{
    // The previous form had three sequential `if (...) return INT32_MIN;`
    // branches. Each one was a hard branch on the validity check
    // followed by a separate compute step, and the caller's `continue`
    // logic added a fourth `cmp` against INT32_MIN. With ~2500 calls per
    // chain × 400 pairs = ~1M chained branches per section, the branch
    // misprediction cost is real even when the inner arithmetic is
    // cheap. Restructure: compute every dependent value up-front and
    // combine the validity checks into one OR'd predicate. A single
    // branch + a single cmov-friendly select-on-INT32_MIN path replaces
    // the three early-exits. Per proposal-2 §N4.
    const int32_t query_distance = static_cast<int32_t>(
        current_anchor.query_position - predecessor_anchor.query_position);
    const int32_t ref_distance = static_cast<int32_t>(
        current_anchor.ref_position - predecessor_anchor.ref_position);
    const int32_t diagonal_deviation = (ref_distance > query_distance)
        ? ref_distance - query_distance
        : query_distance - ref_distance;

    const bool invalid =
        (query_distance <= 0)
      | (query_distance > max_ref_dist)
      | (ref_distance == 0)
      | (query_distance > max_query_dist)
      | (diagonal_deviation > chain_bandwidth);
    if (invalid) [[unlikely]] {
        return std::numeric_limits<int32_t>::min();
    }

    const int32_t gap_length = (ref_distance < query_distance)
        ? ref_distance : query_distance;
    const int32_t predecessor_span = static_cast<int32_t>(
        predecessor_anchor.query_span);
    int32_t score = (predecessor_span < gap_length) ? predecessor_span : gap_length;

    if (diagonal_deviation != 0 || gap_length > predecessor_span) {
        const float linear_penalty = gap_penalty * static_cast<float>(diagonal_deviation)
                                   + skip_penalty * static_cast<float>(gap_length);
        const float logarithmic_penalty = (diagonal_deviation >= 1)
            ? fast_log2(static_cast<float>(diagonal_deviation + 1))
            : 0.0f;
        score -= static_cast<int32_t>(linear_penalty + 0.5f * logarithmic_penalty);
    }
    return score;
}

/// Simplified chaining score for the RMQ path.
///
/// Equivalent to comput_sc_simple() in lchain.c:351-367.
/// Does not check segment IDs or cDNA mode.
/// Reports whether the score is "exact" (no gap penalty applied)
/// and the diagonal width.
///
/// IMPORTANT: The penalty trigger condition is `diagonal_deviation != 0 || query_distance > predecessor_span`.
/// This uses raw query_distance, NOT gap_length = min(ref_distance, query_distance).
/// This differs from the full compute_chaining_score which uses gap_length > predecessor_span.
///
/// ASSUMPTION: Both anchors must be on the same chromosome. The tree pruning logic
/// guarantees this.
///
/// @param current_anchor      Anchor i (the one we are scoring)
/// @param predecessor_anchor  Anchor j (candidate predecessor)
/// @param gap_penalty         Precomputed: chain_gap_scale * 0.01 * kmer_size
/// @param skip_penalty        Precomputed: chain_skip_scale * 0.01 * kmer_size
/// @param[out] is_exact_match True if diagonal_deviation==0 and gap_length<=predecessor_span
/// @param[out] diagonal_width The diagonal deviation |ref_distance - query_distance|
/// @return Score value
[[nodiscard]] inline int32_t compute_chaining_score_simple(
    const Anchor& current_anchor,
    const Anchor& predecessor_anchor,
    float gap_penalty,
    float skip_penalty,
    bool& is_exact_match,
    int32_t& diagonal_width)
{
    const int32_t query_distance = static_cast<int32_t>(
        current_anchor.query_position - predecessor_anchor.query_position);
    const int32_t ref_distance = static_cast<int32_t>(
        current_anchor.ref_position - predecessor_anchor.ref_position);
    const int32_t diagonal_deviation = (ref_distance > query_distance)
        ? (ref_distance - query_distance) : (query_distance - ref_distance);
    diagonal_width = diagonal_deviation;
    const int32_t gap_length = (ref_distance < query_distance) ? ref_distance : query_distance;
    const int32_t predecessor_span = static_cast<int32_t>(predecessor_anchor.query_span);
    int32_t score = (predecessor_span < gap_length) ? predecessor_span : gap_length;
    is_exact_match = (diagonal_deviation == 0 && gap_length <= predecessor_span);
    if (diagonal_deviation != 0 || query_distance > predecessor_span) {
        const float linear_penalty = gap_penalty * static_cast<float>(diagonal_deviation)
                                   + skip_penalty * static_cast<float>(gap_length);
        const float logarithmic_penalty = (diagonal_deviation >= 1)
            ? fast_log2(static_cast<float>(diagonal_deviation + 1)) : 0.0f;
        score -= static_cast<int32_t>(linear_penalty + 0.5f * logarithmic_penalty);
    }
    return score;
}

// ============================================================================
// Shared backtracking and chain extraction
// ============================================================================

/// Extract chains from DP arrays via backtracking.
///
/// Equivalent to mg_chain_backtrack() + compact_a() in lchain.c.
/// Processes anchors from highest to lowest score, tracing predecessor
/// chains and filtering by min_score and min_seed_count.
///
/// CRITICAL: For rejected chains (score or count too low), visited marks
/// are NOT cleared. The C code (lchain.c:43,55) deliberately leaves
/// t[i]=1 for rejected chains to prevent re-tracing through already-explored
/// anchors. The previous minimap_port.cppm (lines 693-698) incorrectly unmarked
/// rejected chains -- this function fixes that behavior.
///
/// @param anchors                  Original (AoS) anchor array
/// @param best_scores              f[i] -- best chain score ending at anchor i
/// @param predecessors             p[i] -- predecessor index (-1 if chain start)
/// @param peak_scores              v[i] -- peak score along the chain through i
/// @param min_score                Minimum chain score to keep
/// @param min_seed_count           Minimum number of anchors per chain
/// @param chains_out               Output slab — surviving chains are appended.
/// @param reordered_anchors_out    Output slab — anchors of surviving chains
///                                 appended in chain order; `Chain::anchor_start`
///                                 indexes into this slab.
///
/// The two output slabs are written directly inside the per-query
/// `Arena::Chain` scope (DOC0249 §13 output-side refactor); no intermediate
/// `std::vector` is allocated, so the caller's pipeline stage avoids a
/// per-stage memcpy on the chaining-output hot path.
struct ScoredAnchorIndex {
    int32_t score;
    int64_t anchor_index;
};
static_assert(sizeof(ScoredAnchorIndex) == 16u);

struct ScoreAscending {
    [[nodiscard, gnu::always_inline]] inline bool operator()(
        const ScoredAnchorIndex& left,
        const ScoredAnchorIndex& right) const noexcept
    {
        return left.score != right.score
            ? left.score < right.score
            : left.anchor_index < right.anchor_index;
    }
};

inline void backtrack_chains(
    std::span<const Anchor> anchors,
    std::span<const int32_t> best_scores,
    std::span<const int64_t> predecessors,
    std::span<const int32_t> peak_scores,
    int32_t min_score,
    int32_t min_seed_count,
    ArenaSlab<Chain>& chains_out,
    ArenaSlab<Anchor>& reordered_anchors_out)
{
    (void)peak_scores;  // upstream keeps it for parity with lchain.c; unused here
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());

    // Build scored index array, filter by min_score.  DOC0254 §10.1: replaces
    // the two heap-allocated `std::vector`s with arena slabs; the scored slab
    // is sized exactly to `anchor_count` (filter produces at most that many
    // rows) and the per-rank-iteration chain index slab is reused via an
    // explicit length counter instead of `clear()`.
    auto& bt_ctx = context::current();

    auto scored = bt_ctx.alloc_chain_array<ScoredAnchorIndex>(
        static_cast<std::size_t>(anchor_count));
    std::size_t scored_count = 0;
    for (int64_t anchor_idx = 0; anchor_idx < anchor_count; ++anchor_idx) {
        if (best_scores[anchor_idx] >= min_score) {
            scored[scored_count++] = ScoredAnchorIndex{best_scores[anchor_idx], anchor_idx};
        }
    }

    // Sort by score ascending so we process highest-scoring chains last->first
    std::sort(scored.data(), scored.data() + scored_count, ScoreAscending{});

    // `visited` was previously a `std::vector<int32_t>(anchor_count, 0)` —
    // a fresh malloc + zero-fill on every chaining call. Move into the
    // chain arena so the per-pair allocator traffic disappears. The
    // chain arena's scope is owned by the calling chain_anchors_*; the
    // span lives until that scope drops, which is after this function
    // returns to the caller — safe.
    auto visited = bt_ctx.alloc_chain_array<int32_t>(static_cast<std::size_t>(anchor_count));
    std::memset(visited.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

    // Per-rank-iteration chain index scratch — sized to the maximum chain
    // length (= anchor_count) and reused across rank iterations via the
    // `chain_len` counter (DOC0254 §10.1).
    auto chain_idx = bt_ctx.alloc_chain_array<int64_t>(
        static_cast<std::size_t>(anchor_count));

    // Process from highest to lowest score
    for (int64_t rank_idx = static_cast<int64_t>(scored_count) - 1;
         rank_idx >= 0; --rank_idx)
    {
        // NOTE: Do NOT skip candidates where visited[candidate] != 0.
        // The trace naturally stops at visited nodes. (lchain.c:35-56)
        std::size_t chain_len = 0;
        int64_t trace_idx;
        for (trace_idx = scored[rank_idx].anchor_index;
             trace_idx >= 0 && visited[trace_idx] == 0;
             trace_idx = predecessors[trace_idx])
        {
            chain_idx[chain_len++] = trace_idx;
            visited[trace_idx] = 1;
        }

        const int32_t chain_score = (trace_idx < 0)
            ? scored[rank_idx].score
            : scored[rank_idx].score - best_scores[trace_idx];
        const int32_t chain_length = static_cast<int32_t>(chain_len);

        if (chain_score >= min_score && chain_length >= min_seed_count) {
            // Reverse to forward order (in-place two-pointer swap; no
            // intermediate container).
            if (chain_len > 1) {
                for (std::size_t lo = 0, hi = chain_len - 1; lo < hi; ++lo, --hi) {
                    std::swap(chain_idx[lo], chain_idx[hi]);
                }
            }

            Chain chain;
            chain.score = chain_score;
            chain.anchor_count = chain_length;
            chain.anchor_start = static_cast<int32_t>(reordered_anchors_out.size());
            chains_out.push_back(chain);

            for (std::size_t i = 0; i < chain_len; ++i) {
                reordered_anchors_out.push_back(anchors[chain_idx[i]]);
            }
        }
        // CRITICAL: Do NOT unmark visited[] for rejected chains.
        // This prevents re-tracing through already-explored anchors.
        // (lchain.c:43,55 deliberately leaves t[i]=1)
    }

    bioseq_log<DBG>("[chaining] Backtracking: {} chains from {} anchors",
            chains_out.size(), anchor_count);
}

}  // namespace bioseq::chaining
}  // export
