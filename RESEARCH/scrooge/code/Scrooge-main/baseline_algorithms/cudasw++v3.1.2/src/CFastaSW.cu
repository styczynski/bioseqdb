#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CFastaSW.h"
#define CUERR do{ cudaError_t err;			\
	if ((err = cudaGetLastError()) != cudaSuccess) {		\
			int device;	\
			cudaGetDevice(&device);	\
  		fprintf(stderr, "CUDA error on GPU %d: %s : %s, line %d\n", device, cudaGetErrorString(err), __FILE__, __LINE__); }}while(0);

CFastaSW::CFastaSW() {
	uchar_channelDesc = cudaCreateChannelDesc<unsigned char>();
	uchar4_channelDesc = cudaCreateChannelDesc<uchar4>();
	uint_channelDesc = cudaCreateChannelDesc<unsigned int>();
	uint2_channelDesc = cudaCreateChannelDesc<uint2>();
	uint4_channelDesc = cudaCreateChannelDesc<uint4>();
	char4_channelDesc = cudaCreateChannelDesc<char4>();
	sint_channelDesc = cudaCreateChannelDesc<int>();
	sint4_channelDesc = cudaCreateChannelDesc<int4>();
}
CFastaSW::~CFastaSW() {
	//do nothing
}
cudaArray* CFastaSW::swMallocArray(int width, int height, int type) {
	cudaArray* cu_array;

	switch (type) {
	case pChannelFormatKindUnsignedChar:
		cudaMallocArray(&cu_array, &uchar_channelDesc, width, height);
		break;
	case pChannelFormatKindUnsignedChar4:
		cudaMallocArray(&cu_array, &uchar4_channelDesc, width, height);
		break;
	case pChannelFormatKindUnsigned:
		cudaMallocArray(&cu_array, &uint_channelDesc, width, height);
		break;
	case pChannelFormatKindUnsignedInt4:
		cudaMallocArray(&cu_array, &uint4_channelDesc, width, height);
		break;
	case pChannelFormatKindUnsignedInt2:
		cudaMallocArray(&cu_array, &uint2_channelDesc, width, height);
		break;
	case pChannelFormatKindChar4:
		cudaMallocArray(&cu_array, &char4_channelDesc, width, height);
		break;
	case pChannelFormatKindSignedInt:
		cudaMallocArray(&cu_array, &sint_channelDesc, width, height);
		break;
	case pChannelFormatKindSignedInt4:
		cudaMallocArray(&cu_array, &sint4_channelDesc, width, height);
		break;
	default:
		fprintf(stderr, "Unknown cuda array type\n");
		exit(-1);
	}

	CUERR
	return cu_array;

}

void CFastaSW::transferResult(int numSeqs) {
	cudaMemcpy(hostResult, cudaResult, numSeqs * sizeof(SeqEntry),
			cudaMemcpyDeviceToHost);
	CUERR
	;
}
