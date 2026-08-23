/**
 * @file scanner/source_provider_factory.cppm
 * @brief Narrow typed factory for scan-owned PostgreSQL sequence providers.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.storage.scanner.source_provider_factory;

export import bioseq.postgres.storage.scanner.source_provider_contract;

export namespace bioseq::storage {

/**
 * Create a source provider owned by one table-scan runtime.
 *
 * The returned typed provider retains the canonical scanner lifecycle and
 * must be released through the existing provider cleanup path.
 */
[[nodiscard]] pg_sequence_source_provider*
create_scan_pg_sequence_source_provider(Relation owning_relation,
                                        Snapshot authoritative_registered_snapshot);

} // namespace bioseq::storage
