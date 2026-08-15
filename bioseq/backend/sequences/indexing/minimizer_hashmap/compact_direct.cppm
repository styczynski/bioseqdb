/**
 * @file minimizer_hashmap/compact_direct.cppm
 * @brief Compact-direct query planning and prepared fragment readers.
 */
module;

#include <libassert/assert.hpp>

export module bioseq.indexing.minimizer_hashmap:compact_direct;

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

/// One query sequence and its precomputed minimizer view for fragment seeding.
struct fragment_seed_query {
    std::span<const minimap_sketch_data> minimizers{};
    sequence_size_t query_length = 0u;
    std::int32_t kmer_size = 0;
};

/// Exact query-set stats produced by the compact-direct occurrence planner.
/// The checksum uses the CPU prepared key-set FNV byte order so deferred
/// publication and direct planning compute the same numeric identity.
struct compact_direct_query_set_stats {
    std::uint64_t query_count = 0u;
    std::uint64_t query_occurrence_count = 0u;
    std::uint64_t unique_hash_count = 0u;
    std::uint64_t group_key_set_checksum = 0u;
    bool present = false;

    [[nodiscard]] constexpr bool
    operator==(const compact_direct_query_set_stats&) const noexcept = default;
};

/// FNV-1a offset basis used by the compact-direct query-set checksum.
inline constexpr std::uint64_t k_compact_direct_query_set_fnv_offset =
    UINT64_C(1469598103934665603);
/// FNV-1a prime used by the compact-direct query-set checksum.
inline constexpr std::uint64_t k_compact_direct_query_set_fnv_prime = UINT64_C(1099511628211);

/// Fold one little-endian 64-bit word into a query-set checksum.
[[nodiscard]] constexpr std::uint64_t
compact_direct_query_set_fnv_mix_u64(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int byte_index = 0; byte_index < 8; ++byte_index) {
        hash ^= value & UINT64_C(0xff);
        hash *= k_compact_direct_query_set_fnv_prime;
        value >>= 8u;
    }
    return hash;
}

/// Start a query-set checksum with its biological shape and occurrence cap.
[[nodiscard]] constexpr std::uint64_t compact_direct_query_set_checksum_prefix(
    std::uint32_t kmer_size, std::uint32_t window_size, std::uint32_t max_occ) noexcept {
    std::uint64_t checksum = k_compact_direct_query_set_fnv_offset;
    checksum = compact_direct_query_set_fnv_mix_u64(checksum, kmer_size);
    checksum = compact_direct_query_set_fnv_mix_u64(checksum, window_size);
    return compact_direct_query_set_fnv_mix_u64(checksum, max_occ);
}

/// Compute the canonical checksum of a sorted unique u32 query-hash set.
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

/// Selects grouped-SoA scans or compact-direct lookup for one fragment reader.
enum class minimizer_fragment_lookup_route : std::uint8_t {
    grouped_soa_control_scans,
    direct_compact_hash_lookup,
};

/// Borrowed, fail-open bridge from the PostgreSQL-free direct reader to the
/// mapping owner which can issue asynchronous page advice.  Absolute virtual
/// page intervals keep the reader independent of file and mmap offsets. The
/// callback validates and translates them against its stable lease context.
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

/// Half-open page-aligned virtual address range selected for advice.
struct compact_direct_selected_page_range {
    std::uintptr_t begin = 0u;
    std::uintptr_t end = 0u;

    [[nodiscard]] constexpr bool
    operator==(const compact_direct_selected_page_range&) const noexcept = default;
};

/// Align a nonempty byte range to complete virtual-memory pages.
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

/// Per-layout-shard, fixed-size coalescer. It merges overlap/touch only and
/// flushes before accepting a genuinely disjoint interval. Callback failure
/// cannot affect control flow because the adviser ABI is noexcept and void.
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

/// Classify and page-align one span exposed by the authenticated compact
/// traversal. Descriptor-resident P32 singletons intentionally return no
/// range. Only spans wholly contained by the authoritative postings section
/// are eligible. This helper performs no allocation and no operating-system
/// call, and keeps all arithmetic checks release-active for focused tests.
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

/// A compact direct view and its exact image token are one contract.  The
/// shared pointer must be an alias whose stored pointer is the first byte of
/// `view.image`. Callers owning a larger mmap create the alias at the direct-
/// section offset.  A token without a control block is non-owning, so its
/// caller must retain the external mmap lease for the reader's full lifetime.
/// Keeping this wrapper intact prevents a validated span from being paired
/// with an unrelated owner.
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

    /// Bind a validated direct view to the owner of its first image byte.
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

    /// Return the bound compact-direct view.
    [[nodiscard]] const compact_hash32_direct_index_view& view() const noexcept {
        return view_;
    }

    /// Return the image lifetime token.
    [[nodiscard]] const std::shared_ptr<const std::byte>& owner() const noexcept {
        return image_owner_;
    }
};

/// Selects the compact or expanded query-occurrence representation.
enum class query_occurrence_route : std::uint8_t {
    packed,
    wide,
};

/// Compact `(hash, query, ordinal)` record with a packed query-and-ordinal word.
struct packed_query_occurrence {
    std::uint32_t hash = 0u;
    std::uint32_t query_and_ordinal = 0u;
};
static_assert(sizeof(packed_query_occurrence) == 8u);

/// The compact-direct split keeps smaller occurrence waves on the measured
/// serial/in-place radix baseline so pool dispatch cannot dominate them.
inline constexpr std::size_t k_compact_direct_parallel_packed_occurrence_radix_min_count = 1u
                                                                                           << 16u;

/// Expanded query occurrence used when the packed ordinal field is too narrow.
struct wide_query_occurrence {
    std::uint32_t hash = 0u;
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;
};
static_assert(sizeof(wide_query_occurrence) == 12u);

/// Bit allocation and capacity of a planned packed occurrence word.
struct query_occurrence_packing_layout {
    query_occurrence_route route = query_occurrence_route::packed;
    std::uint32_t query_count = 0u;
    std::uint32_t occurrence_count = 0u;
    std::uint32_t query_bits = 0u;
    std::uint32_t ordinal_bits = 32u;
    std::uint64_t ordinal_capacity = std::uint64_t{1} << 32u;
    std::uint32_t ordinal_mask = UINT32_MAX;

    [[nodiscard]] constexpr bool packed() const noexcept {
        return route == query_occurrence_route::packed;
    }

    [[nodiscard]] constexpr bool
    operator==(const query_occurrence_packing_layout&) const noexcept = default;
};

/// Decoded query and within-query ordinal from one occurrence record.
struct query_occurrence_reference {
    std::uint32_t batch_query_ordinal = 0u;
    std::uint32_t query_ordinal = 0u;

    [[nodiscard]] constexpr bool
    operator==(const query_occurrence_reference&) const noexcept = default;
};

/// Validate every derived field of a packed occurrence layout.
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
            ? UINT32_MAX
            : static_cast<std::uint32_t>((std::uint32_t{1} << expected_ordinal_bits) - 1u);
    return layout.query_bits == expected_query_bits &&
           layout.ordinal_bits == expected_ordinal_bits &&
           layout.ordinal_capacity == expected_ordinal_capacity &&
           layout.ordinal_mask == expected_ordinal_mask;
}

/// Plan the 32-bit packed query/ordinal word without converting a caller's
/// sizes to 32 bits until every declared query-wave bound is validated.
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
        ordinal_bits == 32u ? UINT32_MAX
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

/// Encode one query occurrence after validating its planned bounds.
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

/// Encode one occurrence under a caller-validated packing layout.
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

/// Decode one word under a caller-validated packing layout.
[[nodiscard]] constexpr query_occurrence_reference
decode_packed_query_occurrence_trusted(const query_occurrence_packing_layout& layout,
                                       std::uint32_t word) noexcept {
    return query_occurrence_reference{
        .batch_query_ordinal = layout.ordinal_bits == 32u ? 0u : word >> layout.ordinal_bits,
        .query_ordinal = layout.ordinal_bits == 32u ? word : word & layout.ordinal_mask,
    };
}

/// Captures live planner sizes, capacities, and selected occurrence route.
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
    // Route selection only: SINGLE mode or a nested task scope may execute the
    // selected split inline without dispatching work to another pool worker.
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

/// Owns the final ordered anchors and immutable identity of one query.
struct prepared_query_anchors {
    std::unique_ptr<bioseq::chaining::anchor[]> anchors{};
    std::uint64_t anchor_count = 0u;
    const minimap_sketch_data* minimizer_data = nullptr;
    std::uint64_t minimizer_count = 0u;
    sequence_size_t query_length = 0u;
    std::int32_t kmer_size = 0;
    std::uint64_t sortedness_receipt = 0u;
};

/// Immutable operation stats for one successful fragment-native query wave.
/// Work and timing fields are additive across waves. Retained-capacity fields are
/// peaks because one query_wave_scratch owner is reused serially.
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

/// Byte and anchor counters produced by one fragment-block wave.
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

/// Reusable, separately admitted query-wave planning state. The scratch owns
/// only transient planner/layout capacity and is never referenced by a
/// returned anchor span.
class query_wave_scratch {
  private:
    static constexpr std::size_t query_membership_filter_word_bits_ = 21u;
    static constexpr std::size_t query_membership_filter_word_count_ =
        std::size_t{1u} << query_membership_filter_word_bits_;

    enum class wave_state : std::uint8_t {
        idle,
        preparing,
        outstanding,
        cleaning,
    };

    struct query_hash_plan {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(query_hash_plan) == 16u);

    // Declared first so all owned capacities die before their permit.
    bioseq::parallelism::memory_permit permit_{};
    std::vector<packed_query_occurrence> packed_occurrences_{};
    std::vector<wide_query_occurrence> wide_occurrences_{};
    std::vector<query_hash_plan> query_plans_{};
    std::vector<std::uint32_t> query_group_offsets_{};
    std::vector<std::uint32_t> query_plan_radix_{};
    std::unique_ptr<std::uint64_t[]> query_membership_filter_{};
    std::size_t query_membership_filter_word_capacity_ = 0u;
    std::unique_ptr<std::atomic<std::uint32_t>[]> occurrence_counts_{};
    std::size_t occurrence_count_capacity_ = 0u;
    std::size_t active_occurrence_count_ = 0u;
    std::vector<block_query_anchor_layout> block_query_layouts_{};
    std::vector<fragment_block_wave_stats> block_stats_{};
    query_occurrence_packing_layout occurrence_layout_{};
    std::size_t query_plan_reserve_request_ = 0u;
    bool occurrence_radix_invoked_ = false;
    bool parallel_occurrence_radix_split_selected_ = false;
    bool bucket_direct_matcher_active_ = false;
    bioseq::parallelism::memory_admission_controller* admission_ = nullptr;
    std::atomic<wave_state> wave_state_{wave_state::idle};

    friend class prepared_minimizer_fragment_reader;

    query_wave_scratch() = default;

    [[nodiscard]] std::uint64_t retained_occurrence_capacity_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        return saturating_add(
            saturating_mul(packed_occurrences_.capacity(), sizeof(packed_query_occurrence)),
            saturating_mul(wide_occurrences_.capacity(), sizeof(wide_query_occurrence)));
    }

    [[nodiscard]] std::uint64_t retained_capacity_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = retained_occurrence_capacity_bytes_();
        bytes =
            saturating_add(bytes, saturating_mul(query_plans_.capacity(), sizeof(query_hash_plan)));
        bytes = saturating_add(
            bytes, saturating_mul(query_group_offsets_.capacity(), sizeof(std::uint32_t)));
        bytes = saturating_add(bytes,
                               saturating_mul(query_plan_radix_.capacity(), sizeof(std::uint32_t)));
        bytes = saturating_add(
            bytes, saturating_mul(query_membership_filter_word_capacity_, sizeof(std::uint64_t)));
        bytes = saturating_add(
            bytes, saturating_mul(occurrence_count_capacity_, sizeof(std::atomic<std::uint32_t>)));
        bytes = saturating_add(
            bytes,
            saturating_mul(block_query_layouts_.capacity(), sizeof(block_query_anchor_layout)));
        bytes = saturating_add(
            bytes, saturating_mul(block_stats_.capacity(), sizeof(fragment_block_wave_stats)));
        return bytes;
    }

    void release_all_capacity_noexcept_() noexcept {
        {
            std::vector<packed_query_occurrence> empty_packed_occurrences;
            std::vector<wide_query_occurrence> empty_wide_occurrences;
            std::vector<query_hash_plan> empty_plans;
            std::vector<std::uint32_t> empty_group_offsets;
            std::vector<std::uint32_t> empty_radix;
            std::vector<block_query_anchor_layout> empty_layouts;
            std::vector<fragment_block_wave_stats> empty_block_stats;
            packed_occurrences_.swap(empty_packed_occurrences);
            wide_occurrences_.swap(empty_wide_occurrences);
            query_plans_.swap(empty_plans);
            query_group_offsets_.swap(empty_group_offsets);
            query_plan_radix_.swap(empty_radix);
            block_query_layouts_.swap(empty_layouts);
            block_stats_.swap(empty_block_stats);
            occurrence_counts_.reset();
            query_membership_filter_.reset();
        }
        query_membership_filter_word_capacity_ = 0u;
        occurrence_count_capacity_ = 0u;
        active_occurrence_count_ = 0u;
        occurrence_layout_ = {};
        query_plan_reserve_request_ = 0u;
        occurrence_radix_invoked_ = false;
        parallel_occurrence_radix_split_selected_ = false;
        bucket_direct_matcher_active_ = false;
        permit_.shrink_to(0u);
    }

    void ensure_query_membership_filter_capacity_() {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (query_membership_filter_word_capacity_ == query_membership_filter_word_count_) {
            std::fill_n(query_membership_filter_.get(),
                        query_membership_filter_word_capacity_,
                        std::uint64_t{0u});
            return;
        }
        const std::uint64_t retained = retained_capacity_bytes_();
        const std::uint64_t allocation =
            saturating_mul(query_membership_filter_word_count_, sizeof(std::uint64_t));
        const std::uint64_t peak = saturating_add(retained, allocation);
        if (allocation == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)() || !permit_.grow_to(peak)) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter admission failed");
        }
        std::unique_ptr<std::uint64_t[]> replacement;
        try {
            replacement = std::make_unique_for_overwrite<std::uint64_t[]>(
                query_membership_filter_word_count_);
        } catch (const std::bad_alloc&) {
            permit_.shrink_to(retained_capacity_bytes_());
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter allocation failed");
        }
        query_membership_filter_ = std::move(replacement);
        query_membership_filter_word_capacity_ = query_membership_filter_word_count_;
        std::fill_n(query_membership_filter_.get(),
                    query_membership_filter_word_capacity_,
                    std::uint64_t{0u});
        const std::uint64_t actual = retained_capacity_bytes_();
        if (actual == (std::numeric_limits<std::uint64_t>::max)() || actual > permit_.bytes()) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                "fragment-native query membership filter reconciliation failed");
        }
        permit_.shrink_to(actual);
    }

    template <class p_value>
    void ensure_vector_capacity_(std::vector<p_value>& values, std::size_t required_capacity) {
        if (required_capacity <= values.capacity())
            return;
        if (required_capacity > values.max_size()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch capacity is out of range");
        }
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        const std::uint64_t retained = retained_capacity_bytes_();
        const std::uint64_t requested = saturating_mul(required_capacity, sizeof(p_value));
        const std::uint64_t headroom =
            saturating_mul(requested, bioseq::parallelism::k_vector_preallocation_headroom_factor);
        const std::uint64_t peak = saturating_add(retained, headroom);
        if (requested == (std::numeric_limits<std::uint64_t>::max)() ||
            headroom == (std::numeric_limits<std::uint64_t>::max)() ||
            peak == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch admission size overflow");
        }
        if (!permit_.grow_to(peak)) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native query scratch admission failed");
        }
        try {
            values.reserve(required_capacity);
        } catch (const std::bad_alloc&) {
            permit_.shrink_to(retained_capacity_bytes_());
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native query scratch allocation failed");
        }
        const std::uint64_t actual = retained_capacity_bytes_();
        if (actual == (std::numeric_limits<std::uint64_t>::max)()) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch reconciliation overflow");
        }
        if (actual > permit_.bytes() && !permit_.grow_to(actual)) {
            release_all_capacity_noexcept_();
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "fragment-native query scratch reconciliation admission "
                "failed");
        }
        permit_.shrink_to(actual);
    }

    void ensure_occurrence_count_capacity_(std::size_t required_capacity) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (required_capacity > occurrence_count_capacity_) {
            const std::uint64_t retained = retained_capacity_bytes_();
            const std::uint64_t allocation =
                saturating_mul(required_capacity, sizeof(std::atomic<std::uint32_t>));
            const std::uint64_t peak = saturating_add(retained, allocation);
            if (allocation == (std::numeric_limits<std::uint64_t>::max)() ||
                peak == (std::numeric_limits<std::uint64_t>::max)() || !permit_.grow_to(peak)) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count admission failed");
            }
            std::unique_ptr<std::atomic<std::uint32_t>[]> replacement;
            try {
                replacement = std::make_unique<std::atomic<std::uint32_t>[]>(required_capacity);
            } catch (const std::bad_alloc&) {
                permit_.shrink_to(retained_capacity_bytes_());
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count allocation failed");
            }
            occurrence_counts_ = std::move(replacement);
            occurrence_count_capacity_ = required_capacity;
            const std::uint64_t actual = retained_capacity_bytes_();
            if (actual == (std::numeric_limits<std::uint64_t>::max)() || actual > permit_.bytes()) {
                release_all_capacity_noexcept_();
                throw persisted_minimizer_lookup_error(
                    "fragment-native occurrence-count reconciliation failed");
            }
            permit_.shrink_to(actual);
        }
        active_occurrence_count_ = required_capacity;
        for (std::size_t ordinal = 0u; ordinal < required_capacity; ++ordinal) {
            occurrence_counts_[ordinal].store(0u, std::memory_order_relaxed);
        }
    }

    void clear_wave_storage_noexcept_() noexcept {
        // The next activation zeros its exact U-sized range once. No reader
        // observes inactive counters, so clearing them here would add a second
        // O(U) atomic-store pass to every wave.
        active_occurrence_count_ = 0u;
        packed_occurrences_.clear();
        wide_occurrences_.clear();
        query_plans_.clear();
        query_group_offsets_.clear();
        query_plan_radix_.clear();
        block_query_layouts_.clear();
        block_stats_.clear();
        occurrence_layout_ = {};
        query_plan_reserve_request_ = 0u;
        occurrence_radix_invoked_ = false;
        parallel_occurrence_radix_split_selected_ = false;
        bucket_direct_matcher_active_ = false;
    }

    void reset_failed_wave_() noexcept {
        clear_wave_storage_noexcept_();
        wave_state_.store(wave_state::idle, std::memory_order_release);
    }

  public:
    query_wave_scratch(query_wave_scratch&& other) noexcept
        : permit_(std::move(other.permit_)),
          packed_occurrences_(std::move(other.packed_occurrences_)),
          wide_occurrences_(std::move(other.wide_occurrences_)),
          query_plans_(std::move(other.query_plans_)),
          query_group_offsets_(std::move(other.query_group_offsets_)),
          query_plan_radix_(std::move(other.query_plan_radix_)),
          query_membership_filter_(std::move(other.query_membership_filter_)),
          query_membership_filter_word_capacity_(other.query_membership_filter_word_capacity_),
          occurrence_counts_(std::move(other.occurrence_counts_)),
          occurrence_count_capacity_(other.occurrence_count_capacity_),
          active_occurrence_count_(other.active_occurrence_count_),
          block_query_layouts_(std::move(other.block_query_layouts_)),
          block_stats_(std::move(other.block_stats_)), occurrence_layout_(other.occurrence_layout_),
          query_plan_reserve_request_(other.query_plan_reserve_request_),
          occurrence_radix_invoked_(other.occurrence_radix_invoked_),
          parallel_occurrence_radix_split_selected_(
              other.parallel_occurrence_radix_split_selected_),
          bucket_direct_matcher_active_(other.bucket_direct_matcher_active_),
          admission_(other.admission_),
          wave_state_(other.wave_state_.exchange(wave_state::idle, std::memory_order_relaxed)) {
        other.occurrence_count_capacity_ = 0u;
        other.active_occurrence_count_ = 0u;
        other.occurrence_layout_ = {};
        other.query_plan_reserve_request_ = 0u;
        other.occurrence_radix_invoked_ = false;
        other.parallel_occurrence_radix_split_selected_ = false;
        other.query_membership_filter_word_capacity_ = 0u;
        other.bucket_direct_matcher_active_ = false;
        other.admission_ = nullptr;
    }

    query_wave_scratch& operator=(query_wave_scratch&&) = delete;
    query_wave_scratch(const query_wave_scratch&) = delete;
    query_wave_scratch& operator=(const query_wave_scratch&) = delete;

    [[nodiscard]] static std::expected<query_wave_scratch, minimizer_generation_encoding_error>
    create(bioseq::parallelism::memory_admission_controller& admission) {
        auto permit = admission.try_reserve(0u);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        query_wave_scratch scratch;
        scratch.permit_ = std::move(*permit);
        scratch.admission_ = &admission;
        return scratch;
    }

    void complete_wave_after_join() noexcept {
        wave_state expected = wave_state::outstanding;
        if (wave_state_.compare_exchange_strong(expected,
                                                wave_state::cleaning,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            clear_wave_storage_noexcept_();
            wave_state_.store(wave_state::idle, std::memory_order_release);
        }
    }

    void release_retained_capacity() {
        wave_state expected = wave_state::idle;
        if (!wave_state_.compare_exchange_strong(expected,
                                                 wave_state::cleaning,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            throw persisted_minimizer_lookup_error("fragment-native query scratch is still in use");
        }
        release_all_capacity_noexcept_();
        wave_state_.store(wave_state::idle, std::memory_order_release);
    }

    [[nodiscard]] bool wave_outstanding() const noexcept {
        return wave_state_.load(std::memory_order_acquire) != wave_state::idle;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] query_occurrence_capacity_snapshot occurrence_capacity_snapshot() const noexcept {
        return query_occurrence_capacity_snapshot{
            .route = occurrence_layout_.route,
            .packed_size = packed_occurrences_.size(),
            .packed_capacity = packed_occurrences_.capacity(),
            .wide_size = wide_occurrences_.size(),
            .wide_capacity = wide_occurrences_.capacity(),
            .query_plan_size = query_plans_.size(),
            .query_plan_capacity = query_plans_.capacity(),
            .query_plan_reserve_request = query_plan_reserve_request_,
            .query_group_offset_size = query_group_offsets_.size(),
            .query_group_offset_capacity = query_group_offsets_.capacity(),
            .aggregate_occurrence_capacity_bytes = retained_occurrence_capacity_bytes_(),
            .query_membership_filter_capacity_bytes = bioseq::common::saturating_mul(
                query_membership_filter_word_capacity_, sizeof(std::uint64_t)),
            .occurrence_radix_invoked = occurrence_radix_invoked_,
            .parallel_occurrence_radix_split_selected = parallel_occurrence_radix_split_selected_,
            .bucket_direct_matcher_active = bucket_direct_matcher_active_,
        };
    }
};

/// Immutable, separately admitted final anchor owner.
class prepared_fragment_anchor_batch {
  private:
    static constexpr std::uint64_t sortedness_receipt_ = UINT64_C(0x46524147414E4348); // "FRAGANCH"

    // Declared first so exact anchors die before their permit.
    bioseq::parallelism::memory_permit permit_{};
    std::unique_ptr<prepared_query_anchors[]> queries_{};
    std::size_t query_count_ = 0u;
    std::uint64_t target_length_ = 0u;
    std::uint64_t max_occ_ = 0u;
    std::uint64_t owned_bytes_ = 0u;
    prepared_fragment_anchor_batch_stats stats_{};
    compact_direct_query_set_stats compact_direct_query_set_stats_{};
    bool ready_ = false;

    friend class prepared_minimizer_fragment_reader;

    explicit prepared_fragment_anchor_batch(bioseq::parallelism::memory_permit&& permit) noexcept
        : permit_(std::move(permit)) {}

  public:
    class query_handle {
      private:
        const prepared_fragment_anchor_batch* batch_ = nullptr;
        std::size_t query_index_ = 0u;

        friend class prepared_fragment_anchor_batch;

        query_handle(const prepared_fragment_anchor_batch& batch, std::size_t query_index) noexcept
            : batch_(&batch), query_index_(query_index) {}

      public:
        query_handle() = default;

        [[nodiscard]] std::uint64_t target_length() const noexcept {
            return batch_ == nullptr ? 0u : batch_->target_length_;
        }

        [[nodiscard]] std::span<const bioseq::chaining::anchor>
        ordered_fragment_seed_anchors(std::span<const minimap_sketch_data> minimizers,
                                      sequence_size_t query_length,
                                      std::int32_t max_occ,
                                      std::int32_t kmer_size) const {
            if (batch_ == nullptr || !batch_->ready_ || query_index_ >= batch_->query_count_ ||
                max_occ < 0 || static_cast<std::uint64_t>(max_occ) != batch_->max_occ_) {
                throw persisted_minimizer_lookup_error(
                    "prepared fragment anchor batch is inconsistent");
            }
            const prepared_query_anchors& query = batch_->queries_[query_index_];
            if (query.minimizer_data != minimizers.data() ||
                query.minimizer_count != minimizers.size() || query.query_length != query_length ||
                query.kmer_size != kmer_size || query.sortedness_receipt != sortedness_receipt_ ||
                query.anchor_count > (std::numeric_limits<std::size_t>::max)() ||
                (query.anchor_count != 0u && query.anchors == nullptr)) {
                throw persisted_minimizer_lookup_error(
                    "prepared fragment anchor query identity is inconsistent");
            }
            return std::span<const bioseq::chaining::anchor>{
                query.anchors.get(), static_cast<std::size_t>(query.anchor_count)};
        }
    };

    prepared_fragment_anchor_batch(prepared_fragment_anchor_batch&& other) noexcept
        : permit_(std::move(other.permit_)), queries_(std::move(other.queries_)),
          query_count_(other.query_count_), target_length_(other.target_length_),
          max_occ_(other.max_occ_), owned_bytes_(other.owned_bytes_), stats_(other.stats_),
          compact_direct_query_set_stats_(other.compact_direct_query_set_stats_),
          ready_(other.ready_) {
        other.query_count_ = 0u;
        other.target_length_ = 0u;
        other.max_occ_ = 0u;
        other.owned_bytes_ = 0u;
        other.stats_ = {};
        other.compact_direct_query_set_stats_ = {};
        other.ready_ = false;
    }

    prepared_fragment_anchor_batch& operator=(prepared_fragment_anchor_batch&&) = delete;
    prepared_fragment_anchor_batch(const prepared_fragment_anchor_batch&) = delete;
    prepared_fragment_anchor_batch& operator=(const prepared_fragment_anchor_batch&) = delete;

    [[nodiscard]] query_handle query_view(std::size_t query_index) const {
        if (!ready_ || query_index >= query_count_) {
            throw persisted_minimizer_lookup_error(
                "prepared fragment anchor query is out of range");
        }
        return query_handle{*this, query_index};
    }

    [[nodiscard]] std::size_t query_count() const noexcept {
        return query_count_;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] const prepared_fragment_anchor_batch_stats& stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] const compact_direct_query_set_stats&
    compact_direct_query_stats() const noexcept {
        return compact_direct_query_set_stats_;
    }
};

/// Fragment-native grouped-SoA reader.  Every physical position segment remains
/// an independently persisted mmap view. Seed collection probes fragment-local
/// controls and packed positions directly. Max-occ reduction is transient
/// query-wave work performed during that same fragment-parallel probe. Reader
/// open never scans or combines fragment controls. No posting list,
/// hash-to-fragment span directory, or target-wide occurrence table is
/// materialized, concatenated, merged, or sorted across fragments. The mapped
/// payload is the exact image emitted by PostgreSQL's complete authenticated
/// READY reread, so opening it validates identity and structure without
/// rereading fragment payload.  The compact-direct-only constructor is a
/// separate backing contract: it retains no grouped-SoA directory or payload and
/// authenticates uniform segment geometry without retaining scan-route
/// address plans.
class prepared_minimizer_fragment_reader {
  public:
    using fragment_seed_query_type = fragment_seed_query;
    using query_wave_scratch_type = query_wave_scratch;
    using prepared_fragment_anchor_batch_type = prepared_fragment_anchor_batch;

  private:
    static constexpr std::uint32_t query_radix_bits_ = 20u;
    static constexpr std::uint32_t query_radix_bucket_count_ = std::uint32_t{1}
                                                               << query_radix_bits_;
    static constexpr std::uint32_t query_radix_bucket_mask_ = query_radix_bucket_count_ - 1u;
    static constexpr std::uint32_t query_membership_filter_word_mask_ =
        (std::uint32_t{1} << 21u) - 1u;
    static constexpr std::size_t compact_direct_selection_shard_target_ = 32u;

    struct compact_direct_selection_shard_range final {
        std::size_t begin = 0u;
        std::size_t end = 0u;
    };

    [[nodiscard]] static constexpr compact_direct_selection_shard_range
    compact_direct_selection_shard_range_(std::size_t selection_count,
                                          std::size_t shard_count,
                                          std::size_t shard_ordinal) noexcept {
        if (shard_count == 0u || shard_ordinal >= shard_count)
            return {};
        const std::size_t selections_per_shard = selection_count / shard_count;
        const std::size_t remainder = selection_count % shard_count;
        const std::size_t begin =
            shard_ordinal * selections_per_shard + std::min(shard_ordinal, remainder);
        return compact_direct_selection_shard_range{
            .begin = begin,
            .end = begin + selections_per_shard + (shard_ordinal < remainder ? 1u : 0u),
        };
    }

    enum class fragment_encoding_dispatch : std::uint8_t {
        checked_dynamic,
        h32_c16_p16,
    };

    enum class backing_kind : std::uint8_t {
        grouped_soa_fragments,
        compact_direct_only,
    };

    struct entry_plan {
        std::uint64_t control_begin = 0u;
        std::uint64_t control_end = 0u;
        std::uint64_t hash_begin = 0u;
        std::uint64_t hash_end = 0u;
        std::uint64_t count_begin = 0u;
        std::uint64_t count_end = 0u;
        std::uint64_t prefix_begin = 0u;
        std::uint64_t prefix_end = 0u;
        std::uint64_t position_begin = 0u;
        std::uint64_t position_end = 0u;
        std::uint64_t control_count = 0u;
        std::uint64_t position_element_count = 0u;
        std::uint64_t reference_begin = 0u;
        std::uint64_t reference_end = 0u;
        std::uint64_t control_scan_bytes = 0u;
        std::uint32_t position_segment = 0u;
        std::uint8_t hash_byte_width = 0u;
        std::uint8_t count_byte_width = 0u;
        std::uint8_t position_byte_width = 0u;
        std::uint8_t offset_byte_width = 0u;
        minimizer_scalar_width hash_width{};
        minimizer_scalar_width count_width{};
        minimizer_scalar_width position_width{};
        minimizer_scalar_width shape_offset_width{};
    };

    struct fragment_block_plan {
        std::uint32_t entry_begin = 0u;
        std::uint32_t entry_end = 0u;
        std::uint32_t first_position_segment = 0u;
        std::uint32_t last_position_segment = 0u;
        std::uint64_t reference_begin = 0u;
        std::uint64_t reference_end = 0u;
    };

    struct direct_open_contract {
        grouped_soa_minimizer_profile profile{};
        std::uint8_t hash_domain_bits = 0u;
    };

    bioseq::parallelism::memory_permit permit_{};
    grouped_soa_directory_view directory_{};
    minimizer_generation_head head_{};
    minimizer_generation_manifest manifest_{};
    grouped_soa_minimizer_profile profile_{};
    std::shared_ptr<const void> mapped_payload_owner_{};
    std::span<const std::byte> mapped_payload_{};
    std::optional<owned_compact_hash32_direct_index_view> compact_direct_{};
    compact_direct_selected_page_adviser selected_page_adviser_{};
    std::unique_ptr<entry_plan[]> entry_plans_{};
    std::unique_ptr<fragment_block_plan[]> block_plans_{};
    std::size_t entry_count_ = 0;
    // Exact retained block_plans_ element count. Direct-only readers keep
    // this zero and expose manifest segment geometry through the public view.
    std::size_t block_count_ = 0;
    std::uint32_t largest_block_entry_count_ = 0u;
    std::uint32_t oversized_segment_block_count_ = 0u;
    std::uint64_t max_occ_ = 0;
    std::uint64_t total_control_hash_scan_bytes_ = 0u;
    std::uint64_t total_control_count_scan_bytes_ = 0u;
    std::uint64_t owned_bytes_ = 0;
    std::uint8_t query_radix_shift_ = 12u;
    std::uint8_t query_membership_filter_shift_ = 11u;
    std::uint16_t query_membership_filter_mask_id_mask_ = 0x7ffu;
    fragment_encoding_dispatch wire_dispatch_ = fragment_encoding_dispatch::checked_dynamic;
    backing_kind backing_kind_ = backing_kind::grouped_soa_fragments;
    bool ready_ = false;

    prepared_minimizer_fragment_reader() = default;

    [[nodiscard]] static constexpr std::uint32_t
    project_query_bucket_(std::uint32_t hash, std::uint8_t shift) noexcept {
        return (hash >> shift) & query_radix_bucket_mask_;
    }

    [[nodiscard]] constexpr std::uint32_t query_bucket_(std::uint32_t hash) const noexcept {
        return project_query_bucket_(hash, query_radix_shift_);
    }

    static_assert(((std::uint32_t{1} << 28u) >> 10u) == (std::uint32_t{1} << 18u));
    static_assert(((std::uint32_t{1} << 29u) >> 10u) == (std::uint32_t{1} << 19u));
    static_assert(((std::uint32_t{3} << 28u) >> 10u) == (std::uint32_t{3} << 18u));
    static_assert((UINT32_MAX >> 12u) == query_radix_bucket_mask_);

    [[nodiscard]] static std::uint64_t stage_now_ns_() noexcept {
        const auto now = bioseq::instrumentation::clock::now_ns();
        return now > 0 ? static_cast<std::uint64_t>(now) : 1u;
    }

    [[nodiscard]] static std::uint64_t elapsed_stage_ns_(std::uint64_t started_ns) noexcept {
        const std::uint64_t finished_ns = stage_now_ns_();
        const std::uint64_t elapsed = finished_ns >= started_ns ? finished_ns - started_ns : 0u;
        return elapsed > 0u ? elapsed : 1u;
    }

    template <minimizer_scalar_width p_width>
    using minimizer_wire_uint_t_ = std::conditional_t<
        p_width == minimizer_scalar_width::u16,
        std::uint16_t,
        std::conditional_t<p_width == minimizer_scalar_width::u32, std::uint32_t, std::uint64_t>>;

    /// Read one scalar after open() has validated the containing component and
    /// extent geometry. The DEBUG_ASSERT documents that stats without
    /// replacing the release-active checks at the plan boundary.
    template <minimizer_scalar_width p_width>
    [[nodiscard, gnu::always_inline]]
    static std::uint64_t read_fixed_width_le_(std::span<const std::byte> validated_extent,
                                              std::size_t offset) noexcept {
        static_assert(p_width == minimizer_scalar_width::u16 ||
                      p_width == minimizer_scalar_width::u32 ||
                      p_width == minimizer_scalar_width::u64);
        using value_type = minimizer_wire_uint_t_<p_width>;
        DEBUG_ASSERT(offset <= validated_extent.size() &&
                         sizeof(value_type) <= validated_extent.size() - offset,
                     "fixed-width scalar read stays in its prevalidated extent");
        return minimizer_generation_wire_detail::read_le<value_type>(validated_extent, offset);
    }

    /// Concrete position decoder shared by both P3 occurrence routes. Route
    /// selection remains outside this cursor, so packed/wide does not become
    /// a template axis for the wire loop.
    class position_range_cursor {
      private:
        std::span<const std::byte> payload_{};
        grouped_soa_decoded_control control_{};
        minimizer_position_segment_bounds core_{};
        minimizer_scalar_width width_ = minimizer_scalar_width::u16;
        std::size_t position_begin_ = 0u;
        std::size_t position_byte_width_ = 0u;
        std::uint64_t ordinal_ = 0u;
        std::optional<std::uint64_t> previous_{};
        std::optional<minimizer_generation_encoding_error> error_{};

        [[nodiscard]] bool accept_local_(std::uint64_t local,
                                         std::uint64_t& packed_global) noexcept {
            const std::uint64_t local_bp = local >> 1u;
            if (core_.end_bp <= core_.begin_bp || local_bp >= core_.end_bp - core_.begin_bp ||
                core_.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
                error_ = minimizer_generation_encoding_error::position_range;
                return false;
            }
            const std::uint64_t global_bp = core_.begin_bp + local_bp;
            if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
                error_ = minimizer_generation_encoding_error::arithmetic_overflow;
                return false;
            }
            packed_global = (global_bp << 1u) | (local & 1u);
            if (previous_ && packed_global <= *previous_) {
                error_ = minimizer_generation_encoding_error::position_range;
                return false;
            }
            previous_ = packed_global;
            ++ordinal_;
            return true;
        }

      public:
        [[nodiscard]] static std::expected<position_range_cursor,
                                           minimizer_generation_encoding_error>
        create(std::span<const std::byte> payload,
               const grouped_soa_decoded_control& control,
               const entry_plan& entry) noexcept {
            if (control.position_end <= control.position_begin ||
                control.position_end > entry.position_element_count) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            DEBUG_ASSERT(is_minimizer_scalar_width(entry.position_width) &&
                             entry.position_byte_width ==
                                 static_cast<std::uint8_t>(entry.position_width) &&
                             entry.position_begin <= entry.position_end &&
                             entry.position_end <= payload.size() &&
                             entry.position_end - entry.position_begin ==
                                 entry.position_element_count * entry.position_byte_width,
                         "position cursor consumes open-validated entry geometry");
            position_range_cursor cursor;
            cursor.payload_ = payload;
            cursor.control_ = control;
            cursor.core_ = minimizer_position_segment_bounds{
                .begin_bp = entry.reference_begin,
                .end_bp = entry.reference_end,
            };
            cursor.width_ = entry.position_width;
            cursor.position_begin_ = static_cast<std::size_t>(entry.position_begin);
            cursor.position_byte_width_ = entry.position_byte_width;
            cursor.ordinal_ = control.position_begin;
            return cursor;
        }

        [[nodiscard, gnu::always_inline]] bool
        next_fixed_p16(std::uint64_t& packed_global) noexcept {
            if (error_ || ordinal_ >= control_.position_end)
                return false;
            DEBUG_ASSERT(width_ == minimizer_scalar_width::u16,
                         "fixed P16 cursor requires a P16 plan");
            const std::size_t offset =
                position_begin_ + static_cast<std::size_t>(ordinal_) * sizeof(std::uint16_t);
            return accept_local_(
                read_fixed_width_le_<minimizer_scalar_width::u16>(payload_, offset), packed_global);
        }

        [[nodiscard, gnu::always_inline]] bool next_dynamic(std::uint64_t& packed_global) noexcept {
            if (error_ || ordinal_ >= control_.position_end)
                return false;
            const std::size_t offset =
                position_begin_ + static_cast<std::size_t>(ordinal_) * position_byte_width_;
            std::uint64_t local = 0u;
            switch (width_) {
            case minimizer_scalar_width::u16:
                local = minimizer_generation_wire_detail::read_le<std::uint16_t>(payload_, offset);
                break;
            case minimizer_scalar_width::u32:
                local = minimizer_generation_wire_detail::read_le<std::uint32_t>(payload_, offset);
                break;
            case minimizer_scalar_width::u64:
                local = minimizer_generation_wire_detail::read_le<std::uint64_t>(payload_, offset);
                break;
            }
            return accept_local_(local, packed_global);
        }

        [[nodiscard]] std::optional<minimizer_generation_encoding_error> error() const noexcept {
            return error_;
        }
    };

    enum class fragment_probe_pass : std::uint8_t {
        control_count,
        rejected_validation,
        orientation,
        emission,
    };

    struct query_occurrence_group final {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t counter_ordinal = 0u;
    };
    static_assert(sizeof(query_occurrence_group) == 16u);

    struct bucket_direct_matcher_view final {
        const std::uint64_t* membership_filter = nullptr;
        const std::uint32_t* radix = nullptr;
        const std::uint32_t* group_offsets = nullptr;
        const packed_query_occurrence* packed_occurrences = nullptr;
        const wide_query_occurrence* wide_occurrences = nullptr;
        std::atomic<std::uint32_t>* occurrence_counts = nullptr;
        std::size_t membership_filter_word_count = 0u;
        std::size_t radix_count = 0u;
        std::size_t group_offset_count = 0u;
        std::size_t packed_occurrence_count = 0u;
        std::size_t wide_occurrence_count = 0u;
        std::size_t occurrence_count = 0u;
        std::size_t counter_count = 0u;
        query_occurrence_packing_layout occurrence_layout{};
    };

    struct fragment_probe_context final {
        fragment_probe_pass pass = fragment_probe_pass::control_count;
        const fragment_block_plan* block = nullptr;
        std::size_t block_ordinal = 0u;
        std::span<const query_wave_scratch::query_hash_plan> plans{};
        std::span<const std::uint32_t> query_plan_radix{};
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        const bucket_direct_matcher_view* bucket_direct_matcher = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::uint32_t max_occ = 0u;
        std::uint64_t* matched_count_bytes = nullptr;
        std::uint64_t* accepted_prefix_bytes = nullptr;
        std::uint64_t* rejected_prefix_bytes = nullptr;
        std::uint64_t* position_bytes = nullptr;
        std::uint64_t* anchors_constructed = nullptr;
    };

    struct alignas(64) compact_direct_layout_pass_context final {
        const prepared_minimizer_fragment_reader* reader = nullptr;
        const compact_hash32_unique_query_plan* plan = nullptr;
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        std::size_t shard_ordinal = 0u;
        std::uint64_t logical_postings_counted = 0u;
        compact_direct_selected_page_advice_accumulator selected_page_advice{};
        std::optional<minimizer_generation_encoding_error> error{};
        std::optional<direct_inverted_index_error> direct_error{};
        std::optional<compact_hash32_selection_span_visit_stats> visit_stats{};
    };

    struct alignas(64) compact_direct_emission_pass_context final {
        const prepared_minimizer_fragment_reader* reader = nullptr;
        const compact_hash32_unique_query_plan* plan = nullptr;
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::size_t shard_ordinal = 0u;
        std::uint64_t posting_bytes_read = 0u;
        std::uint64_t postings_visited = 0u;
        std::uint64_t anchors_constructed = 0u;
        std::optional<minimizer_generation_encoding_error> error{};
        std::optional<direct_inverted_index_error> direct_error{};
        std::optional<compact_hash32_selection_span_visit_stats> visit_stats{};
    };

    inline static constexpr std::array<std::uint64_t, 1u << 11u> query_membership_filter_masks_ =
        []() consteval {
            std::array<std::uint64_t, 1u << 11u> masks{};
            for (std::size_t low = 0u; low < masks.size(); ++low) {
                const std::uint64_t mixed =
                    (static_cast<std::uint64_t>(low) + 1u) * UINT64_C(0x9e3779b97f4a7c15);
                masks[low] = (UINT64_C(1) << ((mixed >> 0u) & 63u)) |
                             (UINT64_C(1) << ((mixed >> 6u) & 63u)) |
                             (UINT64_C(1) << ((mixed >> 12u) & 63u)) |
                             (UINT64_C(1) << ((mixed >> 18u) & 63u)) |
                             (UINT64_C(1) << ((mixed >> 24u) & 63u)) |
                             (UINT64_C(1) << ((mixed >> 30u) & 63u));
            }
            return masks;
        }();

    [[nodiscard, gnu::always_inline]] constexpr std::uint64_t
    query_membership_filter_mask_(std::uint32_t hash) const noexcept {
        // Reader open derives the low-bit mask from the persisted hash domain.
        // Those remaining bits identify a hash inside one filter word; one
        // compile-time LUT preserves the exact six register-blocked positions
        // without repeating their multiplicative diffusion in every probe.
        // A query inserts this exact mask and a probe tests this exact mask,
        // which is the no-false-negative authority; positives still undergo
        // an exact hash comparison inside their packed-occurrence bucket.
        return query_membership_filter_masks_[hash & query_membership_filter_mask_id_mask_];
    }

    [[nodiscard]] static constexpr std::size_t
    project_query_membership_filter_word_(std::uint32_t hash, std::uint8_t shift) noexcept {
        return static_cast<std::size_t>((hash >> shift) & query_membership_filter_word_mask_);
    }

    [[nodiscard, gnu::always_inline]] constexpr std::size_t
    query_membership_filter_word_(std::uint32_t hash) const noexcept {
        return project_query_membership_filter_word_(hash, query_membership_filter_shift_);
    }

    static_assert(((std::uint32_t{1} << 28u) >> 9u) == (std::uint32_t{1} << 19u));
    static_assert(((std::uint32_t{1} << 29u) >> 9u) == (std::uint32_t{1} << 20u));
    static_assert(((std::uint32_t{3} << 28u) >> 9u) == (std::uint32_t{3} << 19u));
    static_assert((UINT32_MAX >> 11u) == query_membership_filter_word_mask_);

    [[nodiscard, gnu::always_inline]] static query_occurrence_reference
    query_occurrence_reference_(const packed_query_occurrence& occurrence,
                                const query_occurrence_packing_layout& layout) noexcept {
        return decode_packed_query_occurrence_trusted(layout, occurrence.query_and_ordinal);
    }

    [[nodiscard, gnu::always_inline]] static query_occurrence_reference
    query_occurrence_reference_(const wide_query_occurrence& occurrence,
                                const query_occurrence_packing_layout&) noexcept {
        return query_occurrence_reference{
            .batch_query_ordinal = occurrence.batch_query_ordinal,
            .query_ordinal = occurrence.query_ordinal,
        };
    }

    /// Select packed or wide storage once for a complete hash plan. The
    /// visitor receives a route-specific span, so its target-position x query-
    /// occurrence loop contains no representation branch.
    template <bool p_oracle, class p_visitor>
    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    with_query_group_occurrences_(const fragment_probe_context& context,
                                  const query_occurrence_group& group,
                                  p_visitor&& visitor) noexcept {
        const std::size_t begin_size = group.occurrence_ref_begin;
        const std::size_t count_size = static_cast<std::size_t>(group.occurrence_ref_count);
        const query_occurrence_packing_layout* occurrence_layout = nullptr;
        const packed_query_occurrence* packed_occurrences = nullptr;
        const wide_query_occurrence* wide_occurrences = nullptr;
        std::size_t packed_occurrence_count = 0u;
        std::size_t wide_occurrence_count = 0u;
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.scratch != nullptr, "oracle occurrence view is prevalidated");
            occurrence_layout = &context.scratch->occurrence_layout_;
            packed_occurrences = context.scratch->packed_occurrences_.data();
            wide_occurrences = context.scratch->wide_occurrences_.data();
            packed_occurrence_count = context.scratch->packed_occurrences_.size();
            wide_occurrence_count = context.scratch->wide_occurrences_.size();
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                         "production occurrence view is prevalidated");
            const bucket_direct_matcher_view& view = *context.bucket_direct_matcher;
            occurrence_layout = &view.occurrence_layout;
            packed_occurrences = view.packed_occurrences;
            wide_occurrences = view.wide_occurrences;
            packed_occurrence_count = view.packed_occurrence_count;
            wide_occurrence_count = view.wide_occurrence_count;
        }
        switch (occurrence_layout->route) {
        case query_occurrence_route::packed:
            DEBUG_ASSERT(begin_size <= packed_occurrence_count &&
                             count_size <= packed_occurrence_count - begin_size,
                         "packed group range is prevalidated");
            return std::forward<p_visitor>(visitor)(
                std::span<const packed_query_occurrence>{packed_occurrences + begin_size,
                                                         count_size},
                *occurrence_layout);
        case query_occurrence_route::wide:
            DEBUG_ASSERT(begin_size <= wide_occurrence_count &&
                             count_size <= wide_occurrence_count - begin_size,
                         "wide group range is prevalidated");
            return std::forward<p_visitor>(visitor)(
                std::span<const wide_query_occurrence>{wide_occurrences + begin_size, count_size},
                *occurrence_layout);
        }
        return minimizer_generation_encoding_error::partition_id;
    }

    template <bool p_oracle>
    [[nodiscard, gnu::always_inline]] static std::atomic<std::uint32_t>&
    occurrence_counter_(const fragment_probe_context& context,
                        std::uint32_t counter_ordinal) noexcept {
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.scratch != nullptr &&
                             counter_ordinal < context.scratch->active_occurrence_count_,
                         "oracle counter ordinal is prevalidated");
            return context.scratch->occurrence_counts_[counter_ordinal];
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr &&
                             counter_ordinal < context.bucket_direct_matcher->counter_count,
                         "production counter ordinal is prevalidated");
            return context.bucket_direct_matcher->occurrence_counts[counter_ordinal];
        }
    }

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    add_expected_anchor_count_(query_occurrence_reference occurrence,
                               std::span<const fragment_seed_query> queries,
                               prepared_fragment_anchor_batch& batch,
                               std::uint32_t global_count) noexcept {
        const std::uint32_t query_ordinal = occurrence.batch_query_ordinal;
        if (query_ordinal >= queries.size() ||
            !add_checked_(batch.queries_[query_ordinal].anchor_count, global_count)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    struct orientation_occurrence_context {
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        std::size_t layout_base = 0u;
        std::uint64_t target_strand_zero = 0u;
        std::uint64_t target_strand_one = 0u;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    accumulate_orientation_(query_occurrence_reference occurrence,
                            const orientation_occurrence_context& context) noexcept {
        if (context.scratch == nullptr ||
            occurrence.batch_query_ordinal >= context.queries.size() ||
            occurrence.query_ordinal >=
                context.queries[occurrence.batch_query_ordinal].minimizers.size()) {
            return minimizer_generation_encoding_error::partition_id;
        }
        const std::size_t layout_ordinal = context.layout_base + occurrence.batch_query_ordinal;
        if (layout_ordinal < context.layout_base ||
            layout_ordinal >= context.scratch->block_query_layouts_.size()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        block_query_anchor_layout& layout = context.scratch->block_query_layouts_[layout_ordinal];
        const std::uint32_t query_strand =
            static_cast<std::uint32_t>(context.queries[occurrence.batch_query_ordinal]
                                           .minimizers[occurrence.query_ordinal]
                                           .pos_strand_encoded() &
                                       1u);
        const std::uint64_t forward =
            query_strand == 0u ? context.target_strand_zero : context.target_strand_one;
        const std::uint64_t reverse =
            query_strand == 0u ? context.target_strand_one : context.target_strand_zero;
        if (!add_checked_(layout.forward_count, forward) ||
            !add_checked_(layout.reverse_count, reverse)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    struct direct_emission_occurrence_context {
        std::span<const fragment_seed_query> queries{};
        const fragment_block_plan* block = nullptr;
        std::size_t block_ordinal = 0u;
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    emit_fragment_anchor_(std::uint64_t packed_target_position,
                          query_occurrence_reference occurrence,
                          const direct_emission_occurrence_context& context) noexcept {
        if (context.block == nullptr || context.scratch == nullptr || context.batch == nullptr ||
            context.queries.empty() || occurrence.batch_query_ordinal >= context.queries.size() ||
            occurrence.query_ordinal >=
                context.queries[occurrence.batch_query_ordinal].minimizers.size()) {
            return minimizer_generation_encoding_error::partition_id;
        }
        const std::size_t query_ordinal = occurrence.batch_query_ordinal;
        const auto anchor = make_fragment_anchor_(
            packed_target_position,
            context.queries[query_ordinal].minimizers[occurrence.query_ordinal],
            context.queries[query_ordinal].query_length,
            static_cast<std::uint8_t>(context.queries[query_ordinal].kmer_size));
        if (!anchor || anchor->ref_position < context.block->reference_begin ||
            anchor->ref_position >= context.block->reference_end) {
            return anchor ? minimizer_generation_encoding_error::position_range : anchor.error();
        }
        if (context.block_ordinal >
            (std::numeric_limits<std::size_t>::max)() / context.queries.size()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        const std::size_t layout_base = context.block_ordinal * context.queries.size();
        const std::size_t layout_ordinal = layout_base + query_ordinal;
        if (layout_ordinal < layout_base ||
            layout_ordinal >= context.scratch->block_query_layouts_.size()) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        block_query_anchor_layout& layout = context.scratch->block_query_layouts_[layout_ordinal];
        std::uint64_t* cursor =
            anchor->is_reverse != 0u ? &layout.reverse_cursor : &layout.forward_cursor;
        const std::uint64_t slice_begin =
            anchor->is_reverse != 0u ? layout.reverse_begin : layout.forward_begin;
        const std::uint64_t slice_count =
            anchor->is_reverse != 0u ? layout.reverse_count : layout.forward_count;
        if (slice_begin > (std::numeric_limits<std::uint64_t>::max)() - slice_count ||
            *cursor < slice_begin || *cursor >= slice_begin + slice_count ||
            *cursor >= context.batch->queries_[query_ordinal].anchor_count ||
            context.batch->queries_[query_ordinal].anchors == nullptr) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        context.batch->queries_[query_ordinal].anchors[static_cast<std::size_t>(*cursor)] = *anchor;
        ++*cursor;
        return std::nullopt;
    }

    struct compact_direct_emission_occurrence_context final {
        std::span<const fragment_seed_query> queries{};
        query_wave_scratch* scratch = nullptr;
        prepared_fragment_anchor_batch* batch = nullptr;
        std::size_t layout_base = 0u;
    };

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    emit_compact_direct_anchor_(
        std::uint64_t packed_target_position,
        query_occurrence_reference occurrence,
        const compact_direct_emission_occurrence_context& context) noexcept {
        if (context.scratch == nullptr || context.batch == nullptr ||
            occurrence.batch_query_ordinal >= context.queries.size() ||
            occurrence.query_ordinal >=
                context.queries[occurrence.batch_query_ordinal].minimizers.size()) {
            return minimizer_generation_encoding_error::partition_id;
        }
        const std::size_t query_ordinal = occurrence.batch_query_ordinal;
        const auto anchor = make_fragment_anchor_(
            packed_target_position,
            context.queries[query_ordinal].minimizers[occurrence.query_ordinal],
            context.queries[query_ordinal].query_length,
            static_cast<std::uint8_t>(context.queries[query_ordinal].kmer_size));
        if (!anchor || anchor->ref_position >= context.batch->target_length_) {
            return anchor ? minimizer_generation_encoding_error::position_range : anchor.error();
        }
        if (context.layout_base > context.scratch->block_query_layouts_.size() ||
            query_ordinal >= context.scratch->block_query_layouts_.size() - context.layout_base ||
            query_ordinal >= context.batch->query_count_) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        block_query_anchor_layout& layout =
            context.scratch->block_query_layouts_[context.layout_base + query_ordinal];
        std::uint64_t* cursor =
            anchor->is_reverse != 0u ? &layout.reverse_cursor : &layout.forward_cursor;
        const std::uint64_t slice_begin =
            anchor->is_reverse != 0u ? layout.reverse_begin : layout.forward_begin;
        const std::uint64_t slice_count =
            anchor->is_reverse != 0u ? layout.reverse_count : layout.forward_count;
        if (slice_begin > (std::numeric_limits<std::uint64_t>::max)() - slice_count ||
            *cursor < slice_begin || *cursor >= slice_begin + slice_count ||
            *cursor >= context.batch->queries_[query_ordinal].anchor_count ||
            context.batch->queries_[query_ordinal].anchors == nullptr) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        context.batch->queries_[query_ordinal].anchors[static_cast<std::size_t>(*cursor)] = *anchor;
        ++*cursor;
        return std::nullopt;
    }

    template <class p_context>
    [[nodiscard]] static bool
    bind_compact_direct_selection_(p_context& context,
                                   const compact_hash32_selection_posting_spans& selected,
                                   query_occurrence_group& group) noexcept {
        if (context.error || context.reader == nullptr || context.plan == nullptr ||
            context.scratch == nullptr ||
            selected.selection_ordinal >= context.plan->selections.size() ||
            selected.global_count == 0u || selected.strand_zero_count > selected.global_count ||
            selected.span_count == 0u || selected.span_count > selected.spans.size()) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        const compact_hash32_unique_selection& selection =
            context.plan->selections[selected.selection_ordinal];
        if (selected.dense_ordinal != selection.dense_ordinal ||
            selected.query_ordinal != selection.query_ordinal ||
            selection.query_ordinal >= context.scratch->query_plans_.size()) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }

        std::uint64_t span_postings = 0u;
        std::uint64_t strand_zero_postings = 0u;
        bool have_previous_strand = false;
        bool previous_strand_one = false;
        for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count; ++span_ordinal) {
            const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];
            if (span.count == 0u || span.posting_width_bytes != context.plan->posting_width_bytes ||
                span.target_length != context.reader->manifest_.target_length ||
                (have_previous_strand && previous_strand_one >= span.target_strand_one) ||
                !add_checked_(span_postings, span.count) ||
                (!span.target_strand_one && !add_checked_(strand_zero_postings, span.count))) {
                context.error = minimizer_generation_encoding_error::partition_id;
                return false;
            }
            previous_strand_one = span.target_strand_one;
            have_previous_strand = true;
        }
        if (span_postings != selected.global_count ||
            strand_zero_postings != selected.strand_zero_count) {
            context.error = minimizer_generation_encoding_error::element_count;
            return false;
        }

        // Compact selections deliberately retain only dense and query
        // ordinals. The query-wave owner remains the single hash authority.
        const query_wave_scratch::query_hash_plan& query_plan =
            context.scratch->query_plans_[selection.query_ordinal];
        if (query_plan.occurrence_ref_count == 0u) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        group = query_occurrence_group{
            .hash = query_plan.hash,
            .occurrence_ref_begin = query_plan.occurrence_ref_begin,
            .occurrence_ref_count = query_plan.occurrence_ref_count,
            .counter_ordinal = selection.query_ordinal,
        };
        return true;
    }

    static void flush_compact_direct_selected_page_advice_(
        compact_direct_layout_pass_context& context) noexcept {
        if (context.reader == nullptr)
            return;
        context.selected_page_advice.flush(context.reader->selected_page_adviser_);
    }

    [[nodiscard]] static bool accumulate_compact_direct_selected_page_advice_(
        compact_direct_layout_pass_context& context,
        const compact_hash32_selection_posting_spans& selected) noexcept {
        if (context.reader == nullptr || !context.reader->compact_direct_) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        const compact_direct_selected_page_adviser adviser = context.reader->selected_page_adviser_;
        if (adviser.empty())
            return true;

        for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count; ++span_ordinal) {
            const auto page_range =
                compact_direct_select_page_range(adviser,
                                                 context.reader->compact_direct_->view(),
                                                 selected.spans[span_ordinal].encoded_bytes);
            if (!page_range) {
                context.direct_error = page_range.error();
                return false;
            }
            if (!*page_range)
                continue;

            if (!context.selected_page_advice.accumulate(adviser, **page_range)) {
                context.direct_error = direct_inverted_index_error::query_plan;
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool visit_compact_direct_layout_selection_(
        compact_direct_layout_pass_context& context,
        const compact_hash32_selection_posting_spans& selected) noexcept {
        query_occurrence_group group{};
        if (!bind_compact_direct_selection_(context, selected, group)) {
            return false;
        }
        if (!accumulate_compact_direct_selected_page_advice_(context, selected)) {
            return false;
        }
        fragment_probe_context occurrence_context{
            .block_ordinal = context.shard_ordinal,
            .queries = context.queries,
            .scratch = context.scratch,
        };
        context.error = accumulate_orientation_for_group_<true>(occurrence_context,
                                                                group,
                                                                selected.strand_zero_count,
                                                                selected.global_count -
                                                                    selected.strand_zero_count);
        if (context.error ||
            !add_checked_(context.logical_postings_counted, selected.global_count)) {
            if (!context.error) {
                context.error = minimizer_generation_encoding_error::arithmetic_overflow;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] static bool visit_compact_direct_emission_selection_(
        compact_direct_emission_pass_context& context,
        const compact_hash32_selection_posting_spans& selected) noexcept {
        if (context.direct_error || context.batch == nullptr) {
            context.error = minimizer_generation_encoding_error::partition_id;
            return false;
        }
        query_occurrence_group group{};
        if (!bind_compact_direct_selection_(context, selected, group)) {
            return false;
        }
        const compact_direct_emission_occurrence_context emission_context{
            .queries = context.queries,
            .scratch = context.scratch,
            .batch = context.batch,
            .layout_base = context.shard_ordinal * context.queries.size(),
        };
        const fragment_probe_context occurrence_context{
            .block_ordinal = context.shard_ordinal,
            .queries = context.queries,
            .scratch = context.scratch,
            .batch = context.batch,
        };

        // Bind the packed/wide occurrence representation once per selected
        // hash. The fixed occurrence span then stays in registers while all of
        // that descriptor's postings are decoded and expanded into anchors.
        context.error = with_query_group_occurrences_<true>(
            occurrence_context,
            group,
            [&context, &selected, &emission_context, &group](
                const auto occurrences,
                const query_occurrence_packing_layout& packing_layout) noexcept
                -> std::optional<minimizer_generation_encoding_error> {
                if (occurrences.size() == 1u) {
                    const query_occurrence_reference occurrence =
                        query_occurrence_reference_(occurrences.front(), packing_layout);
                    if (emission_context.scratch == nullptr || emission_context.batch == nullptr ||
                        occurrence.batch_query_ordinal >= emission_context.queries.size() ||
                        occurrence.query_ordinal >=
                            emission_context.queries[occurrence.batch_query_ordinal]
                                .minimizers.size()) {
                        return minimizer_generation_encoding_error::partition_id;
                    }

                    const std::size_t query_ordinal = occurrence.batch_query_ordinal;
                    if (emission_context.layout_base >
                            emission_context.scratch->block_query_layouts_.size() ||
                        query_ordinal >= emission_context.scratch->block_query_layouts_.size() -
                                             emission_context.layout_base ||
                        query_ordinal >= emission_context.batch->query_count_) {
                        return minimizer_generation_encoding_error::arithmetic_overflow;
                    }
                    const fragment_seed_query& query = emission_context.queries[query_ordinal];
                    const minimap_sketch_data& minimizer =
                        query.minimizers[occurrence.query_ordinal];
                    prepared_query_anchors& output =
                        emission_context.batch->queries_[query_ordinal];
                    block_query_anchor_layout& layout =
                        emission_context.scratch
                            ->block_query_layouts_[emission_context.layout_base + query_ordinal];

                    std::uint64_t selection_postings = 0u;
                    for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                         ++span_ordinal) {
                        const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];

                        // Target strand is invariant for one authoritative
                        // span. Build the query-dependent anchor fields once
                        // at reference position zero; the typed posting loop
                        // below supplies only the changing reference position.
                        const auto prepared_anchor = make_fragment_anchor_(
                            static_cast<std::uint64_t>(span.target_strand_one),
                            minimizer,
                            query.query_length,
                            static_cast<std::uint8_t>(query.kmer_size));
                        if (!prepared_anchor)
                            return prepared_anchor.error();
                        const bioseq::chaining::anchor anchor_template = *prepared_anchor;

                        std::uint64_t* const cursor = anchor_template.is_reverse != 0u
                                                          ? &layout.reverse_cursor
                                                          : &layout.forward_cursor;
                        const std::uint64_t slice_begin = anchor_template.is_reverse != 0u
                                                              ? layout.reverse_begin
                                                              : layout.forward_begin;
                        const std::uint64_t slice_count = anchor_template.is_reverse != 0u
                                                              ? layout.reverse_count
                                                              : layout.forward_count;
                        if (slice_begin >
                            (std::numeric_limits<std::uint64_t>::max)() - slice_count) {
                            return minimizer_generation_encoding_error::arithmetic_overflow;
                        }
                        const std::uint64_t slice_end = slice_begin + slice_count;
                        if (*cursor < slice_begin || *cursor > slice_end ||
                            span.count > slice_end - *cursor || *cursor > output.anchor_count ||
                            span.count > output.anchor_count - *cursor ||
                            output.anchors == nullptr) {
                            return minimizer_generation_encoding_error::arithmetic_overflow;
                        }

                        const std::uint64_t initial_cursor = *cursor;
                        std::uint64_t emitted = 0u;
                        bioseq::chaining::anchor* destination =
                            output.anchors.get() + static_cast<std::size_t>(initial_cursor);
                        bool have_previous_target_bp = false;
                        std::uint64_t previous_target_bp = 0u;
                        std::optional<minimizer_generation_encoding_error> decode_visitor_error{};
                        const auto decoded_positions = span.visit_encoded_positions(
                            [&anchor_template,
                             &decode_visitor_error,
                             &destination,
                             &emission_context,
                             &emitted,
                             &have_previous_target_bp,
                             &previous_target_bp](const std::uint64_t encoded_position) noexcept {
                                const std::uint64_t target_bp = encoded_position >> 1u;
                                if (have_previous_target_bp && target_bp < previous_target_bp) {
                                    decode_visitor_error =
                                        minimizer_generation_encoding_error::position_range;
                                    return false;
                                }
                                previous_target_bp = target_bp;
                                have_previous_target_bp = true;
                                if (target_bp >= emission_context.batch->target_length_) {
                                    decode_visitor_error =
                                        minimizer_generation_encoding_error::position_range;
                                    return false;
                                }

                                *destination = anchor_template;
                                destination->ref_position = target_bp;
                                ++destination;
                                ++emitted;
                                return true;
                            });

                        // The per-anchor path advances after every
                        // successful store. Commit the same partial progress
                        // before propagating a later decode/visitor error.
                        *cursor = initial_cursor + emitted;
                        if (!decoded_positions) {
                            if (decode_visitor_error) {
                                return decode_visitor_error;
                            }
                            context.direct_error = decoded_positions.error();
                            return minimizer_generation_encoding_error::position_range;
                        }
                        if (decode_visitor_error) {
                            return decode_visitor_error;
                        }
                        if (*decoded_positions != span.count || emitted != span.count) {
                            return minimizer_generation_encoding_error::element_count;
                        }
                        if (span.count > (std::numeric_limits<std::uint64_t>::max)() /
                                             group.occurrence_ref_count ||
                            span.count > (std::numeric_limits<std::uint64_t>::max)() /
                                             span.posting_width_bytes ||
                            !add_checked_(context.anchors_constructed,
                                          span.count * group.occurrence_ref_count) ||
                            !add_checked_(context.posting_bytes_read,
                                          span.count * span.posting_width_bytes) ||
                            !add_checked_(context.postings_visited, span.count) ||
                            !add_checked_(selection_postings, span.count)) {
                            return minimizer_generation_encoding_error::arithmetic_overflow;
                        }
                    }
                    if (selection_postings != selected.global_count) {
                        return std::optional<minimizer_generation_encoding_error>{
                            minimizer_generation_encoding_error::element_count};
                    }
                    return std::optional<minimizer_generation_encoding_error>{};
                }

                std::uint64_t selection_postings = 0u;
                for (std::uint8_t span_ordinal = 0u; span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const compact_hash32_posting_span_view& span = selected.spans[span_ordinal];
                    bool have_previous_target_bp = false;
                    std::uint64_t previous_target_bp = 0u;
                    std::optional<minimizer_generation_encoding_error> decode_visitor_error{};
                    const auto decoded_positions = span.visit_encoded_positions(
                        [&decode_visitor_error,
                         &have_previous_target_bp,
                         &previous_target_bp,
                         occurrences,
                         &packing_layout,
                         &emission_context](const std::uint64_t encoded_position) noexcept {
                            const std::uint64_t target_bp = encoded_position >> 1u;
                            if (have_previous_target_bp && target_bp < previous_target_bp) {
                                decode_visitor_error =
                                    minimizer_generation_encoding_error::position_range;
                                return false;
                            }
                            previous_target_bp = target_bp;
                            have_previous_target_bp = true;

                            for (const auto& occurrence : occurrences) {
                                if (const auto error = emit_compact_direct_anchor_(
                                        encoded_position,
                                        query_occurrence_reference_(occurrence, packing_layout),
                                        emission_context)) {
                                    decode_visitor_error = error;
                                    return false;
                                }
                            }
                            return true;
                        });
                    if (!decoded_positions) {
                        if (decode_visitor_error) {
                            return decode_visitor_error;
                        }
                        context.direct_error = decoded_positions.error();
                        return minimizer_generation_encoding_error::position_range;
                    }
                    if (decode_visitor_error)
                        return decode_visitor_error;
                    if (*decoded_positions != span.count) {
                        return minimizer_generation_encoding_error::element_count;
                    }
                    if (span.count > (std::numeric_limits<std::uint64_t>::max)() /
                                         group.occurrence_ref_count ||
                        span.count > (std::numeric_limits<std::uint64_t>::max)() /
                                         span.posting_width_bytes ||
                        !add_checked_(context.anchors_constructed,
                                      span.count * group.occurrence_ref_count) ||
                        !add_checked_(context.posting_bytes_read,
                                      span.count * span.posting_width_bytes) ||
                        !add_checked_(context.postings_visited, span.count) ||
                        !add_checked_(selection_postings, span.count)) {
                        return minimizer_generation_encoding_error::arithmetic_overflow;
                    }
                }
                if (selection_postings != selected.global_count) {
                    return std::optional<minimizer_generation_encoding_error>{
                        minimizer_generation_encoding_error::element_count};
                }
                return std::optional<minimizer_generation_encoding_error>{};
            });
        return !context.error && !context.direct_error;
    }

    /// Reserve one fragment-local occurrence count in a query-wave counter.
    /// A value of max_occ + 1 is the only rejected state. This bounds decoded
    /// candidates for a globally repetitive hash to at most max_occ while
    /// preserving every hit for hashes whose final target-wide count is
    /// accepted. The counter is query state, never persisted minimizer-fragment index state.
    [[nodiscard]] static bool reserve_query_occurrences_(std::atomic<std::uint32_t>& global_count,
                                                         std::uint64_t local_count,
                                                         std::uint32_t max_occ) noexcept {
        const std::uint32_t rejected = max_occ + 1u;
        std::uint32_t current = global_count.load(std::memory_order_relaxed);
        for (;;) {
            if (current > max_occ)
                return false;
            if (local_count == 0u || local_count > max_occ - current) {
                if (global_count.compare_exchange_weak(
                        current, rejected, std::memory_order_relaxed, std::memory_order_relaxed)) {
                    return false;
                }
                continue;
            }
            if (global_count.compare_exchange_weak(current,
                                                   current +
                                                       static_cast<std::uint32_t>(local_count),
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    static void record_error_(std::atomic<std::uint32_t>& destination,
                              minimizer_generation_encoding_error error) noexcept {
        std::uint32_t expected = 0u;
        const std::uint32_t encoded = static_cast<std::uint32_t>(error) + 1u;
        (void)destination.compare_exchange_strong(
            expected, encoded, std::memory_order_relaxed, std::memory_order_relaxed);
    }

    [[nodiscard]] static bool add_atomic_checked_(std::atomic<std::uint64_t>& destination,
                                                  const std::uint64_t increment) noexcept {
        std::uint64_t current = destination.load(std::memory_order_relaxed);
        for (;;) {
            if (increment > (std::numeric_limits<std::uint64_t>::max)() - current) {
                return false;
            }
            if (destination.compare_exchange_weak(current,
                                                  current + increment,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    recorded_error_(const std::atomic<std::uint32_t>& source) noexcept {
        const std::uint32_t encoded = source.load(std::memory_order_relaxed);
        return encoded == 0u ? std::nullopt
                             : std::optional<minimizer_generation_encoding_error>{
                                   static_cast<minimizer_generation_encoding_error>(encoded - 1u)};
    }

    template <class p_body> static void parallel_entries_(std::size_t count, p_body&& body) {
        bioseq::parallelism::parallel_for_pipeline(
            0u,
            count,
            1u,
            bioseq::parallelism::task_parallel_minimums::k_fragment_count,
            bioseq::parallelism::task_parallel_region::fragment_build,
            std::forward<p_body>(body));
    }

    /// Execute exactly the first (most-significant) byte pass of the ordinary
    /// minimap-style u32 radix and return its 256 disjoint bucket extents.
    /// The returned state is constant-size stack data. No occurrence owner or
    /// per-query merge is introduced.  Lower-byte sorts may therefore run in
    /// any order without changing the serial radix's byte-for-byte result.
    [[nodiscard]] static std::array<std::size_t, 257u>
    partition_packed_occurrence_top_byte_(std::span<packed_query_occurrence> occurrences) noexcept {
        struct bucket {
            packed_query_occurrence* begin = nullptr;
            packed_query_occurrence* end = nullptr;
        };

        DEBUG_ASSERT(!occurrences.empty(), "packed occurrence top-byte partition requires payload");
        packed_query_occurrence* const payload_begin = occurrences.data();
        std::array<bucket, 256u> buckets{};
        for (bucket& bucket_slot : buckets) {
            bucket_slot.begin = payload_begin;
            bucket_slot.end = payload_begin;
        }
        for (packed_query_occurrence& occurrence : occurrences) {
            ++buckets[occurrence.hash >> 24u].end;
        }
        for (std::size_t bucket_index = 1u; bucket_index < buckets.size(); ++bucket_index) {
            buckets[bucket_index].end += buckets[bucket_index - 1u].end - payload_begin;
            buckets[bucket_index].begin = buckets[bucket_index - 1u].end;
        }

        for (std::size_t bucket_ordinal = 0u; bucket_ordinal < buckets.size();) {
            bucket& current_bucket = buckets[bucket_ordinal];
            if (current_bucket.begin == current_bucket.end) {
                ++bucket_ordinal;
                continue;
            }
            bucket* destination = &buckets[current_bucket.begin->hash >> 24u];
            if (destination == &current_bucket) {
                ++current_bucket.begin;
                continue;
            }
            packed_query_occurrence displaced = *current_bucket.begin;
            do {
                const packed_query_occurrence placed = displaced;
                displaced = *destination->begin;
                *destination->begin++ = placed;
                destination = &buckets[displaced.hash >> 24u];
            } while (destination != &current_bucket);
            *current_bucket.begin++ = displaced;
        }

        std::array<std::size_t, 257u> bounds{};
        for (std::size_t bucket_index = 0u; bucket_index < buckets.size(); ++bucket_index) {
            bounds[bucket_index + 1u] =
                static_cast<std::size_t>(buckets[bucket_index].end - payload_begin);
        }
        return bounds;
    }

    /// Count-only control scan over one open-validated entry plan. It
    /// deliberately does not load a prefix scalar or position byte. Accepted
    /// plans receive their range validation in passes 2 and 3. All component
    /// offsets, widths, and byte products were validated once by open().
    template <class p_visitor>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_control_counts_planned_(std::size_t entry_ordinal, p_visitor&& visitor) const noexcept {
        DEBUG_ASSERT(entry_ordinal < entry_count_, "block plan entry ordinal is open-validated");
        const grouped_soa_bucket_directory_entry& entry = directory_.entries()[entry_ordinal];
        const entry_plan& plan = entry_plans_[entry_ordinal];
        DEBUG_ASSERT(plan.control_count == entry.control_element_count &&
                         plan.position_element_count == entry.position_element_count &&
                         plan.shape_offset_width == entry.shape_offset_width &&
                         plan.hash_begin <= plan.hash_end && plan.hash_end <= plan.control_end &&
                         plan.count_begin <= plan.count_end && plan.count_end <= plan.control_end &&
                         plan.control_end <= mapped_payload_.size(),
                     "dynamic control scan consumes open-validated geometry");
        const std::size_t count = static_cast<std::size_t>(plan.control_count);

        std::uint64_t previous_hash = 0u;
        std::uint64_t first_hash = 0u;
        std::uint64_t last_hash = 0u;
        for (std::size_t index = 0u; index < count; ++index) {
            const std::uint64_t hash = read_shape_scalar_trusted_(
                mapped_payload_,
                static_cast<std::size_t>(plan.hash_begin) + index * plan.hash_byte_width,
                plan.hash_width);
            const std::uint64_t saturated_count = read_shape_scalar_trusted_(
                mapped_payload_,
                static_cast<std::size_t>(plan.count_begin) + index * plan.count_byte_width,
                plan.count_width);
            if (hash > profile_.max_hash || (index != 0u && hash <= previous_hash) ||
                saturated_count == 0u || saturated_count > profile_.max_occ_cap + 1u) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            if (index == 0u)
                first_hash = hash;
            last_hash = hash;
            std::invoke(visitor, hash, saturated_count);
            previous_hash = hash;
        }
        if (first_hash != entry.lo_hash || last_hash != entry.hi_hash) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        return {};
    }

    template <class p_visitor>
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    visit_controls_(std::span<const std::byte> input,
                    const grouped_soa_bucket_directory_entry& entry,
                    const grouped_soa_minimizer_profile& profile,
                    p_visitor&& visitor) {
        using namespace minimizer_generation_wire_detail;
        if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            entry.control_encoded_bytes != input.size() || entry.control_element_count == 0u ||
            entry.control_element_count > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                           profile.hash_width,
                                                           entry.shape_offset_width,
                                                           profile.count_width);
        if (!layout || layout->encoded_bytes != input.size()) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        const std::size_t count = static_cast<std::size_t>(entry.control_element_count);
        const std::size_t hash_width = static_cast<std::uint8_t>(profile.hash_width);
        const std::size_t offset_width = static_cast<std::uint8_t>(entry.shape_offset_width);
        const std::size_t count_width = static_cast<std::uint8_t>(profile.count_width);
        const std::size_t hash_end = count * hash_width;
        const std::size_t prefix_begin = static_cast<std::size_t>(layout->prefix_offset);
        const std::size_t prefix_end = prefix_begin + (count + 1u) * offset_width;
        const std::size_t count_begin = static_cast<std::size_t>(layout->count_offset);
        const std::size_t count_end = count_begin + count * count_width;
        if (!all_zero(input, hash_end, prefix_begin) || !all_zero(input, prefix_end, count_begin) ||
            !all_zero(input, count_end, input.size())) {
            return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
        }

        std::uint64_t previous_hash = 0u;
        std::uint64_t previous_prefix = 0u;
        std::uint64_t first_hash = 0u;
        std::uint64_t last_hash = 0u;
        for (std::size_t index = 0u; index < count; ++index) {
            const auto hash = read_shape_scalar(input, index * hash_width, profile.hash_width);
            const auto prefix = read_shape_scalar(
                input, prefix_begin + index * offset_width, entry.shape_offset_width);
            const auto next_prefix = read_shape_scalar(
                input, prefix_begin + (index + 1u) * offset_width, entry.shape_offset_width);
            const auto saturated_count =
                read_shape_scalar(input, count_begin + index * count_width, profile.count_width);
            if (!hash || !prefix || !next_prefix || !saturated_count || *hash > profile.max_hash ||
                (index != 0u && *hash <= previous_hash) ||
                (index == 0u ? *prefix != 0u : *prefix <= previous_prefix) ||
                *next_prefix <= *prefix || *next_prefix > entry.position_element_count) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            const std::uint64_t delta = *next_prefix - *prefix;
            if (*saturated_count != std::min(delta, profile.max_occ_cap + 1u)) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            if (index == 0u)
                first_hash = *hash;
            last_hash = *hash;
            std::invoke(visitor,
                        grouped_soa_decoded_control{
                            .hash = *hash,
                            .position_begin = *prefix,
                            .position_end = *next_prefix,
                            .saturated_count = *saturated_count,
                        });
            previous_hash = *hash;
            previous_prefix = *prefix;
        }
        const auto terminal_prefix =
            read_shape_scalar(input, prefix_begin + count * offset_width, entry.shape_offset_width);
        if (!terminal_prefix || *terminal_prefix != entry.position_element_count ||
            first_hash != entry.lo_hash || last_hash != entry.hi_hash) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        return {};
    }

    [[nodiscard, gnu::always_inline]]
    static std::uint64_t read_shape_scalar_trusted_(std::span<const std::byte> input,
                                                    std::size_t offset,
                                                    minimizer_scalar_width width) noexcept {
        using namespace minimizer_generation_wire_detail;
        switch (width) {
        case minimizer_scalar_width::u16:
            return read_le<std::uint16_t>(input, offset);
        case minimizer_scalar_width::u32:
            return read_le<std::uint32_t>(input, offset);
        case minimizer_scalar_width::u64:
            return read_le<std::uint64_t>(input, offset);
        }
        std::unreachable();
    }

    template <class p_visitor>
    [[nodiscard]] static std::expected<void, minimizer_generation_encoding_error>
    visit_position_range_(std::span<const std::byte> input,
                          const grouped_soa_decoded_control& control,
                          const grouped_soa_bucket_directory_entry& entry,
                          const minimizer_position_segment_bounds& core,
                          const grouped_soa_minimizer_profile& profile,
                          p_visitor&& visitor) noexcept {
        using namespace minimizer_generation_wire_detail;
        if (control.position_end <= control.position_begin ||
            control.position_end > entry.position_element_count) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        const std::size_t width = static_cast<std::uint8_t>(profile.position_width);
        if (width == 0u ||
            entry.position_element_count > (std::numeric_limits<std::size_t>::max)() / width ||
            input.size() != static_cast<std::size_t>(entry.position_element_count) * width) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        std::optional<std::uint64_t> previous;
        for (std::uint64_t ordinal = control.position_begin; ordinal < control.position_end;
             ++ordinal) {
            const std::uint64_t local = read_shape_scalar_trusted_(
                input, static_cast<std::size_t>(ordinal) * width, profile.position_width);
            const std::uint64_t local_bp = local >> 1u;
            if (local_bp >= core.end_bp - core.begin_bp ||
                core.begin_bp > (std::numeric_limits<std::uint64_t>::max)() - local_bp) {
                return std::unexpected(minimizer_generation_encoding_error::position_range);
            }
            const std::uint64_t global_bp = core.begin_bp + local_bp;
            if (global_bp > ((std::numeric_limits<std::uint64_t>::max)() >> 1u)) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t encoded = (global_bp << 1u) | (local & 1u);
            if (previous && encoded <= *previous) {
                return std::unexpected(minimizer_generation_encoding_error::position_range);
            }
            std::invoke(visitor, encoded);
            previous = encoded;
        }
        return {};
    }

    [[nodiscard]] static bool add_checked_(std::uint64_t& destination,
                                           std::uint64_t value) noexcept {
        if (destination > (std::numeric_limits<std::uint64_t>::max)() - value) {
            return false;
        }
        destination += value;
        return true;
    }

    /// Delegate both ordinary and fragment-native ordering to the same
    /// duplicate-linear authority and return exact payload-work receipts for
    /// independently timed primary and tie stages.
    [[nodiscard]] static bioseq::chaining::canonical_anchor_sort_stats
    sort_anchor_slice_primary_(bioseq::chaining::anchor* anchors,
                               std::size_t anchor_count,
                               std::uint64_t authenticated_target_length) noexcept {
        return bioseq::chaining::sort_canonical_anchor_primary_bounded(
            std::span<bioseq::chaining::anchor>{anchors, anchor_count},
            authenticated_target_length);
    }

    [[nodiscard]] static bioseq::chaining::canonical_anchor_sort_stats
    normalize_anchor_slice_ties_(bioseq::chaining::anchor* anchors,
                                 std::size_t anchor_count) noexcept {
        return bioseq::chaining::normalize_canonical_anchor_ties(
            std::span<bioseq::chaining::anchor>{anchors, anchor_count});
    }

    [[nodiscard]] static std::expected<bioseq::chaining::anchor,
                                       minimizer_generation_encoding_error>
    make_fragment_anchor_(std::uint64_t packed_target,
                          const minimap_sketch_data& minimizer,
                          sequence_size_t query_length,
                          std::uint8_t query_span) noexcept {
        const std::uint64_t ref_position = packed_target >> 1u;
        const std::uint32_t target_strand = static_cast<std::uint32_t>(packed_target & 1u);
        const std::uint64_t encoded = minimizer.pos_strand_encoded();
        const std::uint32_t query_strand = static_cast<std::uint32_t>(encoded & 1u);
        const std::uint64_t forward_last = encoded >> 1u;
        if (ref_position >= (std::uint64_t{1} << 63u) || query_span == 0u ||
            query_length < query_span || forward_last >= query_length ||
            forward_last + 1u < query_span) {
            return std::unexpected(minimizer_generation_encoding_error::position_range);
        }
        const std::uint64_t kmer_start = forward_last + 1u - query_span;
        const std::uint64_t reverse_last = query_length - 1u - kmer_start;
        const std::uint32_t reverse = target_strand ^ query_strand;
        return bioseq::chaining::anchor{
            .ref_position = ref_position,
            .query_position = reverse != 0u ? reverse_last : forward_last,
            .is_reverse = reverse,
            .query_span = query_span,
            .flags = 0u,
            .segment_id = 0u,
        };
    }

    template <bool p_oracle, class p_visitor>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_matched_control_counts_(const fragment_block_plan& block,
                                  fragment_probe_context& context,
                                  p_visitor&& visitor) const noexcept {
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size() ==
                             static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                         "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                         "production matcher view is prevalidated");
        }
        DEBUG_ASSERT(block.entry_begin < block.entry_end && block.entry_end <= entry_count_,
                     "dynamic count scan consumes an open-validated block");
        for (std::size_t ordinal = block.entry_begin; ordinal < block.entry_end; ++ordinal) {
            std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
            std::uint32_t plan_cursor = 0u;
            std::uint32_t plan_end = 0u;
            std::optional<minimizer_generation_encoding_error> callback_error;
            const auto decoded = visit_control_counts_planned_(
                ordinal,
                [this,
                 &callback_error,
                 &active_bucket,
                 &plan_cursor,
                 &plan_end,
                 &context,
                 &visitor](std::uint64_t hash, std::uint64_t local_count) noexcept {
                    if (callback_error)
                        return;
                    const std::uint32_t hash32 = static_cast<std::uint32_t>(hash);
                    const auto matched_group = match_query_group_<p_oracle>(
                        hash32, context, active_bucket, plan_cursor, plan_end);
                    if (!matched_group)
                        return;
                    callback_error = std::invoke(visitor, *matched_group, local_count);
                });
            if (!decoded)
                return std::unexpected(decoded.error());
            if (callback_error) {
                return std::unexpected(*callback_error);
            }
        }
        return {};
    }

    template <bool p_oracle, class p_visitor>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    visit_matched_controls_(const fragment_block_plan& block,
                            fragment_probe_context& context,
                            p_visitor&& visitor,
                            std::uint64_t* accepted_prefix_bytes = nullptr,
                            std::uint64_t* rejected_prefix_bytes = nullptr,
                            bool validate_rejected_prefixes = true) const noexcept {
        using namespace minimizer_generation_wire_detail;
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size() ==
                             static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                         "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                         "production matcher view is prevalidated");
        }
        DEBUG_ASSERT(block.entry_begin < block.entry_end && block.entry_end <= entry_count_,
                     "dynamic matched scan consumes an open-validated block");
        for (std::size_t ordinal = block.entry_begin; ordinal < block.entry_end; ++ordinal) {
            const entry_plan& selected_entry = entry_plans_[ordinal];
            const std::size_t offset_width = selected_entry.offset_byte_width;
            std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
            std::uint32_t plan_cursor = 0u;
            std::uint32_t plan_end = 0u;
            std::optional<minimizer_generation_encoding_error> callback_error;
            std::optional<std::uint64_t> previous_accepted_position_end;
            std::size_t control_ordinal = 0u;
            const auto decoded = visit_control_counts_planned_(
                ordinal,
                [this,
                 &control_ordinal,
                 &callback_error,
                 &active_bucket,
                 &plan_cursor,
                 &plan_end,
                 &context,
                 offset_width,
                 &selected_entry,
                 &previous_accepted_position_end,
                 &visitor,
                 accepted_prefix_bytes,
                 rejected_prefix_bytes,
                 validate_rejected_prefixes,
                 ordinal](std::uint64_t hash, std::uint64_t saturated_count) noexcept {
                    const std::size_t current_ordinal = control_ordinal++;
                    if (callback_error)
                        return;
                    const std::uint32_t hash32 = static_cast<std::uint32_t>(hash);
                    const auto matched_group = match_query_group_<p_oracle>(
                        hash32, context, active_bucket, plan_cursor, plan_end);
                    if (!matched_group)
                        return;
                    const std::uint32_t global_count =
                        occurrence_counter_<p_oracle>(context, matched_group->counter_ordinal)
                            .load(std::memory_order_relaxed);
                    if (global_count == 0u) {
                        callback_error = minimizer_generation_encoding_error::partition_id;
                        return;
                    }
                    // Pass 3 suppresses a rejected control before either offset
                    // endpoint is read. Rejected endpoints were authenticated
                    // exactly once by Pass 2 (or the rejected-only rescan).
                    if (global_count > context.max_occ && !validate_rejected_prefixes) {
                        return;
                    }
                    std::uint64_t* const prefix_stats = global_count > context.max_occ
                                                            ? rejected_prefix_bytes
                                                            : accepted_prefix_bytes;
                    if (prefix_stats != nullptr &&
                        !add_checked_(*prefix_stats,
                                      2u * static_cast<std::uint64_t>(offset_width))) {
                        callback_error = minimizer_generation_encoding_error::arithmetic_overflow;
                        return;
                    }
                    const std::size_t prefix_offset =
                        static_cast<std::size_t>(selected_entry.prefix_begin) +
                        current_ordinal * offset_width;
                    const std::uint64_t position_begin = read_shape_scalar_trusted_(
                        mapped_payload_, prefix_offset, selected_entry.shape_offset_width);
                    const std::uint64_t position_end =
                        read_shape_scalar_trusted_(mapped_payload_,
                                                   prefix_offset + offset_width,
                                                   selected_entry.shape_offset_width);
                    if (position_end <= position_begin ||
                        position_end > selected_entry.position_element_count ||
                        (current_ordinal == 0u && position_begin != 0u) ||
                        (current_ordinal + 1u == selected_entry.control_count &&
                         position_end != selected_entry.position_element_count) ||
                        (previous_accepted_position_end &&
                         position_begin < *previous_accepted_position_end)) {
                        callback_error = minimizer_generation_encoding_error::control_range;
                        return;
                    }
                    previous_accepted_position_end = position_end;
                    const std::uint64_t delta = position_end - position_begin;
                    if (saturated_count != std::min(delta, profile_.max_occ_cap + 1u)) {
                        callback_error = minimizer_generation_encoding_error::control_range;
                        return;
                    }
                    // A globally rejected query hash still names a selected
                    // control. Authenticate its prefix geometry and saturated
                    // count before suppressing the position walk; otherwise a
                    // corrupt heavy control could hide behind max-occ pruning.
                    if (global_count > context.max_occ) {
                        return;
                    }
                    callback_error = std::invoke(visitor,
                                                 ordinal,
                                                 *matched_group,
                                                 grouped_soa_decoded_control{
                                                     .hash = hash,
                                                     .position_begin = position_begin,
                                                     .position_end = position_end,
                                                     .saturated_count = saturated_count,
                                                 });
                });
            if (!decoded)
                return std::unexpected(decoded.error());
            if (callback_error) {
                return std::unexpected(*callback_error);
            }
        }
        return {};
    }

    template <bool p_oracle>
    [[nodiscard, gnu::always_inline]]
    std::optional<query_occurrence_group>
    match_query_group_(std::uint32_t hash,
                       const fragment_probe_context& context,
                       std::uint32_t& active_bucket,
                       std::uint32_t& plan_cursor,
                       std::uint32_t& plan_end) const noexcept {
        const std::uint32_t bucket = query_bucket_(hash);
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.scratch != nullptr &&
                             context.query_plan_radix.size() ==
                                 static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                         "oracle matcher owners are prevalidated");
            if (bucket != active_bucket) {
                active_bucket = bucket;
                plan_cursor = context.query_plan_radix[bucket];
                plan_end = context.query_plan_radix[bucket + 1u];
            }
            DEBUG_ASSERT(plan_cursor <= plan_end && plan_end <= context.plans.size(),
                         "oracle matcher bucket range is prevalidated");
            while (plan_cursor < plan_end && context.plans[plan_cursor].hash < hash) {
                ++plan_cursor;
            }
            if (plan_cursor == plan_end || context.plans[plan_cursor].hash != hash) {
                return std::nullopt;
            }
            const std::uint32_t matched_plan = plan_cursor++;
            const query_wave_scratch::query_hash_plan& plan = context.plans[matched_plan];
            return query_occurrence_group{
                .hash = plan.hash,
                .occurrence_ref_begin = plan.occurrence_ref_begin,
                .occurrence_ref_count = plan.occurrence_ref_count,
                .counter_ordinal = matched_plan,
            };
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                         "production matcher view is prevalidated");
            const bucket_direct_matcher_view& view = *context.bucket_direct_matcher;
            const std::size_t filter_word = query_membership_filter_word_(hash);
            DEBUG_ASSERT(filter_word < view.membership_filter_word_count,
                         "production filter projection is prevalidated");
            const std::uint64_t required_mask = query_membership_filter_mask_(hash);
            const std::uint64_t present_mask = view.membership_filter[filter_word];
            if ((present_mask & required_mask) != required_mask) {
                return std::nullopt;
            }

            DEBUG_ASSERT(static_cast<std::size_t>(bucket) + 1u < view.radix_count,
                         "production radix projection is prevalidated");
            const std::uint32_t group_begin = view.radix[bucket];
            const std::uint32_t group_end = view.radix[bucket + 1u];
            DEBUG_ASSERT(group_begin <= group_end &&
                             static_cast<std::size_t>(group_end) + 1u <= view.group_offset_count,
                         "production group directory is prevalidated");
            const auto exact_group =
                [&view, hash, group_begin, group_end](const auto* occurrences,
                                                      std::size_t occurrence_count) noexcept
                -> std::optional<query_occurrence_group> {
                const std::uint32_t* const first = view.group_offsets + group_begin;
                const std::uint32_t* const last = view.group_offsets + group_end;
                const auto found = std::lower_bound(
                    first,
                    last,
                    hash,
                    [occurrences, occurrence_count](std::uint32_t occurrence_ordinal,
                                                    std::uint32_t query_hash) noexcept {
                        DEBUG_ASSERT(occurrence_ordinal < occurrence_count,
                                     "production group offset is prevalidated");
                        return occurrences[occurrence_ordinal].hash < query_hash;
                    });
                if (found == last || occurrences[*found].hash != hash) {
                    return std::nullopt;
                }
                const std::uint32_t occurrence_begin = *found;
                const std::uint32_t occurrence_end = *(found + 1u);
                DEBUG_ASSERT(occurrence_end > occurrence_begin &&
                                 occurrence_end <= occurrence_count,
                             "production group extent is prevalidated");
                return query_occurrence_group{
                    .hash = hash,
                    .occurrence_ref_begin = occurrence_begin,
                    .occurrence_ref_count = occurrence_end - occurrence_begin,
                    .counter_ordinal = static_cast<std::uint32_t>(found - view.group_offsets),
                };
            };
            switch (view.occurrence_layout.route) {
            case query_occurrence_route::packed:
                return exact_group(view.packed_occurrences, view.packed_occurrence_count);
            case query_occurrence_route::wide:
                return exact_group(view.wide_occurrences, view.wide_occurrence_count);
            }
            return std::nullopt;
        }
    }

    template <bool p_oracle>
    [[nodiscard]] static std::optional<minimizer_generation_encoding_error>
    accumulate_orientation_for_group_(const fragment_probe_context& context,
                                      const query_occurrence_group& group,
                                      std::uint64_t target_strand_zero,
                                      std::uint64_t target_strand_one) noexcept {
        DEBUG_ASSERT(context.scratch != nullptr && !context.queries.empty() &&
                         context.block_ordinal <=
                             (std::numeric_limits<std::size_t>::max)() / context.queries.size(),
                     "orientation occurrence context is prevalidated");
        const orientation_occurrence_context occurrence_context{
            .queries = context.queries,
            .scratch = context.scratch,
            .layout_base = context.block_ordinal * context.queries.size(),
            .target_strand_zero = target_strand_zero,
            .target_strand_one = target_strand_one,
        };
        return with_query_group_occurrences_<p_oracle>(
            context,
            group,
            [&occurrence_context](const auto occurrences,
                                  const query_occurrence_packing_layout& packing_layout) noexcept
                -> std::optional<minimizer_generation_encoding_error> {
                for (const auto& occurrence : occurrences) {
                    if (const auto error = accumulate_orientation_(
                            query_occurrence_reference_(occurrence, packing_layout),
                            occurrence_context)) {
                        return error;
                    }
                }
                return std::nullopt;
            });
    }

    template <bool p_oracle>
    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    process_fixed_orientation_control_(fragment_probe_context& context,
                                       const entry_plan& entry,
                                       const query_occurrence_group& group,
                                       const grouped_soa_decoded_control& control) const noexcept {
        DEBUG_ASSERT(context.position_bytes != nullptr &&
                         entry.position_width == minimizer_scalar_width::u16,
                     "fixed orientation context is prevalidated");
        auto cursor = position_range_cursor::create(mapped_payload_, control, entry);
        if (!cursor)
            return cursor.error();
        std::uint64_t target_strand_zero = 0u;
        std::uint64_t target_strand_one = 0u;
        std::uint64_t packed = 0u;
        while (cursor->next_fixed_p16(packed)) {
            std::uint64_t& count = (packed & 1u) == 0u ? target_strand_zero : target_strand_one;
            if (count == (std::numeric_limits<std::uint64_t>::max)()) {
                return minimizer_generation_encoding_error::arithmetic_overflow;
            }
            ++count;
        }
        if (const auto error = cursor->error())
            return error;
        std::uint64_t observed = target_strand_zero;
        if (!add_checked_(observed, target_strand_one) ||
            observed != control.position_end - control.position_begin ||
            !add_checked_(*context.position_bytes, observed * sizeof(std::uint16_t))) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return accumulate_orientation_for_group_<p_oracle>(
            context, group, target_strand_zero, target_strand_one);
    }

    template <bool p_oracle>
    [[nodiscard]] std::optional<minimizer_generation_encoding_error>
    process_fixed_emission_control_(fragment_probe_context& context,
                                    const entry_plan& entry,
                                    const query_occurrence_group& group,
                                    const grouped_soa_decoded_control& control) const noexcept {
        DEBUG_ASSERT(context.scratch != nullptr && context.batch != nullptr &&
                         context.block != nullptr && context.position_bytes != nullptr &&
                         context.anchors_constructed != nullptr &&
                         entry.position_width == minimizer_scalar_width::u16,
                     "fixed emission context is prevalidated");
        auto cursor = position_range_cursor::create(mapped_payload_, control, entry);
        if (!cursor)
            return cursor.error();
        const direct_emission_occurrence_context occurrence_context{
            .queries = context.queries,
            .block = context.block,
            .block_ordinal = context.block_ordinal,
            .scratch = context.scratch,
            .batch = context.batch,
        };

        const auto occurrence_error = with_query_group_occurrences_<p_oracle>(
            context,
            group,
            [this, &cursor, &occurrence_context](
                const auto occurrences,
                const query_occurrence_packing_layout& packing_layout) noexcept
                -> std::optional<minimizer_generation_encoding_error> {
                std::uint64_t packed = 0u;
                while (cursor->next_fixed_p16(packed)) {
                    for (const auto& occurrence : occurrences) {
                        if (const auto error = emit_fragment_anchor_(
                                packed,
                                query_occurrence_reference_(occurrence, packing_layout),
                                occurrence_context)) {
                            return error;
                        }
                    }
                }
                return cursor->error();
            });
        if (occurrence_error)
            return occurrence_error;

        const std::uint64_t decoded_positions = control.position_end - control.position_begin;
        const std::uint64_t position_bytes =
            bioseq::common::saturating_mul(decoded_positions, sizeof(std::uint16_t));
        const std::uint64_t constructed =
            bioseq::common::saturating_mul(decoded_positions, group.occurrence_ref_count);
        if (position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            constructed == (std::numeric_limits<std::uint64_t>::max)() ||
            !add_checked_(*context.position_bytes, position_bytes) ||
            !add_checked_(*context.anchors_constructed, constructed)) {
            return minimizer_generation_encoding_error::arithmetic_overflow;
        }
        return std::nullopt;
    }

    template <bool p_oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_count_(fragment_probe_context& context,
                               std::size_t entry_ordinal) const noexcept {
        DEBUG_ASSERT(context.scratch != nullptr && entry_ordinal < entry_count_ &&
                         context.pass == fragment_probe_pass::control_count,
                     "fixed count context is prevalidated");
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size() ==
                             static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                         "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                         "production matcher view is prevalidated");
        }
        const grouped_soa_bucket_directory_entry& directory_entry =
            directory_.entries()[entry_ordinal];
        const entry_plan& entry = entry_plans_[entry_ordinal];
        DEBUG_ASSERT(
            entry.hash_width == minimizer_scalar_width::u32 &&
                entry.count_width == minimizer_scalar_width::u16 &&
                entry.position_width == minimizer_scalar_width::u16 &&
                entry.control_count == directory_entry.control_element_count &&
                entry.position_element_count == directory_entry.position_element_count &&
                entry.control_begin <= entry.hash_begin && entry.hash_begin <= entry.hash_end &&
                entry.hash_end <= entry.prefix_begin && entry.prefix_begin <= entry.prefix_end &&
                entry.prefix_end <= entry.count_begin && entry.count_begin <= entry.count_end &&
                entry.count_end <= entry.control_end && entry.control_end <= mapped_payload_.size(),
            "fixed probe consumes open-validated entry geometry");

        std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t plan_cursor = 0u;
        std::uint32_t plan_end = 0u;
        std::uint32_t previous_hash = 0u;
        std::uint32_t first_hash = 0u;
        std::uint32_t last_hash = 0u;
        const std::size_t count = static_cast<std::size_t>(entry.control_count);
        for (std::size_t control_ordinal = 0u; control_ordinal < count; ++control_ordinal) {
            const std::uint32_t hash =
                static_cast<std::uint32_t>(read_fixed_width_le_<minimizer_scalar_width::u32>(
                    mapped_payload_,
                    static_cast<std::size_t>(entry.hash_begin) +
                        control_ordinal * sizeof(std::uint32_t)));
            const std::uint64_t local_count = read_fixed_width_le_<minimizer_scalar_width::u16>(
                mapped_payload_,
                static_cast<std::size_t>(entry.count_begin) +
                    control_ordinal * sizeof(std::uint16_t));
            if (hash > profile_.max_hash || (control_ordinal != 0u && hash <= previous_hash) ||
                local_count == 0u || local_count > profile_.max_occ_cap + 1u) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            if (control_ordinal == 0u)
                first_hash = hash;
            last_hash = hash;
            previous_hash = hash;

            const auto matched_group =
                match_query_group_<p_oracle>(hash, context, active_bucket, plan_cursor, plan_end);
            if (!matched_group)
                continue;
            (void)reserve_query_occurrences_(
                occurrence_counter_<p_oracle>(context, matched_group->counter_ordinal),
                local_count,
                context.max_occ);
        }
        if (first_hash != directory_entry.lo_hash || last_hash != directory_entry.hi_hash) {
            return std::unexpected(minimizer_generation_encoding_error::control_range);
        }
        return {};
    }

    template <bool p_oracle, minimizer_scalar_width p_offset_width>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_p16_matched_(fragment_probe_context& context,
                                     std::size_t entry_ordinal) const noexcept {
        static_assert(p_offset_width == minimizer_scalar_width::u16 ||
                      p_offset_width == minimizer_scalar_width::u32 ||
                      p_offset_width == minimizer_scalar_width::u64);
        DEBUG_ASSERT(context.scratch != nullptr && entry_ordinal < entry_count_ &&
                         context.pass != fragment_probe_pass::control_count &&
                         context.matched_count_bytes != nullptr,
                     "fixed matched context is prevalidated");
        if constexpr (p_oracle) {
            DEBUG_ASSERT(context.query_plan_radix.size() ==
                             static_cast<std::size_t>(query_radix_bucket_count_) + 1u,
                         "oracle query radix is prevalidated");
        } else {
            DEBUG_ASSERT(context.bucket_direct_matcher != nullptr,
                         "production matcher view is prevalidated");
        }
        const entry_plan& entry = entry_plans_[entry_ordinal];
        DEBUG_ASSERT(
            entry.shape_offset_width == p_offset_width &&
                entry.hash_width == minimizer_scalar_width::u32 &&
                entry.count_width == minimizer_scalar_width::u16 &&
                entry.position_width == minimizer_scalar_width::u16 &&
                entry.control_count == directory_.entries()[entry_ordinal].control_element_count &&
                entry.position_element_count ==
                    directory_.entries()[entry_ordinal].position_element_count &&
                entry.control_begin <= entry.hash_begin && entry.hash_begin <= entry.hash_end &&
                entry.hash_end <= entry.prefix_begin && entry.prefix_begin <= entry.prefix_end &&
                entry.prefix_end <= entry.count_begin && entry.count_begin <= entry.count_end &&
                entry.count_end <= entry.control_end && entry.control_end <= mapped_payload_.size(),
            "fixed matched probe consumes Pass1-authenticated controls");

        std::uint32_t active_bucket = (std::numeric_limits<std::uint32_t>::max)();
        std::uint32_t plan_cursor = 0u;
        std::uint32_t plan_end = 0u;
        std::optional<std::uint64_t> previous_selected_position_end;
        const std::size_t count = static_cast<std::size_t>(entry.control_count);
        for (std::size_t control_ordinal = 0u; control_ordinal < count; ++control_ordinal) {
            // Pass 1 has already authenticated H ordering/range and every C
            // value under a joined barrier. Later mandatory passes scan H for
            // matching, then touch C only for a control they will consume.
            const std::uint32_t hash =
                static_cast<std::uint32_t>(read_fixed_width_le_<minimizer_scalar_width::u32>(
                    mapped_payload_,
                    static_cast<std::size_t>(entry.hash_begin) +
                        control_ordinal * sizeof(std::uint32_t)));
            const auto matched_group =
                match_query_group_<p_oracle>(hash, context, active_bucket, plan_cursor, plan_end);
            if (!matched_group)
                continue;
            const std::uint32_t global_count =
                occurrence_counter_<p_oracle>(context, matched_group->counter_ordinal)
                    .load(std::memory_order_relaxed);
            if (global_count == 0u) {
                return std::unexpected(minimizer_generation_encoding_error::partition_id);
            }
            if (global_count > context.max_occ && context.pass == fragment_probe_pass::emission) {
                continue;
            }
            if (!add_checked_(*context.matched_count_bytes, sizeof(std::uint16_t))) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::uint64_t local_count = read_fixed_width_le_<minimizer_scalar_width::u16>(
                mapped_payload_,
                static_cast<std::size_t>(entry.count_begin) +
                    control_ordinal * sizeof(std::uint16_t));
            std::uint64_t* const prefix_stats = global_count > context.max_occ
                                                    ? context.rejected_prefix_bytes
                                                    : context.accepted_prefix_bytes;
            if (prefix_stats != nullptr &&
                !add_checked_(*prefix_stats, 2u * sizeof(minimizer_wire_uint_t_<p_offset_width>))) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            const std::size_t prefix_offset =
                static_cast<std::size_t>(entry.prefix_begin) +
                control_ordinal * sizeof(minimizer_wire_uint_t_<p_offset_width>);
            const std::uint64_t position_begin =
                read_fixed_width_le_<p_offset_width>(mapped_payload_, prefix_offset);
            const std::uint64_t position_end = read_fixed_width_le_<p_offset_width>(
                mapped_payload_, prefix_offset + sizeof(minimizer_wire_uint_t_<p_offset_width>));
            if (position_end <= position_begin || position_end > entry.position_element_count ||
                (control_ordinal == 0u && position_begin != 0u) ||
                (control_ordinal + 1u == count && position_end != entry.position_element_count) ||
                (previous_selected_position_end &&
                 position_begin < *previous_selected_position_end) ||
                local_count != std::min(position_end - position_begin, profile_.max_occ_cap + 1u)) {
                return std::unexpected(minimizer_generation_encoding_error::control_range);
            }
            previous_selected_position_end = position_end;

            if (global_count > context.max_occ) {
                if (context.pass == fragment_probe_pass::rejected_validation ||
                    context.pass == fragment_probe_pass::orientation) {
                    continue;
                }
                return std::unexpected(minimizer_generation_encoding_error::partition_id);
            }
            if (context.pass == fragment_probe_pass::rejected_validation) {
                return std::unexpected(minimizer_generation_encoding_error::partition_id);
            }
            const grouped_soa_decoded_control control{
                .hash = hash,
                .position_begin = position_begin,
                .position_end = position_end,
                .saturated_count = local_count,
            };
            const auto error = context.pass == fragment_probe_pass::orientation
                                   ? process_fixed_orientation_control_<p_oracle>(
                                         context, entry, *matched_group, control)
                                   : process_fixed_emission_control_<p_oracle>(
                                         context, entry, *matched_group, control);
            if (error)
                return std::unexpected(*error);
        }
        return {};
    }

    template <bool p_oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_entry_h32_c16_p16_dispatch_(fragment_probe_context& context,
                                      std::size_t entry_ordinal) const noexcept {
        if (context.pass == fragment_probe_pass::control_count) {
            return probe_entry_h32_c16_count_<p_oracle>(context, entry_ordinal);
        }
        switch (entry_plans_[entry_ordinal].shape_offset_width) {
        case minimizer_scalar_width::u16:
            return probe_entry_h32_c16_p16_matched_<p_oracle, minimizer_scalar_width::u16>(
                context, entry_ordinal);
        case minimizer_scalar_width::u32:
            return probe_entry_h32_c16_p16_matched_<p_oracle, minimizer_scalar_width::u32>(
                context, entry_ordinal);
        case minimizer_scalar_width::u64:
            return probe_entry_h32_c16_p16_matched_<p_oracle, minimizer_scalar_width::u64>(
                context, entry_ordinal);
        }
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    template <bool p_oracle>
    [[nodiscard]] std::expected<void, minimizer_generation_encoding_error>
    probe_block_h32_c16_p16_(fragment_probe_context& context) const noexcept {
        if (context.block == nullptr || context.block->entry_begin >= context.block->entry_end ||
            context.block->entry_end > entry_count_) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        for (std::size_t entry_ordinal = context.block->entry_begin;
             entry_ordinal < context.block->entry_end;
             ++entry_ordinal) {
            const auto probed = probe_entry_h32_c16_p16_dispatch_<p_oracle>(context, entry_ordinal);
            if (!probed)
                return probed;
        }
        return {};
    }

    [[nodiscard]] std::uint64_t final_owned_bytes_() const noexcept {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        std::uint64_t bytes = directory_.owned_capacity_bytes();
        bytes = saturating_add(bytes, saturating_mul(entry_count_, sizeof(entry_plan)));
        bytes = saturating_add(bytes, saturating_mul(block_count_, sizeof(fragment_block_plan)));
        return bytes;
    }

    [[nodiscard]] static std::expected<direct_open_contract, minimizer_generation_encoding_error>
    validate_direct_open_contract_(const minimizer_generation_head& head,
                                   const minimizer_generation_manifest& manifest,
                                   const expected_minimizer_generation_identity& expected,
                                   std::uint64_t max_occ,
                                   const owned_compact_hash32_direct_index_view& compact_direct) {
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            head.layout != manifest.layout || expected.layout != manifest.layout ||
            !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
            max_occ > profile->max_occ_cap ||
            profile->max_hash > (std::numeric_limits<std::uint32_t>::max)() ||
            max_occ > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (auto validated = validate_minimizer_generation_storage_contract(manifest, expected);
            !validated) {
            return std::unexpected(validated.error());
        }

        const std::uint32_t profile_max_hash = static_cast<std::uint32_t>(profile->max_hash);
        const std::uint32_t profile_hash_bits = std::bit_width(profile_max_hash);
        const std::uint64_t expected_profile_max_hash =
            profile_hash_bits == 0u ? 0u : (std::uint64_t{1} << profile_hash_bits) - 1u;
        if (profile_hash_bits == 0u ||
            profile_hash_bits > (std::numeric_limits<std::uint8_t>::max)() ||
            profile->max_hash != expected_profile_max_hash) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        if (head.generation_id != manifest.generation_id ||
            head.source_identity != manifest.source_identity ||
            head.source_content_checksum != manifest.source_content_checksum) {
            return std::unexpected(minimizer_generation_encoding_error::identity_mismatch);
        }
        const auto manifest_bytes = encode_minimizer_generation_manifest_record(manifest);
        if (!manifest_bytes) {
            return std::unexpected(manifest_bytes.error());
        }
        if (head.manifest_checksum != crc64_ecma182(std::span<const std::byte>{*manifest_bytes})) {
            return std::unexpected(minimizer_generation_encoding_error::checksum);
        }
        if (manifest.gpu_abi_revision != profile->gpu_abi_revision ||
            head.storage_contract_checksum !=
                compute_minimizer_generation_storage_contract_checksum(manifest)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }

        const compact_hash32_header& direct_header = compact_direct.view_.header;
        const direct_index_identity expected_identity{
            .generation_id = head.generation_id,
            .source_identity = head.source_identity,
            .storage_contract_checksum = head.storage_contract_checksum,
            .target_length = manifest.target_length,
        };
        if (direct_header.identity != expected_identity ||
            direct_header.max_occ_cap != profile->max_occ_cap ||
            direct_header.geometry.hash_domain_bits != profile_hash_bits ||
            direct_header.geometry.max_hash != profile_max_hash ||
            direct_header.image_bytes != compact_direct.view_.image.size() ||
            compact_direct.view_.image.empty() ||
            compact_direct.image_owner_.get() != compact_direct.view_.image.data()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return direct_open_contract{
            .profile = *profile,
            .hash_domain_bits = static_cast<std::uint8_t>(profile_hash_bits),
        };
    }

    void
    prepare_compact_direct_anchors_(bioseq::parallelism::memory_admission_controller& admission,
                                    std::span<const fragment_seed_query> queries,
                                    std::uint32_t runtime_max_occ,
                                    query_wave_scratch& scratch,
                                    prepared_fragment_anchor_batch& batch) const {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        if (!compact_direct_) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 direct query contract is inconsistent");
        }
        const std::size_t direct_query_plan_count = scratch.query_plans_.size();

        batch.stats_.compact_direct_waves = 1u;
        batch.stats_.compact_direct_unique_query_groups = direct_query_plan_count;
        const std::uint64_t maximum_plan_bytes = saturating_mul(
            saturating_mul(direct_query_plan_count, sizeof(compact_hash32_unique_selection)), 2u);
        if (maximum_plan_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 accepted-plan admission size overflow");
        }
        auto direct_plan_permit = admission.try_reserve(maximum_plan_bytes);
        if (!direct_plan_permit) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "compact-H32 accepted-plan admission failed");
        }

        const std::uint64_t lookup_started_ns = stage_now_ns_();
        auto queried = query_compact_hash32_direct_index_unique_projected(
            compact_direct_->view_,
            direct_query_plan_count,
            [&query_plans = scratch.query_plans_](std::size_t ordinal) noexcept -> std::uint32_t {
                return query_plans[ordinal].hash;
            },
            runtime_max_occ);
        if (!queried) {
            if (queried.error() == direct_inverted_index_error::admission ||
                queried.error() == direct_inverted_index_error::allocation) {
                throw persisted_minimizer_lookup_error(
                    persisted_minimizer_lookup_error::reason::admission_or_allocation,
                    "compact-H32 unique lookup allocation/admission "
                    "failed");
            }
            throw persisted_minimizer_lookup_error(
                "compact-H32 unique lookup failed; direct_error=" +
                std::to_string(static_cast<unsigned>(queried.error())));
        }
        compact_hash32_unique_query_plan plan = std::move(*queried);
        const std::uint64_t retained_plan_bytes = plan.retained_selection_capacity_bytes();
        if (plan.stats.selection_peak_capacity_bytes > direct_plan_permit->bytes() ||
            retained_plan_bytes != plan.stats.selection_retained_capacity_bytes) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 accepted-plan peak exceeded admission");
        }
        direct_plan_permit->shrink_to(retained_plan_bytes);
        batch.stats_.compact_direct_lookup_ns = elapsed_stage_ns_(lookup_started_ns);
        batch.stats_.compact_direct_unique_lookups = plan.stats.distinct_query_hash_count;
        batch.stats_.compact_direct_selected_hashes = plan.stats.unique_selected_hash_count;
        batch.stats_.compact_direct_runtime_rejected_hashes =
            plan.stats.runtime_rejected_hash_count;
        batch.stats_.compact_direct_runtime_rejected_postings =
            plan.stats.runtime_rejected_posting_count;
        batch.stats_.compact_direct_selected_postings = plan.stats.selected_posting_count;
        batch.stats_.compact_direct_rejected_posting_reads = plan.stats.rejected_posting_reads;
        batch.stats_.compact_direct_plan_bytes =
            saturating_mul(plan.selections.size(), sizeof(compact_hash32_unique_selection));
        batch.stats_.compact_direct_plan_reserve_calls = plan.stats.selection_reserve_call_count;
        batch.stats_.compact_direct_plan_reserve_requested_elements =
            plan.stats.selection_reserve_requested_elements;
        batch.stats_.compact_direct_plan_initial_capacity_elements =
            plan.stats.selection_initial_capacity_elements;
        batch.stats_.compact_direct_plan_fill_reallocations =
            plan.stats.selection_fill_reallocation_count;
        batch.stats_.compact_direct_plan_compaction_allocations =
            plan.stats.selection_compaction_allocation_count;
        batch.stats_.compact_direct_plan_storage_allocations =
            plan.stats.selection_storage_allocation_count;
        batch.stats_.compact_direct_plan_retained_capacity_elements =
            plan.stats.selection_retained_capacity_elements;
        batch.stats_.compact_direct_plan_retained_capacity_bytes =
            plan.stats.selection_retained_capacity_bytes;
        batch.stats_.peak_compact_direct_plan_capacity_bytes =
            plan.stats.selection_peak_capacity_bytes;
        const std::uint64_t expected_storage_allocations =
            direct_query_plan_count == 0u                                                  ? 0u
            : plan.selections.empty() || plan.selections.size() == direct_query_plan_count ? 1u
                                                                                           : 2u;
        const std::uint64_t expected_compaction_allocations =
            !plan.selections.empty() && plan.selections.size() < direct_query_plan_count ? 1u : 0u;
        const std::uint64_t expected_peak_capacity_elements =
            expected_compaction_allocations == 0u
                ? direct_query_plan_count
                : saturating_add(direct_query_plan_count, plan.selections.size());
        const std::uint64_t expected_peak_capacity_bytes = saturating_mul(
            expected_peak_capacity_elements, sizeof(compact_hash32_unique_selection));
        if (plan.stats.query_occurrence_count != direct_query_plan_count ||
            plan.stats.distinct_query_hash_count != direct_query_plan_count ||
            plan.stats.unique_selected_hash_count != plan.selections.size() ||
            plan.stats.rejected_posting_reads != 0u ||
            plan.stats.selection_reserve_call_count != 1u ||
            plan.stats.selection_reserve_requested_elements != direct_query_plan_count ||
            plan.stats.selection_initial_capacity_elements != direct_query_plan_count ||
            plan.stats.selection_fill_reallocation_count != 0u ||
            plan.stats.selection_compaction_allocation_count != expected_compaction_allocations ||
            plan.stats.selection_storage_allocation_count != expected_storage_allocations ||
            plan.stats.selection_retained_capacity_elements != plan.selections.size() ||
            plan.stats.selection_retained_capacity_bytes !=
                batch.stats_.compact_direct_plan_bytes ||
            plan.stats.selection_peak_capacity_bytes != expected_peak_capacity_bytes ||
            retained_plan_bytes != batch.stats_.compact_direct_plan_bytes ||
            batch.stats_.compact_direct_plan_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            expected_peak_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            throw persisted_minimizer_lookup_error("compact-H32 unique lookup stats disagrees");
        }

        const bool have_selected_postings = plan.stats.selected_posting_count != 0u;
        const std::size_t selection_shard_count =
            have_selected_postings
                ? std::min(plan.selections.size(), compact_direct_selection_shard_target_)
                : 0u;
        if (have_selected_postings &&
            (selection_shard_count == 0u || queries.empty() ||
             selection_shard_count > (std::numeric_limits<std::size_t>::max)() / queries.size())) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 shard/query layout size is inconsistent");
        }
        const std::size_t shard_query_layout_count = selection_shard_count * queries.size();
        batch.stats_.compact_direct_selection_shards = selection_shard_count;
        batch.stats_.compact_direct_shard_query_layouts = shard_query_layout_count;
        const std::uint64_t orientation_started_ns = stage_now_ns_();
        if (have_selected_postings) {
            const std::size_t old_layout_capacity = scratch.block_query_layouts_.capacity();
            scratch.ensure_vector_capacity_(scratch.block_query_layouts_, shard_query_layout_count);
            batch.stats_.block_layout_allocations =
                scratch.block_query_layouts_.capacity() > old_layout_capacity ? 1u : 0u;
            batch.stats_.peak_block_layout_capacity_bytes = saturating_mul(
                scratch.block_query_layouts_.capacity(), sizeof(block_query_anchor_layout));
            scratch.block_query_layouts_.assign(shard_query_layout_count,
                                                block_query_anchor_layout{});

            const std::size_t old_block_stats_capacity = scratch.block_stats_.capacity();
            scratch.ensure_vector_capacity_(scratch.block_stats_, queries.size());
            batch.stats_.block_stats_allocations =
                scratch.block_stats_.capacity() > old_block_stats_capacity ? 1u : 0u;
            batch.stats_.peak_block_stats_capacity_bytes =
                saturating_mul(scratch.block_stats_.capacity(), sizeof(fragment_block_wave_stats));
            scratch.block_stats_.assign(queries.size(), fragment_block_wave_stats{});
        }

        const auto selection_access =
            bind_compact_hash32_direct_index_selection_spans(compact_direct_->view_, plan);
        if (!selection_access || selection_access->selection_count() != plan.selections.size()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 selection-range binding failed; direct_error=" +
                std::to_string(static_cast<unsigned>(selection_access
                                                         ? direct_inverted_index_error::query_plan
                                                         : selection_access.error())));
        }

        std::array<compact_direct_layout_pass_context, compact_direct_selection_shard_target_>
            layout_contexts{};
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            layout_contexts[shard_ordinal] = compact_direct_layout_pass_context{
                .reader = this,
                .plan = &plan,
                .queries = queries,
                .scratch = &scratch,
                .shard_ordinal = shard_ordinal,
            };
        }
        if (have_selected_postings) {
            parallel_entries_(
                selection_shard_count,
                [&selection_access,
                 &layout_contexts,
                 selection_count = plan.selections.size(),
                 selection_shard_count](std::size_t shard_begin, std::size_t shard_end) noexcept {
                    for (std::size_t shard_ordinal = shard_begin; shard_ordinal < shard_end;
                         ++shard_ordinal) {
                        compact_direct_layout_pass_context& context =
                            layout_contexts[shard_ordinal];
                        const compact_direct_selection_shard_range range =
                            compact_direct_selection_shard_range_(
                                selection_count, selection_shard_count, shard_ordinal);
                        auto visited = visit_compact_hash32_direct_index_selection_span_range(
                            *selection_access,
                            range.begin,
                            range.end,
                            [&context](
                                const compact_hash32_selection_posting_spans& selected) noexcept {
                                return visit_compact_direct_layout_selection_(context, selected);
                            });
                        flush_compact_direct_selected_page_advice_(context);
                        if (visited) {
                            context.visit_stats = *visited;
                        } else if (!context.error && !context.direct_error) {
                            context.direct_error = visited.error();
                        }
                    }
                });
        }

        // Every shard binds authenticated descriptors and span geometry, but
        // deliberately does not load one encoded posting position.
        batch.stats_.compact_direct_layout_postings_visited = 0u;
        std::uint64_t layout_logical_postings = 0u;
        std::size_t layout_selection_end = 0u;
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            const compact_direct_layout_pass_context& context = layout_contexts[shard_ordinal];
            if (context.error) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 descriptor layout shard failed; wire_error=" +
                    std::to_string(static_cast<unsigned>(*context.error)));
            }
            if (context.direct_error || !context.visit_stats) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 descriptor layout shard failed; direct_error=" +
                    std::to_string(static_cast<unsigned>(
                        context.direct_error ? *context.direct_error
                                             : direct_inverted_index_error::query_plan)));
            }
            const compact_direct_selection_shard_range range =
                compact_direct_selection_shard_range_(
                    plan.selections.size(), selection_shard_count, shard_ordinal);
            const compact_hash32_selection_span_visit_stats& visit = *context.visit_stats;
            if (range.begin != layout_selection_end || range.end <= range.begin ||
                range.end > plan.selections.size() ||
                visit.selection_callbacks != range.end - range.begin ||
                visit.descriptors_examined != range.end - range.begin ||
                visit.postings_exposed != context.logical_postings_counted ||
                visit.rank_checkpoints_examined != 0u || visit.membership_words_examined != 0u ||
                !add_checked_(layout_logical_postings, context.logical_postings_counted) ||
                !add_checked_(batch.stats_.compact_direct_layout_selection_callbacks,
                              visit.selection_callbacks) ||
                !add_checked_(batch.stats_.compact_direct_layout_spans_exposed,
                              visit.spans_exposed) ||
                !add_checked_(batch.stats_.compact_direct_layout_descriptors_examined,
                              visit.descriptors_examined) ||
                !add_checked_(batch.stats_.compact_direct_traversal_rank_checkpoints_examined,
                              visit.rank_checkpoints_examined) ||
                !add_checked_(batch.stats_.compact_direct_traversal_membership_words_examined,
                              visit.membership_words_examined)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 descriptor layout shard receipt disagrees");
            }
            layout_selection_end = range.end;
            ++batch.stats_.compact_direct_layout_shards_completed;
        }
        if (layout_logical_postings != plan.stats.selected_posting_count ||
            layout_selection_end != plan.selections.size() ||
            batch.stats_.compact_direct_layout_selection_callbacks != plan.selections.size() ||
            batch.stats_.compact_direct_layout_descriptors_examined != plan.selections.size()) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 descriptor layout aggregate receipt disagrees");
        }

        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            if (!have_selected_postings) {
                batch.queries_[query_ordinal].anchor_count = 0u;
                continue;
            }
            std::uint64_t forward_cursor = 0u;
            for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
                 ++shard_ordinal) {
                block_query_anchor_layout& layout =
                    scratch.block_query_layouts_[shard_ordinal * queries.size() + query_ordinal];
                layout.forward_begin = forward_cursor;
                layout.forward_cursor = forward_cursor;
                if (!add_checked_(forward_cursor, layout.forward_count)) {
                    throw persisted_minimizer_lookup_error(
                        "compact-H32 forward shard prefix overflow");
                }
            }
            std::uint64_t reverse_cursor = forward_cursor;
            for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
                 ++shard_ordinal) {
                block_query_anchor_layout& layout =
                    scratch.block_query_layouts_[shard_ordinal * queries.size() + query_ordinal];
                layout.reverse_begin = reverse_cursor;
                layout.reverse_cursor = reverse_cursor;
                if (!add_checked_(reverse_cursor, layout.reverse_count)) {
                    throw persisted_minimizer_lookup_error(
                        "compact-H32 reverse shard prefix overflow");
                }
            }
            batch.queries_[query_ordinal].anchor_count = reverse_cursor;
        }

        std::uint64_t final_owned_bytes = batch.owned_bytes_;
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            const prepared_query_anchors& query = batch.queries_[query_ordinal];
            const std::uint64_t anchor_bytes =
                saturating_mul(query.anchor_count, sizeof(bioseq::chaining::anchor));
            final_owned_bytes = saturating_add(final_owned_bytes, anchor_bytes);
            if (anchor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                final_owned_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                query.anchor_count >
                    (std::numeric_limits<std::size_t>::max)() / sizeof(bioseq::chaining::anchor)) {
                throw persisted_minimizer_lookup_error("compact-H32 exact anchor size overflow");
            }
        }
        if (!batch.permit_.grow_to(final_owned_bytes)) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "compact-H32 exact anchor admission failed");
        }
        try {
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
                prepared_query_anchors& query = batch.queries_[query_ordinal];
                if (query.anchor_count == 0u)
                    continue;
                query.anchors = std::make_unique_for_overwrite<bioseq::chaining::anchor[]>(
                    static_cast<std::size_t>(query.anchor_count));
            }
        } catch (const std::bad_alloc&) {
            throw persisted_minimizer_lookup_error(
                persisted_minimizer_lookup_error::reason::admission_or_allocation,
                "compact-H32 exact anchor allocation failed");
        }
        batch.owned_bytes_ = final_owned_bytes;
        batch.permit_.shrink_to(batch.owned_bytes_);
        batch.stats_.orientation_layout_ns = elapsed_stage_ns_(orientation_started_ns);

        const std::uint64_t emission_started_ns = stage_now_ns_();
        std::array<compact_direct_emission_pass_context, compact_direct_selection_shard_target_>
            emission_contexts{};
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            emission_contexts[shard_ordinal] = compact_direct_emission_pass_context{
                .reader = this,
                .plan = &plan,
                .queries = queries,
                .scratch = &scratch,
                .batch = &batch,
                .shard_ordinal = shard_ordinal,
            };
        }
        if (have_selected_postings) {
            parallel_entries_(
                selection_shard_count,
                [&selection_access,
                 &emission_contexts,
                 selection_count = plan.selections.size(),
                 selection_shard_count](std::size_t shard_begin, std::size_t shard_end) noexcept {
                    for (std::size_t shard_ordinal = shard_begin; shard_ordinal < shard_end;
                         ++shard_ordinal) {
                        compact_direct_emission_pass_context& context =
                            emission_contexts[shard_ordinal];
                        const compact_direct_selection_shard_range range =
                            compact_direct_selection_shard_range_(
                                selection_count, selection_shard_count, shard_ordinal);
                        auto visited = visit_compact_hash32_direct_index_selection_span_range(
                            *selection_access,
                            range.begin,
                            range.end,
                            [&context](
                                const compact_hash32_selection_posting_spans& selected) noexcept {
                                return visit_compact_direct_emission_selection_(context, selected);
                            });
                        if (visited) {
                            context.visit_stats = *visited;
                        } else if (!context.error && !context.direct_error) {
                            context.direct_error = visited.error();
                        }
                    }
                });
        }
        batch.stats_.compact_direct_posting_visit_passes = have_selected_postings ? 1u : 0u;
        std::uint64_t emission_posting_bytes = 0u;
        std::uint64_t emission_postings = 0u;
        std::uint64_t emission_anchors = 0u;
        std::size_t emission_selection_end = 0u;
        for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
             ++shard_ordinal) {
            const compact_direct_emission_pass_context& context = emission_contexts[shard_ordinal];
            if (context.error) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 emission shard failed; wire_error=" +
                    std::to_string(static_cast<unsigned>(*context.error)));
            }
            if (context.direct_error || !context.visit_stats) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 emission shard failed; direct_error=" +
                    std::to_string(static_cast<unsigned>(
                        context.direct_error ? *context.direct_error
                                             : direct_inverted_index_error::query_plan)));
            }
            const compact_direct_selection_shard_range range =
                compact_direct_selection_shard_range_(
                    plan.selections.size(), selection_shard_count, shard_ordinal);
            const compact_hash32_selection_span_visit_stats& visit = *context.visit_stats;
            if (range.begin != emission_selection_end || range.end <= range.begin ||
                range.end > plan.selections.size() ||
                visit.selection_callbacks != range.end - range.begin ||
                visit.descriptors_examined != range.end - range.begin ||
                visit.postings_exposed != context.postings_visited ||
                visit.rank_checkpoints_examined != 0u || visit.membership_words_examined != 0u ||
                !add_checked_(emission_posting_bytes, context.posting_bytes_read) ||
                !add_checked_(emission_postings, context.postings_visited) ||
                !add_checked_(emission_anchors, context.anchors_constructed) ||
                !add_checked_(batch.stats_.compact_direct_emission_selection_callbacks,
                              visit.selection_callbacks) ||
                !add_checked_(batch.stats_.compact_direct_emission_spans_exposed,
                              visit.spans_exposed) ||
                !add_checked_(batch.stats_.compact_direct_emission_descriptors_examined,
                              visit.descriptors_examined) ||
                !add_checked_(batch.stats_.compact_direct_traversal_rank_checkpoints_examined,
                              visit.rank_checkpoints_examined) ||
                !add_checked_(batch.stats_.compact_direct_traversal_membership_words_examined,
                              visit.membership_words_examined)) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 emission shard receipt disagrees");
            }
            emission_selection_end = range.end;
            ++batch.stats_.compact_direct_emission_shards_completed;
        }
        batch.stats_.compact_direct_emission_postings_visited = emission_postings;
        batch.stats_.direct_emission_ns = elapsed_stage_ns_(emission_started_ns);
        if (emission_postings != plan.stats.selected_posting_count ||
            emission_selection_end != plan.selections.size() ||
            batch.stats_.compact_direct_emission_selection_callbacks != plan.selections.size() ||
            batch.stats_.compact_direct_emission_descriptors_examined != plan.selections.size() ||
            batch.stats_.compact_direct_emission_spans_exposed !=
                batch.stats_.compact_direct_layout_spans_exposed) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 emission aggregate receipt disagrees");
        }

        if (have_selected_postings) {
            for (std::size_t shard_ordinal = 0u; shard_ordinal < selection_shard_count;
                 ++shard_ordinal) {
                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                     ++query_ordinal) {
                    const block_query_anchor_layout& layout =
                        scratch
                            .block_query_layouts_[shard_ordinal * queries.size() + query_ordinal];
                    if (layout.forward_begin >
                            (std::numeric_limits<std::uint64_t>::max)() - layout.forward_count ||
                        layout.reverse_begin >
                            (std::numeric_limits<std::uint64_t>::max)() - layout.reverse_count ||
                        layout.forward_cursor != layout.forward_begin + layout.forward_count ||
                        layout.reverse_cursor != layout.reverse_begin + layout.reverse_count) {
                        throw persisted_minimizer_lookup_error(
                            "compact-H32 shard emission cursor receipt "
                            "disagrees");
                    }
                }
            }
        }

        std::atomic<std::uint32_t> first_error{0u};
        const auto run_local_order_stage = [this, queries, &batch, &scratch, &first_error](
                                               bool normalize_ties) {
            first_error.store(0u, std::memory_order_relaxed);
            parallel_entries_(
                queries.size(),
                [queries,
                 &batch,
                 &scratch,
                 &first_error,
                 normalize_ties,
                 authenticated_target_length = manifest_.target_length](
                    std::size_t query_begin, std::size_t query_end) noexcept {
                    for (std::size_t query_ordinal = query_begin; query_ordinal < query_end;
                         ++query_ordinal) {
                        if (first_error.load(std::memory_order_relaxed) != 0u ||
                            query_ordinal >= scratch.block_stats_.size()) {
                            if (query_ordinal >= scratch.block_stats_.size()) {
                                record_error_(
                                    first_error,
                                    minimizer_generation_encoding_error::arithmetic_overflow);
                            }
                            return;
                        }
                        std::uint64_t query_order_bytes = 0u;
                        const block_query_anchor_layout& first_shard_layout =
                            scratch.block_query_layouts_[query_ordinal];
                        auto& query = batch.queries_[query_ordinal];
                        const std::uint64_t forward_count = first_shard_layout.reverse_begin;
                        if (forward_count > query.anchor_count) {
                            record_error_(first_error,
                                          minimizer_generation_encoding_error::arithmetic_overflow);
                            return;
                        }
                        const std::uint64_t reverse_count = query.anchor_count - forward_count;
                        const auto order_slice =
                            [normalize_ties, &query_order_bytes, authenticated_target_length](
                                bioseq::chaining::anchor* anchors, std::uint64_t count) noexcept {
                                if (count < 2u)
                                    return true;
                                const auto stats =
                                    normalize_ties ? normalize_anchor_slice_ties_(
                                                         anchors, static_cast<std::size_t>(count))
                                                   : sort_anchor_slice_primary_(
                                                         anchors,
                                                         static_cast<std::size_t>(count),
                                                         authenticated_target_length);
                                const std::uint64_t bytes =
                                    normalize_ties ? stats.tie_bytes : stats.primary_bytes;
                                return !stats.saturated && add_checked_(query_order_bytes, bytes);
                            };
                        if ((forward_count > 1u &&
                             !order_slice(query.anchors.get(), forward_count)) ||
                            (reverse_count > 1u &&
                             !order_slice(query.anchors.get() +
                                              static_cast<std::size_t>(forward_count),
                                          reverse_count))) {
                            record_error_(first_error,
                                          minimizer_generation_encoding_error::arithmetic_overflow);
                            return;
                        }
                        fragment_block_wave_stats& block_stats =
                            scratch.block_stats_[query_ordinal];
                        if (normalize_ties) {
                            block_stats.tie_normalization_bytes = query_order_bytes;
                        } else {
                            block_stats.local_sort_bytes = query_order_bytes;
                        }
                    }
                });
            if (const auto error = recorded_error_(first_error)) {
                throw persisted_minimizer_lookup_error(
                    std::string{normalize_ties ? "compact-H32 tie normalization failed; "
                                               : "compact-H32 local primary sort failed; "} +
                    "wire_error=" + std::to_string(static_cast<unsigned>(*error)));
            }
        };

        const std::uint64_t local_sort_started_ns = stage_now_ns_();
        if (have_selected_postings)
            run_local_order_stage(false);
        batch.stats_.local_sort_ns = elapsed_stage_ns_(local_sort_started_ns);
        const std::uint64_t tie_started_ns = stage_now_ns_();
        if (have_selected_postings)
            run_local_order_stage(true);
        batch.stats_.tie_normalization_ns = elapsed_stage_ns_(tie_started_ns);

        std::uint64_t expected_constructed = 0u;
        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
            if (!add_checked_(expected_constructed, batch.queries_[query_ordinal].anchor_count)) {
                throw persisted_minimizer_lookup_error("compact-H32 anchor stats overflow");
            }
        }
        for (const fragment_block_wave_stats& block_stats : scratch.block_stats_) {
            if (!add_checked_(batch.stats_.local_sort_bytes, block_stats.local_sort_bytes) ||
                !add_checked_(batch.stats_.tie_normalization_bytes,
                              block_stats.tie_normalization_bytes)) {
                throw persisted_minimizer_lookup_error("compact-H32 stats reduction overflow");
            }
        }
        batch.stats_.pass2_position_bytes = 0u;
        batch.stats_.pass3_position_bytes = emission_posting_bytes;
        batch.stats_.anchors_constructed = emission_anchors;
        const std::uint64_t expected_posting_bytes =
            saturating_mul(plan.stats.selected_posting_count, plan.posting_width_bytes);
        const std::uint64_t exact_final_bytes =
            saturating_mul(expected_constructed, sizeof(bioseq::chaining::anchor));
        if (batch.stats_.compact_direct_unique_lookups !=
                batch.stats_.compact_direct_unique_query_groups ||
            batch.stats_.compact_direct_selection_shards != selection_shard_count ||
            batch.stats_.compact_direct_shard_query_layouts != shard_query_layout_count ||
            batch.stats_.compact_direct_layout_shards_completed != selection_shard_count ||
            batch.stats_.compact_direct_emission_shards_completed != selection_shard_count ||
            batch.stats_.compact_direct_posting_visit_passes !=
                (have_selected_postings ? 1u : 0u) ||
            batch.stats_.compact_direct_layout_postings_visited != 0u ||
            batch.stats_.compact_direct_emission_postings_visited !=
                plan.stats.selected_posting_count ||
            batch.stats_.compact_direct_layout_selection_callbacks != plan.selections.size() ||
            batch.stats_.compact_direct_emission_selection_callbacks != plan.selections.size() ||
            (plan.selections.empty() ? batch.stats_.compact_direct_layout_spans_exposed != 0u
                                     : batch.stats_.compact_direct_layout_spans_exposed == 0u) ||
            batch.stats_.compact_direct_layout_spans_exposed !=
                batch.stats_.compact_direct_emission_spans_exposed ||
            batch.stats_.compact_direct_layout_descriptors_examined != plan.selections.size() ||
            batch.stats_.compact_direct_emission_descriptors_examined != plan.selections.size() ||
            batch.stats_.compact_direct_traversal_rank_checkpoints_examined != 0u ||
            batch.stats_.compact_direct_traversal_membership_words_examined != 0u ||
            batch.stats_.compact_direct_target_control_scan_bytes != 0u ||
            batch.stats_.compact_direct_target_control_scan_passes != 0u ||
            batch.stats_.compact_direct_fallbacks != 0u ||
            expected_posting_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            exact_final_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            batch.stats_.pass2_position_bytes != 0u ||
            batch.stats_.pass3_position_bytes != expected_posting_bytes ||
            batch.stats_.anchors_constructed != expected_constructed ||
            emission_anchors != expected_constructed) {
            throw persisted_minimizer_lookup_error(
                "compact-H32 direct-route stats equation failed");
        }
        batch.stats_.exact_final_bytes = exact_final_bytes;
        batch.stats_.peak_exact_final_bytes = exact_final_bytes;
    }

  public:
    prepared_minimizer_fragment_reader(prepared_minimizer_fragment_reader&&) noexcept = default;
    prepared_minimizer_fragment_reader& operator=(prepared_minimizer_fragment_reader&&) = delete;
    prepared_minimizer_fragment_reader(const prepared_minimizer_fragment_reader&) = delete;
    prepared_minimizer_fragment_reader&
    operator=(const prepared_minimizer_fragment_reader&) = delete;

    /// Open the compact-H32 route without decoding or retaining the grouped-SoA
    /// directory and without owning a grouped-SoA payload mapping.  The manifest
    /// supplies only its authenticated uniform segment geometry. The direct
    /// image remains the sole posting/control authority for this reader.
    [[nodiscard]] static std::expected<prepared_minimizer_fragment_reader,
                                       minimizer_generation_encoding_error>
    open_compact_direct(bioseq::parallelism::memory_admission_controller& admission,
                        const minimizer_generation_head& head,
                        const minimizer_generation_manifest& manifest,
                        const expected_minimizer_generation_identity& expected,
                        std::uint64_t max_occ,
                        owned_compact_hash32_direct_index_view compact_direct,
                        compact_direct_selected_page_adviser selected_page_adviser = {}) {
        if (!selected_page_adviser.empty() && !selected_page_adviser.valid()) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const auto contract =
            validate_direct_open_contract_(head, manifest, expected, max_occ, compact_direct);
        if (!contract) {
            return std::unexpected(contract.error());
        }

        auto permit = admission.try_reserve(0u);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }

        prepared_minimizer_fragment_reader reader;
        reader.permit_ = std::move(*permit);
        reader.head_ = head;
        reader.manifest_ = manifest;
        reader.profile_ = contract->profile;
        reader.max_occ_ = max_occ;
        reader.backing_kind_ = backing_kind::compact_direct_only;
        reader.compact_direct_ = std::move(compact_direct);
        reader.selected_page_adviser_ = selected_page_adviser;
        reader.query_radix_shift_ =
            static_cast<std::uint8_t>(contract->hash_domain_bits > query_radix_bits_
                                          ? contract->hash_domain_bits - query_radix_bits_
                                          : 0u);
        reader.query_membership_filter_shift_ = static_cast<std::uint8_t>(
            contract->hash_domain_bits > 21u ? contract->hash_domain_bits - 21u : 0u);
        reader.query_membership_filter_mask_id_mask_ =
            reader.query_membership_filter_shift_ == 0u
                ? 0u
                : static_cast<std::uint16_t>(
                      (std::uint32_t{1} << reader.query_membership_filter_shift_) - 1u);
        reader.wire_dispatch_ =
            reader.profile_.hash_width == minimizer_scalar_width::u32 &&
                    reader.profile_.count_width == minimizer_scalar_width::u16 &&
                    reader.profile_.position_width == minimizer_scalar_width::u16
                ? fragment_encoding_dispatch::h32_c16_p16
                : fragment_encoding_dispatch::checked_dynamic;

        // validate_direct_open_contract_ has already authenticated the
        // manifest checksum and its standalone uniform-segment equations.
        // Compact-direct queries address postings through the direct image,
        // so re-deriving every segment core and retaining fragment_block_plan
        // objects would be redundant O(segment_count) cold-open work.

        reader.ready_ = true;
        return reader;
    }

    [[nodiscard]] static std::expected<prepared_minimizer_fragment_reader,
                                       minimizer_generation_encoding_error>
    open(bioseq::parallelism::memory_admission_controller& admission,
         const minimizer_generation_head& head,
         const minimizer_generation_manifest& manifest,
         const expected_minimizer_generation_identity& expected,
         std::span<const std::byte> encoded_directory,
         std::uint64_t max_occ,
         std::shared_ptr<const void> mapped_payload_owner,
         std::span<const std::byte> mapped_payload,
         std::optional<owned_compact_hash32_direct_index_view> compact_direct = std::nullopt) {
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;
        const auto profile = derive_grouped_soa_minimizer_profile(manifest);
        if (!profile || manifest.layout != minimizer_generation_layout::grouped_soa_segment_local ||
            head.layout != manifest.layout || expected.layout != manifest.layout ||
            !expected.max_occ_cap || *expected.max_occ_cap != profile->max_occ_cap ||
            max_occ > profile->max_occ_cap ||
            profile->max_hash > (std::numeric_limits<std::uint32_t>::max)() ||
            max_occ > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) ||
            mapped_payload_owner == nullptr || mapped_payload.size() != manifest.payload_bytes ||
            encoded_directory.size() != manifest.directory_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (auto validated = validate_minimizer_generation_storage_contract(manifest, expected);
            !validated) {
            return std::unexpected(validated.error());
        }
        const std::uint32_t profile_max_hash = static_cast<std::uint32_t>(profile->max_hash);
        const std::uint32_t profile_hash_bits = std::bit_width(profile_max_hash);
        const std::uint64_t expected_profile_max_hash =
            profile_hash_bits == 0u ? 0u : (std::uint64_t{1} << profile_hash_bits) - 1u;
        if (profile_hash_bits == 0u || profile->max_hash != expected_profile_max_hash) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        if (compact_direct) {
            const compact_hash32_header& direct_header = compact_direct->view_.header;
            const direct_index_identity expected_identity{
                .generation_id = head.generation_id,
                .source_identity = head.source_identity,
                .storage_contract_checksum = head.storage_contract_checksum,
                .target_length = manifest.target_length,
            };
            if (direct_header.identity != expected_identity ||
                direct_header.max_occ_cap != profile->max_occ_cap ||
                direct_header.geometry.hash_domain_bits != profile_hash_bits ||
                direct_header.geometry.max_hash != profile_max_hash ||
                direct_header.image_bytes != compact_direct->view_.image.size() ||
                compact_direct->view_.image.empty() ||
                compact_direct->image_owner_.get() != compact_direct->view_.image.data()) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
        }

        const std::size_t directory_stride = minimizer_directory_entry_encoded_bytes(*profile);
        if (directory_stride == 0u || encoded_directory.size() % directory_stride != 0u) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        const std::size_t entry_count = encoded_directory.size() / directory_stride;
        if (entry_count > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        std::uint64_t initial_bytes =
            saturating_add(encoded_directory.size(),
                           saturating_mul(entry_count, sizeof(grouped_soa_bucket_directory_entry)));
        initial_bytes =
            saturating_add(initial_bytes, saturating_mul(entry_count, sizeof(entry_plan)));
        initial_bytes =
            saturating_add(initial_bytes, saturating_mul(entry_count, sizeof(fragment_block_plan)));
        if (initial_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        auto permit = admission.try_reserve(initial_bytes);
        if (!permit) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }

        prepared_minimizer_fragment_reader reader;
        reader.permit_ = std::move(*permit);
        reader.head_ = head;
        reader.manifest_ = manifest;
        reader.profile_ = *profile;
        reader.query_radix_shift_ = static_cast<std::uint8_t>(
            profile_hash_bits > query_radix_bits_ ? profile_hash_bits - query_radix_bits_ : 0u);
        reader.query_membership_filter_shift_ =
            static_cast<std::uint8_t>(profile_hash_bits > 21u ? profile_hash_bits - 21u : 0u);
        reader.query_membership_filter_mask_id_mask_ =
            reader.query_membership_filter_shift_ == 0u
                ? 0u
                : static_cast<std::uint16_t>(
                      (std::uint32_t{1} << reader.query_membership_filter_shift_) - 1u);
        reader.mapped_payload_owner_ = std::move(mapped_payload_owner);
        reader.mapped_payload_ = mapped_payload;
        reader.compact_direct_ = std::move(compact_direct);
        reader.entry_count_ = entry_count;
        reader.max_occ_ = max_occ;
        try {
            auto directory = grouped_soa_directory_view::decode_admitted(
                encoded_directory, *profile, reader.permit_);
            if (!directory) {
                return std::unexpected(directory.error());
            }
            if (auto metadata = validate_minimizer_generation_metadata(head, manifest, *directory);
                !metadata) {
                return std::unexpected(metadata.error());
            }
            reader.directory_ = std::move(*directory);
            if (entry_count != 0u) {
                reader.entry_plans_ = std::make_unique<entry_plan[]>(entry_count);
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }

        std::uint64_t total_control_bytes = 0u;
        std::uint64_t total_control_hash_scan_bytes = 0u;
        std::uint64_t total_control_count_scan_bytes = 0u;
        std::uint64_t total_position_bytes = 0u;
        const auto entries = reader.directory_.entries();
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            const grouped_soa_bucket_directory_entry& entry = entries[ordinal];
            if (entry.position_segment < 0 || static_cast<std::uint32_t>(entry.position_segment) >=
                                                  manifest.position_segment_count) {
                return std::unexpected(minimizer_generation_encoding_error::position_segment);
            }

            if (!is_minimizer_scalar_width(profile->hash_width) ||
                !is_minimizer_scalar_width(profile->count_width) ||
                !is_minimizer_scalar_width(profile->position_width) ||
                !is_minimizer_scalar_width(entry.shape_offset_width) ||
                entry.control_element_count == 0u) {
                return std::unexpected(
                    minimizer_generation_encoding_error::storage_contract_mismatch);
            }
            const auto layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                               profile->hash_width,
                                                               entry.shape_offset_width,
                                                               profile->count_width);
            if (!layout || layout->encoded_bytes != entry.control_encoded_bytes) {
                return std::unexpected(layout ? minimizer_generation_encoding_error::byte_count
                                              : layout.error());
            }

            const std::uint64_t h = static_cast<std::uint8_t>(profile->hash_width);
            const std::uint64_t o = static_cast<std::uint8_t>(entry.shape_offset_width);
            const std::uint64_t c = static_cast<std::uint8_t>(profile->count_width);
            const std::uint64_t p = static_cast<std::uint8_t>(profile->position_width);
            const std::uint64_t prefix_element_count =
                saturating_add(entry.control_element_count, 1u);
            const std::uint64_t hash_bytes = saturating_mul(entry.control_element_count, h);
            const std::uint64_t prefix_bytes = saturating_mul(prefix_element_count, o);
            const std::uint64_t count_bytes = saturating_mul(entry.control_element_count, c);
            const std::uint64_t position_bytes = saturating_mul(entry.position_element_count, p);
            const std::uint64_t count_scan_bytes =
                saturating_mul(entry.control_element_count, h + c);
            if (prefix_element_count == (std::numeric_limits<std::uint64_t>::max)() ||
                hash_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                prefix_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                count_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                position_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                count_scan_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
            if (position_bytes != entry.position_encoded_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }

            entry_plan& plan = reader.entry_plans_[ordinal];
            plan.control_begin = total_control_bytes;
            plan.control_end = saturating_add(plan.control_begin, entry.control_encoded_bytes);
            plan.hash_begin = saturating_add(plan.control_begin, layout->hash_offset);
            plan.hash_end = saturating_add(plan.hash_begin, hash_bytes);
            plan.prefix_begin = saturating_add(plan.control_begin, layout->prefix_offset);
            plan.prefix_end = saturating_add(plan.prefix_begin, prefix_bytes);
            plan.count_begin = saturating_add(plan.control_begin, layout->count_offset);
            plan.count_end = saturating_add(plan.count_begin, count_bytes);
            if (plan.control_end == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.hash_begin == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.hash_end == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.prefix_begin == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.prefix_end == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.count_begin == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.count_end == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.control_end > manifest.payload_bytes ||
                plan.hash_begin != plan.control_begin || plan.hash_end > plan.prefix_begin ||
                plan.prefix_end > plan.count_begin || plan.count_end > plan.control_end) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }

            const auto core = derive_minimizer_position_segment_bounds(
                manifest, static_cast<std::uint32_t>(entry.position_segment));
            if (!core || core->end_bp <= core->begin_bp || core->end_bp > manifest.target_length) {
                return std::unexpected(minimizer_generation_encoding_error::bp_range);
            }
            plan.control_count = entry.control_element_count;
            plan.position_element_count = entry.position_element_count;
            plan.reference_begin = core->begin_bp;
            plan.reference_end = core->end_bp;
            plan.control_scan_bytes = count_scan_bytes;
            plan.position_segment = static_cast<std::uint32_t>(entry.position_segment);
            plan.hash_byte_width = static_cast<std::uint8_t>(profile->hash_width);
            plan.count_byte_width = static_cast<std::uint8_t>(profile->count_width);
            plan.position_byte_width = static_cast<std::uint8_t>(profile->position_width);
            plan.offset_byte_width = static_cast<std::uint8_t>(entry.shape_offset_width);
            plan.hash_width = profile->hash_width;
            plan.count_width = profile->count_width;
            plan.position_width = profile->position_width;
            plan.shape_offset_width = entry.shape_offset_width;

            total_control_bytes = plan.control_end;
            if (!add_checked_(total_control_hash_scan_bytes, hash_bytes) ||
                !add_checked_(total_control_count_scan_bytes, count_scan_bytes) ||
                !add_checked_(total_position_bytes, entry.position_encoded_bytes)) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        if (total_position_bytes > manifest.payload_bytes ||
            total_control_bytes > manifest.payload_bytes - total_position_bytes ||
            total_control_bytes + total_position_bytes != manifest.payload_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }
        reader.total_control_hash_scan_bytes_ = total_control_hash_scan_bytes;
        reader.total_control_count_scan_bytes_ = total_control_count_scan_bytes;
        std::uint64_t position_cursor = 0u;
        for (std::size_t ordinal = 0u; ordinal < entries.size(); ++ordinal) {
            entry_plan& plan = reader.entry_plans_[ordinal];
            plan.position_begin = saturating_add(total_control_bytes, position_cursor);
            plan.position_end =
                saturating_add(plan.position_begin, entries[ordinal].position_encoded_bytes);
            if (plan.position_begin == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.position_end == (std::numeric_limits<std::uint64_t>::max)() ||
                plan.position_end > manifest.payload_bytes) {
                return std::unexpected(minimizer_generation_encoding_error::byte_count);
            }
            position_cursor =
                saturating_add(position_cursor, entries[ordinal].position_encoded_bytes);
            if (position_cursor == (std::numeric_limits<std::uint64_t>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
            }
        }
        if (position_cursor != total_position_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::byte_count);
        }

        reader.wire_dispatch_ = profile->hash_width == minimizer_scalar_width::u32 &&
                                        profile->count_width == minimizer_scalar_width::u16 &&
                                        profile->position_width == minimizer_scalar_width::u16
                                    ? fragment_encoding_dispatch::h32_c16_p16
                                    : fragment_encoding_dispatch::checked_dynamic;

        // Build a compact address plan whose block boundaries can never split
        // an equal-position-segment run. Empty physical segments are permitted
        // gaps and need no synthetic directory entry.
        constexpr std::size_t target_entries_per_block = 128u;
        for (std::size_t ordinal = 1u; ordinal < entries.size(); ++ordinal) {
            if (entries[ordinal].position_segment < entries[ordinal - 1u].position_segment) {
                return std::unexpected(minimizer_generation_encoding_error::directory_order);
            }
        }
        std::size_t block_count = 0u;
        for (std::size_t begin = 0u; begin < entries.size();) {
            std::size_t end = std::min(entries.size(), begin + target_entries_per_block);
            const auto terminal_segment = entries[end - 1u].position_segment;
            while (end < entries.size() && entries[end].position_segment == terminal_segment) {
                ++end;
            }
            ++block_count;
            begin = end;
        }
        try {
            if (block_count != 0u) {
                reader.block_plans_ = std::make_unique<fragment_block_plan[]>(block_count);
            }
        } catch (const std::bad_alloc&) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        reader.block_count_ = block_count;

        std::uint64_t previous_block_reference_end = 0u;
        bool have_previous_block = false;
        std::size_t block_ordinal = 0u;
        for (std::size_t begin = 0u; begin < entries.size();) {
            std::size_t end = std::min(entries.size(), begin + target_entries_per_block);
            const auto terminal_segment = entries[end - 1u].position_segment;
            while (end < entries.size() && entries[end].position_segment == terminal_segment) {
                ++end;
            }
            if (begin >= end || end > entries.size() ||
                end > (std::numeric_limits<std::uint32_t>::max)()) {
                return std::unexpected(minimizer_generation_encoding_error::directory_size);
            }

            fragment_block_plan& block = reader.block_plans_[block_ordinal];
            block.entry_begin = static_cast<std::uint32_t>(begin);
            block.entry_end = static_cast<std::uint32_t>(end);
            block.first_position_segment =
                static_cast<std::uint32_t>(entries[begin].position_segment);
            block.last_position_segment =
                static_cast<std::uint32_t>(entries[end - 1u].position_segment);

            bool have_core = false;
            std::uint64_t previous_core_end = 0u;
            std::int32_t previous_segment = -1;
            std::size_t segment_run_begin = begin;
            for (std::size_t ordinal = begin; ordinal < end; ++ordinal) {
                const std::int32_t segment = entries[ordinal].position_segment;
                if (segment == previous_segment)
                    continue;
                if (previous_segment >= 0 &&
                    ordinal - segment_run_begin > target_entries_per_block) {
                    ++reader.oversized_segment_block_count_;
                }
                segment_run_begin = ordinal;
                const entry_plan& planned_entry = reader.entry_plans_[ordinal];
                if (planned_entry.position_segment != static_cast<std::uint32_t>(segment) ||
                    planned_entry.reference_end <= planned_entry.reference_begin ||
                    (have_core && previous_core_end > planned_entry.reference_begin)) {
                    return std::unexpected(minimizer_generation_encoding_error::bp_range);
                }
                if (!have_core) {
                    block.reference_begin = planned_entry.reference_begin;
                    have_core = true;
                }
                block.reference_end = planned_entry.reference_end;
                previous_core_end = planned_entry.reference_end;
                previous_segment = segment;
            }
            if (end - segment_run_begin > target_entries_per_block) {
                ++reader.oversized_segment_block_count_;
            }
            if (!have_core || block.reference_end <= block.reference_begin ||
                (have_previous_block && previous_block_reference_end > block.reference_begin)) {
                return std::unexpected(minimizer_generation_encoding_error::bp_range);
            }
            previous_block_reference_end = block.reference_end;
            have_previous_block = true;
            reader.largest_block_entry_count_ = std::max(reader.largest_block_entry_count_,
                                                         static_cast<std::uint32_t>(end - begin));
            ++block_ordinal;
            begin = end;
        }
        if (block_ordinal != block_count) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }

        // The PostgreSQL-owned image is published only after a complete
        // address-bound row and fragment-extent CRC reread, fsync, and durable
        // rename. Its exact generation header was checked before this call.
        // Trust those immutable fragment payloads here. Reader open owns only
        // directory/address plans; max-occ reduction is fused into the later
        // fragment-parallel query wave so minimizer-fragment has no target-wide open scan.

        reader.owned_bytes_ = reader.final_owned_bytes_();
        if (reader.owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)() ||
            (reader.owned_bytes_ > reader.permit_.bytes() &&
             !reader.permit_.grow_to(reader.owned_bytes_))) {
            return std::unexpected(reader.owned_bytes_ ==
                                           (std::numeric_limits<std::uint64_t>::max)()
                                       ? minimizer_generation_encoding_error::arithmetic_overflow
                                       : minimizer_generation_encoding_error::admission);
        }
        reader.permit_.shrink_to(reader.owned_bytes_);
        reader.ready_ = true;
        return reader;
    }

    /// Prepare one immutable, canonically ordered anchor span per query.
    /// grouped-SoA control scans use count/layout/emission passes. The explicit
    /// compact-direct route instead performs one lookup per unique query hash,
    /// derives exact query/orientation counts from accepted descriptors, and
    /// decodes each accepted posting exactly once into its final owner, with no
    /// control-scan fallback.
    [[nodiscard]] prepared_fragment_anchor_batch prepare_fragment_anchor_batch(
        bioseq::parallelism::memory_admission_controller& admission,
        std::span<const fragment_seed_query> queries,
        std::int32_t max_occ,
        query_wave_scratch& scratch,
        bool baseline_query_plan_matcher_for_testing = false,
        bool verify_compositional_order_for_testing = false,
        minimizer_fragment_lookup_route lookup_route =
            minimizer_fragment_lookup_route::grouped_soa_control_scans) const {
        if (lookup_route == minimizer_fragment_lookup_route::grouped_soa_control_scans &&
            backing_kind_ == backing_kind::compact_direct_only) {
            throw persisted_minimizer_lookup_error(
                "grouped-SoA control scans were explicitly selected on a "
                "compact-direct-only reader");
        }
        if (lookup_route == minimizer_fragment_lookup_route::direct_compact_hash_lookup) {
            if (baseline_query_plan_matcher_for_testing) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 direct route cannot use the grouped-SoA "
                    "baseline matcher");
            }
            return prepare_fragment_anchor_batch_impl_<false, true>(
                admission, queries, max_occ, scratch, verify_compositional_order_for_testing);
        }
        if (baseline_query_plan_matcher_for_testing) {
            return prepare_fragment_anchor_batch_impl_<true, false>(
                admission, queries, max_occ, scratch, verify_compositional_order_for_testing);
        }
        return prepare_fragment_anchor_batch_impl_<false, false>(
            admission, queries, max_occ, scratch, verify_compositional_order_for_testing);
    }

    template <bool p_oracle, bool p_compact_direct>
    [[nodiscard]] prepared_fragment_anchor_batch
    prepare_fragment_anchor_batch_impl_(bioseq::parallelism::memory_admission_controller& admission,
                                        std::span<const fragment_seed_query> queries,
                                        std::int32_t max_occ,
                                        query_wave_scratch& scratch,
                                        bool verify_compositional_order_for_testing) const {
        using query_hash_plan = query_wave_scratch::query_hash_plan;
        using bioseq::common::saturating_add;
        using bioseq::common::saturating_mul;

        if (!ready_) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA reader is not ready");
        }
        if constexpr (p_compact_direct) {
            if (!compact_direct_) {
                throw persisted_minimizer_lookup_error(
                    "compact-H32 direct route was explicitly selected "
                    "without an attached direct index");
            }
            if (backing_kind_ == backing_kind::compact_direct_only) {
                if (mapped_payload_owner_ != nullptr || !mapped_payload_.empty() ||
                    directory_.owned_capacity_bytes() != 0u || entry_count_ != 0u ||
                    entry_plans_ != nullptr || block_plans_ != nullptr || block_count_ != 0u ||
                    owned_bytes_ != 0u || permit_.bytes() != 0u) {
                    throw persisted_minimizer_lookup_error(
                        "compact-direct-only reader retained forbidden "
                        "grouped-SoA state");
                }
            } else if (mapped_payload_owner_ == nullptr ||
                       mapped_payload_.size() != manifest_.payload_bytes) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native grouped-SoA reader is not ready");
            }
        } else if (backing_kind_ != backing_kind::grouped_soa_fragments ||
                   mapped_payload_owner_ == nullptr ||
                   mapped_payload_.size() != manifest_.payload_bytes) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA reader is not ready");
        }
        if (max_occ < 0 || static_cast<std::uint64_t>(max_occ) != max_occ_ ||
            queries.size() > (std::numeric_limits<std::uint32_t>::max)()) {
            throw persisted_minimizer_lookup_error(
                "fragment-native grouped-SoA query policy is inconsistent");
        }
        // Freeze the manifest-selected wire route once for the complete wave.
        // Offset width remains an entry-local dispatch inside the fixed leaf family.
        [[maybe_unused]] const bool use_fixed_h32_c16_p16 =
            wire_dispatch_ == fragment_encoding_dispatch::h32_c16_p16;
        static_assert(!(p_oracle && p_compact_direct));
        constexpr bool use_bucket_direct_matcher = !p_oracle && !p_compact_direct;

        // Acquire the reusable scratch before reading or mutating any of its
        // non-atomic owners. The busy state remains published through the
        // synchronous joins that borrow the prepared batch and is cleared by
        // complete_wave_after_join. A failed preparation clears it in the
        // catch path below, so there is no reentrant vector/permit race.
        query_wave_scratch::wave_state expected_state = query_wave_scratch::wave_state::idle;
        if (!scratch.wave_state_.compare_exchange_strong(expected_state,
                                                         query_wave_scratch::wave_state::preparing,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
            throw persisted_minimizer_lookup_error(
                "fragment-native query scratch is already in use");
        }

        try {
            if (!scratch.permit_ || scratch.admission_ != &admission) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native grouped-SoA query scratch identity is inconsistent");
            }
            scratch.clear_wave_storage_noexcept_();
            if constexpr (p_compact_direct) {
                // This route never probes grouped-SoA controls.  Retained Bloom,
                // radix, group-directory, and atomic-counter owners from a
                // preceding control-scan wave are released before direct planning
                // so they cannot overlap the accepted-only direct plan.
                {
                    std::vector<std::uint32_t> no_group_offsets;
                    std::vector<std::uint32_t> no_radix;
                    scratch.query_group_offsets_.swap(no_group_offsets);
                    scratch.query_plan_radix_.swap(no_radix);
                }
                scratch.query_membership_filter_.reset();
                scratch.query_membership_filter_word_capacity_ = 0u;
                scratch.occurrence_counts_.reset();
                scratch.occurrence_count_capacity_ = 0u;
                scratch.active_occurrence_count_ = 0u;
                scratch.permit_.shrink_to(scratch.retained_capacity_bytes_());
            }
            if (use_bucket_direct_matcher && scratch.query_plans_.capacity() != 0u) {
                {
                    std::vector<query_hash_plan> no_baseline_plans;
                    scratch.query_plans_.swap(no_baseline_plans);
                }
                scratch.query_plan_reserve_request_ = 0u;
                scratch.permit_.shrink_to(scratch.retained_capacity_bytes_());
            }
            std::uint64_t total_query_minimizers = 0u;
            std::uint64_t largest_query_minimizer_count = 0u;
            for (const fragment_seed_query& query : queries) {
                if (!bioseq::minimizers::is_minimizer_query_length_representable<
                        minimap_sketch_data>(query.query_length) ||
                    query.kmer_size <= 0 ||
                    query.kmer_size > (std::numeric_limits<std::uint8_t>::max)() ||
                    query.kmer_size != static_cast<std::int32_t>(profile_.kmer_size) ||
                    query.minimizers.size() > (std::numeric_limits<std::uint32_t>::max)() ||
                    total_query_minimizers >
                        (std::numeric_limits<std::uint32_t>::max)() - query.minimizers.size() ||
                    (!query.minimizers.empty() &&
                     query.query_length < static_cast<std::uint64_t>(query.kmer_size))) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native grouped-SoA query descriptor is invalid");
                }
                for (const minimap_sketch_data& minimizer : query.minimizers) {
                    const std::uint64_t forward_last = minimizer.pos_strand_encoded() >> 1u;
                    if (minimizer.kmer() > profile_.max_hash ||
                        minimizer.kmer() > (std::numeric_limits<std::uint32_t>::max)() ||
                        forward_last >= query.query_length ||
                        forward_last + 1u < static_cast<std::uint64_t>(query.kmer_size)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native grouped-SoA query minimizer is invalid");
                    }
                }
                total_query_minimizers += query.minimizers.size();
                largest_query_minimizer_count =
                    std::max<std::uint64_t>(largest_query_minimizer_count, query.minimizers.size());
            }
            const auto occurrence_layout = plan_query_occurrence_packing(
                queries.size(), total_query_minimizers, largest_query_minimizer_count);
            if (!occurrence_layout) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native query occurrence layout is invalid");
            }
            scratch.occurrence_layout_ = *occurrence_layout;

            auto batch_permit = admission.try_reserve(0u);
            if (!batch_permit) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native anchor batch admission failed");
            }
            prepared_fragment_anchor_batch batch{std::move(*batch_permit)};
            batch.query_count_ = queries.size();
            batch.target_length_ = manifest_.target_length;
            batch.max_occ_ = static_cast<std::uint64_t>(max_occ);
            batch.stats_.waves = 1u;
            batch.stats_.queries = queries.size();
            batch.stats_.query_occurrences = total_query_minimizers;
            batch.stats_.packed_occurrence_waves = occurrence_layout->packed() ? 1u : 0u;
            batch.stats_.wide_occurrence_waves = occurrence_layout->packed() ? 0u : 1u;
            batch.stats_.global_sort_calls_skipped = queries.size();

            const std::uint64_t metadata_bytes =
                saturating_mul(queries.size(), sizeof(prepared_query_anchors));
            if (queries.size() >
                    (std::numeric_limits<std::size_t>::max)() / sizeof(prepared_query_anchors) ||
                metadata_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native anchor metadata size overflow");
            }
            if (!batch.permit_.grow_to(metadata_bytes)) {
                throw persisted_minimizer_lookup_error(
                    persisted_minimizer_lookup_error::reason::admission_or_allocation,
                    "fragment-native anchor metadata admission failed");
            }
            try {
                if (!queries.empty()) {
                    batch.queries_ = std::make_unique<prepared_query_anchors[]>(queries.size());
                }
            } catch (const std::bad_alloc&) {
                batch.permit_.shrink_to(0u);
                throw persisted_minimizer_lookup_error(
                    persisted_minimizer_lookup_error::reason::admission_or_allocation,
                    "fragment-native anchor metadata allocation failed");
            }
            batch.owned_bytes_ = metadata_bytes;
            batch.permit_.shrink_to(batch.owned_bytes_);
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
                prepared_query_anchors& destination = batch.queries_[query_ordinal];
                const fragment_seed_query& source = queries[query_ordinal];
                destination.minimizer_data = source.minimizers.data();
                destination.minimizer_count = source.minimizers.size();
                destination.query_length = source.query_length;
                destination.kmer_size = source.kmer_size;
            }

            if constexpr (p_compact_direct) {
                // Ordinary seed lookup treats max_occ==0 as selecting no
                // postings.  Preserve that policy without calling the compact
                // codec's positive-cap query API and without touching either
                // grouped-SoA payload or direct postings.
                if (max_occ == 0) {
                    batch.stats_.compact_direct_waves = 1u;
                    batch.stats_.compositional_sortedness_receipts = queries.size();
                    batch.stats_.exact_final_bytes = 0u;
                    batch.stats_.peak_exact_final_bytes = 0u;
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        batch.queries_[query_ordinal].anchor_count = 0u;
                        batch.queries_[query_ordinal].sortedness_receipt =
                            prepared_fragment_anchor_batch::sortedness_receipt_;
                    }
                    if (!batch.stats_.forbidden_buffers_are_zero() ||
                        batch.owned_bytes_ > batch.permit_.bytes()) {
                        throw persisted_minimizer_lookup_error(
                            "compact-H32 zero-cap ownership stats failed");
                    }
                    batch.permit_.shrink_to(batch.owned_bytes_);
                    batch.ready_ = true;
                    scratch.wave_state_.store(query_wave_scratch::wave_state::outstanding,
                                              std::memory_order_release);
                    return batch;
                }
            }

            const std::size_t occurrence_count =
                static_cast<std::size_t>(occurrence_layout->occurrence_count);
            if (use_bucket_direct_matcher && occurrence_count != 0u) {
                const std::uint64_t query_radix_fill_started_ns = stage_now_ns_();
                const std::size_t radix_size =
                    static_cast<std::size_t>(query_radix_bucket_count_) + 1u;
                const std::size_t old_query_radix_capacity = scratch.query_plan_radix_.capacity();
                scratch.ensure_vector_capacity_(scratch.query_plan_radix_, radix_size);
                batch.stats_.query_radix_allocations =
                    scratch.query_plan_radix_.capacity() > old_query_radix_capacity ? 1u : 0u;
                batch.stats_.peak_query_radix_capacity_bytes =
                    saturating_mul(scratch.query_plan_radix_.capacity(), sizeof(std::uint32_t));
                scratch.query_plan_radix_.assign(radix_size, 0u);
                for (const fragment_seed_query& query : queries) {
                    for (const minimap_sketch_data& minimizer : query.minimizers) {
                        const std::uint32_t bucket =
                            query_bucket_(static_cast<std::uint32_t>(minimizer.kmer()));
                        ++scratch.query_plan_radix_[bucket + 1u];
                    }
                }
                for (std::size_t bucket = 1u; bucket < radix_size; ++bucket) {
                    if (scratch.query_plan_radix_[bucket] >
                        (std::numeric_limits<std::uint32_t>::max)() -
                            scratch.query_plan_radix_[bucket - 1u]) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native occurrence bucket prefix overflow");
                    }
                    scratch.query_plan_radix_[bucket] += scratch.query_plan_radix_[bucket - 1u];
                }
                if (scratch.query_plan_radix_.back() != occurrence_count) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native occurrence bucket count disagrees");
                }
                batch.stats_.query_plan_radix_fill_ns =
                    elapsed_stage_ns_(query_radix_fill_started_ns);
            }

            const std::uint64_t occurrence_fill_started_ns = stage_now_ns_();
            std::size_t occurrence_cursor = 0u;
            if (occurrence_layout->packed()) {
                const std::size_t old_occurrence_capacity = scratch.packed_occurrences_.capacity();
                scratch.ensure_vector_capacity_(scratch.packed_occurrences_, occurrence_count);
                batch.stats_.occurrence_allocations =
                    scratch.packed_occurrences_.capacity() > old_occurrence_capacity ? 1u : 0u;
                scratch.packed_occurrences_.resize(occurrence_count);
                const auto write_packed_occurrence =
                    [&scratch, occurrence_layout](std::size_t destination,
                                                  std::size_t query_ordinal,
                                                  std::size_t minimizer_ordinal,
                                                  const minimap_sketch_data& minimizer) {
                        const std::uint32_t encoded = encode_packed_query_occurrence_trusted(
                            *occurrence_layout,
                            static_cast<std::uint32_t>(query_ordinal),
                            static_cast<std::uint32_t>(minimizer_ordinal));
#ifndef NDEBUG
                        const query_occurrence_reference decoded =
                            decode_packed_query_occurrence_trusted(*occurrence_layout, encoded);
                        DEBUG_ASSERT(decoded.batch_query_ordinal == query_ordinal &&
                                         decoded.query_ordinal == minimizer_ordinal,
                                     "packed query occurrence round-trip");
#endif
                        scratch.packed_occurrences_[destination] = packed_query_occurrence{
                            .hash = static_cast<std::uint32_t>(minimizer.kmer()),
                            .query_and_ordinal = encoded,
                        };
                    };
                if constexpr (p_oracle || p_compact_direct) {
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        const auto minimizers = queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = 0u;
                             minimizer_ordinal < minimizers.size();
                             ++minimizer_ordinal) {
                            write_packed_occurrence(occurrence_cursor++,
                                                    query_ordinal,
                                                    minimizer_ordinal,
                                                    minimizers[minimizer_ordinal]);
                        }
                    }
                } else {
                    for (std::size_t query_ordinal = queries.size(); query_ordinal-- > 0u;) {
                        const auto minimizers = queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = minimizers.size();
                             minimizer_ordinal-- > 0u;) {
                            const std::uint32_t hash =
                                static_cast<std::uint32_t>(minimizers[minimizer_ordinal].kmer());
                            const std::uint32_t bucket = query_bucket_(hash);
                            const std::size_t destination =
                                --scratch.query_plan_radix_[bucket + 1u];
                            write_packed_occurrence(destination,
                                                    query_ordinal,
                                                    minimizer_ordinal,
                                                    minimizers[minimizer_ordinal]);
                            ++occurrence_cursor;
                        }
                    }
                }
            } else {
                const std::size_t old_occurrence_capacity = scratch.wide_occurrences_.capacity();
                scratch.ensure_vector_capacity_(scratch.wide_occurrences_, occurrence_count);
                batch.stats_.occurrence_allocations =
                    scratch.wide_occurrences_.capacity() > old_occurrence_capacity ? 1u : 0u;
                scratch.wide_occurrences_.resize(occurrence_count);
                const auto write_wide_occurrence =
                    [&scratch](std::size_t destination,
                               std::size_t query_ordinal,
                               std::size_t minimizer_ordinal,
                               const minimap_sketch_data& minimizer) {
                        scratch.wide_occurrences_[destination] = wide_query_occurrence{
                            .hash = static_cast<std::uint32_t>(minimizer.kmer()),
                            .batch_query_ordinal = static_cast<std::uint32_t>(query_ordinal),
                            .query_ordinal = static_cast<std::uint32_t>(minimizer_ordinal),
                        };
                    };
                if constexpr (p_oracle || p_compact_direct) {
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        const auto minimizers = queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = 0u;
                             minimizer_ordinal < minimizers.size();
                             ++minimizer_ordinal) {
                            write_wide_occurrence(occurrence_cursor++,
                                                  query_ordinal,
                                                  minimizer_ordinal,
                                                  minimizers[minimizer_ordinal]);
                        }
                    }
                } else {
                    for (std::size_t query_ordinal = queries.size(); query_ordinal-- > 0u;) {
                        const auto minimizers = queries[query_ordinal].minimizers;
                        for (std::size_t minimizer_ordinal = minimizers.size();
                             minimizer_ordinal-- > 0u;) {
                            const std::uint32_t hash =
                                static_cast<std::uint32_t>(minimizers[minimizer_ordinal].kmer());
                            const std::uint32_t bucket = query_bucket_(hash);
                            const std::size_t destination =
                                --scratch.query_plan_radix_[bucket + 1u];
                            write_wide_occurrence(destination,
                                                  query_ordinal,
                                                  minimizer_ordinal,
                                                  minimizers[minimizer_ordinal]);
                            ++occurrence_cursor;
                        }
                    }
                }
            }
            if (occurrence_cursor != occurrence_count) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native query occurrence count disagrees");
            }
            if (use_bucket_direct_matcher && occurrence_count != 0u) {
                for (std::size_t bucket = 0u; bucket < query_radix_bucket_count_; ++bucket) {
                    scratch.query_plan_radix_[bucket] = scratch.query_plan_radix_[bucket + 1u];
                }
                scratch.query_plan_radix_[query_radix_bucket_count_] =
                    static_cast<std::uint32_t>(occurrence_count);
            }
            batch.stats_.peak_occurrence_capacity_bytes =
                scratch.retained_occurrence_capacity_bytes_();
            batch.stats_.occurrence_fill_ns = elapsed_stage_ns_(occurrence_fill_started_ns);

            const std::uint64_t occurrence_radix_started_ns = stage_now_ns_();
            if constexpr (p_oracle || p_compact_direct) {
                if (occurrence_count >= 2u) {
                    scratch.occurrence_radix_invoked_ = true;
                    const auto key = [](const auto& occurrence) noexcept -> std::uint32_t {
                        return occurrence.hash;
                    };
                    if constexpr (p_compact_direct) {
                        if (occurrence_layout->packed() &&
                            occurrence_count >=
                                k_compact_direct_parallel_packed_occurrence_radix_min_count) {
                            const auto bucket_bounds =
                                partition_packed_occurrence_top_byte_(scratch.packed_occurrences_);
                            packed_query_occurrence* const occurrences =
                                scratch.packed_occurrences_.data();
                            scratch.parallel_occurrence_radix_split_selected_ = true;
                            parallel_entries_(
                                bucket_bounds.size() - 1u,
                                [occurrences, &bucket_bounds, key](
                                    std::size_t bucket_begin, std::size_t bucket_end) noexcept {
                                    for (std::size_t bucket = bucket_begin; bucket < bucket_end;
                                         ++bucket) {
                                        const std::size_t begin = bucket_bounds[bucket];
                                        const std::size_t end = bucket_bounds[bucket + 1u];
                                        if (end - begin < 2u)
                                            continue;
                                        bioseq::sorting::radix_sort_from_shift<16>(
                                            occurrences + begin, occurrences + end, key);
                                    }
                                });
                        } else if (occurrence_layout->packed()) {
                            bioseq::sorting::radix_sort(scratch.packed_occurrences_.data(),
                                                        scratch.packed_occurrences_.data() +
                                                            scratch.packed_occurrences_.size(),
                                                        key);
                        } else {
                            bioseq::sorting::radix_sort(scratch.wide_occurrences_.data(),
                                                        scratch.wide_occurrences_.data() +
                                                            scratch.wide_occurrences_.size(),
                                                        key);
                        }
                    } else if (occurrence_layout->packed()) {
                        bioseq::sorting::radix_sort(scratch.packed_occurrences_.data(),
                                                    scratch.packed_occurrences_.data() +
                                                        scratch.packed_occurrences_.size(),
                                                    key);
                    } else {
                        bioseq::sorting::radix_sort(scratch.wide_occurrences_.data(),
                                                    scratch.wide_occurrences_.data() +
                                                        scratch.wide_occurrences_.size(),
                                                    key);
                    }
                }
            } else {
                if (occurrence_count >= 2u) {
                    const auto sort_bucketed_occurrences = [&scratch](auto& occurrences) {
                        const auto hash_less = [](const auto& left, const auto& right) noexcept {
                            return left.hash < right.hash;
                        };
                        for (std::size_t bucket = 0u; bucket < query_radix_bucket_count_;
                             ++bucket) {
                            const std::size_t begin = scratch.query_plan_radix_[bucket];
                            const std::size_t end = scratch.query_plan_radix_[bucket + 1u];
                            if (end - begin < 2u)
                                continue;
                            scratch.occurrence_radix_invoked_ = true;
                            if (end - begin < 256u) {
                                std::sort(occurrences.begin() + begin,
                                          occurrences.begin() + end,
                                          hash_less);
                            } else {
                                const auto key =
                                    [](const auto& occurrence) noexcept -> std::uint32_t {
                                    return occurrence.hash;
                                };
                                bioseq::sorting::radix_sort(
                                    occurrences.data() + begin, occurrences.data() + end, key);
                            }
                        }
                    };
                    if (occurrence_layout->packed()) {
                        sort_bucketed_occurrences(scratch.packed_occurrences_);
                    } else {
                        sort_bucketed_occurrences(scratch.wide_occurrences_);
                    }
                }
            }
            batch.stats_.occurrence_radix_ns = elapsed_stage_ns_(occurrence_radix_started_ns);

            const std::uint64_t unique_plan_started_ns = stage_now_ns_();
            const std::size_t old_query_plan_capacity = scratch.query_plans_.capacity();
            std::uint64_t compact_direct_group_key_set_checksum =
                compact_direct_query_set_checksum_prefix(
                    static_cast<std::uint32_t>(profile_.kmer_size),
                    static_cast<std::uint32_t>(manifest_.window_size),
                    static_cast<std::uint32_t>(max_occ));
            const auto build_query_plans =
                [&scratch, &compact_direct_group_key_set_checksum](const auto& occurrences) {
                    std::size_t unique_count = 0u;
                    for (std::size_t begin = 0u; begin < occurrences.size();) {
                        std::size_t end = begin + 1u;
                        while (end < occurrences.size() &&
                               occurrences[end].hash == occurrences[begin].hash) {
                            ++end;
                        }
                        ++unique_count;
                        begin = end;
                    }
                    if (unique_count > (std::numeric_limits<std::uint32_t>::max)()) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native unique query plan count overflow");
                    }
                    scratch.query_plan_reserve_request_ = unique_count;
                    scratch.ensure_vector_capacity_(scratch.query_plans_, unique_count);
                    scratch.query_plans_.clear();
                    for (std::size_t begin = 0u; begin < occurrences.size();) {
                        std::size_t end = begin + 1u;
                        while (end < occurrences.size() &&
                               occurrences[end].hash == occurrences[begin].hash) {
                            ++end;
                        }
                        const std::uint32_t hash = occurrences[begin].hash;
                        scratch.query_plans_.push_back(query_hash_plan{
                            .hash = hash,
                            .occurrence_ref_begin = static_cast<std::uint32_t>(begin),
                            .occurrence_ref_count = static_cast<std::uint32_t>(end - begin),
                            .global_occurrence_count = 0u,
                        });
                        if constexpr (p_compact_direct) {
                            compact_direct_group_key_set_checksum =
                                compact_direct_query_set_fnv_mix_u64(
                                    compact_direct_group_key_set_checksum, hash);
                        }
                        begin = end;
                    }
                };
            const auto count_unique_occurrence_groups = [](const auto& occurrences) noexcept {
                std::size_t unique_count = 0u;
                for (std::size_t begin = 0u; begin < occurrences.size();) {
                    std::size_t end = begin + 1u;
                    while (end < occurrences.size() &&
                           occurrences[end].hash == occurrences[begin].hash) {
                        ++end;
                    }
                    ++unique_count;
                    begin = end;
                }
                return unique_count;
            };
            std::size_t unique_occurrence_group_count = 0u;
            bool production_group_hashes_strictly_monotonic = false;
            const std::size_t old_query_group_offset_capacity =
                scratch.query_group_offsets_.capacity();
            const std::size_t old_query_membership_filter_word_capacity =
                scratch.query_membership_filter_word_capacity_;
            if constexpr (p_oracle || p_compact_direct) {
                if (occurrence_layout->packed()) {
                    build_query_plans(scratch.packed_occurrences_);
                } else {
                    build_query_plans(scratch.wide_occurrences_);
                }
                unique_occurrence_group_count = scratch.query_plans_.size();
                if constexpr (p_compact_direct) {
                    batch.compact_direct_query_set_stats_ = compact_direct_query_set_stats{
                        .query_count = queries.size(),
                        .query_occurrence_count = occurrence_count,
                        .unique_hash_count = unique_occurrence_group_count,
                        .group_key_set_checksum = compact_direct_group_key_set_checksum,
                        .present = true,
                    };
                }
            } else {
                scratch.query_plan_reserve_request_ = 0u;
                unique_occurrence_group_count =
                    occurrence_layout->packed()
                        ? count_unique_occurrence_groups(scratch.packed_occurrences_)
                        : count_unique_occurrence_groups(scratch.wide_occurrences_);
                if (unique_occurrence_group_count > (std::numeric_limits<std::uint32_t>::max)() ||
                    unique_occurrence_group_count == (std::numeric_limits<std::size_t>::max)()) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native unique occurrence group count overflow");
                }
                if (occurrence_count != 0u) {
                    scratch.ensure_vector_capacity_(scratch.query_group_offsets_,
                                                    unique_occurrence_group_count + 1u);
                    scratch.query_group_offsets_.resize(unique_occurrence_group_count + 1u);
                    scratch.ensure_query_membership_filter_capacity_();
                    const auto build_group_index = [this,
                                                    &scratch,
                                                    &production_group_hashes_strictly_monotonic](
                                                       const auto& occurrences) noexcept {
                        std::size_t group_ordinal = 0u;
                        bool have_previous_hash = false;
                        std::uint32_t previous_hash = 0u;
                        bool strictly_monotonic = true;
                        for (std::size_t begin = 0u; begin < occurrences.size();) {
                            std::size_t end = begin + 1u;
                            while (end < occurrences.size() &&
                                   occurrences[end].hash == occurrences[begin].hash) {
                                ++end;
                            }
                            scratch.query_group_offsets_[group_ordinal++] =
                                static_cast<std::uint32_t>(begin);
                            const std::uint32_t hash = occurrences[begin].hash;
                            strictly_monotonic =
                                strictly_monotonic && (!have_previous_hash || previous_hash < hash);
                            previous_hash = hash;
                            have_previous_hash = true;
                            scratch.query_membership_filter_[query_membership_filter_word_(hash)] |=
                                query_membership_filter_mask_(hash);
                            begin = end;
                        }
                        scratch.query_group_offsets_[group_ordinal] =
                            static_cast<std::uint32_t>(occurrences.size());
                        production_group_hashes_strictly_monotonic =
                            strictly_monotonic && have_previous_hash;
                        return group_ordinal;
                    };
                    const std::size_t built_group_count =
                        occurrence_layout->packed() ? build_group_index(scratch.packed_occurrences_)
                                                    : build_group_index(scratch.wide_occurrences_);
                    if (built_group_count != unique_occurrence_group_count ||
                        !production_group_hashes_strictly_monotonic) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native occurrence group receipt disagrees");
                    }
                    const auto group_hash =
                        [&scratch, occurrence_layout](std::size_t group_ordinal) noexcept {
                            const std::uint32_t occurrence_ordinal =
                                scratch.query_group_offsets_[group_ordinal];
                            return occurrence_layout->packed()
                                       ? scratch.packed_occurrences_[occurrence_ordinal].hash
                                       : scratch.wide_occurrences_[occurrence_ordinal].hash;
                        };
                    std::uint32_t group_cursor = 0u;
                    for (std::uint32_t bucket = 0u; bucket <= query_radix_bucket_count_; ++bucket) {
                        while (group_cursor < unique_occurrence_group_count &&
                               query_bucket_(group_hash(group_cursor)) < bucket) {
                            ++group_cursor;
                        }
                        scratch.query_plan_radix_[bucket] = group_cursor;
                    }
                }
            }
            batch.stats_.query_plan_allocations =
                ((p_oracle || p_compact_direct) &&
                 scratch.query_plans_.capacity() > old_query_plan_capacity)
                    ? 1u
                    : 0u;
            batch.stats_.peak_query_plan_capacity_bytes =
                (p_oracle || p_compact_direct)
                    ? saturating_mul(scratch.query_plans_.capacity(), sizeof(query_hash_plan))
                    : 0u;
            batch.stats_.query_group_offset_allocations =
                use_bucket_direct_matcher &&
                        scratch.query_group_offsets_.capacity() > old_query_group_offset_capacity
                    ? 1u
                    : 0u;
            batch.stats_.peak_query_group_offset_capacity_bytes =
                use_bucket_direct_matcher
                    ? saturating_mul(scratch.query_group_offsets_.capacity(), sizeof(std::uint32_t))
                    : 0u;
            batch.stats_.query_membership_filter_allocations =
                use_bucket_direct_matcher && scratch.query_membership_filter_word_capacity_ >
                                                 old_query_membership_filter_word_capacity
                    ? 1u
                    : 0u;
            batch.stats_.peak_query_membership_filter_capacity_bytes =
                use_bucket_direct_matcher
                    ? saturating_mul(scratch.query_membership_filter_word_capacity_,
                                     sizeof(std::uint64_t))
                    : 0u;
            batch.stats_.unique_query_plans = unique_occurrence_group_count;
            batch.stats_.unique_plan_build_ns = elapsed_stage_ns_(unique_plan_started_ns);

            if constexpr (p_compact_direct) {
                prepare_compact_direct_anchors_(
                    admission, queries, static_cast<std::uint32_t>(max_occ), scratch, batch);
            } else if (occurrence_count != 0u) {
                const std::size_t radix_size =
                    static_cast<std::size_t>(query_radix_bucket_count_) + 1u;
                if constexpr (p_oracle) {
                    const std::uint64_t query_radix_fill_started_ns = stage_now_ns_();
                    const std::size_t old_query_radix_capacity =
                        scratch.query_plan_radix_.capacity();
                    scratch.ensure_vector_capacity_(scratch.query_plan_radix_, radix_size);
                    batch.stats_.query_radix_allocations =
                        scratch.query_plan_radix_.capacity() > old_query_radix_capacity ? 1u : 0u;
                    batch.stats_.peak_query_radix_capacity_bytes =
                        saturating_mul(scratch.query_plan_radix_.capacity(), sizeof(std::uint32_t));
                    scratch.query_plan_radix_.resize(radix_size);
                    std::uint32_t plan_cursor = 0u;
                    const std::uint32_t radix_item_count =
                        static_cast<std::uint32_t>(scratch.query_plans_.size());
                    for (std::uint32_t bucket = 0u; bucket <= query_radix_bucket_count_; ++bucket) {
                        while (plan_cursor < radix_item_count &&
                               query_bucket_(scratch.query_plans_[plan_cursor].hash) < bucket) {
                            ++plan_cursor;
                        }
                        scratch.query_plan_radix_[bucket] = plan_cursor;
                    }
                    batch.stats_.query_plan_radix_fill_ns =
                        elapsed_stage_ns_(query_radix_fill_started_ns);
                }
                const std::size_t old_occurrence_count_capacity =
                    scratch.occurrence_count_capacity_;
                scratch.ensure_occurrence_count_capacity_(p_oracle ? scratch.query_plans_.size()
                                                                   : unique_occurrence_group_count);
                batch.stats_.occurrence_count_allocations =
                    scratch.occurrence_count_capacity_ > old_occurrence_count_capacity ? 1u : 0u;
                batch.stats_.peak_occurrence_count_capacity_bytes = saturating_mul(
                    scratch.occurrence_count_capacity_, sizeof(std::atomic<std::uint32_t>));
                scratch.bucket_direct_matcher_active_ = use_bucket_direct_matcher;

                bucket_direct_matcher_view bucket_direct_matcher_storage{};
                const bucket_direct_matcher_view* bucket_direct_matcher = nullptr;
                if constexpr (!p_oracle) {
                    const bool route_storage_matches =
                        occurrence_layout->packed()
                            ? scratch.packed_occurrences_.size() == occurrence_count &&
                                  scratch.wide_occurrences_.empty() &&
                                  scratch.packed_occurrences_.data() != nullptr
                            : scratch.wide_occurrences_.size() == occurrence_count &&
                                  scratch.packed_occurrences_.empty() &&
                                  scratch.wide_occurrences_.data() != nullptr;
                    if (!production_group_hashes_strictly_monotonic ||
                        !valid_query_occurrence_packing_layout(scratch.occurrence_layout_) ||
                        scratch.occurrence_layout_ != *occurrence_layout ||
                        !route_storage_matches || scratch.query_membership_filter_ == nullptr ||
                        scratch.query_membership_filter_word_capacity_ !=
                            query_wave_scratch::query_membership_filter_word_count_ ||
                        scratch.query_plan_radix_.size() != radix_size ||
                        scratch.query_plan_radix_.data() == nullptr ||
                        scratch.query_plan_radix_.front() != 0u ||
                        scratch.query_plan_radix_.back() != unique_occurrence_group_count ||
                        scratch.query_group_offsets_.size() != unique_occurrence_group_count + 1u ||
                        scratch.query_group_offsets_.data() == nullptr ||
                        scratch.query_group_offsets_.front() != 0u ||
                        scratch.query_group_offsets_.back() != occurrence_count ||
                        scratch.active_occurrence_count_ != unique_occurrence_group_count ||
                        scratch.occurrence_count_capacity_ < unique_occurrence_group_count ||
                        scratch.occurrence_counts_ == nullptr) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native production matcher view receipt disagrees");
                    }
                    bucket_direct_matcher_storage = bucket_direct_matcher_view{
                        .membership_filter = scratch.query_membership_filter_.get(),
                        .radix = scratch.query_plan_radix_.data(),
                        .group_offsets = scratch.query_group_offsets_.data(),
                        .packed_occurrences = scratch.packed_occurrences_.data(),
                        .wide_occurrences = scratch.wide_occurrences_.data(),
                        .occurrence_counts = scratch.occurrence_counts_.get(),
                        .membership_filter_word_count =
                            scratch.query_membership_filter_word_capacity_,
                        .radix_count = scratch.query_plan_radix_.size(),
                        .group_offset_count = scratch.query_group_offsets_.size(),
                        .packed_occurrence_count = scratch.packed_occurrences_.size(),
                        .wide_occurrence_count = scratch.wide_occurrences_.size(),
                        .occurrence_count = occurrence_count,
                        .counter_count = scratch.active_occurrence_count_,
                        .occurrence_layout = scratch.occurrence_layout_,
                    };
                    bucket_direct_matcher = &bucket_direct_matcher_storage;
                    batch.stats_.bucket_direct_matcher_view_receipts = 1u;
                }

                const std::uint32_t max_occ_u32 = static_cast<std::uint32_t>(max_occ);
                std::atomic<std::uint32_t> first_error{0u};

                // Pass 1: controls and saturated counts only.
                const std::uint64_t control_count_started_ns = stage_now_ns_();
                parallel_entries_(
                    block_count_,
                    [this,
                     &scratch,
                     &first_error,
                     max_occ_u32,
                     use_fixed_h32_c16_p16,
                     bucket_direct_matcher](std::size_t block_begin,
                                            std::size_t block_end) noexcept {
                        const std::span<const query_hash_plan> plans{scratch.query_plans_};
                        const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                        for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                             ++block_ordinal) {
                            if (first_error.load(std::memory_order_relaxed) != 0u) {
                                return;
                            }
                            std::expected<void, minimizer_generation_encoding_error> counted{};
                            fragment_probe_context context{
                                .pass = fragment_probe_pass::control_count,
                                .block = &block_plans_[block_ordinal],
                                .block_ordinal = block_ordinal,
                                .plans = plans,
                                .query_plan_radix = radix,
                                .scratch = &scratch,
                                .bucket_direct_matcher = bucket_direct_matcher,
                                .max_occ = max_occ_u32,
                            };
                            if (use_fixed_h32_c16_p16) {
                                counted = probe_block_h32_c16_p16_<p_oracle>(context);
                            } else {
                                counted = visit_matched_control_counts_<p_oracle>(
                                    block_plans_[block_ordinal],
                                    context,
                                    [&context, max_occ_u32](const query_occurrence_group& group,
                                                            std::uint64_t local_count) noexcept
                                        -> std::optional<minimizer_generation_encoding_error> {
                                        (void)reserve_query_occurrences_(
                                            occurrence_counter_<p_oracle>(context,
                                                                          group.counter_ordinal),
                                            local_count,
                                            max_occ_u32);
                                        return std::nullopt;
                                    });
                            }
                            if (!counted) {
                                record_error_(first_error, counted.error());
                                return;
                            }
                        }
                    });
                if (const auto error = recorded_error_(first_error)) {
                    throw persisted_minimizer_lookup_error(
                        "fragment-native control-count pass failed; wire_error=" +
                        std::to_string(static_cast<unsigned>(*error)));
                }
                batch.stats_.control_count_pass_ns = elapsed_stage_ns_(control_count_started_ns);
                batch.stats_.control_count_bytes = total_control_count_scan_bytes_;

                bool have_matched_controls = false;
                fragment_probe_context counted_group_context{
                    .scratch = &scratch,
                    .bucket_direct_matcher = bucket_direct_matcher,
                };
                const auto consume_counted_group = [this,
                                                    &batch,
                                                    &counted_group_context,
                                                    queries,
                                                    max_occ_u32,
                                                    &have_matched_controls](
                                                       const query_occurrence_group& group) {
                    const std::uint32_t global_count =
                        occurrence_counter_<p_oracle>(counted_group_context, group.counter_ordinal)
                            .load(std::memory_order_relaxed);
                    have_matched_controls = have_matched_controls || global_count != 0u;
                    if (global_count == 0u || global_count > max_occ_u32) {
                        return;
                    }
                    const auto occurrence_error = with_query_group_occurrences_<p_oracle>(
                        counted_group_context,
                        group,
                        [this, &batch, queries, global_count](
                            const auto occurrences,
                            const query_occurrence_packing_layout& layout) noexcept
                            -> std::optional<minimizer_generation_encoding_error> {
                            for (const auto& occurrence : occurrences) {
                                const query_occurrence_reference item =
                                    query_occurrence_reference_(occurrence, layout);
                                if (const auto error = add_expected_anchor_count_(
                                        item, queries, batch, global_count)) {
                                    return error;
                                }
                            }
                            return std::nullopt;
                        });
                    if (occurrence_error) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native expected anchor count overflow");
                    }
                };
                if constexpr (p_oracle) {
                    for (std::size_t plan_ordinal = 0u; plan_ordinal < scratch.query_plans_.size();
                         ++plan_ordinal) {
                        query_hash_plan& plan = scratch.query_plans_[plan_ordinal];
                        const query_occurrence_group group{
                            .hash = plan.hash,
                            .occurrence_ref_begin = plan.occurrence_ref_begin,
                            .occurrence_ref_count = plan.occurrence_ref_count,
                            .counter_ordinal = static_cast<std::uint32_t>(plan_ordinal),
                        };
                        plan.global_occurrence_count =
                            occurrence_counter_<p_oracle>(counted_group_context,
                                                          static_cast<std::uint32_t>(plan_ordinal))
                                .load(std::memory_order_relaxed);
                        consume_counted_group(group);
                    }
                } else {
                    DEBUG_ASSERT(bucket_direct_matcher != nullptr,
                                 "production matcher view is prevalidated");
                    const bucket_direct_matcher_view& view = *bucket_direct_matcher;
                    const auto consume_indexed_groups =
                        [&view, &consume_counted_group](const auto* occurrences,
                                                        std::size_t occurrence_count) {
                            for (std::size_t group_ordinal = 0u;
                                 group_ordinal + 1u < view.group_offset_count;
                                 ++group_ordinal) {
                                const std::uint32_t begin = view.group_offsets[group_ordinal];
                                const std::uint32_t end = view.group_offsets[group_ordinal + 1u];
                                DEBUG_ASSERT(end > begin && end <= occurrence_count,
                                             "production group extent is prevalidated");
                                consume_counted_group(query_occurrence_group{
                                    .hash = occurrences[begin].hash,
                                    .occurrence_ref_begin = begin,
                                    .occurrence_ref_count = end - begin,
                                    .counter_ordinal = static_cast<std::uint32_t>(group_ordinal),
                                });
                            }
                        };
                    if (view.occurrence_layout.packed()) {
                        consume_indexed_groups(view.packed_occurrences,
                                               view.packed_occurrence_count);
                    } else {
                        consume_indexed_groups(view.wide_occurrences, view.wide_occurrence_count);
                    }
                }

                const bool have_accepted_anchors = std::ranges::any_of(
                    std::span<const prepared_query_anchors>{batch.queries_.get(),
                                                            batch.query_count_},
                    [](const prepared_query_anchors& query) noexcept {
                        return query.anchor_count != 0u;
                    });
                if (!have_accepted_anchors) {
                    // Matched heavy controls still require prefix/count
                    // authentication, but no layout, position walk, final
                    // allocation, emission pass, or sort is needed.
                    if (have_matched_controls) {
                        const std::uint64_t rejected_validation_started_ns = stage_now_ns_();
                        first_error.store(0u, std::memory_order_relaxed);
                        std::atomic<std::uint64_t> rejected_prefix_bytes{0u};
                        std::atomic<std::uint64_t> rejected_matched_count_bytes{0u};
                        parallel_entries_(
                            block_count_,
                            [this,
                             &scratch,
                             &first_error,
                             &rejected_prefix_bytes,
                             &rejected_matched_count_bytes,
                             max_occ_u32,
                             use_fixed_h32_c16_p16,
                             bucket_direct_matcher](std::size_t block_begin,
                                                    std::size_t block_end) noexcept {
                                const std::span<const query_hash_plan> plans{scratch.query_plans_};
                                const std::span<const std::uint32_t> radix{
                                    scratch.query_plan_radix_};
                                for (std::size_t block_ordinal = block_begin;
                                     block_ordinal < block_end;
                                     ++block_ordinal) {
                                    if (first_error.load(std::memory_order_relaxed) != 0u) {
                                        return;
                                    }
                                    std::uint64_t block_matched_count_bytes = 0u;
                                    std::uint64_t block_prefix_bytes = 0u;
                                    std::expected<void, minimizer_generation_encoding_error>
                                        validated{};
                                    fragment_probe_context context{
                                        .pass = fragment_probe_pass::rejected_validation,
                                        .block = &block_plans_[block_ordinal],
                                        .block_ordinal = block_ordinal,
                                        .plans = plans,
                                        .query_plan_radix = radix,
                                        .scratch = &scratch,
                                        .bucket_direct_matcher = bucket_direct_matcher,
                                        .max_occ = max_occ_u32,
                                        .matched_count_bytes = &block_matched_count_bytes,
                                        .rejected_prefix_bytes = &block_prefix_bytes,
                                    };
                                    if (use_fixed_h32_c16_p16) {
                                        validated = probe_block_h32_c16_p16_<p_oracle>(context);
                                    } else {
                                        validated = visit_matched_controls_<p_oracle>(
                                            block_plans_[block_ordinal],
                                            context,
                                            [](std::size_t,
                                               const query_occurrence_group&,
                                               const grouped_soa_decoded_control&) noexcept
                                                -> std::optional<
                                                    minimizer_generation_encoding_error> {
                                                return minimizer_generation_encoding_error::
                                                    partition_id;
                                            },
                                            nullptr,
                                            &block_prefix_bytes);
                                    }
                                    if (!validated) {
                                        record_error_(first_error, validated.error());
                                        return;
                                    }
                                    if (!add_atomic_checked_(rejected_matched_count_bytes,
                                                             block_matched_count_bytes) ||
                                        !add_atomic_checked_(rejected_prefix_bytes,
                                                             block_prefix_bytes)) {
                                        record_error_(first_error,
                                                      minimizer_generation_encoding_error::
                                                          arithmetic_overflow);
                                        return;
                                    }
                                }
                            });
                        if (const auto error = recorded_error_(first_error)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native rejected-control validation failed; "
                                "wire_error=" +
                                std::to_string(static_cast<unsigned>(*error)));
                        }
                        batch.stats_.rejected_control_validation_ns =
                            elapsed_stage_ns_(rejected_validation_started_ns);
                        batch.stats_.rejected_control_validation_bytes =
                            use_fixed_h32_c16_p16 ? total_control_hash_scan_bytes_
                                                  : total_control_count_scan_bytes_;
                        if (!add_checked_(
                                batch.stats_.rejected_control_validation_bytes,
                                rejected_matched_count_bytes.load(std::memory_order_relaxed)) ||
                            !add_checked_(batch.stats_.rejected_control_validation_bytes,
                                          rejected_prefix_bytes.load(std::memory_order_relaxed))) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native rejected-control byte "
                                "accounting overflow");
                        }
                    }
                } else {

                    // Layout state is activated only after Pass 1 establishes the
                    // exact accepted anchor counts. This keeps failed count waves
                    // from publishing/zeroing partially active layout owners.
                    const std::uint64_t orientation_layout_started_ns = stage_now_ns_();
                    if (queries.size() != 0u &&
                        block_count_ > (std::numeric_limits<std::size_t>::max)() / queries.size()) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native block/query layout overflow");
                    }
                    const std::size_t layout_count = block_count_ * queries.size();
                    const std::size_t old_block_layout_capacity =
                        scratch.block_query_layouts_.capacity();
                    scratch.ensure_vector_capacity_(scratch.block_query_layouts_, layout_count);
                    batch.stats_.block_layout_allocations =
                        scratch.block_query_layouts_.capacity() > old_block_layout_capacity ? 1u
                                                                                            : 0u;
                    batch.stats_.peak_block_layout_capacity_bytes = saturating_mul(
                        scratch.block_query_layouts_.capacity(), sizeof(block_query_anchor_layout));
                    scratch.block_query_layouts_.resize(layout_count);
                    std::fill(scratch.block_query_layouts_.begin(),
                              scratch.block_query_layouts_.end(),
                              block_query_anchor_layout{});
                    const std::size_t old_block_stats_capacity = scratch.block_stats_.capacity();
                    scratch.ensure_vector_capacity_(scratch.block_stats_, block_count_);
                    batch.stats_.block_stats_allocations =
                        scratch.block_stats_.capacity() > old_block_stats_capacity ? 1u : 0u;
                    batch.stats_.peak_block_stats_capacity_bytes = saturating_mul(
                        scratch.block_stats_.capacity(), sizeof(fragment_block_wave_stats));
                    scratch.block_stats_.resize(block_count_);
                    std::fill(scratch.block_stats_.begin(),
                              scratch.block_stats_.end(),
                              fragment_block_wave_stats{});

                    // Pass 2: accepted positions only, counting orientation.
                    first_error.store(0u, std::memory_order_relaxed);
                    parallel_entries_(
                        block_count_,
                        [this,
                         queries,
                         &scratch,
                         &first_error,
                         max_occ_u32,
                         use_fixed_h32_c16_p16,
                         bucket_direct_matcher](std::size_t block_begin,
                                                std::size_t block_end) noexcept {
                            const std::span<const query_hash_plan> plans{scratch.query_plans_};
                            const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                            for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                                 ++block_ordinal) {
                                if (first_error.load(std::memory_order_relaxed) != 0u) {
                                    return;
                                }
                                std::uint64_t block_matched_count_bytes = 0u;
                                std::uint64_t block_accepted_prefix_bytes = 0u;
                                std::uint64_t block_rejected_prefix_bytes = 0u;
                                std::uint64_t block_position_bytes = 0u;
                                std::expected<void, minimizer_generation_encoding_error> counted{};
                                fragment_probe_context context{
                                    .pass = fragment_probe_pass::orientation,
                                    .block = &block_plans_[block_ordinal],
                                    .block_ordinal = block_ordinal,
                                    .plans = plans,
                                    .query_plan_radix = radix,
                                    .queries = queries,
                                    .scratch = &scratch,
                                    .bucket_direct_matcher = bucket_direct_matcher,
                                    .max_occ = max_occ_u32,
                                    .matched_count_bytes = &block_matched_count_bytes,
                                    .accepted_prefix_bytes = &block_accepted_prefix_bytes,
                                    .rejected_prefix_bytes = &block_rejected_prefix_bytes,
                                    .position_bytes = &block_position_bytes,
                                };
                                if (use_fixed_h32_c16_p16) {
                                    counted = probe_block_h32_c16_p16_<p_oracle>(context);
                                } else {
                                    counted = visit_matched_controls_<p_oracle>(
                                        block_plans_[block_ordinal],
                                        context,
                                        [this,
                                         queries,
                                         &scratch,
                                         block_ordinal,
                                         &block_position_bytes,
                                         &context](
                                            std::size_t entry_ordinal,
                                            const query_occurrence_group& group,
                                            const grouped_soa_decoded_control& control) noexcept
                                            -> std::optional<minimizer_generation_encoding_error> {
                                            const entry_plan& planned_entry =
                                                entry_plans_[entry_ordinal];
                                            auto position_cursor = position_range_cursor::create(
                                                mapped_payload_, control, planned_entry);
                                            if (!position_cursor) {
                                                return position_cursor.error();
                                            }
                                            std::uint64_t target_strand_zero = 0u;
                                            std::uint64_t target_strand_one = 0u;
                                            std::uint64_t packed = 0u;
                                            while (position_cursor->next_dynamic(packed)) {
                                                std::uint64_t& count = (packed & 1u) == 0u
                                                                           ? target_strand_zero
                                                                           : target_strand_one;
                                                if (count ==
                                                    (std::numeric_limits<std::uint64_t>::max)()) {
                                                    return minimizer_generation_encoding_error::
                                                        arithmetic_overflow;
                                                }
                                                ++count;
                                            }
                                            if (const auto error = position_cursor->error()) {
                                                return error;
                                            }
                                            std::uint64_t observed = 0u;
                                            if (!add_checked_(observed, target_strand_zero) ||
                                                !add_checked_(observed, target_strand_one) ||
                                                observed !=
                                                    control.position_end - control.position_begin) {
                                                return minimizer_generation_encoding_error::
                                                    arithmetic_overflow;
                                            }
                                            const std::uint64_t position_bytes =
                                                bioseq::common::saturating_mul(
                                                    observed, planned_entry.position_byte_width);
                                            if (position_bytes ==
                                                    (std::numeric_limits<std::uint64_t>::max)() ||
                                                !add_checked_(block_position_bytes,
                                                              position_bytes)) {
                                                return minimizer_generation_encoding_error::
                                                    arithmetic_overflow;
                                            }

                                            const orientation_occurrence_context occurrence_context{
                                                .queries = queries,
                                                .scratch = &scratch,
                                                .layout_base = block_ordinal * queries.size(),
                                                .target_strand_zero = target_strand_zero,
                                                .target_strand_one = target_strand_one,
                                            };
                                            return with_query_group_occurrences_<p_oracle>(
                                                context,
                                                group,
                                                [&occurrence_context](
                                                    const auto occurrences,
                                                    const query_occurrence_packing_layout&
                                                        packing_layout) noexcept
                                                    -> std::optional<
                                                        minimizer_generation_encoding_error> {
                                                    for (const auto& occurrence : occurrences) {
                                                        const query_occurrence_reference item =
                                                            query_occurrence_reference_(
                                                                occurrence, packing_layout);
                                                        if (const auto error =
                                                                accumulate_orientation_(
                                                                    item, occurrence_context)) {
                                                            return error;
                                                        }
                                                    }
                                                    return std::nullopt;
                                                });
                                        },
                                        &block_accepted_prefix_bytes,
                                        &block_rejected_prefix_bytes);
                                }
                                if (!counted) {
                                    record_error_(first_error, counted.error());
                                    return;
                                }
                                if (block_ordinal >= scratch.block_stats_.size()) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::arithmetic_overflow);
                                    return;
                                }
                                scratch.block_stats_[block_ordinal].pass2_matched_count_bytes =
                                    block_matched_count_bytes;
                                scratch.block_stats_[block_ordinal].pass2_accepted_prefix_bytes =
                                    block_accepted_prefix_bytes;
                                scratch.block_stats_[block_ordinal].pass2_rejected_prefix_bytes =
                                    block_rejected_prefix_bytes;
                                scratch.block_stats_[block_ordinal].pass2_position_bytes =
                                    block_position_bytes;
                            }
                        });
                    if (const auto error = recorded_error_(first_error)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native orientation-layout pass failed; "
                            "wire_error=" +
                            std::to_string(static_cast<unsigned>(*error)));
                    }

                    // Prefix [all forward blocks][all reverse blocks].
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        std::uint64_t cursor = 0u;
                        for (std::size_t block_ordinal = 0u; block_ordinal < block_count_;
                             ++block_ordinal) {
                            block_query_anchor_layout& layout =
                                scratch.block_query_layouts_[block_ordinal * queries.size() +
                                                             query_ordinal];
                            layout.forward_begin = cursor;
                            layout.forward_cursor = cursor;
                            if (!add_checked_(cursor, layout.forward_count)) {
                                throw persisted_minimizer_lookup_error(
                                    "fragment-native forward anchor prefix overflow");
                            }
                        }
                        for (std::size_t block_ordinal = 0u; block_ordinal < block_count_;
                             ++block_ordinal) {
                            block_query_anchor_layout& layout =
                                scratch.block_query_layouts_[block_ordinal * queries.size() +
                                                             query_ordinal];
                            layout.reverse_begin = cursor;
                            layout.reverse_cursor = cursor;
                            if (!add_checked_(cursor, layout.reverse_count)) {
                                throw persisted_minimizer_lookup_error(
                                    "fragment-native reverse anchor prefix overflow");
                            }
                        }
                        if (cursor != batch.queries_[query_ordinal].anchor_count ||
                            cursor > (std::numeric_limits<std::size_t>::max)()) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native anchor layout count disagrees");
                        }
                    }

                    std::uint64_t final_owned_bytes = batch.owned_bytes_;
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        const prepared_query_anchors& query = batch.queries_[query_ordinal];
                        const std::uint64_t anchor_bytes =
                            saturating_mul(query.anchor_count, sizeof(bioseq::chaining::anchor));
                        final_owned_bytes = saturating_add(final_owned_bytes, anchor_bytes);
                        if (anchor_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                            final_owned_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                            query.anchor_count > (std::numeric_limits<std::size_t>::max)() /
                                                     sizeof(bioseq::chaining::anchor)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native exact anchor size overflow");
                        }
                    }
                    // Reserve the complete immutable result before allocating its
                    // first per-query array. A later query can therefore never
                    // discover an admission failure after earlier anchors exist.
                    if (!batch.permit_.grow_to(final_owned_bytes)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native exact anchor admission failed");
                    }
                    try {
                        for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                             ++query_ordinal) {
                            prepared_query_anchors& query = batch.queries_[query_ordinal];
                            if (query.anchor_count == 0u)
                                continue;
                            query.anchors =
                                std::make_unique_for_overwrite<bioseq::chaining::anchor[]>(
                                    static_cast<std::size_t>(query.anchor_count));
                        }
                    } catch (const std::bad_alloc&) {
                        // Batch destruction releases every earlier query array
                        // before returning the aggregate permit.
                        throw persisted_minimizer_lookup_error(
                            "fragment-native exact anchor allocation failed");
                    }
                    batch.owned_bytes_ = final_owned_bytes;
                    batch.permit_.shrink_to(batch.owned_bytes_);
                    batch.stats_.orientation_layout_ns =
                        elapsed_stage_ns_(orientation_layout_started_ns);

                    // Pass 3: direct exact emission. Primary and tie sorting use
                    // separate joined fragment barriers below so their wall times
                    // remain independently attributable.
                    const std::uint64_t direct_emission_started_ns = stage_now_ns_();
                    first_error.store(0u, std::memory_order_relaxed);
                    parallel_entries_(
                        block_count_,
                        [this,
                         queries,
                         &batch,
                         &scratch,
                         &first_error,
                         max_occ_u32,
                         use_fixed_h32_c16_p16,
                         bucket_direct_matcher](std::size_t block_begin,
                                                std::size_t block_end) noexcept {
                            const std::span<const query_hash_plan> plans{scratch.query_plans_};
                            const std::span<const std::uint32_t> radix{scratch.query_plan_radix_};
                            for (std::size_t block_ordinal = block_begin; block_ordinal < block_end;
                                 ++block_ordinal) {
                                if (first_error.load(std::memory_order_relaxed) != 0u) {
                                    return;
                                }
                                const fragment_block_plan& block = block_plans_[block_ordinal];
                                std::uint64_t block_matched_count_bytes = 0u;
                                std::uint64_t block_accepted_prefix_bytes = 0u;
                                std::uint64_t block_position_bytes = 0u;
                                std::uint64_t block_anchors_constructed = 0u;
                                std::expected<void, minimizer_generation_encoding_error> emitted{};
                                fragment_probe_context context{
                                    .pass = fragment_probe_pass::emission,
                                    .block = &block,
                                    .block_ordinal = block_ordinal,
                                    .plans = plans,
                                    .query_plan_radix = radix,
                                    .queries = queries,
                                    .scratch = &scratch,
                                    .bucket_direct_matcher = bucket_direct_matcher,
                                    .batch = &batch,
                                    .max_occ = max_occ_u32,
                                    .matched_count_bytes = &block_matched_count_bytes,
                                    .accepted_prefix_bytes = &block_accepted_prefix_bytes,
                                    .position_bytes = &block_position_bytes,
                                    .anchors_constructed = &block_anchors_constructed,
                                };
                                if (use_fixed_h32_c16_p16) {
                                    emitted = probe_block_h32_c16_p16_<p_oracle>(context);
                                } else {
                                    emitted = visit_matched_controls_<p_oracle>(
                                        block,
                                        context,
                                        [this,
                                         queries,
                                         &batch,
                                         &scratch,
                                         block_ordinal,
                                         &block,
                                         &block_position_bytes,
                                         &block_anchors_constructed,
                                         &context](
                                            std::size_t entry_ordinal,
                                            const query_occurrence_group& group,
                                            const grouped_soa_decoded_control& control) noexcept
                                            -> std::optional<minimizer_generation_encoding_error> {
                                            const entry_plan& planned_entry =
                                                entry_plans_[entry_ordinal];
                                            auto position_cursor = position_range_cursor::create(
                                                mapped_payload_, control, planned_entry);
                                            if (!position_cursor) {
                                                return position_cursor.error();
                                            }
                                            const direct_emission_occurrence_context
                                                occurrence_context{
                                                    .queries = queries,
                                                    .block = &block,
                                                    .block_ordinal = block_ordinal,
                                                    .scratch = &scratch,
                                                    .batch = &batch,
                                                };
                                            const auto occurrence_error =
                                                with_query_group_occurrences_<p_oracle>(
                                                    context,
                                                    group,
                                                    [this, &position_cursor, &occurrence_context](
                                                        const auto occurrences,
                                                        const query_occurrence_packing_layout&
                                                            packing_layout) noexcept
                                                        -> std::optional<
                                                            minimizer_generation_encoding_error> {
                                                        std::optional<
                                                            minimizer_generation_encoding_error>
                                                            emit_error;
                                                        std::uint64_t packed = 0u;
                                                        while (
                                                            position_cursor->next_dynamic(packed)) {
                                                            for (const auto& occurrence :
                                                                 occurrences) {
                                                                const query_occurrence_reference
                                                                    item =
                                                                        query_occurrence_reference_(
                                                                            occurrence,
                                                                            packing_layout);
                                                                if (const auto error =
                                                                        emit_fragment_anchor_(
                                                                            packed,
                                                                            item,
                                                                            occurrence_context)) {
                                                                    emit_error = error;
                                                                    break;
                                                                }
                                                            }
                                                            if (emit_error)
                                                                break;
                                                        }
                                                        if (const auto error =
                                                                position_cursor->error()) {
                                                            return error;
                                                        }
                                                        return emit_error;
                                                    });
                                            if (occurrence_error) {
                                                return occurrence_error;
                                            }
                                            const std::uint64_t decoded_positions =
                                                control.position_end - control.position_begin;
                                            const std::uint64_t position_bytes =
                                                bioseq::common::saturating_mul(
                                                    decoded_positions,
                                                    planned_entry.position_byte_width);
                                            const std::uint64_t constructed =
                                                bioseq::common::saturating_mul(
                                                    decoded_positions, group.occurrence_ref_count);
                                            if (position_bytes ==
                                                    (std::numeric_limits<std::uint64_t>::max)() ||
                                                constructed ==
                                                    (std::numeric_limits<std::uint64_t>::max)() ||
                                                !add_checked_(block_position_bytes,
                                                              position_bytes) ||
                                                !add_checked_(block_anchors_constructed,
                                                              constructed)) {
                                                return minimizer_generation_encoding_error::
                                                    arithmetic_overflow;
                                            }
                                            return std::nullopt;
                                        },
                                        &block_accepted_prefix_bytes,
                                        nullptr,
                                        /*validate_rejected_prefixes=*/false);
                                }
                                if (!emitted) {
                                    record_error_(first_error, emitted.error());
                                    return;
                                }
                                if (block_ordinal >= scratch.block_stats_.size()) {
                                    record_error_(
                                        first_error,
                                        minimizer_generation_encoding_error::arithmetic_overflow);
                                    return;
                                }
                                scratch.block_stats_[block_ordinal].pass3_matched_count_bytes =
                                    block_matched_count_bytes;
                                scratch.block_stats_[block_ordinal].pass3_accepted_prefix_bytes =
                                    block_accepted_prefix_bytes;
                                scratch.block_stats_[block_ordinal].pass3_position_bytes =
                                    block_position_bytes;
                                scratch.block_stats_[block_ordinal].anchors_constructed =
                                    block_anchors_constructed;

                                for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                                     ++query_ordinal) {
                                    block_query_anchor_layout& layout =
                                        scratch
                                            .block_query_layouts_[block_ordinal * queries.size() +
                                                                  query_ordinal];
                                    if (layout.forward_begin >
                                            (std::numeric_limits<std::uint64_t>::max)() -
                                                layout.forward_count ||
                                        layout.reverse_begin >
                                            (std::numeric_limits<std::uint64_t>::max)() -
                                                layout.reverse_count ||
                                        layout.forward_cursor !=
                                            layout.forward_begin + layout.forward_count ||
                                        layout.reverse_cursor !=
                                            layout.reverse_begin + layout.reverse_count) {
                                        record_error_(first_error,
                                                      minimizer_generation_encoding_error::
                                                          arithmetic_overflow);
                                        return;
                                    }
                                }
                            }
                        });
                    if (const auto error = recorded_error_(first_error)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native direct anchor emission failed; "
                            "wire_error=" +
                            std::to_string(static_cast<unsigned>(*error)));
                    }
                    batch.stats_.direct_emission_ns = elapsed_stage_ns_(direct_emission_started_ns);

                    const auto run_local_order_stage = [this,
                                                        queries,
                                                        &batch,
                                                        &scratch,
                                                        &first_error](bool normalize_ties) {
                        first_error.store(0u, std::memory_order_relaxed);
                        parallel_entries_(
                            block_count_,
                            [queries,
                             &batch,
                             &scratch,
                             &first_error,
                             normalize_ties,
                             authenticated_target_length = manifest_.target_length](
                                std::size_t block_begin, std::size_t block_end) noexcept {
                                for (std::size_t block_ordinal = block_begin;
                                     block_ordinal < block_end;
                                     ++block_ordinal) {
                                    if (first_error.load(std::memory_order_relaxed) != 0u ||
                                        block_ordinal >= scratch.block_stats_.size()) {
                                        if (block_ordinal >= scratch.block_stats_.size()) {
                                            record_error_(first_error,
                                                          minimizer_generation_encoding_error::
                                                              arithmetic_overflow);
                                        }
                                        return;
                                    }
                                    std::uint64_t block_order_bytes = 0u;
                                    for (std::size_t query_ordinal = 0u;
                                         query_ordinal < queries.size();
                                         ++query_ordinal) {
                                        const block_query_anchor_layout& layout =
                                            scratch.block_query_layouts_[block_ordinal *
                                                                             queries.size() +
                                                                         query_ordinal];
                                        auto& query = batch.queries_[query_ordinal];
                                        const auto order_slice =
                                            [normalize_ties,
                                             &block_order_bytes,
                                             authenticated_target_length](
                                                bioseq::chaining::anchor* anchors,
                                                std::uint64_t count) noexcept -> bool {
                                            if (count < 2u)
                                                return true;
                                            const auto stats =
                                                normalize_ties
                                                    ? normalize_anchor_slice_ties_(
                                                          anchors, static_cast<std::size_t>(count))
                                                    : sort_anchor_slice_primary_(
                                                          anchors,
                                                          static_cast<std::size_t>(count),
                                                          authenticated_target_length);
                                            const std::uint64_t bytes = normalize_ties
                                                                            ? stats.tie_bytes
                                                                            : stats.primary_bytes;
                                            return !stats.saturated &&
                                                   add_checked_(block_order_bytes, bytes);
                                        };
                                        if ((layout.forward_count > 1u &&
                                             !order_slice(
                                                 query.anchors.get() +
                                                     static_cast<std::size_t>(layout.forward_begin),
                                                 layout.forward_count)) ||
                                            (layout.reverse_count > 1u &&
                                             !order_slice(
                                                 query.anchors.get() +
                                                     static_cast<std::size_t>(layout.reverse_begin),
                                                 layout.reverse_count))) {
                                            record_error_(first_error,
                                                          minimizer_generation_encoding_error::
                                                              arithmetic_overflow);
                                            return;
                                        }
                                    }
                                    fragment_block_wave_stats& block_stats =
                                        scratch.block_stats_[block_ordinal];
                                    if (normalize_ties) {
                                        block_stats.tie_normalization_bytes = block_order_bytes;
                                    } else {
                                        block_stats.local_sort_bytes = block_order_bytes;
                                    }
                                }
                            });
                        if (const auto error = recorded_error_(first_error)) {
                            throw persisted_minimizer_lookup_error(
                                std::string{normalize_ties
                                                ? "fragment-native tie normalization failed; "
                                                : "fragment-native local primary sort failed; "} +
                                "wire_error=" + std::to_string(static_cast<unsigned>(*error)));
                        }
                    };

                    const std::uint64_t local_sort_started_ns = stage_now_ns_();
                    run_local_order_stage(/*normalize_ties=*/false);
                    batch.stats_.local_sort_ns = elapsed_stage_ns_(local_sort_started_ns);
                    const std::uint64_t tie_normalization_started_ns = stage_now_ns_();
                    run_local_order_stage(/*normalize_ties=*/true);
                    batch.stats_.tie_normalization_ns =
                        elapsed_stage_ns_(tie_normalization_started_ns);

                    std::uint64_t expected_constructed = 0u;
                    for (std::size_t query_ordinal = 0u; query_ordinal < queries.size();
                         ++query_ordinal) {
                        if (!add_checked_(expected_constructed,
                                          batch.queries_[query_ordinal].anchor_count)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native stats anchor total overflow");
                        }
                    }
                    batch.stats_.pass2_control_bytes = use_fixed_h32_c16_p16
                                                           ? total_control_hash_scan_bytes_
                                                           : total_control_count_scan_bytes_;
                    batch.stats_.pass3_control_bytes = use_fixed_h32_c16_p16
                                                           ? total_control_hash_scan_bytes_
                                                           : total_control_count_scan_bytes_;
                    for (const fragment_block_wave_stats& block_stats : scratch.block_stats_) {
                        if (!add_checked_(batch.stats_.pass2_control_bytes,
                                          block_stats.pass2_matched_count_bytes) ||
                            !add_checked_(batch.stats_.pass2_accepted_prefix_bytes,
                                          block_stats.pass2_accepted_prefix_bytes) ||
                            !add_checked_(batch.stats_.rejected_control_validation_bytes,
                                          block_stats.pass2_rejected_prefix_bytes) ||
                            !add_checked_(batch.stats_.pass2_position_bytes,
                                          block_stats.pass2_position_bytes) ||
                            !add_checked_(batch.stats_.pass3_control_bytes,
                                          block_stats.pass3_matched_count_bytes) ||
                            !add_checked_(batch.stats_.pass3_accepted_prefix_bytes,
                                          block_stats.pass3_accepted_prefix_bytes) ||
                            !add_checked_(batch.stats_.pass3_position_bytes,
                                          block_stats.pass3_position_bytes) ||
                            !add_checked_(batch.stats_.anchors_constructed,
                                          block_stats.anchors_constructed) ||
                            !add_checked_(batch.stats_.local_sort_bytes,
                                          block_stats.local_sort_bytes) ||
                            !add_checked_(batch.stats_.tie_normalization_bytes,
                                          block_stats.tie_normalization_bytes)) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native stats reduction overflow");
                        }
                    }
                    const std::uint64_t exact_final_bytes =
                        saturating_mul(expected_constructed, sizeof(bioseq::chaining::anchor));
                    const bool control_byte_equation =
                        use_fixed_h32_c16_p16
                            ? batch.stats_.pass2_control_bytes >= total_control_hash_scan_bytes_ &&
                                  batch.stats_.pass2_control_bytes <=
                                      batch.stats_.control_count_bytes &&
                                  batch.stats_.pass3_control_bytes >=
                                      total_control_hash_scan_bytes_ &&
                                  batch.stats_.pass3_control_bytes <=
                                      batch.stats_.pass2_control_bytes &&
                                  (batch.stats_.pass2_control_bytes -
                                   total_control_hash_scan_bytes_) %
                                          sizeof(std::uint16_t) ==
                                      0u &&
                                  (batch.stats_.pass3_control_bytes -
                                   total_control_hash_scan_bytes_) %
                                          sizeof(std::uint16_t) ==
                                      0u
                            : batch.stats_.pass2_control_bytes ==
                                      batch.stats_.control_count_bytes &&
                                  batch.stats_.pass3_control_bytes ==
                                      batch.stats_.control_count_bytes;
                    if (!control_byte_equation ||
                        batch.stats_.pass2_accepted_prefix_bytes !=
                            batch.stats_.pass3_accepted_prefix_bytes ||
                        batch.stats_.pass2_position_bytes != batch.stats_.pass3_position_bytes ||
                        batch.stats_.anchors_constructed != expected_constructed ||
                        exact_final_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native stats pass equation failed");
                    }
                    batch.stats_.exact_final_bytes = exact_final_bytes;
                    batch.stats_.peak_exact_final_bytes = exact_final_bytes;
                }
            }

            // Compositional sortedness authority: the control-scan route fills
            // forward/reverse slices inside strictly ordered disjoint blocks;
            // the compact-direct route fills one exact forward and one exact
            // reverse slice per query. Both routes apply the same primary and
            // tie stages inside every resulting slice. canonical_anchor_less
            // encodes orientation as the ref_bits MSB, so the forward slices
            // followed by the reverse slices compose without a further O(A)
            // production traversal.
            batch.stats_.compositional_sortedness_receipts = queries.size();
#ifndef NDEBUG
            verify_compositional_order_for_testing = true;
#endif
            const auto less = bioseq::chaining::canonical_anchor_less{};
            for (std::size_t query_ordinal = 0u; query_ordinal < queries.size(); ++query_ordinal) {
                prepared_query_anchors& query = batch.queries_[query_ordinal];
                if (verify_compositional_order_for_testing) {
                    const std::uint64_t verification_bytes =
                        saturating_mul(query.anchor_count, sizeof(bioseq::chaining::anchor));
                    if (verification_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
                        !add_checked_(batch.stats_.final_sortedness_validation_bytes,
                                      verification_bytes)) {
                        throw persisted_minimizer_lookup_error(
                            "fragment-native anchor order verification overflow");
                    }
                    for (std::size_t anchor_ordinal = 1u;
                         anchor_ordinal < static_cast<std::size_t>(query.anchor_count);
                         ++anchor_ordinal) {
                        if (less(query.anchors[anchor_ordinal],
                                 query.anchors[anchor_ordinal - 1u])) {
                            throw persisted_minimizer_lookup_error(
                                "fragment-native anchor order stats failed");
                        }
                    }
                }
                query.sortedness_receipt = prepared_fragment_anchor_batch::sortedness_receipt_;
            }
            if (batch.owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)() ||
                batch.owned_bytes_ > batch.permit_.bytes()) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native anchor ownership stats failed");
            }
            if (!batch.stats_.forbidden_buffers_are_zero() ||
                batch.stats_.exact_final_bytes > batch.owned_bytes_) {
                throw persisted_minimizer_lookup_error(
                    "fragment-native stats ownership stats failed");
            }
            batch.permit_.shrink_to(batch.owned_bytes_);
            batch.ready_ = true;
            scratch.wave_state_.store(query_wave_scratch::wave_state::outstanding,
                                      std::memory_order_release);
            return batch;
        } catch (...) {
            scratch.reset_failed_wave_();
            throw;
        }
    }

    [[nodiscard]] bool ready() const noexcept {
        return ready_;
    }

    [[nodiscard]] bool compact_direct_ready() const noexcept {
        return ready_ && compact_direct_.has_value();
    }

    [[nodiscard]] bool compact_direct_only() const noexcept {
        return ready_ && backing_kind_ == backing_kind::compact_direct_only;
    }

    [[nodiscard]] std::size_t mapped_grouped_soa_payload_bytes() const noexcept {
        return mapped_payload_.size();
    }

    [[nodiscard]] std::size_t grouped_soa_directory_entry_count() const noexcept {
        return entry_count_;
    }

    [[nodiscard]] std::uint64_t target_length() const noexcept {
        return manifest_.target_length;
    }

    [[nodiscard]] std::uint64_t admitted_bytes() const noexcept {
        return permit_.bytes();
    }

    [[nodiscard]] std::size_t fragment_block_count() const noexcept {
        // Preserve the direct reader's manifest-geometry observation without
        // representing those segments as retained fragment_block_plan owners.
        return backing_kind_ == backing_kind::compact_direct_only ? manifest_.position_segment_count
                                                                  : block_count_;
    }

    [[nodiscard]] std::uint32_t largest_fragment_block_entry_count() const noexcept {
        return largest_block_entry_count_;
    }

    [[nodiscard]] std::uint32_t oversized_segment_block_count() const noexcept {
        return oversized_segment_block_count_;
    }

    [[nodiscard]] grouped_soa_query_reader_memory_stats memory_stats() const noexcept {
        const std::uint64_t directory_bytes = directory_.owned_capacity_bytes();
        const std::uint64_t entry_plan_bytes =
            bioseq::common::saturating_mul(entry_count_, sizeof(entry_plan));
        const std::uint64_t block_plan_bytes =
            bioseq::common::saturating_mul(block_count_, sizeof(fragment_block_plan));
        return grouped_soa_query_reader_memory_stats{
            .capacity_accounting_passes = 1u,
            .global_position_storage_preparations = 0u,
            .global_position_storage_allocations = 0u,
            .position_append_growth_reservations = 0u,
            .retained_position_count = 0u,
            .retained_position_capacity = 0u,
            .query_record_size_bytes = 0u,
            .query_record_count = 0u,
            .query_record_capacity = 0u,
            .query_record_capacity_bytes = 0u,
            .query_record_position_capacity_bytes = 0u,
            .query_hash_capacity_bytes = 0u,
            .released_query_hash_capacity_bytes = 0u,
            .query_route_radix_bits = 0u,
            .query_route_radix_bucket_count = 0u,
            .query_route_radix_nonempty_bucket_count = 0u,
            .query_route_radix_max_bucket_size = 0u,
            .query_route_control_lookups = 0u,
            .query_route_hash_probes = 0u,
            .query_route_matches = 0u,
            .decoded_control_records = 0u,
            .retained_selected_control_records = 0u,
            .selected_control_size_bytes = 0u,
            .retained_selected_control_capacity_bytes = 0u,
            .current_selected_control_capacity_bytes = 0u,
            .directory_capacity_bytes = directory_bytes,
            .ordinal_route_capacity_bytes =
                bioseq::common::saturating_add(entry_plan_bytes, block_plan_bytes),
            .selected_extent_capacity_bytes = 0u,
            .global_position_capacity_bytes = 0u,
            .control_payload_capacity_bytes = 0u,
            .owned_capacity_bytes = owned_bytes_,
            .saturated = owned_bytes_ == (std::numeric_limits<std::uint64_t>::max)(),
        };
    }
};

static_assert(prepared_fragment_anchor_batch_reader<prepared_minimizer_fragment_reader>);
static_assert(prepared_fragment_anchor_query_index<prepared_fragment_anchor_batch::query_handle>);
static_assert(!minimizer_lookup_index<prepared_minimizer_fragment_reader>);

} // namespace bioseq::indexing
