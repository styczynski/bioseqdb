/**
 * @file minimap2_sketch_baseline.cppm
 * @brief Exact Minimap2 minimizer control implementation.
 *
 * The implementation preserves Minimap2's comparison, emission, and circular
 * window order while exposing the result through production C++ types.
 */
module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

export module bioseq.sequences.kmers.minimap2_sketch_baseline;

import bioseq.common.seqan_adapter;
import bioseq.sequences;

export namespace bioseq::minimap2_sketch_baseline_detail {

/// @brief Minimap2 nucleotide ranks with non-ACGT symbols mapped to four.
inline constexpr std::array<unsigned char, 256> k_seq_nt4_table = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 0, 4, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 0, 4, 1, 4, 4, 4, 2, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4};

/// @brief Computes Minimap2's masked 64-bit integer hash.
/// @param key Canonical two-bit k-mer value.
/// @param mask Low-bit mask for the selected k-mer width.
/// @return Mixed hash restricted to `mask`.
[[nodiscard]] inline constexpr uint64_t hash64(uint64_t key, uint64_t mask) noexcept {
    key = (~key + (key << 21)) & mask; // key = (key << 21) - key - 1;
    key = key ^ key >> 24;
    key = ((key + (key << 3)) + (key << 8)) & mask; // key * 265
    key = key ^ key >> 14;
    key = ((key + (key << 2)) + (key << 4)) & mask; // key * 21
    key = key ^ key >> 28;
    key = (key + (key << 31)) & mask;
    return key;
}

/// @brief Stores one minimizer in Minimap2's two-word encoding.
struct minimap2_sketch_record {
    /// Hash and k-mer span packed as `hash << 8 | span`.
    uint64_t x;

    /// Sequence identifier, end position, and strand packed in Minimap2 form.
    uint64_t y;
};

/**
 * @brief Collects minimizers with Minimap2's exact rolling-window semantics.
 *
 * @param sequence ACGT input bytes, with other symbols resetting rolling state.
 * @param sequence_size Number of bytes in `sequence`.
 * @param window_size Window size in the range accepted by Minimap2.
 * @param kmer_size K-mer length in the range accepted by Minimap2.
 * @param sequence_id Identifier packed into each record's high 32 bits.
 * @return Minimizers in Minimap2 emission order without sorting or deduplication.
 *
 * @pre `0 < window_size < 256` and `0 < kmer_size <= 28`
 */
[[nodiscard]] inline std::vector<minimap2_sketch_record>
collect_minimap2_sketch(const char* sequence,
                        int sequence_size,
                        int window_size,
                        int kmer_size,
                        uint32_t sequence_id = 0) {
    constexpr uint64_t k_uint64_max = ~uint64_t{0};

    const uint64_t shift1 = 2u * static_cast<uint64_t>(kmer_size - 1);
    const uint64_t mask = (uint64_t{1} << (2u * static_cast<uint64_t>(kmer_size))) - 1u;
    std::array<uint64_t, 2> kmer{};

    int kmer_span = 0;
    std::vector<minimap2_sketch_record> buffer(static_cast<size_t>(window_size),
                                               minimap2_sketch_record{k_uint64_max, k_uint64_max});
    minimap2_sketch_record minimum{k_uint64_max, k_uint64_max};

    int valid_run_size = 0;
    int buffer_position = 0;
    int minimum_position = 0;
    std::vector<minimap2_sketch_record> output;
    output.reserve(static_cast<size_t>(sequence_size) / static_cast<size_t>(window_size) + 1u);

    for (int sequence_position = 0; sequence_position < sequence_size; ++sequence_position) {
        const int rank = k_seq_nt4_table[static_cast<unsigned char>(sequence[sequence_position])];
        minimap2_sketch_record record{k_uint64_max, k_uint64_max};
        if (rank < 4) {
            kmer_span = (valid_run_size + 1 < kmer_size) ? (valid_run_size + 1) : kmer_size;
            kmer[0] = (kmer[0] << 2 | static_cast<uint64_t>(rank)) & mask;
            kmer[1] = (kmer[1] >> 2)
                    | ((uint64_t{3} ^ static_cast<uint64_t>(rank)) << shift1);
            if (kmer[0] == kmer[1]) continue;
            const int strand = (kmer[0] < kmer[1]) ? 0 : 1;
            ++valid_run_size;
            if (valid_run_size >= kmer_size && kmer_span < 256) {
                record.x = (hash64(kmer[strand], mask) << 8)
                    | static_cast<uint64_t>(kmer_span);
                record.y = (static_cast<uint64_t>(sequence_id) << 32) |
                           (static_cast<uint64_t>(static_cast<uint32_t>(sequence_position)) << 1) |
                           static_cast<uint64_t>(strand);
            }
        } else {
            valid_run_size = 0;
            kmer_span = 0;
        }
        buffer[static_cast<size_t>(buffer_position)] = record;
        if (valid_run_size == window_size + kmer_size - 1 && minimum.x != k_uint64_max) {
            for (int position = buffer_position + 1; position < window_size; ++position) {
                if (minimum.x == buffer[static_cast<size_t>(position)].x
                    && buffer[static_cast<size_t>(position)].y != minimum.y)
                {
                    output.push_back(buffer[static_cast<size_t>(position)]);
                }
            }
            for (int position = 0; position < buffer_position; ++position) {
                if (minimum.x == buffer[static_cast<size_t>(position)].x
                    && buffer[static_cast<size_t>(position)].y != minimum.y)
                {
                    output.push_back(buffer[static_cast<size_t>(position)]);
                }
            }
        }
        if (record.x <= minimum.x) {
            if (valid_run_size >= window_size + kmer_size && minimum.x != k_uint64_max) {
                output.push_back(minimum);
            }
            minimum = record;
            minimum_position = buffer_position;
        }
        else if (buffer_position == minimum_position) {
            if (valid_run_size >= window_size + kmer_size - 1 && minimum.x != k_uint64_max) {
                output.push_back(minimum);
            }
            minimum.x = k_uint64_max;
            for (int position = buffer_position + 1; position < window_size; ++position) {
                if (minimum.x >= buffer[static_cast<size_t>(position)].x) {
                    minimum = buffer[static_cast<size_t>(position)];
                    minimum_position = position;
                }
            }
            for (int position = 0; position <= buffer_position; ++position) {
                if (minimum.x >= buffer[static_cast<size_t>(position)].x) {
                    minimum = buffer[static_cast<size_t>(position)];
                    minimum_position = position;
                }
            }
            if (valid_run_size >= window_size + kmer_size - 1 && minimum.x != k_uint64_max) {
                for (int position = buffer_position + 1; position < window_size; ++position) {
                    if (minimum.x == buffer[static_cast<size_t>(position)].x
                        && minimum.y != buffer[static_cast<size_t>(position)].y)
                    {
                        output.push_back(buffer[static_cast<size_t>(position)]);
                    }
                }
                for (int position = 0; position <= buffer_position; ++position) {
                    if (minimum.x == buffer[static_cast<size_t>(position)].x
                        && minimum.y != buffer[static_cast<size_t>(position)].y)
                    {
                        output.push_back(buffer[static_cast<size_t>(position)]);
                    }
                }
            }
        }
        if (++buffer_position == window_size)
            buffer_position = 0;
    }
    if (minimum.x != k_uint64_max) {
        output.push_back(minimum);
    }
    return output;
}

}  // namespace bioseq::minimap2_sketch_baseline_detail

export namespace bioseq::simd {

/**
 * @brief Collects sorted, unique Minimap2 minimizer position/hash pairs.
 *
 * @tparam p_sequence_range Packed sequence configuration.
 * @param sequence Borrowed compact sequence view.
 * @param window_size Minimizer window length in k-mers.
 * @param kmer_size K-mer length in biological symbols.
 * @return Sorted, deduplicated `(last_position, kmer_hash)` pairs.
 */
template <supports_compact_sequence_range p_sequence_range>
[[nodiscard]] inline std::vector<std::pair<size_t, uint64_t>>
minimap2_sketch_baseline_pairs(compact_sequence_range<p_sequence_range>& sequence,
                               const size_t window_size,
                               const size_t kmer_size) {
    using alphabet_type =
        typename compact_sequence_range<p_sequence_range>::traits::alphabet_type;
    using rank_type = seqan3::alphabet_rank_t<alphabet_type>;

    std::string acgt;
    acgt.reserve(sequence.bp_size());
    alphabet_type letter{};
    for (const auto& base_pair_rank : sequence.template iterable<logical_rank>()) {
        letter.assign_rank(static_cast<rank_type>(base_pair_rank));
        acgt.push_back(letter.to_char());
    }

    const auto records = ::bioseq::minimap2_sketch_baseline_detail::collect_minimap2_sketch(
        acgt.c_str(),
        static_cast<int>(acgt.size()),
        static_cast<int>(window_size),
        static_cast<int>(kmer_size));

    std::vector<std::pair<size_t, uint64_t>> pairs;
    pairs.reserve(records.size());
    for (const auto& record : records) {
        const uint64_t kmer_hash = record.x >> 8;
        const size_t last_pos = static_cast<size_t>(
            (record.y >> 1) & uint64_t{0xFFFFFFFFu});
        pairs.emplace_back(last_pos, kmer_hash);
    }
    std::ranges::sort(pairs);
    const auto unique_end = std::unique(pairs.begin(), pairs.end());
    pairs.resize(static_cast<size_t>(unique_end - pairs.begin()));
    return pairs;
}

}  // namespace bioseq::simd
