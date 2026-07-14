/*-------------------------------------------------------------------------
 *
 * bioseq_cache PostgreSQL adapter.
 *
 * The sibling extension deliberately contains no sequence or Bioseq table-AM
 * knowledge.  Product integrations arrive through bioseq.cache.provider_api.
 * This primary unit owns the PostgreSQL hooks, outer CustomScan, shared
 * bootstrap, GUCs, and SQL administration surface.  Storage and registry
 * behavior is enabled incrementally behind the same fail-open node.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/params.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "port/pg_crc32c.h"

PG_MODULE_MAGIC;
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <array>
#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.cache.pg;

import bioseq.cache.core;
import bioseq.cache.pg.provider_registry;

namespace bioseq::cache::pg {

inline constexpr std::uint32_t kSharedMagic = 0x42535143U; /* BSQC */
inline constexpr std::uint32_t kSharedAbiVersion = 1;
inline constexpr char kSharedName[] = "bioseq_cache shared state v1";
inline constexpr char kRegistryTrancheName[] = "bioseq_cache registry";
inline constexpr std::size_t kRegistryCapacity = 64;
inline constexpr std::size_t kMaximumKeyBytes = 64U * 1024U;
inline constexpr std::uint64_t kMaximumStoredResultBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRelationEpochCapacity = 1024;
inline constexpr std::size_t kMaximumRelationDependencies = 256;

/** PostgreSQL-backed checksums for the shared generic column-store format. */
struct PostgreSqlColumnChecksumPolicy {
    [[nodiscard]] std::expected<std::uint32_t, ChecksumError> block_crc32c(
            std::span<const std::byte> bytes) const noexcept {
        pg_crc32c checksum = 0;
        INIT_CRC32C(checksum);
        if (!bytes.empty()) {
            COMP_CRC32C(checksum, bytes.data(), bytes.size());
        }
        FIN_CRC32C(checksum);
        return static_cast<std::uint32_t>(checksum);
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError> sha256(
            std::span<const std::byte> bytes) const noexcept {
        pg_cryptohash_ctx* context = pg_cryptohash_create(PG_SHA256);
        if (context == nullptr) {
            return std::unexpected(ChecksumError::Unavailable);
        }
        CacheDigest256 digest{};
        const bool success = pg_cryptohash_init(context) >= 0
            && (bytes.empty()
                || pg_cryptohash_update(
                    context,
                    reinterpret_cast<const std::uint8_t*>(bytes.data()),
                    bytes.size()) >= 0)
            && pg_cryptohash_final(
                context, digest.bytes.data(), digest.bytes.size()) >= 0;
        pg_cryptohash_free(context);
        if (!success) {
            return std::unexpected(ChecksumError::Failed);
        }
        return digest;
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError>
    manifest_sha256(
            std::span<const CacheDigest256> digests) const noexcept {
        pg_cryptohash_ctx* context = pg_cryptohash_create(PG_SHA256);
        if (context == nullptr) {
            return std::unexpected(ChecksumError::Unavailable);
        }
        bool success = pg_cryptohash_init(context) >= 0;
        for (const CacheDigest256& digest : digests) {
            success = success
                && pg_cryptohash_update(
                    context, digest.bytes.data(), digest.bytes.size()) >= 0;
        }
        CacheDigest256 result{};
        success = success
            && pg_cryptohash_final(
                context, result.bytes.data(), result.bytes.size()) >= 0;
        pg_cryptohash_free(context);
        if (!success) {
            return std::unexpected(ChecksumError::Failed);
        }
        return result;
    }
};

/** User-visible cache optimization modes. */
enum class QueryCacheMode : int {
    Disabled = 0,
    Same = 1,
    Reuse = 2,
};

/** Shared counters exposed by bioseq_cache.stats(). */
struct CacheSharedStats {
    pg_atomic_uint64 produced;
    pg_atomic_uint64 completed_hits;
    pg_atomic_uint64 inflight_waits;
    pg_atomic_uint64 reuse_hits;
    pg_atomic_uint64 fallbacks;
    pg_atomic_uint64 invalidations;
    pg_atomic_uint64 evictions;
    pg_atomic_uint64 logical_bytes;
    pg_atomic_uint64 stored_bytes;
    pg_atomic_uint64 planner_build_generation;
};

#if defined(BIOSEQ_CACHE_TESTING)
inline constexpr std::size_t kInjectedCoverageCapacity = 256;
inline constexpr std::size_t kInjectedDigestBytes = 64;

enum class TestPausePoint : std::uint32_t {
    AfterBuildingPublish = 0,
    AfterWaiterRegistered = 1,
    BeforeTerminalPublish = 2,
    Count = 3,
};

struct CacheTestControl {
    pg_atomic_uint32 armed_mask;
    pg_atomic_uint32 paused_mask;
    pg_atomic_uint32 released_mask;
    pg_atomic_uint32 fail_next_write;
    ConditionVariable changed;
};

struct InjectedCoverage {
    bool used;
    bool evictable;
    std::uint16_t domain_size;
    std::uint16_t predicate_size;
    std::uint16_t provider_digest_size;
    std::uint16_t forced_digest_size;
    Oid database_oid;
    Oid current_role_oid;
    Oid session_user_oid;
    std::uint32_t security_context_flags;
    std::uint64_t generation;
    unsigned char domain[kInjectedDigestBytes];
    unsigned char predicate[kInjectedDigestBytes];
    unsigned char provider_digest[kInjectedDigestBytes];
    unsigned char forced_digest[kInjectedDigestBytes];
};
#endif

enum class SharedEntryState : std::uint32_t {
    Empty = 0,
    Producing = 1,
    Complete = 2,
    Failed = 3,
    Retired = 4,
};

/** One bounded registry record. Full key bytes make hash collisions harmless. */
struct CacheEntry {
    SharedEntryState state;
    std::uint32_t key_size;
    std::uint64_t generation;
    std::uint64_t key_hash;
    std::uint64_t database_epoch;
    Oid database_oid;
    Oid current_role_oid;
    Oid session_user_oid;
    int producer_pid;
    ProcNumber producer_proc_number;
    std::uint32_t waiter_count;
    std::uint32_t reader_count;
    TimestampTz created_at;
    TimestampTz completed_at;
    TimestampTz expires_at;
    std::uint64_t row_count;
    std::uint64_t logical_bytes;
    std::uint64_t stored_bytes;
    std::uint64_t hits;
    std::uint16_t relation_count;
    std::uint16_t reserved;
    Oid relation_oids[kMaximumRelationDependencies];
    char final_path[MAXPGPATH];
    char display_sql[4096];
    ConditionVariable changed;
    unsigned char key[kMaximumKeyBytes];
};

/** Collision-checked bounded dependency epoch; protected by registry lock. */
struct RelationEpochSlot {
    Oid database_oid;
    Oid relation_oid;
    std::uint64_t committed_epoch;
    bool used;
};

/** Bounded postmaster-lifetime header. Variable cache state is added via DSA. */
struct CacheSharedState {
    std::uint32_t magic;
    std::uint32_t shared_abi_version;
    pg_atomic_uint32 ready;
    pg_atomic_uint32 bootstrap_failure;
    pg_atomic_uint64 boot_nonce;
    pg_atomic_uint64 database_write_epoch;
    pg_atomic_uint64 coarse_dependency_epoch;
    pg_atomic_uint32 last_committed_write_xid;
    pg_atomic_uint32 database_transition_count;
    ConditionVariable database_changed;
    CacheSharedStats stats;
#if defined(BIOSEQ_CACHE_TESTING)
    CacheTestControl test_control;
    pg_atomic_uint64 next_injected_generation;
    InjectedCoverage injected_coverage[kInjectedCoverageCapacity];
#endif
    RelationEpochSlot relation_epochs[kRelationEpochCapacity];
    CacheEntry entries[kRegistryCapacity];
};

static_assert(std::is_standard_layout_v<CacheSharedState>);
static_assert(alignof(CacheSharedState) <= MAXIMUM_ALIGNOF);

inline CacheSharedState* g_shared = nullptr;
inline LWLock* g_registry_lock = nullptr;
inline shmem_request_hook_type g_previous_shmem_request_hook = nullptr;
inline shmem_startup_hook_type g_previous_shmem_startup_hook = nullptr;
inline planner_hook_type g_previous_planner_hook = nullptr;
inline create_upper_paths_hook_type g_previous_create_upper_paths_hook = nullptr;
inline ExecutorStart_hook_type g_previous_executor_start_hook = nullptr;
inline ProcessUtility_hook_type g_previous_process_utility_hook = nullptr;
inline bool g_preload_requested = false;
inline bool g_gucs_registered = false;
inline bool g_hooks_installed = false;
inline bool g_callbacks_registered = false;
inline int g_query_cache_mode = static_cast<int>(QueryCacheMode::Disabled);
inline int g_query_cache_ttl_ms = 60 * 60 * 1000;
inline bool g_internal_planning = false;

/** Backend-local dirty state follows savepoint commit and rollback exactly. */
struct DirtyFrame {
    DirtyFrame* next;
    SubTransactionId subtransaction_id;
    bool dirty;
    bool coarse;
    std::uint16_t relation_count;
    Oid relation_oids[kMaximumRelationDependencies];
};

inline DirtyFrame* g_dirty_frames = nullptr;
inline bool g_database_transition_published = false;
inline TransactionId g_database_transition_xid = InvalidTransactionId;
inline bool g_force_coarse_transition = false;

/** Per-planner invocation frame; PostgreSQL backends are single-threaded. */
struct PlannerFrame {
    PlannerFrame* previous = nullptr;
    Query* delegated_query = nullptr;
    char* canonical_query = nullptr;
    char* display_sql = nullptr;
    List* referenced_parameter_ids = NIL;
    List* referenced_relation_oids = NIL;
    bool eligible = false;
};

inline PlannerFrame* g_planner_frame = nullptr;

/** Initialize all shared atomics exactly once. */
void initialize_shared_state(CacheSharedState& shared) noexcept {
    std::memset(&shared, 0, sizeof(shared));
    shared.magic = kSharedMagic;
    shared.shared_abi_version = kSharedAbiVersion;
    pg_atomic_init_u32(&shared.ready, 1);
    pg_atomic_init_u32(&shared.bootstrap_failure, 0);
    const std::uint64_t nonce =
        static_cast<std::uint64_t>(GetCurrentTimestamp())
        ^ (static_cast<std::uint64_t>(MyProcPid) << 32U);
    pg_atomic_init_u64(&shared.boot_nonce, nonce == 0 ? 1 : nonce);
    pg_atomic_init_u64(&shared.database_write_epoch, 1);
    pg_atomic_init_u64(&shared.coarse_dependency_epoch, 1);
    pg_atomic_init_u32(
        &shared.last_committed_write_xid, InvalidTransactionId);
    pg_atomic_init_u32(&shared.database_transition_count, 0);
    ConditionVariableInit(&shared.database_changed);
    pg_atomic_init_u64(&shared.stats.produced, 0);
    pg_atomic_init_u64(&shared.stats.completed_hits, 0);
    pg_atomic_init_u64(&shared.stats.inflight_waits, 0);
    pg_atomic_init_u64(&shared.stats.reuse_hits, 0);
    pg_atomic_init_u64(&shared.stats.fallbacks, 0);
    pg_atomic_init_u64(&shared.stats.invalidations, 0);
    pg_atomic_init_u64(&shared.stats.evictions, 0);
    pg_atomic_init_u64(&shared.stats.logical_bytes, 0);
    pg_atomic_init_u64(&shared.stats.stored_bytes, 0);
    pg_atomic_init_u64(&shared.stats.planner_build_generation, 0);
#if defined(BIOSEQ_CACHE_TESTING)
    pg_atomic_init_u32(&shared.test_control.armed_mask, 0);
    pg_atomic_init_u32(&shared.test_control.paused_mask, 0);
    pg_atomic_init_u32(&shared.test_control.released_mask, 0);
    pg_atomic_init_u32(&shared.test_control.fail_next_write, 0);
    ConditionVariableInit(&shared.test_control.changed);
    pg_atomic_init_u64(&shared.next_injected_generation, 1);
#endif
    for (CacheEntry& entry : shared.entries) {
        entry.state = SharedEntryState::Empty;
        ConditionVariableInit(&entry.changed);
    }
}

/** Locate or create a savepoint-aware dirty frame in backend-lifetime memory. */
[[nodiscard]] DirtyFrame* dirty_frame(SubTransactionId subtransaction_id) {
    for (DirtyFrame* frame = g_dirty_frames; frame != nullptr;
         frame = frame->next) {
        if (frame->subtransaction_id == subtransaction_id) {
            return frame;
        }
    }
    MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);
    auto* frame = static_cast<DirtyFrame*>(palloc0(sizeof(DirtyFrame)));
    MemoryContextSwitchTo(old_context);
    frame->subtransaction_id = subtransaction_id;
    frame->next = g_dirty_frames;
    g_dirty_frames = frame;
    return frame;
}

/** Remember a possible result-changing write until its transaction outcome. */
void mark_current_transaction_dirty() {
    if (IsTransactionState()) {
        DirtyFrame* frame = dirty_frame(GetCurrentSubTransactionId());
        frame->dirty = true;
        frame->coarse = true;
    }
}

/** Record one precise relation dependency, falling back when the set is full. */
void mark_current_transaction_relation_dirty(Oid relation_oid) {
    if (!IsTransactionState() || !OidIsValid(relation_oid)) {
        return;
    }
    DirtyFrame* frame = dirty_frame(GetCurrentSubTransactionId());
    frame->dirty = true;
    for (std::uint16_t index = 0; index < frame->relation_count; ++index) {
        if (frame->relation_oids[index] == relation_oid) {
            return;
        }
    }
    if (frame->relation_count >= kMaximumRelationDependencies) {
        frame->coarse = true;
        return;
    }
    frame->relation_oids[frame->relation_count++] = relation_oid;
}

/** Provider write observations join the same savepoint-aware epoch ledger. */
void observe_provider_relation_write(
        std::uint32_t database_oid, std::uint32_t relation_oid,
        const provider_registry::PhysicalIdentityV1*) noexcept {
    if (database_oid == static_cast<std::uint32_t>(MyDatabaseId)) {
        mark_current_transaction_relation_dirty(
            static_cast<Oid>(relation_oid));
    }
}

/** True when at least one non-aborted savepoint performed a write. */
[[nodiscard]] bool transaction_is_dirty() noexcept {
    for (const DirtyFrame* frame = g_dirty_frames; frame != nullptr;
         frame = frame->next) {
        if (frame->dirty) {
            return true;
        }
    }
    return false;
}

/** Drop all backend-local transaction frames without retaining stale pointers. */
void clear_dirty_frames() noexcept {
    while (g_dirty_frames != nullptr) {
        DirtyFrame* frame = g_dirty_frames;
        g_dirty_frames = frame->next;
        pfree(frame);
    }
}

/** Remove one subtransaction frame and optionally merge it into its parent. */
void finish_dirty_subtransaction(
        SubTransactionId child_id, SubTransactionId parent_id,
        bool committed) {
    DirtyFrame** link = &g_dirty_frames;
    while (*link != nullptr && (*link)->subtransaction_id != child_id) {
        link = &(*link)->next;
    }
    if (*link == nullptr) {
        return;
    }
    DirtyFrame* child = *link;
    *link = child->next;
    if (committed && child->dirty) {
        DirtyFrame* parent = dirty_frame(parent_id);
        parent->dirty = true;
        parent->coarse = parent->coarse || child->coarse;
        for (std::uint16_t index = 0;
             index < child->relation_count; ++index) {
            const Oid relation_oid = child->relation_oids[index];
            bool present = false;
            for (std::uint16_t parent_index = 0;
                 parent_index < parent->relation_count; ++parent_index) {
                present = present
                    || parent->relation_oids[parent_index] == relation_oid;
            }
            if (present) {
                continue;
            }
            if (parent->relation_count >= kMaximumRelationDependencies) {
                parent->coarse = true;
                break;
            }
            parent->relation_oids[parent->relation_count++] = relation_oid;
        }
    }
    pfree(child);
}

[[nodiscard]] constexpr std::size_t relation_epoch_start(
        Oid database_oid, Oid relation_oid) noexcept {
    const std::uint64_t mixed =
        static_cast<std::uint64_t>(database_oid) * 0x9e3779b185ebca87ULL
        ^ static_cast<std::uint64_t>(relation_oid) * 0xc2b2ae3d27d4eb4fULL;
    return static_cast<std::size_t>(mixed % kRelationEpochCapacity);
}

/** Read the current precise epoch; unseen relations begin at epoch one. */
[[nodiscard]] std::uint64_t relation_dependency_epoch(Oid relation_oid) noexcept {
    if (g_shared == nullptr || !OidIsValid(relation_oid)) {
        return 0;
    }
    std::uint64_t epoch = 1;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    const std::size_t start = relation_epoch_start(MyDatabaseId, relation_oid);
    for (std::size_t probe = 0; probe < kRelationEpochCapacity; ++probe) {
        const RelationEpochSlot& slot =
            g_shared->relation_epochs[(start + probe) % kRelationEpochCapacity];
        if (!slot.used) {
            break;
        }
        if (slot.database_oid == MyDatabaseId
            && slot.relation_oid == relation_oid) {
            epoch = slot.committed_epoch;
            break;
        }
    }
    LWLockRelease(g_registry_lock);
    return epoch;
}

/** Advance one precise dependency under the registry lock; false means full. */
[[nodiscard]] bool bump_relation_epoch_locked(Oid relation_oid) noexcept {
    const std::size_t start = relation_epoch_start(MyDatabaseId, relation_oid);
    for (std::size_t probe = 0; probe < kRelationEpochCapacity; ++probe) {
        RelationEpochSlot& slot =
            g_shared->relation_epochs[(start + probe) % kRelationEpochCapacity];
        if (!slot.used) {
            slot.used = true;
            slot.database_oid = MyDatabaseId;
            slot.relation_oid = relation_oid;
            slot.committed_epoch = 2;
            return true;
        }
        if (slot.database_oid == MyDatabaseId
            && slot.relation_oid == relation_oid) {
            ++slot.committed_epoch;
            if (slot.committed_epoch == 0) {
                slot.committed_epoch = 1;
            }
            return true;
        }
    }
    return false;
}

/** Publish savepoint-filtered precise dependencies, returning coarse fallback. */
[[nodiscard]] bool publish_dirty_dependency_epochs() noexcept {
    bool coarse = g_force_coarse_transition;
    Oid published[kMaximumRelationDependencies]{};
    std::size_t published_count = 0;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (const DirtyFrame* frame = g_dirty_frames;
         frame != nullptr; frame = frame->next) {
        coarse = coarse || frame->coarse;
        for (std::uint16_t index = 0;
             index < frame->relation_count; ++index) {
            const Oid relation_oid = frame->relation_oids[index];
            bool duplicate = false;
            for (std::size_t prior = 0; prior < published_count; ++prior) {
                duplicate = duplicate || published[prior] == relation_oid;
            }
            if (duplicate) {
                continue;
            }
            if (published_count >= kMaximumRelationDependencies
                || !bump_relation_epoch_locked(relation_oid)) {
                coarse = true;
                continue;
            }
            published[published_count++] = relation_oid;
        }
    }
    LWLockRelease(g_registry_lock);
    return coarse;
}

/** Announce a commit transition so concurrent captures cannot race visibility. */
void begin_database_transition() noexcept {
    if (g_database_transition_published || g_shared == nullptr) {
        return;
    }
    pg_atomic_fetch_add_u32(&g_shared->database_transition_count, 1);
    g_database_transition_xid = GetTopTransactionIdIfAny();
    g_database_transition_published = true;
    ConditionVariableBroadcast(&g_shared->database_changed);
}

/** Complete or abandon an announced transition in visibility-safe order. */
void finish_database_transition(bool committed) noexcept {
    if (!g_database_transition_published || g_shared == nullptr) {
        g_database_transition_published = false;
        g_database_transition_xid = InvalidTransactionId;
        g_force_coarse_transition = false;
        return;
    }
    if (committed) {
        if (publish_dirty_dependency_epochs()) {
            pg_atomic_fetch_add_u64(
                &g_shared->coarse_dependency_epoch, 1);
        }
        if (TransactionIdIsValid(g_database_transition_xid)) {
            pg_atomic_write_u32(
                &g_shared->last_committed_write_xid,
                g_database_transition_xid);
        }
        pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
        pg_atomic_fetch_add_u64(&g_shared->stats.invalidations, 1);
    }
    pg_atomic_fetch_sub_u32(&g_shared->database_transition_count, 1);
    g_database_transition_published = false;
    g_database_transition_xid = InvalidTransactionId;
    g_force_coarse_transition = false;
    ConditionVariableBroadcast(&g_shared->database_changed);
}

/** Publish write epochs only for successful top-level outcomes. */
void cache_transaction_callback(XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
        case XACT_EVENT_PRE_PREPARE:
            if (transaction_is_dirty()) {
                begin_database_transition();
            }
            break;
        case XACT_EVENT_COMMIT:
        case XACT_EVENT_PREPARE:
            finish_database_transition(true);
            clear_dirty_frames();
            break;
        case XACT_EVENT_ABORT:
            finish_database_transition(false);
            clear_dirty_frames();
            break;
        case XACT_EVENT_PARALLEL_COMMIT:
        case XACT_EVENT_PARALLEL_ABORT:
            /* Only the leader publishes the database-wide transition. */
            clear_dirty_frames();
            break;
        case XACT_EVENT_PARALLEL_PRE_COMMIT:
            break;
    }
}

/** Savepoint rollback discards dirty state; release merges it upward. */
void cache_subtransaction_callback(
        SubXactEvent event, SubTransactionId child_id,
        SubTransactionId parent_id, void*) {
    switch (event) {
        case SUBXACT_EVENT_COMMIT_SUB:
            finish_dirty_subtransaction(child_id, parent_id, true);
            break;
        case SUBXACT_EVENT_ABORT_SUB:
            finish_dirty_subtransaction(child_id, parent_id, false);
            break;
        case SUBXACT_EVENT_START_SUB:
        case SUBXACT_EVENT_PRE_COMMIT_SUB:
            break;
    }
}

/** Request bounded shared memory and one named registry lock. */
void cache_shmem_request() {
    if (g_previous_shmem_request_hook != nullptr) {
        g_previous_shmem_request_hook();
    }
    RequestAddinShmemSpace(MAXALIGN(sizeof(CacheSharedState)));
    RequestNamedLWLockTranche(kRegistryTrancheName, 1);
}

/** Build the postmaster-private cache directory path. */
void cache_store_directory(char* output, std::size_t capacity) noexcept {
    snprintf(output, capacity, "%s/bioseq_cache", DataDir);
}

/** A new postmaster never trusts file artifacts from an older boot nonce. */
void clean_orphaned_cache_files() {
    char directory[MAXPGPATH]{};
    cache_store_directory(directory, sizeof(directory));
    if (MakePGDirectory(directory) != 0 && errno != EEXIST) {
        ereport(WARNING,
            (errcode_for_file_access(),
             errmsg("could not create bioseq_cache directory \"%s\": %m",
                    directory)));
        return;
    }
    DIR* stream = AllocateDir(directory);
    if (stream == nullptr) {
        return;
    }
    while (struct dirent* item = ReadDir(stream, directory)) {
        if (item->d_name[0] == '.') {
            continue;
        }
        char path[MAXPGPATH]{};
        const int length = snprintf(
            path, sizeof(path), "%s/%s", directory, item->d_name);
        if (length >= 0 && static_cast<std::size_t>(length) < sizeof(path)) {
            static_cast<void>(::unlink(path));
        }
    }
    static_cast<void>(FreeDir(stream));
}

/** Attach or initialize the postmaster-lifetime cache header. */
void cache_shmem_startup() {
    if (g_previous_shmem_startup_hook != nullptr) {
        g_previous_shmem_startup_hook();
    }

    bool found = false;
    g_shared = static_cast<CacheSharedState*>(
        ShmemInitStruct(kSharedName, sizeof(CacheSharedState), &found));
    if (!found) {
        clean_orphaned_cache_files();
        initialize_shared_state(*g_shared);
    }
    g_registry_lock = &GetNamedLWLockTranche(kRegistryTrancheName)[0].lock;
}

/** True only when preload installed and the shared ABI is usable. */
[[nodiscard]] bool shared_cache_ready() noexcept {
    return g_preload_requested
        && g_shared != nullptr
        && g_shared->magic == kSharedMagic
        && g_shared->shared_abi_version == kSharedAbiVersion
        && pg_atomic_read_u32(&g_shared->ready) != 0
        && pg_atomic_read_u32(&g_shared->bootstrap_failure) == 0;
}

struct VisibilityTicket {
    std::uint64_t database_epoch = 0;
    TransactionId last_committed_write_xid = InvalidTransactionId;
    bool valid = false;
};

/** Bind the committed-write epoch to the statement's already-active snapshot. */
[[nodiscard]] VisibilityTicket mint_visibility_ticket() noexcept {
    VisibilityTicket ticket{};
    if (!shared_cache_ready()) {
        return ticket;
    }
    Snapshot snapshot = GetActiveSnapshot();
    if (snapshot == nullptr || !IsMVCCSnapshot(snapshot)) {
        return ticket;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
            return ticket;
        }
        const std::uint64_t before = pg_atomic_read_u64(
            &g_shared->database_write_epoch);
        pg_read_barrier();
        const TransactionId last_write = static_cast<TransactionId>(
            pg_atomic_read_u32(&g_shared->last_committed_write_xid));
        pg_read_barrier();
        const std::uint64_t after = pg_atomic_read_u64(
            &g_shared->database_write_epoch);
        if (before != after
            || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
            continue;
        }
        if (TransactionIdIsValid(last_write)
            && XidInMVCCSnapshot(last_write, snapshot)) {
            return ticket;
        }
        ticket.database_epoch = after;
        ticket.last_committed_write_xid = last_write;
        ticket.valid = true;
        return ticket;
    }
    return ticket;
}

/** Recheck the ticket after dependency-key construction closes commit races. */
[[nodiscard]] bool visibility_ticket_still_bound(
        const VisibilityTicket& ticket) noexcept {
    if (!ticket.valid || g_shared == nullptr
        || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
        return false;
    }
    pg_read_barrier();
    return pg_atomic_read_u64(&g_shared->database_write_epoch)
        == ticket.database_epoch;
}

/** Reject non-disabled use when the library was not preloaded. */
bool check_query_cache_mode(int* new_value, void**, GucSource) {
    if (*new_value == static_cast<int>(QueryCacheMode::Disabled)) {
        return true;
    }
    if (process_shared_preload_libraries_in_progress || g_preload_requested) {
        return true;
    }
    GUC_check_errmsg("bioseq_cache shared memory is not registered");
    GUC_check_errhint(
        "Add bioseq_cache to shared_preload_libraries and restart PostgreSQL.");
    return false;
}

/** Invalidate generic prepared plans on every real mode transition. */
void assign_query_cache_mode(int new_value, void*) {
    static int previous_value = static_cast<int>(QueryCacheMode::Disabled);
    const bool changed = previous_value != new_value;
    previous_value = new_value;
    if (changed && IsTransactionState()) {
        ResetPlanCache();
    }
}

/** Register exactly the two production GUCs requested by DOC0371. */
void register_cache_gucs() {
    if (g_gucs_registered) {
        return;
    }
    static const config_enum_entry mode_options[] = {
        {"disabled", static_cast<int>(QueryCacheMode::Disabled), false},
        {"same", static_cast<int>(QueryCacheMode::Same), false},
        {"reuse", static_cast<int>(QueryCacheMode::Reuse), false},
        {nullptr, 0, false},
    };
    DefineCustomEnumVariable(
        "bioseq.query_cache_optimisation_mode",
        "Controls exact request locking and reusable query-result fragments.",
        nullptr,
        &g_query_cache_mode,
        static_cast<int>(QueryCacheMode::Disabled),
        mode_options,
        PGC_USERSET,
        0,
        check_query_cache_mode,
        assign_query_cache_mode,
        nullptr);
    DefineCustomIntVariable(
        "bioseq.query_cache_ttl",
        "Maximum age of a completed bioseq_cache result.",
        nullptr,
        &g_query_cache_ttl_ms,
        60 * 60 * 1000,
        0,
        INT_MAX,
        PGC_USERSET,
        GUC_UNIT_MS,
        nullptr,
        nullptr,
        nullptr);
    MarkGUCPrefixReserved("bioseq");
    g_gucs_registered = true;
}

/** Reject execution state that cannot be represented by the exact key/epoch. */
bool query_contains_unsafe_state_walker(Node* node, void*) {
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, Var)) {
        const Var* variable = reinterpret_cast<const Var*>(node);
        return variable->varattno < 0;
    }
    if (IsA(node, SQLValueFunction)
        || IsA(node, NextValueExpr)
        || IsA(node, CurrentOfExpr)) {
        return true;
    }
    if (IsA(node, RangeTblEntry)) {
        const auto* range = reinterpret_cast<const RangeTblEntry*>(node);
        if (range->tablesample != nullptr || range->securityQuals != NIL) {
            return true;
        }
        if (range->rtekind == RTE_NAMEDTUPLESTORE
            || range->rtekind == RTE_TABLEFUNC) {
            return true;
        }
        if (range->rtekind == RTE_RELATION) {
            const char relation_kind = get_rel_relkind(range->relid);
            if (get_rel_persistence(range->relid) == RELPERSISTENCE_TEMP
                || (relation_kind != RELKIND_RELATION
                    && relation_kind != RELKIND_PARTITIONED_TABLE
                    && relation_kind != RELKIND_MATVIEW)) {
                return true;
            }
        }
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            query_contains_unsafe_state_walker,
            nullptr,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(
        node, query_contains_unsafe_state_walker, nullptr);
}

/** Collect only PARAM_EXTERN ids the analyzed query can actually evaluate. */
bool collect_external_parameter_walker(Node* node, void* context) {
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, Param)) {
        const auto* parameter = reinterpret_cast<const Param*>(node);
        if (parameter->paramkind == PARAM_EXTERN && parameter->paramid > 0) {
            auto** parameters = static_cast<Bitmapset**>(context);
            *parameters = bms_add_member(
                *parameters, static_cast<int>(parameter->paramid));
        }
        return false;
    }
    if (IsA(node, RangeTblEntry)) {
        // query_tree_walker supplies RTEs to the callback when
        // QTW_EXAMINE_RTES_BEFORE is set; it owns walking their expressions.
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            collect_external_parameter_walker,
            context,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(
        node, collect_external_parameter_walker, context);
}

[[nodiscard]] List* collect_external_parameter_ids(Query& query) {
    Bitmapset* parameters = nullptr;
    static_cast<void>(query_tree_walker(
        &query, collect_external_parameter_walker, &parameters,
        QTW_EXAMINE_RTES_BEFORE));
    List* result = NIL;
    int parameter_id = -1;
    while ((parameter_id = bms_next_member(parameters, parameter_id)) >= 0) {
        result = lappend(result, makeInteger(parameter_id));
    }
    bms_free(parameters);
    return result;
}

struct RelationCollector {
    List* relation_oids = NIL;
    bool overflow = false;
};

bool collect_relation_oid_walker(Node* node, void* context) {
    if (node == nullptr) {
        return false;
    }
    auto* collector = static_cast<RelationCollector*>(context);
    if (IsA(node, RangeTblEntry)) {
        const auto* range = reinterpret_cast<const RangeTblEntry*>(node);
        if (range->rtekind == RTE_RELATION
            && OidIsValid(range->relid)
            && !list_member_oid(collector->relation_oids, range->relid)) {
            if (list_length(collector->relation_oids)
                >= static_cast<int>(kMaximumRelationDependencies)) {
                collector->overflow = true;
                return true;
            }
            collector->relation_oids = lappend_oid(
                collector->relation_oids, range->relid);
        }
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            collect_relation_oid_walker,
            context,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(node, collect_relation_oid_walker, context);
}

[[nodiscard]] List* collect_relation_oids(Query& query, bool& overflow) {
    RelationCollector collector{};
    static_cast<void>(query_tree_walker(
        &query,
        collect_relation_oid_walker,
        &collector,
        QTW_EXAMINE_RTES_BEFORE));
    overflow = collector.overflow;
    return collector.relation_oids;
}

/** Conservative v1 exact-cache eligibility before path decoration. */
[[nodiscard]] bool query_is_exact_cache_eligible(const Query& query) {
    if (query.commandType != CMD_SELECT
        || query.hasModifyingCTE
        || query.rowMarks != NIL
        || query.hasTargetSRFs
        || query.hasRowSecurity
        || query.hasForUpdate) {
        return false;
    }
    if (contain_mutable_functions(reinterpret_cast<Node*>(
            const_cast<Query*>(&query)))) {
        return false;
    }
    if (query_tree_walker(
            const_cast<Query*>(&query),
            query_contains_unsafe_state_walker,
            nullptr,
            QTW_EXAMINE_RTES_BEFORE)) {
        return false;
    }
    return true;
}

/** Canonical analyzed-tree bytes; source locations are omitted by nodeToString. */
[[nodiscard]] char* canonicalize_query(const Query& query) {
    Query* copy = static_cast<Query*>(
        copyObject(const_cast<Query*>(&query)));
    copy->queryId = 0;
    copy->stmt_location = -1;
    copy->stmt_len = 0;
    return nodeToString(copy);
}

/** Extract one bounded display statement for diagnostics only. */
[[nodiscard]] char* copy_display_sql(const char* query_string) {
    if (query_string == nullptr) {
        return pstrdup("<query text unavailable>");
    }
    constexpr std::size_t kMaxDisplayBytes = 1U << 20U;
    const std::size_t length = strnlen(query_string, kMaxDisplayBytes + 1U);
    if (length <= kMaxDisplayBytes) {
        return pnstrdup(query_string, length);
    }
    constexpr char suffix[] = " /* truncated */";
    char* result = static_cast<char*>(palloc(kMaxDisplayBytes + sizeof(suffix)));
    std::memcpy(result, query_string, kMaxDisplayBytes);
    std::memcpy(result + kMaxDisplayBytes, suffix, sizeof(suffix));
    return result;
}

/** Outer scan execution state; CustomScanState must remain first. */
enum class RuntimeRole : std::uint8_t {
    Bypass,
    Producer,
    Consumer,
};

struct StoredAttribute {
    Oid type_oid;
    std::int32_t type_modifier;
    Oid collation_oid;
    std::uint32_t reserved;
};

static_assert(std::is_trivially_copyable_v<StoredAttribute>);

struct BioseqCacheState {
    CustomScanState css;
    bool child_initialized;
    bool capture_failed;
    bool producer_completed;
    RuntimeRole runtime_role;
    int registry_index;
    int output_file;
    std::uint64_t generation;
    std::uint64_t row_count;
    std::uint64_t payload_bytes;
    std::uint64_t logical_bytes;
    ColumnStoreBuilder* column_builder;
    Oid* send_functions;
    char temporary_path[MAXPGPATH];
    char final_path[MAXPGPATH];
    Tuplestorestate* replay_store;
    MemoryContextCallback cleanup_callback;
    bool cleanup_registered;
    bool cleanup_done;
};

static_assert(std::is_standard_layout_v<BioseqCacheState>);
static_assert(offsetof(BioseqCacheState, css) == 0);

/** Read one String node from versioned custom-private metadata. */
[[nodiscard]] const char* private_string(const CustomScan& scan, int index) {
    if (scan.custom_private == NIL || list_length(scan.custom_private) <= index) {
        return "<metadata unavailable>";
    }
    Node* node = static_cast<Node*>(list_nth(scan.custom_private, index));
    return IsA(node, String) ? strVal(node) : "<metadata invalid>";
}

/** Append a length-delimited field to an exact key without ambiguous joins. */
[[nodiscard]] bool append_key_field(
        StringInfo key, const void* bytes, std::size_t size) {
    if (size > kMaximumKeyBytes
        || static_cast<std::size_t>(key->len) + sizeof(std::uint32_t) + size
            > kMaximumKeyBytes) {
        return false;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(size);
    appendBinaryStringInfo(
        key, reinterpret_cast<const char*>(&length), sizeof(length));
    if (size != 0) {
        appendBinaryStringInfo(
            key, static_cast<const char*>(bytes), static_cast<int>(size));
    }
    return true;
}

template <class Value>
[[nodiscard]] bool append_key_scalar(StringInfo key, const Value& value) {
    static_assert(std::is_trivially_copyable_v<Value>);
    return append_key_field(key, &value, sizeof(value));
}

/** Hash the exact output type/collation identity stored beside every result. */
[[nodiscard]] std::uint64_t output_schema_hash(const TupleDesc descriptor) {
    std::uint64_t value = 0x9e3779b97f4a7c15ULL;
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        const StoredAttribute stored{
            .type_oid = metadata->atttypid,
            .type_modifier = metadata->atttypmod,
            .collation_oid = metadata->attcollation,
            .reserved = 0,
        };
        value = hash_any_extended(
            reinterpret_cast<const unsigned char*>(&stored),
            sizeof(stored), value);
    }
    return value;
}

/** Confirm every output column has both binary directions before claiming. */
void validate_binary_schema(const TupleDesc descriptor) {
    if (descriptor->natts < 0
        || static_cast<std::uint32_t>(descriptor->natts)
            > kMaxProviderBoundaryColumns) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("cache result has too many columns")));
    }
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        Oid send_function = InvalidOid;
        Oid receive_function = InvalidOid;
        Oid io_parameter = InvalidOid;
        bool variable_length = false;
        getTypeBinaryOutputInfo(
            metadata->atttypid, &send_function, &variable_length);
        getTypeBinaryInputInfo(
            metadata->atttypid, &receive_function, &io_parameter);
        if (!OidIsValid(send_function) || !OidIsValid(receive_function)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("type %u lacks binary cache I/O", metadata->atttypid)));
        }
    }
}

[[nodiscard]] std::uint8_t type_alignment_bytes(char alignment) {
    switch (alignment) {
        case TYPALIGN_CHAR: return 1;
        case TYPALIGN_SHORT: return 2;
        case TYPALIGN_INT: return 4;
        case TYPALIGN_DOUBLE: return 8;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("unknown PostgreSQL type alignment '%c'", alignment)));
    }
    pg_unreachable();
}

/** Freeze PostgreSQL tuple metadata into the portable column-store schema. */
[[nodiscard]] std::vector<ColumnSchema> build_column_schema(
        TupleDesc descriptor, Oid* send_functions) {
    std::vector<ColumnSchema> schema;
    schema.reserve(static_cast<std::size_t>(descriptor->natts));
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        Oid send_function = InvalidOid;
        Oid receive_function = InvalidOid;
        Oid io_parameter = InvalidOid;
        bool variable_length = false;
        getTypeBinaryOutputInfo(
            metadata->atttypid, &send_function, &variable_length);
        getTypeBinaryInputInfo(
            metadata->atttypid, &receive_function, &io_parameter);
        if (send_functions != nullptr) {
            send_functions[attribute] = send_function;
        }
        const std::uint64_t semantic_high =
            (static_cast<std::uint64_t>(metadata->atttypid) << 32U)
            | static_cast<std::uint32_t>(attribute + 1);
        const std::uint64_t semantic_low =
            (static_cast<std::uint64_t>(metadata->attcollation) << 32U)
            | static_cast<std::uint32_t>(metadata->atttypmod);
        schema.push_back(ColumnSchema{
            .name = NameStr(metadata->attname),
            .descriptor = ColumnDescriptorV1{
                .type_oid = metadata->atttypid,
                .typmod = metadata->atttypmod,
                .collation_oid = metadata->attcollation,
                .type_length = metadata->attlen,
                .by_value = static_cast<std::uint8_t>(metadata->attbyval),
                .type_alignment = type_alignment_bytes(metadata->attalign),
                .encoding = ColumnEncoding::BinarySend,
                .flags = kColumnFlagNullable,
                .send_function_oid = send_function,
                .receive_function_oid = receive_function,
                .receive_type_io_param = io_parameter,
                .reserved = 0,
                .name_offset = 0,
            },
            .semantic_id = SemanticId128{
                .high = semantic_high,
                .low = semantic_low,
            },
        });
    }
    return schema;
}

[[nodiscard]] ColumnStoreIdentity current_column_store_identity() noexcept {
    return ColumnStoreIdentity{
        .postgres_major = static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
        .boot_nonce = g_shared == nullptr
            ? 0
            : pg_atomic_read_u64(&g_shared->boot_nonce),
        .database_oid = MyDatabaseId,
        .owner_role_oid = GetUserId(),
        .datum_width = static_cast<std::uint8_t>(sizeof(Datum)),
    };
}

void destroy_column_builder(BioseqCacheState& state) noexcept {
    delete state.column_builder;
    state.column_builder = nullptr;
    if (state.send_functions != nullptr) {
        pfree(state.send_functions);
        state.send_functions = nullptr;
    }
}

/** Serialize one external parameter with type and NULL identity. */
[[nodiscard]] bool append_parameter_key(
        StringInfo key, const ParamExternData& parameter) {
    if (!append_key_scalar(key, parameter.ptype)
        || !append_key_scalar(key, parameter.isnull)) {
        return false;
    }
    if (parameter.isnull) {
        return append_key_field(key, nullptr, 0);
    }
    Oid send_function = InvalidOid;
    bool variable_length = false;
    getTypeBinaryOutputInfo(
        parameter.ptype, &send_function, &variable_length);
    bytea* bytes = OidSendFunctionCall(send_function, parameter.value);
    const bool appended = append_key_field(
        key, VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));
    pfree(bytes);
    return appended;
}

/** Build the execution-time exact key, including parameters and environment. */
[[nodiscard]] StringInfo build_runtime_key(
        const CustomScan& scan, EState* estate, TupleDesc descriptor) {
    StringInfo key = makeStringInfo();
    const std::uint32_t key_format = 2;
    Oid effective_user = InvalidOid;
    int security_context = 0;
    GetUserIdAndSecContext(&effective_user, &security_context);
    const Oid session_user = GetSessionUserId();
    const std::uint64_t schema_hash = output_schema_hash(descriptor);
    const char* canonical_query = private_string(scan, 0);
    const std::size_t canonical_size = std::strlen(canonical_query);
    if (!append_key_scalar(key, key_format)
        || !append_key_scalar(key, MyDatabaseId)
        || !append_key_scalar(key, effective_user)
        || !append_key_scalar(key, session_user)
        || !append_key_scalar(key, security_context)
        || !append_key_scalar(key, XactIsoLevel)
        || !append_key_scalar(key, schema_hash)
        || !append_key_field(key, canonical_query, canonical_size)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }

    static constexpr const char* environment_options[] = {
        "TimeZone", "DateStyle", "IntervalStyle", "extra_float_digits",
        "bytea_output", "search_path", "row_security",
        "default_table_access_method", "default_tablespace",
    };
    for (const char* option : environment_options) {
        const char* canonical_name = nullptr;
        char* value = GetConfigOptionByName(option, &canonical_name, false);
        if (!append_key_field(key, option, std::strlen(option))
            || !append_key_field(key, value, std::strlen(value))) {
            pfree(value);
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
        pfree(value);
    }

    const std::uint64_t coarse_epoch = pg_atomic_read_u64(
        &g_shared->coarse_dependency_epoch);
    List* referenced_relations = scan.custom_private != NIL
            && list_length(scan.custom_private) > 4
        ? static_cast<List*>(list_nth(scan.custom_private, 4))
        : NIL;
    const int relation_count = list_length(referenced_relations);
    if (relation_count > static_cast<int>(kMaximumRelationDependencies)
        || !append_key_scalar(key, coarse_epoch)
        || !append_key_scalar(key, relation_count)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }
    ListCell* relation_cell = nullptr;
    foreach(relation_cell, referenced_relations) {
        const Oid relation_oid = lfirst_oid(relation_cell);
        const std::uint64_t relation_epoch = relation_dependency_epoch(
            relation_oid);
        if (!OidIsValid(relation_oid)
            || !append_key_scalar(key, relation_oid)
            || !append_key_scalar(key, relation_epoch)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
    }

    ParamListInfo parameters = estate == nullptr
        ? nullptr
        : estate->es_param_list_info;
    List* referenced_parameters = scan.custom_private != NIL
            && list_length(scan.custom_private) > 3
        ? static_cast<List*>(list_nth(scan.custom_private, 3))
        : NIL;
    const int parameter_count = list_length(referenced_parameters);
    if (!append_key_scalar(key, parameter_count)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }
    ListCell* parameter_cell = nullptr;
    foreach(parameter_cell, referenced_parameters) {
        const int parameter_id = intVal(lfirst(parameter_cell));
        if (parameters == nullptr
            || parameter_id <= 0
            || parameter_id > parameters->numParams
            || !append_key_scalar(key, parameter_id)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
        ParamExternData workspace{};
        ParamExternData* parameter = parameters->paramFetch != nullptr
            ? parameters->paramFetch(
                parameters, parameter_id, false, &workspace)
            : &parameters->params[parameter_id - 1];
        if (parameter == nullptr || !OidIsValid(parameter->ptype)
            || !append_parameter_key(key, *parameter)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
    }

    const int attribute_count = descriptor->natts;
    if (!append_key_scalar(key, attribute_count)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }
    for (int attribute = 0; attribute < attribute_count; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        const StoredAttribute stored{
            .type_oid = metadata->atttypid,
            .type_modifier = metadata->atttypmod,
            .collation_oid = metadata->attcollation,
            .reserved = 0,
        };
        if (!append_key_field(key, &stored, sizeof(stored))) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
    }
    return key;
}

/** EINTR-safe cache-file writes; callers fail open on false. */
[[nodiscard]] bool write_all(int file, const void* bytes, std::size_t size) {
    const auto* cursor = static_cast<const unsigned char*>(bytes);
    while (size != 0) {
        const ssize_t written = ::write(file, cursor, size);
        if (written > 0) {
            cursor += written;
            size -= static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/** EINTR-safe exact cache-file reads. */
[[nodiscard]] bool read_all(int file, void* bytes, std::size_t size) {
    auto* cursor = static_cast<unsigned char*>(bytes);
    while (size != 0) {
        const ssize_t received = ::read(file, cursor, size);
        if (received > 0) {
            cursor += received;
            size -= static_cast<std::size_t>(received);
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

#if defined(BIOSEQ_CACHE_TESTING)
[[nodiscard]] constexpr std::uint32_t pause_bit(
        TestPausePoint point) noexcept {
    return 1U << static_cast<std::uint32_t>(point);
}

[[nodiscard]] TestPausePoint parse_pause_point(const char* name) {
    if (std::strcmp(name, "after_building_publish") == 0) {
        return TestPausePoint::AfterBuildingPublish;
    }
    if (std::strcmp(name, "after_waiter_registered") == 0) {
        return TestPausePoint::AfterWaiterRegistered;
    }
    if (std::strcmp(name, "before_terminal_publish") == 0) {
        return TestPausePoint::BeforeTerminalPublish;
    }
    ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("unknown bioseq_cache pause point: %s", name)));
    pg_unreachable();
}

/** Deterministic test pause compiled out of production builds. */
void maybe_pause(TestPausePoint point) {
    if (g_shared == nullptr) {
        return;
    }
    CacheTestControl& control = g_shared->test_control;
    const std::uint32_t bit = pause_bit(point);
    if ((pg_atomic_read_u32(&control.armed_mask) & bit) == 0) {
        return;
    }
    pg_atomic_fetch_or_u32(&control.paused_mask, bit);
    ConditionVariableBroadcast(&control.changed);
    ConditionVariablePrepareToSleep(&control.changed);
    PG_TRY();
    {
        while ((pg_atomic_read_u32(&control.released_mask) & bit) == 0) {
            CHECK_FOR_INTERRUPTS();
            static_cast<void>(ConditionVariableTimedSleep(
                &control.changed, 1000L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        pg_atomic_fetch_and_u32(&control.armed_mask, ~bit);
        pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
        pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
        ConditionVariableBroadcast(&control.changed);
        PG_RE_THROW();
    }
    PG_END_TRY();
    pg_atomic_fetch_and_u32(&control.armed_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
    ConditionVariableBroadcast(&control.changed);
}
#else
enum class TestPausePoint : std::uint32_t {
    AfterBuildingPublish,
    AfterWaiterRegistered,
    BeforeTerminalPublish,
};
inline void maybe_pause(TestPausePoint) noexcept {}
#endif

struct RegistryDecision {
    RuntimeRole role = RuntimeRole::Bypass;
    int index = -1;
    std::uint64_t generation = 0;
    char final_path[MAXPGPATH]{};
};

/** Compare every key byte after the hash bucket shortcut. */
[[nodiscard]] bool entry_key_matches(
        const CacheEntry& entry, std::uint64_t key_hash,
        const StringInfo key) noexcept {
    return entry.key_hash == key_hash
        && entry.key_size == static_cast<std::uint32_t>(key->len)
        && std::memcmp(entry.key, key->data, key->len) == 0;
}

/** Complete entries are usable only in their captured database epoch. */
[[nodiscard]] bool entry_is_current(
        const CacheEntry& entry, TimestampTz now) noexcept {
    return entry.state == SharedEntryState::Complete
        && entry.expires_at > now;
}

/** Mark an abandoned producer failed while holding the registry lock. */
void fail_abandoned_producer(CacheEntry& entry) noexcept {
    entry.state = SharedEntryState::Failed;
    entry.producer_pid = 0;
    entry.producer_proc_number = INVALID_PROC_NUMBER;
    ConditionVariableBroadcast(&entry.changed);
}

/** Wait for a matching producer; pre-existing waiters may consume TTL-zero. */
[[nodiscard]] RegistryDecision wait_for_producer(
        int index, std::uint64_t generation) {
    RegistryDecision decision{};
    CacheEntry& entry = g_shared->entries[index];
    ConditionVariablePrepareToSleep(&entry.changed);
    PG_TRY();
    {
        maybe_pause(TestPausePoint::AfterWaiterRegistered);
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
            if (entry.generation != generation
                || entry.state == SharedEntryState::Failed
                || entry.state == SharedEntryState::Retired
                || entry.state == SharedEntryState::Empty) {
                if (entry.generation == generation && entry.waiter_count > 0) {
                    --entry.waiter_count;
                }
                LWLockRelease(g_registry_lock);
                break;
            }
            if (entry.state == SharedEntryState::Complete) {
                if (entry.waiter_count > 0) {
                    --entry.waiter_count;
                }
                ++entry.reader_count;
                ++entry.hits;
                decision.role = RuntimeRole::Consumer;
                decision.index = index;
                decision.generation = generation;
                strlcpy(
                    decision.final_path, entry.final_path,
                    sizeof(decision.final_path));
                LWLockRelease(g_registry_lock);
                break;
            }
            PGPROC* producer = entry.producer_proc_number == INVALID_PROC_NUMBER
                ? nullptr
                : ProcNumberGetProc(entry.producer_proc_number);
            if (entry.state == SharedEntryState::Producing
                && (producer == nullptr || producer->pid != entry.producer_pid)) {
                if (entry.waiter_count > 0) {
                    --entry.waiter_count;
                }
                fail_abandoned_producer(entry);
                LWLockRelease(g_registry_lock);
                break;
            }
            LWLockRelease(g_registry_lock);
            static_cast<void>(ConditionVariableTimedSleep(
                &entry.changed, 1000L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        if (entry.generation == generation && entry.waiter_count > 0) {
            --entry.waiter_count;
        }
        LWLockRelease(g_registry_lock);
        PG_RE_THROW();
    }
    PG_END_TRY();
    return decision;
}

/** Select an exact hit, wait on its producer, or claim one bounded slot. */
[[nodiscard]] RegistryDecision acquire_registry_entry(
        const StringInfo key, std::uint64_t database_epoch,
        const char* display_sql, List* relation_oids) {
    RegistryDecision decision{};
    const std::uint64_t key_hash = hash_any_extended(
        reinterpret_cast<const unsigned char*>(key->data), key->len,
        pg_atomic_read_u64(&g_shared->boot_nonce));
    const TimestampTz now = GetCurrentTimestamp();
    int reusable_index = -1;
    int lru_index = -1;
    TimestampTz oldest_completion = DT_NOEND;
    TimestampTz oldest_live_completion = DT_NOEND;
    char retired_path[MAXPGPATH]{};

    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t offset = 0; offset < kRegistryCapacity; ++offset) {
        CacheEntry& entry = g_shared->entries[offset];
        if (entry_key_matches(entry, key_hash, key)) {
            if (entry_is_current(entry, now)) {
                ++entry.reader_count;
                ++entry.hits;
                decision.role = RuntimeRole::Consumer;
                decision.index = static_cast<int>(offset);
                decision.generation = entry.generation;
                strlcpy(
                    decision.final_path, entry.final_path,
                    sizeof(decision.final_path));
                LWLockRelease(g_registry_lock);
                return decision;
            }
            if (entry.state == SharedEntryState::Producing
                && entry.database_epoch == database_epoch) {
                ++entry.waiter_count;
                const std::uint64_t generation = entry.generation;
                LWLockRelease(g_registry_lock);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.inflight_waits, 1);
                }
                return wait_for_producer(
                    static_cast<int>(offset), generation);
            }
        }

        const bool unreferenced = entry.reader_count == 0
            && entry.waiter_count == 0;
        if (!unreferenced || entry.state == SharedEntryState::Producing) {
            continue;
        }
        if (entry.state == SharedEntryState::Empty
            || entry.state == SharedEntryState::Failed
            || entry.state == SharedEntryState::Retired) {
            if (reusable_index < 0) {
                reusable_index = static_cast<int>(offset);
            }
            continue;
        }
        if (entry.expires_at <= now
            && (reusable_index < 0
                || entry.completed_at < oldest_completion)) {
            reusable_index = static_cast<int>(offset);
            oldest_completion = entry.completed_at;
            continue;
        }
        if (entry.state == SharedEntryState::Complete
            && entry.completed_at < oldest_live_completion) {
            lru_index = static_cast<int>(offset);
            oldest_live_completion = entry.completed_at;
        }
    }

    if (reusable_index < 0) {
        reusable_index = lru_index;
    }

    if (reusable_index >= 0) {
        CacheEntry& entry = g_shared->entries[reusable_index];
        if (entry.final_path[0] != '\0') {
            strlcpy(retired_path, entry.final_path, sizeof(retired_path));
        }
        const std::uint64_t next_generation = entry.generation + 1;
        entry.state = SharedEntryState::Producing;
        entry.key_size = static_cast<std::uint32_t>(key->len);
        entry.generation = next_generation == 0 ? 1 : next_generation;
        entry.key_hash = key_hash;
        entry.database_epoch = database_epoch;
        entry.database_oid = MyDatabaseId;
        entry.current_role_oid = GetUserId();
        entry.session_user_oid = GetSessionUserId();
        entry.producer_pid = MyProcPid;
        entry.producer_proc_number = MyProcNumber;
        entry.waiter_count = 0;
        entry.reader_count = 0;
        entry.created_at = now;
        entry.completed_at = 0;
        entry.expires_at = 0;
        entry.row_count = 0;
        entry.logical_bytes = 0;
        entry.stored_bytes = 0;
        entry.hits = 0;
        entry.relation_count = 0;
        entry.reserved = 0;
        std::memset(entry.relation_oids, 0, sizeof(entry.relation_oids));
        ListCell* relation_cell = nullptr;
        foreach(relation_cell, relation_oids) {
            if (entry.relation_count >= kMaximumRelationDependencies) {
                break;
            }
            entry.relation_oids[entry.relation_count++] =
                lfirst_oid(relation_cell);
        }
        entry.final_path[0] = '\0';
        strlcpy(
            entry.display_sql,
            display_sql == nullptr ? "<query text unavailable>" : display_sql,
            sizeof(entry.display_sql));
        std::memcpy(entry.key, key->data, key->len);
        decision.role = RuntimeRole::Producer;
        decision.index = reusable_index;
        decision.generation = entry.generation;
    }
    LWLockRelease(g_registry_lock);

    if (retired_path[0] != '\0') {
        static_cast<void>(::unlink(retired_path));
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.evictions, 1);
        }
    }
    return decision;
}

/** Release a short reader pin after full preflight/materialization. */
void release_registry_reader(
        int index, std::uint64_t generation, bool successful) noexcept {
    if (g_shared == nullptr || index < 0) {
        return;
    }
    char invalid_path[MAXPGPATH]{};
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[index];
    if (entry.generation == generation) {
        if (entry.reader_count > 0) {
            --entry.reader_count;
        }
        if (entry.reader_count == 0
            && entry.state == SharedEntryState::Retired
            && entry.final_path[0] != '\0') {
            strlcpy(invalid_path, entry.final_path, sizeof(invalid_path));
            entry.final_path[0] = '\0';
        }
        if (!successful && entry.state == SharedEntryState::Complete) {
            entry.state = SharedEntryState::Retired;
            strlcpy(invalid_path, entry.final_path, sizeof(invalid_path));
            entry.final_path[0] = '\0';
            ConditionVariableBroadcast(&entry.changed);
        }
    }
    LWLockRelease(g_registry_lock);
    if (invalid_path[0] != '\0') {
        static_cast<void>(::unlink(invalid_path));
    }
    if (successful) {
        pg_atomic_fetch_add_u64(&g_shared->stats.completed_hits, 1);
    } else {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

/** Fail a generation only when this backend still owns the producer token. */
void fail_registry_producer(BioseqCacheState& state) noexcept {
    if (g_shared != nullptr && state.registry_index >= 0) {
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        CacheEntry& entry = g_shared->entries[state.registry_index];
        if (entry.generation == state.generation
            && entry.state == SharedEntryState::Producing
            && entry.producer_pid == MyProcPid) {
            fail_abandoned_producer(entry);
        }
        LWLockRelease(g_registry_lock);
    }
    if (state.output_file >= 0) {
        static_cast<void>(CloseTransientFile(state.output_file));
        state.output_file = -1;
    }
    if (state.temporary_path[0] != '\0') {
        static_cast<void>(::unlink(state.temporary_path));
        state.temporary_path[0] = '\0';
    }
    if (state.final_path[0] != '\0' && !state.producer_completed) {
        static_cast<void>(::unlink(state.final_path));
    }
    destroy_column_builder(state);
    state.capture_failed = true;
    state.runtime_role = RuntimeRole::Bypass;
}

/** PostgreSQL memory-context cleanup covers ERROR paths that skip EndCustomScan. */
void cache_state_cleanup(void* argument) noexcept {
    auto* state = static_cast<BioseqCacheState*>(argument);
    if (state == nullptr || state->cleanup_done) {
        return;
    }
    state->cleanup_done = true;
    if (state->runtime_role == RuntimeRole::Producer
        && !state->producer_completed) {
        fail_registry_producer(*state);
    }
    if (state->output_file >= 0) {
        static_cast<void>(CloseTransientFile(state->output_file));
        state->output_file = -1;
    }
    if (state->temporary_path[0] != '\0') {
        static_cast<void>(::unlink(state->temporary_path));
        state->temporary_path[0] = '\0';
    }
    destroy_column_builder(*state);
}

/** Prepare one generation-private self-validating column-store capture. */
[[nodiscard]] bool begin_producer_file(
        BioseqCacheState& state, TupleDesc descriptor) {
    char directory[MAXPGPATH]{};
    cache_store_directory(directory, sizeof(directory));
    const unsigned long long boot_nonce = static_cast<unsigned long long>(
        pg_atomic_read_u64(&g_shared->boot_nonce));
    const unsigned long long generation = static_cast<unsigned long long>(
        state.generation);
    const int final_length = snprintf(
        state.final_path, sizeof(state.final_path),
        "%s/bioseq_cache_%llx_%u_%d_%llu.result",
        directory,
        boot_nonce, MyDatabaseId, state.registry_index, generation);
    const int temporary_length = snprintf(
        state.temporary_path, sizeof(state.temporary_path),
        "%s.tmp.%d", state.final_path, MyProcPid);
    if (final_length < 0
        || static_cast<std::size_t>(final_length) >= sizeof(state.final_path)
        || temporary_length < 0
        || static_cast<std::size_t>(temporary_length)
            >= sizeof(state.temporary_path)) {
        return false;
    }
    try {
        state.send_functions = static_cast<Oid*>(
            palloc0(sizeof(Oid) * static_cast<std::size_t>(descriptor->natts)));
        std::vector<ColumnSchema> schema = build_column_schema(
            descriptor, state.send_functions);
        state.column_builder = new ColumnStoreBuilder(
            current_column_store_identity(), schema);
        return true;
    } catch (...) {
        destroy_column_builder(state);
        return false;
    }
}

/** Capture one projected row through PostgreSQL's lossless binary send ABI. */
[[nodiscard]] bool capture_projected_row(
        BioseqCacheState& state, TupleTableSlot* slot) {
    if (state.column_builder == nullptr || state.send_functions == nullptr) {
        return false;
    }
    slot_getallattrs(slot);
    TupleDesc descriptor = slot->tts_tupleDescriptor;
    auto* values = static_cast<EncodedColumnValue*>(palloc0(
        sizeof(EncodedColumnValue)
        * static_cast<std::size_t>(descriptor->natts)));
    auto** encoded = static_cast<bytea**>(palloc0(
        sizeof(bytea*) * static_cast<std::size_t>(descriptor->natts)));
    bool success = false;
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        if (slot->tts_isnull[attribute]) {
            values[attribute].is_null = true;
            continue;
        }
        encoded[attribute] = OidSendFunctionCall(
            state.send_functions[attribute], slot->tts_values[attribute]);
        const std::size_t byte_count = VARSIZE_ANY_EXHDR(encoded[attribute]);
        values[attribute].bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                VARDATA_ANY(encoded[attribute])),
            byte_count,
        };
        state.logical_bytes += byte_count;
    }
    if (state.logical_bytes <= kMaximumStoredResultBytes) {
        try {
            const auto appended = state.column_builder->append_row(
                std::span<const EncodedColumnValue>{
                    values, static_cast<std::size_t>(descriptor->natts)});
            success = appended.has_value();
        } catch (...) {
            success = false;
        }
    }
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        if (encoded[attribute] != nullptr) {
            pfree(encoded[attribute]);
        }
    }
    pfree(encoded);
    pfree(values);
    if (success) {
        ++state.row_count;
    }
    return success;
}

/** Atomically publish a complete file only if no commit raced its capture. */
[[nodiscard]] bool complete_registry_producer(
        BioseqCacheState& state, TupleDesc descriptor) {
    static_cast<void>(descriptor);
    if (state.column_builder == nullptr
        || state.column_builder->row_count() != state.row_count) {
        return false;
    }
    std::vector<std::byte> sealed;
    try {
        PostgreSqlColumnChecksumPolicy policy;
        auto result = state.column_builder->seal(policy);
        if (!result.has_value()) {
            return false;
        }
        sealed = std::move(*result);
    } catch (...) {
        return false;
    }
    if (sealed.size() > kMaximumStoredResultBytes) {
        return false;
    }
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_shared != nullptr
        && pg_atomic_exchange_u32(
            &g_shared->test_control.fail_next_write, 0) != 0) {
        return false;
    }
#endif
    state.payload_bytes = sealed.size();
    state.output_file = OpenTransientFile(
        state.temporary_path,
        O_CREAT | O_EXCL | O_WRONLY | PG_BINARY);
    if (state.output_file < 0) {
        return false;
    }
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& producing_entry = g_shared->entries[state.registry_index];
    if (producing_entry.generation == state.generation
        && producing_entry.state == SharedEntryState::Producing
        && producing_entry.producer_pid == MyProcPid) {
        strlcpy(
            producing_entry.final_path, state.temporary_path,
            sizeof(producing_entry.final_path));
    }
    LWLockRelease(g_registry_lock);
    if (!write_all(state.output_file, sealed.data(), sealed.size())
        || pg_fsync(state.output_file) != 0
        || CloseTransientFile(state.output_file) != 0) {
        state.output_file = -1;
        return false;
    }
    state.output_file = -1;
    if (::rename(state.temporary_path, state.final_path) != 0) {
        return false;
    }
    state.temporary_path[0] = '\0';
    destroy_column_builder(state);
    maybe_pause(TestPausePoint::BeforeTerminalPublish);

    bool published = false;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[state.registry_index];
    const std::uint64_t current_epoch = pg_atomic_read_u64(
        &g_shared->database_write_epoch);
    const bool stable_visibility = pg_atomic_read_u32(
        &g_shared->database_transition_count) == 0;
    if (entry.generation == state.generation
        && entry.state == SharedEntryState::Producing
        && entry.producer_pid == MyProcPid
        && entry.database_epoch == current_epoch
        && stable_visibility) {
        const TimestampTz completed_at = GetCurrentTimestamp();
        const std::int64_t ttl_microseconds =
            static_cast<std::int64_t>(g_query_cache_ttl_ms) * 1000;
        entry.state = SharedEntryState::Complete;
        entry.producer_pid = 0;
        entry.producer_proc_number = INVALID_PROC_NUMBER;
        entry.completed_at = completed_at;
        entry.expires_at = completed_at + ttl_microseconds;
        entry.row_count = state.row_count;
        entry.logical_bytes = state.logical_bytes;
        entry.stored_bytes = state.payload_bytes;
        strlcpy(entry.final_path, state.final_path, sizeof(entry.final_path));
        pg_atomic_fetch_add_u64(&g_shared->stats.produced, 1);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.logical_bytes, state.logical_bytes);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.stored_bytes, entry.stored_bytes);
        published = true;
        state.producer_completed = true;
        ConditionVariableBroadcast(&entry.changed);
    } else if (entry.generation == state.generation
            && entry.state == SharedEntryState::Producing) {
        fail_abandoned_producer(entry);
    }
    LWLockRelease(g_registry_lock);
    if (!published) {
        static_cast<void>(::unlink(state.final_path));
    }
    return published;
}

/** Materialize and validate an entire shared result before exposing row one. */
[[nodiscard]] Tuplestorestate* load_complete_result(
        const char* path, TupleDesc expected_descriptor) {
    std::vector<ColumnSchema> expected_schema;
    try {
        expected_schema = build_column_schema(expected_descriptor, nullptr);
    } catch (...) {
        return nullptr;
    }
    int file = OpenTransientFile(path, O_RDONLY | PG_BINARY);
    if (file < 0) {
        return nullptr;
    }
    struct stat status{};
    if (::fstat(file, &status) != 0
        || status.st_size <= 0
        || static_cast<std::uint64_t>(status.st_size)
            > kMaximumStoredResultBytes) {
        static_cast<void>(CloseTransientFile(file));
        return nullptr;
    }
    const std::size_t store_size = static_cast<std::size_t>(status.st_size);
    auto* store = static_cast<std::byte*>(palloc(store_size));
    const bool read = read_all(file, store, store_size);
    static_cast<void>(CloseTransientFile(file));
    if (!read) {
        pfree(store);
        return nullptr;
    }

    ColumnStoreView* view = nullptr;
    try {
        PostgreSqlColumnChecksumPolicy policy;
        auto validated = validate_column_store(
            std::span<const std::byte>{store, store_size},
            current_column_store_identity(),
            expected_schema,
            policy);
        if (!validated.has_value()) {
            pfree(store);
            return nullptr;
        }
        view = new ColumnStoreView(std::move(*validated));
    } catch (...) {
        pfree(store);
        return nullptr;
    }
    if (view->column_count()
        != static_cast<std::size_t>(expected_descriptor->natts)) {
        delete view;
        pfree(store);
        return nullptr;
    }
    Tuplestorestate* result = tuplestore_begin_heap(true, false, work_mem);
    MemoryContext row_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq_cache replay row",
        ALLOCSET_DEFAULT_SIZES);
    Datum* values = static_cast<Datum*>(
        palloc0(sizeof(Datum)
            * static_cast<std::size_t>(expected_descriptor->natts)));
    bool* nulls = static_cast<bool*>(
        palloc0(sizeof(bool)
            * static_cast<std::size_t>(expected_descriptor->natts)));
    bool valid = true;
    MemoryContext caller_context = CurrentMemoryContext;
    PG_TRY();
    {
        for (std::uint64_t row = 0; row < view->row_count() && valid; ++row) {
            MemoryContextReset(row_context);
            MemoryContext old_context = MemoryContextSwitchTo(row_context);
            for (int attribute = 0;
                 attribute < expected_descriptor->natts; ++attribute) {
                auto cell = view->value(
                    row, static_cast<std::size_t>(attribute));
                if (!cell.has_value()) {
                    valid = false;
                    break;
                }
                if (cell->is_null) {
                    values[attribute] = static_cast<Datum>(0);
                    nulls[attribute] = true;
                    continue;
                }
                if (cell->bytes.size()
                    > static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)())) {
                    valid = false;
                    break;
                }
                const ColumnDescriptorV1& codec =
                    expected_schema[static_cast<std::size_t>(attribute)]
                        .descriptor;
                StringInfoData input{
                    .data = const_cast<char*>(reinterpret_cast<const char*>(
                        cell->bytes.data())),
                    .len = static_cast<int>(cell->bytes.size()),
                    .maxlen = static_cast<int>(cell->bytes.size()),
                    .cursor = 0,
                };
                values[attribute] = OidReceiveFunctionCall(
                    codec.receive_function_oid,
                    &input,
                    codec.receive_type_io_param,
                    codec.typmod);
                nulls[attribute] = false;
                if (input.cursor != input.len) {
                    valid = false;
                    break;
                }
            }
            MemoryContextSwitchTo(old_context);
            if (valid) {
                tuplestore_putvalues(
                    result, expected_descriptor, values, nulls);
            }
        }
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(caller_context);
        FlushErrorState();
        valid = false;
    }
    PG_END_TRY();
    MemoryContextDelete(row_context);
    pfree(values);
    pfree(nulls);
    delete view;
    pfree(store);
    if (!valid) {
        tuplestore_end(result);
        return nullptr;
    }
    tuplestore_rescan(result);
    return result;
}

/** Initialize the retained child exactly once, releasing claims on error. */
void initialize_retained_child(
        BioseqCacheState& state, CustomScanState* node,
        EState* estate, int eflags) {
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    if (scan->custom_plans == NIL || list_length(scan->custom_plans) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqCacheScan requires exactly one retained child")));
    }
    Plan* child_plan = static_cast<Plan*>(linitial(scan->custom_plans));
    if (node->custom_ps != NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqCacheScan child was initialized twice")));
    }
    node->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
    state.child_initialized = true;
}

[[nodiscard]] PlanState* retained_child(CustomScanState* node) noexcept {
    return node->custom_ps == NIL
        ? nullptr
        : static_cast<PlanState*>(linitial(node->custom_ps));
}

/** Decide producer/consumer/bypass before touching the retained child. */
void cache_begin_custom_scan(CustomScanState* node, EState* estate, int eflags) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    state->runtime_role = RuntimeRole::Bypass;
    state->registry_index = -1;
    state->output_file = -1;
    state->replay_store = nullptr;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->capture_failed = false;
    state->producer_completed = false;
    state->temporary_path[0] = '\0';
    state->final_path[0] = '\0';
    node->custom_ps = NIL;
    state->cleanup_done = false;
    if (!state->cleanup_registered) {
        state->cleanup_callback.func = cache_state_cleanup;
        state->cleanup_callback.arg = state;
        state->cleanup_callback.next = nullptr;
        MemoryContextRegisterResetCallback(
            estate->es_query_cxt, &state->cleanup_callback);
        state->cleanup_registered = true;
    }

    const bool explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
    const VisibilityTicket visibility = explain_only
        ? VisibilityTicket{}
        : mint_visibility_ticket();
    const bool cache_allowed = visibility.valid
        && g_query_cache_mode != static_cast<int>(QueryCacheMode::Disabled)
        && XactIsoLevel == XACT_READ_COMMITTED
        && !transaction_is_dirty()
        && node->ss.ps.ps_ResultTupleDesc != nullptr;
    if (cache_allowed) {
        StringInfo key = nullptr;
        MemoryContext caller_context = CurrentMemoryContext;
        PG_TRY();
        {
            validate_binary_schema(node->ss.ps.ps_ResultTupleDesc);
            key = build_runtime_key(
                *scan, estate, node->ss.ps.ps_ResultTupleDesc);
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(caller_context);
            FlushErrorState();
            key = nullptr;
        }
        PG_END_TRY();

        if (key != nullptr
            && !visibility_ticket_still_bound(visibility)) {
            pfree(key->data);
            pfree(key);
            key = nullptr;
        }

        if (key != nullptr) {
            List* referenced_relations = scan->custom_private != NIL
                    && list_length(scan->custom_private) > 4
                ? static_cast<List*>(list_nth(scan->custom_private, 4))
                : NIL;
            RegistryDecision decision = acquire_registry_entry(
                key, visibility.database_epoch, private_string(*scan, 1),
                referenced_relations);
            state->runtime_role = decision.role;
            state->registry_index = decision.index;
            state->generation = decision.generation;
            strlcpy(
                state->final_path, decision.final_path,
                sizeof(state->final_path));
        }
        if (key != nullptr) {
            pfree(key->data);
            pfree(key);
        }

        if (state->runtime_role == RuntimeRole::Consumer) {
            MemoryContext load_context = CurrentMemoryContext;
            PG_TRY();
            {
                state->replay_store = load_complete_result(
                    state->final_path, node->ss.ps.ps_ResultTupleDesc);
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(load_context);
                FlushErrorState();
                state->replay_store = nullptr;
            }
            PG_END_TRY();
            const bool loaded = state->replay_store != nullptr;
            release_registry_reader(
                state->registry_index, state->generation, loaded);
            if (loaded) {
                return;
            }
            state->runtime_role = RuntimeRole::Bypass;
        }

        if (state->runtime_role == RuntimeRole::Producer
            ) {
            maybe_pause(TestPausePoint::AfterBuildingPublish);
            if (!begin_producer_file(
                    *state, node->ss.ps.ps_ResultTupleDesc)) {
                fail_registry_producer(*state);
            }
        }
    }

    PG_TRY();
    {
        initialize_retained_child(*state, node, estate, eflags);
    }
    PG_CATCH();
    {
        if (state->runtime_role == RuntimeRole::Producer) {
            fail_registry_producer(*state);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (!explain_only
        && state->runtime_role == RuntimeRole::Bypass
        && g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

/** Execute the retained child and explicitly project its raw slot. */
TupleTableSlot* cache_exec_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Consumer) {
        TupleTableSlot* slot = node->ss.ps.ps_ResultTupleSlot;
        ExecClearTuple(slot);
        static_cast<void>(tuplestore_gettupleslot(
            state->replay_store, true, false, slot));
        return slot;
    }
    PlanState* child = retained_child(node);
    if (child == nullptr) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    TupleTableSlot* raw_slot = ExecProcNode(child);
    if (TupIsNull(raw_slot)) {
        if (state->runtime_role == RuntimeRole::Producer) {
            MemoryContext caller_context = CurrentMemoryContext;
            bool completed = false;
            PG_TRY();
            {
                completed = complete_registry_producer(
                    *state, node->ss.ps.ps_ResultTupleDesc);
            }
            PG_CATCH();
            {
                MemoryContextSwitchTo(caller_context);
                FlushErrorState();
                completed = false;
            }
            PG_END_TRY();
            if (!completed) {
                fail_registry_producer(*state);
            } else {
                state->runtime_role = RuntimeRole::Bypass;
            }
        }
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    expression_context->ecxt_scantuple = raw_slot;
    TupleTableSlot* result = node->ss.ps.ps_ProjInfo == nullptr
        ? raw_slot
        : ExecProject(node->ss.ps.ps_ProjInfo);
    if (state->runtime_role == RuntimeRole::Producer) {
        MemoryContext caller_context = CurrentMemoryContext;
        bool captured = false;
        PG_TRY();
        {
            captured = capture_projected_row(*state, result);
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(caller_context);
            FlushErrorState();
            captured = false;
        }
        PG_END_TRY();
        if (!captured) {
            fail_registry_producer(*state);
            if (g_shared != nullptr) {
                pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
            }
        }
    }
    return result;
}

/** End only the child this node initialized. */
void cache_end_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Producer
        && !state->producer_completed) {
        fail_registry_producer(*state);
    }
    if (state->replay_store != nullptr) {
        tuplestore_end(state->replay_store);
        state->replay_store = nullptr;
    }
    cache_state_cleanup(state);
    if (state->child_initialized && retained_child(node) != nullptr) {
        ExecEndNode(retained_child(node));
        node->custom_ps = NIL;
    }
    state->child_initialized = false;
}

/** Rewind the retained child for supported forward rescans. */
void cache_rescan_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Consumer
        && state->replay_store != nullptr) {
        tuplestore_rescan(state->replay_store);
        return;
    }
    if (state->runtime_role == RuntimeRole::Producer) {
        fail_registry_producer(*state);
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
        }
    }
    if (retained_child(node) != nullptr) {
        ExecReScan(retained_child(node));
    }
}

/** Explain provisional and actual pass-through state without side effects. */
void cache_explain_custom_scan(
        CustomScanState* node, List*, ExplainState* explain_state) {
    const auto* scan = reinterpret_cast<const CustomScan*>(node->ss.ps.plan);
    const QueryCacheMode mode = static_cast<QueryCacheMode>(g_query_cache_mode);
    const char* mode_name = mode == QueryCacheMode::Reuse
        ? "reuse"
        : mode == QueryCacheMode::Same ? "same" : "disabled";
    ExplainPropertyText("Cache Mode", mode_name, explain_state);
    ExplainPropertyText("Planned Cache Action", "provisional", explain_state);
    if (explain_state->analyze) {
        const auto* state = reinterpret_cast<const BioseqCacheState*>(node);
        const char* action = state->replay_store != nullptr
            ? "[Hit] exact final result"
            : state->producer_completed
                ? "[Produce] exact final result"
                : "[Bypass] retained child";
        ExplainPropertyText("Actual Cache Action", action, explain_state);
    }
    ExplainPropertyText("SQL", private_string(*scan, 1), explain_state);
    ExplainPropertyText("Cache Entry Kind", "ExactFinalResult", explain_state);
    ExplainPropertyText("Cache Readiness", "passthrough", explain_state);
}

const CustomExecMethods& cache_exec_methods() {
    static const CustomExecMethods methods = {
        .CustomName = "BioseqCacheScan",
        .BeginCustomScan = cache_begin_custom_scan,
        .ExecCustomScan = cache_exec_custom_scan,
        .EndCustomScan = cache_end_custom_scan,
        .ReScanCustomScan = cache_rescan_custom_scan,
        .ExplainCustomScan = cache_explain_custom_scan,
    };
    return methods;
}

/** Allocate the standard-layout executor state. */
Node* cache_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<BioseqCacheState*>(
        newNode(sizeof(BioseqCacheState), T_CustomScanState));
    state->css.methods = &cache_exec_methods();
    state->child_initialized = false;
    state->runtime_role = RuntimeRole::Bypass;
    state->registry_index = -1;
    state->output_file = -1;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods& cache_scan_methods() {
    static const CustomScanMethods methods = {
        .CustomName = "BioseqCacheScan",
        .CreateCustomScanState = cache_create_custom_scan_state,
    };
    return methods;
}

/** Convert one pre-setrefs outer CustomPath into a CustomScan. */
Plan* cache_plan_custom_path(
        PlannerInfo*, RelOptInfo* relation, CustomPath* best_path,
        List* target_list, List*, List* custom_plans) {
    if (custom_plans == NIL || list_length(custom_plans) != 1) {
        return nullptr;
    }
    auto* scan = makeNode(CustomScan);
    scan->methods = &cache_scan_methods();
    scan->flags = best_path->flags;
    scan->scan.scanrelid = 0;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = static_cast<List*>(copyObject(best_path->custom_private));
    scan->custom_exprs = NIL;
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
    scan->custom_scan_tlist = static_cast<List*>(copyObject(child->targetlist));
    scan->custom_relids = bms_copy(relation->relids);
    return reinterpret_cast<Plan*>(scan);
}

const CustomPathMethods& cache_path_methods() {
    static const CustomPathMethods methods = {
        .CustomName = "BioseqCacheScan",
        .PlanCustomPath = cache_plan_custom_path,
    };
    return methods;
}

/** Create an exact-result wrapper around one final path. */
[[nodiscard]] CustomPath* make_exact_cache_path(
        RelOptInfo& output_relation, Path& input_path,
        const PlannerFrame& frame) {
    auto* path = makeNode(CustomPath);
    path->path.pathtype = T_CustomScan;
    path->path.parent = &output_relation;
    path->path.pathtarget = input_path.pathtarget;
    path->path.param_info = nullptr;
#if PG_VERSION_NUM >= 180000
    path->path.disabled_nodes = input_path.disabled_nodes;
#endif
    path->path.parallel_aware = false;
    path->path.parallel_safe = false;
    path->path.parallel_workers = 0;
    path->path.rows = input_path.rows;
    path->path.startup_cost = input_path.startup_cost;
    path->path.total_cost = input_path.total_cost
        + cpu_tuple_cost * input_path.rows * 0.01;
    path->path.pathkeys = input_path.pathkeys;
    path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    path->custom_paths = list_make1(&input_path);
    path->custom_private = list_make3(
        makeString(pstrdup(frame.canonical_query)),
        makeString(pstrdup(frame.display_sql)),
        makeInteger(0)); /* force_bypass is preallocated for later verification */
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_parameter_ids));
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_relation_oids));
    path->custom_restrictinfo = NIL;
    path->methods = &cache_path_methods();
    return path;
}

/** Predecessor-first top-level FINAL path decoration before setrefs. */
void cache_create_upper_paths(
        PlannerInfo* root, UpperRelationKind stage,
        RelOptInfo* input_relation, RelOptInfo* output_relation, void* extra) {
    if (g_previous_create_upper_paths_hook != nullptr) {
        g_previous_create_upper_paths_hook(
            root, stage, input_relation, output_relation, extra);
    }
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr
        || !frame->eligible
        || stage != UPPERREL_FINAL
        || root->parent_root != nullptr
        || root->parse != frame->delegated_query
        || output_relation == nullptr
        || output_relation->pathlist == NIL) {
        return;
    }

    List* original_paths = output_relation->pathlist;
    List* wrappers = NIL;
    ListCell* path_cell = nullptr;
    foreach(path_cell, original_paths) {
        Path* input_path = static_cast<Path*>(lfirst(path_cell));
        if (input_path->param_info != nullptr) {
            continue;
        }
        wrappers = lappend(
            wrappers,
            make_exact_cache_path(*output_relation, *input_path, *frame));
    }
    if (wrappers == NIL) {
        return;
    }

    output_relation->pathlist = NIL;
    output_relation->cheapest_startup_path = nullptr;
    output_relation->cheapest_total_path = nullptr;
    output_relation->cheapest_unique_path = nullptr;
    output_relation->cheapest_parameterized_paths = NIL;
    foreach(path_cell, wrappers) {
        add_path(output_relation, static_cast<Path*>(lfirst(path_cell)));
    }
    set_cheapest(output_relation);
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.planner_build_generation, 1);
    }
}

/** Reentrant planner wrapper that owns canonical bytes for upper-path hooks. */
PlannedStmt* cache_planner(
        Query* query, const char* query_string,
        int cursor_options, ParamListInfo bound_parameters) {
    if (g_internal_planning
        || g_query_cache_mode == static_cast<int>(QueryCacheMode::Disabled)
        || !shared_cache_ready()) {
        return g_previous_planner_hook != nullptr
            ? g_previous_planner_hook(
                query, query_string, cursor_options, bound_parameters)
            : standard_planner(
                query, query_string, cursor_options, bound_parameters);
    }

    MemoryContext planner_context = CurrentMemoryContext;
    PlannerFrame frame{
        .previous = g_planner_frame,
        .delegated_query = query,
        .canonical_query = nullptr,
        .display_sql = nullptr,
        .referenced_parameter_ids = NIL,
        .referenced_relation_oids = NIL,
        .eligible = false,
    };
    g_planner_frame = &frame;
    PlannedStmt* result = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        frame.eligible = query_is_exact_cache_eligible(*query);
        if (frame.eligible) {
            bool relation_overflow = false;
            frame.canonical_query = canonicalize_query(*query);
            frame.display_sql = copy_display_sql(query_string);
            frame.referenced_parameter_ids = collect_external_parameter_ids(*query);
            frame.referenced_relation_oids = collect_relation_oids(
                *query, relation_overflow);
            frame.eligible = !relation_overflow;
        }
        result = g_previous_planner_hook != nullptr
            ? g_previous_planner_hook(
                query, query_string, cursor_options, bound_parameters)
            : standard_planner(
                query, query_string, cursor_options, bound_parameters);
        g_planner_frame = frame.previous;
    }
    PG_CATCH();
    {
        g_planner_frame = frame.previous;
        PG_RE_THROW();
    }
    PG_END_TRY();
    return result;
}

/** Preserve write observability even when a writer session disables caching. */
void mark_planned_result_relations(PlannedStmt& statement) {
    bool precise = false;
    ListCell* relation_cell = nullptr;
    foreach(relation_cell, statement.resultRelations) {
        const int range_index = lfirst_int(relation_cell);
        if (range_index <= 0 || range_index > list_length(statement.rtable)) {
            mark_current_transaction_dirty();
            return;
        }
        const auto* range = static_cast<const RangeTblEntry*>(
            list_nth(statement.rtable, range_index - 1));
        if (range == nullptr || range->rtekind != RTE_RELATION
            || !OidIsValid(range->relid)) {
            mark_current_transaction_dirty();
            return;
        }
        mark_current_transaction_relation_dirty(range->relid);
        precise = true;
    }
    if (!precise) {
        mark_current_transaction_dirty();
    }
}

void cache_executor_start(QueryDesc* query_description, int eflags) {
    if (query_description != nullptr
        && query_description->plannedstmt != nullptr
        && (query_description->plannedstmt->commandType != CMD_SELECT
            || query_description->plannedstmt->hasModifyingCTE)) {
        mark_planned_result_relations(*query_description->plannedstmt);
    }
    if (g_previous_executor_start_hook != nullptr) {
        g_previous_executor_start_hook(query_description, eflags);
    } else {
        standard_ExecutorStart(query_description, eflags);
    }
}

/** Exclude utility commands that cannot change a reusable SQL result. */
[[nodiscard]] bool utility_may_change_database(const Node* statement) noexcept {
    if (statement == nullptr) {
        return false;
    }
    switch (nodeTag(statement)) {
        case T_TransactionStmt:
        case T_VariableSetStmt:
        case T_VariableShowStmt:
        case T_PrepareStmt:
        case T_ExecuteStmt: /* Any executed DML is observed by ExecutorStart. */
        case T_DeallocateStmt:
        case T_ExplainStmt: /* EXPLAIN ANALYZE DML is observed by ExecutorStart. */
        case T_DeclareCursorStmt:
        case T_ClosePortalStmt:
        case T_FetchStmt:
        case T_ListenStmt:
        case T_UnlistenStmt:
        case T_NotifyStmt:
        case T_CheckPointStmt:
        case T_DiscardStmt:
            return false;
        default:
            return true;
    }
}

/** Conservatively publish one database-wide epoch for utility writes. */
void cache_process_utility(
        PlannedStmt* statement, const char* query_string, bool read_only_tree,
        ProcessUtilityContext context, ParamListInfo parameters,
        QueryEnvironment* environment, DestReceiver* destination,
        QueryCompletion* completion) {
    const bool dirty = statement != nullptr
        && utility_may_change_database(statement->utilityStmt);
    const bool outside_transaction = dirty && !IsTransactionState();
    if (dirty && !outside_transaction) {
        mark_current_transaction_dirty();
    }
    if (g_previous_process_utility_hook != nullptr) {
        g_previous_process_utility_hook(
            statement, query_string, read_only_tree, context, parameters,
            environment, destination, completion);
    } else {
        standard_ProcessUtility(
            statement, query_string, read_only_tree, context, parameters,
            environment, destination, completion);
    }
    if (outside_transaction) {
        g_force_coarse_transition = true;
        begin_database_transition();
        finish_database_transition(true);
    }
}

/** Install all permanently chained backend hooks. */
void install_cache_hooks() {
    if (g_hooks_installed) {
        return;
    }
    g_previous_planner_hook = planner_hook;
    planner_hook = cache_planner;
    g_previous_create_upper_paths_hook = create_upper_paths_hook;
    create_upper_paths_hook = cache_create_upper_paths;
    g_previous_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = cache_executor_start;
    g_previous_process_utility_hook = ProcessUtility_hook;
    ProcessUtility_hook = cache_process_utility;
    g_hooks_installed = true;
}

/** Read one shared counter without exposing shared-memory pointers. */
[[nodiscard]] std::uint64_t read_counter(pg_atomic_uint64& counter) noexcept {
    return g_shared == nullptr ? 0 : pg_atomic_read_u64(&counter);
}

struct EntrySnapshot {
    SharedEntryState state;
    std::uint64_t key_hash;
    std::uint64_t generation;
    Oid database_oid;
    Oid current_role_oid;
    int producer_pid;
    std::uint32_t waiter_count;
    std::uint32_t reader_count;
    TimestampTz created_at;
    TimestampTz completed_at;
    TimestampTz expires_at;
    std::uint64_t row_count;
    std::uint64_t logical_bytes;
    std::uint64_t stored_bytes;
    std::uint64_t hits;
    char display_sql[4096];
};

[[nodiscard]] const char* entry_state_name(SharedEntryState state) noexcept {
    switch (state) {
        case SharedEntryState::Empty: return "EMPTY";
        case SharedEntryState::Producing: return "BUILDING";
        case SharedEntryState::Complete: return "COMPLETE";
        case SharedEntryState::Failed: return "FAILED";
        case SharedEntryState::Retired: return "RETIRED";
    }
    return "UNKNOWN";
}

/** Reap dead owners and optionally TTL-expired generations without I/O locks. */
[[nodiscard]] std::uint64_t sweep_registry(bool expire_completed) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    char paths[kRegistryCapacity][MAXPGPATH]{};
    std::size_t path_count = 0;
    std::uint64_t swept = 0;
    const TimestampTz now = GetCurrentTimestamp();
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        PGPROC* producer = entry.producer_proc_number == INVALID_PROC_NUMBER
            ? nullptr
            : ProcNumberGetProc(entry.producer_proc_number);
        const bool dead_producer = entry.state == SharedEntryState::Producing
            && (producer == nullptr || producer->pid != entry.producer_pid);
        const bool expired = expire_completed
            && entry.state == SharedEntryState::Complete
            && entry.expires_at <= now
            && entry.waiter_count == 0
            && entry.reader_count == 0;
        if (!dead_producer && !expired) {
            continue;
        }
        if (dead_producer) {
            fail_abandoned_producer(entry);
        } else {
            entry.state = SharedEntryState::Retired;
            ConditionVariableBroadcast(&entry.changed);
        }
        if (entry.reader_count == 0 && entry.final_path[0] != '\0') {
            strlcpy(paths[path_count++], entry.final_path, MAXPGPATH);
            entry.final_path[0] = '\0';
        }
        ++swept;
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < path_count; ++index) {
        static_cast<void>(::unlink(paths[index]));
    }
    return swept;
}

/** Copy bounded diagnostics under lock; never hold shared locks during SQL I/O. */
[[nodiscard]] int snapshot_entries(EntrySnapshot* snapshots) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    static_cast<void>(sweep_registry(false));
    int count = 0;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const CacheEntry& entry : g_shared->entries) {
        if (entry.state == SharedEntryState::Empty) {
            continue;
        }
        EntrySnapshot& snapshot = snapshots[count++];
        snapshot.state = entry.state;
        snapshot.key_hash = entry.key_hash;
        snapshot.generation = entry.generation;
        snapshot.database_oid = entry.database_oid;
        snapshot.current_role_oid = entry.current_role_oid;
        snapshot.producer_pid = entry.producer_pid;
        snapshot.waiter_count = entry.waiter_count;
        snapshot.reader_count = entry.reader_count;
        snapshot.created_at = entry.created_at;
        snapshot.completed_at = entry.completed_at;
        snapshot.expires_at = entry.expires_at;
        snapshot.row_count = entry.row_count;
        snapshot.logical_bytes = entry.logical_bytes;
        snapshot.stored_bytes = entry.stored_bytes;
        snapshot.hits = entry.hits;
        strlcpy(
            snapshot.display_sql, entry.display_sql,
            sizeof(snapshot.display_sql));
    }
    LWLockRelease(g_registry_lock);
    return count;
}

/** Retire all live generations and wake waiters; returns affected entries. */
[[nodiscard]] std::uint64_t invalidate_registry() noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    char paths[kRegistryCapacity][MAXPGPATH]{};
    std::size_t path_count = 0;
    std::uint64_t invalidated = 0;
    pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
    pg_atomic_fetch_add_u64(&g_shared->coarse_dependency_epoch, 1);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        if (entry.state == SharedEntryState::Producing) {
            fail_abandoned_producer(entry);
            ++invalidated;
            continue;
        }
        if (entry.state != SharedEntryState::Complete) {
            continue;
        }
        entry.state = SharedEntryState::Retired;
        ++invalidated;
        if (entry.reader_count == 0 && entry.final_path[0] != '\0') {
            strlcpy(paths[path_count++], entry.final_path, MAXPGPATH);
            entry.final_path[0] = '\0';
        }
        ConditionVariableBroadcast(&entry.changed);
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < path_count; ++index) {
        static_cast<void>(::unlink(paths[index]));
    }
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.invalidations, invalidated);
    }
    ConditionVariableBroadcast(&g_shared->database_changed);
    return invalidated;
}

[[nodiscard]] bool entry_depends_on_relation(
        const CacheEntry& entry, Oid relation_oid) noexcept {
    if (entry.database_oid != MyDatabaseId) {
        return false;
    }
    for (std::uint16_t index = 0; index < entry.relation_count; ++index) {
        if (entry.relation_oids[index] == relation_oid) {
            return true;
        }
    }
    return false;
}

/** Advance and retire only generations that reference one relation. */
[[nodiscard]] std::uint64_t invalidate_relation_registry(
        Oid relation_oid) noexcept {
    if (g_shared == nullptr || !OidIsValid(relation_oid)) {
        return 0;
    }
    char paths[kRegistryCapacity][MAXPGPATH]{};
    std::size_t path_count = 0;
    std::uint64_t invalidated = 0;
    pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    if (!bump_relation_epoch_locked(relation_oid)) {
        pg_atomic_fetch_add_u64(&g_shared->coarse_dependency_epoch, 1);
    }
    for (CacheEntry& entry : g_shared->entries) {
        if (!entry_depends_on_relation(entry, relation_oid)) {
            continue;
        }
        if (entry.state == SharedEntryState::Producing) {
            fail_abandoned_producer(entry);
            ++invalidated;
            continue;
        }
        if (entry.state != SharedEntryState::Complete) {
            continue;
        }
        entry.state = SharedEntryState::Retired;
        ++invalidated;
        if (entry.reader_count == 0 && entry.final_path[0] != '\0') {
            strlcpy(paths[path_count++], entry.final_path, MAXPGPATH);
            entry.final_path[0] = '\0';
        }
        ConditionVariableBroadcast(&entry.changed);
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < path_count; ++index) {
        static_cast<void>(::unlink(paths[index]));
    }
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.invalidations, invalidated);
    }
    ConditionVariableBroadcast(&g_shared->database_changed);
    return invalidated;
}

/** Require a materialized set-returning call and initialize its tuplestore. */
[[nodiscard]] TupleDesc initialize_srf(
        FunctionCallInfo function_call, Tuplestorestate** tuple_store) {
    auto* return_info = reinterpret_cast<ReturnSetInfo*>(
        function_call->resultinfo);
    if (return_info == nullptr
        || !IsA(return_info, ReturnSetInfo)
        || (return_info->allowedModes & SFRM_Materialize) == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bioseq_cache function requires materialize mode")));
    }
    TupleDesc tuple_descriptor = nullptr;
    if (get_call_result_type(
            function_call, nullptr, &tuple_descriptor) != TYPEFUNC_COMPOSITE) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq_cache function has an invalid result type")));
    }
    MemoryContext old_context = MemoryContextSwitchTo(
        return_info->econtext->ecxt_per_query_memory);
    *tuple_store = tuplestore_begin_heap(true, false, work_mem);
    return_info->returnMode = SFRM_Materialize;
    return_info->setResult = *tuple_store;
    return_info->setDesc = tuple_descriptor;
    MemoryContextSwitchTo(old_context);
    return tuple_descriptor;
}

}  // namespace bioseq::cache::pg

extern "C" {

PG_FUNCTION_INFO_V1(bioseq_cache_version);
Datum bioseq_cache_version(PG_FUNCTION_ARGS) {
    PG_RETURN_TEXT_P(cstring_to_text("1.0"));
}

PG_FUNCTION_INFO_V1(bioseq_cache_stats);
Datum bioseq_cache_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    Tuplestorestate* tuple_store = nullptr;
    TupleDesc tuple_descriptor = initialize_srf(fcinfo, &tuple_store);
    Datum values[9]{};
    bool nulls[9]{};
    if (g_shared != nullptr) {
        values[0] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.produced)));
        values[1] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.completed_hits)));
        values[2] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.inflight_waits)));
        values[3] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.reuse_hits)));
        values[4] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.fallbacks)));
        values[5] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.invalidations)));
        values[6] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.evictions)));
        values[7] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.logical_bytes)));
        values[8] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.stored_bytes)));
    }
    tuplestore_putvalues(tuple_store, tuple_descriptor, values, nulls);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_entries);
Datum bioseq_cache_entries(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    Tuplestorestate* tuple_store = nullptr;
    TupleDesc tuple_descriptor = initialize_srf(fcinfo, &tuple_store);
    if (tuple_descriptor->natts != 20) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq_cache.entries() has an invalid row type")));
    }
    auto* snapshots = static_cast<EntrySnapshot*>(
        palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
    const int count = snapshot_entries(snapshots);
    const TimestampTz now = GetCurrentTimestamp();
    for (int index = 0; index < count; ++index) {
        const EntrySnapshot& entry = snapshots[index];
        Datum values[20]{};
        bool nulls[20]{};
        char* key = psprintf(
            "%016llx", static_cast<unsigned long long>(entry.key_hash));
        values[0] = CStringGetTextDatum(key);
        values[1] = Int64GetDatum(static_cast<int64>(entry.generation));
        values[2] = CStringGetTextDatum("ExactFinalResult");
        values[3] = CStringGetTextDatum(entry_state_name(entry.state));
        values[4] = ObjectIdGetDatum(entry.database_oid);
        values[5] = ObjectIdGetDatum(entry.current_role_oid);
        values[6] = Int32GetDatum(entry.producer_pid);
        values[7] = TimestampTzGetDatum(entry.created_at);
        if (entry.completed_at != 0) {
            values[8] = TimestampTzGetDatum(entry.completed_at);
        } else {
            nulls[8] = true;
        }
        auto* age = static_cast<Interval*>(palloc0(sizeof(Interval)));
        age->time = now - entry.created_at;
        values[9] = IntervalPGetDatum(age);
        auto* ttl = static_cast<Interval*>(palloc0(sizeof(Interval)));
        ttl->time = entry.completed_at == 0
            ? static_cast<std::int64_t>(g_query_cache_ttl_ms) * 1000
            : entry.expires_at - entry.completed_at;
        values[10] = IntervalPGetDatum(ttl);
        values[11] = Int64GetDatum(static_cast<int64>(entry.row_count));
        values[12] = Int64GetDatum(static_cast<int64>(entry.logical_bytes));
        values[13] = Int64GetDatum(static_cast<int64>(entry.stored_bytes));
        values[14] = Int32GetDatum(static_cast<int32>(entry.waiter_count));
        values[15] = Int32GetDatum(static_cast<int32>(entry.reader_count));
        values[16] = Int64GetDatum(static_cast<int64>(entry.hits));
        values[17] = CStringGetTextDatum(entry.display_sql);
        values[18] = CStringGetTextDatum("retained child");
        values[19] = DirectFunctionCall1(
            jsonb_in, CStringGetDatum("[]"));
        tuplestore_putvalues(tuple_store, tuple_descriptor, values, nulls);
        pfree(key);
        pfree(age);
        pfree(ttl);
    }
    pfree(snapshots);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_invalidate_all);
Datum bioseq_cache_invalidate_all(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to invalidate bioseq_cache")));
    }
    const std::uint64_t invalidated = invalidate_registry();
    PG_RETURN_INT64(static_cast<int64>(invalidated));
}

PG_FUNCTION_INFO_V1(bioseq_cache_invalidate_relation);
Datum bioseq_cache_invalidate_relation(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to invalidate bioseq_cache")));
    }
    const Oid relation_oid = PG_GETARG_OID(0);
    const std::uint64_t invalidated = invalidate_relation_registry(
        relation_oid);
    PG_RETURN_INT64(static_cast<int64>(invalidated));
}

PG_FUNCTION_INFO_V1(bioseq_cache_reset_stats);
Datum bioseq_cache_reset_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to reset bioseq_cache statistics")));
    }
    if (g_shared != nullptr) {
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        pg_atomic_write_u64(&g_shared->stats.produced, 0);
        pg_atomic_write_u64(&g_shared->stats.completed_hits, 0);
        pg_atomic_write_u64(&g_shared->stats.inflight_waits, 0);
        pg_atomic_write_u64(&g_shared->stats.reuse_hits, 0);
        pg_atomic_write_u64(&g_shared->stats.fallbacks, 0);
        pg_atomic_write_u64(&g_shared->stats.invalidations, 0);
        pg_atomic_write_u64(&g_shared->stats.evictions, 0);
        pg_atomic_write_u64(&g_shared->stats.logical_bytes, 0);
        pg_atomic_write_u64(&g_shared->stats.stored_bytes, 0);
        LWLockRelease(g_registry_lock);
    }
    PG_RETURN_VOID();
}

#if defined(BIOSEQ_CACHE_TESTING)

static void require_cache_test_superuser() {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to use bioseq_cache test controls")));
    }
    if (bioseq::cache::pg::g_shared == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq_cache test shared memory is unavailable")));
    }
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_arm_pause);
Datum bioseq_cache_test_arm_pause(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const std::uint32_t bit = pause_bit(parse_pause_point(name));
    CacheTestControl& control = g_shared->test_control;
    pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
    pg_atomic_fetch_or_u32(&control.armed_mask, bit);
    ConditionVariableBroadcast(&control.changed);
    pfree(name);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_wait_until_paused);
Datum bioseq_cache_test_wait_until_paused(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const std::uint32_t bit = pause_bit(parse_pause_point(name));
    pfree(name);
    CacheTestControl& control = g_shared->test_control;
    const TimestampTz started = GetCurrentTimestamp();
    ConditionVariablePrepareToSleep(&control.changed);
    PG_TRY();
    {
        while ((pg_atomic_read_u32(&control.paused_mask) & bit) == 0) {
            CHECK_FOR_INTERRUPTS();
            if (TimestampDifferenceExceeds(
                    started, GetCurrentTimestamp(), 30000)) {
                ereport(ERROR,
                    (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                     errmsg("timed out waiting for bioseq_cache pause")));
            }
            static_cast<void>(ConditionVariableTimedSleep(
                &control.changed, 100L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        PG_RE_THROW();
    }
    PG_END_TRY();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_release_pause);
Datum bioseq_cache_test_release_pause(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const std::uint32_t bit = pause_bit(parse_pause_point(name));
    pfree(name);
    pg_atomic_fetch_or_u32(&g_shared->test_control.released_mask, bit);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_fail_next_write);
Datum bioseq_cache_test_fail_next_write(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* failure = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (std::strcmp(failure, "encoder_error") != 0
        && std::strcmp(failure, "enospc") != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown bioseq_cache write failure: %s", failure)));
    }
    pfree(failure);
    pg_atomic_write_u32(&g_shared->test_control.fail_next_write, 1);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_entry_states);
Datum bioseq_cache_test_entry_states(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    auto* snapshots = static_cast<EntrySnapshot*>(
        palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
    const int count = snapshot_entries(snapshots);
    for (int index = 0; index < count; ++index) {
        const EntrySnapshot& entry = snapshots[index];
        char* key = psprintf(
            "%016llx", static_cast<unsigned long long>(entry.key_hash));
        Datum values[6]{
            CStringGetTextDatum(key),
            Int64GetDatum(static_cast<int64>(entry.generation)),
            CStringGetTextDatum(entry_state_name(entry.state)),
            Int32GetDatum(static_cast<int32>(entry.waiter_count)),
            Int32GetDatum(static_cast<int32>(entry.reader_count)),
            Int32GetDatum(entry.producer_pid),
        };
        bool nulls[6]{};
        tuplestore_putvalues(store, descriptor, values, nulls);
        pfree(key);
    }
    pfree(snapshots);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_planner_reference_counts);
Datum bioseq_cache_test_planner_reference_counts(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    auto* snapshots = static_cast<EntrySnapshot*>(
        palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
    const int count = snapshot_entries(snapshots);
    std::uint64_t readers = 0;
    std::uint64_t waiters = 0;
    for (int index = 0; index < count; ++index) {
        readers += snapshots[index].reader_count;
        waiters += snapshots[index].waiter_count;
    }
    Datum values[3]{
        Int64GetDatum(static_cast<int64>(readers)),
        Int64GetDatum(static_cast<int64>(waiters)),
        Int64GetDatum(0),
    };
    bool nulls[3]{};
    tuplestore_putvalues(store, descriptor, values, nulls);
    pfree(snapshots);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_provider_callback_stats);
Datum bioseq_cache_test_provider_callback_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    static_cast<void>(initialize_srf(fcinfo, &store));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_plan_build_stats);
Datum bioseq_cache_test_plan_build_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    const std::uint64_t generation = read_counter(
        g_shared->stats.planner_build_generation);
    const bool same = g_query_cache_mode == static_cast<int>(QueryCacheMode::Same);
    const bool reuse = g_query_cache_mode == static_cast<int>(QueryCacheMode::Reuse);
    Datum values[5]{
        Int64GetDatum(static_cast<int64>(generation)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<int64>(same ? generation : 0)),
        Int64GetDatum(static_cast<int64>(reuse ? generation : 0)),
        Int64GetDatum(0),
    };
    bool nulls[5]{};
    tuplestore_putvalues(store, descriptor, values, nulls);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_mapping_fault);
Datum bioseq_cache_test_set_mapping_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    static_cast<void>(PG_GETARG_TEXT_PP(0));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_mapping_fault);
Datum bioseq_cache_test_clear_mapping_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_runtime_digest_fault);
Datum bioseq_cache_test_set_runtime_digest_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    static_cast<void>(PG_GETARG_TEXT_PP(0));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_runtime_digest_fault);
Datum bioseq_cache_test_clear_runtime_digest_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_forced_coverage_domain_digest);
Datum bioseq_cache_test_set_forced_coverage_domain_digest(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    static_cast<void>(PG_GETARG_BYTEA_PP(0));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_forced_coverage_domain_digest);
Datum bioseq_cache_test_clear_forced_coverage_domain_digest(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    PG_RETURN_VOID();
}

static void copy_injected_bytes(
        unsigned char* destination, std::uint16_t* destination_size,
        const bytea* source, const char* field_name) {
    const std::size_t size = VARSIZE_ANY_EXHDR(source);
    if (size > bioseq::cache::pg::kInjectedDigestBytes) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("%s exceeds %zu bytes", field_name,
                    bioseq::cache::pg::kInjectedDigestBytes)));
    }
    *destination_size = static_cast<std::uint16_t>(size);
    if (size != 0) {
        std::memcpy(destination, VARDATA_ANY(source), size);
    }
}

static bytea* injected_bytes(const unsigned char* source, std::size_t size) {
    bytea* result = static_cast<bytea*>(palloc(VARHDRSZ + size));
    SET_VARSIZE(result, VARHDRSZ + size);
    if (size != 0) {
        std::memcpy(VARDATA(result), source, size);
    }
    return result;
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_inject_complete_coverage);
Datum bioseq_cache_test_inject_complete_coverage(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    bytea* domain = PG_GETARG_BYTEA_PP(0);
    bytea* predicate = PG_GETARG_BYTEA_PP(1);
    bytea* provider_digest = PG_GETARG_BYTEA_PP(2);
    bytea* forced_digest = PG_ARGISNULL(3) ? nullptr : PG_GETARG_BYTEA_PP(3);
    int selected = -1;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0; index < kInjectedCoverageCapacity; ++index) {
        if (!g_shared->injected_coverage[index].used
            || (g_shared->injected_coverage[index].evictable
                && g_shared->injected_coverage[index].database_oid
                    == MyDatabaseId)) {
            selected = static_cast<int>(index);
            break;
        }
    }
    if (selected < 0) {
        LWLockRelease(g_registry_lock);
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
             errmsg("bioseq_cache injected coverage registry is full")));
    }
    InjectedCoverage& entry = g_shared->injected_coverage[selected];
    std::memset(&entry, 0, sizeof(entry));
    entry.used = true;
    entry.database_oid = MyDatabaseId;
    int security_context = 0;
    GetUserIdAndSecContext(&entry.current_role_oid, &security_context);
    entry.security_context_flags = static_cast<std::uint32_t>(security_context);
    entry.session_user_oid = GetSessionUserId();
    entry.generation = pg_atomic_fetch_add_u64(
        &g_shared->next_injected_generation, 1);
    copy_injected_bytes(entry.domain, &entry.domain_size, domain, "domain key");
    copy_injected_bytes(
        entry.predicate, &entry.predicate_size, predicate, "predicate key");
    copy_injected_bytes(
        entry.provider_digest, &entry.provider_digest_size,
        provider_digest, "provider security digest");
    if (forced_digest != nullptr) {
        copy_injected_bytes(
            entry.forced_digest, &entry.forced_digest_size,
            forced_digest, "forced domain digest");
    }
    const std::uint64_t generation = entry.generation;
    LWLockRelease(g_registry_lock);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    PG_RETURN_INT64(static_cast<int64>(generation));
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_lookup_injected_coverage);
Datum bioseq_cache_test_lookup_injected_coverage(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    bytea* domain = PG_GETARG_BYTEA_PP(0);
    const std::size_t domain_size = VARSIZE_ANY_EXHDR(domain);
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const InjectedCoverage& entry : g_shared->injected_coverage) {
        if (!entry.used || entry.database_oid != MyDatabaseId
            || entry.domain_size != domain_size
            || std::memcmp(entry.domain, VARDATA_ANY(domain), domain_size) != 0) {
            continue;
        }
        bytea* predicate = injected_bytes(entry.predicate, entry.predicate_size);
        Datum values[3]{
            Int64GetDatum(static_cast<int64>(entry.generation)),
            PointerGetDatum(predicate),
            CStringGetTextDatum("COMPLETE"),
        };
        bool nulls[3]{};
        tuplestore_putvalues(store, descriptor, values, nulls);
        pfree(predicate);
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_coverage_bucket_states);
Datum bioseq_cache_test_coverage_bucket_states(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const InjectedCoverage& entry : g_shared->injected_coverage) {
        if (!entry.used || entry.database_oid != MyDatabaseId) {
            continue;
        }
        bytea* provider = injected_bytes(
            entry.provider_digest, entry.provider_digest_size);
        const unsigned char* digest = entry.forced_digest_size == 0
            ? entry.domain
            : entry.forced_digest;
        const std::size_t digest_size = entry.forced_digest_size == 0
            ? entry.domain_size
            : entry.forced_digest_size;
        bytea* coverage = injected_bytes(digest, digest_size);
        Datum generation_value = Int64GetDatum(
            static_cast<int64>(entry.generation));
        ArrayType* generations = construct_array(
            &generation_value, 1, INT8OID, 8, FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
        Datum values[9]{
            ObjectIdGetDatum(entry.database_oid),
            ObjectIdGetDatum(entry.current_role_oid),
            ObjectIdGetDatum(entry.session_user_oid),
            Int32GetDatum(static_cast<int32>(entry.security_context_flags)),
            Int32GetDatum(0),
            PointerGetDatum(provider),
            PointerGetDatum(coverage),
            Int32GetDatum(1),
            PointerGetDatum(generations),
        };
        bool nulls[9]{};
        tuplestore_putvalues(store, descriptor, values, nulls);
        pfree(provider);
        pfree(coverage);
        pfree(generations);
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_mutate_coverage_descriptor);
Datum bioseq_cache_test_mutate_coverage_descriptor(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const std::uint64_t generation = static_cast<std::uint64_t>(
        PG_GETARG_INT64(0));
    static_cast<void>(PG_GETARG_TEXT_PP(1));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.generation == generation) {
            entry.predicate_size = 0;
            break;
        }
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_mark_generation_evictable);
Datum bioseq_cache_test_mark_generation_evictable(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const std::uint64_t generation = static_cast<std::uint64_t>(
        PG_GETARG_INT64(0));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.generation == generation) {
            entry.evictable = true;
        }
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_request_janitor_sweep);
Datum bioseq_cache_test_request_janitor_sweep(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    static_cast<void>(sweep_registry(true));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.evictable) {
            entry.used = false;
        }
    }
    LWLockRelease(g_registry_lock);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_wait_for_generation_absent);
Datum bioseq_cache_test_wait_for_generation_absent(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const std::uint64_t generation = static_cast<std::uint64_t>(
        PG_GETARG_INT64(0));
    const Interval* timeout = PG_GETARG_INTERVAL_P(1);
    const TimestampTz deadline = GetCurrentTimestamp()
        + timeout->time
        + static_cast<std::int64_t>(timeout->day) * USECS_PER_DAY;
    bool absent = false;
    for (;;) {
        absent = true;
        LWLockAcquire(g_registry_lock, LW_SHARED);
        for (const InjectedCoverage& entry : g_shared->injected_coverage) {
            if (entry.used && entry.generation == generation) {
                absent = false;
                break;
            }
        }
        LWLockRelease(g_registry_lock);
        if (absent || GetCurrentTimestamp() >= deadline) {
            break;
        }
        CHECK_FOR_INTERRUPTS();
        static_cast<void>(ConditionVariableTimedSleep(
            &g_shared->test_control.changed, 10L, PG_WAIT_EXTENSION));
        ConditionVariableCancelSleep();
    }
    PG_RETURN_BOOL(absent);
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_direct_heap_insert);
Datum bioseq_cache_test_direct_heap_insert(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const Oid relation_oid = PG_GETARG_OID(0);
    const int32 identifier = PG_GETARG_INT32(1);
    text* value = PG_GETARG_TEXT_PP(2);
    char* relation_name = get_rel_name(relation_oid);
    char* namespace_name = get_namespace_name(get_rel_namespace(relation_oid));
    if (relation_name == nullptr || namespace_name == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_TABLE),
             errmsg("relation %u does not exist", relation_oid)));
    }
    char* qualified = quote_qualified_identifier(
        namespace_name, relation_name);
    char* command = psprintf("INSERT INTO %s VALUES ($1, $2)", qualified);
    Oid argument_types[2]{INT4OID, TEXTOID};
    Datum arguments[2]{Int32GetDatum(identifier), PointerGetDatum(value)};
    char nulls[2]{' ', ' '};
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("could not connect to SPI")));
    }
    const int status = SPI_execute_with_args(
        command, 2, argument_types, arguments, nulls, false, 0);
    static_cast<void>(SPI_finish());
    pfree(command);
    pfree(qualified);
    pfree(namespace_name);
    if (status != SPI_OK_INSERT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("direct test insert failed with SPI status %d", status)));
    }
    mark_current_transaction_dirty();
    PG_RETURN_VOID();
}

#endif /* BIOSEQ_CACHE_TESTING */

void _PG_init() {
    using namespace bioseq::cache::pg;
    register_cache_gucs();
    provider_registry::install_relation_write_observer_v1(
        observe_provider_relation_write);
    if (!provider_registry::initialize_provider_registry_v1()) {
        ereport(WARNING,
            (errmsg("bioseq_cache provider rendezvous is owned by another library")));
    }
    if (process_shared_preload_libraries_in_progress) {
        g_preload_requested = true;
        g_previous_shmem_request_hook = shmem_request_hook;
        shmem_request_hook = cache_shmem_request;
        g_previous_shmem_startup_hook = shmem_startup_hook;
        shmem_startup_hook = cache_shmem_startup;
    }
    install_cache_hooks();
    if (!g_callbacks_registered) {
        RegisterXactCallback(cache_transaction_callback, nullptr);
        RegisterSubXactCallback(cache_subtransaction_callback, nullptr);
        g_callbacks_registered = true;
    }
    RegisterCustomScanMethods(&cache_scan_methods());
}

void _PG_fini() {
    using namespace bioseq::cache::pg;
    provider_registry::install_relation_write_observer_v1(nullptr);
    provider_registry::unpublish_provider_registry_v1();
}

}  // extern "C"
