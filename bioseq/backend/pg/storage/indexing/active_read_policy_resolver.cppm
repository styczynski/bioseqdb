/**
 * @file active_read_policy_resolver.cppm
 * @brief Select a column's active index using the executor's snapshot.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.indexing.active_read_policy_resolver;

export import bioseq.postgres.storage.indexing.active_read_policy_resolution_contract;
import bioseq.postgres.storage.indexing.executor_read_context;
import bioseq.postgres.storage.indexing.minimizer_read_stats;

export namespace bioseq::storage::indexing {

/**
 * Read the enabled active index for this relation column using the executor's
 * snapshot. Use the default when no active row exists, and reject multiple
 * active rows.
 * @param executor_read Borrowed relation context with a registered MVCC snapshot.
 * @param attno One-based PostgreSQL column number.
 * @param cold_stats Optional counters for catalog reads and locks.
 * @return Selected index name and the source of that choice.
 */
[[nodiscard]] active_read_policy_resolution
resolve_active_read_index_outcome(const borrowed_executor_read_context& executor_read,
                                  std::int16_t attno,
                                  cold_index_preparation_stats* cold_stats = nullptr);

} // namespace bioseq::storage::indexing
