#ifndef _C_SEARCH_H
#define _C_SEARCH_H
#include "CFastaSW.h"
#include "CParams.h"
#include "Aligner.h"

class SSEThreadParams
{
public:
	SSEThreadParams(int argtid, uint8_t* argQuery, int argQueryLength,
			Aligner* argAligner, uint8_t* argSequences, int32_t * argSeqOffsets,
			size_t argNumSequences, SeqEntry* argScorePtr) {
		tid = argtid;
		query = argQuery;
		queryLength = argQueryLength;
		aligner = argAligner;
		sequences = argSequences;
		seqOffsets = argSeqOffsets;
		numSequences = argNumSequences;
		scorePtr = argScorePtr;
#ifdef RT_DEBUG
		runtime = 1e-20;
#endif
	}

	int tid; /*thread id*/
	uint8_t* query; /*query sequence*/
	int queryLength; /*query sequence length*/
	Aligner* aligner; /*aligner*/
	uint8_t* sequences; /*residues*/
	int32_t* seqOffsets; /*offset of each sequence in the buffer*/
	size_t numSequences; /*the number of sequences assigned to this thread*/
	SeqEntry* scorePtr; /*the alignment result buffer address*/

#ifdef RT_DEBUG
	double runtime;
#endif
};
class CSearch
{
public:
	CSearch(CParams* params);
	virtual ~CSearch();

	void run();

protected:
	float getCPUFrequency();
	void calcThreshold(double cfreq, double gfreq, int cnumCores, int gnumSMX);

	/*SSE-based computing*/
	void createIntraDBSSE(); /*load database*/
	void launchIntraSSE(SeqEntry* hostResult, uint8_t* query, int queryLength); /*launch intra-task threads*/
	void waitIntraSSE(); /*wait for intra-task threads*/
	void launchOverflowSSE(SeqEntry* hostResult, uint8_t* query,
			int queryLength); /*SSE-based computing for overflowed sequences*/

	/*virtual member functions*/
	virtual int loaddb(char* dbFile) {
		return 1;
	}
	virtual int dbsearch(char* query) {
		return 0;
	}
	virtual void printResults(SeqEntry* hostResult, char** dbSeqsName,
			int numSeqs, int top, int scoreThreshold);

	//parameters
	CParams* params;

	/*database information*/
	int dbSeqsSize;
	uint8_t** dbSeqs;
	char** dbSeqsName;
	int* dbSeqsLen;
	int* dbSeqsAlignedLen;
	int numSeqs;
	int numThreshold;
	int numThresholdQuad;
	int scoreLimitQuad;
	int maxSeqLength;
	size_t totalAminoAcids, totalAminoAcidsAligned;
	size_t totalAminoAcidsThreshold, totalAminoAcidsThresholdAligned;
	int avgLengthThreshold;
	int stdLengthThreshold;
	SeqEntry* sortedSeqs;

	/*for GPU computing*/
	int numGPUs;
	/*for SSE computing*/
	size_t numThreads;
	uint8_t* sseQuery;
	int sseQueryLength;
	vector<Aligner*> sseAligners;
	vector<uint8_t*> sseSequences;
	vector<int32_t*> sseSeqOffsets;
	vector<size_t> sseNumSequences;
	vector<size_t> sseCNumSequences; /*cumulative number of sequences*/
	int2* sseOverflowIndices; /*for overflow computing*/
  vector<uint8_t*> sseSequencesOverflow;
  vector<int32_t*> sseSeqOffsetsOverflow;
  vector<size_t> sseNumSequencesOverflow;
  vector<size_t> sseCNumSequencesOverflow; /*cumulative number of sequences*/
	vector<pthread_t> threadIDs;
	vector<pthread_attr_t> threadAttrs;
	vector<SSEThreadParams*> threadParams;

	/*static member variables and functions*/
	static int matrix[32][32]; //scoring matrix
	static int gapOpen; //gap opening penalty
	static int gapExtend; //gap extension penalty

	//sequence length threshold
	static int seqLengthThreshold;
	static int compar_ascent(const void * va, const void * vb);
	static int compar_descent(const void* va, const void* vb);
	static void* sseThreadLocalAlignQuad(void* arg);
	static void* sseThreadLocalAlignDual(void* arg);

#ifdef RT_DEBUG
	static double globalRuntimes[2];
#endif

};

struct TaskPlan
{
	CFastaSW* cudasw;
	//device
	int device;
	int threads;
	GPUInfo* info;
	cudaStream_t stream;
	int l2CacheSize;
	int procsPerPass;
	int interBlocks;
	int numSeqsQuad;
	//query sequence
	uint8_t* query;
	int qAlignedLen;
	//on the host memory
	int cx, cy;
	int maxSeqLength;
	int index;
	int cudaInterTexWidth;
	int cudaInterTexHeight;
	int hostResultPos;
	SeqEntry* hostResult;
	SeqEntry* cudaHostResult;
	SeqEntry* globalHostResult;
	void* interHostSeqArray;
	DatabaseHash * hostSeqHash;

	//on the device memory
	int numSeqs;
	int interSeqNo;
	void* cudaInterSeqs;
	int divergence;
	int useQueryPrf;
};

#endif

