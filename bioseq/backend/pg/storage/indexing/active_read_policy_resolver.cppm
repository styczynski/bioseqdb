/**
 * @file active_read_policy_resolver.cppm
 * @brief Declares the executor-scoped active-read policy resolver.
 */
module;

#include <cstdint>

export module bioseq.postgres.storage.indexing.active_read_policy_resolver;

export import bioseq.postgres.storage.indexing.active_read_policy_resolution_contract;
import bioseq.postgres.storage.indexing.executor_read_context;
import bioseq.postgres.storage.indexing.minimizer_read_stats;

export namespace bioseq::storage::indexing {

/**
 * @brief Resolves the active read index for an executor-owned relation column.
 * @param[in] executor_read Borrowed executor relation and snapshot context.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in,out] cold_stats Optional counters updated for catalog and lock work.
 * @return Selected index name and its policy source.
 */
[[nodiscard]] active_read_policy_resolution
resolve_active_read_index_outcome(const borrowed_executor_read_context& executor_read,
                                  std::int16_t attno,
                                  cold_index_preparation_stats* cold_stats = nullptr);

} // namespace bioseq::storage::indexing
