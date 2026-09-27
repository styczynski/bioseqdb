/**
 * @file index_maintenance_implementation.cppm
 * @brief Run configured index updates after a streamed table load.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstdint>

module bioseq.postgres.storage.io.index_maintenance;

import bioseq.postgres.storage.indexing.admin;

namespace bioseq::postgres::storage::io {

std::int64_t maintain_insert_policy_indexes(const Oid main_relation_oid) {
    return bioseq::storage::indexing::build_policy_update_on_insert_indexes(
        main_relation_oid);
}

} // namespace bioseq::postgres::storage::io
