/**
 * @file scanner/snapshot_rebind.cppm
 * @brief Set a scan source's snapshot after restoring parallel scan state.
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

/**
 * Set the snapshot restored from parallel scan shared memory.
 * Update the existing registry so its counted references still point to it.
 * This is allowed only before any source has opened its read resources.
 * @param provider Open scan provider on the current backend thread.
 * @param authoritative_registered_snapshot Registered MVCC snapshot that
 * must outlive the provider and any counted references.
 * @throws PostgreSQL ERROR for an invalid provider or snapshot, or if a source
 * has already opened a read domain.
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
