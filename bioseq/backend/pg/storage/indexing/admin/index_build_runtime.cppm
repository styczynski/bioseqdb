/**
 * @file admin/index_build_runtime.cppm
 * @brief Defines runtime checks shared by PostgreSQL minimizer index builders.
 */
module;

#include <string_view>

export module bioseq.postgres.storage.indexing.admin.index_build_runtime;

import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::storage::indexing {

/** Prepares the backend-local Vulkan runtime for a selected GPU build. */
void ensure_vulkan_index_build_runtime_ready(bioseq::minimizers::minimizer_sketch_backend backend);

/** Validates the immutable minimizer kernel family selected by an owning operation. */
void require_selected_minimizer_kernels(bioseq::minimizers::minimizer_sketch_backend backend,
                                        const bioseq::minimizers::minimizer_kernels* kernels,
                                        std::string_view operation);

/** Scopes one test-only bounded-ntHash failure across publisher construction. */
class scoped_forced_bounded_nthash_index_failure {
  public:
    /** Arms the configured failure only after publication state exists. */
    void
    arm_after_publisher_state_begin(bioseq::minimizers::minimizer_sketch_backend backend) noexcept;

    /** Disarms a test failure that was armed by this scope. */
    ~scoped_forced_bounded_nthash_index_failure();

  private:
    bool armed_ = false;
};

} // namespace bioseq::storage::indexing
