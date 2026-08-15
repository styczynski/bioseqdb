/**
 * @file instrumentation/arena.cppm
 * @brief Scoped allocation attribution and allocator tracker hook.
 *
 * `arena_scope` records a category in the active thread profiler. Disabled
 * configurations discard the tracking path at compile time.
 */
module;

#include <cstddef>
#include <cstdint>

export module bioseq.instrumentation.arena;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.profiler_state;

export namespace bioseq::instrumentation {

/** @brief Temporarily attribute allocations to one arena category. */
template<profiling_configuration p_configuration_type>
struct [[nodiscard]] arena_scope {
    arena_kind previous_arena = arena_kind::none;

    /** @brief Install an arena category and retain the previous category. */
    [[gnu::always_inline]]
    explicit arena_scope(arena_kind kind) noexcept {
        if constexpr (p_configuration_type::enable_arena_tracking) {
            auto* profiler = active_thread_profiler<p_configuration_type>;
            if (profiler == nullptr) return;
            previous_arena = profiler->active_arena_kind();
            profiler->set_active_arena(kind);
        } else {
            (void)kind;
        }
    }

    /** @brief Restore the arena category active before this scope. */
    [[gnu::always_inline]] ~arena_scope() noexcept {
        if constexpr (p_configuration_type::enable_arena_tracking) {
            auto* profiler = active_thread_profiler<p_configuration_type>;
            if (profiler == nullptr) return;
            profiler->set_active_arena(previous_arena);
        }
    }

    arena_scope(const arena_scope&) = delete;
    arena_scope& operator=(const arena_scope&) = delete;
    arena_scope(arena_scope&&) = delete;
    arena_scope& operator=(arena_scope&&) = delete;
};

/** @brief Create an allocation-attribution scope for an arena category. */
template<profiling_configuration p_configuration_type>
[[nodiscard, gnu::always_inline]]
inline arena_scope<p_configuration_type> arena(arena_kind kind) noexcept {
    return arena_scope<p_configuration_type>(kind);
}

/// foonathan/memory-compatible tracker. Wire by composing into a memory
/// arena (`memory::tracked_allocator<profile_tracker, ...>`) — bytes
/// passed through this tracker are attributed to the currently-active
/// arena_kind on the per-thread profiler block.
template<profiling_configuration p_configuration_type>
struct profile_tracker {
    /** @brief Attribute one node allocation to the active arena. */
    [[gnu::always_inline]]
    void on_node_allocation(void*, std::size_t bytes, std::size_t) noexcept {
        if constexpr (!p_configuration_type::enable_arena_tracking) return;
        auto* profiler = active_thread_profiler<p_configuration_type>;
        if (profiler == nullptr) return;
        const auto kind = profiler->active_arena_kind();
        if (kind == arena_kind::none) return;
        auto& counter = profiler->counter_for(kind);
        counter.bytes_live += bytes;
        counter.bytes_total += bytes;
        counter.allocation_count += 1;
        if (bytes > counter.largest_allocation_bytes) {
            counter.largest_allocation_bytes = bytes;
        }
    }

    /** @brief Remove one node allocation from the active arena's live bytes. */
    [[gnu::always_inline]]
    void on_node_deallocation(void*, std::size_t bytes, std::size_t) noexcept {
        if constexpr (!p_configuration_type::enable_arena_tracking) return;
        auto* profiler = active_thread_profiler<p_configuration_type>;
        if (profiler == nullptr) return;
        const auto kind = profiler->active_arena_kind();
        if (kind == arena_kind::none) return;
        auto& counter = profiler->counter_for(kind);
        if (bytes <= counter.bytes_live) counter.bytes_live -= bytes;
        else counter.bytes_live = 0;
    }

    /** @brief Attribute an array allocation using its total byte count. */
    [[gnu::always_inline]]
    void on_array_allocation(void* ptr, std::size_t count,
                             std::size_t size, std::size_t align) noexcept {
        on_node_allocation(ptr, count * size, align);
    }

    /** @brief Remove an array allocation using its total byte count. */
    [[gnu::always_inline]]
    void on_array_deallocation(void* ptr, std::size_t count,
                               std::size_t size, std::size_t align) noexcept {
        on_node_deallocation(ptr, count * size, align);
    }
};

}  // namespace bioseq::instrumentation
