#define LIKELY(x) (x)
#define UNLIKELY(x) (x)

/**
 * @brief Result structure for KSW2 extension alignment.
 *
 * Stores the alignment score, best-scoring coordinates, CIGAR string,
 * and various metadata produced by the KSW2 alignment algorithms.
 * This struct is populated by align_z2, align_d2, and align_s2.
 */
export typedef struct {
	/** @brief Best alignment score (bits 0-30) and Z-drop flag (bit 31).
	 *  The lower 31 bits store the maximum alignment score observed during
	 *  the DP computation. The highest bit is set to 1 if the alignment
	 *  was terminated early due to the Z-drop heuristic. */
	uint32_t max:31, zdropped:1;

	/** @brief Query coordinate where the maximum score was achieved.
	 *  Set to -1 if not yet determined. In anti-diagonal (rotated) mode,
	 *  this is derived from anti_diagonal - target_position. */
	int max_q;

	/** @brief Target coordinate where the maximum score was achieved.
	 *  Set to -1 if not yet determined. */
	int max_t;

	/** @brief Maximum score when the alignment reaches the end of the query.
	 *  Initialized to KSW_NEG_INF. Used for extension-only mode to decide
	 *  whether the query end was reached with a sufficiently high score. */
	int mqe;

	/** @brief Target coordinate corresponding to mqe (max-query-end score).
	 *  Set to -1 if not yet determined. */
	int mqe_t;

	/** @brief Maximum score when the alignment reaches the end of the target.
	 *  Initialized to KSW_NEG_INF. */
	int mte;

	/** @brief Query coordinate corresponding to mte (max-target-end score).
	 *  Set to -1 if not yet determined. */
	int mte_q;

	/** @brief Final alignment score reaching both query and target ends.
	 *  May remain KSW_NEG_INF if the alignment does not reach both ends
	 *  (e.g., due to Z-drop or extension-only mode). */
	int score;

	/** @brief Allocated capacity (number of uint32_t slots) for the CIGAR array. */
	int m_cigar;

	/** @brief Number of CIGAR operations stored in the cigar array. */
	int n_cigar;

	/** @brief Set to 1 if the alignment reaches the end of the query in
	 *  extension-only mode (KSW_EZ_EXTZ_ONLY) with a score exceeding
	 *  the Z-drop maximum plus end_bonus. Otherwise 0. */
	int reach_end;

	/** @brief Dynamically allocated CIGAR array. Each uint32_t encodes one
	 *  CIGAR operation: the lower 4 bits are the operation type
	 *  (0=match/mismatch, 1=insertion, 2=deletion, 3=N-skip), and the
	 *  upper 28 bits store the operation length. */
	uint32_t *cigar;
} ksw_extz_t;

/**
 * @brief Query profile for the Striped Smith-Waterman (SSW) algorithm.
 *
 * Pre-computes a segmented query profile for SIMD-accelerated alignment.
 * The query is divided into segments of length segment_length, and scores
 * for each alphabet symbol are stored in SIMD-friendly layout.
 * Constructed by ksw_ll_qinit().
 */
typedef struct {
	/** @brief Original query sequence length. */
	int query_length;

	/** @brief Segmented query length: ceil(query_length / values_per_vector).
	 *  Each SIMD vector holds values_per_vector score values. */
	int segment_length;

	/** @brief Score shift applied to make all values non-negative (for 8-bit mode).
	 *  Computed as 256 minus the minimum value in the scoring matrix. */
	uint8_t shift;

	/** @brief Maximum difference between scores after shifting.
	 *  Equal to (max_matrix_value - min_matrix_value) when using 8-bit mode. */
	uint8_t mdiff;

	/** @brief Maximum raw score value in the scoring matrix (before shifting). */
	uint8_t max;

	/** @brief Number of bytes per score element: 1 for int8_t, 2 for int16_t. */
	uint8_t size;

	/** @brief Query profile: segmented scoring values for each alphabet symbol.
	 *  Layout: qp[alphabet_idx * segment_length + segment_position]. */
	__m128i *qp;

	/** @brief DP row buffer H0 (current row scores). */
	__m128i *H0;

	/** @brief DP row buffer H1 (previous row scores). */
	__m128i *H1;

	/** @brief Gap extension state vector E (insertion scores). */
	__m128i *E;

	/** @brief Buffer storing the column-wise maximum scores, used to find
	 *  the best alignment position on the query. */
	__m128i *Hmax;
} kswq_t;

/**
 * @brief Append or merge a CIGAR operation into the CIGAR array.
 *
 * If the last operation in the array has the same type as @p op, the
 * length is merged (added) into that existing operation. Otherwise, a
 * new CIGAR entry is appended, growing the array if needed.
 *
 * CIGAR encoding: each uint32_t stores (length << 4 | op), where op
 * is one of KSW_CIGAR_MATCH (0), KSW_CIGAR_INS (1), KSW_CIGAR_DEL (2),
 * or KSW_CIGAR_N_SKIP (3).
 *
 * @param km             Memory pool (may be NULL for standard malloc).
 * @param cigar_count    [in/out] Current number of CIGAR operations.
 * @param cigar_capacity [in/out] Allocated capacity of the CIGAR array.
 * @param cigar          [in/out] Pointer to the CIGAR array (may be reallocated).
 * @param op             CIGAR operation type (lower 4 bits).
 * @param len            Length of the CIGAR operation.
 * @return               Pointer to the (possibly reallocated) CIGAR array.
 */
export inline uint32_t *ksw_push_cigar(void *km, int *cigar_count, int *cigar_capacity, uint32_t *cigar, uint32_t op, int len)
{
	if (*cigar_count == 0 || op != (cigar[(*cigar_count) - 1] & 0xf)) {
		if (*cigar_count == *cigar_capacity) {
			*cigar_capacity = *cigar_capacity ? (*cigar_capacity) << 1 : 4;
			cigar = (uint32_t*)krealloc(km, cigar, (*cigar_capacity) << 2);
		}
		cigar[(*cigar_count)++] = len << 4 | op;
	} else {
		cigar[(*cigar_count) - 1] += len << 4;
	}
	return cigar;
}

/**
 * @brief Perform CIGAR traceback from the backtrack matrix.
 *
 * Walks backwards through the DP backtrack matrix to reconstruct the
 * alignment path as a CIGAR string. The matrix may use either rotated
 * (anti-diagonal) or standard (row-major) coordinates.
 *
 * Backtrack matrix encoding (per byte):
 *   - Bits 0-2: which DP state achieved the max:
 *     0 = H (match/mismatch), 1 = E (deletion), 2 = F (insertion),
 *     3 = E-tilde (long deletion/N-skip), 4 = F-tilde (long insertion)
 *   - Bit 3 (0x08): 1 if continuing on the E state
 *   - Bit 4 (0x10): 1 if continuing on the F state
 *   - Bit 5 (0x20): 1 if continuing on the E-tilde state
 *   - Bit 6 (0x40): 1 if continuing on the F-tilde state
 *
 * @param km              Memory pool (may be NULL for standard malloc).
 * @param is_rot          Non-zero if using rotated (anti-diagonal) coordinates.
 * @param is_rev          Non-zero to output CIGAR in reverse order (no reversal step).
 * @param min_intron_len  Minimum intron length; if > 0, long deletions in
 *                        E-tilde state are encoded as N-skip (CIGAR 'N').
 * @param backtrack_matrix  Pointer to the backtrack matrix.
 * @param off             Array of row/anti-diagonal start offsets.
 * @param off_end         Array of row/anti-diagonal end offsets (may be NULL).
 * @param n_col           Number of columns in the backtrack matrix.
 * @param start_row       Starting row (or target position) for traceback.
 * @param start_col       Starting column (or query position) for traceback.
 * @param cigar_capacity  [in/out] Allocated capacity of the CIGAR array.
 * @param cigar_count     [in/out] Number of CIGAR operations written.
 * @param cigar           [in/out] Pointer to the CIGAR array (may be reallocated).
 */
export inline void ksw_backtrack(void *km, int is_rot, int is_rev, int min_intron_len, const uint8_t *backtrack_matrix, const int *off, const int *off_end, int n_col, int start_row, int start_col,
								 int *cigar_capacity, int *cigar_count, uint32_t **cigar)
{
	int cigar_count_local = 0, cigar_capacity_local = *cigar_capacity;
	int row_position = start_row, col_position = start_col;
	int traceback_state = 0;
	uint32_t *cigar_local = *cigar;
	uint32_t current_backtrack_value;

	while (row_position >= 0 && col_position >= 0) {
		int force_state = -1;
		if (is_rot) {
			const int anti_diagonal = row_position + col_position;
			if (row_position < off[anti_diagonal]) {
				force_state = 2;
			}
			if (off_end && row_position > off_end[anti_diagonal]) {
				force_state = 1;
			}
			current_backtrack_value = force_state < 0 ? backtrack_matrix[(size_t)anti_diagonal * n_col + row_position - off[anti_diagonal]] : 0;
		} else {
			if (col_position < off[row_position]) {
				force_state = 2;
			}
			if (off_end && col_position > off_end[row_position]) {
				force_state = 1;
			}
			current_backtrack_value = force_state < 0 ? backtrack_matrix[(size_t)row_position * n_col + col_position - off[row_position]] : 0;
		}
		if (traceback_state == 0) {
			traceback_state = current_backtrack_value & 7;
		} else if (!(current_backtrack_value >> (traceback_state + 2) & 1)) {
			traceback_state = 0;
		}
		if (traceback_state == 0) {
			traceback_state = current_backtrack_value & 7; // TODO: probably this line can be merged into the "else if" line right above; not 100% sure
		}
		if (force_state >= 0) {
			traceback_state = force_state;
		}
		if (traceback_state == 0) {
			cigar_local = ksw_push_cigar(km, &cigar_count_local, &cigar_capacity_local, cigar_local, KSW_CIGAR_MATCH, 1);
			--row_position;
			--col_position;
		} else if (traceback_state == 1 || (traceback_state == 3 && min_intron_len <= 0)) {
			cigar_local = ksw_push_cigar(km, &cigar_count_local, &cigar_capacity_local, cigar_local, KSW_CIGAR_DEL, 1);
			--row_position;
		} else if (traceback_state == 3 && min_intron_len > 0) {
			cigar_local = ksw_push_cigar(km, &cigar_count_local, &cigar_capacity_local, cigar_local, KSW_CIGAR_N_SKIP, 1);
			--row_position;
		} else {
			cigar_local = ksw_push_cigar(km, &cigar_count_local, &cigar_capacity_local, cigar_local, KSW_CIGAR_INS, 1);
			--col_position;
		}
	}
	if (row_position >= 0) {
		const int skip_op = (min_intron_len > 0 && row_position >= min_intron_len) ? KSW_CIGAR_N_SKIP : KSW_CIGAR_DEL;
		cigar_local = ksw_push_cigar(km, &cigar_count_local, &cigar_capacity_local, cigar_local, skip_op, row_position + 1);
	}
	if (col_position >= 0) {
		cigar_local = ksw_push_cigar(km, &cigar_count_local, &cigar_capacity_local, cigar_local, KSW_CIGAR_INS, col_position + 1);
	}
	if (!is_rev) {
		for (int swap_idx = 0; swap_idx < cigar_count_local >> 1; ++swap_idx) {
			const uint32_t swap_tmp = cigar_local[swap_idx];
			cigar_local[swap_idx] = cigar_local[cigar_count_local - 1 - swap_idx];
			cigar_local[cigar_count_local - 1 - swap_idx] = swap_tmp;
		}
	}
	*cigar_capacity = cigar_capacity_local;
	*cigar_count = cigar_count_local;
	*cigar = cigar_local;
}

/**
 * @brief Reset an ksw_extz_t result structure to its initial state.
 *
 * Sets all coordinates to -1, all scores to KSW_NEG_INF (or 0 for max),
 * and clears the CIGAR count, Z-drop flag, and reach_end flag. Should be
 * called at the start of each alignment invocation.
 *
 * @param ez  Pointer to the extension result structure to reset.
 */
export inline void ksw_reset_extz(ksw_extz_t *ez)
{
	ez->max_q = ez->max_t = ez->mqe_t = ez->mte_q = -1;
	ez->max = 0, ez->score = ez->mqe = ez->mte = KSW_NEG_INF;
	ez->n_cigar = 0, ez->zdropped = 0, ez->reach_end = 0;
}

/**
 * @brief Apply the Z-drop heuristic to decide whether to terminate alignment.
 *
 * The Z-drop heuristic stops alignment when the current score has dropped
 * significantly below the best score seen so far, adjusted for the distance
 * traveled. Specifically, alignment is terminated when:
 *   (ez->max - current_score) > zdrop + coordinate_difference * gap_extension_penalty
 *
 * This prevents wasting computation on regions that are unlikely to recover
 * a competitive score.
 *
 * @param ez                    [in/out] Extension result; max/max_t/max_q may be updated.
 * @param is_rot                Non-zero if using rotated (anti-diagonal) coordinates.
 * @param current_score         The current DP score (H value) at this position.
 * @param first_coord           First coordinate (anti-diagonal if rotated, else row+col).
 * @param second_coord          Second coordinate (target position if rotated, else row).
 * @param zdrop                 Z-drop threshold; negative disables the heuristic.
 * @param gap_extension_penalty Gap extension penalty used in the distance adjustment.
 * @return                      1 if alignment should be terminated (Z-dropped), 0 otherwise.
 */
export inline int ksw_apply_zdrop(ksw_extz_t *ez, int is_rot, int32_t current_score, int first_coord, int second_coord, int zdrop, int8_t gap_extension_penalty)
{
	int anti_diagonal, target_position;
	if (is_rot) {
		anti_diagonal = first_coord;
		target_position = second_coord;
	} else {
		anti_diagonal = first_coord + second_coord;
		target_position = first_coord;
	}
	if (current_score > (int32_t)ez->max) {
		ez->max = current_score;
		ez->max_t = target_position;
		ez->max_q = anti_diagonal - target_position;
	} else if (target_position >= ez->max_t && anti_diagonal - target_position >= ez->max_q) {
		const int target_distance = target_position - ez->max_t;
		const int query_distance = (anti_diagonal - target_position) - ez->max_q;
		const int coordinate_difference = target_distance > query_distance ? target_distance - query_distance : query_distance - target_distance;
		if (zdrop >= 0 && (int32_t)ez->max - current_score > zdrop + coordinate_difference * gap_extension_penalty) {
			ez->zdropped = 1;
			return 1;
		}
	}
	return 0;
}

/**
 * @brief Initialize a query profile for the striped Smith-Waterman algorithm.
 *
 * Constructs a segmented query profile that enables SIMD-parallel alignment.
 * The query sequence is rearranged into interleaved segments so that each
 * SIMD vector processes one element from each segment simultaneously.
 *
 * For 8-bit mode (size=1), scores are shifted to be non-negative. For
 * 16-bit mode (size=2), raw scores are used directly.
 *
 * Memory layout (single allocation):
 *   [kswq_t header][padding][qp vectors][H0 vectors][H1 vectors][E vectors][Hmax vectors]
 *
 * @param km     Memory pool (may be NULL for standard malloc).
 * @param size   Score element size in bytes: 1 for int8_t, 2 for int16_t.
 * @param qlen   Length of the query sequence.
 * @param query  Query sequence as an array of alphabet indices.
 * @param m      Size of the scoring alphabet.
 * @param mat    Scoring matrix stored as a flat m-by-m array (row-major).
 * @return       Opaque pointer to the allocated kswq_t query profile.
 */
export inline void *ksw_ll_qinit(void *km, int size, int qlen, const uint8_t *query, int m, const int8_t *mat)
{
	kswq_t *query_profile;
	int segment_length, alphabet_idx, matrix_size, values_per_vector;

	size = size > 1 ? 2 : 1;
	values_per_vector = 8 * (3 - size); // # values per __m128i
	segment_length = (qlen + values_per_vector - 1) / values_per_vector; // segmented length
	query_profile = (kswq_t*)kmalloc(km, sizeof(kswq_t) + 256 + 16 * segment_length * (m + 4)); // a single block of memory
	query_profile->qp = (__m128i*)(((size_t)query_profile + sizeof(kswq_t) + 15) >> 4 << 4); // align memory
	query_profile->H0 = query_profile->qp + segment_length * m;
	query_profile->H1 = query_profile->H0 + segment_length;
	query_profile->E  = query_profile->H1 + segment_length;
	query_profile->Hmax = query_profile->E + segment_length;
	query_profile->segment_length = segment_length;
	query_profile->query_length = qlen;
	query_profile->size = size;
	// compute shift
	matrix_size = m * m;
	query_profile->shift = 127;
	query_profile->mdiff = 0;
	for (alphabet_idx = 0; alphabet_idx < matrix_size; ++alphabet_idx) { // find the minimum and maximum score
		if (mat[alphabet_idx] < (int8_t)query_profile->shift) {
			query_profile->shift = mat[alphabet_idx];
		}
		if (mat[alphabet_idx] > (int8_t)query_profile->mdiff) {
			query_profile->mdiff = mat[alphabet_idx];
		}
	}
	query_profile->max = query_profile->mdiff;
	query_profile->shift = 256 - query_profile->shift; // NB: query_profile->shift is uint8_t
	query_profile->mdiff += query_profile->shift; // this is the difference between the min and max scores
	// An example: values_per_vector=8, qlen=19, segment_length=3 and segmentation:
	//  {{0,3,6,9,12,15,18,-1},{1,4,7,10,13,16,-1,-1},{2,5,8,11,14,17,-1,-1}}
	if (size == 1) {
		int8_t *score_table = (int8_t*)query_profile->qp;
		for (alphabet_idx = 0; alphabet_idx < m; ++alphabet_idx) {
			const int8_t *matrix_row = mat + alphabet_idx * m;
			const int total_entries = segment_length * values_per_vector;
			for (int segment_idx = 0; segment_idx < segment_length; ++segment_idx) {
				for (int query_idx = segment_idx; query_idx < total_entries; query_idx += segment_length) { // values_per_vector iterations
					*score_table++ = (query_idx >= qlen ? 0 : matrix_row[query[query_idx]]) + query_profile->shift;
				}
			}
		}
	} else {
		int16_t *score_table = (int16_t*)query_profile->qp;
		for (alphabet_idx = 0; alphabet_idx < m; ++alphabet_idx) {
			const int8_t *matrix_row = mat + alphabet_idx * m;
			const int total_entries = segment_length * values_per_vector;
			for (int segment_idx = 0; segment_idx < segment_length; ++segment_idx) {
				for (int query_idx = segment_idx; query_idx < total_entries; query_idx += segment_length) { // values_per_vector iterations
					*score_table++ = (query_idx >= qlen ? 0 : matrix_row[query[query_idx]]);
				}
			}
		}
	}
	return query_profile;
}

/**
 * @brief Perform 16-bit striped Smith-Waterman alignment using SSE2.
 *
 * Computes the Smith-Waterman local alignment between a pre-computed query
 * profile and a target sequence, using 16-bit score arithmetic for sequences
 * that may overflow 8-bit accumulators. Returns the best alignment score
 * and the positions on both query and target where that score was achieved.
 *
 * @param query_profile_ptr  Opaque query profile pointer from ksw_ll_qinit().
 * @param tlen               Length of the target sequence.
 * @param target             Target sequence as an array of alphabet indices.
 * @param _gapo              Gap open penalty (positive value).
 * @param _gape              Gap extension penalty (positive value).
 * @param qe                 [out] Query position of the best alignment score.
 * @param te                 [out] Target position of the best alignment score.
 * @return                   The best alignment score.
 */
export inline int ksw_ll_i16(void *query_profile_ptr, int tlen, const uint8_t *target, int _gapo, int _gape, int *qe, int *te)
{
	kswq_t *query_profile = (kswq_t*)query_profile_ptr;
	int segment_length, global_max_score = 0, qlen8;
	__m128i zero, gap_open_plus_extend, gap_extend, *H0, *H1, *E, *Hmax;
	uint16_t *H8;

#define __max_8(ret, xx) do { \
		(xx) = _mm_max_epi16((xx), _mm_srli_si128((xx), 8)); \
		(xx) = _mm_max_epi16((xx), _mm_srli_si128((xx), 4)); \
		(xx) = _mm_max_epi16((xx), _mm_srli_si128((xx), 2)); \
		(ret) = _mm_extract_epi16((xx), 0); \
	} while (0)

	// initialization
	*qe = *te = -1;
	zero = _mm_set1_epi32(0);
	gap_open_plus_extend = _mm_set1_epi16(_gapo + _gape);
	gap_extend = _mm_set1_epi16(_gape);
	H0 = query_profile->H0;
	H1 = query_profile->H1;
	E = query_profile->E;
	Hmax = query_profile->Hmax;
	segment_length = query_profile->segment_length;
	qlen8 = segment_length * 8;
	memset(E,    0, segment_length * sizeof(__m128i));
	memset(H0,   0, segment_length * sizeof(__m128i));
	memset(Hmax, 0, segment_length * sizeof(__m128i));
	// the core loop
	for (int target_idx = 0; target_idx < tlen; ++target_idx) {
		int column_max;
		__m128i e, h, f = zero, max = zero, *S = query_profile->qp + target[target_idx] * segment_length; // S is the 1st score vector
		h = _mm_load_si128(H0 + segment_length - 1); // h={2,5,8,11,14,17,-1,-1} in the above example
		h = _mm_slli_si128(h, 2);
		for (int segment_idx = 0; LIKELY(segment_idx < segment_length); ++segment_idx) {
			h = _mm_adds_epi16(h, *S++);
			e = _mm_load_si128(E + segment_idx);
			h = _mm_max_epi16(h, e);
			h = _mm_max_epi16(h, f);
			max = _mm_max_epi16(max, h);
			_mm_store_si128(H1 + segment_idx, h);
			h = _mm_subs_epu16(h, gap_open_plus_extend);
			e = _mm_subs_epu16(e, gap_extend);
			e = _mm_max_epi16(e, h);
			_mm_store_si128(E + segment_idx, e);
			f = _mm_subs_epu16(f, gap_extend);
			f = _mm_max_epi16(f, h);
			h = _mm_load_si128(H0 + segment_idx);
		}
		for (int pass_idx = 0; LIKELY(pass_idx < 8); ++pass_idx) {
			f = _mm_slli_si128(f, 2);
			for (int segment_idx = 0; LIKELY(segment_idx < segment_length); ++segment_idx) {
				h = _mm_load_si128(H1 + segment_idx);
				h = _mm_max_epi16(h, f);
				_mm_store_si128(H1 + segment_idx, h);
				h = _mm_subs_epu16(h, gap_open_plus_extend);
				f = _mm_subs_epu16(f, gap_extend);
				if (UNLIKELY(!_mm_movemask_epi8(_mm_cmpgt_epi16(f, h)))) { goto end_loop_i16; }
			}
		}
end_loop_i16:
		__max_8(column_max, max);
		if (column_max >= global_max_score) {
			global_max_score = column_max;
			*te = target_idx;
			memcpy(Hmax, H1, segment_length * sizeof(__m128i));
		}
		S = H1; H1 = H0; H0 = S;
	}
	H8 = (uint16_t*)Hmax;
	for (int query_idx = 0; query_idx < qlen8; ++query_idx) {
		if ((int)H8[query_idx] == global_max_score) {
			*qe = query_idx / 8 + query_idx % 8 * segment_length;
		}
	}
	return global_max_score;
}
