/**
 * @file bioseq_caching/pg/shared_registry.cppm
 * @brief Lazy pinned-DSA bootstrap for the PostgreSQL cache shared root.
 *
 * Main shared memory contains only the bounded bootstrap header and explicitly
 * requested fixed sidecars.  The reusable exact/coverage registry, dependency
 * directory, and optional adapter metadata live in one normal DSA area.
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "miscadmin.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/backend_status.h"
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.pg.shared_registry;

import bioseq.cache.core;

export namespace bioseq::cache::pg::shared_registry {

inline constexpr std::uint32_t kSharedRegistryBootstrapMagicV1 =
    0x42534231U;
inline constexpr std::uint32_t kSharedRegistryBootstrapAbiVersionV1 = 1;
inline constexpr std::uint32_t kSharedRegistryRootMagicV1 = 0x42535231U;
inline constexpr std::uint32_t kSharedRegistryRootAbiVersionV1 = 1;
inline constexpr std::uint32_t kSharedRegistryTestMagicV1 = 0x42535431U;
inline constexpr std::uint32_t kSharedRegistryTestAbiVersionV1 = 2;
inline constexpr std::uint32_t kDefaultSharedRegistryMaxBackendsV1 = 128;
inline constexpr std::uint32_t kDefaultSharedRegistryWaitTimeoutMsV1 = 1'000;
inline constexpr std::uint32_t kDefaultSharedRegistryWaitLimitV1 = 60;
inline constexpr std::uint32_t kMaximumSharedRegistryWaitLimitV1 = 600;
inline constexpr std::uint64_t kDefaultSharedRegistryDsaLimitBytesV1 =
    512ULL << 20U;
inline constexpr std::uint32_t kMaximumSharedRegistryTestControlsV1 = 4;
inline constexpr std::uint32_t kInvalidSharedRegistryProcNumberV1 =
    (std::numeric_limits<std::uint32_t>::max)();

#ifdef BIOSEQ_CACHE_TESTING
inline constexpr std::uint32_t kDefaultSharedRegistryTestControlsV1 = 1;
#else
inline constexpr std::uint32_t kDefaultSharedRegistryTestControlsV1 = 0;
#endif

enum class SharedRegistryErrorV1 : std::uint32_t {
  None = 0,
  InvalidArgument,
  InvalidConfiguration,
  NotPreloaded,
  MainShmemUnavailable,
  TransitionLedgerUnavailable,
  TestSidecarUnavailable,
  AbiMismatch,
  NotReady,
  InitializerUnavailable,
  InitializerStillAlive,
  InitializerSelfWait,
  WaitTimeout,
  TakeoverExhausted,
  RecoveryCleanupFailed,
  DsaCreateFailed,
  DsaPinFailed,
  DsaMappingPinFailed,
  DsaSizeLimitFailed,
  RootAllocationFailed,
  ReusableAllocationFailed,
  DependencyAllocationFailed,
  ExtensionAllocationFailed,
  ReusableInitializationFailed,
  DependencyInitializationFailed,
  ExtensionInitializationFailed,
  RootValidationFailed,
  ReusableValidationFailed,
  DependencyValidationFailed,
  ExtensionValidationFailed,
  PublicationLost,
  DsaAttachFailed,
  DsaUnpinFailed,
  BootstrapFailed,
  TestInjectedAllocationFailure,
  TestInjectedAbiMismatch,
  TestInjectedMagicMismatch,
};

enum class SharedRegistryTestPointV1 : std::uint32_t {
  None = 0,
  BeforeHandlePublish = 1,
};

struct SharedRegistryBackendIdentityV1 {
  std::uint32_t proc_number = kInvalidSharedRegistryProcNumberV1;
  std::int32_t pid = 0;
  std::uint64_t backend_nonce = 0;
  std::int64_t backend_started_at_us = 0;

  [[nodiscard]] bool valid() const noexcept {
    return proc_number != kInvalidSharedRegistryProcNumberV1 && pid > 0 &&
           backend_nonce != 0 && backend_started_at_us != 0;
  }

  [[nodiscard]] bool operator==(
      const SharedRegistryBackendIdentityV1 &) const noexcept = default;
};

struct SharedRegistryOwnerTokenV1 {
  SharedRegistryBackendIdentityV1 owner{};
  std::uint64_t claim_sequence = 0;

  [[nodiscard]] bool valid() const noexcept {
    return owner.valid() && claim_sequence != 0;
  }

  [[nodiscard]] bool operator==(
      const SharedRegistryOwnerTokenV1 &) const noexcept = default;
};

struct SharedRegistryConfigV1 {
  ReusableRegistryConfigV1 reusable{};
  std::uint32_t max_backends = kDefaultSharedRegistryMaxBackendsV1;
  std::uint32_t dependency_partition_count =
      kDefaultDependencyDirectoryPartitionsV1;
  std::uint32_t dependency_reclaim_scan_limit =
      kDefaultDependencyReclaimScanV1;
  std::uint32_t transition_precise_reference_capacity =
      kMaximumTransitionReferencesV1;
  std::uint32_t extension_metadata_bytes = 0;
  std::uint32_t test_control_count =
      kDefaultSharedRegistryTestControlsV1;
  std::uint32_t wait_timeout_ms =
      kDefaultSharedRegistryWaitTimeoutMsV1;
  std::uint32_t wait_limit = kDefaultSharedRegistryWaitLimitV1;
  std::uint64_t dsa_size_limit_bytes =
      kDefaultSharedRegistryDsaLimitBytesV1;
  std::uint32_t reserved = 0;
};

struct SharedRegistryPersistentLayoutV1 {
  std::uint32_t root_bytes = 0;
  std::uint32_t reusable_bytes = 0;
  std::uint32_t reusable_max_generations = 0;
  std::uint32_t reusable_exact_capacity = 0;
  std::uint32_t reusable_coverage_capacity = 0;
  std::uint32_t reusable_partition_count = 0;
  std::uint32_t reusable_wait_cv_bytes = 0;
  std::uint32_t reusable_wait_cv_capacity = 0;
  std::uint32_t dependency_bytes = 0;
  std::uint32_t dependency_logical_capacity = 0;
  std::uint32_t dependency_physical_capacity = 0;
  std::uint32_t dependency_partition_count = 0;
  std::uint32_t transition_ledger_bytes = 0;
  std::uint32_t transition_owner_capacity = 0;
  std::uint32_t transition_references_per_owner = 0;
  std::uint32_t extension_metadata_bytes = 0;
  std::uint32_t test_control_count = 0;
  std::uint32_t wait_timeout_ms = 0;
  std::uint32_t wait_limit = 0;
  std::uint32_t reserved = 0;
  std::uint64_t dsa_size_limit_bytes = 0;

  [[nodiscard]] bool operator==(
      const SharedRegistryPersistentLayoutV1 &) const noexcept = default;
};

struct SharedRegistryPreloadRequestV1 {
  SharedRegistryConfigV1 requested_config{};
  ReusableRegistryPreloadRequestV1 reusable{};
  DependencyDirectoryPreloadRequestV1 dependency{};
  TransitionLedgerPreloadResultV1 transition{};
  SharedRegistryPersistentLayoutV1 persistent{};
  SharedRegistryErrorV1 error = SharedRegistryErrorV1::None;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SharedRegistryErrorV1::None;
  }
};

/** Immutable root plus shared counters; every address is a DSA relative value. */
struct SharedRegistryRootV1 {
  std::uint32_t magic = 0;
  std::uint32_t abi_version = 0;
  std::uint32_t root_bytes = 0;
  std::uint32_t reserved = 0;
  std::uint64_t boot_nonce = 0;
  SharedRegistryPersistentLayoutV1 layout{};
  dsa_pointer reusable_registry = InvalidDsaPointer;
  dsa_pointer reusable_wait_cvs = InvalidDsaPointer;
  dsa_pointer dependency_directory = InvalidDsaPointer;
  dsa_pointer extension_metadata = InvalidDsaPointer;
  pg_atomic_uint64 hot_live_bytes{};
  pg_atomic_uint64 hot_allocation_failures{};
};

/** Bounded main-shmem publication and recovery record. */
struct SharedRegistryBootstrapHeaderV1 {
  std::uint32_t magic = 0;
  std::uint32_t abi_version = 0;
  std::uint32_t header_bytes = 0;
  std::uint32_t dsa_tranche_id = 0;
  SharedRegistryPersistentLayoutV1 layout{};
  pg_atomic_uint32 state{};
  pg_atomic_uint32 failure_code{};
  pg_atomic_uint64 boot_nonce{};
  pg_atomic_uint64 next_backend_nonce{};
  pg_atomic_uint64 next_claim_sequence{};
  pg_atomic_uint64 emergency_write_epoch{};
  pg_atomic_uint64 emergency_change_generation{};
  pg_atomic_uint32 emergency_transition_count{};
  ConditionVariable emergency_state_changed{};
  pg_atomic_uint64 reserved_dsa_bytes{};
  pg_atomic_uint64 reserved_file_bytes{};
  pg_atomic_uint64 complete_entries{};
  pg_atomic_uint64 inflight_entries{};
  pg_atomic_uint64 indexed_generations{};
  pg_atomic_uint64 retired_generations{};
  pg_atomic_uint64 evictions{};
  pg_atomic_uint64 invalidations{};
  ConditionVariable state_changed{};
  SharedRegistryOwnerTokenV1 initializer{};
  SharedRegistryOwnerTokenV1 last_initializer{};
  std::uint64_t initialization_attempt_count = 0;
  std::uint64_t takeover_count = 0;
  std::uint64_t cleanup_count = 0;
  std::uint64_t wait_count = 0;
  std::uint64_t attach_count = 0;
  std::uint64_t validation_failure_count = 0;
  std::uint32_t takeover_used = 0;
  std::uint32_t cleanup_handle_pinned = 0;
  std::uint32_t live_unpublished_area_count = 0;
  std::uint32_t orderly_unpinned = 0;
  std::uint32_t exact_directory_capacity = 0;
  std::uint32_t coverage_directory_capacity = 0;
  std::uint32_t dependency_directory_capacity = 0;
  std::uint32_t directory_partition_count = 0;
  dsa_handle cleanup_handle = DSA_HANDLE_INVALID;
  dsa_handle published_handle = DSA_HANDLE_INVALID;
  dsa_pointer published_root = InvalidDsaPointer;
  dsa_pointer published_reusable = InvalidDsaPointer;
  dsa_pointer published_reusable_wait_cvs = InvalidDsaPointer;
  dsa_pointer published_dependency = InvalidDsaPointer;
  dsa_pointer published_extension = InvalidDsaPointer;
  dsa_pointer published_retired_generation_head = InvalidDsaPointer;
};

struct SharedRegistryTestControlV1 {
  pg_atomic_uint32 pause_point{};
  pg_atomic_uint32 reached{};
  pg_atomic_uint32 released{};
  pg_atomic_uint32 defer_background_initializer{};
  pg_atomic_uint32 allocation_failure_ordinal{};
  pg_atomic_uint32 inject_root_abi_mismatch{};
  pg_atomic_uint32 inject_root_magic_mismatch{};
  pg_atomic_uint32 allocation_counter{};
  pg_atomic_uint32 last_cleanup_handle{};
  pg_atomic_uint32 last_cleanup_handle_was_pinned{};
  pg_atomic_uint32 last_cleanup_release_verified{};
  ConditionVariable changed{};
};

struct SharedRegistryTestSidecarV1 {
  std::uint32_t magic = 0;
  std::uint32_t abi_version = 0;
  std::uint32_t control_count = 0;
  std::uint32_t reserved = 0;
  std::uint64_t boot_nonce = 0;
  std::array<SharedRegistryTestControlV1,
             kMaximumSharedRegistryTestControlsV1>
      controls{};
};

struct SharedRegistryCallbacksV1 {
  void *context = nullptr;
  SharedRegistryBackendIdentityV1 (*current_owner)(void *) noexcept = nullptr;
  bool (*owner_is_alive)(
      void *, const SharedRegistryOwnerTokenV1 &) noexcept = nullptr;
  bool (*initialize_extension)(
      void *, std::span<std::byte>, std::uint64_t) noexcept = nullptr;
  bool (*validate_extension)(
      void *, std::span<std::byte>, std::uint64_t) noexcept = nullptr;
};

/** Process-local attachment.  None of these addresses enters shared memory. */
struct SharedRegistryAttachmentV1 {
  dsa_area *area = nullptr;
  SharedRegistryRootV1 *root = nullptr;
  ReusableRegistryAttachmentV1 reusable{};
  std::span<ConditionVariable> reusable_wait_cvs{};
  DependencyDirectoryAttachmentV1 dependency{};
  std::span<std::byte> extension_metadata{};
  dsa_handle handle = DSA_HANDLE_INVALID;
  bool mapping_pinned = false;
  std::array<std::uint8_t, 3> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return area != nullptr && root != nullptr &&
           static_cast<bool>(reusable) && !reusable_wait_cvs.empty() &&
           static_cast<bool>(dependency) &&
           handle != DSA_HANDLE_INVALID && mapping_pinned;
  }
};

struct SharedRegistryAttachResultV1 {
  SharedRegistryAttachmentV1 attachment{};
  SharedRegistryErrorV1 error = SharedRegistryErrorV1::None;
  BootstrapState state = BootstrapState::Uninitialized;
  bool initialized_here = false;
  bool takeover = false;
  std::array<std::uint8_t, 2> reserved{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SharedRegistryErrorV1::None &&
           static_cast<bool>(attachment);
  }
};

struct SharedRegistryDiagnosticsV1 {
  BootstrapState state = BootstrapState::Uninitialized;
  SharedRegistryErrorV1 failure = SharedRegistryErrorV1::None;
  std::uint64_t boot_nonce = 0;
  std::uint64_t emergency_write_epoch = 0;
  std::uint64_t emergency_change_generation = 0;
  std::uint64_t reserved_dsa_bytes = 0;
  std::uint64_t reserved_file_bytes = 0;
  std::uint64_t complete_entries = 0;
  std::uint64_t inflight_entries = 0;
  std::uint64_t indexed_generations = 0;
  std::uint64_t retired_generations = 0;
  std::uint64_t evictions = 0;
  std::uint64_t invalidations = 0;
  SharedRegistryPersistentLayoutV1 layout{};
  SharedRegistryOwnerTokenV1 initializer{};
  SharedRegistryOwnerTokenV1 last_initializer{};
  std::uint64_t initialization_attempt_count = 0;
  std::uint64_t takeover_count = 0;
  std::uint64_t cleanup_count = 0;
  std::uint64_t wait_count = 0;
  std::uint64_t attach_count = 0;
  std::uint64_t validation_failure_count = 0;
  std::uint32_t takeover_used = 0;
  std::uint32_t live_unpublished_area_count = 0;
  std::uint32_t cleanup_handle_pinned = 0;
  std::uint32_t orderly_unpinned = 0;
  std::uint32_t emergency_transition_count = 0;
  std::uint32_t exact_directory_capacity = 0;
  std::uint32_t coverage_directory_capacity = 0;
  std::uint32_t dependency_directory_capacity = 0;
  std::uint32_t directory_partition_count = 0;
  bool cleanup_handle_published = false;
  bool ready_handle_published = false;
  bool root_published = false;
  std::uint8_t reserved = 0;
};

struct SharedRegistryTestStatusV1 {
  SharedRegistryTestPointV1 pause_point =
      SharedRegistryTestPointV1::None;
  bool reached = false;
  bool released = false;
  bool defer_background_initializer = false;
  bool inject_root_abi_mismatch = false;
  bool inject_root_magic_mismatch = false;
  bool last_cleanup_handle_was_pinned = false;
  bool last_cleanup_release_verified = false;
  std::uint32_t allocation_failure_ordinal = 0;
  std::uint32_t allocation_counter = 0;
  std::uint32_t last_cleanup_handle = 0;
};

#ifdef BIOSEQ_CACHE_TESTING
enum class SharedRegistryTestManifestCorruptionV1 : std::uint32_t {
  Magic = 1,
  AbiVersion = 2,
  BootNonce = 3,
};
#endif

/** Process-local typed view of the preallocated emergency dependency. */
struct SharedRegistryEmergencyAccessV1 {
  pg_atomic_uint64 *write_epoch = nullptr;
  pg_atomic_uint64 *change_generation = nullptr;
  pg_atomic_uint32 *transition_count = nullptr;
  ConditionVariable *state_changed = nullptr;

  [[nodiscard]] explicit operator bool() const noexcept {
    return write_epoch != nullptr && change_generation != nullptr &&
           transition_count != nullptr && state_changed != nullptr;
  }
};

/** Process-local typed view of bounded bootstrap accounting counters. */
struct SharedRegistryAccountingAccessV1 {
  pg_atomic_uint64 *reserved_dsa_bytes = nullptr;
  pg_atomic_uint64 *reserved_file_bytes = nullptr;
  pg_atomic_uint64 *complete_entries = nullptr;
  pg_atomic_uint64 *inflight_entries = nullptr;
  pg_atomic_uint64 *indexed_generations = nullptr;
  pg_atomic_uint64 *retired_generations = nullptr;
  pg_atomic_uint64 *evictions = nullptr;
  pg_atomic_uint64 *invalidations = nullptr;

  [[nodiscard]] explicit operator bool() const noexcept {
    return reserved_dsa_bytes != nullptr && reserved_file_bytes != nullptr &&
           complete_entries != nullptr && inflight_entries != nullptr &&
           indexed_generations != nullptr && retired_generations != nullptr &&
           evictions != nullptr && invalidations != nullptr;
  }
};

static_assert(std::is_trivially_copyable_v<SharedRegistryBackendIdentityV1>);
static_assert(std::is_trivially_copyable_v<SharedRegistryOwnerTokenV1>);
static_assert(std::is_trivially_copyable_v<SharedRegistryPersistentLayoutV1>);
static_assert(std::is_trivially_copyable_v<SharedRegistryRootV1>);
static_assert(std::is_trivially_copyable_v<SharedRegistryBootstrapHeaderV1>);
static_assert(std::is_standard_layout_v<SharedRegistryRootV1>);
static_assert(std::is_standard_layout_v<SharedRegistryBootstrapHeaderV1>);
static_assert(std::is_standard_layout_v<SharedRegistryTestSidecarV1>);
static_assert(alignof(SharedRegistryBootstrapHeaderV1) <= MAXIMUM_ALIGNOF);
static_assert(alignof(SharedRegistryTestSidecarV1) <= MAXIMUM_ALIGNOF);

namespace shared_registry_detail {

inline constexpr char kBootstrapShmemName[] =
    "bioseq_cache shared registry bootstrap v1";
inline constexpr char kTransitionShmemName[] =
    "bioseq_cache transition ledger v1";
inline constexpr char kTestShmemName[] =
    "bioseq_cache shared registry tests v1";
inline constexpr char kBootstrapLockTranche[] =
    "bioseq_cache shared registry bootstrap lock v1";
inline constexpr char kTransitionLockTranche[] =
    "bioseq_cache transition owner locks v1";
inline constexpr char kDependencyLockTranche[] =
    "bioseq_cache dependency partition locks v1";
inline constexpr char kReusableMembershipLockTranche[] =
    "bioseq_cache shared reusable membership lock v1";
inline constexpr char kReusableExactLockTranche[] =
    "bioseq_cache shared reusable exact locks v1";
inline constexpr char kReusableCoverageLockTranche[] =
    "bioseq_cache shared reusable coverage locks v1";
inline constexpr char kDsaTrancheName[] = "bioseq_cache shared root dsa v1";

inline SharedRegistryBootstrapHeaderV1 *g_bootstrap = nullptr;
inline SharedRegistryTestSidecarV1 *g_test_sidecar = nullptr;
inline LWLock *g_bootstrap_lock = nullptr;
inline LWLockPadded *g_transition_locks = nullptr;
inline LWLockPadded *g_dependency_locks = nullptr;
inline LWLockPadded *g_reusable_membership_lock = nullptr;
inline LWLockPadded *g_reusable_exact_locks = nullptr;
inline LWLockPadded *g_reusable_coverage_locks = nullptr;
inline TransitionLedgerAttachmentV1 g_transition_ledger{};
inline SharedRegistryPreloadRequestV1 g_requested{};
inline SharedRegistryAttachmentV1 g_attachment{};
inline dsa_area *g_initializing_area = nullptr;
inline SharedRegistryOwnerTokenV1 g_initializing_owner{};
inline bool g_initializing_area_pinned = false;
inline bool g_initializing_mapping_pinned = false;
inline bool g_exit_callback_registered = false;

[[nodiscard]] constexpr bool valid_state(std::uint32_t raw) noexcept {
  return raw <= static_cast<std::uint32_t>(BootstrapState::Failed);
}

[[nodiscard]] constexpr bool valid_error(std::uint32_t raw) noexcept {
  return raw <= static_cast<std::uint32_t>(
                    SharedRegistryErrorV1::TestInjectedMagicMismatch);
}

[[nodiscard]] BootstrapState load_state() noexcept {
  if (g_bootstrap == nullptr) {
    return BootstrapState::Failed;
  }
  const std::uint32_t raw = pg_atomic_read_u32(&g_bootstrap->state);
  return valid_state(raw) ? static_cast<BootstrapState>(raw)
                          : BootstrapState::Failed;
}

[[nodiscard]] std::uint64_t load_boot_nonce() noexcept {
  return g_bootstrap == nullptr
             ? 0
             : pg_atomic_read_u64(&g_bootstrap->boot_nonce);
}

void store_state(BootstrapState state) noexcept {
  pg_write_barrier();
  pg_atomic_write_u32(
      &g_bootstrap->state, static_cast<std::uint32_t>(state));
}

[[nodiscard]] SharedRegistryErrorV1 load_failure() noexcept {
  if (g_bootstrap == nullptr) {
    return SharedRegistryErrorV1::NotPreloaded;
  }
  return static_cast<SharedRegistryErrorV1>(
      pg_atomic_read_u32(&g_bootstrap->failure_code));
}

void store_failure(SharedRegistryErrorV1 error) noexcept {
  pg_atomic_write_u32(
      &g_bootstrap->failure_code, static_cast<std::uint32_t>(error));
}

void saturating_increment(std::uint64_t &value) noexcept {
  if (value != (std::numeric_limits<std::uint64_t>::max)()) {
    ++value;
  }
}

[[nodiscard]] constexpr std::uint64_t backend_nonce(
    std::uint32_t proc_number, std::int32_t pid,
    std::int64_t started_at_us) noexcept {
  const std::uint64_t value = static_cast<std::uint64_t>(started_at_us) ^
                              (static_cast<std::uint64_t>(
                                   static_cast<std::uint32_t>(pid))
                               << 32U) ^
                              proc_number;
  return value == 0 ? 1 : value;
}

[[nodiscard]] SharedRegistryBackendIdentityV1
default_current_owner(void *) noexcept {
  const std::uint32_t proc_number =
      static_cast<std::uint32_t>(MyProcNumber);
  const std::int64_t started_at =
      static_cast<std::int64_t>(MyStartTimestamp);
  if (MyProcPid <= 0 ||
      proc_number == static_cast<std::uint32_t>(INVALID_PROC_NUMBER) ||
      started_at == 0) {
    return {};
  }
  return SharedRegistryBackendIdentityV1{
      .proc_number = proc_number,
      .pid = MyProcPid,
      .backend_nonce = backend_nonce(proc_number, MyProcPid, started_at),
      .backend_started_at_us = started_at,
  };
}

[[nodiscard]] bool default_owner_is_alive(
    void *, const SharedRegistryOwnerTokenV1 &token) noexcept {
  const SharedRegistryBackendIdentityV1 &owner = token.owner;
  if (!token.valid() || ProcGlobal == nullptr ||
      owner.proc_number >=
          static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
    return false;
  }
  PGPROC *process = ProcNumberGetProc(
      static_cast<ProcNumber>(owner.proc_number));
  if (process == nullptr || process->pid != owner.pid) {
    return false;
  }
  volatile PgBackendStatus *status = pgstat_get_beentry_by_proc_number(
      static_cast<ProcNumber>(owner.proc_number));
  /* A matching PGPROC is live until a stable activity record proves that
   * its full start-token belongs to another backend.  Missing/unstable
   * statistics are never evidence of death. */
  if (status == nullptr) {
    return true;
  }
  std::int32_t status_pid = 0;
  TimestampTz status_started_at = 0;
  bool stable = false;
  for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
    int before_changecount = 0;
    int after_changecount = 0;
    pgstat_begin_read_activity(status, before_changecount);
    status_pid = status->st_procpid;
    status_started_at = status->st_proc_start_timestamp;
    pgstat_end_read_activity(status, after_changecount);
    if (pgstat_read_activity_complete(
            before_changecount, after_changecount)) {
      stable = true;
      break;
    }
  }
  if (!stable) {
    return true;
  }
  return status_pid == owner.pid &&
         status_started_at == owner.backend_started_at_us &&
         backend_nonce(
             owner.proc_number, owner.pid, status_started_at) ==
             owner.backend_nonce;
}

[[nodiscard]] SharedRegistryBackendIdentityV1 current_owner(
    const SharedRegistryCallbacksV1 &callbacks) noexcept {
  return callbacks.current_owner == nullptr
             ? default_current_owner(nullptr)
             : callbacks.current_owner(callbacks.context);
}

[[nodiscard]] bool owner_is_alive(
    const SharedRegistryCallbacksV1 &callbacks,
    const SharedRegistryOwnerTokenV1 &owner) noexcept {
  return callbacks.owner_is_alive == nullptr
             ? default_owner_is_alive(nullptr, owner)
             : callbacks.owner_is_alive(callbacks.context, owner);
}

[[nodiscard]] bool request_matches(
    const SharedRegistryPreloadRequestV1 &left,
    const SharedRegistryPreloadRequestV1 &right) noexcept {
  return left && right && left.persistent == right.persistent &&
         std::memcmp(
             &left.reusable.config, &right.reusable.config,
             sizeof(left.reusable.config)) == 0 &&
         std::memcmp(
             &left.reusable.layout, &right.reusable.layout,
             sizeof(left.reusable.layout)) == 0 &&
         left.dependency.config == right.dependency.config &&
         left.dependency.layout == right.dependency.layout &&
         left.transition.config.owner_capacity ==
             right.transition.config.owner_capacity &&
         left.transition.config.precise_reference_capacity ==
             right.transition.config.precise_reference_capacity &&
         left.transition.config.dependency_partition_count ==
             right.transition.config.dependency_partition_count &&
         left.transition.layout.total_bytes ==
             right.transition.layout.total_bytes &&
         left.transition.layout.owners_offset ==
             right.transition.layout.owners_offset &&
         left.transition.layout.references_offset ==
             right.transition.layout.references_offset &&
         left.transition.layout.references_per_owner ==
             right.transition.layout.references_per_owner;
}

[[nodiscard]] bool bootstrap_matches(
    const SharedRegistryPreloadRequestV1 &request,
    std::uint64_t boot_nonce) noexcept {
  return g_bootstrap != nullptr &&
         g_bootstrap->magic == kSharedRegistryBootstrapMagicV1 &&
         g_bootstrap->abi_version ==
             kSharedRegistryBootstrapAbiVersionV1 &&
         g_bootstrap->header_bytes ==
             sizeof(SharedRegistryBootstrapHeaderV1) &&
         load_boot_nonce() == boot_nonce &&
         g_bootstrap->layout == request.persistent &&
         g_bootstrap->dsa_tranche_id != 0 &&
         g_bootstrap->exact_directory_capacity ==
             request.persistent.reusable_exact_capacity &&
         g_bootstrap->coverage_directory_capacity ==
             request.persistent.reusable_coverage_capacity &&
         g_bootstrap->dependency_directory_capacity ==
             request.persistent.dependency_physical_capacity &&
         g_bootstrap->directory_partition_count ==
             request.persistent.dependency_partition_count &&
         valid_state(pg_atomic_read_u32(&g_bootstrap->state)) &&
         valid_error(pg_atomic_read_u32(&g_bootstrap->failure_code));
}

[[nodiscard]] SharedRegistryOwnerTokenV1 claim_owner_locked(
    const SharedRegistryBackendIdentityV1 &identity) noexcept {
  std::uint64_t sequence = pg_atomic_fetch_add_u64(
      &g_bootstrap->next_claim_sequence, 1);
  if (sequence == 0) {
    sequence = pg_atomic_fetch_add_u64(
        &g_bootstrap->next_claim_sequence, 1);
  }
  if (sequence == 0) {
    sequence = 1;
    pg_atomic_write_u64(&g_bootstrap->next_claim_sequence, 2);
  }
  const SharedRegistryOwnerTokenV1 result{
      .owner = identity,
      .claim_sequence = sequence,
  };
  g_bootstrap->initializer = result;
  saturating_increment(g_bootstrap->initialization_attempt_count);
  store_failure(SharedRegistryErrorV1::None);
  store_state(BootstrapState::Initializing);
  return result;
}

[[nodiscard]] bool try_create_area(
    std::uint32_t tranche_id, dsa_area **output) noexcept {
  if (output == nullptr || TopMemoryContext == nullptr) {
    return false;
  }
  *output = nullptr;
  MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
  PG_TRY();
  {
    *output = dsa_create(static_cast<int>(tranche_id));
  }
  PG_CATCH();
  {
    *output = nullptr;
    FlushErrorState();
  }
  PG_END_TRY();
  MemoryContextSwitchTo(previous);
  return *output != nullptr;
}

[[nodiscard]] bool try_attach_area(
    dsa_handle handle, dsa_area **output) noexcept {
  if (output == nullptr || handle == DSA_HANDLE_INVALID ||
      TopMemoryContext == nullptr) {
    return false;
  }
  *output = nullptr;
  MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
  PG_TRY();
  {
    *output = dsa_attach(handle);
  }
  PG_CATCH();
  {
    *output = nullptr;
    FlushErrorState();
  }
  PG_END_TRY();
  MemoryContextSwitchTo(previous);
  return *output != nullptr;
}

[[nodiscard]] bool try_pin_area(dsa_area *area) noexcept {
  bool success = true;
  PG_TRY();
  {
    dsa_pin(area);
  }
  PG_CATCH();
  {
    success = false;
    FlushErrorState();
  }
  PG_END_TRY();
  return success;
}

[[nodiscard]] bool try_pin_mapping(dsa_area *area) noexcept {
  bool success = true;
  PG_TRY();
  {
    dsa_pin_mapping(area);
  }
  PG_CATCH();
  {
    success = false;
    FlushErrorState();
  }
  PG_END_TRY();
  return success;
}

[[nodiscard]] bool try_unpin_area(dsa_area *area) noexcept {
  bool success = true;
  PG_TRY();
  {
    dsa_unpin(area);
  }
  PG_CATCH();
  {
    success = false;
    FlushErrorState();
  }
  PG_END_TRY();
  return success;
}

[[nodiscard]] bool try_set_size_limit(
    dsa_area *area, std::uint64_t limit) noexcept {
  if (limit > (std::numeric_limits<std::size_t>::max)()) {
    return false;
  }
  bool success = true;
  PG_TRY();
  {
    dsa_set_size_limit(area, static_cast<std::size_t>(limit));
  }
  PG_CATCH();
  {
    success = false;
    FlushErrorState();
  }
  PG_END_TRY();
  return success;
}

void detach_area(dsa_area *area) noexcept {
  if (area != nullptr) {
    dsa_detach(area);
  }
}

[[nodiscard]] bool extension_callbacks_valid(
    const SharedRegistryPreloadRequestV1 &request,
    const SharedRegistryCallbacksV1 &callbacks) noexcept {
  return request.persistent.extension_metadata_bytes == 0 ||
         (callbacks.initialize_extension != nullptr &&
          callbacks.validate_extension != nullptr);
}

void acquire_dependency_partition(
    void *, std::uint32_t partition) noexcept {
  LWLockAcquire(&g_dependency_locks[partition].lock, LW_EXCLUSIVE);
}

void release_dependency_partition(
    void *, std::uint32_t partition) noexcept {
  LWLockRelease(&g_dependency_locks[partition].lock);
}

[[nodiscard]] DependencyDirectoryCallbacksV1 dependency_callbacks(
    bool unpublished) noexcept {
  if (unpublished) {
    return DependencyDirectoryCallbacksV1{
        .lock_mode = DependencyDirectoryLockModeV1::AlreadyLocked,
    };
  }
  return DependencyDirectoryCallbacksV1{
      .lock_mode = DependencyDirectoryLockModeV1::AcquireRelease,
      .acquire_partition = acquire_dependency_partition,
      .release_partition = release_dependency_partition,
  };
}

[[nodiscard]] SharedRegistryTestControlV1 *test_control() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  if (g_test_sidecar != nullptr && g_test_sidecar->control_count != 0) {
    return &g_test_sidecar->controls[0];
  }
#endif
  return nullptr;
}

/** Arm a pre-postmaster test fault before the janitor can initialize DSA. */
void configure_test_control_from_environment(
    SharedRegistryTestControlV1 &control) noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  const char *fault = std::getenv("BIOSEQ_CACHE_TEST_BOOTSTRAP_FAULT");
  if (fault == nullptr || fault[0] == '\0') {
    return;
  }
  if (std::strcmp(fault, "pause_backend_once") == 0) {
    pg_atomic_write_u32(
        &control.pause_point,
        static_cast<std::uint32_t>(
            SharedRegistryTestPointV1::BeforeHandlePublish));
    pg_atomic_write_u32(&control.defer_background_initializer, 1);
    return;
  }
  if (std::strcmp(fault, "pause_once") == 0) {
    pg_atomic_write_u32(
        &control.pause_point,
        static_cast<std::uint32_t>(
            SharedRegistryTestPointV1::BeforeHandlePublish));
    return;
  }
  if (std::strncmp(fault, "alloc_", 6) == 0 &&
      fault[6] >= '1' && fault[6] <= '5' && fault[7] == '\0') {
    pg_atomic_write_u32(
        &control.allocation_failure_ordinal,
        static_cast<std::uint32_t>(fault[6] - '0'));
    return;
  }
  if (std::strcmp(fault, "root_abi") == 0) {
    pg_atomic_write_u32(&control.inject_root_abi_mismatch, 1);
    return;
  }
  if (std::strcmp(fault, "root_magic") == 0) {
    pg_atomic_write_u32(&control.inject_root_magic_mismatch, 1);
  }
#else
  static_cast<void>(control);
#endif
}

/** Keep the janitor from consuming a backend-targeted one-shot pause. */
[[nodiscard]] bool defer_background_initializer() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  SharedRegistryTestControlV1 *control = test_control();
  return control != nullptr && AmBackgroundWorkerProcess() &&
         pg_atomic_read_u32(&control->defer_background_initializer) != 0;
#else
  return false;
#endif
}

[[nodiscard]] bool inject_allocation_failure(
    std::uint32_t ordinal) noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr) {
    return false;
  }
  pg_atomic_write_u32(&control->allocation_counter, ordinal);
  return pg_atomic_read_u32(&control->allocation_failure_ordinal) == ordinal;
#else
  static_cast<void>(ordinal);
  return false;
#endif
}

[[nodiscard]] bool inject_root_abi_mismatch() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  SharedRegistryTestControlV1 *control = test_control();
  return control != nullptr &&
         pg_atomic_read_u32(&control->inject_root_abi_mismatch) != 0;
#else
  return false;
#endif
}

[[nodiscard]] bool inject_root_magic_mismatch() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  SharedRegistryTestControlV1 *control = test_control();
  return control != nullptr &&
         pg_atomic_read_u32(&control->inject_root_magic_mismatch) != 0;
#else
  return false;
#endif
}

void pause_before_publication() {
#ifdef BIOSEQ_CACHE_TESTING
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr ||
      pg_atomic_read_u32(&control->pause_point) !=
          static_cast<std::uint32_t>(
              SharedRegistryTestPointV1::BeforeHandlePublish)) {
    return;
  }
  std::uint32_t expected = 0;
  if (!pg_atomic_compare_exchange_u32(
          &control->reached, &expected, 1)) {
    return;
  }
  ConditionVariableBroadcast(&control->changed);
  ConditionVariablePrepareToSleep(&control->changed);
  while (pg_atomic_read_u32(&control->released) == 0) {
    static_cast<void>(ConditionVariableTimedSleep(
        &control->changed, 100, PG_WAIT_EXTENSION));
    CHECK_FOR_INTERRUPTS();
  }
  static_cast<void>(ConditionVariableCancelSleep());
#endif
}

[[nodiscard]] dsa_pointer allocate_zeroed(
    dsa_area *area, std::size_t bytes, std::uint32_t ordinal) noexcept {
  if (bytes == 0 || inject_allocation_failure(ordinal)) {
    return InvalidDsaPointer;
  }
  return dsa_allocate_extended(
      area, bytes, DSA_ALLOC_NO_OOM | DSA_ALLOC_ZERO);
}

[[nodiscard]] bool root_layout_matches(
    const SharedRegistryRootV1 &root,
    const SharedRegistryPreloadRequestV1 &request,
    std::uint64_t boot_nonce) noexcept {
  const bool extension_pointer_valid =
      request.persistent.extension_metadata_bytes == 0
          ? root.extension_metadata == InvalidDsaPointer
          : DsaPointerIsValid(root.extension_metadata);
  return root.magic == kSharedRegistryRootMagicV1 &&
         root.abi_version == kSharedRegistryRootAbiVersionV1 &&
         root.root_bytes == sizeof(SharedRegistryRootV1) &&
         root.reserved == 0 && root.boot_nonce == boot_nonce &&
         root.layout == request.persistent &&
         DsaPointerIsValid(root.reusable_registry) &&
         DsaPointerIsValid(root.reusable_wait_cvs) &&
         DsaPointerIsValid(root.dependency_directory) &&
         extension_pointer_valid &&
         root.reusable_registry != root.reusable_wait_cvs &&
         root.reusable_registry != root.dependency_directory &&
         root.reusable_wait_cvs != root.dependency_directory &&
         (request.persistent.extension_metadata_bytes == 0 ||
          (root.extension_metadata != root.reusable_registry &&
           root.extension_metadata != root.reusable_wait_cvs &&
           root.extension_metadata != root.dependency_directory));
}

[[nodiscard]] SharedRegistryAttachResultV1 validate_area(
    dsa_area *area, dsa_handle handle, dsa_pointer root_pointer,
    const SharedRegistryPreloadRequestV1 &request,
    const SharedRegistryCallbacksV1 &callbacks, std::uint64_t boot_nonce,
    bool unpublished) noexcept {
  SharedRegistryAttachResultV1 result{};
  if (area == nullptr || handle == DSA_HANDLE_INVALID ||
      !DsaPointerIsValid(root_pointer)) {
    result.error = SharedRegistryErrorV1::RootValidationFailed;
    return result;
  }
  auto *root = static_cast<SharedRegistryRootV1 *>(
      dsa_get_address(area, root_pointer));
  if (root == nullptr || !root_layout_matches(*root, request, boot_nonce)) {
    result.error = SharedRegistryErrorV1::RootValidationFailed;
    return result;
  }

  auto *reusable_bytes = static_cast<std::byte *>(
      dsa_get_address(area, root->reusable_registry));
  if (reusable_bytes == nullptr) {
    result.error = SharedRegistryErrorV1::ReusableValidationFailed;
    return result;
  }
  auto reusable = attach_reusable_registry(std::span<std::byte>{
      reusable_bytes, request.persistent.reusable_bytes});
  if (!reusable.has_value()) {
    result.error = SharedRegistryErrorV1::ReusableValidationFailed;
    return result;
  }
  if (unpublished &&
      validate_reusable_registry(*reusable) !=
          ReusableRegistryErrorV1::None) {
    result.error = SharedRegistryErrorV1::ReusableValidationFailed;
    return result;
  }

  auto *wait_cvs = static_cast<ConditionVariable *>(
      dsa_get_address(area, root->reusable_wait_cvs));
  if (wait_cvs == nullptr ||
      request.persistent.reusable_wait_cv_capacity == 0 ||
      request.persistent.reusable_wait_cv_bytes !=
          request.persistent.reusable_wait_cv_capacity *
              sizeof(ConditionVariable)) {
    result.error = SharedRegistryErrorV1::ReusableValidationFailed;
    return result;
  }

  auto *dependency_bytes = static_cast<std::byte *>(
      dsa_get_address(area, root->dependency_directory));
  if (dependency_bytes == nullptr || g_dependency_locks == nullptr) {
    result.error = SharedRegistryErrorV1::DependencyValidationFailed;
    return result;
  }
  const DependencyDirectoryCallbacksV1 locks =
      dependency_callbacks(unpublished);
  auto dependency = attach_dependency_directory(
      std::span<std::byte>{
          dependency_bytes, request.persistent.dependency_bytes},
      locks);
  if (!dependency) {
    result.error = SharedRegistryErrorV1::DependencyValidationFailed;
    return result;
  }
  if (unpublished &&
      deep_validate_dependency_directory(
          dependency.directory, locks) !=
          DependencyDirectoryErrorV1::None) {
    result.error = SharedRegistryErrorV1::DependencyValidationFailed;
    return result;
  }

  std::span<std::byte> extension{};
  if (request.persistent.extension_metadata_bytes != 0) {
    auto *extension_bytes = static_cast<std::byte *>(
        dsa_get_address(area, root->extension_metadata));
    if (extension_bytes == nullptr || callbacks.validate_extension == nullptr) {
      result.error = SharedRegistryErrorV1::ExtensionValidationFailed;
      return result;
    }
    extension = std::span<std::byte>{
        extension_bytes, request.persistent.extension_metadata_bytes};
    if (!callbacks.validate_extension(
            callbacks.context, extension, boot_nonce)) {
      result.error = SharedRegistryErrorV1::ExtensionValidationFailed;
      return result;
    }
  }

  result.attachment = SharedRegistryAttachmentV1{
      .area = area,
      .root = root,
      .reusable = *reusable,
      .reusable_wait_cvs = std::span<ConditionVariable>{
          wait_cvs, request.persistent.reusable_wait_cv_capacity},
      .dependency = dependency.directory,
      .extension_metadata = extension,
      .handle = handle,
      .mapping_pinned = true,
  };
  result.state = BootstrapState::Ready;
  return result;
}

void clear_process_attachment() noexcept {
  g_attachment = {};
  g_initializing_area = nullptr;
  g_initializing_owner = {};
  g_initializing_area_pinned = false;
  g_initializing_mapping_pinned = false;
}

struct UnpublishedAreaCleanupV1 {
  dsa_handle handle = DSA_HANDLE_INVALID;
  bool was_pinned = false;
  bool pin_removed = true;
  bool released = true;
};

/** Record proof from the real DSM handle rather than inferring from counters. */
void record_test_cleanup(
    const UnpublishedAreaCleanupV1 &cleanup) noexcept {
#ifdef BIOSEQ_CACHE_TESTING
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr) {
    return;
  }
  pg_atomic_write_u32(&control->last_cleanup_handle, cleanup.handle);
  pg_atomic_write_u32(
      &control->last_cleanup_handle_was_pinned,
      cleanup.was_pinned ? 1U : 0U);
  pg_atomic_write_u32(
      &control->last_cleanup_release_verified,
      cleanup.released ? 1U : 0U);
#else
  static_cast<void>(cleanup);
#endif
}

/** Probe whether a released DSA handle can still be attached. */
[[nodiscard]] bool area_handle_released_once(
    dsa_handle handle) noexcept {
  if (handle == DSA_HANDLE_INVALID) {
    return true;
  }
  dsa_area *probe = nullptr;
  if (!try_attach_area(handle, &probe)) {
    return true;
  }
  detach_area(probe);
  return false;
}

/** Wait boundedly for a dead owner's ResourceOwner/DSM cleanup. */
[[nodiscard]] bool wait_for_area_handle_release(
    dsa_handle handle) {
  if (area_handle_released_once(handle)) {
    return true;
  }
  const std::uint32_t attempts = std::max<std::uint32_t>(
      1, g_requested.persistent.wait_limit);
  for (std::uint32_t attempt = 1; attempt < attempts; ++attempt) {
    ConditionVariablePrepareToSleep(&g_bootstrap->state_changed);
    if (area_handle_released_once(handle)) {
      static_cast<void>(ConditionVariableCancelSleep());
      return true;
    }
    static_cast<void>(ConditionVariableTimedSleep(
        &g_bootstrap->state_changed,
        static_cast<long>(g_requested.persistent.wait_timeout_ms),
        PG_WAIT_EXTENSION));
    static_cast<void>(ConditionVariableCancelSleep());
    CHECK_FOR_INTERRUPTS();
    if (area_handle_released_once(handle)) {
      return true;
    }
  }
  return false;
}

/** Remove this initializer's area pin, detach its mapping, and probe release. */
[[nodiscard]] UnpublishedAreaCleanupV1 cleanup_local_unpublished_area(
    dsa_area *area, bool pinned) noexcept {
  UnpublishedAreaCleanupV1 result{
      .handle = area == nullptr ? DSA_HANDLE_INVALID : dsa_get_handle(area),
      .was_pinned = pinned,
  };
  const bool unpinned = !pinned || try_unpin_area(area);
  detach_area(area);
  result.pin_removed = unpinned;
  result.released = unpinned && area_handle_released_once(result.handle);
  record_test_cleanup(result);
  return result;
}

void shared_registry_backend_exit(int, Datum) noexcept {
  dsa_area *area = g_initializing_area != nullptr
                       ? g_initializing_area
                       : g_attachment.area;
  const SharedRegistryOwnerTokenV1 owner = g_initializing_owner;
  const bool unpublished = g_initializing_area != nullptr;
  const bool pinned = g_initializing_area_pinned;
  const UnpublishedAreaCleanupV1 cleanup = unpublished
      ? cleanup_local_unpublished_area(area, pinned)
      : UnpublishedAreaCleanupV1{};
  if (!unpublished) {
    detach_area(area);
  }
  clear_process_attachment();

  if (!unpublished || g_bootstrap == nullptr ||
      g_bootstrap_lock == nullptr || !owner.valid()) {
    return;
  }
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  if (load_state() == BootstrapState::Initializing &&
      g_bootstrap->initializer == owner) {
    g_bootstrap->cleanup_handle = cleanup.released
        ? DSA_HANDLE_INVALID
        : cleanup.handle;
    g_bootstrap->cleanup_handle_pinned =
        cleanup.was_pinned && !cleanup.pin_removed ? 1U : 0U;
    if (cleanup.released) {
      if (g_bootstrap->live_unpublished_area_count != 0) {
        --g_bootstrap->live_unpublished_area_count;
      }
      saturating_increment(g_bootstrap->cleanup_count);
    }
  }
  LWLockRelease(g_bootstrap_lock);
  ConditionVariableBroadcast(&g_bootstrap->state_changed);
}

void ensure_exit_callback_registered() noexcept {
  if (!g_exit_callback_registered) {
    before_shmem_exit(shared_registry_backend_exit, Datum{0});
    g_exit_callback_registered = true;
  }
}

void record_initializer_failure(
    const SharedRegistryOwnerTokenV1 &owner,
    SharedRegistryErrorV1 error) noexcept {
  dsa_area *area = g_initializing_area;
  const UnpublishedAreaCleanupV1 cleanup = cleanup_local_unpublished_area(
      area, area != nullptr && g_initializing_area_pinned);
  clear_process_attachment();

  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  if (load_state() == BootstrapState::Initializing &&
      g_bootstrap->initializer == owner) {
    g_bootstrap->cleanup_handle = cleanup.released
        ? DSA_HANDLE_INVALID
        : cleanup.handle;
    g_bootstrap->cleanup_handle_pinned =
        cleanup.was_pinned && !cleanup.pin_removed ? 1U : 0U;
    if (cleanup.released) {
      if (g_bootstrap->live_unpublished_area_count != 0) {
        --g_bootstrap->live_unpublished_area_count;
      }
      saturating_increment(g_bootstrap->cleanup_count);
    }
    saturating_increment(g_bootstrap->validation_failure_count);
    g_bootstrap->initializer = {};
    store_failure(cleanup.released
                      ? error
                      : SharedRegistryErrorV1::RecoveryCleanupFailed);
    store_state(BootstrapState::Failed);
  }
  LWLockRelease(g_bootstrap_lock);
  ConditionVariableBroadcast(&g_bootstrap->state_changed);
}

[[nodiscard]] bool recover_unpublished_area(
    dsa_handle handle, bool pinned) {
  if (handle == DSA_HANDLE_INVALID) {
    return true;
  }
  dsa_area *area = nullptr;
  if (!try_attach_area(handle, &area)) {
    record_test_cleanup(UnpublishedAreaCleanupV1{
        .handle = handle,
        .was_pinned = pinned,
        .released = true,
    });
    return true;
  }
  const bool unpinned = !pinned || try_unpin_area(area);
  detach_area(area);
  const bool released = unpinned && wait_for_area_handle_release(handle);
  record_test_cleanup(UnpublishedAreaCleanupV1{
      .handle = handle,
      .was_pinned = pinned,
      .released = released,
  });
  return released;
}

[[nodiscard]] bool finish_takeover_cleanup(
    const SharedRegistryOwnerTokenV1 &owner, dsa_handle old_handle,
    bool old_handle_pinned, bool cleanup_required) {
  const bool cleaned = recover_unpublished_area(
      old_handle, old_handle_pinned);
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  const bool still_owner = load_state() == BootstrapState::Initializing &&
                           g_bootstrap->initializer == owner;
  if (still_owner && cleanup_required) {
    if (g_bootstrap->live_unpublished_area_count != 0) {
      --g_bootstrap->live_unpublished_area_count;
    }
    saturating_increment(g_bootstrap->cleanup_count);
  }
  if (still_owner && !cleaned) {
    g_bootstrap->initializer = {};
    store_failure(SharedRegistryErrorV1::RecoveryCleanupFailed);
    store_state(BootstrapState::Failed);
  }
  LWLockRelease(g_bootstrap_lock);
  if (!cleaned) {
    ConditionVariableBroadcast(&g_bootstrap->state_changed);
  }
  return cleaned && still_owner;
}

[[nodiscard]] SharedRegistryAttachResultV1 initialize_claimed_area(
    const SharedRegistryPreloadRequestV1 &request,
    const SharedRegistryCallbacksV1 &callbacks,
    const SharedRegistryOwnerTokenV1 &owner, bool takeover) {
  SharedRegistryAttachResultV1 result{};
  result.state = BootstrapState::Initializing;
  result.initialized_here = true;
  result.takeover = takeover;
  ensure_exit_callback_registered();

  dsa_area *area = nullptr;
  if (!try_create_area(g_bootstrap->dsa_tranche_id, &area)) {
    result.error = SharedRegistryErrorV1::DsaCreateFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  g_initializing_area = area;
  g_initializing_owner = owner;
  const dsa_handle handle = dsa_get_handle(area);
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  const bool still_owner = load_state() == BootstrapState::Initializing &&
                           g_bootstrap->initializer == owner;
  if (still_owner) {
    g_bootstrap->cleanup_handle = handle;
    g_bootstrap->cleanup_handle_pinned = 0;
    if (g_bootstrap->live_unpublished_area_count !=
        (std::numeric_limits<std::uint32_t>::max)()) {
      ++g_bootstrap->live_unpublished_area_count;
    }
  }
  LWLockRelease(g_bootstrap_lock);
  if (!still_owner) {
    result.error = SharedRegistryErrorV1::PublicationLost;
    record_initializer_failure(owner, result.error);
    return result;
  }

  if (!try_set_size_limit(area, request.persistent.dsa_size_limit_bytes)) {
    result.error = SharedRegistryErrorV1::DsaSizeLimitFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  if (!try_pin_area(area)) {
    result.error = SharedRegistryErrorV1::DsaPinFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  g_initializing_area_pinned = true;
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  if (load_state() == BootstrapState::Initializing &&
      g_bootstrap->initializer == owner) {
    g_bootstrap->cleanup_handle_pinned = 1;
  }
  LWLockRelease(g_bootstrap_lock);
  if (!try_pin_mapping(area)) {
    result.error = SharedRegistryErrorV1::DsaMappingPinFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  g_initializing_mapping_pinned = true;

  const dsa_pointer root_pointer = allocate_zeroed(
      area, sizeof(SharedRegistryRootV1), 1);
  if (!DsaPointerIsValid(root_pointer)) {
    result.error = inject_allocation_failure(1)
                       ? SharedRegistryErrorV1::TestInjectedAllocationFailure
                       : SharedRegistryErrorV1::RootAllocationFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  const dsa_pointer reusable_pointer = allocate_zeroed(
      area, request.persistent.reusable_bytes, 2);
  if (!DsaPointerIsValid(reusable_pointer)) {
    result.error = inject_allocation_failure(2)
                       ? SharedRegistryErrorV1::TestInjectedAllocationFailure
                       : SharedRegistryErrorV1::ReusableAllocationFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  const dsa_pointer dependency_pointer = allocate_zeroed(
      area, request.persistent.dependency_bytes, 3);
  if (!DsaPointerIsValid(dependency_pointer)) {
    result.error = inject_allocation_failure(3)
                       ? SharedRegistryErrorV1::TestInjectedAllocationFailure
                       : SharedRegistryErrorV1::DependencyAllocationFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  const dsa_pointer wait_cvs_pointer = allocate_zeroed(
      area, request.persistent.reusable_wait_cv_bytes, 4);
  if (!DsaPointerIsValid(wait_cvs_pointer)) {
    result.error = inject_allocation_failure(4)
                       ? SharedRegistryErrorV1::TestInjectedAllocationFailure
                       : SharedRegistryErrorV1::ReusableAllocationFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  dsa_pointer extension_pointer = InvalidDsaPointer;
  if (request.persistent.extension_metadata_bytes != 0) {
    extension_pointer = allocate_zeroed(
        area, request.persistent.extension_metadata_bytes, 5);
    if (!DsaPointerIsValid(extension_pointer)) {
      result.error = inject_allocation_failure(5)
                         ? SharedRegistryErrorV1::TestInjectedAllocationFailure
                         : SharedRegistryErrorV1::ExtensionAllocationFailed;
      record_initializer_failure(owner, result.error);
      return result;
    }
  }

  auto *root = static_cast<SharedRegistryRootV1 *>(
      dsa_get_address(area, root_pointer));
  *root = SharedRegistryRootV1{
      .magic = kSharedRegistryRootMagicV1,
      .abi_version = kSharedRegistryRootAbiVersionV1,
      .root_bytes = sizeof(SharedRegistryRootV1),
      .boot_nonce = load_boot_nonce(),
      .layout = request.persistent,
      .reusable_registry = reusable_pointer,
      .reusable_wait_cvs = wait_cvs_pointer,
      .dependency_directory = dependency_pointer,
      .extension_metadata = extension_pointer,
  };
  pg_atomic_init_u64(&root->hot_live_bytes, 0);
  pg_atomic_init_u64(&root->hot_allocation_failures, 0);

  auto *reusable_bytes = static_cast<std::byte *>(
      dsa_get_address(area, reusable_pointer));
  auto reusable = startup_reusable_registry(
      std::span<std::byte>{
          reusable_bytes, request.persistent.reusable_bytes},
      load_boot_nonce(), request.reusable.config, false);
  if (!reusable.has_value() ||
      validate_reusable_registry(*reusable) !=
          ReusableRegistryErrorV1::None) {
    result.error = SharedRegistryErrorV1::ReusableInitializationFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }
  auto *wait_cvs = static_cast<ConditionVariable *>(
      dsa_get_address(area, wait_cvs_pointer));
  for (std::uint32_t index = 0;
       index < request.persistent.reusable_wait_cv_capacity; ++index) {
    ConditionVariableInit(&wait_cvs[index]);
  }

  auto *dependency_bytes = static_cast<std::byte *>(
      dsa_get_address(area, dependency_pointer));
  const std::span<std::byte> dependency_region{
      dependency_bytes, request.persistent.dependency_bytes};
  if (initialize_dependency_directory(
          dependency_region, request.dependency) !=
      DependencyDirectoryErrorV1::None) {
    result.error = SharedRegistryErrorV1::DependencyInitializationFailed;
    record_initializer_failure(owner, result.error);
    return result;
  }

  if (request.persistent.extension_metadata_bytes != 0) {
    auto *extension_bytes = static_cast<std::byte *>(
        dsa_get_address(area, extension_pointer));
    const std::span<std::byte> extension{
        extension_bytes, request.persistent.extension_metadata_bytes};
    if (callbacks.initialize_extension == nullptr ||
        !callbacks.initialize_extension(
            callbacks.context, extension, load_boot_nonce())) {
      result.error = SharedRegistryErrorV1::ExtensionInitializationFailed;
      record_initializer_failure(owner, result.error);
      return result;
    }
  }

  if (inject_root_magic_mismatch()) {
    ++root->magic;
  }
  if (inject_root_abi_mismatch()) {
    ++root->abi_version;
  }
  SharedRegistryAttachResultV1 validated = validate_area(
      area, handle, root_pointer, request, callbacks,
      load_boot_nonce(), true);
  if (!validated) {
    result.error = inject_root_magic_mismatch()
                       ? SharedRegistryErrorV1::TestInjectedMagicMismatch
                       : inject_root_abi_mismatch()
                             ? SharedRegistryErrorV1::TestInjectedAbiMismatch
                             : validated.error;
    record_initializer_failure(owner, result.error);
    return result;
  }

  pause_before_publication();

  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  const bool publishable = load_state() == BootstrapState::Initializing &&
                           g_bootstrap->initializer == owner;
  if (publishable) {
    g_bootstrap->published_handle = handle;
    g_bootstrap->published_root = root_pointer;
    g_bootstrap->published_reusable = reusable_pointer;
    g_bootstrap->published_reusable_wait_cvs = wait_cvs_pointer;
    g_bootstrap->published_dependency = dependency_pointer;
    g_bootstrap->published_extension = extension_pointer;
    g_bootstrap->cleanup_handle = DSA_HANDLE_INVALID;
    g_bootstrap->cleanup_handle_pinned = 0;
    if (g_bootstrap->live_unpublished_area_count != 0) {
      --g_bootstrap->live_unpublished_area_count;
    }
    g_bootstrap->last_initializer = owner;
    g_bootstrap->initializer = {};
    store_failure(SharedRegistryErrorV1::None);
    store_state(BootstrapState::Ready);
  }
  LWLockRelease(g_bootstrap_lock);
  ConditionVariableBroadcast(&g_bootstrap->state_changed);
  if (!publishable) {
    result.error = SharedRegistryErrorV1::PublicationLost;
    record_initializer_failure(owner, result.error);
    return result;
  }

  g_attachment = validated.attachment;
  g_initializing_area = nullptr;
  g_initializing_owner = {};
  g_initializing_area_pinned = false;
  g_initializing_mapping_pinned = false;
  validated.initialized_here = true;
  validated.takeover = takeover;
  validated.state = BootstrapState::Ready;
  return validated;
}

/** A READY root that fails immutable-manifest validation is boot-fatal. */
void fail_published_validation(
    dsa_handle handle, dsa_pointer root_pointer,
    SharedRegistryErrorV1 error) noexcept {
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  if (load_state() == BootstrapState::Ready &&
      g_bootstrap->published_handle == handle &&
      g_bootstrap->published_root == root_pointer) {
    saturating_increment(g_bootstrap->validation_failure_count);
    store_failure(error == SharedRegistryErrorV1::None
                      ? SharedRegistryErrorV1::RootValidationFailed
                      : error);
    store_state(BootstrapState::Failed);
  }
  LWLockRelease(g_bootstrap_lock);
  ConditionVariableBroadcast(&g_bootstrap->state_changed);
}

[[nodiscard]] SharedRegistryAttachResultV1 attach_published_area(
    const SharedRegistryPreloadRequestV1 &request,
    const SharedRegistryCallbacksV1 &callbacks) noexcept {
  SharedRegistryAttachResultV1 result{};
  LWLockAcquire(g_bootstrap_lock, LW_SHARED);
  if (load_state() != BootstrapState::Ready) {
    result.error = SharedRegistryErrorV1::NotReady;
    result.state = load_state();
    LWLockRelease(g_bootstrap_lock);
    return result;
  }
  pg_read_barrier();
  const dsa_handle handle = g_bootstrap->published_handle;
  const dsa_pointer root_pointer = g_bootstrap->published_root;
  const dsa_pointer reusable_pointer = g_bootstrap->published_reusable;
  const dsa_pointer wait_cvs_pointer =
      g_bootstrap->published_reusable_wait_cvs;
  const dsa_pointer dependency_pointer =
      g_bootstrap->published_dependency;
  const dsa_pointer extension_pointer = g_bootstrap->published_extension;
  const std::uint32_t exact_capacity =
      g_bootstrap->exact_directory_capacity;
  const std::uint32_t coverage_capacity =
      g_bootstrap->coverage_directory_capacity;
  const std::uint32_t dependency_capacity =
      g_bootstrap->dependency_directory_capacity;
  const std::uint32_t partition_count =
      g_bootstrap->directory_partition_count;
  const std::uint64_t boot_nonce = load_boot_nonce();
  LWLockRelease(g_bootstrap_lock);

  dsa_area *area = nullptr;
  if (!try_attach_area(handle, &area)) {
    result.error = SharedRegistryErrorV1::DsaAttachFailed;
    result.state = BootstrapState::Ready;
    return result;
  }
  if (!try_pin_mapping(area)) {
    detach_area(area);
    result.error = SharedRegistryErrorV1::DsaMappingPinFailed;
    result.state = BootstrapState::Ready;
    return result;
  }
  result = validate_area(
      area, handle, root_pointer, request, callbacks, boot_nonce, false);
  if (result &&
      (result.attachment.root->reusable_registry != reusable_pointer ||
       result.attachment.root->reusable_wait_cvs != wait_cvs_pointer ||
       result.attachment.root->dependency_directory != dependency_pointer ||
       result.attachment.root->extension_metadata != extension_pointer ||
       exact_capacity != request.persistent.reusable_exact_capacity ||
       coverage_capacity != request.persistent.reusable_coverage_capacity ||
       dependency_capacity != request.persistent.dependency_physical_capacity ||
       partition_count != request.persistent.dependency_partition_count)) {
    result.attachment = {};
    result.error = SharedRegistryErrorV1::RootValidationFailed;
    result.state = BootstrapState::Ready;
  }
  if (!result) {
    detach_area(area);
    fail_published_validation(handle, root_pointer, result.error);
    return result;
  }
  ensure_exit_callback_registered();
  g_attachment = result.attachment;
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  saturating_increment(g_bootstrap->attach_count);
  LWLockRelease(g_bootstrap_lock);
  return result;
}

void timed_initialization_wait() {
  ConditionVariablePrepareToSleep(&g_bootstrap->state_changed);
  LWLockAcquire(g_bootstrap_lock, LW_SHARED);
  const bool still_initializing =
      load_state() == BootstrapState::Initializing;
  LWLockRelease(g_bootstrap_lock);
  if (still_initializing) {
    static_cast<void>(ConditionVariableTimedSleep(
        &g_bootstrap->state_changed,
        static_cast<long>(g_requested.persistent.wait_timeout_ms),
        PG_WAIT_EXTENSION));
  }
  static_cast<void>(ConditionVariableCancelSleep());
  CHECK_FOR_INTERRUPTS();
}

}  // namespace shared_registry_detail

/** Normalize every exact core layout before shared memory is requested. */
[[nodiscard]] inline SharedRegistryPreloadRequestV1
shared_registry_preload_request(
    SharedRegistryConfigV1 config = {}) noexcept {
  SharedRegistryPreloadRequestV1 result{};
  if (config.max_backends == 0 ||
      config.max_backends > kMaximumTransitionOwnersV1 ||
      config.dependency_partition_count == 0 ||
      config.dependency_partition_count >
          kMaximumDependencyDirectoryPartitionsV1 ||
      config.dependency_reclaim_scan_limit == 0 ||
      config.dependency_reclaim_scan_limit >
          kMaximumDependencyVectorSizeV1 ||
      config.transition_precise_reference_capacity == 0 ||
      config.transition_precise_reference_capacity >
          kMaximumTransitionReferencesV1 ||
      config.test_control_count >
          kMaximumSharedRegistryTestControlsV1 ||
      config.wait_timeout_ms == 0 || config.wait_limit == 0 ||
      config.wait_limit > kMaximumSharedRegistryWaitLimitV1 ||
      config.dsa_size_limit_bytes == 0 || config.reserved != 0) {
    result.error = SharedRegistryErrorV1::InvalidConfiguration;
    return result;
  }
#ifndef BIOSEQ_CACHE_TESTING
  if (config.test_control_count != 0) {
    result.error = SharedRegistryErrorV1::InvalidConfiguration;
    return result;
  }
#endif

  const std::uint64_t generation_capacity = std::min<std::uint64_t>(
      kMaximumReusableRegistryGenerationsV1,
      std::max<std::uint64_t>(
          kDefaultReusableRegistryGenerationsV1,
          static_cast<std::uint64_t>(config.max_backends) * 64U));
  config.reusable.max_generations =
      static_cast<std::uint32_t>(generation_capacity);
  result.requested_config = config;

  const auto reusable = reusable_registry_preload_request(config.reusable);
  if (!reusable.has_value()) {
    result.error = SharedRegistryErrorV1::InvalidConfiguration;
    return result;
  }
  result.reusable = *reusable;

  DependencyDirectoryConfigV1 dependency_config{
      .logical_capacity =
          dependency_directory_logical_capacity_for_backends(
              config.max_backends),
      .partition_count = config.dependency_partition_count,
      .reclaim_scan_limit = config.dependency_reclaim_scan_limit,
      .pressure_percent = kDependencyDirectoryPressurePercentV1,
      .backend_reference_limit = config.max_backends,
      .generation_reference_limit = reusable->config.max_generations,
  };
  const DependencyDirectoryPreloadResultV1 dependency =
      dependency_directory_preload_request(dependency_config);
  if (!dependency) {
    result.error = SharedRegistryErrorV1::InvalidConfiguration;
    return result;
  }
  result.dependency = dependency.request;

  result.transition = transition_ledger_preload_request(
      TransitionLedgerConfigV1{
          .owner_capacity = config.max_backends,
          .precise_reference_capacity =
              config.transition_precise_reference_capacity,
          .dependency_partition_count =
              config.dependency_partition_count,
      });
  if (!result.transition) {
    result.error = SharedRegistryErrorV1::InvalidConfiguration;
    return result;
  }

  result.persistent = SharedRegistryPersistentLayoutV1{
      .root_bytes = sizeof(SharedRegistryRootV1),
      .reusable_bytes = reusable->layout.total_bytes,
      .reusable_max_generations = reusable->config.max_generations,
      .reusable_exact_capacity =
          reusable->config.exact_directory_capacity,
      .reusable_coverage_capacity =
          reusable->config.coverage_directory_capacity,
      .reusable_partition_count = reusable->config.partition_count,
      .reusable_wait_cv_bytes = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(reusable->config.max_generations) *
          sizeof(ConditionVariable)),
      .reusable_wait_cv_capacity = reusable->config.max_generations,
      .dependency_bytes = dependency.request.layout.total_bytes,
      .dependency_logical_capacity =
          dependency.request.layout.logical_capacity,
      .dependency_physical_capacity =
          dependency.request.layout.physical_capacity,
      .dependency_partition_count =
          dependency.request.config.partition_count,
      .transition_ledger_bytes = result.transition.layout.total_bytes,
      .transition_owner_capacity = result.transition.config.owner_capacity,
      .transition_references_per_owner =
          result.transition.layout.references_per_owner,
      .extension_metadata_bytes = config.extension_metadata_bytes,
      .test_control_count = config.test_control_count,
      .wait_timeout_ms = config.wait_timeout_ms,
      .wait_limit = config.wait_limit,
      .dsa_size_limit_bytes = config.dsa_size_limit_bytes,
  };
  const std::uint64_t fixed_bytes =
      static_cast<std::uint64_t>(result.persistent.root_bytes) +
      result.persistent.reusable_bytes +
      result.persistent.reusable_wait_cv_bytes +
      result.persistent.dependency_bytes +
      result.persistent.extension_metadata_bytes;
  if (fixed_bytes > result.persistent.dsa_size_limit_bytes) {
    result.error = SharedRegistryErrorV1::InvalidConfiguration;
  }
  return result;
}

/** Request only the bounded bootstrap/sidecars and their named locks. */
inline void request_shared_registry_resources(
    const SharedRegistryPreloadRequestV1 &request) {
  using namespace shared_registry_detail;
  g_requested = request;
  if (!request) {
    ereport(ERROR,
            (errmsg("bioseq_cache shared registry configuration is invalid")));
  }
  RequestAddinShmemSpace(
      MAXALIGN(sizeof(SharedRegistryBootstrapHeaderV1)));
  RequestAddinShmemSpace(
      MAXALIGN(request.transition.layout.total_bytes));
  if (request.persistent.test_control_count != 0) {
    RequestAddinShmemSpace(MAXALIGN(sizeof(SharedRegistryTestSidecarV1)));
  }
  RequestNamedLWLockTranche(kBootstrapLockTranche, 1);
  RequestNamedLWLockTranche(
      kTransitionLockTranche, request.transition.owner_lock_count);
  RequestNamedLWLockTranche(
      kDependencyLockTranche,
      request.dependency.partition_lock_count);
  RequestNamedLWLockTranche(
      kReusableMembershipLockTranche,
      request.reusable.membership_lock_count);
  RequestNamedLWLockTranche(
      kReusableExactLockTranche,
      request.reusable.exact_partition_lock_count);
  RequestNamedLWLockTranche(
      kReusableCoverageLockTranche,
      request.reusable.coverage_partition_lock_count);
}

/** Initialize bounded postmaster shmem; never create or attach the DSA here. */
[[nodiscard]] inline bool startup_shared_registry(
    const SharedRegistryPreloadRequestV1 &request,
    std::uint64_t boot_nonce) noexcept {
  using namespace shared_registry_detail;
  if (!request || boot_nonce == 0 ||
      (!g_requested || !request_matches(request, g_requested))) {
    return false;
  }
  bool found = false;
  g_bootstrap = static_cast<SharedRegistryBootstrapHeaderV1 *>(
      ShmemInitStruct(
          kBootstrapShmemName,
          sizeof(SharedRegistryBootstrapHeaderV1), &found));
  g_bootstrap_lock =
      &GetNamedLWLockTranche(kBootstrapLockTranche)[0].lock;
  g_transition_locks = GetNamedLWLockTranche(kTransitionLockTranche);
  g_dependency_locks = GetNamedLWLockTranche(kDependencyLockTranche);
  g_reusable_membership_lock =
      GetNamedLWLockTranche(kReusableMembershipLockTranche);
  g_reusable_exact_locks =
      GetNamedLWLockTranche(kReusableExactLockTranche);
  g_reusable_coverage_locks =
      GetNamedLWLockTranche(kReusableCoverageLockTranche);
  if (g_bootstrap == nullptr || g_bootstrap_lock == nullptr ||
      g_transition_locks == nullptr || g_dependency_locks == nullptr ||
      g_reusable_membership_lock == nullptr ||
      g_reusable_exact_locks == nullptr ||
      g_reusable_coverage_locks == nullptr) {
    return false;
  }
  if (!found) {
    std::memset(g_bootstrap, 0, sizeof(*g_bootstrap));
    g_bootstrap->magic = kSharedRegistryBootstrapMagicV1;
    g_bootstrap->abi_version = kSharedRegistryBootstrapAbiVersionV1;
    g_bootstrap->header_bytes = sizeof(*g_bootstrap);
    pg_atomic_init_u64(&g_bootstrap->boot_nonce, boot_nonce);
    g_bootstrap->layout = request.persistent;
    g_bootstrap->dsa_tranche_id =
        static_cast<std::uint32_t>(LWLockNewTrancheId());
    g_bootstrap->exact_directory_capacity =
        request.persistent.reusable_exact_capacity;
    g_bootstrap->coverage_directory_capacity =
        request.persistent.reusable_coverage_capacity;
    g_bootstrap->dependency_directory_capacity =
        request.persistent.dependency_physical_capacity;
    g_bootstrap->directory_partition_count =
        request.persistent.dependency_partition_count;
    pg_atomic_init_u64(&g_bootstrap->next_backend_nonce, 1);
    pg_atomic_init_u64(&g_bootstrap->next_claim_sequence, 1);
    pg_atomic_init_u64(&g_bootstrap->emergency_write_epoch, 0);
    pg_atomic_init_u64(&g_bootstrap->emergency_change_generation, 1);
    pg_atomic_init_u32(&g_bootstrap->emergency_transition_count, 0);
    ConditionVariableInit(&g_bootstrap->emergency_state_changed);
    pg_atomic_init_u64(&g_bootstrap->reserved_dsa_bytes, 0);
    pg_atomic_init_u64(&g_bootstrap->reserved_file_bytes, 0);
    pg_atomic_init_u64(&g_bootstrap->complete_entries, 0);
    pg_atomic_init_u64(&g_bootstrap->inflight_entries, 0);
    pg_atomic_init_u64(&g_bootstrap->indexed_generations, 0);
    pg_atomic_init_u64(&g_bootstrap->retired_generations, 0);
    pg_atomic_init_u64(&g_bootstrap->evictions, 0);
    pg_atomic_init_u64(&g_bootstrap->invalidations, 0);
    g_bootstrap->cleanup_handle = DSA_HANDLE_INVALID;
    g_bootstrap->published_handle = DSA_HANDLE_INVALID;
    g_bootstrap->published_root = InvalidDsaPointer;
    g_bootstrap->published_reusable = InvalidDsaPointer;
    g_bootstrap->published_reusable_wait_cvs = InvalidDsaPointer;
    g_bootstrap->published_dependency = InvalidDsaPointer;
    g_bootstrap->published_extension = InvalidDsaPointer;
    g_bootstrap->published_retired_generation_head = InvalidDsaPointer;
    ConditionVariableInit(&g_bootstrap->state_changed);
    pg_atomic_init_u32(
        &g_bootstrap->state,
        static_cast<std::uint32_t>(BootstrapState::Uninitialized));
    pg_atomic_init_u32(
        &g_bootstrap->failure_code,
        static_cast<std::uint32_t>(SharedRegistryErrorV1::None));
  } else if (!bootstrap_matches(request, boot_nonce)) {
    return false;
  }
  LWLockRegisterTranche(
      static_cast<int>(g_bootstrap->dsa_tranche_id), kDsaTrancheName);

  bool transition_found = false;
  auto *transition_bytes = static_cast<std::byte *>(ShmemInitStruct(
      kTransitionShmemName, request.transition.layout.total_bytes,
      &transition_found));
  if (transition_bytes == nullptr) {
    store_failure(SharedRegistryErrorV1::TransitionLedgerUnavailable);
    store_state(BootstrapState::Failed);
    return false;
  }
  const std::span<std::byte> transition_region{
      transition_bytes, request.transition.layout.total_bytes};
  if (!transition_found &&
      initialize_transition_ledger(
          transition_region, request.transition) !=
          TransitionLedgerErrorV1::None) {
    store_failure(SharedRegistryErrorV1::TransitionLedgerUnavailable);
    store_state(BootstrapState::Failed);
    return false;
  }
  const TransitionLedgerAttachResultV1 transition =
      attach_transition_ledger(transition_region);
  if (!transition) {
    store_failure(SharedRegistryErrorV1::TransitionLedgerUnavailable);
    store_state(BootstrapState::Failed);
    return false;
  }
  g_transition_ledger = transition.ledger;

  if (request.persistent.test_control_count != 0) {
    bool test_found = false;
    g_test_sidecar = static_cast<SharedRegistryTestSidecarV1 *>(
        ShmemInitStruct(
            kTestShmemName, sizeof(SharedRegistryTestSidecarV1),
            &test_found));
    if (g_test_sidecar == nullptr) {
      store_failure(SharedRegistryErrorV1::TestSidecarUnavailable);
      store_state(BootstrapState::Failed);
      return false;
    }
    if (!test_found) {
      std::memset(g_test_sidecar, 0, sizeof(*g_test_sidecar));
      g_test_sidecar->magic = kSharedRegistryTestMagicV1;
      g_test_sidecar->abi_version = kSharedRegistryTestAbiVersionV1;
      g_test_sidecar->control_count =
          request.persistent.test_control_count;
      g_test_sidecar->boot_nonce = boot_nonce;
      for (SharedRegistryTestControlV1 &control :
           g_test_sidecar->controls) {
        pg_atomic_init_u32(&control.pause_point, 0);
        pg_atomic_init_u32(&control.reached, 0);
        pg_atomic_init_u32(&control.released, 0);
        pg_atomic_init_u32(&control.defer_background_initializer, 0);
        pg_atomic_init_u32(&control.allocation_failure_ordinal, 0);
        pg_atomic_init_u32(&control.inject_root_abi_mismatch, 0);
        pg_atomic_init_u32(&control.inject_root_magic_mismatch, 0);
        pg_atomic_init_u32(&control.allocation_counter, 0);
        pg_atomic_init_u32(&control.last_cleanup_handle, 0);
        pg_atomic_init_u32(
            &control.last_cleanup_handle_was_pinned, 0);
        pg_atomic_init_u32(
            &control.last_cleanup_release_verified, 0);
        ConditionVariableInit(&control.changed);
        configure_test_control_from_environment(control);
      }
    } else if (g_test_sidecar->magic != kSharedRegistryTestMagicV1 ||
               g_test_sidecar->abi_version !=
                   kSharedRegistryTestAbiVersionV1 ||
               g_test_sidecar->control_count !=
                   request.persistent.test_control_count ||
               g_test_sidecar->boot_nonce != boot_nonce) {
      store_failure(SharedRegistryErrorV1::TestSidecarUnavailable);
      store_state(BootstrapState::Failed);
      return false;
    }
  }
  return load_state() != BootstrapState::Failed;
}

/** Register this first so later storage/transition cleanup runs before detach. */
inline void register_shared_registry_exit_callback() noexcept {
  shared_registry_detail::ensure_exit_callback_registered();
}

/** Lazy first initializer, timed wait, or one dead-owner takeover. */
[[nodiscard]] inline SharedRegistryAttachResultV1 attach_shared_registry(
    const SharedRegistryPreloadRequestV1 &request,
    const SharedRegistryCallbacksV1 &callbacks = {},
    bool allow_initialize = true) {
  using namespace shared_registry_detail;
  SharedRegistryAttachResultV1 result{};
  if (!request || g_bootstrap == nullptr || g_bootstrap_lock == nullptr ||
      !g_requested || !request_matches(request, g_requested)) {
    result.error = SharedRegistryErrorV1::NotPreloaded;
    result.state = BootstrapState::Failed;
    return result;
  }
  if (!extension_callbacks_valid(request, callbacks)) {
    result.error = SharedRegistryErrorV1::InvalidArgument;
    result.state = load_state();
    return result;
  }
  if (g_attachment) {
    result.attachment = g_attachment;
    result.state = BootstrapState::Ready;
    return result;
  }
  const bool initialize_allowed =
      allow_initialize && !defer_background_initializer();

  for (std::uint32_t wait_index = 0;
       wait_index < request.persistent.wait_limit; ++wait_index) {
    const BootstrapState observed_state = load_state();
    if (observed_state == BootstrapState::Ready) {
      return attach_published_area(request, callbacks);
    }
    if (observed_state == BootstrapState::Failed) {
      result.error = load_failure() == SharedRegistryErrorV1::None
                         ? SharedRegistryErrorV1::BootstrapFailed
                         : load_failure();
      result.state = BootstrapState::Failed;
      return result;
    }
    if (observed_state == BootstrapState::Uninitialized) {
      if (!initialize_allowed) {
        result.error = SharedRegistryErrorV1::NotReady;
        result.state = observed_state;
        return result;
      }
      const SharedRegistryBackendIdentityV1 identity =
          current_owner(callbacks);
      if (!identity.valid()) {
        result.error = SharedRegistryErrorV1::InitializerUnavailable;
        result.state = observed_state;
        return result;
      }
      LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
      if (load_state() == BootstrapState::Uninitialized) {
        const SharedRegistryOwnerTokenV1 owner =
            claim_owner_locked(identity);
        LWLockRelease(g_bootstrap_lock);
        ConditionVariableBroadcast(&g_bootstrap->state_changed);
        return initialize_claimed_area(
            request, callbacks, owner, false);
      }
      LWLockRelease(g_bootstrap_lock);
      continue;
    }

    timed_initialization_wait();
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() != BootstrapState::Initializing) {
      LWLockRelease(g_bootstrap_lock);
      continue;
    }
    const SharedRegistryOwnerTokenV1 observed_owner =
        g_bootstrap->initializer;
    saturating_increment(g_bootstrap->wait_count);
    LWLockRelease(g_bootstrap_lock);

    const SharedRegistryBackendIdentityV1 identity =
        current_owner(callbacks);
    if (identity.valid() && observed_owner.owner == identity) {
      result.error = SharedRegistryErrorV1::InitializerSelfWait;
      result.state = BootstrapState::Initializing;
      return result;
    }

    /* Both full-token liveness probes happen outside every shared lock. */
    const bool first_alive = owner_is_alive(callbacks, observed_owner);
    CHECK_FOR_INTERRUPTS();
    const bool second_alive = owner_is_alive(callbacks, observed_owner);
    if (first_alive || second_alive) {
      continue;
    }
    if (!initialize_allowed || !identity.valid()) {
      result.error = SharedRegistryErrorV1::InitializerUnavailable;
      result.state = BootstrapState::Initializing;
      return result;
    }

    dsa_handle old_handle = DSA_HANDLE_INVALID;
    bool old_handle_pinned = false;
    bool cleanup_required = false;
    SharedRegistryOwnerTokenV1 takeover_owner{};
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() == BootstrapState::Initializing &&
        g_bootstrap->initializer == observed_owner) {
      if (g_bootstrap->takeover_used != 0) {
        g_bootstrap->initializer = {};
        store_failure(SharedRegistryErrorV1::TakeoverExhausted);
        store_state(BootstrapState::Failed);
        LWLockRelease(g_bootstrap_lock);
        ConditionVariableBroadcast(&g_bootstrap->state_changed);
        result.error = SharedRegistryErrorV1::TakeoverExhausted;
        result.state = BootstrapState::Failed;
        return result;
      }
      std::uint32_t expected = static_cast<std::uint32_t>(
          BootstrapState::Initializing);
      if (pg_atomic_compare_exchange_u32(
              &g_bootstrap->state, &expected,
              static_cast<std::uint32_t>(
                  BootstrapState::Uninitialized))) {
        g_bootstrap->takeover_used = 1;
        saturating_increment(g_bootstrap->takeover_count);
        old_handle = g_bootstrap->cleanup_handle;
        old_handle_pinned = g_bootstrap->cleanup_handle_pinned != 0;
        cleanup_required = old_handle != DSA_HANDLE_INVALID;
        g_bootstrap->initializer = {};
        g_bootstrap->cleanup_handle = DSA_HANDLE_INVALID;
        g_bootstrap->cleanup_handle_pinned = 0;
        g_bootstrap->published_handle = DSA_HANDLE_INVALID;
        g_bootstrap->published_root = InvalidDsaPointer;
        g_bootstrap->published_reusable = InvalidDsaPointer;
        g_bootstrap->published_reusable_wait_cvs = InvalidDsaPointer;
        g_bootstrap->published_dependency = InvalidDsaPointer;
        g_bootstrap->published_extension = InvalidDsaPointer;
        g_bootstrap->published_retired_generation_head = InvalidDsaPointer;
        takeover_owner = claim_owner_locked(identity);
      }
    }
    LWLockRelease(g_bootstrap_lock);
    if (!takeover_owner.valid()) {
      continue;
    }
    if (!finish_takeover_cleanup(
            takeover_owner, old_handle, old_handle_pinned,
            cleanup_required)) {
      result.error = SharedRegistryErrorV1::RecoveryCleanupFailed;
      result.state = load_state();
      return result;
    }
    return initialize_claimed_area(
        request, callbacks, takeover_owner, true);
  }

  result.error = SharedRegistryErrorV1::WaitTimeout;
  result.state = load_state();
  return result;
}

[[nodiscard]] inline bool shared_registry_ready() noexcept {
  return shared_registry_detail::g_attachment &&
         shared_registry_detail::load_state() == BootstrapState::Ready;
}

[[nodiscard]] inline const SharedRegistryAttachmentV1 *
current_shared_registry_attachment() noexcept {
  return shared_registry_ready() ? &shared_registry_detail::g_attachment
                                 : nullptr;
}

/** Detach only this process mapping; the boot-wide DSA pin remains owned. */
inline void detach_shared_registry_local_mapping() noexcept {
  using namespace shared_registry_detail;
  if (g_initializing_area != nullptr) {
    return;
  }
  dsa_area *area = g_attachment.area;
  g_attachment = {};
  detach_area(area);
}

[[nodiscard]] inline TransitionLedgerAttachmentV1
shared_transition_ledger() noexcept {
  return shared_registry_detail::g_transition_ledger;
}

[[nodiscard]] inline LWLockPadded *shared_transition_owner_locks() noexcept {
  return shared_registry_detail::g_transition_locks;
}

[[nodiscard]] inline LWLockPadded *shared_dependency_partition_locks() noexcept {
  return shared_registry_detail::g_dependency_locks;
}

[[nodiscard]] inline LWLockPadded *shared_reusable_membership_lock() noexcept {
  return shared_registry_detail::g_reusable_membership_lock;
}

[[nodiscard]] inline LWLockPadded *shared_reusable_exact_locks() noexcept {
  return shared_registry_detail::g_reusable_exact_locks;
}

[[nodiscard]] inline LWLockPadded *shared_reusable_coverage_locks() noexcept {
  return shared_registry_detail::g_reusable_coverage_locks;
}

[[nodiscard]] inline SharedRegistryEmergencyAccessV1
shared_emergency_dependency() noexcept {
  using namespace shared_registry_detail;
  if (g_bootstrap == nullptr ||
      g_bootstrap->magic != kSharedRegistryBootstrapMagicV1 ||
      g_bootstrap->abi_version != kSharedRegistryBootstrapAbiVersionV1) {
    return {};
  }
  return SharedRegistryEmergencyAccessV1{
      .write_epoch = &g_bootstrap->emergency_write_epoch,
      .change_generation = &g_bootstrap->emergency_change_generation,
      .transition_count = &g_bootstrap->emergency_transition_count,
      .state_changed = &g_bootstrap->emergency_state_changed,
  };
}

[[nodiscard]] inline SharedRegistryAccountingAccessV1
shared_registry_accounting() noexcept {
  using namespace shared_registry_detail;
  if (g_bootstrap == nullptr ||
      g_bootstrap->magic != kSharedRegistryBootstrapMagicV1 ||
      g_bootstrap->abi_version != kSharedRegistryBootstrapAbiVersionV1) {
    return {};
  }
  return SharedRegistryAccountingAccessV1{
      .reserved_dsa_bytes = &g_bootstrap->reserved_dsa_bytes,
      .reserved_file_bytes = &g_bootstrap->reserved_file_bytes,
      .complete_entries = &g_bootstrap->complete_entries,
      .inflight_entries = &g_bootstrap->inflight_entries,
      .indexed_generations = &g_bootstrap->indexed_generations,
      .retired_generations = &g_bootstrap->retired_generations,
      .evictions = &g_bootstrap->evictions,
      .invalidations = &g_bootstrap->invalidations,
  };
}

[[nodiscard]] inline std::uint64_t
next_shared_registry_backend_nonce() noexcept {
  using namespace shared_registry_detail;
  if (g_bootstrap == nullptr) {
    return 0;
  }
  std::uint64_t nonce =
      pg_atomic_fetch_add_u64(&g_bootstrap->next_backend_nonce, 1);
  if (nonce == 0) {
    nonce = pg_atomic_fetch_add_u64(&g_bootstrap->next_backend_nonce, 1);
  }
  return nonce == 0 ? 1 : nonce;
}

/** Allocate one globally monotonic mutation sequence without wrapping. */
[[nodiscard]] inline std::uint64_t
next_shared_registry_claim_sequence() noexcept {
  using namespace shared_registry_detail;
  if (g_bootstrap == nullptr) {
    return 0;
  }
  std::uint64_t observed =
      pg_atomic_read_u64(&g_bootstrap->next_claim_sequence);
  const std::uint64_t maximum =
      (std::numeric_limits<std::uint64_t>::max)();
  while (observed != 0 && observed != maximum) {
    if (pg_atomic_compare_exchange_u64(
        &g_bootstrap->next_claim_sequence, &observed, observed + 1U)) {
      return observed;
    }
  }
  return 0;
}

[[nodiscard]] inline SharedRegistryBackendIdentityV1
current_backend_identity() noexcept {
  return shared_registry_detail::default_current_owner(nullptr);
}

[[nodiscard]] inline bool backend_identity_alive(
    const SharedRegistryOwnerTokenV1 &owner) noexcept {
  return shared_registry_detail::default_owner_is_alive(nullptr, owner);
}

[[nodiscard]] inline SharedRegistryDiagnosticsV1
shared_registry_diagnostics() noexcept {
  using namespace shared_registry_detail;
  SharedRegistryDiagnosticsV1 result{};
  if (g_bootstrap == nullptr || g_bootstrap_lock == nullptr) {
    result.state = BootstrapState::Failed;
    result.failure = SharedRegistryErrorV1::NotPreloaded;
    return result;
  }
  LWLockAcquire(g_bootstrap_lock, LW_SHARED);
  result.state = load_state();
  result.failure = load_failure();
  result.boot_nonce = load_boot_nonce();
  result.emergency_write_epoch =
      pg_atomic_read_u64(&g_bootstrap->emergency_write_epoch);
  result.emergency_change_generation =
      pg_atomic_read_u64(&g_bootstrap->emergency_change_generation);
  result.emergency_transition_count =
      pg_atomic_read_u32(&g_bootstrap->emergency_transition_count);
  result.reserved_dsa_bytes =
      pg_atomic_read_u64(&g_bootstrap->reserved_dsa_bytes);
  result.reserved_file_bytes =
      pg_atomic_read_u64(&g_bootstrap->reserved_file_bytes);
  result.complete_entries =
      pg_atomic_read_u64(&g_bootstrap->complete_entries);
  result.inflight_entries =
      pg_atomic_read_u64(&g_bootstrap->inflight_entries);
  result.indexed_generations =
      pg_atomic_read_u64(&g_bootstrap->indexed_generations);
  result.retired_generations =
      pg_atomic_read_u64(&g_bootstrap->retired_generations);
  result.evictions = pg_atomic_read_u64(&g_bootstrap->evictions);
  result.invalidations =
      pg_atomic_read_u64(&g_bootstrap->invalidations);
  result.layout = g_bootstrap->layout;
  result.initializer = g_bootstrap->initializer;
  result.last_initializer = g_bootstrap->last_initializer;
  result.initialization_attempt_count =
      g_bootstrap->initialization_attempt_count;
  result.takeover_count = g_bootstrap->takeover_count;
  result.cleanup_count = g_bootstrap->cleanup_count;
  result.wait_count = g_bootstrap->wait_count;
  result.attach_count = g_bootstrap->attach_count;
  result.validation_failure_count =
      g_bootstrap->validation_failure_count;
  result.takeover_used = g_bootstrap->takeover_used;
  result.live_unpublished_area_count =
      g_bootstrap->live_unpublished_area_count;
  result.cleanup_handle_pinned = g_bootstrap->cleanup_handle_pinned;
  result.orderly_unpinned = g_bootstrap->orderly_unpinned;
  result.exact_directory_capacity =
      g_bootstrap->exact_directory_capacity;
  result.coverage_directory_capacity =
      g_bootstrap->coverage_directory_capacity;
  result.dependency_directory_capacity =
      g_bootstrap->dependency_directory_capacity;
  result.directory_partition_count =
      g_bootstrap->directory_partition_count;
  result.cleanup_handle_published =
      g_bootstrap->cleanup_handle != DSA_HANDLE_INVALID;
  result.ready_handle_published =
      g_bootstrap->published_handle != DSA_HANDLE_INVALID;
  result.root_published =
      DsaPointerIsValid(g_bootstrap->published_root);
  LWLockRelease(g_bootstrap_lock);
  return result;
}

/** Called only by the orderly janitor stop path; backend exit never unpins. */
[[nodiscard]] inline bool orderly_unpin_shared_registry() noexcept {
  using namespace shared_registry_detail;
  if (!g_attachment || g_bootstrap == nullptr ||
      g_bootstrap_lock == nullptr) {
    return false;
  }
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  if (load_state() != BootstrapState::Ready ||
      g_bootstrap->orderly_unpinned != 0) {
    LWLockRelease(g_bootstrap_lock);
    return false;
  }
  g_bootstrap->orderly_unpinned = 1;
  LWLockRelease(g_bootstrap_lock);
  if (!try_unpin_area(g_attachment.area)) {
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    g_bootstrap->orderly_unpinned = 0;
    store_failure(SharedRegistryErrorV1::DsaUnpinFailed);
    LWLockRelease(g_bootstrap_lock);
    return false;
  }
  return true;
}

#ifdef BIOSEQ_CACHE_TESTING

[[nodiscard]] inline bool arm_shared_registry_test_pause(
    SharedRegistryTestPointV1 point) noexcept {
  using namespace shared_registry_detail;
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr ||
      point != SharedRegistryTestPointV1::BeforeHandlePublish) {
    return false;
  }
  pg_atomic_write_u32(&control->reached, 0);
  pg_atomic_write_u32(&control->released, 0);
  pg_atomic_write_u32(
      &control->pause_point, static_cast<std::uint32_t>(point));
  return true;
}

inline void release_shared_registry_test_pause() noexcept {
  using namespace shared_registry_detail;
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr) {
    return;
  }
  pg_atomic_write_u32(&control->released, 1);
  pg_atomic_write_u32(
      &control->pause_point,
      static_cast<std::uint32_t>(SharedRegistryTestPointV1::None));
  pg_atomic_write_u32(&control->defer_background_initializer, 0);
  ConditionVariableBroadcast(&control->changed);
}

[[nodiscard]] inline bool inject_shared_registry_allocation_failure(
    std::uint32_t ordinal) noexcept {
  using namespace shared_registry_detail;
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr || ordinal > 5) {
    return false;
  }
  pg_atomic_write_u32(&control->allocation_counter, 0);
  pg_atomic_write_u32(&control->allocation_failure_ordinal, ordinal);
  return true;
}

[[nodiscard]] inline bool inject_shared_registry_root_abi_mismatch(
    bool enabled) noexcept {
  using namespace shared_registry_detail;
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr) {
    return false;
  }
  pg_atomic_write_u32(
      &control->inject_root_abi_mismatch, enabled ? 1U : 0U);
  return true;
}

[[nodiscard]] inline bool inject_shared_registry_root_magic_mismatch(
    bool enabled) noexcept {
  using namespace shared_registry_detail;
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr) {
    return false;
  }
  pg_atomic_write_u32(
      &control->inject_root_magic_mismatch, enabled ? 1U : 0U);
  return true;
}

/** Corrupt one immutable READY-root field for attach rejection proof. */
[[nodiscard]] inline bool corrupt_shared_registry_manifest_for_test(
    SharedRegistryTestManifestCorruptionV1 corruption) noexcept {
  using namespace shared_registry_detail;
  if (!g_attachment || g_attachment.root == nullptr ||
      g_bootstrap == nullptr || g_bootstrap_lock == nullptr) {
    return false;
  }
  LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
  const bool current = load_state() == BootstrapState::Ready &&
                       g_bootstrap->published_handle ==
                           g_attachment.handle &&
                       dsa_get_address(
                           g_attachment.area,
                           g_bootstrap->published_root) ==
                           g_attachment.root;
  if (current) {
    switch (corruption) {
      case SharedRegistryTestManifestCorruptionV1::Magic:
        ++g_attachment.root->magic;
        break;
      case SharedRegistryTestManifestCorruptionV1::AbiVersion:
        ++g_attachment.root->abi_version;
        break;
      case SharedRegistryTestManifestCorruptionV1::BootNonce:
        ++g_attachment.root->boot_nonce;
        break;
      default:
        LWLockRelease(g_bootstrap_lock);
        return false;
    }
    pg_write_barrier();
  }
  LWLockRelease(g_bootstrap_lock);
  return current;
}

[[nodiscard]] inline SharedRegistryTestStatusV1
shared_registry_test_status() noexcept {
  using namespace shared_registry_detail;
  SharedRegistryTestStatusV1 result{};
  SharedRegistryTestControlV1 *control = test_control();
  if (control == nullptr) {
    return result;
  }
  result.pause_point = static_cast<SharedRegistryTestPointV1>(
      pg_atomic_read_u32(&control->pause_point));
  result.reached = pg_atomic_read_u32(&control->reached) != 0;
  result.released = pg_atomic_read_u32(&control->released) != 0;
  result.defer_background_initializer = pg_atomic_read_u32(
      &control->defer_background_initializer) != 0;
  result.inject_root_abi_mismatch =
      pg_atomic_read_u32(&control->inject_root_abi_mismatch) != 0;
  result.inject_root_magic_mismatch =
      pg_atomic_read_u32(&control->inject_root_magic_mismatch) != 0;
  result.last_cleanup_handle_was_pinned = pg_atomic_read_u32(
      &control->last_cleanup_handle_was_pinned) != 0;
  result.last_cleanup_release_verified = pg_atomic_read_u32(
      &control->last_cleanup_release_verified) != 0;
  result.allocation_failure_ordinal =
      pg_atomic_read_u32(&control->allocation_failure_ordinal);
  result.allocation_counter =
      pg_atomic_read_u32(&control->allocation_counter);
  result.last_cleanup_handle =
      pg_atomic_read_u32(&control->last_cleanup_handle);
  return result;
}

#endif

}  // namespace bioseq::cache::pg::shared_registry
