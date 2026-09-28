/**
 * @file window_map.cppm
 * @brief Stores the trailing bytes needed by the next decompression chunk.
 */
module;

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.window_map;

export namespace bioseq::io::compression {

inline constexpr std::size_t window_size_bytes = 32 * 1024;

/// Saved trailing bytes from  a decoded chunk, with their valid length.
/// `encoded_end_bit_offset` locates the end of its compressed blocks.
/// `generation` distinguishes successive uses of the same entry.
struct decompression_window {
    std::array<std::uint8_t, window_size_bytes> bytes{};
    std::size_t size = 0;
    std::size_t encoded_end_bit_offset = 0;
    std::uint64_t generation = 0;
};

} // namespace bioseq::io::compression
