/**
 * @file scheduler/supervisor/vulkan.cppm
 * @brief Open the Vulkan runtime after PostgreSQL shader preparation.
 */
export module bioseq.postgres.scheduler.supervisor.vulkan;

import bioseq.postgres.scheduler.supervisor;
export import bioseq.gpu.vulkan.runtime;

export namespace bioseq::postgres::scheduler {

/** Prepare PostgreSQL shader state, then return this process's Vulkan runtime. */
[[nodiscard]] inline bioseq::gpu::vulkan::runtime& ensure_pg_vulkan_runtime_ready() {
    prepare_pg_vulkan_runtime_ready();
    return bioseq::gpu::vulkan::runtime::instance();
}

} // namespace bioseq::postgres::scheduler
