/**
 * @file admin/ready_minimizer_fixture_kernel_implementation.cppm
 * @brief Connect index-generation tests to the shared kernel  selector.
 */
module;

#include <string_view>

module bioseq.postgres.storage.indexing.admin.ready_minimizer_fixture_kernel;

import bioseq.postgres.storage.indexing.admin.minimizer_kernel_selection;

namespace bioseq::storage::indexing {

const bioseq::minimizers::minimizer_kernels*
bind_ready_minimizer_fixture_kernels(
    const bioseq::minimizers::minimizer_sketch_backend backend,
    const std::string_view operation) {
    return bind_selected_minimizer_kernels_for_operation(backend, operation);
}

} // namespace bioseq::storage::indexing
