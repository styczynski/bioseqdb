/**
 * @file admin/rebuild/generation_route_implementation.cppm
 * @brief Choose and publish the stored format for a minimizer rebuild.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <new>
#include <string_view>

module bioseq.postgres.storage.indexing.admin.rebuild.generation_route;

import bioseq.postgres.storage.indexing.admin.rebuild.promoted_sequence;
import bioseq.postgres.storage.indexing.admin;
import bioseq.postgres.storage.indexing.admin.diagnostic_support;
import bioseq.common.logging;
import bioseq.postgres.support;
import bioseq.postgres.storage.manager;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.types.compact_sequence;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.sequences;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.type;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.postgres.storage.indexing.chunked_storage;
import bioseq.postgres.storage.indexing.handles;

namespace bioseq::storage::indexing::rebuild_detail {

/**
 * Use hash-major storage for supported sequence-wide CPU builds. Fragment
 * indexes, Vulkan builds, and other unsupported profiles use grouped-SoA.
 * After publication, check the source identity and counts in the returned metadata.
 */
template <class p_sequence_range>
inline void build_and_verify_hash_major_minimizer_generation(
    const locator_generation_rebuild_request& request,
    fragment_index_storage& storage,
    bioseq::storage::lazy_sequence_view<
        typename bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>::alphabet_type>
        sequence,
    bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range> source_binding) {
    using source_binding_type = bioseq::storage::postgres_lazy_sequence_binding<p_sequence_range>;
    const sequence_locator& locator = source_binding.locator();
    constexpr std::uint16_t k_alphabet_code = source_binding_type::alphabet_code();
    // The Vulkan sequence reader uses grouped-SoA. Publish that format while
    // keeping the selected GPU minimizer algorithm.
    if (request.index_name == k_minimizers_per_fragment_index_name ||
        request.backend == bioseq::minimizers::minimizer_sketch_backend::vulkan_segment) {
        build_and_verify_grouped_soa_minimizer_generation_pg(request.user_relation_oid,
                                                             request.column_attno,
                                                             request.index_name,
                                                             request.sequence_rowno,
                                                             storage,
                                                             sequence,
                                                             source_binding,
                                                             request.algorithm_identity,
                                                             request.backend,
                                                             request.kernels);
        return;
    }
    if constexpr (k_alphabet_code != 0u) {
        build_and_verify_grouped_soa_minimizer_generation_pg(request.user_relation_oid,
                                                             request.column_attno,
                                                             request.index_name,
                                                             request.sequence_rowno,
                                                             storage,
                                                             sequence,
                                                             source_binding,
                                                             request.algorithm_identity,
                                                             request.backend,
                                                             request.kernels);
        return;
    } else {
        const auto profile = bioseq::indexing::hash_major_minimizer_profile_for_backend(
            request.backend,
            request.algorithm_identity,
            k_alphabet_code,
            locator.logical_length);
        if (!profile) {
            build_and_verify_grouped_soa_minimizer_generation_pg(request.user_relation_oid,
                                                                 request.column_attno,
                                                                 request.index_name,
                                                                 request.sequence_rowno,
                                                                 storage,
                                                                 sequence,
                                                                 source_binding,
                                                                 request.algorithm_identity,
                                                                 request.backend,
                                                                 request.kernels);
            return;
        }
        const ready_hash_major_minimizer_publication publication =
            build_and_publish_hash_major_minimizer_generation_pg(request.user_relation_oid,
                                                                 request.column_attno,
                                                                 request.index_name,
                                                                 request.sequence_rowno,
                                                                 storage,
                                                                 sequence,
                                                                 source_binding,
                                                                 request.algorithm_identity,
                                                                 request.backend,
                                                                 request.kernels);
        if (!publication.published || publication.generation_id == 0u ||
            publication.query_key != request.sequence_rowno ||
            publication.source_epoch != locator.source_epoch ||
            publication.selected_backend != request.backend ||
            publication.head.generation_id != publication.generation_id ||
            publication.head.query_key != request.sequence_rowno ||
            publication.head.source_epoch != locator.source_epoch ||
            publication.manifest.generation_id != publication.generation_id ||
            publication.manifest.source_epoch != locator.source_epoch ||
            publication.manifest.target_length != locator.logical_length ||
            publication.unique_hash_count != publication.manifest.unique_hash_count ||
            publication.total_posting_count != publication.manifest.total_posting_count) {
            ready_minimizer_error_as_cpp(
                ERRCODE_DATA_CORRUPTED,
                "normal hash-major minimizer rebuild publication validation was rejected",
                fmt::format("index={} backend={}",
                            request.index_name,
                            bioseq::minimizers::minimizer_sketch_backend_name(request.backend)));
        }
    }
}

void build_and_verify_locator_minimizer_generation(
    const locator_generation_rebuild_request& request) {
    bioseq::pg_types::seq::dispatch_instance(
        request.alphabet_code,
        request.storage_width,
        [&request]<class p_sequence_type>() {
            using source_binding =
                bioseq::storage::postgres_lazy_sequence_binding<p_sequence_type>;
            const source_binding bound_source{
                request.database_oid,
                request.tablespace_oid,
                request.main_relfilenode,
                request.column_attno,
                request.locator,
                source_binding::expected_last_fragment_padding(request.locator),
                request.snapshot,
                request.caches,
                request.row_context};
            bioseq::postgres::support::invoke_pg_as_cpp([&request, bound_source] {
                with_promoted_pg_sequence_view<typename source_binding::alphabet_type>(
                    request.promoted_datum,
                    request.row_context,
                    [&request, bound_source](auto lazy_view) {
                        auto storage = open_fragment_storage(request.user_relation_oid,
                                                             request.database_oid,
                                                             request.tablespace_oid,
                                                             request.main_relfilenode,
                                                             request.column_attno,
                                                             request.index_name,
                                                             request.locator,
                                                             request.sequence_rowno,
                                                             request.caches,
                                                             request.work_context);
                        build_and_verify_hash_major_minimizer_generation<p_sequence_type>(
                            request, storage, lazy_view, bound_source);
                    });
            });
        });
}

} // namespace bioseq::storage::indexing::rebuild_detail
