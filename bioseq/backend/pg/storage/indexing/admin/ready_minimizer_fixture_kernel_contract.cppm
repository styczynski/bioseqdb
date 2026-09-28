/**
 * @file admin/ready_minimizer_fixture_kernel_contract.cppm
 * @brief Select minimizer kernels for index-generation tests.
 */
module;

#include <string_view>

export module bioseq.postgres.storage.indexing.admin.ready_minimizer_fixture_kernel;

import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

/** Select the SIMD kernel table used by the requested test backend. */
[[nodiscard]] const bioseq::minimizers::minimizer_kernels*
bind_ready_minimizer_fixture_kernels(
    bioseq::minimizers::minimizer_sketch_backend backend,
    std::string_view operation);

} // namespace bioseq::storage::indexing
