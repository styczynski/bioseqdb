/**
 * @file bioseq_caching/core/wire.cppm
 * @brief Fixed-width cache identities, enums, and serialized-record fields.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.cache.core:wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t kSharedAbiVersion = 1;
inline constexpr std::uint32_t kColumnFormatVersion = 1;
inline constexpr std::uint32_t kCoverageDescriptorVersion = 1;
inline constexpr std::uint32_t kSourceMappingFormatVersion = 1;
inline constexpr std::uint32_t kProviderRecipeFormatVersion = 1;
inline constexpr std::array<char, 8> kColumnMagic{
    'B', 'S', 'Q', 'C', 'A', 'C', 'H', 'E'};
inline constexpr std::array<char, 8> kColumnFooterMagic{
    'B', 'S', 'Q', 'C', 'F', 'T', 'R', '1'};

inline constexpr std::uint32_t kMaxGenerationsPerExactDigest = 8;
inline constexpr std::uint32_t kMaxCoverageGenerationsPerDomain = 64;
inline constexpr std::uint32_t kMaxSourcesPerCoverageNode = 32;
inline constexpr std::uint32_t kMaxCoverageNodesPerPlan = 128;
inline constexpr std::uint32_t kMaxProviderBoundaryColumns = 1024;
inline constexpr std::uint32_t kMaxProviderRecipesPerExactPlan = 128;
inline constexpr std::uint32_t kMaxPreciseDirtyDependenciesPerBackend = 256;
inline constexpr std::uint32_t kMaxDependenciesPerGeneration = 1024;
inline constexpr std::uint64_t kMaxCanonicalKeyBytes = 1ULL << 20;
inline constexpr std::uint64_t kMaxCoverageDomainKeyBytes = 1ULL << 20;
inline constexpr std::uint64_t kMaxProviderRecipeBytes = 1ULL << 20;
inline constexpr std::uint64_t kMaxDisplaySqlBytes = 1ULL << 20;
inline constexpr std::uint64_t kMaxPlanTextBytes = 4ULL << 20;
inline constexpr std::uint32_t kDefaultRowGroupRows = 4096;
inline constexpr std::uint64_t kDefaultRowGroupValueBytes = 8ULL << 20;
inline constexpr std::uint64_t kCacheMemoryFractionNumerator = 1;
inline constexpr std::uint64_t kCacheMemoryFractionDenominator = 4;
inline constexpr std::uint64_t kMaxFileCacheBytes = 128ULL << 30;
inline constexpr std::uint64_t kMinFileFreeBytes = 10ULL << 30;

/** A collision-index digest; equality still requires canonical bytes. */
struct CacheDigest256 {
    std::array<std::uint8_t, 32> bytes{};

    [[nodiscard]] bool operator==(
        const CacheDigest256&) const noexcept = default;
};

/** ABA-safe identity for one cache generation. */
struct CacheEntryId {
    CacheDigest256 digest{};
    std::uint64_t boot_nonce = 0;
    std::uint64_t generation = 0;
    std::uint64_t claim_sequence = 0;

    [[nodiscard]] bool operator==(const CacheEntryId&) const noexcept = default;
};

enum class CacheEntryKind : std::uint8_t {
    ExactFinalResult = 1,
    ReusableCoverage = 2,
};

enum class CacheEntryState : std::uint32_t {
    Absent = 0,
    Claimed = 1,
    Building = 2,
    Complete = 3,
    Failed = 4,
    Invalidated = 5,
    Evicting = 6,
};

enum class CacheFailureCode : std::uint32_t {
    None = 0,
    ProducerError,
    ProducerCancelled,
    ProducerExited,
    DependencyChanged,
    SnapshotMismatch,
    AdmissionDenied,
    StorageFailure,
    CorruptResult,
    UnsupportedType,
    RuntimeSemanticDigestUnavailable,
};

enum class CacheExecutionMode : std::uint32_t {
    Bypass = 0,
    ExactRead = 1,
    ExactProduce = 2,
    ReuseMerge = 3,
    ExplainOnly = 4,
};

/** Disclosure level for one producer-owned administrative cache row. */
enum class CacheDiagnosticDisclosure : std::uint8_t {
    SummaryOnly = 0,
    FullText = 1,
};

/**
 * Decide whether one caller may see SQL and producer-plan text for one row.
 * Explicit monitoring grants alone expose bounded scalar summaries; the
 * producing role, a superuser, or the policy monitoring role may see text.
 */
[[nodiscard]] constexpr CacheDiagnosticDisclosure
cache_diagnostic_disclosure(
        bool caller_is_superuser,
        bool caller_is_producing_role,
        bool caller_is_policy_monitor) noexcept {
    return caller_is_superuser || caller_is_producing_role
            || caller_is_policy_monitor
        ? CacheDiagnosticDisclosure::FullText
        : CacheDiagnosticDisclosure::SummaryOnly;
}

enum class DependencyKind : std::uint16_t {
    RelationContent = 1,
    RelationTopology = 2,
    Procedure = 3,
    Operator = 4,
    Type = 5,
    Cast = 6,
    Collation = 7,
    Policy = 8,
    Role = 9,
    Provider = 10,
    SyscacheHash = 11,
    DatabaseWriteEpoch = 12,
    DatabaseDefinitionEpoch = 13,
};

struct DependencyKey {
    std::uint32_t database_oid = 0;
    std::uint32_t class_oid = 0;
    std::uint32_t object_oid = 0;
    std::int32_t object_sub_id = 0;
    DependencyKind kind = DependencyKind::RelationContent;
    std::uint16_t reserved = 0;

    [[nodiscard]] bool operator==(const DependencyKey&) const noexcept = default;
};

enum class PhysicalIdentityKind : std::uint32_t {
    None = 0,
    PgRelFileLocator = 1,
    ProviderOpaque = 2,
};

struct PhysicalIdentityV1 {
    PhysicalIdentityKind kind = PhysicalIdentityKind::None;
    std::uint32_t length = 0;
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] bool operator==(
        const PhysicalIdentityV1&) const noexcept = default;
};

enum class WriteObservability : std::uint32_t {
    None = 0,
    ProviderComplete = 1,
    ExecutorUtilityOnly = 2,
    DatabaseWideFallback = 3,
};

struct DependencyVersion {
    DependencyKey key{};
    std::uint32_t alignment_reserved = 0;
    std::uint64_t committed_epoch = 0;
    PhysicalIdentityV1 physical_identity{};
    WriteObservability observability = WriteObservability::DatabaseWideFallback;
    std::uint32_t reserved = 0;

    [[nodiscard]] bool operator==(
        const DependencyVersion&) const noexcept = default;
};

inline constexpr std::uint32_t kDependencyVectorMagicV1 = 0x42445631U;
inline constexpr std::uint32_t kDependencyVectorAbiVersionV1 = 1;

/** Exact dependency epoch plus the ABA generation of its directory record. */
struct DependencyVectorEntryV1 {
    DependencyVersion version{};
    std::uint64_t record_generation = 0;

    [[nodiscard]] bool operator==(
        const DependencyVectorEntryV1&) const noexcept = default;
};

inline constexpr std::uint32_t kDependencyVectorHeaderBytesV1 = 40;
inline constexpr std::uint32_t kDependencyVectorEntryBytesV1 = 88;

/** Native shared-memory envelope; no process pointer enters persisted bytes. */
struct DependencyVectorHeaderV1 {
    std::uint32_t magic = kDependencyVectorMagicV1;
    std::uint32_t abi_version = kDependencyVectorAbiVersionV1;
    std::uint32_t header_bytes = kDependencyVectorHeaderBytesV1;
    std::uint32_t entry_bytes = kDependencyVectorEntryBytesV1;
    std::uint32_t entry_count = 0;
    std::uint32_t reserved = 0;
    std::uint64_t total_bytes = kDependencyVectorHeaderBytesV1;
    std::uint64_t payload_checksum = 0;
};

enum class DependencyVectorEnvelopeErrorV1 : std::uint32_t {
    None = 0,
    InvalidArgument,
    InvalidSize,
    InvalidMagic,
    AbiMismatch,
    InvalidHeader,
    CountExceeded,
    ChecksumMismatch,
    InvalidEntry,
    NonCanonical,
};

struct DependencyVectorEnvelopeResultV1 {
    DependencyVectorHeaderV1 header{};
    DependencyVectorEnvelopeErrorV1 error =
        DependencyVectorEnvelopeErrorV1::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == DependencyVectorEnvelopeErrorV1::None;
    }
};

/** Exact byte requirement, or zero when the bounded count is invalid. */
[[nodiscard]] constexpr std::size_t dependency_vector_envelope_bytes(
        std::size_t entry_count) noexcept {
    return entry_count <= kMaxDependenciesPerGeneration
        ? kDependencyVectorHeaderBytesV1
            + entry_count * kDependencyVectorEntryBytesV1
        : 0;
}

/** Stable non-cryptographic integrity checksum over canonical entry bytes. */
[[nodiscard]] constexpr std::uint64_t dependency_vector_payload_checksum(
        std::span<const std::byte> payload) noexcept {
    std::uint64_t checksum = 14695981039346656037ULL;
    for (const std::byte value : payload) {
        checksum ^= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(value));
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

[[nodiscard]] constexpr bool dependency_key_less_v1(
        const DependencyKey& left, const DependencyKey& right) noexcept {
    if (left.database_oid != right.database_oid) {
        return left.database_oid < right.database_oid;
    }
    if (left.class_oid != right.class_oid) {
        return left.class_oid < right.class_oid;
    }
    if (left.object_oid != right.object_oid) {
        return left.object_oid < right.object_oid;
    }
    if (left.object_sub_id != right.object_sub_id) {
        return left.object_sub_id < right.object_sub_id;
    }
    return left.kind < right.kind;
}

[[nodiscard]] constexpr bool physical_identity_valid_v1(
        const PhysicalIdentityV1& identity) noexcept {
    if (identity.kind < PhysicalIdentityKind::None
        || identity.kind > PhysicalIdentityKind::ProviderOpaque
        || identity.length > identity.bytes.size()
        || (identity.kind == PhysicalIdentityKind::None
            && identity.length != 0)) {
        return false;
    }
    for (std::size_t index = identity.length;
         index < identity.bytes.size(); ++index) {
        if (identity.bytes[index] != std::byte{}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool dependency_vector_entry_valid_v1(
        const DependencyVectorEntryV1& entry) noexcept {
    const DependencyVersion& version = entry.version;
    const bool observability_valid =
        version.observability >= WriteObservability::ProviderComplete
        && version.observability <= WriteObservability::DatabaseWideFallback;
    return entry.record_generation != 0
        && version.key.database_oid != 0
        && version.key.kind >= DependencyKind::RelationContent
        && version.key.kind <= DependencyKind::DatabaseDefinitionEpoch
        && version.key.reserved == 0
        && version.alignment_reserved == 0
        && version.committed_epoch != 0
        && physical_identity_valid_v1(version.physical_identity)
        && observability_valid
        && version.reserved == 0;
}

/** One database-wide committed-write epoch used as an exact fallback guard. */
[[nodiscard]] constexpr bool database_write_guard_entry_valid_v1(
        const DependencyVectorEntryV1& entry) noexcept {
    const DependencyVersion& version = entry.version;
    return dependency_vector_entry_valid_v1(entry)
        && version.key.class_oid == 0
        && version.key.object_oid == version.key.database_oid
        && version.key.object_sub_id == 0
        && version.key.kind == DependencyKind::DatabaseWriteEpoch
        && version.physical_identity.kind == PhysicalIdentityKind::None
        && version.physical_identity.length == 0
        && version.observability == WriteObservability::DatabaseWideFallback;
}

/** One database-wide committed-definition epoch used by precise generations. */
[[nodiscard]] constexpr bool database_definition_guard_entry_valid_v1(
        const DependencyVectorEntryV1& entry) noexcept {
    const DependencyVersion& version = entry.version;
    return dependency_vector_entry_valid_v1(entry)
        && version.key.class_oid == 0
        && version.key.object_oid == version.key.database_oid
        && version.key.object_sub_id == 0
        && version.key.kind == DependencyKind::DatabaseDefinitionEpoch
        && version.physical_identity.kind == PhysicalIdentityKind::None
        && version.physical_identity.length == 0
        && version.observability == WriteObservability::DatabaseWideFallback;
}

/** Validate the single outside-envelope per-database epoch guard. */
[[nodiscard]] constexpr bool database_guard_entry_valid_v1(
        const DependencyVectorEntryV1& entry) noexcept {
    return database_write_guard_entry_valid_v1(entry)
        || database_definition_guard_entry_valid_v1(entry);
}

/** Only the conservative data-write guard carries a continuing XID oracle. */
[[nodiscard]] constexpr bool database_guard_requires_completion_v1(
        const DependencyKey& key) noexcept {
    return key.kind == DependencyKind::DatabaseWriteEpoch;
}

[[nodiscard]] constexpr bool database_guard_requires_completion_v1(
        const DependencyVectorEntryV1& entry) noexcept {
    return database_guard_entry_valid_v1(entry)
        && database_guard_requires_completion_v1(entry.version.key);
}

/**
 * Validate the PG snapshot-to-dependency handshake without depending on a
 * native SnapshotData layout.  PostgreSQL deliberately clears the copied
 * snapshot field, so a nonzero caller-minted binding is always authoritative;
 * a nonzero snapshot field is an additional equality proof.  The continuing
 * count is optional and is used only by conservative in-flight generations.
 */
[[nodiscard]] constexpr bool dependency_completion_handshake_valid_v1(
        std::uint64_t copied_snapshot_completion_count,
        std::uint64_t snapshot_binding_completion_count,
        std::uint64_t current_completion_count,
        std::uint64_t continuing_completion_count) noexcept {
    return snapshot_binding_completion_count != 0
        && current_completion_count == snapshot_binding_completion_count
        && (copied_snapshot_completion_count == 0
            || copied_snapshot_completion_count
                == snapshot_binding_completion_count)
        && (continuing_completion_count == 0
            || continuing_completion_count == current_completion_count);
}

namespace dependency_vector_wire_detail {

constexpr void write_u16(
        std::span<std::byte> output, std::size_t offset,
        std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

constexpr void write_u32(
        std::span<std::byte> output, std::size_t offset,
        std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

constexpr void write_u64(
        std::span<std::byte> output, std::size_t offset,
        std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] constexpr std::uint16_t read_u16(
        std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(input[offset + index]))
            << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr std::uint32_t read_u32(
        std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(input[offset + index]))
            << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr std::uint64_t read_u64(
        std::span<const std::byte> input, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(input[offset + index]))
            << (index * 8U);
    }
    return value;
}

constexpr void encode_header(
        std::span<std::byte> output,
        const DependencyVectorHeaderV1& header) noexcept {
    write_u32(output, 0, header.magic);
    write_u32(output, 4, header.abi_version);
    write_u32(output, 8, header.header_bytes);
    write_u32(output, 12, header.entry_bytes);
    write_u32(output, 16, header.entry_count);
    write_u32(output, 20, header.reserved);
    write_u64(output, 24, header.total_bytes);
    write_u64(output, 32, header.payload_checksum);
}

[[nodiscard]] constexpr DependencyVectorHeaderV1 decode_header(
        std::span<const std::byte> input) noexcept {
    return DependencyVectorHeaderV1{
        .magic = read_u32(input, 0),
        .abi_version = read_u32(input, 4),
        .header_bytes = read_u32(input, 8),
        .entry_bytes = read_u32(input, 12),
        .entry_count = read_u32(input, 16),
        .reserved = read_u32(input, 20),
        .total_bytes = read_u64(input, 24),
        .payload_checksum = read_u64(input, 32),
    };
}

constexpr void encode_entry(
        std::span<std::byte> output,
        const DependencyVectorEntryV1& entry) noexcept {
    const DependencyVersion& version = entry.version;
    write_u32(output, 0, version.key.database_oid);
    write_u32(output, 4, version.key.class_oid);
    write_u32(output, 8, version.key.object_oid);
    write_u32(
        output, 12,
        std::bit_cast<std::uint32_t>(version.key.object_sub_id));
    write_u16(
        output, 16, static_cast<std::uint16_t>(version.key.kind));
    write_u16(output, 18, version.key.reserved);
    write_u32(output, 20, version.alignment_reserved);
    write_u64(output, 24, version.committed_epoch);
    write_u32(
        output, 32,
        static_cast<std::uint32_t>(version.physical_identity.kind));
    write_u32(output, 36, version.physical_identity.length);
    for (std::size_t index = 0;
         index < version.physical_identity.bytes.size(); ++index) {
        output[40 + index] = version.physical_identity.bytes[index];
    }
    write_u32(
        output, 72, static_cast<std::uint32_t>(version.observability));
    write_u32(output, 76, version.reserved);
    write_u64(output, 80, entry.record_generation);
}

[[nodiscard]] constexpr DependencyVectorEntryV1 decode_entry(
        std::span<const std::byte> input) noexcept {
    DependencyVectorEntryV1 entry{};
    entry.version.key.database_oid = read_u32(input, 0);
    entry.version.key.class_oid = read_u32(input, 4);
    entry.version.key.object_oid = read_u32(input, 8);
    entry.version.key.object_sub_id =
        std::bit_cast<std::int32_t>(read_u32(input, 12));
    entry.version.key.kind =
        static_cast<DependencyKind>(read_u16(input, 16));
    entry.version.key.reserved = read_u16(input, 18);
    entry.version.alignment_reserved = read_u32(input, 20);
    entry.version.committed_epoch = read_u64(input, 24);
    entry.version.physical_identity.kind =
        static_cast<PhysicalIdentityKind>(read_u32(input, 32));
    entry.version.physical_identity.length = read_u32(input, 36);
    for (std::size_t index = 0;
         index < entry.version.physical_identity.bytes.size(); ++index) {
        entry.version.physical_identity.bytes[index] = input[40 + index];
    }
    entry.version.observability =
        static_cast<WriteObservability>(read_u32(input, 72));
    entry.version.reserved = read_u32(input, 76);
    entry.record_generation = read_u64(input, 80);
    return entry;
}

}  // namespace dependency_vector_wire_detail

/** Build an exact, canonical envelope without allocating or retaining pointers. */
[[nodiscard]] inline DependencyVectorEnvelopeResultV1
initialize_dependency_vector_envelope(
        std::span<std::byte> envelope,
        std::span<const DependencyVectorEntryV1> entries) noexcept {
    DependencyVectorEnvelopeResultV1 result{};
    const std::size_t required = dependency_vector_envelope_bytes(entries.size());
    if (required == 0) {
        result.error = DependencyVectorEnvelopeErrorV1::CountExceeded;
        return result;
    }
    if (envelope.data() == nullptr || envelope.size() != required) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidSize;
        return result;
    }
    std::fill(envelope.begin(), envelope.end(), std::byte{});
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!dependency_vector_entry_valid_v1(entries[index])) {
            result.error = DependencyVectorEnvelopeErrorV1::InvalidEntry;
            return result;
        }
        if (index != 0
            && !dependency_key_less_v1(
                entries[index - 1].version.key,
                entries[index].version.key)) {
            result.error = DependencyVectorEnvelopeErrorV1::NonCanonical;
            return result;
        }
    }
    const std::size_t payload_bytes =
        entries.size() * kDependencyVectorEntryBytesV1;
    std::span<std::byte> payload =
        envelope.subspan(kDependencyVectorHeaderBytesV1, payload_bytes);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        dependency_vector_wire_detail::encode_entry(
            payload.subspan(
                index * kDependencyVectorEntryBytesV1,
                kDependencyVectorEntryBytesV1),
            entries[index]);
    }
    result.header = DependencyVectorHeaderV1{
        .entry_count = static_cast<std::uint32_t>(entries.size()),
        .total_bytes = static_cast<std::uint64_t>(required),
        .payload_checksum = dependency_vector_payload_checksum(payload),
    };
    dependency_vector_wire_detail::encode_header(
        envelope.first(kDependencyVectorHeaderBytesV1), result.header);
    return result;
}

/** Validate shape, cap, checksum, entry semantics, and strict canonical order. */
[[nodiscard]] inline DependencyVectorEnvelopeResultV1
validate_dependency_vector_envelope(
        std::span<const std::byte> envelope) noexcept {
    DependencyVectorEnvelopeResultV1 result{};
    if (envelope.data() == nullptr
        || envelope.size() < kDependencyVectorHeaderBytesV1) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidArgument;
        return result;
    }
    result.header = dependency_vector_wire_detail::decode_header(
        envelope.first(kDependencyVectorHeaderBytesV1));
    if (result.header.magic != kDependencyVectorMagicV1) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidMagic;
        return result;
    }
    if (result.header.abi_version != kDependencyVectorAbiVersionV1) {
        result.error = DependencyVectorEnvelopeErrorV1::AbiMismatch;
        return result;
    }
    if (result.header.header_bytes != kDependencyVectorHeaderBytesV1
        || result.header.entry_bytes != kDependencyVectorEntryBytesV1
        || result.header.reserved != 0) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidHeader;
        return result;
    }
    if (result.header.entry_count > kMaxDependenciesPerGeneration) {
        result.error = DependencyVectorEnvelopeErrorV1::CountExceeded;
        return result;
    }
    const std::size_t required =
        dependency_vector_envelope_bytes(result.header.entry_count);
    if (required == 0 || result.header.total_bytes != required
        || envelope.size() != required) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidSize;
        return result;
    }
    const std::span<const std::byte> payload =
        envelope.subspan(kDependencyVectorHeaderBytesV1);
    if (dependency_vector_payload_checksum(payload)
        != result.header.payload_checksum) {
        result.error = DependencyVectorEnvelopeErrorV1::ChecksumMismatch;
        return result;
    }
    DependencyKey previous{};
    bool have_previous = false;
    for (std::uint32_t index = 0; index < result.header.entry_count; ++index) {
        const DependencyVectorEntryV1 entry =
            dependency_vector_wire_detail::decode_entry(
                payload.subspan(
                    static_cast<std::size_t>(index)
                        * kDependencyVectorEntryBytesV1,
                    kDependencyVectorEntryBytesV1));
        if (!dependency_vector_entry_valid_v1(entry)) {
            result.error = DependencyVectorEnvelopeErrorV1::InvalidEntry;
            return result;
        }
        if (have_previous
            && !dependency_key_less_v1(previous, entry.version.key)) {
            result.error = DependencyVectorEnvelopeErrorV1::NonCanonical;
            return result;
        }
        previous = entry.version.key;
        have_previous = true;
    }
    return result;
}

/** Decode every validated entry into caller-owned process-local storage. */
[[nodiscard]] inline DependencyVectorEnvelopeResultV1
copy_dependency_vector_entries(
        std::span<const std::byte> envelope,
        std::span<DependencyVectorEntryV1> output) noexcept {
    DependencyVectorEnvelopeResultV1 result =
        validate_dependency_vector_envelope(envelope);
    if (!result) {
        return result;
    }
    if (output.size() != result.header.entry_count) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidSize;
        return result;
    }
    const std::span<const std::byte> payload =
        envelope.subspan(kDependencyVectorHeaderBytesV1);
    for (std::uint32_t index = 0; index < result.header.entry_count; ++index) {
        output[index] = dependency_vector_wire_detail::decode_entry(
            payload.subspan(
                static_cast<std::size_t>(index)
                    * kDependencyVectorEntryBytesV1,
                kDependencyVectorEntryBytesV1));
    }
    return result;
}

/**
 * Exact SHA input size for a canonical plan envelope and optional database
 * guard.  The completion counter is deliberately absent: it is an in-flight
 * publication handshake, not a committed dependency version.
 */
[[nodiscard]] constexpr std::size_t dependency_fingerprint_material_bytes(
        std::size_t plan_dependency_count,
        bool has_database_guard) noexcept {
    const std::size_t plan_bytes =
        dependency_vector_envelope_bytes(plan_dependency_count);
    return plan_bytes == 0
        ? 0
        : plan_bytes
            + (has_database_guard
                   ? kDependencyVectorEntryBytesV1
                   : 0U);
}

/**
 * Copy stable dependency-fingerprint material without native layout bytes:
 * the validated canonical plan envelope followed by one encoded guard entry.
 */
[[nodiscard]] inline DependencyVectorEnvelopeResultV1
initialize_dependency_fingerprint_material(
        std::span<std::byte> material,
        std::span<const std::byte> canonical_plan_envelope,
        std::span<const DependencyVectorEntryV1> database_guard) noexcept {
    DependencyVectorEnvelopeResultV1 result =
        validate_dependency_vector_envelope(canonical_plan_envelope);
    if (!result) {
        return result;
    }
    if (database_guard.size() > 1) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidArgument;
        return result;
    }
    if (!database_guard.empty()
        && !database_guard_entry_valid_v1(database_guard.front())) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidEntry;
        return result;
    }
    const std::size_t required = dependency_fingerprint_material_bytes(
        result.header.entry_count, !database_guard.empty());
    if (required == 0 || material.data() == nullptr
        || material.size() != required) {
        result.error = DependencyVectorEnvelopeErrorV1::InvalidSize;
        return result;
    }
    std::copy(canonical_plan_envelope.begin(), canonical_plan_envelope.end(),
              material.begin());
    if (!database_guard.empty()) {
        dependency_vector_wire_detail::encode_entry(
            material.subspan(canonical_plan_envelope.size(),
                             kDependencyVectorEntryBytesV1),
            database_guard.front());
    }
    return result;
}

enum class DirectorySlotState : std::uint32_t {
    Empty = 0,
    Occupied = 1,
    Tombstone = 2,
    Retiring = 3,
};

enum class BootstrapState : std::uint32_t {
    Uninitialized = 0,
    Initializing = 1,
    Ready = 2,
    Failed = 3,
};

enum class TransitionOwnerState : std::uint32_t {
    Free = 0,
    Collecting = 1,
    Transitioning = 2,
    Active = 3,
    Repairing = 4,
};

enum class CacheCorrectnessReadiness : std::uint32_t {
    PassthroughOnly = 0,
    RelationFreeOnly = 1,
    GenericMvccReady = 2,
    ProviderCompleteReady = 3,
    ReusableCoverageReady = 4,
};

enum class DataEndian : std::uint8_t {
    Little = 1,
    Big = 2,
};

enum class ColumnEncoding : std::uint16_t {
    BooleanBits = 1,
    FixedByValue = 2,
    FixedByReference = 3,
    FlatVarlena = 4,
    BinarySend = 5,
    Dictionary = 6,
    RunLength = 7,
};

enum class BlockBackingTierV1 : std::uint16_t {
    Dsa = 1,
    File = 2,
};

struct SemanticId128 {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] bool operator==(const SemanticId128&) const noexcept = default;
};

struct ColumnStoreHeaderV1 {
    std::array<char, 8> magic{};
    std::uint32_t format_version = 0;
    std::uint32_t postgres_major = 0;
    std::uint32_t shared_abi_version = 0;
    std::uint32_t flags = 0;
    std::uint64_t boot_nonce = 0;
    std::uint32_t database_oid = 0;
    std::uint32_t owner_role_oid = 0;
    std::uint8_t endian = 0;
    std::uint8_t datum_width = 0;
    std::uint16_t column_count = 0;
    std::uint64_t row_count = 0;
    std::uint64_t row_group_count = 0;
    std::uint64_t column_descriptor_offset = 0;
    std::uint64_t row_group_index_offset = 0;
    CacheDigest256 schema_digest{};
    std::uint32_t header_crc32c = 0;
};

struct ColumnDescriptorV1 {
    std::uint32_t type_oid = 0;
    std::int32_t typmod = -1;
    std::uint32_t collation_oid = 0;
    std::int16_t type_length = 0;
    std::uint8_t by_value = 0;
    std::uint8_t type_alignment = 1;
    ColumnEncoding encoding = ColumnEncoding::BinarySend;
    std::uint16_t flags = 0;
    std::uint32_t send_function_oid = 0;
    std::uint32_t receive_function_oid = 0;
    std::uint32_t receive_type_io_param = 0;
    std::uint32_t reserved = 0;
    std::uint64_t name_offset = 0;

    [[nodiscard]] bool operator==(
        const ColumnDescriptorV1&) const noexcept = default;
};

struct ColumnSchemaWireV1 {
    std::uint32_t attno = 0;
    SemanticId128 semantic_id{};
    std::uint32_t type_oid = 0;
    std::int32_t typmod = -1;
    std::uint32_t collation_oid = 0;
    std::uint32_t flags = 0;

    [[nodiscard]] bool operator==(
        const ColumnSchemaWireV1&) const noexcept = default;
};

struct RowGroupHeaderV1 {
    std::uint64_t first_row_ordinal = 0;
    std::uint32_t row_count = 0;
    std::uint16_t column_count = 0;
    std::uint16_t flags = 0;
    std::uint64_t column_chunk_table_offset = 0;
    std::uint64_t byte_length = 0;
    std::uint32_t group_crc32c = 0;
    std::uint32_t reserved = 0;
};

struct ColumnChunkV1 {
    std::uint64_t null_bitmap_offset = 0;
    std::uint64_t null_bitmap_bytes = 0;
    std::uint64_t offsets_offset = 0;
    std::uint64_t offsets_bytes = 0;
    std::uint64_t values_offset = 0;
    std::uint64_t values_bytes = 0;
    std::uint64_t logical_values_bytes = 0;
    std::uint32_t crc32c = 0;
    std::uint32_t flags = 0;
};

struct BlockReferenceV1 {
    BlockBackingTierV1 tier = BlockBackingTierV1::Dsa;
    std::uint16_t flags = 0;
    std::uint32_t reserved = 0;
    std::uint64_t handle_or_file_id = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t crc32c = 0;
    std::uint32_t reserved2 = 0;
};

enum class RowIdentityKind : std::uint16_t {
    HeapTid = 1,
    BioseqRowNumber = 2,
    ProviderOpaque = 3,
};

struct BaseRowReferenceV1 {
    std::uint32_t rte_ordinal = 0;
    std::uint32_t logical_rel_oid = 0;
    std::uint32_t leaf_rel_oid = 0;
    PhysicalIdentityV1 physical_identity{};
    RowIdentityKind identity_kind = RowIdentityKind::HeapTid;
    std::uint16_t flags = 0;
    std::uint32_t block_number = 0;
    std::uint16_t offset_number = 0;
    std::uint16_t reserved = 0;
    std::uint64_t row_number_or_opaque = 0;
    std::uint64_t xmin_identity = 0;
};

struct ResultOccurrenceV1 {
    std::uint64_t result_ordinal = 0;
    std::uint64_t provider_occurrence_hi = 0;
    std::uint64_t provider_occurrence_lo = 0;
    std::uint32_t base_reference_count = 0;
    std::uint32_t capability_mask = 0;
    std::uint64_t base_reference_offset = 0;
};

enum BioseqMatchCapability : std::uint64_t {
    BIOSEQ_CAP_SCORE = 1ULL << 0,
    BIOSEQ_CAP_DP_SCORE = 1ULL << 1,
    BIOSEQ_CAP_QUERY_COORDS = 1ULL << 2,
    BIOSEQ_CAP_TARGET_COORDS = 1ULL << 3,
    BIOSEQ_CAP_COUNTS = 1ULL << 4,
    BIOSEQ_CAP_MAPQ = 1ULL << 5,
    BIOSEQ_CAP_DIVERGENCE = 1ULL << 6,
    BIOSEQ_CAP_CIGAR = 1ULL << 7,
};

static_assert(std::is_trivially_copyable_v<CacheDigest256>);
static_assert(std::is_trivially_copyable_v<CacheEntryId>);
static_assert(std::is_trivially_copyable_v<DependencyKey>);
static_assert(std::is_trivially_copyable_v<DependencyVersion>);
static_assert(std::is_trivially_copyable_v<DependencyVectorEntryV1>);
static_assert(std::is_standard_layout_v<DependencyVectorEntryV1>);
static_assert(sizeof(DependencyVectorEntryV1)
              == kDependencyVectorEntryBytesV1);
static_assert(std::is_trivially_copyable_v<DependencyVectorHeaderV1>);
static_assert(std::is_standard_layout_v<DependencyVectorHeaderV1>);
static_assert(sizeof(DependencyVectorHeaderV1)
              == kDependencyVectorHeaderBytesV1);
static_assert(std::is_trivially_copyable_v<ColumnStoreHeaderV1>);
static_assert(std::is_trivially_copyable_v<RowGroupHeaderV1>);
static_assert(std::is_trivially_copyable_v<ColumnChunkV1>);
static_assert(std::is_trivially_copyable_v<BlockReferenceV1>);

} // namespace bioseq::cache
