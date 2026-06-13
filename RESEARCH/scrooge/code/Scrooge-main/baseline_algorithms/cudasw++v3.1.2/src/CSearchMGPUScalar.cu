/***********************************************
 * # Copyright 2009. Liu Yongchao
 * # Contact: Liu Yongchao
 * #          liuy0039@ntu.edu.sg; nkcslyc@hotmail.com
 * #
 * # GPL 2.0 applies.
 * #
 * ************************************************/

#include "CSearchMGPUScalar.h"
#include "CFastaFile.h"
#include "CFastaSWScalar.h"

CSearchMGPUScalar::CSearchMGPUScalar(CParams* params) :
		CSearchScalar(params) {
	globalHostResult = 0;
}
CSearchMGPUScalar::~CSearchMGPUScalar() {
	if (globalHostResult) {
		pFreeHost(globalHostResult);
	}
}
#define MINIMAL_WIDTH	(7680 >> 2)
int CSearchMGPUScalar::dbsearch(char* queryFile) {

	CFastaFile* queryLib = new CFastaFile;
	int width, height;

	int interThreads = 64;
	double divergence = 0;

	/*calculate divergence*/
	if (avgLengthThreshold > 0) {
		divergence = ((double) stdLengthThreshold) * 100 / avgLengthThreshold;
	}
	globalHostResult = (SeqEntry*) pMallocHost(sizeof(SeqEntry) * numSeqs); //allocate global host result buffer
	GPUInfo* info = pGetGPUInfo(); /*get the GPU information*/
	TaskPlan * plans = (TaskPlan*) malloc(info->n_device * sizeof(TaskPlan)); /*allocate task plans*/
	for (int dev = 0; dev < info->n_device; dev++) {
		TaskPlan * plan = &plans[dev];
		plan->threads = interThreads;
		plan->device = dev;
		plan->info = info;
		plan->cudaInterTexWidth = max(
				pGetMultiProcessorCount(info, dev) * interThreads,
				MINIMAL_WIDTH);
		plan->cudaInterTexHeight = 0;
		plan->maxSeqLength = maxSeqLength;
		//
		plan->hostSeqHash = (DatabaseHash*) pMallocHost(
				numSeqs * sizeof(DatabaseHash));
		plan->numSeqs = 0;
		plan->interSeqNo = 0;
		plan->cudasw = new CFastaSWScalar;
		plan->divergence = divergence;
	}

	/*estimate the texture memory width and height*/
	bool done;
	do {
		done = true;
		//allocate sequences for each GPU
		for (int n = 0; n < numThreshold;) {
			/*for each effective GPU*/
			for (int dev = 0; dev < info->n_device; dev++) {
				TaskPlan * plan = &plans[dev];
				if (n >= numThreshold) {
					break;
				}
				width = plan->cudaInterTexWidth;
				if (n + width * 4 < numThreshold) {
					n += width * 4;
					plan->interSeqNo += width * 4; /*calculate the number of sequences*/
					plan->cudaInterTexHeight += dbSeqsAlignedLen[sortedSeqs[n
							- 1].idx] / 4 + 1; /*depends on the longest*/
				} else {
					plan->interSeqNo += numThreshold - n; /*calculate the number of sequences*/
					n = numThreshold; /*the last sequence*/
					plan->cudaInterTexHeight += dbSeqsAlignedLen[sortedSeqs[n
							- 1].idx] / 4 + 1; /*depends on the longest*/
					break;
				}
			}
		}
		/*check if the size is out of range*/
		for (int i = 0; i < info->n_device; i++) {
			plans[i].cudaInterTexHeight++;
			if (plans[i].cudaInterTexHeight > 32768) {
				done = false;
				for (int j = 0; j < info->n_device; j++) {
					plans[j].cudaInterTexWidth = 65536;
					plans[j].cudaInterTexHeight = 0;
					plans[j].interSeqNo = 0;
				}
				break;
			}
		}
	} while (!done);

	//fill the array with the sorted sequences
	for (int dev = 0; dev < info->n_device; dev++) {
		TaskPlan * plan = &plans[dev];
		plan->cx = 0; /*specify the x-coordinates*/
		plan->cy = 0; /*spcify the y-coordinates*/
		if (plan->cudaInterTexHeight == 0) {
			plan->cudaInterTexHeight = 1;
		}
		plan->interHostSeqArray = pMallocHost(
				plan->cudaInterTexWidth * plan->cudaInterTexHeight
						* sizeof(uint4));

		//allocate result slot for host
		plan->hostResult = (SeqEntry*) pMallocHost(sizeof(SeqEntry) * numSeqs);
		plan->hostResultPos = 0; /*starting position in the result buffer*/
		plan->globalHostResult = globalHostResult; /*global host result buffer*/
	}

	/*packing sequences and filling the buffer*/
	uint4 *ptr, bases;
	int4 lengths, indices;
	uint8_t *seq, *seq2, *seq3, *seq4;
	int index = 0;
	for (int n = 0; n < numThreshold;) {
		for (int dev = 0; dev < info->n_device; dev++) {
			TaskPlan * plan = &plans[dev]; /*get the plan pointer*/

			/*get the texture memory size information*/
			width = plan->cudaInterTexWidth;
			height = plan->cudaInterTexHeight;
			uint4* array = (uint4*) plan->interHostSeqArray;
			if (n >= numThreshold) {
				break;
			}
			//get the sequence and its length after sorting
			int realWidth = min(width * 4, numThreshold - n); /*the number of sequences*/
			int realWidthQuad = (realWidth >> 2) << 2;
			for (int k = 0; k < realWidthQuad; k += 4) { /*pack 4 sequences*/
				int baseIndex = n + k; /*get the base index for the 4 sequences*/
				/*get the sequence index of the 4-sequence group*/
				indices = make_int4(sortedSeqs[baseIndex].idx,
						sortedSeqs[baseIndex + 1].idx,
						sortedSeqs[baseIndex + 2].idx,
						sortedSeqs[baseIndex + 3].idx);

				/*get the aligned sequence length*/
				lengths = make_int4(dbSeqsAlignedLen[indices.x],
						dbSeqsAlignedLen[indices.y],
						dbSeqsAlignedLen[indices.z],
						dbSeqsAlignedLen[indices.w]);

				int4 lengths2 = make_int4(dbSeqsLen[indices.x],
						dbSeqsLen[indices.y], dbSeqsLen[indices.z],
						dbSeqsLen[indices.w]);

				/*get the sequence pointers*/
				seq = dbSeqs[indices.x];
				seq2 = dbSeqs[indices.y];
				seq3 = dbSeqs[indices.z];
				seq4 = dbSeqs[indices.w];

				//copy the sequence into the array,starting from index 1 instead of 0
				ptr = array + plan->cy * width + plan->cx;
				for (int j = 0; j < lengths.w; j += 4) {
					/*get the bases for the 1-th sequence*/
					bases.x =
							(j + 3 < lengths.x) ? seq[j + 3] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |=
							(j + 2 < lengths.x) ? seq[j + 2] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |=
							(j + 1 < lengths.x) ? seq[j + 1] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |= (j < lengths.x) ? seq[j] : DUMMY_AMINO_ACID;
					/*get the bases for the 2-th sequence*/
					bases.y =
							(j + 3 < lengths.y) ?
									seq2[j + 3] : DUMMY_AMINO_ACID;
					bases.y <<= 8;
					bases.y |=
							(j + 2 < lengths.y) ?
									seq2[j + 2] : DUMMY_AMINO_ACID;
					bases.y <<= 8;
					bases.y |=
							(j + 1 < lengths.y) ?
									seq2[j + 1] : DUMMY_AMINO_ACID;
					bases.y <<= 8;
					bases.y |= (j < lengths.y) ? seq2[j] : DUMMY_AMINO_ACID;
					/*get the bases for the 3-th sequence*/
					bases.z =
							(j + 3 < lengths.z) ?
									seq3[j + 3] : DUMMY_AMINO_ACID;
					bases.z <<= 8;
					bases.z |=
							(j + 2 < lengths.z) ?
									seq3[j + 2] : DUMMY_AMINO_ACID;
					bases.z <<= 8;
					bases.z |=
							(j + 1 < lengths.z) ?
									seq3[j + 1] : DUMMY_AMINO_ACID;
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

					*ptr = bases; /*save the packed data*/
					ptr += width; /*move to the next row*/
				}
				//build the corresponding hash item
				index = plan->numSeqs >> 2;
				plan->hostSeqHash[index].cx = plan->cx;
				plan->hostSeqHash[index].cy = plan->cy;
				plan->hostSeqHash[index].alignedLen = lengths.w;
				plan->hostSeqHash[index].length = lengths.w;

				/*save the corresponding sequence index*/
				index <<= 2;
				plan->hostResult[index].idx = indices.x;
				plan->hostResult[index].value = 65536;
				plan->hostResult[index + 1].idx = indices.y;
				plan->hostResult[index + 1].value = 65536;
				plan->hostResult[index + 2].idx = indices.z;
				plan->hostResult[index + 2].value = 65536;
				plan->hostResult[index + 3].idx = indices.w;
				plan->hostResult[index + 3].value = 65536;

				/*calculate the total number of sequences*/
				plan->numSeqs += 4;

				//adjust the coordinates
				plan->cx++;
				if (plan->cx >= width) {
					plan->cx = 0;
					plan->cy += lengths.w >> 2;
				}

				if (plan->cy >= height) {
					fprintf(stderr,
							"the array overflowed at the bottom (cy:%d height:%d)! press any key to continue\n",
							plan->cy, height);
					getchar();
					break;
				}
			}
			/*possible remainder*/
			index = plan->numSeqs >> 2;
			switch (realWidth & 3) {
			case 1:
				indices.x = sortedSeqs[numThreshold - 1].idx;
				lengths.x = dbSeqsAlignedLen[indices.x];
				seq = dbSeqs[indices.x];
				ptr = array + plan->cy * width + plan->cx;
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
					ptr += width;
				}
				//build the corresponding hash item
				plan->hostSeqHash[index].cx = plan->cx;
				plan->hostSeqHash[index].cy = plan->cy;
				plan->hostSeqHash[index].alignedLen = lengths.x;
				plan->hostSeqHash[index].length = lengths.x;

				/*save the corresponding sequence index*/
				index <<= 2;
				plan->hostResult[index].idx = indices.x;
				plan->hostResult[index].value = 65536;
				plan->numSeqs++;
				break;
			case 2:
				indices.x = sortedSeqs[numThreshold - 2].idx;
				indices.y = sortedSeqs[numThreshold - 1].idx;
				lengths.x = dbSeqsAlignedLen[indices.x];
				lengths.y = dbSeqsAlignedLen[indices.y];
				seq = dbSeqs[indices.x];
				seq2 = dbSeqs[indices.y];
				ptr = array + plan->cy * width + plan->cx;
				for (int j = 0; j < lengths.y; j += 4) {
					bases.x =
							(j + 3 < lengths.x) ? seq[j + 3] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |=
							(j + 2 < lengths.x) ? seq[j + 2] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |=
							(j + 1 < lengths.x) ? seq[j + 1] : DUMMY_AMINO_ACID;
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
					ptr += width;
				}
				//build the corresponding hash item
				plan->hostSeqHash[index].cx = plan->cx;
				plan->hostSeqHash[index].cy = plan->cy;
				plan->hostSeqHash[index].alignedLen = lengths.y;
				plan->hostSeqHash[index].length = lengths.y;

				/*save the corresponding sequence index*/
				index <<= 2;
				plan->hostResult[index].idx = indices.x;
				plan->hostResult[index].value = 65536;
				plan->hostResult[index + 1].idx = indices.y;
				plan->hostResult[index + 1].value = 65536;
				plan->numSeqs += 2;
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
				ptr = array + plan->cy * width + plan->cx;
				for (int j = 0; j < lengths.z; j += 4) {
					bases.x =
							(j + 3 < lengths.x) ? seq[j + 3] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |=
							(j + 2 < lengths.x) ? seq[j + 2] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |=
							(j + 1 < lengths.x) ? seq[j + 1] : DUMMY_AMINO_ACID;
					bases.x <<= 8;
					bases.x |= (j < lengths.x) ? seq[j] : DUMMY_AMINO_ACID;

					bases.y =
							(j + 3 < lengths.y) ?
									seq2[j + 3] : DUMMY_AMINO_ACID;
					bases.y <<= 8;
					bases.y |=
							(j + 2 < lengths.y) ?
									seq2[j + 2] : DUMMY_AMINO_ACID;
					bases.y <<= 8;
					bases.y |=
							(j + 1 < lengths.y) ?
									seq2[j + 1] : DUMMY_AMINO_ACID;
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
					ptr += width;
				}
				//build the corresponding hash item
				plan->hostSeqHash[index].cx = plan->cx;
				plan->hostSeqHash[index].cy = plan->cy;
				plan->hostSeqHash[index].alignedLen = lengths.z;
				plan->hostSeqHash[index].length = lengths.z;

				/*save the corresponding sequence index*/
				index <<= 2;
				plan->hostResult[index].idx = indices.x;
				plan->hostResult[index].value = 65536;
				plan->hostResult[index + 1].idx = indices.y;
				plan->hostResult[index + 1].value = 65536;
				plan->hostResult[index + 2].idx = indices.z;
				plan->hostResult[index + 2].value = 65536;

				plan->numSeqs += 3;
				break;
			}

			/*increase the sequence index*/
			n += realWidth;
		}
	}
	/*check the range of texture memory*/
	for (int pos = 0, dev = 0; dev < info->n_device; ++dev) {

		/*check texture memory range*/
		width = plans[dev].cudaInterTexWidth;
		height = plans[dev].cudaInterTexHeight;

		if (width > 65536 || height > 32768) {
			fprintf(stderr,
					"width(%d) or height(%d) out of texture reference range\n",
					width, height);
		}
		/*fprintf(stderr,
				"#memory for GPU %d: width:%d height:%d size:%ld (MB)\n", dev,
				width, height, sizeof(uint4) * width * height / 0x100000);*/

		/*check the number of sequences*/
		if (plans[dev].interSeqNo != plans[dev].numSeqs) {
			fprintf(stderr, "Sequence number for GPU %d has errors\n", dev);
			return -1;
		}
		//fprintf(stderr, "#sequenes on GPU %d: %d\n", dev, plans[dev].numSeqs);

		//initialize the results buffer on the GPU
		plans[dev].hostResultPos = pos;
		pos += plans[dev].numSeqs;
	}

	/*for long sequences*/
	createIntraDBSSE(); /*For SSE-based computing (only for long sequences)*/

	/*initialize the global host buffer for SSE*/
	for (int i = numThreshold; i < numSeqs; ++i) {
		globalHostResult[i].idx = sortedSeqs[i].idx;
		globalHostResult[i].value = 65536;
	}

	/*initialize*/
	for (int dev = 0; dev < info->n_device; ++dev) {
		TaskPlan* plan = &plans[dev];
		CFastaSW* cudasw = plan->cudasw;
		/*select device*/
		pSetDevice(info, info->devices[plan->device]);

		/*create a stream*/
		cudaStreamCreate(&(plan->stream));

		/*copy parameters*/
		cudasw->swMemcpyParameters(CSearch::matrix, CSearch::gapOpen,
				CSearch::gapExtend);

		//Calculate the total number of blocks for inter-task parallelization
		plan->interBlocks = (plan->interSeqNo + plan->threads - 1)
				/ plan->threads;

		/*calculate the number of thread blocks per pass*/
		int numSMXProcessors = pGetMultiProcessorCount(plan->info,
				plan->device);
		plan->procsPerPass = numSMXProcessors * 2048 * 2 / plan->threads;

		/*get the L2 cache size*/
		plan->l2CacheSize = pGetL2CacheSize(plan->info, plan->device);

		/*allocate memory on the device*/
		int width = plan->cudaInterTexWidth;
		int height = plan->cudaInterTexHeight;

		cudasw->cudaInterSeqsQuad = cudasw->swMallocArray(width, height,
				pChannelFormatKindUnsignedInt4);
		pMemcpyToArray(cudasw->cudaInterSeqsQuad, 0, 0, plan->interHostSeqArray,
				width * height * sizeof(uint4), pMemcpyHostToDevice);

		//copy the hash table from host to GPU
		plan->numSeqsQuad = (plan->numSeqs + 3) >> 2;
		cudasw->cudaSeqHashQuad = (DatabaseHash*) pMallocPitch(
				sizeof(DatabaseHash), plan->numSeqsQuad, 1, 0);
		pMemcpy(cudasw->cudaSeqHashQuad, plan->hostSeqHash,
				plan->numSeqsQuad * sizeof(DatabaseHash), pMemcpyHostToDevice);

		//initialize the results buffer on the GPU
		memcpy(plan->globalHostResult + plan->hostResultPos, plan->hostResult,
				plan->numSeqs * sizeof(SeqEntry));
		cudasw->hostResult = &plan->globalHostResult[plan->hostResultPos];

		cudasw->cudaResult = (SeqEntry*) pMallocPitch(sizeof(SeqEntry),
				plan->numSeqs, 1, 0);
		pMemcpy(cudasw->cudaResult, cudasw->hostResult,
				plan->numSeqs * sizeof(SeqEntry), pMemcpyHostToDevice);

		//bind the CUDA Array to texture
		cudasw->swBindTextureToArrayQuad();
	}

	fprintf(stderr, "Loading database successfully\n");
	fprintf(stderr, "numSeqs: %d numThreshold: %d\n", numSeqs, numThreshold);
	fprintf(stderr,
			"maxSeqLength: %d totalAminoAcidsThreshod: %ld totalAminoAcids: %ld\n",
			maxSeqLength, totalAminoAcidsThreshold, totalAminoAcids);

	fprintf(stderr,
			"******************************\n******************************\n");

	double dif, gcups;
	double start, end;
	int qlen, qAlignedLen;
	uint8_t* query;

	/*open query file*/
	queryLib->open(queryFile);

	/*load the first query*/
	query = queryLib->nextSeq(&qlen, &qAlignedLen, QUERY_SEQ_LENGTH_ALIGNED);
	if (qlen == 0) {
		fprintf(stderr, "query file is empty!");
		goto out;
	}
	while (qlen > 0) {
		//get the system time
		CParams::getSysTime(&start);

		/*set the query*/
		for (int dev = 0; dev < info->n_device; dev++) {
			TaskPlan* plan = &plans[dev];
			plan->qAlignedLen = qAlignedLen;
			plan->query = query;
			plan->useQueryPrf = params->isQueryProfile();
		}

		/*launch CPU threads*/
		launchIntraSSE(globalHostResult, query, qlen);

		/*perform GPU computing*/
		performGPUComputing(plans);

		waitIntraSSE(); /*wait for the completion of SSE threads*/
#ifndef DISABLE_CPU_THREADS
		launchOverflowSSE(globalHostResult, query, qlen); /*recompute the sequences that overflowed*/
#endif

		//get the system time
		CParams::getSysTime(&end);

		/*for overall computing*/
		dif = end - start;
		gcups = ((double) totalAminoAcids) / 1000000.0;
		gcups /= 1000.0;
		gcups *= qlen;
		gcups /= dif;

		fprintf(stderr, "query:%s\n", queryLib->getSeqName());
		fprintf(stderr, "Length: %d --- time: %g (s) and GCUPS: %g\n",
				qlen, dif, gcups);

		//display results
		int top =
				numSeqs > params->getTopScoresNum() ?
						params->getTopScoresNum() : numSeqs;
		int scoreThreshold = params->getScoreThreshold();
		fprintf(stderr, "----------Display the top %d ----------\n", top);
		printResults(globalHostResult, dbSeqsName, numSeqs, top,
				scoreThreshold);

		/*VERBOSE*/
		//printf("%d\t%g\t%g\n", qlen, dif, gcups);

		//load the next query sequence
		query = queryLib->nextSeq(&qlen, &qAlignedLen,
				QUERY_SEQ_LENGTH_ALIGNED);
		if (qlen == 0) {
			fprintf(stderr, "Reaching the end of the query file!\n");
			break;
		}
		/*re-initialize the host buffer*/
  	for (int i = numThreshold; i < numSeqs; i++) {
    	globalHostResult[i].idx = sortedSeqs[i].idx;
    	globalHostResult[i].value = 65536;
  	}
		for (int dev = 0; dev < info->n_device; ++dev) {
			TaskPlan* plan = &plans[dev];
			memcpy(plan->globalHostResult + plan->hostResultPos, plan->hostResult,
				plan->numSeqs * sizeof(SeqEntry));
		}
	}

	out:
	/*release resources*/
	for (int dev = 0; dev < info->n_device; ++dev) {
		TaskPlan* plan = &plans[dev];
		CFastaSW* cudasw = plan->cudasw;

		/*set device*/
		pSetDevice(info, info->devices[plan->device]);

		/*synchronize the stream*/
		cudaStreamSynchronize(plan->stream);

		cudasw->swUnbindQueryProfile();
		cudasw->transferResult(plan->numSeqs); /*transfer the results*/

		//free device resources
		cudasw->swUnbindTextureQuad();
		pFree(cudasw->cudaSeqHashQuad);
		pFreeArray(cudasw->cudaInterSeqsQuad);
		pFree(cudasw->cudaResult);
		delete plan->cudasw;

		/*release host resources*/
		pFreeHost(plan->interHostSeqArray);
		pFreeHost(plan->hostSeqHash);
		pFreeHost(plan->hostResult);
	}
	free(plans); /*free resources*/

	if (globalHostResult) {
		pFreeHost(globalHostResult);
		globalHostResult = 0;
	}

	delete queryLib; /*close the data*/

	return 0;
}
void CSearchMGPUScalar::performGPUComputing(TaskPlan* plans) {
	CFastaSW* cudasw;

	/*get GPU information*/
	GPUInfo* info = pGetGPUInfo();
	for (int dev = 0; dev < info->n_device; ++dev) {
		TaskPlan* plan = &plans[dev];
		CFastaSW* cudasw = plan->cudasw;

		/*select device*/
		pSetDevice(info, info->devices[plan->device]);

		/*copy the query sequence*/
		cudasw->swMemcpyQuery(plan->query, plan->qAlignedLen,
				sizeof(unsigned char), CSearch::matrix, plan->useQueryPrf);

		/*bind query profiles*/
		cudasw->swBindQueryProfile();
	}

	/*core loop*/
	for (int dev = 0; dev < info->n_device; ++dev) {
		TaskPlan* plan = &plans[dev];
		cudasw = plan->cudasw;
		/*set device*/
		pSetDevice(info, info->devices[plan->device]);

		/*invoke the kernel*/
		if (plan->divergence <= DB_MAX_DIVERGENCE) {
			cudasw->InterRunGlobalDatabaseScanningQuad(plan->interBlocks, plan->threads,
						plan->numSeqsQuad, plan->useQueryPrf, plan->stream);
		} else {
			cudasw->InterRunGlobalDatabaseScanningQuadDynamic(
					min(plan->interBlocks, plan->procsPerPass), plan->threads,
					plan->numSeqsQuad, plan->useQueryPrf, plan->stream);
		}
	}

	for (int dev = 0; dev < info->n_device; ++dev) {
		TaskPlan* plan = &plans[dev];
		cudasw = plan->cudasw;

		/*set device*/
		pSetDevice(info, info->devices[plan->device]);

		/*synchronize the stream*/
		cudaStreamSynchronize(plan->stream);

		cudasw->swUnbindQueryProfile(); /*unbind query profiles*/
		cudasw->transferResult(plan->numSeqs); /*transfer the results*/
	}
}
