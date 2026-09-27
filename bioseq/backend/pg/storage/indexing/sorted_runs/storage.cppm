/**
 * @file storage.cppm
 * @brief Keep sorted runs in memory or temporary files and reserve memory for their buffers.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/buffile.h"

// Keep the PostgreSQL error implementation in a separate module unit
// so this declaration can be compiled by both supported compilers.
[[noreturn]] void bioseq_sorted_run_error_pg_leaf(unsigned error_code);
}

#include <algorithm>
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
#include <cstdio>
#endif
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

export module bioseq.postgres.storage.indexing.sorted_runs:sorted_runs_storage;

import bioseq.common.checked_arithmetic;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.common.logging;
#endif
import bioseq.postgres.support;
import bioseq.parallelism.memory_admission;

export import :sorted_runs_contracts;

namespace bioseq::storage::indexing {

/// The hash and encoded position written for one pair in a temporary run.
struct sorted_pair_wire {
    std::uint64_t kmer = 0;
    std::uint64_t encoded = 0;
};
static_assert(sizeof(sorted_pair_wire) == 16);
static_assert(alignof(sorted_pair_wire) == alignof(std::uint64_t));
static_assert(offsetof(sorted_pair_wire, kmer) == 0);
static_assert(offsetof(sorted_pair_wire, encoded) == 8);

inline constexpr std::uint64_t target_position_batch_bytes = std::uint64_t{8} << 20;
inline constexpr std::uint64_t minimum_position_batch_bytes = std::uint64_t{32} << 10;
inline constexpr std::uint64_t minimum_merge_reader_buffer_bytes = std::uint64_t{32} << 10;
inline constexpr std::uint64_t maximum_merge_reader_buffer_bytes = std::uint64_t{4} << 20;

/// Error codes passed to the PostgreSQL reporting function.
enum class sorted_run_error : unsigned {
    // Keep these values in sync with the separate PostgreSQL error implementation.
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

/// Report a sorted-run error as a PostgreSQL error wrapped in a C++ exception.
[[noreturn]] void report_sorted_run_error(sorted_run_error error) {
    const unsigned error_code = static_cast<unsigned>(error);
    bioseq::postgres::support::invoke_pg_as_cpp(
        [error_code] { bioseq_sorted_run_error_pg_leaf(error_code); });
    // The error bridge should throw. Trap if it returns despite the noreturn declaration.
    __builtin_trap();
}

/// Memory request that caused a merge to fail.
enum class sorted_run_merge_admission_site : std::uint8_t {
    compaction_readers,
    merge_head_queue,
    final_readers,
};

#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
/// Log the memory budget and failed request when the merge cannot continue.
void emit_terminal_merge_admission_failure(
    sorted_run_merge_admission_site site,
    const bioseq::parallelism::memory_admission_failure& failure) noexcept {
    const char* site_name = "unknown";
    switch (site) {
    case sorted_run_merge_admission_site::compaction_readers:
        site_name = "compaction_readers";
        break;
    case sorted_run_merge_admission_site::merge_head_queue:
        site_name = "merge_head_queue";
        break;
    case sorted_run_merge_admission_site::final_readers:
        site_name = "final_readers";
        break;
    }
    const auto failure_reason =
        bioseq::parallelism::memory_admission_failure_reason_name(failure.reason);
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
    // Write to stderr so the benchmark saves this failure with the server log.
    (void)std::fprintf(
        stderr,
        "[bioseq.sorted_runs.merge_admission] site=%s reason=%.*s requested_bytes=%llu "
        "reserved_bytes=%llu budget_ceiling_bytes=%llu "
        "allocatable_after_reservations_bytes=%llu process_footprint_bytes=%llu "
        "system_available_bytes=%llu system_total_bytes=%llu process_soft_cap_bytes=%llu\n",
        site_name,
        static_cast<int>(failure_reason.size()),
        failure_reason.data(),
        static_cast<unsigned long long>(failure.requested_bytes),
        static_cast<unsigned long long>(failure.reserved_bytes),
        static_cast<unsigned long long>(failure.budget_ceiling_bytes),
        static_cast<unsigned long long>(failure.allocatable_after_reservations_bytes),
        static_cast<unsigned long long>(failure.process_footprint_bytes),
        static_cast<unsigned long long>(failure.system_available_bytes),
        static_cast<unsigned long long>(failure.system_total_bytes),
        static_cast<unsigned long long>(failure.process_soft_cap_bytes));
    (void)std::fflush(stderr);
#endif
#if defined(BIOSEQ_TEST_INDEXES)
    try {
        bioseq_log<logging_level::warning, logging_target::file>(
            "[bioseq.sorted_runs.merge_admission] site={} reason={} requested_bytes={} "
            "reserved_bytes={} budget_ceiling_bytes={} "
            "allocatable_after_reservations_bytes={} process_footprint_bytes={} "
            "system_available_bytes={} system_total_bytes={} process_soft_cap_bytes={}",
            site_name,
            failure_reason,
            static_cast<unsigned long long>(failure.requested_bytes),
            static_cast<unsigned long long>(failure.reserved_bytes),
            static_cast<unsigned long long>(failure.budget_ceiling_bytes),
            static_cast<unsigned long long>(failure.allocatable_after_reservations_bytes),
            static_cast<unsigned long long>(failure.process_footprint_bytes),
            static_cast<unsigned long long>(failure.system_available_bytes),
            static_cast<unsigned long long>(failure.system_total_bytes),
            static_cast<unsigned long long>(failure.process_soft_cap_bytes));
    } catch (...) {
        // Diagnostics must not replace the existing sorted-run PostgreSQL error.
    }
#endif
}
#endif

#ifdef BIOSEQ_TEST_INDEXES
inline thread_local sorted_run_failure_injection g_sorted_run_pg_leaf_failure =
    sorted_run_failure_injection::none;
inline thread_local std::uint64_t g_sorted_run_pg_leaf_failure_remaining = 0;
inline thread_local std::uint64_t g_sorted_run_merge_reader_budget_for_test = 0;
inline thread_local sorted_run_reader_admission_injection
    g_sorted_run_reader_admission_injection_for_test =
        sorted_run_reader_admission_injection::none;
inline thread_local std::uint64_t
    g_sorted_run_reader_request_ceiling_for_test = 0;

/// Fail at the next call matching this PostgreSQL test hook.
void arm_sorted_run_pg_leaf_failure(sorted_run_failure_injection point) noexcept {
    arm_sorted_run_pg_leaf_failure_on_nth(point, 1u);
}

/// Fail at the selected occurrence of this PostgreSQL test hook.
void arm_sorted_run_pg_leaf_failure_on_nth(sorted_run_failure_injection point,
                                           std::uint64_t operation) noexcept {
    g_sorted_run_pg_leaf_failure = point;
    g_sorted_run_pg_leaf_failure_remaining =
        point == sorted_run_failure_injection::none ? 0u : std::max<std::uint64_t>(1u, operation);
}

/// Override the merge-reader memory budget for one backend test.
void set_sorted_run_merge_reader_budget_for_test(std::uint64_t bytes) noexcept {
    g_sorted_run_merge_reader_budget_for_test = bytes;
}

/// Force a reader memory request to fail under the selected test condition.
void set_sorted_run_reader_admission_injection_for_test(
    sorted_run_reader_admission_injection injection,
    std::uint64_t request_ceiling_bytes) noexcept {
    g_sorted_run_reader_admission_injection_for_test = injection;
    g_sorted_run_reader_request_ceiling_for_test = request_ceiling_bytes;
}

/// Clear every sorted-run failure and budget override.
void disarm_sorted_run_pg_leaf_failure() noexcept {
    g_sorted_run_pg_leaf_failure = sorted_run_failure_injection::none;
    g_sorted_run_pg_leaf_failure_remaining = 0;
    g_sorted_run_merge_reader_budget_for_test = 0;
    g_sorted_run_reader_admission_injection_for_test =
        sorted_run_reader_admission_injection::none;
    g_sorted_run_reader_request_ceiling_for_test = 0;
}

/// Report whether a PostgreSQL test failure is armed.
bool sorted_run_pg_leaf_failure_is_armed() noexcept {
    return g_sorted_run_pg_leaf_failure != sorted_run_failure_injection::none;
}

/// Raise the selected test error when its countdown reaches this call.
void inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection point) {
    if (g_sorted_run_pg_leaf_failure != point) {
        return;
    }
    if (g_sorted_run_pg_leaf_failure_remaining > 1u) {
        --g_sorted_run_pg_leaf_failure_remaining;
        return;
    }
    // Disarm before raising ERROR so the next statement can test recovery.
    g_sorted_run_pg_leaf_failure = sorted_run_failure_injection::none;
    g_sorted_run_pg_leaf_failure_remaining = 0;
    ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("injected sorted-run PostgreSQL leaf failure"),
             errdetail("failure point %u", static_cast<unsigned>(point))));
}
#endif

/** Clear the file pointer and leave closing it to PostgreSQL ResourceOwner cleanup. */
void abandon_buffile_noexcept(BufFile*& file) noexcept {
    file = nullptr;
}

/// Create a ResourceOwner-managed temporary buffer file.
[[nodiscard]] BufFile* create_temp_buffile_as_cpp() {
    return bioseq::postgres::support::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::create_before);
#endif
        BufFile* result = BufFileCreateTemp(false);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::create_after);
#endif
        return result;
    });
}

/// Write bytes to a temporary file, converting PostgreSQL errors to C++ exceptions.
void write_buffile_as_cpp(BufFile* file, const void* data, std::size_t bytes) {
    bioseq::postgres::support::invoke_pg_as_cpp([file, data, bytes] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::write_before);
#endif
        BufFileWrite(file, data, bytes);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::write_after);
#endif
    });
}

/// Read the requested bytes, converting PostgreSQL errors to C++ exceptions.
void read_buffile_exact_as_cpp(BufFile* file, void* data, std::size_t bytes) {
    bioseq::postgres::support::invoke_pg_as_cpp([file, data, bytes] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::read_before);
#endif
        BufFileReadExact(file, data, bytes);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::read_after);
#endif
    });
}

/// Seek a temporary file, converting PostgreSQL errors to C++ exceptions.
[[nodiscard]] int seek_buffile_as_cpp(BufFile* file, int fileno, off_t offset, int whence) {
    return bioseq::postgres::support::invoke_pg_as_cpp([file, fileno, offset, whence] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::seek_before);
#endif
        const int result = BufFileSeek(file, fileno, offset, whence);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::seek_after);
#endif
        return result;
    });
}

/// Return a buffer file's current byte length.
[[nodiscard]] int64 size_buffile_as_cpp(BufFile* file) {
    return bioseq::postgres::support::invoke_pg_as_cpp([file] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::size_before);
#endif
        const int64 result = BufFileSize(file);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::size_after);
#endif
        return result;
    });
}

/// Check PostgreSQL interrupts through the C++ error bridge.
void check_for_interrupts_as_cpp() {
    bioseq::postgres::support::invoke_pg_as_cpp([] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::interrupt_before);
#endif
        CHECK_FOR_INTERRUPTS();
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::interrupt_after);
#endif
    });
}

/** Close a file after success, clearing the pointer before calling PostgreSQL. */
void close_buffile_checked_as_cpp(BufFile*& file) {
    BufFile* closing = std::exchange(file, nullptr);
    if (closing == nullptr) {
        return;
    }
    bioseq::postgres::support::invoke_pg_as_cpp([closing] {
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::close_before);
#endif
        BufFileClose(closing);
#ifdef BIOSEQ_TEST_INDEXES
        inject_sorted_run_pg_leaf_failure(sorted_run_failure_injection::close_after);
#endif
    });
}

/// Sorted pairs held in memory with a reservation covering the vector capacity.
struct memory_run {
    // Declare the permit first so pair storage is freed before its reservation.
    bioseq::parallelism::memory_permit permit;
    std::uint64_t admitted_bytes = 0;
    std::vector<sorted_pair_t> pairs;
};

/// A temporary run file and its pair and byte counts.
struct buffer_file_run {
    BufFile* file = nullptr;
    std::uint64_t pair_count = 0;
    std::uint64_t byte_count = 0;
};

/// Vector whose capacity remains covered by its memory permit.
template <class p_value> struct admitted_owned_vector {
    // Reverse destruction drops vector capacity before its reservation.
    bioseq::parallelism::memory_permit permit;
    std::vector<p_value> values;
};

/// Reserve the peak memory needed before growing a vector.
template <class p_value>
[[nodiscard]] bool try_ensure_admitted_vector_capacity(std::vector<p_value>& values,
                                                       bioseq::parallelism::memory_permit& permit,
                                                       std::size_t required_capacity) {
    const std::uint64_t existing_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()), static_cast<std::uint64_t>(sizeof(p_value)));
    if (existing_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (values.capacity() >= required_capacity) {
        return existing_bytes == 0u || (permit && permit.bytes() >= existing_bytes);
    }
    const std::uint64_t logical_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(required_capacity), static_cast<std::uint64_t>(sizeof(p_value)));
    const std::uint64_t admitted_headroom = bioseq::common::saturating_mul(
        logical_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    const std::uint64_t allocation_peak =
        bioseq::common::saturating_add(existing_bytes, admitted_headroom);
    if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        admitted_headroom == (std::numeric_limits<std::uint64_t>::max)() ||
        allocation_peak == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    if (permit) {
        if (permit.bytes() < existing_bytes || !permit.grow_to(allocation_peak)) {
            return false;
        }
    } else {
        if (existing_bytes != 0u) {
            return false;
        }
        auto acquired = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            allocation_peak);
        if (!acquired) {
            return false;
        }
        permit = std::move(*acquired);
    }
    values.reserve(required_capacity);
    const std::uint64_t exact_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(values.capacity()), static_cast<std::uint64_t>(sizeof(p_value)));
    if (exact_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        exact_bytes > permit.bytes()) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    permit.shrink_to(exact_bytes);
    return true;
}

/// Grow a vector together with the reservation that covers its capacity.
template <class p_value>
[[nodiscard]] bool try_ensure_admitted_vector_capacity(admitted_owned_vector<p_value>& owned,
                                                       std::size_t required_capacity) {
    return try_ensure_admitted_vector_capacity(owned.values, owned.permit, required_capacity);
}

/// Read a sorted temporary run sequentially through a reusable buffer.
struct file_reader {
    BufFile* file = nullptr;
    std::uint64_t remaining = 0;
    std::vector<sorted_pair_wire> buffer;
    std::size_t offset = 0;
    std::size_t available = 0;
    sorted_pair_wire current{};

    /// Read the next pair, returning false at the end of the run.
    [[nodiscard]] bool read_next() {
        if (offset >= available) {
            if (remaining == 0) {
                return false;
            }
            const std::uint64_t cap =
                std::max<std::uint64_t>(1, static_cast<std::uint64_t>(buffer.capacity()));
            const std::size_t count =
                static_cast<std::size_t>(std::min<std::uint64_t>(remaining, cap));
            buffer.resize(count);
            read_buffile_exact_as_cpp(file, buffer.data(), count * sizeof(sorted_pair_wire));
            remaining -= count;
            offset = 0;
            available = count;
        }
        current = buffer[offset++];
        return true;
    }
};

/// Collect one hash's positions in batches, spilling full batches to a temporary file.
struct position_accumulator {
    // Declare the permit first so both buffers are freed before it.
    bioseq::parallelism::memory_permit resident_permit;
    std::vector<std::uint64_t> resident;
    std::vector<std::uint64_t> read_buffer;
    BufFile* spill = nullptr;
    std::uint64_t total = 0;
    std::uint64_t spilled_bytes = 0;
    bool resident_batch_ready = false;

    /// Reset storage before collecting another minimizer hash.
    void reset_for_kmer() {
        resident.clear();
        read_buffer.clear();
        if (spill != nullptr) {
            close_buffile_checked_as_cpp(spill);
        }
        total = 0;
        spilled_bytes = 0;
    }

    /// Reserve memory for the collection and read buffers before allocating them.
    void ensure_resident_batch() {
        if (resident_batch_ready) {
            return;
        }
        auto& admission = bioseq::parallelism::memory_admission_controller::instance();
        std::uint64_t per_buffer_bytes = target_position_batch_bytes;
        const std::uint64_t available = admission.allocatable_after_reservations();
        if (available != (std::numeric_limits<std::uint64_t>::max)()) {
            constexpr std::uint64_t total_headroom_factor =
                2u * bioseq::parallelism::k_vector_preallocation_headroom_factor;
            per_buffer_bytes = std::min(per_buffer_bytes, available / total_headroom_factor);
        }
        per_buffer_bytes -= per_buffer_bytes % sizeof(std::uint64_t);
        if (per_buffer_bytes < minimum_position_batch_bytes) {
            report_sorted_run_error(sorted_run_error::position_buffer_admission_failed);
        }
        for (;;) {
            constexpr std::uint64_t total_headroom_factor =
                2u * bioseq::parallelism::k_vector_preallocation_headroom_factor;
            if (per_buffer_bytes >
                (std::numeric_limits<std::uint64_t>::max)() / total_headroom_factor) {
                report_sorted_run_error(sorted_run_error::position_buffer_admission_failed);
            }
            // Reserve space for both buffers, including possible extra vector capacity,
            // before either allocation starts.
            const std::uint64_t requested_total_bytes = per_buffer_bytes * total_headroom_factor;
            if (auto permit = admission.try_reserve(requested_total_bytes)) {
                struct candidate_buffers {
                    bioseq::parallelism::memory_permit permit;
                    std::vector<std::uint64_t> resident;
                    std::vector<std::uint64_t> read;
                } candidate{
                    .permit = std::move(*permit),
                    .resident = {},
                    .read = {},
                };
                const std::size_t units =
                    static_cast<std::size_t>(per_buffer_bytes / sizeof(std::uint64_t));
                candidate.resident.reserve(units);
                candidate.read.reserve(units);

                const auto capacity_bytes =
                    [](std::size_t capacity) -> std::optional<std::uint64_t> {
                    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();
                    if (capacity > max / sizeof(std::uint64_t)) {
                        return std::nullopt;
                    }
                    return static_cast<std::uint64_t>(capacity) * sizeof(std::uint64_t);
                };
                const auto resident_bytes = capacity_bytes(candidate.resident.capacity());
                const auto read_bytes = capacity_bytes(candidate.read.capacity());
                if (!resident_bytes || !read_bytes ||
                    *resident_bytes > (std::numeric_limits<std::uint64_t>::max)() - *read_bytes) {
                    bioseq::parallelism::release_admitted_vector_capacities_before_permit(
                        candidate.resident, candidate.read, candidate.permit);
                    report_sorted_run_error(sorted_run_error::position_buffer_admission_failed);
                }
                const std::uint64_t exact_total_bytes = *resident_bytes + *read_bytes;
                if (exact_total_bytes > candidate.permit.bytes()) {
                    bioseq::parallelism::release_admitted_vector_capacities_before_permit(
                        candidate.resident, candidate.read, candidate.permit);
                    report_sorted_run_error(sorted_run_error::position_buffer_admission_failed);
                }
                candidate.permit.shrink_to(exact_total_bytes);
                resident = std::move(candidate.resident);
                read_buffer = std::move(candidate.read);
                resident_permit = std::move(candidate.permit);
                resident_batch_ready = true;
                return;
            }
            if (per_buffer_bytes == minimum_position_batch_bytes) {
                break;
            }
            per_buffer_bytes = std::max(per_buffer_bytes / 2u, minimum_position_batch_bytes);
            per_buffer_bytes -= per_buffer_bytes % sizeof(std::uint64_t);
        }
        report_sorted_run_error(sorted_run_error::position_buffer_admission_failed);
    }

    /// Append an encoded position, spilling the preceding batch when it is full.
    void push_unique_encoded(std::uint64_t encoded) {
        ensure_resident_batch();
        if (resident.size() == resident.capacity()) {
            if (spill == nullptr) {
                spill = create_temp_buffile_as_cpp();
                if (spill == nullptr) {
                    report_sorted_run_error(sorted_run_error::position_spill_create_failed);
                }
            }
            write_buffile_as_cpp(spill, resident.data(), resident.size() * sizeof(std::uint64_t));
            spilled_bytes += resident.size() * sizeof(std::uint64_t);
            resident.clear();
        }
        resident.push_back(encoded);
        ++total;
    }

    /// Visit positions in insertion order, reading spilled batches before the final memory batch.
    void emit_batches(position_batch_sink sink) {
        if (sink.call == nullptr) {
            return;
        }
        if (spill != nullptr) {
            if (seek_buffile_as_cpp(spill, 0, 0, SEEK_SET) != 0) {
                report_sorted_run_error(sorted_run_error::position_spill_rewind_failed);
            }
            std::uint64_t remaining =
                spilled_bytes / static_cast<std::uint64_t>(sizeof(std::uint64_t));
            const std::uint64_t cap =
                std::max<std::uint64_t>(1, static_cast<std::uint64_t>(read_buffer.capacity()));
            while (remaining != 0) {
                const std::size_t count =
                    static_cast<std::size_t>(std::min<std::uint64_t>(remaining, cap));
                read_buffer.resize(count);
                read_buffile_exact_as_cpp(spill, read_buffer.data(), count * sizeof(std::uint64_t));
                sink.call(sink.ctx,
                          std::span<const std::uint64_t>(read_buffer.data(), read_buffer.size()));
                remaining -= count;
            }
        }
        if (!resident.empty()) {
            sink.call(sink.ctx, std::span<const std::uint64_t>(resident.data(), resident.size()));
        }
    }

    /// Call the sink for this hash with a cursor over its collected positions.
    void emit(std::uint64_t kmer, sorted_pair_kmer_sink sink) {
        if (sink.call == nullptr) {
            return;
        }
        sorted_pair_position_cursor cursor{this};
        sink.call(sink.ctx, kmer, cursor);
    }

    /// Close the spill file and free both position buffers after success.
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

    /// Free position buffers and leave file cleanup to PostgreSQL after an error.
    void abandon_pg_resources_noexcept() noexcept {
        bioseq::parallelism::release_admitted_vector_capacities_before_permit(
            resident, read_buffer, resident_permit);
        // A PostgreSQL error may have interrupted file IO. Leave the file to
        // ResourceOwner cleanup instead of calling PostgreSQL while unwinding.
        abandon_buffile_noexcept(spill);
        total = 0;
        spilled_bytes = 0;
        resident_batch_ready = false;
    }
};

/// File readers and position buffers used by one merge.
struct merge_resources {
    // Declare the permit first so reader buffers are freed before it.
    bioseq::parallelism::memory_permit reader_budget_permit;
    std::vector<file_reader> readers;
    position_accumulator positions;

    /// Free reader buffers and close the position spill file after success.
    void finish_success() {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(readers,
                                                                            reader_budget_permit);
        positions.finish_success();
    }

    /// Free merge buffers and leave spill file cleanup to PostgreSQL.
    void abandon_pg_resources_noexcept() noexcept {
        bioseq::parallelism::release_admitted_vector_capacity_before_permit(readers,
                                                                            reader_budget_permit);
        positions.abandon_pg_resources_noexcept();
    }
};

/// The next pair from one input run and the location needed to advance it.
struct merge_head {
    std::uint64_t kmer = 0;
    std::uint64_t encoded = 0;
    std::size_t run_index = 0;
    std::size_t pair_index = 0;
    bool file = false;
};

/// Orders merge heads by hash and encoded position.
struct merge_head_greater {
    bool operator()(const merge_head& lhs, const merge_head& rhs) const noexcept {
        if (lhs.kmer != rhs.kmer) {
            return lhs.kmer > rhs.kmer;
        }
        return lhs.encoded > rhs.encoded;
    }
};

using merge_head_queue =
    std::priority_queue<merge_head, std::vector<merge_head>, merge_head_greater>;

/// Merge priority queue with a reservation covering its vector capacity.
struct admitted_merge_head_queue {
    // Queue capacity is destroyed before its permit by reverse order.
    bioseq::parallelism::memory_permit permit;
    merge_head_queue queue;

    admitted_merge_head_queue() = default;
    admitted_merge_head_queue(const admitted_merge_head_queue&) = delete;
    admitted_merge_head_queue& operator=(const admitted_merge_head_queue&) = delete;
    admitted_merge_head_queue(admitted_merge_head_queue&& other) noexcept {
        // Move the queue storage before its reservation so the memory remains
        // accounted for throughout the transfer.
        queue.swap(other.queue);
        permit = std::move(other.permit);
    }
    admitted_merge_head_queue& operator=(admitted_merge_head_queue&&) = delete;
};

/// A reserved merge queue, or the reason its memory request failed.
struct merge_head_queue_admission {
    std::optional<admitted_merge_head_queue> admitted;
    bioseq::parallelism::memory_admission_failure failure{};
};

/// Reserve and create a merge queue with room for every input run.
[[nodiscard]] merge_head_queue_admission
try_make_admitted_merge_head_queue(std::size_t maximum_heads) {
    const std::uint64_t logical_bytes = bioseq::common::saturating_mul(
        static_cast<std::uint64_t>(maximum_heads), static_cast<std::uint64_t>(sizeof(merge_head)));
    const std::uint64_t admitted_headroom = bioseq::common::saturating_mul(
        logical_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        admitted_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        return {};
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto permit = admission.try_reserve(admitted_headroom, &admission_failure);
    if (!permit) {
        return merge_head_queue_admission{.failure = admission_failure};
    }
    std::vector<merge_head> backing;
    backing.reserve(maximum_heads);
    const std::uint64_t exact_bytes =
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(backing.capacity()),
                                       static_cast<std::uint64_t>(sizeof(merge_head)));
    if (exact_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        exact_bytes > permit->bytes()) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    permit->shrink_to(exact_bytes);
    // Keep the local reservation alive while constructing the queue.
    // The following swaps and permit move cannot throw.
    merge_head_queue prepared{merge_head_greater{}, std::move(backing)};
    admitted_merge_head_queue admitted;
    admitted.queue.swap(prepared);
    admitted.permit = std::move(*permit);
    merge_head_queue_admission result;
    result.admitted.emplace(std::move(admitted));
    return result;
}

/// Create a merge queue or report that its memory could not be reserved.
[[nodiscard]] admitted_merge_head_queue make_admitted_merge_head_queue(std::size_t maximum_heads) {
    auto attempt = try_make_admitted_merge_head_queue(maximum_heads);
    if (!attempt.admitted.has_value()) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
        if (attempt.failure.has_failure()) {
            emit_terminal_merge_admission_failure(
                sorted_run_merge_admission_site::merge_head_queue, attempt.failure);
        }
#endif
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    return std::move(*attempt.admitted);
}

/// Count capacity used by the reader vector and all its pair buffers.
[[nodiscard]] std::uint64_t
file_reader_owned_capacity_bytes(const std::vector<file_reader>& readers) noexcept {
    std::uint64_t bytes =
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(readers.capacity()),
                                       static_cast<std::uint64_t>(sizeof(file_reader)));
    for (const file_reader& reader : readers) {
        bytes = bioseq::common::saturating_add(
            bytes,
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(reader.buffer.capacity()),
                                           static_cast<std::uint64_t>(sizeof(sorted_pair_wire))));
    }
    return bytes;
}

/// Chosen buffer size per reader and the reservation covering all readers.
struct admitted_file_reader_budget {
    bioseq::parallelism::memory_permit permit;
    std::uint64_t per_reader_bytes = 0;
};

/// Readers and merge queue prepared before creating a compaction output file.
struct admitted_file_merge {
    merge_resources resources;
    std::optional<admitted_merge_head_queue> heap;
};

/// Reserve memory for file readers, with optional failure injection in tests.
[[nodiscard]] std::optional<bioseq::parallelism::memory_permit>
try_reserve_merge_reader_bytes(
    std::uint64_t bytes,
    bioseq::parallelism::memory_admission_failure* failure) noexcept {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_sorted_run_reader_admission_injection_for_test ==
            sorted_run_reader_admission_injection::request_ceiling &&
        bytes > g_sorted_run_reader_request_ceiling_for_test) {
        if (failure != nullptr) {
            *failure = bioseq::parallelism::memory_admission_failure{
                .reason =
                    bioseq::parallelism::memory_admission_failure_reason::budget_exhausted,
                .requested_bytes = bytes,
                .budget_ceiling_bytes = g_sorted_run_reader_request_ceiling_for_test,
                .allocatable_after_reservations_bytes =
                    g_sorted_run_reader_request_ceiling_for_test,
            };
        }
        return std::nullopt;
    }
    if (g_sorted_run_reader_admission_injection_for_test ==
        sorted_run_reader_admission_injection::system_pressure_once) {
        g_sorted_run_reader_admission_injection_for_test =
            sorted_run_reader_admission_injection::none;
        g_sorted_run_reader_request_ceiling_for_test = 0;
        if (failure != nullptr) {
            *failure = bioseq::parallelism::memory_admission_failure{
                .reason = bioseq::parallelism::memory_admission_failure_reason::system_pressure,
                .requested_bytes = bytes,
            };
        }
        return std::nullopt;
    }
#endif
    return bioseq::parallelism::memory_admission_controller::instance().try_reserve(bytes,
                                                                                    failure);
}

/**
 * Reserve reader buffers, trying smaller buffers if the memory budget is full.
 * Stop on system memory pressure or size overflow. Retries use whole-pair
 * buffer sizes and never fall below the minimum reader size.
 */
[[nodiscard]] std::optional<admitted_file_reader_budget>
try_admit_file_reader_budget(
    std::size_t reader_count,
    std::uint64_t preferred_per_reader_bytes,
    bioseq::parallelism::memory_admission_failure* terminal_failure = nullptr) noexcept {
    if (terminal_failure != nullptr) {
        *terminal_failure = {};
    }
    std::uint64_t per_reader_bytes = std::clamp<std::uint64_t>(
        preferred_per_reader_bytes,
        minimum_merge_reader_buffer_bytes,
        maximum_merge_reader_buffer_bytes);
    for (;;) {
        const std::uint64_t logical_reader_bytes = bioseq::common::saturating_add(
            bioseq::common::saturating_mul(per_reader_bytes,
                                           static_cast<std::uint64_t>(reader_count)),
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(reader_count),
                                           static_cast<std::uint64_t>(sizeof(file_reader))));
        const std::uint64_t reader_headroom = bioseq::common::saturating_mul(
            logical_reader_bytes,
            bioseq::parallelism::k_vector_preallocation_headroom_factor);
        if (logical_reader_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            reader_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::nullopt;
        }

        bioseq::parallelism::memory_admission_failure admission_failure{};
        auto permit = try_reserve_merge_reader_bytes(reader_headroom, &admission_failure);
        if (permit) {
            return admitted_file_reader_budget{
                .permit = std::move(*permit),
                .per_reader_bytes = per_reader_bytes,
            };
        }
        if (admission_failure.reason !=
                bioseq::parallelism::memory_admission_failure_reason::budget_exhausted ||
            per_reader_bytes == minimum_merge_reader_buffer_bytes) {
            if (terminal_failure != nullptr) {
                *terminal_failure = admission_failure;
            }
            return std::nullopt;
        }

        const std::uint64_t halved = per_reader_bytes / 2u;
        const std::uint64_t wire_aligned =
            halved - (halved % static_cast<std::uint64_t>(sizeof(sorted_pair_wire)));
        per_reader_bytes = std::max(minimum_merge_reader_buffer_bytes, wire_aligned);
    }
}

} // namespace bioseq::storage::indexing

export namespace bioseq::storage::indexing {

/// Keep sorted runs in memory or temporary files for repeated merges.
class sorted_pair_run_store {
  public:
    sorted_pair_run_store() = default;
    sorted_pair_run_store(const sorted_pair_run_store&) = delete;
    sorted_pair_run_store& operator=(const sorted_pair_run_store&) = delete;
    sorted_pair_run_store(sorted_pair_run_store&& other) noexcept {
        steal_(std::move(other));
    }
    sorted_pair_run_store& operator=(sorted_pair_run_store&& other) noexcept {
        if (this != &other) {
            release_runs_noexcept();
            steal_(std::move(other));
        }
        return *this;
    }
    ~sorted_pair_run_store() {
        release_runs_noexcept();
    }

    /// Return the total resident and file run count.
    [[nodiscard]] std::uint64_t run_count() const noexcept {
        return static_cast<std::uint64_t>(memory_runs_.size() + file_runs_.size());
    }
    /// Return the file-backed run count.
    [[nodiscard]] std::uint64_t file_run_count() const noexcept {
        return static_cast<std::uint64_t>(file_runs_.size());
    }
    /// Return bytes held by runs in memory.
    [[nodiscard]] std::uint64_t resident_run_bytes() const noexcept {
        return memory_run_bytes_;
    }
    /// Return run payload bytes spilled from memory, excluding compaction output.
    [[nodiscard]] std::uint64_t spilled_run_bytes() const noexcept {
        return spilled_run_bytes_;
    }
    /// Return a snapshot of run and merge counters.
    [[nodiscard]] sorted_run_store_stats stats() const noexcept {
        sorted_run_store_stats out = stats_;
        out.resident_runs = static_cast<std::uint64_t>(memory_runs_.size());
        out.file_runs = static_cast<std::uint64_t>(file_runs_.size());
        out.resident_bytes = memory_run_bytes_;
        out.resident_peak_bytes = memory_run_peak_bytes_;
        out.spilled_bytes = spilled_run_bytes_;
        return out;
    }

    /// Copy a borrowed sorted run to a temporary file and return its run index.
    [[nodiscard]] std::size_t spill_sorted_run(std::span<const sorted_pair_t> sorted);

    /// Take a sorted vector, keeping it in memory if a reservation is available.
    [[nodiscard]] std::size_t spill_sorted_run_owned(std::vector<sorted_pair_t> sorted);

    /// Take a sorted vector and the reservation already covering its capacity.
    [[nodiscard]] std::size_t
    spill_sorted_run_owned_admitted(std::vector<sorted_pair_t> sorted,
                                    std::uint64_t admitted_bytes,
                                    bioseq::parallelism::memory_permit permit);
    /// Spill memory runs until enough budget is available or no memory runs remain.
    void demote_resident_runs_until_available(std::uint64_t bytes_needed);

    /// Free the reusable spill buffer after a completed write, keeping the stored runs.
    void release_completed_write_buffer();

    /// Move the reusable spill buffer to a store with no buffer of its own.
    [[nodiscard]] bool
    try_transfer_completed_write_buffer_to(sorted_pair_run_store& destination) noexcept;

    /// Merge every run and invoke a callable once per minimizer hash.
    template <class p_function> void merge_runs(p_function&& on_kmer) {
        sorted_pair_kmer_sink sink{
            .ctx = std::addressof(on_kmer),
            .call =
                [](void* ctx, std::uint64_t kmer, const sorted_pair_position_cursor& cursor) {
                    (*static_cast<std::remove_reference_t<p_function>*>(ctx))(kmer, cursor);
                },
        };
        merge_runs_erased(sink);
    }

    /// Discard runs held in memory and release their reservations without PostgreSQL calls.
    void release_resident_runs_noexcept() noexcept;

    /// Free run memory and leave temporary files to PostgreSQL cleanup.
    void release_runs_noexcept() noexcept;

    /// Close temporary files and free all run memory after successful use.
    void release_runs();

    /// Mark the store unusable and leave PostgreSQL resources to error cleanup.
    void abandon_after_pg_error_noexcept() noexcept {
        poisoned_ = true;
        release_runs_noexcept();
    }

  private:
    /// Move resources before their reservations so the memory stays accounted for.
    void steal_(sorted_pair_run_store&& other) noexcept;

    /// Merge runs through a callback and keep successful runs available for replay.
    void merge_runs_erased(sorted_pair_kmer_sink sink);

    /// Merge all runs using the supplied reader and position buffers.
    void merge_runs_erased_impl_(sorted_pair_kmer_sink sink, merge_resources& resources);

    /// Close all file handles after successful use.
    void finalize_file_handles_success_();

    /// Reserve memory before creating the reusable file-write buffer.
    [[nodiscard]] bool try_ensure_write_buffer_(std::size_t requested_bytes);

    /// Write a sorted run using the reusable buffer, falling back to a stack buffer.
    [[nodiscard]] std::size_t spill_sorted_run_impl_(std::span<const sorted_pair_t> sorted);

    /// Spill a sorted run through a fixed stack buffer.
    [[nodiscard]] std::size_t spill_sorted_run_direct_(std::span<const sorted_pair_t> sorted);

    /// Spill memory runs to free budget, with failure state handled by the caller.
    void demote_resident_runs_until_available_impl_(std::uint64_t bytes_needed);

    /// Spill the largest run held in memory, returning false when none remain.
    [[nodiscard]] bool demote_largest_resident_run_();

    /// Choose a reader-buffer budget from the memory currently available.
    [[nodiscard]] std::uint64_t merge_reader_budget_bytes_() const;

    /// Reduce the file count so minimum-sized readers fit the merge budget.
    void compact_file_runs_until_bounded_();

    /// Merge file groups until at most the requested number remain.
    void compact_file_runs_to_output_count_(std::size_t target_output_runs);

    /// Reserve memory and prepare readers and a queue for one group of files.
    [[nodiscard]] std::unique_ptr<admitted_file_merge> try_prepare_compaction_merge_(
        std::span<buffer_file_run> runs,
        bioseq::parallelism::memory_admission_failure& terminal_failure,
        sorted_run_merge_admission_site& terminal_site);

    /// Merge a prepared group of runs into a new temporary file.
    [[nodiscard]] buffer_file_run
    merge_file_run_group_to_new_buffile_(admitted_file_merge& admitted);

    /// Merge sorted file runs into one writer callback.
    template <class p_writer>
    void merge_sorted_file_runs_to_writer_(merge_resources& resources,
                                           admitted_merge_head_queue& admitted_heap,
                                           p_writer&& writer);
    /// Reserve and prepare readers for all file runs, returning false on memory refusal.
    [[nodiscard]] bool try_open_file_readers_(
        merge_resources& resources,
        bioseq::parallelism::memory_admission_failure& terminal_failure);

    /// Prepare the merge queue and readers, reducing the file count if the budget is full.
    [[nodiscard]] admitted_merge_head_queue open_file_readers_(merge_resources& resources);

    bioseq::parallelism::memory_permit memory_runs_metadata_permit_;
    std::vector<memory_run> memory_runs_;
    bioseq::parallelism::memory_permit file_runs_metadata_permit_;
    std::vector<buffer_file_run> file_runs_;
    std::uint64_t memory_run_bytes_ = 0;
    std::uint64_t memory_run_peak_bytes_ = 0;
    std::uint64_t spilled_run_bytes_ = 0;
    // Declare the permit first so the write buffer is freed before it.
    bioseq::parallelism::memory_permit write_buffer_permit_;
    std::vector<sorted_pair_wire> write_buffer_;
    sorted_run_store_stats stats_{};
    bool poisoned_ = false;
    bool finalized_ = false;
};

} // namespace bioseq::storage::indexing

namespace bioseq::storage::indexing {

std::uint64_t sorted_pair_position_cursor::total_count() const noexcept {
    const auto* acc = static_cast<const position_accumulator*>(impl_);
    return acc == nullptr ? 0 : acc->total;
}

void sorted_pair_position_cursor::for_each_batch_erased(position_batch_sink sink) const {
    auto* acc = const_cast<position_accumulator*>(static_cast<const position_accumulator*>(impl_));
    if (acc == nullptr) {
        return;
    }
    acc->emit_batches(sink);
}

void sorted_pair_run_store::steal_(sorted_pair_run_store&& other) noexcept {
    // The destination has no allocated capacity. Swap each vector before
    // moving its permit so the source is left empty and every allocation
    // remains covered by a reservation.
    memory_runs_.swap(other.memory_runs_);
    memory_runs_metadata_permit_ = std::move(other.memory_runs_metadata_permit_);
    file_runs_.swap(other.file_runs_);
    file_runs_metadata_permit_ = std::move(other.file_runs_metadata_permit_);
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

bool sorted_pair_run_store::try_ensure_write_buffer_(std::size_t requested_bytes) {
    if (write_buffer_permit_) {
        return true;
    }
    requested_bytes = std::max<std::size_t>(requested_bytes, sizeof(sorted_pair_wire));
    if (requested_bytes >
        (std::numeric_limits<std::size_t>::max)() - (sizeof(sorted_pair_wire) - 1u)) {
        return false;
    }
    const std::uint64_t logical_bytes = static_cast<std::uint64_t>(requested_bytes);
    const std::uint64_t admitted_headroom = bioseq::common::saturating_mul(
        logical_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor);
    if (admitted_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
        return false;
    }
    auto permit =
        bioseq::parallelism::memory_admission_controller::instance().try_reserve(admitted_headroom);
    if (!permit) {
        return false;
    }
    struct candidate_write_buffer {
        bioseq::parallelism::memory_permit permit;
        std::vector<sorted_pair_wire> values;
    } candidate{
        .permit = std::move(*permit),
        .values = {},
    };
    const std::size_t requested_pairs =
        (requested_bytes + sizeof(sorted_pair_wire) - 1u) / sizeof(sorted_pair_wire);
    candidate.values.reserve(requested_pairs);
    if (candidate.values.capacity() >
        (std::numeric_limits<std::uint64_t>::max)() / sizeof(sorted_pair_wire)) {
        return false;
    }
    const std::uint64_t exact_bytes =
        static_cast<std::uint64_t>(candidate.values.capacity()) * sizeof(sorted_pair_wire);
    if (exact_bytes > candidate.permit.bytes()) {
        return false;
    }
    candidate.permit.shrink_to(exact_bytes);
    write_buffer_ = std::move(candidate.values);
    write_buffer_permit_ = std::move(candidate.permit);
    return true;
}

std::size_t sorted_pair_run_store::spill_sorted_run_direct_(std::span<const sorted_pair_t> sorted) {
    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
    if (!try_ensure_admitted_vector_capacity(
            file_runs_, file_runs_metadata_permit_, file_runs_.size() + 1u)) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    BufFile* file = create_temp_buffile_as_cpp();
    if (file == nullptr) {
        report_sorted_run_error(sorted_run_error::temp_file_create_failed);
    }
    file_runs_.push_back(buffer_file_run{.file = file});
    buffer_file_run& run = file_runs_.back();

    std::array<sorted_pair_wire, 1024> stack_buffer{};
    for (std::size_t off = 0; off < sorted.size(); off += stack_buffer.size()) {
        const std::size_t n = std::min(stack_buffer.size(), sorted.size() - off);
        for (std::size_t i = 0; i < n; ++i) {
            const sorted_pair_t& p = sorted[off + i];
            stack_buffer[i] = sorted_pair_wire{.kmer = p.first, .encoded = p.second};
        }
        write_buffile_as_cpp(file, stack_buffer.data(), n * sizeof(sorted_pair_wire));
        check_for_interrupts_as_cpp();
    }

    run.pair_count = static_cast<std::uint64_t>(sorted.size());
    run.byte_count = static_cast<std::uint64_t>(sorted.size() * sizeof(sorted_pair_wire));
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

std::size_t sorted_pair_run_store::spill_sorted_run_impl_(std::span<const sorted_pair_t> sorted) {
    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
    if (!try_ensure_admitted_vector_capacity(
            file_runs_, file_runs_metadata_permit_, file_runs_.size() + 1u)) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    BufFile* file = create_temp_buffile_as_cpp();
    if (file == nullptr) {
        report_sorted_run_error(sorted_run_error::temp_file_create_failed);
    }
    file_runs_.push_back(buffer_file_run{.file = file});
    buffer_file_run& run = file_runs_.back();

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
            write_buffer_.push_back(sorted_pair_wire{.kmer = p.first, .encoded = p.second});
        }
        write_buffile_as_cpp(file, write_buffer_.data(), n * sizeof(sorted_pair_wire));
        check_for_interrupts_as_cpp();
    }

    run.pair_count = static_cast<std::uint64_t>(sorted.size());
    run.byte_count = static_cast<std::uint64_t>(sorted.size() * sizeof(sorted_pair_wire));
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

std::size_t sorted_pair_run_store::spill_sorted_run(std::span<const sorted_pair_t> sorted) {
    if (poisoned_) {
        report_sorted_run_error(sorted_run_error::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(sorted_run_error::finalized_store_reused);
    }
    poisoned_ = true;
    const std::size_t run_index = spill_sorted_run_impl_(sorted);
    poisoned_ = false;
    return run_index;
}

bool sorted_pair_run_store::demote_largest_resident_run_() {
    if (memory_runs_.empty()) {
        return false;
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i < memory_runs_.size(); ++i) {
        if (memory_runs_[i].admitted_bytes > memory_runs_[best].admitted_bytes) {
            best = i;
        }
    }

    memory_run& run = memory_runs_[best];
    const std::uint64_t admitted_bytes = run.admitted_bytes;
    spill_sorted_run_direct_(std::span<const sorted_pair_t>(run.pairs.data(), run.pairs.size()));
    bioseq::parallelism::release_admitted_vector_capacity_before_permit(run.pairs, run.permit);
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

void sorted_pair_run_store::demote_resident_runs_until_available_impl_(std::uint64_t bytes_needed) {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    while (!memory_runs_.empty()) {
        const std::uint64_t available = admission.allocatable_after_reservations();
        if (available == (std::numeric_limits<std::uint64_t>::max)() || available >= bytes_needed) {
            return;
        }
        if (!demote_largest_resident_run_()) {
            return;
        }
    }
}

void sorted_pair_run_store::demote_resident_runs_until_available(std::uint64_t bytes_needed) {
    if (poisoned_) {
        report_sorted_run_error(sorted_run_error::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(sorted_run_error::finalized_store_reused);
    }
    poisoned_ = true;
    demote_resident_runs_until_available_impl_(bytes_needed);
    poisoned_ = false;
}

std::size_t sorted_pair_run_store::spill_sorted_run_owned(std::vector<sorted_pair_t> sorted) {
    const std::uint64_t capacity_bytes =
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(sorted.capacity()),
                                       static_cast<std::uint64_t>(sizeof(sorted_pair_t)));
    if (auto permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
            capacity_bytes)) {
        return spill_sorted_run_owned_admitted(
            std::move(sorted), capacity_bytes, std::move(*permit));
    }

    ++stats_.spill_count_from_pressure;
    return spill_sorted_run(std::span<const sorted_pair_t>(sorted.data(), sorted.size()));
}

std::size_t
sorted_pair_run_store::spill_sorted_run_owned_admitted(std::vector<sorted_pair_t> sorted,
                                                       std::uint64_t admitted_bytes,
                                                       bioseq::parallelism::memory_permit permit) {
    memory_run candidate{
        .permit = std::move(permit),
        .admitted_bytes = admitted_bytes,
        .pairs = std::move(sorted),
    };
    if (poisoned_) {
        report_sorted_run_error(sorted_run_error::poisoned_store_reused);
    }
    if (finalized_) {
        report_sorted_run_error(sorted_run_error::finalized_store_reused);
    }
    if (!candidate.permit && admitted_bytes != 0) {
        report_sorted_run_error(sorted_run_error::admitted_run_without_permit);
    }
    const std::uint64_t exact_capacity_bytes =
        bioseq::common::saturating_mul(static_cast<std::uint64_t>(candidate.pairs.capacity()),
                                       static_cast<std::uint64_t>(sizeof(sorted_pair_t)));
    if (exact_capacity_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        exact_capacity_bytes != admitted_bytes || candidate.permit.bytes() < exact_capacity_bytes ||
        memory_run_bytes_ > (std::numeric_limits<std::uint64_t>::max)() - admitted_bytes) {
        report_sorted_run_error(sorted_run_error::admitted_run_without_permit);
    }
    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
    candidate.permit.shrink_to(exact_capacity_bytes);
    const std::uint64_t next_memory_run_bytes = memory_run_bytes_ + admitted_bytes;
    if (!try_ensure_admitted_vector_capacity(
            memory_runs_, memory_runs_metadata_permit_, memory_runs_.size() + 1u)) {
        report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
    }
    memory_runs_.push_back(std::move(candidate));
    memory_run_bytes_ = next_memory_run_bytes;
    memory_run_peak_bytes_ = std::max(memory_run_peak_bytes_, memory_run_bytes_);
    stats_.resident_runs = static_cast<std::uint64_t>(memory_runs_.size());
    stats_.resident_bytes = memory_run_bytes_;
    stats_.resident_peak_bytes = memory_run_peak_bytes_;
    return run_index;
}

std::uint64_t sorted_pair_run_store::merge_reader_budget_bytes_() const {
#ifdef BIOSEQ_TEST_INDEXES
    if (g_sorted_run_merge_reader_budget_for_test != 0u) {
        return g_sorted_run_merge_reader_budget_for_test;
    }
#endif
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t available = admission.allocatable_after_reservations();
    if (available == 0) {
        return std::uint64_t{1} << 20;
    }
    if (available == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::uint64_t{256} << 20;
    }
    return std::clamp<std::uint64_t>(
        available / 8, std::uint64_t{1} << 20, std::uint64_t{256} << 20);
}

bool sorted_pair_run_store::try_open_file_readers_(
    merge_resources& resources,
    bioseq::parallelism::memory_admission_failure& terminal_failure) {
    terminal_failure = {};
    if (file_runs_.empty()) {
        return true;
    }
    const std::uint64_t reader_budget = merge_reader_budget_bytes_();
    const std::uint64_t preferred_per_reader =
        std::clamp<std::uint64_t>(reader_budget / std::max<std::size_t>(1, file_runs_.size()),
                                  minimum_merge_reader_buffer_bytes,
                                  maximum_merge_reader_buffer_bytes);
    bioseq::parallelism::memory_admission_failure admission_failure{};
    auto admitted = try_admit_file_reader_budget(
        file_runs_.size(), preferred_per_reader, &admission_failure);
    if (!admitted) {
        terminal_failure = admission_failure;
        return false;
    }
    const std::uint64_t per_reader = admitted->per_reader_bytes;
    resources.reader_budget_permit = std::move(admitted->permit);
    resources.readers.reserve(file_runs_.size());

    for (buffer_file_run& run : file_runs_) {
        if (seek_buffile_as_cpp(run.file, 0, 0, SEEK_SET) != 0) {
            report_sorted_run_error(sorted_run_error::run_rewind_failed);
        }
        file_reader reader{};
        reader.file = run.file;
        reader.remaining = run.pair_count;
        reader.buffer.reserve(std::max<std::uint64_t>(1, per_reader / sizeof(sorted_pair_wire)));
        resources.readers.push_back(std::move(reader));
    }
    const std::uint64_t exact_reader_bytes = file_reader_owned_capacity_bytes(resources.readers);
    if (exact_reader_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
        exact_reader_bytes > resources.reader_budget_permit.bytes()) {
        resources.finish_success();
        return false;
    }
    resources.reader_budget_permit.shrink_to(exact_reader_bytes);
    stats_.merge_reader_buffer_bytes += exact_reader_bytes;
    return true;
}

admitted_merge_head_queue
sorted_pair_run_store::open_file_readers_(merge_resources& resources) {
    bioseq::parallelism::memory_admission_failure terminal_admission_failure{};
    for (;;) {
        {
            if (file_runs_.size() >
                (std::numeric_limits<std::size_t>::max)() - memory_runs_.size()) {
                report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
            }
            auto admitted_heap = make_admitted_merge_head_queue(
                memory_runs_.size() + file_runs_.size());
            if (try_open_file_readers_(resources, terminal_admission_failure)) {
                return admitted_heap;
            }
        }
        // Free the final merge queue before reserving memory for compaction.
        const bool may_compact =
            terminal_admission_failure.reason ==
                bioseq::parallelism::memory_admission_failure_reason::budget_exhausted;
        resources.finish_success();
        if (!may_compact || file_runs_.size() <= 1) {
#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS) || defined(BIOSEQ_TEST_INDEXES)
            if (terminal_admission_failure.has_failure()) {
                emit_terminal_merge_admission_failure(
                    sorted_run_merge_admission_site::final_readers,
                    terminal_admission_failure);
            }
#endif
            report_sorted_run_error(sorted_run_error::merge_reader_admission_failed);
        }
        compact_file_runs_to_output_count_(std::max<std::size_t>(1, file_runs_.size() / 2));
    }
}

} // namespace bioseq::storage::indexing
