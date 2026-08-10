/**
 * @file bioseq_caching/core/dependency_directory.cppm
 * @brief Offset-backed, refcounted dependency epoch directory.
 *
 * The region contains no process pointer, PostgreSQL type, lock, or allocator.
 * Every mutable operation is serialized by caller-owned partition callbacks.
 * Record and change generations come from one caller-owned monotonic source.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:dependency_directory;

import :wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t kDependencyDirectoryMagicV1 = 0x42444431U;
inline constexpr std::uint32_t kDependencyDirectoryAbiVersionV1 = 3;
inline constexpr std::uint32_t kMinimumDependencyDirectoryLogicalCapacityV1 =
    4'096;
inline constexpr std::uint32_t kDefaultDependencyDirectoryLogicalCapacityV1 =
    4'096;
inline constexpr std::uint32_t kMaximumDependencyDirectoryLogicalCapacityV1 =
    262'144;
inline constexpr std::uint32_t kDependencyDirectoryEntriesPerBackendV1 = 512;
inline constexpr std::uint32_t kMaximumDependencyDirectoryPhysicalCapacityV1 =
    524'288;
inline constexpr std::uint32_t kDefaultDependencyDirectoryPartitionsV1 = 16;
inline constexpr std::uint32_t kDefaultDependencyReclaimScanV1 = 64;
inline constexpr std::uint32_t kMaximumDependencyDirectoryPartitionsV1 =
    1'024;
inline constexpr std::uint32_t kMaximumDependencyVectorSizeV1 = 1'024;
inline constexpr std::uint32_t kMaximumDependencyReferenceVectorV1 = 1'024;
inline constexpr std::uint32_t kDefaultDependencyBackendReferenceLimitV1 =
    1'024;
inline constexpr std::uint32_t kDefaultDependencyGenerationReferenceLimitV1 =
    65'536;
inline constexpr std::uint32_t kMaximumDependencyReferenceCountV1 =
    1U << 20U;
inline constexpr std::uint32_t kDependencyDirectoryPressurePercentV1 = 70;
inline constexpr std::uint32_t kInvalidDependencyPartitionV1 =
    (std::numeric_limits<std::uint32_t>::max)();

/** Every error tells the adapter to bypass or use its coarse epoch. */
enum class DependencyDirectoryErrorV1 : std::uint32_t {
  None = 0,
  InvalidArgument,
  InvalidConfiguration,
  RegionTooSmall,
  RegionSizeMismatch,
  RegionMisaligned,
  NotInitialized,
  AbiMismatch,
  CorruptLayout,
  CorruptDirectory,
  DirectoryFull,
  PressureLimit,
  NotFound,
  StaleReference,
  InvalidReferenceKind,
  ObservabilityMismatch,
  PhysicalIdentityMismatch,
  CounterOverflow,
  CounterUnderflow,
  SequenceUnavailable,
  WakeFailed,
  InvalidState,
  EmergencyFallback,
};

/** The three independently audited reasons a dependency remains resident. */
enum class DependencyReferenceKindV1 : std::uint8_t {
  None = 0,
  Owner = 1,
  Ticket = 2,
  Generation = 3,
};

enum class DependencyTransitionOutcomeV1 : std::uint8_t {
  Aborted = 0,
  Committed = 1,
  /** PostgreSQL proved that no write became externally visible. */
  KnownAborted = 2,
};

enum class DependencyPhysicalIdentityActionV1 : std::uint8_t {
  Preserve = 1,
  Replace = 2,
};

struct DependencyTransitionResolutionV1 {
  DependencyTransitionOutcomeV1 outcome =
      DependencyTransitionOutcomeV1::Aborted;
  DependencyPhysicalIdentityActionV1 physical_identity_action =
      DependencyPhysicalIdentityActionV1::Preserve;
  std::array<std::uint8_t, 6> reserved{};
  PhysicalIdentityV1 physical_identity{};
};

enum class DependencyTerminalStageV1 : std::uint8_t {
  EpochPublished = 0,
  TransitionDecremented = 1,
  ChangeGenerationPublished = 2,
  WaitersBroadcast = 3,
};

enum class DependencyDirectoryLockModeV1 : std::uint8_t {
  AcquireRelease = 1,
  AlreadyLocked = 2,
};

enum class DependencyAdmissionOutcomeV1 : std::uint8_t {
  None = 0,
  Existing = 1,
  Inserted = 2,
  PressureFallback = 3,
};

/**
 * Fixed preload configuration.
 *
 * logical_capacity is the admission budget.  The layout reserves a separate
 * power-of-two physical table with at least twice that many slots.  The
 * backend and generation limits are hard per-record refcount bounds supplied
 * by the adapter from its own fixed shared-resource budgets.
 */
struct DependencyDirectoryConfigV1 {
  std::uint32_t logical_capacity =
      kDefaultDependencyDirectoryLogicalCapacityV1;
  std::uint32_t partition_count = kDefaultDependencyDirectoryPartitionsV1;
  std::uint32_t reclaim_scan_limit = kDefaultDependencyReclaimScanV1;
  std::uint32_t pressure_percent = kDependencyDirectoryPressurePercentV1;
  std::uint32_t backend_reference_limit =
      kDefaultDependencyBackendReferenceLimitV1;
  std::uint32_t generation_reference_limit =
      kDefaultDependencyGenerationReferenceLimitV1;
  std::uint32_t reserved = 0;

  [[nodiscard]] bool
  operator==(const DependencyDirectoryConfigV1 &) const noexcept = default;
};

/** Exact region-relative offsets computed before shared memory is requested. */
struct DependencyDirectoryLayoutV1 {
  std::uint32_t total_bytes = 0;
  std::uint32_t partitions_offset = 0;
  std::uint32_t records_offset = 0;
  std::uint32_t logical_capacity = 0;
  std::uint32_t physical_capacity = 0;
  std::uint32_t partition_capacity = 0;
  std::uint32_t reserved = 0;

  [[nodiscard]] bool
  operator==(const DependencyDirectoryLayoutV1 &) const noexcept = default;
};

struct DependencyDirectoryPreloadRequestV1 {
  DependencyDirectoryConfigV1 config{};
  DependencyDirectoryLayoutV1 layout{};
  std::uint32_t partition_lock_count = 0;
  std::uint32_t reserved = 0;
};

struct DependencyDirectoryPreloadResultV1 {
  DependencyDirectoryPreloadRequestV1 request{};
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None;
  }
};

/** One fixed partition descriptor; record_offset is always region-relative. */
struct DependencyDirectoryPartitionV1 {
  std::uint32_t record_offset = 0;
  std::uint32_t physical_capacity = 0;
  std::uint32_t logical_capacity = 0;
  std::uint32_t occupied_count = 0;
  std::uint32_t tombstone_count = 0;
  std::uint32_t reclaim_cursor = 0;
  std::uint32_t emergency_latched = 0;
  std::uint32_t reserved = 0;
  std::uint64_t last_sequence = 0;
  std::uint64_t emergency_change_generation = 0;
  std::uint64_t pressure_event_count = 0;
  std::uint64_t reclaim_attempt_count = 0;
  std::uint64_t reclaimed_record_count = 0;
  std::uint64_t retiring_recheck_failure_count = 0;
  std::uint64_t pressure_fallback_count = 0;
  std::uint64_t directory_full_fallback_count = 0;
  std::uint64_t emergency_fallback_count = 0;
  std::uint64_t wake_failure_count = 0;
};

/** One bounded predecessor cohort retained only for exact old-ref release. */
struct RetiredDependencyReferenceCohortV1 {
  PhysicalIdentityV1 physical_identity{};
  std::uint64_t record_generation = 0;
  std::uint64_t committed_epoch = 0;
  std::uint32_t generation_ref_count = 0;
  std::uint32_t ticket_ref_count = 0;
  WriteObservability observability = WriteObservability::None;
  std::uint32_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return record_generation != 0;
  }

  [[nodiscard]] bool operator==(
      const RetiredDependencyReferenceCohortV1 &) const noexcept = default;
};

/** One pointer-free dependency epoch and all reasons it cannot be reclaimed. */
struct SharedDependencyEpochV1 {
  DirectorySlotState slot_state = DirectorySlotState::Empty;
  std::uint32_t reserved = 0;
  DependencyKey key{};
  std::uint32_t alignment_reserved = 0;
  PhysicalIdentityV1 physical_identity{};
  std::uint64_t record_generation = 0;
  std::uint64_t committed_epoch = 0;
  std::uint64_t change_generation = 0;
  std::uint32_t transition_count = 0;
  std::uint32_t generation_ref_count = 0;
  std::uint32_t ticket_ref_count = 0;
  std::uint32_t owner_ref_count = 0;
  WriteObservability observability = WriteObservability::None;
  std::uint32_t reserved2 = 0;
  RetiredDependencyReferenceCohortV1 retired_references{};
};

/** Header occupies offset zero; process-local attachments contain the pointer. */
struct DependencyDirectoryHeaderV1 {
  std::uint32_t magic = 0;
  std::uint32_t abi_version = 0;
  BootstrapState bootstrap_state = BootstrapState::Uninitialized;
  std::uint32_t reserved = 0;
  DependencyDirectoryConfigV1 config{};
  DependencyDirectoryLayoutV1 layout{};
};

struct DependencyDirectoryAttachmentV1 {
  std::span<std::byte> region{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !region.empty();
  }
};

struct DependencyEpochSnapshotV1;

/** No lock address enters the region.  A null acquire/release pair is test-safe. */
struct DependencyDirectoryCallbacksV1 {
  void *context = nullptr;
  DependencyDirectoryLockModeV1 lock_mode =
      DependencyDirectoryLockModeV1::AcquireRelease;
  std::array<std::uint8_t, 7> reserved{};
  void (*acquire_partition)(void *, std::uint32_t) noexcept = nullptr;
  void (*release_partition)(void *, std::uint32_t) noexcept = nullptr;
  std::uint64_t (*next_sequence)(void *) noexcept = nullptr;
  bool (*wake_partition)(void *, std::uint32_t) noexcept = nullptr;
  void (*observe_terminal_stage)(
      void *, DependencyTerminalStageV1,
      const DependencyEpochSnapshotV1 &) noexcept = nullptr;

  [[nodiscard]] bool locks_valid() const noexcept {
    const bool reserved_zero = std::all_of(
        reserved.begin(), reserved.end(),
        [](std::uint8_t value) noexcept { return value == 0; });
    if (!reserved_zero) {
      return false;
    }
    if (lock_mode == DependencyDirectoryLockModeV1::AcquireRelease) {
      return acquire_partition != nullptr && release_partition != nullptr;
    }
    if (lock_mode == DependencyDirectoryLockModeV1::AlreadyLocked) {
      return acquire_partition == nullptr && release_partition == nullptr;
    }
    return false;
  }

  [[nodiscard]] bool mutation_valid() const noexcept {
    return locks_valid() && next_sequence != nullptr;
  }

  [[nodiscard]] bool terminal_mutation_valid() const noexcept {
    return mutation_valid() && wake_partition != nullptr;
  }
};

/**
 * ABA-safe process-local linear capability; no shared address is persisted.
 * Adapters must move, not duplicate, active references.  transition_active
 * catches local replay and the transition owner ledger remains authoritative
 * when multiple owners retain the same aggregate shared record.
 */
struct DependencyReferenceV1 {
  DependencyKey key{};
  std::uint32_t record_offset = 0;
  std::uint32_t partition_index = kInvalidDependencyPartitionV1;
  std::uint32_t alignment_reserved = 0;
  PhysicalIdentityV1 physical_identity{};
  std::uint64_t record_generation = 0;
  DependencyReferenceKindV1 kind = DependencyReferenceKindV1::None;
  bool active = false;
  bool transition_active = false;
  std::array<std::uint8_t, 5> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return active && record_offset != 0 &&
           partition_index != kInvalidDependencyPartitionV1 &&
           alignment_reserved == 0 &&
           physical_identity_valid_v1(physical_identity) &&
           record_generation != 0 &&
           kind != DependencyReferenceKindV1::None &&
           (!transition_active || kind == DependencyReferenceKindV1::Owner) &&
           std::all_of(reserved.begin(), reserved.end(),
                       [](std::uint8_t value) noexcept {
                         return value == 0;
                       });
  }

  [[nodiscard]] bool operator==(const DependencyReferenceV1 &) const noexcept =
      default;
};

struct DependencyReferenceRequestV1 {
  DependencyKey key{};
  PhysicalIdentityV1 physical_identity{};
  WriteObservability observability = WriteObservability::None;
  DependencyReferenceKindV1 kind = DependencyReferenceKindV1::None;
  std::array<std::uint8_t, 3> reserved{};
};

struct DependencyEpochSnapshotV1 {
  DependencyKey key{};
  std::uint32_t alignment_reserved = 0;
  PhysicalIdentityV1 physical_identity{};
  std::uint64_t record_generation = 0;
  std::uint64_t committed_epoch = 0;
  std::uint64_t change_generation = 0;
  std::uint32_t transition_count = 0;
  std::uint32_t generation_ref_count = 0;
  std::uint32_t ticket_ref_count = 0;
  std::uint32_t owner_ref_count = 0;
  WriteObservability observability = WriteObservability::None;
  std::uint32_t reserved = 0;
};

struct DependencyReferenceResultV1 {
  DependencyReferenceV1 reference{};
  DependencyEpochSnapshotV1 snapshot{};
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;
  std::uint32_t reclaimed_count = 0;
  bool pressure_observed = false;
  bool coarse_fallback_required = false;
  DependencyAdmissionOutcomeV1 outcome = DependencyAdmissionOutcomeV1::None;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None &&
           static_cast<bool>(reference);
  }
};

struct DependencySnapshotResultV1 {
  DependencyEpochSnapshotV1 snapshot{};
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;
  bool coarse_fallback_required = false;
  std::array<std::uint8_t, 3> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None;
  }
};

struct DependencyMutationResultV1 {
  DependencyEpochSnapshotV1 snapshot{};
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;
  std::uint32_t reclaimed_count = 0;
  bool applied = false;
  bool diagnostic_success = false;
  bool coarse_fallback_required = false;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None && applied &&
           diagnostic_success;
  }
};

struct DependencyReclaimResultV1 {
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;
  std::uint32_t reclaimed_count = 0;
  std::uint32_t scanned_count = 0;
  bool pressure_remains = false;
  bool coarse_fallback_required = false;
  std::array<std::uint8_t, 2> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None;
  }
};

struct DependencyDirectoryStatsV1 {
  std::uint32_t logical_capacity = 0;
  std::uint32_t physical_capacity = 0;
  std::uint32_t partition_count = 0;
  std::uint32_t occupied_count = 0;
  std::uint32_t tombstone_count = 0;
  std::uint32_t transition_count = 0;
  std::uint32_t generation_ref_count = 0;
  std::uint32_t ticket_ref_count = 0;
  std::uint32_t owner_ref_count = 0;
  std::uint64_t maximum_change_generation = 0;
  std::uint64_t emergency_change_generation = 0;
  std::uint64_t pressure_event_count = 0;
  std::uint64_t reclaim_attempt_count = 0;
  std::uint64_t reclaimed_record_count = 0;
  std::uint64_t retiring_recheck_failure_count = 0;
  std::uint64_t pressure_fallback_count = 0;
  std::uint64_t directory_full_fallback_count = 0;
  std::uint64_t emergency_fallback_count = 0;
  std::uint64_t wake_failure_count = 0;
};

struct DependencyDirectoryStatsResultV1 {
  DependencyDirectoryStatsV1 stats{};
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None;
  }
};

[[nodiscard]] inline DependencyDirectoryPreloadResultV1
dependency_directory_preload_request(
    DependencyDirectoryConfigV1 config = {}) noexcept;

static_assert(std::is_trivially_copyable_v<DependencyDirectoryConfigV1>);
static_assert(std::is_trivially_copyable_v<DependencyDirectoryLayoutV1>);
static_assert(std::is_trivially_copyable_v<DependencyDirectoryPartitionV1>);
static_assert(
    std::is_trivially_copyable_v<RetiredDependencyReferenceCohortV1>);
static_assert(std::is_trivially_copyable_v<SharedDependencyEpochV1>);
static_assert(std::is_trivially_copyable_v<DependencyReferenceV1>);
static_assert(std::is_standard_layout_v<DependencyDirectoryConfigV1>);
static_assert(std::is_standard_layout_v<DependencyDirectoryLayoutV1>);
static_assert(std::is_standard_layout_v<DependencyDirectoryPartitionV1>);
static_assert(std::is_standard_layout_v<RetiredDependencyReferenceCohortV1>);
static_assert(std::is_standard_layout_v<SharedDependencyEpochV1>);
static_assert(std::is_standard_layout_v<DependencyDirectoryHeaderV1>);
static_assert(alignof(DependencyDirectoryPartitionV1) >=
              alignof(std::uint64_t));

inline constexpr std::size_t kDependencyDirectoryRegionAlignmentV1 =
    std::max({alignof(DependencyDirectoryHeaderV1),
              alignof(DependencyDirectoryPartitionV1),
              alignof(SharedDependencyEpochV1)});
static_assert((kDependencyDirectoryRegionAlignmentV1 &
               (kDependencyDirectoryRegionAlignmentV1 - 1U)) == 0);

/** Clamp the adapter's MaxBackends-derived logical budget without overflow. */
[[nodiscard]] constexpr std::uint32_t
dependency_directory_logical_capacity_for_backends(
    std::uint32_t max_backends) noexcept {
  const std::uint64_t requested =
      static_cast<std::uint64_t>(max_backends) *
      kDependencyDirectoryEntriesPerBackendV1;
  return static_cast<std::uint32_t>(std::clamp(
      requested,
      static_cast<std::uint64_t>(
          kMinimumDependencyDirectoryLogicalCapacityV1),
      static_cast<std::uint64_t>(
          kMaximumDependencyDirectoryLogicalCapacityV1)));
}

/** Return the exact power-of-two table size, or zero for an invalid budget. */
[[nodiscard]] constexpr std::uint32_t
dependency_directory_physical_capacity(
    std::uint32_t logical_capacity) noexcept {
  if (logical_capacity < kMinimumDependencyDirectoryLogicalCapacityV1 ||
      logical_capacity > kMaximumDependencyDirectoryLogicalCapacityV1) {
    return 0;
  }
  const std::uint64_t minimum =
      static_cast<std::uint64_t>(logical_capacity) * 2U;
  std::uint64_t physical = 1;
  while (physical < minimum) {
    physical <<= 1U;
  }
  return physical <= kMaximumDependencyDirectoryPhysicalCapacityV1
             ? static_cast<std::uint32_t>(physical)
             : 0;
}

namespace dependency_directory_detail {

[[nodiscard]] constexpr bool power_of_two(std::uint32_t value) noexcept {
  return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] constexpr bool valid_transition_outcome(
    DependencyTransitionOutcomeV1 outcome) noexcept {
  return outcome == DependencyTransitionOutcomeV1::Aborted ||
         outcome == DependencyTransitionOutcomeV1::Committed ||
         outcome == DependencyTransitionOutcomeV1::KnownAborted;
}

[[nodiscard]] constexpr bool valid_transition_resolution(
    const DependencyTransitionResolutionV1 &resolution) noexcept {
  const bool reserved_zero = std::all_of(
      resolution.reserved.begin(), resolution.reserved.end(),
      [](std::uint8_t value) noexcept { return value == 0; });
  const bool action_valid =
      resolution.physical_identity_action ==
          DependencyPhysicalIdentityActionV1::Preserve ||
      resolution.physical_identity_action ==
          DependencyPhysicalIdentityActionV1::Replace;
  return valid_transition_outcome(resolution.outcome) && action_valid &&
         reserved_zero &&
         physical_identity_valid_v1(resolution.physical_identity) &&
         (resolution.outcome == DependencyTransitionOutcomeV1::Committed ||
          resolution.physical_identity_action ==
              DependencyPhysicalIdentityActionV1::Preserve);
}

[[nodiscard]] constexpr std::uint32_t logical_partition_capacity(
    std::uint32_t logical_capacity, std::uint32_t partition_count,
    std::uint32_t partition_index) noexcept {
  if (partition_count == 0 || partition_index >= partition_count) {
    return 0;
  }
  return logical_capacity / partition_count +
         (partition_index < logical_capacity % partition_count ? 1U : 0U);
}

inline void saturating_increment(std::uint64_t &value) noexcept {
  if (value != (std::numeric_limits<std::uint64_t>::max)()) {
    ++value;
  }
}

[[nodiscard]] inline bool checked_add_u64(
    std::uint64_t &destination, std::uint64_t value) noexcept {
  if (destination >
      (std::numeric_limits<std::uint64_t>::max)() - value) {
    return false;
  }
  destination += value;
  return true;
}

[[nodiscard]] constexpr std::uint64_t align_up(
    std::uint64_t value, std::uint64_t alignment) noexcept {
  if (alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
      value > (std::numeric_limits<std::uint64_t>::max)() -
                  (alignment - 1U)) {
    return 0;
  }
  return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] constexpr bool valid_observability(
    WriteObservability observability) noexcept {
  return observability == WriteObservability::ProviderComplete ||
         observability == WriteObservability::ExecutorUtilityOnly ||
         observability == WriteObservability::DatabaseWideFallback;
}

[[nodiscard]] constexpr bool valid_reference_kind(
    DependencyReferenceKindV1 kind) noexcept {
  return kind == DependencyReferenceKindV1::Owner ||
         kind == DependencyReferenceKindV1::Ticket ||
         kind == DependencyReferenceKindV1::Generation;
}

[[nodiscard]] constexpr bool valid_dependency_kind(
    DependencyKind kind) noexcept {
  return kind >= DependencyKind::RelationContent &&
         kind <= DependencyKind::DatabaseDefinitionEpoch;
}

[[nodiscard]] constexpr bool valid_key(const DependencyKey &key) noexcept {
  return key.database_oid != 0 && key.reserved == 0 &&
         valid_dependency_kind(key.kind);
}

[[nodiscard]] inline std::uint64_t hash_byte(
    std::uint64_t hash, std::uint8_t value) noexcept {
  return (hash ^ value) * 1'099'511'628'211ULL;
}

[[nodiscard]] inline std::uint64_t hash_u32(
    std::uint64_t hash, std::uint32_t value) noexcept {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    hash = hash_byte(
        hash, static_cast<std::uint8_t>(value >> (index * 8U)));
  }
  return hash;
}

[[nodiscard]] inline std::uint64_t dependency_key_hash(
    const DependencyKey &key) noexcept {
  std::uint64_t hash = 14'695'981'039'346'656'037ULL;
  hash = hash_u32(hash, key.database_oid);
  hash = hash_u32(hash, key.class_oid);
  hash = hash_u32(hash, key.object_oid);
  hash = hash_u32(hash, static_cast<std::uint32_t>(key.object_sub_id));
  hash = hash_u32(hash, static_cast<std::uint32_t>(key.kind));
  return hash_u32(hash, key.reserved);
}

template <class Value>
[[nodiscard]] Value *object_at(
    std::span<std::byte> region, std::uint32_t offset) noexcept {
  if (offset > region.size() || sizeof(Value) > region.size() - offset) {
    return nullptr;
  }
  std::byte *address = region.data() + offset;
  if (reinterpret_cast<std::uintptr_t>(address) % alignof(Value) != 0) {
    return nullptr;
  }
  return reinterpret_cast<Value *>(address);
}

template <class Value>
[[nodiscard]] const Value *object_at(
    std::span<const std::byte> region, std::uint32_t offset) noexcept {
  if (offset > region.size() || sizeof(Value) > region.size() - offset) {
    return nullptr;
  }
  const std::byte *address = region.data() + offset;
  if (reinterpret_cast<std::uintptr_t>(address) % alignof(Value) != 0) {
    return nullptr;
  }
  return reinterpret_cast<const Value *>(address);
}

[[nodiscard]] inline DependencyDirectoryHeaderV1 *header(
    DependencyDirectoryAttachmentV1 directory) noexcept {
  return object_at<DependencyDirectoryHeaderV1>(directory.region, 0);
}

[[nodiscard]] inline DependencyDirectoryPartitionV1 *partition(
    DependencyDirectoryAttachmentV1 directory,
    std::uint32_t partition_index) noexcept {
  DependencyDirectoryHeaderV1 *value = header(directory);
  if (value == nullptr || partition_index >= value->config.partition_count) {
    return nullptr;
  }
  const std::uint64_t offset =
      static_cast<std::uint64_t>(value->layout.partitions_offset) +
      static_cast<std::uint64_t>(partition_index) *
          sizeof(DependencyDirectoryPartitionV1);
  if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
    return nullptr;
  }
  return object_at<DependencyDirectoryPartitionV1>(
      directory.region, static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline SharedDependencyEpochV1 *record_at_offset(
    DependencyDirectoryAttachmentV1 directory,
    std::uint32_t offset) noexcept {
  return object_at<SharedDependencyEpochV1>(directory.region, offset);
}

[[nodiscard]] inline SharedDependencyEpochV1 *record_at_index(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyDirectoryPartitionV1 &partition_value,
    std::uint32_t index) noexcept {
  if (index >= partition_value.physical_capacity) {
    return nullptr;
  }
  const std::uint64_t offset =
      static_cast<std::uint64_t>(partition_value.record_offset) +
      static_cast<std::uint64_t>(index) * sizeof(SharedDependencyEpochV1);
  if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
    return nullptr;
  }
  return record_at_offset(directory, static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline std::uint32_t record_offset(
    const DependencyDirectoryPartitionV1 &partition_value,
    std::uint32_t index) noexcept {
  const std::uint64_t offset =
      static_cast<std::uint64_t>(partition_value.record_offset) +
      static_cast<std::uint64_t>(index) * sizeof(SharedDependencyEpochV1);
  return offset <= (std::numeric_limits<std::uint32_t>::max)()
             ? static_cast<std::uint32_t>(offset)
             : 0;
}

class PartitionLockGuard {
 public:
  PartitionLockGuard(const DependencyDirectoryCallbacksV1 &callbacks,
                     std::uint32_t partition_index) noexcept
      : callbacks_(callbacks), partition_index_(partition_index) {
    if (callbacks_.lock_mode ==
        DependencyDirectoryLockModeV1::AcquireRelease) {
      callbacks_.acquire_partition(callbacks_.context, partition_index_);
    }
  }

  PartitionLockGuard(const PartitionLockGuard &) = delete;
  PartitionLockGuard &operator=(const PartitionLockGuard &) = delete;

  ~PartitionLockGuard() {
    if (callbacks_.lock_mode ==
        DependencyDirectoryLockModeV1::AcquireRelease) {
      callbacks_.release_partition(callbacks_.context, partition_index_);
    }
  }

 private:
  const DependencyDirectoryCallbacksV1 &callbacks_;
  std::uint32_t partition_index_;
};

[[nodiscard]] inline DependencyDirectoryErrorV1 take_sequence(
    DependencyDirectoryPartitionV1 &partition_value,
    const DependencyDirectoryCallbacksV1 &callbacks,
    std::uint64_t *sequence) noexcept {
  if (sequence == nullptr || callbacks.next_sequence == nullptr) {
    return DependencyDirectoryErrorV1::SequenceUnavailable;
  }
  const std::uint64_t next = callbacks.next_sequence(callbacks.context);
  if (next == 0 || next <= partition_value.last_sequence) {
    return DependencyDirectoryErrorV1::SequenceUnavailable;
  }
  partition_value.last_sequence = next;
  *sequence = next;
  return DependencyDirectoryErrorV1::None;
}

inline void note_emergency_fallback(
    DependencyDirectoryPartitionV1 &partition_value) noexcept {
  saturating_increment(partition_value.emergency_fallback_count);
}

inline void latch_emergency(
    DependencyDirectoryPartitionV1 &partition_value,
    SharedDependencyEpochV1 *record = nullptr) noexcept {
  partition_value.emergency_latched = 1;
  partition_value.emergency_change_generation =
      (std::numeric_limits<std::uint64_t>::max)();
  note_emergency_fallback(partition_value);
  if (record != nullptr) {
    record->change_generation =
        (std::numeric_limits<std::uint64_t>::max)();
  }
}

[[nodiscard]] inline bool wake_partition(
    DependencyDirectoryPartitionV1 &partition_value,
    const DependencyDirectoryCallbacksV1 &callbacks,
    std::uint32_t partition_index) noexcept {
  if (callbacks.wake_partition != nullptr &&
      callbacks.wake_partition(callbacks.context, partition_index)) {
    return true;
  }
  saturating_increment(partition_value.wake_failure_count);
  return false;
}

/**
 * Strictly less than 70% is a safety backstop.  Valid layouts reserve at
 * least two physical slots per logical record, so the quotient/remainder
 * logical quota normally binds at no more than 50% first.
 */
[[nodiscard]] constexpr bool projected_below_physical_pressure(
    const DependencyDirectoryHeaderV1 &header_value,
    const DependencyDirectoryPartitionV1 &partition_value,
    std::uint32_t projected_occupied) noexcept {
  return static_cast<std::uint64_t>(projected_occupied) * 100U <
         static_cast<std::uint64_t>(partition_value.physical_capacity) *
             header_value.config.pressure_percent;
}

[[nodiscard]] constexpr bool can_admit_new_record(
    const DependencyDirectoryHeaderV1 &header_value,
    const DependencyDirectoryPartitionV1 &partition_value) noexcept {
  if (partition_value.occupied_count >= partition_value.logical_capacity ||
      partition_value.occupied_count ==
          (std::numeric_limits<std::uint32_t>::max)()) {
    return false;
  }
  return projected_below_physical_pressure(
      header_value, partition_value, partition_value.occupied_count + 1U);
}

[[nodiscard]] constexpr bool under_pressure(
    const DependencyDirectoryHeaderV1 &header_value,
    const DependencyDirectoryPartitionV1 &partition_value) noexcept {
  return !can_admit_new_record(header_value, partition_value);
}

[[nodiscard]] constexpr bool zero_current_references(
    const SharedDependencyEpochV1 &record) noexcept {
  return record.transition_count == 0 && record.generation_ref_count == 0 &&
         record.ticket_ref_count == 0 && record.owner_ref_count == 0;
}

[[nodiscard]] constexpr bool zero_references(
    const SharedDependencyEpochV1 &record) noexcept {
  return zero_current_references(record) &&
         !static_cast<bool>(record.retired_references);
}

inline void clear_retired_references_if_drained(
    SharedDependencyEpochV1 &record) noexcept {
  RetiredDependencyReferenceCohortV1 &retired = record.retired_references;
  if (retired && retired.generation_ref_count == 0 &&
      retired.ticket_ref_count == 0) {
    retired = {};
  }
}

[[nodiscard]] inline std::uint32_t *reference_counter(
    SharedDependencyEpochV1 &record,
    DependencyReferenceKindV1 kind) noexcept {
  switch (kind) {
    case DependencyReferenceKindV1::Owner:
      return &record.owner_ref_count;
    case DependencyReferenceKindV1::Ticket:
      return &record.ticket_ref_count;
    case DependencyReferenceKindV1::Generation:
      return &record.generation_ref_count;
    case DependencyReferenceKindV1::None:
      return nullptr;
  }
  return nullptr;
}

[[nodiscard]] inline std::uint32_t reference_limit(
    const DependencyDirectoryHeaderV1 &header_value,
    DependencyReferenceKindV1 kind) noexcept {
  switch (kind) {
    case DependencyReferenceKindV1::Owner:
    case DependencyReferenceKindV1::Ticket:
      return header_value.config.backend_reference_limit;
    case DependencyReferenceKindV1::Generation:
      return header_value.config.generation_reference_limit;
    case DependencyReferenceKindV1::None:
      return 0;
  }
  return 0;
}

[[nodiscard]] inline DependencyEpochSnapshotV1 snapshot_record(
    const SharedDependencyEpochV1 &record) noexcept {
  return DependencyEpochSnapshotV1{
      .key = record.key,
      .physical_identity = record.physical_identity,
      .record_generation = record.record_generation,
      .committed_epoch = record.committed_epoch,
      .change_generation = record.change_generation,
      .transition_count = record.transition_count,
      .generation_ref_count = record.generation_ref_count,
      .ticket_ref_count = record.ticket_ref_count,
      .owner_ref_count = record.owner_ref_count,
      .observability = record.observability,
  };
}

struct ProbeResultV1 {
  std::uint32_t matching_index = kInvalidDependencyPartitionV1;
  std::uint32_t insertion_index = kInvalidDependencyPartitionV1;
  bool corrupt = false;
};

[[nodiscard]] inline ProbeResultV1 probe_partition(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyDirectoryPartitionV1 &partition_value,
    const DependencyKey &key,
    std::uint64_t hash) noexcept {
  ProbeResultV1 result{};
  const std::uint32_t mask = partition_value.physical_capacity - 1U;
  const std::uint32_t start = static_cast<std::uint32_t>(
      (hash >> 32U) & static_cast<std::uint64_t>(mask));
  for (std::uint32_t probe = 0;
       probe < partition_value.physical_capacity; ++probe) {
    const std::uint32_t index = (start + probe) & mask;
    const SharedDependencyEpochV1 *record =
        record_at_index(directory, partition_value, index);
    if (record == nullptr) {
      result.corrupt = true;
      return result;
    }
    if (record->slot_state == DirectorySlotState::Occupied) {
      if (record->key == key) {
        result.matching_index = index;
        return result;
      }
      continue;
    }
    if (record->slot_state == DirectorySlotState::Tombstone) {
      if (result.insertion_index == kInvalidDependencyPartitionV1) {
        result.insertion_index = index;
      }
      continue;
    }
    if (record->slot_state == DirectorySlotState::Empty) {
      if (result.insertion_index == kInvalidDependencyPartitionV1) {
        result.insertion_index = index;
      }
      return result;
    }
    result.corrupt = true;
    return result;
  }
  return result;
}

[[nodiscard]] inline DependencyReclaimResultV1 reclaim_locked(
    DependencyDirectoryAttachmentV1 directory,
    DependencyDirectoryPartitionV1 &partition_value,
    std::uint32_t budget,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  DependencyReclaimResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr || partition_value.physical_capacity == 0) {
    result.error = DependencyDirectoryErrorV1::CorruptLayout;
    return result;
  }
  if (partition_value.emergency_latched != 0) {
    note_emergency_fallback(partition_value);
    result.error = DependencyDirectoryErrorV1::EmergencyFallback;
    result.coarse_fallback_required = true;
    result.pressure_remains = under_pressure(*header_value, partition_value);
    return result;
  }
  const std::uint32_t bounded_budget = std::min(
      std::min(budget, partition_value.physical_capacity),
      kMaximumDependencyVectorSizeV1);
  for (std::uint32_t scanned = 0; scanned < bounded_budget; ++scanned) {
    const std::uint32_t index = partition_value.reclaim_cursor &
                                (partition_value.physical_capacity - 1U);
    partition_value.reclaim_cursor =
        (index + 1U) & (partition_value.physical_capacity - 1U);
    ++result.scanned_count;
    SharedDependencyEpochV1 *record =
        record_at_index(directory, partition_value, index);
    if (record == nullptr) {
      result.error = DependencyDirectoryErrorV1::CorruptLayout;
      return result;
    }
    if (record->slot_state != DirectorySlotState::Occupied ||
        !zero_references(*record)) {
      continue;
    }
    if (partition_value.occupied_count == 0 ||
        partition_value.tombstone_count >=
            partition_value.physical_capacity) {
      latch_emergency(partition_value, record);
      result.error = partition_value.occupied_count == 0
                         ? DependencyDirectoryErrorV1::CounterUnderflow
                         : DependencyDirectoryErrorV1::CounterOverflow;
      result.coarse_fallback_required = true;
      return result;
    }
    saturating_increment(partition_value.reclaim_attempt_count);
    const SharedDependencyEpochV1 before = *record;
    record->slot_state = DirectorySlotState::Retiring;
    std::uint64_t change_sequence = 0;
    const DependencyDirectoryErrorV1 sequence_error = take_sequence(
        partition_value, callbacks, &change_sequence);
    if (sequence_error != DependencyDirectoryErrorV1::None) {
      *record = before;
      saturating_increment(
          partition_value.retiring_recheck_failure_count);
      note_emergency_fallback(partition_value);
      result.error = sequence_error;
      result.coarse_fallback_required = true;
      return result;
    }
    if (record->slot_state != DirectorySlotState::Retiring ||
        record->record_generation != before.record_generation ||
        record->key != before.key || !zero_references(*record)) {
      *record = before;
      saturating_increment(
          partition_value.retiring_recheck_failure_count);
      note_emergency_fallback(partition_value);
      result.error = DependencyDirectoryErrorV1::CorruptDirectory;
      result.coarse_fallback_required = true;
      return result;
    }
    const std::uint64_t previous_record_generation =
        record->record_generation;
    std::memset(record, 0, sizeof(*record));
    record->slot_state = DirectorySlotState::Tombstone;
    record->record_generation = previous_record_generation;
    record->change_generation = change_sequence;
    --partition_value.occupied_count;
    ++partition_value.tombstone_count;
    ++result.reclaimed_count;
    saturating_increment(partition_value.reclaimed_record_count);
  }
  result.pressure_remains = under_pressure(*header_value, partition_value);
  return result;
}

[[nodiscard]] inline DependencyDirectoryErrorV1 validate_reference_locked(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyDirectoryPartitionV1 &partition_value,
    const DependencyReferenceV1 &reference,
    SharedDependencyEpochV1 **record,
    bool require_physical_identity = true) noexcept {
  if (record == nullptr || !reference ||
      reference.partition_index == kInvalidDependencyPartitionV1 ||
      !valid_key(reference.key) ||
      reference.alignment_reserved != 0 ||
      !physical_identity_valid_v1(reference.physical_identity) ||
      !valid_reference_kind(reference.kind)) {
    return DependencyDirectoryErrorV1::InvalidArgument;
  }
  const std::uint64_t partition_begin = partition_value.record_offset;
  const std::uint64_t partition_bytes =
      static_cast<std::uint64_t>(partition_value.physical_capacity) *
      sizeof(SharedDependencyEpochV1);
  const std::uint64_t reference_offset = reference.record_offset;
  if (reference_offset < partition_begin ||
      reference_offset >= partition_begin + partition_bytes ||
      (reference_offset - partition_begin) %
              sizeof(SharedDependencyEpochV1) !=
          0) {
    return DependencyDirectoryErrorV1::StaleReference;
  }
  SharedDependencyEpochV1 *value =
      record_at_offset(directory, reference.record_offset);
  if (value == nullptr || value->slot_state != DirectorySlotState::Occupied ||
      value->record_generation != reference.record_generation ||
      value->key != reference.key) {
    return DependencyDirectoryErrorV1::StaleReference;
  }
  if (require_physical_identity &&
      value->physical_identity != reference.physical_identity) {
    return DependencyDirectoryErrorV1::PhysicalIdentityMismatch;
  }
  *record = value;
  return DependencyDirectoryErrorV1::None;
}

[[nodiscard]] constexpr bool zero_dependency_key_storage(
    const DependencyKey &key) noexcept {
  return key.database_oid == 0 && key.class_oid == 0 &&
         key.object_oid == 0 && key.object_sub_id == 0 &&
         static_cast<std::uint16_t>(key.kind) == 0 && key.reserved == 0;
}

[[nodiscard]] inline bool empty_record_is_zero(
    const SharedDependencyEpochV1 &record) noexcept {
  return record.reserved == 0 && zero_dependency_key_storage(record.key) &&
         record.alignment_reserved == 0 &&
         record.physical_identity == PhysicalIdentityV1{} &&
         record.record_generation == 0 && record.committed_epoch == 0 &&
         record.change_generation == 0 && zero_references(record) &&
         record.observability == WriteObservability::None &&
         record.reserved2 == 0 &&
         record.retired_references ==
             RetiredDependencyReferenceCohortV1{};
}

[[nodiscard]] inline DependencyDirectoryErrorV1 validate_partition_locked(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyDirectoryHeaderV1 &header_value,
    std::uint32_t partition_index) noexcept {
  const DependencyDirectoryPartitionV1 *partition_value =
      partition(directory, partition_index);
  const std::uint64_t expected_offset =
      static_cast<std::uint64_t>(header_value.layout.records_offset) +
      static_cast<std::uint64_t>(partition_index) *
          header_value.layout.partition_capacity *
          sizeof(SharedDependencyEpochV1);
  const std::uint32_t expected_logical_capacity =
      logical_partition_capacity(
          header_value.config.logical_capacity,
          header_value.config.partition_count, partition_index);
  if (partition_value == nullptr ||
      expected_offset > (std::numeric_limits<std::uint32_t>::max)() ||
      partition_value->record_offset != expected_offset ||
      partition_value->physical_capacity !=
          header_value.layout.partition_capacity ||
      partition_value->logical_capacity != expected_logical_capacity ||
      partition_value->logical_capacity == 0 ||
      partition_value->reserved != 0 ||
      partition_value->reclaim_cursor >=
          partition_value->physical_capacity ||
      partition_value->occupied_count > partition_value->logical_capacity ||
      partition_value->tombstone_count >
          partition_value->physical_capacity ||
      static_cast<std::uint64_t>(partition_value->occupied_count) +
              partition_value->tombstone_count >
          partition_value->physical_capacity ||
      partition_value->emergency_latched > 1 ||
      (partition_value->emergency_latched == 0 &&
       partition_value->emergency_change_generation != 0) ||
      (partition_value->emergency_latched == 1 &&
       partition_value->emergency_change_generation !=
           (std::numeric_limits<std::uint64_t>::max)()) ||
      !projected_below_physical_pressure(
          header_value, *partition_value,
          partition_value->logical_capacity)) {
    return DependencyDirectoryErrorV1::CorruptLayout;
  }

  std::uint32_t occupied = 0;
  std::uint32_t tombstones = 0;
  for (std::uint32_t index = 0;
       index < partition_value->physical_capacity; ++index) {
    const SharedDependencyEpochV1 *record =
        record_at_index(directory, *partition_value, index);
    if (record == nullptr) {
      return DependencyDirectoryErrorV1::CorruptLayout;
    }
    if (record->slot_state == DirectorySlotState::Empty) {
      if (!empty_record_is_zero(*record)) {
        return DependencyDirectoryErrorV1::CorruptDirectory;
      }
      continue;
    }
    if (record->slot_state == DirectorySlotState::Tombstone) {
      ++tombstones;
      if (record->reserved != 0 ||
          !zero_dependency_key_storage(record->key) ||
          record->alignment_reserved != 0 ||
          record->physical_identity != PhysicalIdentityV1{} ||
          record->record_generation == 0 || record->committed_epoch != 0 ||
          record->change_generation == 0 || !zero_references(*record) ||
          record->observability != WriteObservability::None ||
          record->reserved2 != 0 ||
          record->retired_references !=
              RetiredDependencyReferenceCohortV1{}) {
        return DependencyDirectoryErrorV1::CorruptDirectory;
      }
      continue;
    }
    if (record->slot_state != DirectorySlotState::Occupied ||
        record->reserved != 0 || !valid_key(record->key) ||
        record->alignment_reserved != 0 ||
        !physical_identity_valid_v1(record->physical_identity) ||
        record->record_generation == 0 || record->committed_epoch == 0 ||
        record->change_generation == 0 ||
        !valid_observability(record->observability) ||
        record->reserved2 != 0 ||
        record->transition_count >
            header_value.config.backend_reference_limit ||
        record->generation_ref_count >
            header_value.config.generation_reference_limit ||
        record->ticket_ref_count >
            header_value.config.backend_reference_limit ||
        record->owner_ref_count >
            header_value.config.backend_reference_limit ||
        record->retired_references.generation_ref_count >
            header_value.config.generation_reference_limit ||
        record->retired_references.ticket_ref_count >
            header_value.config.backend_reference_limit ||
        record->transition_count > record->owner_ref_count ||
        (dependency_key_hash(record->key) &
         (header_value.config.partition_count - 1U)) != partition_index) {
      return DependencyDirectoryErrorV1::CorruptDirectory;
    }
    const RetiredDependencyReferenceCohortV1 &retired =
        record->retired_references;
    const bool retired_valid = !retired
        ? retired == RetiredDependencyReferenceCohortV1{}
        : physical_identity_valid_v1(retired.physical_identity) &&
              retired.record_generation != record->record_generation &&
              retired.committed_epoch != 0 &&
              (retired.generation_ref_count != 0 ||
               retired.ticket_ref_count != 0) &&
              valid_observability(retired.observability) &&
              retired.observability == record->observability &&
              retired.reserved == 0;
    if (!retired_valid) {
      return DependencyDirectoryErrorV1::CorruptDirectory;
    }
    ++occupied;
  }
  if (occupied != partition_value->occupied_count ||
      tombstones != partition_value->tombstone_count) {
    return DependencyDirectoryErrorV1::CorruptDirectory;
  }
  return DependencyDirectoryErrorV1::None;
}

[[nodiscard]] inline DependencyDirectoryErrorV1 structural_validate(
    std::span<std::byte> region,
    DependencyDirectoryHeaderV1 **output_header) noexcept {
  if (output_header == nullptr) {
    return DependencyDirectoryErrorV1::InvalidArgument;
  }
  if (region.data() == nullptr ||
      reinterpret_cast<std::uintptr_t>(region.data()) %
              kDependencyDirectoryRegionAlignmentV1 !=
          0) {
    return DependencyDirectoryErrorV1::RegionMisaligned;
  }
  DependencyDirectoryHeaderV1 *header_value =
      object_at<DependencyDirectoryHeaderV1>(region, 0);
  if (header_value == nullptr) {
    return DependencyDirectoryErrorV1::RegionTooSmall;
  }
  if (header_value->magic != kDependencyDirectoryMagicV1 ||
      header_value->bootstrap_state != BootstrapState::Ready) {
    return DependencyDirectoryErrorV1::NotInitialized;
  }
  if (header_value->abi_version != kDependencyDirectoryAbiVersionV1) {
    return DependencyDirectoryErrorV1::AbiMismatch;
  }
  if (header_value->reserved != 0) {
    return DependencyDirectoryErrorV1::CorruptLayout;
  }
  const DependencyDirectoryPreloadResultV1 expected =
      dependency_directory_preload_request(header_value->config);
  const DependencyDirectoryLayoutV1 &layout = header_value->layout;
  if (!expected || layout != expected.request.layout) {
    return DependencyDirectoryErrorV1::CorruptLayout;
  }
  if (region.size() != layout.total_bytes) {
    return DependencyDirectoryErrorV1::RegionSizeMismatch;
  }
  *output_header = header_value;
  return DependencyDirectoryErrorV1::None;
}

}  // namespace dependency_directory_detail

/** Compute one exact layout; invalid power-of-two slices are never rounded. */
[[nodiscard]] inline DependencyDirectoryPreloadResultV1
dependency_directory_preload_request(
    DependencyDirectoryConfigV1 config) noexcept {
  using namespace dependency_directory_detail;
  DependencyDirectoryPreloadResultV1 result{};
  const std::uint32_t physical_capacity =
      dependency_directory_physical_capacity(config.logical_capacity);
  if (physical_capacity == 0 ||
      !power_of_two(config.partition_count) ||
      config.partition_count > config.logical_capacity ||
      config.partition_count > kMaximumDependencyDirectoryPartitionsV1 ||
      config.reclaim_scan_limit == 0 ||
      config.reclaim_scan_limit > kMaximumDependencyVectorSizeV1 ||
      config.pressure_percent != kDependencyDirectoryPressurePercentV1 ||
      config.backend_reference_limit == 0 ||
      config.backend_reference_limit > kMaximumDependencyReferenceCountV1 ||
      config.generation_reference_limit == 0 ||
      config.generation_reference_limit >
          kMaximumDependencyReferenceCountV1 ||
      config.reserved != 0) {
    result.error = DependencyDirectoryErrorV1::InvalidConfiguration;
    return result;
  }
  const std::uint32_t partition_capacity =
      physical_capacity / config.partition_count;
  if (!power_of_two(partition_capacity)) {
    result.error = DependencyDirectoryErrorV1::InvalidConfiguration;
    return result;
  }
  std::uint64_t cursor = sizeof(DependencyDirectoryHeaderV1);
  cursor = align_up(cursor, alignof(DependencyDirectoryPartitionV1));
  const std::uint64_t partitions_offset = cursor;
  cursor += static_cast<std::uint64_t>(config.partition_count) *
            sizeof(DependencyDirectoryPartitionV1);
  cursor = align_up(cursor, alignof(SharedDependencyEpochV1));
  const std::uint64_t records_offset = cursor;
  cursor += static_cast<std::uint64_t>(physical_capacity) *
            sizeof(SharedDependencyEpochV1);
  cursor = align_up(cursor, kDependencyDirectoryRegionAlignmentV1);
  if (partitions_offset == 0 || records_offset == 0 || cursor == 0 ||
      cursor > (std::numeric_limits<std::uint32_t>::max)()) {
    result.error = DependencyDirectoryErrorV1::InvalidConfiguration;
    return result;
  }
  result.request = DependencyDirectoryPreloadRequestV1{
      .config = config,
      .layout = DependencyDirectoryLayoutV1{
          .total_bytes = static_cast<std::uint32_t>(cursor),
          .partitions_offset =
              static_cast<std::uint32_t>(partitions_offset),
          .records_offset = static_cast<std::uint32_t>(records_offset),
          .logical_capacity = config.logical_capacity,
          .physical_capacity = physical_capacity,
          .partition_capacity = partition_capacity,
      },
      .partition_lock_count = config.partition_count,
  };
  return result;
}

/** Initialize one new region before any backend attaches it. */
[[nodiscard]] inline DependencyDirectoryErrorV1
initialize_dependency_directory(
    std::span<std::byte> region,
    const DependencyDirectoryPreloadRequestV1 &request) noexcept {
  using namespace dependency_directory_detail;
  const DependencyDirectoryPreloadResultV1 expected =
      dependency_directory_preload_request(request.config);
  if (!expected || request.reserved != 0 ||
      request.partition_lock_count != request.config.partition_count ||
      request.layout != expected.request.layout) {
    return DependencyDirectoryErrorV1::InvalidConfiguration;
  }
  if (region.size() < request.layout.total_bytes) {
    return DependencyDirectoryErrorV1::RegionTooSmall;
  }
  if (region.size() != request.layout.total_bytes) {
    return DependencyDirectoryErrorV1::RegionSizeMismatch;
  }
  if (region.data() == nullptr ||
      reinterpret_cast<std::uintptr_t>(region.data()) %
              kDependencyDirectoryRegionAlignmentV1 !=
          0) {
    return DependencyDirectoryErrorV1::RegionMisaligned;
  }
  std::memset(region.data(), 0, request.layout.total_bytes);
  DependencyDirectoryHeaderV1 *header_value =
      object_at<DependencyDirectoryHeaderV1>(region, 0);
  if (header_value == nullptr) {
    return DependencyDirectoryErrorV1::RegionMisaligned;
  }
  header_value->magic = kDependencyDirectoryMagicV1;
  header_value->abi_version = kDependencyDirectoryAbiVersionV1;
  header_value->bootstrap_state = BootstrapState::Initializing;
  header_value->config = request.config;
  header_value->layout = request.layout;
  DependencyDirectoryAttachmentV1 directory{.region = region};
  std::uint64_t logical_capacity_sum = 0;
  for (std::uint32_t index = 0; index < request.config.partition_count;
       ++index) {
    DependencyDirectoryPartitionV1 *partition_value =
        dependency_directory_detail::partition(directory, index);
    if (partition_value == nullptr) {
      header_value->bootstrap_state = BootstrapState::Failed;
      return DependencyDirectoryErrorV1::CorruptLayout;
    }
    const std::uint64_t record_offset_value =
        static_cast<std::uint64_t>(request.layout.records_offset) +
        static_cast<std::uint64_t>(index) *
            request.layout.partition_capacity *
            sizeof(SharedDependencyEpochV1);
    if (record_offset_value >
        (std::numeric_limits<std::uint32_t>::max)()) {
      header_value->bootstrap_state = BootstrapState::Failed;
      return DependencyDirectoryErrorV1::CorruptLayout;
    }
    const std::uint32_t partition_logical_capacity =
        logical_partition_capacity(
            request.config.logical_capacity,
            request.config.partition_count, index);
    if (partition_logical_capacity == 0 ||
        !projected_below_physical_pressure(
            *header_value,
            DependencyDirectoryPartitionV1{
                .physical_capacity = request.layout.partition_capacity,
                .logical_capacity = partition_logical_capacity,
            },
            partition_logical_capacity)) {
      header_value->bootstrap_state = BootstrapState::Failed;
      return DependencyDirectoryErrorV1::InvalidConfiguration;
    }
    *partition_value = DependencyDirectoryPartitionV1{
        .record_offset = static_cast<std::uint32_t>(record_offset_value),
        .physical_capacity = request.layout.partition_capacity,
        .logical_capacity = partition_logical_capacity,
    };
    logical_capacity_sum += partition_logical_capacity;
  }
  if (logical_capacity_sum != request.config.logical_capacity) {
    header_value->bootstrap_state = BootstrapState::Failed;
    return DependencyDirectoryErrorV1::CorruptLayout;
  }
  header_value->bootstrap_state = BootstrapState::Ready;
  return DependencyDirectoryErrorV1::None;
}

struct DependencyDirectoryAttachResultV1 {
  DependencyDirectoryAttachmentV1 directory{};
  DependencyDirectoryErrorV1 error = DependencyDirectoryErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyDirectoryErrorV1::None &&
           static_cast<bool>(directory);
  }
};

/**
 * Attach only after exact layout validation and one locked deep scan per
 * partition.  AlreadyLocked means the caller holds every partition lock for
 * the duration of this call; tests must opt into it explicitly.
 */
[[nodiscard]] inline DependencyDirectoryAttachResultV1
attach_dependency_directory(
    std::span<std::byte> region,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyDirectoryAttachResultV1 result{};
  if (!callbacks.locks_valid()) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  DependencyDirectoryHeaderV1 *header_value = nullptr;
  result.error = structural_validate(region, &header_value);
  if (result.error != DependencyDirectoryErrorV1::None) {
    return result;
  }
  DependencyDirectoryAttachmentV1 directory{.region = region};
  std::uint64_t logical_capacity_sum = 0;
  for (std::uint32_t index = 0;
       index < header_value->config.partition_count; ++index) {
    PartitionLockGuard lock(callbacks, index);
    const DependencyDirectoryPartitionV1 *partition_value =
        dependency_directory_detail::partition(directory, index);
    result.error = validate_partition_locked(directory, *header_value, index);
    if (result.error != DependencyDirectoryErrorV1::None) {
      return result;
    }
    logical_capacity_sum += partition_value->logical_capacity;
  }
  if (logical_capacity_sum != header_value->config.logical_capacity) {
    result.error = DependencyDirectoryErrorV1::CorruptLayout;
    return result;
  }
  result.directory = directory;
  return result;
}

/** Retain one full-equality record, reclaiming only bounded zero-ref records. */
[[nodiscard]] inline DependencyReferenceResultV1
acquire_dependency_reference(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyReferenceRequestV1 &request,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyReferenceResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.mutation_valid() || !valid_key(request.key) ||
      !physical_identity_valid_v1(request.physical_identity) ||
      !valid_observability(request.observability) ||
      !valid_reference_kind(request.kind) ||
      std::any_of(request.reserved.begin(), request.reserved.end(),
                  [](std::uint8_t value) noexcept { return value != 0; })) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  const std::uint64_t hash = dependency_key_hash(request.key);
  const std::uint32_t partition_index = static_cast<std::uint32_t>(
      hash & (header_value->config.partition_count - 1U));
  PartitionLockGuard lock(callbacks, partition_index);
  DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(directory, partition_index);
  if (partition_value == nullptr) {
    result.error = DependencyDirectoryErrorV1::CorruptLayout;
    return result;
  }
  if (partition_value->emergency_latched != 0) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::EmergencyFallback;
    result.coarse_fallback_required = true;
    result.outcome = DependencyAdmissionOutcomeV1::PressureFallback;
    return result;
  }
  ProbeResultV1 probe = probe_partition(
      directory, *partition_value, request.key, hash);
  if (probe.corrupt) {
    latch_emergency(*partition_value);
    result.error = DependencyDirectoryErrorV1::CorruptDirectory;
    result.coarse_fallback_required = true;
    return result;
  }
  if (probe.matching_index == kInvalidDependencyPartitionV1 &&
      !can_admit_new_record(*header_value, *partition_value)) {
    result.pressure_observed = true;
    saturating_increment(partition_value->pressure_event_count);
    const DependencyReclaimResultV1 reclaimed = reclaim_locked(
        directory, *partition_value,
        header_value->config.reclaim_scan_limit, callbacks);
    result.reclaimed_count = reclaimed.reclaimed_count;
    if (reclaimed.error != DependencyDirectoryErrorV1::None) {
      result.error = reclaimed.error;
      result.coarse_fallback_required =
          reclaimed.coarse_fallback_required;
      return result;
    }
    probe = probe_partition(directory, *partition_value, request.key, hash);
    if (probe.corrupt) {
      latch_emergency(*partition_value);
      result.error = DependencyDirectoryErrorV1::CorruptDirectory;
      result.coarse_fallback_required = true;
      return result;
    }
    if (probe.matching_index == kInvalidDependencyPartitionV1 &&
        !can_admit_new_record(*header_value, *partition_value)) {
      saturating_increment(partition_value->pressure_fallback_count);
      note_emergency_fallback(*partition_value);
      result.error = DependencyDirectoryErrorV1::PressureLimit;
      result.coarse_fallback_required = true;
      result.outcome = DependencyAdmissionOutcomeV1::PressureFallback;
      return result;
    }
  }
  SharedDependencyEpochV1 *record = nullptr;
  std::uint32_t index = probe.matching_index;
  if (index != kInvalidDependencyPartitionV1) {
    record = record_at_index(directory, *partition_value, index);
    if (record == nullptr) {
      result.error = DependencyDirectoryErrorV1::CorruptLayout;
      return result;
    }
    if (record->observability != request.observability) {
      note_emergency_fallback(*partition_value);
      result.error = DependencyDirectoryErrorV1::ObservabilityMismatch;
      result.coarse_fallback_required = true;
      return result;
    }
    if (record->physical_identity != request.physical_identity) {
      note_emergency_fallback(*partition_value);
      result.error = DependencyDirectoryErrorV1::PhysicalIdentityMismatch;
      result.coarse_fallback_required = true;
      return result;
    }
    result.outcome = DependencyAdmissionOutcomeV1::Existing;
  } else {
    index = probe.insertion_index;
    if (index == kInvalidDependencyPartitionV1) {
      saturating_increment(partition_value->directory_full_fallback_count);
      note_emergency_fallback(*partition_value);
      result.error = DependencyDirectoryErrorV1::DirectoryFull;
      result.coarse_fallback_required = true;
      result.outcome = DependencyAdmissionOutcomeV1::PressureFallback;
      return result;
    }
    if (!can_admit_new_record(*header_value, *partition_value)) {
      saturating_increment(partition_value->pressure_fallback_count);
      note_emergency_fallback(*partition_value);
      result.error = DependencyDirectoryErrorV1::PressureLimit;
      result.coarse_fallback_required = true;
      result.outcome = DependencyAdmissionOutcomeV1::PressureFallback;
      return result;
    }
    record = record_at_index(directory, *partition_value, index);
    if (record == nullptr ||
        (record->slot_state != DirectorySlotState::Empty &&
         record->slot_state != DirectorySlotState::Tombstone)) {
      result.error = DependencyDirectoryErrorV1::CorruptDirectory;
      return result;
    }
    std::uint64_t record_sequence = 0;
    std::uint64_t change_sequence = 0;
    result.error = take_sequence(
        *partition_value, callbacks, &record_sequence);
    if (result.error == DependencyDirectoryErrorV1::None) {
      result.error = take_sequence(
          *partition_value, callbacks, &change_sequence);
    }
    if (result.error != DependencyDirectoryErrorV1::None) {
      note_emergency_fallback(*partition_value);
      result.coarse_fallback_required = true;
      return result;
    }
    const bool reused_tombstone =
        record->slot_state == DirectorySlotState::Tombstone;
    if (reused_tombstone && partition_value->tombstone_count == 0) {
      latch_emergency(*partition_value);
      result.error = DependencyDirectoryErrorV1::CounterUnderflow;
      result.coarse_fallback_required = true;
      return result;
    }
    *record = SharedDependencyEpochV1{
        .slot_state = DirectorySlotState::Occupied,
        .key = request.key,
        .physical_identity = request.physical_identity,
        .record_generation = record_sequence,
        .committed_epoch = 1,
        .change_generation = change_sequence,
        .observability = request.observability,
    };
    ++partition_value->occupied_count;
    if (reused_tombstone) {
      --partition_value->tombstone_count;
    }
    result.outcome = DependencyAdmissionOutcomeV1::Inserted;
  }
  std::uint32_t *counter = reference_counter(*record, request.kind);
  if (counter == nullptr) {
    result.error = DependencyDirectoryErrorV1::InvalidReferenceKind;
    return result;
  }
  const std::uint32_t limit = reference_limit(*header_value, request.kind);
  if (limit == 0 || *counter >= limit) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::CounterOverflow;
    result.coarse_fallback_required = true;
    return result;
  }
  ++(*counter);
  result.reference = DependencyReferenceV1{
      .key = request.key,
      .record_offset = record_offset(*partition_value, index),
      .partition_index = partition_index,
      .physical_identity = record->physical_identity,
      .record_generation = record->record_generation,
      .kind = request.kind,
      .active = true,
  };
  result.snapshot = snapshot_record(*record);
  return result;
}

/** Snapshot by an ABA token while retaining the caller's existing ref. */
[[nodiscard]] inline DependencySnapshotResultV1 snapshot_dependency(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyReferenceV1 &reference,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencySnapshotResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.locks_valid() || !reference ||
      reference.partition_index >= header_value->config.partition_count) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  PartitionLockGuard lock(callbacks, reference.partition_index);
  const DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(
          directory, reference.partition_index);
  if (partition_value != nullptr && partition_value->emergency_latched != 0) {
    result.error = DependencyDirectoryErrorV1::EmergencyFallback;
    result.coarse_fallback_required = true;
    return result;
  }
  SharedDependencyEpochV1 *record = nullptr;
  result.error = partition_value == nullptr
                     ? DependencyDirectoryErrorV1::CorruptLayout
                     : validate_reference_locked(
                           directory, *partition_value, reference, &record);
  if (result.error == DependencyDirectoryErrorV1::None) {
    result.snapshot = snapshot_record(*record);
  }
  return result;
}

/** Release exactly one reference kind; pressure may reclaim the now-zero record. */
[[nodiscard]] inline DependencyMutationResultV1 release_dependency_reference(
    DependencyDirectoryAttachmentV1 directory,
    DependencyReferenceV1 &reference,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyMutationResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.mutation_valid() || !reference ||
      reference.partition_index >= header_value->config.partition_count) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  const std::uint32_t partition_index = reference.partition_index;
  PartitionLockGuard lock(callbacks, partition_index);
  DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(directory, partition_index);
  SharedDependencyEpochV1 *record = nullptr;
  bool retired_reference = false;
  result.error = partition_value == nullptr
                     ? DependencyDirectoryErrorV1::CorruptLayout
                     : validate_reference_locked(
                           directory, *partition_value, reference, &record,
                           false);
  const std::uint64_t partition_begin = partition_value == nullptr
      ? 0
      : partition_value->record_offset;
  const std::uint64_t partition_bytes = partition_value == nullptr
      ? 0
      : static_cast<std::uint64_t>(partition_value->physical_capacity) *
          sizeof(SharedDependencyEpochV1);
  const std::uint64_t reference_offset = reference.record_offset;
  const bool offset_in_locked_partition = partition_value != nullptr &&
      reference_offset >= partition_begin &&
      reference_offset < partition_begin + partition_bytes &&
      (reference_offset - partition_begin) %
              sizeof(SharedDependencyEpochV1) ==
          0;
  if (result.error == DependencyDirectoryErrorV1::StaleReference &&
      offset_in_locked_partition) {
    record = record_at_offset(directory, reference.record_offset);
    const RetiredDependencyReferenceCohortV1 *retired =
        record == nullptr ? nullptr : &record->retired_references;
    if (record != nullptr &&
        record->slot_state == DirectorySlotState::Occupied &&
        record->key == reference.key && retired != nullptr && *retired &&
        retired->record_generation == reference.record_generation &&
        retired->physical_identity == reference.physical_identity &&
        (reference.kind == DependencyReferenceKindV1::Ticket ||
         reference.kind == DependencyReferenceKindV1::Generation) &&
        !reference.transition_active) {
      result.error = DependencyDirectoryErrorV1::None;
      retired_reference = true;
    }
  }
  if (result.error != DependencyDirectoryErrorV1::None) {
    return result;
  }
  if (!retired_reference &&
      record->physical_identity != reference.physical_identity) {
    result.error = DependencyDirectoryErrorV1::PhysicalIdentityMismatch;
    return result;
  }
  if (reference.kind == DependencyReferenceKindV1::Owner &&
      (reference.transition_active ||
       record->transition_count >= record->owner_ref_count)) {
    result.error = DependencyDirectoryErrorV1::InvalidState;
    result.coarse_fallback_required = true;
    return result;
  }
  std::uint32_t *counter = nullptr;
  if (retired_reference) {
    counter = reference.kind == DependencyReferenceKindV1::Ticket
        ? &record->retired_references.ticket_ref_count
        : &record->retired_references.generation_ref_count;
  } else {
    counter = reference_counter(*record, reference.kind);
  }
  if (counter == nullptr || *counter == 0) {
    latch_emergency(*partition_value, record);
    result.error = DependencyDirectoryErrorV1::CounterUnderflow;
    result.coarse_fallback_required = true;
    return result;
  }
  --(*counter);
  reference.active = false;
  clear_retired_references_if_drained(*record);
  result.applied = true;
  result.diagnostic_success = true;
  result.snapshot = snapshot_record(*record);
  if (partition_value->emergency_latched != 0) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::EmergencyFallback;
    result.diagnostic_success = false;
    result.coarse_fallback_required = true;
    return result;
  }
  if (under_pressure(*header_value, *partition_value) &&
      zero_references(*record)) {
    const DependencyReclaimResultV1 reclaimed = reclaim_locked(
        directory, *partition_value,
        header_value->config.reclaim_scan_limit, callbacks);
    result.reclaimed_count = reclaimed.reclaimed_count;
    if (reclaimed.error != DependencyDirectoryErrorV1::None) {
      result.error = reclaimed.error;
      result.diagnostic_success = false;
      result.coarse_fallback_required = true;
    }
  }
  return result;
}

/** Release one persisted generation capability by full key and ABA token. */
[[nodiscard]] inline DependencyMutationResultV1
release_dependency_generation_entry(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyVectorEntryV1 &entry,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyMutationResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.mutation_valid() ||
      !dependency_vector_entry_valid_v1(entry)) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  const std::uint64_t hash = dependency_key_hash(entry.version.key);
  const std::uint32_t partition_index = static_cast<std::uint32_t>(
      hash & (header_value->config.partition_count - 1U));
  PartitionLockGuard lock(callbacks, partition_index);
  DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(directory, partition_index);
  if (partition_value == nullptr) {
    result.error = DependencyDirectoryErrorV1::CorruptLayout;
    return result;
  }
  const ProbeResultV1 probe = probe_partition(
      directory, *partition_value, entry.version.key, hash);
  if (probe.corrupt) {
    latch_emergency(*partition_value);
    result.error = DependencyDirectoryErrorV1::CorruptDirectory;
    result.coarse_fallback_required = true;
    return result;
  }
  if (probe.matching_index == kInvalidDependencyPartitionV1) {
    latch_emergency(*partition_value);
    result.error = DependencyDirectoryErrorV1::StaleReference;
    result.coarse_fallback_required = true;
    return result;
  }
  SharedDependencyEpochV1 *record = record_at_index(
      directory, *partition_value, probe.matching_index);
  const bool current_generation = record != nullptr &&
      record->record_generation == entry.record_generation;
  const bool retired_generation = record != nullptr &&
      record->retired_references &&
      record->retired_references.record_generation == entry.record_generation;
  const bool current_identity = current_generation &&
      record->observability == entry.version.observability &&
      record->physical_identity == entry.version.physical_identity;
  const bool retired_identity = retired_generation &&
      record->retired_references.observability ==
          entry.version.observability &&
      record->retired_references.physical_identity ==
          entry.version.physical_identity;
  if (record == nullptr || (!current_identity && !retired_identity)) {
    latch_emergency(*partition_value, record);
    result.error = (current_generation || retired_generation)
        ? DependencyDirectoryErrorV1::PhysicalIdentityMismatch
        : DependencyDirectoryErrorV1::StaleReference;
    result.coarse_fallback_required = true;
    return result;
  }
  std::uint32_t &generation_ref_count = retired_identity
      ? record->retired_references.generation_ref_count
      : record->generation_ref_count;
  if (generation_ref_count == 0) {
    latch_emergency(*partition_value, record);
    result.error = DependencyDirectoryErrorV1::CounterUnderflow;
    result.coarse_fallback_required = true;
    return result;
  }
  --generation_ref_count;
  clear_retired_references_if_drained(*record);
  result.applied = true;
  result.diagnostic_success = true;
  result.snapshot = snapshot_record(*record);
  if (partition_value->emergency_latched != 0) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::EmergencyFallback;
    result.diagnostic_success = false;
    result.coarse_fallback_required = true;
    return result;
  }
  if (under_pressure(*header_value, *partition_value) &&
      zero_references(*record)) {
    const DependencyReclaimResultV1 reclaimed = reclaim_locked(
        directory, *partition_value,
        header_value->config.reclaim_scan_limit, callbacks);
    result.reclaimed_count = reclaimed.reclaimed_count;
    if (reclaimed.error != DependencyDirectoryErrorV1::None) {
      result.error = reclaimed.error;
      result.diagnostic_success = false;
      result.coarse_fallback_required = true;
    }
  }
  return result;
}

/** Mark one owner-retained dependency unstable before a write can commit. */
[[nodiscard]] inline DependencyMutationResultV1 begin_dependency_transition(
    DependencyDirectoryAttachmentV1 directory,
    DependencyReferenceV1 &owner_reference,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyMutationResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.mutation_valid() ||
      !owner_reference ||
      owner_reference.kind != DependencyReferenceKindV1::Owner ||
      owner_reference.transition_active ||
      owner_reference.partition_index >= header_value->config.partition_count) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  PartitionLockGuard lock(callbacks, owner_reference.partition_index);
  DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(
          directory, owner_reference.partition_index);
  SharedDependencyEpochV1 *record = nullptr;
  result.error = partition_value == nullptr
                     ? DependencyDirectoryErrorV1::CorruptLayout
                     : validate_reference_locked(
                           directory, *partition_value, owner_reference,
                           &record);
  if (result.error != DependencyDirectoryErrorV1::None) {
    return result;
  }
  if (partition_value->emergency_latched != 0) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::EmergencyFallback;
    result.coarse_fallback_required = true;
    return result;
  }
  if (record->owner_ref_count == 0) {
    latch_emergency(*partition_value, record);
    result.error = DependencyDirectoryErrorV1::CounterUnderflow;
    result.coarse_fallback_required = true;
    return result;
  }
  if (record->transition_count >= record->owner_ref_count) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::InvalidState;
    result.coarse_fallback_required = true;
    return result;
  }
  if (record->transition_count >=
      header_value->config.backend_reference_limit) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::CounterOverflow;
    result.coarse_fallback_required = true;
    return result;
  }
  std::uint64_t sequence = 0;
  result.error = take_sequence(*partition_value, callbacks, &sequence);
  if (result.error != DependencyDirectoryErrorV1::None) {
    note_emergency_fallback(*partition_value);
    result.coarse_fallback_required = true;
    return result;
  }
  ++record->transition_count;
  record->change_generation = sequence;
  owner_reference.transition_active = true;
  result.applied = true;
  result.diagnostic_success = true;
  result.snapshot = snapshot_record(*record);
  return result;
}

/** Resolve one transition with epoch, decrement, generation, then wake order. */
[[nodiscard]] inline DependencyMutationResultV1 finish_dependency_transition(
    DependencyDirectoryAttachmentV1 directory,
    DependencyReferenceV1 &owner_reference,
    const DependencyTransitionResolutionV1 &resolution,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyMutationResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.terminal_mutation_valid() ||
      !owner_reference ||
      owner_reference.kind != DependencyReferenceKindV1::Owner ||
      !owner_reference.transition_active ||
      !valid_transition_resolution(resolution) ||
      owner_reference.partition_index >= header_value->config.partition_count) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  PartitionLockGuard lock(callbacks, owner_reference.partition_index);
  DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(
          directory, owner_reference.partition_index);
  SharedDependencyEpochV1 *record = nullptr;
  result.error = partition_value == nullptr
                     ? DependencyDirectoryErrorV1::CorruptLayout
                     : validate_reference_locked(
                           directory, *partition_value, owner_reference,
                           &record);
  if (result.error != DependencyDirectoryErrorV1::None) {
    return result;
  }
  if (record->transition_count == 0 || record->owner_ref_count == 0) {
    latch_emergency(*partition_value, record);
    result.error = DependencyDirectoryErrorV1::CounterUnderflow;
    result.coarse_fallback_required = true;
    return result;
  }
  if (resolution.physical_identity_action ==
          DependencyPhysicalIdentityActionV1::Preserve &&
      resolution.physical_identity != record->physical_identity) {
    result.error = DependencyDirectoryErrorV1::PhysicalIdentityMismatch;
    result.coarse_fallback_required = true;
    return result;
  }

  /*
   * Reserve the nonzero change generation before the critical publication
   * sequence.  Aborted is deliberately conservative; KnownAborted is the
   * narrow proof-carrying outcome that drains without advancing the epoch.
   */
  const bool publish_epoch = resolution.outcome !=
      DependencyTransitionOutcomeV1::KnownAborted;
  const bool epoch_overflow = publish_epoch && record->committed_epoch ==
      (std::numeric_limits<std::uint64_t>::max)();
  const bool emergency_before = partition_value->emergency_latched != 0;
  const bool replacing = resolution.physical_identity_action ==
      DependencyPhysicalIdentityActionV1::Replace;
  if (replacing && record->retired_references) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::PressureLimit;
    result.coarse_fallback_required = true;
    return result;
  }
  if (replacing &&
      (record->owner_ref_count != 1 || record->transition_count != 1)) {
    note_emergency_fallback(*partition_value);
    result.error = DependencyDirectoryErrorV1::InvalidState;
    result.coarse_fallback_required = true;
    return result;
  }
  std::uint64_t record_sequence = record->record_generation;
  std::uint64_t change_sequence = 0;
  DependencyDirectoryErrorV1 sequence_error =
      !epoch_overflow && !emergency_before
      ? DependencyDirectoryErrorV1::None
      : DependencyDirectoryErrorV1::EmergencyFallback;
  if (sequence_error == DependencyDirectoryErrorV1::None && replacing) {
    sequence_error = take_sequence(
        *partition_value, callbacks, &record_sequence);
  }
  if (sequence_error == DependencyDirectoryErrorV1::None) {
    sequence_error = take_sequence(
        *partition_value, callbacks, &change_sequence);
  }
  if (replacing && sequence_error != DependencyDirectoryErrorV1::None) {
    note_emergency_fallback(*partition_value);
    result.error = sequence_error;
    result.coarse_fallback_required = true;
    return result;
  }

  if (replacing) {
    if (record->ticket_ref_count != 0 ||
        record->generation_ref_count != 0) {
      record->retired_references =
          RetiredDependencyReferenceCohortV1{
              .physical_identity = record->physical_identity,
              .record_generation = record->record_generation,
              .committed_epoch = record->committed_epoch,
              .generation_ref_count = record->generation_ref_count,
              .ticket_ref_count = record->ticket_ref_count,
              .observability = record->observability,
          };
    }
    record->generation_ref_count = 0;
    record->ticket_ref_count = 0;
    record->physical_identity = resolution.physical_identity;
    record->record_generation = record_sequence;
    owner_reference.physical_identity = resolution.physical_identity;
    owner_reference.record_generation = record_sequence;
  }
  if (publish_epoch && !epoch_overflow) {
    ++record->committed_epoch;
  }
  if (callbacks.observe_terminal_stage != nullptr) {
    const DependencyEpochSnapshotV1 snapshot = snapshot_record(*record);
    callbacks.observe_terminal_stage(
        callbacks.context, DependencyTerminalStageV1::EpochPublished,
        snapshot);
  }

  --record->transition_count;
  owner_reference.transition_active = false;
  result.applied = true;
  result.diagnostic_success = true;
  if (callbacks.observe_terminal_stage != nullptr) {
    const DependencyEpochSnapshotV1 snapshot = snapshot_record(*record);
    callbacks.observe_terminal_stage(
        callbacks.context,
        DependencyTerminalStageV1::TransitionDecremented, snapshot);
  }

  if (epoch_overflow || emergency_before ||
      sequence_error != DependencyDirectoryErrorV1::None) {
    latch_emergency(*partition_value, record);
    result.error = epoch_overflow
        ? DependencyDirectoryErrorV1::CounterOverflow
        : sequence_error;
    result.diagnostic_success = false;
    result.coarse_fallback_required = true;
  } else {
    record->change_generation = change_sequence;
  }
  if (callbacks.observe_terminal_stage != nullptr) {
    const DependencyEpochSnapshotV1 snapshot = snapshot_record(*record);
    callbacks.observe_terminal_stage(
        callbacks.context,
        DependencyTerminalStageV1::ChangeGenerationPublished, snapshot);
  }

  if (!wake_partition(
      *partition_value, callbacks, owner_reference.partition_index)) {
    latch_emergency(*partition_value, record);
    if (result.error == DependencyDirectoryErrorV1::None) {
      result.error = DependencyDirectoryErrorV1::WakeFailed;
    }
    result.diagnostic_success = false;
    result.coarse_fallback_required = true;
  } else if (callbacks.observe_terminal_stage != nullptr) {
    const DependencyEpochSnapshotV1 snapshot = snapshot_record(*record);
    callbacks.observe_terminal_stage(
        callbacks.context, DependencyTerminalStageV1::WaitersBroadcast,
        snapshot);
  }
  result.snapshot = snapshot_record(*record);
  return result;
}

/** Explicit bounded janitor entry point for one caller-owned partition lock. */
[[nodiscard]] inline DependencyReclaimResultV1
reclaim_dependency_partition(
    DependencyDirectoryAttachmentV1 directory,
    std::uint32_t partition_index, std::uint32_t budget,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyReclaimResultV1 result{};
  DependencyDirectoryHeaderV1 *header_value = header(directory);
  if (header_value == nullptr ||
      header_value->bootstrap_state != BootstrapState::Ready ||
      !callbacks.mutation_valid() || budget == 0 ||
      budget > kMaximumDependencyVectorSizeV1 ||
      partition_index >= header_value->config.partition_count) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  PartitionLockGuard lock(callbacks, partition_index);
  DependencyDirectoryPartitionV1 *partition_value =
      dependency_directory_detail::partition(directory, partition_index);
  if (partition_value == nullptr) {
    result.error = DependencyDirectoryErrorV1::CorruptLayout;
    return result;
  }
  return reclaim_locked(
      directory, *partition_value, budget, callbacks);
}

/** Deep validation is bounded by configured capacity and takes one lock at a time. */
[[nodiscard]] inline DependencyDirectoryErrorV1
deep_validate_dependency_directory(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  if (!callbacks.locks_valid()) {
    return DependencyDirectoryErrorV1::InvalidArgument;
  }
  DependencyDirectoryHeaderV1 *header_value = nullptr;
  DependencyDirectoryErrorV1 error = structural_validate(
      directory.region, &header_value);
  if (error != DependencyDirectoryErrorV1::None) {
    return error;
  }
  std::uint64_t logical_capacity_sum = 0;
  for (std::uint32_t partition_index = 0;
       partition_index < header_value->config.partition_count;
       ++partition_index) {
    PartitionLockGuard lock(callbacks, partition_index);
    const DependencyDirectoryPartitionV1 *partition_value =
        dependency_directory_detail::partition(directory, partition_index);
    error = validate_partition_locked(
        directory, *header_value, partition_index);
    if (error != DependencyDirectoryErrorV1::None) {
      return error;
    }
    logical_capacity_sum += partition_value->logical_capacity;
  }
  return logical_capacity_sum == header_value->config.logical_capacity
             ? DependencyDirectoryErrorV1::None
             : DependencyDirectoryErrorV1::CorruptLayout;
}

/** Aggregate diagnostics without exposing region pointers. */
[[nodiscard]] inline DependencyDirectoryStatsResultV1
dependency_directory_stats(
    DependencyDirectoryAttachmentV1 directory,
    const DependencyDirectoryCallbacksV1 &callbacks) noexcept {
  using namespace dependency_directory_detail;
  DependencyDirectoryStatsResultV1 result{};
  if (!callbacks.locks_valid()) {
    result.error = DependencyDirectoryErrorV1::InvalidArgument;
    return result;
  }
  DependencyDirectoryHeaderV1 *header_value = nullptr;
  result.error = structural_validate(directory.region, &header_value);
  if (result.error != DependencyDirectoryErrorV1::None) {
    return result;
  }
  result.stats.logical_capacity = header_value->config.logical_capacity;
  result.stats.physical_capacity = header_value->layout.physical_capacity;
  result.stats.partition_count = header_value->config.partition_count;
  for (std::uint32_t partition_index = 0;
       partition_index < header_value->config.partition_count;
       ++partition_index) {
    PartitionLockGuard lock(callbacks, partition_index);
    const DependencyDirectoryPartitionV1 *partition_value =
        dependency_directory_detail::partition(directory, partition_index);
    result.error = validate_partition_locked(
        directory, *header_value, partition_index);
    if (result.error != DependencyDirectoryErrorV1::None) {
      return result;
    }
    if (result.stats.occupied_count >
            (std::numeric_limits<std::uint32_t>::max)() -
                partition_value->occupied_count ||
        result.stats.tombstone_count >
            (std::numeric_limits<std::uint32_t>::max)() -
                partition_value->tombstone_count) {
      result.error = DependencyDirectoryErrorV1::CounterOverflow;
      return result;
    }
    result.stats.occupied_count += partition_value->occupied_count;
    result.stats.tombstone_count += partition_value->tombstone_count;
    result.stats.emergency_change_generation = std::max(
        result.stats.emergency_change_generation,
        partition_value->emergency_change_generation);
    if (!checked_add_u64(
            result.stats.pressure_event_count,
            partition_value->pressure_event_count) ||
        !checked_add_u64(
            result.stats.reclaim_attempt_count,
            partition_value->reclaim_attempt_count) ||
        !checked_add_u64(
            result.stats.reclaimed_record_count,
            partition_value->reclaimed_record_count) ||
        !checked_add_u64(
            result.stats.retiring_recheck_failure_count,
            partition_value->retiring_recheck_failure_count) ||
        !checked_add_u64(
            result.stats.pressure_fallback_count,
            partition_value->pressure_fallback_count) ||
        !checked_add_u64(
            result.stats.directory_full_fallback_count,
            partition_value->directory_full_fallback_count) ||
        !checked_add_u64(
            result.stats.emergency_fallback_count,
            partition_value->emergency_fallback_count) ||
        !checked_add_u64(
            result.stats.wake_failure_count,
            partition_value->wake_failure_count)) {
      result.error = DependencyDirectoryErrorV1::CounterOverflow;
      return result;
    }
    for (std::uint32_t index = 0;
         index < partition_value->physical_capacity; ++index) {
      const SharedDependencyEpochV1 *record =
          record_at_index(directory, *partition_value, index);
      if (record == nullptr) {
        result.error = DependencyDirectoryErrorV1::CorruptLayout;
        return result;
      }
      if (record->slot_state != DirectorySlotState::Occupied) {
        continue;
      }
      const std::array<std::uint32_t, 6> additions{
          record->transition_count,
          record->generation_ref_count,
          record->retired_references.generation_ref_count,
          record->ticket_ref_count,
          record->retired_references.ticket_ref_count,
          record->owner_ref_count,
      };
      std::array<std::uint32_t *, 6> destinations{
          &result.stats.transition_count,
          &result.stats.generation_ref_count,
          &result.stats.generation_ref_count,
          &result.stats.ticket_ref_count,
          &result.stats.ticket_ref_count,
          &result.stats.owner_ref_count,
      };
      for (std::size_t count_index = 0;
           count_index < additions.size(); ++count_index) {
        if (*destinations[count_index] >
            (std::numeric_limits<std::uint32_t>::max)() -
                additions[count_index]) {
          result.error = DependencyDirectoryErrorV1::CounterOverflow;
          return result;
        }
        *destinations[count_index] += additions[count_index];
      }
      result.stats.maximum_change_generation = std::max(
          result.stats.maximum_change_generation,
          record->change_generation);
    }
  }
  return result;
}

}  // namespace bioseq::cache
