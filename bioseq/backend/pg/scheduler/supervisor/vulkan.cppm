/**
 * @file scheduler/supervisor/vulkan.cppm
 * @brief Vulkan-typed PostgreSQL scheduler supervision API.
 */
export module bioseq.postgres.scheduler.supervisor.vulkan;

import bioseq.postgres.scheduler.supervisor;
export import bioseq.gpu.vulkan.runtime;

export namespace bioseq::postgres::scheduler {

/**
 * @brief Prepares PostgreSQL runtime state before accessing the Vulkan singleton.
 * @return The process-local Vulkan runtime after provider and cache preparation.
 */
[[nodiscard]] inline bioseq::gpu::vulkan::runtime& ensure_pg_vulkan_runtime_ready() {
    prepare_pg_vulkan_runtime_ready();
    return bioseq::gpu::vulkan::runtime::instance();
}

} // namespace bioseq::postgres::scheduler
