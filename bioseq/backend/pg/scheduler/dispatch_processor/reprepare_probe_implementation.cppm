/**
 * @file reprepare_probe_implementation.cppm
 * @brief Calls the scheduler test that checks prepared-target cleanup and reuse.
 */
module bioseq.postgres.scheduler.dispatch_processor.reprepare_probe;

import bioseq.postgres.scheduler.dispatch_processor;

namespace bioseq::simd {

[[nodiscard]] bool cpu_prepared_target_same_object_reprepare_probe() {
    return batch_processor::cpu_prepared_target_same_object_reprepare_probe();
}

} // namespace bioseq::simd
