/**
 * @file supervisor_pg_persist_queue_impl.cppm
 * @brief Shared asynchronous persistence rejection and cleanup policy.
 */
module;

#include "common.h"

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>

module bioseq.scheduler.supervisor_pg;

import bioseq.common.mlogging;

namespace bioseq::parallelism::pg {

inline std::atomic<std::uint64_t> g_prohibited_legacy_persist_rejections{0};

void record_prohibited_legacy_persist_rejection(
        std::string_view route) noexcept {
    g_prohibited_legacy_persist_rejections.fetch_add(
        1, std::memory_order_relaxed);
    bioseq_log<DBG>(
        "[bioseq] ERROR prohibited legacy async persistence rejected route={} total_rejections={}",
        route,
        static_cast<unsigned long long>(
            g_prohibited_legacy_persist_rejections.load(
                std::memory_order_relaxed)));
}

[[nodiscard]] std::uint64_t align_up_u64(
        std::uint64_t value,
        std::uint64_t alignment) noexcept {
    return alignment == 0
        ? value
        : ((value + alignment - 1) / alignment) * alignment;
}

void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    std::string owned{path};
    std::remove(owned.c_str());
}

} // namespace bioseq::parallelism::pg
