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
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"
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
#include "portability/instr_time.h"
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
#include "utils/rls.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
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
import bioseq.cache.pg.maintenance;
import bioseq.cache.pg.provider_registry;
import bioseq.cache.pg.reusable_adapter;
import bioseq.cache.pg.reusable_planning;
import bioseq.cache.pg.storage;

namespace bioseq::cache::pg {

inline constexpr std::uint32_t kSharedMagic = 0x42535143U; /* BSQC */
inline constexpr std::uint32_t kSharedAbiVersion = 4;
inline constexpr char kSharedName[] = "bioseq_cache shared state v4";
inline constexpr char kRegistryTrancheName[] = "bioseq_cache registry";
inline constexpr char kReusableSharedName[] =
    "bioseq_cache reusable registry v1";
inline constexpr char kReusableMembershipTrancheName[] =
    "bioseq_cache reusable membership";
inline constexpr char kReusableExactTrancheName[] =
    "bioseq_cache reusable exact partitions";
inline constexpr char kReusableCoverageTrancheName[] =
    "bioseq_cache reusable coverage partitions";
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
    AfterReaderPinned = 3,
    Count = 4,
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

struct RegistryWaiterOwner {
    bool active;
    std::uint8_t reserved8[3];
    int owner_pid;
    std::uint32_t owner_proc_number;
    std::uint32_t reserved32;
    std::uint64_t registration_sequence;
    TimestampTz registered_at;
    storage::ManifestWaiterToken storage_token;
};

struct RegistryReaderOwner {
    bool active;
    std::uint8_t reserved8[3];
    int owner_pid;
    std::uint32_t owner_proc_number;
    std::uint32_t reserved32;
    std::uint64_t registration_sequence;
};

/** One bounded registry record. Full key bytes make hash collisions harmless. */
struct CacheEntry {
    SharedEntryState state;
    std::uint32_t key_size;
    std::uint64_t generation;
    std::uint64_t claim_sequence;
    std::uint64_t key_hash;
    CacheDigest256 key_digest;
    std::uint64_t database_epoch;
    Oid database_oid;
    Oid current_role_oid;
    Oid session_user_oid;
    int producer_pid;
    ProcNumber producer_proc_number;
    std::uint32_t waiter_count;
    std::uint32_t reader_count;
    RegistryReaderOwner readers[storage::kReaderPinsPerManifest];
    RegistryWaiterOwner waiters[storage::kWaitersPerManifest];
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
    storage::ManifestHandle manifest_handle;
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
    pg_atomic_uint64 next_claim_sequence;
    pg_atomic_uint64 next_registry_waiter_sequence;
    pg_atomic_uint64 next_registry_reader_sequence;
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
inline ReusableRegistryAttachmentV1 g_reusable_registry{};
inline LWLock* g_reusable_membership_lock = nullptr;
inline LWLockPadded* g_reusable_exact_locks = nullptr;
inline LWLockPadded* g_reusable_coverage_locks = nullptr;
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
#if defined(BIOSEQ_CACHE_TESTING)
inline bool g_forced_coverage_domain_digest_active = false;
inline CacheDigest256 g_forced_coverage_domain_digest{};
#endif

#if defined(BIOSEQ_CACHE_TESTING)
/** Backend-local verifier faults exercise unavailable third-party AM paths. */
enum class TestExactPlanFault : std::uint8_t {
    None,
    UnknownTableAccessMethod,
    UnknownIndexAccessMethod,
};

inline TestExactPlanFault g_test_exact_plan_fault = TestExactPlanFault::None;
#endif

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
    std::array<const provider_registry::ProviderV1*,
        provider_registry::kMaximumRegisteredProvidersV1> providers{};
    std::array<reuse::PlannerOwnedIntentV1,
        provider_registry::kMaximumRegisteredProvidersV1> intents{};
    std::size_t provider_intent_count = 0;
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
    pg_atomic_init_u64(&shared.next_claim_sequence, 1);
    pg_atomic_init_u64(&shared.next_registry_waiter_sequence, 1);
    pg_atomic_init_u64(&shared.next_registry_reader_sequence, 1);
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

/** Implemented after the reusable-registry lock adapters are available. */
void consider_provider_reusable_paths(
    PlannerInfo*, RelOptInfo*, unsigned int, RangeTblEntry*,
    const provider_registry::ProviderV1*) noexcept;

/** Provider-owned path hook rendezvous; every optional failure is fail-open. */
void observe_provider_rel_paths(
        PlannerInfo* root, RelOptInfo* relation,
        unsigned int range_table_index, RangeTblEntry* range_entry,
        const provider_registry::ProviderV1* provider) noexcept {
    if (g_query_cache_mode != static_cast<int>(QueryCacheMode::Reuse)
        || !g_reusable_registry) {
        return;
    }
    consider_provider_reusable_paths(
        root, relation, range_table_index, range_entry, provider);
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

[[nodiscard]] constexpr ReusableRegistryConfigV1
reusable_registry_config() noexcept {
    return ReusableRegistryConfigV1{};
}

void acquire_reusable_exact_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockAcquire(&g_reusable_exact_locks[partition].lock, LW_EXCLUSIVE);
}

void release_reusable_exact_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockRelease(&g_reusable_exact_locks[partition].lock);
}

void acquire_reusable_coverage_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockAcquire(&g_reusable_coverage_locks[partition].lock, LW_EXCLUSIVE);
}

void release_reusable_coverage_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockRelease(&g_reusable_coverage_locks[partition].lock);
}

[[nodiscard]] ReusableRegistryPartitionCallbacksV1
reusable_partition_callbacks() noexcept {
    return ReusableRegistryPartitionCallbacksV1{
        .acquire_exact = acquire_reusable_exact_partition,
        .release_exact = release_reusable_exact_partition,
        .acquire_coverage = acquire_reusable_coverage_partition,
        .release_coverage = release_reusable_coverage_partition,
    };
}

/**
 * The bridge definition remains before the implementation so provider hooks
 * can call it without exposing planner internals through the rendezvous ABI.
 */
void consider_provider_reusable_paths_impl(
    PlannerInfo*, RelOptInfo*, unsigned int, RangeTblEntry*,
    const provider_registry::ProviderV1*) noexcept;

void consider_provider_reusable_paths(
        PlannerInfo* root, RelOptInfo* relation,
        unsigned int range_table_index, RangeTblEntry* range_entry,
        const provider_registry::ProviderV1* provider) noexcept {
    consider_provider_reusable_paths_impl(
        root, relation, range_table_index, range_entry, provider);
}

[[nodiscard]] std::uint64_t reusable_monotonic_now_ns() noexcept {
    instr_time current{};
    INSTR_TIME_SET_CURRENT(current);
    return INSTR_TIME_GET_NANOSEC(current);
}

[[nodiscard]] ReusableProducerIdentityV1
current_reusable_producer_identity() noexcept {
    const std::uint64_t nonce =
        static_cast<std::uint64_t>(MyStartTimestamp)
        ^ (static_cast<std::uint64_t>(MyProcPid) << 32U)
        ^ static_cast<std::uint64_t>(MyProcNumber);
    return ReusableProducerIdentityV1{
        .proc_number = static_cast<std::uint32_t>(MyProcNumber),
        .pid = MyProcPid,
        .backend_nonce = nonce == 0 ? 1 : nonce,
        .backend_started_at_us = MyStartTimestamp,
    };
}

[[nodiscard]] ReusableManifestHandleV1 reusable_manifest_handle(
        const storage::ManifestHandle& handle) noexcept {
    return ReusableManifestHandleV1{
        .slot = handle.slot,
        .reserved = handle.reserved,
        .slot_sequence = handle.slot_sequence,
        .id = handle.id,
    };
}

[[nodiscard]] storage::ManifestHandle storage_manifest_handle(
        const ReusableManifestHandleV1& handle) noexcept {
    return storage::ManifestHandle{
        .slot = handle.slot,
        .reserved = handle.reserved,
        .slot_sequence = handle.slot_sequence,
        .id = handle.id,
    };
}

[[nodiscard]] bool reusable_producer_alive(
        void*, const ReusableProducerIdentityV1& producer) noexcept {
    if (producer.pid <= 0
        || producer.proc_number
            >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    PGPROC* process = ProcNumberGetProc(
        static_cast<ProcNumber>(producer.proc_number));
    return process != nullptr && process->pid == producer.pid;
}

/** Request bounded shared memory and partitioned registry locks. */
void cache_shmem_request() {
    if (g_previous_shmem_request_hook != nullptr) {
        g_previous_shmem_request_hook();
    }
    RequestAddinShmemSpace(MAXALIGN(sizeof(CacheSharedState)));
    RequestNamedLWLockTranche(kRegistryTrancheName, 1);
    const auto reusable_request = reusable_registry_preload_request(
        reusable_registry_config());
    if (!reusable_request.has_value()) {
        ereport(ERROR,
            (errmsg("bioseq_cache reusable registry configuration is invalid")));
    }
    RequestAddinShmemSpace(MAXALIGN(reusable_request->layout.total_bytes));
    RequestNamedLWLockTranche(
        kReusableMembershipTrancheName,
        reusable_request->membership_lock_count);
    RequestNamedLWLockTranche(
        kReusableExactTrancheName,
        reusable_request->exact_partition_lock_count);
    RequestNamedLWLockTranche(
        kReusableCoverageTrancheName,
        reusable_request->coverage_partition_lock_count);
    storage::request_storage_resources();
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
        initialize_shared_state(*g_shared);
    }
    g_registry_lock = &GetNamedLWLockTranche(kRegistryTrancheName)[0].lock;
    const std::uint64_t boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce);
    const auto reusable_request = reusable_registry_preload_request(
        reusable_registry_config());
    if (!reusable_request.has_value()) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    bool reusable_found = false;
    void* reusable_region = ShmemInitStruct(
        kReusableSharedName,
        reusable_request->layout.total_bytes,
        &reusable_found);
    if (reusable_region == nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    auto reusable_started = startup_reusable_registry(
        std::span<std::byte>{
            static_cast<std::byte*>(reusable_region),
            reusable_request->layout.total_bytes},
        boot_nonce,
        reusable_request->config,
        reusable_found);
    if (!reusable_started.has_value()) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    g_reusable_registry = *reusable_started;
    g_reusable_membership_lock =
        &GetNamedLWLockTranche(kReusableMembershipTrancheName)[0].lock;
    g_reusable_exact_locks =
        GetNamedLWLockTranche(kReusableExactTrancheName);
    g_reusable_coverage_locks =
        GetNamedLWLockTranche(kReusableCoverageTrancheName);
    if (!storage::startup_storage(boot_nonce)) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    if (maintenance::static_janitor_registered()) {
        storage::publish_worker_lifecycle(
            storage::WorkerLifecycle::Registered, 0);
    }
    if (!storage::publish_correctness_readiness(
            CacheCorrectnessReadiness::GenericMvccReady)) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
    }
}

/** True only when preload installed and the shared ABI is usable. */
[[nodiscard]] bool shared_cache_ready() noexcept {
    const bool header_ready = g_preload_requested
        && g_shared != nullptr
        && g_shared->magic == kSharedMagic
        && g_shared->shared_abi_version == kSharedAbiVersion
        && pg_atomic_read_u32(&g_shared->ready) != 0
        && pg_atomic_read_u32(&g_shared->bootstrap_failure) == 0;
    if (!header_ready) {
        return false;
    }
    if (!storage::storage_ready() && !storage::attach_storage()) {
        return false;
    }
    const CacheCorrectnessReadiness required_readiness =
        g_query_cache_mode == static_cast<int>(QueryCacheMode::Reuse)
        ? CacheCorrectnessReadiness::ReusableCoverageReady
        : CacheCorrectnessReadiness::GenericMvccReady;
    return storage::runtime_snapshot().correctness_readiness
            >= required_readiness
        && (required_readiness
                != CacheCorrectnessReadiness::ReusableCoverageReady
            || (g_reusable_registry
                && g_reusable_membership_lock != nullptr
                && g_reusable_exact_locks != nullptr
                && g_reusable_coverage_locks != nullptr));
}

[[nodiscard]] bool cache_interrupt_error(int sqlstate) noexcept {
    return sqlstate == ERRCODE_QUERY_CANCELED
        || sqlstate == ERRCODE_ADMIN_SHUTDOWN
        || sqlstate == ERRCODE_CRASH_SHUTDOWN
        || ProcDiePending;
}

inline constexpr std::uint32_t kCoverageDescriptorMagicV1 = 0x42434431U;
inline constexpr std::uint32_t kCoverageDescriptorFormatV1 = 1;

/** Pointer-free persisted provider contract stored in the reusable registry. */
struct CoverageDescriptorHeaderV1 {
    std::uint32_t magic = kCoverageDescriptorMagicV1;
    std::uint32_t format_version = kCoverageDescriptorFormatV1;
    std::uint32_t provider_id = 0;
    std::uint32_t flags = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    std::uint32_t boundary_count = 0;
    std::uint32_t lineage_count = 0;
    std::uint32_t required_boundary_count = 0;
    std::uint32_t required_lineage_count = 0;
    std::uint32_t predicate_bytes = 0;
    std::uint32_t dependency_node_bytes = 0;
    std::uint32_t provider_private_node_bytes = 0;
    std::uint32_t reserved = 0;
};

static_assert(std::is_standard_layout_v<CoverageDescriptorHeaderV1>);
static_assert(std::is_trivially_copyable_v<CoverageDescriptorHeaderV1>);

/** Planner-owned reconstruction of one pointer-free coverage descriptor. */
struct PlannerDecodedCoverageV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePathDomainV1 domain{};
    BioseqCacheReusableSourceV1 source{};
};

[[nodiscard]] bool add_coverage_descriptor_size(
        std::uint64_t& total, std::uint64_t count,
        std::uint64_t width) noexcept {
    if (width != 0
        && count > (std::numeric_limits<std::uint64_t>::max)() / width) {
        return false;
    }
    const std::uint64_t bytes = count * width;
    if (total > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
        return false;
    }
    total += bytes;
    return total <= kMaxProviderRecipeBytes;
}

[[nodiscard]] bool coverage_descriptor_size(
        const CoverageDescriptorHeaderV1& header,
        std::uint64_t* total) noexcept {
    if (total == nullptr
        || header.magic != kCoverageDescriptorMagicV1
        || header.format_version != kCoverageDescriptorFormatV1
        || header.provider_id == 0 || header.flags != 0
        || header.compatibility_family_id == 0 || header.reserved != 0
        || header.boundary_count > kMaxProviderBoundaryColumns
        || header.lineage_count > kMaxProviderBoundaryColumns
        || header.required_boundary_count > kMaxProviderBoundaryColumns
        || header.required_lineage_count > kMaxProviderBoundaryColumns
        || header.predicate_bytes == 0) {
        return false;
    }
    std::uint64_t bytes = sizeof(CoverageDescriptorHeaderV1);
    if (!add_coverage_descriptor_size(
            bytes, header.boundary_count,
            sizeof(BioseqCacheColumnSchemaV1))
        || !add_coverage_descriptor_size(
            bytes, header.lineage_count,
            sizeof(BioseqCacheColumnSchemaV1))
        || !add_coverage_descriptor_size(
            bytes, header.required_boundary_count,
            sizeof(std::uint32_t))
        || !add_coverage_descriptor_size(
            bytes, header.required_lineage_count,
            sizeof(std::uint32_t))
        || !add_coverage_descriptor_size(
            bytes, 1, header.predicate_bytes)
        || !add_coverage_descriptor_size(
            bytes, 1, header.dependency_node_bytes)
        || !add_coverage_descriptor_size(
            bytes, 1, header.provider_private_node_bytes)) {
        return false;
    }
    *total = bytes;
    return true;
}

/** Serialize validated provider POD and Nodes without retaining any pointer. */
[[nodiscard]] bytea* serialize_coverage_descriptor_fail_open(
        const BioseqCachePathDomainV1& domain,
        MemoryContext planner_context) noexcept {
    if (planner_context == nullptr
        || !reuse::validate_path_domain(
                domain.provider_id, &domain).has_value()) {
        return nullptr;
    }
    MemoryContext old_context = CurrentMemoryContext;
    volatile bytea* accepted = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        char* predicate = nodeToString(domain.coverage_predicate);
        char* dependencies = nodeToString(domain.dependency_nodes);
        char* provider_private = nodeToString(domain.provider_private_nodes);
        if (predicate == nullptr || dependencies == nullptr
            || provider_private == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache provider descriptor is not serializable")));
        }
        const std::size_t predicate_bytes = std::strlen(predicate);
        const std::size_t dependency_bytes = std::strlen(dependencies);
        const std::size_t provider_private_bytes =
            std::strlen(provider_private);
        if (predicate_bytes > (std::numeric_limits<std::uint32_t>::max)()
            || dependency_bytes
                > (std::numeric_limits<std::uint32_t>::max)()
            || provider_private_bytes
                > (std::numeric_limits<std::uint32_t>::max)()) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq cache provider descriptor is too large")));
        }
        const CoverageDescriptorHeaderV1 header{
            .provider_id = domain.provider_id,
            .compatibility_family_id = domain.compatibility_family_id,
            .capability_mask = domain.capability_mask,
            .boundary_count = static_cast<std::uint32_t>(
                domain.boundary_column_count),
            .lineage_count = static_cast<std::uint32_t>(
                domain.lineage_column_count),
            .required_boundary_count = static_cast<std::uint32_t>(
                domain.required_boundary_attno_count),
            .required_lineage_count = static_cast<std::uint32_t>(
                domain.required_lineage_attno_count),
            .predicate_bytes = static_cast<std::uint32_t>(predicate_bytes),
            .dependency_node_bytes = static_cast<std::uint32_t>(
                dependency_bytes),
            .provider_private_node_bytes = static_cast<std::uint32_t>(
                provider_private_bytes),
        };
        std::uint64_t raw_size = 0;
        if (!coverage_descriptor_size(header, &raw_size)
            || raw_size > static_cast<std::uint64_t>(MaxAllocSize - VARHDRSZ)) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq cache provider descriptor exceeds its bound")));
        }
        auto* descriptor = static_cast<bytea*>(
            palloc(VARHDRSZ + static_cast<std::size_t>(raw_size)));
        SET_VARSIZE(
            descriptor, VARHDRSZ + static_cast<std::size_t>(raw_size));
        std::byte* destination = reinterpret_cast<std::byte*>(
            VARDATA(descriptor));
        std::size_t offset = 0;
        const auto append = [&](const void* source, std::size_t size) {
            if (size != 0) {
                std::memcpy(destination + offset, source, size);
                offset += size;
            }
        };
        append(&header, sizeof(header));
        append(
            domain.boundary_columns,
            domain.boundary_column_count
                * sizeof(BioseqCacheColumnSchemaV1));
        append(
            domain.lineage_columns,
            domain.lineage_column_count
                * sizeof(BioseqCacheColumnSchemaV1));
        append(
            domain.required_boundary_attnos,
            domain.required_boundary_attno_count * sizeof(std::uint32_t));
        append(
            domain.required_lineage_attnos,
            domain.required_lineage_attno_count * sizeof(std::uint32_t));
        append(predicate, predicate_bytes);
        append(dependencies, dependency_bytes);
        append(provider_private, provider_private_bytes);
        if (offset != raw_size) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache provider descriptor size changed")));
        }
        accepted = descriptor;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = nullptr;
    }
    PG_END_TRY();
    return const_cast<bytea*>(accepted);
}

[[nodiscard]] void* parse_coverage_node_field(
        std::span<const std::byte> bytes,
        std::size_t& offset,
        std::uint32_t length) {
    if (length == 0 || offset > bytes.size()
        || length > bytes.size() - offset
        || std::memchr(bytes.data() + offset, 0, length) != nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq cache coverage descriptor node is malformed")));
    }
    auto* serialized = static_cast<char*>(palloc(length + 1U));
    std::memcpy(serialized, bytes.data() + offset, length);
    serialized[length] = '\0';
    offset += length;
    return stringToNode(serialized);
}

/** Reconstruct one source in disposable planner memory before releasing pin. */
[[nodiscard]] bool decode_coverage_descriptor_fail_open(
        std::span<const std::byte> bytes,
        MemoryContext planner_context,
        PlannerDecodedCoverageV1* output) noexcept {
    if (planner_context == nullptr || output == nullptr
        || bytes.size() < sizeof(CoverageDescriptorHeaderV1)
        || bytes.size() > kMaxProviderRecipeBytes) {
        return false;
    }
    CoverageDescriptorHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::uint64_t expected_size = 0;
    if (!coverage_descriptor_size(header, &expected_size)
        || expected_size != bytes.size()) {
        return false;
    }

    MemoryContext old_context = CurrentMemoryContext;
    volatile MemoryContext child_context = nullptr;
    volatile PlannerDecodedCoverageV1* accepted = nullptr;
    PG_TRY();
    {
        child_context = AllocSetContextCreate(
            planner_context,
            "bioseq cache coverage descriptor",
            ALLOCSET_SMALL_SIZES);
        MemoryContextSwitchTo(child_context);
        auto* decoded = static_cast<PlannerDecodedCoverageV1*>(
            palloc0(sizeof(PlannerDecodedCoverageV1)));
        std::size_t offset = sizeof(CoverageDescriptorHeaderV1);
        auto copy_array = [&](std::size_t count, std::size_t width) -> void* {
            if (count == 0) {
                return nullptr;
            }
            const std::size_t size = count * width;
            if (offset > bytes.size() || size > bytes.size() - offset) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq cache coverage descriptor is truncated")));
            }
            void* copy = palloc(size);
            std::memcpy(copy, bytes.data() + offset, size);
            offset += size;
            return copy;
        };
        auto* boundary = static_cast<BioseqCacheColumnSchemaV1*>(copy_array(
            header.boundary_count, sizeof(BioseqCacheColumnSchemaV1)));
        auto* lineage = static_cast<BioseqCacheColumnSchemaV1*>(copy_array(
            header.lineage_count, sizeof(BioseqCacheColumnSchemaV1)));
        auto* required_boundary = static_cast<std::uint32_t*>(copy_array(
            header.required_boundary_count, sizeof(std::uint32_t)));
        auto* required_lineage = static_cast<std::uint32_t*>(copy_array(
            header.required_lineage_count, sizeof(std::uint32_t)));
        void* predicate = parse_coverage_node_field(
            bytes, offset, header.predicate_bytes);
        void* dependencies = parse_coverage_node_field(
            bytes, offset, header.dependency_node_bytes);
        void* provider_private = parse_coverage_node_field(
            bytes, offset, header.provider_private_node_bytes);
        if (offset != bytes.size() || predicate == nullptr
            || (dependencies != nullptr && !IsA(dependencies, List))
            || (provider_private != nullptr && !IsA(provider_private, List))) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache coverage descriptor has invalid Nodes")));
        }
        decoded->owner_context = child_context;
        decoded->domain = BioseqCachePathDomainV1{
            .struct_size = sizeof(BioseqCachePathDomainV1),
            .provider_id = header.provider_id,
            .compatibility_family_id = header.compatibility_family_id,
            .capability_mask = header.capability_mask,
            .coverage_predicate = static_cast<Expr*>(predicate),
            .boundary_columns = boundary,
            .boundary_column_count = header.boundary_count,
            .lineage_columns = lineage,
            .lineage_column_count = header.lineage_count,
            .required_boundary_attnos = required_boundary,
            .required_boundary_attno_count = header.required_boundary_count,
            .required_lineage_attnos = required_lineage,
            .required_lineage_attno_count = header.required_lineage_count,
            .dependency_nodes = static_cast<List*>(dependencies),
            .provider_private_nodes = static_cast<List*>(provider_private),
        };
        decoded->source = BioseqCacheReusableSourceV1{
            .struct_size = sizeof(BioseqCacheReusableSourceV1),
            .provider_id = header.provider_id,
            .compatibility_family_id = header.compatibility_family_id,
            .capability_mask = header.capability_mask,
            .boundary_columns = boundary,
            .boundary_column_count = header.boundary_count,
            .lineage_columns = lineage,
            .lineage_column_count = header.lineage_count,
            .provider_private_nodes = static_cast<List*>(provider_private),
        };
        if (!reuse::validate_path_domain(
                header.provider_id, &decoded->domain).has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache coverage descriptor contract is invalid")));
        }
        accepted = decoded;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            if (child_context != nullptr) {
                MemoryContextDelete(child_context);
            }
            PG_RE_THROW();
        }
        FlushErrorState();
        if (child_context != nullptr) {
            MemoryContextDelete(child_context);
        }
        child_context = nullptr;
        accepted = nullptr;
    }
    PG_END_TRY();
    if (accepted == nullptr) {
        return false;
    }
    *output = *const_cast<PlannerDecodedCoverageV1*>(accepted);
    return true;
}

struct PlannerSemanticKeysV1 {
    bytea* canonical_domain_key = nullptr;
    bytea* canonical_generation_key = nullptr;
    CacheDigest256 domain_digest{};
    CacheDigest256 generation_digest{};
};

/** Capture both provider-defined canonical keys and their SHA-256 directories. */
[[nodiscard]] bool capture_planner_semantic_keys_fail_open(
        const provider_registry::ProviderV1* provider,
        const BioseqCachePathDomainV1& domain,
        MemoryContext planner_context,
        PlannerSemanticKeysV1* output) noexcept {
    if (provider == nullptr || planner_context == nullptr || output == nullptr) {
        return false;
    }
    MemoryContext old_context = CurrentMemoryContext;
    volatile PlannerSemanticKeysV1* accepted = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        auto* keys = static_cast<PlannerSemanticKeysV1*>(
            palloc0(sizeof(PlannerSemanticKeysV1)));
        auto* domain_key = static_cast<bytea*>(palloc(
            VARHDRSZ + static_cast<std::size_t>(kMaxCoverageDomainKeyBytes)));
        auto* generation_key = static_cast<bytea*>(palloc(
            VARHDRSZ + static_cast<std::size_t>(kMaxCanonicalKeyBytes)));
        std::size_t domain_size = 0;
        std::size_t generation_size = 0;
        reuse::ProviderCallDiagnosticV1 diagnostic{};
        if (!reuse::invoke_semantic_digest_fail_open(
                provider,
                &domain,
                reuse::SemanticDigestKindV1::Domain,
                std::span<std::byte>{
                    reinterpret_cast<std::byte*>(VARDATA(domain_key)),
                    static_cast<std::size_t>(kMaxCoverageDomainKeyBytes)},
                &domain_size,
                planner_context,
                &diagnostic)
            || !reuse::invoke_semantic_digest_fail_open(
                provider,
                &domain,
                reuse::SemanticDigestKindV1::Generation,
                std::span<std::byte>{
                    reinterpret_cast<std::byte*>(VARDATA(generation_key)),
                    static_cast<std::size_t>(kMaxCanonicalKeyBytes)},
                &generation_size,
                planner_context,
                &diagnostic)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache provider semantic key is unavailable")));
        }
        SET_VARSIZE(domain_key, VARHDRSZ + domain_size);
        SET_VARSIZE(generation_key, VARHDRSZ + generation_size);
        PostgreSqlColumnChecksumPolicy checksum_policy;
        const auto domain_digest = checksum_policy.sha256(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(VARDATA(domain_key)),
                domain_size});
        const auto generation_digest = checksum_policy.sha256(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(VARDATA(generation_key)),
                generation_size});
        if (!domain_digest.has_value() || !generation_digest.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq cache semantic digest is unavailable")));
        }
        keys->canonical_domain_key = domain_key;
        keys->canonical_generation_key = generation_key;
        keys->domain_digest = *domain_digest;
        keys->generation_digest = *generation_digest;
#if defined(BIOSEQ_CACHE_TESTING)
        if (g_forced_coverage_domain_digest_active) {
            keys->domain_digest = g_forced_coverage_domain_digest;
        }
#endif
        accepted = keys;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = nullptr;
    }
    PG_END_TRY();
    if (accepted == nullptr) {
        return false;
    }
    *output = *const_cast<PlannerSemanticKeysV1*>(accepted);
    return true;
}

[[nodiscard]] ReusableSecurityNamespaceV1
current_reusable_security_namespace() noexcept {
    Oid current_role = InvalidOid;
    int security_context = 0;
    GetUserIdAndSecContext(&current_role, &security_context);
    return ReusableSecurityNamespaceV1{
        .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
        .current_role_oid = static_cast<std::uint32_t>(current_role),
        .session_user_oid = static_cast<std::uint32_t>(GetSessionUserId()),
        .security_context_flags = static_cast<std::uint32_t>(security_context),
        .row_security_mode = row_security ? 1 : 0,
        .provider_security_digest = {},
    };
}

[[nodiscard]] reuse::CoverageSecurityNamespaceV1
planner_security_namespace(
        const ReusableSecurityNamespaceV1& source) noexcept {
    return reuse::CoverageSecurityNamespaceV1{
        .database_oid = source.database_oid,
        .current_role_oid = source.current_role_oid,
        .session_user_oid = source.session_user_oid,
        .security_context_flags = source.security_context_flags,
        .row_security_mode = static_cast<std::uint8_t>(
            source.row_security_mode != 0),
        .provider_security_digest = source.provider_security_digest,
    };
}

inline constexpr std::uint32_t kCoverageCurrentPlanMagicV1 = 0x42435031U;

/** Fixed prefix of executor metadata embedded as one bytea Const Node. */
struct CoverageCurrentPlanHeaderV1 {
    std::uint32_t magic = kCoverageCurrentPlanMagicV1;
    std::uint32_t format_version = reuse::kReusablePlanFormatVersionV1;
    std::uint32_t provider_id = 0;
    std::uint32_t provider_abi_version = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    ReusableCoverageDirectoryKeyV1 coverage_directory_key{};
    CacheDigest256 generation_digest{};
    std::uint32_t canonical_domain_key_bytes = 0;
    std::uint32_t canonical_generation_key_bytes = 0;
    std::uint32_t coverage_descriptor_bytes = 0;
    std::uint32_t reserved = 0;
};

static_assert(std::is_standard_layout_v<CoverageCurrentPlanHeaderV1>);
static_assert(std::is_trivially_copyable_v<CoverageCurrentPlanHeaderV1>);

struct CoverageCurrentPlanViewV1 {
    CoverageCurrentPlanHeaderV1 header{};
    std::span<const std::byte> canonical_domain_key{};
    std::span<const std::byte> canonical_generation_key{};
    std::span<const std::byte> coverage_descriptor{};
};

[[nodiscard]] bool decode_current_coverage_plan(
        std::span<const std::byte> bytes,
        CoverageCurrentPlanViewV1* output) noexcept {
    if (output == nullptr || bytes.size() < sizeof(CoverageCurrentPlanHeaderV1)) {
        return false;
    }
    CoverageCurrentPlanHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kCoverageCurrentPlanMagicV1
        || header.format_version != reuse::kReusablePlanFormatVersionV1
        || header.provider_id == 0 || header.provider_abi_version == 0
        || header.compatibility_family_id == 0 || header.reserved != 0
        || header.canonical_domain_key_bytes == 0
        || header.canonical_generation_key_bytes == 0
        || header.coverage_descriptor_bytes == 0) {
        return false;
    }
    std::uint64_t expected = sizeof(header);
    if (!add_coverage_descriptor_size(
            expected, 1, header.canonical_domain_key_bytes)
        || !add_coverage_descriptor_size(
            expected, 1, header.canonical_generation_key_bytes)
        || !add_coverage_descriptor_size(
            expected, 1, header.coverage_descriptor_bytes)
        || expected != bytes.size()) {
        return false;
    }
    std::size_t offset = sizeof(header);
    const auto take = [&](std::uint32_t size) {
        const auto result = bytes.subspan(offset, size);
        offset += size;
        return result;
    };
    output->header = header;
    output->canonical_domain_key = take(header.canonical_domain_key_bytes);
    output->canonical_generation_key = take(
        header.canonical_generation_key_bytes);
    output->coverage_descriptor = take(header.coverage_descriptor_bytes);
    return offset == bytes.size()
        && output->header.coverage_directory_key.security_namespace
            == current_reusable_security_namespace();
}

[[nodiscard]] CacheDigest256 current_reusable_dependency_fingerprint() noexcept {
    struct DependencyEpochV1 {
        std::uint64_t database_epoch;
        std::uint64_t coarse_epoch;
    } epochs{
        .database_epoch = g_shared == nullptr
            ? 0
            : pg_atomic_read_u64(&g_shared->database_write_epoch),
        .coarse_epoch = g_shared == nullptr
            ? 0
            : pg_atomic_read_u64(&g_shared->coarse_dependency_epoch),
    };
    PostgreSqlColumnChecksumPolicy checksum;
    const auto digest = checksum.sha256(std::as_bytes(std::span{&epochs, 1U}));
    return digest.has_value() ? *digest : CacheDigest256{};
}

/** Membership mutations are centralized for lock-order and future owner tokens. */
[[nodiscard]] ReusableClaimResultV1 claim_current_reusable_generation(
        const ReusableClaimRequestV1& request) noexcept {
    ReusableClaimResultV1 result{};
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        result = claim_reusable_generation(
            g_reusable_registry,
            request,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableClaimResultV1{
            .error = ReusableRegistryErrorV1::InvalidState};
    }
    PG_END_TRY();
    return result;
}

[[nodiscard]] ReusableRegistryErrorV1 publish_current_reusable_generation(
        const ReusablePublishRequestV1& request) noexcept {
    ReusableRegistryErrorV1 result = ReusableRegistryErrorV1::InvalidState;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        result = publish_reusable_generation(
            g_reusable_registry,
            request,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableRegistryErrorV1::InvalidState;
    }
    PG_END_TRY();
    return result;
}

void fail_current_reusable_generation(
        const ReusableGenerationReferenceV1& reference,
        CacheFailureCode failure) noexcept {
    if (!reference || g_reusable_membership_lock == nullptr) {
        return;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        static_cast<void>(fail_reusable_generation(
            g_reusable_registry,
            reference,
            failure,
            reusable_partition_callbacks()));
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
    }
    PG_END_TRY();
}

[[nodiscard]] Const* make_binary_node(
        std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(MaxAllocSize)) {
        return nullptr;
    }
    auto* value = static_cast<bytea*>(palloc(VARHDRSZ + bytes.size()));
    SET_VARSIZE(value, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(value), bytes.data(), bytes.size());
    return makeConst(
        BYTEAOID,
        -1,
        InvalidOid,
        -1,
        PointerGetDatum(value),
        false,
        false);
}

[[nodiscard]] std::span<const std::byte> bytea_payload(
        const bytea* value) noexcept {
    if (value == nullptr || VARSIZE_ANY_EXHDR(value) == 0) {
        return {};
    }
    return std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(value)),
        static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value)),
    };
}

[[nodiscard]] bytea* copy_bytea_payload(
        std::span<const std::byte> bytes) {
    if (bytes.empty()
        || bytes.size() > static_cast<std::size_t>(MaxAllocSize - VARHDRSZ)) {
        return nullptr;
    }
    auto* result = static_cast<bytea*>(palloc(VARHDRSZ + bytes.size()));
    SET_VARSIZE(result, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(result), bytes.data(), bytes.size());
    return result;
}

/** Persist current exact-generation identity for executor-time claiming. */
[[nodiscard]] Const* make_current_coverage_plan_node(
        const provider_registry::ProviderV1& provider,
        const BioseqCachePathDomainV1& domain,
        const PlannerSemanticKeysV1& keys,
        const bytea* descriptor) {
    const std::span<const std::byte> domain_key =
        bytea_payload(keys.canonical_domain_key);
    const std::span<const std::byte> generation_key =
        bytea_payload(keys.canonical_generation_key);
    const std::span<const std::byte> descriptor_bytes =
        bytea_payload(descriptor);
    if (domain_key.empty() || generation_key.empty()
        || descriptor_bytes.empty()
        || domain_key.size() > (std::numeric_limits<std::uint32_t>::max)()
        || generation_key.size() > (std::numeric_limits<std::uint32_t>::max)()
        || descriptor_bytes.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
    }
    const CoverageCurrentPlanHeaderV1 header{
        .provider_id = provider.provider_id,
        .provider_abi_version = provider.abi_version,
        .compatibility_family_id = domain.compatibility_family_id,
        .capability_mask = domain.capability_mask,
        .coverage_directory_key = {
            .security_namespace = current_reusable_security_namespace(),
            .coverage_domain_digest = keys.domain_digest,
        },
        .generation_digest = keys.generation_digest,
        .canonical_domain_key_bytes = static_cast<std::uint32_t>(
            domain_key.size()),
        .canonical_generation_key_bytes = static_cast<std::uint32_t>(
            generation_key.size()),
        .coverage_descriptor_bytes = static_cast<std::uint32_t>(
            descriptor_bytes.size()),
    };
    std::uint64_t total = sizeof(header);
    if (!add_coverage_descriptor_size(total, 1, domain_key.size())
        || !add_coverage_descriptor_size(total, 1, generation_key.size())
        || !add_coverage_descriptor_size(total, 1, descriptor_bytes.size())) {
        return nullptr;
    }
    auto* packed = static_cast<std::byte*>(palloc(total));
    std::size_t offset = 0;
    const auto append = [&](const void* source, std::size_t size) {
        std::memcpy(packed + offset, source, size);
        offset += size;
    };
    append(&header, sizeof(header));
    append(domain_key.data(), domain_key.size());
    append(generation_key.data(), generation_key.size());
    append(descriptor_bytes.data(), descriptor_bytes.size());
    return make_binary_node(
        std::span<const std::byte>{packed, static_cast<std::size_t>(total)});
}

inline constexpr std::uint32_t kCoverageMappingMagicV1 = 0x42434d31U;

struct CoverageMappingHeaderV1 {
    std::uint32_t magic = kCoverageMappingMagicV1;
    std::uint32_t format_version = reuse::kReusablePlanFormatVersionV1;
    std::uint32_t boundary_count = 0;
    std::uint32_t lineage_count = 0;
};

/** Persist validated source-column maps as POD; private Nodes stay separate. */
[[nodiscard]] Const* make_source_mapping_node(
        const BioseqCacheSourceMappingV1& mapping) {
    if (mapping.boundary_map_count > kMaxProviderBoundaryColumns
        || mapping.lineage_map_count > kMaxProviderBoundaryColumns
        || (mapping.boundary_map_count != 0
            && mapping.boundary_map == nullptr)
        || (mapping.lineage_map_count != 0
            && mapping.lineage_map == nullptr)) {
        return nullptr;
    }
    const CoverageMappingHeaderV1 header{
        .boundary_count = static_cast<std::uint32_t>(
            mapping.boundary_map_count),
        .lineage_count = static_cast<std::uint32_t>(
            mapping.lineage_map_count),
    };
    std::uint64_t total = sizeof(header);
    if (!add_coverage_descriptor_size(
            total, mapping.boundary_map_count,
            sizeof(BioseqCacheColumnMapEntryV1))
        || !add_coverage_descriptor_size(
            total, mapping.lineage_map_count,
            sizeof(BioseqCacheColumnMapEntryV1))) {
        return nullptr;
    }
    auto* packed = static_cast<std::byte*>(palloc(total));
    std::size_t offset = 0;
    std::memcpy(packed + offset, &header, sizeof(header));
    offset += sizeof(header);
    const std::size_t boundary_bytes = mapping.boundary_map_count
        * sizeof(BioseqCacheColumnMapEntryV1);
    const std::size_t lineage_bytes = mapping.lineage_map_count
        * sizeof(BioseqCacheColumnMapEntryV1);
    if (boundary_bytes != 0) {
        std::memcpy(packed + offset, mapping.boundary_map, boundary_bytes);
        offset += boundary_bytes;
    }
    if (lineage_bytes != 0) {
        std::memcpy(packed + offset, mapping.lineage_map, lineage_bytes);
        offset += lineage_bytes;
    }
    return make_binary_node(
        std::span<const std::byte>{packed, static_cast<std::size_t>(total)});
}

struct PlannerPinReleaseV1 {
    ReusableReferenceTokenV1 token{};
};

struct PlannerCandidateBytesV1 {
    bytea* canonical_domain_key = nullptr;
    bytea* canonical_generation_key = nullptr;
    bytea* coverage_descriptor = nullptr;
};

/** Planner-context callback target; idempotent token clearing prevents leaks. */
void release_planner_pin_noerror(
        void* opaque, const CacheEntryId&,
        reuse::CandidateReferenceKindV1 kind) noexcept {
    auto* owner = static_cast<PlannerPinReleaseV1*>(opaque);
    if (owner == nullptr
        || kind != reuse::CandidateReferenceKindV1::ProvisionalPin
        || !owner->token.active || !g_reusable_registry) {
        return;
    }
    static_cast<void>(release_reusable_reference(
        g_reusable_registry,
        owner->token,
        reusable_partition_callbacks()));
}

[[nodiscard]] const CustomPathMethods& coverage_path_methods();

/**
 * Build one coverage wrapper from a provider-owned path.  Candidate pins are
 * copied into planner-context cleanup records before any provider callback;
 * every callback then releases its pin after copying validated output.
 */
[[nodiscard]] bool wrap_one_provider_path(
        PlannerInfo* root,
        RelOptInfo* relation,
        Path* provider_path,
        const provider_registry::ProviderV1* provider,
        const BioseqCacheQueryIntentV1& intent,
        MemoryContext planner_context) {
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    reuse::PlannerOwnedDomainV1 described{};
    if (!reuse::invoke_describe_path_fail_open(
            provider,
            root,
            relation,
            provider_path,
            &intent,
            planner_context,
            &described,
            &diagnostic)) {
        return false;
    }
    PlannerSemanticKeysV1 keys{};
    if (!capture_planner_semantic_keys_fail_open(
            provider,
            described.value,
            planner_context,
            &keys)) {
        return false;
    }
    bytea* descriptor = serialize_coverage_descriptor_fail_open(
        described.value, planner_context);
    Const* current_plan = descriptor == nullptr
        ? nullptr
        : make_current_coverage_plan_node(
            *provider, described.value, keys, descriptor);
    if (current_plan == nullptr) {
        return false;
    }

    auto* release_owners = static_cast<PlannerPinReleaseV1*>(
        MemoryContextAllocZero(
            planner_context,
            sizeof(PlannerPinReleaseV1)
                * kMaxCoverageGenerationsPerDomain));
    auto* release_records = static_cast<
        reuse::PlannerCandidateCleanupRecordV1*>(MemoryContextAllocZero(
            planner_context,
            sizeof(reuse::PlannerCandidateCleanupRecordV1)
                * kMaxCoverageGenerationsPerDomain));
    for (std::size_t index = 0;
         index < kMaxCoverageGenerationsPerDomain;
         ++index) {
        if (reuse::register_candidate_context_cleanup(
                planner_context, &release_records[index]) == nullptr) {
            return false;
        }
    }

    const ReusableCandidateRequestV1 request{
        .coverage_directory_key = {
            .security_namespace = current_reusable_security_namespace(),
            .coverage_domain_digest = keys.domain_digest,
        },
        .canonical_domain_key = bytea_payload(keys.canonical_domain_key),
        .reference_owner = current_reusable_producer_identity(),
        .provider_id = provider->provider_id,
        .compatibility_family_id = described.value.compatibility_family_id,
        .required_capability_mask = described.value.capability_mask,
    };
    auto* candidate_bytes = static_cast<PlannerCandidateBytesV1*>(
        MemoryContextAllocZero(
            planner_context,
            sizeof(PlannerCandidateBytesV1)
                * kMaxCoverageGenerationsPerDomain));
    auto* candidates = static_cast<ReusableCandidateBatchV1*>(
        MemoryContextAllocZero(
            planner_context, sizeof(ReusableCandidateBatchV1)));
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool snapshot_valid = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        auto candidate_result = snapshot_reusable_candidates(
            g_reusable_registry, request, reusable_partition_callbacks());
        if (candidate_result.has_value()) {
            *candidates = *candidate_result;
            snapshot_valid = true;
        }
        for (std::size_t index = 0;
             snapshot_valid && index < candidates->candidate_count;
             ++index) {
            release_owners[index].token =
                candidates->candidates[index].planner_pin;
            candidates->candidates[index].planner_pin.active = false;
            if (!reuse::activate_candidate_reference(
                    release_records[index],
                    candidates->candidates[index].generation.reference.id,
                    reuse::CandidateReferenceKindV1::ProvisionalPin,
                    &release_planner_pin_noerror,
                    &release_owners[index])) {
                static_cast<void>(release_reusable_reference(
                    g_reusable_registry,
                    release_owners[index].token,
                    reusable_partition_callbacks()));
                snapshot_valid = false;
                break;
            }
        }
        MemoryContextSwitchTo(planner_context);
        for (std::size_t index = 0;
             snapshot_valid && index < candidates->candidate_count;
             ++index) {
            const ReusableGenerationSnapshotV1& generation =
                candidates->candidates[index].generation;
            candidate_bytes[index].canonical_domain_key = copy_bytea_payload(
                reusable_shared_bytes(
                    g_reusable_registry,
                    generation.canonical_domain_key));
            candidate_bytes[index].canonical_generation_key =
                copy_bytea_payload(reusable_shared_bytes(
                    g_reusable_registry,
                    generation.canonical_generation_key));
            candidate_bytes[index].coverage_descriptor = copy_bytea_payload(
                reusable_shared_bytes(
                    g_reusable_registry,
                    generation.coverage_descriptor));
            snapshot_valid =
                candidate_bytes[index].canonical_domain_key != nullptr
                && candidate_bytes[index].canonical_generation_key != nullptr
                && candidate_bytes[index].coverage_descriptor != nullptr;
        }
        MemoryContextSwitchTo(caller_context);
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        for (std::size_t index = 0;
             index < candidates->candidate_count;
             ++index) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
        }
        static_cast<void>(release_reusable_candidate_batch(
            g_reusable_registry,
            *candidates,
            reusable_partition_callbacks()));
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        snapshot_valid = false;
    }
    PG_END_TRY();
    if (!snapshot_valid) {
        for (std::size_t index = 0;
             index < candidates->candidate_count;
             ++index) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
        }
        static_cast<void>(release_reusable_candidate_batch(
            g_reusable_registry,
            *candidates,
            reusable_partition_callbacks()));
        return false;
    }

    std::array<reuse::CoverageCandidateViewV1,
        kMaxCoverageGenerationsPerDomain> candidate_views{};
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        const ReusableCandidateSnapshotV1& candidate =
            candidates->candidates[index];
        const auto domain_bytes = bytea_payload(
            candidate_bytes[index].canonical_domain_key);
        const auto generation_bytes = bytea_payload(
            candidate_bytes[index].canonical_generation_key);
        const bool is_current_exact = reuse::exact_current_key_equal(
            keys.generation_digest,
            bytea_payload(keys.canonical_generation_key),
            candidate.generation.reference.id.digest,
            generation_bytes);
        candidate_views[index] = reuse::CoverageCandidateViewV1{
            .entry_id = candidate.generation.reference.id,
            .domain_digest = candidate.generation.coverage_directory_key
                .coverage_domain_digest,
            .canonical_domain_key = domain_bytes,
            .canonical_generation_key = generation_bytes,
            .security_namespace = planner_security_namespace(
                candidate.generation.coverage_directory_key
                    .security_namespace),
            .provider_id = candidate.generation.metadata.provider_id,
            .compatibility_family_id = candidate.generation.metadata
                .compatibility_family_id,
            .capability_mask = candidate.generation.metadata.capability_mask,
            .complete = candidate.complete && !is_current_exact,
            .in_flight = candidate.in_flight && !is_current_exact,
            .estimated_covered_rows = candidate.generation.metadata
                .estimated_covered_rows,
            .stored_bytes = candidate.generation.metadata.stored_bytes,
        };
    }
    const reuse::CoverageCandidateProbeV1 probe{
        .domain_digest = keys.domain_digest,
        .canonical_domain_key = bytea_payload(keys.canonical_domain_key),
        .security_namespace = planner_security_namespace(
            request.coverage_directory_key.security_namespace),
        .provider_id = provider->provider_id,
        .compatibility_family_id = described.value.compatibility_family_id,
        .required_capability_mask = described.value.capability_mask,
    };
    const auto selected = reuse::select_candidates(
        probe,
        std::span<const reuse::CoverageCandidateViewV1>{
            candidate_views.data(), candidates->candidate_count});
    std::array<bool, kMaxCoverageGenerationsPerDomain> selected_mask{};
    for (std::size_t index = 0; index < selected.selected_count; ++index) {
        selected_mask[selected.indices[index]] = true;
    }

    List* gate_expressions = NIL;
    List* candidate_private = NIL;
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        if (!selected_mask[index]) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        const ReusableCandidateSnapshotV1& candidate =
            candidates->candidates[index];
        const auto descriptor_bytes = bytea_payload(
            candidate_bytes[index].coverage_descriptor);
        PlannerDecodedCoverageV1 decoded{};
        if (!decode_coverage_descriptor_fail_open(
                descriptor_bytes, planner_context, &decoded)) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        Const* reference_node = make_binary_node(
            std::as_bytes(std::span{
                &candidate.generation.reference,
                std::size_t{1}}));
        Const* generation_key_node = make_binary_node(
            bytea_payload(candidate_bytes[index].canonical_generation_key));
        Const* descriptor_node = make_binary_node(descriptor_bytes);
        reuse::PlannerGateNodesV1 gate{};
        if (reference_node == nullptr || generation_key_node == nullptr
            || descriptor_node == nullptr
            || !reuse::build_planner_gate_nodes(
                root, decoded.domain.coverage_predicate, &gate)) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        reuse::PlannerOwnedSourceMappingV1 mapping{};
        if (!reuse::invoke_map_source_fail_open(
                provider,
                root,
                &described.value,
                &decoded.source,
                planner_context,
                &mapping,
                &diagnostic,
                &release_records[index])) {
            continue;
        }
        Const* mapping_node = make_source_mapping_node(mapping.value);
        if (mapping_node == nullptr) {
            continue;
        }
        List* source_private = list_make5(
            reference_node,
            generation_key_node,
            descriptor_node,
            mapping_node,
            copyObject(mapping.value.provider_private_nodes));
        source_private = lappend(
            source_private,
            makeInteger(gate.param_id));
        source_private = lappend(
            source_private,
            copyObject(decoded.domain.coverage_predicate));
        candidate_private = lappend(candidate_private, source_private);
        gate_expressions = lappend(
            gate_expressions, copyObject(gate.residual_gate));
    }
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        reuse::release_candidate_reference_noerror(release_records[index]);
    }

    reuse::PlannerOwnedDecoratedPathV1 decorated{};
    if (!reuse::invoke_decorate_path_fail_open(
            provider,
            root,
            relation,
            provider_path,
            &described.value,
            gate_expressions,
            planner_context,
            &decorated,
            &diagnostic)) {
        return false;
    }
    List* private_nodes = list_make5(
        makeInteger(reuse::kReusablePlanFormatVersionV1),
        makeInteger(static_cast<std::int32_t>(provider->provider_id)),
        current_plan,
        candidate_private,
        copyObject(described.value.coverage_predicate));
    const Cost overhead = cpu_tuple_cost
        * (provider_path->rows > 0 ? provider_path->rows : 1.0)
        * 0.0001;
    CustomPath* wrapper = reuse::make_coverage_cache_path(
        relation,
        provider_path,
        decorated.value.path,
        private_nodes,
        &coverage_path_methods(),
        reuse::CoveragePathCostsV1{
            .startup_cost = decorated.value.path->startup_cost,
            .total_cost = decorated.value.path->total_cost + overhead,
        });
    return wrapper != nullptr
        && reuse::replace_provider_path(relation, provider_path, wrapper);
}

/** Discover, map, decorate, and replace paths owned by one provider bridge. */
void consider_provider_reusable_paths_impl(
        PlannerInfo* root,
        RelOptInfo* relation,
        unsigned int range_table_index,
        RangeTblEntry* range_entry,
        const provider_registry::ProviderV1* provider) noexcept {
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr || !frame->eligible || root == nullptr
        || relation == nullptr || range_entry == nullptr
        || provider == nullptr || relation->pathlist == NIL
        || root->parse != frame->delegated_query
        || range_table_index == 0
        || range_table_index > static_cast<unsigned int>(
            list_length(root->parse->rtable))) {
        return;
    }
    const BioseqCacheQueryIntentV1* intent = nullptr;
    for (std::size_t index = 0; index < frame->provider_intent_count; ++index) {
        if (frame->providers[index] == provider) {
            intent = &frame->intents[index].value;
            break;
        }
    }
    if (intent == nullptr) {
        return;
    }
    MemoryContext planner_context = root->planner_cxt != nullptr
        ? root->planner_cxt
        : CurrentMemoryContext;
    List* original_paths = list_copy(relation->pathlist);
    ListCell* cell = nullptr;
    foreach(cell, original_paths) {
        Path* path = static_cast<Path*>(lfirst(cell));
        if (path != nullptr && list_member_ptr(relation->pathlist, path)) {
            static_cast<void>(wrap_one_provider_path(
                root,
                relation,
                path,
                provider,
                *intent,
                planner_context));
        }
    }
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

/** Aggregates are immutable only when every executor support function is. */
[[nodiscard]] bool aggregate_has_mutable_support(const Aggref& aggregate) {
    HeapTuple tuple = SearchSysCache1(
        AGGFNOID, ObjectIdGetDatum(aggregate.aggfnoid));
    if (!HeapTupleIsValid(tuple)) {
        return true;
    }
    const auto* metadata = reinterpret_cast<const FormData_pg_aggregate*>(
        GETSTRUCT(tuple));
    const std::array<Oid, 8> functions{
        metadata->aggtransfn,
        metadata->aggfinalfn,
        metadata->aggcombinefn,
        metadata->aggserialfn,
        metadata->aggdeserialfn,
        metadata->aggmtransfn,
        metadata->aggminvtransfn,
        metadata->aggmfinalfn,
    };
    bool mutable_support = false;
    for (const Oid function_oid : functions) {
        mutable_support = mutable_support
            || (OidIsValid(function_oid)
                && func_volatile(function_oid) != PROVOLATILE_IMMUTABLE);
    }
    ReleaseSysCache(tuple);
    return mutable_support;
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
    if (IsA(node, Aggref)
        && aggregate_has_mutable_support(
            *reinterpret_cast<const Aggref*>(node))) {
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

struct RegistryReaderClaim {
    std::uint32_t slot =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t registration_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < storage::kReaderPinsPerManifest
            && registration_sequence != 0;
    }
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
    bool manifest_pin_owned;
    bool manifest_waiter_owned;
    bool registry_reader_owned;
    RuntimeRole runtime_role;
    int registry_index;
    std::uint64_t generation;
    std::uint64_t row_count;
    std::uint64_t payload_bytes;
    std::uint64_t logical_bytes;
    RegistryReaderClaim registry_reader_claim;
    CacheEntryId entry_id;
    storage::ManifestHandle manifest_handle;
    storage::ManifestWaiterToken manifest_waiter_token;
    storage::ManifestPin manifest_pin;
    std::array<storage::StoredBlockToken,
        storage::kBlocksPerManifest> stored_tokens;
    std::uint32_t stored_token_count;
    ColumnStoreBuilder* column_builder;
    Oid* send_functions;
    Tuplestorestate* replay_store;
    TupleTableSlot* replay_slot;
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

/** Read the post-setrefs fail-open bit; malformed metadata always bypasses. */
[[nodiscard]] bool private_force_bypass(const CustomScan& scan) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= 2) {
        return true;
    }
    const Node* marker = static_cast<const Node*>(
        list_nth(scan.custom_private, 2));
    return marker == nullptr || !IsA(marker, Integer) || intVal(marker) != 0;
}

/** Mutate only the preallocated scalar; never reshape a finalized Plan. */
[[nodiscard]] bool set_private_force_bypass(
        CustomScan& scan, bool force_bypass) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= 2) {
        return false;
    }
    Node* marker = static_cast<Node*>(list_nth(scan.custom_private, 2));
    if (marker == nullptr || !IsA(marker, Integer)) {
        return false;
    }
    reinterpret_cast<Integer*>(marker)->ival = force_bypass ? 1 : 0;
    return true;
}

/** Count one typed private list; -1 identifies malformed finalized metadata. */
[[nodiscard]] int private_list_length(
        const CustomScan& scan, int index, NodeTag expected_tag) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= index) {
        return -1;
    }
    const auto* values = static_cast<const List*>(
        list_nth(scan.custom_private, index));
    if (values == NIL) {
        return 0;
    }
    return nodeTag(values) == expected_tag ? list_length(values) : -1;
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

#if defined(BIOSEQ_CACHE_TESTING)
[[nodiscard]] constexpr std::uint32_t pause_bit(
        TestPausePoint point) noexcept {
    return 1U << static_cast<std::uint32_t>(point);
}

[[nodiscard]] std::int64_t pause_lock_key(TestPausePoint point) noexcept {
    const std::uint64_t database = static_cast<std::uint64_t>(MyDatabaseId);
    const std::uint64_t ordinal = static_cast<std::uint32_t>(point) + 1U;
    return static_cast<std::int64_t>(
        0x4253514300000000ULL ^ (database << 8U) ^ ordinal);
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
    if (std::strcmp(name, "after_reader_pinned") == 0) {
        return TestPausePoint::AfterReaderPinned;
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
    volatile bool lock_acquired = false;
    PG_TRY();
    {
        /* A heavyweight advisory lock makes the pause visible to
         * pg_isolation_regress's blocked-session detector. */
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_lock_int8,
            Int64GetDatum(pause_lock_key(point))));
        lock_acquired = true;
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_unlock_int8,
            Int64GetDatum(pause_lock_key(point))));
        lock_acquired = false;
    }
    PG_CATCH();
    {
        if (lock_acquired) {
            static_cast<void>(DirectFunctionCall1(
                pg_advisory_unlock_int8,
                Int64GetDatum(pause_lock_key(point))));
        }
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
    AfterReaderPinned,
};
inline void maybe_pause(TestPausePoint) noexcept {}
#endif

struct RegistryDecision {
    RuntimeRole role = RuntimeRole::Bypass;
    int index = -1;
    std::uint64_t generation = 0;
    RegistryReaderClaim reader_claim{};
    CacheEntryId entry_id{};
    storage::ManifestHandle manifest_handle{};
    storage::ManifestWaiterToken manifest_waiter_token{};
};

struct RegistryWaiterClaim {
    std::uint32_t slot =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t registration_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < storage::kWaitersPerManifest
            && registration_sequence != 0;
    }
};

[[nodiscard]] CacheEntryId cache_entry_id(const CacheEntry& entry) noexcept {
    return CacheEntryId{
        .digest = entry.key_digest,
        .boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce),
        .generation = entry.generation,
        .claim_sequence = entry.claim_sequence,
    };
}

[[nodiscard]] std::uint64_t next_claim_sequence() noexcept {
    std::uint64_t sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_claim_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_claim_sequence, 1);
    }
    return sequence == 0 ? 1 : sequence;
}

[[nodiscard]] bool registry_owner_alive(
        int owner_pid,
        std::uint32_t owner_proc_number) noexcept {
    if (owner_pid <= 0
        || owner_proc_number
            >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    PGPROC* owner = ProcNumberGetProc(
        static_cast<ProcNumber>(owner_proc_number));
    return owner != nullptr && owner->pid == owner_pid;
}

void clear_registry_reader_locked(
        CacheEntry& entry,
        RegistryReaderOwner& reader) noexcept {
    if (!reader.active) {
        return;
    }
    reader = RegistryReaderOwner{};
    if (entry.reader_count > 0) {
        --entry.reader_count;
    }
}

void reap_dead_registry_readers_locked(CacheEntry& entry) noexcept {
    for (RegistryReaderOwner& reader : entry.readers) {
        if (reader.active
            && !registry_owner_alive(
                reader.owner_pid, reader.owner_proc_number)) {
            clear_registry_reader_locked(entry, reader);
        }
    }
}

[[nodiscard]] RegistryReaderClaim claim_registry_reader_locked(
        CacheEntry& entry) noexcept {
    for (std::uint32_t index = 0;
         index < storage::kReaderPinsPerManifest; ++index) {
        RegistryReaderOwner& reader = entry.readers[index];
        if (reader.active) {
            continue;
        }
        std::uint64_t sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_registry_reader_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(
                &g_shared->next_registry_reader_sequence, 1);
        }
        if (sequence == 0) {
            sequence = 1;
        }
        reader = RegistryReaderOwner{
            .active = true,
            .owner_pid = MyProcPid,
            .owner_proc_number = static_cast<std::uint32_t>(MyProcNumber),
            .registration_sequence = sequence,
        };
        ++entry.reader_count;
        return RegistryReaderClaim{
            .slot = index,
            .registration_sequence = sequence,
        };
    }
    return {};
}

void clear_registry_waiter_locked(
        CacheEntry& entry,
        RegistryWaiterOwner& waiter) noexcept {
    if (!waiter.active) {
        return;
    }
    waiter = RegistryWaiterOwner{};
    if (entry.waiter_count > 0) {
        --entry.waiter_count;
    }
}

void reap_dead_registry_waiters_locked(
        CacheEntry& entry,
        bool storage_attached) noexcept {
    for (RegistryWaiterOwner& waiter : entry.waiters) {
        if (!waiter.active
            || registry_owner_alive(
                waiter.owner_pid, waiter.owner_proc_number)) {
            continue;
        }
        if (waiter.storage_token) {
            if (!storage_attached) {
                continue;
            }
            static_cast<void>(storage::unregister_manifest_waiter(
                waiter.storage_token));
        }
        clear_registry_waiter_locked(entry, waiter);
    }
}

[[nodiscard]] RegistryWaiterClaim claim_registry_waiter_locked(
        CacheEntry& entry) noexcept {
    for (std::uint32_t index = 0;
         index < storage::kWaitersPerManifest; ++index) {
        RegistryWaiterOwner& waiter = entry.waiters[index];
        if (waiter.active) {
            continue;
        }
        std::uint64_t sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_registry_waiter_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(
                &g_shared->next_registry_waiter_sequence, 1);
        }
        if (sequence == 0) {
            sequence = 1;
        }
        waiter = RegistryWaiterOwner{
            .active = true,
            .owner_pid = MyProcPid,
            .owner_proc_number = static_cast<std::uint32_t>(MyProcNumber),
            .registration_sequence = sequence,
            .registered_at = GetCurrentTimestamp(),
        };
        ++entry.waiter_count;
        return RegistryWaiterClaim{
            .slot = index,
            .registration_sequence = sequence,
        };
    }
    return {};
}

[[nodiscard]] bool take_registry_waiter_locked(
        CacheEntry& entry,
        const RegistryWaiterClaim& claim,
        storage::ManifestWaiterToken& token) noexcept {
    if (!claim) {
        return false;
    }
    RegistryWaiterOwner& waiter = entry.waiters[claim.slot];
    if (!waiter.active
        || waiter.registration_sequence != claim.registration_sequence
        || waiter.owner_pid != MyProcPid
        || waiter.owner_proc_number
            != static_cast<std::uint32_t>(MyProcNumber)) {
        return false;
    }
    token = waiter.storage_token;
    clear_registry_waiter_locked(entry, waiter);
    return true;
}

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
        const CacheEntry& entry, std::uint64_t database_epoch,
        TimestampTz now) noexcept {
    return entry.state == SharedEntryState::Complete
        && static_cast<bool>(entry.manifest_handle)
        && entry.database_epoch == database_epoch
        && entry.expires_at > now;
}

/** Mark an abandoned producer failed while holding the registry lock. */
void fail_abandoned_producer(CacheEntry& entry) noexcept {
    entry.state = SharedEntryState::Failed;
    entry.producer_pid = 0;
    entry.producer_proc_number = INVALID_PROC_NUMBER;
    ConditionVariableBroadcast(&entry.changed);
}

/** Make one unreferenced terminal slot reusable without resetting its ABA id. */
void recycle_registry_entry_locked(CacheEntry& entry) noexcept {
    const std::uint64_t generation = entry.generation;
    entry.key_size = 0;
    entry.claim_sequence = 0;
    entry.key_hash = 0;
    entry.key_digest = {};
    entry.database_epoch = 0;
    entry.database_oid = InvalidOid;
    entry.current_role_oid = InvalidOid;
    entry.session_user_oid = InvalidOid;
    entry.producer_pid = 0;
    entry.producer_proc_number = INVALID_PROC_NUMBER;
    entry.waiter_count = 0;
    entry.reader_count = 0;
    std::memset(entry.readers, 0, sizeof(entry.readers));
    std::memset(entry.waiters, 0, sizeof(entry.waiters));
    entry.created_at = 0;
    entry.completed_at = 0;
    entry.expires_at = 0;
    entry.row_count = 0;
    entry.logical_bytes = 0;
    entry.stored_bytes = 0;
    entry.hits = 0;
    entry.relation_count = 0;
    entry.reserved = 0;
    std::memset(entry.relation_oids, 0, sizeof(entry.relation_oids));
    entry.manifest_handle = {};
    std::memset(entry.display_sql, 0, sizeof(entry.display_sql));
    std::memset(entry.key, 0, sizeof(entry.key));
    entry.generation = generation;
    pg_write_barrier();
    entry.state = SharedEntryState::Empty;
    ConditionVariableBroadcast(&entry.changed);
}

/** Wait for a matching producer; pre-existing waiters may consume TTL-zero. */
[[nodiscard]] RegistryDecision wait_for_producer(
        int index, std::uint64_t generation,
        const RegistryWaiterClaim& waiter_claim) {
    RegistryDecision decision{};
    CacheEntry& entry = g_shared->entries[index];
    storage::ManifestWaiterToken waiter_to_release{};
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
                if (entry.generation == generation) {
                    static_cast<void>(take_registry_waiter_locked(
                        entry, waiter_claim, waiter_to_release));
                }
                LWLockRelease(g_registry_lock);
                break;
            }
            if (entry.state == SharedEntryState::Complete) {
                storage::ManifestWaiterToken waiter_token{};
                const bool claimed = take_registry_waiter_locked(
                    entry, waiter_claim, waiter_token);
                if (claimed && waiter_token) {
                    const RegistryReaderClaim reader_claim =
                        claim_registry_reader_locked(entry);
                    if (reader_claim) {
                        ++entry.hits;
                        decision.role = RuntimeRole::Consumer;
                        decision.index = index;
                        decision.generation = generation;
                        decision.reader_claim = reader_claim;
                        decision.entry_id = cache_entry_id(entry);
                        decision.manifest_handle = entry.manifest_handle;
                        decision.manifest_waiter_token = waiter_token;
                    } else {
                        waiter_to_release = waiter_token;
                    }
                } else if (waiter_token) {
                    waiter_to_release = waiter_token;
                }
                LWLockRelease(g_registry_lock);
                break;
            }
            PGPROC* producer = entry.producer_proc_number == INVALID_PROC_NUMBER
                ? nullptr
                : ProcNumberGetProc(entry.producer_proc_number);
            if (entry.state == SharedEntryState::Producing
                && (producer == nullptr || producer->pid != entry.producer_pid)) {
                static_cast<void>(take_registry_waiter_locked(
                    entry, waiter_claim, waiter_to_release));
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
        if (entry.generation == generation) {
            static_cast<void>(take_registry_waiter_locked(
                entry, waiter_claim, waiter_to_release));
        }
        LWLockRelease(g_registry_lock);
        if (waiter_to_release) {
            static_cast<void>(
                storage::unregister_manifest_waiter(waiter_to_release));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (waiter_to_release) {
        static_cast<void>(
            storage::unregister_manifest_waiter(waiter_to_release));
    }
    return decision;
}

/** Select an exact hit, wait on its producer, or claim one bounded slot. */
[[nodiscard]] RegistryDecision acquire_registry_entry(
        const StringInfo key, std::uint64_t database_epoch,
        const char* display_sql, List* relation_oids) {
    RegistryDecision decision{};
    PostgreSqlColumnChecksumPolicy checksum_policy;
    const auto digest = checksum_policy.sha256(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(key->data),
        static_cast<std::size_t>(key->len),
    });
    if (!digest.has_value()) {
        return decision;
    }
    const std::uint64_t key_hash = hash_any_extended(
        reinterpret_cast<const unsigned char*>(key->data), key->len,
        pg_atomic_read_u64(&g_shared->boot_nonce));
    const TimestampTz now = GetCurrentTimestamp();
    int reusable_index = -1;
    int lru_index = -1;
    TimestampTz oldest_completion = DT_NOEND;
    TimestampTz oldest_live_completion = DT_NOEND;
    storage::ManifestHandle retired_manifest{};

    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t offset = 0; offset < kRegistryCapacity; ++offset) {
        CacheEntry& entry = g_shared->entries[offset];
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, true);
        if (entry_key_matches(entry, key_hash, key)) {
            if (entry_is_current(entry, database_epoch, now)) {
                const RegistryReaderClaim reader_claim =
                    claim_registry_reader_locked(entry);
                if (!reader_claim) {
                    LWLockRelease(g_registry_lock);
                    return decision;
                }
                ++entry.hits;
                decision.role = RuntimeRole::Consumer;
                decision.index = static_cast<int>(offset);
                decision.generation = entry.generation;
                decision.reader_claim = reader_claim;
                decision.entry_id = cache_entry_id(entry);
                decision.manifest_handle = entry.manifest_handle;
                LWLockRelease(g_registry_lock);
                return decision;
            }
            if (entry.state == SharedEntryState::Producing
                && entry.database_epoch == database_epoch) {
                const RegistryWaiterClaim waiter_claim =
                    claim_registry_waiter_locked(entry);
                if (!waiter_claim) {
                    LWLockRelease(g_registry_lock);
                    return decision;
                }
                const std::uint64_t generation = entry.generation;
                LWLockRelease(g_registry_lock);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.inflight_waits, 1);
                }
                return wait_for_producer(
                    static_cast<int>(offset), generation, waiter_claim);
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
        retired_manifest = entry.manifest_handle;
        const std::uint64_t next_generation = entry.generation + 1;
        entry.state = SharedEntryState::Producing;
        entry.key_size = static_cast<std::uint32_t>(key->len);
        entry.generation = next_generation == 0 ? 1 : next_generation;
        entry.claim_sequence = next_claim_sequence();
        entry.key_hash = key_hash;
        entry.key_digest = *digest;
        entry.database_epoch = database_epoch;
        entry.database_oid = MyDatabaseId;
        entry.current_role_oid = GetUserId();
        entry.session_user_oid = GetSessionUserId();
        entry.producer_pid = MyProcPid;
        entry.producer_proc_number = MyProcNumber;
        entry.waiter_count = 0;
        entry.reader_count = 0;
        std::memset(entry.readers, 0, sizeof(entry.readers));
        std::memset(entry.waiters, 0, sizeof(entry.waiters));
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
        entry.manifest_handle = {};
        strlcpy(
            entry.display_sql,
            display_sql == nullptr ? "<query text unavailable>" : display_sql,
            sizeof(entry.display_sql));
        std::memcpy(entry.key, key->data, key->len);
        decision.role = RuntimeRole::Producer;
        decision.index = reusable_index;
        decision.generation = entry.generation;
        decision.entry_id = cache_entry_id(entry);
    }
    LWLockRelease(g_registry_lock);

    if (retired_manifest) {
        static_cast<void>(storage::retire_manifest(retired_manifest));
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.evictions, 1);
        }
    }
    return decision;
}

/** Release a short reader pin after full preflight/materialization. */
void release_registry_reader(
        int index, std::uint64_t generation,
        const RegistryReaderClaim& reader_claim,
        bool successful,
        bool invalidate_failure = true,
        bool count_outcome = true) noexcept {
    if (g_shared == nullptr || index < 0 || !reader_claim) {
        return;
    }
    storage::ManifestHandle retired_manifest{};
    bool released = false;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[index];
    if (entry.generation == generation) {
        RegistryReaderOwner& reader = entry.readers[reader_claim.slot];
        if (reader.active
            && reader.registration_sequence
                == reader_claim.registration_sequence
            && reader.owner_pid == MyProcPid
            && reader.owner_proc_number
                == static_cast<std::uint32_t>(MyProcNumber)) {
            clear_registry_reader_locked(entry, reader);
            released = true;
        }
        if (released && entry.reader_count == 0
            && entry.state == SharedEntryState::Retired
            && entry.manifest_handle) {
            retired_manifest = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        if (released && !successful && invalidate_failure
            && entry.state == SharedEntryState::Complete) {
            entry.state = SharedEntryState::Retired;
            if (entry.reader_count == 0 && entry.manifest_handle) {
                retired_manifest = entry.manifest_handle;
                entry.manifest_handle = {};
            }
            ConditionVariableBroadcast(&entry.changed);
        }
    }
    LWLockRelease(g_registry_lock);
    if (retired_manifest) {
        static_cast<void>(storage::retire_manifest(retired_manifest));
    }
    if (!released || !count_outcome) {
        return;
    }
    if (successful) {
        pg_atomic_fetch_add_u64(&g_shared->stats.completed_hits, 1);
    } else {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

void discard_unpublished_tokens(BioseqCacheState& state) noexcept {
    for (std::uint32_t index = 0;
         index < state.stored_token_count; ++index) {
        if (state.stored_tokens[index]) {
            static_cast<void>(storage::discard_unpublished_block(
                state.stored_tokens[index]));
            state.stored_tokens[index] = {};
        }
    }
    state.stored_token_count = 0;
}

void release_storage_reader_claims(BioseqCacheState& state) noexcept {
    if (state.manifest_pin_owned) {
        static_cast<void>(storage::unpin_manifest(state.manifest_pin));
        state.manifest_pin = {};
        state.manifest_pin_owned = false;
    }
    if (state.manifest_waiter_owned) {
        static_cast<void>(storage::unregister_manifest_waiter(
            state.manifest_waiter_token));
        state.manifest_waiter_token = {};
        state.manifest_waiter_owned = false;
    }
}

/** Fail a generation only when this backend still owns the producer token. */
void fail_registry_producer(BioseqCacheState& state) noexcept {
    storage::ManifestHandle retired_manifest{};
    if (g_shared != nullptr && state.registry_index >= 0) {
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        CacheEntry& entry = g_shared->entries[state.registry_index];
        if (entry.generation == state.generation
            && entry.state == SharedEntryState::Producing
            && entry.producer_pid == MyProcPid) {
            retired_manifest = entry.manifest_handle;
            entry.manifest_handle = {};
            fail_abandoned_producer(entry);
        }
        LWLockRelease(g_registry_lock);
    }
    if (retired_manifest) {
        static_cast<void>(storage::retire_manifest(retired_manifest));
    }
    if (state.manifest_handle && !state.producer_completed) {
        static_cast<void>(storage::retire_manifest(state.manifest_handle));
        state.manifest_handle = {};
    }
    release_storage_reader_claims(state);
    discard_unpublished_tokens(state);
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
    release_storage_reader_claims(*state);
    if (state->registry_reader_owned) {
        release_registry_reader(
            state->registry_index,
            state->generation,
            state->registry_reader_claim,
            false);
        state->registry_reader_owned = false;
        state->registry_reader_claim = {};
    }
    if (state->runtime_role == RuntimeRole::Producer
        && !state->producer_completed) {
        fail_registry_producer(*state);
    }
    discard_unpublished_tokens(*state);
    if (state->replay_store != nullptr) {
        tuplestore_end(state->replay_store);
        state->replay_store = nullptr;
    }
    if (state->replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state->replay_slot);
        state->replay_slot = nullptr;
    }
    destroy_column_builder(*state);
}

/** Prepare one generation-private self-validating column-store capture. */
[[nodiscard]] bool begin_producer_capture(
        BioseqCacheState& state, TupleDesc descriptor) {
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

/** Publish immutable storage blocks only if no commit raced their capture. */
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
    if (sealed.empty()) {
        return false;
    }
    for (std::size_t offset = 0; offset < sealed.size();) {
        if (state.stored_token_count >= storage::kBlocksPerManifest) {
            discard_unpublished_tokens(state);
            return false;
        }
        const std::size_t remaining = sealed.size() - offset;
        const std::size_t block_bytes = remaining
                < static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes)
            ? remaining
            : static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes);
        auto stored = storage::store_block(std::span<const std::byte>{
            sealed.data() + offset, block_bytes});
        if (!stored) {
            discard_unpublished_tokens(state);
            return false;
        }
        state.stored_tokens[state.stored_token_count++] = stored.token;
        offset += block_bytes;
    }
    destroy_column_builder(state);
    maybe_pause(TestPausePoint::BeforeTerminalPublish);

    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[state.registry_index];
    const std::uint64_t current_epoch = pg_atomic_read_u64(
        &g_shared->database_write_epoch);
    const bool stable_visibility = pg_atomic_read_u32(
        &g_shared->database_transition_count) == 0;
    reap_dead_registry_waiters_locked(entry, true);
    if (entry.generation == state.generation
        && entry.state == SharedEntryState::Producing
        && entry.producer_pid == MyProcPid
        && entry.claim_sequence == state.entry_id.claim_sequence
        && entry.database_epoch == current_epoch
        && stable_visibility) {
        const TimestampTz completed_at = GetCurrentTimestamp();
        const std::int64_t ttl_microseconds =
            static_cast<std::int64_t>(g_query_cache_ttl_ms) * 1000;
        storage::ManifestDescriptor manifest{
            .id = state.entry_id,
            .kind = CacheEntryKind::ExactFinalResult,
            .logical_bytes = state.logical_bytes,
            .row_count = state.row_count,
            .created_at_us = entry.created_at,
            .expires_at_us = completed_at + ttl_microseconds,
        };
        auto publication = storage::publish_manifest(
            manifest,
            std::span<const storage::StoredBlockToken>{
                state.stored_tokens.data(), state.stored_token_count});
        if (!publication) {
            fail_abandoned_producer(entry);
            LWLockRelease(g_registry_lock);
            discard_unpublished_tokens(state);
            return false;
        }
        state.stored_token_count = 0;
        bool waiters_registered = true;
        for (RegistryWaiterOwner& waiter : entry.waiters) {
            if (!waiter.active) {
                continue;
            }
            auto registered = storage::register_manifest_waiter(
                publication.handle,
                waiter.owner_pid,
                waiter.owner_proc_number,
                waiter.registered_at);
            if (registered) {
                waiter.storage_token = registered.token;
                continue;
            }
            if (!registry_owner_alive(
                    waiter.owner_pid, waiter.owner_proc_number)) {
                clear_registry_waiter_locked(entry, waiter);
                continue;
            }
            waiters_registered = false;
            break;
        }
        if (!waiters_registered) {
            for (RegistryWaiterOwner& waiter : entry.waiters) {
                if (waiter.storage_token) {
                    static_cast<void>(storage::unregister_manifest_waiter(
                        waiter.storage_token));
                    waiter.storage_token = {};
                }
            }
            static_cast<void>(storage::retire_manifest(publication.handle));
            fail_abandoned_producer(entry);
            LWLockRelease(g_registry_lock);
            return false;
        }
        state.manifest_handle = publication.handle;
        entry.state = SharedEntryState::Complete;
        entry.producer_pid = 0;
        entry.producer_proc_number = INVALID_PROC_NUMBER;
        entry.completed_at = completed_at;
        entry.expires_at = completed_at + ttl_microseconds;
        entry.row_count = state.row_count;
        entry.logical_bytes = state.logical_bytes;
        entry.stored_bytes = state.payload_bytes;
        entry.manifest_handle = publication.handle;
        pg_atomic_fetch_add_u64(&g_shared->stats.produced, 1);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.logical_bytes, state.logical_bytes);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.stored_bytes, entry.stored_bytes);
        state.producer_completed = true;
        ConditionVariableBroadcast(&entry.changed);
    } else if (entry.generation == state.generation
            && entry.state == SharedEntryState::Producing) {
        fail_abandoned_producer(entry);
    }
    const bool published = state.producer_completed;
    LWLockRelease(g_registry_lock);
    if (!published) {
        discard_unpublished_tokens(state);
    }
    return published;
}

/** Materialize and validate an entire shared result before exposing row one. */
[[nodiscard]] Tuplestorestate* load_complete_result(
        const storage::ManifestPin& pin, TupleDesc expected_descriptor,
        CacheEntryKind expected_kind = CacheEntryKind::ExactFinalResult) {
    std::vector<ColumnSchema> expected_schema;
    try {
        expected_schema = build_column_schema(expected_descriptor, nullptr);
    } catch (...) {
        return nullptr;
    }
    if (!pin || pin.kind != expected_kind
        || pin.block_count > storage::kBlocksPerManifest) {
        return nullptr;
    }
    std::uint64_t total_bytes = 0;
    for (std::uint32_t index = 0; index < pin.block_count; ++index) {
        const std::uint64_t length = pin.blocks[index].length;
        if (length == 0 || length > kMaximumStoredResultBytes
            || total_bytes > kMaximumStoredResultBytes - length) {
            return nullptr;
        }
        total_bytes += length;
    }
    if (total_bytes == 0 || total_bytes > kMaximumStoredResultBytes) {
        return nullptr;
    }
    const std::size_t store_size = static_cast<std::size_t>(total_bytes);
    auto* store = static_cast<std::byte*>(palloc(store_size));
    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < pin.block_count; ++index) {
        const std::size_t length = static_cast<std::size_t>(
            pin.blocks[index].length);
        if (storage::read_pinned_block(
                pin, index,
                std::span<std::byte>{store + offset, length})
                != storage::StorageError::None) {
            pfree(store);
            return nullptr;
        }
        offset += length;
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
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            MemoryContextDelete(row_context);
            pfree(values);
            pfree(nulls);
            delete view;
            pfree(store);
            tuplestore_end(result);
            PG_RE_THROW();
        }
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
    state->replay_store = nullptr;
    state->replay_slot = nullptr;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->capture_failed = false;
    state->producer_completed = false;
    state->manifest_pin_owned = false;
    state->manifest_waiter_owned = false;
    state->registry_reader_owned = false;
    state->registry_reader_claim = {};
    state->entry_id = {};
    state->manifest_handle = {};
    state->manifest_waiter_token = {};
    state->manifest_pin = {};
    state->stored_tokens = {};
    state->stored_token_count = 0;
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
    const bool force_bypass = private_force_bypass(*scan);
    const VisibilityTicket visibility = explain_only || force_bypass
        ? VisibilityTicket{}
        : mint_visibility_ticket();
    const bool cache_allowed = !force_bypass && visibility.valid
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
            const int sqlstate = geterrcode();
            MemoryContextSwitchTo(caller_context);
            if (cache_interrupt_error(sqlstate)) {
                PG_RE_THROW();
            }
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
            state->registry_reader_claim = decision.reader_claim;
            state->entry_id = decision.entry_id;
            state->manifest_handle = decision.manifest_handle;
            state->manifest_waiter_token =
                decision.manifest_waiter_token;
            state->manifest_waiter_owned =
                static_cast<bool>(decision.manifest_waiter_token);
            state->registry_reader_owned =
                static_cast<bool>(decision.reader_claim);
        }
        if (key != nullptr) {
            pfree(key->data);
            pfree(key);
        }

        if (state->runtime_role == RuntimeRole::Consumer) {
            MemoryContext load_context = CurrentMemoryContext;
            PG_TRY();
            {
                storage::PinManifestResult pinned =
                    state->manifest_waiter_owned
                    ? storage::convert_waiter_to_pin(
                        state->manifest_waiter_token)
                    : storage::pin_manifest(state->entry_id);
                if (pinned) {
                    state->manifest_waiter_owned = false;
                    state->manifest_waiter_token = {};
                    state->manifest_pin = pinned.pin;
                    state->manifest_pin_owned = true;
                    maybe_pause(TestPausePoint::AfterReaderPinned);
                    state->replay_store = load_complete_result(
                        state->manifest_pin,
                        node->ss.ps.ps_ResultTupleDesc);
                }
                if (state->replay_store != nullptr) {
                    state->replay_slot = MakeSingleTupleTableSlot(
                        node->ss.ps.ps_ResultTupleDesc,
                        &TTSOpsMinimalTuple);
                }
            }
            PG_CATCH();
            {
                const int sqlstate = geterrcode();
                MemoryContextSwitchTo(load_context);
                if (state->replay_store != nullptr) {
                    tuplestore_end(state->replay_store);
                }
                if (state->replay_slot != nullptr) {
                    ExecDropSingleTupleTableSlot(state->replay_slot);
                }
                state->replay_store = nullptr;
                state->replay_slot = nullptr;
                if (cache_interrupt_error(sqlstate)) {
                    release_storage_reader_claims(*state);
                    if (state->registry_reader_owned) {
                        release_registry_reader(
                            state->registry_index,
                            state->generation,
                            state->registry_reader_claim,
                            false,
                            false,
                            false);
                        state->registry_reader_owned = false;
                        state->registry_reader_claim = {};
                    }
                    PG_RE_THROW();
                }
                FlushErrorState();
            }
            PG_END_TRY();
            const bool loaded = state->replay_store != nullptr
                && state->replay_slot != nullptr;
            release_storage_reader_claims(*state);
            release_registry_reader(
                state->registry_index,
                state->generation,
                state->registry_reader_claim,
                loaded);
            state->registry_reader_owned = false;
            state->registry_reader_claim = {};
            if (loaded) {
                return;
            }
            state->runtime_role = RuntimeRole::Bypass;
        }

        if (state->runtime_role == RuntimeRole::Producer
            ) {
            maybe_pause(TestPausePoint::AfterBuildingPublish);
            if (!begin_producer_capture(
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
        TupleTableSlot* slot = state->replay_slot;
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
                const int sqlstate = geterrcode();
                MemoryContextSwitchTo(caller_context);
                if (cache_interrupt_error(sqlstate)) {
                    fail_registry_producer(*state);
                    PG_RE_THROW();
                }
                FlushErrorState();
                completed = false;
            }
            PG_END_TRY();
            if (!completed) {
                fail_registry_producer(*state);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.fallbacks, 1);
                }
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
            const int sqlstate = geterrcode();
            MemoryContextSwitchTo(caller_context);
            if (cache_interrupt_error(sqlstate)) {
                fail_registry_producer(*state);
                PG_RE_THROW();
            }
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
[[nodiscard]] const char* readiness_name(
        CacheCorrectnessReadiness readiness) noexcept {
    switch (readiness) {
        case CacheCorrectnessReadiness::PassthroughOnly:
            return "passthrough-only";
        case CacheCorrectnessReadiness::RelationFreeOnly:
            return "relation-free-only";
        case CacheCorrectnessReadiness::GenericMvccReady:
            return "generic-mvcc-ready";
        case CacheCorrectnessReadiness::ProviderCompleteReady:
            return "provider-complete-ready";
        case CacheCorrectnessReadiness::ReusableCoverageReady:
            return "reusable-coverage-ready";
    }
    return "unknown";
}

[[nodiscard]] const char* worker_lifecycle_name(
        storage::WorkerLifecycle lifecycle) noexcept {
    switch (lifecycle) {
        case storage::WorkerLifecycle::NotRegistered: return "not-registered";
        case storage::WorkerLifecycle::Registered: return "registered";
        case storage::WorkerLifecycle::Starting: return "starting";
        case storage::WorkerLifecycle::Running: return "running";
        case storage::WorkerLifecycle::Stopping: return "stopping";
        case storage::WorkerLifecycle::Stopped: return "stopped";
        case storage::WorkerLifecycle::Failed: return "failed";
    }
    return "unknown";
}

void cache_explain_custom_scan(
        CustomScanState* node, List*, ExplainState* explain_state) {
    const auto* scan = reinterpret_cast<const CustomScan*>(node->ss.ps.plan);
    const QueryCacheMode mode = static_cast<QueryCacheMode>(g_query_cache_mode);
    const char* mode_name = mode == QueryCacheMode::Reuse
        ? "reuse"
        : mode == QueryCacheMode::Same ? "same" : "disabled";
    const bool force_bypass = private_force_bypass(*scan);
    const int parameter_count = private_list_length(*scan, 3, T_List);
    const int dependency_count = private_list_length(*scan, 4, T_OidList);
    ExplainPropertyText("Cache Mode", mode_name, explain_state);
    ExplainPropertyText(
        "Planned Cache Action",
        force_bypass
            ? "[Bypass] unsafe finalized source path"
            : "provisional exact result",
        explain_state);
    ExplainPropertyBool("Cache Force Bypass", force_bypass, explain_state);
    if (force_bypass) {
        ExplainPropertyText(
            "Cache Bypass Reason",
            "unsafe finalized source path",
            explain_state);
    }
    ExplainPropertyInteger(
        "Cache Parameter Count", nullptr, parameter_count, explain_state);
    ExplainPropertyInteger(
        "Cache Dependency Count", nullptr, dependency_count, explain_state);
    char* dependency_diagnostic = dependency_count < 0
        ? pstrdup("metadata invalid")
        : dependency_count == 0
            ? pstrdup("none (relation-free exact)")
            : psprintf("%d relation%s", dependency_count,
                dependency_count == 1 ? "" : "s");
    ExplainPropertyText(
        "Cache Dependencies", dependency_diagnostic, explain_state);
    pfree(dependency_diagnostic);
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
    const storage::StorageRuntimeSnapshot storage_snapshot =
        storage::runtime_snapshot();
    ExplainPropertyText(
        "Cache Readiness",
        readiness_name(storage_snapshot.correctness_readiness),
        explain_state);
    char* storage_diagnostic = psprintf(
        "%s; backend=%s; hot=%llu/%llu; file=%llu/%llu; "
        "manifests=%llu; waiters=%llu",
        storage_snapshot.bootstrap_ready ? "ready" : "unavailable",
        storage_snapshot.dsa_attached ? "attached" : "not-attached",
        static_cast<unsigned long long>(storage_snapshot.reserved_hot_bytes),
        static_cast<unsigned long long>(storage_snapshot.hot_ceiling_bytes),
        static_cast<unsigned long long>(storage_snapshot.reserved_file_bytes),
        static_cast<unsigned long long>(storage_snapshot.file_ceiling_bytes),
        static_cast<unsigned long long>(storage_snapshot.manifest_count),
        static_cast<unsigned long long>(
            storage_snapshot.waiter_reference_count));
    ExplainPropertyText("Cache Storage", storage_diagnostic, explain_state);
    pfree(storage_diagnostic);
    ExplainPropertyText(
        "Cache Janitor",
        worker_lifecycle_name(storage_snapshot.worker_lifecycle),
        explain_state);
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
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->replay_slot = nullptr;
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

enum class CoverageRuntimeRoleV1 : std::uint8_t {
    Bypass = 0,
    Producer = 1,
    CurrentConsumer = 2,
};

struct CoverageRuntimeSourceV1 {
    bool valid;
    bool registry_reference_owned;
    bool manifest_pin_owned;
    std::uint8_t reserved;
    ReusableGenerationReferenceV1 reference;
    ReusableReferenceTokenV1 registry_reference;
    storage::ManifestPin manifest_pin;
    Tuplestorestate* replay_store;
    TupleTableSlot* replay_slot;
    ExprState* predicate_state;
};

struct ReusableCoverageState {
    CustomScanState css;
    bool child_initialized;
    bool runtime_recipe_valid;
    bool current_producer_owned;
    bool current_reference_owned;
    bool producer_completed;
    bool capture_failed;
    bool cleanup_registered;
    bool cleanup_done;
    std::uint16_t source_count;
    std::uint16_t active_source_count;
    std::uint16_t replay_source_index;
    std::uint16_t reserved;
    CoverageRuntimeRoleV1 runtime_role;
    std::array<std::uint8_t, 7> reserved_role;
    const provider_registry::ProviderV1* provider;
    CoverageCurrentPlanHeaderV1 current_header;
    CacheDigest256 dependency_fingerprint;
    PlannerDecodedCoverageV1 current_contract;
    ReusableGenerationReferenceV1 current_reference;
    ReusableReferenceTokenV1 current_reference_token;
    storage::ManifestHandle manifest_handle;
    std::array<storage::StoredBlockToken, storage::kBlocksPerManifest>
        stored_tokens;
    std::uint32_t stored_token_count;
    std::uint32_t reserved_count;
    std::uint64_t row_count;
    std::uint64_t logical_bytes;
    std::uint64_t payload_bytes;
    ColumnStoreBuilder* column_builder;
    Oid* send_functions;
    Tuplestorestate* current_replay_store;
    TupleTableSlot* current_replay_slot;
    ExprState* requested_predicate_state;
    std::array<CoverageRuntimeSourceV1, kMaxSourcesPerCoverageNode> sources;
    MemoryContextCallback cleanup_callback;
};

static_assert(std::is_standard_layout_v<ReusableCoverageState>);
static_assert(offsetof(ReusableCoverageState, css) == 0);

[[nodiscard]] std::span<const std::byte> binary_node_payload(
        const void* node) noexcept {
    if (node == nullptr || !IsA(node, Const)) {
        return {};
    }
    const auto* constant = static_cast<const Const*>(node);
    if (constant->constisnull || constant->consttype != BYTEAOID
        || constant->constbyval) {
        return {};
    }
    const auto* value = DatumGetByteaPP(constant->constvalue);
    return bytea_payload(value);
}

[[nodiscard]] PlanState* coverage_retained_child(
        CustomScanState* node) noexcept {
    return node->custom_ps == NIL
        ? nullptr
        : static_cast<PlanState*>(linitial(node->custom_ps));
}

void destroy_coverage_builder(ReusableCoverageState& state) noexcept {
    delete state.column_builder;
    state.column_builder = nullptr;
    if (state.send_functions != nullptr) {
        pfree(state.send_functions);
        state.send_functions = nullptr;
    }
}

void discard_coverage_blocks(ReusableCoverageState& state) noexcept {
    for (std::uint32_t index = 0;
         index < state.stored_token_count;
         ++index) {
        if (state.stored_tokens[index]) {
            static_cast<void>(storage::discard_unpublished_block(
                state.stored_tokens[index]));
            state.stored_tokens[index] = {};
        }
    }
    state.stored_token_count = 0;
}

void release_runtime_reusable_reference(
        ReusableReferenceTokenV1& token) noexcept {
    if (token.active && g_reusable_registry) {
        static_cast<void>(release_reusable_reference(
            g_reusable_registry,
            token,
            reusable_partition_callbacks()));
    }
}

void release_coverage_source(CoverageRuntimeSourceV1& source) noexcept {
    if (source.manifest_pin_owned) {
        static_cast<void>(storage::unpin_manifest(source.manifest_pin));
        source.manifest_pin = {};
        source.manifest_pin_owned = false;
    }
    if (source.registry_reference_owned) {
        release_runtime_reusable_reference(source.registry_reference);
        source.registry_reference_owned = false;
    }
    if (source.replay_store != nullptr) {
        tuplestore_end(source.replay_store);
        source.replay_store = nullptr;
    }
    if (source.replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(source.replay_slot);
        source.replay_slot = nullptr;
    }
    source.valid = false;
}

void fail_coverage_producer(ReusableCoverageState& state) noexcept {
    if (state.current_producer_owned && !state.producer_completed) {
        fail_current_reusable_generation(
            state.current_reference, CacheFailureCode::ProducerExited);
    }
    state.current_producer_owned = false;
    if (state.manifest_handle && !state.producer_completed) {
        static_cast<void>(storage::retire_manifest(state.manifest_handle));
        state.manifest_handle = {};
    }
    discard_coverage_blocks(state);
    destroy_coverage_builder(state);
    state.capture_failed = true;
    state.runtime_role = CoverageRuntimeRoleV1::Bypass;
}

void coverage_state_cleanup(void* opaque) noexcept {
    auto* state = static_cast<ReusableCoverageState*>(opaque);
    if (state == nullptr || state->cleanup_done) {
        return;
    }
    state->cleanup_done = true;
    if (state->current_reference_owned) {
        release_runtime_reusable_reference(state->current_reference_token);
        state->current_reference_owned = false;
    }
    for (CoverageRuntimeSourceV1& source : state->sources) {
        release_coverage_source(source);
    }
    if (state->current_replay_store != nullptr) {
        tuplestore_end(state->current_replay_store);
        state->current_replay_store = nullptr;
    }
    if (state->current_replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state->current_replay_slot);
        state->current_replay_slot = nullptr;
    }
    if (state->current_producer_owned && !state->producer_completed) {
        fail_coverage_producer(*state);
    }
    discard_coverage_blocks(*state);
    destroy_coverage_builder(*state);
}

[[nodiscard]] bool begin_coverage_capture(
        ReusableCoverageState& state, TupleDesc descriptor) {
    try {
        state.send_functions = static_cast<Oid*>(palloc0(
            sizeof(Oid) * static_cast<std::size_t>(descriptor->natts)));
        std::vector<ColumnSchema> schema = build_column_schema(
            descriptor, state.send_functions);
        state.column_builder = new ColumnStoreBuilder(
            current_column_store_identity(), schema);
        return true;
    } catch (...) {
        destroy_coverage_builder(state);
        return false;
    }
}

[[nodiscard]] bool capture_coverage_row(
        ReusableCoverageState& state, TupleTableSlot* slot) {
    if (state.column_builder == nullptr || state.send_functions == nullptr
        || slot == nullptr) {
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
            byte_count};
        state.logical_bytes += byte_count;
    }
    if (state.logical_bytes <= kMaximumStoredResultBytes) {
        try {
            success = state.column_builder->append_row(
                std::span<const EncodedColumnValue>{
                    values,
                    static_cast<std::size_t>(descriptor->natts)})
                .has_value();
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

/** Seal full merged output, publish storage, then make registry COMPLETE last. */
[[nodiscard]] bool complete_coverage_producer(
        ReusableCoverageState& state) {
    if (!state.current_producer_owned || state.column_builder == nullptr
        || state.column_builder->row_count() != state.row_count
        || state.dependency_fingerprint
            != current_reusable_dependency_fingerprint()
        || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
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
    if (sealed.empty() || sealed.size() > kMaximumStoredResultBytes) {
        return false;
    }
    state.payload_bytes = sealed.size();
    for (std::size_t offset = 0; offset < sealed.size();) {
        if (state.stored_token_count >= storage::kBlocksPerManifest) {
            discard_coverage_blocks(state);
            return false;
        }
        const std::size_t remaining = sealed.size() - offset;
        const std::size_t block_bytes = remaining
                < static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes)
            ? remaining
            : static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes);
        auto stored = storage::store_block(std::span<const std::byte>{
            sealed.data() + offset, block_bytes});
        if (!stored) {
            discard_coverage_blocks(state);
            return false;
        }
        state.stored_tokens[state.stored_token_count++] = stored.token;
        offset += block_bytes;
    }
    destroy_coverage_builder(state);
    const TimestampTz now = GetCurrentTimestamp();
    const std::int64_t ttl_us = static_cast<std::int64_t>(
        g_query_cache_ttl_ms) * 1000;
    const storage::ManifestDescriptor descriptor{
        .id = state.current_reference.id,
        .kind = CacheEntryKind::ReusableCoverage,
        .logical_bytes = state.logical_bytes,
        .row_count = state.row_count,
        .created_at_us = now,
        .expires_at_us = now + ttl_us,
    };
    auto publication = storage::publish_manifest(
        descriptor,
        std::span<const storage::StoredBlockToken>{
            state.stored_tokens.data(), state.stored_token_count});
    if (!publication) {
        discard_coverage_blocks(state);
        return false;
    }
    state.stored_token_count = 0;
    state.manifest_handle = publication.handle;
    const ReusableRegistryErrorV1 published =
        publish_current_reusable_generation(ReusablePublishRequestV1{
            .reference = state.current_reference,
            .manifest = reusable_manifest_handle(publication.handle),
            .completed_monotonic_ns = reusable_monotonic_now_ns(),
            .row_count = state.row_count,
            .logical_bytes = state.logical_bytes,
            .stored_bytes = state.payload_bytes,
        });
    if (published != ReusableRegistryErrorV1::None) {
        static_cast<void>(storage::retire_manifest(publication.handle));
        state.manifest_handle = {};
        return false;
    }
    state.producer_completed = true;
    state.current_producer_owned = false;
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.produced, 1);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.logical_bytes, state.logical_bytes);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.stored_bytes, state.payload_bytes);
    }
    return true;
}

void initialize_coverage_child(
        ReusableCoverageState& state,
        CustomScanState* node,
        EState* estate,
        int eflags) {
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    if (scan->custom_plans == NIL || list_length(scan->custom_plans) != 1
        || node->custom_ps != NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan retained child is malformed")));
    }
    Plan* child_plan = static_cast<Plan*>(linitial(scan->custom_plans));
    node->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
    state.child_initialized = true;
}

/** Validate finalized provider semantics and close every gate before child init. */
void coverage_begin_custom_scan(
        CustomScanState* node, EState* estate, int eflags) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    state->child_initialized = false;
    state->runtime_recipe_valid = false;
    state->source_count = 0;
    node->custom_ps = NIL;

    List* candidates = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 4
        ? static_cast<List*>(list_nth(scan->custom_private, 3))
        : NIL;
    const int source_count = list_length(candidates);
    if (source_count < 0
        || source_count > static_cast<int>(kMaxSourcesPerCoverageNode)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan source count is invalid")));
    }
    state->source_count = static_cast<std::uint16_t>(source_count);

    std::array<int, kMaxSourcesPerCoverageNode> gate_ids{};
    int gate_index = 0;
    ListCell* cell = nullptr;
    foreach(cell, candidates) {
        const auto* source = static_cast<const List*>(lfirst(cell));
        if (source == nullptr || list_length(source) != 7
            || !IsA(list_nth(source, 5), Integer)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan source metadata is invalid")));
        }
        gate_ids[gate_index++] = intVal(list_nth(source, 5));
    }

    const int provider_id = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 2
            && IsA(list_nth(scan->custom_private, 1), Integer)
        ? intVal(list_nth(scan->custom_private, 1))
        : 0;
    const provider_registry::ProviderV1* provider = provider_id > 0
        ? provider_registry::find_provider_v1(
            static_cast<std::uint32_t>(provider_id))
        : nullptr;
    List* recipe_private = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 6
        ? static_cast<List*>(list_nth(scan->custom_private, 5))
        : NIL;
    if (provider != nullptr && recipe_private != NIL
        && list_length(recipe_private) == 4
        && IsA(list_nth(recipe_private, 0), Integer)
        && intVal(list_nth(recipe_private, 0)) != 0
        && IsA(list_nth(recipe_private, 3), Integer)) {
        const auto expected = binary_node_payload(
            list_nth(recipe_private, 2));
        if (expected.size() == CacheDigest256{}.bytes.size()) {
            BioseqCachePlanDigestRecipeV1 recipe{
                .struct_size = sizeof(BioseqCachePlanDigestRecipeV1),
                .flags = 0,
                .digest_format_version = static_cast<std::uint32_t>(
                    intVal(list_nth(recipe_private, 3))),
                .reserved = 0,
                .plan_domain_nodes = static_cast<List*>(
                    list_nth(recipe_private, 1)),
            };
            std::memcpy(
                recipe.expected_generation_digest,
                expected.data(),
                expected.size());
            reuse::ProviderCallDiagnosticV1 diagnostic{};
            state->runtime_recipe_valid =
                reuse::validate_runtime_recipe_fail_open(
                    provider,
                    &recipe,
                    estate,
                    estate->es_query_cxt,
                    &diagnostic);
        }
    }

    if (source_count != 0) {
        reuse::ReuseGateFreezeV1 gates{};
        if (!reuse::begin_gate_preflight(gates, source_count)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan gates are malformed")));
        }
        reuse::freeze_all_sources_invalid(gates);
        if (!reuse::publish_frozen_gates(
                estate,
                std::span<const int>{
                    gate_ids.data(), static_cast<std::size_t>(source_count)},
                gates)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan gates cannot be frozen")));
        }
    }
    initialize_coverage_child(*state, node, estate, eflags);
}

TupleTableSlot* coverage_exec_custom_scan(CustomScanState* node) {
    PlanState* child = coverage_retained_child(node);
    if (child == nullptr) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    TupleTableSlot* raw = ExecProcNode(child);
    if (TupIsNull(raw)) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    expression_context->ecxt_scantuple = raw;
    return node->ss.ps.ps_ProjInfo == nullptr
        ? raw
        : ExecProject(node->ss.ps.ps_ProjInfo);
}

void coverage_end_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    if (state->child_initialized && coverage_retained_child(node) != nullptr) {
        ExecEndNode(coverage_retained_child(node));
        node->custom_ps = NIL;
    }
    state->child_initialized = false;
}

void coverage_rescan_custom_scan(CustomScanState* node) {
    if (coverage_retained_child(node) != nullptr) {
        ExecReScan(coverage_retained_child(node));
    }
}

void coverage_explain_custom_scan(
        CustomScanState* node, List*, ExplainState* explain) {
    const auto* state = reinterpret_cast<const ReusableCoverageState*>(node);
    ExplainPropertyText("Cache Entry Kind", "ReusableCoverage", explain);
    ExplainPropertyInteger(
        "Reusable Sources", nullptr, state->source_count, explain);
    ExplainPropertyBool(
        "Provider Runtime Recipe Valid",
        state->runtime_recipe_valid,
        explain);
}

const CustomExecMethods& coverage_exec_methods() {
    static const CustomExecMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .BeginCustomScan = coverage_begin_custom_scan,
        .ExecCustomScan = coverage_exec_custom_scan,
        .EndCustomScan = coverage_end_custom_scan,
        .ReScanCustomScan = coverage_rescan_custom_scan,
        .ExplainCustomScan = coverage_explain_custom_scan,
    };
    return methods;
}

Node* coverage_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(
        newNode(sizeof(ReusableCoverageState), T_CustomScanState));
    state->css.methods = &coverage_exec_methods();
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods& coverage_scan_methods() {
    static const CustomScanMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .CreateCustomScanState = coverage_create_custom_scan_state,
    };
    return methods;
}

/** Finalize the provider recipe only after the decorated child Plan exists. */
Plan* coverage_plan_custom_path(
        PlannerInfo* root,
        RelOptInfo* relation,
        CustomPath* best_path,
        List* target_list,
        List*,
        List* custom_plans) {
    if (root == nullptr || relation == nullptr || best_path == nullptr
        || list_length(custom_plans) != 1
        || list_length(best_path->custom_private) != 5
        || !IsA(list_nth(best_path->custom_private, 0), Integer)
        || intVal(list_nth(best_path->custom_private, 0))
            != static_cast<int>(reuse::kReusablePlanFormatVersionV1)
        || !IsA(list_nth(best_path->custom_private, 1), Integer)) {
        return nullptr;
    }
    const int provider_id = intVal(list_nth(best_path->custom_private, 1));
    const provider_registry::ProviderV1* provider = provider_id > 0
        ? provider_registry::find_provider_v1(
            static_cast<std::uint32_t>(provider_id))
        : nullptr;
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
    reuse::PlannerOwnedDigestRecipeV1 recipe{};
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    const bool recipe_valid = provider != nullptr
        && reuse::invoke_build_recipe_fail_open(
            provider,
            root,
            child,
            root->planner_cxt,
            &recipe,
            &diagnostic);
    Const* expected_digest = recipe_valid
        ? make_binary_node(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                recipe.value.expected_generation_digest),
            sizeof(recipe.value.expected_generation_digest)})
        : nullptr;
    List* recipe_private = list_make4(
        makeInteger(recipe_valid && expected_digest != nullptr ? 1 : 0),
        recipe_valid
            ? copyObject(recipe.value.plan_domain_nodes)
            : static_cast<void*>(NIL),
        expected_digest != nullptr
            ? static_cast<void*>(expected_digest)
            : static_cast<void*>(makeInteger(0)),
        makeInteger(recipe_valid
            ? static_cast<int>(recipe.value.digest_format_version)
            : 0));

    auto* scan = makeNode(CustomScan);
    scan->methods = &coverage_scan_methods();
    scan->flags = best_path->flags;
    scan->scan.scanrelid = 0;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = static_cast<List*>(
        copyObject(best_path->custom_private));
    scan->custom_private = lappend(scan->custom_private, recipe_private);
    scan->custom_exprs = list_make1(
        copyObject(list_nth(best_path->custom_private, 4)));
    const auto* sources = static_cast<const List*>(
        list_nth(best_path->custom_private, 3));
    ListCell* cell = nullptr;
    foreach(cell, sources) {
        const auto* source = static_cast<const List*>(lfirst(cell));
        if (source == nullptr || list_length(source) != 7) {
            return nullptr;
        }
        scan->custom_exprs = lappend(
            scan->custom_exprs,
            copyObject(list_nth(source, 6)));
    }
    scan->custom_scan_tlist = static_cast<List*>(
        copyObject(child->targetlist));
    scan->custom_relids = bms_copy(relation->relids);
    return &scan->scan.plan;
}

const CustomPathMethods& coverage_path_methods() {
    static const CustomPathMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .PlanCustomPath = coverage_plan_custom_path,
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

struct ExactPlanVerification {
    const PlannedStmt* statement = nullptr;
    const List* relation_oids = NIL;
    bool relation_free = false;
    bool saw_relation_free_source = false;
};

[[nodiscard]] const RangeTblEntry* exact_plan_range_entry(
        const ExactPlanVerification& verification,
        Index range_table_index) noexcept {
    if (verification.statement == nullptr || range_table_index == 0
        || range_table_index > static_cast<Index>(
            list_length(verification.statement->rtable))) {
        return nullptr;
    }
    return static_cast<const RangeTblEntry*>(list_nth(
        verification.statement->rtable,
        static_cast<int>(range_table_index - 1)));
}

/** V1 admits only PostgreSQL's built-in heap table AM. */
[[nodiscard]] bool exact_table_access_method_safe(Oid relation_oid) {
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_test_exact_plan_fault
        == TestExactPlanFault::UnknownTableAccessMethod) {
        return false;
    }
#endif
    return OidIsValid(relation_oid)
        && get_rel_relam(relation_oid) == HEAP_TABLE_AM_OID;
}

/** V1 admits the built-in index AMs whose executor paths are reviewed. */
[[nodiscard]] bool exact_index_access_method_safe(Oid index_oid) {
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_test_exact_plan_fault
        == TestExactPlanFault::UnknownIndexAccessMethod) {
        return false;
    }
#endif
    if (!OidIsValid(index_oid)) {
        return false;
    }
    switch (get_rel_relam(index_oid)) {
        case BTREE_AM_OID:
        case HASH_AM_OID:
        case GIST_AM_OID:
        case GIN_AM_OID:
        case SPGIST_AM_OID:
        case BRIN_AM_OID:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool exact_relation_scan_safe(
        ExactPlanVerification& verification, const Scan& scan) {
    const RangeTblEntry* range = exact_plan_range_entry(
        verification, scan.scanrelid);
    if (range == nullptr || range->rtekind != RTE_RELATION
        || !OidIsValid(range->relid) || verification.relation_free) {
        return false;
    }
    const char relation_kind = get_rel_relkind(range->relid);
    return (relation_kind == RELKIND_RELATION
            || relation_kind == RELKIND_MATVIEW)
        && exact_table_access_method_safe(range->relid);
}

[[nodiscard]] bool exact_synthetic_scan_safe(
        const ExactPlanVerification& verification,
        const Scan& scan, RTEKind expected_kind) noexcept {
    const RangeTblEntry* range = exact_plan_range_entry(
        verification, scan.scanrelid);
    return range != nullptr && range->rtekind == expected_kind;
}

[[nodiscard]] bool exact_plan_tree_safe(
    ExactPlanVerification& verification, const Plan* plan);

[[nodiscard]] bool exact_plan_list_safe(
        ExactPlanVerification& verification, const List* plans) {
    const ListCell* cell = nullptr;
    foreach(cell, plans) {
        if (!exact_plan_tree_safe(
                verification, static_cast<const Plan*>(lfirst(cell)))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool exact_common_plan_expressions_safe(const Plan& plan) {
    return !contain_mutable_functions(
            reinterpret_cast<Node*>(plan.targetlist))
        && !contain_mutable_functions(reinterpret_cast<Node*>(plan.qual))
        && !contain_mutable_functions(reinterpret_cast<Node*>(plan.initPlan));
}

/** Positive allowlist for finalized executor sources and structural nodes. */
[[nodiscard]] bool exact_plan_tree_safe(
        ExactPlanVerification& verification, const Plan* plan) {
    if (plan == nullptr || !exact_common_plan_expressions_safe(*plan)) {
        return false;
    }

    switch (nodeTag(plan)) {
        case T_Result: {
            const auto* result = reinterpret_cast<const Result*>(plan);
            if (contain_mutable_functions(result->resconstantqual)) {
                return false;
            }
            if (verification.relation_free && outerPlan(plan) == nullptr
                && innerPlan(plan) == nullptr) {
                verification.saw_relation_free_source = true;
            }
            break;
        }
        case T_ValuesScan: {
            const auto* scan = reinterpret_cast<const ValuesScan*>(plan);
            if (!exact_synthetic_scan_safe(
                    verification, scan->scan, RTE_VALUES)
                || contain_mutable_functions(
                    reinterpret_cast<Node*>(scan->values_lists))) {
                return false;
            }
            if (verification.relation_free) {
                verification.saw_relation_free_source = true;
            }
            break;
        }
        case T_SubqueryScan: {
            const auto* scan = reinterpret_cast<const SubqueryScan*>(plan);
            if (!exact_synthetic_scan_safe(
                    verification, scan->scan, RTE_SUBQUERY)
                || !exact_plan_tree_safe(verification, scan->subplan)) {
                return false;
            }
            break;
        }
        case T_SeqScan:
        case T_BitmapHeapScan:
        case T_TidScan:
        case T_TidRangeScan:
            if (!exact_relation_scan_safe(
                    verification,
                    *reinterpret_cast<const Scan*>(plan))) {
                return false;
            }
            break;
        case T_IndexScan: {
            const auto* scan = reinterpret_cast<const IndexScan*>(plan);
            if (!exact_relation_scan_safe(verification, scan->scan)
                || !exact_index_access_method_safe(scan->indexid)) {
                return false;
            }
            break;
        }
        case T_IndexOnlyScan: {
            const auto* scan = reinterpret_cast<const IndexOnlyScan*>(plan);
            if (!exact_relation_scan_safe(verification, scan->scan)
                || !exact_index_access_method_safe(scan->indexid)) {
                return false;
            }
            break;
        }
        case T_BitmapIndexScan: {
            const auto* scan = reinterpret_cast<const BitmapIndexScan*>(plan);
            if (!exact_relation_scan_safe(verification, scan->scan)
                || !exact_index_access_method_safe(scan->indexid)) {
                return false;
            }
            break;
        }
        case T_Append:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const Append*>(plan)->appendplans)) {
                return false;
            }
            break;
        case T_MergeAppend:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const MergeAppend*>(plan)->mergeplans)) {
                return false;
            }
            break;
        case T_BitmapAnd:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans)) {
                return false;
            }
            break;
        case T_BitmapOr:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const BitmapOr*>(plan)->bitmapplans)) {
                return false;
            }
            break;
        case T_CustomScan: {
            const auto* scan = reinterpret_cast<const CustomScan*>(plan);
            if (scan->methods != &coverage_scan_methods()
                || list_length(scan->custom_plans) != 1
                || contain_mutable_functions(
                    reinterpret_cast<Node*>(scan->custom_exprs))
                || !exact_plan_list_safe(
                    verification, scan->custom_plans)) {
                return false;
            }
            break;
        }
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Material:
        case T_Memoize:
        case T_Sort:
        case T_IncrementalSort:
        case T_Group:
        case T_Agg:
        case T_WindowAgg:
        case T_Unique:
        case T_Gather:
        case T_GatherMerge:
        case T_Hash:
        case T_SetOp:
        case T_Limit:
            break;
        case T_ProjectSet:
        case T_ModifyTable:
        case T_RecursiveUnion:
        case T_SampleScan:
        case T_FunctionScan:
        case T_TableFuncScan:
        case T_CteScan:
        case T_NamedTuplestoreScan:
        case T_WorkTableScan:
        case T_ForeignScan:
        case T_LockRows:
        default:
            return false;
    }

    return (outerPlan(plan) == nullptr
            || exact_plan_tree_safe(verification, outerPlan(plan)))
        && (innerPlan(plan) == nullptr
            || exact_plan_tree_safe(verification, innerPlan(plan)));
}

[[nodiscard]] bool exact_dependency_metadata(
        const CustomScan& scan, const List** relation_oids) noexcept {
    if (relation_oids == nullptr || scan.custom_private == NIL
        || list_length(scan.custom_private) <= 4) {
        return false;
    }
    const auto* dependencies = static_cast<const List*>(
        list_nth(scan.custom_private, 4));
    if (dependencies != NIL && nodeTag(dependencies) != T_OidList) {
        return false;
    }
    *relation_oids = dependencies;
    return true;
}

/** Verify the exact retained tree after setrefs and flip only its marker. */
void verify_exact_cache_plan_fail_open(PlannedStmt& statement) {
    if (statement.planTree == nullptr
        || !IsA(statement.planTree, CustomScan)) {
        return;
    }
    auto* exact = reinterpret_cast<CustomScan*>(statement.planTree);
    if (exact->methods != &cache_scan_methods()) {
        return;
    }
    if (!set_private_force_bypass(*exact, true)) {
        return;
    }

    bool safe = false;
    MemoryContext caller_context = CurrentMemoryContext;
    PG_TRY();
    {
        const List* relation_oids = NIL;
        ExactPlanVerification verification{
            .statement = &statement,
            .relation_oids = NIL,
            .relation_free = false,
            .saw_relation_free_source = false,
        };
        if (exact_dependency_metadata(*exact, &relation_oids)
            && list_length(exact->custom_plans) == 1) {
            verification.relation_oids = relation_oids;
            verification.relation_free = relation_oids == NIL;
            safe = exact_plan_tree_safe(
                verification,
                static_cast<const Plan*>(linitial(exact->custom_plans)));
            ListCell* subplan_cell = nullptr;
            foreach(subplan_cell, statement.subplans) {
                safe = safe && exact_plan_tree_safe(
                    verification,
                    static_cast<const Plan*>(lfirst(subplan_cell)));
            }
            safe = safe && (!verification.relation_free
                || verification.saw_relation_free_source);
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        safe = false;
    }
    PG_END_TRY();
    if (safe) {
        static_cast<void>(set_private_force_bypass(*exact, false));
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
        const bool current_statement_text = query_string != nullptr
            && (debug_query_string == nullptr
                || query_string == debug_query_string
                || std::strcmp(query_string, debug_query_string) == 0);
        if (frame.eligible && query->rtable == NIL
            && !current_statement_text) {
            /* PL/pgSQL simple-expression plans must retain a Result root. */
            frame.eligible = false;
        }
        if (frame.eligible) {
            bool relation_overflow = false;
            frame.canonical_query = canonicalize_query(*query);
            frame.display_sql = copy_display_sql(query_string);
            frame.referenced_parameter_ids = collect_external_parameter_ids(*query);
            frame.referenced_relation_oids = collect_relation_oids(
                *query, relation_overflow);
            frame.eligible = !relation_overflow;
            if (frame.eligible
                && g_query_cache_mode
                    == static_cast<int>(QueryCacheMode::Reuse)) {
                const auto providers =
                    provider_registry::snapshot_providers_v1();
                for (std::uint32_t provider_index = 0;
                     provider_index < providers.provider_count
                     && frame.provider_intent_count
                         < frame.providers.size();
                     ++provider_index) {
                    const provider_registry::ProviderV1* provider =
                        providers.providers[provider_index];
                    reuse::ProviderCallDiagnosticV1 diagnostic{};
                    reuse::PlannerOwnedIntentV1 intent{};
                    if (!reuse::invoke_analyze_query_fail_open(
                            provider,
                            query,
                            planner_context,
                            &intent,
                            &diagnostic)) {
                        continue;
                    }
                    const std::size_t destination =
                        frame.provider_intent_count++;
                    frame.providers[destination] = provider;
                    frame.intents[destination] = intent;
                }
            }
        }
        result = g_previous_planner_hook != nullptr
            ? g_previous_planner_hook(
                query, query_string, cursor_options, bound_parameters)
            : standard_planner(
                query, query_string, cursor_options, bound_parameters);
        if (result != nullptr) {
            verify_exact_cache_plan_fail_open(*result);
        }
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
    std::uint64_t database_epoch;
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
    std::uint16_t relation_count;
    Oid relation_oids[kMaximumRelationDependencies];
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

/** Reap dead owners and converge unreferenced terminal slots to EMPTY. */
[[nodiscard]] std::uint64_t sweep_registry(bool expire_completed) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    const bool storage_attached = storage::storage_ready()
        || storage::attach_storage();
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t swept = 0;
    const TimestampTz now = GetCurrentTimestamp();
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, storage_attached);
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
        const bool terminal_reclaimable =
            entry.state == SharedEntryState::Retired
            && entry.waiter_count == 0
            && entry.reader_count == 0;
        if (!dead_producer && !expired && !terminal_reclaimable) {
            continue;
        }
        if (dead_producer) {
            fail_abandoned_producer(entry);
        } else if (expired) {
            entry.state = SharedEntryState::Retired;
            ConditionVariableBroadcast(&entry.changed);
        }
        const bool reclaimable =
            entry.state == SharedEntryState::Retired
            && entry.waiter_count == 0
            && entry.reader_count == 0;
        if (storage_attached && reclaimable && entry.manifest_handle) {
            manifests[manifest_count++] = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        if (reclaimable && !entry.manifest_handle) {
            recycle_registry_entry_locked(entry);
        }
        if (expired) {
            pg_atomic_fetch_add_u64(&g_shared->stats.evictions, 1);
        }
        ++swept;
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < manifest_count; ++index) {
        static_cast<void>(storage::retire_manifest(manifests[index]));
    }
    return swept;
}

/** Copy bounded diagnostics under lock; never hold shared locks during SQL I/O. */
[[nodiscard]] int snapshot_entries(EntrySnapshot* snapshots) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
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
        snapshot.database_epoch = entry.database_epoch;
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
        snapshot.relation_count = entry.relation_count
                <= kMaximumRelationDependencies
            ? entry.relation_count
            : static_cast<std::uint16_t>(kMaximumRelationDependencies);
        std::memcpy(
            snapshot.relation_oids,
            entry.relation_oids,
            static_cast<std::size_t>(snapshot.relation_count) * sizeof(Oid));
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
    const bool storage_attached = storage::storage_ready()
        || storage::attach_storage();
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t invalidated = 0;
    pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
    pg_atomic_fetch_add_u64(&g_shared->coarse_dependency_epoch, 1);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, storage_attached);
        if (entry.state == SharedEntryState::Producing) {
            fail_abandoned_producer(entry);
            ++invalidated;
            if (storage_attached && entry.reader_count == 0
                && entry.manifest_handle) {
                manifests[manifest_count++] = entry.manifest_handle;
                entry.manifest_handle = {};
            }
            continue;
        }
        if (entry.state != SharedEntryState::Complete) {
            continue;
        }
        entry.state = SharedEntryState::Retired;
        ++invalidated;
        if (storage_attached && entry.reader_count == 0
            && entry.manifest_handle) {
            manifests[manifest_count++] = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        ConditionVariableBroadcast(&entry.changed);
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < manifest_count; ++index) {
        static_cast<void>(storage::retire_manifest(manifests[index]));
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
    const bool storage_attached = storage::storage_ready()
        || storage::attach_storage();
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t invalidated = 0;
    pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    if (!bump_relation_epoch_locked(relation_oid)) {
        pg_atomic_fetch_add_u64(&g_shared->coarse_dependency_epoch, 1);
    }
    for (CacheEntry& entry : g_shared->entries) {
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, storage_attached);
        if (!entry_depends_on_relation(entry, relation_oid)) {
            continue;
        }
        if (entry.state == SharedEntryState::Producing) {
            fail_abandoned_producer(entry);
            ++invalidated;
            if (storage_attached && entry.reader_count == 0
                && entry.manifest_handle) {
                manifests[manifest_count++] = entry.manifest_handle;
                entry.manifest_handle = {};
            }
            continue;
        }
        if (entry.state != SharedEntryState::Complete) {
            continue;
        }
        entry.state = SharedEntryState::Retired;
        ++invalidated;
        if (storage_attached && entry.reader_count == 0
            && entry.manifest_handle) {
            manifests[manifest_count++] = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        ConditionVariableBroadcast(&entry.changed);
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < manifest_count; ++index) {
        static_cast<void>(storage::retire_manifest(manifests[index]));
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
        nulls[18] = true; /* V1 does not persist a producer Plan snapshot. */
        StringInfoData dependencies{};
        initStringInfo(&dependencies);
        appendStringInfo(
            &dependencies,
            "{\"database_epoch\":%llu,\"relation_oids\":[",
            static_cast<unsigned long long>(entry.database_epoch));
        for (std::uint16_t relation_index = 0;
             relation_index < entry.relation_count;
             ++relation_index) {
            appendStringInfo(
                &dependencies,
                "%s%u",
                relation_index == 0 ? "" : ",",
                entry.relation_oids[relation_index]);
        }
        appendStringInfoChar(&dependencies, ']');
        appendStringInfoChar(&dependencies, '}');
        values[19] = DirectFunctionCall1(
            jsonb_in, CStringGetDatum(dependencies.data));
        tuplestore_putvalues(tuple_store, tuple_descriptor, values, nulls);
        pfree(key);
        pfree(age);
        pfree(ttl);
        pfree(dependencies.data);
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

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_exact_plan_fault);
Datum bioseq_cache_test_set_exact_plan_fault(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (std::strcmp(name, "none") == 0) {
        g_test_exact_plan_fault = TestExactPlanFault::None;
    } else if (std::strcmp(name, "unknown_table_am") == 0) {
        g_test_exact_plan_fault =
            TestExactPlanFault::UnknownTableAccessMethod;
    } else if (std::strcmp(name, "unknown_index_am") == 0) {
        g_test_exact_plan_fault =
            TestExactPlanFault::UnknownIndexAccessMethod;
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown exact-plan verifier fault: %s", name)));
    }
    pfree(name);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_arm_pause);
Datum bioseq_cache_test_arm_pause(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const TestPausePoint point = parse_pause_point(name);
    const std::uint32_t bit = pause_bit(point);
    CacheTestControl& control = g_shared->test_control;
    static_cast<void>(DirectFunctionCall1(
        pg_advisory_lock_int8,
        Int64GetDatum(pause_lock_key(point))));
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
    const TestPausePoint point = parse_pause_point(name);
    const std::uint32_t bit = pause_bit(point);
    pfree(name);
    pg_atomic_fetch_or_u32(&g_shared->test_control.released_mask, bit);
    static_cast<void>(DirectFunctionCall1(
        pg_advisory_unlock_int8,
        Int64GetDatum(pause_lock_key(point))));
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

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_force_file_tier);
Datum bioseq_cache_test_set_force_file_tier(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    if (!storage::set_test_force_file_tier(PG_GETARG_BOOL(0))) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq_cache storage is unavailable")));
    }
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_storage_tier_stats);
Datum bioseq_cache_test_storage_tier_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    const storage::StorageTierTestSnapshot snapshot =
        storage::test_storage_tier_snapshot();
    Datum values[13]{
        BoolGetDatum(snapshot.force_file_tier),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.dsa_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.file_blocks)),
        Int64GetDatum(
            static_cast<std::int64_t>(snapshot.reserved_hot_bytes)),
        Int64GetDatum(
            static_cast<std::int64_t>(snapshot.reserved_file_bytes)),
        BoolGetDatum(snapshot.coherent),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.occupied_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.reserved_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.stored_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.referenced_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.retiring_blocks)),
        Int64GetDatum(
            static_cast<std::int64_t>(snapshot.release_committed_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.reader_pins)),
    };
    bool nulls[13]{};
    tuplestore_putvalues(store, descriptor, values, nulls);
    PG_RETURN_NULL();
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
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const bytea* value = PG_GETARG_BYTEA_PP(0);
    if (VARSIZE_ANY_EXHDR(value) != g_forced_coverage_domain_digest.bytes.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("forced coverage domain digest must contain 32 bytes")));
    }
    std::memcpy(
        g_forced_coverage_domain_digest.bytes.data(),
        VARDATA_ANY(value),
        g_forced_coverage_domain_digest.bytes.size());
    g_forced_coverage_domain_digest_active = true;
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_forced_coverage_domain_digest);
Datum bioseq_cache_test_clear_forced_coverage_domain_digest(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    g_forced_coverage_domain_digest = {};
    g_forced_coverage_domain_digest_active = false;
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
    storage::request_janitor_sweep(true);
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
    provider_registry::install_planner_bridge_observer_v1(
        observe_provider_rel_paths);
    if (!provider_registry::initialize_provider_registry_v1()) {
        ereport(WARNING,
            (errmsg("bioseq_cache provider rendezvous is owned by another library")));
    }
    if (process_shared_preload_libraries_in_progress) {
        g_preload_requested = true;
        if (!maintenance::register_static_janitor_worker()) {
            ereport(WARNING,
                (errmsg("bioseq_cache could not register its janitor worker")));
        }
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
    provider_registry::install_planner_bridge_observer_v1(nullptr);
    provider_registry::unpublish_provider_registry_v1();
}

}  // extern "C"
