/**
 * @file scanner/source_provider_factory_implementation.cppm
 * @brief Connect the table-scan factory to sequence source creation.
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

/** Closes a provider through its checked or abort cleanup, preserving the existing behavior. */
void close_pg_sequence_source_provider(pg_sequence_source_provider* provider,
                                       bool is_commit) {
    if (provider == nullptr) {
        return;
    }
    if (is_commit) {
        sequence_source_detail::close_provider_checked(*provider, true);
    } else {
        sequence_source_detail::close_provider_abort_noerror(*provider);
    }
}

} // namespace bioseq::storage
