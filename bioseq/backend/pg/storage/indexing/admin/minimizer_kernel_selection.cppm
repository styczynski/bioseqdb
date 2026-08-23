/**
 * @file admin/minimizer_kernel_selection.cppm
 * @brief Binds the exact minimizer kernel table for one owning operation.
 */
module;

#include <string_view>

export module bioseq.postgres.storage.indexing.admin.minimizer_kernel_selection;

import bioseq.highway.dispatch_contract;
import bioseq.postgres.storage.indexing.admin.index_build_runtime;
import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

/** Bind the process-local minimizer table without importing the policy catalog. */
[[nodiscard]] inline const bioseq::minimizers::minimizer_kernels*
bind_selected_minimizer_kernels_for_operation(
    bioseq::minimizers::minimizer_sketch_backend backend, std::string_view operation) {
    using backend_type = bioseq::minimizers::minimizer_sketch_backend;
    if (backend != backend_type::exact_simd && backend != backend_type::bounded_nthash_simd) {
        return nullptr;
    }
    const auto plan = bioseq::simd_dispatch::registered_highway_dispatch_plan();
    const auto& binding =
        bioseq::simd_dispatch::registered_highway_target_binding(plan.planned_target);
    const auto* kernels = &binding.kernels->minimizers;
    require_selected_minimizer_kernels(backend, kernels, operation);
    return kernels;
}

} // namespace bioseq::storage::indexing
