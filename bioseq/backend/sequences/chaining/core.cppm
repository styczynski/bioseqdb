/**
 * @file core.cppm
 * @brief Canonical chaining records, parameters, scoring, and backtracking.
 *
 * The module owns the shared contracts used by scalar, SIMD, RMQ, and Vulkan
 * chaining. Coordinates remain 64-bit through sorting and exact score
 * calculations. Packed accessors exist only for canonical ordering.
 */
module;

#include <libassert/assert.hpp>

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

export module bioseq.chaining.core;

import bioseq.common.logging;
import bioseq.common.context;
import bioseq.common.radix_sort;

export {
namespace bioseq::chaining {

// ============================================================================
// Canonical anchor and chain records
// ============================================================================

/**
 * @brief Flat, single-target seed anchor consumed by chaining backends.
 *
 * Reference and query positions are stored at full 64-bit precision. The
 * stable 24-byte layout is shared with the Vulkan seed-anchor wire record.
 * Every producer must initialize all fields before publishing an anchor.
 */
struct anchor {
    std::uint64_t ref_position;   ///< Absolute reference coordinate.
    std::uint64_t query_position; ///< Absolute query coordinate.
    std::uint32_t is_reverse;     ///< Reverse-strand flag encoded as zero or one.
    std::uint8_t query_span;      ///< Query minimizer span in symbols.
    std::uint8_t flags;           ///< Seed flags propagated by collection.
    std::uint16_t segment_id;     ///< Query segment identifier.

    /**
     * @brief Return the canonical reference ordering key.
     *
     * The strand occupies the most-significant bit and bits 0..62 retain the
     * complete reference position. Exact distance arithmetic reads
     * @ref ref_position directly.
     */
    [[nodiscard, gnu::always_inline]] inline std::uint64_t
    reference_sort_key() const noexcept {
        return (static_cast<std::uint64_t>(is_reverse) << 63)
             | (ref_position & ((static_cast<std::uint64_t>(1) << 63) - 1));
    }

    /// Return the canonical packed segment, flags, span, and low query bits.
    [[nodiscard, gnu::always_inline]] inline std::uint64_t
    query_sort_key() const noexcept {
        return (static_cast<std::uint64_t>(segment_id) << 48)
             | (static_cast<std::uint64_t>(flags) << 40)
             | (static_cast<std::uint64_t>(query_span) << 32)
             | static_cast<std::uint32_t>(query_position);
    }

    /// Return the single-target strand partition used by window pruning.
    [[nodiscard, gnu::always_inline]] inline std::uint64_t
    strand_partition_key() const noexcept {
        return static_cast<std::uint64_t>(is_reverse) << 31;
    }
};

/**
 * @brief Canonical total order for persisted and freshly collected anchors.
 *
 * Full query position is the final tie-breaker because @ref anchor::query_sort_key
 * intentionally carries only the low 32 query-position bits.
 */
struct canonical_anchor_less {
    [[nodiscard, gnu::always_inline]] inline bool operator()(
            const anchor& left, const anchor& right) const noexcept {
        const std::uint64_t left_ref = left.reference_sort_key();
        const std::uint64_t right_ref = right.reference_sort_key();
        if (left_ref != right_ref) return left_ref < right_ref;

        const std::uint64_t left_query = left.query_sort_key();
        const std::uint64_t right_query = right.query_sort_key();
        if (left_query != right_query) return left_query < right_query;

        return left.query_position < right.query_position;
    }
};

/// Exact payload presented to the in-place canonical sort stages.  The byte
/// counters describe algorithmic work, not allocator traffic: the primary
/// field counts each non-trivial span once, while tie bytes count every
/// equal-reference and equal-query-key subspan presented to its radix pass.
struct canonical_anchor_sort_stats {
    std::uint64_t primary_bytes = 0u; ///< Bytes presented to the primary sort.
    std::uint64_t tie_bytes = 0u;     ///< Bytes presented to tie normalization.
    bool saturated = false;           ///< A byte counter reached uint64 maximum.
    bool bounded_u32_primary_key = false; ///< The bounded 32-bit key was used.
};

namespace canonical_anchor_sort_detail {

template <class p_key_type>
[[nodiscard]] inline canonical_anchor_sort_stats sort_primary(
        std::span<anchor> anchors,
        p_key_type key,
        bool bounded_u32_primary_key) noexcept {
    canonical_anchor_sort_stats stats{};
    stats.bounded_u32_primary_key = bounded_u32_primary_key;
    if (anchors.size() < 2u) return stats;

    const auto add_payload_bytes = [&stats](
            std::uint64_t& destination,
            std::size_t count) noexcept {
        constexpr std::uint64_t k_element_bytes = sizeof(anchor);
        if (count > (std::numeric_limits<std::uint64_t>::max)()
                        / k_element_bytes) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            stats.saturated = true;
            return;
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(count) * k_element_bytes;
        if (destination
                > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            stats.saturated = true;
            return;
        }
        destination += bytes;
    };

    add_payload_bytes(stats.primary_bytes, anchors.size());
    bioseq::sorting::radix_sort(
        anchors.data(), anchors.data() + anchors.size(), key);
    return stats;
}

} // namespace canonical_anchor_sort_detail

[[nodiscard]] inline canonical_anchor_sort_stats sort_canonical_anchor_primary(
        std::span<anchor> anchors) noexcept {
    struct anchor_reference_sort_key {
        [[nodiscard, gnu::always_inline]] std::uint64_t operator()(
                const anchor& anchor) const noexcept {
            return anchor.reference_sort_key();
        }
    };
    return canonical_anchor_sort_detail::sort_primary(
        anchors, anchor_reference_sort_key{}, false);
}

/// Fragment-native callers present one orientation-homogeneous slice at a
/// time and have already authenticated every reference position against the
/// target length.  For targets whose exclusive length fits in u32, the
/// orientation bit is constant and `ref_position` is therefore the exact
/// canonical primary key.  Selecting a u32 key prevents the in-place MSD
/// radix from scanning four constant high bytes.  Larger targets retain the
/// full `reference_sort_key()` authority without first scanning the anchor payload for
/// a narrower maximum.
[[nodiscard]] inline canonical_anchor_sort_stats
sort_canonical_anchor_primary_bounded(
        std::span<anchor> orientation_homogeneous_anchors,
        std::uint64_t authenticated_target_length) noexcept {
    if (authenticated_target_length
            <= (std::numeric_limits<std::uint32_t>::max)()) {
        struct anchor_ref_position_u32_key {
            [[nodiscard, gnu::always_inline]] std::uint32_t operator()(
                    const anchor& anchor) const noexcept {
                return static_cast<std::uint32_t>(anchor.ref_position);
            }
        };
        return canonical_anchor_sort_detail::sort_primary(
            orientation_homogeneous_anchors,
            anchor_ref_position_u32_key{},
            true);
    }
    return sort_canonical_anchor_primary(orientation_homogeneous_anchors);
}

[[nodiscard]] inline canonical_anchor_sort_stats
normalize_canonical_anchor_ties(std::span<anchor> anchors) noexcept {
    canonical_anchor_sort_stats stats{};
    if (anchors.size() < 2u) return stats;

    const auto add_payload_bytes = [&stats](
            std::size_t count) noexcept {
        constexpr std::uint64_t k_element_bytes = sizeof(anchor);
        if (count > (std::numeric_limits<std::uint64_t>::max)()
                        / k_element_bytes) {
            stats.tie_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
            stats.saturated = true;
            return;
        }
        const std::uint64_t bytes =
            static_cast<std::uint64_t>(count) * k_element_bytes;
        if (stats.tie_bytes
                > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
            stats.tie_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
            stats.saturated = true;
            return;
        }
        stats.tie_bytes += bytes;
    };

    struct anchor_query_sort_key {
        [[nodiscard, gnu::always_inline]] std::uint64_t operator()(
                const anchor& anchor) const noexcept {
            return anchor.query_sort_key();
        }
    };
    struct anchor_full_query_position_key {
        [[nodiscard, gnu::always_inline]] std::uint64_t operator()(
                const anchor& anchor) const noexcept {
            return anchor.query_position;
        }
    };

    // The project radix sorter is intentionally unstable.  Establish each
    // subsequent key only inside the already-equal primary group: first
    // query_sort_key, then the full query position where mm128's low-32-bit query
    // word still ties.  This is linear for duplicate-heavy runs and produces
    // exactly canonical_anchor_less order without a comparison-sort cliff.
    for (std::size_t ref_begin = 0u; ref_begin < anchors.size();) {
        std::size_t ref_end = ref_begin + 1u;
        const std::uint64_t reference_sort_key = anchors[ref_begin].reference_sort_key();
        while (ref_end < anchors.size()
               && anchors[ref_end].reference_sort_key() == reference_sort_key) {
            ++ref_end;
        }
        if (ref_end - ref_begin > 1u) {
            add_payload_bytes(ref_end - ref_begin);
            bioseq::sorting::radix_sort(
                anchors.data() + ref_begin,
                anchors.data() + ref_end,
                anchor_query_sort_key{});

            for (std::size_t query_begin = ref_begin;
                 query_begin < ref_end;) {
                std::size_t query_end = query_begin + 1u;
                const std::uint64_t query_sort_key =
                    anchors[query_begin].query_sort_key();
                while (query_end < ref_end
                       && anchors[query_end].query_sort_key() == query_sort_key) {
                    ++query_end;
                }
                if (query_end - query_begin > 1u) {
                    add_payload_bytes(query_end - query_begin);
                    bioseq::sorting::radix_sort(
                        anchors.data() + query_begin,
                        anchors.data() + query_end,
                        anchor_full_query_position_key{});
                }
                query_begin = query_end;
            }
        }
        ref_begin = ref_end;
    }
    return stats;
}

[[nodiscard]] inline canonical_anchor_sort_stats sort_canonical_anchors(
        std::span<anchor> anchors) noexcept {
    canonical_anchor_sort_stats stats =
        sort_canonical_anchor_primary(anchors);
    const canonical_anchor_sort_stats tie_stats =
        normalize_canonical_anchor_ties(anchors);
    stats.tie_bytes = tie_stats.tie_bytes;
    stats.saturated = stats.saturated || tie_stats.saturated;
    return stats;
}

static_assert(
    sizeof(anchor) == 24,
    "anchor layout must match the 24-byte GPU seed-anchor record");

/// One chain of anchors, with metadata.
///
/// Equivalent to one entry in the u[] array after chaining.
struct chain {
    int32_t score        = 0;    ///< chain score (upper 32 bits of u[])
    int32_t anchor_count = 0;    ///< anchor count (lower 32 bits of u[])
    int32_t anchor_start = 0;    ///< start index in the reordered anchor array
};

// ============================================================================
// Arena-backed owning slab
// ============================================================================

/// Arena-backed owning batch storage shared by pipeline stages and kernels.
///
/// Allocates via `context::current().alloc_chain_array<element_type>(...)` so the
/// underlying storage is reclaimed at the surrounding `arena::chain` scope
/// unwind. `push_back` grows geometrically. Orphaned slabs land in the
/// arena and are freed in O(1) at scope exit.
///
/// Two patterns are supported:
///
///   1. Trivially-copyable `element_type` (e.g. `anchor`, `chain`, `minimizer_record`):
///      callers use `reserve(N)` + `push_back` or `reserve(N)` + write
///      through `data()` + `resize(actual_n)`.  The arena scope reclaims
///      the slab without per-element work. Element destructors are no-op.
///
///   2. Non-trivially-destructible `element_type` (e.g. `alignment_region` with its
///      `std::vector<cigar_operation>` member): callers MUST use
///      `construct_at_back(args...)` to placement-new each element and the
///      slab's own destructor walks `[0, size_)` calling `~element_type()` so heap-owned
///      fields are released before the arena pointer-bump reclaims the slab.
///
/// Lives in `bioseq::chaining` because the chaining kernels are the primary
/// producers (`backtrack_chains`, `chain_anchors_*`) and the pipeline stages
/// (`bioseq::simd::HWY_NAMESPACE::pipe`) import this module already.
template <class p_element>
struct arena_slab {
    using element_type = p_element;

    element_type* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    context::context* arena_ = nullptr;

    constexpr arena_slab() noexcept = default;
    constexpr arena_slab(element_type* data_ptr, std::size_t length, std::size_t capacity,
                        context::context* arena) noexcept
        : data_(data_ptr), size_(length), capacity_(capacity), arena_(arena) {}

    arena_slab(const arena_slab&)            = delete;
    arena_slab& operator=(const arena_slab&) = delete;

    arena_slab(arena_slab&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_), arena_(other.arena_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.arena_ = nullptr;
    }

    arena_slab& operator=(arena_slab&& other) noexcept {
        if (this != &other) {
            destroy_live_elements();
            data_ = other.data_;
            size_ = other.size_;
            capacity_ = other.capacity_;
            arena_ = other.arena_;
            other.data_ = nullptr;
            other.size_ = 0;
            other.capacity_ = 0;
            other.arena_ = nullptr;
        }
        return *this;
    }

    ~arena_slab() noexcept {
        destroy_live_elements();
    }

    void reserve(std::size_t expected_capacity) {
        static_assert(std::is_trivially_copyable_v<element_type>,
            "arena_slab::reserve uses memcpy to relocate live elements during "
            "growth; only trivially-copyable element types are supported on "
            "this path.  Non-trivial types must size the slab once via "
            "reserve_for_placement_new() and never call reserve a second time.");
        constexpr std::size_t k_maximum_elements =
            (std::numeric_limits<std::size_t>::max)() / sizeof(element_type);
        if (expected_capacity > k_maximum_elements) {
            throw std::length_error{"arena_slab capacity exceeds addressable bytes"};
        }
        if (capacity_ >= expected_capacity) return;
        auto allocated_span = arena_->template alloc_chain_array<element_type, alignof(element_type)>(expected_capacity);
        if (size_ > 0) std::memcpy(allocated_span.data(), data_, size_ * sizeof(element_type));
        data_ = allocated_span.data();
        capacity_ = expected_capacity;
    }

    /// Grow to at least @p required_capacity using an overflow-safe geometric
    /// policy.  This is the append-loop form of reserve: arena allocations
    /// cannot resize in place, so exact `old_size + batch_size` growth would
    /// retain and recopy every superseded prefix until scope unwind.
    void reserve_geometric(std::size_t required_capacity) {
        static_assert(std::is_trivially_copyable_v<element_type>,
            "arena_slab::reserve_geometric relocates live elements with memcpy");
        if (capacity_ >= required_capacity) return;

        constexpr std::size_t k_maximum_capacity =
            (std::numeric_limits<std::size_t>::max)() / sizeof(element_type);
        if (required_capacity > k_maximum_capacity) {
            throw std::length_error{"arena_slab capacity exceeds addressable bytes"};
        }
        std::size_t new_capacity = capacity_ == 0 ? std::size_t{64} : capacity_;
        while (new_capacity < required_capacity) {
            if (new_capacity > k_maximum_capacity / 2) {
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
    /// `element_type`. Callers then `construct_at_back(args...)` per element. The
    /// slab's destructor walks the live prefix and calls `~element_type()` on each.
    void reserve_for_placement_new(std::size_t capacity) {
        DEBUG_ASSERT(capacity_ == 0, "reserve_for_placement_new must be called on a fresh slab");
        if (capacity == 0) return;
        auto allocated_span = arena_->template alloc_chain_array<element_type, alignof(element_type)>(capacity);
        data_ = allocated_span.data();
        capacity_ = capacity;
        size_ = 0;
    }

    /// Construct a new `element_type` in the next free slot, returning a reference.
    ///
    /// Caller must have called `reserve_for_placement_new(N)` beforehand and
    /// must not exceed `N` constructions.  Used for non-trivial types so the
    /// slab's destructor can call `~element_type()` on each live element.
    template <class... p_argument_types>
    [[gnu::always_inline]] element_type& construct_at_back(p_argument_types&&... args) {
        DEBUG_ASSERT(size_ < capacity_, "construct_at_back beyond reserved capacity");
        element_type* slot = new (data_ + size_) element_type(std::forward<p_argument_types>(args)...);
        ++size_;
        return *slot;
    }

    /// `push_back` overload restricted to trivially-copyable types.
    ///
    /// Geometric growth via `memcpy` of the live prefix. Non-trivial types
    /// must use the `reserve_for_placement_new` + `construct_at_back`
    /// pattern instead so destructors are called.
    void push_back(element_type value) {
        static_assert(std::is_trivially_copyable_v<element_type>,
            "arena_slab::push_back uses memcpy + trivial relocation during "
            "growth; non-trivially-copyable element types must use the "
            "reserve_for_placement_new + construct_at_back pattern.");
        if (size_ == capacity_) {
            constexpr std::size_t k_maximum_elements =
                (std::numeric_limits<std::size_t>::max)() / sizeof(element_type);
            if (size_ == k_maximum_elements) {
                throw std::length_error{"arena_slab size exceeds addressable bytes"};
            }
            reserve_geometric(size_ + 1);
        }
        data_[size_++] = std::move(value);
    }

    /** Append exact trivially-copyable records into uninitialized arena slots. */
    [[nodiscard]] bool append_trivial_records(
            const std::span<const element_type> records) {
        static_assert(std::is_trivially_copyable_v<element_type>);
        if (records.size()
            > (std::numeric_limits<std::size_t>::max)() - size_) {
            return false;
        }
        reserve_geometric(size_ + records.size());
        if (!records.empty()) {
            // Copying an implicit-lifetime element_type object representation into the
            // suitably aligned arena storage starts the destination objects'
            // lifetimes without value-initializing unused capacity.
            std::memcpy(
                static_cast<void*>(data_ + size_),
                static_cast<const void*>(records.data()),
                records.size_bytes());
            size_ += records.size();
        }
        return true;
    }

    /** Append storage-sized wire values that are exactly element_type's representation. */
    template <class p_storage_type>
    [[nodiscard]] bool append_trivial_storage(
            const std::span<const p_storage_type> records)
        requires (
            std::is_trivially_copyable_v<element_type>
            && std::is_trivially_copyable_v<p_storage_type>
            && sizeof(element_type) == sizeof(p_storage_type))
    {
        if (records.size()
            > (std::numeric_limits<std::size_t>::max)() - size_) {
            return false;
        }
        reserve_geometric(size_ + records.size());
        if (!records.empty()) {
            std::memcpy(
                static_cast<void*>(data_ + size_),
                static_cast<const void*>(records.data()),
                records.size_bytes());
            size_ += records.size();
        }
        return true;
    }

    void resize(std::size_t new_len) noexcept {
        DEBUG_ASSERT(new_len <= capacity_, "arena_slab::resize beyond reserved capacity");
        size_ = new_len;
    }

    [[nodiscard]] element_type*          data() noexcept       { return data_; }
    [[nodiscard]] const element_type*    data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::span<element_type>       as_span()       noexcept { return {data_, size_}; }
    [[nodiscard]] std::span<const element_type> as_const_span() const noexcept { return {data_, size_}; }

private:
    void destroy_live_elements() noexcept {
        if constexpr (!std::is_trivially_destructible_v<element_type>) {
            if (data_ == nullptr) return;
            for (std::size_t reverse_idx = size_; reverse_idx > 0; --reverse_idx) {
                data_[reverse_idx - 1].~element_type();
            }
        }
        size_ = 0;
    }
};

/// Helper: make a fresh arena_slab bound to the current pipeline arena context.
template <class p_element>
[[nodiscard]] inline arena_slab<p_element> make_arena_slab() noexcept {
    return arena_slab<p_element>{nullptr, 0u, 0u, &context::current()};
}

// ============================================================================
// Chaining parameters
// ============================================================================

/// Algorithm selection for chaining.
enum class chaining_mode : uint8_t {
    scalar_dp,   ///< Scalar O(n * max_iter) dynamic programming
    simd_dp,     ///< SIMD-vectorized DP (same algorithm, faster inner loop)
    rmq,         ///< AVL+rmq tree-based O(n log n)
};

/// Forward-search semantics used by the authentic SIMD chaining backend.
///
/// The exact policy preserves minimap2's visited-generation/max-skip control.
/// The bounded policy is an explicitly selected throughput policy: it scores
/// predecessor batches with Highway and may stop after a bounded run of
/// non-improving batches.  Neither value permits a scalar implementation.
enum class simd_chaining_policy : std::uint8_t {
    exact_visited = 0,
    bounded_batch_approximate = 1,
};

/// Stable diagnostic spelling for a selected SIMD chaining policy.
[[nodiscard]] constexpr std::string_view simd_chaining_policy_name(
    simd_chaining_policy policy) noexcept
{
    switch (policy) {
        case simd_chaining_policy::exact_visited:
            return "exact_visited";
        case simd_chaining_policy::bounded_batch_approximate:
            return "bounded_batch_approximate";
    }
    return "unknown";
}

/// Parameters controlling anchor chaining behavior.
///
/// Chaining-specific parameters populated from request configuration.
struct chaining_parameters {
    int32_t kmer_size         = 15;
    int32_t max_gap           = 5000;
    int32_t bandwidth         = 500;
    int32_t max_chain_skip    = 25;
    int32_t max_chain_iter    = 5000;
    int32_t min_seed_count    = 3;
    int32_t min_chain_score   = 40;
    float   chain_gap_scale   = 0.8f;
    float   chain_skip_scale  = 0.0f;

    bioseq::chaining::chaining_mode chaining_mode =
        bioseq::chaining::chaining_mode::simd_dp;

    /// Named SIMD forward-search policy. Exact is the permanent default and
    /// oracle. Accelerated execution must always be requested explicitly.
    simd_chaining_policy simd_policy = simd_chaining_policy::exact_visited;

    /// Enable Vulkan execution for the vulkan_tiled_dp chaining backend. When
    /// false and the Vulkan backend is selected, the backend raises an error.
    /// Algorithm fallback must be selected before entering the pipeline.
    bool    vulkan_enabled    = true;

    /// True when downstream stages only need a mapped coordinate/strand result,
    /// not the full secondary list or base-alignment metadata. The scalar/SIMD
    /// chaining backends ignore this hint. The Vulkan backend uses it to keep the
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
/// advancement loop in the DP outer loop filters cross-partition pairs before
/// they reach this function. Distances subtract full 64-bit coordinates and
/// narrow only after the caller has enforced the configured bounded window.
///
/// @param current_anchor      anchor i (the one we are scoring)
/// @param predecessor_anchor  anchor j (candidate predecessor)
/// @param max_ref_dist        Maximum reference distance (max_dist_x)
/// @param max_query_dist      Maximum query distance (max_dist_y)
/// @param chain_bandwidth     Bandwidth for diagonal deviation
/// @param gap_penalty         Precomputed: chain_gap_scale * 0.01 * kmer_size
/// @param skip_penalty        Precomputed: chain_skip_scale * 0.01 * kmer_size
/// @return Score, or INT32_MIN if the pair is invalid
[[nodiscard]] inline int32_t compute_chaining_score(
    const anchor& current_anchor, const anchor& predecessor_anchor,
    int32_t max_ref_dist, int32_t max_query_dist, int32_t chain_bandwidth,
    float gap_penalty, float skip_penalty)
{
    // Compute every dependent value before one combined validity branch. This
    // keeps the hot candidate loop branch-light without changing score order.
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

/// Simplified chaining score for the rmq path.
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
/// @param current_anchor      anchor i (the one we are scoring)
/// @param predecessor_anchor  anchor j (candidate predecessor)
/// @param gap_penalty         Precomputed: chain_gap_scale * 0.01 * kmer_size
/// @param skip_penalty        Precomputed: chain_skip_scale * 0.01 * kmer_size
/// @param[out] is_exact_match True if diagonal_deviation==0 and gap_length<=predecessor_span
/// @param[out] diagonal_width The diagonal deviation |ref_distance - query_distance|
/// @return Score value
[[nodiscard]] inline int32_t compute_chaining_score_simple(
    const anchor& current_anchor,
    const anchor& predecessor_anchor,
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
/// Processes anchors from highest to lowest score, tracing predecessor
/// chains and filtering by min_score and min_seed_count.
///
/// Rejected traces remain visited to prevent repeated traversal through
/// already-explored anchors.
///
/// @param anchors                  Input anchor array in array-of-structs form.
/// @param best_scores              f[i] -- best chain score ending at anchor i
/// @param predecessors             p[i] -- predecessor index (-1 if chain start)
/// @param peak_scores              v[i] -- peak score along the chain through i
/// @param min_score                Minimum chain score to keep
/// @param min_seed_count           Minimum number of anchors per chain
/// @param chains_out               Output slab — surviving chains are appended.
/// @param reordered_anchors_out    Output slab — anchors of surviving chains
///                                 appended in chain order. `chain::anchor_start`
///                                 indexes into this slab.
///
/// The two output slabs are written directly in the per-query chain arena.
/// No intermediate vector or output-side copy is allocated.
struct scored_anchor_index {
    int32_t score;
    int64_t anchor_index;
};
static_assert(sizeof(scored_anchor_index) == 16u);

struct score_ascending {
    [[nodiscard, gnu::always_inline]] inline bool operator()(
        const scored_anchor_index& left,
        const scored_anchor_index& right) const noexcept
    {
        return left.score != right.score
            ? left.score < right.score
            : left.anchor_index < right.anchor_index;
    }
};

inline void backtrack_chains(
    std::span<const anchor> anchors,
    std::span<const int32_t> best_scores,
    std::span<const int64_t> predecessors,
    std::span<const int32_t> peak_scores,
    int32_t min_score,
    int32_t min_seed_count,
    arena_slab<chain>& chains_out,
    arena_slab<anchor>& reordered_anchors_out)
{
    (void)peak_scores;  // Retained by the chaining contract; unused here.
    const int64_t anchor_count = static_cast<int64_t>(anchors.size());

    // Allocate the maximum scored-index population once in the chain arena;
    // scored_count tracks the filtered live prefix.
    auto& bt_ctx = context::current();

    auto scored = bt_ctx.alloc_chain_array<scored_anchor_index>(
        static_cast<std::size_t>(anchor_count));
    std::size_t scored_count = 0;
    for (int64_t anchor_idx = 0; anchor_idx < anchor_count; ++anchor_idx) {
        if (best_scores[anchor_idx] >= min_score) {
            scored[scored_count++] = scored_anchor_index{best_scores[anchor_idx], anchor_idx};
        }
    }

    // Sort by score ascending so we process highest-scoring chains last->first
    std::sort(scored.data(), scored.data() + scored_count, score_ascending{});

    // The visited bitmap shares the caller-owned chain arena and remains valid
    // until backtracking returns.
    auto visited = bt_ctx.alloc_chain_array<int32_t>(
        static_cast<std::size_t>(anchor_count));
    std::memset(visited.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

    // Reuse one maximum-length chain-index scratch span for every rank.
    auto chain_idx = bt_ctx.alloc_chain_array<int64_t>(
        static_cast<std::size_t>(anchor_count));

    // Process from highest to lowest score
    for (int64_t rank_idx = static_cast<int64_t>(scored_count) - 1;
         rank_idx >= 0; --rank_idx)
    {
        // Do not skip previously visited candidates here. The trace naturally
        // stops at the first visited predecessor.
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

            chain chain_record;
            chain_record.score = chain_score;
            chain_record.anchor_count = chain_length;
            chain_record.anchor_start =
                static_cast<int32_t>(reordered_anchors_out.size());
            chains_out.push_back(chain_record);

            for (std::size_t i = 0; i < chain_len; ++i) {
                reordered_anchors_out.push_back(anchors[chain_idx[i]]);
            }
        }
        // Keep visited marks for rejected chains so later candidates do not
        // retrace already-explored anchors.
    }

    bioseq_log<logging_level::debug>("[chaining] Backtracking: {} chains from {} anchors",
            chains_out.size(), anchor_count);
}

}  // namespace bioseq::chaining
}  // export
