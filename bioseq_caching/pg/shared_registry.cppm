/**
 * @file bioseq_caching/pg/shared_registry.cppm
 * @brief Creates and shares the PostgreSQL cache registry on first use.
 *
 * Startup reserves a fixed header, transition records and optional test
 * controls in PostgreSQL shared memory. The first initializer creates a
 * dynamic shared-memory area (DSA) for cache indexes, dependency records
 * and adapter metadata. A pin keeps that area alive across backend exits.
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

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

#include "miscadmin.h"
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

inline constexpr std::uint32_t k_shared_registry_bootstrap_magic = 0x42534231U;
inline constexpr std::uint32_t k_shared_registry_bootstrap_abi_revision = 2;
inline constexpr std::uint32_t k_shared_registry_root_magic = 0x42535231U;
inline constexpr std::uint32_t k_shared_registry_root_abi_revision = 2;
inline constexpr std::uint32_t k_shared_registry_test_magic = 0x42535431U;
inline constexpr std::uint32_t k_shared_registry_test_abi_revision = 2;
inline constexpr std::uint32_t k_default_shared_registry_max_backends = 128;
inline constexpr std::uint32_t k_default_shared_registry_wait_timeout_ms = 1'000;
inline constexpr std::uint32_t k_default_shared_registry_wait_limit = 60;
inline constexpr std::uint32_t k_maximum_shared_registry_wait_limit = 600;
inline constexpr std::uint64_t k_default_shared_registry_dsa_limit_bytes = 512ULL << 20U;
inline constexpr std::uint32_t k_maximum_shared_registry_test_controls = 4;
inline constexpr std::uint32_t k_invalid_shared_registry_proc_number =
    (std::numeric_limits<std::uint32_t>::max)();

#ifdef BIOSEQ_CACHE_TESTING
inline constexpr std::uint32_t k_default_shared_registry_test_controls = 1;
#else
inline constexpr std::uint32_t k_default_shared_registry_test_controls = 0;
#endif

enum class shared_registry_error : std::uint32_t {
    none = 0,
    invalid_argument,
    invalid_configuration,
    not_preloaded,
    main_shmem_unavailable,
    transition_ledger_unavailable,
    test_sidecar_unavailable,
    abi_mismatch,
    not_ready,
    initializer_unavailable,
    initializer_still_alive,
    initializer_self_wait,
    wait_timeout,
    takeover_exhausted,
    recovery_cleanup_failed,
    dsa_create_failed,
    dsa_pin_failed,
    dsa_mapping_pin_failed,
    dsa_size_limit_failed,
    root_allocation_failed,
    reusable_allocation_failed,
    dependency_allocation_failed,
    extension_allocation_failed,
    reusable_initialization_failed,
    dependency_initialization_failed,
    extension_initialization_failed,
    root_validation_failed,
    reusable_validation_failed,
    dependency_validation_failed,
    extension_validation_failed,
    publication_lost,
    dsa_attach_failed,
    dsa_unpin_failed,
    bootstrap_failed,
    test_injected_allocation_failure,
    test_injected_abi_mismatch,
    test_injected_magic_mismatch,
};

enum class shared_registry_test_point : std::uint32_t {
    none = 0,
    before_handle_publish = 1,
};

struct shared_registry_backend_identity {
    std::uint32_t proc_number = k_invalid_shared_registry_proc_number;
    std::int32_t pid = 0;
    std::uint64_t backend_nonce = 0;
    std::int64_t backend_started_at_us = 0;

    [[nodiscard]] bool valid() const noexcept {
        return proc_number != k_invalid_shared_registry_proc_number && pid > 0 &&
               backend_nonce != 0 && backend_started_at_us != 0;
    }

    [[nodiscard]] bool operator==(const shared_registry_backend_identity&) const noexcept = default;
};

struct shared_registry_owner_token {
    shared_registry_backend_identity owner{};
    std::uint64_t claim_sequence = 0;

    [[nodiscard]] bool valid() const noexcept {
        return owner.valid() && claim_sequence != 0;
    }

    [[nodiscard]] bool operator==(const shared_registry_owner_token&) const noexcept = default;
};

struct shared_registry_config {
    reusable_registry_config reusable{};
    std::uint32_t max_backends = k_default_shared_registry_max_backends;
    std::uint32_t dependency_partition_count = k_default_dependency_directory_partitions;
    std::uint32_t dependency_reclaim_scan_limit = k_default_dependency_reclaim_scan;
    std::uint32_t transition_precise_reference_capacity = k_maximum_transition_references;
    std::uint32_t extension_metadata_bytes = 0;
    std::uint32_t test_control_count = k_default_shared_registry_test_controls;
    std::uint32_t wait_timeout_ms = k_default_shared_registry_wait_timeout_ms;
    std::uint32_t wait_limit = k_default_shared_registry_wait_limit;
    std::uint64_t dsa_allocator_reserve_bytes = 0;
    std::uint64_t dsa_size_limit_bytes = k_default_shared_registry_dsa_limit_bytes;
    std::uint32_t reserved = 0;
};

struct shared_registry_persistent_layout {
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
    std::uint64_t dsa_allocator_reserve_bytes = 0;
    std::uint64_t fixed_dsa_reserved_bytes = 0;
    std::uint64_t dsa_size_limit_bytes = 0;

    [[nodiscard]] bool
    operator==(const shared_registry_persistent_layout&) const noexcept = default;
};

struct shared_registry_preload_request {
    shared_registry_config requested_config{};
    reusable_registry_preload_request reusable{};
    dependency_directory_preload_request dependency{};
    transition_ledger_preload_result transition{};
    shared_registry_persistent_layout persistent{};
    shared_registry_error error = shared_registry_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == shared_registry_error::none;
    }
};

/** Shared layout and DSA offsets, with counters that change during use. */
struct shared_registry_root {
    std::uint32_t magic = 0;
    std::uint32_t abi_revision = 0;
    std::uint32_t root_bytes = 0;
    std::uint32_t reserved = 0;
    std::uint64_t boot_nonce = 0;
    shared_registry_persistent_layout layout{};
    dsa_pointer reusable_registry = InvalidDsaPointer;
    dsa_pointer reusable_wait_cvs = InvalidDsaPointer;
    dsa_pointer dependency_directory = InvalidDsaPointer;
    dsa_pointer extension_metadata = InvalidDsaPointer;
    pg_atomic_uint64 hot_live_bytes{};
    pg_atomic_uint64 hot_allocation_failures{};
};

/** Shared initialization state, published DSA handles and recovery information. */
struct shared_registry_bootstrap_header {
    std::uint32_t magic = 0;
    std::uint32_t abi_revision = 0;
    std::uint32_t header_bytes = 0;
    std::uint32_t dsa_tranche_id = 0;
    shared_registry_persistent_layout layout{};
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
    shared_registry_owner_token initializer{};
    shared_registry_owner_token last_initializer{};
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

struct shared_registry_test_control {
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

struct shared_registry_test_sidecar {
    std::uint32_t magic = 0;
    std::uint32_t abi_revision = 0;
    std::uint32_t control_count = 0;
    std::uint32_t reserved = 0;
    std::uint64_t boot_nonce = 0;
    std::array<shared_registry_test_control, k_maximum_shared_registry_test_controls> controls{};
};

struct shared_registry_callbacks {
    void* context = nullptr;
    shared_registry_backend_identity (*current_owner)(void*) noexcept = nullptr;
    bool (*owner_is_alive)(void*, const shared_registry_owner_token&) noexcept = nullptr;
    bool (*initialize_extension)(void*, std::span<std::byte>, std::uint64_t) noexcept = nullptr;
    bool (*validate_extension)(void*, std::span<std::byte>, std::uint64_t) noexcept = nullptr;
};

/** Pointers and mapped views valid only in the attached process. */
struct shared_registry_attachment {
    dsa_area* area = nullptr;
    shared_registry_root* root = nullptr;
    reusable_registry_attachment reusable{};
    std::span<ConditionVariable> reusable_wait_cvs{};
    dependency_directory_attachment dependency{};
    std::span<std::byte> extension_metadata{};
    dsa_handle handle = DSA_HANDLE_INVALID;
    bool mapping_pinned = false;
    std::array<std::uint8_t, 3> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return area != nullptr && root != nullptr && static_cast<bool>(reusable) &&
               !reusable_wait_cvs.empty() && static_cast<bool>(dependency) &&
               handle != DSA_HANDLE_INVALID && mapping_pinned;
    }
};

struct shared_registry_attach_result {
    shared_registry_attachment attachment{};
    shared_registry_error error = shared_registry_error::none;
    bootstrap_state state = bootstrap_state::uninitialized;
    bool initialized_here = false;
    bool takeover = false;
    std::array<std::uint8_t, 2> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == shared_registry_error::none && static_cast<bool>(attachment);
    }
};

struct shared_registry_diagnostics {
    bootstrap_state state = bootstrap_state::uninitialized;
    shared_registry_error failure = shared_registry_error::none;
    std::uint64_t boot_nonce = 0;
    std::uint64_t emergency_write_epoch = 0;
    std::uint64_t emergency_change_generation = 0;
    std::uint64_t reserved_dsa_bytes = 0;
    std::uint64_t fixed_dsa_reserved_bytes = 0;
    std::uint64_t reserved_file_bytes = 0;
    std::uint64_t complete_entries = 0;
    std::uint64_t inflight_entries = 0;
    std::uint64_t indexed_generations = 0;
    std::uint64_t retired_generations = 0;
    std::uint64_t evictions = 0;
    std::uint64_t invalidations = 0;
    shared_registry_persistent_layout layout{};
    shared_registry_owner_token initializer{};
    shared_registry_owner_token last_initializer{};
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

struct shared_registry_test_status {
    shared_registry_test_point pause_point = shared_registry_test_point::none;
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
enum class shared_registry_test_manifest_corruption : std::uint32_t {
    magic = 1,
    abi_revision = 2,
    boot_nonce = 3,
};
#endif

/** Local pointers to the shared emergency-dependency state. */
struct shared_registry_emergency_access {
    pg_atomic_uint64* write_epoch = nullptr;
    pg_atomic_uint64* change_generation = nullptr;
    pg_atomic_uint32* transition_count = nullptr;
    ConditionVariable* state_changed = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept {
        return write_epoch != nullptr && change_generation != nullptr &&
               transition_count != nullptr && state_changed != nullptr;
    }
};

/** Local pointers to shared cache size and activity counters. */
struct shared_registry_accounting_access {
    pg_atomic_uint64* reserved_dsa_bytes = nullptr;
    pg_atomic_uint64* reserved_file_bytes = nullptr;
    pg_atomic_uint64* complete_entries = nullptr;
    pg_atomic_uint64* inflight_entries = nullptr;
    pg_atomic_uint64* indexed_generations = nullptr;
    pg_atomic_uint64* retired_generations = nullptr;
    pg_atomic_uint64* evictions = nullptr;
    pg_atomic_uint64* invalidations = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept {
        return reserved_dsa_bytes != nullptr && reserved_file_bytes != nullptr &&
               complete_entries != nullptr && inflight_entries != nullptr &&
               indexed_generations != nullptr && retired_generations != nullptr &&
               evictions != nullptr && invalidations != nullptr;
    }
};

static_assert(std::is_trivially_copyable_v<shared_registry_backend_identity>);
static_assert(std::is_trivially_copyable_v<shared_registry_owner_token>);
static_assert(std::is_trivially_copyable_v<shared_registry_persistent_layout>);
static_assert(std::is_trivially_copyable_v<shared_registry_root>);
static_assert(std::is_trivially_copyable_v<shared_registry_bootstrap_header>);
static_assert(std::is_standard_layout_v<shared_registry_root>);
static_assert(std::is_standard_layout_v<shared_registry_bootstrap_header>);
static_assert(std::is_standard_layout_v<shared_registry_test_sidecar>);
static_assert(alignof(shared_registry_bootstrap_header) <= MAXIMUM_ALIGNOF);
static_assert(alignof(shared_registry_test_sidecar) <= MAXIMUM_ALIGNOF);

namespace shared_registry_detail {

inline constexpr char k_bootstrap_shmem_name[] = "bioseq_cache shared registry bootstrap";
inline constexpr char k_transition_shmem_name[] = "bioseq_cache transition ledger";
inline constexpr char k_test_shmem_name[] = "bioseq_cache shared registry tests";
inline constexpr char k_bootstrap_lock_tranche[] = "bioseq_cache shared registry bootstrap lock";
inline constexpr char k_transition_lock_tranche[] = "bioseq_cache transition owner locks";
inline constexpr char k_dependency_lock_tranche[] = "bioseq_cache dependency partition locks";
inline constexpr char k_reusable_membership_lock_tranche[] =
    "bioseq_cache shared reusable membership lock";
inline constexpr char k_reusable_exact_lock_tranche[] = "bioseq_cache shared reusable exact locks";
inline constexpr char k_reusable_coverage_lock_tranche[] =
    "bioseq_cache shared reusable coverage locks";
inline constexpr char k_dsa_tranche_name[] = "bioseq_cache shared root dsa";

inline shared_registry_bootstrap_header* g_bootstrap = nullptr;
inline shared_registry_test_sidecar* g_test_sidecar = nullptr;
inline LWLock* g_bootstrap_lock = nullptr;
inline LWLockPadded* g_transition_locks = nullptr;
inline LWLockPadded* g_dependency_locks = nullptr;
inline LWLockPadded* g_reusable_membership_lock = nullptr;
inline LWLockPadded* g_reusable_exact_locks = nullptr;
inline LWLockPadded* g_reusable_coverage_locks = nullptr;
inline transition_ledger_attachment g_transition_ledger{};
inline shared_registry_preload_request g_requested{};
inline shared_registry_attachment g_attachment{};
inline dsa_area* g_initializing_area = nullptr;
inline shared_registry_owner_token g_initializing_owner{};
inline bool g_initializing_area_pinned = false;
inline bool g_initializing_mapping_pinned = false;
inline bool g_exit_callback_registered = false;

[[nodiscard]] constexpr bool valid_state(std::uint32_t raw) noexcept {
    return raw <= static_cast<std::uint32_t>(bootstrap_state::failed);
}

[[nodiscard]] constexpr bool valid_error(std::uint32_t raw) noexcept {
    return raw <= static_cast<std::uint32_t>(shared_registry_error::test_injected_magic_mismatch);
}

[[nodiscard]] bootstrap_state load_state() noexcept {
    if (g_bootstrap == nullptr) {
        return bootstrap_state::failed;
    }
    const std::uint32_t raw = pg_atomic_read_u32(&g_bootstrap->state);
    return valid_state(raw) ? static_cast<bootstrap_state>(raw) : bootstrap_state::failed;
}

[[nodiscard]] std::uint64_t load_boot_nonce() noexcept {
    return g_bootstrap == nullptr ? 0 : pg_atomic_read_u64(&g_bootstrap->boot_nonce);
}

void store_state(bootstrap_state state) noexcept {
    pg_write_barrier();
    pg_atomic_write_u32(&g_bootstrap->state, static_cast<std::uint32_t>(state));
}

[[nodiscard]] shared_registry_error load_failure() noexcept {
    if (g_bootstrap == nullptr) {
        return shared_registry_error::not_preloaded;
    }
    return static_cast<shared_registry_error>(pg_atomic_read_u32(&g_bootstrap->failure_code));
}

void store_failure(shared_registry_error error) noexcept {
    pg_atomic_write_u32(&g_bootstrap->failure_code, static_cast<std::uint32_t>(error));
}

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

[[nodiscard]] constexpr std::uint64_t
backend_nonce(std::uint32_t proc_number, std::int32_t pid, std::int64_t started_at_us) noexcept {
    const std::uint64_t value =
        static_cast<std::uint64_t>(started_at_us) ^
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pid)) << 32U) ^ proc_number;
    return value == 0 ? 1 : value;
}

[[nodiscard]] shared_registry_backend_identity default_current_owner(void*) noexcept {
    const std::uint32_t proc_number = static_cast<std::uint32_t>(MyProcNumber);
    const std::int64_t started_at = static_cast<std::int64_t>(MyStartTimestamp);
    if (MyProcPid <= 0 || proc_number == static_cast<std::uint32_t>(INVALID_PROC_NUMBER) ||
        started_at == 0) {
        return {};
    }
    return shared_registry_backend_identity{
        .proc_number = proc_number,
        .pid = MyProcPid,
        .backend_nonce = backend_nonce(proc_number, MyProcPid, started_at),
        .backend_started_at_us = started_at,
    };
}

[[nodiscard]] bool default_owner_is_alive(void*,
                                          const shared_registry_owner_token& token) noexcept {
    const shared_registry_backend_identity& owner = token.owner;
    if (!token.valid() || ProcGlobal == nullptr ||
        owner.proc_number >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    PGPROC* process = ProcNumberGetProc(static_cast<ProcNumber>(owner.proc_number));
    if (process == nullptr || process->pid != owner.pid) {
        return false;
    }
    volatile PgBackendStatus* status =
        pgstat_get_beentry_by_proc_number(static_cast<ProcNumber>(owner.proc_number));
    /* Treat the matching PGPROC as alive while activity statistics are
     * missing or changing. Only a stable record can show that its start
     * token belongs to a different backend. */
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
        if (pgstat_read_activity_complete(before_changecount, after_changecount)) {
            stable = true;
            break;
        }
    }
    if (!stable) {
        return true;
    }
    return status_pid == owner.pid && status_started_at == owner.backend_started_at_us &&
           backend_nonce(owner.proc_number, owner.pid, status_started_at) == owner.backend_nonce;
}

[[nodiscard]] shared_registry_backend_identity
current_owner(const shared_registry_callbacks& callbacks) noexcept {
    return callbacks.current_owner == nullptr ? default_current_owner(nullptr)
                                              : callbacks.current_owner(callbacks.context);
}

[[nodiscard]] bool owner_is_alive(const shared_registry_callbacks& callbacks,
                                  const shared_registry_owner_token& owner) noexcept {
    return callbacks.owner_is_alive == nullptr ? default_owner_is_alive(nullptr, owner)
                                               : callbacks.owner_is_alive(callbacks.context, owner);
}

[[nodiscard]] bool request_matches(const shared_registry_preload_request& left,
                                   const shared_registry_preload_request& right) noexcept {
    return left && right && left.persistent == right.persistent &&
           std::memcmp(
               &left.reusable.config, &right.reusable.config, sizeof(left.reusable.config)) == 0 &&
           std::memcmp(
               &left.reusable.layout, &right.reusable.layout, sizeof(left.reusable.layout)) == 0 &&
           left.dependency.config == right.dependency.config &&
           left.dependency.layout == right.dependency.layout &&
           left.transition.config.owner_capacity == right.transition.config.owner_capacity &&
           left.transition.config.precise_reference_capacity ==
               right.transition.config.precise_reference_capacity &&
           left.transition.config.dependency_partition_count ==
               right.transition.config.dependency_partition_count &&
           left.transition.layout.total_bytes == right.transition.layout.total_bytes &&
           left.transition.layout.owners_offset == right.transition.layout.owners_offset &&
           left.transition.layout.references_offset == right.transition.layout.references_offset &&
           left.transition.layout.references_per_owner ==
               right.transition.layout.references_per_owner;
}

[[nodiscard]] bool bootstrap_matches(const shared_registry_preload_request& request,
                                     std::uint64_t boot_nonce) noexcept {
    return g_bootstrap != nullptr && g_bootstrap->magic == k_shared_registry_bootstrap_magic &&
           g_bootstrap->abi_revision == k_shared_registry_bootstrap_abi_revision &&
           g_bootstrap->header_bytes == sizeof(shared_registry_bootstrap_header) &&
           load_boot_nonce() == boot_nonce && g_bootstrap->layout == request.persistent &&
           g_bootstrap->dsa_tranche_id != 0 &&
           g_bootstrap->exact_directory_capacity == request.persistent.reusable_exact_capacity &&
           g_bootstrap->coverage_directory_capacity ==
               request.persistent.reusable_coverage_capacity &&
           g_bootstrap->dependency_directory_capacity ==
               request.persistent.dependency_physical_capacity &&
           g_bootstrap->directory_partition_count ==
               request.persistent.dependency_partition_count &&
           valid_state(pg_atomic_read_u32(&g_bootstrap->state)) &&
           valid_error(pg_atomic_read_u32(&g_bootstrap->failure_code));
}

[[nodiscard]] shared_registry_owner_token
claim_owner_locked(const shared_registry_backend_identity& identity) noexcept {
    std::uint64_t sequence = pg_atomic_fetch_add_u64(&g_bootstrap->next_claim_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(&g_bootstrap->next_claim_sequence, 1);
    }
    if (sequence == 0) {
        sequence = 1;
        pg_atomic_write_u64(&g_bootstrap->next_claim_sequence, 2);
    }
    const shared_registry_owner_token result{
        .owner = identity,
        .claim_sequence = sequence,
    };
    g_bootstrap->initializer = result;
    saturating_increment(g_bootstrap->initialization_attempt_count);
    store_failure(shared_registry_error::none);
    store_state(bootstrap_state::initializing);
    return result;
}

[[nodiscard]] bool try_create_area(std::uint32_t tranche_id, dsa_area** output) noexcept {
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

[[nodiscard]] bool try_attach_area(dsa_handle handle, dsa_area** output) noexcept {
    if (output == nullptr || handle == DSA_HANDLE_INVALID || TopMemoryContext == nullptr) {
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

[[nodiscard]] bool try_pin_area(dsa_area* area) noexcept {
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

[[nodiscard]] bool try_pin_mapping(dsa_area* area) noexcept {
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

[[nodiscard]] bool try_unpin_area(dsa_area* area) noexcept {
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

[[nodiscard]] bool try_set_size_limit(dsa_area* area, std::uint64_t limit) noexcept {
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

void detach_area(dsa_area* area) noexcept {
    if (area != nullptr) {
        dsa_detach(area);
    }
}

[[nodiscard]] bool extension_callbacks_valid(const shared_registry_preload_request& request,
                                             const shared_registry_callbacks& callbacks) noexcept {
    return request.persistent.extension_metadata_bytes == 0 ||
           (callbacks.initialize_extension != nullptr && callbacks.validate_extension != nullptr);
}

void acquire_dependency_partition(void*, std::uint32_t partition) noexcept {
    LWLockAcquire(&g_dependency_locks[partition].lock, LW_EXCLUSIVE);
}

void release_dependency_partition(void*, std::uint32_t partition) noexcept {
    LWLockRelease(&g_dependency_locks[partition].lock);
}

[[nodiscard]] dependency_directory_callbacks dependency_callbacks(bool unpublished) noexcept {
    if (unpublished) {
        return dependency_directory_callbacks{
            .lock_mode = dependency_directory_lock_mode::already_locked,
        };
    }
    return dependency_directory_callbacks{
        .lock_mode = dependency_directory_lock_mode::acquire_release,
        .acquire_partition = acquire_dependency_partition,
        .release_partition = release_dependency_partition,
    };
}

[[nodiscard]] shared_registry_test_control* test_control() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
    if (g_test_sidecar != nullptr && g_test_sidecar->control_count != 0) {
        return &g_test_sidecar->controls[0];
    }
#endif
    return nullptr;
}

/** Reads the bootstrap fault requested by the test environment before DSA initialization. */
void configure_test_control_from_environment(shared_registry_test_control& control) noexcept {
#ifdef BIOSEQ_CACHE_TESTING
    const char* fault = std::getenv("BIOSEQ_CACHE_TEST_BOOTSTRAP_FAULT");
    if (fault == nullptr || fault[0] == '\0') {
        return;
    }
    if (std::strcmp(fault, "pause_backend_once") == 0) {
        pg_atomic_write_u32(
            &control.pause_point,
            static_cast<std::uint32_t>(shared_registry_test_point::before_handle_publish));
        pg_atomic_write_u32(&control.defer_background_initializer, 1);
        return;
    }
    if (std::strcmp(fault, "pause_once") == 0) {
        pg_atomic_write_u32(
            &control.pause_point,
            static_cast<std::uint32_t>(shared_registry_test_point::before_handle_publish));
        return;
    }
    if (std::strncmp(fault, "alloc_", 6) == 0 && fault[6] >= '1' && fault[6] <= '5' &&
        fault[7] == '\0') {
        pg_atomic_write_u32(&control.allocation_failure_ordinal,
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

/** Keeps background workers from consuming a pause intended for a test backend. */
[[nodiscard]] bool defer_background_initializer() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
    shared_registry_test_control* control = test_control();
    return control != nullptr && AmBackgroundWorkerProcess() &&
           pg_atomic_read_u32(&control->defer_background_initializer) != 0;
#else
    return false;
#endif
}

[[nodiscard]] bool inject_allocation_failure(std::uint32_t ordinal) noexcept {
#ifdef BIOSEQ_CACHE_TESTING
    shared_registry_test_control* control = test_control();
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
    shared_registry_test_control* control = test_control();
    return control != nullptr && pg_atomic_read_u32(&control->inject_root_abi_mismatch) != 0;
#else
    return false;
#endif
}

[[nodiscard]] bool inject_root_magic_mismatch() noexcept {
#ifdef BIOSEQ_CACHE_TESTING
    shared_registry_test_control* control = test_control();
    return control != nullptr && pg_atomic_read_u32(&control->inject_root_magic_mismatch) != 0;
#else
    return false;
#endif
}

void pause_before_publication() {
#ifdef BIOSEQ_CACHE_TESTING
    shared_registry_test_control* control = test_control();
    if (control == nullptr ||
        pg_atomic_read_u32(&control->pause_point) !=
            static_cast<std::uint32_t>(shared_registry_test_point::before_handle_publish)) {
        return;
    }
    std::uint32_t expected = 0;
    if (!pg_atomic_compare_exchange_u32(&control->reached, &expected, 1)) {
        return;
    }
    ConditionVariableBroadcast(&control->changed);
    ConditionVariablePrepareToSleep(&control->changed);
    while (pg_atomic_read_u32(&control->released) == 0) {
        static_cast<void>(ConditionVariableTimedSleep(&control->changed, 100, PG_WAIT_EXTENSION));
        CHECK_FOR_INTERRUPTS();
    }
    static_cast<void>(ConditionVariableCancelSleep());
#endif
}

[[nodiscard]] dsa_pointer
allocate_zeroed(dsa_area* area, std::size_t bytes, std::uint32_t ordinal) noexcept {
    if (bytes == 0 || inject_allocation_failure(ordinal)) {
        return InvalidDsaPointer;
    }
    return dsa_allocate_extended(area, bytes, DSA_ALLOC_NO_OOM | DSA_ALLOC_ZERO);
}

[[nodiscard]] bool root_layout_matches(const shared_registry_root& root,
                                       const shared_registry_preload_request& request,
                                       std::uint64_t boot_nonce) noexcept {
    const bool extension_pointer_valid = request.persistent.extension_metadata_bytes == 0
                                             ? root.extension_metadata == InvalidDsaPointer
                                             : DsaPointerIsValid(root.extension_metadata);
    return root.magic == k_shared_registry_root_magic &&
           root.abi_revision == k_shared_registry_root_abi_revision &&
           root.root_bytes == sizeof(shared_registry_root) && root.reserved == 0 &&
           root.boot_nonce == boot_nonce && root.layout == request.persistent &&
           DsaPointerIsValid(root.reusable_registry) && DsaPointerIsValid(root.reusable_wait_cvs) &&
           DsaPointerIsValid(root.dependency_directory) && extension_pointer_valid &&
           root.reusable_registry != root.reusable_wait_cvs &&
           root.reusable_registry != root.dependency_directory &&
           root.reusable_wait_cvs != root.dependency_directory &&
           (request.persistent.extension_metadata_bytes == 0 ||
            (root.extension_metadata != root.reusable_registry &&
             root.extension_metadata != root.reusable_wait_cvs &&
             root.extension_metadata != root.dependency_directory));
}

[[nodiscard]] shared_registry_attach_result
validate_area(dsa_area* area,
              dsa_handle handle,
              dsa_pointer root_pointer,
              const shared_registry_preload_request& request,
              const shared_registry_callbacks& callbacks,
              std::uint64_t boot_nonce,
              bool unpublished) noexcept {
    shared_registry_attach_result result{};
    if (area == nullptr || handle == DSA_HANDLE_INVALID || !DsaPointerIsValid(root_pointer)) {
        result.error = shared_registry_error::root_validation_failed;
        return result;
    }
    auto* root = static_cast<shared_registry_root*>(dsa_get_address(area, root_pointer));
    if (root == nullptr || !root_layout_matches(*root, request, boot_nonce)) {
        result.error = shared_registry_error::root_validation_failed;
        return result;
    }

    auto* reusable_bytes = static_cast<std::byte*>(dsa_get_address(area, root->reusable_registry));
    if (reusable_bytes == nullptr) {
        result.error = shared_registry_error::reusable_validation_failed;
        return result;
    }
    auto reusable = attach_reusable_registry(
        std::span<std::byte>{reusable_bytes, request.persistent.reusable_bytes});
    if (!reusable.has_value()) {
        result.error = shared_registry_error::reusable_validation_failed;
        return result;
    }
    if (unpublished && validate_reusable_registry(*reusable) != reusable_registry_error::none) {
        result.error = shared_registry_error::reusable_validation_failed;
        return result;
    }

    auto* wait_cvs =
        static_cast<ConditionVariable*>(dsa_get_address(area, root->reusable_wait_cvs));
    if (wait_cvs == nullptr || request.persistent.reusable_wait_cv_capacity == 0 ||
        request.persistent.reusable_wait_cv_bytes !=
            request.persistent.reusable_wait_cv_capacity * sizeof(ConditionVariable)) {
        result.error = shared_registry_error::reusable_validation_failed;
        return result;
    }

    auto* dependency_bytes =
        static_cast<std::byte*>(dsa_get_address(area, root->dependency_directory));
    if (dependency_bytes == nullptr || g_dependency_locks == nullptr) {
        result.error = shared_registry_error::dependency_validation_failed;
        return result;
    }
    const dependency_directory_callbacks locks = dependency_callbacks(unpublished);
    auto dependency = attach_dependency_directory(
        std::span<std::byte>{dependency_bytes, request.persistent.dependency_bytes}, locks);
    if (!dependency) {
        result.error = shared_registry_error::dependency_validation_failed;
        return result;
    }
    if (unpublished && deep_validate_dependency_directory(dependency.directory, locks) !=
                           dependency_directory_error::none) {
        result.error = shared_registry_error::dependency_validation_failed;
        return result;
    }

    std::span<std::byte> extension{};
    if (request.persistent.extension_metadata_bytes != 0) {
        auto* extension_bytes =
            static_cast<std::byte*>(dsa_get_address(area, root->extension_metadata));
        if (extension_bytes == nullptr || callbacks.validate_extension == nullptr) {
            result.error = shared_registry_error::extension_validation_failed;
            return result;
        }
        extension =
            std::span<std::byte>{extension_bytes, request.persistent.extension_metadata_bytes};
        if (!callbacks.validate_extension(callbacks.context, extension, boot_nonce)) {
            result.error = shared_registry_error::extension_validation_failed;
            return result;
        }
    }

    result.attachment = shared_registry_attachment{
        .area = area,
        .root = root,
        .reusable = *reusable,
        .reusable_wait_cvs =
            std::span<ConditionVariable>{wait_cvs, request.persistent.reusable_wait_cv_capacity},
        .dependency = dependency.directory,
        .extension_metadata = extension,
        .handle = handle,
        .mapping_pinned = true,
    };
    result.state = bootstrap_state::ready;
    return result;
}

void clear_process_attachment() noexcept {
    g_attachment = {};
    g_initializing_area = nullptr;
    g_initializing_owner = {};
    g_initializing_area_pinned = false;
    g_initializing_mapping_pinned = false;
}

struct unpublished_area_cleanup {
    dsa_handle handle = DSA_HANDLE_INVALID;
    bool was_pinned = false;
    bool pin_removed = true;
    bool released = true;
};

/** Saves the cleaned-up handle and release-check result for tests. */
void record_test_cleanup(const unpublished_area_cleanup& cleanup) noexcept {
#ifdef BIOSEQ_CACHE_TESTING
    shared_registry_test_control* control = test_control();
    if (control == nullptr) {
        return;
    }
    pg_atomic_write_u32(&control->last_cleanup_handle, cleanup.handle);
    pg_atomic_write_u32(&control->last_cleanup_handle_was_pinned, cleanup.was_pinned ? 1U : 0U);
    pg_atomic_write_u32(&control->last_cleanup_release_verified, cleanup.released ? 1U : 0U);
#else
    static_cast<void>(cleanup);
#endif
}

/** Returns true when the handle is invalid or a test attachment fails. */
[[nodiscard]] bool area_handle_released_once(dsa_handle handle) noexcept {
    if (handle == DSA_HANDLE_INVALID) {
        return true;
    }
    dsa_area* probe = nullptr;
    if (!try_attach_area(handle, &probe)) {
        return true;
    }
    detach_area(probe);
    return false;
}

/** Waits a limited time for the old DSA handle to stop accepting attachments after owner cleanup. */
[[nodiscard]] bool wait_for_area_handle_release(dsa_handle handle) {
    if (area_handle_released_once(handle)) {
        return true;
    }
    const std::uint32_t attempts = std::max<std::uint32_t>(1, g_requested.persistent.wait_limit);
    for (std::uint32_t attempt = 1; attempt < attempts; ++attempt) {
        ConditionVariablePrepareToSleep(&g_bootstrap->state_changed);
        if (area_handle_released_once(handle)) {
            static_cast<void>(ConditionVariableCancelSleep());
            return true;
        }
        static_cast<void>(
            ConditionVariableTimedSleep(&g_bootstrap->state_changed,
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

/** Tries to unpin and detach an unpublished area, then optionally checks its release. */
[[nodiscard]] unpublished_area_cleanup
cleanup_local_unpublished_area(dsa_area* area, bool pinned, bool verify_release) noexcept {
    unpublished_area_cleanup result{
        .handle = area == nullptr ? DSA_HANDLE_INVALID : dsa_get_handle(area),
        .was_pinned = pinned,
    };
    const bool unpinned = !pinned || try_unpin_area(area);
    detach_area(area);
    result.pin_removed = unpinned;
    result.released = unpinned && verify_release && area_handle_released_once(result.handle);
    record_test_cleanup(result);
    return result;
}

void shared_registry_backend_exit(int, Datum) noexcept {
    dsa_area* area = g_initializing_area != nullptr ? g_initializing_area : g_attachment.area;
    const shared_registry_owner_token owner = g_initializing_owner;
    const bool unpublished = g_initializing_area != nullptr;
    const bool pinned = g_initializing_area_pinned;
    /* Attaching a DSA area during process exit can raise FATAL. Leave the
     * handle and pin status for the next initializer to check. */
    const unpublished_area_cleanup cleanup =
        unpublished ? cleanup_local_unpublished_area(area, pinned, false)
                    : unpublished_area_cleanup{};
    if (!unpublished) {
        detach_area(area);
    }
    clear_process_attachment();

    if (!unpublished || g_bootstrap == nullptr || g_bootstrap_lock == nullptr || !owner.valid()) {
        return;
    }
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() == bootstrap_state::initializing && g_bootstrap->initializer == owner) {
        pg_atomic_write_u64(&g_bootstrap->reserved_dsa_bytes, 0);
        g_bootstrap->cleanup_handle = cleanup.released ? DSA_HANDLE_INVALID : cleanup.handle;
        g_bootstrap->cleanup_handle_pinned = cleanup.was_pinned && !cleanup.pin_removed ? 1U : 0U;
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

void record_initializer_failure(const shared_registry_owner_token& owner,
                                shared_registry_error error) noexcept {
    dsa_area* area = g_initializing_area;
    const unpublished_area_cleanup cleanup =
        cleanup_local_unpublished_area(area, area != nullptr && g_initializing_area_pinned, true);
    clear_process_attachment();

    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() == bootstrap_state::initializing && g_bootstrap->initializer == owner) {
        pg_atomic_write_u64(&g_bootstrap->reserved_dsa_bytes, 0);
        g_bootstrap->cleanup_handle = cleanup.released ? DSA_HANDLE_INVALID : cleanup.handle;
        g_bootstrap->cleanup_handle_pinned = cleanup.was_pinned && !cleanup.pin_removed ? 1U : 0U;
        if (cleanup.released) {
            if (g_bootstrap->live_unpublished_area_count != 0) {
                --g_bootstrap->live_unpublished_area_count;
            }
            saturating_increment(g_bootstrap->cleanup_count);
        }
        saturating_increment(g_bootstrap->validation_failure_count);
        g_bootstrap->initializer = {};
        store_failure(cleanup.released ? error : shared_registry_error::recovery_cleanup_failed);
        store_state(bootstrap_state::failed);
    }
    LWLockRelease(g_bootstrap_lock);
    ConditionVariableBroadcast(&g_bootstrap->state_changed);
}

[[nodiscard]] bool recover_unpublished_area(dsa_handle handle, bool pinned) {
    if (handle == DSA_HANDLE_INVALID) {
        return true;
    }
    dsa_area* area = nullptr;
    if (!try_attach_area(handle, &area)) {
        record_test_cleanup(unpublished_area_cleanup{
            .handle = handle,
            .was_pinned = pinned,
            .released = true,
        });
        return true;
    }
    const bool unpinned = !pinned || try_unpin_area(area);
    detach_area(area);
    const bool released = unpinned && wait_for_area_handle_release(handle);
    record_test_cleanup(unpublished_area_cleanup{
        .handle = handle,
        .was_pinned = pinned,
        .released = released,
    });
    return released;
}

[[nodiscard]] bool finish_takeover_cleanup(const shared_registry_owner_token& owner,
                                           dsa_handle old_handle,
                                           bool old_handle_pinned,
                                           bool cleanup_required) {
    const bool cleaned = recover_unpublished_area(old_handle, old_handle_pinned);
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    const bool still_owner =
        load_state() == bootstrap_state::initializing && g_bootstrap->initializer == owner;
    if (still_owner) {
        pg_atomic_write_u64(&g_bootstrap->reserved_dsa_bytes, 0);
    }
    if (still_owner && cleanup_required) {
        if (g_bootstrap->live_unpublished_area_count != 0) {
            --g_bootstrap->live_unpublished_area_count;
        }
        saturating_increment(g_bootstrap->cleanup_count);
    }
    if (still_owner && !cleaned) {
        g_bootstrap->initializer = {};
        store_failure(shared_registry_error::recovery_cleanup_failed);
        store_state(bootstrap_state::failed);
    }
    LWLockRelease(g_bootstrap_lock);
    if (!cleaned) {
        ConditionVariableBroadcast(&g_bootstrap->state_changed);
    }
    return cleaned && still_owner;
}

[[nodiscard]] shared_registry_attach_result
initialize_claimed_area(const shared_registry_preload_request& request,
                        const shared_registry_callbacks& callbacks,
                        const shared_registry_owner_token& owner,
                        bool takeover) {
    shared_registry_attach_result result{};
    result.state = bootstrap_state::initializing;
    result.initialized_here = true;
    result.takeover = takeover;
    ensure_exit_callback_registered();

    dsa_area* area = nullptr;
    if (!try_create_area(g_bootstrap->dsa_tranche_id, &area)) {
        result.error = shared_registry_error::dsa_create_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    g_initializing_area = area;
    g_initializing_owner = owner;
    const dsa_handle handle = dsa_get_handle(area);
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    const bool still_owner =
        load_state() == bootstrap_state::initializing && g_bootstrap->initializer == owner;
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
        result.error = shared_registry_error::publication_lost;
        record_initializer_failure(owner, result.error);
        return result;
    }

    if (!try_set_size_limit(area, request.persistent.dsa_size_limit_bytes)) {
        result.error = shared_registry_error::dsa_size_limit_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    if (!try_pin_area(area)) {
        result.error = shared_registry_error::dsa_pin_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    g_initializing_area_pinned = true;
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() == bootstrap_state::initializing && g_bootstrap->initializer == owner) {
        g_bootstrap->cleanup_handle_pinned = 1;
    }
    LWLockRelease(g_bootstrap_lock);
    if (!try_pin_mapping(area)) {
        result.error = shared_registry_error::dsa_mapping_pin_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    g_initializing_mapping_pinned = true;

    const dsa_pointer root_pointer = allocate_zeroed(area, sizeof(shared_registry_root), 1);
    if (!DsaPointerIsValid(root_pointer)) {
        result.error = inject_allocation_failure(1)
                           ? shared_registry_error::test_injected_allocation_failure
                           : shared_registry_error::root_allocation_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    const dsa_pointer reusable_pointer =
        allocate_zeroed(area, request.persistent.reusable_bytes, 2);
    if (!DsaPointerIsValid(reusable_pointer)) {
        result.error = inject_allocation_failure(2)
                           ? shared_registry_error::test_injected_allocation_failure
                           : shared_registry_error::reusable_allocation_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    const dsa_pointer dependency_pointer =
        allocate_zeroed(area, request.persistent.dependency_bytes, 3);
    if (!DsaPointerIsValid(dependency_pointer)) {
        result.error = inject_allocation_failure(3)
                           ? shared_registry_error::test_injected_allocation_failure
                           : shared_registry_error::dependency_allocation_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    const dsa_pointer wait_cvs_pointer =
        allocate_zeroed(area, request.persistent.reusable_wait_cv_bytes, 4);
    if (!DsaPointerIsValid(wait_cvs_pointer)) {
        result.error = inject_allocation_failure(4)
                           ? shared_registry_error::test_injected_allocation_failure
                           : shared_registry_error::reusable_allocation_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    dsa_pointer extension_pointer = InvalidDsaPointer;
    if (request.persistent.extension_metadata_bytes != 0) {
        extension_pointer = allocate_zeroed(area, request.persistent.extension_metadata_bytes, 5);
        if (!DsaPointerIsValid(extension_pointer)) {
            result.error = inject_allocation_failure(5)
                               ? shared_registry_error::test_injected_allocation_failure
                               : shared_registry_error::extension_allocation_failed;
            record_initializer_failure(owner, result.error);
            return result;
        }
    }

    auto* root = static_cast<shared_registry_root*>(dsa_get_address(area, root_pointer));
    *root = shared_registry_root{
        .magic = k_shared_registry_root_magic,
        .abi_revision = k_shared_registry_root_abi_revision,
        .root_bytes = sizeof(shared_registry_root),
        .boot_nonce = load_boot_nonce(),
        .layout = request.persistent,
        .reusable_registry = reusable_pointer,
        .reusable_wait_cvs = wait_cvs_pointer,
        .dependency_directory = dependency_pointer,
        .extension_metadata = extension_pointer,
    };
    pg_atomic_init_u64(&root->hot_live_bytes, 0);
    pg_atomic_init_u64(&root->hot_allocation_failures, 0);

    auto* reusable_bytes = static_cast<std::byte*>(dsa_get_address(area, reusable_pointer));
    auto reusable = startup_reusable_registry(
        std::span<std::byte>{reusable_bytes, request.persistent.reusable_bytes},
        load_boot_nonce(),
        request.reusable.config,
        false);
    if (!reusable.has_value() ||
        validate_reusable_registry(*reusable) != reusable_registry_error::none) {
        result.error = shared_registry_error::reusable_initialization_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }
    auto* wait_cvs = static_cast<ConditionVariable*>(dsa_get_address(area, wait_cvs_pointer));
    for (std::uint32_t index = 0; index < request.persistent.reusable_wait_cv_capacity; ++index) {
        ConditionVariableInit(&wait_cvs[index]);
    }

    auto* dependency_bytes = static_cast<std::byte*>(dsa_get_address(area, dependency_pointer));
    const std::span<std::byte> dependency_region{dependency_bytes,
                                                 request.persistent.dependency_bytes};
    if (initialize_dependency_directory(dependency_region, request.dependency) !=
        dependency_directory_error::none) {
        result.error = shared_registry_error::dependency_initialization_failed;
        record_initializer_failure(owner, result.error);
        return result;
    }

    if (request.persistent.extension_metadata_bytes != 0) {
        auto* extension_bytes = static_cast<std::byte*>(dsa_get_address(area, extension_pointer));
        const std::span<std::byte> extension{extension_bytes,
                                             request.persistent.extension_metadata_bytes};
        if (callbacks.initialize_extension == nullptr ||
            !callbacks.initialize_extension(callbacks.context, extension, load_boot_nonce())) {
            result.error = shared_registry_error::extension_initialization_failed;
            record_initializer_failure(owner, result.error);
            return result;
        }
    }

    if (inject_root_magic_mismatch()) {
        ++root->magic;
    }
    if (inject_root_abi_mismatch()) {
        ++root->abi_revision;
    }
    shared_registry_attach_result validated =
        validate_area(area, handle, root_pointer, request, callbacks, load_boot_nonce(), true);
    if (!validated) {
        result.error =
            inject_root_magic_mismatch() ? shared_registry_error::test_injected_magic_mismatch
            : inject_root_abi_mismatch() ? shared_registry_error::test_injected_abi_mismatch
                                         : validated.error;
        record_initializer_failure(owner, result.error);
        return result;
    }

    pause_before_publication();

    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    const bool publishable =
        load_state() == bootstrap_state::initializing && g_bootstrap->initializer == owner;
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
        store_failure(shared_registry_error::none);
        store_state(bootstrap_state::ready);
    }
    LWLockRelease(g_bootstrap_lock);
    ConditionVariableBroadcast(&g_bootstrap->state_changed);
    if (!publishable) {
        result.error = shared_registry_error::publication_lost;
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
    validated.state = bootstrap_state::ready;
    return validated;
}

/** Marks the shared registry failed when a published root fails validation. */
void fail_published_validation(dsa_handle handle,
                               dsa_pointer root_pointer,
                               shared_registry_error error) noexcept {
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() == bootstrap_state::ready && g_bootstrap->published_handle == handle &&
        g_bootstrap->published_root == root_pointer) {
        saturating_increment(g_bootstrap->validation_failure_count);
        store_failure(error == shared_registry_error::none
                          ? shared_registry_error::root_validation_failed
                          : error);
        store_state(bootstrap_state::failed);
    }
    LWLockRelease(g_bootstrap_lock);
    ConditionVariableBroadcast(&g_bootstrap->state_changed);
}

[[nodiscard]] shared_registry_attach_result
attach_published_area(const shared_registry_preload_request& request,
                      const shared_registry_callbacks& callbacks) noexcept {
    shared_registry_attach_result result{};
    LWLockAcquire(g_bootstrap_lock, LW_SHARED);
    if (load_state() != bootstrap_state::ready) {
        result.error = shared_registry_error::not_ready;
        result.state = load_state();
        LWLockRelease(g_bootstrap_lock);
        return result;
    }
    pg_read_barrier();
    const dsa_handle handle = g_bootstrap->published_handle;
    const dsa_pointer root_pointer = g_bootstrap->published_root;
    const dsa_pointer reusable_pointer = g_bootstrap->published_reusable;
    const dsa_pointer wait_cvs_pointer = g_bootstrap->published_reusable_wait_cvs;
    const dsa_pointer dependency_pointer = g_bootstrap->published_dependency;
    const dsa_pointer extension_pointer = g_bootstrap->published_extension;
    const std::uint32_t exact_capacity = g_bootstrap->exact_directory_capacity;
    const std::uint32_t coverage_capacity = g_bootstrap->coverage_directory_capacity;
    const std::uint32_t dependency_capacity = g_bootstrap->dependency_directory_capacity;
    const std::uint32_t partition_count = g_bootstrap->directory_partition_count;
    const std::uint64_t boot_nonce = load_boot_nonce();
    LWLockRelease(g_bootstrap_lock);

    dsa_area* area = nullptr;
    if (!try_attach_area(handle, &area)) {
        result.error = shared_registry_error::dsa_attach_failed;
        result.state = bootstrap_state::ready;
        return result;
    }
    if (!try_pin_mapping(area)) {
        detach_area(area);
        result.error = shared_registry_error::dsa_mapping_pin_failed;
        result.state = bootstrap_state::ready;
        return result;
    }
    result = validate_area(area, handle, root_pointer, request, callbacks, boot_nonce, false);
    if (result && (result.attachment.root->reusable_registry != reusable_pointer ||
                   result.attachment.root->reusable_wait_cvs != wait_cvs_pointer ||
                   result.attachment.root->dependency_directory != dependency_pointer ||
                   result.attachment.root->extension_metadata != extension_pointer ||
                   exact_capacity != request.persistent.reusable_exact_capacity ||
                   coverage_capacity != request.persistent.reusable_coverage_capacity ||
                   dependency_capacity != request.persistent.dependency_physical_capacity ||
                   partition_count != request.persistent.dependency_partition_count)) {
        result.attachment = {};
        result.error = shared_registry_error::root_validation_failed;
        result.state = bootstrap_state::ready;
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
    const bool still_initializing = load_state() == bootstrap_state::initializing;
    LWLockRelease(g_bootstrap_lock);
    if (still_initializing) {
        static_cast<void>(
            ConditionVariableTimedSleep(&g_bootstrap->state_changed,
                                        static_cast<long>(g_requested.persistent.wait_timeout_ms),
                                        PG_WAIT_EXTENSION));
    }
    static_cast<void>(ConditionVariableCancelSleep());
    CHECK_FOR_INTERRUPTS();
}

} // namespace shared_registry_detail

/** Checks configuration and calculates the shared-memory layouts and DSA reservation. */
[[nodiscard]] inline shared_registry_preload_request
make_shared_registry_preload_request(shared_registry_config config = {}) noexcept {
    shared_registry_preload_request result{};
    if (config.max_backends == 0 || config.max_backends > k_maximum_transition_owners ||
        config.dependency_partition_count == 0 ||
        config.dependency_partition_count > k_maximum_dependency_directory_partitions ||
        config.dependency_reclaim_scan_limit == 0 ||
        config.dependency_reclaim_scan_limit > k_maximum_dependency_vector_size ||
        config.transition_precise_reference_capacity == 0 ||
        config.transition_precise_reference_capacity > k_maximum_transition_references ||
        config.test_control_count > k_maximum_shared_registry_test_controls ||
        config.wait_timeout_ms == 0 || config.wait_limit == 0 ||
        config.wait_limit > k_maximum_shared_registry_wait_limit ||
        config.dsa_allocator_reserve_bytes == 0 || config.dsa_size_limit_bytes == 0 ||
        config.reserved != 0) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }
#ifndef BIOSEQ_CACHE_TESTING
    if (config.test_control_count != 0) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }
#endif

    const std::uint64_t generation_capacity = std::min<std::uint64_t>(
        k_maximum_reusable_registry_generations,
        std::max<std::uint64_t>(k_default_reusable_registry_generations,
                                static_cast<std::uint64_t>(config.max_backends) * 64U));
    config.reusable.max_generations = static_cast<std::uint32_t>(generation_capacity);
    result.requested_config = config;

    const auto reusable = make_reusable_registry_preload_request(config.reusable);
    if (!reusable.has_value()) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }
    result.reusable = *reusable;

    dependency_directory_config dependency_config{
        .logical_capacity = dependency_directory_logical_capacity_for_backends(config.max_backends),
        .partition_count = config.dependency_partition_count,
        .reclaim_scan_limit = config.dependency_reclaim_scan_limit,
        .pressure_percent = k_dependency_directory_pressure_percent,
        .backend_reference_limit = config.max_backends,
        .generation_reference_limit = reusable->config.max_generations,
    };
    const dependency_directory_preload_result dependency =
        make_dependency_directory_preload_request(dependency_config);
    if (!dependency) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }
    result.dependency = dependency.request;

    result.transition = transition_ledger_preload_request(transition_ledger_config{
        .owner_capacity = config.max_backends,
        .precise_reference_capacity = config.transition_precise_reference_capacity,
        .dependency_partition_count = config.dependency_partition_count,
    });
    if (!result.transition) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }

    const auto wait_cv_payload =
        checked_mul(static_cast<std::uint64_t>(reusable->config.max_generations),
                    static_cast<std::uint64_t>(sizeof(ConditionVariable)));
    if (!wait_cv_payload.has_value()) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }
    std::array<std::uint64_t, 5> fixed_payloads{
        sizeof(shared_registry_root),
        reusable->layout.total_bytes,
        *wait_cv_payload,
        dependency.request.layout.total_bytes,
        config.extension_metadata_bytes,
    };
    const std::size_t fixed_payload_count =
        config.extension_metadata_bytes == 0 ? 4U : fixed_payloads.size();
    const auto fixed_dsa_reserved_bytes = conservative_fixed_dsa_reservation_bytes(
        config.dsa_allocator_reserve_bytes,
        std::span<const std::uint64_t>{fixed_payloads.data(), fixed_payload_count});
    if (!fixed_dsa_reserved_bytes.has_value()) {
        result.error = shared_registry_error::invalid_configuration;
        return result;
    }

    result.persistent = shared_registry_persistent_layout{
        .root_bytes = sizeof(shared_registry_root),
        .reusable_bytes = reusable->layout.total_bytes,
        .reusable_max_generations = reusable->config.max_generations,
        .reusable_exact_capacity = reusable->config.exact_directory_capacity,
        .reusable_coverage_capacity = reusable->config.coverage_directory_capacity,
        .reusable_partition_count = reusable->config.partition_count,
        .reusable_wait_cv_bytes = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(reusable->config.max_generations) *
            sizeof(ConditionVariable)),
        .reusable_wait_cv_capacity = reusable->config.max_generations,
        .dependency_bytes = dependency.request.layout.total_bytes,
        .dependency_logical_capacity = dependency.request.layout.logical_capacity,
        .dependency_physical_capacity = dependency.request.layout.physical_capacity,
        .dependency_partition_count = dependency.request.config.partition_count,
        .transition_ledger_bytes = result.transition.layout.total_bytes,
        .transition_owner_capacity = result.transition.config.owner_capacity,
        .transition_references_per_owner = result.transition.layout.references_per_owner,
        .extension_metadata_bytes = config.extension_metadata_bytes,
        .test_control_count = config.test_control_count,
        .wait_timeout_ms = config.wait_timeout_ms,
        .wait_limit = config.wait_limit,
        .dsa_allocator_reserve_bytes = config.dsa_allocator_reserve_bytes,
        .fixed_dsa_reserved_bytes = *fixed_dsa_reserved_bytes,
        .dsa_size_limit_bytes = config.dsa_size_limit_bytes,
    };
    if (result.persistent.fixed_dsa_reserved_bytes > result.persistent.dsa_size_limit_bytes) {
        result.error = shared_registry_error::invalid_configuration;
    }
    return result;
}

/** Reserves startup shared memory and the locks used by the registry. */
inline void request_shared_registry_resources(const shared_registry_preload_request& request) {
    using namespace shared_registry_detail;
    g_requested = request;
    if (!request) {
        ereport(ERROR, (errmsg("bioseq_cache shared registry configuration is invalid")));
    }
    RequestAddinShmemSpace(MAXALIGN(sizeof(shared_registry_bootstrap_header)));
    RequestAddinShmemSpace(MAXALIGN(request.transition.layout.total_bytes));
    if (request.persistent.test_control_count != 0) {
        RequestAddinShmemSpace(MAXALIGN(sizeof(shared_registry_test_sidecar)));
    }
    RequestNamedLWLockTranche(k_bootstrap_lock_tranche, 1);
    RequestNamedLWLockTranche(k_transition_lock_tranche, request.transition.owner_lock_count);
    RequestNamedLWLockTranche(k_dependency_lock_tranche, request.dependency.partition_lock_count);
    RequestNamedLWLockTranche(k_reusable_membership_lock_tranche,
                              request.reusable.membership_lock_count);
    RequestNamedLWLockTranche(k_reusable_exact_lock_tranche,
                              request.reusable.exact_partition_lock_count);
    RequestNamedLWLockTranche(k_reusable_coverage_lock_tranche,
                              request.reusable.coverage_partition_lock_count);
}

/** Initializes the startup header and fixed shared records. DSA creation happens on first use. */
[[nodiscard]] inline bool startup_shared_registry(const shared_registry_preload_request& request,
                                                  std::uint64_t boot_nonce) noexcept {
    using namespace shared_registry_detail;
    if (!request || boot_nonce == 0 || (!g_requested || !request_matches(request, g_requested))) {
        return false;
    }
    bool found = false;
    g_bootstrap = static_cast<shared_registry_bootstrap_header*>(
        ShmemInitStruct(k_bootstrap_shmem_name, sizeof(shared_registry_bootstrap_header), &found));
    g_bootstrap_lock = &GetNamedLWLockTranche(k_bootstrap_lock_tranche)[0].lock;
    g_transition_locks = GetNamedLWLockTranche(k_transition_lock_tranche);
    g_dependency_locks = GetNamedLWLockTranche(k_dependency_lock_tranche);
    g_reusable_membership_lock = GetNamedLWLockTranche(k_reusable_membership_lock_tranche);
    g_reusable_exact_locks = GetNamedLWLockTranche(k_reusable_exact_lock_tranche);
    g_reusable_coverage_locks = GetNamedLWLockTranche(k_reusable_coverage_lock_tranche);
    if (g_bootstrap == nullptr || g_bootstrap_lock == nullptr || g_transition_locks == nullptr ||
        g_dependency_locks == nullptr || g_reusable_membership_lock == nullptr ||
        g_reusable_exact_locks == nullptr || g_reusable_coverage_locks == nullptr) {
        return false;
    }
    if (!found) {
        std::memset(g_bootstrap, 0, sizeof(*g_bootstrap));
        g_bootstrap->magic = k_shared_registry_bootstrap_magic;
        g_bootstrap->abi_revision = k_shared_registry_bootstrap_abi_revision;
        g_bootstrap->header_bytes = sizeof(*g_bootstrap);
        pg_atomic_init_u64(&g_bootstrap->boot_nonce, boot_nonce);
        g_bootstrap->layout = request.persistent;
        g_bootstrap->dsa_tranche_id = static_cast<std::uint32_t>(LWLockNewTrancheId(
#if PG_VERSION_NUM >= 190000
            k_dsa_tranche_name
#endif
        ));
        g_bootstrap->exact_directory_capacity = request.persistent.reusable_exact_capacity;
        g_bootstrap->coverage_directory_capacity = request.persistent.reusable_coverage_capacity;
        g_bootstrap->dependency_directory_capacity =
            request.persistent.dependency_physical_capacity;
        g_bootstrap->directory_partition_count = request.persistent.dependency_partition_count;
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
        pg_atomic_init_u32(&g_bootstrap->state,
                           static_cast<std::uint32_t>(bootstrap_state::uninitialized));
        pg_atomic_init_u32(&g_bootstrap->failure_code,
                           static_cast<std::uint32_t>(shared_registry_error::none));
    } else if (!bootstrap_matches(request, boot_nonce)) {
        return false;
    }
#if PG_VERSION_NUM < 190000
    LWLockRegisterTranche(static_cast<int>(g_bootstrap->dsa_tranche_id), k_dsa_tranche_name);
#endif

    bool transition_found = false;
    auto* transition_bytes = static_cast<std::byte*>(ShmemInitStruct(
        k_transition_shmem_name, request.transition.layout.total_bytes, &transition_found));
    if (transition_bytes == nullptr) {
        store_failure(shared_registry_error::transition_ledger_unavailable);
        store_state(bootstrap_state::failed);
        return false;
    }
    const std::span<std::byte> transition_region{transition_bytes,
                                                 request.transition.layout.total_bytes};
    if (!transition_found && initialize_transition_ledger(transition_region, request.transition) !=
                                 transition_ledger_error::none) {
        store_failure(shared_registry_error::transition_ledger_unavailable);
        store_state(bootstrap_state::failed);
        return false;
    }
    const transition_ledger_attach_result transition = attach_transition_ledger(transition_region);
    if (!transition) {
        store_failure(shared_registry_error::transition_ledger_unavailable);
        store_state(bootstrap_state::failed);
        return false;
    }
    g_transition_ledger = transition.ledger;

    if (request.persistent.test_control_count != 0) {
        bool test_found = false;
        g_test_sidecar = static_cast<shared_registry_test_sidecar*>(
            ShmemInitStruct(k_test_shmem_name, sizeof(shared_registry_test_sidecar), &test_found));
        if (g_test_sidecar == nullptr) {
            store_failure(shared_registry_error::test_sidecar_unavailable);
            store_state(bootstrap_state::failed);
            return false;
        }
        if (!test_found) {
            std::memset(g_test_sidecar, 0, sizeof(*g_test_sidecar));
            g_test_sidecar->magic = k_shared_registry_test_magic;
            g_test_sidecar->abi_revision = k_shared_registry_test_abi_revision;
            g_test_sidecar->control_count = request.persistent.test_control_count;
            g_test_sidecar->boot_nonce = boot_nonce;
            for (shared_registry_test_control& control : g_test_sidecar->controls) {
                pg_atomic_init_u32(&control.pause_point, 0);
                pg_atomic_init_u32(&control.reached, 0);
                pg_atomic_init_u32(&control.released, 0);
                pg_atomic_init_u32(&control.defer_background_initializer, 0);
                pg_atomic_init_u32(&control.allocation_failure_ordinal, 0);
                pg_atomic_init_u32(&control.inject_root_abi_mismatch, 0);
                pg_atomic_init_u32(&control.inject_root_magic_mismatch, 0);
                pg_atomic_init_u32(&control.allocation_counter, 0);
                pg_atomic_init_u32(&control.last_cleanup_handle, 0);
                pg_atomic_init_u32(&control.last_cleanup_handle_was_pinned, 0);
                pg_atomic_init_u32(&control.last_cleanup_release_verified, 0);
                ConditionVariableInit(&control.changed);
                configure_test_control_from_environment(control);
            }
        } else if (g_test_sidecar->magic != k_shared_registry_test_magic ||
                   g_test_sidecar->abi_revision != k_shared_registry_test_abi_revision ||
                   g_test_sidecar->control_count != request.persistent.test_control_count ||
                   g_test_sidecar->boot_nonce != boot_nonce) {
            store_failure(shared_registry_error::test_sidecar_unavailable);
            store_state(bootstrap_state::failed);
            return false;
        }
    }
    return load_state() != bootstrap_state::failed;
}

/** Registers detach cleanup early so later storage callbacks run before it. */
inline void register_shared_registry_exit_callback() noexcept {
    shared_registry_detail::ensure_exit_callback_registered();
}

/**
 * Attaches this process to the cache registry, creating the DSA area if
 * initialization is allowed and nobody has claimed it. Other callers wait
 * for publication. If the initializer dies, one caller may take over after
 * cleaning up the unfinished area. Waiting and takeover attempts are limited.
 */
[[nodiscard]] inline shared_registry_attach_result
attach_shared_registry(const shared_registry_preload_request& request,
                       const shared_registry_callbacks& callbacks = {},
                       bool allow_initialize = true) {
    using namespace shared_registry_detail;
    shared_registry_attach_result result{};
    if (!request || g_bootstrap == nullptr || g_bootstrap_lock == nullptr || !g_requested ||
        !request_matches(request, g_requested)) {
        result.error = shared_registry_error::not_preloaded;
        result.state = bootstrap_state::failed;
        return result;
    }
    if (!extension_callbacks_valid(request, callbacks)) {
        result.error = shared_registry_error::invalid_argument;
        result.state = load_state();
        return result;
    }
    if (g_attachment) {
        result.attachment = g_attachment;
        result.state = bootstrap_state::ready;
        return result;
    }
    const bool initialize_allowed = allow_initialize && !defer_background_initializer();

    for (std::uint32_t wait_index = 0; wait_index < request.persistent.wait_limit; ++wait_index) {
        const bootstrap_state observed_state = load_state();
        if (observed_state == bootstrap_state::ready) {
            return attach_published_area(request, callbacks);
        }
        if (observed_state == bootstrap_state::failed) {
            result.error = load_failure() == shared_registry_error::none
                               ? shared_registry_error::bootstrap_failed
                               : load_failure();
            result.state = bootstrap_state::failed;
            return result;
        }
        if (observed_state == bootstrap_state::uninitialized) {
            if (!initialize_allowed) {
                result.error = shared_registry_error::not_ready;
                result.state = observed_state;
                return result;
            }
            const shared_registry_backend_identity identity = current_owner(callbacks);
            if (!identity.valid()) {
                result.error = shared_registry_error::initializer_unavailable;
                result.state = observed_state;
                return result;
            }
            LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
            if (load_state() == bootstrap_state::uninitialized) {
                const shared_registry_owner_token owner = claim_owner_locked(identity);
                LWLockRelease(g_bootstrap_lock);
                ConditionVariableBroadcast(&g_bootstrap->state_changed);
                return initialize_claimed_area(request, callbacks, owner, false);
            }
            LWLockRelease(g_bootstrap_lock);
            continue;
        }

        timed_initialization_wait();
        LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
        if (load_state() != bootstrap_state::initializing) {
            LWLockRelease(g_bootstrap_lock);
            continue;
        }
        const shared_registry_owner_token observed_owner = g_bootstrap->initializer;
        saturating_increment(g_bootstrap->wait_count);
        LWLockRelease(g_bootstrap_lock);

        const shared_registry_backend_identity identity = current_owner(callbacks);
        if (identity.valid() && observed_owner.owner == identity) {
            result.error = shared_registry_error::initializer_self_wait;
            result.state = bootstrap_state::initializing;
            return result;
        }

        /* Check owner liveness twice while no registry lock is held. */
        const bool first_alive = owner_is_alive(callbacks, observed_owner);
        CHECK_FOR_INTERRUPTS();
        const bool second_alive = owner_is_alive(callbacks, observed_owner);
        if (first_alive || second_alive) {
            continue;
        }
        if (!initialize_allowed || !identity.valid()) {
            result.error = shared_registry_error::initializer_unavailable;
            result.state = bootstrap_state::initializing;
            return result;
        }

        dsa_handle old_handle = DSA_HANDLE_INVALID;
        bool old_handle_pinned = false;
        bool cleanup_required = false;
        shared_registry_owner_token takeover_owner{};
        LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
        if (load_state() == bootstrap_state::initializing &&
            g_bootstrap->initializer == observed_owner) {
            if (g_bootstrap->takeover_used != 0) {
                g_bootstrap->initializer = {};
                store_failure(shared_registry_error::takeover_exhausted);
                store_state(bootstrap_state::failed);
                LWLockRelease(g_bootstrap_lock);
                ConditionVariableBroadcast(&g_bootstrap->state_changed);
                result.error = shared_registry_error::takeover_exhausted;
                result.state = bootstrap_state::failed;
                return result;
            }
            std::uint32_t expected = static_cast<std::uint32_t>(bootstrap_state::initializing);
            if (pg_atomic_compare_exchange_u32(
                    &g_bootstrap->state,
                    &expected,
                    static_cast<std::uint32_t>(bootstrap_state::uninitialized))) {
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
                takeover_owner, old_handle, old_handle_pinned, cleanup_required)) {
            result.error = shared_registry_error::recovery_cleanup_failed;
            result.state = load_state();
            return result;
        }
        return initialize_claimed_area(request, callbacks, takeover_owner, true);
    }

    result.error = shared_registry_error::wait_timeout;
    result.state = load_state();
    return result;
}

/** Reads the shared initialization state without attaching to DSA or taking a lock. */
[[nodiscard]] inline bootstrap_state shared_registry_bootstrap_state() noexcept {
    return shared_registry_detail::load_state();
}

[[nodiscard]] inline bool shared_registry_ready() noexcept {
    return shared_registry_detail::g_attachment &&
           shared_registry_bootstrap_state() == bootstrap_state::ready;
}

[[nodiscard]] inline const shared_registry_attachment*
current_shared_registry_attachment() noexcept {
    return shared_registry_ready() ? &shared_registry_detail::g_attachment : nullptr;
}

/** Detaches this process from the published area while keeping its shared lifetime pin. */
inline void detach_shared_registry_local_mapping() noexcept {
    using namespace shared_registry_detail;
    if (g_initializing_area != nullptr) {
        return;
    }
    dsa_area* area = g_attachment.area;
    g_attachment = {};
    detach_area(area);
}

[[nodiscard]] inline transition_ledger_attachment shared_transition_ledger() noexcept {
    return shared_registry_detail::g_transition_ledger;
}

[[nodiscard]] inline LWLockPadded* shared_transition_owner_locks() noexcept {
    return shared_registry_detail::g_transition_locks;
}

[[nodiscard]] inline LWLockPadded* shared_dependency_partition_locks() noexcept {
    return shared_registry_detail::g_dependency_locks;
}

[[nodiscard]] inline LWLockPadded* shared_reusable_membership_lock() noexcept {
    return shared_registry_detail::g_reusable_membership_lock;
}

[[nodiscard]] inline LWLockPadded* shared_reusable_exact_locks() noexcept {
    return shared_registry_detail::g_reusable_exact_locks;
}

[[nodiscard]] inline LWLockPadded* shared_reusable_coverage_locks() noexcept {
    return shared_registry_detail::g_reusable_coverage_locks;
}

[[nodiscard]] inline shared_registry_emergency_access shared_emergency_dependency() noexcept {
    using namespace shared_registry_detail;
    if (g_bootstrap == nullptr || g_bootstrap->magic != k_shared_registry_bootstrap_magic ||
        g_bootstrap->abi_revision != k_shared_registry_bootstrap_abi_revision) {
        return {};
    }
    return shared_registry_emergency_access{
        .write_epoch = &g_bootstrap->emergency_write_epoch,
        .change_generation = &g_bootstrap->emergency_change_generation,
        .transition_count = &g_bootstrap->emergency_transition_count,
        .state_changed = &g_bootstrap->emergency_state_changed,
    };
}

[[nodiscard]] inline shared_registry_accounting_access shared_registry_accounting() noexcept {
    using namespace shared_registry_detail;
    if (g_bootstrap == nullptr || g_bootstrap->magic != k_shared_registry_bootstrap_magic ||
        g_bootstrap->abi_revision != k_shared_registry_bootstrap_abi_revision) {
        return {};
    }
    return shared_registry_accounting_access{
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

[[nodiscard]] inline std::uint64_t next_shared_registry_backend_nonce() noexcept {
    using namespace shared_registry_detail;
    if (g_bootstrap == nullptr) {
        return 0;
    }
    std::uint64_t nonce = pg_atomic_fetch_add_u64(&g_bootstrap->next_backend_nonce, 1);
    if (nonce == 0) {
        nonce = pg_atomic_fetch_add_u64(&g_bootstrap->next_backend_nonce, 1);
    }
    return nonce == 0 ? 1 : nonce;
}

/** Returns a new claim sequence for this server run, or zero when unavailable or exhausted. */
[[nodiscard]] inline std::uint64_t next_shared_registry_claim_sequence() noexcept {
    using namespace shared_registry_detail;
    if (g_bootstrap == nullptr) {
        return 0;
    }
    uint64 observed = pg_atomic_read_u64(&g_bootstrap->next_claim_sequence);
    const uint64 maximum = (std::numeric_limits<uint64>::max)();
    while (observed != 0 && observed != maximum) {
        if (pg_atomic_compare_exchange_u64(
                &g_bootstrap->next_claim_sequence, &observed, observed + 1U)) {
            return observed;
        }
    }
    return 0;
}

[[nodiscard]] inline shared_registry_backend_identity current_backend_identity() noexcept {
    return shared_registry_detail::default_current_owner(nullptr);
}

[[nodiscard]] inline bool
backend_identity_alive(const shared_registry_owner_token& owner) noexcept {
    return shared_registry_detail::default_owner_is_alive(nullptr, owner);
}

[[nodiscard]] inline shared_registry_diagnostics snapshot_shared_registry_diagnostics() noexcept {
    using namespace shared_registry_detail;
    shared_registry_diagnostics result{};
    if (g_bootstrap == nullptr || g_bootstrap_lock == nullptr) {
        result.state = bootstrap_state::failed;
        result.failure = shared_registry_error::not_preloaded;
        return result;
    }
    LWLockAcquire(g_bootstrap_lock, LW_SHARED);
    result.state = load_state();
    result.failure = load_failure();
    result.boot_nonce = load_boot_nonce();
    result.emergency_write_epoch = pg_atomic_read_u64(&g_bootstrap->emergency_write_epoch);
    result.emergency_change_generation =
        pg_atomic_read_u64(&g_bootstrap->emergency_change_generation);
    result.emergency_transition_count =
        pg_atomic_read_u32(&g_bootstrap->emergency_transition_count);
    result.reserved_dsa_bytes = pg_atomic_read_u64(&g_bootstrap->reserved_dsa_bytes);
    result.fixed_dsa_reserved_bytes = g_bootstrap->layout.fixed_dsa_reserved_bytes;
    result.reserved_file_bytes = pg_atomic_read_u64(&g_bootstrap->reserved_file_bytes);
    result.complete_entries = pg_atomic_read_u64(&g_bootstrap->complete_entries);
    result.inflight_entries = pg_atomic_read_u64(&g_bootstrap->inflight_entries);
    result.indexed_generations = pg_atomic_read_u64(&g_bootstrap->indexed_generations);
    result.retired_generations = pg_atomic_read_u64(&g_bootstrap->retired_generations);
    result.evictions = pg_atomic_read_u64(&g_bootstrap->evictions);
    result.invalidations = pg_atomic_read_u64(&g_bootstrap->invalidations);
    result.layout = g_bootstrap->layout;
    result.initializer = g_bootstrap->initializer;
    result.last_initializer = g_bootstrap->last_initializer;
    result.initialization_attempt_count = g_bootstrap->initialization_attempt_count;
    result.takeover_count = g_bootstrap->takeover_count;
    result.cleanup_count = g_bootstrap->cleanup_count;
    result.wait_count = g_bootstrap->wait_count;
    result.attach_count = g_bootstrap->attach_count;
    result.validation_failure_count = g_bootstrap->validation_failure_count;
    result.takeover_used = g_bootstrap->takeover_used;
    result.live_unpublished_area_count = g_bootstrap->live_unpublished_area_count;
    result.cleanup_handle_pinned = g_bootstrap->cleanup_handle_pinned;
    result.orderly_unpinned = g_bootstrap->orderly_unpinned;
    result.exact_directory_capacity = g_bootstrap->exact_directory_capacity;
    result.coverage_directory_capacity = g_bootstrap->coverage_directory_capacity;
    result.dependency_directory_capacity = g_bootstrap->dependency_directory_capacity;
    result.directory_partition_count = g_bootstrap->directory_partition_count;
    result.cleanup_handle_published = g_bootstrap->cleanup_handle != DSA_HANDLE_INVALID;
    result.ready_handle_published = g_bootstrap->published_handle != DSA_HANDLE_INVALID;
    result.root_published = DsaPointerIsValid(g_bootstrap->published_root);
    LWLockRelease(g_bootstrap_lock);
    return result;
}

/** Releases the published area's lifetime pin during orderly janitor shutdown. */
[[nodiscard]] inline bool orderly_unpin_shared_registry() noexcept {
    using namespace shared_registry_detail;
    if (!g_attachment || g_bootstrap == nullptr || g_bootstrap_lock == nullptr) {
        return false;
    }
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    if (load_state() != bootstrap_state::ready || g_bootstrap->orderly_unpinned != 0) {
        LWLockRelease(g_bootstrap_lock);
        return false;
    }
    g_bootstrap->orderly_unpinned = 1;
    LWLockRelease(g_bootstrap_lock);
    if (!try_unpin_area(g_attachment.area)) {
        LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
        g_bootstrap->orderly_unpinned = 0;
        store_failure(shared_registry_error::dsa_unpin_failed);
        LWLockRelease(g_bootstrap_lock);
        return false;
    }
    return true;
}

#ifdef BIOSEQ_CACHE_TESTING

[[nodiscard]] inline bool
arm_shared_registry_test_pause(shared_registry_test_point point) noexcept {
    using namespace shared_registry_detail;
    shared_registry_test_control* control = test_control();
    if (control == nullptr || point != shared_registry_test_point::before_handle_publish) {
        return false;
    }
    pg_atomic_write_u32(&control->reached, 0);
    pg_atomic_write_u32(&control->released, 0);
    pg_atomic_write_u32(&control->pause_point, static_cast<std::uint32_t>(point));
    return true;
}

inline void release_shared_registry_test_pause() noexcept {
    using namespace shared_registry_detail;
    shared_registry_test_control* control = test_control();
    if (control == nullptr) {
        return;
    }
    pg_atomic_write_u32(&control->released, 1);
    pg_atomic_write_u32(&control->pause_point,
                        static_cast<std::uint32_t>(shared_registry_test_point::none));
    pg_atomic_write_u32(&control->defer_background_initializer, 0);
    ConditionVariableBroadcast(&control->changed);
}

[[nodiscard]] inline bool
inject_shared_registry_allocation_failure(std::uint32_t ordinal) noexcept {
    using namespace shared_registry_detail;
    shared_registry_test_control* control = test_control();
    if (control == nullptr || ordinal > 5) {
        return false;
    }
    pg_atomic_write_u32(&control->allocation_counter, 0);
    pg_atomic_write_u32(&control->allocation_failure_ordinal, ordinal);
    return true;
}

[[nodiscard]] inline bool inject_shared_registry_root_abi_mismatch(bool enabled) noexcept {
    using namespace shared_registry_detail;
    shared_registry_test_control* control = test_control();
    if (control == nullptr) {
        return false;
    }
    pg_atomic_write_u32(&control->inject_root_abi_mismatch, enabled ? 1U : 0U);
    return true;
}

[[nodiscard]] inline bool inject_shared_registry_root_magic_mismatch(bool enabled) noexcept {
    using namespace shared_registry_detail;
    shared_registry_test_control* control = test_control();
    if (control == nullptr) {
        return false;
    }
    pg_atomic_write_u32(&control->inject_root_magic_mismatch, enabled ? 1U : 0U);
    return true;
}

/** Changes a published root field so tests can check rejection during attachment. */
[[nodiscard]] inline bool corrupt_shared_registry_manifest_for_test(
    shared_registry_test_manifest_corruption corruption) noexcept {
    using namespace shared_registry_detail;
    if (!g_attachment || g_attachment.root == nullptr || g_bootstrap == nullptr ||
        g_bootstrap_lock == nullptr) {
        return false;
    }
    LWLockAcquire(g_bootstrap_lock, LW_EXCLUSIVE);
    const bool current =
        load_state() == bootstrap_state::ready &&
        g_bootstrap->published_handle == g_attachment.handle &&
        dsa_get_address(g_attachment.area, g_bootstrap->published_root) == g_attachment.root;
    if (current) {
        switch (corruption) {
        case shared_registry_test_manifest_corruption::magic:
            ++g_attachment.root->magic;
            break;
        case shared_registry_test_manifest_corruption::abi_revision:
            ++g_attachment.root->abi_revision;
            break;
        case shared_registry_test_manifest_corruption::boot_nonce:
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

[[nodiscard]] inline shared_registry_test_status snapshot_shared_registry_test_status() noexcept {
    using namespace shared_registry_detail;
    shared_registry_test_status result{};
    shared_registry_test_control* control = test_control();
    if (control == nullptr) {
        return result;
    }
    result.pause_point =
        static_cast<shared_registry_test_point>(pg_atomic_read_u32(&control->pause_point));
    result.reached = pg_atomic_read_u32(&control->reached) != 0;
    result.released = pg_atomic_read_u32(&control->released) != 0;
    result.defer_background_initializer =
        pg_atomic_read_u32(&control->defer_background_initializer) != 0;
    result.inject_root_abi_mismatch = pg_atomic_read_u32(&control->inject_root_abi_mismatch) != 0;
    result.inject_root_magic_mismatch =
        pg_atomic_read_u32(&control->inject_root_magic_mismatch) != 0;
    result.last_cleanup_handle_was_pinned =
        pg_atomic_read_u32(&control->last_cleanup_handle_was_pinned) != 0;
    result.last_cleanup_release_verified =
        pg_atomic_read_u32(&control->last_cleanup_release_verified) != 0;
    result.allocation_failure_ordinal = pg_atomic_read_u32(&control->allocation_failure_ordinal);
    result.allocation_counter = pg_atomic_read_u32(&control->allocation_counter);
    result.last_cleanup_handle = pg_atomic_read_u32(&control->last_cleanup_handle);
    return result;
}

#endif

} // namespace bioseq::cache::pg::shared_registry
