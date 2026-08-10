/** @file bench_v10_main.cpp @brief Thin launcher for the aligner benchmarks. */

import bioseq.bench.aligners;

int main(int argc, char** argv) {
    return bioseq::bench::aligners::run(argc, argv);
}
