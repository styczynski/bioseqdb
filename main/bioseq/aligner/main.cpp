// #include <iostream>

#include "common_simd.h"
#include "common.h"
#include "./bioseq_sequence.hpp"

#include "./minimap2/main.cpp"

#include <vector>

import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.minimap_port;

using namespace seq::literals;

int k;

int main(int argc, char *argv[]) {
    log_configure();
    EASY_PROFILER_ENABLE;
    enable_logging = true;
    JustLog("Hello {}", "world :0");

    // --- Phase 1: Baseline — run the original minimap2 C pipeline ---
    // This path exercises map.c / lchain.c / sketch.c and dominates the
    // profile output for the "minimap2 baseline" timeline.
    int status;
    {
        EASY_BLOCK("call_main");
        status = call_main(argc, argv);
    }

    // --- Phase 2: Our C++ port — run minimap_port with hardcoded sequences ---
    // The query/target are hardcoded (same sequences that were previously
    // wired into map.c) so that we always compare the two implementations on
    // identical inputs. Profile blocks named mm_sketch / mg_lchain_* /
    // collect_seed_hits / ... correspond one-to-one with the minimap2 blocks
    // above so that the two phases can be compared side-by-side.
    std::vector<seq::dna4> FIXED_TARGET{"ACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTACCCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCCATACCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCCCAGGAGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTACCCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCCATACCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCCCAGGAGTAGCTAGTCACTGCCACACCATGTAGTCACGTGGGCTAGTTTTTTTATATATTATATTAATTTAGGATATTTGATGATTAGTTAGATGATTAGTGATCCCATCGTAAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTACCCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCCATACCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCCCAGGAGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCCACTCGTAGCTACCCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCCATACCTGATTTTTTTGTATATAGCTACGTAGCTTAGAGTCGATTGCGGCGCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTCCGCGCTATATACGCATCGATCGATCGATCGTATTAATTTCCCCCCCCCACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTTTAGACTCATATCGCTACTCTGA"_dna4};
    std::vector<seq::dna4> FIXED_QUERY{"TCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCCTGATCGTAGCTAGTCACTGTAGCCATACCTGAA"_dna4};

    auto target_compact = compact_sequence_range<sequence_custom_alphabet<seq::dna4, uint64_t>>(FIXED_TARGET);
    auto query_compact  = compact_sequence_range<sequence_custom_alphabet<seq::dna4, uint64_t>>(FIXED_QUERY);

    auto new_results = bioseq::simd::HWY_NAMESPACE::minimap_port(
        query_compact,
        target_compact
    );
    new_results.print();

    profiler::dumpBlocksToFile("bioseq.prof");
    return status;
}
