/**
 * @file minimizer_persistence_policy.cppm
 * @brief Read saved settings for minimizer indexes on a sequence column.
 */
module;

#include <array>
#include <cstdint>

struct SnapshotData;

export module bioseq.postgres.storage.indexing.minimizer_persistence_policy;

import bioseq.indexing.type;

export namespace bioseq::storage::indexing {

/// Whether each minimizer index is enabled and updated when rows are inserted.
struct minimizer_persistence_policy_resolution {
    std::array<bool, bioseq::indexing::k_minimizer_index_names.size()> enabled{};
    std::array<bool, bioseq::indexing::k_minimizer_index_names.size()> update_on_insert{};
};

/**
 * @brief Read minimizer index settings visible to the supplied snapshot.
 *
 * Missing catalog rows use the index defaults. The caller keeps the MVCC
 * snapshot registered throughout the lookup.
 *
 * @param[in] user_relid User table OID.
 * @param[in] relfilenode Current main table file number.
 * @param[in] attno One-based sequence column number.
 * @param[in] registered_snapshot Snapshot used to read the settings.
 * @return Enabled and update-on-insert flags for each minimizer index.
 */
[[nodiscard]] minimizer_persistence_policy_resolution
resolve_minimizer_persistence_policy(std::uint32_t user_relid,
                                     std::uint32_t relfilenode,
                                     std::int16_t attno,
                                     SnapshotData* registered_snapshot);

} // namespace bioseq::storage::indexing
