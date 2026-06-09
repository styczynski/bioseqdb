// Main function.
#include <stdio.h>
#include <iostream>
#include <thread>
#include <string.h>
#include "cuda_wrapper.h"

int main_mem(int argc, char *argv[]);
extern int bwa_index(int argc, char *argv[]);

static int usage()
{
	fprintf(stderr, "\n");
	fprintf(stderr, "Program: bwa (alignment via Burrows-Wheeler transformation)\n");
	fprintf(stderr, "Usage:   bwa <command> [options]\n\n");
	fprintf(stderr, "Command: mem           BWA-MEM algorithm\n");
	fprintf(stderr, "\n");
    return 1;
}

int main(int argc, char *argv[])
{
	int i, ret;

	if (argc < 2) return usage();
	else if (strcmp(argv[1], "mem") == 0) ret = main_mem(argc-1, argv+1);
	else if (strcmp(argv[1], "index") == 0) ret = bwa_index(argc-1, argv+1);
	else {
		fprintf(stderr, "[main] unrecognized command '%s'\n", argv[1]);
		return 1;
	}
	fflush(stdout);
	fclose(stdout);

	if (ret == 0) {
		fprintf(stderr, "[%s] CMD:", __func__);
		for (i = 0; i < argc; ++i)
			fprintf(stderr, " %s", argv[i]);
        std::cerr << std::endl;
	}
	return ret;
}
