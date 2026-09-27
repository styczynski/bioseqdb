/**
 * @file bioseq_caching/pg/dependency_runtime.cppm
 * @brief Connects cache dependency tracking to PostgreSQL snapshots and transactions.
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

inline constexpr std::uint32_t k_maximum_dirty_dependencies = 256;
inline constexpr std::uint32_t k_maximum_snapshot_ticket_dependencies = 1'024;
inline constexpr std::uint32_t k_maximum_dirty_frames = 256;

enum class dependency_runtime_error : std::uint32_t {
  none = 0,
  invalid_argument,
  registry_unavailable,
  capacity_exceeded,
  non_canonical,
  reference_acquire_failed,
  reference_validation_failed,
  reference_release_failed,
  owner_claim_failed,
  owner_record_failed,
  transition_publish_failed,
  transition_finish_failed,
  savepoint_mismatch,
  vector_allocation_failed,
  vector_validation_failed,
  locked_action_failed,
  emergency_fallback,
};

enum class dependency_runtime_pause_point : std::uint8_t {
  before_visibility_ticket = 1,
  after_transition_publish = 2,
};

/** Prepared-transaction command being processed. */
enum class external_prepared_resolution_kind : std::uint8_t {
  commit_prepared = 1,
  rollback_prepared = 2,
};

/** Outcome of the prepared-transaction command, including an uncertain result. */
enum class external_prepared_resolution_outcome : std::uint8_t {
  committed = 1,
  rolled_back = 2,
  not_resolved = 3,
  ambiguous = 4,
};

enum class external_prepared_resolution_state : std::uint8_t {
  empty = 0,
  active = 1,
  finished = 2,
};

/**
 * Identifies the cache transition for a prepared-transaction command.
 * The ledger owns the dependency reference while this token is active.
 * `database_guard` is a copy for identity checks and diagnostics, so releasing
 * it separately would release the same reference twice.
 */
struct external_prepared_resolution_token {
  transition_owner_claim claim{};
  dependency_reference database_guard{};
  std::uint64_t resolution_identity = 0;
  std::uint32_t database_oid = 0;
  external_prepared_resolution_kind kind =
      external_prepared_resolution_kind::commit_prepared;
  external_prepared_resolution_outcome outcome =
      external_prepared_resolution_outcome::not_resolved;
  external_prepared_resolution_state state =
      external_prepared_resolution_state::empty;
  std::uint8_t reserved = 0;

  [[nodiscard]] explicit operator bool() const noexcept {
    return state == external_prepared_resolution_state::active &&
           static_cast<bool>(claim) && database_guard.active &&
           resolution_identity != 0 && database_oid != 0 && reserved == 0;
  }

  [[nodiscard]] bool operator==(
      const external_prepared_resolution_token &) const noexcept = default;
};

struct dependency_runtime_test_hooks {
  void *context = nullptr;
  void (*pause)(void *, dependency_runtime_pause_point) noexcept = nullptr;
};

struct dependency_runtime_result {
  dependency_runtime_error error = dependency_runtime_error::none;
  std::uint32_t dependency_count = 0;
  bool fail_open = false;
  bool coarse_fallback_required = false;
  std::array<std::uint8_t, 2> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == dependency_runtime_error::none;
  }
};

/** Holds dependency references for one snapshot check. Copying an active ticket would duplicate ownership. */
struct dependency_snapshot_ticket {
  std::array<dependency_reference,
             k_maximum_snapshot_ticket_dependencies>
      references{};
  std::array<dependency_vector_entry,
             k_maximum_snapshot_ticket_dependencies>
      entries{};
  dependency_reference conservative_guard_reference{};
  dependency_vector_entry conservative_guard_entry{};
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
 * Optional action run with every ticket dependency partition locked.
 * It may take registry locks. It must avoid dependency-directory calls, which
 * would acquire locks in the wrong order. Supply a rollback callback so the
 * caller can undo a successful action if the final dependency check fails.
 */
struct dependency_ticket_locked_action {
  void *context = nullptr;
  bool (*apply)(
      void *, std::span<const dependency_vector_entry>) noexcept = nullptr;
  void (*rollback)(void *) noexcept = nullptr;

  [[nodiscard]] bool valid() const noexcept {
    return (apply == nullptr && rollback == nullptr) ||
           (apply != nullptr && rollback != nullptr);
  }
};

/** Serialized dependencies in PostgreSQL shared storage, with references held for a cache generation. */
struct dependency_generation_vector {
  dependency_vector_entry conservative_guard_entry{};
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

/** Records how many references were released, including releases completed before an error. */
struct dependency_generation_release_batch {
  dependency_runtime_error error = dependency_runtime_error::none;
  std::uint32_t dependency_count = 0;
  std::uint32_t reference_count = 0;
  std::uint32_t first_reference = 0;
  std::uint32_t applied_count = 0;
  bool complete = false;
  bool coarse_fallback_required = false;
  std::array<std::uint8_t, 2> reserved{};
};

struct dependency_runtime_diagnostics {
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

[[nodiscard]] dependency_runtime_result capture_snapshot_ticket(
    std::span<const dependency_version> dependencies,
    dependency_snapshot_ticket *ticket,
    const dependency_key *conservative_guard_key,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count) noexcept;

[[nodiscard]] dependency_runtime_result validate_snapshot_ticket(
    const dependency_snapshot_ticket &ticket) noexcept;

[[nodiscard]] dependency_runtime_result validate_snapshot_ticket_locked(
    const dependency_snapshot_ticket &ticket,
    dependency_ticket_locked_action action = {}) noexcept;

/**
 * Check serialized dependency versions against the bound snapshot, then run the
 * optional action while dependency locks are held. A nonzero continuing count
 * also ties an unfinished build to its original transaction-completion count.
 * With zero, completed hits use the current snapshot binding and committed
 * dependency versions, so unrelated rollbacks need not discard them.
 */
[[nodiscard]] dependency_runtime_result validate_dependency_vector_locked(
    std::span<const std::byte> dependency_vector_envelope,
    const dependency_vector_entry *conservative_guard,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count,
    dependency_ticket_locked_action action = {}) noexcept;

[[nodiscard]] dependency_runtime_result release_snapshot_ticket(
    dependency_snapshot_ticket *ticket) noexcept;

[[nodiscard]] dependency_runtime_result retain_generation_vector(
    dependency_snapshot_ticket *ticket,
    dependency_generation_vector *generation) noexcept;

/** Create generation references and serialized storage while keeping the snapshot ticket active. */
[[nodiscard]] dependency_runtime_result
retain_generation_vector_preserving_ticket(
    const dependency_snapshot_ticket &ticket,
    dependency_generation_vector *generation) noexcept;

/** Borrow the validated dependency bytes until the generation is released or transferred. */
[[nodiscard]] std::span<const std::byte>
dependency_generation_vector_envelope(
    const dependency_generation_vector &generation) noexcept;

/** Free the temporary serialized copy after the registry takes responsibility for its references. */
[[nodiscard]] dependency_runtime_result transfer_generation_vector_to_registry(
    dependency_generation_vector *generation) noexcept;

[[nodiscard]] dependency_runtime_result release_generation_vector(
    dependency_generation_vector *generation) noexcept;

/** Release references from `first_reference` onward. The caller must hold no registry locks. */
[[nodiscard]] dependency_generation_release_batch release_generation_entries(
    std::span<const std::byte> dependency_vector_envelope,
    const dependency_vector_entry *conservative_guard,
    std::uint32_t first_reference) noexcept;

[[nodiscard]] dependency_runtime_result observe_dirty_dependency(
    const dependency_version &dependency) noexcept;

[[nodiscard]] dependency_runtime_result observe_dirty_dependency(
    const dependency_version &dependency,
    const physical_identity &committed_physical_identity) noexcept;

/** Record a dependency whose physical identity will change on commit. All other fields must match. */
[[nodiscard]] dependency_runtime_result observe_dirty_dependency(
    const dependency_version &observed_dependency,
    const dependency_version &committed_dependency) noexcept;

/** Use database-wide invalidation for definition changes in this transaction. */
[[nodiscard]] dependency_runtime_result
observe_database_definition_dependency(
    std::uint32_t database_oid) noexcept;

/** Record an object-definition change for dependency capture before commit. */
[[nodiscard]] dependency_runtime_result
observe_object_definition_dependency(
    const object_definition_mutation &mutation) noexcept;

[[nodiscard]] dependency_runtime_result begin_dirty_frame(
    SubTransactionId child, SubTransactionId parent) noexcept;

[[nodiscard]] dependency_runtime_result finish_dirty_frame(
    SubTransactionId child, SubTransactionId parent,
    bool committed) noexcept;

[[nodiscard]] dependency_runtime_result
publish_pre_commit_transitions() noexcept;

[[nodiscard]] dependency_runtime_result
finish_post_visibility_transitions(bool committed) noexcept;

/** Start the database write transition before resolving a prepared transaction. */
[[nodiscard]] dependency_runtime_result
begin_external_prepared_resolution(
    external_prepared_resolution_kind kind,
    std::uint32_t database_oid,
    std::uint64_t resolution_identity,
    external_prepared_resolution_token *token) noexcept;

/** Finish the prepared-transaction transition and release its reference. Repeating the same completed outcome is safe. */
[[nodiscard]] dependency_runtime_result
finish_external_prepared_resolution(
    external_prepared_resolution_token *token,
    external_prepared_resolution_outcome outcome) noexcept;

void force_emergency_fallback() noexcept;
void register_dependency_runtime_callbacks() noexcept;
void set_dependency_runtime_test_hooks(
    dependency_runtime_test_hooks hooks) noexcept;
[[nodiscard]] dependency_runtime_diagnostics
read_dependency_runtime_diagnostics() noexcept;

}  // namespace bioseq::cache::pg::dependency_runtime

namespace bioseq::cache::pg::dependency_runtime::detail {

struct dirty_frame {
  SubTransactionId child = InvalidSubTransactionId;
  SubTransactionId parent = InvalidSubTransactionId;
  std::uint32_t retained_count = 0;
};

inline std::array<transition_dependency_reference,
                  k_maximum_dirty_dependencies>
    g_dirty_references{};
inline std::array<dependency_version, k_maximum_dirty_dependencies>
    g_dirty_versions{};
inline std::array<physical_identity, k_maximum_dirty_dependencies>
    g_committed_physical_identities{};
inline std::array<dirty_frame, k_maximum_dirty_frames> g_dirty_frames{};
inline std::uint32_t g_dirty_count = 0;
inline std::uint32_t g_dirty_frame_count = 0;
inline transition_owner_claim g_owner_claim{};
inline bool g_owner_active = false;
inline bool g_transition_published = false;
inline bool g_force_coarse = false;
inline bool g_emergency_transition_published = false;
inline bool g_callbacks_registered = false;
inline bool g_external_exit_registered = false;
inline external_prepared_resolution_token g_external_prepared_token{};
inline dependency_runtime_test_hooks g_test_hooks{};
inline dependency_runtime_diagnostics g_diagnostics{};
inline object_definition_dependency_collector<>
    g_object_definition_dependencies{};
inline bool g_object_definition_pre_commit_captured = false;

static_assert(
    k_maximum_object_definition_specific_dependencies + 2U ==
    k_maximum_dirty_dependencies);

[[nodiscard]] dependency_runtime_result
validate_snapshot_ticket_locked_impl(
    const dependency_snapshot_ticket &ticket,
    dependency_ticket_locked_action action,
    bool completion_lock_already_held) noexcept;

void saturating_increment(std::uint64_t &value) noexcept {
  if (value != (std::numeric_limits<std::uint64_t>::max)()) {
    ++value;
  }
}

void pause(dependency_runtime_pause_point point) noexcept {
  if (g_test_hooks.pause != nullptr) {
    g_test_hooks.pause(g_test_hooks.context, point);
  }
}

[[nodiscard]] const shared_registry::shared_registry_attachment *
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
  const shared_registry::shared_registry_emergency_access emergency =
      shared_registry::shared_emergency_dependency();
  if (emergency.state_changed == nullptr) {
    return false;
  }
  ConditionVariableBroadcast(emergency.state_changed);
  return true;
}

[[nodiscard]] dependency_directory_callbacks directory_callbacks(
    dependency_directory_lock_mode mode =
        dependency_directory_lock_mode::acquire_release) noexcept {
  dependency_directory_callbacks callbacks{
      .lock_mode = mode,
      .next_sequence = next_sequence,
      .wake_partition = wake_partition,
  };
  if (mode == dependency_directory_lock_mode::acquire_release) {
    callbacks.acquire_partition = acquire_partition;
    callbacks.release_partition = release_partition;
  }
  return callbacks;
}

[[nodiscard]] dependency_reference dependency_reference_from_transition(
    const transition_dependency_reference &reference) noexcept {
  return dependency_reference{
      .key = reference.key,
      .record_offset = reference.record_offset,
      .partition_index = reference.partition_index,
      .physical_identity = reference.physical_identity,
      .record_generation = reference.record_generation,
      .kind = dependency_reference_kind::owner,
      .active = reference.active,
      .transition_active = reference.transition_active,
  };
}

[[nodiscard]] transition_dependency_reference transition_reference(
    const dependency_reference &reference) noexcept {
  return transition_dependency_reference{
      .key = reference.key,
      .record_offset = reference.record_offset,
      .partition_index = reference.partition_index,
      .physical_identity = reference.physical_identity,
      .record_generation = reference.record_generation,
      .kind = transition_reference_kind::owner,
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

[[nodiscard]] transition_callback_result begin_transition(
    void *, transition_dependency_reference *reference) noexcept {
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr || reference == nullptr) {
    return {.coarse_fallback_required = true, .error = 1};
  }
  dependency_reference capability = dependency_reference_from_transition(*reference);
  const dependency_mutation_result mutation = begin_dependency_transition(
      registry->dependency, capability,
      directory_callbacks(dependency_directory_lock_mode::already_locked));
  if (mutation.applied) {
    reference->physical_identity = capability.physical_identity;
    reference->transition_active = capability.transition_active;
  }
  return transition_callback_result{
      .applied = mutation.applied,
      .diagnostic_success = mutation.diagnostic_success,
      .coarse_fallback_required = mutation.coarse_fallback_required,
      .error = static_cast<std::uint32_t>(mutation.error),
  };
}

[[nodiscard]] transition_callback_result finish_transition(
    void *, transition_dependency_reference *reference,
    transition_resolution resolution) noexcept {
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr || reference == nullptr) {
    return {.coarse_fallback_required = true, .error = 1};
  }
  dependency_reference capability = dependency_reference_from_transition(*reference);
  dependency_physical_identity_action identity_action =
      dependency_physical_identity_action::preserve;
  physical_identity committed_identity = capability.physical_identity;
  if (resolution == transition_resolution::committed) {
    for (std::uint32_t index = 0; index < g_dirty_count; ++index) {
      const transition_dependency_reference &dirty =
          g_dirty_references[index];
      if (dirty.key == reference->key &&
          dirty.record_offset == reference->record_offset &&
          dirty.record_generation == reference->record_generation) {
        committed_identity = g_committed_physical_identities[index];
        identity_action = committed_identity == capability.physical_identity
            ? dependency_physical_identity_action::preserve
            : dependency_physical_identity_action::replace;
        break;
      }
    }
  }
  const dependency_mutation_result mutation = finish_dependency_transition(
      registry->dependency, capability,
      dependency_transition_resolution{
          .outcome = resolution == transition_resolution::committed
              ? dependency_transition_outcome::committed
              : resolution == transition_resolution::known_aborted
                  ? dependency_transition_outcome::known_aborted
                  : dependency_transition_outcome::aborted,
          .physical_identity_action = identity_action,
          .physical_identity = committed_identity,
      },
      directory_callbacks(dependency_directory_lock_mode::already_locked));
  if (mutation.applied) {
    reference->physical_identity = capability.physical_identity;
    reference->record_generation = capability.record_generation;
    reference->transition_active = capability.transition_active;
  }
  return transition_callback_result{
      .applied = mutation.applied,
      .diagnostic_success = mutation.diagnostic_success,
      .coarse_fallback_required = mutation.coarse_fallback_required,
      .error = static_cast<std::uint32_t>(mutation.error),
  };
}

[[nodiscard]] transition_callback_result release_owner_reference(
    void *, transition_dependency_reference *reference) noexcept {
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr || reference == nullptr) {
    return {.coarse_fallback_required = true, .error = 1};
  }
  dependency_reference capability = dependency_reference_from_transition(*reference);
  const dependency_mutation_result mutation = release_dependency_reference(
      registry->dependency, capability,
      directory_callbacks(dependency_directory_lock_mode::already_locked));
  if (mutation.applied) {
    reference->active = capability.active;
  }
  return transition_callback_result{
      .applied = mutation.applied,
      .diagnostic_success = mutation.diagnostic_success,
      .coarse_fallback_required = mutation.coarse_fallback_required,
      .error = static_cast<std::uint32_t>(mutation.error),
  };
}

[[nodiscard]] bool owner_alive(
    void *, const transition_owner_identity &owner) noexcept {
  return shared_registry::backend_identity_alive(
      shared_registry::shared_registry_owner_token{
          .owner = {
              .proc_number = owner.proc_number,
              .pid = owner.pid,
              .backend_nonce = owner.backend_nonce,
              .backend_started_at_us = owner.backend_started_at,
          },
          .claim_sequence = 1,
      });
}

void mark_coarse(void *, transition_ledger_error) noexcept {
  g_force_coarse = true;
  saturating_increment(g_diagnostics.emergency_fallback_count);
}

void mark_failed(void *, transition_ledger_error) noexcept {
  g_force_coarse = true;
  saturating_increment(g_diagnostics.emergency_fallback_count);
}

[[nodiscard]] transition_ledger_callbacks ledger_callbacks() noexcept {
  return transition_ledger_callbacks{
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
  uint64 observed = pg_atomic_read_u64(value);
  const uint64 maximum = (std::numeric_limits<uint64>::max)();
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
  uint64 observed = pg_atomic_read_u64(value);
  const uint64 native_amount = static_cast<uint64>(amount);
  const uint64 maximum = (std::numeric_limits<uint64>::max)();
  while (native_amount <= maximum - observed) {
    if (pg_atomic_compare_exchange_u64(
            value, &observed, observed + native_amount)) {
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
  uint64 observed = pg_atomic_read_u64(value);
  const uint64 native_amount = static_cast<uint64>(amount);
  while (observed >= native_amount) {
    if (pg_atomic_compare_exchange_u64(
            value, &observed, observed - native_amount)) {
      return true;
    }
  }
  return false;
}

/** Hold ProcArrayLock so the transaction-completion count stays fixed during validation. */
class completion_count_guard final {
 public:
  completion_count_guard(bool required, std::uint64_t expected) noexcept
      : required_(required), expected_(expected) {
    if (required_) {
      LWLockAcquire(ProcArrayLock, LW_SHARED);
      locked_ = true;
    }
  }

  completion_count_guard(const completion_count_guard &) = delete;
  completion_count_guard &operator=(const completion_count_guard &) = delete;

  ~completion_count_guard() {
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
  const shared_registry::shared_registry_emergency_access emergency =
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
  const shared_registry::shared_registry_emergency_access emergency =
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
  for (transition_dependency_reference &reference : g_dirty_references) {
    reference = {};
  }
  for (dependency_version &version : g_dirty_versions) {
    version = {};
  }
  for (physical_identity &identity : g_committed_physical_identities) {
    identity = {};
  }
  for (dirty_frame &frame : g_dirty_frames) {
    frame = {};
  }
  g_object_definition_dependencies.reset();
  g_object_definition_pre_commit_captured = false;
}

[[nodiscard]] bool valid_version(const dependency_version &version) noexcept {
  return dependency_vector_entry_valid(
      dependency_vector_entry{
          .version = version,
          .record_generation = 1,
      });
}

[[nodiscard]] dependency_version object_definition_version(
    const object_definition_mutation &mutation,
    const physical_identity &identity) noexcept {
  return dependency_version{
      .key = mutation.key,
      .committed_epoch = 1,
      .physical_identity = identity,
      .observability = database_definition_guard_key_valid(mutation.key)
          ? write_observability::database_wide_fallback
          : write_observability::executor_utility_only,
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
         object_definition_dependency_error::none;
}

[[nodiscard]] dependency_runtime_result
ensure_object_definition_guard_reference(
    std::uint32_t database_oid) noexcept {
  const object_definition_mutation guard =
      make_object_definition_mutation(
          database_definition_guard_key(database_oid));
  return dependency_runtime::observe_dirty_dependency(
      object_definition_version(
          guard, guard.previous_physical_identity));
}

[[nodiscard]] dependency_runtime_result
ensure_object_definition_write_guard_reference(
    std::uint32_t database_oid) noexcept {
  return dependency_runtime::observe_dirty_dependency(dependency_version{
      .key = {
          .database_oid = database_oid,
          .class_oid = 0,
          .object_oid = database_oid,
          .object_sub_id = 0,
          .kind = dependency_kind::database_write_epoch,
          .reserved = 0,
      },
      .committed_epoch = 1,
      .physical_identity = {},
      .observability = write_observability::database_wide_fallback,
  });
}

/** Record definition changes against both database versions, for writes and definitions. */
[[nodiscard]] dependency_runtime_result
ensure_object_definition_database_guards(
    std::uint32_t database_oid) noexcept {
  dependency_runtime_result result =
      ensure_object_definition_guard_reference(database_oid);
  if (!result) {
    return result;
  }
  const dependency_runtime_result write =
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

[[nodiscard]] dependency_runtime_result
capture_object_definition_dependencies_pre_commit() noexcept {
  dependency_runtime_result result{};
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

  std::array<object_definition_mutation,
             k_maximum_dirty_dependencies>
      mutations{};
  const object_definition_copy_result copied =
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
    const object_definition_mutation &mutation = mutations[index];
    if (database_definition_guard_key_valid(mutation.key)) {
      continue;
    }
    const bool duplicate = std::any_of(
        g_dirty_versions.begin(),
        g_dirty_versions.begin() + g_dirty_count,
        [&mutation](const dependency_version &version) noexcept {
          return version.key == mutation.key;
        });
    new_specific_count += static_cast<std::size_t>(!duplicate);
  }
  if (new_specific_count >
      k_maximum_dirty_dependencies - g_dirty_count) {
    g_object_definition_dependencies.force_database_definition_guard();
    result.dependency_count = g_dirty_count;
    result.coarse_fallback_required = true;
    return result;
  }

  for (std::size_t index = 0; index < copied.count; ++index) {
    const object_definition_mutation &mutation = mutations[index];
    if (database_definition_guard_key_valid(mutation.key)) {
      continue;
    }
    const dependency_version observed = object_definition_version(
        mutation, mutation.previous_physical_identity);
    const dependency_version committed = object_definition_version(
        mutation, mutation.current_physical_identity);
    const dependency_runtime_result staged =
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

/** Validate the requested identity and write tracking. The directory supplies the current epoch. */
[[nodiscard]] bool valid_capture_request(
    const dependency_version &request) noexcept {
  dependency_version shape = request;
  shape.committed_epoch = 1;
  return valid_version(shape);
}

[[nodiscard]] bool conservative_guard_key_valid(
    const dependency_key &key) noexcept {
  return key.database_oid != 0 && key.class_oid == 0 &&
         key.object_oid == key.database_oid && key.object_sub_id == 0 &&
         (key.kind == dependency_kind::database_write_epoch ||
          key.kind == dependency_kind::database_definition_epoch) &&
         key.reserved == 0;
}

[[nodiscard]] bool conservative_guard_entry_valid(
    const dependency_vector_entry &entry) noexcept {
  return database_guard_entry_valid(entry);
}

[[nodiscard]] bool exact_snapshot(
    const dependency_vector_entry &expected,
    const dependency_epoch_snapshot &actual) noexcept {
  return actual.key == expected.version.key &&
         actual.physical_identity == expected.version.physical_identity &&
         actual.record_generation == expected.record_generation &&
         actual.committed_epoch == expected.version.committed_epoch &&
         actual.observability == expected.version.observability &&
         actual.transition_count == 0;
}

struct emergency_epoch_snapshot {
  std::uint64_t write_epoch = 0;
  std::uint64_t change_generation = 0;
};

/** Accept a zero-transition sample only if its change counter stayed fixed while reading. */
[[nodiscard]] bool sample_stable_emergency_epoch(
    emergency_epoch_snapshot *snapshot) noexcept {
  if (snapshot == nullptr) {
    return false;
  }
  const shared_registry::shared_registry_emergency_access emergency =
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
  *snapshot = emergency_epoch_snapshot{
      .write_epoch = write_epoch,
      .change_generation = generation_after,
  };
  return true;
}

[[nodiscard]] bool emergency_epoch_matches(
    const dependency_snapshot_ticket &ticket,
    const emergency_epoch_snapshot &snapshot) noexcept {
  return ticket.emergency_write_epoch == snapshot.write_epoch &&
         ticket.emergency_change_generation == snapshot.change_generation;
}

[[nodiscard]] dependency_runtime_result release_ticket_internal(
    dependency_snapshot_ticket *ticket) noexcept {
  dependency_runtime_result result{};
  if (ticket == nullptr || !ticket->active ||
      ticket->count > k_maximum_snapshot_ticket_dependencies) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    result.error = dependency_runtime_error::registry_unavailable;
    result.fail_open = true;
    return result;
  }
  bool failed = false;
  for (std::uint32_t index = 0; index < ticket->count; ++index) {
    if (!ticket->references[index].active) {
      continue;
    }
    const dependency_mutation_result released = release_dependency_reference(
        registry->dependency, ticket->references[index],
        directory_callbacks());
    failed = failed || !released.applied;
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
  }
  if (ticket->conservative_guard_reference.active) {
    const dependency_mutation_result released =
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
    result.error = dependency_runtime_error::reference_release_failed;
    result.fail_open = true;
  } else {
    *ticket = {};
  }
  return result;
}

void external_prepared_exit(int, Datum) {
  if (g_external_prepared_token.state !=
      external_prepared_resolution_state::active) {
    return;
  }
  static_cast<void>(finish_external_prepared_resolution(
      &g_external_prepared_token,
      external_prepared_resolution_outcome::ambiguous));
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
        const object_definition_dependency_error definition_error =
            g_object_definition_dependencies.begin_savepoint(
                static_cast<std::uint32_t>(child),
                static_cast<std::uint32_t>(parent));
        if (definition_error !=
            object_definition_dependency_error::none) {
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
        const dependency_runtime_result guard =
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

[[nodiscard]] dependency_runtime_result capture_snapshot_ticket(
    std::span<const dependency_version> dependencies,
    dependency_snapshot_ticket *ticket,
    const dependency_key *conservative_guard_key,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (ticket == nullptr || ticket->active ||
      snapshot_binding_completion_count == 0) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  if (dependencies.size() > k_maximum_snapshot_ticket_dependencies) {
    result.error = dependency_runtime_error::capacity_exceeded;
    result.fail_open = true;
    return result;
  }
  if (conservative_guard_key != nullptr &&
      !conservative_guard_key_valid(*conservative_guard_key)) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  for (std::size_t index = 0; index < dependencies.size(); ++index) {
    if (!valid_capture_request(dependencies[index])) {
      result.error = dependency_runtime_error::invalid_argument;
      result.fail_open = true;
      return result;
    }
    if (conservative_guard_key != nullptr &&
        dependencies[index].key == *conservative_guard_key) {
      result.error = dependency_runtime_error::non_canonical;
      result.fail_open = true;
      return result;
    }
    if (index != 0 && !dependency_key_less(
        dependencies[index - 1].key, dependencies[index].key)) {
      result.error = dependency_runtime_error::non_canonical;
      result.fail_open = true;
      return result;
    }
  }
  const bool explicit_guard_requires_completion =
      conservative_guard_key != nullptr &&
      database_guard_requires_completion(*conservative_guard_key);
  const bool effective_completion_guard =
      explicit_guard_requires_completion || std::any_of(
          dependencies.begin(), dependencies.end(),
          [](const dependency_version &version) noexcept {
            return version.observability ==
                   write_observability::database_wide_fallback;
          });
  if (continuing_completion_count != 0 &&
      (!effective_completion_guard ||
       continuing_completion_count !=
           snapshot_binding_completion_count)) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    result.error = dependency_runtime_error::registry_unavailable;
    result.fail_open = true;
    return result;
  }
  Snapshot snapshot = GetActiveSnapshot();
  if (snapshot == nullptr || !IsMVCCSnapshot(snapshot)) {
    result.error = dependency_runtime_error::reference_validation_failed;
    result.fail_open = true;
    return result;
  }
  pause(dependency_runtime_pause_point::before_visibility_ticket);
  LWLockAcquire(ProcArrayLock, LW_SHARED);
  const std::uint64_t completion_count =
      TransamVariables->xactCompletionCount;
  if (!dependency_completion_handshake_valid(
          snapshot->snapXactCompletionCount,
          snapshot_binding_completion_count,
          completion_count,
          continuing_completion_count)) {
    LWLockRelease(ProcArrayLock);
    result.error = dependency_runtime_error::reference_validation_failed;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
    return result;
  }
  emergency_epoch_snapshot emergency{};
  if (!sample_stable_emergency_epoch(&emergency)) {
    LWLockRelease(ProcArrayLock);
    result.error = dependency_runtime_error::emergency_fallback;
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
  for (const dependency_version &version : dependencies) {
    const dependency_reference_result acquired =
        acquire_dependency_reference(
            registry->dependency,
            dependency_reference_request{
                .key = version.key,
                .physical_identity = version.physical_identity,
                .observability = version.observability,
                .kind = dependency_reference_kind::ticket,
            },
            directory_callbacks());
    if (!acquired) {
      result.error = dependency_runtime_error::reference_acquire_failed;
      result.fail_open = true;
      result.coarse_fallback_required =
          acquired.coarse_fallback_required;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      return result;
    }
    const std::uint32_t index = ticket->count++;
    ticket->references[index] = acquired.reference;
    ticket->entries[index] = dependency_vector_entry{
        .version = {
            .key = acquired.snapshot.key,
            .committed_epoch = acquired.snapshot.committed_epoch,
            .physical_identity = acquired.snapshot.physical_identity,
            .observability = acquired.snapshot.observability,
        },
        .record_generation = acquired.snapshot.record_generation,
    };
    if (!exact_snapshot(ticket->entries[index], acquired.snapshot)) {
      result.error = dependency_runtime_error::reference_validation_failed;
      result.fail_open = true;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      saturating_increment(g_diagnostics.ticket_validation_failure_count);
      return result;
    }
  }
  if (conservative_guard_key != nullptr) {
    const dependency_reference_result acquired =
        acquire_dependency_reference(
            registry->dependency,
            dependency_reference_request{
                .key = *conservative_guard_key,
                .physical_identity = {},
                .observability =
                    write_observability::database_wide_fallback,
                .kind = dependency_reference_kind::ticket,
            },
            directory_callbacks());
    if (!acquired) {
      result.error = dependency_runtime_error::reference_acquire_failed;
      result.fail_open = true;
      result.coarse_fallback_required = true;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      return result;
    }
    ticket->conservative_guard_reference = acquired.reference;
    ticket->conservative_guard_entry = dependency_vector_entry{
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
      result.error = dependency_runtime_error::reference_validation_failed;
      result.fail_open = true;
      result.coarse_fallback_required = true;
      static_cast<void>(release_ticket_internal(ticket));
      LWLockRelease(ProcArrayLock);
      saturating_increment(g_diagnostics.ticket_validation_failure_count);
      return result;
    }
  }
  const dependency_runtime_result stable =
      validate_snapshot_ticket_locked_impl(*ticket, {}, true);
  if (!stable) {
    const dependency_runtime_result released = release_ticket_internal(ticket);
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
    result.error = dependency_runtime_error::reference_validation_failed;
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

[[nodiscard]] dependency_runtime_result
validate_snapshot_ticket_locked_impl(
    const dependency_snapshot_ticket &ticket,
    dependency_ticket_locked_action action,
    bool completion_lock_already_held) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  result.dependency_count = ticket.count;
  if (!ticket.active ||
      ticket.count > k_maximum_snapshot_ticket_dependencies ||
      ticket.emergency_change_generation == 0 ||
      ticket.snapshot_binding_completion_count == 0 ||
      !action.valid() ||
      std::any_of(
          ticket.reserved.begin(), ticket.reserved.end(),
          [](std::uint8_t value) noexcept { return value != 0; })) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    result.error = dependency_runtime_error::registry_unavailable;
    result.fail_open = true;
    return result;
  }
  const auto *header = reinterpret_cast<const dependency_directory_header *>(
      registry->dependency.region.data());
  if (header == nullptr ||
      header->bootstrap_state != bootstrap_state::ready ||
      header->config.partition_count == 0) {
    result.error = dependency_runtime_error::registry_unavailable;
    result.fail_open = true;
    return result;
  }
  std::array<std::uint32_t,
             k_maximum_snapshot_ticket_dependencies + 1U>
      partitions{};
  std::uint32_t partition_count = 0;
  bool requires_completion_guard = false;
  for (std::uint32_t index = 0; index < ticket.count; ++index) {
    if (!ticket.references[index] ||
        ticket.references[index].partition_index >=
            header->config.partition_count ||
        !dependency_vector_entry_valid(ticket.entries[index]) ||
        (index != 0 && !dependency_key_less(
            ticket.entries[index - 1U].version.key,
            ticket.entries[index].version.key))) {
      result.error = dependency_runtime_error::invalid_argument;
      result.fail_open = true;
      return result;
    }
    requires_completion_guard =
        requires_completion_guard ||
        ticket.entries[index].version.observability ==
            write_observability::database_wide_fallback;
    partitions[partition_count++] =
        ticket.references[index].partition_index;
  }
  if (ticket.explicit_completion_guard) {
    if (!ticket.conservative_guard_reference ||
        ticket.conservative_guard_reference.partition_index >=
            header->config.partition_count ||
        !conservative_guard_entry_valid(
            ticket.conservative_guard_entry)) {
      result.error = dependency_runtime_error::invalid_argument;
      result.fail_open = true;
      return result;
    }
    partitions[partition_count++] =
        ticket.conservative_guard_reference.partition_index;
  } else if (ticket.conservative_guard_reference ||
             ticket.conservative_guard_entry !=
                 dependency_vector_entry{}) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  const bool explicit_guard_requires_completion =
      ticket.explicit_completion_guard &&
      database_guard_requires_completion(
          ticket.conservative_guard_entry);
  if ((explicit_guard_requires_completion || requires_completion_guard) !=
      ticket.requires_completion_guard) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  if (ticket.snapshot_xact_completion_count != 0 &&
      !ticket.requires_completion_guard) {
    result.error = dependency_runtime_error::invalid_argument;
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
  if (!dependency_completion_handshake_valid(
          0,
          ticket.snapshot_binding_completion_count,
          TransamVariables->xactCompletionCount,
          ticket.snapshot_xact_completion_count)) {
    if (acquired_completion_lock) {
      LWLockRelease(ProcArrayLock);
    }
    result.error = dependency_runtime_error::reference_validation_failed;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
    return result;
  }
  if (!lock_partitions(nullptr, locked_partitions)) {
    if (acquired_completion_lock) {
      LWLockRelease(ProcArrayLock);
    }
    result.error = dependency_runtime_error::registry_unavailable;
    result.fail_open = true;
    result.coarse_fallback_required = true;
    return result;
  }

  bool action_applied = false;
  emergency_epoch_snapshot before{};
  if (!sample_stable_emergency_epoch(&before) ||
      !emergency_epoch_matches(ticket, before)) {
    result.error = dependency_runtime_error::emergency_fallback;
    result.fail_open = true;
    result.coarse_fallback_required = true;
  }
  for (std::uint32_t index = 0;
       result.error == dependency_runtime_error::none &&
       index < ticket.count;
       ++index) {
    const dependency_snapshot_result snapshot = snapshot_dependency(
        registry->dependency, ticket.references[index],
        directory_callbacks(dependency_directory_lock_mode::already_locked));
    if (!snapshot ||
        !exact_snapshot(ticket.entries[index], snapshot.snapshot)) {
      result.error = dependency_runtime_error::reference_validation_failed;
      result.fail_open = true;
      result.coarse_fallback_required = snapshot.coarse_fallback_required;
    }
  }
  if (result.error == dependency_runtime_error::none &&
      ticket.explicit_completion_guard) {
    const dependency_snapshot_result snapshot = snapshot_dependency(
        registry->dependency, ticket.conservative_guard_reference,
        directory_callbacks(dependency_directory_lock_mode::already_locked));
    if (!snapshot ||
        !exact_snapshot(ticket.conservative_guard_entry,
                        snapshot.snapshot)) {
      result.error = dependency_runtime_error::reference_validation_failed;
      result.fail_open = true;
      result.coarse_fallback_required = true;
    }
  }
  if (result.error == dependency_runtime_error::none &&
      action.apply != nullptr) {
    action_applied = action.apply(
        action.context,
        std::span<const dependency_vector_entry>(
            ticket.entries.data(), ticket.count));
    if (!action_applied) {
      result.error = dependency_runtime_error::locked_action_failed;
      result.fail_open = true;
    }
  }
  emergency_epoch_snapshot after{};
  if (result.error == dependency_runtime_error::none &&
      (!sample_stable_emergency_epoch(&after) ||
       !emergency_epoch_matches(ticket, after))) {
    if (action_applied) {
      action.rollback(action.context);
      action_applied = false;
    }
    result.error = dependency_runtime_error::emergency_fallback;
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

[[nodiscard]] dependency_runtime_result validate_snapshot_ticket_locked(
    const dependency_snapshot_ticket &ticket,
    dependency_ticket_locked_action action) noexcept {
  return detail::validate_snapshot_ticket_locked_impl(ticket, action, false);
}

[[nodiscard]] dependency_runtime_result validate_dependency_vector_locked(
    std::span<const std::byte> dependency_vector_envelope,
    const dependency_vector_entry *conservative_guard,
    std::uint64_t snapshot_binding_completion_count,
    std::uint64_t continuing_completion_count,
    dependency_ticket_locked_action action) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (!action.valid() || snapshot_binding_completion_count == 0) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  if (conservative_guard != nullptr &&
      !conservative_guard_entry_valid(*conservative_guard)) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  const dependency_vector_envelope_result validation =
      validate_dependency_vector_envelope(dependency_vector_envelope);
  if (!validation ||
      validation.header.entry_count >
          k_maximum_snapshot_ticket_dependencies) {
    result.error = dependency_runtime_error::vector_validation_failed;
    result.fail_open = true;
    return result;
  }
  result.dependency_count = validation.header.entry_count;
  std::array<dependency_vector_entry,
             k_maximum_snapshot_ticket_dependencies>
      expected_entries{};
  const dependency_vector_envelope_result decoded =
      copy_dependency_vector_entries(
          dependency_vector_envelope,
          std::span<dependency_vector_entry>(
              expected_entries.data(), result.dependency_count));
  if (!decoded) {
    result.error = dependency_runtime_error::vector_validation_failed;
    result.fail_open = true;
    return result;
  }
  const bool vector_requires_completion_guard = std::any_of(
      expected_entries.begin(),
      expected_entries.begin() + result.dependency_count,
      [](const dependency_vector_entry &entry) noexcept {
        return entry.version.observability ==
               write_observability::database_wide_fallback;
      });
  const bool effective_completion_guard =
      (conservative_guard != nullptr &&
       database_guard_requires_completion(*conservative_guard)) ||
      vector_requires_completion_guard;
  if (continuing_completion_count != 0 &&
      !effective_completion_guard) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }

  std::array<dependency_version,
             k_maximum_snapshot_ticket_dependencies>
      versions{};
  for (std::uint32_t index = 0; index < result.dependency_count; ++index) {
    versions[index] = expected_entries[index].version;
  }
  dependency_snapshot_ticket ticket{};
  const dependency_runtime_result captured = capture_snapshot_ticket(
      std::span<const dependency_version>(
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
    result.error = dependency_runtime_error::reference_validation_failed;
    result.fail_open = true;
    result.coarse_fallback_required = effective_completion_guard;
    saturating_increment(g_diagnostics.ticket_validation_failure_count);
  } else {
    result = validate_snapshot_ticket_locked(ticket, action);
  }
  const dependency_runtime_result released =
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

[[nodiscard]] dependency_runtime_result validate_snapshot_ticket(
    const dependency_snapshot_ticket &ticket) noexcept {
  return validate_snapshot_ticket_locked(ticket);
}

[[nodiscard]] dependency_runtime_result release_snapshot_ticket(
    dependency_snapshot_ticket *ticket) noexcept {
  return detail::release_ticket_internal(ticket);
}

[[nodiscard]] dependency_runtime_result
retain_generation_vector_preserving_ticket(
    const dependency_snapshot_ticket &ticket,
    dependency_generation_vector *generation) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (generation == nullptr || generation->active || !ticket.active ||
      ticket.count > k_maximum_snapshot_ticket_dependencies) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  const dependency_runtime_result stable = validate_snapshot_ticket(ticket);
  if (!stable) {
    return stable;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    return {.error = dependency_runtime_error::registry_unavailable,
            .fail_open = true};
  }
  completion_count_guard completion_guard(
      true, ticket.snapshot_binding_completion_count);
  if (!completion_guard.matches()) {
    return {.error = dependency_runtime_error::reference_validation_failed,
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
  std::array<dependency_reference,
             k_maximum_snapshot_ticket_dependencies>
      generation_references{};
  dependency_reference generation_guard_reference{};
  std::uint32_t retained_count = 0;
  const auto release_retained_references =
      [registry,
       &generation_references,
       &generation_guard_reference,
       &retained_count]() noexcept {
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
    const dependency_vector_entry &expected = ticket.entries[index];
    const dependency_reference_result acquired = acquire_dependency_reference(
        registry->dependency,
        dependency_reference_request{
            .key = expected.version.key,
            .physical_identity = expected.version.physical_identity,
            .observability = expected.version.observability,
            .kind = dependency_reference_kind::generation,
        },
        directory_callbacks());
    if (!acquired || !exact_snapshot(expected, acquired.snapshot)) {
      if (acquired) {
        dependency_reference cleanup = acquired.reference;
        static_cast<void>(release_dependency_reference(
            registry->dependency, cleanup, directory_callbacks()));
      }
      release_retained_references();
      return {.error = dependency_runtime_error::reference_acquire_failed,
              .dependency_count = retained_count,
              .fail_open = true,
              .coarse_fallback_required =
                  acquired.coarse_fallback_required};
    }
    generation_references[retained_count++] = acquired.reference;
  }
  if (ticket.explicit_completion_guard) {
    const dependency_reference_result acquired = acquire_dependency_reference(
        registry->dependency,
        dependency_reference_request{
            .key = ticket.conservative_guard_entry.version.key,
            .physical_identity =
                ticket.conservative_guard_entry.version.physical_identity,
            .observability =
                ticket.conservative_guard_entry.version.observability,
            .kind = dependency_reference_kind::generation,
        },
        directory_callbacks());
    if (!acquired ||
        !exact_snapshot(ticket.conservative_guard_entry,
                        acquired.snapshot)) {
      if (acquired) {
        dependency_reference cleanup = acquired.reference;
        static_cast<void>(release_dependency_reference(
            registry->dependency, cleanup, directory_callbacks()));
      }
      release_retained_references();
      return {.error = dependency_runtime_error::reference_acquire_failed,
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
    return {.error = dependency_runtime_error::vector_allocation_failed,
            .dependency_count = retained_count,
            .fail_open = true};
  }
  auto *address = static_cast<std::byte *>(
      dsa_get_address(registry->area, allocation));
  const std::span<std::byte> envelope(address, bytes);
  const dependency_vector_envelope_result initialized =
      initialize_dependency_vector_envelope(
          envelope,
          std::span<const dependency_vector_entry>(
              ticket.entries.data(), ticket.count));
  if (!initialized) {
    dsa_free(registry->area, allocation);
    release_retained_references();
    return {.error = dependency_runtime_error::vector_validation_failed,
            .dependency_count = retained_count,
            .fail_open = true};
  }
  if (!add_atomic(&registry->root->hot_live_bytes, bytes)) {
    dsa_free(registry->area, allocation);
    release_retained_references();
    static_cast<void>(increment_atomic(
        &registry->root->hot_allocation_failures));
    return {.error = dependency_runtime_error::vector_allocation_failed,
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

[[nodiscard]] dependency_runtime_result retain_generation_vector(
    dependency_snapshot_ticket *ticket,
    dependency_generation_vector *generation) noexcept {
  dependency_runtime_result result{};
  if (ticket == nullptr) {
    result.error = dependency_runtime_error::invalid_argument;
    result.fail_open = true;
    return result;
  }
  result = retain_generation_vector_preserving_ticket(*ticket, generation);
  if (!result) {
    return result;
  }
  const dependency_runtime_result released_ticket =
      release_snapshot_ticket(ticket);
  result.coarse_fallback_required =
      result.coarse_fallback_required ||
      released_ticket.coarse_fallback_required;
  if (released_ticket) {
    return result;
  }
  const dependency_runtime_result released_generation =
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
    const dependency_generation_vector &generation) noexcept {
  using namespace detail;
  const std::uint32_t reference_count =
      generation.count +
      static_cast<std::uint32_t>(generation.explicit_completion_guard);
  if (!generation.active || !DsaPointerIsValid(generation.envelope) ||
      generation.count > k_maximum_snapshot_ticket_dependencies ||
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
       generation.conservative_guard_entry != dependency_vector_entry{})) {
    return {};
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    return {};
  }
  const auto *address = static_cast<const std::byte *>(
      dsa_get_address(registry->area, generation.envelope));
  const std::span<const std::byte> envelope(address, generation.bytes);
  const dependency_vector_envelope_result validation =
      validate_dependency_vector_envelope(envelope);
  if (!validation || validation.header.entry_count != generation.count ||
      validation.header.payload_checksum != generation.checksum) {
    return {};
  }
  std::array<dependency_vector_entry,
             k_maximum_snapshot_ticket_dependencies>
      entries{};
  const dependency_vector_envelope_result decoded =
      copy_dependency_vector_entries(
          envelope,
          std::span<dependency_vector_entry>(entries.data(), generation.count));
  const bool requires_completion_guard =
      decoded && std::any_of(
          entries.begin(), entries.begin() + generation.count,
          [](const dependency_vector_entry &entry) noexcept {
            return entry.version.observability ==
                   write_observability::database_wide_fallback;
          });
  const bool explicit_guard_requires_completion =
      generation.explicit_completion_guard &&
      database_guard_requires_completion(
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

[[nodiscard]] dependency_runtime_result transfer_generation_vector_to_registry(
    dependency_generation_vector *generation) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (generation == nullptr || generation->released_count != 0 ||
      dependency_generation_vector_envelope(*generation).empty()) {
    result.error = dependency_runtime_error::vector_validation_failed;
    result.fail_open = true;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    result.error = dependency_runtime_error::registry_unavailable;
    result.fail_open = true;
    return result;
  }
  const std::uint32_t dependency_count = generation->count;
  if (!subtract_atomic(&registry->root->hot_live_bytes, generation->bytes)) {
    result.error = dependency_runtime_error::reference_release_failed;
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

[[nodiscard]] dependency_generation_release_batch release_generation_entries(
    std::span<const std::byte> dependency_vector_envelope,
    const dependency_vector_entry *conservative_guard,
    std::uint32_t first_reference) noexcept {
  using namespace detail;
  dependency_generation_release_batch result{
      .first_reference = first_reference,
  };
  const dependency_vector_envelope_result validation =
      validate_dependency_vector_envelope(dependency_vector_envelope);
  if (!validation ||
      (conservative_guard != nullptr &&
       !conservative_guard_entry_valid(*conservative_guard))) {
    result.error = dependency_runtime_error::vector_validation_failed;
    return result;
  }
  result.dependency_count = validation.header.entry_count;
  result.reference_count =
      result.dependency_count +
      static_cast<std::uint32_t>(conservative_guard != nullptr);
  if (first_reference > result.reference_count) {
    result.error = dependency_runtime_error::vector_validation_failed;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    result.error = dependency_runtime_error::registry_unavailable;
    return result;
  }
  std::array<dependency_vector_entry,
             k_maximum_snapshot_ticket_dependencies>
      entries{};
  const dependency_vector_envelope_result decoded =
      copy_dependency_vector_entries(
          dependency_vector_envelope,
          std::span<dependency_vector_entry>(entries.data(),
                                             result.dependency_count));
  if (!decoded) {
    result.error = dependency_runtime_error::vector_validation_failed;
    return result;
  }
  for (std::uint32_t index = first_reference;
       index < result.reference_count; ++index) {
    const dependency_vector_entry &entry =
        index < result.dependency_count ? entries[index]
                                        : *conservative_guard;
    const dependency_mutation_result released =
        release_dependency_generation_entry(
            registry->dependency, entry, directory_callbacks());
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    if (!released.applied) {
      result.error = dependency_runtime_error::reference_release_failed;
      break;
    }
    ++result.applied_count;
    if (released.error != dependency_directory_error::none &&
        result.error == dependency_runtime_error::none) {
      result.error = dependency_runtime_error::reference_release_failed;
    }
  }
  result.complete =
      first_reference + result.applied_count == result.reference_count;
  return result;
}

[[nodiscard]] dependency_runtime_result release_generation_vector(
    dependency_generation_vector *generation) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (generation == nullptr) {
    result.error = dependency_runtime_error::invalid_argument;
    return result;
  }
  const std::span<const std::byte> envelope =
      dependency_generation_vector_envelope(*generation);
  if (envelope.empty()) {
    result.error = dependency_runtime_error::vector_validation_failed;
    result.coarse_fallback_required = true;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  if (registry == nullptr) {
    result.error = dependency_runtime_error::registry_unavailable;
    return result;
  }
  std::array<dependency_vector_entry,
             k_maximum_snapshot_ticket_dependencies>
      entries{};
  const dependency_vector_envelope_result decoded =
      copy_dependency_vector_entries(
          envelope,
          std::span<dependency_vector_entry>(
              entries.data(), generation->count));
  if (!decoded || decoded.header.payload_checksum != generation->checksum) {
    result.error = dependency_runtime_error::vector_validation_failed;
    result.coarse_fallback_required = true;
    return result;
  }
  for (std::uint32_t index = generation->released_count;
       index < generation->count; ++index) {
    const dependency_mutation_result released =
        release_dependency_generation_entry(
            registry->dependency, entries[index], directory_callbacks());
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    if (!released.applied) {
      result.error = dependency_runtime_error::reference_release_failed;
      result.dependency_count = generation->count;
      return result;
    }
    generation->released_count = index + 1U;
  }
  if (generation->explicit_completion_guard &&
      generation->released_count == generation->count) {
    const dependency_mutation_result released =
        release_dependency_generation_entry(
            registry->dependency,
            generation->conservative_guard_entry,
            directory_callbacks());
    result.coarse_fallback_required =
        result.coarse_fallback_required ||
        released.coarse_fallback_required;
    if (!released.applied) {
      result.error = dependency_runtime_error::reference_release_failed;
      result.dependency_count = generation->count;
      return result;
    }
    ++generation->released_count;
  }
  if (!subtract_atomic(
      &registry->root->hot_live_bytes, generation->bytes)) {
    result.error = dependency_runtime_error::reference_release_failed;
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

[[nodiscard]] dependency_runtime_result observe_dirty_dependency(
    const dependency_version &dependency,
    const physical_identity &committed_physical_identity) noexcept {
  using namespace detail;
  if (!valid_version(dependency) ||
      !physical_identity_valid(committed_physical_identity)) {
    return {.error = dependency_runtime_error::invalid_argument,
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
      return {.error = dependency_runtime_error::reference_validation_failed,
              .dependency_count = g_dirty_count,
              .coarse_fallback_required = true};
    }
  }
  if (g_dirty_count >= k_maximum_dirty_dependencies) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::capacity_exceeded,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  const shared_registry::shared_registry_backend_identity backend =
      shared_registry::current_backend_identity();
  if (registry == nullptr || !backend.valid()) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::registry_unavailable,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const dependency_reference_result acquired = acquire_dependency_reference(
      registry->dependency,
      dependency_reference_request{
          .key = dependency.key,
          .physical_identity = dependency.physical_identity,
          .observability = dependency.observability,
          .kind = dependency_reference_kind::owner,
      },
      directory_callbacks());
  if (!acquired) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::reference_acquire_failed,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  dependency_reference capability = acquired.reference;
  const transition_dependency_reference ledger_reference =
      transition_reference(capability);
  if (!g_owner_active) {
    const FullTransactionId full_xid = GetTopFullTransactionId();
    const transition_ledger_operation_result claimed = claim_transition_owner(
        shared_registry::shared_transition_ledger(),
        transition_owner_identity{
            .proc_number = backend.proc_number,
            .pid = backend.pid,
            .backend_nonce = backend.backend_nonce,
            .backend_started_at = backend.backend_started_at_us,
            .full_xid = U64FromFullTransactionId(full_xid),
        },
        ledger_reference, ledger_callbacks(),
        transition_initial_reference_role::precise_and_fallback);
    if (!claimed) {
      static_cast<void>(release_dependency_reference(
          registry->dependency, capability, directory_callbacks()));
      force_emergency_fallback();
      return {.error = dependency_runtime_error::owner_claim_failed,
              .dependency_count = g_dirty_count,
              .coarse_fallback_required = true};
    }
    g_owner_claim = claimed.claim;
    g_owner_active = true;
    saturating_increment(g_diagnostics.owner_claim_count);
  } else {
    const transition_ledger_operation_result recorded =
        record_transition_dependency(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            ledger_reference, ledger_callbacks());
    if (!recorded) {
      static_cast<void>(release_dependency_reference(
          registry->dependency, capability, directory_callbacks()));
      force_emergency_fallback();
      return {.error = dependency_runtime_error::owner_record_failed,
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

[[nodiscard]] dependency_runtime_result observe_dirty_dependency(
    const dependency_version &dependency) noexcept {
  return observe_dirty_dependency(
      dependency, dependency.physical_identity);
}

[[nodiscard]] dependency_runtime_result observe_dirty_dependency(
    const dependency_version &observed_dependency,
    const dependency_version &committed_dependency) noexcept {
  dependency_version observed_shape = observed_dependency;
  dependency_version committed_shape = committed_dependency;
  observed_shape.physical_identity = {};
  committed_shape.physical_identity = {};
  if (!detail::valid_version(observed_dependency) ||
      !detail::valid_version(committed_dependency) ||
      observed_shape != committed_shape) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::invalid_argument,
            .dependency_count = detail::g_dirty_count,
            .coarse_fallback_required = true};
  }
  return observe_dirty_dependency(
      observed_dependency, committed_dependency.physical_identity);
}

[[nodiscard]] dependency_runtime_result
observe_database_definition_dependency(
    std::uint32_t database_oid) noexcept {
  using namespace detail;
  if (!IsTransactionState() ||
      !initialize_object_definition_collector(
          database_oid, GetCurrentSubTransactionId())) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::invalid_argument,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  g_object_definition_dependencies.force_database_definition_guard();
  g_object_definition_pre_commit_captured = false;
  dependency_runtime_result result =
      ensure_object_definition_database_guards(database_oid);
  result.coarse_fallback_required = true;
  if (!result) {
    force_emergency_fallback();
  }
  return result;
}

[[nodiscard]] dependency_runtime_result
observe_object_definition_dependency(
    const object_definition_mutation &mutation) noexcept {
  using namespace detail;
  if (!IsTransactionState() || mutation.key.database_oid == 0 ||
      !initialize_object_definition_collector(
          mutation.key.database_oid,
          GetCurrentSubTransactionId())) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::invalid_argument,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const object_definition_observation_result observed =
      g_object_definition_dependencies.observe(
          static_cast<std::uint32_t>(GetCurrentSubTransactionId()),
          mutation);
  g_object_definition_pre_commit_captured = false;
  if (!observed.root_guard_required) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::invalid_argument,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  dependency_runtime_result result =
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

[[nodiscard]] dependency_runtime_result begin_dirty_frame(
    SubTransactionId child, SubTransactionId parent) noexcept {
  using namespace detail;
  if (child == InvalidSubTransactionId ||
      g_dirty_frame_count >= k_maximum_dirty_frames) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::capacity_exceeded,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  g_dirty_frames[g_dirty_frame_count++] = dirty_frame{
      .child = child,
      .parent = parent,
      .retained_count = g_dirty_count,
  };
  return {.dependency_count = g_dirty_count,
          .coarse_fallback_required = g_force_coarse};
}

[[nodiscard]] dependency_runtime_result finish_dirty_frame(
    SubTransactionId child, SubTransactionId parent,
    bool committed) noexcept {
  using namespace detail;
  if (g_dirty_frame_count == 0) {
    return {.error = dependency_runtime_error::savepoint_mismatch,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  const dirty_frame frame = g_dirty_frames[g_dirty_frame_count - 1U];
  if (frame.child != child || frame.parent != parent) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::savepoint_mismatch,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  --g_dirty_frame_count;
  if (frame.retained_count > g_dirty_count) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::savepoint_mismatch,
            .dependency_count = g_dirty_count,
            .coarse_fallback_required = true};
  }
  if (committed || !g_owner_active || frame.retained_count == g_dirty_count) {
    return {.dependency_count = g_dirty_count,
            .coarse_fallback_required = g_force_coarse};
  }
  if (frame.retained_count == 0) {
    const transition_ledger_operation_result cleared =
        finish_owner_transitions(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            transition_resolution::known_aborted, ledger_callbacks());
    if (!cleared) {
      force_emergency_fallback();
      return {.error = dependency_runtime_error::owner_record_failed,
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
  const transition_ledger_operation_result rebuilt =
      rebuild_transition_dependencies(
          shared_registry::shared_transition_ledger(), g_owner_claim,
          std::span<const transition_dependency_reference>(
              g_dirty_references.data() + 1U,
              frame.retained_count - 1U),
          ledger_callbacks());
  if (!rebuilt) {
    force_emergency_fallback();
    return {.error = dependency_runtime_error::owner_record_failed,
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

[[nodiscard]] dependency_runtime_result
publish_pre_commit_transitions() noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (g_transition_published || g_emergency_transition_published) {
    result.dependency_count = g_dirty_count;
    result.coarse_fallback_required = g_force_coarse;
    return result;
  }
  if (g_owner_active) {
    const transition_ledger_operation_result published =
        begin_owner_transitions(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            ledger_callbacks());
    if (published) {
      g_transition_published = true;
      saturating_increment(g_diagnostics.transition_publish_count);
    } else {
      g_force_coarse = true;
      result.error = dependency_runtime_error::transition_publish_failed;
      result.coarse_fallback_required = true;
    }
  }
  if (g_force_coarse && !begin_emergency_transition()) {
    result.error = dependency_runtime_error::emergency_fallback;
    result.coarse_fallback_required = true;
  }
  pause(dependency_runtime_pause_point::after_transition_publish);
  result.dependency_count = g_dirty_count;
  result.coarse_fallback_required =
      result.coarse_fallback_required || g_force_coarse;
  return result;
}

[[nodiscard]] dependency_runtime_result
finish_post_visibility_transitions(bool committed) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (committed && (g_owner_active || g_force_coarse) &&
      !g_transition_published && !g_emergency_transition_published) {
    g_force_coarse = true;
    static_cast<void>(begin_emergency_transition());
  }
  if (g_owner_active) {
    const transition_resolution resolution =
        g_transition_published
        ? (committed ? transition_resolution::committed
                     : transition_resolution::known_aborted)
        : transition_resolution::known_aborted;
    const transition_ledger_operation_result finished =
        finish_owner_transitions(
            shared_registry::shared_transition_ledger(), g_owner_claim,
            resolution, ledger_callbacks());
    if (!finished) {
      result.error = dependency_runtime_error::transition_finish_failed;
      result.coarse_fallback_required = true;
      g_force_coarse = true;
    }
  }
  if (!finish_emergency_transition(committed)) {
    result.error = dependency_runtime_error::emergency_fallback;
    result.coarse_fallback_required = true;
  }
  result.dependency_count = g_dirty_count;
  if (g_owner_active || g_transition_published || g_force_coarse) {
    saturating_increment(g_diagnostics.transition_finish_count);
  }
  reset_owner_state();
  return result;
}

[[nodiscard]] dependency_runtime_result
begin_external_prepared_resolution(
    external_prepared_resolution_kind kind,
    std::uint32_t database_oid,
    std::uint64_t resolution_identity,
    external_prepared_resolution_token *token) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  const bool kind_valid =
      kind == external_prepared_resolution_kind::commit_prepared ||
      kind == external_prepared_resolution_kind::rollback_prepared;
  if (token == nullptr ||
      token->state != external_prepared_resolution_state::empty ||
      !kind_valid || database_oid == 0 || resolution_identity == 0) {
    result.error = dependency_runtime_error::invalid_argument;
    result.coarse_fallback_required = true;
    return result;
  }
  if (g_owner_active || g_dirty_count != 0 ||
      g_emergency_transition_published ||
      g_external_prepared_token.state ==
          external_prepared_resolution_state::active) {
    result.error = dependency_runtime_error::owner_claim_failed;
    result.coarse_fallback_required = true;
    return result;
  }
  const shared_registry::shared_registry_attachment *registry = attachment();
  const shared_registry::shared_registry_backend_identity backend =
      shared_registry::current_backend_identity();
  if (registry == nullptr || !backend.valid()) {
    result.error = dependency_runtime_error::registry_unavailable;
    result.coarse_fallback_required = true;
    return result;
  }
  dependency_reference_result acquired = acquire_dependency_reference(
      registry->dependency,
      dependency_reference_request{
          .key = dependency_key{
              .database_oid = database_oid,
              .class_oid = 0,
              .object_oid = database_oid,
              .object_sub_id = 0,
              .kind = dependency_kind::database_write_epoch,
          },
          .physical_identity = {},
          .observability = write_observability::database_wide_fallback,
          .kind = dependency_reference_kind::owner,
      },
      directory_callbacks());
  if (!acquired) {
    force_emergency_fallback();
    result.error = dependency_runtime_error::reference_acquire_failed;
    result.coarse_fallback_required = true;
    return result;
  }
  dependency_reference capability = acquired.reference;
  const transition_ledger_operation_result claimed = claim_transition_owner(
      shared_registry::shared_transition_ledger(),
      transition_owner_identity{
          .proc_number = backend.proc_number,
          .pid = backend.pid,
          .backend_nonce = backend.backend_nonce,
          .backend_started_at = backend.backend_started_at_us,
          .full_xid = resolution_identity,
      },
      transition_reference(capability), ledger_callbacks(),
      transition_initial_reference_role::precise_and_fallback);
  if (!claimed) {
    static_cast<void>(release_dependency_reference(
        registry->dependency, capability, directory_callbacks()));
    force_emergency_fallback();
    result.error = dependency_runtime_error::owner_claim_failed;
    result.coarse_fallback_required = true;
    return result;
  }
  const transition_ledger_operation_result published =
      begin_owner_transitions(
          shared_registry::shared_transition_ledger(), claimed.claim,
          ledger_callbacks());
  if (!published) {
    const transition_ledger_operation_result drained =
        finish_owner_transitions(
            shared_registry::shared_transition_ledger(), claimed.claim,
            transition_resolution::aborted, ledger_callbacks());
    force_emergency_fallback();
    const bool emergency_started = begin_emergency_transition();
    const bool emergency_finished =
        emergency_started && finish_emergency_transition(true);
    result.error = dependency_runtime_error::transition_publish_failed;
    result.coarse_fallback_required = true;
    if (!drained || !emergency_finished) {
      result.error = dependency_runtime_error::emergency_fallback;
    }
    return result;
  }
  if (!g_external_exit_registered) {
    before_shmem_exit(external_prepared_exit, Datum{0});
    g_external_exit_registered = true;
  }
  *token = external_prepared_resolution_token{
      .claim = claimed.claim,
      .database_guard = capability,
      .resolution_identity = resolution_identity,
      .database_oid = database_oid,
      .kind = kind,
      .outcome = external_prepared_resolution_outcome::not_resolved,
      .state = external_prepared_resolution_state::active,
  };
  g_external_prepared_token = *token;
  result.dependency_count = 1;
  result.coarse_fallback_required = g_force_coarse;
  return result;
}

[[nodiscard]] dependency_runtime_result
finish_external_prepared_resolution(
    external_prepared_resolution_token *token,
    external_prepared_resolution_outcome outcome) noexcept {
  using namespace detail;
  dependency_runtime_result result{};
  if (token == nullptr) {
    result.error = dependency_runtime_error::invalid_argument;
    result.coarse_fallback_required = true;
    return result;
  }
  if (token->state == external_prepared_resolution_state::finished) {
    if (token->outcome != outcome) {
      result.error = dependency_runtime_error::invalid_argument;
      result.coarse_fallback_required = true;
      return result;
    }
    result.dependency_count = 1;
    result.coarse_fallback_required =
        outcome == external_prepared_resolution_outcome::ambiguous;
    return result;
  }
  const bool outcome_valid =
      (token->kind == external_prepared_resolution_kind::commit_prepared &&
       (outcome == external_prepared_resolution_outcome::committed ||
        outcome == external_prepared_resolution_outcome::not_resolved ||
        outcome == external_prepared_resolution_outcome::ambiguous)) ||
      (token->kind == external_prepared_resolution_kind::rollback_prepared &&
       (outcome == external_prepared_resolution_outcome::rolled_back ||
        outcome == external_prepared_resolution_outcome::not_resolved ||
        outcome == external_prepared_resolution_outcome::ambiguous));
  const bool exact_active_token =
      static_cast<bool>(*token) && outcome_valid &&
      g_external_prepared_token.state ==
          external_prepared_resolution_state::active &&
      token->claim == g_external_prepared_token.claim &&
      token->database_guard ==
          g_external_prepared_token.database_guard &&
      token->resolution_identity ==
          g_external_prepared_token.resolution_identity &&
      token->database_oid == g_external_prepared_token.database_oid &&
      token->kind == g_external_prepared_token.kind;
  if (!exact_active_token) {
    result.error = dependency_runtime_error::invalid_argument;
    result.coarse_fallback_required = true;
    return result;
  }
  const transition_resolution resolution =
      outcome == external_prepared_resolution_outcome::committed
      ? transition_resolution::committed
      : outcome == external_prepared_resolution_outcome::ambiguous
          ? transition_resolution::aborted
          : transition_resolution::known_aborted;
  const transition_ledger_operation_result finished =
      finish_owner_transitions(
          shared_registry::shared_transition_ledger(), token->claim,
          resolution, ledger_callbacks());
  const bool fully_drained =
      finished.state == transition_owner_state::free &&
      finished.outstanding_transition_count == 0;
  bool emergency_ok = true;
  if (outcome == external_prepared_resolution_outcome::ambiguous ||
      !fully_drained || !finished) {
    force_emergency_fallback();
    const bool emergency_started = begin_emergency_transition();
    emergency_ok =
        emergency_started && finish_emergency_transition(true);
  }
  result.dependency_count = 1;
  result.coarse_fallback_required =
      finished.coarse_fallback_required ||
      outcome == external_prepared_resolution_outcome::ambiguous ||
      !fully_drained || !finished || !emergency_ok;
  if (!fully_drained || !finished) {
    result.error = dependency_runtime_error::transition_finish_failed;
  }
  if (!emergency_ok) {
    result.error = dependency_runtime_error::emergency_fallback;
  }
  if (!fully_drained) {
    return result;
  }
  const bool global_token = token == &g_external_prepared_token;
  token->outcome = outcome;
  token->state = external_prepared_resolution_state::finished;
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
    dependency_runtime_test_hooks hooks) noexcept {
  detail::g_test_hooks = hooks;
}

[[nodiscard]] dependency_runtime_diagnostics
read_dependency_runtime_diagnostics() noexcept {
  using namespace detail;
  dependency_runtime_diagnostics result = g_diagnostics;
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
