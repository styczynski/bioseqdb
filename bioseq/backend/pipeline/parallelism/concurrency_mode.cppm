/**
 * @file concurrency_mode.cppm
 * @brief Choose which pipeline work may run in parallel.
 */
module;

#include <cstdint>
#include <string_view>

export module bioseq.parallelism.mode;

export namespace bioseq::parallelism {

/// Controls parallel work within a pipeline invocation.
enum class pipeline_concurrency_mode : std::uint8_t {
    /// Run dispatch and coarse pipeline work on the caller thread.
    single_thread = 0,
    /// Allow independent batch items to run in the shared pool.
    dispatch_parallel = 1,
    /// Also allow work within a pipeline to run in the shared pool.
    task_parallel = 2,
};

/// Return the setting name, using "single_thread" for an unknown value.
[[nodiscard]] constexpr std::string_view concurrency_mode_name(
        pipeline_concurrency_mode mode) noexcept {
    switch (mode) {
        case pipeline_concurrency_mode::single_thread:
            return "single_thread";
        case pipeline_concurrency_mode::dispatch_parallel:
            return "dispatch_parallel";
        case pipeline_concurrency_mode::task_parallel:
            return "task_parallel";
    }
    return "single_thread";
}

/// Check whether batch items may run in parallel.
[[nodiscard]] constexpr bool has_parallel_dispatch(
        pipeline_concurrency_mode mode) noexcept {
    return mode == pipeline_concurrency_mode::dispatch_parallel
        || mode == pipeline_concurrency_mode::task_parallel;
}

/// Check whether work within one pipeline may run in parallel.
[[nodiscard]] constexpr bool has_parallel_tasks(
        pipeline_concurrency_mode mode) noexcept {
    return mode == pipeline_concurrency_mode::task_parallel;
}

/// Read the integer setting, using single_thread for unknown values.
[[nodiscard]] constexpr pipeline_concurrency_mode
pipeline_concurrency_mode_from_int(int value) noexcept {
    if (value == 1) {
        return pipeline_concurrency_mode::dispatch_parallel;
    }
    if (value == 2) {
        return pipeline_concurrency_mode::task_parallel;
    }
    return pipeline_concurrency_mode::single_thread;
}

} // namespace bioseq::parallelism
