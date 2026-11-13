/**
 * @file buffer.cppm
 * @brief Aligned input buffer shared by the reader and parser.
 */
module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <hwy/aligned_allocator.h>
#include <libassert/assert.hpp>
#include <limits>
#include <utility>

export module bioseq.io.buffer;

import bioseq.io.options;
import bioseq.io.sinks;

export namespace bioseq::io {

/// Aligned input bytes with separate fill and parse positions. The reader
/// writes into [fill_size, capacity), and the parser consumes
/// [parse_cursor, fill_size).
struct buffer_slab {
    hwy::AlignedFreeUniquePtr<std::uint8_t[]> data;
    std::size_t capacity = 0;
    std::size_t fill_size = 0;
    std::size_t parse_cursor = 0;
    bool is_eof = false;

    /// Construct an empty slab without an allocation.
    buffer_slab() = default;

    /// Allocate an empty slab, leaving capacity zero on refusal. Optional Highway
    /// callbacks apply only to this allocation; their free/context must outlive it.
    explicit buffer_slab(std::size_t initial_capacity, hwy::AllocPtr allocate = nullptr,
                         hwy::FreePtr release = nullptr, void* context = nullptr) {
        // Highway asserts for zero and for payloads at least SIZE_MAX/2.
        if (initial_capacity == 0u || initial_capacity >= std::numeric_limits<std::size_t>::max() / 2u)
            return;
        data = hwy::AllocateAligned<std::uint8_t>(initial_capacity, allocate, release, context);
        if (data) {
            capacity = initial_capacity;
            io_resource_diagnostics().observe_input_slab(capacity);
        }
    }

    /// Normalize reader initialization to [1,max] when max is nonzero; max zero
    /// leaves an empty slab for the caller's ordinary failure callback.
    explicit buffer_slab(const reader_options& options, hwy::AllocPtr allocate = nullptr,
                         hwy::FreePtr release = nullptr, void* context = nullptr)
        : buffer_slab(std::min(options.io.max_buffer_size,
              std::max<std::size_t>(1u, options.io.initial_buffer_size)), allocate, release, context) {}

    /// Return a writable pointer to the buffer.
    [[nodiscard]] std::uint8_t* raw() noexcept {
        return data.get();
    }
    /// Return a read-only pointer to the  buffer.
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

    /// Enlarge the buffer while preserving filled bytes. Growth slows to fixed
    /// increments for large records and stops at max_buffer_size.
    /// Return false at the limit or on allocation failure, preserving every field.
    /// Per-call callbacks affect only new storage; each old owner retains its own
    /// Highway deleter. A successful allocation's free/context must outlive it.
    bool grow(const reader_options& options, hwy::AllocPtr allocate = nullptr,
              hwy::FreePtr release = nullptr, void* context = nullptr) {
        if (capacity >= options.io.max_buffer_size)
            return false;
        // Limit spare capacity when a record needs a large buffer.
        constexpr std::size_t geometric_growth_limit_bytes = static_cast<std::size_t>(1)
                                                             << 23; // 8 MiB
        const std::size_t increment = capacity == 0u ? 1u
            : std::min(capacity, geometric_growth_limit_bytes);
        // Bound the increment before adding, including near SIZE_MAX. Empty
        // slabs make positive progress without invoking Highway with zero.
        const std::size_t new_capacity = capacity
            + std::min(increment, options.io.max_buffer_size - capacity);
        if (new_capacity >= std::numeric_limits<std::size_t>::max() / 2u) return false;
        auto new_data = hwy::AllocateAligned<std::uint8_t>(new_capacity, allocate, release, context);
        if (!new_data) return false;
        if (fill_size > 0) {
            std::memcpy(new_data.get(), raw(), fill_size);
        }
        data = std::move(new_data);
        capacity = new_capacity;
        io_resource_diagnostics().observe_input_slab(capacity);
        return true;
    }
};

} // namespace bioseq::io
