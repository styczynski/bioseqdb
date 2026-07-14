/*-------------------------------------------------------------------------
 *
 * Generic PostgreSQL storage for cache column-store blocks.
 *
 * This module owns no planner or provider policy.  It supplies a bounded
 * postmaster-lifetime DSA hot tier, a private file overflow tier, an ABA-safe
 * block ledger, generation manifests, reader pins, and deterministic
 * reclamation primitives for the PostgreSQL adapter.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
}

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <span>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <type_traits>
#include <unistd.h>

export module bioseq.cache.pg.storage;

import bioseq.cache.core;

export namespace bioseq::cache::pg::storage {

inline constexpr std::uint32_t kStorageAbiVersion = 1;
inline constexpr std::size_t kManifestCapacity = 256;
inline constexpr std::size_t kBlocksPerManifest = 32;
inline constexpr std::size_t kBlockLedgerCapacity =
    kManifestCapacity * kBlocksPerManifest + 256;
inline constexpr std::uint64_t kDefaultDsaInitialBytes = 256ULL << 10;
inline constexpr std::uint64_t kDefaultHotCeilingBytes = 256ULL << 20;
inline constexpr std::uint64_t kDefaultFileCeilingBytes = 128ULL << 30;
inline constexpr std::uint64_t kDefaultMaximumBlockBytes = 64ULL << 20;
inline constexpr std::int64_t kDefaultPendingTimeoutUs = 5LL * 60LL * 1000000LL;
inline constexpr std::uint32_t kDefaultSweepIntervalMs = 1000;
inline constexpr std::uint32_t kDefaultReclaimBatch = 64;

/** Fixed postmaster configuration; no additional production GUC is needed. */
struct StorageConfig {
    std::uint64_t dsa_initial_bytes = kDefaultDsaInitialBytes;
    std::uint64_t hot_ceiling_bytes = kDefaultHotCeilingBytes;
    std::uint64_t file_ceiling_bytes = kDefaultFileCeilingBytes;
    std::uint64_t maximum_block_bytes = kDefaultMaximumBlockBytes;
    std::int64_t pending_timeout_us = kDefaultPendingTimeoutUs;
    std::uint32_t sweep_interval_ms = kDefaultSweepIntervalMs;
    std::uint32_t reclaim_batch = kDefaultReclaimBatch;
    std::uint32_t soft_limit_percent = 80;
};

enum class StorageError : std::uint32_t {
    None = 0,
    NotPreloaded,
    BootstrapFailed,
    InvalidArgument,
    BlockTooLarge,
    LedgerFull,
    ManifestFull,
    DuplicateGeneration,
    MemoryPressure,
    HotTierFull,
    FileTierFull,
    FilesystemPressure,
    DsaAllocationFailed,
    UnsafePath,
    PathTooLong,
    OpenFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    ReadFailed,
    ShortRead,
    ChecksumMismatch,
    NotFound,
    StaleToken,
    InvalidState,
    Pinned,
};

enum class WorkerLifecycle : std::uint32_t {
    NotRegistered = 0,
    Registered,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class ManifestState : std::uint32_t {
    Empty = 0,
    Complete,
    Retiring,
};

/** An ABA-safe capability for one stored but not necessarily published block. */
struct StoredBlockToken {
    std::uint32_t ledger_slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;
    BlockReferenceV1 reference{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return sequence != 0
            && ledger_slot < kBlockLedgerCapacity
            && reference.length != 0;
    }
};

struct StoreBlockResult {
    StorageError error = StorageError::None;
    StoredBlockToken token{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(token);
    }
};

/** Immutable generation information published with its block manifest. */
struct ManifestDescriptor {
    CacheEntryId id{};
    CacheEntryKind kind = CacheEntryKind::ExactFinalResult;
    std::uint32_t flags = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::int64_t created_at_us = 0;
    std::int64_t expires_at_us = 0;
    std::uint32_t initial_waiter_references = 0;
    std::uint32_t reserved = 0;
};

struct ManifestHandle {
    std::uint32_t slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t slot_sequence = 0;
    CacheEntryId id{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < kManifestCapacity && slot_sequence != 0;
    }
};

struct PublishManifestResult {
    StorageError error = StorageError::None;
    ManifestHandle handle{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(handle);
    }
};

/** A reader-owned snapshot.  The shared generation remains pinned until release. */
struct ManifestPin {
    ManifestHandle handle{};
    CacheEntryKind kind = CacheEntryKind::ExactFinalResult;
    std::uint32_t flags = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint32_t block_count = 0;
    std::uint32_t reserved = 0;
    std::array<BlockReferenceV1, kBlocksPerManifest> blocks{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle) && block_count != 0;
    }
};

struct PinManifestResult {
    StorageError error = StorageError::None;
    ManifestPin pin{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(pin);
    }
};

struct JanitorSweepResult {
    std::uint32_t expired_manifests = 0;
    std::uint32_t lru_manifests = 0;
    std::uint32_t abandoned_blocks = 0;
    std::uint32_t reclaimed_blocks = 0;
    std::uint64_t reclaimed_hot_bytes = 0;
    std::uint64_t reclaimed_file_bytes = 0;
};

struct StorageRuntimeSnapshot {
    bool bootstrap_ready = false;
    bool dsa_attached = false;
    BootstrapState bootstrap_state = BootstrapState::Uninitialized;
    WorkerLifecycle worker_lifecycle = WorkerLifecycle::NotRegistered;
    CacheCorrectnessReadiness correctness_readiness =
        CacheCorrectnessReadiness::PassthroughOnly;
    StorageError last_failure = StorageError::None;
    std::uint64_t boot_nonce = 0;
    std::uint64_t reserved_hot_bytes = 0;
    std::uint64_t reserved_file_bytes = 0;
    std::uint64_t hot_ceiling_bytes = 0;
    std::uint64_t file_ceiling_bytes = 0;
    std::uint64_t manifest_count = 0;
    std::uint64_t completed_sweeps = 0;
    std::int64_t last_sweep_at_us = 0;
    std::int64_t worker_heartbeat_at_us = 0;
    int worker_pid = 0;
};

/** Return the main-shmem bytes that request_storage_resources() will request. */
[[nodiscard]] std::size_t storage_shared_memory_size(
    const StorageConfig& config = {}) noexcept;

/** Request main shared memory and a named LWLock tranche during preload. */
void request_storage_resources(const StorageConfig& config = {});

/** Initialize/attach the shared header and pinned in-place DSA in startup hook. */
[[nodiscard]] bool startup_storage(
    std::uint64_t boot_nonce,
    const StorageConfig& config = {});

/** Attach this backend/worker to the postmaster-lifetime storage state. */
[[nodiscard]] bool attach_storage();

/** Return true only when shared bootstrap and this process's DSA attach succeeded. */
[[nodiscard]] bool storage_ready() noexcept;

/** Store a checksummed block, preferring DSA and falling back to a private file. */
[[nodiscard]] StoreBlockResult store_block(std::span<const std::byte> bytes);

/** Discard a stored block that was not transferred into a manifest. */
[[nodiscard]] StorageError discard_unpublished_block(
    const StoredBlockToken& token);

/** Atomically publish a generation and transfer all tokens into its manifest. */
[[nodiscard]] PublishManifestResult publish_manifest(
    const ManifestDescriptor& descriptor,
    std::span<const StoredBlockToken> blocks);

/** Pin one non-expired complete manifest for a reader. */
[[nodiscard]] PinManifestResult pin_manifest(
    const CacheEntryId& id,
    std::int64_t now_us = 0);

/** Register a pre-expiry waiter whose entitlement can survive TTL expiry. */
[[nodiscard]] StorageError register_manifest_waiter(
    const ManifestHandle& handle,
    std::int64_t now_us = 0);

/** Convert one registered waiter to a reader pin even after expiry. */
[[nodiscard]] PinManifestResult convert_waiter_to_pin(
    const ManifestHandle& handle);

/** Release one reader pin exactly once. */
[[nodiscard]] StorageError unpin_manifest(const ManifestPin& pin) noexcept;

/** Release one waiter reference without pinning. */
[[nodiscard]] StorageError unregister_manifest_waiter(
    const ManifestHandle& handle) noexcept;

/** Copy and checksum-verify one block from a pinned generation. */
[[nodiscard]] StorageError read_pinned_block(
    const ManifestPin& pin,
    std::size_t block_index,
    std::span<std::byte> destination);

/** Mark a generation for reclamation. Existing pins drain before physical free. */
[[nodiscard]] StorageError retire_manifest(const ManifestHandle& handle) noexcept;

/** Run one bounded TTL/LRU/abandoned-producer reclamation pass. */
[[nodiscard]] JanitorSweepResult janitor_sweep(
    std::int64_t now_us = 0,
    bool aggressive = false);

/** Ask the static janitor for a normal or aggressive sweep. */
void request_janitor_sweep(bool aggressive = false) noexcept;

/** Publish the highest correctness level actually proven by the adapter. */
[[nodiscard]] bool publish_correctness_readiness(
    CacheCorrectnessReadiness readiness) noexcept;

/** Record worker lifecycle/heartbeat without exposing the shared header. */
void publish_worker_lifecycle(
    WorkerLifecycle lifecycle,
    int worker_pid = 0) noexcept;
void publish_worker_heartbeat(std::int64_t now_us = 0) noexcept;

/** Snapshot bounded counters and lifecycle diagnostics. */
[[nodiscard]] StorageRuntimeSnapshot runtime_snapshot() noexcept;

/** Authoritative interval copied into shared state at startup. */
[[nodiscard]] std::uint32_t janitor_interval_ms() noexcept;

} // namespace bioseq::cache::pg::storage

namespace bioseq::cache::pg::storage {
namespace {

inline constexpr std::uint32_t kStorageMagic = 0x42535452U; /* BSTR */
inline constexpr char kStorageSharedName[] = "bioseq_cache storage v1";
inline constexpr char kStorageLockTranche[] = "bioseq_cache storage";
inline constexpr char kStoreSubdirectory[] = "storage-v1";
inline constexpr std::uint32_t kInvalidSlot =
    (std::numeric_limits<std::uint32_t>::max)();

enum class BlockLedgerState : std::uint32_t {
    Empty = 0,
    Reserved,
    Stored,
    Referenced,
    Retiring,
};

struct BlockLedgerEntry {
    BlockLedgerState state;
    std::uint32_t owner_proc_number;
    int owner_pid;
    std::uint32_t manifest_slot;
    std::uint64_t sequence;
    std::int64_t created_at_us;
    std::uint64_t reserved_bytes;
    BlockReferenceV1 reference;
};

struct SharedManifestEntry {
    ManifestState state;
    CacheEntryKind kind;
    std::uint16_t block_count;
    std::uint32_t flags;
    std::uint64_t slot_sequence;
    CacheEntryId id;
    std::uint64_t logical_bytes;
    std::uint64_t stored_bytes;
    std::uint64_t row_count;
    std::int64_t created_at_us;
    std::int64_t expires_at_us;
    pg_atomic_uint64 last_access_at_us;
    pg_atomic_uint32 reader_pins;
    pg_atomic_uint32 waiter_references;
    std::array<std::uint32_t, kBlocksPerManifest> ledger_slots;
    std::array<BlockReferenceV1, kBlocksPerManifest> blocks;
};

struct StorageSharedState {
    std::uint32_t magic;
    std::uint32_t abi_version;
    std::uint64_t total_shmem_bytes;
    std::uint64_t dsa_initial_bytes;
    std::uint64_t hot_ceiling_bytes;
    std::uint64_t file_ceiling_bytes;
    std::uint64_t maximum_block_bytes;
    std::int64_t pending_timeout_us;
    std::uint32_t sweep_interval_ms;
    std::uint32_t reclaim_batch;
    std::uint32_t soft_limit_percent;
    std::uint32_t dsa_tranche_id;
    pg_atomic_uint32 bootstrap_state;
    pg_atomic_uint32 worker_lifecycle;
    pg_atomic_uint32 correctness_readiness;
    pg_atomic_uint32 last_failure;
    pg_atomic_uint64 boot_nonce;
    pg_atomic_uint64 next_block_sequence;
    pg_atomic_uint64 next_manifest_sequence;
    pg_atomic_uint64 next_file_id;
    pg_atomic_uint64 reserved_hot_bytes;
    pg_atomic_uint64 reserved_file_bytes;
    pg_atomic_uint64 manifest_count;
    pg_atomic_uint64 completed_sweeps;
    pg_atomic_uint64 last_sweep_at_us;
    pg_atomic_uint64 worker_heartbeat_at_us;
    pg_atomic_uint32 worker_pid;
    pg_atomic_uint32 worker_proc_number;
    pg_atomic_uint32 sweep_requested;
    pg_atomic_uint32 reserved_atomic;
    BlockLedgerEntry block_ledger[kBlockLedgerCapacity];
    SharedManifestEntry manifests[kManifestCapacity];
};

static_assert(std::is_standard_layout_v<StorageSharedState>);
static_assert(alignof(StorageSharedState) <= MAXIMUM_ALIGNOF);

inline StorageSharedState* g_shared = nullptr;
inline LWLock* g_storage_lock = nullptr;
inline dsa_area* g_dsa = nullptr;
inline StorageConfig g_requested_config{};
inline bool g_resources_requested = false;
inline bool g_exit_callback_registered = false;

[[nodiscard]] constexpr std::uint64_t checked_multiply_or_max(
        std::uint64_t left, std::uint64_t right) noexcept {
    if (left != 0
        && right > (std::numeric_limits<std::uint64_t>::max)() / left) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return left * right;
}

[[nodiscard]] std::uint64_t physical_memory_bytes() noexcept {
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        return checked_multiply_or_max(
            static_cast<std::uint64_t>(pages),
            static_cast<std::uint64_t>(page_size));
    }
#endif
    return 0;
}

[[nodiscard]] bool system_memory_under_pressure() noexcept {
#if defined(_SC_AVPHYS_PAGES) && defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long available_pages = ::sysconf(_SC_AVPHYS_PAGES);
    const long total_pages = ::sysconf(_SC_PHYS_PAGES);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (available_pages > 0 && total_pages > 0 && page_size > 0) {
        const std::uint64_t available = checked_multiply_or_max(
            static_cast<std::uint64_t>(available_pages),
            static_cast<std::uint64_t>(page_size));
        const std::uint64_t total = checked_multiply_or_max(
            static_cast<std::uint64_t>(total_pages),
            static_cast<std::uint64_t>(page_size));
        const std::uint64_t floor = total / 20U > (64ULL << 20)
            ? total / 20U
            : (64ULL << 20);
        return available < floor;
    }
#endif
    return false;
}

[[nodiscard]] std::uint64_t bounded_hot_ceiling(
        const StorageConfig& config) noexcept {
    const std::uint64_t physical = physical_memory_bytes();
    if (physical == 0) {
        return config.hot_ceiling_bytes;
    }
    const std::uint64_t quarter = physical / kCacheMemoryFractionDenominator
        * kCacheMemoryFractionNumerator;
    return config.hot_ceiling_bytes < quarter
        ? config.hot_ceiling_bytes
        : quarter;
}

[[nodiscard]] bool config_valid(const StorageConfig& config) noexcept {
    return config.dsa_initial_bytes >= dsa_minimum_size()
        && config.hot_ceiling_bytes != 0
        && config.file_ceiling_bytes != 0
        && config.maximum_block_bytes != 0
        && config.maximum_block_bytes <= kMaxFileCacheBytes
        && config.pending_timeout_us > 0
        && config.sweep_interval_ms >= 10
        && config.reclaim_batch != 0
        && config.reclaim_batch <= kManifestCapacity
        && config.soft_limit_percent >= 1
        && config.soft_limit_percent <= 100;
}

[[nodiscard]] std::int64_t current_time_us(std::int64_t value = 0) noexcept {
    return value != 0
        ? value
        : static_cast<std::int64_t>(GetCurrentTimestamp());
}

[[nodiscard]] std::uint32_t crc32c(
        std::span<const std::byte> bytes) noexcept {
    pg_crc32c checksum = 0;
    INIT_CRC32C(checksum);
    if (!bytes.empty()) {
        COMP_CRC32C(checksum, bytes.data(), bytes.size());
    }
    FIN_CRC32C(checksum);
    return static_cast<std::uint32_t>(checksum);
}

[[nodiscard]] bool reserve_counter(
        pg_atomic_uint64& counter,
        std::uint64_t bytes,
        std::uint64_t ceiling) noexcept {
    std::uint64_t observed = pg_atomic_read_u64(&counter);
    for (;;) {
        if (bytes > ceiling || observed > ceiling - bytes) {
            return false;
        }
        const std::uint64_t desired = observed + bytes;
        if (pg_atomic_compare_exchange_u64(&counter, &observed, desired)) {
            return true;
        }
    }
}

[[nodiscard]] std::uint32_t atomic_read(
        const pg_atomic_uint32& value) noexcept {
    return pg_atomic_read_u32(
        const_cast<pg_atomic_uint32*>(&value));
}

[[nodiscard]] std::uint64_t atomic_read(
        const pg_atomic_uint64& value) noexcept {
    return pg_atomic_read_u64(
        const_cast<pg_atomic_uint64*>(&value));
}

[[nodiscard]] bool release_counter(
        pg_atomic_uint64& counter,
        std::uint64_t bytes) noexcept {
    std::uint64_t observed = pg_atomic_read_u64(&counter);
    for (;;) {
        if (bytes > observed) {
            return false;
        }
        if (pg_atomic_compare_exchange_u64(
                &counter, &observed, observed - bytes)) {
            return true;
        }
    }
}

void remember_failure(StorageError error) noexcept {
    if (g_shared != nullptr && error != StorageError::None) {
        pg_atomic_write_u32(
            &g_shared->last_failure,
            static_cast<std::uint32_t>(error));
    }
}

[[nodiscard]] bool build_store_directory(
        char* output, std::size_t capacity) noexcept {
    if (DataDir == nullptr) {
        return false;
    }
    const int length = snprintf(
        output, capacity, "%s/bioseq_cache/%s", DataDir, kStoreSubdirectory);
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] bool safe_owned_directory(const char* path) noexcept {
    struct stat status{};
    if (::lstat(path, &status) != 0) {
        return false;
    }
    return S_ISDIR(status.st_mode)
        && status.st_uid == ::geteuid()
        && (status.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

[[nodiscard]] StorageError ensure_store_directory() noexcept {
    char base[MAXPGPATH]{};
    const int base_length = snprintf(base, sizeof(base), "%s/bioseq_cache", DataDir);
    if (base_length < 0
        || static_cast<std::size_t>(base_length) >= sizeof(base)) {
        return StorageError::PathTooLong;
    }
    if (::mkdir(base, S_IRWXU) != 0 && errno != EEXIST) {
        return StorageError::OpenFailed;
    }
    struct stat base_status{};
    if (::lstat(base, &base_status) != 0
        || !S_ISDIR(base_status.st_mode)
        || base_status.st_uid != ::geteuid()) {
        return StorageError::UnsafePath;
    }
    if (::chmod(base, S_IRWXU) != 0) {
        return StorageError::UnsafePath;
    }

    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))) {
        return StorageError::PathTooLong;
    }
    if (::mkdir(directory, S_IRWXU) != 0 && errno != EEXIST) {
        return StorageError::OpenFailed;
    }
    struct stat status{};
    if (::lstat(directory, &status) != 0
        || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid()) {
        return StorageError::UnsafePath;
    }
    if (::chmod(directory, S_IRWXU) != 0
        || !safe_owned_directory(directory)) {
        return StorageError::UnsafePath;
    }
    return StorageError::None;
}

[[nodiscard]] bool is_hex(char character) noexcept {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
}

[[nodiscard]] bool safe_cache_filename(const char* name) noexcept {
    const std::size_t length = std::strlen(name);
    if (length == 6 + 16 + 1 + 16 + 6
        && std::memcmp(name, "block-", 6) == 0
        && std::memcmp(name + length - 6, ".cache", 6) == 0) {
        for (std::size_t index = 6; index < 22; ++index) {
            if (!is_hex(name[index])) {
                return false;
            }
        }
        if (name[22] != '-') {
            return false;
        }
        for (std::size_t index = 23; index < 39; ++index) {
            if (!is_hex(name[index])) {
                return false;
            }
        }
        return true;
    }
    if (length > 16
        && std::memcmp(name, ".block-", 7) == 0
        && std::memcmp(name + length - 4, ".tmp", 4) == 0) {
        for (std::size_t index = 7; index < length - 4; ++index) {
            const char character = name[index];
            if (!is_hex(character)
                && !(character >= '0' && character <= '9')
                && character != '-') {
                return false;
            }
        }
        return true;
    }
    return false;
}

void clean_orphaned_store_files() noexcept {
    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))
        || !safe_owned_directory(directory)) {
        return;
    }
    DIR* stream = AllocateDir(directory);
    if (stream == nullptr) {
        return;
    }
    while (struct dirent* item = ReadDir(stream, directory)) {
        if (!safe_cache_filename(item->d_name)) {
            continue;
        }
        char path[MAXPGPATH]{};
        const int length = snprintf(
            path, sizeof(path), "%s/%s", directory, item->d_name);
        if (length >= 0
            && static_cast<std::size_t>(length) < sizeof(path)) {
            struct stat status{};
            if (::lstat(path, &status) == 0
                && S_ISREG(status.st_mode)
                && status.st_uid == ::geteuid()) {
                static_cast<void>(::unlink(path));
            }
        }
    }
    static_cast<void>(FreeDir(stream));
}

[[nodiscard]] bool build_file_paths(
        std::uint64_t file_id,
        char* temporary,
        std::size_t temporary_capacity,
        char* final,
        std::size_t final_capacity) noexcept {
    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))
        || g_shared == nullptr) {
        return false;
    }
    const std::uint64_t boot = pg_atomic_read_u64(&g_shared->boot_nonce);
    const int final_length = snprintf(
        final,
        final_capacity,
        "%s/block-%016llx-%016llx.cache",
        directory,
        static_cast<unsigned long long>(boot),
        static_cast<unsigned long long>(file_id));
    const int temporary_length = snprintf(
        temporary,
        temporary_capacity,
        "%s/.block-%016llx-%016llx-%d.tmp",
        directory,
        static_cast<unsigned long long>(boot),
        static_cast<unsigned long long>(file_id),
        MyProcPid);
    return final_length >= 0
        && temporary_length >= 0
        && static_cast<std::size_t>(final_length) < final_capacity
        && static_cast<std::size_t>(temporary_length) < temporary_capacity;
}

[[nodiscard]] StorageError filesystem_capacity(
        std::uint64_t& capacity,
        std::uint64_t& available) noexcept {
    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))) {
        return StorageError::PathTooLong;
    }
    struct statvfs status{};
    if (::statvfs(directory, &status) != 0) {
        return StorageError::FilesystemPressure;
    }
    const std::uint64_t fragment_size = status.f_frsize != 0
        ? static_cast<std::uint64_t>(status.f_frsize)
        : static_cast<std::uint64_t>(status.f_bsize);
    capacity = checked_multiply_or_max(
        fragment_size, static_cast<std::uint64_t>(status.f_blocks));
    available = checked_multiply_or_max(
        fragment_size, static_cast<std::uint64_t>(status.f_bavail));
    return StorageError::None;
}

[[nodiscard]] bool process_alive(
        int pid, std::uint32_t proc_number) noexcept {
    if (pid <= 0 || proc_number >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    PGPROC* process = ProcNumberGetProc(static_cast<ProcNumber>(proc_number));
    return process != nullptr && process->pid == pid;
}

void initialize_shared_state(
        StorageSharedState& shared,
        std::size_t total_bytes,
        std::uint64_t boot_nonce,
        const StorageConfig& config) noexcept {
    std::memset(&shared, 0, sizeof(shared));
    shared.magic = kStorageMagic;
    shared.abi_version = kStorageAbiVersion;
    shared.total_shmem_bytes = total_bytes;
    shared.dsa_initial_bytes = MAXALIGN(config.dsa_initial_bytes);
    shared.hot_ceiling_bytes = bounded_hot_ceiling(config);
    shared.file_ceiling_bytes = config.file_ceiling_bytes;
    shared.maximum_block_bytes = config.maximum_block_bytes;
    shared.pending_timeout_us = config.pending_timeout_us;
    shared.sweep_interval_ms = config.sweep_interval_ms;
    shared.reclaim_batch = config.reclaim_batch;
    shared.soft_limit_percent = config.soft_limit_percent;
    pg_atomic_init_u32(
        &shared.bootstrap_state,
        static_cast<std::uint32_t>(BootstrapState::Initializing));
    pg_atomic_init_u32(
        &shared.worker_lifecycle,
        static_cast<std::uint32_t>(WorkerLifecycle::Registered));
    pg_atomic_init_u32(
        &shared.correctness_readiness,
        static_cast<std::uint32_t>(CacheCorrectnessReadiness::PassthroughOnly));
    pg_atomic_init_u32(
        &shared.last_failure,
        static_cast<std::uint32_t>(StorageError::None));
    pg_atomic_init_u64(&shared.boot_nonce, boot_nonce == 0 ? 1 : boot_nonce);
    pg_atomic_init_u64(&shared.next_block_sequence, 1);
    pg_atomic_init_u64(&shared.next_manifest_sequence, 1);
    pg_atomic_init_u64(&shared.next_file_id, 1);
    pg_atomic_init_u64(&shared.reserved_hot_bytes, 0);
    pg_atomic_init_u64(&shared.reserved_file_bytes, 0);
    pg_atomic_init_u64(&shared.manifest_count, 0);
    pg_atomic_init_u64(&shared.completed_sweeps, 0);
    pg_atomic_init_u64(&shared.last_sweep_at_us, 0);
    pg_atomic_init_u64(&shared.worker_heartbeat_at_us, 0);
    pg_atomic_init_u32(&shared.worker_pid, 0);
    pg_atomic_init_u32(&shared.worker_proc_number, INVALID_PROC_NUMBER);
    pg_atomic_init_u32(&shared.sweep_requested, 0);
    pg_atomic_init_u32(&shared.reserved_atomic, 0);
    for (BlockLedgerEntry& entry : shared.block_ledger) {
        entry.state = BlockLedgerState::Empty;
        entry.manifest_slot = kInvalidSlot;
    }
    for (SharedManifestEntry& entry : shared.manifests) {
        entry.state = ManifestState::Empty;
        pg_atomic_init_u64(&entry.last_access_at_us, 0);
        pg_atomic_init_u32(&entry.reader_pins, 0);
        pg_atomic_init_u32(&entry.waiter_references, 0);
        entry.ledger_slots.fill(kInvalidSlot);
    }
}

void storage_backend_exit(int, Datum) {
    if (g_dsa != nullptr) {
        dsa_detach(g_dsa);
        if (g_shared != nullptr) {
            void* place = reinterpret_cast<char*>(g_shared)
                + MAXALIGN(sizeof(StorageSharedState));
            dsa_release_in_place(place);
        }
        g_dsa = nullptr;
    }
}

[[nodiscard]] bool same_id(
        const CacheEntryId& left,
        const CacheEntryId& right) noexcept {
    return left == right;
}

[[nodiscard]] bool handle_matches(
        const SharedManifestEntry& entry,
        const ManifestHandle& handle) noexcept {
    return handle.slot < kManifestCapacity
        && entry.state != ManifestState::Empty
        && entry.slot_sequence == handle.slot_sequence
        && same_id(entry.id, handle.id);
}

[[nodiscard]] StorageError allocate_ledger_slot(
        std::uint32_t& slot,
        std::uint64_t& sequence,
        std::uint64_t bytes) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::uint32_t index = 0; index < kBlockLedgerCapacity; ++index) {
        BlockLedgerEntry& entry = g_shared->block_ledger[index];
        if (entry.state != BlockLedgerState::Empty) {
            continue;
        }
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_block_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(
                &g_shared->next_block_sequence, 1);
        }
        entry = BlockLedgerEntry{};
        entry.state = BlockLedgerState::Reserved;
        entry.owner_pid = MyProcPid;
        entry.owner_proc_number = static_cast<std::uint32_t>(MyProcNumber);
        entry.manifest_slot = kInvalidSlot;
        entry.sequence = sequence;
        entry.created_at_us = current_time_us();
        entry.reserved_bytes = bytes;
        slot = index;
        LWLockRelease(g_storage_lock);
        return StorageError::None;
    }
    LWLockRelease(g_storage_lock);
    return StorageError::LedgerFull;
}

void clear_unreserved_ledger_slot(
        std::uint32_t slot, std::uint64_t sequence) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence == sequence
        && entry.state == BlockLedgerState::Reserved
        && entry.reference.length == 0) {
        entry = BlockLedgerEntry{};
        entry.manifest_slot = kInvalidSlot;
    }
    LWLockRelease(g_storage_lock);
}

[[nodiscard]] StorageError publish_stored_ledger_block(
        std::uint32_t slot,
        std::uint64_t sequence,
        const BlockReferenceV1& reference) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || entry.state != BlockLedgerState::Reserved) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    entry.reference = reference;
    pg_write_barrier();
    entry.state = BlockLedgerState::Stored;
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

[[nodiscard]] StorageError write_all_at(
        int descriptor,
        std::span<const std::byte> bytes) noexcept {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = pg_pwrite(
            descriptor,
            bytes.data() + written,
            bytes.size() - written,
            static_cast<off_t>(written));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return StorageError::WriteFailed;
        }
        written += static_cast<std::size_t>(count);
    }
    return StorageError::None;
}

[[nodiscard]] StorageError sync_directory(const char* directory) noexcept {
#if defined(O_DIRECTORY)
    const int descriptor = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    const int descriptor = ::open(directory, O_RDONLY | O_CLOEXEC);
#endif
    if (descriptor < 0) {
        return StorageError::SyncFailed;
    }
    const bool success = pg_fsync(descriptor) == 0;
    const int saved_errno = errno;
    static_cast<void>(::close(descriptor));
    errno = saved_errno;
    return success ? StorageError::None : StorageError::SyncFailed;
}

[[nodiscard]] StorageError store_file_block(
        std::span<const std::byte> bytes,
        std::uint64_t file_id,
        BlockReferenceV1& reference) noexcept {
    char temporary[MAXPGPATH]{};
    char final[MAXPGPATH]{};
    if (!build_file_paths(
            file_id, temporary, sizeof(temporary), final, sizeof(final))) {
        return StorageError::PathTooLong;
    }
    int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(temporary, flags, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return StorageError::OpenFailed;
    }
    StorageError error = StorageError::None;
    if (::fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
        error = StorageError::UnsafePath;
    }
    struct stat status{};
    if (error == StorageError::None
        && (::fstat(descriptor, &status) != 0
            || !S_ISREG(status.st_mode)
            || status.st_uid != ::geteuid()
            || status.st_nlink != 1
            || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0)) {
        error = StorageError::UnsafePath;
    }
    if (error == StorageError::None) {
        error = write_all_at(descriptor, bytes);
    }
    if (error == StorageError::None && pg_fsync(descriptor) != 0) {
        error = StorageError::SyncFailed;
    }
    const int close_status = ::close(descriptor);
    if (error == StorageError::None && close_status != 0) {
        error = StorageError::SyncFailed;
    }
    if (error != StorageError::None) {
        static_cast<void>(::unlink(temporary));
        return error;
    }
    if (durable_rename(temporary, final, WARNING) != 0) {
        static_cast<void>(::unlink(temporary));
        return StorageError::RenameFailed;
    }
    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))) {
        static_cast<void>(::unlink(final));
        return StorageError::PathTooLong;
    }
    if ((error = sync_directory(directory)) != StorageError::None) {
        static_cast<void>(::unlink(final));
        return error;
    }
    reference.tier = BlockBackingTierV1::File;
    reference.handle_or_file_id = file_id;
    reference.offset = 0;
    reference.length = bytes.size();
    reference.crc32c = crc32c(bytes);
    return StorageError::None;
}

[[nodiscard]] StorageError read_all_at(
        int descriptor,
        std::uint64_t offset,
        std::span<std::byte> destination) noexcept {
    std::size_t read = 0;
    while (read < destination.size()) {
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())
            - read) {
            return StorageError::InvalidArgument;
        }
        const ssize_t count = pg_pread(
            descriptor,
            destination.data() + read,
            destination.size() - read,
            static_cast<off_t>(offset + read));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return StorageError::ReadFailed;
        }
        if (count == 0) {
            return StorageError::ShortRead;
        }
        read += static_cast<std::size_t>(count);
    }
    return StorageError::None;
}

[[nodiscard]] StorageError read_file_block(
        const BlockReferenceV1& reference,
        std::span<std::byte> destination) noexcept {
    char temporary[MAXPGPATH]{};
    char final[MAXPGPATH]{};
    if (!build_file_paths(
            reference.handle_or_file_id,
            temporary,
            sizeof(temporary),
            final,
            sizeof(final))) {
        return StorageError::PathTooLong;
    }
    int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(final, flags);
    if (descriptor < 0) {
        return errno == ENOENT ? StorageError::NotFound : StorageError::OpenFailed;
    }
    struct stat status{};
    StorageError error = StorageError::None;
    const std::uint64_t end = reference.offset <=
            (std::numeric_limits<std::uint64_t>::max)() - reference.length
        ? reference.offset + reference.length
        : (std::numeric_limits<std::uint64_t>::max)();
    if (::fstat(descriptor, &status) != 0
        || !S_ISREG(status.st_mode)
        || status.st_uid != ::geteuid()
        || status.st_nlink != 1
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0
        || status.st_size < 0
        || static_cast<std::uint64_t>(status.st_size) < end) {
        error = StorageError::UnsafePath;
    } else {
        error = read_all_at(descriptor, reference.offset, destination);
    }
    const int saved_errno = errno;
    static_cast<void>(::close(descriptor));
    errno = saved_errno;
    return error;
}

[[nodiscard]] StorageError release_block_slot(
        std::uint32_t slot,
        std::uint64_t sequence,
        JanitorSweepResult* result = nullptr) noexcept {
    if (slot >= kBlockLedgerCapacity) {
        return StorageError::InvalidArgument;
    }
    BlockReferenceV1 reference{};
    std::uint64_t reserved_bytes = 0;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || (entry.state != BlockLedgerState::Stored
            && entry.state != BlockLedgerState::Retiring)) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    entry.state = BlockLedgerState::Retiring;
    reference = entry.reference;
    reserved_bytes = entry.reserved_bytes;

    /* Physical release is serialized so no second janitor can double-free. */
    if (reference.tier == BlockBackingTierV1::Dsa
        && DsaPointerIsValid(static_cast<dsa_pointer>(reference.handle_or_file_id))) {
        dsa_free(g_dsa, static_cast<dsa_pointer>(reference.handle_or_file_id));
        static_cast<void>(release_counter(g_shared->reserved_hot_bytes, reserved_bytes));
        if (result != nullptr) {
            result->reclaimed_hot_bytes += reserved_bytes;
        }
    } else if (reference.tier == BlockBackingTierV1::File) {
        char temporary[MAXPGPATH]{};
        char final[MAXPGPATH]{};
        if (build_file_paths(
                reference.handle_or_file_id,
                temporary,
                sizeof(temporary),
                final,
                sizeof(final))) {
            static_cast<void>(::unlink(temporary));
            static_cast<void>(::unlink(final));
        }
        static_cast<void>(release_counter(g_shared->reserved_file_bytes, reserved_bytes));
        if (result != nullptr) {
            result->reclaimed_file_bytes += reserved_bytes;
        }
    }
    entry = BlockLedgerEntry{};
    entry.manifest_slot = kInvalidSlot;
    LWLockRelease(g_storage_lock);
    if (result != nullptr) {
        ++result->reclaimed_blocks;
    }
    return StorageError::None;
}

[[nodiscard]] std::uint64_t soft_limit(
        std::uint64_t ceiling, std::uint32_t percent) noexcept {
    return ceiling / 100U * percent;
}

[[nodiscard]] bool manifest_reclaimable(
        const SharedManifestEntry& entry) noexcept {
    return entry.state != ManifestState::Empty
        && atomic_read(entry.reader_pins) == 0
        && atomic_read(entry.waiter_references) == 0;
}

[[nodiscard]] StorageError reclaim_manifest_slot(
        std::uint32_t slot,
        std::uint64_t sequence,
        JanitorSweepResult& result) noexcept {
    if (slot >= kManifestCapacity) {
        return StorageError::InvalidArgument;
    }
    std::array<std::uint32_t, kBlocksPerManifest> ledger_slots{};
    std::array<std::uint64_t, kBlocksPerManifest> ledger_sequences{};
    std::uint32_t block_count = 0;

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest = g_shared->manifests[slot];
    if (manifest.slot_sequence != sequence
        || !manifest_reclaimable(manifest)) {
        LWLockRelease(g_storage_lock);
        return StorageError::Pinned;
    }
    manifest.state = ManifestState::Retiring;
    block_count = manifest.block_count;
    for (std::uint32_t index = 0; index < block_count; ++index) {
        ledger_slots[index] = manifest.ledger_slots[index];
        if (ledger_slots[index] < kBlockLedgerCapacity) {
            BlockLedgerEntry& block = g_shared->block_ledger[ledger_slots[index]];
            ledger_sequences[index] = block.sequence;
            if (block.state == BlockLedgerState::Referenced
                && block.manifest_slot == slot) {
                block.state = BlockLedgerState::Stored;
                block.manifest_slot = kInvalidSlot;
            }
        }
    }
    LWLockRelease(g_storage_lock);

    for (std::uint32_t index = 0; index < block_count; ++index) {
        if (ledger_slots[index] < kBlockLedgerCapacity) {
            static_cast<void>(release_block_slot(
                ledger_slots[index], ledger_sequences[index], &result));
        }
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& current = g_shared->manifests[slot];
    if (current.slot_sequence == sequence
        && current.state == ManifestState::Retiring
        && pg_atomic_read_u32(&current.reader_pins) == 0
        && pg_atomic_read_u32(&current.waiter_references) == 0) {
        current = SharedManifestEntry{};
        current.state = ManifestState::Empty;
        current.ledger_slots.fill(kInvalidSlot);
        pg_atomic_init_u64(&current.last_access_at_us, 0);
        pg_atomic_init_u32(&current.reader_pins, 0);
        pg_atomic_init_u32(&current.waiter_references, 0);
        static_cast<void>(pg_atomic_fetch_sub_u64(&g_shared->manifest_count, 1));
    }
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

} // namespace

std::size_t storage_shared_memory_size(const StorageConfig& config) noexcept {
    const std::uint64_t initial = config.dsa_initial_bytes < dsa_minimum_size()
        ? dsa_minimum_size()
        : config.dsa_initial_bytes;
    const std::uint64_t header = MAXALIGN(sizeof(StorageSharedState));
    const std::uint64_t area = MAXALIGN(initial);
    if (header > (std::numeric_limits<std::size_t>::max)() - area) {
        return 0;
    }
    return static_cast<std::size_t>(header + area);
}

void request_storage_resources(const StorageConfig& config) {
    g_requested_config = config;
    g_resources_requested = config_valid(config);
    const std::size_t bytes = storage_shared_memory_size(config);
    if (!g_resources_requested || bytes == 0) {
        return;
    }
    RequestAddinShmemSpace(MAXALIGN(bytes));
    RequestNamedLWLockTranche(kStorageLockTranche, 1);
}

bool startup_storage(
        std::uint64_t boot_nonce,
        const StorageConfig& config) {
    const std::size_t bytes = storage_shared_memory_size(config);
    if (!config_valid(config) || bytes == 0) {
        return false;
    }
    bool found = false;
    g_shared = static_cast<StorageSharedState*>(
        ShmemInitStruct(kStorageSharedName, bytes, &found));
    g_storage_lock = &GetNamedLWLockTranche(kStorageLockTranche)[0].lock;
    if (found) {
        const bool matches = g_shared->magic == kStorageMagic
            && g_shared->abi_version == kStorageAbiVersion
            && g_shared->total_shmem_bytes == bytes
            && g_shared->dsa_initial_bytes == MAXALIGN(config.dsa_initial_bytes);
        if (!matches) {
            remember_failure(StorageError::BootstrapFailed);
            return false;
        }
        LWLockRegisterTranche(
            static_cast<int>(g_shared->dsa_tranche_id),
            "bioseq_cache storage dsa");
        return pg_atomic_read_u32(&g_shared->bootstrap_state)
            == static_cast<std::uint32_t>(BootstrapState::Ready);
    }

    initialize_shared_state(*g_shared, bytes, boot_nonce, config);
    const StorageError directory_error = ensure_store_directory();
    if (directory_error != StorageError::None) {
        remember_failure(directory_error);
        pg_atomic_write_u32(
            &g_shared->bootstrap_state,
            static_cast<std::uint32_t>(BootstrapState::Failed));
        return false;
    }
    clean_orphaned_store_files();
    void* place = reinterpret_cast<char*>(g_shared)
        + MAXALIGN(sizeof(StorageSharedState));
    g_shared->dsa_tranche_id = static_cast<std::uint32_t>(LWLockNewTrancheId());
    LWLockRegisterTranche(
        static_cast<int>(g_shared->dsa_tranche_id),
        "bioseq_cache storage dsa");
    dsa_area* area = dsa_create_in_place(
        place,
        static_cast<std::size_t>(g_shared->dsa_initial_bytes),
        static_cast<int>(g_shared->dsa_tranche_id),
        nullptr);
    if (area == nullptr) {
        remember_failure(StorageError::DsaAllocationFailed);
        pg_atomic_write_u32(
            &g_shared->bootstrap_state,
            static_cast<std::uint32_t>(BootstrapState::Failed));
        return false;
    }
    dsa_pin(area);
    const std::uint64_t size_limit = g_shared->hot_ceiling_bytes <=
            (std::numeric_limits<std::size_t>::max)() - g_shared->dsa_initial_bytes
        ? g_shared->hot_ceiling_bytes + g_shared->dsa_initial_bytes
        : (std::numeric_limits<std::size_t>::max)();
    dsa_set_size_limit(area, static_cast<std::size_t>(size_limit));
    dsa_detach(area);
    pg_write_barrier();
    pg_atomic_write_u32(
        &g_shared->bootstrap_state,
        static_cast<std::uint32_t>(BootstrapState::Ready));
    return true;
}

bool attach_storage() {
    if (g_shared == nullptr) {
        const std::size_t bytes = storage_shared_memory_size(g_requested_config);
        bool found = false;
        g_shared = static_cast<StorageSharedState*>(
            ShmemInitStruct(kStorageSharedName, bytes, &found));
        if (!found) {
            g_shared = nullptr;
            return false;
        }
        g_storage_lock = &GetNamedLWLockTranche(kStorageLockTranche)[0].lock;
    }
    if (g_shared->magic != kStorageMagic
        || g_shared->abi_version != kStorageAbiVersion
        || pg_atomic_read_u32(&g_shared->bootstrap_state)
            != static_cast<std::uint32_t>(BootstrapState::Ready)) {
        return false;
    }
    LWLockRegisterTranche(
        static_cast<int>(g_shared->dsa_tranche_id),
        "bioseq_cache storage dsa");
    if (g_dsa == nullptr) {
        void* place = reinterpret_cast<char*>(g_shared)
            + MAXALIGN(sizeof(StorageSharedState));
        MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
        g_dsa = dsa_attach_in_place(place, nullptr);
        if (g_dsa != nullptr) {
            dsa_pin_mapping(g_dsa);
        }
        MemoryContextSwitchTo(previous);
        if (g_dsa == nullptr) {
            remember_failure(StorageError::DsaAllocationFailed);
            return false;
        }
        if (!g_exit_callback_registered) {
            before_shmem_exit(storage_backend_exit, Datum{0});
            g_exit_callback_registered = true;
        }
    }
    return true;
}

bool storage_ready() noexcept {
    return g_shared != nullptr
        && g_dsa != nullptr
        && g_shared->magic == kStorageMagic
        && g_shared->abi_version == kStorageAbiVersion
        && pg_atomic_read_u32(&g_shared->bootstrap_state)
            == static_cast<std::uint32_t>(BootstrapState::Ready);
}

StoreBlockResult store_block(std::span<const std::byte> bytes) {
    StoreBlockResult result{};
    if (!attach_storage()) {
        result.error = StorageError::NotPreloaded;
        return result;
    }
    if (bytes.empty()) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    if (bytes.size() > g_shared->maximum_block_bytes) {
        result.error = StorageError::BlockTooLarge;
        return result;
    }

    std::uint32_t ledger_slot = kInvalidSlot;
    std::uint64_t sequence = 0;
    result.error = allocate_ledger_slot(
        ledger_slot, sequence, static_cast<std::uint64_t>(bytes.size()));
    if (result.error != StorageError::None) {
        remember_failure(result.error);
        return result;
    }

    BlockReferenceV1 reference{};
    bool hot_reserved = false;
    if (!system_memory_under_pressure()) {
        hot_reserved = reserve_counter(
            g_shared->reserved_hot_bytes,
            bytes.size(),
            g_shared->hot_ceiling_bytes);
    }
    if (hot_reserved) {
        const dsa_pointer allocation = dsa_allocate_extended(
            g_dsa,
            bytes.size(),
            DSA_ALLOC_NO_OOM);
        if (DsaPointerIsValid(allocation)) {
            void* address = dsa_get_address(g_dsa, allocation);
            std::memcpy(address, bytes.data(), bytes.size());
            pg_write_barrier();
            reference.tier = BlockBackingTierV1::Dsa;
            reference.handle_or_file_id = static_cast<std::uint64_t>(allocation);
            reference.offset = 0;
            reference.length = bytes.size();
            reference.crc32c = crc32c(bytes);
            result.error = publish_stored_ledger_block(
                ledger_slot, sequence, reference);
            if (result.error == StorageError::None) {
                result.token = StoredBlockToken{
                    .ledger_slot = ledger_slot,
                    .sequence = sequence,
                    .reference = reference,
                };
                return result;
            }
            dsa_free(g_dsa, allocation);
        }
        static_cast<void>(release_counter(
            g_shared->reserved_hot_bytes, bytes.size()));
    }

    std::uint64_t filesystem_bytes = 0;
    std::uint64_t filesystem_available = 0;
    result.error = filesystem_capacity(
        filesystem_bytes, filesystem_available);
    if (result.error != StorageError::None) {
        clear_unreserved_ledger_slot(ledger_slot, sequence);
        remember_failure(result.error);
        return result;
    }
    const std::uint64_t policy_ceiling = FileAdmissionPolicy::ceiling_bytes(
        filesystem_bytes);
    const std::uint64_t ceiling = g_shared->file_ceiling_bytes < policy_ceiling
        ? g_shared->file_ceiling_bytes
        : policy_ceiling;
    const std::uint64_t observed = pg_atomic_read_u64(
        &g_shared->reserved_file_bytes);
    if (!FileAdmissionPolicy::can_reserve(
            observed,
            bytes.size(),
            filesystem_bytes,
            filesystem_available)
        || !reserve_counter(
            g_shared->reserved_file_bytes, bytes.size(), ceiling)) {
        clear_unreserved_ledger_slot(ledger_slot, sequence);
        result.error = system_memory_under_pressure()
            ? StorageError::MemoryPressure
            : StorageError::FileTierFull;
        remember_failure(result.error);
        return result;
    }
    std::uint64_t file_id = pg_atomic_fetch_add_u64(
        &g_shared->next_file_id, 1);
    if (file_id == 0) {
        file_id = pg_atomic_fetch_add_u64(&g_shared->next_file_id, 1);
    }
    result.error = store_file_block(bytes, file_id, reference);
    if (result.error != StorageError::None) {
        static_cast<void>(release_counter(
            g_shared->reserved_file_bytes, bytes.size()));
        clear_unreserved_ledger_slot(ledger_slot, sequence);
        remember_failure(result.error);
        return result;
    }
    result.error = publish_stored_ledger_block(
        ledger_slot, sequence, reference);
    if (result.error != StorageError::None) {
        char temporary[MAXPGPATH]{};
        char final[MAXPGPATH]{};
        if (build_file_paths(
                file_id, temporary, sizeof(temporary), final, sizeof(final))) {
            static_cast<void>(::unlink(final));
        }
        static_cast<void>(release_counter(
            g_shared->reserved_file_bytes, bytes.size()));
        clear_unreserved_ledger_slot(ledger_slot, sequence);
        remember_failure(result.error);
        return result;
    }
    result.token = StoredBlockToken{
        .ledger_slot = ledger_slot,
        .sequence = sequence,
        .reference = reference,
    };
    return result;
}

StorageError discard_unpublished_block(const StoredBlockToken& token) {
    if (!storage_ready() || !token) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const BlockLedgerEntry& entry = g_shared->block_ledger[token.ledger_slot];
    const bool valid = entry.sequence == token.sequence
        && entry.state == BlockLedgerState::Stored
        && entry.manifest_slot == kInvalidSlot;
    LWLockRelease(g_storage_lock);
    return valid
        ? release_block_slot(token.ledger_slot, token.sequence)
        : StorageError::StaleToken;
}

PublishManifestResult publish_manifest(
        const ManifestDescriptor& descriptor,
        std::span<const StoredBlockToken> blocks) {
    PublishManifestResult result{};
    if (!storage_ready()) {
        result.error = StorageError::NotPreloaded;
        return result;
    }
    if (descriptor.id.boot_nonce == 0
        || descriptor.id.generation == 0
        || descriptor.id.claim_sequence == 0
        || descriptor.reserved != 0
        || descriptor.flags != 0
        || blocks.empty()
        || blocks.size() > kBlocksPerManifest) {
        result.error = StorageError::InvalidArgument;
        return result;
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t free_slot = kInvalidSlot;
    for (std::uint32_t index = 0; index < kManifestCapacity; ++index) {
        SharedManifestEntry& manifest = g_shared->manifests[index];
        if (manifest.state == ManifestState::Empty && free_slot == kInvalidSlot) {
            free_slot = index;
        } else if (manifest.state != ManifestState::Empty
            && same_id(manifest.id, descriptor.id)) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::DuplicateGeneration;
            return result;
        }
    }
    if (free_slot == kInvalidSlot) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::ManifestFull;
        return result;
    }
    std::uint64_t stored_bytes = 0;
    for (const StoredBlockToken& token : blocks) {
        if (!token || token.ledger_slot >= kBlockLedgerCapacity) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::InvalidArgument;
            return result;
        }
        const BlockLedgerEntry& block = g_shared->block_ledger[token.ledger_slot];
        if (block.sequence != token.sequence
            || block.state != BlockLedgerState::Stored
            || block.manifest_slot != kInvalidSlot
            || std::memcmp(
                &block.reference,
                &token.reference,
                sizeof(BlockReferenceV1)) != 0
            || stored_bytes > (std::numeric_limits<std::uint64_t>::max)()
                - block.reference.length) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::StaleToken;
            return result;
        }
        stored_bytes += block.reference.length;
    }

    SharedManifestEntry& manifest = g_shared->manifests[free_slot];
    manifest = SharedManifestEntry{};
    manifest.state = ManifestState::Complete;
    manifest.kind = descriptor.kind;
    manifest.block_count = static_cast<std::uint16_t>(blocks.size());
    manifest.flags = descriptor.flags;
    manifest.slot_sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_manifest_sequence, 1);
    if (manifest.slot_sequence == 0) {
        manifest.slot_sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_manifest_sequence, 1);
    }
    manifest.id = descriptor.id;
    manifest.logical_bytes = descriptor.logical_bytes;
    manifest.stored_bytes = stored_bytes;
    manifest.row_count = descriptor.row_count;
    manifest.created_at_us = current_time_us(descriptor.created_at_us);
    manifest.expires_at_us = descriptor.expires_at_us;
    manifest.ledger_slots.fill(kInvalidSlot);
    pg_atomic_init_u64(&manifest.last_access_at_us, manifest.created_at_us);
    pg_atomic_init_u32(&manifest.reader_pins, 0);
    pg_atomic_init_u32(
        &manifest.waiter_references,
        descriptor.initial_waiter_references);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const StoredBlockToken& token = blocks[index];
        manifest.ledger_slots[index] = token.ledger_slot;
        manifest.blocks[index] = token.reference;
        BlockLedgerEntry& block = g_shared->block_ledger[token.ledger_slot];
        block.state = BlockLedgerState::Referenced;
        block.manifest_slot = free_slot;
    }
    pg_write_barrier();
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->manifest_count, 1));
    result.handle = ManifestHandle{
        .slot = free_slot,
        .slot_sequence = manifest.slot_sequence,
        .id = manifest.id,
    };
    LWLockRelease(g_storage_lock);
    return result;
}

PinManifestResult pin_manifest(
        const CacheEntryId& id,
        std::int64_t now_us) {
    PinManifestResult result{};
    if (!storage_ready()) {
        result.error = StorageError::NotPreloaded;
        return result;
    }
    const std::int64_t now = current_time_us(now_us);
    LWLockAcquire(g_storage_lock, LW_SHARED);
    for (std::uint32_t index = 0; index < kManifestCapacity; ++index) {
        SharedManifestEntry& manifest = g_shared->manifests[index];
        if (manifest.state != ManifestState::Complete
            || !same_id(manifest.id, id)) {
            continue;
        }
        if (manifest.expires_at_us != 0 && now >= manifest.expires_at_us) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::NotFound;
            return result;
        }
        static_cast<void>(pg_atomic_fetch_add_u32(&manifest.reader_pins, 1));
        pg_atomic_write_u64(&manifest.last_access_at_us, now);
        result.pin.handle = ManifestHandle{
            .slot = index,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        result.pin.kind = manifest.kind;
        result.pin.flags = manifest.flags;
        result.pin.logical_bytes = manifest.logical_bytes;
        result.pin.row_count = manifest.row_count;
        result.pin.block_count = manifest.block_count;
        for (std::uint32_t block = 0; block < manifest.block_count; ++block) {
            result.pin.blocks[block] = manifest.blocks[block];
        }
        LWLockRelease(g_storage_lock);
        return result;
    }
    LWLockRelease(g_storage_lock);
    result.error = StorageError::NotFound;
    return result;
}

StorageError register_manifest_waiter(
        const ManifestHandle& handle,
        std::int64_t now_us) {
    if (!storage_ready() || !handle) {
        return StorageError::InvalidArgument;
    }
    const std::int64_t now = current_time_us(now_us);
    LWLockAcquire(g_storage_lock, LW_SHARED);
    SharedManifestEntry& manifest = g_shared->manifests[handle.slot];
    if (!handle_matches(manifest, handle)
        || manifest.state != ManifestState::Complete
        || (manifest.expires_at_us != 0 && now >= manifest.expires_at_us)) {
        LWLockRelease(g_storage_lock);
        return StorageError::NotFound;
    }
    const std::uint32_t waiters = pg_atomic_read_u32(
        &manifest.waiter_references);
    if (waiters == (std::numeric_limits<std::uint32_t>::max)()) {
        LWLockRelease(g_storage_lock);
        return StorageError::InvalidState;
    }
    static_cast<void>(pg_atomic_fetch_add_u32(
        &manifest.waiter_references, 1));
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

PinManifestResult convert_waiter_to_pin(const ManifestHandle& handle) {
    PinManifestResult result{};
    if (!storage_ready() || !handle) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    SharedManifestEntry& manifest = g_shared->manifests[handle.slot];
    if (!handle_matches(manifest, handle)
        || manifest.state != ManifestState::Complete
        || pg_atomic_read_u32(&manifest.waiter_references) == 0) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::NotFound;
        return result;
    }
    static_cast<void>(pg_atomic_fetch_sub_u32(
        &manifest.waiter_references, 1));
    static_cast<void>(pg_atomic_fetch_add_u32(&manifest.reader_pins, 1));
    pg_atomic_write_u64(&manifest.last_access_at_us, current_time_us());
    result.pin.handle = handle;
    result.pin.kind = manifest.kind;
    result.pin.flags = manifest.flags;
    result.pin.logical_bytes = manifest.logical_bytes;
    result.pin.row_count = manifest.row_count;
    result.pin.block_count = manifest.block_count;
    for (std::uint32_t index = 0; index < manifest.block_count; ++index) {
        result.pin.blocks[index] = manifest.blocks[index];
    }
    LWLockRelease(g_storage_lock);
    return result;
}

StorageError unpin_manifest(const ManifestPin& pin) noexcept {
    if (!storage_ready() || !pin || pin.handle.slot >= kManifestCapacity) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    SharedManifestEntry& manifest = g_shared->manifests[pin.handle.slot];
    if (!handle_matches(manifest, pin.handle)
        || pg_atomic_read_u32(&manifest.reader_pins) == 0) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    static_cast<void>(pg_atomic_fetch_sub_u32(&manifest.reader_pins, 1));
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

StorageError unregister_manifest_waiter(
        const ManifestHandle& handle) noexcept {
    if (!storage_ready() || !handle) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    SharedManifestEntry& manifest = g_shared->manifests[handle.slot];
    if (!handle_matches(manifest, handle)
        || pg_atomic_read_u32(&manifest.waiter_references) == 0) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    static_cast<void>(pg_atomic_fetch_sub_u32(
        &manifest.waiter_references, 1));
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

StorageError read_pinned_block(
        const ManifestPin& pin,
        std::size_t block_index,
        std::span<std::byte> destination) {
    if (!storage_ready() || !pin
        || block_index >= pin.block_count
        || destination.size() != pin.blocks[block_index].length) {
        return StorageError::InvalidArgument;
    }
    const BlockReferenceV1& reference = pin.blocks[block_index];
    StorageError error = StorageError::None;
    if (reference.tier == BlockBackingTierV1::Dsa) {
        if (!DsaPointerIsValid(
                static_cast<dsa_pointer>(reference.handle_or_file_id))) {
            return StorageError::InvalidArgument;
        }
        const auto* source = static_cast<const std::byte*>(
            dsa_get_address(
                g_dsa,
                static_cast<dsa_pointer>(reference.handle_or_file_id)));
        if (source == nullptr) {
            return StorageError::NotFound;
        }
        std::memcpy(
            destination.data(),
            source + reference.offset,
            destination.size());
    } else if (reference.tier == BlockBackingTierV1::File) {
        error = read_file_block(reference, destination);
    } else {
        return StorageError::InvalidArgument;
    }
    if (error == StorageError::None
        && crc32c(destination) != reference.crc32c) {
        error = StorageError::ChecksumMismatch;
    }
    if (error != StorageError::None) {
        remember_failure(error);
    }
    return error;
}

StorageError retire_manifest(const ManifestHandle& handle) noexcept {
    if (!storage_ready() || !handle) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest = g_shared->manifests[handle.slot];
    if (!handle_matches(manifest, handle)) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    manifest.state = ManifestState::Retiring;
    LWLockRelease(g_storage_lock);
    request_janitor_sweep(false);
    return StorageError::None;
}

JanitorSweepResult janitor_sweep(
        std::int64_t now_us,
        bool aggressive) {
    JanitorSweepResult result{};
    if (!attach_storage()) {
        return result;
    }
    const std::int64_t now = current_time_us(now_us);
    const std::uint32_t requested = pg_atomic_exchange_u32(
        &g_shared->sweep_requested, 0);
    aggressive = aggressive || requested > 1;

    /* First reclaim abandoned, never-published producer blocks. */
    for (std::uint32_t index = 0;
         index < kBlockLedgerCapacity
            && result.abandoned_blocks < g_shared->reclaim_batch;
         ++index) {
        std::uint64_t sequence = 0;
        bool reclaim = false;
        LWLockAcquire(g_storage_lock, LW_SHARED);
        const BlockLedgerEntry& block = g_shared->block_ledger[index];
        if (block.state == BlockLedgerState::Reserved
            || (block.state == BlockLedgerState::Stored
                && block.manifest_slot == kInvalidSlot)) {
            const bool abandoned = !process_alive(
                block.owner_pid, block.owner_proc_number);
            const bool timed_out = block.created_at_us > 0
                && now > block.created_at_us
                && now - block.created_at_us >= g_shared->pending_timeout_us;
            reclaim = abandoned || timed_out;
            sequence = block.sequence;
        }
        LWLockRelease(g_storage_lock);
        if (!reclaim) {
            continue;
        }
        LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
        BlockLedgerEntry& current_block = g_shared->block_ledger[index];
        if (current_block.sequence == sequence
            && current_block.state == BlockLedgerState::Reserved
            && current_block.reference.length == 0) {
            current_block = BlockLedgerEntry{};
            current_block.manifest_slot = kInvalidSlot;
            ++result.abandoned_blocks;
            LWLockRelease(g_storage_lock);
            continue;
        }
        LWLockRelease(g_storage_lock);
        if (release_block_slot(index, sequence, &result)
            == StorageError::None) {
            ++result.abandoned_blocks;
        }
    }

    /* Expiry and explicit retirement always precede pressure-based LRU. */
    for (std::uint32_t index = 0;
         index < kManifestCapacity
            && result.expired_manifests < g_shared->reclaim_batch;
         ++index) {
        std::uint64_t sequence = 0;
        bool expired = false;
        LWLockAcquire(g_storage_lock, LW_SHARED);
        const SharedManifestEntry& manifest = g_shared->manifests[index];
        if (manifest_reclaimable(manifest)) {
            expired = manifest.state == ManifestState::Retiring
                || (manifest.expires_at_us != 0
                    && now >= manifest.expires_at_us);
            sequence = manifest.slot_sequence;
        }
        LWLockRelease(g_storage_lock);
        if (expired
            && reclaim_manifest_slot(index, sequence, result)
                == StorageError::None) {
            ++result.expired_manifests;
        }
    }

    const std::uint64_t hot_soft = soft_limit(
        g_shared->hot_ceiling_bytes, g_shared->soft_limit_percent);
    const std::uint64_t file_soft = soft_limit(
        g_shared->file_ceiling_bytes, g_shared->soft_limit_percent);
    auto pressure_remains = [&]() noexcept {
        return aggressive
            || system_memory_under_pressure()
            || pg_atomic_read_u64(&g_shared->reserved_hot_bytes) > hot_soft
            || pg_atomic_read_u64(&g_shared->reserved_file_bytes) > file_soft;
    };
    while (pressure_remains()
        && result.lru_manifests < g_shared->reclaim_batch) {
        std::uint32_t oldest_slot = kInvalidSlot;
        std::uint64_t oldest_sequence = 0;
        std::uint64_t oldest_access =
            (std::numeric_limits<std::uint64_t>::max)();
        LWLockAcquire(g_storage_lock, LW_SHARED);
        for (std::uint32_t index = 0; index < kManifestCapacity; ++index) {
            const SharedManifestEntry& manifest = g_shared->manifests[index];
            if (manifest.state != ManifestState::Complete
                || !manifest_reclaimable(manifest)) {
                continue;
            }
            const std::uint64_t access = atomic_read(
                manifest.last_access_at_us);
            if (access < oldest_access) {
                oldest_access = access;
                oldest_slot = index;
                oldest_sequence = manifest.slot_sequence;
            }
        }
        LWLockRelease(g_storage_lock);
        if (oldest_slot == kInvalidSlot) {
            break;
        }
        if (reclaim_manifest_slot(
                oldest_slot, oldest_sequence, result) == StorageError::None) {
            ++result.lru_manifests;
        } else {
            break;
        }
    }
    if (g_dsa != nullptr && result.reclaimed_hot_bytes != 0) {
        dsa_trim(g_dsa);
    }
    pg_atomic_write_u64(&g_shared->last_sweep_at_us, now);
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->completed_sweeps, 1));
    return result;
}

void request_janitor_sweep(bool aggressive) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    const std::uint32_t desired = aggressive ? 2U : 1U;
    std::uint32_t observed = pg_atomic_read_u32(&g_shared->sweep_requested);
    while (observed < desired
        && !pg_atomic_compare_exchange_u32(
            &g_shared->sweep_requested, &observed, desired)) {
    }
    const std::uint32_t proc_number = pg_atomic_read_u32(
        &g_shared->worker_proc_number);
    const int pid = static_cast<int>(pg_atomic_read_u32(&g_shared->worker_pid));
    if (process_alive(pid, proc_number)) {
        PGPROC* worker = ProcNumberGetProc(static_cast<ProcNumber>(proc_number));
        if (worker != nullptr) {
            SetLatch(&worker->procLatch);
        }
    }
}

bool publish_correctness_readiness(
        CacheCorrectnessReadiness readiness) noexcept {
    if (g_shared == nullptr || !known_readiness(readiness)) {
        return false;
    }
    pg_atomic_write_u32(
        &g_shared->correctness_readiness,
        static_cast<std::uint32_t>(readiness));
    return true;
}

void publish_worker_lifecycle(
        WorkerLifecycle lifecycle,
        int worker_pid) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    pg_atomic_write_u32(
        &g_shared->worker_lifecycle,
        static_cast<std::uint32_t>(lifecycle));
    pg_atomic_write_u32(
        &g_shared->worker_pid,
        worker_pid > 0 ? static_cast<std::uint32_t>(worker_pid) : 0);
    pg_atomic_write_u32(
        &g_shared->worker_proc_number,
        worker_pid > 0
            ? static_cast<std::uint32_t>(MyProcNumber)
            : static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
    publish_worker_heartbeat();
}

void publish_worker_heartbeat(std::int64_t now_us) noexcept {
    if (g_shared != nullptr) {
        pg_atomic_write_u64(
            &g_shared->worker_heartbeat_at_us,
            static_cast<std::uint64_t>(current_time_us(now_us)));
    }
}

StorageRuntimeSnapshot runtime_snapshot() noexcept {
    StorageRuntimeSnapshot snapshot{};
    snapshot.dsa_attached = g_dsa != nullptr;
    if (g_shared == nullptr) {
        return snapshot;
    }
    snapshot.bootstrap_state = static_cast<BootstrapState>(
        pg_atomic_read_u32(&g_shared->bootstrap_state));
    snapshot.bootstrap_ready =
        snapshot.bootstrap_state == BootstrapState::Ready;
    snapshot.worker_lifecycle = static_cast<WorkerLifecycle>(
        pg_atomic_read_u32(&g_shared->worker_lifecycle));
    snapshot.correctness_readiness = static_cast<CacheCorrectnessReadiness>(
        pg_atomic_read_u32(&g_shared->correctness_readiness));
    snapshot.last_failure = static_cast<StorageError>(
        pg_atomic_read_u32(&g_shared->last_failure));
    snapshot.boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce);
    snapshot.reserved_hot_bytes = pg_atomic_read_u64(
        &g_shared->reserved_hot_bytes);
    snapshot.reserved_file_bytes = pg_atomic_read_u64(
        &g_shared->reserved_file_bytes);
    snapshot.hot_ceiling_bytes = g_shared->hot_ceiling_bytes;
    snapshot.file_ceiling_bytes = g_shared->file_ceiling_bytes;
    snapshot.manifest_count = pg_atomic_read_u64(&g_shared->manifest_count);
    snapshot.completed_sweeps = pg_atomic_read_u64(
        &g_shared->completed_sweeps);
    snapshot.last_sweep_at_us = static_cast<std::int64_t>(
        pg_atomic_read_u64(&g_shared->last_sweep_at_us));
    snapshot.worker_heartbeat_at_us = static_cast<std::int64_t>(
        pg_atomic_read_u64(&g_shared->worker_heartbeat_at_us));
    snapshot.worker_pid = static_cast<int>(
        pg_atomic_read_u32(&g_shared->worker_pid));
    return snapshot;
}

std::uint32_t janitor_interval_ms() noexcept {
    return g_shared != nullptr
        ? g_shared->sweep_interval_ms
        : g_requested_config.sweep_interval_ms;
}

} // namespace bioseq::cache::pg::storage
