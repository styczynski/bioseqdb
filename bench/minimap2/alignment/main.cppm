/** @file bench/minimap2/alignment/main.cppm @brief Thin launcher for alignment benchmarks. */

import bioseq.bench.minimap2.alignment;

int main(int argc, char** argv) {
    return bioseq::bench::alignment::run(argc, argv);
}
