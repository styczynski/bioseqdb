/**
 * @file representation.cppm
 * @brief Exact minimizer record layouts and query bounds.
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

export module bioseq.sequences.kmers.highway_minimizer_contract:representation;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.size;

export namespace bioseq::minimizers {

/**
 * @brief Selects the smallest packed record that preserves a minimizer shape.
 * @tparam p_max_sequence_log2 Position bits required by the sequence bound.
 * @tparam p_kmer_length K-mer length used to size the hash field.
 */
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

/**
 * @brief Stores one exact minimizer shape in its selected packed layout.
 * @tparam p_max_sequence_log2 Position bits carried by the record.
 * @tparam p_kmer_length K-mer length used to size the hash field.
 * @tparam p_is_128_bit Selects the two-word layout when `true`.
 */
template<std::size_t p_max_sequence_log2, std::size_t p_kmer_length,
         bool p_is_128_bit =
             (minimizer_layout<p_max_sequence_log2, p_kmer_length>::k_storage_bits == 128u)>
struct minimizer_representation;

/**
 * @brief Stores hash, position, and strand in one unsigned integer.
 * @tparam p_max_sequence_log2 Position bits carried by the record.
 * @tparam p_kmer_length K-mer length used to size the hash field.
 */
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
    /** @brief Orders records by their masked canonical hash. */
    [[nodiscard]] constexpr auto operator<=> (
        const minimizer_representation& other) const noexcept
    {
        return (value_ & k_hash_mask) <=> (other.value_ & k_hash_mask);
    }
    /** @brief Tests canonical-hash equality without position identity. */
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

/**
 * @brief Stores hash, span, position, and strand in two 64-bit words.
 * @tparam p_max_sequence_log2 Position bits carried by the record.
 * @tparam p_kmer_length K-mer length used as the default span.
 */
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
    /** @brief Returns the canonical hash field. */
    [[nodiscard]] constexpr std::uint64_t kmer() const noexcept {
        return high_ >> k_high_hash_shift;
    }
    /** @brief Returns the stored k-mer span. */
    [[nodiscard]] constexpr std::uint64_t span() const noexcept {
        return high_ & k_span_field_mask;
    }
    /** @brief Returns the zero-based position of the final k-mer symbol. */
    [[nodiscard]] constexpr std::uint64_t last_pos() const noexcept {
        return (low_ >> 1u) & k_position_field_mask;
    }
    /** @brief Returns the canonical strand bit. */
    [[nodiscard]] constexpr std::uint64_t strand() const noexcept {
        return low_ & 1ULL;
    }
    /** @brief Returns the packed position and strand word. */
    [[nodiscard]] constexpr std::uint64_t pos_strand_encoded() const noexcept {
        return low_;
    }
    /** @brief Returns the sentinel that compares above every valid hash. */
    [[nodiscard]] static constexpr minimizer_representation maximum() noexcept {
        minimizer_representation result;
        result.high_ = k_high_hash_mask;
        return result;
    }
    /** @brief Reports whether the record carries the maximum sentinel hash. */
    [[nodiscard]] constexpr bool is_max() const noexcept {
        return (high_ & k_high_hash_mask) == k_high_hash_mask;
    }
    /** @brief Replaces the record with the maximum sentinel. */
    constexpr void set_max() noexcept {
        high_ = k_high_hash_mask;
        low_ = 0u;
    }
    /** @brief Orders records by their masked canonical hash. */
    [[nodiscard]] constexpr auto operator<=> (
        const minimizer_representation& other) const noexcept
    {
        return (high_ & k_high_hash_mask) <=> (other.high_ & k_high_hash_mask);
    }
    /** @brief Tests canonical-hash equality without span or position identity. */
    [[nodiscard]] constexpr bool operator==(
        const minimizer_representation& other) const noexcept
    {
        return (high_ & k_high_hash_mask) == (other.high_ & k_high_hash_mask);
    }
    /** @brief Tests equal hashes with distinct span or position identity. */
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

/**
 * @brief Maximum sequence length representable without masking a position bit.
 * @tparam p_minimizer_type Packed minimizer record defining the position mask.
 */
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

/**
 * @brief Tests whether every zero-based query position fits the record.
 * @tparam p_minimizer_type Packed minimizer record defining the position mask.
 * @param query_length Query length in biological symbols.
 * @return `true` when the final position cannot be truncated.
 */
template <class p_minimizer_type>
[[nodiscard]] inline constexpr bool is_minimizer_query_length_representable(
        bioseq::storage::sequence_size_t query_length) noexcept {
    return query_length <= k_max_minimizer_sequence_length<p_minimizer_type>;
}

/**
 * @brief Rejects a query whose final position exceeds the record field.
 * @tparam p_minimizer_type Packed minimizer record defining the position mask.
 * @param query_length Query length in biological symbols.
 * @throws std::length_error If the final position would be truncated.
 */
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

} // namespace bioseq::minimizers
