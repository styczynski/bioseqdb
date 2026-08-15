/**
 * @file hash_major_minimizer_index/planner.cppm
 * @brief Canonical query-hash images and admitted selective-read plans.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.indexing.hash_major_minimizer_index:planner;

import :codec;

export namespace bioseq::indexing {

/** @brief Width-tagged borrowed canonical query hashes. */
struct canonical_query_hash_view {
    std::span<const std::uint32_t> values32{};
    std::span<const std::uint64_t> values64{};
    std::uint8_t width_bytes = 0u;
    std::uint8_t storage_width_bytes = 0u;
    std::array<std::byte, 32> storage_profile_digest{};

    [[nodiscard]] std::size_t size() const noexcept {
        return storage_width_bytes == sizeof(std::uint32_t)
            ? values32.size() : values64.size();
    }

    [[nodiscard]] std::uint64_t at(std::size_t ordinal) const noexcept {
        return storage_width_bytes == sizeof(std::uint32_t)
            ? static_cast<std::uint64_t>(values32[ordinal])
            : values64[ordinal];
    }

    [[nodiscard]] bool storage_matches_width() const noexcept {
        return width_bytes == storage_width_bytes
            && ((storage_width_bytes == sizeof(std::uint32_t)
                    && values64.empty())
                || (storage_width_bytes == sizeof(std::uint64_t)
                    && values32.empty()));
    }

    [[nodiscard]] std::size_t lower_bound(
            std::size_t first,
            std::size_t last,
            std::uint64_t hash) const noexcept {
        if (storage_width_bytes == sizeof(std::uint32_t)) {
            if (hash > (std::numeric_limits<std::uint32_t>::max)()) {
                return last;
            }
            const auto begin = values32.begin()
                + static_cast<std::ptrdiff_t>(first);
            const auto end = values32.begin()
                + static_cast<std::ptrdiff_t>(last);
            return static_cast<std::size_t>(
                std::lower_bound(
                    begin, end, static_cast<std::uint32_t>(hash))
                - values32.begin());
        }
        const auto begin = values64.begin()
            + static_cast<std::ptrdiff_t>(first);
        const auto end = values64.begin()
            + static_cast<std::ptrdiff_t>(last);
        return static_cast<std::size_t>(
            std::lower_bound(begin, end, hash) - values64.begin());
    }
};

/// Owning query-hash storage selected by the persisted hash width.
using canonical_query_hash_storage = std::variant<
    std::vector<std::uint32_t>, std::vector<std::uint64_t>>;

/** @brief Move-only canonical query hashes with their storage-profile identity. */
struct canonical_query_hash_image {
    canonical_query_hash_storage values{};
    std::uint8_t width_bytes = 0u;
    std::array<std::byte, 32> storage_profile_digest{};

    canonical_query_hash_image() = default;
    canonical_query_hash_image(const canonical_query_hash_image&) = delete;
    canonical_query_hash_image& operator=(const canonical_query_hash_image&) = delete;
    canonical_query_hash_image(canonical_query_hash_image&&) noexcept = default;
    canonical_query_hash_image& operator=(canonical_query_hash_image&&) noexcept =
        default;

    [[nodiscard]] canonical_query_hash_view view() const noexcept {
        return std::visit(
            [this](const auto& value) {
                using p_value_type = typename std::remove_cvref_t<
                    decltype(value)>::value_type;
                canonical_query_hash_view view{
                    .width_bytes = width_bytes,
                    .storage_width_bytes = sizeof(p_value_type),
                    .storage_profile_digest = storage_profile_digest,
                };
                if constexpr (std::is_same_v<p_value_type, std::uint32_t>) {
                    view.values32 = std::span<const p_value_type>{value};
                } else {
                    view.values64 = std::span<const p_value_type>{value};
                }
                return view;
            },
            values);
    }
};

/** @brief Exact generation identity required before a reader is admitted. */
struct expected_hash_major_minimizer_generation {
    std::uint64_t generation_id = 0u;
    std::uint64_t query_key = 0u;
    std::uint64_t source_epoch = 0u;
    std::uint64_t source_content_crc64 = 0u;
    std::uint64_t target_length = 0u;
    hash_major_minimizer_storage_profile storage_profile{};
};

/** @brief Exact count replacing one saturated compact count. */
struct exact_global_count_exception {
    std::uint32_t query_ordinal = 0;
    std::uint32_t reserved_zero = 0;
    std::uint64_t exact_count = 0;
};
static_assert(sizeof(exact_global_count_exception) == 16u);

/** @brief Authorized posting interval for one query hash. */
struct posting_request {
    std::uint64_t posting_byte_offset = 0;
    std::uint64_t exact_count = 0;
    std::uint32_t query_ordinal = 0;
    std::uint32_t flags = 0;
};
static_assert(sizeof(posting_request) == 24u);

/** @brief Admission, retention, checksum, and delivery counters for one open. */
struct hash_major_minimizer_memory_stats {
    std::uint64_t directory_stage_peak_admitted_bytes = 0u;
    std::uint64_t control_stage_peak_admitted_bytes = 0u;
    std::uint64_t posting_stage_peak_admitted_bytes = 0u;
    std::uint64_t peak_admitted_bytes = 0u;
    std::uint64_t retained_owner_bytes = 0u;
    std::uint64_t query_count = 0u;
    std::uint64_t exact_count_exception_count = 0u;
    std::uint64_t retained_position_count = 0u;
    std::uint64_t directory_entry_count = 0u;
    std::uint64_t selected_block_count = 0u;
    std::uint64_t accepted_hash_count = 0u;
    std::uint64_t directory_stream_crc_receipts = 0u;
    std::uint64_t control_block_crc_receipts = 0u;
    std::uint64_t control_semantic_receipts = 0u;
    std::uint64_t posting_ranges_delivered = 0u;
    std::uint64_t posting_range_crc_receipts = 0u;
    std::uint64_t posting_semantic_receipts = 0u;
    std::uint64_t authorized_posting_bytes = 0u;
    std::uint64_t generic_source_fallback_calls = 0u;
};

} // namespace bioseq::indexing
