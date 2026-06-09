#include "krmq_cuda.h"
#include <thrust/host_vector.h>
#include <thrust/device_vector.h>
#include <thrust/sort.h>

__device__ 
lc_elem_t* krmq_find(const lc_elem_t* root, const lc_elem_t* x, unsigned* cnt) {
    const lc_elem_t* p = root;
    unsigned count = 0;
    while (p != nullptr) {
        int cmp = lc_elem_cmp(x, p);
        if (cmp >= 0) count += krmq_size_child(p, 0) + 1;
        if (cmp < 0) p = p->head.p[0];
        else if (cmp > 0) p = p->head.p[1];
        else break;
    }
    if (cnt) *cnt = count;
    return const_cast<lc_elem_t*>(p);
}


__device__ 
lc_elem_t* krmq_interval(const lc_elem_t* root, const lc_elem_t* x, lc_elem_t** lower, lc_elem_t** upper) {
    const lc_elem_t* p = root;
    const lc_elem_t* l = nullptr;
    const lc_elem_t* u = nullptr;
    while (p != nullptr) {
        int cmp = lc_elem_cmp(x, p);
        if (cmp < 0) {
            u = p;
            p = p->head.p[0];
        } else if (cmp > 0) {
            l = p;
            p = p->head.p[1];
        } else {
            l = u = p;
            break;
        }
    }
    if (lower) *lower = const_cast<lc_elem_t*>(l);
    if (upper) *upper = const_cast<lc_elem_t*>(u);
    return const_cast<lc_elem_t*>(p);
}

__device__ 
lc_elem_t* krmq_rmq(const lc_elem_t* root, const lc_elem_t* lo, const lc_elem_t* up) {
    const lc_elem_t* p = root;
    const lc_elem_t* path[2][KRMQ_MAX_DEPTH], *min;
    int plen[2] = {0, 0}, pcmp[2][KRMQ_MAX_DEPTH], i, cmp,lca;
    
    if (root == nullptr) return nullptr;
    // Traverse towards lo
    while (p) {
        cmp = lc_elem_cmp(lo, p); 
        path[0][plen[0]] = p;
        pcmp[0][plen[0]++] = cmp;
        if (cmp < 0) p = p->head.p[0];
        else if (cmp > 0) p = p->head.p[1];
        else break;
    }
    p = root;
    // Traverse towards up
    while (p) {
        cmp = lc_elem_cmp(up, p); 
        path[1][plen[1]] = p;
        pcmp[1][plen[1]++] = cmp;
        if (cmp < 0) p = p->head.p[0];
        else if (cmp > 0) p = p->head.p[1];
        else break;
    }
    // Find the Lowest Common Ancestor (LCA)
    for (i = 0; i < plen[0] && i < plen[1]; ++i) {
        if (path[0][i] == path[1][i] && pcmp[0][i] <= 0 && pcmp[1][i] >= 0) {
            break;
        }
    }
    if (i == plen[0] || i == plen[1]) return nullptr; // No elements in the closed interval
    lca = i;
    min = path[0][lca];
    // Traverse path[0] beyond LCA
    for (i = lca + 1; i < plen[0]; ++i) {
        if (pcmp[0][i] <= 0) {
            if (lc_elem_lt2(path[0][i], min)) min = path[0][i];
            if (path[0][i]->head.p[1] && lc_elem_lt2(path[0][i]->head.p[1]->head.s, min)) min = path[0][i]->head.p[1]->head.s;
        }
    }
    // Traverse path[1] beyond LCA
    for (i = lca + 1; i < plen[1]; ++i) {
        if (pcmp[1][i] >= 0) {
            if (lc_elem_lt2(path[1][i], min)) min = path[1][i];
            if (path[1][i]->head.p[0] && lc_elem_lt2(path[1][i]->head.p[0]->head.s, min)) min = path[1][i]->head.p[0]->head.s;
        }
    }
    return const_cast<lc_elem_t*>(min);
}
__device__ 
static inline void krmq_update_min(lc_elem_t *p, const lc_elem_t *q, const lc_elem_t *r) {
    p->head.s = (!q || lc_elem_lt2(p, q->head.s)) ? p : q->head.s;
    p->head.s = (!r || lc_elem_lt2(p->head.s, r->head.s)) ? p->head.s : r->head.s;
}
__device__ 
static inline lc_elem_t *krmq_rotate1(lc_elem_t *p, int dir) { // dir=0 to left; dir=1 to right
    int opp = 1 - dir; // opposite direction
    lc_elem_t *q = p->head.p[opp];
    lc_elem_t *s = p->head.s;
    unsigned size_p = p->head.size;

    p->head.size -= q->head.size - krmq_size_child(q, dir);
    q->head.size = size_p;
    krmq_update_min(p, p->head.p[dir], q->head.p[dir]);
    q->head.s = s;
    p->head.p[opp] = q->head.p[dir];
    q->head.p[dir] = p;
    return q;
}
__device__ 
static inline lc_elem_t *krmq_rotate2(lc_elem_t *p, int dir) {
    int b1, opp = 1 - dir;
    lc_elem_t *q = p->head.p[opp];
    lc_elem_t *r = q->head.p[dir];
    lc_elem_t *s = p->head.s;
    unsigned size_x_dir = krmq_size_child(r, dir);

    r->head.size = p->head.size;
    p->head.size -= q->head.size - size_x_dir;
    q->head.size -= size_x_dir + 1;
    krmq_update_min(p, p->head.p[dir], r->head.p[dir]);
    krmq_update_min(q, q->head.p[opp], r->head.p[opp]);
    r->head.s = s;
    p->head.p[opp] = r->head.p[dir];
    r->head.p[dir] = p;
    q->head.p[dir] = r->head.p[opp];
    r->head.p[opp] = q;

    b1 = (dir == 0) ? +1 : -1;
    if (r->head.balance == b1)
        q->head.balance = 0, p->head.balance = -b1;
    else if (r->head.balance == 0)
        q->head.balance = p->head.balance = 0;
    else
        q->head.balance = b1, p->head.balance = 0;
    r->head.balance = 0;
    return r;
}

__device__ 
lc_elem_t* krmq_insert(lc_elem_t** root_, lc_elem_t* x, unsigned* cnt_) {
    unsigned char stack[KRMQ_MAX_DEPTH];
    lc_elem_t* path[KRMQ_MAX_DEPTH];
    lc_elem_t* bp, *bq;
    lc_elem_t* p, *q, *r = nullptr; // r is potentially the new root
    int i, which = 0, top, b1, path_len;
    unsigned cnt = 0;
    
    bp = *root_, bq = nullptr;
    
    // Find the insertion location
    for (p = bp, q = bq, top = path_len = 0; p; q = p, p = p->head.p[which]) {
        int cmp = lc_elem_cmp(x, p); // Assume __cmp is defined somewhere
        
        if (cmp >= 0) cnt += krmq_size_child(p, 0) + 1; // Assuming krmq_size_child function exists
        if (cmp == 0) {
            if (cnt_) *cnt_ = cnt;
            return p;
        }
        
        if (p->head.balance != 0)
            bq = q, bp = p, top = 0;
        
        stack[top++] = which = (cmp > 0);
        path[path_len++] = p;
    }
    if (cnt_) *cnt_ = cnt;
    x->head.balance = 0;
    x->head.size = 1;
    x->head.p[0] = x->head.p[1] = nullptr;
    x->head.s = x;  
    if (q == nullptr) *root_ = x;
    else q->head.p[which] = x;
    if (bp == nullptr) return x;
    for (i = 0; i < path_len; ++i)
        ++path[i]->head.size;
    for (i = path_len - 1; i >= 0; --i) {
        krmq_update_min(path[i], path[i]->head.p[0], path[i]->head.p[1]); 
        if (path[i]->head.s != x)
            break;
    }
    for (p = bp, top = 0; p != x; p = p->head.p[stack[top]], ++top) {
        if (stack[top] == 0)
            --p->head.balance;
        else
            ++p->head.balance;
    }
    if (bp->head.balance > -2 && bp->head.balance < 2)
        return x; // no re-balance needed
    // re-balance
    which = (bp->head.balance < 0);
    b1 = which == 0 ? +1 : -1;
    q = bp->head.p[1 - which];
    if (q->head.balance == b1) {
        r = krmq_rotate1(bp, which); // Assuming krmq_rotate1_lc_elem_t function exists
        q->head.balance = bp->head.balance = 0;
    } else {
        r = krmq_rotate2(bp, which); // Assuming krmq_rotate2_lc_elem_t function exists
    }
    if (bq == nullptr) *root_ = r;
    else bq->head.p[bp != bq->head.p[0]] = r;
    return x;
}

__device__ 
lc_elem_t* krmq_erase(lc_elem_t **root_, const lc_elem_t *x, unsigned *cnt_){
    lc_elem_t *p, *path[KRMQ_MAX_DEPTH], fake;
    unsigned char dir[KRMQ_MAX_DEPTH];
    int i, d = 0, cmp;
    unsigned cnt = 0;

    fake = **root_, fake.head.p[0] = *root_, fake.head.p[1] = 0;
    if (cnt_) *cnt_ = 0;

    if (x) {
        // Find the node to be deleted
        for (cmp = -1, p = &fake; cmp; cmp = lc_elem_cmp(x, p)) {
            int which = (cmp > 0);
            if (cmp > 0)
                cnt += krmq_size_child(p, 0) + 1;
            dir[d] = which;
            path[d++] = p;
            p = p->head.p[which];
            if (p == 0) {
                if (cnt_)
                    *cnt_ = 0;
                return 0; // Node not found
            }
        }
        cnt += krmq_size_child(p, 0) + 1; // Adjust count
    } else {
        // If x is NULL, simply find the rightmost node
        for (p = &fake, cnt = 1; p; p = p->head.p[0]) {
            dir[d] = 0;
            path[d++] = p;
        }
        p = path[--d];
    }
    if (cnt_) *cnt_ = cnt;
    // Adjust sizes of nodes in the path
    for (i = 1; i < d; ++i)
        --path[i]->head.size;
    // Case 1: Node to be deleted has no right child
    if (p->head.p[1] == 0) {
        path[d - 1]->head.p[dir[d - 1]] = p->head.p[0]; // Promote left child
    } else {
        // Case 2: Node to be deleted has a right child
        lc_elem_t *q = p->head.p[1];
        if (q->head.p[0] == 0) {
            // Case 2a: Right child has no left child
            q->head.p[0] = p->head.p[0];
            q->head.balance = p->head.balance;
            path[d - 1]->head.p[dir[d - 1]] = q;
            path[d] = q;
            dir[d++] = 1;
            q->head.size = p->head.size - 1;
        } else {
            // Case 2b: Right child has a left child
            lc_elem_t *r;
            int e = d++;
            // Find the right child's leftmost descendant
            for (;;) {
                dir[d] = 0;
                path[d++] = q;
                r = q->head.p[0];
                if (r->head.p[0] == 0)
                    break;
                q = r;
            }
            // Reorganize the tree structure
            r->head.p[0] = p->head.p[0];
            q->head.p[0] = r->head.p[1];
            r->head.p[1] = p->head.p[1];
            r->head.balance = p->head.balance;
            path[e - 1]->head.p[dir[e - 1]] = r;
            path[e] = r;
            dir[e] = 1;
            // Adjust sizes of nodes in the path
            for (i = e + 1; i < d; ++i)
                --path[i]->head.size;

            r->head.size = p->head.size - 1;
        }
    }

    // Update minimum values in the path
    for (i = d - 1; i >= 0; --i)
        krmq_update_min(path[i], path[i]->head.p[0], path[i]->head.p[1]);

    // Rebalance the tree if necessary
    while (--d > 0) {
        lc_elem_t *q = path[d];
        int which, other, b1 = 1, b2 = 2;
        which = dir[d];
        other = 1 - which;
        if (which)
            b1 = -b1, b2 = -b2;
        q->head.balance += b1;
        if (q->head.balance == b1)
            break;
        else if (q->head.balance == b2) {
            lc_elem_t *r = q->head.p[other];

            if (r->head.balance == -b1) {
                path[d - 1]->head.p[dir[d - 1]] = krmq_rotate2(q, which);
            } else {
                path[d - 1]->head.p[dir[d - 1]] = krmq_rotate1(q, which);

                if (r->head.balance == 0) {
                    r->head.balance = -b1;
                    q->head.balance = b1;
                    break;
                } else
                    r->head.balance = q->head.balance = 0;
            }
        }
    }
    *root_ = fake.head.p[0]; // Update root
    return p; // Return the deleted node
}

__device__ // Initialize iterator to the first element in the tree
static void krmq_itr_first(lc_elem_t *root, krmq_itr_t *itr) {
    lc_elem_t *p;
    for (itr->top = itr->stack - 1, p = root; p; p = p->head.p[0])
        *++itr->top = p;
}

__device__ // Find a specific element in the tree
static int krmq_itr_find(lc_elem_t *root, lc_elem_t *x, krmq_itr_t *itr) {
    lc_elem_t *p = root;
    itr->top = itr->stack - 1;
    while (p != 0) {
        int cmp;
        *++itr->top = p;
        cmp = lc_elem_cmp(x, p);
        if (cmp < 0) p = p->head.p[0];
        else if (cmp > 0) p = p->head.p[1];
        else break;
    }
    return p ? 1 : 0;
}

__device__ // Move the iterator to the next element in the specified direction (0 = left, 1 = right)
static int krmq_itr_next_bidir(krmq_itr_t *itr, int dir) {
    lc_elem_t *p;
    if (itr->top < itr->stack) return 0;
    dir = !!dir;
    p = (*itr->top)->head.p[dir];
    if (p) { /* go down */
        for (; p; p = p->head.p[!dir])
            *++itr->top = p;
        return 1;
    } else { /* go up */
        do {
            p = *itr->top--;
        } while (itr->top >= itr->stack && p == (*itr->top)->head.p[dir]);
        return itr->top < itr->stack ? 0 : 1;
    }
}






// RMQ chaining

__device__
static inline float mg_log2_device(float x) // NB: this doesn't work when x<2
{
	union { float f; uint32_t i; } z = { x };
	float log_2 = ((z.i >> 23) & 255) - 128;
	z.i &= ~(255 << 23);
	z.i += 127 << 23;
	log_2 += (-0.34484843f * z.f + 2.02466578f) * z.f - 0.67487759f;
	return log_2;
}
// simpler linear chain score function 
__device__
int32_t comput_sc_simple(const uint64_t aix, const uint64_t aiy, const uint64_t ajx, const uint64_t ajy, float chn_pen_gap, float chn_pen_skip, int32_t *exact, int32_t *width)
{
	int32_t dq = (int32_t)aiy - (int32_t)ajy, dr, dd, dg, q_span, sc;
	dr = (int32_t)(aix - ajx);
	*width = dd = dr > dq? dr - dq : dq - dr;
	dg = dr < dq? dr : dq;
	q_span = ajy>>32&0xff;
	sc = q_span < dg? q_span : dg;
	if (exact) *exact = (dd == 0 && dg <= q_span);
	if (dd || dq > q_span) {
		float lin_pen, log_pen;
		lin_pen = chn_pen_gap * (float)dd + chn_pen_skip * (float)dg;
		log_pen = dd >= 1? mg_log2_device(dd + 1) : 0.0f; // mg_log2() only works for dd>=2
		sc -= (int)(lin_pen + .5f * log_pen);
	}
	return sc;
}


// __global__
// void gpu_chain_rmq(int max_dist, int max_dist_inner, int bw, int max_chn_skip, int cap_rmq_size, int min_cnt, int min_sc, float chn_pen_gap, float chn_pen_skip,
// 					   int* g_n, uint64_t* g_ax, uint64_t* g_ay, int* g_ofs, int32_t* g_f, int64_t* g_p, int n_task, lc_elem_t* rmq_nodes, int max_tree_size)
// {
// 	int32_t *t, *v, n_u, n_v, mmax_f = 0, max_rmq_size = 0, max_drop = bw;
// 	int64_t i, i0, st = 0, st_inner = 0;
// 	uint64_t *u;
// 	lc_elem_t *root = 0, *root_inner = 0;

//     // initialize rmq tree

//     int id = threadIdx.x + blockIdx.x * blockDim.x;
//     if(id >= n_task) return;

//     int ofs = g_ofs[id];
//     int n = g_n[id];
//     if (n == 0) { // no chain 
// 		return;
// 	}
//     if(id==7)printf("[%d] launched rmq kernel %d\n", id, n);

//     uint64_t* ax = &g_ax[ofs * MEM_FACTOR];
//     uint64_t* ay = &g_ay[ofs * MEM_FACTOR];
//     int32_t* f = &g_f[ofs * MEM_FACTOR];
//     int64_t* p = &g_p[ofs * MEM_FACTOR];
//     lc_elem_t* rmq_base = &rmq_nodes[id * max_tree_size];
//     int rmq_idx = 0;
//     //if(id==301)printf("start rmq chaining, rmq base: %d\n", rmq_base->i);

//     thrust::sort_by_key(thrust::seq, ax, ax + n, ay);

// 	if (max_dist < bw) max_dist = bw;
// 	if (max_dist_inner <= 0 || max_dist_inner >= max_dist) max_dist_inner = 0;

// 	// fill the score and backtrack arrays
// 	for (i = i0 = 0; i < n; ++i) {
//         //if(id==301) printf("[%d]rmq loop: %lu\n", id, i);
// 		int64_t max_j = -1;
// 		int32_t q_span = ay[i]>>32&0xff, max_f = q_span;
// 		lc_elem_t s, *q, *r, lo, hi;
// 		// add in-range anchors
// 		if (i0 < i && ax[i0] != ax[i]) {
// 			int64_t j;
// 			for (j = i0; j < i; ++j) {
// 				q = rmq_base + rmq_idx++; 
//                 assert(rmq_idx < max_tree_size);
// 				q->y = (int32_t)ay[j], q->i = j, q->pri = -(f[j] + 0.5 * chn_pen_gap * ((int32_t)ax[j] + (int32_t)ay[j]));
//                 krmq_insert(&root, q, 0);
// 				if (max_dist_inner > 0) {
// 					//r = kmp_alloc_rmq(mp);
//                     r = rmq_base + rmq_idx++; 
// 					*r = *q;
// 					krmq_insert(&root_inner, r, 0);
// 				}
// 			}
// 			i0 = i;
// 		}
// 		// get rid of active chains out of range
// 		while (st < i && (ax[i]>>32 != ax[st]>>32 || ax[i] > ax[st] + max_dist || krmq_size(root) > cap_rmq_size)) {
// 			s.y = (int32_t)ay[st], s.i = st;
// 			if ((q = krmq_find(root, &s, 0)) != 0) {
// 				q = krmq_erase(&root, q, 0);
// 			}
// 			++st;
// 		}
// 		if (max_dist_inner > 0)  { // similar to the block above, but applied to the inner tree
// 			while (st_inner < i && (ax[i]>>32 != ax[st_inner]>>32 || ax[i] > ax[st_inner] + max_dist_inner || krmq_size(root_inner) > cap_rmq_size)) {
// 				s.y = (int32_t)ay[st_inner], s.i = st_inner;
// 				if ((q = krmq_find(root_inner, &s, 0)) != 0) {
// 					q = krmq_erase(&root_inner, q, 0);
// 				}
// 				++st_inner;
// 			}
// 		}
// 		// RMQ
// 		lo.i = INT32_MAX, lo.y = (int32_t)ay[i] - max_dist;
// 		hi.i = 0, hi.y = (int32_t)ay[i];
// 		if ((q = krmq_rmq(root, &lo, &hi)) != 0) {
// 			int32_t sc, exact, width, n_skip = 0;
// 			int64_t j = q->i;
// 			assert(q->y >= lo.y && q->y <= hi.y);
// 			sc = f[j] + comput_sc_simple(ax[i],ay[i], ax[j], ay[j], chn_pen_gap, chn_pen_skip, &exact, &width);
// 			if (width <= bw && sc > max_f) { 
//                 max_f = sc, max_j = j;
//             //     if(id==212)printf("update [%d], parent: %lu, max_f: %d, sc:%d \n", i, max_j, max_f, max_f-f[j]);
//             //     if(id==212)printf("ax:%lu\tay:%lu\n", ax[i], ay[i]);
//             }
// 			// if (!exact && root_inner && (int32_t)ay[i] > 0) {
// 			// 	lc_elem_t *lo, *hi;
// 			// 	s.y = (int32_t)ay[i] - 1, s.i = n;
// 			// 	krmq_interval(root_inner, &s, &lo, &hi);
// 			// 	if (lo) {
// 			// 		const lc_elem_t *q;
// 			// 		int32_t width, n_rmq_iter = 0;
// 			// 		krmq_itr_t itr;
// 			// 		krmq_itr_find(root_inner, lo, &itr);
// 			// 		while ((q = krmq_at(&itr)) != 0) {
// 			// 			if (q->y < (int32_t)ay[i] - max_dist_inner) break;
// 			// 			++n_rmq_iter;
// 			// 			j = q->i;
// 			// 			sc = f[j] + comput_sc_simple(ax[i], ay[i], ax[j], ay[j], chn_pen_gap, chn_pen_skip, 0, &width);
// 			// 			if (width <= bw) {
// 			// 				if (sc > max_f) {
// 			// 					max_f = sc, max_j = j; 
// 			// 					if (n_skip > 0) --n_skip;
// 			// 				} else if (t[j] == (int32_t)i) {
// 			// 					if (++n_skip > max_chn_skip)
// 			// 						break;
// 			// 				}
// 			// 				if (p[j] >= 0) t[p[j]] = i;
// 			// 			}
// 			// 			if (!krmq_itr_next_bidir(&itr, 0)) break;
// 			// 		}
// 			// 	}
// 			// }
// 		}
// 		// set max
// 		assert(max_j < 0 || (ax[max_j] < ax[i] && (int32_t)ay[max_j] < (int32_t)ay[i])); // Why? backtrack?
// 		f[i] = max_f, p[i] = max_j;
// 		// v[i] = max_j >= 0 && v[max_j] > max_f? v[max_j] : max_f; // v[] keeps the peak score up to i; f[] is the score ending at i, not always the peak
// 		if (mmax_f < max_f) mmax_f = max_f;
// 		if (max_rmq_size < krmq_size(root)) max_rmq_size = krmq_size(root);
// 	}

// }



