/**
 * @file sorted_pair_runs_pg.cppm
 * @brief PostgreSQL BufFile-backed sorted `(kmer, encoded)` run store for
 *        minimizers_per_sequence builds.
 */
module;
#include "common.h"

extern "C" {
#include "storage/buffile.h"
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
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

enum class SortedRunError {
    position_spill_create_failed,
    position_spill_rewind_failed,
    temp_file_create_failed,
    admitted_run_without_permit,
    run_rewind_failed,
    merge_reader_admission_failed,
    compacted_run_rewind_failed,
    compact_temp_file_create_failed,
    poisoned_store_reused,
};

void report_sorted_run_error(SortedRunError error);

template <class T>
void release_vector_storage_noexcept(std::vector<T>& values) noexcept {
    try {
        std::vector<T> empty;
        values.swap(empty);
    } catch (...) {
        values.clear();
    }
}

void close_buffile_noexcept(BufFile*& file) noexcept {
    if (file == nullptr) {
        return;
    }
    PG_TRY();
    {
        BufFileClose(file);
        file = nullptr;
    }
    PG_CATCH();
    {
        FlushErrorState();
        file = nullptr;
    }
    PG_END_TRY();
}

[[nodiscard]] BufFile* create_temp_buffile_as_cpp() {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [] { return BufFileCreateTemp(false); });
}

void write_buffile_as_cpp(
        BufFile* file,
        const void* data,
        std::size_t bytes) {
    bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { BufFileWrite(file, data, bytes); });
}

void read_buffile_exact_as_cpp(
        BufFile* file,
        void* data,
        std::size_t bytes) {
    bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { BufFileReadExact(file, data, bytes); });
}

[[nodiscard]] int seek_buffile_as_cpp(
        BufFile* file,
        int fileno,
        off_t offset,
        int whence) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { return BufFileSeek(file, fileno, offset, whence); });
}

[[nodiscard]] int64 size_buffile_as_cpp(BufFile* file) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [&] { return BufFileSize(file); });
}

void check_for_interrupts_as_cpp() {
    bioseq::pg_utils::invoke_pg_as_cpp([] { CHECK_FOR_INTERRUPTS(); });
}

struct MemoryRun {
    std::vector<sorted_pair_t> pairs;
    std::uint64_t admitted_bytes = 0;
    bioseq::parallelism::MemoryPermit permit;
};

struct BufFileRun {
    BufFile* file = nullptr;
    std::uint64_t pair_count = 0;
    std::uint64_t byte_count = 0;
};

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
    std::vector<std::uint64_t> resident;
    std::vector<std::uint64_t> read_buffer;
    bioseq::parallelism::MemoryPermit resident_permit;
    BufFile* spill = nullptr;
    std::uint64_t total = 0;
    std::uint64_t spilled_bytes = 0;
    bool resident_batch_ready = false;

    void reset_for_kmer() {
        resident.clear();
        read_buffer.clear();
        if (spill != nullptr) {
            close_buffile_noexcept(spill);
        }
        total = 0;
        spilled_bytes = 0;
    }

    void ensure_resident_batch() {
        if (resident_batch_ready) {
            return;
        }
        auto& admission = bioseq::parallelism::MemoryAdmissionController::instance();
        std::uint64_t batch_bytes = kTargetPositionBatchBytes;
        const std::uint64_t available = admission.allocatable_after_reservations();
        if (available != (std::numeric_limits<std::uint64_t>::max)()) {
            batch_bytes = std::min(batch_bytes, available);
        }
        batch_bytes = std::max(batch_bytes, kMinPositionBatchBytes);
        for (;;) {
            if (auto permit = admission.try_reserve(batch_bytes)) {
                resident.reserve(batch_bytes / sizeof(std::uint64_t));
                read_buffer.reserve(batch_bytes / sizeof(std::uint64_t));
                resident_permit = std::move(*permit);
                resident_batch_ready = true;
                return;
            }
            if (batch_bytes == kMinPositionBatchBytes) {
                break;
            }
            batch_bytes = std::max(batch_bytes / 2, kMinPositionBatchBytes);
        }
        // Keep the cursor usable when resident sorted runs have consumed the
        // formal admission budget. This is a fixed 32 KiB scratch batch; larger
        // k-mer hit lists still spill through BufFile in the usual way.
        resident.reserve(kMinPositionBatchBytes / sizeof(std::uint64_t));
        read_buffer.reserve(kMinPositionBatchBytes / sizeof(std::uint64_t));
        resident_batch_ready = true;
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

    void release_noexcept() noexcept {
        resident_permit.release();
        release_vector_storage_noexcept(resident);
        release_vector_storage_noexcept(read_buffer);
        if (spill != nullptr) {
            close_buffile_noexcept(spill);
        }
        total = 0;
        spilled_bytes = 0;
        resident_batch_ready = false;
    }

    void abandon_pg_resources_noexcept() noexcept {
        resident_permit.release();
        release_vector_storage_noexcept(resident);
        release_vector_storage_noexcept(read_buffer);
        // A PostgreSQL ERROR may have interrupted a BufFile operation. Do not
        // issue another PG call while unwinding; ResourceOwner cleanup at the
        // outer statement/transaction boundary owns this file.
        spill = nullptr;
        total = 0;
        spilled_bytes = 0;
        resident_batch_ready = false;
    }
};

struct MergeResources {
    std::vector<FileReader> readers;
    bioseq::parallelism::MemoryPermit reader_budget_permit;
    PositionAccumulator positions;

    void release_noexcept() noexcept {
        release_vector_storage_noexcept(readers);
        reader_budget_permit.release();
        positions.release_noexcept();
    }


    void abandon_pg_resources_noexcept() noexcept {
        release_vector_storage_noexcept(readers);
        reader_budget_permit.release();
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

    std::vector<MemoryRun> memory_runs_;
    std::vector<BufFileRun> file_runs_;
    std::uint64_t memory_run_bytes_ = 0;
    std::uint64_t memory_run_peak_bytes_ = 0;
    std::uint64_t spilled_run_bytes_ = 0;
    std::vector<SortedPairWire> write_buffer_;
    bioseq::parallelism::MemoryPermit write_buffer_permit_;
    SortedRunStoreStats stats_{};
    bool poisoned_ = false;
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
    memory_runs_ = std::move(other.memory_runs_);
    file_runs_ = std::move(other.file_runs_);
    memory_run_bytes_ = other.memory_run_bytes_;
    memory_run_peak_bytes_ = other.memory_run_peak_bytes_;
    spilled_run_bytes_ = other.spilled_run_bytes_;
    write_buffer_ = std::move(other.write_buffer_);
    write_buffer_permit_ = std::move(other.write_buffer_permit_);
    stats_ = other.stats_;
    poisoned_ = other.poisoned_;

    other.file_runs_.clear();
    other.memory_run_bytes_ = 0;
    other.memory_run_peak_bytes_ = 0;
    other.spilled_run_bytes_ = 0;
    other.stats_ = {};
    other.poisoned_ = true;
}

	bool SortedPairRunStore::try_ensure_write_buffer_(std::size_t requested_bytes) {
	    if (write_buffer_permit_) {
	        return true;
	    }
	    requested_bytes = std::max<std::size_t>(requested_bytes, sizeof(SortedPairWire));
	    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
	        .try_reserve(static_cast<std::uint64_t>(requested_bytes));
	    if (!permit) {
	        return false;
	    }
	    write_buffer_.reserve(requested_bytes / sizeof(SortedPairWire));
	    write_buffer_permit_ = std::move(*permit);
	    return true;
	}

	std::size_t SortedPairRunStore::spill_sorted_run_direct_(
	        std::span<const sorted_pair_t> sorted) {
	    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
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
	    BufFile* file = create_temp_buffile_as_cpp();
    if (file == nullptr) {
        report_sorted_run_error(SortedRunError::temp_file_create_failed);
    }
	    file_runs_.push_back(BufFileRun{.file = file});
	    BufFileRun& run = file_runs_.back();

	    if (!try_ensure_write_buffer_(std::size_t{8} << 20)) {
	        close_buffile_noexcept(file_runs_.back().file);
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
	    run.permit.release();
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
    auto owned = std::make_unique<std::vector<sorted_pair_t>>(
        std::move(sorted));
    return spill_sorted_run(
        std::span<const sorted_pair_t>(owned->data(), owned->size()));
}

std::size_t SortedPairRunStore::spill_sorted_run_owned_admitted(
        std::vector<sorted_pair_t> sorted,
        std::uint64_t admitted_bytes,
        bioseq::parallelism::MemoryPermit permit) {
    if (!permit && admitted_bytes != 0) {
        report_sorted_run_error(SortedRunError::admitted_run_without_permit);
    }
    const std::size_t run_index = memory_runs_.size() + file_runs_.size();
    memory_run_bytes_ += admitted_bytes;
    memory_run_peak_bytes_ = std::max(memory_run_peak_bytes_, memory_run_bytes_);
    memory_runs_.push_back(MemoryRun{
        .pairs = std::move(sorted),
        .admitted_bytes = admitted_bytes,
        .permit = std::move(permit),
    });
    stats_.resident_runs = static_cast<std::uint64_t>(memory_runs_.size());
    stats_.resident_bytes = memory_run_bytes_;
    stats_.resident_peak_bytes = memory_run_peak_bytes_;
    return run_index;
}

std::uint64_t SortedPairRunStore::merge_reader_budget_bytes_() const {
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
        bioseq::parallelism::saturating_mul(
            per_reader,
            static_cast<std::uint64_t>(file_runs_.size()));
    auto permit = bioseq::parallelism::MemoryAdmissionController::instance()
        .try_reserve(total_reader_bytes);
    if (!permit) {
        if (file_runs_.size() > 1) {
            return false;
        }
        // The store has already compacted to one file run. Keep the merge
        // streaming with a fixed 32 KiB reader instead of failing a query after
        // the large sorted runs consumed the formal admission budget.
        per_reader = std::uint64_t{32} << 10;
    } else {
        resources.reader_budget_permit = std::move(*permit);
    }

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
        stats_.merge_reader_buffer_bytes += per_reader;
    }
    return true;
}

void SortedPairRunStore::open_file_readers_(MergeResources& resources) {
    while (!try_open_file_readers_(resources)) {
        resources.release_noexcept();
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
    resources.release_noexcept();
    resources.readers.reserve(runs.size());
    std::priority_queue<MergeHead, std::vector<MergeHead>, MergeHeadGreater> heap;
    for (BufFileRun& run : runs) {
        if (seek_buffile_as_cpp(run.file, 0, 0, SEEK_SET) != 0) {
            report_sorted_run_error(SortedRunError::compacted_run_rewind_failed);
        }
        FileReader reader{};
        reader.file = run.file;
        reader.remaining = run.pair_count;
        reader.buffer.reserve(std::size_t{1} << 15);
        if (reader.read_next()) {
            resources.readers.push_back(std::move(reader));
            const std::size_t idx = resources.readers.size() - 1;
            heap.push(MergeHead{
                .kmer = resources.readers.back().current.kmer,
                .encoded = resources.readers.back().current.encoded,
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
        resources->release_noexcept();
        out_file = nullptr;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        resources->abandon_pg_resources_noexcept();
        // out_file remains registered with the current ResourceOwner.
        out_file = nullptr;
        throw;
    } catch (...) {
        resources->release_noexcept();
        close_buffile_noexcept(out_file);
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
        std::vector<BufFileRun> next;
        try {
            next.reserve(target_output_runs);
            for (std::size_t lo = 0; lo < file_runs_.size(); lo += group_size) {
                const std::size_t hi = std::min(file_runs_.size(), lo + group_size);
                next.push_back(merge_file_run_group_to_new_buffile_(lo, hi));
                for (std::size_t i = lo; i < hi; ++i) {
                    close_buffile_noexcept(file_runs_[i].file);
                }
            }
            file_runs_ = std::move(next);
            ++stats_.merge_pass_count;
        } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
            for (BufFileRun& run : next) {
                // ResourceOwner cleanup owns files touched by the failed PG
                // operation; do not issue more PostgreSQL calls here.
                run.file = nullptr;
            }
            throw;
        } catch (...) {
            for (BufFileRun& run : next) {
                close_buffile_noexcept(run.file);
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

    std::priority_queue<MergeHead, std::vector<MergeHead>, MergeHeadGreater> heap;
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
    poisoned_ = true;
    auto resources = std::make_unique<MergeResources>();
    try {
        merge_runs_erased_impl_(sink, *resources);
        resources->release_noexcept();
        poisoned_ = false;
    } catch (const bioseq::pg_utils::PostgresErrorAsCpp&) {
        resources->abandon_pg_resources_noexcept();
        throw;
    } catch (...) {
        // The callback may throw an ordinary C++ exception. All PG leaves have
        // already restored PostgreSQL's exception stack, so normal cleanup is
        // safe and the original C++ exception remains authoritative.
        resources->release_noexcept();
        throw;
    }
}

void SortedPairRunStore::release_resident_runs_noexcept() noexcept {
    for (MemoryRun& run : memory_runs_) {
        run.permit.release();
    }
    memory_runs_.clear();
    memory_run_bytes_ = 0;
    memory_run_peak_bytes_ = 0;
}

void SortedPairRunStore::release_runs_noexcept() noexcept {
    release_resident_runs_noexcept();
    write_buffer_permit_.release();
    release_vector_storage_noexcept(write_buffer_);
    if (poisoned_) {
        for (BufFileRun& run : file_runs_) {
            // A failed BufFile leaf is owned by PostgreSQL's ResourceOwner.
            // Re-entering BufFileClose while propagating that error can mask
            // the original SQLSTATE or longjmp across C++ cleanup.
            run.file = nullptr;
        }
    } else {
        for (BufFileRun& run : file_runs_) {
            close_buffile_noexcept(run.file);
        }
    }
    file_runs_.clear();
    spilled_run_bytes_ = 0;
    stats_ = {};
}

void SortedPairRunStore::release_runs() {
    release_runs_noexcept();
}

} // namespace bioseq::storage::indexing
