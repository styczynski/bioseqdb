/**
 * @file core.cppm
 * @brief Shared anchor types, scoring rules, sorting, and chain extraction.
 *
 * Scalar, SIMD, RMQ, and Vulkan chaining use these records and settings.
 * Anchors store 64-bit positions. Sorting packs selected fields into keys,
 * while scoring subtracts the stored coordinates before narrowing distances.
 */
module;

#include <libassert/assert.hpp>

#include <cstdint>
#include <cstring>
#include <cmath>
#include <bit>
#include <new>
#include <optional>
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
import bioseq.common.checked_arithmetic;

export {
namespace bioseq::chaining {

// Anchor and chain records

/**
 * @brief Query and target positions with matching minimizer hashes.
 *
 * The positions use 64-bit coordinates. This 24-byte record also matches
 * the Vulkan seed-anchor layout, so producers must fill every field.
 */
struct anchor {
    std::uint64_t ref_position;   ///< Reference minimizer end position, starting at zero.
    std::uint64_t query_position; ///< Query minimizer end position in the orientation being chained.
    std::uint32_t is_reverse;     ///< Reverse-strand flag encoded as zero or one.
    std::uint8_t query_span;      ///< Query minimizer span in symbols.
    std::uint8_t flags;           ///< Flags copied from the collected seed.
    std::uint16_t segment_id;     ///< Query segment identifier.

    /**
     * @brief Sort by strand and then by the low 63 reference-position bits.
     *
     * Scoring uses @ref ref_position directly. The caller must keep reference
     * positions below 2^63 for this key to preserve their order.
     */
    [[nodiscard, gnu::always_inline]] inline std::uint64_t
    reference_sort_key() const noexcept {
        return (static_cast<std::uint64_t>(is_reverse) << 63)
             | (ref_position & ((static_cast<std::uint64_t>(1) << 63) - 1));
    }

    /// Pack segment, flags, span, and the low 32 query-position bits for sorting.
    [[nodiscard, gnu::always_inline]] inline std::uint64_t
    query_sort_key() const noexcept {
        return (static_cast<std::uint64_t>(segment_id) << 48)
             | (static_cast<std::uint64_t>(flags) << 40)
             | (static_cast<std::uint64_t>(query_span) << 32)
             | static_cast<std::uint32_t>(query_position);
    }

    /// Return the strand key used to keep chaining windows within one orientation.
    [[nodiscard, gnu::always_inline]] inline std::uint64_t
    strand_partition_key() const noexcept {
        return static_cast<std::uint64_t>(is_reverse) << 31;
    }
};

/**
 * @brief Compare anchors in the order expected by the chaining implementations.
 *
 * Compare the reference key, then the packed query key. Use the full query
 * position to break ties left by the packed key's 32-bit position field.
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

/// Count anchor bytes passed to radix sorting. Each primary span and each
/// tied group contributes its size once, even if the sorter makes several
/// passes over it. These counters estimate sorting work.
struct canonical_anchor_sort_stats {
    std::uint64_t primary_bytes = 0u; ///< Bytes presented to the primary sort.
    std::uint64_t tie_bytes = 0u;     ///< Bytes in groups sorted to break ties.
    bool saturated = false;           ///< A byte counter reached uint64 maximum.
    bool bounded_u32_primary_key = false; ///< Reference positions were sorted using a 32-bit key.
};

namespace canonical_anchor_sort_detail {

/**
 * @brief Sort anchors in place by one key and count the input bytes.
 * @tparam p_key_type Callable that extracts a key from an anchor.
 * @param anchors Anchors to reorder.
 * @param key Key passed to the radix sorter.
 * @param bounded_u32_primary_key Record that the caller selected a 32-bit key.
 * @return Counters for this sort.
 */
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

/**
 * @brief Sort anchors by strand and reference position.
 * @param anchors Anchors to reorder.
 * @return Counters for the reference-key sort.
 */
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

/// @brief Sort anchors from one strand by reference position.
///
/// The caller must check that all positions fall within the target and that
/// every anchor has the same strand. When the target length fits in 32 bits,
/// a 32-bit key saves radix passes over the unused high bytes.
/// Larger targets use reference_sort_key().
/// @param orientation_homogeneous_anchors Anchors from a single strand.
/// @param authenticated_target_length Target length already checked by the caller.
/// @return Sort counters, including the chosen key width.
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

/**
 * @brief Sort tied reference groups by the packed query key and full query position.
 * @param anchors Anchors already sorted by reference_sort_key().
 * @return Counters for the groups sorted to break ties.
 */
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

    // Radix sorting can reorder equal keys. Sort each later key only within
    // the groups where earlier keys tie, preserving their established order.
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

/**
 * @brief Sort anchors in place using the order defined by canonical_anchor_less.
 * @param anchors Anchors to reorder.
 * @return Combined counters for the reference sort and tie handling.
 */
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

/// A chain score and its range in the reordered anchor array.
struct chain {
    int32_t score        = 0;    ///< Score of this chain.
    int32_t anchor_count = 0;    ///< Number of anchors in this chain.
    int32_t anchor_start = 0;    ///< First anchor in the reordered anchor array.
};

// Storage in the chaining arena

/// A sequence of objects stored in the chaining arena.
///
/// The arena frees the current buffer and any older buffers when its scope
/// ends. The slab first destroys its live objects. Keep the arena alive until
/// the slab is destroyed.
///
/// For trivially copyable elements, use push_back(), or reserve space, write
/// through data(), and call resize(). For objects that need destructors,
/// reserve_for_placement_new() allocates the full capacity once and
/// construct_at_back() creates each object.
template <class p_element>
struct arena_slab {
    /** @brief Element type stored in arena-owned memory. */
    using element_type = p_element;

    element_type* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
    context::context* arena_ = nullptr;

    /** @brief Creates an unbound empty slab. */
    constexpr arena_slab() noexcept = default;

    /**
     * @brief Use existing arena storage containing length live elements.
     * @param data_ptr First element slot, or null for an empty slab.
     * @param length Number of live elements.
     * @param capacity Number of allocated element slots.
     * @param arena Context that owns the allocation and supplies future storage.
     */
    constexpr arena_slab(element_type* data_ptr, std::size_t length, std::size_t capacity,
                        context::context* arena) noexcept
        : data_(data_ptr), size_(length), capacity_(capacity), arena_(arena) {}

    arena_slab(const arena_slab&)            = delete;
    arena_slab& operator=(const arena_slab&) = delete;

    /** @brief Take the other slab's storage and responsibility for destroying its elements. */
    arena_slab(arena_slab&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_), arena_(other.arena_)
    {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        other.arena_ = nullptr;
    }

    /**
     * @brief Destroy this slab's elements and take the other slab's storage.
     * @param other Slab to leave empty after the move.
     * @return This slab.
     */
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

    /** @brief Destroys live non-trivial elements without releasing arena storage. */
    ~arena_slab() noexcept {
        destroy_live_elements();
    }

    /**
     * @brief Reserve room for trivially copyable elements, copying them if needed.
     * @param expected_capacity Minimum number of element slots.
     * @throws std::length_error If the requested size cannot fit in size_t bytes.
     */
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

    /// Grow capacity geometrically to hold @p required_capacity elements.
    /// Old allocations stay in the arena until its scope ends, so reserving
    /// extra space avoids copying the whole array on every append.
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

    /// Allocate @p capacity uninitialized slots once on a fresh slab.
    /// Use construct_at_back() to create objects in those slots. The slab's
    /// destructor will destroy each object that was constructed.
    void reserve_for_placement_new(std::size_t capacity) {
        DEBUG_ASSERT(capacity_ == 0, "reserve_for_placement_new must be called on a fresh slab");
        if (capacity == 0) return;
        auto allocated_span = arena_->template alloc_chain_array<element_type, alignof(element_type)>(capacity);
        data_ = allocated_span.data();
        capacity_ = capacity;
        size_ = 0;
    }

    /// Construct an object in the next reserved slot and return it.
    /// Reserve the full capacity with reserve_for_placement_new() first and
    /// stay within that capacity. Each successful construction adds a live object.
    template <class... p_argument_types>
    [[gnu::always_inline]] element_type& construct_at_back(p_argument_types&&... args) {
        DEBUG_ASSERT(size_ < capacity_, "construct_at_back beyond reserved capacity");
        element_type* slot = new (data_ + size_) element_type(std::forward<p_argument_types>(args)...);
        ++size_;
        return *slot;
    }

    /// Append a trivially copyable element, growing capacity when needed.
    /// For other types, reserve_for_placement_new() and construct_at_back()
    /// create elements without copying their object representations.
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

    /** Copy trivially copyable records into the slab. Return false if the element count overflows. */
    [[nodiscard]] bool append_trivial_records(
            const std::span<const element_type> records) {
        static_assert(std::is_trivially_copyable_v<element_type>);
        if (records.size()
            > (std::numeric_limits<std::size_t>::max)() - size_) {
            return false;
        }
        reserve_geometric(size_ + records.size());
        if (!records.empty()) {
            // memcpy starts the copied objects' lifetimes in the aligned arena
            // storage. Unused slots remain uninitialized.
            std::memcpy(
                static_cast<void*>(data_ + size_),
                static_cast<const void*>(records.data()),
                records.size_bytes());
            size_ += records.size();
        }
        return true;
    }

    /**
     * Append raw values with the same size and representation as element_type.
     * The caller must ensure that their bits represent valid element_type objects.
     * Return false if the combined element count overflows.
     */
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

    /**
     * @brief Set the number of live elements without constructing or destroying them.
     * @param new_len Number of live elements, within the reserved capacity.
     */
    void resize(std::size_t new_len) noexcept {
        DEBUG_ASSERT(new_len <= capacity_, "arena_slab::resize beyond reserved capacity");
        size_ = new_len;
    }

    /** @brief Returns the first mutable element slot, or null when unallocated. */
    [[nodiscard]] element_type*          data() noexcept       { return data_; }
    /** @brief Returns the first element slot, or null when unallocated. */
    [[nodiscard]] const element_type*    data() const noexcept { return data_; }
    /** @brief Returns the number of live elements. */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** @brief Returns the number of allocated element slots. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    /** @brief Return a writable view of the live elements. */
    [[nodiscard]] std::span<element_type>       as_span()       noexcept { return {data_, size_}; }
    /** @brief Return a read-only view of the live elements. */
    [[nodiscard]] std::span<const element_type> as_const_span() const noexcept { return {data_, size_}; }

private:
    /** @brief Destroy live elements in reverse order when their type needs destruction. */
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

/// @brief Create an empty slab that allocates from the current chaining arena.
/// @tparam p_element Type of the stored elements.
/// @return Empty slab bound to the current context.
template <class p_element>
[[nodiscard]] inline arena_slab<p_element> make_arena_slab() noexcept {
    return arena_slab<p_element>{nullptr, 0u, 0u, &context::current()};
}

// Chaining settings

/// Algorithm selection for chaining.
enum class chaining_mode : uint8_t {
    scalar_dp,   ///< Score candidate predecessors one at a time.
    simd_dp,     ///< Score predecessor batches with SIMD using the selected search policy.
    rmq,         ///< Find promising predecessors with an AVL range-minimum tree.
};

/// Choose how SIMD chaining stops its predecessor search.
///
/// exact_visited follows minimap2's visited marks and skip counter.
/// bounded_batch_approximate stops after enough batches fail to improve any
/// lane's score, trading search coverage for fewer predecessor checks.
enum class simd_chaining_policy : std::uint8_t {
    exact_visited = 0,
    bounded_batch_approximate = 1,
};

/// @brief Return the policy name used in diagnostics, or "unknown".
/// @param policy SIMD predecessor-search policy.
/// @return A static string view.
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

/// Scoring rules, search limits, and algorithm choices for anchor chaining.
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

    /// SIMD predecessor-search policy. C++ callers default to exact_visited.
    /// Select bounded_batch_approximate explicitly to use its earlier stopping rule.
    simd_chaining_policy simd_policy = simd_chaining_policy::exact_visited;

    /// Allow the selected Vulkan chainer to process nonempty input. A disabled
    /// Vulkan chainer raises an error when given anchors.
    bool    vulkan_enabled    = true;

    /// Only coordinates and strand are needed by later stages.
    /// Vulkan uses this hint to recover a limited set of chains. Scalar and
    /// SIMD chaining ignore it. Leave it false for CIGAR, divergence, or MAPQ.
    bool    coordinate_only    = false;
    std::uint64_t query_len    = 0;

    /// Call prepare() after changing kmer_size or either penalty scale.
    float   gap_penalty       = 0.0f;   ///< Penalty scale for differences between reference and query advance.
    float   skip_penalty      = 0.0f;   ///< Penalty scale for the distance advanced between anchors.

    /// Update the gap and skip penalties from kmer_size and their scale settings.
    void prepare() {
        gap_penalty  = chain_gap_scale  * 0.01f * static_cast<float>(kmer_size);
        skip_penalty = chain_skip_scale * 0.01f * static_cast<float>(kmer_size);
    }
};

// Fast log2 approximation used by minimap2 gap scoring

/// Approximate log2 for the logarithmic chaining gap penalty.
/// Split the IEEE 754 exponent from the mantissa, then approximate the
/// mantissa's logarithm with the polynomial used by minimap2's mg_log2().
/// @param input Value of at least 2.
/// @return Approximate base-2 logarithm.
[[nodiscard]] inline float fast_log2(float input) {
    uint32_t float_bits = std::bit_cast<uint32_t>(input);
    float log_result = static_cast<float>(((float_bits >> 23) & 255) - 128);
    float_bits &= ~(255u << 23);
    float_bits += 127u << 23;
    const float mantissa_approx = std::bit_cast<float>(float_bits);
    log_result += (-0.34484843f * mantissa_approx + 2.02466578f) * mantissa_approx - 0.67487759f;
    return log_result;
}

// Scores for linking two anchors

/// Score the additional support gained by linking two anchors.
///
/// Reward the part of the minimizer span supported by both coordinate advances,
/// then penalize gaps and differences between those advances. Reject pairs
/// outside the query-distance or diagonal limits.
///
/// The caller must supply anchors from the same target and strand, with bounded
/// coordinate differences that fit in int32_t. Reference distance is limited
/// by the caller's search window.
/// @param current_anchor Anchor being scored.
/// @param predecessor_anchor Earlier anchor that could extend the chain.
/// @param max_ref_dist Reference-window limit, also checked against query advance here.
/// @param max_query_dist Maximum query advance.
/// @param chain_bandwidth Maximum difference between reference and query advance.
/// @param gap_penalty Prepared penalty for diagonal deviation.
/// @param skip_penalty Prepared penalty for distance between anchors.
/// @return Link score, or INT32_MIN for an invalid pair.
[[nodiscard]] inline int32_t compute_chaining_score(
    const anchor& current_anchor, const anchor& predecessor_anchor,
    int32_t max_ref_dist, int32_t max_query_dist, int32_t chain_bandwidth,
    float gap_penalty, float skip_penalty)
{
    // Combine the distance checks into one branch in the predecessor loop.
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

/// Score an RMQ predecessor and report how far the link moves off the diagonal.
///
/// The caller limits the reference window and query range. This helper rewards
/// shared advance and subtracts gap penalties without checking those limits,
/// segment IDs, or cDNA rules. Distances must fit in int32_t.
///
/// The RMQ rule tests query advance against the predecessor span when deciding
/// to apply penalties. The full DP scorer tests the smaller coordinate advance.
/// @param current_anchor Anchor being scored.
/// @param predecessor_anchor Earlier anchor on the same target and strand.
/// @param gap_penalty Prepared penalty for diagonal deviation.
/// @param skip_penalty Prepared penalty for distance between anchors.
/// @param[out] is_exact_match True for a same-diagonal link within the predecessor span.
/// @param[out] diagonal_width Absolute difference between reference and query advance.
/// @return Link score.
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

// Recover chains from predecessor links

/// An anchor index paired with its chain-ending score.
struct scored_anchor_index {
    int32_t score;
    int64_t anchor_index;
};
static_assert(sizeof(scored_anchor_index) == 16u);

/// Output array capacities. The context handles memory reservation and allocation.
struct chaining_output_storage_plan {
    std::size_t anchor_count;
    std::size_t initial_chain_capacity;
    std::size_t reordered_anchor_capacity;
};

/// Estimate output capacities, returning no plan if their sizes overflow.
[[nodiscard]] constexpr std::optional<chaining_output_storage_plan>
derive_chaining_output_storage_plan(std::size_t count, std::int32_t min_seed_count) noexcept {
    if (count > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)()) ||
        !bioseq::common::checked_mul(count, sizeof(anchor))) {
        return std::nullopt;
    }
    const auto chains = min_seed_count <= 0
        ? std::optional<std::size_t>{count}
        : bioseq::common::checked_add(count / static_cast<std::size_t>(min_seed_count),
                                      std::size_t{1u});
    if (!chains || !bioseq::common::checked_mul(*chains, sizeof(chain))) {
        return std::nullopt;
    }
    return chaining_output_storage_plan{count, *chains, count};
}

/// Three shared backtracking arrays, each with one element per input anchor.
struct scalar_backtracking_storage_plan {
    std::size_t anchor_scratch_count;
    std::size_t logical_scratch_bytes;
};

[[nodiscard]] constexpr std::optional<scalar_backtracking_storage_plan>
derive_scalar_backtracking_storage_plan(std::size_t count) noexcept {
    constexpr std::size_t bytes_per_anchor = sizeof(scored_anchor_index) +
        sizeof(std::int32_t) + sizeof(std::int64_t);
    const auto bytes = bioseq::common::checked_mul(count, bytes_per_anchor);
    if (!bytes || count > static_cast<std::size_t>((std::numeric_limits<std::int64_t>::max)())) {
        return std::nullopt;
    }
    return scalar_backtracking_storage_plan{count, *bytes};
}

/// Scratch size while scalar scoring arrays and backtracking arrays are both live.
struct scalar_chaining_storage_plan {
    chaining_output_storage_plan output;
    scalar_backtracking_storage_plan backtracking;
    std::size_t forward_anchor_count;
    std::size_t logical_scratch_bytes;
};

[[nodiscard]] constexpr std::optional<scalar_chaining_storage_plan>
derive_scalar_chaining_storage_plan(std::size_t count, const chaining_parameters& params) noexcept {
    const auto output = derive_chaining_output_storage_plan(count, params.min_seed_count);
    const auto backtracking = derive_scalar_backtracking_storage_plan(count);
    const auto forward_bytes = bioseq::common::checked_mul(count,
        3u * sizeof(std::int32_t) + sizeof(std::int64_t));
    if (!output || !backtracking || !forward_bytes) {
        return std::nullopt;
    }
    const auto total = bioseq::common::checked_add(*forward_bytes,
        backtracking->logical_scratch_bytes);
    if (!total) {
        return std::nullopt;
    }
    return scalar_chaining_storage_plan{*output, *backtracking, count, *total};
}

/// Sort by increasing score, then by increasing anchor index.
struct score_ascending {
    /// Compare scores, using the anchor index to break ties.
    [[nodiscard, gnu::always_inline]] inline bool operator()(
        const scored_anchor_index& left,
        const scored_anchor_index& right) const noexcept
    {
        return left.score != right.score
            ? left.score < right.score
            : left.anchor_index < right.anchor_index;
    }
};

/// Recover chains by following predecessors from the highest-scoring endpoints.
///
/// Stop a trace at a chain start or an anchor already visited by an earlier
/// trace. Subtract the visited anchor's score so shared support is counted once.
/// A trace must meet both the score and seed-count limits to be emitted.
/// Rejected traces stay marked.
/// @param anchors Anchors in the order used by dynamic programming.
/// @param best_scores Best score ending at each anchor.
/// @param predecessors Earlier anchor index, or a negative value at a chain start.
/// @param peak_scores Accepted for the shared interface, unused here.
/// @param min_score Minimum endpoint score and minimum extracted-chain score.
/// @param min_seed_count Minimum number of anchors in an emitted chain.
/// @param chains_out Receives accepted chains, in endpoint-processing order.
/// @param reordered_anchors_out Receives each chain's anchors in forward order.
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
    (void)peak_scores;  // This shared-interface argument is unused by this backtracker.
    const auto storage_plan = derive_scalar_backtracking_storage_plan(anchors.size());
    if (!storage_plan) {
        throw std::length_error{"shared chain backtracking storage exceeds its addressable domain"};
    }
    const auto scratch_count = storage_plan->anchor_scratch_count;
    const int64_t anchor_count = static_cast<int64_t>(scratch_count);

    // Reserve space for every possible endpoint, then keep only those
    // whose score meets min_score.
    auto& bt_ctx = context::current();

    auto scored = bt_ctx.alloc_chain_array<scored_anchor_index>(
        scratch_count);
    std::size_t scored_count = 0;
    for (int64_t anchor_idx = 0; anchor_idx < anchor_count; ++anchor_idx) {
        if (best_scores[anchor_idx] >= min_score) {
            scored[scored_count++] = scored_anchor_index{best_scores[anchor_idx], anchor_idx};
        }
    }

    // Traverse this ascending order backwards to start with the best scores.
    std::sort(scored.data(), scored.data() + scored_count, score_ascending{});

    // Keep visited marks for the full backtracking call.
    auto visited = bt_ctx.alloc_chain_array<int32_t>(
        scratch_count);
    std::memset(visited.data(), 0,
                static_cast<std::size_t>(anchor_count) * sizeof(int32_t));

    // Reuse one scratch array to collect each predecessor path.
    auto chain_idx = bt_ctx.alloc_chain_array<int64_t>(
        scratch_count);

    // Try the highest-scoring endpoints first.
    for (int64_t rank_idx = static_cast<int64_t>(scored_count) - 1;
         rank_idx >= 0; --rank_idx)
    {
        // A visited endpoint produces an empty trace. Otherwise stop when
        // this path reaches an anchor claimed by an earlier trace.
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
            // Put the collected anchor indices in forward order.
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
        // Keep marks even when this trace is rejected, so later endpoints
        // cannot claim the same anchors again.
    }

    bioseq_log<logging_level::debug>("[chaining] Backtracking: {} chains from {} anchors",
            chains_out.size(), anchor_count);
}

}  // namespace bioseq::chaining
}  // export
