#include "Defs.h"
#include "CFastaFile.h"
#include "CFastaSWScalar.h"
#include "CSearchScalar.h"

CSearchScalar::CSearchScalar(CParams* params) :
		CSearch(params) {
}

CSearchScalar::~CSearchScalar() {
}
int CSearchScalar::loaddb(char* dbFile) {
	int i;
	CFastaFile *dbLib;
	int seqLen;
	int seqAlignedLen;
	uint8_t* seq;

	fprintf(stderr,
			"Loading database sequences from file into host memory...\n");

#define INIT_SIZE 		819200
	numSeqs = 0;
	numThreshold = 0;
	maxSeqLength = 0;
	totalAminoAcids = 0;
	dbSeqsSize = INIT_SIZE;
	dbSeqs = (uint8_t**) malloc(sizeof(uint8_t*) * dbSeqsSize);
	dbSeqsLen = (int*) malloc(sizeof(int) * dbSeqsSize);
	dbSeqsAlignedLen = (int*) malloc(sizeof(int) * dbSeqsSize);
	dbSeqsName = (char**) malloc(sizeof(char*) * dbSeqsSize);

	//open the database
	dbLib = new CFastaFile;
	dbLib->open(dbFile);

	/*load all subject sequences*/
	seq = dbLib->nextSeq(&seqLen, &seqAlignedLen, DB_SEQ_LENGTH_ALIGNED);
	while (seqLen > 0) {

		if (numSeqs >= dbSeqsSize) {
			dbSeqsSize *= 2;
			dbSeqs = (uint8_t**) realloc(dbSeqs, sizeof(uint8_t*) * dbSeqsSize);
			dbSeqsName = (char**) realloc(dbSeqsName,
					sizeof(char*) * dbSeqsSize);
			dbSeqsLen = (int*) realloc(dbSeqsLen, sizeof(int) * dbSeqsSize);
			dbSeqsAlignedLen = (int*) realloc(dbSeqsAlignedLen,
					sizeof(int) * dbSeqsSize);
			if (dbSeqs == NULL || dbSeqsLen == NULL || dbSeqsName == NULL
					|| dbSeqsAlignedLen == NULL) {
				fprintf(stderr, "no memory space for database sequences\n");
				return 0;
			}
		}

		dbSeqs[numSeqs] = (uint8_t*) malloc(
				sizeof(uint8_t) * (seqAlignedLen + 1));
		if (dbSeqs[numSeqs] == NULL) {
			fprintf(stderr, "no memory space available for the database\n");
			return 0;
		}
		//save sequence name;
		dbSeqsName[numSeqs] = strdup((char*) dbLib->getSeqName());
		//save sequence length
		dbSeqsLen[numSeqs] = seqLen;
		dbSeqsAlignedLen[numSeqs] = seqAlignedLen;
		//save sequence symbols
		memcpy(dbSeqs[numSeqs], seq, sizeof(uint8_t) * seqAlignedLen);

		/*statistic information*/
		++numSeqs; /*the number of sequences*/
		totalAminoAcids += seqLen; /*the number of residues*/
		totalAminoAcidsAligned += seqAlignedLen; /*the number of aligned residues*/

		/*calculate the maximal sequence length*/
		if (maxSeqLength < seqLen) {
			maxSeqLength = seqLen;
		}

		/*load the next sequence*/
		seq = dbLib->nextSeq(&seqLen, &seqAlignedLen, DB_SEQ_LENGTH_ALIGNED); /*aligned to be multiples of 4*/
	}
	if (numSeqs == 0) {
		fprintf(stderr, "The database file is empty");
		return 0;
	}

	/*sort the sequence in the order of length*/
	sortedSeqs = new SeqEntry[numSeqs];
	if (!sortedSeqs) {
		fprintf(stderr, "Memory allocation failed at line %d in function %s\n",
				__LINE__, __FUNCTION__);
		return 0;
	}
	for (i = 0; i < numSeqs; i++) {
		sortedSeqs[i].idx = i;
		sortedSeqs[i].value = dbSeqsAlignedLen[i];
	}
	qsort(sortedSeqs, numSeqs, sizeof(SeqEntry), compar_ascent);

	/*calculate the threshold for CPUs and GPUs*/
	GPUInfo* gpuInfo = pGetGPUInfo();

	/*determine the number of SMs in the GPUs used*/
	if(params->isUseSingleGPU()){
		calcThreshold(getCPUFrequency(),
			pGetClockRate(gpuInfo, params->getSingleGPUID()), numThreads,
					pGetMultiProcessorCount(gpuInfo, params->getSingleGPUID()));
	}else{
		calcThreshold(getCPUFrequency(),
			pGetClockRate(gpuInfo, 0), numThreads,
					params->getNumGPUs() * pGetMultiProcessorCount(gpuInfo, 0));
	}

	/*recalculate other values related to the threshold*/
	numThresholdQuad = (numThreshold + 3) >> 2;
	if(numSeqs > 0) fprintf(stderr, "overall mean: %ld\n", totalAminoAcids / numSeqs);

	/*calculate the average length*/
	double mean = 0, variance = 0;
	if (numThreshold > 0) {
		mean = ((double) totalAminoAcidsThresholdAligned) / numThreshold;
		variance = 0;
		for (int i = 0; i < numThreshold; ++i) {
			int length = dbSeqsAlignedLen[sortedSeqs[i].idx];
			variance += (length - mean) * (length - mean);
		}
		variance = sqrt(variance / numThreshold);
	}
	avgLengthThreshold = (int) (mean + 0.49);
	stdLengthThreshold = (int) (variance + 0.49);
	fprintf(stderr, "mean %d deviation %d\n",
			avgLengthThreshold, stdLengthThreshold);

	//release database structure
	dbLib->close();
	delete dbLib;

	return 1;
}

int CSearchScalar::dbsearch(char*queryFile) {
	CFastaFile* queryLib = new CFastaFile;
	CFastaSW* cudasw = new CFastaSWScalar;

	/*load parameters onto the device*/
	cudasw->swMemcpyParameters(matrix, gapOpen, gapExtend);

	/*calculate the runtime information*/
	GPUInfo* gpuInfo = pGetGPUInfo();
	int numSMXProcessors = pGetMultiProcessorCount(gpuInfo,
			params->getSingleGPUID()); /*get the number of SMs*/
	int l2CacheSize = pGetL2CacheSize(gpuInfo, params->getSingleGPUID());

	/*compute the total number of thread blocks for inter-task parallelization*/
	int interThreads = 64; /*the number of threads per thread block*/
	int interBlocks = (numThresholdQuad + interThreads - 1) / interThreads;

	/*calculate the number of thread blocks in each iteration*/
	int procsPerPass = numSMXProcessors * 2048 * 2 / interThreads;
	/*fprintf(stderr, "threads: %d interBlocks %d procsPerPass %d numSMXProcessors %d \n", interThreads,
	 interBlocks, procsPerPass, numSMXProcessors);*/

	/*********************************Stage 1***************************/
	DatabaseHash* hashQuad = (DatabaseHash*) pMallocHost(
			sizeof(DatabaseHash) * numSeqs);
	Database<uint4>* interDBQuad = createInterDBQuad(cudasw, hashQuad);

	createIntraDBSSE(); /*For SSE-based computing (only for long sequences)*/

	/*********************************Stage 2***************************/
	loadInterDBQuad(cudasw, hashQuad, interDBQuad);

	/*********************************Stage 3***************************/
	/*allocate result buffers for the host and the device*/
	cudasw->hostResult = (SeqEntry*) pMallocHost(sizeof(SeqEntry) * numSeqs);
	for (int i = 0; i < numSeqs; i++) {
		cudasw->hostResult[i].idx = sortedSeqs[i].idx;
		cudasw->hostResult[i].value = 65536;
	}
	cudasw->cudaResult = (SeqEntry*) pMallocPitch(sizeof(SeqEntry),
			(numSeqs >> 2) << 2, 1, 0); /*aligned to 4*/

	/*initialize the result buffer*/
	pMemcpy(cudasw->cudaResult, cudasw->hostResult, numSeqs * sizeof(SeqEntry),
			pMemcpyHostToDevice);

	fprintf(stderr, "Loading database successfully\n");
	fprintf(stderr, "numSeqs: %d numThreshold: %d\n", numSeqs, numThreshold);
	fprintf(stderr,
			"maxSeqLength: %d totalAminoAcidsThreshod: %ld totalAminoAcids: %ld\n",
			maxSeqLength, totalAminoAcidsThreshold, totalAminoAcids);

	fprintf(stderr,
			"******************************\n******************************\n");

	int qlen, qAlignedLen;
	uint8_t* query;
	double diff, gcups;
	double divergence = 0;
	double start, end;
	bool useQueryPrf = params->isQueryProfile();

	/*calculate divergence*/
	if (avgLengthThreshold > 0) {
		divergence = ((double) stdLengthThreshold) * 100 / avgLengthThreshold;
	}

	//open the query file
	queryLib->open(queryFile);
	//only load the first query sequence
	query = queryLib->nextSeq(&qlen, &qAlignedLen, QUERY_SEQ_LENGTH_ALIGNED);
	if (qlen == 0) {
		fprintf(stderr, "query file is empty!");
		goto out;
	}
	while (qlen > 0) {

		//get the system time
		CParams::getSysTime(&start);

		/*launch CPU threads*/
		launchIntraSSE(cudasw->hostResult, query, qlen);

		/*determine whether to use profile*/
#ifdef RT_DEBUG
	double rtstart, rtend;
	CParams::getSysTime(&rtstart);
#endif
		cudasw->swMemcpyQuery(query, qAlignedLen, sizeof(uint8_t), matrix,
				useQueryPrf); //copy the query sequence from host to GPU, indexing from 1

		/*bind query profile*/
		cudasw->swBindQueryProfile();

		/*invoke the kernel*/
		if (divergence <= DB_MAX_DIVERGENCE) {
      cudasw->InterRunGlobalDatabaseScanningQuad(interBlocks, interThreads,
            numThresholdQuad, useQueryPrf);
		} else {
			cudasw->InterRunGlobalDatabaseScanningQuadDynamic(
					min(interBlocks, procsPerPass), interThreads,
					numThresholdQuad, useQueryPrf);
		}
		cudasw->swUnbindQueryProfile();
		cudasw->transferResult(numThreshold); /*transfer back the results*/
#ifdef RT_DEBUG
	CParams::getSysTime(&rtend);
	CSearch::globalRuntimes[1] = rtend - rtstart;
#endif
		waitIntraSSE(); /*wait for the completion of SSE threads*/
#ifndef DISABLE_CPU_THREADS
		launchOverflowSSE(cudasw->hostResult, query, qlen); /*recompute the sequences*/
#endif

		//get the system time
		CParams::getSysTime(&end);
#ifdef RT_DEBUG
		/*performance of CPU and GPUs*/
		gcups = ((double) totalAminoAcids - totalAminoAcidsThreshold) / 1000000000.0f;
    gcups *= qlen;
    gcups /= globalRuntimes[0];
		/*performanc of CPU*/
		printf("%g\t%g\t", globalRuntimes[0], gcups); 

    gcups = ((double)totalAminoAcidsThreshold) / 1000000000.0f;
    gcups *= qlen;
    gcups /= globalRuntimes[1];
		/*performance of GPU*/
		printf("%g\t%g\n", globalRuntimes[1], gcups); 
#endif

		/*overall performance*/
		diff = end - start;
		gcups = ((double) totalAminoAcids) / 1000000000.0f;
		gcups *= qlen;
		gcups /= diff;

		fprintf(stderr, "query:%s\n", queryLib->getSeqName());
		fprintf(stderr, "Length: %d --- time: %g (s) and GCUPS: %g\n",
				qlen, diff, gcups);

		//display results
		int top =
				numSeqs > params->getTopScoresNum() ?
						params->getTopScoresNum() : numSeqs;
		int scoreThreshold = params->getScoreThreshold();
		fprintf(stderr, "----------Display the top %d ----------\n", top);
		printResults(cudasw->hostResult, dbSeqsName, numSeqs, top,
				scoreThreshold);

		/*VERBOSE*/
		//printf("%d\t%g\t%g\n", qlen, diff, gcups);

		//load the next query sequence
		query = queryLib->nextSeq(&qlen, &qAlignedLen,
				QUERY_SEQ_LENGTH_ALIGNED);
		if (qlen == 0) {
			fprintf(stderr, "Reaching the end of the query file!\n");
			break;
		}
		/*re-initialize the host buffer*/
  	for (int i = 0; i < numSeqs; i++) {
    	cudasw->hostResult[i].idx = sortedSeqs[i].idx;
    	cudasw->hostResult[i].value = 65536;
  	}
	}
	out:
	/*release host resources*/
	unloadInterDBQuad(cudasw);
	if (hashQuad){
		pFreeHost(hashQuad);
	}
	if (interDBQuad) {
		pFreeHost(interDBQuad->array);
		delete interDBQuad;
	}

	/*release results*/
	pFreeHost(cudasw->hostResult);
	pFree(cudasw->cudaResult);

	delete queryLib;
	delete cudasw;

	/*aligners*/
	for (size_t i = 0; i < sseAligners.size(); ++i) {
		delete sseAligners[i];
	}
	sseAligners.clear();

	return 0;
}
/**************Inter-task quad-lane vector computing***************************/
Database<uint4>* CSearchScalar::createInterDBQuad(CFastaSW* cudasw,
		DatabaseHash* hashQuad, int initWidth) {

	size_t totalNumBases = 0, totalNumNulls = 0;
	/*compute the height of the CUDA array*/
	int widthQuad = initWidth;
	int heightQuad = 0;
	int n = 0;
	bool done;
	do {
		n = 0;
		heightQuad = 0;
		done = true;
		while (n < numThreshold) {
			if (n + widthQuad * 4 < numThreshold) {
				/*a new row*/
				n += widthQuad * 4; /*four sequences per texture element*/
				heightQuad += dbSeqsAlignedLen[sortedSeqs[n - 1].idx] / 4 + 1; /*four residues per packed sequence*/
				continue;
			} else {
				n = numThreshold;
				heightQuad += dbSeqsAlignedLen[sortedSeqs[n - 1].idx] / 4 + 1; //four residues per texture element
			}
		}
		if (++heightQuad > 32768) {
			if (widthQuad == 65536) {
				fprintf(stderr,
						"No availabe device memory space for the database (width %d height: %d)\n",
						widthQuad, heightQuad);
				exit(-1);
			}
			widthQuad = max(widthQuad + 1024, 65536);
			;
			done = false;
		}
	} while (!done);
	if(heightQuad == 0) heightQuad = 1;
	/*fprintf(stderr,
			"Loading inter-task quad-lane SIMD computing database: width:%d height:%d size:%ld (MB)\n",
			widthQuad, heightQuad,
			sizeof(uint4) * widthQuad * heightQuad / 0x100000);*/

	//fill the array with the sorted sequences
	int cx = 0, cy = 0;
	uint4* arrayQuad = (uint4*) pMallocHost(
			widthQuad * heightQuad * sizeof(uint4));
	uint4 *ptr, bases;
	int4 lengths, indices;
	uint8_t *seq, *seq2, *seq3, *seq4;
	int index;
	for (int i = 0; i < numThreshold; i += 4) {
		/*get the sequence index of the quad-sequence group*/
		indices = make_int4(sortedSeqs[i].idx, sortedSeqs[i + 1].idx,
				sortedSeqs[i + 2].idx, sortedSeqs[i + 3].idx);

		/*get the aligned sequence length*/
		lengths = make_int4(dbSeqsAlignedLen[indices.x],
				dbSeqsAlignedLen[indices.y], dbSeqsAlignedLen[indices.z],
				dbSeqsAlignedLen[indices.w]);

		int4 lengths2 = make_int4(dbSeqsLen[indices.x], dbSeqsLen[indices.y],
				dbSeqsLen[indices.z], dbSeqsLen[indices.w]);

		totalNumBases += lengths2.x + lengths2.y + lengths2.z + lengths2.w;
		totalNumNulls += (lengths.x - lengths2.x) + (lengths.y - lengths2.y)
				+ (lengths.z - lengths2.z) + (lengths.w - lengths2.w);

		/*get the sequence pointers*/
		seq = dbSeqs[indices.x];
		seq2 = dbSeqs[indices.y];
		seq3 = dbSeqs[indices.z];
		seq4 = dbSeqs[indices.w];

		/*copy the sequence into the array*/
		ptr = arrayQuad + cy * widthQuad + cx;
		for (int j = 0; j < lengths.w; j += 4) {
			/*get the bases for the 1-th sequence*/
			bases.x = (j + 3 < lengths.x) ? seq[j + 3] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j + 2 < lengths.x) ? seq[j + 2] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j + 1 < lengths.x) ? seq[j + 1] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j < lengths.x) ? seq[j] : DUMMY_AMINO_ACID;
			/*get the bases for the 2-th sequence*/
			bases.y = (j + 3 < lengths.y) ? seq2[j + 3] : DUMMY_AMINO_ACID;
			bases.y <<= 8;
			bases.y |= (j + 2 < lengths.y) ? seq2[j + 2] : DUMMY_AMINO_ACID;
			bases.y <<= 8;
			bases.y |= (j + 1 < lengths.y) ? seq2[j + 1] : DUMMY_AMINO_ACID;
			bases.y <<= 8;
			bases.y |= (j < lengths.y) ? seq2[j] : DUMMY_AMINO_ACID;
			/*get the bases for the 3-th sequence*/
			bases.z = (j + 3 < lengths.z) ? seq3[j + 3] : DUMMY_AMINO_ACID;
			bases.z <<= 8;
			bases.z |= (j + 2 < lengths.z) ? seq3[j + 2] : DUMMY_AMINO_ACID;
			bases.z <<= 8;
			bases.z |= (j + 1 < lengths.z) ? seq3[j + 1] : DUMMY_AMINO_ACID;
			bases.z <<= 8;
			bases.z |= (j < lengths.z) ? seq3[j] : DUMMY_AMINO_ACID;
			/*get the bases for the 4-th sequence*/
			bases.w = seq4[j + 3];
			bases.w <<= 8;
			bases.w |= seq4[j + 2];
			bases.w <<= 8;
			bases.w |= seq4[j + 1];
			bases.w <<= 8;
			bases.w |= seq4[j];

			/*move to the next row*/
			*ptr = bases;
			ptr += widthQuad;
		}
		//build the corresponding hash item
		index = i >> 2;
		hashQuad[index].cx = cx;
		hashQuad[index].cy = cy;
		hashQuad[index].length = lengths.w; /*this field is not used actually*/
		hashQuad[index].alignedLen = lengths.w;

		//adjust the coordinates
		cx++;
		if (cx >= widthQuad) {
			cx = 0;
			cy += lengths.w >> 2; //increase the vertical coordinate
		}
	}
	switch (numThreshold & 3) {
	case 1:
		indices.x = sortedSeqs[numThreshold - 1].idx;
		lengths.x = dbSeqsAlignedLen[indices.x];
		seq = dbSeqs[indices.x];
		ptr = arrayQuad + cy * widthQuad + cx;
		for (int j = 0; j < lengths.x; j += 4) {
			bases.x = seq[j + 3];
			bases.x <<= 8;
			bases.x |= seq[j + 2];
			bases.x <<= 8;
			bases.x |= seq[j + 1];
			bases.x <<= 8;
			bases.x |= seq[j];
			bases.y = DUMMY_QUAD_AMINO_ACIDS;
			bases.z = DUMMY_QUAD_AMINO_ACIDS;
			bases.w = DUMMY_QUAD_AMINO_ACIDS;
			*ptr = bases;
			ptr += widthQuad;
		}
		//build the corresponding hash item
		index = numThresholdQuad - 1;
		hashQuad[index].cx = cx;
		hashQuad[index].cy = cy;
		hashQuad[index].length = lengths.x; /*this field is not used actually*/
		hashQuad[index].alignedLen = lengths.x;
		break;
	case 2:
		indices.x = sortedSeqs[numThreshold - 2].idx;
		indices.y = sortedSeqs[numThreshold - 1].idx;
		lengths.x = dbSeqsAlignedLen[indices.x];
		lengths.y = dbSeqsAlignedLen[indices.y];
		seq = dbSeqs[indices.x];
		seq2 = dbSeqs[indices.y];
		ptr = arrayQuad + cy * widthQuad + cx;
		for (int j = 0; j < lengths.y; j += 4) {
			bases.x = (j + 3 < lengths.x) ? seq[j + 3] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j + 2 < lengths.x) ? seq[j + 2] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j + 1 < lengths.x) ? seq[j + 1] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j < lengths.x) ? seq[j] : DUMMY_AMINO_ACID;

			bases.y = seq2[j + 3];
			bases.y <<= 8;
			bases.y |= seq2[j + 2];
			bases.y <<= 8;
			bases.y |= seq2[j + 1];
			bases.y <<= 8;
			bases.y |= seq2[j];
			bases.z = DUMMY_QUAD_AMINO_ACIDS;
			bases.w = DUMMY_QUAD_AMINO_ACIDS;
			*ptr = bases;
			ptr += widthQuad;
		}
		//build the corresponding hash item
		index = numThresholdQuad - 1;
		hashQuad[index].cx = cx;
		hashQuad[index].cy = cy;
		hashQuad[index].length = lengths.y; /*this field is not used actually*/
		hashQuad[index].alignedLen = lengths.y;
		break;
	case 3:
		indices.x = sortedSeqs[numThreshold - 3].idx;
		indices.y = sortedSeqs[numThreshold - 2].idx;
		indices.z = sortedSeqs[numThreshold - 1].idx;
		lengths.x = dbSeqsAlignedLen[indices.x];
		lengths.y = dbSeqsAlignedLen[indices.y];
		lengths.z = dbSeqsAlignedLen[indices.z];
		seq = dbSeqs[indices.x];
		seq2 = dbSeqs[indices.y];
		seq3 = dbSeqs[indices.z];
		ptr = arrayQuad + cy * widthQuad + cx;
		for (int j = 0; j < lengths.z; j += 4) {
			bases.x = (j + 3 < lengths.x) ? seq[j + 3] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j + 2 < lengths.x) ? seq[j + 2] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j + 1 < lengths.x) ? seq[j + 1] : DUMMY_AMINO_ACID;
			bases.x <<= 8;
			bases.x |= (j < lengths.x) ? seq[j] : DUMMY_AMINO_ACID;

			bases.y = (j + 3 < lengths.y) ? seq2[j + 3] : DUMMY_AMINO_ACID;
			bases.y <<= 8;
			bases.y |= (j + 2 < lengths.y) ? seq2[j + 2] : DUMMY_AMINO_ACID;
			bases.y <<= 8;
			bases.y |= (j + 1 < lengths.y) ? seq2[j + 1] : DUMMY_AMINO_ACID;
			bases.y <<= 8;
			bases.y |= (j < lengths.y) ? seq2[j] : DUMMY_AMINO_ACID;

			bases.z = seq3[j + 3];
			bases.z <<= 8;
			bases.z |= seq3[j + 2];
			bases.z <<= 8;
			bases.z |= seq3[j + 1];
			bases.z <<= 8;
			bases.z |= seq3[j];
			bases.w = DUMMY_QUAD_AMINO_ACIDS;
			*ptr = bases;
			ptr += widthQuad;
		}
		//build the corresponding hash item
		index = numThresholdQuad - 1;
		hashQuad[index].cx = cx;
		hashQuad[index].cy = cy;
		hashQuad[index].length = lengths.z; /*this field is not used actually*/
		hashQuad[index].alignedLen = lengths.z;
		break;
	}

	return new Database<uint4>(arrayQuad, widthQuad, heightQuad);
}
void CSearchScalar::loadInterDBQuad(CFastaSW* cudasw, DatabaseHash* hashQuad,
		Database<uint4>* db) {
	int widthQuad = db->width;
	int heightQuad = db->height;
	uint4* arrayQuad = db->array;

	//copy the database sequences from host to GPU
	cudasw->cudaInterSeqsQuad = cudasw->swMallocArray(widthQuad, heightQuad,
			pChannelFormatKindUnsignedInt4);
	pMemcpyToArray(cudasw->cudaInterSeqsQuad, 0, 0, arrayQuad,
			widthQuad * heightQuad * sizeof(uint4), pMemcpyHostToDevice);

	/*copy the hash table for quad-lane SIMD computing*/
	cudasw->cudaSeqHashQuad = (DatabaseHash*) pMallocPitch(sizeof(hashQuad[0]),
			numThresholdQuad, 1, 0);
	pMemcpy(cudasw->cudaSeqHashQuad, hashQuad,
			numThresholdQuad * sizeof(hashQuad[0]), pMemcpyHostToDevice);

	/*bind to texture*/
	cudasw->swBindTextureToArrayQuad();
}
void CSearchScalar::unloadInterDBQuad(CFastaSW* cudasw) {

	/*unbind texture memory*/
	cudasw->swUnbindTextureQuad();

	/*release the sequence buffer on the device*/
	pFreeArray(cudasw->cudaInterSeqsQuad);
}

