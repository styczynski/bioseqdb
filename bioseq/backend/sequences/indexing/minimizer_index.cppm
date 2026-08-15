/**
 * @file minimizer_index.cppm
 * @brief Shared minimizer lookup/persistence concepts and runtime segment range.
 *
 * Lives in `bioseq::indexing`. Highway-namespace consumers reference it
 * cross-namespace (the concept names only a POD value type plus a concept, not
 * Highway intrinsics). NO_POSTGRES.
 */
module;
#include <span>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <limits>
#include <stdexcept>
#include <vector>
export module bioseq.indexing.minimizer_index;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.chaining.core;
import bioseq.storage.size;
import bioseq.parallelism.memory_admission;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {
using bioseq::minimizers::minimap_sketch_data;

/**
 * @brief Reports failure to produce a complete authenticated lookup result.
 *
 * PostgreSQL callers catch this named type before crossing the longjmp error
 * boundary.
 */
class persisted_minimizer_lookup_error final : public std::runtime_error {
public:
  /** @brief Classifies failures that require distinct caller handling. */
  enum class reason : std::uint8_t {
      /// @brief Failure without a more specific classification.
      unspecified,
      /// @brief Memory admission or allocation prevented a complete result.
      admission_or_allocation,
  };

private:
    reason reason_ = reason::unspecified;

public:
    using std::runtime_error::runtime_error;

    /**
     * @brief Creates a classified persisted-lookup error.
     * @param reason Failure classification
     * @param message Diagnostic message
     */
    persisted_minimizer_lookup_error(
            reason reason,
            const char* message)
        : std::runtime_error(message), reason_(reason) {}

    /**
     * @brief Returns the failure classification.
     * @return Classification supplied at construction
     */
    [[nodiscard]] reason failure_reason() const noexcept {
        return reason_;
    }
};

/** @brief Borrows one chronologically encoded position run for a hash. */
struct minimizer_position_span {
    /// @brief Chronologically encoded positions owned by the source index.
    std::span<const std::uint64_t> positions{};
};

/**
 * @brief Borrows either one contiguous position run or segmented runs.
 *
 * Segment-local results can contain several persisted runs.
 * `global_occurrence_count` is
 * authoritative even when high-frequency positions were deliberately not
 * loaded. A consumer may prune on that count before requiring complete spans.
 */
class minimizer_lookup_spans {
private:
    const void* contiguous_data_ = nullptr;
    std::size_t contiguous_size_ = 0u;
    std::span<const minimizer_position_span> segmented_{};
    std::uint64_t global_occurrence_count_ = 0;
    std::uint8_t contiguous_width_bytes_ = 0u;
    bool positions_complete_ = true;

    /** @brief Creates a lookup view from its validated shape components. */
    constexpr minimizer_lookup_spans(
            const void* contiguous_data,
            std::size_t contiguous_size,
            std::uint8_t contiguous_width_bytes,
            std::span<const minimizer_position_span> segmented,
            std::uint64_t global_occurrence_count,
            bool positions_complete) noexcept
        : contiguous_data_(contiguous_data),
          contiguous_size_(contiguous_size),
          segmented_(segmented),
          global_occurrence_count_(global_occurrence_count),
          contiguous_width_bytes_(contiguous_width_bytes),
          positions_complete_(positions_complete) {}

public:
  /** @brief Creates an empty, complete lookup result. */
  constexpr minimizer_lookup_spans() noexcept = default;

  /**
   * @brief Borrows one contiguous packed-64 position run.
   * @param positions Chronologically encoded positions
   * @return Complete contiguous lookup result
   */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  contiguous(std::span<const std::uint64_t> positions) noexcept {
      return minimizer_lookup_spans{
          positions.data(), positions.size(), sizeof(std::uint64_t), {}, positions.size(), true};
  }

  /**
   * @brief Borrows one contiguous packed-32 position run.
   * @param positions Chronologically encoded positions
   * @return Complete contiguous lookup result
   */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  contiguous(std::span<const std::uint32_t> positions) noexcept {
      return minimizer_lookup_spans{
          positions.data(), positions.size(), sizeof(std::uint32_t), {}, positions.size(), true};
  }

  /**
   * @brief Borrows segmented packed-64 position runs.
   * @param spans Chronologically ordered segment-local runs
   * @param global_occurrence_count Authoritative total occurrence count
   * @param positions_complete Whether all counted positions are present
   * @return Segmented lookup result
   */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  segmented(std::span<const minimizer_position_span> spans,
            std::uint64_t global_occurrence_count,
            bool positions_complete = true) noexcept {
      return minimizer_lookup_spans{
          nullptr, 0u, 0u, spans, global_occurrence_count, positions_complete};
  }

  /**
   * @brief Creates a count-only result whose positions were not loaded.
   * @param global_occurrence_count Authoritative total occurrence count
   * @return Incomplete count-only lookup result
   */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  positions_unavailable(std::uint64_t global_occurrence_count) noexcept {
      return minimizer_lookup_spans{nullptr, 0u, 0u, {}, global_occurrence_count, false};
  }

  /**
   * @brief Returns the authoritative occurrence count.
   * @return Count across all persisted segments
   */
  [[nodiscard]] constexpr std::uint64_t global_occurrence_count() const noexcept {
      return global_occurrence_count_;
  }

  /**
   * @brief Reports whether every counted position is available.
   * @return `true` for complete lookup results
   */
  [[nodiscard]] constexpr bool positions_complete() const noexcept {
      return positions_complete_;
  }

  /**
   * @brief Returns the packed-64 contiguous run when present.
   * @return Borrowed run or an empty span for another result shape
   */
  [[nodiscard]] std::span<const std::uint64_t> contiguous_positions() const noexcept {
      return contiguous_width_bytes_ == sizeof(std::uint64_t)
                 ? std::span<const std::uint64_t>{static_cast<const std::uint64_t*>(
                                                      contiguous_data_),
                                                  contiguous_size_}
                 : std::span<const std::uint64_t>{};
  }

  /**
   * @brief Returns the packed-32 contiguous run when present.
   * @return Borrowed run or an empty span for another result shape
   */
  [[nodiscard]] std::span<const std::uint32_t> contiguous_positions32() const noexcept {
      return contiguous_width_bytes_ == sizeof(std::uint32_t)
                 ? std::span<const std::uint32_t>{static_cast<const std::uint32_t*>(
                                                      contiguous_data_),
                                                  contiguous_size_}
                 : std::span<const std::uint32_t>{};
  }

  /**
   * @brief Returns the segmented packed-64 runs when present.
   * @return Borrowed segment descriptors
   */
  [[nodiscard]] constexpr std::span<const minimizer_position_span>
  segmented_positions() const noexcept {
      return segmented_;
  }

  /**
   * @brief Validates shape exclusivity, segment contents, and total count.
   * @return `true` when the borrowed result is internally consistent
   */
  [[nodiscard]] constexpr bool well_formed() const noexcept {
      const unsigned active_shapes = static_cast<unsigned>(contiguous_size_ != 0u) +
                                     static_cast<unsigned>(!segmented_.empty());
      if (active_shapes > 1u ||
          (contiguous_size_ != 0u &&
           (contiguous_data_ == nullptr || (contiguous_width_bytes_ != sizeof(std::uint32_t) &&
                                            contiguous_width_bytes_ != sizeof(std::uint64_t))))) {
          return false;
      }
      if (!positions_complete_) {
          return active_shapes == 0u;
      }
      if (contiguous_size_ != 0u) {
          return global_occurrence_count_ == contiguous_size_;
      }
      std::uint64_t observed = 0;
      for (const minimizer_position_span& span : segmented_) {
          if (span.positions.empty() ||
              observed > (std::numeric_limits<std::uint64_t>::max)() - span.positions.size()) {
              return false;
          }
          observed += span.positions.size();
      }
      return observed == global_occurrence_count_;
  }
};

/**
 * @brief Checks for a read-only contiguous minimizer lookup interface.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept contiguous_minimizer_lookup_index =
    requires(const p_index& const_index, std::uint64_t kmer_hash) {
        { const_index.lookup(kmer_hash) } -> std::same_as<std::span<const std::uint64_t>>;
    };

/**
 * @brief Checks for a read-only segmented minimizer lookup interface.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept segmented_minimizer_lookup_index =
    requires(const p_index& const_index, std::uint64_t kmer_hash) {
        { const_index.lookup_spans(kmer_hash) } -> std::same_as<minimizer_lookup_spans>;
    };

/**
 * @brief Checks for any read-only minimizer lookup shape used by seeding.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept minimizer_lookup_index = requires(const p_index& const_index) {
    { const_index.target_length() } -> std::convertible_to<bioseq::storage::sequence_size_t>;
} && (contiguous_minimizer_lookup_index<p_index> || segmented_minimizer_lookup_index<p_index>);

/**
 * @brief Checks for a reader that prepares one query wave into final anchors.
 * @tparam p_index Candidate prepared reader type
 */
template <typename p_index>
concept prepared_fragment_anchor_batch_reader =
    requires(const p_index& const_reader,
             bioseq::parallelism::memory_admission_controller& admission,
             std::span<const typename p_index::fragment_seed_query_type> queries,
             typename p_index::query_wave_scratch_type& scratch,
             std::int32_t max_occ) {
        typename p_index::fragment_seed_query_type;
        typename p_index::query_wave_scratch_type;
        typename p_index::prepared_fragment_anchor_batch_type;
        {
            const_reader.prepare_fragment_anchor_batch(admission, queries, max_occ, scratch)
        } -> std::same_as<typename p_index::prepared_fragment_anchor_batch_type>;
    };

/**
 * @brief Checks for a per-query view over prepared canonical anchors.
 * @tparam p_index Candidate query-view type
 */
template <typename p_index>
concept prepared_fragment_anchor_query_index =
    requires(const p_index& const_index,
             std::span<const minimap_sketch_data> query_minimizers,
             bioseq::storage::sequence_size_t query_length,
             std::int32_t max_occ,
             std::int32_t kmer_size) {
        { const_index.target_length() } -> std::convertible_to<bioseq::storage::sequence_size_t>;
        {
            const_index.ordered_fragment_seed_anchors(
                query_minimizers, query_length, max_occ, kmer_size)
        } -> std::same_as<std::span<const bioseq::chaining::anchor>>;
    };

/**
 * @brief Checks for either target-index shape accepted by the seed stage.
 * @tparam p_index Candidate target index type
 */
template <typename p_index>
concept minimizer_seed_index =
    minimizer_lookup_index<p_index> || prepared_fragment_anchor_query_index<p_index>;

/**
 * @brief Adapts either ordinary lookup interface to borrowed lookup spans.
 * @tparam p_index Read-only minimizer lookup type
 * @param index Index to query
 * @param kmer_hash Hash to look up
 * @return Borrowed positions and authoritative count
 */
template <minimizer_lookup_index p_index>
[[nodiscard]] inline minimizer_lookup_spans lookup_minimizer_spans(
        const p_index& index,
        std::uint64_t kmer_hash) {
    if constexpr (segmented_minimizer_lookup_index<p_index>) {
        return index.lookup_spans(kmer_hash);
    } else {
        return minimizer_lookup_spans::contiguous(index.lookup(kmer_hash));
    }
}

/**
 * @brief Checks for a lookup index with direct binary persistence support.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept persistable_minimizer_index =
    minimizer_lookup_index<p_index> && requires(p_index& mutable_index,
                                                const p_index& const_index,
                                                const std::vector<minimap_sketch_data>& sketches,
                                                bioseq::storage::sequence_size_t target_length_arg,
                                                char* output_buffer,
                                                const char* input_buffer,
                                                std::size_t input_size) {
        { mutable_index.build(sketches, target_length_arg) } -> std::same_as<void>;
        { const_index.serialize_binary_size() } -> std::convertible_to<std::size_t>;
        { const_index.serialize_to_binary(output_buffer) } -> std::same_as<void>;
        { p_index::deserialize_from_binary(input_buffer, input_size) } -> std::same_as<p_index>;
    };

/** @brief Carries collection and core bounds shared by Highway targets. */
struct segment_unit_range {
    /// @brief First collection storage unit.
    bioseq::storage::sequence_size_t cs_u;
    /// @brief One-past-last collection storage unit.
    bioseq::storage::sequence_size_t ce_u;
    /// @brief First base in the emission core.
    bioseq::storage::sequence_size_t core_lo_bp;
    /// @brief One-past-last base in the emission core.
    bioseq::storage::sequence_size_t core_hi_bp;
};

} // namespace bioseq::indexing
