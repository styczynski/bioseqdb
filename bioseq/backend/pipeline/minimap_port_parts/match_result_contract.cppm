/**
 * @file
 * @brief Alignment results returned by the mapping pipeline.
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
 * @brief One alignment hit, with PAF-style coordinates and alignment details.
 *
 * Query and target names are supplied separately by the caller. Fields that
 * require base alignment or optional statistics are filled only when requested.
 */
struct match_result {
    // PAF lengths and coordinates use 64-bit sequence sizes. This keeps large
    // positions intact when results are later printed as decimal values.
    sequence_size_t query_len = 0;    ///< PAF column 2: query length in symbols.
    sequence_size_t query_start = 0;  ///< PAF column 3: first query position, zero-based and included.
    sequence_size_t query_end = 0;    ///< PAF column 4: first query position after the match.
    bool is_reverse = false;           ///< PAF column 5: true for a reverse-strand match.
    sequence_size_t target_len = 0;   ///< PAF column 7: target length in symbols.
    sequence_size_t target_start = 0; ///< PAF column 8: included target start on the original strand.
    sequence_size_t target_end = 0;   ///< PAF column 9: excluded target end on the original strand.
    sequence_size_t num_matches = 0;  ///< PAF column 10: estimated match length. CIGAR M operations can include mismatches.
    sequence_size_t block_length = 0; ///< PAF column 11: aligned block length, estimated from seeds before alignment.
    std::int32_t mapq = 0;            ///< PAF column 12: mapping quality. The CPU calculation uses 0..60.

    // Scores and optional base-alignment results.
    std::int32_t chain_score = 0;           ///< Score assigned to the seed chain.
    std::int32_t dp_score = 0;              ///< Score from base alignment, when performed.
    std::int32_t dp_max_score = 0;          ///< Score used for MAPQ comparison. CPU alignment currently copies dp_score.
    std::int32_t seed_count = 0;            ///< Number of anchors in the chain.
    std::int32_t secondary_chain_score = 0; ///< Best competing secondary chain score for this primary.
    float divergence = -1.f;                ///< Divergence estimated from minimizers, or -1 when unavailable.
    bool is_primary = true;                  ///< Whether this region is its own primary parent.
    bool has_cigar = false;                  ///< Whether base alignment was performed.
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    alignment_result_vector<cigar_operation> cigar;  ///< CIGAR operations with their benchmark memory reservation.
#else
    std::vector<cigar_operation> cigar;      ///< CIGAR operations owned by this result.
#endif
    sequence_size_t edit_distance = 0;      ///< Unit-cost edits in this alignment, when exact edit counting was requested.
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
static_assert(!std::is_copy_constructible_v<match_result>);
static_assert(std::is_nothrow_move_constructible_v<match_result>);
#endif

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
/** Result vector that owns its memory reservation. */
using match_result_vector = admitted_alignment_result_vector<match_result, false>;
#else
/** Result vector used by ordinary builds. */
using match_result_vector = std::vector<match_result>;
#endif

/** Group results by key. Benchmark builds account for map nodes and each vector. */
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
/** Result owners used by direct unit tests without changing the pipeline types. */
using test_match_result_vector = test_outer_alignment_result_vector<match_result>;
template <class p_key_type>
using test_match_result_map = std::map<
    p_key_type, test_match_result_vector, std::less<p_key_type>,
    admitted_alignment_node_allocator<std::pair<const p_key_type, test_match_result_vector>>>;
static_assert(std::is_nothrow_move_constructible_v<match_result>);
#endif

} // namespace bioseq::simd
