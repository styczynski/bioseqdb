#include "hashKMer.hpp"

#define charToInt(c) (__nst_nt4_table__[(int)c])
#define intToChar(x) ("ACGTN"[(x)])

int hashK(const char* s){
    int out = 0;
    for (int i=0; i<KMER_K; i++){
        if (s[i]=='N' || s[i]=='n') return -1;
        out += charToInt(s[i])*pow4(KMER_K-1-i);
    }
    return out;
}

char* inverseHashK(int x){
	char* out = (char*)malloc((KMER_K+1)*sizeof(char));
	for (int i=0; i<KMER_K; i++){
		if (x==0) out[KMER_K-1-i] = intToChar(0);
		else out[KMER_K-1-i] = intToChar(x%4);
		x = x/4;
	}
	out[KMER_K] = '\0';
	return out;
}


// hash table as array
// (arrayIndex = hashValue) --> bwt interval kmers_bucket_t
kmers_bucket_t* createHashKTable(const bwt_t *bwt){
	kmers_bucket_t* out = (kmers_bucket_t*)malloc(pow4(KMER_K)*sizeof(kmers_bucket_t));
	for (int hashValue=0; hashValue<pow4(KMER_K); hashValue++){
		char *read = inverseHashK(hashValue);	// K-length string for finding intervals
		bwtintv_t ik, ok[4];
		bwt_set_intv(bwt, charToInt(read[0]), ik); // set ik = the initial interval of the first base
		for (int i=1; i<KMER_K; i++){	// forward extend
			if (ik.x[2] < 1) break; // no match
			char cb = 3 - charToInt(read[i]);	// complement of next base
			bwt_extend(bwt, &ik, ok, 0);
			ik = ok[cb];
		}
		// save result
		out[hashValue].x[0] = ik.x[0];
		out[hashValue].x[1] = ik.x[1];
		out[hashValue].x[2] = ik.x[2];
	}
	
	return out;
}

#undef chartoInt
#undef inToChar
