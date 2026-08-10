/**
 * @file scheduler/supervisor/highway_binding.cppm
 * @brief Exact-target Highway rebinding for PostgreSQL background workers.
 */
module;

#include <cstdint>
#include <stdexcept>

module bioseq.postgres.scheduler.supervisor;

import bioseq.highway.dispatch_contract;

namespace bioseq::postgres::scheduler {

const bioseq::minimizers::minimizer_kernels*
rebind_minimizer_kernels_for_backend(
        const std::int32_t backend_value,
        const std::int64_t highway_executed_target) {
    using bioseq::minimizers::minimizer_sketch_backend;
    const auto backend = static_cast<minimizer_sketch_backend>(backend_value);
    if (backend == minimizer_sketch_backend::scalar
        || backend == minimizer_sketch_backend::vulkan_segment) {
        return nullptr;
    }

    const auto& binding = bioseq::simd_dispatch::
        registered_highway_target_binding(highway_executed_target);
    const auto* kernels = &binding.kernels->minimizers;
    if ((backend == minimizer_sketch_backend::two_stack_nthash
            && kernels->two_stack_nthash == nullptr)
        || (backend == minimizer_sketch_backend::bounded_nthash_simd
            && kernels->bounded_nthash == nullptr)) {
        throw std::logic_error{
            "selected Highway target lacks the requested minimizer kernel"};
    }
    return kernels;
}

}  // namespace bioseq::postgres::scheduler
