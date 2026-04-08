#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "CFastaSWScalar.h"

#define CUERR do{ cudaError_t err;			\
	if ((err = cudaGetLastError()) != cudaSuccess) {		\
			int device;	\
			cudaGetDevice(&device);	\
  		printf("CUDA error on GPU %d: %s : %s, line %d\n", device, cudaGetErrorString(err), __FILE__, __LINE__); }}while(0);

/*subsititution matrix in constant memory*/
__device__ __constant__ int cudaSubMatrix[32][32];

/*gap penalties*/
__device__ __constant__ int cudaGapExtend;
__device__ __constant__ int cudaGapOE; //cudaGapOpen + cudaGapExtend;
__device__ __constant__ int cudaGapExtendQuad;
__device__ __constant__ int cudaGapOEQuad;

/*query sequence*/
__device__ __constant__ int cudaQueryAlignedLen;

/*program counter*/
__device__ int cudaCounter;

#define NEG 		0x0FF
#define LIMIT		0x7F7F7F7F
#define FLIMIT	0xFFFFFFFF

/*inter-task parallelization*/
texture<unsigned int, 2, cudaReadModeElementType> InterSeqs; /*scalar computing packing*/
texture<uint4, 2, cudaReadModeElementType> InterSeqsQuad; /*quad-lane SIMD computing packing*/

/*query profiles*/
texture<int4, 2, cudaReadModeElementType> InterQueryPrf;
texture<int4, 3, cudaReadModeElementType> InterQueryPrfPtx;

#define ONE_CELL_COMP_QUAD(f, oe, ie, h, he, hd, sub, gapoe, gape,maxHH) \
				asm("vsub4.s32.s32.s32.sat %0, %1, %2, %3;" : "=r"(f) : "r"(f),"r"(gape), "r"(0));		\
				asm("vsub4.s32.s32.s32.sat %0, %1, %2, %3;" : "=r"(oe) : "r"(ie), "r"(gape), "r"(0));	\
				asm("vsub4.s32.s32.s32.sat %0, %1, %2, %3;" : "=r"(h) : "r"(h), "r"(gapoe), "r"(0));	\
				asm("vmax4.s32.s32.s32 %0, %1, %2, %3;" : "=r"(f) : "r"(f), "r"(h), "r"(0));	\
				asm("vsub4.s32.s32.s32.sat %0, %1, %2, %3;" : "=r"(h) : "r"(he), "r"(gapoe), "r"(0));	\
				asm("vmax4.s32.s32.s32 %0, %1, %2, %3;" : "=r"(oe) : "r"(oe), "r"(h), "r"(0));	\
				asm("vadd4.s32.s32.s32.sat %0, %1, %2, %3;" : "=r"(h) : "r"(hd), "r"(sub), "r"(0));	\
				asm("vmax4.s32.s32.s32 %0, %1, %2, %3;" : "=r"(h) : "r"(h), "r"(f), "r"(0));	\
				asm("vmax4.s32.s32.s32 %0, %1, %2, %3;" : "=r"(h) : "r"(h), "r"(oe), "r"(0));	\
				asm("vmax4.s32.s32.s32 %0, %1, %2, %3;" : "=r"(h) : "r"(h), "r"(0), "r"(0)); 	\
				asm("vmax4.s32.s32.s32 %0, %1, %2, %3;" : "=r"(maxHH) : "r"(maxHH), "r"(h), "r"(0)); \
				asm("mov.s32 %0, %1;" : "=r"(hd) : "r"(he));

/*Quad byte SIMD vectors using standard query profile*/
__device__ int InterGlobalSmithWatermanQuadQueryPrf(int qlen, int db_cx,
		int db_cy, int dblen) {
	int i, j, k;
	uint4 pack;
	int4 sa;
	int2 sb;
	int4 h, p, f, h0, p0, f0, sub, sub2, sub3, sub4;
	int2 HD;
	int maxHH;
	int e;
	int gapoe = cudaGapOEQuad;
	int gape = cudaGapExtendQuad;
	int4 zero = make_int4(0, 0, 0, 0);
	int2 zero2 = make_int2(0, 0);
	int2 global[MAX_SEQ_LENGTH_THRESHOLD + 1];
	for (i = 0; i <= dblen; i++) {
		global[i] = zero2;
	}

	maxHH = 0;
	for (i = 1; i <= qlen; i += QUERY_SEQ_LENGTH_ALIGNED) {
		h = zero;
		p = zero;
		f = zero;

		h0 = zero;
		p0 = zero;
		f0 = zero;
		/*get the index for query profile*/
		sb.x = i >> 2;
		sb.y = sb.x + 1;
		for (j = 0; j < dblen; j += 4) /*use the longest sequence*/
		{
			//load the packed 4 residues from 4 sequences
			pack = tex2D(InterSeqsQuad, db_cx, db_cy + (j >> 2));
			//compute the cell block SEQ_LENGTH_ALIGNED x 4
			for (k = 0; k < 4; k++) {
				//load data
				HD = global[j + k];

				//get the (j + k)-th residue
				sa = make_int4(pack.x & 0x0FF, pack.y & 0x0FF, pack.z & 0x0FF,
						pack.w & 0x0FF);
				pack.x >>= 8;
				pack.y >>= 8;
				pack.z >>= 8;
				pack.w >>= 8;

				//loading substitution scores
				sub = tex2D(InterQueryPrf, sa.x, sb.x);
				sub2 = tex2D(InterQueryPrf, sa.y, sb.x);
				sub3 = tex2D(InterQueryPrf, sa.z, sb.x);
				sub4 = tex2D(InterQueryPrf, sa.w, sb.x);

				sub.x = (sub.x << 24) | ((sub2.x & NEG) << 16)
						| ((sub3.x & NEG) << 8) | sub4.x & NEG; //
				sub.y = (sub.y << 24) | ((sub2.y & NEG) << 16)
						| ((sub3.y & NEG) << 8) | sub4.y & NEG; //
				sub.z = (sub.z << 24) | ((sub2.z & NEG) << 16)
						| ((sub3.z & NEG) << 8) | sub4.z & NEG; //
				sub.w = (sub.w << 24) | ((sub2.w & NEG) << 16)
						| ((sub3.w & NEG) << 8) | sub4.w & NEG; //

				//compute the cell (0, 0);
				ONE_CELL_COMP_QUAD(f.x, e, HD.y, h.x, HD.x, p.x, sub.x, gapoe,
						gape, maxHH)

				//compute cell (0, 1)
				ONE_CELL_COMP_QUAD(f.y, e, e, h.y, h.x, p.y, sub.y, gapoe, gape,
						maxHH)

				//compute cell (0, 2);
				ONE_CELL_COMP_QUAD(f.w, e, e, h.w, h.y, p.w, sub.w, gapoe, gape,
						maxHH)

				//compute cell (0, 3)
				ONE_CELL_COMP_QUAD(f.z, e, e, h.z, h.w, p.z, sub.z, gapoe, gape,
						maxHH)

				//loading substitution score
				sub = tex2D(InterQueryPrf, sa.x, sb.y);
				sub2 = tex2D(InterQueryPrf, sa.y, sb.y);
				sub3 = tex2D(InterQueryPrf, sa.z, sb.y);
				sub4 = tex2D(InterQueryPrf, sa.w, sb.y);

				sub.x = (sub.x << 24) | ((sub2.x & NEG) << 16)
						| ((sub3.x & NEG) << 8) | sub4.x & NEG; //
				sub.y = (sub.y << 24) | ((sub2.y & NEG) << 16)
						| ((sub3.y & NEG) << 8) | sub4.y & NEG; //
				sub.z = (sub.z << 24) | ((sub2.z & NEG) << 16)
						| ((sub3.z & NEG) << 8) | sub4.z & NEG; //
				sub.w = (sub.w << 24) | ((sub2.w & NEG) << 16)
						| ((sub3.w & NEG) << 8) | sub4.w & NEG; //

				//compute cell(0, 4)
				ONE_CELL_COMP_QUAD(f0.x, e, e, h0.x, h.z, p0.x, sub.x, gapoe,
						gape, maxHH)

				//compute cell(0, 5)
				ONE_CELL_COMP_QUAD(f0.y, e, e, h0.y, h0.x, p0.y, sub.y, gapoe,
						gape, maxHH)

				//compute cell (0, 6)
				ONE_CELL_COMP_QUAD(f0.w, e, e, h0.w, h0.y, p0.w, sub.w, gapoe,
						gape, maxHH)

				//compute cell(0, 7)
				ONE_CELL_COMP_QUAD(f0.z, e, e, h0.z, h0.w, p0.z, sub.z, gapoe,
						gape, maxHH)

				//save data cell(0, 7)
				//*(int2*) (((char*) global) + (j + k) * gpitch) = make_int2(h0.z, e);
				global[j + k] = make_int2(h0.z, e);
			}
		}
	}
	return maxHH;
}
/*Quad byte SIMD vectors using query profile variant*/
__device__ int InterGlobalSmithWatermanQuadVariant(int qlen, int db_cx,
		int db_cy, int dblen) {
	int i, j, k;
	uint4 pack;
	int4 sa;
	int2 sb;
	int4 h, p, f, h0, p0, f0, sub;
	int2 HD;
	int4 qprf, qprf2;
	int maxHH;
	int e;
	int gapoe = cudaGapOEQuad;
	int gape = cudaGapExtendQuad;
	int4 zero = make_int4(0, 0, 0, 0);
	int2 zero2 = make_int2(0, 0);
	int2 global[MAX_SEQ_LENGTH_THRESHOLD + 1];
	for (i = 0; i <= dblen; i++) {
		global[i] = zero2;
	}

	maxHH = 0;
	for (i = 1; i <= qlen; i += QUERY_SEQ_LENGTH_ALIGNED) {
		h = zero;
		p = zero;
		f = zero;

		h0 = zero;
		p0 = zero;
		f0 = zero;
		/*get the index for query profile*/
		sb.x = i >> 2;
		sb.y = sb.x + 1;
		for (j = 0; j < dblen; j += 4) /*use the longest sequence*/
		{
			//load the packed 4 residues from 4 sequences
			pack = tex2D(InterSeqsQuad, db_cx, db_cy + (j >> 2));
			//compute the cell block SEQ_LENGTH_ALIGNED x 4
			for (k = 0; k < 4; k++) {
				//load data
				HD = global[j + k];

				//get the (j + k)-th residue
				sa = make_int4(pack.x & 0x0FF, pack.y & 0x0FF, pack.z & 0x0FF,
						pack.w & 0x0FF);
				pack.x >>= 8;
				pack.y >>= 8;
				pack.z >>= 8;
				pack.w >>= 8;

				//loading substitution scores
				qprf = tex3D(InterQueryPrfPtx, sa.x, sa.y, sb.x);
				qprf2 = tex3D(InterQueryPrfPtx, sa.z, sa.w, sb.x);
				sub = make_int4((qprf.x << 16) | (qprf2.x & 0x0ffff),
						(qprf.y << 16) | (qprf2.y & 0x0ffff),
						(qprf.z << 16) | (qprf2.z & 0x0ffff),
						(qprf.w << 16) | (qprf2.w & 0x0ffff));

				//compute the cell (0, 0);
				ONE_CELL_COMP_QUAD(f.x, e, HD.y, h.x, HD.x, p.x, sub.x, gapoe,
						gape, maxHH)

				//compute cell (0, 1)
				ONE_CELL_COMP_QUAD(f.y, e, e, h.y, h.x, p.y, sub.y, gapoe, gape,
						maxHH)

				//compute cell (0, 2);
				ONE_CELL_COMP_QUAD(f.w, e, e, h.w, h.y, p.w, sub.w, gapoe, gape,
						maxHH)

				//compute cell (0, 3)
				ONE_CELL_COMP_QUAD(f.z, e, e, h.z, h.w, p.z, sub.z, gapoe, gape,
						maxHH)

				//loading substitution scores
				qprf = tex3D(InterQueryPrfPtx, sa.x, sa.y, sb.y);
				qprf2 = tex3D(InterQueryPrfPtx, sa.z, sa.w, sb.y);
				sub = make_int4((qprf.x << 16) | (qprf2.x & 0x0ffff),
						(qprf.y << 16) | (qprf2.y & 0x0ffff),
						(qprf.z << 16) | (qprf2.z & 0x0ffff),
						(qprf.w << 16) | (qprf2.w & 0x0ffff));

				//compute cell(0, 4)
				ONE_CELL_COMP_QUAD(f0.x, e, e, h0.x, h.z, p0.x, sub.x, gapoe,
						gape, maxHH)

				//compute cell(0, 5)
				ONE_CELL_COMP_QUAD(f0.y, e, e, h0.y, h0.x, p0.y, sub.y, gapoe,
						gape, maxHH)

				//compute cell (0, 6)
				ONE_CELL_COMP_QUAD(f0.w, e, e, h0.w, h0.y, p0.w, sub.w, gapoe,
						gape, maxHH)

				//compute cell(0, 7)
				ONE_CELL_COMP_QUAD(f0.z, e, e, h0.z, h0.w, p0.z, sub.z, gapoe,
						gape, maxHH)

				//save data cell(0, 7)
				//*(int2*) (((char*) global) + (j + k) * gpitch) = make_int2(h0.z, e);
				global[j + k] = make_int2(h0.z, e);
			}
		}
	}
	return maxHH;
}

__global__ void interSWUsingSIMTQuadDynamicQueryPrf(DatabaseHash* hash,
		SeqEntry* cudaResult, int numSeqQuads) {
	int tidx;
	int score, seqidx;
	__shared__ int blkIdx;

	/*calculate the global index of the thread in the grid*/
	tidx = blockIdx.x * blockDim.x + threadIdx.x;

	/*get the sequence index*/
	seqidx = tidx;

	/*for each thread*/
	while (1) {
		if (seqidx < numSeqQuads) {
			/*get the sequence information*/
			DatabaseHash dbhash = hash[seqidx];

			/*run the kernel*/
			score = InterGlobalSmithWatermanQuadQueryPrf(cudaQueryAlignedLen,
					dbhash.cx, dbhash.cy, dbhash.alignedLen);

			/*save the scores. It does not matter if the index overflows here*/
			seqidx *= 4;
			cudaResult[seqidx++].value = (score >> 24) & 0x0ff;
			cudaResult[seqidx++].value = (score >> 16) & 0X0ff;
			cudaResult[seqidx++].value = (score >> 8) & 0x0ff;
			cudaResult[seqidx].value = score & 0x0ff;
		}
		__syncthreads();
		if (threadIdx.x == 0) {
			/*get a new thread block*/
			blkIdx = atomicAdd(&cudaCounter, 1);
		}
		__syncthreads();

		/*compute teh base index for the thread block*/
		blkIdx *= blockDim.x;
		if (blkIdx >= numSeqQuads) {
			break;
		}
		/*compute the new sequence index*/
		seqidx = blkIdx + threadIdx.x;
	}
}
__global__ void interSWUsingSIMTQuadDynamicVariant(DatabaseHash* hash,
		SeqEntry* cudaResult, int numSeqQuads) {
	int tidx;
	int score, seqidx;
	__shared__ int blkIdx;

	/*calculate the global index of the thread in the grid*/
	tidx = blockIdx.x * blockDim.x + threadIdx.x;

	/*get the sequence index*/
	seqidx = tidx;

	/*for each thread*/
	while (1) {
		if (seqidx < numSeqQuads) {
			/*get the sequence information*/
			DatabaseHash dbhash = hash[seqidx];

			/*run the kernel*/
			score = InterGlobalSmithWatermanQuadVariant(cudaQueryAlignedLen,
					dbhash.cx, dbhash.cy, dbhash.alignedLen);

			/*save the scores. It does not matter if the index overflows here*/
			seqidx *= 4;
			cudaResult[seqidx++].value = (score >> 24) & 0x0ff;
			cudaResult[seqidx++].value = (score >> 16) & 0X0ff;
			cudaResult[seqidx++].value = (score >> 8) & 0x0ff;
			cudaResult[seqidx].value = score & 0x0ff;
		}
		__syncthreads();
		if (threadIdx.x == 0) {
			/*get a new thread block*/
			blkIdx = atomicAdd(&cudaCounter, 1);
		}
		__syncthreads();

		/*compute teh base index for the thread block*/
		blkIdx *= blockDim.x;
		if (blkIdx >= numSeqQuads) {
			break;
		}
		/*compute the new sequence index*/
		seqidx = blkIdx + threadIdx.x;
	}
}
void CFastaSWScalar::InterRunGlobalDatabaseScanningQuadDynamic(int blknum,
		int threads, int numSeqQuads, bool useQueryPrf, cudaStream_t stream) {

	dim3 grid(blknum, 1, 1);
	dim3 block(threads, 1, 1);

	/*copy the counter*/
	cudaMemcpyToSymbol(cudaCounter, &blknum, sizeof(int));
	CUERR

	if (useQueryPrf) {
		interSWUsingSIMTQuadDynamicQueryPrf<<<grid, block, 0, stream>>>(
				cudaSeqHashQuad, cudaResult, numSeqQuads);
	} else {
		interSWUsingSIMTQuadDynamicVariant<<<grid, block, 0, stream>>>(
				cudaSeqHashQuad, cudaResult, numSeqQuads);
	}

	CUERR
}
__global__ void interSWUsingSIMTQuadQueryPrf(DatabaseHash* hash, SeqEntry* cudaResult,
		int numSeqQuads) {
	int tidx;
	int score;

	/*calculate the global index of the thread in the grid*/
	tidx = blockIdx.x * blockDim.x + threadIdx.x;

	/*check the sequence index*/
	if (tidx >= numSeqQuads) {
		return;
	}

	//get the hash item
	DatabaseHash dbhash = hash[tidx];
	score = InterGlobalSmithWatermanQuadQueryPrf(cudaQueryAlignedLen, dbhash.cx,
      dbhash.cy, dbhash.alignedLen);

	/*save the scores. It does not matter if the index overflows here*/
	int seqidx = tidx << 2;
	cudaResult[seqidx++].value = (score >> 24) & 0x0ff;
	cudaResult[seqidx++].value = (score >> 16) & 0X0ff;
	cudaResult[seqidx++].value = (score >> 8) & 0x0ff;
	cudaResult[seqidx].value = score & 0x0ff;
}
__global__ void interSWUsingSIMTQuadVariant(DatabaseHash* hash, SeqEntry* cudaResult,
		int numSeqQuads) {
	int tidx;
	int score;

	/*calculate the global index of the thread in the grid*/
	tidx = blockIdx.x * blockDim.x + threadIdx.x;

	/*check the sequence index*/
	if (tidx >= numSeqQuads) {
		return;
	}

	//get the hash item
	DatabaseHash dbhash = hash[tidx];
	score = InterGlobalSmithWatermanQuadVariant(cudaQueryAlignedLen, dbhash.cx,
			dbhash.cy, dbhash.alignedLen);

	/*save the scores. It does not matter if the index overflows here*/
	int seqidx = tidx << 2;
	cudaResult[seqidx++].value = (score >> 24) & 0x0ff;
	cudaResult[seqidx++].value = (score >> 16) & 0X0ff;
	cudaResult[seqidx++].value = (score >> 8) & 0x0ff;
	cudaResult[seqidx].value = score & 0x0ff;
}

void CFastaSWScalar::InterRunGlobalDatabaseScanningQuad(int blknum, int threads,
		int numSeqQuads, int useQueryPrf, cudaStream_t stream) {
	dim3 grid(blknum, 1, 1);
	dim3 block(threads, 1, 1);

	/*invoke the kernel*/
#if 0
	if(useQueryPrf){
		interSWUsingSIMTQuadQueryPrf<<<grid, block, 0, stream>>>(cudaSeqHashQuad,
			cudaResult, numSeqQuads);
	}else{
		interSWUsingSIMTQuadVariant<<<grid, block, 0, stream>>>(cudaSeqHashQuad,
			cudaResult, numSeqQuads);
	}
#else
 	interSWUsingSIMTQuadVariant<<<grid, block, 0, stream>>>(cudaSeqHashQuad,
      cudaResult, numSeqQuads);
#endif
	CUERR
}

/*class functions*/
CFastaSWScalar::CFastaSWScalar() :
		CFastaSW() {
	cudaUseQueryPrf = false;
}
CFastaSWScalar::~CFastaSWScalar() {
	//do nothing
}
//global functions
void CFastaSWScalar::swMemcpyParameters(int matrix[32][32], int gapOpen,
		int gapExtend) {
	int gapOE = gapOpen + gapExtend;
	assert(gapOE <= 0x0FF);

	cudaMemcpyToSymbol(cudaSubMatrix, matrix, 32 * 32 * sizeof(int));
	CUERR

	cudaMemcpyToSymbol(cudaGapExtend, &gapExtend, sizeof(int));
	CUERR

	cudaMemcpyToSymbol(cudaGapOE, &gapOE, sizeof(int));
	CUERR

			/*for SIMD vectors*/
			/*quad half-word integer*/
	int gapExtendQuad = (gapExtend << 24) | (gapExtend << 16) | (gapExtend << 8)
			| gapExtend;
	int gapOEQuad = (gapOE << 24) | (gapOE << 16) | (gapOE << 8) | gapOE;

	cudaMemcpyToSymbol(cudaGapExtendQuad, &gapExtendQuad, sizeof(int));
	CUERR

	cudaMemcpyToSymbol(cudaGapOEQuad, &gapOEQuad, sizeof(int));
	CUERR

	/*for longer queries, we use the standard query profile*/
	cudaFuncSetCacheConfig(interSWUsingSIMTQuadDynamicQueryPrf,
				cudaFuncCachePreferL1);
	CUERR

	/*for shorter queries, we use the query profile variant*/
	cudaFuncSetCacheConfig(interSWUsingSIMTQuadDynamicVariant,
				cudaFuncCachePreferL1);
	CUERR

	/*set L1 cache*/
	cudaFuncSetCacheConfig(interSWUsingSIMTQuadQueryPrf, cudaFuncCachePreferL1);
	CUERR

	cudaFuncSetCacheConfig(interSWUsingSIMTQuadVariant, cudaFuncCachePreferL1);
	CUERR

}
void CFastaSWScalar::swMemcpyQuery(unsigned char* query, int qAlignedLen,
		int offset, int matrix[32][32], bool userQueryPrf) {
	int i, j;
	int prfLength = qAlignedLen >> 2;

	/*copy the query length*/
	cudaMemcpyToSymbol(cudaQueryAlignedLen, &qAlignedLen, sizeof(int));
	CUERR

	/*build the profile for inter-task parallelization*/
	cudaUseQueryPrf = userQueryPrf;
	if (cudaUseQueryPrf) {
		cudaInterQueryPrf = swMallocArray(MATRIX_SIZE, prfLength,
				pChannelFormatKindSignedInt4);
		int4* hostQueryPrf = (int4*) pMallocHost(
				sizeof(int4) * prfLength * MATRIX_SIZE);
		for (i = 0; i < prfLength; ++i) {
			int4* p = hostQueryPrf + i * MATRIX_SIZE;
			int index = i << 2;
			for (j = 0; j < MATRIX_SIZE; ++j) {
				p->x = matrix[j][query[index]];
				p->y = matrix[j][query[index + 1]];
				p->w = matrix[j][query[index + 2]];
				p->z = matrix[j][query[index + 3]];
				++p;
			}
		}
		pMemcpy2DToArray(cudaInterQueryPrf, 0, 0, hostQueryPrf,
				MATRIX_SIZE * sizeof(int4), MATRIX_SIZE * sizeof(int4),
				prfLength, pMemcpyHostToDevice);
		pFreeHost(hostQueryPrf);

	} else {
		/*construct SIMD query profile*/
		cudaExtent volumeSize = make_cudaExtent(MATRIX_SIZE, MATRIX_SIZE,
				prfLength);
		/*allocate host memory*/
		short4* hostPtxQueryPrf = (short4*) pMallocHost(
				sizeof(short4) * volumeSize.width * volumeSize.height
						* volumeSize.depth);
		/*create 3D array*/
		cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<short4>();
		cudaMalloc3DArray(&cudaPtxQueryPrf, &channelDesc, volumeSize);
		CUERR

				/*fill the host array*/
		int4 index, bases;
		for (int d = 0; d < volumeSize.depth; ++d) {
			index.x = d << 2;
			bases.x = query[index.x];
			index.y = index.x + 1;
			bases.y = query[index.y];
			index.w = index.x + 2;
			bases.w = query[index.w];
			index.z = index.x + 3;
			bases.z = query[index.z];
			for (int h = 0; h < volumeSize.height; ++h) {
				short4* p = hostPtxQueryPrf
						+ d * volumeSize.height * volumeSize.width
						+ h * volumeSize.width;
				for (int w = 0; w < volumeSize.width; ++w) {
					p->x = (matrix[w][bases.x] << 8)
							| (matrix[h][bases.x] & 0x0ff);
					p->y = (matrix[w][bases.y] << 8)
							| (matrix[h][bases.y] & 0x0ff);
					p->w = (matrix[w][bases.w] << 8)
							| (matrix[h][bases.w] & 0x0ff);
					p->z = (matrix[w][bases.z] << 8)
							| (matrix[h][bases.z] & 0x0ff);
					++p;
				}
			}
		}

		/*copy data to 3D array*/
		cudaMemcpy3DParms copyParams = { 0 };
		copyParams.srcPtr = make_cudaPitchedPtr((void*) hostPtxQueryPrf,
				volumeSize.width * sizeof(short4), volumeSize.width,
				volumeSize.height);
		copyParams.dstArray = cudaPtxQueryPrf;
		copyParams.extent = volumeSize;
		copyParams.kind = cudaMemcpyHostToDevice;
		cudaMemcpy3D(&copyParams);
		CUERR

				/*release host array*/
		pFreeHost(hostPtxQueryPrf);
	}
}

void CFastaSWScalar::swBindTextureToArrayQuad() {

	/*for inter-task quad-lane SIMD computing*/
	InterSeqsQuad.addressMode[0] = cudaAddressModeClamp;
	InterSeqsQuad.addressMode[1] = cudaAddressModeClamp;
	InterSeqsQuad.filterMode = cudaFilterModePoint;
	InterSeqsQuad.normalized = false;

	cudaBindTextureToArray(InterSeqsQuad, (cudaArray*) cudaInterSeqsQuad,
			uint4_channelDesc);
	CUERR
}

void CFastaSWScalar::swBindQueryProfile() {

	/*bind query profile for inter-task scalar computing*/
	if (cudaUseQueryPrf) {
		InterQueryPrf.addressMode[0] = cudaAddressModeClamp;
		InterQueryPrf.addressMode[1] = cudaAddressModeClamp;
		InterQueryPrf.filterMode = cudaFilterModePoint;
		InterQueryPrf.normalized = false;

		cudaBindTextureToArray(InterQueryPrf, (cudaArray*) cudaInterQueryPrf,
				sint4_channelDesc);
		CUERR
	} else {
		/*bind query profile for inter-task quad-lane SIMD computing*/
		InterQueryPrfPtx.filterMode = cudaFilterModePoint;
		InterQueryPrfPtx.normalized = false;
		InterQueryPrfPtx.addressMode[0] = cudaAddressModeClamp;
		InterQueryPrfPtx.addressMode[1] = cudaAddressModeClamp;
		InterQueryPrfPtx.addressMode[2] = cudaAddressModeClamp;

		cudaChannelFormatDesc short4_channelDesc =
				cudaCreateChannelDesc<short4>();
		cudaBindTextureToArray(InterQueryPrfPtx, (cudaArray*) cudaPtxQueryPrf,
				short4_channelDesc);
		CUERR
	}
}

void CFastaSWScalar::swUnbindTextureQuad() {
	cudaUnbindTexture(InterSeqsQuad);
	CUERR
}

void CFastaSWScalar::swUnbindQueryProfile() {
	if (cudaUseQueryPrf) {
		//Unbind Query Profile
		cudaUnbindTexture(InterQueryPrf);
		CUERR

				//release the CUDA array
		pFreeArray(cudaInterQueryPrf);
		cudaInterQueryPrf = NULL;
	} else {
		//Unbind Query Profile for ptx
		cudaUnbindTexture(InterQueryPrfPtx);
		CUERR

				//release the CUDA array for ptx
		pFreeArray(cudaPtxQueryPrf);
		cudaPtxQueryPrf = NULL;
	}
}


