/**
 * @file validation_test.cpp
 * @brief Checks stored index metadata and rejects damaged images.
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
        "Minimizer-fragment image validation authenticates bounded metadata and defers payload CRC",
        "[component:bioseq][layer:unit][feature:storage]") {
    auto fixture = make_image_fixture();
    const auto identity = expected_identity(fixture.header);
    const idx::minimizer_fragment_image_validation_options normal_options{
        .expected_identity = identity,
        .payload_crc_policy = idx::minimizer_fragment_payload_crc_policy::deferred,
    };
    const auto normal = idx::validate_minimizer_fragment_image(
        fixture.image, normal_options);
    REQUIRE(normal);
    REQUIRE(std::equal(
        normal->payload.begin(),
        normal->payload.end(),
        fixture.payload.begin(),
        fixture.payload.end()));
    REQUIRE(std::equal(
        normal->directory.begin(),
        normal->directory.end(),
        fixture.directory.begin(),
        fixture.directory.end()));
    REQUIRE(std::equal(
        normal->manifest.begin(),
        normal->manifest.end(),
        fixture.manifest.begin(),
        fixture.manifest.end()));
    REQUIRE(normal->header_crc_bytes == idx::k_minimizer_fragment_image_header_bytes);
    REQUIRE(normal->payload_crc_bytes == 0u);
    REQUIRE(normal->directory_crc_bytes == fixture.directory.size());
    REQUIRE(normal->manifest_crc_bytes == fixture.manifest.size());
    REQUIRE(normal->compact_hash32_direct.empty());

    const idx::minimizer_fragment_image_validation_options scrub_options{
        .expected_identity = identity,
        .payload_crc_policy = idx::minimizer_fragment_payload_crc_policy::validate,
    };
    const auto scrub = idx::validate_minimizer_fragment_image(
        fixture.image, scrub_options);
    REQUIRE(scrub);
    REQUIRE(scrub->payload_crc_bytes == fixture.payload.size());

    fixture.image[static_cast<std::size_t>(fixture.header.payload.offset)]
        ^= std::byte{1};
    // Opening with deferred checks skips the full payload checksum.
    REQUIRE(idx::validate_minimizer_fragment_image(
        fixture.image, normal_options));
    const auto scrubbed = idx::validate_minimizer_fragment_image(
        fixture.image, scrub_options);
    REQUIRE_FALSE(scrubbed);
    REQUIRE(scrubbed.error()
        == idx::minimizer_fragment_image_error::payload_crc_mismatch);
}

TEST_CASE(
        "Minimizer-fragment image validation rejects metadata CRC padding identity and size corruption",
        "[component:bioseq][layer:unit][feature:storage]") {
    SECTION("directory CRC") {
        auto fixture = make_image_fixture();
        fixture.image[static_cast<std::size_t>(fixture.header.directory.offset)]
            ^= std::byte{1};
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_crc_mismatch);
    }
    SECTION("manifest CRC") {
        auto fixture = make_image_fixture();
        fixture.image[static_cast<std::size_t>(fixture.header.manifest.offset)]
            ^= std::byte{1};
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::manifest_crc_mismatch);
    }
    SECTION("payload-to-directory alignment padding") {
        auto fixture = make_image_fixture();
        const auto padding_offset = fixture.header.payload.offset
            + fixture.header.payload.bytes;
        REQUIRE(padding_offset < fixture.header.directory.offset);
        fixture.image[static_cast<std::size_t>(padding_offset)] = std::byte{1};
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::nonzero_section_padding);
    }
    SECTION("manifest-to-direct alignment padding") {
        auto fixture = make_image_fixture(true);
        const auto padding_offset = fixture.header.manifest.offset
            + fixture.header.manifest.bytes;
        REQUIRE(padding_offset
            < fixture.header.compact_hash32_direct.offset);
        fixture.image[static_cast<std::size_t>(padding_offset)] =
            std::byte{1};
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::nonzero_section_padding);
    }
    // Each directory entry is 128 bytes, so the directory ends on an
    // eight-byte boundary. No padding is needed before the manifest.
    STATIC_REQUIRE(
        idx::grouped_soa_bucket_directory_entry::encoded_bytes
            % idx::k_minimizer_fragment_image_section_alignment
        == 0u);
    SECTION("identity") {
        auto fixture = make_image_fixture();
        auto identity = expected_identity(fixture.header);
        ++identity.generation_id;
        const idx::minimizer_fragment_image_validation_options options{
            .expected_identity = identity,
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::identity_mismatch);
    }
    SECTION("nested direct-header receipt mismatch after valid outer reseal") {
        auto fixture = make_image_fixture(true);
        fixture.image[216] ^= std::byte{1};
        reference_reseal_header(fixture.image);
        const auto decoded = idx::decode_minimizer_fragment_image_header(
            std::span<const std::byte>{fixture.image}.first(
                idx::k_minimizer_fragment_image_header_bytes));
        REQUIRE(decoded);
        auto identity = expected_identity(fixture.header);
        // Update the expected outer checksum so validation reaches the changed
        // nested-header checksum field.
        identity.header_crc64 = decoded->header_crc64;
        const idx::minimizer_fragment_image_validation_options options{
            .expected_identity = identity,
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::identity_mismatch);
    }
    SECTION("file truncation") {
        auto fixture = make_image_fixture();
        fixture.image.pop_back();
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::file_size_mismatch);
    }
    SECTION("header truncation") {
        std::vector<std::byte> image(4095u, std::byte{0});
        const auto fixture = make_image_fixture();
        const auto result = idx::validate_minimizer_fragment_image(
            image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::file_too_small);
    }
    SECTION("file oversize") {
        auto fixture = make_image_fixture();
        fixture.image.push_back(std::byte{0});
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::file_size_mismatch);
    }
    SECTION("injected off_t oversize") {
        auto fixture = make_image_fixture();
        const idx::minimizer_fragment_image_validation_options options{
            .address_limits = idx::minimizer_fragment_image_address_limits{
                .max_file_offset = fixture.header.file_bytes - 1u,
                .max_mapped_bytes = fixture.header.file_bytes,
            },
            .expected_identity = expected_identity(fixture.header),
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::file_offset_not_representable);
    }
    SECTION("injected size_t oversize") {
        auto fixture = make_image_fixture();
        const idx::minimizer_fragment_image_validation_options options{
            .address_limits = idx::minimizer_fragment_image_address_limits{
                .max_file_offset = fixture.header.file_bytes,
                .max_mapped_bytes = fixture.header.file_bytes - 1u,
            },
            .expected_identity = expected_identity(fixture.header),
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::mapped_size_not_representable);
    }
}

TEST_CASE(
        "Minimizer-fragment image rejects corrupt geometry before constructing section spans",
        "[component:bioseq][layer:unit][feature:storage]") {
    SECTION("overlapping directory") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            136u,
            fixture.header.payload.offset + 1u);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_offset_mismatch);
    }
    SECTION("payload end overflow") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            120u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
    SECTION("directory end overflow") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            144u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
    SECTION("manifest end overflow") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            168u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
    SECTION("unexpected directory gap") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            136u,
            fixture.header.directory.offset + 8u);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_offset_mismatch);
    }
    SECTION("direct section overlaps manifest") {
        auto fixture = make_image_fixture(true);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            200u,
            fixture.header.manifest.offset);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_offset_mismatch);
    }
    SECTION("direct section ends outside file") {
        auto fixture = make_image_fixture(true);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            208u,
            fixture.header.compact_hash32_direct.bytes + 1u);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::compact_hash32_direct_end_mismatch);
    }
    SECTION("direct section end overflows") {
        auto fixture = make_image_fixture(true);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            208u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::arithmetic_overflow);
    }
}

TEST_CASE(
        "Minimizer-fragment image open-ready validation requires every authority field",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto fixture = make_image_fixture();
    const auto base = expected_identity(fixture.header);
    std::array<idx::expected_minimizer_fragment_image_identity, 22u> candidates{};
    candidates.fill(base);
    ++candidates[0].database_oid;
    ++candidates[1].tablespace_oid;
    ++candidates[2].main_relfilenode;
    ++candidates[3].user_relid;
    ++candidates[4].column_attno;
    ++candidates[5].index_identity;
    ++candidates[6].layout_identity;
    ++candidates[7].query_key;
    ++candidates[8].generation_id;
    ++candidates[9].source_identity;
    ++candidates[10].source_content_crc;
    ++candidates[11].storage_contract_checksum;
    ++candidates[12].target_length;
    ++candidates[13].file_bytes;
    ++candidates[14].position_segment_count;
    ++candidates[15].position_segment_span_bp;
    ++candidates[16].payload.crc64;
    ++candidates[17].directory.crc64;
    ++candidates[18].manifest.crc64;
    ++candidates[19].header_crc64;
    candidates[20].feature_bitmap =
        idx::k_minimizer_fragment_compact_hash32_feature;
    candidates[21].compact_hash32_direct =
        idx::minimizer_fragment_compact_hash32_section_receipt{
            .offset = 4160u,
            .bytes = 256u,
            .direct_header_crc64 = 1u,
        };
    constexpr std::array<std::string_view, 22u> labels{
        "database", "tablespace", "relfilenode", "user relation",
        "column", "index identity", "layout identity", "query key",
        "generation", "source identity", "source content",
        "storage contract", "target length", "file bytes", "segment count",
        "segment span", "payload receipt", "directory receipt",
        "manifest receipt", "header receipt", "feature bitmap",
        "direct-section receipt",
    };
    for (std::size_t index = 0u; index < candidates.size(); ++index) {
        INFO("authority field=" << labels[index]);
        const idx::minimizer_fragment_image_validation_options options{
            .expected_identity = candidates[index],
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::identity_mismatch);
    }
}

TEST_CASE(
        "Minimizer-fragment image cross-checks decoded grouped-SoA manifest and directory metadata",
        "[component:bioseq][layer:unit][feature:storage]") {
    SECTION("header and manifest duplicated identity disagree") {
        auto fixture = make_image_fixture();
        const std::size_t manifest_offset = static_cast<std::size_t>(
            fixture.header.manifest.offset);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            manifest_offset + 32u,
            std::uint64_t{fixture.header.target_length + 1u});
        const auto manifest = std::span<const std::byte>{fixture.image}.
            subspan(
                manifest_offset,
                static_cast<std::size_t>(fixture.header.manifest.bytes));
        reference_store_le(
            std::span<std::byte>{fixture.image},
            176u,
            reference_crc64_ecma182(manifest));
        reference_reseal_header(fixture.image);
        const auto header = idx::decode_minimizer_fragment_image_header(
            std::span<const std::byte>{fixture.image}.first(
                idx::k_minimizer_fragment_image_header_bytes));
        REQUIRE(header);
        const idx::minimizer_fragment_image_validation_options options{
            .expected_identity = expected_identity(*header),
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::generation_metadata_mismatch);
    }

    SECTION("directory structure is invalid under self-consistent CRCs") {
        auto fixture = make_image_fixture();
        const std::size_t directory_offset = static_cast<std::size_t>(
            fixture.header.directory.offset);
        const std::size_t manifest_offset = static_cast<std::size_t>(
            fixture.header.manifest.offset);
        // The first directory entry must have partition ID zero.
        reference_store_le(
            std::span<std::byte>{fixture.image},
            directory_offset,
            std::uint32_t{1u});
        const auto directory = std::span<const std::byte>{fixture.image}.
            subspan(
                directory_offset,
                static_cast<std::size_t>(fixture.header.directory.bytes));
        const std::uint64_t directory_crc =
            reference_crc64_ecma182(directory);
        reference_store_le(
            std::span<std::byte>{fixture.image}, 152u, directory_crc);
        // Update both copies of the directory checksum so validation reaches
        // the invalid partition ID.
        reference_store_le(
            std::span<std::byte>{fixture.image},
            manifest_offset + 80u,
            directory_crc);
        const auto manifest = std::span<const std::byte>{fixture.image}.
            subspan(
                manifest_offset,
                static_cast<std::size_t>(fixture.header.manifest.bytes));
        reference_store_le(
            std::span<std::byte>{fixture.image},
            176u,
            reference_crc64_ecma182(manifest));
        reference_reseal_header(fixture.image);
        const auto header = idx::decode_minimizer_fragment_image_header(
            std::span<const std::byte>{fixture.image}.first(
                idx::k_minimizer_fragment_image_header_bytes));
        REQUIRE(header);
        const idx::minimizer_fragment_image_validation_options options{
            .expected_identity = expected_identity(*header),
        };
        const auto result = idx::validate_minimizer_fragment_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_metadata_invalid);
    }
}

TEST_CASE(
        "Minimizer-fragment image rejects a header-only image without grouped-SoA metadata",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto layout = idx::plan_minimizer_fragment_image_layout(0u, 0u, 0u);
    REQUIRE(layout);
    REQUIRE(layout->file_bytes == idx::k_minimizer_fragment_image_header_bytes);

    auto header = golden_header();
    header.payload = layout->payload;
    header.directory = layout->directory;
    header.manifest = layout->manifest;
    header.file_bytes = layout->file_bytes;
    const auto encoded = idx::encode_minimizer_fragment_image_header(header);
    REQUIRE(encoded);
    const auto decoded = idx::decode_minimizer_fragment_image_header(*encoded);
    REQUIRE(decoded);

    const idx::minimizer_fragment_image_validation_options options{
        .expected_identity = expected_identity(*decoded),
        .payload_crc_policy = idx::minimizer_fragment_payload_crc_policy::validate,
    };
    const auto image = idx::validate_minimizer_fragment_image(*encoded, options);
    REQUIRE_FALSE(image);
    REQUIRE(image.error()
        == idx::minimizer_fragment_image_error::manifest_metadata_invalid);
}
