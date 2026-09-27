/**
 * @file persist.cppm
 * @brief PostgreSQL asynchronous persistence staging and worker implementation.
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
 * @brief Owns one transaction-local persistence job until publish or rollback.
 *
 * Variable-length identity remains backend-local while the pinned DSM carries only fixed
 * metadata. The transaction callback releases the permit and every unpublished resource.
 */
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

/**
 * @brief Returns the backend-local transaction queue.
 * @return Jobs awaiting the current transaction outcome.
 */
[[nodiscard]] std::vector<pending_persist_job>& pending_persist_jobs() {
    static std::vector<pending_persist_job> jobs;
    return jobs;
}

/**
 * @brief Transfers a pinned metadata DSM into transaction-local pending work.
 * @param[in] target Stable relation and sequence identity.
 * @param[in] handle Pinned metadata DSM released by publish failure or worker completion.
 * @param[in] payload_bytes Mapped metadata size in bytes.
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
 * @brief Publishes jobs after commit and releases them after abort or prepare.
 * @param[in] event PostgreSQL transaction event controlling queue publication.
 * @note Publication failure releases the job locally and does not escape the callback.
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

/**
 * @brief Registers the persistence transaction callback at the PostgreSQL leaf boundary.
 * @note Called once during backend initialization.
 */
void register_persist_xact_callback_pg_leaf() {
    RegisterXactCallback(bioseq_persist_xact_callback, nullptr);
}

} // namespace bioseq::postgres::scheduler
