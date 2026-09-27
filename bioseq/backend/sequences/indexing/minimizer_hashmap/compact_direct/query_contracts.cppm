/**
 * @file compact_direct/query_contracts.cppm
 * @brief Query records and shared types for compact-index lookup and anchor preparation.
 */

module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:compact_direct.query_contracts;

import std;
import :exact_simd;
import :grouped_soa;
import :prepared_reader;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;

export namespace bioseq::indexing {

/// Query length, k-mer size, and a borrowed view of its precomputed minimizers.
struct fragment_seed_query {
    ::bioseq::minimizers::minimizer_physical_batch_view minimizers{};
    sequence_size_t query_length = 0u;
    std::int32_t kmer_size = 0;
};

/// Query and hash counts recorded by compact-index planning.
/// The checksum follows the CPU prepared key-set byte order so planning and
/// deferred publication identify the same set of hashes.
struct compact_direct_query_set_stats {
    std::uint64_t query_count = 0u;
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t group_key_set_checksum = 0u;
    bool present = false;

    [[nodiscard]] constexpr bool
    operator==(const compact_direct_query_set_stats&) const noexcept = default;
};

/// Initial value used by this project's FNV-style query-set checksum.
inline constexpr std::uint64_t k_compact_direct_query_set_fnv_offset =
    std::uint64_t{1469598103934665603};
/// FNV-1a prime used by the compact-direct query-set checksum.
inline constexpr std::uint64_t k_compact_direct_query_set_fnv_prime =
    std::uint64_t{1099511628211};

/// Fold one little-endian 64-bit word into a query-set checksum.
[[nodiscard]] constexpr std::uint64_t
compact_direct_query_set_fnv_mix_u64(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int byte_index = 0; byte_index < 8; ++byte_index) {
        hash ^= value & std::uint64_t{0xff};
        hash *= k_compact_direct_query_set_fnv_prime;
        value >>= 8u;
    }
    return hash;
}

/// Start a query-set checksum with k-mer size, window size, and occurrence limit.
[[nodiscard]] constexpr std::uint64_t compact_direct_query_set_checksum_prefix(
    std::uint32_t kmer_size, std::uint32_t window_size, std::uint32_t max_occ) noexcept {
    std::uint64_t checksum = k_compact_direct_query_set_fnv_offset;
    checksum = compact_direct_query_set_fnv_mix_u64(checksum, kmer_size);
    checksum = compact_direct_query_set_fnv_mix_u64(checksum, window_size);
    return compact_direct_query_set_fnv_mix_u64(checksum, max_occ);
}

/// Checksum sorted unique 32-bit query hashes together with their seeding settings.
[[nodiscard]] inline std::uint64_t
compact_direct_query_set_checksum(std::span<const std::uint32_t> strictly_increasing_unique_hashes,
                                  std::uint32_t kmer_size,
                                  std::uint32_t window_size,
                                  std::uint32_t max_occ) noexcept {
    std::uint64_t checksum =
        compact_direct_query_set_checksum_prefix(kmer_size, window_size, max_occ);
    for (const std::uint32_t hash : strictly_increasing_unique_hashes) {
        checksum = compact_direct_query_set_fnv_mix_u64(checksum, hash);
    }
    return checksum;
}

/// Choose control-array scans or direct compact-index lookup for a fragment reader.
enum class minimizer_fragment_lookup_route : std::uint8_t {
    grouped_soa_control_scans,
    direct_compact_hash_lookup,
};

/// Optional callback asking the mapping owner to prepare a virtual page range for reading.
/// The owner checks the range against its mapping and translates it to file
/// offsets. Advice has no result and does not change the lookup result.
using compact_direct_selected_page_advice_fn = void (*)(void* context,
                                                        std::uintptr_t page_begin,
                                                        std::uintptr_t page_end) noexcept;

/// Borrowed page-advice callback and its virtual-memory page size.
struct compact_direct_selected_page_adviser {
    void* context = nullptr;
    std::size_t page_bytes = 0u;
    compact_direct_selected_page_advice_fn advise_fn = nullptr;

    [[nodiscard]] constexpr bool empty() const noexcept {
        return context == nullptr && page_bytes == 0u && advise_fn == nullptr;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return context != nullptr && page_bytes != 0u && advise_fn != nullptr;
    }

    void advise(const std::uintptr_t page_begin, const std::uintptr_t page_end) const noexcept {
        if (valid() && page_begin < page_end) {
            advise_fn(context, page_begin, page_end);
        }
    }
};
static_assert(std::is_trivially_copyable_v<compact_direct_selected_page_adviser>);

/// Page-aligned virtual address interval with an exclusive end.
struct compact_direct_selected_page_range {
    std::uintptr_t begin = 0u;
    std::uintptr_t end = 0u;

    [[nodiscard]] constexpr bool
    operator==(const compact_direct_selected_page_range&) const noexcept = default;
};

/// Expand a nonempty byte interval to whole pages, checking address overflow.
[[nodiscard]] inline std::expected<compact_direct_selected_page_range, direct_inverted_index_error>
compact_direct_align_selected_page_range(const std::uintptr_t raw_begin,
                                         const std::size_t raw_bytes,
                                         const std::size_t page_bytes) noexcept {
    if (raw_begin == 0u || raw_bytes == 0u || page_bytes == 0u) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    if (raw_bytes > (std::numeric_limits<std::uintptr_t>::max)() - raw_begin) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    const std::uintptr_t raw_end = raw_begin + raw_bytes;
    const std::uintptr_t page_width = page_bytes;
    const std::uintptr_t aligned_begin = raw_begin - raw_begin % page_width;
    std::uintptr_t aligned_end = raw_end;
    const std::uintptr_t remainder = aligned_end % page_width;
    if (remainder != 0u) {
        const std::uintptr_t growth = page_width - remainder;
        if (growth > (std::numeric_limits<std::uintptr_t>::max)() - aligned_end) {
            return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
        }
        aligned_end += growth;
    }
    if (aligned_begin >= aligned_end) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }
    return compact_direct_selected_page_range{
        .begin = aligned_begin,
        .end = aligned_end,
    };
}

/// Keep one pending page interval and merge overlapping or touching ranges.
/// Flush it before accepting a disjoint range. Advice remains optional and
/// returns no status to the lookup code.
struct compact_direct_selected_page_advice_accumulator {
    compact_direct_selected_page_range pending_range{};
    bool pending = false;

    [[nodiscard]] bool accumulate(const compact_direct_selected_page_adviser adviser,
                                  const compact_direct_selected_page_range range) noexcept {
        if (range.begin >= range.end)
            return false;
        if (pending && range.begin <= pending_range.end && pending_range.begin <= range.end) {
            pending_range.begin = std::min(pending_range.begin, range.begin);
            pending_range.end = std::max(pending_range.end, range.end);
            return true;
        }
        if (pending)
            flush(adviser);
        pending_range = range;
        pending = true;
        return true;
    }

    void flush(const compact_direct_selected_page_adviser adviser) noexcept {
        if (!pending)
            return;
        adviser.advise(pending_range.begin, pending_range.end);
        pending_range = {};
        pending = false;
    }
};
static_assert(std::is_trivially_copyable_v<compact_direct_selected_page_advice_accumulator>);

/// Return pages containing a selected posting span.
/// Spans stored inside descriptors need no advice. Other spans must lie fully
/// within the posting section. This helper checks and aligns addresses without
/// allocating memory or calling the operating system.
[[nodiscard]] inline std::expected<std::optional<compact_direct_selected_page_range>,
                                   direct_inverted_index_error>
compact_direct_select_page_range(const compact_direct_selected_page_adviser adviser,
                                 const compact_hash32_direct_index_view& direct,
                                 const std::span<const std::byte> encoded_bytes) noexcept {
    if (adviser.empty())
        return std::nullopt;
    if (!adviser.valid() || direct.image.empty() || encoded_bytes.empty() ||
        encoded_bytes.data() == nullptr || direct.header.image_bytes != direct.image.size()) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    const auto checked_section_end =
        [&direct](const std::uint64_t offset,
                  const std::uint64_t bytes) -> std::optional<std::uint64_t> {
        if (offset > direct.image.size() ||
            bytes > direct.image.size() - static_cast<std::size_t>(offset)) {
            return std::nullopt;
        }
        return offset + bytes;
    };
    const auto descriptors_end =
        checked_section_end(direct.header.descriptors_offset, direct.header.descriptors_bytes);
    const auto postings_end =
        checked_section_end(direct.header.postings_offset, direct.header.postings_bytes);
    if (!descriptors_end || !postings_end || *descriptors_end > direct.header.postings_offset) {
        return std::unexpected(direct_inverted_index_error::section_layout);
    }

    const std::uintptr_t image_begin = reinterpret_cast<std::uintptr_t>(direct.image.data());
    if (direct.image.size() > (std::numeric_limits<std::uintptr_t>::max)() - image_begin) {
        return std::unexpected(direct_inverted_index_error::arithmetic_overflow);
    }
    const std::uintptr_t image_end = image_begin + direct.image.size();
    const std::uintptr_t raw_begin = reinterpret_cast<std::uintptr_t>(encoded_bytes.data());
    const auto aligned = compact_direct_align_selected_page_range(
        raw_begin, encoded_bytes.size(), adviser.page_bytes);
    if (!aligned)
        return std::unexpected(aligned.error());
    const std::uintptr_t raw_end = raw_begin + encoded_bytes.size();
    if (raw_begin < image_begin || raw_end > image_end) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    const std::uintptr_t descriptors_begin =
        image_begin + static_cast<std::uintptr_t>(direct.header.descriptors_offset);
    const std::uintptr_t descriptors_limit =
        image_begin + static_cast<std::uintptr_t>(*descriptors_end);
    if (raw_begin >= descriptors_begin && raw_end <= descriptors_limit) {
        return std::nullopt;
    }

    const std::uintptr_t postings_begin =
        image_begin + static_cast<std::uintptr_t>(direct.header.postings_offset);
    const std::uintptr_t postings_limit = image_begin + static_cast<std::uintptr_t>(*postings_end);
    if (raw_begin < postings_begin || raw_end > postings_limit) {
        return std::unexpected(direct_inverted_index_error::query_plan);
    }

    return std::optional<compact_direct_selected_page_range>{*aligned};
}

/// Keep a compact-index view with a token pointing to its first byte.
/// For an index inside a larger mapping, use an aliasing shared pointer at the
/// section offset. A token with no ownership control block still needs an
/// external mapping lease kept alive for the reader lifetime.
class owned_compact_hash32_direct_index_view {
  private:
    std::shared_ptr<const std::byte> image_owner_{};
    compact_hash32_direct_index_view view_{};

    owned_compact_hash32_direct_index_view(std::shared_ptr<const std::byte> image_owner,
                                           compact_hash32_direct_index_view view) noexcept
        : image_owner_(std::move(image_owner)), view_(view) {}

    friend class prepared_minimizer_fragment_reader;

  public:
    owned_compact_hash32_direct_index_view() = delete;

    /// Pair an already-validated view with a token pointing to its first image byte.
    [[nodiscard]] static std::expected<owned_compact_hash32_direct_index_view,
                                       minimizer_generation_encoding_error>
    bind(std::shared_ptr<const std::byte> image_owner,
         compact_hash32_direct_index_view view) noexcept {
        if (image_owner == nullptr || view.image.empty() ||
            image_owner.get() != view.image.data() ||
            view.header.image_bytes != view.image.size()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return owned_compact_hash32_direct_index_view{std::move(image_owner), view};
    }

    /// Borrow the bound compact-index view.
    [[nodiscard]] const compact_hash32_direct_index_view& view() const noexcept {
        return view_;
    }

    /// Borrow the image token, which may rely on an external mapping owner.
    [[nodiscard]] const std::shared_ptr<const std::byte>& owner() const noexcept {
        return image_owner_;
    }
};

/// Choose packed query references or separate query and occurrence ordinals.
enum class query_occurrence_route : std::uint8_t {
    packed,
    wide,
};

/// A 32-bit hash with query and occurrence ordinals packed into one word.
struct packed_query_occurrence {
    using hash_type = std::uint32_t;
    std::uint32_t hash = 0u;
    std::uint32_t query_and_ordinal = 0u;
};
static_assert(sizeof(packed_query_occurrence) == 8u);

/// A 64-bit hash with query and occurrence ordinals packed into one word.
struct packed_query_occurrence_h64 {
    using hash_type = std::uint64_t;
    std::uint64_t hash = 0u;
    std::uint32_t query_and_ordinal = 0u;
};
static_assert(sizeof(packed_query_occurrence_h64) == 16u);

/// Keep small occurrence batches on the in-place serial radix sort to avoid task overhead.
inline constexpr std::size_t k_compact_direct_parallel_packed_occurrence_radix_min_count = 1u
                                                                                           << 16u;

/// A 32-bit hash with separate ordinals when one packed word cannot hold both.
struct wide_query_occurrence {
    using hash_type = std::uint32_t;
    std::uint32_t hash = 0u;
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
};
static_assert(sizeof(wide_query_occurrence) == 12u);

/// A 64-bit hash with separate ordinals when one packed word cannot hold both.
struct wide_query_occurrence_h64 {
    using hash_type = std::uint64_t;
    std::uint64_t hash = 0u;
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
};
static_assert(sizeof(wide_query_occurrence_h64) == 16u);

/// Bit widths and limits for packing query and occurrence ordinals into 32 bits.
struct query_occurrence_packing_layout {
    query_occurrence_route route = query_occurrence_route::packed;
    std::uint32_t query_count = 0u;
    std::uint32_t occurrence_count = 0u;
    std::uint32_t query_bits = 0u;
    std::uint32_t ordinal_bits = 32u;
    std::uint64_t ordinal_capacity = std::uint64_t{1} << 32u;
    std::uint32_t ordinal_mask = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] constexpr bool packed() const noexcept {
        return route == query_occurrence_route::packed;
    }

    [[nodiscard]] constexpr bool
    operator==(const query_occurrence_packing_layout&) const noexcept = default;
};

/// Query number and minimizer number within that query.
struct query_occurrence_reference {
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;

    [[nodiscard]] constexpr bool
    operator==(const query_occurrence_reference&) const noexcept = default;
};

/// Check the bit widths, mask, and ordinal capacity derived from the query count.
[[nodiscard]] constexpr bool
valid_query_occurrence_packing_layout(const query_occurrence_packing_layout& layout) noexcept {
    if (layout.query_bits > 32u || layout.ordinal_bits > 32u ||
        layout.query_bits + layout.ordinal_bits != 32u) {
        return false;
    }
    const std::uint32_t expected_query_bits =
        layout.query_count == 0u ? 0u : std::bit_width(std::uint32_t(layout.query_count - 1u));
    const std::uint32_t expected_ordinal_bits = 32u - expected_query_bits;
    const std::uint64_t expected_ordinal_capacity =
        expected_ordinal_bits == 32u ? (std::uint64_t{1} << 32u)
                                     : (std::uint64_t{1} << expected_ordinal_bits);
    const std::uint32_t expected_ordinal_mask =
        expected_ordinal_bits == 32u
            ? std::numeric_limits<std::uint32_t>::max()
            : static_cast<std::uint32_t>((std::uint32_t{1} << expected_ordinal_bits) - 1u);
    return layout.query_bits == expected_query_bits &&
           layout.ordinal_bits == expected_ordinal_bits &&
           layout.ordinal_capacity == expected_ordinal_capacity &&
           layout.ordinal_mask == expected_ordinal_mask;
}

/// Choose packed or wide occurrence records after checking the batch size limits.
/// Packing uses enough bits for the query number and gives the rest to the
/// minimizer number within that query.
[[nodiscard]] constexpr std::expected<query_occurrence_packing_layout,
                                      minimizer_generation_encoding_error>
plan_query_occurrence_packing(std::uint64_t query_count,
                              std::uint64_t occurrence_count,
                              std::uint64_t largest_query_occurrence_count) noexcept {
    constexpr std::uint64_t u32_limit = (std::numeric_limits<std::uint32_t>::max)();
    if (query_count > u32_limit || occurrence_count > u32_limit ||
        largest_query_occurrence_count > u32_limit ||
        largest_query_occurrence_count > occurrence_count ||
        (query_count == 0u && (occurrence_count != 0u || largest_query_occurrence_count != 0u))) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint32_t q32 = static_cast<std::uint32_t>(query_count);
    const std::uint32_t query_bits = q32 == 0u ? 0u : std::bit_width(std::uint32_t(q32 - 1u));
    const std::uint32_t ordinal_bits = 32u - query_bits;
    const std::uint64_t ordinal_capacity =
        ordinal_bits == 32u ? (std::uint64_t{1} << 32u) : (std::uint64_t{1} << ordinal_bits);
    const std::uint32_t ordinal_mask =
        ordinal_bits == 32u ? std::numeric_limits<std::uint32_t>::max()
                            : static_cast<std::uint32_t>((std::uint32_t{1} << ordinal_bits) - 1u);
    return query_occurrence_packing_layout{
        .route = largest_query_occurrence_count <= ordinal_capacity ? query_occurrence_route::packed
                                                                    : query_occurrence_route::wide,
        .query_count = q32,
        .occurrence_count = static_cast<std::uint32_t>(occurrence_count),
        .query_bits = query_bits,
        .ordinal_bits = ordinal_bits,
        .ordinal_capacity = ordinal_capacity,
        .ordinal_mask = ordinal_mask,
    };
}

/// Pack a query number and occurrence number after checking the layout and bounds.
[[nodiscard]] constexpr std::expected<std::uint32_t, minimizer_generation_encoding_error>
encode_packed_query_occurrence(const query_occurrence_packing_layout& layout,
                               std::uint32_t query_ordinal,
                               std::uint32_t occurrence_ordinal) noexcept {
    if (!valid_query_occurrence_packing_layout(layout) || !layout.packed() ||
        query_ordinal >= layout.query_count ||
        static_cast<std::uint64_t>(occurrence_ordinal) >= layout.ordinal_capacity) {
        return std::unexpected(minimizer_generation_encoding_error::partition_id);
    }
    const std::uint64_t encoded =
        (static_cast<std::uint64_t>(query_ordinal) << layout.ordinal_bits) | occurrence_ordinal;
    if (encoded > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return static_cast<std::uint32_t>(encoded);
}

/// Pack an occurrence whose layout and bounds were already checked.
/// Debug builds repeat the checks.
[[nodiscard, gnu::always_inline]] inline std::uint32_t
encode_packed_query_occurrence_trusted(const query_occurrence_packing_layout& layout,
                                       std::uint32_t query_ordinal,
                                       std::uint32_t occurrence_ordinal) noexcept {
#ifndef NDEBUG
    DEBUG_ASSERT(valid_query_occurrence_packing_layout(layout) && layout.packed() &&
                     query_ordinal < layout.query_count &&
                     static_cast<std::uint64_t>(occurrence_ordinal) < layout.ordinal_capacity,
                 "trusted packed query occurrence stays within planned layout");
#endif
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(query_ordinal) << layout.ordinal_bits) | occurrence_ordinal);
}

/// Unpack a query and occurrence number using a caller-validated layout.
[[nodiscard]] constexpr query_occurrence_reference
decode_packed_query_occurrence_trusted(const query_occurrence_packing_layout& layout,
                                       std::uint32_t word) noexcept {
    return query_occurrence_reference{
        .batch_query_ordinal = layout.ordinal_bits == 32u ? 0u : word >> layout.ordinal_bits,
        .query_ordinal = layout.ordinal_bits == 32u ? word : word & layout.ordinal_mask,
    };
}

/// Current occurrence-buffer sizes, capacities, and chosen planning path.
struct query_occurrence_capacity_snapshot {
    query_occurrence_route route = query_occurrence_route::packed;
    std::size_t packed_size = 0u;
    std::size_t packed_capacity = 0u;
    std::size_t wide_size = 0u;
    std::size_t wide_capacity = 0u;
    std::size_t query_plan_size = 0u;
    std::size_t query_plan_capacity = 0u;
    std::size_t query_plan_reserve_request = 0u;
    std::size_t query_group_offset_size = 0u;
    std::size_t query_group_offset_capacity = 0u;
    std::uint64_t aggregate_occurrence_capacity_bytes = 0u;
    std::uint64_t query_membership_filter_capacity_bytes = 0u;
    bool occurrence_radix_invoked = false;
    // This records the selected split. Single-thread mode or a nested task
    // can still run that work inline.
    bool parallel_occurrence_radix_split_selected = false;
    bool bucket_direct_matcher_active = false;
};

/// Counts and write cursors for one query in one fragment block.
struct block_query_anchor_layout {
    std::uint64_t forward_begin = 0u;
    std::uint64_t forward_count = 0u;
    std::uint64_t reverse_begin = 0u;
    std::uint64_t reverse_count = 0u;
    std::uint64_t forward_cursor = 0u;
    std::uint64_t reverse_cursor = 0u;
};
static_assert(sizeof(block_query_anchor_layout) == 48u);

/// Own one query's sorted anchors and remember the borrowed minimizer view they came from.
struct prepared_query_anchors {
    std::unique_ptr<bioseq::chaining::anchor[]> anchors{};
    std::uint64_t anchor_count = 0u;
    ::bioseq::minimizers::minimizer_physical_batch_view minimizers{};
    std::uint64_t minimizer_count = 0u;
    sequence_size_t query_length = 0u;
    std::int32_t kmer_size = 0;
    std::uint64_t sortedness_receipt = 0u;
};

/// Work, timing, and memory counts for prepared anchor batches.
/// Merging adds work and time counters, and takes the maximum buffer
/// capacities because the same scratch buffers are reused between batches.
struct prepared_fragment_anchor_batch_stats {
    std::uint64_t waves = 0u;
    std::uint64_t queries = 0u;
    std::uint64_t query_occurrences = 0u;
    std::uint64_t unique_query_plans = 0u;
    std::uint64_t packed_occurrence_waves = 0u;
    std::uint64_t wide_occurrence_waves = 0u;
    std::uint64_t occurrence_fill_ns = 0u;
    std::uint64_t occurrence_radix_ns = 0u;
    std::uint64_t unique_plan_build_ns = 0u;
    std::uint64_t query_plan_radix_fill_ns = 0u;
    std::uint64_t control_count_pass_ns = 0u;
    std::uint64_t compact_direct_lookup_ns = 0u;
    std::uint64_t rejected_control_validation_ns = 0u;
    std::uint64_t orientation_layout_ns = 0u;
    std::uint64_t direct_emission_ns = 0u;
    std::uint64_t local_sort_ns = 0u;
    std::uint64_t tie_normalization_ns = 0u;
    std::uint64_t control_count_bytes = 0u;
    std::uint64_t rejected_control_validation_bytes = 0u;
    std::uint64_t pass2_control_bytes = 0u;
    std::uint64_t pass2_accepted_prefix_bytes = 0u;
    std::uint64_t pass2_position_bytes = 0u;
    std::uint64_t pass3_control_bytes = 0u;
    std::uint64_t pass3_accepted_prefix_bytes = 0u;
    std::uint64_t pass3_position_bytes = 0u;
    std::uint64_t anchors_constructed = 0u;
    std::uint64_t local_sort_bytes = 0u;
    std::uint64_t tie_normalization_bytes = 0u;
    std::uint64_t exact_final_bytes = 0u;
    std::uint64_t peak_exact_final_bytes = 0u;
    std::uint64_t global_sort_calls_invoked = 0u;
    std::uint64_t global_sort_calls_skipped = 0u;
    std::uint64_t bucket_direct_matcher_view_receipts = 0u;
    std::uint64_t compositional_sortedness_receipts = 0u;
    std::uint64_t final_sortedness_validation_bytes = 0u;
    std::uint64_t candidate_anchor_bytes = 0u;
    std::uint64_t plan_ordinal_bytes = 0u;
    std::uint64_t compaction_bytes = 0u;
    std::uint64_t final_copy_bytes = 0u;
    std::uint64_t peak_occurrence_capacity_bytes = 0u;
    std::uint64_t peak_query_plan_capacity_bytes = 0u;
    std::uint64_t peak_query_group_offset_capacity_bytes = 0u;
    std::uint64_t peak_query_membership_filter_capacity_bytes = 0u;
    std::uint64_t peak_query_radix_capacity_bytes = 0u;
    std::uint64_t peak_occurrence_count_capacity_bytes = 0u;
    std::uint64_t peak_block_layout_capacity_bytes = 0u;
    std::uint64_t peak_block_stats_capacity_bytes = 0u;
    std::uint64_t compact_direct_waves = 0u;
    std::uint64_t compact_direct_unique_query_groups = 0u;
    std::uint64_t compact_direct_unique_lookups = 0u;
    std::uint64_t compact_direct_selected_hashes = 0u;
    std::uint64_t compact_direct_runtime_rejected_hashes = 0u;
    std::uint64_t compact_direct_runtime_rejected_postings = 0u;
    std::uint64_t compact_direct_selected_postings = 0u;
    std::uint64_t compact_direct_selection_shards = 0u;
    std::uint64_t compact_direct_shard_query_layouts = 0u;
    std::uint64_t compact_direct_layout_shards_completed = 0u;
    std::uint64_t compact_direct_emission_shards_completed = 0u;
    std::uint64_t compact_direct_posting_visit_passes = 0u;
    std::uint64_t compact_direct_layout_postings_visited = 0u;
    std::uint64_t compact_direct_emission_postings_visited = 0u;
    std::uint64_t compact_direct_layout_selection_callbacks = 0u;
    std::uint64_t compact_direct_emission_selection_callbacks = 0u;
    std::uint64_t compact_direct_layout_spans_exposed = 0u;
    std::uint64_t compact_direct_emission_spans_exposed = 0u;
    std::uint64_t compact_direct_layout_descriptors_examined = 0u;
    std::uint64_t compact_direct_emission_descriptors_examined = 0u;
    std::uint64_t compact_direct_traversal_rank_checkpoints_examined = 0u;
    std::uint64_t compact_direct_traversal_membership_words_examined = 0u;
    std::uint64_t compact_direct_rejected_posting_reads = 0u;
    std::uint64_t compact_direct_target_control_scan_bytes = 0u;
    std::uint64_t compact_direct_target_control_scan_passes = 0u;
    std::uint64_t compact_direct_fallbacks = 0u;
    std::uint64_t compact_direct_plan_bytes = 0u;
    std::uint64_t compact_direct_plan_reserve_calls = 0u;
    std::uint64_t compact_direct_plan_reserve_requested_elements = 0u;
    std::uint64_t compact_direct_plan_initial_capacity_elements = 0u;
    std::uint64_t compact_direct_plan_fill_reallocations = 0u;
    std::uint64_t compact_direct_plan_compaction_allocations = 0u;
    std::uint64_t compact_direct_plan_storage_allocations = 0u;
    std::uint64_t compact_direct_plan_retained_capacity_elements = 0u;
    std::uint64_t compact_direct_plan_retained_capacity_bytes = 0u;
    std::uint64_t peak_compact_direct_plan_capacity_bytes = 0u;
    std::uint64_t occurrence_allocations = 0u;
    std::uint64_t query_plan_allocations = 0u;
    std::uint64_t query_group_offset_allocations = 0u;
    std::uint64_t query_membership_filter_allocations = 0u;
    std::uint64_t query_radix_allocations = 0u;
    std::uint64_t occurrence_count_allocations = 0u;
    std::uint64_t block_layout_allocations = 0u;
    std::uint64_t block_stats_allocations = 0u;
    bool saturated = false;

    void merge(const prepared_fragment_anchor_batch_stats& other) noexcept {
        bool overflow = saturated || other.saturated;
        const auto add = [&overflow](std::uint64_t& destination, std::uint64_t increment) noexcept {
            if (increment > (std::numeric_limits<std::uint64_t>::max)() - destination) {
                destination = (std::numeric_limits<std::uint64_t>::max)();
                overflow = true;
            } else {
                destination += increment;
            }
        };
        add(waves, other.waves);
        add(queries, other.queries);
        add(query_occurrences, other.query_occurrences);
        add(unique_query_plans, other.unique_query_plans);
        add(packed_occurrence_waves, other.packed_occurrence_waves);
        add(wide_occurrence_waves, other.wide_occurrence_waves);
        add(occurrence_fill_ns, other.occurrence_fill_ns);
        add(occurrence_radix_ns, other.occurrence_radix_ns);
        add(unique_plan_build_ns, other.unique_plan_build_ns);
        add(query_plan_radix_fill_ns, other.query_plan_radix_fill_ns);
        add(control_count_pass_ns, other.control_count_pass_ns);
        add(compact_direct_lookup_ns, other.compact_direct_lookup_ns);
        add(rejected_control_validation_ns, other.rejected_control_validation_ns);
        add(orientation_layout_ns, other.orientation_layout_ns);
        add(direct_emission_ns, other.direct_emission_ns);
        add(local_sort_ns, other.local_sort_ns);
        add(tie_normalization_ns, other.tie_normalization_ns);
        add(control_count_bytes, other.control_count_bytes);
        add(rejected_control_validation_bytes, other.rejected_control_validation_bytes);
        add(pass2_control_bytes, other.pass2_control_bytes);
        add(pass2_accepted_prefix_bytes, other.pass2_accepted_prefix_bytes);
        add(pass2_position_bytes, other.pass2_position_bytes);
        add(pass3_control_bytes, other.pass3_control_bytes);
        add(pass3_accepted_prefix_bytes, other.pass3_accepted_prefix_bytes);
        add(pass3_position_bytes, other.pass3_position_bytes);
        add(anchors_constructed, other.anchors_constructed);
        add(local_sort_bytes, other.local_sort_bytes);
        add(tie_normalization_bytes, other.tie_normalization_bytes);
        add(exact_final_bytes, other.exact_final_bytes);
        add(global_sort_calls_invoked, other.global_sort_calls_invoked);
        add(global_sort_calls_skipped, other.global_sort_calls_skipped);
        add(bucket_direct_matcher_view_receipts, other.bucket_direct_matcher_view_receipts);
        add(compositional_sortedness_receipts, other.compositional_sortedness_receipts);
        add(final_sortedness_validation_bytes, other.final_sortedness_validation_bytes);
        add(candidate_anchor_bytes, other.candidate_anchor_bytes);
        add(plan_ordinal_bytes, other.plan_ordinal_bytes);
        add(compaction_bytes, other.compaction_bytes);
        add(final_copy_bytes, other.final_copy_bytes);
        add(occurrence_allocations, other.occurrence_allocations);
        add(query_plan_allocations, other.query_plan_allocations);
        add(query_group_offset_allocations, other.query_group_offset_allocations);
        add(query_membership_filter_allocations, other.query_membership_filter_allocations);
        add(query_radix_allocations, other.query_radix_allocations);
        add(occurrence_count_allocations, other.occurrence_count_allocations);
        add(block_layout_allocations, other.block_layout_allocations);
        add(block_stats_allocations, other.block_stats_allocations);
        add(compact_direct_waves, other.compact_direct_waves);
        add(compact_direct_unique_query_groups, other.compact_direct_unique_query_groups);
        add(compact_direct_unique_lookups, other.compact_direct_unique_lookups);
        add(compact_direct_selected_hashes, other.compact_direct_selected_hashes);
        add(compact_direct_runtime_rejected_hashes, other.compact_direct_runtime_rejected_hashes);
        add(compact_direct_runtime_rejected_postings,
            other.compact_direct_runtime_rejected_postings);
        add(compact_direct_selected_postings, other.compact_direct_selected_postings);
        add(compact_direct_selection_shards, other.compact_direct_selection_shards);
        add(compact_direct_shard_query_layouts, other.compact_direct_shard_query_layouts);
        add(compact_direct_layout_shards_completed, other.compact_direct_layout_shards_completed);
        add(compact_direct_emission_shards_completed,
            other.compact_direct_emission_shards_completed);
        add(compact_direct_posting_visit_passes, other.compact_direct_posting_visit_passes);
        add(compact_direct_layout_postings_visited, other.compact_direct_layout_postings_visited);
        add(compact_direct_emission_postings_visited,
            other.compact_direct_emission_postings_visited);
        add(compact_direct_layout_selection_callbacks,
            other.compact_direct_layout_selection_callbacks);
        add(compact_direct_emission_selection_callbacks,
            other.compact_direct_emission_selection_callbacks);
        add(compact_direct_layout_spans_exposed, other.compact_direct_layout_spans_exposed);
        add(compact_direct_emission_spans_exposed, other.compact_direct_emission_spans_exposed);
        add(compact_direct_layout_descriptors_examined,
            other.compact_direct_layout_descriptors_examined);
        add(compact_direct_emission_descriptors_examined,
            other.compact_direct_emission_descriptors_examined);
        add(compact_direct_traversal_rank_checkpoints_examined,
            other.compact_direct_traversal_rank_checkpoints_examined);
        add(compact_direct_traversal_membership_words_examined,
            other.compact_direct_traversal_membership_words_examined);
        add(compact_direct_rejected_posting_reads, other.compact_direct_rejected_posting_reads);
        add(compact_direct_target_control_scan_bytes,
            other.compact_direct_target_control_scan_bytes);
        add(compact_direct_target_control_scan_passes,
            other.compact_direct_target_control_scan_passes);
        add(compact_direct_fallbacks, other.compact_direct_fallbacks);
        add(compact_direct_plan_bytes, other.compact_direct_plan_bytes);
        add(compact_direct_plan_reserve_calls, other.compact_direct_plan_reserve_calls);
        add(compact_direct_plan_reserve_requested_elements,
            other.compact_direct_plan_reserve_requested_elements);
        add(compact_direct_plan_initial_capacity_elements,
            other.compact_direct_plan_initial_capacity_elements);
        add(compact_direct_plan_fill_reallocations, other.compact_direct_plan_fill_reallocations);
        add(compact_direct_plan_compaction_allocations,
            other.compact_direct_plan_compaction_allocations);
        add(compact_direct_plan_storage_allocations, other.compact_direct_plan_storage_allocations);
        peak_exact_final_bytes = std::max(peak_exact_final_bytes, other.peak_exact_final_bytes);
        peak_occurrence_capacity_bytes =
            std::max(peak_occurrence_capacity_bytes, other.peak_occurrence_capacity_bytes);
        peak_query_plan_capacity_bytes =
            std::max(peak_query_plan_capacity_bytes, other.peak_query_plan_capacity_bytes);
        peak_query_group_offset_capacity_bytes = std::max(
            peak_query_group_offset_capacity_bytes, other.peak_query_group_offset_capacity_bytes);
        peak_query_membership_filter_capacity_bytes =
            std::max(peak_query_membership_filter_capacity_bytes,
                     other.peak_query_membership_filter_capacity_bytes);
        peak_query_radix_capacity_bytes =
            std::max(peak_query_radix_capacity_bytes, other.peak_query_radix_capacity_bytes);
        peak_occurrence_count_capacity_bytes = std::max(peak_occurrence_count_capacity_bytes,
                                                        other.peak_occurrence_count_capacity_bytes);
        peak_block_layout_capacity_bytes =
            std::max(peak_block_layout_capacity_bytes, other.peak_block_layout_capacity_bytes);
        peak_block_stats_capacity_bytes =
            std::max(peak_block_stats_capacity_bytes, other.peak_block_stats_capacity_bytes);
        peak_compact_direct_plan_capacity_bytes = std::max(
            peak_compact_direct_plan_capacity_bytes, other.peak_compact_direct_plan_capacity_bytes);
        compact_direct_plan_retained_capacity_elements =
            std::max(compact_direct_plan_retained_capacity_elements,
                     other.compact_direct_plan_retained_capacity_elements);
        compact_direct_plan_retained_capacity_bytes =
            std::max(compact_direct_plan_retained_capacity_bytes,
                     other.compact_direct_plan_retained_capacity_bytes);
        saturated = overflow;
    }

    [[nodiscard]] constexpr bool forbidden_buffers_are_zero() const noexcept {
        return candidate_anchor_bytes == 0u && plan_ordinal_bytes == 0u && compaction_bytes == 0u &&
               final_copy_bytes == 0u && global_sort_calls_invoked == 0u;
    }

    [[nodiscard]] constexpr bool
    operator==(const prepared_fragment_anchor_batch_stats&) const noexcept = default;
};

/// Bytes processed and anchors produced by one fragment-block task.
struct fragment_block_wave_stats {
    std::uint64_t pass2_matched_count_bytes = 0u;
    std::uint64_t pass2_accepted_prefix_bytes = 0u;
    std::uint64_t pass2_rejected_prefix_bytes = 0u;
    std::uint64_t pass2_position_bytes = 0u;
    std::uint64_t pass3_matched_count_bytes = 0u;
    std::uint64_t pass3_accepted_prefix_bytes = 0u;
    std::uint64_t pass3_position_bytes = 0u;
    std::uint64_t anchors_constructed = 0u;
    std::uint64_t local_sort_bytes = 0u;
    std::uint64_t tie_normalization_bytes = 0u;
};
static_assert(sizeof(fragment_block_wave_stats) == 80u);

class prepared_minimizer_fragment_reader;

} // namespace bioseq::indexing
