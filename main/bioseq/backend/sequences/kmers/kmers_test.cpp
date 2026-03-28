#include "common_simd.h"
#include "common_tests.h"
#include "common.h"


import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import bioseq_sequences.kmers;
import testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

TEST_CASE("HEHESZKI", "[heheszki-2]") {
    std::vector<seq::dna4> FIXED_TARGET{"GATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCAAA"_dna4};
    //std::vector<seq::dna4> FIXED_TARGET{"ACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCGTAGCTACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTC"_dna4};
	
    // auto query_compact = compact_sequence_range<seq::dna4>(FIXED_TARGET);
    // collect_minimizers<bioseq::simd::N_NEON::lexic_hasher>(
	// 	query_compact,
	// 	query_compact.begin(),
	// 	query_compact.end(),
	// 	10,
	// 	15
	// );

    auto query_compact2 = compact_sequence_range<seq::dna4>(FIXED_TARGET);
    bioseq::simd::N_NEON::collect_minimizers_simd<bioseq::simd::N_NEON::lexic_hasher>(
		query_compact2,
		query_compact2.begin(),
		query_compact2.end(),
		10,
		15
	);
};

};

HWY_AFTER_NAMESPACE();