/**
 * @file persisted_minimizer_image_test.cpp
 * @brief Checks stored index headers, optional sections, and encoded bytes.
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
        "Minimizer-fragment base header has a byte-exact 4096-byte little-endian layout",
        "[component:bioseq][layer:unit][feature:storage]") {
    STATIC_REQUIRE(idx::k_minimizer_fragment_image_header_bytes == 4096u);
    STATIC_REQUIRE(idx::k_minimizer_fragment_image_magic == 0x32563546504d5342ULL);
    STATIC_REQUIRE(
        idx::k_minimizer_fragment_image_relation_backed_storage_kind == 0u);
    STATIC_REQUIRE(sizeof(idx::minimizer_fragment_image_header)
        != idx::k_minimizer_fragment_image_header_bytes);

    const auto encoded = idx::encode_minimizer_fragment_image_header(golden_header());
    REQUIRE(encoded);
    const auto expected = golden_header_bytes();
    REQUIRE(*encoded == expected);

    // The file must begin with these ASCII bytes on every machine.
    constexpr std::array<std::byte, 8> magic_bytes{
        static_cast<std::byte>('B'), static_cast<std::byte>('S'),
        static_cast<std::byte>('M'), static_cast<std::byte>('P'),
        static_cast<std::byte>('F'), static_cast<std::byte>('5'),
        static_cast<std::byte>('V'), static_cast<std::byte>('2'),
    };
    REQUIRE(std::equal(
        magic_bytes.begin(), magic_bytes.end(), encoded->begin()));
    REQUIRE(std::all_of(
        encoded->begin() + 192,
        encoded->end(),
        [](std::byte byte) { return byte == std::byte{0}; }));

    const auto decoded = idx::decode_minimizer_fragment_image_header(expected);
    REQUIRE(decoded);
    auto expected_decoded = golden_header();
    expected_decoded.header_crc64 = reference_header_crc(expected);
    REQUIRE(*decoded == expected_decoded);
    REQUIRE(decoded->header_crc64 == reference_header_crc(expected));
    REQUIRE(decoded->feature_bitmap == 0u);
    REQUIRE(decoded->compact_hash32_direct
        == idx::minimizer_fragment_compact_hash32_section_receipt{});
}

TEST_CASE(
        "Minimizer-fragment image compact-hash32 extension round-trips an opaque direct section",
        "[component:bioseq][layer:unit][feature:storage]") {
    STATIC_REQUIRE(idx::k_minimizer_fragment_compact_hash32_feature == 1u);
    STATIC_REQUIRE(idx::k_minimizer_fragment_image_known_features
        == idx::k_minimizer_fragment_compact_hash32_feature);
    STATIC_REQUIRE(idx::k_minimizer_fragment_compact_hash32_alignment == 64u);
    const auto fixture = make_image_fixture(true);
    REQUIRE(fixture.header.feature_bitmap
        == idx::k_minimizer_fragment_compact_hash32_feature);
    REQUIRE(fixture.header.compact_hash32_direct.bytes
        == fixture.compact_hash32_direct.size());
    REQUIRE(fixture.header.compact_hash32_direct.direct_header_crc64
        == reference_crc64_ecma182(
            std::span<const std::byte>{fixture.compact_hash32_direct}.
                first(256u)));
    REQUIRE(fixture.header.compact_hash32_direct.offset
            % idx::k_minimizer_fragment_compact_hash32_alignment
        == 0u);
    REQUIRE(fixture.header.compact_hash32_direct.offset
            + fixture.header.compact_hash32_direct.bytes
        == fixture.header.file_bytes);

    const auto encoded = idx::encode_minimizer_fragment_image_header(fixture.header);
    REQUIRE(encoded);
    std::array<std::byte, 32u> expected_extension{};
    const std::span<std::byte> extension{expected_extension};
    reference_store_le(
        extension, 0u, idx::k_minimizer_fragment_compact_hash32_feature);
    reference_store_le(
        extension, 8u, fixture.header.compact_hash32_direct.offset);
    reference_store_le(
        extension, 16u, fixture.header.compact_hash32_direct.bytes);
    reference_store_le(
        extension,
        24u,
        fixture.header.compact_hash32_direct.direct_header_crc64);
    REQUIRE(std::equal(
        expected_extension.begin(),
        expected_extension.end(),
        encoded->begin() + 192));
    REQUIRE(std::all_of(
        encoded->begin() + 224,
        encoded->end(),
        [](std::byte byte) { return byte == std::byte{0}; }));

    const auto decoded = idx::decode_minimizer_fragment_image_header(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->header_crc64 == reference_header_crc(*encoded));
    REQUIRE(decoded->feature_bitmap == fixture.header.feature_bitmap);
    REQUIRE(decoded->compact_hash32_direct
        == fixture.header.compact_hash32_direct);

    const auto validated = idx::validate_minimizer_fragment_image(
        fixture.image, validation_options(fixture));
    REQUIRE(validated);
    REQUIRE(std::equal(
        validated->compact_hash32_direct.begin(),
        validated->compact_hash32_direct.end(),
        fixture.compact_hash32_direct.begin(),
        fixture.compact_hash32_direct.end()));
    REQUIRE(validated->compact_hash32_direct.data()
        == fixture.image.data()
            + static_cast<std::ptrdiff_t>(
                fixture.header.compact_hash32_direct.offset));

    auto outer_receipt_only = expected_identity(fixture.header);
    outer_receipt_only.feature_bitmap.reset();
    outer_receipt_only.compact_hash32_direct.reset();
    REQUIRE(idx::validate_minimizer_fragment_image(
        fixture.image,
        idx::minimizer_fragment_image_validation_options{
            .expected_identity = outer_receipt_only,
        }));

    auto opaque_payload_mutation = fixture;
    const auto nested_payload_offset =
        opaque_payload_mutation.header.compact_hash32_direct.offset + 256u;
    opaque_payload_mutation.image[
        static_cast<std::size_t>(nested_payload_offset)] ^= std::byte{1};
    // The outer image compares the saved nested-header checksum and section bounds.
    // The direct index reader checks the nested bytes.
    REQUIRE(idx::validate_minimizer_fragment_image(
        opaque_payload_mutation.image,
        validation_options(opaque_payload_mutation)));
}

TEST_CASE(
        "Minimizer-fragment image bounded open authenticates only the fixed outer header",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto fixture = make_image_fixture(true);

    SECTION("section bytes are opaque and all section CRC work is zero") {
        auto opaque = fixture.image;
        std::fill(
            opaque.begin() + static_cast<std::ptrdiff_t>(
                idx::k_minimizer_fragment_image_header_bytes),
            opaque.end(),
            std::byte{0xa5});
        const auto opened = idx::open_minimizer_fragment_image_bounded(
            opaque, validation_options(fixture));
        REQUIRE(opened);
        REQUIRE(opened->header == fixture.header);
        REQUIRE(opened->header_crc_bytes == idx::k_minimizer_fragment_image_header_bytes);
        REQUIRE(opened->payload_crc_bytes == 0u);
        REQUIRE(opened->directory_crc_bytes == 0u);
        REQUIRE(opened->manifest_crc_bytes == 0u);
        REQUIRE(opened->payload.data()
            == opaque.data() + static_cast<std::ptrdiff_t>(
                fixture.header.payload.offset));
        REQUIRE(opened->directory.data()
            == opaque.data() + static_cast<std::ptrdiff_t>(
                fixture.header.directory.offset));
        REQUIRE(opened->manifest.data()
            == opaque.data() + static_cast<std::ptrdiff_t>(
                fixture.header.manifest.offset));
        REQUIRE(opened->compact_hash32_direct.data()
            == opaque.data() + static_cast<std::ptrdiff_t>(
                fixture.header.compact_hash32_direct.offset));
    }

    SECTION("every byte after the header may be inaccessible") {
        const long native_page_bytes = ::sysconf(_SC_PAGESIZE);
        REQUIRE(native_page_bytes > 0);
        const std::size_t page_bytes = static_cast<std::size_t>(
            native_page_bytes);
        REQUIRE(page_bytes >= idx::k_minimizer_fragment_image_header_bytes);
        REQUIRE(fixture.header.file_bytes
            <= page_bytes + idx::k_minimizer_fragment_image_header_bytes);
        anonymous_mapping_guard mapping{
            .address = ::mmap(
                nullptr,
                page_bytes * 2u,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS,
                -1,
                0),
            .bytes = page_bytes * 2u,
        };
        REQUIRE(mapping.address != MAP_FAILED);
        auto* const mapping_bytes = static_cast<std::byte*>(
            mapping.address);
        std::byte* const header_begin = mapping_bytes
            + page_bytes - idx::k_minimizer_fragment_image_header_bytes;
        std::copy_n(
            fixture.image.begin(),
            idx::k_minimizer_fragment_image_header_bytes,
            header_begin);
        REQUIRE(::mprotect(
            mapping_bytes + page_bytes,
            page_bytes,
            PROT_NONE) == 0);

        const std::span<const std::byte> protected_image{
            header_begin,
            static_cast<std::size_t>(fixture.header.file_bytes)};
        const auto opened = idx::open_minimizer_fragment_image_bounded(
            protected_image, validation_options(fixture));
        REQUIRE(opened);
        REQUIRE(opened->header_crc_bytes == idx::k_minimizer_fragment_image_header_bytes);
        REQUIRE(opened->payload_crc_bytes == 0u);
        REQUIRE(opened->directory_crc_bytes == 0u);
        REQUIRE(opened->manifest_crc_bytes == 0u);
        REQUIRE(opened->compact_hash32_direct.size()
            == fixture.header.compact_hash32_direct.bytes);
    }

    SECTION("header identity and receipt mismatches remain fatal") {
        auto wrong = validation_options(fixture);
        ++wrong.expected_identity.header_crc64;
        const auto opened = idx::open_minimizer_fragment_image_bounded(
            fixture.image, wrong);
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error() == idx::minimizer_fragment_image_error::identity_mismatch);
    }
}

TEST_CASE(
        "Minimizer-fragment image decoder rejects every fixed-code reserved and header CRC violation",
        "[component:bioseq][layer:unit][feature:storage]") {
    const auto valid = golden_header_bytes();

    SECTION("truncated header") {
        REQUIRE(decoded_error(std::span<const std::byte>{valid}.first(4095u))
            == idx::minimizer_fragment_image_error::header_size_mismatch);
    }
    SECTION("oversized standalone header") {
        std::array<std::byte, 4097> oversized{};
        std::copy(valid.begin(), valid.end(), oversized.begin());
        REQUIRE(decoded_error(oversized)
            == idx::minimizer_fragment_image_error::header_size_mismatch);
    }
    SECTION("magic") {
        auto bytes = valid;
        bytes[0] ^= std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes) == idx::minimizer_fragment_image_error::bad_magic);
    }
    SECTION("unsupported header revision") {
        auto bytes = valid;
        reference_store_le(std::span<std::byte>{bytes}, 8u, std::uint32_t{3u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes) == idx::minimizer_fragment_image_error::bad_layout_revision);
    }
    SECTION("encoded header size") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes}, 12u, std::uint32_t{2048u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::bad_header_bytes);
    }
    SECTION("index identity") {
        auto bytes = valid;
        bytes[34] = std::byte{2};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::bad_index_identity);
    }
    SECTION("storage kind") {
        auto bytes = valid;
        bytes[35] = std::byte{2};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes) == idx::minimizer_fragment_image_error::bad_storage_kind);
    }
    SECTION("layout identity") {
        auto bytes = valid;
        reference_store_le(std::span<std::byte>{bytes}, 36u, std::uint16_t{6u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::bad_layout_identity);
    }
    SECTION("reserved bytes 38..39") {
        auto bytes = valid;
        bytes[39] = std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::nonzero_reserved);
    }
    SECTION("reserved bytes 100..103") {
        auto bytes = valid;
        bytes[101] = std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::nonzero_reserved);
    }
    SECTION("reserved bytes 224..4095") {
        auto bytes = valid;
        bytes[4095] = std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::nonzero_reserved);
    }
    SECTION("unknown feature bit") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes},
            192u,
            idx::k_minimizer_fragment_image_known_features | (std::uint64_t{1u} << 63u));
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::unsupported_features);
    }
    SECTION("direct receipt without feature") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes}, 200u, std::uint64_t{4160u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_feature_mismatch);
    }
    SECTION("direct feature without nonempty section") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes},
            192u,
            idx::k_minimizer_fragment_compact_hash32_feature);
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_feature_mismatch);
    }
    SECTION("header CRC") {
        auto bytes = valid;
        bytes[80] ^= std::byte{1};
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::header_crc_mismatch);
    }
    SECTION("checksum field is treated as zero") {
        auto bytes = valid;
        bytes[idx::k_minimizer_fragment_image_header_crc_offset] ^= std::byte{1};
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::header_crc_mismatch);
        REQUIRE(reference_header_crc(bytes) == reference_header_crc(valid));
    }
    SECTION("direct-header receipt is protected by outer header CRC") {
        const auto fixture = make_image_fixture(true);
        auto bytes = idx::encoded_minimizer_fragment_image_header{};
        std::copy_n(fixture.image.begin(), bytes.size(), bytes.begin());
        bytes[216] ^= std::byte{1};
        REQUIRE(decoded_error(bytes)
            == idx::minimizer_fragment_image_error::header_crc_mismatch);
    }
}

TEST_CASE(
        "Minimizer-fragment image encoder refuses invalid fixed codes and geometry",
        "[component:bioseq][layer:unit][feature:storage]") {
    SECTION("magic") {
        auto header = golden_header();
        ++header.magic;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::bad_magic);
    }
    SECTION("unsupported header revision") {
        auto header = golden_header();
        ++header.layout_revision;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::bad_layout_revision);
    }
    SECTION("header bytes") {
        auto header = golden_header();
        --header.header_bytes;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::bad_header_bytes);
    }
    SECTION("index identity") {
        auto header = golden_header();
        ++header.index_identity;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::bad_index_identity);
    }
    SECTION("storage kind") {
        auto header = golden_header();
        header.storage_kind = 2u;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::bad_storage_kind);
    }
    SECTION("layout identity") {
        auto header = golden_header();
        ++header.layout_identity;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::minimizer_fragment_image_error::bad_layout_identity);
    }
    SECTION("geometry") {
        auto header = golden_header();
        ++header.directory.offset;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::directory_offset_mismatch);
    }
    SECTION("unknown feature") {
        auto header = golden_header();
        header.feature_bitmap = std::uint64_t{1u} << 63u;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::unsupported_features);
    }
    SECTION("direct receipt without feature") {
        auto header = golden_header();
        header.compact_hash32_direct =
            idx::minimizer_fragment_compact_hash32_section_receipt{
                .offset = 4160u,
                .bytes = 256u,
                .direct_header_crc64 = 7u,
            };
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_feature_mismatch);
    }
    SECTION("direct feature without nonempty section") {
        auto header = golden_header();
        header.feature_bitmap = idx::k_minimizer_fragment_compact_hash32_feature;
        const auto result = idx::encode_minimizer_fragment_image_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::minimizer_fragment_image_error::
                compact_hash32_direct_feature_mismatch);
    }
}
