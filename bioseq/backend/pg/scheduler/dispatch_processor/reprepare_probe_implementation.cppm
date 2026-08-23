/**
 * @file reprepare_probe_implementation.cppm
 * @brief Connects the narrow reprepare probe API to the dispatch processor.
 */
module bioseq.postgres.scheduler.dispatch_processor.reprepare_probe;

import bioseq.postgres.scheduler.dispatch_processor;

namespace bioseq::simd {

[[nodiscard]] bool cpu_prepared_target_same_object_reprepare_probe() {
    return batch_processor::cpu_prepared_target_same_object_reprepare_probe();
}

} // namespace bioseq::simd
