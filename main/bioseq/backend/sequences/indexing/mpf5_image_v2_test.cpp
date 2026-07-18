// DOC0383 section 13.2 — byte-exact tests for the PG-free MPF5 v2 image ABI.

#include "common_tests.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.mpf5_image_v2;

namespace {

namespace idx = bioseq::indexing;

template <class UInt>
requires (std::is_unsigned_v<UInt>)
constexpr void reference_store_le(
        std::span<std::byte> bytes,
        std::size_t offset,
        UInt value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(UInt); ++byte) {
        bytes[offset + byte] = static_cast<std::byte>(value & UInt{0xffu});
        value >>= 8u;
    }
}

/// Independent bit-at-a-time CRC-64/ECMA-182 oracle. It deliberately does not
/// call the production lookup-table implementation used by the codec.
[[nodiscard]] constexpr std::uint64_t reference_crc64_ecma182(
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

[[nodiscard]] constexpr std::uint64_t reference_header_crc(
        idx::EncodedMpf5HeaderV2 header) noexcept {
    std::fill(
        header.begin() + static_cast<std::ptrdiff_t>(
            idx::kMpf5V2HeaderCrcOffset),
        header.begin() + static_cast<std::ptrdiff_t>(
            idx::kMpf5V2HeaderCrcOffset + idx::kMpf5V2HeaderCrcBytes),
        std::byte{0});
    return reference_crc64_ecma182(header);
}

void reference_reseal_header(std::span<std::byte> image) {
    auto header = idx::EncodedMpf5HeaderV2{};
    std::copy_n(image.begin(), header.size(), header.begin());
    reference_store_le(
        image,
        idx::kMpf5V2HeaderCrcOffset,
        reference_header_crc(header));
}

[[nodiscard]] idx::DecodedMpf5HeaderV2 golden_header() {
    return idx::DecodedMpf5HeaderV2{
        .magic = idx::kMpf5V2Magic,
        .version = idx::kMpf5V2Version,
        .header_bytes = idx::kMpf5V2HeaderBytes,
        .database_oid = 0x13121110u,
        .tablespace_oid = 0x17161514u,
        .main_relfilenode = 0x1b1a1918u,
        .user_relid = 0x1f1e1d1cu,
        .column_attno = 0x2120u,
        .index_kind = idx::kMpf5V2IndexKind,
        .provider = static_cast<std::uint8_t>(
            idx::Mpf5V2Provider::mapped_file),
        .wire_format = idx::kMpf5V2WireFormat,
        .query_key = 0x4746454443424140ULL,
        .generation_id = 0x4f4e4d4c4b4a4948ULL,
        .source_identity = 0x5756555453525150ULL,
        .source_content_crc = 0x5f5e5d5c5b5a5958ULL,
        .compatibility_crc = 0x6766656463626160ULL,
        .target_length = 0x6f6e6d6c6b6a6968ULL,
        .file_bytes = 4119u,
        .position_segment_count = 0x73727170u,
        .position_segment_span_bp = 0x7f7e7d7c7b7a7978ULL,
        .payload = idx::ImageSectionV2{
            .offset = 4096u,
            .bytes = 3u,
            .crc64 = 0x8786858483828180ULL,
        },
        .directory = idx::ImageSectionV2{
            .offset = 4104u,
            .bytes = 5u,
            .crc64 = 0x8f8e8d8c8b8a8988ULL,
        },
        .manifest = idx::ImageSectionV2{
            .offset = 4112u,
            .bytes = 7u,
            .crc64 = 0x9796959493929190ULL,
        },
        // Deliberately nonzero: the encoder must ignore this convenience
        // value and checksum the wire field as zero.
        .header_crc64 = std::numeric_limits<std::uint64_t>::max(),
    };
}

[[nodiscard]] idx::DecodedMpf5HeaderV2 direct_golden_header() {
    auto header = golden_header();
    const auto layout = idx::plan_mpf5_v2_layout(
        header.payload.bytes,
        header.directory.bytes,
        header.manifest.bytes,
        header.payload.crc64,
        header.directory.crc64,
        header.manifest.crc64,
        idx::Mpf5V2CompactH32DirectSectionSpec{
            .bytes = 257u,
            .direct_header_crc64 = 0xa7a6a5a4a3a2a1a0ULL,
        });
    if (!layout) {
        std::terminate();
    }
    header.payload = layout->payload;
    header.directory = layout->directory;
    header.manifest = layout->manifest;
    header.feature_bitmap = layout->feature_bitmap;
    header.compact_h32_direct = layout->compact_h32_direct;
    header.file_bytes = layout->file_bytes;
    return header;
}

[[nodiscard]] idx::EncodedMpf5HeaderV2 golden_header_bytes() {
    const auto header = golden_header();
    idx::EncodedMpf5HeaderV2 expected{};
    const std::span<std::byte> bytes{expected};
    reference_store_le(bytes, 0u, header.magic);
    reference_store_le(bytes, 8u, header.version);
    reference_store_le(bytes, 12u, header.header_bytes);
    reference_store_le(bytes, 16u, header.database_oid);
    reference_store_le(bytes, 20u, header.tablespace_oid);
    reference_store_le(bytes, 24u, header.main_relfilenode);
    reference_store_le(bytes, 28u, header.user_relid);
    reference_store_le(bytes, 32u, header.column_attno);
    reference_store_le(bytes, 34u, header.index_kind);
    reference_store_le(bytes, 35u, header.provider);
    reference_store_le(bytes, 36u, header.wire_format);
    reference_store_le(bytes, 40u, header.query_key);
    reference_store_le(bytes, 48u, header.generation_id);
    reference_store_le(bytes, 56u, header.source_identity);
    reference_store_le(bytes, 64u, header.source_content_crc);
    reference_store_le(bytes, 72u, header.compatibility_crc);
    reference_store_le(bytes, 80u, header.target_length);
    reference_store_le(bytes, 88u, header.file_bytes);
    reference_store_le(bytes, 96u, header.position_segment_count);
    reference_store_le(bytes, 104u, header.position_segment_span_bp);
    reference_store_le(bytes, 112u, header.payload.offset);
    reference_store_le(bytes, 120u, header.payload.bytes);
    reference_store_le(bytes, 128u, header.payload.crc64);
    reference_store_le(bytes, 136u, header.directory.offset);
    reference_store_le(bytes, 144u, header.directory.bytes);
    reference_store_le(bytes, 152u, header.directory.crc64);
    reference_store_le(bytes, 160u, header.manifest.offset);
    reference_store_le(bytes, 168u, header.manifest.bytes);
    reference_store_le(bytes, 176u, header.manifest.crc64);
    reference_store_le(
        bytes,
        idx::kMpf5V2HeaderCrcOffset,
        reference_header_crc(expected));
    return expected;
}

[[nodiscard]] idx::Mpf5V2ExpectedIdentity expected_identity(
        const idx::DecodedMpf5HeaderV2& header) noexcept {
    return idx::Mpf5V2ExpectedIdentity{
        .database_oid = header.database_oid,
        .tablespace_oid = header.tablespace_oid,
        .main_relfilenode = header.main_relfilenode,
        .user_relid = header.user_relid,
        .column_attno = header.column_attno,
        .index_kind = header.index_kind,
        .provider = header.provider,
        .wire_format = header.wire_format,
        .query_key = header.query_key,
        .generation_id = header.generation_id,
        .source_identity = header.source_identity,
        .source_content_crc = header.source_content_crc,
        .compatibility_crc = header.compatibility_crc,
        .target_length = header.target_length,
        .file_bytes = header.file_bytes,
        .position_segment_count = header.position_segment_count,
        .position_segment_span_bp = header.position_segment_span_bp,
        .payload = header.payload,
        .directory = header.directory,
        .manifest = header.manifest,
        .header_crc64 = header.header_crc64,
        .feature_bitmap = header.feature_bitmap,
        .compact_h32_direct = header.compact_h32_direct,
    };
}

struct ImageFixture {
    idx::DecodedMpf5HeaderV2 header{};
    std::vector<std::byte> payload{};
    std::vector<std::byte> directory{};
    std::vector<std::byte> manifest{};
    std::vector<std::byte> compact_h32_direct{};
    std::vector<std::byte> image{};
};

[[nodiscard]] ImageFixture make_image_fixture(
        idx::Mpf5V2Provider provider = idx::Mpf5V2Provider::mapped_file,
        bool include_compact_h32_direct = false) {
    constexpr std::uint64_t target_length = 100u;
    constexpr std::uint64_t segment_span = 32'768u;
    constexpr std::uint64_t max_occ = 10u;
    const auto profile = idx::minimizer_generation_payload_profile(
        idx::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        idx::kLegacyMinimapV1SchemeIdentity,
        idx::kMinimizerPersistedDna4AlphabetCode,
        15u,
        segment_span,
        max_occ);
    if (!profile || !profile->shape_v5) {
        std::terminate();
    }
    constexpr idx::MinimizerScalarWidth offset_width =
        idx::MinimizerScalarWidth::u16;
    const auto control_layout = idx::minimizer_shape_v5_control_layout(
        1u,
        profile->shape_v5->hash_width,
        offset_width,
        profile->shape_v5->count_width);
    if (!control_layout) {
        std::terminate();
    }

    std::vector<std::byte> controls(
        static_cast<std::size_t>(control_layout->encoded_bytes),
        std::byte{0});
    reference_store_le(
        controls,
        static_cast<std::size_t>(control_layout->hash_offset),
        std::uint32_t{7u});
    reference_store_le(
        controls,
        static_cast<std::size_t>(control_layout->prefix_offset),
        std::uint16_t{0u});
    reference_store_le(
        controls,
        static_cast<std::size_t>(control_layout->prefix_offset)
            + sizeof(std::uint16_t),
        std::uint16_t{1u});
    reference_store_le(
        controls,
        static_cast<std::size_t>(control_layout->count_offset),
        std::uint16_t{1u});
    std::vector<std::byte> positions(sizeof(std::uint16_t), std::byte{0});
    reference_store_le(positions, 0u, std::uint16_t{20u});

    const idx::MinimizerBucketDirEntry entry{
        .partition_id = 0u,
        .position_segment = 0,
        .lo_hash = 7u,
        .hi_hash = 7u,
        .sub_key = 0,
        .control_first_chunk = 0u,
        .control_chunk_count = 1u,
        .position_first_chunk = 0u,
        .position_chunk_count = 1u,
        .control_element_count = 1u,
        .position_element_count = 1u,
        .control_encoded_bytes = controls.size(),
        .position_encoded_bytes = positions.size(),
        .control_checksum = idx::crc64_ecma182(controls),
        .position_checksum = idx::crc64_ecma182(positions),
        .shape_offset_width = offset_width,
    };
    const std::array entries{entry};
    const auto encoded_directory = idx::encode_minimizer_bucket_directory(
        entries, *profile);
    if (!encoded_directory) {
        std::terminate();
    }

    ImageFixture fixture{};
    fixture.header = golden_header();
    fixture.payload.reserve(controls.size() + positions.size());
    fixture.payload.insert(
        fixture.payload.end(), controls.begin(), controls.end());
    fixture.payload.insert(
        fixture.payload.end(), positions.begin(), positions.end());
    fixture.directory = *encoded_directory;

    const std::uint64_t raw_payload_crc =
        idx::crc64_ecma182(fixture.payload);
    const idx::MinimizerGenerationManifest generation_manifest{
        .wire_format = idx::MinimizerGenerationWireFormat::
            grouped_shape_soa_segment_local,
        .generation_id = fixture.header.generation_id,
        .source_identity = fixture.header.source_identity,
        .source_content_checksum = fixture.header.source_content_crc,
        .target_length = target_length,
        .control_count = 1u,
        .position_count = 1u,
        .payload_bytes = fixture.payload.size(),
        // The manifest authenticates the little-endian sequence of
        // address-bound row CRCs, not the concatenated raw payload bytes.
        .payload_checksum = raw_payload_crc ^ std::uint64_t{1u},
        .directory_bytes = fixture.directory.size(),
        .directory_checksum = idx::crc64_ecma182(fixture.directory),
        .position_segment_count = 1u,
        .extent_count = 2u,
        .directory_chunk_count = 1u,
        .alphabet_code = idx::kMinimizerPersistedDna4AlphabetCode,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_version = profile->gpu_abi_version,
        .position_segment_span_bp = segment_span,
        .segment_geometry_version = idx::MinimizerGenerationManifest::
            uniform_segment_geometry_version,
        .minimizer_scheme_id = idx::kLegacyMinimapV1SchemeId,
        .minimizer_scheme_version = idx::kLegacyMinimapV1SchemeVersion,
        .producer_backend_id = idx::MinimizerProducerBackendId::scalar,
        .producer_backend_identity_version =
            idx::kMinimizerProducerBackendIdentityVersion,
        .shape_v5 = profile->shape_v5,
    };
    const auto encoded_manifest =
        idx::encode_minimizer_generation_manifest_v5(
            generation_manifest);
    if (!encoded_manifest) {
        std::terminate();
    }
    fixture.manifest.assign(
        encoded_manifest->begin(), encoded_manifest->end());

    std::optional<idx::Mpf5V2CompactH32DirectSectionSpec> direct_spec;
    if (include_compact_h32_direct) {
        // The MPF5 codec treats this section as opaque.  The first 256 bytes
        // stand in for the nested direct header and the remainder for nested
        // payload; only the direct codec interprets either one.
        constexpr std::size_t opaque_direct_header_bytes = 256u;
        fixture.compact_h32_direct.resize(
            opaque_direct_header_bytes + 37u, std::byte{0});
        for (std::size_t byte = 0u;
             byte < fixture.compact_h32_direct.size(); ++byte) {
            fixture.compact_h32_direct[byte] = static_cast<std::byte>(
                (byte * 17u + 3u) & 0xffu);
        }
        direct_spec = idx::Mpf5V2CompactH32DirectSectionSpec{
            .bytes = fixture.compact_h32_direct.size(),
            .direct_header_crc64 = reference_crc64_ecma182(
                std::span<const std::byte>{fixture.compact_h32_direct}.
                    first(opaque_direct_header_bytes)),
        };
    }

    const auto layout = idx::plan_mpf5_v2_layout(
        fixture.payload.size(),
        fixture.directory.size(),
        fixture.manifest.size(),
        raw_payload_crc,
        generation_manifest.directory_checksum,
        idx::crc64_ecma182(fixture.manifest),
        direct_spec);
    if (!layout) {
        std::terminate();
    }
    fixture.header.provider = static_cast<std::uint8_t>(provider);
    fixture.header.wire_format = idx::kMpf5V2WireFormat;
    fixture.header.source_identity = generation_manifest.source_identity;
    fixture.header.source_content_crc =
        generation_manifest.source_content_checksum;
    fixture.header.compatibility_crc =
        idx::minimizer_generation_compatibility_checksum(
            generation_manifest);
    fixture.header.target_length = generation_manifest.target_length;
    fixture.header.position_segment_count =
        generation_manifest.position_segment_count;
    fixture.header.position_segment_span_bp =
        generation_manifest.position_segment_span_bp;
    fixture.header.payload = layout->payload;
    fixture.header.directory = layout->directory;
    fixture.header.manifest = layout->manifest;
    fixture.header.feature_bitmap = layout->feature_bitmap;
    fixture.header.compact_h32_direct = layout->compact_h32_direct;
    fixture.header.file_bytes = layout->file_bytes;
    fixture.header.header_crc64 = 0u;

    const auto encoded = idx::encode_mpf5_v2_header(fixture.header);
    if (!encoded) {
        std::terminate();
    }
    const auto decoded_header = idx::decode_mpf5_v2_header(*encoded);
    if (!decoded_header) {
        std::terminate();
    }
    fixture.header = *decoded_header;
    fixture.image.assign(
        static_cast<std::size_t>(layout->file_bytes), std::byte{0});
    std::copy(encoded->begin(), encoded->end(), fixture.image.begin());
    std::copy(
        fixture.payload.begin(),
        fixture.payload.end(),
        fixture.image.begin() + static_cast<std::ptrdiff_t>(
            layout->payload.offset));
    std::copy(
        fixture.directory.begin(),
        fixture.directory.end(),
        fixture.image.begin() + static_cast<std::ptrdiff_t>(
            layout->directory.offset));
    std::copy(
        fixture.manifest.begin(),
        fixture.manifest.end(),
        fixture.image.begin() + static_cast<std::ptrdiff_t>(
            layout->manifest.offset));
    if (include_compact_h32_direct) {
        std::copy(
            fixture.compact_h32_direct.begin(),
            fixture.compact_h32_direct.end(),
            fixture.image.begin() + static_cast<std::ptrdiff_t>(
                layout->compact_h32_direct.offset));
    }
    return fixture;
}

[[nodiscard]] idx::Mpf5V2ValidationOptions validation_options(
        const ImageFixture& fixture,
        idx::Mpf5V2PayloadCrcPolicy payload_crc_policy =
            idx::Mpf5V2PayloadCrcPolicy::deferred) noexcept {
    return idx::Mpf5V2ValidationOptions{
        .expected_identity = expected_identity(fixture.header),
        .payload_crc_policy = payload_crc_policy,
    };
}

[[nodiscard]] idx::Mpf5V2WireError decoded_error(
        std::span<const std::byte> encoded) {
    const auto result = idx::decode_mpf5_v2_header(encoded);
    if (result) {
        std::terminate();
    }
    return result.error();
}

struct AnonymousMappingGuard {
    void* address = MAP_FAILED;
    std::size_t bytes = 0u;

    ~AnonymousMappingGuard() {
        if (address != MAP_FAILED) {
            (void)::munmap(address, bytes);
        }
    }
};

} // namespace

TEST_CASE(
        "MPF5 v2 legacy header is a byte-exact little-endian 4096-byte ABI",
        "[doc0383][mpf5-v2][wire][golden]") {
    STATIC_REQUIRE(idx::kMpf5V2HeaderBytes == 4096u);
    STATIC_REQUIRE(idx::kMpf5V2Magic == 0x32563546504d5342ULL);
    STATIC_REQUIRE(static_cast<std::uint8_t>(
        idx::Mpf5V2Provider::postgres_relation) == 0u);
    STATIC_REQUIRE(static_cast<std::uint8_t>(
        idx::Mpf5V2Provider::mapped_file) == 1u);
    STATIC_REQUIRE(sizeof(idx::DecodedMpf5HeaderV2)
        != idx::kMpf5V2HeaderBytes);

    const auto encoded = idx::encode_mpf5_v2_header(golden_header());
    REQUIRE(encoded);
    const auto expected = golden_header_bytes();
    REQUIRE(*encoded == expected);

    // Frozen magic bytes read as ASCII in file order, independent of the
    // machine's native integer representation.
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

    const auto decoded = idx::decode_mpf5_v2_header(expected);
    REQUIRE(decoded);
    auto expected_decoded = golden_header();
    expected_decoded.header_crc64 = reference_header_crc(expected);
    REQUIRE(*decoded == expected_decoded);
    REQUIRE(decoded->header_crc64 == reference_header_crc(expected));
    REQUIRE(decoded->feature_bitmap == 0u);
    REQUIRE(decoded->compact_h32_direct
        == idx::Mpf5V2CompactH32DirectSectionReceipt{});
}

TEST_CASE(
        "MPF5 v2 compact-H32 extension round-trips an opaque direct section",
        "[doc0383][mpf5-v2][wire][direct][golden]") {
    STATIC_REQUIRE(idx::kMpf5V2FeatureCompactH32Direct == 1u);
    STATIC_REQUIRE(idx::kMpf5V2KnownFeatures
        == idx::kMpf5V2FeatureCompactH32Direct);
    STATIC_REQUIRE(idx::kMpf5V2CompactH32DirectAlignment == 64u);
    const auto fixture = make_image_fixture(
        idx::Mpf5V2Provider::mapped_file, true);
    REQUIRE(fixture.header.feature_bitmap
        == idx::kMpf5V2FeatureCompactH32Direct);
    REQUIRE(fixture.header.compact_h32_direct.bytes
        == fixture.compact_h32_direct.size());
    REQUIRE(fixture.header.compact_h32_direct.direct_header_crc64
        == reference_crc64_ecma182(
            std::span<const std::byte>{fixture.compact_h32_direct}.
                first(256u)));
    REQUIRE(fixture.header.compact_h32_direct.offset
            % idx::kMpf5V2CompactH32DirectAlignment
        == 0u);
    REQUIRE(fixture.header.compact_h32_direct.offset
            + fixture.header.compact_h32_direct.bytes
        == fixture.header.file_bytes);

    const auto encoded = idx::encode_mpf5_v2_header(fixture.header);
    REQUIRE(encoded);
    std::array<std::byte, 32u> expected_extension{};
    const std::span<std::byte> extension{expected_extension};
    reference_store_le(
        extension, 0u, idx::kMpf5V2FeatureCompactH32Direct);
    reference_store_le(
        extension, 8u, fixture.header.compact_h32_direct.offset);
    reference_store_le(
        extension, 16u, fixture.header.compact_h32_direct.bytes);
    reference_store_le(
        extension,
        24u,
        fixture.header.compact_h32_direct.direct_header_crc64);
    REQUIRE(std::equal(
        expected_extension.begin(),
        expected_extension.end(),
        encoded->begin() + 192));
    REQUIRE(std::all_of(
        encoded->begin() + 224,
        encoded->end(),
        [](std::byte byte) { return byte == std::byte{0}; }));

    const auto decoded = idx::decode_mpf5_v2_header(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->header_crc64 == reference_header_crc(*encoded));
    REQUIRE(decoded->feature_bitmap == fixture.header.feature_bitmap);
    REQUIRE(decoded->compact_h32_direct
        == fixture.header.compact_h32_direct);

    const auto validated = idx::validate_mpf5_v2_image(
        fixture.image, validation_options(fixture));
    REQUIRE(validated);
    REQUIRE(std::equal(
        validated->compact_h32_direct.begin(),
        validated->compact_h32_direct.end(),
        fixture.compact_h32_direct.begin(),
        fixture.compact_h32_direct.end()));
    REQUIRE(validated->compact_h32_direct.data()
        == fixture.image.data()
            + static_cast<std::ptrdiff_t>(
                fixture.header.compact_h32_direct.offset));

    auto outer_receipt_only = expected_identity(fixture.header);
    outer_receipt_only.feature_bitmap.reset();
    outer_receipt_only.compact_h32_direct.reset();
    REQUIRE(idx::validate_mpf5_v2_image(
        fixture.image,
        idx::Mpf5V2ValidationOptions{
            .expected_identity = outer_receipt_only,
        }));

    auto opaque_payload_mutation = fixture;
    const auto nested_payload_offset =
        opaque_payload_mutation.header.compact_h32_direct.offset + 256u;
    opaque_payload_mutation.image[
        static_cast<std::size_t>(nested_payload_offset)] ^= std::byte{1};
    // MPF5 authenticates the nested-header receipt and extent, not the opaque
    // direct payload.  Nested section CRCs remain the direct codec's job.
    REQUIRE(idx::validate_mpf5_v2_image(
        opaque_payload_mutation.image,
        validation_options(opaque_payload_mutation)));
}

TEST_CASE(
        "MPF5 v2 bounded open authenticates only the fixed outer header",
        "[doc0383][mpf5-v2][bounded-open][no-section-touch]") {
    const auto fixture = make_image_fixture(
        idx::Mpf5V2Provider::mapped_file, true);

    SECTION("section bytes are opaque and all section CRC work is zero") {
        auto opaque = fixture.image;
        std::fill(
            opaque.begin() + static_cast<std::ptrdiff_t>(
                idx::kMpf5V2HeaderBytes),
            opaque.end(),
            std::byte{0xa5});
        const auto opened = idx::open_mpf5_v2_image_bounded(
            opaque, validation_options(fixture));
        REQUIRE(opened);
        REQUIRE(opened->header == fixture.header);
        REQUIRE(opened->header_crc_bytes == idx::kMpf5V2HeaderBytes);
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
        REQUIRE(opened->compact_h32_direct.data()
            == opaque.data() + static_cast<std::ptrdiff_t>(
                fixture.header.compact_h32_direct.offset));
    }

    SECTION("every byte after the header may be inaccessible") {
        const long native_page_bytes = ::sysconf(_SC_PAGESIZE);
        REQUIRE(native_page_bytes > 0);
        const std::size_t page_bytes = static_cast<std::size_t>(
            native_page_bytes);
        REQUIRE(page_bytes >= idx::kMpf5V2HeaderBytes);
        REQUIRE(fixture.header.file_bytes
            <= page_bytes + idx::kMpf5V2HeaderBytes);
        AnonymousMappingGuard mapping{
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
            + page_bytes - idx::kMpf5V2HeaderBytes;
        std::copy_n(
            fixture.image.begin(),
            idx::kMpf5V2HeaderBytes,
            header_begin);
        REQUIRE(::mprotect(
            mapping_bytes + page_bytes,
            page_bytes,
            PROT_NONE) == 0);

        const std::span<const std::byte> protected_image{
            header_begin,
            static_cast<std::size_t>(fixture.header.file_bytes)};
        const auto opened = idx::open_mpf5_v2_image_bounded(
            protected_image, validation_options(fixture));
        REQUIRE(opened);
        REQUIRE(opened->header_crc_bytes == idx::kMpf5V2HeaderBytes);
        REQUIRE(opened->payload_crc_bytes == 0u);
        REQUIRE(opened->directory_crc_bytes == 0u);
        REQUIRE(opened->manifest_crc_bytes == 0u);
        REQUIRE(opened->compact_h32_direct.size()
            == fixture.header.compact_h32_direct.bytes);
    }

    SECTION("header identity and receipt mismatches remain fatal") {
        auto wrong = validation_options(fixture);
        ++wrong.expected_identity.header_crc64;
        const auto opened = idx::open_mpf5_v2_image_bounded(
            fixture.image, wrong);
        REQUIRE_FALSE(opened);
        REQUIRE(opened.error() == idx::Mpf5V2WireError::identity_mismatch);
    }
}

TEST_CASE(
        "MPF5 v2 decoder rejects every fixed-code reserved and header CRC violation",
        "[doc0383][mpf5-v2][wire][negative]") {
    const auto valid = golden_header_bytes();

    SECTION("truncated header") {
        REQUIRE(decoded_error(std::span<const std::byte>{valid}.first(4095u))
            == idx::Mpf5V2WireError::header_size_mismatch);
    }
    SECTION("oversized standalone header") {
        std::array<std::byte, 4097> oversized{};
        std::copy(valid.begin(), valid.end(), oversized.begin());
        REQUIRE(decoded_error(oversized)
            == idx::Mpf5V2WireError::header_size_mismatch);
    }
    SECTION("magic") {
        auto bytes = valid;
        bytes[0] ^= std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes) == idx::Mpf5V2WireError::bad_magic);
    }
    SECTION("version") {
        auto bytes = valid;
        reference_store_le(std::span<std::byte>{bytes}, 8u, std::uint32_t{3u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes) == idx::Mpf5V2WireError::bad_version);
    }
    SECTION("encoded header size") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes}, 12u, std::uint32_t{2048u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::bad_header_bytes);
    }
    SECTION("index kind") {
        auto bytes = valid;
        bytes[34] = std::byte{2};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::bad_index_kind);
    }
    SECTION("provider") {
        auto bytes = valid;
        bytes[35] = std::byte{2};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes) == idx::Mpf5V2WireError::bad_provider);
    }
    SECTION("wire format") {
        auto bytes = valid;
        reference_store_le(std::span<std::byte>{bytes}, 36u, std::uint16_t{6u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::bad_wire_format);
    }
    SECTION("reserved bytes 38..39") {
        auto bytes = valid;
        bytes[39] = std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::nonzero_reserved);
    }
    SECTION("reserved bytes 100..103") {
        auto bytes = valid;
        bytes[101] = std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::nonzero_reserved);
    }
    SECTION("reserved bytes 224..4095") {
        auto bytes = valid;
        bytes[4095] = std::byte{1};
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::nonzero_reserved);
    }
    SECTION("unknown feature bit") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes},
            192u,
            idx::kMpf5V2KnownFeatures | (std::uint64_t{1u} << 63u));
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::unsupported_features);
    }
    SECTION("direct receipt without feature") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes}, 200u, std::uint64_t{4160u});
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::
                compact_h32_direct_feature_mismatch);
    }
    SECTION("direct feature without nonempty section") {
        auto bytes = valid;
        reference_store_le(
            std::span<std::byte>{bytes},
            192u,
            idx::kMpf5V2FeatureCompactH32Direct);
        reference_reseal_header(bytes);
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::
                compact_h32_direct_feature_mismatch);
    }
    SECTION("header CRC") {
        auto bytes = valid;
        bytes[80] ^= std::byte{1};
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::header_crc_mismatch);
    }
    SECTION("checksum field is treated as zero") {
        auto bytes = valid;
        bytes[idx::kMpf5V2HeaderCrcOffset] ^= std::byte{1};
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::header_crc_mismatch);
        REQUIRE(reference_header_crc(bytes) == reference_header_crc(valid));
    }
    SECTION("direct-header receipt is protected by outer header CRC") {
        const auto fixture = make_image_fixture(
            idx::Mpf5V2Provider::mapped_file, true);
        auto bytes = idx::EncodedMpf5HeaderV2{};
        std::copy_n(fixture.image.begin(), bytes.size(), bytes.begin());
        bytes[216] ^= std::byte{1};
        REQUIRE(decoded_error(bytes)
            == idx::Mpf5V2WireError::header_crc_mismatch);
    }
}

TEST_CASE(
        "MPF5 v2 encoder refuses invalid fixed codes and geometry",
        "[doc0383][mpf5-v2][wire][encode][negative]") {
    SECTION("magic") {
        auto header = golden_header();
        ++header.magic;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::bad_magic);
    }
    SECTION("version") {
        auto header = golden_header();
        ++header.version;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::bad_version);
    }
    SECTION("header bytes") {
        auto header = golden_header();
        --header.header_bytes;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::bad_header_bytes);
    }
    SECTION("index kind") {
        auto header = golden_header();
        ++header.index_kind;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::bad_index_kind);
    }
    SECTION("provider") {
        auto header = golden_header();
        header.provider = 2u;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::bad_provider);
    }
    SECTION("wire format") {
        auto header = golden_header();
        ++header.wire_format;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::bad_wire_format);
    }
    SECTION("geometry") {
        auto header = golden_header();
        ++header.directory.offset;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_offset_mismatch);
    }
    SECTION("unknown feature") {
        auto header = golden_header();
        header.feature_bitmap = std::uint64_t{1u} << 63u;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::unsupported_features);
    }
    SECTION("direct receipt without feature") {
        auto header = golden_header();
        header.compact_h32_direct =
            idx::Mpf5V2CompactH32DirectSectionReceipt{
                .offset = 4160u,
                .bytes = 256u,
                .direct_header_crc64 = 7u,
            };
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_feature_mismatch);
    }
    SECTION("direct feature without nonempty section") {
        auto header = golden_header();
        header.feature_bitmap = idx::kMpf5V2FeatureCompactH32Direct;
        const auto result = idx::encode_mpf5_v2_header(header);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_feature_mismatch);
    }
}

TEST_CASE(
        "MPF5 v2 layout planning is exact aligned and overflow-checked",
        "[doc0383][mpf5-v2][layout]") {
    const auto layout = idx::plan_mpf5_v2_layout(
        3u, 5u, 7u, 11u, 13u, 17u);
    REQUIRE(layout);
    REQUIRE((layout->payload == idx::ImageSectionV2{4096u, 3u, 11u}));
    REQUIRE((layout->directory == idx::ImageSectionV2{4104u, 5u, 13u}));
    REQUIRE((layout->manifest == idx::ImageSectionV2{4112u, 7u, 17u}));
    REQUIRE(layout->feature_bitmap == 0u);
    REQUIRE(layout->compact_h32_direct
        == idx::Mpf5V2CompactH32DirectSectionReceipt{});
    REQUIRE(layout->file_bytes == 4119u);

    const auto already_aligned = idx::plan_mpf5_v2_layout(8u, 16u, 24u);
    REQUIRE(already_aligned);
    REQUIRE(already_aligned->directory.offset == 4104u);
    REQUIRE(already_aligned->manifest.offset == 4120u);
    REQUIRE(already_aligned->file_bytes == 4144u);

    const auto with_direct = idx::plan_mpf5_v2_layout(
        3u,
        5u,
        7u,
        11u,
        13u,
        17u,
        idx::Mpf5V2CompactH32DirectSectionSpec{
            .bytes = 9u,
            .direct_header_crc64 = 23u,
        });
    REQUIRE(with_direct);
    REQUIRE(with_direct->payload == layout->payload);
    REQUIRE(with_direct->directory == layout->directory);
    REQUIRE(with_direct->manifest == layout->manifest);
    REQUIRE(with_direct->feature_bitmap
        == idx::kMpf5V2FeatureCompactH32Direct);
    REQUIRE((with_direct->compact_h32_direct
        == idx::Mpf5V2CompactH32DirectSectionReceipt{
            .offset = 4160u,
            .bytes = 9u,
            .direct_header_crc64 = 23u,
        }));
    REQUIRE(with_direct->file_bytes == 4169u);

    const auto empty_direct = idx::plan_mpf5_v2_layout(
        3u,
        5u,
        7u,
        0u,
        0u,
        0u,
        idx::Mpf5V2CompactH32DirectSectionSpec{});
    REQUIRE_FALSE(empty_direct);
    REQUIRE(empty_direct.error()
        == idx::Mpf5V2WireError::compact_h32_direct_feature_mismatch);

    const auto payload_add_overflow = idx::plan_mpf5_v2_layout(
        std::numeric_limits<std::uint64_t>::max(), 0u, 0u);
    REQUIRE_FALSE(payload_add_overflow);
    REQUIRE(payload_add_overflow.error()
        == idx::Mpf5V2WireError::arithmetic_overflow);

    const auto payload_align_overflow = idx::plan_mpf5_v2_layout(
        std::numeric_limits<std::uint64_t>::max() - 4096u, 0u, 0u);
    REQUIRE_FALSE(payload_align_overflow);
    REQUIRE(payload_align_overflow.error()
        == idx::Mpf5V2WireError::arithmetic_overflow);

    const auto directory_align_overflow = idx::plan_mpf5_v2_layout(
        0u, std::numeric_limits<std::uint64_t>::max() - 4096u, 0u);
    REQUIRE_FALSE(directory_align_overflow);
    REQUIRE(directory_align_overflow.error()
        == idx::Mpf5V2WireError::arithmetic_overflow);

    const auto manifest_add_overflow = idx::plan_mpf5_v2_layout(
        0u, 0u, std::numeric_limits<std::uint64_t>::max() - 4095u);
    REQUIRE_FALSE(manifest_add_overflow);
    REQUIRE(manifest_add_overflow.error()
        == idx::Mpf5V2WireError::arithmetic_overflow);

    const auto direct_align_overflow = idx::plan_mpf5_v2_layout(
        0u,
        0u,
        std::numeric_limits<std::uint64_t>::max() - 4127u,
        0u,
        0u,
        0u,
        idx::Mpf5V2CompactH32DirectSectionSpec{
            .bytes = 1u,
            .direct_header_crc64 = 1u,
        });
    REQUIRE_FALSE(direct_align_overflow);
    REQUIRE(direct_align_overflow.error()
        == idx::Mpf5V2WireError::arithmetic_overflow);

    const auto direct_add_overflow = idx::plan_mpf5_v2_layout(
        0u,
        0u,
        0u,
        0u,
        0u,
        0u,
        idx::Mpf5V2CompactH32DirectSectionSpec{
            .bytes = std::numeric_limits<std::uint64_t>::max(),
            .direct_header_crc64 = 1u,
        });
    REQUIRE_FALSE(direct_add_overflow);
    REQUIRE(direct_add_overflow.error()
        == idx::Mpf5V2WireError::arithmetic_overflow);
}

TEST_CASE(
        "MPF5 v2 layout validator rejects size limits overlap gaps and arithmetic overflow",
        "[doc0383][mpf5-v2][layout][negative]") {
    const auto original = golden_header();

    SECTION("truncated file") {
        const auto result = idx::validate_mpf5_v2_layout(original, 4095u);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::file_too_small);
    }
    SECTION("wrong exact file size") {
        const auto result = idx::validate_mpf5_v2_layout(original, 4120u);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::file_size_mismatch);
    }
    SECTION("off_t bound") {
        const auto result = idx::validate_mpf5_v2_layout(
            original,
            original.file_bytes,
            idx::Mpf5V2AddressLimits{
                .max_file_offset = original.file_bytes - 1u,
                .max_mapped_bytes = original.file_bytes,
            });
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::file_offset_not_representable);
    }
    SECTION("size_t bound") {
        const auto result = idx::validate_mpf5_v2_layout(
            original,
            original.file_bytes,
            idx::Mpf5V2AddressLimits{
                .max_file_offset = original.file_bytes,
                .max_mapped_bytes = original.file_bytes - 1u,
            });
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::mapped_size_not_representable);
    }
    SECTION("payload must begin immediately after header") {
        auto header = original;
        header.payload.offset = 4104u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::payload_offset_mismatch);
    }
    SECTION("payload end overflows") {
        auto header = original;
        header.payload.bytes = std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
    SECTION("directory overlap") {
        auto header = original;
        header.directory.offset = 4098u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_offset_mismatch);
    }
    SECTION("directory unproved gap") {
        auto header = original;
        header.directory.offset += 8u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_offset_mismatch);
    }
    SECTION("directory end overflows") {
        auto header = original;
        header.directory.bytes = std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
    SECTION("manifest overlap") {
        auto header = original;
        header.manifest.offset = header.directory.offset;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::manifest_offset_mismatch);
    }
    SECTION("manifest unproved gap") {
        auto header = original;
        header.manifest.offset += 8u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::manifest_offset_mismatch);
    }
    SECTION("manifest end overflows") {
        auto header = original;
        header.manifest.bytes = std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
    SECTION("manifest must end at exact file size") {
        auto header = original;
        --header.manifest.bytes;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::manifest_end_mismatch);
    }
}

TEST_CASE(
        "MPF5 v2 direct layout rejects feature overlap gap overflow and out-of-file extents",
        "[doc0383][mpf5-v2][layout][direct][negative]") {
    const auto original = direct_golden_header();
    REQUIRE(original.compact_h32_direct.offset
            % idx::kMpf5V2CompactH32DirectAlignment
        == 0u);
    REQUIRE(idx::validate_mpf5_v2_layout(
        original, original.file_bytes));

    SECTION("unknown feature") {
        auto header = original;
        header.feature_bitmap |= std::uint64_t{1u} << 63u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::unsupported_features);
    }
    SECTION("receipt without feature") {
        auto header = original;
        header.feature_bitmap = 0u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_feature_mismatch);
    }
    SECTION("feature with empty section") {
        auto header = original;
        header.compact_h32_direct.bytes = 0u;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_feature_mismatch);
    }
    SECTION("direct section overlaps manifest") {
        auto header = original;
        header.compact_h32_direct.offset = header.manifest.offset;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_offset_mismatch);
    }
    SECTION("direct section has an unproved aligned gap") {
        auto header = original;
        header.compact_h32_direct.offset +=
            idx::kMpf5V2CompactH32DirectAlignment;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_offset_mismatch);
    }
    SECTION("direct end is outside exact file extent") {
        auto header = original;
        ++header.compact_h32_direct.bytes;
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::compact_h32_direct_end_mismatch);
    }
    SECTION("direct end overflows") {
        auto header = original;
        header.compact_h32_direct.bytes =
            std::numeric_limits<std::uint64_t>::max();
        const auto result = idx::validate_mpf5_v2_layout(
            header, header.file_bytes);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
}

TEST_CASE(
        "MPF5 v2 image validation authenticates bounded metadata and defers payload CRC",
        "[doc0383][mpf5-v2][image][crc]") {
    for (const auto provider : {
             idx::Mpf5V2Provider::postgres_relation,
             idx::Mpf5V2Provider::mapped_file}) {
        auto fixture = make_image_fixture(provider);
        const auto identity = expected_identity(fixture.header);
        const idx::Mpf5V2ValidationOptions normal_options{
            .expected_identity = identity,
            .payload_crc_policy = idx::Mpf5V2PayloadCrcPolicy::deferred,
        };
        const auto normal = idx::validate_mpf5_v2_image(
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
        REQUIRE(normal->header_crc_bytes == idx::kMpf5V2HeaderBytes);
        REQUIRE(normal->payload_crc_bytes == 0u);
        REQUIRE(normal->directory_crc_bytes == fixture.directory.size());
        REQUIRE(normal->manifest_crc_bytes == fixture.manifest.size());
        REQUIRE(normal->compact_h32_direct.empty());

        const idx::Mpf5V2ValidationOptions scrub_options{
            .expected_identity = identity,
            .payload_crc_policy = idx::Mpf5V2PayloadCrcPolicy::validate,
        };
        const auto scrub = idx::validate_mpf5_v2_image(
            fixture.image, scrub_options);
        REQUIRE(scrub);
        REQUIRE(scrub->payload_crc_bytes == fixture.payload.size());

        fixture.image[static_cast<std::size_t>(fixture.header.payload.offset)]
            ^= std::byte{1};
        // Normal open deliberately does not scan the payload aggregate.
        REQUIRE(idx::validate_mpf5_v2_image(
            fixture.image, normal_options));
        const auto scrubbed = idx::validate_mpf5_v2_image(
            fixture.image, scrub_options);
        REQUIRE_FALSE(scrubbed);
        REQUIRE(scrubbed.error()
            == idx::Mpf5V2WireError::payload_crc_mismatch);
    }
}

TEST_CASE(
        "MPF5 v2 image validation rejects metadata CRC padding identity and size corruption",
        "[doc0383][mpf5-v2][image][negative]") {
    SECTION("directory CRC") {
        auto fixture = make_image_fixture();
        fixture.image[static_cast<std::size_t>(fixture.header.directory.offset)]
            ^= std::byte{1};
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_crc_mismatch);
    }
    SECTION("manifest CRC") {
        auto fixture = make_image_fixture();
        fixture.image[static_cast<std::size_t>(fixture.header.manifest.offset)]
            ^= std::byte{1};
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::manifest_crc_mismatch);
    }
    SECTION("payload-to-directory alignment padding") {
        auto fixture = make_image_fixture();
        const auto padding_offset = fixture.header.payload.offset
            + fixture.header.payload.bytes;
        REQUIRE(padding_offset < fixture.header.directory.offset);
        fixture.image[static_cast<std::size_t>(padding_offset)] = std::byte{1};
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::nonzero_section_padding);
    }
    SECTION("manifest-to-direct alignment padding") {
        auto fixture = make_image_fixture(
            idx::Mpf5V2Provider::mapped_file, true);
        const auto padding_offset = fixture.header.manifest.offset
            + fixture.header.manifest.bytes;
        REQUIRE(padding_offset
            < fixture.header.compact_h32_direct.offset);
        fixture.image[static_cast<std::size_t>(padding_offset)] =
            std::byte{1};
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::nonzero_section_padding);
    }
    // A valid Format-5 directory is a whole number of fixed 128-byte entries,
    // so its end is necessarily 8-byte aligned.  Consequently the
    // directory-to-manifest seam is empty; the two reachable non-empty seams
    // are exercised above.
    STATIC_REQUIRE(
        idx::MinimizerBucketDirEntry::format5_encoded_bytes
            % idx::kMpf5V2SectionAlignment
        == 0u);
    SECTION("identity") {
        auto fixture = make_image_fixture();
        auto identity = expected_identity(fixture.header);
        ++identity.generation_id;
        const idx::Mpf5V2ValidationOptions options{
            .expected_identity = identity,
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::identity_mismatch);
    }
    SECTION("provider identity") {
        auto fixture = make_image_fixture();
        auto identity = expected_identity(fixture.header);
        identity.provider = static_cast<std::uint8_t>(
            idx::Mpf5V2Provider::postgres_relation);
        const idx::Mpf5V2ValidationOptions options{
            .expected_identity = identity,
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::identity_mismatch);
    }
    SECTION("nested direct-header receipt mismatch after valid outer reseal") {
        auto fixture = make_image_fixture(
            idx::Mpf5V2Provider::mapped_file, true);
        fixture.image[216] ^= std::byte{1};
        reference_reseal_header(fixture.image);
        const auto decoded = idx::decode_mpf5_v2_header(
            std::span<const std::byte>{fixture.image}.first(
                idx::kMpf5V2HeaderBytes));
        REQUIRE(decoded);
        auto identity = expected_identity(fixture.header);
        // Keep the outer receipt current so this section isolates the nested
        // direct-header receipt pin from the outer MPF5 header CRC check.
        identity.header_crc64 = decoded->header_crc64;
        const idx::Mpf5V2ValidationOptions options{
            .expected_identity = identity,
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::identity_mismatch);
    }
    SECTION("file truncation") {
        auto fixture = make_image_fixture();
        fixture.image.pop_back();
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::file_size_mismatch);
    }
    SECTION("header truncation") {
        std::vector<std::byte> image(4095u, std::byte{0});
        const auto fixture = make_image_fixture();
        const auto result = idx::validate_mpf5_v2_image(
            image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::file_too_small);
    }
    SECTION("file oversize") {
        auto fixture = make_image_fixture();
        fixture.image.push_back(std::byte{0});
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::file_size_mismatch);
    }
    SECTION("injected off_t oversize") {
        auto fixture = make_image_fixture();
        const idx::Mpf5V2ValidationOptions options{
            .address_limits = idx::Mpf5V2AddressLimits{
                .max_file_offset = fixture.header.file_bytes - 1u,
                .max_mapped_bytes = fixture.header.file_bytes,
            },
            .expected_identity = expected_identity(fixture.header),
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::file_offset_not_representable);
    }
    SECTION("injected size_t oversize") {
        auto fixture = make_image_fixture();
        const idx::Mpf5V2ValidationOptions options{
            .address_limits = idx::Mpf5V2AddressLimits{
                .max_file_offset = fixture.header.file_bytes,
                .max_mapped_bytes = fixture.header.file_bytes - 1u,
            },
            .expected_identity = expected_identity(fixture.header),
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::mapped_size_not_representable);
    }
}

TEST_CASE(
        "MPF5 v2 image rejects corrupt geometry before constructing section spans",
        "[doc0383][mpf5-v2][image][overflow]") {
    SECTION("overlapping directory") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            136u,
            fixture.header.payload.offset + 1u);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_offset_mismatch);
    }
    SECTION("payload end overflow") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            120u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
    SECTION("directory end overflow") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            144u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
    SECTION("manifest end overflow") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            168u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
    SECTION("unproved directory gap") {
        auto fixture = make_image_fixture();
        reference_store_le(
            std::span<std::byte>{fixture.image},
            136u,
            fixture.header.directory.offset + 8u);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_offset_mismatch);
    }
    SECTION("direct section overlaps manifest") {
        auto fixture = make_image_fixture(
            idx::Mpf5V2Provider::mapped_file, true);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            200u,
            fixture.header.manifest.offset);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::
                compact_h32_direct_offset_mismatch);
    }
    SECTION("direct section ends outside file") {
        auto fixture = make_image_fixture(
            idx::Mpf5V2Provider::mapped_file, true);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            208u,
            fixture.header.compact_h32_direct.bytes + 1u);
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::compact_h32_direct_end_mismatch);
    }
    SECTION("direct section end overflows") {
        auto fixture = make_image_fixture(
            idx::Mpf5V2Provider::mapped_file, true);
        reference_store_le(
            std::span<std::byte>{fixture.image},
            208u,
            std::numeric_limits<std::uint64_t>::max());
        reference_reseal_header(fixture.image);
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, validation_options(fixture));
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::arithmetic_overflow);
    }
}

TEST_CASE(
        "MPF5 v2 open-ready validation requires every authority field",
        "[doc0383][mpf5-v2][image][identity][negative]") {
    const auto fixture = make_image_fixture();
    const auto base = expected_identity(fixture.header);
    std::array<idx::Mpf5V2ExpectedIdentity, 23u> candidates{};
    candidates.fill(base);
    ++candidates[0].database_oid;
    ++candidates[1].tablespace_oid;
    ++candidates[2].main_relfilenode;
    ++candidates[3].user_relid;
    ++candidates[4].column_attno;
    ++candidates[5].index_kind;
    candidates[6].provider = static_cast<std::uint8_t>(
        idx::Mpf5V2Provider::postgres_relation);
    ++candidates[7].wire_format;
    ++candidates[8].query_key;
    ++candidates[9].generation_id;
    ++candidates[10].source_identity;
    ++candidates[11].source_content_crc;
    ++candidates[12].compatibility_crc;
    ++candidates[13].target_length;
    ++candidates[14].file_bytes;
    ++candidates[15].position_segment_count;
    ++candidates[16].position_segment_span_bp;
    ++candidates[17].payload.crc64;
    ++candidates[18].directory.crc64;
    ++candidates[19].manifest.crc64;
    ++candidates[20].header_crc64;
    candidates[21].feature_bitmap =
        idx::kMpf5V2FeatureCompactH32Direct;
    candidates[22].compact_h32_direct =
        idx::Mpf5V2CompactH32DirectSectionReceipt{
            .offset = 4160u,
            .bytes = 256u,
            .direct_header_crc64 = 1u,
        };
    constexpr std::array<std::string_view, 23u> labels{
        "database", "tablespace", "relfilenode", "user relation",
        "column", "index kind", "provider", "wire format", "query key",
        "generation", "source identity", "source content",
        "compatibility", "target length", "file bytes", "segment count",
        "segment span", "payload receipt", "directory receipt",
        "manifest receipt", "header receipt", "feature bitmap",
        "direct-section receipt",
    };
    for (std::size_t index = 0u; index < candidates.size(); ++index) {
        INFO("authority field=" << labels[index]);
        const idx::Mpf5V2ValidationOptions options{
            .expected_identity = candidates[index],
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == idx::Mpf5V2WireError::identity_mismatch);
    }
}

TEST_CASE(
        "MPF5 v2 cross-checks decoded Format-5 manifest and directory metadata",
        "[doc0383][mpf5-v2][image][metadata][negative]") {
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
        const auto header = idx::decode_mpf5_v2_header(
            std::span<const std::byte>{fixture.image}.first(
                idx::kMpf5V2HeaderBytes));
        REQUIRE(header);
        const idx::Mpf5V2ValidationOptions options{
            .expected_identity = expected_identity(*header),
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::generation_metadata_mismatch);
    }

    SECTION("directory structure is invalid under self-consistent CRCs") {
        auto fixture = make_image_fixture();
        const std::size_t directory_offset = static_cast<std::size_t>(
            fixture.header.directory.offset);
        const std::size_t manifest_offset = static_cast<std::size_t>(
            fixture.header.manifest.offset);
        // Partition id 1 is invalid for the first/only directory entry.
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
        // Keep the manifest's duplicate directory receipt consistent so the
        // structural directory decoder is the rejecting authority.
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
        const auto header = idx::decode_mpf5_v2_header(
            std::span<const std::byte>{fixture.image}.first(
                idx::kMpf5V2HeaderBytes));
        REQUIRE(header);
        const idx::Mpf5V2ValidationOptions options{
            .expected_identity = expected_identity(*header),
        };
        const auto result = idx::validate_mpf5_v2_image(
            fixture.image, options);
        REQUIRE_FALSE(result);
        REQUIRE(result.error()
            == idx::Mpf5V2WireError::directory_metadata_invalid);
    }
}

TEST_CASE(
        "MPF5 v2 rejects a header-only image without Format-5 metadata",
        "[doc0383][mpf5-v2][image][empty][negative]") {
    const auto layout = idx::plan_mpf5_v2_layout(0u, 0u, 0u);
    REQUIRE(layout);
    REQUIRE(layout->file_bytes == idx::kMpf5V2HeaderBytes);

    auto header = golden_header();
    header.payload = layout->payload;
    header.directory = layout->directory;
    header.manifest = layout->manifest;
    header.file_bytes = layout->file_bytes;
    const auto encoded = idx::encode_mpf5_v2_header(header);
    REQUIRE(encoded);
    const auto decoded = idx::decode_mpf5_v2_header(*encoded);
    REQUIRE(decoded);

    const idx::Mpf5V2ValidationOptions options{
        .expected_identity = expected_identity(*decoded),
        .payload_crc_policy = idx::Mpf5V2PayloadCrcPolicy::validate,
    };
    const auto image = idx::validate_mpf5_v2_image(*encoded, options);
    REQUIRE_FALSE(image);
    REQUIRE(image.error()
        == idx::Mpf5V2WireError::manifest_metadata_invalid);
}
