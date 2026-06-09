#include <fstream>
#include <iostream>

#include "args_parser.h"



Parameters::Parameters(int argc_, char **argv_) {


    // default values
    sa = (2);
    sb = (4);
    gapo = (4);
    gape = (2);

    print_out = (0);
    n_threads = (1);
    // For AGAThA
    slice_width = (3);
    z_threshold = (400);
    band_width = (751);
    kernel_align_num = (8192);
    kernel_block_num = (256);
    kernel_thread_num = (256);

    isPacked = false;
    isReverseComplement = false;

    query_batch_fasta_filename = "";
    target_batch_fasta_filename = "";
    raw_filename = "";

    argc = argc_;
    argv = argv_;

}

Parameters::~Parameters() {
    query_batch_fasta.close();
    target_batch_fasta.close();
    raw_file.close();
}

void Parameters::print() {
    std::cerr <<  "sa=" << sa <<" , sb=" << sb <<" , gapo=" <<  gapo << " , gape="<<gape << std::endl;
    std::cerr <<  "slice_width=" << slice_width << ", z_threshold=" << z_threshold << ", band_width=" << band_width << std::endl;
    std::cerr <<  "kernel launch: block_num=" << kernel_block_num << ", thread_num=" << kernel_thread_num << ", align_num=" << kernel_align_num << std::endl;
    std::cerr <<  "print_out=" << print_out <<" , n_threads=" <<  n_threads << std::endl;
    std::cerr <<  std::boolalpha << "isPacked = " << isPacked  << std::endl;
    std::cerr <<  "query_batch_fasta_filename=" << query_batch_fasta_filename <<" , target_batch_fasta_filename=" << target_batch_fasta_filename << std::endl;
}

void Parameters::failure(fail_type f) {
    switch(f)
    {
            case NOT_ENOUGH_ARGS:
                std::cerr << "Not enough Parameters. Required: -y AL_TYPE file1.fasta file2.fasta. See help (--help, -h) for usage. " << std::endl;
            break;
            case WRONG_ARG:
                std::cerr << "Wrong argument. See help (--help, -h) for usage. " << std::endl;
            break;
            case WRONG_FILES:
                std::cerr << "File error: either a file doesn't exist, or cannot be opened." << std::endl;
            break;

            default:
            break;
    }
    exit(1);
}

void Parameters::help() {
            std::cerr << "Usage: ./test_prog.out [-m] [-x] [-q] [-r] [-s] [-z] [-w] [-b] [-t] [-a] [-p] [-n] <query_batch.fasta> <target_batch.fasta>" << std::endl;
            std::cerr << "Options: -m INT    match score ["<< sa <<"]" << std::endl;
            std::cerr << "         -x INT    mismatch penalty [" << sb << "]"<< std::endl;
            std::cerr << "         -q INT    gap open penalty [" << gapo << "]" << std::endl;
            std::cerr << "         -r INT    gap extension penalty ["<< gape <<"]" << std::endl;
            std::cerr << "         -s        (AGAThA) slice_width" << std::endl;
            std::cerr << "         -z        (AGAThA) z-drop threshold" << std::endl;
            std::cerr << "         -w        (AGAThA) band width" << std::endl;
            std::cerr << "         -b        (AGAThA) number of blocks called per kernel" << std::endl;
            std::cerr << "         -t        (AGAThA) number of threads in a block called per kernel" << std::endl;
            std::cerr << "         -a        (AGAThA) number of alignments computed per kernel" << std::endl;
            std::cerr << "         -p        print the alignment results and time" << std::endl;
            std::cerr << "         -n INT    Number of CPU threads ["<< n_threads<<"]" << std::endl;
            std::cerr << "         --help, -h : displays this message." << std::endl;
            std::cerr << "Single-pack multi-Parameters (e.g. -sp) is not supported." << std::endl;
            std::cerr << "		  "  << std::endl;
}


void Parameters::parse() {

    // before testing anything, check if calling for help.
    int c;
        
    std::string arg_next = "";
    std::string arg_cur = "";

    for (c = 1; c < argc; c++)
    {
        arg_cur = std::string((const char*) (*(argv + c) ) );
        arg_next = "";
        if (!arg_cur.compare("--help") || !arg_cur.compare("-h"))
        {
            help();
            exit(0);
        }
    }

    if (argc < 4)
    {
        failure(NOT_ENOUGH_ARGS);
    }

    for (c = 1; c < argc - 3; c++)
    {
        arg_cur = std::string((const char*) (*(argv + c) ) );
        if (arg_cur.at(0) == '-' && arg_cur.at(1) == '-' )
        {
            if (!arg_cur.compare("--help"))
            {
                help();
                exit(0);
            }

        } else if (arg_cur.at(0) == '-' )
        {
            if (arg_cur.length() > 2)
                failure(WRONG_ARG);
            char param = arg_cur.at(1);
            switch(param)
            {
                case 'm':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    sa = std::stoi(arg_next);
                break;
                case 'x':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    sb = std::stoi(arg_next);
                break;
                case 'q':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    gapo = std::stoi(arg_next);
                break;
                case 'r':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    gape = std::stoi(arg_next);
                break;
                case 'p':
                    print_out = 1;
                break;
                case 'n':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    n_threads = std::stoi(arg_next);
                break;
                case 's':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    slice_width = std::stoi(arg_next);
                break;
                case 'z':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    z_threshold = std::stoi(arg_next);
                break;
                case 'w':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    band_width = std::stoi(arg_next);
                break;
                case 'b':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    kernel_block_num = std::stoi(arg_next);
                break;
                case 't':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    kernel_thread_num = std::stoi(arg_next);
                break;
                case 'a':
                    c++;
                    arg_next = std::string((const char*) (*(argv + c) ) );
                    kernel_align_num = std::stoi(arg_next);
                break;

            }

            
        } else {
            failure(WRONG_ARG);
        }
    }


    // the last 2 Parameters are the 2 filenames.
    query_batch_fasta_filename = std::string( (const char*)  (*(argv + c) ) );
    c++;
    target_batch_fasta_filename = std::string( (const char*) (*(argv + c) ) );

    if (print_out) {
        c++;
        raw_filename = std::string( (const char*) (*(argv + c) ) );
    }

    // Parameters retrieved successfully, open files.
    fileopen();
}

void Parameters::fileopen() {
    query_batch_fasta.open(query_batch_fasta_filename, std::ifstream::in);
    if (!query_batch_fasta)
        failure(WRONG_FILES);

    target_batch_fasta.open(target_batch_fasta_filename);
    if (!target_batch_fasta)
        failure(WRONG_FILES);

    if (print_out) {
        raw_file.open(raw_filename, std::ios::app);
    }
}
