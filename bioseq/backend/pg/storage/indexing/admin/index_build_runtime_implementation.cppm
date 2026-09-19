/**
 * @file admin/index_build_runtime_implementation.cppm
 * @brief Implements PostgreSQL minimizer index build runtime checks.
 */
module bioseq.postgres.storage.indexing.admin.index_build_runtime;

import std;
import bioseq.postgres.configuration.core;
import bioseq.indexing.minimizer_hashmap;
import bioseq.postgres.scheduler.supervisor.vulkan;

namespace bioseq::storage::indexing {

void ensure_vulkan_index_build_runtime_ready(bioseq::minimizers::minimizer_sketch_backend backend) {
    if (backend != bioseq::minimizers::minimizer_sketch_backend::vulkan_segment) {
        return;
    }
    (void)bioseq::postgres::scheduler::ensure_pg_vulkan_runtime_ready();
}

void require_selected_minimizer_kernels(bioseq::minimizers::minimizer_sketch_backend backend,
                                        const bioseq::minimizers::minimizer_kernels* kernels,
                                        std::string_view operation) {
    if (backend == bioseq::minimizers::minimizer_sketch_backend::exact_simd &&
        (kernels == nullptr || kernels->two_stack_nthash == nullptr)) {
        throw bioseq::indexing::exact_simd_index_runtime_error{
            bioseq::indexing::exact_simd_index_status::invalid_state, operation};
    }
    if (backend == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd &&
        (kernels == nullptr || kernels->bounded_nthash == nullptr)) {
        throw bioseq::indexing::packed_bounded_nthash_index_runtime_error{
            bioseq::indexing::packed_bounded_nthash_index_status::invalid_state, operation};
    }
}

void scoped_forced_bounded_nthash_index_failure::arm_after_publisher_state_begin(
    bioseq::minimizers::minimizer_sketch_backend backend) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (static_cast<int>(bioseq::settings::testing::persisted_index::failure_mode.get_value()) ==
            1 &&
        backend == bioseq::minimizers::minimizer_sketch_backend::bounded_nthash_simd) {
        bioseq::indexing::arm_forced_packed_bounded_index_status_for_test(
            bioseq::indexing::packed_bounded_nthash_index_status::unsupported_vector_shape);
        armed_ = true;
    }
#else
    (void)backend;
#endif
}

scoped_forced_bounded_nthash_index_failure::~scoped_forced_bounded_nthash_index_failure() {
    if (armed_) {
        bioseq::indexing::disarm_forced_packed_bounded_index_status_for_test();
    }
}

} // namespace bioseq::storage::indexing
