/**
 * @file index.cppm
 * @brief Combines the two halves of the dynamic-Huffman search table.
 */
module;

#include <array>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.dynamic_huffman_skip_table;

import :low_windows;
import :high_windows;

export namespace bioseq::io::compression {

/// Search hints indexed by the  next 15 compressed bits. Positive values
/// give a bit count to skip. Negative values tell the block finder to
/// validate a possible dynamic-Huffman header at its current position.
inline constexpr std::array<std::int8_t, 32768> dynamic_huffman_skip_table = []() consteval {
    std::array<std::int8_t, 32768> table{};
    std::size_t offset = 0;

    // Append this half in increasing table-index order.
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
