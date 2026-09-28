/**
 * @file admin/rebuild/generation_route.cppm
 * @brief Rebuild a minimizer index from a stored sequence locator
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
 * Source, storage, and backend settings for one minimizer generation build.
 * The caller owns the snapshot, caches, memory contexts, sequence datum, and
 * kernel table. They must remain valid until the build returns.
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
 * Build and publish the supported grouped-SoA or hash-major format for this source.
 * Check the published identity and counts, then  close the source read session.
 * Failures are rethrown after local cleanup.
 */
void build_and_verify_locator_minimizer_generation(
    const locator_generation_rebuild_request& request);

} // namespace bioseq::storage::indexing::rebuild_detail
