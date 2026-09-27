/**
 * @file scanner/source_provider_contract.cppm
 * @brief Exact PostgreSQL source-provider ownership and layout contract.
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

/// Ownership boundary that permits reclaiming unused source domains.
enum class pg_sequence_source_sweep_boundary : std::uint8_t {
    /// Base provider has ended.
    provider_end,
    /// Upper executor owner released a route lease.
    upper_lease_end,
    /// Executor rescan may reuse an unchanged binding.
    rescan,
    /// Standalone index owner has ended.
    standalone_end,
    /// Top-level transaction cleanup is running.
    transaction_end,
    /// Creating subtransaction is ending.
    subtransaction_end,
};

/// Executor owner that created a source provider.
enum class pg_sequence_source_provider_kind : std::uint8_t {
    /// Provider follows a table scan lifecycle.
    scan,
    /// Provider follows standalone index administration.
    standalone_index,
};

namespace sequence_source_detail {
struct pg_sequence_source_domain;
}

/**
 * Transaction-owned registry for one verified relation/storage/snapshot flow.
 * It contains no Relation pointer and is never shared through DSM.
 */
struct pg_sequence_source_registry {
    /// Backend thread on which every registry operation must run.
    pg_sequence_backend_thread_token backend_thread;
    /// User-visible parent relation OID.
    Oid user_relid = InvalidOid;
    /// Database component of the parent physical identity.
    Oid database_oid = InvalidOid;
    /// Tablespace component of the parent physical identity.
    Oid tablespace_oid = InvalidOid;
    /// File component of the parent physical identity.
    Oid main_relfilenode = InvalidOid;
    /// Storage-generation identity authenticated when the provider is created.
    std::uint64_t main_storage_id = 0u;
    /* Bioseq's metapage storage id is the immutable binding generation.  Keep
     * the named copy in the registry key so a rel/attno pair can never reuse a
     * domain after its physical binding has been replaced. */
    std::uint64_t binding_generation = 0u;
    /// Borrowed registered MVCC snapshot governing source-catalog lookup.
    Snapshot authoritative_snapshot = nullptr;
    /// Subtransaction that created this registry.
    SubTransactionId creating_subid = InvalidSubTransactionId;
    /// Backend-local generation authenticating leases.
    std::uint64_t generation = 0u;
    /// Registry-owned linked list of source domains.
    sequence_source_detail::pg_sequence_source_domain* domains = nullptr;
    /// Link in the backend-local registry list.
    pg_sequence_source_registry* next = nullptr;
    /// Number of provider and upper-owner lease tokens.
    std::uint32_t active_leases = 0u;
    /// Whether authenticated leases may resolve new source lookups.
    bool accepting_lookups = false;
    /// Whether the base provider has released its token.
    bool provider_closed = false;
    /// Sticky lease-accounting failure.
    bool lease_fault = false;
    /// Whether top-level transaction cleanup detached the registry.
    bool xact_ended = false;
};

/** A checked upper/provider retain on a registry. */
struct pg_sequence_source_registry_lease {
    /// Borrowed registry control while `owned` is true.
    pg_sequence_source_registry* registry = nullptr;
    /// Generation that must match the current registry generation.
    std::uint64_t provider_generation = 0u;
    /// Whether this object owns one active-lease count.
    bool owned = false;
};

namespace sequence_source_detail {

/// Validate lease ownership, generation, lookup state and backend thread.
/// @param lease Candidate provider or upper-owner lease.
/// @return Whether this thread may use it for source lookup.
[[nodiscard]] inline bool registry_lease_is_live(
        const pg_sequence_source_registry_lease& lease) noexcept {
    return lease.owned && lease.registry != nullptr
        && !lease.registry->lease_fault
        && lease.provider_generation == lease.registry->generation
        && lease.registry->accepting_lookups
        && lease.registry->backend_thread.is_current();
}

} // namespace sequence_source_detail

/** Provider control owned by a base scan or standalone index owner. */
struct pg_sequence_source_provider {
    /// Provider-owned initial registry lease.
    pg_sequence_source_registry_lease lease{};
    /// Link in the backend-local provider list.
    pg_sequence_source_provider* next = nullptr;
    /// Executor lifecycle that owns this provider.
    pg_sequence_source_provider_kind kind =
        pg_sequence_source_provider_kind::scan;
    /// Whether provider operations may still use `lease`.
    bool open = false;
};

/** One source-provider route retained by an upper executor owner. */
struct pg_sequence_source_route {
    /// Parent relation axis of the route key.
    Oid relation_oid = InvalidOid;
    /// Independently retained registry lease.
    pg_sequence_source_registry_lease lease{};
};

static_assert(std::is_trivially_copyable_v<pg_sequence_source_route>);

/** Query-lifetime routing for joins, appends and nested upper wrappers.
 *
 * A PostgreSQL self join has independent registries even when every binding
 * names the same physical relation. The provider generation is therefore the
 * route key. `relation_oid` supplies the physical-relation axis. Two routes stay inline
 * for the common binary-join case. Larger legal plans grow in the executor's
 * query MemoryContext, before any new lease is retained.
 */
struct pg_sequence_source_route_set {
    /// Inline storage for the common one- or two-route plan.
    std::array<pg_sequence_source_route, 2u> inline_routes{};
    /// Memory-context-owned storage used after inline capacity is exceeded.
    pg_sequence_source_route* dynamic_routes = nullptr;
    /// Number of entries available in `dynamic_routes`.
    std::size_t dynamic_capacity = 0u;
    /// Number of live route entries.
    std::size_t count = 0u;
    /// Query-lifetime context owning dynamic storage.
    MemoryContext allocation_context = nullptr;
};

namespace sequence_source_detail {

/// Source-domain construction and cleanup state.
enum class domain_lifecycle : std::uint8_t {
    constructing,
    live,
    cleaning,
    faulted,
    invalid,
    released,
};

/// ResourceOwner release stages already completed for one domain.
enum class owner_release_stage_bits : std::uint8_t {
    none = 0u,
    before_locks = 1u,
    locks = 2u,
    after_locks = 4u,
};

/// Convert one release stage into its stored bit mask.
/// @param stage ResourceOwner release stage.
/// @return Underlying single-bit mask.
[[nodiscard]] constexpr std::uint8_t stage_bit(
        owner_release_stage_bits stage) noexcept {
    return static_cast<std::uint8_t>(stage);
}

/// Transaction-owned source domain with a registered snapshot, read cache and
/// staged ResourceOwner cleanup state.
struct pg_sequence_source_domain {
    /// Backend thread on which reads and cleanup must run.
    pg_sequence_backend_thread_token backend_thread;
    /// Borrowed registry while the registry token is owned.
    pg_sequence_source_registry* registry = nullptr;
    /// Link in the owning registry's domain list.
    pg_sequence_source_domain* next = nullptr;
    /// Transaction-child context owning scanners and read-cache allocation.
    MemoryContext payload_context = nullptr;
    /// Transaction-child owner for snapshot, relation, and lock resources.
    ResourceOwner resource_owner = nullptr;
    /// MVCC snapshot registered on `resource_owner`.
    Snapshot registered_snapshot = nullptr;
    /// Read-side fragment cache owned by `payload_context` and `resource_owner`.
    seq_caches* read_caches = nullptr;
    /// One-based parent sequence attribute number.
    AttrNumber column_attno = InvalidAttrNumber;
    /// Persisted alphabet and packed-unit width.
    compact_sequence_stdc_type_params stored_type{};
    /// Physical storage generation authenticated by the registry.
    std::uint64_t binding_generation = 0u;
    /// Subtransaction that created this domain.
    SubTransactionId creating_subid = InvalidSubTransactionId;
    /// Backend-local identity generation.
    std::uint64_t generation = 0u;
    /// Registry token plus retained expanded-value and scanner references.
    std::uint32_t refcount = 0u;
    /// Construction and cleanup lifecycle.
    domain_lifecycle lifecycle = domain_lifecycle::constructing;
    /// ResourceOwner release phases already attempted.
    std::uint8_t released_owner_stages = 0u;
    /// Whether the registry owns one reference count.
    bool registry_token_owned = false;
    /// Hint that only the registry token remains.
    bool reclaimable_hint = false;
    /// Sticky reference-count failure that prevents further reads.
    bool refcount_fault = false;
};

} // namespace sequence_source_detail
} // namespace bioseq::storage
