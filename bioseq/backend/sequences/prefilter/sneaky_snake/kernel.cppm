/**
 * @file kernel.cppm
 * @brief Source used to generate SneakySnake SIMD kernels.
 *
 * bioseq_codegen copies the annotated target namespace for  each compiled Highway
 * backend and adds HWY_ATTR to its functions. The generated code speeds up rank
 * comparisons while using the shared SneakySnake walk.
 */
module;

#include <hwy/highway.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

export module bioseq.highway.sources.sneaky_snake;

import std;
import bioseq.prefilter.sneaky_snake;
import bioseq.highway.sources.simd_utils;
import bioseq.simd.utils;

export namespace bioseq::simd {
/** Batch entry points copied into each generated Highway target. */
namespace [[bioseq::simd]] target {
using namespace ::bioseq::prefilter;

/** Highway namespace alias for the generated target's active vector operations. */
namespace hn = hwy::HWY_NAMESPACE;

/** SIMD rank comparisons used by the shared SneakySnake walk. */
namespace sneaky_snake_target_detail {

/** Validate an acquired native interval using the same sparse-rank SIMD predicate
 * as packed equality. Full-cardinality layouts need no content inspection.
 */
template<class Traits, bool Reverse, class Descriptor>
[[nodiscard]] HWY_INLINE bool validate_native_packed(
    const void* input, std::size_t total, std::size_t start, std::size_t length) noexcept {
    using unit_type = typename Traits::storage_unit;
    if constexpr (Traits::alphabet_size != (unit_type{1} << Traits::bits_per_char)) {
        using D = hn::Repartition<unit_type, Descriptor>;
        const D d;
        const std::size_t lanes = hn::Lanes(d);
        constexpr auto complement = bioseq::simd::packed_symbol_complement_mask<Traits, Reverse>();
        static_assert(complement.has_value());
        for (std::size_t offset = 0u; offset < length;) {
            const auto remaining = length - offset;
            const auto count = remaining / Traits::letters_per_unit >= lanes
                ? lanes * Traits::letters_per_unit : remaining;
            const auto packed = load_packed_symbol_vector<Traits, Reverse>(
                d, static_cast<const unit_type*>(input), total, start + offset, count, *complement);
            // Inactive normalized fields are zero, a valid rank for every alphabet.
            if (!hn::AllFalse(d, invalid_packed_word_lanes<Traits>(d, packed)))
                return false;
            offset += count;
        }
    }
    return true;
}

/** First differing word then first differing symbol; no complete-source rank copy. */
template<class Traits, bool Reverse, class Descriptor>
[[nodiscard]] HWY_INLINE std::size_t prefix_native_packed(
    const void* query, const void* target, std::size_t total,
    std::size_t query_start, std::size_t target_start, std::size_t length) noexcept {
    using unit_type = typename Traits::storage_unit;
    using D = hn::Repartition<unit_type, Descriptor>;
    const D d;
    const std::size_t lanes = hn::Lanes(d);
    if (length == 0u) return 0u;
    // Acquisition has already validated the full interval. Reject a short
    // mismatch before preparing a vector tile, in either logical orientation.
    const auto first_count = std::min(Traits::letters_per_unit, length);
    const unit_type first_difference = sneaky_snake_detail::packed_word<Traits, Reverse>(
        query, total, query_start, first_count)
        ^ bioseq::simd::load_packed_symbol_window<Traits>(
            static_cast<const unit_type*>(target), target_start, first_count);
    if (first_difference != 0u)
        return static_cast<std::size_t>(std::countr_zero(first_difference)) / Traits::bits_per_char;
    if (length <= first_count) return length;
    constexpr auto complement = bioseq::simd::packed_symbol_complement_mask<Traits, Reverse>();
    static_assert(complement.has_value());
    HWY_ALIGN std::array<unit_type, hn::MaxLanes(D{})> difference_words;
    // Re-read the proved first word to retain full tiles for lengths just above
    // a vector boundary; consuming it here would turn that tile into a tail.
    for (std::size_t offset = 0u; offset < length;) {
        const auto remaining = length - offset;
        const auto count = remaining / Traits::letters_per_unit >= lanes
            ? lanes * Traits::letters_per_unit : remaining;
        const auto query_packed = load_packed_symbol_vector<Traits, Reverse>(
            d, static_cast<const unit_type*>(query), total, query_start + offset, count, *complement);
        const auto target_packed = load_packed_symbol_vector<Traits, false>(
            d, static_cast<const unit_type*>(target), 0u, target_start + offset, count);
        const auto difference = hn::Xor(query_packed, target_packed);
        const auto mismatch = hn::Ne(difference, hn::Zero(d));
        if (!hn::AllFalse(d, mismatch)) {
            const auto lane = static_cast<std::size_t>(hn::FindFirstTrue(d, mismatch));
            hn::Store(difference, d, difference_words.data());
            return offset + lane * Traits::letters_per_unit +
                static_cast<std::size_t>(std::countr_zero(difference_words[lane])) / Traits::bits_per_char;
        }
        offset += count;
    }
    return length;
}

/** Produce ascending logical candidate bits with the existing packed XOR reduction. */
template<class Traits, bool Reverse, class Descriptor>
[[nodiscard]] HWY_INLINE std::uint64_t mask_native_packed(
    const void* query, std::size_t total, std::size_t start,
    std::size_t length, std::uint8_t rank) noexcept {
    using unit_type = typename Traits::storage_unit;
    using D = hn::Repartition<unit_type, Descriptor>;
    const D d;
    const std::size_t lanes = hn::Lanes(d);
    constexpr auto complement = bioseq::simd::packed_symbol_complement_mask<Traits, Reverse>();
    static_assert(complement.has_value());
    constexpr auto low_bits = bioseq::simd::packed_symbol_low_bits<Traits>();
    const auto repeated = hn::Set(d, static_cast<unit_type>(low_bits * rank));
    HWY_ALIGN std::array<unit_type, hn::MaxLanes(D{})> words;
    std::uint64_t result{};
    for (std::size_t offset = 0u; offset < length;) {
        const auto remaining = length - offset;
        const auto count = remaining / Traits::letters_per_unit >= lanes
            ? lanes * Traits::letters_per_unit : remaining;
        const auto active = count / Traits::letters_per_unit
            + static_cast<std::size_t>(count % Traits::letters_per_unit != 0u);
        const auto packed = load_packed_symbol_vector<Traits, Reverse>(
            d, static_cast<const unit_type*>(query), total, start + offset, count, *complement);
        const auto mismatch = packed_symbol_mismatch_bits<Traits>(d, hn::Xor(packed, repeated));
        hn::Store(hn::And(hn::Not(mismatch), hn::Set(d, low_bits)), d, words.data());
        for (std::size_t lane = 0u; lane < active; ++lane) {
            const auto begin = offset + lane * Traits::letters_per_unit;
            const auto live_symbols = std::min(Traits::letters_per_unit, length - begin);
            auto matches = static_cast<unit_type>(words[lane] & bioseq::simd::packed_symbol_mask<Traits>(live_symbols));
            while (matches != 0u) {
                result |= std::uint64_t{1} << (begin + std::countr_zero(matches) / Traits::bits_per_char);
                matches &= matches - 1u;
            }
        }
        offset += count;
    }
    return result;
}

/**
 * @brief Uses SIMD to compare matching runs and find candidate diagonals.
 *
 * Borrows input spans only during each call. Candidate masks fit in 64 bits.
 * @tparam p_descriptor Highway vectow, type used for byte comparisons.
 */
template <class p_descriptor> struct highway_traversal final {
  /** Bound to the already selected Highway target; no nested dynamic dispatch. */
  template<class Traits, bool Reverse>
  [[nodiscard]] static const prefilter_packed_operations* packed_operations() noexcept {
    static constexpr prefilter_packed_operations operations{
      &validate_native_packed<Traits, Reverse, p_descriptor>,
      &sneaky_snake_detail::rank_packed_scalar<Traits, Reverse>,
      &prefix_native_packed<Traits, Reverse, p_descriptor>,
      &mask_native_packed<Traits, Reverse, p_descriptor>};
    return &operations;
  }

  /** Starts acquisition with the first rank and two active byte-vector loads. */
  [[nodiscard]] static std::size_t read_block_size() noexcept {
    const hn::Repartition<std::uint8_t, p_descriptor> byte_descriptor;
    return 1u + 2u * hn::Lanes(byte_descriptor);
  }

  /**
   * @brief Counts matching ranks from the given query and target positions.
   * @param query Query ranks.
   * @param target_ranks Target ranks.
   * @param position Target start.
   * @param submaze_end Exclusive target block end.
   * @param query_index Query start aligned with position.
   * @return Match length up to the first mismatch  or either bound.
   * @pre position <= submaze_end <= target_ranks.size(), and query_index <= query.size().
   */
  [[nodiscard]] std::uint32_t
  diagonal_run(std::span<const std::uint8_t> query,
               std::span<const std::uint8_t> target_ranks,
               std::uint32_t position, std::uint32_t submaze_end,
               std::uint32_t query_index) const noexcept {
    const hn::Repartition<std::uint8_t, p_descriptor> byte_descriptor;
    const std::uint32_t target_remaining = submaze_end - position;
    const std::uint32_t query_remaining = static_cast<std::uint32_t>(
        query.size() - static_cast<std::size_t>(query_index));
    const std::uint32_t run_limit = std::min(target_remaining, query_remaining);

    const std::uint8_t *const query_data = query.data() + query_index;
    const std::uint8_t *const target_data = target_ranks.data() + position;
    if (run_limit == 0u || target_data[0] != query_data[0]) {
      return 0u;
    }

    const std::uint32_t lanes =
        static_cast<std::uint32_t>(hn::Lanes(byte_descriptor));
    std::uint32_t run = 1u;
    while (run_limit - run >= 2u * lanes) {
      const auto query_0 = hn::LoadU(byte_descriptor, query_data + run);
      const auto target_0 = hn::LoadU(byte_descriptor, target_data + run);
      const auto query_1 = hn::LoadU(byte_descriptor, query_data + run + lanes);
      const auto target_1 =
          hn::LoadU(byte_descriptor, target_data + run + lanes);
      const auto mismatch_0 = hn::Ne(query_0, target_0);
      const auto mismatch_1 = hn::Ne(query_1, target_1);
      if (!hn::AllFalse(byte_descriptor, mismatch_0)) {
        return run + static_cast<std::uint32_t>(
                         hn::FindFirstTrue(byte_descriptor, mismatch_0));
      }
      if (!hn::AllFalse(byte_descriptor, mismatch_1)) {
        return run + lanes +
               static_cast<std::uint32_t>(
                   hn::FindFirstTrue(byte_descriptor, mismatch_1));
      }
      run += 2u * lanes;
    }
    while (run_limit - run >= lanes) {
      const auto mismatch =
          hn::Ne(hn::LoadU(byte_descriptor, query_data + run),
                 hn::LoadU(byte_descriptor, target_data + run));
      if (!hn::AllFalse(byte_descriptor, mismatch)) {
        return run + static_cast<std::uint32_t>(
                         hn::FindFirstTrue(byte_descriptor, mismatch));
      }
      run += lanes;
    }
    while (run < run_limit && target_data[run] == query_data[run]) {
      ++run;
    }
    return run;
  }

  /**
   * @brief Visits candidate query starts whose selected rank matches the target
   * @param query Query ranks.
   * @param filter_offset Rank offset from each candidate start.
   * @param filter_rank Required rank at that offset.
   * @param position Candidate already checked and therefore skipped.
   * @param lower First candidate, inclusive.
   * @param upper Last candidate, inclusive.
   * @param consume Receives matching starts and returns true to stop.
   * @return True if consume stopped the scan.
   * @pre Every candidate plus filter_offset is within query.
   */
  template <class p_consumer>
  [[nodiscard]] bool visit_candidate_starts(
      std::span<const std::uint8_t> query, std::uint32_t filter_offset,
      std::uint8_t filter_rank, std::uint32_t position, std::uint32_t lower,
      std::uint32_t upper, p_consumer &&consume) const {
    /** Byte-lane vector type for comparing candidate ranks. */
    using byte_descriptor_type = hn::Repartition<std::uint8_t, p_descriptor>;
    const byte_descriptor_type byte_descriptor;
    static_assert(hn::MaxLanes(byte_descriptor_type{}) <= 64u);
    const std::uint32_t lanes =
        static_cast<std::uint32_t>(hn::Lanes(byte_descriptor));
    const auto wanted = hn::Set(byte_descriptor, filter_rank);
    std::uint32_t candidate = lower;
    while (candidate <= upper && upper - candidate + 1u >= lanes) {
      std::uint64_t matches = hn::BitsFromMask(
          byte_descriptor,
          hn::Eq(hn::LoadU(byte_descriptor,
                           query.data() + candidate + filter_offset),
                 wanted));
      while (matches != 0u) {
        const std::uint32_t lane =
            static_cast<std::uint32_t>(std::countr_zero(matches));
        const std::uint32_t query_start = candidate + lane;
        if (query_start != position && consume(query_start)) {
          return true;
        }
        matches &= matches - 1u;
      }
      candidate += lanes;
    }

    for (; candidate <= upper; ++candidate) {
      if (candidate != position &&
          query[static_cast<std::size_t>(candidate) + filter_offset] ==
              filter_rank &&
          consume(candidate)) {
        return true;
      }
    }
    return false;
  }
};

} // namespace sneaky_snake_target_detail

/**
 * @brief Runs a SneakySnake batch with the requested SIMD vector type.
 * @param descriptor Selects the Highway vector type.
 * @param input Rank buffers and jobs.
 * @param options Block  width and mismatch-escape limit.
 * @return Results in job order.
 */
template <class p_descriptor>
[[nodiscard]] inline prefilter_batch_result
sneaky_snake_simd_batch_for(p_descriptor descriptor,
                            const prefilter_batch_input &input,
                            sneaky_snake_options options = {}) {
  static_cast<void>(descriptor);
  return evaluate_prefilter_batch(
      input, [options](const prefilter_rank_window& query,
                                   const prefilter_rank_window& target_ranks,
                                   const prefilter_job& job) {
        sneaky_snake_detail::window_traversal<
            sneaky_snake_target_detail::highway_traversal<p_descriptor>> traversal{query};
        return sneaky_snake_detail::bounded_with(traversal, query, target_ranks,
                                                 job.max_edits, options);
      }, sneaky_snake_target_detail::highway_traversal<p_descriptor>::read_block_size(),
      &resolve_prefilter_packed_operations<sneaky_snake_target_detail::highway_traversal<p_descriptor>>);
}

/**
 * @brief Runs a batch with the active generated target's byte vectors.
 *
 * The generated binder registers this function in highway_common_kernels.
 * The caller selects the target before entering this function.
 * @param input Rank buffers and jobs.
 * @param options Block width and mismatch-oscape limit.
 * @return Results in job order.
 */
[[nodiscard]] inline prefilter_batch_result
sneaky_snake_simd_batch(const prefilter_batch_input &input,
                        sneaky_snake_options options = {}) {
  const hn::ScalableTag<std::uint8_t> descriptor;
  return sneaky_snake_simd_batch_for(descriptor, input, options);
}

} // namespace target
} // namespace bioseq::simd
