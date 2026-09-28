/**
 * @file minimizer_persistence_policy_implementation.cppm
 * @brief Read minimizer settings through the PostgreSQL index catalog code.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/snapshot.h"
}

#include <cstdint>

module bioseq.postgres.storage.indexing.minimizer_persistence_policy;

import bioseq.postgres.storage.indexing.handles;

namespace bioseq::storage::indexing {

[[nodiscard]] minimizer_persistence_policy_resolution
resolve_minimizer_persistence_policy(std::uint32_t user_relid,
                                     std::uint32_t relfilenode,
                                     std::int16_t attno,
                                     SnapshotData* registered_snapshot) {
    return resolve_minimizer_persistence_policy_implementation(
        user_relid, relfilenode, attno, registered_snapshot);
}

} // namespace bioseq::storage::indexing
