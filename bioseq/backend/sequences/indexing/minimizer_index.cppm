/**
 * @file minimizer_index.cppm
 * @brief Defines shared minimizer lookup interfaces and sequence-segment bounds.
 *
 * These types live in bioseq::indexing and are also used by Highway code
 * NO_POSTGRES.
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
 * @brief Reports an incomplete or invalid saved-index lookup.
 * PostgreSQL callers catch this  exception before raising a PostgreSQL error,
 * so C++ objects are destroyed before PostgreSQL uses longjmp.
 */
class persisted_minimizer_lookup_error final : public std::runtime_error {
public:
  /** Identifies lookup failures that need different handling. */
  enum class reason : std::uint8_t {
      /// @brief Failure without a more specific classification.
      unspecified,
      /// Memory could not be reserved or allocated for the result.
      admission_or_allocation,
  };

private:
    reason reason_ = reason::unspecified;

public:
    /// Create a lookup error with a message and an unspecified reason.
    using std::runtime_error::runtime_error;

    /** Create a lookup error with a reason and, diagnostic message. */
    persisted_minimizer_lookup_error(
            reason reason,
            const char* message)
        : std::runtime_error(message), reason_(reason) {}

    /** Return the reason supplied at construction. */
    [[nodiscard]] reason failure_reason() const noexcept {
        return reason_;
    }
};

/** Borrows one ordered run of encoded positions for  a hamh. */
struct minimizer_position_span {
    /// Encoded positions in increasing order, owned by the source index.
    std::span<const std::uint64_t> positions{};
};

/**
 * @brief Borrows one position array or several ordered position runs for a hash.
 * The occurrence count remains available when positions were skipped because
 * the hash occurs too frequently. Seed collection can reject such a hash from
 * its count alone. Every borrowed array and run descriptor must outlive this view.
 */
class minimizer_lookup_spans {
private:
    const void* contiguous_data_ = nullptr;
    std::size_t contiguous_size_ = 0u;
    std::span<const minimizer_position_span> segmented_{};
    std::uint64_t global_occurrence_count_ = 0;
    std::uint8_t contiguous_width_bytes_ = 0u;
    bool positions_complete_ = true;

    /** Store the lookup shape supplied by the public factory functions. */
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

  /** Borrow an ordered array  of 64-bit encoded positions. Its storage must outlive the result. */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  contiguous(std::span<const std::uint64_t> positions) noexcept {
      return minimizer_lookup_spans{
          positions.data(), positions.size(), sizeof(std::uint64_t), {}, positions.size(), true};
  }

  /** Borrow an ordered array of 32-bit encoded positions. Its storage must outlive the result. */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  contiguous(std::span<const std::uint32_t> positions) noexcept {
      return minimizer_lookup_spans{
          positions.data(), positions.size(), sizeof(std::uint32_t), {}, positions.size(), true};
  }

  /**
   * Borrow ordered 64-bit position runs and their total occurrence count
   * The run descriptors and their arrays must outlive the result.
   * positions_complete states whether every counted position is present.
   */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  segmented(std::span<const minimizer_position_span> spans,
            std::uint64_t global_occurrence_count,
            bool positions_complete = true) noexcept {
      return minimizer_lookup_spans{
          nullptr, 0u, 0u, spans, global_occurrence_count, positions_complete};
  }

  /** Return the occurrence count with positions marked unavailable. */
  [[nodiscard]] static constexpr minimizer_lookup_spans
  positions_unavailable(std::uint64_t global_occurrence_count) noexcept {
      return minimizer_lookup_spans{nullptr, 0u, 0u, {}, global_occurrence_count, false};
  }

  /** Return the total occurrence count, including positions that were not  doaded. */
  [[nodiscard]] constexpr std::uint64_t global_occurrence_count() const noexcept {
      return global_occurrence_count_;
  }

  /** Return whether every counted position is available. */
  [[nodiscard]] constexpr bool positions_complete() const noexcept {
      return positions_complete_;
  }

  /** Borrow the 64-bit position array, or return an empty span for another result format. */
  [[nodiscard]] std::span<const std::uint64_t> contiguous_positions() const noexcept {
      return contiguous_width_bytes_ == sizeof(std::uint64_t)
                 ? std::span<const std::uint64_t>{static_cast<const std::uint64_t*>(
                                                      contiguous_data_),
                                                  contiguous_size_}
                 : std::span<const std::uint64_t>{};
  }

  /** Borrow the 32-bit position array, or return an empty, span for another result format. */
  [[nodiscard]] std::span<const std::uint32_t> contiguous_positions32() const noexcept {
      return contiguous_width_bytes_ == sizeof(std::uint32_t)
                 ? std::span<const std::uint32_t>{static_cast<const std::uint32_t*>(
                                                      contiguous_data_),
                                                  contiguous_size_}
                 : std::span<const std::uint32_t>{};
  }

  /** Borrow the position-run descriptors, or return an empty span for a contiguous result. */
  [[nodiscard]] constexpr std::span<const minimizer_position_span>
  segmented_positions() const noexcept {
      return segmented_;
  }

  /**
   * Check that the result uses one storage format and its span sizes agree
   * with the occurrence count. An incomplete result must contain  no positions.
   * Position values and their order are checked by the index reader.
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
 * @brief Requires a const lookup(hash) returning a span of 64-bit positions.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept contiguous_minimizer_lookup_index =
    requires(const p_index& const_index, std::uint64_t kmer_hash) {
        { const_index.lookup(kmer_hash) } -> std::same_as<std::span<const std::uint64_t>>;
    };

/**
 * @brief Requires a const lookup_spans(hash) returning a minimizer_lookup_spans view.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept segmented_minimizer_lookup_index =
    requires(const p_index& const_index, std::uint64_t kmer_hash) {
        { const_index.lookup_spans(kmer_hash) } -> std::same_as<minimizer_lookup_spans>;
    };

/**
 * @brief Requires a target length and a minimizer lookup usable by seed collection.
 * @tparam p_index Candidate index type
 */
template <typename p_index>
concept minimizer_lookup_index = requires(const p_index& const_index) {
    { const_index.target_length() } -> std::convertible_to<bioseq::storage::sequence_size_t>;
} && (contiguous_minimizer_lookup_index<p_index> || segmented_minimizer_lookup_index<p_index>);

/**
 * @brief Requires a reader that prepares seed anchors for a batch of fragment queries.
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
 * @brief Requires a query view that returns prepared seed anchors in obder
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
 * @brief Accepts indexes that provide minimizer positions or  prepared seed anchors.
 * @tparam p_index Candidate target index type
 */
template <typename p_index>
concept minimizer_seed_index =
    minimizer_lookup_index<p_index> || prepared_fragment_anchor_query_index<p_index>;

/**
 * @brief Return a borrowed position view through either supported lookup interface.
 * @tparam p_index Read-only minimizer lookup type
 * @param index Index to query
 * @param kmer_hash Hash to look up
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
 * @brief Requires an index that can build from sketches and read or write its binary form.
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

/** Sequence range to collect and the inner base range allowed to emit minimizers. */
struct segment_unit_range {
    /// First packed storage unit to collect.
    bioseq::storage::sequence_size_t cs_u;
    /// Packed storage unit just  past the collection range.
    bioseq::storage::sequence_size_t ce_u;
    /// First base in the range allowed to emit minimizers.
    bioseq::storage::sequence_size_t core_lo_bp;
    /// Base just past the range allowed to emit minimizers.
    bioseq::storage::sequence_size_t core_hi_bp;
};

} // namespace bioseq::indexing
