/** @file bench/e2e/main.cppm @brief Thin launcher for end-to-end benchmarks. */

import bioseq.bench.e2e;

int main(int argc, char** argv) {
    return bioseq::bench::e2e::run(argc, argv);
}
