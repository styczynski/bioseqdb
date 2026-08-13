/**
 * @file pipeline/pipe/raw_dna5.cppm
 * @brief Owns the width-erased dna5 raw minimap pipeline instantiation.
 */
module;

#include <bioseq/pg_build_profile.h>

#include <utility>

module bioseq.pipeline.pipe;

import bioseq.common.seqan_adapter;
import bioseq.storage.lazy_sequence;

#if defined(BIOSEQ_TEMPLATE_FLATTEN_HAS_ALL_ALPHABETS_UINT64)
namespace bioseq::simd {

[[nodiscard, gnu::noinline]] match_result_collection minimap_port(
    bioseq::storage::lazy_sequence_view<::seq::dna5> query_seq,
    bioseq::storage::lazy_sequence_view<::seq::dna5> target_seq,
    const minimap2_parameters& params)
{
    return minimap_port<::seq::dna5>(
        std::move(query_seq), std::move(target_seq), params);
}

} // namespace bioseq::simd
#endif
