/**
 * @file scanner/source_provider_contract.cppm
 * @brief Shared state that keeps PostgreSQL sequence readers alive.
 */
module;

extern "C" {
#include "postgres.h"
#include "access/attnum.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.postgres.storage.scanner.source_provider_contract;

import bioseq.postgres.storage.manager;
import bioseq.postgres.types.compact_sequence;
import bioseq.sequences.compact_sequence_typmod;

export namespace bioseq::storage {

/// Events at which unused sequence read resources can be released.
enum class pg_sequence_source_sweep_boundary : std::uint8_t {
    /// Base provider has ended.
    provider_end,
    /// A parent plan node released its reference to the source registry.
    upper_lease_end,
    /// A rescan can reuse readers for the same stored data.
    rescan,
    /// A standalone index operation has ended.
    standalone_end,
    /// Top-level transaction cleanup is running.
    transaction_end,
    /// Creating subtransaction is ending.
    subtransaction_end,
};

/// Operation that owns a source provider.
enum class pg_sequence_source_provider_kind : std::uint8_t {
    /// Provider belongs to a table scan.
    scan,
    /// Provider belongs to an index administration operation.
    standalone_index,
};

namespace sequence_source_detail {
struct pg_sequence_source_domain;
}

/**
 * Track sequence read resources for one table, storage version and snapshot.
 * The registry stores table identifiers without holding a Relation pointer.
 * It belongs to this backend and is never sent through parallel shared memory.
 */
struct pg_sequence_source_registry {
    /// Backend thread on which every registry operation must run.
    pg_sequence_backend_thread_token backend_thread;
    /// User-visible parent relation OID.
    Oid user_relid = InvalidOid;
    /// Parent database OID.
    Oid database_oid = InvalidOid;
    /// Parent tablespace OID.
    Oid tablespace_oid = InvalidOid;
    /// Parent table file number.
    Oid main_relfilenode = InvalidOid;
    /// Storage ID read from the parent table when the provider opens.
    std::uint64_t main_storage_id = 0u;
    /* Copy of the table's storage ID used to match read domains. A replaced
     * table must get new readers even if its relation and column IDs match. */
    std::uint64_t binding_generation = 0u;
    /// Borrowed registered MVCC snapshot used to look up source catalog rows.
    Snapshot authoritative_snapshot = nullptr;
    /// Subtransaction that created this registry.
    SubTransactionId creating_subid = InvalidSubTransactionId;
    /// Backend-local number used to reject stale registry references.
    std::uint64_t generation = 0u;
    /// Read domains owned by this registry.
    sequence_source_detail::pg_sequence_source_domain* domains = nullptr;
    /// Link in the backend-local registry list.
    pg_sequence_source_registry* next = nullptr;
    /// Number of references held by the provider and parent plan nodes.
    std::uint32_t active_leases = 0u;
    /// Whether valid registry references may open sequence readers.
    bool accepting_lookups = false;
    /// Whether the original provider has closed.
    bool provider_closed = false;
    /// A reference-count error occurred. Further lookups are rejected.
    bool lease_fault = false;
    /// Whether transaction cleanup detached this registry.
    bool xact_ended = false;
};

/** A counted reference that keeps a source registry alive. */
struct pg_sequence_source_registry_lease {
    /// Referenced registry while `owned` is true.
    pg_sequence_source_registry* registry = nullptr;
    /// Generation that must match the current registry generation.
    std::uint64_t provider_generation = 0u;
    /// Whether this object accounts for one active registry reference.
    bool owned = false;
};

namespace sequence_source_detail {

/// Check that this reference still permits lookups on the current backend thread.
/// Reject a missing reference, a stale generation or a registry with failed
/// reference accounting.
/// @param lease Reference to check.
/// @return Whether the current backend thread may use it for source lookups.
[[nodiscard]] inline bool registry_lease_is_live(
        const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.owned && lease.registry != nullptr
        && !lease.registry->lease_fault
        && lease.provider_generation == lease.registry->generation
        && lease.registry->accepting_lookups
        && lease.registry->backend_thread.is_current();
}

} // namespace sequence_source_detail

/** Source registry reference owned by a table scan or index operation. */
struct pg_sequence_source_provider {
    /// Initial registry reference held by this provider.
    pg_sequence_source_registry_lease lease{};
    /// Link in the backend-local provider list.
    pg_sequence_source_provider* next = nullptr;
    /// Operation responsible for closing this provider.
    pg_sequence_source_provider_kind kind =
        pg_sequence_source_provider_kind::scan;
    /// Whether provider operations may still use `lease`.
    bool open = false;
};

/** Reference through which a parent plan node can read a sequence source. */
struct pg_sequence_source_route {
    /// Parent table OID used to find this route.
    Oid relation_oid = InvalidOid;
    /// Registry reference held by this route.
    pg_sequence_source_registry_lease lease{};
};

static_assert(std::is_trivially_copyable_v<pg_sequence_source_route>);

/**
 * Sequence sources held by joins, appends and other parent plan nodes.
 * Self joins need separate sources even when both sides read the same table,
 * so routes identify the provider generation as well as the table OID.
 * Two routes fit in the object. Additional routes use the query memory context.
 * Grow that storage before acquiring another registry reference.
 */
struct pg_sequence_source_route_set {
    /// Space for up to two routes without a separate allocation.
    std::array<pg_sequence_source_route, 2u> inline_routes{};
    /// Additional route storage allocated in the query memory context.
    pg_sequence_source_route* dynamic_routes = nullptr;
    /// Number of entries available in `dynamic_routes`.
    std::size_t dynamic_capacity = 0u;
    /// Number of live route entries.
    std::size_t count = 0u;
    /// Query-lifetime context owning dynamic storage.
    MemoryContext allocation_context = nullptr;
};

namespace sequence_source_detail {

/// Whether a read domain is being created, used or cleaned up.
enum class domain_lifecycle : std::uint8_t {
    constructing,
    live,
    cleaning,
    faulted,
    invalid,
    released,
};

/// ResourceOwner cleanup stages recorded for a domain.
enum class owner_release_stage_bits : std::uint8_t {
    none = 0u,
    before_locks = 1u,
    locks = 2u,
    after_locks = 4u,
};

/// @param stage ResourceOwner cleanup stage.
/// @return Bit mask for the stage, or zero for `none`.
[[nodiscard]] constexpr std::uint8_t stage_bit(
        owner_release_stage_bits stage) noexcept {
    return static_cast<std::uint8_t>(stage);
}

/// Shared read resources for one sequence column and packed type.
/// Keeps a snapshot registered and owns fragment readers until cleanup.
struct pg_sequence_source_domain {
    /// Backend thread on which reads and cleanup must run.
    pg_sequence_backend_thread_token backend_thread;
    /// Registry that holds the initial reference to this domain.
    pg_sequence_source_registry* registry = nullptr;
    /// Link in the owning registry's domain list.
    pg_sequence_source_domain* next = nullptr;
    /// Context below TopTransactionContext for scanner and cache memory.
    MemoryContext payload_context = nullptr;
    /// Child of the transaction ResourceOwner for snapshots, relations and locks.
    ResourceOwner resource_owner = nullptr;
    /// MVCC snapshot registered on `resource_owner`.
    Snapshot registered_snapshot = nullptr;
    /// Read-side fragment cache owned by `payload_context` and `resource_owner`.
    seq_caches* read_caches = nullptr;
    /// One-based sequence column number in the parent table.
    AttrNumber column_attno = InvalidAttrNumber;
    /// Persisted alphabet and packed-unit width.
    compact_sequence_stdc_type_params stored_type{};
    /// Parent storage ID copied from the registry.
    std::uint64_t binding_generation = 0u;
    /// Subtransaction that created this domain.
    SubTransactionId creating_subid = InvalidSubTransactionId;
    /// Backend-local number distinguishing this domain from earlier allocations.
    std::uint64_t generation = 0u;
    /// References held by the registry, expanded sequence values and scanners.
    std::uint32_t refcount = 0u;
    /// Current state of domain creation, use or cleanup.
    domain_lifecycle lifecycle = domain_lifecycle::constructing;
    /// ResourceOwner release phases already attempted.
    std::uint8_t released_owner_stages = 0u;
    /// Whether the registry holds one reference to this domain.
    bool registry_token_owned = false;
    /// The registry may now hold the only remaining reference.
    bool reclaimable_hint = false;
    /// A reference-count error occurred. Further reads are rejected.
    bool refcount_fault = false;
};

} // namespace sequence_source_detail
} // namespace bioseq::storage
