module;

#include <hwy/aligned_allocator.h>

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

export module bioseq.io.buffer;

import bioseq.io.options;

export namespace bioseq::io {

/// One I/O slab: a HWY-aligned byte buffer plus the parser's bookkeeping
/// (fill cursor, parse cursor, EOF marker). The producer fills bytes
/// in `[fill_size, capacity)`; the parser reads from
/// `[parse_cursor, fill_size)`. Spec §7.1.
struct buffer_slab {
    hwy::AlignedFreeUniquePtr<std::uint8_t[]> data;
    std::size_t capacity = 0;
    std::size_t fill_size = 0;
    std::size_t parse_cursor = 0;
    bool        is_eof = false;

    buffer_slab() = default;

    explicit buffer_slab(std::size_t initial_capacity)
        : data(hwy::AllocateAligned<std::uint8_t>(initial_capacity)),
          capacity(initial_capacity) {}

    [[nodiscard]] std::uint8_t* raw() noexcept { return data.get(); }
    [[nodiscard]] const std::uint8_t* raw() const noexcept { return data.get(); }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return fill_size - parse_cursor;
    }

    /// Slide unconsumed bytes down to offset 0, freeing space at the
    /// tail for the next refill. Returns true if any room was made.
    /// Spec §7.2.
    bool make_room() noexcept {
        DEBUG_ASSERT(parse_cursor <= fill_size,
                     "buffer_slab::make_room: parse_cursor must be within fill_size");
        if (parse_cursor == 0) return false;
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
    /// Returns true on success; false if the cap was hit.
    /// Preserves bytes in `[0, fill_size)`. Spec §7.2.
    bool grow(const reader_options& options) {
        if (capacity >= options.io.max_buffer_size) return false;
        // Threshold above which we switch from geometric (doubling)
        // to linear (8 MiB increments) growth — borrowed from
        // seq_io's `StdPolicy` (deep-dive §3.1). Doubling past 8 MiB
        // would over-reserve for large FASTA records; linear growth
        // amortises the realloc cost without burning excess RAM.
        constexpr std::size_t kGeometricToLinearThresholdBytes =
            static_cast<std::size_t>(1) << 23;  // 8 MiB
        std::size_t new_capacity = (capacity < kGeometricToLinearThresholdBytes)
                                       ? capacity * 2
                                       : capacity + kGeometricToLinearThresholdBytes;
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

}  // namespace bioseq::io
