/**
 * @file context.cppm
 * @brief Per-pipeline memory arenas for the minimap_port pipeline.
 *
 * Implements the allocator wrapper described in
 * RESEARCH/SPEC_MEMORY_ALLOCATORS.md. Each `context::context` owns a few
 * arenas whose lifetimes match distinct pipeline stages:
 *
 *   - scratch_arena_  : short-lived per-pipeline temporaries (regions,
 *                       coverage intervals, keep flags, sketch outputs).
 *   - chain_arena_    : anchors, chaining DP arrays, RMQ pool, chains.
 *                       Lives for the whole query because align_regions
 *                       still reads the chains.
 *   - align_arena_    : KSW2 DP state, backtrack matrix, encoded sequence
 *                       bytes, block-aligner traces. A memory_stack with
 *                       RAII scopes around each align call; the backing
 *                       block is reused across calls so we never
 *                       system-malloc inside the per-region loop after
 *                       the first growth.
 *   - result_arena_   : surviving output vectors. Backed by a log2-bucket
 *                       memory pool so repeated queries do not fragment
 *                       the system allocator.
 *   - hash_arena_     : TargetIndex hash map buckets + nested position
 *                       vectors. Backed by a log2-bucket memory pool.
 *
 * The spec argues against thread-local storage but the existing pipeline
 * has dozens of templated function signatures that would all need a
 * context& parameter. We therefore expose the current context as a
 * thread-local pointer; callers install one through
 * `installed_context_scope` (RAII).
 */
module;

#include "common.h"

#include <foonathan/memory/container.hpp>
#include <foonathan/memory/memory_stack.hpp>
#include <foonathan/memory/memory_pool.hpp>
#include <foonathan/memory/memory_pool_collection.hpp>
#include <foonathan/memory/aligned_allocator.hpp>
#include <foonathan/memory/allocator_storage.hpp>
#include <foonathan/memory/std_allocator.hpp>
#include <foonathan/memory/allocator_traits.hpp>
#include <foonathan/memory/namespace_alias.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <type_traits>
#include <functional>

export module context;

import mlogging;

using namespace memory::literals;

export namespace context {

// =====================================================================
// Internal allocator aliases. Hidden behind `detail::` so the concrete
// foonathan types can be swapped without touching call sites.
// =====================================================================
namespace detail {

using scratch_arena_t = memory::memory_stack<>;
using chain_arena_t   = memory::memory_stack<>;
using align_arena_t   = memory::memory_stack<>;
using result_arena_t  = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets>;
using hash_arena_t    = memory::memory_pool_collection<
    memory::node_pool, memory::log2_buckets>;

} // namespace detail

/// Identifies a scopable per-pipeline arena.
enum class Arena : std::uint8_t { Scratch, Chain, Align };

// =====================================================================
// STL container aliases. These ARE part of the public surface.
// =====================================================================

template<typename T>
using scratch_vector = memory::vector<T, detail::scratch_arena_t>;

template<typename T>
using chain_vector = memory::vector<T, detail::chain_arena_t>;

template<typename T>
using align_vector = memory::vector<T, detail::align_arena_t>;

template<typename T>
using result_vector = memory::vector<T, detail::result_arena_t>;

template<typename Key, typename Value>
using scratch_unordered_map = memory::unordered_map<Key, Value, detail::hash_arena_t>;

// =====================================================================
// context — the per-query allocator owner.
// =====================================================================

class context {
public:
    /// Block-size budget. Numbers derived from SPEC §5.1 and the current
    /// profile (see build/prof.json — 10 kbp×10 kbp × 50 regions × 3
    /// phases for the dominant KSW2 backtrack matrix term).
    struct sizes {
        std::size_t scratch_block = 256_KiB;
        std::size_t align_block   = 4_MiB;
        std::size_t chain_block   = 1_MiB;
        std::size_t hash_node_max = 256;
        std::size_t hash_block    = 1_MiB;
        std::size_t result_node_max = 256;
        std::size_t result_block  = 64_KiB;
    };

    context()
        : context(sizes{})
    {}

    explicit context(const sizes& budget)
        : scratch_arena_(budget.scratch_block)
        , chain_arena_(budget.chain_block)
        , align_arena_(budget.align_block)
        , result_arena_(budget.result_node_max, budget.result_block)
        , hash_arena_(budget.hash_node_max, budget.hash_block)
    {}

    context(const context&) = delete;
    context& operator=(const context&) = delete;
    context(context&&) = delete;
    context& operator=(context&&) = delete;

    // -----------------------------------------------------------------
    // Raw arena accessors. Used when building std_allocator wrappers
    // for STL containers — prefer make_* factories when possible.
    // -----------------------------------------------------------------
    [[nodiscard]] detail::scratch_arena_t& scratch_arena() noexcept { return scratch_arena_; }
    [[nodiscard]] detail::chain_arena_t&   chain_arena()   noexcept { return chain_arena_;   }
    [[nodiscard]] detail::align_arena_t&   align_arena()   noexcept { return align_arena_;   }
    [[nodiscard]] detail::result_arena_t&  result_arena()  noexcept { return result_arena_;  }
    [[nodiscard]] detail::hash_arena_t&    hash_arena()    noexcept { return hash_arena_;    }

    // -----------------------------------------------------------------
    // Container factories. Each returns a fresh empty container bound
    // to the right arena.
    // -----------------------------------------------------------------
    template<typename T>
    [[nodiscard]] scratch_vector<T> make_scratch_vector(std::size_t reserve_hint = 0) {
        scratch_vector<T> v(scratch_arena_);
        if (reserve_hint) v.reserve(reserve_hint);
        return v;
    }

    template<typename T>
    [[nodiscard]] chain_vector<T> make_chain_vector(std::size_t reserve_hint = 0) {
        chain_vector<T> v(chain_arena_);
        if (reserve_hint) v.reserve(reserve_hint);
        return v;
    }

    template<typename T>
    [[nodiscard]] align_vector<T> make_align_vector(std::size_t reserve_hint = 0) {
        align_vector<T> v(align_arena_);
        if (reserve_hint) v.reserve(reserve_hint);
        return v;
    }

    template<typename T>
    [[nodiscard]] result_vector<T> make_result_vector(std::size_t reserve_hint = 0) {
        result_vector<T> v(result_arena_);
        if (reserve_hint) v.reserve(reserve_hint);
        return v;
    }

    template<typename Key, typename Value>
    [[nodiscard]] scratch_unordered_map<Key, Value>
    make_scratch_unordered_map(std::size_t bucket_hint = 0) {
        scratch_unordered_map<Key, Value> m(
            bucket_hint,
            std::hash<Key>{},
            std::equal_to<Key>{},
            hash_arena_);
        return m;
    }

    // -----------------------------------------------------------------
    // Typed aligned raw buffers. Used where a container is too heavy
    // (KSW2 DP state, backtrack matrix, block aligner trace).
    //
    // The returned span is owned by the matching arena; callers must not
    // retain the pointer across the corresponding scope unwind.
    // -----------------------------------------------------------------
    template<typename T, std::size_t Alignment = alignof(T)>
    [[nodiscard]] std::span<T> alloc_align_array(std::size_t count) {
        static constexpr std::size_t effective_align = Alignment < alignof(T) ? alignof(T) : Alignment;
        if (count == 0) return {};
        const std::size_t bytes = count * sizeof(T);
        void* raw = memory::allocator_traits<detail::align_arena_t>::allocate_node(
            align_arena_, bytes, effective_align);
        return std::span<T>(static_cast<T*>(raw), count);
    }

    template<typename T, std::size_t Alignment = alignof(T)>
    [[nodiscard]] std::span<T> alloc_chain_array(std::size_t count) {
        static constexpr std::size_t effective_align = Alignment < alignof(T) ? alignof(T) : Alignment;
        if (count == 0) return {};
        const std::size_t bytes = count * sizeof(T);
        void* raw = memory::allocator_traits<detail::chain_arena_t>::allocate_node(
            chain_arena_, bytes, effective_align);
        return std::span<T>(static_cast<T*>(raw), count);
    }

    template<typename T, std::size_t Alignment = alignof(T)>
    [[nodiscard]] std::span<T> alloc_scratch_array(std::size_t count) {
        static constexpr std::size_t effective_align = Alignment < alignof(T) ? alignof(T) : Alignment;
        if (count == 0) return {};
        const std::size_t bytes = count * sizeof(T);
        void* raw = memory::allocator_traits<detail::scratch_arena_t>::allocate_node(
            scratch_arena_, bytes, effective_align);
        return std::span<T>(static_cast<T*>(raw), count);
    }

    // -----------------------------------------------------------------
    // Scopes.
    // -----------------------------------------------------------------
    template<Arena A>
    class scope {
    public:
        explicit scope(context& ctx) noexcept
            : ctx_(&ctx), marker_(select_arena(ctx).top())
        {}

        scope(const scope&)            = delete;
        scope& operator=(const scope&) = delete;
        scope(scope&&)                 = delete;
        scope& operator=(scope&&)      = delete;

        ~scope() noexcept {
            select_arena(*ctx_).unwind(marker_);
        }

    private:
        static auto& select_arena(context& ctx) noexcept {
            if constexpr (A == Arena::Scratch) return ctx.scratch_arena_;
            else if constexpr (A == Arena::Chain) return ctx.chain_arena_;
            else                                 return ctx.align_arena_;
        }
        using marker_t = decltype(select_arena(std::declval<context&>()).top());
        context* ctx_;
        marker_t marker_;
    };

    template<Arena A>
    [[nodiscard]] scope<A> push() noexcept { return scope<A>(*this); }

    /// Reset per-pipeline arenas. Keeps backing blocks in the cache for
    /// the next query but marks everything free.
    void reset_pipeline() noexcept {
        scratch_arena_.shrink_to_fit();
        chain_arena_.shrink_to_fit();
        align_arena_.shrink_to_fit();
    }

    // -----------------------------------------------------------------
    // Diagnostics — used by tests/profiling, not by hot code.
    // -----------------------------------------------------------------
    [[nodiscard]] std::size_t scratch_capacity() const noexcept {
        return scratch_arena_.capacity_left();
    }
    [[nodiscard]] std::size_t chain_capacity() const noexcept {
        return chain_arena_.capacity_left();
    }
    [[nodiscard]] std::size_t align_capacity() const noexcept {
        return align_arena_.capacity_left();
    }

private:
    detail::scratch_arena_t scratch_arena_;
    detail::chain_arena_t   chain_arena_;
    detail::align_arena_t   align_arena_;
    detail::result_arena_t  result_arena_;
    detail::hash_arena_t    hash_arena_;
};

// =====================================================================
// Thread-local installation hook.
//
// SPEC §10.3 rejected `temporary_allocator` because it is implicitly
// thread-local. We chose the same pattern anyway because the pipeline's
// templated call graph is 7 000+ LOC and threading a `context&` through
// every overload is infeasible for the first cut. Mitigation:
//   1. Callers use `installed_context_scope` (RAII) rather than raw
//      setters — this ties installation to a stack frame.
//   2. `current()` aborts fast if no context is installed.
//   3. Each top-level pipeline entry point constructs its own context
//      and installs it, so callers outside the pipeline never observe
//      the state.
// =====================================================================

namespace detail {
inline thread_local context* tls_current = nullptr;
} // namespace detail

[[nodiscard]] inline context* current_or_null() noexcept {
    return detail::tls_current;
}

[[nodiscard]] inline context& current() noexcept {
    context* ctx = detail::tls_current;
    if (ctx == nullptr) {
        JustLog("[context] FATAL: no context installed; aborting");
        std::abort();
    }
    return *ctx;
}

class installed_context_scope {
public:
    explicit installed_context_scope(context& ctx) noexcept
        : previous_(detail::tls_current)
    {
        detail::tls_current = &ctx;
    }

    installed_context_scope(const installed_context_scope&)            = delete;
    installed_context_scope& operator=(const installed_context_scope&) = delete;
    installed_context_scope(installed_context_scope&&)                 = delete;
    installed_context_scope& operator=(installed_context_scope&&)      = delete;

    ~installed_context_scope() noexcept {
        detail::tls_current = previous_;
    }

private:
    context* previous_;
};

// =====================================================================
// Legacy alias — the previous stub exposed `context::vector<T>` backed
// by a single memory_pool. A few existing call sites (notably
// kmers/index.cppm) still construct a bare `context::context` purely to
// get a timing-only instrumentation hook; keep the alias as a thin
// scratch_vector so those sites compile unchanged.
// =====================================================================

template<typename T> using vector = scratch_vector<T>;

} // namespace context
