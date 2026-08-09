/**
 * @file supervisor_pg_persist_queue_legacy_impl.cppm
 * @brief Fail-closed legacy persistence staging entry points.
 */
module;

#include "common.h"

#include <cstdint>
#include <span>
#include <string>

module bioseq.scheduler.supervisor_pg;
import :persist_queue_publish;

namespace bioseq::parallelism::pg {

[[nodiscard]] bool stage_async_index_persist(
        const PersistTarget& target,
        std::span<const PersistChunk> chunks) {
    (void)target;
    (void)chunks;
    record_prohibited_legacy_persist_rejection("stage_dsm_chunks");
    return false;
}

[[nodiscard]] bool stage_async_index_persist_file(
        const PersistTarget& target,
        std::string temp_file_path,
        std::uint64_t file_bytes,
        std::uint64_t chunk_count) {
    (void)target;
    (void)file_bytes;
    (void)chunk_count;
    remove_temp_file_noexcept(temp_file_path);
    record_prohibited_legacy_persist_rejection("stage_external_file");
    return false;
}

} // namespace bioseq::parallelism::pg
