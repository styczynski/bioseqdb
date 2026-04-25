/**
 * @file minimap2/instr_shim.h
 * @brief Plain-header shim used by reference minimap2 .c sources to feed
 *        the bioseq instrumentation runtime.
 *
 * The minimap2 sources under this directory used to call easy_profiler's
 * EASY_BLOCK("name") macro at a handful of hot spots. easy_profiler is
 * gone (Part 48.7 of NEW_PROFILING_RESEARCH.md). To preserve the same
 * minimal-edit surface — one line per measurement — this header
 * defines BENCH_INSTR_BLOCK("name"), a drop-in replacement that
 * registers the region once per call site (via a static-local cache)
 * and opens an RAII scope that closes on block exit.
 *
 * The trampolines themselves live in
 * `main/bench/tool_minimap2/instr_shim.cppm` (a C++26 module that
 * imports bioseq_instr). This header only declares them in C linkage
 * and wraps them in a tiny RAII type, so the minimap2 sources never
 * need to import a module.
 *
 * Per-call-site cost when profiling is OFF (no tls_profiler installed):
 *   - one extern call returning 0
 *   - one branch
 *   - one extern call early-out
 * ~5–10 ns per region. Acceptable: the minimap2 source is the
 * non-bioseq baseline and is only invoked from the bench harness.
 */
#ifndef BENCH_TOOL_MINIMAP2_INSTR_SHIM_H
#define BENCH_TOOL_MINIMAP2_INSTR_SHIM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Register or look up the descriptor for `name`. Returns a stable
/// pointer; the same name always returns the same pointer.
const void* bench_instr_region_register(
    const char* name, const char* file, uint32_t line);

/// Open a region timer using the descriptor returned by
/// bench_instr_region_register. Returns t_begin_ns, or 0 when
/// profiling is currently disabled — pass that value back to _end().
uint64_t bench_instr_region_begin(const void* desc);

/// Close a region timer. No-op when t_begin_ns is 0.
void bench_instr_region_end(const void* desc, uint64_t t_begin_ns);

#ifdef __cplusplus
}  // extern "C"
#endif

#ifdef __cplusplus

namespace bench_instr_shim_detail {

/// RAII wrapper around the extern-"C" begin/end pair. Stack-only.
struct ScopedRegion {
    const void*    desc;
    std::uint64_t  t_begin_ns;

    explicit ScopedRegion(const void* d) noexcept
        : desc(d), t_begin_ns(::bench_instr_region_begin(d)) {}

    ~ScopedRegion() noexcept {
        ::bench_instr_region_end(desc, t_begin_ns);
    }

    ScopedRegion(const ScopedRegion&)            = delete;
    ScopedRegion& operator=(const ScopedRegion&) = delete;
    ScopedRegion(ScopedRegion&&)                 = delete;
    ScopedRegion& operator=(ScopedRegion&&)      = delete;
};

}  // namespace bench_instr_shim_detail

#define BENCH_INSTR_SHIM_PASTE_INNER(a, b) a##b
#define BENCH_INSTR_SHIM_PASTE(a, b) BENCH_INSTR_SHIM_PASTE_INNER(a, b)

/// One-line replacement for the legacy EASY_BLOCK("name") call sites.
/// Registers the descriptor once (static-local cache) and opens an
/// RAII scope. The closing brace of the surrounding block ends the
/// region — same lifetime semantics as the easy_profiler scope it
/// replaces. Safe to invoke multiple times in the same function (each
/// call site gets its own static-local via __LINE__-suffixed names).
#define BENCH_INSTR_BLOCK(name)                                            \
    static const void* BENCH_INSTR_SHIM_PASTE(_bench_instr_desc_, __LINE__) = \
        ::bench_instr_region_register((name), __FILE__,                    \
                                       static_cast<uint32_t>(__LINE__));   \
    ::bench_instr_shim_detail::ScopedRegion                                \
        BENCH_INSTR_SHIM_PASTE(_bench_instr_scope_, __LINE__){             \
            BENCH_INSTR_SHIM_PASTE(_bench_instr_desc_, __LINE__)           \
        }

#else  // !__cplusplus

// We only ever build the minimap2 sources as C++ TUs (CMake routes the
// .c files through CXX), so the C-only path is unreachable. Define
// BENCH_INSTR_BLOCK as a no-op anyway in case a downstream consumer
// tries to compile this header in pure C.
#define BENCH_INSTR_BLOCK(name) ((void)0)

#endif  // __cplusplus

#endif  // BENCH_TOOL_MINIMAP2_INSTR_SHIM_H
