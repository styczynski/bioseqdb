/**
 * @file persisted_minimizer_image_test.cpp
 * @brief Tests byte-exact minimizer-fragment image planning and validation.
 */

#include <tests/support/common.hpp>

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
import bioseq.indexing.minimizer_fragment_image;

namespace {

namespace idx = bioseq::indexing;

/// Encode one unsigned integer in little-endian order for wire fixtures.
template <class p_uint>
requires (std::is_unsigned_v<p_uint>)
constexpr void reference_store_le(
        std::span<std::byte> bytes,
        std::size_t offset,
        p_uint value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(p_uint); ++byte) {
        bytes[offset + byte] = static_cast<std::byte>(value & p_uint{0xffu});
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

/// Compute the header checksum with its checksum field cleared.
[[nodiscard]] constexpr std::uint64_t reference_header_crc(
        idx::encoded_minimizer_fragment_image_header header) noexcept {
    std::fill(
        header.begin() + static_cast<std::ptrdiff_t>(
            idx::k_minimizer_fragment_image_header_crc_offset),
        header.begin() + static_cast<std::ptrdiff_t>(
            idx::k_minimizer_fragment_image_header_crc_offset + idx::k_minimizer_fragment_image_header_crc_bytes),
        std::byte{0});
    return reference_crc64_ecma182(header);
}

/// Rewrite the checksum after mutating a header fixture.
void reference_reseal_header(std::span<std::byte> image) {
    auto header = idx::encoded_minimizer_fragment_image_header{};
    std::copy_n(image.begin(), header.size(), header.begin());
    reference_store_le(
        image,
        idx::k_minimizer_fragment_image_header_crc_offset,
        reference_header_crc(header));
}

/// Construct the byte-pinned minimizer-fragment header fixture.
[[nodiscard]] idx::minimizer_fragment_image_header golden_header() {
    return idx::minimizer_fragment_image_header{
        .magic = idx::k_minimizer_fragment_image_magic,
        .layout_revision = idx::k_minimizer_fragment_image_layout_revision,
        .header_bytes = idx::k_minimizer_fragment_image_header_bytes,
        .database_oid = 0x13121110u,
        .tablespace_oid = 0x17161514u,
        .main_relfilenode = 0x1b1a1918u,
        .user_relid = 0x1f1e1d1cu,
        .column_attno = 0x2120u,
        .index_identity = idx::k_minimizer_fragment_image_index_identity,
        .storage_kind =
            idx::k_minimizer_fragment_image_relation_backed_storage_kind,
        .layout_identity = idx::k_minimizer_fragment_image_layout_identity,
        .query_key = 0x4746454443424140ULL,
        .generation_id = 0x4f4e4d4c4b4a4948ULL,
        .source_identity = 0x5756555453525150ULL,
        .source_content_crc = 0x5f5e5d5c5b5a5958ULL,
        .storage_contract_checksum = 0x6766656463626160ULL,
        .target_length = 0x6f6e6d6c6b6a6968ULL,
        .file_bytes = 4119u,
        .position_segment_count = 0x73727170u,
        .position_segment_span_bp = 0x7f7e7d7c7b7a7978ULL,
        .payload = idx::minimizer_fragment_image_section{
            .offset = 4096u,
            .bytes = 3u,
            .crc64 = 0x8786858483828180ULL,
        },
        .directory = idx::minimizer_fragment_image_section{
            .offset = 4104u,
            .bytes = 5u,
            .crc64 = 0x8f8e8d8c8b8a8988ULL,
        },
        .manifest = idx::minimizer_fragment_image_section{
            .offset = 4112u,
            .bytes = 7u,
            .crc64 = 0x9796959493929190ULL,
        },
        // Deliberately nonzero: the encoder must ignore this convenience
        // value and checksum the wire field as zero.
        .header_crc64 = std::numeric_limits<std::uint64_t>::max(),
    };
}

/// Extend the header fixture with a compact-hash32 direct section.
[[nodiscard]] idx::minimizer_fragment_image_header direct_golden_header() {
    auto header = golden_header();
    const auto layout = idx::plan_minimizer_fragment_image_layout(
        header.payload.bytes,
        header.directory.bytes,
        header.manifest.bytes,
        header.payload.crc64,
        header.directory.crc64,
        header.manifest.crc64,
        idx::minimizer_fragment_compact_hash32_section_spec{
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
    header.compact_hash32_direct = layout->compact_hash32_direct;
    header.file_bytes = layout->file_bytes;
    return header;
}

/// Encode the byte-pinned header through the independent reference writer.
[[nodiscard]] idx::encoded_minimizer_fragment_image_header golden_header_bytes() {
    const auto header = golden_header();
    idx::encoded_minimizer_fragment_image_header expected{};
    const std::span<std::byte> bytes{expected};
    reference_store_le(bytes, 0u, header.magic);
    reference_store_le(bytes, 8u, header.layout_revision);
    reference_store_le(bytes, 12u, header.header_bytes);
    reference_store_le(bytes, 16u, header.database_oid);
    reference_store_le(bytes, 20u, header.tablespace_oid);
    reference_store_le(bytes, 24u, header.main_relfilenode);
    reference_store_le(bytes, 28u, header.user_relid);
    reference_store_le(bytes, 32u, header.column_attno);
    reference_store_le(bytes, 34u, header.index_identity);
    reference_store_le(bytes, 35u, header.storage_kind);
    reference_store_le(bytes, 36u, header.layout_identity);
    reference_store_le(bytes, 40u, header.query_key);
    reference_store_le(bytes, 48u, header.generation_id);
    reference_store_le(bytes, 56u, header.source_identity);
    reference_store_le(bytes, 64u, header.source_content_crc);
    reference_store_le(bytes, 72u, header.storage_contract_checksum);
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
        idx::k_minimizer_fragment_image_header_crc_offset,
        reference_header_crc(expected));
    return expected;
}

/// Derive the exact identity expected by the image validator.
[[nodiscard]] idx::expected_minimizer_fragment_image_identity expected_identity(
        const idx::minimizer_fragment_image_header& header) noexcept {
    return idx::expected_minimizer_fragment_image_identity{
        .database_oid = header.database_oid,
        .tablespace_oid = header.tablespace_oid,
        .main_relfilenode = header.main_relfilenode,
        .user_relid = header.user_relid,
        .column_attno = header.column_attno,
        .index_identity = header.index_identity,
        .layout_identity = header.layout_identity,
        .query_key = header.query_key,
        .generation_id = header.generation_id,
        .source_identity = header.source_identity,
        .source_content_crc = header.source_content_crc,
        .storage_contract_checksum = header.storage_contract_checksum,
        .target_length = header.target_length,
        .file_bytes = header.file_bytes,
        .position_segment_count = header.position_segment_count,
        .position_segment_span_bp = header.position_segment_span_bp,
        .payload = header.payload,
        .directory = header.directory,
        .manifest = header.manifest,
        .header_crc64 = header.header_crc64,
        .feature_bitmap = header.feature_bitmap,
        .compact_hash32_direct = header.compact_hash32_direct,
    };
}

/// Own all sections and the assembled bytes of one image fixture.
struct image_fixture {
    idx::minimizer_fragment_image_header header{};
    std::vector<std::byte> payload{};
    std::vector<std::byte> directory{};
    std::vector<std::byte> manifest{};
    std::vector<std::byte> compact_hash32_direct{};
    std::vector<std::byte> image{};
};

/// Build a valid grouped-SoA image with an optional opaque direct section.
[[nodiscard]] image_fixture make_image_fixture(
        bool include_compact_hash32_direct = false) {
    constexpr std::uint64_t target_length = 100u;
    constexpr std::uint64_t segment_span = 32'768u;
    constexpr std::uint64_t max_occ = 10u;
    const auto profile = idx::derive_grouped_soa_minimizer_profile(
        idx::k_minimap_scheme_identity,
        idx::k_minimizer_persisted_dna4_alphabet_code,
        15u,
        segment_span,
        max_occ);
    if (!profile) {
        std::terminate();
    }
    constexpr idx::minimizer_scalar_width offset_width =
        idx::minimizer_scalar_width::u16;
    const auto control_layout = idx::plan_grouped_soa_control_plane(
        1u,
        profile->hash_width,
        offset_width,
        profile->count_width);
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

    const idx::grouped_soa_bucket_directory_entry entry{
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

    image_fixture fixture{};
    fixture.header = golden_header();
    fixture.payload.reserve(controls.size() + positions.size());
    fixture.payload.insert(
        fixture.payload.end(), controls.begin(), controls.end());
    fixture.payload.insert(
        fixture.payload.end(), positions.begin(), positions.end());
    fixture.directory = *encoded_directory;

    const std::uint64_t raw_payload_crc =
        idx::crc64_ecma182(fixture.payload);
    const idx::minimizer_generation_manifest generation_manifest{
        .layout = idx::minimizer_generation_layout::
            grouped_soa_segment_local,
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
        .alphabet_code = idx::k_minimizer_persisted_dna4_alphabet_code,
        .storage_unit_byte_width = 8u,
        .kmer_size = 15u,
        .window_size = 10u,
        .gpu_abi_revision = profile->gpu_abi_revision,
        .position_segment_span_bp = segment_span,
        .segment_geometry_revision = idx::minimizer_generation_manifest::
            uniform_segment_geometry_revision,
        .minimizer_scheme_id = idx::k_minimap_scheme_id,
        .minimizer_scheme_revision = idx::k_minimap_scheme_revision,
        .producer_backend_id = idx::minimizer_producer_backend::scalar,
        .producer_identity_revision =
            idx::k_minimizer_producer_identity_revision,
        .hash_width = profile->hash_width,
        .position_width = profile->position_width,
        .count_width = profile->count_width,
        .offset_policy = profile->offset_policy,
        .control_layout = profile->control_layout,
        .count_semantics = profile->count_semantics,
        .position_semantics = profile->position_semantics,
        .max_hash = profile->max_hash,
        .max_occ_cap = profile->max_occ_cap,
    };
    const auto encoded_manifest =
        idx::encode_minimizer_generation_manifest(
            generation_manifest);
    if (!encoded_manifest) {
        std::terminate();
    }
    fixture.manifest.assign(
        encoded_manifest->begin(), encoded_manifest->end());

    std::optional<idx::minimizer_fragment_compact_hash32_section_spec> direct_spec;
    if (include_compact_hash32_direct) {
        // The fragment-image codec treats this section as opaque. The first
        // 256 bytes
        // stand in for the nested direct header and the remainder for nested
        // payload; only the direct codec interprets either one.
        constexpr std::size_t opaque_direct_header_bytes = 256u;
        fixture.compact_hash32_direct.resize(
            opaque_direct_header_bytes + 37u, std::byte{0});
        for (std::size_t byte = 0u;
             byte < fixture.compact_hash32_direct.size(); ++byte) {
            fixture.compact_hash32_direct[byte] = static_cast<std::byte>(
                (byte * 17u + 3u) & 0xffu);
        }
        direct_spec = idx::minimizer_fragment_compact_hash32_section_spec{
            .bytes = fixture.compact_hash32_direct.size(),
            .direct_header_crc64 = reference_crc64_ecma182(
                std::span<const std::byte>{fixture.compact_hash32_direct}.
                    first(opaque_direct_header_bytes)),
        };
    }

    const auto layout = idx::plan_minimizer_fragment_image_layout(
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
    fixture.header.layout_identity =
        idx::k_minimizer_fragment_image_layout_identity;
    fixture.header.source_identity = generation_manifest.source_identity;
    fixture.header.source_content_crc =
        generation_manifest.source_content_checksum;
    fixture.header.storage_contract_checksum =
        idx::compute_minimizer_generation_storage_contract_checksum(
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
    fixture.header.compact_hash32_direct = layout->compact_hash32_direct;
    fixture.header.file_bytes = layout->file_bytes;
    fixture.header.header_crc64 = 0u;

    const auto encoded = idx::encode_minimizer_fragment_image_header(fixture.header);
    if (!encoded) {
        std::terminate();
    }
    const auto decoded_header = idx::decode_minimizer_fragment_image_header(*encoded);
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
    if (include_compact_hash32_direct) {
        std::copy(
            fixture.compact_hash32_direct.begin(),
            fixture.compact_hash32_direct.end(),
            fixture.image.begin() + static_cast<std::ptrdiff_t>(
                layout->compact_hash32_direct.offset));
    }
    return fixture;
}

/// Construct validation options pinned to a fixture header.
[[nodiscard]] idx::minimizer_fragment_image_validation_options validation_options(
        const image_fixture& fixture,
        idx::minimizer_fragment_payload_crc_policy payload_crc_policy =
            idx::minimizer_fragment_payload_crc_policy::deferred) noexcept {
    return idx::minimizer_fragment_image_validation_options{
        .expected_identity = expected_identity(fixture.header),
        .payload_crc_policy = payload_crc_policy,
    };
}

/// Decode an invalid header and return its exact error.
[[nodiscard]] idx::minimizer_fragment_image_error decoded_error(
        std::span<const std::byte> encoded) {
    const auto result = idx::decode_minimizer_fragment_image_header(encoded);
    if (result) {
        std::terminate();
    }
    return result.error();
}

/// Release one anonymous mapping at scope exit.
struct anonymous_mapping_guard {
    void* address = MAP_FAILED;
    std::size_t bytes = 0u;

    /// Unmap the owned virtual address range when present.
    ~anonymous_mapping_guard() {
        if (address != MAP_FAILED) {
            (void)::munmap(address, bytes);
        }
    }
};

} // namespace

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
    // The fragment image authenticates the nested-header receipt and extent, not the opaque
    // direct payload.  Nested section CRCs remain the direct codec's job.
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
    // Normal open deliberately does not scan the payload aggregate.
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
    // A valid grouped-SoA directory is a whole number of fixed 128-byte entries,
    // so its end is necessarily 8-byte aligned. Consequently the
    // directory-to-manifest gap is empty. The two reachable non-empty gaps
    // are exercised above.
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
        // Keep the outer receipt consistent so this section isolates the nested
        // direct-header receipt pin from the outer fragment-image header CRC
        // check.
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
