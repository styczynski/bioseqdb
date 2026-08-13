/**
 * @file pipeline/pipe/segmented_dna4.cppm
 * @brief Owns the width-erased dna4 segmented minimap instantiation.
 */
module;

#include <bioseq/pg_build_profile.h>

#include <cstddef>
#include <utility>
#include <vector>

module bioseq.pipeline.pipe;

import bioseq.common.seqan_adapter;
import bioseq.storage.lazy_sequence;

namespace bioseq::simd {

[[nodiscard, gnu::noinline]] match_result_collection minimap_port_segmented_owned(
    bioseq::storage::lazy_sequence_view<::seq::dna4> query_seq,
    bioseq::storage::lazy_sequence_view<::seq::dna4> target_seq,
    const minimap2_parameters& params)
{
    return pipe_entry_detail::minimap_port_segmented_owned_dispatch<
        ::seq::dna4>(
            std::move(query_seq), std::move(target_seq), params);
}

} // namespace bioseq::simd
