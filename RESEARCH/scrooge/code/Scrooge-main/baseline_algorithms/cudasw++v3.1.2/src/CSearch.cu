#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "CFastaFile.h"
#include "CFastaSW.h"
#include "CSearch.h"
#include <math.h>

int CSearch::gapOpen = DEFAULT_GAPO;
int CSearch::gapExtend = DEFAULT_GAPE;
int CSearch::matrix[32][32] = { { 0, 0 } };
int CSearch::seqLengthThreshold = min(2000, MAX_SEQ_LENGTH_THRESHOLD); /*initial value*/
#ifdef RT_DEBUG
double CSearch::globalRuntimes[2] = {1e-20, 1e-20};
#endif

CSearch::CSearch(CParams* params) {
	this->params = params;
	numSeqs = 0;
	numThreshold = 0;
	maxSeqLength = 0;
	totalAminoAcids = totalAminoAcidsAligned = 0;
	totalAminoAcidsThreshold = totalAminoAcidsThresholdAligned = 0;
	dbSeqsSize = 0;
	dbSeqs = 0;
	dbSeqsLen = 0;
	dbSeqsAlignedLen = 0;
	dbSeqsName = 0;
	sortedSeqs = 0;

	/*get the number of GPUs*/
	numGPUs = params->getNumGPUs();

	/*get the number of threads*/
	numThreads = params->getNumCPUThreads();

	/*thread information*/
	threadIDs.resize(numThreads);
	threadAttrs.resize(numThreads);
	for (size_t i = 0; i < threadAttrs.size(); ++i) {
		pthread_attr_init(&threadAttrs[i]);
	}
	threadParams.clear();

	/*initialize related vectors*/
	sseOverflowIndices = NULL;
}

CSearch::~CSearch() {
	int i;
	if (sortedSeqs) {
		delete[] sortedSeqs;
	}
	//free database
	if (dbSeqs) {
		for (i = 0; i < numSeqs; i++) {
			free(dbSeqs[i]);
			free(dbSeqsName[i]);
		}
		free(dbSeqs);
		free(dbSeqsName);
		free(dbSeqsLen);
		free(dbSeqsAlignedLen);
	}
	/*attributes*/
	for (size_t i = 0; i < threadAttrs.size(); ++i) {
		pthread_attr_destroy(&threadAttrs[i]);
	}
	threadAttrs.clear();

	/*parameters*/
	for (size_t i = 0; i < threadParams.size(); ++i) {
		delete threadParams[i];
	}
	threadParams.clear();
	threadIDs.clear();

	if (sseOverflowIndices) {
		delete[] sseOverflowIndices;
	}
}
void CSearch::run() {
	//read in the input sequence
	char* query = params->getQueryFile();
	char* mat = params->getSubMatrixName();
	char* db = params->getDbFile();

	//get the gap penalties
	gapOpen = params->getGapOpen();
	gapExtend = params->getGapExtend();

	fprintf(stderr, "/**********************************/\n");
	fprintf(stderr, "\tScoring matrix:\t\t\t%s\n", mat);
	fprintf(stderr, "\tGap Open penalty:\t\t%d\n", gapOpen);
	fprintf(stderr, "\tGap Extension penalty:\t\t%d\n", gapExtend);
	if(params->isQueryProfile()){
		fprintf(stderr, "\tQUERY PROFILE will be used\n");
	}else{
		fprintf(stderr, "\tQUERY PROFILE VARIANT will be used\n");
	}
	fprintf(stderr, "/**********************************/\n");

	//loading substitution matrix
	params->getMatrix(matrix);

	//calcualte the score limit for quad-lane SIMD computing*/
	int highScore = -1000;
	for (int i = 0; i < MATRIX_SIZE; ++i) {
		for (int j = 0; j < MATRIX_SIZE; ++j) {
			highScore = max(highScore, matrix[i][j]);
		}
	}
	scoreLimitQuad = 128 - highScore;

	//load database
	loaddb(db);

	//generate the object
	dbsearch(query);

	fprintf(stderr, "Finished!\n");
}

//for quick sort
int CSearch::compar_ascent(const void * va, const void * vb) {
	const SeqEntry* a = (const SeqEntry*) va;
	const SeqEntry* b = (const SeqEntry*) vb;

	if (a->value > b->value)
		return 1;
	if (a->value < b->value)
		return -1;

	return 0;
}
int CSearch::compar_descent(const void* va, const void* vb) {
	const SeqEntry* a = (const SeqEntry*) va;
	const SeqEntry* b = (const SeqEntry*) vb;

	if (a->value > b->value)
		return -1;
	if (a->value < b->value)
		return 1;

	return 0;
}
void CSearch::printResults(SeqEntry* hostResult, char** dbSeqsName, int numSeqs,
		int top, int scoreThreshold) {
	int i;

	//sorting the scores
	qsort(hostResult, numSeqs, sizeof(SeqEntry), compar_descent);

	//display the results. Here, nothing to do!
	for (i = 0; i < top; i++) {
		if (hostResult[i].value < scoreThreshold) {
			fprintf(stderr, "the score reaches the threshold (%d)\n", scoreThreshold);
			break;
		}
		if (i && i % 128 == 0) {
			fprintf(stderr, "press 'y' to quit and another key to continue\n");
			int c = getchar();
			if (c == 'y' || c == 'Y')
				break;

		}
		fprintf(stderr, "score: %d -- %s\n", hostResult[i].value,
				dbSeqsName[hostResult[i].idx]);
	}
}
float CSearch::getCPUFrequency() {
	char buffer[4096];
	size_t nbytes;
	float frequency = 2.4;
	const char fileName[] = "/proc/cpuinfo";
	FILE* file = fopen(fileName, "r");
	if (!file) {
		fprintf(stderr, "Failed to get the CPU frequency by opening file %s\n",
				fileName);
		fprintf(stderr, "Will assume the CPU frequency to be %f GHz\n",
				frequency);
		return frequency;
	}
	nbytes = fread(buffer, 1, sizeof(buffer) - 1, file);
	fclose(file);

	if (nbytes == 0) {
		fprintf(stderr,
				"Failed to read the file %s and Will use the default frequency %f GHz\n",
				fileName, frequency);
		fclose(file);
		return frequency;
	}
	buffer[nbytes] = '\0';

	/*locate the line that starts with "GHz"*/
	char* match = strstr(buffer, "cpu MHz");
	if (match == NULL) {
		fprintf(stderr,
				"Failed to find the frequency field. Will use the default %f GHz\n",
				frequency);
		fclose(file);
		return frequency;
	}
	/*parse the line to extract the clock speed*/
	sscanf(match, "cpu MHz  :  %f", &frequency);
	frequency /= 1000;

	/*read the cpuinfo_max_freq file*/
	file = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
	if (!file) {
		return frequency;
	}
	/*read the file*/
	nbytes = fread(buffer, 1, sizeof(buffer) - 1, file);
	if (nbytes == 0 || nbytes >= sizeof(buffer)) {
		fclose(file);
		return frequency;
	}
	buffer[nbytes] = '\0';

	float maxFreq = 0;
	sscanf(buffer, "%f", &maxFreq);
	maxFreq /= 1000000;

	if (maxFreq > 4.5 || maxFreq < 1.0) {
		return frequency;
	}
	frequency = max(frequency, maxFreq);

	return frequency;
}
void CSearch::calcThreshold(double cfreq, double gfreq, int cnumCores,
		int gnumSMX) {
	/*cfreq: CPU frequency
	 * gfreq: GPU core frequency
	 * cnumCores: number of CPU cores used
	 * gnumSMX: number of GPU streaming multiprocessors
	 */
	numThreshold = 0;
	seqLengthThreshold = 0;
	totalAminoAcidsThreshold = totalAminoAcidsThresholdAligned = 0;
	if (numGPUs == 0) {
		/*if GPUs are not used*/
		return;
	}
	fprintf(stderr, "[%s] %g %g %d %d\n", __FUNCTION__, cfreq, gfreq, cnumCores,
			gnumSMX);
	
	/*calculate the ratio of GPUs*/
	const double CGPU_COEFFICIENT = params->isQueryProfile() ? 3.2 : 5.0;
	double ratio;
#ifdef DISABLE_CPU_THREADS
	ratio = 1.0;
	/*check the maximum sequence length*/
	if(maxSeqLength > MAX_SEQ_LENGTH_THRESHOLD){
		fprintf(stderr, "All sequence lengths MUST be <= %d (user configurable), when disable CPU threads\n", MAX_SEQ_LENGTH_THRESHOLD);
		exit(-1);
	}
#else
	ratio = gfreq * gnumSMX * CGPU_COEFFICIENT / (cfreq * cnumCores);
	ratio = ratio / (ratio + 1);
	if (ratio < 0.01){
		ratio = 0.01;
	}
#endif

	/*calculate the number of residues assigned to GPUs*/
	size_t gpuTotalResidues = (size_t) (totalAminoAcidsAligned * ratio);

	/*calculate the number of sequences assigned to GPUs*/
	if (dbSeqsAlignedLen[sortedSeqs[0].idx] <= MAX_SEQ_LENGTH_THRESHOLD) {
		for (numThreshold = 0; numThreshold < numSeqs; ++numThreshold) {
			int index = sortedSeqs[numThreshold].idx;
			int length = dbSeqsAlignedLen[index];
			if (length > MAX_SEQ_LENGTH_THRESHOLD
					|| totalAminoAcidsThresholdAligned >= gpuTotalResidues) {
				break;
			}
			totalAminoAcidsThreshold += dbSeqsLen[index];
			totalAminoAcidsThresholdAligned += length;
		}
		seqLengthThreshold = dbSeqsAlignedLen[sortedSeqs[numThreshold - 1].idx];
	}
	fprintf(stderr, "[%s]%g %d\n", __FUNCTION__, ratio,
			seqLengthThreshold);
	if (numThreshold == 0) {
		fprintf(stderr, "No jobs are assigned to GPUs due to long sequences\n");
	}
}
void CSearch::createIntraDBSSE() {
	//fprintf(stderr, "Load long sequences for SSE computing\n");
	size_t numResiduesAligned, numResiduesPerBlock, numSeqsPerBlocks;

	/*create aligners*/
	sseAligners.resize(numThreads);
	for (size_t i = 0; i < sseAligners.size(); ++i) {
		sseAligners[i] = new Aligner(matrix, gapOpen, gapExtend, 0x10000, scoreLimitQuad);
	}

	/*create overflow buffer*/
	sseOverflowIndices = new int2[numSeqs];
	if (!sseOverflowIndices) {
		fprintf(stderr, "Memory allocation failed in function %s at line %d\n",
				__FUNCTION__, __LINE__);
	}

	/*if no sequence is assigned to SSE*/
	if (numThreshold == numSeqs) {
		sseSequences.clear();
		sseSeqOffsets.clear();
		sseNumSequences.clear();
		sseCNumSequences.clear();
		return;
	}

	/*calculate the total length of all sequences*/
	numResiduesAligned = totalAminoAcidsAligned
			- totalAminoAcidsThresholdAligned;
	numResiduesPerBlock = (numResiduesAligned + numThreads - 1) / numThreads;
	numSeqsPerBlocks = (numSeqs - numThreshold + numThreads - 1) / numThreads; //assume they have equal lengths

	/*allocate sequences for each block*/
	size_t nsequence = 0, cnsequence = 0, nresidues = 0, offset = 0;
	size_t sequenceSize = numResiduesPerBlock;
	size_t seqOffsetSize = numSeqsPerBlocks;
	uint8_t* sequence = new uint8_t[sequenceSize];
	int32_t* seqOffset = new int32_t[seqOffsetSize];

	/*push the first element*/
	sseCNumSequences.push_back(0);
	for (size_t i = numThreshold; i < numSeqs; ++i) {
		int index = sortedSeqs[i].idx;
		int length = dbSeqsAlignedLen[index];

		/*do we need to save the data*/
		if (nresidues >= numResiduesPerBlock) {
			/*save the old batch*/
			sseSequences.push_back(sequence);
			sseSeqOffsets.push_back(seqOffset);
			sseNumSequences.push_back(nsequence);
			sseCNumSequences.push_back(cnsequence);

			nresidues = 0;
			nsequence = 0;
			offset = 0;
			sequenceSize = numResiduesPerBlock;
			seqOffsetSize = numSeqsPerBlocks;
			sequence = new uint8_t[sequenceSize];
			seqOffset = new int32_t[seqOffsetSize];
		}
		/*increase the total number of residues*/
		nresidues += length;

		/*record the starting position of the sequence*/
		if (nsequence >= seqOffsetSize) {
			seqOffsetSize *= 2;
			int32_t* buffer = new int32_t[seqOffsetSize];
			if (!buffer) {
				fprintf(stderr, "Memory allocation failed\n");
				exit(-1);
			}
			memcpy(buffer, seqOffset, nsequence * sizeof(seqOffset[0]));
			delete[] seqOffset;
			seqOffset = buffer;
			//fprintf(stderr, "seqOffsetSize: %d\n", seqOffsetSize);
		}
		seqOffset[nsequence++] = offset;
		++cnsequence;

		/*copy the residues*/
		if (offset + length + 1 >= sequenceSize) {
			sequenceSize *= 2;
			uint8_t *buffer = new uint8_t[sequenceSize];
			if (!buffer) {
				fprintf(stderr, "Memory allocation failed\n");
				exit(-1);
			}
			memcpy(buffer, sequence, offset);
			delete[] sequence;
			sequence = buffer;
			//fprintf(stderr, "sequenceSize: %d\n", sequenceSize);
		}
		memcpy(sequence + offset, dbSeqs[index], length);
		offset += length;
		sequence[offset++] = 0; /*delimiter between sequences*/
	}
	if (nresidues > 0) {
		/*save the old batch*/
		sseSequences.push_back(sequence);
		sseSeqOffsets.push_back(seqOffset);
		sseNumSequences.push_back(nsequence);
		sseCNumSequences.push_back(cnsequence);
	}
}
/*recompute overflows*/
void CSearch::launchOverflowSSE(SeqEntry *hostResult, uint8_t* sseQuery,
		int sseQueryLength) {
	size_t numOverflows = 0;
	size_t totalNumResidues = 0;

	//fprintf(stderr, "Recomputing alignments that overflowed\n");

	/*getting the indices of sequences that have overflowed on GPUs*/
	SeqEntry* result = hostResult;
	for (int i = 0; i < numThreshold; ++i) {
		int score = result->value;
		//if (score >= scoreLimitQuad) {
		if (score >= 127) {	/*on GPUs, we have used saturation additions/subtractions*/
			/*an overflow occurs*/
			sseOverflowIndices[numOverflows++] = make_int2(i, result->idx);

			/*calculate the overall number of residues*/
			totalNumResidues += dbSeqsAlignedLen[result->idx];
		}
		++result;
	}
	fprintf(stderr, "#alignments that overflowed: %ld\n", numOverflows);
	if (numOverflows == 0) {
		return;
	}

	/*divide the sequences over the set of threads*/
	size_t numSeqsPerBlocks = (numOverflows + numThreads - 1) / numThreads; //assume they have equal lengths
	size_t numResiduesPerBlock = (totalNumResidues + numThreads - 1)
			/ numThreads;

	/*allocate sequences for each block*/
	size_t nsequence = 0, cnsequence = 0, nresidues = 0, offset = 0;
	size_t sequenceSize = numResiduesPerBlock;
	size_t seqOffsetSize = numSeqsPerBlocks;
	uint8_t* sequence = new uint8_t[sequenceSize];
	int32_t* seqOffset = new int32_t[seqOffsetSize];
	if (!sequence || !seqOffset) {
		fprintf(stderr, "Memory allocation failed in function %s at line %d\n",
				__FUNCTION__, __LINE__);
		exit(-1);
	}
	/*push the first element*/
	sseCNumSequencesOverflow.push_back(0);
	for (size_t i = 0; i < numOverflows; ++i) {
		int index = sseOverflowIndices[i].y; /*get the input index of the sequence*/
		int length = dbSeqsAlignedLen[index]; /*get the aligned sequence length*/

		/*do we need to save the data*/
		if (nresidues >= numResiduesPerBlock) {
			/*save the old batch*/
			sseSequencesOverflow.push_back(sequence);
			sseSeqOffsetsOverflow.push_back(seqOffset);
			sseNumSequencesOverflow.push_back(nsequence);
			sseCNumSequencesOverflow.push_back(cnsequence);

			nresidues = 0;
			nsequence = 0;
			offset = 0;
			sequenceSize = numResiduesPerBlock;
			seqOffsetSize = numSeqsPerBlocks;
			sequence = new uint8_t[sequenceSize];
			seqOffset = new int32_t[seqOffsetSize];
		}
		/*increase the total number of residues*/
		nresidues += length;

		/*record the starting position of the sequence*/
		if (nsequence >= seqOffsetSize) {
			seqOffsetSize += 256;
			int32_t* buffer = new int32_t[seqOffsetSize];
			if (!buffer) {
				fprintf(stderr, "Memory allocation failed\n");
				exit(-1);
			}
			memcpy(buffer, seqOffset, nsequence * sizeof(seqOffset[0]));
			delete[] seqOffset;
			seqOffset = buffer;
		}
		seqOffset[nsequence++] = offset;
		++cnsequence;

		/*copy the residues*/
		if (offset + length + 1 >= sequenceSize) {
			sequenceSize += 16 * maxSeqLength;
			uint8_t *buffer = new uint8_t[sequenceSize];
			if (!buffer) {
				fprintf(stderr, "Memory allocation failed\n");
				exit(-1);
			}
			memcpy(buffer, sequence, offset);
			delete[] sequence;
			sequence = buffer;
		}
		memcpy(sequence + offset, dbSeqs[index], length);
		offset += length;
		sequence[offset++] = 0; /*delimiter between sequences*/
	}
	if (nresidues > 0) {
		/*save the old batch*/
		sseSequencesOverflow.push_back(sequence);
		sseSeqOffsetsOverflow.push_back(seqOffset);
		sseNumSequencesOverflow.push_back(nsequence);
		sseCNumSequencesOverflow.push_back(cnsequence);
	}

	/*allocate thread parameters*/
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		delete threadParams[tid];
	}
	threadParams.clear();

	/*get the number of sequence blocks*/
	threadParams.resize(sseSequencesOverflow.size());

	/*allocate result vectors*/
	SeqEntry* sseResults = new SeqEntry[numOverflows];
	if (!sseResults) {
		fprintf(stderr, "Memory allocation failed at line %d in function %s\n",
				__LINE__, __FUNCTION__);
		exit(-1);
	}
	/*create thread parameters*/
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		threadParams[tid] = new SSEThreadParams(tid, sseQuery, sseQueryLength,
				sseAligners[tid], sseSequencesOverflow[tid], sseSeqOffsetsOverflow[tid],
				sseNumSequencesOverflow[tid], sseResults + sseCNumSequencesOverflow[tid]);
	}

	/*create a batch of threads*/
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		if (threadParams[tid]) {
			pthread_create(&threadIDs[tid], &threadAttrs[tid],
					sseThreadLocalAlignDual, threadParams[tid]);
		}
	}
	/*wait for the completion*/
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		pthread_join(threadIDs[tid], NULL);
	}

	/*re-locate the results*/
	result = hostResult;
	for (int i = 0; i < numOverflows; ++i) {
		result[sseOverflowIndices[i].x].value = sseResults[i].value;
	}

	/*release resources*/
	delete[] sseResults;
	sseResults = NULL;

	/*release sequence resources*/
	for (size_t i = 0; i < sseSequencesOverflow.size(); ++i) {
		if (sseSequencesOverflow[i]) {
			delete[] sseSequencesOverflow[i];
			sseSequencesOverflow[i] = NULL;
		}
	}
	sseSequencesOverflow.clear();

	for (size_t i = 0; i < sseSeqOffsetsOverflow.size(); ++i) {
		if (sseSeqOffsetsOverflow[i]) {
			delete[] sseSeqOffsetsOverflow[i];
			sseSeqOffsetsOverflow[i] = NULL;
		}
	}
	sseSeqOffsetsOverflow.clear();
	sseNumSequencesOverflow.clear();
	sseCNumSequencesOverflow.clear();
}

/*thread function for intra-task parallelization*/
void CSearch::launchIntraSSE(SeqEntry* hostResult, uint8_t* sseQuery,
		int sseQueryLength) {

	/*if no sequence is assigned to SSE*/
	if (numThreshold == numSeqs) {
		return;
	}

#ifdef RT_DEBUG
	double rtstart;
	CParams::getSysTime(&rtstart);
#endif

	/*allocate thread parameters*/
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		delete threadParams[tid];
	}
	threadParams.clear();
	threadParams.resize(sseSequences.size());

	SeqEntry *results = hostResult + numThreshold;
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		/*create parameters for each thread*/
		threadParams[tid] = new SSEThreadParams(tid, sseQuery, sseQueryLength,
				sseAligners[tid], sseSequences[tid], sseSeqOffsets[tid],
				sseNumSequences[tid], results + sseCNumSequences[tid]);
#ifdef RT_DEBUG
		threadParams[tid]->runtime = rtstart;
#endif
	}
	/*create a batch of threads*/
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		pthread_create(&threadIDs[tid], &threadAttrs[tid], sseThreadLocalAlignQuad,
				threadParams[tid]);
	}
}
void CSearch::waitIntraSSE() {
	/*if no sequence is assigned to SSE*/
	if (numThreshold == numSeqs) {
		return;
	}
	for (size_t tid = 0; tid < threadParams.size(); ++tid) {
		pthread_join(threadIDs[tid], NULL);
	}
#ifdef RT_DEBUG
	double runtime = 0;
	for(size_t tid = 0; tid < threadParams.size(); ++tid){
		runtime = max(threadParams[tid]->runtime, runtime);
	}
	globalRuntimes[0] = runtime;
#endif
}
void* CSearch::sseThreadLocalAlignQuad(void* arg) {
#ifdef RT_DEBUG
	double end;
#endif
	SSEThreadParams* params = static_cast<SSEThreadParams*>(arg);

	/*perform alignment for the batch of reads*/
	if (params->numSequences > 0) {
		params->aligner->lalignScoreQuad(params->query, params->queryLength,
				params->sequences, params->seqOffsets, params->numSequences,
				params->scorePtr);
	}
#ifdef RT_DEBUG
	CParams::getSysTime(&end);
	params->runtime = end - params->runtime;
#endif
	return 0;
}
void* CSearch::sseThreadLocalAlignDual(void* arg) {
	SSEThreadParams* params = static_cast<SSEThreadParams*>(arg);

	/*perform alignment for the batch of reads*/
	if (params->numSequences > 0) {
		params->aligner->lalignScoreDual(params->query, params->queryLength,
				params->sequences, params->seqOffsets, params->numSequences,
				params->scorePtr);
	}

	return 0;
}
