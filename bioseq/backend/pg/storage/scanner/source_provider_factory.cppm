/**
 * @file scanner/source_provider_factory.cppm
 * @brief Create sequence source providers for table scans.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.scanner.source_provider_factory;

export import bioseq.postgres.storage.scanner.source_provider_contract;

export namespace bioseq::storage {

/**
 * Create a source provider for a table scan. The scan owns the provider and
 * must close it through the source-provider cleanup functions.
 * Keep the MVCC snapshot registered until the provider and all counted
 * registry references have closed.
 */
[[nodiscard]] pg_sequence_source_provider*
create_scan_pg_sequence_source_provider(Relation owning_relation,
                                        Snapshot authoritative_registered_snapshot);

} // namespace bioseq::storage
