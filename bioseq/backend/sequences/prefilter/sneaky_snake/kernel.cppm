/**
 * @file kernel.cppm
 * @brief Canonical generator input for fixed-target SneakySnake SIMD kernels.
 *
 * This module is scanned by bioseq_codegen and is not a final production
 * provider. The generator replaces the annotated `target` namespace with one
 * namespace per compiled Highway target and adds HWY_ATTR to every callable.
 * Each generated provider preserves the scalar maze policy while using the
 * active target's byte-lane width for diagonal comparisons and candidate masks.
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

export namespace bioseq::prefilter {
/** @brief Fixed-target SneakySnake batch entry points duplicated by codegen per Highway backend. */
namespace [[bioseq::simd]] target {

/** Highway namespace alias for the generated target's active vector operations. */
namespace hn = hwy::HWY_NAMESPACE;

/** @brief Target-specific traversal hooks consumed by the shared scalar maze evaluator. */
namespace sneaky_snake_target_detail {

/**
 * @brief Supplies fixed-target diagonal and candidate traversal to the shared maze evaluator.
 * @tparam p_descriptor Highway descriptor whose active byte-lane width drives traversal.
 *
 * The policy is stateless and borrows every input span only for the duration of a call. It uses
 * `hn::Lanes` for active loads and constrains each candidate mask to the 64 bits returned by
 * `hn::BitsFromMask`.
 */
template <class p_descriptor> struct highway_traversal final {
  /**
   * @brief Counts the equal-rank run beginning at one target/query diagonal coordinate.
   * @param[in] query Query ranks in logical order.
   * @param[in] target_ranks Target ranks in logical order.
   * @param[in] position Target position at which the diagonal begins.
   * @param[in] submaze_end Exclusive target bound for this traversal.
   * @param[in] query_index Query position aligned with `position`.
   * @return The number of consecutive equal ranks before either bound or the first mismatch.
   * @pre `position <= submaze_end <= target_ranks.size()` and `query_index <= query.size()`.
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
   * @brief Visits matching candidate starts across one inclusive query interval.
   * @tparam p_consumer Callable receiving each matching candidate start.
   * @param[in] query Query ranks searched for candidate starts.
   * @param[in] filter_offset Offset from each candidate start to the filter rank.
   * @param[in] filter_rank Rank required at the offset position.
   * @param[in] position Candidate start excluded from visitation.
   * @param[in] lower Inclusive first candidate start.
   * @param[in] upper Inclusive last candidate start.
   * @param[in] consume Consumer that returns `true` to stop the search.
   * @return `true` when a matching candidate makes `consume` stop the search.
   * The result is `false` otherwise.
   * @pre Every candidate start through `upper`, plus `filter_offset`, is a valid query position.
   */
  template <class p_consumer>
  [[nodiscard]] bool visit_candidate_starts(
      std::span<const std::uint8_t> query, std::uint32_t filter_offset,
      std::uint8_t filter_rank, std::uint32_t position, std::uint32_t lower,
      std::uint32_t upper, p_consumer &&consume) const noexcept {
    /** @brief Byte-lane descriptor derived from the fixed-target shape. */
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
 * @brief Evaluates a batch with an explicitly selected fixed-target descriptor.
 * @tparam p_descriptor Highway descriptor used for byte-lane traversal.
 * @param[in] descriptor Descriptor value whose type determines the fixed-target lane shape.
 * @param[in] input Flat rank buffers and ordered jobs.
 * @param[in] options Submaze width and escape-segment limits.
 * @return One SneakySnake result per job in input order.
 */
template <class p_descriptor>
[[nodiscard]] inline std::vector<prefilter_result>
sneaky_snake_simd_batch_for(p_descriptor descriptor,
                            const prefilter_batch_input &input,
                            sneaky_snake_options options = {}) {
  static_cast<void>(descriptor);
  sneaky_snake_target_detail::highway_traversal<p_descriptor> traversal;
  // Per-job adapter that runs bounded maze evaluation through the fixed-target traversal.
  return sneaky_snake_detail::evaluate_batch_with(
      input, [&traversal, options](std::span<const std::uint8_t> query,
                                   std::span<const std::uint8_t> target_ranks,
                                   const prefilter_job &job) noexcept {
        return sneaky_snake_detail::bounded_with(traversal, query, target_ranks,
                                                 job.max_edits, options);
      });
}

/**
 * @brief Evaluates a batch through the generated target's scalable byte descriptor.
 * @param[in] input Flat rank buffers and ordered jobs.
 * @param[in] options Submaze width and escape-segment limits.
 * @return One SneakySnake result per job in input order.
 *
 * Each generated binder installs this exact callable in `highway_common_kernels`.
 * Runtime target selection remains outside this source module.
 */
[[nodiscard]] inline std::vector<prefilter_result>
sneaky_snake_simd_batch(const prefilter_batch_input &input,
                        sneaky_snake_options options = {}) {
  const hn::ScalableTag<std::uint8_t> descriptor;
  return sneaky_snake_simd_batch_for(descriptor, input, options);
}

} // namespace target
} // namespace bioseq::prefilter
