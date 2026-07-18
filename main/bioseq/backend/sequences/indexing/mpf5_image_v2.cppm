/**
 * @file mpf5_image_v2.cppm
 * @brief DOC0383 section 10.2 — PG-free MPF5 v2 image codec and validator.
 *
 * The wire image is deliberately not a native C++ object. Every scalar is
 * encoded and decoded explicitly in little-endian order, the complete header
 * is authenticated with its checksum field treated as zero, and all range
 * arithmetic is checked before a span is constructed.  Legacy headers keep
 * bytes 192..4095 zero.  Feature-bearing headers use bytes 192..223 for the
 * feature bitmap and compact-H32 receipt and keep bytes 224..4095 reserved.
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

export module bioseq.indexing.mpf5_image_v2;

import bioseq.indexing.storage;
import bioseq.indexing.minimizer_hashmap;

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {

inline constexpr std::uint64_t kMpf5V2Magic = 0x32563546504d5342ULL;
inline constexpr std::uint32_t kMpf5V2Version = 2u;
inline constexpr std::uint32_t kMpf5V2HeaderBytes = 4096u;
inline constexpr std::uint8_t kMpf5V2IndexKind = 1u;
inline constexpr std::uint16_t kMpf5V2WireFormat = 5u;
inline constexpr std::uint64_t kMpf5V2SectionAlignment = 8u;
inline constexpr std::uint64_t kMpf5V2CompactH32DirectAlignment = 64u;
inline constexpr std::uint64_t kMpf5V2FeatureCompactH32Direct =
    std::uint64_t{1u} << 0u;
inline constexpr std::uint64_t kMpf5V2KnownFeatures =
    kMpf5V2FeatureCompactH32Direct;
inline constexpr std::size_t kMpf5V2HeaderCrcOffset = 184u;
inline constexpr std::size_t kMpf5V2HeaderCrcBytes = 8u;
inline constexpr std::uint64_t kMpf5V2DefaultMaxFileOffset = []() constexpr {
    static_assert(std::numeric_limits<::off_t>::is_integer);
    if constexpr (std::numeric_limits<::off_t>::digits
        >= std::numeric_limits<std::uint64_t>::digits) {
        return std::numeric_limits<std::uint64_t>::max();
    } else {
        return static_cast<std::uint64_t>(
            std::numeric_limits<::off_t>::max());
    }
}();
inline constexpr std::uint64_t kMpf5V2DefaultMaxMappedBytes = []() constexpr {
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
        return std::numeric_limits<std::uint64_t>::max();
    } else {
        return static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max());
    }
}();

enum class Mpf5V2Provider : std::uint8_t {
    postgres_relation = 0u,
    mapped_file = 1u,
};

static_assert(static_cast<std::uint8_t>(
                  Mpf5V2Provider::postgres_relation) == 0u);
static_assert(static_cast<std::uint8_t>(
                  Mpf5V2Provider::mapped_file) == 1u);

[[nodiscard]] constexpr bool is_valid_mpf5_v2_provider(
        std::uint8_t provider) noexcept {
    return provider
            == static_cast<std::uint8_t>(Mpf5V2Provider::postgres_relation)
        || provider == static_cast<std::uint8_t>(Mpf5V2Provider::mapped_file);
}

struct ImageSectionV2 {
    std::uint64_t offset = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t crc64 = 0u;

    friend constexpr bool operator==(
        const ImageSectionV2&, const ImageSectionV2&) = default;
};

/// Outer-image receipt for an optional compact-H32 direct image.  The nested
/// header checksum is produced and verified by the direct-image codec; MPF5
/// authenticates the receipt and the exact opaque section extent.
struct Mpf5V2CompactH32DirectSectionReceipt {
    std::uint64_t offset = 0u;
    std::uint64_t bytes = 0u;
    std::uint64_t direct_header_crc64 = 0u;

    friend constexpr bool operator==(
        const Mpf5V2CompactH32DirectSectionReceipt&,
        const Mpf5V2CompactH32DirectSectionReceipt&) = default;
};

/// Planning input for an opaque compact-H32 direct section.  Presence of the
/// optional value enables the corresponding MPF5 feature bit; a present
/// section must contain at least one byte.
struct Mpf5V2CompactH32DirectSectionSpec {
    std::uint64_t bytes = 0u;
    std::uint64_t direct_header_crc64 = 0u;

    friend constexpr bool operator==(
        const Mpf5V2CompactH32DirectSectionSpec&,
        const Mpf5V2CompactH32DirectSectionSpec&) = default;
};

struct DecodedMpf5HeaderV2 {
    std::uint64_t magic = 0u;
    std::uint32_t version = 0u;
    std::uint32_t header_bytes = 0u;
    std::uint32_t database_oid = 0u;
    std::uint32_t tablespace_oid = 0u;
    std::uint32_t main_relfilenode = 0u;
    std::uint32_t user_relid = 0u;
    std::uint16_t column_attno = 0u;
    std::uint8_t index_kind = 0u;
    std::uint8_t provider = 0u;
    std::uint16_t wire_format = 0u;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_crc = 0u;
    std::uint64_t compatibility_crc = 0u;
    std::uint64_t target_length = 0u;
    std::uint64_t file_bytes = 0u;
    std::uint32_t position_segment_count = 0u;
    std::uint64_t position_segment_span_bp = 0u;
    ImageSectionV2 payload{};
    ImageSectionV2 directory{};
    ImageSectionV2 manifest{};
    std::uint64_t header_crc64 = 0u;
    std::uint64_t feature_bitmap = 0u;
    Mpf5V2CompactH32DirectSectionReceipt compact_h32_direct{};

    friend constexpr bool operator==(
        const DecodedMpf5HeaderV2&, const DecodedMpf5HeaderV2&) = default;
};

using EncodedMpf5HeaderV2 =
    std::array<std::byte, static_cast<std::size_t>(kMpf5V2HeaderBytes)>;

struct Mpf5V2ImageLayout {
    ImageSectionV2 payload{};
    ImageSectionV2 directory{};
    ImageSectionV2 manifest{};
    std::uint64_t feature_bitmap = 0u;
    Mpf5V2CompactH32DirectSectionReceipt compact_h32_direct{};
    std::uint64_t file_bytes = 0u;

    friend constexpr bool operator==(
        const Mpf5V2ImageLayout&, const Mpf5V2ImageLayout&) = default;
};

/// The PG adapter may replace these defaults with its exact off_t/size_t
/// maxima. The defaults reflect this build's native off_t and size_t.
struct Mpf5V2AddressLimits {
    std::uint64_t max_file_offset = kMpf5V2DefaultMaxFileOffset;
    std::uint64_t max_mapped_bytes = kMpf5V2DefaultMaxMappedBytes;
};

struct Mpf5V2ExpectedIdentity {
    std::uint32_t database_oid = 0u;
    std::uint32_t tablespace_oid = 0u;
    std::uint32_t main_relfilenode = 0u;
    std::uint32_t user_relid = 0u;
    std::uint16_t column_attno = 0u;
    std::uint8_t index_kind = kMpf5V2IndexKind;
    std::uint8_t provider = 0u;
    std::uint16_t wire_format = kMpf5V2WireFormat;
    std::uint64_t query_key = 0u;
    std::uint64_t generation_id = 0u;
    std::uint64_t source_identity = 0u;
    std::uint64_t source_content_crc = 0u;
    std::uint64_t compatibility_crc = 0u;
    std::uint64_t target_length = 0u;
    std::uint64_t file_bytes = 0u;
    std::uint32_t position_segment_count = 0u;
    std::uint64_t position_segment_span_bp = 0u;
    ImageSectionV2 payload{};
    ImageSectionV2 directory{};
    ImageSectionV2 manifest{};
    std::uint64_t header_crc64 = 0u;
    /// Optional because the LIVE authority may pin only the complete outer
    /// header receipt.  When supplied, these fields are compared exactly.
    std::optional<std::uint64_t> feature_bitmap = std::nullopt;
    std::optional<Mpf5V2CompactH32DirectSectionReceipt>
        compact_h32_direct = std::nullopt;
};

enum class Mpf5V2PayloadCrcPolicy : std::uint8_t {
    /// Normal open: authenticate bounded metadata only. Payload extents remain
    /// lazy and payload.crc64 is a publication/offline-scrub receipt.
    deferred = 0u,
    /// Publication verification, explicit scrub, and fault tests only.
    validate = 1u,
};

struct Mpf5V2ValidationOptions {
    Mpf5V2AddressLimits address_limits{};
    Mpf5V2ExpectedIdentity expected_identity{};
    Mpf5V2PayloadCrcPolicy payload_crc_policy =
        Mpf5V2PayloadCrcPolicy::deferred;
};

enum class Mpf5V2WireError : std::uint8_t {
    header_size_mismatch,
    bad_magic,
    bad_version,
    bad_header_bytes,
    bad_index_kind,
    bad_provider,
    bad_wire_format,
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
    compact_h32_direct_feature_mismatch,
    compact_h32_direct_offset_mismatch,
    compact_h32_direct_end_mismatch,
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

struct ValidatedMpf5V2ImageView {
    DecodedMpf5HeaderV2 header{};
    std::span<const std::byte> payload{};
    std::span<const std::byte> directory{};
    std::span<const std::byte> manifest{};
    std::span<const std::byte> compact_h32_direct{};
    std::uint64_t header_crc_bytes = 0u;
    std::uint64_t payload_crc_bytes = 0u;
    std::uint64_t directory_crc_bytes = 0u;
    std::uint64_t manifest_crc_bytes = 0u;
};

[[nodiscard]] constexpr std::expected<Mpf5V2ImageLayout, Mpf5V2WireError>
plan_mpf5_v2_layout(
    std::uint64_t payload_bytes,
    std::uint64_t directory_bytes,
    std::uint64_t manifest_bytes,
    std::uint64_t payload_crc64 = 0u,
    std::uint64_t directory_crc64 = 0u,
    std::uint64_t manifest_crc64 = 0u,
    std::optional<Mpf5V2CompactH32DirectSectionSpec>
        compact_h32_direct = std::nullopt) noexcept;

[[nodiscard]] constexpr std::expected<void, Mpf5V2WireError>
validate_mpf5_v2_layout(
    const DecodedMpf5HeaderV2& header,
    std::uint64_t actual_file_bytes,
    Mpf5V2AddressLimits limits = {}) noexcept;

[[nodiscard]] constexpr std::expected<EncodedMpf5HeaderV2, Mpf5V2WireError>
encode_mpf5_v2_header(DecodedMpf5HeaderV2 header) noexcept;

[[nodiscard]] constexpr std::expected<DecodedMpf5HeaderV2, Mpf5V2WireError>
decode_mpf5_v2_header(std::span<const std::byte> encoded) noexcept;

/// Authenticate only the fixed outer header, complete layout, and expected
/// identity. The returned section spans are borrowed but no byte beyond the
/// 4 KiB header is read. Normal restart/timed opens use this entry point;
/// publication and explicit scrub retain `validate_mpf5_v2_image` below.
[[nodiscard]] inline std::expected<ValidatedMpf5V2ImageView, Mpf5V2WireError>
open_mpf5_v2_image_bounded(
    std::span<const std::byte> image,
    const Mpf5V2ValidationOptions& options);

[[nodiscard]] inline std::expected<ValidatedMpf5V2ImageView, Mpf5V2WireError>
validate_mpf5_v2_image(
    std::span<const std::byte> image,
    const Mpf5V2ValidationOptions& options);

} // namespace bioseq::indexing

namespace bioseq::indexing::mpf5_v2_detail {

template <class UInt>
requires (std::is_unsigned_v<UInt>)
constexpr void store_le(
        std::span<std::byte> encoded,
        std::size_t offset,
        UInt value) noexcept {
    for (std::size_t byte = 0u; byte < sizeof(UInt); ++byte) {
        encoded[offset + byte] = static_cast<std::byte>(value & UInt{0xffu});
        value >>= 8u;
    }
}

template <class UInt>
requires (std::is_unsigned_v<UInt>)
[[nodiscard]] constexpr UInt load_le(
        std::span<const std::byte> encoded,
        std::size_t offset) noexcept {
    UInt value = 0u;
    for (std::size_t byte = 0u; byte < sizeof(UInt); ++byte) {
        value |= static_cast<UInt>(
            std::to_integer<std::uint8_t>(encoded[offset + byte]))
            << (byte * 8u);
    }
    return value;
}

[[nodiscard]] constexpr std::expected<std::uint64_t, Mpf5V2WireError>
checked_add(std::uint64_t left, std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::unexpected(Mpf5V2WireError::arithmetic_overflow);
    }
    return left + right;
}

[[nodiscard]] constexpr std::expected<std::uint64_t, Mpf5V2WireError>
checked_align_up(
        std::uint64_t value,
        std::uint64_t alignment) noexcept {
    const std::uint64_t remainder = value & (alignment - 1u);
    const std::uint64_t padding = remainder == 0u
        ? 0u
        : alignment - remainder;
    return checked_add(value, padding);
}

static_assert((kMpf5V2SectionAlignment
    & (kMpf5V2SectionAlignment - 1u)) == 0u);
static_assert((kMpf5V2CompactH32DirectAlignment
    & (kMpf5V2CompactH32DirectAlignment - 1u)) == 0u);

[[nodiscard]] constexpr bool has_compact_h32_direct_feature(
        std::uint64_t feature_bitmap) noexcept {
    return (feature_bitmap & kMpf5V2FeatureCompactH32Direct) != 0u;
}

[[nodiscard]] constexpr bool direct_receipt_is_zero(
        const Mpf5V2CompactH32DirectSectionReceipt& receipt) noexcept {
    return receipt == Mpf5V2CompactH32DirectSectionReceipt{};
}

[[nodiscard]] constexpr bool all_zero(
        std::span<const std::byte> bytes) noexcept {
    for (const std::byte value : bytes) {
        if (value != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr std::uint64_t header_crc(
        std::span<const std::byte> encoded) noexcept {
    Crc64Ecma182 crc{};
    crc.update(encoded.first(kMpf5V2HeaderCrcOffset));
    for (std::size_t byte = 0u; byte < kMpf5V2HeaderCrcBytes; ++byte) {
        crc.update_byte(0u);
    }
    crc.update(encoded.subspan(
        kMpf5V2HeaderCrcOffset + kMpf5V2HeaderCrcBytes));
    return crc.checksum();
}

[[nodiscard]] constexpr bool identity_matches(
        const DecodedMpf5HeaderV2& header,
        const Mpf5V2ExpectedIdentity& expected) noexcept {
    return header.database_oid == expected.database_oid
        && header.tablespace_oid == expected.tablespace_oid
        && header.main_relfilenode == expected.main_relfilenode
        && header.user_relid == expected.user_relid
        && header.column_attno == expected.column_attno
        && header.index_kind == expected.index_kind
        && header.provider == expected.provider
        && header.wire_format == expected.wire_format
        && header.query_key == expected.query_key
        && header.generation_id == expected.generation_id
        && header.source_identity == expected.source_identity
        && header.source_content_crc == expected.source_content_crc
        && header.compatibility_crc == expected.compatibility_crc
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
        && (!expected.compact_h32_direct
            || header.compact_h32_direct == *expected.compact_h32_direct);
}

} // namespace bioseq::indexing::mpf5_v2_detail

export namespace bioseq::indexing {

[[nodiscard]] constexpr std::expected<Mpf5V2ImageLayout, Mpf5V2WireError>
plan_mpf5_v2_layout(
        std::uint64_t payload_bytes,
        std::uint64_t directory_bytes,
        std::uint64_t manifest_bytes,
        std::uint64_t payload_crc64,
        std::uint64_t directory_crc64,
        std::uint64_t manifest_crc64,
        std::optional<Mpf5V2CompactH32DirectSectionSpec>
            compact_h32_direct) noexcept {
    Mpf5V2ImageLayout layout{};
    layout.payload = ImageSectionV2{
        .offset = kMpf5V2HeaderBytes,
        .bytes = payload_bytes,
        .crc64 = payload_crc64,
    };

    const auto payload_end = mpf5_v2_detail::checked_add(
        layout.payload.offset, layout.payload.bytes);
    if (!payload_end) {
        return std::unexpected(payload_end.error());
    }
    const auto directory_offset =
        mpf5_v2_detail::checked_align_up(
            *payload_end, kMpf5V2SectionAlignment);
    if (!directory_offset) {
        return std::unexpected(directory_offset.error());
    }
    layout.directory = ImageSectionV2{
        .offset = *directory_offset,
        .bytes = directory_bytes,
        .crc64 = directory_crc64,
    };

    const auto directory_end = mpf5_v2_detail::checked_add(
        layout.directory.offset, layout.directory.bytes);
    if (!directory_end) {
        return std::unexpected(directory_end.error());
    }
    const auto manifest_offset =
        mpf5_v2_detail::checked_align_up(
            *directory_end, kMpf5V2SectionAlignment);
    if (!manifest_offset) {
        return std::unexpected(manifest_offset.error());
    }
    layout.manifest = ImageSectionV2{
        .offset = *manifest_offset,
        .bytes = manifest_bytes,
        .crc64 = manifest_crc64,
    };

    const auto manifest_end = mpf5_v2_detail::checked_add(
        layout.manifest.offset, layout.manifest.bytes);
    if (!manifest_end) {
        return std::unexpected(manifest_end.error());
    }
    if (!compact_h32_direct) {
        layout.file_bytes = *manifest_end;
        return layout;
    }
    if (compact_h32_direct->bytes == 0u) {
        return std::unexpected(
            Mpf5V2WireError::compact_h32_direct_feature_mismatch);
    }
    const auto direct_offset = mpf5_v2_detail::checked_align_up(
        *manifest_end, kMpf5V2CompactH32DirectAlignment);
    if (!direct_offset) {
        return std::unexpected(direct_offset.error());
    }
    layout.feature_bitmap = kMpf5V2FeatureCompactH32Direct;
    layout.compact_h32_direct = Mpf5V2CompactH32DirectSectionReceipt{
        .offset = *direct_offset,
        .bytes = compact_h32_direct->bytes,
        .direct_header_crc64 = compact_h32_direct->direct_header_crc64,
    };
    const auto direct_end = mpf5_v2_detail::checked_add(
        layout.compact_h32_direct.offset,
        layout.compact_h32_direct.bytes);
    if (!direct_end) {
        return std::unexpected(direct_end.error());
    }
    layout.file_bytes = *direct_end;
    return layout;
}

[[nodiscard]] constexpr std::expected<void, Mpf5V2WireError>
validate_mpf5_v2_layout(
        const DecodedMpf5HeaderV2& header,
        std::uint64_t actual_file_bytes,
        Mpf5V2AddressLimits limits) noexcept {
    limits.max_file_offset = std::min(
        limits.max_file_offset, kMpf5V2DefaultMaxFileOffset);
    limits.max_mapped_bytes = std::min(
        limits.max_mapped_bytes, kMpf5V2DefaultMaxMappedBytes);
    if (actual_file_bytes < kMpf5V2HeaderBytes) {
        return std::unexpected(Mpf5V2WireError::file_too_small);
    }
    if (header.file_bytes != actual_file_bytes) {
        return std::unexpected(Mpf5V2WireError::file_size_mismatch);
    }
    if (header.file_bytes > limits.max_file_offset) {
        return std::unexpected(
            Mpf5V2WireError::file_offset_not_representable);
    }
    if (header.file_bytes > limits.max_mapped_bytes) {
        return std::unexpected(
            Mpf5V2WireError::mapped_size_not_representable);
    }
    if (header.payload.offset != kMpf5V2HeaderBytes) {
        return std::unexpected(Mpf5V2WireError::payload_offset_mismatch);
    }

    const auto payload_end = mpf5_v2_detail::checked_add(
        header.payload.offset, header.payload.bytes);
    if (!payload_end) {
        return std::unexpected(payload_end.error());
    }
    const auto expected_directory_offset =
        mpf5_v2_detail::checked_align_up(
            *payload_end, kMpf5V2SectionAlignment);
    if (!expected_directory_offset) {
        return std::unexpected(expected_directory_offset.error());
    }
    if (header.directory.offset != *expected_directory_offset) {
        return std::unexpected(Mpf5V2WireError::directory_offset_mismatch);
    }

    const auto directory_end = mpf5_v2_detail::checked_add(
        header.directory.offset, header.directory.bytes);
    if (!directory_end) {
        return std::unexpected(directory_end.error());
    }
    const auto expected_manifest_offset =
        mpf5_v2_detail::checked_align_up(
            *directory_end, kMpf5V2SectionAlignment);
    if (!expected_manifest_offset) {
        return std::unexpected(expected_manifest_offset.error());
    }
    if (header.manifest.offset != *expected_manifest_offset) {
        return std::unexpected(Mpf5V2WireError::manifest_offset_mismatch);
    }

    const auto manifest_end = mpf5_v2_detail::checked_add(
        header.manifest.offset, header.manifest.bytes);
    if (!manifest_end) {
        return std::unexpected(manifest_end.error());
    }

    if ((header.feature_bitmap & ~kMpf5V2KnownFeatures) != 0u) {
        return std::unexpected(Mpf5V2WireError::unsupported_features);
    }
    if (!mpf5_v2_detail::has_compact_h32_direct_feature(
            header.feature_bitmap)) {
        if (!mpf5_v2_detail::direct_receipt_is_zero(
                header.compact_h32_direct)) {
            return std::unexpected(
                Mpf5V2WireError::compact_h32_direct_feature_mismatch);
        }
        if (*manifest_end != header.file_bytes) {
            return std::unexpected(Mpf5V2WireError::manifest_end_mismatch);
        }
        return {};
    }
    if (header.compact_h32_direct.bytes == 0u) {
        return std::unexpected(
            Mpf5V2WireError::compact_h32_direct_feature_mismatch);
    }
    const auto expected_direct_offset = mpf5_v2_detail::checked_align_up(
        *manifest_end, kMpf5V2CompactH32DirectAlignment);
    if (!expected_direct_offset) {
        return std::unexpected(expected_direct_offset.error());
    }
    if (header.compact_h32_direct.offset != *expected_direct_offset) {
        return std::unexpected(
            Mpf5V2WireError::compact_h32_direct_offset_mismatch);
    }
    const auto direct_end = mpf5_v2_detail::checked_add(
        header.compact_h32_direct.offset,
        header.compact_h32_direct.bytes);
    if (!direct_end) {
        return std::unexpected(direct_end.error());
    }
    if (*direct_end != header.file_bytes) {
        return std::unexpected(
            Mpf5V2WireError::compact_h32_direct_end_mismatch);
    }
    return {};
}

[[nodiscard]] constexpr std::expected<EncodedMpf5HeaderV2, Mpf5V2WireError>
encode_mpf5_v2_header(DecodedMpf5HeaderV2 header) noexcept {
    if (header.magic != kMpf5V2Magic) {
        return std::unexpected(Mpf5V2WireError::bad_magic);
    }
    if (header.version != kMpf5V2Version) {
        return std::unexpected(Mpf5V2WireError::bad_version);
    }
    if (header.header_bytes != kMpf5V2HeaderBytes) {
        return std::unexpected(Mpf5V2WireError::bad_header_bytes);
    }
    if (header.index_kind != kMpf5V2IndexKind) {
        return std::unexpected(Mpf5V2WireError::bad_index_kind);
    }
    if (!is_valid_mpf5_v2_provider(header.provider)) {
        return std::unexpected(Mpf5V2WireError::bad_provider);
    }
    if (header.wire_format != kMpf5V2WireFormat) {
        return std::unexpected(Mpf5V2WireError::bad_wire_format);
    }
    const auto valid_layout = validate_mpf5_v2_layout(
        header, header.file_bytes);
    if (!valid_layout) {
        return std::unexpected(valid_layout.error());
    }

    EncodedMpf5HeaderV2 encoded{};
    const std::span<std::byte> bytes{encoded};
    using mpf5_v2_detail::store_le;
    store_le(bytes, 0u, header.magic);
    store_le(bytes, 8u, header.version);
    store_le(bytes, 12u, header.header_bytes);
    store_le(bytes, 16u, header.database_oid);
    store_le(bytes, 20u, header.tablespace_oid);
    store_le(bytes, 24u, header.main_relfilenode);
    store_le(bytes, 28u, header.user_relid);
    store_le(bytes, 32u, header.column_attno);
    store_le(bytes, 34u, header.index_kind);
    store_le(bytes, 35u, header.provider);
    store_le(bytes, 36u, header.wire_format);
    store_le(bytes, 40u, header.query_key);
    store_le(bytes, 48u, header.generation_id);
    store_le(bytes, 56u, header.source_identity);
    store_le(bytes, 64u, header.source_content_crc);
    store_le(bytes, 72u, header.compatibility_crc);
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
    store_le(bytes, 200u, header.compact_h32_direct.offset);
    store_le(bytes, 208u, header.compact_h32_direct.bytes);
    store_le(
        bytes, 216u, header.compact_h32_direct.direct_header_crc64);
    // encoded is zero-initialized, including 184..191 during this checksum.
    header.header_crc64 = mpf5_v2_detail::header_crc(bytes);
    store_le(bytes, kMpf5V2HeaderCrcOffset, header.header_crc64);
    return encoded;
}

[[nodiscard]] constexpr std::expected<DecodedMpf5HeaderV2, Mpf5V2WireError>
decode_mpf5_v2_header(std::span<const std::byte> encoded) noexcept {
    if (encoded.size() != kMpf5V2HeaderBytes) {
        return std::unexpected(Mpf5V2WireError::header_size_mismatch);
    }
    using mpf5_v2_detail::load_le;
    DecodedMpf5HeaderV2 header{};
    header.magic = load_le<std::uint64_t>(encoded, 0u);
    header.version = load_le<std::uint32_t>(encoded, 8u);
    header.header_bytes = load_le<std::uint32_t>(encoded, 12u);
    header.database_oid = load_le<std::uint32_t>(encoded, 16u);
    header.tablespace_oid = load_le<std::uint32_t>(encoded, 20u);
    header.main_relfilenode = load_le<std::uint32_t>(encoded, 24u);
    header.user_relid = load_le<std::uint32_t>(encoded, 28u);
    header.column_attno = load_le<std::uint16_t>(encoded, 32u);
    header.index_kind = load_le<std::uint8_t>(encoded, 34u);
    header.provider = load_le<std::uint8_t>(encoded, 35u);
    header.wire_format = load_le<std::uint16_t>(encoded, 36u);
    header.query_key = load_le<std::uint64_t>(encoded, 40u);
    header.generation_id = load_le<std::uint64_t>(encoded, 48u);
    header.source_identity = load_le<std::uint64_t>(encoded, 56u);
    header.source_content_crc = load_le<std::uint64_t>(encoded, 64u);
    header.compatibility_crc = load_le<std::uint64_t>(encoded, 72u);
    header.target_length = load_le<std::uint64_t>(encoded, 80u);
    header.file_bytes = load_le<std::uint64_t>(encoded, 88u);
    header.position_segment_count = load_le<std::uint32_t>(encoded, 96u);
    header.position_segment_span_bp = load_le<std::uint64_t>(encoded, 104u);
    header.payload = ImageSectionV2{
        .offset = load_le<std::uint64_t>(encoded, 112u),
        .bytes = load_le<std::uint64_t>(encoded, 120u),
        .crc64 = load_le<std::uint64_t>(encoded, 128u),
    };
    header.directory = ImageSectionV2{
        .offset = load_le<std::uint64_t>(encoded, 136u),
        .bytes = load_le<std::uint64_t>(encoded, 144u),
        .crc64 = load_le<std::uint64_t>(encoded, 152u),
    };
    header.manifest = ImageSectionV2{
        .offset = load_le<std::uint64_t>(encoded, 160u),
        .bytes = load_le<std::uint64_t>(encoded, 168u),
        .crc64 = load_le<std::uint64_t>(encoded, 176u),
    };
    header.header_crc64 = load_le<std::uint64_t>(
        encoded, kMpf5V2HeaderCrcOffset);
    header.feature_bitmap = load_le<std::uint64_t>(encoded, 192u);
    header.compact_h32_direct = Mpf5V2CompactH32DirectSectionReceipt{
        .offset = load_le<std::uint64_t>(encoded, 200u),
        .bytes = load_le<std::uint64_t>(encoded, 208u),
        .direct_header_crc64 = load_le<std::uint64_t>(encoded, 216u),
    };

    if (header.magic != kMpf5V2Magic) {
        return std::unexpected(Mpf5V2WireError::bad_magic);
    }
    if (header.version != kMpf5V2Version) {
        return std::unexpected(Mpf5V2WireError::bad_version);
    }
    if (header.header_bytes != kMpf5V2HeaderBytes) {
        return std::unexpected(Mpf5V2WireError::bad_header_bytes);
    }
    if (header.index_kind != kMpf5V2IndexKind) {
        return std::unexpected(Mpf5V2WireError::bad_index_kind);
    }
    if (!is_valid_mpf5_v2_provider(header.provider)) {
        return std::unexpected(Mpf5V2WireError::bad_provider);
    }
    if (header.wire_format != kMpf5V2WireFormat) {
        return std::unexpected(Mpf5V2WireError::bad_wire_format);
    }
    if ((header.feature_bitmap & ~kMpf5V2KnownFeatures) != 0u) {
        return std::unexpected(Mpf5V2WireError::unsupported_features);
    }
    const bool has_direct =
        mpf5_v2_detail::has_compact_h32_direct_feature(
            header.feature_bitmap);
    if (has_direct != (header.compact_h32_direct.bytes != 0u)
        || (!has_direct
            && !mpf5_v2_detail::direct_receipt_is_zero(
                header.compact_h32_direct))) {
        return std::unexpected(
            Mpf5V2WireError::compact_h32_direct_feature_mismatch);
    }
    if (!mpf5_v2_detail::all_zero(encoded.subspan(38u, 2u))
        || !mpf5_v2_detail::all_zero(encoded.subspan(100u, 4u))
        || !mpf5_v2_detail::all_zero(encoded.subspan(224u))) {
        return std::unexpected(Mpf5V2WireError::nonzero_reserved);
    }
    if (header.header_crc64 != mpf5_v2_detail::header_crc(encoded)) {
        return std::unexpected(Mpf5V2WireError::header_crc_mismatch);
    }
    return header;
}

[[nodiscard]] inline std::expected<ValidatedMpf5V2ImageView, Mpf5V2WireError>
open_mpf5_v2_image_bounded(
        std::span<const std::byte> image,
        const Mpf5V2ValidationOptions& options) {
    if (image.size() < kMpf5V2HeaderBytes) {
        return std::unexpected(Mpf5V2WireError::file_too_small);
    }
    const auto decoded = decode_mpf5_v2_header(
        image.first(static_cast<std::size_t>(kMpf5V2HeaderBytes)));
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (image.size() > std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected(
                Mpf5V2WireError::mapped_size_not_representable);
        }
    }
    const std::uint64_t actual_file_bytes =
        static_cast<std::uint64_t>(image.size());
    const auto valid_layout = validate_mpf5_v2_layout(
        *decoded, actual_file_bytes, options.address_limits);
    if (!valid_layout) {
        return std::unexpected(valid_layout.error());
    }
    if (!mpf5_v2_detail::identity_matches(
            *decoded, options.expected_identity)) {
        return std::unexpected(Mpf5V2WireError::identity_mismatch);
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
    const auto compact_h32_direct =
        mpf5_v2_detail::has_compact_h32_direct_feature(
            decoded->feature_bitmap)
        ? image.subspan(
            static_cast<std::size_t>(
                decoded->compact_h32_direct.offset),
            static_cast<std::size_t>(
                decoded->compact_h32_direct.bytes))
        : std::span<const std::byte>{};

    return ValidatedMpf5V2ImageView{
        .header = *decoded,
        .payload = payload,
        .directory = directory,
        .manifest = manifest,
        .compact_h32_direct = compact_h32_direct,
        .header_crc_bytes = kMpf5V2HeaderBytes,
        .payload_crc_bytes = 0u,
        .directory_crc_bytes = 0u,
        .manifest_crc_bytes = 0u,
    };
}

[[nodiscard]] inline std::expected<ValidatedMpf5V2ImageView, Mpf5V2WireError>
validate_mpf5_v2_image(
        std::span<const std::byte> image,
        const Mpf5V2ValidationOptions& options) {
    const auto bounded = open_mpf5_v2_image_bounded(image, options);
    if (!bounded) return std::unexpected(bounded.error());
    const DecodedMpf5HeaderV2& decoded = bounded->header;
    const auto payload = bounded->payload;
    const auto directory = bounded->directory;
    const auto manifest = bounded->manifest;
    const auto compact_h32_direct = bounded->compact_h32_direct;
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
        mpf5_v2_detail::has_compact_h32_direct_feature(
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
            decoded.compact_h32_direct.offset);
        manifest_padding = image.subspan(
            manifest_end, direct_offset - manifest_end);
    }

    if (!mpf5_v2_detail::all_zero(payload_padding)
        || !mpf5_v2_detail::all_zero(directory_padding)
        || !mpf5_v2_detail::all_zero(manifest_padding)) {
        return std::unexpected(Mpf5V2WireError::nonzero_section_padding);
    }
    if (crc64_ecma182(directory) != decoded.directory.crc64) {
        return std::unexpected(Mpf5V2WireError::directory_crc_mismatch);
    }
    if (crc64_ecma182(manifest) != decoded.manifest.crc64) {
        return std::unexpected(Mpf5V2WireError::manifest_crc_mismatch);
    }
    const auto generation_manifest =
        decode_minimizer_generation_manifest(manifest);
    if (!generation_manifest
        || generation_manifest->wire_format
            != MinimizerGenerationWireFormat::
                grouped_shape_soa_segment_local) {
        return std::unexpected(
            Mpf5V2WireError::manifest_metadata_invalid);
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
            Mpf5V2WireError::generation_metadata_mismatch);
    }
    const auto profile = minimizer_generation_payload_profile(
        *generation_manifest);
    if (!profile) {
        return std::unexpected(
            Mpf5V2WireError::manifest_metadata_invalid);
    }
    const auto decoded_directory = DirView::decode(directory, *profile);
    if (!decoded_directory) {
        return std::unexpected(
            Mpf5V2WireError::directory_metadata_invalid);
    }
    const MinimizerGenerationHead generation_head{
        .wire_format = generation_manifest->wire_format,
        .generation_id = decoded.generation_id,
        .source_identity = decoded.source_identity,
        .compatibility_checksum = decoded.compatibility_crc,
        .manifest_checksum = decoded.manifest.crc64,
        .source_content_checksum = decoded.source_content_crc,
    };
    if (!validate_minimizer_generation_metadata(
            generation_head,
            *generation_manifest,
            *decoded_directory)) {
        return std::unexpected(
            Mpf5V2WireError::generation_metadata_mismatch);
    }
    if (options.payload_crc_policy == Mpf5V2PayloadCrcPolicy::validate
        && crc64_ecma182(payload) != decoded.payload.crc64) {
        return std::unexpected(Mpf5V2WireError::payload_crc_mismatch);
    }

    return ValidatedMpf5V2ImageView{
        .header = decoded,
        .payload = payload,
        .directory = directory,
        .manifest = manifest,
        .compact_h32_direct = compact_h32_direct,
        .header_crc_bytes = kMpf5V2HeaderBytes,
        .payload_crc_bytes =
            options.payload_crc_policy == Mpf5V2PayloadCrcPolicy::validate
            ? decoded.payload.bytes : 0u,
        .directory_crc_bytes = decoded.directory.bytes,
        .manifest_crc_bytes = decoded.manifest.bytes,
    };
}

} // namespace bioseq::indexing
