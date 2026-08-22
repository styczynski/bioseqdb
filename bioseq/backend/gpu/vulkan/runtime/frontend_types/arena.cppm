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
    buffer* buffer = nullptr;       ///< Arena backing buffer, or null when invalid.
    std::uint64_t offset = 0;       ///< Byte offset from the buffer base.
    std::uint64_t size = 0;         ///< Sub-range byte length bound into descriptors.

    /// @brief Reports whether the slice refers to an arena buffer.
    /// @return `true` when the buffer pointer is non-null.
    [[nodiscard]] bool is_valid() const noexcept {
        return buffer != nullptr;
    }
};

/// @brief A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct gpu_arena_mark {
    std::uint64_t device_cursor = 0;   ///< Device-local bump cursor at `mark()`.
    std::uint64_t upload_cursor = 0;    ///< Host-visible upload bump cursor at `mark()`.
    std::uint64_t download_cursor = 0;  ///< Host-visible download bump cursor at `mark()`.
};

/// @brief Per-runtime arena with three geometrically-grown, persistently-mapped
/// buffers (device-local, host-visible upload, host-visible download). Slices
/// are bump-allocated with alignment. `rewind` frees everything past a mark.
/// Growth is allowed only outside an active count and execution submission pair.
class gpu_arena {
  public:
    /// @brief Associates an empty arena with its owning runtime.
    /// @param[in] runtime Runtime that allocates and destroys backing buffers.
    explicit gpu_arena(runtime& runtime) noexcept : runtime_(&runtime) {}

    /// @brief Captures the current allocation cursors.
    /// @return Cursor snapshot suitable for `rewind`.
    [[nodiscard]] gpu_arena_mark mark() const noexcept {
        return gpu_arena_mark{device_cursor_, upload_cursor_, download_cursor_};
    }
    /// @brief Restores all allocation cursors to a prior snapshot.
    /// @param[in] mark Cursor snapshot to restore.
    void rewind(gpu_arena_mark mark) noexcept {
        device_cursor_ = mark.device_cursor;
        upload_cursor_ = mark.upload_cursor;
        download_cursor_ = mark.download_cursor;
    }

    /// @brief Reserve worst-case capacity for one count and execution submission pair
    /// before count submit,
    /// so no buffer is reallocated between the two submits. Returns false if any
    /// grow failed (caller declines GPU frontend).
    /// @param[in] device_bytes Worst-case device-local bytes for the pair.
    /// @param[in] upload_bytes Worst-case host-visible upload bytes for the pair.
    /// @param[in] download_bytes Worst-case host-visible download bytes for the pair.
    /// @return `true` when every arena buffer satisfies the requested capacity.
    [[nodiscard]] bool ensure_capacity(std::uint64_t device_bytes,
                                       std::uint64_t upload_bytes,
                                       std::uint64_t download_bytes);

    /// @brief Bump-allocates an aligned slice from device-local storage.
    /// @param[in] bytes Logical byte size to reserve.
    /// @param[in] alignment Alignment requirement in bytes.
    /// @return Allocated device slice, or an invalid slice on failure.
    [[nodiscard]] gpu_arena_slice allocate_device(std::uint64_t bytes, std::uint64_t alignment);
    /// @brief Bump-allocates an aligned slice from host-visible upload storage.
    /// @param[in] bytes Logical byte size to reserve.
    /// @param[in] alignment Alignment requirement in bytes.
    /// @return Allocated upload slice, or an invalid slice on failure.
    [[nodiscard]] gpu_arena_slice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    /// @brief Bump-allocates an aligned slice from host-visible download storage.
    /// @param[in] bytes Logical byte size to reserve.
    /// @param[in] alignment Alignment requirement in bytes.
    /// @return Allocated download slice, or an invalid slice on failure.
    [[nodiscard]] gpu_arena_slice allocate_download(std::uint64_t bytes, std::uint64_t alignment);

    /// @brief Binds an arena slice to one storage-buffer descriptor write.
    /// @param[out] write Descriptor write receiving the storage binding.
    /// @param[out] info Buffer info populated with offset and range.
    /// @param[in] binding Descriptor binding index within `dst_set`.
    /// @param[in] dst_set Target descriptor set.
    /// @param[in] slice Arena sub-range to bind.
    void bind_storage_slice(VkWriteDescriptorSet& write,
                            VkDescriptorBufferInfo& info,
                            std::uint32_t binding,
                            VkDescriptorSet dst_set,
                            const gpu_arena_slice& slice) const noexcept;

    /// @brief Returns the device-local backing buffer.
    /// @return Mutable device-local buffer owner.
    [[nodiscard]] buffer& device_buffer() noexcept {
        return device_;
    }
    /// @brief Returns the host-visible upload backing buffer.
    /// @return Mutable upload buffer owner.
    [[nodiscard]] buffer& upload_buffer() noexcept {
        return upload_;
    }
    /// @brief Returns the host-visible download backing buffer.
    /// @return Mutable download buffer owner.
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
    /// @brief Rounds a byte offset up to the requested alignment without overflow.
    /// @return Aligned offset, or the maximum `uint64_t` value on overflow.
    [[nodiscard]] static std::uint64_t align_up(std::uint64_t value,
                                                std::uint64_t alignment) noexcept {
        if (alignment <= 1u)
            return value;
        return bioseq::common::checked_align_up(value, alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    }

    runtime* runtime_ = nullptr;              ///< Owning runtime that allocates backing buffers.
    buffer device_;                         ///< Device-local arena backing buffer.
    buffer upload_;                         ///< Host-visible upload arena backing buffer.
    buffer download_;                       ///< Host-visible download arena backing buffer.
    std::uint64_t device_cursor_ = 0;       ///< Current device-local bump offset.
    std::uint64_t upload_cursor_ = 0;       ///< Current upload bump offset.
    std::uint64_t download_cursor_ = 0;     ///< Current download bump offset.
    std::uint64_t device_capacity_ = 0;     ///< Reserved device-local capacity in bytes.
    std::uint64_t upload_capacity_ = 0;     ///< Reserved upload capacity in bytes.
    std::uint64_t download_capacity_ = 0;   ///< Reserved download capacity in bytes.
};

/// @brief RAII arena reservation: holds the admission permit, marks the arena on
/// construction, and rewinds on destruction. Spans count submit + execution submit.
class gpu_arena_reservation {
  public:
    /// @brief Takes an admission permit and records the arena rewind point.
    /// @param[in] arena Arena whose cursors are marked for later rewind.
    /// @param[in] permit Admission permit held for the count and execution submit pair.
    gpu_arena_reservation(gpu_arena& arena, bioseq::parallelism::memory_permit permit) noexcept
        : arena_(&arena), permit_(std::move(permit)), mark_(arena.mark()) {}
    /// @brief Prevents copying an owning arena reservation.
    gpu_arena_reservation(const gpu_arena_reservation&) = delete;
    /// @brief Prevents copy-assignment of an owning arena reservation.
    gpu_arena_reservation& operator=(const gpu_arena_reservation&) = delete;
    /// @brief Transfers the reservation and invalidates the source.
    gpu_arena_reservation(gpu_arena_reservation&& other) noexcept
        : arena_(std::exchange(other.arena_, nullptr)), permit_(std::move(other.permit_)),
          mark_(other.mark_) {}
    /// @brief Releases the current reservation, then transfers another reservation.
    /// @return This reservation.
    gpu_arena_reservation& operator=(gpu_arena_reservation&& other) noexcept {
        if (this != &other) {
            release();
            arena_ = std::exchange(other.arena_, nullptr);
            permit_ = std::move(other.permit_);
            mark_ = other.mark_;
        }
        return *this;
    }
    /// @brief Rewinds the arena when the reservation still owns it.
    ~gpu_arena_reservation() {
        release();
    }
    /// @brief Rewinds the arena once and relinquishes the reservation.
    void release() noexcept {
        if (arena_ != nullptr) {
            arena_->rewind(mark_);
            arena_ = nullptr;
        }
    }
    /// @brief Returns the reserved arena.
    /// @return Arena associated with this live reservation.
    [[nodiscard]] gpu_arena& arena() noexcept {
        return *arena_;
    }

  private:
    gpu_arena* arena_ = nullptr;                         ///< Reserved arena, or null after `release`.
    bioseq::parallelism::memory_permit permit_;          ///< Peak admission held across both submits.
    gpu_arena_mark mark_{};                            ///< Cursor snapshot restored on rewind.
};

/// @brief Request-boundary cleanup for operations whose result never aliases the
/// shared GPU frontend arena.  Rewinding alone leaves the arena's allocator-rounded
/// high-water live. This guard also destroys all idle backing buffers. It is
/// intentionally independent of an admission permit so it can be installed
/// before arena growth and covers every return and exception path.
class gpu_arena_trim_guard {
  public:
    /// @brief Records an arena mark for request-boundary trimming.
    /// @param[in] arena Arena trimmed and rewound at guard destruction.
    explicit gpu_arena_trim_guard(gpu_arena& arena) noexcept
        : arena_(&arena), mark_(arena.mark()) {}
    /// @brief Prevents copying a trim guard.
    gpu_arena_trim_guard(const gpu_arena_trim_guard&) = delete;
    /// @brief Prevents copy-assignment of a trim guard.
    gpu_arena_trim_guard& operator=(const gpu_arena_trim_guard&) = delete;
    /// @brief Prevents moving a trim guard away from its lexical scope.
    gpu_arena_trim_guard(gpu_arena_trim_guard&&) = delete;
    /// @brief Prevents move-assignment of a trim guard.
    gpu_arena_trim_guard& operator=(gpu_arena_trim_guard&&) = delete;
    /// @brief Rewinds and trims the arena when still active.
    ~gpu_arena_trim_guard() {
        (void)release_now();
    }

    /// @brief Rewinds and releases idle arena capacity at most once.
    /// @return `true` when the guard is inactive or capacity was released.
    [[nodiscard]] bool release_now() noexcept {
        if (arena_ == nullptr) {
            return true;
        }
        gpu_arena* arena = std::exchange(arena_, nullptr);
        arena->rewind(mark_);
        return arena->release_idle_capacity();
    }

  private:
    gpu_arena* arena_ = nullptr; ///< Arena trimmed on destruction, or null after `release_now`.
    gpu_arena_mark mark_{};      ///< Cursor snapshot restored before idle-capacity release.
};

} // namespace bioseq::gpu::vulkan
