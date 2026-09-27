/**
 * @file heap_allocation.cppm
 * @brief Estimates heap allocation  size before allocation and measures it afterwards.
 */
module;

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__linux__)
#include <features.h>
#if defined(__GLIBC__)
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <malloc.h>
#include <unistd.h>
#endif
#endif

export module bioseq.common.heap_allocation;

import std;
import bioseq.common.checked_arithmetic;

export namespace bioseq::common::heap_allocation_detail {

struct heap_allocation_plan {
    /// Bytes passed to malloc.
    std::size_t allocator_request_bytes = 0u;
    /// Upper bound reserved for the allocator-reported size before calling malloc.
    std::size_t allocation_requirement_bytes = 0u;
};

/**
 * Heap size estimates depend on the platform allocator. Darwin uses
 * malloc_good_size and allows for a cached large block below twice that
 * request (Apple libmalloc-715.140.5, magazine_large.c:562). Supported glibc
 * LP64 builds account for allocator alignment and page rounding.
 *
 * The glibc path rejects replacement allocators because these bounds assume
 * its native allodator. Builds with other runtimes are unsupported.
 */
#if defined(__GLIBC__) && defined(__LP64__) && \
    (defined(__x86_64__) || defined(__aarch64__))
[[nodiscard]] inline const void* shared_object_base(const void* symbol) noexcept {
    Dl_info info{};
    return symbol != nullptr && ::dladdr(symbol, std::addressof(info)) != 0
               ? info.dli_fbase
               : nullptr;
}

/** Return true only when every heap primitive resolves to the native glibc image. */
[[nodiscard]] inline bool has_supported_glibc_allocator() noexcept {
    const void* const libc_image = shared_object_base(
        reinterpret_cast<const void*>(&::gnu_get_libc_version));
    return libc_image != nullptr &&
           shared_object_base(reinterpret_cast<const void*>(&::malloc)) == libc_image &&
           shared_object_base(reinterpret_cast<const void*>(&::free)) == libc_image &&
           shared_object_base(reinterpret_cast<const void*>(&::malloc_usable_size)) ==
               libc_image;
}
#endif

[[nodiscard]] inline heap_allocation_plan plan_heap_allocation(std::size_t bytes) {
    if (bytes == 0u) {
        return {};
    }
#if defined(__APPLE__)
    const std::size_t allocation_bytes = ::malloc_good_size(bytes);
    if (allocation_bytes < bytes) {
        throw std::overflow_error("heap physical-allocation plan overflow");
    }
    const auto requirement = bioseq::common::checked_add(
        allocation_bytes, allocation_bytes - 1u);
    if (!requirement) {
        throw std::overflow_error("heap physical-allocation plan overflow");
    }
    return {
        .allocator_request_bytes = allocation_bytes,
        .allocation_requirement_bytes = *requirement,
    };
#elif defined(__GLIBC__) && defined(__LP64__) && \
    (defined(__x86_64__) || defined(__aarch64__))
    // Cache loader lookups and the host page  size. They stay fixed for
    // this process, so each allocation can reuse the result
    static const bool supported_allocator = has_supported_glibc_allocator();
    if (!supported_allocator) {
        throw std::runtime_error(
            "packed physical allocation requires the native glibc allocator");
    }
    static const long reported_page_size = ::sysconf(_SC_PAGESIZE);
    if (reported_page_size <= 0) {
        throw std::runtime_error("glibc allocator page size is unavailable");
    }
    const std::size_t page_size = static_cast<std::size_t>(reported_page_size);
    if ((page_size & (page_size - 1u)) != 0u) {
        throw std::runtime_error("glibc allocator page size is invalid");
    }
    static_assert(sizeof(void*) == 8u && sizeof(std::size_t) == 8u);
    static_assert(alignof(long double) <= 2u * sizeof(std::size_t));
    // The huge-page size is private to glibc, so use a bound based on the
    // request. Reserve room for allocator padding and page rounding, then
    // account for the measured size after allocation.
    constexpr std::size_t minimum_chunk_bytes = 4u * sizeof(std::size_t);
    const auto huge_page_bound = bioseq::common::checked_add(bytes, minimum_chunk_bytes);
    const auto mmap_overhead = huge_page_bound
        ? bioseq::common::checked_add(
              2u * sizeof(std::size_t), std::max(page_size, *huge_page_bound) - 1u)
        : std::optional<std::size_t>{};
    const auto requirement = mmap_overhead
                                 ? bioseq::common::checked_add(
                                       bytes, std::max(minimum_chunk_bytes, *mmap_overhead))
                                 : std::optional<std::size_t>{};
    if (!requirement) {
        throw std::overflow_error("heap physical-allocation plan overflow");
    }
    return {
        .allocator_request_bytes = bytes,
        .allocation_requirement_bytes = *requirement,
    };
#else
    throw std::runtime_error(
        "allocator-reported physical extent is unsupported on this platform");
#endif
}

/** Estimates space for the payload, optional prefix and alignment padding. */
[[nodiscard]] inline heap_allocation_plan plan_aligned_heap_allocation(
    std::size_t bytes,
    std::size_t alignment,
    std::size_t prefix_bytes = 0u) {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        throw std::invalid_argument("heap allocation alignment is invalid");
    }
    if (bytes == 0u) {
        return {};
    }
    const auto prefix_and_padding = bioseq::common::checked_add(
        prefix_bytes, alignment - 1u);
    const auto request = prefix_and_padding
        ? bioseq::common::checked_add(bytes, *prefix_and_padding)
        : std::optional<std::size_t>{};
    if (!request) {
        throw std::overflow_error("aligned heap allocation request overflow");
    }
    return plan_heap_allocation(*request);
}

/** Finds an aligned payload inside an allocation, or returns nullptr if it will not fit. */
[[nodiscard]] inline void* aligned_heap_payload(
    void* allocation,
    std::size_t allocation_bytes,
    std::size_t bytes,
    std::size_t alignment,
    std::size_t prefix_bytes = 0u) noexcept {
    if (allocation == nullptr || alignment == 0u ||
        (alignment & (alignment - 1u)) != 0u ||
        prefix_bytes > allocation_bytes) {
        return nullptr;
    }
    void* payload = static_cast<std::byte*>(allocation) + prefix_bytes;
    std::size_t available_bytes = allocation_bytes - prefix_bytes;
    return std::align(alignment, bytes, payload, available_bytes);
}

/** Returns the  size reported by the allocator for this allocation. */
[[nodiscard]] inline std::size_t allocator_reported_bytes(void* allocation) noexcept {
    if (allocation == nullptr) {
        return 0u;
    }
#if defined(__APPLE__)
    return ::malloc_size(allocation);
#elif defined(__GLIBC__) && defined(__LP64__) && \
    (defined(__x86_64__) || defined(__aarch64__))
    return ::malloc_usable_size(allocation);
#else
    return 0u;
#endif
}

/**
 * Allocates storage and checks its reported size against the plan.
 * Returns nullptr on failure and sets allocation_bytes to zero. The payload
 * is left uninitialized.
 */
[[nodiscard]] inline void* allocate_measured(const heap_allocation_plan& plan,
                                            std::size_t& allocation_bytes) noexcept {
    allocation_bytes = 0u;
    if (plan.allocator_request_bytes == 0u ||
        plan.allocation_requirement_bytes < plan.allocator_request_bytes) {
        return nullptr;
    }
    void* allocation = std::malloc(plan.allocator_request_bytes);
    if (allocation == nullptr) {
        return nullptr;
    }
    const std::size_t reported = allocator_reported_bytes(allocation);
    if (reported < plan.allocator_request_bytes ||
        reported > plan.allocation_requirement_bytes) {
        std::free(allocation);
        return nullptr;
    }
    allocation_bytes = reported;
    return allocation;
}

/** Allocates storage, measures it and zeroes every byte reported by the allocator. */
[[nodiscard]] inline void* allocate_and_touch(const heap_allocation_plan& plan,
                                              std::size_t& allocation_bytes) noexcept {
    void* allocation = allocate_measured(plan, allocation_bytes);
    if (allocation != nullptr) {
        std::memset(allocation, 0, allocation_bytes);
    }
    return allocation;
}

} // namespace bioseq::common::heap_allocation_detail
