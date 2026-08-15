/**
 * @file collectors.cppm
 * @brief Exact minimizer collectors and output-layout dispatch.
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
 * @brief Collects records of one exact minimizer representation.
 *
 * The default representation is the canonical 64-bit minimap2 sketch record.
 * Wider position and hash layouts select another `minimizer_representation`
 * instantiation without changing collection semantics.
 *
 * @tparam p_minimizer_type Packed record stored by the collector.
 */
template<class p_minimizer_type = minimap_sketch_data>
struct basic_sketch_collector {
private:
    std::vector<p_minimizer_type> sketches_;
public:
    using minimizer_type = p_minimizer_type;

    /** Construct an empty collector without speculative allocation. */
    basic_sketch_collector() = default;

    /**
     * @brief Reserves storage for a known output bound.
     * @param expected_count Maximum anticipated record count.
     */
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
     * @param records Records retained in their existing order.
     * @return `false` when the resulting size exceeds vector limits.
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
     * @brief Appends exact raw storage emitted by a SIMD collector.
     * @tparam p_record_type Record type reconstructed from each raw word.
     * @param raw First raw storage word.
     * @param count Number of raw words.
     * @return `false` when the resulting size exceeds vector limits.
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

    /** @brief Borrows all records for the collector lifetime. */
    constexpr const std::vector<p_minimizer_type>& extract_sketches_data() const & {
        return sketches_;
    }

    /** @brief Transfers all records out of an rvalue collector. */
    constexpr std::vector<p_minimizer_type> extract_sketches_data() && {
        return std::move(sketches_);
    }

    /** @brief Returns sorted unique position and hash pairs. */
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

    /** @brief Removes all records while retaining allocated capacity. */
    [[always_inline]] constexpr void clear() {
        sketches_.clear();
    }
};

/** Collector for the canonical 28-position-bit minimap2 representation. */
using sketch_collector = basic_sketch_collector<minimap_sketch_data>;

/**
 * @brief Collects minimizers under a caller-supplied memory permit.
 *
 * Vector capacity is owned by the supplied memory
 * admission permit.  `p_permit_type` is intentionally a template parameter: the core
 * k-mer module stays independent of the backend memory-admission module while
 * PostgreSQL and tests can supply `memory_permit`.
 *
 * The permit is declared before the vector, hence the vector is destroyed
 * first.  Each growth reserves the old retained capacity plus headroom for the
 * replacement allocation before calling `std::vector::reserve()`, then
 * reconciles the permit to the allocator-reported retained capacity.  A denied
 * growth is sticky and emits no partial record. Callers must check `is_ok()` after
 * kernels whose scalar `collect()` surface cannot return a status.
 *
 * @tparam p_minimizer_type Packed record stored by the collector.
 * @tparam p_permit_type Move-only permit with grow, shrink, and byte queries.
 */
template<class p_minimizer_type, class p_permit_type>
class admitted_sketch_collector {
public:
    using minimizer_type = p_minimizer_type;
    static constexpr std::size_t k_initial_growth_records = 8u;

    /**
     * @brief Binds an empty vector to an exact zero-byte permit.
     * @param permit Permit that owns all future vector capacity.
     * @param allocation_headroom_factor Allocator peak multiplier for growth.
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
     * @brief Reserves admitted capacity for an anticipated record count.
     * @param expected_count Required vector capacity.
     * @return `false` when capacity cannot be admitted safely.
     */
    [[nodiscard]] bool reserve(std::size_t expected_count) {
        return ensure_capacity(expected_count);
    }

    /**
     * @brief Appends a movable record if its capacity is admitted.
     * @param sketch Record transferred into the collector.
     */
    [[always_inline]] void collect(p_minimizer_type&& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(std::move(sketch));
        }
    }

    /**
     * @brief Appends a copied record if its capacity is admitted.
     * @param sketch Record copied into the collector.
     */
    [[always_inline]] void collect(const p_minimizer_type& sketch) {
        if (sketches_.size() == sketches_.max_size()) {
            failed_ = true;
        } else if (ensure_capacity(sketches_.size() + 1u)) {
            sketches_.push_back(sketch);
        }
    }

    /**
     * @brief Appends a batch after admitting its final capacity.
     * @param records Records retained in their existing order.
     * @return `false` when capacity cannot be admitted safely.
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
     * @brief Appends exact raw storage after admitting its final capacity.
     * @tparam p_record_type Record type reconstructed from each raw word.
     * @param raw First raw storage word.
     * @param count Number of raw words.
     * @return `false` when capacity cannot be admitted safely.
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

    /** @brief Borrows all collected records for the collector lifetime. */
    [[nodiscard]] const std::vector<p_minimizer_type>&
    extract_sketches_data() const & noexcept {
        return sketches_;
    }

    /** @brief Reports whether every requested growth and append succeeded. */
    [[nodiscard]] bool is_ok() const noexcept {
        return !failed_;
    }

    /** @brief Returns bytes currently retained by the capacity permit. */
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
     * @brief Grows vector capacity while retaining its allocation peak permit.
     * @param required Minimum record capacity.
     * @return `false` when admission or capacity reconciliation fails.
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

/**
 * @brief Requires the exact native-uint64 minimizer surface used by SIMD sinks.
 * @tparam p_minimizer_type Candidate minimizer record.
 */
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
 * @brief Identifies exact record layouts reachable by generated minimizers.
 *
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

/**
 * @brief Maps a minimizer record type to its exact runtime layout identity.
 * @tparam p_minimizer_type Candidate minimizer record.
 */
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
        case minimizer_output_layout::unsupported:
            break;
    }
    return 0u;
}

} // namespace bioseq::minimizers
