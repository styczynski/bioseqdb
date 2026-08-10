/**
 * @file bioseq_caching/core/column_format.cppm
 * @brief Policy-hashed little-endian columnar row-group writer and reader.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.cache.core:column_format;
import :checked_math;
import :wire;

export namespace bioseq::cache {

enum class ChecksumError : std::uint8_t {
    Unavailable,
    Failed,
};

template <class Policy>
concept column_checksum_policy = requires(
    Policy policy,
    std::span<const std::byte> bytes,
    std::span<const CacheDigest256> block_digests) {
    { policy.block_crc32c(bytes) }
        -> std::same_as<std::expected<std::uint32_t, ChecksumError>>;
    { policy.sha256(bytes) }
        -> std::same_as<std::expected<CacheDigest256, ChecksumError>>;
    { policy.manifest_sha256(block_digests) }
        -> std::same_as<std::expected<CacheDigest256, ChecksumError>>;
};

enum class FormatError : std::uint16_t {
    ArithmeticOverflow,
    InvalidSchema,
    TooManyColumns,
    ColumnCountMismatch,
    UnsupportedEncoding,
    InvalidFixedWidth,
    InvalidBoolean,
    InvalidAlignment,
    ChecksumUnavailable,
    BadMagic,
    UnsupportedVersion,
    EndianMismatch,
    AbiMismatch,
    IdentityMismatch,
    SchemaMismatch,
    Truncated,
    OffsetOutOfBounds,
    InvalidRowOrdinal,
    InvalidNullBitmap,
    InvalidOffsets,
    CorruptBlock,
    CorruptManifest,
    InvalidBlockReference,
    InvalidRowGroupLimits,
    RowGroupValueLimitExceeded,
    TooManyRowGroups,
    MetadataLimitExceeded,
    MaximumStoreBytesExceeded,
    PhysicalBlockLimitExceeded,
    SinkFailed,
    InvalidBuilderState,
};

inline constexpr std::uint16_t kColumnFlagNullable = 1u << 0;
inline constexpr std::size_t kColumnStoreHeaderBytes = 112;
inline constexpr std::size_t kRowGroupHeaderBytes = 40;
inline constexpr std::size_t kColumnChunkBytes = 64;
inline constexpr std::size_t kRowGroupIndexEntryBytes = 24;
inline constexpr std::size_t kFooterPrefixBytes = 16;
inline constexpr std::size_t kHeaderCrcOffset = 108;
inline constexpr std::size_t kGroupCrcOffset = 32;
inline constexpr std::uint64_t kMaximumColumnStoreBytes = 256ULL << 20U;
inline constexpr std::uint32_t kMaximumColumnStoreRowGroups = 65'536;
inline constexpr std::uint64_t kMaximumColumnStoreMetadataBytes = 8ULL << 20U;
inline constexpr std::uint64_t kMaximumColumnStorePhysicalBlockBytes =
    64ULL << 20U;
inline constexpr std::uint32_t kMaximumColumnStorePhysicalBlocks = 32;

enum class ColumnStoreBlockKind : std::uint8_t {
    Header = 0,
    Schema = 1,
    RowGroup = 2,
    Index = 3,
    Footer = 4,
};

struct ColumnSchema {
    std::string name;
    ColumnDescriptorV1 descriptor{};
    SemanticId128 semantic_id{};

    [[nodiscard]] bool operator==(const ColumnSchema&) const noexcept = default;
};

struct EncodedColumnValue {
    bool is_null = false;
    std::span<const std::byte> bytes{};
};

struct ColumnValueView {
    bool is_null = false;
    bool is_boolean = false;
    bool boolean_value = false;
    std::span<const std::byte> bytes{};
};

struct ColumnStoreIdentity {
    std::uint32_t postgres_major = 0;
    std::uint64_t boot_nonce = 0;
    std::uint32_t database_oid = 0;
    std::uint32_t owner_role_oid = 0;
    std::uint8_t datum_width = sizeof(std::uintptr_t);

    [[nodiscard]] bool operator==(
        const ColumnStoreIdentity&) const noexcept = default;
};

struct RowGroupLimits {
    std::uint32_t rows = kDefaultRowGroupRows;
    std::uint64_t value_bytes = kDefaultRowGroupValueBytes;
};

/** Exact final v1 size after all row-group bytes have been serialized. */
[[nodiscard]] inline constexpr std::expected<std::uint64_t, FormatError>
projected_column_store_bytes(
    std::uint64_t end_of_groups,
    std::uint64_t row_group_count) noexcept {
    if (row_group_count > kMaximumColumnStoreRowGroups) {
        return std::unexpected(FormatError::TooManyRowGroups);
    }
    const auto index_entries = checked_mul(
        row_group_count,
        static_cast<std::uint64_t>(kRowGroupIndexEntryBytes));
    const auto index_bytes = index_entries.has_value()
        ? checked_add(std::uint64_t{8}, *index_entries)
        : std::optional<std::uint64_t>{};
    const auto digest_count = checked_add(row_group_count, std::uint64_t{3});
    const auto digest_bytes = digest_count.has_value()
        ? checked_mul(*digest_count, std::uint64_t{32})
        : std::optional<std::uint64_t>{};
    const auto footer_bytes = digest_bytes.has_value()
        ? checked_add(
            static_cast<std::uint64_t>(kFooterPrefixBytes + 32u),
            *digest_bytes)
        : std::optional<std::uint64_t>{};
    const auto through_index = index_bytes.has_value()
        ? checked_add(end_of_groups, *index_bytes)
        : std::optional<std::uint64_t>{};
    const auto total = through_index.has_value() && footer_bytes.has_value()
        ? checked_add(*through_index, *footer_bytes)
        : std::optional<std::uint64_t>{};
    if (!total.has_value()) {
        return std::unexpected(FormatError::ArithmeticOverflow);
    }
    if (*total > kMaximumColumnStoreBytes) {
        return std::unexpected(FormatError::MaximumStoreBytesExceeded);
    }
    return *total;
}

/** Encode validity bits with one for non-NULL and deterministic zero tail bits. */
[[nodiscard]] inline std::vector<std::byte> encode_validity(
    std::span<const bool> valid) {
    std::vector<std::byte> bytes((valid.size() + 7u) / 8u, std::byte{0});
    for (std::size_t index = 0; index < valid.size(); ++index) {
        if (valid[index]) {
            bytes[index / 8u] |= static_cast<std::byte>(1u << (index % 8u));
        }
    }
    return bytes;
}

/** Support the standard packed-bool container used by boundary fixtures. */
[[nodiscard]] inline std::vector<std::byte> encode_validity(
    const std::vector<bool>& valid) {
    std::vector<std::byte> bytes((valid.size() + 7u) / 8u, std::byte{0});
    for (std::size_t index = 0; index < valid.size(); ++index) {
        if (valid[index]) {
            bytes[index / 8u] |= static_cast<std::byte>(1u << (index % 8u));
        }
    }
    return bytes;
}

/** Decode exactly the requested number of validity bits. */
[[nodiscard]] inline std::expected<std::vector<bool>, FormatError>
decode_validity(
    std::span<const std::byte> bytes,
    std::size_t row_count) {
    if (bytes.size() != (row_count + 7u) / 8u) {
        return std::unexpected(FormatError::InvalidNullBitmap);
    }
    const std::size_t used_tail_bits = row_count % 8u;
    if (used_tail_bits != 0 && !bytes.empty()) {
        const unsigned int unused_mask =
            static_cast<unsigned int>(0xffu << used_tail_bits);
        if ((std::to_integer<unsigned int>(bytes.back()) & unused_mask) != 0) {
            return std::unexpected(FormatError::InvalidNullBitmap);
        }
    }
    std::vector<bool> valid(row_count, false);
    for (std::size_t index = 0; index < row_count; ++index) {
        valid[index] =
            (std::to_integer<unsigned int>(bytes[index / 8u])
             & (1u << (index % 8u))) != 0;
    }
    return valid;
}

/** Verify that bits beyond the row count cannot destabilize checksums. */
[[nodiscard]] inline bool unused_tail_bits_are_zero(
    std::span<const std::byte> bytes,
    std::size_t row_count) noexcept {
    if (bytes.size() != (row_count + 7u) / 8u || bytes.empty()) {
        return bytes.empty() && row_count == 0;
    }
    const std::size_t used = row_count % 8u;
    if (used == 0) {
        return true;
    }
    const unsigned int mask = static_cast<unsigned int>(0xffu << used);
    return (std::to_integer<unsigned int>(bytes.back()) & mask) == 0;
}

/** Reject dictionary candidates that do not strictly reduce encoded bytes. */
[[nodiscard]] constexpr bool dictionary_is_beneficial(
    std::uint64_t plain_bytes,
    std::uint64_t dictionary_bytes,
    std::uint64_t index_bytes) noexcept {
    const auto encoded = checked_add(dictionary_bytes, index_bytes);
    return encoded.has_value() && *encoded < plain_bytes;
}

/** Validate a DSA/file reference before deriving a backing-store pointer. */
[[nodiscard]] constexpr bool validate_block_reference(
    const BlockReferenceV1& reference,
    std::uint64_t backing_size) noexcept {
    const bool known_tier = reference.tier == BlockBackingTierV1::Dsa
        || reference.tier == BlockBackingTierV1::File;
    return known_tier
        && reference.flags == 0
        && reference.reserved == 0
        && reference.reserved2 == 0
        && reference.handle_or_file_id != 0
        && reference.length != 0
        && checked_range(reference.offset, reference.length, backing_size);
}

namespace detail {

struct OwnedValue {
    bool is_null = false;
    std::vector<std::byte> bytes;
};

using OwnedRow = std::vector<OwnedValue>;

struct PendingGroup {
    std::uint64_t first_row_ordinal = 0;
    std::uint64_t value_bytes = 0;
    std::vector<OwnedRow> rows;
};

struct ParsedChunk {
    ColumnChunkV1 wire{};
};

struct ParsedGroup {
    std::uint64_t first_row_ordinal = 0;
    std::uint32_t row_count = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::vector<ParsedChunk> chunks;
};

struct GroupIndexEntry {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t crc32c = 0;
};

/** Overwrite a fixed-width integer already reserved in a byte vector. */
template <class Integer>
void write_little_endian_at(
    std::vector<std::byte>& destination,
    std::size_t offset,
    Integer value) {
    std::vector<std::byte> encoded;
    encoded.reserve(sizeof(Integer));
    append_little_endian(encoded, value);
    std::copy(encoded.begin(), encoded.end(), destination.begin() + offset);
}

/** Append zero padding to reach a checked power-of-two alignment. */
[[nodiscard]] inline std::expected<void, FormatError> align_vector(
    std::vector<std::byte>& bytes,
    std::uint64_t alignment,
    std::uint64_t base_offset = 0) {
    const auto absolute_offset = checked_add(
        base_offset, static_cast<std::uint64_t>(bytes.size()));
    if (!absolute_offset.has_value()) {
        return std::unexpected(FormatError::ArithmeticOverflow);
    }
    const auto aligned = checked_align_up(*absolute_offset, alignment);
    if (!aligned.has_value()) {
        return std::unexpected(FormatError::InvalidAlignment);
    }
    const std::uint64_t relative = *aligned - base_offset;
    if (relative > (std::numeric_limits<std::size_t>::max)()) {
        return std::unexpected(FormatError::ArithmeticOverflow);
    }
    bytes.resize(static_cast<std::size_t>(relative), std::byte{0});
    return {};
}

/** Return the dense or aligned-stride width for a fixed encoding. */
[[nodiscard]] inline std::expected<std::uint64_t, FormatError>
fixed_value_stride(const ColumnDescriptorV1& descriptor) noexcept {
    if (descriptor.type_length <= 0) {
        return std::unexpected(FormatError::InvalidFixedWidth);
    }
    const std::uint64_t width = static_cast<std::uint64_t>(
        descriptor.type_length);
    if (descriptor.encoding != ColumnEncoding::FixedByReference) {
        return width;
    }
    const auto stride = checked_align_up(width, descriptor.type_alignment);
    if (!stride.has_value()) {
        return std::unexpected(FormatError::InvalidAlignment);
    }
    return *stride;
}

/** Build the exact canonical descriptor block used for the schema digest. */
[[nodiscard]] inline std::expected<std::vector<std::byte>, FormatError>
serialize_schema(std::span<const ColumnSchema> schema) {
    if (schema.size() > kMaxProviderBoundaryColumns
        || schema.size() > (std::numeric_limits<std::uint16_t>::max)()) {
        return std::unexpected(FormatError::TooManyColumns);
    }
    std::vector<std::byte> bytes;
    append_little_endian(bytes, static_cast<std::uint32_t>(schema.size()));
    for (const ColumnSchema& column : schema) {
        const ColumnDescriptorV1& descriptor = column.descriptor;
        const bool supported_encoding =
            descriptor.encoding == ColumnEncoding::BooleanBits
            || descriptor.encoding == ColumnEncoding::FixedByValue
            || descriptor.encoding == ColumnEncoding::FixedByReference
            || descriptor.encoding == ColumnEncoding::FlatVarlena
            || descriptor.encoding == ColumnEncoding::BinarySend;
        const bool fixed_encoding =
            descriptor.encoding == ColumnEncoding::FixedByValue
            || descriptor.encoding == ColumnEncoding::FixedByReference;
        const bool supported_by_value_width = descriptor.type_length == 1
            || descriptor.type_length == 2
            || descriptor.type_length == 4
            || descriptor.type_length == 8;
        if (column.name.size() > kMaxCanonicalKeyBytes
            || descriptor.type_alignment == 0
            || (descriptor.type_alignment & (descriptor.type_alignment - 1))
                != 0
            || descriptor.by_value > 1
            || descriptor.reserved != 0
            || descriptor.name_offset != 0
            || (descriptor.flags & ~kColumnFlagNullable) != 0) {
            return std::unexpected(FormatError::InvalidSchema);
        }
        if (!supported_encoding) {
            return std::unexpected(
                descriptor.encoding == ColumnEncoding::Dictionary
                    || descriptor.encoding == ColumnEncoding::RunLength
                ? FormatError::UnsupportedEncoding
                : FormatError::InvalidSchema);
        }
        if ((fixed_encoding && descriptor.type_length <= 0)
            || (descriptor.encoding == ColumnEncoding::BooleanBits
                && (descriptor.type_length != 1 || descriptor.by_value != 1))
            || (descriptor.encoding == ColumnEncoding::FixedByValue
                && (descriptor.by_value != 1 || !supported_by_value_width))
            || (descriptor.encoding == ColumnEncoding::FixedByReference
                && descriptor.by_value != 0)
            || (descriptor.encoding == ColumnEncoding::FlatVarlena
                && (descriptor.by_value != 0
                    || descriptor.type_length != -1))
            || (descriptor.encoding == ColumnEncoding::BinarySend
                && (descriptor.send_function_oid == 0
                    || descriptor.receive_function_oid == 0))) {
            return std::unexpected(FormatError::InvalidFixedWidth);
        }
        const std::uint64_t name_offset = bytes.size() + 64u;
        append_little_endian(bytes, descriptor.type_oid);
        append_little_endian(bytes, descriptor.typmod);
        append_little_endian(bytes, descriptor.collation_oid);
        append_little_endian(bytes, descriptor.type_length);
        append_little_endian(bytes, descriptor.by_value);
        append_little_endian(bytes, descriptor.type_alignment);
        append_little_endian(bytes, descriptor.encoding);
        append_little_endian(bytes, descriptor.flags);
        append_little_endian(bytes, descriptor.send_function_oid);
        append_little_endian(bytes, descriptor.receive_function_oid);
        append_little_endian(bytes, descriptor.receive_type_io_param);
        append_little_endian(bytes, descriptor.reserved);
        append_little_endian(bytes, name_offset);
        append_little_endian(bytes, column.semantic_id.high);
        append_little_endian(bytes, column.semantic_id.low);
        append_little_endian(bytes, static_cast<std::uint32_t>(column.name.size()));
        for (const char character : column.name) {
            bytes.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(character)));
        }
        if (bytes.size() > kMaxCanonicalKeyBytes) {
            return std::unexpected(FormatError::InvalidSchema);
        }
    }
    return bytes;
}

/** Copy selected byte ranges into one canonical chunk-checksum input. */
[[nodiscard]] inline std::expected<std::vector<std::byte>, FormatError>
chunk_checksum_bytes(
    std::span<const std::byte> group,
    const ColumnChunkV1& chunk) {
    std::vector<std::byte> bytes;
    for (const auto [offset, length] : std::array{
             std::pair{chunk.null_bitmap_offset, chunk.null_bitmap_bytes},
             std::pair{chunk.offsets_offset, chunk.offsets_bytes},
             std::pair{chunk.values_offset, chunk.values_bytes}}) {
        if (length == 0) {
            continue;
        }
        const auto region = checked_subspan(group, offset, length);
        if (!region.has_value()) {
            return std::unexpected(FormatError::OffsetOutOfBounds);
        }
        bytes.insert(bytes.end(), region->begin(), region->end());
    }
    return bytes;
}

} // namespace detail

/** Return the canonical schema bytes used by writer and validator. */
[[nodiscard]] inline std::expected<std::vector<std::byte>, FormatError>
schema_canonical_bytes(std::span<const ColumnSchema> schema) {
    return detail::serialize_schema(schema);
}

/** Compute a provider-independent schema digest through the injected policy. */
template <column_checksum_policy Policy>
[[nodiscard]] std::expected<CacheDigest256, FormatError> schema_digest(
    std::span<const ColumnSchema> schema,
    Policy& policy) {
    auto bytes = schema_canonical_bytes(schema);
    if (!bytes.has_value()) {
        return std::unexpected(bytes.error());
    }
    auto digest = policy.sha256(*bytes);
    if (!digest.has_value()) {
        return std::unexpected(FormatError::ChecksumUnavailable);
    }
    return *digest;
}

template <class Sink>
concept column_store_block_sink = requires(
    Sink& sink,
    ColumnStoreBlockKind kind,
    std::uint64_t ordinal,
    std::span<const std::byte> bytes,
    const typename Sink::token_type& token) {
    typename Sink::token_type;
    requires std::movable<typename Sink::token_type>;
    { sink.store(kind, ordinal, bytes) }
        -> std::same_as<std::expected<typename Sink::token_type, FormatError>>;
    { sink.discard(token) } noexcept -> std::same_as<void>;
};

template <column_checksum_policy Policy, column_store_block_sink Sink>
class StreamingColumnStoreBuilder;

/**
 * Logical block order plus cleanup ownership after a streaming finalization.
 * A caller marks the blocks published only after its manifest owns every token.
 */
template <column_store_block_sink Sink>
class StreamedColumnStoreBlocks {
public:
    using token_type = typename Sink::token_type;

    StreamedColumnStoreBlocks() = default;

    StreamedColumnStoreBlocks(StreamedColumnStoreBlocks&& other) noexcept {
        move_from(other);
    }

    StreamedColumnStoreBlocks& operator=(
            StreamedColumnStoreBlocks&& other) noexcept {
        if (this != &other) {
            discard();
            move_from(other);
        }
        return *this;
    }

    StreamedColumnStoreBlocks(const StreamedColumnStoreBlocks&) = delete;
    StreamedColumnStoreBlocks& operator=(
        const StreamedColumnStoreBlocks&) = delete;

    ~StreamedColumnStoreBlocks() noexcept {
        discard();
    }

    [[nodiscard]] std::span<const token_type> tokens() const noexcept {
        return tokens_;
    }

    [[nodiscard]] std::uint64_t logical_bytes() const noexcept {
        return logical_bytes_;
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }

    [[nodiscard]] std::uint32_t row_group_count() const noexcept {
        return row_group_count_;
    }

    /** Transfer cleanup responsibility to an atomically published manifest. */
    void mark_published() noexcept {
        sink_ = nullptr;
    }

    /** Discard every still-temporary token exactly once in logical order. */
    void discard() noexcept {
        if (sink_ != nullptr) {
            for (const token_type& token : tokens_) {
                sink_->discard(token);
            }
        }
        sink_ = nullptr;
        tokens_.clear();
    }

private:
    template <column_checksum_policy, column_store_block_sink>
    friend class StreamingColumnStoreBuilder;

    StreamedColumnStoreBlocks(
        Sink* sink,
        std::vector<token_type> tokens,
        std::uint64_t logical_bytes,
        std::uint64_t row_count,
        std::uint32_t row_group_count) noexcept
        : sink_(sink),
          tokens_(std::move(tokens)),
          logical_bytes_(logical_bytes),
          row_count_(row_count),
          row_group_count_(row_group_count) {}

    void move_from(StreamedColumnStoreBlocks& other) noexcept {
        sink_ = std::exchange(other.sink_, nullptr);
        tokens_ = std::move(other.tokens_);
        logical_bytes_ = std::exchange(other.logical_bytes_, 0);
        row_count_ = std::exchange(other.row_count_, 0);
        row_group_count_ = std::exchange(other.row_group_count_, 0);
    }

    Sink* sink_ = nullptr;
    std::vector<token_type> tokens_;
    std::uint64_t logical_bytes_ = 0;
    std::uint64_t row_count_ = 0;
    std::uint32_t row_group_count_ = 0;
};

/** Zone order used when physical blocks are assembled into one manifest. */
enum class ColumnStorePhysicalZone : std::uint8_t {
    Prefix = 0,
    RowGroups = 1,
    Suffix = 2,
};

/** A sink-local handle for one logical v1 block. */
struct ColumnStoreLogicalBlockToken {
    std::uint32_t slot = (std::numeric_limits<std::uint32_t>::max)();

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot != (std::numeric_limits<std::uint32_t>::max)();
    }

    [[nodiscard]] bool operator==(
        const ColumnStoreLogicalBlockToken&) const noexcept = default;
};

template <class Sink>
concept column_store_physical_sink = requires(
    Sink& sink,
    std::span<const std::byte> bytes,
    const typename Sink::token_type& token) {
    typename Sink::token_type;
    requires std::copyable<typename Sink::token_type>;
    { sink.store_physical(bytes) }
        -> std::same_as<std::expected<typename Sink::token_type, FormatError>>;
    { sink.discard_physical(token) } noexcept -> std::same_as<void>;
};

/**
 * Coalesce logical v1 blocks into bounded physical blocks without changing
 * their manifest order.  Row groups are written first, while the later-created
 * prefix and suffix tokens are returned in prefix/group/suffix order.
 */
template <column_store_physical_sink PhysicalSink>
class CoalescingColumnStoreSink {
public:
    using token_type = ColumnStoreLogicalBlockToken;
    using physical_token_type = typename PhysicalSink::token_type;

    explicit CoalescingColumnStoreSink(
        PhysicalSink& physical_sink,
        std::uint64_t maximum_physical_block_bytes =
            kMaximumColumnStorePhysicalBlockBytes,
        std::uint32_t maximum_physical_blocks =
            kMaximumColumnStorePhysicalBlocks) noexcept
        : physical_sink_(&physical_sink),
          maximum_physical_block_bytes_(maximum_physical_block_bytes),
          maximum_physical_blocks_(maximum_physical_blocks) {
        if (maximum_physical_block_bytes_ == 0 ||
            maximum_physical_block_bytes_ >
                kMaximumColumnStorePhysicalBlockBytes ||
            maximum_physical_block_bytes_ >
                (std::numeric_limits<std::size_t>::max)() ||
            maximum_physical_blocks_ == 0 ||
            maximum_physical_blocks_ >
                kMaximumColumnStorePhysicalBlocks) {
            initial_error_ = FormatError::InvalidBuilderState;
            return;
        }
        const auto physical_metadata = checked_mul(
            static_cast<std::uint64_t>(maximum_physical_blocks_),
            static_cast<std::uint64_t>(sizeof(PhysicalRecord)));
        if (!physical_metadata.has_value() ||
            *physical_metadata > kMaximumColumnStoreMetadataBytes) {
            initial_error_ = FormatError::MetadataLimitExceeded;
        }
    }

    CoalescingColumnStoreSink(const CoalescingColumnStoreSink&) = delete;
    CoalescingColumnStoreSink& operator=(
        const CoalescingColumnStoreSink&) = delete;
    CoalescingColumnStoreSink(CoalescingColumnStoreSink&&) = delete;
    CoalescingColumnStoreSink& operator=(
        CoalescingColumnStoreSink&&) = delete;

    ~CoalescingColumnStoreSink() noexcept {
        cleanup_physical();
    }

    [[nodiscard]] std::expected<token_type, FormatError> store(
        ColumnStoreBlockKind kind,
        std::uint64_t ordinal,
        std::span<const std::byte> bytes) {
        if (initial_error_.has_value()) {
            return std::unexpected(*initial_error_);
        }
        if (state_ == State::Failed) {
            return std::unexpected(last_error_);
        }
        if (!valid_next_block(kind, ordinal)) {
            return fail_token(FormatError::InvalidBuilderState);
        }
        const auto capacity = ensure_logical_capacity(records_.size() + 1u);
        if (!capacity.has_value()) {
            return fail_token(capacity.error());
        }

        switch (kind) {
        case ColumnStoreBlockKind::RowGroup: {
            const auto appended = append_bytes(
                bytes, ColumnStorePhysicalZone::RowGroups);
            if (!appended.has_value()) {
                return std::unexpected(appended.error());
            }
            break;
        }
        case ColumnStoreBlockKind::Header: {
            const auto flushed = flush_buffer(
                ColumnStorePhysicalZone::RowGroups);
            if (!flushed.has_value()) {
                return std::unexpected(flushed.error());
            }
            state_ = State::Prefix;
            const auto appended = append_bytes(
                bytes, ColumnStorePhysicalZone::Prefix);
            if (!appended.has_value()) {
                return std::unexpected(appended.error());
            }
            break;
        }
        case ColumnStoreBlockKind::Schema: {
            const auto appended = append_bytes(
                bytes, ColumnStorePhysicalZone::Prefix);
            if (!appended.has_value()) {
                return std::unexpected(appended.error());
            }
            break;
        }
        case ColumnStoreBlockKind::Index: {
            const auto flushed = flush_buffer(
                ColumnStorePhysicalZone::Prefix);
            if (!flushed.has_value()) {
                return std::unexpected(flushed.error());
            }
            state_ = State::Suffix;
            const auto appended = append_bytes(
                bytes, ColumnStorePhysicalZone::Suffix);
            if (!appended.has_value()) {
                return std::unexpected(appended.error());
            }
            break;
        }
        case ColumnStoreBlockKind::Footer: {
            const auto appended = append_bytes(
                bytes, ColumnStorePhysicalZone::Suffix);
            if (!appended.has_value()) {
                return std::unexpected(appended.error());
            }
            const auto flushed = flush_buffer(
                ColumnStorePhysicalZone::Suffix);
            if (!flushed.has_value()) {
                return std::unexpected(flushed.error());
            }
            state_ = State::Complete;
            break;
        }
        }

        const std::uint32_t slot = static_cast<std::uint32_t>(records_.size());
        records_.push_back(LogicalRecord{
            .kind = kind,
            .discarded = false,
            .ordinal = ordinal,
        });
        if (kind == ColumnStoreBlockKind::RowGroup) {
            ++row_group_count_;
        }
        return token_type{.slot = slot};
    }

    /** Release logical ownership; the final release frees physical tokens. */
    void discard(const token_type& token) noexcept {
        if (token.slot >= records_.size()) {
            return;
        }
        LogicalRecord& record = records_[token.slot];
        if (record.discarded) {
            return;
        }
        record.discarded = true;
        ++discarded_record_count_;
        if (discarded_record_count_ == records_.size() &&
            state_ != State::Published) {
            cleanup_physical();
        }
    }

    /** Copy the bounded physical capabilities in exact manifest byte order. */
    [[nodiscard]] std::expected<std::vector<physical_token_type>, FormatError>
    manifest_tokens(std::span<const token_type> logical_order) const {
        if (state_ != State::Complete ||
            !valid_logical_order(logical_order) ||
            physical_records_.size() > maximum_physical_blocks_) {
            return std::unexpected(
                physical_records_.size() > maximum_physical_blocks_
                    ? FormatError::PhysicalBlockLimitExceeded
                    : FormatError::InvalidBuilderState);
        }
        std::vector<physical_token_type> tokens;
        tokens.reserve(physical_records_.size());
        for (const ColumnStorePhysicalZone zone : {
                 ColumnStorePhysicalZone::Prefix,
                 ColumnStorePhysicalZone::RowGroups,
                 ColumnStorePhysicalZone::Suffix}) {
            for (const PhysicalRecord& record : physical_records_) {
                if (record.zone == zone) {
                    tokens.push_back(record.token);
                }
            }
        }
        if (tokens.empty() || tokens.size() > maximum_physical_blocks_) {
            return std::unexpected(FormatError::PhysicalBlockLimitExceeded);
        }
        return tokens;
    }

    /** Stop cleanup only after the copied capabilities were atomically owned. */
    void mark_published() noexcept {
        state_ = State::Published;
        physical_records_.clear();
        buffer_.clear();
    }

    [[nodiscard]] std::uint32_t physical_block_count() const noexcept {
        return static_cast<std::uint32_t>(physical_records_.size());
    }

    [[nodiscard]] std::uint64_t retained_buffer_bytes() const noexcept {
        return buffer_.size();
    }

    [[nodiscard]] std::uint64_t retained_metadata_bytes() const noexcept {
        const auto logical = checked_mul(
            static_cast<std::uint64_t>(records_.capacity()),
            static_cast<std::uint64_t>(sizeof(LogicalRecord)));
        const auto physical = checked_mul(
            static_cast<std::uint64_t>(physical_records_.capacity()),
            static_cast<std::uint64_t>(sizeof(PhysicalRecord)));
        if (!logical.has_value() || !physical.has_value()) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return checked_add(*logical, *physical).value_or(
            (std::numeric_limits<std::uint64_t>::max)());
    }

private:
    enum class State : std::uint8_t {
        RowGroups,
        Prefix,
        Suffix,
        Complete,
        Failed,
        Published,
    };

    struct LogicalRecord {
        ColumnStoreBlockKind kind = ColumnStoreBlockKind::Header;
        bool discarded = false;
        std::uint16_t reserved = 0;
        std::uint32_t reserved2 = 0;
        std::uint64_t ordinal = 0;
    };

    struct PhysicalRecord {
        physical_token_type token;
        ColumnStorePhysicalZone zone = ColumnStorePhysicalZone::RowGroups;
    };

    [[nodiscard]] bool valid_next_block(
            ColumnStoreBlockKind kind,
            std::uint64_t ordinal) const noexcept {
        switch (kind) {
        case ColumnStoreBlockKind::RowGroup:
            return state_ == State::RowGroups &&
                ordinal == row_group_count_ &&
                row_group_count_ < kMaximumColumnStoreRowGroups;
        case ColumnStoreBlockKind::Header:
            return state_ == State::RowGroups && ordinal == 0;
        case ColumnStoreBlockKind::Schema:
            return state_ == State::Prefix && ordinal == 0 &&
                !records_.empty() &&
                records_.back().kind == ColumnStoreBlockKind::Header;
        case ColumnStoreBlockKind::Index:
            return state_ == State::Prefix && ordinal == 0 &&
                !records_.empty() &&
                records_.back().kind == ColumnStoreBlockKind::Schema;
        case ColumnStoreBlockKind::Footer:
            return state_ == State::Suffix && ordinal == 0 &&
                !records_.empty() &&
                records_.back().kind == ColumnStoreBlockKind::Index;
        }
        return false;
    }

    [[nodiscard]] std::expected<void, FormatError>
    ensure_logical_capacity(std::size_t logical_count) {
        constexpr std::size_t maximum_logical_blocks =
            static_cast<std::size_t>(kMaximumColumnStoreRowGroups) + 4u;
        if (logical_count > maximum_logical_blocks) {
            return std::unexpected(FormatError::TooManyRowGroups);
        }
        std::size_t target = records_.capacity();
        if (target < logical_count) {
            target = target == 0 ? 16u : target;
            while (target < logical_count) {
                if (target > maximum_logical_blocks / 2u) {
                    target = maximum_logical_blocks;
                    break;
                }
                target *= 2u;
            }
        }
        const auto logical_bytes = checked_mul(
            static_cast<std::uint64_t>(target),
            static_cast<std::uint64_t>(sizeof(LogicalRecord)));
        const auto physical_bytes = checked_mul(
            static_cast<std::uint64_t>(maximum_physical_blocks_),
            static_cast<std::uint64_t>(sizeof(PhysicalRecord)));
        const auto total = logical_bytes.has_value() &&
                physical_bytes.has_value()
            ? checked_add(*logical_bytes, *physical_bytes)
            : std::optional<std::uint64_t>{};
        if (!total.has_value() ||
            *total > kMaximumColumnStoreMetadataBytes) {
            return std::unexpected(FormatError::MetadataLimitExceeded);
        }
        records_.reserve(target);
        physical_records_.reserve(maximum_physical_blocks_);
        return {};
    }

    [[nodiscard]] std::expected<void, FormatError> append_bytes(
        std::span<const std::byte> bytes,
        ColumnStorePhysicalZone zone) {
        while (!bytes.empty()) {
            const std::size_t limit = static_cast<std::size_t>(
                maximum_physical_block_bytes_);
            const std::size_t available = limit - buffer_.size();
            const std::size_t take = std::min(available, bytes.size());
            buffer_.insert(
                buffer_.end(), bytes.begin(), bytes.begin() + take);
            bytes = bytes.subspan(take);
            if (buffer_.size() == limit) {
                const auto flushed = flush_buffer(zone);
                if (!flushed.has_value()) {
                    return flushed;
                }
            }
        }
        return {};
    }

    [[nodiscard]] std::expected<void, FormatError> flush_buffer(
        ColumnStorePhysicalZone zone) {
        if (buffer_.empty()) {
            return {};
        }
        if (physical_records_.size() >= maximum_physical_blocks_) {
            return fail_void(FormatError::PhysicalBlockLimitExceeded);
        }
        auto stored = physical_sink_->store_physical(buffer_);
        if (!stored.has_value()) {
            return fail_void(FormatError::SinkFailed);
        }
        physical_records_.push_back(PhysicalRecord{
            .token = std::move(*stored),
            .zone = zone,
        });
        buffer_.clear();
        return {};
    }

    [[nodiscard]] bool valid_logical_order(
            std::span<const token_type> order) const noexcept {
        const std::size_t groups = row_group_count_;
        if (records_.size() != groups + 4u ||
            order.size() != records_.size() ||
            order[0].slot != groups ||
            order[1].slot != groups + 1u ||
            order[groups + 2u].slot != groups + 2u ||
            order[groups + 3u].slot != groups + 3u) {
            return false;
        }
        for (std::size_t index = 0; index < groups; ++index) {
            if (order[index + 2u].slot != index ||
                records_[index].kind != ColumnStoreBlockKind::RowGroup ||
                records_[index].ordinal != index ||
                records_[index].discarded) {
                return false;
            }
        }
        return records_[groups].kind == ColumnStoreBlockKind::Header &&
            records_[groups + 1u].kind == ColumnStoreBlockKind::Schema &&
            records_[groups + 2u].kind == ColumnStoreBlockKind::Index &&
            records_[groups + 3u].kind == ColumnStoreBlockKind::Footer &&
            !records_[groups].discarded &&
            !records_[groups + 1u].discarded &&
            !records_[groups + 2u].discarded &&
            !records_[groups + 3u].discarded;
    }

    [[nodiscard]] std::expected<void, FormatError> fail_void(
            FormatError error) noexcept {
        last_error_ = error;
        state_ = State::Failed;
        cleanup_physical();
        return std::unexpected(error);
    }

    [[nodiscard]] std::expected<token_type, FormatError> fail_token(
            FormatError error) noexcept {
        static_cast<void>(fail_void(error));
        return std::unexpected(error);
    }

    void cleanup_physical() noexcept {
        if (physical_sink_ != nullptr && state_ != State::Published) {
            for (const PhysicalRecord& record : physical_records_) {
                physical_sink_->discard_physical(record.token);
            }
        }
        physical_records_.clear();
        buffer_.clear();
    }

    PhysicalSink* physical_sink_ = nullptr;
    std::uint64_t maximum_physical_block_bytes_ = 0;
    std::uint32_t maximum_physical_blocks_ = 0;
    std::optional<FormatError> initial_error_;
    FormatError last_error_ = FormatError::InvalidBuilderState;
    State state_ = State::RowGroups;
    std::vector<std::byte> buffer_;
    std::vector<LogicalRecord> records_;
    std::vector<PhysicalRecord> physical_records_;
    std::uint32_t row_group_count_ = 0;
    std::size_t discarded_record_count_ = 0;
};

/** Incremental row API; sealed output is one self-validating byte blob. */
class ColumnStoreBuilder {
public:
    ColumnStoreBuilder(
        ColumnStoreIdentity identity,
        std::span<const ColumnSchema> schema,
        RowGroupLimits limits = {})
        : identity_(identity),
          schema_(schema.begin(), schema.end()),
          limits_(limits) {
        if (limits_.rows == 0) {
            limits_.rows = kDefaultRowGroupRows;
        }
        if (limits_.value_bytes == 0) {
            limits_.value_bytes = kDefaultRowGroupValueBytes;
        }
    }

    /** Copy one raw encoded row; no caller memory survives this call. */
    [[nodiscard]] std::expected<void, FormatError> append_row(
        std::span<const EncodedColumnValue> values) {
        if (values.size() != schema_.size()) {
            return std::unexpected(FormatError::ColumnCountMismatch);
        }
        std::uint64_t row_bytes = 0;
        detail::OwnedRow row;
        row.reserve(values.size());
        for (std::size_t column_index = 0;
             column_index < values.size();
             ++column_index) {
            const ColumnSchema& schema = schema_[column_index];
            const EncodedColumnValue& value = values[column_index];
            const ColumnEncoding encoding = schema.descriptor.encoding;
            if (value.is_null
                && (schema.descriptor.flags & kColumnFlagNullable) == 0) {
                return std::unexpected(FormatError::InvalidNullBitmap);
            }
            if (encoding == ColumnEncoding::Dictionary
                || encoding == ColumnEncoding::RunLength) {
                return std::unexpected(FormatError::UnsupportedEncoding);
            }
            if (!value.is_null) {
                if ((encoding == ColumnEncoding::FixedByValue
                     || encoding == ColumnEncoding::FixedByReference)
                    && (schema.descriptor.type_length <= 0
                        || value.bytes.size()
                            != static_cast<std::size_t>(
                                schema.descriptor.type_length))) {
                    return std::unexpected(FormatError::InvalidFixedWidth);
                }
                if (encoding == ColumnEncoding::BooleanBits
                    && (value.bytes.size() != 1
                        || (value.bytes[0] != std::byte{0}
                            && value.bytes[0] != std::byte{1}))) {
                    return std::unexpected(FormatError::InvalidBoolean);
                }
            }
            const auto next_bytes = checked_add(
                row_bytes, static_cast<std::uint64_t>(value.bytes.size()));
            if (!next_bytes.has_value()) {
                return std::unexpected(FormatError::ArithmeticOverflow);
            }
            row_bytes = *next_bytes;
            row.push_back(detail::OwnedValue{
                .is_null = value.is_null,
                .bytes = value.is_null
                    ? std::vector<std::byte>{}
                    : std::vector<std::byte>(
                        value.bytes.begin(), value.bytes.end()),
            });
        }
        bool group_value_limit_exceeded = false;
        if (!groups_.empty() && !groups_.back().rows.empty()) {
            const auto next_group_bytes = checked_add(
                groups_.back().value_bytes, row_bytes);
            group_value_limit_exceeded = !next_group_bytes.has_value()
                || *next_group_bytes > limits_.value_bytes;
        }
        if (groups_.empty()
            || groups_.back().rows.size() >= limits_.rows
            || group_value_limit_exceeded) {
            groups_.push_back(detail::PendingGroup{
                .first_row_ordinal = row_count_,
            });
        }
        groups_.back().rows.push_back(std::move(row));
        const auto next_group_bytes = checked_add(
            groups_.back().value_bytes, row_bytes);
        const auto next_row_count = checked_add(row_count_, std::uint64_t{1});
        if (!next_group_bytes.has_value() || !next_row_count.has_value()) {
            groups_.back().rows.pop_back();
            if (groups_.back().rows.empty()) {
                groups_.pop_back();
            }
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        groups_.back().value_bytes = *next_group_bytes;
        row_count_ = *next_row_count;
        return {};
    }

    /** Serialize all row groups, checksums, index, and manifest. */
    template <column_checksum_policy Policy>
    [[nodiscard]] std::expected<std::vector<std::byte>, FormatError> seal(
        Policy& policy) const {
        if (groups_.size() > kMaximumColumnStoreRowGroups) {
            return std::unexpected(FormatError::TooManyRowGroups);
        }
        auto descriptors = detail::serialize_schema(schema_);
        if (!descriptors.has_value()) {
            return std::unexpected(descriptors.error());
        }
        auto schema_hash = policy.sha256(*descriptors);
        if (!schema_hash.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }

        std::vector<std::byte> store(kColumnStoreHeaderBytes, std::byte{0});
        const std::uint64_t descriptor_offset = store.size();
        store.insert(store.end(), descriptors->begin(), descriptors->end());
        const auto empty_projection = projected_column_store_bytes(
            static_cast<std::uint64_t>(store.size()), groups_.size());
        if (!empty_projection.has_value()) {
            return std::unexpected(empty_projection.error());
        }

        std::vector<detail::GroupIndexEntry> group_index;
        group_index.reserve(groups_.size());
        for (const detail::PendingGroup& group : groups_) {
            auto encoded = serialize_group(
                group, static_cast<std::uint64_t>(store.size()), policy);
            if (!encoded.has_value()) {
                return std::unexpected(encoded.error());
            }
            auto group_crc = policy.block_crc32c(*encoded);
            if (!group_crc.has_value()) {
                return std::unexpected(FormatError::ChecksumUnavailable);
            }
            const std::uint64_t offset = store.size();
            const auto group_end = checked_add(
                offset, static_cast<std::uint64_t>(encoded->size()));
            if (!group_end.has_value()) {
                return std::unexpected(FormatError::ArithmeticOverflow);
            }
            const auto projected = projected_column_store_bytes(
                *group_end, groups_.size());
            if (!projected.has_value()) {
                return std::unexpected(projected.error());
            }
            store.insert(store.end(), encoded->begin(), encoded->end());
            group_index.push_back(detail::GroupIndexEntry{
                .offset = offset,
                .length = encoded->size(),
                .crc32c = *group_crc,
            });
        }

        const std::uint64_t index_offset = store.size();
        std::vector<std::byte> index_bytes;
        append_little_endian(
            index_bytes, static_cast<std::uint64_t>(group_index.size()));
        for (const auto& group : group_index) {
            append_little_endian(index_bytes, group.offset);
            append_little_endian(index_bytes, group.length);
            append_little_endian(index_bytes, group.crc32c);
            append_little_endian(index_bytes, std::uint32_t{0});
        }
        store.insert(store.end(), index_bytes.begin(), index_bytes.end());

        std::vector<std::byte> header;
        header.reserve(kColumnStoreHeaderBytes);
        for (const char byte : kColumnMagic) {
            header.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(byte)));
        }
        append_little_endian(header, kColumnFormatVersion);
        append_little_endian(header, identity_.postgres_major);
        append_little_endian(header, kSharedAbiVersion);
        append_little_endian(header, std::uint32_t{0});
        append_little_endian(header, identity_.boot_nonce);
        append_little_endian(header, identity_.database_oid);
        append_little_endian(header, identity_.owner_role_oid);
        append_little_endian(header, static_cast<std::uint8_t>(DataEndian::Little));
        append_little_endian(header, identity_.datum_width);
        append_little_endian(header, static_cast<std::uint16_t>(schema_.size()));
        append_little_endian(header, row_count_);
        append_little_endian(header, static_cast<std::uint64_t>(groups_.size()));
        append_little_endian(header, descriptor_offset);
        append_little_endian(header, index_offset);
        for (const std::uint8_t byte : schema_hash->bytes) {
            header.push_back(static_cast<std::byte>(byte));
        }
        append_little_endian(header, std::uint32_t{0});
        if (header.size() != kColumnStoreHeaderBytes) {
            return std::unexpected(FormatError::InvalidSchema);
        }
        auto header_crc = policy.block_crc32c(header);
        if (!header_crc.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        detail::write_little_endian_at(header, kHeaderCrcOffset, *header_crc);
        std::copy(header.begin(), header.end(), store.begin());

        std::vector<CacheDigest256> block_digests;
        block_digests.reserve(groups_.size() + 3u);
        const auto append_digest = [&policy, &block_digests](
                std::span<const std::byte> block)
                -> std::expected<void, FormatError> {
            auto digest = policy.sha256(block);
            if (!digest.has_value()) {
                return std::unexpected(FormatError::ChecksumUnavailable);
            }
            block_digests.push_back(*digest);
            return {};
        };
        if (!append_digest(std::span<const std::byte>(store).first(
                kColumnStoreHeaderBytes))) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        const std::uint64_t first_group_offset = group_index.empty()
            ? index_offset
            : group_index.front().offset;
        if (!append_digest(std::span<const std::byte>(store).subspan(
                static_cast<std::size_t>(descriptor_offset),
                static_cast<std::size_t>(
                    first_group_offset - descriptor_offset)))) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        for (const auto& group : group_index) {
            if (!append_digest(std::span<const std::byte>(store).subspan(
                    static_cast<std::size_t>(group.offset),
                    static_cast<std::size_t>(group.length)))) {
                return std::unexpected(FormatError::ChecksumUnavailable);
            }
        }
        if (!append_digest(std::span<const std::byte>(store).subspan(
                static_cast<std::size_t>(index_offset)))) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }

        std::vector<std::byte> digest_bytes;
        digest_bytes.reserve(block_digests.size() * 32u);
        for (const CacheDigest256& digest : block_digests) {
            for (const std::uint8_t byte : digest.bytes) {
                digest_bytes.push_back(static_cast<std::byte>(byte));
            }
        }
        auto manifest = policy.manifest_sha256(block_digests);
        if (!manifest.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        for (const char byte : kColumnFooterMagic) {
            store.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(byte)));
        }
        append_little_endian(
            store, static_cast<std::uint32_t>(block_digests.size()));
        append_little_endian(store, std::uint32_t{0});
        store.insert(store.end(), digest_bytes.begin(), digest_bytes.end());
        for (const std::uint8_t byte : manifest->bytes) {
            store.push_back(static_cast<std::byte>(byte));
        }
        const auto measured = projected_column_store_bytes(
            index_offset, groups_.size());
        if (!measured.has_value()) {
            return std::unexpected(measured.error());
        }
        if (store.size() != *measured) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        return store;
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }

private:
    template <column_checksum_policy, column_store_block_sink>
    friend class StreamingColumnStoreBuilder;

    /** Serialize one internally columnar row group. */
    template <column_checksum_policy Policy>
    [[nodiscard]] std::expected<std::vector<std::byte>, FormatError>
    serialize_group(
        const detail::PendingGroup& group,
        std::uint64_t store_offset,
        Policy& policy) const {
        const auto chunk_table_bytes = checked_mul(
            static_cast<std::uint64_t>(schema_.size()),
            static_cast<std::uint64_t>(kColumnChunkBytes));
        if (!chunk_table_bytes.has_value()) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        const auto payload_start = checked_add(
            static_cast<std::uint64_t>(kRowGroupHeaderBytes),
            *chunk_table_bytes);
        if (!payload_start.has_value()
            || *payload_start > (std::numeric_limits<std::size_t>::max)()) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        std::vector<std::byte> bytes(
            static_cast<std::size_t>(*payload_start), std::byte{0});
        std::vector<ColumnChunkV1> chunks(schema_.size());
        for (std::size_t column_index = 0;
             column_index < schema_.size();
             ++column_index) {
            const ColumnSchema& schema = schema_[column_index];
            ColumnChunkV1& chunk = chunks[column_index];
            std::vector<bool> valid(group.rows.size(), true);
            bool has_null = false;
            for (std::size_t row = 0; row < group.rows.size(); ++row) {
                valid[row] = !group.rows[row][column_index].is_null;
                has_null = has_null || !valid[row];
            }
            if (has_null) {
                chunk.null_bitmap_offset = bytes.size();
                std::vector<bool> stable_valid = valid;
                std::vector<std::byte> bitmap(
                    (stable_valid.size() + 7u) / 8u, std::byte{0});
                for (std::size_t row = 0; row < stable_valid.size(); ++row) {
                    if (stable_valid[row]) {
                        bitmap[row / 8u] |= static_cast<std::byte>(
                            1u << (row % 8u));
                    }
                }
                chunk.null_bitmap_bytes = bitmap.size();
                bytes.insert(bytes.end(), bitmap.begin(), bitmap.end());
            }

            const ColumnEncoding encoding = schema.descriptor.encoding;
            const bool variable = encoding == ColumnEncoding::FlatVarlena
                || encoding == ColumnEncoding::BinarySend;
            if (variable) {
                chunk.offsets_offset = bytes.size();
                chunk.offsets_bytes = (group.rows.size() + 1u) * sizeof(std::uint64_t);
                std::uint64_t payload_offset = 0;
                append_little_endian(bytes, payload_offset);
                for (const auto& row : group.rows) {
                    const auto next = checked_add(
                        payload_offset,
                        static_cast<std::uint64_t>(
                            row[column_index].bytes.size()));
                    if (!next.has_value()) {
                        return std::unexpected(FormatError::ArithmeticOverflow);
                    }
                    payload_offset = *next;
                    append_little_endian(bytes, payload_offset);
                }
                chunk.values_offset = bytes.size();
                for (const auto& row : group.rows) {
                    bytes.insert(
                        bytes.end(),
                        row[column_index].bytes.begin(),
                        row[column_index].bytes.end());
                }
                chunk.values_bytes = payload_offset;
                chunk.logical_values_bytes = payload_offset;
            } else if (encoding == ColumnEncoding::BooleanBits) {
                chunk.values_offset = bytes.size();
                chunk.values_bytes = (group.rows.size() + 7u) / 8u;
                bytes.resize(
                    bytes.size() + static_cast<std::size_t>(chunk.values_bytes),
                    std::byte{0});
                for (std::size_t row = 0; row < group.rows.size(); ++row) {
                    if (!group.rows[row][column_index].is_null
                        && group.rows[row][column_index].bytes[0]
                            == std::byte{1}) {
                        bytes[static_cast<std::size_t>(chunk.values_offset)
                              + row / 8u]
                            |= static_cast<std::byte>(1u << (row % 8u));
                    }
                }
                chunk.logical_values_bytes = group.rows.size();
            } else {
                const auto alignment = detail::align_vector(
                    bytes,
                    schema.descriptor.type_alignment,
                    store_offset);
                if (!alignment.has_value()) {
                    return std::unexpected(alignment.error());
                }
                chunk.values_offset = bytes.size();
                const std::size_t width = static_cast<std::size_t>(
                    schema.descriptor.type_length);
                const auto stride = detail::fixed_value_stride(
                    schema.descriptor);
                if (!stride.has_value()) {
                    return std::unexpected(stride.error());
                }
                const auto required_values = checked_mul(
                    *stride,
                    static_cast<std::uint64_t>(group.rows.size()));
                const auto logical_values = checked_mul(
                    static_cast<std::uint64_t>(width),
                    static_cast<std::uint64_t>(group.rows.size()));
                if (!required_values.has_value()
                    || !logical_values.has_value()
                    || !checked_range(
                        static_cast<std::uint64_t>(bytes.size()),
                        *required_values,
                        (std::numeric_limits<std::size_t>::max)())) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                for (const auto& row : group.rows) {
                    if (row[column_index].is_null) {
                        bytes.resize(
                            bytes.size() + static_cast<std::size_t>(*stride),
                            std::byte{0});
                    } else {
                        bytes.insert(
                            bytes.end(),
                            row[column_index].bytes.begin(),
                            row[column_index].bytes.end());
                        bytes.resize(
                            bytes.size()
                                + static_cast<std::size_t>(*stride) - width,
                            std::byte{0});
                    }
                }
                chunk.values_bytes = *required_values;
                chunk.logical_values_bytes = *logical_values;
            }
            auto checksum_input = detail::chunk_checksum_bytes(bytes, chunk);
            if (!checksum_input.has_value()) {
                return std::unexpected(checksum_input.error());
            }
            auto checksum = policy.block_crc32c(*checksum_input);
            if (!checksum.has_value()) {
                return std::unexpected(FormatError::ChecksumUnavailable);
            }
            chunk.crc32c = *checksum;
        }

        for (std::size_t index = 0; index < chunks.size(); ++index) {
            const std::size_t offset = kRowGroupHeaderBytes
                + index * kColumnChunkBytes;
            const ColumnChunkV1& chunk = chunks[index];
            std::size_t cursor = offset;
            for (const std::uint64_t value : {
                     chunk.null_bitmap_offset,
                     chunk.null_bitmap_bytes,
                     chunk.offsets_offset,
                     chunk.offsets_bytes,
                     chunk.values_offset,
                     chunk.values_bytes,
                     chunk.logical_values_bytes}) {
                detail::write_little_endian_at(bytes, cursor, value);
                cursor += sizeof(value);
            }
            detail::write_little_endian_at(bytes, cursor, chunk.crc32c);
            cursor += sizeof(chunk.crc32c);
            detail::write_little_endian_at(bytes, cursor, chunk.flags);
        }

        std::vector<std::byte> header;
        append_little_endian(header, group.first_row_ordinal);
        append_little_endian(
            header, static_cast<std::uint32_t>(group.rows.size()));
        append_little_endian(
            header, static_cast<std::uint16_t>(schema_.size()));
        append_little_endian(header, std::uint16_t{0});
        append_little_endian(
            header, static_cast<std::uint64_t>(kRowGroupHeaderBytes));
        append_little_endian(header, static_cast<std::uint64_t>(bytes.size()));
        append_little_endian(header, std::uint32_t{0});
        append_little_endian(header, std::uint32_t{0});
        std::copy(header.begin(), header.end(), bytes.begin());
        auto group_crc = policy.block_crc32c(bytes);
        if (!group_crc.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        detail::write_little_endian_at(bytes, kGroupCrcOffset, *group_crc);
        return bytes;
    }

    ColumnStoreIdentity identity_{};
    std::vector<ColumnSchema> schema_;
    RowGroupLimits limits_{};
    std::vector<detail::PendingGroup> groups_;
    std::uint64_t row_count_ = 0;
};

/**
 * One-row-group writer whose immutable blocks are copied directly into a
 * caller-owned sink.  Completed groups are never retained as byte vectors.
 */
template <column_checksum_policy Policy, column_store_block_sink Sink>
class StreamingColumnStoreBuilder {
public:
    using token_type = typename Sink::token_type;
    using result_type = StreamedColumnStoreBlocks<Sink>;

    StreamingColumnStoreBuilder(
        ColumnStoreIdentity identity,
        std::span<const ColumnSchema> schema,
        Policy& policy,
        Sink& sink,
        RowGroupLimits limits = {})
        : identity_(identity),
          schema_(schema.begin(), schema.end()),
          limits_(limits),
          group_encoder_(identity, schema, limits),
          policy_(&policy),
          sink_(&sink) {
        if (limits_.rows == 0) {
            limits_.rows = kDefaultRowGroupRows;
        }
        if (limits_.value_bytes == 0) {
            limits_.value_bytes = kDefaultRowGroupValueBytes;
        }
        if (limits_.rows > kDefaultRowGroupRows ||
            limits_.value_bytes > kDefaultRowGroupValueBytes) {
            initial_error_ = FormatError::InvalidRowGroupLimits;
            return;
        }
        auto descriptors = detail::serialize_schema(schema_);
        if (!descriptors.has_value()) {
            initial_error_ = descriptors.error();
            return;
        }
        descriptors_ = std::move(*descriptors);
        auto descriptor_digest = policy_->sha256(descriptors_);
        if (!descriptor_digest.has_value()) {
            initial_error_ = FormatError::ChecksumUnavailable;
            return;
        }
        descriptor_digest_ = *descriptor_digest;
        const auto initial_bytes = checked_add(
            static_cast<std::uint64_t>(kColumnStoreHeaderBytes),
            static_cast<std::uint64_t>(descriptors_.size()));
        if (!initial_bytes.has_value()) {
            initial_error_ = FormatError::ArithmeticOverflow;
            return;
        }
        virtual_store_bytes_ = *initial_bytes;
        const auto empty_size = projected_column_store_bytes(
            virtual_store_bytes_, 0);
        if (!empty_size.has_value()) {
            initial_error_ = empty_size.error();
            return;
        }
        const auto initial_metadata = metadata_bytes_for_capacities(0, 0, 4);
        if (!initial_metadata.has_value() ||
            *initial_metadata > kMaximumColumnStoreMetadataBytes) {
            initial_error_ = FormatError::MetadataLimitExceeded;
            return;
        }
        terminal_tokens_.reserve(4);
    }

    StreamingColumnStoreBuilder(const StreamingColumnStoreBuilder&) = delete;
    StreamingColumnStoreBuilder& operator=(
        const StreamingColumnStoreBuilder&) = delete;
    StreamingColumnStoreBuilder(StreamingColumnStoreBuilder&&) = delete;
    StreamingColumnStoreBuilder& operator=(
        StreamingColumnStoreBuilder&&) = delete;

    ~StreamingColumnStoreBuilder() noexcept {
        cleanup_tokens();
    }

    /** Copy one row, flushing the preceding complete group when required. */
    [[nodiscard]] std::expected<void, FormatError> append_row(
        std::span<const EncodedColumnValue> values) {
        if (state_ != State::Open) {
            return std::unexpected(state_error());
        }
        if (initial_error_.has_value()) {
            return std::unexpected(*initial_error_);
        }
        if (values.size() != schema_.size()) {
            return std::unexpected(FormatError::ColumnCountMismatch);
        }

        std::uint64_t row_bytes = 0;
        for (std::size_t column_index = 0;
             column_index < values.size(); ++column_index) {
            const ColumnSchema& schema = schema_[column_index];
            const EncodedColumnValue& value = values[column_index];
            const ColumnEncoding encoding = schema.descriptor.encoding;
            if (value.is_null &&
                (schema.descriptor.flags & kColumnFlagNullable) == 0) {
                return std::unexpected(FormatError::InvalidNullBitmap);
            }
            if (encoding == ColumnEncoding::Dictionary ||
                encoding == ColumnEncoding::RunLength) {
                return std::unexpected(FormatError::UnsupportedEncoding);
            }
            if (!value.is_null) {
                if ((encoding == ColumnEncoding::FixedByValue ||
                     encoding == ColumnEncoding::FixedByReference) &&
                    (schema.descriptor.type_length <= 0 ||
                     value.bytes.size() != static_cast<std::size_t>(
                         schema.descriptor.type_length))) {
                    return std::unexpected(FormatError::InvalidFixedWidth);
                }
                if (encoding == ColumnEncoding::BooleanBits &&
                    (value.bytes.size() != 1 ||
                     (value.bytes[0] != std::byte{0} &&
                      value.bytes[0] != std::byte{1}))) {
                    return std::unexpected(FormatError::InvalidBoolean);
                }
            }
            const auto next = checked_add(
                row_bytes, static_cast<std::uint64_t>(value.bytes.size()));
            if (!next.has_value()) {
                return std::unexpected(FormatError::ArithmeticOverflow);
            }
            row_bytes = *next;
        }
        if (row_bytes > limits_.value_bytes) {
            return std::unexpected(FormatError::RowGroupValueLimitExceeded);
        }

        bool value_limit_exceeded = false;
        if (!pending_.rows.empty()) {
            const auto next = checked_add(pending_.value_bytes, row_bytes);
            value_limit_exceeded = !next.has_value() ||
                *next > limits_.value_bytes;
        }
        if (!pending_.rows.empty() &&
            (pending_.rows.size() >= limits_.rows ||
             value_limit_exceeded)) {
            auto flushed = flush_pending_group();
            if (!flushed.has_value()) {
                return flushed;
            }
        }

        detail::OwnedRow row;
        row.reserve(values.size());
        for (const EncodedColumnValue& value : values) {
            row.push_back(detail::OwnedValue{
                .is_null = value.is_null,
                .bytes = value.is_null
                    ? std::vector<std::byte>{}
                    : std::vector<std::byte>(
                        value.bytes.begin(), value.bytes.end()),
            });
        }
        const auto next_group_bytes = checked_add(
            pending_.value_bytes, row_bytes);
        const auto next_row_count = checked_add(row_count_, std::uint64_t{1});
        if (!next_group_bytes.has_value() || !next_row_count.has_value()) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        pending_.rows.push_back(std::move(row));
        pending_.value_bytes = *next_group_bytes;
        row_count_ = *next_row_count;
        return {};
    }

    /** Flush the last group and emit header/schema/index/footer blocks. */
    [[nodiscard]] std::expected<result_type, FormatError> finalize() {
        if (state_ != State::Open) {
            return std::unexpected(state_error());
        }
        if (initial_error_.has_value()) {
            return std::unexpected(*initial_error_);
        }
        if (!pending_.rows.empty()) {
            auto flushed = flush_pending_group();
            if (!flushed.has_value()) {
                return std::unexpected(flushed.error());
            }
        }

        const std::uint64_t index_offset = virtual_store_bytes_;
        std::vector<std::byte> index_bytes;
        append_little_endian(
            index_bytes, static_cast<std::uint64_t>(group_index_.size()));
        for (const detail::GroupIndexEntry& group : group_index_) {
            append_little_endian(index_bytes, group.offset);
            append_little_endian(index_bytes, group.length);
            append_little_endian(index_bytes, group.crc32c);
            append_little_endian(index_bytes, std::uint32_t{0});
        }

        std::vector<std::byte> header;
        header.reserve(kColumnStoreHeaderBytes);
        for (const char byte : kColumnMagic) {
            header.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(byte)));
        }
        append_little_endian(header, kColumnFormatVersion);
        append_little_endian(header, identity_.postgres_major);
        append_little_endian(header, kSharedAbiVersion);
        append_little_endian(header, std::uint32_t{0});
        append_little_endian(header, identity_.boot_nonce);
        append_little_endian(header, identity_.database_oid);
        append_little_endian(header, identity_.owner_role_oid);
        append_little_endian(
            header, static_cast<std::uint8_t>(DataEndian::Little));
        append_little_endian(header, identity_.datum_width);
        append_little_endian(
            header, static_cast<std::uint16_t>(schema_.size()));
        append_little_endian(header, row_count_);
        append_little_endian(
            header, static_cast<std::uint64_t>(group_index_.size()));
        append_little_endian(
            header, static_cast<std::uint64_t>(kColumnStoreHeaderBytes));
        append_little_endian(header, index_offset);
        for (const std::uint8_t byte : descriptor_digest_.bytes) {
            header.push_back(static_cast<std::byte>(byte));
        }
        append_little_endian(header, std::uint32_t{0});
        if (header.size() != kColumnStoreHeaderBytes) {
            return fail_result(FormatError::InvalidSchema);
        }
        auto header_crc = policy_->block_crc32c(header);
        if (!header_crc.has_value()) {
            return fail_result(FormatError::ChecksumUnavailable);
        }
        detail::write_little_endian_at(
            header, kHeaderCrcOffset, *header_crc);

        auto header_digest = policy_->sha256(header);
        auto index_digest = policy_->sha256(index_bytes);
        if (!header_digest.has_value() || !index_digest.has_value()) {
            return fail_result(FormatError::ChecksumUnavailable);
        }
        std::vector<CacheDigest256> block_digests;
        block_digests.reserve(group_digests_.size() + 3u);
        block_digests.push_back(*header_digest);
        block_digests.push_back(descriptor_digest_);
        block_digests.insert(
            block_digests.end(),
            group_digests_.begin(), group_digests_.end());
        block_digests.push_back(*index_digest);
        auto manifest = policy_->manifest_sha256(block_digests);
        if (!manifest.has_value()) {
            return fail_result(FormatError::ChecksumUnavailable);
        }

        std::vector<std::byte> footer;
        for (const char byte : kColumnFooterMagic) {
            footer.push_back(static_cast<std::byte>(
                static_cast<unsigned char>(byte)));
        }
        append_little_endian(
            footer, static_cast<std::uint32_t>(block_digests.size()));
        append_little_endian(footer, std::uint32_t{0});
        for (const CacheDigest256& digest : block_digests) {
            for (const std::uint8_t byte : digest.bytes) {
                footer.push_back(static_cast<std::byte>(byte));
            }
        }
        for (const std::uint8_t byte : manifest->bytes) {
            footer.push_back(static_cast<std::byte>(byte));
        }

        const auto final_bytes = projected_column_store_bytes(
            index_offset, group_index_.size());
        if (!final_bytes.has_value()) {
            return fail_result(final_bytes.error());
        }
        const auto index_end = checked_add(
            index_offset, static_cast<std::uint64_t>(index_bytes.size()));
        const auto measured_final = index_end.has_value()
            ? checked_add(
                *index_end, static_cast<std::uint64_t>(footer.size()))
            : std::optional<std::uint64_t>{};
        if (!measured_final.has_value() || *measured_final != *final_bytes) {
            return fail_result(FormatError::ArithmeticOverflow);
        }

        if (!store_terminal(ColumnStoreBlockKind::Header, header) ||
            !store_terminal(ColumnStoreBlockKind::Schema, descriptors_) ||
            !store_terminal(ColumnStoreBlockKind::Index, index_bytes) ||
            !store_terminal(ColumnStoreBlockKind::Footer, footer)) {
            return std::unexpected(failure_error_);
        }

        group_tokens_.insert(
            group_tokens_.begin(), std::move(terminal_tokens_[1]));
        group_tokens_.insert(
            group_tokens_.begin(), std::move(terminal_tokens_[0]));
        group_tokens_.push_back(std::move(terminal_tokens_[2]));
        group_tokens_.push_back(std::move(terminal_tokens_[3]));
        terminal_tokens_.clear();
        state_ = State::Finalized;
        return result_type{
            sink_, std::move(group_tokens_), *final_bytes, row_count_,
            static_cast<std::uint32_t>(group_index_.size())};
    }

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }

    [[nodiscard]] std::uint32_t pending_row_count() const noexcept {
        return static_cast<std::uint32_t>(pending_.rows.size());
    }

    [[nodiscard]] std::uint64_t retained_value_bytes() const noexcept {
        return pending_.value_bytes;
    }

    [[nodiscard]] std::uint32_t emitted_row_group_count() const noexcept {
        return static_cast<std::uint32_t>(group_index_.size());
    }

    [[nodiscard]] std::uint64_t retained_metadata_bytes() const noexcept {
        const auto bytes = metadata_bytes_for_capacities(
            group_index_.capacity(),
            group_digests_.capacity(),
            group_tokens_.capacity() + terminal_tokens_.capacity());
        return bytes.value_or((std::numeric_limits<std::uint64_t>::max)());
    }

private:
    enum class State : std::uint8_t {
        Open,
        Failed,
        Finalized,
    };

    [[nodiscard]] FormatError state_error() const noexcept {
        return state_ == State::Failed
            ? failure_error_ : FormatError::InvalidBuilderState;
    }

    [[nodiscard]] std::expected<void, FormatError> flush_pending_group() {
        if (pending_.rows.empty()) {
            return {};
        }
        if (group_index_.size() >= kMaximumColumnStoreRowGroups) {
            return fail_void(FormatError::TooManyRowGroups);
        }
        const auto capacity = ensure_metadata_capacity(
            group_index_.size() + 1u);
        if (!capacity.has_value()) {
            return fail_void(capacity.error());
        }
        auto encoded = group_encoder_.serialize_group(
            pending_, virtual_store_bytes_, *policy_);
        if (!encoded.has_value()) {
            return fail_void(encoded.error());
        }
        auto group_crc = policy_->block_crc32c(*encoded);
        auto group_digest = policy_->sha256(*encoded);
        if (!group_crc.has_value() || !group_digest.has_value()) {
            return fail_void(FormatError::ChecksumUnavailable);
        }
        const auto group_end = checked_add(
            virtual_store_bytes_,
            static_cast<std::uint64_t>(encoded->size()));
        if (!group_end.has_value()) {
            return fail_void(FormatError::ArithmeticOverflow);
        }
        const auto projected = projected_column_store_bytes(
            *group_end, group_index_.size() + 1u);
        if (!projected.has_value()) {
            return fail_void(projected.error());
        }
        auto stored = sink_->store(
            ColumnStoreBlockKind::RowGroup,
            static_cast<std::uint64_t>(group_index_.size()), *encoded);
        if (!stored.has_value()) {
            return fail_void(FormatError::SinkFailed);
        }
        group_tokens_.push_back(std::move(*stored));
        group_index_.push_back(detail::GroupIndexEntry{
            .offset = virtual_store_bytes_,
            .length = encoded->size(),
            .crc32c = *group_crc,
        });
        group_digests_.push_back(*group_digest);
        virtual_store_bytes_ = *group_end;
        pending_ = detail::PendingGroup{
            .first_row_ordinal = row_count_,
        };
        return {};
    }

    [[nodiscard]] std::optional<std::uint64_t> metadata_bytes_for_capacities(
            std::uint64_t index_capacity,
            std::uint64_t digest_capacity,
            std::uint64_t token_capacity) const noexcept {
        std::optional<std::uint64_t> result{
            static_cast<std::uint64_t>(descriptors_.capacity())};
        const auto schema_storage = checked_mul(
            static_cast<std::uint64_t>(schema_.capacity()),
            static_cast<std::uint64_t>(sizeof(ColumnSchema) * 2u));
        if (!schema_storage.has_value()) {
            return std::nullopt;
        }
        result = checked_add(*result, *schema_storage);
        for (const ColumnSchema& column : schema_) {
            const auto names = checked_mul(
                static_cast<std::uint64_t>(column.name.capacity()),
                std::uint64_t{2});
            if (!result.has_value() || !names.has_value()) {
                return std::nullopt;
            }
            result = checked_add(*result, *names);
        }
        const auto index_bytes = checked_mul(
            index_capacity,
            static_cast<std::uint64_t>(sizeof(detail::GroupIndexEntry)));
        const auto digest_bytes = checked_mul(
            digest_capacity,
            static_cast<std::uint64_t>(sizeof(CacheDigest256)));
        const auto token_bytes = checked_mul(
            token_capacity,
            static_cast<std::uint64_t>(sizeof(token_type)));
        if (!result.has_value() || !index_bytes.has_value() ||
            !digest_bytes.has_value() || !token_bytes.has_value()) {
            return std::nullopt;
        }
        result = checked_add(*result, *index_bytes);
        if (!result.has_value()) {
            return std::nullopt;
        }
        result = checked_add(*result, *digest_bytes);
        if (!result.has_value()) {
            return std::nullopt;
        }
        return checked_add(*result, *token_bytes);
    }

    [[nodiscard]] std::expected<void, FormatError>
    ensure_metadata_capacity(std::size_t group_count) {
        if (group_count > kMaximumColumnStoreRowGroups) {
            return std::unexpected(FormatError::TooManyRowGroups);
        }
        std::size_t target = group_index_.capacity();
        if (target < group_count) {
            target = target == 0 ? 16u : target;
            while (target < group_count) {
                if (target > kMaximumColumnStoreRowGroups / 2u) {
                    target = kMaximumColumnStoreRowGroups;
                    break;
                }
                target *= 2u;
            }
        }
        const auto token_capacity = checked_add(
            static_cast<std::uint64_t>(target), std::uint64_t{4});
        const auto metadata = token_capacity.has_value()
            ? metadata_bytes_for_capacities(target, target, *token_capacity)
            : std::optional<std::uint64_t>{};
        if (!metadata.has_value() ||
            *metadata > kMaximumColumnStoreMetadataBytes) {
            return std::unexpected(FormatError::MetadataLimitExceeded);
        }
        group_index_.reserve(target);
        group_digests_.reserve(target);
        group_tokens_.reserve(static_cast<std::size_t>(*token_capacity));
        return {};
    }

    [[nodiscard]] bool store_terminal(
            ColumnStoreBlockKind kind,
            std::span<const std::byte> bytes) {
        auto stored = sink_->store(kind, 0, bytes);
        if (!stored.has_value()) {
            static_cast<void>(fail_void(FormatError::SinkFailed));
            return false;
        }
        terminal_tokens_.push_back(std::move(*stored));
        return true;
    }

    [[nodiscard]] std::expected<void, FormatError> fail_void(
            FormatError error) noexcept {
        failure_error_ = error;
        state_ = State::Failed;
        cleanup_tokens();
        return std::unexpected(error);
    }

    [[nodiscard]] std::expected<result_type, FormatError> fail_result(
            FormatError error) noexcept {
        static_cast<void>(fail_void(error));
        return std::unexpected(error);
    }

    void cleanup_tokens() noexcept {
        if (sink_ != nullptr && state_ != State::Finalized) {
            for (const token_type& token : group_tokens_) {
                sink_->discard(token);
            }
            for (const token_type& token : terminal_tokens_) {
                sink_->discard(token);
            }
        }
        group_tokens_.clear();
        terminal_tokens_.clear();
    }

    ColumnStoreIdentity identity_{};
    std::vector<ColumnSchema> schema_;
    RowGroupLimits limits_{};
    ColumnStoreBuilder group_encoder_;
    Policy* policy_ = nullptr;
    Sink* sink_ = nullptr;
    std::optional<FormatError> initial_error_;
    FormatError failure_error_ = FormatError::InvalidBuilderState;
    State state_ = State::Open;
    std::vector<std::byte> descriptors_;
    CacheDigest256 descriptor_digest_{};
    detail::PendingGroup pending_{};
    std::vector<detail::GroupIndexEntry> group_index_;
    std::vector<CacheDigest256> group_digests_;
    std::vector<token_type> group_tokens_;
    std::vector<token_type> terminal_tokens_;
    std::uint64_t virtual_store_bytes_ = 0;
    std::uint64_t row_count_ = 0;
};

/** Fully prevalidated, non-owning view over a caller-owned sealed blob. */
class ColumnStoreView {
public:
    /**
     * One already-validated column chunk.  The spans borrow from the sealed
     * store owned by the caller of validate_column_store().
     *
     * Consumers that process a column at a time should use this view instead
     * of repeatedly resolving a global row ordinal through value().  All
     * offsets, lengths, padding, bitmaps, and checksums have been validated
     * before a chunk can be returned.
     */
    struct ColumnChunkView {
        std::uint64_t first_row_ordinal = 0;
        std::uint64_t row_count = 0;
        std::uint64_t fixed_width = 0;
        std::uint64_t fixed_stride = 0;
        ColumnEncoding encoding = ColumnEncoding::BinarySend;
        std::span<const std::byte> validity_bitmap{};
        std::span<const std::byte> offsets{};
        std::span<const std::byte> values{};

        [[nodiscard]] bool valid(std::uint64_t local_row) const noexcept {
            if (local_row >= row_count) {
                return false;
            }
            return validity_bitmap.empty()
                || (std::to_integer<unsigned int>(
                        validity_bitmap[local_row / 8u])
                    & (1u << (local_row % 8u))) != 0;
        }
    };

    ColumnStoreView() = default;

    ColumnStoreView(
        std::span<const std::byte> bytes,
        ColumnStoreIdentity identity,
        std::vector<ColumnSchema> schema,
        std::vector<detail::ParsedGroup> groups,
        std::uint64_t row_count) noexcept
        : bytes_(bytes),
          identity_(identity),
          schema_(std::move(schema)),
          groups_(std::move(groups)),
          row_count_(row_count) {}

    [[nodiscard]] std::uint64_t row_count() const noexcept {
        return row_count_;
    }

    [[nodiscard]] std::size_t column_count() const noexcept {
        return schema_.size();
    }

    [[nodiscard]] std::size_t row_group_count() const noexcept {
        return groups_.size();
    }

    [[nodiscard]] const std::vector<ColumnSchema>& schema() const noexcept {
        return schema_;
    }

    [[nodiscard]] const ColumnStoreIdentity& identity() const noexcept {
        return identity_;
    }

    /**
     * Resolve one column into row-group chunks once.  This is the streaming
     * replay interface: callers retain row-group order while avoiding a
     * linear group lookup for every cell.
     */
    [[nodiscard]] std::expected<std::vector<ColumnChunkView>, FormatError>
    column_chunks(std::size_t column_index) const {
        if (column_index >= schema_.size()) {
            return std::unexpected(FormatError::OffsetOutOfBounds);
        }
        const ColumnDescriptorV1& descriptor =
            schema_[column_index].descriptor;
        const bool fixed =
            descriptor.encoding == ColumnEncoding::FixedByValue
            || descriptor.encoding == ColumnEncoding::FixedByReference;
        const auto fixed_stride = fixed
            ? detail::fixed_value_stride(descriptor)
            : std::expected<std::uint64_t, FormatError>{std::uint64_t{0}};
        if (!fixed_stride.has_value()) {
            return std::unexpected(fixed_stride.error());
        }

        std::vector<ColumnChunkView> result;
        result.reserve(groups_.size());
        for (const detail::ParsedGroup& group : groups_) {
            if (column_index >= group.chunks.size()) {
                return std::unexpected(FormatError::ColumnCountMismatch);
            }
            auto group_bytes = checked_subspan(
                bytes_, group.offset, group.length);
            if (!group_bytes.has_value()) {
                return std::unexpected(FormatError::OffsetOutOfBounds);
            }
            const ColumnChunkV1& chunk = group.chunks[column_index].wire;
            std::span<const std::byte> validity{};
            std::span<const std::byte> offsets{};
            if (chunk.null_bitmap_bytes != 0) {
                auto selected = checked_subspan(
                    *group_bytes,
                    chunk.null_bitmap_offset,
                    chunk.null_bitmap_bytes);
                if (!selected.has_value()) {
                    return std::unexpected(FormatError::OffsetOutOfBounds);
                }
                validity = *selected;
            }
            if (chunk.offsets_bytes != 0) {
                auto selected = checked_subspan(
                    *group_bytes, chunk.offsets_offset, chunk.offsets_bytes);
                if (!selected.has_value()) {
                    return std::unexpected(FormatError::OffsetOutOfBounds);
                }
                offsets = *selected;
            }
            auto values = checked_subspan(
                *group_bytes, chunk.values_offset, chunk.values_bytes);
            if (!values.has_value()) {
                return std::unexpected(FormatError::OffsetOutOfBounds);
            }
            result.push_back(ColumnChunkView{
                .first_row_ordinal = group.first_row_ordinal,
                .row_count = group.row_count,
                .fixed_width = fixed
                    ? static_cast<std::uint64_t>(descriptor.type_length)
                    : 0,
                .fixed_stride = *fixed_stride,
                .encoding = descriptor.encoding,
                .validity_bitmap = validity,
                .offsets = offsets,
                .values = *values,
            });
        }
        return result;
    }

    /** Return one encoded cell without invoking PostgreSQL receive functions. */
    [[nodiscard]] std::expected<ColumnValueView, FormatError> value(
        std::uint64_t row_ordinal,
        std::size_t column_index) const noexcept {
        if (row_ordinal >= row_count_ || column_index >= schema_.size()) {
            return std::unexpected(FormatError::OffsetOutOfBounds);
        }
        const detail::ParsedGroup* selected = nullptr;
        for (const auto& group : groups_) {
            const auto group_end = checked_add(
                group.first_row_ordinal,
                static_cast<std::uint64_t>(group.row_count));
            if (row_ordinal >= group.first_row_ordinal
                && group_end.has_value() && row_ordinal < *group_end) {
                selected = &group;
                break;
            }
        }
        if (selected == nullptr) {
            return std::unexpected(FormatError::InvalidRowOrdinal);
        }
        const std::uint64_t local_row = row_ordinal - selected->first_row_ordinal;
        const ColumnChunkV1& chunk = selected->chunks[column_index].wire;
        auto group_bytes = checked_subspan(
            bytes_, selected->offset, selected->length);
        if (!group_bytes.has_value()) {
            return std::unexpected(FormatError::OffsetOutOfBounds);
        }
        bool valid = true;
        if (chunk.null_bitmap_bytes != 0) {
            auto bitmap = checked_subspan(
                *group_bytes,
                chunk.null_bitmap_offset,
                chunk.null_bitmap_bytes);
            if (!bitmap.has_value()) {
                return std::unexpected(FormatError::OffsetOutOfBounds);
            }
            valid = (std::to_integer<unsigned int>(
                         (*bitmap)[local_row / 8u])
                     & (1u << (local_row % 8u))) != 0;
        }
        if (!valid) {
            return ColumnValueView{.is_null = true};
        }
        const ColumnEncoding encoding = schema_[column_index].descriptor.encoding;
        if (encoding == ColumnEncoding::BooleanBits) {
            auto values = checked_subspan(
                *group_bytes, chunk.values_offset, chunk.values_bytes);
            if (!values.has_value()) {
                return std::unexpected(FormatError::OffsetOutOfBounds);
            }
            const bool boolean =
                (std::to_integer<unsigned int>((*values)[local_row / 8u])
                 & (1u << (local_row % 8u))) != 0;
            return ColumnValueView{
                .is_boolean = true,
                .boolean_value = boolean,
            };
        }
        std::uint64_t value_offset = 0;
        std::uint64_t value_length = 0;
        if (encoding == ColumnEncoding::FlatVarlena
            || encoding == ColumnEncoding::BinarySend) {
            auto offsets = checked_subspan(
                *group_bytes, chunk.offsets_offset, chunk.offsets_bytes);
            if (!offsets.has_value()) {
                return std::unexpected(FormatError::OffsetOutOfBounds);
            }
            std::size_t first_cursor = local_row * sizeof(std::uint64_t);
            std::size_t next_cursor = first_cursor + sizeof(std::uint64_t);
            auto first = read_little_endian<std::uint64_t>(*offsets, first_cursor);
            auto next = read_little_endian<std::uint64_t>(*offsets, next_cursor);
            if (!first.has_value() || !next.has_value() || *next < *first) {
                return std::unexpected(FormatError::InvalidOffsets);
            }
            value_offset = *first;
            value_length = *next - *first;
        } else {
            const std::uint64_t width = static_cast<std::uint64_t>(
                schema_[column_index].descriptor.type_length);
            const auto stride = detail::fixed_value_stride(
                schema_[column_index].descriptor);
            if (!stride.has_value()) {
                return std::unexpected(stride.error());
            }
            const auto offset = checked_mul(local_row, *stride);
            if (!offset.has_value()) {
                return std::unexpected(FormatError::ArithmeticOverflow);
            }
            value_offset = *offset;
            value_length = width;
        }
        auto values = checked_subspan(
            *group_bytes, chunk.values_offset, chunk.values_bytes);
        if (!values.has_value()) {
            return std::unexpected(FormatError::OffsetOutOfBounds);
        }
        auto selected_value = checked_subspan(
            *values, value_offset, value_length);
        if (!selected_value.has_value()) {
            return std::unexpected(FormatError::InvalidOffsets);
        }
        return ColumnValueView{.bytes = *selected_value};
    }

private:
    std::span<const std::byte> bytes_{};
    ColumnStoreIdentity identity_{};
    std::vector<ColumnSchema> schema_;
    std::vector<detail::ParsedGroup> groups_;
    std::uint64_t row_count_ = 0;
};

/** Validate every protected byte and decoder-independent layout before replay. */
template <column_checksum_policy Policy>
[[nodiscard]] std::expected<ColumnStoreView, FormatError>
validate_column_store(
    std::span<const std::byte> store,
    const ColumnStoreIdentity& expected_identity,
    std::span<const ColumnSchema> expected_schema,
    Policy& policy) {
    if (store.size() > kMaximumColumnStoreBytes) {
        return std::unexpected(FormatError::MaximumStoreBytesExceeded);
    }
    if (store.size() < kColumnStoreHeaderBytes + kFooterPrefixBytes + 32u) {
        return std::unexpected(FormatError::Truncated);
    }
    for (std::size_t index = 0; index < kColumnMagic.size(); ++index) {
        if (store[index] != static_cast<std::byte>(
                static_cast<unsigned char>(kColumnMagic[index]))) {
            return std::unexpected(FormatError::BadMagic);
        }
    }
    std::size_t cursor = kColumnMagic.size();
    auto format_version = read_little_endian<std::uint32_t>(store, cursor);
    if (!format_version.has_value() || *format_version != kColumnFormatVersion) {
        return std::unexpected(FormatError::UnsupportedVersion);
    }
    auto postgres_major = read_little_endian<std::uint32_t>(store, cursor);
    auto shared_abi = read_little_endian<std::uint32_t>(store, cursor);
    auto flags = read_little_endian<std::uint32_t>(store, cursor);
    auto boot_nonce = read_little_endian<std::uint64_t>(store, cursor);
    auto database_oid = read_little_endian<std::uint32_t>(store, cursor);
    auto owner_role_oid = read_little_endian<std::uint32_t>(store, cursor);
    auto endian = read_little_endian<std::uint8_t>(store, cursor);
    auto datum_width = read_little_endian<std::uint8_t>(store, cursor);
    auto column_count = read_little_endian<std::uint16_t>(store, cursor);
    auto row_count = read_little_endian<std::uint64_t>(store, cursor);
    auto group_count = read_little_endian<std::uint64_t>(store, cursor);
    auto descriptor_offset = read_little_endian<std::uint64_t>(store, cursor);
    auto index_offset = read_little_endian<std::uint64_t>(store, cursor);
    if (!postgres_major || !shared_abi || !flags || !boot_nonce
        || !database_oid || !owner_role_oid || !endian || !datum_width
        || !column_count || !row_count || !group_count
        || !descriptor_offset || !index_offset) {
        return std::unexpected(FormatError::Truncated);
    }
    if (*group_count > kMaximumColumnStoreRowGroups) {
        return std::unexpected(FormatError::TooManyRowGroups);
    }
    CacheDigest256 stored_schema_digest{};
    for (std::uint8_t& byte : stored_schema_digest.bytes) {
        byte = std::to_integer<std::uint8_t>(store[cursor++]);
    }
    auto stored_header_crc = read_little_endian<std::uint32_t>(store, cursor);
    if (!stored_header_crc || cursor != kColumnStoreHeaderBytes) {
        return std::unexpected(FormatError::Truncated);
    }
    if (*shared_abi != kSharedAbiVersion) {
        return std::unexpected(FormatError::AbiMismatch);
    }
    if (*flags != 0) {
        return std::unexpected(FormatError::UnsupportedVersion);
    }
    if (*endian != static_cast<std::uint8_t>(DataEndian::Little)
        || !host_is_little_endian()) {
        return std::unexpected(FormatError::EndianMismatch);
    }
    const ColumnStoreIdentity identity{
        .postgres_major = *postgres_major,
        .boot_nonce = *boot_nonce,
        .database_oid = *database_oid,
        .owner_role_oid = *owner_role_oid,
        .datum_width = *datum_width,
    };
    if (identity != expected_identity) {
        return std::unexpected(FormatError::IdentityMismatch);
    }
    std::vector<std::byte> header(
        store.begin(), store.begin() + kColumnStoreHeaderBytes);
    detail::write_little_endian_at(
        header, kHeaderCrcOffset, std::uint32_t{0});
    auto header_crc = policy.block_crc32c(header);
    if (!header_crc.has_value()) {
        return std::unexpected(FormatError::ChecksumUnavailable);
    }
    if (*header_crc != *stored_header_crc) {
        return std::unexpected(FormatError::CorruptBlock);
    }
    if (*descriptor_offset != kColumnStoreHeaderBytes
        || *index_offset < *descriptor_offset
        || *index_offset >= store.size()) {
        return std::unexpected(FormatError::OffsetOutOfBounds);
    }
    if (expected_schema.size() != *column_count) {
        return std::unexpected(FormatError::SchemaMismatch);
    }
    auto expected_descriptor_block = detail::serialize_schema(expected_schema);
    if (!expected_descriptor_block.has_value()) {
        return std::unexpected(FormatError::InvalidSchema);
    }
    auto stored_expected_descriptor = checked_subspan(
        store, *descriptor_offset, expected_descriptor_block->size());
    if (!stored_expected_descriptor.has_value()
        || *descriptor_offset + expected_descriptor_block->size()
            > *index_offset
        || !std::equal(
            expected_descriptor_block->begin(),
            expected_descriptor_block->end(),
            stored_expected_descriptor->begin())) {
        return std::unexpected(FormatError::SchemaMismatch);
    }

    std::size_t schema_cursor = static_cast<std::size_t>(*descriptor_offset);
    auto descriptor_count = read_little_endian<std::uint32_t>(
        store, schema_cursor);
    if (!descriptor_count || *descriptor_count != *column_count
        || *descriptor_count > kMaxProviderBoundaryColumns) {
        return std::unexpected(FormatError::InvalidSchema);
    }
    std::vector<ColumnSchema> schema;
    schema.reserve(*descriptor_count);
    for (std::uint32_t index = 0; index < *descriptor_count; ++index) {
        ColumnSchema column{};
        auto type_oid = read_little_endian<std::uint32_t>(store, schema_cursor);
        auto typmod = read_little_endian<std::int32_t>(store, schema_cursor);
        auto collation = read_little_endian<std::uint32_t>(store, schema_cursor);
        auto type_length = read_little_endian<std::int16_t>(store, schema_cursor);
        auto by_value = read_little_endian<std::uint8_t>(store, schema_cursor);
        auto alignment = read_little_endian<std::uint8_t>(store, schema_cursor);
        auto encoding = read_little_endian<ColumnEncoding>(store, schema_cursor);
        auto column_flags = read_little_endian<std::uint16_t>(store, schema_cursor);
        auto send_oid = read_little_endian<std::uint32_t>(store, schema_cursor);
        auto receive_oid = read_little_endian<std::uint32_t>(store, schema_cursor);
        auto io_param = read_little_endian<std::uint32_t>(store, schema_cursor);
        auto reserved = read_little_endian<std::uint32_t>(store, schema_cursor);
        auto name_offset = read_little_endian<std::uint64_t>(store, schema_cursor);
        auto semantic_high = read_little_endian<std::uint64_t>(store, schema_cursor);
        auto semantic_low = read_little_endian<std::uint64_t>(store, schema_cursor);
        auto name_length = read_little_endian<std::uint32_t>(store, schema_cursor);
        if (!type_oid || !typmod || !collation || !type_length || !by_value
            || !alignment || !encoding || !column_flags || !send_oid
            || !receive_oid || !io_param || !reserved || !name_offset
            || !semantic_high || !semantic_low || !name_length
            || *name_offset != schema_cursor - *descriptor_offset
            || !checked_range(schema_cursor, *name_length, *index_offset)) {
            return std::unexpected(FormatError::InvalidSchema);
        }
        const bool supported_encoding =
            *encoding == ColumnEncoding::BooleanBits
            || *encoding == ColumnEncoding::FixedByValue
            || *encoding == ColumnEncoding::FixedByReference
            || *encoding == ColumnEncoding::FlatVarlena
            || *encoding == ColumnEncoding::BinarySend;
        const bool fixed_encoding =
            *encoding == ColumnEncoding::FixedByValue
            || *encoding == ColumnEncoding::FixedByReference;
        const bool supported_by_value_width = *type_length == 1
            || *type_length == 2
            || *type_length == 4
            || *type_length == 8;
        if (!supported_encoding) {
            return std::unexpected(
                *encoding == ColumnEncoding::Dictionary
                    || *encoding == ColumnEncoding::RunLength
                ? FormatError::UnsupportedEncoding
                : FormatError::InvalidSchema);
        }
        if (*alignment == 0 || (*alignment & (*alignment - 1)) != 0) {
            return std::unexpected(FormatError::InvalidAlignment);
        }
        if (*by_value > 1 || *reserved != 0
            || (*column_flags & ~kColumnFlagNullable) != 0
            || *name_length > kMaxCanonicalKeyBytes) {
            return std::unexpected(FormatError::InvalidSchema);
        }
        if ((fixed_encoding && *type_length <= 0)
            || (*encoding == ColumnEncoding::BooleanBits
                && (*type_length != 1 || *by_value != 1))
            || (*encoding == ColumnEncoding::FixedByValue
                && (*by_value != 1 || !supported_by_value_width))
            || (*encoding == ColumnEncoding::FixedByReference
                && *by_value != 0)
            || (*encoding == ColumnEncoding::FlatVarlena
                && (*by_value != 0 || *type_length != -1))
            || (*encoding == ColumnEncoding::BinarySend
                && (*send_oid == 0 || *receive_oid == 0))) {
            return std::unexpected(FormatError::InvalidFixedWidth);
        }
        column.name.reserve(*name_length);
        for (std::uint32_t byte = 0; byte < *name_length; ++byte) {
            column.name.push_back(static_cast<char>(
                std::to_integer<unsigned char>(store[schema_cursor++])));
        }
        column.descriptor = ColumnDescriptorV1{
            .type_oid = *type_oid,
            .typmod = *typmod,
            .collation_oid = *collation,
            .type_length = *type_length,
            .by_value = *by_value,
            .type_alignment = *alignment,
            .encoding = *encoding,
            .flags = *column_flags,
            .send_function_oid = *send_oid,
            .receive_function_oid = *receive_oid,
            .receive_type_io_param = *io_param,
            .reserved = *reserved,
            .name_offset = 0,
        };
        column.semantic_id = SemanticId128{
            .high = *semantic_high,
            .low = *semantic_low,
        };
        schema.push_back(std::move(column));
    }
    const std::uint64_t descriptor_end = schema_cursor;

    std::size_t index_cursor = static_cast<std::size_t>(*index_offset);
    auto indexed_group_count = read_little_endian<std::uint64_t>(
        store, index_cursor);
    if (!indexed_group_count || *indexed_group_count != *group_count
        || *indexed_group_count > kMaximumColumnStoreRowGroups) {
        return std::unexpected(FormatError::InvalidOffsets);
    }
    const auto index_entries_bytes = checked_mul(
        *indexed_group_count,
        static_cast<std::uint64_t>(kRowGroupIndexEntryBytes));
    const auto index_total_bytes = index_entries_bytes.has_value()
        ? checked_add(std::uint64_t{8}, *index_entries_bytes)
        : std::optional<std::uint64_t>{};
    if (!index_total_bytes.has_value()
        || !checked_range(*index_offset, *index_total_bytes, store.size())) {
        return std::unexpected(FormatError::InvalidOffsets);
    }
    std::vector<detail::GroupIndexEntry> group_index;
    group_index.reserve(static_cast<std::size_t>(*group_count));
    for (std::uint64_t index = 0; index < *group_count; ++index) {
        auto offset = read_little_endian<std::uint64_t>(store, index_cursor);
        auto length = read_little_endian<std::uint64_t>(store, index_cursor);
        auto crc = read_little_endian<std::uint32_t>(store, index_cursor);
        auto index_reserved = read_little_endian<std::uint32_t>(store, index_cursor);
        if (!offset || !length || !crc || !index_reserved || *index_reserved != 0
            || !checked_range(*offset, *length, *index_offset)) {
            return std::unexpected(FormatError::InvalidOffsets);
        }
        group_index.push_back(detail::GroupIndexEntry{
            .offset = *offset,
            .length = *length,
            .crc32c = *crc,
        });
    }
    const std::uint64_t footer_offset = index_cursor;
    const std::uint64_t first_group_offset = group_index.empty()
        ? *index_offset
        : group_index.front().offset;
    if (descriptor_end != first_group_offset) {
        return std::unexpected(FormatError::InvalidOffsets);
    }
    std::uint64_t expected_group_offset = first_group_offset;
    for (const auto& entry : group_index) {
        if (entry.offset != expected_group_offset) {
            return std::unexpected(FormatError::InvalidOffsets);
        }
        const auto next_group_offset = checked_add(
            expected_group_offset, entry.length);
        if (!next_group_offset.has_value()) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        expected_group_offset = *next_group_offset;
    }
    if (expected_group_offset != *index_offset) {
        return std::unexpected(FormatError::InvalidOffsets);
    }

    auto descriptor_block = checked_subspan(
        store,
        *descriptor_offset,
        first_group_offset - *descriptor_offset);
    if (!descriptor_block.has_value()) {
        return std::unexpected(FormatError::OffsetOutOfBounds);
    }
    auto computed_schema_digest = policy.sha256(*descriptor_block);
    if (!computed_schema_digest.has_value()) {
        return std::unexpected(FormatError::ChecksumUnavailable);
    }
    if (*computed_schema_digest != stored_schema_digest) {
        return std::unexpected(FormatError::SchemaMismatch);
    }
    if (expected_descriptor_block->size() != descriptor_block->size()
        || !std::equal(
            expected_descriptor_block->begin(),
            expected_descriptor_block->end(),
            descriptor_block->begin())) {
        return std::unexpected(FormatError::SchemaMismatch);
    }

    const std::uint32_t expected_block_count = static_cast<std::uint32_t>(
        group_index.size() + 3u);
    const auto footer_digest_bytes = checked_mul(
        static_cast<std::uint64_t>(expected_block_count),
        std::uint64_t{32});
    const auto footer_bytes = checked_add(
        static_cast<std::uint64_t>(kFooterPrefixBytes + 32u),
        footer_digest_bytes.value_or(
            (std::numeric_limits<std::uint64_t>::max)()));
    const auto footer_digest_size = footer_digest_bytes.has_value()
        ? checked_narrow<std::size_t>(*footer_digest_bytes)
        : std::optional<std::size_t>{};
    if (!footer_digest_size.has_value() || !footer_bytes.has_value()
        || !checked_range(footer_offset, *footer_bytes, store.size())
        || footer_offset + *footer_bytes != store.size()) {
        return std::unexpected(FormatError::Truncated);
    }
    for (std::size_t index = 0; index < kColumnFooterMagic.size(); ++index) {
        if (store[footer_offset + index] != static_cast<std::byte>(
                static_cast<unsigned char>(kColumnFooterMagic[index]))) {
            return std::unexpected(FormatError::CorruptManifest);
        }
    }
    std::size_t footer_cursor = static_cast<std::size_t>(footer_offset)
        + kColumnFooterMagic.size();
    auto block_count = read_little_endian<std::uint32_t>(store, footer_cursor);
    auto footer_reserved = read_little_endian<std::uint32_t>(store, footer_cursor);
    if (!block_count || !footer_reserved
        || *block_count != expected_block_count
        || *footer_reserved != 0) {
        return std::unexpected(FormatError::CorruptManifest);
    }
    std::vector<std::byte> stored_digest_bytes(
        store.begin() + footer_cursor,
        store.begin() + footer_cursor + *footer_digest_size);
    footer_cursor += *footer_digest_size;
    std::vector<CacheDigest256> stored_block_digests(expected_block_count);
    for (std::size_t block = 0; block < stored_block_digests.size(); ++block) {
        for (std::size_t byte = 0;
             byte < stored_block_digests[block].bytes.size();
             ++byte) {
            stored_block_digests[block].bytes[byte] =
                std::to_integer<std::uint8_t>(
                    stored_digest_bytes[block * 32u + byte]);
        }
    }
    CacheDigest256 stored_manifest{};
    for (std::uint8_t& byte : stored_manifest.bytes) {
        byte = std::to_integer<std::uint8_t>(store[footer_cursor++]);
    }
    auto computed_manifest = policy.manifest_sha256(stored_block_digests);
    if (!computed_manifest.has_value()) {
        return std::unexpected(FormatError::ChecksumUnavailable);
    }
    if (*computed_manifest != stored_manifest) {
        return std::unexpected(FormatError::CorruptManifest);
    }

    std::vector<std::span<const std::byte>> blocks;
    blocks.reserve(expected_block_count);
    blocks.push_back(store.first(kColumnStoreHeaderBytes));
    blocks.push_back(*descriptor_block);
    for (const auto& entry : group_index) {
        blocks.push_back(store.subspan(
            static_cast<std::size_t>(entry.offset),
            static_cast<std::size_t>(entry.length)));
    }
    blocks.push_back(store.subspan(
        static_cast<std::size_t>(*index_offset),
        static_cast<std::size_t>(footer_offset - *index_offset)));
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        auto digest = policy.sha256(blocks[index]);
        if (!digest.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        for (std::size_t byte = 0; byte < digest->bytes.size(); ++byte) {
            if (digest->bytes[byte]
                != std::to_integer<std::uint8_t>(
                    stored_digest_bytes[index * 32u + byte])) {
                return std::unexpected(FormatError::CorruptManifest);
            }
        }
    }

    std::vector<detail::ParsedGroup> parsed_groups;
    parsed_groups.reserve(group_index.size());
    std::uint64_t expected_ordinal = 0;
    for (const auto& entry : group_index) {
        auto group = checked_subspan(store, entry.offset, entry.length);
        if (!group.has_value() || group->size() < kRowGroupHeaderBytes) {
            return std::unexpected(FormatError::Truncated);
        }
        std::vector<std::byte> crc_bytes(group->begin(), group->end());
        auto computed_index_crc = policy.block_crc32c(*group);
        if (!computed_index_crc.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        if (*computed_index_crc != entry.crc32c) {
            return std::unexpected(FormatError::CorruptBlock);
        }
        std::size_t group_cursor = 0;
        auto first_ordinal = read_little_endian<std::uint64_t>(*group, group_cursor);
        auto rows = read_little_endian<std::uint32_t>(*group, group_cursor);
        auto columns = read_little_endian<std::uint16_t>(*group, group_cursor);
        auto group_flags = read_little_endian<std::uint16_t>(*group, group_cursor);
        auto chunk_table = read_little_endian<std::uint64_t>(*group, group_cursor);
        auto byte_length = read_little_endian<std::uint64_t>(*group, group_cursor);
        auto stored_group_crc = read_little_endian<std::uint32_t>(*group, group_cursor);
        auto group_reserved = read_little_endian<std::uint32_t>(*group, group_cursor);
        if (!first_ordinal || !rows || !columns || !group_flags || !chunk_table
            || !byte_length || !stored_group_crc || !group_reserved
            || *first_ordinal != expected_ordinal
            || *rows == 0
            || *columns != schema.size()
            || *group_flags != 0
            || *chunk_table != kRowGroupHeaderBytes
            || *byte_length != entry.length
            || *group_reserved != 0) {
            return std::unexpected(FormatError::InvalidRowOrdinal);
        }
        detail::write_little_endian_at(
            crc_bytes, kGroupCrcOffset, std::uint32_t{0});
        auto computed_group_crc = policy.block_crc32c(crc_bytes);
        if (!computed_group_crc.has_value()) {
            return std::unexpected(FormatError::ChecksumUnavailable);
        }
        if (*computed_group_crc != *stored_group_crc) {
            return std::unexpected(FormatError::CorruptBlock);
        }
        detail::ParsedGroup parsed{
            .first_row_ordinal = *first_ordinal,
            .row_count = *rows,
            .offset = entry.offset,
            .length = entry.length,
        };
        parsed.chunks.reserve(schema.size());
        std::size_t chunk_cursor = kRowGroupHeaderBytes;
        const std::uint64_t payload_start = kRowGroupHeaderBytes
            + schema.size() * kColumnChunkBytes;
        std::uint64_t expected_payload_offset = payload_start;
        const auto bitmap_numerator = checked_add(
            static_cast<std::uint64_t>(*rows), std::uint64_t{7});
        if (!bitmap_numerator.has_value()) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        const std::uint64_t expected_bitmap_bytes = *bitmap_numerator / 8u;
        for (std::size_t column_index = 0;
             column_index < schema.size();
             ++column_index) {
            ColumnChunkV1 chunk{};
            auto read_u64 = [&group, &chunk_cursor]() {
                return read_little_endian<std::uint64_t>(*group, chunk_cursor);
            };
            auto null_offset = read_u64();
            auto null_bytes = read_u64();
            auto offsets_offset = read_u64();
            auto offsets_bytes = read_u64();
            auto values_offset = read_u64();
            auto values_bytes = read_u64();
            auto logical_bytes = read_u64();
            auto chunk_crc = read_little_endian<std::uint32_t>(*group, chunk_cursor);
            auto chunk_flags = read_little_endian<std::uint32_t>(*group, chunk_cursor);
            if (!null_offset || !null_bytes || !offsets_offset || !offsets_bytes
                || !values_offset || !values_bytes || !logical_bytes
                || !chunk_crc || !chunk_flags || *chunk_flags != 0) {
                return std::unexpected(FormatError::Truncated);
            }
            chunk = ColumnChunkV1{
                .null_bitmap_offset = *null_offset,
                .null_bitmap_bytes = *null_bytes,
                .offsets_offset = *offsets_offset,
                .offsets_bytes = *offsets_bytes,
                .values_offset = *values_offset,
                .values_bytes = *values_bytes,
                .logical_values_bytes = *logical_bytes,
                .crc32c = *chunk_crc,
                .flags = *chunk_flags,
            };
            if ((chunk.null_bitmap_bytes != 0
                 && (chunk.null_bitmap_offset < payload_start
                     || !checked_range(
                         chunk.null_bitmap_offset,
                         chunk.null_bitmap_bytes,
                         entry.length)))
                || (chunk.offsets_bytes != 0
                    && (chunk.offsets_offset < payload_start
                        || !checked_range(
                            chunk.offsets_offset,
                            chunk.offsets_bytes,
                            entry.length)))
                || chunk.values_offset < payload_start
                || !checked_range(
                    chunk.values_offset, chunk.values_bytes, entry.length)) {
                return std::unexpected(FormatError::OffsetOutOfBounds);
            }
            if ((chunk.null_bitmap_bytes == 0
                 && chunk.null_bitmap_offset != 0)
                || (chunk.null_bitmap_bytes != 0
                    && chunk.null_bitmap_offset != expected_payload_offset)) {
                return std::unexpected(FormatError::InvalidNullBitmap);
            }
            if (chunk.null_bitmap_bytes != 0) {
                if (chunk.null_bitmap_bytes != expected_bitmap_bytes) {
                    return std::unexpected(FormatError::InvalidNullBitmap);
                }
                auto bitmap = checked_subspan(
                    *group, chunk.null_bitmap_offset, chunk.null_bitmap_bytes);
                if (!bitmap.has_value()
                    || !unused_tail_bits_are_zero(*bitmap, *rows)) {
                    return std::unexpected(FormatError::InvalidNullBitmap);
                }
                bool has_null = false;
                for (std::uint64_t row = 0; row < *rows; ++row) {
                    has_null = has_null
                        || (std::to_integer<unsigned int>(
                                (*bitmap)[row / 8u])
                            & (1u << (row % 8u))) == 0;
                }
                if (!has_null
                    || (schema[column_index].descriptor.flags
                        & kColumnFlagNullable) == 0) {
                    return std::unexpected(FormatError::InvalidNullBitmap);
                }
                const auto next_payload = checked_add(
                    expected_payload_offset, chunk.null_bitmap_bytes);
                if (!next_payload.has_value()) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                expected_payload_offset = *next_payload;
            }
            const ColumnSchema& column = schema[column_index];
            const bool variable =
                column.descriptor.encoding == ColumnEncoding::FlatVarlena
                || column.descriptor.encoding == ColumnEncoding::BinarySend;
            if (variable) {
                const std::uint64_t required_offset_bytes =
                    (static_cast<std::uint64_t>(*rows) + 1u)
                    * sizeof(std::uint64_t);
                if (chunk.offsets_bytes != required_offset_bytes
                    || chunk.logical_values_bytes != chunk.values_bytes
                    || chunk.offsets_offset != expected_payload_offset) {
                    return std::unexpected(FormatError::InvalidOffsets);
                }
                auto offsets = checked_subspan(
                    *group, chunk.offsets_offset, chunk.offsets_bytes);
                if (!offsets.has_value()) {
                    return std::unexpected(FormatError::InvalidOffsets);
                }
                std::uint64_t prior = 0;
                std::size_t offsets_cursor = 0;
                std::vector<std::uint64_t> parsed_offsets;
                parsed_offsets.reserve(static_cast<std::size_t>(*rows) + 1u);
                for (std::uint64_t row = 0; row <= *rows; ++row) {
                    auto offset = read_little_endian<std::uint64_t>(
                        *offsets, offsets_cursor);
                    if (!offset.has_value()
                        || (row == 0 && *offset != 0)
                        || *offset < prior
                        || *offset > chunk.values_bytes) {
                        return std::unexpected(FormatError::InvalidOffsets);
                    }
                    prior = *offset;
                    parsed_offsets.push_back(*offset);
                }
                if (prior != chunk.values_bytes) {
                    return std::unexpected(FormatError::InvalidOffsets);
                }
                const auto values_position = checked_add(
                    expected_payload_offset, chunk.offsets_bytes);
                if (!values_position.has_value()) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                const auto next_payload = checked_add(
                    *values_position, chunk.values_bytes);
                if (!next_payload.has_value()) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                if (chunk.values_offset != *values_position) {
                    return std::unexpected(FormatError::InvalidOffsets);
                }
                if (chunk.null_bitmap_bytes != 0) {
                    auto bitmap = checked_subspan(
                        *group,
                        chunk.null_bitmap_offset,
                        chunk.null_bitmap_bytes);
                    for (std::uint64_t row = 0; row < *rows; ++row) {
                        const bool valid =
                            (std::to_integer<unsigned int>(
                                 (*bitmap)[row / 8u])
                             & (1u << (row % 8u))) != 0;
                        if (!valid
                            && parsed_offsets[row]
                                != parsed_offsets[row + 1u]) {
                            return std::unexpected(FormatError::InvalidOffsets);
                        }
                    }
                }
                expected_payload_offset = *next_payload;
            } else if (column.descriptor.encoding
                       == ColumnEncoding::BooleanBits) {
                if (chunk.offsets_bytes != 0
                    || chunk.offsets_offset != 0
                    || chunk.values_offset != expected_payload_offset
                    || chunk.values_bytes != expected_bitmap_bytes
                    || chunk.logical_values_bytes != *rows) {
                    return std::unexpected(FormatError::InvalidOffsets);
                }
                auto values = checked_subspan(
                    *group, chunk.values_offset, chunk.values_bytes);
                if (!values.has_value()
                    || !unused_tail_bits_are_zero(*values, *rows)) {
                    return std::unexpected(FormatError::InvalidBoolean);
                }
                if (chunk.null_bitmap_bytes != 0) {
                    auto bitmap = checked_subspan(
                        *group,
                        chunk.null_bitmap_offset,
                        chunk.null_bitmap_bytes);
                    for (std::uint64_t row = 0; row < *rows; ++row) {
                        const bool valid =
                            (std::to_integer<unsigned int>(
                                 (*bitmap)[row / 8u])
                             & (1u << (row % 8u))) != 0;
                        const bool bit =
                            (std::to_integer<unsigned int>(
                                 (*values)[row / 8u])
                             & (1u << (row % 8u))) != 0;
                        if (!valid && bit) {
                            return std::unexpected(FormatError::InvalidBoolean);
                        }
                    }
                }
                const auto next_payload = checked_add(
                    expected_payload_offset, chunk.values_bytes);
                if (!next_payload.has_value()) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                expected_payload_offset = *next_payload;
            } else {
                const auto stride = detail::fixed_value_stride(
                    column.descriptor);
                const auto required_values = checked_mul(
                    static_cast<std::uint64_t>(*rows),
                    stride.has_value() ? *stride : 0);
                const auto required_logical_values = checked_mul(
                    static_cast<std::uint64_t>(*rows),
                    column.descriptor.type_length > 0
                        ? static_cast<std::uint64_t>(
                            column.descriptor.type_length)
                        : 0);
                if (!stride.has_value() || !required_values.has_value()
                    || !required_logical_values.has_value()
                    || column.descriptor.type_length <= 0
                    || chunk.offsets_bytes != 0
                    || chunk.offsets_offset != 0
                    || chunk.values_bytes != *required_values
                    || chunk.logical_values_bytes != *required_logical_values) {
                    return std::unexpected(FormatError::InvalidFixedWidth);
                }
                const auto absolute_payload_offset = checked_add(
                    entry.offset, expected_payload_offset);
                if (!absolute_payload_offset.has_value()) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                const auto aligned_absolute = checked_align_up(
                    *absolute_payload_offset,
                    column.descriptor.type_alignment);
                if (!aligned_absolute.has_value()
                    || *aligned_absolute < entry.offset) {
                    return std::unexpected(FormatError::InvalidAlignment);
                }
                const std::uint64_t aligned =
                    *aligned_absolute - entry.offset;
                const auto next_payload = checked_add(
                    aligned, chunk.values_bytes);
                if (!next_payload.has_value()) {
                    return std::unexpected(FormatError::ArithmeticOverflow);
                }
                if (chunk.values_offset != aligned) {
                    return std::unexpected(FormatError::InvalidFixedWidth);
                }
                for (std::uint64_t padding = expected_payload_offset;
                     padding < aligned;
                     ++padding) {
                    if ((*group)[padding] != std::byte{0}) {
                        return std::unexpected(FormatError::CorruptBlock);
                    }
                }
                auto values = checked_subspan(
                    *group, chunk.values_offset, chunk.values_bytes);
                if (!values.has_value()) {
                    return std::unexpected(FormatError::OffsetOutOfBounds);
                }
                const std::uint64_t width = static_cast<std::uint64_t>(
                    column.descriptor.type_length);
                for (std::uint64_t row = 0; row < *rows; ++row) {
                    const std::uint64_t row_offset = row * *stride;
                    for (std::uint64_t byte = width;
                         byte < *stride;
                         ++byte) {
                        if ((*values)[row_offset + byte] != std::byte{0}) {
                            return std::unexpected(FormatError::CorruptBlock);
                        }
                    }
                }
                if (chunk.null_bitmap_bytes != 0) {
                    auto bitmap = checked_subspan(
                        *group,
                        chunk.null_bitmap_offset,
                        chunk.null_bitmap_bytes);
                    for (std::uint64_t row = 0; row < *rows; ++row) {
                        const bool valid =
                            (std::to_integer<unsigned int>(
                                 (*bitmap)[row / 8u])
                             & (1u << (row % 8u))) != 0;
                        if (valid) {
                            continue;
                        }
                        const std::uint64_t row_offset = row * *stride;
                        for (std::uint64_t byte = 0;
                             byte < *stride;
                             ++byte) {
                            if ((*values)[row_offset + byte]
                                != std::byte{0}) {
                                return std::unexpected(FormatError::CorruptBlock);
                            }
                        }
                    }
                }
                expected_payload_offset = *next_payload;
            }
            auto checksum_input = detail::chunk_checksum_bytes(*group, chunk);
            if (!checksum_input.has_value()) {
                return std::unexpected(checksum_input.error());
            }
            auto computed_chunk_crc = policy.block_crc32c(*checksum_input);
            if (!computed_chunk_crc.has_value()) {
                return std::unexpected(FormatError::ChecksumUnavailable);
            }
            if (*computed_chunk_crc != chunk.crc32c) {
                return std::unexpected(FormatError::CorruptBlock);
            }
            parsed.chunks.push_back(detail::ParsedChunk{.wire = chunk});
        }
        if (expected_payload_offset != entry.length) {
            return std::unexpected(FormatError::InvalidOffsets);
        }
        const auto next_ordinal = checked_add(
            expected_ordinal, static_cast<std::uint64_t>(*rows));
        if (!next_ordinal.has_value()) {
            return std::unexpected(FormatError::ArithmeticOverflow);
        }
        expected_ordinal = *next_ordinal;
        parsed_groups.push_back(std::move(parsed));
    }
    if (expected_ordinal != *row_count) {
        return std::unexpected(FormatError::InvalidRowOrdinal);
    }
    return ColumnStoreView{
        store,
        identity,
        std::move(schema),
        std::move(parsed_groups),
        *row_count,
    };
}

} // namespace bioseq::cache
