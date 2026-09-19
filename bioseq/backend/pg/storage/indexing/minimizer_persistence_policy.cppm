/**
 * @file minimizer_persistence_policy.cppm
 * @brief Declares the persisted minimizer policy resolver.
 */
module;

#include <array>
#include <cstdint>

struct SnapshotData;

export module bioseq.postgres.storage.indexing.minimizer_persistence_policy;

import bioseq.indexing.type;

export namespace bioseq::storage::indexing {

/// Effective enablement and insert-maintenance policy for minimizer indexes.
struct minimizer_persistence_policy_resolution {
    std::array<bool, bioseq::indexing::k_minimizer_index_names.size()> enabled{};
    std::array<bool, bioseq::indexing::k_minimizer_index_names.size()> update_on_insert{};
};

/**
 * @brief Resolves persisted minimizer policy under a registered MVCC snapshot.
 * @param[in] user_relid User relation named by the policy rows.
 * @param[in] relfilenode Current main-relation relfilenode.
 * @param[in] attno One-based PostgreSQL column number.
 * @param[in] registered_snapshot Registered MVCC snapshot used for the scan.
 * @return Effective policy for every minimizer index.
 */
[[nodiscard]] minimizer_persistence_policy_resolution
resolve_minimizer_persistence_policy(std::uint32_t user_relid,
                                     std::uint32_t relfilenode,
                                     std::int16_t attno,
                                     SnapshotData* registered_snapshot);

} // namespace bioseq::storage::indexing
