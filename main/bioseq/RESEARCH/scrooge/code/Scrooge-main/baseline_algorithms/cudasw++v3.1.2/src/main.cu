#include "CSearchScalar.h"
#include "CSearchMGPUScalar.h"
#include <stdio.h>

int main(int argc, char* argv[]) {
	CParams params;
	CSearch* search = 0;

	//init graphics card device
	pInitDevice(argc, argv);
	GPUInfo* info = pGetGPUInfo();

	//parse parameters
	if (!params.parseParams(argc, argv)) {
		return -1;
	}
	int singleGPUID = params.getSingleGPUID();
	if ((params.getNumGPUs() == 1)) {
		if (singleGPUID >= info->n_device) {
			singleGPUID = info->n_device - 1;
		}
		fprintf(stderr,
				"Use a single compatible GPU with ID %d and %d CPU thread(s)\n",
				info->devices[singleGPUID], params.getNumCPUThreads());
		pSetDevice(info, info->devices[singleGPUID]); //select the specified compatible GPU

		/*run the search engine*/
		search = new CSearchScalar(&params);
	} else if (params.getNumGPUs() > 1) {
		fprintf(stderr,
				"\nUse the first %d compatible GPUs and %d CPU thread(s)\n",
				info->n_device, params.getNumCPUThreads());
		/*run the search engine*/
		search = new CSearchMGPUScalar(&params);
	} else {
		fprintf(stderr, "No compatible device available.\n");
		return -1;
	}

	if (search) {
		search->run();
		delete search;
	}

	return 0;
}
