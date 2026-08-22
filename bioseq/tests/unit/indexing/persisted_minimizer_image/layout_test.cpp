/**
 * @file layout_test.cpp
 * @brief Tests persisted minimizer image layout planning and validation.
 */

#include <sys/mman.h>
#include <unistd.h>

#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_fragment_image;
import bioseq.tests.unit.indexing.persisted_minimizer_image_support;

namespace idx = bioseq::indexing;
using namespace bioseq::tests::unit::indexing::persisted_minimizer_image_test_support;

TEST_CASE(
        "Minimizer-fragment image layout planning is exact aligned and overflow-checked",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto layout = idx::plan_minimizer_fragment_image_layout(
        3u, 5u, 7u, 11u, 13u, 17u);
    REQUIRE(layout);
    REQUIRE((layout->payload == idx::minimizer_fragment_image_section{4096u, 3u, 11u}));
    REQUIRE((layout->directory == idx::minimizer_fragment_image_section{4104u, 5u, 13u}));
    REQUIRE((layout->manifest == idx::minimizer_fragment_image_section{4112u, 7u, 17u}));
    REQUIRE(layout->feature_bitmap == 0u);
    REQUIRE(layout->compact_hash32_direct
        == idx::minimizer_fragment_compact_hash32_section_receipt{});
    REQUIRE(layout->file_bytes == 4119u);

    const auto already_aligned = idx::plan_minimizer_fragment_image_layout(8u, 16u, 24u);
    REQUIRE(already_aligned);
    REQUIRE(already_aligned->directory.offset == 4104u);
    REQUIRE(already_aligned->manifest.offset == 4120u);
    REQUIRE(already_aligned->file_bytes == 4144u);

    const auto with_direct = idx::plan_minimizer_fragment_image_layout(
        3u,
        5u,
        7u,
        11u,
        13u,
        17u,
        idx::minimizer_fragment_compact_hash32_section_spec{
            .bytes = 9u,
            .direct_header_crc64 = 23u,
        });
    REQUIRE(with_direct);
    REQUIRE(with_direct->payload == layout->payload);
    REQUIRE(with_direct->directory == layout->directory);
    REQUIRE(with_direct->manifest == layout->manifest);
    REQUIRE(with_direct->feature_bitmap
        == idx::k_minimizer_fragment_compact_hash32_feature);
    REQUIRE((with_direct->compact_hash32_direct
        == idx::minimizer_fragment_compact_hash32_section_receipt{
            .offset = 4160u,
            .bytes = 9u,
            .direct_header_crc64 = 23u,
        }));
    REQUIRE(with_direct->file_bytes == 4169u);

    const auto empty_direct = idx::plan_minimizer_fragment_image_layout(
        3u,
        5u,
        7u,
        0u,
        0u,
        0u,
        idx::minimizer_fragment_compact_hash32_section_spec{});
    REQUIRE_FALSE(empty_direct);
    REQUIRE(empty_direct.error()
        == idx::minimizer_fragment_image_error::compact_hash32_direct_feature_mismatch);

    const auto payload_add_overflow = idx::plan_minimizer_fragment_image_layout(
        std::numeric_limits<std::uint64_t>::max(), 0u, 0u);
    REQUIRE_FALSE(payload_add_overflow);
    REQUIRE(payload_add_overflow.error()
        == idx::minimizer_fragment_image_error::arithmetic_overflow);

    const auto payload_align_overflow = idx::plan_minimizer_fragment_image_layout(
        std::numeric_limits<std::uint64_t>::max() - 4096u, 0u, 0u);
    REQUIRE_FALSE(payload_align_overflow);
    REQUIRE(payload_align_overflow.error()
        == idx::minimizer_fragment_image_error::arithmetic_overflow);

    const auto directory_align_overflow = idx::plan_minimizer_fragment_image_layout(
        0u, std::numeric_limits<std::uint64_t>::max() - 4096u, 0u);
    REQUIRE_FALSE(directory_align_overflow);
    REQUIRE(directory_align_overflow.error()
        == idx::minimizer_fragment_image_error::arithmetic_overflow);

    const auto manifest_add_overflow = idx::plan_minimizer_fragment_image_layout(
        0u, 0u, std::numeric_limits<std::uint64_t>::max() - 4095u);
    REQUIRE_FALSE(manifest_add_overflow);
    REQUIRE(manifest_add_overflow.error()
        == idx::minimizer_fragment_image_error::arithmetic_overflow);

    const auto direct_align_overflow = idx::plan_minimizer_fragment_image_layout(
        0u,
        0u,
        std::numeric_limits<std::uint64_t>::max() - 4127u,
        0u,
        0u,
        0u,
        idx::minimizer_fragment_compact_hash32_section_spec{
            .bytes = 1u,
            .direct_header_crc64 = 1u,
        });
    REQUIRE_FALSE(direct_align_overflow);
    REQUIRE(direct_align_overflow.error()
        == idx::minimizer_fragment_image_error::arithmetic_overflow);

    const auto direct_add_overflow = idx::plan_minimizer_fragment_image_layout(
        0u,
        0u,
        0u,
        0u,
        0u,
        0u,
        idx::minimizer_fragment_compact_hash32_section_spec{
            .bytes = std::numeric_limits<std::uint64_t>::max(),
            .direct_header_crc64 = 1u,
        });
    REQUIRE_FALSE(direct_add_overflow);
    REQUIRE(direct_add_overflow.error()
        == idx::minimizer_fragment_image_error::arithmetic_overflow);
}

TEST_CASE(
        "Minimizer-fragment image layout validator rejects size limits overlap gaps and arithmetic overflow",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto original = golden_header();

    SECTION("truncated file") {
        const auto result = idx::validate_minimizer_fragment_image_layout(original, 4095u);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::file_too_small);
    }
    SECTION("wrong exact file size") {
        const auto result = idx::validate_minimizer_fragment_image_layout(original, 4120u);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::file_size_mismatch);
    }
    SECTION("off_t bound") {
        const auto result = idx::validate_minimizer_fragment_image_layout(
            original,
            original.file_bytes,
            idx::minimizer_fragment_image_address_limits{
                .max_file_offset = original.file_bytes - 1u,
                .max_mapped_bytes = original.file_bytes,
            });
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::file_offset_not_representable);
    }
    SECTION("size_t bound") {
        const auto result = idx::validate_minimizer_fragment_image_layout(
            original,
            original.file_bytes,
            idx::minimizer_fragment_image_address_limits{
                .max_file_offset = original.file_bytes,
                .max_mapped_bytes = original.file_bytes - 1u,
            });
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::mapped_size_not_representable);
    }
    SECTION("payload must begin immediately after header") {
        auto header = original;
        header.payload.offset = 4104u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::payload_offset_mismatch);
    }
    SECTION("payload end overflows") {
        auto header = original;
        header.payload.bytes = std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
    SECTION("directory overlap") {
        auto header = original;
        header.directory.offset = 4098u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_offset_mismatch);
    }
    SECTION("unexpected directory gap") {
        auto header = original;
        header.directory.offset += 8u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_offset_mismatch);
    }
    SECTION("directory end overflows") {
        auto header = original;
        header.directory.bytes = std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
    SECTION("manifest overlap") {
        auto header = original;
        header.manifest.offset = header.directory.offset;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::manifest_offset_mismatch);
    }
    SECTION("unexpected manifest gap") {
        auto header = original;
        header.manifest.offset += 8u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::manifest_offset_mismatch);
    }
    SECTION("manifest end overflows") {
        auto header = original;
        header.manifest.bytes = std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
    SECTION("manifest must end at exact file size") {
        auto header = original;
        --header.manifest.bytes;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::manifest_end_mismatch);
    }
}
TEST_CASE(
        "Minimizer-fragment image direct layout rejects feature overlap gap overflow and out-of-file extents",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto original = direct_golden_header();
    REQUIRE(original.compact_hash32_direct.offset
            % idx::k_minimizer_fragment_compact_hash32_alignment
        == 0u);
    REQUIRE(idx::validate_minimizer_fragment_image_layout(
        original, original.file_bytes));

    SECTION("unknown feature") {
        auto header = original;
        header.feature_bitmap |= std::uint64_t{1u} << 63u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::unsupported_features);
    }
    SECTION("receipt without feature") {
        auto header = original;
        header.feature_bitmap = 0u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_feature_mismatch);
    }
    SECTION("feature with empty section") {
        auto header = original;
        header.compact_hash32_direct.bytes = 0u;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_feature_mismatch);
    }
    SECTION("direct section overlaps manifest") {
        auto header = original;
        header.compact_hash32_direct.offset = header.manifest.offset;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_offset_mismatch);
    }
    SECTION("direct section has an unexpected aligned gap") {
        auto header = original;
        header.compact_hash32_direct.offset +=
            idx::k_minimizer_fragment_compact_hash32_alignment;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_offset_mismatch);
    }
    SECTION("direct end is outside exact file extent") {
        auto header = original;
        ++header.compact_hash32_direct.bytes;
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::compact_hash32_direct_end_mismatch);
    }
    SECTION("direct end overflows") {
        auto header = original;
        header.compact_hash32_direct.bytes =
            std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_minimizer_fragment_image_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
}
