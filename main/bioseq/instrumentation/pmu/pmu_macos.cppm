/**
 * @file instrumentation/pmu/pmu_macos.cppm
 * @brief Apple Silicon kperf backend.
 *
 * Reads cycles / instructions / branches / branch_misses from Apple's
 * private kperf.framework + kperfdata.framework via dlopen. The
 * underlying kpc_get_thread_counters is a sysctl, so per-call overhead
 * is on the order of a few hundred nanoseconds. PMU access on macOS
 * normally requires root (kpc_force_all_ctrs_set returns EPERM
 * otherwise); the public read_thread_counters() falls back to zeros
 * when init fails so the rest of the pipeline keeps running.
 *
 * Reverse-engineered framework symbols are documented in
 * main/bioseq/RESEARCH/KPERF_EXAMPLE.md (the public-domain xnu/kperf
 * demo by ibireme that this implementation is modelled after).
 */
module;

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(__APPLE__)
#  include <dlfcn.h>
#  include <sys/sysctl.h>
#endif

export module bioseq_instr.pmu.macos;

export namespace bioseq::instr::pmu::macos_backend {

/// Canonical 4-event sample. Mirrors the public counters_t in pmu.cppm
/// so the dispatch in pmu.cppm can copy field-for-field without having
/// to know about platform structs.
struct counters_t {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};

#if defined(__APPLE__)

namespace detail {

// ------------------------------------------------------------------
// Subset of kperf / kperfdata symbol surface we need.
// See KPERF_EXAMPLE.md for the full reverse-engineered header.
// ------------------------------------------------------------------

constexpr std::uint32_t kKpcClassFixedMask        = 1u << 0;
constexpr std::uint32_t kKpcClassConfigurableMask = 1u << 1;
constexpr std::size_t   kKpcMaxCounters           = 32;

using kpc_config_t = std::uint64_t;

struct kpep_event {
    const char* name;
    const char* description;
    const char* errata;
    const char* alias;
    const char* fallback;
    std::uint32_t mask;
    std::uint8_t  number;
    std::uint8_t  umask;
    std::uint8_t  reserved;
    std::uint8_t  is_fixed;
};

struct kpep_db {};
struct kpep_config {};

// Function pointer typedefs for the symbols we resolve at runtime.
using fn_kpc_force_all_ctrs_get  = int  (*)(int*);
using fn_kpc_force_all_ctrs_set  = int  (*)(int);
using fn_kpc_set_counting        = int  (*)(std::uint32_t);
using fn_kpc_set_thread_counting = int  (*)(std::uint32_t);
using fn_kpc_set_config          = int  (*)(std::uint32_t, kpc_config_t*);
using fn_kpc_get_counter_count   = std::uint32_t (*)(std::uint32_t);
using fn_kpc_get_thread_counters = int  (*)(std::uint32_t, std::uint32_t, std::uint64_t*);

using fn_kpep_db_create     = int  (*)(const char*, kpep_db**);
using fn_kpep_db_event      = int  (*)(kpep_db*, const char*, kpep_event**);
using fn_kpep_db_free       = void (*)(kpep_db*);
using fn_kpep_config_create = int  (*)(kpep_db*, kpep_config**);
using fn_kpep_config_free   = void (*)(kpep_config*);
using fn_kpep_config_force_counters = int (*)(kpep_config*);
using fn_kpep_config_add_event   = int (*)(kpep_config*, kpep_event**, std::uint32_t, std::uint32_t*);
using fn_kpep_config_kpc_classes = int (*)(kpep_config*, std::uint32_t*);
using fn_kpep_config_kpc_count   = int (*)(kpep_config*, std::size_t*);
using fn_kpep_config_kpc_map     = int (*)(kpep_config*, std::size_t*, std::size_t);
using fn_kpep_config_kpc         = int (*)(kpep_config*, kpc_config_t*, std::size_t);

/// Process-wide kperf state. Initialised under g_init_once on the first
/// thread that calls init(). Once initialised, every thread can
/// independently call read() without further coordination — the
/// kpc_get_thread_counters sysctl is per-thread.
struct ProcessState {
    std::atomic<bool> initialised{false};
    std::atomic<bool> failed{false};

    void* kperf_handle     = nullptr;
    void* kperfdata_handle = nullptr;

    fn_kpc_force_all_ctrs_get  kpc_force_all_ctrs_get  = nullptr;
    fn_kpc_force_all_ctrs_set  kpc_force_all_ctrs_set  = nullptr;
    fn_kpc_set_counting        kpc_set_counting        = nullptr;
    fn_kpc_set_thread_counting kpc_set_thread_counting = nullptr;
    fn_kpc_set_config          kpc_set_config          = nullptr;
    fn_kpc_get_counter_count   kpc_get_counter_count   = nullptr;
    fn_kpc_get_thread_counters kpc_get_thread_counters = nullptr;

    fn_kpep_db_create          kpep_db_create          = nullptr;
    fn_kpep_db_event           kpep_db_event           = nullptr;
    fn_kpep_db_free            kpep_db_free            = nullptr;
    fn_kpep_config_create      kpep_config_create      = nullptr;
    fn_kpep_config_free        kpep_config_free        = nullptr;
    fn_kpep_config_force_counters kpep_config_force_counters = nullptr;
    fn_kpep_config_add_event   kpep_config_add_event   = nullptr;
    fn_kpep_config_kpc_classes kpep_config_kpc_classes = nullptr;
    fn_kpep_config_kpc_count   kpep_config_kpc_count   = nullptr;
    fn_kpep_config_kpc_map     kpep_config_kpc_map     = nullptr;
    fn_kpep_config_kpc         kpep_config_kpc         = nullptr;

    std::uint32_t classes        = 0;
    std::uint32_t counter_count  = 0;
    std::size_t   counter_map[4] = {0, 0, 0, 0};   // event-index -> counter slot
};

inline ProcessState& process_state() noexcept {
    static ProcessState s;
    return s;
}

inline std::once_flag& init_once() noexcept {
    static std::once_flag flag;
    return flag;
}

/// Try to dlopen() kperf.framework / kperfdata.framework and resolve the
/// minimum symbol set needed for our 4-event group. Returns true iff
/// every required symbol resolved.
[[nodiscard]] inline bool resolve_symbols(ProcessState& s) noexcept {
    constexpr const char* kKperfPath =
        "/System/Library/PrivateFrameworks/kperf.framework/kperf";
    constexpr const char* kKperfdataPath =
        "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata";

    s.kperf_handle = ::dlopen(kKperfPath, RTLD_LAZY);
    s.kperfdata_handle = ::dlopen(kKperfdataPath, RTLD_LAZY);
    if (s.kperf_handle == nullptr || s.kperfdata_handle == nullptr) {
        return false;
    }

    auto resolve = [](void* handle, const char* name) noexcept -> void* {
        return ::dlsym(handle, name);
    };

    s.kpc_force_all_ctrs_get  = reinterpret_cast<fn_kpc_force_all_ctrs_get >(resolve(s.kperf_handle, "kpc_force_all_ctrs_get"));
    s.kpc_force_all_ctrs_set  = reinterpret_cast<fn_kpc_force_all_ctrs_set >(resolve(s.kperf_handle, "kpc_force_all_ctrs_set"));
    s.kpc_set_counting        = reinterpret_cast<fn_kpc_set_counting       >(resolve(s.kperf_handle, "kpc_set_counting"));
    s.kpc_set_thread_counting = reinterpret_cast<fn_kpc_set_thread_counting>(resolve(s.kperf_handle, "kpc_set_thread_counting"));
    s.kpc_set_config          = reinterpret_cast<fn_kpc_set_config         >(resolve(s.kperf_handle, "kpc_set_config"));
    s.kpc_get_counter_count   = reinterpret_cast<fn_kpc_get_counter_count  >(resolve(s.kperf_handle, "kpc_get_counter_count"));
    s.kpc_get_thread_counters = reinterpret_cast<fn_kpc_get_thread_counters>(resolve(s.kperf_handle, "kpc_get_thread_counters"));

    s.kpep_db_create          = reinterpret_cast<fn_kpep_db_create         >(resolve(s.kperfdata_handle, "kpep_db_create"));
    s.kpep_db_event           = reinterpret_cast<fn_kpep_db_event          >(resolve(s.kperfdata_handle, "kpep_db_event"));
    s.kpep_db_free            = reinterpret_cast<fn_kpep_db_free           >(resolve(s.kperfdata_handle, "kpep_db_free"));
    s.kpep_config_create      = reinterpret_cast<fn_kpep_config_create     >(resolve(s.kperfdata_handle, "kpep_config_create"));
    s.kpep_config_free        = reinterpret_cast<fn_kpep_config_free       >(resolve(s.kperfdata_handle, "kpep_config_free"));
    s.kpep_config_force_counters = reinterpret_cast<fn_kpep_config_force_counters>(resolve(s.kperfdata_handle, "kpep_config_force_counters"));
    s.kpep_config_add_event   = reinterpret_cast<fn_kpep_config_add_event  >(resolve(s.kperfdata_handle, "kpep_config_add_event"));
    s.kpep_config_kpc_classes = reinterpret_cast<fn_kpep_config_kpc_classes>(resolve(s.kperfdata_handle, "kpep_config_kpc_classes"));
    s.kpep_config_kpc_count   = reinterpret_cast<fn_kpep_config_kpc_count  >(resolve(s.kperfdata_handle, "kpep_config_kpc_count"));
    s.kpep_config_kpc_map     = reinterpret_cast<fn_kpep_config_kpc_map    >(resolve(s.kperfdata_handle, "kpep_config_kpc_map"));
    s.kpep_config_kpc         = reinterpret_cast<fn_kpep_config_kpc        >(resolve(s.kperfdata_handle, "kpep_config_kpc"));

    return s.kpc_force_all_ctrs_get  != nullptr &&
           s.kpc_force_all_ctrs_set  != nullptr &&
           s.kpc_set_counting        != nullptr &&
           s.kpc_set_thread_counting != nullptr &&
           s.kpc_set_config          != nullptr &&
           s.kpc_get_counter_count   != nullptr &&
           s.kpc_get_thread_counters != nullptr &&
           s.kpep_db_create          != nullptr &&
           s.kpep_db_event           != nullptr &&
           s.kpep_db_free            != nullptr &&
           s.kpep_config_create      != nullptr &&
           s.kpep_config_free        != nullptr &&
           s.kpep_config_force_counters != nullptr &&
           s.kpep_config_add_event   != nullptr &&
           s.kpep_config_kpc_classes != nullptr &&
           s.kpep_config_kpc_count   != nullptr &&
           s.kpep_config_kpc_map     != nullptr &&
           s.kpep_config_kpc         != nullptr;
}

/// The 4-event canon. The first matching alias (by lookup against the
/// current host's PMC database) wins; on Apple Silicon the
/// FIXED_CYCLES / FIXED_INSTRUCTIONS pair lives on dedicated fixed
/// counters and the two branch counters consume two configurable slots.
struct event_alias {
    const char* names[8];
};

inline constexpr event_alias kProfileEvents[4] = {
    { { "FIXED_CYCLES",
        "CPU_CLK_UNHALTED.THREAD",
        "CPU_CLK_UNHALTED.CORE",
        nullptr } },
    { { "FIXED_INSTRUCTIONS",
        "INST_RETIRED.ANY",
        nullptr } },
    { { "INST_BRANCH",
        "BR_INST_RETIRED.ALL_BRANCHES",
        "INST_RETIRED.ANY",
        nullptr } },
    { { "BRANCH_MISPRED_NONSPEC",
        "BRANCH_MISPREDICT",
        "BR_MISP_RETIRED.ALL_BRANCHES",
        "BR_INST_RETIRED.MISPRED",
        nullptr } },
};

/// Walk the alias list and return the first event the loaded PMC
/// database knows about, or nullptr if none match.
[[nodiscard]] inline kpep_event* find_event(
    ProcessState& s, kpep_db* db, const event_alias& alias) noexcept
{
    for (const char* name : alias.names) {
        if (name == nullptr) break;
        kpep_event* ev = nullptr;
        if (s.kpep_db_event(db, name, &ev) == 0 && ev != nullptr) {
            return ev;
        }
    }
    return nullptr;
}

/// Process-wide one-shot init. Returns true on success, false on any
/// failure (permissions, missing symbols, unknown CPU, ...). The caller
/// can call this once and remember the result; subsequent calls are
/// no-ops. Failure modes are reported via stderr (cheap and visible
/// when running the bench from a TTY) so the operator can tell
/// `requires sudo` apart from `unknown CPU` apart from `framework
/// missing` apart from `event not found on this PMU`.
[[nodiscard]] inline bool process_init_unguarded() noexcept {
    auto& s = process_state();

    if (!resolve_symbols(s)) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: could not resolve "
            "kperf/kperfdata symbols (framework missing or stripped)\n");
        return false;
    }

    // Permission check — needs root. Without root, kpc_force_all_ctrs_get
    // returns EPERM and there is nothing more we can do.
    int forced = 0;
    if (s.kpc_force_all_ctrs_get(&forced) != 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_force_all_ctrs_get returned "
            "non-zero — macOS PMU access requires root, rerun under sudo\n");
        return false;
    }

    kpep_db* db = nullptr;
    if (s.kpep_db_create(nullptr, &db) != 0 || db == nullptr) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpep_db_create returned no PMC "
            "database — current CPU may not be in /usr/share/kpep/\n");
        return false;
    }

    kpep_config* cfg = nullptr;
    if (s.kpep_config_create(db, &cfg) != 0 || cfg == nullptr) {
        s.kpep_db_free(db);
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpep_config_create returned "
            "null\n");
        return false;
    }

    if (s.kpep_config_force_counters(cfg) != 0) {
        s.kpep_config_free(cfg);
        s.kpep_db_free(db);
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpep_config_force_counters "
            "returned non-zero\n");
        return false;
    }

    // Look up our 4 canonical events. If any one is unavailable on this
    // CPU, abort entire init — the report would be missing a column
    // that downstream code expects.
    static constexpr const char* kAliasNames[4] = {
        "cycles", "instructions", "branches", "branch-misses"
    };
    kpep_event* event_ptrs[4] = { nullptr, nullptr, nullptr, nullptr };
    for (std::size_t i = 0; i < 4; ++i) {
        event_ptrs[i] = find_event(s, db, kProfileEvents[i]);
        if (event_ptrs[i] == nullptr) {
            std::fprintf(stderr,
                "[bioseq.pmu.macos] init failed: event '%s' not found in "
                "this CPU's PMC database\n", kAliasNames[i]);
            s.kpep_config_free(cfg);
            s.kpep_db_free(db);
            return false;
        }
    }

    for (std::size_t i = 0; i < 4; ++i) {
        kpep_event* ev = event_ptrs[i];
        if (s.kpep_config_add_event(cfg, &ev, 0, nullptr) != 0) {
            s.kpep_config_free(cfg);
            s.kpep_db_free(db);
            return false;
        }
    }

    if (s.kpep_config_kpc_classes(cfg, &s.classes) != 0) {
        s.kpep_config_free(cfg);
        s.kpep_db_free(db);
        return false;
    }

    std::size_t reg_count = 0;
    if (s.kpep_config_kpc_count(cfg, &reg_count) != 0) {
        s.kpep_config_free(cfg);
        s.kpep_db_free(db);
        return false;
    }

    if (s.kpep_config_kpc_map(cfg, s.counter_map, sizeof(s.counter_map)) != 0) {
        s.kpep_config_free(cfg);
        s.kpep_db_free(db);
        return false;
    }

    kpc_config_t regs[kKpcMaxCounters] = { 0 };
    if (s.kpep_config_kpc(cfg, regs, sizeof(regs)) != 0) {
        s.kpep_config_free(cfg);
        s.kpep_db_free(db);
        return false;
    }

    // The kpep config is no longer needed after we've copied the
    // register configuration out — kpc_set_config takes ownership of
    // the kernel-side state.
    s.kpep_config_free(cfg);
    s.kpep_db_free(db);

    if (s.kpc_force_all_ctrs_set(1) != 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_force_all_ctrs_set "
            "returned non-zero — needs root\n");
        return false;
    }

    if ((s.classes & kKpcClassConfigurableMask) != 0 && reg_count > 0) {
        if (s.kpc_set_config(s.classes, regs) != 0) {
            std::fprintf(stderr,
                "[bioseq.pmu.macos] init failed: kpc_set_config returned "
                "non-zero (counter conflict?)\n");
            return false;
        }
    }

    if (s.kpc_set_counting(s.classes) != 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_set_counting returned "
            "non-zero\n");
        return false;
    }

    s.counter_count = s.kpc_get_counter_count(s.classes);
    if (s.counter_count == 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_get_counter_count "
            "returned 0\n");
        return false;
    }

    std::fprintf(stderr,
        "[bioseq.pmu.macos] init OK: %u counters, classes=0x%x, "
        "map=[%zu,%zu,%zu,%zu]\n",
        s.counter_count, s.classes,
        s.counter_map[0], s.counter_map[1],
        s.counter_map[2], s.counter_map[3]);
    return true;
}

}  // namespace detail

/// Process-wide init. Idempotent: subsequent calls are no-ops once the
/// first one succeeds (or have failed). The bool return is the cached
/// success/failure of the *first* invocation. The init is std::call_once
/// guarded so concurrent threads can race here without corrupting the
/// kperf state.
[[nodiscard]] inline bool process_init() noexcept {
    auto& s = detail::process_state();
    if (s.initialised.load(std::memory_order_acquire)) return true;
    if (s.failed.load(std::memory_order_acquire))      return false;

    std::call_once(detail::init_once(), [&] {
        if (detail::process_init_unguarded()) {
            s.initialised.store(true, std::memory_order_release);
        } else {
            s.failed.store(true, std::memory_order_release);
        }
    });

    return s.initialised.load(std::memory_order_acquire);
}

/// Per-thread init. Enables thread-local PMU counting on the calling
/// thread. Idempotent. Thread-local cached after first call.
[[nodiscard]] inline bool init_thread() noexcept {
    if (!process_init()) return false;
    auto& s = detail::process_state();

    static thread_local bool t_initialised = false;
    static thread_local bool t_failed      = false;
    if (t_initialised) return true;
    if (t_failed)      return false;

    if (s.kpc_set_thread_counting(s.classes) != 0) {
        t_failed = true;
        return false;
    }
    t_initialised = true;
    return true;
}

inline void shutdown_thread() noexcept {
    auto& s = detail::process_state();
    if (!s.initialised.load(std::memory_order_acquire)) return;
    // Best-effort disable of per-thread counting. On macOS this is
    // process-wide for the calling thread; we do not unwind the
    // process-wide state because other threads might still be using it.
    if (s.kpc_set_thread_counting != nullptr) {
        s.kpc_set_thread_counting(0);
    }
}

/// Read the current 4-event counter values for this thread. Returns
/// zeros if the per-thread group is not initialised. The kernel hands
/// back monotonically-increasing counter values; the caller is expected
/// to subtract a baseline taken at scope-begin to obtain the delta.
[[nodiscard]] inline counters_t read() noexcept {
    auto& s = detail::process_state();
    if (!s.initialised.load(std::memory_order_acquire)) return counters_t{};

    std::uint64_t buf[detail::kKpcMaxCounters] = { 0 };
    if (s.kpc_get_thread_counters(0, detail::kKpcMaxCounters, buf) != 0) {
        return counters_t{};
    }

    return counters_t{
        .cycles        = buf[s.counter_map[0]],
        .instructions  = buf[s.counter_map[1]],
        .branches      = buf[s.counter_map[2]],
        .branch_misses = buf[s.counter_map[3]],
    };
}

[[nodiscard]] inline bool counters_available() noexcept {
    auto& s = detail::process_state();
    return s.initialised.load(std::memory_order_acquire);
}

#else  // !defined(__APPLE__)

[[nodiscard]] inline bool process_init() noexcept     { return false; }
[[nodiscard]] inline bool init_thread() noexcept      { return false; }
inline void shutdown_thread() noexcept                {}
[[nodiscard]] inline counters_t read() noexcept       { return counters_t{}; }
[[nodiscard]] inline bool counters_available() noexcept { return false; }

#endif

}  // namespace bioseq::instr::pmu::macos_backend
