/**
 * @file admin/rebuild/generation_route.cppm
 * @brief Internal locator-backed minimizer generation rebuild contract.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <string_view>

export module bioseq.postgres.storage.indexing.admin.rebuild.generation_route;

import bioseq.postgres.storage.manager;
import bioseq.indexing.minimizer_hashmap;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing::rebuild_detail {

/**
 * @brief Borrowed state required to rebuild one locator-backed minimizer generation.
 *
 * Relation and locator identities select the source and destination. The snapshot,
 * caches, memory contexts, promoted datum, and kernel table remain owned by the
 * caller and must outlive the call. The route publishes either grouped-SoA or the
 * registered hash-major representation and propagates PostgreSQL and C++ failures
 * after its local read-session and storage cleanup.
 */
struct locator_generation_rebuild_request {
    Oid user_relation_oid = InvalidOid;
    Oid database_oid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    sequence_locator locator{};
    Snapshot snapshot = nullptr;
    seq_caches* caches = nullptr;
    MemoryContext row_context = nullptr;
    MemoryContext work_context = nullptr;
    Datum promoted_datum = 0;
    std::string_view index_name{};
    std::uint64_t sequence_rowno = 0;
    std::uint8_t storage_width = 0;
    std::uint8_t alphabet_code = 0;
    bioseq::minimizers::minimizer_sketch_backend backend =
        bioseq::minimizers::minimizer_sketch_backend::scalar;
    bioseq::indexing::minimizer_generation_algorithm_identity algorithm_identity{};
    const bioseq::minimizers::minimizer_kernels* kernels = nullptr;
};

/**
 * @brief Build and validate one locator-backed persisted minimizer generation.
 * @param request Borrowed source, storage, snapshot, and backend state.
 *
 * The selected representation is published atomically. Storage owners and the
 * promoted sequence view are closed before any failure is propagated.
 */
void build_and_verify_locator_minimizer_generation(
    const locator_generation_rebuild_request& request);

} // namespace bioseq::storage::indexing::rebuild_detail
