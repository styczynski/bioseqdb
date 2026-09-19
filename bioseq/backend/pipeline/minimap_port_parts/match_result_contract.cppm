/**
 * @file
 * @brief Defines the canonical minimap-compatible alignment result value type.
 */
export module bioseq.pipeline.minimap_port.match_result_contract;

import std;
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
import bioseq.alignment.contracts;
#endif
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
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    alignment_result_vector<cigar_operation> cigar;  ///< CIGAR string and campaign lease
#else
    std::vector<cigar_operation> cigar;      ///< CIGAR string
#endif
    sequence_size_t edit_distance = 0;      ///< exact unit-cost edits in the reported alignment
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
static_assert(!std::is_copy_constructible_v<match_result>);
static_assert(std::is_nothrow_move_constructible_v<match_result>);
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Independently admitted owner for ordered result payloads. */
using match_result_vector = admitted_alignment_result_vector<match_result, false>;
#else
/** Ordinary builds retain the original zero-overhead result representation. */
using match_result_vector = std::vector<match_result>;
#endif

/** Campaign nodes and mapped vectors each retain their own physical permit. */
template <class p_key_type>
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
using basic_match_result_map = std::map<
    p_key_type, match_result_vector, std::less<p_key_type>,
    admitted_alignment_node_allocator<std::pair<const p_key_type, match_result_vector>>>;
#else
using basic_match_result_map = std::map<p_key_type, match_result_vector>;
#endif

using match_result_map = basic_match_result_map<std::int32_t>;

#if defined(BIOSEQ_TEST_INDEXES)
/** Direct unit-test owners without changing the test-opt pipeline ABI. */
using test_match_result_vector = test_outer_alignment_result_vector<match_result>;
template <class p_key_type>
using test_match_result_map = std::map<
    p_key_type, test_match_result_vector, std::less<p_key_type>,
    admitted_alignment_node_allocator<std::pair<const p_key_type, test_match_result_vector>>>;
static_assert(std::is_nothrow_move_constructible_v<match_result>);
#endif

} // namespace bioseq::simd
