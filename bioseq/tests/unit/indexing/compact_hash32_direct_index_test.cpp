/**
 * @file compact_hash32_direct_index_test.cpp
 * @brief Checks direct-index formats, corruption detection and opening headers without reading payloads.
 */

#include <cstring>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.tests.unit.indexing.compact_hash32_direct_index_support;

namespace idx = bioseq::indexing;
using namespace bioseq::tests::unit::indexing::compact_hash32_direct_index_test_support;

TEST_CASE("direct hash32 codec selects P32 and P64 without narrowing positions",
          "[component:bioseq][layer:unit][feature:index]") {
    SECTION("P32 includes the maximum encoded uint32 position") {
        constexpr std::uint64_t target_length = std::uint64_t{1u} << 31u;
        const std::vector<idx::direct_index_occurrence> input{
            {.hash = 1u, .target_bp = target_length - 1u, .target_strand_one = true},
        };
        const auto build_options = options(target_length, 0u, 8u);
        const auto image = build_or_fail(input, build_options);
        const auto view = validate_or_fail(image, build_options.identity);
        CHECK(view.header.posting_width_bytes == 4u);
        const std::array queries{
            idx::direct_index_query_occurrence{.hash = 1u, .query_strand_one = false},
        };
        const auto plan = idx::query_direct_inverted_index(view, queries);
        REQUIRE(plan.has_value());
        collected_postings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        REQUIRE(
            idx::visit_direct_inverted_index_postings(view, *plan, &collected, &collect_posting));
        REQUIRE(collected.by_selection.front().size() == 1u);
        CHECK(collected.by_selection.front().front() ==
              (std::numeric_limits<std::uint32_t>::max)());
    }

    SECTION("P64 preserves a position beyond the packed-32 boundary") {
        constexpr std::uint64_t target_length = (std::uint64_t{1u} << 31u) + 10u;
        const std::vector<idx::direct_index_occurrence> input{
            {.hash = 1u, .target_bp = (std::uint64_t{1u} << 31u) + 3u, .target_strand_one = false},
        };
        const auto build_options = options(target_length, 0u, 8u);
        const auto image = build_or_fail(input, build_options);
        const auto view = validate_or_fail(image, build_options.identity);
        CHECK(view.header.posting_width_bytes == 8u);
        const std::array queries{
            idx::direct_index_query_occurrence{.hash = 1u, .query_strand_one = false},
        };
        const auto plan = idx::query_direct_inverted_index(view, queries);
        REQUIRE(plan.has_value());
        collected_postings collected{
            .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
        };
        REQUIRE(
            idx::visit_direct_inverted_index_postings(view, *plan, &collected, &collect_posting));
        REQUIRE(collected.by_selection.front().size() == 1u);
        CHECK(collected.by_selection.front().front() == (((std::uint64_t{1u} << 31u) + 3u) << 1u));
    }
}

TEST_CASE("direct hash32 validation fails closed on identity checksum dictionary and "
          "semantic corruption",
          "[component:bioseq][layer:unit][feature:index]") {
    const std::vector<idx::direct_index_occurrence> input{
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
        CHECK(result.error() == idx::direct_inverted_index_error::identity_mismatch);
    }

    SECTION("posting corruption is authenticated") {
        auto corrupt = pristine;
        const std::uint64_t postings_offset = reference_read_le<std::uint64_t>(corrupt, 144u);
        corrupt[static_cast<std::size_t>(postings_offset)] ^= std::byte{0x40};
        const auto result = idx::validate_direct_inverted_index(corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == idx::direct_inverted_index_error::section_checksum);
    }

    SECTION("reserved header bytes remain zero even with a valid header CRC") {
        auto corrupt = pristine;
        corrupt[248u] = std::byte{1};
        reference_reseal_header(corrupt);
        const auto result = idx::validate_direct_inverted_index(corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == idx::direct_inverted_index_error::reserved_nonzero);
    }

    SECTION("duplicate dictionary keys fail after checksums are resealed") {
        auto corrupt = pristine;
        const std::uint64_t controls_offset = reference_read_le<std::uint64_t>(corrupt, 112u);
        const std::uint32_t first_hash =
            reference_read_le<std::uint32_t>(corrupt, static_cast<std::size_t>(controls_offset));
        reference_write_le<std::uint32_t>(
            corrupt,
            static_cast<std::size_t>(controls_offset + idx::k_direct_inverted_index_control_bytes),
            first_hash);
        reference_reseal_controls(corrupt);
        const auto result = idx::validate_direct_inverted_index(corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == idx::direct_inverted_index_error::dictionary);
    }

    SECTION("authoritative count cannot exceed its posting extent") {
        auto corrupt = pristine;
        const std::uint64_t controls_offset = reference_read_le<std::uint64_t>(corrupt, 112u);
        const std::uint64_t first_count = reference_read_le<std::uint64_t>(
            corrupt, static_cast<std::size_t>(controls_offset + 8u));
        reference_write_le<std::uint64_t>(
            corrupt, static_cast<std::size_t>(controls_offset + 8u), first_count + 1u);
        reference_reseal_controls(corrupt);
        const auto result = idx::validate_direct_inverted_index(corrupt, build_options.identity);
        REQUIRE_FALSE(result.has_value());
        CHECK((result.error() == idx::direct_inverted_index_error::control ||
               result.error() == idx::direct_inverted_index_error::posting));
    }
}

TEST_CASE("compact hash32 bounded open retains a nested nonzero-offset section owner "
          "and exposes a queryable alias-bound view",
          "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 7u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(17u, false)},
                },
            },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t nested_header_receipt_crc64 = reference_crc64(
        std::span<const std::byte>{image}.first(idx::k_compact_hash32_direct_index_header_bytes));
    const auto open_options = compact_bounded_options(build_options, nested_header_receipt_crc64);

    constexpr std::size_t prefix_bytes = 777u;
    constexpr std::size_t suffix_bytes = 31u;
    std::weak_ptr<std::vector<std::byte>> owner_lifetime;
    {
        auto outer_image = std::make_shared<std::vector<std::byte>>(
            prefix_bytes + image.size() + suffix_bytes, std::byte{0xa5});
        std::copy(image.begin(),
                  image.end(),
                  outer_image->begin() + static_cast<std::ptrdiff_t>(prefix_bytes));
        owner_lifetime = outer_image;
        const std::span<const std::byte> direct_section{outer_image->data() + prefix_bytes,
                                                        image.size()};
        const std::byte* const outer_image_start = outer_image->data();
        std::shared_ptr<const void> mapped_owner{outer_image, outer_image_start};
        REQUIRE(mapped_owner.get() != direct_section.data());

        auto opened = idx::open_compact_hash32_direct_index_bounded(
            std::move(mapped_owner), direct_section, open_options);
        REQUIRE(opened.has_value());
        CHECK(opened->image_owner().get() == direct_section.data());
        CHECK(opened->image_owner().get() != outer_image_start);
        CHECK(opened->view().image.data() == direct_section.data());
        CHECK(opened->view().image.size() == image.size());
        CHECK(opened->view().header.header_crc64 ==
              reference_read_le<std::uint64_t>(image, idx::k_compact_hash32_header_crc_offset));
        CHECK(opened->stats().direct_section_bytes == image.size());
        CHECK(opened->stats().header_bytes_examined ==
              idx::k_compact_hash32_direct_index_header_bytes);
        CHECK(opened->stats().payload_bytes_examined == 0u);
        CHECK(opened->stats().expected_direct_header_crc64_checked);

        outer_image.reset();
        CHECK_FALSE(owner_lifetime.expired());
        const std::array query{
            idx::compact_hash32_query_occurrence{.hash = 7u, .query_strand_one = false},
        };
        const auto planned = idx::query_compact_hash32_direct_index(opened->view(), query, 2u);
        REQUIRE(planned.has_value());
        REQUIRE(planned->results.size() == 1u);
        CHECK(planned->results.front().accepted);
        CHECK(planned->results.front().global_count == 1u);
    }
    CHECK(owner_lifetime.expired());
}

TEST_CASE("compact hash32 bounded open rejects truncated unauthenticated and forged "
          "fixed-header geometry without scanning payload sections",
          "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 7u, .global_count = 2u, .target_strand_zero_count = 1u},
                    .encoded_positions =
                        {
                            encoded_position(3u, false),
                            encoded_position(9u, true),
                        },
                },
            },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto open_options = compact_bounded_options(build_options);
    const auto open_with_size = [](std::vector<std::byte> candidate,
                                   std::size_t exposed_bytes,
                                   idx::compact_hash32_bounded_open_options candidate_options) {
        auto owner = std::make_shared<std::vector<std::byte>>(std::move(candidate));
        REQUIRE(exposed_bytes <= owner->size());
        const std::span<const std::byte> direct_section{owner->data(), exposed_bytes};
        std::shared_ptr<const void> mapped_owner = owner;
        return idx::open_compact_hash32_direct_index_bounded(
            std::move(mapped_owner), direct_section, candidate_options);
    };

    SECTION("a fixed header shorter than 256 bytes is rejected") {
        const auto opened = open_with_size(
            image, idx::k_compact_hash32_direct_index_header_bytes - 1u, open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("a section shorter than its authenticated total is rejected") {
        const auto opened = open_with_size(image, image.size() - 1u, open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("a corrupted embedded header checksum is rejected") {
        auto corrupt = image;
        corrupt[idx::k_compact_hash32_header_crc_offset] ^= std::byte{0x01};
        const auto opened = open_compact_bounded_owned(std::move(corrupt), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::header_checksum);
    }

    SECTION("an unsupported header revision is rejected exactly") {
        auto forged = image;
        reference_write_le<std::uint32_t>(forged, 8u, 3u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::bad_layout_revision);
    }

    SECTION("an unknown descriptor width is rejected") {
        auto forged = image;
        reference_write_le<std::uint16_t>(forged, 22u, 12u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::bad_header);
    }

    SECTION("an impossible inline-singleton count is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, idx::k_compact_hash32_inline_singleton_count_offset, 2u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("a mismatched outer nested-header receipt is rejected") {
        const std::uint64_t actual_crc = reference_crc64(std::span<const std::byte>{image}.first(
            idx::k_compact_hash32_direct_index_header_bytes));
        const auto opened = open_compact_bounded_owned(
            image, compact_bounded_options(build_options, actual_crc ^ 1u));
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::receipt_mismatch);
    }

    SECTION("a misaligned membership offset is rejected after resealing") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 112u, reference_read_le<std::uint64_t>(forged, 112u) + 1u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::section_layout);
    }

    SECTION("overlapping rank and descriptor sections are rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 144u, reference_read_le<std::uint64_t>(forged, 128u));
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::section_layout);
    }

    SECTION("an overflowing posting interval is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 160u, (std::numeric_limits<std::uint64_t>::max)() - 63u);
        reference_write_le<std::uint64_t>(forged, 168u, 128u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::section_layout);
    }

    SECTION("a forged total direct-section size is rejected") {
        auto forged = image;
        reference_write_le<std::uint64_t>(
            forged, 216u, reference_read_le<std::uint64_t>(forged, 216u) + 1u);
        reference_reseal_compact_hash32_header(forged);
        const auto opened = open_compact_bounded_owned(std::move(forged), open_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::image_size);
    }

    SECTION("the persisted max-occ cap is matched exactly") {
        auto wrong_options = open_options;
        ++wrong_options.expected_max_occ_cap;
        const auto opened = open_compact_bounded_owned(image, wrong_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::identity_mismatch);
    }

    SECTION("the hash-bits and max-hash profile is matched exactly") {
        auto wrong_options = open_options;
        wrong_options.expected_geometry = compact_geometry(10u);
        const auto opened = open_compact_bounded_owned(image, wrong_options);
        REQUIRE_FALSE(opened.has_value());
        CHECK(opened.error() == idx::direct_inverted_index_error::identity_mismatch);
    }
}

TEST_CASE("compact hash32 bounded open succeeds when every payload page is no-access",
          "[component:bioseq][layer:unit][feature:storage]") {
#if defined(__unix__) || defined(__APPLE__)
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 7u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(17u, false)},
                },
            },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t nested_header_receipt_crc64 = reference_crc64(
        std::span<const std::byte>{image}.first(idx::k_compact_hash32_direct_index_header_bytes));
    const auto open_options = compact_bounded_options(build_options, nested_header_receipt_crc64);

    const long page_size_result = ::sysconf(_SC_PAGESIZE);
    REQUIRE(page_size_result > static_cast<long>(idx::k_compact_hash32_direct_index_header_bytes));
    const std::size_t page_size = static_cast<std::size_t>(page_size_result);
    const std::size_t payload_bytes =
        image.size() - idx::k_compact_hash32_direct_index_header_bytes;
    const std::size_t payload_pages =
        (std::max)(std::size_t{1u}, (payload_bytes + page_size - 1u) / page_size);
    const std::size_t mapping_bytes = (1u + payload_pages) * page_size;
#if defined(MAP_ANONYMOUS)
    constexpr int anonymous_map_flag = MAP_ANONYMOUS;
#else
    constexpr int anonymous_map_flag = MAP_ANON;
#endif
    void* const mapping = ::mmap(
        nullptr, mapping_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | anonymous_map_flag, -1, 0);
    REQUIRE(mapping != MAP_FAILED);
    std::weak_ptr<const void> mapping_lifetime;
    {
        std::shared_ptr<const void> mapped_owner{
            mapping,
            [mapping_bytes](const void* address) noexcept {
                static_cast<void>(::munmap(const_cast<void*>(address), mapping_bytes));
            },
        };
        mapping_lifetime = mapped_owner;
        auto* const mapping_bytes_begin = static_cast<std::byte*>(mapping);
        std::byte* const direct_section_begin =
            mapping_bytes_begin + page_size - idx::k_compact_hash32_direct_index_header_bytes;
        std::memcpy(
            direct_section_begin, image.data(), idx::k_compact_hash32_direct_index_header_bytes);
        REQUIRE(::mprotect(mapping_bytes_begin, page_size, PROT_READ) == 0);
        REQUIRE(::mprotect(mapping_bytes_begin + page_size, mapping_bytes - page_size, PROT_NONE) ==
                0);
        const std::span<const std::byte> guarded_direct_section{direct_section_begin, image.size()};

        auto opened = idx::open_compact_hash32_direct_index_bounded(
            std::move(mapped_owner), guarded_direct_section, open_options);
        REQUIRE(opened.has_value());
        CHECK(opened->image_owner().get() == direct_section_begin);
        CHECK(opened->view().image.size() == image.size());
        CHECK(opened->view().header.header_crc64 ==
              reference_read_le<std::uint64_t>(image, idx::k_compact_hash32_header_crc_offset));
        CHECK(opened->stats().header_bytes_examined ==
              idx::k_compact_hash32_direct_index_header_bytes);
        CHECK(opened->stats().payload_bytes_examined == 0u);
        CHECK_FALSE(mapping_lifetime.expired());
    }
    CHECK(mapping_lifetime.expired());
#else
    SUCCEED("guard-page validation requires a POSIX mmap implementation");
#endif
}

TEST_CASE("compact exact-domain hash32 codec preserves P64 accepted postings",
          "[component:bioseq][layer:unit][feature:storage]") {
    constexpr std::uint64_t target_length = (std::uint64_t{1u} << 31u) + 100u;
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 2u, .global_count = 1u, .target_strand_zero_count = 0u},
                    .encoded_positions =
                        {
                            encoded_position((std::uint64_t{1u} << 31u) + 7u, true),
                        },
                },
            },
    };
    const auto build_options = compact_options(target_length, 1u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const auto view = validate_compact_or_fail(image, build_options);
    CHECK(view.header.layout_revision == idx::k_compact_hash32_direct_index_layout_revision);
    CHECK(view.header.descriptor_width_bytes ==
          idx::k_compact_hash32_direct_index_descriptor_bytes);
    CHECK(view.header.posting_width_bytes == 8u);
    CHECK(view.header.inline_singleton_count == 0u);
    CHECK(view.header.materialized_hash_count == 1u);
    CHECK(view.header.materialized_posting_count == 1u);
    const std::array query{
        idx::compact_hash32_query_occurrence{.hash = 2u, .query_strand_one = false},
    };
    const auto plan = idx::query_compact_hash32_direct_index(view, query, 1u);
    REQUIRE(plan.has_value());
    collected_postings collected{
        .by_selection = std::vector<std::vector<std::uint64_t>>(1u),
    };
    const auto visited =
        idx::visit_compact_hash32_direct_index_postings(view, *plan, &collected, &collect_posting);
    REQUIRE(visited.has_value());
    CHECK(collected.by_selection.front() == source.runs.front().encoded_positions);

    const std::array<std::uint32_t, 1u> unique_hashes{2u};
    const auto unique_plan = idx::query_compact_hash32_direct_index_unique(view, unique_hashes, 1u);
    REQUIRE(unique_plan.has_value());
    REQUIRE(unique_plan->selections.size() == 1u);
    CHECK(unique_plan->selections.front().dense_ordinal == 0u);
    CHECK(unique_plan->selections.front().query_ordinal == 0u);
    CHECK(unique_plan->stats.selected_inline_singleton_count == 0u);
    CHECK(unique_plan->stats.selected_materialized_hash_count == 1u);
    CHECK(unique_plan->stats.selected_materialized_posting_count == 1u);
    struct projected_position64_hash_record {
        std::uint32_t hash = 0u;
        std::uint32_t occurrence_ref_begin = 0u;
        std::uint32_t occurrence_ref_count = 0u;
        std::uint32_t global_occurrence_count = 0u;
    };
    static_assert(sizeof(projected_position64_hash_record) == 16u);
    const std::array projected_hashes{
        projected_position64_hash_record{
            .hash = 2u,
            .occurrence_ref_begin = 3u,
            .occurrence_ref_count = 5u,
            .global_occurrence_count = 7u,
        },
    };
    std::size_t projected_hash_reads = 0u;
    const auto projected_plan = idx::query_compact_hash32_direct_index_unique_projected(
        view,
        projected_hashes.size(),
        [&projected_hashes, &projected_hash_reads](std::size_t ordinal) noexcept -> std::uint32_t {
            ++projected_hash_reads;
            return projected_hashes[ordinal].hash;
        },
        1u);
    REQUIRE(projected_plan.has_value());
    CHECK(projected_hash_reads == projected_hashes.size());
    CHECK(projected_plan->stats == unique_plan->stats);
    REQUIRE(projected_plan->selections.size() == unique_plan->selections.size());
    CHECK(projected_plan->selections.front().dense_ordinal ==
          unique_plan->selections.front().dense_ordinal);
    CHECK(projected_plan->selections.front().query_ordinal ==
          unique_plan->selections.front().query_ordinal);
    const auto projected_p64_accessor =
        idx::bind_compact_hash32_direct_index_selection_spans(view, *projected_plan);
    REQUIRE(projected_p64_accessor.has_value());
    CHECK(projected_p64_accessor->selection_count() == 1u);
    std::uint64_t selection_callback_count = 0u;
    std::vector<std::uint64_t> bulk_positions;
    bool span_shape_valid = true;
    const auto bulk_visited = idx::visit_compact_hash32_direct_index_selection_spans(
        view,
        *unique_plan,
        [&selection_callback_count, &span_shape_valid, &bulk_positions](
            const idx::compact_hash32_selection_posting_spans& selected) {
            ++selection_callback_count;
            if (selected.selection_ordinal != 0u || selected.dense_ordinal != 0u ||
                selected.query_ordinal != 0u || selected.global_count != 1u ||
                selected.strand_zero_count != 0u || selected.span_count != 1u) {
                span_shape_valid = false;
                return false;
            }
            const auto& span = selected.spans.front();
            if (span.count != 1u || span.posting_width_bytes != 8u ||
                span.encoded_bytes.size() != 8u || !span.target_strand_one ||
                span.target_length != target_length) {
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
        idx::bind_compact_hash32_direct_index_selection_spans(view, *unique_plan);
    REQUIRE(p64_range_accessor.has_value());
    std::vector<std::uint64_t> p64_range_positions;
    std::optional<idx::compact_hash32_posting_span_view> p64_span;
    const auto p64_range_visited = idx::visit_compact_hash32_direct_index_selection_span_range(
        *p64_range_accessor,
        0u,
        1u,
        [&p64_span,
         &p64_range_positions](const idx::compact_hash32_selection_posting_spans& selected) {
            if (selected.selection_ordinal != 0u || selected.span_count != 1u ||
                selected.spans.front().posting_width_bytes != 8u) {
                return false;
            }
            p64_span = selected.spans.front();
            const auto decoded = selected.spans.front().visit_encoded_positions(
                [&p64_range_positions](std::uint64_t encoded) {
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
    truncated_p64_span.encoded_bytes = truncated_p64_span.encoded_bytes.first(7u);
    std::uint64_t truncated_callbacks = 0u;
    const auto truncated_p64 =
        truncated_p64_span.visit_encoded_positions([&truncated_callbacks](std::uint64_t) {
            ++truncated_callbacks;
            return true;
        });
    REQUIRE_FALSE(truncated_p64.has_value());
    CHECK(truncated_p64.error() == idx::direct_inverted_index_error::query_plan);
    CHECK(truncated_callbacks == 0u);

    auto wrong_strand_p64_span = *p64_span;
    wrong_strand_p64_span.target_strand_one = false;
    std::uint64_t wrong_strand_callbacks = 0u;
    const auto wrong_strand_p64 =
        wrong_strand_p64_span.visit_encoded_positions([&wrong_strand_callbacks](std::uint64_t) {
            ++wrong_strand_callbacks;
            return true;
        });
    REQUIRE_FALSE(wrong_strand_p64.has_value());
    CHECK(wrong_strand_p64.error() == idx::direct_inverted_index_error::query_plan);
    CHECK(wrong_strand_callbacks == 0u);

    auto out_of_range_p64_span = *p64_span;
    out_of_range_p64_span.target_length = 1u;
    std::uint64_t out_of_range_callbacks = 0u;
    const auto out_of_range_p64 =
        out_of_range_p64_span.visit_encoded_positions([&out_of_range_callbacks](std::uint64_t) {
            ++out_of_range_callbacks;
            return true;
        });
    REQUIRE_FALSE(out_of_range_p64.has_value());
    CHECK(out_of_range_p64.error() == idx::direct_inverted_index_error::query_plan);
    CHECK(out_of_range_callbacks == 0u);

    std::uint64_t rejected_callbacks = 0u;
    const auto rejected_p64 =
        p64_span->visit_encoded_positions([&rejected_callbacks](std::uint64_t) {
            ++rejected_callbacks;
            return false;
        });
    REQUIRE_FALSE(rejected_p64.has_value());
    CHECK(rejected_p64.error() == idx::direct_inverted_index_error::visitor_rejected);
    CHECK(rejected_callbacks == 1u);
}

TEST_CASE("compact hash32 reader rejects forged descriptor interpretation after "
          "payload checksums are resealed",
          "[component:bioseq][layer:unit][feature:storage]") {
    compact_test_source source{
        .runs =
            {
                compact_test_run{
                    .header = {.hash = 2u, .global_count = 1u, .target_strand_zero_count = 1u},
                    .encoded_positions = {encoded_position(4u, false)},
                },
                compact_test_run{
                    .header = {.hash = 5u, .global_count = 2u, .target_strand_zero_count = 1u},
                    .encoded_positions =
                        {
                            encoded_position(6u, false),
                            encoded_position(8u, true),
                        },
                },
            },
    };
    const auto build_options = compact_options(100u, 2u, 9u);
    const auto image = build_compact_or_fail(source, build_options);
    const std::uint64_t descriptors_offset = reference_read_le<std::uint64_t>(image, 144u);

    SECTION("an inline singleton cannot forge its persisted strand") {
        auto forged = image;
        reference_write_le<std::uint32_t>(
            forged, descriptors_offset, static_cast<std::uint32_t>(encoded_position(4u, true)));
        reference_reseal_compact_hash32_payload_and_header(forged);
        const auto validated = idx::validate_compact_hash32_direct_index(
            forged, build_options.identity, build_options.max_occ_cap, build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error() == idx::direct_inverted_index_error::posting);
    }

    SECTION("a materialized descriptor cannot be retyped as an extra inline") {
        auto forged = image;
        const std::uint64_t second_descriptor =
            descriptors_offset + idx::k_compact_hash32_direct_index_descriptor_bytes;
        reference_write_le<std::uint16_t>(forged, second_descriptor + 4u, 1u);
        reference_reseal_compact_hash32_payload_and_header(forged);
        const auto validated = idx::validate_compact_hash32_direct_index(
            forged, build_options.identity, build_options.max_occ_cap, build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error() == idx::direct_inverted_index_error::dictionary);
    }

    SECTION("a materialized posting ordinal remains exact") {
        auto forged = image;
        const std::uint64_t second_descriptor =
            descriptors_offset + idx::k_compact_hash32_direct_index_descriptor_bytes;
        reference_write_le<std::uint32_t>(forged, second_descriptor, 99u);
        reference_reseal_compact_hash32_payload_and_header(forged);
        const auto validated = idx::validate_compact_hash32_direct_index(
            forged, build_options.identity, build_options.max_occ_cap, build_options.geometry);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.error() == idx::direct_inverted_index_error::control);
    }
}
