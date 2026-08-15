/**
 * @file window_map.cppm
 * @brief Trailing decompression window keyed by encoded bit offset.
 */
module;


#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

export module bioseq.io.compression.window_map;


export namespace bioseq::io::compression {

inline constexpr std::size_t window_size_bytes = 32 * 1024;

/// One reusable window entry. `bytes` holds the trailing 32 KiB of
/// the chunk's decoded output; `size` < 32 KiB only at end-of-stream.
/// `encoded_end_bit_offset` is the bit-offset just past the chunk's
/// last DEFLATE block. `generation` distinguishes reused entries.
struct decompression_window {
    std::array<std::uint8_t, window_size_bytes> bytes{};
    std::size_t   size                       = 0;
    std::size_t   encoded_end_bit_offset     = 0;
    std::uint64_t generation                 = 0;
};

}  // namespace bioseq::io::compression
