/**
 * @file bioseq_caching/pg/dependency_runtime.cppm
 * @brief PostgreSQL lifecycle adapter for generic dependency capabilities.
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/dsa.h"
#include "utils/snapmgr.h"
#include "utils/snapshot.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

export module bioseq.cache.pg.dependency_runtime;

import bioseq.cache.core;
import bioseq.cache.pg.shared_registry;

export namespace bioseq::cache::pg::dependency_runtime {

inline constexpr std::uint32_t kMaximumDirtyDependenciesV1 = 256;
inline constexpr std::uint32_t kMaximumSnapshotTicketDependenciesV1 = 1'024;
inline constexpr std::uint32_t kMaximumDirtyFramesV1 = 256;

enum class DependencyRuntimeErrorV1 : std::uint32_t {
  None = 0,
  InvalidArgument,
  RegistryUnavailable,
  CapacityExceeded,
  NonCanonical,
  ReferenceAcquireFailed,
  ReferenceValidationFailed,
  ReferenceReleaseFailed,
  OwnerClaimFailed,
  OwnerRecordFailed,
  TransitionPublishFailed,
  TransitionFinishFailed,
  SavepointMismatch,
  VectorAllocationFailed,
  VectorValidationFailed,
  LockedActionFailed,
  EmergencyFallback,
};

enum class DependencyRuntimePausePointV1 : std::uint8_t {
  BeforeVisibilityTicket = 1,
  AfterTransitionPublish = 2,
};

/** Utility command whose prepared transaction is about to be resolved. */
enum class ExternalPreparedResolutionKindV1 : std::uint8_t {
  CommitPrepared = 1,
  RollbackPrepared = 2,
};

/** Result known after standard_ProcessUtility returns or throws. */
enum class ExternalPreparedResolutionOutcomeV1 : std::uint8_t {
  Committed = 1,
  RolledBack = 2,
  NotResolved = 3,
  Ambiguous = 4,
};

enum class ExternalPreparedResolutionStateV1 : std::uint8_t {
  Empty = 0,
  Active = 1,
  Finished = 2,
};

/**
 * Full ABA-safe ownership token for one externally resolved prepared write.
 * The directory reference is diagnostic only while active; the transition
 * ledger owns and releases the linear capability.
 */
struct ExternalPreparedResolutionTokenV1 {
  TransitionOwnerClaimV1 claim{};
  DependencyReferenceV1 database_guard{};
  std::uint64_t resolution_identity = 0;
  std::uint32_t database_oid = 0;
  ExternalPreparedResolutionKindV1 kind =
      ExternalPreparedResolutionKindV1::CommitPrepared;
  ExternalPreparedResolutionOutcomeV1 outcome =
      ExternalPreparedResolutionOutcomeV1::NotResolved;
  ExternalPreparedResolutionStateV1 state =
      ExternalPreparedResolutionStateV1::Empty;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return state == ExternalPreparedResolutionStateV1::Active &&
           static_cast<bool>(claim) && database_guard.active &&
           resolution_identity != 0 && database_oid != 0 && reserved == 0;
  }

  [[nodiscard]] bool operator==(
      const ExternalPreparedResolutionTokenV1 &) const noexcept = default;
};

struct DependencyRuntimeTestHooksV1 {
  void *context = nullptr;
  void (*pause)(void *, DependencyRuntimePausePointV1) noexcept = nullptr;
};

struct DependencyRuntimeResultV1 {
  DependencyRuntimeErrorV1 error = DependencyRuntimeErrorV1::None;
  std::uint32_t dependency_count = 0;
  bool fail_open = false;
  bool coarse_fallback_required = false;
  std::array<std::uint8_t, 2> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DependencyRuntimeErrorV1::None;
  }
};

/** Process-local read capabilities; never copy an active ticket. */
struct DependencySnapshotTicketV1 {
  std::array<DependencyReferenceV1,
             kMaximumSnapshotTicketDependenciesV1>
      references{};
  std::array<DependencyVectorEntryV1,
             kMaximumSnapshotTicketDependenciesV1>
      entries{};
  DependencyReferenceV1 conservative_guard_reference{};
  DependencyVectorEntryV1 conservative_guard_entry{};
  std::uint64_t emergency_write_epoch = 0;
  std::uint64_t emergency_change_generation = 0;
  std::uint64_t snapshot_binding_completion_count = 0;
  std::uint64_t snapshot_xact_completion_count = 0;
  std::uint32_t count = 0;
  bool active = false;
  bool requires_completion_guard = false;
  bool explicit_completion_guard = false;
  std::array<std::uint8_t, 1> reserved{};
};

/**
 * Optional registry-side action executed while every dependency partition in
 * a ticket is locked.  The action may acquire registry locks, but it must not
 * call a dependency-directory API; this preserves dependency -> registry lock
 * order.  A successful action must provide an exact rollback operation.
 */
struct DependencyTicketLockedActionV1 {
  void *context = nullptr;
  bool (*apply)(
      void *, std::span<const DependencyVectorEntryV1>) noexcept = nullptr;
  void (*rollback)(void *) noexcept = nullptr;

  [[nodiscard]] bool valid() const noexcept {
    return (apply == nullptr && rollback == nullptr) ||
           (apply != nullptr && rollback != nullptr);
  }
};

/** DSA-owned canonical envelope backed by generation references. */
struct DependencyGenerationVectorV1 {
  DependencyVectorEntryV1 conservative_guard_entry{};
  dsa_pointer envelope = InvalidDsaPointer;
  std::uint32_t bytes = 0;
  std::uint32_t count = 0;
  std::uint32_t released_count = 0;
  std::uint32_t reserved_alignment = 0;
  std::uint64_t checksum = 0;
  std::uint64_t snapshot_xact_completion_count = 0;
  bool active = false;
  bool requires_completion_guard = false;
  bool explicit_completion_guard = false;
  std::array<std::uint8_t, 5> reserved{};
};

/** Lock-dropped release outcome; applied prefix remains authoritative on error. */
struct DependencyGenerationReleaseBatchV1 {
  DependencyRuntimeErrorV1 error = DependencyRuntimeErrorV1::None;
  std::uint32_t dependency_count = 0;
  std::uint32_t reference_count = 0;
  std::uint32_t first_reference = 0;
  std::uint32_t applied_count = 0;
  bool complete = false;
  bool coarse_fallback_required = false;
  std::array<std::uint8_t, 2> reserved{};
};

struct DependencyRuntimeDiagnosticsV1 {
  std::uint64_t ticket_capture_count = 0;
  std::uint64_t ticket_validation_failure_count = 0;
  std::uint64_t generation_handoff_count = 0;
  std::uint64_t generation_release_count = 0;
  std::uint64_t owner_claim_count = 0;
  std::uint64_t transition_publish_count = 0;
  std::uint64_t transition_finish_count = 0;
  std::uint64_t emergency_fallback_count = 0;
  std::uint64_t savepoint_rebuild_count = 0;
  std::uint32_t dirty_dependency_count = 0;
  std::uint32_t dirty_frame_count = 0;
  bool owner_active = false;
  bool transition_published = false;
  bool emergency_transition_published = false;
  bool coarse_fallback_required = false;
};

[[nodiscard]] DependencyRuntimeResultV1 capture_snapshot_ticket(
    std::span<const DependencyVersion> dependencies,
    DependencySnapshotTicketV1 *ticket,
    const DependencyKey *conservative_guard_key,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 validate_snapshot_ticket(
    const DependencySnapshotTicketV1 &ticket) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 validate_snapshot_ticket_locked(
    const DependencySnapshotTicketV1 &ticket,
    DependencyTicketLockedActionV1 action = {}) noexcept;

/**
 * Revalidate a copied canonical vector and optionally apply one atomic action.
 * A nonzero expected completion count binds conservative in-flight work to the
 * producer's snapshot. Zero still performs a fresh snapshot handshake, which
 * lets COMPLETE hits tolerate unrelated rollbacks while their action checks a
 * persisted committed-write epoch/token.
 */
[[nodiscard]] DependencyRuntimeResultV1 validate_dependency_vector_locked(
    std::span<const std::byte> dependency_vector_envelope,
    const DependencyVectorEntryV1 *conservative_guard,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count,
    DependencyTicketLockedActionV1 action = {}) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 release_snapshot_ticket(
    DependencySnapshotTicketV1 *ticket) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 retain_generation_vector(
    DependencySnapshotTicketV1 *ticket,
    DependencyGenerationVectorV1 *generation) noexcept;

/** Acquire Generation refs/envelope while retaining the active Ticket refs. */
[[nodiscard]] DependencyRuntimeResultV1
retain_generation_vector_preserving_ticket(
    const DependencySnapshotTicketV1 &ticket,
    DependencyGenerationVectorV1 *generation) noexcept;

/** Borrow the canonical envelope until the wrapper is released/transferred. */
[[nodiscard]] std::span<const std::byte>
dependency_generation_vector_envelope(
    const DependencyGenerationVectorV1 &generation) noexcept;

/** Free only temporary DSA bytes after registry ownership is established. */
[[nodiscard]] DependencyRuntimeResultV1 transfer_generation_vector_to_registry(
    DependencyGenerationVectorV1 *generation) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 release_generation_vector(
    DependencyGenerationVectorV1 *generation) noexcept;

/** Release a contiguous Generation-reference suffix outside registry locks. */
[[nodiscard]] DependencyGenerationReleaseBatchV1 release_generation_entries(
    std::span<const std::byte> dependency_vector_envelope,
    const DependencyVectorEntryV1 *conservative_guard,
    std::uint32_t first_reference) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 observe_dirty_dependency(
    const DependencyVersion &dependency) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 observe_dirty_dependency(
    const DependencyVersion &dependency,
    const PhysicalIdentityV1 &committed_physical_identity) noexcept;

/** Stage one same-key physical identity replacement for commit. */
[[nodiscard]] DependencyRuntimeResultV1 observe_dirty_dependency(
    const DependencyVersion &observed_dependency,
    const DependencyVersion &committed_dependency) noexcept;

/** Force the root transaction's per-database definition guard. */
[[nodiscard]] DependencyRuntimeResultV1
observe_database_definition_dependency(
    std::uint32_t database_oid) noexcept;

/** Stage one precise object-definition mutation for PRE_COMMIT capture. */
[[nodiscard]] DependencyRuntimeResultV1
observe_object_definition_dependency(
    const ObjectDefinitionMutationV1 &mutation) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 begin_dirty_frame(
    SubTransactionId child, SubTransactionId parent) noexcept;

[[nodiscard]] DependencyRuntimeResultV1 finish_dirty_frame(
    SubTransactionId child, SubTransactionId parent,
    bool committed) noexcept;

[[nodiscard]] DependencyRuntimeResultV1
publish_pre_commit_transitions() noexcept;

[[nodiscard]] DependencyRuntimeResultV1
finish_post_visibility_transitions(bool committed) noexcept;

/** Acquire and publish the database guard before PostgreSQL visibility. */
[[nodiscard]] DependencyRuntimeResultV1
begin_external_prepared_resolution(
    ExternalPreparedResolutionKindV1 kind,
    std::uint32_t database_oid,
    std::uint64_t resolution_identity,
    ExternalPreparedResolutionTokenV1 *token) noexcept;

/** Resolve, drain, generation-publish, wake, and release exactly once. */
[[nodiscard]] DependencyRuntimeResultV1
finish_external_prepared_resolution(
    ExternalPreparedResolutionTokenV1 *token,
    ExternalPreparedResolutionOutcomeV1 outcome) noexcept;

void force_emergency_fallback() noexcept;
void register_dependency_runtime_callbacks() noexcept;
void set_dependency_runtime_test_hooks(
    DependencyRuntimeTestHooksV1 hooks) noexcept;
[[nodiscard]] DependencyRuntimeDiagnosticsV1
dependency_runtime_diagnostics() noexcept;

}  // namespace bioseq::cache::pg::dependency_runtime

namespace bioseq::cache::pg::dependency_runtime::detail {

struct DirtyFrameV1 {
  SubTransactionId child = InvalidSubTransactionId;
  SubTransactionId parent = InvalidSubTransactionId;
  std::uint32_t retained_count = 0;
};

inline std::array<TransitionDependencyReferenceV1,
                  kMaximumDirtyDependenciesV1>
    g_dirty_references{};
inline std::array<DependencyVersion, kMaximumDirtyDependenciesV1>
    g_dirty_versions{};
inline std::array<PhysicalIdentityV1, kMaximumDirtyDependenciesV1>
    g_committed_physical_identities{};
inline std::array<DirtyFrameV1, kMaximumDirtyFramesV1> g_dirty_frames{};
inline std::uint32_t g_dirty_count = 0;
inline std::uint32_t g_dirty_frame_count = 0;
inline TransitionOwnerClaimV1 g_owner_claim{};
inline bool g_owner_active = false;
inline bool g_transition_published = false;
inline bool g_force_coarse = false;
inline bool g_emergency_transition_published = false;
inline bool g_callbacks_registered = false;
inline bool g_external_exit_registered = false;
inline ExternalPreparedResolutionTokenV1 g_external_prepared_token{};
inline DependencyRuntimeTestHooksV1 g_test_hooks{};
inline DependencyRuntimeDiagnosticsV1 g_diagnostics{};
inline ObjectDefinitionDependencyCollectorV1<>
    g_object_definition_dependencies{};
inline bool g_object_definition_pre_commit_captured = false;

static_assert(
    kMaximumObjectDefinitionSpecificDependenciesV1 + 2U ==
    kMaximumDirtyDependenciesV1);

[[nodiscard]] DependencyRuntimeResultV1
validate_snapshot_ticket_locked_impl(
    const DependencySnapshotTicketV1 &ticket,
    DependencyTicketLockedActionV1 action,
    bool completion_lock_already_held) noexcept;

void saturating_increment(std::uint64_t &value) noexcept {
  if (value != (std::numeric_limits<std::uint64_t>::max)()) {
    ++value;
  }
}

void pause(DependencyRuntimePausePointV1 point) noexcept {
  if (g_test_hooks.pause != nullptr) {
    g_test_hooks.pause(g_test_hooks.context, point);
  }
}

[[nodiscard]] const shared_registry::SharedRegistryAttachmentV1 *
attachment() noexcept {
  return shared_registry::current_shared_registry_attachment();
}

void acquire_partition(void *, std::uint32_t partition) noexcept {
  LWLockPadded *locks =
      shared_registry::shared_dependency_partition_locks();
  if (locks != nullptr) {
    LWLockAcquire(&locks[partition].lock, LW_EXCLUSIVE);
  }
}

void release_partition(void *, std::uint32_t partition) noexcept {
  LWLockPadded *locks =
      shared_registry::shared_dependency_partition_locks();
  if (locks != nullptr) {
    LWLockRelease(&locks[partition].lock);
  }
}

[[nodiscard]] std::uint64_t next_sequence(void *) noexcept {
  return shared_registry::next_shared_registry_claim_sequence();
}

[[nodiscard]] bool wake_partition(void *, std::uint32_t) noexcept {
  const shared_registry::SharedRegistryEmergencyAccessV1 emergency =
      shared_registry::shared_emergency_dependency();
  if (emergency.state_changed == nullptr) {
    return false;
  }
  ConditionVariableBroadcast(emergency.state_changed);
  return true;
}

[[nodiscard]] DependencyDirectoryCallbacksV1 directory_callbacks(
    DependencyDirectoryLockModeV1 mode =
        DependencyDirectoryLockModeV1::AcquireRelease) noexcept {
  DependencyDirectoryCallbacksV1 callbacks{
      .lock_mode = mode,
      .next_sequence = next_sequence,
      .wake_partition = wake_partition,
  };
  if (mode == DependencyDirectoryLockModeV1::AcquireRelease) {
    callbacks.acquire_partition = acquire_partition;
    callbacks.release_partition = release_partition;
  }
  return callbacks;
}

[[nodiscard]] DependencyReferenceV1 dependency_reference(
    const TransitionDependencyReferenceV1 &reference) noexcept {
  return DependencyReferenceV1{
      .key = reference.key,
      .record_offset = reference.record_offset,
      .partition_index = reference.partition_index,
      .physical_identity = reference.physical_identity,
      .record_generation = reference.record_generation,
      .kind = DependencyReferenceKindV1::Owner,
      .active = reference.active,
      .transition_active = reference.transition_active,
  };
}

[[nodiscard]] TransitionDependencyReferenceV1 transition_reference(
    const DependencyReferenceV1 &reference) noexcept {
  return TransitionDependencyReferenceV1{
      .key = reference.key,
      .record_offset = reference.record_offset,
      .partition_index = reference.partition_index,
      .physical_identity = reference.physical_identity,
      .record_generation = reference.record_generation,
      .kind = TransitionReferenceKindV1::Owner,
      .active = reference.active,
      .transition_active = reference.transition_active,
  };
}

void lock_owner(void *, std::uint32_t owner) noexcept {
  LWLockPadded *locks = shared_registry::shared_transition_owner_locks();
  if (locks != nullptr) {
    LWLockAcquire(&locks[owner].lock, LW_EXCLUSIVE);
  }
}

void unlock_owner(void *, std::uint32_t owner) noexcept {
  LWLockPadded *locks = shared_registry::shared_transition_owner_locks();
  if (locks != nullptr) {
    LWLockRelease(&locks[owner].lock);
  }
}

[[nodiscard]] bool lock_partitions(
    void *, std::span<const std::uint32_t> partitions) noexcept {
  LWLockPadded *locks =
      shared_registry::shared_dependency_partition_locks();
  if (locks == nullptr) {
    return false;
  }
  for (const std::uint32_t partition : partitions) {
    LWLockAcquire(&locks[partition].lock, LW_EXCLUSIVE);
  }
  return true;
}

void unlock_partitions_reverse(
    void *, std::span<const std::uint32_t> partitions) noexcept {
  LWLockPadded *locks =
      shared_registry::shared_dependency_partition_locks();
  if (locks == nullptr) {
    return;
  }
  for (std::size_t index = partitions.size(); index != 0; --index) {
    LWLockRelease(&locks[partitions[index - 1U]].lock);
  }
}

[[nodiscard]] TransitionCallbackResultV1 begin_transition(
    void *, TransitionDependencyReferenceV1 *reference) noexcept {
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr || reference == nullptr) {
    return {.coarse_fallback_required = true, .error = 1};
  }
  DependencyReferenceV1 capability = dependency_reference(*reference);
  const DependencyMutationResultV1 mutation = begin_dependency_transition(
      registry->dependency, capability,
      directory_callbacks(DependencyDirectoryLockModeV1::AlreadyLocked));
  if (mutation.applied) {
    reference->physical_identity = capability.physical_identity;
    reference->transition_active = capability.transition_active;
  }
  return TransitionCallbackResultV1{
      .applied = mutation.applied,
      .diagnostic_success = mutation.diagnostic_success,
      .coarse_fallback_required = mutation.coarse_fallback_required,
      .error = static_cast<std::uint32_t>(mutation.error),
  };
}

[[nodiscard]] TransitionCallbackResultV1 finish_transition(
    void *, TransitionDependencyReferenceV1 *reference,
    TransitionResolutionV1 resolution) noexcept {
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr || reference == nullptr) {
    return {.coarse_fallback_required = true, .error = 1};
  }
  DependencyReferenceV1 capability = dependency_reference(*reference);
  DependencyPhysicalIdentityActionV1 identity_action =
      DependencyPhysicalIdentityActionV1::Preserve;
  PhysicalIdentityV1 committed_identity = capability.physical_identity;
  if (resolution == TransitionResolutionV1::Committed) {
    for (std::uint32_t index = 0; index < g_dirty_count; ++index) {
      const TransitionDependencyReferenceV1 &dirty =
          g_dirty_references[index];
      if (dirty.key == reference->key &&
          dirty.record_offset == reference->record_offset &&
          dirty.record_generation == reference->record_generation) {
        committed_identity = g_committed_physical_identities[index];
        identity_action = committed_identity == capability.physical_identity
            ? DependencyPhysicalIdentityActionV1::Preserve
            : DependencyPhysicalIdentityActionV1::Replace;
        break;
      }
    }
  }
  const DependencyMutationResultV1 mutation = finish_dependency_transition(
      registry->dependency, capability,
      DependencyTransitionResolutionV1{
          .outcome = resolution == TransitionResolutionV1::Committed
              ? DependencyTransitionOutcomeV1::Committed
              : resolution == TransitionResolutionV1::KnownAborted
                  ? DependencyTransitionOutcomeV1::KnownAborted
                  : DependencyTransitionOutcomeV1::Aborted,
          .physical_identity_action = identity_action,
          .physical_identity = committed_identity,
      },
      directory_callbacks(DependencyDirectoryLockModeV1::AlreadyLocked));
  if (mutation.applied) {
    reference->physical_identity = capability.physical_identity;
    reference->record_generation = capability.record_generation;
    reference->transition_active = capability.transition_active;
  }
  return TransitionCallbackResultV1{
      .applied = mutation.applied,
      .diagnostic_success = mutation.diagnostic_success,
      .coarse_fallback_required = mutation.coarse_fallback_required,
      .error = static_cast<std::uint32_t>(mutation.error),
  };
}

[[nodiscard]] TransitionCallbackResultV1 release_owner_reference(
    void *, TransitionDependencyReferenceV1 *reference) noexcept {
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr || reference == nullptr) {
    return {.coarse_fallback_required = true, .error = 1};
  }
  DependencyReferenceV1 capability = dependency_reference(*reference);
  const DependencyMutationResultV1 mutation = release_dependency_reference(
      registry->dependency, capability,
      directory_callbacks(DependencyDirectoryLockModeV1::AlreadyLocked));
  if (mutation.applied) {
    reference->active = capability.active;
  }
  return TransitionCallbackResultV1{
      .applied = mutation.applied,
      .diagnostic_success = mutation.diagnostic_success,
      .coarse_fallback_required = mutation.coarse_fallback_required,
      .error = static_cast<std::uint32_t>(mutation.error),
  };
}

[[nodiscard]] bool owner_alive(
    void *, const TransitionOwnerIdentityV1 &owner) noexcept {
  return shared_registry::backend_identity_alive(
      shared_registry::SharedRegistryOwnerTokenV1{
          .owner = {
              .proc_number = owner.proc_number,
              .pid = owner.pid,
              .backend_nonce = owner.backend_nonce,
              .backend_started_at_us = owner.backend_started_at,
          },
          .claim_sequence = 1,
      });
}

void mark_coarse(void *, TransitionLedgerErrorV1) noexcept {
  g_force_coarse = true;
  saturating_increment(g_diagnostics.emergency_fallback_count);
}

void mark_failed(void *, TransitionLedgerErrorV1) noexcept {
  g_force_coarse = true;
  saturating_increment(g_diagnostics.emergency_fallback_count);
}

[[nodiscard]] TransitionLedgerCallbacksV1 ledger_callbacks() noexcept {
  return TransitionLedgerCallbacksV1{
      .lock_owner_slot = lock_owner,
      .unlock_owner_slot = unlock_owner,
      .lock_partitions = lock_partitions,
      .unlock_partitions_reverse = unlock_partitions_reverse,
      .begin_transition_locked = begin_transition,
      .finish_transition_and_wake_locked = finish_transition,
      .release_owner_reference_locked = release_owner_reference,
      .owner_is_alive = owner_alive,
      .mark_coarse_fallback = mark_coarse,
      .mark_bootstrap_failed = mark_failed,
  };
}

[[nodiscard]] bool increment_atomic(pg_atomic_uint64 *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::uint64_t observed = pg_atomic_read_u64(value);
  const std::uint64_t maximum =
      (std::numeric_limits<std::uint64_t>::max)();
  while (observed != maximum) {
    if (pg_atomic_compare_exchange_u64(value, &observed, observed + 1U)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool add_atomic(
    pg_atomic_uint64 *value, std::uint64_t amount) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::uint64_t observed = pg_atomic_read_u64(value);
  const std::uint64_t maximum =
      (std::numeric_limits<std::uint64_t>::max)();
  while (amount <= maximum - observed) {
    if (pg_atomic_compare_exchange_u64(value, &observed, observed + amount)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool subtract_atomic(
    pg_atomic_uint64 *value, std::uint64_t amount) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::uint64_t observed = pg_atomic_read_u64(value);
  while (observed >= amount) {
    if (pg_atomic_compare_exchange_u64(value, &observed, observed - amount)) {
      return true;
    }
  }
  return false;
}

/** Hold the PostgreSQL completion counter stable for fallback vectors. */
class CompletionCountGuard final {
 public:
  CompletionCountGuard(bool required, std::uint64_t expected) noexcept
      : required_(required), expected_(expected) {
    if (required_) {
      LWLockAcquire(ProcArrayLock, LW_SHARED);
      locked_ = true;
    }
  }

  CompletionCountGuard(const CompletionCountGuard &) = delete;
  CompletionCountGuard &operator=(const CompletionCountGuard &) = delete;

  ~CompletionCountGuard() {
    if (locked_) {
      LWLockRelease(ProcArrayLock);
    }
  }

  [[nodiscard]] bool matches() const noexcept {
    return !required_ ||
           TransamVariables->xactCompletionCount == expected_;
  }

 private:
  bool required_ = false;
  bool locked_ = false;
  std::uint64_t expected_ = 0;
};

[[nodiscard]] bool increment_atomic(pg_atomic_uint32 *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t observed = pg_atomic_read_u32(value);
  const std::uint32_t maximum =
      (std::numeric_limits<std::uint32_t>::max)();
  while (observed != maximum) {
    if (pg_atomic_compare_exchange_u32(value, &observed, observed + 1U)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool decrement_atomic(pg_atomic_uint32 *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  std::uint32_t observed = pg_atomic_read_u32(value);
  while (observed != 0) {
    if (pg_atomic_compare_exchange_u32(value, &observed, observed - 1U)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool begin_emergency_transition() noexcept {
  const shared_registry::SharedRegistryEmergencyAccessV1 emergency =
      shared_registry::shared_emergency_dependency();
  if (g_emergency_transition_published) {
    return true;
  }
  if (!increment_atomic(emergency.transition_count)) {
    return false;
  }
  g_emergency_transition_published = true;
  const bool generation_ok = increment_atomic(emergency.change_generation);
  if (emergency.state_changed != nullptr) {
    ConditionVariableBroadcast(emergency.state_changed);
  }
  return generation_ok;
}

[[nodiscard]] bool finish_emergency_transition(bool publish_epoch) noexcept {
  const shared_registry::SharedRegistryEmergencyAccessV1 emergency =
      shared_registry::shared_emergency_dependency();
  if (!g_emergency_transition_published) {
    return true;
  }
  const bool epoch_ok = !publish_epoch || increment_atomic(emergency.write_epoch);
  const bool decrement_ok = decrement_atomic(emergency.transition_count);
  const bool generation_ok = increment_atomic(emergency.change_generation);
  if (emergency.state_changed != nullptr) {
    ConditionVariableBroadcast(emergency.state_changed);
  }
  g_emergency_transition_published = false;
  return epoch_ok && decrement_ok && generation_ok;
}

void reset_owner_state() noexcept {
  g_dirty_count = 0;
  g_dirty_frame_count = 0;
  g_owner_claim = {};
  g_owner_active = false;
  g_transition_published = false;
  g_force_coarse = false;
  for (TransitionDependencyReferenceV1 &reference : g_dirty_references) {
    reference = {};
  }
  for (DependencyVersion &version : g_dirty_versions) {
    version = {};
  }
  for (PhysicalIdentityV1 &identity : g_committed_physical_identities) {
    identity = {};
  }
  for (DirtyFrameV1 &frame : g_dirty_frames) {
    frame = {};
  }
  g_object_definition_dependencies.reset();
  g_object_definition_pre_commit_captured = false;
}

[[nodiscard]] bool valid_version(const DependencyVersion &version) noexcept {
  return dependency_vector_entry_valid_v1(
      DependencyVectorEntryV1{
          .version = version,
          .record_generation = 1,
      });
}

[[nodiscard]] DependencyVersion object_definition_version(
    const ObjectDefinitionMutationV1 &mutation,
    const PhysicalIdentityV1 &identity) noexcept {
  return DependencyVersion{
      .key = mutation.key,
      .committed_epoch = 1,
      .physical_identity = identity,
      .observability = database_definition_guard_key_valid_v1(mutation.key)
          ? WriteObservability::DatabaseWideFallback
          : WriteObservability::ExecutorUtilityOnly,
  };
}

[[nodiscard]] bool initialize_object_definition_collector(
    std::uint32_t database_oid,
    SubTransactionId current_subtransaction) noexcept {
  if (database_oid == 0 || database_oid != MyDatabaseId ||
      current_subtransaction == InvalidSubTransactionId) {
    return false;
  }
  if (g_object_definition_dependencies.frame_count() != 0) {
    return g_object_definition_dependencies.guard_key().database_oid ==
           database_oid;
  }
  return g_object_definition_dependencies.initialize(
             database_oid,
             static_cast<std::uint32_t>(current_subtransaction)) ==
         ObjectDefinitionDependencyErrorV1::None;
}

[[nodiscard]] DependencyRuntimeResultV1
ensure_object_definition_guard_reference(
    std::uint32_t database_oid) noexcept {
  const ObjectDefinitionMutationV1 guard =
      make_object_definition_mutation_v1(
          database_definition_guard_key_v1(database_oid));
  return dependency_runtime::observe_dirty_dependency(
      object_definition_version(
          guard, guard.previous_physical_identity));
}

[[nodiscard]] DependencyRuntimeResultV1
ensure_object_definition_write_guard_reference(
    std::uint32_t database_oid) noexcept {
  return dependency_runtime::observe_dirty_dependency(DependencyVersion{
      .key = {
          .database_oid = database_oid,
          .class_oid = 0,
          .object_oid = database_oid,
          .object_sub_id = 0,
          .kind = DependencyKind::DatabaseWriteEpoch,
          .reserved = 0,
      },
      .committed_epoch = 1,
      .physical_identity = {},
      .observability = WriteObservability::DatabaseWideFallback,
  });
}

/** Definition mutations invalidate both choices for the reserved DB guard. */
[[nodiscard]] DependencyRuntimeResultV1
ensure_object_definition_database_guards(
    std::uint32_t database_oid) noexcept {
  DependencyRuntimeResultV1 result =
      ensure_object_definition_guard_reference(database_oid);
  if (!result) {
    return result;
  }
  const DependencyRuntimeResultV1 write =
      ensure_object_definition_write_guard_reference(database_oid);
  if (!write) {
    return write;
  }
  result.dependency_count = write.dependency_count;
  result.coarse_fallback_required =
      result.coarse_fallback_required ||
      write.coarse_fallback_required;
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1
capture_object_definition_dependencies_pre_commit() noexcept {
  DependencyRuntimeResultV1 result{};
  if (g_object_definition_pre_commit_captured) {
    result.dependency_count = g_dirty_count;
    result.coarse_fallback_required =
        g_object_definition_dependencies.guard_only();
    return result;
  }
  g_object_definition_pre_commit_captured = true;
  if (g_object_definition_dependencies.frame_count() == 0 ||
      !g_object_definition_dependencies.transaction_dirty()) {
    result.dependency_count = g_dirty_count;
    return result;
  }

  std::array<ObjectDefinitionMutationV1,
             kMaximumDirtyDependenciesV1>
      mutations{};
  const ObjectDefinitionCopyResultV1 copied =
      g_object_definition_dependencies.copy_committed_dependencies(
          mutations);
  const std::uint32_t database_oid =
      g_object_definition_dependencies.guard_key().database_oid;
  result = ensure_object_definition_database_guards(database_oid);
  if (!result) {
    force_emergency_fallback();
    result.coarse_fallback_required = true;
    return result;
  }
  if (!copied || copied.count == 0 ||
      copied.count > mutations.size()) {
    g_object_definition_dependencies.force_database_definition_guard();
    result.coarse_fallback_required = true;
    result.dependency_count = g_dirty_count;
    return result;
  }

  std::size_t new_specific_count = 0;
  for (std::size_t index = 0; index < copied.count; ++index) {
    const ObjectDefinitionMutationV1 &mutation = mutations[index];
    if (database_definition_guard_key_valid_v1(mutation.key)) {
      continue;
    }
    const bool duplicate = std::any_of(
        g_dirty_versions.begin(),
        g_dirty_versions.begin() + g_dirty_count,
        [&mutation](const DependencyVersion &version) noexcept {
          return version.key == mutation.key;
        });
    new_specific_count += static_cast<std::size_t>(!duplicate);
  }
  if (new_specific_count >
      kMaximumDirtyDependenciesV1 - g_dirty_count) {
    g_object_definition_dependencies.force_database_definition_guard();
    result.dependency_count = g_dirty_count;
    result.coarse_fallback_required = true;
    return result;
  }

  for (std::size_t index = 0; index < copied.count; ++index) {
    const ObjectDefinitionMutationV1 &mutation = mutations[index];
    if (database_definition_guard_key_valid_v1(mutation.key)) {
      continue;
    }
    const DependencyVersion observed = object_definition_version(
        mutation, mutation.previous_physical_identity);
    const DependencyVersion committed = object_definition_version(
        mutation, mutation.current_physical_identity);
    const DependencyRuntimeResultV1 staged =
        dependency_runtime::observe_dirty_dependency(
            observed, committed);
    if (!staged) {
      g_object_definition_dependencies.force_database_definition_guard();
      force_emergency_fallback();
      result = staged;
      result.coarse_fallback_required = true;
      return result;
    }
  }
  result.dependency_count = g_dirty_count;
  result.coarse_fallback_required =
      g_object_definition_dependencies.guard_only();
  return result;
}

/** Capture requests name identity/observability; the directory supplies epoch. */
[[nodiscard]] bool valid_capture_request(
    const DependencyVersion &request) noexcept {
  DependencyVersion shape = request;
  shape.committed_epoch = 1;
  return valid_version(shape);
}

[[nodiscard]] bool conservative_guard_key_valid(
    const DependencyKey &key) noexcept {
  return key.database_oid != 0 && key.class_oid == 0 &&
         key.object_oid == key.database_oid && key.object_sub_id == 0 &&
         (key.kind == DependencyKind::DatabaseWriteEpoch ||
          key.kind == DependencyKind::DatabaseDefinitionEpoch) &&
         key.reserved == 0;
}

[[nodiscard]] bool conservative_guard_entry_valid(
    const DependencyVectorEntryV1 &entry) noexcept {
  return database_guard_entry_valid_v1(entry);
}

[[nodiscard]] bool exact_snapshot(
    const DependencyVectorEntryV1 &expected,
    const DependencyEpochSnapshotV1 &actual) noexcept {
  return actual.key == expected.version.key &&
         actual.physical_identity == expected.version.physical_identity &&
         actual.record_generation == expected.record_generation &&
         actual.committed_epoch == expected.version.committed_epoch &&
         actual.observability == expected.version.observability &&
         actual.transition_count == 0;
}

struct EmergencyEpochSnapshotV1 {
  std::uint64_t write_epoch = 0;
  std::uint64_t change_generation = 0;
};

/** A generation bracket detects begin/finish races around the zero sample. */
[[nodiscard]] bool sample_stable_emergency_epoch(
    EmergencyEpochSnapshotV1 *snapshot) noexcept {
  if (snapshot == nullptr) {
    return false;
  }
  const shared_registry::SharedRegistryEmergencyAccessV1 emergency =
      shared_registry::shared_emergency_dependency();
  if (!emergency) {
    return false;
  }
  const std::uint64_t generation_before =
      pg_atomic_read_u64(emergency.change_generation);
  const std::uint32_t transition_count =
      pg_atomic_read_u32(emergency.transition_count);
  const std::uint64_t write_epoch =
      pg_atomic_read_u64(emergency.write_epoch);
  const std::uint64_t generation_after =
      pg_atomic_read_u64(emergency.change_generation);
  if (generation_before == 0 || generation_before != generation_after ||
      transition_count != 0) {
    return false;
  }
  *snapshot = EmergencyEpochSnapshotV1{
      .write_epoch = write_epoch,
      .change_generation = generation_after,
  };
  return true;
}

[[nodiscard]] bool emergency_epoch_matches(
    const DependencySnapshotTicketV1 &ticket,
    const EmergencyEpochSnapshotV1 &snapshot) noexcept {
  return ticket.emergency_write_epoch == snapshot.write_epoch &&
         ticket.emergency_change_generation == snapshot.change_generation;
}

[[nodiscard]] DependencyRuntimeResultV1 release_ticket_internal(
    DependencySnapshotTicketV1 *ticket) noexcept {
  DependencyRuntimeResultV1 result{};
  if (ticket == nullptr || !ticket->active ||
      ticket->count > kMaximumSnapshotTicketDependenciesV1) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.fail_open = true;
    return result;
  }
  bool failed = false;
  for (std::uint32_t index = 0; index < ticket->count; ++index) {
    if (!ticket->references[index].active) {
      continue;
    }
    const DependencyMutationResultV1 released = release_dependency_reference(
        registry->dependency, ticket->references[index],
        directory_callbacks());
    failed = failed || !released.applied;
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
  }
  if (ticket->conservative_guard_reference.active) {
    const DependencyMutationResultV1 released =
        release_dependency_reference(
            registry->dependency,
            ticket->conservative_guard_reference,
            directory_callbacks());
    failed = failed || !released.applied;
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
  }
  result.dependency_count = ticket->count;
  if (failed) {
    result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
    result.fail_open = true;
  } else {
    *ticket = {};
  }
  return result;
}

void external_prepared_exit(int, Datum) {
  if (g_external_prepared_token.state !=
      ExternalPreparedResolutionStateV1::Active) {
    return;
  }
  static_cast<void>(finish_external_prepared_resolution(
      &g_external_prepared_token,
      ExternalPreparedResolutionOutcomeV1::Ambiguous));
}

void xact_callback(XactEvent event, void *) {
  switch (event) {
    case XACT_EVENT_PRE_COMMIT:
    case XACT_EVENT_PRE_PREPARE:
      static_cast<void>(
          capture_object_definition_dependencies_pre_commit());
      if (g_owner_active || g_force_coarse) {
        static_cast<void>(publish_pre_commit_transitions());
      }
      break;
    case XACT_EVENT_COMMIT:
      static_cast<void>(finish_post_visibility_transitions(true));
      break;
    case XACT_EVENT_PREPARE:
      static_cast<void>(finish_post_visibility_transitions(false));
      break;
    case XACT_EVENT_ABORT:
      static_cast<void>(finish_post_visibility_transitions(false));
      break;
    case XACT_EVENT_PARALLEL_COMMIT:
      static_cast<void>(finish_post_visibility_transitions(true));
      break;
    case XACT_EVENT_PARALLEL_ABORT:
      static_cast<void>(finish_post_visibility_transitions(false));
      break;
    case XACT_EVENT_PARALLEL_PRE_COMMIT:
      break;
  }
}

void subxact_callback(
    SubXactEvent event, SubTransactionId child,
    SubTransactionId parent, void *) {
  switch (event) {
    case SUBXACT_EVENT_START_SUB: {
      static_cast<void>(begin_dirty_frame(child, parent));
      if (initialize_object_definition_collector(
              static_cast<std::uint32_t>(MyDatabaseId), parent)) {
        const ObjectDefinitionDependencyErrorV1 definition_error =
            g_object_definition_dependencies.begin_savepoint(
                static_cast<std::uint32_t>(child),
                static_cast<std::uint32_t>(parent));
        if (definition_error !=
            ObjectDefinitionDependencyErrorV1::None) {
          static_cast<void>(ensure_object_definition_database_guards(
              static_cast<std::uint32_t>(MyDatabaseId)));
        }
      } else {
        force_emergency_fallback();
      }
      break;
    }
    case SUBXACT_EVENT_COMMIT_SUB:
    case SUBXACT_EVENT_ABORT_SUB: {
      const bool committed = event == SUBXACT_EVENT_COMMIT_SUB;
      if (g_object_definition_dependencies.frame_count() > 1) {
        static_cast<void>(
            g_object_definition_dependencies.finish_savepoint(
                static_cast<std::uint32_t>(child),
                static_cast<std::uint32_t>(parent), committed));
      }
      static_cast<void>(finish_dirty_frame(
          child, parent, committed));
      if (g_object_definition_dependencies.transaction_dirty()) {
        const DependencyRuntimeResultV1 guard =
            ensure_object_definition_database_guards(
                static_cast<std::uint32_t>(MyDatabaseId));
        if (!guard) {
          force_emergency_fallback();
        }
      }
      break;
    }
    case SUBXACT_EVENT_PRE_COMMIT_SUB:
      break;
  }
}

}  // namespace bioseq::cache::pg::dependency_runtime::detail

export namespace bioseq::cache::pg::dependency_runtime {

[[nodiscard]] DependencyRuntimeResultV1 capture_snapshot_ticket(
    std::span<const DependencyVersion> dependencies,
    DependencySnapshotTicketV1 *ticket,
    const DependencyKey *conservative_guard_key,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (ticket == nullptr || ticket->active ||
      snapshot_binding_completion_count == 0) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  if (dependencies.size() > kMaximumSnapshotTicketDependenciesV1) {
    result.error = DependencyRuntimeErrorV1::CapacityExceeded;
    result.fail_open = true;
    return result;
  }
  if (conservative_guard_key != nullptr &&
      !conservative_guard_key_valid(*conservative_guard_key)) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    if (!valid_capture_request(dependencies[index])) {
      result.error = DependencyRuntimeErrorV1::InvalidArgument;
      result.fail_open = true;
      return result;
    }
    if (conservative_guard_key != nullptr &&
        dependencies[index].key == *conservative_guard_key) {
      result.error = DependencyRuntimeErrorV1::NonCanonical;
      result.fail_open = true;
      return result;
    }
    if (index != 0 && !dependency_key_less_v1(
        dependencies[index - 1].key, dependencies[index].key)) {
      result.error = DependencyRuntimeErrorV1::NonCanonical;
      result.fail_open = true;
      return result;
    }
  }
  const bool explicit_guard_requires_completion =
      conservative_guard_key != nullptr &&
      database_guard_requires_completion_v1(*conservative_guard_key);
  const bool effective_completion_guard =
      explicit_guard_requires_completion || std::any_of(
          dependencies.begin(), dependencies.end(),
          [](const DependencyVersion &version) noexcept {
            return version.observability ==
                   WriteObservability::DatabaseWideFallback;
          });
  if (continuing_completion_count != 0 &&
      (!effective_completion_guard ||
       continuing_completion_count !=
           snapshot_binding_completion_count)) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.fail_open = true;
    return result;
  }
  Snapshot snapshot = GetActiveSnapshot();
  if (snapshot == nullptr || !IsMVCCSnapshot(snapshot)) {
    result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
    result.fail_open = true;
    return result;
  }
  pause(DependencyRuntimePausePointV1::BeforeVisibilityTicket);
  LWLockAcquire(ProcArrayLock, LW_SHARED);
  const std::uint64_t completion_count =
      TransamVariables->xactCompletionCount;
  if (!dependency_completion_handshake_valid_v1(
          snapshot->snapXactCompletionCount,
          snapshot_binding_completion_count,
          completion_count,
          continuing_completion_count)) {
    LWLockRelease(ProcArrayLock);
    result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
    return result;
  }
  EmergencyEpochSnapshotV1 emergency{};
  if (!sample_stable_emergency_epoch(&emergency)) {
    LWLockRelease(ProcArrayLock);
    result.error = DependencyRuntimeErrorV1::EmergencyFallback;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    return result;
  }
  *ticket = {};
  ticket->emergency_write_epoch = emergency.write_epoch;
  ticket->emergency_change_generation = emergency.change_generation;
  ticket->snapshot_binding_completion_count = completion_count;
  ticket->snapshot_xact_completion_count = continuing_completion_count;
  ticket->explicit_completion_guard = conservative_guard_key != nullptr;
  ticket->requires_completion_guard = effective_completion_guard;
  ticket->active = true;
  for (const DependencyVersion &version : dependencies) {
    const DependencyReferenceResultV1 acquired =
        acquire_dependency_reference(
            registry->dependency,
            DependencyReferenceRequestV1{
                .key = version.key,
                .physical_identity = version.physical_identity,
                .observability = version.observability,
                .kind = DependencyReferenceKindV1::Ticket,
            },
            directory_callbacks());
    if (!acquired) {
      result.error = DependencyRuntimeErrorV1::ReferenceAcquireFailed;
      result.fail_open = true;
      result.coarse_fallback_required =
          acquired.coarse_fallback_required;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      return result;
    }
    const std::uint32_t index = ticket->count++;
    ticket->references[index] = acquired.reference;
    ticket->entries[index] = DependencyVectorEntryV1{
        .version = {
            .key = acquired.snapshot.key,
            .committed_epoch = acquired.snapshot.committed_epoch,
            .physical_identity = acquired.snapshot.physical_identity,
            .observability = acquired.snapshot.observability,
        },
        .record_generation = acquired.snapshot.record_generation,
    };
    if (!exact_snapshot(ticket->entries[index], acquired.snapshot)) {
      result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
      result.fail_open = true;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      saturating_increment(g_diagnostics.ticket_validation_failure_count);
      return result;
    }
  }
  if (conservative_guard_key != nullptr) {
    const DependencyReferenceResultV1 acquired =
        acquire_dependency_reference(
            registry->dependency,
            DependencyReferenceRequestV1{
                .key = *conservative_guard_key,
                .physical_identity = {},
                .observability =
                    WriteObservability::DatabaseWideFallback,
                .kind = DependencyReferenceKindV1::Ticket,
            },
            directory_callbacks());
    if (!acquired) {
      result.error = DependencyRuntimeErrorV1::ReferenceAcquireFailed;
      result.fail_open = true;
      result.coarse_fallback_required = true;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      return result;
    }
    ticket->conservative_guard_reference = acquired.reference;
    ticket->conservative_guard_entry = DependencyVectorEntryV1{
        .version = {
            .key = acquired.snapshot.key,
            .committed_epoch = acquired.snapshot.committed_epoch,
            .physical_identity = acquired.snapshot.physical_identity,
            .observability = acquired.snapshot.observability,
        },
        .record_generation = acquired.snapshot.record_generation,
    };
    if (!conservative_guard_entry_valid(
            ticket->conservative_guard_entry) ||
        !exact_snapshot(ticket->conservative_guard_entry,
                        acquired.snapshot)) {
      result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
      result.fail_open = true;
      result.coarse_fallback_required = true;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      saturating_increment(g_diagnostics.ticket_validation_failure_count);
      return result;
    }
  }
  const DependencyRuntimeResultV1 stable =
      validate_snapshot_ticket_locked_impl(*ticket, {}, true);
  if (!stable) {
    const DependencyRuntimeResultV1 released = release_ticket_internal(ticket);
    result = stable;
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    LWLockRelease(ProcArrayLock);
    return result;
  }
  if (TransamVariables->xactCompletionCount != completion_count) {
    static_cast<void>(release_ticket_internal(ticket));
    LWLockRelease(ProcArrayLock);
    result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
    return result;
  }
  LWLockRelease(ProcArrayLock);
  result.dependency_count = ticket->count;
  saturating_increment(g_diagnostics.ticket_capture_count);
  return result;
}

}  // namespace bioseq::cache::pg::dependency_runtime

namespace bioseq::cache::pg::dependency_runtime::detail {

[[nodiscard]] DependencyRuntimeResultV1
validate_snapshot_ticket_locked_impl(
    const DependencySnapshotTicketV1 &ticket,
    DependencyTicketLockedActionV1 action,
    bool completion_lock_already_held) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  result.dependency_count = ticket.count;
  if (!ticket.active ||
      ticket.count > kMaximumSnapshotTicketDependenciesV1 ||
      ticket.emergency_change_generation == 0 ||
      ticket.snapshot_binding_completion_count == 0 ||
      !action.valid() ||
      std::any_of(
          ticket.reserved.begin(), ticket.reserved.end(),
          [](std::uint8_t value) noexcept { return value != 0; })) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.fail_open = true;
    return result;
  }
  const auto *header = reinterpret_cast<const DependencyDirectoryHeaderV1 *>(
      registry->dependency.region.data());
  if (header == nullptr ||
      header->bootstrap_state != BootstrapState::Ready ||
      header->config.partition_count == 0) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.fail_open = true;
    return result;
  }
  std::array<std::uint32_t,
             kMaximumSnapshotTicketDependenciesV1 + 1U>
      partitions{};
  std::uint32_t partition_count = 0;
  bool requires_completion_guard = false;
  for (std::uint32_t index = 0; index < ticket.count; ++index) {
    if (!ticket.references[index] ||
        ticket.references[index].partition_index >=
            header->config.partition_count ||
        !dependency_vector_entry_valid_v1(ticket.entries[index]) ||
        (index != 0 && !dependency_key_less_v1(
            ticket.entries[index - 1U].version.key,
            ticket.entries[index].version.key))) {
      result.error = DependencyRuntimeErrorV1::InvalidArgument;
      result.fail_open = true;
      return result;
    }
    requires_completion_guard =
        requires_completion_guard ||
        ticket.entries[index].version.observability ==
            WriteObservability::DatabaseWideFallback;
    partitions[partition_count++] =
        ticket.references[index].partition_index;
  }
  if (ticket.explicit_completion_guard) {
    if (!ticket.conservative_guard_reference ||
        ticket.conservative_guard_reference.partition_index >=
            header->config.partition_count ||
        !conservative_guard_entry_valid(
            ticket.conservative_guard_entry)) {
      result.error = DependencyRuntimeErrorV1::InvalidArgument;
      result.fail_open = true;
      return result;
    }
    partitions[partition_count++] =
        ticket.conservative_guard_reference.partition_index;
  } else if (ticket.conservative_guard_reference ||
             ticket.conservative_guard_entry !=
                 DependencyVectorEntryV1{}) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  const bool explicit_guard_requires_completion =
      ticket.explicit_completion_guard &&
      database_guard_requires_completion_v1(
          ticket.conservative_guard_entry);
  if ((explicit_guard_requires_completion || requires_completion_guard) !=
      ticket.requires_completion_guard) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  if (ticket.snapshot_xact_completion_count != 0 &&
      !ticket.requires_completion_guard) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  std::sort(partitions.begin(), partitions.begin() + partition_count);
  partition_count = static_cast<std::uint32_t>(
      std::unique(
          partitions.begin(), partitions.begin() + partition_count) -
      partitions.begin());
  const std::span<const std::uint32_t> locked_partitions(
      partitions.data(), partition_count);
  const bool acquired_completion_lock = !completion_lock_already_held;
  if (acquired_completion_lock) {
    LWLockAcquire(ProcArrayLock, LW_SHARED);
  }
  if (!dependency_completion_handshake_valid_v1(
          0,
          ticket.snapshot_binding_completion_count,
          TransamVariables->xactCompletionCount,
          ticket.snapshot_xact_completion_count)) {
    if (acquired_completion_lock) {
      LWLockRelease(ProcArrayLock);
    }
    result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
    return result;
  }
  if (!lock_partitions(nullptr, locked_partitions)) {
    if (acquired_completion_lock) {
      LWLockRelease(ProcArrayLock);
    }
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    return result;
  }

  bool action_applied = false;
  EmergencyEpochSnapshotV1 before{};
  if (!sample_stable_emergency_epoch(&before) ||
      !emergency_epoch_matches(ticket, before)) {
    result.error = DependencyRuntimeErrorV1::EmergencyFallback;
    result.fail_open = true;
    result.coarse_fallback_required = true;
  }
  for (std::uint32_t index = 0;
       result.error == DependencyRuntimeErrorV1::None &&
       index < ticket.count;
       ++index) {
    const DependencySnapshotResultV1 snapshot = snapshot_dependency(
        registry->dependency, ticket.references[index],
        directory_callbacks(DependencyDirectoryLockModeV1::AlreadyLocked));
    if (!snapshot ||
        !exact_snapshot(ticket.entries[index], snapshot.snapshot)) {
      result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
      result.fail_open = true;
      result.coarse_fallback_required = snapshot.coarse_fallback_required;
    }
  }
  if (result.error == DependencyRuntimeErrorV1::None &&
      ticket.explicit_completion_guard) {
    const DependencySnapshotResultV1 snapshot = snapshot_dependency(
        registry->dependency, ticket.conservative_guard_reference,
        directory_callbacks(DependencyDirectoryLockModeV1::AlreadyLocked));
    if (!snapshot ||
        !exact_snapshot(ticket.conservative_guard_entry,
                        snapshot.snapshot)) {
      result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
      result.fail_open = true;
      result.coarse_fallback_required = true;
    }
  }
  if (result.error == DependencyRuntimeErrorV1::None &&
      action.apply != nullptr) {
    action_applied = action.apply(
        action.context,
        std::span<const DependencyVectorEntryV1>(
            ticket.entries.data(), ticket.count));
    if (!action_applied) {
      result.error = DependencyRuntimeErrorV1::LockedActionFailed;
      result.fail_open = true;
    }
  }
  EmergencyEpochSnapshotV1 after{};
  if (result.error == DependencyRuntimeErrorV1::None &&
      (!sample_stable_emergency_epoch(&after) ||
       !emergency_epoch_matches(ticket, after))) {
    if (action_applied) {
      action.rollback(action.context);
      action_applied = false;
    }
    result.error = DependencyRuntimeErrorV1::EmergencyFallback;
    result.fail_open = true;
    result.coarse_fallback_required = true;
  }
  unlock_partitions_reverse(nullptr, locked_partitions);
  if (acquired_completion_lock) {
    LWLockRelease(ProcArrayLock);
  }
  if (!result) {
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
  }
  return result;
}

}  // namespace bioseq::cache::pg::dependency_runtime::detail

export namespace bioseq::cache::pg::dependency_runtime {

[[nodiscard]] DependencyRuntimeResultV1 validate_snapshot_ticket_locked(
    const DependencySnapshotTicketV1 &ticket,
    DependencyTicketLockedActionV1 action) noexcept {
  return detail::validate_snapshot_ticket_locked_impl(ticket, action, false);
}

[[nodiscard]] DependencyRuntimeResultV1 validate_dependency_vector_locked(
    std::span<const std::byte> dependency_vector_envelope,
    const DependencyVectorEntryV1 *conservative_guard,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count,
    DependencyTicketLockedActionV1 action) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (!action.valid() || snapshot_binding_completion_count == 0) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  if (conservative_guard != nullptr &&
      !conservative_guard_entry_valid(*conservative_guard)) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  const DependencyVectorEnvelopeResultV1 validation =
      validate_dependency_vector_envelope(dependency_vector_envelope);
  if (!validation ||
      validation.header.entry_count >
          kMaximumSnapshotTicketDependenciesV1) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    result.fail_open = true;
    return result;
  }
  result.dependency_count = validation.header.entry_count;
  std::array<DependencyVectorEntryV1,
             kMaximumSnapshotTicketDependenciesV1>
      expected_entries{};
  const DependencyVectorEnvelopeResultV1 decoded =
      copy_dependency_vector_entries(
          dependency_vector_envelope,
          std::span<DependencyVectorEntryV1>(
              expected_entries.data(), result.dependency_count));
  if (!decoded) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    result.fail_open = true;
    return result;
  }
  const bool vector_requires_completion_guard = std::any_of(
      expected_entries.begin(),
      expected_entries.begin() + result.dependency_count,
      [](const DependencyVectorEntryV1 &entry) noexcept {
        return entry.version.observability ==
               WriteObservability::DatabaseWideFallback;
      });
  const bool effective_completion_guard =
      (conservative_guard != nullptr &&
       database_guard_requires_completion_v1(*conservative_guard)) ||
      vector_requires_completion_guard;
  if (continuing_completion_count != 0 &&
      !effective_completion_guard) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }

  std::array<DependencyVersion,
             kMaximumSnapshotTicketDependenciesV1>
      versions{};
  for (std::uint32_t index = 0; index < result.dependency_count; ++index) {
    versions[index] = expected_entries[index].version;
  }
  DependencySnapshotTicketV1 ticket{};
  const DependencyRuntimeResultV1 captured = capture_snapshot_ticket(
      std::span<const DependencyVersion>(
          versions.data(), result.dependency_count),
      &ticket,
      conservative_guard == nullptr
          ? nullptr
          : &conservative_guard->version.key,
      snapshot_binding_completion_count,
      continuing_completion_count);
  if (!captured) {
    return captured;
  }

  const bool exact_entries =
      ticket.count == result.dependency_count &&
      std::equal(expected_entries.begin(),
                 expected_entries.begin() + result.dependency_count,
                 ticket.entries.begin()) &&
      ticket.explicit_completion_guard ==
          (conservative_guard != nullptr) &&
      (conservative_guard == nullptr ||
       ticket.conservative_guard_entry == *conservative_guard);
  if (!exact_entries ||
      ticket.snapshot_binding_completion_count !=
          snapshot_binding_completion_count ||
      ticket.snapshot_xact_completion_count !=
          continuing_completion_count) {
    result.error = DependencyRuntimeErrorV1::ReferenceValidationFailed;
    result.fail_open = true;
    result.coarse_fallback_required = effective_completion_guard;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
  } else {
    result = validate_snapshot_ticket_locked(ticket, action);
  }
  const DependencyRuntimeResultV1 released =
      release_snapshot_ticket(&ticket);
  result.coarse_fallback_required =
      result.coarse_fallback_required ||
      released.coarse_fallback_required;
  if (result && !released) {
    result = released;
  }
  result.dependency_count = validation.header.entry_count;
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1 validate_snapshot_ticket(
    const DependencySnapshotTicketV1 &ticket) noexcept {
  return validate_snapshot_ticket_locked(ticket);
}

[[nodiscard]] DependencyRuntimeResultV1 release_snapshot_ticket(
    DependencySnapshotTicketV1 *ticket) noexcept {
  return detail::release_ticket_internal(ticket);
}

[[nodiscard]] DependencyRuntimeResultV1
retain_generation_vector_preserving_ticket(
    const DependencySnapshotTicketV1 &ticket,
    DependencyGenerationVectorV1 *generation) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (generation == nullptr || generation->active || !ticket.active ||
      ticket.count > kMaximumSnapshotTicketDependenciesV1) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  const DependencyRuntimeResultV1 stable = validate_snapshot_ticket(ticket);
  if (!stable) {
    return stable;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    return {.error = DependencyRuntimeErrorV1::RegistryUnavailable,
            .fail_open = true};
  }
  CompletionCountGuard completion_guard(
      true, ticket.snapshot_binding_completion_count);
  if (!completion_guard.matches()) {
    return {.error = DependencyRuntimeErrorV1::ReferenceValidationFailed,
            .dependency_count = ticket.count,
            .fail_open = true,
            .coarse_fallback_required = true};
  }
  const std::uint64_t snapshot_xact_completion_count =
      ticket.snapshot_xact_completion_count;
  const bool requires_completion_guard =
      ticket.requires_completion_guard;
  const bool explicit_completion_guard =
      ticket.explicit_completion_guard;
  std::array<DependencyReferenceV1,
             kMaximumSnapshotTicketDependenciesV1>
      generation_references{};
  DependencyReferenceV1 generation_guard_reference{};
  std::uint32_t retained_count = 0;
  const auto release_retained_references = [&]() noexcept {
    for (std::uint32_t cleanup = 0; cleanup < retained_count; ++cleanup) {
      static_cast<void>(release_dependency_reference(
          registry->dependency, generation_references[cleanup],
          directory_callbacks()));
    }
    if (generation_guard_reference.active) {
      static_cast<void>(release_dependency_reference(
          registry->dependency, generation_guard_reference,
          directory_callbacks()));
    }
  };
  for (std::uint32_t index = 0; index < ticket.count; ++index) {
    const DependencyVectorEntryV1 &expected = ticket.entries[index];
    const DependencyReferenceResultV1 acquired = acquire_dependency_reference(
        registry->dependency,
        DependencyReferenceRequestV1{
            .key = expected.version.key,
            .physical_identity = expected.version.physical_identity,
            .observability = expected.version.observability,
            .kind = DependencyReferenceKindV1::Generation,
        },
        directory_callbacks());
    if (!acquired || !exact_snapshot(expected, acquired.snapshot)) {
      if (acquired) {
        DependencyReferenceV1 cleanup = acquired.reference;
        static_cast<void>(release_dependency_reference(
            registry->dependency, cleanup, directory_callbacks()));
      }
      release_retained_references();
      return {.error = DependencyRuntimeErrorV1::ReferenceAcquireFailed,
              .dependency_count = retained_count,
              .fail_open = true,
              .coarse_fallback_required =
                  acquired.coarse_fallback_required};
    }
    generation_references[retained_count++] = acquired.reference;
  }
  if (ticket.explicit_completion_guard) {
    const DependencyReferenceResultV1 acquired = acquire_dependency_reference(
        registry->dependency,
        DependencyReferenceRequestV1{
            .key = ticket.conservative_guard_entry.version.key,
            .physical_identity =
                ticket.conservative_guard_entry.version.physical_identity,
            .observability =
                ticket.conservative_guard_entry.version.observability,
            .kind = DependencyReferenceKindV1::Generation,
        },
        directory_callbacks());
    if (!acquired ||
        !exact_snapshot(ticket.conservative_guard_entry,
                        acquired.snapshot)) {
      if (acquired) {
        DependencyReferenceV1 cleanup = acquired.reference;
        static_cast<void>(release_dependency_reference(
            registry->dependency, cleanup, directory_callbacks()));
      }
      release_retained_references();
      return {.error = DependencyRuntimeErrorV1::ReferenceAcquireFailed,
              .dependency_count = retained_count,
              .fail_open = true,
              .coarse_fallback_required = true};
    }
    generation_guard_reference = acquired.reference;
  }
  const std::size_t bytes = dependency_vector_envelope_bytes(ticket.count);
  const dsa_pointer allocation = dsa_allocate_extended(
      registry->area, bytes, DSA_ALLOC_NO_OOM | DSA_ALLOC_ZERO);
  if (!DsaPointerIsValid(allocation)) {
    release_retained_references();
    static_cast<void>(increment_atomic(
        &registry->root->hot_allocation_failures));
    return {.error = DependencyRuntimeErrorV1::VectorAllocationFailed,
            .dependency_count = retained_count,
            .fail_open = true};
  }
  auto *address = static_cast<std::byte *>(
      dsa_get_address(registry->area, allocation));
  const std::span<std::byte> envelope(address, bytes);
  const DependencyVectorEnvelopeResultV1 initialized =
      initialize_dependency_vector_envelope(
          envelope,
          std::span<const DependencyVectorEntryV1>(
              ticket.entries.data(), ticket.count));
  if (!initialized) {
    dsa_free(registry->area, allocation);
    release_retained_references();
    return {.error = DependencyRuntimeErrorV1::VectorValidationFailed,
            .dependency_count = retained_count,
            .fail_open = true};
  }
  if (!add_atomic(&registry->root->hot_live_bytes, bytes)) {
    dsa_free(registry->area, allocation);
    release_retained_references();
    static_cast<void>(increment_atomic(
        &registry->root->hot_allocation_failures));
    return {.error = DependencyRuntimeErrorV1::VectorAllocationFailed,
            .dependency_count = retained_count,
            .fail_open = true,
            .coarse_fallback_required = true};
  }
  const std::uint32_t generation_count = ticket.count;
  result.dependency_count = generation_count;
  generation->envelope = allocation;
  generation->conservative_guard_entry =
      ticket.conservative_guard_entry;
  generation->bytes = static_cast<std::uint32_t>(bytes);
  generation->count = generation_count;
  generation->released_count = 0;
  generation->checksum = initialized.header.payload_checksum;
  generation->snapshot_xact_completion_count =
      snapshot_xact_completion_count;
  generation->active = true;
  generation->requires_completion_guard = requires_completion_guard;
  generation->explicit_completion_guard = explicit_completion_guard;
  saturating_increment(g_diagnostics.generation_handoff_count);
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1 retain_generation_vector(
    DependencySnapshotTicketV1 *ticket,
    DependencyGenerationVectorV1 *generation) noexcept {
  DependencyRuntimeResultV1 result{};
  if (ticket == nullptr) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.fail_open = true;
    return result;
  }
  result = retain_generation_vector_preserving_ticket(*ticket, generation);
  if (!result) {
    return result;
  }
  const DependencyRuntimeResultV1 released_ticket =
      release_snapshot_ticket(ticket);
  result.coarse_fallback_required =
      result.coarse_fallback_required ||
      released_ticket.coarse_fallback_required;
  if (released_ticket) {
    return result;
  }
  const DependencyRuntimeResultV1 released_generation =
      release_generation_vector(generation);
  result.error = released_ticket.error;
  result.fail_open = true;
  result.coarse_fallback_required =
      result.coarse_fallback_required ||
      released_generation.coarse_fallback_required || !released_generation;
  return result;
}

[[nodiscard]] std::span<const std::byte>
dependency_generation_vector_envelope(
    const DependencyGenerationVectorV1 &generation) noexcept {
  using namespace detail;
  const std::uint32_t reference_count =
      generation.count +
      static_cast<std::uint32_t>(generation.explicit_completion_guard);
  if (!generation.active || !DsaPointerIsValid(generation.envelope) ||
      generation.count > kMaximumSnapshotTicketDependenciesV1 ||
      generation.released_count > reference_count ||
      generation.reserved_alignment != 0 ||
      generation.bytes != dependency_vector_envelope_bytes(generation.count) ||
      std::any_of(generation.reserved.begin(), generation.reserved.end(),
                  [](std::uint8_t value) noexcept { return value != 0; })) {
    return {};
  }
  if ((generation.explicit_completion_guard &&
       !conservative_guard_entry_valid(
           generation.conservative_guard_entry)) ||
      (!generation.explicit_completion_guard &&
       generation.conservative_guard_entry != DependencyVectorEntryV1{})) {
    return {};
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    return {};
  }
  const auto *address = static_cast<const std::byte *>(
      dsa_get_address(registry->area, generation.envelope));
  const std::span<const std::byte> envelope(address, generation.bytes);
  const DependencyVectorEnvelopeResultV1 validation =
      validate_dependency_vector_envelope(envelope);
  if (!validation || validation.header.entry_count != generation.count ||
      validation.header.payload_checksum != generation.checksum) {
    return {};
  }
  std::array<DependencyVectorEntryV1,
             kMaximumSnapshotTicketDependenciesV1>
      entries{};
  const DependencyVectorEnvelopeResultV1 decoded =
      copy_dependency_vector_entries(
          envelope,
          std::span<DependencyVectorEntryV1>(entries.data(), generation.count));
  const bool requires_completion_guard =
      decoded && std::any_of(
          entries.begin(), entries.begin() + generation.count,
          [](const DependencyVectorEntryV1 &entry) noexcept {
            return entry.version.observability ==
                   WriteObservability::DatabaseWideFallback;
          });
  const bool explicit_guard_requires_completion =
      generation.explicit_completion_guard &&
      database_guard_requires_completion_v1(
          generation.conservative_guard_entry);
  if (!decoded ||
      (explicit_guard_requires_completion || requires_completion_guard) !=
          generation.requires_completion_guard ||
      (generation.snapshot_xact_completion_count != 0 &&
       !generation.requires_completion_guard)) {
    return {};
  }
  return envelope;
}

[[nodiscard]] DependencyRuntimeResultV1 transfer_generation_vector_to_registry(
    DependencyGenerationVectorV1 *generation) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (generation == nullptr || generation->released_count != 0 ||
      dependency_generation_vector_envelope(*generation).empty()) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    result.fail_open = true;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.fail_open = true;
    return result;
  }
  const std::uint32_t dependency_count = generation->count;
  if (!subtract_atomic(&registry->root->hot_live_bytes, generation->bytes)) {
    result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
    result.dependency_count = dependency_count;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    return result;
  }
  dsa_free(registry->area, generation->envelope);
  *generation = {};
  result.dependency_count = dependency_count;
  return result;
}

[[nodiscard]] DependencyGenerationReleaseBatchV1 release_generation_entries(
    std::span<const std::byte> dependency_vector_envelope,
    const DependencyVectorEntryV1 *conservative_guard,
    std::uint32_t first_reference) noexcept {
  using namespace detail;
  DependencyGenerationReleaseBatchV1 result{
      .first_reference = first_reference,
  };
  const DependencyVectorEnvelopeResultV1 validation =
      validate_dependency_vector_envelope(dependency_vector_envelope);
  if (!validation ||
      (conservative_guard != nullptr &&
       !conservative_guard_entry_valid(*conservative_guard))) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    return result;
  }
  result.dependency_count = validation.header.entry_count;
  result.reference_count =
      result.dependency_count +
      static_cast<std::uint32_t>(conservative_guard != nullptr);
  if (first_reference > result.reference_count) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    return result;
  }
  std::array<DependencyVectorEntryV1,
             kMaximumSnapshotTicketDependenciesV1>
      entries{};
  const DependencyVectorEnvelopeResultV1 decoded =
      copy_dependency_vector_entries(
          dependency_vector_envelope,
          std::span<DependencyVectorEntryV1>(entries.data(),
                                             result.dependency_count));
  if (!decoded) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    return result;
  }
  for (std::uint32_t index = first_reference;
       index < result.reference_count; ++index) {
    const DependencyVectorEntryV1 &entry =
        index < result.dependency_count ? entries[index]
                                        : *conservative_guard;
    const DependencyMutationResultV1 released =
        release_dependency_generation_entry(
            registry->dependency, entry, directory_callbacks());
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    if (!released.applied) {
      result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
      break;
    }
    ++result.applied_count;
    if (released.error != DependencyDirectoryErrorV1::None &&
        result.error == DependencyRuntimeErrorV1::None) {
      result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
    }
  }
  result.complete =
      first_reference + result.applied_count == result.reference_count;
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1 release_generation_vector(
    DependencyGenerationVectorV1 *generation) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (generation == nullptr) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    return result;
  }
  const std::span<const std::byte> envelope =
      dependency_generation_vector_envelope(*generation);
  if (envelope.empty()) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    result.coarse_fallback_required = true;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  if (registry == nullptr) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    return result;
  }
  std::array<DependencyVectorEntryV1,
             kMaximumSnapshotTicketDependenciesV1>
      entries{};
  const DependencyVectorEnvelopeResultV1 decoded =
      copy_dependency_vector_entries(
          envelope,
          std::span<DependencyVectorEntryV1>(
              entries.data(), generation->count));
  if (!decoded || decoded.header.payload_checksum != generation->checksum) {
    result.error = DependencyRuntimeErrorV1::VectorValidationFailed;
    result.coarse_fallback_required = true;
    return result;
  }
  for (std::uint32_t index = generation->released_count;
       index < generation->count; ++index) {
    const DependencyMutationResultV1 released =
        release_dependency_generation_entry(
            registry->dependency, entries[index], directory_callbacks());
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    if (!released.applied) {
      result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
      result.dependency_count = generation->count;
      return result;
    }
    generation->released_count = index + 1U;
  }
  if (generation->explicit_completion_guard &&
      generation->released_count == generation->count) {
    const DependencyMutationResultV1 released =
        release_dependency_generation_entry(
            registry->dependency,
            generation->conservative_guard_entry,
            directory_callbacks());
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    if (!released.applied) {
      result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
      result.dependency_count = generation->count;
      return result;
    }
    ++generation->released_count;
  }
  if (!subtract_atomic(
      &registry->root->hot_live_bytes, generation->bytes)) {
    result.error = DependencyRuntimeErrorV1::ReferenceReleaseFailed;
    result.dependency_count = generation->count;
    result.coarse_fallback_required = true;
    return result;
  }
  dsa_free(registry->area, generation->envelope);
  result.dependency_count = generation->count;
  *generation = {};
  saturating_increment(g_diagnostics.generation_release_count);
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1 observe_dirty_dependency(
    const DependencyVersion &dependency,
    const PhysicalIdentityV1 &committed_physical_identity) noexcept {
  using namespace detail;
  if (!valid_version(dependency) ||
      !physical_identity_valid_v1(committed_physical_identity)) {
    return {.error = DependencyRuntimeErrorV1::InvalidArgument,
            .coarse_fallback_required = true};
  }
  for (std::uint32_t index = 0; index < g_dirty_count; ++index) {
    if (g_dirty_versions[index].key == dependency.key) {
      if (g_dirty_versions[index] == dependency) {
        g_committed_physical_identities[index] =
            committed_physical_identity;
        return {.dependency_count = g_dirty_count,
                .coarse_fallback_required = g_force_coarse};
      }
      force_emergency_fallback();
      return {.error = DependencyRuntimeErrorV1::ReferenceValidationFailed,
              .dependency_count = g_dirty_count,
              .coarse_fallback_required = true};
    }
  }
  if (g_dirty_count >= kMaximumDirtyDependenciesV1) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::CapacityExceeded,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  const shared_registry::SharedRegistryBackendIdentityV1 backend =
      shared_registry::current_backend_identity();
  if (registry == nullptr || !backend.valid()) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::RegistryUnavailable,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const DependencyReferenceResultV1 acquired = acquire_dependency_reference(
      registry->dependency,
      DependencyReferenceRequestV1{
          .key = dependency.key,
          .physical_identity = dependency.physical_identity,
          .observability = dependency.observability,
          .kind = DependencyReferenceKindV1::Owner,
      },
      directory_callbacks());
  if (!acquired) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::ReferenceAcquireFailed,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  DependencyReferenceV1 capability = acquired.reference;
  const TransitionDependencyReferenceV1 ledger_reference =
      transition_reference(capability);
  if (!g_owner_active) {
    const FullTransactionId full_xid = GetTopFullTransactionId();
    const TransitionLedgerOperationResultV1 claimed = claim_transition_owner(
        shared_registry::shared_transition_ledger(),
        TransitionOwnerIdentityV1{
            .proc_number = backend.proc_number,
            .pid = backend.pid,
            .backend_nonce = backend.backend_nonce,
            .backend_started_at = backend.backend_started_at_us,
            .full_xid = U64FromFullTransactionId(full_xid),
        },
        ledger_reference, ledger_callbacks(),
        TransitionInitialReferenceRoleV1::PreciseAndFallback);
    if (!claimed) {
      static_cast<void>(release_dependency_reference(
          registry->dependency, capability, directory_callbacks()));
      force_emergency_fallback();
      return {.error = DependencyRuntimeErrorV1::OwnerClaimFailed,
              .dependency_count = g_dirty_count,
              .coarse_fallback_required = true};
    }
    g_owner_claim = claimed.claim;
    g_owner_active = true;
    saturating_increment(g_diagnostics.owner_claim_count);
  } else {
    const TransitionLedgerOperationResultV1 recorded =
        record_transition_dependency(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            ledger_reference, ledger_callbacks());
    if (!recorded) {
      static_cast<void>(release_dependency_reference(
          registry->dependency, capability, directory_callbacks()));
      force_emergency_fallback();
      return {.error = DependencyRuntimeErrorV1::OwnerRecordFailed,
              .dependency_count = g_dirty_count,
              .coarse_fallback_required = true};
    }
  }
  g_dirty_references[g_dirty_count] = ledger_reference;
  g_dirty_versions[g_dirty_count] = dependency;
  g_committed_physical_identities[g_dirty_count] =
      committed_physical_identity;
  ++g_dirty_count;
  return {.dependency_count = g_dirty_count,
          .coarse_fallback_required = g_force_coarse};
}

[[nodiscard]] DependencyRuntimeResultV1 observe_dirty_dependency(
    const DependencyVersion &dependency) noexcept {
  return observe_dirty_dependency(
      dependency, dependency.physical_identity);
}

[[nodiscard]] DependencyRuntimeResultV1 observe_dirty_dependency(
    const DependencyVersion &observed_dependency,
    const DependencyVersion &committed_dependency) noexcept {
  DependencyVersion observed_shape = observed_dependency;
  DependencyVersion committed_shape = committed_dependency;
  observed_shape.physical_identity = {};
  committed_shape.physical_identity = {};
  if (!detail::valid_version(observed_dependency) ||
      !detail::valid_version(committed_dependency) ||
      observed_shape != committed_shape) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::InvalidArgument,
            .dependency_count = detail::g_dirty_count,
            .coarse_fallback_required = true};
  }
  return observe_dirty_dependency(
      observed_dependency, committed_dependency.physical_identity);
}

[[nodiscard]] DependencyRuntimeResultV1
observe_database_definition_dependency(
    std::uint32_t database_oid) noexcept {
  using namespace detail;
  if (!IsTransactionState() ||
      !initialize_object_definition_collector(
          database_oid, GetCurrentSubTransactionId())) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::InvalidArgument,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  g_object_definition_dependencies.force_database_definition_guard();
  g_object_definition_pre_commit_captured = false;
  DependencyRuntimeResultV1 result =
      ensure_object_definition_database_guards(database_oid);
  result.coarse_fallback_required = true;
  if (!result) {
    force_emergency_fallback();
  }
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1
observe_object_definition_dependency(
    const ObjectDefinitionMutationV1 &mutation) noexcept {
  using namespace detail;
  if (!IsTransactionState() || mutation.key.database_oid == 0 ||
      !initialize_object_definition_collector(
          mutation.key.database_oid,
          GetCurrentSubTransactionId())) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::InvalidArgument,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const ObjectDefinitionObservationResultV1 observed =
      g_object_definition_dependencies.observe(
          static_cast<std::uint32_t>(GetCurrentSubTransactionId()),
          mutation);
  g_object_definition_pre_commit_captured = false;
  if (!observed.root_guard_required) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::InvalidArgument,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  DependencyRuntimeResultV1 result =
      ensure_object_definition_database_guards(
          mutation.key.database_oid);
  result.coarse_fallback_required =
      result.coarse_fallback_required ||
      g_object_definition_dependencies.guard_only();
  if (!result) {
    force_emergency_fallback();
  }
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1 begin_dirty_frame(
    SubTransactionId child, SubTransactionId parent) noexcept {
  using namespace detail;
  if (child == InvalidSubTransactionId ||
      g_dirty_frame_count >= kMaximumDirtyFramesV1) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::CapacityExceeded,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  g_dirty_frames[g_dirty_frame_count++] = DirtyFrameV1{
      .child = child,
      .parent = parent,
      .retained_count = g_dirty_count,
  };
  return {.dependency_count = g_dirty_count,
          .coarse_fallback_required = g_force_coarse};
}

[[nodiscard]] DependencyRuntimeResultV1 finish_dirty_frame(
    SubTransactionId child, SubTransactionId parent,
    bool committed) noexcept {
  using namespace detail;
  if (g_dirty_frame_count == 0) {
    return {.error = DependencyRuntimeErrorV1::SavepointMismatch,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const DirtyFrameV1 frame = g_dirty_frames[g_dirty_frame_count - 1U];
  if (frame.child != child || frame.parent != parent) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::SavepointMismatch,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  --g_dirty_frame_count;
  if (frame.retained_count > g_dirty_count) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::SavepointMismatch,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  if (committed || !g_owner_active || frame.retained_count == g_dirty_count) {
    return {.dependency_count = g_dirty_count,
            .coarse_fallback_required = g_force_coarse};
  }
  if (frame.retained_count == 0) {
    const TransitionLedgerOperationResultV1 cleared =
        finish_owner_transitions(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            TransitionResolutionV1::KnownAborted, ledger_callbacks());
    if (!cleared) {
      force_emergency_fallback();
      return {.error = DependencyRuntimeErrorV1::OwnerRecordFailed,
              .dependency_count = g_dirty_count,
              .coarse_fallback_required = true};
    }
    for (std::uint32_t index = 0; index < g_dirty_count; ++index) {
      g_dirty_references[index] = {};
      g_dirty_versions[index] = {};
      g_committed_physical_identities[index] = {};
    }
    g_dirty_count = 0;
    g_owner_claim = {};
    g_owner_active = false;
    g_transition_published = false;
    saturating_increment(g_diagnostics.savepoint_rebuild_count);
    return {.coarse_fallback_required = g_force_coarse};
  }
  const TransitionLedgerOperationResultV1 rebuilt =
      rebuild_transition_dependencies(
          shared_registry::shared_transition_ledger(), g_owner_claim,
          std::span<const TransitionDependencyReferenceV1>(
              g_dirty_references.data() + 1U,
              frame.retained_count - 1U),
          ledger_callbacks());
  if (!rebuilt) {
    force_emergency_fallback();
    return {.error = DependencyRuntimeErrorV1::OwnerRecordFailed,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  for (std::uint32_t index = frame.retained_count;
       index < g_dirty_count; ++index) {
    g_dirty_references[index] = {};
    g_dirty_versions[index] = {};
    g_committed_physical_identities[index] = {};
  }
  g_dirty_count = frame.retained_count;
  saturating_increment(g_diagnostics.savepoint_rebuild_count);
  return {.dependency_count = g_dirty_count,
          .coarse_fallback_required = g_force_coarse};
}

[[nodiscard]] DependencyRuntimeResultV1
publish_pre_commit_transitions() noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (g_transition_published || g_emergency_transition_published) {
    result.dependency_count = g_dirty_count;
    result.coarse_fallback_required = g_force_coarse;
    return result;
  }
  if (g_owner_active) {
    const TransitionLedgerOperationResultV1 published =
        begin_owner_transitions(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            ledger_callbacks());
    if (published) {
      g_transition_published = true;
      saturating_increment(g_diagnostics.transition_publish_count);
    } else {
      g_force_coarse = true;
      result.error = DependencyRuntimeErrorV1::TransitionPublishFailed;
      result.coarse_fallback_required = true;
    }
  }
  if (g_force_coarse && !begin_emergency_transition()) {
    result.error = DependencyRuntimeErrorV1::EmergencyFallback;
    result.coarse_fallback_required = true;
  }
  pause(DependencyRuntimePausePointV1::AfterTransitionPublish);
  result.dependency_count = g_dirty_count;
  result.coarse_fallback_required =
      result.coarse_fallback_required || g_force_coarse;
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1
finish_post_visibility_transitions(bool committed) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (committed && (g_owner_active || g_force_coarse) &&
      !g_transition_published && !g_emergency_transition_published) {
    g_force_coarse = true;
    static_cast<void>(begin_emergency_transition());
  }
  if (g_owner_active) {
    const TransitionResolutionV1 resolution =
        g_transition_published
        ? (committed ? TransitionResolutionV1::Committed
                     : TransitionResolutionV1::KnownAborted)
        : TransitionResolutionV1::KnownAborted;
    const TransitionLedgerOperationResultV1 finished =
        finish_owner_transitions(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            resolution, ledger_callbacks());
    if (!finished) {
      result.error = DependencyRuntimeErrorV1::TransitionFinishFailed;
      result.coarse_fallback_required = true;
      g_force_coarse = true;
    }
  }
  if (!finish_emergency_transition(committed)) {
    result.error = DependencyRuntimeErrorV1::EmergencyFallback;
    result.coarse_fallback_required = true;
  }
  result.dependency_count = g_dirty_count;
  if (g_owner_active || g_transition_published || g_force_coarse) {
    saturating_increment(g_diagnostics.transition_finish_count);
  }
  reset_owner_state();
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1
begin_external_prepared_resolution(
    ExternalPreparedResolutionKindV1 kind,
    std::uint32_t database_oid,
    std::uint64_t resolution_identity,
    ExternalPreparedResolutionTokenV1 *token) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  const bool kind_valid =
      kind == ExternalPreparedResolutionKindV1::CommitPrepared ||
      kind == ExternalPreparedResolutionKindV1::RollbackPrepared;
  if (token == nullptr ||
      token->state != ExternalPreparedResolutionStateV1::Empty ||
      !kind_valid || database_oid == 0 || resolution_identity == 0) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.coarse_fallback_required = true;
    return result;
  }
  if (g_owner_active || g_dirty_count != 0 ||
      g_emergency_transition_published ||
      g_external_prepared_token.state ==
          ExternalPreparedResolutionStateV1::Active) {
    result.error = DependencyRuntimeErrorV1::OwnerClaimFailed;
    result.coarse_fallback_required = true;
    return result;
  }
  const shared_registry::SharedRegistryAttachmentV1 *registry = attachment();
  const shared_registry::SharedRegistryBackendIdentityV1 backend =
      shared_registry::current_backend_identity();
  if (registry == nullptr || !backend.valid()) {
    result.error = DependencyRuntimeErrorV1::RegistryUnavailable;
    result.coarse_fallback_required = true;
    return result;
  }
  DependencyReferenceResultV1 acquired = acquire_dependency_reference(
      registry->dependency,
      DependencyReferenceRequestV1{
          .key = DependencyKey{
              .database_oid = database_oid,
              .class_oid = 0,
              .object_oid = database_oid,
              .object_sub_id = 0,
              .kind = DependencyKind::DatabaseWriteEpoch,
          },
          .physical_identity = {},
          .observability = WriteObservability::DatabaseWideFallback,
          .kind = DependencyReferenceKindV1::Owner,
      },
      directory_callbacks());
  if (!acquired) {
    force_emergency_fallback();
    result.error = DependencyRuntimeErrorV1::ReferenceAcquireFailed;
    result.coarse_fallback_required = true;
    return result;
  }
  DependencyReferenceV1 capability = acquired.reference;
  const TransitionLedgerOperationResultV1 claimed = claim_transition_owner(
      shared_registry::shared_transition_ledger(),
      TransitionOwnerIdentityV1{
          .proc_number = backend.proc_number,
          .pid = backend.pid,
          .backend_nonce = backend.backend_nonce,
          .backend_started_at = backend.backend_started_at_us,
          .full_xid = resolution_identity,
      },
      transition_reference(capability), ledger_callbacks(),
      TransitionInitialReferenceRoleV1::PreciseAndFallback);
  if (!claimed) {
    static_cast<void>(release_dependency_reference(
        registry->dependency, capability, directory_callbacks()));
    force_emergency_fallback();
    result.error = DependencyRuntimeErrorV1::OwnerClaimFailed;
    result.coarse_fallback_required = true;
    return result;
  }
  const TransitionLedgerOperationResultV1 published =
      begin_owner_transitions(
          shared_registry::shared_transition_ledger(), claimed.claim,
          ledger_callbacks());
  if (!published) {
    const TransitionLedgerOperationResultV1 drained =
        finish_owner_transitions(
            shared_registry::shared_transition_ledger(), claimed.claim,
            TransitionResolutionV1::Aborted, ledger_callbacks());
    force_emergency_fallback();
    const bool emergency_started = begin_emergency_transition();
    const bool emergency_finished =
        emergency_started && finish_emergency_transition(true);
    result.error = DependencyRuntimeErrorV1::TransitionPublishFailed;
    result.coarse_fallback_required = true;
    if (!drained || !emergency_finished) {
      result.error = DependencyRuntimeErrorV1::EmergencyFallback;
    }
    return result;
  }
  if (!g_external_exit_registered) {
    before_shmem_exit(external_prepared_exit, Datum{0});
    g_external_exit_registered = true;
  }
  *token = ExternalPreparedResolutionTokenV1{
      .claim = claimed.claim,
      .database_guard = capability,
      .resolution_identity = resolution_identity,
      .database_oid = database_oid,
      .kind = kind,
      .outcome = ExternalPreparedResolutionOutcomeV1::NotResolved,
      .state = ExternalPreparedResolutionStateV1::Active,
  };
  g_external_prepared_token = *token;
  result.dependency_count = 1;
  result.coarse_fallback_required = g_force_coarse;
  return result;
}

[[nodiscard]] DependencyRuntimeResultV1
finish_external_prepared_resolution(
    ExternalPreparedResolutionTokenV1 *token,
    ExternalPreparedResolutionOutcomeV1 outcome) noexcept {
  using namespace detail;
  DependencyRuntimeResultV1 result{};
  if (token == nullptr) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.coarse_fallback_required = true;
    return result;
  }
  if (token->state == ExternalPreparedResolutionStateV1::Finished) {
    if (token->outcome != outcome) {
      result.error = DependencyRuntimeErrorV1::InvalidArgument;
      result.coarse_fallback_required = true;
      return result;
    }
    result.dependency_count = 1;
    result.coarse_fallback_required =
        outcome == ExternalPreparedResolutionOutcomeV1::Ambiguous;
    return result;
  }
  const bool outcome_valid =
      (token->kind == ExternalPreparedResolutionKindV1::CommitPrepared &&
       (outcome == ExternalPreparedResolutionOutcomeV1::Committed ||
        outcome == ExternalPreparedResolutionOutcomeV1::NotResolved ||
        outcome == ExternalPreparedResolutionOutcomeV1::Ambiguous)) ||
      (token->kind == ExternalPreparedResolutionKindV1::RollbackPrepared &&
       (outcome == ExternalPreparedResolutionOutcomeV1::RolledBack ||
        outcome == ExternalPreparedResolutionOutcomeV1::NotResolved ||
        outcome == ExternalPreparedResolutionOutcomeV1::Ambiguous));
  const bool exact_active_token =
      static_cast<bool>(*token) && outcome_valid &&
      g_external_prepared_token.state ==
          ExternalPreparedResolutionStateV1::Active &&
      token->claim == g_external_prepared_token.claim &&
      token->database_guard ==
          g_external_prepared_token.database_guard &&
      token->resolution_identity ==
          g_external_prepared_token.resolution_identity &&
      token->database_oid == g_external_prepared_token.database_oid &&
      token->kind == g_external_prepared_token.kind;
  if (!exact_active_token) {
    result.error = DependencyRuntimeErrorV1::InvalidArgument;
    result.coarse_fallback_required = true;
    return result;
  }
  const TransitionResolutionV1 resolution =
      outcome == ExternalPreparedResolutionOutcomeV1::Committed
      ? TransitionResolutionV1::Committed
      : outcome == ExternalPreparedResolutionOutcomeV1::Ambiguous
          ? TransitionResolutionV1::Aborted
          : TransitionResolutionV1::KnownAborted;
  const TransitionLedgerOperationResultV1 finished =
      finish_owner_transitions(
          shared_registry::shared_transition_ledger(), token->claim,
          resolution, ledger_callbacks());
  const bool fully_drained =
      finished.state == TransitionOwnerState::Free &&
      finished.outstanding_transition_count == 0;
  bool emergency_ok = true;
  if (outcome == ExternalPreparedResolutionOutcomeV1::Ambiguous ||
      !fully_drained || !finished) {
    force_emergency_fallback();
    const bool emergency_started = begin_emergency_transition();
    emergency_ok =
        emergency_started && finish_emergency_transition(true);
  }
  result.dependency_count = 1;
  result.coarse_fallback_required =
      finished.coarse_fallback_required ||
      outcome == ExternalPreparedResolutionOutcomeV1::Ambiguous ||
      !fully_drained || !finished || !emergency_ok;
  if (!fully_drained || !finished) {
    result.error = DependencyRuntimeErrorV1::TransitionFinishFailed;
  }
  if (!emergency_ok) {
    result.error = DependencyRuntimeErrorV1::EmergencyFallback;
  }
  if (!fully_drained) {
    return result;
  }
  const bool global_token = token == &g_external_prepared_token;
  token->outcome = outcome;
  token->state = ExternalPreparedResolutionStateV1::Finished;
  g_external_prepared_token = {};
  if (global_token) {
    return result;
  }
  return result;
}

void force_emergency_fallback() noexcept {
  using namespace detail;
  if (!g_force_coarse) {
    saturating_increment(g_diagnostics.emergency_fallback_count);
  }
  g_force_coarse = true;
}

void register_dependency_runtime_callbacks() noexcept {
  if (detail::g_callbacks_registered) {
    return;
  }
  RegisterXactCallback(detail::xact_callback, nullptr);
  RegisterSubXactCallback(detail::subxact_callback, nullptr);
  detail::g_callbacks_registered = true;
}

void set_dependency_runtime_test_hooks(
    DependencyRuntimeTestHooksV1 hooks) noexcept {
  detail::g_test_hooks = hooks;
}

[[nodiscard]] DependencyRuntimeDiagnosticsV1
dependency_runtime_diagnostics() noexcept {
  using namespace detail;
  DependencyRuntimeDiagnosticsV1 result = g_diagnostics;
  result.dirty_dependency_count = g_dirty_count;
  result.dirty_frame_count = g_dirty_frame_count;
  result.owner_active = g_owner_active;
  result.transition_published = g_transition_published;
  result.emergency_transition_published =
      g_emergency_transition_published;
  result.coarse_fallback_required = g_force_coarse;
  return result;
}

}  // namespace bioseq::cache::pg::dependency_runtime
