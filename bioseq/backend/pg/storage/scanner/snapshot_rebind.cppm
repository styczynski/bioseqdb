/**
 * @file scanner/snapshot_rebind.cppm
 * @brief Parallel-worker snapshot rebinding for a scan source provider.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/errcodes.h"
#include "utils/snapmgr.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.storage.scanner.snapshot_rebind;

import bioseq.postgres.storage.scanner.source_provider_contract;

export namespace bioseq::storage {

/** Rebind a scan provider to the registered snapshot restored from its
 * parallel DSM coordinate. Upper owners may already retain leases on this
 * registry, so the registry is updated in place rather than replaced. No
 * source domain may have observed the preceding snapshot.
 * @param provider Open scan provider owned by the current backend thread.
 * @param authoritative_registered_snapshot Replacement registered MVCC
 * snapshot that must outlive the provider and derived leases.
 * @throws PostgreSQL ERROR when the provider is invalid or a domain is active.
 */
inline void
rebind_pg_sequence_source_provider_snapshot(pg_sequence_source_provider* provider,
                                            Snapshot authoritative_registered_snapshot) {
    using namespace sequence_source_detail;
    if (provider == nullptr || !provider->open ||
        provider->kind != pg_sequence_source_provider_kind::scan ||
        !registry_lease_is_live(provider->lease)) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan source provider cannot rebind its snapshot")));
    }
    auto* registry = provider->lease.registry;
    if (authoritative_registered_snapshot == nullptr ||
        !IsMVCCSnapshot(authoritative_registered_snapshot) ||
        authoritative_registered_snapshot->regd_count == 0 ||
        registry->authoritative_snapshot == nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan source provider requires a registered MVCC snapshot")));
    }
    if (registry->domains != nullptr) {
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("bioseq scan source provider cannot rebind after source activation")));
    }
    registry->authoritative_snapshot = authoritative_registered_snapshot;
}

} // namespace bioseq::storage
