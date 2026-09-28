/**
 * @file collectors.cppm
 * @brief Stores minimizer records and identifies supported output formats.
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

export module bioseq.sequences.kmers.highway_minimizer_contract:collectors;

import bioseq.common.seqan_adapter;
import bioseq.sequences;
import bioseq.storage.size;
import :representation;

export namespace bioseq::minimizers {

/**
 * @brief Appends minimizer records to a vector in collection order
 * The record type controls how hashes and positions are packed. The default
 * is minimap_sketch_data, a 64-bit record with a 28-bit position field.
 */
template<class p_minimizer_type = minimap_sketch_data>
struct basic_sketch_collector {
private:
    std::vector<p_minimizer_type> sketches_;
public:
    using minimizer_type = p_minimizer_type;

    /** Creates an empty collector without allocating a record buffer. */
    basic_sketch_collector() = default;

    /** @brief Reserves room  for at least expected_count records. */
    void reserve(size_t expected_count) {
        sketches_.reserve(expected_count);
    }

    /**
     * @brief Appends one movable minimizer record.
     * @param sketch Record transferred into the collector.
     */
    [[always_inline]] constexpr void collect(p_minimizer_type&& sketch) {
        sketches_.push_back(std::move(sketch));
    }

    /**
     * @brief Appends one copied minimizer record.
     * @param sketch Record copied into the collector.
     */
    [[always_inline]] constexpr void collect(const p_minimizer_type& sketch) {
        sketches_.push_back(sketch);
    }

    /**
     * @brief Appends an already-constructed record batch.
     * @param records Records copied in their existing order.
     * @return false when the resulting size exceeds vector limits.
     */
    [[nodiscard, always_inline]] bool append_records(
        const std::span<const p_minimizer_type> records)
    {
        if (records.size() > sketches_.max_size() - sketches_.size()) {
            return false;
        }
        sketches_.insert(sketches_.end(), records.begin(), records.end());
        return true;
    }

    /**
     * @brief Reconstructs records from packed words and appends them in order.
     * @return false if the resulting record count exceeds the vector limit.
     */
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

    /** @brief Returns a copy of all records in collection order. */
    constexpr std::vector<p_minimizer_type> extract_sketches() const {
        return sketches_;
    }

    /** @brief Returns a reference to the collector's record vector. */
    constexpr const std::vector<p_minimizer_type>& extract_sketches_data() const & {
        return sketches_;
    }

    /** @brief Transfers all records out of an rvalue collector. */
    constexpr std::vector<p_minimizer_type> extract_sketches_data() && {
        return std::move(sketches_);
    }

    /** @brief Returns unique (k-mer end position, hash) pairs sorted by position, then hash. */
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

    /** @brief Removes all records while keeping allocated capacity. */
    [[always_inline]] constexpr void clear() {
        sketches_.clear();
    }
};

/** Collector for 64-bit minimizer records with 28-bit positions. */
using sketch_collector = basic_sketch_collector<minimap_sketch_data>;

/**
 * @brief Stores minimizers while reserving memory for the vector's capacity.
 *
 * Growth reserves room for both old and  replacement buffers efore allocating.
 * A denied reservation stops further appends. Check is_ok() after collection.
 *
 * The permit type is supplied by the caller so this, module stays independent
 * of the memory controller. Member order frees the vector before its permit.
 */
template<class p_minimizer_type, class p_permit_type>
class admitted_sketch_collector {
public:
    using minimizer_type = p_minimizer_type;
    static constexpr std::size_t k_initial_growth_records = 8u;

    /**
     * @brief Starts an empty collector with a valid zero-byte memory permit.
     * allocation_headroom_factor reserves extra room for allocator growth.
     * An invalid permit or zero factor marks the collector as failed.
     */
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

    /**
     * @brief Reserves memory and vector capacity  for at least expected_count records.
     * @return false if the collector has failed or cannot reserve enough memory
     */
    [[nodiscard]] bool reserve(std::size_t expected_count) {
        return ensure_capacity(expected_count);
    }

    /** @brief Moves one record into the vector if its memory reservation succeeds. */
    [[always_inline]] void collect(p_minimizer_type&& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(std::move(sketch));
        }
    }

    /** @brief Copies one record into the vector if its memory reservation succeeds. */
    [[always_inline]] void collect(const p_minimizer_type& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(sketch);
        }
    }

    /**
     * @brief Reserves room for the whole batch before appending records in order.
     * @return false if the size or memory reservation is rejected.
     */
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

    /**
     * @brief Reserves room, reconstructs records from packed words and appends them.
     * @return false if the size or memory reservation is rejected.
     */
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

    /** @brief Returns a reference to the collected record vector. */
    [[nodiscard]] const std::vector<p_minimizer_type>&
    extract_sketches_data() const & noexcept {
        return sketches_;
    }

    /** @brief Reports whether capacity checks and  memory reservations have succeeded. */
    [[nodiscard]] bool is_ok() const noexcept {
        return !failed_;
    }

    /** @brief Returns the bytes currently resered by the memory permit. */
    [[nodiscard]] std::uint64_t admitted_capacity_bytes() const noexcept {
        return permit_.bytes();
    }

    /** @brief Transfers the record vector together with its capacity permit. */
    [[nodiscard]] std::pair<std::vector<p_minimizer_type>, p_permit_type>
    release_owned_storage() && {
        return {
            std::move(sketches_),
            std::move(permit_),
        };
    }

private:
    /**
     * @brief Computes exact record bytes without integer overflow.
     * @param count Record count.
     * @param result Receives the exact byte count on success.
     * @return `false` when multiplication exceeds `uint64_t`.
     */
    [[nodiscard]] static bool checked_bytes(
            std::size_t count,
            std::uint64_t& result) noexcept {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        if (count > maximum / sizeof(p_minimizer_type)) return false;
        result = static_cast<std::uint64_t>(count) * sizeof(p_minimizer_type);
        return true;
    }

    /**
     * @brief Grows the record buffer after reserving memory for the allocation peak.
     * Reduces the reservation to the buffer capacity after allocation.
     * @return false if capacity checks or memory reservations fail.
     */
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
        // Use the capacity and peak reservation that repeated, single-record
        // appends would reach. Allocate only the final buffer to avoid
        // copying through  every intermediate size.
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
            // Keep the reservation while this allocation exists. Stop collecting
            // because its reported capacity exceeds the reserved allowance.
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

/** @brief Requires a 64-bit minimizer record with packed-word access and field masks. */
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

/** @brief Requires a 128-bit minimizer record stored as two accessible 64-bit halves. */
template<class p_minimizer_type>
concept wide_simd_minimizer_representation =
    std::is_standard_layout_v<p_minimizer_type>
    && std::is_trivially_copyable_v<p_minimizer_type>
    && sizeof(p_minimizer_type) == 2u * sizeof(std::uint64_t)
    && requires (
        p_minimizer_type value,
        std::uint64_t hash,
        std::uint64_t position,
        std::uint64_t strand,
        std::uint64_t span) {
        typename p_minimizer_type::layout_type;
        requires p_minimizer_type::layout_type::k_storage_bits == 128u;
        { p_minimizer_type{hash, position, strand, span} }
            -> std::same_as<p_minimizer_type>;
        { p_minimizer_type::maximum() } -> std::same_as<p_minimizer_type>;
        { value.kmer() } -> std::convertible_to<std::uint64_t>;
        { value.pos_strand_encoded() } -> std::convertible_to<std::uint64_t>;
        { value.data_low_64() } -> std::convertible_to<std::uint64_t>;
        { value.data_high_64() } -> std::convertible_to<std::uint64_t>;
        { p_minimizer_type::k_hash_field_mask }
            -> std::convertible_to<std::uint64_t>;
        { p_minimizer_type::k_position_field_mask }
            -> std::convertible_to<std::uint64_t>;
    };

/** Record formats accepted by the two-stack SIMD minimizer kernel */
template<class p_minimizer_type>
concept two_stack_simd_minimizer_representation =
    exact_simd_minimizer_representation<p_minimizer_type>
    || wide_simd_minimizer_representation<p_minimizer_type>;

/**
 * @brief Identifies the output record format requested from a generated kernel.
 * The selected minimizer_representation type defines how that  format is packed.
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
    sequence_log2_20_kmer_20_uint64 = 11u,
    sequence_log2_23_kmer_20_uint64 = 12u,
    sequence_log2_63_kmer_20_uint128 = 13u,
};

/** @brief Returns a record type's output format, or unsupporte for unregistered types. */
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
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<20u, 20u>>) {
        return minimizer_output_layout::sequence_log2_20_kmer_20_uint64;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<23u, 20u>>) {
        return minimizer_output_layout::sequence_log2_23_kmer_20_uint64;
    } else if constexpr (
        std::same_as<output_type, minimizer_representation<63u, 20u>>) {
        return minimizer_output_layout::sequence_log2_63_kmer_20_uint128;
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
static_assert(sizeof(minimizer_representation<20u, 20u>) == 8u);
static_assert(sizeof(minimizer_representation<23u, 20u>) == 8u);
static_assert(sizeof(minimizer_representation<63u, 20u>) == 16u);
static_assert(minimizer_representation<20u, 20u>::layout_type::k_hash_bits >= 40u);
static_assert(minimizer_representation<23u, 20u>::layout_type::k_hash_bits >= 40u);
static_assert(minimizer_representation<63u, 20u>::layout_type::k_hash_bits >= 40u);
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
static_assert(k_minimizer_output_layout<minimizer_representation<20u, 20u>>
    == minimizer_output_layout::sequence_log2_20_kmer_20_uint64);
static_assert(k_minimizer_output_layout<minimizer_representation<23u, 20u>>
    == minimizer_output_layout::sequence_log2_23_kmer_20_uint64);
static_assert(k_minimizer_output_layout<minimizer_representation<63u, 20u>>
    == minimizer_output_layout::sequence_log2_63_kmer_20_uint128);

/**
 * @brief Returns the position mask encoded by an output layout.
 * @param layout Exact minimizer output layout.
 * @return Position mask, or zero for an unsupported layout.
 */
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
        case minimizer_output_layout::sequence_log2_20_kmer_20_uint64:
            return minimizer_representation<20u, 20u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_23_kmer_20_uint64:
            return minimizer_representation<23u, 20u>::k_position_field_mask;
        case minimizer_output_layout::sequence_log2_63_kmer_20_uint128:
            return minimizer_representation<63u, 20u>::k_position_field_mask;
        case minimizer_output_layout::unsupported:
            break;
    }
    return 0u;
}

} // namespace bioseq::minimizers
