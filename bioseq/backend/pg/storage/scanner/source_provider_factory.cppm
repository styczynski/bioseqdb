/**
 * @file scanner/source_provider_factory.cppm
 * @brief Create sequence source providers for table scans.
 */
module;

extern "C" {
#include "postgres.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.storage.scanner.source_provider_factory;

export import bioseq.postgres.storage.scanner.source_provider_contract;

export namespace bioseq::storage {

/**
 * Create a source provider for, a table scan. The scan owns the provider and
 * must close it through the source-provider cleanup functions.
 * Keep the MVCC snapshot registered until the provider and all counted
 * registry references have closed.
 */
[[nodiscard]] pg_sequence_source_provider*
create_scan_pg_sequence_source_provider(Relation owning_relation,
                                        Snapshot authoritative_registered_snapshot);

// The existing implementation unit owns cleanup so callers need only this contract,
// without importing the complete scanner and its template implementations.
/// Close and free a source provider. A null pointer does nothing.
/// @param is_commit Report cleanup errors when true. Suppress them during abort.
/// @throws PostgreSQL ERROR if commit cleanup fails or detects invalid state.
/// @param provider Provider  to close, or null. Invalid after a successful return.
void close_pg_sequence_source_provider(pg_sequence_source_provider* provider,
                                       bool is_commit);

// Parent plan nodes retain providers without importing the scanner templates.
// The scanner implementation preserves the existing reference and abort rules.
namespace source_routes {
[[nodiscard]] bool add(pg_sequence_source_route_set& routes,
                       const pg_sequence_source_provider* provider);
[[nodiscard]] bool merge(pg_sequence_source_route_set& destination,
                         const pg_sequence_source_route_set& source);
void release_noexcept(pg_sequence_source_route_set& routes) noexcept;
}

} // namespace bioseq::storage
