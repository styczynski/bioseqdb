#ifndef BIOSEQ_BACKEND_SEQUENCES_CHAINING_SIMD_ABI_H_
#define BIOSEQ_BACKEND_SEQUENCES_CHAINING_SIMD_ABI_H_

#include <cstddef>

namespace bioseq::chaining {
struct Anchor;
struct Chain;
struct ChainingParams;
struct SimdChainingExecutionProof;
template<class Element>
struct ArenaSlab;
}  // namespace bioseq::chaining

namespace bioseq::simd {

// Target-neutral request for the complete chaining family.  It contains only
// pointers/scalars and neutral model types; Highway vectors, tags, and target
// namespaces remain wholly inside ChainAnchorsSimdTarget.
struct ChainAnchorsSimdRequest {
    const bioseq::chaining::Anchor* anchors = nullptr;
    std::size_t anchor_count = 0;
    const bioseq::chaining::ChainingParams* params = nullptr;
    bioseq::chaining::ArenaSlab<bioseq::chaining::Chain>* chains_out =
        nullptr;
    bioseq::chaining::ArenaSlab<bioseq::chaining::Anchor>*
        reordered_anchors_out = nullptr;
    bioseq::chaining::SimdChainingExecutionProof* proof = nullptr;
    bool aggregate_proof = false;
};

}  // namespace bioseq::simd

#endif  // BIOSEQ_BACKEND_SEQUENCES_CHAINING_SIMD_ABI_H_
