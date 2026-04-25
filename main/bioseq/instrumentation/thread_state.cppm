/**
 * @file instrumentation/thread_state.cppm
 * @brief ProfilerBlock<Args> — per-thread profiler state.
 *        installed_profiler_scope<Args> — RAII installer for the typed
 *        thread-local pointer.
 *
 * The block lives in a separate thread-local from the context. This is
 * intentional: keeping the existing bioseq::context::context untouched
 * avoids layout changes that would ripple through every allocation site.
 * The dispatch fork in BatchProcessor::process_sequences instantiates a
 * ProfilerBlock<Args> on the stack and installs it via
 * installed_profiler_scope<Args>; nested templated callees read the
 * block via tls_profiler<Args>.
 */
module;

#include <array>
#include <cstdint>

export module bioseq_instr.thread_state;

import bioseq_instr.config;
import bioseq_instr.aggregate;

export namespace bioseq::instr {

enum class arena_kind : std::uint8_t {
    none = 0,
    scratch = 1,
    chain = 2,
    align = 3,
    result = 4,
    hash = 5,
};

constexpr std::size_t kArenaKindCount = 6;

struct ArenaCounter {
    std::uint64_t bytes_live  = 0;
    std::uint64_t bytes_total = 0;
    std::uint64_t alloc_count = 0;
    std::uint64_t largest     = 0;
};

/// Per-pipeline profiler state. Owned on the stack by the dispatch fork
/// in BatchProcessor::process_sequences when Args::enable_profiling is
/// true; not constructed at all when false.
template<is_context_args Args>
struct ProfilerBlock {
    RegionAggregateTable<Args>          aggregates;
    LoopAggregateTable<Args>            loop_aggregates;
    arena_kind                           current_arena = arena_kind::none;
    std::array<ArenaCounter, kArenaKindCount> arena_counters{};
    std::uint16_t                        stack_depth = 0;
    std::uint16_t                        stack_error = 0;

    [[nodiscard, gnu::always_inline]]
    arena_kind current_arena_kind() const noexcept { return current_arena; }

    [[gnu::always_inline]]
    void set_current_arena_kind(arena_kind k) noexcept { current_arena = k; }

    [[nodiscard, gnu::always_inline]]
    ArenaCounter& arena_counter(arena_kind k) noexcept {
        return arena_counters[static_cast<unsigned>(k)];
    }
};

/// Typed thread-local pointer to the currently-installed ProfilerBlock.
/// One slot per distinct Args type the program instantiates. When no
/// profiler is installed for that Args, the pointer is null.
template<is_context_args Args>
inline thread_local ProfilerBlock<Args>* tls_profiler = nullptr;

/// RAII installer. Construct on the stack alongside a stack-local
/// ProfilerBlock<Args>; the constructor stores the address in
/// tls_profiler<Args>; the destructor restores the previous value.
template<is_context_args Args>
struct [[nodiscard]] installed_profiler_scope {
    ProfilerBlock<Args>* previous = nullptr;

    [[gnu::always_inline]]
    explicit installed_profiler_scope(ProfilerBlock<Args>& block) noexcept {
        previous = tls_profiler<Args>;
        tls_profiler<Args> = &block;
    }

    [[gnu::always_inline]]
    ~installed_profiler_scope() noexcept {
        tls_profiler<Args> = previous;
    }

    installed_profiler_scope(const installed_profiler_scope&)            = delete;
    installed_profiler_scope& operator=(const installed_profiler_scope&) = delete;
    installed_profiler_scope(installed_profiler_scope&&)                 = delete;
    installed_profiler_scope& operator=(installed_profiler_scope&&)      = delete;
};

}  // namespace bioseq::instr
