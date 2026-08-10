/** @file bench_e2e_main.cpp @brief Thin launcher for the E2E benchmarks. */

import bioseq.bench.e2e;

#ifndef BIOSEQ_E2E_CONTRACT_TEST
int main(int argc, char** argv) {
    return bioseq::bench::e2e::run(argc, argv);
}
#endif
