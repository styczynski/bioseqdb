/**
 * @file buffer.cppm
 * @brief Aligned refill storage and parser cursors.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <hwy/aligned_allocator.h>
#include <libassert/assert.hpp>

export module bioseq.io.buffer;

import bioseq.io.options;

export namespace bioseq::io {

/// One I/O slab: a HWY-aligned byte buffer plus the parser's bookkeeping
/// (fill cursor, parse cursor, EOF marker). The producer fills bytes
/// in `[fill_size, capacity)`. The parser reads from
/// `[parse_cursor, fill_size)`.
struct buffer_slab {
    hwy::AlignedFreeUniquePtr<std::uint8_t[]> data;
    std::size_t capacity = 0;
    std::size_t fill_size = 0;
    std::size_t parse_cursor = 0;
    bool is_eof = false;

    buffer_slab() = default;

    /// Allocate an empty slab with the requested byte capacity.
    explicit buffer_slab(std::size_t initial_capacity)
        : data(hwy::AllocateAligned<std::uint8_t>(initial_capacity)), capacity(initial_capacity) {}

    /// Return the writable start of the slab allocation.
    [[nodiscard]] std::uint8_t* raw() noexcept {
        return data.get();
    }
    /// Return the readable start of the slab allocation.
    [[nodiscard]] const std::uint8_t* raw() const noexcept {
        return data.get();
    }
    /// Return the number of filled bytes not yet consumed by the parser.
    [[nodiscard]] std::size_t remaining() const noexcept {
        return fill_size - parse_cursor;
    }

    /// Slide unconsumed bytes down to offset 0, freeing space at the
    /// tail for the next refill. Returns true if any room was made.
    bool make_room() noexcept {
        DEBUG_ASSERT(parse_cursor <= fill_size,
                     "buffer_slab::make_room: parse_cursor must be within fill_size");
        if (parse_cursor == 0)
            return false;
        const std::size_t unconsumed = fill_size - parse_cursor;
        if (unconsumed > 0) {
            std::memmove(raw(), raw() + parse_cursor, unconsumed);
        }
        fill_size = unconsumed;
        parse_cursor = 0;
        return true;
    }

    /// Grow the slab according to the policy: double up to 8 MiB, then
    /// linear 8 MiB increments forever, capped by `max_buffer_size`.
    /// Return true on success and false when the cap is reached.
    /// Preserves bytes in `[0, fill_size)`.
    bool grow(const reader_options& options) {
        if (capacity >= options.io.max_buffer_size)
            return false;
        // Switch to 8 MiB increments once geometric growth would
        // over-reserve large records.
        constexpr std::size_t geometric_growth_limit_bytes = static_cast<std::size_t>(1)
                                                             << 23; // 8 MiB
        std::size_t new_capacity = (capacity < geometric_growth_limit_bytes)
                                       ? capacity * 2
                                       : capacity + geometric_growth_limit_bytes;
        new_capacity = std::min(new_capacity, options.io.max_buffer_size);
        auto new_data = hwy::AllocateAligned<std::uint8_t>(new_capacity);
        if (fill_size > 0) {
            std::memcpy(new_data.get(), raw(), fill_size);
        }
        data = std::move(new_data);
        capacity = new_capacity;
        return true;
    }
};

} // namespace bioseq::io
