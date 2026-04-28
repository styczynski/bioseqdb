#include "fast_reader.h"
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <x86intrin.h>
#include <isa-l/igzip_lib.h>
#include "macro.h"
#include "bwa.h"

extern uint64_t tprof[LIM_R][LIM_C];

#define FR_INBUF_SIZE  (8 << 20)   /* 8MB compressed input buffer */
#define FR_DECBUF_SIZE (32 << 20)  /* 32MB decoded output buffer */

struct fast_reader_t {
    int fd;
    int is_gzipped;

    /* Compressed input buffer (gzip only) */
    uint8_t *inbuf;
    size_t inbuf_size;
    int eof_in;

    /* Decoded (decompressed / raw) buffer */
    char *decbuf;
    size_t decbuf_size;
    size_t decbuf_len;   /* valid bytes */
    size_t decbuf_pos;   /* parse cursor */
    int eof_dec;

    /* ISA-L inflate state (gzip only) */
    struct inflate_state isal_state;

    int fill_count;
};

/* ── helpers ─────────────────────────────────────────────── */

static size_t full_read(int fd, void *buf, size_t count)
{
    size_t total = 0;
    while (total < count) {
        ssize_t n = read(fd, (char *)buf + total, count - total);
        if (n <= 0) break;
        total += n;
    }
    return total;
}

static int ends_with_gz(const char *s)
{
    size_t len = strlen(s);
    return (len > 3 && strcmp(s + len - 3, ".gz") == 0);
}

/* ── open / close ────────────────────────────────────────── */

fast_reader_t *fast_reader_open(int fd, const char *filename)
{
    fast_reader_t *fr = (fast_reader_t *)calloc(1, sizeof(fast_reader_t));
    fr->fd = fd;
    fr->is_gzipped = ends_with_gz(filename);

    fr->decbuf_size = FR_DECBUF_SIZE;
    fr->decbuf = (char *)malloc(fr->decbuf_size);

    if (fr->is_gzipped) {
        fr->inbuf_size = FR_INBUF_SIZE;
        fr->inbuf = (uint8_t *)malloc(fr->inbuf_size);
        isal_inflate_init(&fr->isal_state);
        fr->isal_state.crc_flag = ISAL_GZIP;
    }

    fr->fill_count = 0;
    if (bwa_verbose >= 4)
        fprintf(stderr, "[fast_reader] opened %s (gzip=%d)\n", filename, fr->is_gzipped);
    return fr;
}

void fast_reader_close(fast_reader_t *fr)
{
    if (!fr) return;
    close(fr->fd);
    free(fr->decbuf);
    free(fr->inbuf);
    free(fr);
}

/* ── fill decode buffer ──────────────────────────────────── */

static int fast_reader_fill(fast_reader_t *fr)
{
    if (fr->eof_dec) return 0;

    /* shift unconsumed data to front */
    size_t remaining = fr->decbuf_len - fr->decbuf_pos;
    if (remaining > 0 && fr->decbuf_pos > 0)
        memmove(fr->decbuf, fr->decbuf + fr->decbuf_pos, remaining);
    fr->decbuf_len = remaining;
    fr->decbuf_pos = 0;

    if (!fr->is_gzipped) {
        /* ── uncompressed: direct read ── */
        size_t space = fr->decbuf_size - fr->decbuf_len;
        size_t n = full_read(fr->fd, fr->decbuf + fr->decbuf_len, space);
        if (n == 0) {
            fr->eof_dec = 1;
            return remaining > 0 ? 1 : 0;
        }
        fr->decbuf_len += n;
        fr->fill_count++;
        if (fr->fill_count <= 3 && bwa_verbose >= 4)
            fprintf(stderr, "[fast_reader] fill #%d: decbuf_len=%zu (raw)\n",
                    fr->fill_count, fr->decbuf_len);
        return 1;
    } else {
        /* ── gzip: ISA-L inflate ── */
        struct inflate_state *st = &fr->isal_state;

        while (fr->decbuf_len < fr->decbuf_size) {
            /* refill compressed input if exhausted */
            if (st->avail_in == 0 && !fr->eof_in) {
                size_t n = full_read(fr->fd, fr->inbuf, fr->inbuf_size);
                if (n == 0) {
                    fr->eof_in = 1;
                } else {
                    st->next_in = fr->inbuf;
                    st->avail_in = n;
                }
            }

            if (st->avail_in == 0 && fr->eof_in) {
                fr->eof_dec = 1;
                break;
            }

            st->next_out = (uint8_t *)fr->decbuf + fr->decbuf_len;
            st->avail_out = fr->decbuf_size - fr->decbuf_len;

            int ret = isal_inflate(st);
            fr->decbuf_len = fr->decbuf_size - st->avail_out;

            if (ret != ISAL_DECOMP_OK) {
                fprintf(stderr, "[fast_reader] ISA-L inflate error: %d\n", ret);
                fr->eof_dec = 1;
                break;
            }

            if (st->block_state == ISAL_BLOCK_FINISH) {
                /* end of gzip member — check for concatenated streams */
                if (st->avail_in > 0 || !fr->eof_in) {
                    isal_inflate_reset(st);
                    st->crc_flag = ISAL_GZIP;
                } else {
                    fr->eof_dec = 1;
                }
                break;  /* return what we have, decompress more next call */
            }

            if (st->avail_out == 0) break;  /* output buffer full */
        }

        fr->fill_count++;
        if (fr->fill_count <= 3 && bwa_verbose >= 4)
            fprintf(stderr, "[fast_reader] fill #%d: decbuf_len=%zu (gzip)\n",
                    fr->fill_count, fr->decbuf_len);

        return fr->decbuf_len > 0 ? 1 : 0;
    }
}

/* ── trim /1 or /2 read-number suffix from name (like kseq trim_readno) ── */

static inline void trim_readno_fast(char *name, size_t *len)
{
    if (*len > 2 && name[*len - 2] == '/' &&
        (name[*len - 1] == '1' || name[*len - 1] == '2')) {
        *len -= 2;
        name[*len] = '\0';
    }
}

/* ── parse one FASTQ record from a fast_reader ───────────── */

static int fast_reader_next(fast_reader_t *fr, bseq1_t *seq, str_arena_t *arena,
                            uint64_t *t_decomp)
{
    char *nl1, *nl2, *nl3, *nl4;
    char *name_start, *comment_start, *seq_start, *qual_start;
    char *sep, *tab, *blk, *wp;
    size_t name_len, comment_len, seq_len, qual_len, line1_len, tot;

    while (1) {
        /* ensure decode buffer has data */
        if (fr->decbuf_pos >= fr->decbuf_len) {
            uint64_t t0 = __rdtsc();
            if (!fast_reader_fill(fr)) { *t_decomp += __rdtsc() - t0; return 0; }
            *t_decomp += __rdtsc() - t0;
        }

        char *buf = fr->decbuf;
        size_t pos = fr->decbuf_pos;
        size_t len = fr->decbuf_len;

        char *p = buf + pos;
        size_t rem = len - pos;

        /* skip whitespace / blank lines before '@' */
        while (rem > 0 && (*p == '\n' || *p == '\r')) {
            p++; pos++; rem--;
        }
        if (rem == 0) { fr->decbuf_pos = pos; continue; }
        if (*p != '@') {
            char *nl = (char *)memchr(p, '\n', rem);
            if (!nl) { fr->decbuf_pos = len; continue; }
            fr->decbuf_pos = nl + 1 - buf;
            continue;
        }

        /* Line 1: @name [comment]\n */
        nl1 = (char *)memchr(p, '\n', rem);
        if (!nl1) { fr->decbuf_pos = pos; goto refill; }

        /* Line 2: sequence\n */
        nl2 = (char *)memchr(nl1 + 1, '\n', rem - (size_t)(nl1 + 1 - p));
        if (!nl2) { fr->decbuf_pos = pos; goto refill; }

        /* Line 3: +[...]\n */
        nl3 = (char *)memchr(nl2 + 1, '\n', rem - (size_t)(nl2 + 1 - p));
        if (!nl3) { fr->decbuf_pos = pos; goto refill; }

        /* Line 4: quality\n */
        nl4 = (char *)memchr(nl3 + 1, '\n', rem - (size_t)(nl3 + 1 - p));
        if (!nl4) {
            if (fr->eof_dec && nl3 + 1 < buf + len) {
                nl4 = buf + len;
            } else {
                fr->decbuf_pos = pos;
                goto refill;
            }
        }

        /* ── extract fields ── */
        name_start = p + 1;
        line1_len = (size_t)(nl1 - name_start);

        sep = (char *)memchr(name_start, ' ', line1_len);
        tab = (char *)memchr(name_start, '\t', line1_len);
        if (tab && (!sep || tab < sep)) sep = tab;

        if (sep) {
            name_len = (size_t)(sep - name_start);
            comment_start = sep + 1;
            comment_len = (size_t)(nl1 - comment_start);
        } else {
            name_len = line1_len;
            comment_start = NULL;
            comment_len = 0;
        }

        seq_start  = nl1 + 1;
        seq_len    = (size_t)(nl2 - seq_start);
        qual_start = nl3 + 1;
        qual_len   = (size_t)(nl4 - qual_start);

        if (name_len > 0 && name_start[name_len - 1] == '\r') name_len--;
        if (comment_len > 0 && comment_start[comment_len - 1] == '\r') comment_len--;
        if (seq_len > 0 && seq_start[seq_len - 1] == '\r') seq_len--;
        if (qual_len > 0 && qual_start[qual_len - 1] == '\r') qual_len--;

        /* allocate from arena & copy */
        tot = (name_len + 1) + (seq_len + 1);
        if (comment_len > 0) tot += comment_len + 1;
        if (qual_len > 0)    tot += qual_len + 1;

        blk = str_arena_alloc(arena, tot);
        wp  = blk;

        seq->name = wp;  memcpy(wp, name_start, name_len);  wp[name_len] = '\0';
        trim_readno_fast(seq->name, &name_len);
        wp += name_len + 1;
        if (comment_len > 0) {
            seq->comment = wp;  memcpy(wp, comment_start, comment_len);  wp[comment_len] = '\0';  wp += comment_len + 1;
        } else {
            seq->comment = NULL;
        }
        seq->seq = wp;   memcpy(wp, seq_start, seq_len);    wp[seq_len] = '\0';   wp += seq_len + 1;
        if (qual_len > 0) {
            seq->qual = wp;  memcpy(wp, qual_start, qual_len);  wp[qual_len] = '\0';
        } else {
            seq->qual = NULL;
        }
        seq->l_seq = seq_len;
        seq->sam   = NULL;
        seq->id    = 0;

        fr->decbuf_pos = (nl4 < buf + len) ? (size_t)(nl4 + 1 - buf) : len;
        return 1;

    refill:
        if (fr->eof_dec) return 0;
        uint64_t t0 = __rdtsc();
        if (!fast_reader_fill(fr)) { *t_decomp += __rdtsc() - t0; return 0; }
        *t_decomp += __rdtsc() - t0;
    }
}

/* ── read a chunk of FASTQ records (single-end) ─────────── */

bseq1_t *fast_reader_read_chunk(fast_reader_t *fr, int64_t chunk_size, int *n_seqs,
                                 int64_t *total_size, str_arena_t *arena)
{
    uint64_t t_decomp = 0, t_realloc = 0;

    int m = chunk_size / 80 + 256;
    bseq1_t *seqs = (bseq1_t *)malloc(m * sizeof(bseq1_t));
    str_arena_init(arena);

    int n = 0;
    int64_t size = 0;

    while (size < chunk_size) {
        if (n >= m) {
            uint64_t tr = __rdtsc();
            m <<= 1;
            seqs = (bseq1_t *)realloc(seqs, m * sizeof(bseq1_t));
            t_realloc += __rdtsc() - tr;
        }
        if (!fast_reader_next(fr, &seqs[n], arena, &t_decomp)) break;
        seqs[n].id = n;
        size += seqs[n].l_seq;
        n++;
    }

    tprof[READ_IO_KSEQ][0] += t_decomp;
    tprof[READ_IO_REALLOC][0] += t_realloc;

    if (n == 0) {
        free(seqs);
        *n_seqs = 0;
        *total_size = 0;
        return NULL;
    }

    *n_seqs = n;
    *total_size = size;
    return seqs;
}

/* ── read a chunk of FASTQ records (paired-end, interleaved) ── */

bseq1_t *fast_reader_read_chunk_pe(fast_reader_t *fr, fast_reader_t *fr2,
                                    int64_t chunk_size, int *n_seqs,
                                    int64_t *total_size, str_arena_t *arena)
{
    uint64_t t_decomp = 0, t_realloc = 0;

    int m = chunk_size / 80 + 256;
    bseq1_t *seqs = (bseq1_t *)malloc(m * sizeof(bseq1_t));
    str_arena_init(arena);

    int n = 0;
    int64_t size = 0;

    while (size < chunk_size) {
        /* ensure space for 2 records */
        if (n + 1 >= m) {
            uint64_t tr = __rdtsc();
            m <<= 1;
            seqs = (bseq1_t *)realloc(seqs, m * sizeof(bseq1_t));
            t_realloc += __rdtsc() - tr;
        }
        /* read R1 */
        if (!fast_reader_next(fr, &seqs[n], arena, &t_decomp)) break;
        seqs[n].id = n;
        size += seqs[n].l_seq;
        n++;

        /* read R2 */
        if (!fast_reader_next(fr2, &seqs[n], arena, &t_decomp)) {
            fprintf(stderr, "[W::%s] the 2nd file has fewer sequences.\n", __func__);
            break;
        }
        seqs[n].id = n;
        size += seqs[n].l_seq;
        n++;
    }

    tprof[READ_IO_KSEQ][0] += t_decomp;
    tprof[READ_IO_REALLOC][0] += t_realloc;

    if (n == 0) {
        free(seqs);
        *n_seqs = 0;
        *total_size = 0;
        return NULL;
    }

    *n_seqs = n;
    *total_size = size;
    return seqs;
}
