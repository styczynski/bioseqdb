/**
 * @file highway_minimizer_contract.cppm
 * @brief Target-neutral ABI between the scheduler and generated minimizers.
 *
 * No Highway type is reachable from this module.  Generated fixed-target
 * modules consume these records, while neutral scheduler code owns their
 * storage and lifetime.
 */
module;

#include <algorithm>
#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.highway_minimizer_contract;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.size;

export namespace bioseq::minimizers {

/** Select the smallest exact storage layout for one minimizer shape. */
template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length>
struct minimizer_layout {
    static constexpr unsigned k_strand_bits = 1u;
    static constexpr unsigned k_position_bits = p_max_sequence_log2;
    static constexpr unsigned k_minimum_hash_bits =
        (2u * p_kmer_length < 56u) ? static_cast<unsigned>(2u * p_kmer_length) : 56u;
    static constexpr int k_uint16_hash_bits = 16 - 1 - static_cast<int>(k_position_bits);
    static constexpr int k_uint32_hash_bits = 32 - 1 - static_cast<int>(k_position_bits);
    static constexpr int k_uint64_hash_bits = 64 - 1 - static_cast<int>(k_position_bits);
    static constexpr int k_uint128_hash_bits =
        128 - 1 - static_cast<int>(k_position_bits) - 8;
    static constexpr unsigned k_storage_bits =
        (k_uint16_hash_bits >= static_cast<int>(k_minimum_hash_bits)) ? 16u
        : (k_uint32_hash_bits >= static_cast<int>(k_minimum_hash_bits)) ? 32u
        : (k_uint64_hash_bits >= static_cast<int>(k_minimum_hash_bits)) ? 64u
        : 128u;
    static constexpr unsigned k_hash_bits =
        (k_storage_bits == 16u) ? static_cast<unsigned>(k_uint16_hash_bits)
        : (k_storage_bits == 32u) ? static_cast<unsigned>(k_uint32_hash_bits)
        : (k_storage_bits == 64u) ? static_cast<unsigned>(k_uint64_hash_bits)
        : static_cast<unsigned>(k_uint128_hash_bits);
    static constexpr unsigned k_span_bits = k_storage_bits == 128u ? 8u : 0u;
    using storage_type =
        std::conditional_t<k_storage_bits == 16u, std::uint16_t,
        std::conditional_t<k_storage_bits == 32u, std::uint32_t,
        std::conditional_t<k_storage_bits == 64u, std::uint64_t, void>>>;
};

template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length,
         bool p_is_128_bit =
             (minimizer_layout<p_max_sequence_log2, p_kmer_length>::k_storage_bits == 128u)>
struct minimizer_representation;

template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length>
struct minimizer_representation<p_max_sequence_log2, p_kmer_length, false> {
    using layout_type = minimizer_layout<p_max_sequence_log2, p_kmer_length>;
    using storage_type = typename layout_type::storage_type;
    static_assert(!std::is_void_v<storage_type>);

    static constexpr unsigned k_strand_shift = 0u;
    static constexpr unsigned k_position_shift = layout_type::k_strand_bits;
    static constexpr unsigned k_hash_shift =
        layout_type::k_strand_bits + layout_type::k_position_bits;
    static constexpr storage_type k_hash_field_mask =
        (layout_type::k_hash_bits >= sizeof(storage_type) * 8u)
            ? static_cast<storage_type>(~storage_type{0})
            : static_cast<storage_type>(
                (storage_type{1} << layout_type::k_hash_bits) - storage_type{1});
    static constexpr storage_type k_position_field_mask =
        (layout_type::k_position_bits >= sizeof(storage_type) * 8u)
            ? static_cast<storage_type>(~storage_type{0})
            : static_cast<storage_type>(
                (storage_type{1} << layout_type::k_position_bits) - storage_type{1});
    static constexpr storage_type k_strand_field_mask = storage_type{1};
    static constexpr storage_type k_hash_mask =
        static_cast<storage_type>(k_hash_field_mask << k_hash_shift);
    static constexpr storage_type k_position_strand_mask =
        static_cast<storage_type>(~k_hash_mask);

    storage_type value_;

    /** Construct the zero-valued wire record. */
    constexpr minimizer_representation() noexcept : value_(0) {}

    /** Pack hash, last position, and strand into the selected storage word. */
    constexpr minimizer_representation(
        const std::uint64_t kmer_hash,
        const std::uint64_t last_position,
        const std::uint64_t strand_bit) noexcept
        : value_(static_cast<storage_type>(
            ((static_cast<storage_type>(kmer_hash) & k_hash_field_mask)
                << k_hash_shift)
            | ((static_cast<storage_type>(last_position)
                    & k_position_field_mask)
                << k_position_shift)
            | (static_cast<storage_type>(strand_bit) & k_strand_field_mask)))
    {}

    /** Restore a record from its exact packed storage word. */
    [[nodiscard]] static constexpr minimizer_representation from_raw_storage(
        const storage_type value) noexcept
    {
        minimizer_representation result;
        result.value_ = value;
        return result;
    }
    /** Canonical k-mer hash. */
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return static_cast<std::uint64_t>(value_) >> k_hash_shift;
    }
    /** Zero-based position of the k-mer's final symbol. */
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (static_cast<std::uint64_t>(value_) >> k_position_shift)
            & static_cast<std::uint64_t>(k_position_field_mask);
    }
    /** Canonical strand bit. */
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return static_cast<std::uint64_t>(value_) & 1ULL;
    }
    /** Packed position and strand bits. */
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return static_cast<std::uint64_t>(value_)
            & static_cast<std::uint64_t>(k_position_strand_mask);
    }
    /** Exact packed storage word. */
    [[nodiscard]] constexpr storage_type raw_value() const noexcept {
        return value_;
    }
    /** Sentinel that compares above every valid hash. */
    [[nodiscard]] static constexpr minimizer_representation maximum() noexcept {
        return from_raw_storage(k_hash_mask);
    }
    /** Whether this record carries the maximum sentinel hash. */
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (value_ & k_hash_mask) == k_hash_mask;
    }
    /** Replace this record with the maximum sentinel. */
    constexpr void set_max() noexcept {
        value_ = k_hash_mask;
    }
    [[nodiscard]] constexpr auto operator<=> (
        const minimizer_representation& other) const noexcept
    {
        return (value_ & k_hash_mask) <=> (other.value_ & k_hash_mask);
    }
    [[nodiscard]] constexpr bool operator==(
        const minimizer_representation& other) const noexcept
    {
        return (value_ & k_hash_mask) == (other.value_ & k_hash_mask);
    }
    /** Whether equal hashes carry different position/strand identity. */
    [[nodiscard]] static constexpr bool does_sketch_match(
        const minimizer_representation& first,
        const minimizer_representation& second) noexcept
    {
        return ((first.value_ ^ second.value_) & k_hash_mask) == 0u
            && first.value_ != second.value_;
    }
};

template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length>
struct minimizer_representation<p_max_sequence_log2, p_kmer_length, true> {
    using layout_type = minimizer_layout<p_max_sequence_log2, p_kmer_length>;
    static_assert(layout_type::k_position_bits + 1u <= 64u);

    static constexpr unsigned k_high_span_shift = 0u;
    static constexpr unsigned k_high_hash_shift = 8u;
    static constexpr std::uint64_t k_hash_field_mask =
        layout_type::k_hash_bits >= 64u
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << layout_type::k_hash_bits) - std::uint64_t{1};
    static constexpr std::uint64_t k_position_field_mask =
        layout_type::k_position_bits >= 64u
            ? ~std::uint64_t{0}
            : (std::uint64_t{1} << layout_type::k_position_bits) - std::uint64_t{1};
    static constexpr std::uint64_t k_span_field_mask = std::uint64_t{0xFFu};
    static constexpr std::uint64_t k_high_hash_mask =
        k_hash_field_mask << k_high_hash_shift;
    static constexpr std::uint64_t k_low_position_mask = k_position_field_mask << 1u;

    std::uint64_t low_;
    std::uint64_t high_;

    /** Construct the zero-valued two-word record. */
    constexpr minimizer_representation() noexcept : low_(0), high_(0) {}

    /** Pack hash, position, strand, and span into two uint64 words. */
    constexpr minimizer_representation(
        const std::uint64_t kmer_hash,
        const std::uint64_t last_position,
        const std::uint64_t strand_bit,
        const std::uint64_t span = static_cast<std::uint64_t>(p_kmer_length)) noexcept
        : low_(((last_position & k_position_field_mask) << 1u)
                | (strand_bit & 1ULL)),
          high_(((kmer_hash & k_hash_field_mask) << k_high_hash_shift)
                | (span & k_span_field_mask))
    {}

    /** Low packed position/strand word. */
    [[nodiscard]] constexpr std::uint64_t data_low_64() const noexcept {
        return low_;
    }
    /** High packed hash/span word. */
    [[nodiscard]] constexpr std::uint64_t data_high_64() const noexcept {
        return high_;
    }
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return high_ >> k_high_hash_shift;
    }
    [[nodiscard]] constexpr std::uint64_t span() const noexcept {
        return high_ & k_span_field_mask;
    }
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (low_ >> 1u) & k_position_field_mask;
    }
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return low_ & 1ULL;
    }
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return low_;
    }
    [[nodiscard]] static constexpr minimizer_representation maximum() noexcept {
        minimizer_representation result;
        result.high_ = k_high_hash_mask;
        return result;
    }
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (high_ & k_high_hash_mask) == k_high_hash_mask;
    }
    constexpr void set_max() noexcept {
        high_ = k_high_hash_mask;
        low_ = 0u;
    }
    [[nodiscard]] constexpr auto operator<=> (
        const minimizer_representation& other) const noexcept
    {
        return (high_ & k_high_hash_mask) <=> (other.high_ & k_high_hash_mask);
    }
    [[nodiscard]] constexpr bool operator==(
        const minimizer_representation& other) const noexcept
    {
        return (high_ & k_high_hash_mask) == (other.high_ & k_high_hash_mask);
    }
    [[nodiscard]] static constexpr bool does_sketch_match(
        const minimizer_representation& first,
        const minimizer_representation& second) noexcept
    {
        constexpr std::uint64_t k_high_identity_mask = ~k_span_field_mask;
        return ((first.high_ ^ second.high_) & k_high_hash_mask) == 0u
            && ((first.high_ & k_high_identity_mask)
                    != (second.high_ & k_high_identity_mask)
                || first.low_ != second.low_);
    }
};

/** Canonical minimap2-compatible 64-bit sketch record. */
using minimap_sketch_data = minimizer_representation<28u, 15u>;

static_assert(std::same_as<minimap_sketch_data, minimizer_representation<28u, 15u>>);
static_assert(sizeof(minimap_sketch_data) == sizeof(std::uint64_t));
static_assert(alignof(minimap_sketch_data) == alignof(std::uint64_t));
static_assert(std::is_standard_layout_v<minimap_sketch_data>);
static_assert(std::is_trivially_copyable_v<minimap_sketch_data>);

using std::size_t;
using std::uint64_t;

/// Maximum sequence length whose zero-based minimizer positions fit in a
/// representation.  A length of exactly `2^k_position_bits` is valid because its
/// final position is `2^k_position_bits - 1`; the next base would be truncated by
/// `minimizer_representation`'s constructor mask.
template <class p_minimizer_type>
inline constexpr bioseq::storage::sequence_size_t
k_max_minimizer_sequence_length = []() constexpr {
    constexpr auto mask = static_cast<bioseq::storage::sequence_size_t>(
        p_minimizer_type::k_position_field_mask);
    if constexpr (mask
            == (std::numeric_limits<
                bioseq::storage::sequence_size_t>::max)()) {
        return mask;
    } else {
        return static_cast<bioseq::storage::sequence_size_t>(mask + 1u);
    }
}();

template <class p_minimizer_type>
[[nodiscard]] inline constexpr bool is_minimizer_query_length_representable(
        bioseq::storage::sequence_size_t query_length) noexcept {
    return query_length <= k_max_minimizer_sequence_length<p_minimizer_type>;
}

/// Fail before any minimizer constructor can mask an out-of-range query
/// position.  Short queries, including `query_length < k`, deliberately pass;
/// sketch kernels return the valid empty minimizer set for that shape.
template <class p_minimizer_type>
inline void require_representable_minimizer_query_length(
        bioseq::storage::sequence_size_t query_length) {
    if (!is_minimizer_query_length_representable<p_minimizer_type>(query_length)) {
        throw std::length_error(
            "query length exceeds the selected minimizer position field");
    }
}

static_assert(
    k_max_minimizer_sequence_length<minimap_sketch_data>
        == (bioseq::storage::sequence_size_t{1u} << 28u),
    "minimap_sketch_data must accept 2^28 bases and reject 2^28 + 1");
static_assert(is_minimizer_query_length_representable<minimap_sketch_data>(
    bioseq::storage::sequence_size_t{1u} << 28u));
static_assert(!is_minimizer_query_length_representable<minimap_sketch_data>(
    (bioseq::storage::sequence_size_t{1u} << 28u) + 1u));

static_assert(sizeof(minimap_sketch_data) == 8,
              "Canonical minimap_sketch_data must remain a single uint64_t");
static_assert(alignof(minimap_sketch_data) == 8);
static_assert(std::is_standard_layout_v<minimap_sketch_data>);
static_assert(std::is_trivially_copyable_v<minimap_sketch_data>);
static_assert(std::is_trivially_destructible_v<minimap_sketch_data>);

// Storage and wire consumers require the canonical 35:28:1 bit layout.
static_assert(minimizer_layout<28u, 15u>::k_storage_bits == 64u,
    "minimap_sketch_data = minimizer_representation<28, 15> must remain a 64-bit storage type. "
    "Postgres sidecar / dispatch_buffer rely on sizeof(minimap_sketch_data) == 8 "
    "and the bit-layout (hash:35 | pos:28 | strand:1) is hardcoded in the wire "
    "protocol.  Update the sidecar wire format before relaxing this constraint.");

// Pin the canonical layout independently of other representation shapes.
static_assert(minimap_sketch_data::layout_type::k_position_bits == 28u);
static_assert(minimap_sketch_data::layout_type::k_hash_bits == 35u);
static_assert(minimap_sketch_data::layout_type::k_strand_bits == 1u);
static_assert(minimap_sketch_data::k_hash_shift == 29u);
static_assert(minimap_sketch_data::k_hash_mask == 0xFFFFFFFFE0000000ULL);

/**
 * Collector for an exact minimizer representation.
 *
 * The default representation is the canonical 64-bit minimap2 sketch record;
 * wider position/hash layouts select another `minimizer_representation`
 * instantiation without changing collection semantics.
 */
template<class p_minimizer_type = minimap_sketch_data>
struct basic_sketch_collector {
private:
    std::vector<p_minimizer_type> sketches_;
public:
    using minimizer_type = p_minimizer_type;

    /** Construct an empty collector without speculative allocation. */
    basic_sketch_collector() = default;

    /** Reserve storage for a known upper-bound output count. */
    void reserve(size_t expected_count) {
        sketches_.reserve(expected_count);
    }

    /** Append one movable minimizer record. */
    [[always_inline]] constexpr void collect(p_minimizer_type&& sketch) {
        sketches_.push_back(std::move(sketch));
    }

    /** Append one copied minimizer record. */
    [[always_inline]] constexpr void collect(const p_minimizer_type& sketch) {
        sketches_.push_back(sketch);
    }

    /** Append one already-constructed exact-representation batch. */
    [[nodiscard, always_inline]] bool append_records(
        const std::span<const p_minimizer_type> records)
    {
        if (records.size() > sketches_.max_size() - sketches_.size()) {
            return false;
        }
        sketches_.insert(sketches_.end(), records.begin(), records.end());
        return true;
    }

    /// Append a stable SIMD-compressed batch without value-initializing and
    /// then overwriting the destination tail. Each raw word is converted by
    /// the representation owner's `from_raw_storage` constructor and range-
    /// inserted directly into uninitialized vector storage.
    template<class p_record_type = p_minimizer_type>
    [[nodiscard, always_inline]] bool append_raw_storage(
        const typename p_record_type::storage_type* raw,
        std::size_t count)
        requires (
            std::same_as<p_record_type, p_minimizer_type>
            && requires { typename p_record_type::storage_type; }
            && std::is_trivially_copyable_v<p_record_type>
            && sizeof(p_record_type) == sizeof(typename p_record_type::storage_type))
    {
        if (count > sketches_.max_size() - sketches_.size()) {
            return false;
        }
        const auto records = std::span<const typename p_record_type::storage_type>{raw, count}
            | std::views::transform([](const typename p_record_type::storage_type value) {
                return p_record_type::from_raw_storage(value);
            });
        sketches_.insert(sketches_.end(), records.begin(), records.end());
        return true;
    }

    /** Copy the collected minimizer records. */
    constexpr std::vector<p_minimizer_type> extract_sketches() const {
        return sketches_;
    }

    /** Borrow the collected minimizer records. */
    constexpr const std::vector<p_minimizer_type>& extract_sketches_data() const & {
        return sketches_;
    }

    /** Release the collected minimizer records. */
    constexpr std::vector<p_minimizer_type> extract_sketches_data() && {
        return std::move(sketches_);
    }

    /** Return sorted unique (position, hash) pairs. */
    constexpr auto extract_pairs() const {
        auto pairs = sketches_
               | std::ranges::views::transform([](const p_minimizer_type& el) -> std::pair<size_t, uint64_t> {
                     return std::make_pair(static_cast<size_t>(el.last_pos()), el.kmer());
                 })
               | std::ranges::to<std::vector>();
        std::ranges::sort(pairs);
        auto n = std::unique(pairs.begin(), pairs.end()) - pairs.begin();
        pairs.resize(n);
        return pairs;
    }

    /** Drop all collected records while retaining capacity. */
    [[always_inline]] constexpr void clear() {
        sketches_.clear();
    }
};

/** Collector for the canonical 28-position-bit minimap2 representation. */
using sketch_collector = basic_sketch_collector<minimap_sketch_data>;

/**
 * Sketch collector whose vector capacity is owned by a caller-supplied memory
 * admission permit.  `p_permit_type` is intentionally a template parameter: the core
 * k-mer module stays independent of the backend memory-admission module while
 * PostgreSQL and tests can supply `memory_permit`.
 *
 * The permit is declared before the vector, hence the vector is destroyed
 * first.  Each growth reserves the old retained capacity plus headroom for the
 * replacement allocation before calling `std::vector::reserve()`, then
 * reconciles the permit to the allocator-reported retained capacity.  A denied
 * growth is sticky and emits no partial record; callers must check `is_ok()` after
 * kernels whose scalar `collect()` surface cannot return a status.
 */
template<class p_minimizer_type, class p_permit_type>
class admitted_sketch_collector {
public:
    using minimizer_type = p_minimizer_type;
    static constexpr std::size_t k_initial_growth_records = 8u;

    explicit admitted_sketch_collector(
            p_permit_type permit,
            std::uint64_t allocation_headroom_factor)
        : permit_(std::move(permit)),
          allocation_headroom_factor_(allocation_headroom_factor) {
        if (!permit_ || permit_.bytes() != 0u
            || allocation_headroom_factor_ == 0u) {
            failed_ = true;
        }
    }

    admitted_sketch_collector(const admitted_sketch_collector&) = delete;
    admitted_sketch_collector& operator=(
        const admitted_sketch_collector&) = delete;
    admitted_sketch_collector(admitted_sketch_collector&&) = delete;
    admitted_sketch_collector& operator=(
        admitted_sketch_collector&&) = delete;

    [[nodiscard]] bool reserve(std::size_t expected_count) {
        return ensure_capacity(expected_count);
    }

    [[always_inline]] void collect(p_minimizer_type&& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(std::move(sketch));
        }
    }

    [[always_inline]] void collect(const p_minimizer_type& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(sketch);
        }
    }

    /** Append one admitted batch without value-initializing its tail. */
    [[nodiscard, always_inline]] bool append_records(
        const std::span<const p_minimizer_type> records)
    {
        const std::size_t old_size = sketches_.size();
        if (records.size() > sketches_.max_size() - old_size
            || !ensure_capacity(old_size + records.size())) {
            failed_ = true;
            return false;
        }
        sketches_.insert(sketches_.end(), records.begin(), records.end());
        return true;
    }

    template<class p_record_type = p_minimizer_type>
    [[nodiscard, always_inline]] bool append_raw_storage(
        const typename p_record_type::storage_type* raw,
        std::size_t count)
        requires (
            std::same_as<p_record_type, p_minimizer_type>
            && requires { typename p_record_type::storage_type; }
            && std::is_trivially_copyable_v<p_record_type>
            && sizeof(p_record_type) == sizeof(typename p_record_type::storage_type))
    {
        const std::size_t old_size = sketches_.size();
        if (count > sketches_.max_size() - old_size
            || !ensure_capacity(old_size + count)) {
            failed_ = true;
            return false;
        }
        const auto records = std::span<const typename p_record_type::storage_type>{raw, count}
            | std::views::transform([](const typename p_record_type::storage_type value) {
                return p_record_type::from_raw_storage(value);
            });
        sketches_.insert(sketches_.end(), records.begin(), records.end());
        return true;
    }

    [[nodiscard]] const std::vector<p_minimizer_type>&
    extract_sketches_data() const & noexcept {
        return sketches_;
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return !failed_;
    }

    [[nodiscard]] std::uint64_t admitted_capacity_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::pair<std::vector<p_minimizer_type>, p_permit_type>
    release_owned_storage() && {
        return {
            std::move(sketches_),
            std::move(permit_),
        };
    }

private:
    [[nodiscard]] static bool checked_bytes(
            std::size_t count,
            std::uint64_t& result) noexcept {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        if (count > maximum / sizeof(p_minimizer_type)) return false;
        result = static_cast<std::uint64_t>(count) * sizeof(p_minimizer_type);
        return true;
    }

    [[nodiscard]] bool ensure_capacity(std::size_t required) {
        if (failed_) return false;
        if (required <= sketches_.capacity()) return true;
        if (required > sketches_.max_size()) {
            failed_ = true;
            return false;
        }

        const std::size_t old_capacity = sketches_.capacity();
        std::size_t predecessor_capacity = old_capacity;
        std::size_t requested_capacity = k_initial_growth_records;
        if (old_capacity != 0u) {
            requested_capacity = old_capacity <= sketches_.max_size() / 2u
                ? old_capacity * 2u
                : required;
        }
        // A coarse exact-p_record_type sink batch must retain the same observable
        // admission/capacity boundary as the established per-record collector.
        // Walk the *logical* geometric capacities, but perform only the final
        // allocation: this preserves the last old-plus-replacement peak without
        // reintroducing every intermediate copy.
        while (requested_capacity < required) {
            predecessor_capacity = requested_capacity;
            if (requested_capacity > sketches_.max_size() / 2u) {
                requested_capacity = required;
                break;
            }
            requested_capacity *= 2u;
        }
        requested_capacity = std::max(requested_capacity, required);

        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        std::uint64_t old_bytes = 0u;
        std::uint64_t predecessor_bytes = 0u;
        std::uint64_t requested_bytes = 0u;
        if (!checked_bytes(old_capacity, old_bytes)
            || !checked_bytes(predecessor_capacity, predecessor_bytes)
            || !checked_bytes(requested_capacity, requested_bytes)
            || requested_bytes > maximum / allocation_headroom_factor_
            || permit_.bytes() != old_bytes) {
            failed_ = true;
            return false;
        }
        const std::uint64_t allocation_headroom =
            requested_bytes * allocation_headroom_factor_;
        if (allocation_headroom > maximum - predecessor_bytes
            || !permit_.grow_to(
                predecessor_bytes + allocation_headroom)) {
            failed_ = true;
            return false;
        }

        try {
            sketches_.reserve(requested_capacity);
        } catch (...) {
            permit_.shrink_to(old_bytes);
            throw;
        }

        std::uint64_t retained_bytes = 0u;
        if (!checked_bytes(sketches_.capacity(), retained_bytes)
            || retained_bytes > allocation_headroom) {
            // The allocation already exists.  Retain all reserved headroom
            // until destruction and fail closed instead of emitting into an
            // allocation whose exact ownership could not be reconciled.
            failed_ = true;
            return false;
        }
        permit_.shrink_to(retained_bytes);
        return true;
    }

    p_permit_type permit_;
    std::vector<p_minimizer_type> sketches_{};
    std::uint64_t allocation_headroom_factor_ = 0u;
    bool failed_ = false;
};

inline constexpr std::size_t k_max_two_stack_window = 256u;
inline constexpr std::size_t k_max_two_stack_kmer = 32u;
inline constexpr std::size_t k_max_exact_two_stack_kmer = 31u;

/** Target-neutral admission contract for native-u64 two_stack_nthash records. */
template<class p_minimizer_type>
concept exact_simd_minimizer_representation =
    std::is_trivially_copyable_v<p_minimizer_type>
    && sizeof(p_minimizer_type) == sizeof(std::uint64_t)
    && requires (p_minimizer_type value, std::uint64_t raw) {
        typename p_minimizer_type::storage_type;
        requires std::same_as<
            typename p_minimizer_type::storage_type, std::uint64_t>;
        { p_minimizer_type::maximum() } -> std::same_as<p_minimizer_type>;
        { p_minimizer_type::from_raw_storage(raw) } -> std::same_as<p_minimizer_type>;
        { value.kmer() } -> std::convertible_to<std::uint64_t>;
        { value.raw_value() } -> std::convertible_to<std::uint64_t>;
        { p_minimizer_type::k_hash_mask } -> std::convertible_to<std::uint64_t>;
        { p_minimizer_type::k_hash_field_mask } -> std::convertible_to<std::uint64_t>;
        { p_minimizer_type::k_position_field_mask } -> std::convertible_to<std::uint64_t>;
        { p_minimizer_type::k_hash_shift } -> std::convertible_to<unsigned>;
        { p_minimizer_type::k_position_shift } -> std::convertible_to<unsigned>;
    };

/**
 * Finite exact-representation identities reachable by generated minimizers.
 * This is invocation metadata for one coarse typed switch, never a generated
 * source-record axis or a per-record packing authority.
 */
enum class minimizer_output_layout : std::uint8_t {
    unsupported = 0u,
    sequence_log2_15_kmer_15_uint64 = 1u,
    sequence_log2_28_kmer_15_uint64 = 2u,
    sequence_log2_33_kmer_15_uint64 = 3u,
    sequence_log2_49_kmer_7_uint64 = 4u,
    sequence_log2_63_kmer_15_uint128 = 5u,
    sequence_log2_7_kmer_7_uint32 = 6u,
    sequence_log2_17_kmer_7_uint32 = 7u,
    sequence_log2_63_kmer_7_uint128 = 8u,
    sequence_log2_27_kmer_27_uint128 = 9u,
    sequence_log2_63_kmer_27_uint128 = 10u,
};

template<class p_minimizer_type>
inline constexpr minimizer_output_layout k_minimizer_output_layout = [] {
    using output_type = std::remove_cv_t<p_minimizer_type>;
    if constexpr (std::same_as<output_type, minimizer_representation<15u, 15u>>) {
        return minimizer_output_layout::sequence_log2_15_kmer_15_uint64;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<28u, 15u>>) {
        return minimizer_output_layout::sequence_log2_28_kmer_15_uint64;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<33u, 15u>>) {
        return minimizer_output_layout::sequence_log2_33_kmer_15_uint64;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<63u, 15u>>) {
        return minimizer_output_layout::sequence_log2_63_kmer_15_uint128;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<7u, 7u>>) {
        return minimizer_output_layout::sequence_log2_7_kmer_7_uint32;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<17u, 7u>>) {
        return minimizer_output_layout::sequence_log2_17_kmer_7_uint32;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<49u, 7u>>) {
        return minimizer_output_layout::sequence_log2_49_kmer_7_uint64;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<63u, 7u>>) {
        return minimizer_output_layout::sequence_log2_63_kmer_7_uint128;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<27u, 27u>>) {
        return minimizer_output_layout::sequence_log2_27_kmer_27_uint128;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<63u, 27u>>) {
        return minimizer_output_layout::sequence_log2_63_kmer_27_uint128;
    } else {
        return minimizer_output_layout::unsupported;
    }
}();

static_assert(sizeof(minimizer_representation<15u, 15u>) == 8u);
static_assert(sizeof(minimizer_representation<28u, 15u>) == 8u);
static_assert(sizeof(minimizer_representation<33u, 15u>) == 8u);
static_assert(sizeof(minimizer_representation<63u, 15u>) == 16u);
static_assert(sizeof(minimizer_representation<7u, 7u>) == 4u);
static_assert(sizeof(minimizer_representation<17u, 7u>) == 4u);
static_assert(sizeof(minimizer_representation<49u, 7u>) == 8u);
static_assert(sizeof(minimizer_representation<63u, 7u>) == 16u);
static_assert(sizeof(minimizer_representation<27u, 27u>) == 16u);
static_assert(sizeof(minimizer_representation<63u, 27u>) == 16u);
static_assert(k_minimizer_output_layout<minimizer_representation<15u, 15u>>
    == minimizer_output_layout::sequence_log2_15_kmer_15_uint64);
static_assert(k_minimizer_output_layout<minimizer_representation<28u, 15u>>
    == minimizer_output_layout::sequence_log2_28_kmer_15_uint64);
static_assert(k_minimizer_output_layout<minimizer_representation<33u, 15u>>
    == minimizer_output_layout::sequence_log2_33_kmer_15_uint64);
static_assert(k_minimizer_output_layout<minimizer_representation<63u, 15u>>
    == minimizer_output_layout::sequence_log2_63_kmer_15_uint128);
static_assert(k_minimizer_output_layout<minimizer_representation<7u, 7u>>
    == minimizer_output_layout::sequence_log2_7_kmer_7_uint32);
static_assert(k_minimizer_output_layout<minimizer_representation<17u, 7u>>
    == minimizer_output_layout::sequence_log2_17_kmer_7_uint32);
static_assert(k_minimizer_output_layout<minimizer_representation<49u, 7u>>
    == minimizer_output_layout::sequence_log2_49_kmer_7_uint64);
static_assert(k_minimizer_output_layout<minimizer_representation<63u, 7u>>
    == minimizer_output_layout::sequence_log2_63_kmer_7_uint128);
static_assert(k_minimizer_output_layout<minimizer_representation<27u, 27u>>
    == minimizer_output_layout::sequence_log2_27_kmer_27_uint128);
static_assert(k_minimizer_output_layout<minimizer_representation<63u, 27u>>
    == minimizer_output_layout::sequence_log2_63_kmer_27_uint128);

[[nodiscard]] constexpr std::uint64_t minimizer_output_position_mask(
    minimizer_output_layout layout) noexcept
{
    switch (layout) {
        case minimizer_output_layout::sequence_log2_15_kmer_15_uint64:
            return minimizer_representation<15u, 15u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_28_kmer_15_uint64:
            return minimizer_representation<28u, 15u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_33_kmer_15_uint64:
            return minimizer_representation<33u, 15u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_63_kmer_15_uint128:
            return minimizer_representation<63u, 15u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_7_kmer_7_uint32:
            return minimizer_representation<7u, 7u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_17_kmer_7_uint32:
            return minimizer_representation<17u, 7u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_49_kmer_7_uint64:
            return minimizer_representation<49u, 7u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_63_kmer_7_uint128:
            return minimizer_representation<63u, 7u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_27_kmer_27_uint128:
            return minimizer_representation<27u, 27u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_63_kmer_27_uint128:
            return minimizer_representation<63u, 27u>::k_position_field_mask;
        case minimizer_output_layout::unsupported:
            break;
    }
    return 0u;
}

/** Runtime backend selected for one minimizer sketch operation. */
enum class minimizer_sketch_backend : std::uint8_t {
    scalar = 0,
    vulkan_segment = 1,
    exact_simd = 2,
    bounded_nthash_simd = 3,
};

/** Stable diagnostic name for a minimizer backend. */
[[nodiscard]] constexpr const char* minimizer_sketch_backend_name(
    const minimizer_sketch_backend backend) noexcept
{
    switch (backend) {
        case minimizer_sketch_backend::scalar: return "scalar";
        case minimizer_sketch_backend::vulkan_segment: return "vulkan_segment";
        case minimizer_sketch_backend::exact_simd:
            return "exact_simd";
        case minimizer_sketch_backend::bounded_nthash_simd:
            return "bounded_nthash_simd";
    }
    return "unknown";
}

/** Whether a backend executes the Vulkan segment route. */
[[nodiscard]] constexpr bool is_vulkan_minimizer_backend(
    const minimizer_sketch_backend backend) noexcept
{
    return backend == minimizer_sketch_backend::vulkan_segment;
}

/** Canonical sorted (position, hash) pair. */
using minimizer_sorted_pair =
    std::pair<std::uint64_t, std::uint64_t>;

/** Status of an incremental two-stack minimizer execution. */
enum class two_stack_continuation_status : std::uint8_t {
    ok = 0,
    invalid_window_size,
    invalid_kmer_size,
    invalid_static_configuration,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    not_initialized,
    already_finalized,
    invalid_state,
    invalid_range,
    non_contiguous_range,
    arithmetic_overflow,
    collector_overflow,
};

/** Operation-originated two_stack_nthash kernel identity. */
enum class exact_simd_kernel : std::uint8_t {
    generic_u64 = 0,
    kmer_15_window_10_byte_8_uint32_hash,
};

/** Stable diagnostic name for a two-stack continuation status. */
[[nodiscard]] constexpr std::string_view two_stack_continuation_status_name(
    two_stack_continuation_status status) noexcept
{
    switch (status) {
        case two_stack_continuation_status::ok: return "ok";
        case two_stack_continuation_status::invalid_window_size:
            return "invalid_window_size";
        case two_stack_continuation_status::invalid_kmer_size:
            return "invalid_kmer_size";
        case two_stack_continuation_status::invalid_static_configuration:
            return "invalid_static_configuration";
        case two_stack_continuation_status::unsupported_alphabet:
            return "unsupported_alphabet";
        case two_stack_continuation_status::unsupported_storage:
            return "unsupported_storage";
        case two_stack_continuation_status::unsupported_representation:
            return "unsupported_representation";
        case two_stack_continuation_status::unsupported_vector_shape:
            return "unsupported_vector_shape";
        case two_stack_continuation_status::not_initialized:
            return "not_initialized";
        case two_stack_continuation_status::already_finalized:
            return "already_finalized";
        case two_stack_continuation_status::invalid_state:
            return "invalid_state";
        case two_stack_continuation_status::invalid_range:
            return "invalid_range";
        case two_stack_continuation_status::non_contiguous_range:
            return "non_contiguous_range";
        case two_stack_continuation_status::arithmetic_overflow:
            return "arithmetic_overflow";
        case two_stack_continuation_status::collector_overflow:
            return "collector_overflow";
    }
    return "unknown";
}

/** Fail-closed error returned by strict exact-SIMD entry points. */
class exact_simd_runtime_error final : public std::runtime_error {
public:
    exact_simd_runtime_error(
        two_stack_continuation_status status,
        std::string_view operation)
        : std::runtime_error(
              std::string{operation}
              + ": two_stack_nthash minimizer execution rejected: "
              + std::string{two_stack_continuation_status_name(status)}),
          status_(status) {}

    [[nodiscard]] two_stack_continuation_status status() const noexcept {
        return status_;
    }

private:
    two_stack_continuation_status status_;
};

/** Operation-derived counters for the two-stack minimizer algorithms. */
struct two_stack_execution_stats {
    std::uint64_t bp_consumed = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t vector_batches = 0;
    std::uint64_t vector_lanes_visited = 0;
    std::uint64_t masked_tail_lanes = 0;
    std::uint64_t bridge_symbols_staged = 0;
    std::uint64_t bridge_vector_runs = 0;
    std::uint64_t consecutive_kmer_batches = 0;
    std::uint64_t canonical_scramble_batches = 0;
    std::uint64_t window_reduction_batches = 0;
    std::uint64_t event_generation_batches = 0;
    std::uint64_t stable_compress_batches = 0;
    std::uint64_t direct_event_compress_batches = 0;
    std::uint64_t tie_matrix_event_batches = 0;
    std::uint64_t row_transpose_batches = 0;
    std::uint64_t row_vector_stores = 0;
    std::uint64_t palindrome_lanes = 0;
    std::uint64_t first_window_tie_events = 0;
    std::uint64_t expiry_tie_events = 0;
    std::uint64_t tie_hash_prefilter_batches = 0;
    std::uint64_t tie_hash_prefilter_rejected_lanes = 0;
    std::uint64_t tie_raw_verification_batches = 0;
    std::uint64_t entering_min_transitions = 0;
    std::uint64_t expiry_rescan_transitions = 0;
    std::uint64_t final_flush_records = 0;
    std::uint64_t fragment_symbols_staged = 0;
    std::uint64_t packed_decode_batches = 0;
    std::uint64_t packed_decode_lanes = 0;
    std::uint64_t packed_word_boundary_merges = 0;
    std::uint64_t ambiguous_symbols = 0;
    std::uint64_t ambiguous_reset_batches = 0;
    std::uint64_t scalar_orchestration_steps = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_backend_branches = 0;
    std::uint64_t scalar_tail_symbols = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_event_records = 0;
    std::uint64_t scalar_sort_records = 0;
    std::uint64_t scalar_dedup_records = 0;
    // Scalable-vector storage fallbacks are distinct from fixed-width routes.
    std::uint64_t scalable_storage_fallback_calls = 0;
    std::uint64_t scalable_storage_fallback_input_symbols = 0;
    std::uint64_t scalable_storage_fallback_minimizers_emitted = 0;
    // Full, contiguous dna4/u32 inputs may use the resident vector stream
    // collector. These counters are written by the wrapper that
    // actually invokes that collector; they are deliberately separate from
    // the chronological continuation counters below.  In particular, a
    // resident call is never relabelled as a typed continuation call.
    std::uint64_t resident_vector_stream_kernel_calls = 0;
    std::uint64_t resident_vector_stream_single_calls = 0;
    std::uint64_t resident_vector_stream_dual_calls = 0;
    std::uint64_t resident_vector_stream_nonempty_work_calls = 0;
    std::uint64_t resident_vector_stream_input_bp = 0;
    std::uint64_t resident_vector_stream_minimizers_emitted = 0;
    std::uint64_t generic_u64_kernel_calls = 0;
    std::uint64_t generic_u64_candidate_blocks = 0;
    std::uint64_t generic_u64_lane_visits = 0;
    std::uint64_t kmer_15_window_10_byte_8_uint32_hash_kernel_calls = 0;
    std::uint64_t kmer_15_window_10_byte_8_uint32_hash_candidate_blocks = 0;
    std::uint64_t kmer_15_window_10_byte_8_uint32_hash_lane_visits = 0;
    std::uint64_t kmer_15_window_10_byte_8_packed_22_groups = 0;
    std::uint64_t kmer_15_window_10_byte_8_packed_22_lane_visits = 0;
    bool saturated = false;

    /** Saturating-add one counter into an aggregate. */
    static void add_counter(
            std::uint64_t& destination,
            std::uint64_t increment,
            bool& did_saturate) noexcept {
        if (increment > (std::numeric_limits<std::uint64_t>::max)()
                - destination) {
            destination = (std::numeric_limits<std::uint64_t>::max)();
            did_saturate = true;
            return;
        }
        destination += increment;
    }

    /** Merge another execution using saturating counters. */
    void merge(const two_stack_execution_stats& other) noexcept {
        saturated = saturated || other.saturated;
#define BIOSEQ_MERGE_TWO_STACK_COUNTER(name) \
        add_counter(name, other.name, saturated)
        BIOSEQ_MERGE_TWO_STACK_COUNTER(bp_consumed);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(minimizers_emitted);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(vector_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(vector_lanes_visited);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(masked_tail_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(bridge_symbols_staged);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(bridge_vector_runs);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(consecutive_kmer_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(canonical_scramble_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(window_reduction_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(event_generation_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(stable_compress_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(direct_event_compress_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(tie_matrix_event_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(row_transpose_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(row_vector_stores);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(palindrome_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(first_window_tie_events);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(expiry_tie_events);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(tie_hash_prefilter_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            tie_hash_prefilter_rejected_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(tie_raw_verification_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(entering_min_transitions);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(expiry_rescan_transitions);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(final_flush_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(fragment_symbols_staged);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(packed_decode_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(packed_decode_lanes);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(packed_word_boundary_merges);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(ambiguous_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(ambiguous_reset_batches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_orchestration_steps);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(fail_closed_rejections);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_backend_branches);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_tail_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_hash_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_window_transitions);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_event_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_sort_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(scalar_dedup_records);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            scalable_storage_fallback_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            scalable_storage_fallback_input_symbols);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            scalable_storage_fallback_minimizers_emitted);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_kernel_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_single_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_dual_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_nonempty_work_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_input_bp);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            resident_vector_stream_minimizers_emitted);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(generic_u64_kernel_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(generic_u64_candidate_blocks);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(generic_u64_lane_visits);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_uint32_hash_kernel_calls);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_uint32_hash_candidate_blocks);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_uint32_hash_lane_visits);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(kmer_15_window_10_byte_8_packed_22_groups);
        BIOSEQ_MERGE_TWO_STACK_COUNTER(
            kmer_15_window_10_byte_8_packed_22_lane_visits);
#undef BIOSEQ_MERGE_TWO_STACK_COUNTER
    }

    [[nodiscard]] constexpr bool is_internally_consistent(
            bool require_nonempty_vector_work = false) const noexcept {
        const std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        // A fixed-width Highway identity cannot authenticate a scalable or
        // scalar storage fallback; reject every non-zero fallback payload.
        if (scalable_storage_fallback_calls != 0
            || scalable_storage_fallback_input_symbols != 0
            || scalable_storage_fallback_minimizers_emitted != 0) {
            return false;
        }
        const bool resident_payload_present =
            resident_vector_stream_kernel_calls != 0
            || resident_vector_stream_single_calls != 0
            || resident_vector_stream_dual_calls != 0
            || resident_vector_stream_nonempty_work_calls != 0
            || resident_vector_stream_input_bp != 0
            || resident_vector_stream_minimizers_emitted != 0;
        if (resident_payload_present) {
            const bool stream_call_sum_valid =
                resident_vector_stream_single_calls
                    <= maximum - resident_vector_stream_dual_calls
                && resident_vector_stream_single_calls
                    + resident_vector_stream_dual_calls
                    == resident_vector_stream_kernel_calls;
            const bool continuation_payload_absent =
                vector_batches == 0
                && vector_lanes_visited == 0
                && masked_tail_lanes == 0
                && bridge_symbols_staged == 0
                && bridge_vector_runs == 0
                && consecutive_kmer_batches == 0
                && canonical_scramble_batches == 0
                && window_reduction_batches == 0
                && event_generation_batches == 0
                && stable_compress_batches == 0
                && direct_event_compress_batches == 0
                && tie_matrix_event_batches == 0
                && row_transpose_batches == 0
                && row_vector_stores == 0
                && palindrome_lanes == 0
                && first_window_tie_events == 0
                && expiry_tie_events == 0
                && tie_hash_prefilter_batches == 0
                && tie_hash_prefilter_rejected_lanes == 0
                && tie_raw_verification_batches == 0
                && entering_min_transitions == 0
                && expiry_rescan_transitions == 0
                && final_flush_records == 0
                && fragment_symbols_staged == 0
                && packed_decode_batches == 0
                && packed_decode_lanes == 0
                && packed_word_boundary_merges == 0
                && ambiguous_symbols == 0
                && ambiguous_reset_batches == 0
                && scalar_orchestration_steps == 0
                && generic_u64_kernel_calls == 0
                && generic_u64_candidate_blocks == 0
                && generic_u64_lane_visits == 0
                && kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0
                && kmer_15_window_10_byte_8_uint32_hash_candidate_blocks == 0
                && kmer_15_window_10_byte_8_uint32_hash_lane_visits == 0
                && kmer_15_window_10_byte_8_packed_22_groups == 0
                && kmer_15_window_10_byte_8_packed_22_lane_visits == 0;
            const bool resident_backend = !saturated
                && resident_vector_stream_kernel_calls > 0
                && stream_call_sum_valid
                && resident_vector_stream_nonempty_work_calls
                    <= resident_vector_stream_kernel_calls
                && (resident_vector_stream_nonempty_work_calls == 0
                    || resident_vector_stream_minimizers_emitted > 0)
                && resident_vector_stream_input_bp == bp_consumed
                && resident_vector_stream_minimizers_emitted
                    == minimizers_emitted
                && fail_closed_rejections == 0
                && scalar_backend_branches == 0
                && scalar_tail_symbols == 0
                && scalar_hash_symbols == 0
                && scalar_window_transitions == 0
                && scalar_event_records == 0
                && scalar_sort_records == 0
                && scalar_dedup_records == 0
                && continuation_payload_absent;
            return resident_backend
                && (!require_nonempty_vector_work
                    || (bp_consumed > 0
                        && resident_vector_stream_nonempty_work_calls > 0));
        }
        const bool generic_selected = generic_u64_kernel_calls > 0
            && kmer_15_window_10_byte_8_uint32_hash_kernel_calls == 0;
        const bool fast_selected = generic_u64_kernel_calls == 0
            && kmer_15_window_10_byte_8_uint32_hash_kernel_calls > 0;
        const bool typed_route_complete =
            (generic_selected
                && generic_u64_candidate_blocks == vector_batches
                && generic_u64_lane_visits == vector_lanes_visited
                && kmer_15_window_10_byte_8_uint32_hash_candidate_blocks == 0
                && kmer_15_window_10_byte_8_uint32_hash_lane_visits == 0
                && kmer_15_window_10_byte_8_packed_22_groups == 0
                && kmer_15_window_10_byte_8_packed_22_lane_visits == 0)
            || (fast_selected
                && kmer_15_window_10_byte_8_uint32_hash_candidate_blocks == vector_batches
                && kmer_15_window_10_byte_8_uint32_hash_lane_visits == vector_lanes_visited
                && kmer_15_window_10_byte_8_packed_22_groups
                    == kmer_15_window_10_byte_8_uint32_hash_candidate_blocks
                && kmer_15_window_10_byte_8_packed_22_lane_visits
                    == kmer_15_window_10_byte_8_uint32_hash_lane_visits
                && generic_u64_candidate_blocks == 0
                && generic_u64_lane_visits == 0);
        const std::uint64_t route_calls = generic_selected
            ? generic_u64_kernel_calls
            : kmer_15_window_10_byte_8_uint32_hash_kernel_calls;
        const bool exact_backend = !saturated
            && fail_closed_rejections == 0
            && bridge_symbols_staged == 0
            && bridge_vector_runs == 0
            && scalar_backend_branches == 0
            && scalar_tail_symbols == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_event_records == 0
            && scalar_sort_records == 0
            && scalar_dedup_records == 0
            && packed_decode_lanes == bp_consumed
            && fragment_symbols_staged == bp_consumed
            && ((bp_consumed == 0 && packed_decode_batches == 0)
                || (bp_consumed > 0 && packed_decode_batches > 0))
            && typed_route_complete;
        if (!exact_backend) return false;

        const bool has_vector_activity = vector_batches != 0
            || vector_lanes_visited != 0
            || generic_u64_candidate_blocks != 0
            || kmer_15_window_10_byte_8_uint32_hash_candidate_blocks != 0
            || kmer_15_window_10_byte_8_packed_22_groups != 0
            || generic_u64_lane_visits != 0
            || kmer_15_window_10_byte_8_uint32_hash_lane_visits != 0
            || kmer_15_window_10_byte_8_packed_22_lane_visits != 0;
        if (!has_vector_activity) {
            const bool flush_batches_complete =
                event_generation_batches == route_calls
                && stable_compress_batches == route_calls;
            const bool empty_stage_consistent =
                vector_batches == 0
                && vector_lanes_visited == 0
                && masked_tail_lanes == 0
                && consecutive_kmer_batches == 0
                && canonical_scramble_batches == 0
                && window_reduction_batches == 0
                && flush_batches_complete
                && direct_event_compress_batches == 0
                && tie_matrix_event_batches == 0
                && row_transpose_batches == 0
                && row_vector_stores == 0;
            return empty_stage_consistent
                && !require_nonempty_vector_work;
        }

        const bool event_routes_complete =
            direct_event_compress_batches <= maximum - tie_matrix_event_batches
            && direct_event_compress_batches + tie_matrix_event_batches
                == vector_batches
            && (tie_matrix_event_batches == 0
                || (row_transpose_batches > 0 && row_vector_stores > 0));
        const bool event_batches_complete =
            vector_batches <= maximum - route_calls
            && event_generation_batches == vector_batches + route_calls;
        const bool stable_batches_complete =
            vector_batches <= maximum - route_calls
            && stable_compress_batches >= vector_batches + route_calls;
        const bool active_stage_consistent =
            vector_batches > 0
            && vector_lanes_visited > 0
            && consecutive_kmer_batches == canonical_scramble_batches
            && consecutive_kmer_batches <= vector_batches
            && window_reduction_batches == vector_batches
            && event_batches_complete
            && stable_batches_complete
            && event_routes_complete
            && packed_decode_batches > 0
            && packed_decode_lanes > 0
            && typed_route_complete;
        if (!active_stage_consistent) return false;
        return !require_nonempty_vector_work
            || (bp_consumed > 0
                && consecutive_kmer_batches > 0
                && canonical_scramble_batches > 0);
    }

    [[nodiscard]] constexpr bool operator==(
        const two_stack_execution_stats&) const noexcept = default;
};

/** Final status and counters from a two-stack continuation. */
struct two_stack_finalize_result final {
    two_stack_continuation_status status = two_stack_continuation_status::ok;
    two_stack_execution_stats stats{};

    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return status == two_stack_continuation_status::ok;
    }
};

/** Exact packed bounded_nthash-ntHash status and evidence models. */
enum class bounded_nthash_status : std::uint8_t {
    ok = 0,
    invalid_configuration,
    input_too_short,
    unsupported_alphabet,
    unsupported_storage,
    unsupported_representation,
    unsupported_vector_shape,
    invalid_range,
    input_too_large,
};

/** Stable persisted identity for the bounded ntHash scheme. */
inline constexpr std::uint32_t k_bounded_nthash_scheme_id =
    0x504E5431u;
inline constexpr std::size_t k_bounded_nthash_max_window = 64;

/**
 * Algorithm-local evidence for the packed bounded_nthash-ntHash kernel.
 *
 * The scalar append count is truthful orchestration after fixed-width SIMD
 * has selected a complete (hash, position, strand) event. It is not a
 * hash/window fallback. All scalar-work and fallback counters must remain zero
 * on every is_internally_consistent execution.
 */
struct bounded_nthash_execution_stats {
    std::uint32_t scheme_id = 0;
    std::uint32_t kmer_size = 0;
    std::uint32_t window_size = 0;
    std::uint32_t nthash_rotation = 0;
    std::uint32_t source_storage_unit_bytes = 0;
    std::uint64_t input_symbols = 0;
    std::uint64_t input_storage_units = 0;
    std::uint64_t segments_combined = 0;
    std::uint64_t logical_streams = 0;
    std::uint64_t physical_vector_lanes = 0;
    std::uint64_t vector_registers_per_step = 0;
    std::uint64_t packed_decode_batches = 0;
    /** SIMD lane visits, including per-lane p_kmer_size/p_window_size context; not unique input bp. */
    std::uint64_t packed_decode_lane_visits = 0;
    std::uint64_t hash_initialization_batches = 0;
    std::uint64_t rolling_hash_batches = 0;
    std::uint64_t hash_table_lookup_batches = 0;
    std::uint64_t canonical_sum_batches = 0;
    std::uint64_t window_update_batches = 0;
    std::uint64_t suffix_rebuild_batches = 0;
    std::uint64_t emission_decision_batches = 0;
    std::uint64_t emission_mask_store_batches = 0;
    std::uint64_t minimizers_emitted = 0;
    std::uint64_t tied_endpoint_records_emitted = 0;
    std::uint64_t stream_boundary_records_buffered = 0;
    std::uint64_t stream_boundary_records_committed = 0;
    std::uint64_t stream_boundary_duplicates_suppressed = 0;
    std::uint64_t scalar_collector_appends = 0;
    std::uint64_t partial_chunk_callback_batches = 0;
    std::uint64_t masked_tail_lane_visits = 0;
    std::uint64_t fail_closed_rejections = 0;
    std::uint64_t scalar_hash_symbols = 0;
    std::uint64_t scalar_window_transitions = 0;
    std::uint64_t scalar_minimum_decisions = 0;
    std::uint64_t fallback_calls = 0;
    bool positions_are_segment_local = false;
    bool aggregation_mismatch = false;
    bool saturated = false;

    [[nodiscard]] constexpr bool operator==(
        const bounded_nthash_execution_stats&) const noexcept = default;

    static void saturating_add(
        std::uint64_t& destination,
        std::uint64_t increment,
        bool& did_saturate) noexcept
    {
        const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        if (increment > maximum - destination) {
            destination = maximum;
            did_saturate = true;
        } else {
            destination += increment;
        }
    }

    /** Saturating, semantic-identity-checked segment aggregation. */
    void merge(const bounded_nthash_execution_stats& other) noexcept {
        if (other == bounded_nthash_execution_stats{}) return;
        if (*this == bounded_nthash_execution_stats{}) {
            *this = other;
            return;
        }
        const bool same_identity =
            segments_combined > 0
            && other.segments_combined > 0
            && scheme_id == other.scheme_id
            && kmer_size == other.kmer_size
            && window_size == other.window_size
            && nthash_rotation == other.nthash_rotation
            && source_storage_unit_bytes
                == other.source_storage_unit_bytes
            && logical_streams == other.logical_streams
            && physical_vector_lanes == other.physical_vector_lanes
            && vector_registers_per_step
                == other.vector_registers_per_step
            && positions_are_segment_local
                == other.positions_are_segment_local;
        aggregation_mismatch = aggregation_mismatch
            || other.aggregation_mismatch || !same_identity;
        saturated = saturated || other.saturated;
#define BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(name) \
        saturating_add(name, other.name, saturated)
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(input_symbols);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(input_storage_units);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(segments_combined);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(packed_decode_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(packed_decode_lane_visits);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(hash_initialization_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(rolling_hash_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(hash_table_lookup_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(canonical_sum_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(window_update_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(suffix_rebuild_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(emission_decision_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(emission_mask_store_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(minimizers_emitted);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(tied_endpoint_records_emitted);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(stream_boundary_records_buffered);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(stream_boundary_records_committed);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(stream_boundary_duplicates_suppressed);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_collector_appends);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(partial_chunk_callback_batches);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(masked_tail_lane_visits);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(fail_closed_rejections);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_hash_symbols);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_window_transitions);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(scalar_minimum_decisions);
        BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER(fallback_calls);
#undef BIOSEQ_MERGE_BOUNDED_NTHASH_COUNTER
    }

    [[nodiscard]] constexpr bool is_internally_consistent(
        bool require_nonempty_vector_work = true) const noexcept {
        const bool vector_work_ok = !require_nonempty_vector_work
            || (packed_decode_batches > 0
                && packed_decode_lane_visits > 0
                && hash_initialization_batches > 0
                && hash_table_lookup_batches > 0
                && canonical_sum_batches > 0
                && window_update_batches > 0
                && emission_decision_batches > 0
                && emission_mask_store_batches > 0);
        return !saturated
            && !aggregation_mismatch
            && scheme_id == k_bounded_nthash_scheme_id
            && kmer_size > 0
            && window_size > 0
            && window_size <= k_bounded_nthash_max_window
            && nthash_rotation == 7
            && source_storage_unit_bytes >= sizeof(std::uint32_t)
            && logical_streams == 8
            && (physical_vector_lanes == 4
                || physical_vector_lanes == 8)
            && vector_registers_per_step * physical_vector_lanes == 8
            && segments_combined > 0
            && positions_are_segment_local
            && vector_work_ok
            && canonical_sum_batches == window_update_batches
            && minimizers_emitted == scalar_collector_appends
            && stream_boundary_records_buffered
                == stream_boundary_records_committed
                    + stream_boundary_duplicates_suppressed
            && fail_closed_rejections == 0
            && scalar_hash_symbols == 0
            && scalar_window_transitions == 0
            && scalar_minimum_decisions == 0
            && fallback_calls == 0;
    }
};

struct bounded_nthash_result {
    bounded_nthash_status status = bounded_nthash_status::ok;
    bounded_nthash_execution_stats stats{};

    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return status == bounded_nthash_status::ok;
    }

    /** Kernel-local admission only; pipeline routing adds stronger evidence. */
    [[nodiscard]] constexpr bool has_valid_kernel_stats(
        bool require_nonempty_vector_work = true) const noexcept {
        return is_ok() && stats.is_internally_consistent(require_nonempty_vector_work);
    }
};

/** Sentinel used only before source/topology alphabet evidence is attached. */
inline constexpr std::uint8_t k_unbound_packed_alphabet_code =
    (std::numeric_limits<std::uint8_t>::max)();

/**
 * Borrowed, already-resident packed sequence segment.
 *
 * `storage` points at the first aligned packed unit. `begin_symbol` is the
 * leading-symbol offset within that borrowed unit range and `end_symbol` is
 * the exclusive local packed coordinate, so the biological length is
 * `end_symbol - begin_symbol`. Continuation state owns the global biological
 * coordinate. Packing is least-significant-symbol first, matching the only
 * sequence storage order currently registered by the scheduler.
 */
struct packed_sequence_kernel_view final {
    const std::byte* storage = nullptr;
    std::size_t storage_units = 0u;
    std::size_t begin_symbol = 0u;
    std::size_t end_symbol = 0u;
    std::uint8_t alphabet_code = k_unbound_packed_alphabet_code;
    std::uint8_t bits_per_symbol = 0u;
    std::uint8_t storage_unit_bytes = 0u;
    bool final_segment = false;
};

/** One type-erased view of storage for live objects of one exact layout. */
struct minimizer_sink_batch_view final {
    void* data = nullptr;
    std::size_t capacity = 0u;
    minimizer_output_layout layout = minimizer_output_layout::unsupported;
    std::size_t record_size = 0u;
    std::size_t record_alignment = 0u;
};

static_assert(std::is_standard_layout_v<minimizer_sink_batch_view>);
static_assert(std::is_trivially_copyable_v<minimizer_sink_batch_view>);

/**
 * Batch-oriented output adapter; callbacks are never invoked per base/lane.
 * `acquire` returns uninitialized storage whose identity is validated once by
 * the target collector before any `minimizer_representation` object is constructed.
 * Genuine u64 layouts may retain their raw representation through the
 * optional layout-checked fast path.
 */
struct minimizer_sink final {
    void* state = nullptr;
    minimizer_sink_batch_view (*acquire)(
        void*, std::size_t minimum) = nullptr;
    void (*commit)(void*, std::size_t initialized) noexcept = nullptr;
    bool (*append_raw_u64)(
        void*,
        minimizer_output_layout,
        const std::uint64_t*,
        std::size_t) noexcept = nullptr;
};

/** One borrowed packed fragment consumed synchronously by an two_stack_nthash call. */
struct packed_sequence_kernel_fragment final {
    const std::byte* storage = nullptr;
    std::size_t storage_units = 0u;
    std::size_t fragment_bp_size = 0u;
    std::size_t fragment_begin_bp = 0u;
    std::size_t consume_begin_bp = 0u;
    std::size_t consume_end_bp = 0u;
};

/**
 * Pull source for a bounded fragment interval. The target-local continuation
 * is created and finalized during the single table call; only borrowed packed
 * fragments cross this neutral callback boundary.
 */
struct packed_sequence_fragment_source final {
    void* state = nullptr;
    std::size_t first_fragment = 0u;
    std::size_t fragment_end = 0u;
    packed_sequence_kernel_fragment (*read)(void*, std::size_t) = nullptr;
    void (*release)(void*, std::size_t) noexcept = nullptr;

    [[nodiscard]] constexpr bool is_present() const noexcept {
        return read != nullptr || release != nullptr;
    }
};

/**
 * Batch adapter shared by all generated minimizer families. Exact kernels keep
 * their established collector surface; acquire/commit is crossed only when a
 * full batch is ready or the coarse wrapper finishes. The batch is acquired
 * lazily from the sink and written in place, avoiding a second local buffer.
 */
template<class p_minimizer_type>
class minimizer_sink_collector {
public:
    using minimizer_type = p_minimizer_type;
    static constexpr std::size_t k_batch_capacity = 256u;
    static constexpr minimizer_output_layout k_output_layout =
        k_minimizer_output_layout<p_minimizer_type>;

    static_assert(k_output_layout != minimizer_output_layout::unsupported);
    static_assert(std::is_standard_layout_v<p_minimizer_type>);
    static_assert(std::is_trivially_copyable_v<p_minimizer_type>);
    static_assert(std::is_trivially_destructible_v<p_minimizer_type>);

    /** Bind a validated batch sink. */
    explicit minimizer_sink_collector(minimizer_sink sink) noexcept
        : sink_(sink),
          failed_(sink_.acquire == nullptr || sink_.commit == nullptr) {}

    /** Queue one movable minimizer record. */
    void collect(p_minimizer_type&& sketch) {
        enqueue(std::move(sketch));
    }

    /** Queue one copied minimizer record. */
    void collect(const p_minimizer_type& sketch) {
        enqueue(sketch);
    }

    /** Append records directly from exact packed storage. */
    template<class p_record_type = p_minimizer_type>
    [[nodiscard]] bool append_raw_storage(
        const typename p_record_type::storage_type* raw,
        std::size_t count)
        requires (
            std::same_as<p_record_type, p_minimizer_type>
            && requires { typename p_record_type::storage_type; }
            && std::is_trivially_copyable_v<p_record_type>
            && sizeof(p_record_type) == sizeof(typename p_record_type::storage_type)
            && requires (typename p_record_type::storage_type raw_value, p_record_type sketch) {
                { p_record_type::from_raw_storage(raw_value) } -> std::same_as<p_record_type>;
                { sketch.kmer() } -> std::convertible_to<std::uint64_t>;
                { sketch.last_pos() }
                    -> std::convertible_to<std::uint64_t>;
                { sketch.strand() } -> std::convertible_to<std::uint64_t>;
            })
    {
        if (!flush() || failed_) {
            return false;
        }
        if (count == 0u) {
            return true;
        }
        if constexpr (
            std::same_as<typename p_record_type::storage_type, std::uint64_t>
            && k_minimizer_output_layout<p_record_type>
                != minimizer_output_layout::unsupported) {
            if (sink_.append_raw_u64 != nullptr) {
                if (!sink_.append_raw_u64(
                        sink_.state,
                        k_minimizer_output_layout<p_record_type>,
                        raw,
                        count)) {
                    failed_ = true;
                    return false;
                }
                emitted_ += count;
                return true;
            }
        }
        std::size_t offset = 0u;
        while (offset < count) {
            const std::size_t chunk =
                std::min(k_batch_capacity, count - offset);
            if (!acquire_slots(chunk)) {
                return false;
            }
            for (std::size_t i = 0u; i < chunk; ++i) {
                std::construct_at(
                    pending_ + i,
                    p_record_type::from_raw_storage(raw[offset + i]));
            }
            buffered_ = chunk;
            if (!flush()) {
                return false;
            }
            offset += chunk;
        }
        return true;
    }

    /** Validate reserve intent; allocation remains sink-owned. */
    [[nodiscard]] bool reserve(std::size_t) const noexcept {
        return !failed_;
    }

    /** Total committed and buffered output records. */
    [[nodiscard]] std::size_t size() const noexcept {
        return emitted_ + buffered_;
    }

    /** Records whose sink commit callback has completed. */
    [[nodiscard]] std::size_t committed_size() const noexcept {
        return emitted_;
    }

    /** Whether every sink operation has succeeded. */
    [[nodiscard]] bool is_ok() const noexcept {
        return !failed_;
    }

    /** Commit the pending batch to the sink. */
    [[nodiscard]] bool flush() {
        if (failed_) {
            return false;
        }
        if (buffered_ == 0u) {
            return true;
        }
        sink_.commit(sink_.state, buffered_);
        emitted_ += buffered_;
        pending_ = nullptr;
        buffered_ = 0u;
        return true;
    }

private:
    [[nodiscard]] bool acquire_slots(const std::size_t minimum) {
        const minimizer_sink_batch_view batch =
            sink_.acquire(sink_.state, minimum);
        if (batch.data == nullptr
            || batch.capacity < minimum
            || batch.layout != k_output_layout
            || batch.record_size != sizeof(p_minimizer_type)
            || batch.record_alignment != alignof(p_minimizer_type)
            || reinterpret_cast<std::uintptr_t>(batch.data)
                    % alignof(p_minimizer_type) != 0u) {
            failed_ = true;
            return false;
        }
        pending_ = static_cast<p_minimizer_type*>(batch.data);
        return true;
    }

    template<class p_value_type>
    void enqueue(p_value_type&& sketch) {
        if (failed_) {
            return;
        }
        if (buffered_ == k_batch_capacity && !flush()) {
            return;
        }
        if (buffered_ == 0u) {
            if (!acquire_slots(k_batch_capacity)) {
                return;
            }
        }
        // Preserve the exact object representation selected by the owner.
        // In particular, split 128-bit layouts carry an explicit span byte;
        // rebuilding from only hash/position/strand would silently replace a
        // non-default span even though the neutral ABI promises exact p_record_type.
        std::construct_at(
            pending_ + buffered_, std::forward<p_value_type>(sketch));
        ++buffered_;
    }

    minimizer_sink sink_{};
    p_minimizer_type* pending_ = nullptr;
    std::size_t buffered_ = 0u;
    std::size_t emitted_ = 0u;
    bool failed_ = false;
};

namespace kernel_detail {

/** Header shape consumed by the existing zero-copy compact range adapter. */
struct borrowed_packed_header {
    const void* data = nullptr;
    std::size_t bp_size = 0u;
    std::uint8_t storage_unit_byte_width = 0u;
    std::uint16_t padding_letters = 0u;
};

template<sequence_alphabet p_alphabet, class p_storage_unit>
struct borrowed_packed_instance {
    using alphabet_type = p_alphabet;
    using storage_unit = p_storage_unit;
    using compact_sequence_stdc_type = borrowed_packed_header;

    static constexpr std::size_t letter_bit_width =
        seqan3::detail::ceil_log2(seqan3::alphabet_size<p_alphabet>);
    static constexpr std::size_t byte_width = sizeof(p_storage_unit);
    static constexpr bool is_valid_compact_sequence_stdc_instance = true;
};

/** Validate and borrow a packed view without materialising or re-encoding. */
template<
    sequence_alphabet p_alphabet,
    class p_storage_unit,
    class p_function>
[[nodiscard]] bool with_borrowed_compact_range(
    const packed_sequence_kernel_view& sequence,
    p_function&& function)
{
    using instance = borrowed_packed_instance<p_alphabet, p_storage_unit>;
    using range_type = compact_sequence_range<instance>;
    constexpr std::size_t bits_per_symbol = instance::letter_bit_width;
    constexpr std::size_t letters_per_unit =
        sizeof(p_storage_unit) * 8u / bits_per_symbol;

    if (sequence.bits_per_symbol != bits_per_symbol
        || sequence.storage_unit_bytes != sizeof(p_storage_unit)
        || sequence.end_symbol < sequence.begin_symbol
        || (sequence.end_symbol != 0u && sequence.storage == nullptr)
        || (sequence.storage != nullptr
            && reinterpret_cast<std::uintptr_t>(sequence.storage)
                % alignof(p_storage_unit) != 0u)) {
        return false;
    }

    const std::size_t bp_size = sequence.end_symbol;
    if (bp_size > (std::numeric_limits<std::size_t>::max)()
            - (letters_per_unit - 1u)) {
        return false;
    }
    const std::size_t expected_units =
        (bp_size + letters_per_unit - 1u) / letters_per_unit;
    if (sequence.storage_units != expected_units) {
        return false;
    }
    const std::size_t padding =
        expected_units * letters_per_unit - bp_size;
    if (padding > (std::numeric_limits<std::uint16_t>::max)()) {
        return false;
    }

    borrowed_packed_header header{
        .data = sequence.storage,
        .bp_size = bp_size,
        .storage_unit_byte_width =
            static_cast<std::uint8_t>(sizeof(p_storage_unit)),
        .padding_letters = static_cast<std::uint16_t>(padding),
    };
    range_type range{header};
    std::forward<p_function>(function)(range);
    return true;
}

} // namespace kernel_detail

/** Target-neutral completion status for a minimizer kernel call. */
enum class minimizer_kernel_status : std::uint8_t {
    ok = 0u,
    invalid_sequence_view,
    invalid_configuration,
    invalid_continuation,
    sink_rejected,
};

/** Algorithm identity recorded in minimizer execution statistics. */
enum class minimizer_kernel_algorithm : std::uint8_t {
    none = 0u,
    minimap2_reference,
    two_stack_nthash,
    bounded_nthash,
};

/**
 * Biological two_stack_nthash sub-route selected by neutral orchestration.
 *
 * This is not a Highway target choice.  The scheduler has already selected
 * one fixed-target table before either algorithm is entered; the target leaf
 * merely selects between the two existing two-stack algorithms for the
 * admitted input shape.
 */
enum class two_stack_route : std::uint8_t {
    chronological_continuation = 0u,
    resident_vector_stream = 1u,
};

/** Complete type-erased input and output contract for one kernel call. */
struct minimizer_kernel_invocation final {
    packed_sequence_kernel_view sequence{};
    packed_sequence_fragment_source fragments{};
    std::size_t window = 0u;
    std::size_t kmer = 0u;
    std::size_t emitted_position_base = 0u;
    minimizer_output_layout output_layout = minimizer_output_layout::unsupported;
    std::uint64_t output_hash_mask = minimap_sketch_data::k_hash_field_mask;
    std::uint64_t output_position_mask = minimap_sketch_data::k_position_field_mask;
    ::bioseq::minimizers::two_stack_route two_stack_route =
        ::bioseq::minimizers::two_stack_route::chronological_continuation;
    minimizer_sink sink{};
};

/** Operation-derived counters for the minimap2 reference SIMD kernel. */
struct reference_minimizer_execution_stats final {
    std::uint64_t kernel_calls = 0u;
    std::uint64_t vector_callback_batches = 0u;
    std::uint64_t vector_lane_visits = 0u;
    std::uint64_t last_storage_unit_bytes = 0u;
    std::uint64_t scalable_storage_fallback_calls = 0u;
    std::uint64_t scalable_storage_fallback_input_symbols = 0u;
    std::uint64_t scalable_storage_fallback_minimizers_emitted = 0u;
};

/** Exact family evidence; only the member named by `algorithm` is populated. */
struct minimizer_execution_stats final {
    minimizer_kernel_algorithm algorithm = minimizer_kernel_algorithm::none;
    reference_minimizer_execution_stats reference{};
    two_stack_execution_stats two_stack{};
    bounded_nthash_execution_stats bounded_nthash{};
    std::int64_t target = 0;
};

/** Target-neutral result returned by every registered minimizer kernel. */
struct minimizer_kernel_result final {
    minimizer_execution_stats stats{};
    std::size_t emitted = 0u;
    minimizer_kernel_status status = minimizer_kernel_status::ok;
    /** Unchanged numeric value of the selected family's native status enum. */
    std::uint32_t family_status = 0u;

    [[nodiscard]] constexpr bool is_ok() const noexcept {
        return status == minimizer_kernel_status::ok;
    }
};

/** Uniform function-pointer signature stored in the Highway dispatch table. */
using minimizer_kernel_function = minimizer_kernel_result (*)(
    minimizer_kernel_invocation&);

/** Registered minimizer algorithms for one Highway target. */
struct minimizer_kernels final {
    minimizer_kernel_function minimap2_reference = nullptr;
    minimizer_kernel_function two_stack_nthash = nullptr;
    minimizer_kernel_function bounded_nthash = nullptr;
};

static_assert(std::is_standard_layout_v<packed_sequence_kernel_view>);
static_assert(std::is_trivially_copyable_v<packed_sequence_kernel_view>);
static_assert(std::is_standard_layout_v<minimizer_sink>);
static_assert(std::is_trivially_copyable_v<minimizer_sink>);
static_assert(std::is_standard_layout_v<packed_sequence_kernel_fragment>);
static_assert(std::is_trivially_copyable_v<packed_sequence_kernel_fragment>);
static_assert(std::is_standard_layout_v<packed_sequence_fragment_source>);
static_assert(std::is_trivially_copyable_v<packed_sequence_fragment_source>);
static_assert(std::is_standard_layout_v<minimizer_kernel_invocation>);
static_assert(std::is_trivially_copyable_v<minimizer_kernel_invocation>);
static_assert(std::is_standard_layout_v<reference_minimizer_execution_stats>);
static_assert(std::is_trivially_copyable_v<reference_minimizer_execution_stats>);
static_assert(std::is_standard_layout_v<minimizer_execution_stats>);
static_assert(std::is_trivially_copyable_v<minimizer_execution_stats>);
static_assert(std::is_standard_layout_v<minimizer_kernel_result>);
static_assert(std::is_trivially_copyable_v<minimizer_kernel_result>);
static_assert(std::is_standard_layout_v<minimizer_kernels>);
static_assert(std::is_trivially_copyable_v<minimizer_kernels>);

} // namespace bioseq::minimizers
