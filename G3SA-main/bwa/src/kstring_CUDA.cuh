#ifndef _KSTRING_CUDA_H
#define _KSTRING_CUDA_H

typedef struct __kstring_t {
	size_t l, m;
	char *s;
} kstring_t;

extern __device__ int strlen_GPU(const char *p);
extern __device__ void ks_resize(kstring_t *s, size_t size, void* d_buffer_ptr);
__device__ int ksprintf(kstring_t *s, const char *fmt, float alt_sc, void* d_buffer_ptr);
extern __device__ int kputsn(const char *p, int l, kstring_t *s, void* d_buffer_ptr);
extern __device__ int kputs(const char *p, kstring_t *s, void* d_buffer_ptr);
extern __device__ int kputc(int c, kstring_t *s, void* d_buffer_ptr);
extern __device__ int kputw(int c, kstring_t *s, void* d_buffer_ptr);
extern __device__ int kputl(long c, kstring_t *s, void* d_buffer_ptr);
extern __device__ char* strdup_GPU(char* src, void* d_buffer_ptr);
#endif
