/**
 * @file aligned_byte_buffer.cppm
 * @brief Generalised uninitialised-extend buffer with explicit alignment.
 *
 * `writer.cppm`'s `writer_byte_buffer` is functionally identical to what
 * the chunk arena needs except it hardcodes `uint8_t` and uses
 * `std::make_unique_for_overwrite` (default `new` alignment, not 64-byte).
 * `aligned_extend_buffer<T, A>` is the generalised form. When `A` exceeds
 * `alignof(std::max_align_t)`, allocation goes through `posix_memalign`
 * with a custom deleter; otherwise through `std::make_unique_for_overwrite`.
 *
 * Both `extend` and `reserve` return `std::expected<...>` so allocation
 * failures are surfaced explicitly rather than terminating via `bad_alloc`.
 */
module;

#include "common.h"

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

import bioseq.common.mlogging;

export namespace bioseq::io::compression {

/// Uninitialised-on-extend buffer with explicit element alignment. Backed
/// by `posix_memalign` + custom deleter when `Alignment` exceeds the
/// default `new`-alignment; otherwise by `std::make_unique_for_overwrite`.
template <typename T, std::size_t Alignment = alignof(T)>
struct aligned_extend_buffer {
    static_assert(std::is_trivially_copyable_v<T>,
                  "aligned_extend_buffer is for trivially-copyable T only.");
    static_assert((Alignment & (Alignment - 1)) == 0,
                  "Alignment must be a power of two.");

    static constexpr bool kUsePosixMemalign =
        (Alignment > alignof(std::max_align_t));

    struct posix_deleter {
        void operator()(T* ptr) const noexcept { std::free(ptr); }
    };
    using owning_ptr_t = std::conditional_t<
        kUsePosixMemalign,
        std::unique_ptr<T[], posix_deleter>,
        std::unique_ptr<T[]>>;

    owning_ptr_t data;
    std::size_t  size     = 0;
    std::size_t  capacity = 0;

    [[nodiscard]] std::expected<void, std::string>
    reserve(std::size_t units) noexcept {
        if (units <= capacity) return {};
        if constexpr (kUsePosixMemalign) {
            void* raw = nullptr;
            const std::size_t bytes = units * sizeof(T);
            if (::posix_memalign(&raw, Alignment, bytes) != 0) {
                return std::unexpected(fmt::format(
                    "aligned_extend_buffer alloc failed: bytes={}", bytes));
            }
            if (size > 0 && data) {
                std::memcpy(raw, data.get(), size * sizeof(T));
            }
            data.reset(static_cast<T*>(raw));
        } else {
            auto fresh = std::make_unique_for_overwrite<T[]>(units);
            if (size > 0 && data) {
                std::memcpy(fresh.get(), data.get(), size * sizeof(T));
            }
            data = std::move(fresh);
        }
        capacity = units;
        return {};
    }

    [[nodiscard]] std::expected<std::span<T>, std::string>
    extend(std::size_t units) noexcept {
        if (size + units > capacity) {
            const std::size_t needed  = size + units;
            const std::size_t doubled = capacity * 2;
            if (auto reserved = reserve(doubled > needed ? doubled : needed);
                !reserved) {
                return std::unexpected(std::move(reserved.error()));
            }
        }
        std::span<T> out{data.get() + size, units};
        size += units;
        return out;
    }

    void clear() noexcept { size = 0; }

    [[nodiscard]] bool                empty() const noexcept { return size == 0; }
    [[nodiscard]] std::span<T>        view()        noexcept { return {data.get(), size}; }
    [[nodiscard]] std::span<const T>  view()  const noexcept { return {data.get(), size}; }
};

/// 64-byte-aligned uint16 buffer for the chunk arena (SIMD loads).
using chunk_uint16_buffer = aligned_extend_buffer<std::uint16_t, 64>;
/// 64-byte-aligned uint8 buffer for the chunk arena.
using chunk_uint8_buffer  = aligned_extend_buffer<std::uint8_t,  64>;

}  // namespace bioseq::io::compression
