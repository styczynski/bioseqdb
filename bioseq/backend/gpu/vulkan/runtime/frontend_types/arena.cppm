/**
 * @file arena.cppm
 * @brief Defines runtime-owned arena slices, reservations, and trimming guards.
 */
module;

#include <cstddef>
#include <cstdint>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

export module bioseq.gpu.vulkan.runtime:frontend_types.arena;

import :receipts;
import :frontend_types.sketch;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.parallelism.memory_admission;
import std;

export namespace bioseq::gpu::vulkan {

// ===========================================================================
// Runtime-owned GPU arena
// ===========================================================================

/// @brief A view into one arena buffer: `(buffer, offset, size)`. Descriptor writes
/// bind exactly this sub-range, so several logical resources can share one
/// physical arena buffer.
struct gpu_arena_slice {
    buffer* buffer = nullptr;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;

    [[nodiscard]] bool is_valid() const noexcept {
        return buffer != nullptr;
    }
};

/// @brief A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct gpu_arena_mark {
    std::uint64_t device_cursor = 0;
    std::uint64_t upload_cursor = 0;
    std::uint64_t download_cursor = 0;
};

/// @brief Per-runtime arena with three geometrically-grown, persistently-mapped
/// buffers (device-local, host-visible upload, host-visible download). Slices
/// are bump-allocated with alignment. `rewind` frees everything past a mark.
/// Growth is allowed only outside an active count and execution submission pair.
class gpu_arena {
  public:
    explicit gpu_arena(runtime& runtime) noexcept : runtime_(&runtime) {}

    [[nodiscard]] gpu_arena_mark mark() const noexcept {
        return gpu_arena_mark{device_cursor_, upload_cursor_, download_cursor_};
    }
    void rewind(gpu_arena_mark mark) noexcept {
        device_cursor_ = mark.device_cursor;
        upload_cursor_ = mark.upload_cursor;
        download_cursor_ = mark.download_cursor;
    }

    /// @brief Reserve worst-case capacity for one count and execution submission pair
    /// before count submit,
    /// so no buffer is reallocated between the two submits. Returns false if any
    /// grow failed (caller declines GPU frontend).
    /// @return `true` when every arena buffer satisfies the requested capacity.
    [[nodiscard]] bool ensure_capacity(std::uint64_t device_bytes,
                                       std::uint64_t upload_bytes,
                                       std::uint64_t download_bytes);

    [[nodiscard]] gpu_arena_slice allocate_device(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] gpu_arena_slice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    [[nodiscard]] gpu_arena_slice allocate_download(std::uint64_t bytes, std::uint64_t alignment);

    void bind_storage_slice(VkWriteDescriptorSet& write,
                            VkDescriptorBufferInfo& info,
                            std::uint32_t binding,
                            VkDescriptorSet dst_set,
                            const gpu_arena_slice& slice) const noexcept;

    [[nodiscard]] buffer& device_buffer() noexcept {
        return device_;
    }
    [[nodiscard]] buffer& upload_buffer() noexcept {
        return upload_;
    }
    [[nodiscard]] buffer& download_buffer() noexcept {
        return download_;
    }

    /// @brief Destroy every retained backing buffer after an operation has rewound all
    /// of its slices. Seed lookup is a useful full-lifetime boundary: its output
    /// is copied to owned host storage, while adopted target-index buffers are
    /// standalone allocations and therefore cannot alias this arena. Releasing
    /// the idle arena prevents large transient high-water from leaking across
    /// otherwise independent benchmark/production requests.
    /// @return `true` when all idle backing capacity was released.
    [[nodiscard]] bool release_idle_capacity() noexcept;

  private:
    [[nodiscard]] static std::uint64_t align_up(std::uint64_t value,
                                                std::uint64_t alignment) noexcept {
        if (alignment <= 1u)
            return value;
        return bioseq::common::checked_align_up(value, alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    }

    runtime* runtime_ = nullptr;
    buffer device_;
    buffer upload_;
    buffer download_;
    std::uint64_t device_cursor_ = 0;
    std::uint64_t upload_cursor_ = 0;
    std::uint64_t download_cursor_ = 0;
    std::uint64_t device_capacity_ = 0;
    std::uint64_t upload_capacity_ = 0;
    std::uint64_t download_capacity_ = 0;
};

/// @brief RAII arena reservation: holds the admission permit, marks the arena on
/// construction, and rewinds on destruction. Spans count submit + execution submit.
class gpu_arena_reservation {
  public:
    gpu_arena_reservation(gpu_arena& arena, bioseq::parallelism::memory_permit permit) noexcept
        : arena_(&arena), permit_(std::move(permit)), mark_(arena.mark()) {}
    gpu_arena_reservation(const gpu_arena_reservation&) = delete;
    gpu_arena_reservation& operator=(const gpu_arena_reservation&) = delete;
    gpu_arena_reservation(gpu_arena_reservation&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)), permit_(std::move(other.permit_)),
          mark_(other.mark_) {}
    gpu_arena_reservation& operator=(gpu_arena_reservation&& other) noexcept {
        if (this != &other) {
            release();
            arena_ = std::exchange(other.arena_, nullptr);
            permit_ = std::move(other.permit_);
            mark_ = other.mark_;
        }
        return *this;
    }
    ~gpu_arena_reservation() {
        release();
    }
    void release() noexcept {
        if (arena_ != nullptr) {
            arena_->rewind(mark_);
            arena_ = nullptr;
        }
    }
    [[nodiscard]] gpu_arena& arena() noexcept {
        return *arena_;
    }

  private:
    gpu_arena* arena_ = nullptr;
    bioseq::parallelism::memory_permit permit_;
    gpu_arena_mark mark_{};
};

/// @brief Request-boundary cleanup for operations whose result never aliases the
/// shared GPU frontend arena.  Rewinding alone leaves the arena's allocator-rounded
/// high-water live. This guard also destroys all idle backing buffers. It is
/// intentionally independent of an admission permit so it can be installed
/// before arena growth and covers every return and exception path.
class gpu_arena_trim_guard {
  public:
    explicit gpu_arena_trim_guard(gpu_arena& arena) noexcept
        : arena_(&arena), mark_(arena.mark()) {}
    gpu_arena_trim_guard(const gpu_arena_trim_guard&) = delete;
    gpu_arena_trim_guard& operator=(const gpu_arena_trim_guard&) = delete;
    gpu_arena_trim_guard(gpu_arena_trim_guard&&) = delete;
    gpu_arena_trim_guard& operator=(gpu_arena_trim_guard&&) = delete;
    ~gpu_arena_trim_guard() {
        (void)release_now();
    }

    [[nodiscard]] bool release_now() noexcept {
        if (arena_ == nullptr) {
            return true;
        }
        gpu_arena* arena = std::exchange(arena_, nullptr);
        arena->rewind(mark_);
        return arena->release_idle_capacity();
    }

  private:
    gpu_arena* arena_ = nullptr;
    gpu_arena_mark mark_{};
};

} // namespace bioseq::gpu::vulkan
