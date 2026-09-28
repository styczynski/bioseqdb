/**
 * @file admin/index_build_runtime.cppm
 * @brief Prepare the selected minimizer backend and  test failure settings.
 */
module;

#include <string_view>

export module bioseq.postgres.storage.indexing.admin.index_build_runtime;

import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

/** Prepares the backend-local Vulkan runtime for a selected GPU build. */
void ensure_vulkan_index_build_runtime_ready(bioseq::minimizers::minimizer_sketch_backend backend);

/** Require a kernel table when the selected SIMD algorithm needs one. */
void require_selected_minimizer_kernels(bioseq::minimizers::minimizer_sketch_backend backend,
                                        const bioseq::minimizers::minimizer_kernels* kernels,
                                        std::string_view operation);

/** Keep a forced bounded-ntHash test failure active for one build. */
class scoped_forced_bounded_nthash_index_failure {
  public:
    /** Enable the configured bounded-ntHash test failure after creating publication state. */
    void
    arm_after_publisher_state_begin(bioseq::minimizers::minimizer_sketch_backend backend) noexcept;

    /** Disarms a test failure that was armed by this scope. */
    ~scoped_forced_bounded_nthash_index_failure();

  private:
    bool armed_ = false;
};

} // namespace bioseq::storage::indexing
