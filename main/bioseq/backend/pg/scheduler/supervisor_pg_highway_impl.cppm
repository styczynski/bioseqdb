/**
 * @file supervisor_pg_highway_impl.cppm
 * @brief Exact-target Highway rebinding for PostgreSQL background workers.
 */
module;

#include <cstdint>
#include <stdexcept>

module bioseq.postgres.scheduler.supervisor;

import bioseq.highway.dispatch_contract;

namespace bioseq::parallelism::pg {

const bioseq::minimizers::MinimizerKernels*
rebind_minimizer_kernels_for_backend(
        const std::int32_t backend_value,
        const std::int64_t highway_executed_target) {
    using bioseq::minimizers::MinimizerSketchBackend;
    const auto backend = static_cast<MinimizerSketchBackend>(backend_value);
    if (backend == MinimizerSketchBackend::Scalar
        || backend == MinimizerSketchBackend::VulkanSegment) {
        return nullptr;
    }

    const auto& binding = bioseq::simd_dispatch::
        registered_highway_target_binding(highway_executed_target);
    const auto* kernels = &binding.kernels->minimizers;
    if ((backend == MinimizerSketchBackend::ExactSimd
            && kernels->two_stack_nthash == nullptr)
        || (backend == MinimizerSketchBackend::PaperNtHashSimd
            && kernels->paper_nthash == nullptr)) {
        throw std::logic_error{
            "selected Highway target lacks the requested minimizer kernel"};
    }
    return kernels;
}

}  // namespace bioseq::parallelism::pg
