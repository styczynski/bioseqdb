/**
 * @file
 * @brief Defines the canonical minimap-compatible alignment result value type.
 */
export module bioseq.pipeline.minimap_port.match_result_contract;

import std;
import bioseq.sequences.cigar;
import bioseq.storage.size;

export namespace bioseq::simd {

using bioseq::storage::sequence_size_t;

/**
 * @brief PAF-like alignment result.
 *
 * Maps to the 12 mandatory PAF columns plus alignment details.
 */
struct match_result {
    // PAF mandatory columns.: every length / absolute coordinate
    // (cols 2,3,4,7,8,9,10,11) is `sequence_size_t` (uint64) so it is never
    // truncated for sequences longer than 2^31. PAF emits them as decimal
    // strings, so the wire width is unchanged.
    sequence_size_t query_len = 0;    ///< col 2: query sequence length
    sequence_size_t query_start = 0;  ///< col 3: query start (0-based, closed)
    sequence_size_t query_end = 0;    ///< col 4: query end (0-based, open)
    bool is_reverse = false;           ///< col 5: '+' or '-'
    sequence_size_t target_len = 0;   ///< col 7: target sequence length
    sequence_size_t target_start = 0; ///< col 8: target start on original strand
    sequence_size_t target_end = 0;   ///< col 9: target end on original strand
    sequence_size_t num_matches = 0;  ///< col 10: number of residue matches (mlen)
    sequence_size_t block_length = 0; ///< col 11: alignment block length (blen)
    std::int32_t mapq = 0;            ///< col 12: mapping quality (0-255)

    // Additional alignment info
    std::int32_t chain_score = 0;           ///< chaining score
    std::int32_t dp_score = 0;              ///< DP alignment score
    std::int32_t dp_max_score = 0;          ///< max scoring segment score (dp_max0 in original)
    std::int32_t seed_count = 0;            ///< number of seeds in chain
    std::int32_t secondary_chain_score = 0; ///< best secondary score against this primary
    float divergence = -1.f;                ///< sequence divergence estimate
    bool is_primary = true;                  ///< primary mapping flag
    bool has_cigar = false;                  ///< whether CIGAR alignment was performed
    std::vector<cigar_operation> cigar;      ///< CIGAR string
    sequence_size_t edit_distance = 0;      ///< exact unit-cost edits in the reported alignment
};

} // namespace bioseq::simd
