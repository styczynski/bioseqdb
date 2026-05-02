//#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "kseq.h"
KSEQ_INIT(int, read)

int main() {
	int fp;
	unsigned int count = 0;
	kseq_t *seq;
	fp = 0;
	seq = kseq_init(fp);
	while (kseq_read(seq) >= 0) count++;
	kseq_destroy(seq);
	close(fp);
	printf("%i\n", count);
	return 0;
}

