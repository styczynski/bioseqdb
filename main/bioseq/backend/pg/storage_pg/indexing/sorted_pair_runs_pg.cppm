/**
 * @file sorted_pair_runs_pg.cppm
 * @brief PostgreSQL BufFile-backed sorted `(kmer, encoded)` run store for
 *        minimizers_per_sequence builds.
 */
module;
#include "common.h"

extern "C" {
#include "storage/buffile.h"

// Keep this leaf entirely PCM-free.  Importing either this module or mlogging
// from its translation unit makes Clang 22 try to emit a deferred libc++
// allocation helper at EOF and crash in EmitBuiltinNewDeleteCall.
[[noreturn]] void bioseq_sorted_run_error_pg_leaf(unsigned error_code);
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.storage.indexing_pg.sorted_runs;

import bioseq.pg_utils;
import bioseq.parallelism.memory_admission;

export namespace bioseq::storage::indexing {

using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

struct PositionBatchSink {
    void* ctx = nullptr;
    void (*call)(void*, std::span<const std::uint64_t>) = nullptr;
};

class SortedPairPositionCursor {
public:
    SortedPairPositionCursor() noexcept = default;
    explicit SortedPairPositionCursor(const void* impl) noexcept
        : impl_(impl)
    {}

    [[nodiscard]] std::uint64_t total_count() const noexcept;
    void for_each_batch_erased(PositionBatchSink sink) const;

    template <class F>
    void for_each_batch(F&& f) const {
        PositionBatchSink sink{
            .ctx = std::addressof(f),
            .call = [](void* ctx, std::span<const std::uint64_t> batch) {
                (*static_cast<std::remove_reference_t<F>*>(ctx))(batch);
            },
        };
        for_each_batch_erased(sink);
    }

private:
    friend class SortedPairRunStore;
    const void* impl_ = nullptr;
};

struct SortedPairKmerSink {
    void* ctx = nullptr;
    void (*call)(void*, std::uint64_t, const SortedPairPositionCursor&) = nullptr;
};

struct SortedRunStoreStats {
    std::uint64_t resident_runs = 0;
    std::uint64_t file_runs = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t resident_peak_bytes = 0;
    std::uint64_t spilled_bytes = 0;
    std::uint64_t postgres_temp_file_bytes = 0;
    std::uint64_t postgres_temp_file_count = 0;
    std::uint64_t merge_reader_buffer_bytes = 0;
    std::uint64_t merge_pass_count = 0;
    std::uint64_t spill_count_from_pressure = 0;
    std::uint64_t resident_demotions = 0;
    std::uint64_t position_spill_bytes = 0;
    std::uint64_t position_spill_count = 0;
};

#ifdef BIOSEQ_TEST_INDEXES
/**
 * Deterministic one-shot leaf failures used by PostgreSQL integration tests.
 * "after" points model the important partial-success case: the BufFile state
 * has changed, but the C++ caller must still poison/abandon its handles.
 */
enum class SortedRunPgLeafFailure : std::uint8_t {
    none,
    create_before,
    create_after,
    write_before,
    write_after,
    read_before,
    read_after,
    seek_before,
    seek_after,
    size_before,
    size_after,
    close_before,
    close_after,
    interrupt_before,
    interrupt_after,
    // Dedicated compaction point: unlike `create_before`, this does not count
    // the input spill files created before a merge pass starts.
    compact_create_before,
};

void arm_sorted_run_pg_leaf_failure(SortedRunPgLeafFailure point) noexcept;
void arm_sorted_run_pg_leaf_failure_on_nth(
    SortedRunPgLeafFailure point,
    std::uint64_t operation) noexcept;
void set_sorted_run_merge_reader_budget_for_test(
    std::uint64_t bytes) noexcept;
void disarm_sorted_run_pg_leaf_failure() noexcept;
[[nodiscard]] bool sorted_run_pg_leaf_failure_is_armed() noexcept;
#endif

} // namespace bioseq::storage::indexing

namespace bioseq::storage::indexing {

struct SortedPairWire {
    std::uint64_t kmer = 0;
    std::uint64_t encoded = 0;
};
static_assert(sizeof(SortedPairWire) == 16);
static_assert(alignof(SortedPairWire) == alignof(std::uint64_t));
static_assert(offsetof(SortedPairWire, kmer) == 0);
static_assert(offsetof(SortedPairWire, encoded) == 8);

inline constexpr std::uint64_t kTargetPositionBatchBytes = std::uint64_t{8} << 20;
inline constexpr std::uint64_t kMinPositionBatchBytes = std::uint64_t{32} << 10;

enum class SortedRunError : unsigned {
    // Values are the private C-leaf ABI used by sorted_pair_runs_pg.cpp.
    position_spill_create_failed = 0u,
    position_spill_rewind_failed = 1u,
    position_buffer_admission_failed = 2u,
    temp_file_create_failed = 3u,
    admitted_run_without_permit = 4u,
    run_rewind_failed = 5u,
    merge_reader_admission_failed = 6u,
    compacted_run_rewind_failed = 7u,
    compact_temp_file_create_failed = 8u,
    poisoned_store_reused = 9u,
    finalized_store_reused = 10u,
};

[[noreturn]] void report_sorted_run_error(SortedRunError error) {
    const unsigned error_code = static_cast<unsigned>(error);
    bioseq::pg_utils::invoke_pg_as_cpp([error_code] {
        bioseq_sorted_run_error_pg_leaf(error_code);
    });
    // The leaf raises PostgreSQL ERROR, which the bridge converts to a C++
    // exception.  Keep the noreturn contract fail-closed if a logging backend
    // ever violates that invariant.
    __builtin_trap();
}

#ifdef BIOSEQ_TEST_INDEXES
inline thread_local SortedRunPgLeafFailure g_sorted_run_pg_leaf_failure =
    SortedRunPgLeafFailure::none;
inline thread_local std::uint64_t g_sorted_run_pg_leaf_failure_remaining = 0;
inline thread_local std::uint64_t
    g_sorted_run_merge_reader_budget_for_test = 0;

void arm_sorted_run_pg_leaf_failure(SortedRunPgLeafFailure point) noexcept {
    arm_sorted_run_pg_leaf_failure_on_nth(point, 1u);
}

void arm_sorted_run_pg_leaf_failure_on_nth(
        SortedRunPgLeafFailure point,
        std::uint64_t operation) noexcept {
    g_sorted_run_pg_leaf_failure = point;
    g_sorted_run_pg_leaf_failure_remaining =
        point == SortedRunPgLeafFailure::none
            ? 0u
            : std::max<std::uint64_t>(1u, operation);
}

void set_sorted_run_merge_reader_budget_for_test(
        std::uint64_t bytes) noexcept {
    g_sorted_run_merge_reader_budget_for_test = bytes;
}

void disarm_sorted_run_pg_leaf_failure() noexcept {
    g_sorted_run_pg_leaf_failure = SortedRunPgLeafFailure::none;
    g_sorted_run_pg_leaf_failure_remaining = 0;
    g_sorted_run_merge_reader_budget_for_test = 0;
}

bool sorted_run_pg_leaf_failure_is_armed() noexcept {
    return g_sorted_run_pg_leaf_failure != SortedRunPgLeafFailure::none;
}

void inject_sorted_run_pg_leaf_failure(SortedRunPgLeafFailure point) {
    if (g_sorted_run_pg_leaf_failure != point) {
        return;
    }
    if (g_sorted_run_pg_leaf_failure_remaining > 1u) {
        --g_sorted_run_pg_leaf_failure_remaining;
        return;
    }
    // One-shot reset is deliberately before ERROR so a follow-up statement can
    // prove recovery rather than repeatedly tripping the same test hook.
    g_sorted_run_pg_leaf_failure = SortedRunPgLeafFailure::none;
    g_sorted_run_pg_leaf_failure_remaining = 0;
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("injected sorted-run PostgreSQL leaf failure"),
         errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

/** Error/unwind cleanup: drop only the C++ handle; ResourceOwner owns file. */
void abandon_buffile_noexcept(BufFile*& file) noexcept {
    file = nullptr;
}

[[nodiscard]] BufFile* create_temp_buffile_as_cpp() {
    return bioseq::pg_utils::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::create_before);
#endif
        BufFile* result = BufFileCreateTemp(false);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::create_after);
#endif
        return result;
    });
}

void write_buffile_as_cpp(
        BufFile* file,
        const void* data,
        std::size_t bytes) {
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::write_before);
#endif
        BufFileWrite(file, data, bytes);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::write_after);
#endif
    });
}

void read_buffile_exact_as_cpp(
        BufFile* file,
        void* data,
        std::size_t bytes) {
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::read_before);
#endif
        BufFileReadExact(file, data, bytes);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::read_after);
#endif
    });
}

[[nodiscard]] int seek_buffile_as_cpp(
        BufFile* file,
        int fileno,
        off_t offset,
        int whence) {
    return bioseq::pg_utils::invoke_pg_as_cpp([&] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::seek_before);
#endif
        const int result = BufFileSeek(file, fileno, offset, whence);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::seek_after);
#endif
        return result;
    });
}

[[nodiscard]] int64 size_buffile_as_cpp(BufFile* file) {
    return bioseq::pg_utils::invoke_pg_as_cpp([&] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::size_before);
#endif
        const int64 result = BufFileSize(file);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::size_after);
#endif
        return result;
    });
}

void check_for_interrupts_as_cpp() {
    bioseq::pg_utils::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::interrupt_before);
#endif
        CHECK_FOR_INTERRUPTS();
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::interrupt_after);
#endif
    });
}

/** Success-only close.  Clear the handle before entering PostgreSQL. */
void close_buffile_checked_as_cpp(BufFile*& file) {
    BufFile* closing = std::exchange(file, nullptr);
    if (closing == nullptr) {
        return;
    }
    bioseq::pg_utils::invoke_pg_as_cpp([&] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::close_before);
#endif
        BufFileClose(closing);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::close_after);
#endif
    });
}

struct MemoryRun {
    // Reverse member destruction is part of the accounting contract: pair
    // capacity must disappear before the reservation becomes reusable.
    bioseq::parallelism::MemoryPermit permit;
    std::uint64_t admitted_bytes = 0;
    std::vector<sorted_pair_t> pairs;
};

struct BufFileRun {
    BufFile* file = nullptr;
    std::uint64_t pair_count = 0;
    std::uint64_t byte_count = 0;
};

template <class T>
struct AdmittedOwnedVector {
    // Reverse destruction drops vector capacity before its reservation.
    bioseq::parallelism::MemoryPermit permit;
    std::vector<T> values;
};

template <class T>
[[nodiscard]] bool try_ensure_admitted_vector_capacity(
        std::vector<T>& values,
        bioseq::parallelism::MemoryPermit& permit,
        std::size_t required_capacity) {
    const std::uint64_t existing_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(T)));
    if (existing_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (values.capacity() >= required_capacity) {
        return existing_bytes == 0u
            || (permit && permit.bytes() >= existing_bytes);
    }
    const std::uint64_t logical_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(required_capacity),
        static_cast<std::uint64_t>(sizeof(T)));
    const std::uint64_t admitted_headroom = bioseq::parallelism::saturating_mul(
        logical_bytes,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    const std::uint64_t allocation_peak = bioseq::parallelism::saturating_add(
        existing_bytes, admitted_headroom);
    if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || admitted_headroom == (std::numeric_limits<std::uint64_t>::max)()
        || allocation_peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (permit) {
        if (permit.bytes() < existing_bytes
            || !permit.grow_to(allocation_peak)) {
            return false;
        }
    } else {
        if (existing_bytes != 0u) {
            return false;
        }
        auto acquired = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(allocation_peak);
        if (!acquired) {
            return false;
        }
        permit = std::move(*acquired);
    }
    values.reserve(required_capacity);
    const std::uint64_t exact_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()),
        static_cast<std::uint64_t>(sizeof(T)));
    if (exact_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || exact_bytes > permit.bytes()) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    permit.shrink_to(exact_bytes);
    return true;
}

template <class T>
[[nodiscard]] bool try_ensure_admitted_vector_capacity(
        AdmittedOwnedVector<T>& owned,
        std::size_t required_capacity) {
    return try_ensure_admitted_vector_capacity(
        owned.values, owned.permit, required_capacity);
}

struct FileReader {
    BufFile* file = nullptr;
    std::uint64_t remaining = 0;
    std::vector<SortedPairWire> buffer;
    std::size_t offset = 0;
    std::size_t available = 0;
    SortedPairWire current{};

    [[nodiscard]] bool read_next() {
        if (offset >= available) {
            if (remaining == 0) {
                return false;
            }
            const std::uint64_t cap = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(buffer.capacity()));
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(remaining, cap));
            buffer.resize(count);
            read_buffile_exact_as_cpp(
                file, buffer.data(), count * sizeof(SortedPairWire));
            remaining -= count;
            offset = 0;
            available = count;
        }
        current = buffer[offset++];
        return true;
    }
};

struct PositionAccumulator {
    // Permit first => default reverse destruction drops both buffers first.
    bioseq::parallelism::MemoryPermit resident_permit;
    std::vector<std::uint64_t> resident;
    std::vector<std::uint64_t> read_buffer;
    BufFile* spill = nullptr;
    std::uint64_t total = 0;
    std::uint64_t spilled_bytes = 0;
    bool resident_batch_ready = false;

    void reset_for_kmer() {
        resident.clear();
        read_buffer.clear();
        if (spill != nullptr) {
            close_buffile_checked_as_cpp(spill);
        }
        total = 0;
        spilled_bytes = 0;
    }

    void ensure_resident_batch() {
        if (resident_batch_ready) {
            return;
        }
        auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
        std::uint64_t per_buffer_bytes = kTargetPositionBatchBytes;
        const std::uint64_t available = admission.allocatable_after_reservations();
        if (available != (std::numeric_limits<std::uint64_t>::max)()) {
            constexpr std::uint64_t total_headroom_factor =
                2u * bioseq::parallelism::kVectorPreallocationHeadroomFactor;
            per_buffer_bytes = std::min(
                per_buffer_bytes, available / total_headroom_factor);
        }
        per_buffer_bytes -= per_buffer_bytes % sizeof(std::uint64_t);
        if (per_buffer_bytes < kMinPositionBatchBytes) {
            report_sorted_run_error(
                SortedRunError::position_buffer_admission_failed);
        }
        for (;;) {
            constexpr std::uint64_t total_headroom_factor =
                2u * bioseq::parallelism::kVectorPreallocationHeadroomFactor;
            if (per_buffer_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        / total_headroom_factor) {
                report_sorted_run_error(
                    SortedRunError::position_buffer_admission_failed);
            }
            // Reserve both buffers plus the accepted vector-capacity headroom
            // before either std::vector::reserve may allocate.
            const std::uint64_t requested_total_bytes =
                per_buffer_bytes * total_headroom_factor;
            if (auto permit = admission.try_reserve(requested_total_bytes)) {
                struct CandidateBuffers {
                    bioseq::parallelism::MemoryPermit permit;
                    std::vector<std::uint64_t> resident;
                    std::vector<std::uint64_t> read;
                } candidate{
                    .permit = std::move(*permit),
                    .resident = {},
                    .read = {},
                };
                const std::size_t units = static_cast<std::size_t>(
                    per_buffer_bytes / sizeof(std::uint64_t));
                candidate.resident.reserve(units);
                candidate.read.reserve(units);

                const auto capacity_bytes = [](std::size_t capacity)
                        -> std::optional<std::uint64_t> {
                    constexpr std::uint64_t max =
                        (std::numeric_limits<std::uint64_t>::max)();
                    if (capacity > max / sizeof(std::uint64_t)) {
                        return std::nullopt;
                    }
                    return static_cast<std::uint64_t>(capacity)
                        * sizeof(std::uint64_t);
                };
                const auto resident_bytes = capacity_bytes(
                    candidate.resident.capacity());
                const auto read_bytes = capacity_bytes(candidate.read.capacity());
                if (!resident_bytes || !read_bytes
                    || *resident_bytes
                        > (std::numeric_limits<std::uint64_t>::max)()
                            - *read_bytes) {
                    bioseq::parallelism::
                        release_admitted_vector_capacities_before_permit(
                            candidate.resident, candidate.read, candidate.permit);
                    report_sorted_run_error(
                        SortedRunError::position_buffer_admission_failed);
                }
                const std::uint64_t exact_total_bytes =
                    *resident_bytes + *read_bytes;
                if (exact_total_bytes > candidate.permit.bytes()) {
                    bioseq::parallelism::
                        release_admitted_vector_capacities_before_permit(
                            candidate.resident, candidate.read, candidate.permit);
                    report_sorted_run_error(
                        SortedRunError::position_buffer_admission_failed);
                }
                candidate.permit.shrink_to(exact_total_bytes);
                resident = std::move(candidate.resident);
                read_buffer = std::move(candidate.read);
                resident_permit = std::move(candidate.permit);
                resident_batch_ready = true;
                return;
            }
            if (per_buffer_bytes == kMinPositionBatchBytes) {
                break;
            }
            per_buffer_bytes = std::max(
                per_buffer_bytes / 2u, kMinPositionBatchBytes);
            per_buffer_bytes -= per_buffer_bytes % sizeof(std::uint64_t);
        }
        report_sorted_run_error(
            SortedRunError::position_buffer_admission_failed);
    }

    void push_unique_encoded(std::uint64_t encoded) {
        ensure_resident_batch();
        if (resident.size() == resident.capacity()) {
            if (spill == nullptr) {
                spill = create_temp_buffile_as_cpp();
                if (spill == nullptr) {
                    report_sorted_run_error(SortedRunError::position_spill_create_failed);
                }
            }
            write_buffile_as_cpp(
                spill,
                resident.data(),
                resident.size() * sizeof(std::uint64_t));
            spilled_bytes += resident.size() * sizeof(std::uint64_t);
            resident.clear();
        }
        resident.push_back(encoded);
        ++total;
    }

    void emit_batches(PositionBatchSink sink) {
        if (sink.call == nullptr) {
            return;
        }
        if (spill != nullptr) {
            if (seek_buffile_as_cpp(spill, 0, 0, SEEK_SET) != 0) {
                report_sorted_run_error(SortedRunError::position_spill_rewind_failed);
            }
            std::uint64_t remaining =
                spilled_bytes / static_cast<std::uint64_t>(sizeof(std::uint64_t));
            const std::uint64_t cap = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(read_buffer.capacity()));
            while (remaining != 0) {
                const std::size_t count = static_cast<std::size_t>(
                    std::min<std::uint64_t>(remaining, cap));
                read_buffer.resize(count);
                read_buffile_exact_as_cpp(
                    spill,
                    read_buffer.data(),
                    count * sizeof(std::uint64_t));
                sink.call(sink.ctx, std::span<const std::uint64_t>(
                    read_buffer.data(), read_buffer.size()));
                remaining -= count;
            }
        }
        if (!resident.empty()) {
            sink.call(sink.ctx, std::span<const std::uint64_t>(
                resident.data(), resident.size()));
        }
    }

    void emit(std::uint64_t kmer, SortedPairKmerSink sink) {
        if (sink.call == nullptr) {
            return;
        }
        SortedPairPositionCursor cursor{this};
        sink.call(sink.ctx, kmer, cursor);
    }

    void finish_success() {
        if (spill != nullptr) {
            close_buffile_checked_as_cpp(spill);
        }
        bioseq::parallelism::release_admitted_vector_capacities_before_permit(
            resident, read_buffer, resident_permit);
        total = 0;
        spilled_bytes = 0;
        resident_batch_ready = false;
    }

    void abandon_pg_resources_noexcept() noexcept {
        bioseq::parallelism::release_admitted_vector_capacities_before_permit(
            resident, read_buffer, resident_permit);
        // A PostgreSQL ERROR may have interrupted a BufFile operation. Do not
        // issue another PG call while unwinding; ResourceOwner cleanup at the
        // outer statement/transaction boundary owns this file.
        abandon_buffile_noexcept(spill);
        total = 0;
        spilled_bytes = 0;
        resident_batch_ready = false;
    }
};

struct MergeResources {
    // Permit first => reader storage is destroyed before reservation release.
    bioseq::parallelism::MemoryPermit reader_budget_permit;
    std::vector<FileReader> readers;
    PositionAccumulator positions;

    void finish_success() {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            readers, reader_budget_permit);
        positions.finish_success();
    }


    void abandon_pg_resources_noexcept() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            readers, reader_budget_permit);
        positions.abandon_pg_resources_noexcept();
    }
};

struct MergeHead {
    std::uint64_t kmer = 0;
    std::uint64_t encoded = 0;
    std::size_t run_index = 0;
    std::size_t pair_index = 0;
    bool file = false;
};

struct MergeHeadGreater {
    bool operator()(const MergeHead& lhs, const MergeHead& rhs) const noexcept {
        if (lhs.kmer != rhs.kmer) {
            return lhs.kmer > rhs.kmer;
        }
        return lhs.encoded > rhs.encoded;
    }
};

using MergeHeadQueue = std::priority_queue<
    MergeHead, std::vector<MergeHead>, MergeHeadGreater>;

struct AdmittedMergeHeadQueue {
    // Queue capacity is destroyed before its permit by reverse order.
    bioseq::parallelism::MemoryPermit permit;
    MergeHeadQueue queue;

    AdmittedMergeHeadQueue() = default;
    AdmittedMergeHeadQueue(const AdmittedMergeHeadQueue&) = delete;
    AdmittedMergeHeadQueue& operator=(const AdmittedMergeHeadQueue&) = delete;
    AdmittedMergeHeadQueue(AdmittedMergeHeadQueue&& other) noexcept {
        // Transfer queue storage while the source reservation is still live;
        // only then move the permit.  This also makes a non-elided return safe.
        queue.swap(other.queue);
        permit = std::move(other.permit);
    }
    AdmittedMergeHeadQueue& operator=(AdmittedMergeHeadQueue&&) = delete;
};

[[nodiscard]] AdmittedMergeHeadQueue make_admitted_merge_head_queue(
        std::size_t maximum_heads) {
    const std::uint64_t logical_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(maximum_heads),
        static_cast<std::uint64_t>(sizeof(MergeHead)));
    const std::uint64_t admitted_headroom = bioseq::parallelism::saturating_mul(
        logical_bytes,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || admitted_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(admitted_headroom);
    if (!permit) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    std::vector<MergeHead> backing;
    backing.reserve(maximum_heads);
    const std::uint64_t exact_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(backing.capacity()),
        static_cast<std::uint64_t>(sizeof(MergeHead)));
    if (exact_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || exact_bytes > permit->bytes()) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    permit->shrink_to(exact_bytes);
    // Construct the potentially-throwing queue while the local permit still
    // owns the reservation.  The final swaps/move are non-throwing.
    MergeHeadQueue prepared{MergeHeadGreater{}, std::move(backing)};
    AdmittedMergeHeadQueue admitted;
    admitted.queue.swap(prepared);
    admitted.permit = std::move(*permit);
    return admitted;
}

[[nodiscard]] std::uint64_t file_reader_owned_capacity_bytes(
        const std::vector<FileReader>& readers) noexcept {
    std::uint64_t bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(readers.capacity()),
        static_cast<std::uint64_t>(sizeof(FileReader)));
    for (const FileReader& reader : readers) {
        bytes = bioseq::parallelism::saturating_add(
            bytes,
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(reader.buffer.capacity()),
                static_cast<std::uint64_t>(sizeof(SortedPairWire))));
    }
    return bytes;
}

} // namespace bioseq::storage::indexing

export namespace bioseq::storage::indexing {

class SortedPairRunStore {
public:
    SortedPairRunStore() = default;
    SortedPairRunStore(const SortedPairRunStore&) = delete;
    SortedPairRunStore& operator=(const SortedPairRunStore&) = delete;
    SortedPairRunStore(SortedPairRunStore&& other) noexcept { steal_(std::move(other)); }
    SortedPairRunStore& operator=(SortedPairRunStore&& other) noexcept {
        if (this != &other) {
            release_runs_noexcept();
            steal_(std::move(other));
        }
        return *this;
    }
    ~SortedPairRunStore() { release_runs_noexcept(); }

    [[nodiscard]] std::uint64_t run_count() const noexcept {
        return static_cast<std::uint64_t>(memory_runs_.size() + file_runs_.size());
    }
    [[nodiscard]] std::uint64_t file_run_count() const noexcept {
        return static_cast<std::uint64_t>(file_runs_.size());
    }
    [[nodiscard]] std::uint64_t resident_run_bytes() const noexcept {
        return memory_run_bytes_;
    }
    [[nodiscard]] std::uint64_t spilled_run_bytes() const noexcept {
        return spilled_run_bytes_;
    }
    [[nodiscard]] SortedRunStoreStats stats() const noexcept {
        SortedRunStoreStats out = stats_;
        out.resident_runs = static_cast<std::uint64_t>(memory_runs_.size());
        out.file_runs = static_cast<std::uint64_t>(file_runs_.size());
        out.resident_bytes = memory_run_bytes_;
        out.resident_peak_bytes = memory_run_peak_bytes_;
        out.spilled_bytes = spilled_run_bytes_;
        return out;
    }

    [[nodiscard]] std::size_t spill_sorted_run(std::span<const sorted_pair_t> sorted);
    [[nodiscard]] std::size_t spill_sorted_run_owned(std::vector<sorted_pair_t> sorted);
	    [[nodiscard]] std::size_t spill_sorted_run_owned_admitted(
	        std::vector<sorted_pair_t> sorted,
	        std::uint64_t admitted_bytes,
	        bioseq::parallelism::MemoryPermit permit);
	    void demote_resident_runs_until_available(std::uint64_t bytes_needed);

	    template <class F>
	    void merge_runs(F&& on_kmer) {
        SortedPairKmerSink sink{
            .ctx = std::addressof(on_kmer),
            .call = [](void* ctx,
                       std::uint64_t kmer,
                       const SortedPairPositionCursor& cursor) {
                (*static_cast<std::remove_reference_t<F>*>(ctx))(kmer, cursor);
            },
        };
        merge_runs_erased(sink);
    }

    void release_resident_runs_noexcept() noexcept;
    void release_runs_noexcept() noexcept;
    void release_runs();
    void abandon_after_pg_error_noexcept() noexcept {
        poisoned_ = true;
        release_runs_noexcept();
    }

	private:
	    void steal_(SortedPairRunStore&& other) noexcept;
	    void merge_runs_erased(SortedPairKmerSink sink);
	    void merge_runs_erased_impl_(SortedPairKmerSink sink, MergeResources& resources);
	    void finalize_file_handles_success_();
	    [[nodiscard]] bool try_ensure_write_buffer_(std::size_t requested_bytes);
	    [[nodiscard]] std::size_t spill_sorted_run_impl_(
            std::span<const sorted_pair_t> sorted);
	    [[nodiscard]] std::size_t spill_sorted_run_direct_(std::span<const sorted_pair_t> sorted);
	    void demote_resident_runs_until_available_impl_(std::uint64_t bytes_needed);
	    [[nodiscard]] bool demote_largest_resident_run_();
	    [[nodiscard]] std::uint64_t merge_reader_budget_bytes_() const;
	    void compact_file_runs_until_bounded_();
	    void compact_file_runs_to_output_count_(std::size_t target_output_runs);
    [[nodiscard]] BufFileRun merge_file_run_group_to_new_buffile_(std::size_t lo, std::size_t hi);
    template <class Writer>
    void merge_sorted_file_runs_to_writer_(
        std::span<BufFileRun> runs,
        MergeResources& resources,
        Writer&& writer);
    [[nodiscard]] bool try_open_file_readers_(MergeResources& resources);
    void open_file_readers_(MergeResources& resources);

    bioseq::parallelism::MemoryPermit memory_runs_metadata_permit_;
    std::vector<MemoryRun> memory_runs_;
    bioseq::parallelism::MemoryPermit file_runs_metadata_permit_;
    std::vector<BufFileRun> file_runs_;
    std::uint64_t memory_run_bytes_ = 0;
    std::uint64_t memory_run_peak_bytes_ = 0;
    std::uint64_t spilled_run_bytes_ = 0;
    // Permit first => default reverse destruction drops capacity first.
    bioseq::parallelism::MemoryPermit write_buffer_permit_;
    std::vector<SortedPairWire> write_buffer_;
    SortedRunStoreStats stats_{};
    bool poisoned_ = false;
    bool finalized_ = false;
};

} // namespace bioseq::storage::indexing

namespace bioseq::storage::indexing {

std::uint64_t SortedPairPositionCursor::total_count() const noexcept {
    const auto* acc = static_cast<const PositionAccumulator*>(impl_);
    return acc == nullptr ? 0 : acc->total;
}

void SortedPairPositionCursor::for_each_batch_erased(PositionBatchSink sink) const {
    auto* acc = const_cast<PositionAccumulator*>(
        static_cast<const PositionAccumulator*>(impl_));
    if (acc == nullptr) {
        return;
    }
    acc->emit_batches(sink);
}

void SortedPairRunStore::steal_(SortedPairRunStore&& other) noexcept {
    // The destination is capacity-free (default construction or checked
    // release before move assignment).  Swap first while the source permit is
    // still live, then transfer the permit; this leaves no unspecified
    // moved-from vector capacity behind without a reservation.
    memory_runs_.swap(other.memory_runs_);
    memory_runs_metadata_permit_ =
        std::move(other.memory_runs_metadata_permit_);
    file_runs_.swap(other.file_runs_);
    file_runs_metadata_permit_ =
        std::move(other.file_runs_metadata_permit_);
    memory_run_bytes_ = other.memory_run_bytes_;
    memory_run_peak_bytes_ = other.memory_run_peak_bytes_;
    spilled_run_bytes_ = other.spilled_run_bytes_;
    write_buffer_.swap(other.write_buffer_);
    write_buffer_permit_ = std::move(other.write_buffer_permit_);
    stats_ = other.stats_;
    poisoned_ = other.poisoned_;
    finalized_ = other.finalized_;

    other.file_runs_.clear();
    other.memory_run_bytes_ = 0;
    other.memory_run_peak_bytes_ = 0;
    other.spilled_run_bytes_ = 0;
    other.stats_ = {};
    other.poisoned_ = true;
    other.finalized_ = true;
}

	bool SortedPairRunStore::try_ensure_write_buffer_(std::size_t requested_bytes) {
	    if (write_buffer_permit_) {
	        return true;
	    }
	    requested_bytes = std::max<std::size_t>(requested_bytes, sizeof(SortedPairWire));
	    if (requested_bytes
	            > (std::numeric_limits<std::size_t>::max)()
	                - (sizeof(SortedPairWire) - 1u)) {
	        return false;
	    }
	    const std::uint64_t logical_bytes = static_cast<std::uint64_t>(requested_bytes);
	    const std::uint64_t admitted_headroom = bioseq::parallelism::saturating_mul(
	        logical_bytes,
	        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
	    if (admitted_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
	        return false;
	    }
	    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
	        .try_reserve(admitted_headroom);
	    if (!permit) {
	        return false;
	    }
	    struct CandidateWriteBuffer {
	        bioseq::parallelism::MemoryPermit permit;
	        std::vector<SortedPairWire> values;
	    } candidate{
	        .permit = std::move(*permit),
	        .values = {},
	    };
	    const std::size_t requested_pairs =
	        (requested_bytes + sizeof(SortedPairWire) - 1u)
	        / sizeof(SortedPairWire);
	    candidate.values.reserve(requested_pairs);
	    if (candidate.values.capacity()
	            > (std::numeric_limits<std::uint64_t>::max)()
	                / sizeof(SortedPairWire)) {
	        return false;
	    }
	    const std::uint64_t exact_bytes =
	        static_cast<std::uint64_t>(candidate.values.capacity())
	        * sizeof(SortedPairWire);
	    if (exact_bytes > candidate.permit.bytes()) {
	        return false;
	    }
	    candidate.permit.shrink_to(exact_bytes);
	    write_buffer_ = std::move(candidate.values);
	    write_buffer_permit_ = std::move(candidate.permit);
	    return true;
	}

	std::size_t SortedPairRunStore::spill_sorted_run_direct_(
	        std::span<const sorted_pair_t> sorted) {
	    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
	    if (!try_ensure_admitted_vector_capacity(
	            file_runs_, file_runs_metadata_permit_, file_runs_.size() + 1u)) {
	        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
	    }
	    BufFile* file = create_temp_buffile_as_cpp();
	    if (file == nullptr) {
	        report_sorted_run_error(SortedRunError::temp_file_create_failed);
	    }
	    file_runs_.push_back(BufFileRun{.file = file});
	    BufFileRun& run = file_runs_.back();

	    std::array<SortedPairWire, 1024> stack_buffer{};
	    for (std::size_t off = 0; off < sorted.size(); off += stack_buffer.size()) {
	        const std::size_t n = std::min(stack_buffer.size(), sorted.size() - off);
	        for (std::size_t i = 0; i < n; ++i) {
	            const sorted_pair_t& p = sorted[off + i];
	            stack_buffer[i] = SortedPairWire{.kmer = p.first, .encoded = p.second};
	        }
	        write_buffile_as_cpp(
            file, stack_buffer.data(), n * sizeof(SortedPairWire));
	        check_for_interrupts_as_cpp();
	    }

	    run.pair_count = static_cast<std::uint64_t>(sorted.size());
	    run.byte_count = static_cast<std::uint64_t>(
	        sorted.size() * sizeof(SortedPairWire));
	    spilled_run_bytes_ += run.byte_count;
	    ++stats_.postgres_temp_file_count;
	    const int64 size = size_buffile_as_cpp(file);
	    if (size > 0) {
	        stats_.postgres_temp_file_bytes += static_cast<std::uint64_t>(size);
	    }
	    stats_.file_runs = static_cast<std::uint64_t>(file_runs_.size());
	    stats_.spilled_bytes = spilled_run_bytes_;
	    return run_index;
	}

	std::size_t SortedPairRunStore::spill_sorted_run_impl_(
        std::span<const sorted_pair_t> sorted) {
	    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
	    if (!try_ensure_admitted_vector_capacity(
	            file_runs_, file_runs_metadata_permit_, file_runs_.size() + 1u)) {
	        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
	    }
	    BufFile* file = create_temp_buffile_as_cpp();
    if (file == nullptr) {
        report_sorted_run_error(SortedRunError::temp_file_create_failed);
    }
	    file_runs_.push_back(BufFileRun{.file = file});
	    BufFileRun& run = file_runs_.back();

	    if (!try_ensure_write_buffer_(std::size_t{8} << 20)) {
	        close_buffile_checked_as_cpp(file_runs_.back().file);
	        file_runs_.pop_back();
	        return spill_sorted_run_direct_(sorted);
	    }
	    const std::size_t write_pairs = std::max<std::size_t>(1, write_buffer_.capacity());
	    for (std::size_t off = 0; off < sorted.size(); off += write_pairs) {
	        const std::size_t n = std::min(write_pairs, sorted.size() - off);
        write_buffer_.clear();
        for (std::size_t i = 0; i < n; ++i) {
            const sorted_pair_t& p = sorted[off + i];
            write_buffer_.push_back(SortedPairWire{.kmer = p.first, .encoded = p.second});
        }
        write_buffile_as_cpp(
            file, write_buffer_.data(), n * sizeof(SortedPairWire));
        check_for_interrupts_as_cpp();
    }

    run.pair_count = static_cast<std::uint64_t>(sorted.size());
    run.byte_count = static_cast<std::uint64_t>(
        sorted.size() * sizeof(SortedPairWire));
    spilled_run_bytes_ += run.byte_count;
    ++stats_.postgres_temp_file_count;
    const int64 size = size_buffile_as_cpp(file);
    if (size > 0) {
        stats_.postgres_temp_file_bytes += static_cast<std::uint64_t>(size);
    }
    stats_.file_runs = static_cast<std::uint64_t>(file_runs_.size());
	    stats_.spilled_bytes = spilled_run_bytes_;
	    return run_index;
	}

std::size_t SortedPairRunStore::spill_sorted_run(
        std::span<const sorted_pair_t> sorted) {
    if (poisoned_) {
        report_sorted_run_error(SortedRunError::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(SortedRunError::finalized_store_reused);
    }
    poisoned_ = true;
    const std::size_t run_index = spill_sorted_run_impl_(sorted);
    poisoned_ = false;
    return run_index;
}

	bool SortedPairRunStore::demote_largest_resident_run_() {
	    if (memory_runs_.empty()) {
	        return false;
	    }
	    std::size_t best = 0;
	    for (std::size_t i = 1; i < memory_runs_.size(); ++i) {
	        if (memory_runs_[i].admitted_bytes > memory_runs_[best].admitted_bytes) {
	            best = i;
	        }
	    }

	    MemoryRun& run = memory_runs_[best];
	    const std::uint64_t admitted_bytes = run.admitted_bytes;
	    spill_sorted_run_direct_(
	        std::span<const sorted_pair_t>(run.pairs.data(), run.pairs.size()));
	    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
	        run.pairs, run.permit);
	    if (memory_run_bytes_ >= admitted_bytes) {
	        memory_run_bytes_ -= admitted_bytes;
	    } else {
	        memory_run_bytes_ = 0;
	    }
	    memory_runs_.erase(memory_runs_.begin() + static_cast<std::ptrdiff_t>(best));
	    ++stats_.resident_demotions;
	    stats_.resident_runs = static_cast<std::uint64_t>(memory_runs_.size());
	    stats_.resident_bytes = memory_run_bytes_;
	    stats_.resident_peak_bytes = memory_run_peak_bytes_;
	    return true;
	}

	void SortedPairRunStore::demote_resident_runs_until_available_impl_(
	        std::uint64_t bytes_needed) {
	    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
	    while (!memory_runs_.empty()) {
	        const std::uint64_t available = admission.allocatable_after_reservations();
	        if (available == (std::numeric_limits<std::uint64_t>::max)()
	            || available >= bytes_needed) {
	            return;
	        }
	        if (!demote_largest_resident_run_()) {
	            return;
	        }
	    }
	}

void SortedPairRunStore::demote_resident_runs_until_available(
        std::uint64_t bytes_needed) {
    if (poisoned_) {
        report_sorted_run_error(SortedRunError::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(SortedRunError::finalized_store_reused);
    }
    poisoned_ = true;
    demote_resident_runs_until_available_impl_(bytes_needed);
    poisoned_ = false;
}

std::size_t SortedPairRunStore::spill_sorted_run_owned(
        std::vector<sorted_pair_t> sorted) {
    const std::uint64_t capacity_bytes =
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(sorted.capacity()),
            static_cast<std::uint64_t>(sizeof(sorted_pair_t)));
    if (auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(capacity_bytes)) {
        return spill_sorted_run_owned_admitted(
            std::move(sorted), capacity_bytes, std::move(*permit));
    }

    ++stats_.spill_count_from_pressure;
    return spill_sorted_run(
        std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
}

std::size_t SortedPairRunStore::spill_sorted_run_owned_admitted(
        std::vector<sorted_pair_t> sorted,
        std::uint64_t admitted_bytes,
        bioseq::parallelism::MemoryPermit permit) {
    MemoryRun candidate{
        .permit = std::move(permit),
        .admitted_bytes = admitted_bytes,
        .pairs = std::move(sorted),
    };
    if (poisoned_) {
        report_sorted_run_error(SortedRunError::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(SortedRunError::finalized_store_reused);
    }
    if (!candidate.permit && admitted_bytes != 0) {
        report_sorted_run_error(SortedRunError::admitted_run_without_permit);
    }
    const std::uint64_t exact_capacity_bytes = bioseq::parallelism::saturating_mul(
        static_cast<std::uint64_t>(candidate.pairs.capacity()),
        static_cast<std::uint64_t>(sizeof(sorted_pair_t)));
    if (exact_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || exact_capacity_bytes != admitted_bytes
        || candidate.permit.bytes() < exact_capacity_bytes
        || memory_run_bytes_
            > (std::numeric_limits<std::uint64_t>::max)() - admitted_bytes) {
        report_sorted_run_error(SortedRunError::admitted_run_without_permit);
    }
    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
    candidate.permit.shrink_to(exact_capacity_bytes);
    const std::uint64_t next_memory_run_bytes =
        memory_run_bytes_ + admitted_bytes;
    if (!try_ensure_admitted_vector_capacity(
            memory_runs_, memory_runs_metadata_permit_,
            memory_runs_.size() + 1u)) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    memory_runs_.push_back(std::move(candidate));
    memory_run_bytes_ = next_memory_run_bytes;
    memory_run_peak_bytes_ = std::max(memory_run_peak_bytes_, memory_run_bytes_);
    stats_.resident_runs = static_cast<std::uint64_t>(memory_runs_.size());
    stats_.resident_bytes = memory_run_bytes_;
    stats_.resident_peak_bytes = memory_run_peak_bytes_;
    return run_index;
}

std::uint64_t SortedPairRunStore::merge_reader_budget_bytes_() const {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_sorted_run_merge_reader_budget_for_test != 0u) {
        return g_sorted_run_merge_reader_budget_for_test;
    }
#endif
    auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
    const std::uint64_t available = admission.allocatable_after_reservations();
    if (available == 0) {
        return std::uint64_t{1} << 20;
    }
    if (available == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::uint64_t{256} << 20;
    }
    return std::clamp<std::uint64_t>(
        available / 8,
        std::uint64_t{1} << 20,
        std::uint64_t{256} << 20);
}

bool SortedPairRunStore::try_open_file_readers_(MergeResources& resources) {
    if (file_runs_.empty()) {
        return true;
    }
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    std::uint64_t per_reader = std::clamp<std::uint64_t>(
        reader_budget / std::max<std::size_t>(1, file_runs_.size()),
        std::uint64_t{32} << 10,
        std::uint64_t{4} << 20);
    const std::uint64_t total_reader_bytes =
        bioseq::parallelism::saturating_add(
            bioseq::parallelism::saturating_mul(
                per_reader,
                static_cast<std::uint64_t>(file_runs_.size())),
            bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(file_runs_.size()),
                static_cast<std::uint64_t>(sizeof(FileReader))));
    const std::uint64_t reader_headroom = bioseq::parallelism::saturating_mul(
        total_reader_bytes,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (total_reader_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || reader_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(reader_headroom);
    if (!permit) {
        return false;
    }
    resources.reader_budget_permit = std::move(*permit);
    resources.readers.reserve(file_runs_.size());

    for (BufFileRun& run : file_runs_) {
        if (seek_buffile_as_cpp(run.file, 0, 0, SEEK_SET) != 0) {
            report_sorted_run_error(SortedRunError::run_rewind_failed);
        }
        FileReader reader{};
        reader.file = run.file;
        reader.remaining = run.pair_count;
        reader.buffer.reserve(std::max<std::uint64_t>(
            1, per_reader / sizeof(SortedPairWire)));
        resources.readers.push_back(std::move(reader));
    }
    const std::uint64_t exact_reader_bytes =
        file_reader_owned_capacity_bytes(resources.readers);
    if (exact_reader_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || exact_reader_bytes > resources.reader_budget_permit.bytes()) {
        resources.finish_success();
        return false;
    }
    resources.reader_budget_permit.shrink_to(exact_reader_bytes);
    stats_.merge_reader_buffer_bytes += exact_reader_bytes;
    return true;
}

void SortedPairRunStore::open_file_readers_(MergeResources& resources) {
    while (!try_open_file_readers_(resources)) {
        resources.finish_success();
        if (file_runs_.size() <= 1) {
            report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
        }
        compact_file_runs_to_output_count_(
            std::max<std::size_t>(1, file_runs_.size() / 2));
    }
}

template <class Writer>
void SortedPairRunStore::merge_sorted_file_runs_to_writer_(
        std::span<BufFileRun> runs,
        MergeResources& resources,
        Writer&& writer) {
    resources.finish_success();
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::uint64_t per_reader_bytes = std::clamp<std::uint64_t>(
        reader_budget / std::max<std::size_t>(1u, runs.size()),
        std::uint64_t{32} << 10,
        std::uint64_t{4} << 20);
    const std::uint64_t logical_reader_bytes = bioseq::parallelism::saturating_add(
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(runs.size()), per_reader_bytes),
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(runs.size()),
            static_cast<std::uint64_t>(sizeof(FileReader))));
    const std::uint64_t reader_headroom = bioseq::parallelism::saturating_mul(
        logical_reader_bytes,
        bioseq::parallelism::kVectorPreallocationHeadroomFactor);
    if (logical_reader_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || reader_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    auto reader_permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(reader_headroom);
    if (!reader_permit) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    resources.reader_budget_permit = std::move(*reader_permit);
    resources.readers.reserve(runs.size());
    for (BufFileRun& run : runs) {
        if (seek_buffile_as_cpp(run.file, 0, 0, SEEK_SET) != 0) {
            report_sorted_run_error(SortedRunError::compacted_run_rewind_failed);
        }
        FileReader reader{};
        reader.file = run.file;
        reader.remaining = run.pair_count;
        reader.buffer.reserve(static_cast<std::size_t>(
            std::max<std::uint64_t>(
                1u, per_reader_bytes / sizeof(SortedPairWire))));
        resources.readers.push_back(std::move(reader));
    }
    const std::uint64_t exact_reader_bytes =
        file_reader_owned_capacity_bytes(resources.readers);
    if (exact_reader_bytes == (std::numeric_limits<std::uint64_t>::max)()
        || exact_reader_bytes > resources.reader_budget_permit.bytes()) {
        report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
    }
    resources.reader_budget_permit.shrink_to(exact_reader_bytes);
    stats_.merge_reader_buffer_bytes += exact_reader_bytes;

    auto admitted_heap = make_admitted_merge_head_queue(runs.size());
    MergeHeadQueue& heap = admitted_heap.queue;
    for (std::size_t idx = 0; idx < resources.readers.size(); ++idx) {
        FileReader& reader = resources.readers[idx];
        if (reader.read_next()) {
            heap.push(MergeHead{
                .kmer = reader.current.kmer,
                .encoded = reader.current.encoded,
                .run_index = idx,
                .file = true,
            });
        }
    }
    while (!heap.empty()) {
        MergeHead head = heap.top();
        heap.pop();
        writer(SortedPairWire{.kmer = head.kmer, .encoded = head.encoded});
        FileReader& reader = resources.readers[head.run_index];
        if (reader.read_next()) {
            heap.push(MergeHead{
                .kmer = reader.current.kmer,
                .encoded = reader.current.encoded,
                .run_index = head.run_index,
                .file = true,
            });
        }
    }
}

BufFileRun SortedPairRunStore::merge_file_run_group_to_new_buffile_(
        std::size_t lo,
        std::size_t hi) {
#ifdef BIOSEQ_TEST_INDEXES
    bioseq::pg_utils::invoke_pg_as_cpp([] {
        inject_sorted_run_pg_leaf_failure(
            SortedRunPgLeafFailure::compact_create_before);
    });
#endif
    BufFile* out_file = create_temp_buffile_as_cpp();
    if (out_file == nullptr) {
        report_sorted_run_error(SortedRunError::compact_temp_file_create_failed);
    }
    auto resources = std::make_unique<MergeResources>();
    BufFileRun out{.file = out_file};
    try {
        merge_sorted_file_runs_to_writer_(
            std::span<BufFileRun>(file_runs_.data() + lo, hi - lo),
            *resources,
            [&](SortedPairWire pair) {
                write_buffile_as_cpp(out_file, &pair, sizeof(pair));
                ++out.pair_count;
                out.byte_count += sizeof(pair);
            });
        ++stats_.postgres_temp_file_count;
        const int64 size = size_buffile_as_cpp(out.file);
        if (size > 0) {
            stats_.postgres_temp_file_bytes += static_cast<std::uint64_t>(size);
        }
        resources->finish_success();
        out_file = nullptr;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        resources->abandon_pg_resources_noexcept();
        // out_file remains registered with the current ResourceOwner.
        out_file = nullptr;
        throw;
    } catch (...) {
        // The caller will turn the C++ failure into a PostgreSQL ERROR.  Do
        // not call BufFileClose while preserving that original exception.
        resources->abandon_pg_resources_noexcept();
        abandon_buffile_noexcept(out_file);
        throw;
    }
    return out;
}

void SortedPairRunStore::compact_file_runs_to_output_count_(
        std::size_t target_output_runs) {
    target_output_runs = std::max<std::size_t>(1, target_output_runs);
    while (file_runs_.size() > target_output_runs) {
        const std::size_t group_size = std::max<std::size_t>(
            2,
            (file_runs_.size() + target_output_runs - 1) / target_output_runs);
        AdmittedOwnedVector<BufFileRun> admitted_next;
        if (!try_ensure_admitted_vector_capacity(
                admitted_next, target_output_runs)) {
            report_sorted_run_error(SortedRunError::merge_reader_admission_failed);
        }
        std::vector<BufFileRun>& next = admitted_next.values;
        try {
            for (std::size_t lo = 0; lo < file_runs_.size(); lo += group_size) {
                const std::size_t hi = std::min(file_runs_.size(), lo + group_size);
                next.push_back(merge_file_run_group_to_new_buffile_(lo, hi));
                for (std::size_t i = lo; i < hi; ++i) {
                    close_buffile_checked_as_cpp(file_runs_[i].file);
                }
            }
            bioseq::parallelism::release_admitted_vector_capacity_before_permit(
                file_runs_, file_runs_metadata_permit_);
            // `file_runs_` is now capacity-free.  Swap transfers the next
            // allocation while its permit remains live and proves `next` is
            // empty before that permit moves into the store.
            file_runs_.swap(next);
            file_runs_metadata_permit_ = std::move(admitted_next.permit);
            ++stats_.merge_pass_count;
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            // Some earlier input groups may already be closed.  The original
            // run set is no longer retryable even if this helper is ever
            // reached from a new caller that did not pre-poison the store.
            poisoned_ = true;
            for (BufFileRun& run : next) {
                // ResourceOwner cleanup owns files touched by the failed PG
                // operation; do not issue more PostgreSQL calls here.
                run.file = nullptr;
            }
            throw;
        } catch (...) {
            poisoned_ = true;
            for (BufFileRun& run : next) {
                abandon_buffile_noexcept(run.file);
            }
            throw;
        }
    }
}

void SortedPairRunStore::compact_file_runs_until_bounded_() {
    if (file_runs_.empty()) {
        return;
    }
    constexpr std::uint64_t kMinReaderBytes = std::uint64_t{32} << 10;
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::size_t max_output_runs =
        std::max<std::size_t>(1, reader_budget / kMinReaderBytes);
    compact_file_runs_to_output_count_(max_output_runs);
}

void SortedPairRunStore::merge_runs_erased_impl_(
        SortedPairKmerSink sink,
        MergeResources& resources) {
    compact_file_runs_until_bounded_();
    open_file_readers_(resources);

    auto admitted_heap = make_admitted_merge_head_queue(
        memory_runs_.size() + resources.readers.size());
    MergeHeadQueue& heap = admitted_heap.queue;
    for (std::size_t run_index = 0; run_index < memory_runs_.size(); ++run_index) {
        const auto& pairs = memory_runs_[run_index].pairs;
        if (!pairs.empty()) {
            heap.push(MergeHead{
                .kmer = pairs.front().first,
                .encoded = pairs.front().second,
                .run_index = run_index,
                .pair_index = 0,
                .file = false,
            });
        }
    }
    for (std::size_t run_index = 0; run_index < resources.readers.size(); ++run_index) {
        FileReader& reader = resources.readers[run_index];
        if (reader.read_next()) {
            heap.push(MergeHead{
                .kmer = reader.current.kmer,
                .encoded = reader.current.encoded,
                .run_index = run_index,
                .pair_index = 0,
                .file = true,
            });
        }
    }

    while (!heap.empty()) {
        const std::uint64_t kmer = heap.top().kmer;
        resources.positions.reset_for_kmer();
        std::uint64_t last_encoded = 0;
        bool have_last = false;
        while (!heap.empty() && heap.top().kmer == kmer) {
            const MergeHead head = heap.top();
            heap.pop();
            if (!have_last || head.encoded != last_encoded) {
                resources.positions.push_unique_encoded(head.encoded);
                last_encoded = head.encoded;
                have_last = true;
            }
            if (head.file) {
                FileReader& reader = resources.readers[head.run_index];
                if (reader.read_next()) {
                    heap.push(MergeHead{
                        .kmer = reader.current.kmer,
                        .encoded = reader.current.encoded,
                        .run_index = head.run_index,
                        .file = true,
                    });
                }
            } else {
                const std::size_t next_pair = head.pair_index + 1;
                const auto& pairs = memory_runs_[head.run_index].pairs;
                if (next_pair < pairs.size()) {
                    heap.push(MergeHead{
                        .kmer = pairs[next_pair].first,
                        .encoded = pairs[next_pair].second,
                        .run_index = head.run_index,
                        .pair_index = next_pair,
                        .file = false,
                    });
                }
            }
        }
        resources.positions.emit(kmer, sink);
        if (resources.positions.spill != nullptr) {
            ++stats_.position_spill_count;
            stats_.position_spill_bytes += resources.positions.spilled_bytes;
        }
    }
}

void SortedPairRunStore::merge_runs_erased(SortedPairKmerSink sink) {
    if (poisoned_) {
        report_sorted_run_error(SortedRunError::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(SortedRunError::finalized_store_reused);
    }
    poisoned_ = true;
    auto resources = std::make_unique<MergeResources>();
    try {
        merge_runs_erased_impl_(sink, *resources);
        resources->finish_success();
        // MPS qualification and generation encoding deliberately replay the
        // same immutable runs. Keep successful handles rewindable until the
        // owning storage calls the checked `release_runs()` boundary.
        poisoned_ = false;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        resources->abandon_pg_resources_noexcept();
        throw;
    } catch (...) {
        // The callback may throw an ordinary C++ exception.  Abandon all
        // ResourceOwner-backed handles; closing here could mask that error.
        resources->abandon_pg_resources_noexcept();
        throw;
    }
}

void SortedPairRunStore::finalize_file_handles_success_() {
    for (BufFileRun& run : file_runs_) {
        close_buffile_checked_as_cpp(run.file);
    }
}

void SortedPairRunStore::release_resident_runs_noexcept() noexcept {
    for (MemoryRun& run : memory_runs_) {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(
            run.pairs, run.permit);
    }
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        memory_runs_, memory_runs_metadata_permit_);
    memory_run_bytes_ = 0;
    memory_run_peak_bytes_ = 0;
}

void SortedPairRunStore::release_runs_noexcept() noexcept {
    release_resident_runs_noexcept();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        write_buffer_, write_buffer_permit_);
    for (BufFileRun& run : file_runs_) {
        // Destructors, move assignment and explicit error cleanup never enter
        // PostgreSQL.  Temp files remain registered with ResourceOwner.
        abandon_buffile_noexcept(run.file);
    }
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        file_runs_, file_runs_metadata_permit_);
    spilled_run_bytes_ = 0;
    stats_ = {};
    finalized_ = false;
}

void SortedPairRunStore::release_runs() {
    if (poisoned_) {
        report_sorted_run_error(SortedRunError::poisoned_store_reused);
    }
    poisoned_ = true;
    finalize_file_handles_success_();
    release_resident_runs_noexcept();
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        write_buffer_, write_buffer_permit_);
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(
        file_runs_, file_runs_metadata_permit_);
    spilled_run_bytes_ = 0;
    stats_ = {};
    finalized_ = false;
    poisoned_ = false;
}

} // namespace bioseq::storage::indexing
