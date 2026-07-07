/**
 * @file minimizer_index.cppm
 * @brief DOC0279 §7.1 — the relocated `MinimizerIndex` concept (moved out of
 *        minimap_port.cppm without generalising its build signature).
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

/// Any type usable as a minimizer lookup structure on the aligner hot path
/// (collect_seed_hits). Consumers template on the concrete type — zero vtable.
template <typename T>
concept MinimizerIndex = requires (
    T& mutable_index, const T& const_index,
    const std::vector<MinimapSketchData>& sketches,    // the EXISTING TargetIndex::build signature
    bioseq::storage::sequence_size_t target_length_arg,
    std::uint64_t kmer_hash, char* output_buffer,
    const char* input_buffer, std::size_t input_size)
{
    { mutable_index.build(sketches, target_length_arg) } -> std::same_as<void>;
    { const_index.lookup(kmer_hash) }        -> std::convertible_to<std::span<const std::uint64_t>>;
    { const_index.target_length() }          -> std::convertible_to<bioseq::storage::sequence_size_t>;
    { const_index.serialize_binary_size() }  -> std::convertible_to<std::size_t>;
    { const_index.serialize_to_binary(output_buffer) } -> std::same_as<void>;
    { T::deserialize_from_binary(input_buffer, input_size) } -> std::same_as<T>;
};
}  // namespace bioseq::indexing
