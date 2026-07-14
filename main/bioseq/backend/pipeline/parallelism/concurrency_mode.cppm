/**
 * @file concurrency_mode.cppm
 * @brief Runtime pipeline concurrency mode helpers.
 */
module;

#include <cstdint>
#include <string_view>

export module bioseq.parallelism.mode;

export namespace bioseq::parallelism {

enum class PipelineConcurrencyMode : std::uint8_t {
    SINGLE = 0,
    PARALLEL = 1,
    TASK_PARALLEL = 2,
};

[[nodiscard]] constexpr std::string_view to_string(PipelineConcurrencyMode mode) noexcept {
    switch (mode) {
        case PipelineConcurrencyMode::SINGLE:
            return "SINGLE";
        case PipelineConcurrencyMode::PARALLEL:
            return "PARALLEL";
        case PipelineConcurrencyMode::TASK_PARALLEL:
            return "TASK_PARALLEL";
    }
    return "SINGLE";
}

[[nodiscard]] constexpr bool dispatch_parallel(PipelineConcurrencyMode mode) noexcept {
    return mode == PipelineConcurrencyMode::PARALLEL
        || mode == PipelineConcurrencyMode::TASK_PARALLEL;
}

[[nodiscard]] constexpr bool task_parallel(PipelineConcurrencyMode mode) noexcept {
    return mode == PipelineConcurrencyMode::TASK_PARALLEL;
}

[[nodiscard]] constexpr PipelineConcurrencyMode mode_from_int(int value) noexcept {
    if (value == 1) {
        return PipelineConcurrencyMode::PARALLEL;
    }
    if (value == 2) {
        return PipelineConcurrencyMode::TASK_PARALLEL;
    }
    return PipelineConcurrencyMode::SINGLE;
}

} // namespace bioseq::parallelism
