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
 * `bench/tool_minimap2/instr_shim.cppm` (a C++26 module that
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

#if !defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)

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

#endif  // !BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT

#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)

#include <stdio.h>

#define BIOSEQ_MINIMAP2_E2E_PHASE_CIGAR ((uint32_t)1u)
#define BIOSEQ_MINIMAP2_E2E_PHASE_MAPQ ((uint32_t)2u)

#ifdef __cplusplus
extern "C" {
#endif

/// Start one operation-origin phase interval. A zero return value is an
/// invalid/disabled sentinel and must be passed unchanged to _end().
uint64_t bioseq_minimap2_e2e_phase_begin(uint32_t phase);

/// Close and publish the half-open [begin_ns, end_ns) interval.
void bioseq_minimap2_e2e_phase_end(uint32_t phase, uint64_t begin_ns);

/// Reset, publish, and inspect helper-private split-file I/O failure state.
void bioseq_minimap2_e2e_io_error_reset(void);
void bioseq_minimap2_e2e_io_error_mark(void);
int bioseq_minimap2_e2e_io_error_seen(void);

/// Flush/check/close a split output, or check/close a split input. Each function
/// consumes the FILE exactly once and publishes any failure to the I/O latch.
int bioseq_minimap2_e2e_close_split_output(FILE *fp);
int bioseq_minimap2_e2e_close_split_input(FILE *fp);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT

#ifdef __cplusplus

#if !defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)

namespace bench_instr_shim_detail {

/// RAII wrapper around the extern-"C" begin/end pair. Stack-only.
struct ScopedRegion {
    const void*    desc;
    uint64_t       t_begin_ns;

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

#endif  // !BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT

#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)

namespace bioseq_minimap2_e2e_phase_detail {

/// Stack-only scope used exclusively by the isolated E2E minimap2 binary.
struct ScopedPhase {
    uint32_t phase;
    uint64_t begin_ns;

    explicit ScopedPhase(uint32_t phase_value) noexcept
        : phase(phase_value),
          begin_ns(::bioseq_minimap2_e2e_phase_begin(phase_value)) {}

    ~ScopedPhase() noexcept {
        ::bioseq_minimap2_e2e_phase_end(phase, begin_ns);
    }

    ScopedPhase(const ScopedPhase&) = delete;
    ScopedPhase& operator=(const ScopedPhase&) = delete;
    ScopedPhase(ScopedPhase&&) = delete;
    ScopedPhase& operator=(ScopedPhase&&) = delete;
};

}  // namespace bioseq_minimap2_e2e_phase_detail

#endif  // BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT

#define BENCH_INSTR_SHIM_PASTE_INNER(a, b) a##b
#define BENCH_INSTR_SHIM_PASTE(a, b) BENCH_INSTR_SHIM_PASTE_INNER(a, b)

/// One-line replacement for the legacy EASY_BLOCK("name") call sites.
/// Registers the descriptor once (static-local cache) and opens an
/// RAII scope. The closing brace of the surrounding block ends the
/// region — same lifetime semantics as the easy_profiler scope it
/// replaces. Safe to invoke multiple times in the same function (each
/// call site gets its own static-local via __LINE__-suffixed names).
#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
#define BENCH_INSTR_BLOCK(name) ((void)0)
#else
#define BENCH_INSTR_BLOCK(name)                                            \
    static const void* BENCH_INSTR_SHIM_PASTE(_bench_instr_desc_, __LINE__) = \
        ::bench_instr_region_register((name), __FILE__,                    \
                                       static_cast<uint32_t>(__LINE__));   \
    ::bench_instr_shim_detail::ScopedRegion                                \
        BENCH_INSTR_SHIM_PASTE(_bench_instr_scope_, __LINE__){             \
            BENCH_INSTR_SHIM_PASTE(_bench_instr_desc_, __LINE__)           \
        }
#endif

#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)
#define BIOSEQ_MINIMAP2_E2E_PHASE_SCOPE(phase_value)                       \
    ::bioseq_minimap2_e2e_phase_detail::ScopedPhase                        \
        BENCH_INSTR_SHIM_PASTE(_bioseq_minimap2_e2e_phase_, __LINE__){     \
            (phase_value)                                                   \
        }
#endif

#else  // !__cplusplus

#if defined(BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT)

// Portable downstream C builds use a cleanup guard below. This repository's
// isolated target follows the committed bundled sources and compiles them as
// C++26, matching the existing alignment benchmark target.
#define BENCH_INSTR_BLOCK(name) ((void)0)

#define BENCH_INSTR_SHIM_PASTE_INNER(a, b) a##b
#define BENCH_INSTR_SHIM_PASTE(a, b) BENCH_INSTR_SHIM_PASTE_INNER(a, b)

#if defined(__clang__)
#if !__has_attribute(cleanup)
#error "BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT requires Clang cleanup support"
#endif
#elif !defined(__GNUC__)
#error "BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT requires Clang or GCC cleanup support"
#endif

typedef struct bioseq_minimap2_e2e_phase_guard_s {
    uint32_t phase;
    uint64_t begin_ns;
} bioseq_minimap2_e2e_phase_guard_t;

static inline bioseq_minimap2_e2e_phase_guard_t
bioseq_minimap2_e2e_phase_guard_make(uint32_t phase)
{
    bioseq_minimap2_e2e_phase_guard_t guard;
    guard.phase = phase;
    guard.begin_ns = bioseq_minimap2_e2e_phase_begin(phase);
    return guard;
}

static inline void bioseq_minimap2_e2e_phase_guard_cleanup(
    bioseq_minimap2_e2e_phase_guard_t *guard)
{
    bioseq_minimap2_e2e_phase_end(guard->phase, guard->begin_ns);
}

#define BIOSEQ_MINIMAP2_E2E_PHASE_SCOPE(phase_value)                       \
    bioseq_minimap2_e2e_phase_guard_t                                      \
        BENCH_INSTR_SHIM_PASTE(_bioseq_minimap2_e2e_phase_, __LINE__)     \
        __attribute__((cleanup(bioseq_minimap2_e2e_phase_guard_cleanup))) = \
            bioseq_minimap2_e2e_phase_guard_make((phase_value))

#else

// We only ever build the minimap2 sources as C++ TUs (CMake routes the
// .c files through CXX), so the C-only path is unreachable. Define
// BENCH_INSTR_BLOCK as a no-op anyway in case a downstream consumer
// tries to compile this header in pure C.
#define BENCH_INSTR_BLOCK(name) ((void)0)

#endif  // BIOSEQ_MINIMAP2_E2E_PHASE_RECEIPT

#endif  // __cplusplus

#endif  // BENCH_TOOL_MINIMAP2_INSTR_SHIM_H
