/**
 * @file bioseq_caching/core/reusable_registry.cppm
 * @brief Offset-backed unified exact-result and reusable-coverage registry.
 *
 * The region contains no process pointer, PostgreSQL Node, Datum, or lock.
 * A PostgreSQL adapter allocates the exact byte count during preload and owns
 * the membership/directory locks.  Tests can attach the same engine to an
 * aligned byte vector and supply no-op partition callbacks.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:reusable_registry;

import :coverage;
import :provider_validation;
import :state_machine;
import :ttl;
import :wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t kReusableRegistryMagicV1 = 0x42524347U;
inline constexpr std::uint32_t kReusableRegistryAbiVersionV1 = 8;
inline constexpr std::uint32_t kDefaultReusableRegistryGenerationsV1 = 1'024;
inline constexpr std::uint32_t kDefaultReusableRegistryPartitionsV1 = 16;
inline constexpr std::uint32_t kDefaultReusableRegistryArenaBytesV1 = 16U << 20;
inline constexpr std::uint32_t kMaximumReusableRegistryGenerationsV1 = 65'536;
inline constexpr std::uint32_t kMaximumReusableJanitorBatchV1 = 64;
inline constexpr std::uint32_t kReusableReferenceOwnersPerGenerationV1 = 64;
inline constexpr std::size_t kMaximumReusableJanitorLivenessObservationsV1 =
    static_cast<std::size_t>(kMaximumReusableJanitorBatchV1)
    * (static_cast<std::size_t>(kReusableReferenceOwnersPerGenerationV1)
        + 1U);
inline constexpr std::uint32_t kMaximumReusableInvalidationScanV1 = 64;
inline constexpr std::uint32_t kReusableDisplaySqlCapacityV1 = 1U << 20;
inline constexpr std::uint32_t kReusableCanonicalPlanCapacityV1 = 4U << 20;
inline constexpr std::uint32_t kReusableProducerPlanCapacityV1 = 4U << 20;
inline constexpr std::uint32_t kInvalidReusableRegistrySlotV1 =
    (std::numeric_limits<std::uint32_t>::max)();

/** Every failure is fail-open to ordinary provider execution. */
enum class ReusableRegistryErrorV1 : std::uint32_t {
  None = 0,
  InvalidArgument,
  InvalidConfiguration,
  RegionTooSmall,
  RegionMisaligned,
  NotInitialized,
  AbiMismatch,
  CorruptLayout,
  CorruptDirectory,
  CorruptGeneration,
  DirectoryFull,
  GlobalGenerationLimit,
  ExactBucketFull,
  CoverageBucketFull,
  ByteArenaFull,
  KeyTooLarge,
  DescriptorTooLarge,
  NotFound,
  StaleReference,
  InvalidState,
  CounterOverflow,
  CounterUnderflow,
  ReferenceSlotsFull,
  ManifestMismatch,
  NoReclaimableGeneration,
};

/** Fixed postmaster configuration normalized before shmem is requested. */
struct ReusableRegistryConfigV1 {
  std::uint32_t max_generations = kDefaultReusableRegistryGenerationsV1;
  std::uint32_t exact_directory_capacity = 0;
  std::uint32_t coverage_directory_capacity = 0;
  std::uint32_t partition_count = kDefaultReusableRegistryPartitionsV1;
  std::uint32_t byte_arena_bytes = kDefaultReusableRegistryArenaBytesV1;
  std::uint32_t reserved = 0;
};

/** Exact offsets returned before PostgreSQL's RequestAddinShmemSpace call. */
struct ReusableRegistryLayoutV1 {
  std::uint32_t total_bytes = 0;
  std::uint32_t exact_directory_offset = 0;
  std::uint32_t coverage_directory_offset = 0;
  std::uint32_t generations_offset = 0;
  std::uint32_t free_extents_offset = 0;
  std::uint32_t byte_arena_offset = 0;
  std::uint32_t free_extent_capacity = 0;
  std::uint32_t reserved = 0;
};

/** Adapter-facing resource request, including caller-owned named locks. */
struct ReusableRegistryPreloadRequestV1 {
  ReusableRegistryConfigV1 config{};
  ReusableRegistryLayoutV1 layout{};
  std::uint32_t membership_lock_count = 1;
  std::uint32_t exact_partition_lock_count = 0;
  std::uint32_t coverage_partition_lock_count = 0;
  std::uint32_t reserved = 0;
};

/** Explicit security namespace precedes the bounded 64-member budget. */
struct ReusableSecurityNamespaceV1 {
  std::uint32_t database_oid = 0;
  std::uint32_t current_role_oid = 0;
  std::uint32_t session_user_oid = 0;
  std::uint32_t security_context_flags = 0;
  std::int32_t row_security_mode = 0;
  std::uint32_t reserved = 0;
  CacheDigest256 provider_security_digest{};

  [[nodiscard]] bool
  operator==(const ReusableSecurityNamespaceV1 &) const noexcept = default;
};

/** Explicit provider-independent namespace value; it is still serialized. */
inline constexpr CacheDigest256 kProviderIndependentSecurityDigestV1{};

/**
 * Validate every fixed security field before hashing or directory access.
 * OID zero and a non-Boolean row-security value are invalid sentinels.
 */
[[nodiscard]] constexpr bool valid_reusable_security_namespace(
    const ReusableSecurityNamespaceV1 &value) noexcept {
  return value.database_oid != 0 && value.current_role_oid != 0 &&
         value.session_user_oid != 0 &&
         (value.row_security_mode == 0 || value.row_security_mode == 1) &&
         value.reserved == 0;
}

/** Fixed coverage-directory key; digest collisions still compare domain bytes.
 */
struct ReusableCoverageDirectoryKeyV1 {
  ReusableSecurityNamespaceV1 security_namespace{};
  CacheDigest256 coverage_domain_digest{};

  [[nodiscard]] bool
  operator==(const ReusableCoverageDirectoryKeyV1 &) const noexcept = default;
};

/** Stable POD mirror of storage::ManifestHandle. */
struct ReusableManifestHandleV1 {
  std::uint32_t slot = kInvalidReusableRegistrySlotV1;
  std::uint32_t reserved = 0;
  std::uint64_t slot_sequence = 0;
  CacheEntryId id{};

  [[nodiscard]] bool
  operator==(const ReusableManifestHandleV1 &) const noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept {
    return slot != kInvalidReusableRegistrySlotV1 && slot_sequence != 0 &&
           id.boot_nonce != 0 && id.generation != 0 && id.claim_sequence != 0;
  }
};

/** Liveness identity copied from PostgreSQL fixed-width scalar fields. */
struct ReusableProducerIdentityV1 {
  std::uint32_t proc_number = kInvalidReusableRegistrySlotV1;
  std::int32_t pid = 0;
  std::uint64_t backend_nonce = 0;
  std::int64_t backend_started_at_us = 0;

  [[nodiscard]] bool valid() const noexcept {
    return proc_number != kInvalidReusableRegistrySlotV1 && pid > 0 &&
           backend_nonce != 0 && backend_started_at_us != 0;
  }

  [[nodiscard]] bool
  operator==(const ReusableProducerIdentityV1 &) const noexcept = default;
};

/** Whether bounded producer-plan text is absent, planned, or instrumented. */
enum class ReusablePlanSnapshotStatusV1 : std::uint8_t {
  Unavailable = 0,
  Planned = 1,
  Analyzed = 2,
};

/** Generic backend identity used by every waiter/planner/reader reference. */
using ReusableReferenceOwnerV1 = ReusableProducerIdentityV1;

/** Region-relative immutable bytes. Offset zero is always invalid. */
struct ReusableSharedByteRangeV1 {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return offset != 0 && length != 0;
  }
};

/** Result-semantic and compatibility facts retained with every generation. */
struct ReusableGenerationMetadataV1 {
  std::uint32_t provider_id = 0;
  std::uint32_t provider_abi_version = 0;
  std::uint32_t provider_domain_abi_version = 0;
  std::uint32_t dependency_count = 0;
  std::uint64_t compatibility_family_id = 0;
  std::uint64_t capability_mask = 0;
  CacheDigest256 schema_fingerprint{};
  CacheDigest256 mapping_fingerprint{};
  CacheDigest256 lineage_fingerprint{};
  CacheDigest256 dependency_fingerprint{};
  PhysicalIdentityV1 physical_identity{};
  WriteObservability observability = WriteObservability::DatabaseWideFallback;
  std::uint32_t reserved = 0;
  std::uint64_t estimated_covered_rows = 0;
  std::uint64_t logical_bytes = 0;
  std::uint64_t stored_bytes = 0;
  std::uint64_t producer_ttl_ms = 0;
};

/**
 * One conservative capability retained outside the logical plan vector.
 * It never contributes to metadata.dependency_count or the plan envelope.
 * The completion count is an in-flight/publication guard only; COMPLETE hits
 * validate the retained dependency entry against a fresh snapshot handshake.
 */
struct ReusableConservativeGuardV1 {
  DependencyVectorEntryV1 dependency{};
  std::uint64_t snapshot_completion_count = 0;
  bool active = false;
  std::array<std::uint8_t, 7> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    const bool completion_valid =
        database_guard_requires_completion_v1(dependency)
        ? snapshot_completion_count != 0
        : snapshot_completion_count == 0;
    return active && completion_valid &&
           database_guard_entry_valid_v1(dependency) &&
           std::all_of(reserved.begin(), reserved.end(),
                       [](std::uint8_t value) noexcept {
                         return value == 0;
                       });
  }

  [[nodiscard]] bool
  operator==(const ReusableConservativeGuardV1 &) const noexcept = default;
};

/** ABA-safe public identity; callers never persist a region address. */
struct ReusableGenerationReferenceV1 {
  CacheEntryId id{};
  std::uint32_t generation_slot = kInvalidReusableRegistrySlotV1;
  std::uint32_t reserved = 0;
  std::uint64_t slot_sequence = 0;

  [[nodiscard]] bool
  operator==(const ReusableGenerationReferenceV1 &) const noexcept = default;

  [[nodiscard]] explicit operator bool() const noexcept {
    return generation_slot != kInvalidReusableRegistrySlotV1 &&
           slot_sequence != 0 && id.boot_nonce != 0 && id.generation != 0 &&
           id.claim_sequence != 0;
  }
};

/** Producer-owned diagnostic text update; neither field participates in identity. */
struct ReusableDiagnosticTextUpdateV1 {
  ReusableGenerationReferenceV1 reference{};
  ReusableProducerIdentityV1 producer{};
  std::span<const char> display_sql{};
  std::span<const char> canonical_plan{};
  std::span<const char> producer_plan{};
  ReusablePlanSnapshotStatusV1 canonical_plan_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  ReusablePlanSnapshotStatusV1 plan_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  bool display_sql_truncated = false;
  bool canonical_plan_truncated = false;
  bool producer_plan_truncated = false;
  std::array<std::uint8_t, 3> reserved{};
};

/** Metadata returned before caller-owned buffers receive authorized text. */
struct ReusableDiagnosticTextCopyV1 {
  ReusablePlanSnapshotStatusV1 canonical_plan_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  ReusablePlanSnapshotStatusV1 plan_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  bool display_sql_truncated = false;
  bool canonical_plan_truncated = false;
  bool producer_plan_truncated = false;
  std::array<std::uint8_t, 3> reserved{};
  std::uint32_t display_sql_bytes = 0;
  std::uint32_t canonical_plan_bytes = 0;
  std::uint32_t producer_plan_bytes = 0;
};

enum class ReusableReferenceKindV1 : std::uint8_t {
  None = 0,
  Waiter = 1,
  PlannerPin = 2,
  ReaderPin = 3,
};

/** One fixed shared owner record; sequence prevents slot reuse ABA. */
struct ReusableSharedReferenceOwnerV1 {
  ReusableReferenceOwnerV1 owner{};
  std::uint64_t owner_sequence = 0;
  ReusableReferenceKindV1 kind = ReusableReferenceKindV1::None;
  bool active = false;
  std::array<std::uint8_t, 6> reserved{};
};

/** Backend-local ownership token; release clears active before mutation. */
struct ReusableReferenceTokenV1 {
  ReusableGenerationReferenceV1 reference{};
  ReusableReferenceOwnerV1 owner{};
  std::uint64_t owner_sequence = 0;
  std::uint32_t owner_slot = kInvalidReusableRegistrySlotV1;
  ReusableReferenceKindV1 kind = ReusableReferenceKindV1::None;
  bool active = false;
  std::array<std::uint8_t, 2> reserved{};

  [[nodiscard]] bool
  operator==(const ReusableReferenceTokenV1 &) const noexcept = default;
};

/** Bounded lock-stable copy of every waiter attached to one generation. */
struct ReusableWaiterOwnerBatchV1 {
  std::array<ReusableReferenceTokenV1,
             kReusableReferenceOwnersPerGenerationV1>
      waiters{};
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  std::uint32_t count = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == ReusableRegistryErrorV1::None &&
           count <= waiters.size();
  }
};

/**
 * One lock-stable liveness subject copied before PostgreSQL probes ProcArray.
 * Producer observations carry a generation ABA reference and full owner
 * identity. Reference observations additionally carry the exact shared owner
 * token, including its slot and monotonically assigned sequence.
 */
struct ReusableJanitorOwnerObservationV1 {
  ReusableGenerationReferenceV1 reference{};
  ReusableReferenceOwnerV1 owner{};
  ReusableReferenceTokenV1 reference_token{};
  CacheEntryKind entry_kind = CacheEntryKind::ReusableCoverage;
  bool producer = false;
  std::array<std::uint8_t, 6> reserved{};

  /** Reject malformed, partial, and unknown-kind observations. */
  [[nodiscard]] bool valid() const noexcept {
    const bool known_kind = entry_kind == CacheEntryKind::ExactFinalResult ||
                            entry_kind == CacheEntryKind::ReusableCoverage;
    const bool reserved_clear =
        std::all_of(reserved.begin(), reserved.end(),
                    [](std::uint8_t value) noexcept { return value == 0; });
    const bool token_reserved_clear = std::all_of(
        reference_token.reserved.begin(), reference_token.reserved.end(),
        [](std::uint8_t value) noexcept { return value == 0; });
    if (!known_kind || !reference || reference.reserved != 0 ||
        !owner.valid() || !reserved_clear || !token_reserved_clear) {
      return false;
    }
    if (producer) {
      return !reference_token.active && !reference_token.reference &&
             !reference_token.owner.valid() &&
             reference_token.owner_sequence == 0 &&
             reference_token.owner_slot == kInvalidReusableRegistrySlotV1 &&
             reference_token.kind == ReusableReferenceKindV1::None;
    }
    return reference_token.active &&
           reference_token.reference == reference &&
           reference_token.owner == owner &&
           reference_token.owner_sequence != 0 &&
           reference_token.owner_slot <
               kReusableReferenceOwnersPerGenerationV1 &&
           (reference_token.kind == ReusableReferenceKindV1::Waiter ||
            reference_token.kind == ReusableReferenceKindV1::PlannerPin ||
            reference_token.kind == ReusableReferenceKindV1::ReaderPin);
  }

  [[nodiscard]] bool
  operator==(const ReusableJanitorOwnerObservationV1 &) const noexcept =
      default;
};

enum class ReusableRecordMembershipV1 : std::uint32_t {
  Free = 0,
  Indexed = 1,
  Retired = 2,
};

/** Dependency-reference ownership state for one unified generation. */
enum class ReusableDependencyReleaseStateV1 : std::uint8_t {
  None = 0,
  Retained = 1,
  Pending = 2,
  Prepared = 3,
  Released = 4,
};

/** Open-addressed exact digest bucket; full generation bytes prove equality. */
struct ReusableExactBucketV1 {
  DirectorySlotState slot_state = DirectorySlotState::Empty;
  std::uint32_t reserved = 0;
  CacheDigest256 digest{};
  std::uint32_t first_generation = kInvalidReusableRegistrySlotV1;
  std::uint32_t generation_count = 0;
  std::uint32_t terminal_generation_count = 0;
  std::uint32_t reserved2 = 0;
};

/** Open-addressed domain bucket with the explicit security namespace. */
struct ReusableCoverageBucketV1 {
  DirectorySlotState slot_state = DirectorySlotState::Empty;
  std::uint32_t reserved = 0;
  ReusableCoverageDirectoryKeyV1 key{};
  std::uint32_t first_generation = kInvalidReusableRegistrySlotV1;
  std::uint32_t generation_count = 0;
  std::uint32_t terminal_generation_count = 0;
  std::uint32_t reserved2 = 0;
};

/** One free byte-arena extent, always sorted and coalesced. */
struct ReusableFreeExtentV1 {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
};

/** Fixed-width shared record; all variable data are region-relative ranges. */
struct ReusableSharedGenerationV1 {
  ReusableRecordMembershipV1 membership = ReusableRecordMembershipV1::Free;
  CacheEntryKind entry_kind = CacheEntryKind::ReusableCoverage;
  CacheEntryState state = CacheEntryState::Absent;
  CacheFailureCode failure = CacheFailureCode::None;
  std::uint32_t next_in_exact_bucket = kInvalidReusableRegistrySlotV1;
  std::uint32_t next_in_coverage_bucket = kInvalidReusableRegistrySlotV1;
  std::uint32_t next_retired_or_free = kInvalidReusableRegistrySlotV1;
  std::uint32_t reserved = 0;
  std::uint64_t slot_sequence = 0;
  CacheEntryId id{};
  ReusableCoverageDirectoryKeyV1 coverage_directory_key{};
  ReusableSharedByteRangeV1 canonical_generation_key{};
  ReusableSharedByteRangeV1 canonical_domain_key{};
  ReusableSharedByteRangeV1 coverage_descriptor{};
  ReusableSharedByteRangeV1 dependency_vector{};
  ReusableConservativeGuardV1 conservative_guard{};
  std::uint32_t allocation_offset = 0;
  std::uint32_t allocation_length = 0;
  std::uint32_t dependency_release_cursor = 0;
  ReusableDependencyReleaseStateV1 dependency_release_state =
      ReusableDependencyReleaseStateV1::None;
  std::array<std::uint8_t, 3> reserved_dependency_release{};
  std::uint64_t dependency_release_sequence = 0;
  ReusableProducerIdentityV1 producer{};
  std::uint32_t waiter_count = 0;
  std::uint32_t planner_pin_count = 0;
  std::uint32_t reader_pin_count = 0;
  std::uint32_t reserved_count = 0;
  std::array<ReusableSharedReferenceOwnerV1,
             kReusableReferenceOwnersPerGenerationV1>
      reference_owners{};
  std::uint64_t started_monotonic_ns = 0;
  std::uint64_t completed_monotonic_ns = 0;
  std::uint64_t last_access_monotonic_ns = 0;
  std::uint64_t row_count = 0;
  ReusableGenerationMetadataV1 metadata{};
  ReusableManifestHandleV1 manifest{};
  ReusablePlanSnapshotStatusV1 canonical_plan_snapshot_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  ReusablePlanSnapshotStatusV1 plan_snapshot_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  bool display_sql_truncated = false;
  bool canonical_plan_truncated = false;
  bool producer_plan_truncated = false;
  std::array<std::uint8_t, 3> reserved_diagnostic{};
  ReusableSharedByteRangeV1 display_sql{};
  ReusableSharedByteRangeV1 canonical_plan{};
  ReusableSharedByteRangeV1 producer_plan{};
};

/** Header occupies offset zero; every other structure is layout-addressed. */
struct ReusableRegistryHeaderV1 {
  std::uint32_t magic = 0;
  std::uint32_t abi_version = 0;
  BootstrapState bootstrap_state = BootstrapState::Uninitialized;
  std::uint32_t reserved = 0;
  ReusableRegistryConfigV1 config{};
  ReusableRegistryLayoutV1 layout{};
  std::uint64_t boot_nonce = 0;
  std::uint64_t next_generation = 1;
  std::uint64_t next_claim_sequence = 1;
  std::uint64_t next_slot_sequence = 1;
  std::uint64_t next_reference_sequence = 1;
  std::uint64_t next_dependency_release_sequence = 1;
  std::uint32_t free_generation_head = kInvalidReusableRegistrySlotV1;
  std::uint32_t retired_generation_head = kInvalidReusableRegistrySlotV1;
  std::uint32_t free_extent_count = 0;
  std::uint32_t janitor_scan_cursor = 0;
  std::uint32_t indexed_generations = 0;
  std::uint32_t retired_generations = 0;
  std::uint32_t exact_occupied_buckets = 0;
  std::uint32_t coverage_occupied_buckets = 0;
  std::uint32_t evictions = 0;
  std::uint32_t invalidations = 0;
};

/** Process-local view of a validated region; this pointer is never shared. */
struct ReusableRegistryAttachmentV1 {
  std::span<std::byte> region{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !region.empty();
  }
};

/** Adapter callbacks enforce membership -> exact -> coverage lock order. */
struct ReusableRegistryPartitionCallbacksV1 {
  void *context = nullptr;
  void (*acquire_exact)(void *, std::uint32_t) noexcept = nullptr;
  void (*release_exact)(void *, std::uint32_t) noexcept = nullptr;
  void (*acquire_coverage)(void *, std::uint32_t) noexcept = nullptr;
  void (*release_coverage)(void *, std::uint32_t) noexcept = nullptr;

  [[nodiscard]] bool valid() const noexcept {
    const bool none = acquire_exact == nullptr && release_exact == nullptr &&
                      acquire_coverage == nullptr &&
                      release_coverage == nullptr;
    const bool complete =
        acquire_exact != nullptr && release_exact != nullptr &&
        acquire_coverage != nullptr && release_coverage != nullptr;
    return none || complete;
  }
};

namespace reusable_registry_detail {

[[nodiscard]] constexpr bool power_of_two(std::uint32_t value) noexcept {
  return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] constexpr std::uint32_t
next_power_of_two(std::uint32_t value) noexcept {
  if (value <= 1) {
    return 1;
  }
  --value;
  value |= value >> 1U;
  value |= value >> 2U;
  value |= value >> 4U;
  value |= value >> 8U;
  value |= value >> 16U;
  return value + 1U;
}

[[nodiscard]] constexpr std::uint64_t
align_up(std::uint64_t value, std::uint64_t alignment) noexcept {
  if (alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
      value > (std::numeric_limits<std::uint64_t>::max)() - (alignment - 1U)) {
    return 0;
  }
  return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] inline std::uint64_t mix_hash_byte(std::uint64_t hash,
                                                 std::uint8_t value) noexcept {
  hash ^= value;
  return hash * 1'099'511'628'211ULL;
}

[[nodiscard]] inline std::uint64_t mix_hash_u32(std::uint64_t hash,
                                                std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    hash =
        mix_hash_byte(hash, static_cast<std::uint8_t>(value >> (index * 8U)));
  }
  return hash;
}

[[nodiscard]] inline std::uint64_t
digest_hash(const CacheDigest256 &digest) noexcept {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  for (const std::uint8_t value : digest.bytes) {
    hash = mix_hash_byte(hash, value);
  }
  return hash;
}

[[nodiscard]] inline std::uint64_t
coverage_key_hash(const ReusableCoverageDirectoryKeyV1 &key) noexcept {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  hash = mix_hash_u32(hash, key.security_namespace.database_oid);
  hash = mix_hash_u32(hash, key.security_namespace.current_role_oid);
  hash = mix_hash_u32(hash, key.security_namespace.session_user_oid);
  hash = mix_hash_u32(hash, key.security_namespace.security_context_flags);
  hash = mix_hash_u32(hash, static_cast<std::uint32_t>(
                                key.security_namespace.row_security_mode));
  for (const std::uint8_t value :
       key.security_namespace.provider_security_digest.bytes) {
    hash = mix_hash_byte(hash, value);
  }
  for (const std::uint8_t value : key.coverage_domain_digest.bytes) {
    hash = mix_hash_byte(hash, value);
  }
  return hash;
}

template <class T>
[[nodiscard]] inline T *array_at(ReusableRegistryAttachmentV1 &attachment,
                                 std::uint32_t offset) noexcept {
  return reinterpret_cast<T *>(attachment.region.data() + offset);
}

template <class T>
[[nodiscard]] inline const T *
array_at(const ReusableRegistryAttachmentV1 &attachment,
         std::uint32_t offset) noexcept {
  return reinterpret_cast<const T *>(attachment.region.data() + offset);
}

[[nodiscard]] inline ReusableRegistryHeaderV1 &
header(ReusableRegistryAttachmentV1 &attachment) noexcept {
  return *reinterpret_cast<ReusableRegistryHeaderV1 *>(
      attachment.region.data());
}

[[nodiscard]] inline const ReusableRegistryHeaderV1 &
header(const ReusableRegistryAttachmentV1 &attachment) noexcept {
  return *reinterpret_cast<const ReusableRegistryHeaderV1 *>(
      attachment.region.data());
}

[[nodiscard]] inline ReusableExactBucketV1 *
exact_buckets(ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableExactBucketV1>(
      attachment, header(attachment).layout.exact_directory_offset);
}

[[nodiscard]] inline const ReusableExactBucketV1 *
exact_buckets(const ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableExactBucketV1>(
      attachment, header(attachment).layout.exact_directory_offset);
}

[[nodiscard]] inline ReusableCoverageBucketV1 *
coverage_buckets(ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableCoverageBucketV1>(
      attachment, header(attachment).layout.coverage_directory_offset);
}

[[nodiscard]] inline const ReusableCoverageBucketV1 *
coverage_buckets(const ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableCoverageBucketV1>(
      attachment, header(attachment).layout.coverage_directory_offset);
}

[[nodiscard]] inline ReusableSharedGenerationV1 *
generations(ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableSharedGenerationV1>(
      attachment, header(attachment).layout.generations_offset);
}

[[nodiscard]] inline const ReusableSharedGenerationV1 *
generations(const ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableSharedGenerationV1>(
      attachment, header(attachment).layout.generations_offset);
}

[[nodiscard]] inline ReusableFreeExtentV1 *
free_extents(ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableFreeExtentV1>(
      attachment, header(attachment).layout.free_extents_offset);
}

[[nodiscard]] inline const ReusableFreeExtentV1 *
free_extents(const ReusableRegistryAttachmentV1 &attachment) noexcept {
  return array_at<ReusableFreeExtentV1>(
      attachment, header(attachment).layout.free_extents_offset);
}

[[nodiscard]] inline bool
range_valid(const ReusableRegistryAttachmentV1 &attachment,
            ReusableSharedByteRangeV1 range) noexcept {
  if (!range) {
    return false;
  }
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint64_t start = range.offset;
  const std::uint64_t end = start + range.length;
  const std::uint64_t arena_start = shared.layout.byte_arena_offset;
  const std::uint64_t arena_end = arena_start + shared.config.byte_arena_bytes;
  return start >= arena_start && end >= start && end <= arena_end &&
         end <= shared.layout.total_bytes;
}

[[nodiscard]] inline bool
range_equal(const ReusableRegistryAttachmentV1 &attachment,
            ReusableSharedByteRangeV1 stored,
            std::span<const std::byte> requested) noexcept {
  return range_valid(attachment, stored) && stored.length == requested.size() &&
         std::memcmp(attachment.region.data() + stored.offset, requested.data(),
                     requested.size()) == 0;
}

[[nodiscard]] inline bool terminal_state(CacheEntryState state) noexcept {
  return state == CacheEntryState::Complete ||
         state == CacheEntryState::Failed ||
         state == CacheEntryState::Invalidated;
}

/** Accept only the two wire-stable cache products from DOC0371. */
[[nodiscard]] constexpr bool known_entry_kind(CacheEntryKind kind) noexcept {
  return kind == CacheEntryKind::ExactFinalResult ||
         kind == CacheEntryKind::ReusableCoverage;
}

/** Only reusable coverage participates in candidate-domain discovery. */
[[nodiscard]] constexpr bool
has_coverage_membership(CacheEntryKind kind) noexcept {
  return kind == CacheEntryKind::ReusableCoverage;
}

[[nodiscard]] inline bool
reference_matches(const ReusableSharedGenerationV1 &generation,
                  const ReusableGenerationReferenceV1 &reference,
                  bool allow_retired = false) noexcept {
  const bool membership_ok =
      generation.membership == ReusableRecordMembershipV1::Indexed ||
      (allow_retired &&
       generation.membership == ReusableRecordMembershipV1::Retired);
  return membership_ok && generation.slot_sequence == reference.slot_sequence &&
         generation.id == reference.id;
}

inline void acquire_exact(const ReusableRegistryPartitionCallbacksV1 &callbacks,
                          std::uint32_t partition) noexcept {
  if (callbacks.acquire_exact != nullptr) {
    callbacks.acquire_exact(callbacks.context, partition);
  }
}

inline void release_exact(const ReusableRegistryPartitionCallbacksV1 &callbacks,
                          std::uint32_t partition) noexcept {
  if (callbacks.release_exact != nullptr) {
    callbacks.release_exact(callbacks.context, partition);
  }
}

inline void
acquire_coverage(const ReusableRegistryPartitionCallbacksV1 &callbacks,
                 std::uint32_t partition) noexcept {
  if (callbacks.acquire_coverage != nullptr) {
    callbacks.acquire_coverage(callbacks.context, partition);
  }
}

inline void
release_coverage(const ReusableRegistryPartitionCallbacksV1 &callbacks,
                 std::uint32_t partition) noexcept {
  if (callbacks.release_coverage != nullptr) {
    callbacks.release_coverage(callbacks.context, partition);
  }
}

} // namespace reusable_registry_detail

/** Compute and normalize the exact shared-memory request. */
[[nodiscard]] inline std::expected<ReusableRegistryPreloadRequestV1,
                                   ReusableRegistryErrorV1>
reusable_registry_preload_request(
    ReusableRegistryConfigV1 requested = {}) noexcept {
  using namespace reusable_registry_detail;
  if (requested.reserved != 0 || requested.max_generations == 0 ||
      requested.max_generations > kMaximumReusableRegistryGenerationsV1 ||
      requested.partition_count == 0 ||
      !power_of_two(requested.partition_count) ||
      requested.partition_count > 256 || requested.byte_arena_bytes == 0) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidConfiguration);
  }

  const std::uint64_t minimum_capacity64 =
      static_cast<std::uint64_t>(requested.max_generations) * 2U;
  const std::uint64_t minimum_partitioned64 =
      static_cast<std::uint64_t>(requested.partition_count) * 8U;
  const std::uint64_t minimum64 =
      std::max(minimum_capacity64, minimum_partitioned64);
  if (minimum64 > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidConfiguration);
  }
  const std::uint32_t minimum = static_cast<std::uint32_t>(minimum64);
  if (requested.exact_directory_capacity == 0) {
    requested.exact_directory_capacity = next_power_of_two(minimum);
  }
  if (requested.coverage_directory_capacity == 0) {
    requested.coverage_directory_capacity = next_power_of_two(minimum);
  }
  const bool directory_valid =
      power_of_two(requested.exact_directory_capacity) &&
      power_of_two(requested.coverage_directory_capacity) &&
      requested.exact_directory_capacity >= minimum &&
      requested.coverage_directory_capacity >= minimum &&
      requested.exact_directory_capacity % requested.partition_count == 0 &&
      requested.coverage_directory_capacity % requested.partition_count == 0 &&
      power_of_two(requested.exact_directory_capacity /
                   requested.partition_count) &&
      power_of_two(requested.coverage_directory_capacity /
                   requested.partition_count);
  if (!directory_valid) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidConfiguration);
  }

  ReusableRegistryLayoutV1 layout{};
  std::uint64_t cursor = align_up(sizeof(ReusableRegistryHeaderV1),
                                  alignof(ReusableExactBucketV1));
  layout.exact_directory_offset = static_cast<std::uint32_t>(cursor);
  cursor = align_up(
      cursor + static_cast<std::uint64_t>(requested.exact_directory_capacity) *
                   sizeof(ReusableExactBucketV1),
      alignof(ReusableCoverageBucketV1));
  layout.coverage_directory_offset = static_cast<std::uint32_t>(cursor);
  cursor = align_up(cursor + static_cast<std::uint64_t>(
                                 requested.coverage_directory_capacity) *
                                 sizeof(ReusableCoverageBucketV1),
                    alignof(ReusableSharedGenerationV1));
  layout.generations_offset = static_cast<std::uint32_t>(cursor);
  cursor =
      align_up(cursor + static_cast<std::uint64_t>(requested.max_generations) *
                            sizeof(ReusableSharedGenerationV1),
               alignof(ReusableFreeExtentV1));
  layout.free_extents_offset = static_cast<std::uint32_t>(cursor);
  const std::uint64_t free_extent_capacity64 =
      static_cast<std::uint64_t>(requested.max_generations) * 4U + 4U;
  if (free_extent_capacity64 >
      (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidConfiguration);
  }
  layout.free_extent_capacity =
      static_cast<std::uint32_t>(free_extent_capacity64);
  cursor = align_up(
      cursor + static_cast<std::uint64_t>(layout.free_extent_capacity) *
                   sizeof(ReusableFreeExtentV1),
      8U);
  layout.byte_arena_offset = static_cast<std::uint32_t>(cursor);
  cursor = align_up(cursor + requested.byte_arena_bytes, 8U);
  if (cursor == 0 || cursor > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidConfiguration);
  }
  layout.total_bytes = static_cast<std::uint32_t>(cursor);

  return ReusableRegistryPreloadRequestV1{
      .config = requested,
      .layout = layout,
      .membership_lock_count = 1,
      .exact_partition_lock_count = requested.partition_count,
      .coverage_partition_lock_count = requested.partition_count,
  };
}

/** Validate layout metadata without traversing mutable directory membership. */
[[nodiscard]] inline ReusableRegistryErrorV1
validate_reusable_registry_attachment(
    const ReusableRegistryAttachmentV1 &attachment) noexcept {
  if (attachment.region.size() < sizeof(ReusableRegistryHeaderV1)) {
    return ReusableRegistryErrorV1::RegionTooSmall;
  }
  if (reinterpret_cast<std::uintptr_t>(attachment.region.data()) %
          alignof(ReusableRegistryHeaderV1) !=
      0) {
    return ReusableRegistryErrorV1::RegionMisaligned;
  }
  const ReusableRegistryHeaderV1 &shared =
      reusable_registry_detail::header(attachment);
  if (shared.magic != kReusableRegistryMagicV1) {
    return ReusableRegistryErrorV1::NotInitialized;
  }
  if (shared.abi_version != kReusableRegistryAbiVersionV1) {
    return ReusableRegistryErrorV1::AbiMismatch;
  }
  if (shared.bootstrap_state != BootstrapState::Ready) {
    return ReusableRegistryErrorV1::NotInitialized;
  }
  const auto expected = reusable_registry_preload_request(shared.config);
  if (!expected.has_value() ||
      std::memcmp(&expected->layout, &shared.layout, sizeof(shared.layout)) !=
          0 ||
      shared.layout.total_bytes > attachment.region.size() ||
      shared.boot_nonce == 0 || shared.next_generation == 0 ||
      shared.next_claim_sequence == 0 || shared.next_slot_sequence == 0 ||
      shared.next_reference_sequence == 0 ||
      shared.next_dependency_release_sequence == 0 ||
      shared.free_extent_count > shared.layout.free_extent_capacity ||
      shared.janitor_scan_cursor >= shared.config.max_generations ||
      shared.indexed_generations > shared.config.max_generations ||
      shared.retired_generations > shared.config.max_generations ||
      shared.indexed_generations + shared.retired_generations >
          shared.config.max_generations) {
    return ReusableRegistryErrorV1::CorruptLayout;
  }
  return ReusableRegistryErrorV1::None;
}

/** Attach one process-local view to an already initialized region. */
[[nodiscard]] inline std::expected<ReusableRegistryAttachmentV1,
                                   ReusableRegistryErrorV1>
attach_reusable_registry(std::span<std::byte> region) noexcept {
  ReusableRegistryAttachmentV1 attachment{.region = region};
  const ReusableRegistryErrorV1 error =
      validate_reusable_registry_attachment(attachment);
  if (error != ReusableRegistryErrorV1::None) {
    return std::unexpected(error);
  }
  attachment.region = attachment.region.first(
      reusable_registry_detail::header(attachment).layout.total_bytes);
  return attachment;
}

/** Initialize a new region or validate an existing startup-hook allocation. */
[[nodiscard]] inline std::expected<ReusableRegistryAttachmentV1,
                                   ReusableRegistryErrorV1>
startup_reusable_registry(std::span<std::byte> region, std::uint64_t boot_nonce,
                          ReusableRegistryConfigV1 config = {},
                          bool found = false) noexcept {
  const auto request = reusable_registry_preload_request(config);
  if (!request.has_value()) {
    return std::unexpected(request.error());
  }
  if (boot_nonce == 0 || region.size() < request->layout.total_bytes) {
    return std::unexpected(boot_nonce == 0
                               ? ReusableRegistryErrorV1::InvalidArgument
                               : ReusableRegistryErrorV1::RegionTooSmall);
  }
  if (reinterpret_cast<std::uintptr_t>(region.data()) %
          alignof(ReusableRegistryHeaderV1) !=
      0) {
    return std::unexpected(ReusableRegistryErrorV1::RegionMisaligned);
  }
  if (found) {
    auto attached = attach_reusable_registry(region);
    if (!attached.has_value() ||
        reusable_registry_detail::header(*attached).boot_nonce != boot_nonce ||
        std::memcmp(&reusable_registry_detail::header(*attached).config,
                    &request->config, sizeof(request->config)) != 0) {
      return std::unexpected(attached.has_value()
                                 ? ReusableRegistryErrorV1::AbiMismatch
                                 : attached.error());
    }
    return attached;
  }

  std::memset(region.data(), 0, request->layout.total_bytes);
  ReusableRegistryAttachmentV1 attachment{
      .region = region.first(request->layout.total_bytes),
  };
  ReusableRegistryHeaderV1 &shared =
      reusable_registry_detail::header(attachment);
  shared.magic = kReusableRegistryMagicV1;
  shared.abi_version = kReusableRegistryAbiVersionV1;
  shared.bootstrap_state = BootstrapState::Initializing;
  shared.config = request->config;
  shared.layout = request->layout;
  shared.boot_nonce = boot_nonce;
  shared.next_generation = 1;
  shared.next_claim_sequence = 1;
  shared.next_slot_sequence = 1;
  shared.next_reference_sequence = 1;
  shared.next_dependency_release_sequence = 1;
  shared.free_generation_head = 0;
  shared.retired_generation_head = kInvalidReusableRegistrySlotV1;
  shared.free_extent_count = 1;

  ReusableExactBucketV1 *exact =
      reusable_registry_detail::exact_buckets(attachment);
  for (std::uint32_t bucket = 0;
       bucket < shared.config.exact_directory_capacity; ++bucket) {
    exact[bucket] = ReusableExactBucketV1{};
  }
  ReusableCoverageBucketV1 *coverage =
      reusable_registry_detail::coverage_buckets(attachment);
  for (std::uint32_t bucket = 0;
       bucket < shared.config.coverage_directory_capacity; ++bucket) {
    coverage[bucket] = ReusableCoverageBucketV1{};
  }
  ReusableSharedGenerationV1 *records =
      reusable_registry_detail::generations(attachment);
  for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
    records[slot] = ReusableSharedGenerationV1{};
    records[slot].next_retired_or_free =
        slot + 1U < shared.config.max_generations
            ? slot + 1U
            : kInvalidReusableRegistrySlotV1;
  }
  ReusableFreeExtentV1 *extents =
      reusable_registry_detail::free_extents(attachment);
  extents[0] = ReusableFreeExtentV1{
      .offset = shared.layout.byte_arena_offset,
      .length = shared.config.byte_arena_bytes,
  };
  shared.bootstrap_state = BootstrapState::Ready;
  return attachment;
}

/** Return the exact-directory partition selected by a SHA-256 digest. */
[[nodiscard]] inline std::uint32_t
reusable_exact_partition(const ReusableRegistryAttachmentV1 &attachment,
                         const CacheDigest256 &digest) noexcept {
  return static_cast<std::uint32_t>(
      reusable_registry_detail::digest_hash(digest) &
      (reusable_registry_detail::header(attachment).config.partition_count -
       1U));
}

/** Return the coverage partition after hashing every explicit namespace field.
 */
[[nodiscard]] inline std::uint32_t reusable_coverage_partition(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableCoverageDirectoryKeyV1 &key) noexcept {
  return static_cast<std::uint32_t>(
      reusable_registry_detail::coverage_key_hash(key) &
      (reusable_registry_detail::header(attachment).config.partition_count -
       1U));
}

/** Borrow immutable shared bytes only while the corresponding reference lives.
 */
[[nodiscard]] inline std::span<const std::byte>
reusable_shared_bytes(const ReusableRegistryAttachmentV1 &attachment,
                      ReusableSharedByteRangeV1 range) noexcept {
  if (!reusable_registry_detail::range_valid(attachment, range)) {
    return {};
  }
  return std::span<const std::byte>{
      attachment.region.data() + range.offset,
      range.length,
  };
}

/** Immutable request copied into a newly claimed unified generation. */
struct ReusableClaimRequestV1 {
  CacheEntryKind entry_kind = CacheEntryKind::ReusableCoverage;
  ReusableCoverageDirectoryKeyV1 coverage_directory_key{};
  CacheDigest256 generation_digest{};
  std::span<const std::byte> canonical_domain_key{};
  std::span<const std::byte> canonical_generation_key{};
  std::span<const std::byte> coverage_descriptor{};
  std::span<const std::byte> dependency_vector_envelope{};
  ReusableConservativeGuardV1 conservative_guard{};
  ReusableGenerationMetadataV1 metadata{};
  ReusableProducerIdentityV1 producer{};
  ReusableReferenceOwnerV1 reference_owner{};
  std::uint64_t started_monotonic_ns = 0;
  std::uint64_t consumer_ttl_ms = (std::numeric_limits<std::uint64_t>::max)();
  bool pin_complete = true;
  bool register_waiter = true;
};

enum class ReusableClaimDispositionV1 : std::uint8_t {
  Rejected = 0,
  ClaimedProducer = 1,
  ExistingInFlight = 2,
  ExistingComplete = 3,
};

struct ReusableClaimResultV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  ReusableClaimDispositionV1 disposition = ReusableClaimDispositionV1::Rejected;
  ReusableGenerationReferenceV1 reference{};
  ReusableReferenceTokenV1 token{};
  std::array<ReusableManifestHandleV1, 2> retired_manifests{};
  std::uint32_t retired_manifest_count = 0;
  std::uint32_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == ReusableRegistryErrorV1::None &&
           disposition != ReusableClaimDispositionV1::Rejected;
  }
};

/**
 * ABA token for one lock-dropped dependency-release attempt. The copied
 * envelope is bound by its byte count and payload checksum. `first_reference`
 * is an ordinal across plan entries followed by the optional guard.
 */
struct ReusableDependencyReleaseTicketV1 {
  ReusableGenerationReferenceV1 reference{};
  ReusableManifestHandleV1 manifest{};
  ReusableConservativeGuardV1 conservative_guard{};
  std::uint64_t release_sequence = 0;
  std::uint64_t payload_checksum = 0;
  std::uint32_t first_reference = 0;
  std::uint32_t reference_count = 0;
  std::uint32_t dependency_count = 0;
  std::uint32_t vector_bytes = 0;
  std::uint32_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return reference && release_sequence != 0 &&
           first_reference <= reference_count &&
           dependency_count <= kMaxDependenciesPerGeneration &&
           reference_count ==
               dependency_count +
                   static_cast<std::uint32_t>(conservative_guard.active) &&
           (conservative_guard.active
                ? static_cast<bool>(conservative_guard)
                : conservative_guard == ReusableConservativeGuardV1{}) &&
           vector_bytes ==
               dependency_vector_envelope_bytes(dependency_count) &&
           reserved == 0;
  }

  [[nodiscard]] bool
  operator==(const ReusableDependencyReleaseTicketV1 &) const noexcept =
      default;
};

/** Result of copying and claiming one retired generation's release work. */
struct ReusableDependencyReleasePreparationV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  ReusableDependencyReleaseTicketV1 ticket{};
  std::uint32_t required_vector_bytes = 0;
  std::uint32_t copied_vector_bytes = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == ReusableRegistryErrorV1::None && ticket &&
           copied_vector_bytes == required_vector_bytes;
  }
};

enum class ReusableLookupModeV1 : std::uint8_t {
  Observe = 0,
  RegisterWaiter = 1,
  PinComplete = 2,
};

/** Fixed snapshot returned without exposing the shared record address. */
struct ReusableGenerationSnapshotV1 {
  ReusableGenerationReferenceV1 reference{};
  CacheEntryKind entry_kind = CacheEntryKind::ReusableCoverage;
  ReusableCoverageDirectoryKeyV1 coverage_directory_key{};
  CacheEntryState state = CacheEntryState::Absent;
  CacheFailureCode failure = CacheFailureCode::None;
  ReusableProducerIdentityV1 producer{};
  ReusableGenerationMetadataV1 metadata{};
  ReusableManifestHandleV1 manifest{};
  ReusableSharedByteRangeV1 canonical_generation_key{};
  ReusableSharedByteRangeV1 canonical_domain_key{};
  ReusableSharedByteRangeV1 coverage_descriptor{};
  ReusableSharedByteRangeV1 dependency_vector{};
  ReusableConservativeGuardV1 conservative_guard{};
  std::uint32_t waiter_count = 0;
  std::uint32_t planner_pin_count = 0;
  std::uint32_t reader_pin_count = 0;
  std::uint32_t reserved = 0;
  std::uint64_t started_monotonic_ns = 0;
  std::uint64_t completed_monotonic_ns = 0;
  std::uint64_t last_access_monotonic_ns = 0;
  std::uint64_t row_count = 0;
};

struct ReusableLookupResultV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  ReusableGenerationSnapshotV1 snapshot{};
  ReusableReferenceTokenV1 token{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == ReusableRegistryErrorV1::None &&
           static_cast<bool>(snapshot.reference);
  }
};

/** Coverage lookup input; schema mapping remains a provider callback. */
struct ReusableCandidateRequestV1 {
  ReusableCoverageDirectoryKeyV1 coverage_directory_key{};
  std::span<const std::byte> canonical_domain_key{};
  ReusableReferenceOwnerV1 reference_owner{};
  std::uint32_t provider_id = 0;
  std::uint32_t reserved = 0;
  std::uint64_t compatibility_family_id = 0;
  std::uint64_t required_capability_mask = 0;
};

/** One planner-pinned candidate suitable for existing select_candidates input.
 */
struct ReusableCandidateSnapshotV1 {
  ReusableGenerationSnapshotV1 generation{};
  ReusableReferenceTokenV1 planner_pin{};
  bool complete = false;
  bool in_flight = false;
  std::array<std::uint8_t, 6> reserved{};
};

struct ReusableCandidateBatchV1 {
  std::array<ReusableCandidateSnapshotV1, kMaxCoverageGenerationsPerDomain>
      candidates{};
  std::size_t candidate_count = 0;
  std::size_t visited_count = 0;
  std::size_t domain_collision_count = 0;
  std::size_t invalid_record_count = 0;
  bool bucket_truncated = false;
  bool selection_truncated = false;
};

/** Publish input binds the registry generation to one storage manifest handle.
 */
struct ReusablePublishRequestV1 {
  ReusableGenerationReferenceV1 reference{};
  ReusableManifestHandleV1 manifest{};
  std::uint64_t completed_monotonic_ns = 0;
  std::uint64_t row_count = 0;
  std::uint64_t logical_bytes = 0;
  std::uint64_t stored_bytes = 0;
};

/**
 * One bounded commit-time invalidation selector. Database scope is mandatory;
 * every other equality gate is opt-in. Database-wide coarse fallback is
 * mutually exclusive with every precise gate.
 */
struct ReusableInvalidationSelectorV1 {
  std::uint32_t database_oid = 0;
  std::uint32_t provider_id = 0;
  ReusableSecurityNamespaceV1 security_namespace{};
  PhysicalIdentityV1 physical_identity{};
  CacheDigest256 dependency_fingerprint{};
  CacheDigest256 schema_fingerprint{};
  CacheDigest256 mapping_fingerprint{};
  CacheDigest256 lineage_fingerprint{};
  WriteObservability observability = WriteObservability::DatabaseWideFallback;
  std::uint32_t cursor = 0;
  std::uint32_t maximum_scan = kMaximumReusableInvalidationScanV1;
  bool match_security_namespace = false;
  bool match_provider_id = false;
  bool match_physical_identity = false;
  bool match_dependency_fingerprint = false;
  bool match_schema_fingerprint = false;
  bool match_mapping_fingerprint = false;
  bool match_lineage_fingerprint = false;
  bool match_observability = false;
  bool coarse_database_fallback = false;
  std::array<std::uint8_t, 7> reserved{};
};

/**
 * Deterministic bounded invalidation output. Affected manifests are identities
 * only; this operation never retires, frees, or otherwise mutates storage.
 */
struct ReusableInvalidationResultV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  std::uint32_t next_cursor = 0;
  std::uint32_t scanned_generations = 0;
  std::uint32_t matched_generations = 0;
  std::uint32_t invalidated_complete = 0;
  std::uint32_t failed_inflight = 0;
  std::uint32_t already_invalidated = 0;
  std::uint32_t already_failed = 0;
  std::uint32_t referenced_generations = 0;
  std::uint32_t waiter_references = 0;
  std::uint32_t planner_pins = 0;
  std::uint32_t reader_pins = 0;
  std::array<ReusableGenerationReferenceV1,
             kMaximumReusableInvalidationScanV1>
      affected_references{};
  std::array<ReusableManifestHandleV1, kMaximumReusableInvalidationScanV1>
      affected_manifests{};
  std::array<CacheEntryState, kMaximumReusableInvalidationScanV1>
      previous_states{};
  std::size_t affected_count = 0;
  bool done = false;
};

struct ReusableRegistrySnapshotV1 {
  std::uint64_t boot_nonce = 0;
  std::uint32_t indexed_generations = 0;
  std::uint32_t retired_generations = 0;
  std::uint32_t free_generations = 0;
  std::uint32_t exact_occupied_buckets = 0;
  std::uint32_t coverage_occupied_buckets = 0;
  std::uint32_t exact_final_generations = 0;
  std::uint32_t reusable_coverage_generations = 0;
  std::uint32_t inflight_generations = 0;
  std::uint32_t complete_generations = 0;
  std::uint32_t failed_generations = 0;
  std::uint32_t invalidated_generations = 0;
  std::uint64_t waiter_references = 0;
  std::uint64_t planner_pins = 0;
  std::uint64_t reader_pins = 0;
  std::uint64_t arena_free_bytes = 0;
  std::uint32_t evictions = 0;
  std::uint32_t invalidations = 0;
};

/** Pointer-free, redacted admin row for one currently indexed generation. */
struct ReusableGenerationDiagnosticsRowV1 {
  ReusableGenerationReferenceV1 reference{};
  ReusableManifestHandleV1 manifest{};
  CacheDigest256 dependency_fingerprint{};
  CacheEntryKind entry_kind = CacheEntryKind::ReusableCoverage;
  CacheEntryState state = CacheEntryState::Absent;
  CacheFailureCode failure = CacheFailureCode::None;
  WriteObservability observability = WriteObservability::DatabaseWideFallback;
  std::uint32_t provider_id = 0;
  std::uint32_t provider_abi_version = 0;
  std::uint32_t provider_domain_abi_version = 0;
  std::uint32_t dependency_count = 0;
  std::uint32_t database_oid = 0;
  std::uint32_t current_role_oid = 0;
  std::int32_t producer_pid = 0;
  std::uint32_t waiter_count = 0;
  std::uint32_t planner_pin_count = 0;
  std::uint32_t reader_pin_count = 0;
  std::uint64_t compatibility_family_id = 0;
  std::uint64_t capability_mask = 0;
  std::uint64_t estimated_covered_rows = 0;
  std::uint64_t row_count = 0;
  std::uint64_t logical_bytes = 0;
  std::uint64_t stored_bytes = 0;
  std::uint64_t producer_ttl_ms = 0;
  std::uint64_t conservative_guard_committed_epoch = 0;
  std::uint64_t conservative_guard_record_generation = 0;
  std::uint64_t snapshot_completion_count = 0;
  std::uint64_t started_monotonic_ns = 0;
  std::uint64_t completed_monotonic_ns = 0;
  std::uint64_t last_access_monotonic_ns = 0;
  ReusablePlanSnapshotStatusV1 canonical_plan_snapshot_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  ReusablePlanSnapshotStatusV1 plan_snapshot_status =
      ReusablePlanSnapshotStatusV1::Unavailable;
  bool display_sql_truncated = false;
  bool canonical_plan_truncated = false;
  bool producer_plan_truncated = false;
  bool has_conservative_guard = false;
  std::array<std::uint8_t, 2> reserved_diagnostic{};
};

/** Counts for one caller-bounded diagnostics copy. */
struct ReusableGenerationDiagnosticsResultV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  std::uint32_t indexed_generation_count = 0;
  std::size_t count = 0;
  bool truncated = false;
};

using ReusableJanitorOwnerAliveV1 =
    bool (*)(void *, const ReusableJanitorOwnerObservationV1 &) noexcept;

struct ReusableJanitorCallbacksV1 {
  void *context = nullptr;
  ReusableJanitorOwnerAliveV1 owner_alive = nullptr;
};

/** Counts returned after copying one caller-bounded janitor scan window. */
struct ReusableJanitorLivenessSnapshotV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  std::size_t count = 0;
  std::uint32_t scan_start = 0;
  std::uint32_t scanned_generations = 0;
  bool truncated = false;
};

struct ReusableJanitorSweepResultV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  std::uint32_t reclaimed_retired = 0;
  std::uint32_t retired_expired = 0;
  std::uint32_t retired_failed = 0;
  std::uint32_t retired_invalidated = 0;
  std::uint32_t failed_dead_producers = 0;
  std::uint32_t reaped_waiters = 0;
  std::uint32_t reaped_planner_pins = 0;
  std::uint32_t reaped_reader_pins = 0;
  std::array<ReusableGenerationReferenceV1, kMaximumReusableJanitorBatchV1>
      affected_references{};
  std::size_t affected_reference_count = 0;
  std::array<ReusableManifestHandleV1, kMaximumReusableJanitorBatchV1>
      retired_manifests{};
  std::size_t retired_manifest_count = 0;
};

namespace reusable_registry_detail {

/** Remember one ABA-safe state transition exactly once for lock-drop wakeup. */
[[nodiscard]] inline bool remember_janitor_affected_reference(
    ReusableJanitorSweepResultV1 &result,
    const ReusableGenerationReferenceV1 &reference) noexcept {
  for (std::size_t index = 0; index < result.affected_reference_count;
       ++index) {
    if (result.affected_references[index] == reference) {
      return true;
    }
  }
  if (result.affected_reference_count >= result.affected_references.size()) {
    return false;
  }
  result.affected_references[result.affected_reference_count++] = reference;
  return true;
}

struct DirectoryProbeV1 {
  std::uint32_t slot = kInvalidReusableRegistrySlotV1;
  bool found = false;
};

struct ArenaAllocationV1 {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
};

struct GenerationByteBundleV1 {
  ArenaAllocationV1 allocation{};
  ReusableSharedByteRangeV1 domain_key{};
  ReusableSharedByteRangeV1 generation_key{};
  ReusableSharedByteRangeV1 descriptor{};
  ReusableSharedByteRangeV1 dependency_vector{};
};

/** Three independently replaceable diagnostic arena allocations. */
struct DiagnosticByteBundleV1 {
  ReusableSharedByteRangeV1 display_sql{};
  ReusableSharedByteRangeV1 canonical_plan{};
  ReusableSharedByteRangeV1 producer_plan{};
};

[[nodiscard]] inline std::uint32_t
partition_slice_start(std::uint64_t hash, std::uint32_t capacity,
                      std::uint32_t partitions) noexcept {
  const std::uint32_t partition =
      static_cast<std::uint32_t>(hash & (partitions - 1U));
  return partition * (capacity / partitions);
}

[[nodiscard]] inline std::uint32_t
partition_probe_start(std::uint64_t hash, std::uint32_t capacity,
                      std::uint32_t partitions) noexcept {
  const std::uint32_t slice = capacity / partitions;
  return static_cast<std::uint32_t>(hash / partitions) & (slice - 1U);
}

[[nodiscard]] inline DirectoryProbeV1
probe_exact_directory(const ReusableRegistryAttachmentV1 &attachment,
                      const CacheDigest256 &digest) noexcept {
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint64_t hash = digest_hash(digest);
  const std::uint32_t slice =
      shared.config.exact_directory_capacity / shared.config.partition_count;
  const std::uint32_t base =
      partition_slice_start(hash, shared.config.exact_directory_capacity,
                            shared.config.partition_count);
  const std::uint32_t start =
      partition_probe_start(hash, shared.config.exact_directory_capacity,
                            shared.config.partition_count);
  const ReusableExactBucketV1 *buckets = exact_buckets(attachment);
  std::uint32_t first_tombstone = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t step = 0; step < slice; ++step) {
    const std::uint32_t slot = base + ((start + step) & (slice - 1U));
    const ReusableExactBucketV1 &bucket = buckets[slot];
    if (bucket.slot_state == DirectorySlotState::Occupied &&
        bucket.digest == digest) {
      return DirectoryProbeV1{.slot = slot, .found = true};
    }
    if (bucket.slot_state == DirectorySlotState::Tombstone &&
        first_tombstone == kInvalidReusableRegistrySlotV1) {
      first_tombstone = slot;
    }
    if (bucket.slot_state == DirectorySlotState::Empty) {
      return DirectoryProbeV1{
          .slot = first_tombstone != kInvalidReusableRegistrySlotV1
                      ? first_tombstone
                      : slot,
      };
    }
  }
  return DirectoryProbeV1{.slot = first_tombstone};
}

[[nodiscard]] inline DirectoryProbeV1
probe_coverage_directory(const ReusableRegistryAttachmentV1 &attachment,
                         const ReusableCoverageDirectoryKeyV1 &key) noexcept {
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint64_t hash = coverage_key_hash(key);
  const std::uint32_t slice =
      shared.config.coverage_directory_capacity / shared.config.partition_count;
  const std::uint32_t base =
      partition_slice_start(hash, shared.config.coverage_directory_capacity,
                            shared.config.partition_count);
  const std::uint32_t start =
      partition_probe_start(hash, shared.config.coverage_directory_capacity,
                            shared.config.partition_count);
  const ReusableCoverageBucketV1 *buckets = coverage_buckets(attachment);
  std::uint32_t first_tombstone = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t step = 0; step < slice; ++step) {
    const std::uint32_t slot = base + ((start + step) & (slice - 1U));
    const ReusableCoverageBucketV1 &bucket = buckets[slot];
    if (bucket.slot_state == DirectorySlotState::Occupied &&
        bucket.key == key) {
      return DirectoryProbeV1{.slot = slot, .found = true};
    }
    if (bucket.slot_state == DirectorySlotState::Tombstone &&
        first_tombstone == kInvalidReusableRegistrySlotV1) {
      first_tombstone = slot;
    }
    if (bucket.slot_state == DirectorySlotState::Empty) {
      return DirectoryProbeV1{
          .slot = first_tombstone != kInvalidReusableRegistrySlotV1
                      ? first_tombstone
                      : slot,
      };
    }
  }
  return DirectoryProbeV1{.slot = first_tombstone};
}

[[nodiscard]] inline std::uint64_t take_sequence(std::uint64_t &next) noexcept {
  if (next == 0 || next == (std::numeric_limits<std::uint64_t>::max)()) {
    return 0;
  }
  const std::uint64_t result = next;
  ++next;
  return result;
}

[[nodiscard]] inline std::expected<ArenaAllocationV1, ReusableRegistryErrorV1>
allocate_arena_bytes(ReusableRegistryAttachmentV1 &attachment,
                     std::uint32_t requested_bytes) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint64_t aligned64 = align_up(requested_bytes, 8U);
  if (requested_bytes == 0 || aligned64 == 0 ||
      aligned64 > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  const std::uint32_t aligned = static_cast<std::uint32_t>(aligned64);
  ReusableFreeExtentV1 *extents = free_extents(attachment);
  for (std::uint32_t index = 0; index < shared.free_extent_count; ++index) {
    ReusableFreeExtentV1 &extent = extents[index];
    if (extent.length < aligned) {
      continue;
    }
    const ArenaAllocationV1 allocation{
        .offset = extent.offset,
        .length = aligned,
    };
    extent.offset += aligned;
    extent.length -= aligned;
    if (extent.length == 0) {
      for (std::uint32_t move = index + 1U; move < shared.free_extent_count;
           ++move) {
        extents[move - 1U] = extents[move];
      }
      --shared.free_extent_count;
    }
    return allocation;
  }
  return std::unexpected(ReusableRegistryErrorV1::ByteArenaFull);
}

[[nodiscard]] inline bool
free_arena_bytes(ReusableRegistryAttachmentV1 &attachment,
                 ArenaAllocationV1 allocation) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  if (allocation.offset < shared.layout.byte_arena_offset ||
      allocation.length == 0 ||
      static_cast<std::uint64_t>(allocation.offset) + allocation.length >
          static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
              shared.config.byte_arena_bytes ||
      shared.free_extent_count >= shared.layout.free_extent_capacity) {
    return false;
  }
  ReusableFreeExtentV1 *extents = free_extents(attachment);
  std::uint32_t position = 0;
  while (position < shared.free_extent_count &&
         extents[position].offset < allocation.offset) {
    ++position;
  }
  if ((position > 0 &&
       static_cast<std::uint64_t>(extents[position - 1U].offset) +
               extents[position - 1U].length >
           allocation.offset) ||
      (position<shared.free_extent_count &&static_cast<std::uint64_t>(
                    allocation.offset) +
                allocation.length>
           extents[position]
               .offset)) {
    return false;
  }
  for (std::uint32_t move = shared.free_extent_count; move > position; --move) {
    extents[move] = extents[move - 1U];
  }
  extents[position] = ReusableFreeExtentV1{
      .offset = allocation.offset,
      .length = allocation.length,
  };
  ++shared.free_extent_count;

  if (position > 0) {
    ReusableFreeExtentV1 &previous = extents[position - 1U];
    ReusableFreeExtentV1 &current = extents[position];
    if (previous.offset + previous.length == current.offset) {
      previous.length += current.length;
      for (std::uint32_t move = position + 1U; move < shared.free_extent_count;
           ++move) {
        extents[move - 1U] = extents[move];
      }
      --shared.free_extent_count;
      --position;
    }
  }
  if (position + 1U < shared.free_extent_count) {
    ReusableFreeExtentV1 &current = extents[position];
    ReusableFreeExtentV1 &next = extents[position + 1U];
    if (current.offset + current.length == next.offset) {
      current.length += next.length;
      for (std::uint32_t move = position + 2U; move < shared.free_extent_count;
           ++move) {
        extents[move - 1U] = extents[move];
      }
      --shared.free_extent_count;
    }
  }
  return true;
}

[[nodiscard]] inline std::expected<GenerationByteBundleV1,
                                   ReusableRegistryErrorV1>
copy_generation_bytes(ReusableRegistryAttachmentV1 &attachment,
                      std::span<const std::byte> domain_key,
                      std::span<const std::byte> generation_key,
                      std::span<const std::byte> descriptor,
                      std::span<const std::byte>
                          dependency_vector_envelope) noexcept {
  if (generation_key.empty()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  if (domain_key.size() > kMaxCoverageDomainKeyBytes ||
      generation_key.size() > kMaxCanonicalKeyBytes) {
    return std::unexpected(ReusableRegistryErrorV1::KeyTooLarge);
  }
  if (descriptor.size() > kMaxProviderRecipeBytes) {
    return std::unexpected(ReusableRegistryErrorV1::DescriptorTooLarge);
  }
  const DependencyVectorEnvelopeResultV1 dependency_vector_validation =
      validate_dependency_vector_envelope(dependency_vector_envelope);
  if (!dependency_vector_validation) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  const std::uint64_t total64 =
      domain_key.size() + generation_key.size() + descriptor.size() +
      dependency_vector_envelope.size();
  if (total64 == 0 || total64 > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::ByteArenaFull);
  }
  auto allocation =
      allocate_arena_bytes(attachment, static_cast<std::uint32_t>(total64));
  if (!allocation.has_value()) {
    return std::unexpected(allocation.error());
  }

  GenerationByteBundleV1 bundle{
      .allocation = *allocation,
      .generation_key =
          {
              .offset = allocation->offset +
                        static_cast<std::uint32_t>(domain_key.size()),
              .length = static_cast<std::uint32_t>(generation_key.size()),
          },
  };
  if (!domain_key.empty()) {
    bundle.domain_key = ReusableSharedByteRangeV1{
        .offset = allocation->offset,
        .length = static_cast<std::uint32_t>(domain_key.size()),
    };
  }
  if (!descriptor.empty()) {
    bundle.descriptor = ReusableSharedByteRangeV1{
        .offset = bundle.generation_key.offset + bundle.generation_key.length,
        .length = static_cast<std::uint32_t>(descriptor.size()),
    };
  }
  bundle.dependency_vector = ReusableSharedByteRangeV1{
      .offset = bundle.generation_key.offset + bundle.generation_key.length +
                static_cast<std::uint32_t>(descriptor.size()),
      .length = static_cast<std::uint32_t>(dependency_vector_envelope.size()),
  };
  std::memset(attachment.region.data() + allocation->offset, 0,
              allocation->length);
  if (!domain_key.empty()) {
    std::memcpy(attachment.region.data() + bundle.domain_key.offset,
                domain_key.data(), domain_key.size());
  }
  std::memcpy(attachment.region.data() + bundle.generation_key.offset,
              generation_key.data(), generation_key.size());
  if (!descriptor.empty()) {
    std::memcpy(attachment.region.data() + bundle.descriptor.offset,
                descriptor.data(), descriptor.size());
  }
  std::memcpy(attachment.region.data() + bundle.dependency_vector.offset,
              dependency_vector_envelope.data(),
              dependency_vector_envelope.size());
  return bundle;
}

[[nodiscard]] inline std::expected<std::uint32_t, ReusableRegistryErrorV1>
allocate_generation_record(ReusableRegistryAttachmentV1 &attachment) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  if (shared.indexed_generations + shared.retired_generations >=
          shared.config.max_generations ||
      shared.free_generation_head == kInvalidReusableRegistrySlotV1) {
    return std::unexpected(ReusableRegistryErrorV1::GlobalGenerationLimit);
  }
  const std::uint32_t slot = shared.free_generation_head;
  ReusableSharedGenerationV1 *records = generations(attachment);
  if (slot >= shared.config.max_generations ||
      records[slot].membership != ReusableRecordMembershipV1::Free) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }
  const std::uint32_t next = records[slot].next_retired_or_free;
  const std::uint64_t slot_sequence = take_sequence(shared.next_slot_sequence);
  if (slot_sequence == 0) {
    return std::unexpected(ReusableRegistryErrorV1::CounterOverflow);
  }
  shared.free_generation_head = next;
  records[slot] = ReusableSharedGenerationV1{};
  records[slot].slot_sequence = slot_sequence;
  return slot;
}

inline void return_generation_record(ReusableRegistryAttachmentV1 &attachment,
                                     std::uint32_t slot) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  const std::uint64_t old_sequence = records[slot].slot_sequence;
  records[slot] = ReusableSharedGenerationV1{};
  records[slot].slot_sequence = old_sequence;
  records[slot].next_retired_or_free = shared.free_generation_head;
  shared.free_generation_head = slot;
}

[[nodiscard]] inline ReusableGenerationReferenceV1
make_reference(const ReusableSharedGenerationV1 &generation,
               std::uint32_t slot) noexcept {
  return ReusableGenerationReferenceV1{
      .id = generation.id,
      .generation_slot = slot,
      .slot_sequence = generation.slot_sequence,
  };
}

/** Copy one in-flight producer as a generation-bound liveness subject. */
[[nodiscard]] inline ReusableJanitorOwnerObservationV1
make_producer_observation(const ReusableSharedGenerationV1 &generation,
                          std::uint32_t slot) noexcept {
  return ReusableJanitorOwnerObservationV1{
      .reference = make_reference(generation, slot),
      .owner = generation.producer,
      .entry_kind = generation.entry_kind,
      .producer = true,
  };
}

/** Copy one active reference owner with its complete ABA release token. */
[[nodiscard]] inline ReusableJanitorOwnerObservationV1
make_reference_owner_observation(
    const ReusableSharedGenerationV1 &generation, std::uint32_t slot,
    const ReusableSharedReferenceOwnerV1 &shared_owner,
    std::uint32_t owner_slot) noexcept {
  const ReusableGenerationReferenceV1 reference =
      make_reference(generation, slot);
  const ReusableReferenceTokenV1 token{
      .reference = reference,
      .owner = shared_owner.owner,
      .owner_sequence = shared_owner.owner_sequence,
      .owner_slot = owner_slot,
      .kind = shared_owner.kind,
      .active = shared_owner.active,
  };
  return ReusableJanitorOwnerObservationV1{
      .reference = reference,
      .owner = shared_owner.owner,
      .reference_token = token,
      .entry_kind = generation.entry_kind,
  };
}

[[nodiscard]] inline ReusableGenerationSnapshotV1
make_snapshot(const ReusableSharedGenerationV1 &generation,
              std::uint32_t slot) noexcept {
  return ReusableGenerationSnapshotV1{
      .reference = make_reference(generation, slot),
      .entry_kind = generation.entry_kind,
      .coverage_directory_key = generation.coverage_directory_key,
      .state = generation.state,
      .failure = generation.failure,
      .producer = generation.producer,
      .metadata = generation.metadata,
      .manifest = generation.manifest,
      .canonical_generation_key = generation.canonical_generation_key,
      .canonical_domain_key = generation.canonical_domain_key,
      .coverage_descriptor = generation.coverage_descriptor,
      .dependency_vector = generation.dependency_vector,
      .conservative_guard = generation.conservative_guard,
      .waiter_count = generation.waiter_count,
      .planner_pin_count = generation.planner_pin_count,
      .reader_pin_count = generation.reader_pin_count,
      .started_monotonic_ns = generation.started_monotonic_ns,
      .completed_monotonic_ns = generation.completed_monotonic_ns,
      .last_access_monotonic_ns = generation.last_access_monotonic_ns,
      .row_count = generation.row_count,
  };
}

/** Copy only scalar/digest diagnostics, never shared byte ranges or pointers. */
[[nodiscard]] inline ReusableGenerationDiagnosticsRowV1
make_diagnostics_row(const ReusableSharedGenerationV1 &generation,
                     std::uint32_t slot) noexcept {
  return ReusableGenerationDiagnosticsRowV1{
      .reference = make_reference(generation, slot),
      .manifest = generation.manifest,
      .dependency_fingerprint = generation.metadata.dependency_fingerprint,
      .entry_kind = generation.entry_kind,
      .state = generation.state,
      .failure = generation.failure,
      .observability = generation.metadata.observability,
      .provider_id = generation.metadata.provider_id,
      .provider_abi_version = generation.metadata.provider_abi_version,
      .provider_domain_abi_version =
          generation.metadata.provider_domain_abi_version,
      .dependency_count = generation.metadata.dependency_count,
      .database_oid =
          generation.coverage_directory_key.security_namespace.database_oid,
      .current_role_oid = generation.coverage_directory_key
                              .security_namespace.current_role_oid,
      .producer_pid = generation.producer.pid,
      .waiter_count = generation.waiter_count,
      .planner_pin_count = generation.planner_pin_count,
      .reader_pin_count = generation.reader_pin_count,
      .compatibility_family_id = generation.metadata.compatibility_family_id,
      .capability_mask = generation.metadata.capability_mask,
      .estimated_covered_rows = generation.metadata.estimated_covered_rows,
      .row_count = generation.row_count,
      .logical_bytes = generation.metadata.logical_bytes,
      .stored_bytes = generation.metadata.stored_bytes,
      .producer_ttl_ms = generation.metadata.producer_ttl_ms,
      .conservative_guard_committed_epoch =
          generation.conservative_guard.active
              ? generation.conservative_guard.dependency.version
                    .committed_epoch
              : 0,
      .conservative_guard_record_generation =
          generation.conservative_guard.active
              ? generation.conservative_guard.dependency.record_generation
              : 0,
      .snapshot_completion_count =
          generation.conservative_guard.snapshot_completion_count,
      .started_monotonic_ns = generation.started_monotonic_ns,
      .completed_monotonic_ns = generation.completed_monotonic_ns,
      .last_access_monotonic_ns = generation.last_access_monotonic_ns,
      .canonical_plan_snapshot_status =
          generation.canonical_plan_snapshot_status,
      .plan_snapshot_status = generation.plan_snapshot_status,
      .display_sql_truncated = generation.display_sql_truncated,
      .canonical_plan_truncated = generation.canonical_plan_truncated,
      .producer_plan_truncated = generation.producer_plan_truncated,
      .has_conservative_guard = generation.conservative_guard.active,
  };
}

[[nodiscard]] inline ReusableRegistryErrorV1
link_exact_generation(ReusableRegistryAttachmentV1 &attachment,
                      std::uint32_t generation_slot) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  ReusableSharedGenerationV1 &generation = records[generation_slot];
  if (!known_entry_kind(generation.entry_kind)) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }
  const DirectoryProbeV1 probe =
      probe_exact_directory(attachment, generation.id.digest);
  if (probe.slot == kInvalidReusableRegistrySlotV1) {
    return ReusableRegistryErrorV1::DirectoryFull;
  }
  ReusableExactBucketV1 *buckets = exact_buckets(attachment);
  ReusableExactBucketV1 &bucket = buckets[probe.slot];
  if (probe.found && bucket.generation_count >= kMaxGenerationsPerExactDigest) {
    return ReusableRegistryErrorV1::ExactBucketFull;
  }
  if (!probe.found) {
    bucket = ReusableExactBucketV1{
        .slot_state = DirectorySlotState::Occupied,
        .digest = generation.id.digest,
    };
    ++shared.exact_occupied_buckets;
  }
  generation.next_in_exact_bucket = bucket.first_generation;
  bucket.first_generation = generation_slot;
  ++bucket.generation_count;
  if (terminal_state(generation.state)) {
    ++bucket.terminal_generation_count;
  }
  return ReusableRegistryErrorV1::None;
}

[[nodiscard]] inline ReusableRegistryErrorV1
link_coverage_generation(ReusableRegistryAttachmentV1 &attachment,
                         std::uint32_t generation_slot) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  ReusableSharedGenerationV1 &generation = records[generation_slot];
  if (generation.entry_kind != CacheEntryKind::ReusableCoverage) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }
  const DirectoryProbeV1 probe =
      probe_coverage_directory(attachment, generation.coverage_directory_key);
  if (probe.slot == kInvalidReusableRegistrySlotV1) {
    return ReusableRegistryErrorV1::DirectoryFull;
  }
  ReusableCoverageBucketV1 *buckets = coverage_buckets(attachment);
  ReusableCoverageBucketV1 &bucket = buckets[probe.slot];
  if (probe.found &&
      bucket.generation_count >= kMaxCoverageGenerationsPerDomain) {
    return ReusableRegistryErrorV1::CoverageBucketFull;
  }
  if (!probe.found) {
    bucket = ReusableCoverageBucketV1{
        .slot_state = DirectorySlotState::Occupied,
        .key = generation.coverage_directory_key,
    };
    ++shared.coverage_occupied_buckets;
  }
  generation.next_in_coverage_bucket = bucket.first_generation;
  bucket.first_generation = generation_slot;
  ++bucket.generation_count;
  if (terminal_state(generation.state)) {
    ++bucket.terminal_generation_count;
  }
  return ReusableRegistryErrorV1::None;
}

[[nodiscard]] inline ReusableRegistryErrorV1
unlink_exact_generation(ReusableRegistryAttachmentV1 &attachment,
                        std::uint32_t generation_slot) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  ReusableSharedGenerationV1 &generation = records[generation_slot];
  if (!known_entry_kind(generation.entry_kind)) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }
  const DirectoryProbeV1 probe =
      probe_exact_directory(attachment, generation.id.digest);
  if (!probe.found) {
    return ReusableRegistryErrorV1::CorruptDirectory;
  }
  ReusableExactBucketV1 &bucket = exact_buckets(attachment)[probe.slot];
  std::uint32_t current = bucket.first_generation;
  std::uint32_t previous = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t visited = 0; visited < kMaxGenerationsPerExactDigest &&
                                  current != kInvalidReusableRegistrySlotV1;
       ++visited) {
    if (current >= shared.config.max_generations) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    if (current == generation_slot) {
      if (previous == kInvalidReusableRegistrySlotV1) {
        bucket.first_generation = generation.next_in_exact_bucket;
      } else {
        records[previous].next_in_exact_bucket =
            generation.next_in_exact_bucket;
      }
      if (bucket.generation_count == 0) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      --bucket.generation_count;
      if (terminal_state(generation.state)) {
        if (bucket.terminal_generation_count == 0) {
          return ReusableRegistryErrorV1::CorruptDirectory;
        }
        --bucket.terminal_generation_count;
      }
      generation.next_in_exact_bucket = kInvalidReusableRegistrySlotV1;
      if (bucket.generation_count == 0) {
        bucket = ReusableExactBucketV1{
            .slot_state = DirectorySlotState::Tombstone,
        };
        --shared.exact_occupied_buckets;
      }
      return ReusableRegistryErrorV1::None;
    }
    previous = current;
    current = records[current].next_in_exact_bucket;
  }
  return ReusableRegistryErrorV1::CorruptDirectory;
}

[[nodiscard]] inline ReusableRegistryErrorV1
unlink_coverage_generation(ReusableRegistryAttachmentV1 &attachment,
                           std::uint32_t generation_slot) noexcept {
  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  ReusableSharedGenerationV1 &generation = records[generation_slot];
  if (generation.entry_kind != CacheEntryKind::ReusableCoverage) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }
  const DirectoryProbeV1 probe =
      probe_coverage_directory(attachment, generation.coverage_directory_key);
  if (!probe.found) {
    return ReusableRegistryErrorV1::CorruptDirectory;
  }
  ReusableCoverageBucketV1 &bucket = coverage_buckets(attachment)[probe.slot];
  std::uint32_t current = bucket.first_generation;
  std::uint32_t previous = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t visited = 0; visited < kMaxCoverageGenerationsPerDomain &&
                                  current != kInvalidReusableRegistrySlotV1;
       ++visited) {
    if (current >= shared.config.max_generations) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    if (current == generation_slot) {
      if (previous == kInvalidReusableRegistrySlotV1) {
        bucket.first_generation = generation.next_in_coverage_bucket;
      } else {
        records[previous].next_in_coverage_bucket =
            generation.next_in_coverage_bucket;
      }
      if (bucket.generation_count == 0) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      --bucket.generation_count;
      if (terminal_state(generation.state)) {
        if (bucket.terminal_generation_count == 0) {
          return ReusableRegistryErrorV1::CorruptDirectory;
        }
        --bucket.terminal_generation_count;
      }
      generation.next_in_coverage_bucket = kInvalidReusableRegistrySlotV1;
      if (bucket.generation_count == 0) {
        bucket = ReusableCoverageBucketV1{
            .slot_state = DirectorySlotState::Tombstone,
        };
        --shared.coverage_occupied_buckets;
      }
      return ReusableRegistryErrorV1::None;
    }
    previous = current;
    current = records[current].next_in_coverage_bucket;
  }
  return ReusableRegistryErrorV1::CorruptDirectory;
}

[[nodiscard]] inline ReusableRegistryErrorV1 adjust_terminal_counts(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation, bool increment,
    const ReusableRegistryPartitionCallbacksV1 &callbacks) noexcept {
  const std::uint32_t exact_partition =
      reusable_exact_partition(attachment, generation.id.digest);
  acquire_exact(callbacks, exact_partition);
  const DirectoryProbeV1 exact_probe =
      probe_exact_directory(attachment, generation.id.digest);
  if (!exact_probe.found) {
    release_exact(callbacks, exact_partition);
    return ReusableRegistryErrorV1::CorruptDirectory;
  }
  ReusableExactBucketV1 &exact = exact_buckets(attachment)[exact_probe.slot];
  if (!increment && exact.terminal_generation_count == 0) {
    release_exact(callbacks, exact_partition);
    return ReusableRegistryErrorV1::CorruptDirectory;
  }
  if (increment) {
    ++exact.terminal_generation_count;
  } else {
    --exact.terminal_generation_count;
  }
  release_exact(callbacks, exact_partition);

  if (generation.entry_kind == CacheEntryKind::ExactFinalResult) {
    return ReusableRegistryErrorV1::None;
  }
  if (generation.entry_kind != CacheEntryKind::ReusableCoverage) {
    acquire_exact(callbacks, exact_partition);
    ReusableExactBucketV1 &rollback =
        exact_buckets(attachment)[exact_probe.slot];
    if (increment) {
      --rollback.terminal_generation_count;
    } else {
      ++rollback.terminal_generation_count;
    }
    release_exact(callbacks, exact_partition);
    return ReusableRegistryErrorV1::CorruptGeneration;
  }

  const std::uint32_t coverage_partition = reusable_coverage_partition(
      attachment, generation.coverage_directory_key);
  acquire_coverage(callbacks, coverage_partition);
  const DirectoryProbeV1 coverage_probe =
      probe_coverage_directory(attachment, generation.coverage_directory_key);
  if (!coverage_probe.found) {
    release_coverage(callbacks, coverage_partition);
    acquire_exact(callbacks, exact_partition);
    ReusableExactBucketV1 &rollback =
        exact_buckets(attachment)[exact_probe.slot];
    if (increment) {
      --rollback.terminal_generation_count;
    } else {
      ++rollback.terminal_generation_count;
    }
    release_exact(callbacks, exact_partition);
    return ReusableRegistryErrorV1::CorruptDirectory;
  }
  ReusableCoverageBucketV1 &coverage =
      coverage_buckets(attachment)[coverage_probe.slot];
  if (!increment && coverage.terminal_generation_count == 0) {
    release_coverage(callbacks, coverage_partition);
    acquire_exact(callbacks, exact_partition);
    ReusableExactBucketV1 &rollback =
        exact_buckets(attachment)[exact_probe.slot];
    ++rollback.terminal_generation_count;
    release_exact(callbacks, exact_partition);
    return ReusableRegistryErrorV1::CorruptDirectory;
  }
  if (increment) {
    ++coverage.terminal_generation_count;
  } else {
    --coverage.terminal_generation_count;
  }
  release_coverage(callbacks, coverage_partition);
  return ReusableRegistryErrorV1::None;
}

[[nodiscard]] inline CoverageSourceOrderKey
candidate_order_key(const ReusableCandidateSnapshotV1 &candidate) noexcept {
  return CoverageSourceOrderKey{
      .complete = candidate.complete,
      .estimated_covered_rows =
          candidate.generation.metadata.estimated_covered_rows,
      .stored_bytes = candidate.generation.metadata.stored_bytes,
      .claim_sequence = candidate.generation.reference.id.claim_sequence,
      .digest = candidate.generation.reference.id.digest,
  };
}

[[nodiscard]] inline bool
candidate_precedes(const ReusableCandidateSnapshotV1 &left,
                   const ReusableCandidateSnapshotV1 &right) noexcept {
  return coverage_source_precedes(candidate_order_key(left),
                                  candidate_order_key(right));
}

} // namespace reusable_registry_detail

/** Copy one current record by ABA-safe reference under the caller's membership
 * lock. */
[[nodiscard]] inline std::expected<ReusableGenerationSnapshotV1,
                                   ReusableRegistryErrorV1>
snapshot_reusable_generation(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference) noexcept {
  if (validate_reusable_registry_attachment(attachment) !=
          ReusableRegistryErrorV1::None ||
      !reference ||
      reference.generation_slot >=
          reusable_registry_detail::header(attachment).config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  const ReusableSharedGenerationV1 &generation =
      reusable_registry_detail::generations(
          attachment)[reference.generation_slot];
  if (!reusable_registry_detail::reference_matches(generation, reference)) {
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  return reusable_registry_detail::make_snapshot(generation,
                                                 reference.generation_slot);
}

namespace reusable_registry_detail {

struct MatchingSlotV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  std::uint32_t slot = kInvalidReusableRegistrySlotV1;
};

[[nodiscard]] inline bool claim_dependencies_match(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation,
    const ReusableClaimRequestV1 &request) noexcept {
  if (!range_equal(attachment, generation.dependency_vector,
                   request.dependency_vector_envelope) ||
      generation.conservative_guard.active !=
          request.conservative_guard.active) {
    return false;
  }
  if (!generation.conservative_guard.active) {
    return generation.conservative_guard ==
               ReusableConservativeGuardV1{} &&
           request.conservative_guard ==
               ReusableConservativeGuardV1{};
  }
  if (generation.conservative_guard.dependency !=
      request.conservative_guard.dependency) {
    return false;
  }
  const bool in_flight =
      generation.state == CacheEntryState::Claimed ||
      generation.state == CacheEntryState::Building;
  return !in_flight ||
         generation.conservative_guard.snapshot_completion_count ==
             request.conservative_guard.snapshot_completion_count;
}

[[nodiscard]] inline MatchingSlotV1
find_exact_current(const ReusableRegistryAttachmentV1 &attachment,
                   CacheEntryKind entry_kind,
                   const ReusableCoverageDirectoryKeyV1 &coverage_key,
                   std::span<const std::byte> domain_key,
                   const CacheDigest256 &generation_digest,
                   std::span<const std::byte> generation_key,
                   const ReusableClaimRequestV1 *claim_request =
                       nullptr) noexcept {
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const DirectoryProbeV1 probe =
      probe_exact_directory(attachment, generation_digest);
  if (!probe.found) {
    return MatchingSlotV1{
        .error = ReusableRegistryErrorV1::NotFound,
    };
  }
  const ReusableExactBucketV1 &bucket = exact_buckets(attachment)[probe.slot];
  if (bucket.generation_count > kMaxGenerationsPerExactDigest) {
    return MatchingSlotV1{
        .error = ReusableRegistryErrorV1::CorruptDirectory,
    };
  }
  const ReusableSharedGenerationV1 *records = generations(attachment);
  std::uint32_t current = bucket.first_generation;
  for (std::uint32_t visited = 0; visited < bucket.generation_count;
       ++visited) {
    if (current == kInvalidReusableRegistrySlotV1 ||
        current >= shared.config.max_generations) {
      return MatchingSlotV1{
          .error = ReusableRegistryErrorV1::CorruptDirectory,
      };
    }
    const ReusableSharedGenerationV1 &generation = records[current];
    if (generation.membership != ReusableRecordMembershipV1::Indexed ||
        !known_entry_kind(generation.entry_kind) ||
        generation.id.digest != generation_digest) {
      return MatchingSlotV1{
          .error = ReusableRegistryErrorV1::CorruptGeneration,
      };
    }
    const bool kind_matches = generation.entry_kind == entry_kind;
    const bool coverage_identity_matches =
        entry_kind == CacheEntryKind::ExactFinalResult
            ? generation.coverage_directory_key.security_namespace ==
                  coverage_key.security_namespace &&
                  generation.coverage_directory_key.coverage_domain_digest ==
                      CacheDigest256{}
            : generation.coverage_directory_key == coverage_key &&
                  range_equal(attachment, generation.canonical_domain_key,
                              domain_key);
    if (kind_matches && coverage_identity_matches &&
        range_equal(attachment, generation.canonical_generation_key,
                    generation_key) &&
        (claim_request == nullptr ||
         claim_dependencies_match(
             attachment, generation, *claim_request))) {
      return MatchingSlotV1{.slot = current};
    }
    current = generation.next_in_exact_bucket;
  }
  if (current != kInvalidReusableRegistrySlotV1) {
    return MatchingSlotV1{
        .error = ReusableRegistryErrorV1::CorruptDirectory,
    };
  }
  return MatchingSlotV1{
      .error = ReusableRegistryErrorV1::NotFound,
  };
}

[[nodiscard]] inline std::expected<ReusableReferenceTokenV1,
                                   ReusableRegistryErrorV1>
increment_reference(ReusableRegistryAttachmentV1 &attachment,
                    ReusableSharedGenerationV1 &generation,
                    std::uint32_t slot, ReusableReferenceKindV1 kind,
                    const ReusableReferenceOwnerV1 &owner) noexcept {
  if (!owner.valid()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  std::uint32_t *counter = nullptr;
  switch (kind) {
  case ReusableReferenceKindV1::Waiter:
    if (generation.state != CacheEntryState::Claimed &&
        generation.state != CacheEntryState::Building) {
      return std::unexpected(ReusableRegistryErrorV1::InvalidState);
    }
    counter = &generation.waiter_count;
    break;
  case ReusableReferenceKindV1::PlannerPin:
    if (generation.state != CacheEntryState::Claimed &&
        generation.state != CacheEntryState::Building &&
        generation.state != CacheEntryState::Complete) {
      return std::unexpected(ReusableRegistryErrorV1::InvalidState);
    }
    counter = &generation.planner_pin_count;
    break;
  case ReusableReferenceKindV1::ReaderPin:
    if (generation.state != CacheEntryState::Complete) {
      return std::unexpected(ReusableRegistryErrorV1::InvalidState);
    }
    counter = &generation.reader_pin_count;
    break;
  case ReusableReferenceKindV1::None:
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  default:
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  if (*counter == (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::CounterOverflow);
  }
  std::uint32_t owner_slot = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t index = 0;
       index < kReusableReferenceOwnersPerGenerationV1; ++index) {
    if (!generation.reference_owners[index].active) {
      owner_slot = index;
      break;
    }
  }
  if (owner_slot == kInvalidReusableRegistrySlotV1) {
    return std::unexpected(ReusableRegistryErrorV1::ReferenceSlotsFull);
  }
  const std::uint64_t owner_sequence =
      take_sequence(header(attachment).next_reference_sequence);
  if (owner_sequence == 0) {
    return std::unexpected(ReusableRegistryErrorV1::CounterOverflow);
  }
  generation.reference_owners[owner_slot] = ReusableSharedReferenceOwnerV1{
      .owner = owner,
      .owner_sequence = owner_sequence,
      .kind = kind,
      .active = true,
  };
  ++*counter;
  return ReusableReferenceTokenV1{
      .reference = make_reference(generation, slot),
      .owner = owner,
      .owner_sequence = owner_sequence,
      .owner_slot = owner_slot,
      .kind = kind,
      .active = true,
  };
}

[[nodiscard]] inline std::uint32_t *reference_counter(
    ReusableSharedGenerationV1 &generation,
    ReusableReferenceKindV1 kind) noexcept {
  if (kind == ReusableReferenceKindV1::Waiter) {
    return &generation.waiter_count;
  }
  if (kind == ReusableReferenceKindV1::PlannerPin) {
    return &generation.planner_pin_count;
  }
  if (kind == ReusableReferenceKindV1::ReaderPin) {
    return &generation.reader_pin_count;
  }
  return nullptr;
}

[[nodiscard]] inline bool reference_owner_matches(
    const ReusableSharedReferenceOwnerV1 &shared_owner,
    const ReusableReferenceTokenV1 &token) noexcept {
  return token.owner_slot < kReusableReferenceOwnersPerGenerationV1 &&
         shared_owner.active && shared_owner.kind == token.kind &&
         shared_owner.owner_sequence == token.owner_sequence &&
         shared_owner.owner == token.owner;
}

[[nodiscard]] inline ReusableRegistryErrorV1 deactivate_reference_owner(
    ReusableSharedGenerationV1 &generation,
    ReusableSharedReferenceOwnerV1 &shared_owner) noexcept {
  std::uint32_t *counter = reference_counter(generation, shared_owner.kind);
  if (!shared_owner.active || counter == nullptr) {
    return ReusableRegistryErrorV1::InvalidState;
  }
  if (*counter == 0) {
    return ReusableRegistryErrorV1::CounterUnderflow;
  }
  --*counter;
  shared_owner = ReusableSharedReferenceOwnerV1{};
  return ReusableRegistryErrorV1::None;
}

/** Reject unknown/stale observability enum values before policy checks. */
[[nodiscard]] inline bool
write_observability_valid(WriteObservability observability) noexcept {
  return observability == WriteObservability::None ||
         observability == WriteObservability::ProviderComplete ||
         observability == WriteObservability::ExecutorUtilityOnly ||
         observability == WriteObservability::DatabaseWideFallback;
}

[[nodiscard]] inline bool conservative_guard_valid(
    const ReusableConservativeGuardV1 &guard,
    const ReusableGenerationMetadataV1 &metadata,
    const ReusableSecurityNamespaceV1 &security_namespace) noexcept {
  if (!guard.active) {
    return guard == ReusableConservativeGuardV1{} &&
           metadata.observability !=
               WriteObservability::DatabaseWideFallback;
  }
  const DependencyVectorEntryV1 &entry = guard.dependency;
  const bool conservative_data = metadata.observability ==
      WriteObservability::DatabaseWideFallback;
  const DependencyKind required_kind = conservative_data
      ? DependencyKind::DatabaseWriteEpoch
      : DependencyKind::DatabaseDefinitionEpoch;
  return static_cast<bool>(guard) &&
         database_guard_entry_valid_v1(entry) &&
         entry.version.key.kind == required_kind &&
         entry.version.key.database_oid == security_namespace.database_oid &&
         entry.version.key.object_oid == security_namespace.database_oid;
}

[[nodiscard]] inline bool shared_dependency_vector_valid(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation) noexcept {
  const std::size_t maximum_bytes =
      dependency_vector_envelope_bytes(kMaxDependenciesPerGeneration);
  if (!range_valid(attachment, generation.dependency_vector) ||
      generation.dependency_vector.length > maximum_bytes) {
    return false;
  }
  const std::span<const std::byte> envelope{
      attachment.region.data() + generation.dependency_vector.offset,
      generation.dependency_vector.length,
  };
  const DependencyVectorEnvelopeResultV1 validation =
      validate_dependency_vector_envelope(envelope);
  return validation &&
         validation.header.entry_count == generation.metadata.dependency_count;
}

[[nodiscard]] inline std::uint32_t generation_reference_count(
    const ReusableSharedGenerationV1 &generation) noexcept {
  return generation.metadata.dependency_count +
         static_cast<std::uint32_t>(generation.conservative_guard.active);
}

[[nodiscard]] inline bool
claim_request_valid(const ReusableClaimRequestV1 &request) noexcept {
  const ReusableSecurityNamespaceV1 &security_namespace =
      request.coverage_directory_key.security_namespace;
  const DependencyVectorEnvelopeResultV1 dependency_vector_validation =
      validate_dependency_vector_envelope(
          request.dependency_vector_envelope);
  const bool common_valid =
      known_entry_kind(request.entry_kind) &&
      valid_reusable_security_namespace(security_namespace) &&
      !request.canonical_generation_key.empty() &&
      request.metadata.dependency_count <= kMaxDependenciesPerGeneration &&
      dependency_vector_validation &&
      dependency_vector_validation.header.entry_count ==
          request.metadata.dependency_count &&
      request.metadata.reserved == 0 &&
      request.metadata.physical_identity.length <=
          request.metadata.physical_identity.bytes.size() &&
      write_observability_valid(request.metadata.observability) &&
      conservative_guard_valid(
          request.conservative_guard, request.metadata,
          security_namespace) &&
      request.producer.valid() &&
      ((!request.pin_complete && !request.register_waiter) ||
       request.reference_owner.valid());
  if (!common_valid) {
    return false;
  }
  if (request.entry_kind == CacheEntryKind::ReusableCoverage) {
    return !request.canonical_domain_key.empty() &&
           request.metadata.provider_id != 0 &&
           request.metadata.provider_abi_version != 0 &&
           request.metadata.provider_domain_abi_version != 0 &&
           request.metadata.compatibility_family_id != 0 &&
           request.metadata.observability ==
               WriteObservability::ProviderComplete;
  }
  const bool provider_metadata_consistent =
      request.metadata.provider_id == 0
          ? request.metadata.provider_abi_version == 0 &&
                request.metadata.provider_domain_abi_version == 0 &&
                request.metadata.observability !=
                    WriteObservability::ProviderComplete
          : request.metadata.provider_abi_version != 0 &&
                request.metadata.provider_domain_abi_version != 0 &&
                request.metadata.observability ==
                    WriteObservability::ProviderComplete;
  return request.canonical_domain_key.empty() &&
         request.coverage_descriptor.empty() &&
         request.coverage_directory_key.coverage_domain_digest ==
             CacheDigest256{} &&
         provider_metadata_consistent;
}

[[nodiscard]] inline bool diagnostic_status_valid(
    ReusablePlanSnapshotStatusV1 status) noexcept {
  return status == ReusablePlanSnapshotStatusV1::Unavailable ||
         status == ReusablePlanSnapshotStatusV1::Planned ||
         status == ReusablePlanSnapshotStatusV1::Analyzed;
}

[[nodiscard]] inline bool diagnostic_text_valid(
    std::span<const char> text, std::size_t capacity) noexcept {
  return text.size() <= capacity &&
         (text.empty() ||
          std::memchr(text.data(), 0, text.size()) == nullptr);
}

[[nodiscard]] inline bool diagnostic_capture_valid(
    std::span<const char> text, std::size_t capacity,
    ReusablePlanSnapshotStatusV1 status, bool truncated,
    bool analyzed_allowed) noexcept {
  return diagnostic_status_valid(status) &&
         (analyzed_allowed ||
          status != ReusablePlanSnapshotStatusV1::Analyzed) &&
         diagnostic_text_valid(text, capacity) &&
         (!text.empty() ||
          (status == ReusablePlanSnapshotStatusV1::Unavailable &&
           !truncated));
}

[[nodiscard]] inline bool diagnostic_update_valid(
    const ReusableDiagnosticTextUpdateV1 &update) noexcept {
  return update.reference && update.producer.valid() &&
         diagnostic_text_valid(update.display_sql,
                               kReusableDisplaySqlCapacityV1) &&
         !update.display_sql.empty() &&
         diagnostic_capture_valid(
             update.canonical_plan, kReusableCanonicalPlanCapacityV1,
             update.canonical_plan_status,
             update.canonical_plan_truncated, false) &&
         diagnostic_capture_valid(
             update.producer_plan, kReusableProducerPlanCapacityV1,
             update.plan_status, update.producer_plan_truncated, true) &&
         std::all_of(update.reserved.begin(), update.reserved.end(),
                     [](std::uint8_t value) noexcept { return value == 0; });
}

[[nodiscard]] inline bool empty_range_shape(
    ReusableSharedByteRangeV1 range) noexcept {
  return range.offset == 0 && range.length == 0;
}

[[nodiscard]] inline std::uint32_t diagnostic_allocation_length(
    ReusableSharedByteRangeV1 range) noexcept {
  const std::uint64_t aligned = align_up(range.length, 8U);
  return aligned == 0 ||
                 aligned > (std::numeric_limits<std::uint32_t>::max)()
             ? 0
             : static_cast<std::uint32_t>(aligned);
}

[[nodiscard]] inline bool diagnostic_range_layout_valid(
    const ReusableRegistryAttachmentV1 &attachment,
    ReusableSharedByteRangeV1 range, std::uint32_t maximum) noexcept {
  if (range.length == 0) {
    return range.offset == 0;
  }
  const std::uint32_t allocation_length =
      diagnostic_allocation_length(range);
  if (range.offset == 0 || range.offset % 8U != 0 ||
      range.length > maximum || allocation_length == 0 ||
      !range_valid(attachment, range)) {
    return false;
  }
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint64_t allocation_end =
      static_cast<std::uint64_t>(range.offset) + allocation_length;
  return allocation_end <=
         static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
             shared.config.byte_arena_bytes;
}

[[nodiscard]] inline bool diagnostic_range_valid(
    const ReusableRegistryAttachmentV1 &attachment,
    ReusableSharedByteRangeV1 range, std::uint32_t maximum) noexcept {
  return diagnostic_range_layout_valid(attachment, range, maximum) &&
         (range.length == 0 ||
          std::memchr(attachment.region.data() + range.offset, 0,
                      range.length) == nullptr);
}

[[nodiscard]] inline std::span<const char> diagnostic_text_span(
    const ReusableRegistryAttachmentV1 &attachment,
    ReusableSharedByteRangeV1 range) noexcept {
  if (range.length == 0) {
    return {};
  }
  return {
      reinterpret_cast<const char *>(attachment.region.data() + range.offset),
      range.length,
  };
}

[[nodiscard]] inline bool arena_allocations_overlap(
    ArenaAllocationV1 left, ArenaAllocationV1 right) noexcept {
  if (left.length == 0 || right.length == 0) {
    return false;
  }
  const std::uint64_t left_end =
      static_cast<std::uint64_t>(left.offset) + left.length;
  const std::uint64_t right_end =
      static_cast<std::uint64_t>(right.offset) + right.length;
  return left.offset < right_end && right.offset < left_end;
}

[[nodiscard]] inline ArenaAllocationV1 diagnostic_allocation(
    ReusableSharedByteRangeV1 range) noexcept {
  return ArenaAllocationV1{
      .offset = range.offset,
      .length = range.length == 0 ? 0U : diagnostic_allocation_length(range),
  };
}

[[nodiscard]] inline DiagnosticByteBundleV1 diagnostic_bundle(
    const ReusableSharedGenerationV1 &generation) noexcept {
  return DiagnosticByteBundleV1{
      .display_sql = generation.display_sql,
      .canonical_plan = generation.canonical_plan,
      .producer_plan = generation.producer_plan,
  };
}

[[nodiscard]] inline bool free_diagnostic_range(
    ReusableRegistryAttachmentV1 &attachment,
    ReusableSharedByteRangeV1 range) noexcept {
  if (range.length == 0) {
    return range.offset == 0;
  }
  if (!diagnostic_range_layout_valid(
          attachment, range,
          (std::numeric_limits<std::uint32_t>::max)())) {
    return false;
  }
  return free_arena_bytes(attachment, diagnostic_allocation(range));
}

[[nodiscard]] inline bool free_diagnostic_bundle(
    ReusableRegistryAttachmentV1 &attachment,
    const DiagnosticByteBundleV1 &bundle) noexcept {
  bool valid = true;
  if (!free_diagnostic_range(attachment, bundle.display_sql)) {
    valid = false;
  }
  if (!free_diagnostic_range(attachment, bundle.canonical_plan)) {
    valid = false;
  }
  if (!free_diagnostic_range(attachment, bundle.producer_plan)) {
    valid = false;
  }
  return valid;
}

[[nodiscard]] inline std::expected<ReusableSharedByteRangeV1,
                                   ReusableRegistryErrorV1>
copy_diagnostic_range(ReusableRegistryAttachmentV1 &attachment,
                      std::span<const char> text) noexcept {
  if (text.empty()) {
    return ReusableSharedByteRangeV1{};
  }
  if (text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  auto allocation = allocate_arena_bytes(
      attachment, static_cast<std::uint32_t>(text.size()));
  if (!allocation.has_value()) {
    return std::unexpected(allocation.error());
  }
  std::memset(attachment.region.data() + allocation->offset, 0,
              allocation->length);
  std::memcpy(attachment.region.data() + allocation->offset, text.data(),
              text.size());
  return ReusableSharedByteRangeV1{
      .offset = allocation->offset,
      .length = static_cast<std::uint32_t>(text.size()),
  };
}

[[nodiscard]] inline std::expected<DiagnosticByteBundleV1,
                                   ReusableRegistryErrorV1>
copy_diagnostic_bytes(ReusableRegistryAttachmentV1 &attachment,
                      const ReusableDiagnosticTextUpdateV1 &update) noexcept {
  DiagnosticByteBundleV1 bundle{};
  auto display = copy_diagnostic_range(attachment, update.display_sql);
  if (!display.has_value()) {
    return std::unexpected(display.error());
  }
  bundle.display_sql = *display;

  auto canonical = copy_diagnostic_range(attachment, update.canonical_plan);
  if (!canonical.has_value()) {
    const ReusableRegistryErrorV1 error = canonical.error();
    return free_diagnostic_bundle(attachment, bundle)
               ? std::unexpected(error)
               : std::unexpected(ReusableRegistryErrorV1::CorruptLayout);
  }
  bundle.canonical_plan = *canonical;

  auto producer = copy_diagnostic_range(attachment, update.producer_plan);
  if (!producer.has_value()) {
    const ReusableRegistryErrorV1 error = producer.error();
    return free_diagnostic_bundle(attachment, bundle)
               ? std::unexpected(error)
               : std::unexpected(ReusableRegistryErrorV1::CorruptLayout);
  }
  bundle.producer_plan = *producer;
  return bundle;
}

[[nodiscard]] inline bool shared_diagnostic_layout_valid(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation) noexcept {
  if (!std::all_of(generation.reserved_diagnostic.begin(),
                   generation.reserved_diagnostic.end(),
                   [](std::uint8_t value) noexcept { return value == 0; }) ||
      !diagnostic_range_layout_valid(attachment, generation.display_sql,
                                     kReusableDisplaySqlCapacityV1) ||
      !diagnostic_range_layout_valid(attachment, generation.canonical_plan,
                                     kReusableCanonicalPlanCapacityV1) ||
      !diagnostic_range_layout_valid(attachment, generation.producer_plan,
                                     kReusableProducerPlanCapacityV1)) {
    return false;
  }
  const ArenaAllocationV1 identity{
      .offset = generation.allocation_offset,
      .length = generation.allocation_length,
  };
  const ArenaAllocationV1 display =
      diagnostic_allocation(generation.display_sql);
  const ArenaAllocationV1 canonical =
      diagnostic_allocation(generation.canonical_plan);
  const ArenaAllocationV1 producer =
      diagnostic_allocation(generation.producer_plan);
  return !arena_allocations_overlap(identity, display) &&
         !arena_allocations_overlap(identity, canonical) &&
         !arena_allocations_overlap(identity, producer) &&
         !arena_allocations_overlap(display, canonical) &&
         !arena_allocations_overlap(display, producer) &&
         !arena_allocations_overlap(canonical, producer);
}

[[nodiscard]] inline bool shared_diagnostic_text_valid(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation) noexcept {
  if (!shared_diagnostic_layout_valid(attachment, generation) ||
      !diagnostic_range_valid(attachment, generation.display_sql,
                              kReusableDisplaySqlCapacityV1) ||
      !diagnostic_range_valid(attachment, generation.canonical_plan,
                              kReusableCanonicalPlanCapacityV1) ||
      !diagnostic_range_valid(attachment, generation.producer_plan,
                              kReusableProducerPlanCapacityV1)) {
    return false;
  }
  const std::span<const char> display_sql =
      diagnostic_text_span(attachment, generation.display_sql);
  const std::span<const char> canonical_plan =
      diagnostic_text_span(attachment, generation.canonical_plan);
  const std::span<const char> producer_plan =
      diagnostic_text_span(attachment, generation.producer_plan);
  const bool has_sql = !display_sql.empty();
  const bool analyzed_state_valid =
      generation.plan_snapshot_status !=
          ReusablePlanSnapshotStatusV1::Analyzed ||
      generation.state == CacheEntryState::Building ||
      generation.state == CacheEntryState::Complete ||
      generation.state == CacheEntryState::Failed ||
      generation.state == CacheEntryState::Invalidated ||
      generation.state == CacheEntryState::Evicting;
  if ((!has_sql && generation.display_sql_truncated) ||
      !diagnostic_text_valid(display_sql, kReusableDisplaySqlCapacityV1) ||
      !diagnostic_capture_valid(
          canonical_plan, kReusableCanonicalPlanCapacityV1,
          generation.canonical_plan_snapshot_status,
          generation.canonical_plan_truncated, false) ||
      !diagnostic_capture_valid(
          producer_plan, kReusableProducerPlanCapacityV1,
          generation.plan_snapshot_status,
          generation.producer_plan_truncated, true) ||
      (generation.plan_snapshot_status ==
           ReusablePlanSnapshotStatusV1::Analyzed &&
       !analyzed_state_valid) ||
      (generation.state == CacheEntryState::Absent &&
       (has_sql || !canonical_plan.empty() || !producer_plan.empty())) ||
      (generation.state == CacheEntryState::Claimed &&
       (has_sql || !canonical_plan.empty() || !producer_plan.empty())) ||
      ((generation.state == CacheEntryState::Building ||
        generation.state == CacheEntryState::Complete) && !has_sql)) {
    return false;
  }
  return true;
}

/** Publish already-copied ranges after final ABA and ownership validation. */
inline void assign_diagnostic_text_unchecked(
    ReusableSharedGenerationV1 &generation,
    const DiagnosticByteBundleV1 &bundle,
    const ReusableDiagnosticTextUpdateV1 &update) noexcept {
  generation.display_sql = bundle.display_sql;
  generation.canonical_plan = bundle.canonical_plan;
  generation.producer_plan = bundle.producer_plan;
  generation.display_sql_truncated = update.display_sql_truncated;
  generation.canonical_plan_truncated = update.canonical_plan_truncated;
  generation.producer_plan_truncated = update.producer_plan_truncated;
  generation.canonical_plan_snapshot_status =
      update.canonical_plan_status;
  generation.plan_snapshot_status = update.plan_status;
}

[[nodiscard]] inline bool invalidation_selector_valid(
    const ReusableInvalidationSelectorV1 &selector) noexcept {
  const bool reserved_clear = std::all_of(
      selector.reserved.begin(), selector.reserved.end(),
      [](std::uint8_t value) noexcept { return value == 0; });
  if (selector.database_oid == 0 || selector.maximum_scan == 0 ||
      !reserved_clear) {
    return false;
  }
  if (selector.match_security_namespace &&
      (selector.security_namespace.database_oid != selector.database_oid ||
       !valid_reusable_security_namespace(selector.security_namespace))) {
    return false;
  }
  if (selector.match_provider_id && selector.provider_id == 0) {
    return false;
  }
  const bool physical_kind_valid =
      selector.physical_identity.kind ==
          PhysicalIdentityKind::PgRelFileLocator ||
      selector.physical_identity.kind == PhysicalIdentityKind::ProviderOpaque;
  if (selector.match_physical_identity &&
      (!physical_kind_valid ||
       selector.physical_identity.length == 0 ||
       selector.physical_identity.length >
           selector.physical_identity.bytes.size())) {
    return false;
  }
  if (selector.match_observability &&
      !write_observability_valid(selector.observability)) {
    return false;
  }
  const bool precise_metadata_match =
      selector.match_provider_id || selector.match_physical_identity ||
      selector.match_dependency_fingerprint ||
      selector.match_schema_fingerprint || selector.match_mapping_fingerprint ||
      selector.match_lineage_fingerprint || selector.match_observability;
  if (selector.coarse_database_fallback) {
    return !selector.match_security_namespace && !precise_metadata_match;
  }
  return selector.match_security_namespace || precise_metadata_match;
}

[[nodiscard]] inline bool generation_matches_invalidation(
    const ReusableSharedGenerationV1 &generation,
    const ReusableInvalidationSelectorV1 &selector) noexcept {
  const ReusableSecurityNamespaceV1 &security_namespace =
      generation.coverage_directory_key.security_namespace;
  if (security_namespace.database_oid != selector.database_oid ||
      (selector.match_security_namespace &&
       security_namespace != selector.security_namespace)) {
    return false;
  }
  if (selector.coarse_database_fallback) {
    return true;
  }
  const ReusableGenerationMetadataV1 &metadata = generation.metadata;
  return (!selector.match_provider_id ||
          metadata.provider_id == selector.provider_id) &&
         (!selector.match_physical_identity ||
          physical_identity_equal(metadata.physical_identity,
                                  selector.physical_identity)) &&
         (!selector.match_dependency_fingerprint ||
          metadata.dependency_fingerprint ==
              selector.dependency_fingerprint) &&
         (!selector.match_schema_fingerprint ||
          metadata.schema_fingerprint == selector.schema_fingerprint) &&
         (!selector.match_mapping_fingerprint ||
          metadata.mapping_fingerprint == selector.mapping_fingerprint) &&
         (!selector.match_lineage_fingerprint ||
          metadata.lineage_fingerprint == selector.lineage_fingerprint) &&
         (!selector.match_observability ||
          metadata.observability == selector.observability);
}

[[nodiscard]] inline bool
completed_generation_expired(const ReusableSharedGenerationV1 &generation,
                             std::uint64_t consumer_ttl_ms,
                             std::uint64_t now_ns) noexcept {
  const CacheTimestamps timestamps{
      .started_ns = generation.started_monotonic_ns,
      .completed_ns = generation.completed_monotonic_ns,
  };
  return expired(timestamps, generation.metadata.producer_ttl_ms,
                 consumer_ttl_ms, now_ns);
}

struct AdmissionVictimSelectionV1 {
  ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
  ReusableGenerationReferenceV1 reference{};
  bool bucket_full = false;
};

[[nodiscard]] inline std::uint8_t admission_victim_priority(
    const ReusableSharedGenerationV1 &generation,
    std::uint64_t now_monotonic_ns) noexcept {
  if (generation.state == CacheEntryState::Complete &&
      completed_generation_expired(
          generation, (std::numeric_limits<std::uint64_t>::max)(),
          now_monotonic_ns)) {
    return 0;
  }
  if (generation.state == CacheEntryState::Invalidated) {
    return 1;
  }
  if (generation.state == CacheEntryState::Failed) {
    return 2;
  }
  return generation.state == CacheEntryState::Complete ? 3 : 4;
}

[[nodiscard]] inline bool admission_victim_precedes(
    const ReusableSharedGenerationV1 &candidate, std::uint32_t candidate_slot,
    const ReusableSharedGenerationV1 &current, std::uint32_t current_slot,
    std::uint64_t now_monotonic_ns) noexcept {
  const std::uint8_t candidate_priority =
      admission_victim_priority(candidate, now_monotonic_ns);
  const std::uint8_t current_priority =
      admission_victim_priority(current, now_monotonic_ns);
  if (candidate_priority != current_priority) {
    return candidate_priority < current_priority;
  }
  if (candidate.last_access_monotonic_ns != current.last_access_monotonic_ns) {
    return candidate.last_access_monotonic_ns <
           current.last_access_monotonic_ns;
  }
  if (candidate.id.claim_sequence != current.id.claim_sequence) {
    return candidate.id.claim_sequence < current.id.claim_sequence;
  }
  return candidate_slot < current_slot;
}

[[nodiscard]] inline bool admission_victim_eligible(
    const ReusableSharedGenerationV1 &generation) noexcept {
  return generation.membership == ReusableRecordMembershipV1::Indexed &&
         terminal_state(generation.state) && generation.waiter_count == 0 &&
         generation.planner_pin_count == 0 &&
         generation.reader_pin_count == 0;
}

/** Select without mutation while the exact partition is held. */
[[nodiscard]] inline AdmissionVictimSelectionV1
select_exact_admission_victim(ReusableRegistryAttachmentV1 &attachment,
                              const CacheDigest256 &digest,
                              std::uint64_t now_monotonic_ns) noexcept {
  AdmissionVictimSelectionV1 result{};
  const DirectoryProbeV1 probe = probe_exact_directory(attachment, digest);
  if (!probe.found) {
    return result;
  }
  const ReusableExactBucketV1 &bucket = exact_buckets(attachment)[probe.slot];
  if (bucket.generation_count > kMaxGenerationsPerExactDigest) {
    result.error = ReusableRegistryErrorV1::CorruptDirectory;
    return result;
  }
  if (bucket.generation_count < kMaxGenerationsPerExactDigest) {
    return result;
  }
  result.bucket_full = true;
  if (bucket.terminal_generation_count == 0) {
    return result;
  }

  const ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  std::uint32_t current = bucket.first_generation;
  std::uint32_t selected = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t visited = 0; visited < bucket.generation_count;
       ++visited) {
    if (current == kInvalidReusableRegistrySlotV1 ||
        current >= shared.config.max_generations) {
      result.error = ReusableRegistryErrorV1::CorruptDirectory;
      return result;
    }
    const ReusableSharedGenerationV1 &generation = records[current];
    if (generation.membership != ReusableRecordMembershipV1::Indexed ||
        !known_entry_kind(generation.entry_kind) ||
        generation.id.digest != digest) {
      result.error = ReusableRegistryErrorV1::CorruptDirectory;
      return result;
    }
    if (admission_victim_eligible(generation) &&
        (selected == kInvalidReusableRegistrySlotV1 ||
         admission_victim_precedes(generation, current, records[selected],
                                   selected, now_monotonic_ns))) {
      selected = current;
    }
    current = generation.next_in_exact_bucket;
  }
  if (current != kInvalidReusableRegistrySlotV1) {
    result.error = ReusableRegistryErrorV1::CorruptDirectory;
    return result;
  }
  if (selected != kInvalidReusableRegistrySlotV1) {
    result.reference = make_reference(records[selected], selected);
  }
  return result;
}

/** Select without mutation while the coverage partition is held. */
[[nodiscard]] inline AdmissionVictimSelectionV1
select_coverage_admission_victim(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableCoverageDirectoryKeyV1 &key,
    std::uint64_t now_monotonic_ns) noexcept {
  AdmissionVictimSelectionV1 result{};
  const DirectoryProbeV1 probe = probe_coverage_directory(attachment, key);
  if (!probe.found) {
    return result;
  }
  const ReusableCoverageBucketV1 &bucket =
      coverage_buckets(attachment)[probe.slot];
  if (bucket.generation_count > kMaxCoverageGenerationsPerDomain) {
    result.error = ReusableRegistryErrorV1::CorruptDirectory;
    return result;
  }
  if (bucket.generation_count < kMaxCoverageGenerationsPerDomain) {
    return result;
  }
  result.bucket_full = true;
  if (bucket.terminal_generation_count == 0) {
    return result;
  }

  const ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  std::uint32_t current = bucket.first_generation;
  std::uint32_t selected = kInvalidReusableRegistrySlotV1;
  for (std::uint32_t visited = 0; visited < bucket.generation_count;
       ++visited) {
    if (current == kInvalidReusableRegistrySlotV1 ||
        current >= shared.config.max_generations) {
      result.error = ReusableRegistryErrorV1::CorruptDirectory;
      return result;
    }
    const ReusableSharedGenerationV1 &generation = records[current];
    if (generation.membership != ReusableRecordMembershipV1::Indexed ||
        generation.entry_kind != CacheEntryKind::ReusableCoverage ||
        generation.coverage_directory_key != key) {
      result.error = ReusableRegistryErrorV1::CorruptDirectory;
      return result;
    }
    if (admission_victim_eligible(generation) &&
        (selected == kInvalidReusableRegistrySlotV1 ||
         admission_victim_precedes(generation, current, records[selected],
                                   selected, now_monotonic_ns))) {
      selected = current;
    }
    current = generation.next_in_coverage_bucket;
  }
  if (current != kInvalidReusableRegistrySlotV1) {
    result.error = ReusableRegistryErrorV1::CorruptDirectory;
    return result;
  }
  if (selected != kInvalidReusableRegistrySlotV1) {
    result.reference = make_reference(records[selected], selected);
  }
  return result;
}

[[nodiscard]] inline bool admission_victim_reduces_coverage_bucket(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    const ReusableCoverageDirectoryKeyV1 &key) noexcept {
  if (!reference ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return false;
  }
  const ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  return reference_matches(generation, reference) &&
         generation.entry_kind == CacheEntryKind::ReusableCoverage &&
         generation.coverage_directory_key == key;
}

[[nodiscard]] inline bool remember_claim_retired_manifest(
    ReusableClaimResultV1 &result,
    const ReusableManifestHandleV1 &manifest) noexcept {
  if (!manifest) {
    return true;
  }
  if (result.retired_manifest_count >= result.retired_manifests.size()) {
    return false;
  }
  result.retired_manifests[result.retired_manifest_count++] = manifest;
  return true;
}

} // namespace reusable_registry_detail

[[nodiscard]] inline std::expected<ReusableManifestHandleV1,
                                   ReusableRegistryErrorV1>
retire_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept;

/**
 * Collision-safe current-generation lookup for either cache product. The
 * caller holds shared membership; only the exact partition is acquired.
 */
[[nodiscard]] inline ReusableLookupResultV1 lookup_cache_current(
    ReusableRegistryAttachmentV1 &attachment,
    CacheEntryKind entry_kind,
    const ReusableCoverageDirectoryKeyV1 &coverage_key,
    std::span<const std::byte> domain_key,
    const CacheDigest256 &generation_digest,
    std::span<const std::byte> generation_key, ReusableLookupModeV1 mode,
    const ReusableReferenceOwnerV1 &reference_owner,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableLookupResultV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None || !callbacks.valid() ||
      !known_entry_kind(entry_kind) || generation_key.empty() ||
      (mode != ReusableLookupModeV1::Observe && !reference_owner.valid())) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  const ReusableSecurityNamespaceV1 &security_namespace =
      coverage_key.security_namespace;
  const bool kind_identity_valid =
      entry_kind == CacheEntryKind::ReusableCoverage
          ? !domain_key.empty()
          : domain_key.empty() &&
                coverage_key.coverage_domain_digest == CacheDigest256{};
  if (!valid_reusable_security_namespace(security_namespace) ||
      !kind_identity_valid || domain_key.size() > kMaxCoverageDomainKeyBytes ||
      generation_key.size() > kMaxCanonicalKeyBytes) {
    result.error = ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, generation_digest);
  acquire_exact(callbacks, partition);
  const MatchingSlotV1 match =
      find_exact_current(attachment, entry_kind, coverage_key, domain_key,
                         generation_digest, generation_key);
  if (match.error != ReusableRegistryErrorV1::None) {
    release_exact(callbacks, partition);
    result.error = match.error;
    return result;
  }
  ReusableSharedGenerationV1 &generation = generations(attachment)[match.slot];
  result.snapshot = make_snapshot(generation, match.slot);
  ReusableReferenceKindV1 kind = ReusableReferenceKindV1::None;
  if (mode == ReusableLookupModeV1::RegisterWaiter) {
    kind = ReusableReferenceKindV1::Waiter;
  } else if (mode == ReusableLookupModeV1::PinComplete) {
    kind = ReusableReferenceKindV1::ReaderPin;
  }
  if (kind != ReusableReferenceKindV1::None) {
    auto token = increment_reference(attachment, generation, match.slot, kind,
                                     reference_owner);
    if (!token.has_value()) {
      release_exact(callbacks, partition);
      result.error = token.error();
      return result;
    }
    result.token = *token;
    result.snapshot = make_snapshot(generation, match.slot);
  }
  release_exact(callbacks, partition);
  return result;
}

/** Reusable-coverage convenience overload retaining the provider API shape. */
[[nodiscard]] inline ReusableLookupResultV1 lookup_reusable_current(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableCoverageDirectoryKeyV1 &coverage_key,
    std::span<const std::byte> domain_key,
    const CacheDigest256 &generation_digest,
    std::span<const std::byte> generation_key, ReusableLookupModeV1 mode,
    const ReusableReferenceOwnerV1 &reference_owner,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  return lookup_cache_current(
      attachment, CacheEntryKind::ReusableCoverage, coverage_key, domain_key,
      generation_digest, generation_key, mode, reference_owner, callbacks);
}

/**
 * Coalesce exact-current work or atomically link one new CLAIMED generation.
 * The caller holds exclusive membership; exact and coverage callbacks are
 * invoked sequentially and are never held at the same time.
 */
[[nodiscard]] inline ReusableClaimResultV1 claim_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableClaimRequestV1 &request,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableClaimResultV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None || !callbacks.valid() ||
      !claim_request_valid(request)) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  if (request.canonical_domain_key.size() > kMaxCoverageDomainKeyBytes ||
      request.canonical_generation_key.size() > kMaxCanonicalKeyBytes) {
    result.error = ReusableRegistryErrorV1::KeyTooLarge;
    return result;
  }
  if (request.coverage_descriptor.size() > kMaxProviderRecipeBytes) {
    result.error = ReusableRegistryErrorV1::DescriptorTooLarge;
    return result;
  }

  ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint32_t exact_partition =
      reusable_exact_partition(attachment, request.generation_digest);
  acquire_exact(callbacks, exact_partition);
  const MatchingSlotV1 existing = find_exact_current(
      attachment, request.entry_kind, request.coverage_directory_key,
      request.canonical_domain_key, request.generation_digest,
      request.canonical_generation_key, &request);
  if (existing.error == ReusableRegistryErrorV1::None) {
    ReusableSharedGenerationV1 &generation =
        generations(attachment)[existing.slot];
    result.reference = make_reference(generation, existing.slot);
    if (generation.state == CacheEntryState::Claimed ||
        generation.state == CacheEntryState::Building) {
      result.disposition = ReusableClaimDispositionV1::ExistingInFlight;
      if (request.register_waiter) {
        auto token = increment_reference(
            attachment, generation, existing.slot,
            ReusableReferenceKindV1::Waiter, request.reference_owner);
        if (!token.has_value()) {
          release_exact(callbacks, exact_partition);
          result.error = token.error();
          return result;
        }
        result.token = *token;
      }
      release_exact(callbacks, exact_partition);
      return result;
    }
    if (generation.state == CacheEntryState::Complete &&
        !completed_generation_expired(generation, request.consumer_ttl_ms,
                                      request.started_monotonic_ns)) {
      result.disposition = ReusableClaimDispositionV1::ExistingComplete;
      if (request.pin_complete) {
        auto token = increment_reference(
            attachment, generation, existing.slot,
            ReusableReferenceKindV1::ReaderPin, request.reference_owner);
        if (!token.has_value()) {
          release_exact(callbacks, exact_partition);
          result.error = token.error();
          return result;
        }
        result.token = *token;
      }
      release_exact(callbacks, exact_partition);
      return result;
    }
  } else if (existing.error != ReusableRegistryErrorV1::NotFound) {
    release_exact(callbacks, exact_partition);
    result.error = existing.error;
    return result;
  }
  release_exact(callbacks, exact_partition);

  const bool coverage_membership =
      has_coverage_membership(request.entry_kind);
  const std::uint32_t coverage_partition =
      coverage_membership
          ? reusable_coverage_partition(attachment,
                                        request.coverage_directory_key)
          : 0;

  auto record_slot = allocate_generation_record(attachment);
  if (!record_slot.has_value()) {
    result.error = record_slot.error();
    return result;
  }
  auto bundle = copy_generation_bytes(attachment, request.canonical_domain_key,
                                      request.canonical_generation_key,
                                      request.coverage_descriptor,
                                      request.dependency_vector_envelope);
  if (!bundle.has_value()) {
    return_generation_record(attachment, *record_slot);
    result.error = bundle.error();
    return result;
  }
  const std::uint64_t generation_number = take_sequence(shared.next_generation);
  const std::uint64_t claim_sequence =
      take_sequence(shared.next_claim_sequence);
  if (generation_number == 0 || claim_sequence == 0) {
    static_cast<void>(free_arena_bytes(attachment, bundle->allocation));
    return_generation_record(attachment, *record_slot);
    result.error = ReusableRegistryErrorV1::CounterOverflow;
    return result;
  }

  ReusableSharedGenerationV1 &generation =
      generations(attachment)[*record_slot];
  const std::uint64_t slot_sequence = generation.slot_sequence;
  generation = ReusableSharedGenerationV1{
      .membership = ReusableRecordMembershipV1::Indexed,
      .entry_kind = request.entry_kind,
      .state = CacheEntryState::Claimed,
      .slot_sequence = slot_sequence,
      .id =
          {
              .digest = request.generation_digest,
              .boot_nonce = shared.boot_nonce,
              .generation = generation_number,
              .claim_sequence = claim_sequence,
          },
      .coverage_directory_key = request.coverage_directory_key,
      .canonical_generation_key = bundle->generation_key,
      .canonical_domain_key = bundle->domain_key,
      .coverage_descriptor = bundle->descriptor,
      .dependency_vector = bundle->dependency_vector,
      .conservative_guard = request.conservative_guard,
      .allocation_offset = bundle->allocation.offset,
      .allocation_length = bundle->allocation.length,
      .dependency_release_state =
          ReusableDependencyReleaseStateV1::Retained,
      .producer = request.producer,
      .started_monotonic_ns = request.started_monotonic_ns,
      .last_access_monotonic_ns = request.started_monotonic_ns,
      .metadata = request.metadata,
  };

  const auto discard_provisional_generation = [
      &attachment, &bundle, &record_slot]() noexcept {
    static_cast<void>(free_arena_bytes(attachment, bundle->allocation));
    return_generation_record(attachment, *record_slot);
  };

  acquire_exact(callbacks, exact_partition);
  const AdmissionVictimSelectionV1 exact_victim =
      select_exact_admission_victim(attachment, request.generation_digest,
                                    request.started_monotonic_ns);
  release_exact(callbacks, exact_partition);
  if (exact_victim.error != ReusableRegistryErrorV1::None) {
    discard_provisional_generation();
    result.error = exact_victim.error;
    return result;
  }

  AdmissionVictimSelectionV1 coverage_victim{};
  if (coverage_membership) {
    acquire_coverage(callbacks, coverage_partition);
    coverage_victim = select_coverage_admission_victim(
        attachment, request.coverage_directory_key,
        request.started_monotonic_ns);
    release_coverage(callbacks, coverage_partition);
    if (coverage_victim.error != ReusableRegistryErrorV1::None) {
      discard_provisional_generation();
      result.error = coverage_victim.error;
      return result;
    }
  }

  if (exact_victim.bucket_full && !exact_victim.reference) {
    discard_provisional_generation();
    result.error = ReusableRegistryErrorV1::ExactBucketFull;
    return result;
  }
  const bool exact_victim_reduces_coverage =
      coverage_membership && exact_victim.reference &&
      admission_victim_reduces_coverage_bucket(
          attachment, exact_victim.reference,
          request.coverage_directory_key);
  if (coverage_victim.bucket_full && !exact_victim_reduces_coverage &&
      !coverage_victim.reference) {
    discard_provisional_generation();
    result.error = ReusableRegistryErrorV1::CoverageBucketFull;
    return result;
  }

  std::array<ReusableGenerationReferenceV1, 2> victims{};
  std::size_t victim_count = 0;
  if (exact_victim.reference) {
    victims[victim_count++] = exact_victim.reference;
  }
  if (coverage_victim.bucket_full && !exact_victim_reduces_coverage &&
      coverage_victim.reference &&
      (victim_count == 0 || victims[0] != coverage_victim.reference)) {
    victims[victim_count++] = coverage_victim.reference;
  }
  for (std::size_t index = 0; index < victim_count; ++index) {
    auto retired =
        retire_reusable_generation(attachment, victims[index], callbacks);
    if (!retired.has_value()) {
      discard_provisional_generation();
      result.error = retired.error();
      return result;
    }
    if (!remember_claim_retired_manifest(result, *retired)) {
      discard_provisional_generation();
      result.error = ReusableRegistryErrorV1::CorruptLayout;
      return result;
    }
  }

  acquire_exact(callbacks, exact_partition);
  ReusableRegistryErrorV1 link_error =
      link_exact_generation(attachment, *record_slot);
  release_exact(callbacks, exact_partition);
  if (link_error == ReusableRegistryErrorV1::None && coverage_membership) {
    acquire_coverage(callbacks, coverage_partition);
    link_error = link_coverage_generation(attachment, *record_slot);
    release_coverage(callbacks, coverage_partition);
  }
  if (link_error != ReusableRegistryErrorV1::None) {
    acquire_exact(callbacks, exact_partition);
    const DirectoryProbeV1 linked_exact =
        probe_exact_directory(attachment, request.generation_digest);
    if (linked_exact.found) {
      static_cast<void>(unlink_exact_generation(attachment, *record_slot));
    }
    release_exact(callbacks, exact_partition);
    static_cast<void>(free_arena_bytes(attachment, bundle->allocation));
    return_generation_record(attachment, *record_slot);
    result.error = link_error;
    return result;
  }
  ++shared.indexed_generations;
  result.disposition = ReusableClaimDispositionV1::ClaimedProducer;
  result.reference = make_reference(generation, *record_slot);
  return result;
}

/** Pin one generation by ABA-safe identity under its exact partition. */
[[nodiscard]] inline std::expected<ReusableReferenceTokenV1,
                                   ReusableRegistryErrorV1>
pin_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    ReusableReferenceKindV1 kind,
    const ReusableReferenceOwnerV1 &reference_owner,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  if (!callbacks.valid() || !reference || !reference_owner.valid() ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, reference.id.digest);
  acquire_exact(callbacks, partition);
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    release_exact(callbacks, partition);
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  auto token = increment_reference(attachment, generation,
                                   reference.generation_slot, kind,
                                   reference_owner);
  release_exact(callbacks, partition);
  return token;
}

/** Idempotently release exactly the counter tagged in a local token. */
[[nodiscard]] inline ReusableRegistryErrorV1 release_reusable_reference(
    ReusableRegistryAttachmentV1 &attachment, ReusableReferenceTokenV1 &token,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  if (!token.active) {
    return ReusableRegistryErrorV1::None;
  }
  const ReusableReferenceTokenV1 owned = token;
  token = ReusableReferenceTokenV1{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !owned.reference || !owned.owner.valid() ||
      owned.owner_sequence == 0 ||
      owned.owner_slot >= kReusableReferenceOwnersPerGenerationV1 ||
      owned.kind == ReusableReferenceKindV1::None ||
      owned.reference.generation_slot >=
          header(attachment).config.max_generations) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, owned.reference.id.digest);
  acquire_exact(callbacks, partition);
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[owned.reference.generation_slot];
  if (!reference_matches(generation, owned.reference)) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::StaleReference;
  }
  ReusableSharedReferenceOwnerV1 &shared_owner =
      generation.reference_owners[owned.owner_slot];
  if (!reference_owner_matches(shared_owner, owned)) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::StaleReference;
  }
  const ReusableRegistryErrorV1 release_error =
      deactivate_reference_owner(generation, shared_owner);
  release_exact(callbacks, partition);
  return release_error;
}

/** Atomically convert an in-flight waiter entitlement to a complete reader pin.
 */
[[nodiscard]] inline ReusableRegistryErrorV1 convert_reusable_waiter_to_reader(
    ReusableRegistryAttachmentV1 &attachment, ReusableReferenceTokenV1 &token,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !token.active ||
      token.kind != ReusableReferenceKindV1::Waiter || !token.reference ||
      !token.owner.valid() || token.owner_sequence == 0 ||
      token.owner_slot >= kReusableReferenceOwnersPerGenerationV1 ||
      token.reference.generation_slot >=
          header(attachment).config.max_generations) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, token.reference.id.digest);
  acquire_exact(callbacks, partition);
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[token.reference.generation_slot];
  if (!reference_matches(generation, token.reference)) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::StaleReference;
  }
  ReusableSharedReferenceOwnerV1 &shared_owner =
      generation.reference_owners[token.owner_slot];
  if (!reference_owner_matches(shared_owner, token)) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::StaleReference;
  }
  if (generation.state != CacheEntryState::Complete) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::InvalidState;
  }
  if (generation.waiter_count == 0) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::CounterUnderflow;
  }
  if (generation.reader_pin_count ==
      (std::numeric_limits<std::uint32_t>::max)()) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::CounterOverflow;
  }
  --generation.waiter_count;
  ++generation.reader_pin_count;
  shared_owner.kind = ReusableReferenceKindV1::ReaderPin;
  token.kind = ReusableReferenceKindV1::ReaderPin;
  release_exact(callbacks, partition);
  return ReusableRegistryErrorV1::None;
}

/** Observe state/owner/failure while a waiter decides whether to sleep again.
 */
[[nodiscard]] inline std::expected<ReusableGenerationSnapshotV1,
                                   ReusableRegistryErrorV1>
wait_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  if (!callbacks.valid() || !reference ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, reference.id.digest);
  acquire_exact(callbacks, partition);
  const ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    release_exact(callbacks, partition);
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  const ReusableGenerationSnapshotV1 result =
      make_snapshot(generation, reference.generation_slot);
  release_exact(callbacks, partition);
  return result;
}

/**
 * Copy all active waiter capabilities while the exact partition is stable.
 * Callers that must exclude new waiters also hold exclusive membership across
 * this snapshot and the terminal publication that consumes it.
 */
[[nodiscard]] inline ReusableWaiterOwnerBatchV1
snapshot_reusable_waiter_owners(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableWaiterOwnerBatchV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None ||
      !callbacks.valid() || !reference ||
      reference.generation_slot >=
          header(attachment).config.max_generations) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, reference.id.digest);
  acquire_exact(callbacks, partition);
  const ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    release_exact(callbacks, partition);
    result.error = ReusableRegistryErrorV1::StaleReference;
    return result;
  }
  for (std::uint32_t owner_slot = 0;
       owner_slot < kReusableReferenceOwnersPerGenerationV1;
       ++owner_slot) {
    const ReusableSharedReferenceOwnerV1 &shared_owner =
        generation.reference_owners[owner_slot];
    if (!shared_owner.active ||
        shared_owner.kind != ReusableReferenceKindV1::Waiter) {
      continue;
    }
    const bool reserved_clear = std::all_of(
        shared_owner.reserved.begin(), shared_owner.reserved.end(),
        [](std::uint8_t value) noexcept { return value == 0; });
    if (!shared_owner.owner.valid() || shared_owner.owner_sequence == 0 ||
        !reserved_clear || result.count >= result.waiters.size()) {
      release_exact(callbacks, partition);
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      result.count = 0;
      return result;
    }
    result.waiters[result.count++] = ReusableReferenceTokenV1{
        .reference = reference,
        .owner = shared_owner.owner,
        .owner_sequence = shared_owner.owner_sequence,
        .owner_slot = owner_slot,
        .kind = ReusableReferenceKindV1::Waiter,
        .active = true,
    };
  }
  if (result.count != generation.waiter_count) {
    result.error = ReusableRegistryErrorV1::CorruptGeneration;
    result.count = 0;
  }
  release_exact(callbacks, partition);
  return result;
}

/**
 * Snapshot at most 64 full-domain-equal candidates in deterministic order.
 * Every returned candidate owns one planner pin until explicitly released.
 */
[[nodiscard]] inline std::expected<ReusableCandidateBatchV1,
                                   ReusableRegistryErrorV1>
snapshot_reusable_candidates(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableCandidateRequestV1 &request,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableCandidateBatchV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None || !callbacks.valid() ||
      request.canonical_domain_key.empty() ||
      request.canonical_domain_key.size() > kMaxCoverageDomainKeyBytes ||
      !request.reference_owner.valid() || request.provider_id == 0 ||
      request.compatibility_family_id == 0) {
    return std::unexpected(attachment_error != ReusableRegistryErrorV1::None
                               ? attachment_error
                               : ReusableRegistryErrorV1::InvalidArgument);
  }
  const ReusableSecurityNamespaceV1 &security_namespace =
      request.coverage_directory_key.security_namespace;
  if (!valid_reusable_security_namespace(security_namespace)) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }

  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint32_t partition =
      reusable_coverage_partition(attachment, request.coverage_directory_key);
  acquire_coverage(callbacks, partition);
  const DirectoryProbeV1 probe =
      probe_coverage_directory(attachment, request.coverage_directory_key);
  if (!probe.found) {
    release_coverage(callbacks, partition);
    return result;
  }
  const ReusableCoverageBucketV1 &bucket =
      coverage_buckets(attachment)[probe.slot];
  if (bucket.generation_count > kMaxCoverageGenerationsPerDomain) {
    release_coverage(callbacks, partition);
    return std::unexpected(ReusableRegistryErrorV1::CorruptDirectory);
  }
  const std::size_t visit_limit = std::min<std::size_t>(
      bucket.generation_count, kMaxCoverageGenerationsPerDomain);
  result.bucket_truncated = bucket.generation_count > visit_limit;
  ReusableSharedGenerationV1 *records = generations(attachment);
  std::uint32_t current = bucket.first_generation;
  for (std::size_t visited = 0; visited < visit_limit; ++visited) {
    ++result.visited_count;
    if (current == kInvalidReusableRegistrySlotV1 ||
        current >= shared.config.max_generations) {
      release_coverage(callbacks, partition);
      return std::unexpected(ReusableRegistryErrorV1::CorruptDirectory);
    }
    ReusableSharedGenerationV1 &generation = records[current];
    const std::uint32_t next = generation.next_in_coverage_bucket;
    if (generation.membership != ReusableRecordMembershipV1::Indexed ||
        generation.entry_kind != CacheEntryKind::ReusableCoverage ||
        generation.coverage_directory_key != request.coverage_directory_key) {
      ++result.invalid_record_count;
      current = next;
      continue;
    }
    if (!range_equal(attachment, generation.canonical_domain_key,
                     request.canonical_domain_key)) {
      ++result.domain_collision_count;
      current = next;
      continue;
    }
    const bool state_eligible = generation.state == CacheEntryState::Complete ||
                                generation.state == CacheEntryState::Claimed ||
                                generation.state == CacheEntryState::Building;
    const bool metadata_eligible =
        generation.metadata.provider_id == request.provider_id &&
        generation.metadata.compatibility_family_id ==
            request.compatibility_family_id &&
        capabilities_satisfy(generation.metadata.capability_mask,
                             request.required_capability_mask);
    if (!state_eligible || !metadata_eligible ||
        generation.planner_pin_count ==
            (std::numeric_limits<std::uint32_t>::max)()) {
      current = next;
      continue;
    }
    if (result.candidate_count >= result.candidates.size()) {
      result.bucket_truncated = true;
      current = next;
      continue;
    }
    ReusableCandidateSnapshotV1 candidate{
        .generation = make_snapshot(generation, current),
        .complete = generation.state == CacheEntryState::Complete,
        .in_flight = generation.state == CacheEntryState::Claimed ||
                     generation.state == CacheEntryState::Building,
    };
    std::size_t position = result.candidate_count;
    while (position > 0 &&
           candidate_precedes(candidate, result.candidates[position - 1U])) {
      result.candidates[position] = result.candidates[position - 1U];
      --position;
    }
    result.candidates[position] = candidate;
    ++result.candidate_count;
    current = next;
  }
  if (current != kInvalidReusableRegistrySlotV1) {
    release_coverage(callbacks, partition);
    return std::unexpected(ReusableRegistryErrorV1::CorruptDirectory);
  }
  result.selection_truncated =
      result.candidate_count > kMaxSourcesPerCoverageNode;
  release_coverage(callbacks, partition);

  for (std::size_t index = 0; index < result.candidate_count; ++index) {
    ReusableCandidateSnapshotV1 &candidate = result.candidates[index];
    auto planner_pin =
        pin_reusable_generation(attachment, candidate.generation.reference,
                                ReusableReferenceKindV1::PlannerPin,
                                request.reference_owner, callbacks);
    if (!planner_pin.has_value()) {
      for (std::size_t release_index = 0; release_index < index;
           ++release_index) {
        static_cast<void>(release_reusable_reference(
            attachment, result.candidates[release_index].planner_pin,
            callbacks));
      }
      return std::unexpected(planner_pin.error());
    }
    candidate.planner_pin = *planner_pin;
    auto pinned_snapshot = wait_reusable_generation(
        attachment, candidate.generation.reference, callbacks);
    if (!pinned_snapshot.has_value()) {
      static_cast<void>(release_reusable_reference(
          attachment, candidate.planner_pin, callbacks));
      for (std::size_t release_index = 0; release_index < index;
           ++release_index) {
        static_cast<void>(release_reusable_reference(
            attachment, result.candidates[release_index].planner_pin,
            callbacks));
      }
      return std::unexpected(pinned_snapshot.error());
    }
    candidate.generation = *pinned_snapshot;
    candidate.complete =
        candidate.generation.state == CacheEntryState::Complete;
    candidate.in_flight =
        candidate.generation.state == CacheEntryState::Claimed ||
        candidate.generation.state == CacheEntryState::Building;
    if (!candidate.complete && !candidate.in_flight) {
      static_cast<void>(release_reusable_reference(
          attachment, candidate.planner_pin, callbacks));
      for (std::size_t release_index = 0; release_index < index;
           ++release_index) {
        static_cast<void>(release_reusable_reference(
            attachment, result.candidates[release_index].planner_pin,
            callbacks));
      }
      return std::unexpected(ReusableRegistryErrorV1::InvalidState);
    }
  }
  return result;
}

/** Release every planner pin retained by a candidate batch. */
[[nodiscard]] inline ReusableRegistryErrorV1 release_reusable_candidate_batch(
    ReusableRegistryAttachmentV1 &attachment, ReusableCandidateBatchV1 &batch,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  if (batch.candidate_count > batch.candidates.size()) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  ReusableRegistryErrorV1 first_error = ReusableRegistryErrorV1::None;
  for (std::size_t index = 0; index < batch.candidate_count; ++index) {
    const ReusableRegistryErrorV1 error = release_reusable_reference(
        attachment, batch.candidates[index].planner_pin, callbacks);
    if (first_error == ReusableRegistryErrorV1::None &&
        error != ReusableRegistryErrorV1::None) {
      first_error = error;
    }
  }
  return first_error;
}

namespace reusable_registry_detail {

enum class DiagnosticMutationModeV1 : std::uint8_t {
  BeginBuilding,
  Replace,
};

[[nodiscard]] inline ReusableRegistryErrorV1 diagnostic_mutation_error(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation,
    const ReusableDiagnosticTextUpdateV1 &update,
    DiagnosticMutationModeV1 mode) noexcept {
  if (!known_entry_kind(generation.entry_kind) ||
      !shared_diagnostic_layout_valid(attachment, generation)) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }
  if (generation.producer != update.producer) {
    return ReusableRegistryErrorV1::InvalidState;
  }
  if (mode == DiagnosticMutationModeV1::BeginBuilding) {
    return legal_state_transition(generation.state, CacheEntryState::Building)
               ? ReusableRegistryErrorV1::None
               : ReusableRegistryErrorV1::InvalidState;
  }

  const bool state_allows_update =
      generation.state == CacheEntryState::Building ||
      generation.state == CacheEntryState::Complete;
  const bool analyzed_state_valid =
      update.plan_status != ReusablePlanSnapshotStatusV1::Analyzed ||
      state_allows_update;
  const bool no_status_downgrade =
      generation.plan_snapshot_status !=
          ReusablePlanSnapshotStatusV1::Analyzed ||
      update.plan_status == ReusablePlanSnapshotStatusV1::Analyzed;
  const bool no_canonical_status_downgrade =
      generation.canonical_plan_snapshot_status !=
          ReusablePlanSnapshotStatusV1::Planned ||
      update.canonical_plan_status == ReusablePlanSnapshotStatusV1::Planned;
  return state_allows_update && analyzed_state_valid && no_status_downgrade &&
                 no_canonical_status_downgrade
             ? ReusableRegistryErrorV1::None
             : ReusableRegistryErrorV1::InvalidState;
}

[[nodiscard]] inline ReusableRegistryErrorV1 rollback_diagnostic_bundle(
    ReusableRegistryAttachmentV1 &attachment,
    const DiagnosticByteBundleV1 &bundle,
    ReusableRegistryErrorV1 error) noexcept {
  return free_diagnostic_bundle(attachment, bundle)
             ? error
             : ReusableRegistryErrorV1::CorruptLayout;
}

/**
 * Allocate outside the exact lock, then publish only after full ABA, kind,
 * producer, state, and status revalidation. Membership EXCLUSIVE stabilizes
 * arena ownership across both exact-lock windows.
 */
[[nodiscard]] inline ReusableRegistryErrorV1 mutate_diagnostic_text(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableDiagnosticTextUpdateV1 &update,
    DiagnosticMutationModeV1 mode,
    const ReusableRegistryPartitionCallbacksV1 &callbacks) noexcept {
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !diagnostic_update_valid(update) ||
      (mode == DiagnosticMutationModeV1::BeginBuilding &&
       update.plan_status == ReusablePlanSnapshotStatusV1::Analyzed) ||
      update.reference.generation_slot >=
          header(attachment).config.max_generations) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }

  const std::uint32_t partition =
      reusable_exact_partition(attachment, update.reference.id.digest);
  acquire_exact(callbacks, partition);
  ReusableSharedGenerationV1 &preflight =
      generations(attachment)[update.reference.generation_slot];
  if (!reference_matches(preflight, update.reference)) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::StaleReference;
  }
  const ReusableRegistryErrorV1 preflight_error =
      diagnostic_mutation_error(attachment, preflight, update, mode);
  const CacheEntryKind preflight_kind = preflight.entry_kind;
  release_exact(callbacks, partition);
  if (preflight_error != ReusableRegistryErrorV1::None) {
    return preflight_error;
  }
  if (!shared_diagnostic_text_valid(attachment, preflight)) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }

  auto replacement = copy_diagnostic_bytes(attachment, update);
  if (!replacement.has_value()) {
    return replacement.error();
  }

  acquire_exact(callbacks, partition);
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[update.reference.generation_slot];
  ReusableRegistryErrorV1 publish_error = ReusableRegistryErrorV1::None;
  if (!reference_matches(generation, update.reference)) {
    publish_error = ReusableRegistryErrorV1::StaleReference;
  } else if (generation.entry_kind != preflight_kind ||
             !known_entry_kind(generation.entry_kind)) {
    publish_error = ReusableRegistryErrorV1::CorruptGeneration;
  } else {
    publish_error =
        diagnostic_mutation_error(attachment, generation, update, mode);
  }
  if (publish_error != ReusableRegistryErrorV1::None) {
    release_exact(callbacks, partition);
    return rollback_diagnostic_bundle(attachment, *replacement,
                                      publish_error);
  }

  const DiagnosticByteBundleV1 previous = diagnostic_bundle(generation);
  assign_diagnostic_text_unchecked(generation, *replacement, update);
  if (mode == DiagnosticMutationModeV1::BeginBuilding) {
    generation.state = CacheEntryState::Building;
  }
  release_exact(callbacks, partition);
  return free_diagnostic_bundle(attachment, previous)
             ? ReusableRegistryErrorV1::None
             : ReusableRegistryErrorV1::CorruptLayout;
}

} // namespace reusable_registry_detail

/**
 * Transition CLAIMED to BUILDING with explicit unavailable plan statuses.
 * New producer paths should use the diagnostic overload below so BUILDING is
 * first published with the initialized child's planned snapshot. The caller
 * holds membership EXCLUSIVE because diagnostics allocate from the arena.
 */
[[nodiscard]] inline ReusableRegistryErrorV1 begin_reusable_build(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !reference ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, reference.id.digest);
  acquire_exact(callbacks, partition);
  const ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    release_exact(callbacks, partition);
    return ReusableRegistryErrorV1::StaleReference;
  }
  const ReusableProducerIdentityV1 producer = generation.producer;
  release_exact(callbacks, partition);
  constexpr char unavailable_sql[] = "SQL capture unavailable";
  const ReusableDiagnosticTextUpdateV1 unavailable{
      .reference = reference,
      .producer = producer,
      .display_sql = std::span<const char>{
          unavailable_sql, sizeof(unavailable_sql) - 1},
  };
  return mutate_diagnostic_text(attachment, unavailable,
                                DiagnosticMutationModeV1::BeginBuilding,
                                callbacks);
}

/**
 * Publish initialized-child diagnostics and BUILDING atomically. The caller
 * holds membership EXCLUSIVE; the state becomes observable only after text and
 * status are complete under the exact partition lock.
 */
[[nodiscard]] inline ReusableRegistryErrorV1
begin_reusable_build_with_diagnostics(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableDiagnosticTextUpdateV1 &update,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  return mutate_diagnostic_text(attachment, update,
                                DiagnosticMutationModeV1::BeginBuilding,
                                callbacks);
}

/**
 * Store bounded producer-owned diagnostic text after child initialization.
 * The caller holds membership EXCLUSIVE; arena allocation precedes a second
 * exact-lock validation and old ranges are reclaimed only after publication.
 */
[[nodiscard]] inline ReusableRegistryErrorV1
store_reusable_diagnostic_text(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableDiagnosticTextUpdateV1 &update,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  return mutate_diagnostic_text(attachment, update,
                                DiagnosticMutationModeV1::Replace,
                                callbacks);
}

/**
 * Copy diagnostic text only after the PostgreSQL adapter authorizes this row.
 * The caller holds membership SHARED across validation and copying, so an
 * exclusive replacement or retired-range reclaim cannot race the byte copy.
 * No canonical key, predicate serialization, or shared pointer is returned.
 */
[[nodiscard]] inline std::expected<ReusableDiagnosticTextCopyV1,
                                   ReusableRegistryErrorV1>
copy_reusable_diagnostic_text(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    std::span<char> display_sql,
    std::span<char> canonical_plan,
    std::span<char> producer_plan,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  if (!callbacks.valid() || !reference ||
      reference.generation_slot >=
          header(attachment).config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  const std::uint32_t partition =
      reusable_exact_partition(attachment, reference.id.digest);
  acquire_exact(callbacks, partition);
  const ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    release_exact(callbacks, partition);
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  if (!shared_diagnostic_layout_valid(attachment, generation)) {
    release_exact(callbacks, partition);
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }
  const DiagnosticByteBundleV1 ranges = diagnostic_bundle(generation);
  const ReusableDiagnosticTextCopyV1 result{
      .canonical_plan_status = generation.canonical_plan_snapshot_status,
      .plan_status = generation.plan_snapshot_status,
      .display_sql_truncated = generation.display_sql_truncated,
      .canonical_plan_truncated = generation.canonical_plan_truncated,
      .producer_plan_truncated = generation.producer_plan_truncated,
      .display_sql_bytes = ranges.display_sql.length,
      .canonical_plan_bytes = ranges.canonical_plan.length,
      .producer_plan_bytes = ranges.producer_plan.length,
  };
  release_exact(callbacks, partition);

  if (!shared_diagnostic_text_valid(attachment, generation)) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }
  if (display_sql.size() < result.display_sql_bytes ||
      canonical_plan.size() < result.canonical_plan_bytes ||
      producer_plan.size() < result.producer_plan_bytes) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  if (result.display_sql_bytes != 0) {
    std::memcpy(display_sql.data(),
                attachment.region.data() + ranges.display_sql.offset,
                result.display_sql_bytes);
  }
  if (result.canonical_plan_bytes != 0) {
    std::memcpy(canonical_plan.data(),
                attachment.region.data() + ranges.canonical_plan.offset,
                result.canonical_plan_bytes);
  }
  if (result.producer_plan_bytes != 0) {
    std::memcpy(producer_plan.data(),
                attachment.region.data() + ranges.producer_plan.offset,
                result.producer_plan_bytes);
  }
  return result;
}

/**
 * Publish a sealed storage manifest and make COMPLETE visible last. The caller
 * holds exclusive membership while terminal counts are updated in lock order.
 */
[[nodiscard]] inline ReusableRegistryErrorV1 publish_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusablePublishRequestV1 &request,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !request.reference || !request.manifest ||
      request.reference.generation_slot >=
          header(attachment).config.max_generations ||
      request.manifest.reserved != 0 ||
      request.manifest.id != request.reference.id ||
      request.completed_monotonic_ns <
          generations(attachment)[request.reference.generation_slot]
              .started_monotonic_ns) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[request.reference.generation_slot];
  if (!reference_matches(generation, request.reference)) {
    return ReusableRegistryErrorV1::StaleReference;
  }
  if (!legal_state_transition(generation.state, CacheEntryState::Complete)) {
    return ReusableRegistryErrorV1::InvalidState;
  }
  const ReusableRegistryErrorV1 count_error =
      adjust_terminal_counts(attachment, generation, true, callbacks);
  if (count_error != ReusableRegistryErrorV1::None) {
    return count_error;
  }
  generation.manifest = request.manifest;
  generation.completed_monotonic_ns = request.completed_monotonic_ns;
  generation.last_access_monotonic_ns = request.completed_monotonic_ns;
  generation.row_count = request.row_count;
  generation.metadata.logical_bytes = request.logical_bytes;
  generation.metadata.stored_bytes = request.stored_bytes;
  generation.failure = CacheFailureCode::None;
  generation.state = CacheEntryState::Complete;
  return ReusableRegistryErrorV1::None;
}

/** Publish a bounded producer failure and retain the generation for waiters. */
[[nodiscard]] inline ReusableRegistryErrorV1 fail_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference, CacheFailureCode failure,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !reference || failure == CacheFailureCode::None ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    return ReusableRegistryErrorV1::StaleReference;
  }
  if (!legal_state_transition(generation.state, CacheEntryState::Failed)) {
    return ReusableRegistryErrorV1::InvalidState;
  }
  const ReusableRegistryErrorV1 count_error =
      adjust_terminal_counts(attachment, generation, true, callbacks);
  if (count_error != ReusableRegistryErrorV1::None) {
    return count_error;
  }
  generation.failure = failure;
  generation.state = CacheEntryState::Failed;
  return ReusableRegistryErrorV1::None;
}

/** Invalidate a BUILDING or COMPLETE generation without admitting new pins. */
[[nodiscard]] inline ReusableRegistryErrorV1 invalidate_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    CacheFailureCode failure = CacheFailureCode::DependencyChanged,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }
  if (!callbacks.valid() || !reference || failure == CacheFailureCode::None ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return ReusableRegistryErrorV1::InvalidArgument;
  }
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    return ReusableRegistryErrorV1::StaleReference;
  }
  if (!legal_state_transition(generation.state, CacheEntryState::Invalidated)) {
    return ReusableRegistryErrorV1::InvalidState;
  }
  if (!terminal_state(generation.state)) {
    const ReusableRegistryErrorV1 count_error =
        adjust_terminal_counts(attachment, generation, true, callbacks);
    if (count_error != ReusableRegistryErrorV1::None) {
      return count_error;
    }
  }
  generation.failure = failure;
  generation.state = CacheEntryState::Invalidated;
  ++header(attachment).invalidations;
  return ReusableRegistryErrorV1::None;
}

/**
 * Scan a bounded slot interval and invalidate matching reusable generations.
 * The caller holds membership EXCLUSIVE. Matching uses only immutable fields;
 * no manifest is retired or freed, so existing pins may finish normally.
 */
[[nodiscard]] inline ReusableInvalidationResultV1
invalidate_reusable_generations(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableInvalidationSelectorV1 &selector,
    const ReusableRegistryPartitionCallbacksV1 &callbacks = {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableInvalidationResultV1 result{
      .next_cursor = selector.cursor,
  };
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None ||
      !callbacks.valid() || !invalidation_selector_valid(selector)) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  ReusableRegistryHeaderV1 &shared = header(attachment);
  if (selector.cursor > shared.config.max_generations) {
    result.error = ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  if (selector.cursor == shared.config.max_generations) {
    result.done = true;
    return result;
  }
  const std::uint32_t maximum_scan = std::min(
      selector.maximum_scan, kMaximumReusableInvalidationScanV1);
  const std::uint32_t scan_count = std::min(
      maximum_scan, shared.config.max_generations - selector.cursor);
  const std::uint32_t scan_end = selector.cursor + scan_count;
  ReusableSharedGenerationV1 *records = generations(attachment);
  for (std::uint32_t slot = selector.cursor; slot < scan_end; ++slot) {
    result.next_cursor = slot + 1U;
    ++result.scanned_generations;
    ReusableSharedGenerationV1 &generation = records[slot];
    if (generation.membership != ReusableRecordMembershipV1::Indexed ||
        !generation_matches_invalidation(generation, selector)) {
      continue;
    }
    ++result.matched_generations;
    result.waiter_references += generation.waiter_count;
    result.planner_pins += generation.planner_pin_count;
    result.reader_pins += generation.reader_pin_count;
    if (generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
        generation.reader_pin_count != 0) {
      ++result.referenced_generations;
    }

    const ReusableGenerationReferenceV1 reference =
        make_reference(generation, slot);
    const CacheEntryState previous_state = generation.state;
    ReusableRegistryErrorV1 transition_error =
        ReusableRegistryErrorV1::None;
    bool affected = false;
    if (previous_state == CacheEntryState::Claimed ||
        previous_state == CacheEntryState::Building) {
      transition_error = fail_reusable_generation(
          attachment, reference, CacheFailureCode::DependencyChanged,
          callbacks);
      if (transition_error == ReusableRegistryErrorV1::None) {
        ++shared.invalidations;
        ++result.failed_inflight;
        affected = true;
      }
    } else if (previous_state == CacheEntryState::Complete) {
      transition_error = invalidate_reusable_generation(
          attachment, reference, CacheFailureCode::DependencyChanged,
          callbacks);
      if (transition_error == ReusableRegistryErrorV1::None) {
        ++result.invalidated_complete;
        affected = true;
      }
    } else if (previous_state == CacheEntryState::Invalidated) {
      ++result.already_invalidated;
    } else if (previous_state == CacheEntryState::Failed) {
      ++result.already_failed;
    } else {
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      return result;
    }
    if (transition_error != ReusableRegistryErrorV1::None) {
      result.error = transition_error;
      return result;
    }
    if (!affected) {
      continue;
    }
    if (result.affected_count >= result.affected_references.size()) {
      result.error = ReusableRegistryErrorV1::CorruptLayout;
      return result;
    }
    result.affected_references[result.affected_count] = reference;
    result.affected_manifests[result.affected_count] = generation.manifest;
    result.previous_states[result.affected_count] = previous_state;
    ++result.affected_count;
  }
  result.done = result.next_cursor == shared.config.max_generations;
  return result;
}

/**
 * Unlink every index owned by the entry kind and move one unreferenced
 * terminal generation to the bounded retired list. Exact results own only the
 * digest membership; coverage results own digest plus coverage membership.
 */
[[nodiscard]] inline std::expected<ReusableManifestHandleV1,
                                   ReusableRegistryErrorV1>
retire_reusable_generation(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableGenerationReferenceV1 &reference,
    const ReusableRegistryPartitionCallbacksV1 &callbacks) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  if (!callbacks.valid() || !reference ||
      reference.generation_slot >= header(attachment).config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[reference.generation_slot];
  if (!reference_matches(generation, reference)) {
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  if (!terminal_state(generation.state) ||
      !legal_state_transition(generation.state, CacheEntryState::Evicting) ||
      generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
      generation.reader_pin_count != 0) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidState);
  }
  if (!shared_dependency_vector_valid(attachment, generation) ||
      !conservative_guard_valid(
          generation.conservative_guard, generation.metadata,
          generation.coverage_directory_key.security_namespace) ||
      !std::all_of(generation.reserved_dependency_release.begin(),
                   generation.reserved_dependency_release.end(),
                   [](std::uint8_t value) noexcept { return value == 0; })) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }
  if (generation.dependency_release_state !=
          ReusableDependencyReleaseStateV1::Retained ||
      generation.dependency_release_cursor != 0 ||
      generation.dependency_release_sequence != 0) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidState);
  }
  if (shared.indexed_generations == 0) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }

  const std::uint32_t exact_partition =
      reusable_exact_partition(attachment, generation.id.digest);
  acquire_exact(callbacks, exact_partition);
  const ReusableRegistryErrorV1 exact_error =
      unlink_exact_generation(attachment, reference.generation_slot);
  release_exact(callbacks, exact_partition);
  if (exact_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(exact_error);
  }

  if (generation.entry_kind == CacheEntryKind::ReusableCoverage) {
    const std::uint32_t coverage_partition = reusable_coverage_partition(
        attachment, generation.coverage_directory_key);
    acquire_coverage(callbacks, coverage_partition);
    const ReusableRegistryErrorV1 coverage_error =
        unlink_coverage_generation(attachment, reference.generation_slot);
    release_coverage(callbacks, coverage_partition);
    if (coverage_error != ReusableRegistryErrorV1::None) {
      acquire_exact(callbacks, exact_partition);
      const ReusableRegistryErrorV1 rollback_error =
          link_exact_generation(attachment, reference.generation_slot);
      release_exact(callbacks, exact_partition);
      return std::unexpected(rollback_error == ReusableRegistryErrorV1::None
                                 ? coverage_error
                                 : rollback_error);
    }
  } else if (generation.entry_kind != CacheEntryKind::ExactFinalResult) {
    acquire_exact(callbacks, exact_partition);
    const ReusableRegistryErrorV1 rollback_error =
        link_exact_generation(attachment, reference.generation_slot);
    release_exact(callbacks, exact_partition);
    return std::unexpected(rollback_error == ReusableRegistryErrorV1::None
                               ? ReusableRegistryErrorV1::CorruptGeneration
                               : rollback_error);
  }

  generation.state = CacheEntryState::Evicting;
  generation.membership = ReusableRecordMembershipV1::Retired;
  generation.dependency_release_state =
      ReusableDependencyReleaseStateV1::Pending;
  generation.next_retired_or_free = shared.retired_generation_head;
  shared.retired_generation_head = reference.generation_slot;
  --shared.indexed_generations;
  ++shared.retired_generations;
  ++shared.evictions;
  return generation.manifest;
}

namespace reusable_registry_detail {

[[nodiscard]] inline bool arena_allocation_reclaimable(
    const ReusableRegistryAttachmentV1 &attachment,
    ArenaAllocationV1 allocation) noexcept {
  if (allocation.length == 0) {
    return allocation.offset == 0;
  }
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const std::uint64_t end =
      static_cast<std::uint64_t>(allocation.offset) + allocation.length;
  if (allocation.offset < shared.layout.byte_arena_offset ||
      allocation.offset % 8U != 0 || allocation.length % 8U != 0 ||
      end > static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                shared.config.byte_arena_bytes) {
    return false;
  }
  const ReusableFreeExtentV1 *extents = free_extents(attachment);
  for (std::uint32_t index = 0; index < shared.free_extent_count; ++index) {
    if (arena_allocations_overlap(
            allocation,
            ArenaAllocationV1{.offset = extents[index].offset,
                              .length = extents[index].length})) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool dependency_release_ticket_matches(
    const ReusableRegistryAttachmentV1 &attachment,
    const ReusableSharedGenerationV1 &generation,
    const ReusableDependencyReleaseTicketV1 &ticket) noexcept {
  if (!reference_matches(generation, ticket.reference, true) ||
      generation.membership != ReusableRecordMembershipV1::Retired ||
      generation.state != CacheEntryState::Evicting ||
      generation.dependency_release_sequence != ticket.release_sequence ||
      generation.metadata.dependency_count != ticket.dependency_count ||
      generation_reference_count(generation) != ticket.reference_count ||
      generation.conservative_guard != ticket.conservative_guard ||
      generation.dependency_vector.length != ticket.vector_bytes ||
      generation.manifest != ticket.manifest ||
      !conservative_guard_valid(
          generation.conservative_guard, generation.metadata,
          generation.coverage_directory_key.security_namespace) ||
      !shared_dependency_vector_valid(attachment, generation)) {
    return false;
  }
  const std::span<const std::byte> envelope{
      attachment.region.data() + generation.dependency_vector.offset,
      generation.dependency_vector.length,
  };
  const DependencyVectorEnvelopeResultV1 validation =
      validate_dependency_vector_envelope(envelope);
  return validation &&
         validation.header.payload_checksum == ticket.payload_checksum;
}

} // namespace reusable_registry_detail

/**
 * Under exclusive membership, copy and mark one retired generation's pending
 * dependency-release work. The caller must drop every registry lock before it
 * decodes the copied envelope or calls the dependency-directory release API.
 * A short output reports the exact byte requirement without claiming work.
 */
[[nodiscard]] inline ReusableDependencyReleasePreparationV1
prepare_reusable_dependency_release(
    ReusableRegistryAttachmentV1 &attachment,
    std::span<std::byte> copied_dependency_vector) noexcept {
  using namespace reusable_registry_detail;
  ReusableDependencyReleasePreparationV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    result.error = attachment_error;
    return result;
  }

  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  std::uint32_t current = shared.retired_generation_head;
  std::uint32_t visited = 0;
  while (current != kInvalidReusableRegistrySlotV1 &&
         visited < shared.config.max_generations) {
    if (current >= shared.config.max_generations) {
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      return result;
    }
    ReusableSharedGenerationV1 &generation = records[current];
    if (generation.membership != ReusableRecordMembershipV1::Retired ||
        generation.state != CacheEntryState::Evicting ||
        generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
        generation.reader_pin_count != 0 ||
        generation.dependency_release_cursor >
            generation_reference_count(generation) ||
        !std::all_of(generation.reserved_dependency_release.begin(),
                     generation.reserved_dependency_release.end(),
                     [](std::uint8_t value) noexcept { return value == 0; }) ||
        !conservative_guard_valid(
            generation.conservative_guard, generation.metadata,
            generation.coverage_directory_key.security_namespace) ||
        !shared_dependency_vector_valid(attachment, generation)) {
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      return result;
    }
    const ReusableDependencyReleaseStateV1 release_state =
        generation.dependency_release_state;
    if (release_state == ReusableDependencyReleaseStateV1::None ||
        release_state == ReusableDependencyReleaseStateV1::Retained ||
        (release_state == ReusableDependencyReleaseStateV1::Released &&
         generation.dependency_release_cursor !=
             generation_reference_count(generation)) ||
        (release_state == ReusableDependencyReleaseStateV1::Prepared &&
         generation.dependency_release_sequence == 0)) {
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      return result;
    }
    if (release_state == ReusableDependencyReleaseStateV1::Pending ||
        release_state == ReusableDependencyReleaseStateV1::Released) {
      const std::span<const std::byte> envelope{
          attachment.region.data() + generation.dependency_vector.offset,
          generation.dependency_vector.length,
      };
      const DependencyVectorEnvelopeResultV1 validation =
          validate_dependency_vector_envelope(envelope);
      if (!validation ||
          validation.header.entry_count !=
              generation.metadata.dependency_count) {
        result.error = ReusableRegistryErrorV1::CorruptGeneration;
        return result;
      }
      result.required_vector_bytes = generation.dependency_vector.length;
      if (copied_dependency_vector.size() < result.required_vector_bytes) {
        result.error = ReusableRegistryErrorV1::RegionTooSmall;
        return result;
      }
      const std::uint64_t release_sequence =
          take_sequence(shared.next_dependency_release_sequence);
      if (release_sequence == 0) {
        result.error = ReusableRegistryErrorV1::CounterOverflow;
        return result;
      }
      std::memcpy(copied_dependency_vector.data(), envelope.data(),
                  envelope.size());
      generation.dependency_release_sequence = release_sequence;
      generation.dependency_release_state =
          ReusableDependencyReleaseStateV1::Prepared;
      result.ticket = ReusableDependencyReleaseTicketV1{
          .reference = make_reference(generation, current),
          .manifest = generation.manifest,
          .conservative_guard = generation.conservative_guard,
          .release_sequence = release_sequence,
          .payload_checksum = validation.header.payload_checksum,
          .first_reference = generation.dependency_release_cursor,
          .reference_count = generation_reference_count(generation),
          .dependency_count = generation.metadata.dependency_count,
          .vector_bytes = generation.dependency_vector.length,
      };
      result.copied_vector_bytes = generation.dependency_vector.length;
      return result;
    }
    current = generation.next_retired_or_free;
    ++visited;
  }
  if (current != kInvalidReusableRegistrySlotV1) {
    result.error = ReusableRegistryErrorV1::CorruptGeneration;
    return result;
  }
  result.error = ReusableRegistryErrorV1::NoReclaimableGeneration;
  return result;
}

/**
 * Persist the exact successfully-applied prefix after external release calls.
 * `released_dependencies` counts directory mutations with `applied == true`,
 * even when the directory also reports a diagnostic/fallback error. A partial
 * prefix returns the record to Pending so the next ticket starts after it.
 */
[[nodiscard]] inline std::expected<bool, ReusableRegistryErrorV1>
record_reusable_dependency_release_progress(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableDependencyReleaseTicketV1 &ticket,
    std::uint32_t released_dependencies) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  if (!ticket ||
      ticket.reference.generation_slot >=
          header(attachment).config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  ReusableSharedGenerationV1 &generation =
      generations(attachment)[ticket.reference.generation_slot];
  if (!dependency_release_ticket_matches(attachment, generation, ticket)) {
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  const std::uint32_t remaining =
      ticket.reference_count - ticket.first_reference;
  if (released_dependencies > remaining) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  if (generation.dependency_release_state ==
          ReusableDependencyReleaseStateV1::Released &&
      generation.dependency_release_cursor == ticket.reference_count &&
      released_dependencies == remaining) {
    return true;
  }
  if (generation.dependency_release_state !=
          ReusableDependencyReleaseStateV1::Prepared ||
      generation.dependency_release_cursor != ticket.first_reference) {
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  generation.dependency_release_cursor =
      ticket.first_reference + released_dependencies;
  const bool complete = generation.dependency_release_cursor ==
                        generation_reference_count(generation);
  generation.dependency_release_state =
      complete ? ReusableDependencyReleaseStateV1::Released
               : ReusableDependencyReleaseStateV1::Pending;
  if (!complete) {
    generation.dependency_release_sequence = 0;
  }
  return complete;
}

/**
 * ABA-revalidate a completed release ticket, unlink its arbitrary retired-list
 * position, free all generation bytes, and return the slot to the free list.
 * The caller holds exclusive membership; no directory callback is made here.
 */
[[nodiscard]] inline std::expected<ReusableManifestHandleV1,
                                   ReusableRegistryErrorV1>
finalize_reusable_generation_reclaim(
    ReusableRegistryAttachmentV1 &attachment,
    const ReusableDependencyReleaseTicketV1 &ticket) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  ReusableRegistryHeaderV1 &shared = header(attachment);
  if (!ticket ||
      ticket.reference.generation_slot >= shared.config.max_generations) {
    return std::unexpected(ReusableRegistryErrorV1::InvalidArgument);
  }
  ReusableSharedGenerationV1 *records = generations(attachment);
  ReusableSharedGenerationV1 &generation =
      records[ticket.reference.generation_slot];
  if (generation.dependency_release_state !=
          ReusableDependencyReleaseStateV1::Released ||
      !dependency_release_ticket_matches(attachment, generation, ticket) ||
      generation.dependency_release_cursor !=
          generation_reference_count(generation)) {
    return std::unexpected(ReusableRegistryErrorV1::StaleReference);
  }
  if (!shared_diagnostic_text_valid(attachment, generation) ||
      shared.retired_generations == 0) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }

  std::uint32_t previous = kInvalidReusableRegistrySlotV1;
  std::uint32_t current = shared.retired_generation_head;
  std::uint32_t visited = 0;
  while (current != kInvalidReusableRegistrySlotV1 &&
         current != ticket.reference.generation_slot &&
         visited < shared.config.max_generations) {
    if (current >= shared.config.max_generations ||
        records[current].membership != ReusableRecordMembershipV1::Retired) {
      return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
    }
    previous = current;
    current = records[current].next_retired_or_free;
    ++visited;
  }
  if (current != ticket.reference.generation_slot) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptGeneration);
  }

  const ArenaAllocationV1 identity{
      .offset = generation.allocation_offset,
      .length = generation.allocation_length,
  };
  const DiagnosticByteBundleV1 diagnostics = diagnostic_bundle(generation);
  const std::array<ArenaAllocationV1, 4> allocations{
      identity,
      diagnostic_allocation(diagnostics.display_sql),
      diagnostic_allocation(diagnostics.canonical_plan),
      diagnostic_allocation(diagnostics.producer_plan),
  };
  std::uint32_t allocation_count = 0;
  for (const ArenaAllocationV1 allocation : allocations) {
    if (allocation.length != 0) {
      ++allocation_count;
    }
    if (!arena_allocation_reclaimable(attachment, allocation)) {
      return std::unexpected(ReusableRegistryErrorV1::CorruptLayout);
    }
  }
  if (shared.free_extent_count + allocation_count >
      shared.layout.free_extent_capacity) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptLayout);
  }

  if (!free_diagnostic_bundle(attachment, diagnostics) ||
      !free_arena_bytes(attachment, identity)) {
    return std::unexpected(ReusableRegistryErrorV1::CorruptLayout);
  }
  if (previous == kInvalidReusableRegistrySlotV1) {
    shared.retired_generation_head = generation.next_retired_or_free;
  } else {
    records[previous].next_retired_or_free = generation.next_retired_or_free;
  }
  const ReusableManifestHandleV1 manifest = generation.manifest;
  --shared.retired_generations;
  return_generation_record(attachment, ticket.reference.generation_slot);
  return manifest;
}

/**
 * Copy every producer/reference-owner token in the next janitor window.
 * The adapter holds membership shared; partition callbacks make each record
 * internally consistent. Liveness probing happens only after those locks drop.
 * A short destination is reported as truncated and must disable reaping for
 * that sample.
 */
[[nodiscard]] inline ReusableJanitorLivenessSnapshotV1
snapshot_reusable_janitor_liveness(
    const ReusableRegistryAttachmentV1 &attachment,
    std::span<ReusableJanitorOwnerObservationV1> observations,
    std::uint32_t maximum = kMaximumReusableJanitorBatchV1,
    const ReusableRegistryPartitionCallbacksV1 &partition_callbacks =
        {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableJanitorLivenessSnapshotV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None ||
      !partition_callbacks.valid()) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const ReusableSharedGenerationV1 *records = generations(attachment);
  maximum = std::min(maximum, kMaximumReusableJanitorBatchV1);
  const std::uint32_t scan_limit =
      std::min(maximum, shared.config.max_generations);
  result.scan_start = shared.janitor_scan_cursor;
  for (std::uint32_t scanned = 0; scanned < scan_limit; ++scanned) {
    const std::uint32_t slot =
        (result.scan_start + scanned) % shared.config.max_generations;
    const ReusableSharedGenerationV1 &generation = records[slot];
    ++result.scanned_generations;
    if (generation.membership != ReusableRecordMembershipV1::Indexed) {
      continue;
    }
    const ReusableGenerationReferenceV1 reference =
        make_reference(generation, slot);
    const std::uint32_t exact_partition =
        reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(partition_callbacks, exact_partition);
    if (!reference_matches(generation, reference)) {
      release_exact(partition_callbacks, exact_partition);
      continue;
    }
    if (generation.state == CacheEntryState::Claimed ||
        generation.state == CacheEntryState::Building) {
      const ReusableJanitorOwnerObservationV1 observation =
          make_producer_observation(generation, slot);
      if (!observation.valid()) {
        release_exact(partition_callbacks, exact_partition);
        result.error = ReusableRegistryErrorV1::CorruptGeneration;
        return result;
      }
      if (result.count >= observations.size()) {
        result.truncated = true;
      } else {
        observations[result.count++] = observation;
      }
    }
    for (std::uint32_t owner_slot = 0;
         owner_slot < kReusableReferenceOwnersPerGenerationV1;
         ++owner_slot) {
      const ReusableSharedReferenceOwnerV1 &owner =
          generation.reference_owners[owner_slot];
      if (!owner.active) {
        continue;
      }
      const ReusableJanitorOwnerObservationV1 observation =
          make_reference_owner_observation(generation, slot, owner,
                                           owner_slot);
      if (!observation.valid()) {
        release_exact(partition_callbacks, exact_partition);
        result.error = ReusableRegistryErrorV1::CorruptGeneration;
        return result;
      }
      if (result.count >= observations.size()) {
        result.truncated = true;
        break;
      }
      observations[result.count++] = observation;
    }
    release_exact(partition_callbacks, exact_partition);
    if (result.truncated) {
      break;
    }
  }
  return result;
}

/**
 * Legacy safety probe. Retired generations are never freed through this API;
 * callers must use prepare, lock-dropped directory release, progress, and
 * ABA-finalize. Returning zero keeps old janitor adapters fail-open.
 */
[[nodiscard]] inline std::expected<std::uint32_t, ReusableRegistryErrorV1>
reclaim_retired_reusable_generations(
    ReusableRegistryAttachmentV1 &attachment,
    std::uint32_t maximum = kMaximumReusableJanitorBatchV1) noexcept {
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return std::unexpected(attachment_error);
  }
  static_cast<void>(maximum);
  return 0;
}

/**
 * Run one bounded database-independent sweep. The liveness callback must be a
 * nonblocking scalar check and is never persisted in the shared region.
 */
[[nodiscard]] inline ReusableJanitorSweepResultV1 janitor_reusable_registry(
    ReusableRegistryAttachmentV1 &attachment, std::uint64_t now_monotonic_ns,
    std::uint32_t maximum = kMaximumReusableJanitorBatchV1,
    const ReusableJanitorCallbacksV1 &janitor_callbacks = {},
    const ReusableRegistryPartitionCallbacksV1 &partition_callbacks =
        {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableJanitorSweepResultV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None ||
      !partition_callbacks.valid()) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }
  maximum = std::min(maximum, kMaximumReusableJanitorBatchV1);
  if (maximum == 0) {
    return result;
  }

  ReusableRegistryHeaderV1 &shared = header(attachment);
  ReusableSharedGenerationV1 *records = generations(attachment);
  const std::uint32_t remaining = maximum;
  const std::uint32_t scan_limit =
      std::min(remaining, shared.config.max_generations);
  const std::uint32_t scan_start = shared.janitor_scan_cursor;
  std::uint32_t scanned = 0;
  std::uint32_t retired_this_pass = 0;
  std::uint32_t reaped_this_pass = 0;
  for (; scanned < scan_limit &&
         retired_this_pass + reaped_this_pass < remaining;
       ++scanned) {
    const std::uint32_t slot =
        (scan_start + scanned) % shared.config.max_generations;
    ReusableSharedGenerationV1 &generation = records[slot];
    if (generation.membership != ReusableRecordMembershipV1::Indexed) {
      continue;
    }
    const ReusableGenerationReferenceV1 reference =
        make_reference(generation, slot);
    const std::uint32_t exact_partition =
        reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(partition_callbacks, exact_partition);
    if (!reference_matches(generation, reference)) {
      release_exact(partition_callbacks, exact_partition);
      continue;
    }
    if (janitor_callbacks.owner_alive != nullptr) {
      for (std::uint32_t owner_slot = 0;
           owner_slot < kReusableReferenceOwnersPerGenerationV1;
           ++owner_slot) {
        ReusableSharedReferenceOwnerV1 &shared_owner =
            generation.reference_owners[owner_slot];
        if (retired_this_pass + reaped_this_pass >= remaining) {
          break;
        }
        if (!shared_owner.active) {
          continue;
        }
        const ReusableJanitorOwnerObservationV1 observation =
            make_reference_owner_observation(generation, slot, shared_owner,
                                             owner_slot);
        if (!observation.valid()) {
          release_exact(partition_callbacks, exact_partition);
          result.error = ReusableRegistryErrorV1::CorruptGeneration;
          return result;
        }
        if (janitor_callbacks.owner_alive(janitor_callbacks.context,
                                          observation)) {
          continue;
        }
        const ReusableReferenceKindV1 reaped_kind = shared_owner.kind;
        const ReusableRegistryErrorV1 reap_error =
            deactivate_reference_owner(generation, shared_owner);
        if (reap_error != ReusableRegistryErrorV1::None) {
          release_exact(partition_callbacks, exact_partition);
          result.error = reap_error;
          return result;
        }
        if (reaped_kind == ReusableReferenceKindV1::Waiter) {
          ++result.reaped_waiters;
        } else if (reaped_kind == ReusableReferenceKindV1::PlannerPin) {
          ++result.reaped_planner_pins;
        } else if (reaped_kind == ReusableReferenceKindV1::ReaderPin) {
          ++result.reaped_reader_pins;
        } else {
          release_exact(partition_callbacks, exact_partition);
          result.error = ReusableRegistryErrorV1::CorruptGeneration;
          return result;
        }
        ++reaped_this_pass;
      }
    }
    bool producer_dead = false;
    if ((generation.state == CacheEntryState::Claimed ||
         generation.state == CacheEntryState::Building) &&
        janitor_callbacks.owner_alive != nullptr) {
      const ReusableJanitorOwnerObservationV1 observation =
          make_producer_observation(generation, slot);
      if (!observation.valid()) {
        release_exact(partition_callbacks, exact_partition);
        result.error = ReusableRegistryErrorV1::CorruptGeneration;
        return result;
      }
      producer_dead = !janitor_callbacks.owner_alive(
          janitor_callbacks.context, observation);
    }
    release_exact(partition_callbacks, exact_partition);

    if (producer_dead) {
      const ReusableRegistryErrorV1 failure_error = fail_reusable_generation(
          attachment, reference, CacheFailureCode::ProducerExited,
          partition_callbacks);
      if (failure_error != ReusableRegistryErrorV1::None) {
        result.error = failure_error;
        return result;
      }
      if (!remember_janitor_affected_reference(result, reference)) {
        result.error = ReusableRegistryErrorV1::CorruptLayout;
        return result;
      }
      ++result.failed_dead_producers;
    }

    enum class RetireReason : std::uint8_t {
      None,
      Expired,
      Failed,
      Invalidated,
    };
    RetireReason reason = RetireReason::None;
    acquire_exact(partition_callbacks, exact_partition);
    if (!reference_matches(generation, reference)) {
      release_exact(partition_callbacks, exact_partition);
      continue;
    }
    if (generation.state == CacheEntryState::Complete &&
        completed_generation_expired(generation,
                                     generation.metadata.producer_ttl_ms,
                                     now_monotonic_ns)) {
      reason = RetireReason::Expired;
    } else if (generation.state == CacheEntryState::Failed) {
      reason = RetireReason::Failed;
    } else if (generation.state == CacheEntryState::Invalidated) {
      reason = RetireReason::Invalidated;
    }
    const bool referenced = generation.waiter_count != 0 ||
                            generation.planner_pin_count != 0 ||
                            generation.reader_pin_count != 0;
    release_exact(partition_callbacks, exact_partition);
    if (reason == RetireReason::None || referenced ||
        retired_this_pass + reaped_this_pass >= remaining) {
      continue;
    }
    auto manifest =
        retire_reusable_generation(attachment, reference, partition_callbacks);
    if (!manifest.has_value()) {
      if (manifest.error() == ReusableRegistryErrorV1::InvalidState ||
          manifest.error() == ReusableRegistryErrorV1::StaleReference) {
        continue;
      }
      result.error = manifest.error();
      return result;
    }
    if (!remember_janitor_affected_reference(result, reference)) {
      result.error = ReusableRegistryErrorV1::CorruptLayout;
      return result;
    }
    if (static_cast<bool>(*manifest) &&
        result.retired_manifest_count < result.retired_manifests.size()) {
      result.retired_manifests[result.retired_manifest_count++] = *manifest;
    }
    if (reason == RetireReason::Expired) {
      ++result.retired_expired;
    } else if (reason == RetireReason::Failed) {
      ++result.retired_failed;
    } else if (reason == RetireReason::Invalidated) {
      ++result.retired_invalidated;
    }
    ++retired_this_pass;
  }
  shared.janitor_scan_cursor =
      (scan_start + scanned) % shared.config.max_generations;
  return result;
}

/**
 * Copy a caller-bounded redacted view while membership is held shared.
 * Optional partition callbacks make each indexed row internally consistent.
 */
[[nodiscard]] inline ReusableGenerationDiagnosticsResultV1
snapshot_reusable_generation_diagnostics(
    const ReusableRegistryAttachmentV1 &attachment,
    std::span<ReusableGenerationDiagnosticsRowV1> rows,
    const ReusableRegistryPartitionCallbacksV1 &partition_callbacks =
        {}) noexcept {
  using namespace reusable_registry_detail;
  ReusableGenerationDiagnosticsResultV1 result{};
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None ||
      !partition_callbacks.valid()) {
    result.error = attachment_error != ReusableRegistryErrorV1::None
                       ? attachment_error
                       : ReusableRegistryErrorV1::InvalidArgument;
    return result;
  }

  const ReusableRegistryHeaderV1 &shared = header(attachment);
  result.indexed_generation_count = shared.indexed_generations;
  if (rows.empty()) {
    result.truncated = shared.indexed_generations != 0;
    return result;
  }

  const ReusableSharedGenerationV1 *records = generations(attachment);
  for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
    const ReusableSharedGenerationV1 &generation = records[slot];
    if (generation.membership != ReusableRecordMembershipV1::Indexed) {
      continue;
    }
    if (result.count >= rows.size()) {
      result.truncated = true;
      break;
    }

    const ReusableGenerationReferenceV1 reference =
        make_reference(generation, slot);
    if (!reference) {
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      return result;
    }
    const std::uint32_t exact_partition =
        reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(partition_callbacks, exact_partition);
    const bool known_kind =
        generation.entry_kind == CacheEntryKind::ExactFinalResult ||
        generation.entry_kind == CacheEntryKind::ReusableCoverage;
    const bool known_state =
        generation.state == CacheEntryState::Claimed ||
        generation.state == CacheEntryState::Building ||
        generation.state == CacheEntryState::Complete ||
        generation.state == CacheEntryState::Failed ||
        generation.state == CacheEntryState::Invalidated;
    if (!reference_matches(generation, reference) || !known_kind ||
        !known_state ||
        !shared_diagnostic_text_valid(attachment, generation)) {
      release_exact(partition_callbacks, exact_partition);
      result.error = ReusableRegistryErrorV1::CorruptGeneration;
      return result;
    }
    rows[result.count++] = make_diagnostics_row(generation, slot);
    release_exact(partition_callbacks, exact_partition);
  }
  if (!result.truncated && result.count != shared.indexed_generations) {
    result.error = ReusableRegistryErrorV1::CorruptGeneration;
  }
  return result;
}

/** Snapshot bounded diagnostics without exposing query or canonical bytes. */
[[nodiscard]] inline ReusableRegistrySnapshotV1 reusable_registry_snapshot(
    const ReusableRegistryAttachmentV1 &attachment) noexcept {
  using namespace reusable_registry_detail;
  ReusableRegistrySnapshotV1 result{};
  if (validate_reusable_registry_attachment(attachment) !=
      ReusableRegistryErrorV1::None) {
    return result;
  }
  const ReusableRegistryHeaderV1 &shared = header(attachment);
  result.boot_nonce = shared.boot_nonce;
  result.indexed_generations = shared.indexed_generations;
  result.retired_generations = shared.retired_generations;
  result.exact_occupied_buckets = shared.exact_occupied_buckets;
  result.coverage_occupied_buckets = shared.coverage_occupied_buckets;
  result.evictions = shared.evictions;
  result.invalidations = shared.invalidations;
  const ReusableSharedGenerationV1 *records = generations(attachment);
  for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
    const ReusableSharedGenerationV1 &generation = records[slot];
    if (generation.membership == ReusableRecordMembershipV1::Free) {
      ++result.free_generations;
      continue;
    }
    result.waiter_references += generation.waiter_count;
    result.planner_pins += generation.planner_pin_count;
    result.reader_pins += generation.reader_pin_count;
    if (generation.membership != ReusableRecordMembershipV1::Indexed) {
      continue;
    }
    if (generation.entry_kind == CacheEntryKind::ExactFinalResult) {
      ++result.exact_final_generations;
    } else if (generation.entry_kind == CacheEntryKind::ReusableCoverage) {
      ++result.reusable_coverage_generations;
    } else {
      return ReusableRegistrySnapshotV1{};
    }
    if (generation.state == CacheEntryState::Claimed ||
        generation.state == CacheEntryState::Building) {
      ++result.inflight_generations;
    } else if (generation.state == CacheEntryState::Complete) {
      ++result.complete_generations;
    } else if (generation.state == CacheEntryState::Failed) {
      ++result.failed_generations;
    } else if (generation.state == CacheEntryState::Invalidated) {
      ++result.invalidated_generations;
    }
  }
  const ReusableFreeExtentV1 *extents = array_at<ReusableFreeExtentV1>(
      attachment, shared.layout.free_extents_offset);
  for (std::uint32_t index = 0; index < shared.free_extent_count; ++index) {
    result.arena_free_bytes += extents[index].length;
  }
  return result;
}

/**
 * Deep structural audit for startup repair, tests, and operator diagnostics.
 * The adapter must hold exclusive membership and quiesce both directories.
 */
[[nodiscard]] inline ReusableRegistryErrorV1 validate_reusable_registry(
    const ReusableRegistryAttachmentV1 &attachment) noexcept {
  using namespace reusable_registry_detail;
  const ReusableRegistryErrorV1 attachment_error =
      validate_reusable_registry_attachment(attachment);
  if (attachment_error != ReusableRegistryErrorV1::None) {
    return attachment_error;
  }

  const ReusableRegistryHeaderV1 &shared = header(attachment);
  const ReusableSharedGenerationV1 *records = generations(attachment);
  std::uint32_t indexed_records = 0;
  std::uint32_t indexed_exact_records = 0;
  std::uint32_t indexed_coverage_records = 0;
  std::uint32_t retired_records = 0;
  std::uint32_t free_records = 0;
  std::uint64_t allocated_bytes = 0;
  for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
    const ReusableSharedGenerationV1 &generation = records[slot];
    std::uint32_t waiter_owners = 0;
    std::uint32_t planner_owners = 0;
    std::uint32_t reader_owners = 0;
    for (const ReusableSharedReferenceOwnerV1 &shared_owner :
         generation.reference_owners) {
      const bool reserved_clear = std::all_of(
          shared_owner.reserved.begin(), shared_owner.reserved.end(),
          [](std::uint8_t value) noexcept { return value == 0; });
      if (!shared_owner.active) {
        if (shared_owner.kind != ReusableReferenceKindV1::None ||
            shared_owner.owner_sequence != 0 ||
            shared_owner.owner != ReusableReferenceOwnerV1{} ||
            !reserved_clear) {
          return ReusableRegistryErrorV1::CorruptGeneration;
        }
        continue;
      }
      if (!shared_owner.owner.valid() || shared_owner.owner_sequence == 0 ||
          !reserved_clear) {
        return ReusableRegistryErrorV1::CorruptGeneration;
      }
      if (shared_owner.kind == ReusableReferenceKindV1::Waiter) {
        ++waiter_owners;
      } else if (shared_owner.kind == ReusableReferenceKindV1::PlannerPin) {
        ++planner_owners;
      } else if (shared_owner.kind == ReusableReferenceKindV1::ReaderPin) {
        ++reader_owners;
      } else {
        return ReusableRegistryErrorV1::CorruptGeneration;
      }
    }
    if (waiter_owners != generation.waiter_count ||
        planner_owners != generation.planner_pin_count ||
        reader_owners != generation.reader_pin_count) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    if (generation.membership == ReusableRecordMembershipV1::Free) {
      ++free_records;
      if (generation.state != CacheEntryState::Absent ||
          generation.id.boot_nonce != 0 || generation.id.generation != 0 ||
          generation.id.claim_sequence != 0 ||
          generation.allocation_offset != 0 ||
          generation.allocation_length != 0 ||
          generation.canonical_generation_key ||
          generation.canonical_domain_key || generation.coverage_descriptor ||
          generation.dependency_vector ||
          generation.conservative_guard !=
              ReusableConservativeGuardV1{} ||
          generation.dependency_release_cursor != 0 ||
          generation.dependency_release_state !=
              ReusableDependencyReleaseStateV1::None ||
          generation.dependency_release_sequence != 0 ||
          !std::all_of(generation.reserved_dependency_release.begin(),
                       generation.reserved_dependency_release.end(),
                       [](std::uint8_t value) noexcept { return value == 0; }) ||
          !empty_range_shape(generation.display_sql) ||
          !empty_range_shape(generation.canonical_plan) ||
          !empty_range_shape(generation.producer_plan) ||
          generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
          generation.reader_pin_count != 0 ||
          !shared_diagnostic_text_valid(attachment, generation) ||
          generation.next_in_exact_bucket != kInvalidReusableRegistrySlotV1 ||
          generation.next_in_coverage_bucket !=
              kInvalidReusableRegistrySlotV1) {
        return ReusableRegistryErrorV1::CorruptGeneration;
      }
      continue;
    }
    if (generation.membership != ReusableRecordMembershipV1::Indexed &&
        generation.membership != ReusableRecordMembershipV1::Retired) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    const bool known_kind = known_entry_kind(generation.entry_kind);
    const bool is_coverage =
        generation.entry_kind == CacheEntryKind::ReusableCoverage;
    const bool provider_metadata_consistent =
        generation.metadata.provider_id == 0
            ? generation.metadata.provider_abi_version == 0 &&
                  generation.metadata.provider_domain_abi_version == 0 &&
                  generation.metadata.observability !=
                      WriteObservability::ProviderComplete
            : generation.metadata.provider_abi_version != 0 &&
                  generation.metadata.provider_domain_abi_version != 0 &&
                  generation.metadata.observability ==
                      WriteObservability::ProviderComplete;
    const bool kind_metadata_valid =
        is_coverage
            ? generation.metadata.provider_id != 0 &&
                  generation.metadata.provider_abi_version != 0 &&
                  generation.metadata.provider_domain_abi_version != 0 &&
                  generation.metadata.compatibility_family_id != 0 &&
                  generation.metadata.observability ==
                      WriteObservability::ProviderComplete
            : provider_metadata_consistent;
    if (!known_kind || !kind_metadata_valid ||
        generation.slot_sequence == 0 ||
        generation.id.boot_nonce != shared.boot_nonce ||
        generation.id.generation == 0 || generation.id.claim_sequence == 0 ||
        !generation.producer.valid() ||
        !valid_reusable_security_namespace(
            generation.coverage_directory_key.security_namespace) ||
        generation.metadata.dependency_count > kMaxDependenciesPerGeneration ||
        generation.metadata.reserved != 0 ||
        generation.manifest.reserved != 0 ||
        !write_observability_valid(generation.metadata.observability) ||
        !conservative_guard_valid(
            generation.conservative_guard, generation.metadata,
            generation.coverage_directory_key.security_namespace) ||
        generation.metadata.physical_identity.length >
            generation.metadata.physical_identity.bytes.size() ||
        !std::all_of(generation.reserved_dependency_release.begin(),
                     generation.reserved_dependency_release.end(),
                     [](std::uint8_t value) noexcept { return value == 0; }) ||
        !shared_dependency_vector_valid(attachment, generation) ||
        !shared_diagnostic_text_valid(attachment, generation) ||
        !range_valid(attachment, generation.canonical_generation_key) ||
        generation.canonical_generation_key.length > kMaxCanonicalKeyBytes ||
        generation.coverage_descriptor.length > kMaxProviderRecipeBytes) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    const bool domain_valid =
        is_coverage
            ? range_valid(attachment, generation.canonical_domain_key) &&
                  generation.canonical_domain_key.length <=
                      kMaxCoverageDomainKeyBytes
            : generation.canonical_domain_key.offset == 0 &&
                  generation.canonical_domain_key.length == 0 &&
                  generation.coverage_directory_key.coverage_domain_digest ==
                      CacheDigest256{};
    const bool descriptor_shape_valid =
        generation.coverage_descriptor.length == 0
            ? generation.coverage_descriptor.offset == 0
            : range_valid(attachment, generation.coverage_descriptor);
    const bool descriptor_valid =
        is_coverage ? descriptor_shape_valid
                    : generation.coverage_descriptor.offset == 0 &&
                          generation.coverage_descriptor.length == 0;
    const std::uint64_t unaligned_length =
        static_cast<std::uint64_t>(generation.canonical_domain_key.length) +
        generation.canonical_generation_key.length +
        generation.coverage_descriptor.length +
        generation.dependency_vector.length;
    const std::uint64_t aligned_length = align_up(unaligned_length, 8U);
    const bool identity_start_valid =
        is_coverage
            ? generation.allocation_offset ==
                  generation.canonical_domain_key.offset
            : generation.allocation_offset ==
                  generation.canonical_generation_key.offset;
    const bool domain_contiguous =
        !is_coverage ||
        static_cast<std::uint64_t>(generation.canonical_domain_key.offset) +
                generation.canonical_domain_key.length ==
            generation.canonical_generation_key.offset;
    const std::uint64_t generation_key_end =
        static_cast<std::uint64_t>(
            generation.canonical_generation_key.offset) +
        generation.canonical_generation_key.length;
    const bool descriptor_contiguous =
        generation.coverage_descriptor.length == 0
            ? generation.coverage_descriptor.offset == 0
            : generation_key_end == generation.coverage_descriptor.offset;
    const bool dependency_vector_contiguous =
        generation_key_end + generation.coverage_descriptor.length ==
        generation.dependency_vector.offset;
    const bool contiguous = identity_start_valid && domain_contiguous &&
                            descriptor_contiguous &&
                            dependency_vector_contiguous;
    const std::uint64_t allocation_end =
        static_cast<std::uint64_t>(generation.allocation_offset) +
        generation.allocation_length;
    if (!domain_valid || !descriptor_valid || !contiguous ||
        aligned_length == 0 ||
        generation.allocation_length != aligned_length ||
        generation.allocation_offset % 8U != 0 ||
        generation.allocation_length % 8U != 0 ||
        generation.allocation_offset < shared.layout.byte_arena_offset ||
        allocation_end >
            static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                shared.config.byte_arena_bytes) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    const std::uint64_t diagnostic_bytes =
        diagnostic_allocation(generation.display_sql).length +
        static_cast<std::uint64_t>(
            diagnostic_allocation(generation.canonical_plan).length) +
        diagnostic_allocation(generation.producer_plan).length;
    allocated_bytes += generation.allocation_length + diagnostic_bytes;
    if (allocated_bytes > shared.config.byte_arena_bytes) {
      return ReusableRegistryErrorV1::CorruptLayout;
    }

    if (generation.membership == ReusableRecordMembershipV1::Retired) {
      ++retired_records;
      const bool pending_release =
          generation.dependency_release_state ==
              ReusableDependencyReleaseStateV1::Pending &&
          generation.dependency_release_cursor <=
              generation_reference_count(generation) &&
          generation.dependency_release_sequence == 0;
      const bool prepared_release =
          generation.dependency_release_state ==
              ReusableDependencyReleaseStateV1::Prepared &&
          generation.dependency_release_cursor <=
              generation_reference_count(generation) &&
          generation.dependency_release_sequence != 0;
      const bool released =
          generation.dependency_release_state ==
              ReusableDependencyReleaseStateV1::Released &&
          generation.dependency_release_cursor ==
              generation_reference_count(generation) &&
          generation.dependency_release_sequence != 0;
      if (generation.state != CacheEntryState::Evicting ||
          generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
          generation.reader_pin_count != 0 ||
          (!pending_release && !prepared_release && !released) ||
          generation.next_in_exact_bucket != kInvalidReusableRegistrySlotV1 ||
          generation.next_in_coverage_bucket !=
              kInvalidReusableRegistrySlotV1) {
        return ReusableRegistryErrorV1::CorruptGeneration;
      }
      continue;
    }
    ++indexed_records;
    if (generation.dependency_release_state !=
            ReusableDependencyReleaseStateV1::Retained ||
        generation.dependency_release_cursor != 0 ||
        generation.dependency_release_sequence != 0) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    if (is_coverage) {
      ++indexed_coverage_records;
    } else {
      ++indexed_exact_records;
      if (generation.next_in_coverage_bucket !=
          kInvalidReusableRegistrySlotV1) {
        return ReusableRegistryErrorV1::CorruptGeneration;
      }
    }
    const bool indexed_state = generation.state == CacheEntryState::Claimed ||
                               generation.state == CacheEntryState::Building ||
                               generation.state == CacheEntryState::Complete ||
                               generation.state == CacheEntryState::Failed ||
                               generation.state == CacheEntryState::Invalidated;
    if (!indexed_state ||
        (generation.state == CacheEntryState::Complete &&
         (!generation.manifest || generation.manifest.id != generation.id ||
          generation.failure != CacheFailureCode::None ||
          generation.completed_monotonic_ns <
              generation.started_monotonic_ns)) ||
        (generation.state == CacheEntryState::Failed &&
         generation.failure == CacheFailureCode::None) ||
        (generation.state == CacheEntryState::Invalidated &&
         generation.failure == CacheFailureCode::None) ||
        ((generation.state == CacheEntryState::Claimed ||
          generation.state == CacheEntryState::Building) &&
         (generation.failure != CacheFailureCode::None ||
          static_cast<bool>(generation.manifest)))) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
  }
  if (indexed_records != shared.indexed_generations ||
      indexed_exact_records + indexed_coverage_records != indexed_records ||
      retired_records != shared.retired_generations ||
      indexed_records + retired_records + free_records !=
          shared.config.max_generations) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }

  std::uint32_t free_list_count = 0;
  std::uint32_t current = shared.free_generation_head;
  while (current != kInvalidReusableRegistrySlotV1 &&
         free_list_count <= shared.config.max_generations) {
    if (current >= shared.config.max_generations ||
        records[current].membership != ReusableRecordMembershipV1::Free) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    ++free_list_count;
    current = records[current].next_retired_or_free;
  }
  if (current != kInvalidReusableRegistrySlotV1 ||
      free_list_count != free_records) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }
  std::uint32_t retired_list_count = 0;
  current = shared.retired_generation_head;
  while (current != kInvalidReusableRegistrySlotV1 &&
         retired_list_count <= shared.config.max_generations) {
    if (current >= shared.config.max_generations ||
        records[current].membership != ReusableRecordMembershipV1::Retired) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
    ++retired_list_count;
    current = records[current].next_retired_or_free;
  }
  if (current != kInvalidReusableRegistrySlotV1 ||
      retired_list_count != retired_records) {
    return ReusableRegistryErrorV1::CorruptGeneration;
  }

  const ReusableExactBucketV1 *exact = exact_buckets(attachment);
  std::uint32_t exact_occupied = 0;
  std::uint64_t exact_members = 0;
  for (std::uint32_t bucket_index = 0;
       bucket_index < shared.config.exact_directory_capacity; ++bucket_index) {
    const ReusableExactBucketV1 &bucket = exact[bucket_index];
    if (bucket.slot_state != DirectorySlotState::Occupied) {
      if ((bucket.slot_state != DirectorySlotState::Empty &&
           bucket.slot_state != DirectorySlotState::Tombstone) ||
          bucket.first_generation != kInvalidReusableRegistrySlotV1 ||
          bucket.generation_count != 0 ||
          bucket.terminal_generation_count != 0) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      continue;
    }
    ++exact_occupied;
    if (bucket.generation_count == 0 ||
        bucket.generation_count > kMaxGenerationsPerExactDigest ||
        bucket.terminal_generation_count > bucket.generation_count) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    std::uint32_t terminal_count = 0;
    current = bucket.first_generation;
    for (std::uint32_t visited = 0; visited < bucket.generation_count;
         ++visited) {
      if (current == kInvalidReusableRegistrySlotV1 ||
          current >= shared.config.max_generations ||
          records[current].membership != ReusableRecordMembershipV1::Indexed ||
          !known_entry_kind(records[current].entry_kind) ||
          records[current].id.digest != bucket.digest) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      terminal_count += terminal_state(records[current].state) ? 1U : 0U;
      current = records[current].next_in_exact_bucket;
    }
    if (current != kInvalidReusableRegistrySlotV1 ||
        terminal_count != bucket.terminal_generation_count) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    exact_members += bucket.generation_count;
  }
  if (exact_occupied != shared.exact_occupied_buckets ||
      exact_members != indexed_records) {
    return ReusableRegistryErrorV1::CorruptDirectory;
  }

  const ReusableCoverageBucketV1 *coverage = coverage_buckets(attachment);
  std::uint32_t coverage_occupied = 0;
  std::uint64_t coverage_members = 0;
  for (std::uint32_t bucket_index = 0;
       bucket_index < shared.config.coverage_directory_capacity;
       ++bucket_index) {
    const ReusableCoverageBucketV1 &bucket = coverage[bucket_index];
    if (bucket.slot_state != DirectorySlotState::Occupied) {
      if ((bucket.slot_state != DirectorySlotState::Empty &&
           bucket.slot_state != DirectorySlotState::Tombstone) ||
          bucket.first_generation != kInvalidReusableRegistrySlotV1 ||
          bucket.generation_count != 0 ||
          bucket.terminal_generation_count != 0) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      continue;
    }
    ++coverage_occupied;
    if (bucket.generation_count == 0 ||
        bucket.generation_count > kMaxCoverageGenerationsPerDomain ||
        bucket.terminal_generation_count > bucket.generation_count) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    std::uint32_t terminal_count = 0;
    current = bucket.first_generation;
    for (std::uint32_t visited = 0; visited < bucket.generation_count;
         ++visited) {
      if (current == kInvalidReusableRegistrySlotV1 ||
          current >= shared.config.max_generations ||
          records[current].membership != ReusableRecordMembershipV1::Indexed ||
          records[current].entry_kind != CacheEntryKind::ReusableCoverage ||
          records[current].coverage_directory_key != bucket.key) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      terminal_count += terminal_state(records[current].state) ? 1U : 0U;
      current = records[current].next_in_coverage_bucket;
    }
    if (current != kInvalidReusableRegistrySlotV1 ||
        terminal_count != bucket.terminal_generation_count) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    coverage_members += bucket.generation_count;
  }
  if (coverage_occupied != shared.coverage_occupied_buckets ||
      coverage_members != indexed_coverage_records) {
    return ReusableRegistryErrorV1::CorruptDirectory;
  }

  for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
    const ReusableSharedGenerationV1 &generation = records[slot];
    if (generation.membership != ReusableRecordMembershipV1::Indexed) {
      continue;
    }
    const DirectoryProbeV1 exact_probe =
        probe_exact_directory(attachment, generation.id.digest);
    if (!exact_probe.found) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    std::uint32_t occurrences = 0;
    current = exact[exact_probe.slot].first_generation;
    for (std::uint32_t visited = 0;
         visited < exact[exact_probe.slot].generation_count; ++visited) {
      occurrences += current == slot ? 1U : 0U;
      current = records[current].next_in_exact_bucket;
    }
    if (occurrences != 1) {
      return ReusableRegistryErrorV1::CorruptDirectory;
    }
    if (generation.entry_kind == CacheEntryKind::ReusableCoverage) {
      const DirectoryProbeV1 coverage_probe = probe_coverage_directory(
          attachment, generation.coverage_directory_key);
      if (!coverage_probe.found) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
      occurrences = 0;
      current = coverage[coverage_probe.slot].first_generation;
      for (std::uint32_t visited = 0;
           visited < coverage[coverage_probe.slot].generation_count;
           ++visited) {
        occurrences += current == slot ? 1U : 0U;
        current = records[current].next_in_coverage_bucket;
      }
      if (occurrences != 1) {
        return ReusableRegistryErrorV1::CorruptDirectory;
      }
    } else if (generation.entry_kind != CacheEntryKind::ExactFinalResult) {
      return ReusableRegistryErrorV1::CorruptGeneration;
    }
  }

  const ReusableFreeExtentV1 *extents = array_at<ReusableFreeExtentV1>(
      attachment, shared.layout.free_extents_offset);
  std::uint64_t free_bytes = 0;
  std::uint64_t previous_end = shared.layout.byte_arena_offset;
  for (std::uint32_t extent_index = 0; extent_index < shared.free_extent_count;
       ++extent_index) {
    const ReusableFreeExtentV1 &extent = extents[extent_index];
    const std::uint64_t extent_end =
        static_cast<std::uint64_t>(extent.offset) + extent.length;
    if (extent.length == 0 || extent.offset % 8U != 0 ||
        extent.length % 8U != 0 ||
        extent.offset < shared.layout.byte_arena_offset ||
        extent_end >
            static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                shared.config.byte_arena_bytes ||
        (extent_index != 0 && extent.offset <= previous_end)) {
      return ReusableRegistryErrorV1::CorruptLayout;
    }
    previous_end = extent_end;
    free_bytes += extent.length;
  }
  if (free_bytes + allocated_bytes != shared.config.byte_arena_bytes) {
    return ReusableRegistryErrorV1::CorruptLayout;
  }
  return ReusableRegistryErrorV1::None;
}

static_assert(std::is_standard_layout_v<ReusableRegistryHeaderV1>);
static_assert(std::is_standard_layout_v<ReusableSharedGenerationV1>);
static_assert(std::is_trivially_copyable_v<ReusableSharedGenerationV1>);
static_assert(std::is_trivially_copyable_v<ReusableManifestHandleV1>);
static_assert(std::is_trivially_copyable_v<ReusableGenerationReferenceV1>);
static_assert(std::is_trivially_copyable_v<ReusableSharedReferenceOwnerV1>);
static_assert(std::is_trivially_copyable_v<ReusableReferenceTokenV1>);
static_assert(
    std::is_trivially_copyable_v<ReusableJanitorOwnerObservationV1>);
static_assert(std::is_trivially_copyable_v<ReusableInvalidationSelectorV1>);
static_assert(std::is_trivially_copyable_v<ReusableInvalidationResultV1>);
static_assert(std::is_standard_layout_v<ReusableGenerationDiagnosticsRowV1>);
static_assert(std::is_trivially_copyable_v<ReusableGenerationDiagnosticsRowV1>);
static_assert(
    std::is_trivially_copyable_v<ReusableGenerationDiagnosticsResultV1>);

} // namespace bioseq::cache
