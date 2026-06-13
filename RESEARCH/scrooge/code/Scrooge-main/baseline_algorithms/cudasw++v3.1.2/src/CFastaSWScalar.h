#ifndef _CFASTASW_SCALAR_H
#define _CFASTASW_SCALAR_H
#include "CParams.h"
#include "CFastaSW.h"
#include "GenericFunction.h"

/*score profile for SIMD vectors in global memory*/
#define SCORE_PRF_COLS				MATRIX_SIZE * MATRIX_SIZE
#define SCORE_PRF_ROWS				MATRIX_SIZE

class CFastaSWScalar: public CFastaSW
{
public:
	CFastaSWScalar();
	~CFastaSWScalar();

	void swMemcpyParameters(int matrix[32][32], int gapOpen, int gapExtend);
	void swMemcpyQuery(unsigned char* query, int qAlignedLen, int offset,
			int matrix[32][32], bool useQueryPrf);
	void swBindTextureToArrayQuad();
	void swUnbindTextureQuad();
	void swBindQueryProfile();
	void swUnbindQueryProfile();

	void InterRunGlobalDatabaseScanningQuadDynamic(int blknum, int threads,
			int numSeqs, bool useQueryPrf, cudaStream_t stream = 0);

	void InterRunGlobalDatabaseScanningQuad(int blknum, int threads,
			int numSeqs, int useQueryPrf, cudaStream_t stream = 0);
private:
	//query profile
	void* cudaInterQueryPrf;
	cudaArray* cudaPtxQueryPrf;
	bool cudaUseQueryPrf;
};
#endif
