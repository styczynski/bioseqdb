#include "common.h"

#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "mmpriv.h"

#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
#include "instr_shim.h"
#endif

FILE *mm_split_init(const char *prefix, const mm_idx_t *mi)
{
	char *fn;
	FILE *fp;
	uint32_t i, k = mi->k;
	fn = (char*)calloc(strlen(prefix) + 10, 1);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
	if (fn == 0) {
		bioseq_minimap2_e2e_io_error_mark();
		return 0;
	}
#endif
	sprintf(fn, "%s.%.4d.tmp", prefix, mi->index);
	if ((fp = fopen(fn, "wb")) == NULL) {
		if (mm_verbose >= 1)
			fprintf(stderr, "[ERROR]\033[1;31m failed to write to temporary file '%s'\033[0m: %s\n", fn, strerror(errno));
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
		bioseq_minimap2_e2e_io_error_mark();
		free(fn);
		return 0;
#else
		exit(1);
#endif
	}
	mm_err_fwrite(&k, 4, 1, fp);
	mm_err_fwrite(&mi->n_seq, 4, 1, fp);
	for (i = 0; i < mi->n_seq; ++i) {
		uint32_t l;
		l = strlen(mi->seq[i].name);
		mm_err_fwrite(&l, 1, 4, fp);
		mm_err_fwrite(mi->seq[i].name, 1, l, fp);
		mm_err_fwrite(&mi->seq[i].len, 4, 1, fp);
	}
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
	if (bioseq_minimap2_e2e_io_error_seen()) {
		bioseq_minimap2_e2e_close_split_output(fp);
		free(fn);
		return 0;
	}
#endif
	free(fn);
	return fp;
}

mm_idx_t *mm_split_merge_prep(const char *prefix, int n_splits, FILE **fp, uint32_t *n_seq_part)
{
	mm_idx_t *mi = 0;
	char *fn;
	int i, j;

	if (n_splits < 1) return 0;
	fn = CALLOC(char, strlen(prefix) + 10);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
	if (fn == 0) {
		bioseq_minimap2_e2e_io_error_mark();
		return 0;
	}
#endif
	for (i = 0; i < n_splits; ++i) {
		sprintf(fn, "%s.%.4d.tmp", prefix, i);
		if ((fp[i] = fopen(fn, "rb")) == 0) {
			if (mm_verbose >= 1)
				fprintf(stderr, "ERROR: failed to open temporary file '%s': %s\n", fn, strerror(errno));
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
			bioseq_minimap2_e2e_io_error_mark();
			for (j = 0; j < i; ++j) {
				bioseq_minimap2_e2e_close_split_input(fp[j]);
				fp[j] = 0;
			}
#else
			for (j = 0; j < i; ++j)
				fclose(fp[j]);
#endif
			free(fn);
			return 0;
		}
	}
	free(fn);

	mi = CALLOC(mm_idx_t, 1);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
	if (mi == 0) {
		bioseq_minimap2_e2e_io_error_mark();
		goto e2e_io_fail;
	}
#endif
	for (i = 0; i < n_splits; ++i) {
		mm_err_fread(&mi->k, 4, 1, fp[i]); // TODO: check if k is all the same
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
		if (bioseq_minimap2_e2e_io_error_seen()) goto e2e_io_fail;
#endif
		mm_err_fread(&n_seq_part[i], 4, 1, fp[i]);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
		if (bioseq_minimap2_e2e_io_error_seen()) goto e2e_io_fail;
		if (n_seq_part[i] > UINT32_MAX - mi->n_seq) {
			bioseq_minimap2_e2e_io_error_mark();
			goto e2e_io_fail;
		}
#endif
		mi->n_seq += n_seq_part[i];
	}
	mi->seq = CALLOC(mm_idx_seq_t, mi->n_seq);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
	if (mi->n_seq != 0 && mi->seq == 0) {
		bioseq_minimap2_e2e_io_error_mark();
		goto e2e_io_fail;
	}
#endif
	for (i = j = 0; i < n_splits; ++i) {
		uint32_t k;
		for (k = 0; k < n_seq_part[i]; ++k, ++j) {
			uint32_t l;
			mm_err_fread(&l, 1, 4, fp[i]);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
			if (bioseq_minimap2_e2e_io_error_seen()) goto e2e_io_fail;
			if (l == UINT32_MAX) {
				bioseq_minimap2_e2e_io_error_mark();
				goto e2e_io_fail;
			}
#endif
			mi->seq[j].name = (char*)calloc(l + 1, 1);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
			if (mi->seq[j].name == 0) {
				bioseq_minimap2_e2e_io_error_mark();
				goto e2e_io_fail;
			}
#endif
			mm_err_fread(mi->seq[j].name, 1, l, fp[i]);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
			if (bioseq_minimap2_e2e_io_error_seen()) goto e2e_io_fail;
#endif
			mm_err_fread(&mi->seq[j].len, 4, 1, fp[i]);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
			if (bioseq_minimap2_e2e_io_error_seen()) goto e2e_io_fail;
#endif
		}
	}
	return mi;

#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
e2e_io_fail:
	if (mi != 0) {
		if (mi->seq == 0) mi->n_seq = 0;
		mm_idx_destroy(mi);
	}
	for (i = 0; i < n_splits; ++i) {
		if (fp[i] != 0) {
			bioseq_minimap2_e2e_close_split_input(fp[i]);
			fp[i] = 0;
		}
	}
	return 0;
#endif
}

void mm_split_rm_tmp(const char *prefix, int n_splits)
{
	int i;
	char *fn;
	fn = CALLOC(char, strlen(prefix) + 10);
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
	if (fn == 0) {
		bioseq_minimap2_e2e_io_error_mark();
		return;
	}
#endif
	for (i = 0; i < n_splits; ++i) {
		sprintf(fn, "%s.%.4d.tmp", prefix, i);
		remove(fn);
	}
	free(fn);
}
