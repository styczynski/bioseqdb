/**
 * @file instrumentation/arena.cppm
 * @brief Arena attribution scope and foonathan/memory tracker hook.
 *
 * The ArenaScope<Args> RAII type stamps a kind into the per-thread
 * profiler block; allocations through the matching foonathan/memory
 * tracker increment the per-arena counter. When Args::enable_arena_tracking
 * is false the entire mechanism collapses (every if constexpr branch is
 * empty in the discarded instantiation).
 */
module;

#include <cstddef>
#include <cstdint>

export module bioseq.instr.arena;

import bioseq.instr.config;
import bioseq.instr.thread_state;

export namespace bioseq::instr {

template<is_context_args Args>
struct [[nodiscard]] ArenaScope {
    arena_kind prev = arena_kind::none;

    [[gnu::always_inline]]
    explicit ArenaScope(arena_kind k) noexcept {
        if constexpr (Args::enable_arena_tracking) {
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            prev = prof->current_arena_kind();
            prof->set_current_arena_kind(k);
        } else {
            (void)k;
        }
    }

    [[gnu::always_inline]] ~ArenaScope() noexcept {
        if constexpr (Args::enable_arena_tracking) {
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            prof->set_current_arena_kind(prev);
        }
    }

    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;
    ArenaScope(ArenaScope&&) = delete;
    ArenaScope& operator=(ArenaScope&&) = delete;
};

template<is_context_args Args>
[[nodiscard, gnu::always_inline]]
inline ArenaScope<Args> arena(arena_kind k) noexcept {
    return ArenaScope<Args>(k);
}

/// foonathan/memory-compatible tracker. Wire by composing into a memory
/// arena (`memory::tracked_allocator<profile_tracker, ...>`) — bytes
/// passed through this tracker are attributed to the currently-active
/// arena_kind on the per-thread profiler block.
template<is_context_args Args>
struct profile_tracker {
    [[gnu::always_inline]]
    void on_node_allocation(void*, std::size_t bytes, std::size_t) noexcept {
        if constexpr (!Args::enable_arena_tracking) return;
        auto* prof = tls_profiler<Args>;
        if (prof == nullptr) return;
        const auto k = prof->current_arena_kind();
        if (k == arena_kind::none) return;
        auto& c = prof->arena_counter(k);
        c.bytes_live  += bytes;
        c.bytes_total += bytes;
        c.alloc_count += 1;
        if (bytes > c.largest) c.largest = bytes;
    }

    [[gnu::always_inline]]
    void on_node_deallocation(void*, std::size_t bytes, std::size_t) noexcept {
        if constexpr (!Args::enable_arena_tracking) return;
        auto* prof = tls_profiler<Args>;
        if (prof == nullptr) return;
        const auto k = prof->current_arena_kind();
        if (k == arena_kind::none) return;
        auto& c = prof->arena_counter(k);
        if (bytes <= c.bytes_live) c.bytes_live -= bytes;
        else c.bytes_live = 0;
    }

    [[gnu::always_inline]]
    void on_array_allocation(void* ptr, std::size_t count,
                             std::size_t size, std::size_t align) noexcept {
        on_node_allocation(ptr, count * size, align);
    }

    [[gnu::always_inline]]
    void on_array_deallocation(void* ptr, std::size_t count,
                               std::size_t size, std::size_t align) noexcept {
        on_node_deallocation(ptr, count * size, align);
    }
};

}  // namespace bioseq::instr
