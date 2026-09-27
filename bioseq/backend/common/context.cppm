/**
 * @file context.cppm
 * @brief Memory arenas for pipeline work and their per-thread context.
 *
 * Scratch, chain and alignment arenas let callers free temporary allocations
 * as a group when a scope ends. Result and hash pools reuse storage across
 * operations. Arrays too large for a stack arena use separate heap blocks,
 * which are freed with the same scope.
 */
module;

#include <foonathan/memory/aligned_allocator.hpp>
#include <foonathan/memory/allocator_storage.hpp>
#include <foonathan/memory/allocator_traits.hpp>
#include <foonathan/memory/container.hpp>
#include <foonathan/memory/default_allocator.hpp>
#include <foonathan/memory/memory_pool.hpp>
#include <foonathan/memory/memory_pool_collection.hpp>
#include <foonathan/memory/memory_stack.hpp>
#include <foonathan/memory/namespace_alias.hpp>
#include <foonathan/memory/std_allocator.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.common.context;

import bioseq.common.logging;
import bioseq.common.heap_allocation;

using namespace memory::literals;

export namespace context {

/** @brief Owns an allocation hook's move-only reservation token. */
class backing_allocation_lease {
public:
    backing_allocation_lease() = default;
    backing_allocation_lease(const backing_allocation_lease&) = delete;
    backing_allocation_lease& operator=(const backing_allocation_lease&) = delete;

    backing_allocation_lease(backing_allocation_lease&& other) noexcept {
        move_from(other);
    }

    backing_allocation_lease& operator=(backing_allocation_lease&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    /** @brief Destroys the stored token, releasing its memory reservation. */
    ~backing_allocation_lease() noexcept { reset(); }

    /**
     * @brief Moves a token into fixed storage inside this object.
     *
     * The token must fit and its move constructor and destructor must not throw.
     * @param value Token to take ownership of.
     */
    template<class p_value_type>
    void emplace(p_value_type&& value) noexcept {
        using value_type = std::remove_cvref_t<p_value_type>;
        static_assert(sizeof(value_type) <= k_storage_bytes);
        static_assert(alignof(value_type) <= alignof(std::max_align_t));
        static_assert(std::is_nothrow_move_constructible_v<value_type>);
        static_assert(std::is_nothrow_destructible_v<value_type>);
        reset();
        std::construct_at(
            reinterpret_cast<value_type*>(storage_),
            std::forward<p_value_type>(value));
        destroy_ = [](void* storage) noexcept {
            std::destroy_at(static_cast<value_type*>(storage));
        };
        move_ = [](void* destination, void* source) noexcept {
            auto* source_value = static_cast<value_type*>(source);
            std::construct_at(
                static_cast<value_type*>(destination), std::move(*source_value));
            std::destroy_at(source_value);
        };
    }

    /** @brief Releases any stored token and makes this lease empty. */
    void reset() noexcept {
        if (destroy_ != nullptr) {
            destroy_(storage_);
            destroy_ = nullptr;
            move_ = nullptr;
        }
    }

    /** @return Whether a token is stored here. */
    [[nodiscard]] explicit operator bool() const noexcept {
        return destroy_ != nullptr;
    }

private:
    static constexpr std::size_t k_storage_bytes = 4u * sizeof(void*);
    using destroy_function = void (*)(void*) noexcept;
    using move_function = void (*)(void*, void*) noexcept;

    void move_from(backing_allocation_lease& other) noexcept {
        if (other.move_ == nullptr) {
            return;
        }
        other.move_(storage_, other.storage_);
        destroy_ = std::exchange(other.destroy_, nullptr);
        move_ = std::exchange(other.move_, nullptr);
    }

    alignas(std::max_align_t) std::byte storage_[k_storage_bytes]{};
    destroy_function destroy_ = nullptr;
    move_function move_ = nullptr;
};

/** @brief Reservations for a new allocation and optional replacement storage. */
struct backing_allocation_authority {
    backing_allocation_lease primary{};
    backing_allocation_lease replacement{};
};

/** @brief Adjusts reserved bytes while an allocation hook is running. */
struct backing_allocation_reconciliation {
    void* state = nullptr;
    bool (*reconcile)(void*, std::uint64_t) noexcept = nullptr;

    /** @return True if the reservation was adjusted to exactly bytes. */
    [[nodiscard]] bool reconcile_to(std::uint64_t bytes) const noexcept {
        return reconcile != nullptr && reconcile(state, bytes);
    }
};

/** @brief Measured allocation size and visibility, returned to the memory controller. */
struct backing_allocation_materialization {
    std::uint64_t actual_allocation_bytes = 0u;
    std::uint64_t fully_touched_bytes = 0u;
    std::uint64_t primary_owner_bytes = 0u;
    std::uint64_t replacement_owner_bytes = 0u;
    bool is_host_visible = false;
    bool is_host_coherent = false;
    bool is_process_footprint_reflected = false;
};

/** @brief Keeps reservations until context destruction or beside individual allocations. */
enum class backing_allocation_lifetime : std::uint8_t {
    context,
    adjacent,
};

using backing_allocation_materializer = backing_allocation_materialization (*)(
    void*, const backing_allocation_reconciliation&);
using backing_allocation_cleanup = void (*)(void*) noexcept;

using backing_allocation_transaction = backing_allocation_authority (*)(
    void*,
    std::uint64_t,
    backing_allocation_lifetime,
    void*,
    backing_allocation_materializer,
    backing_allocation_cleanup);

/**
 * @brief Calls the memory controller when a context needs a heap block.
 *
 * The hook borrows its state and function, which must outlive the context.
 * Leave both null to use the allocator without memory admission checks.
 */
struct backing_allocation_hook {
    void* state = nullptr;
    backing_allocation_transaction transaction = nullptr;

    /** @return Whether both hook fields are installed. */
    [[nodiscard]] constexpr bool is_installed() const noexcept {
        return state != nullptr && transaction != nullptr;
    }

    /** @return Whether both hook fields are null or both are set. */
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return (state == nullptr) == (transaction == nullptr);
    }

    /** @brief Runs an allocation and its cleanup through the installed hook. */
    [[nodiscard]] backing_allocation_authority materialize(
        std::uint64_t requested_bytes,
        backing_allocation_lifetime lifetime,
        void* operation_state,
        backing_allocation_materializer materializer,
        backing_allocation_cleanup cleanup) const {
        return transaction(
            state,
            requested_bytes,
            lifetime,
            operation_state,
            materializer,
            cleanup);
    }
};

// Internal allocator types used by the public context and container aliases.
namespace detail {

/**
 * @brief Supplies heap blocks to foonathan arenas under memory admission checks.
 *
 * The hook reserves space for each whole block, including alignment padding.
 * Allocations made inside an existing arena block need no separate reservation.
 * Without a hook, requests go directly to the default allocator.
 */
class context_block_allocator {
public:
    using is_stateful = std::true_type;

    context_block_allocator() = default;
    explicit context_block_allocator(backing_allocation_hook hook) noexcept
        : hook_(hook) {}

    [[nodiscard]] void* allocate_node(std::size_t bytes, std::size_t alignment) {
        if (bytes == 0) {
            return nullptr;
        }
        if (!hook_.is_installed()) {
            return raw_traits::allocate_node(raw_allocator_, bytes, alignment);
        }

        block_materialization_state state{
            .bytes = bytes,
            .alignment = alignment,
            .plan = bioseq::common::heap_allocation_detail::plan_aligned_heap_allocation(
                bytes, alignment, sizeof(void*)),
        };
        try {
            (void)hook_.materialize(
                static_cast<std::uint64_t>(state.plan.allocation_requirement_bytes),
                backing_allocation_lifetime::context,
                std::addressof(state),
                &materialize_block,
                &cleanup_block);
            return state.allocation;
        } catch (...) {
            cleanup_block(std::addressof(state));
            throw;
        }
    }

    void deallocate_node(
        void* allocation,
        std::size_t bytes,
        std::size_t alignment) noexcept {
        if (!hook_.is_installed()) {
            raw_traits::deallocate_node(raw_allocator_, allocation, bytes, alignment);
            return;
        }
        if (allocation != nullptr) {
            void* base = nullptr;
            std::memcpy(
                std::addressof(base),
                static_cast<std::byte*>(allocation) - sizeof(void*),
                sizeof(void*));
            std::free(base);
        }
    }

private:
    using raw_traits = memory::allocator_traits<memory::default_allocator>;

    struct block_materialization_state {
        std::size_t bytes = 0u;
        std::size_t alignment = 0u;
        bioseq::common::heap_allocation_detail::heap_allocation_plan plan{};
        void* base = nullptr;
        void* allocation = nullptr;
    };

    [[nodiscard]] static backing_allocation_materialization materialize_block(
        void* opaque_state,
        const backing_allocation_reconciliation& reconciliation) {
        auto& state = *static_cast<block_materialization_state*>(opaque_state);
        try {
            std::size_t allocation_bytes = 0u;
            state.base = bioseq::common::heap_allocation_detail::allocate_and_touch(
                state.plan, allocation_bytes);
            state.allocation = bioseq::common::heap_allocation_detail::aligned_heap_payload(
                state.base, allocation_bytes, state.bytes, state.alignment, sizeof(void*));
            if (state.allocation == nullptr) {
                throw std::bad_alloc{};
            }
            // Store the malloc base just before the aligned payload so freeing it
            // needs no separate pointer map. This prefix is included in the reservation.
            std::memcpy(
                static_cast<std::byte*>(state.allocation) - sizeof(void*),
                std::addressof(state.base),
                sizeof(void*));
            const std::uint64_t actual_bytes = static_cast<std::uint64_t>(allocation_bytes);
            if (!reconciliation.reconcile_to(actual_bytes)) {
                throw std::bad_alloc{};
            }
            return {
                .actual_allocation_bytes = actual_bytes,
                .fully_touched_bytes = actual_bytes,
                .is_host_visible = true,
                .is_host_coherent = true,
                .is_process_footprint_reflected = true,
            };
        } catch (...) {
            cleanup_block(std::addressof(state));
            throw;
        }
    }

    /** @brief Frees an unfinished allocation before its reservation is released. */
    static void cleanup_block(void* opaque_state) noexcept {
        auto& state = *static_cast<block_materialization_state*>(opaque_state);
        std::free(std::exchange(state.base, nullptr));
        state.allocation = nullptr;
    }

    memory::default_allocator raw_allocator_{};
    backing_allocation_hook hook_{};
};

/** @brief Stack allocator for temporary scratch arrays. */
using scratch_arena_type = memory::memory_stack<context_block_allocator>;

/** @brief Stack allocator for data kept through chaining and alignment. */
using chain_arena_type = memory::memory_stack<context_block_allocator>;

/** @brief Stack allocator for temporary alignment data. */
using alignment_arena_type = memory::memory_stack<context_block_allocator>;

/** @brief Pool that reuses result allocations in size buckets. */
using result_arena_type = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets, context_block_allocator>;

/** @brief Pool that reuses hash-map node allocations in size buckets. */
using hash_arena_type = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets, context_block_allocator>;

} // namespace detail

/** @brief Selects the stack arena that a scope restores. */
enum class arena : std::uint8_t {
    scratch, ///< Short-lived working storage.
    chain, ///< Storage kept through chaining and alignment.
    alignment ///< Storage scoped to alignment work.
};

// Public container aliases using context-owned allocators.

/**
 * @brief Vector using a context's scratch arena.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using scratch_vector = memory::vector<p_value_type, detail::scratch_arena_type>;

/**
 * @brief Vector using a context's chain arena.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using chain_vector = memory::vector<p_value_type, detail::chain_arena_type>;

/**
 * @brief Vector using a context's alignment arena.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using align_vector = memory::vector<p_value_type, detail::alignment_arena_type>;

/**
 * @brief Vector using a context's result pool.
 * @tparam p_value_type Stored element type.
 */
template <class p_value_type>
using result_vector = memory::vector<p_value_type, detail::result_arena_type>;

/**
 * @brief Hash map using a context's hash pool.
 * @tparam p_key_type Lookup key type.
 * @tparam p_value_type Mapped value type.
 */
template <class p_key_type, class p_value_type>
using scratch_unordered_map =
    memory::unordered_map<p_key_type, p_value_type, detail::hash_arena_type>;

// Context ownership and allocation helpers.

/**
 * @brief Owns reusable memory for one pipeline worker.
 *
 * Containers borrow these allocators, so the context cannot move and must
 * outlive them. Use installed_context_scope to make it available to calls
 * on the current thread. Each thread needs its own context during concurrent
 * work.
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

    /** @brief Creates arenas with default initial sizes. */
    context() : context(arena_configuration{}, backing_allocation_hook{}) {}

    /**
     * @brief Creates arenas with selected initial sizes and pool node limits.
     * @param[in] configuration Sizes used when constructing the arenas.
     */
    explicit context(const arena_configuration& configuration)
        : context(configuration, backing_allocation_hook{})
    {}

    /**
     * @brief Installs the memory hook before allocating the arenas' first blocks.
     * @param[in] configuration Initial block sizes and pool node limits.
     * @param[in] hook Allocation hook whose state must outlive this context.
     */
    context(
        const arena_configuration& configuration,
        backing_allocation_hook hook)
        : backing_hook_(validate_backing_hook(hook))
        , scratch_arena_(
              configuration.scratch_block,
              detail::context_block_allocator{backing_hook_})
        , chain_arena_(
              configuration.chain_block,
              detail::context_block_allocator{backing_hook_})
        , align_arena_(
              configuration.align_block,
              detail::context_block_allocator{backing_hook_})
        , result_arena_(
              configuration.result_node_max,
              configuration.result_block,
              detail::context_block_allocator{backing_hook_})
        , hash_arena_(
              configuration.hash_node_max,
              configuration.hash_block,
              detail::context_block_allocator{backing_hook_})
    {}

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;

    /** @return Whether heap block allocations use a memory admission hook. */
    [[nodiscard]] bool has_backing_allocation_hook() const noexcept {
        return backing_hook_.is_installed();
    }

    /**
     * @brief Allocates replaceable heap storage through the memory hook.
     *
     * Keep each returned reservation beside the allocation it covers and free
     * the storage before releasing that reservation. Use this for separate heap
     * allocations. Arena allocations already use their arena's hook.
     *
     * @throws std::invalid_argument If the hook or required arguments are missing.
     */
    [[nodiscard]] backing_allocation_authority materialize_adjacent_allocation(
        std::uint64_t requested_bytes,
        void* operation_state,
        backing_allocation_materializer materializer,
        backing_allocation_cleanup cleanup) const {
        if (!backing_hook_.is_installed() || requested_bytes == 0u ||
            operation_state == nullptr || materializer == nullptr || cleanup == nullptr) {
            throw std::invalid_argument{
                "adjacent allocation requires a complete installed backing hook"};
        }
        return backing_hook_.materialize(
            requested_bytes,
            backing_allocation_lifetime::adjacent,
            operation_state,
            materializer,
            cleanup);
    }

    /**
     * @brief Returns the scratch arena for constructing an allocator.
     */
    [[nodiscard]] detail::scratch_arena_type& scratch_arena() noexcept {
        return scratch_arena_;
    }

    /**
     * @brief Returns the chain arena for constructing an allocator.
     */
    [[nodiscard]] detail::chain_arena_type& chain_arena() noexcept { return chain_arena_; }

    /**
     * @brief Returns the alignment arena for constructing an allocator.
     */
    [[nodiscard]] detail::alignment_arena_type& align_arena() noexcept {
        return align_arena_;
    }

    /**
     * @brief Returns the mutable result pool, including through a const context.
     */
    [[nodiscard]] detail::result_arena_type& result_arena() const noexcept {
        return result_arena_;
    }

    /**
     * @brief Returns the hash pool for constructing an allocator.
     */
    [[nodiscard]] detail::hash_arena_type& hash_arena() noexcept { return hash_arena_; }

    /**
     * @brief Creates an empty vector using the scratch arena.
     * @param[in] reserve_hint Optional number of elements to reserve.
     * @pre Destroy the vector before unwinding its arena scope.
     */
    template <class p_value_type>
    [[nodiscard]] scratch_vector<p_value_type>
    make_scratch_vector(std::size_t reserve_hint = 0) {
        return make_arena_vector<scratch_vector<p_value_type>>(scratch_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty vector using the chain arena.
     * @param[in] reserve_hint Optional number of elements to reserve.
     * @pre Destroy the vector before unwinding its arena scope.
     */
    template <class p_value_type>
    [[nodiscard]] chain_vector<p_value_type> make_chain_vector(std::size_t reserve_hint = 0) {
        return make_arena_vector<chain_vector<p_value_type>>(chain_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty vector using the alignment arena.
     * @param[in] reserve_hint Optional number of elements to reserve.
     * @pre Destroy the vector before unwinding its arena scope.
     */
    template <class p_value_type>
    [[nodiscard]] align_vector<p_value_type> make_align_vector(std::size_t reserve_hint = 0) {
        return make_arena_vector<align_vector<p_value_type>>(align_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty vector using the result pool.
     * @param[in] reserve_hint Optional number of elements to reserve.
     * @pre The context must outlive the vector.
     */
    template <class p_value_type>
    [[nodiscard]] result_vector<p_value_type>
    make_result_vector(std::size_t reserve_hint = 0) const {
        return make_arena_vector<result_vector<p_value_type>>(result_arena_, reserve_hint);
    }

    /**
     * @brief Creates an empty hash map using the context's hash pool.
     * @param[in] bucket_hint Initial bucket count, or zero for the container default.
     * @pre The context must outlive the map.
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
     * @brief Reserves aligned array storage until the alignment scope ends.
     *
     * The storage is uninitialized. The caller must construct and destroy any
     * objects that need it.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested alignment, raised to the element alignment if needed.
     * @param[in] count Number of elements.
     * @return Span over context-owned storage.
     * @throws std::bad_array_new_length If the byte count overflows.
     */
    template <class p_value_type, std::size_t p_alignment = alignof(p_value_type)>
    [[nodiscard]] std::span<p_value_type> alloc_align_array(std::size_t count) {
        return allocate_array<p_value_type, p_alignment>(
            align_arena_, large_align_blocks_, count);
    }

    /**
     * @brief Reserves aligned array storage until the chain scope ends.
     *
     * The storage is uninitialized. The caller must construct and destroy any
     * objects that need it.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested alignment, raised to the element alignment if needed.
     * @param[in] count Number of elements.
     * @return Span over context-owned storage.
     * @throws std::bad_array_new_length If the byte count overflows.
     */
    template <class p_value_type, std::size_t p_alignment = alignof(p_value_type)>
    [[nodiscard]] std::span<p_value_type> alloc_chain_array(std::size_t count) {
        return allocate_array<p_value_type, p_alignment>(
            chain_arena_, large_chain_blocks_, count);
    }

    /**
     * @brief Reserves aligned array storage until the scratch scope ends.
     *
     * The storage is uninitialized. The caller must construct and destroy any
     * objects that need it.
     *
     * @tparam p_value_type Element type.
     * @tparam p_alignment Requested alignment, raised to the element alignment if needed.
     * @param[in] count Number of elements.
     * @return Span over context-owned storage.
     * @throws std::bad_array_new_length If the byte count overflows.
     */
    template <class p_value_type, std::size_t p_alignment = alignof(p_value_type)>
    [[nodiscard]] std::span<p_value_type> alloc_scratch_array(std::size_t count) {
        return allocate_array<p_value_type, p_alignment>(
            scratch_arena_, large_scratch_blocks_, count);
    }

    /**
     * @brief Frees allocations made in one stack arena during this scope.
     *
     * This also frees heap blocks used for large arrays. Objects must be
     * destroyed before the scope ends.
     */
    template <arena p_arena>
    class scope {
    public:
        /**
         * @brief Saves the arena position and number of tracked heap blocks.
         * @param[in] owner Context whose allocations this scope will release.
         */
        explicit scope(context& owner) noexcept
            : owner_(&owner)
            , arena_marker_(select_arena(owner).top())
            , heap_marker_(select_heap_blocks(owner).blocks.size())
        {}

        scope(const scope&)            = delete;
        scope& operator=(const scope&) = delete;
        scope(scope&&)                 = delete;
        scope& operator=(scope&&)      = delete;

        /** @brief Restores the saved arena position and frees later heap blocks. */
        ~scope() noexcept {
            select_arena(*owner_).unwind(arena_marker_);
            auto& blocks = select_heap_blocks(*owner_).blocks;
            while (blocks.size() > heap_marker_) {
                blocks.pop_back();
            }
        }

    private:
        /**
         * @brief Returns the stack arena selected by p_arena.
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
         * @brief Returns the large-array tracker paired with the selected arena.
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
     * @brief Starts a nested allocation scope for the selected stack arena.
     * @return Object that releases later allocations when it is destroyed.
     */
    template <arena p_arena>
    [[nodiscard]] scope<p_arena> push() noexcept {
        return scope<p_arena>(*this);
    }

    /**
     * @brief Shrinks the stack arenas and frees all separately tracked heap blocks.
     *
     * Calls shrink_to_fit on each stack arena. Callers must unwind active
     * scopes before resetting pipeline storage.
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
     * @brief Returns unused bytes in the scratch arena's current block.
     */
    [[nodiscard]] std::size_t scratch_capacity() const noexcept {
        return scratch_arena_.capacity_left();
    }

    /**
     * @brief Returns unused bytes in the chain arena's current block.
     */
    [[nodiscard]] std::size_t chain_capacity() const noexcept {
        return chain_arena_.capacity_left();
    }

    /**
     * @brief Returns unused bytes in the alignment arena's current block.
     */
    [[nodiscard]] std::size_t align_capacity() const noexcept {
        return align_arena_.capacity_left();
    }

    /**
     * @brief Sums tracked scratch allocations that bypassed the stack arena.
     * @return Allocated bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] std::size_t scratch_heap_fallback_bytes() const noexcept {
        return heap_fallback_bytes(large_scratch_blocks_.blocks);
    }

    /**
     * @brief Sums tracked chain allocations that bypassed the stack arena.
     * @return Allocated bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] std::size_t chain_heap_fallback_bytes() const noexcept {
        return heap_fallback_bytes(large_chain_blocks_.blocks);
    }

    /**
     * @brief Sums tracked alignment allocations that bypassed the stack arena.
     * @return Allocated bytes, saturated at `std::size_t` maximum.
     */
    [[nodiscard]] std::size_t align_heap_fallback_bytes() const noexcept {
        return heap_fallback_bytes(large_align_blocks_.blocks);
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
        /** @brief Memory reservation released after this block is freed. */
        backing_allocation_lease lease{};

        /** @brief Base address to pass to free, which may precede the aligned payload. */
        void* address = nullptr;

        /** @brief Allocation size in bytes. */
        std::size_t bytes = 0;

        heap_block() = default;
        heap_block(void* block_address, std::size_t block_bytes) noexcept
            : address(block_address), bytes(block_bytes) {}
        heap_block(const heap_block&) = delete;
        heap_block& operator=(const heap_block&) = delete;

        heap_block(heap_block&& other) noexcept
            : lease(std::move(other.lease))
            , address(std::exchange(other.address, nullptr))
            , bytes(std::exchange(other.bytes, 0))
        {}

        heap_block& operator=(heap_block&& other) noexcept {
            if (this != &other) {
                release();
                lease = std::move(other.lease);
                address = std::exchange(other.address, nullptr);
                bytes = std::exchange(other.bytes, 0);
            }
            return *this;
        }

        /** @brief Frees the block before releasing its memory reservation. */
        void release() noexcept {
            if (address != nullptr) {
                std::free(address);
                address = nullptr;
            }
            bytes = 0;
            lease.reset();
        }

        ~heap_block() noexcept { release(); }
    };

    /** @brief Tracks large arrays and the reservation for the tracker's own storage. */
    struct heap_block_tracker {
        /** @brief Reservation declared before the vector so it is released after storage. */
        backing_allocation_lease capacity_lease{};

        /** @brief Large arrays, each with its own memory reservation. */
        std::vector<heap_block> blocks{};
    };

    static_assert(std::is_nothrow_move_constructible_v<heap_block>);

    /** @brief Rejects a hook with only its state or function set. */
    [[nodiscard]] static backing_allocation_hook validate_backing_hook(
        backing_allocation_hook hook) {
        if (!hook.is_valid()) {
            throw std::invalid_argument{
                "context backing allocation hook is only partially installed"};
        }
        return hook;
    }

    /** @brief Converts an element count to bytes, checking size_t and uint64_t limits. */
    [[nodiscard]] static std::uint64_t checked_allocation_bytes(
        std::size_t count,
        std::size_t element_bytes) {
        if (element_bytes != 0
            && count > (std::numeric_limits<std::size_t>::max)() / element_bytes) {
            throw std::bad_array_new_length{};
        }
        const std::size_t bytes = count * element_bytes;
        if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)()) {
                throw std::bad_array_new_length{};
            }
        }
        return static_cast<std::uint64_t>(bytes);
    }

    /** @brief Chooses room for another tracker entry, checking for overflow. */
    [[nodiscard]] static std::size_t next_tracker_capacity(
        const std::vector<heap_block>& blocks) {
        if (blocks.size() == (std::numeric_limits<std::size_t>::max)()) {
            throw std::bad_array_new_length{};
        }
        const std::size_t required = blocks.size() + 1u;
        if (required <= blocks.capacity()) {
            return blocks.capacity();
        }
        if (blocks.capacity() == 0) {
            return 1u;
        }
        if (blocks.capacity()
            > (std::numeric_limits<std::size_t>::max)() / 2u) {
            return required;
        }
        const std::size_t doubled = blocks.capacity() * 2u;
        return doubled < required ? required : doubled;
    }

    struct heap_materialization_state {
        std::size_t bytes = 0u;
        std::size_t alignment = 0u;
        bioseq::common::heap_allocation_detail::heap_allocation_plan plan{};
        std::size_t planned_capacity = 0u;
        bool replace_tracker = false;
        void* pending_allocation = nullptr;
        void* payload = nullptr;
        std::size_t allocation_bytes = 0u;
        std::vector<heap_block> replacement{};
    };

    /** @brief Allocates a large array and any needed tracker storage under one reservation. */
    [[nodiscard]] static backing_allocation_materialization
    materialize_heap_block(
        void* opaque_state,
        const backing_allocation_reconciliation& reconciliation) {
        auto& state = *static_cast<heap_materialization_state*>(opaque_state);
        try {
            state.pending_allocation =
                bioseq::common::heap_allocation_detail::allocate_and_touch(
                    state.plan, state.allocation_bytes);
            state.payload = bioseq::common::heap_allocation_detail::aligned_heap_payload(
                state.pending_allocation,
                state.allocation_bytes,
                state.bytes,
                state.alignment);
            if (state.payload == nullptr) {
                throw std::bad_alloc{};
            }
            const std::uint64_t block_bytes = checked_allocation_bytes(
                state.allocation_bytes, 1u);

            std::uint64_t tracker_bytes = 0u;
            if (state.replace_tracker) {
                // libc++ is expected to reserve exactly the requested capacity here.
                // Keep the new vector separate until the hook finishes measuring memory.
                state.replacement.reserve(state.planned_capacity);
                if (state.replacement.capacity() != state.planned_capacity) {
                    throw std::logic_error("context tracker allocation count changed");
                }
                tracker_bytes = checked_allocation_bytes(
                    state.replacement.capacity(), sizeof(heap_block));
                if (tracker_bytes
                    > (std::numeric_limits<std::uint64_t>::max)() - block_bytes) {
                    throw std::bad_array_new_length{};
                }
                if (!reconciliation.reconcile_to(block_bytes + tracker_bytes)) {
                    throw std::bad_alloc{};
                }
            } else {
                if (!reconciliation.reconcile_to(block_bytes)) {
                    throw std::bad_alloc{};
                }
            }

            return {
                .actual_allocation_bytes = block_bytes + tracker_bytes,
                // All raw block bytes were written. Unconstructed spare vector slots
                // have not been touched, so they are excluded from this count.
                .fully_touched_bytes = block_bytes,
                .primary_owner_bytes = block_bytes,
                .replacement_owner_bytes = tracker_bytes,
                .is_host_visible = true,
                .is_host_coherent = true,
                .is_process_footprint_reflected = true,
            };
        } catch (...) {
            cleanup_heap_block(std::addressof(state));
            throw;
        }
    }

    /** @brief Frees unfinished array and tracker allocations before releasing reservations. */
    static void cleanup_heap_block(void* opaque_state) noexcept {
        auto& state = *static_cast<heap_materialization_state*>(opaque_state);
        if (state.pending_allocation != nullptr) {
            std::free(state.pending_allocation);
            state.pending_allocation = nullptr;
        }
        state.payload = nullptr;
        state.allocation_bytes = 0u;
        std::vector<heap_block>{}.swap(state.replacement);
    }

    /**
     * @brief Adds a large array after the memory hook accepts its allocation.
     *
     * If the tracker must grow, build its replacement alongside the old vector.
     * Only move entries and replace the tracker after the hook has accepted
     * the measured size. Failure leaves existing entries in place.
     */
    [[nodiscard]] void* allocate_admitted_heap_block(
        heap_block_tracker& tracker,
        std::size_t bytes,
        std::size_t alignment) {
        const bool replace_tracker = tracker.blocks.size() == tracker.blocks.capacity();
        const std::size_t planned_capacity = replace_tracker
            ? next_tracker_capacity(tracker.blocks)
            : tracker.blocks.capacity();
        const std::uint64_t planned_tracker_bytes = replace_tracker
            ? checked_allocation_bytes(planned_capacity, sizeof(heap_block))
            : 0u;
        const auto plan =
            bioseq::common::heap_allocation_detail::plan_aligned_heap_allocation(
                bytes, alignment);
        const std::uint64_t block_bytes = checked_allocation_bytes(
            plan.allocation_requirement_bytes, 1u);
        if (planned_tracker_bytes
            > (std::numeric_limits<std::uint64_t>::max)() - block_bytes) {
            throw std::bad_array_new_length{};
        }

        heap_materialization_state state{
            .bytes = bytes,
            .alignment = alignment,
            .plan = plan,
            .planned_capacity = planned_capacity,
            .replace_tracker = replace_tracker,
        };
        backing_allocation_authority authority;
        try {
            authority = backing_hook_.materialize(
                block_bytes + planned_tracker_bytes,
                backing_allocation_lifetime::adjacent,
                std::addressof(state),
                &materialize_heap_block,
                &cleanup_heap_block);
        } catch (...) {
            cleanup_heap_block(std::addressof(state));
            throw;
        }

        // Moving heap_block entries cannot throw. Free the old tracker storage
        // before replacing the reservation that covers it.
        if (replace_tracker) {
            for (auto& block : tracker.blocks) {
                state.replacement.emplace_back(std::move(block));
            }
            state.replacement.emplace_back(
                state.pending_allocation, state.allocation_bytes);
            state.pending_allocation = nullptr;
            tracker.blocks.swap(state.replacement);
            std::vector<heap_block>{}.swap(state.replacement);
        } else {
            tracker.blocks.emplace_back(
                state.pending_allocation, state.allocation_bytes);
            state.pending_allocation = nullptr;
        }
        heap_block& admitted_block = tracker.blocks.back();
        admitted_block.lease = std::move(authority.primary);
        if (replace_tracker) {
            // Replacing the lease releases the old tracker reservation after its
            // storage has been freed, even when the new lease is empty.
            tracker.capacity_lease = std::move(authority.replacement);
        }
        return state.payload;
    }

    /**
     * @brief Creates an empty vector using an arena and optionally reserves capacity.
     * @param[in] owner_arena Allocator borrowed by the returned vector.
     * @param[in] reserve_hint Optional number of elements to reserve.
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
     * @brief Allocates array storage from an arena, using the heap for large arrays.
     *
     * Scopes release these raw arrays as a group. Heap blocks are tracked
     * separately so a nested scope can free only the arrays allocated inside it.
     *
     * @param[in] owner_arena Stack arena for arrays below the size threshold.
     * @param[in,out] heap_tracker Large arrays paired with that arena.
     * @param[in] count Number of elements.
     * @return Uninitialized aligned storage owned by the context.
     * @throws std::bad_array_new_length If the byte count overflows.
     * @warning Without a hook, a failed heap allocation logs and aborts. Hook
     * failures and tracker allocation failures propagate after cleanup.
     */
    template <class p_value_type, std::size_t p_alignment, class p_arena_type>
    [[nodiscard]] std::span<p_value_type> allocate_array(
        p_arena_type& owner_arena,
        heap_block_tracker& heap_tracker,
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
        if (backing_hook_.is_installed()) {
            void* allocation = allocate_admitted_heap_block(
                heap_tracker, bytes, effective_alignment);
            return std::span<p_value_type>(
                static_cast<p_value_type*>(allocation), count);
        }

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
        heap_block pending{allocation, bytes};
        heap_tracker.blocks.push_back(std::move(pending));
        return std::span<p_value_type>(static_cast<p_value_type*>(allocation), count);
    }

    /**
     * @brief Frees all large arrays in a tracker.
     * @param[in,out] tracker Entries to clear. The vector keeps its capacity.
     */
    static void free_heap_blocks(heap_block_tracker& tracker) noexcept {
        tracker.blocks.clear();
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

    backing_allocation_hook backing_hook_{}; ///< Borrowed hook for heap block allocations.
    detail::scratch_arena_type scratch_arena_; ///< Scratch-lifetime stack storage.
    detail::chain_arena_type chain_arena_; ///< Chain-lifetime stack storage.
    detail::alignment_arena_type align_arena_; ///< Alignment-lifetime stack storage.
    mutable detail::result_arena_type result_arena_; ///< Pooled result storage.
    detail::hash_arena_type hash_arena_; ///< Pooled hash-node storage.

    // Trackers use the system allocator so unwinding an arena cannot
    // invalidate the records needed to free its large arrays.
    heap_block_tracker large_align_blocks_; ///< Oversize alignment allocations.
    heap_block_tracker large_chain_blocks_; ///< Oversize chain allocations.
    heap_block_tracker large_scratch_blocks_; ///< Oversize scratch allocations.
};

namespace detail {
inline thread_local context* tls_current = nullptr;
} // namespace detail

/**
 * @brief Returns this thread's installed context, or nullptr if none is installed.
 */
[[nodiscard]] inline context* current_or_null() noexcept {
    return detail::tls_current;
}

/**
 * @brief Returns this thread's installed context.
 * @warning Logs and aborts if no context is installed.
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
 * @brief Installs a context on this thread until the scope ends.
 *
 * Nested scopes restore the previous context. Create and destroy this object
 * on the same thread, while the supplied context is still alive.
 */
class installed_context_scope {
public:
    /**
     * @brief Installs a context until this object is destroyed.
     * @param[in] installed_context Context that must outlive this object.
     */
    explicit installed_context_scope(context& installed_context) noexcept
        : previous_context_(detail::tls_current) {
        detail::tls_current = &installed_context;
    }

    installed_context_scope(const installed_context_scope&)            = delete;
    installed_context_scope& operator=(const installed_context_scope&) = delete;
    installed_context_scope(installed_context_scope&&)                 = delete;
    installed_context_scope& operator=(installed_context_scope&&)      = delete;

    /** @brief Restores the context that was installed before this scope. */
    ~installed_context_scope() noexcept { detail::tls_current = previous_context_; }

private:
    context* previous_context_;
};

} // namespace context
