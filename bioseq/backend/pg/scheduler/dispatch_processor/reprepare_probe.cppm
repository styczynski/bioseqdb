/**
 * @file reprepare_probe.cppm
 * @brief Lets tests check prepared-target reuse without importing the scheduler.
 */
export module bioseq.postgres.scheduler.dispatch_processor.reprepare_probe;

export namespace bioseq::simd {

/**
 * @brief Checks that a prepared target can release its data and prepare  again.
 * @return True if its move, validation, cleanup, and reuse checks pass.
 */
[[nodiscard]] bool cpu_prepared_target_same_object_reprepare_probe();

} // namespace bioseq::simd
