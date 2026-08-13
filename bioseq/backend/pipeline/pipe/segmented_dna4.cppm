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
    return minimap_port_segmented<::seq::dna4, std::nullptr_t>(
        std::move(query_seq), std::move(target_seq), params, nullptr);
}

} // namespace bioseq::simd
