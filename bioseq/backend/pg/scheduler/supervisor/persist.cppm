/**
 * @file persist.cppm
 * @brief Hold index-build jobs until their transaction commits.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/parallel.h"
#include "access/xact.h"
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

/**
 * Own a pending index-build job until its transaction commits or aborts.
 * A successful commit transfers the pinned shared-memory segment to the
 * worker queue. Failed publication and rollback release it locally.
 */
struct pending_persist_job {
    // Own the index name sepavately so target never keeps a dangling view.
    // The shared-memory segment contains the name and build metadata only.
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

/** Return this backend's jobs awaiting the transaction  outcome. */
[[nodiscard]] std::vector<pending_persist_job>& pending_persist_jobs() {
    static std::vector<pending_persist_job> jobs;
    return jobs;
}

/**
 * Add a pinned build request to the current transaction's pending jobs.
 * The caller keeps cleanup responsibility until this insertion succeeds.
 * @param target Table, column, index, and sequence identity to copy.
 * @param handle Pinned shared-memory segment containing the build metadata.
 * @param payload_bytes Size of the metadata in bytes.
 */
void enqueue_pending_minimizer_sequence_build(const persist_target& target,
                                              dsm_handle handle,
                                              std::uint64_t payload_bytes) {
    pending_persist_job job{};
    job.target = target;
    job.target.index_name = {};
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.dsm = handle;
    job.payload_bytes = payload_bytes;
    job.payload_kind = persist_payload_kind::direct_minimizer_build_identity;
    pending_persist_jobs().push_back(std::move(job));
}

/**
 * Publish pending jobs on commit. Release them on abort, prepare, or a full
 * shared queue, so a failed background request cannot undo the transaction.
 */
void bioseq_persist_xact_callback(XactEvent event, void*) {
    std::vector<pending_persist_job>& jobs = pending_persist_jobs();
    const bool commit = event == XACT_EVENT_COMMIT || event == XACT_EVENT_PARALLEL_COMMIT;
    const bool abort = event == XACT_EVENT_ABORT || event == XACT_EVENT_PARALLEL_ABORT ||
                       event == XACT_EVENT_PREPARE;
    if (!commit && !abort) {
        return;
    }
    if (commit) {
        for (pending_persist_job& job : jobs) {
            const std::span<pg_persist_queue_slot> persist_queue =
                g_shared == nullptr ? std::span<pg_persist_queue_slot>{}
                                    : std::span<pg_persist_queue_slot>{g_shared->persist_queue};
            if (publish_persist_job_to_shared_queue(persist_queue,
                                                    job.target,
                                                    job.index_name,
                                                    job.dsm,
                                                    job.payload_bytes,
                                                    job.payload_kind)) {
                job.published = true;
            } else {
                bioseq_log<logging_level::debug>(
                    "[bioseq] index: async persist publish failed row={} index={} dsm={} "
                    "shared_queue_available={}",
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

/** Register the callback that publishes or releases this transaction's jobs. */
void register_persist_xact_callback_pg_leaf() {
    RegisterXactCallback(bioseq_persist_xact_callback, nullptr);
}

} // namespace bioseq::postgres::scheduler
