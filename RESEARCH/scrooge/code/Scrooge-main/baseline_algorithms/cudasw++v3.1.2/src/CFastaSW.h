#ifndef _CFASTASW_H
#define _CFASTASW_H
#include "GenericFunction.h"

#define pChannelFormatKindSignedInt       	 	0	
#define pChannelFormatKindUnsignedChar      	1
#define pChannelFormatKindUnsignedChar4      	2
#define pChannelFormatKindChar4      					3
#define pChannelFormatKindSignedInt4      		4
#define pChannelFormatKindUnsigned						5
#define pChannelFormatKindUnsignedInt4				6
#define pChannelFormatKindUnsignedInt2				7

class CFastaSW
{
public:
	CFastaSW();
	virtual ~CFastaSW();

	virtual void swMemcpyParameters(int matrix[32][32], int gapOpen,
			int gapExtend) {
	}
	;
	virtual void swMemcpyQuery(unsigned char* query, int qAlignedLen,
			int offset, int matrix[32][32], bool useQueryPrf) {
	}
	;
	cudaArray* swMallocArray(int width, int height, int type);
	virtual void swBindTextureToArrayQuad() {
	}
	;
	virtual void swBindQueryProfile() {
	}
	;
	virtual void swUnbindTextureQuad() {
	}
	;
	virtual void swUnbindQueryProfile() {
	}
	;
	virtual void InterRunGlobalDatabaseScanningQuadDynamic(int blknum,
			int threads, int numSeqs, bool useQueryPrf,
			cudaStream_t stream = 0) {
	}
	;
	virtual void InterRunGlobalDatabaseScanningQuad(int blknum, int threads,
			int numSeqs, int useQueryPrf, cudaStream_t stream = 0) {
	}
	;
	//transfer back results
	void transferResult(int numSeqs);

public:
	cudaArray *cudaInterSeqsQuad;
	DatabaseHash *cudaSeqHashQuad;

	//result buffers
	SeqEntry* cudaResult;
	SeqEntry* hostResult;
	int* cudaOverflows;

protected:
	struct cudaChannelFormatDesc uchar_channelDesc;
	struct cudaChannelFormatDesc uchar4_channelDesc;
	struct cudaChannelFormatDesc uint_channelDesc;
	struct cudaChannelFormatDesc uint2_channelDesc;
	struct cudaChannelFormatDesc uint4_channelDesc;
	struct cudaChannelFormatDesc char4_channelDesc;
	struct cudaChannelFormatDesc sint_channelDesc;
	struct cudaChannelFormatDesc sint4_channelDesc;
};
#endif
