/**
 * @file concurrency_mode.cppm
 * @brief Runtime pipeline concurrency mode helpers.
 */
module;

#include <cstdint>
#include <string_view>

export module bioseq.parallelism.mode;

export namespace bioseq::parallelism {

/// Scheduler concurrency policy selected for one pipeline invocation.
enum class pipeline_concurrency_mode : std::uint8_t {
    /// Execute every dispatch and coarse region on the caller thread.
    single_thread = 0,
    /// Allow dispatch-level work to use the shared pool.
    dispatch_parallel = 1,
    /// Allow dispatch-level work and coarse regions to use the shared pool.
    task_parallel = 2,
};

/// Return the stable configuration spelling of a concurrency mode.
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

/// Return whether dispatch-level work may use the shared pool.
[[nodiscard]] constexpr bool has_parallel_dispatch(
        pipeline_concurrency_mode mode) noexcept {
    return mode == pipeline_concurrency_mode::dispatch_parallel
        || mode == pipeline_concurrency_mode::task_parallel;
}

/// Return whether coarse pipeline regions may create parallel tasks.
[[nodiscard]] constexpr bool has_parallel_tasks(
        pipeline_concurrency_mode mode) noexcept {
    return mode == pipeline_concurrency_mode::task_parallel;
}

/// Decode the PostgreSQL integer setting without admitting unknown values.
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
