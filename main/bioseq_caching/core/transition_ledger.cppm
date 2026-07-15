/**
 * @file bioseq_caching/core/transition_ledger.cppm
 * @brief Pointer-free bounded owners for dependency transition handshakes.
 *
 * The shared region stores offsets and fixed-width values only.  PostgreSQL
 * ownership, liveness, locking, epoch mutation, and wakeup mechanics are
 * supplied by process-local callbacks.  In particular, a copied dependency
 * reference cannot resolve a transition: callbacks receive the authoritative
 * reference stored in the owner slot and mutate its linear transition flag.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:transition_ledger;

import :wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t kTransitionLedgerMagicV1 = 0x42544c31U;
inline constexpr std::uint32_t kTransitionLedgerAbiVersionV1 = 2;
inline constexpr std::uint32_t kMaximumTransitionOwnersV1 = 1U << 20U;
inline constexpr std::uint32_t kMaximumTransitionPartitionsV1 = 1U << 10U;
inline constexpr std::uint32_t kMaximumTransitionReferencesV1 =
    kMaxPreciseDirtyDependenciesPerBackend;
inline constexpr std::uint32_t kInvalidTransitionOffsetV1 =
    (std::numeric_limits<std::uint32_t>::max)();

/** Every result is deterministic and safe to expose in diagnostics. */
enum class TransitionLedgerErrorV1 : std::uint32_t {
  None = 0,
  InvalidArgument,
  InvalidConfiguration,
  RegionTooSmall,
  RegionMisaligned,
  NotInitialized,
  AbiMismatch,
  CorruptLayout,
  CorruptLedger,
  SequenceUnavailable,
  OwnerBusy,
  StaleOwner,
  InvalidOwnerState,
  InvalidDependencyReference,
  DuplicateDependencyReference,
  DependencyAbaMismatch,
  CapacityExceeded,
  SubtransactionRebuildFailed,
  PartitionLockFailed,
  DependencyBeginFailed,
  DependencyFinishFailed,
  ReferenceReleaseFailed,
  OwnerAlive,
  NothingToRepair,
  BootstrapFailed,
};

enum class TransitionResolutionV1 : std::uint8_t {
  Aborted = 0,
  Committed = 1,
  /** Resolution is proven not to have made the write visible. */
  KnownAborted = 2,
};

enum class TransitionReferenceKindV1 : std::uint8_t {
  None = 0,
  Owner = 1,
};

enum class TransitionInitialReferenceRoleV1 : std::uint8_t {
  EmergencyOnly = 0,
  PreciseAndFallback = 1,
};

enum class TransitionRepairDispositionV1 : std::uint8_t {
  None = 0,
  ClearedDeadCollecting = 1,
  OwnerStillAlive = 2,
  FatalTransitionOwner = 3,
};

/** Full backend identity; ProcNumber alone is never an ownership proof. */
struct TransitionOwnerIdentityV1 {
  std::uint32_t proc_number = 0;
  std::int32_t pid = 0;
  std::uint64_t backend_nonce = 0;
  std::int64_t backend_started_at = 0;
  std::uint64_t full_xid = 0;

  [[nodiscard]] bool operator==(
      const TransitionOwnerIdentityV1 &) const noexcept = default;
};

/** ABA-safe claim for one fixed ProcNumber slot and one top transaction. */
struct TransitionOwnerClaimV1 {
  TransitionOwnerIdentityV1 owner{};
  std::uint64_t claim_sequence = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return pid_and_transaction_valid() && claim_sequence != 0;
  }

  [[nodiscard]] bool pid_and_transaction_valid() const noexcept {
    return owner.pid > 0 && owner.backend_nonce != 0 &&
           owner.backend_started_at != 0 && owner.full_xid != 0;
  }

  [[nodiscard]] bool operator==(
      const TransitionOwnerClaimV1 &) const noexcept = default;
};

/**
 * Exact dependency capability retained by an owner slot.
 *
 * `active` represents the directory owner reference. `transition_active` is
 * a linear capability: begin sets it only after incrementing the dependency,
 * finish clears it only after decrementing that exact contribution, and
 * release must refuse it while set.
 */
struct TransitionDependencyReferenceV1 {
  DependencyKey key{};
  std::uint32_t record_offset = 0;
  std::uint32_t partition_index = 0;
  std::uint32_t reserved_alignment = 0;
  PhysicalIdentityV1 physical_identity{};
  std::uint64_t record_generation = 0;
  TransitionReferenceKindV1 kind = TransitionReferenceKindV1::None;
  bool active = false;
  bool transition_active = false;
  std::array<std::uint8_t, 5> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return active && !transition_active && record_offset != 0 &&
           record_generation != 0 && kind == TransitionReferenceKindV1::Owner;
  }

  [[nodiscard]] bool exact_identity_equal(
      const TransitionDependencyReferenceV1 &other) const noexcept {
    return key == other.key && record_offset == other.record_offset &&
           partition_index == other.partition_index &&
           physical_identity == other.physical_identity &&
           record_generation == other.record_generation &&
           kind == other.kind;
  }

  [[nodiscard]] bool operator==(
      const TransitionDependencyReferenceV1 &) const noexcept = default;
};

/** Fixed preload configuration; one additional binding stores the emergency. */
struct TransitionLedgerConfigV1 {
  std::uint32_t owner_capacity = 1;
  std::uint32_t precise_reference_capacity =
      kMaximumTransitionReferencesV1;
  std::uint32_t dependency_partition_count = 1;
  std::uint32_t reserved = 0;
};

struct TransitionLedgerLayoutV1 {
  std::uint32_t total_bytes = 0;
  std::uint32_t owners_offset = 0;
  std::uint32_t references_offset = 0;
  std::uint32_t references_per_owner = 0;
};

struct TransitionLedgerPreloadResultV1 {
  TransitionLedgerConfigV1 config{};
  TransitionLedgerLayoutV1 layout{};
  std::uint32_t owner_lock_count = 0;
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TransitionLedgerErrorV1::None;
  }
};

/** One shared slot. references_offset is relative to the ledger region. */
struct SharedTransitionOwnerSlotV1 {
  alignas(4) std::uint32_t state =
      static_cast<std::uint32_t>(TransitionOwnerState::Free);
  std::uint32_t references_offset = 0;
  std::uint32_t reference_capacity = 0;
  std::uint32_t precise_reference_count = 0;
  TransitionOwnerIdentityV1 owner{};
  std::uint64_t claim_sequence = 0;
  std::uint64_t last_claim_sequence = 0;
  alignas(4) std::uint32_t incremented_count = 0;
  alignas(4) std::uint32_t outstanding_transition_count = 0;
  std::uint32_t transition_target_count = 0;
  std::uint32_t coarse = 0;
  std::uint32_t initial_reference_precise = 0;
  std::uint32_t diagnostic_failure_count = 0;
  TransitionLedgerErrorV1 last_error = TransitionLedgerErrorV1::None;
  std::uint32_t repair_forbidden = 0;
  std::uint32_t collection_generation = 0;
};

/** Saturating shared counters; snapshots never expose process-local pointers. */
struct SharedTransitionLedgerMetricsV1 {
  alignas(8) std::uint64_t successful_claim_count = 0;
  std::uint64_t coarse_fallback_count = 0;
  std::uint64_t bookkeeping_failure_count = 0;
  std::uint64_t begin_failure_count = 0;
  std::uint64_t finish_failure_count = 0;
  std::uint64_t diagnostic_failure_count = 0;
  std::uint64_t repaired_collecting_count = 0;
  std::uint64_t fatal_owner_count = 0;
  alignas(4) std::uint32_t current_owner_count = 0;
  std::uint32_t owner_high_water = 0;
  std::uint32_t current_transition_count = 0;
  std::uint32_t transition_high_water = 0;
};

struct TransitionLedgerHeaderV1 {
  std::uint32_t magic = 0;
  std::uint32_t abi_version = 0;
  alignas(4) std::uint32_t bootstrap_state =
      static_cast<std::uint32_t>(BootstrapState::Uninitialized);
  std::uint32_t reserved = 0;
  TransitionLedgerConfigV1 config{};
  TransitionLedgerLayoutV1 layout{};
  alignas(8) std::uint64_t next_claim_sequence = 1;
  SharedTransitionLedgerMetricsV1 metrics{};
};

/** Process-local attachment; its pointer is never persisted in shared bytes. */
struct TransitionLedgerAttachmentV1 {
  std::span<std::byte> region{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !region.empty();
  }
};

struct TransitionLedgerAttachResultV1 {
  TransitionLedgerAttachmentV1 ledger{};
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TransitionLedgerErrorV1::None &&
           static_cast<bool>(ledger);
  }
};

/**
 * Result of one dependency-directory callback.
 *
 * `applied` describes the correctness mutation separately from diagnostics.
 * A finish with `applied=true, diagnostic_success=false` has completed every
 * correctness mutation and must not be retried.  For finish callbacks,
 * `applied=true` specifically means that, under the dependency partition lock,
 * the callback published the conservative epoch, decremented this exact
 * owner's transition contribution, advanced change_generation, broadcast the
 * dependency condition variable, and cleared `transition_active`, in that
 * order.  Mandatory publication and wakeup are never merely diagnostics.
 */
struct TransitionCallbackResultV1 {
  bool applied = false;
  bool diagnostic_success = true;
  bool coarse_fallback_required = false;
  std::uint8_t reserved = 0;
  std::uint32_t error = 0;
};

/** No lock or callback address is stored in the shared region. */
struct TransitionLedgerCallbacksV1 {
  void *context = nullptr;
  void (*lock_owner_slot)(void *, std::uint32_t) noexcept = nullptr;
  void (*unlock_owner_slot)(void *, std::uint32_t) noexcept = nullptr;
  bool (*lock_partitions)(
      void *, std::span<const std::uint32_t>) noexcept = nullptr;
  /** Receives ascending unique partitions and must release them in reverse. */
  void (*unlock_partitions_reverse)(
      void *, std::span<const std::uint32_t>) noexcept = nullptr;
  TransitionCallbackResultV1 (*begin_transition_locked)(
      void *, TransitionDependencyReferenceV1 *) noexcept = nullptr;
  TransitionCallbackResultV1 (*finish_transition_and_wake_locked)(
      void *, TransitionDependencyReferenceV1 *,
      TransitionResolutionV1) noexcept = nullptr;
  TransitionCallbackResultV1 (*release_owner_reference_locked)(
      void *, TransitionDependencyReferenceV1 *) noexcept = nullptr;
  bool (*owner_is_alive)(
      void *, const TransitionOwnerIdentityV1 &) noexcept = nullptr;
  void (*mark_coarse_fallback)(
      void *, TransitionLedgerErrorV1) noexcept = nullptr;
  void (*mark_bootstrap_failed)(
      void *, TransitionLedgerErrorV1) noexcept = nullptr;

  [[nodiscard]] bool owner_locks_valid() const noexcept {
    return lock_owner_slot != nullptr && unlock_owner_slot != nullptr;
  }

  [[nodiscard]] bool partition_locks_valid() const noexcept {
    return lock_partitions != nullptr &&
           unlock_partitions_reverse != nullptr;
  }

  [[nodiscard]] bool transition_mutations_valid() const noexcept {
    return owner_locks_valid() && partition_locks_valid() &&
           begin_transition_locked != nullptr &&
           finish_transition_and_wake_locked != nullptr &&
           release_owner_reference_locked != nullptr;
  }
};

struct TransitionLedgerOperationResultV1 {
  TransitionOwnerClaimV1 claim{};
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;
  TransitionOwnerState state = TransitionOwnerState::Free;
  std::uint32_t precise_reference_count = 0;
  std::uint32_t incremented_count = 0;
  std::uint32_t outstanding_transition_count = 0;
  std::uint32_t diagnostic_failure_count = 0;
  bool coarse_fallback_required = false;
  bool bootstrap_failed = false;
  bool reference_consumed = false;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TransitionLedgerErrorV1::None;
  }
};

struct TransitionRepairResultV1 {
  TransitionOwnerClaimV1 observed_claim{};
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;
  TransitionRepairDispositionV1 disposition =
      TransitionRepairDispositionV1::None;
  bool bootstrap_failed = false;
  std::array<std::uint8_t, 2> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TransitionLedgerErrorV1::None;
  }
};

/** Bounded, text-free public diagnostic row. */
struct TransitionOwnerDiagnosticV1 {
  std::uint32_t slot_index = 0;
  TransitionOwnerState state = TransitionOwnerState::Free;
  TransitionOwnerIdentityV1 owner{};
  std::uint64_t claim_sequence = 0;
  std::uint32_t precise_reference_count = 0;
  std::uint32_t incremented_count = 0;
  std::uint32_t outstanding_transition_count = 0;
  std::uint32_t retained_owner_reference_count = 0;
  std::uint32_t coarse = 0;
  std::uint32_t diagnostic_failure_count = 0;
  TransitionLedgerErrorV1 last_error = TransitionLedgerErrorV1::None;
  std::uint32_t repair_forbidden = 0;
};

struct TransitionOwnerDiagnosticsResultV1 {
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;
  std::uint32_t copied_count = 0;
  std::uint32_t total_non_free = 0;
  bool truncated = false;
  std::array<std::uint8_t, 3> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TransitionLedgerErrorV1::None;
  }
};

struct TransitionLedgerMetricsResultV1 {
  SharedTransitionLedgerMetricsV1 metrics{};
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == TransitionLedgerErrorV1::None;
  }
};

static_assert(std::is_trivially_copyable_v<TransitionOwnerIdentityV1>);
static_assert(std::is_trivially_copyable_v<TransitionOwnerClaimV1>);
static_assert(
    std::is_trivially_copyable_v<TransitionDependencyReferenceV1>);
static_assert(std::is_trivially_copyable_v<SharedTransitionOwnerSlotV1>);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

namespace transition_ledger_detail {

[[nodiscard]] constexpr bool power_of_two(std::uint32_t value) noexcept {
  return value != 0 && (value & (value - 1U)) == 0;
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

[[nodiscard]] inline TransitionLedgerHeaderV1 *header(
    TransitionLedgerAttachmentV1 ledger) noexcept {
  return object_at<TransitionLedgerHeaderV1>(ledger.region, 0);
}

[[nodiscard]] inline const TransitionLedgerHeaderV1 *header(
    std::span<const std::byte> region) noexcept {
  return object_at<TransitionLedgerHeaderV1>(region, 0);
}

[[nodiscard]] inline BootstrapState load_bootstrap_state(
    TransitionLedgerHeaderV1 &header_value) noexcept {
  return static_cast<BootstrapState>(
      std::atomic_ref<std::uint32_t>(
          header_value.bootstrap_state).load(std::memory_order_acquire));
}

inline void store_bootstrap_state(
    TransitionLedgerHeaderV1 &header_value,
    BootstrapState state) noexcept {
  std::atomic_ref<std::uint32_t>(header_value.bootstrap_state).store(
      static_cast<std::uint32_t>(state), std::memory_order_release);
}

[[nodiscard]] inline SharedTransitionOwnerSlotV1 *owner_slot(
    TransitionLedgerAttachmentV1 ledger, std::uint32_t slot_index) noexcept {
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr ||
      slot_index >= header_value->config.owner_capacity) {
    return nullptr;
  }
  const std::uint64_t offset =
      static_cast<std::uint64_t>(header_value->layout.owners_offset) +
      static_cast<std::uint64_t>(slot_index) *
          sizeof(SharedTransitionOwnerSlotV1);
  if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
    return nullptr;
  }
  return object_at<SharedTransitionOwnerSlotV1>(
      ledger.region, static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline TransitionDependencyReferenceV1 *reference_at(
    TransitionLedgerAttachmentV1 ledger,
    const SharedTransitionOwnerSlotV1 &slot,
    std::uint32_t reference_index) noexcept {
  if (reference_index >= slot.reference_capacity) {
    return nullptr;
  }
  const std::uint64_t offset =
      static_cast<std::uint64_t>(slot.references_offset) +
      static_cast<std::uint64_t>(reference_index) *
          sizeof(TransitionDependencyReferenceV1);
  if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
    return nullptr;
  }
  return object_at<TransitionDependencyReferenceV1>(
      ledger.region, static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline TransitionOwnerState load_state(
    SharedTransitionOwnerSlotV1 &slot) noexcept {
  return static_cast<TransitionOwnerState>(
      std::atomic_ref<std::uint32_t>(slot.state).load(
          std::memory_order_acquire));
}

inline void store_state(
    SharedTransitionOwnerSlotV1 &slot,
    TransitionOwnerState state) noexcept {
  std::atomic_ref<std::uint32_t>(slot.state).store(
      static_cast<std::uint32_t>(state), std::memory_order_release);
}

[[nodiscard]] inline bool compare_exchange_state(
    SharedTransitionOwnerSlotV1 &slot,
    TransitionOwnerState expected,
    TransitionOwnerState desired) noexcept {
  std::uint32_t expected_value = static_cast<std::uint32_t>(expected);
  return std::atomic_ref<std::uint32_t>(slot.state).compare_exchange_strong(
      expected_value, static_cast<std::uint32_t>(desired),
      std::memory_order_acq_rel, std::memory_order_acquire);
}

[[nodiscard]] inline std::uint32_t load_incremented(
    SharedTransitionOwnerSlotV1 &slot) noexcept {
  return std::atomic_ref<std::uint32_t>(slot.incremented_count)
      .load(std::memory_order_acquire);
}

inline void store_incremented(
    SharedTransitionOwnerSlotV1 &slot, std::uint32_t value) noexcept {
  std::atomic_ref<std::uint32_t>(slot.incremented_count)
      .store(value, std::memory_order_release);
}

[[nodiscard]] inline std::uint32_t load_outstanding(
    SharedTransitionOwnerSlotV1 &slot) noexcept {
  return std::atomic_ref<std::uint32_t>(slot.outstanding_transition_count)
      .load(std::memory_order_acquire);
}

inline void store_outstanding(
    SharedTransitionOwnerSlotV1 &slot, std::uint32_t value) noexcept {
  std::atomic_ref<std::uint32_t>(slot.outstanding_transition_count)
      .store(value, std::memory_order_release);
}

[[nodiscard]] constexpr bool valid_owner_state(
    TransitionOwnerState state) noexcept {
  return state == TransitionOwnerState::Free ||
         state == TransitionOwnerState::Collecting ||
         state == TransitionOwnerState::Transitioning ||
         state == TransitionOwnerState::Active ||
         state == TransitionOwnerState::Repairing;
}

[[nodiscard]] inline bool valid_owner(
    const TransitionOwnerClaimV1 &claim,
    std::uint32_t owner_capacity) noexcept {
  return static_cast<bool>(claim) &&
         claim.owner.proc_number < owner_capacity;
}

[[nodiscard]] inline bool valid_owner_identity(
    const TransitionOwnerIdentityV1 &owner,
    std::uint32_t owner_capacity) noexcept {
  return owner.proc_number < owner_capacity && owner.pid > 0 &&
         owner.backend_nonce != 0 && owner.backend_started_at != 0 &&
         owner.full_xid != 0;
}

[[nodiscard]] inline bool valid_reference_identity(
    const TransitionDependencyReferenceV1 &reference,
    std::uint32_t partition_count) noexcept {
  if (!reference.active || reference.record_offset == 0 ||
      reference.record_generation == 0 ||
      reference.kind != TransitionReferenceKindV1::Owner ||
      reference.partition_index >= partition_count ||
      reference.reserved_alignment != 0 ||
      !physical_identity_valid_v1(reference.physical_identity) ||
      reference.key.database_oid == 0 ||
      reference.key.kind < DependencyKind::RelationContent ||
      reference.key.kind > DependencyKind::DatabaseDefinitionEpoch ||
      reference.key.reserved != 0) {
    return false;
  }
  for (const std::uint8_t value : reference.reserved) {
    if (value != 0) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline bool valid_reference(
    const TransitionDependencyReferenceV1 &reference,
    std::uint32_t partition_count) noexcept {
  return valid_reference_identity(reference, partition_count) &&
         !reference.transition_active;
}

[[nodiscard]] inline bool claim_matches(
    const SharedTransitionOwnerSlotV1 &slot,
    const TransitionOwnerClaimV1 &claim) noexcept {
  return slot.owner == claim.owner &&
         slot.claim_sequence == claim.claim_sequence;
}

inline void saturating_increment(std::uint64_t &value) noexcept {
  std::atomic_ref<std::uint64_t> atomic_value(value);
  std::uint64_t observed = atomic_value.load(std::memory_order_relaxed);
  while (observed != (std::numeric_limits<std::uint64_t>::max)() &&
         !atomic_value.compare_exchange_weak(
             observed, observed + 1U, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

inline void saturating_add(
    std::uint64_t &value, std::uint32_t amount) noexcept {
  std::atomic_ref<std::uint64_t> atomic_value(value);
  std::uint64_t observed = atomic_value.load(std::memory_order_relaxed);
  for (;;) {
    const std::uint64_t maximum =
        (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t desired =
        amount > maximum - observed ? maximum : observed + amount;
    if (desired == observed || atomic_value.compare_exchange_weak(
        observed, desired, std::memory_order_relaxed,
        std::memory_order_relaxed)) {
      return;
    }
  }
}

[[nodiscard]] inline std::uint32_t increment_current(
    std::uint32_t &current, std::uint32_t &high_water) noexcept {
  std::atomic_ref<std::uint32_t> atomic_current(current);
  std::uint32_t observed = atomic_current.load(std::memory_order_relaxed);
  while (observed != (std::numeric_limits<std::uint32_t>::max)() &&
         !atomic_current.compare_exchange_weak(
             observed, observed + 1U, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
  const std::uint32_t value = observed ==
      (std::numeric_limits<std::uint32_t>::max)()
      ? observed
      : observed + 1U;
  std::atomic_ref<std::uint32_t> atomic_high(high_water);
  std::uint32_t previous_high =
      atomic_high.load(std::memory_order_relaxed);
  while (previous_high < value && !atomic_high.compare_exchange_weak(
      previous_high, value, std::memory_order_relaxed,
      std::memory_order_relaxed)) {
  }
  return value;
}

inline void decrement_current(std::uint32_t &current) noexcept {
  std::atomic_ref<std::uint32_t> atomic_current(current);
  std::uint32_t observed = atomic_current.load(std::memory_order_relaxed);
  while (observed != 0 && !atomic_current.compare_exchange_weak(
      observed, observed - 1U, std::memory_order_relaxed,
      std::memory_order_relaxed)) {
  }
}

[[nodiscard]] inline std::uint64_t allocate_claim_sequence(
    TransitionLedgerHeaderV1 &header_value) noexcept {
  std::atomic_ref<std::uint64_t> next(header_value.next_claim_sequence);
  std::uint64_t observed = next.load(std::memory_order_acquire);
  while (observed != 0 &&
         observed != (std::numeric_limits<std::uint64_t>::max)()) {
    if (next.compare_exchange_weak(
        observed, observed + 1U, std::memory_order_acq_rel,
        std::memory_order_acquire)) {
      return observed;
    }
  }
  return 0;
}

class OwnerLockGuard {
 public:
  OwnerLockGuard(
      const TransitionLedgerCallbacksV1 &callbacks,
      std::uint32_t slot_index) noexcept
      : callbacks_(callbacks), slot_index_(slot_index) {
    callbacks_.lock_owner_slot(callbacks_.context, slot_index_);
  }

  OwnerLockGuard(const OwnerLockGuard &) = delete;
  OwnerLockGuard &operator=(const OwnerLockGuard &) = delete;

  ~OwnerLockGuard() {
    callbacks_.unlock_owner_slot(callbacks_.context, slot_index_);
  }

 private:
  const TransitionLedgerCallbacksV1 &callbacks_;
  std::uint32_t slot_index_;
};

[[nodiscard]] inline std::uint32_t sort_and_deduplicate(
    std::span<std::uint32_t> values, std::uint32_t count) noexcept {
  std::sort(values.begin(), values.begin() + count);
  std::uint32_t unique_count = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (unique_count == 0 || values[index] != values[unique_count - 1U]) {
      values[unique_count++] = values[index];
    }
  }
  return unique_count;
}

[[nodiscard]] inline TransitionLedgerOperationResultV1 snapshot_result(
    SharedTransitionOwnerSlotV1 &slot,
    TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None) noexcept {
  return TransitionLedgerOperationResultV1{
      .claim = TransitionOwnerClaimV1{
          .owner = slot.owner,
          .claim_sequence = slot.claim_sequence,
      },
      .error = error,
      .state = load_state(slot),
      .precise_reference_count =
          slot.precise_reference_count + slot.initial_reference_precise,
      .incremented_count = load_incremented(slot),
      .outstanding_transition_count = load_outstanding(slot),
      .diagnostic_failure_count = slot.diagnostic_failure_count,
      .coarse_fallback_required = slot.coarse != 0,
      .bootstrap_failed = error == TransitionLedgerErrorV1::BootstrapFailed,
  };
}

inline void note_diagnostic_failure(
    TransitionLedgerHeaderV1 &header_value,
    SharedTransitionOwnerSlotV1 &slot,
    TransitionLedgerErrorV1 error) noexcept {
  if (slot.diagnostic_failure_count !=
      (std::numeric_limits<std::uint32_t>::max)()) {
    ++slot.diagnostic_failure_count;
  }
  slot.last_error = error;
  saturating_increment(
      header_value.metrics.diagnostic_failure_count);
  saturating_increment(header_value.metrics.coarse_fallback_count);
  slot.coarse = 1;
}

inline void note_bookkeeping_failure(
    TransitionLedgerHeaderV1 &header_value,
    SharedTransitionOwnerSlotV1 &slot,
  TransitionLedgerErrorV1 error) noexcept {
  saturating_increment(header_value.metrics.bookkeeping_failure_count);
  saturating_increment(header_value.metrics.coarse_fallback_count);
  slot.coarse = 1;
  slot.last_error = error;
}

inline void note_coarse_fallback(
    TransitionLedgerHeaderV1 &header_value,
    SharedTransitionOwnerSlotV1 &slot,
    TransitionLedgerErrorV1 error) noexcept {
  saturating_increment(header_value.metrics.coarse_fallback_count);
  slot.coarse = 1;
  slot.last_error = error;
}

inline void add_diagnostic_failures(
    SharedTransitionOwnerSlotV1 &slot, std::uint32_t count) noexcept {
  const std::uint32_t maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  slot.diagnostic_failure_count =
      count > maximum - slot.diagnostic_failure_count
      ? maximum
      : slot.diagnostic_failure_count + count;
}

inline void zero_reference(
    TransitionDependencyReferenceV1 &reference) noexcept {
  std::memset(&reference, 0, sizeof(reference));
}

[[nodiscard]] inline bool reference_is_zero(
    const TransitionDependencyReferenceV1 &reference) noexcept {
  const auto *bytes = reinterpret_cast<const std::byte *>(&reference);
  return std::all_of(
      bytes, bytes + sizeof(reference),
      [](std::byte value) noexcept { return value == std::byte{}; });
}

inline void clear_owner_slot(
    TransitionLedgerAttachmentV1 ledger,
    TransitionLedgerHeaderV1 &header_value,
    SharedTransitionOwnerSlotV1 &slot) noexcept {
  const std::uint32_t references_offset = slot.references_offset;
  const std::uint32_t reference_capacity = slot.reference_capacity;
  const std::uint64_t last_claim_sequence = slot.last_claim_sequence;
  for (std::uint32_t index = 0; index < reference_capacity; ++index) {
    TransitionDependencyReferenceV1 *reference =
        reference_at(ledger, slot, index);
    if (reference != nullptr) {
      zero_reference(*reference);
    }
  }
  slot = SharedTransitionOwnerSlotV1{
      .references_offset = references_offset,
      .reference_capacity = reference_capacity,
      .last_claim_sequence = last_claim_sequence,
  };
  decrement_current(header_value.metrics.current_owner_count);
  store_state(slot, TransitionOwnerState::Free);
}

[[nodiscard]] inline std::uint32_t collect_partitions(
    TransitionLedgerAttachmentV1 ledger,
    SharedTransitionOwnerSlotV1 &slot,
    std::span<std::uint32_t> partitions,
    bool transition_only) noexcept {
  std::uint32_t count = 0;
  for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
    TransitionDependencyReferenceV1 *reference =
        reference_at(ledger, slot, index);
    if (reference == nullptr) {
      return kInvalidTransitionOffsetV1;
    }
    const bool selected = transition_only
        ? reference->transition_active
        : (reference->active || reference->transition_active);
    if (!selected) {
      continue;
    }
    if (count >= partitions.size()) {
      return kInvalidTransitionOffsetV1;
    }
    partitions[count++] = reference->partition_index;
  }
  return sort_and_deduplicate(partitions, count);
}

struct DrainResultV1 {
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;
  std::uint32_t outstanding_transitions = 0;
  std::uint32_t retained_owner_references = 0;
  std::uint32_t diagnostic_failures = 0;
  bool coarse_fallback_required = false;
  bool bootstrap_failed = false;
  bool contract_failure = false;
};

/** Drain every binding; diagnostic failure never short-circuits cleanup. */
[[nodiscard]] inline DrainResultV1 drain_locked(
    TransitionLedgerAttachmentV1 ledger,
    TransitionLedgerHeaderV1 &header_value,
    SharedTransitionOwnerSlotV1 &slot,
    TransitionResolutionV1 resolution,
    bool release_owner_references,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  DrainResultV1 result{};
  for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
    TransitionDependencyReferenceV1 *reference =
        reference_at(ledger, slot, index);
    if (reference == nullptr) {
      result.error = TransitionLedgerErrorV1::CorruptLayout;
      result.bootstrap_failed = true;
      return result;
    }
    if (!reference->transition_active) {
      continue;
    }
    const TransitionCallbackResultV1 mutation =
        callbacks.finish_transition_and_wake_locked(
            callbacks.context, reference, resolution);
    const bool contract_valid = mutation.applied
        ? !reference->transition_active
        : reference->transition_active;
    if (mutation.applied) {
      reference->transition_active = false;
      decrement_current(
          header_value.metrics.current_transition_count);
    }
    if (!contract_valid) {
      result.error = TransitionLedgerErrorV1::DependencyFinishFailed;
      result.bootstrap_failed = true;
      result.contract_failure = true;
    } else if (!mutation.applied) {
      result.error = TransitionLedgerErrorV1::DependencyFinishFailed;
      result.bootstrap_failed = true;
    }
    if (!mutation.diagnostic_success) {
      ++result.diagnostic_failures;
      result.coarse_fallback_required = true;
      if (result.error == TransitionLedgerErrorV1::None) {
        result.error = TransitionLedgerErrorV1::DependencyFinishFailed;
      }
    }
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        mutation.coarse_fallback_required;
  }

  if (release_owner_references) {
    for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
      TransitionDependencyReferenceV1 *reference =
          reference_at(ledger, slot, index);
      if (reference == nullptr) {
        result.error = TransitionLedgerErrorV1::CorruptLayout;
        result.bootstrap_failed = true;
        return result;
      }
      if (!reference->active || reference->transition_active) {
        continue;
      }
      const TransitionCallbackResultV1 mutation =
          callbacks.release_owner_reference_locked(
              callbacks.context, reference);
      const bool contract_valid = mutation.applied
          ? !reference->active
          : reference->active;
      if (mutation.applied) {
        reference->active = false;
      }
      if (!contract_valid || !mutation.applied) {
        result.error = TransitionLedgerErrorV1::ReferenceReleaseFailed;
        result.bootstrap_failed = true;
        result.contract_failure = result.contract_failure || !contract_valid;
      }
      if (!mutation.diagnostic_success) {
        ++result.diagnostic_failures;
        result.coarse_fallback_required = true;
        if (result.error == TransitionLedgerErrorV1::None) {
          result.error = TransitionLedgerErrorV1::ReferenceReleaseFailed;
        }
      }
      result.coarse_fallback_required =
          result.coarse_fallback_required ||
          mutation.coarse_fallback_required;
    }
  }

  for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
    TransitionDependencyReferenceV1 *reference =
        reference_at(ledger, slot, index);
    if (reference == nullptr) {
      result.error = TransitionLedgerErrorV1::CorruptLayout;
      result.bootstrap_failed = true;
      return result;
    }
    result.outstanding_transitions += reference->transition_active ? 1U : 0U;
    result.retained_owner_references += reference->active ? 1U : 0U;
  }
  store_outstanding(slot, result.outstanding_transitions);
  return result;
}

[[nodiscard]] inline TransitionLedgerErrorV1 structural_validate(
    std::span<std::byte> region,
    TransitionLedgerHeaderV1 **output_header) noexcept {
  if (output_header == nullptr || region.data() == nullptr) {
    return TransitionLedgerErrorV1::InvalidArgument;
  }
  if (reinterpret_cast<std::uintptr_t>(region.data()) %
          alignof(TransitionLedgerHeaderV1) !=
      0) {
    return TransitionLedgerErrorV1::RegionMisaligned;
  }
  TransitionLedgerHeaderV1 *header_value =
      object_at<TransitionLedgerHeaderV1>(region, 0);
  if (header_value == nullptr) {
    return TransitionLedgerErrorV1::RegionTooSmall;
  }
  if (header_value->magic != kTransitionLedgerMagicV1 ||
      load_bootstrap_state(*header_value) != BootstrapState::Ready) {
    return TransitionLedgerErrorV1::NotInitialized;
  }
  if (header_value->abi_version != kTransitionLedgerAbiVersionV1) {
    return TransitionLedgerErrorV1::AbiMismatch;
  }
  const TransitionLedgerConfigV1 &config = header_value->config;
  const TransitionLedgerLayoutV1 &layout = header_value->layout;
  if (config.owner_capacity == 0 ||
      config.owner_capacity > kMaximumTransitionOwnersV1 ||
      config.precise_reference_capacity == 0 ||
      config.precise_reference_capacity > kMaximumTransitionReferencesV1 ||
      !power_of_two(config.dependency_partition_count) ||
      config.dependency_partition_count > kMaximumTransitionPartitionsV1 ||
      config.reserved != 0 || layout.total_bytes > region.size() ||
      layout.owners_offset == 0 ||
      layout.references_offset <= layout.owners_offset ||
      layout.references_per_owner !=
          config.precise_reference_capacity + 1U ||
      std::atomic_ref<std::uint64_t>(
          header_value->next_claim_sequence).load(
              std::memory_order_acquire) == 0 ||
      std::atomic_ref<std::uint32_t>(
          header_value->metrics.current_owner_count).load(
              std::memory_order_acquire) > config.owner_capacity ||
      static_cast<std::uint64_t>(
          std::atomic_ref<std::uint32_t>(
              header_value->metrics.current_transition_count).load(
                  std::memory_order_acquire)) >
          static_cast<std::uint64_t>(config.owner_capacity) *
              layout.references_per_owner) {
    return TransitionLedgerErrorV1::CorruptLayout;
  }
  *output_header = header_value;
  return TransitionLedgerErrorV1::None;
}

}  // namespace transition_ledger_detail

/** Compute exact fixed shared bytes; no runtime allocation follows preload. */
[[nodiscard]] inline TransitionLedgerPreloadResultV1
transition_ledger_preload_request(
    TransitionLedgerConfigV1 config = {}) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerPreloadResultV1 result{.config = config};
  if (config.owner_capacity == 0 ||
      config.owner_capacity > kMaximumTransitionOwnersV1 ||
      config.precise_reference_capacity == 0 ||
      config.precise_reference_capacity > kMaximumTransitionReferencesV1 ||
      !power_of_two(config.dependency_partition_count) ||
      config.dependency_partition_count > kMaximumTransitionPartitionsV1 ||
      config.reserved != 0) {
    result.error = TransitionLedgerErrorV1::InvalidConfiguration;
    return result;
  }
  const std::uint64_t owners_offset = align_up(
      sizeof(TransitionLedgerHeaderV1), alignof(SharedTransitionOwnerSlotV1));
  const std::uint64_t owners_bytes =
      static_cast<std::uint64_t>(config.owner_capacity) *
      sizeof(SharedTransitionOwnerSlotV1);
  const std::uint64_t references_offset = align_up(
      owners_offset + owners_bytes,
      alignof(TransitionDependencyReferenceV1));
  const std::uint64_t references_per_owner =
      static_cast<std::uint64_t>(config.precise_reference_capacity) + 1U;
  const std::uint64_t references_bytes =
      static_cast<std::uint64_t>(config.owner_capacity) *
      references_per_owner * sizeof(TransitionDependencyReferenceV1);
  const std::uint64_t total_bytes = references_offset + references_bytes;
  if (owners_offset == 0 || references_offset == 0 ||
      total_bytes > (std::numeric_limits<std::uint32_t>::max)()) {
    result.error = TransitionLedgerErrorV1::InvalidConfiguration;
    return result;
  }
  result.layout = TransitionLedgerLayoutV1{
      .total_bytes = static_cast<std::uint32_t>(total_bytes),
      .owners_offset = static_cast<std::uint32_t>(owners_offset),
      .references_offset = static_cast<std::uint32_t>(references_offset),
      .references_per_owner =
          static_cast<std::uint32_t>(references_per_owner),
  };
  result.owner_lock_count = config.owner_capacity;
  return result;
}

/** Initialize caller-owned bytes and publish Ready only after all offsets exist. */
[[nodiscard]] inline TransitionLedgerErrorV1 initialize_transition_ledger(
    std::span<std::byte> region,
    const TransitionLedgerPreloadResultV1 &request) noexcept {
  using namespace transition_ledger_detail;
  if (!request || region.data() == nullptr) {
    return TransitionLedgerErrorV1::InvalidArgument;
  }
  if (reinterpret_cast<std::uintptr_t>(region.data()) %
          alignof(TransitionLedgerHeaderV1) !=
      0) {
    return TransitionLedgerErrorV1::RegionMisaligned;
  }
  if (region.size() < request.layout.total_bytes) {
    return TransitionLedgerErrorV1::RegionTooSmall;
  }
  std::memset(region.data(), 0, request.layout.total_bytes);
  TransitionLedgerHeaderV1 *header_value =
      object_at<TransitionLedgerHeaderV1>(region, 0);
  if (header_value == nullptr) {
    return TransitionLedgerErrorV1::CorruptLayout;
  }
  *header_value = TransitionLedgerHeaderV1{
      .magic = kTransitionLedgerMagicV1,
      .abi_version = kTransitionLedgerAbiVersionV1,
      .bootstrap_state =
          static_cast<std::uint32_t>(BootstrapState::Initializing),
      .config = request.config,
      .layout = request.layout,
  };
  TransitionLedgerAttachmentV1 ledger{.region = region};
  for (std::uint32_t index = 0;
       index < request.config.owner_capacity; ++index) {
    SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, index);
    const std::uint64_t references_offset =
        static_cast<std::uint64_t>(request.layout.references_offset) +
        static_cast<std::uint64_t>(index) *
            request.layout.references_per_owner *
            sizeof(TransitionDependencyReferenceV1);
    if (slot == nullptr ||
        references_offset > (std::numeric_limits<std::uint32_t>::max)()) {
      store_bootstrap_state(*header_value, BootstrapState::Failed);
      return TransitionLedgerErrorV1::CorruptLayout;
    }
    *slot = SharedTransitionOwnerSlotV1{
        .references_offset = static_cast<std::uint32_t>(references_offset),
        .reference_capacity = request.layout.references_per_owner,
    };
    store_state(*slot, TransitionOwnerState::Free);
  }
  store_bootstrap_state(*header_value, BootstrapState::Ready);
  return TransitionLedgerErrorV1::None;
}

/** Attach only after validating every immutable owner/reference slice. */
[[nodiscard]] inline TransitionLedgerAttachResultV1
attach_transition_ledger(std::span<std::byte> region) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerAttachResultV1 result{};
  TransitionLedgerHeaderV1 *header_value = nullptr;
  result.error = structural_validate(region, &header_value);
  if (result.error != TransitionLedgerErrorV1::None) {
    return result;
  }
  TransitionLedgerAttachmentV1 ledger{.region = region};
  for (std::uint32_t index = 0;
       index < header_value->config.owner_capacity; ++index) {
    SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, index);
    const std::uint64_t expected_offset =
        static_cast<std::uint64_t>(header_value->layout.references_offset) +
        static_cast<std::uint64_t>(index) *
            header_value->layout.references_per_owner *
            sizeof(TransitionDependencyReferenceV1);
    if (slot == nullptr || slot->references_offset != expected_offset ||
        slot->reference_capacity !=
            header_value->layout.references_per_owner ||
        !valid_owner_state(load_state(*slot))) {
      result.error = TransitionLedgerErrorV1::CorruptLayout;
      return result;
    }
  }
  result.ledger = ledger;
  return result;
}

/** Claim the ProcNumber-indexed slot and retain its emergency owner reference. */
[[nodiscard]] inline TransitionLedgerOperationResultV1
claim_transition_owner(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionOwnerIdentityV1 &owner,
    const TransitionDependencyReferenceV1 &emergency_reference,
    const TransitionLedgerCallbacksV1 &callbacks,
    TransitionInitialReferenceRoleV1 initial_role =
        TransitionInitialReferenceRoleV1::EmergencyOnly) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.owner_locks_valid() ||
      !valid_owner_identity(owner, header_value->config.owner_capacity) ||
      (initial_role != TransitionInitialReferenceRoleV1::EmergencyOnly &&
       initial_role !=
           TransitionInitialReferenceRoleV1::PreciseAndFallback)) {
    return TransitionLedgerOperationResultV1{
        .claim = TransitionOwnerClaimV1{.owner = owner},
        .error = TransitionLedgerErrorV1::InvalidArgument,
        .coarse_fallback_required = true,
    };
  }
  if (!valid_reference(
      emergency_reference,
      header_value->config.dependency_partition_count)) {
    saturating_increment(
        header_value->metrics.bookkeeping_failure_count);
    saturating_increment(
        header_value->metrics.coarse_fallback_count);
    if (callbacks.mark_coarse_fallback != nullptr) {
      callbacks.mark_coarse_fallback(
          callbacks.context,
          TransitionLedgerErrorV1::InvalidDependencyReference);
    }
    return TransitionLedgerOperationResultV1{
        .claim = TransitionOwnerClaimV1{.owner = owner},
        .error = TransitionLedgerErrorV1::InvalidDependencyReference,
        .coarse_fallback_required = true,
    };
  }
  const std::uint32_t slot_index = owner.proc_number;
  OwnerLockGuard lock(callbacks, slot_index);
  SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, slot_index);
  if (slot == nullptr) {
    return TransitionLedgerOperationResultV1{
        .claim = TransitionOwnerClaimV1{.owner = owner},
        .error = TransitionLedgerErrorV1::CorruptLayout,
        .bootstrap_failed = true,
    };
  }
  if (load_state(*slot) != TransitionOwnerState::Free) {
    return snapshot_result(*slot, TransitionLedgerErrorV1::OwnerBusy);
  }
  const std::uint64_t claim_sequence =
      allocate_claim_sequence(*header_value);
  if (claim_sequence == 0) {
    TransitionLedgerOperationResultV1 result = snapshot_result(
        *slot, TransitionLedgerErrorV1::SequenceUnavailable);
    result.claim = TransitionOwnerClaimV1{.owner = owner};
    result.coarse_fallback_required = true;
    return result;
  }
  TransitionDependencyReferenceV1 *emergency =
      reference_at(ledger, *slot, 0);
  if (emergency == nullptr) {
    return snapshot_result(*slot, TransitionLedgerErrorV1::CorruptLayout);
  }
  for (std::uint32_t index = 0; index < slot->reference_capacity; ++index) {
    TransitionDependencyReferenceV1 *reference =
        reference_at(ledger, *slot, index);
    if (reference == nullptr) {
      return snapshot_result(*slot, TransitionLedgerErrorV1::CorruptLayout);
    }
    zero_reference(*reference);
  }
  *emergency = emergency_reference;
  slot->owner = owner;
  slot->claim_sequence = claim_sequence;
  slot->last_claim_sequence = claim_sequence;
  slot->precise_reference_count = 0;
  slot->transition_target_count = 0;
  slot->coarse = 0;
  slot->initial_reference_precise =
      initial_role == TransitionInitialReferenceRoleV1::PreciseAndFallback
      ? 1U
      : 0U;
  slot->diagnostic_failure_count = 0;
  slot->last_error = TransitionLedgerErrorV1::None;
  slot->repair_forbidden = 0;
  slot->collection_generation = 1;
  store_incremented(*slot, 0);
  store_outstanding(*slot, 0);
  store_state(*slot, TransitionOwnerState::Collecting);
  saturating_increment(
      header_value->metrics.successful_claim_count);
  static_cast<void>(increment_current(
      header_value->metrics.current_owner_count,
      header_value->metrics.owner_high_water));
  TransitionLedgerOperationResultV1 result = snapshot_result(*slot);
  result.reference_consumed = true;
  return result;
}

/**
 * Add one precise owner reference while the slot is collecting.
 *
 * The coarse marker is set before validation and restored only after the full
 * exact binding is durable.  Capacity or ABA trouble therefore cannot leave a
 * partial precise promise.
 */
[[nodiscard]] inline TransitionLedgerOperationResultV1
record_transition_dependency(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionOwnerClaimV1 &claim,
    const TransitionDependencyReferenceV1 &reference,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.owner_locks_valid() ||
      !valid_owner(claim, header_value->config.owner_capacity)) {
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::InvalidArgument,
        .coarse_fallback_required = true,
    };
  }
  const std::uint32_t slot_index = claim.owner.proc_number;
  OwnerLockGuard lock(callbacks, slot_index);
  SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, slot_index);
  if (slot == nullptr) {
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::CorruptLayout,
        .bootstrap_failed = true,
    };
  }
  if (!claim_matches(*slot, claim)) {
    return snapshot_result(*slot, TransitionLedgerErrorV1::StaleOwner);
  }
  if (load_state(*slot) != TransitionOwnerState::Collecting) {
    return snapshot_result(
        *slot, TransitionLedgerErrorV1::InvalidOwnerState);
  }
  if (slot->coarse != 0) {
    return snapshot_result(*slot, TransitionLedgerErrorV1::CapacityExceeded);
  }
  const std::uint32_t previous_coarse = slot->coarse;
  slot->coarse = 1;
  if (!valid_reference(
      reference, header_value->config.dependency_partition_count)) {
    note_bookkeeping_failure(
        *header_value, *slot,
        TransitionLedgerErrorV1::InvalidDependencyReference);
    return snapshot_result(
        *slot, TransitionLedgerErrorV1::InvalidDependencyReference);
  }
  if (slot->initial_reference_precise != 0) {
    TransitionDependencyReferenceV1 *initial =
        reference_at(ledger, *slot, 0);
    if (initial == nullptr) {
      note_diagnostic_failure(
          *header_value, *slot, TransitionLedgerErrorV1::CorruptLayout);
      saturating_increment(
          header_value->metrics.bookkeeping_failure_count);
      return snapshot_result(*slot, TransitionLedgerErrorV1::CorruptLayout);
    }
    if (initial->exact_identity_equal(reference)) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::DuplicateDependencyReference);
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::DuplicateDependencyReference);
    }
    if (initial->key == reference.key) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::DependencyAbaMismatch);
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::DependencyAbaMismatch);
    }
  }
  for (std::uint32_t index = 1;
       index <= slot->precise_reference_count; ++index) {
    TransitionDependencyReferenceV1 *existing =
        reference_at(ledger, *slot, index);
    if (existing == nullptr) {
      note_diagnostic_failure(
          *header_value, *slot, TransitionLedgerErrorV1::CorruptLayout);
      saturating_increment(
          header_value->metrics.bookkeeping_failure_count);
      return snapshot_result(*slot, TransitionLedgerErrorV1::CorruptLayout);
    }
    if (existing->exact_identity_equal(reference)) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::DuplicateDependencyReference);
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::DuplicateDependencyReference);
    }
    if (existing->key == reference.key) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::DependencyAbaMismatch);
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::DependencyAbaMismatch);
    }
  }
  if (slot->precise_reference_count + slot->initial_reference_precise >=
      header_value->config.precise_reference_capacity) {
    note_bookkeeping_failure(
        *header_value, *slot, TransitionLedgerErrorV1::CapacityExceeded);
    return snapshot_result(*slot, TransitionLedgerErrorV1::CapacityExceeded);
  }
  if (slot->collection_generation ==
      (std::numeric_limits<std::uint32_t>::max)()) {
    note_bookkeeping_failure(
        *header_value, *slot, TransitionLedgerErrorV1::SequenceUnavailable);
    return snapshot_result(
        *slot, TransitionLedgerErrorV1::SequenceUnavailable);
  }
  const std::uint32_t target_index = slot->precise_reference_count + 1U;
  TransitionDependencyReferenceV1 *target =
      reference_at(ledger, *slot, target_index);
  if (target == nullptr) {
    note_diagnostic_failure(
        *header_value, *slot, TransitionLedgerErrorV1::CorruptLayout);
    saturating_increment(
        header_value->metrics.bookkeeping_failure_count);
    return snapshot_result(*slot, TransitionLedgerErrorV1::CorruptLayout);
  }
  *target = reference;
  ++slot->precise_reference_count;
  ++slot->collection_generation;
  slot->coarse = previous_coarse;
  TransitionLedgerOperationResultV1 result = snapshot_result(*slot);
  result.reference_consumed = true;
  return result;
}

/** Make the preallocated emergency dependency sticky for this transaction. */
[[nodiscard]] inline TransitionLedgerOperationResultV1
force_transition_owner_coarse(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionOwnerClaimV1 &claim,
    TransitionLedgerErrorV1 reason,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.owner_locks_valid() ||
      !valid_owner(claim, header_value->config.owner_capacity)) {
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::InvalidArgument,
        .coarse_fallback_required = true,
    };
  }
  OwnerLockGuard lock(callbacks, claim.owner.proc_number);
  SharedTransitionOwnerSlotV1 *slot =
      owner_slot(ledger, claim.owner.proc_number);
  if (slot == nullptr || !claim_matches(*slot, claim)) {
    return slot == nullptr
        ? TransitionLedgerOperationResultV1{
              .claim = claim,
              .error = TransitionLedgerErrorV1::CorruptLayout,
              .bootstrap_failed = true,
          }
        : snapshot_result(*slot, TransitionLedgerErrorV1::StaleOwner);
  }
  if (load_state(*slot) != TransitionOwnerState::Collecting) {
    return snapshot_result(
        *slot, TransitionLedgerErrorV1::InvalidOwnerState);
  }
  note_bookkeeping_failure(*header_value, *slot, reason);
  return snapshot_result(*slot);
}

/**
 * Replace the precise Collecting set with an exact retained subset.
 *
 * This is the bounded savepoint/subtransaction rollback primitive.  Every
 * retained capability must already belong to this owner slot.  Removed owner
 * references are released under ascending dependency partition locks, which
 * the callback releases in reverse.  Any invalid input, overflow, ABA, lock,
 * release, or diagnostic failure makes the emergency coarse binding sticky.
 */
[[nodiscard]] inline TransitionLedgerOperationResultV1
rebuild_transition_dependencies(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionOwnerClaimV1 &claim,
    std::span<const TransitionDependencyReferenceV1> retained,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.owner_locks_valid() ||
      !callbacks.partition_locks_valid() ||
      callbacks.release_owner_reference_locked == nullptr ||
      !valid_owner(claim, header_value->config.owner_capacity)) {
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::InvalidArgument,
        .coarse_fallback_required = true,
    };
  }

  constexpr std::size_t kBindingCapacity =
      static_cast<std::size_t>(kMaximumTransitionReferencesV1) + 1U;
  std::array<bool, kBindingCapacity> keep{};
  std::array<std::uint32_t, kBindingCapacity> partitions{};
  std::uint32_t partition_count = 0;
  std::uint32_t observed_generation = 0;
  std::uint32_t observed_precise_count = 0;
  std::uint32_t previous_coarse = 0;
  SharedTransitionOwnerSlotV1 *slot = nullptr;

  {
    OwnerLockGuard owner_lock(callbacks, claim.owner.proc_number);
    slot = owner_slot(ledger, claim.owner.proc_number);
    if (slot == nullptr) {
      return TransitionLedgerOperationResultV1{
          .claim = claim,
          .error = TransitionLedgerErrorV1::CorruptLayout,
          .bootstrap_failed = true,
      };
    }
    if (!claim_matches(*slot, claim)) {
      return snapshot_result(*slot, TransitionLedgerErrorV1::StaleOwner);
    }
    if (load_state(*slot) != TransitionOwnerState::Collecting) {
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::InvalidOwnerState);
    }
    previous_coarse = slot->coarse;
    slot->coarse = 1;
    observed_precise_count = slot->precise_reference_count;
    if (retained.size() > observed_precise_count ||
        retained.size() >
            header_value->config.precise_reference_capacity ||
        slot->collection_generation ==
            (std::numeric_limits<std::uint32_t>::max)()) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::SubtransactionRebuildFailed);
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::SubtransactionRebuildFailed);
    }

    for (const TransitionDependencyReferenceV1 &wanted : retained) {
      if (!valid_reference(
          wanted, header_value->config.dependency_partition_count)) {
        note_bookkeeping_failure(
            *header_value, *slot,
            TransitionLedgerErrorV1::InvalidDependencyReference);
        return snapshot_result(
            *slot, TransitionLedgerErrorV1::InvalidDependencyReference);
      }
      std::uint32_t matched_index = 0;
      for (std::uint32_t index = 1;
           index <= observed_precise_count; ++index) {
        TransitionDependencyReferenceV1 *existing =
            reference_at(ledger, *slot, index);
        if (existing == nullptr) {
          note_diagnostic_failure(
              *header_value, *slot,
              TransitionLedgerErrorV1::CorruptLayout);
          saturating_increment(
              header_value->metrics.bookkeeping_failure_count);
          return snapshot_result(
              *slot, TransitionLedgerErrorV1::CorruptLayout);
        }
        if (existing->exact_identity_equal(wanted)) {
          matched_index = index;
          break;
        }
      }
      if (matched_index == 0 || keep[matched_index]) {
        note_bookkeeping_failure(
            *header_value, *slot,
            TransitionLedgerErrorV1::SubtransactionRebuildFailed);
        return snapshot_result(
            *slot, TransitionLedgerErrorV1::SubtransactionRebuildFailed);
      }
      keep[matched_index] = true;
    }

    for (std::uint32_t index = 1;
         index <= observed_precise_count; ++index) {
      if (keep[index]) {
        continue;
      }
      TransitionDependencyReferenceV1 *removed =
          reference_at(ledger, *slot, index);
      if (removed == nullptr) {
        note_diagnostic_failure(
            *header_value, *slot,
            TransitionLedgerErrorV1::CorruptLayout);
        saturating_increment(
            header_value->metrics.bookkeeping_failure_count);
        return snapshot_result(
            *slot, TransitionLedgerErrorV1::CorruptLayout);
      }
      if (removed->active) {
        partitions[partition_count++] = removed->partition_index;
      }
    }
    partition_count = sort_and_deduplicate(
        std::span<std::uint32_t>(partitions), partition_count);
    observed_generation = slot->collection_generation;
    if (partition_count == 0 && retained.size() == observed_precise_count) {
      slot->coarse = previous_coarse;
      return snapshot_result(*slot);
    }
  }

  const std::span<const std::uint32_t> locked_partitions(
      partitions.data(), partition_count);
  if (partition_count != 0 && !callbacks.lock_partitions(
      callbacks.context, locked_partitions)) {
    {
      OwnerLockGuard owner_lock(callbacks, claim.owner.proc_number);
      slot = owner_slot(ledger, claim.owner.proc_number);
      if (slot != nullptr && claim_matches(*slot, claim) &&
          load_state(*slot) == TransitionOwnerState::Collecting) {
        note_bookkeeping_failure(
            *header_value, *slot,
            TransitionLedgerErrorV1::PartitionLockFailed);
      }
    }
    if (callbacks.mark_coarse_fallback != nullptr) {
      callbacks.mark_coarse_fallback(
          callbacks.context, TransitionLedgerErrorV1::PartitionLockFailed);
    }
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::PartitionLockFailed,
        .state = TransitionOwnerState::Collecting,
        .coarse_fallback_required = true,
    };
  }

  TransitionLedgerOperationResultV1 result{};
  bool notify_coarse = false;
  callbacks.lock_owner_slot(callbacks.context, claim.owner.proc_number);
  slot = owner_slot(ledger, claim.owner.proc_number);
  if (slot == nullptr || !claim_matches(*slot, claim) ||
      load_state(*slot) != TransitionOwnerState::Collecting ||
      slot->collection_generation != observed_generation ||
      slot->precise_reference_count != observed_precise_count) {
    if (slot != nullptr && claim_matches(*slot, claim)) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::SubtransactionRebuildFailed);
      result = snapshot_result(
          *slot, TransitionLedgerErrorV1::SubtransactionRebuildFailed);
    } else {
      result = TransitionLedgerOperationResultV1{
          .claim = claim,
          .error = TransitionLedgerErrorV1::StaleOwner,
          .coarse_fallback_required = true,
      };
    }
    notify_coarse = true;
  } else {
    bool release_failed = false;
    std::uint32_t diagnostic_failures = 0;
    for (std::uint32_t index = 1;
         index <= observed_precise_count; ++index) {
      if (keep[index]) {
        continue;
      }
      TransitionDependencyReferenceV1 *removed =
          reference_at(ledger, *slot, index);
      if (removed == nullptr) {
        release_failed = true;
        break;
      }
      if (!removed->active) {
        continue;
      }
      const TransitionCallbackResultV1 mutation =
          callbacks.release_owner_reference_locked(
              callbacks.context, removed);
      const bool contract_valid = mutation.applied
          ? !removed->active
          : removed->active;
      if (mutation.applied) {
        removed->active = false;
      }
      release_failed = release_failed || !mutation.applied ||
                       !contract_valid;
      if (!mutation.diagnostic_success) {
        ++diagnostic_failures;
      }
      notify_coarse = notify_coarse ||
                      mutation.coarse_fallback_required ||
                      !mutation.diagnostic_success;
    }

    bool all_removed = true;
    for (std::uint32_t index = 1;
         index <= observed_precise_count; ++index) {
      const TransitionDependencyReferenceV1 *reference =
          reference_at(ledger, *slot, index);
      if (!keep[index] &&
          (reference == nullptr || reference->active)) {
        all_removed = false;
        break;
      }
    }
    release_failed = release_failed || !all_removed;
    if (!release_failed) {
      std::uint32_t target_index = 1;
      for (std::uint32_t index = 1;
           index <= observed_precise_count; ++index) {
        if (!keep[index]) {
          continue;
        }
        TransitionDependencyReferenceV1 *source =
            reference_at(ledger, *slot, index);
        TransitionDependencyReferenceV1 *target =
            reference_at(ledger, *slot, target_index);
        if (source == nullptr || target == nullptr) {
          release_failed = true;
          break;
        }
        if (source != target) {
          *target = *source;
        }
        ++target_index;
      }
      if (!release_failed) {
        for (std::uint32_t index = target_index;
             index < slot->reference_capacity; ++index) {
          TransitionDependencyReferenceV1 *unused =
              reference_at(ledger, *slot, index);
          if (unused == nullptr) {
            release_failed = true;
            break;
          }
          zero_reference(*unused);
        }
      }
      if (!release_failed) {
        slot->precise_reference_count =
            static_cast<std::uint32_t>(retained.size());
        ++slot->collection_generation;
      }
    }

    add_diagnostic_failures(*slot, diagnostic_failures);
    if (diagnostic_failures != 0) {
      saturating_add(
          header_value->metrics.diagnostic_failure_count,
          diagnostic_failures);
    }
    if (release_failed || diagnostic_failures != 0 || notify_coarse) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::SubtransactionRebuildFailed);
      result = snapshot_result(
          *slot, release_failed
              ? TransitionLedgerErrorV1::ReferenceReleaseFailed
              : TransitionLedgerErrorV1::SubtransactionRebuildFailed);
      notify_coarse = true;
    } else {
      slot->coarse = previous_coarse;
      slot->last_error = TransitionLedgerErrorV1::None;
      result = snapshot_result(*slot);
    }
  }
  callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
  if (partition_count != 0) {
    callbacks.unlock_partitions_reverse(
        callbacks.context, locked_partitions);
  }
  if (notify_coarse && callbacks.mark_coarse_fallback != nullptr) {
    callbacks.mark_coarse_fallback(
        callbacks.context,
        TransitionLedgerErrorV1::SubtransactionRebuildFailed);
  }
  return result;
}

/**
 * Publish a bounded transition prefix without holding an owner lock while
 * dependency partitions are acquired.  Any begin failure rolls back every
 * applied prefix member before returning to sticky coarse collection.
 */
[[nodiscard]] inline TransitionLedgerOperationResultV1
begin_owner_transitions(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionOwnerClaimV1 &claim,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.transition_mutations_valid() ||
      !valid_owner(claim, header_value->config.owner_capacity)) {
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::InvalidArgument,
        .coarse_fallback_required = true,
    };
  }
  constexpr std::size_t kBindingCapacity =
      static_cast<std::size_t>(kMaximumTransitionReferencesV1) + 1U;
  std::array<std::uint32_t, kBindingCapacity> binding_indices{};
  std::array<std::uint32_t, kBindingCapacity> partitions{};
  std::uint32_t binding_count = 0;
  std::uint32_t partition_count = 0;
  SharedTransitionOwnerSlotV1 *slot = nullptr;
  {
    OwnerLockGuard owner_lock(callbacks, claim.owner.proc_number);
    slot = owner_slot(ledger, claim.owner.proc_number);
    if (slot == nullptr) {
      return TransitionLedgerOperationResultV1{
          .claim = claim,
          .error = TransitionLedgerErrorV1::CorruptLayout,
          .bootstrap_failed = true,
      };
    }
    if (!claim_matches(*slot, claim)) {
      return snapshot_result(*slot, TransitionLedgerErrorV1::StaleOwner);
    }
    if (load_state(*slot) != TransitionOwnerState::Collecting) {
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::InvalidOwnerState);
    }
    if (slot->coarse != 0) {
      binding_indices[0] = 0;
      binding_count = 1;
    } else {
      binding_count =
          slot->precise_reference_count + slot->initial_reference_precise;
      std::uint32_t target = 0;
      if (slot->initial_reference_precise != 0) {
        binding_indices[target++] = 0;
      }
      for (std::uint32_t index = 0;
           index < slot->precise_reference_count; ++index) {
        binding_indices[target++] = index + 1U;
      }
    }
    if (binding_count == 0) {
      note_bookkeeping_failure(
          *header_value, *slot,
          TransitionLedgerErrorV1::InvalidOwnerState);
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::InvalidOwnerState);
    }
    for (std::uint32_t index = 0; index < binding_count; ++index) {
      TransitionDependencyReferenceV1 *reference =
          reference_at(ledger, *slot, binding_indices[index]);
      if (reference == nullptr ||
          !valid_reference(
              *reference,
              header_value->config.dependency_partition_count)) {
        note_bookkeeping_failure(
            *header_value, *slot,
            TransitionLedgerErrorV1::InvalidDependencyReference);
        return snapshot_result(
            *slot, TransitionLedgerErrorV1::InvalidDependencyReference);
      }
      partitions[index] = reference->partition_index;
    }
    partition_count = sort_and_deduplicate(
        std::span<std::uint32_t>(partitions), binding_count);
    slot->transition_target_count = binding_count;
    store_incremented(*slot, 0);
    store_outstanding(*slot, 0);
    store_state(*slot, TransitionOwnerState::Transitioning);
  }

  const std::span<const std::uint32_t> locked_partitions(
      partitions.data(), partition_count);
  if (!callbacks.lock_partitions(
          callbacks.context, locked_partitions)) {
    {
      OwnerLockGuard owner_lock(callbacks, claim.owner.proc_number);
      slot = owner_slot(ledger, claim.owner.proc_number);
      if (slot != nullptr && claim_matches(*slot, claim) &&
          load_state(*slot) == TransitionOwnerState::Transitioning &&
          load_incremented(*slot) == 0) {
        note_coarse_fallback(
            *header_value, *slot,
            TransitionLedgerErrorV1::PartitionLockFailed);
        slot->transition_target_count = 0;
        store_state(*slot, TransitionOwnerState::Collecting);
      }
    }
    saturating_increment(header_value->metrics.begin_failure_count);
    if (callbacks.mark_coarse_fallback != nullptr) {
      callbacks.mark_coarse_fallback(
          callbacks.context, TransitionLedgerErrorV1::PartitionLockFailed);
    }
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::PartitionLockFailed,
        .state = TransitionOwnerState::Collecting,
      .coarse_fallback_required = true,
    };
  }

  // Global mutation order is dependency partitions, then owner slot.  The
  // owner lock was not held while acquiring partitions and is released first.
  callbacks.lock_owner_slot(callbacks.context, claim.owner.proc_number);
  slot = owner_slot(ledger, claim.owner.proc_number);
  const bool cancelled_by_finish = slot != nullptr &&
      ((load_state(*slot) == TransitionOwnerState::Free &&
        slot->last_claim_sequence == claim.claim_sequence) ||
       (claim_matches(*slot, claim) &&
        load_state(*slot) == TransitionOwnerState::Repairing));
  const TransitionOwnerState cancelled_state = slot == nullptr
      ? TransitionOwnerState::Free
      : load_state(*slot);
  if (slot == nullptr || !claim_matches(*slot, claim) ||
      load_state(*slot) != TransitionOwnerState::Transitioning ||
      load_incremented(*slot) != 0 || load_outstanding(*slot) != 0 ||
      slot->transition_target_count != binding_count) {
    callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
    callbacks.unlock_partitions_reverse(
        callbacks.context, locked_partitions);
    if (cancelled_by_finish) {
      return TransitionLedgerOperationResultV1{
          .claim = claim,
          .error = TransitionLedgerErrorV1::StaleOwner,
          .state = cancelled_state,
      };
    }
    if (callbacks.mark_bootstrap_failed != nullptr) {
      callbacks.mark_bootstrap_failed(
          callbacks.context, TransitionLedgerErrorV1::BootstrapFailed);
    }
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::BootstrapFailed,
        .state = TransitionOwnerState::Repairing,
        .coarse_fallback_required = true,
        .bootstrap_failed = true,
    };
  }

  bool begin_failed = false;
  bool begin_contract_failed = false;
  bool coarse_required = false;
  std::uint32_t diagnostic_failures = 0;
  std::uint32_t applied_count = 0;
  for (std::uint32_t index = 0; index < binding_count; ++index) {
    TransitionDependencyReferenceV1 *reference =
        reference_at(ledger, *slot, binding_indices[index]);
    if (reference == nullptr) {
      begin_failed = true;
      begin_contract_failed = true;
      break;
    }
    const TransitionCallbackResultV1 mutation =
        callbacks.begin_transition_locked(callbacks.context, reference);
    const bool contract_valid = mutation.applied
        ? reference->transition_active
        : !reference->transition_active;
    if (mutation.applied) {
      reference->transition_active = true;
    }
    if (!contract_valid) {
      begin_failed = true;
      begin_contract_failed = true;
      break;
    }
    if (mutation.applied) {
      ++applied_count;
      store_incremented(*slot, applied_count);
      store_outstanding(*slot, applied_count);
      static_cast<void>(increment_current(
          header_value->metrics.current_transition_count,
          header_value->metrics.transition_high_water));
    }
    if (!mutation.diagnostic_success) {
      ++diagnostic_failures;
    }
    coarse_required = coarse_required || mutation.coarse_fallback_required ||
                      !mutation.diagnostic_success;
    if (!mutation.applied || !mutation.diagnostic_success ||
        mutation.coarse_fallback_required) {
      begin_failed = true;
      break;
    }
  }

  DrainResultV1 rollback{};
  if (begin_failed) {
    rollback = drain_locked(
        ledger, *header_value, *slot,
        TransitionResolutionV1::Aborted, false, callbacks);
    diagnostic_failures += rollback.diagnostic_failures;
    coarse_required = true;
  }
  bool bootstrap_failed = rollback.bootstrap_failed || begin_contract_failed;
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;
  if (begin_failed) {
      saturating_increment(header_value->metrics.begin_failure_count);
      add_diagnostic_failures(*slot, diagnostic_failures);
      if (diagnostic_failures != 0) {
        saturating_add(
            header_value->metrics.diagnostic_failure_count,
            diagnostic_failures);
      }
      note_coarse_fallback(
          *header_value, *slot,
          TransitionLedgerErrorV1::DependencyBeginFailed);
      if (rollback.outstanding_transitions == 0 && !bootstrap_failed) {
        store_incremented(*slot, 0);
        store_outstanding(*slot, 0);
        slot->transition_target_count = 0;
        slot->last_error = TransitionLedgerErrorV1::DependencyBeginFailed;
        store_state(*slot, TransitionOwnerState::Collecting);
        error = TransitionLedgerErrorV1::DependencyBeginFailed;
      } else {
        slot->last_error = TransitionLedgerErrorV1::BootstrapFailed;
        slot->repair_forbidden =
            begin_contract_failed || rollback.contract_failure ? 1U : 0U;
        store_state(*slot, TransitionOwnerState::Repairing);
        bootstrap_failed = true;
        error = TransitionLedgerErrorV1::BootstrapFailed;
      }
  } else {
      add_diagnostic_failures(*slot, diagnostic_failures);
      if (diagnostic_failures != 0) {
        saturating_add(
            header_value->metrics.diagnostic_failure_count,
            diagnostic_failures);
      }
      store_state(*slot, TransitionOwnerState::Active);
      error = TransitionLedgerErrorV1::None;
  }
  const TransitionLedgerOperationResultV1 result{
      .claim = claim,
      .error = error,
      .state = bootstrap_failed
          ? TransitionOwnerState::Repairing
          : (begin_failed ? TransitionOwnerState::Collecting
                          : TransitionOwnerState::Active),
      .precise_reference_count =
          slot->precise_reference_count + slot->initial_reference_precise,
      .incremented_count = load_incremented(*slot),
      .outstanding_transition_count = load_outstanding(*slot),
      .diagnostic_failure_count = slot->diagnostic_failure_count,
      .coarse_fallback_required = coarse_required || begin_failed,
      .bootstrap_failed = bootstrap_failed,
  };
  callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
  callbacks.unlock_partitions_reverse(
      callbacks.context, locked_partitions);
  if (coarse_required && callbacks.mark_coarse_fallback != nullptr) {
    callbacks.mark_coarse_fallback(
        callbacks.context,
        error == TransitionLedgerErrorV1::None
            ? TransitionLedgerErrorV1::DependencyBeginFailed
            : error);
  }
  if (bootstrap_failed && callbacks.mark_bootstrap_failed != nullptr) {
    callbacks.mark_bootstrap_failed(
        callbacks.context, TransitionLedgerErrorV1::BootstrapFailed);
  }
  return result;
}

/**
 * Resolve every durable transition and release every retained owner reference.
 * The loop never stops on diagnostic/sequence trouble after an applied
 * decrement.  A true unapplied decrement leaves REPAIRING and fails bootstrap.
 */
[[nodiscard]] inline TransitionLedgerOperationResultV1
finish_owner_transitions(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionOwnerClaimV1 &claim,
    TransitionResolutionV1 resolution,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.transition_mutations_valid() ||
      !valid_owner(claim, header_value->config.owner_capacity) ||
      (resolution != TransitionResolutionV1::Aborted &&
       resolution != TransitionResolutionV1::Committed &&
       resolution != TransitionResolutionV1::KnownAborted)) {
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::InvalidArgument,
        .coarse_fallback_required = true,
    };
  }
  constexpr std::size_t kBindingCapacity =
      static_cast<std::size_t>(kMaximumTransitionReferencesV1) + 1U;
  std::array<std::uint32_t, kBindingCapacity> partitions{};
  std::uint32_t partition_count = 0;
  std::uint32_t expected_incremented = 0;
  std::uint32_t expected_outstanding = 0;
  SharedTransitionOwnerSlotV1 *slot = nullptr;
  {
    OwnerLockGuard owner_lock(callbacks, claim.owner.proc_number);
    slot = owner_slot(ledger, claim.owner.proc_number);
    if (slot == nullptr) {
      return TransitionLedgerOperationResultV1{
          .claim = claim,
          .error = TransitionLedgerErrorV1::CorruptLayout,
          .bootstrap_failed = true,
      };
    }
    if (!claim_matches(*slot, claim)) {
      return snapshot_result(*slot, TransitionLedgerErrorV1::StaleOwner);
    }
    const TransitionOwnerState state = load_state(*slot);
    if (state == TransitionOwnerState::Collecting &&
        resolution == TransitionResolutionV1::Committed) {
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::InvalidOwnerState);
    }
    if (state == TransitionOwnerState::Repairing &&
        slot->repair_forbidden != 0) {
      return snapshot_result(*slot, TransitionLedgerErrorV1::BootstrapFailed);
    }
    if (state != TransitionOwnerState::Collecting &&
        state != TransitionOwnerState::Transitioning &&
        state != TransitionOwnerState::Active &&
        state != TransitionOwnerState::Repairing) {
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::InvalidOwnerState);
    }
    if (state != TransitionOwnerState::Repairing &&
        !compare_exchange_state(
            *slot, state, TransitionOwnerState::Repairing)) {
      return snapshot_result(
          *slot, TransitionLedgerErrorV1::InvalidOwnerState);
    }
    partition_count = collect_partitions(
        ledger, *slot, std::span<std::uint32_t>(partitions), false);
    if (partition_count == kInvalidTransitionOffsetV1) {
      slot->last_error = TransitionLedgerErrorV1::CorruptLayout;
      return snapshot_result(*slot, TransitionLedgerErrorV1::CorruptLayout);
    }
    expected_incremented = load_incremented(*slot);
    expected_outstanding = load_outstanding(*slot);
  }

  const std::span<const std::uint32_t> locked_partitions(
      partitions.data(), partition_count);
  if (partition_count != 0 &&
      !callbacks.lock_partitions(callbacks.context, locked_partitions)) {
    {
      OwnerLockGuard owner_lock(callbacks, claim.owner.proc_number);
      slot = owner_slot(ledger, claim.owner.proc_number);
      if (slot != nullptr && claim_matches(*slot, claim)) {
        note_coarse_fallback(
            *header_value, *slot,
            TransitionLedgerErrorV1::PartitionLockFailed);
      }
    }
    saturating_increment(header_value->metrics.finish_failure_count);
    if (callbacks.mark_bootstrap_failed != nullptr) {
      callbacks.mark_bootstrap_failed(
          callbacks.context, TransitionLedgerErrorV1::PartitionLockFailed);
    }
    return TransitionLedgerOperationResultV1{
        .claim = claim,
        .error = TransitionLedgerErrorV1::PartitionLockFailed,
        .state = TransitionOwnerState::Repairing,
        .incremented_count = expected_incremented,
        .outstanding_transition_count = expected_outstanding,
        .coarse_fallback_required = true,
      .bootstrap_failed = true,
    };
  }
  DrainResultV1 drain{};
  bool bootstrap_failed = false;
  TransitionLedgerErrorV1 error = TransitionLedgerErrorV1::None;
  std::uint32_t precise_count = 0;
  std::uint32_t diagnostics = 0;
  std::uint32_t final_incremented = expected_incremented;
  callbacks.lock_owner_slot(callbacks.context, claim.owner.proc_number);
  slot = owner_slot(ledger, claim.owner.proc_number);
  if (slot == nullptr) {
    bootstrap_failed = true;
    error = TransitionLedgerErrorV1::BootstrapFailed;
  } else if (!claim_matches(*slot, claim)) {
    error = TransitionLedgerErrorV1::StaleOwner;
    final_incremented = 0;
  } else if (load_state(*slot) != TransitionOwnerState::Repairing ||
             load_incremented(*slot) != expected_incremented ||
             load_outstanding(*slot) != expected_outstanding) {
    bootstrap_failed = true;
    error = TransitionLedgerErrorV1::BootstrapFailed;
  } else {
    drain = drain_locked(
        ledger, *header_value, *slot, resolution, true, callbacks);
    const bool fully_drained = drain.outstanding_transitions == 0 &&
                               drain.retained_owner_references == 0;
    bootstrap_failed = drain.bootstrap_failed || !fully_drained;
    error = drain.error;
    precise_count =
        slot->precise_reference_count + slot->initial_reference_precise;
    add_diagnostic_failures(*slot, drain.diagnostic_failures);
    if (drain.diagnostic_failures != 0) {
      saturating_add(
          header_value->metrics.diagnostic_failure_count,
          drain.diagnostic_failures);
    }
    if (drain.coarse_fallback_required ||
        drain.diagnostic_failures != 0) {
      saturating_increment(
          header_value->metrics.coarse_fallback_count);
    }
    diagnostics = slot->diagnostic_failure_count;
    if (fully_drained && !bootstrap_failed) {
      store_incremented(*slot, 0);
      store_outstanding(*slot, 0);
      clear_owner_slot(ledger, *header_value, *slot);
      final_incremented = 0;
    } else {
      note_coarse_fallback(
          *header_value, *slot,
          TransitionLedgerErrorV1::BootstrapFailed);
      slot->repair_forbidden = drain.contract_failure ? 1U : 0U;
      store_state(*slot, TransitionOwnerState::Repairing);
      final_incremented = load_incremented(*slot);
      error = TransitionLedgerErrorV1::BootstrapFailed;
    }
  }
  callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
  if (partition_count != 0) {
    callbacks.unlock_partitions_reverse(
        callbacks.context, locked_partitions);
  }
  if (error != TransitionLedgerErrorV1::None || bootstrap_failed) {
    saturating_increment(header_value->metrics.finish_failure_count);
  }
  if ((drain.coarse_fallback_required || drain.diagnostic_failures != 0) &&
      callbacks.mark_coarse_fallback != nullptr) {
    callbacks.mark_coarse_fallback(
        callbacks.context,
        error == TransitionLedgerErrorV1::None
            ? TransitionLedgerErrorV1::DependencyFinishFailed
            : error);
  }
  if (bootstrap_failed && callbacks.mark_bootstrap_failed != nullptr) {
    callbacks.mark_bootstrap_failed(
        callbacks.context, TransitionLedgerErrorV1::BootstrapFailed);
  }
  return TransitionLedgerOperationResultV1{
      .claim = claim,
      .error = error,
      .state = bootstrap_failed ? TransitionOwnerState::Repairing
                                : TransitionOwnerState::Free,
      .precise_reference_count = precise_count,
      .incremented_count = final_incremented,
      .outstanding_transition_count = drain.outstanding_transitions,
      .diagnostic_failure_count = diagnostics,
      .coarse_fallback_required = drain.coarse_fallback_required ||
                                  drain.diagnostic_failures != 0,
      .bootstrap_failed = bootstrap_failed,
  };
}

/**
 * Sample liveness outside every cache lock, then revalidate the full token.
 * Dead COLLECTING owners with no transition are aborted safely.  Any dead
 * TRANSITIONING/ACTIVE/REPAIRING owner fails bootstrap without guessed repair.
 */
[[nodiscard]] inline TransitionRepairResultV1 repair_dead_transition_owner(
    TransitionLedgerAttachmentV1 ledger,
    std::uint32_t slot_index,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.transition_mutations_valid() ||
      callbacks.owner_is_alive == nullptr ||
      slot_index >= header_value->config.owner_capacity) {
    return TransitionRepairResultV1{
        .error = TransitionLedgerErrorV1::InvalidArgument,
    };
  }
  TransitionOwnerClaimV1 observed{};
  TransitionOwnerState observed_state = TransitionOwnerState::Free;
  std::uint32_t observed_incremented = 0;
  bool fatal_dead_owner = false;
  {
    OwnerLockGuard owner_lock(callbacks, slot_index);
    SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, slot_index);
    if (slot == nullptr) {
      return TransitionRepairResultV1{
          .error = TransitionLedgerErrorV1::CorruptLayout,
          .bootstrap_failed = true,
      };
    }
    observed_state = load_state(*slot);
    if (observed_state == TransitionOwnerState::Free) {
      return TransitionRepairResultV1{
          .error = TransitionLedgerErrorV1::NothingToRepair,
      };
    }
    observed = TransitionOwnerClaimV1{
        .owner = slot->owner,
        .claim_sequence = slot->claim_sequence,
    };
    observed_incremented = load_incremented(*slot);
  }

  if (callbacks.owner_is_alive(callbacks.context, observed.owner)) {
    return TransitionRepairResultV1{
        .observed_claim = observed,
        .error = TransitionLedgerErrorV1::OwnerAlive,
        .disposition = TransitionRepairDispositionV1::OwnerStillAlive,
    };
  }

  {
    OwnerLockGuard owner_lock(callbacks, slot_index);
    SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, slot_index);
    if (slot == nullptr || !claim_matches(*slot, observed) ||
        load_state(*slot) != observed_state ||
        load_incremented(*slot) != observed_incremented) {
      return TransitionRepairResultV1{
          .observed_claim = observed,
          .error = TransitionLedgerErrorV1::StaleOwner,
      };
    }
    if (observed_state != TransitionOwnerState::Collecting ||
        observed_incremented != 0 || load_outstanding(*slot) != 0) {
      note_coarse_fallback(
          *header_value, *slot,
          TransitionLedgerErrorV1::BootstrapFailed);
      fatal_dead_owner = true;
    }
  }
  if (fatal_dead_owner) {
    saturating_increment(header_value->metrics.fatal_owner_count);
    if (callbacks.mark_bootstrap_failed != nullptr) {
      callbacks.mark_bootstrap_failed(
          callbacks.context, TransitionLedgerErrorV1::BootstrapFailed);
    }
    return TransitionRepairResultV1{
        .observed_claim = observed,
        .error = TransitionLedgerErrorV1::BootstrapFailed,
        .disposition =
            TransitionRepairDispositionV1::FatalTransitionOwner,
        .bootstrap_failed = true,
    };
  }

  const TransitionLedgerOperationResultV1 cleanup =
      finish_owner_transitions(
          ledger, observed, TransitionResolutionV1::Aborted, callbacks);
  if (cleanup.bootstrap_failed) {
    saturating_increment(header_value->metrics.fatal_owner_count);
  } else {
    saturating_increment(
        header_value->metrics.repaired_collecting_count);
  }
  return TransitionRepairResultV1{
      .observed_claim = observed,
      .error = cleanup.error,
      .disposition = cleanup.bootstrap_failed
          ? TransitionRepairDispositionV1::FatalTransitionOwner
          : TransitionRepairDispositionV1::ClearedDeadCollecting,
      .bootstrap_failed = cleanup.bootstrap_failed,
  };
}

/** Copy non-FREE slots in increasing ProcNumber order. */
[[nodiscard]] inline TransitionOwnerDiagnosticsResultV1
transition_owner_diagnostics(
    TransitionLedgerAttachmentV1 ledger,
    std::span<TransitionOwnerDiagnosticV1> output,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionOwnerDiagnosticsResultV1 result{};
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.owner_locks_valid()) {
    result.error = TransitionLedgerErrorV1::InvalidArgument;
    return result;
  }
  for (std::uint32_t slot_index = 0;
       slot_index < header_value->config.owner_capacity; ++slot_index) {
    OwnerLockGuard owner_lock(callbacks, slot_index);
    SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, slot_index);
    if (slot == nullptr) {
      result.error = TransitionLedgerErrorV1::CorruptLayout;
      return result;
    }
    const TransitionOwnerState state = load_state(*slot);
    if (state == TransitionOwnerState::Free) {
      continue;
    }
    ++result.total_non_free;
    if (result.copied_count >= output.size()) {
      result.truncated = true;
      continue;
    }
    std::uint32_t retained = 0;
    for (std::uint32_t index = 0;
         index < slot->reference_capacity; ++index) {
      const TransitionDependencyReferenceV1 *reference =
          reference_at(ledger, *slot, index);
      if (reference == nullptr) {
        result.error = TransitionLedgerErrorV1::CorruptLayout;
        return result;
      }
      retained += reference->active ? 1U : 0U;
    }
    output[result.copied_count++] = TransitionOwnerDiagnosticV1{
        .slot_index = slot_index,
        .state = state,
        .owner = slot->owner,
        .claim_sequence = slot->claim_sequence,
        .precise_reference_count =
            slot->precise_reference_count + slot->initial_reference_precise,
        .incremented_count = load_incremented(*slot),
        .outstanding_transition_count = load_outstanding(*slot),
        .retained_owner_reference_count = retained,
        .coarse = slot->coarse,
        .diagnostic_failure_count = slot->diagnostic_failure_count,
        .last_error = slot->last_error,
        .repair_forbidden = slot->repair_forbidden,
    };
  }
  return result;
}

/** Lock-free bounded aggregate snapshot; individual fields may advance. */
[[nodiscard]] inline TransitionLedgerMetricsResultV1
transition_ledger_metrics(
    TransitionLedgerAttachmentV1 ledger) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr ||
      header_value->magic != kTransitionLedgerMagicV1 ||
      header_value->abi_version != kTransitionLedgerAbiVersionV1 ||
      load_bootstrap_state(*header_value) != BootstrapState::Ready) {
    return TransitionLedgerMetricsResultV1{
        .error = TransitionLedgerErrorV1::NotInitialized,
    };
  }
  SharedTransitionLedgerMetricsV1 &shared =
      header_value->metrics;
  return TransitionLedgerMetricsResultV1{
      .metrics = SharedTransitionLedgerMetricsV1{
          .successful_claim_count =
              std::atomic_ref<std::uint64_t>(
                  shared.successful_claim_count).load(
                      std::memory_order_acquire),
          .coarse_fallback_count =
              std::atomic_ref<std::uint64_t>(
                  shared.coarse_fallback_count).load(
                      std::memory_order_acquire),
          .bookkeeping_failure_count =
              std::atomic_ref<std::uint64_t>(
                  shared.bookkeeping_failure_count).load(
                      std::memory_order_acquire),
          .begin_failure_count =
              std::atomic_ref<std::uint64_t>(
                  shared.begin_failure_count).load(
                      std::memory_order_acquire),
          .finish_failure_count =
              std::atomic_ref<std::uint64_t>(
                  shared.finish_failure_count).load(
                      std::memory_order_acquire),
          .diagnostic_failure_count =
              std::atomic_ref<std::uint64_t>(
                  shared.diagnostic_failure_count).load(
                      std::memory_order_acquire),
          .repaired_collecting_count =
              std::atomic_ref<std::uint64_t>(
                  shared.repaired_collecting_count).load(
                      std::memory_order_acquire),
          .fatal_owner_count =
              std::atomic_ref<std::uint64_t>(
                  shared.fatal_owner_count).load(
                      std::memory_order_acquire),
          .current_owner_count =
              std::atomic_ref<std::uint32_t>(
                  shared.current_owner_count).load(
                      std::memory_order_acquire),
          .owner_high_water =
              std::atomic_ref<std::uint32_t>(
                  shared.owner_high_water).load(
                      std::memory_order_acquire),
          .current_transition_count =
              std::atomic_ref<std::uint32_t>(
                  shared.current_transition_count).load(
                      std::memory_order_acquire),
          .transition_high_water =
              std::atomic_ref<std::uint32_t>(
                  shared.transition_high_water).load(
                      std::memory_order_acquire),
      },
  };
}

/** Bounded structural oracle; takes exactly one owner lock at a time. */
[[nodiscard]] inline TransitionLedgerErrorV1
deep_validate_transition_ledger(
    TransitionLedgerAttachmentV1 ledger,
    const TransitionLedgerCallbacksV1 &callbacks) noexcept {
  using namespace transition_ledger_detail;
  TransitionLedgerHeaderV1 *header_value = header(ledger);
  if (header_value == nullptr || !callbacks.owner_locks_valid()) {
    return TransitionLedgerErrorV1::InvalidArgument;
  }
  for (std::uint32_t slot_index = 0;
       slot_index < header_value->config.owner_capacity; ++slot_index) {
    OwnerLockGuard owner_lock(callbacks, slot_index);
    SharedTransitionOwnerSlotV1 *slot = owner_slot(ledger, slot_index);
    if (slot == nullptr || !valid_owner_state(load_state(*slot)) ||
        slot->reference_capacity !=
            header_value->layout.references_per_owner ||
        slot->initial_reference_precise > 1U ||
        slot->precise_reference_count + slot->initial_reference_precise >
            header_value->config.precise_reference_capacity ||
        load_outstanding(*slot) > load_incremented(*slot) ||
        load_incremented(*slot) > slot->transition_target_count ||
        slot->transition_target_count >
            header_value->config.precise_reference_capacity + 1U) {
      return TransitionLedgerErrorV1::CorruptLedger;
    }
    const TransitionOwnerState state = load_state(*slot);
    if (state == TransitionOwnerState::Free) {
      if (slot->claim_sequence != 0 || slot->owner.pid != 0 ||
          slot->precise_reference_count != 0 ||
          slot->initial_reference_precise != 0 ||
          load_incremented(*slot) != 0 || load_outstanding(*slot) != 0 ||
          slot->collection_generation != 0) {
        return TransitionLedgerErrorV1::CorruptLedger;
      }
      for (std::uint32_t index = 0;
           index < slot->reference_capacity; ++index) {
        const TransitionDependencyReferenceV1 *reference =
            reference_at(ledger, *slot, index);
        if (reference == nullptr || !reference_is_zero(*reference)) {
          return TransitionLedgerErrorV1::CorruptLedger;
        }
      }
      continue;
    }
    if (slot->repair_forbidden != 0 &&
        state != TransitionOwnerState::Repairing) {
      return TransitionLedgerErrorV1::CorruptLedger;
    }
    const TransitionOwnerClaimV1 claim{
        .owner = slot->owner,
        .claim_sequence = slot->claim_sequence,
    };
    if (!valid_owner(claim, header_value->config.owner_capacity) ||
        claim.owner.proc_number != slot_index) {
      return TransitionLedgerErrorV1::CorruptLedger;
    }
    std::uint32_t active_transitions = 0;
    for (std::uint32_t index = 0;
         index < slot->reference_capacity; ++index) {
      const TransitionDependencyReferenceV1 *reference =
          reference_at(ledger, *slot, index);
      if (reference == nullptr) {
        return TransitionLedgerErrorV1::CorruptLayout;
      }
      if (reference->transition_active) {
        ++active_transitions;
        if (!reference->active) {
          return TransitionLedgerErrorV1::CorruptLedger;
        }
      }
      if (reference->active &&
          !valid_reference_identity(
              *reference,
              header_value->config.dependency_partition_count)) {
        return TransitionLedgerErrorV1::CorruptLedger;
      }
    }
    if (active_transitions != load_outstanding(*slot)) {
      return TransitionLedgerErrorV1::CorruptLedger;
    }
    if ((state == TransitionOwnerState::Collecting &&
         active_transitions != 0) ||
        (state == TransitionOwnerState::Active &&
         active_transitions != slot->transition_target_count)) {
      return TransitionLedgerErrorV1::CorruptLedger;
    }
  }
  return TransitionLedgerErrorV1::None;
}

}  // namespace bioseq::cache
