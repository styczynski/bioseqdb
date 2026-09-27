/**
 * @file scanner/source_scanner/routes.cppm
 * @brief Keep and find sequence sources used by parent plan nodes.
 */
module;

extern "C" {
#include "postgres.h"

#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/errcodes.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

export module bioseq.postgres.storage.scanner:source_scanner_routes;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;
import bioseq.postgres.storage.manager;
import bioseq.storage.scanner;          // Source registry reference types.
import bioseq.storage.lazy_sequence;
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;
import bioseq.postgres.storage.metadata;
import bioseq.sequences;
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io;
import bioseq.parallelism.pool;
import bioseq.postgres.storage.scanner.fragment_scanner;
import :source_registry;

export import :source_scanner_provider;

export namespace bioseq::storage {
/// Set the query memory context for an empty route set.
/// Later growth allocates additional route storage there.
/// @throws PostgreSQL ERROR if the set already has entries or dynamic storage,
/// or the supplied context is null.
/// @param routes Empty set to initialize.
/// @param allocation_context Query context for additional storage.
inline void initialize_pg_sequence_source_routes(pg_sequence_source_route_set& routes,
                                                 MemoryContext allocation_context) {
    if (routes.count != 0u || routes.dynamic_routes != nullptr || allocation_context == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source route storage cannot be initialized")));
    }
    routes.allocation_context = allocation_context;
}

/// @return Number of routes that fit in the currently allocated storage.
[[nodiscard]] inline std::size_t
pg_sequence_source_route_capacity(const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr ? routes.inline_routes.size() : routes.dynamic_capacity;
}

/// @return Mutable route storage. Growth or context reset invalidates the pointer.
[[nodiscard]] inline pg_sequence_source_route*
pg_sequence_source_routes_data(pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr ? routes.inline_routes.data() : routes.dynamic_routes;
}

/// @return Read-only route storage. Growth or context reset invalidates the pointer.
[[nodiscard]] inline const pg_sequence_source_route*
pg_sequence_source_routes_data(const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr ? routes.inline_routes.data() : routes.dynamic_routes;
}

/// @return Mutable reference token at `index`, which must be below `routes.count`.
/// Growing the route array invalidates this reference.
/// @param index Zero-based index below `routes.count`.
[[nodiscard]] inline pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(pg_sequence_source_route_set& routes, std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

/// @return Read-only reference token at `index`, which must be below `routes.count`.
/// Growing the route array invalidates this reference.
/// @param index Zero-based index below `routes.count`.
[[nodiscard]] inline const pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(const pg_sequence_source_route_set& routes,
                               std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

/// Reserve room for at least `required` routes, preserving existing entries.
/// Capacity doubles as needed, up to PostgreSQL's allocation limit.
/// @throws PostgreSQL ERROR for invalid counts, excessive size or a missing context.
/// @param routes Initialized set whose entries are preserved.
/// @param required Minimum capacity in entries.
inline void ensure_pg_sequence_source_route_capacity(pg_sequence_source_route_set& routes,
                                                     std::size_t required) {
    const std::size_t current = pg_sequence_source_route_capacity(routes);
    if (routes.count > current) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED), errmsg("bioseq source route count is corrupt")));
    }
    if (required <= current)
        return;

    constexpr std::size_t entry_bytes = sizeof(pg_sequence_source_route);
    const std::size_t maximum = static_cast<std::size_t>(MaxAllocSize) / entry_bytes;
    if (required > maximum) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq source route set is too large")));
    }
    std::size_t capacity = current;
    while (capacity < required) {
        capacity = capacity > maximum / 2u ? maximum : capacity * 2u;
    }
    MemoryContext context = routes.allocation_context;
    if (context == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source route allocation context is unavailable")));
    }
    auto* grown = static_cast<pg_sequence_source_route*>(
        MemoryContextAllocZero(context, capacity * entry_bytes));
    std::memcpy(grown, pg_sequence_source_routes_data(routes), routes.count * entry_bytes);
    auto* old_dynamic = routes.dynamic_routes;
    routes.dynamic_routes = grown;
    routes.dynamic_capacity = capacity;
    if (old_dynamic != nullptr)
        pfree(old_dynamic);
}

/// Add a counted registry reference unless that registry is already present.
/// Grow storage before acquiring the reference, so allocation failure leaves
/// the registry's reference count unchanged.
/// @return Whether the source is live and its route is present afterward.
/// @throws PostgreSQL ERROR for corrupt counts, allocation or reference failure.
/// @param routes Initialized set to receive the reference.
/// @param parent Live registry reference to copy.
[[nodiscard]] inline bool
add_pg_sequence_source_route(pg_sequence_source_route_set& routes,
                             const pg_sequence_source_registry_lease& parent) {
    if (routes.count > pg_sequence_source_route_capacity(routes)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED), errmsg("bioseq source route count is corrupt")));
    }
    if (!sequence_source_detail::registry_lease_is_live(parent)) {
        return false;
    }
    const Oid relation_oid = parent.registry->user_relid;
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < routes.count; ++index) {
        if (entries[index].lease.registry == parent.registry) {
            return true;
        }
    }
    if (routes.count == (std::numeric_limits<std::size_t>::max)()) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq source route count overflow")));
    }
    ensure_pg_sequence_source_route_capacity(routes, routes.count + 1u);
    entries = pg_sequence_source_routes_data(routes);
    auto lease = acquire_pg_sequence_source_registry_lease(parent);
    entries[routes.count] = pg_sequence_source_route{
        .relation_oid = relation_oid,
        .lease = lease,
    };
    ++routes.count;
    return true;
}

/// Add a route from an open provider.
/// @return Whether the provider is open, its reference is live and the route is present.
/// @param routes Initialized set to receive the reference.
/// @param provider Open provider whose registry reference is copied.
[[nodiscard]] inline bool
add_pg_sequence_source_route(pg_sequence_source_route_set& routes,
                             const pg_sequence_source_provider* provider) {
    return provider != nullptr && provider->open &&
           add_pg_sequence_source_route(routes, provider->lease);
}

/// Undo routes appended after `original_count`, releasing their registry references.
/// Invalid counts do nothing. Cleanup errors are suppressed.
/// @param routes Partially modified route set.
/// @param original_count Number of leading entries to keep.
inline void rollback_pg_sequence_source_routes_noexcept(pg_sequence_source_route_set& routes,
                                                        std::size_t original_count) noexcept {
    const std::size_t capacity = pg_sequence_source_route_capacity(routes);
    if (original_count > routes.count || routes.count > capacity)
        return;
    auto* entries = pg_sequence_source_routes_data(routes);
    while (routes.count > original_count) {
        const std::size_t index = routes.count - 1u;
        release_pg_sequence_source_registry_lease_noexcept(
            entries[index].lease, pg_sequence_source_sweep_boundary::upper_lease_end);
        entries[index] = {};
        routes.count = index;
    }
}

/**
 * Add a counted reference for each source registry missing from the destination.
 * Duplicate registries share one route. The source remains unchanged.
 * Failure releases references added by this call and restores the original
 * destination count. Its allocated capacity may remain larger.
 * @param destination Initialized set that receives the references.
 * @param source Set to copy from.
 * @return Whether every source route was valid and merged.
 * @throws PostgreSQL ERROR for corrupt counts, allocation or reference failure.
 */
[[nodiscard]] inline bool
merge_pg_sequence_source_routes(pg_sequence_source_route_set& destination,
                                const pg_sequence_source_route_set& source) {
    using namespace sequence_source_detail;
    if (destination.count > pg_sequence_source_route_capacity(destination) ||
        source.count > pg_sequence_source_route_capacity(source)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED), errmsg("bioseq source route count is corrupt")));
    }
    std::size_t distinct_new_routes = 0u;
    const auto* source_entries = pg_sequence_source_routes_data(source);
    const auto* destination_entries = pg_sequence_source_routes_data(destination);
    for (std::size_t source_index = 0u; source_index < source.count; ++source_index) {
        const auto& source_route = source_entries[source_index];
        const auto& source_lease = source_route.lease;
        if (!registry_lease_is_live(source_lease) ||
            source_route.relation_oid != source_lease.registry->user_relid) {
            return false;
        }
        bool already_present = false;
        for (std::size_t destination_index = 0u; destination_index < destination.count;
             ++destination_index) {
            if (destination_entries[destination_index].lease.registry == source_lease.registry) {
                already_present = true;
                break;
            }
        }
        if (!already_present) {
            for (std::size_t prior = 0u; prior < source_index; ++prior) {
                if (source_entries[prior].lease.registry == source_lease.registry) {
                    already_present = true;
                    break;
                }
            }
        }
        if (!already_present) {
            ++distinct_new_routes;
        }
    }
    if (distinct_new_routes > (std::numeric_limits<std::size_t>::max)() - destination.count) {
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq source route count overflow")));
    }
    ensure_pg_sequence_source_route_capacity(destination, destination.count + distinct_new_routes);
    const std::size_t original_count = destination.count;
    volatile bool merge_ok = true;
    PG_TRY();
    {
        for (std::size_t source_index = 0u; source_index < source.count; ++source_index) {
            if (!add_pg_sequence_source_route(destination, source_entries[source_index].lease)) {
                merge_ok = false;
                break;
            }
        }
    }
    PG_CATCH();
    {
        rollback_pg_sequence_source_routes_noexcept(destination, original_count);
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (!merge_ok) {
        rollback_pg_sequence_source_routes_noexcept(destination, original_count);
    }
    return merge_ok;
}

/// Find a live route for the table and provider generation.
/// @return Borrowed reference token, or null for no match, ambiguity or an
/// invalid route anywhere in the set.
/// @param routes Set to search.
/// @param relation_oid Parent table OID.
/// @param provider_generation Generation identifying the table scan.
[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(const pg_sequence_source_route_set& routes,
                              Oid relation_oid,
                              std::uint64_t provider_generation) noexcept {
    if (routes.count > pg_sequence_source_route_capacity(routes) || !OidIsValid(relation_oid) ||
        provider_generation == 0u) {
        return nullptr;
    }
    const auto* entries = pg_sequence_source_routes_data(routes);
    const pg_sequence_source_registry_lease* match = nullptr;
    for (std::size_t index = 0u; index < routes.count; ++index) {
        const auto& route = entries[index];
        const auto& lease = route.lease;
        if (!sequence_source_detail::registry_lease_is_live(lease) ||
            route.relation_oid != lease.registry->user_relid) {
            return nullptr;
        }
        if (route.relation_oid != relation_oid ||
            lease.provider_generation != provider_generation) {
            continue;
        }
        if (match != nullptr && match->registry != lease.registry) {
            return nullptr;
        }
        match = &lease;
    }
    return match;
}

/**
 * Find the single live registry for a table. A self join with separate
 * registries is ambiguous and returns null.
 * @param routes Set to search.
 * @param relation_oid Parent table OID.
 * @return Borrowed reference token, or null for no match, ambiguity or an
 * invalid route anywhere in the set.
 */
[[nodiscard]] inline const pg_sequence_source_registry_lease*
find_pg_sequence_source_route(const pg_sequence_source_route_set& routes,
                              Oid relation_oid) noexcept {
    if (routes.count > pg_sequence_source_route_capacity(routes) || !OidIsValid(relation_oid)) {
        return nullptr;
    }
    const auto* entries = pg_sequence_source_routes_data(routes);
    const pg_sequence_source_registry_lease* match = nullptr;
    for (std::size_t index = 0u; index < routes.count; ++index) {
        const auto& route = entries[index];
        const auto& lease = route.lease;
        if (!sequence_source_detail::registry_lease_is_live(lease) ||
            route.relation_oid != lease.registry->user_relid) {
            return nullptr;
        }
        if (route.relation_oid != relation_oid) {
            continue;
        }
        if (match != nullptr && match->registry != lease.registry) {
            return nullptr;
        }
        match = &lease;
    }
    return match;
}

/// Release every registry reference and empty the route set.
/// @param is_commit Report cleanup errors when true. Suppress them during abort.
/// @throws PostgreSQL ERROR for corrupt counts, invalid references or cleanup failure.
/// @param routes Set cleared after releasing its references.
inline void release_pg_sequence_source_routes(pg_sequence_source_route_set& routes,
                                              bool is_commit) {
    if (routes.count > pg_sequence_source_route_capacity(routes)) {
        ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED), errmsg("bioseq source route count is corrupt")));
    }
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < routes.count; ++index) {
        if (!release_pg_sequence_source_registry_lease(
                entries[index].lease,
                pg_sequence_source_sweep_boundary::upper_lease_end,
                is_commit)) {
            ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq source route lease accounting is corrupt")));
        }
        entries[index].relation_oid = InvalidOid;
    }
    routes.count = 0u;
}

/// Release routes during abort, suppressing cleanup failures.
/// Limit traversal to allocated storage if the count is corrupt, then clear the count.
/// @param routes Set whose count is cleared.
inline void
release_pg_sequence_source_routes_noexcept(pg_sequence_source_route_set& routes) noexcept {
    const std::size_t safe_count =
        std::min<std::size_t>(routes.count, pg_sequence_source_route_capacity(routes));
    auto* entries = pg_sequence_source_routes_data(routes);
    for (std::size_t index = 0u; index < safe_count; ++index) {
        release_pg_sequence_source_registry_lease_noexcept(
            entries[index].lease, pg_sequence_source_sweep_boundary::upper_lease_end);
        entries[index].relation_oid = InvalidOid;
    }
    routes.count = 0u;
}

} // namespace bioseq::storage
