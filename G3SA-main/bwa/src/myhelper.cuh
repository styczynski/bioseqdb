#ifndef _MYHELPER
#define _MYHELPER
#include <cuda_runtime.h>
#include <thrust/device_ptr.h>
#include <thrust/transform.h>
#include <thrust/scan.h>
#include <thrust/execution_policy.h>
#include "bwa.h"
#include "macro.h"

// your functor, assuming already defined:
struct get_n {
    __host__ __device__
        int operator()(const mem_alnreg_v& x) const { return x.n; }
};

// 1) assuming proc->d_offsets already allocated.
void compute_offsets_on_host(process_data_t* proc, int batch_size) {
    // 2) wrap your input pointer in a thrust device_ptr
    thrust::device_ptr<mem_alnreg_v>  reg_ptr = thrust::device_pointer_cast(proc->d_regs);
    auto n_iter = thrust::make_transform_iterator(reg_ptr, get_n{});

    // 3) wrap the output pointer similarly
    thrust::device_ptr<int> offsets_ptr = thrust::device_pointer_cast(proc->d_offsets);

    // 4a) exclusive scan [0, n₀, n₀+n₁, …] into d_offsets[0]…d_offsets[batch_size]
    thrust::exclusive_scan(
            thrust::device,               // execution policy
            n_iter,
            n_iter + batch_size + 1, // +1 for the total sum
            offsets_ptr
            );
}

// verification kernel: one thread will print the whole arrays
__global__ void verify_scan_kernel(const mem_alnreg_v* regs,
        const int*         offsets,
        int                batch_size)
{
    if (threadIdx.x == 0 && blockIdx.x == 0) {
        printf("=== scan verification ===\n");
        printf(" input n:    ");
        for (int i = 0; i < batch_size; ++i)
            printf("%d ", regs[i].n);
        printf("\n offsets:    ");
        for (int i = 0; i < batch_size+1; ++i)
            printf("%d ", offsets[i]);
        printf("\n total sum:  %d\n",
                offsets[batch_size]);  // sanity check
    }
}

__global__ void verify_writing_kernel(const int    *offsets,
        int           batch_size,
        const int    *rids,
        const int64_t*positions,
        const int    *ncigars,
        const uint32_t *cigars)
{
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    printf("=== positions array ===\n");
    for (int index = 0; index < batch_size; ++index) {
    printf("positions[%d] = %ld\n", index, positions[index]);
    }
    printf("=== end positions array ===\n\n");

    printf("=== writing verification ===\n");
    for (int batch_index = 0; batch_index < batch_size; ++batch_index) {
    if(offsets[batch_index+1]==offsets[batch_index]){
        printf("read %2d: X\n",
            batch_index);
        continue;
    }
    int offset = offsets[batch_index];
    printf("read %2d: offset=%4d, rid=%4d, pos=%8ld, ciglen=%2d\n",
        batch_index, offset, rids[offset], positions[offset], ncigars[offset]);

    printf("  cigar: ");
    const uint32_t* bam_base = cigars + offset * MAX_N_CIGAR;
    for (int cigar_index = 0; cigar_index < ncigars[offset]; ++cigar_index) {
        uint32_t cw = bam_base[cigar_index];
        int   len = BAM2LEN(cw);
        char  cch = BAM2OP(cw);
        printf("%d%c", len, cch);
    }
    printf("\n");
    }
    printf("=== end of writing verification ===\n");
}
#endif // _MYHELPER
