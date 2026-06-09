#ifndef ARGS_PARSER_H
#define ARGS_PARSER_H

/*
#include <stdint.h>


#include "gasal.h"
*/
#include <fstream>
#include <iostream>
#include "gasal.h"
#include <string.h>


enum fail_type {
    NOT_ENOUGH_ARGS,
    TOO_MANY_ARGS,
    WRONG_ARG,
    WRONG_FILES,
    WRONG_ALGO
};

class Parameters{

    public: 
        Parameters(int argc, char** argv);
        ~Parameters();
        void print();
        void failure(fail_type f);
        void help();
        void parse();
        void fileopen();

        int32_t sa;
        int32_t sb;
        int32_t gapo;
        int32_t gape;

        int print_out;
        int n_threads;

        int slice_width;
        int z_threshold;
        int band_width;

        int32_t kernel_block_num;
        int32_t kernel_thread_num;
        int32_t kernel_align_num;

        bool isPacked;
        bool isReverseComplement;

        std::string query_batch_fasta_filename;
        std::string target_batch_fasta_filename;
        std::string raw_filename;

        std::ifstream query_batch_fasta;
        std::ifstream target_batch_fasta;
        std::ofstream raw_file;


    protected:

    private:
        int argc;
        char** argv;
};


#endif
