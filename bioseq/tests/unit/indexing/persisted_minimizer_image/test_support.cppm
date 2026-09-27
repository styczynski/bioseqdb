/**
 * @file test_support.cppm
 * @brief Builds stored index fixtures and computes reference bytes and checksums.
 */
module;

#include <new>
#include <sys/mman.h>
#include <unistd.h>

export module bioseq.tests.unit.indexing.persisted_minimizer_image_support;

import std;
import bioseq.pipeline.runtime_contract;
import bioseq.common.seqan_adapter;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_fragment_image;

namespace idx = bioseq::indexing;

export namespace bioseq::tests::unit::indexing::persisted_minimizer_image_test_support {

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

/// Compute CRC-64/ECMA-182 one bit at a time for comparison with the codec.
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
            idx::k_minimizer_fragment_image_header_crc_offset
            + idx::k_minimizer_fragment_image_header_crc_bytes),
        std::byte{0});
    return reference_crc64_ecma182(header);
}

/// Update the header checksum after changing fixture bytes.
void reference_reseal_header(std::span<std::byte> image) {
    auto header = idx::encoded_minimizer_fragment_image_header{};
    std::copy_n(image.begin(), header.size(), header.begin());
    reference_store_le(
        image,
        idx::k_minimizer_fragment_image_header_crc_offset,
        reference_header_crc(header));
}

/// Build a header with fixed values for byte-layout tests.
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
        // The encoder must replace this value with a newly computed checksum.
        .header_crc64 = std::numeric_limits<std::uint64_t>::max(),
    };
}

/// Add a compact hash index section to the fixed header fixture.
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

/// Write the expected header bytes with the reference encoder.
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

/// Copy the header fields that image validation must match.
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

/// The section bytes and complete image used by a test.
struct image_fixture {
    idx::minimizer_fragment_image_header header{};
    std::vector<std::byte> payload{};
    std::vector<std::byte> directory{};
    std::vector<std::byte> manifest{};
    std::vector<std::byte> compact_hash32_direct{};
    std::vector<std::byte> image{};
};

/// Build an image for outer-format validation, optionally including
/// placeholder bytes for a nested direct index.
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
    constexpr auto physical_layout =
        bioseq::minimizers::select_minimizer_physical_layout<seq::dna4, std::uint64_t>(
            bioseq::simd::minimizer_algorithm::scalar,
            idx::k_minimizer_fragment_algorithm_position_extent_bp,
            15u,
            10u);
    constexpr auto algorithm_identity =
        idx::make_minimizer_generation_algorithm_identity(physical_layout);
    static_assert(algorithm_identity.has_value());
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
        // The manifest checksum describes row checksums. Give it a different
        // value from the raw payload checksum used by the image header.
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
        .algorithm_identity = *algorithm_identity,
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
        // Use placeholder bytes for a 256-byte direct index header and its
        // payload. These tests check the outer image format.
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

/// Match validation to the fixture header, with payload checks deferred by default.
[[nodiscard]] idx::minimizer_fragment_image_validation_options validation_options(
        const image_fixture& fixture,
        idx::minimizer_fragment_payload_crc_policy payload_crc_policy =
            idx::minimizer_fragment_payload_crc_policy::deferred) noexcept {
    return idx::minimizer_fragment_image_validation_options{
        .expected_identity = expected_identity(fixture.header),
        .payload_crc_policy = payload_crc_policy,
    };
}

/// Return the header decoding error, terminating if decoding succeeds.
[[nodiscard]] idx::minimizer_fragment_image_error decoded_error(
        std::span<const std::byte> encoded) {
    const auto result = idx::decode_minimizer_fragment_image_header(encoded);
    if (result) {
        std::terminate();
    }
    return result.error();
}

/// Release an anonymous memory mapping when this object leaves scope.
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

} // namespace bioseq::tests::unit::indexing::persisted_minimizer_image_test_support
