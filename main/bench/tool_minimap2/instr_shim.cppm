/**
 * @file instr_shim.cppm
 * @brief C-linkage bridge from native minimap2 markers to instrumentation.
 *
 * Native sources call the stable functions declared by `instr_shim.h`.
 * This module keeps their C-linkage surface independent of the C++ module
 * API while forwarding timing records into the benchmark thread profiler.
 */
module;

#include <cstdint>

export module bench.tool_minimap2.instr_shim;

import bioseq.instrumentation;

namespace bioseq::bench::tool_minimap2_instrumentation {

/** @brief Profiling policy shared with benchmark execution. */
using profiling_configuration =
    bioseq::instrumentation::benchmark_profiling_configuration;

}  // namespace bioseq::bench::tool_minimap2_instrumentation

extern "C" {

/**
 * @brief Registers or finds a stable instrumentation region.
 * @param[in] name Stable region key.
 * @param[in] file Native source-file name retained for the C ABI.
 * @param[in] line Native source line retained for the C ABI.
 * @return Opaque address of the instrumentation descriptor.
 */
[[gnu::cold]]
const void* bench_instr_region_register(
    const char* name,
    const char* /*file*/,
    std::uint32_t /*line*/)
{
    // The trampoline's own source location is common to every call, so the
    // native marker name is the only meaningful stable identity here.
    auto* descriptor =
        bioseq::instrumentation::register_static_region(name);
    return static_cast<const void*>(descriptor);
}

/**
 * @brief Starts timing an instrumentation region.
 * @param[in] descriptor_address Opaque registered descriptor address.
 * @return Start time in nanoseconds, or zero when profiling is inactive.
 */
[[gnu::hot]]
std::uint64_t bench_instr_region_begin(
    const void* descriptor_address)
{
    using profiling_configuration =
        bioseq::bench::tool_minimap2_instrumentation::profiling_configuration;
    if (!bioseq::instrumentation::is_runtime_enabled()) {
        return 0;
    }
    if (descriptor_address == nullptr) {
        return 0;
    }
    const auto* descriptor = static_cast<
        const bioseq::instrumentation::region_descriptor*>(
            descriptor_address);
    if (descriptor->id == 0) {
        return 0;
    }
    auto* profiler = bioseq::instrumentation::active_thread_profiler<
        profiling_configuration>;
    if (profiler == nullptr) {
        return 0;
    }
    profiler->stack_depth += 1;
    return bioseq::instrumentation::clock::now_ns();
}

/**
 * @brief Finishes timing an instrumentation region.
 * @param[in] descriptor_address Opaque registered descriptor address.
 * @param[in] begin_time_ns Start value returned by `bench_instr_region_begin`.
 */
[[gnu::hot]]
void bench_instr_region_end(
    const void* descriptor_address,
    std::uint64_t begin_time_ns)
{
    using profiling_configuration =
        bioseq::bench::tool_minimap2_instrumentation::profiling_configuration;
    if (begin_time_ns == 0) {
        return;
    }
    if (descriptor_address == nullptr) {
        return;
    }
    const auto* descriptor = static_cast<
        const bioseq::instrumentation::region_descriptor*>(
            descriptor_address);
    if (descriptor->id == 0) {
        return;
    }
    const auto end_time_ns = bioseq::instrumentation::clock::now_ns();
    const auto duration_ns = end_time_ns - begin_time_ns;
    auto* profiler = bioseq::instrumentation::active_thread_profiler<
        profiling_configuration>;
    if (profiler == nullptr) {
        return;
    }
    if (profiler->stack_depth > 0) {
        profiler->stack_depth -= 1;
    }
    auto* aggregate = profiler->aggregates.find_or_create(descriptor->id);
    if (aggregate != nullptr) {
        bioseq::instrumentation::record_region_duration(
            *aggregate,
            duration_ns);
    }
}

}  // extern "C"
