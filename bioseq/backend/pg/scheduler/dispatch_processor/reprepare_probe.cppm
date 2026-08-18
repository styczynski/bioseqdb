/**
 * @file reprepare_probe.cppm
 * @brief Exposes the prepared-target lifetime probe without the dispatch type graph.
 */
export module bioseq.postgres.scheduler.dispatch_processor.reprepare_probe;

export namespace bioseq::simd {

/**
 * @brief Runs the same-object prepared-target reprepare contract probe.
 * @return True when move, validation, release, and reprepare checks pass.
 */
[[nodiscard]] bool cpu_prepared_target_same_object_reprepare_probe();

} // namespace bioseq::simd
