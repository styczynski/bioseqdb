/** Static-target facade for minimizer hasher foundations. */
module;

#include <hwy/highway.h>

#include "common.h"
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.hashers.base;

import bioseq.common.sequan_compat;
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.simd.utils;

// The marked source is the single lexical owner. Replay it only after this
// independent facade has fixed Highway's static target; vector-bearing types
// therefore never cross a target-neutral module boundary.
HWY_BEFORE_NAMESPACE();

#define BIOSEQ_HASHER_BASE_STATIC_FACADE 1
#define target HWY_NAMESPACE
// Suppress only the canonical source's required leading global fragment.
#define module
#include "highway_base_kernel.cppm"
#undef module
#undef target
#undef BIOSEQ_HASHER_BASE_STATIC_FACADE

HWY_AFTER_NAMESPACE();

export namespace bioseq::simd {

using HWY_NAMESPACE::char_to_nt4;
using HWY_NAMESPACE::hasher_base;
using HWY_NAMESPACE::kNt4Sentinel;
using HWY_NAMESPACE::make_nt4_lut;
using HWY_NAMESPACE::nt4_lut_v;
using HWY_NAMESPACE::nt4_normalize;
using HWY_NAMESPACE::simd_hasher_base;
using HWY_NAMESPACE::supports_scalar_hash;
using HWY_NAMESPACE::supports_simd_hash;

} // namespace bioseq::simd

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {

// Independent static-target test/support API. Production-generated target
// partitions do not need this collection helper, so it remains in the facade
// while all shared hasher algorithm bodies come from the canonical owner.
template<
    typename I,
    template<typename> typename Hasher,
    supports_compact_sequence_range R>
auto run_all_hashers_on_sequence(
    const compact_sequence_range<R>& seq,
    const std::size_t kmer_length)
{
    using seq_traits = typename compact_sequence_range<R>::traits;
    hn::ScalableTag<I> d;
    typename Hasher<typename seq_traits::alphabet_type>::
        template scalar_hash<I> scalar_hasher(kmer_length);
    typename Hasher<typename seq_traits::alphabet_type>::
        template simd_hash<I, decltype(d)> simd_hasher(d, kmer_length);

    std::vector<std::pair<std::size_t, I>> scalar_results;
    std::vector<std::pair<std::size_t, I>> simd_results;
    scalar_results.reserve(seq.bp_size());
    simd_results.reserve(seq.bp_size());

    std::size_t pos = 0u;
    for (auto const& nucl : seq.template iterable<BP>()) {
        const auto [hash_value, strand] = scalar_hasher.next(
            static_cast<I>(nucl), static_cast<I>(nucl));
        (void)strand;
        if (pos >= kmer_length) {
            scalar_results.emplace_back(pos, hash_value);
        }
        ++pos;
    }

    pos = 0u;
    iterate_sequence_bp_simd_offsets(
        seq, seq.begin(), seq.end(), kmer_length,
        [&]<bool IsFull>(
            std::bool_constant<IsFull>,
            auto const& offsets,
            auto& descriptor,
            auto& mask,
            auto&& incoming,
            [[maybe_unused]] auto outgoing_mask,
            auto outgoing) {
            const auto hash_value = simd_hasher.next(
                descriptor, incoming, outgoing);
            if (pos >= kmer_length) {
                const auto values = extract_lanes<I>(descriptor, hash_value);
                const auto mask_values = extract_lanes<I>(descriptor, mask);
                std::size_t index = 0u;
                for (const auto value : values) {
                    if (mask_values[index]) {
                        simd_results.emplace_back(
                            offsets[index] * seq_traits::letters_per_unit + pos,
                            value);
                    }
                    ++index;
                }
            }
            ++pos;
        },
        [](
            [[maybe_unused]] auto const& offsets,
            [[maybe_unused]] auto& descriptor,
            [[maybe_unused]] auto& mask) {},
        0);

    std::sort(simd_results.begin(), simd_results.end());
    return std::make_tuple(
        std::move(simd_results), std::move(scalar_results));
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

} // export

export namespace bioseq::simd {
using HWY_NAMESPACE::run_all_hashers_on_sequence;
} // namespace bioseq::simd
