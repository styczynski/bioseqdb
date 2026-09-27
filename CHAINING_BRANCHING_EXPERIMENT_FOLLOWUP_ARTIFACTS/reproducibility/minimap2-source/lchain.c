#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <assert.h>
#include "mmpriv.h"
#include "kalloc.h"
#include "krmq.h"

static int64_t mg_chain_bk_end(int32_t max_drop, const mm128_t *z, const int32_t *f, const int64_t *p, int32_t *t, int64_t k)
{
	int64_t i = z[k].y, end_i = -1, max_i = i;
	int32_t max_s = 0;
	if (i < 0 || t[i] != 0) return i;
	do {
		int32_t s;
		t[i] = 2;
		end_i = i = p[i];
		s = i < 0? z[k].x : (int32_t)z[k].x - f[i];
		if (s > max_s) max_s = s, max_i = i;
		else if (max_s - s > max_drop) break;
	} while (i >= 0 && t[i] == 0);
	for (i = z[k].y; i >= 0 && i != end_i; i = p[i]) // reset modified t[]
		t[i] = 0;
	return max_i;
}

/*
 * Experiment-only support for measuring the BioSeqDB first-path SIMD branch.
 * The code is inactive unless MM_RUNL_EXPERIMENT is set in the environment.
 */
static int runl_experiment_enabled(void)
{
	const char *value = getenv("MM_RUNL_EXPERIMENT");
	return value != 0 && value[0] != 0 && strcmp(value, "0") != 0;
}

static int runl_candidate_index_cmp(const void *left_, const void *right_)
{
	const mm128_t *left = (const mm128_t *)left_;
	const mm128_t *right = (const mm128_t *)right_;
	return left->y < right->y? -1 : left->y > right->y? 1 : 0;
}

static void runl_sort_equal_score_candidates(mm128_t *z, int64_t n_z)
{
	int64_t begin, end;
	for (begin = 0; begin < n_z; begin = end) {
		for (end = begin + 1; end < n_z && z[end].x == z[begin].x; ++end) {}
		if (end - begin > 1)
			qsort(z + begin, (size_t)(end - begin), sizeof(*z), runl_candidate_index_cmp);
	}
}

static void runl_measure_first_path(const char *stage, int64_t n, int64_t n_z,
									const mm128_t *z, const int32_t *f, const int64_t *p,
									int32_t min_sc)
{
	static uint64_t next_call_id = 0;
	static const int32_t widths[] = { 4, 8, 16 };
	uint64_t call_id = __atomic_add_fetch(&next_call_id, 1, __ATOMIC_RELAXED);
	const char *dump_dir = getenv("MM_RUNL_DUMP_DIR");
	size_t width_index;

	if (dump_dir != 0 && dump_dir[0] != 0) {
		char path[4096];
		FILE *dump;
		int64_t i;
		snprintf(path, sizeof(path), "%s/runl_call_%" PRIu64 "_%s.tsv", dump_dir, call_id, stage);
		dump = fopen(path, "w");
		if (dump == 0) {
			fprintf(stderr, "MM_RUNL_ERROR\tunable_to_open_dump=%s\n", path);
		} else {
			fprintf(dump, "META\t%s\t%" PRIu64 "\t%" PRId64 "\t%" PRId64 "\t%d\n", stage, call_id, n, n_z, min_sc);
			for (i = 0; i < n_z; ++i)
				fprintf(dump, "Z\t%" PRId64 "\t%" PRIu64 "\t%" PRIu64 "\n", i, z[i].x, z[i].y);
			for (i = 0; i < n; ++i)
				fprintf(dump, "P\t%" PRId64 "\t%d\t%" PRId64 "\n", i, f[i], p[i]);
			fclose(dump);
		}
	}

	for (width_index = 0; width_index < sizeof(widths) / sizeof(widths[0]); ++width_index) {
		const int64_t lanes = widths[width_index];
		int64_t start = n_z > 0? (int64_t)z[n_z - 1].y : -1;
		int64_t j = start, c = 0;
		uint64_t loop_evaluations = 0, bounds_pass = 0;
		uint64_t bounds_fail_current = 0, bounds_fail_capacity = 0, bounds_fail_both = 0;
		uint64_t contiguous_mismatch = 0, fast_batches = 0, scalar_fallbacks = 0;
		uint64_t scalar_validation_error = 0, emulation_error = 0, scalar_path_length = 0;
		int64_t scalar_j = start;

		while (scalar_j >= 0 && scalar_path_length < (uint64_t)n) {
			int64_t next;
			if (scalar_j >= n) {
				scalar_validation_error = 1;
				break;
			}
			next = p[scalar_j];
			++scalar_path_length;
			if (next < -1 || next >= scalar_j) {
				scalar_validation_error = 1;
				break;
			}
			scalar_j = next;
		}
		if (scalar_j >= 0 && scalar_path_length >= (uint64_t)n)
			scalar_validation_error = 1;

		while (j >= 0 && c < n) {
			int position_ok = j >= lanes - 1;
			int capacity_ok = c <= n - lanes;
			int contiguous = 1;
			int64_t u;

			++loop_evaluations;
			if (!position_ok || !capacity_ok) {
				if (!position_ok && !capacity_ok) ++bounds_fail_both;
				else if (!position_ok) ++bounds_fail_current;
				else ++bounds_fail_capacity;
			} else {
				++bounds_pass;
				for (u = 0; u < lanes; ++u) {
					if (p[j - u] != j - u - 1) {
						contiguous = 0;
						break;
					}
				}
			}

			if (position_ok && capacity_ok && contiguous) {
				++fast_batches;
				j -= lanes;
				c += lanes;
			} else {
				int64_t next;
				if (position_ok && capacity_ok) ++contiguous_mismatch;
				++scalar_fallbacks;
				if (j >= n) {
					emulation_error = 1;
					break;
				}
				next = p[j];
				++c;
				if (next < -1 || next >= j) {
					emulation_error = 1;
					break;
				}
				j = next;
			}
		}

		fprintf(stderr,
				"MM_RUNL\t{\"schema_version\":1,\"call_id\":%" PRIu64
				",\"stage\":\"%s\",\"lanes\":%" PRId64
				",\"anchor_count\":%" PRId64 ",\"candidate_end_count\":%" PRId64
				",\"start_index\":%" PRId64 ",\"start_score\":%" PRId64
				",\"loop_evaluations\":%" PRIu64 ",\"bounds_pass\":%" PRIu64
				",\"bounds_fail_current\":%" PRIu64 ",\"bounds_fail_capacity\":%" PRIu64
				",\"bounds_fail_both\":%" PRIu64 ",\"contiguous_mismatch\":%" PRIu64
				",\"fast_batches\":%" PRIu64 ",\"scalar_fallbacks\":%" PRIu64
				",\"primary_path_anchors\":%" PRId64 ",\"fast_covered_anchors\":%" PRIu64
				",\"scalar_path_length\":%" PRIu64 ",\"scalar_validation_error\":%" PRIu64
				",\"emulation_error\":%" PRIu64 ",\"min_score\":%d"
				",\"has_ranked_end\":%d,\"has_fast_batch\":%d}\n",
				call_id, stage, lanes, n, n_z, start,
				n_z > 0? (int64_t)z[n_z - 1].x : -1,
				loop_evaluations, bounds_pass, bounds_fail_current, bounds_fail_capacity,
				bounds_fail_both, contiguous_mismatch, fast_batches, scalar_fallbacks,
				c, (uint64_t)lanes * fast_batches, scalar_path_length, scalar_validation_error,
				emulation_error, min_sc, n_z > 0, fast_batches > 0);
	}
}

uint64_t *mg_chain_backtrack(void *km, int64_t n, const int32_t *f, const int64_t *p, int32_t *v, int32_t *t, int32_t min_cnt, int32_t min_sc, int32_t max_drop, int32_t *n_u_, int32_t *n_v_, const char *runl_stage)
{
	mm128_t *z;
	uint64_t *u;
	int64_t i, k, n_z, n_v;
	int32_t n_u;

	*n_u_ = *n_v_ = 0;
	for (i = 0, n_z = 0; i < n; ++i) // precompute n_z
		if (f[i] >= min_sc) ++n_z;
	if (n_z == 0) {
		if (runl_experiment_enabled()) runl_measure_first_path(runl_stage, n, 0, 0, f, p, min_sc);
		return 0;
	}
	z = Kmalloc(km, mm128_t, n_z);
	for (i = 0, k = 0; i < n; ++i) // populate z[]
		if (f[i] >= min_sc) z[k].x = f[i], z[k++].y = i;
	radix_sort_128x(z, z + n_z);
	if (runl_experiment_enabled()) {
		runl_sort_equal_score_candidates(z, n_z);
		runl_measure_first_path(runl_stage, n, n_z, z, f, p, min_sc);
	}

	memset(t, 0, n * 4);
	for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // precompute n_u
		if (t[z[k].y] == 0) {
			int64_t n_v0 = n_v, end_i;
			int32_t sc;
			end_i = mg_chain_bk_end(max_drop, z, f, p, t, k);
			for (i = z[k].y; i != end_i; i = p[i])
				++n_v, t[i] = 1;
			sc = i < 0? z[k].x : (int32_t)z[k].x - f[i];
			if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
				++n_u;
			else n_v = n_v0;
		}
	}
	u = Kmalloc(km, uint64_t, n_u);
	memset(t, 0, n * 4);
	for (k = n_z - 1, n_v = n_u = 0; k >= 0; --k) { // populate u[]
		if (t[z[k].y] == 0) {
			int64_t n_v0 = n_v, end_i;
			int32_t sc;
			end_i = mg_chain_bk_end(max_drop, z, f, p, t, k);
			for (i = z[k].y; i != end_i; i = p[i])
				v[n_v++] = i, t[i] = 1;
			sc = i < 0? z[k].x : (int32_t)z[k].x - f[i];
			if (sc >= min_sc && n_v > n_v0 && n_v - n_v0 >= min_cnt)
				u[n_u++] = (uint64_t)sc << 32 | (n_v - n_v0);
			else n_v = n_v0;
		}
	}
	kfree(km, z);
	assert(n_v < INT32_MAX);
	*n_u_ = n_u, *n_v_ = n_v;
	return u;
}

static mm128_t *compact_a(void *km, int32_t n_u, uint64_t *u, int32_t n_v, int32_t *v, mm128_t *a)
{
	mm128_t *b, *w;
	uint64_t *u2;
	int64_t i, j, k;

	// write the result to b[]
	b = Kmalloc(km, mm128_t, n_v);
	for (i = 0, k = 0; i < n_u; ++i) {
		int32_t k0 = k, ni = (int32_t)u[i];
		for (j = 0; j < ni; ++j)
			b[k++] = a[v[k0 + (ni - j - 1)]];
	}
	kfree(km, v);

	// sort u[] and a[] by the target position, such that adjacent chains may be joined
	w = Kmalloc(km, mm128_t, n_u);
	for (i = k = 0; i < n_u; ++i) {
		w[i].x = b[k].x, w[i].y = (uint64_t)k<<32|i;
		k += (int32_t)u[i];
	}
	radix_sort_128x(w, w + n_u);
	u2 = Kmalloc(km, uint64_t, n_u);
	for (i = k = 0; i < n_u; ++i) {
		int32_t j = (int32_t)w[i].y, n = (int32_t)u[j];
		u2[i] = u[j];
		memcpy(&a[k], &b[w[i].y>>32], n * sizeof(mm128_t));
		k += n;
	}
	memcpy(u, u2, n_u * 8);
	memcpy(b, a, k * sizeof(mm128_t)); // write _a_ to _b_ and deallocate _a_ because _a_ is oversized, sometimes a lot
	kfree(km, a); kfree(km, w); kfree(km, u2);
	return b;
}

static inline int32_t comput_sc(const mm128_t *ai, const mm128_t *aj, int32_t max_dist_x, int32_t max_dist_y, int32_t bw, float chn_pen_gap, float chn_pen_skip, int is_cdna, int n_seg)
{
	int32_t dq = (int32_t)ai->y - (int32_t)aj->y, dr, dd, dg, q_span, sc;
	int32_t sidi = (ai->y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT;
	int32_t sidj = (aj->y & MM_SEED_SEG_MASK) >> MM_SEED_SEG_SHIFT;
	if (dq <= 0 || dq > max_dist_x) return INT32_MIN;
	dr = (int32_t)(ai->x - aj->x);
	if (sidi == sidj && (dr == 0 || dq > max_dist_y)) return INT32_MIN;
	dd = dr > dq? dr - dq : dq - dr;
	if (sidi == sidj && dd > bw) return INT32_MIN;
	if (n_seg > 1 && !is_cdna && sidi == sidj && dr > max_dist_y) return INT32_MIN;
	dg = dr < dq? dr : dq;
	q_span = aj->y>>32&0xff;
	sc = q_span < dg? q_span : dg;
	if (dd || dg > q_span) {
		float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		if (is_cdna || sidi != sidj) {
			if (sidi != sidj && dr == 0) ++sc; // possibly due to overlapping paired ends; give a minor bonus
			else if (dr > dq || sidi != sidj) sc -= (int)(lin_pen < log_pen? lin_pen : log_pen); // deletion or jump between paired ends
			else sc -= (int)(lin_pen + .5f * log_pen);
		} else sc -= (int)(lin_pen + .5f * log_pen);
	}
	return sc;
}

/* Input:
 *   a[].x: rev<<63 | tid<<32 | tpos
 *   a[].y: flags<<40 | q_span<<32 | q_pos
 * Output:
 *   n_u: #chains
 *   u[]: score<<32 | #anchors (sum of lower 32 bits of u[] is the returned length of a[])
 * input a[] is deallocated on return
 */
mm128_t *mg_lchain_dp(int max_dist_x, int max_dist_y, int bw, int max_skip, int max_iter, int min_cnt, int min_sc, float chn_pen_gap, float chn_pen_skip,
					  int is_cdna, int n_seg, int64_t n, mm128_t *a, int *n_u_, uint64_t **_u, void *km)
{ // TODO: make sure this works when n has more than 32 bits
	int32_t *f, *t, *v, n_u, n_v, mmax_f = 0, max_drop = bw;
	int64_t *p, i, j, max_ii, st = 0;
	uint64_t *u;

	if (_u) *_u = 0, *n_u_ = 0;
	if (n == 0 || a == 0) {
		kfree(km, a);
		return 0;
	}
	if (max_dist_x < bw) max_dist_x = bw;
	if (max_dist_y < bw && !is_cdna) max_dist_y = bw;
	if (is_cdna) max_drop = INT32_MAX;
	p = Kmalloc(km, int64_t, n);
	f = Kmalloc(km, int32_t, n);
	v = Kmalloc(km, int32_t, n);
	t = Kcalloc(km, int32_t, n);

	// fill the score and backtrack arrays
	for (i = 0, max_ii = -1; i < n; ++i) {
		int64_t max_j = -1, end_j;
		int32_t max_f = a[i].y>>32&0xff, n_skip = 0;
		while (st < i && (a[i].x>>32 != a[st].x>>32 || a[i].x > a[st].x + max_dist_x)) ++st;
		if (i - st > max_iter) st = i - max_iter;
		for (j = i - 1; j >= st; --j) {
			int32_t sc;
			sc = comput_sc(&a[i], &a[j], max_dist_x, max_dist_y, bw, chn_pen_gap, chn_pen_skip, is_cdna, n_seg);
			if (sc == INT32_MIN) continue;
			sc += f[j];
			if (sc > max_f) {
				max_f = sc, max_j = j;
				if (n_skip > 0) --n_skip;
			} else if (t[j] == (int32_t)i) {
				if (++n_skip > max_skip)
					break;
			}
			if (p[j] >= 0) t[p[j]] = i;
		}
		end_j = j;
		if (max_ii < 0 || a[i].x - a[max_ii].x > (int64_t)max_dist_x) {
			int32_t max = INT32_MIN;
			max_ii = -1;
			for (j = i - 1; j >= st; --j)
				if (max < f[j]) max = f[j], max_ii = j;
		}
		if (max_ii >= 0 && max_ii < end_j) {
			int32_t tmp;
			tmp = comput_sc(&a[i], &a[max_ii], max_dist_x, max_dist_y, bw, chn_pen_gap, chn_pen_skip, is_cdna, n_seg);
			if (tmp != INT32_MIN && max_f < tmp + f[max_ii])
				max_f = tmp + f[max_ii], max_j = max_ii;
		}
		f[i] = max_f, p[i] = max_j;
		v[i] = max_j >= 0 && v[max_j] > max_f? v[max_j] : max_f; // v[] keeps the peak score up to i; f[] is the score ending at i, not always the peak
		if (max_ii < 0 || (a[i].x - a[max_ii].x <= (int64_t)max_dist_x && f[max_ii] < f[i]))
			max_ii = i;
		if (mmax_f < max_f) mmax_f = max_f;
		//fprintf(stderr, "X1\t%ld\t%ld:%d\t%ld\t%ld:%d\t%ld\t%ld\n", (long)i, (long)(a[i].x>>32), (int32_t)a[i].x, (long)max_j, max_j<0?-1L:(long)(a[max_j].x>>32), max_j<0?-1:(int32_t)a[max_j].x, (long)max_f, (long)v[i]);
	}

	u = mg_chain_backtrack(km, n, f, p, v, t, min_cnt, min_sc, max_drop, &n_u, &n_v, "dp");
	*n_u_ = n_u, *_u = u; // NB: note that u[] may not be sorted by score here
	kfree(km, p); kfree(km, f); kfree(km, t);
	if (n_u == 0) {
		kfree(km, a); kfree(km, v);
		return 0;
	}
	return compact_a(km, n_u, u, n_v, v, a);
}

typedef struct lc_elem_s {
	int32_t y;
	int64_t i;
	double pri;
	KRMQ_HEAD(struct lc_elem_s) head;
} lc_elem_t;

#define lc_elem_cmp(a, b) ((a)->y < (b)->y? -1 : (a)->y > (b)->y? 1 : ((a)->i > (b)->i) - ((a)->i < (b)->i))
#define lc_elem_lt2(a, b) ((a)->pri < (b)->pri)
KRMQ_INIT(lc_elem, lc_elem_t, head, lc_elem_cmp, lc_elem_lt2)

KALLOC_POOL_INIT(rmq, lc_elem_t)

static inline int32_t comput_sc_simple(const mm128_t *ai, const mm128_t *aj, float chn_pen_gap, float chn_pen_skip, int32_t *exact, int32_t *width)
{
	int32_t dq = (int32_t)ai->y - (int32_t)aj->y, dr, dd, dg, q_span, sc;
	dr = (int32_t)(ai->x - aj->x);
	*width = dd = dr > dq? dr - dq : dq - dr;
	dg = dr < dq? dr : dq;
	q_span = aj->y>>32&0xff;
	sc = q_span < dg? q_span : dg;
	if (exact) *exact = (dd == 0 && dg <= q_span);
	if (dd || dq > q_span) {
		float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		sc -= (int)(lin_pen + .5f * log_pen);
	}
	return sc;
}

mm128_t *mg_lchain_rmq(int max_dist, int max_dist_inner, int bw, int max_chn_skip, int cap_rmq_size, int min_cnt, int min_sc, float chn_pen_gap, float chn_pen_skip,
					   int64_t n, mm128_t *a, int *n_u_, uint64_t **_u, void *km)
{
	int32_t *f,*t, *v, n_u, n_v, mmax_f = 0, max_rmq_size = 0, max_drop = bw;
	int64_t *p, i, i0, st = 0, st_inner = 0;
	uint64_t *u;
	lc_elem_t *root = 0, *root_inner = 0;
	void *mem_mp = 0;
	kmp_rmq_t *mp;

	if (_u) *_u = 0, *n_u_ = 0;
	if (n == 0 || a == 0) {
		kfree(km, a);
		return 0;
	}
	if (max_dist < bw) max_dist = bw;
	if (max_dist_inner < 0) max_dist_inner = 0;
	if (max_dist_inner > max_dist) max_dist_inner = max_dist;
	p = Kmalloc(km, int64_t, n);
	f = Kmalloc(km, int32_t, n);
	t = Kcalloc(km, int32_t, n);
	v = Kmalloc(km, int32_t, n);
	mem_mp = km_init2(km, 0x10000);
	mp = kmp_init_rmq(mem_mp);

	// fill the score and backtrack arrays
	for (i = i0 = 0; i < n; ++i) {
		int64_t max_j = -1;
		int32_t q_span = a[i].y>>32&0xff, max_f = q_span;
		lc_elem_t s, *q, *r, lo, hi;
		// add in-range anchors
		if (i0 < i && a[i0].x != a[i].x) {
			int64_t j;
			for (j = i0; j < i; ++j) {
				q = kmp_alloc_rmq(mp);
				q->y = (int32_t)a[j].y, q->i = j, q->pri = -(f[j] + 0.5 * chn_pen_gap * ((int32_t)a[j].x + (int32_t)a[j].y));
				krmq_insert(lc_elem, &root, q, 0);
				if (max_dist_inner > 0) {
					r = kmp_alloc_rmq(mp);
					*r = *q;
					krmq_insert(lc_elem, &root_inner, r, 0);
				}
			}
			i0 = i;
		}
		// get rid of active chains out of range
		while (st < i && (a[i].x>>32 != a[st].x>>32 || a[i].x > a[st].x + max_dist || krmq_size(head, root) > cap_rmq_size)) {
			s.y = (int32_t)a[st].y, s.i = st;
			if ((q = krmq_find(lc_elem, root, &s, 0)) != 0) {
				q = krmq_erase(lc_elem, &root, q, 0);
				kmp_free_rmq(mp, q);
			}
			++st;
		}
		if (max_dist_inner > 0)  { // similar to the block above, but applied to the inner tree
			while (st_inner < i && (a[i].x>>32 != a[st_inner].x>>32 || a[i].x > a[st_inner].x + max_dist_inner || krmq_size(head, root_inner) > cap_rmq_size)) {
				s.y = (int32_t)a[st_inner].y, s.i = st_inner;
				if ((q = krmq_find(lc_elem, root_inner, &s, 0)) != 0) {
					q = krmq_erase(lc_elem, &root_inner, q, 0);
					kmp_free_rmq(mp, q);
				}
				++st_inner;
			}
		}
		// RMQ
		lo.i = INT32_MAX, lo.y = (int32_t)a[i].y - max_dist;
		hi.i = 0, hi.y = (int32_t)a[i].y;
		if ((q = krmq_rmq(lc_elem, root, &lo, &hi)) != 0) {
			int32_t sc, exact, width, n_skip = 0;
			int64_t j = q->i;
			assert(q->y >= lo.y && q->y <= hi.y);
			sc = f[j] + comput_sc_simple(&a[i], &a[j], chn_pen_gap, chn_pen_skip, &exact, &width);
			if (width <= bw && sc > max_f) max_f = sc, max_j = j;
			if (!exact && root_inner && (int32_t)a[i].y > 0) {
				lc_elem_t *lo, *hi;
				s.y = (int32_t)a[i].y - 1, s.i = n;
				krmq_interval(lc_elem, root_inner, &s, &lo, &hi);
				if (lo) {
					const lc_elem_t *q;
					int32_t width;
					krmq_itr_t(lc_elem) itr;
					krmq_itr_find(lc_elem, root_inner, lo, &itr);
					while ((q = krmq_at(&itr)) != 0) {
						if (q->y < (int32_t)a[i].y - max_dist_inner) break;
						j = q->i;
						sc = f[j] + comput_sc_simple(&a[i], &a[j], chn_pen_gap, chn_pen_skip, 0, &width);
						if (width <= bw) {
							if (sc > max_f) {
								max_f = sc, max_j = j;
								if (n_skip > 0) --n_skip;
							} else if (t[j] == (int32_t)i) {
								if (++n_skip > max_chn_skip)
									break;
							}
							if (p[j] >= 0) t[p[j]] = i;
						}
						if (!krmq_itr_prev(lc_elem, &itr)) break;
					}
				}
			}
		}
		// set max
		assert(max_j < 0 || (a[max_j].x < a[i].x && (int32_t)a[max_j].y < (int32_t)a[i].y));
		f[i] = max_f, p[i] = max_j;
		v[i] = max_j >= 0 && v[max_j] > max_f? v[max_j] : max_f; // v[] keeps the peak score up to i; f[] is the score ending at i, not always the peak
		if (mmax_f < max_f) mmax_f = max_f;
		if (max_rmq_size < krmq_size(head, root)) max_rmq_size = krmq_size(head, root);
	}
	km_destroy(mem_mp);

	u = mg_chain_backtrack(km, n, f, p, v, t, min_cnt, min_sc, max_drop, &n_u, &n_v, "rmq");
	*n_u_ = n_u, *_u = u; // NB: note that u[] may not be sorted by score here
	kfree(km, p); kfree(km, f); kfree(km, t);
	if (n_u == 0) {
		kfree(km, a); kfree(km, v);
		return 0;
	}
	return compact_a(km, n_u, u, n_v, v, a);
}
