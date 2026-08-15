/**
 * @file aligned_byte_buffer.cppm
 * @brief Fallible uninitialized storage with explicit alignment.
 *
 * Over-aligned storage uses `posix_memalign`. Other storage uses an
 * overwrite allocation. Growth preserves the live prefix.
 */
module;

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <type_traits>

export module bioseq.io.compression.aligned_byte_buffer;

import bioseq.common.logging;

export namespace bioseq::io::compression {

/// Uninitialised-on-extend buffer with explicit element alignment. Backed
/// by `posix_memalign` + custom deleter when `p_alignment` exceeds the
/// default `new` alignment, otherwise by `std::make_unique_for_overwrite`.
template <typename p_value_type, std::size_t p_alignment = alignof(p_value_type)>
struct aligned_buffer {
    static_assert(std::is_trivially_copyable_v<p_value_type>,
                  "aligned_buffer is for trivially-copyable p_value_type only.");
    static_assert((p_alignment & (p_alignment - 1)) == 0, "p_alignment must be a power of two.");

    static constexpr bool use_posix_memalign = (p_alignment > alignof(std::max_align_t));

    /// Releases storage returned by `posix_memalign`.
    struct posix_deleter {
        /// Return an aligned allocation to the C allocator.
        void operator()(p_value_type* ptr) const noexcept {
            std::free(ptr);
        }
    };
    using owning_ptr_t = std::conditional_t<use_posix_memalign,
                                            std::unique_ptr<p_value_type[], posix_deleter>,
                                            std::unique_ptr<p_value_type[]>>;

    owning_ptr_t data;
    std::size_t size = 0;
    std::size_t capacity = 0;

    /// Ensure capacity for `units` elements while preserving the live prefix.
    [[nodiscard]] std::expected<void, std::string> reserve(std::size_t units) noexcept {
        if (units <= capacity)
            return {};
        if constexpr (use_posix_memalign) {
            void* raw = nullptr;
            const std::size_t bytes = units * sizeof(p_value_type);
            if (::posix_memalign(&raw, p_alignment, bytes) != 0) {
                return std::unexpected(fmt::format("aligned_buffer alloc failed: bytes={}", bytes));
            }
            if (size > 0 && data) {
                std::memcpy(raw, data.get(), size * sizeof(p_value_type));
            }
            data.reset(static_cast<p_value_type*>(raw));
        } else {
            auto fresh = std::make_unique_for_overwrite<p_value_type[]>(units);
            if (size > 0 && data) {
                std::memcpy(fresh.get(), data.get(), size * sizeof(p_value_type));
            }
            data = std::move(fresh);
        }
        capacity = units;
        return {};
    }

    /// Append `units` uninitialized elements and return their writable span.
    [[nodiscard]] std::expected<std::span<p_value_type>, std::string>
    extend(std::size_t units) noexcept {
        if (size + units > capacity) {
            const std::size_t needed = size + units;
            const std::size_t doubled = capacity * 2;
            if (auto reserved = reserve(doubled > needed ? doubled : needed); !reserved) {
                return std::unexpected(std::move(reserved.error()));
            }
        }
        std::span<p_value_type> out{data.get() + size, units};
        size += units;
        return out;
    }

    /// Discard the live prefix without releasing the allocation.
    void clear() noexcept {
        size = 0;
    }

    /// Return whether the live prefix contains no elements.
    [[nodiscard]] bool empty() const noexcept {
        return size == 0;
    }
    /// Return the writable live prefix.
    [[nodiscard]] std::span<p_value_type> view() noexcept {
        return {data.get(), size};
    }
    /// Return the readable live prefix.
    [[nodiscard]] std::span<const p_value_type> view() const noexcept {
        return {data.get(), size};
    }
};

/// 64-byte-aligned uint16 buffer for the chunk arena (SIMD loads).
using chunk_marker_buffer = aligned_buffer<std::uint16_t, 64>;
/// 64-byte-aligned uint8 buffer for the chunk arena.
using chunk_byte_buffer = aligned_buffer<std::uint8_t, 64>;

} // namespace bioseq::io::compression
