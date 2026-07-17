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
#include <stdexcept>
#include <vector>
export module bioseq.indexing.minimizer_index;
import bioseq.sequences.kmers;     // MinimapSketchData (POD), via the HWY_NAMESPACE using below
import bioseq.sequences.chaining.utils; // prepared fragment-anchor query view
import bioseq.storage.size;        // sequence_size_t
import bioseq.parallelism.memory_admission;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {
using bioseq::simd::HWY_NAMESPACE::MinimapSketchData;

/// Fail-closed error raised when an authenticated persisted lookup cannot
/// provide a complete, internally consistent position result.  PostgreSQL
/// callers catch this named type before crossing the longjmp ERROR boundary.
class PersistedMinimizerLookupError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
    const void* contiguous_data_ = nullptr;
    std::size_t contiguous_size_ = 0u;
    std::span<const MinimizerPositionSpan> segmented_{};
    std::uint64_t global_occurrence_count_ = 0;
    std::uint8_t contiguous_width_bytes_ = 0u;
    bool positions_complete_ = true;

    constexpr MinimizerLookupSpans(
            const void* contiguous_data,
            std::size_t contiguous_size,
            std::uint8_t contiguous_width_bytes,
            std::span<const MinimizerPositionSpan> segmented,
            std::uint64_t global_occurrence_count,
            bool positions_complete) noexcept
        : contiguous_data_(contiguous_data),
          contiguous_size_(contiguous_size),
          segmented_(segmented),
          global_occurrence_count_(global_occurrence_count),
          contiguous_width_bytes_(contiguous_width_bytes),
          positions_complete_(positions_complete) {}

public:
    constexpr MinimizerLookupSpans() noexcept = default;

    [[nodiscard]] static constexpr MinimizerLookupSpans contiguous(
            std::span<const std::uint64_t> positions) noexcept {
        return MinimizerLookupSpans{
            positions.data(), positions.size(), sizeof(std::uint64_t), {},
            positions.size(), /*positions_complete=*/true};
    }

    /// Borrow one directly consumable packed-32 position run.
    [[nodiscard]] static constexpr MinimizerLookupSpans contiguous(
            std::span<const std::uint32_t> positions) noexcept {
        return MinimizerLookupSpans{
            positions.data(), positions.size(), sizeof(std::uint32_t), {},
            positions.size(), /*positions_complete=*/true};
    }

    [[nodiscard]] static constexpr MinimizerLookupSpans segmented(
            std::span<const MinimizerPositionSpan> spans,
            std::uint64_t global_occurrence_count,
            bool positions_complete = true) noexcept {
        return MinimizerLookupSpans{
            nullptr, 0u, 0u, spans, global_occurrence_count,
            positions_complete};
    }

    [[nodiscard]] static constexpr MinimizerLookupSpans positions_unavailable(
            std::uint64_t global_occurrence_count) noexcept {
        return MinimizerLookupSpans{
            nullptr, 0u, 0u, {}, global_occurrence_count,
            /*positions_complete=*/false};
    }

    [[nodiscard]] constexpr std::uint64_t global_occurrence_count()
            const noexcept {
        return global_occurrence_count_;
    }

    [[nodiscard]] constexpr bool positions_complete() const noexcept {
        return positions_complete_;
    }

    [[nodiscard]] std::span<const std::uint64_t>
    contiguous_positions() const noexcept {
        return contiguous_width_bytes_ == sizeof(std::uint64_t)
            ? std::span<const std::uint64_t>{
                static_cast<const std::uint64_t*>(contiguous_data_),
                contiguous_size_}
            : std::span<const std::uint64_t>{};
    }

    /// Packed-32 counterpart of `contiguous_positions()`. At most one
    /// contiguous width is present in a well-formed result.
    [[nodiscard]] std::span<const std::uint32_t>
    contiguous_positions32() const noexcept {
        return contiguous_width_bytes_ == sizeof(std::uint32_t)
            ? std::span<const std::uint32_t>{
                static_cast<const std::uint32_t*>(contiguous_data_),
                contiguous_size_}
            : std::span<const std::uint32_t>{};
    }

    [[nodiscard]] constexpr std::span<const MinimizerPositionSpan>
    segmented_positions() const noexcept {
        return segmented_;
    }

    /// Validate exclusivity, nonempty segments and the authoritative count.
    [[nodiscard]] constexpr bool well_formed() const noexcept {
        const unsigned active_shapes =
            static_cast<unsigned>(contiguous_size_ != 0u)
            + static_cast<unsigned>(!segmented_.empty());
        if (active_shapes > 1u
            || (contiguous_size_ != 0u
                && (contiguous_data_ == nullptr
                    || (contiguous_width_bytes_ != sizeof(std::uint32_t)
                        && contiguous_width_bytes_
                            != sizeof(std::uint64_t))))) {
            return false;
        }
        if (!positions_complete_) {
            return active_shapes == 0u;
        }
        if (contiguous_size_ != 0u) {
            return global_occurrence_count_ == contiguous_size_;
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
        -> std::same_as<std::span<const std::uint64_t>>;
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

/// Structural interface for an MPF reader that prepares one query wave into
/// final chaining anchors.  The concrete request/scratch/batch types remain
/// owned by the reader module; aliases avoid a circular dependency from this
/// common concept module back to the hashmap implementation.
template <typename T>
concept PreparedFragmentAnchorBatchReader = requires (
    const T& const_reader,
    bioseq::parallelism::MemoryAdmissionController& admission,
    std::span<const typename T::fragment_seed_query_type> queries,
    typename T::query_wave_scratch_type& scratch,
    std::int32_t max_occ) {
    typename T::fragment_seed_query_type;
    typename T::query_wave_scratch_type;
    typename T::prepared_fragment_anchor_batch_type;
    { const_reader.prepare_fragment_anchor_batch(
        admission, queries, max_occ, scratch) }
        -> std::same_as<typename T::prepared_fragment_anchor_batch_type>;
};

/// Per-query borrowed view produced by `PreparedFragmentAnchorBatchReader`.
/// Anchors are already in the final canonical order and remain owned by the
/// wave batch, so the pipeline must neither copy nor globally re-sort them.
template <typename T>
concept PreparedFragmentAnchorQueryIndex = requires (
    const T& const_index,
    std::span<const MinimapSketchData> query_minimizers,
    bioseq::storage::sequence_size_t query_length,
    std::int32_t max_occ,
    std::int32_t kmer_size) {
    { const_index.target_length() }
        -> std::convertible_to<bioseq::storage::sequence_size_t>;
    { const_index.ordered_fragment_seed_anchors(
        query_minimizers, query_length, max_occ, kmer_size) }
        -> std::same_as<std::span<const bioseq::chaining::Anchor>>;
};

/// Any target index shape accepted by the seed stage: either ordinary
/// per-hash lookup or a prepared fragment-anchor query view.
template <typename T>
concept MinimizerSeedIndex =
    MinimizerLookupIndex<T> || PreparedFragmentAnchorQueryIndex<T>;

/// Zero-copy adapter used by seed consumers for both lookup shapes.
template <MinimizerLookupIndex T>
[[nodiscard]] inline MinimizerLookupSpans lookup_minimizer_spans(
        const T& index,
        std::uint64_t kmer_hash) {
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
