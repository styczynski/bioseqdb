// Differential tests for the PG-free H32 direct inverted-index prototype.

#include "common_tests.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

import bioseq.indexing.minimizer_direct_inverted_index;

namespace {

namespace idx = bioseq::indexing;

[[nodiscard]] constexpr idx::DirectIndexIdentity identity(
        std::uint64_t target_length) noexcept {
    return idx::DirectIndexIdentity{
        .generation_id = 0x1112131415161718ULL,
        .source_identity = 0x2122232425262728ULL,
        .compatibility_checksum = 0x3132333435363738ULL,
        .target_length = target_length,
    };
}

[[nodiscard]] idx::DirectIndexBuildOptions options(
        std::uint64_t target_length,
        std::uint64_t fragment_span = 0u,
        std::uint64_t bucket_override = 0u) noexcept {
    return idx::DirectIndexBuildOptions{
        .identity = identity(target_length),
        .fragment_span_bp = fragment_span,
        .bucket_count_override = bucket_override,
    };
}

template <class UInt>
requires std::is_unsigned_v<UInt>
[[nodiscard]] constexpr UInt reference_read_le(
        std::span<const std::byte> input,
        std::size_t offset) noexcept {
    UInt value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(UInt); ++byte) {
        value |= static_cast<UInt>(
            std::to_integer<std::uint8_t>(input[offset + byte]))
            << (byte * 8u);
    }
    return value;
}

template <class UInt>
requires std::is_unsigned_v<UInt>
constexpr void reference_write_le(
        std::span<std::byte> output,
        std::size_t offset,
        UInt value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(UInt); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & UInt{0xffu});
        value >>= 8u;
    }
}

/// Independent bit-at-a-time CRC-64/ECMA-182 oracle.  The implementation
/// deliberately does not call the production lookup-table codec.
[[nodiscard]] constexpr std::uint64_t reference_crc64(
        std::span<const std::byte> bytes) noexcept {
    constexpr std::uint64_t polynomial = 0x42F0E1EBA9EA3693ULL;
    std::uint64_t checksum = 0u;
    for (const std::byte byte : bytes) {
        checksum ^= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(byte)) << 56u;
        for (unsigned bit = 0u; bit < 8u; ++bit) {
            checksum = (checksum & (std::uint64_t{1u} << 63u)) != 0u
                ? (checksum << 1u) ^ polynomial
                : checksum << 1u;
        }
    }
    return checksum;
}

void reference_reseal_header(std::vector<std::byte>& image) {
    std::vector<std::byte> header(
        image.begin(),
        image.begin() + idx::kDirectInvertedIndexHeaderBytes);
    std::fill_n(
        header.begin()
            + static_cast<std::ptrdiff_t>(
                idx::kDirectInvertedIndexHeaderCrcOffset),
        idx::kDirectInvertedIndexChecksumBytes,
        std::byte{0});
    reference_write_le<std::uint64_t>(
        image,
        idx::kDirectInvertedIndexHeaderCrcOffset,
        reference_crc64(header));
}

void reference_reseal_compact_h32_header(
        std::vector<std::byte>& image) {
    REQUIRE(image.size() >= idx::kCompactH32DirectIndexHeaderBytes);
    std::array<std::byte, idx::kCompactH32DirectIndexHeaderBytes> header{};
    std::copy_n(image.begin(), header.size(), header.begin());
    std::fill_n(
        header.begin()
            + static_cast<std::ptrdiff_t>(idx::kCompactH32HeaderCrcOffset),
        idx::kDirectInvertedIndexChecksumBytes,
        std::byte{0});
    reference_write_le<std::uint64_t>(
        image,
        idx::kCompactH32HeaderCrcOffset,
        reference_crc64(header));
}

void reference_reseal_controls(std::vector<std::byte>& image) {
    const std::uint64_t offset = reference_read_le<std::uint64_t>(image, 112u);
    const std::uint64_t bytes = reference_read_le<std::uint64_t>(image, 120u);
    reference_write_le<std::uint64_t>(
        image,
        idx::kDirectInvertedIndexControlsCrcOffset,
        reference_crc64(std::span<const std::byte>{image}.subspan(
            static_cast<std::size_t>(offset),
            static_cast<std::size_t>(bytes))));
    reference_reseal_header(image);
}

using Oracle = std::map<std::uint32_t, std::vector<idx::DirectIndexOccurrence>>;

[[nodiscard]] Oracle make_oracle(
        std::span<const idx::DirectIndexOccurrence> input) {
    Oracle oracle;
    for (const auto& occurrence : input) {
        oracle[occurrence.hash].push_back(occurrence);
    }
    return oracle;
}

[[nodiscard]] std::uint64_t oracle_strand_zero_count(
        const std::vector<idx::DirectIndexOccurrence>& occurrences) noexcept {
    return static_cast<std::uint64_t>(std::count_if(
        occurrences.begin(), occurrences.end(),
        [](const auto& occurrence) {
            return !occurrence.target_strand_one;
        }));
}

struct CollectedPostings {
    std::vector<std::vector<std::uint64_t>> by_selection{};
    std::uint64_t callback_count = 0u;
};

bool collect_posting(void* opaque, const idx::DirectIndexPosting& posting) {
    auto& collected = *static_cast<CollectedPostings*>(opaque);
    if (posting.selection_ordinal >= collected.by_selection.size()) {
        return false;
    }
    collected.by_selection[posting.selection_ordinal].push_back(
        posting.encoded_position);
    ++collected.callback_count;
    return posting.target_strand_one
        == ((posting.encoded_position & 1u) != 0u);
}

[[nodiscard]] std::vector<std::uint64_t> expected_postings(
        std::vector<idx::DirectIndexOccurrence> occurrences,
        std::uint64_t fragment_span) {
    std::sort(
        occurrences.begin(), occurrences.end(),
        [fragment_span](const auto& left, const auto& right) {
            const std::uint64_t left_fragment = fragment_span == 0u
                ? 0u : left.target_bp / fragment_span;
            const std::uint64_t right_fragment = fragment_span == 0u
                ? 0u : right.target_bp / fragment_span;
            return std::tuple{
                    left_fragment, left.target_strand_one, left.target_bp}
                < std::tuple{
                    right_fragment, right.target_strand_one, right.target_bp};
        });
    std::vector<std::uint64_t> result;
    result.reserve(occurrences.size());
    for (const auto& occurrence : occurrences) {
        result.push_back(
            (occurrence.target_bp << 1u)
            | static_cast<std::uint64_t>(occurrence.target_strand_one));
    }
    return result;
}

[[nodiscard]] std::vector<std::byte> build_or_fail(
        std::span<const idx::DirectIndexOccurrence> input,
        const idx::DirectIndexBuildOptions& build_options) {
    auto image = idx::build_direct_inverted_index(input, build_options);
    REQUIRE(image.has_value());
    return std::move(*image);
}

[[nodiscard]] idx::DirectInvertedIndexView validate_or_fail(
        const std::vector<std::byte>& image,
        const idx::DirectIndexIdentity& expected_identity) {
    auto view = idx::validate_direct_inverted_index(image, expected_identity);
    REQUIRE(view.has_value());
    return *view;
}

void require_matches_oracle(
        const idx::DirectIndexQueryPlan& plan,
        std::span<const idx::DirectIndexQueryOccurrence> queries,
        const Oracle& oracle,
        std::uint64_t max_occ) {
    REQUIRE(plan.results.size() == queries.size());
    for (std::size_t ordinal = 0u; ordinal < queries.size(); ++ordinal) {
        const auto found = oracle.find(queries[ordinal].hash);
        const auto& result = plan.results[ordinal];
        if (found == oracle.end()) {
            CHECK_FALSE(result.present);
            CHECK_FALSE(result.positions_selected);
            CHECK(result.global_count == 0u);
            CHECK(result.forward_count == 0u);
            CHECK(result.reverse_count == 0u);
            continue;
        }
        const std::uint64_t total = found->second.size();
        const std::uint64_t strand_zero =
            oracle_strand_zero_count(found->second);
        CHECK(result.present);
        CHECK(result.global_count == total);
        CHECK(result.forward_count
            == (queries[ordinal].query_strand_one
                ? total - strand_zero : strand_zero));
        CHECK(result.reverse_count
            == total - result.forward_count);
        CHECK(result.positions_selected == (total <= max_occ));
        CHECK((result.positions_selected
                ? result.selection_ordinal
                    != idx::kDirectInvertedIndexNoSelection
                : result.selection_ordinal
                    == idx::kDirectInvertedIndexNoSelection));
    }
}

struct CompactTestRun {
    idx::CompactH32SortedRunHeader header{};
    std::vector<std::uint64_t> encoded_positions{};
};

struct CompactTestSource {
    std::vector<CompactTestRun> runs{};
    std::vector<CompactTestRun> replay_runs{};
    bool use_replay_runs_after_first_scan = false;
    std::uint64_t scans = 0u;
};

[[nodiscard]] std::expected<void, idx::DirectInvertedIndexError>
scan_compact_test_runs(
        void* opaque,
        const idx::CompactH32SortedRunReceiver& receiver) {
    auto& source = *static_cast<CompactTestSource*>(opaque);
    ++source.scans;
    const auto& runs = source.use_replay_runs_after_first_scan
            && source.scans > 1u
        ? source.replay_runs : source.runs;
    for (const CompactTestRun& run : runs) {
        if (auto begun = receiver.begin_run(run.header); !begun) {
            return std::unexpected(begun.error());
        }
        const std::size_t split = run.encoded_positions.size() / 2u;
        if (auto first = receiver.consume_positions(
                std::span<const std::uint64_t>{run.encoded_positions}.first(
                    split)); !first) {
            return std::unexpected(first.error());
        }
        if (auto second = receiver.consume_positions(
                std::span<const std::uint64_t>{run.encoded_positions}.subspan(
                    split)); !second) {
            return std::unexpected(second.error());
        }
    }
    return {};
}

[[nodiscard]] idx::CompactH32SortedRunSourceRef compact_source_ref(
        CompactTestSource& source) noexcept {
    return idx::CompactH32SortedRunSourceRef{
        .context = &source,
        .scan_sorted_runs_fn = &scan_compact_test_runs,
    };
}

[[nodiscard]] constexpr idx::CompactH32Geometry compact_geometry(
        std::uint8_t bits) noexcept {
    return idx::CompactH32Geometry{
        .hash_domain_bits = bits,
        .max_hash = bits == 32u
            ? (std::numeric_limits<std::uint32_t>::max)()
            : static_cast<std::uint32_t>(
                (std::uint64_t{1u} << bits) - 1u),
    };
}

[[nodiscard]] idx::CompactH32BuildOptions compact_options(
        std::uint64_t target_length,
        std::uint32_t max_occ_cap,
        std::uint8_t hash_bits = 9u) noexcept {
    return idx::CompactH32BuildOptions{
        .identity = identity(target_length),
        .max_occ_cap = max_occ_cap,
        .geometry = compact_geometry(hash_bits),
    };
}

[[nodiscard]] idx::CompactH32BoundedOpenOptions compact_bounded_options(
        const idx::CompactH32BuildOptions& build_options,
        std::optional<std::uint64_t> expected_direct_header_crc64 =
            std::nullopt) noexcept {
    return idx::CompactH32BoundedOpenOptions{
        .expected_identity = build_options.identity,
        .expected_max_occ_cap = build_options.max_occ_cap,
        .expected_geometry = build_options.geometry,
        .expected_direct_header_crc64 = expected_direct_header_crc64,
    };
}

[[nodiscard]] constexpr std::uint64_t encoded_position(
        std::uint64_t bp,
        bool strand_one) noexcept {
    return (bp << 1u) | static_cast<std::uint64_t>(strand_one);
}

[[nodiscard]] std::vector<std::byte> build_compact_or_fail(
        CompactTestSource& source,
        const idx::CompactH32BuildOptions& build_options) {
    auto image = idx::build_compact_h32_direct_index(
        compact_source_ref(source), build_options);
    REQUIRE(image.has_value());
    return std::move(*image);
}

[[nodiscard]] idx::CompactH32DirectIndexView validate_compact_or_fail(
        const std::vector<std::byte>& image,
        const idx::CompactH32BuildOptions& build_options) {
    auto view = idx::validate_compact_h32_direct_index(
        image,
        build_options.identity,
        build_options.max_occ_cap,
        build_options.geometry);
    REQUIRE(view.has_value());
    return *view;
}

void reference_reseal_compact_h32_payload_and_header(
        std::vector<std::byte>& image) {
    const auto reseal_section = [&image](
            std::size_t crc_offset,
            std::size_t section_offset_field,
            std::size_t section_bytes_field) {
        const std::uint64_t offset =
            reference_read_le<std::uint64_t>(
                image, section_offset_field);
        const std::uint64_t bytes =
            reference_read_le<std::uint64_t>(
                image, section_bytes_field);
        REQUIRE(offset <= image.size());
        REQUIRE(bytes <= image.size() - offset);
        reference_write_le<std::uint64_t>(
            image,
            crc_offset,
            reference_crc64(std::span<const std::byte>{image}.subspan(
                static_cast<std::size_t>(offset),
                static_cast<std::size_t>(bytes))));
    };
    reseal_section(idx::kCompactH32MembershipCrcOffset, 112u, 120u);
    reseal_section(idx::kCompactH32RankCrcOffset, 128u, 136u);
    reseal_section(idx::kCompactH32DescriptorsCrcOffset, 144u, 152u);
    reseal_section(idx::kCompactH32PostingsCrcOffset, 160u, 168u);
    reference_reseal_compact_h32_header(image);
}

[[nodiscard]] std::vector<std::byte> reference_convert_compact_v2_to_v1(
        const std::vector<std::byte>& v2_image) {
    REQUIRE(reference_read_le<std::uint32_t>(v2_image, 8u)
        == idx::kCompactH32DirectIndexVersionV2);
    REQUIRE(reference_read_le<std::uint16_t>(v2_image, 22u)
        == idx::kCompactH32DirectIndexDescriptorBytesV2);
    const std::uint8_t posting_width =
        std::to_integer<std::uint8_t>(v2_image[20u]);
    REQUIRE((posting_width == 4u || posting_width == 8u));
    const std::uint64_t accepted_hash_count =
        reference_read_le<std::uint64_t>(v2_image, 80u);
    const std::uint64_t accepted_posting_count =
        reference_read_le<std::uint64_t>(v2_image, 88u);
    const std::uint64_t membership_bytes =
        reference_read_le<std::uint64_t>(v2_image, 120u);
    const std::uint64_t rank_bytes =
        reference_read_le<std::uint64_t>(v2_image, 136u);
    const std::uint64_t v2_membership_offset =
        reference_read_le<std::uint64_t>(v2_image, 112u);
    const std::uint64_t v2_rank_offset =
        reference_read_le<std::uint64_t>(v2_image, 128u);
    const std::uint64_t v2_descriptors_offset =
        reference_read_le<std::uint64_t>(v2_image, 144u);
    const std::uint64_t v2_postings_offset =
        reference_read_le<std::uint64_t>(v2_image, 160u);
    const auto align_section = [](std::uint64_t value) {
        constexpr std::uint64_t alignment =
            idx::kDirectInvertedIndexSectionAlignment;
        return (value + alignment - 1u) & ~(alignment - 1u);
    };
    const std::uint64_t membership_offset = align_section(
        idx::kCompactH32DirectIndexHeaderBytes);
    const std::uint64_t rank_offset = align_section(
        membership_offset + membership_bytes);
    const std::uint64_t descriptors_offset = align_section(
        rank_offset + rank_bytes);
    const std::uint64_t descriptors_bytes = accepted_hash_count
        * idx::kCompactH32DirectIndexDescriptorBytesV1;
    const std::uint64_t postings_offset = align_section(
        descriptors_offset + descriptors_bytes);
    const std::uint64_t postings_bytes = accepted_posting_count
        * posting_width;
    const std::uint64_t image_bytes = postings_offset + postings_bytes;
    REQUIRE(image_bytes <= (std::numeric_limits<std::size_t>::max)());
    std::vector<std::byte> v1_image(
        static_cast<std::size_t>(image_bytes), std::byte{0});
    std::copy_n(v2_image.begin(),
        idx::kCompactH32DirectIndexHeaderBytes, v1_image.begin());
    std::copy_n(
        v2_image.begin()
            + static_cast<std::ptrdiff_t>(v2_membership_offset),
        static_cast<std::size_t>(membership_bytes),
        v1_image.begin()
            + static_cast<std::ptrdiff_t>(membership_offset));
    std::copy_n(
        v2_image.begin() + static_cast<std::ptrdiff_t>(v2_rank_offset),
        static_cast<std::size_t>(rank_bytes),
        v1_image.begin() + static_cast<std::ptrdiff_t>(rank_offset));

    std::uint64_t v1_posting_cursor = 0u;
    for (std::uint64_t dense = 0u;
         dense < accepted_hash_count;
         ++dense) {
        const std::uint64_t v2_descriptor = v2_descriptors_offset
            + dense * idx::kCompactH32DirectIndexDescriptorBytesV2;
        const std::uint32_t posting_or_inline =
            reference_read_le<std::uint32_t>(v2_image, v2_descriptor);
        const std::uint32_t count =
            reference_read_le<std::uint16_t>(v2_image, v2_descriptor + 4u);
        const std::uint32_t strand_zero =
            reference_read_le<std::uint16_t>(v2_image, v2_descriptor + 6u);
        REQUIRE(count != 0u);
        REQUIRE(strand_zero <= count);
        const std::uint64_t v1_descriptor = descriptors_offset
            + dense * idx::kCompactH32DirectIndexDescriptorBytesV1;
        reference_write_le<std::uint32_t>(v1_image, v1_descriptor,
            static_cast<std::uint32_t>(v1_posting_cursor));
        reference_write_le<std::uint32_t>(v1_image,
            v1_descriptor + 4u, count);
        reference_write_le<std::uint32_t>(v1_image,
            v1_descriptor + 8u, strand_zero);
        const std::uint64_t destination = postings_offset
            + v1_posting_cursor * posting_width;
        if (posting_width == 4u && count == 1u) {
            reference_write_le<std::uint32_t>(
                v1_image, destination, posting_or_inline);
        } else {
            const std::uint64_t source = v2_postings_offset
                + static_cast<std::uint64_t>(posting_or_inline)
                    * posting_width;
            const std::uint64_t copy_bytes =
                static_cast<std::uint64_t>(count) * posting_width;
            REQUIRE(source <= v2_image.size());
            REQUIRE(copy_bytes <= v2_image.size() - source);
            std::copy_n(
                v2_image.begin() + static_cast<std::ptrdiff_t>(source),
                static_cast<std::size_t>(copy_bytes),
                v1_image.begin()
                    + static_cast<std::ptrdiff_t>(destination));
        }
        v1_posting_cursor += count;
    }
    REQUIRE(v1_posting_cursor == accepted_posting_count);

    reference_write_le<std::uint32_t>(v1_image, 8u,
        idx::kCompactH32DirectIndexVersionV1);
    reference_write_le<std::uint16_t>(v1_image, 22u,
        idx::kCompactH32DirectIndexDescriptorBytesV1);
    reference_write_le<std::uint64_t>(v1_image, 112u, membership_offset);
    reference_write_le<std::uint64_t>(v1_image, 128u, rank_offset);
    reference_write_le<std::uint64_t>(v1_image, 144u, descriptors_offset);
    reference_write_le<std::uint64_t>(v1_image, 152u, descriptors_bytes);
    reference_write_le<std::uint64_t>(v1_image, 160u, postings_offset);
    reference_write_le<std::uint64_t>(v1_image, 168u, postings_bytes);
    reference_write_le<std::uint64_t>(v1_image, 216u, image_bytes);
    std::fill(
        v1_image.begin()
            + static_cast<std::ptrdiff_t>(
                idx::kCompactH32InlineSingletonCountOffset),
        v1_image.begin()
            + idx::kCompactH32DirectIndexHeaderBytes,
        std::byte{0});
    reference_reseal_compact_h32_payload_and_header(v1_image);
    return v1_image;
}

[[nodiscard]] std::expected<idx::CompactH32BoundedOpenView,
    idx::DirectInvertedIndexError>
open_compact_bounded_owned(
        std::vector<std::byte> image,
        const idx::CompactH32BoundedOpenOptions& open_options) {
    auto owner = std::make_shared<std::vector<std::byte>>(std::move(image));
    const std::span<const std::byte> direct_section{
        owner->data(), owner->size()};
    std::shared_ptr<const void> mapped_owner = owner;
    return idx::open_compact_h32_direct_index_bounded(
        std::move(mapped_owner), direct_section, open_options);
}

} // namespace

TEST_CASE(
    "direct H32 index differentially resolves absent singleton duplicate and "
    "unsorted query occurrences",
    "[doc0383][p4][direct-index][differential][h32]") {
    const std::vector<idx::DirectIndexOccurrence> input{
        {.hash = 0u, .target_bp = 3u, .target_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .target_bp = 11u, .target_strand_one = true},
        // An exact duplicate occurrence remains authoritative, not deduped.
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .target_bp = 11u, .target_strand_one = true},
        {.hash = 42u, .target_bp = 90u, .target_strand_one = true},
        {.hash = 42u, .target_bp = 10u, .target_strand_one = false},
        {.hash = 42u, .target_bp = 20u, .target_strand_one = false},
    };
    const auto build_options = options(1'000u, 0u, 8u);
    const auto plan = idx::plan_direct_inverted_index(input, build_options);
    REQUIRE(plan.has_value());
    CHECK(plan->posting_width_bytes == 4u);
    CHECK(plan->unique_hash_count == 3u);
    CHECK(plan->occurrence_count == input.size());
    CHECK(plan->occupancy_bytes == plan->bucket_count / 8u);
    CHECK(plan->rank_bytes
        == ((plan->bucket_count
                + idx::kDirectInvertedIndexRankGroupBuckets - 1u)
            / idx::kDirectInvertedIndexRankGroupBuckets + 1u) * 8u);
    CHECK(plan->controls_bytes
        == plan->unique_hash_count * idx::kDirectInvertedIndexControlBytes);
    CHECK(plan->summaries_bytes == 0u);
    CHECK(plan->postings_bytes == input.size() * 4u);
    CHECK(plan->image_bytes == plan->postings_offset + plan->postings_bytes);
    CHECK(plan->estimated_build_scratch_bytes != 0u);

    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    const std::vector<idx::DirectIndexQueryOccurrence> queries{
        {.hash = 42u, .query_strand_one = true},
        {.hash = 999u, .query_strand_one = false},
        {.hash = 0u, .query_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .query_strand_one = false},
        {.hash = 42u, .query_strand_one = false},
        {.hash = (std::numeric_limits<std::uint32_t>::max)(),
            .query_strand_one = true},
    };
    const auto query_plan = idx::query_direct_inverted_index(view, queries);
    REQUIRE(query_plan.has_value());
    const Oracle oracle = make_oracle(input);
    require_matches_oracle(
        *query_plan, queries, oracle,
        (std::numeric_limits<std::uint64_t>::max)());
    // Duplicate query occurrences share one unique target selection.
    CHECK(query_plan->selections.size() == oracle.size());
    CHECK(query_plan->evidence.unique_selected_hash_count == oracle.size());
    CHECK(query_plan->evidence.selected_posting_count == input.size());

    CollectedPostings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(
            query_plan->selections.size()),
    };
    const auto visited = idx::visit_direct_inverted_index_postings(
        view, *query_plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == input.size());
    CHECK(collected.callback_count == input.size());
    for (std::size_t selection = 0u;
         selection < query_plan->selections.size(); ++selection) {
        const auto hash = query_plan->selections[selection].hash;
        REQUIRE(oracle.contains(hash));
        CHECK(collected.by_selection[selection]
            == expected_postings(oracle.at(hash), 0u));
    }
}

TEST_CASE(
    "direct H32 summaries give exact both-strand layouts and visit heavy "
    "selected postings once",
    "[doc0383][p4][direct-index][fragment-summary][max-occ]") {
    std::vector<idx::DirectIndexOccurrence> input;
    constexpr std::uint64_t heavy_count = 1'001u;
    input.reserve(heavy_count + 4u);
    for (std::uint64_t ordinal = 0u; ordinal < heavy_count; ++ordinal) {
        input.push_back(idx::DirectIndexOccurrence{
            .hash = 7u,
            .target_bp = (ordinal * 37u) % 9'900u,
            .target_strand_one = ordinal % 3u == 0u,
        });
    }
    for (std::uint64_t ordinal = 0u; ordinal < 4u; ++ordinal) {
        input.push_back(idx::DirectIndexOccurrence{
            .hash = 9u,
            .target_bp = 50u + ordinal * 100u,
            .target_strand_one = ordinal >= 2u,
        });
    }
    const auto build_options = options(10'000u, 100u, 8u);
    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    REQUIRE(view.has_run_summaries());
    CHECK(view.header.run_summary_count > view.header.unique_hash_count);
    const Oracle oracle = make_oracle(input);

    const std::vector<idx::DirectIndexQueryOccurrence> heavy_queries{
        {.hash = 7u, .query_strand_one = false},
        {.hash = 7u, .query_strand_one = true},
    };
    const auto accepted = idx::query_direct_inverted_index(
        view, heavy_queries, heavy_count);
    REQUIRE(accepted.has_value());
    require_matches_oracle(*accepted, heavy_queries, oracle, heavy_count);
    REQUIRE(accepted->selections.size() == 1u);
    CHECK(accepted->evidence.selected_posting_count == heavy_count);
    std::set<std::uint64_t> heavy_fragments;
    for (const auto& occurrence : oracle.at(7u)) {
        heavy_fragments.insert(occurrence.target_bp / 100u);
    }
    CHECK(accepted->evidence.run_summaries_examined
        == heavy_fragments.size());

    CollectedPostings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_direct_inverted_index_postings(
        view, *accepted, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    // Two query orientations still read the selected target postings once.
    CHECK(visited->postings_visited == heavy_count);
    CHECK(collected.callback_count == heavy_count);
    CHECK(collected.by_selection.front()
        == expected_postings(oracle.at(7u), 100u));

    const auto rejected = idx::query_direct_inverted_index(
        view, heavy_queries, heavy_count - 1u);
    REQUIRE(rejected.has_value());
    require_matches_oracle(
        *rejected, heavy_queries, oracle, heavy_count - 1u);
    CHECK(rejected->selections.empty());
    CHECK(rejected->spans.empty());
    CHECK(rejected->evidence.selected_posting_count == 0u);

    const std::array boundary_query{
        idx::DirectIndexQueryOccurrence{
            .hash = 9u, .query_strand_one = false},
    };
    const auto boundary_accepted = idx::query_direct_inverted_index(
        view, boundary_query, 4u);
    const auto boundary_rejected = idx::query_direct_inverted_index(
        view, boundary_query, 3u);
    REQUIRE(boundary_accepted.has_value());
    REQUIRE(boundary_rejected.has_value());
    CHECK(boundary_accepted->results.front().positions_selected);
    CHECK_FALSE(boundary_rejected->results.front().positions_selected);
    CHECK(boundary_rejected->results.front().global_count == 4u);
}

TEST_CASE(
    "direct H32 codec selects P32 and P64 without narrowing positions",
    "[doc0383][p4][direct-index][p32][p64]") {
    SECTION("P32 includes the maximum encoded uint32 position") {
        constexpr std::uint64_t target_length = std::uint64_t{1u} << 31u;
        const std::vector<idx::DirectIndexOccurrence> input{
            {.hash = 1u, .target_bp = target_length - 1u,
                .target_strand_one = true},
        };
        const auto build_options = options(target_length, 0u, 8u);
        const auto image = build_or_fail(input, build_options);
        const auto view = validate_or_fail(image, build_options.identity);
        CHECK(view.header.posting_width_bytes == 4u);
        const std::array queries{
            idx::DirectIndexQueryOccurrence{
                .hash = 1u, .query_strand_one = false},
        };
        const auto plan = idx::query_direct_inverted_index(view, queries);
        REQUIRE(plan.has_value());
        CollectedPostings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        REQUIRE(idx::visit_direct_inverted_index_postings(
            view, *plan, &collected, &collect_posting));
        REQUIRE(collected.by_selection.front().size() == 1u);
        CHECK(collected.by_selection.front().front()
            == (std::numeric_limits<std::uint32_t>::max)());
    }

    SECTION("P64 preserves a position beyond the packed-32 boundary") {
        constexpr std::uint64_t target_length =
            (std::uint64_t{1u} << 31u) + 10u;
        const std::vector<idx::DirectIndexOccurrence> input{
            {.hash = 1u, .target_bp = (std::uint64_t{1u} << 31u) + 3u,
                .target_strand_one = false},
        };
        const auto build_options = options(target_length, 0u, 8u);
        const auto image = build_or_fail(input, build_options);
        const auto view = validate_or_fail(image, build_options.identity);
        CHECK(view.header.posting_width_bytes == 8u);
        const std::array queries{
            idx::DirectIndexQueryOccurrence{
                .hash = 1u, .query_strand_one = false},
        };
        const auto plan = idx::query_direct_inverted_index(view, queries);
        REQUIRE(plan.has_value());
        CollectedPostings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        REQUIRE(idx::visit_direct_inverted_index_postings(
            view, *plan, &collected, &collect_posting));
        REQUIRE(collected.by_selection.front().size() == 1u);
        CHECK(collected.by_selection.front().front()
            == (((std::uint64_t{1u} << 31u) + 3u) << 1u));
    }
}

TEST_CASE(
    "direct H32 validation fails closed on identity checksum dictionary and "
    "semantic corruption",
    "[doc0383][p4][direct-index][corruption]") {
    const std::vector<idx::DirectIndexOccurrence> input{
        {.hash = 1u, .target_bp = 10u, .target_strand_one = false},
        {.hash = 1u, .target_bp = 20u, .target_strand_one = true},
        {.hash = 2u, .target_bp = 30u, .target_strand_one = false},
    };
    const auto build_options = options(100u, 0u, 8u);
    const auto pristine = build_or_fail(input, build_options);

    SECTION("expected identity is exact") {
        auto wrong = build_options.identity;
        ++wrong.source_identity;
        const auto result = idx::validate_direct_inverted_index(pristine, wrong);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()
            == idx::DirectInvertedIndexError::identity_mismatch);
    }

    SECTION("posting corruption is authenticated") {
        auto corrupt = pristine;
        const std::uint64_t postings_offset =
            reference_read_le<std::uint64_t>(corrupt, 144u);
        corrupt[static_cast<std::size_t>(postings_offset)] ^= std::byte{0x40};
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()
            == idx::DirectInvertedIndexError::section_checksum);
    }

    SECTION("reserved header bytes remain zero even with a valid header CRC") {
        auto corrupt = pristine;
        corrupt[248u] = std::byte{1};
        reference_reseal_header(corrupt);
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()
            == idx::DirectInvertedIndexError::reserved_nonzero);
    }

    SECTION("duplicate dictionary keys fail after checksums are resealed") {
        auto corrupt = pristine;
        const std::uint64_t controls_offset =
            reference_read_le<std::uint64_t>(corrupt, 112u);
        const std::uint32_t first_hash = reference_read_le<std::uint32_t>(
            corrupt, static_cast<std::size_t>(controls_offset));
        reference_write_le<std::uint32_t>(
            corrupt,
            static_cast<std::size_t>(controls_offset
                + idx::kDirectInvertedIndexControlBytes),
            first_hash);
        reference_reseal_controls(corrupt);
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == idx::DirectInvertedIndexError::dictionary);
    }

    SECTION("authoritative count cannot exceed its posting extent") {
        auto corrupt = pristine;
        const std::uint64_t controls_offset =
            reference_read_le<std::uint64_t>(corrupt, 112u);
        const std::uint64_t first_count = reference_read_le<std::uint64_t>(
            corrupt, static_cast<std::size_t>(controls_offset + 8u));
        reference_write_le<std::uint64_t>(
            corrupt,
            static_cast<std::size_t>(controls_offset + 8u),
            first_count + 1u);
        reference_reseal_controls(corrupt);
        const auto result = idx::validate_direct_inverted_index(
            corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK((result.error() == idx::DirectInvertedIndexError::control
            || result.error() == idx::DirectInvertedIndexError::posting));
    }
}

TEST_CASE(
    "direct H32 point lookup examines a probe chain rather than every target "
    "control",
    "[doc0383][p4][direct-index][no-control-scan]") {
    std::vector<idx::DirectIndexOccurrence> input;
    for (std::uint32_t hash = 0u; hash < 128u; ++hash) {
        input.push_back(idx::DirectIndexOccurrence{
            .hash = hash,
            .target_bp = hash,
            .target_strand_one = (hash & 1u) != 0u,
        });
    }
    const auto build_options = options(1'000u);
    const auto image = build_or_fail(input, build_options);
    const auto view = validate_or_fail(image, build_options.identity);
    const std::array query{
        idx::DirectIndexQueryOccurrence{
            // mix_hash32(0)==0 and hash zero is inserted first, so this is a
            // deterministic one-control probe rather than a probabilistic
            // assertion about the avalanche mixer's low bits.
            .hash = 0u, .query_strand_one = false},
    };
    const auto plan = idx::query_direct_inverted_index(view, query);
    REQUIRE(plan.has_value());
    REQUIRE(plan->results.front().present);
    CHECK(plan->evidence.control_records_examined
        < view.header.unique_hash_count);
    CHECK(plan->evidence.dictionary_slots_examined
        < view.header.bucket_count);
}

TEST_CASE(
    "compact exact-domain H32 geometry freezes canonical K15 and full-H32 "
    "boundaries and report128 economics",
    "[doc0383][p4][compact-h32][geometry][economics]") {
    const auto zero = idx::compact_h32_address(
        0u, compact_geometry(32u));
    const auto byte_boundary = idx::compact_h32_address(
        8u, compact_geometry(32u));
    const auto rank_left = idx::compact_h32_address(
        255u, compact_geometry(32u));
    const auto rank_right = idx::compact_h32_address(
        256u, compact_geometry(32u));
    const auto maximum = idx::compact_h32_address(
        (std::numeric_limits<std::uint32_t>::max)(),
        compact_geometry(32u));
    REQUIRE(zero.has_value());
    REQUIRE(byte_boundary.has_value());
    REQUIRE(rank_left.has_value());
    REQUIRE(rank_right.has_value());
    REQUIRE(maximum.has_value());
    CHECK(zero->membership_byte_ordinal == 0u);
    CHECK(zero->membership_bit == 0u);
    CHECK(byte_boundary->membership_byte_ordinal == 1u);
    CHECK(byte_boundary->membership_bit == 0u);
    CHECK(rank_left->rank_checkpoint_ordinal == 0u);
    CHECK(rank_left->bit_within_rank_group == 255u);
    CHECK(rank_right->rank_checkpoint_ordinal == 1u);
    CHECK(rank_right->bit_within_rank_group == 0u);
    CHECK(maximum->membership_byte_ordinal
        == idx::kCompactH32FullMembershipBytes - 1u);
    CHECK(maximum->membership_bit == 7u);
    CHECK(maximum->rank_checkpoint_ordinal
        == (std::uint64_t{1u} << 24u) - 1u);
    CHECK(maximum->bit_within_rank_group == 255u);

    CompactTestSource empty;
    const auto canonical_options = idx::CompactH32BuildOptions{
        .identity = identity(1'000u),
        .max_occ_cap = 500u,
    };
    const auto canonical_plan = idx::plan_compact_h32_direct_index(
        compact_source_ref(empty), canonical_options);
    REQUIRE(canonical_plan.has_value());
    CHECK(canonical_plan->geometry.canonical_k15());
    CHECK(canonical_plan->membership_bytes
        == idx::kCompactH32CanonicalK15MembershipBytes);
    CHECK(canonical_plan->rank_bytes
        == idx::kCompactH32CanonicalK15RankBytes);
    CHECK(canonical_plan->image_bytes == 150'995'200u);

    auto full_options = canonical_options;
    full_options.geometry = compact_geometry(32u);
    const auto full_plan = idx::plan_compact_h32_direct_index(
        compact_source_ref(empty), full_options);
    REQUIRE(full_plan.has_value());
    CHECK(full_plan->geometry.full_h32());
    CHECK(full_plan->membership_bytes
        == idx::kCompactH32FullMembershipBytes);
    CHECK(full_plan->rank_bytes == idx::kCompactH32FullRankBytes);
    CHECK(full_plan->image_bytes == 603'980'032u);

    constexpr std::uint64_t report128_unique_estimate = 357'000'000u;
    constexpr std::uint64_t report128_postings = 373'218'519u;
    const auto canonical_report128 = idx::estimate_compact_h32_image_bytes(
        canonical_options.geometry,
        report128_unique_estimate,
        report128_postings,
        4u);
    const auto full_report128 = idx::estimate_compact_h32_image_bytes(
        full_options.geometry,
        report128_unique_estimate,
        report128_postings,
        4u);
    REQUIRE(canonical_report128.has_value());
    REQUIRE(full_report128.has_value());
    CHECK(canonical_report128->inline_singleton_count == 340'781'481u);
    CHECK(canonical_report128->materialized_hash_count == 16'218'519u);
    CHECK(canonical_report128->materialized_posting_count == 32'437'038u);
    CHECK(canonical_report128->descriptor_bytes == 2'856'000'000u);
    CHECK(canonical_report128->posting_bytes == 129'748'152u);
    CHECK(canonical_report128->image_bytes == 3'136'743'352u);
    CHECK(full_report128->image_bytes == 3'589'728'184u);

    constexpr std::uint64_t report128_query_hashes = 14'171'377u;
    const auto canonical_touches = idx::estimate_compact_h32_query_touches(
        report128_query_hashes,
        std::uint64_t{1u} << 22u,
        report128_query_hashes);
    const auto full_touches = idx::estimate_compact_h32_query_touches(
        report128_query_hashes,
        report128_query_hashes,
        report128_query_hashes);
    REQUIRE(canonical_touches.has_value());
    REQUIRE(full_touches.has_value());
    CHECK(canonical_touches->maximum_membership_bytes == 453'484'064u);
    CHECK(canonical_touches->maximum_rank_bytes == 16'777'216u);
    CHECK(canonical_touches->maximum_descriptor_bytes == 113'371'016u);
    CHECK(canonical_touches->maximum_metadata_bytes == 583'632'296u);
    CHECK(full_touches->maximum_metadata_bytes == 623'540'588u);
}

TEST_CASE(
    "compact H32 V2 writer binds the persisted cap to uint16 descriptors",
    "[compact-h32][v2][descriptor][max-occ-cap]") {
    CompactTestSource empty;
    const auto maximum = compact_options(
        100u,
        (std::numeric_limits<std::uint16_t>::max)(),
        9u);
    const auto accepted = idx::plan_compact_h32_direct_index(
        compact_source_ref(empty), maximum);
    REQUIRE(accepted.has_value());
    CHECK(accepted->wire_version
        == idx::kCompactH32DirectIndexVersionV2);
    CHECK(accepted->descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV2);

    auto too_wide = maximum;
    too_wide.max_occ_cap =
        static_cast<std::uint32_t>(
            (std::numeric_limits<std::uint16_t>::max)()) + 1u;
    const auto rejected = idx::plan_compact_h32_direct_index(
        compact_source_ref(empty), too_wide);
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error()
        == idx::DirectInvertedIndexError::invalid_options);
    CHECK(empty.scans == 1u);
}

TEST_CASE(
    "compact H32 bounded open retains a nested nonzero-offset section owner "
    "and exposes a queryable alias-bound view",
    "[doc0383][compact-h32][bounded-open][alias-owner][lifetime]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 7u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(17u, false)},
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t nested_header_receipt_crc64 = reference_crc64(
        std::span<const std::byte>{image}.first(
            idx::kCompactH32DirectIndexHeaderBytes));
    const auto open_options = compact_bounded_options(
        build_options, nested_header_receipt_crc64);

    constexpr std::size_t prefix_bytes = 777u;
    constexpr std::size_t suffix_bytes = 31u;
    std::weak_ptr<std::vector<std::byte>> owner_lifetime;
    {
        auto outer_image = std::make_shared<std::vector<std::byte>>(
            prefix_bytes + image.size() + suffix_bytes, std::byte{0xa5});
        std::copy(image.begin(), image.end(),
            outer_image->begin()
                + static_cast<std::ptrdiff_t>(prefix_bytes));
        owner_lifetime = outer_image;
        const std::span<const std::byte> direct_section{
            outer_image->data() + prefix_bytes, image.size()};
        const std::byte* const outer_image_start = outer_image->data();
        std::shared_ptr<const void> mapped_owner{
            outer_image, outer_image_start};
        REQUIRE(mapped_owner.get() != direct_section.data());

        auto opened = idx::open_compact_h32_direct_index_bounded(
            std::move(mapped_owner), direct_section, open_options);
        REQUIRE(opened.has_value());
        CHECK(opened->image_owner().get() == direct_section.data());
        CHECK(opened->image_owner().get() != outer_image_start);
        CHECK(opened->view().image.data() == direct_section.data());
        CHECK(opened->view().image.size() == image.size());
        CHECK(opened->view().header.header_crc64
            == reference_read_le<std::uint64_t>(
                image, idx::kCompactH32HeaderCrcOffset));
        CHECK(opened->evidence().direct_section_bytes == image.size());
        CHECK(opened->evidence().header_bytes_examined
            == idx::kCompactH32DirectIndexHeaderBytes);
        CHECK(opened->evidence().payload_bytes_examined == 0u);
        CHECK(opened->evidence().expected_direct_header_crc64_checked);

        outer_image.reset();
        CHECK_FALSE(owner_lifetime.expired());
        const std::array query{
            idx::CompactH32QueryOccurrence{
                .hash = 7u, .query_strand_one = false},
        };
        const auto planned = idx::query_compact_h32_direct_index(
            opened->view(), query, 2u);
        REQUIRE(planned.has_value());
        REQUIRE(planned->results.size() == 1u);
        CHECK(planned->results.front().accepted);
        CHECK(planned->results.front().global_count == 1u);
    }
    CHECK(owner_lifetime.expired());
}

TEST_CASE(
    "compact H32 bounded open rejects truncated unauthenticated and forged "
    "fixed-header geometry without scanning payload sections",
    "[doc0383][compact-h32][bounded-open][adversarial][receipt]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 7u, .global_count = 2u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {
                    encoded_position(3u, false),
                    encoded_position(9u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto open_options = compact_bounded_options(build_options);
    const auto open_with_size = [](
            std::vector<std::byte> candidate,
            std::size_t exposed_bytes,
            idx::CompactH32BoundedOpenOptions candidate_options) {
        auto owner = std::make_shared<std::vector<std::byte>>(
            std::move(candidate));
        REQUIRE(exposed_bytes <= owner->size());
        const std::span<const std::byte> direct_section{
            owner->data(), exposed_bytes};
        std::shared_ptr<const void> mapped_owner = owner;
        return idx::open_compact_h32_direct_index_bounded(
            std::move(mapped_owner), direct_section, candidate_options);
    };

    SECTION("a fixed header shorter than 256 bytes is rejected") {
        const auto opened = open_with_size(
            image,
            idx::kCompactH32DirectIndexHeaderBytes - 1u,
            open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::DirectInvertedIndexError::image_size);
    }

    SECTION("a section shorter than its authenticated total is rejected") {
        const auto opened = open_with_size(
            image, image.size() - 1u, open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::DirectInvertedIndexError::image_size);
    }

    SECTION("a corrupted embedded header checksum is rejected") {
        auto corrupt = image;
        corrupt[idx::kCompactH32HeaderCrcOffset] ^= std::byte{0x01};
        const auto opened = open_compact_bounded_owned(
            std::move(corrupt), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::header_checksum);
    }

    SECTION("an unknown wire version is rejected exactly") {
        auto forged = image;
        reference_write_le<std::uint32_t>(forged, 8u, 3u);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::bad_version);
    }

    SECTION("V2 cannot be paired with the V1 descriptor width") {
        auto forged = image;
        reference_write_le<std::uint16_t>(forged, 22u,
            idx::kCompactH32DirectIndexDescriptorBytesV1);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::bad_header);
    }

    SECTION("V1 cannot be paired with the V2 descriptor width") {
        auto forged = image;
        reference_write_le<std::uint32_t>(forged, 8u,
            idx::kCompactH32DirectIndexVersionV1);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::bad_header);
    }

    SECTION("an impossible V2 inline-singleton count is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(forged,
            idx::kCompactH32InlineSingletonCountOffset, 2u);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::DirectInvertedIndexError::image_size);
    }

    SECTION("a mismatched outer nested-header receipt is rejected") {
        const std::uint64_t actual_crc = reference_crc64(
            std::span<const std::byte>{image}.first(
                idx::kCompactH32DirectIndexHeaderBytes));
        const auto opened = open_compact_bounded_owned(
            image,
            compact_bounded_options(build_options, actual_crc ^ 1u));
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::receipt_mismatch);
    }

    SECTION("a misaligned membership offset is rejected after resealing") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 112u,
            reference_read_le<std::uint64_t>(forged, 112u) + 1u);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::section_layout);
    }

    SECTION("overlapping rank and descriptor sections are rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 144u,
            reference_read_le<std::uint64_t>(forged, 128u));
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::section_layout);
    }

    SECTION("an overflowing posting interval is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 160u,
            (std::numeric_limits<std::uint64_t>::max)() - 63u);
        reference_write_le<std::uint64_t>(forged, 168u, 128u);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::section_layout);
    }

    SECTION("a forged total direct-section size is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 216u,
            reference_read_le<std::uint64_t>(forged, 216u) + 1u);
        reference_reseal_compact_h32_header(forged);
        const auto opened = open_compact_bounded_owned(
            std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::DirectInvertedIndexError::image_size);
    }

    SECTION("the persisted max-occ cap is matched exactly") {
        auto wrong_options = open_options;
        ++wrong_options.expected_max_occ_cap;
        const auto opened = open_compact_bounded_owned(
            image, wrong_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::identity_mismatch);
    }

    SECTION("the hash-bits and max-hash profile is matched exactly") {
        auto wrong_options = open_options;
        wrong_options.expected_geometry = compact_geometry(10u);
        const auto opened = open_compact_bounded_owned(
            image, wrong_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error()
            == idx::DirectInvertedIndexError::identity_mismatch);
    }
}

TEST_CASE(
    "compact H32 bounded open succeeds when every payload page is no-access",
    "[doc0383][compact-h32][bounded-open][fixed-header][no-payload-read]") {
#if defined(__unix__) || defined(__APPLE__)
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 7u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(17u, false)},
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t nested_header_receipt_crc64 = reference_crc64(
        std::span<const std::byte>{image}.first(
            idx::kCompactH32DirectIndexHeaderBytes));
    const auto open_options = compact_bounded_options(
        build_options, nested_header_receipt_crc64);

    const long page_size_result = ::sysconf(_SC_PAGESIZE);
    REQUIRE(page_size_result
        > static_cast<long>(idx::kCompactH32DirectIndexHeaderBytes));
    const std::size_t page_size =
        static_cast<std::size_t>(page_size_result);
    const std::size_t payload_bytes =
        image.size() - idx::kCompactH32DirectIndexHeaderBytes;
    const std::size_t payload_pages = (std::max)(
        std::size_t{1u}, (payload_bytes + page_size - 1u) / page_size);
    const std::size_t mapping_bytes = (1u + payload_pages) * page_size;
#if defined(MAP_ANONYMOUS)
    constexpr int anonymous_map_flag = MAP_ANONYMOUS;
#else
    constexpr int anonymous_map_flag = MAP_ANON;
#endif
    void* const mapping = ::mmap(
        nullptr,
        mapping_bytes,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | anonymous_map_flag,
        -1,
        0);
    REQUIRE(mapping != MAP_FAILED);
    std::weak_ptr<const void> mapping_lifetime;
    {
        std::shared_ptr<const void> mapped_owner{
            mapping,
            [mapping_bytes](const void* address) noexcept {
                static_cast<void>(::munmap(
                    const_cast<void*>(address), mapping_bytes));
            },
        };
        mapping_lifetime = mapped_owner;
        auto* const mapping_bytes_begin = static_cast<std::byte*>(mapping);
        std::byte* const direct_section_begin =
            mapping_bytes_begin + page_size
            - idx::kCompactH32DirectIndexHeaderBytes;
        std::memcpy(
            direct_section_begin,
            image.data(),
            idx::kCompactH32DirectIndexHeaderBytes);
        REQUIRE(::mprotect(mapping_bytes_begin, page_size, PROT_READ) == 0);
        REQUIRE(::mprotect(
            mapping_bytes_begin + page_size,
            mapping_bytes - page_size,
            PROT_NONE) == 0);
        const std::span<const std::byte> guarded_direct_section{
            direct_section_begin, image.size()};

        auto opened = idx::open_compact_h32_direct_index_bounded(
            std::move(mapped_owner), guarded_direct_section, open_options);
        REQUIRE(opened.has_value());
        CHECK(opened->image_owner().get() == direct_section_begin);
        CHECK(opened->view().image.size() == image.size());
        CHECK(opened->view().header.header_crc64
            == reference_read_le<std::uint64_t>(
                image, idx::kCompactH32HeaderCrcOffset));
        CHECK(opened->evidence().header_bytes_examined
            == idx::kCompactH32DirectIndexHeaderBytes);
        CHECK(opened->evidence().payload_bytes_examined == 0u);
        CHECK_FALSE(mapping_lifetime.expired());
    }
    CHECK(mapping_lifetime.expired());
#else
    SUCCEED("guard-page proof requires a POSIX mmap implementation");
#endif
}

TEST_CASE(
    "compact exact-domain H32 monotonic batch derives dense ranks and omits "
    "globally rejected max-occ runs",
    "[doc0383][p4][compact-h32][rank][max-occ][differential]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 0u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(20u, false)},
            },
            CompactTestRun{
                .header = {.hash = 255u, .global_count = 3u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(1u, false),
                    encoded_position(9u, false),
                    encoded_position(5u, true),
                },
            },
            CompactTestRun{
                .header = {.hash = 256u, .global_count = 2u,
                    .target_strand_zero_count = 0u},
                .encoded_positions = {
                    encoded_position(10u, true),
                    encoded_position(20u, true),
                },
            },
            // Count four exceeds the compatibility-bound max_occ=3.  Neither
            // its membership bit, descriptor, nor postings are persisted.
            CompactTestRun{
                .header = {.hash = 300u, .global_count = 4u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(2u, false),
                    encoded_position(4u, false),
                    encoded_position(6u, true),
                    encoded_position(8u, true),
                },
            },
            CompactTestRun{
                .header = {.hash = 511u, .global_count = 3u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {
                    encoded_position(3u, false),
                    encoded_position(7u, true),
                    encoded_position(11u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto planned = idx::plan_compact_h32_direct_index(
        compact_source_ref(source), build_options);
    REQUIRE(planned.has_value());
    CHECK(planned->source_hash_count == 5u);
    CHECK(planned->source_posting_count == 13u);
    CHECK(planned->accepted_hash_count == 4u);
    CHECK(planned->accepted_posting_count == 9u);
    CHECK(planned->wire_version == idx::kCompactH32DirectIndexVersionV2);
    CHECK(planned->descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV2);
    CHECK(planned->inline_singleton_count == 1u);
    CHECK(planned->materialized_hash_count == 3u);
    CHECK(planned->materialized_posting_count == 8u);
    CHECK(planned->rejected_hash_count == 1u);
    CHECK(planned->rejected_posting_count == 4u);
    CHECK(planned->descriptors_bytes == 4u * 8u);
    CHECK(planned->postings_bytes == 8u * 4u);

    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.sorted_stream_crc64 == planned->sorted_stream_crc64);
    const std::vector<idx::CompactH32QueryOccurrence> queries{
        {.hash = 0u, .query_strand_one = false},
        {.hash = 1u, .query_strand_one = false},
        {.hash = 255u, .query_strand_one = false},
        {.hash = 255u, .query_strand_one = true},
        {.hash = 256u, .query_strand_one = false},
        {.hash = 300u, .query_strand_one = false},
        {.hash = 511u, .query_strand_one = true},
    };
    const auto query_plan = idx::query_compact_h32_direct_index(
        view, queries, 3u);
    REQUIRE(query_plan.has_value());
    REQUIRE(query_plan->results.size() == queries.size());
    CHECK(query_plan->results[0].accepted);
    CHECK(query_plan->results[0].dense_ordinal == 0u);
    CHECK_FALSE(query_plan->results[1].accepted);
    CHECK(query_plan->results[2].accepted);
    CHECK(query_plan->results[2].dense_ordinal == 1u);
    CHECK(query_plan->results[2].forward_count == 2u);
    CHECK(query_plan->results[2].reverse_count == 1u);
    CHECK(query_plan->results[3].selection_ordinal
        == query_plan->results[2].selection_ordinal);
    CHECK(query_plan->results[3].forward_count == 1u);
    CHECK(query_plan->results[3].reverse_count == 2u);
    CHECK(query_plan->results[4].dense_ordinal == 2u);
    CHECK(query_plan->results[4].forward_count == 0u);
    CHECK(query_plan->results[4].reverse_count == 2u);
    CHECK_FALSE(query_plan->results[5].accepted);
    CHECK(query_plan->results[6].dense_ordinal == 3u);
    CHECK(query_plan->evidence.rank_checkpoints_examined == 2u);
    CHECK(query_plan->evidence.descriptors_examined == 4u);
    CHECK(query_plan->evidence.unique_selected_hash_count == 4u);
    CHECK(query_plan->evidence.selected_posting_count == 9u);
    CHECK(query_plan->evidence.selected_inline_singleton_count == 1u);
    CHECK(query_plan->evidence.selected_materialized_hash_count == 3u);
    CHECK(query_plan->evidence.selected_materialized_posting_count == 8u);

    CollectedPostings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(
            query_plan->selections.size()),
    };
    const auto visited = idx::visit_compact_h32_direct_index_postings(
        view, *query_plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == 9u);
    REQUIRE(collected.by_selection.size() == 4u);
    CHECK(collected.by_selection[0]
        == source.runs[0].encoded_positions);
    CHECK(collected.by_selection[1]
        == source.runs[1].encoded_positions);
    CHECK(collected.by_selection[2]
        == source.runs[2].encoded_positions);
    CHECK(collected.by_selection[3]
        == source.runs[4].encoded_positions);

    const std::array absent_query{
        idx::CompactH32QueryOccurrence{
            .hash = 1u, .query_strand_one = false},
    };
    const auto absent = idx::query_compact_h32_direct_index(
        view, absent_query, 3u);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->results.front().accepted);
    CHECK(absent->evidence.rank_checkpoints_examined == 1u);
    CHECK(absent->evidence.membership_words_examined == 1u);
    CHECK(absent->evidence.descriptors_examined == 0u);

    const std::array unsorted{
        idx::CompactH32QueryOccurrence{
            .hash = 256u, .query_strand_one = false},
        idx::CompactH32QueryOccurrence{
            .hash = 255u, .query_strand_one = false},
    };
    const auto bad_order = idx::query_compact_h32_direct_index(
        view, unsorted, 3u);
    REQUIRE_FALSE(bad_order.has_value());
    CHECK(bad_order.error() == idx::DirectInvertedIndexError::order);
}

TEST_CASE(
    "compact H32 unique lookup caches one four-word membership rank group "
    "across present absent and boundary hashes",
    "[compact-h32][unique-query][rank-group-cache][membership-evidence]"
    "[differential][projected-hash]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 0u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(1u, false)},
            },
            CompactTestRun{
                .header = {.hash = 63u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(2u, false)},
            },
            CompactTestRun{
                .header = {.hash = 64u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(3u, false)},
            },
            CompactTestRun{
                .header = {.hash = 127u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(4u, false)},
            },
            CompactTestRun{
                .header = {.hash = 128u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(5u, false)},
            },
            CompactTestRun{
                .header = {.hash = 191u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(6u, false)},
            },
            CompactTestRun{
                .header = {.hash = 192u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(7u, false)},
            },
            CompactTestRun{
                .header = {.hash = 255u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(8u, false)},
            },
            CompactTestRun{
                .header = {.hash = 256u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(9u, false)},
            },
            CompactTestRun{
                .header = {.hash = 319u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(10u, false)},
            },
            CompactTestRun{
                .header = {.hash = 320u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(11u, false)},
            },
            CompactTestRun{
                .header = {.hash = 383u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(12u, false)},
            },
            CompactTestRun{
                .header = {.hash = 384u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(13u, false)},
            },
            CompactTestRun{
                .header = {.hash = 447u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(14u, false)},
            },
            CompactTestRun{
                .header = {.hash = 448u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(15u, false)},
            },
            CompactTestRun{
                .header = {.hash = 511u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(16u, false)},
            },
        },
    };
    const auto build_options = compact_options(100u, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    const std::array<std::uint32_t, 26u> hashes{
        0u, 1u, 63u, 64u, 65u, 127u, 128u, 129u, 191u, 192u,
        193u, 254u, 255u, 256u, 257u, 319u, 320u, 321u, 383u,
        384u, 385u, 447u, 448u, 449u, 510u, 511u,
    };

    const std::span<const std::uint32_t> first_rank_group{
        hashes.data(), 13u};
    const auto same_group = idx::query_compact_h32_direct_index_unique(
        view, first_rank_group, 1u);
    REQUIRE(same_group.has_value());
    CHECK(same_group->selections.size() == 8u);
    CHECK(same_group->evidence.rank_checkpoints_examined == 1u);
    // Thirteen present/absent lookups physically load exactly one cached
    // four-word group, not one to four words again for every hash.
    CHECK(same_group->evidence.membership_words_examined == 4u);
    CHECK(same_group->evidence.descriptors_examined == 8u);

    const auto cached = idx::query_compact_h32_direct_index_unique(
        view, hashes, 1u);
    REQUIRE(cached.has_value());
    CHECK(cached->selections.size() == source.runs.size());
    CHECK(cached->evidence.rank_checkpoints_examined == 2u);
    CHECK(cached->evidence.membership_words_examined == 8u);
    CHECK(cached->evidence.descriptors_examined == source.runs.size());
    REQUIRE(cached->selections.size() == 16u);
    CHECK(cached->selections[7u].query_ordinal == 12u);
    CHECK(cached->selections[7u].dense_ordinal == 7u);
    CHECK(cached->selections[8u].query_ordinal == 13u);
    CHECK(cached->selections[8u].dense_ordinal == 8u);

    struct ProjectedHashRecord {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(ProjectedHashRecord) == 16u);
    std::array<ProjectedHashRecord, 26u> projected_hashes{};
    for (std::size_t ordinal = 0u; ordinal < hashes.size(); ++ordinal) {
        projected_hashes[ordinal] = ProjectedHashRecord{
            .hash = hashes[ordinal],
            .occurrence_ref_begin = static_cast<std::uint32_t>(ordinal),
            .occurrence_ref_count = static_cast<std::uint32_t>(ordinal + 1u),
            .global_occurrence_count = static_cast<std::uint32_t>(
                ordinal + 2u),
        };
    }
    std::size_t projected_hash_reads = 0u;
    const auto projected =
        idx::query_compact_h32_direct_index_unique_projected(
            view,
            projected_hashes.size(),
            [&projected_hashes, &projected_hash_reads](
                    std::size_t ordinal) noexcept
                    -> std::uint32_t {
                ++projected_hash_reads;
                return projected_hashes[ordinal].hash;
            },
            1u);
    REQUIRE(projected.has_value());
    CHECK(projected_hash_reads == projected_hashes.size());
    CHECK(projected->evidence == cached->evidence);
    REQUIRE(projected->selections.size() == cached->selections.size());
    for (std::size_t ordinal = 0u;
         ordinal < cached->selections.size(); ++ordinal) {
        CHECK(projected->selections[ordinal].query_ordinal
            == cached->selections[ordinal].query_ordinal);
        CHECK(projected->selections[ordinal].dense_ordinal
            == cached->selections[ordinal].dense_ordinal);
    }
    const auto projected_accessor =
        idx::bind_compact_h32_direct_index_selection_spans(
            view, *projected);
    REQUIRE(projected_accessor.has_value());
    CHECK(projected_accessor->selection_count()
        == projected->selections.size());

    std::vector<idx::CompactH32QueryOccurrence> occurrence_queries;
    occurrence_queries.reserve(hashes.size());
    for (const std::uint32_t hash : hashes) {
        occurrence_queries.push_back(idx::CompactH32QueryOccurrence{
            .hash = hash,
            .query_strand_one = false,
        });
    }
    const auto reference = idx::query_compact_h32_direct_index(
        view, occurrence_queries, 1u);
    REQUIRE(reference.has_value());
    REQUIRE(reference->results.size() == hashes.size());
    std::size_t selected_ordinal = 0u;
    for (std::size_t query_ordinal = 0u;
         query_ordinal < hashes.size(); ++query_ordinal) {
        if (!reference->results[query_ordinal].accepted) continue;
        REQUIRE(selected_ordinal < cached->selections.size());
        CHECK(cached->selections[selected_ordinal].query_ordinal
            == query_ordinal);
        CHECK(cached->selections[selected_ordinal].dense_ordinal
            == reference->results[query_ordinal].dense_ordinal);
        ++selected_ordinal;
    }
    CHECK(selected_ordinal == cached->selections.size());
}

TEST_CASE(
    "compact H32 dual reader preserves V1/V2 P32 semantic parity while V2 "
    "inlines singletons",
    "[compact-h32][v1][v2][dual-read][p32][inline-singleton][parity]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(4u, false)},
            },
            CompactTestRun{
                .header = {.hash = 5u, .global_count = 3u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(5u, false),
                    encoded_position(7u, false),
                    encoded_position(9u, true),
                },
            },
            CompactTestRun{
                .header = {.hash = 7u, .global_count = 1u,
                    .target_strand_zero_count = 0u},
                .encoded_positions = {encoded_position(11u, true)},
            },
        },
    };
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto v2_image = build_compact_or_fail(source, build_options);
    const auto v1_image = reference_convert_compact_v2_to_v1(v2_image);
    const auto v2_view = validate_compact_or_fail(v2_image, build_options);
    const auto v1_view = validate_compact_or_fail(v1_image, build_options);

    CHECK(v2_view.header.wire_version
        == idx::kCompactH32DirectIndexVersionV2);
    CHECK(v2_view.header.descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV2);
    CHECK(v2_view.header.inline_singleton_count == 2u);
    CHECK(v2_view.header.materialized_hash_count == 1u);
    CHECK(v2_view.header.materialized_posting_count == 3u);
    CHECK(v2_view.header.descriptors_bytes == 3u * 8u);
    CHECK(v2_view.header.postings_bytes == 3u * 4u);
    CHECK(v1_view.header.wire_version
        == idx::kCompactH32DirectIndexVersionV1);
    CHECK(v1_view.header.descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV1);
    CHECK(v1_view.header.inline_singleton_count == 0u);
    CHECK(v1_view.header.materialized_hash_count == 3u);
    CHECK(v1_view.header.materialized_posting_count == 5u);
    CHECK(v1_view.header.descriptors_bytes == 3u * 12u);
    CHECK(v1_view.header.postings_bytes == 5u * 4u);
    const auto bounded_v1 = open_compact_bounded_owned(
        v1_image, compact_bounded_options(build_options));
    REQUIRE(bounded_v1.has_value());
    CHECK(bounded_v1->view().header.wire_version
        == idx::kCompactH32DirectIndexVersionV1);
    auto wide_cap_v1_image = v1_image;
    constexpr std::uint32_t legacy_wide_cap = 70'000u;
    reference_write_le<std::uint32_t>(
        wide_cap_v1_image, 56u, legacy_wide_cap);
    reference_reseal_compact_h32_header(wide_cap_v1_image);
    auto wide_cap_options = build_options;
    wide_cap_options.max_occ_cap = legacy_wide_cap;
    const auto wide_cap_v1 = validate_compact_or_fail(
        wide_cap_v1_image, wide_cap_options);
    CHECK(wide_cap_v1.header.wire_version
        == idx::kCompactH32DirectIndexVersionV1);
    CHECK(wide_cap_v1.header.max_occ_cap == legacy_wide_cap);

    const std::array queries{
        idx::CompactH32QueryOccurrence{
            .hash = 2u, .query_strand_one = false},
        idx::CompactH32QueryOccurrence{
            .hash = 5u, .query_strand_one = true},
        idx::CompactH32QueryOccurrence{
            .hash = 7u, .query_strand_one = false},
    };
    const auto v2_plan = idx::query_compact_h32_direct_index(
        v2_view, queries, 3u);
    const auto v1_plan = idx::query_compact_h32_direct_index(
        v1_view, queries, 3u);
    REQUIRE(v2_plan.has_value());
    REQUIRE(v1_plan.has_value());
    REQUIRE(v2_plan->results.size() == v1_plan->results.size());
    for (std::size_t ordinal = 0u;
         ordinal < v2_plan->results.size();
         ++ordinal) {
        CHECK(v2_plan->results[ordinal].accepted
            == v1_plan->results[ordinal].accepted);
        CHECK(v2_plan->results[ordinal].dense_ordinal
            == v1_plan->results[ordinal].dense_ordinal);
        CHECK(v2_plan->results[ordinal].global_count
            == v1_plan->results[ordinal].global_count);
        CHECK(v2_plan->results[ordinal].forward_count
            == v1_plan->results[ordinal].forward_count);
        CHECK(v2_plan->results[ordinal].reverse_count
            == v1_plan->results[ordinal].reverse_count);
    }
    CHECK(v2_plan->evidence.selected_inline_singleton_count == 2u);
    CHECK(v2_plan->evidence.selected_materialized_hash_count == 1u);
    CHECK(v2_plan->evidence.selected_materialized_posting_count == 3u);
    CHECK(v1_plan->evidence.selected_inline_singleton_count == 0u);
    CHECK(v1_plan->evidence.selected_materialized_hash_count == 3u);
    CHECK(v1_plan->evidence.selected_materialized_posting_count == 5u);

    const auto collect_legacy = [](const auto& view, const auto& plan) {
        CollectedPostings collected{
            .by_selection =
                std::vector<std::vector<std::uint64_t>>(
                    plan.selections.size()),
        };
        const auto visited = idx::visit_compact_h32_direct_index_postings(
            view, plan, &collected, &collect_posting);
        REQUIRE(visited.has_value());
        return collected.by_selection;
    };
    const auto v2_legacy = collect_legacy(v2_view, *v2_plan);
    const auto v1_legacy = collect_legacy(v1_view, *v1_plan);
    CHECK(v2_legacy == v1_legacy);
    REQUIRE(v2_legacy.size() == source.runs.size());
    for (std::size_t ordinal = 0u;
         ordinal < source.runs.size();
         ++ordinal) {
        CHECK(v2_legacy[ordinal]
            == source.runs[ordinal].encoded_positions);
    }

    const std::array<std::uint32_t, 3u> unique_hashes{2u, 5u, 7u};
    const auto v2_unique = idx::query_compact_h32_direct_index_unique(
        v2_view, unique_hashes, 3u);
    const auto v1_unique = idx::query_compact_h32_direct_index_unique(
        v1_view, unique_hashes, 3u);
    REQUIRE(v2_unique.has_value());
    REQUIRE(v1_unique.has_value());
    struct BulkResult {
        std::vector<std::tuple<std::uint32_t, std::uint32_t,
            std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t>>
            order{};
        std::vector<std::vector<std::uint64_t>> postings{};
        idx::CompactH32SelectionSpanVisitEvidence evidence{};
    };
    const auto collect_bulk = [](const auto& view, const auto& plan) {
        BulkResult result{
            .postings = std::vector<std::vector<std::uint64_t>>(
                plan.selections.size()),
        };
        bool decoded = true;
        const auto visited =
            idx::visit_compact_h32_direct_index_selection_spans(
                view,
                plan,
                [&](const idx::CompactH32SelectionPostingSpans& selected) {
                    result.order.emplace_back(
                        selected.selection_ordinal,
                        selected.dense_ordinal,
                        selected.query_ordinal,
                        selected.global_count,
                        selected.strand_zero_count,
                        selected.span_count);
                    for (std::uint8_t span_ordinal = 0u;
                         span_ordinal < selected.span_count;
                         ++span_ordinal) {
                        const auto& span = selected.spans[span_ordinal];
                        for (std::uint64_t ordinal = 0u;
                             ordinal < span.count;
                             ++ordinal) {
                            const auto encoded =
                                span.encoded_position_at(ordinal);
                            if (!encoded) {
                                decoded = false;
                                return false;
                            }
                            result.postings[selected.selection_ordinal]
                                .push_back(*encoded);
                        }
                    }
                    return true;
                });
        CHECK(decoded);
        REQUIRE(visited.has_value());
        result.evidence = *visited;
        return result;
    };
    const BulkResult v2_bulk = collect_bulk(v2_view, *v2_unique);
    const BulkResult v1_bulk = collect_bulk(v1_view, *v1_unique);
    CHECK(v2_bulk.order == v1_bulk.order);
    CHECK(v2_bulk.postings == v1_bulk.postings);
    CHECK(v2_bulk.evidence.selection_callbacks == 3u);
    CHECK(v2_bulk.evidence.inline_selection_callbacks == 2u);
    CHECK(v2_bulk.evidence.materialized_selection_callbacks == 1u);
    CHECK(v2_bulk.evidence.inline_postings_exposed == 2u);
    CHECK(v2_bulk.evidence.materialized_postings_exposed == 3u);
    CHECK(v1_bulk.evidence.selection_callbacks == 3u);
    CHECK(v1_bulk.evidence.inline_selection_callbacks == 0u);
    CHECK(v1_bulk.evidence.materialized_selection_callbacks == 3u);
    CHECK(v1_bulk.evidence.inline_postings_exposed == 0u);
    CHECK(v1_bulk.evidence.materialized_postings_exposed == 5u);
    CHECK(v2_bulk.evidence.rank_checkpoints_examined == 0u);
    CHECK(v2_bulk.evidence.membership_words_examined == 0u);
    CHECK(v1_bulk.evidence.rank_checkpoints_examined == 0u);
    CHECK(v1_bulk.evidence.membership_words_examined == 0u);
}

TEST_CASE(
    "compact exact-domain H32 codec preserves P64 accepted postings",
    "[doc0383][p4][compact-h32][p64][selection-range][projected-hash]") {
    constexpr std::uint64_t target_length =
        (std::uint64_t{1u} << 31u) + 100u;
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 0u},
                .encoded_positions = {
                    encoded_position(
                        (std::uint64_t{1u} << 31u) + 7u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(target_length, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.wire_version
        == idx::kCompactH32DirectIndexVersionV2);
    CHECK(view.header.descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV2);
    CHECK(view.header.posting_width_bytes == 8u);
    CHECK(view.header.inline_singleton_count == 0u);
    CHECK(view.header.materialized_hash_count == 1u);
    CHECK(view.header.materialized_posting_count == 1u);
    const std::array query{
        idx::CompactH32QueryOccurrence{
            .hash = 2u, .query_strand_one = false},
    };
    const auto plan = idx::query_compact_h32_direct_index(view, query, 1u);
    REQUIRE(plan.has_value());
    CollectedPostings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_compact_h32_direct_index_postings(
        view, *plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(collected.by_selection.front()
        == source.runs.front().encoded_positions);

    const std::array<std::uint32_t, 1u> unique_hashes{2u};
    const auto unique_plan = idx::query_compact_h32_direct_index_unique(
        view, unique_hashes, 1u);
    REQUIRE(unique_plan.has_value());
    REQUIRE(unique_plan->selections.size() == 1u);
    CHECK(unique_plan->selections.front().dense_ordinal == 0u);
    CHECK(unique_plan->selections.front().query_ordinal == 0u);
    CHECK(unique_plan->evidence.selected_inline_singleton_count == 0u);
    CHECK(unique_plan->evidence.selected_materialized_hash_count == 1u);
    CHECK(unique_plan->evidence.selected_materialized_posting_count == 1u);
    struct ProjectedP64HashRecord {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(ProjectedP64HashRecord) == 16u);
    const std::array projected_hashes{
        ProjectedP64HashRecord{
            .hash = 2u,
            .occurrence_ref_begin = 3u,
            .occurrence_ref_count = 5u,
            .global_occurrence_count = 7u,
        },
    };
    std::size_t projected_hash_reads = 0u;
    const auto projected_plan =
        idx::query_compact_h32_direct_index_unique_projected(
            view,
            projected_hashes.size(),
            [&projected_hashes, &projected_hash_reads](
                    std::size_t ordinal) noexcept
                    -> std::uint32_t {
                ++projected_hash_reads;
                return projected_hashes[ordinal].hash;
            },
            1u);
    REQUIRE(projected_plan.has_value());
    CHECK(projected_hash_reads == projected_hashes.size());
    CHECK(projected_plan->evidence == unique_plan->evidence);
    REQUIRE(projected_plan->selections.size()
        == unique_plan->selections.size());
    CHECK(projected_plan->selections.front().dense_ordinal
        == unique_plan->selections.front().dense_ordinal);
    CHECK(projected_plan->selections.front().query_ordinal
        == unique_plan->selections.front().query_ordinal);
    const auto projected_p64_accessor =
        idx::bind_compact_h32_direct_index_selection_spans(
            view, *projected_plan);
    REQUIRE(projected_p64_accessor.has_value());
    CHECK(projected_p64_accessor->selection_count() == 1u);
    std::uint64_t selection_callback_count = 0u;
    std::vector<std::uint64_t> bulk_positions;
    bool span_shape_valid = true;
    const auto bulk_visited =
        idx::visit_compact_h32_direct_index_selection_spans(
            view,
            *unique_plan,
            [&](const idx::CompactH32SelectionPostingSpans& selected) {
                ++selection_callback_count;
                if (selected.selection_ordinal != 0u
                    || selected.dense_ordinal != 0u
                    || selected.query_ordinal != 0u
                    || selected.global_count != 1u
                    || selected.strand_zero_count != 0u
                    || selected.span_count != 1u) {
                    span_shape_valid = false;
                    return false;
                }
                const auto& span = selected.spans.front();
                if (span.count != 1u
                    || span.posting_width_bytes != 8u
                    || span.encoded_bytes.size() != 8u
                    || !span.target_strand_one
                    || span.target_length != target_length) {
                    span_shape_valid = false;
                    return false;
                }
                const auto encoded = span.encoded_position_at(0u);
                if (!encoded) {
                    span_shape_valid = false;
                    return false;
                }
                bulk_positions.push_back(*encoded);
                return true;
            });
    CHECK(span_shape_valid);
    REQUIRE(bulk_visited.has_value());
    CHECK(selection_callback_count == 1u);
    CHECK(bulk_visited->selection_callbacks == 1u);
    CHECK(bulk_visited->spans_exposed == 1u);
    CHECK(bulk_visited->postings_exposed == 1u);
    CHECK(bulk_visited->inline_selection_callbacks == 0u);
    CHECK(bulk_visited->materialized_selection_callbacks == 1u);
    CHECK(bulk_visited->inline_postings_exposed == 0u);
    CHECK(bulk_visited->materialized_postings_exposed == 1u);
    CHECK(bulk_visited->descriptors_examined == 1u);
    CHECK(bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(bulk_visited->membership_words_examined == 0u);
    CHECK(bulk_positions == source.runs.front().encoded_positions);

    const auto p64_range_accessor =
        idx::bind_compact_h32_direct_index_selection_spans(
            view, *unique_plan);
    REQUIRE(p64_range_accessor.has_value());
    std::vector<std::uint64_t> p64_range_positions;
    std::optional<idx::CompactH32PostingSpanView> p64_span;
    const auto p64_range_visited =
        idx::visit_compact_h32_direct_index_selection_span_range(
            *p64_range_accessor,
            0u,
            1u,
            [&](const idx::CompactH32SelectionPostingSpans& selected) {
                if (selected.selection_ordinal != 0u
                    || selected.span_count != 1u
                    || selected.spans.front().posting_width_bytes != 8u) {
                    return false;
                }
                p64_span = selected.spans.front();
                const auto decoded =
                    selected.spans.front().visit_encoded_positions(
                        [&](std::uint64_t encoded) {
                            p64_range_positions.push_back(encoded);
                            return true;
                        });
                return decoded.has_value() && *decoded == 1u;
            });
    REQUIRE(p64_range_visited.has_value());
    CHECK(p64_range_visited->selection_callbacks == 1u);
    CHECK(p64_range_visited->postings_exposed == 1u);
    CHECK(p64_range_visited->materialized_postings_exposed == 1u);
    CHECK(p64_range_positions == bulk_positions);
    REQUIRE(p64_span.has_value());

    auto truncated_p64_span = *p64_span;
    truncated_p64_span.encoded_bytes =
        truncated_p64_span.encoded_bytes.first(7u);
    std::uint64_t truncated_callbacks = 0u;
    const auto truncated_p64 =
        truncated_p64_span.visit_encoded_positions(
            [&](std::uint64_t) {
                ++truncated_callbacks;
                return true;
            });
    REQUIRE_FALSE(truncated_p64.has_value());
    CHECK(truncated_p64.error()
        == idx::DirectInvertedIndexError::query_plan);
    CHECK(truncated_callbacks == 0u);

    auto wrong_strand_p64_span = *p64_span;
    wrong_strand_p64_span.target_strand_one = false;
    std::uint64_t wrong_strand_callbacks = 0u;
    const auto wrong_strand_p64 =
        wrong_strand_p64_span.visit_encoded_positions(
            [&](std::uint64_t) {
                ++wrong_strand_callbacks;
                return true;
            });
    REQUIRE_FALSE(wrong_strand_p64.has_value());
    CHECK(wrong_strand_p64.error()
        == idx::DirectInvertedIndexError::query_plan);
    CHECK(wrong_strand_callbacks == 0u);

    auto out_of_range_p64_span = *p64_span;
    out_of_range_p64_span.target_length = 1u;
    std::uint64_t out_of_range_callbacks = 0u;
    const auto out_of_range_p64 =
        out_of_range_p64_span.visit_encoded_positions(
            [&](std::uint64_t) {
                ++out_of_range_callbacks;
                return true;
            });
    REQUIRE_FALSE(out_of_range_p64.has_value());
    CHECK(out_of_range_p64.error()
        == idx::DirectInvertedIndexError::query_plan);
    CHECK(out_of_range_callbacks == 0u);

    std::uint64_t rejected_callbacks = 0u;
    const auto rejected_p64 = p64_span->visit_encoded_positions(
        [&](std::uint64_t) {
            ++rejected_callbacks;
            return false;
        });
    REQUIRE_FALSE(rejected_p64.has_value());
    CHECK(rejected_p64.error()
        == idx::DirectInvertedIndexError::visitor_rejected);
    CHECK(rejected_callbacks == 1u);

    const auto v1_image = reference_convert_compact_v2_to_v1(image);
    const auto v1_view = validate_compact_or_fail(v1_image, build_options);
    CHECK(v1_view.header.wire_version
        == idx::kCompactH32DirectIndexVersionV1);
    CHECK(v1_view.header.descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV1);
    CHECK(v1_view.header.inline_singleton_count == 0u);
    CHECK(v1_view.header.materialized_hash_count == 1u);
    CHECK(v1_view.header.materialized_posting_count == 1u);
    const auto v1_unique = idx::query_compact_h32_direct_index_unique(
        v1_view, unique_hashes, 1u);
    REQUIRE(v1_unique.has_value());
    const auto cross_version_plan_binding =
        idx::bind_compact_h32_direct_index_selection_spans(
            v1_view, *unique_plan);
    REQUIRE_FALSE(cross_version_plan_binding.has_value());
    CHECK(cross_version_plan_binding.error()
        == idx::DirectInvertedIndexError::query_plan);
    std::vector<std::uint64_t> v1_bulk_positions;
    const auto v1_bulk_visited =
        idx::visit_compact_h32_direct_index_selection_spans(
            v1_view,
            *v1_unique,
            [&](const idx::CompactH32SelectionPostingSpans& selected) {
                if (selected.span_count != 1u) return false;
                const auto encoded =
                    selected.spans.front().encoded_position_at(0u);
                if (!encoded) return false;
                v1_bulk_positions.push_back(*encoded);
                return true;
            });
    REQUIRE(v1_bulk_visited.has_value());
    CHECK(v1_bulk_visited->inline_selection_callbacks == 0u);
    CHECK(v1_bulk_visited->materialized_selection_callbacks == 1u);
    CHECK(v1_bulk_visited->inline_postings_exposed == 0u);
    CHECK(v1_bulk_visited->materialized_postings_exposed == 1u);
    CHECK(v1_bulk_positions == bulk_positions);
}

TEST_CASE(
    "compact H32 V1/V2 readers reject forged descriptor interpretation after "
    "payload checksums are resealed",
    "[compact-h32][v1][v2][descriptor][forged-image][safety]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(4u, false)},
            },
            CompactTestRun{
                .header = {.hash = 5u, .global_count = 2u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {
                    encoded_position(6u, false),
                    encoded_position(8u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto v2_image = build_compact_or_fail(source, build_options);
    const std::uint64_t descriptors_offset =
        reference_read_le<std::uint64_t>(v2_image, 144u);

    SECTION("an inline singleton cannot forge its persisted strand") {
        auto forged = v2_image;
        reference_write_le<std::uint32_t>(forged, descriptors_offset,
            static_cast<std::uint32_t>(encoded_position(4u, true)));
        reference_reseal_compact_h32_payload_and_header(forged);
        const auto validated = idx::validate_compact_h32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::DirectInvertedIndexError::posting);
    }

    SECTION("a materialized descriptor cannot be retyped as an extra inline") {
        auto forged = v2_image;
        const std::uint64_t second_descriptor = descriptors_offset
            + idx::kCompactH32DirectIndexDescriptorBytesV2;
        reference_write_le<std::uint16_t>(
            forged, second_descriptor + 4u, 1u);
        reference_reseal_compact_h32_payload_and_header(forged);
        const auto validated = idx::validate_compact_h32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::DirectInvertedIndexError::dictionary);
    }

    SECTION("a materialized posting ordinal remains exact") {
        auto forged = v2_image;
        const std::uint64_t second_descriptor = descriptors_offset
            + idx::kCompactH32DirectIndexDescriptorBytesV2;
        reference_write_le<std::uint32_t>(forged, second_descriptor, 99u);
        reference_reseal_compact_h32_payload_and_header(forged);
        const auto validated = idx::validate_compact_h32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::DirectInvertedIndexError::control);
    }

    SECTION("the legacy V1 count field remains checked") {
        auto forged = reference_convert_compact_v2_to_v1(v2_image);
        const std::uint64_t v1_descriptors_offset =
            reference_read_le<std::uint64_t>(forged, 144u);
        reference_write_le<std::uint32_t>(
            forged, v1_descriptors_offset + 4u, 0u);
        reference_reseal_compact_h32_payload_and_header(forged);
        const auto validated = idx::validate_compact_h32_direct_index(
            forged,
            build_options.identity,
            build_options.max_occ_cap,
            build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error()
            == idx::DirectInvertedIndexError::control);
    }
}

TEST_CASE(
    "compact exact-domain H32 rejects a forged image plan before touching "
    "the destination",
    "[doc0383][p4][compact-h32][plan][canary]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 7u, .global_count = 2u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(3u, false),
                    encoded_position(9u, false),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto planned = idx::plan_compact_h32_direct_index(
        compact_source_ref(source), build_options);
    REQUIRE(planned.has_value());
    auto forged = *planned;
    forged.descriptors_offset += 64u;

    constexpr std::byte canary{0xa5};
    std::vector<std::byte> guarded(
        static_cast<std::size_t>(planned->image_bytes) + 32u, canary);
    const auto destination = std::span<std::byte>{guarded}.first(
        static_cast<std::size_t>(planned->image_bytes));
    const auto built = idx::build_compact_h32_direct_index_into(
        compact_source_ref(source), build_options, forged, destination);
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error() == idx::DirectInvertedIndexError::compatibility);

    auto forged_v1_write = *planned;
    forged_v1_write.wire_version = idx::kCompactH32DirectIndexVersionV1;
    forged_v1_write.descriptor_width_bytes =
        idx::kCompactH32DirectIndexDescriptorBytesV1;
    const auto v1_write = idx::build_compact_h32_direct_index_into(
        compact_source_ref(source),
        build_options,
        forged_v1_write,
        destination);
    REQUIRE_FALSE(v1_write.has_value());
    CHECK(v1_write.error()
        == idx::DirectInvertedIndexError::compatibility);
    CHECK(source.scans == 1u);
    CHECK(std::all_of(guarded.begin(), guarded.end(),
        [canary](std::byte value) { return value == canary; }));
}

TEST_CASE(
    "compact exact-domain H32 bounds flapping replay sources before every "
    "planned section write",
    "[doc0383][p4][compact-h32][replay][canary]") {
    const auto build_options = compact_options(100u, 3u, 9u);
    const auto require_replay_rejected = [&build_options](
            CompactTestSource& source) {
        const auto planned = idx::plan_compact_h32_direct_index(
            compact_source_ref(source), build_options);
        REQUIRE(planned.has_value());
        constexpr std::byte canary{0xa5};
        std::vector<std::byte> guarded(
            static_cast<std::size_t>(planned->image_bytes) + 32u, canary);
        const auto destination = std::span<std::byte>{guarded}.first(
            static_cast<std::size_t>(planned->image_bytes));
        const auto built = idx::build_compact_h32_direct_index_into(
            compact_source_ref(source), build_options, *planned,
            destination);
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error()
            == idx::DirectInvertedIndexError::replay_mismatch);
        CHECK(source.scans == 2u);
        const auto tail = std::span<const std::byte>{guarded}.subspan(
            static_cast<std::size_t>(planned->image_bytes));
        CHECK(std::all_of(tail.begin(), tail.end(),
            [canary](std::byte value) { return value == canary; }));
    };

    SECTION("replay adds an accepted run") {
        CompactTestSource source{
            .runs = {
                CompactTestRun{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {
                CompactTestRun{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
                CompactTestRun{
                    .header = {.hash = 30u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(3u, false)},
                },
            },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay removes an accepted run") {
        CompactTestSource source{
            .runs = {
                CompactTestRun{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {},
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay changes an accepted posting count") {
        CompactTestSource source{
            .runs = {
                CompactTestRun{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {
                CompactTestRun{
                    .header = {.hash = 20u, .global_count = 2u,
                        .target_strand_zero_count = 2u},
                    .encoded_positions = {
                        encoded_position(2u, false),
                        encoded_position(4u, false),
                    },
                },
            },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }

    SECTION("replay changes a hash without changing section sizes") {
        CompactTestSource source{
            .runs = {
                CompactTestRun{
                    .header = {.hash = 20u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .replay_runs = {
                CompactTestRun{
                    .header = {.hash = 21u, .global_count = 1u,
                        .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(2u, false)},
                },
            },
            .use_replay_runs_after_first_scan = true,
        };
        require_replay_rejected(source);
    }
}

TEST_CASE(
    "compact exact-domain H32 point lookup decodes one descriptor among many",
    "[doc0383][p4][compact-h32][point-lookup][evidence]") {
    CompactTestSource source;
    for (std::uint32_t hash = 0u; hash < 64u; ++hash) {
        source.runs.push_back(CompactTestRun{
            .header = {.hash = hash, .global_count = 1u,
                .target_strand_zero_count = 1u},
            .encoded_positions = {encoded_position(hash, false)},
        });
    }
    const auto build_options = compact_options(100u, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.wire_version
        == idx::kCompactH32DirectIndexVersionV2);
    CHECK(view.header.descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV2);
    CHECK(view.header.inline_singleton_count == 64u);
    CHECK(view.header.materialized_hash_count == 0u);
    CHECK(view.header.materialized_posting_count == 0u);
    CHECK(view.header.postings_bytes == 0u);
    REQUIRE(view.header.accepted_hash_count == 64u);

    const std::array present_query{
        idx::CompactH32QueryOccurrence{
            .hash = 63u, .query_strand_one = false},
    };
    const auto present = idx::query_compact_h32_direct_index(
        view, present_query, 1u);
    REQUIRE(present.has_value());
    REQUIRE(present->results.front().accepted);
    CHECK(present->results.front().dense_ordinal == 63u);
    CHECK(present->evidence.descriptors_examined == 1u);
    CHECK(present->evidence.unique_selected_hash_count == 1u);

    const std::array absent_query{
        idx::CompactH32QueryOccurrence{
            .hash = 64u, .query_strand_one = false},
    };
    const auto absent = idx::query_compact_h32_direct_index(
        view, absent_query, 1u);
    REQUIRE(absent.has_value());
    CHECK_FALSE(absent->results.front().accepted);
    CHECK(absent->evidence.descriptors_examined == 0u);
    CHECK(absent->evidence.unique_selected_hash_count == 0u);
}

TEST_CASE(
    "compact exact-domain H32 applies runtime max-occ below the persisted cap "
    "without retaining or reading rejected postings under exact U+c admission",
    "[doc0383][p4][compact-h32][runtime-max-occ][accepted-only][admission]"
    "[selection-range][projected-hash]") {
    STATIC_REQUIRE(sizeof(idx::CompactH32UniqueSelection) == 8u);
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 2u, .global_count = 1u,
                    .target_strand_zero_count = 1u},
                .encoded_positions = {encoded_position(2u, false)},
            },
            CompactTestRun{
                .header = {.hash = 5u, .global_count = 3u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(5u, false),
                    encoded_position(7u, false),
                    encoded_position(9u, true),
                },
            },
            CompactTestRun{
                .header = {.hash = 7u, .global_count = 4u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(10u, false),
                    encoded_position(12u, false),
                    encoded_position(14u, true),
                    encoded_position(16u, true),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 4u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.wire_version
        == idx::kCompactH32DirectIndexVersionV2);
    CHECK(view.header.descriptor_width_bytes
        == idx::kCompactH32DirectIndexDescriptorBytesV2);
    CHECK(view.header.inline_singleton_count == 1u);
    CHECK(view.header.materialized_hash_count == 2u);
    CHECK(view.header.materialized_posting_count == 7u);

    const std::array queries{
        idx::CompactH32QueryOccurrence{
            .hash = 1u, .query_strand_one = false},
        idx::CompactH32QueryOccurrence{
            .hash = 2u, .query_strand_one = false},
        idx::CompactH32QueryOccurrence{
            .hash = 5u, .query_strand_one = false},
        idx::CompactH32QueryOccurrence{
            .hash = 7u, .query_strand_one = false},
    };
    const auto lower = idx::query_compact_h32_direct_index(
        view, queries, 2u);
    REQUIRE(lower.has_value());
    REQUIRE(lower->results.size() == queries.size());
    CHECK_FALSE(lower->results[0].accepted);
    CHECK(lower->results[1].accepted);
    CHECK_FALSE(lower->results[2].accepted);
    CHECK_FALSE(lower->results[3].accepted);
    CHECK(lower->selections.size() == 1u);
    CHECK(lower->evidence.runtime_rejected_hash_count == 2u);
    CHECK(lower->evidence.runtime_rejected_posting_count == 7u);
    CHECK(lower->evidence.selected_posting_count == 1u);
    CHECK(lower->evidence.rejected_posting_reads == 0u);

    const std::array<std::uint32_t, 4u> unique_hashes{1u, 2u, 5u, 7u};
    const auto sparse = idx::query_compact_h32_direct_index_unique(
        view, unique_hashes, 2u);
    REQUIRE(sparse.has_value());
    REQUIRE(sparse->selections.size() == 1u);
    CHECK(sparse->selections.front().dense_ordinal == 0u);
    CHECK(sparse->selections.front().query_ordinal == 1u);
    CHECK(sparse->retained_selection_capacity_bytes()
        == sparse->selections.capacity()
            * sizeof(idx::CompactH32UniqueSelection));
    CHECK(sparse->selections.capacity() == 1u);
    CHECK(sparse->evidence.selection_reserve_call_count == 1u);
    CHECK(sparse->evidence.selection_reserve_requested_elements == 4u);
    CHECK(sparse->evidence.selection_initial_capacity_elements == 4u);
    CHECK(sparse->evidence.selection_fill_reallocation_count == 0u);
    CHECK(sparse->evidence.selection_compaction_allocation_count == 1u);
    CHECK(sparse->evidence.selection_storage_allocation_count == 2u);
    CHECK(sparse->evidence.selection_peak_capacity_bytes
        == 5u * sizeof(idx::CompactH32UniqueSelection));
    CHECK(sparse->evidence.selection_retained_capacity_elements == 1u);
    CHECK(sparse->evidence.selection_retained_capacity_bytes
        == sizeof(idx::CompactH32UniqueSelection));
    CHECK(sparse->evidence.runtime_rejected_hash_count == 2u);
    CHECK(sparse->evidence.runtime_rejected_posting_count == 7u);
    CHECK(sparse->evidence.selected_posting_count == 1u);
    CHECK(sparse->evidence.selected_inline_singleton_count == 1u);
    CHECK(sparse->evidence.selected_materialized_hash_count == 0u);
    CHECK(sparse->evidence.selected_materialized_posting_count == 0u);
    CHECK(sparse->evidence.rejected_posting_reads == 0u);

    CollectedPostings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited = idx::visit_compact_h32_direct_index_postings(
        view, *sparse, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(visited->postings_visited == 1u);
    CHECK(collected.callback_count == 1u);
    CHECK(collected.by_selection.front()
        == source.runs.front().encoded_positions);

    std::uint64_t sparse_selection_callbacks = 0u;
    const auto sparse_bulk_visited =
        idx::visit_compact_h32_direct_index_selection_spans(
            view,
            *sparse,
            [&](const idx::CompactH32SelectionPostingSpans& selected) {
                ++sparse_selection_callbacks;
                return selected.selection_ordinal == 0u
                    && selected.dense_ordinal == 0u
                    && selected.query_ordinal == 1u
                    && selected.global_count == 1u
                    && selected.strand_zero_count == 1u
                    && selected.span_count == 1u;
            });
    REQUIRE(sparse_bulk_visited.has_value());
    CHECK(sparse_selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->spans_exposed == 1u);
    CHECK(sparse_bulk_visited->postings_exposed == 1u);
    CHECK(sparse_bulk_visited->inline_selection_callbacks == 1u);
    CHECK(sparse_bulk_visited->materialized_selection_callbacks == 0u);
    CHECK(sparse_bulk_visited->inline_postings_exposed == 1u);
    CHECK(sparse_bulk_visited->materialized_postings_exposed == 0u);
    CHECK(sparse_bulk_visited->descriptors_examined == 1u);
    CHECK(sparse_bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(sparse_bulk_visited->membership_words_examined == 0u);

    const auto equal = idx::query_compact_h32_direct_index_unique(
        view, unique_hashes, 4u);
    REQUIRE(equal.has_value());
    CHECK(equal->selections.size() == 3u);
    CHECK(equal->evidence.runtime_rejected_hash_count == 0u);
    CHECK(equal->evidence.runtime_rejected_posting_count == 0u);
    CHECK(equal->evidence.selected_posting_count == 8u);
    CHECK(equal->evidence.selected_inline_singleton_count == 1u);
    CHECK(equal->evidence.selected_materialized_hash_count == 2u);
    CHECK(equal->evidence.selected_materialized_posting_count == 7u);
    CHECK(equal->evidence.rejected_posting_reads == 0u);
    CHECK(equal->selections.size() == 3u);
    CHECK(equal->selections.capacity() == 3u);
    CHECK(equal->evidence.selection_reserve_requested_elements == 4u);
    CHECK(equal->evidence.selection_initial_capacity_elements == 4u);
    CHECK(equal->evidence.selection_fill_reallocation_count == 0u);
    CHECK(equal->evidence.selection_compaction_allocation_count == 1u);
    CHECK(equal->evidence.selection_storage_allocation_count == 2u);
    CHECK(equal->evidence.selection_peak_capacity_bytes
        == 7u * sizeof(idx::CompactH32UniqueSelection));
    CHECK(equal->evidence.selection_retained_capacity_elements == 3u);
    CHECK(equal->evidence.selection_retained_capacity_bytes
        == 3u * sizeof(idx::CompactH32UniqueSelection));
    REQUIRE(equal->selections.size() == 3u);
    CHECK(equal->selections[0].dense_ordinal == 0u);
    CHECK(equal->selections[0].query_ordinal == 1u);
    CHECK(equal->selections[1].dense_ordinal == 1u);
    CHECK(equal->selections[1].query_ordinal == 2u);
    CHECK(equal->selections[2].dense_ordinal == 2u);
    CHECK(equal->selections[2].query_ordinal == 3u);
    // Returned capacity is exactly c: a hypothetical c+1 push would need a
    // growth, while all c query-time pushes provably performed none.
    CHECK(equal->selections.size() + 1u > equal->selections.capacity());

    std::vector<std::tuple<std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t>>
        callback_order;
    std::vector<std::vector<std::uint64_t>> bulk_postings(
        equal->selections.size());
    std::vector<std::vector<bool>> bulk_span_strands(
        equal->selections.size());
    bool all_bulk_positions_decoded = true;
    const auto bulk_visited =
        idx::visit_compact_h32_direct_index_selection_spans(
            view,
            *equal,
            [&](const idx::CompactH32SelectionPostingSpans& selected) {
                callback_order.emplace_back(
                    selected.selection_ordinal,
                    selected.dense_ordinal,
                    selected.query_ordinal,
                    selected.global_count,
                    selected.strand_zero_count,
                    selected.span_count);
                if (selected.selection_ordinal
                        >= bulk_postings.size()) {
                    return false;
                }
                for (std::uint8_t span_ordinal = 0u;
                     span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const auto& span = selected.spans[span_ordinal];
                    bulk_span_strands[selected.selection_ordinal].push_back(
                        span.target_strand_one);
                    if (span.encoded_bytes.size()
                            != span.count * span.posting_width_bytes) {
                        return false;
                    }
                    for (std::uint64_t ordinal = 0u;
                         ordinal < span.count;
                         ++ordinal) {
                        const auto encoded =
                            span.encoded_position_at(ordinal);
                        if (!encoded) {
                            all_bulk_positions_decoded = false;
                            return false;
                        }
                        bulk_postings[selected.selection_ordinal]
                            .push_back(*encoded);
                    }
                    CHECK_FALSE(
                        span.encoded_position_at(span.count).has_value());
                }
                return true;
            });
    CHECK(all_bulk_positions_decoded);
    REQUIRE(bulk_visited.has_value());
    CHECK(bulk_visited->selection_callbacks == 3u);
    CHECK(bulk_visited->spans_exposed == 5u);
    CHECK(bulk_visited->postings_exposed == 8u);
    CHECK(bulk_visited->inline_selection_callbacks == 1u);
    CHECK(bulk_visited->materialized_selection_callbacks == 2u);
    CHECK(bulk_visited->inline_postings_exposed == 1u);
    CHECK(bulk_visited->materialized_postings_exposed == 7u);
    CHECK(bulk_visited->descriptors_examined == 3u);
    CHECK(bulk_visited->rank_checkpoints_examined == 0u);
    CHECK(bulk_visited->membership_words_examined == 0u);
    CHECK(equal->evidence.rank_checkpoints_examined != 0u);
    CHECK(equal->evidence.membership_words_examined != 0u);
    CHECK((callback_order == decltype(callback_order){
        {0u, 0u, 1u, 1u, 1u, 1u},
        {1u, 1u, 2u, 3u, 2u, 2u},
        {2u, 2u, 3u, 4u, 2u, 2u},
    }));
    REQUIRE(bulk_postings.size() == source.runs.size());
    REQUIRE(bulk_span_strands.size() == source.runs.size());
    for (std::size_t ordinal = 0u;
         ordinal < source.runs.size();
         ++ordinal) {
        CHECK(bulk_postings[ordinal]
            == source.runs[ordinal].encoded_positions);
    }
    CHECK(bulk_span_strands[0] == std::vector<bool>{false});
    CHECK(bulk_span_strands[1]
        == (std::vector<bool>{false, true}));
    CHECK(bulk_span_strands[2]
        == (std::vector<bool>{false, true}));

    const auto range_accessor =
        idx::bind_compact_h32_direct_index_selection_spans(
            view, *equal);
    REQUIRE(range_accessor.has_value());
    CHECK(range_accessor->selection_count() == equal->selections.size());
    std::vector<std::tuple<std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t>>
        ranged_callback_order;
    std::vector<std::vector<std::uint64_t>> ranged_postings(
        equal->selections.size());
    const auto visit_range = [&](std::uint64_t selection_begin,
                                 std::uint64_t selection_end) {
        return idx::visit_compact_h32_direct_index_selection_span_range(
            *range_accessor,
            selection_begin,
            selection_end,
            [&](const idx::CompactH32SelectionPostingSpans& selected) {
                ranged_callback_order.emplace_back(
                    selected.selection_ordinal,
                    selected.dense_ordinal,
                    selected.query_ordinal,
                    selected.global_count,
                    selected.strand_zero_count,
                    selected.span_count);
                if (selected.selection_ordinal
                        >= ranged_postings.size()) {
                    return false;
                }
                for (std::uint8_t span_ordinal = 0u;
                     span_ordinal < selected.span_count;
                     ++span_ordinal) {
                    const auto& span = selected.spans[span_ordinal];
                    const auto decoded = span.visit_encoded_positions(
                        [&](std::uint64_t encoded) {
                            ranged_postings[selected.selection_ordinal]
                                .push_back(encoded);
                            return true;
                        });
                    if (!decoded || *decoded != span.count) return false;
                }
                return true;
            });
    };
    const auto first_shard = visit_range(0u, 2u);
    const auto second_shard = visit_range(2u, 3u);
    REQUIRE(first_shard.has_value());
    REQUIRE(second_shard.has_value());
    CHECK(ranged_callback_order == callback_order);
    CHECK(ranged_postings == bulk_postings);
    CHECK(first_shard->selection_callbacks
            + second_shard->selection_callbacks
        == bulk_visited->selection_callbacks);
    CHECK(first_shard->spans_exposed + second_shard->spans_exposed
        == bulk_visited->spans_exposed);
    CHECK(first_shard->postings_exposed
            + second_shard->postings_exposed
        == bulk_visited->postings_exposed);
    CHECK(first_shard->inline_selection_callbacks
            + second_shard->inline_selection_callbacks
        == bulk_visited->inline_selection_callbacks);
    CHECK(first_shard->materialized_selection_callbacks
            + second_shard->materialized_selection_callbacks
        == bulk_visited->materialized_selection_callbacks);
    CHECK(first_shard->inline_postings_exposed
            + second_shard->inline_postings_exposed
        == bulk_visited->inline_postings_exposed);
    CHECK(first_shard->materialized_postings_exposed
            + second_shard->materialized_postings_exposed
        == bulk_visited->materialized_postings_exposed);
    CHECK(first_shard->descriptors_examined
            + second_shard->descriptors_examined
        == bulk_visited->descriptors_examined);

    // The authenticated accessor is intentionally reusable: a second pass
    // over the same shard boundaries does not rescan the whole plan seal.
    std::uint64_t second_pass_callbacks = 0u;
    for (const auto [selection_begin, selection_end] :
         std::array{
             std::pair{std::uint64_t{0u}, std::uint64_t{2u}},
             std::pair{std::uint64_t{2u}, std::uint64_t{3u}},
         }) {
        const auto revisited =
            idx::visit_compact_h32_direct_index_selection_span_range(
                *range_accessor,
                selection_begin,
                selection_end,
                [&](const idx::CompactH32SelectionPostingSpans&) {
                    ++second_pass_callbacks;
                    return true;
                });
        REQUIRE(revisited.has_value());
    }
    CHECK(second_pass_callbacks == equal->selections.size());

    for (const std::uint64_t empty_boundary :
         std::array<std::uint64_t, 3u>{0u, 1u, 3u}) {
        std::uint64_t empty_callbacks = 0u;
        const auto empty =
            idx::visit_compact_h32_direct_index_selection_span_range(
                *range_accessor,
                empty_boundary,
                empty_boundary,
                [&](const idx::CompactH32SelectionPostingSpans&) {
                    ++empty_callbacks;
                    return true;
                });
        REQUIRE(empty.has_value());
        CHECK(empty_callbacks == 0u);
        CHECK(empty->selection_callbacks == 0u);
        CHECK(empty->spans_exposed == 0u);
        CHECK(empty->postings_exposed == 0u);
        CHECK(empty->descriptors_examined == 0u);
    }
    for (const auto [selection_begin, selection_end] :
         std::array{
             std::pair{std::uint64_t{2u}, std::uint64_t{1u}},
             std::pair{std::uint64_t{0u}, std::uint64_t{4u}},
             std::pair{std::uint64_t{4u}, std::uint64_t{4u}},
         }) {
        std::uint64_t invalid_callbacks = 0u;
        const auto invalid =
            idx::visit_compact_h32_direct_index_selection_span_range(
                *range_accessor,
                selection_begin,
                selection_end,
                [&](const idx::CompactH32SelectionPostingSpans&) {
                    ++invalid_callbacks;
                    return true;
                });
        REQUIRE_FALSE(invalid.has_value());
        CHECK(invalid.error()
            == idx::DirectInvertedIndexError::query_plan);
        CHECK(invalid_callbacks == 0u);
    }

    auto descriptor_mismatched_view = view;
    --descriptor_mismatched_view.header.descriptors_bytes;
    const auto descriptor_mismatched =
        idx::bind_compact_h32_direct_index_selection_spans(
            descriptor_mismatched_view, *equal);
    REQUIRE_FALSE(descriptor_mismatched.has_value());
    CHECK(descriptor_mismatched.error()
        == idx::DirectInvertedIndexError::query_plan);

    const auto require_unique_plan_rejected_without_callback =
        [&view](const idx::CompactH32UniqueQueryPlan& forged) {
            const auto binding =
                idx::bind_compact_h32_direct_index_selection_spans(
                    view, forged);
            REQUIRE_FALSE(binding.has_value());
            CHECK(binding.error()
                == idx::DirectInvertedIndexError::query_plan);
            std::uint64_t callback_count = 0u;
            const auto rejected =
                idx::visit_compact_h32_direct_index_selection_spans(
                    view,
                    forged,
                    [&](const idx::CompactH32SelectionPostingSpans&) {
                        ++callback_count;
                        return true;
                    });
            REQUIRE_FALSE(rejected.has_value());
            CHECK(rejected.error()
                == idx::DirectInvertedIndexError::query_plan);
            CHECK(callback_count == 0u);
        };

    auto forged_valid_dense = *equal;
    forged_valid_dense.selections.front().dense_ordinal = 1u;
    require_unique_plan_rejected_without_callback(forged_valid_dense);

    auto forged_out_of_range_dense = *equal;
    forged_out_of_range_dense.selections.front().dense_ordinal =
        static_cast<std::uint32_t>(view.header.accepted_hash_count);
    require_unique_plan_rejected_without_callback(
        forged_out_of_range_dense);

    idx::CompactH32UniqueQueryPlan unsealed{};
    unsealed.image_header_crc64 = equal->image_header_crc64;
    unsealed.max_occ_cap = equal->max_occ_cap;
    unsealed.runtime_max_occ = equal->runtime_max_occ;
    unsealed.posting_width_bytes = equal->posting_width_bytes;
    unsealed.selections = equal->selections;
    unsealed.evidence = equal->evidence;
    require_unique_plan_rejected_without_callback(unsealed);

    const std::array<std::uint32_t, 3u> all_accepted_hashes{2u, 5u, 7u};
    const auto full = idx::query_compact_h32_direct_index_unique(
        view, all_accepted_hashes, 4u);
    REQUIRE(full.has_value());
    CHECK(full->selections.size() == all_accepted_hashes.size());
    CHECK(full->selections[0].dense_ordinal == 0u);
    CHECK(full->selections[0].query_ordinal == 0u);
    CHECK(full->selections[1].dense_ordinal == 1u);
    CHECK(full->selections[1].query_ordinal == 1u);
    CHECK(full->selections[2].dense_ordinal == 2u);
    CHECK(full->selections[2].query_ordinal == 2u);
    CHECK(full->selections.capacity() == all_accepted_hashes.size());
    CHECK(full->evidence.selection_reserve_requested_elements == 3u);
    CHECK(full->evidence.selection_initial_capacity_elements == 3u);
    CHECK(full->evidence.selection_fill_reallocation_count == 0u);
    CHECK(full->evidence.selection_compaction_allocation_count == 0u);
    CHECK(full->evidence.selection_storage_allocation_count == 1u);
    CHECK(full->evidence.selection_peak_capacity_bytes
        == 3u * sizeof(idx::CompactH32UniqueSelection));
    CHECK(full->evidence.selection_retained_capacity_elements == 3u);
    CHECK(full->evidence.selection_retained_capacity_bytes
        == 3u * sizeof(idx::CompactH32UniqueSelection));

    const std::array<std::uint32_t, 4u> all_absent_hashes{1u, 3u, 4u, 6u};
    const auto none = idx::query_compact_h32_direct_index_unique(
        view, all_absent_hashes, 4u);
    REQUIRE(none.has_value());
    CHECK(none->selections.empty());
    CHECK(none->selections.capacity() == 0u);
    CHECK(none->evidence.selection_reserve_requested_elements == 4u);
    CHECK(none->evidence.selection_initial_capacity_elements == 4u);
    CHECK(none->evidence.selection_fill_reallocation_count == 0u);
    CHECK(none->evidence.selection_compaction_allocation_count == 0u);
    CHECK(none->evidence.selection_storage_allocation_count == 1u);
    CHECK(none->evidence.selection_peak_capacity_bytes
        == 4u * sizeof(idx::CompactH32UniqueSelection));
    CHECK(none->evidence.selection_retained_capacity_elements == 0u);
    CHECK(none->evidence.selection_retained_capacity_bytes == 0u);
    std::uint64_t absent_selection_callbacks = 0u;
    const auto none_visited =
        idx::visit_compact_h32_direct_index_selection_spans(
            view,
            *none,
            [&](const idx::CompactH32SelectionPostingSpans&) {
                ++absent_selection_callbacks;
                return true;
            });
    REQUIRE(none_visited.has_value());
    CHECK(absent_selection_callbacks == 0u);
    CHECK(none_visited->selection_callbacks == 0u);
    CHECK(none_visited->spans_exposed == 0u);
    CHECK(none_visited->postings_exposed == 0u);
    CHECK(none_visited->inline_selection_callbacks == 0u);
    CHECK(none_visited->materialized_selection_callbacks == 0u);
    CHECK(none_visited->inline_postings_exposed == 0u);
    CHECK(none_visited->materialized_postings_exposed == 0u);
    CHECK(none_visited->descriptors_examined == 0u);
    CHECK(none_visited->rank_checkpoints_examined == 0u);
    CHECK(none_visited->membership_words_examined == 0u);
    const auto none_range_accessor =
        idx::bind_compact_h32_direct_index_selection_spans(
            view, *none);
    REQUIRE(none_range_accessor.has_value());
    CHECK(none_range_accessor->selection_count() == 0u);
    const auto none_range =
        idx::visit_compact_h32_direct_index_selection_span_range(
            *none_range_accessor,
            0u,
            0u,
            [](const idx::CompactH32SelectionPostingSpans&) {
                return false;
            });
    REQUIRE(none_range.has_value());
    CHECK(none_range->selection_callbacks == 0u);
    CHECK(none_range->postings_exposed == 0u);

    const auto greater = idx::query_compact_h32_direct_index_unique(
        view, unique_hashes, 5u);
    REQUIRE_FALSE(greater.has_value());
    CHECK(greater.error() == idx::DirectInvertedIndexError::compatibility);
    const auto zero = idx::query_compact_h32_direct_index_unique(
        view, unique_hashes, 0u);
    REQUIRE_FALSE(zero.has_value());
    CHECK(zero.error() == idx::DirectInvertedIndexError::compatibility);

    const std::array<std::uint32_t, 2u> duplicate_hashes{2u, 2u};
    const auto duplicate = idx::query_compact_h32_direct_index_unique(
        view, duplicate_hashes, 2u);
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error() == idx::DirectInvertedIndexError::order);
    const auto duplicate_projected =
        idx::query_compact_h32_direct_index_unique_projected(
            view,
            duplicate_hashes.size(),
            [&duplicate_hashes](std::size_t ordinal) noexcept
                    -> std::uint32_t {
                return duplicate_hashes[ordinal];
            },
            2u);
    REQUIRE_FALSE(duplicate_projected.has_value());
    CHECK(duplicate_projected.error()
        == idx::DirectInvertedIndexError::order);
}

TEST_CASE(
    "compact exact-domain H32 visitor rejects unbound misaligned and "
    "out-of-section spans",
    "[doc0383][p4][compact-h32][visitor][query-plan]") {
    CompactTestSource source{
        .runs = {
            CompactTestRun{
                .header = {.hash = 10u, .global_count = 2u,
                    .target_strand_zero_count = 2u},
                .encoded_positions = {
                    encoded_position(1u, false),
                    encoded_position(3u, false),
                },
            },
        },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    const std::array query{
        idx::CompactH32QueryOccurrence{
            .hash = 10u, .query_strand_one = false},
    };
    const auto planned = idx::query_compact_h32_direct_index(
        view, query, 2u);
    REQUIRE(planned.has_value());
    REQUIRE(planned->selections.size() == 1u);
    REQUIRE(planned->spans.size() == 1u);

    const auto require_plan_rejected = [&view](
            const idx::CompactH32QueryPlan& forged) {
        CollectedPostings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        const auto visited = idx::visit_compact_h32_direct_index_postings(
            view, forged, &collected, &collect_posting);
        REQUIRE_FALSE(visited.has_value());
        CHECK(visited.error() == idx::DirectInvertedIndexError::query_plan);
        CHECK(collected.callback_count == 0u);
    };

    auto misaligned = *planned;
    ++misaligned.spans.front().byte_offset;
    require_plan_rejected(misaligned);

    auto out_of_section = *planned;
    out_of_section.spans.front().byte_offset =
        view.header.postings_offset + view.header.postings_bytes;
    require_plan_rejected(out_of_section);

    auto unbound = *planned;
    ++unbound.selections.front().hash;
    require_plan_rejected(unbound);
}
