/**
 * @file minimizer_index.cppm
 * @brief Shared minimizer lookup/persistence concepts and runtime segment range.
 *
 * Lives in `bioseq::indexing`; Highway-namespace consumers reference it
 * cross-namespace (the concept names only a POD value type plus a concept, not
 * Highway intrinsics). NO_POSTGRES.
 */
module;
#include <hwy/highway.h>            // ONLY for the HWY_NAMESPACE macro (cf. sidecar.cppm)
#include <span>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <vector>
export module bioseq.indexing.minimizer_index;
import bioseq.sequences.kmers;     // MinimapSketchData (POD), via the HWY_NAMESPACE using below
import bioseq.storage.size;        // sequence_size_t
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

/// Any read-only minimizer lookup structure usable by seed/pipeline consumers.
template <typename T>
concept MinimizerLookupIndex = requires (
    const T& const_index,
    std::uint64_t kmer_hash) {
    { const_index.lookup(kmer_hash) }        -> std::convertible_to<std::span<const std::uint64_t>>;
    { const_index.target_length() }          -> std::convertible_to<bioseq::storage::sequence_size_t>;
};

/// A lookup index that can also be built and persisted in the legacy binary form.
template <typename T>
concept PersistableMinimizerIndex = MinimizerLookupIndex<T> && requires (
    T& mutable_index,
    const T& const_index,
    const std::vector<MinimapSketchData>& sketches,
    bioseq::storage::sequence_size_t target_length_arg,
    char* output_buffer,
    const char* input_buffer,
    std::size_t input_size) {
    { mutable_index.build(sketches, target_length_arg) } -> std::same_as<void>;
    { const_index.serialize_binary_size() }  -> std::convertible_to<std::size_t>;
    { const_index.serialize_to_binary(output_buffer) } -> std::same_as<void>;
    { T::deserialize_from_binary(input_buffer, input_size) } -> std::same_as<T>;
};

/// Runtime collection/core geometry shared across every Highway target.
struct SegmentUnitRange {
    bioseq::storage::sequence_size_t cs_u;
    bioseq::storage::sequence_size_t ce_u;
    bioseq::storage::sequence_size_t core_lo_bp;
    bioseq::storage::sequence_size_t core_hi_bp;
};

}  // namespace bioseq::indexing
