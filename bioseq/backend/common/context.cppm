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

/** @brief Move-only inline lifetime token supplied by an allocation hook. */
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

    /** @brief Destroys the inline token, releasing its external authority. */
    ~backing_allocation_lease() noexcept { reset(); }

    /**
     * @brief Stores a nothrow-movable lifetime token without heap allocation.
     * @tparam p_value_type Concrete token type supplied by the hook owner.
     * @param value Token transferred into this lease.
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

    /** @return Whether this lease contains an external lifetime token. */
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

/** @brief Adjacent authorities returned by one physical-allocation transaction. */
struct backing_allocation_authority {
    backing_allocation_lease primary{};
    backing_allocation_lease replacement{};
};

/** @brief Permit adjustment view valid only inside one hook transaction. */
struct backing_allocation_reconciliation {
    void* state = nullptr;
    bool (*reconcile)(void*, std::uint64_t) noexcept = nullptr;

    /** @return Whether the transaction now owns exactly `bytes`. */
    [[nodiscard]] bool reconcile_to(std::uint64_t bytes) const noexcept {
        return reconcile != nullptr && reconcile(state, bytes);
    }
};

/** @brief Physical facts returned by one allocation callback. */
struct backing_allocation_materialization {
    std::uint64_t actual_allocation_bytes = 0u;
    std::uint64_t fully_touched_bytes = 0u;
    std::uint64_t primary_owner_bytes = 0u;
    std::uint64_t replacement_owner_bytes = 0u;
    bool is_host_visible = false;
    bool is_host_coherent = false;
    bool is_process_footprint_reflected = false;
};

/** @brief Selects aggregate context lifetime or adjacent replaceable leases. */
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
 * @brief Non-owning, allocation-free hook for physical context allocations.
 *
 * The state and transaction function must outlive the context. An all-null
 * hook preserves the ordinary allocator path.
 */
struct backing_allocation_hook {
    void* state = nullptr;
    backing_allocation_transaction transaction = nullptr;

    /** @return Whether physical allocations are admission controlled. */
    [[nodiscard]] constexpr bool is_installed() const noexcept {
        return state != nullptr && transaction != nullptr;
    }

    /** @return Whether the two non-owning hook fields agree. */
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return (state == nullptr) == (transaction == nullptr);
    }

    /** @brief Runs one physical-allocation transaction through the owner. */
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

// =====================================================================
// Internal allocator aliases. Hidden behind `detail::` so the concrete
// foonathan types can be swapped without touching call sites.
// =====================================================================
namespace detail {

/**
 * @brief Stateful foonathan raw allocator that admits whole backing blocks.
 *
 * The allocator receives the requested extent from foonathan's growing block
 * allocator. Instrumented blocks retain their measured native allocation,
 * including alignment padding. Covered suballocations never reach here.
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
            // The prefix is within the admitted/touched allocation and avoids a
            // separately allocating map from aligned pointers to malloc bases.
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

    /** @brief Releases a not-yet-adopted backing block before permit unwind. */
    static void cleanup_block(void* opaque_state) noexcept {
        auto& state = *static_cast<block_materialization_state*>(opaque_state);
        std::free(std::exchange(state.base, nullptr));
        state.allocation = nullptr;
    }

    memory::default_allocator raw_allocator_{};
    backing_allocation_hook hook_{};
};

/** @brief Stack allocator backing scratch-lifetime storage. */
using scratch_arena_type = memory::memory_stack<context_block_allocator>;

/** @brief Stack allocator backing chain-lifetime storage. */
using chain_arena_type = memory::memory_stack<context_block_allocator>;

/** @brief Stack allocator backing alignment-lifetime storage. */
using alignment_arena_type = memory::memory_stack<context_block_allocator>;

/** @brief Bucketed pool allocator backing surviving results. */
using result_arena_type = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets, context_block_allocator>;

/** @brief Bucketed pool allocator backing hash-map nodes. */
using hash_arena_type = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets, context_block_allocator>;

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
 * `installed_context_scope`. Callers must not share it concurrently.
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
    context() : context(arena_configuration{}, backing_allocation_hook{}) {}

    /**
     * @brief Constructs arenas with an explicit block-size budget.
     * @param[in] configuration Initial arena block sizes and pool node limits.
     */
    explicit context(const arena_configuration& configuration)
        : context(configuration, backing_allocation_hook{})
    {}

    /**
     * @brief Constructs arenas with admission installed before every initial block.
     * @param[in] configuration Initial arena block sizes and pool node limits.
     * @param[in] hook Non-owning physical-allocation admission endpoints.
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

    /** @return Whether this context admits every physical backing allocation. */
    [[nodiscard]] bool has_backing_allocation_hook() const noexcept {
        return backing_hook_.is_installed();
    }

    /**
     * @brief Materializes one replaceable heap allocation through this context's hook.
     *
     * This inline boundary belongs to the shared macro-neutral context module.
     * Its result-container callers remain E2E/test-only; declaring the method
     * adds no state or automatic work to ordinary contexts.
     *
     * The returned authorities must be stored beside the physical owners they
     * cover. Arena suballocations must continue to use their arena allocator and
     * therefore never call this boundary.
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
     * @throws std::bad_array_new_length When the requested byte count overflows.
     * @warning The returned storage is uninitialized. The caller owns object initialization.
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
     * @throws std::bad_array_new_length When the requested byte count overflows.
     * @warning The returned storage is uninitialized. The caller owns object initialization.
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
     * @throws std::bad_array_new_length When the requested byte count overflows.
     * @warning The returned storage is uninitialized. The caller owns object initialization.
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
            , heap_marker_(select_heap_blocks(owner).blocks.size())
        {}

        scope(const scope&)            = delete;
        scope& operator=(const scope&) = delete;
        scope(scope&&)                 = delete;
        scope& operator=(scope&&)      = delete;

        /** @brief Unwinds arena and oversize allocations created inside the scope. */
        ~scope() noexcept {
            select_arena(*owner_).unwind(arena_marker_);
            auto& blocks = select_heap_blocks(*owner_).blocks;
            while (blocks.size() > heap_marker_) {
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
     * `shrink_to_fit` contract. Aligned heap fallbacks are released immediately.
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
        /** @brief Exact unreflected authority released after the block is freed. */
        backing_allocation_lease lease{};

        /** @brief Address returned by `posix_memalign`. */
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

        /** @brief Frees storage before releasing its adjacent permit. */
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

    /** @brief Owns one replaceable tracker allocation and its live entries. */
    struct heap_block_tracker {
        /** @brief Authority for the current vector capacity, before its owner. */
        backing_allocation_lease capacity_lease{};

        /** @brief Oversize blocks; entries carry their own adjacent permits. */
        std::vector<heap_block> blocks{};
    };

    static_assert(std::is_nothrow_move_constructible_v<heap_block>);

    /** @brief Rejects a half-installed non-owning hook before any arena exists. */
    [[nodiscard]] static backing_allocation_hook validate_backing_hook(
        backing_allocation_hook hook) {
        if (!hook.is_valid()) {
            throw std::invalid_argument{
                "context backing allocation hook is only partially installed"};
        }
        return hook;
    }

    /** @brief Computes a byte product accepted by both size_t and permit APIs. */
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

    /** @brief Computes the explicit next tracker capacity without overflow. */
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

    /** @brief Performs the allocation portion of one admitted heap transaction. */
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
                // The supported libc++ reserve path forwards this exact count
                // to std::allocator::allocate_at_least, which returns that count.
                // Keep the replacement private through the hook's second sample.
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
                // The raw block is fully touched. Spare vector capacity is
                // unconstructed and therefore deliberately excluded.
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

    /** @brief Frees a pending oversize block before local authority unwinds. */
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
     * @brief Allocates one oversize block and any tracker replacement atomically.
     *
     * The new block is allocated first. A replacement tracker is then prepared
     * without disturbing the old owner. Entries move and the replacement is
     * published only after reconciliation and the hook transaction complete.
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

        // The capacity is already materialized and heap_block moves cannot
        // throw. Free old tracker storage before replacing its authority.
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
            // Moving an empty reflected lease also releases the old tracker
            // authority after its replaced storage has been destroyed.
            tracker.capacity_lease = std::move(authority.replacement);
        }
        return state.payload;
    }

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
     * @warning Failure of an oversize aligned allocation logs and aborts. Failure to
     *          extend the heap tracker releases the new block and propagates.
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
     * @brief Releases every aligned block in a tracker.
     * @param[in,out] blocks Tracker cleared after all addresses are freed.
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

    backing_allocation_hook backing_hook_{}; ///< Non-owning physical-allocation hook.
    detail::scratch_arena_type scratch_arena_; ///< Scratch-lifetime stack storage.
    detail::chain_arena_type chain_arena_; ///< Chain-lifetime stack storage.
    detail::alignment_arena_type align_arena_; ///< Alignment-lifetime stack storage.
    mutable detail::result_arena_type result_arena_; ///< Pooled result storage.
    detail::hash_arena_type hash_arena_; ///< Pooled hash-node storage.

    // These sparse trackers deliberately use the system allocator: placing
    // their bookkeeping in a scoped arena would make tracker lifetime depend
    // on the allocation lifetime it controls.
    heap_block_tracker large_align_blocks_; ///< Oversize alignment allocations.
    heap_block_tracker large_chain_blocks_; ///< Oversize chain allocations.
    heap_block_tracker large_scratch_blocks_; ///< Oversize scratch allocations.
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
 * Scopes may nest. Destruction restores the pointer observed by the constructor.
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
