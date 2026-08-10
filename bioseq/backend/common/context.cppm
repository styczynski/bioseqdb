/**
 * @file context.cppm
 * @brief Owns reusable memory arenas and their thread-local installation scope.
 *
 * A `context::context` separates allocations by lifetime: scratch storage is
 * short lived, chain storage lasts through alignment, alignment storage is
 * reclaimed around individual alignment calls, and pooled result/hash storage
 * reduces allocator fragmentation across repeated work. Oversize stack
 * allocations use tracked aligned heap blocks with the same scope lifetime as
 * ordinary arena allocations.
 */
module;

#include <foonathan/memory/aligned_allocator.hpp>
#include <foonathan/memory/allocator_storage.hpp>
#include <foonathan/memory/allocator_traits.hpp>
#include <foonathan/memory/container.hpp>
#include <foonathan/memory/memory_pool.hpp>
#include <foonathan/memory/memory_pool_collection.hpp>
#include <foonathan/memory/memory_stack.hpp>
#include <foonathan/memory/namespace_alias.hpp>
#include <foonathan/memory/std_allocator.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

export module bioseq.common.context;

import bioseq.common.logging;

using namespace memory::literals;

export namespace context {

// =====================================================================
// Internal allocator aliases. Hidden behind `detail::` so the concrete
// foonathan types can be swapped without touching call sites.
// =====================================================================
namespace detail {

/** @brief Stack allocator backing scratch-lifetime storage. */
using scratch_arena_type = memory::memory_stack<>;

/** @brief Stack allocator backing chain-lifetime storage. */
using chain_arena_type = memory::memory_stack<>;

/** @brief Stack allocator backing alignment-lifetime storage. */
using alignment_arena_type = memory::memory_stack<>;

/** @brief Bucketed pool allocator backing surviving results. */
using result_arena_type = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets>;

/** @brief Bucketed pool allocator backing hash-map nodes. */
using hash_arena_type = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets>;

} // namespace detail

/** @brief Identifies a stack arena that can be unwound as a scope. */
enum class arena : std::uint8_t {
    scratch, ///< Short-lived working storage.
    chain, ///< Storage retained through chaining and alignment.
    alignment ///< Storage scoped to alignment work.
};

// =====================================================================
// STL container aliases. These ARE part of the public surface.
// =====================================================================

/**
 * @brief Vector whose elements have scratch-arena lifetime.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using scratch_vector = memory::vector<p_value_type, detail::scratch_arena_type>;

/**
 * @brief Vector whose elements have chain-arena lifetime.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using chain_vector = memory::vector<p_value_type, detail::chain_arena_type>;

/**
 * @brief Vector whose elements have alignment-arena lifetime.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using align_vector = memory::vector<p_value_type, detail::alignment_arena_type>;

/**
 * @brief Vector whose elements have result-pool lifetime.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using result_vector = memory::vector<p_value_type, detail::result_arena_type>;

/**
 * @brief Hash map whose nodes have context-owned hash-pool lifetime.
 * @tparam p_key_type Lookup key type.
 * @tparam p_value_type Mapped value type.
 */
template <class p_key_type, class p_value_type>
using scratch_unordered_map =
    memory::unordered_map<p_key_type, p_value_type, detail::hash_arena_type>;

// =====================================================================
// context — the per-query allocator owner.
// =====================================================================

/**
 * @brief Owns allocator arenas used by one pipeline context.
 *
 * The owner is neither copyable nor movable because arena-backed containers
 * retain allocator identity. One instance may be installed per thread with
 * `installed_context_scope`; callers must not share it concurrently.
 *
 * @code{.cpp}
 * context::context memory;
 * context::installed_context_scope installation(memory);
 * auto scratch_scope = memory.push<context::arena::scratch>();
 * auto values = memory.make_scratch_vector<std::uint32_t>(128);
 * @endcode
 */
class context {
public:
    /** @brief Initial block sizes and pool node limits for every owned arena. */
    struct arena_configuration {
        /** @brief Initial scratch stack block size in bytes. */
        std::size_t scratch_block = 256_KiB;

        /** @brief Initial alignment stack block size in bytes. */
        std::size_t align_block = 4_MiB;

        /** @brief Initial chain stack block size in bytes. */
        std::size_t chain_block = 1_MiB;

        /** @brief Largest node size served by the hash pool. */
        std::size_t hash_node_max = 256;

        /** @brief Initial hash pool block size in bytes. */
        std::size_t hash_block = 1_MiB;

        /** @brief Largest node size served by the result pool. */
        std::size_t result_node_max = 256;

        /** @brief Initial result pool block size in bytes. */
        std::size_t result_block = 64_KiB;
    };

    /** @brief Constructs arenas with the default block-size budget. */
    context() : context(arena_configuration{}) {}

    /**
     * @brief Constructs arenas with an explicit block-size budget.
     * @param[in] configuration Initial arena block sizes and pool node limits.
     */
    explicit context(const arena_configuration& configuration)
        : scratch_arena_(configuration.scratch_block)
        , chain_arena_(configuration.chain_block)
        , align_arena_(configuration.align_block)
        , result_arena_(configuration.result_node_max, configuration.result_block)
        , hash_arena_(configuration.hash_node_max, configuration.hash_block)
    {}

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;

    /**
     * @brief Accesses the scratch arena for allocator construction.
     * @return Mutable scratch arena owned by this context.
     */
    [[nodiscard]] detail::scratch_arena_type& scratch_arena() noexcept {
        return scratch_arena_;
    }

    /**
     * @brief Accesses the chain arena for allocator construction.
     * @return Mutable chain arena owned by this context.
     */
    [[nodiscard]] detail::chain_arena_type& chain_arena() noexcept { return chain_arena_; }

    /**
     * @brief Accesses the alignment arena for allocator construction.
     * @return Mutable alignment arena owned by this context.
     */
    [[nodiscard]] detail::alignment_arena_type& align_arena() noexcept {
        return align_arena_;
    }

    /**
     * @brief Accesses the result pool without changing context identity.
     * @return Mutable result pool owned by this context.
     */
    [[nodiscard]] detail::result_arena_type& result_arena() const noexcept {
        return result_arena_;
    }

    /**
     * @brief Accesses the hash pool for allocator construction.
     * @return Mutable hash pool owned by this context.
     */
    [[nodiscard]] detail::hash_arena_type& hash_arena() noexcept { return hash_arena_; }

    /**
     * @brief Creates an empty vector bound to the scratch arena.
     * @tparam p_value_type Stored element type.
     * @param[in] reserve_hint Optional initial element capacity.
     * @return Scratch vector owned by this context.
     */
    template <class p_value_type>
    [[nodiscard]] scratch_vector<p_value_type>
    make_scratch_vector(std::size_t reserve_hint = 0) {
        return make_arena_vector<scratch_vector<p_value_type>>(scratch_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty vector bound to the chain arena.
     * @tparam p_value_type Stored element type.
     * @param[in] reserve_hint Optional initial element capacity.
     * @return Chain vector owned by this context.
     */
    template <class p_value_type>
    [[nodiscard]] chain_vector<p_value_type> make_chain_vector(std::size_t reserve_hint = 0) {
        return make_arena_vector<chain_vector<p_value_type>>(chain_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty vector bound to the alignment arena.
     * @tparam p_value_type Stored element type.
     * @param[in] reserve_hint Optional initial element capacity.
     * @return Alignment vector owned by this context.
     */
    template <class p_value_type>
    [[nodiscard]] align_vector<p_value_type> make_align_vector(std::size_t reserve_hint = 0) {
        return make_arena_vector<align_vector<p_value_type>>(align_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty vector bound to the result pool.
     * @tparam p_value_type Stored element type.
     * @param[in] reserve_hint Optional initial element capacity.
     * @return Result vector owned by this context.
     */
    template <class p_value_type>
    [[nodiscard]] result_vector<p_value_type>
    make_result_vector(std::size_t reserve_hint = 0) const {
        return make_arena_vector<result_vector<p_value_type>>(result_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty hash map bound to the context hash pool.
     * @tparam p_key_type Lookup key type.
     * @tparam p_value_type Mapped value type.
     * @param[in] bucket_hint Initial bucket count, or zero for the container default.
     * @return Hash map owned by this context.
     */
    template <class p_key_type, class p_value_type>
    [[nodiscard]] scratch_unordered_map<p_key_type, p_value_type>
    make_scratch_unordered_map(std::size_t bucket_hint = 0) {
        scratch_unordered_map<p_key_type, p_value_type> arena_map(
            bucket_hint,
            std::hash<p_key_type>{},
            std::equal_to<p_key_type>{},
            hash_arena_);
        return arena_map;
    }

    /**
     * @brief Allocates an uninitialized aligned array from alignment-lifetime storage.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested byte alignment, clamped to the element alignment.
     * @param[in] count Number of elements to allocate.
     * @return Span owned by the arena until the alignment scope unwinds.
     */
    template <class p_value_type, std::size_t p_alignment = alignof(p_value_type)>
    [[nodiscard]] std::span<p_value_type> alloc_align_array(std::size_t count) {
        return allocate_array<p_value_type, p_alignment>(
            align_arena_, large_align_blocks_, count);
    }

    /**
     * @brief Allocates an uninitialized aligned array from chain-lifetime storage.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested byte alignment, clamped to the element alignment.
     * @param[in] count Number of elements to allocate.
     * @return Span owned by the arena until the chain scope unwinds.
     */
    template <class p_value_type, std::size_t p_alignment = alignof(p_value_type)>
    [[nodiscard]] std::span<p_value_type> alloc_chain_array(std::size_t count) {
        return allocate_array<p_value_type, p_alignment>(
            chain_arena_, large_chain_blocks_, count);
    }

    /**
     * @brief Allocates an uninitialized aligned array from scratch-lifetime storage.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested byte alignment, clamped to the element alignment.
     * @param[in] count Number of elements to allocate.
     * @return Span owned by the arena until the scratch scope unwinds.
     */
    template <class p_value_type, std::size_t p_alignment = alignof(p_value_type)>
    [[nodiscard]] std::span<p_value_type> alloc_scratch_array(std::size_t count) {
        return allocate_array<p_value_type, p_alignment>(
            scratch_arena_, large_scratch_blocks_, count);
    }

    /**
     * @brief Restores one stack arena and its oversize allocations on destruction.
     * @tparam p_arena arena category selected for the scope lifetime.
     */
    template <arena p_arena>
    class scope {
    public:
        /**
         * @brief Records the current arena and heap-tracker positions.
         * @param[in] owner Context whose selected arena is scoped.
         */
        explicit scope(context& owner) noexcept
            : owner_(&owner)
            , arena_marker_(select_arena(owner).top())
            , heap_marker_(select_heap_blocks(owner).size())
        {}

        scope(const scope&)            = delete;
        scope& operator=(const scope&) = delete;
        scope(scope&&)                 = delete;
        scope& operator=(scope&&)      = delete;

        /** @brief Unwinds arena and oversize allocations created inside the scope. */
        ~scope() noexcept {
            select_arena(*owner_).unwind(arena_marker_);
            auto& blocks = select_heap_blocks(*owner_);
            while (blocks.size() > heap_marker_) {
                std::free(blocks.back().address);
                blocks.pop_back();
            }
        }

    private:
        /**
         * @brief Selects the stack arena represented by the template argument.
         * @param[in] owner Context containing the arena.
         * @return Selected mutable arena.
         */
        static auto& select_arena(context& owner) noexcept {
            if constexpr (p_arena == arena::scratch) {
                return owner.scratch_arena_;
            } else if constexpr (p_arena == arena::chain) {
                return owner.chain_arena_;
            } else {
                return owner.align_arena_;
            }
        }

        /**
         * @brief Selects the oversize-allocation tracker paired with the arena.
         * @param[in] owner Context containing the tracker.
         * @return Selected mutable heap-block tracker.
         */
        static auto& select_heap_blocks(context& owner) noexcept {
            if constexpr (p_arena == arena::scratch) {
                return owner.large_scratch_blocks_;
            } else if constexpr (p_arena == arena::chain) {
                return owner.large_chain_blocks_;
            } else {
                return owner.large_align_blocks_;
            }
        }

        using marker_type = decltype(select_arena(std::declval<context&>()).top());
        context* owner_;
        marker_type arena_marker_;
        std::size_t heap_marker_;
    };

    /**
     * @brief Opens a nested lifetime scope for one stack arena.
     * @tparam p_arena arena category selected for unwinding.
     * @return Non-movable scope guard bound to this context.
     */
    template <arena p_arena>
    [[nodiscard]] scope<p_arena> push() noexcept {
        return scope<p_arena>(*this);
    }

    /**
     * @brief Resets pipeline stacks and releases all tracked oversize allocations.
     *
     * Stack arenas retain reusable backing capacity according to foonathan's
     * `shrink_to_fit` contract; aligned heap fallbacks are released immediately.
     */
    void reset_pipeline() noexcept {
        scratch_arena_.shrink_to_fit();
        chain_arena_.shrink_to_fit();
        align_arena_.shrink_to_fit();
        free_heap_blocks(large_scratch_blocks_);
        free_heap_blocks(large_chain_blocks_);
        free_heap_blocks(large_align_blocks_);
    }

    /**
     * @brief Reports remaining capacity in the scratch stack's active block.
     * @return Remaining scratch bytes exposed by the allocator.
     */
    [[nodiscard]] std::size_t scratch_capacity() const noexcept {
        return scratch_arena_.capacity_left();
    }

    /**
     * @brief Reports remaining capacity in the chain stack's active block.
     * @return Remaining chain bytes exposed by the allocator.
     */
    [[nodiscard]] std::size_t chain_capacity() const noexcept {
        return chain_arena_.capacity_left();
    }

    /**
     * @brief Reports remaining capacity in the alignment stack's active block.
     * @return Remaining alignment bytes exposed by the allocator.
     */
    [[nodiscard]] std::size_t align_capacity() const noexcept {
        return align_arena_.capacity_left();
    }

    /**
     * @brief Sums tracked scratch allocations that bypassed the stack arena.
     * @return Allocated bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] std::size_t scratch_heap_fallback_bytes() const noexcept {
        return heap_fallback_bytes(large_scratch_blocks_);
    }

    /**
     * @brief Sums tracked chain allocations that bypassed the stack arena.
     * @return Allocated bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] std::size_t chain_heap_fallback_bytes() const noexcept {
        return heap_fallback_bytes(large_chain_blocks_);
    }

    /**
     * @brief Sums tracked alignment allocations that bypassed the stack arena.
     * @return Allocated bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] std::size_t align_heap_fallback_bytes() const noexcept {
        return heap_fallback_bytes(large_align_blocks_);
    }

    /** @brief Releases tracked oversize allocations before arena destruction. */
    ~context() {
        free_heap_blocks(large_align_blocks_);
        free_heap_blocks(large_chain_blocks_);
        free_heap_blocks(large_scratch_blocks_);
    }

private:
    /** @brief Largest allocation, in bytes, served directly by a stack arena. */
    static constexpr std::size_t k_heap_fallback_threshold = std::size_t{1} << 20;

    /** @brief Records one aligned allocation that bypassed a stack arena. */
    struct heap_block {
        /** @brief Address returned by `posix_memalign`. */
        void* address;

        /** @brief Allocation size in bytes. */
        std::size_t bytes;
    };

    /**
     * @brief Constructs and optionally reserves any arena-backed vector.
     * @tparam p_vector_type Concrete foonathan vector type.
     * @tparam p_arena_type Allocator arena accepted by the vector constructor.
     * @param[in] owner_arena arena category that owns the returned elements.
     * @param[in] reserve_hint Optional initial element capacity.
     * @return Empty vector bound to `owner_arena`.
     */
    template <class p_vector_type, class p_arena_type>
    [[nodiscard]] static p_vector_type make_arena_vector(
        p_arena_type& owner_arena,
        std::size_t reserve_hint) {
        p_vector_type arena_vector(owner_arena);
        if (reserve_hint != 0) {
            arena_vector.reserve(reserve_hint);
        }
        return arena_vector;
    }

    /**
     * @brief Allocates an aligned array from a stack arena or its heap fallback.
     *
     * Direct stack allocation intentionally bypasses allocator traits because
     * scope unwinding, rather than node deallocation, owns these raw arrays.
     * Oversize allocations are tracked so a nested scope releases only its tail.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested byte alignment.
     * @tparam p_arena_type Stack arena type.
     * @param[in] owner_arena arena category used below the allocation threshold.
     * @param[in,out] heap_tracker Oversize blocks paired with `owner_arena`.
     * @param[in] count Number of elements to allocate.
     * @return Span over uninitialized context-owned storage.
     * @throws std::bad_array_new_length When `count * sizeof(p_value_type)` overflows.
     */
    template <class p_value_type, std::size_t p_alignment, class p_arena_type>
    [[nodiscard]] std::span<p_value_type> allocate_array(
        p_arena_type& owner_arena,
        std::vector<heap_block>& heap_tracker,
        std::size_t count) {
        static constexpr std::size_t requested_alignment =
            p_alignment < alignof(p_value_type) ? alignof(p_value_type) : p_alignment;
        if (count == 0) {
            return {};
        }
        if (count > (std::numeric_limits<std::size_t>::max)() / sizeof(p_value_type)) {
            throw std::bad_array_new_length{};
        }
        const std::size_t bytes = count * sizeof(p_value_type);
        if (bytes <= k_heap_fallback_threshold) {
            void* allocation = owner_arena.allocate(bytes, requested_alignment);
            return std::span<p_value_type>(static_cast<p_value_type*>(allocation), count);
        }

        constexpr std::size_t minimum_alignment = sizeof(void*);
        constexpr std::size_t effective_alignment = requested_alignment < minimum_alignment
            ? minimum_alignment
            : requested_alignment;
        void* allocation = nullptr;
        const int allocation_status =
            ::posix_memalign(&allocation, effective_alignment, bytes);
        if (allocation_status != 0 || allocation == nullptr) {
            bioseq_log<logging_level::debug>(
                "[context] heap fallback FAILED: bytes={} align={} rc={}",
                bytes,
                effective_alignment,
                allocation_status);
            std::abort();
        }
        try {
            heap_tracker.push_back({allocation, bytes});
        } catch (...) {
            std::free(allocation);
            throw;
        }
        return std::span<p_value_type>(static_cast<p_value_type*>(allocation), count);
    }

    /**
     * @brief Releases every aligned block in a tracker.
     * @param[in,out] blocks Tracker cleared after all addresses are freed.
     */
    static void free_heap_blocks(std::vector<heap_block>& blocks) noexcept {
        for (auto& block : blocks) {
            std::free(block.address);
        }
        blocks.clear();
    }

    /**
     * @brief Sums aligned allocation sizes without unsigned overflow.
     * @param[in] blocks Tracked allocations to measure.
     * @return Total tracked bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] static std::size_t heap_fallback_bytes(
        const std::vector<heap_block>& blocks) noexcept {
        std::size_t total = 0;
        for (const auto& block : blocks) {
            if (block.bytes > (std::numeric_limits<std::size_t>::max)() - total) {
                return (std::numeric_limits<std::size_t>::max)();
            }
            total += block.bytes;
        }
        return total;
    }

    detail::scratch_arena_type scratch_arena_; ///< Scratch-lifetime stack storage.
    detail::chain_arena_type chain_arena_; ///< Chain-lifetime stack storage.
    detail::alignment_arena_type align_arena_; ///< Alignment-lifetime stack storage.
    mutable detail::result_arena_type result_arena_; ///< Pooled result storage.
    detail::hash_arena_type hash_arena_; ///< Pooled hash-node storage.

    // These sparse trackers deliberately use the system allocator: placing
    // their bookkeeping in a scoped arena would make tracker lifetime depend
    // on the allocation lifetime it controls.
    std::vector<heap_block> large_align_blocks_; ///< Oversize alignment allocations.
    std::vector<heap_block> large_chain_blocks_; ///< Oversize chain allocations.
    std::vector<heap_block> large_scratch_blocks_; ///< Oversize scratch allocations.
};

namespace detail {
inline thread_local context* tls_current = nullptr;
} // namespace detail

/**
 * @brief Retrieves the context installed on the calling thread when present.
 * @return Installed context pointer, or `nullptr` when the thread has no context.
 */
[[nodiscard]] inline context* current_or_null() noexcept {
    return detail::tls_current;
}

/**
 * @brief Retrieves the context installed on the calling thread.
 * @return Installed context reference.
 * @warning Aborts when no context is installed.
 */
[[nodiscard]] inline context& current() noexcept {
    context* installed_context = detail::tls_current;
    if (installed_context == nullptr) {
        bioseq_log<logging_level::debug>(
            "[context] FATAL: no context installed; aborting");
        std::abort();
    }
    return *installed_context;
}

/**
 * @brief Installs a context for the current thread and restores the prior one.
 *
 * Scopes may nest; destruction restores the pointer observed by the constructor.
 * The guard is thread-affine and must not outlive the referenced context.
 */
class installed_context_scope {
public:
    /**
     * @brief Installs a context until this guard is destroyed.
     * @param[in] installed_context Context that must outlive the guard.
     */
    explicit installed_context_scope(context& installed_context) noexcept
        : previous_context_(detail::tls_current) {
        detail::tls_current = &installed_context;
    }

    installed_context_scope(const installed_context_scope&)            = delete;
    installed_context_scope& operator=(const installed_context_scope&) = delete;
    installed_context_scope(installed_context_scope&&)                 = delete;
    installed_context_scope& operator=(installed_context_scope&&)      = delete;

    /** @brief Restores the context that was installed before this guard. */
    ~installed_context_scope() noexcept { detail::tls_current = previous_context_; }

private:
    context* previous_context_;
};

} // namespace context
