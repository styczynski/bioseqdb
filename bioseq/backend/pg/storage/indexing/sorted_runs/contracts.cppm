/**
 * @file contracts.cppm
 * @brief Callbacks and statistics for merging sorted minimizer runs.
 */
module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.postgres.storage.indexing.sorted_runs:sorted_runs_contracts;

export namespace bioseq::storage::indexing {

/// Pair of minimizer hash and encoded sequence position.
using sorted_pair_t = std::pair<std::uint64_t, std::uint64_t>;

/// A callback and its context for reading a batch of encoded positions.
struct position_batch_sink {
    /// Caller-owned state passed to the callback.
    void* ctx = nullptr;
    /// Callback receiving a batch view valid only for that invocation.
    void (*call)(void*, std::span<const std::uint64_t>) = nullptr;
};

/// Read-only cursor over positions collected for one minimizer hash.
class sorted_pair_position_cursor {
  public:
    sorted_pair_position_cursor() noexcept = default;
    /// Borrow the position accumulator for the current minimizer hash.
    /// @param impl Accumulator that must outlive this cursor.
    explicit sorted_pair_position_cursor(const void* impl) noexcept : impl_(impl) {}

    /// @return Number of encoded positions, including those stored in temporary files.
    [[nodiscard]] std::uint64_t total_count() const noexcept;

    /// Visit each position batch before returning.
    /// @param sink Callback whose batch view is valid only during that call
    void for_each_batch_erased(position_batch_sink sink) const;

    /// Visit each position batch  before returning.
    /// @tparam p_function Callable accepting `std::span<const std::uint64_t>`.
    /// @param f Callback whose batch view is valid only during that call.
    template <class p_function> void for_each_batch(p_function&& f) const {
        position_batch_sink sink{
            .ctx = std::addressof(f),
            .call =
                [](void* ctx, std::span<const std::uint64_t> batch) {
                    (*static_cast<std::remove_reference_t<p_function>*>(ctx))(batch);
                },
        };
        for_each_batch_erased(sink);
    }

  private:
    friend class sorted_pair_run_store;
    const void* impl_ = nullptr;
};

/// A callback and its context for reading one hash and its positions.
struct sorted_pair_kmer_sink {
    /// Caller-owned state passed to each merge callback.
    void* ctx = nullptr;
    /// Callback receiving a hash and cursor valid only for that invocation.
    void (*call)(void*, std::uint64_t, const sorted_pair_position_cursor&) = nullptr;
};

/// Memory, temporary-file and erge counts for one sorted-run store.
struct sorted_run_store_stats {
    /// Runs currently kept in memory.
    std::uint64_t resident_runs = 0;
    /// Number of runs currently backed by PostgreSQL temporary files.
    std::uint64_t file_runs = 0;
    /// Bytes reserved for runs currently kept in memory.
    std::uint64_t resident_bytes = 0;
    /// Largest total memory reservation for runs at one time.
    std::uint64_t resident_peak_bytes = 0;
    /// Run payload bytes spilled from memory, excluding compaction output.
    std::uint64_t spilled_bytes = 0;
    /// Total temporary-file sizes recorded as run files are  written.
    std::uint64_t postgres_temp_file_bytes = 0;
    /// Number of run and compaction temporary files created.
    std::uint64_t postgres_temp_file_count = 0;
    /// Sum of buffer bytes reserved across merge-reader setups.
    std::uint64_t merge_reader_buffer_bytes = 0;
    /// Number of external merge passes.
    std::uint64_t merge_pass_count = 0;
    /// Runs written to a temporary file after a memory reservation was refused.
    std::uint64_t spill_count_from_pressure = 0;
    /// Runs moved from memory to temporary files.
    std::uint64_t resident_demotions = 0;
    /// Total bytes of positions written to temporary files while grouping by hash.
    std::uint64_t position_spill_bytes = 0;
    /// Number of position spill files created.
    std::uint64_t position_spill_count = 0;
};

#ifdef BIOSEQ_TEST_INDEXES
/**
 * One-shot PostgreSQL failures used by integration tests.
 * An "after" point raises an error after the file, operation has changed state,
 * so the caller must stop using the affected handles and let PostgreSQL clean up.
 */
enum class sorted_run_failure_injection : std::uint8_t {
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
    // Fail when creating a merge output file. Input  spill files do not count
    // toward this failure point
    compact_create_before,
};

/// Test outcomes for merge-reader memory reservations.
enum class sorted_run_reader_admission_injection : std::uint8_t {
    none,
    request_ceiling,
    system_pressure_once,
};

/// Fail the next matching PostgreSQL operation once.
/// @param point Operation and before/after position to fail.
void arm_sorted_run_pg_leaf_failure(sorted_run_failure_injection point) noexcept;

/// Fail a seleted matching PostgreSQL operation once.
/// @param point Operation and before/after position to fail.
/// @param operation Matching call number, starting at one.
void arm_sorted_run_pg_leaf_failure_on_nth(sorted_run_failure_injection point,
                                           std::uint64_t operation) noexcept;
/// Override the merge-reader memory budget for one backend test.
/// @param bytes Budget in bytes, or zero to use the production value.
void set_sorted_run_merge_reader_budget_for_test(std::uint64_t bytes) noexcept;
/// Override merge-reader memory reservation results for a test.
/// @param injection Test result checked before the memory controller.
/// @param request_ceiling_bytes Largest request accepted by `request_ceiling`.
void set_sorted_run_reader_admission_injection_for_test(
    sorted_run_reader_admission_injection injection,
    std::uint64_t request_ceiling_bytes = 0u) noexcept;
/// Clear every sorted-run failure and budget override.
void disarm_sorted_run_pg_leaf_failure() noexcept;

/// @return True if  a PostgreSQL operation is set to fail.
[[nodiscard]] bool sorted_run_pg_leaf_failure_is_armed() noexcept;
#endif

} // namespace bioseq::storage::indexing
