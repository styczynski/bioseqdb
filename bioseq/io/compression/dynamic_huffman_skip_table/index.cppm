/**
 * @file index.cppm
 * @brief Stable facade for the dynamic-Huffman skip table.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.dynamic_huffman_skip_table;

import :low_windows;
import :high_windows;

export namespace bioseq::io::compression {

/// Maps a 15-bit window to the distance of its next dynamic-Huffman candidate.
///
/// Negative entries identify a candidate at the absolute distance.
inline constexpr std::array<std::int8_t, 32768> dynamic_huffman_skip_table = []() consteval {
    std::array<std::int8_t, 32768> table{};
    std::size_t offset = 0;

    // Append one generated table partition while retaining descriptor order.
    const auto append = [&table, &offset]<std::size_t p_part_size>(
                            const std::array<std::int8_t, p_part_size>& part) constexpr {
        for (const std::int8_t value : part) {
            table[offset] = value;
            ++offset;
        }
    };

    append(detail::dynamic_huffman_skip_table_low_windows);
    append(detail::dynamic_huffman_skip_table_high_windows);
    return table;
}();

} // namespace bioseq::io::compression
