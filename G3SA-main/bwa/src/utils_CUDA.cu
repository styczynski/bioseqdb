#include "bwa.h"
#include "macro.h"
#include "utils_CUDA.cuh"

__device__ const char * const phasename[] = {
     /* _DETAIL     */ "dummy",
     /* _STAGE_TIME */ "dummy",
     /* _STEP_TIME  */ "dummy",
     /* _SMEM       */ "smem",
     /* _INTV       */ "intv",
     /* _SEED       */ "seed",
     /* _STSEED     */ "stseed",
     /* _CHAIN      */ "chain",
     /* _STCHAIN    */ "stchain",
     /* _FTCHAIN    */ "ftchain",
     /* _EXPAIR     */ "expair",
     /* _REGION     */ "region",
     /* _FTREGION   */ "ftregion",
     /* _STREGION   */ "stregion",
     /* _TBPAIR     */ "tbpair",
     /* _ALIGNMENT  */ "alignment",
     /* _RESULT     */ "result",
};

// format: phase readID qbeg qend num_hits sa_k
__global__ void printIntv(smem_aux_t *d_intvvecs, int readID, int type)
{
    if(blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    bwtintv_t *intvs = d_intvvecs[readID].mem.a;
    int num_intv = d_intvvecs[readID].mem.n;
    int intv_type = 0;
    printf("seq_id %d num_seeds %d\n", readID, num_intv);
    /*
    for(bwtintv_t *intv = intvs; intv < intvs + num_intv; intv++) {
        if(intv->x[2] > 0) {
            printf("%s %d %d %d %d %lu\n", phasename[type], readID, \
                    M(*intv), N(*intv), (int)intv->x[2],\
                    intv->x[0]);
        } else { // SMEM and each reseed types are 0-separated.
        intv_type++;
        intv_type = intv_type > 2 ? 2 : intv_type;
        }
    }
    */
}


// format: phase readID rbeg len qbeg
__global__ void printSeed(mem_seed_v *d_seedvecs, int readID, int type)
{
    if(blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    mem_seed_t *seedvec = d_seedvecs[readID].a;
    int num_seeds = d_seedvecs[readID].n;
    for(mem_seed_t *seed = seedvec; seed < seedvec + num_seeds; seed++) {
        printf("%s %d %ld %d %d\n", phasename[type], readID,\
                seed->rbeg, seed->len, seed->qbeg);
    }
}


// format: phase readID rpos weight num_seeds
__global__ void printChain(mem_chain_v *d_chainvecs, int readID, int type)
{
    if(blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    mem_chain_t *chainvec = d_chainvecs[readID].a;
    int num_chains = d_chainvecs[readID].n;
    for(mem_chain_t *chain = chainvec; chain < chainvec + num_chains; chain++) {
        printf("%s %d %ld %u %d\n", phasename[type], readID,\
                chain->pos, chain->w, chain->n);
    }
}


// format: [SWpair readID] q_left r_left q_right r_right
//         [ANpair readID] q r
__global__ void printPair(seed_record_t *d_pairs, int num_records, int type)
{
    if(blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    if(type == _EXPAIR) {
        for(seed_record_t *record = d_pairs; record < d_pairs + num_records;\
                record++) {
            printf("%s %d ", phasename[type], record->seqID);
            if(record->readlen_left == 0) {
                printf("- -");
            } else {
                for(uint8_t *b = record->read_left;\
                        b < record->read_left + record->readlen_left; b++) {
                    printf("%c", "ACGTN"[*b]);
                }
                printf(" ");
                for(uint8_t *b = record->ref_left;\
                        b < record->ref_left + record->reflen_left; b++) {
                    printf("%c", "ACGTN"[*b]);
                }
            }
            printf(" ");
            if(record->readlen_right == 0) {
                printf("- -");
            } else {
                for(uint8_t *b = record->read_right;\
                        b < record->read_right + record->readlen_right; b++) {
                    printf("%c", "ACGTN"[*b]);
                }
                printf(" ");
                for(uint8_t *b = record->ref_right;\
                        b < record->ref_right + record->reflen_right; b++) {
                    printf("%c", "ACGTN"[*b]);
                }
            }
            printf("\n");
        }
    } else { // _TBPAIR
        for(seed_record_t *record = d_pairs; record < d_pairs + num_records;\
                record++) {
                //printf("(%d %d %d %d)\n", record->readlen_left, record->reflen_left,\
                record->readlen_right, record->reflen_right);
                //printf("(%p %p %p %p)\n", record->read_left, record->ref_left,\
                record->read_right, record->ref_right);
            printf("%s %d ", phasename[type], record->seqID);
            if(record->readlen_right == 0 || !(record->read_right)) {
                printf("-");
            } else {
                for(uint8_t *b = record->read_right;\
                        b < record->read_right + record->readlen_right; b++) {
                    printf("%c", "ACGTN"[*b]);
                }
            }
            printf(" ");
            if(record->reflen_right == 0 || !(record->ref_right)) {
                printf("-");
            } else {
                for(uint8_t *b = record->ref_right;\
                        b < record->ref_right + record->reflen_right; b++) {
                    printf("%c", "ACGTN"[*b]);
                }
            }
            printf("\n");
        }
    }
}


// format: phase readID rb re qb qe score w seedcov frac_rep seedlen0
__global__ void printReg(mem_alnreg_v *d_regvecs, int readID, int type)
{
    if(blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }

    for(mem_alnreg_t *reg = d_regvecs[readID].a;\
            reg < d_regvecs[readID].a + d_regvecs[readID].n; reg++) {

        printf("%s %d %ld %ld %d %d %d %d %d %f %d\n", phasename[type],\
                readID, reg->rb, reg->re, reg->qb, reg->qe, reg->score, reg->w,\
                reg->seedcov, reg->frac_rep, reg->seedlen0);
    }
}


// format: phase readID rname rpos cigarstring
__global__ void printAln(bntseq_t *d_bns, mem_aln_v *d_alnvecs, int readID, int type)
{
    if(blockIdx.x != 0 || threadIdx.x != 0) {
        return;
    }
    for(mem_aln_t *aln = d_alnvecs[readID].a;\
            aln < d_alnvecs[readID].a + d_alnvecs[readID].n; aln++) {
        printf("%s %d %s %ld ", phasename[type], readID+1, d_bns->anns[aln->rid].name, aln->pos+1); //1-based rpos
        for(uint32_t *bam = aln->cigar; bam < aln->cigar + aln->n_cigar;\
                bam++) {
            printf("%d%c", BAM2LEN(*bam), BAM2OP(*bam));
        }
        printf("\n");
    }
    if(d_alnvecs[readID].n == 0) {
        printf("%d -1 -1\n", readID+1);
    }
}
