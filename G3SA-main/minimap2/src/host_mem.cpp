#include "host_mem.h"

void allocate_host_input_memory(char** h_seqs, int** h_lens, int** h_ofs, int max_seq_len, int batch_size, int num_device){
    
    cudaMallocHost(h_seqs, sizeof(char)*max_seq_len*batch_size*num_device);
    cudaMallocHost(h_lens, sizeof(int)*batch_size*num_device);
    cudaMallocHost(h_ofs, sizeof(int)*batch_size*num_device);

    printf("Allocated pinned host memory of size (%d * %d) bp\n", num_device, max_seq_len * batch_size);
}

void allocate_host_output_memory(int** h_score, int** h_n_cigar, uint32_t** h_cigar, int max_seq_len, int batch_size, int num_device){
    // very generous estimation of output size
    cudaMallocHost(h_cigar, sizeof(uint32_t)*batch_size*num_device*20*max_seq_len);
    cudaMallocHost(h_n_cigar, sizeof(int)*batch_size*20);
    cudaMallocHost(h_score, sizeof(int)*batch_size*20);
}

void fill_host_batch(mm_bseq_file_t ** fp, char* h_seqs, int* h_lens, int* h_ofs, int max_seq_len, int batch_size, int num_device){
    int seq_data_index = 0;      // Index for the seq_data array
    int seq_lens_index = 0;      // Index for the seq_lens array
    int offset_index = 0;        // Index for the offset array
    int batch_info_index = 0;    // Index for the batch_info array

    int total_reads = batch_size * num_device;
    int n_read = 0;
    int n_seq;
    int with_qual = 1;

    //printf("Start filling host pinned memory\n");

    int ofs = 0;
    h_ofs[0] = ofs; // very first offset
    mm_bseq1_t *seqs;

    while (n_read < total_reads) {
        seqs = mm_bseq_read(fp[0], 1, with_qual, &n_seq);

        if (n_read % batch_size == 0) { // first read in a batch
            seq_data_index = max_seq_len * batch_size * batch_info_index;
            // Reset batch-relative offset
            seq_lens_index = batch_size * batch_info_index;
            offset_index = batch_size * batch_info_index;
            ofs = 0;
            h_ofs[offset_index++] = ofs;
        }
        h_lens[seq_lens_index++] = seqs->l_seq;


        n_read++;

        // Store sequence data at batch-relative position
        for (int i = 0; i < seqs->l_seq; i++) {
            h_seqs[seq_data_index++] = seqs->seq[i];
            ofs++;
        }

        // Pad the remaining sequence data in the batch with 255
        for (int i = 0; i < MM_BUFFER_SIZE - seqs->l_seq % (MM_TILE_SIZE); i++) {
            h_seqs[seq_data_index++] = 255;
            ofs++;
        }
        h_ofs[offset_index++] = ofs;

        if (n_read % batch_size == 0) { // last read in a batch
            batch_info_index++;
        }
    }

    return;
}

void write_output_batch(std::ofstream& fout, const host_output_buffer& out, int batch_id, int chunk_size, int max_cigar_len, int batch_size) {

    std::string line;
    line.reserve(max_cigar_len * 5);  // reserve for reuse

    for (int i = 0; i < chunk_size; ++i) {
        line.clear();

        if(out.h_n_cigar[i] != 0){
            if(out.h_score[i] < 0) printf("err: invalid read idx\n");
            line += "[read id:";
            line += std::to_string(out.h_score[i] + batch_id * batch_size); // read id : temporary TODO: add read information
            line += "]\t";
            for (int j = 0; j < out.h_n_cigar[i]; ++j) {
                uint32_t c = out.h_cigar[i * max_cigar_len + j];

                int len = static_cast<int>(c >> 4);
                char op = MM_CIGAR_STR[c & 0xf];

                line += std::to_string(len);
                line += op;
            }
            fout << line << '\n';
        }
    }

    fout.flush(); // optionally flush for each batch (can remove if batching further)
}
