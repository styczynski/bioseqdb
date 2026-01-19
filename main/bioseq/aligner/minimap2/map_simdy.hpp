#include <hwy/highway.h>


HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

static inline void check_simd_packed_seq() {

    std::vector<seq::dna4> FIXED_TARGET{"ACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGTCGTAGCTAGTCCGTAATCGTAGCTAGTCAAACTGTATCGTAGCTAGTCCGTAGCTACTGTAGCTGATCGTAGCTAGTCACTGTAGGATCGTAGCTAGTCCGTACTGTAGCTGATCGTAGCCCTAGTCACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTCTCGTAGCTAGTCCGTAACTGTAGCTGATCGTAGCTAGTCACTGTAGCTGATCGTAGCTAGTC"_dna4};
	
    auto seq = CompressedSeqanView(FIXED_TARGET);
    for (auto i = seq.begin(); i < seq.end(); ++i) {
        JustLog("v = {}", seq.get_chunk(i));
    }

    JustLog("Check check_simd_packed_seq()");
}

} // namespace BPCells::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();