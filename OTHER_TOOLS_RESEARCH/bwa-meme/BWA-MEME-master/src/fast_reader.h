#ifndef FAST_READER_H
#define FAST_READER_H

#include <stdint.h>
#include "bwa.h"

typedef struct fast_reader_t fast_reader_t;

fast_reader_t *fast_reader_open(int fd, const char *filename);
void fast_reader_close(fast_reader_t *fr);
bseq1_t *fast_reader_read_chunk(fast_reader_t *fr, int64_t chunk_size, int *n_seqs,
                                 int64_t *total_size, str_arena_t *arena);
/* Paired-end: interleave records from fr (R1) and fr2 (R2) */
bseq1_t *fast_reader_read_chunk_pe(fast_reader_t *fr, fast_reader_t *fr2,
                                    int64_t chunk_size, int *n_seqs,
                                    int64_t *total_size, str_arena_t *arena);

#endif
