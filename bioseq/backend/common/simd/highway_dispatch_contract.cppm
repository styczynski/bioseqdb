/**
 * @file
 * @brief Target-neutral ABI for the one-shot Highway dispatch root.
 *
 * The interface excludes Highway declarations so fixed-target implementations
 * cannot expose target-local vector or descriptor types through module
 * boundaries.
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
 * @brief Build a dispatch plan from an exact compiled-target mask.
 * @param compiled_targets Mask of fixed targets present in the registry.
 * @return Plan intersecting the compiled mask with the current CPU and OS
 * capabilities and selecting the highest-priority runnable target.
 */
[[nodiscard]] auto make_highway_dispatch_plan(std::int64_t compiled_targets) noexcept
    -> highway_dispatch_plan;

/**
 * @brief Return metadata for the registered fixed-target inventory.
 * @return Dispatch plan for the process registry.
 */
[[nodiscard]] auto registered_highway_dispatch_plan() noexcept -> highway_dispatch_plan;

/**
 * @brief Resolve an already-selected target bit in the current process.
 *
 * The lookup is direct and never invokes another dynamic dispatch root.
 *
 * @param exact_target Exact single-bit Highway target identity.
 * @return Registered binding for `exact_target`.
 */
[[nodiscard]] auto registered_highway_target_binding(std::int64_t exact_target)
    -> const highway_target_binding&;

/**
 * @brief Format target bits in Highway priority order.
 * @param mask Target mask to enumerate.
 * @return TargetName strings ordered from highest to lowest priority.
 */
[[nodiscard]] auto highway_target_names(std::int64_t mask) -> std::vector<std::string_view>;

} // namespace bioseq::simd_dispatch
