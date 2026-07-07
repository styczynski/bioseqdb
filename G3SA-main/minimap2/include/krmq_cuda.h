#ifndef KRMQ_CUDA_H
#define KRMQ_CUDA_H

// #ifndef __host__
// #define __host__
// #endif

// #ifndef __device__
// #define __device__
// #endif
#ifdef __CUDACC__
// #include <cuda_runtime.h>

#include "minimap.h"
#include "common.h"
#include <assert.h>


#define KRMQ_MAX_DEPTH 64

template <typename T>        
struct krmq_node{
    T *p[2], *s;
    signed char balance; /* balance factor */
    unsigned size; /* #elements in subtree */ 
};

typedef struct lc_elem_s {
	int32_t y;
	int64_t i;
	double pri;
	krmq_node<lc_elem_s> head;
} lc_elem_t;


template <typename T>
struct krmq_itr_s {
    T** top;  // Pointer to the current position in the stack
    T* stack[KRMQ_MAX_DEPTH];  // Base of the stack
};

using krmq_itr_t = krmq_itr_s <lc_elem_t>;

#define lc_elem_cmp(a, b) ((a)->y < (b)->y? -1 : (a)->y > (b)->y? 1 : ((a)->i > (b)->i) - ((a)->i < (b)->i))
#define lc_elem_lt2(a, b) ((a)->pri < (b)->pri)

#define krmq_size(p) ((p)? (p)->head.size : 0)
#define krmq_size_child(q, i) ((q)->head.p[(i)]? (q)->head.p[(i)]->head.size : 0)
#define krmq_at(itr) ((itr)->top < (itr)->stack? 0 : *(itr)->top)


__device__ lc_elem_t* krmq_find(const lc_elem_t* root, const lc_elem_t* x, unsigned* cnt);
__device__ lc_elem_t* krmq_interval(const lc_elem_t* root, const lc_elem_t* x, lc_elem_t** lower, lc_elem_t** upper);
__device__ lc_elem_t* krmq_rmq(const lc_elem_t* root, const lc_elem_t* lo, const lc_elem_t* up);
__device__ static inline void krmq_update_min(lc_elem_t *p, const lc_elem_t *q, const lc_elem_t *r);
__device__ static inline lc_elem_t *krmq_rotate1(lc_elem_t *p, int dir);
__device__ static inline lc_elem_t *krmq_rotate2(lc_elem_t *p, int dir);
__device__ lc_elem_t* krmq_insert(lc_elem_t** root_, lc_elem_t* x, unsigned* cnt_);
__device__ static void krmq_itr_first(lc_elem_t *root, krmq_itr_t *itr);
__device__ static int krmq_itr_find(lc_elem_t *root, lc_elem_t *x, krmq_itr_t *itr);
__device__ static int krmq_itr_next_bidir(krmq_itr_t *itr, int dir);
__device__ lc_elem_t* krmq_erase(lc_elem_t **root_, const lc_elem_t *x, unsigned *cnt_);

__global__
void gpu_chain_rmq(int max_dist, int max_dist_inner, int bw, int max_chn_skip, int cap_rmq_size, int min_cnt, int min_sc, float chn_pen_gap, float chn_pen_skip,
					   int* g_n, uint64_t* g_ax, uint64_t* g_ay, int* g_ofs, int32_t* g_f, int64_t* g_p, int n_task, lc_elem_t* rmq_nodes, int max_tree_size);

#endif // __CUDACC__
#endif // KRMQ_CUDA_H