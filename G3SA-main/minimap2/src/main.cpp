#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <zlib.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <math.h>
#include <errno.h>
#include <vector>
#include <fstream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>

#include "minimap.h"
#include "mmpriv.h"
#include "khash.h"
#include "kvec.h"
#include "index.h"
#include "sdust.h"
#include "common.h"
#include "kernel_wrapper.h"
#include "host_mem.h"
#include "seed.h"
#include "map.h"
#include "cpu_align.h"
#include "config.h"

#ifdef ENABLE_DEBUG
#warning "ENABLE_DEBUG is ON (host/device TU)"
#endif


void checkAvailableMemory() {
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.find("MemAvailable") != std::string::npos) {
            std::cout << line << std::endl;
        }
    }
}


static mm_bseq_file_t **open_bseqs(int n, const char **fn)
{
	mm_bseq_file_t **fp;
	int i, j;
	fp = (mm_bseq_file_t**)calloc(n, sizeof(mm_bseq_file_t*));
	for (i = 0; i < n; ++i) {
		if ((fp[i] = mm_bseq_open(fn[i])) == 0) {
			// if (mm_verbose >= 1)
			fprintf(stderr, "ERROR: failed to open file '%s': %s\n", fn[i], strerror(errno));
			for (j = 0; j < i; ++j)
				mm_bseq_close(fp[j]);
			free(fp);
			return 0;
		}
        else fprintf(stderr, "Successfully opened read file #%d : '%s' \n", i, fn[i]);
	}
	return fp;
}

void gpu_initialize_caller(int gpu_id, mm_idx_t* mi, mm_mapopt_t* opt, device_pointer_batch_t* d_ptrs, uint32_t max_seq_len, int batch_size) {
    cudaSetDevice(gpu_id);
    printf("kernel called for gpu %d\n", gpu_id);

    gpu_initialize(gpu_id, mi, opt, &d_ptrs[gpu_id], max_seq_len, batch_size);
}

void gpu_mm2_kernel_caller (int tid, device_pointer_batch_t* d_ptrs, char* seqs, int* len, int* ofs, int w, int k, uint64_t max_seq_len, int batch_size,
    uint32_t* h_cigar, int* h_n_cigar, int* h_score,  uint8_t* h_dropped)// output data
{
    int gpu_id = d_ptrs[tid].device_num;
    cudaSetDevice(gpu_id);

    // Set Host Memory Location
    char* h_seqs = &seqs[max_seq_len * batch_size * gpu_id];
    int* h_lens = &len[batch_size * gpu_id];
    int* h_ofs = &ofs[batch_size * gpu_id];

    DEBUG_ONLY(
        printf("[%d] Lauch GPU kernels \n", gpu_id);
    );

    gpu_mm2_kernel_wrapper(h_seqs, h_lens, h_ofs, &d_ptrs[tid], w, k, max_seq_len, batch_size, h_cigar, h_n_cigar, h_score, h_dropped);

}

int main(int argc, char *argv[]) {

    char *idx_file = argv[optind];

    int num_gpus;
    cudaGetDeviceCount(&num_gpus);
    if (num_gpus < 1) {
        std::cerr << "This program requires at least 1 GPU." << std::endl;
        return -1;
    }
    num_gpus = 1;
    
    printf("Number of gpus: %d\n", num_gpus);
    int idx_threads = 1;

    mm_idxopt_t ipt;
    mm_mapopt_t opt;

    mm_idx_reader_t *idx_rdr;
	mm_idx_t *mi;

    // output file name
    char *fnw = 0;

    mm_mapopt_init(&opt);
    mm_set_opt("map-ont", &ipt, &opt); 
    opt.flag |= MM_F_CIGAR; 

    opt.flag |= MM_F_NO_HASH_NAME;   // --no-hash-name

    opt.q2 = 4;
    opt.e2 = 2;
    opt.max_chain_skip = 5000;
    opt.bw = 0;


    /* Load reference index file */

    idx_rdr = mm_idx_reader_open(idx_file, &ipt, fnw);
    if (idx_rdr == 0) {
		fprintf(stderr, "[ERROR] failed to open file '%s': %s\n", argv[optind], strerror(errno));
		return 1;
	}
    else fprintf(stderr, "Index file '%s' opened\n", argv[optind++]);
    mi = mm_idx_reader_read(idx_rdr, idx_threads);
    mm_idx_stat(mi); // check if loaded correctly
    mm_mapopt_update(&opt, mi);

    /* Set input read file */
    int n_file = 1;
    const char *read_file = argv[optind];
    mm_bseq_file_t **fp;
    fp = open_bseqs(n_file, &read_file);

    /* Set gpu alignment configuration */
    runtime_cfg::init_once(argc, argv, "config.json");

    const int n_reads_tot = runtime_cfg::num_reads();
    const int batch_size = runtime_cfg::batch_size();
    const int max_seq_len = runtime_cfg::max_seq_len();
    const int num_batches = (n_reads_tot + batch_size - 1) / (batch_size * num_gpus);
    
    /* Set output sam file */
    std::ofstream fout("output_cigar.sam");

    /* Double buffering */
    host_input_buffer host_input_buffers[2];
    allocate_host_input_memory(&(host_input_buffers[0].h_seqs), &(host_input_buffers[0].h_lens), &(host_input_buffers[0].h_ofs), max_seq_len, batch_size, num_gpus);
    allocate_host_input_memory(&(host_input_buffers[1].h_seqs), &(host_input_buffers[1].h_lens), &(host_input_buffers[1].h_ofs), max_seq_len, batch_size, num_gpus);
    host_output_buffer host_output_buffers[2];
    allocate_host_output_memory(&host_output_buffers[0].h_score, &host_output_buffers[0].h_n_cigar, &host_output_buffers[0].h_cigar, max_seq_len, batch_size, num_gpus);
    allocate_host_output_memory(&host_output_buffers[1].h_score, &host_output_buffers[1].h_n_cigar, &host_output_buffers[1].h_cigar, max_seq_len, batch_size, num_gpus);

    /* Initialize Device Memory & Move Index */
    std::vector<std::thread> init_threads;
    device_pointer_batch_t* device_ptr_arr = (device_pointer_batch_t*) malloc(num_gpus * sizeof(device_pointer_batch_t));
    
    for (int gpu_id = 0; gpu_id < num_gpus; gpu_id++) {
        int offset = gpu_id * batch_size;
        init_threads.emplace_back([&, gpu_id, mi, device_ptr_arr, max_seq_len, batch_size]() {
            gpu_initialize_caller(gpu_id, mi, &opt, device_ptr_arr, max_seq_len, batch_size);
        });
    }
    for (auto &t : init_threads) {
        t.join();
    }
    
    // variables for double buffering

    std::mutex mtx;
    std::condition_variable cv_io_done, cv_gpu_done, cv_gpu_output_ready, cv_dump_done;
    std::queue<int> ready_buffers;
    std::queue<int> ready_output_buffers;
    std::vector<bool> buffer_in_use(2, false);
    std::vector<bool> output_buffer_in_use(2, false);
    bool no_more_data = false;

    std::queue<fallback_batch> fallback_batches;
    std::mutex fallback_mtx;
    std::condition_variable cv_fallback_ready;
    bool fallback_done = false;
    bool gpu_done = false;

    double total_io_time = 0.0;
    double total_gpu_time = 0.0;
    double total_out_time = 0.0;

    uint8_t flagged_reads[batch_size] = {0};  // Array to pass to CPU for flagged reads

    int batch_idx = 0;
    int batch_idx_io = 0;

    auto start_total = std::chrono::high_resolution_clock::now();

    printf(" Total number of batches : %d\n", num_batches);

    /* Host buffer I/O thread */
    std::thread io_thread([&]() {
        while (batch_idx_io < num_batches) {
            int buf_id = batch_idx_io % 2;

            // Wait if GPU is still using this buffer
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_gpu_done.wait(lock, [&]() { return !buffer_in_use[buf_id]; });
                buffer_in_use[buf_id] = true;
            }

            DEBUG_ONLY(
                std::cout <<"[IO] start loading input buffer\n";
            );

            auto& buf = host_input_buffers[buf_id];
            auto io_start = std::chrono::high_resolution_clock::now();
            fill_host_batch(fp, buf.h_seqs, buf.h_lens, buf.h_ofs,
                                       max_seq_len, batch_size, num_gpus);
            auto io_end = std::chrono::high_resolution_clock::now();
            double io_elapsed = std::chrono::duration<double>(io_end - io_start).count();
            total_io_time += io_elapsed;

            DEBUG_ONLY(
            std::cout << "[IO] Batch " << batch_idx_io << " loaded into buffer " << buf_id << " in " << io_elapsed << " sec\n";
            );

            // Notify GPU thread this buffer is ready
            {
                std::unique_lock<std::mutex> lock(mtx);
                ready_buffers.push(buf_id);
            }
            cv_io_done.notify_one();

            batch_idx_io++;
        }

        {
            std::unique_lock<std::mutex> lock(mtx);
            no_more_data = true;
        }
        cv_io_done.notify_one();
        printf("Input thread returned\n");
    });

    /* GPU kernel managing thread */
    std::thread gpu_thread([&]() {
        //int batch_idx = 0;
        while (true) {
            int buf_id = -1;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_io_done.wait(lock, [&]() {
                    return !ready_buffers.empty() || no_more_data;
                });

                if (ready_buffers.empty() && no_more_data) break;

                buf_id = ready_buffers.front();
                ready_buffers.pop();
            }

            // output

            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_dump_done.wait(lock, [&]() { return !output_buffer_in_use[buf_id]; });
                output_buffer_in_use[buf_id] = true;
            }

            auto& buf = host_input_buffers[buf_id];
            auto& out = host_output_buffers[buf_id];

            DEBUG_ONLY(
                std::cout << "[GPU] Processing batch " << batch_idx << " from buffer " << buf_id << std::endl;
            );

            auto gpu_start = std::chrono::high_resolution_clock::now();

            std::vector<std::thread> exec_threads;
            for (int gpu_id = 0; gpu_id < num_gpus; gpu_id++) {
                exec_threads.emplace_back([&, gpu_id]() {
                    gpu_mm2_kernel_caller(gpu_id, device_ptr_arr, buf.h_seqs, buf.h_lens, buf.h_ofs, mi->w, mi->k, max_seq_len, batch_size, 
                    out.h_cigar, out.h_n_cigar, out.h_score, flagged_reads);
                });
            }

            for (auto& t : exec_threads) t.join();
            
            auto gpu_end = std::chrono::high_resolution_clock::now();
            double gpu_elapsed = std::chrono::duration<double>(gpu_end - gpu_start).count();
            total_gpu_time += gpu_elapsed;

            //DEBUG_ONLY(
                std::cout << "[GPU] Batch " << batch_idx << " processed in " << gpu_elapsed << " sec\n";
            //);

            fallback_batch fb;
            fb.is_empty_batch = false;
            fb.buf_id = buf_id;
            for (int i = 0; i < batch_size; ++i) {
                if (flagged_reads[i]) {
                    fb.read_ids.push_back(i);
                    int len = buf.h_lens[i];
                    fb.seqs.emplace_back(&buf.h_seqs[buf.h_ofs[i]], len);
                    flagged_reads[i] = 0;
                }
            }
            if (!fb.read_ids.empty()) {
                std::lock_guard<std::mutex> lock(fallback_mtx);
                fallback_batches.push(std::move(fb));
                cv_fallback_ready.notify_one();
            }
            else {
                fb.is_empty_batch = true;
                fallback_batches.push(std::move(fb));
                cv_fallback_ready.notify_one();
            }

            // Mark buffer as available again
            {
                std::lock_guard<std::mutex> lock(mtx);
                buffer_in_use[buf_id] = false;
                ready_output_buffers.push(buf_id);
            }
            {
                std::lock_guard<std::mutex> lock(mtx);
                gpu_done = true;
            }   
            cv_gpu_done.notify_all();
            cv_gpu_output_ready.notify_one();

            batch_idx++;
        }
        printf("GPU thread returned\n");
    });

    std::mutex fout_mutex;
    std::thread dump_thread([&]() {
        int dump_batch_idx = 0;
        while (true) {
            int buf_id = -1;
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv_gpu_output_ready.wait(lock, [&]() {
                    return !ready_output_buffers.empty() || (no_more_data && dump_batch_idx >= batch_idx_io);
                });

                if (ready_output_buffers.empty() && no_more_data && dump_batch_idx >= batch_idx_io) break;
            
                buf_id = ready_output_buffers.front();
                ready_output_buffers.pop();
            }

            const auto& out = host_output_buffers[buf_id];

            DEBUG_ONLY(
                std::cout << "[DUMP] Writing batch " << dump_batch_idx << " from buffer " << buf_id << std::endl;
            );

            auto out_start = std::chrono::high_resolution_clock::now();

            {
                std::lock_guard<std::mutex> fout_lock(fout_mutex);
                write_output_batch(fout, out, dump_batch_idx, batch_size * 10, max_seq_len, batch_size);
            }

            auto out_end = std::chrono::high_resolution_clock::now();
            double out_elapsed = std::chrono::duration<double>(out_end - out_start).count();
            total_out_time += out_elapsed;

            DEBUG_ONLY(
                std::cout << "[DUMP] Batch " << dump_batch_idx << " processed in " << out_elapsed << " sec\n";
            );
            
            {
                std::lock_guard<std::mutex> lock(mtx);
                output_buffer_in_use[buf_id] = false;
            }
            cv_dump_done.notify_all();

            dump_batch_idx++;
        }
        printf("OUTPUT thread returned\n");
    });

    int n_threads = 4;

    std::thread cpu_thread([&]() {
        int batch_idx_cpu = 0;
        while (true) {
            fallback_batch fb;
            {
                std::unique_lock<std::mutex> lock(fallback_mtx);
                cv_fallback_ready.wait(lock, [&]() {
                    return !fallback_batches.empty() || (no_more_data && batch_idx_cpu >= batch_idx_io);
                });

                if (fallback_batches.empty() && no_more_data && batch_idx_cpu >= batch_idx_io) break;

                fb = std::move(fallback_batches.front());
                fallback_batches.pop();
            }

            if(!(fb.is_empty_batch)){
                DEBUG_ONLY(
                    std::cout << "[CPU] Fallback processing " << fb.read_ids.size() << " reads from buffer " << fb.buf_id << std::endl;
                );

                auto cpu_start = std::chrono::high_resolution_clock::now();
                fallback_worker_t worker;
                worker.fout = &fout;
                worker.fout_mutex = &fout_mutex;
                worker.read_ids = fb.read_ids.data();
                worker.num_reads = fb.read_ids.size();
                worker.reads = fb.seqs;
                worker.mi = mi;
                worker.opt = &opt;
                worker.tbufs = new mm_tbuf_t*[n_threads];
                worker.batch_id = batch_idx_cpu;
                worker.batch_size = batch_size;

                for (int i = 0; i < n_threads; ++i)
                    worker.tbufs[i] = mm_tbuf_init();

                kt_for(n_threads, fallback_worker, &worker, worker.num_reads);

                for (int i = 0; i < n_threads; ++i)
                    mm_tbuf_destroy(worker.tbufs[i]);
                delete[] worker.tbufs;

                auto cpu_end = std::chrono::high_resolution_clock::now();
                double cpu_elapsed = std::chrono::duration<double>(cpu_end - cpu_start).count();

                DEBUG_ONLY(
                    std::cout << "[CPU] Batch " << batch_idx_cpu << " fallback processing time: " << cpu_elapsed << " sec\n";
                );
            }
            else {
                DEBUG_ONLY(
                    std::cout << "[CPU] Fallback processing " << fb.read_ids.size() << " reads from buffer - buffer is empty! " << fb.buf_id << std::endl;
                );
            }

            batch_idx_cpu++;
        }
        printf("CPU fallback thread returned\n");
    });

    io_thread.join();
    gpu_thread.join();
    cpu_thread.join();
    dump_thread.join();

    std::cout << "All batches processed.\n";

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_elapsed = std::chrono::duration<double>(end_total - start_total).count();

    std::cout << "\n==== Summary ====\n";
    std::cout << "Total I/O time   : " << total_io_time << " seconds\n";
    std::cout << "Total GPU time   : " << total_gpu_time << " seconds\n";
    std::cout << "Total wall time  : " << total_elapsed << " seconds\n";

    mm_idx_destroy(mi);
    mm_idx_reader_close(idx_rdr);

    // TODO : free allocated memory 

    return 0;
}