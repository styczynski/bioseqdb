/**
 * @file instrumentation/pmu/macos.cppm
 * @brief Read macOS hardware counters through kperf.
 *
 * Load kperf and kperfdata at runtime, find matching events, and configure
 * thread counter reads. Failed setup leaves this backend unavailable.
 */
module;

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>

#if defined(__APPLE__)
#include <dlfcn.h>
#endif

export module bioseq.instrumentation.pmu.macos;

export namespace bioseq::instrumentation::pmu::macos {

/** @brief Four-event sample matching the public PMU counter order. */
struct counter_sample {
    std::uint64_t cycles        = 0;
    std::uint64_t instructions  = 0;
    std::uint64_t branches      = 0;
    std::uint64_t branch_misses = 0;
};
static_assert(sizeof(counter_sample) == 32);

#if defined(__APPLE__)

namespace detail {

inline constexpr std::uint32_t kpc_configurable_class_mask = 1u << 1;
inline constexpr std::size_t maximum_kpc_counter_count = 32;

using kpc_configuration_word = std::uint64_t;

/// Private kperf event layout used by the dynamically resolved API.
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

/// Event-database handle managed by kperfdata.
struct kpep_db {};
/// Event-configuration handle managed by kperfdata.
struct kpep_config {};

// Function pointer typedefs for the symbols we resolve at runtime.
using fn_kpc_force_all_ctrs_get  = int  (*)(int*);
using fn_kpc_force_all_ctrs_set  = int  (*)(int);
using fn_kpc_set_counting        = int  (*)(std::uint32_t);
using fn_kpc_set_thread_counting = int  (*)(std::uint32_t);
using fn_kpc_set_config          = int  (*)(std::uint32_t, kpc_configuration_word*);
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
using fn_kpep_config_kpc         = int (*)(kpep_config*, kpc_configuration_word*, std::size_t);

/// Framework functions and event settings shared by the process.
/// Initialization runs once, while counter reads request the calling thread's values.
struct process_state {
    std::atomic<bool> is_initialized{false};
    std::atomic<bool> has_failed{false};

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
    std::size_t   counter_map[4] = {0, 0, 0, 0};   // Event index to hardware-counter slot.
};

/// Return this module's shared process state.
/// @return State kept until process shutdown.
inline process_state& get_process_state() noexcept {
    static process_state state;
    return state;
}

/// Return the flag used to run process setup once.
/// @return Process-lifetime initialization flag.
inline std::once_flag& initialization_once() noexcept {
    static std::once_flag flag;
    return flag;
}

/// Load kperf and kperfdata and find the required functions.
/// @return True when both frameworks and all required symbols are available.
[[nodiscard]] inline bool resolve_symbols(process_state& state) noexcept {
    constexpr const char* kperf_framework_path =
        "/System/Library/PrivateFrameworks/kperf.framework/kperf";
    constexpr const char* kperfdata_framework_path =
        "/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata";

    state.kperf_handle = ::dlopen(kperf_framework_path, RTLD_LAZY);
    state.kperfdata_handle = ::dlopen(kperfdata_framework_path, RTLD_LAZY);
    if (state.kperf_handle == nullptr || state.kperfdata_handle == nullptr) {
        return false;
    }

    // Look up a function in the loaded framework.
    auto resolve = [](void* handle, const char* name) noexcept -> void* {
        return ::dlsym(handle, name);
    };

    state.kpc_force_all_ctrs_get  = reinterpret_cast<fn_kpc_force_all_ctrs_get >(resolve(state.kperf_handle, "kpc_force_all_ctrs_get"));
    state.kpc_force_all_ctrs_set  = reinterpret_cast<fn_kpc_force_all_ctrs_set >(resolve(state.kperf_handle, "kpc_force_all_ctrs_set"));
    state.kpc_set_counting        = reinterpret_cast<fn_kpc_set_counting       >(resolve(state.kperf_handle, "kpc_set_counting"));
    state.kpc_set_thread_counting = reinterpret_cast<fn_kpc_set_thread_counting>(resolve(state.kperf_handle, "kpc_set_thread_counting"));
    state.kpc_set_config          = reinterpret_cast<fn_kpc_set_config         >(resolve(state.kperf_handle, "kpc_set_config"));
    state.kpc_get_counter_count   = reinterpret_cast<fn_kpc_get_counter_count  >(resolve(state.kperf_handle, "kpc_get_counter_count"));
    state.kpc_get_thread_counters = reinterpret_cast<fn_kpc_get_thread_counters>(resolve(state.kperf_handle, "kpc_get_thread_counters"));

    state.kpep_db_create          = reinterpret_cast<fn_kpep_db_create         >(resolve(state.kperfdata_handle, "kpep_db_create"));
    state.kpep_db_event           = reinterpret_cast<fn_kpep_db_event          >(resolve(state.kperfdata_handle, "kpep_db_event"));
    state.kpep_db_free            = reinterpret_cast<fn_kpep_db_free           >(resolve(state.kperfdata_handle, "kpep_db_free"));
    state.kpep_config_create      = reinterpret_cast<fn_kpep_config_create     >(resolve(state.kperfdata_handle, "kpep_config_create"));
    state.kpep_config_free        = reinterpret_cast<fn_kpep_config_free       >(resolve(state.kperfdata_handle, "kpep_config_free"));
    state.kpep_config_force_counters = reinterpret_cast<fn_kpep_config_force_counters>(resolve(state.kperfdata_handle, "kpep_config_force_counters"));
    state.kpep_config_add_event   = reinterpret_cast<fn_kpep_config_add_event  >(resolve(state.kperfdata_handle, "kpep_config_add_event"));
    state.kpep_config_kpc_classes = reinterpret_cast<fn_kpep_config_kpc_classes>(resolve(state.kperfdata_handle, "kpep_config_kpc_classes"));
    state.kpep_config_kpc_count   = reinterpret_cast<fn_kpep_config_kpc_count  >(resolve(state.kperfdata_handle, "kpep_config_kpc_count"));
    state.kpep_config_kpc_map     = reinterpret_cast<fn_kpep_config_kpc_map    >(resolve(state.kperfdata_handle, "kpep_config_kpc_map"));
    state.kpep_config_kpc         = reinterpret_cast<fn_kpep_config_kpc        >(resolve(state.kperfdata_handle, "kpep_config_kpc"));

    return state.kpc_force_all_ctrs_get  != nullptr &&
           state.kpc_force_all_ctrs_set  != nullptr &&
           state.kpc_set_counting        != nullptr &&
           state.kpc_set_thread_counting != nullptr &&
           state.kpc_set_config          != nullptr &&
           state.kpc_get_counter_count   != nullptr &&
           state.kpc_get_thread_counters != nullptr &&
           state.kpep_db_create          != nullptr &&
           state.kpep_db_event           != nullptr &&
           state.kpep_db_free            != nullptr &&
           state.kpep_config_create      != nullptr &&
           state.kpep_config_free        != nullptr &&
           state.kpep_config_force_counters != nullptr &&
           state.kpep_config_add_event   != nullptr &&
           state.kpep_config_kpc_classes != nullptr &&
           state.kpep_config_kpc_count   != nullptr &&
           state.kpep_config_kpc_map     != nullptr &&
           state.kpep_config_kpc         != nullptr;
}

/// Alternative event names tried in order for each reported counter.
/// The branch-event list includes an instruction-count fallback.
struct event_alias {
    const char* names[8];
};

inline constexpr event_alias profile_events[4] = {
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

/// Find the first alias known to the loaded event database.
/// @return Borrowed event descriptor, or nullptr if none match.
[[nodiscard]] inline kpep_event* find_event(
    process_state& state, kpep_db* db, const event_alias& alias) noexcept
{
    for (const char* name : alias.names) {
        if (name == nullptr) break;
        kpep_event* event = nullptr;
        if (state.kpep_db_event(db, name, &event) == 0 && event != nullptr) {
            return event;
        }
    }
    return nullptr;
}

/// Configure process-wide counters after the caller has serialized setup.
/// Report several setup failures to stderr, including missing frameworks and events.
/// @return True when event configuration and counting setup succeed.
[[nodiscard]] inline bool initialize_process_unlocked() noexcept {
    auto& state = get_process_state();

    if (!resolve_symbols(state)) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: could not resolve "
            "kperf/kperfdata symbols (framework missing or stripped)\n");
        return false;
    }

    // Check whether this process can access the hardware counters.
    int forced = 0;
    if (state.kpc_force_all_ctrs_get(&forced) != 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_force_all_ctrs_get returned "
            "non-zero — macOS PMU access requires root, rerun under sudo\n");
        return false;
    }

    kpep_db* db = nullptr;
    if (state.kpep_db_create(nullptr, &db) != 0 || db == nullptr) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpep_db_create returned no PMC "
            "database — current CPU may not be in /usr/share/kpep/\n");
        return false;
    }

    kpep_config* configuration = nullptr;
    if (state.kpep_config_create(db, &configuration) != 0 || configuration == nullptr) {
        state.kpep_db_free(db);
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpep_config_create returned "
            "null\n");
        return false;
    }

    if (state.kpep_config_force_counters(configuration) != 0) {
        state.kpep_config_free(configuration);
        state.kpep_db_free(db);
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpep_config_force_counters "
            "returned non-zero\n");
        return false;
    }

    // Find all four requested events before enabling any counter group.
    static constexpr const char* event_names[4] = {
        "cycles", "instructions", "branches", "branch-misses"
    };
    kpep_event* events[4] = { nullptr, nullptr, nullptr, nullptr };
    for (std::size_t i = 0; i < 4; ++i) {
        events[i] = find_event(state, db, profile_events[i]);
        if (events[i] == nullptr) {
            std::fprintf(stderr,
                "[bioseq.pmu.macos] init failed: event '%s' not found in "
                "this CPU's PMC database\n", event_names[i]);
            state.kpep_config_free(configuration);
            state.kpep_db_free(db);
            return false;
        }
    }

    for (std::size_t i = 0; i < 4; ++i) {
        kpep_event* event = events[i];
        if (state.kpep_config_add_event(configuration, &event, 0, nullptr) != 0) {
            state.kpep_config_free(configuration);
            state.kpep_db_free(db);
            return false;
        }
    }

    if (state.kpep_config_kpc_classes(configuration, &state.classes) != 0) {
        state.kpep_config_free(configuration);
        state.kpep_db_free(db);
        return false;
    }

    std::size_t register_count = 0;
    if (state.kpep_config_kpc_count(configuration, &register_count) != 0) {
        state.kpep_config_free(configuration);
        state.kpep_db_free(db);
        return false;
    }

    if (state.kpep_config_kpc_map(configuration, state.counter_map, sizeof(state.counter_map)) != 0) {
        state.kpep_config_free(configuration);
        state.kpep_db_free(db);
        return false;
    }

    kpc_configuration_word register_configuration[maximum_kpc_counter_count] = { 0 };
    if (state.kpep_config_kpc(configuration, register_configuration, sizeof(register_configuration)) != 0) {
        state.kpep_config_free(configuration);
        state.kpep_db_free(db);
        return false;
    }

    // The register settings have been copied out, so the temporary event
    // configuration and database can now be freed.
    state.kpep_config_free(configuration);
    state.kpep_db_free(db);

    if (state.kpc_force_all_ctrs_set(1) != 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_force_all_ctrs_set "
            "returned non-zero — needs root\n");
        return false;
    }

    if ((state.classes & kpc_configurable_class_mask) != 0 && register_count > 0) {
        if (state.kpc_set_config(state.classes, register_configuration) != 0) {
            std::fprintf(stderr,
                "[bioseq.pmu.macos] init failed: kpc_set_config returned "
                "non-zero (counter conflict?)\n");
            return false;
        }
    }

    if (state.kpc_set_counting(state.classes) != 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_set_counting returned "
            "non-zero\n");
        return false;
    }

    state.counter_count = state.kpc_get_counter_count(state.classes);
    if (state.counter_count == 0) {
        std::fprintf(stderr,
            "[bioseq.pmu.macos] init failed: kpc_get_counter_count "
            "returned 0\n");
        return false;
    }

    std::fprintf(stderr,
        "[bioseq.pmu.macos] init OK: %u counters, classes=0x%x, "
        "map=[%zu,%zu,%zu,%zu]\n",
        state.counter_count, state.classes,
        state.counter_map[0], state.counter_map[1],
        state.counter_map[2], state.counter_map[3]);
    return true;
}

}  // namespace detail

/// Run process setup once, even when several threads call concurrently.
/// Cache the first success or failure.
/// @return Result of the first setup attempt.
[[nodiscard]] inline bool initialize_process() noexcept {
    auto& state = detail::get_process_state();
    if (state.is_initialized.load(std::memory_order_acquire)) return true;
    if (state.has_failed.load(std::memory_order_acquire))      return false;

    std::call_once(detail::initialization_once(), [&state] {
        if (detail::initialize_process_unlocked()) {
            state.is_initialized.store(true, std::memory_order_release);
        } else {
            state.has_failed.store(true, std::memory_order_release);
        }
    });

    return state.is_initialized.load(std::memory_order_acquire);
}

/// Set up thread counting once after process setup succeeds.
/// Cache the result for this thread. shutdown_thread does not clear that cache.
/// @return Cached thread setup result.
[[nodiscard]] inline bool initialize_thread() noexcept {
    if (!initialize_process()) return false;
    auto& state = detail::get_process_state();

    static thread_local bool thread_is_initialized = false;
    static thread_local bool thread_has_failed      = false;
    if (thread_is_initialized) return true;
    if (thread_has_failed)      return false;

    if (state.kpc_set_thread_counting(state.classes) != 0) {
        thread_has_failed = true;
        return false;
    }
    thread_is_initialized = true;
    return true;
}

/// Request that thread counting stop while keeping process-wide setup.
inline void shutdown_thread() noexcept {
    auto& state = detail::get_process_state();
    if (!state.is_initialized.load(std::memory_order_acquire)) return;
    // Keep framework and event state because other threads may still use it.
    if (state.kpc_set_thread_counting != nullptr) {
        state.kpc_set_thread_counting(0);
    }
}

/// Read the counters selected by the saved event-to-slot mapping.
/// Subtract an earlier sample to measure work over an interval.
/// @return Counts, or zeros if process setup has not succeeded or the read fails.
[[nodiscard]] inline counter_sample read_counters() noexcept {
    auto& state = detail::get_process_state();
    if (!state.is_initialized.load(std::memory_order_acquire)) return counter_sample{};

    std::uint64_t counter_buffer[detail::maximum_kpc_counter_count] = { 0 };
    if (state.kpc_get_thread_counters(0, detail::maximum_kpc_counter_count, counter_buffer) != 0) {
        return counter_sample{};
    }

    return counter_sample{
        .cycles        = counter_buffer[state.counter_map[0]],
        .instructions  = counter_buffer[state.counter_map[1]],
        .branches      = counter_buffer[state.counter_map[2]],
        .branch_misses = counter_buffer[state.counter_map[3]],
    };
}

/// Report whether process-wide counter setup succeeded.
/// @return Recorded process setup state, regardless of thread setup.
[[nodiscard]] inline bool are_thread_counters_available() noexcept {
    auto& state = detail::get_process_state();
    return state.is_initialized.load(std::memory_order_acquire);
}

#else  // !defined(__APPLE__)

/// Reports that the macOS backend is unavailable on this platform.
/// @return Always `false`.
[[nodiscard]] inline bool initialize_process() noexcept     { return false; }
/// Reports that thread-local macOS counters are unavailable on this platform.
/// @return Always `false`.
[[nodiscard]] inline bool initialize_thread() noexcept      { return false; }
/// Performs no work when the macOS backend is unavailable.
inline void shutdown_thread() noexcept                {}
/// Returns an empty sample when the macOS backend is unavailable.
/// @return A zero-initialized sample.
[[nodiscard]] inline counter_sample read_counters() noexcept       { return counter_sample{}; }
/// Reports that thread-local macOS counters are unavailable on this platform.
/// @return Always `false`.
[[nodiscard]] inline bool are_thread_counters_available() noexcept { return false; }

#endif

}  // namespace bioseq::instrumentation::pmu::macos
