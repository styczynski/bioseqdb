/**
 * @file bioseq_caching/core/wire.cppm
 * @brief Fixed-width cache identities, enums, and serialized-record fields.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>
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
    std::uint64_t committed_epoch = 0;
    PhysicalIdentityV1 physical_identity{};
    WriteObservability observability = WriteObservability::DatabaseWideFallback;
    std::uint32_t reserved = 0;

    [[nodiscard]] bool operator==(
        const DependencyVersion&) const noexcept = default;
};

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
static_assert(std::is_trivially_copyable_v<ColumnStoreHeaderV1>);
static_assert(std::is_trivially_copyable_v<RowGroupHeaderV1>);
static_assert(std::is_trivially_copyable_v<ColumnChunkV1>);
static_assert(std::is_trivially_copyable_v<BlockReferenceV1>);

} // namespace bioseq::cache
