/**
 * @file heap_allocation.cppm
 * @brief Source-derived plans and measured extents for native heap allocations.
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
    /// Maximum allocator-reported retained extent admitted before malloc.
    std::size_t allocation_requirement_bytes = 0u;
};

/**
 * Return the allocator request and a pre-admission bound for its reported
 * retained extent. Darwin uses malloc_good_size for the allocator request;
 * Apple libmalloc-715.140.5 magazine_large.c:562 permits a cached large extent
 * strictly below twice that request. The supported GNU/Linux x86-64 LP64 build
 * uses native glibc ptmalloc. Other C runtimes and replacement allocators expose
 * no matching pre-allocation retained-extent contract and are rejected.
 */
#if defined(__GLIBC__) && defined(__x86_64__) && defined(__LP64__)
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
#elif defined(__GLIBC__) && defined(__x86_64__) && defined(__LP64__)
    // Symbol bindings and the host base-page size are process invariants; do
    // not repeat loader lookup or sysconf for every admitted map node.
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
    // Native ptmalloc may use configured huge pages instead of the system
    // page. Its sysmalloc gate uses a huge page only when hp_pagesize <= nb.
    // request2size rounds bytes by less than MINSIZE on this LP64 ABI, so
    // bytes + MINSIZE bounds that otherwise private page size. Keep this
    // pre-allocation bound conservative, then shrink to malloc_usable_size.
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

/** Plan a payload, an optional prefix, and the worst-case alignment padding. */
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

/** Borrow the aligned payload inside a known allocation without reallocating. */
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

/** Return the retained extent reported by the allocator owning `allocation`. */
[[nodiscard]] inline std::size_t allocator_reported_bytes(void* allocation) noexcept {
    if (allocation == nullptr) {
        return 0u;
    }
#if defined(__APPLE__)
    return ::malloc_size(allocation);
#elif defined(__GLIBC__) && defined(__x86_64__) && defined(__LP64__)
    return ::malloc_usable_size(allocation);
#else
    return 0u;
#endif
}

/** Allocate and measure storage without constructing or touching its payload. */
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

/** Allocate and initialize the complete allocator-reported retained extent. */
[[nodiscard]] inline void* allocate_and_touch(const heap_allocation_plan& plan,
                                              std::size_t& allocation_bytes) noexcept {
    void* allocation = allocate_measured(plan, allocation_bytes);
    if (allocation != nullptr) {
        std::memset(allocation, 0, allocation_bytes);
    }
    return allocation;
}

} // namespace bioseq::common::heap_allocation_detail
