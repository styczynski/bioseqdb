/**
 * @file
 * @brief Copies Highway vector lanes into ordinary C++ vectors.
 */
module;

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <hwy/aligned_allocator.h>
#include <hwy/highway.h>
#include <limits>
#include <span>
#include <vector>

export module bioseq.simd.utils;

import bioseq.common.templating;

export namespace bioseq::simd {

/** Whether a bounded equality scan must reject ranks outside a supplied alphabet. */
enum class sequence_rank_validation : std::uint8_t { none, upper_bound };

/** Counts from a bounded SIMD equality scan, without allocating full sequences. */
struct sequence_symbol_equality {
    std::uint64_t equal_symbols{};
    std::size_t vector_batches{};
    bool valid_ranks{true};
};

/** Scalar-only ABI for a caller-selected Highway rank comparison kernel. */
using sequence_rank_equality_kernel = sequence_symbol_equality (*)(
    const std::uint8_t*, const std::uint8_t*, std::size_t,
    sequence_rank_validation, std::size_t);

/**
 * Count selected sequence windows through bounded byte readers and a selected
 * target-owned numeric kernel. No Highway tag/vector crosses this neutral API.
 * The fixed I/O chunk size is independent of vector width; target kernels own
 * their own MaxLanes-sized final-vector scratch.
 */
template<bool p_query_reversed,
         sequence_rank_validation p_validation = sequence_rank_validation::none,
         class QueryReader, class TargetReader>
[[nodiscard]] inline sequence_symbol_equality count_equal_sequence_symbols(
        const QueryReader& query, const TargetReader& target,
        std::size_t query_start, std::size_t target_start, std::size_t length,
        sequence_rank_equality_kernel counter, std::size_t rank_limit = 0u) {
    constexpr std::size_t chunk_capacity = 1024u;
    std::array<std::uint8_t, chunk_capacity> query_chunk;
    std::array<std::uint8_t, chunk_capacity> target_chunk;
    sequence_symbol_equality result;
    if (counter == nullptr) {
        result.valid_ranks = false;
        return result;
    }
    for (std::size_t consumed = 0u; consumed < length;) {
        const std::size_t chunk_size = std::min(chunk_capacity, length - consumed);
        query.template copy_ranks<p_query_reversed, false>(
            std::span<std::uint8_t>{query_chunk.data(), chunk_size},
            query_start + consumed, query_start + consumed + chunk_size);
        target.template copy_ranks<false, false>(
            std::span<std::uint8_t>{target_chunk.data(), chunk_size},
            target_start + consumed, target_start + consumed + chunk_size);
        const auto compared = counter(query_chunk.data(), target_chunk.data(),
            chunk_size, p_validation, rank_limit);
        result.equal_symbols += compared.equal_symbols;
        result.vector_batches += compared.vector_batches;
        if (!compared.valid_ranks) {
            result.valid_ranks = false;
            return result;
        }
        consumed += chunk_size;
    }
    return result;
}

} // namespace bioseq::simd

export {

HWY_BEFORE_NAMESPACE();

namespace bioseq::simd::HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

/// @brief Makes the shared compile-time index, loop available in this SIMD namespace.
using ::bioseq::common::templating::for_each_index;

/**
 * @brief Copies the active SIMD lanes into a C++ vector in lane order
 * @param[in] simd_tag Highway descriptor giving the active lne count.
 * @param[in] vector Vector to copy.
 * @tparam p_value_type Scalar lane type.
 */
template <typename p_value_type, typename p_simd_tag>
HWY_INLINE auto extract_lanes(const p_simd_tag& simd_tag, const hn::Vec<p_simd_tag>& vector)
    -> std::vector<p_value_type> {
#if HWY_TARGET == HWY_RVV
    auto storage = hwy::AllocateAligned<p_value_type>(hn::Lanes(simd_tag));
    p_value_type* HWY_RESTRICT storage_buffer = storage.get();
#else
    HWY_ALIGN std::array<p_value_type, hn::MaxLanes(p_simd_tag{})> storage;
    p_value_type* storage_buffer = storage.data();
#endif
    hn::StoreU(vector, simd_tag, storage_buffer);
    return std::vector<p_value_type>(
        storage_buffer,
        storage_buffer + hn::Lanes(simd_tag));
}

/**
 * @brief Copies a SIMD mask into a C++ vector.
 *
 * Each false lane has all bits clear. Each true lane has all bits set, using
 * Highway's vector representation of the mask. Lane order is preserved.
 */
template <typename p_value_type, typename p_simd_tag>
HWY_INLINE auto extract_lanes(const p_simd_tag& simd_tag, const hn::Mask<p_simd_tag>& mask)
    -> std::vector<p_value_type> {
    return extract_lanes<p_value_type>(
        simd_tag,
        hn::VecFromMask(simd_tag, mask));
}

} // namespace bioseq::simd::HWY_NAMESPACE

HWY_AFTER_NAMESPACE();

}
