#include "kbtree_CUDA.cuh"
#include "gmem_alloc.h"
__device__ kbtree_chn_t *kb_init_chn(int size, void* CUDAKernel_buffer)		
{																	
	kbtree_chn_t *b;											
	b = (kbtree_chn_t*)CUDAKernelCalloc(CUDAKernel_buffer, 1, sizeof(kbtree_chn_t), 8);
	b->t = ((size - 4 - sizeof(void*)) / (sizeof(void*) + sizeof(mem_chain_t)) + 1) >> 1; 
	if (b->t < 2) {
		return 0;
	}

	b->n = 2 * b->t - 1;
	b->off_ptr = 8 + b->n * sizeof(mem_chain_t);
	b->ilen = (8 + sizeof(void*) + b->n * (sizeof(void*) + sizeof(mem_chain_t)) + 3) >> 2 << 2;
	b->elen = (b->off_ptr + 3) >> 2 << 2;
	b->root = (kbnode_t*)CUDAKernelCalloc(CUDAKernel_buffer, 1, b->ilen, 8);
	++b->n_nodes;													
	return b;														
}


__device__ static inline int __kb_getp_aux_chn(const kbnode_t * __restrict x, const mem_chain_t * __restrict k, int *r) 
{									
	mem_chain_t a, b;								
	int tr, *rr, begin = 0, end = x->n;								
	if (x->n == 0) return -1;										
	rr = r? r : &tr;												
	while (begin < end) {											
		int mid = (begin + end) >> 1;
		a = __KB_KEY(x)[mid];
		b = *k; 								
		if (chain_cmp(a, b) < 0)
			begin = mid + 1; 
		else end = mid;												
	}																
	if (begin == x->n) { *rr = 1; return x->n - 1; }				
	a = *k;
	b = __KB_KEY(x)[begin];
	if ((*rr = chain_cmp(a, b)) < 0) --begin;	
	return begin;													
}

/* find the closest chain to k in tree b, return interval to lower and upper */
__device__ void kb_intervalp_chn(kbtree_chn_t *b, const mem_chain_t * __restrict k, mem_chain_t **lower, mem_chain_t **upper)	
{																	
	int i, r = 0;													
	kbnode_t *x = b->root;											
	*lower = *upper = 0;											
	while (x) {														
		i = __kb_getp_aux_chn(x, k, &r);							
		if (i >= 0 && r == 0) {										
			*lower = *upper = &__KB_KEY(x)[i];				
			return;													
		}															
		if (i >= 0) *lower = &__KB_KEY(x)[i];				
		if (i < x->n - 1) *upper = &__KB_KEY(x)[i + 1];		
		if (x->is_internal == 0) return;							
		x = __KB_PTR(b, x)[i + 1];									
	}																
}																	


__device__ static void __kb_split_chn(kbtree_chn_t *b, kbnode_t *x, int i, kbnode_t *y, void* CUDAKernel_buffer) 
{																	
	kbnode_t *z;													
	z = (kbnode_t*)CUDAKernelCalloc(CUDAKernel_buffer, 1, y->is_internal? b->ilen : b->elen, 8);	
	++b->n_nodes;													
	z->is_internal = y->is_internal;								
	z->n = b->t - 1;												
	cudaKernelMemcpy(__KB_KEY(y) + b->t, __KB_KEY(z), sizeof(mem_chain_t) * (b->t - 1)); 
	if (y->is_internal) cudaKernelMemcpy(__KB_PTR(b, y) + b->t, __KB_PTR(b, z), sizeof(void*) * b->t); 
	y->n = b->t - 1;												
	cudaKernelMemmove(__KB_PTR(b, x) + i + 1, __KB_PTR(b, x) + i + 2, sizeof(void*) * (x->n - i)); 
	__KB_PTR(b, x)[i + 1] = z;										
	cudaKernelMemmove(__KB_KEY(x) + i, __KB_KEY(x) + i + 1, sizeof(mem_chain_t) * (x->n - i)); 
	__KB_KEY(x)[i] = __KB_KEY(y)[b->t - 1];			
	++x->n;															
}																	

// TODO: CHECK MEMMOVE IF RESULT IS STRANGE
__device__ static void __kb_putp_aux_chn(kbtree_chn_t *b, kbnode_t *x, const mem_chain_t * __restrict k, void* CUDAKernel_buffer) 
{																	
	int i = x->n - 1;												
	if (x->is_internal == 0) {										
		i = __kb_getp_aux_chn(x, k, 0);							
		if (i != x->n - 1)											
			cudaKernelMemmove(__KB_KEY(x) + i + 1, __KB_KEY(x) + i + 2, (x->n - i - 1) * sizeof(mem_chain_t)); 
		__KB_KEY(x)[i + 1] = *k;								
		++x->n;														
	} else {														
		i = __kb_getp_aux_chn(x, k, 0) + 1;						
		if (__KB_PTR(b, x)[i]->n == 2 * b->t - 1) {					
			__kb_split_chn(b, x, i, __KB_PTR(b, x)[i], CUDAKernel_buffer);			
			if (chain_cmp(*k, __KB_KEY(x)[i]) > 0) ++i;			
		}															
		__kb_putp_aux_chn(b, __KB_PTR(b, x)[i], k, CUDAKernel_buffer);				
	}																
}

__device__ void kb_putp_chn(kbtree_chn_t *b, const mem_chain_t * __restrict k, void* CUDAKernel_buffer) 
{																	
	kbnode_t *r, *s;												
	++b->n_keys;													
	r = b->root;													
	if (r->n == 2 * b->t - 1) {										
		++b->n_nodes;												
		s = (kbnode_t*)CUDAKernelCalloc(CUDAKernel_buffer, 1, b->ilen, 8);							
		b->root = s; s->is_internal = 1; s->n = 0;					
		__KB_PTR(b, s)[0] = r;										
		__kb_split_chn(b, s, 0, r, CUDAKernel_buffer);								
		r = s;														
	}																
	__kb_putp_aux_chn(b, r, k, CUDAKernel_buffer);									
}																	

typedef struct {
	kbnode_t *x;
	int i;
} __kbstack_t;

__device__ void __kb_traverse(kbtree_chn_t* b, mem_chain_v* chain, void* CUDAKernel_buffer){
	int __kmax = 8;
	__kbstack_t *__kstack, *__kp;
	__kp = __kstack = (__kbstack_t*)CUDAKernelCalloc(CUDAKernel_buffer, __kmax, sizeof(__kbstack_t), 8);
	__kp->x = b->root; __kp->i = 0;
	for (;;) {
		while (__kp->x && __kp->i <= __kp->x->n) {
			if (__kp - __kstack == __kmax - 1) {
				__kmax <<= 1;
				__kstack = (__kbstack_t*)CUDAKernelRealloc(CUDAKernel_buffer, __kstack, __kmax * sizeof(__kbstack_t), 8);
				__kp = __kstack + (__kmax>>1) - 1;
			}
			(__kp+1)->i = 0; (__kp+1)->x = __kp->x->is_internal? __KB_PTR(b, __kp->x)[__kp->i] : 0;
			++__kp;
		}
		--__kp;
		if (__kp >= __kstack) {
			if (__kp->x && __kp->i < __kp->x->n) 
				chain->a[chain->n++] = __KB_KEY(__kp->x)[__kp->i];
			++__kp->i;
		} else break;
	}
	// free(__kstack);
}
