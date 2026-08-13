/**
 * @file pipeline/pipe/fragment_index_dna4.cppm
 * @brief Owns the dna4 canonical-vector fragment-index pipeline entry.
 */
module;

#include <bioseq/pg_build_profile.h>

#include <utility>
#include <vector>

module bioseq.pipeline.pipe;

import bioseq.common.seqan_adapter;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.kmers;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.gpu_minimizer_index;

namespace bioseq::simd {

[[nodiscard, gnu::noinline]] match_result_collection
minimap2_port_with_index_owned(
    bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
    bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
    const std::vector<::bioseq::minimizers::minimap_sketch_data>&
        query_minimizers,
    const ::bioseq::indexing::minimizer_hashmap_fragment_index&
        prebuilt_target_index,
    const minimap2_parameters& params,
    bioseq::indexing::gpu_minimizer_index_cache* gpu_index_cache,
    bioseq::indexing::gpu_index_cache_identity gpu_index_identity,
    bool allow_gpu_target_index_build)
{
    return minimap2_port_with_index<
        ::bioseq::minimizers::minimap_sketch_data,
        ::seq::dna4,
        ::bioseq::indexing::minimizer_hashmap_fragment_index>(
            std::move(query_seq), std::move(target_seq), query_minimizers,
            prebuilt_target_index, params, gpu_index_cache,
            gpu_index_identity, allow_gpu_target_index_build);
}

} // namespace bioseq::simd
