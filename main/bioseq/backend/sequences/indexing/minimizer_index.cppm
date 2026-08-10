/**
 * @file minimizer_index.cppm
 * @brief Shared minimizer lookup/persistence concepts and runtime segment range.
 *
 * Lives in `bioseq::indexing`; Highway-namespace consumers reference it
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
import bioseq.chaining.core; // prepared fragment-anchor query view
import bioseq.storage.size;        // sequence_size_t
import bioseq.parallelism.memory_admission;
static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::indexing {
using bioseq::minimizers::minimap_sketch_data;

/// Fail-closed error raised when an authenticated persisted lookup cannot
/// provide a complete, internally consistent position result.  PostgreSQL
/// callers catch this named type before crossing the longjmp ERROR boundary.
class persisted_minimizer_lookup_error final : public std::runtime_error {
public:
    enum class reason : std::uint8_t {
        unspecified,
        admission_or_allocation,
    };

private:
    reason reason_ = reason::unspecified;

public:
    using std::runtime_error::runtime_error;

    persisted_minimizer_lookup_error(
            reason reason,
            const char* message)
        : std::runtime_error(message), reason_(reason) {}

    [[nodiscard]] reason failure_reason() const noexcept {
        return reason_;
    }
};

/// One borrowed, chronologically encoded position run for a minimizer hash.
struct minimizer_position_span {
    std::span<const std::uint64_t> positions{};
};

/**
 * Allocation-free lookup result for either one contiguous positions run or
 * several persisted segment-local runs.  `global_occurrence_count` is
 * authoritative even when high-frequency positions were deliberately not
 * loaded; a consumer may prune on that count before requiring complete spans.
 */
class minimizer_lookup_spans {
private:
    const void* contiguous_data_ = nullptr;
    std::size_t contiguous_size_ = 0u;
    std::span<const minimizer_position_span> segmented_{};
    std::uint64_t global_occurrence_count_ = 0;
    std::uint8_t contiguous_width_bytes_ = 0u;
    bool positions_complete_ = true;

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
    constexpr minimizer_lookup_spans() noexcept = default;

    [[nodiscard]] static constexpr minimizer_lookup_spans contiguous(
            std::span<const std::uint64_t> positions) noexcept {
        return minimizer_lookup_spans{
            positions.data(), positions.size(), sizeof(std::uint64_t), {},
            positions.size(), /*positions_complete=*/true};
    }

    /// Borrow one directly consumable packed-32 position run.
    [[nodiscard]] static constexpr minimizer_lookup_spans contiguous(
            std::span<const std::uint32_t> positions) noexcept {
        return minimizer_lookup_spans{
            positions.data(), positions.size(), sizeof(std::uint32_t), {},
            positions.size(), /*positions_complete=*/true};
    }

    [[nodiscard]] static constexpr minimizer_lookup_spans segmented(
            std::span<const minimizer_position_span> spans,
            std::uint64_t global_occurrence_count,
            bool positions_complete = true) noexcept {
        return minimizer_lookup_spans{
            nullptr, 0u, 0u, spans, global_occurrence_count,
            positions_complete};
    }

    [[nodiscard]] static constexpr minimizer_lookup_spans positions_unavailable(
            std::uint64_t global_occurrence_count) noexcept {
        return minimizer_lookup_spans{
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

    [[nodiscard]] constexpr std::span<const minimizer_position_span>
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
        for (const minimizer_position_span& span : segmented_) {
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

template <typename p_index>
concept contiguous_minimizer_lookup_index = requires (
    const p_index& const_index,
    std::uint64_t kmer_hash) {
    { const_index.lookup(kmer_hash) }
        -> std::same_as<std::span<const std::uint64_t>>;
};

template <typename p_index>
concept segmented_minimizer_lookup_index = requires (
    const p_index& const_index,
    std::uint64_t kmer_hash) {
    { const_index.lookup_spans(kmer_hash) }
        -> std::same_as<minimizer_lookup_spans>;
};

/// Any read-only minimizer lookup structure usable by seed/pipeline consumers.
template <typename p_index>
concept minimizer_lookup_index = requires (
    const p_index& const_index) {
    { const_index.target_length() }          -> std::convertible_to<bioseq::storage::sequence_size_t>;
} && (contiguous_minimizer_lookup_index<p_index> || segmented_minimizer_lookup_index<p_index>);

/// Structural interface for an MPF reader that prepares one query wave into
/// final chaining anchors.  The concrete request/scratch/batch types remain
/// owned by the reader module; aliases avoid a circular dependency from this
/// common concept module back to the hashmap implementation.
template <typename p_index>
concept prepared_fragment_anchor_batch_reader = requires (
    const p_index& const_reader,
    bioseq::parallelism::memory_admission_controller& admission,
    std::span<const typename p_index::fragment_seed_query_type> queries,
    typename p_index::query_wave_scratch_type& scratch,
    std::int32_t max_occ) {
    typename p_index::fragment_seed_query_type;
    typename p_index::query_wave_scratch_type;
    typename p_index::prepared_fragment_anchor_batch_type;
    { const_reader.prepare_fragment_anchor_batch(
        admission, queries, max_occ, scratch) }
        -> std::same_as<typename p_index::prepared_fragment_anchor_batch_type>;
};

/// Per-query borrowed view produced by `prepared_fragment_anchor_batch_reader`.
/// Anchors are already in the final canonical order and remain owned by the
/// wave batch, so the pipeline must neither copy nor globally re-sort them.
template <typename p_index>
concept prepared_fragment_anchor_query_index = requires (
    const p_index& const_index,
    std::span<const minimap_sketch_data> query_minimizers,
    bioseq::storage::sequence_size_t query_length,
    std::int32_t max_occ,
    std::int32_t kmer_size) {
    { const_index.target_length() }
        -> std::convertible_to<bioseq::storage::sequence_size_t>;
    { const_index.ordered_fragment_seed_anchors(
        query_minimizers, query_length, max_occ, kmer_size) }
        -> std::same_as<std::span<const bioseq::chaining::anchor>>;
};

/// Any target index shape accepted by the seed stage: either ordinary
/// per-hash lookup or a prepared fragment-anchor query view.
template <typename p_index>
concept minimizer_seed_index =
    minimizer_lookup_index<p_index> || prepared_fragment_anchor_query_index<p_index>;

/// Zero-copy adapter used by seed consumers for both lookup shapes.
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

/// A lookup index that can also be built and persisted in the direct binary form.
template <typename p_index>
concept persistable_minimizer_index = minimizer_lookup_index<p_index> && requires (
    p_index& mutable_index,
    const p_index& const_index,
    const std::vector<minimap_sketch_data>& sketches,
    bioseq::storage::sequence_size_t target_length_arg,
    char* output_buffer,
    const char* input_buffer,
    std::size_t input_size) {
    { mutable_index.build(sketches, target_length_arg) } -> std::same_as<void>;
    { const_index.serialize_binary_size() }  -> std::convertible_to<std::size_t>;
    { const_index.serialize_to_binary(output_buffer) } -> std::same_as<void>;
    { p_index::deserialize_from_binary(input_buffer, input_size) } -> std::same_as<p_index>;
};

/// Runtime collection/core geometry shared across every Highway target.
struct segment_unit_range {
    bioseq::storage::sequence_size_t cs_u;
    bioseq::storage::sequence_size_t ce_u;
    bioseq::storage::sequence_size_t core_lo_bp;
    bioseq::storage::sequence_size_t core_hi_bp;
};

}  // namespace bioseq::indexing
