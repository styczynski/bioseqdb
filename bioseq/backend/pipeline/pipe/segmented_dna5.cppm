/**
 * @file pipeline/pipe/segmented_dna5.cppm
 * @brief Owns the width-erased dna5 segmented minimap instantiation.
 */
module;

#include <bioseq/pg_build_profile.h>

#include <cstddef>
#include <utility>

module bioseq.pipeline.pipe;

import bioseq.common.seqan_adapter;
import bioseq.storage.lazy_sequence;

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
namespace bioseq::simd {

[[nodiscard, gnu::noinline]] match_result_collection minimap_port_segmented(
    bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
    bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
    const minimap2_parameters& params)
{
    return minimap_port_segmented<::seq::dna5, std::nullptr_t>(
        std::move(query_seq), std::move(target_seq), params, nullptr);
}

} // namespace bioseq::simd
#endif
