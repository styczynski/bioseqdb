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
#include <limits>
#include <vector>
export module bioseq.indexing.minimizer_index;
import bioseq.sequences.kmers;     // MinimapSketchData (POD), via the HWY_NAMESPACE using below
import bioseq.storage.size;        // sequence_size_t
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

/// One borrowed, chronologically encoded position run for a minimizer hash.
struct MinimizerPositionSpan {
    std::span<const std::uint64_t> positions{};
};

/**
 * Allocation-free lookup result for either one contiguous positions run or
 * several persisted segment-local runs.  `global_occurrence_count` is
 * authoritative even when high-frequency positions were deliberately not
 * loaded; a consumer may prune on that count before requiring complete spans.
 */
class MinimizerLookupSpans {
private:
    std::span<const std::uint64_t> contiguous_{};
    std::span<const MinimizerPositionSpan> segmented_{};
    std::uint64_t global_occurrence_count_ = 0;
    bool positions_complete_ = true;

    constexpr MinimizerLookupSpans(
            std::span<const std::uint64_t> contiguous,
            std::span<const MinimizerPositionSpan> segmented,
            std::uint64_t global_occurrence_count,
            bool positions_complete) noexcept
        : contiguous_(contiguous),
          segmented_(segmented),
          global_occurrence_count_(global_occurrence_count),
          positions_complete_(positions_complete) {}

public:
    constexpr MinimizerLookupSpans() noexcept = default;

    [[nodiscard]] static constexpr MinimizerLookupSpans contiguous(
            std::span<const std::uint64_t> positions) noexcept {
        return MinimizerLookupSpans{
            positions, {}, positions.size(), /*positions_complete=*/true};
    }

    [[nodiscard]] static constexpr MinimizerLookupSpans segmented(
            std::span<const MinimizerPositionSpan> spans,
            std::uint64_t global_occurrence_count,
            bool positions_complete = true) noexcept {
        return MinimizerLookupSpans{
            {}, spans, global_occurrence_count, positions_complete};
    }

    [[nodiscard]] static constexpr MinimizerLookupSpans positions_unavailable(
            std::uint64_t global_occurrence_count) noexcept {
        return MinimizerLookupSpans{
            {}, {}, global_occurrence_count, /*positions_complete=*/false};
    }

    [[nodiscard]] constexpr std::uint64_t global_occurrence_count()
            const noexcept {
        return global_occurrence_count_;
    }

    [[nodiscard]] constexpr bool positions_complete() const noexcept {
        return positions_complete_;
    }

    [[nodiscard]] constexpr std::span<const std::uint64_t>
    contiguous_positions() const noexcept {
        return contiguous_;
    }

    [[nodiscard]] constexpr std::span<const MinimizerPositionSpan>
    segmented_positions() const noexcept {
        return segmented_;
    }

    /// Validate exclusivity, nonempty segments and the authoritative count.
    [[nodiscard]] constexpr bool well_formed() const noexcept {
        if (!contiguous_.empty() && !segmented_.empty()) {
            return false;
        }
        if (!positions_complete_) {
            return contiguous_.empty() && segmented_.empty();
        }
        if (!contiguous_.empty()) {
            return global_occurrence_count_ == contiguous_.size();
        }
        std::uint64_t observed = 0;
        for (const MinimizerPositionSpan& span : segmented_) {
            if (span.positions.empty()
                || observed > (std::numeric_limits<std::uint64_t>::max)()
                    - span.positions.size()) {
                return false;
            }
            observed += span.positions.size();
        }
        return observed == global_occurrence_count_;
    }
};

template <typename T>
concept ContiguousMinimizerLookupIndex = requires (
    const T& const_index,
    std::uint64_t kmer_hash) {
    { const_index.lookup(kmer_hash) }
        -> std::convertible_to<std::span<const std::uint64_t>>;
};

template <typename T>
concept SegmentedMinimizerLookupIndex = requires (
    const T& const_index,
    std::uint64_t kmer_hash) {
    { const_index.lookup_spans(kmer_hash) }
        -> std::same_as<MinimizerLookupSpans>;
};

/// Any read-only minimizer lookup structure usable by seed/pipeline consumers.
template <typename T>
concept MinimizerLookupIndex = requires (
    const T& const_index) {
    { const_index.target_length() }          -> std::convertible_to<bioseq::storage::sequence_size_t>;
} && (ContiguousMinimizerLookupIndex<T> || SegmentedMinimizerLookupIndex<T>);

/// Zero-copy adapter used by seed consumers for both lookup shapes.
template <MinimizerLookupIndex T>
[[nodiscard]] constexpr MinimizerLookupSpans lookup_minimizer_spans(
        const T& index,
        std::uint64_t kmer_hash) noexcept {
    if constexpr (SegmentedMinimizerLookupIndex<T>) {
        return index.lookup_spans(kmer_hash);
    } else {
        return MinimizerLookupSpans::contiguous(index.lookup(kmer_hash));
    }
}

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
