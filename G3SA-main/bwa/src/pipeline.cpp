// pipeline for bwa-mem operation.
#include "pipeline.h"
#include "concurrentqueue.h"
#include <queue>
#include <vector>
#include <thread>
#include <atomic>
#include <iostream>
#include "timer.h"
#include <string.h>
#include "macro.h"
#include "cuda_wrapper.h"

#define MAX_RECORD_BYTES 1024
#define MAX_QUEUE_SIZE_FACTOR 8

extern float tprof[MAX_NUM_GPUS][MAX_NUM_STEPS];
extern int bwa_align(int gpuid, process_data_t *proc, g3_opt_t *g3_opt);

std::atomic<int> dispatch_queue_size;
std::atomic<int> active_loader_cnt;
std::atomic<int> active_dispatcher_cnt;
std::atomic<long long> loaded_cnt;
std::atomic<long long> dispatched_cnt;
std::atomic<long long> written_cnt;
std::mutex write_queue_mutex;

void pipeline(pipeline_aux_t *aux)
{
	// pipeline queues
	moodycamel::ConcurrentQueue<parsed_chunk *> dispatch_queue;
	std::priority_queue<
		aligned_chunk *, std::vector<aligned_chunk *>, writequeue_compare
		> write_queue;
	loaded_cnt = dispatched_cnt = written_cnt = 0;
	dispatch_queue_size = 0;

	// load input from disk
	std::vector<std::thread> load_threads;
	if(aux->load_thread_cnt == 0){ // to test loading at GPU-zero envs.
		active_loader_cnt = aux->load_thread_cnt = 1;
	} else{
		active_loader_cnt = aux->load_thread_cnt;
	}
	for(int t=0; t<aux->load_thread_cnt; t++) {
		load_threads.emplace_back(worker_load_and_parse, 
				std::ref(aux->g3_opt),
				std::ref(aux->fd_input),
				std::ref(aux->load_chunk_bytes),
				t, aux->load_thread_cnt, std::ref(dispatch_queue));
	}

	// work (align) with GPUs
	int num_workers = aux->g3_opt->num_use_gpus;
	active_dispatcher_cnt = num_workers;
	auto gpu_worker = [&](int tid) {
		memcpy_index(aux->proc[tid], tid, aux);
		worker_dispatch(std::ref(dispatch_queue),
				tid,
				std::ref(aux),
				std::ref(aux->proc[tid]),
				std::ref(write_queue));
	};
	std::vector<std::thread> gpu_threads;
	for (int t=0; t < num_workers; t++) {
		gpu_threads.emplace_back(gpu_worker, t);
	}

	// write sams to disk in-order
	std::thread write_thread(worker_write, std::ref(write_queue), std::ref(aux->samout),
			std::ref(aux->g3_opt));

	for (auto &th : load_threads)
		th.join();
	for (auto &th : gpu_threads)
		th.join();
	write_thread.join();
	for(int t=0; t<num_workers; t++){
		destruct_proc(aux->proc[t]);
	}

	std::cerr << "* ALL PROCESSING DONE\n";
	std::cerr << "* loaded cnt: " << loaded_cnt << "\n";
	std::cerr << "* dispatched cnt: " << dispatched_cnt << "\n";
	std::cerr << "* written cnt: " << written_cnt << "\n";
	return;
}


const uint8_t encode_nt4[256] = {
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 5 /*'-'*/, 4, 4,
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 0, 4, 1,  4, 4, 4, 2,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  3, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4, 
	4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4,  4, 4, 4, 4
};



void worker_load_and_parse(
		g3_opt_t *g3_opt,
		int fd_input,
		long load_chunk_bytes,
		int tid,
		int thread_cnt,
		moodycamel::ConcurrentQueue<parsed_chunk *> &dispatch_queue
		)
{
	long long file_offset;
	ssize_t loaded_bytes, rec_loaded_bytes;
	std::string buffer;
	int niter;
	char *p, *q;

	LARGE_TIMER_INIT();
	TIMER_INIT();

	LARGE_TIMER_START();
	long long name_offset, qual_offset, seq_offset;
	char buf_record[MAX_RECORD_BYTES];
	int nline;
	niter = 0;
	int id;
	bool first = true;
	while(true) {
		if(g3_opt->bound_load_queue &&
				dispatch_queue_size >= MAX_QUEUE_SIZE_FACTOR * thread_cnt){
			continue;
		} 
		dispatch_queue_size++;
		if(first){
			TIMER_START();
		}
		parsed_chunk *pc = new parsed_chunk;
		id = tid + thread_cnt * niter++;
		file_offset = load_chunk_bytes * id;
		pc->chunk_offset = file_offset;


		long load_request_bytes = load_chunk_bytes + g3_opt->single_record_bytes;

		buffer.clear();
		buffer.resize(load_request_bytes);
		// load a chunk.
		loaded_bytes = pread(fd_input, &buffer[0], load_request_bytes,
				file_offset);
		fflush(stderr);
		if(loaded_bytes == 0) break; // EOF.

		char *begin, *end;
		begin = &buffer[0];
		end = &buffer[0] + load_chunk_bytes;

		// boundary calibration
		while(*begin != '@') begin++;

		if(loaded_bytes < load_chunk_bytes){ // at some EOF
			end = &buffer[0] + loaded_bytes;
		} else{
			while(end < &buffer[0] + loaded_bytes &&
					*end != '@') end++;
		}

		// parse the loaded chunk.
		// input format:
		// @NAME EXTRA\n
		// SEQ\n
		// EXTRA2\n
		// QUAL\n
		// (repeats).
		char *pos, *base;
		pos = base = begin;
		pc->chunk_size = 0;
		name_offset = qual_offset = seq_offset = 0;
		for(pos = begin; pos < end; pos++){ // always *pos = '@' or term. here.
			pc->name_offsets.emplace_back(name_offset);
			pc->seq_offsets.emplace_back(seq_offset);
			pc->qual_offsets.emplace_back(qual_offset);

			for(base = ++pos; pos < end && *pos != ' '; pos++) ; // NAME
			pc->name.append(base, pos - base);
			name_offset += pos - base;
			for(; pos < end && *pos != '\n'; pos++) ; // skip EXTRA
			for(base = ++pos; pos < end && *pos != '\n'; pos++) ; // SEQ
			for(char *c = base; c < pos; c++)
				pc->seq.emplace_back(encode_nt4[(int)*c]);
			seq_offset += pos - base;
			for(++pos; pos < end && *pos != '\n'; pos++) ; // skip EXTRA2
			for(base = ++pos; pos < end && *pos != '\n'; pos++) ; // QUAL
			pc->qual.append(base, pos - base);
			qual_offset += pos - base;

			pc->chunk_size++;
		}

		// 1 additional sentinel
		pc->name_offsets.emplace_back(name_offset);
		pc->qual_offsets.emplace_back(qual_offset);
		pc->seq_offsets.emplace_back(seq_offset);

		// enqueue
		loaded_cnt += pc->chunk_size;
		dispatch_queue.enqueue(pc);
		if(first){
			TIMER_END(0, "loaded the first chunk");
			tprof[tid][FILE_INPUT_FIRST] += (float)(duration.count() / 1000);
			first = false;
		}
		//std::cerr << "* loaded " << pc->chunk_offset / MB_SIZE << " MB\n";
	}
	LARGE_TIMER_END(1, "loaded all chunks");
	tprof[tid][FILE_INPUT] += (float)(large_duration.count() / 1000);

	active_loader_cnt--; // EOF
	return;
}

void worker_dispatch(
		moodycamel::ConcurrentQueue<parsed_chunk *> &dispatch_queue,
		int tid,
		pipeline_aux_t *aux,
		process_data_t *_proc,
		std::priority_queue<
		aligned_chunk *, std::vector<aligned_chunk *>, writequeue_compare
		> &write_queue
		)
{
	LARGE_TIMER_INIT();
	LARGE_TIMER_START();
	TIMER_INIT();
	process_data_t *proc = _proc;
	int gpuid = tid;
	aligned_chunk *ac;
	parsed_chunk *pc;
	while(true){
		if(dispatch_queue.try_dequeue(pc)){
			dispatch_queue_size--;
			TIMER_START();
			ac = new aligned_chunk;
			ac->chunk_offset = pc->chunk_offset;
			ac->chunk_size = pc->chunk_size;
			ac->seq_offsets = pc->seq_offsets;
			ac->seq = pc->seq;
			ac->name_offsets = pc->name_offsets;
			ac->name = pc->name;
			ac->qual_offsets = pc->qual_offsets;
			ac->qual = pc->qual;
			delete pc;
			dispatched_cnt += ac->chunk_size;

			TIMER_END(0, "dequeued a chunk");


			TIMER_START();
			memcpy_input(ac->chunk_size, proc, 
					ac->seq.data(), ac->seq_offsets.data());
			TIMER_END(0, "sent a chunk");
			tprof[gpuid][PUSH_TOTAL] += (float)(duration.count() / 1000);

			// launch compute kernels.
			TIMER_START();
			if(bwa_align(gpuid, proc, aux->g3_opt) != 0){
				// reset TODO isolate cuda error using processes not threads.
			}
			TIMER_END(0, "computed a chunk");
			tprof[gpuid][COMPUTE_TOTAL] += ((float)duration.count() / 1000);


			TIMER_START();
			memcpy_output(ac, proc);
			TIMER_END(0, "received a chunk");
			tprof[gpuid][PULL_TOTAL] += ((float)duration.count() / 1000);

			TIMER_START();

			for(int seq_id = 0; seq_id < ac->chunk_size; seq_id++)
			{
				uint8_t *seqbuf;
				int name_len, qual_len, seq_len;
				name_len = ac->name_offsets.at(seq_id + 1) - \
					   ac->name_offsets.at(seq_id);
				qual_len = ac->qual_offsets.at(seq_id + 1) - \
					   ac->qual_offsets.at(seq_id);
				seqbuf = ac->seq.data() + ac->seq_offsets.at(seq_id);
				seq_len = ac->seq_offsets.at(seq_id + 1) - \
					  ac->seq_offsets.at(seq_id);

				int offset = ac->offsets.at(seq_id);
				int offset_next = ac->offsets.at(seq_id + 1);
				// write lines for empty alignments as well.
				if(offset_next <= offset){
					ac->sambuf += ac->name.substr(ac->name_offsets.at(seq_id), name_len);
					ac->sambuf += " ";
					ac->sambuf += "*"; // in place of reference name
					ac->sambuf += " ";
					ac->sambuf += "0"; // in place of position
					ac->sambuf += " ";
					ac->sambuf += "*"; // in place of CIGAR
					ac->sambuf += " ";

					uint8_t *p = seqbuf;
					while(p < seqbuf + seq_len){
						ac->sambuf += "ACGTN"[(int)*p++];
					}
					ac->sambuf += " ";
					ac->sambuf += ac->qual.substr(ac->qual_offsets.at(seq_id), qual_len);
					ac->sambuf += "\n";
					continue;
				}
				for(int aln_id = offset; aln_id < offset_next;
						aln_id++){
					ac->sambuf += ac->name.substr(ac->name_offsets.at(seq_id), name_len);
					ac->sambuf += " ";
					int rid = ac->rids.at(aln_id);
					ac->sambuf += aux->idx->bns->anns[rid].name; // append reference name
					ac->sambuf += " ";
					ac->sambuf += std::to_string(ac->positions.at(aln_id));
					ac->sambuf += " ";


					int ncigars = ac->ncigars.at(aln_id);
					uint32_t *cigars = ac->cigars.data() + aln_id * MAX_N_CIGAR;
					while(ncigars-- > 0)
					{
						int len = BAM2LEN(*cigars);
						char cch = BAM2OP(*cigars++);
						ac->sambuf += std::to_string(len);
						ac->sambuf += cch;
					}
					ac->sambuf += " ";

					uint8_t *p = seqbuf;
					while(p < seqbuf + seq_len){
						ac->sambuf += "ACGTN"[(int)*p++];
					}
					ac->sambuf += " ";
					ac->sambuf += ac->qual.substr(ac->qual_offsets.at(seq_id), qual_len);
					ac->sambuf += "\n";
				}
			}

			{
				std::lock_guard<std::mutex> lock(write_queue_mutex);
				write_queue.push(ac);
			}
			TIMER_END(0, "samgen a chunk");
			tprof[gpuid][SAMGEN_TOTAL] += (float)(duration.count() / 1000);
			//std::cerr << "* computed " << ac->chunk_offset / MB_SIZE << " MB\n";
		} else{
			if(active_loader_cnt == 0){
				break; // fastq all loaded.
			}
		}
	}

	active_dispatcher_cnt--;
	LARGE_TIMER_END(0, "dispatched chunks");
	tprof[gpuid][ALIGNER_TOP] += (float)(large_duration.count() / 1000);
	return;
}

void worker_write(
		std::priority_queue<
		aligned_chunk *, std::vector<aligned_chunk *>, writequeue_compare
		> &write_queue,
		std::ostream *samout,
		g3_opt_t *g3_opt
		)
{
	TIMER_INIT();
	float write_time = 0;
	aligned_chunk *ac;
	bool first = true;
	while(true) {
		if(active_dispatcher_cnt == 0) break;
		{
			std::lock_guard<std::mutex> lock(write_queue_mutex);
			if(write_queue.empty()){
				continue;
			} else {
				ac = write_queue.top();
				write_queue.pop();
			}
		}
		if(first){
			std::cerr << "* WRITING OUTPUT STARTS...\n";
			first = false;
		}
		TIMER_START();
		*samout << ac->sambuf;
		samout->flush();
		TIMER_END(0, "");
		write_time += (float)(duration.count() / 1000);
		written_cnt += ac->chunk_size;
		delete ac;
		std::cerr << "* wrote sams for " << written_cnt << " reads\n";
	}

	for(int gpuid = 0; gpuid < g3_opt->num_use_gpus; gpuid++){
		tprof[gpuid][FILE_OUTPUT] = write_time;
	}
}
