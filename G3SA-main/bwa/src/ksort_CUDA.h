typedef struct {
	void *left, *right;
	int depth;
} ks_isort_stack_t;

#define __sort_lt(a, b) ((a).info < (b).info)

__device__ static inline void __ks_insertsort(bwtintv_t *s, bwtintv_t *t)		
{																	
	bwtintv_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && __sort_lt(*j, *(j-1)); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}

__device__ void ks_combsort(size_t n, bwtintv_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	bwtintv_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if (__sort_lt(*j, *i)) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) __ks_insertsort(a, a + n);					
}	


__device__ void ks_introsort(size_t n, bwtintv_t a[], void* d_buffer_ptr)						
{																	
	int d;															
	ks_isort_stack_t *top, *stack;									
	bwtintv_t rp, swap_tmp;											
	bwtintv_t *s, *t, *i, *j, *k;										
																	
	if (n < 1) return;												
	else if (n == 2) {												
		if (__sort_lt(a[1], a[0])) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; } 
		return;														
	}																
	for (d = 2; 1ul<<d < n; ++d);									
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8); 
	top = stack; s = a; t = a + (n-1); d <<= 1;						
	while (1) {														
		if (s < t) {												
			if (--d == 0) {											
				ks_combsort(t - s + 1, s);					
				t = s;												
				continue;											
			}														
			i = s; j = t; k = i + ((j-i)>>1) + 1;					
			if (__sort_lt(*k, *i)) {								
				if (__sort_lt(*k, *j)) k = j;						
			} else k = __sort_lt(*j, *i)? i : j;					
			rp = *k;												
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }	
			for (;;) {												
				do ++i; while (__sort_lt(*i, rp));					
				do --j; while (i <= j && __sort_lt(rp, *j));		
				if (j <= i) break;									
				swap_tmp = *i; *i = *j; *j = swap_tmp;				
			}														
			swap_tmp = *i; *i = *t; *t = swap_tmp;					
			if (i-s > t-i) {										
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; } 
				s = t-i > 16? i+1 : t;								
			} else {												
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; } 
				t = i-s > 16? i-1 : s;								
			}														
		} else {													
			if (top == stack) {										
				// free(stack);										
				__ks_insertsort(a, a+n);						
				return;												
			} else { --top; s = (bwtintv_t*)top->left; t = (bwtintv_t*)top->right; d = top->depth; } 
		}															
	}																
}																	

/* -------------- SORT for chain filtering -----------------------*/
#define flt_lt(a, b) ((a).w > (b).w)

__device__ static inline void ks_insertsort_mem_flt(mem_chain_t *s, mem_chain_t *t)		
{																	
	mem_chain_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && flt_lt(*j, *(j-1)); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}

__device__ void ks_combsort_mem_flt(size_t n, mem_chain_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	mem_chain_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if (flt_lt(*j, *i)) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) ks_insertsort_mem_flt(a, a + n);					
}	

__device__ void ks_introsort_mem_flt(size_t n, mem_chain_t a[], void* d_buffer_ptr)
{
	int d;
	ks_isort_stack_t *top, *stack;
	mem_chain_t rp, swap_tmp;
	mem_chain_t *s, *t, *i, *j, *k;

	if (n < 1) return;
	else if (n == 2) {
		if (flt_lt(a[1], a[0])) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; }
		return;
	}
	for (d = 2; 1ul<<d < n; ++d);
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8);
	top = stack; s = a; t = a + (n-1); d <<= 1;
	while (1) {
		if (s < t) {
			if (--d == 0) {
				ks_combsort_mem_flt(t - s + 1, s);
				t = s;
				continue;
			}
			i = s; j = t; k = i + ((j-i)>>1) + 1;
			if (flt_lt(*k, *i)) {
				if (flt_lt(*k, *j)) k = j;
			} else k = flt_lt(*j, *i)? i : j;
			rp = *k;
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }
			for (;;) {
				do ++i; while (flt_lt(*i, rp));
				do --j; while (i <= j && flt_lt(rp, *j));
				if (j <= i) break;
				swap_tmp = *i; *i = *j; *j = swap_tmp;
			}
			swap_tmp = *i; *i = *t; *t = swap_tmp;
			if (i-s > t-i) {
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; }
				s = t-i > 16? i+1 : t;
			} else {
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; }
				t = i-s > 16? i-1 : s;
			}
		} else {
			if (top == stack) {
				// free(stack);
				ks_insertsort_mem_flt(a, a+n);
				return;
			} else { --top; s = (mem_chain_t*)top->left; t = (mem_chain_t*)top->right; d = top->depth; }
		}
	}
}

/* -------------- SORT for uint64_t -----------------------*/
__device__ static inline void ks_insertsort_64(uint64_t *s, uint64_t *t){																	
	uint64_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && (*j<*(j-1)); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}

__device__ void ks_combsort_64(size_t n, uint64_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	uint64_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if (*j < *i) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) ks_insertsort_64(a, a + n);					
}																	

__device__ void ks_introsort_64(size_t n, uint64_t a[], void* d_buffer_ptr)						
{																	
	int d;															
	ks_isort_stack_t *top, *stack;									
	uint64_t rp, swap_tmp;											
	uint64_t *s, *t, *i, *j, *k;										
																	
	if (n < 1) return;												
	else if (n == 2) {												
		if (a[1]<a[0]) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; } 
		return;														
	}																
	for (d = 2; 1ul<<d < n; ++d);									
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8); 
	top = stack; s = a; t = a + (n-1); d <<= 1;						
	while (1) {														
		if (s < t) {												
			if (--d == 0) {											
				ks_combsort_64(t - s + 1, s);					
				t = s;												
				continue;											
			}														
			i = s; j = t; k = i + ((j-i)>>1) + 1;					
			if (*k < *i) {								
				if (*k < *j) k = j;						
			} else k = (*j < *i)? i : j;					
			rp = *k;												
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }	
			for (;;) {												
				do ++i; while (*i < rp);					
				do --j; while (i <= j && (rp < *j));		
				if (j <= i) break;									
				swap_tmp = *i; *i = *j; *j = swap_tmp;				
			}														
			swap_tmp = *i; *i = *t; *t = swap_tmp;					
			if (i-s > t-i) {										
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; } 
				s = t-i > 16? i+1 : t;								
			} else {												
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; } 
				t = i-s > 16? i-1 : s;								
			}														
		} else {													
			if (top == stack) {										
	// 			free(stack);										
				ks_insertsort_64(a, a+n);						
				return;												
			} else { --top; s = (uint64_t*)top->left; t = (uint64_t*)top->right; d = top->depth; } 
		}															
	}																
}																	


/* -------------------- sort for mem_ars2 -------------------------------------------*/
__device__ static inline void __ks_insertsort_mem_ars2(mem_alnreg_t *s, mem_alnreg_t *t)		
{																	
	mem_alnreg_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && (j->re < (j-1)->re); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}																	
__device__ void ks_combsort_mem_ars2(size_t n, mem_alnreg_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	mem_alnreg_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if ((j->re < i->re)) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) __ks_insertsort_mem_ars2(a, a + n);					
}																	
__device__ void ks_introsort_mem_ars2(size_t n, mem_alnreg_t a[], void* d_buffer_ptr)						
{																	
	int d;															
	ks_isort_stack_t *top, *stack;									
	mem_alnreg_t rp, swap_tmp;											
	mem_alnreg_t *s, *t, *i, *j, *k;										
																	
	if (n < 1) return;												
	else if (n == 2) {												
		if (a[1].re < a[0].re) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; } 
		return;														
	}																
	for (d = 2; 1ul<<d < n; ++d);									
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8); 
	top = stack; s = a; t = a + (n-1); d <<= 1;						
	while (1) {														
		if (s < t) {												
			if (--d == 0) {											
				ks_combsort_mem_ars2(t - s + 1, s);					
				t = s;												
				continue;											
			}														
			i = s; j = t; k = i + ((j-i)>>1) + 1;					
			if (k->re < i->re) {								
				if (k->re < j->re) k = j;						
			} else k = (j->re < i->re)? i : j;					
			rp = *k;												
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }	
			for (;;) {												
				do ++i; while (i->re < rp.re);					
				do --j; while (i <= j && (rp.re < j->re));		
				if (j <= i) break;									
				swap_tmp = *i; *i = *j; *j = swap_tmp;				
			}														
			swap_tmp = *i; *i = *t; *t = swap_tmp;					
			if (i-s > t-i) {										
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; } 
				s = t-i > 16? i+1 : t;								
			} else {												
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; } 
				t = i-s > 16? i-1 : s;								
			}														
		} else {													
			if (top == stack) {										
				// free(stack);										
				__ks_insertsort_mem_ars2(a, a+n);						
				return;												
			} else { --top; s = (mem_alnreg_t*)top->left; t = (mem_alnreg_t*)top->right; d = top->depth; } 
		}															
	}																
}																	

/* ------------------------- mem_ars_hash --------------------------------*/
#define alnreg_hlt(a, b)  ((a).score > (b).score || ((a).score == (b).score && ((a).is_alt < (b).is_alt || ((a).is_alt == (b).is_alt && (a).hash < (b).hash))))

__device__ static inline void ks_insertsort_mem_ars_hash(mem_alnreg_t *s, mem_alnreg_t *t)		
{																	
	mem_alnreg_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && alnreg_hlt(*j, *(j-1)); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}																	
__device__ void ks_combsort_mem_ars_hash(size_t n, mem_alnreg_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	mem_alnreg_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if (alnreg_hlt(*j, *i)) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) ks_insertsort_mem_ars_hash(a, a + n);					
}																	

__device__ void ks_introsort_mem_ars_hash(size_t n, mem_alnreg_t a[], void* d_buffer_ptr)						
{																	
	int d;															
	ks_isort_stack_t *top, *stack;									
	mem_alnreg_t rp, swap_tmp;											
	mem_alnreg_t *s, *t, *i, *j, *k;										
																	
	if (n < 1) return;												
	else if (n == 2) {												
		if (alnreg_hlt(a[1], a[0])) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; } 
		return;														
	}																
	for (d = 2; 1ul<<d < n; ++d);									
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8); 
	top = stack; s = a; t = a + (n-1); d <<= 1;						
	while (1) {														
		if (s < t) {												
			if (--d == 0) {											
				ks_combsort_mem_ars_hash(t - s + 1, s);					
				t = s;												
				continue;											
			}														
			i = s; j = t; k = i + ((j-i)>>1) + 1;					
			if (alnreg_hlt(*k, *i)) {								
				if (alnreg_hlt(*k, *j)) k = j;						
			} else k = alnreg_hlt(*j, *i)? i : j;					
			rp = *k;												
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }	
			for (;;) {												
				do ++i; while (alnreg_hlt(*i, rp));					
				do --j; while (i <= j && alnreg_hlt(rp, *j));		
				if (j <= i) break;									
				swap_tmp = *i; *i = *j; *j = swap_tmp;				
			}														
			swap_tmp = *i; *i = *t; *t = swap_tmp;					
			if (i-s > t-i) {										
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; } 
				s = t-i > 16? i+1 : t;								
			} else {												
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; } 
				t = i-s > 16? i-1 : s;								
			}														
		} else {													
			if (top == stack) {										
				// free(stack);										
				ks_insertsort_mem_ars_hash(a, a+n);						
				return;												
			} else { --top; s = (mem_alnreg_t*)top->left; t = (mem_alnreg_t*)top->right; d = top->depth; } 
		}															
	}																
}																	

/* ------------------------- mem_ars_hash2 --------------------------------*/
#define alnreg_hlt2(a, b)  ((a).score > (b).score || ((a).score == (b).score && ((a).is_alt < (b).is_alt || ((a).is_alt == (b).is_alt && (a).hash < (b).hash))))

__device__ static inline void ks_insertsort_mem_ars_hash2(mem_alnreg_t *s, mem_alnreg_t *t)		
{																	
	mem_alnreg_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && alnreg_hlt2(*j, *(j-1)); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}																	

__device__ void ks_combsort_mem_ars_hash2(size_t n, mem_alnreg_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	mem_alnreg_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if (alnreg_hlt2(*j, *i)) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) ks_insertsort_mem_ars_hash2(a, a + n);					
}																	

__device__ void ks_introsort_mem_ars_hash2(size_t n, mem_alnreg_t a[], void* d_buffer_ptr)						
{																	
	int d;															
	ks_isort_stack_t *top, *stack;									
	mem_alnreg_t rp, swap_tmp;											
	mem_alnreg_t *s, *t, *i, *j, *k;										
																	
	if (n < 1) return;												
	else if (n == 2) {												
		if (alnreg_hlt2(a[1], a[0])) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; } 
		return;														
	}																
	for (d = 2; 1ul<<d < n; ++d);									
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8); 
	top = stack; s = a; t = a + (n-1); d <<= 1;						
	while (1) {														
		if (s < t) {												
			if (--d == 0) {											
				ks_combsort_mem_ars_hash2(t - s + 1, s);					
				t = s;												
				continue;											
			}														
			i = s; j = t; k = i + ((j-i)>>1) + 1;					
			if (alnreg_hlt2(*k, *i)) {								
				if (alnreg_hlt2(*k, *j)) k = j;						
			} else k = alnreg_hlt2(*j, *i)? i : j;					
			rp = *k;												
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }	
			for (;;) {												
				do ++i; while (alnreg_hlt2(*i, rp));					
				do --j; while (i <= j && alnreg_hlt2(rp, *j));		
				if (j <= i) break;									
				swap_tmp = *i; *i = *j; *j = swap_tmp;				
			}														
			swap_tmp = *i; *i = *t; *t = swap_tmp;					
			if (i-s > t-i) {										
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; } 
				s = t-i > 16? i+1 : t;								
			} else {												
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; } 
				t = i-s > 16? i-1 : s;								
			}														
		} else {													
			if (top == stack) {										
				// free(stack);										
				ks_insertsort_mem_ars_hash2(a, a+n);						
				return;												
			} else { --top; s = (mem_alnreg_t*)top->left; t = (mem_alnreg_t*)top->right; d = top->depth; } 
		}															
	}																
}																	

/* ------------------------- pair of uint64_t --------------------------------*/
typedef struct {
	uint64_t x, y;
} pair64_t;

typedef struct { size_t n, m; pair64_t *a; } pair64_v;

#define pair64_lt(a, b) ((a).x < (b).x || ((a).x == (b).x && (a).y < (b).y))

__device__ static inline void ks_insertsort_128(pair64_t *s, pair64_t *t)		
{																	
	pair64_t *i, *j, swap_tmp;										
	for (i = s + 1; i < t; ++i)										
		for (j = i; j > s && pair64_lt(*j, *(j-1)); --j) {			
			swap_tmp = *j; *j = *(j-1); *(j-1) = swap_tmp;			
		}															
}																	

__device__ static void ks_combsort_128(size_t n, pair64_t a[])						
{																	
	const double shrink_factor = 1.2473309501039786540366528676643; 
	int do_swap;													
	size_t gap = n;													
	pair64_t tmp, *i, *j;												
	do {															
		if (gap > 2) {												
			gap = (size_t)(gap / shrink_factor);					
			if (gap == 9 || gap == 10) gap = 11;					
		}															
		do_swap = 0;												
		for (i = a; i < a + n - gap; ++i) {							
			j = i + gap;											
			if (pair64_lt(*j, *i)) {								
				tmp = *i; *i = *j; *j = tmp;						
				do_swap = 1;										
			}														
		}															
	} while (do_swap || gap > 2);									
	if (gap != 1) ks_insertsort_128(a, a + n);					
}																	
__device__ static void ks_introsort_128(size_t n, pair64_t a[], void* d_buffer_ptr)						
{																	
	int d;															
	ks_isort_stack_t *top, *stack;									
	pair64_t rp, swap_tmp;											
	pair64_t *s, *t, *i, *j, *k;										
																	
	if (n < 1) return;												
	else if (n == 2) {												
		if (pair64_lt(a[1], a[0])) { swap_tmp = a[0]; a[0] = a[1]; a[1] = swap_tmp; } 
		return;														
	}																
	for (d = 2; 1ul<<d < n; ++d);									
	stack = (ks_isort_stack_t*)CUDAKernelMalloc(d_buffer_ptr, sizeof(ks_isort_stack_t) * ((sizeof(size_t)*d)+2), 8); 
	top = stack; s = a; t = a + (n-1); d <<= 1;						
	while (1) {														
		if (s < t) {												
			if (--d == 0) {											
				ks_combsort_128(t - s + 1, s);					
				t = s;												
				continue;											
			}														
			i = s; j = t; k = i + ((j-i)>>1) + 1;					
			if (pair64_lt(*k, *i)) {								
				if (pair64_lt(*k, *j)) k = j;						
			} else k = pair64_lt(*j, *i)? i : j;					
			rp = *k;												
			if (k != t) { swap_tmp = *k; *k = *t; *t = swap_tmp; }	
			for (;;) {												
				do ++i; while (pair64_lt(*i, rp));					
				do --j; while (i <= j && pair64_lt(rp, *j));		
				if (j <= i) break;									
				swap_tmp = *i; *i = *j; *j = swap_tmp;				
			}														
			swap_tmp = *i; *i = *t; *t = swap_tmp;					
			if (i-s > t-i) {										
				if (i-s > 16) { top->left = s; top->right = i-1; top->depth = d; ++top; } 
				s = t-i > 16? i+1 : t;								
			} else {												
				if (t-i > 16) { top->left = i+1; top->right = t; top->depth = d; ++top; } 
				t = i-s > 16? i-1 : s;								
			}														
		} else {													
			if (top == stack) {										
				// free(stack);										
				ks_insertsort_128(a, a+n);						
				return;												
			} else { --top; s = (pair64_t*)top->left; t = (pair64_t*)top->right; d = top->depth; } 
		}															
	}																
}