#include "common_simd.h"
#include "common_tests.h"
#include "common.h"


import mlogging;
import sequan_compat;
import bioseq_sequences;
import bioseq_sequences.simd;
import testing;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;


TEMPLATE_TEST_CASE("Sequence SIMD itreration", "[sequences][simd]", uint8_t) {
    std::vector<seq::dna4> FIXED_TARGET5{
        "\
ACTGTAGCTGATCGTAGCTAGTCACTGTAGCT\
GTCGTAGCTAGTCCGTAATCGTAGCTAGTCAA\
ACTGTATCGTAGCTAGTCCGTAGCTACTGTAG\
CTGATCGTAGCTAGTCACTGTAGGATCGTAGC\
TAGTCCGTACTGTAGCTGATCGTAGCCCTAGT\
CACTGTAGCTGATCGTAGCTAGTCACTGTAGC\
TGATCGTAGCTAGTCTCGTAGCTAGTCCGTAA\
CTGTAGCTGATCGTAGCTAGTCACTGTAGCTG\
ATCGTAGCTAGTC\
"_dna4};
    auto testtest = compact_sequence_range<seq::dna4>(FIXED_TARGET5);
    JustLog("heheszke = {}", testtest);
    
    const hn::ScalableTag<typename decltype(testtest)::traits::storage_unit> d;
    std::string trails[256];
    iterate_sequence_bp_simd(testtest, testtest.begin(), testtest.end(), [&]<bool is_full>(std::bool_constant<is_full>, auto& d, auto& mask, auto&& incoming){ 
        using seq_traits = decltype(testtest)::traits;
        HWY_ALIGN seq_traits::storage_unit lanes_values[hn::MaxLanes(d)];
        HWY_ALIGN seq_traits::storage_unit lanes_mask[hn::MaxLanes(d)];
        hn::Store(incoming, d, lanes_values);
        hn::Store(hn::VecFromMask(d, mask), d, lanes_mask);
        for(size_t i=0;i<hn::Lanes(d);++i) {
            if (lanes_mask[i]) {
                trails[i] += seq_traits::letter_to_char(lanes_values[i]);
            }
        }
        //JustLog("got vec [{}] test: {} <{}>", is_full, hwy_str_as<seq::dna4>(d, incoming), hwy_str(d, hn::VecFromMask(d, mask))); //hwy_str_as<seq::dna4>(d, outgoing));
    });
    for(size_t i=0;i<hn::Lanes(d);++i) {
        JustLog("trace [{}] = {}", i, trails[i]);
    }
}


};

HWY_AFTER_NAMESPACE();