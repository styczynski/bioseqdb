#ifndef PIPELINE_H
#define PIPELINE_H

#include <vector>
#include <string>
#include <queue>
#include "concurrentqueue.h"
#include "bwa.h"
#include "macro.h"


typedef struct
{
	//kseq_t *ks, *ks2;
	mem_opt_t *opt;
	mem_pestat_t *pes0;
	//int64_t n_processed;
	int copy_comment;
    long load_chunk_bytes;
	bwaidx_t *idx;
	kmers_bucket_t *kmerHashTab;
    fmIndex loadedIndex;
    std::ostream *samout;
    g3_opt_t *g3_opt;
    int load_thread_cnt;
    int dispatch_thread_cnt;
    int fd_input;
    process_data_t *proc[MAX_NUM_GPUS];
} pipeline_aux_t;

typedef struct {
    long long chunk_offset;
    int chunk_size;
    std::vector<int> seq_offsets;
    std::vector<uint8_t> seq;

    std::vector<int> name_offsets;
    std::string name;
    std::vector<int> qual_offsets;
    std::string qual;
} parsed_chunk;


typedef struct {
    long long chunk_offset;
    int chunk_size;
    std::vector<int> seq_offsets;
    std::vector<uint8_t> seq;
    std::vector<int> name_offsets;
    std::string name;
    std::vector<int> qual_offsets;
    std::string qual;

    int chunk_aln_cnt;
    std::vector<int> aln_offsets; // for both rid and pos.
    std::vector<int> rid;
    std::vector<uint64_t> pos;
    std::vector<int> bam_cigar_offsets;
    std::vector<uint32_t> bam_cigar;

    
    std::vector<int> offsets;
    std::vector<int> rids;
    std::vector<int64_t> positions;
    std::vector<int> ncigars;
    std::vector<uint32_t> cigars;


    std::string sambuf;
} aligned_chunk;


// top level pipeline wrapper.
void pipeline(pipeline_aux_t *aux);


// thread_cnt threads read then parse fastq inputs from file
// fd_input load_chunk_bytes each from the start (boundaries are checked)
// and enqueue them to the dispatch_queue.
void worker_load_and_parse(
        g3_opt_t *g3_opt,
        int fd_input,
        long load_chunk_bytes,
        int tid,
        int thread_cnt,
        moodycamel::ConcurrentQueue<parsed_chunk *> &dispatch_queue
        );


struct writequeue_compare {
    bool operator()(const aligned_chunk *a, const aligned_chunk *b) const {
        return a->chunk_offset > b->chunk_offset; // in-order
    }
};

void worker_dispatch(
        moodycamel::ConcurrentQueue<parsed_chunk *> &dispatch_queue,
        int tid,
        pipeline_aux_t *aux,
        process_data_t *proc,
        std::priority_queue<
            aligned_chunk *, std::vector<aligned_chunk *>, writequeue_compare
        > &write_queue
        );

void worker_write(
        std::priority_queue<
            aligned_chunk *, std::vector<aligned_chunk *>, writequeue_compare
        > &write_queue,
        std::ostream *samout,
        g3_opt_t *g3_opt
        );

#endif
