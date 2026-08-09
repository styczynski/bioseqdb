/**
 * @file supervisor_pg_vulkan.cppm
 * @brief Vulkan-typed PostgreSQL scheduler supervision API.
 */
export module bioseq.scheduler.supervisor_pg_vulkan;

import bioseq.postgres.scheduler.supervisor;
export import bioseq.gpu.vulkan.runtime;

export namespace bioseq::parallelism::pg {

[[nodiscard]] inline bioseq::gpu::vulkan::Runtime&
ensure_pg_vulkan_runtime_ready() {
    prepare_pg_vulkan_runtime_ready();
    return bioseq::gpu::vulkan::Runtime::instance();
}

} // namespace bioseq::parallelism::pg
