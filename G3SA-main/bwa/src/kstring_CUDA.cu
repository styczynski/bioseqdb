#include "gmem_alloc.h"
#include "kstring_CUDA.cuh"
#include <stdio.h>
#include <stdarg.h>

#define kroundup32(x) (--(x), (x)|=(x)>>1, (x)|=(x)>>2, (x)|=(x)>>4, (x)|=(x)>>8, (x)|=(x)>>16, ++(x))

__device__ void ks_resize(kstring_t *s, size_t size, void* d_buffer_ptr)
{
	if (s->m < size) {
		s->m = size;
		kroundup32(s->m);
		s->s = (char*)CUDAKernelRealloc(d_buffer_ptr, s->s, s->m, 1);
	}
}

__device__ int strlen_GPU(const char *p)
{
	int counter = 0;
    if(!p)
    {
        return 0;
    }
	while (*(p+counter))
		counter++;
	return counter;
}

/* concatenate string p to the end of string s->s */
__device__ int kputsn(const char *p, int l, kstring_t *s, void* d_buffer_ptr)
{
	if (s->l + l + 1 >= s->m) {
		s->m = s->l + l + 2;
		kroundup32(s->m);
		s->s = (char*)CUDAKernelRealloc(d_buffer_ptr, s->s, s->m, 1);
	}
	cudaKernelMemcpy((void*)p, s->s + s->l, l);
	s->l += l;
	s->s[s->l] = 0;
	return l;
}

/* concatenate string p to the end of string s->s without given len(p)*/
__device__ int kputs(const char *p, kstring_t *s, void* d_buffer_ptr)
{
	return kputsn(p, strlen_GPU(p), s, d_buffer_ptr);
}

/* add one char (c) to string s->s*/
__device__ int kputc(int c, kstring_t *s, void* d_buffer_ptr)
{
	if (s->l + 1 >= s->m) {
		s->m = s->l + 2;
		kroundup32(s->m);
		s->s = (char*)CUDAKernelRealloc(d_buffer_ptr, s->s, s->m, 1);
	}
	s->s[s->l++] = c;
	s->s[s->l] = 0;
	return c;
}

__device__ int kputw(int c, kstring_t *s, void* d_buffer_ptr)
{
	char buf[16];
	int l, x;
	if (c == 0) return kputc('0', s, d_buffer_ptr);
	for (l = 0, x = c < 0? -c : c; x > 0; x /= 10) buf[l++] = x%10 + '0';
	if (c < 0) buf[l++] = '-';
	if (s->l + l + 1 >= s->m) {
		s->m = s->l + l + 2;
		kroundup32(s->m);
		s->s = (char*)CUDAKernelRealloc(d_buffer_ptr, s->s, s->m, 1);
	}
	for (x = l - 1; x >= 0; --x) s->s[s->l++] = buf[x];
	s->s[s->l] = 0;
	return 0;
}

__device__ int kputl(long c, kstring_t *s, void* d_buffer_ptr)
{
	char buf[32];
	long l, x;
	if (c == 0) return kputc('0', s, d_buffer_ptr);
	for (l = 0, x = c < 0? -c : c; x > 0; x /= 10) buf[l++] = x%10 + '0';
	if (c < 0) buf[l++] = '-';
	if (s->l + l + 1 >= s->m) {
		s->m = s->l + l + 2;
		kroundup32(s->m);
		s->s = (char*)CUDAKernelRealloc(d_buffer_ptr, s->s, s->m, 1);
	}
	for (x = l - 1; x >= 0; --x) s->s[s->l++] = buf[x];
	s->s[s->l] = 0;
	return 0;
}

// Reverses a string 'str' of length 'len' 
__device__ static inline void reverse(char* str, int len) 
{ 
	int i = 0, j = len - 1, temp; 
	while (i < j) { 
		temp = str[i]; 
		str[i] = str[j]; 
		str[j] = temp; 
		i++; 
		j--; 
	} 
} 

// Converts a given integer x to string str[]. 
// d is the number of digits required in the output. 
// If d is more than the number of digits in x, 
// then 0s are added at the beginning. 
__device__ static inline int intToStr(int x, char str[], int d) 
{ 
	int i = 0; 
	while (x) { 
		str[i++] = (x % 10) + '0'; 
		x = x / 10; 
	} 

	// If number of digits required is more, then 
	// add 0s at the beginning 
	while (i < d) 
		str[i++] = '0'; 

	reverse(str, i); 
	str[i] = '\0'; 
	return i; 
} 

// Converts a floating-point/double number to a string. 
// afterpoint is the number of digits after the dot "."
// return length of string (exclude NULL terminating)
__device__ static inline int ftoa(float n, char* res, int afterpoint) 
{
	// Extract integer part 
	int ipart = (int)n; 

	// Extract floating part 
	float fpart = n - (float)ipart; 

	// convert integer part to string 
	int i = intToStr(ipart, res, 0); 

	// check for display option after point 
	if (afterpoint != 0) { 
		res[i] = '.'; // add dot 

		// Get the value of fraction part upto given no. 
		// of points after dot. The third parameter 
		// is needed to handle cases like 233.007 
		fpart = fpart * pow(10, afterpoint); 

		intToStr((int)fpart, res + i + 1, afterpoint); 
	}
	return i + 1 + afterpoint;
} 


__device__ int ksprintf(kstring_t *s, const char *fmt, float alt_sc, void* d_buffer_ptr)
{
	int l;
	char out_string[8];
	l = ftoa(alt_sc, out_string, 3);
	if (l + 1 >= s->m - s->l) {
		s->m = s->l + l + 2;
		kroundup32(s->m);
		s->s = (char*)CUDAKernelRealloc(d_buffer_ptr, s->s, s->m, 1);
	}
	// add out_tring to s->s
	kputsn(out_string, l, s, d_buffer_ptr);
	s->l += l;
	return l;
}

/* duplicate a string and return the new pointer */
__device__ char* strdup_GPU(char* src, void* d_buffer_ptr)
{
    char *str;
    char *p;
    int len = 0;

    while (src[len])
        len++;
    str = (char*)CUDAKernelMalloc(d_buffer_ptr, len + 1, 1);
    p = str;
    while (*src)
        *p++ = *src++;
    *p = '\0';
    return str;
}
