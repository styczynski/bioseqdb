/**
 * @file minimizer_fragment_image.cppm
 * @brief Encode and check a minimizer-fragment file without PostgreSQL dependencies.
 *
 * Integer fields use little-endian encoding. The header checksum covers the
 * complete header with its own field zeroed. Section bounds are checked before
 * constructing spans. Bytes 192..223 describe optional features and the compact
 * hash section, while bytes 224..4095 remain reserved.
 */
module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <sys/types.h>
#include <type_traits>
#include <utility>

export module bioseq.indexing.minimizer_fragment_image;

import bioseq.common.checked_arithmetic;
import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

/// Magic value  that identifies a minimizez-fragment file.
inline constexpr std::uint64_t k_minimizer_fragment_image_magic = 0x32563546504d5342ULL;
/// Current serialized image layout revision.
inline constexpr std::uint32_t k_minimizer_fragment_image_layout_revision = 2u;
/// Fixed header size covered by the header checksum.
inline constexpr std::uint32_t k_minimizer_fragment_image_header_bytes = 4096u;
/// Minimizer-index kind stored at header byte 34.
inline constexpr std::uint8_t k_minimizer_fragment_image_index_identity = 1u;
/// Relation-backed storage kind stored at header byte 35.
inline constexpr std::uint8_t
    k_minimizer_fragment_image_relation_backed_storage_kind = 0u;
/// Grouped minimizer layout ID stored at header bytes 36..37.
inline constexpr std::uint16_t k_minimizer_fragment_image_layout_identity = 5u;
/// Alignment required for the three primary image sections.
inline constexpr std::uint64_t k_minimizer_fragment_image_section_alignment = 8u;
/// Alignment of the optional compact hash index section.
inline constexpr std::uint64_t k_minimizer_fragment_compact_hash32_alignment = 64u;
/// Feature bit indicating that the file contains a compact hash index.
inline constexpr std::uint64_t k_minimizer_fragment_compact_hash32_feature =
    std::uint64_t{1u} << 0u;
/// Feature bits understood by this image revision.
inline constexpr std::uint64_t k_minimizer_fragment_image_known_features =
    k_minimizer_fragment_compact_hash32_feature;
/// Byte offset of the checksum field within the fixed header.
inline constexpr std::size_t k_minimizer_fragment_image_header_crc_offset = 184u;
/// Width of the fixed header checksum field in bytes.
inline constexpr std::size_t k_minimizer_fragment_image_header_crc_bytes = 8u;
/// Largest file offset supported by off_t on this platform.
inline constexpr std::uint64_t k_minimizer_fragment_image_default_max_file_offset = []() constexpr {
    static_assert(std::numeric_limits<::off_t>::is_integer);
    if constexpr (std::numeric_limits<::off_t>::digits
        >= std::numeric_limits<std::uint64_t>::digits) {
        return std::numeric_limits<std::uint64_t>::max();
    } else {
        return static_cast<std::uint64_t>(
            std::numeric_limits<::off_t>::max());
    }
}();
/// Largest mapped byte count supported by size_t on, this platform.
inline constexpr std::uint64_t k_minimizer_fragment_image_default_max_mapped_bytes = []() constexpr {
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
        return std::numeric_limits<std::uint64_t>::max();
    } else {
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max());
    }
}();

/** @brief Byte range and checksum for one serialized image section. */
struct minimizer_fragment_image_section {
    std::uint64_t offset = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t crc64 = 0u;

    friend constexpr bool operator==(
        const minimizer_fragment_image_section&, const minimizer_fragment_image_section&) = default;
};

/// Location and header checksum of an optional  compact hash index.
/// The outer header protects these fields with its checksum. The compact index
/// codec checks the nested header and payload separately.
struct minimizer_fragment_compact_hash32_section_receipt {
    std::uint64_t offset = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t direct_header_crc64 = 0u;

    friend constexpr bool operator==(
        const minimizer_fragment_compact_hash32_section_receipt&,
        const minimizer_fragment_compact_hash32_section_receipt&) = default;
};

/// Size and header checksum supplied when adding a compact hash section.
/// Providing this option enables the feature bit and requires a nonempty section
struct minimizer_fragment_compact_hash32_section_spec {
    std::uint64_t bytes = 0u;
    std::uint64_t direct_header_crc64 = 0u;

    friend constexpr bool operator==(
        const minimizer_fragment_compact_hash32_section_spec&,
        const minimizer_fragment_compact_hash32_section_spec&) = default;
};

/** @brief Header fields describing the source generation and section locations. */
struct minimizer_fragment_image_header {
    std::uint64_t magic = 0u;
    std::uint32_t layout_revision = 0u;
    std::uint32_t header_bytes = 0u;
    std::uint32_t database_oid = 0u;
    std::uint32_t tablespace_oid = 0u;
    std::uint32_t main_relfilenode = 0u;
    std::uint32_t user_relid = 0u;
    std::uint16_t column_attno = 0u;
    std::uint8_t index_identity = 0u;
    std::uint8_t storage_kind =
        k_minimizer_fragment_image_relation_backed_storage_kind;
    std::uint16_t layout_identity = 0u;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_crc = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t target_length = 0u;
    std::uint64_t file_bytes = 0u;
    std::uint32_t position_segment_count = 0u;
    std::uint64_t position_segment_span_bp = 0u;
    minimizer_fragment_image_section payload{};
    minimizer_fragment_image_section directory{};
    minimizer_fragment_image_section manifest{};
    std::uint64_t header_crc64 = 0u;
    std::uint64_t feature_bitmap = 0u;
    minimizer_fragment_compact_hash32_section_receipt compact_hash32_direct{};

    friend constexpr bool operator==(
        const minimizer_fragment_image_header&, const minimizer_fragment_image_header&) = default;
};

/// Fixed-size byte array containing the little-endian file header.
using encoded_minimizer_fragment_image_header =
    std::array<std::byte, static_cast<std::size_t>(k_minimizer_fragment_image_header_bytes)>;

/** @brief Aligned section locations and total size of a complete image. */
struct minimizer_fragment_image_layout {
    minimizer_fragment_image_section payload{};
    minimizer_fragment_image_section directory{};
    minimizer_fragment_image_section manifest{};
    std::uint64_t feature_bitmap = 0u;
    minimizer_fragment_compact_hash32_section_receipt compact_hash32_direct{};
    std::uint64_t file_bytes = 0u;

    friend constexpr bool operator==(
        const minimizer_fragment_image_layout&, const minimizer_fragment_image_layout&) = default;
};

/// File-offset and mapping-size limits checked before creating section spans.
struct minimizer_fragment_image_address_limits {
    std::uint64_t max_file_offset = k_minimizer_fragment_image_default_max_file_offset;
    std::uint64_t max_mapped_bytes = k_minimizer_fragment_image_default_max_mapped_bytes;
};

/** @brief Expected source, generation, and  section fields checked when opening a file. */
struct expected_minimizer_fragment_image_identity {
    std::uint32_t database_oid = 0u;
    std::uint32_t tablespace_oid = 0u;
    std::uint32_t main_relfilenode = 0u;
    std::uint32_t user_relid = 0u;
    std::uint16_t column_attno = 0u;
    std::uint8_t index_identity = k_minimizer_fragment_image_index_identity;
    std::uint16_t layout_identity = k_minimizer_fragment_image_layout_identity;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_crc = 0u;
    std::uint64_t storage_contract_checksum = 0u;
    std::uint64_t target_length = 0u;
    std::uint64_t file_bytes = 0u;
    std::uint32_t position_segment_count = 0u;
    std::uint64_t position_segment_span_bp = 0u;
    minimizer_fragment_image_section payload{};
    minimizer_fragment_image_section directory{};
    minimizer_fragment_image_section manifest{};
    std::uint64_t header_crc64 = 0u;
    /// Optional bepause the expected header checksum already covers these fields.
    /// When supplied, they must also match individually.
    std::optional<std::uint64_t> feature_bitmap = std::nullopt;
    std::optional<minimizer_fragment_compact_hash32_section_receipt>
        compact_hash32_direct = std::nullopt;
};

/** @brief Choose whether image validation also reads and checksums the payload. */
enum class minimizer_fragment_payload_crc_policy : std::uint8_t {
    /// Check metadata without scanning the payload. Its stored checksum can
    /// be verified later by a full validation pass.
    deferred = 0u,
    /// Read the complete payload and compare its checksum during validation.
    validate = 1u,
};

/** @brief Expected identity, address limits, and payload-checksum policy for  opening. */
struct minimizer_fragment_image_validation_options {
    minimizer_fragment_image_address_limits address_limits{};
    expected_minimizer_fragment_image_identity expected_identity{};
    minimizer_fragment_payload_crc_policy payload_crc_policy =
        minimizer_fragment_payload_crc_policy::deferred;
};

/** @brief Errors while planning, decoding, or checking a fragment image. */
enum class minimizer_fragment_image_error : std::uint8_t {
    header_size_mismatch,
    bad_magic,
    bad_layout_revision,
    bad_header_bytes,
    bad_index_identity,
    bad_storage_kind,
    bad_layout_identity,
    nonzero_reserved,
    header_crc_mismatch,
    file_too_small,
    file_size_mismatch,
    arithmetic_overflow,
    payload_offset_mismatch,
    directory_offset_mismatch,
    manifest_offset_mismatch,
    manifest_end_mismatch,
    unsupported_features,
    compact_hash32_direct_feature_mismatch,
    compact_hash32_direct_offset_mismatch,
    compact_hash32_direct_end_mismatch,
    file_offset_not_representable,
    mapped_size_not_representable,
    nonzero_section_padding,
    identity_mismatch,
    payload_crc_mismatch,
    directory_crc_mismatch,
    manifest_crc_mismatch,
    manifest_metadata_invalid,
    directory_metadata_invalid,
    generation_metadata_mismatch,
};

/**
 * @brief Section spans returned after checking the header, identity, and bounds.
 *
 * Keep the mapped image alive and unchanged while using these spans. Byte
 * counters show how much data was read for each checksum.
 */
struct validated_minimizer_fragment_image_view {
    minimizer_fragment_image_header header{};
    std::span<const std::byte> payload{};
    std::span<const std::byte> directory{};
    std::span<const std::byte> manifest{};
    std::span<const std::byte> compact_hash32_direct{};
    std::uint64_t header_crc_bytes = 0u;
    std::uint64_t payload_crc_bytes = 0u;
    std::uint64_t directory_crc_bytes = 0u;
    std::uint64_t manifest_crc_bytes = 0u;
};

} // namespace bioseq::indexing

namespace bioseq::indexing::minimizer_fragment_image_detail {

/**
 * @brief Write an unsigned integer in little-endian order to a range the caller checked.
 * @tparam p_uint Integer type that determines the encoded width.
 */
template <class p_uint>
requires (std::is_unsigned_v<p_uint>)
constexpr void store_le(
        std::span<std::byte> encoded,
        std::size_t offset,
        p_uint value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(p_uint); ++byte) {
        encoded[offset + byte] = static_cast<std::byte>(value & p_uint{0xffu});
        value >>= 8u;
    }
}

/**
 * @brief Read an unsigned integer in little-endian order frm a range the caller checked.
 * @tparam p_uint Integer type, that determines the encoded width
 */
template <class p_uint>
requires (std::is_unsigned_v<p_uint>)
[[nodiscard]] constexpr p_uint load_le(
        std::span<const std::byte> encoded,
        std::size_t offset) noexcept {
    p_uint value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(p_uint); ++byte) {
        value |= static_cast<p_uint>(
            std::to_integer<std::uint8_t>(encoded[offset + byte]))
            << (byte * 8u);
    }
    return value;
}

/// Add two file sizes or offsets, returning an error on overflow.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_fragment_image_error>
checked_add(std::uint64_t left, std::uint64_t right) noexcept {
    const auto sum = bioseq::common::checked_add(left, right);
    if (!sum) {
        return std::unexpected(minimizer_fragment_image_error::arithmetic_overflow);
    }
    return *sum;
}

/// Round  an offset up, returning an error for invalid alignment or overflow.
[[nodiscard]] constexpr std::expected<std::uint64_t, minimizer_fragment_image_error>
checked_align_up(
        std::uint64_t value,
        std::uint64_t alignment) noexcept {
    const auto aligned = bioseq::common::checked_align_up(value, alignment);
    if (!aligned) {
        return std::unexpected(minimizer_fragment_image_error::arithmetic_overflow);
    }
    return *aligned;
}

static_assert((k_minimizer_fragment_image_section_alignment
    & (k_minimizer_fragment_image_section_alignment - 1u)) == 0u);
static_assert((k_minimizer_fragment_compact_hash32_alignment
    & (k_minimizer_fragment_compact_hash32_alignment - 1u)) == 0u);

/// Check whether the compact hash index feature bit is set.
[[nodiscard]] constexpr bool has_compact_hash32_direct_feature(
        std::uint64_t feature_bitmap) noexcept {
    return (feature_bitmap & k_minimizer_fragment_compact_hash32_feature) != 0u;
}

/// Check that an absent compact hash section has no leftover metadata.
[[nodiscard]] constexpr bool direct_receipt_is_zero(
        const minimizer_fragment_compact_hash32_section_receipt& receipt) noexcept {
    return receipt == minimizer_fragment_compact_hash32_section_receipt{};
}

/// Test whether a reserved or alignment-padding byte range is zero-filled.
[[nodiscard]] constexpr bool all_zero(
        std::span<const std::byte> bytes) noexcept {
    for (const std::byte value : bytes) {
        if (value != std::byte{0}) {
            return false;
        }
    }
    return true;
}

/// Compute the full-header CRC with its checksum field treated as zero.
[[nodiscard]] constexpr std::uint64_t header_crc(
        std::span<const std::byte> encoded) noexcept {
    crc64_ecma182_accumulator crc{};
    crc.update(encoded.first(k_minimizer_fragment_image_header_crc_offset));
    for (std::size_t byte = 0u; byte < k_minimizer_fragment_image_header_crc_bytes; ++byte) {
        crc.update_byte(0u);
    }
    crc.update(encoded.subspan(
        k_minimizer_fragment_image_header_crc_offset + k_minimizer_fragment_image_header_crc_bytes));
    return crc.checksum();
}

/// Compare the header with the caller's expected source and section fields.
[[nodiscard]] constexpr bool identity_matches(
        const minimizer_fragment_image_header& header,
        const expected_minimizer_fragment_image_identity& expected) noexcept {
    return header.database_oid == expected.database_oid
        && header.tablespace_oid == expected.tablespace_oid
        && header.main_relfilenode == expected.main_relfilenode
        && header.user_relid == expected.user_relid
        && header.column_attno == expected.column_attno
        && header.index_identity == expected.index_identity
        && header.layout_identity == expected.layout_identity
        && header.query_key == expected.query_key
        && header.generation_id == expected.generation_id
        && header.source_identity == expected.source_identity
        && header.source_content_crc == expected.source_content_crc
        && header.storage_contract_checksum
            == expected.storage_contract_checksum
        && header.target_length == expected.target_length
        && header.file_bytes == expected.file_bytes
        && header.position_segment_count == expected.position_segment_count
        && header.position_segment_span_bp
            == expected.position_segment_span_bp
        && header.payload == expected.payload
        && header.directory == expected.directory
        && header.manifest == expected.manifest
        && header.header_crc64 == expected.header_crc64
        && (!expected.feature_bitmap
            || header.feature_bitmap == *expected.feature_bitmap)
        && (!expected.compact_hash32_direct
            || header.compact_hash32_direct == *expected.compact_hash32_direct);
}

} // namespace bioseq::indexing::minimizer_fragment_image_detail

export namespace bioseq::indexing {

/**
 * @brief Place payload, directory, and manifest after the fixed header with required alignment.
 *
 * An optional compact hash index follows the  manifest. The supplied checksums
 * are recorded as metadata, without reading section bytes.
 *
 * @param[in] payload_bytes Payload size in bytes.
 * @param[in] directory_bytes Directory size in bytes.
 * @param[in] manifest_bytes Manifest size in bytes.
 * @param[in] payload_crc64 Stored payload checksum.
 * @param[in] directory_crc64 Stored directory checksum.
 * @param[in] manifest_crc64 Stored manifest checksum.
 * @param[in] compact_hash32_direct Optional compact-section size and header checksum.
 * @return Section locations and total size, or an overflow or layout error.
 */
[[nodiscard]] constexpr std::expected<minimizer_fragment_image_layout,
                                      minimizer_fragment_image_error>
plan_minimizer_fragment_image_layout(std::uint64_t payload_bytes,
                                     std::uint64_t directory_bytes,
                                     std::uint64_t manifest_bytes,
                                     std::uint64_t payload_crc64 = 0u,
                                     std::uint64_t directory_crc64 = 0u,
                                     std::uint64_t manifest_crc64 = 0u,
                                     std::optional<minimizer_fragment_compact_hash32_section_spec>
                                         compact_hash32_direct = std::nullopt) noexcept {
    minimizer_fragment_image_layout layout{};
    layout.payload = minimizer_fragment_image_section{
        .offset = k_minimizer_fragment_image_header_bytes,
        .bytes = payload_bytes,
        .crc64 = payload_crc64,
    };

    const auto payload_end = minimizer_fragment_image_detail::checked_add(
        layout.payload.offset, layout.payload.bytes);
    if (!payload_end) {
        return std::unexpected(payload_end.error());
    }
    const auto directory_offset =
        minimizer_fragment_image_detail::checked_align_up(
            *payload_end, k_minimizer_fragment_image_section_alignment);
    if (!directory_offset) {
        return std::unexpected(directory_offset.error());
    }
    layout.directory = minimizer_fragment_image_section{
        .offset = *directory_offset,
        .bytes = directory_bytes,
        .crc64 = directory_crc64,
    };

    const auto directory_end = minimizer_fragment_image_detail::checked_add(
        layout.directory.offset, layout.directory.bytes);
    if (!directory_end) {
        return std::unexpected(directory_end.error());
    }
    const auto manifest_offset =
        minimizer_fragment_image_detail::checked_align_up(
            *directory_end, k_minimizer_fragment_image_section_alignment);
    if (!manifest_offset) {
        return std::unexpected(manifest_offset.error());
    }
    layout.manifest = minimizer_fragment_image_section{
        .offset = *manifest_offset,
        .bytes = manifest_bytes,
        .crc64 = manifest_crc64,
    };

    const auto manifest_end = minimizer_fragment_image_detail::checked_add(
        layout.manifest.offset, layout.manifest.bytes);
    if (!manifest_end) {
        return std::unexpected(manifest_end.error());
    }
    if (!compact_hash32_direct) {
        layout.file_bytes = *manifest_end;
        return layout;
    }
    if (compact_hash32_direct->bytes == 0u) {
        return std::unexpected(
            minimizer_fragment_image_error::compact_hash32_direct_feature_mismatch);
    }
    const auto direct_offset = minimizer_fragment_image_detail::checked_align_up(
        *manifest_end, k_minimizer_fragment_compact_hash32_alignment);
    if (!direct_offset) {
        return std::unexpected(direct_offset.error());
    }
    layout.feature_bitmap = k_minimizer_fragment_compact_hash32_feature;
    layout.compact_hash32_direct = minimizer_fragment_compact_hash32_section_receipt{
        .offset = *direct_offset,
        .bytes = compact_hash32_direct->bytes,
        .direct_header_crc64 = compact_hash32_direct->direct_header_crc64,
    };
    const auto direct_end = minimizer_fragment_image_detail::checked_add(
        layout.compact_hash32_direct.offset,
        layout.compact_hash32_direct.bytes);
    if (!direct_end) {
        return std::unexpected(direct_end.error());
    }
    layout.file_bytes = *direct_end;
    return layout;
}

/**
 * @brief Check section order, alignment, and whether the file fits native address limits.
 *
 * @param[in] header Decoded section locations to cheok.
 * @param[in] actual_file_bytes Complete mapped file size.
 * @param[in] limits File-offset and mapping-size limits.
 * @return Success  or a size, alignment, feature, or overflow error.
 */
[[nodiscard]] constexpr std::expected<void, minimizer_fragment_image_error>
validate_minimizer_fragment_image_layout(
    const minimizer_fragment_image_header& header,
    std::uint64_t actual_file_bytes,
    minimizer_fragment_image_address_limits limits = {}) noexcept {
    limits.max_file_offset = std::min(
        limits.max_file_offset, k_minimizer_fragment_image_default_max_file_offset);
    limits.max_mapped_bytes = std::min(
        limits.max_mapped_bytes, k_minimizer_fragment_image_default_max_mapped_bytes);
    if (actual_file_bytes < k_minimizer_fragment_image_header_bytes) {
        return std::unexpected(minimizer_fragment_image_error::file_too_small);
    }
    if (header.file_bytes != actual_file_bytes) {
        return std::unexpected(minimizer_fragment_image_error::file_size_mismatch);
    }
    if (header.file_bytes > limits.max_file_offset) {
        return std::unexpected(
            minimizer_fragment_image_error::file_offset_not_representable);
    }
    if (header.file_bytes > limits.max_mapped_bytes) {
        return std::unexpected(
            minimizer_fragment_image_error::mapped_size_not_representable);
    }
    if (header.payload.offset != k_minimizer_fragment_image_header_bytes) {
        return std::unexpected(minimizer_fragment_image_error::payload_offset_mismatch);
    }

    const auto payload_end = minimizer_fragment_image_detail::checked_add(
        header.payload.offset, header.payload.bytes);
    if (!payload_end) {
        return std::unexpected(payload_end.error());
    }
    const auto expected_directory_offset =
        minimizer_fragment_image_detail::checked_align_up(
            *payload_end, k_minimizer_fragment_image_section_alignment);
    if (!expected_directory_offset) {
        return std::unexpected(expected_directory_offset.error());
    }
    if (header.directory.offset != *expected_directory_offset) {
        return std::unexpected(minimizer_fragment_image_error::directory_offset_mismatch);
    }

    const auto directory_end = minimizer_fragment_image_detail::checked_add(
        header.directory.offset, header.directory.bytes);
    if (!directory_end) {
        return std::unexpected(directory_end.error());
    }
    const auto expected_manifest_offset =
        minimizer_fragment_image_detail::checked_align_up(
            *directory_end, k_minimizer_fragment_image_section_alignment);
    if (!expected_manifest_offset) {
        return std::unexpected(expected_manifest_offset.error());
    }
    if (header.manifest.offset != *expected_manifest_offset) {
        return std::unexpected(minimizer_fragment_image_error::manifest_offset_mismatch);
    }

    const auto manifest_end = minimizer_fragment_image_detail::checked_add(
        header.manifest.offset, header.manifest.bytes);
    if (!manifest_end) {
        return std::unexpected(manifest_end.error());
    }

    if ((header.feature_bitmap & ~k_minimizer_fragment_image_known_features) != 0u) {
        return std::unexpected(minimizer_fragment_image_error::unsupported_features);
    }
    if (!minimizer_fragment_image_detail::has_compact_hash32_direct_feature(
            header.feature_bitmap)) {
        if (!minimizer_fragment_image_detail::direct_receipt_is_zero(
                header.compact_hash32_direct)) {
            return std::unexpected(
                minimizer_fragment_image_error::compact_hash32_direct_feature_mismatch);
        }
        if (*manifest_end != header.file_bytes) {
            return std::unexpected(minimizer_fragment_image_error::manifest_end_mismatch);
        }
        return {};
    }
    if (header.compact_hash32_direct.bytes == 0u) {
        return std::unexpected(
            minimizer_fragment_image_error::compact_hash32_direct_feature_mismatch);
    }
    const auto expected_direct_offset = minimizer_fragment_image_detail::checked_align_up(
        *manifest_end, k_minimizer_fragment_compact_hash32_alignment);
    if (!expected_direct_offset) {
        return std::unexpected(expected_direct_offset.error());
    }
    if (header.compact_hash32_direct.offset != *expected_direct_offset) {
        return std::unexpected(
            minimizer_fragment_image_error::compact_hash32_direct_offset_mismatch);
    }
    const auto direct_end = minimizer_fragment_image_detail::checked_add(
        header.compact_hash32_direct.offset,
        header.compact_hash32_direct.bytes);
    if (!direct_end) {
        return std::unexpected(direct_end.error());
    }
    if (*direct_end != header.file_bytes) {
        return std::unexpected(
            minimizer_fragment_image_error::compact_hash32_direct_end_mismatch);
    }
    return {};
}

/**
 * @brief Check header fields and layout, then encode them with a header checksum.
 *
 * @param[in] header Header values and section checksums to store.
 * @return Fixed-size header bytes, or a format or layout error
 */
[[nodiscard]] constexpr std::expected<encoded_minimizer_fragment_image_header, minimizer_fragment_image_error>
encode_minimizer_fragment_image_header(minimizer_fragment_image_header header) noexcept {
    if (header.magic != k_minimizer_fragment_image_magic) {
        return std::unexpected(minimizer_fragment_image_error::bad_magic);
    }
    if (header.layout_revision != k_minimizer_fragment_image_layout_revision) {
        return std::unexpected(minimizer_fragment_image_error::bad_layout_revision);
    }
    if (header.header_bytes != k_minimizer_fragment_image_header_bytes) {
        return std::unexpected(minimizer_fragment_image_error::bad_header_bytes);
    }
    if (header.index_identity != k_minimizer_fragment_image_index_identity) {
        return std::unexpected(minimizer_fragment_image_error::bad_index_identity);
    }
    if (header.storage_kind
        != k_minimizer_fragment_image_relation_backed_storage_kind) {
        return std::unexpected(minimizer_fragment_image_error::bad_storage_kind);
    }
    if (header.layout_identity != k_minimizer_fragment_image_layout_identity) {
        return std::unexpected(minimizer_fragment_image_error::bad_layout_identity);
    }
    const auto valid_layout = validate_minimizer_fragment_image_layout(
        header, header.file_bytes);
    if (!valid_layout) {
        return std::unexpected(valid_layout.error());
    }

    encoded_minimizer_fragment_image_header encoded{};
    const std::span<std::byte> bytes{encoded};
    using minimizer_fragment_image_detail::store_le;
    store_le(bytes, 0u, header.magic);
    store_le(bytes, 8u, header.layout_revision);
    store_le(bytes, 12u, header.header_bytes);
    store_le(bytes, 16u, header.database_oid);
    store_le(bytes, 20u, header.tablespace_oid);
    store_le(bytes, 24u, header.main_relfilenode);
    store_le(bytes, 28u, header.user_relid);
    store_le(bytes, 32u, header.column_attno);
    store_le(bytes, 34u, header.index_identity);
    store_le(bytes, 35u, header.storage_kind);
    store_le(bytes, 36u, header.layout_identity);
    store_le(bytes, 40u, header.query_key);
    store_le(bytes, 48u, header.generation_id);
    store_le(bytes, 56u, header.source_identity);
    store_le(bytes, 64u, header.source_content_crc);
    store_le(bytes, 72u, header.storage_contract_checksum);
    store_le(bytes, 80u, header.target_length);
    store_le(bytes, 88u, header.file_bytes);
    store_le(bytes, 96u, header.position_segment_count);
    store_le(bytes, 104u, header.position_segment_span_bp);
    store_le(bytes, 112u, header.payload.offset);
    store_le(bytes, 120u, header.payload.bytes);
    store_le(bytes, 128u, header.payload.crc64);
    store_le(bytes, 136u, header.directory.offset);
    store_le(bytes, 144u, header.directory.bytes);
    store_le(bytes, 152u, header.directory.crc64);
    store_le(bytes, 160u, header.manifest.offset);
    store_le(bytes, 168u, header.manifest.bytes);
    store_le(bytes, 176u, header.manifest.crc64);
    store_le(bytes, 192u, header.feature_bitmap);
    store_le(bytes, 200u, header.compact_hash32_direct.offset);
    store_le(bytes, 208u, header.compact_hash32_direct.bytes);
    store_le(
        bytes, 216u, header.compact_hash32_direct.direct_header_crc64);
    // The checksum field is still zero before storing the calculated CRC.
    header.header_crc64 = minimizer_fragment_image_detail::header_crc(bytes);
    store_le(bytes, k_minimizer_fragment_image_header_crc_offset, header.header_crc64);
    return encoded;
}

/**
 * @brief Decode the fixed header and verify its format and checksum.
 *
 * @param[in] encoded Exactly one header's bytes.
 * @return Header fields, or a format, reserved-byte, or checksum error.
 */
[[nodiscard]] constexpr std::expected<minimizer_fragment_image_header, minimizer_fragment_image_error>
decode_minimizer_fragment_image_header(std::span<const std::byte> encoded) noexcept {
    if (encoded.size() != k_minimizer_fragment_image_header_bytes) {
        return std::unexpected(minimizer_fragment_image_error::header_size_mismatch);
    }
    using minimizer_fragment_image_detail::load_le;
    minimizer_fragment_image_header header{};
    header.magic = load_le<std::uint64_t>(encoded, 0u);
    header.layout_revision = load_le<std::uint32_t>(encoded, 8u);
    header.header_bytes = load_le<std::uint32_t>(encoded, 12u);
    header.database_oid = load_le<std::uint32_t>(encoded, 16u);
    header.tablespace_oid = load_le<std::uint32_t>(encoded, 20u);
    header.main_relfilenode = load_le<std::uint32_t>(encoded, 24u);
    header.user_relid = load_le<std::uint32_t>(encoded, 28u);
    header.column_attno = load_le<std::uint16_t>(encoded, 32u);
    header.index_identity = load_le<std::uint8_t>(encoded, 34u);
    header.storage_kind = load_le<std::uint8_t>(encoded, 35u);
    header.layout_identity = load_le<std::uint16_t>(encoded, 36u);
    header.query_key = load_le<std::uint64_t>(encoded, 40u);
    header.generation_id = load_le<std::uint64_t>(encoded, 48u);
    header.source_identity = load_le<std::uint64_t>(encoded, 56u);
    header.source_content_crc = load_le<std::uint64_t>(encoded, 64u);
    header.storage_contract_checksum = load_le<std::uint64_t>(encoded, 72u);
    header.target_length = load_le<std::uint64_t>(encoded, 80u);
    header.file_bytes = load_le<std::uint64_t>(encoded, 88u);
    header.position_segment_count = load_le<std::uint32_t>(encoded, 96u);
    header.position_segment_span_bp = load_le<std::uint64_t>(encoded, 104u);
    header.payload = minimizer_fragment_image_section{
        .offset = load_le<std::uint64_t>(encoded, 112u),
        .bytes = load_le<std::uint64_t>(encoded, 120u),
        .crc64 = load_le<std::uint64_t>(encoded, 128u),
    };
    header.directory = minimizer_fragment_image_section{
        .offset = load_le<std::uint64_t>(encoded, 136u),
        .bytes = load_le<std::uint64_t>(encoded, 144u),
        .crc64 = load_le<std::uint64_t>(encoded, 152u),
    };
    header.manifest = minimizer_fragment_image_section{
        .offset = load_le<std::uint64_t>(encoded, 160u),
        .bytes = load_le<std::uint64_t>(encoded, 168u),
        .crc64 = load_le<std::uint64_t>(encoded, 176u),
    };
    header.header_crc64 = load_le<std::uint64_t>(
        encoded, k_minimizer_fragment_image_header_crc_offset);
    header.feature_bitmap = load_le<std::uint64_t>(encoded, 192u);
    header.compact_hash32_direct = minimizer_fragment_compact_hash32_section_receipt{
        .offset = load_le<std::uint64_t>(encoded, 200u),
        .bytes = load_le<std::uint64_t>(encoded, 208u),
        .direct_header_crc64 = load_le<std::uint64_t>(encoded, 216u),
    };

    if (header.magic != k_minimizer_fragment_image_magic) {
        return std::unexpected(minimizer_fragment_image_error::bad_magic);
    }
    if (header.layout_revision != k_minimizer_fragment_image_layout_revision) {
        return std::unexpected(minimizer_fragment_image_error::bad_layout_revision);
    }
    if (header.header_bytes != k_minimizer_fragment_image_header_bytes) {
        return std::unexpected(minimizer_fragment_image_error::bad_header_bytes);
    }
    if (header.index_identity != k_minimizer_fragment_image_index_identity) {
        return std::unexpected(minimizer_fragment_image_error::bad_index_identity);
    }
    if (header.storage_kind
        != k_minimizer_fragment_image_relation_backed_storage_kind) {
        return std::unexpected(minimizer_fragment_image_error::bad_storage_kind);
    }
    if (header.layout_identity != k_minimizer_fragment_image_layout_identity) {
        return std::unexpected(minimizer_fragment_image_error::bad_layout_identity);
    }
    if ((header.feature_bitmap & ~k_minimizer_fragment_image_known_features) != 0u) {
        return std::unexpected(minimizer_fragment_image_error::unsupported_features);
    }
    const bool has_direct =
        minimizer_fragment_image_detail::has_compact_hash32_direct_feature(
            header.feature_bitmap);
    if (has_direct != (header.compact_hash32_direct.bytes != 0u)
        || (!has_direct
            && !minimizer_fragment_image_detail::direct_receipt_is_zero(
                header.compact_hash32_direct))) {
        return std::unexpected(
            minimizer_fragment_image_error::compact_hash32_direct_feature_mismatch);
    }
    if (!minimizer_fragment_image_detail::all_zero(encoded.subspan(38u, 2u))
        || !minimizer_fragment_image_detail::all_zero(encoded.subspan(100u, 4u))
        || !minimizer_fragment_image_detail::all_zero(encoded.subspan(224u))) {
        return std::unexpected(minimizer_fragment_image_error::nonzero_reserved);
    }
    if (header.header_crc64 != minimizer_fragment_image_detail::header_crc(encoded)) {
        return std::unexpected(minimizer_fragment_image_error::header_crc_mismatch);
    }
    return header;
}

/**
 * @brief Open section spans by reading only the fixed header.
 *
 * Check the header checksum, expected identity, and  section bounds. Section
 * contents are checked by later readers. Keep image alive and unchanged while
 * using the returned spans.
 *
 * @param[in] image Complete mapped file.
 * @param[in] options Address limits and expected identity. Payload policy is unused here.
 * @return Borrowed section spans, or a validation error.
 */
[[nodiscard]] inline std::expected<validated_minimizer_fragment_image_view, minimizer_fragment_image_error>
open_minimizer_fragment_image_bounded(
        std::span<const std::byte> image,
        const minimizer_fragment_image_validation_options& options) {
    if (image.size() < k_minimizer_fragment_image_header_bytes) {
        return std::unexpected(minimizer_fragment_image_error::file_too_small);
    }
    const auto decoded = decode_minimizer_fragment_image_header(
        image.first(static_cast<std::size_t>(k_minimizer_fragment_image_header_bytes)));
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (image.size() > std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(
                minimizer_fragment_image_error::mapped_size_not_representable);
        }
    }
    const std::uint64_t actual_file_bytes =
        static_cast<std::uint64_t>(image.size());
    const auto valid_layout = validate_minimizer_fragment_image_layout(
        *decoded, actual_file_bytes, options.address_limits);
    if (!valid_layout) {
        return std::unexpected(valid_layout.error());
    }
    if (!minimizer_fragment_image_detail::identity_matches(
            *decoded, options.expected_identity)) {
        return std::unexpected(minimizer_fragment_image_error::identity_mismatch);
    }

    const auto payload = image.subspan(
        static_cast<std::size_t>(decoded->payload.offset),
        static_cast<std::size_t>(decoded->payload.bytes));
    const auto directory = image.subspan(
        static_cast<std::size_t>(decoded->directory.offset),
        static_cast<std::size_t>(decoded->directory.bytes));
    const auto manifest = image.subspan(
        static_cast<std::size_t>(decoded->manifest.offset),
        static_cast<std::size_t>(decoded->manifest.bytes));
    const auto compact_hash32_direct =
        minimizer_fragment_image_detail::has_compact_hash32_direct_feature(
            decoded->feature_bitmap)
        ? image.subspan(
            static_cast<std::size_t>(
                decoded->compact_hash32_direct.offset),
            static_cast<std::size_t>(
                decoded->compact_hash32_direct.bytes))
        : std::span<const std::byte>{};

    return validated_minimizer_fragment_image_view{
        .header = *decoded,
        .payload = payload,
        .directory = directory,
        .manifest = manifest,
        .compact_hash32_direct = compact_hash32_direct,
        .header_crc_bytes = k_minimizer_fragment_image_header_bytes,
        .payload_crc_bytes = 0u,
        .directory_crc_bytes = 0u,
        .manifest_crc_bytes = 0u,
    };
}

/**
 * @brief Check image metadata, section, padding, and the requested payload checksum.
 *
 * Directory and manifest checksums are always verified. The payload is scanned
 * only when its policy is validate. The compact hash sction is returned for
 * its own codec to validate separately.
 *
 * @param[in] image Complete mapped file to keep alive while using the  result.
 * @param[in] options Expected identity, address limits, and payload policy.
 * @return Borrowed section spans, or a validation error.
 */
[[nodiscard]] inline std::expected<validated_minimizer_fragment_image_view, minimizer_fragment_image_error>
validate_minimizer_fragment_image(
        std::span<const std::byte> image,
        const minimizer_fragment_image_validation_options& options) {
    const auto bounded = open_minimizer_fragment_image_bounded(image, options);
    if (!bounded) return std::unexpected(bounded.error());
    const minimizer_fragment_image_header& decoded = bounded->header;
    const auto payload = bounded->payload;
    const auto directory = bounded->directory;
    const auto manifest = bounded->manifest;
    const auto compact_hash32_direct = bounded->compact_hash32_direct;
    const auto payload_offset = static_cast<std::size_t>(
        decoded.payload.offset);
    const auto payload_bytes = static_cast<std::size_t>(
        decoded.payload.bytes);
    const auto directory_offset = static_cast<std::size_t>(
        decoded.directory.offset);
    const auto directory_bytes = static_cast<std::size_t>(
        decoded.directory.bytes);
    const auto manifest_offset = static_cast<std::size_t>(
        decoded.manifest.offset);
    const auto manifest_bytes = static_cast<std::size_t>(
        decoded.manifest.bytes);
    const bool has_direct =
        minimizer_fragment_image_detail::has_compact_hash32_direct_feature(
            decoded.feature_bitmap);

    const auto payload_end = payload_offset + payload_bytes;
    const auto payload_padding = image.subspan(
        payload_end, directory_offset - payload_end);
    const auto directory_end = directory_offset + directory_bytes;
    const auto directory_padding = image.subspan(
        directory_end, manifest_offset - directory_end);
    const auto manifest_end = manifest_offset + manifest_bytes;
    std::span<const std::byte> manifest_padding{};
    if (has_direct) {
        const auto direct_offset = static_cast<std::size_t>(
            decoded.compact_hash32_direct.offset);
        manifest_padding = image.subspan(
            manifest_end, direct_offset - manifest_end);
    }

    if (!minimizer_fragment_image_detail::all_zero(payload_padding)
        || !minimizer_fragment_image_detail::all_zero(directory_padding)
        || !minimizer_fragment_image_detail::all_zero(manifest_padding)) {
        return std::unexpected(minimizer_fragment_image_error::nonzero_section_padding);
    }
    if (crc64_ecma182(directory) != decoded.directory.crc64) {
        return std::unexpected(minimizer_fragment_image_error::directory_crc_mismatch);
    }
    if (crc64_ecma182(manifest) != decoded.manifest.crc64) {
        return std::unexpected(minimizer_fragment_image_error::manifest_crc_mismatch);
    }
    const auto generation_manifest =
        decode_minimizer_generation_manifest(manifest);
    if (!generation_manifest
        || generation_manifest->layout
            != minimizer_generation_layout::grouped_soa_segment_local) {
        return std::unexpected(
            minimizer_fragment_image_error::manifest_metadata_invalid);
    }
    if (generation_manifest->generation_id != decoded.generation_id
        || generation_manifest->source_identity
            != decoded.source_identity
        || generation_manifest->source_content_checksum
            != decoded.source_content_crc
        || generation_manifest->target_length != decoded.target_length
        || generation_manifest->position_segment_count
            != decoded.position_segment_count
        || generation_manifest->position_segment_span_bp
            != decoded.position_segment_span_bp
        || generation_manifest->payload_bytes != decoded.payload.bytes
        || generation_manifest->directory_bytes
            != decoded.directory.bytes
        || generation_manifest->directory_checksum
            != decoded.directory.crc64) {
        return std::unexpected(
            minimizer_fragment_image_error::generation_metadata_mismatch);
    }
    const auto profile = derive_grouped_soa_minimizer_profile(
        *generation_manifest);
    if (!profile) {
        return std::unexpected(
            minimizer_fragment_image_error::manifest_metadata_invalid);
    }
    const auto decoded_directory = grouped_soa_directory_view::decode(
        directory, *profile);
    if (!decoded_directory) {
        return std::unexpected(
            minimizer_fragment_image_error::directory_metadata_invalid);
    }
    const minimizer_generation_head generation_head{
        .layout = generation_manifest->layout,
        .generation_id = decoded.generation_id,
        .source_identity = decoded.source_identity,
        .storage_contract_checksum = decoded.storage_contract_checksum,
        .manifest_checksum = decoded.manifest.crc64,
        .source_content_checksum = decoded.source_content_crc,
    };
    if (!validate_minimizer_generation_metadata(
            generation_head,
            *generation_manifest,
            *decoded_directory)) {
        return std::unexpected(
            minimizer_fragment_image_error::generation_metadata_mismatch);
    }
    if (options.payload_crc_policy == minimizer_fragment_payload_crc_policy::validate
        && crc64_ecma182(payload) != decoded.payload.crc64) {
        return std::unexpected(minimizer_fragment_image_error::payload_crc_mismatch);
    }

    return validated_minimizer_fragment_image_view{
        .header = decoded,
        .payload = payload,
        .directory = directory,
        .manifest = manifest,
        .compact_hash32_direct = compact_hash32_direct,
        .header_crc_bytes = k_minimizer_fragment_image_header_bytes,
        .payload_crc_bytes =
            options.payload_crc_policy == minimizer_fragment_payload_crc_policy::validate
            ? decoded.payload.bytes : 0u,
        .directory_crc_bytes = decoded.directory.bytes,
        .manifest_crc_bytes = decoded.manifest.bytes,
    };
}

} // namespace bioseq::indexing
