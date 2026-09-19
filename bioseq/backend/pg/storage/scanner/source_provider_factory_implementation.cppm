/**
 * @file scanner/source_provider_factory_implementation.cppm
 * @brief Implements the narrow scan-provider factory through its scanner owner.
 */
module;

#include <bioseq/pg_compat.h>

module bioseq.postgres.storage.scanner.source_provider_factory;

import bioseq.postgres.storage.scanner;

namespace bioseq::storage {

[[nodiscard]] pg_sequence_source_provider*
create_scan_pg_sequence_source_provider(Relation owning_relation,
                                        Snapshot authoritative_registered_snapshot)
{
    return create_pg_sequence_source_provider_owner_boundary(
        owning_relation, authoritative_registered_snapshot);
}

} // namespace bioseq::storage
