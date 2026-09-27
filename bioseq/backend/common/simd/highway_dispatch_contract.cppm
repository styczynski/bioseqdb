/**
 * @file
 * @brief Selects and looks up Highway implementations supported by this CPU.
 *
 * Public signatures use ordinary C++ types so Highway vectors and tags
 * stay inside the modules compiled for each instruction set.
 */
module;

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

export module bioseq.highway.dispatch_contract;

export import :pipeline;
export import bioseq.highway.kernel_table_contract;
export import bioseq.sequences.kmers.highway_minimizer_contract;

import bioseq.prefilter.sneaky_snake;
import bioseq.chaining.simd;

export namespace bioseq::simd_dispatch {

/**
 * @brief Selects the highest-priority compiled target that this CPU can run.
 * @param compiled_targets Bit mask of targets included in the build.
 * @return Compiled, supported and runnable masks, plus the selected target bit.
 * The selected bit is zero when none are runnable.
 */
[[nodiscard]] auto make_highway_dispatch_plan(std::int64_t compiled_targets) noexcept
    -> highway_dispatch_plan;

/**
 * @brief Returns target availability and selection for the current build.
 */
[[nodiscard]] auto registered_highway_dispatch_plan() noexcept -> highway_dispatch_plan;

/**
 * @brief Looks up the functions for an already selected Highway target.
 *
 * This lookup keeps the requested target and performs no new selection.
 * @param exact_target One compiled target bit supported by the current CPU.
 * @throws std::logic_error If the bit is invalid, unavailable or cannot run.
 */
[[nodiscard]] auto registered_highway_target_binding(std::int64_t exact_target)
    -> const highway_target_binding&;

/**
 * @brief Returns target names from highest to lowest Highway priority.
 * @param mask Target bits to name.
 */
[[nodiscard]] auto highway_target_names(std::int64_t mask) -> std::vector<std::string_view>;

} // namespace bioseq::simd_dispatch
