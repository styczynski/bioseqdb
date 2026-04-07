#include "common_simd.h"
#include "common_tests.h"
#include "common.h"


import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers;
import bioseq_sequences.kmers.hashers;
import testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

TEST_CASE("HEHESZKI", "[heheszki-2]") {
    sketch_collector collector;
    std::vector<seq::dna4> FIXED_TARGET{"GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTC"_dna4};
    //std::vector<seq::dna4> FIXED_TARGET{"ACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCGTAGCTACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTC"_dna4};
	
    // auto query_test = compact_sequence_range<sequence_custom_alphabet<seq::dna4, uint64_t>>(FIXED_TARGET);
    // for(auto el : query_test.template iterable<SYMBOL>()) {
    //     JustLog("el = {}", el);
    // }

    // return;

    //using hasher = bioseq::simd::N_NEON::minimap_int_hasher; // bioseq::simd::N_NEON::lexic_hasher;

    auto query_compact = compact_sequence_range<sequence_custom_alphabet<seq::dna4, uint32_t>>(FIXED_TARGET);
    collect_minimizers<bioseq::simd::N_NEON::minimap_int_hasher>(
        collector,
		query_compact,
		query_compact.begin(),
		query_compact.end(),
		10,
		15
	);
    auto pairs_scalar = collector.extract_pairs();
    // minimap_int_hasher
    //return;

    collector.clear();
    auto query_compact2 = compact_sequence_range<sequence_custom_alphabet<seq::dna4, uint32_t>>(FIXED_TARGET);
    bioseq::simd::N_NEON::collect_minimizers_simd<bioseq::simd::N_NEON::minimap_int_hasher>(
        collector,
		query_compact2,
		query_compact2.begin(),
		query_compact2.end(),
		10,
		15
	);
    auto pairs_simd = collector.extract_pairs();
    
    ASSERT(pairs_scalar == pairs_simd, "SIMD should return exact same sketches as scalar version");
};

};

HWY_AFTER_NAMESPACE();