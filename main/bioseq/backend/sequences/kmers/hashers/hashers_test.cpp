#include "common_simd.h"
#include "common_tests.h"
#include "common.h"


import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.sequences;

import bioseq.sequences.kmers.hashers.base;
import bioseq.sequences.kmers.hashers.lexic;
import bioseq.sequences.kmers.hashers.minimap_int;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;


extern std::vector<seq::dna4> DNA4_SEQ_SHORT;

TEMPLATE_TEST_CASE("Lexic hasher works the same way for SIMD and scalar", "[hasher]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    auto seq = compact_sequence_range<sequence_custom_alphabet<seq::dna4, unit_t>>(DNA4_SEQ_SHORT);
    const auto& [simd, scalar] = run_all_hashers_on_sequence<unit_t, lexic_hasher>(seq, 10);
    assert_range_set_equality(scalar, simd);
};

TEMPLATE_TEST_CASE("Minimap INT hasher works the same way for SIMD and scalar", "[hasher]", uint8_t, uint16_t, uint32_t, uint64_t) {
    using unit_t = TestType;
    auto seq = compact_sequence_range<sequence_custom_alphabet<seq::dna4, unit_t>>(DNA4_SEQ_SHORT);
    const auto& [simd, scalar] = run_all_hashers_on_sequence<unit_t, minimap_int_hasher>(seq, 10);
    assert_range_set_equality(scalar, simd);
};

};

HWY_AFTER_NAMESPACE();