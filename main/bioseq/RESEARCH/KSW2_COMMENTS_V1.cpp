
// NEW IMPL


/**
 * @brief Dual-affine-gap banded extension alignment using Highway SIMD.
 *
 * Port of `ksw2_extd2_sse`. Uses two affine gap penalty functions combined
 * into `gap_cost(k) = min(q + k*e, q2 + k*e2)`, which captures both short
 * gaps (`q + k*e`) and long gaps (`q2 + k*e2`) efficiently. Two additional
 * DP state arrays (`gap_e2_offset`, `gap_f2_offset`) track the long-gap
 * continuation. All 8-bit arithmetic is signed (no unsigned bias, unlike
 * `z2`).
 *
 * Algorithm structure mirrors `z2` with the following changes:
 *   - Two pairs of gap penalties (swapped if `q + e > q2 + e2`).
 *   - Two additional state arrays for the long-gap state.
 *   - Different score-update formula in the DP code blocks.
 *   - Backtrack encodes five states (`0=H, 1=E, 2=F, 3=E-tilde, 4=F-tilde`).
 *
 * @tparam AlphabetDim  Scoring matrix dimension — `alphabet_size + 1`.
 * @param  query           Query sequence (encoded as rank bytes).
 * @param  target          Target sequence (encoded as rank bytes).
 * @param  scoring_matrix  Compile-time sized scoring matrix.
 * @param  params          Penalty/flag parameters (see `KswD2Params`).
 * @return Populated `KswAlignResult` — owns its CIGAR vector.
 */
 template<
 // AI Note: one parameter R instead of two template parameters for target/query is enough
 supports_compact_sequence_range R,
 int AlphabetDim = compact_sequence_range<R>::traits::alphabet_size + 1,
 typename partial_score_t = int8_t,
 typename score_t = int32_t
>
[[nodiscard]] KswAlignResult hwy_align_d2_new(
 const compact_sequence_range<R>& query,
 const compact_sequence_range<R>& target,
 const KswScoringMatrix<AlphabetDim>& scoring_matrix,
 const KswD2Params& params
) {
 // AI Note: I dissalow usage of global tags - KswByteTag/KswByteTagU/KswScoreTag and KswByteVec/KswScoreVe and kKswVecBytes/kKswScoreLanes
 // Everything should be parametrized by some template type eiter as external input (score_t and partial_score_t) or relying on some sequence properties like typename seq_traits::letter_unit
 
 /*
     using KswByteTag   = hn::ScalableTag<int8_t>;
     using KswByteTagU  = hn::ScalableTag<uint8_t>;
     using KswScoreTag  = hn::Full128<int32_t>;  // exact-max inner loop (4 lanes)

     inline constexpr int kKswVecBytes = static_cast<int>(hn::MaxLanes(KswByteTag{}));
     inline constexpr int kKswScoreLanes = static_cast<int>(hn::MaxLanes(KswScoreTag{}));

     static_assert(kKswVecBytes == 16,
         "KSW2 bit-exact output is validated only for 16-byte SIMD vectors. "
         "Wider SIMD targets need validation before this assertion is relaxed.");
     static_assert(kKswScoreLanes == 4,
         "KSW2 exact-max inner loop processes 4 int32 lanes per step; widening "
         "KswScoreTag would require re-validating the score-block max extraction.");

     using KswByteVec   = hn::Vec<KswByteTag>;
     using KswScoreVec  = hn::Vec<KswScoreTag>;
     static constexpr int kVecBytes = kKswVecBytes;
 */
  using seq_traits = compact_sequence_range<R>::traits;
  // AI Note: We might thing that we can use scalable tag parameterized by letter unit, this is the smallest type that can hold single letter of the sequence
  // but for alphabet of size 1 or 2 this will break
  // Semantically byte_tag is just narrow score tag (for partial resuts)
  // So you should parametrize this tab by partial_score_t and rename to partial_score_tag
  // Also rename score_tag to full_score_tag and rename score_t to full_score_t?
  hn::ScalableTag<partial_score_t> byte_tag;
  hn::ScalableTag<score_t> score_tag;

  // AI Note: Instead of using kKswVecBytes and kKswScoreLanes (MaxLanes is improper as it will fail for any other target)
  // hn::MaxLanes() is constexpr upper bound on number of lanes and can be used to allocate arrays, reserve max capacity for butters etc.
  // But for actual iteration we MUST use hn::Lanes(d) which is const not constexpr.
  const size_t byte_lanes_count = hn::Lanes(byte_tag);
  const size_t score_lanes_count = hn::Lanes(score_tag);

  constexpr size_t byte_lanes_bound = hn::MaxLanes(byte_tag);

  KswAlignResult result;

  // AI Note: I extracted this to constexpr if as it can fail faster
  if constexpr (AlphabetDim <= 1) {
     return result;
  }

  // AI Note: I hate when you make static cast everywhere for int32 <-> size_t. Can we use size_t to indicate positions/indices?
  const size_t query_len  = query.bp_size();
  const size_t target_len = target.bp_size();

  if (query_len == 0 || target_len == 0) {
      return result;
  }

  // Ensure q+e <= q2+e2 (swap if needed so "short" is really short)
  if (params.gap_open_long + params.gap_extend_long
      < params.gap_open_short + params.gap_extend_short)
  {
      std::swap(params.gap_open_short,   params.gap_open_long);
      std::swap(params.gap_extend_short, params.gap_extend_long);
  }
  const int8_t gap_open_short   = params.gap_open_short;
  const int8_t gap_extend_short = params.gap_extend_short;
  const int8_t gap_open_long    = params.gap_open_long;
  const int8_t gap_extend_long  = params.gap_extend_long;
  const int    gap_open_plus_extend = gap_open_short + gap_extend_short;
  const KswFlags flags         = params.flags;
  const bool   compute_cigar   = !flags.score_only;
  const bool   use_approx_max  = flags.approx_max;

  // --- SIMD constant vectors -------------------------------------------
  const auto zero_vec                = hn::Zero(byte_tag);
  const auto gap_open_short_vec      = hn::Set(byte_tag, gap_open_short);
  const auto gap_open_long_vec       = hn::Set(byte_tag, gap_open_long);
  const auto short_gap_cost_vec      = hn::Set(byte_tag,
      static_cast<int8_t>(gap_open_short + gap_extend_short));
  const auto long_gap_cost_vec       = hn::Set(byte_tag,
      static_cast<int8_t>(gap_open_long + gap_extend_long));
  const auto match_score_vec         = hn::Set(byte_tag, scoring_matrix.match_score());
  const auto mismatch_score_vec      = hn::Set(byte_tag, scoring_matrix.mismatch_score());
  const auto wildcard_score_vec      = scoring_matrix.wildcard_score() == 0
      ? hn::Set(byte_tag, static_cast<int8_t>(-gap_extend_long))
      : hn::Set(byte_tag, scoring_matrix.wildcard_score());
  const auto wildcard_rank_vec       = hn::Set(byte_tag,
      static_cast<int8_t>(AlphabetDim - 1));

  // Backtrack-direction constants (five-state scheme)
  const auto backtrack_dir_1_vec = hn::Set(byte_tag, 1);
  const auto backtrack_dir_2_vec = hn::Set(byte_tag, 2);
  const auto backtrack_dir_3_vec = hn::Set(byte_tag, 3);
  const auto backtrack_dir_4_vec = hn::Set(byte_tag, 4);
  const auto backtrack_e_cont    = hn::Set(byte_tag, 0x08);
  const auto backtrack_f_cont    = hn::Set(byte_tag, 0x10);
  const auto backtrack_e2_cont   = hn::Set(byte_tag, 0x20);
  const auto backtrack_f2_cont   = hn::Set(byte_tag, 0x40);

  // --- Band width ------------------------------------------------------
  const int band_width = params.band_width < 0
      ? std::max(target_len, query_len)
      : params.band_width;
  const int left_band  = band_width;
  const int right_band = band_width;

  // --- Score range sanity check ----------------------------------------
  int matrix_min = scoring_matrix.mismatch_score();
  for (size_t entry_index = 1; entry_index < scoring_matrix.entry_count; ++entry_index) {
      const int8_t entry = scoring_matrix.entries[entry_index];
      if (entry < matrix_min) { matrix_min = entry; }
  }
  if (-matrix_min > 2 * (gap_open_short + gap_extend_short)) {
      return result;
  }

  // --- Long-gap threshold (see Suzuki's paper §3.2) --------------------
  const int long_gap_threshold = gap_extend_short != gap_extend_long
      ? [&]() {
          int threshold = (gap_open_long - gap_open_short)
              / (gap_extend_short - gap_extend_long) - 1;
          if (gap_open_long + gap_extend_long + threshold * gap_extend_long
              > gap_open_short + gap_extend_short + threshold * gap_extend_short)
          {
              ++threshold;
          }
          return threshold;
        }()
      : 0;
  const int long_gap_diff = long_gap_threshold * (gap_extend_short - gap_extend_long)
                            - (gap_open_long - gap_open_short) - gap_extend_long;

  // --- Block dimensions ------------------------------------------------
  // AI Note: used byte_lanes_bound instead of bytes_lanes_count here beacuse we use this variable only in reserve so it's okay to have constexpr bound instead of actual value
  int band_vec_capacity =
      query_len < target_len ? query_len : target_len;
  band_vec_capacity =
      ((band_vec_capacity < band_width + 1 ? band_vec_capacity : band_width + 1)
          + byte_lanes_bound - 1) / byte_lanes_bound + 1;

  // --- Scratch arena (u, v, x, y, x2, y2, s + sf + qr) ----------------
  // d2 uses signed arithmetic end-to-end, so the state arrays are accessed
  // as `int8_t*` views throughout. After the arena's zero-init, overwrite
  // the first four state arrays (u, v, x, y) with the short-gap baseline
  // in a single memset, and arrays 4-5 (x2, y2) with the long-gap baseline.

  // AI Note: I hate how this type is defined this is ridicoulus.
  // Can we have other struct that won't be templated by size and will just hold named fields
  // Look below this line you extract all pointers using "arena.state_bytes_signed(0).data();"
  // The developer won't understand what's happening there.
  // I would prefer something like:
  //    ksw_state state(target_len, query_len);
  //    state.diff_up_base += ...
  // etc.
  // Remove this ridicioulus type
  // I know that in this file you have two specializations KswDpArena<7> and KswDpArena<5>
  // but they're different only in gap_f2_offset_base so we can have one base type let's say ksw_state and then another struct inheriting without any virtual methods
  // ksw_state_double : public ksw_state { gap_e2_offset_base - other extra fields here ... }
  KswDpArena<7> arena(target_len, query_len);

  arena.fill_first_state_arrays(4,
      static_cast<uint8_t>(-gap_open_short - gap_extend_short));
  arena.fill_state_bytes(4,
      static_cast<uint8_t>(-gap_open_long - gap_extend_long));
  arena.fill_state_bytes(5,
      static_cast<uint8_t>(-gap_open_long - gap_extend_long));

  int8_t* diff_up_base       = arena.state_bytes_signed(0).data();
  int8_t* diff_left_base     = arena.state_bytes_signed(1).data();
  int8_t* gap_e_offset_base  = arena.state_bytes_signed(2).data();
  int8_t* gap_f_offset_base  = arena.state_bytes_signed(3).data();
  int8_t* gap_e2_offset_base = arena.state_bytes_signed(4).data();
  int8_t* gap_f2_offset_base = arena.state_bytes_signed(5).data();
  int8_t* diag_score_base    = arena.state_bytes_signed(6).data();
  uint8_t* target_bytes      = arena.target_bytes_span().data();
  uint8_t* query_reversed    = arena.query_reversed_span().data();

  // --- Exact max tracking ---------------------------------------------
  // AI Note: I parametrized hardcoded int32_t to be score_t
  std::vector<score_t> exact_score_row_storage;
  score_t* exact_score_row = nullptr;
  if (!use_approx_max) {
      exact_score_row_storage.assign(
         // AI Note: Previopusly we used kVecBytes here which is terrible because it wouldn't work on other targets
         // We can have up to MaxLanes(d) but we actually have only Lanes(d), so the data could possible not fit into vector, again MaxLanes can be used for allocation, reserving capacity
         // but if you use it to load data the vector will just accept Lanes(d) so we won't load some data points!!
          static_cast<size_t>(arena.target_vec_count()) * byte_lanes_count, kKswNegInf);
      exact_score_row = exact_score_row_storage.data();
  }

  // --- Backtrack storage ----------------------------------------------
  // See the z2 variant for the rationale behind the byte-addressed
  // matrix; the d2 kernel uses exactly the same write pattern.
  KswBacktrackMatrix backtrack;
  std::span<int> anti_diag_band_start{};
  std::span<int> anti_diag_band_end{};
  if (compute_cigar) {
      backtrack.allocate(query_len + target_len - 1, band_vec_capacity);
      anti_diag_band_start = backtrack.band_starts();
      anti_diag_band_end   = backtrack.band_ends();
  }

  // AI Note: We don't ahve to do this reversing this is wasteful
  // Note that lateer we access sequences (reversed) only using query_reversed_aligned[target_position]
  // so in a loop that iterates we can have to values target_position = ... and target_position_rev = band_end 
  // and then do target_position_rev -= byte_lanes_count
  // After that you can hn::Load data and do hn::Reverse(d, vec) to get reversed values!
  // --- Prepare sequences ----------------------------------------------
  for (int query_index = 0; query_index < query_len; ++query_index) {
      query_reversed[query_index] = query.bytes[query_len - 1 - query_index];
  }
  std::memcpy(target_bytes, target.bytes, static_cast<size_t>(target_len));

  // --- Approximate max tracking ---------------------------------------
  // AI Note: I believe chaning to score_t is appropiate
  int32_t approx_score = 0;
  int32_t approx_score_target_pos = 0;

  // ====================================================================
  // Main DP loop
  // ====================================================================
  int prev_band_start = -1;
  int prev_band_end   = -1;
  for (int anti_diagonal = 0; anti_diagonal < query_len + target_len - 1; ++anti_diagonal) {
      int band_start = 0;
      int band_end   = target_len - 1;

      // --- Band boundaries ----------------------------------------
      if (band_start < anti_diagonal - query_len + 1) {
          band_start = anti_diagonal - query_len + 1;
      }
      if (band_end > anti_diagonal) {
          band_end = anti_diagonal;
      }
      if (band_start < (anti_diagonal - right_band + 1) >> 1) {
          band_start = (anti_diagonal - right_band + 1) >> 1;
      }
      if (band_end > (anti_diagonal + left_band) >> 1) {
          band_end = (anti_diagonal + left_band) >> 1;
      }
      if (band_start > band_end) {
          result.zdropped = true;
          break;
      }
      const int band_start_raw = band_start;
      const int band_end_raw   = band_end;
      // AI Note: kVecBytes should be changed to byte_lanes_count, for other cases as well. You should check if it makes sense, but I believe it does.
      // Also kVecBytes * kVecBytes could be extracted to const size_t matrix_size = hn::Lanes(d) * hn::Lanes(d);
      // This value represents NxN matrix of lanes
      // Also note that we already have util functions int_div_ceil and int_div_floor (search for occurences)
      // Using them will be more readable because purpose here is to round band_start to nearest unit of size kVecBytes (so hn::Lanes(d))
      band_start = band_start / kVecBytes * kVecBytes;
      band_end   = (band_end + kVecBytes) / kVecBytes * kVecBytes - 1;

      // --- Boundary conditions (signed 8-bit) ---------------------
      // d2 uses signed arithmetic throughout, so every state array is
      // accessed as `int8_t` — no reinterpret_cast ping-pong.
      int8_t boundary_x_carry;
      int8_t boundary_x2_carry;
      int8_t boundary_v_carry;
      uint8_t* query_reversed_aligned = query_reversed + (query_len - 1 - anti_diagonal);

      if (band_start > 0) {
          if (band_start - 1 >= prev_band_start && band_start - 1 <= prev_band_end) {
              boundary_x_carry  = gap_e_offset_base[band_start - 1];
              boundary_x2_carry = gap_e2_offset_base[band_start - 1];
              boundary_v_carry  = diff_left_base[band_start - 1];
          } else {
              boundary_x_carry  = static_cast<int8_t>(-gap_open_short - gap_extend_short);
              boundary_x2_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
              boundary_v_carry  = static_cast<int8_t>(-gap_open_short - gap_extend_short);
          }
      } else {
          boundary_x_carry  = static_cast<int8_t>(-gap_open_short - gap_extend_short);
          boundary_x2_carry = static_cast<int8_t>(-gap_open_long - gap_extend_long);
          boundary_v_carry  = anti_diagonal == 0
              ? static_cast<int8_t>(-gap_open_short - gap_extend_short)
              : anti_diagonal < long_gap_threshold
                  ? static_cast<int8_t>(-gap_extend_short)
                  : anti_diagonal == long_gap_threshold
                      ? static_cast<int8_t>(long_gap_diff)
                      : static_cast<int8_t>(-gap_extend_long);
      }
      if (band_end >= anti_diagonal) {
          DEBUG_ASSERT(anti_diagonal < arena.target_vec_count() * kKswVecBytes,
              "anti_diagonal exceeds target-state byte span — allocation too small");
          gap_f_offset_base[anti_diagonal]  =
              static_cast<int8_t>(-gap_open_short - gap_extend_short);
          gap_f2_offset_base[anti_diagonal] =
              static_cast<int8_t>(-gap_open_long - gap_extend_long);
          diff_up_base[anti_diagonal] = anti_diagonal == 0
              ? static_cast<int8_t>(-gap_open_short - gap_extend_short)
              : anti_diagonal < long_gap_threshold
                  ? static_cast<int8_t>(-gap_extend_short)
                  : anti_diagonal == long_gap_threshold
                      ? static_cast<int8_t>(long_gap_diff)
                      : static_cast<int8_t>(-gap_extend_long);
      }

      // --- Compute scores -----------------------------------------
      if (!flags.generic_score) {
          for (int target_position = band_start_raw;
               target_position <= band_end_raw;
               target_position += kVecBytes)
          {
             // AI Note: Okay this is interesting
             // We need to load some type that is traits::letter_type but the sqeuence is encoded on traits::storage_unit. We need to extract individual bits of the elements of the sequence.
             // Previously we had here encode_sequence() to do that and later we just used hn::Load from uint8_t* 
             // I hate it because why we have to translate entire sequence at once 
             // encode_sequence() should not be used
             // 
              auto target_lanes = hn::LoadU(byte_tag,
                  reinterpret_cast<const int8_t*>(&target_bytes[target_position]));
              auto query_lanes = hn::LoadU(byte_tag,
                  reinterpret_cast<const int8_t*>(&query_reversed_aligned[target_position]));
              auto wildcard_mask = hn::Or(
                  hn::VecFromMask(byte_tag, hn::Eq(target_lanes, wildcard_rank_vec)),
                  hn::VecFromMask(byte_tag, hn::Eq(query_lanes, wildcard_rank_vec)));
              auto equal_mask = hn::VecFromMask(byte_tag, hn::Eq(target_lanes, query_lanes));
              auto cell_scores = hn::IfThenElse(
                  hn::MaskFromVec(equal_mask), match_score_vec, mismatch_score_vec);
              cell_scores = hn::IfThenElse(
                  hn::MaskFromVec(wildcard_mask), wildcard_score_vec, cell_scores);
              hn::StoreU(cell_scores, byte_tag, diag_score_base + target_position);
          }
      } else {
          for (int target_position = band_start_raw;
               target_position <= band_end_raw;
               ++target_position)
          {
              diag_score_base[target_position] = scoring_matrix.at(
                  target_bytes[target_position],
                  query_reversed_aligned[target_position]);
          }
      }

      // --- Boundary vectors --------------------------------------
      // AI Note: Please refactor all set_lane_zero functions and other functions that use hardcoded tag types like  KswByteTag/KswByteTagU/KswScoreTag and KswByteVec/KswScoreVe and kKswVecBytes/kKswScoreLanes
      // Those types should be removed any function that have inputs as vectors (or masks) should look like this:
      // template<typename D, typename vec_t = hn::Vec<D>> void fun(const D& d, vec_t some_vector) - it's templated by D and can potentially support override for vector type
      // DO NOT USE HARDCODED TYPES FOR VECTORS OR HWY FULL VECTOR, WE WANT TO HAVE SCALABLE TAGS AND TEMPLATED D TYPES ETC.
      // AI Note: Can you look at how main/bioseq/backend/common/simd/utils.cppm is written or are you stupid?
      auto boundary_x_vec  = set_lane_zero(byte_tag, boundary_x_carry);
      auto boundary_x2_vec = set_lane_zero(byte_tag, boundary_x2_carry);
      auto boundary_v_vec  = set_lane_zero(byte_tag, boundary_v_carry);
      const int band_start_vec = band_start / kVecBytes;
      const int band_end_vec   = band_end / kVecBytes;
      DEBUG_ASSERT(band_end_vec - band_start_vec + 1 <= band_vec_capacity,
          "Band width exceeds allocated column count");

      // ============================================================
      // Core DP
      // ============================================================
      if (!compute_cigar) {
          // --- Score-only --------------------------------------
          // AI Note: can we rename band_vec_index to target_position? This loop looks very similar to the loop above with target_position
          for (int band_vec_index = band_start_vec;
               band_vec_index <= band_end_vec;
               ++band_vec_index)
          {
              auto score_max_cell = hn::Load(byte_tag,
                  diag_score_base + band_vec_index * kVecBytes);
              auto gap_e_shifted = hn::Load(byte_tag,
                  gap_e_offset_base + band_vec_index * kVecBytes);
              gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
              auto diff_left_shifted = hn::Load(byte_tag,
                  diff_left_base + band_vec_index * kVecBytes);
              diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
              auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
              auto diff_up_cur = hn::Load(byte_tag,
                  diff_up_base + band_vec_index * kVecBytes);
              auto vert_chain_sum = hn::Add(
                  hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                  diff_up_cur);

              auto gap_e2_shifted = hn::Load(byte_tag,
                  gap_e2_offset_base + band_vec_index * kVecBytes);
              gap_e2_shifted = shift_left_insert(byte_tag, gap_e2_shifted, boundary_x2_vec);
              auto diag_chain_sum2 = hn::Add(gap_e2_shifted, diff_left_shifted);
              auto vert_chain_sum2 = hn::Add(
                  hn::Load(byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes),
                  diff_up_cur);

              // score_max_cell = max(score, a, b, a2, b2) — all signed
              score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
              score_max_cell = hn::Max(score_max_cell, vert_chain_sum);
              score_max_cell = hn::Max(score_max_cell, diag_chain_sum2);
              score_max_cell = hn::Max(score_max_cell, vert_chain_sum2);
              score_max_cell = hn::Min(score_max_cell, match_score_vec);

              hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                        diff_up_base + band_vec_index * kVecBytes);
              hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                        diff_left_base + band_vec_index * kVecBytes);
              auto score_minus_short_open = hn::Sub(score_max_cell, gap_open_short_vec);
              diag_chain_sum  = hn::Sub(diag_chain_sum,  score_minus_short_open);
              vert_chain_sum  = hn::Sub(vert_chain_sum,  score_minus_short_open);
              auto score_minus_long_open = hn::Sub(score_max_cell, gap_open_long_vec);
              diag_chain_sum2 = hn::Sub(diag_chain_sum2, score_minus_long_open);
              vert_chain_sum2 = hn::Sub(vert_chain_sum2, score_minus_long_open);

              hn::Store(hn::Sub(hn::Max(diag_chain_sum,  zero_vec), short_gap_cost_vec),
                        byte_tag, gap_e_offset_base  + band_vec_index * kVecBytes);
              hn::Store(hn::Sub(hn::Max(vert_chain_sum,  zero_vec), short_gap_cost_vec),
                        byte_tag, gap_f_offset_base  + band_vec_index * kVecBytes);
              hn::Store(hn::Sub(hn::Max(diag_chain_sum2, zero_vec), long_gap_cost_vec),
                        byte_tag, gap_e2_offset_base + band_vec_index * kVecBytes);
              hn::Store(hn::Sub(hn::Max(vert_chain_sum2, zero_vec), long_gap_cost_vec),
                        byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes);
          }
      } else if (!flags.right_align) {
          // --- CIGAR, gap left-alignment ----------------------
          anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
          anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
          for (int band_vec_index = band_start_vec;
               band_vec_index <= band_end_vec;
               ++band_vec_index)
          {
              auto score_max_cell = hn::Load(byte_tag,
                  diag_score_base + band_vec_index * kVecBytes);
              auto gap_e_shifted = hn::Load(byte_tag,
                  gap_e_offset_base + band_vec_index * kVecBytes);
              gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
              auto diff_left_shifted = hn::Load(byte_tag,
                  diff_left_base + band_vec_index * kVecBytes);
              diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
              auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
              auto diff_up_cur = hn::Load(byte_tag,
                  diff_up_base + band_vec_index * kVecBytes);
              auto vert_chain_sum = hn::Add(
                  hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                  diff_up_cur);
              auto gap_e2_shifted = hn::Load(byte_tag,
                  gap_e2_offset_base + band_vec_index * kVecBytes);
              gap_e2_shifted = shift_left_insert(byte_tag, gap_e2_shifted, boundary_x2_vec);
              auto diag_chain_sum2 = hn::Add(gap_e2_shifted, diff_left_shifted);
              auto vert_chain_sum2 = hn::Add(
                  hn::Load(byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes),
                  diff_up_cur);

              // Backtrack direction: find which state achieves the max
              auto direction_vec = hn::IfThenElseZero(
                  hn::Gt(diag_chain_sum, score_max_cell), backtrack_dir_1_vec);
              score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
              direction_vec = hn::IfThenElse(
                  hn::Gt(vert_chain_sum, score_max_cell), backtrack_dir_2_vec, direction_vec);
              score_max_cell = hn::Max(score_max_cell, vert_chain_sum);
              direction_vec = hn::IfThenElse(
                  hn::Gt(diag_chain_sum2, score_max_cell), backtrack_dir_3_vec, direction_vec);
              score_max_cell = hn::Max(score_max_cell, diag_chain_sum2);
              direction_vec = hn::IfThenElse(
                  hn::Gt(vert_chain_sum2, score_max_cell), backtrack_dir_4_vec, direction_vec);
              score_max_cell = hn::Max(score_max_cell, vert_chain_sum2);
              score_max_cell = hn::Min(score_max_cell, match_score_vec);

              hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                        diff_up_base + band_vec_index * kVecBytes);
              hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                        diff_left_base + band_vec_index * kVecBytes);
              auto score_minus_short_open = hn::Sub(score_max_cell, gap_open_short_vec);
              diag_chain_sum  = hn::Sub(diag_chain_sum,  score_minus_short_open);
              vert_chain_sum  = hn::Sub(vert_chain_sum,  score_minus_short_open);
              auto score_minus_long_open = hn::Sub(score_max_cell, gap_open_long_vec);
              diag_chain_sum2 = hn::Sub(diag_chain_sum2, score_minus_long_open);
              vert_chain_sum2 = hn::Sub(vert_chain_sum2, score_minus_long_open);

              auto diag_positive_mask = hn::Gt(diag_chain_sum, zero_vec);
              hn::Store(hn::Sub(hn::IfThenElseZero(diag_positive_mask, diag_chain_sum),
                                 short_gap_cost_vec),
                        byte_tag, gap_e_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenElseZero(diag_positive_mask, backtrack_e_cont));

              auto vert_positive_mask = hn::Gt(vert_chain_sum, zero_vec);
              hn::Store(hn::Sub(hn::IfThenElseZero(vert_positive_mask, vert_chain_sum),
                                 short_gap_cost_vec),
                        byte_tag, gap_f_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenElseZero(vert_positive_mask, backtrack_f_cont));

              auto diag2_positive_mask = hn::Gt(diag_chain_sum2, zero_vec);
              hn::Store(hn::Sub(hn::IfThenElseZero(diag2_positive_mask, diag_chain_sum2),
                                 long_gap_cost_vec),
                        byte_tag, gap_e2_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenElseZero(diag2_positive_mask, backtrack_e2_cont));

              auto vert2_positive_mask = hn::Gt(vert_chain_sum2, zero_vec);
              hn::Store(hn::Sub(hn::IfThenElseZero(vert2_positive_mask, vert_chain_sum2),
                                 long_gap_cost_vec),
                        byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenElseZero(vert2_positive_mask, backtrack_f2_cont));

              hn::Store(direction_vec, byte_tag,
                  backtrack.vec_slot_bytes(anti_diagonal,
                                           band_vec_index - band_start_vec));
          }
      } else {
          // --- CIGAR, gap right-alignment ---------------------
          anti_diag_band_start[static_cast<size_t>(anti_diagonal)] = band_start;
          anti_diag_band_end  [static_cast<size_t>(anti_diagonal)] = band_end;
          for (int band_vec_index = band_start_vec;
               band_vec_index <= band_end_vec;
               ++band_vec_index)
          {
              auto score_max_cell = hn::Load(byte_tag,
                  diag_score_base + band_vec_index * kVecBytes);
              auto gap_e_shifted = hn::Load(byte_tag,
                  gap_e_offset_base + band_vec_index * kVecBytes);
              gap_e_shifted = shift_left_insert(byte_tag, gap_e_shifted, boundary_x_vec);
              auto diff_left_shifted = hn::Load(byte_tag,
                  diff_left_base + band_vec_index * kVecBytes);
              diff_left_shifted = shift_left_insert(byte_tag, diff_left_shifted, boundary_v_vec);
              auto diag_chain_sum = hn::Add(gap_e_shifted, diff_left_shifted);
              auto diff_up_cur = hn::Load(byte_tag,
                  diff_up_base + band_vec_index * kVecBytes);
              auto vert_chain_sum = hn::Add(
                  hn::Load(byte_tag, gap_f_offset_base + band_vec_index * kVecBytes),
                  diff_up_cur);
              auto gap_e2_shifted = hn::Load(byte_tag,
                  gap_e2_offset_base + band_vec_index * kVecBytes);
              gap_e2_shifted = shift_left_insert(byte_tag, gap_e2_shifted, boundary_x2_vec);
              auto diag_chain_sum2 = hn::Add(gap_e2_shifted, diff_left_shifted);
              auto vert_chain_sum2 = hn::Add(
                  hn::Load(byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes),
                  diff_up_cur);

              // Right-align: prefer gaps on the right (opposite tie-break)
              auto direction_vec = hn::IfThenZeroElse(
                  hn::Gt(score_max_cell, diag_chain_sum), backtrack_dir_1_vec);
              score_max_cell = hn::Max(score_max_cell, diag_chain_sum);
              direction_vec = hn::IfThenElse(
                  hn::Gt(score_max_cell, vert_chain_sum), direction_vec, backtrack_dir_2_vec);
              score_max_cell = hn::Max(score_max_cell, vert_chain_sum);
              direction_vec = hn::IfThenElse(
                  hn::Gt(score_max_cell, diag_chain_sum2), direction_vec, backtrack_dir_3_vec);
              score_max_cell = hn::Max(score_max_cell, diag_chain_sum2);
              direction_vec = hn::IfThenElse(
                  hn::Gt(score_max_cell, vert_chain_sum2), direction_vec, backtrack_dir_4_vec);
              score_max_cell = hn::Max(score_max_cell, vert_chain_sum2);
              score_max_cell = hn::Min(score_max_cell, match_score_vec);

              hn::Store(hn::Sub(score_max_cell, diff_left_shifted), byte_tag,
                        diff_up_base + band_vec_index * kVecBytes);
              hn::Store(hn::Sub(score_max_cell, diff_up_cur), byte_tag,
                        diff_left_base + band_vec_index * kVecBytes);
              auto score_minus_short_open = hn::Sub(score_max_cell, gap_open_short_vec);
              diag_chain_sum  = hn::Sub(diag_chain_sum,  score_minus_short_open);
              vert_chain_sum  = hn::Sub(vert_chain_sum,  score_minus_short_open);
              auto score_minus_long_open = hn::Sub(score_max_cell, gap_open_long_vec);
              diag_chain_sum2 = hn::Sub(diag_chain_sum2, score_minus_long_open);
              vert_chain_sum2 = hn::Sub(vert_chain_sum2, score_minus_long_open);

              auto diag_non_positive_mask = hn::Gt(zero_vec, diag_chain_sum);
              hn::Store(hn::Sub(hn::IfThenZeroElse(diag_non_positive_mask, diag_chain_sum),
                                 short_gap_cost_vec),
                        byte_tag, gap_e_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenZeroElse(diag_non_positive_mask, backtrack_e_cont));

              auto vert_non_positive_mask = hn::Gt(zero_vec, vert_chain_sum);
              hn::Store(hn::Sub(hn::IfThenZeroElse(vert_non_positive_mask, vert_chain_sum),
                                 short_gap_cost_vec),
                        byte_tag, gap_f_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenZeroElse(vert_non_positive_mask, backtrack_f_cont));

              auto diag2_non_positive_mask = hn::Gt(zero_vec, diag_chain_sum2);
              hn::Store(hn::Sub(hn::IfThenZeroElse(diag2_non_positive_mask, diag_chain_sum2),
                                 long_gap_cost_vec),
                        byte_tag, gap_e2_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenZeroElse(diag2_non_positive_mask, backtrack_e2_cont));

              auto vert2_non_positive_mask = hn::Gt(zero_vec, vert_chain_sum2);
              hn::Store(hn::Sub(hn::IfThenZeroElse(vert2_non_positive_mask, vert_chain_sum2),
                                 long_gap_cost_vec),
                        byte_tag, gap_f2_offset_base + band_vec_index * kVecBytes);
              direction_vec = hn::Or(direction_vec,
                  hn::IfThenZeroElse(vert2_non_positive_mask, backtrack_f2_cont));

              hn::Store(direction_vec, byte_tag,
                  backtrack.vec_slot_bytes(anti_diagonal,
                                           band_vec_index - band_start_vec));
          }
      }

      // ============================================================
      // Max score tracking and Z-drop
      // ============================================================
      if (!use_approx_max) {
          int32_t anti_diag_max_score;
          int32_t anti_diag_max_target_pos;
          if (anti_diagonal > 0) {
              // d2 difference: no -qe in H update
              anti_diag_max_score = exact_score_row[band_end_raw] = band_end_raw > 0
                  ? exact_score_row[band_end_raw - 1] + static_cast<int32_t>(diff_up_base[band_end_raw])
                  : exact_score_row[band_end_raw] + static_cast<int32_t>(diff_left_base[band_end_raw]);
              anti_diag_max_target_pos = band_end_raw;

              auto max_score_wide_vec = hn::Set(score_tag, anti_diag_max_score);
              auto max_target_pos_vec = hn::Set(score_tag, anti_diag_max_target_pos);

              const int block_end = band_start_raw + (band_end_raw - band_start_raw) / 4 * 4;
              int target_position = band_start_raw;
              for (; target_position < block_end; target_position += 4) {
                  auto exact_scores = hn::LoadU(score_tag,
                      &exact_score_row[target_position]);
                  HWY_ALIGN int32_t diff_left_scalars[4] = {
                      static_cast<int32_t>(diff_left_base[target_position]),
                      static_cast<int32_t>(diff_left_base[target_position + 1]),
                      static_cast<int32_t>(diff_left_base[target_position + 2]),
                      static_cast<int32_t>(diff_left_base[target_position + 3])
                  };
                  auto diff_left_vec = hn::Load(score_tag, diff_left_scalars);
                  exact_scores = hn::Add(exact_scores, diff_left_vec);
                  hn::StoreU(exact_scores, score_tag,
                      &exact_score_row[target_position]);

                  auto target_position_bcast = hn::Set(score_tag, target_position);
                  auto greater_mask = hn::Gt(exact_scores, max_score_wide_vec);
                  max_score_wide_vec = hn::IfThenElse(
                      greater_mask, exact_scores, max_score_wide_vec);
                  max_target_pos_vec = hn::IfThenElse(
                      greater_mask, target_position_bcast, max_target_pos_vec);
              }

              HWY_ALIGN int32_t score_block[4];
              HWY_ALIGN int32_t position_block[4];
              hn::Store(max_score_wide_vec, score_tag, score_block);
              hn::Store(max_target_pos_vec, score_tag, position_block);
              for (int block_lane = 0; block_lane < 4; ++block_lane) {
                  if (anti_diag_max_score < score_block[block_lane]) {
                      anti_diag_max_score = score_block[block_lane];
                      anti_diag_max_target_pos = position_block[block_lane] + block_lane;
                  }
              }
              for (; target_position < band_end_raw; ++target_position) {
                  exact_score_row[target_position] +=
                      static_cast<int32_t>(diff_left_base[target_position]);
                  if (exact_score_row[target_position] > anti_diag_max_score) {
                      anti_diag_max_score = exact_score_row[target_position];
                      anti_diag_max_target_pos = target_position;
                  }
              }
          } else {
              exact_score_row[0] = static_cast<int32_t>(diff_left_base[0])
                  - gap_open_plus_extend;
              anti_diag_max_score = exact_score_row[0];
              anti_diag_max_target_pos = 0;
          }
          if (band_end_raw == target_len - 1 && exact_score_row[band_end_raw] > result.mte) {
              result.mte = exact_score_row[band_end_raw];
              result.mte_q = anti_diagonal - band_end_raw;
          }
          if (anti_diagonal - band_start_raw == query_len - 1
              && exact_score_row[band_start_raw] > result.mqe)
          {
              result.mqe = exact_score_row[band_start_raw];
              result.mqe_t = band_start_raw;
          }
          // d2 uses e2 for Z-drop (not e)
          if (apply_zdrop_heuristic(result, /*use_rotated_coords=*/true,
                                    anti_diag_max_score, anti_diagonal,
                                    anti_diag_max_target_pos,
                                    params.z_drop, gap_extend_long))
          {
              break;
          }
          if (anti_diagonal == query_len + target_len - 2 && band_end_raw == target_len - 1) {
              result.score = exact_score_row[target_len - 1];
          }
      } else {
          // --- Approximate max ------------------------------
          if (anti_diagonal > 0) {
              if (approx_score_target_pos >= band_start_raw
                  && approx_score_target_pos <= band_end_raw
                  && approx_score_target_pos + 1 >= band_start_raw
                  && approx_score_target_pos + 1 <= band_end_raw)
              {
                  const int32_t delta_left = diff_left_base[approx_score_target_pos];
                  const int32_t delta_up = diff_up_base[approx_score_target_pos + 1];
                  if (delta_left > delta_up) {
                      approx_score += delta_left;
                  } else {
                      approx_score += delta_up;
                      ++approx_score_target_pos;
                  }
              } else if (approx_score_target_pos >= band_start_raw
                         && approx_score_target_pos <= band_end_raw)
              {
                  approx_score +=
                      static_cast<int32_t>(diff_left_base[approx_score_target_pos]);
              } else {
                  ++approx_score_target_pos;
                  approx_score +=
                      static_cast<int32_t>(diff_up_base[approx_score_target_pos]);
              }
          } else {
              approx_score = static_cast<int32_t>(diff_left_base[0])
                  - gap_open_plus_extend;
              approx_score_target_pos = 0;
          }
          if (flags.approx_zdrop
              && apply_zdrop_heuristic(result, /*use_rotated_coords=*/true,
                                       approx_score, anti_diagonal,
                                       approx_score_target_pos,
                                       params.z_drop, gap_extend_long))
          {
              break;
          }
          if (anti_diagonal == query_len + target_len - 2 && band_end_raw == target_len - 1) {
              result.score = approx_score;
          }
      }
      prev_band_start = band_start;
      prev_band_end   = band_end;
  }

  // ====================================================================
  // Backtrack
  // ====================================================================
  if (compute_cigar) {
      const bool output_reversed = flags.reverse_cigar;
      if (!result.zdropped && !flags.extz_only) {
          ksw_backtrack_cigar(backtrack,
              /*use_rotated_coords=*/true, output_reversed,
              /*min_intron_length=*/0,
              target_len - 1, query_len - 1, result.cigar);
      } else if (!result.zdropped && flags.extz_only
                 && result.mqe + params.end_bonus > result.max)
      {
          result.reach_end = true;
          ksw_backtrack_cigar(backtrack,
              /*use_rotated_coords=*/true, output_reversed,
              /*min_intron_length=*/0,
              result.mqe_t, query_len - 1, result.cigar);
      } else if (result.max_t >= 0 && result.max_q >= 0) {
          ksw_backtrack_cigar(backtrack,
              /*use_rotated_coords=*/true, output_reversed,
              /*min_intron_length=*/0,
              result.max_t, result.max_q, result.cigar);
      }
  }

  return result;
}