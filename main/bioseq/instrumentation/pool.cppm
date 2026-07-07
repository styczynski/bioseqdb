/**
 * @file instrumentation/pool.cppm
 * @brief Bump allocator backed by 64 KiB chunks (Part 5 / Part 19 of the
 *        design doc).
 *
 * The pool is a per-thread cold-path allocator used for descriptor
 * names, aggregate blocks, and any other profiler-private storage that
 * must outlive a single region. The hot path never touches the pool.
 */
module;

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

export module bioseq.instr.pool;

export namespace bioseq::instr {

class Pool {
public:
    static constexpr std::size_t kChunkBytes = 64 * 1024;

    Pool() = default;

    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    ~Pool() noexcept {
        for (auto* c : chunks_) {
            ::operator delete(c, std::align_val_t{kAlign});
        }
    }

    /// Allocate `bytes` aligned to at least `align`. Returns nullptr if
    /// `bytes` exceeds the chunk size (callers should split the request).
    [[nodiscard]] void* allocate(std::size_t bytes, std::size_t align) {
        if (align < kAlign) align = kAlign;
        if (bytes == 0) return nullptr;
        if (bytes > kChunkBytes) return nullptr;

        const std::size_t aligned_wmark =
            (write_mark_ + (align - 1)) & ~(align - 1);
        if (aligned_wmark + bytes > chunk_capacity_) {
            grow_();
            return allocate(bytes, align);
        }

        void* result = static_cast<std::byte*>(chunks_.back()) + aligned_wmark;
        write_mark_ = aligned_wmark + bytes;
        return result;
    }

    [[nodiscard]] std::size_t high_water_mark() const noexcept {
        return high_water_;
    }

private:
    static constexpr std::size_t kAlign = alignof(std::max_align_t);

    void grow_() {
        auto* fresh = ::operator new(kChunkBytes, std::align_val_t{kAlign});
        chunks_.push_back(fresh);
        write_mark_ = 0;
        chunk_capacity_ = kChunkBytes;
        high_water_ += kChunkBytes;
    }

    std::vector<void*> chunks_;
    std::size_t        write_mark_     = 0;
    std::size_t        chunk_capacity_ = 0;
    std::size_t        high_water_     = 0;
};

}  // namespace bioseq::instr
