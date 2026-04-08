/*
 *                             The MIT License
 *
 * Wavefront Alignments Algorithms
 * Copyright (c) 2017 by Santiago Marco-Sola  <santiagomsola@gmail.com>
 *
 * This file is part of Wavefront Alignments Algorithms.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * PROJECT: Wavefront Alignments Algorithms
 * AUTHOR(S): Santiago Marco-Sola <santiagomsola@gmail.com>
 * DESCRIPTION: WFA display functions
 */

#include "gap_affine/affine_wavefront_display.h"
#include "gap_affine/affine_wavefront_utils.h"

/*
 * Accessors
 */
void affine_wavefronts_set_edit_table(
    affine_wavefronts_t* const affine_wavefronts,
    const int pattern_length,
    const int text_length,
    const int k,
    const awf_offset_t offset,
    const int score) {
#ifdef AFFINE_WAVEFRONT_DEBUG
  if (offset < 0) return;
  const int h = AFFINE_WAVEFRONT_H(k,offset);
  const int v = AFFINE_WAVEFRONT_V(k,offset);
  if (0 <= v && v <= pattern_length &&
      0 <= h && h <= text_length) {
    if (affine_wavefronts->gap_affine_table.columns[h][v].M == -1) {
      affine_wavefronts->gap_affine_table.columns[h][v].M = score;
    }
//    // Overwritte
//    else {
//      fprintf(stderr,"Attempt to overwrite (score %d over %d)\n",
//          score,affine_wavefronts->gap_affine_table.columns[h][v].M);
//    }
//    // Out negative
  }
//  else if (v < 0 || h < 0) {
//    fprintf(stderr,"Out-of-table. Negative values (h,v)=(%d,%d)\n",h,v);
//  }
//    // Out
//  else {
//    fprintf(stderr,"Out-of-table. Beyond table limits\n");
//  }
#endif
}
/*
 * Display
 */
#define AFFINE_WAVEFRONTS_PRINT_ELEMENT(wavefront,k) \
  /* Check limits */ \
  if (wavefront!=NULL && wavefront->lo <= k && k <= wavefront->hi) { \
    if (wavefront->offsets[k] >= 0) { \
      fprintf(stream,"[%2d]",(int)wavefront->offsets[k]); \
    } else { \
      fprintf(stream,"[  ]"); \
    } \
  } else { \
    fprintf(stream,"    "); \
  }
void affine_wavefronts_print_wavefronts_block(
    FILE* const stream,
    affine_wavefronts_t* const affine_wavefronts,
    const int score_begin,
    const int score_end) {
  // Compute min/max k
  int s, max_k=0, min_k=0;
  for (s=score_begin;s<=score_end;++s) {
    affine_wavefront_t* const mwavefront = affine_wavefronts->mwavefronts[s];
    if (mwavefront != NULL) {
      max_k = MAX(max_k,mwavefront->hi);
      min_k = MIN(min_k,mwavefront->lo);
    }
    affine_wavefront_t* const i1wavefront = affine_wavefronts->iwavefronts[s];
    if (i1wavefront != NULL) {
      max_k = MAX(max_k,i1wavefront->hi);
      min_k = MIN(min_k,i1wavefront->lo);
    }
    affine_wavefront_t* const d1wavefront = affine_wavefronts->dwavefronts[s];
    if (d1wavefront != NULL) {
      max_k = MAX(max_k,d1wavefront->hi);
      min_k = MIN(min_k,d1wavefront->lo);
    }
  }
  // Headers
  fprintf(stream,">[SCORE %3d-%3d]\n",score_begin,score_end);
  fprintf(stream,"         ");
  for (s=score_begin;s<=score_end;++s) {
    fprintf(stream,"[ M][I1][I2][D1][D2] ");
  }
  fprintf(stream,"\n");
  fprintf(stream,"        ");
  for (s=score_begin;s<=score_end;++s) {
    fprintf(stream,"+--------------------");
  }
  fprintf(stream,"\n");
  // Traverse all diagonals
  int k;
  for (k=max_k;k>=min_k;k--) {
    fprintf(stream,"[k=%3d] ",k);
    // Traverse all scores
    for (s=score_begin;s<=score_end;++s) {
      fprintf(stream,"|");
      // Fetch wavefront
      affine_wavefront_t* const mwavefront = affine_wavefronts->mwavefronts[s];
      affine_wavefront_t* const iwavefront = affine_wavefronts->iwavefronts[s];
      affine_wavefront_t* const dwavefront = affine_wavefronts->dwavefronts[s];
      AFFINE_WAVEFRONTS_PRINT_ELEMENT(mwavefront,k);
      AFFINE_WAVEFRONTS_PRINT_ELEMENT(iwavefront,k);
      fprintf(stream,"    ");
      AFFINE_WAVEFRONTS_PRINT_ELEMENT(dwavefront,k);
      fprintf(stream,"    ");
    }
    fprintf(stream,"|\n");
  }
  // Headers
  fprintf(stream,"        ");
  for (s=score_begin;s<=score_end;++s) {
    fprintf(stream,"+--------------------");
  }
  fprintf(stream,"\n");
  fprintf(stream,"SCORE   ");
  for (s=score_begin;s<=score_end;++s) {
    fprintf(stream,"          %2d         ",s);
  }
  fprintf(stream,"\n");
}
void affine_wavefronts_print_wavefronts(
    FILE* const stream,
    affine_wavefronts_t* const affine_wavefronts,
    const int current_score) {
  // Print wavefronts by chunks
  const int display_block_wf = 5;
  int s;
  for (s=0;s<=current_score;s+=display_block_wf) {
    affine_wavefronts_print_wavefronts_block(stream,
        affine_wavefronts,s,MIN(s+display_block_wf,current_score));
  }
}
/*
 * Debug
 */
void affine_wavefronts_debug_step(
    affine_wavefronts_t* const affine_wavefronts,
    const char* const pattern,
    const char* const text,
    const int score) {
#ifdef AFFINE_WAVEFRONT_DEBUG
//  affine_table_print(stderr,&affine_wavefronts->gap_affine_table,pattern,text);
//  affine_wavefronts_print_wavefronts_pretty(stderr,affine_wavefronts,score);
//  affine_wavefronts_print_wavefronts(stderr,affine_wavefronts,score);
//  affine_wavefronts_print_wavefront(stderr,affine_wavefronts,score);
//  affine_wavefronts_print_computation_stats(stderr,affine_wavefronts);
#endif
}


