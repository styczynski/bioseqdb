/**
 * @file arena.cppm
 * @brief Reusable GPU buffer slices and cleanup after a request.
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

/// Maximum descriptor sets available to one frontend submission.
inline constexpr std::uint32_t k_gpu_dispatch_descriptor_set_capacity = 4096u;
/// Maximum storage-buffer descriptors available in that pool.
inline constexpr std::uint32_t k_gpu_dispatch_storage_descriptor_capacity =
    k_gpu_dispatch_descriptor_set_capacity * 9u;

/**
 * @brief Returns how many jobs fit in the descriptor pool.
 * @param[in] descriptor_sets_per_job Descriptor sets needed by one job.
 * @param[in] storage_descriptors_per_job Storage-buffer descriptors needed by one job.
 * @return The smaller capacity, or zero if either per-job count is zero.
 */
[[nodiscard]] inline constexpr std::uint32_t gpu_dispatch_job_capacity(
    std::uint32_t descriptor_sets_per_job,
    std::uint32_t storage_descriptors_per_job) noexcept {
    if (descriptor_sets_per_job == 0u || storage_descriptors_per_job == 0u) {
        return 0u;
    }
    return std::min(
        k_gpu_dispatch_descriptor_set_capacity / descriptor_sets_per_job,
        k_gpu_dispatch_storage_descriptor_capacity / storage_descriptors_per_job);
}

// Runtime-owned GPU arena

/**
 * @brief A byte range within an arena buffer.
 *
 * Each descriptor binds this range, so several shader inputs can share one allocation.
 */
struct gpu_arena_slice {
    buffer* buffer = nullptr;       ///< Arena backing buffer, or null when invalid.
    std::uint64_t offset = 0;       ///< Byte offset from the buffer base.
    std::uint64_t size = 0;         ///< Number of bytes in this slice.

    /// @brief Reports whether the slice refers to an arena buffer.
    /// @return `true` when the buffer pointer is non-null.
    [[nodiscard]] bool is_valid() const noexcept {
        return buffer != nullptr;
    }
};

/// @brief A snapshot of the three arena cursors. `rewind(mark)` restores all three.
struct gpu_arena_mark {
    std::uint64_t device_cursor = 0;   ///< Next free byte in the device buffer when marked.
    std::uint64_t upload_cursor = 0;    ///< Next free byte in the upload buffer when marked.
    std::uint64_t download_cursor = 0;  ///< Next free byte in the download buffer when marked.
};

/**
 * @brief Reuses three mapped buffers for device work, uploads, and downloads.
 *
 * Slices share the buffers and consume space from the current cursors. Rewinding
 * allows that space to be reused. Grow the buffers before an operation starts,
 * while no live slice depends on their addresses.
 */
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

    /**
     * @brief Prepares enough buffer space for the count and execution submissions.
     *
     * Call this before creating slices. Growing a buffer invalidates its old slices.
     * A failure may leave some buffers resized.
     * @param[in] device_bytes Required device-buffer capacity.
     * @param[in] upload_bytes Required upload-buffer capacity.
     * @param[in] download_bytes Required download-buffer capacity.
     * @return True when all three capacities are sufficient.
     */
    [[nodiscard]] bool ensure_capacity(std::uint64_t device_bytes,
                                       std::uint64_t upload_bytes,
                                       std::uint64_t download_bytes);

    /**
     * @brief Reserves memory budget and prepares the requested arena buffers.
     *
     * The reservation covers the allocation size reported by Vulkan. An existing
     * admitted buffer stays alive until its replacement has been allocated and fully
     * touched. Later slices use this reservation without charging the same bytes again.
     * @return True when all requested buffers have enough admitted capacity.
     */
    [[nodiscard]] bool ensure_admitted_capacity(std::uint64_t device_bytes,
                                                std::uint64_t upload_bytes,
                                                std::uint64_t download_bytes);

    /// @brief Reserves an aligned slice at the current device-buffer cursor.
    /// @param[in] bytes Slice size in bytes.
    /// @param[in] alignment Required byte alignment.
    /// @return The reserved slice, or an invalid slice if it does not fit.
    [[nodiscard]] gpu_arena_slice allocate_device(std::uint64_t bytes, std::uint64_t alignment);
    /// @brief Reserves an aligned slice at the current upload-buffer cursor.
    /// @param[in] bytes Slice size in bytes.
    /// @param[in] alignment Required byte alignment.
    /// @return The reserved slice, or an invalid slice if it does not fit.
    [[nodiscard]] gpu_arena_slice allocate_upload(std::uint64_t bytes, std::uint64_t alignment);
    /// @brief Reserves an aligned slice at the current download-buffer cursor.
    /// @param[in] bytes Slice size in bytes.
    /// @param[in] alignment Required byte alignment.
    /// @return The reserved slice, or an invalid slice if it does not fit.
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

    /// @brief Returns the mutable buffer used for GPU work.
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

    /**
     * @brief Frees the buffers when every allocation cursor is zero.
     *
     * Use this after a request has copied its results out of the arena. It frees the
     * allocated capacity between requests. Target-index buffers adopted as separate
     * allocations remain valid.
     * @return True if the arena was idle and its buffers were released.
     */
    [[nodiscard]] bool release_idle_capacity() noexcept;

  private:
    /// @brief Rounds a byte offset up to the requested alignment.
    /// @return The input for alignment zero or one. Otherwise, the aligned offset,
    /// or UINT64_MAX for overflow or alignment that is not a power of two.
    [[nodiscard]] static std::uint64_t align_up(std::uint64_t value,
                                                std::uint64_t alignment) noexcept {
        if (alignment <= 1u)
            return value;
        return bioseq::common::checked_align_up(value, alignment)
            .value_or((std::numeric_limits<std::uint64_t>::max)());
    }

    runtime* runtime_ = nullptr;             ///< Owning runtime that allocates backing buffers.
    // Every permit precedes its allocation so reverse destruction frees storage first.
    bioseq::parallelism::memory_permit device_permit_{};
    buffer device_;                          ///< Arena buffer used for GPU work.
    bioseq::parallelism::memory_permit upload_permit_{};
    buffer upload_;                          ///< Host-visible upload arena backing buffer.
    bioseq::parallelism::memory_permit download_permit_{};
    buffer download_;                        ///< Host-visible download arena backing buffer.
    std::uint64_t device_cursor_ = 0;       ///< Next free byte in the device buffer.
    std::uint64_t upload_cursor_ = 0;       ///< Next free byte in the upload buffer.
    std::uint64_t download_cursor_ = 0;     ///< Next free byte in the download buffer.
    std::uint64_t device_capacity_ = 0;     ///< Device-buffer capacity in bytes.
    std::uint64_t upload_capacity_ = 0;     ///< Reserved upload capacity in bytes.
    std::uint64_t download_capacity_ = 0;   ///< Reserved download capacity in bytes.
    bool device_admission_established_ = false;   ///< Device allocation has passed memory admission.
    bool upload_admission_established_ = false;   ///< Upload allocation has passed memory admission.
    bool download_admission_established_ = false; ///< Download allocation has passed memory admission.
};

/**
 * @brief Holds a memory permit while a count and execution submission use the arena.
 *
 * Destruction rewinds the arena to the saved mark, then releases the permit.
 */
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
    /// @brief Rewinds the arena once. The memory permit remains held by this object.
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
    bioseq::parallelism::memory_permit permit_;          ///< Memory permit held until destruction or move assignment.
    gpu_arena_mark mark_{};                            ///< Cursor snapshot restored on rewind.
};

/**
 * @brief Rewinds and frees an idle arena when a request leaves its scope.
 *
 * Use this when the result owns its storage outside the arena. Rewinding alone
 * keeps the buffers allocated. The guard can be created before buffer growth,
 * so early returns and exceptions also release idle capacity.
 */
class gpu_arena_trim_guard {
  public:
    /// @brief Saves the arena cursors for cleanup on destruction.
    explicit gpu_arena_trim_guard(gpu_arena& arena) noexcept
        : arena_(&arena), mark_(arena.mark()) {}
    /// @brief Prevents copying a trim guard.
    gpu_arena_trim_guard(const gpu_arena_trim_guard&) = delete;
    /// @brief Prevents copy-assignment of a trim guard.
    gpu_arena_trim_guard& operator=(const gpu_arena_trim_guard&) = delete;
    /// @brief Prevents moving cleanup out of this scope.
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
