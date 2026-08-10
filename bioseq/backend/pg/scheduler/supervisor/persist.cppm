/**
 * @file persist.cppm
 * @brief PostgreSQL asynchronous persistence staging and worker implementation.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/xact.h"
#include "access/parallel.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
}

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

module bioseq.postgres.scheduler.supervisor;
import :persist_queue_publish;
import bioseq.common.logging;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.type;

namespace bioseq::postgres::scheduler {

struct pending_persist_job {
    // `target.index_name` is deliberately empty after staging. The owned name
    // below is the only variable-length identity and is serialized into the
    // metadata-only DSM; no biological/index bytes enter this queue.
    persist_target target{};
    std::string index_name{};
    std::string temp_file_path{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    bool published = false;
    bool file_backed = false;
    persist_payload_kind payload_kind = persist_payload_kind::none;
    bioseq::parallelism::memory_permit permit{};
};

[[nodiscard]] std::vector<pending_persist_job>& pending_persist_jobs() {
    static std::vector<pending_persist_job> jobs;
    return jobs;
}

void enqueue_pending_mps_build(
        const persist_target& target,
        dsm_handle handle,
        std::uint64_t payload_bytes) {
    pending_persist_job job{};
    job.target = target;
    job.target.index_name = {};
    job.index_name.assign(
        target.index_name.begin(), target.index_name.end());
    job.dsm = handle;
    job.payload_bytes = payload_bytes;
    job.payload_kind = persist_payload_kind::direct_minimizer_build_identity;
    pending_persist_jobs().push_back(std::move(job));
}

void bioseq_persist_xact_callback(XactEvent event, void*) {
    std::vector<pending_persist_job>& jobs = pending_persist_jobs();
    const bool commit =
        event == XACT_EVENT_COMMIT
        || event == XACT_EVENT_PARALLEL_COMMIT;
    const bool abort =
        event == XACT_EVENT_ABORT
        || event == XACT_EVENT_PARALLEL_ABORT
        || event == XACT_EVENT_PREPARE;
    if (!commit && !abort) {
        return;
    }
    if (commit) {
        for (pending_persist_job& job : jobs) {
            const std::span<pg_persist_queue_slot> persist_queue =
                g_shared == nullptr
                    ? std::span<pg_persist_queue_slot>{}
                    : std::span<pg_persist_queue_slot>{g_shared->persist_queue};
            if (publish_persist_job_to_shared_queue(
                    persist_queue,
                    job.target,
                    job.index_name,
                    job.dsm,
                    job.payload_bytes,
                    job.payload_kind)) {
                job.published = true;
            } else {
                bioseq_log<logging_level::debug>("[bioseq] index: async persist publish failed row={} index={} dsm={} shared_queue_available={}",
                        static_cast<unsigned long long>(job.target.sequence_rowno),
                        job.index_name,
                        static_cast<unsigned>(job.dsm),
                        g_shared != nullptr);
                if (job.file_backed) {
                    remove_temp_file_noexcept(job.temp_file_path);
                }
                release_pinned_dsm(job.dsm);
                job.dsm = DSM_HANDLE_INVALID;
            }
            job.permit.release();
        }
    } else {
        for (pending_persist_job& job : jobs) {
            if (!job.published) {
                if (job.file_backed) {
                    remove_temp_file_noexcept(job.temp_file_path);
                }
                release_pinned_dsm(job.dsm);
            }
            job.permit.release();
            job.dsm = DSM_HANDLE_INVALID;
        }
    }
    jobs.clear();
}

void register_persist_xact_callback_pg_leaf() {
    RegisterXactCallback(bioseq_persist_xact_callback, nullptr);
}

} // namespace bioseq::postgres::scheduler
