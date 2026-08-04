/**
 * @file supervisor_pg_mps_impl.cppm
 * @brief Heavy asynchronous MPS worker implementation.
 */
module;

#include "common.h"

extern "C" {
#include "miscadmin.h"
#include "utils/elog.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

module bioseq.scheduler.supervisor_pg;

import bioseq.common.mlogging;
import bioseq.instr;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.pg_utils;
import bioseq.indexing.minimizer_sequence;
import bioseq.storage.lazy;
import bioseq.storage.scanner;
import bioseq.storage.scanner_pg;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;

namespace bioseq::parallelism::pg {

template <class F>
auto invoke_async_mps_materialization_pg_leaf(
        AsyncMpsWorkerPgFailure point,
        F&& function)
    -> std::invoke_result_t<std::remove_reference_t<F>&> {
    record_async_mps_worker_pg_leaf_call(point);
    try {
        return bioseq::pg_utils::invoke_pg_as_cpp(
            std::forward<F>(function));
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        record_async_mps_worker_pg_leaf_failure(point);
        throw;
    }
}

template <sequence_alphabet Alphabet>
struct AsyncMpsResidentTarget {
    bioseq::storage::admitted_packed_region<Alphabet> owner;
    std::uint64_t packed_bytes = 0;
};

template <sequence_alphabet Alphabet>
[[nodiscard]] std::optional<AsyncMpsResidentTarget<Alphabet>>
try_materialize_async_mps_worker_target(
        bioseq::storage::basic_lazy_sequence_view<Alphabet> seq,
        std::uint8_t requested_width,
        std::uint64_t sequence_rowno,
        std::string_view index_name) {
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    if (!bioseq::parallelism::task_parallel(pool.mode())
        || pool.max_concurrency() <= 1) {
        return std::nullopt;
    }
    const auto access = bioseq::storage::plan_sequence_access(
        seq,
        bioseq::storage::sequence_access_requirement::contiguous_resident,
        0u,
        seq.bp_size(),
        requested_width);
    if (access.route
        == bioseq::storage::sequence_access_route::borrow_resident) {
        return std::nullopt;
    }
    constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
    const std::uint64_t reserve_bytes = access.admission_bytes
            > (std::numeric_limits<std::uint64_t>::max)()
                - context_reserve_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : access.admission_bytes + context_reserve_bytes;
    auto& admission =
        bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t reserved_before = admission.reserved_bytes();
    auto permit = admission.try_reserve(reserve_bytes);
    if (!permit) {
        bioseq_log<DBG>(
            "[bioseq] index: async mps worker row={} index={} target_bp={} "
            "admitted_bytes={} not admitted for resident materialization; "
            "using rank stream",
            static_cast<unsigned long long>(sequence_rowno), index_name,
            static_cast<unsigned long long>(seq.bp_size()),
            static_cast<unsigned long long>(access.admission_bytes));
        return std::nullopt;
    }
    record_async_mps_materialize_admission();
    try {
        invoke_async_mps_materialization_pg_leaf(
            AsyncMpsWorkerPgFailure::materialize_error_log,
            [] {
                inject_async_mps_worker_pg_failure_leaf(
                    AsyncMpsWorkerPgFailure::materialize_error_log);
            });
        invoke_async_mps_materialization_pg_leaf(
            AsyncMpsWorkerPgFailure::materialize_interrupt,
            [] {
                inject_async_mps_worker_pg_failure_leaf(
                    AsyncMpsWorkerPgFailure::materialize_interrupt);
                CHECK_FOR_INTERRUPTS();
            });
        invoke_async_mps_materialization_pg_leaf(
            AsyncMpsWorkerPgFailure::materialize_scanner_read,
            [] {
                inject_async_mps_worker_pg_failure_leaf(
                    AsyncMpsWorkerPgFailure::materialize_scanner_read);
            });
        auto owner = bioseq::storage::materialize_packed_region(
            seq, 0u, seq.bp_size(), requested_width, std::move(*permit));
        const auto& descriptor = owner.descriptor();
        const std::uint64_t packed_bytes =
            static_cast<std::uint64_t>(descriptor.unit_count)
            * descriptor.storage_unit_byte_width;
        bioseq_log<DBG>(
            "[bioseq] index: async mps worker row={} index={} target_bp={} "
            "packed_bytes={} materialized resident target",
            static_cast<unsigned long long>(sequence_rowno), index_name,
            static_cast<unsigned long long>(seq.bp_size()),
            static_cast<unsigned long long>(packed_bytes));
        return AsyncMpsResidentTarget<Alphabet>{
            std::move(owner), packed_bytes};
    } catch (...) {
        permit->release();
        const std::uint64_t reserved_after = admission.reserved_bytes();
        record_async_mps_materialize_failure_unwind(
            reserved_after == reserved_before);
        throw;
    }
}

void process_mps_build_claim_cpp(
        const PersistClaim& claim,
        const char* base,
        Size mapped,
        Snapshot registered_snapshot) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const std::optional<MpsBuildMetadata> meta =
        parse_mps_build_metadata(base, mapped);
    if (!meta.has_value()) {
        bioseq_log<DBG>("[bioseq] index: discarded invalid async mps build metadata row={}",
                static_cast<unsigned long long>(claim.target.sequence_rowno));
        return;
    }
    if (meta->requested_provider != claim.requested_provider) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("asynchronous MPS provider identity disagrees across queue and DSM")));
        });
    }
    if (meta->requested_provider
            != IndexPayloadStorageBackend::postgres_relation) {
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("mapped_file payload storage does not support MPS persistence"),
                 errdetail("the queued job is rejected without provider fallback")));
        });
    }

    PersistTarget target = claim.target;
    target.index_name = meta->index_name;
    if (!worker_target_accepts_index_persist(target, meta->index_name)) {
        bioseq_log<DBG>("[bioseq] index: discarded stale async mps build job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name);
        return;
    }

    const auto worker_backend =
        static_cast<bioseq::minimizers::MinimizerSketchBackend>(
            meta->index_sketch_backend);

    MemoryContext work = nullptr;
    bioseq::storage::seq_caches* caches = nullptr;

    const auto previous_sort_override =
        bioseq::indexing::detail::current_pair_run_sort_override();
    bool sort_impl_changed = false;
    const auto worker_mode =
        bioseq::parallelism::mode_from_int(meta->pipeline_mode);
    const unsigned worker_concurrency = effective_max_concurrency(
        static_cast<unsigned>(std::max(0, meta->max_concurrency)));
    const std::uint64_t worker_index_cap_bytes =
        meta->index_memory_cap_mb > 0
            ? (static_cast<std::uint64_t>(meta->index_memory_cap_mb) << 20)
            : 0;

    bool built = false;
    bool lazy_worker_shareable = false;
    bioseq_log<DBG>("[bioseq] index: async mps build worker starting row={} index={} target_bp={} mode={} max_concurrency={} index_memory_cap_mb={} sort_impl={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(meta->locator.total_bp_size),
            std::string_view(bioseq::parallelism::to_string(worker_mode)),
            static_cast<unsigned long long>(worker_concurrency),
            meta->index_memory_cap_mb,
            meta->pair_sort_impl);
    bioseq_log<DBG>(
        "[bioseq] index: async mps build worker backend={}",
        bioseq::minimizers::minimizer_sketch_backend_name(worker_backend));
    const auto abandon_pg_resources_noexcept = [&]() noexcept {
        if (caches != nullptr) {
            // PostgreSQL owns every relation/read-state after an ERROR. Null
            // those borrowed handles, but still run the vector destructor so
            // its ordinary heap allocation cannot leak until backend exit.
            for (auto& [key, entry] : caches->entries) {
                (void)key;
                entry.rel = nullptr;
                entry.write_state = nullptr;
                entry.read_state = nullptr;
                entry.read_snapshot_source = nullptr;
                entry.pending_bytes_delta = 0;
                entry.pending_total_payload_delta = 0;
            }
            caches->entries.clear();
            caches->~seq_caches();
            caches = nullptr;
        }
        // The transaction owns the palloc block and child context.
        work = nullptr;
    };
    try {
        work = bioseq::pg_utils::invoke_pg_as_cpp([] {
            return AllocSetContextCreate(
                CurrentMemoryContext,
                "bioseq_async_mps_build",
                ALLOCSET_DEFAULT_SIZES);
        });
        caches = bioseq::pg_utils::invoke_pg_as_cpp(
            [work] { return bioseq::storage::alloc_seq_caches(work); });
        caches->side = bioseq::storage::seq_caches::Side::Read;
        caches->lock_mode = AccessShareLock;
        caches->read_state_mctx = work;
        caches->main_database_oid = target.db_oid;
        caches->main_tablespace_oid = target.tablespace_oid;
        caches->main_relfilenode = target.main_relfilenode;

        bioseq::indexing::detail::set_pair_run_sort_impl(
            static_cast<bioseq::indexing::detail::PairRunSortImpl>(
                meta->pair_sort_impl));
        sort_impl_changed = true;
        bioseq::parallelism::GlobalPool::instance().configure(
            bioseq::parallelism::PoolConfig{
                .max_concurrency = worker_concurrency,
                .mode = worker_mode,
            });
        bioseq::parallelism::MemoryAdmissionController::instance()
            .set_index_memory_cap_bytes(worker_index_cap_bytes);

        if (worker_backend
            == bioseq::minimizers::MinimizerSketchBackend::PaperNtHashSimd) {
            bioseq::pg_utils::invoke_pg_as_cpp([] {
                ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("asynchronous paper_nthash_simd persistence requires the scheme-tagged READY-generation format"),
                     errdetail("legacy MPS payloads do not record a minimizer-scheme identity; the queued build is rejected without fallback")));
            });
        }
        bioseq::pg_types::seq::dispatch_instance(
            meta->alphabet_code,
            meta->storage_unit_byte_width,
            [&]<class S>() {
                auto store = bioseq::storage::indexing::open_fragment_storage(
                    target.user_relid,
                    target.db_oid,
                    target.tablespace_oid,
                    target.main_relfilenode,
                    target.column_attno,
                    meta->index_name,
                    meta->locator,
                    target.sequence_rowno,
                    caches,
                    work);
                try {
                    const bioseq::storage::postgres_lazy_sequence_binding<S>
                        source_binding{
                            target.db_oid, target.tablespace_oid,
                            target.main_relfilenode, target.column_attno,
                            meta->locator, meta->last_fragment_padding,
                            registered_snapshot, caches, work};
                    bioseq::storage::with_lazy_postgres<S>(
                        source_binding,
                        [&lazy_worker_shareable, meta, &target, &store,
                         worker_backend](auto source_view) {
                            const bool pg_lazy_worker_shareable =
                                bioseq::storage::lazy_is_worker_shareable(
                                    source_view);
                            lazy_worker_shareable = pg_lazy_worker_shareable;
                            if (!pg_lazy_worker_shareable) {
                                if (auto resident =
                                        try_materialize_async_mps_worker_target(
                                            source_view,
                                            meta->storage_unit_byte_width,
                                            target.sequence_rowno,
                                            meta->index_name);
                                    resident.has_value()) {
                                    const auto resident_view =
                                        resident->owner.view();
                                    lazy_worker_shareable =
                                        bioseq::storage::lazy_is_worker_shareable(
                                            resident_view);
                                    bioseq_log<DBG>("[bioseq] index: async mps worker row={} index={} "
                                            "using resident target packed_bytes={} "
                                            "lazy_worker_shareable={}",
                                            static_cast<unsigned long long>(
                                                target.sequence_rowno),
                                            meta->index_name,
                                            static_cast<unsigned long long>(
                                                resident->packed_bytes),
                                            lazy_worker_shareable);
                                    bioseq::indexing::MinimizerSequenceMergedIndex<
                                        bioseq::storage::indexing::IndexStorageFragmentPg>::rebuild(
                                            store, resident_view, worker_backend,
                                            nullptr,
                                            meta->alphabet_code);
                                    return;
                                }
                            }
                            bioseq_log<DBG>("[bioseq] index: async mps worker row={} index={} "
                                    "using scanner_pg target lazy_worker_shareable={}",
                                    static_cast<unsigned long long>(
                                        target.sequence_rowno),
                                    meta->index_name,
                                    lazy_worker_shareable);
                            bioseq::indexing::MinimizerSequenceMergedIndex<
                                bioseq::storage::indexing::IndexStorageFragmentPg>::rebuild(
                                    store, source_view, worker_backend,
                                    nullptr,
                                    meta->alphabet_code);
                        });
                    store.close_checked();
                } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
                    store.abandon_after_pg_error_noexcept();
                    throw;
                } catch (...) {
                    try {
                        store.close_checked();
                    } catch (...) {
                        store.abandon_after_pg_error_noexcept();
                    }
                    throw;
                }
            });
        built = true;
        bioseq::pg_utils::invoke_pg_as_cpp(
            [caches] { caches->close_all(/*commit=*/false); });
        bioseq::storage::seq_caches* finished_caches =
            std::exchange(caches, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_caches] {
                bioseq::storage::destroy_seq_caches(finished_caches);
            });
        MemoryContext finished_work = std::exchange(work, nullptr);
        bioseq::pg_utils::invoke_pg_as_cpp(
            [finished_work] { MemoryContextDelete(finished_work); });
        bioseq::indexing::detail::restore_pair_run_sort_override(
            previous_sort_override);
        sort_impl_changed = false;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        if (sort_impl_changed) {
            bioseq::indexing::detail::restore_pair_run_sort_override(
                previous_sort_override);
            sort_impl_changed = false;
        }
        abandon_pg_resources_noexcept();
        throw;
    } catch (...) {
        if (sort_impl_changed) {
            bioseq::indexing::detail::restore_pair_run_sort_override(
                previous_sort_override);
            sort_impl_changed = false;
        }
        abandon_pg_resources_noexcept();
        throw;
    }

    if (built) {
        bioseq::storage::indexing::
            mark_serialized_index_write_successful_pg(
                target.user_relid,
                target.main_relfilenode,
                target.column_attno,
                meta->index_name);
    }

    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const auto stats = bioseq::storage::indexing::g_last_mps_build_stats;
    publish_async_mps_build_stats(
        stats,
        worker_end_ns - worker_start_ns,
        static_cast<std::uint64_t>(worker_mode),
        static_cast<std::uint64_t>(worker_concurrency),
        static_cast<std::uint64_t>(std::max(0, meta->index_memory_cap_mb)),
        lazy_worker_shareable);
    bioseq_log<DBG>("[bioseq] index: async mps build worker built row={} index={} target_bp={} worker_build_ns={} mode={} max_concurrency={} lazy_worker_shareable={} runs={} resident_runs={} file_runs={} resident_peak_bytes={} spilled_run_bytes={} postgres_temp_file_bytes={} merge_passes={} position_spill_bytes={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(meta->locator.total_bp_size),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns),
            std::string_view(bioseq::parallelism::to_string(worker_mode)),
            static_cast<unsigned long long>(worker_concurrency),
            lazy_worker_shareable,
            static_cast<unsigned long long>(stats.total_run_count),
            static_cast<unsigned long long>(stats.resident_runs),
            static_cast<unsigned long long>(stats.file_runs),
            static_cast<unsigned long long>(stats.resident_peak_bytes),
            static_cast<unsigned long long>(stats.spilled_run_bytes),
            static_cast<unsigned long long>(stats.postgres_temp_file_bytes),
            static_cast<unsigned long long>(stats.merge_pass_count),
            static_cast<unsigned long long>(stats.position_spill_bytes));
}

// Trivial PostgreSQL ABI seam. The implementation above has already unwound
// every C++ owner and restored sort/admission state before this function
// rethrows the exact ErrorData or creates one terminal diagnostic.
void process_mps_build_claim(
        const PersistClaim& claim,
        const char* base,
        Size mapped,
        Snapshot registered_snapshot) {
    ErrorData* exact_error = nullptr;
    char cpp_error_detail[512] = "unknown C++ exception";
    try {
        process_mps_build_claim_cpp(
            claim, base, mapped, registered_snapshot);
        return;
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        exact_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(cpp_error_detail,
            error.what() == nullptr ? "std::exception without detail"
                                    : error.what(),
            sizeof(cpp_error_detail));
    } catch (...) {
    }
    if (exact_error != nullptr) {
        ReThrowError(exact_error);
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("asynchronous MPS build raised a C++ exception"),
         errdetail_internal("%s", cpp_error_detail)));
    pg_unreachable();
}

} // namespace bioseq::parallelism::pg
