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


template<supports_compact_sequence_range R>
class sequence_simd_accumulator {
private:
    using seq_traits = compact_sequence_range<R>::traits;
    static constexpr size_t max_lanes = hn::MaxLanes(hn::ScalableTag<typename seq_traits::storage_unit>());
    
    std::array<std::vector<typename seq_traits::alphabet_type>, max_lanes> traces;
    compact_sequence_range<R>& source;
    size_t actual_lanes;
    size_t used_context;
public:

    sequence_simd_accumulator(
        compact_sequence_range<R>& seq
    ) : source(seq), actual_lanes(0), used_context(0) {};

    size_t context_size_bps() const {
        return used_context;
    };

    size_t lanes() const {
        return actual_lanes;
    };

    compact_sequence_range<R>& get_source() const {
        return source;
    };

    template<typename D, typename M, typename V>
    void _add_record(D& d, M& mask, V& incoming) {
        HWY_ALIGN typename seq_traits::storage_unit lanes_values[hn::MaxLanes(d)];
        HWY_ALIGN typename seq_traits::storage_unit lanes_mask[hn::MaxLanes(d)];
        hn::Store(incoming, d, lanes_values);
        hn::Store(hn::VecFromMask(d, mask), d, lanes_mask);
        std::string dbgstr = "";
        for(size_t i=0;i<hn::Lanes(d);++i) {
            if (lanes_mask[i]) {
                traces[i].push_back(seq_traits::letter_to_symbol(lanes_values[i]));
                dbgstr += std::string(1, seq_traits::letter_to_char(lanes_values[i]));
            } else {
                dbgstr += "X";
            }
            dbgstr += " ";
        }
        // JustLog("XDBG --> {}", dbgstr);
        actual_lanes = hn::Lanes(d);
    }

    template<typename I>
    //requires (compact_sequence_range<R>::template is_valid_derived_iter<I>::value == true)
    void accumulate(const I& begin, const I& end, const size_t context_bps) {
        used_context = context_bps;
        actual_lanes = 0;
        iterate_sequence_bp_simd_offsets(source, begin, end, context_bps, [&]<bool is_full>(std::bool_constant<is_full>, auto const& _offsets, auto& d, auto& mask, auto&& incoming){
            _add_record(d, mask, incoming);
        }, [](auto const& _offsets, auto& d, auto& mask){});
    };

    template<typename I>
    //requires (compact_sequence_range<R>::template is_valid_derived_iter<I>::value == true)
    void accumulate_delayed(const I& begin, const I& end, const size_t context_bps, const size_t delay) {
        used_context = context_bps;
        actual_lanes = 0;
        iterate_sequence_bp_simd_offsets(source, begin, end, context_bps, [&]<bool is_full>(std::bool_constant<is_full>, auto const& _offsets, auto& d, auto& _mask, auto&& _incoming, auto delayed_mask, auto delayed_incoming){
            _add_record(d, delayed_mask, delayed_incoming);
        }, [](auto const& _offsets, auto& d, auto& mask){}, delay);
    };

    std::vector<std::string> to_strings() const {
        std::vector<std::string> result;
        result.reserve(actual_lanes);
        for(size_t i=0;i<actual_lanes;++i) {
            result.push_back(symbols_vector_to_string(traces[i]));
        }
        return result;
    }

    std::vector<std::optional<std::pair<size_t, size_t>>> to_indices() const {
        std::vector<std::optional<std::pair<size_t, size_t>>> result;
        size_t start = 0;
        for(size_t i=0;i<actual_lanes;++i) {
            if (!traces[i].empty()) {
                const auto pos = source.slow_find_symbols_left(source.begin(), source.end(), traces[i], start);
                start = pos.value_or(source.bp_size());
                result.push_back(pos.transform([&](auto& pos_val){ return std::make_pair(pos_val, pos_val + traces[i].size()); }));
            } else {
                result.push_back(std::nullopt);
            }
        }
        return result;
    }
};

template<supports_compact_sequence_range R>
void assert_integrity_of_indices(const sequence_simd_accumulator<R>& acc, const std::string description) {
    const auto coverage = acc.to_indices();
    const auto traces = acc.to_strings();

    // JustLog("Y = {}", traces);
    // JustLog("I = {}", coverage);

    std::vector<std::pair<size_t, bool>> ends;
    ends.reserve(coverage.size() * 2);
    for (auto& el : coverage) {
        ASSERT(el.has_value(), fmt::format("All indices in coverage cannot be std::nullopt: {}", description), acc.get_source(), coverage, traces);
        const auto& [from, to] = el.value();
        ends.push_back(std::make_pair(from, true));
        ends.push_back(std::make_pair(to, false));

        ASSERT(from <= to, fmt::format("Indices are correct - from <= to for (from, to): {}", description), coverage, traces);
        ASSERT(to-from >= acc.context_size_bps(), fmt::format("Each interval has at least context bps: {}", description), coverage, traces);
    }

    if (acc.get_source().bp_size() < acc.context_size_bps()) {
        ASSERT(coverage.size() == 0, fmt::format("For sequence with size lower than context we don't run iterator at all: {}", description), acc.get_source().bp_size(), acc.context_size_bps(), coverage, traces);
        return;
    } else {
        ASSERT(coverage.size() == acc.lanes(), fmt::format("Coverage must contain no. of traces equal to number of lanes: {}", description), coverage, traces);
    }

    const size_t coverage_len = coverage.size();
    ASSERT(coverage[0].value().first == 0, fmt::format("Coverage should start at (0, _): {}", description), coverage, traces);
    ASSERT(coverage[coverage_len-1].value().second == acc.get_source().bp_size(), fmt::format("Coverage should end at (_, bp_size()): {}", description), coverage, traces);

    // Naive algorithm to check if we cover everything
    std::sort(ends.begin(), ends.end());
    int32_t opened = 0;
    bool covered = true;
    size_t i = 0;
    for(; i<ends.size(); ++i) {
        if (ends[i].second) {
            ++opened;
        } else {
            --opened;
        }
        if (opened == 0 && i!=ends.size()-1 && ends[i+1].first != ends[i].first) {
            covered = false;
            break;
        }
    }
    ASSERT(covered, fmt::format("Entire sequence must be covered, but after position {}, there's no coverage: {}", ends[i].first, description), coverage, ends, traces);

    // JustLog("coverage = {} / {}", coverage, traces);
};

// void assert_full_sequence_coverage(const int sequence_size, const std::vector<std::optional<std::tuple<size_t, size_t>>>& indices) {
//     ASSERT(indices.size() > 0, "sequence indices must be non-empty");
// };

std::vector<seq::dna4> DNA4_SEQ_SHORT{
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

std::vector<seq::dna5> DNA5_SEQ_SHORT{
    "\
ACTGTAGCTGATCGTAGCTAGTCACTGTAGCT\
GTCGTAGCTCCACACGGGGGGATCGTAGCTAGTCAA\
ACTGTATCGCCCCACACACTCCGTAGCTACTGTAG\
CTGATCGTAGCTAGTCACTGTAGGATCGTAGC\
TAGTCCGTACCTAGAATCGTAGCCCTAGT\
CACTGTAGCTGATCGTAGNCTAGTCACTGTAGC\
TGATCGTAGCGGGGGGTTTTTAGTCCGTAA\
CTGTAGGTGTATAGTGNATAGCTAGTCACTGTAGCTG\
ATCGTAGCTAGTCNNNNNA\
"_dna5};

std::vector<seq::dna4> DNA4_SEQ_MEDIUM{
    "ATAGGTGCACGCTTATGCGCCCATTCGGGATGGAACCGCGTGGGTTTAAGGGCGTTCACT\
AGGAAAAAGGCTGTAGTGCTACGGTAACGGAACCTACTAACGGTACTGCCACATCGACGG\
TCATTGAAAGAGACTATTAAACTGACCAGATCCCTTGAATGAGCGGGGTATCGATGAAAC\
CAACTCCCTAAGTGCGAGATTAGAGAGCTTTGGTACCCCCAGTATCTTCGATATCCGAGA\
CGGAATGCGGCGCTTCCATCAGAACTTCGTAGTGAACGGATTGAGAATGGGCGTAATTCG\
ACGTATAAAATACGTATTCTGAGCCATCCCACTAACTGATGAATGATGTTACGTACGGTG\
TGAGAAGTACAATGGACTCGCCGGTTCTCAGGGGTTCTGCATCTCGTACTCGTACACTAG\
GTGGGTGGCTTCATTTGAATGTATGTGACCAAGCTGGTGTGAAATGATCGTTCTCAAGCG\
GGCCCGGAGACCGTGTTTCCCGAACCGATCAGGCACGGTCATCAAACCGTTACGGCAGGG\
GCTAAGCCTAATCTTGGTCACGGTGGTCACTCGCCTAGGAATGGTTGGAGCGATGGGTGT\
TTTATTCGCAATTCTGGGGGGGACTTCATGGGCACATTAATACCGTTATTGGAAGTCACC\
AAAATAAGCGCGATGTGGTATTCAAACTATTGAAACTTGAATATGGTTAGAATTGATTGT\
GCGAGAGCTAAGAGCGGAAGTTACGAAGAATTCAGTGCCTGGTGATCTTTAATTCCGGTC\
CTTCACGGCGGCGCGAAGGATCCTGTGGGCACGCAGAACAACTGCGGCGCCTTAGACAAC\
CCCATGGGTATTATTGTATCGAGTGGAAATTCCACCAAGGGCCAATGTCGTTGGCATCGA\
GTATTTTACACTCAGTCCTATTCGAAGTCCCCGACTCAACCATCAGTCCGGTGTGGTCTC\
TCTACCGGCTTAATCGCCAGCCACTGAATTAGCTTCCAGCACGTGCTTCAATAGTCACGT\
GATAGGAGTAATTATGCCAGGTAGAATACTGAGCATGTAGCACATGATCTATCTCAGGAT\
GGCTACGAAGTGTTACCGAAAGGGAGTGCGTGCATTTAAGATACTAGAGCCTACACGTGT\
CATCTAACACGCGCTTTAGTTATTCGTCGCGGAACACTTTCCTTTCACAGTATCGCCCAT\
ACCGGCCTAACCTGAATACCTTAGAACTGCCTGCATGGGGATCCAATAGGATCTTGATTA\
CTATGGCGCTAAAGGCTGCTGAAGGTCGCCTGCCGACCCGGTCAATAACTTCGGTGCGTC\
GTACTAAACTATGCGATGCTCTGTGCACTGTAGGGACGACGATACATGCCCTCGGGGCCC\
TCGACCGTTACTATCAGACAGGAGAAAACGTTAAATCGGGCGTACCCGGCCACAAAATGG\
GCGCCCCAATTTTCTGAGCAGCACGAAGCATTGTGTGGGCATCCTGAGTTAAAGGGAGGC\
TAACCCAGAGGCCCAATCGTGAACGAGTTAAACTGAGCGCGAAGATAGTAGTGCGCGCCG\
GACAGTTCTGGGTTTAGGGGCACATCGAAAAGTATTTGCGAATACGACGAGACAACCCAT\
ATCACGCCTGGGGCGTAGATCAGTTTGTGAGTGGCGAGCGCCCCCATCGTCTCCGAGTGG\
CATGGTCATTAGCGCTATACATGATCCCTGGCGCTAACTGGCTTTGCCACAAGAACTCTC\
TGCCCACCTGCGGCGTTTGGACGCCTGCCCCATAGTTAACAAAAGGACTACGAAGTTGTT\
TGGCACACTACTGGACTAATGTCCGATTCTATTGGTAGTTCCTGCGCCCATACAGTCGGC\
TCTCTTTGAAGGCCGCAGTAGTGTATCGAGTGCTCCTCGAAACTACTATGTCCGTCGGTT\
CTAGTTGCTATGTTCTTGATGTTTCGTGGAAGACCTCGATTGCTGCCACTTTGCAGGCGA\
TTGCTTGTACGGACCGCGTAGGTGACGATTAACAATTTTGCGATCTTACGCTCACGGGTG\
CAACACTACATCTGTGCTCAATCCACAGGAGTCATTACAGTGAGTGCACTGCCGCTTTCC\
GTGCAGCAGATTACTCGACGGCCCATGAGAATGTTTGACCCTAAGTCGCGCGAATGCACG\
TAATTGCGCAACCGGTCCCCGAAGGAGCAATCAAGGAGCGAGACCTTTAAGAGACATGGT\
GCTCATGAATCGCGTGCTCTTTAACTCCCCCCGTCTTGCTCGAAACCGCCGTCTATAGGC\
GCGTGCCATACCCAAGCGCACCTAACAGCTCTAAGTTGGTGGATGCTGTCGGGTACCTTC\
CACCCCAAATATGTCCGTGTCCCCATACTTGTGCGTCGCCCGCTCATCGCTACGCCGCTA\
ACAAATCCCTCGACGTGGCTATGTGTAAAATCCATGCTTTATATGCTGGGCGCGTCCGTG\
GCAGACGTGATGGTTAGTCGCTATCACAGCTAAAAACCGA"_dna4};

template<typename T>
void test_sequence(T testtest) {
    const size_t seq_len = testtest.bp_size();
    SECTION("Context bigger than size()") {
        sequence_simd_accumulator acc(testtest);
        const size_t context_bps = GENERATE_COPY(seq_len+1, seq_len*2);
        acc.accumulate(testtest.begin(), testtest.end(), context_bps);
        assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}", context_bps, seq_len));
    }
    SECTION("Context exactly equal to size()") {
        sequence_simd_accumulator acc(testtest);
        const size_t context_bps = seq_len;
        acc.accumulate(testtest.begin(), testtest.end(), context_bps);
        assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}", context_bps, seq_len));
    }
    SECTION("Context smaller than size(), but very close to") {
        sequence_simd_accumulator acc(testtest);
        const size_t context_bps = GENERATE_COPY(seq_len-1, seq_len-2, seq_len-3, seq_len-4, seq_len-8);
        acc.accumulate(testtest.begin(), testtest.end(), context_bps);
        assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}", context_bps, seq_len));
    }
    SECTION("Context smaller than size() (usual case)") {
        sequence_simd_accumulator acc(testtest);
        const size_t context_bps = GENERATE_COPY(0, 1, 2, 5, 10, seq_len/2);
        acc.accumulate(testtest.begin(), testtest.end(), context_bps);
        assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}", context_bps, seq_len));
    }
    SECTION("Correct delay handling for context smaller than size() (usual case)") {
        sequence_simd_accumulator acc(testtest);
        const size_t context_bps = 5;
        const size_t delay = GENERATE(0, 1, 3, 5);
        acc.accumulate_delayed(testtest.begin(), testtest.end(), context_bps, delay);
        assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}, delay = {}", context_bps, seq_len, delay));
    }
};

// TEST_CASE("DDDDBGDBGDG!!!!! XASDAS", "[simd-new-x]") {
//     auto testtest = compact_sequence_range<std::vector<seq::dna4>>(DNA4_SEQ_MEDIUM);
//     const size_t seq_len = testtest.bp_size();
//     sequence_simd_accumulator acc(testtest);
//     const size_t context_bps = 0;
//     acc.accumulate(testtest.begin(), testtest.end(), context_bps);
//     assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}", context_bps, seq_len));
// };

// TEST_CASE("DDDDBGDBGDG!!!!! z", "[simd-new-x]") {
//     auto testtest = compact_sequence_range<seq::dna4>(DNA4_SEQ_MEDIUM);
//     const size_t seq_len = testtest.bp_size();
//     sequence_simd_accumulator acc(testtest);
//     const size_t context_bps =seq_len-8;
//     acc.accumulate(testtest.begin(), testtest.end(), context_bps);
//     assert_integrity_of_indices(acc, fmt::format("context_bps = {}, seq_len = {}", context_bps, seq_len));
// };

TEMPLATE_TEST_CASE("Sequence SIMD full itreration for short sequence DNA4", "[simd-new-x]", seq::dna4, std::vector<seq::dna4>) {
    test_sequence(compact_sequence_range<TestType>(DNA4_SEQ_SHORT));
};

TEMPLATE_TEST_CASE("Sequence SIMD full itreration for short sequence DNA5", "[simd-new-x]", seq::dna5, std::vector<seq::dna5>) {
    test_sequence(compact_sequence_range<TestType>(DNA5_SEQ_SHORT));
};

TEMPLATE_TEST_CASE("Sequence SIMD full itreration for medium sequence DNA4", "[simd-new-x]", seq::dna4, std::vector<seq::dna4>) {
    test_sequence(compact_sequence_range<TestType>(DNA4_SEQ_MEDIUM));
};

// TEMPLATE_TEST_CASE("LOLTEST", "[simd-new-x]", std::vector<seq::dna4>) {
//     test_sequence(compact_sequence_range<TestType>(DNA4_SEQ_SHORT));
// };


// TEMPLATE_TEST_CASE("Sequence SIMD itreration", "[sequences][simd]", uint8_t) {
//     std::vector<seq::dna4> FIXED_TARGET5{
//         "\
// ACTGTAGCTGATCGTAGCTAGTCACTGTAGCT\
// GTCGTAGCTAGTCCGTAATCGTAGCTAGTCAA\
// ACTGTATCGTAGCTAGTCCGTAGCTACTGTAG\
// CTGATCGTAGCTAGTCACTGTAGGATCGTAGC\
// TAGTCCGTACTGTAGCTGATCGTAGCCCTAGT\
// CACTGTAGCTGATCGTAGCTAGTCACTGTAGC\
// TGATCGTAGCTAGTCTCGTAGCTAGTCCGTAA\
// CTGTAGCTGATCGTAGCTAGTCACTGTAGCTG\
// ATCGTAGCTAGTC\
// "_dna4};
//     auto testtest = compact_sequence_range<seq::dna4>(FIXED_TARGET5);
//     JustLog("heheszke = {}", testtest);
    
//     const hn::ScalableTag<typename decltype(testtest)::traits::storage_unit> d;
//     std::string trails[256];
//     iterate_sequence_bp_simd(testtest, testtest.begin(), testtest.end(), [&]<bool is_full>(std::bool_constant<is_full>, auto& d, auto& mask, auto&& incoming){ 
//         using seq_traits = decltype(testtest)::traits;
//         HWY_ALIGN seq_traits::storage_unit lanes_values[hn::MaxLanes(d)];
//         HWY_ALIGN seq_traits::storage_unit lanes_mask[hn::MaxLanes(d)];
//         hn::Store(incoming, d, lanes_values);
//         hn::Store(hn::VecFromMask(d, mask), d, lanes_mask);
//         for(size_t i=0;i<hn::Lanes(d);++i) {
//             if (lanes_mask[i]) {
//                 trails[i] += seq_traits::letter_to_char(lanes_values[i]);
//             }
//         }
//         //JustLog("got vec [{}] test: {} <{}>", is_full, hwy_str_as<seq::dna4>(d, incoming), hwy_str(d, hn::VecFromMask(d, mask))); //hwy_str_as<seq::dna4>(d, outgoing));
//     });
//     for(size_t i=0;i<hn::Lanes(d);++i) {
//         JustLog("trace [{}] = {}", i, trails[i]);
//     }
// }


};

HWY_AFTER_NAMESPACE();