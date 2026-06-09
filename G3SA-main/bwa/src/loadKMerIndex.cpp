#include "datadump.hpp"
#include "hashKMer.hpp"

kmers_bucket_t *loadKMerIndex(const char* prefix){
    kmers_bucket_t *hashTable = loadArray((unsigned long long)pow4(KMER_K), std::string(prefix) + ".hash");
    return hashTable;
}
