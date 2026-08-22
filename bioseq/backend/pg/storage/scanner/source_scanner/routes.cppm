/**
 * @file scanner/source_scanner/routes.cppm
 * @brief Authenticated source-route storage and lookup.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.scanner:source_scanner_routes;

import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.configuration.core;
import bioseq.postgres.support;
import bioseq.common.seqan_adapter;     // seq::dna4 in static_assert below
import bioseq.postgres.storage.manager; // seq_caches + open_sub_table_for_locator
import bioseq.storage.scanner;          // scanner concepts
import bioseq.storage.lazy_sequence;    // lazy_sequence + with_postgres_lazy_sequence
import bioseq.storage.coordinates;
import bioseq.storage.layout;
import bioseq.postgres.storage.physical;
import bioseq.storage.size;              // sequence_size_t
import bioseq.postgres.storage.metadata; // stripe_metadata/find_stripe_by_row_number
import bioseq.sequences;                 // compact_sequence_range, compact_sequence_stdc_instance
import bioseq.postgres.types.compact_sequence;
import bioseq.postgres.table.io; // begin_column_read/read_column_row_by_number
import bioseq.parallelism.pool;
import :fragment_scanner;
import :source_registry;

export import :source_scanner_provider;

export namespace bioseq::storage {
/// Initialize empty route storage in one query-lifetime memory context.
/// @param routes Empty route set to initialize.
/// @param allocation_context Context that owns any future dynamic route array.
/// @throws PostgreSQL ERROR when the set is nonempty or the context is null.
inline void initialize_pg_sequence_source_routes(pg_sequence_source_route_set& routes,
                                                 MemoryContext allocation_context) {
    if (routes.count != 0u || routes.dynamic_routes != nullptr || allocation_context == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq source route storage cannot be initialized")));
    }
    routes.allocation_context = allocation_context;
}

/// Return inline or dynamic route capacity.
/// @return Number of routes that fit without allocation.
[[nodiscard]] inline std::size_t
pg_sequence_source_route_capacity(const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr ? routes.inline_routes.size() : routes.dynamic_capacity;
}

/// Return mutable contiguous route storage.
/// @return Pointer valid until capacity grows or the allocation context resets.
[[nodiscard]] inline pg_sequence_source_route*
pg_sequence_source_routes_data(pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr ? routes.inline_routes.data() : routes.dynamic_routes;
}

/// Return immutable contiguous route storage.
/// @return Pointer valid until capacity grows or the allocation context resets.
[[nodiscard]] inline const pg_sequence_source_route*
pg_sequence_source_routes_data(const pg_sequence_source_route_set& routes) noexcept {
    return routes.dynamic_routes == nullptr ? routes.inline_routes.data() : routes.dynamic_routes;
}

/// Return one mutable route lease by validated caller index.
/// @param index Zero-based index that must be less than `routes.count`.
/// @return Mutable lease reference invalidated by route-storage growth.
[[nodiscard]] inline pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(pg_sequence_source_route_set& routes, std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

/// Return one immutable route lease by validated caller index.
/// @param index Zero-based index that must be less than `routes.count`.
/// @return Immutable lease reference invalidated by route-storage growth.
[[nodiscard]] inline const pg_sequence_source_registry_lease&
pg_sequence_source_route_lease(const pg_sequence_source_route_set& routes,
                               std::size_t index) noexcept {
    return pg_sequence_source_routes_data(routes)[index].lease;
}

/// Grow route storage geometrically inside its allocation context.
/// @param routes Initialized route set whose entries are preserved.
/// @param required Minimum required capacity in entries.
/// @throws PostgreSQL ERROR for corrupt counts, allocation limits, or a missing context.
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

/// Retain and append one distinct registry route.
/// @param routes Initialized destination route set.
/// @param parent Live lease retained independently when not already present.
/// @return Whether the parent was live and the route is present afterward.
/// @throws PostgreSQL ERROR for corrupt counts, overflow, allocation, or lease failure.
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

/// Append the route exposed by one open provider.
/// @param routes Initialized destination route set.
/// @param provider Provider whose lease is retained independently.
/// @return Whether the provider was open and its route is present afterward.
[[nodiscard]] inline bool
add_pg_sequence_source_route(pg_sequence_source_route_set& routes,
                             const pg_sequence_source_provider* provider) {
    return provider != nullptr && provider->open &&
           add_pg_sequence_source_route(routes, provider->lease);
}

/// Release routes appended after one saved count.
/// @param routes Route set modified by a partially completed operation.
/// @param original_count Prefix length retained by the caller.
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
 * @brief Acquire an independent upper-owner copy of every opaque child route.
 * @param destination Initialized route set receiving distinct leases.
 * @param source Borrowed route set left unchanged.
 * @return Whether every source route was live and merged.
 * @throws PostgreSQL ERROR for corrupt counts, allocation, or lease failure.
 *
 * Failure rolls `destination` back to its original count.
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

/// Find the unique live route matching a relation and provider generation.
/// @param routes Borrowed route set.
/// @param relation_oid Parent relation identity.
/// @param provider_generation Exact scan-binding generation.
/// @return Borrowed lease pointer, or null for invalid, absent, stale, or ambiguous routes.
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

/** Relation-only lookup for validated single-source upper nodes.
 * It deliberately fails closed when self-join aliases contribute multiple
 * live registries for the same physical relation.
 * @param routes Borrowed route set.
 * @param relation_oid Parent relation identity.
 * @return Borrowed unique lease, or null for invalid, absent, stale, or ambiguous routes.
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

/// Release every route lease through the checked upper-owner boundary.
/// @param routes Route set cleared after releasing each owned lease.
/// @param is_commit Whether checked commit cleanup is permitted.
/// @throws PostgreSQL ERROR for corrupt counts or lease accounting.
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

/// Release every valid route prefix through the abort cleanup path.
/// @param routes Route set cleared without propagating cleanup failures.
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
