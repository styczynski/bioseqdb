/**
 * @file contracts.cppm
 * @brief Public sorted-run callbacks, statistics, and store contracts.
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

/// Type-erased callback for one contiguous position batch.
struct position_batch_sink {
    void* ctx = nullptr;
    void (*call)(void*, std::span<const std::uint64_t>) = nullptr;
};

/// Read-only cursor over positions collected for one minimizer hash.
class sorted_pair_position_cursor {
  public:
    sorted_pair_position_cursor() noexcept = default;
    explicit sorted_pair_position_cursor(const void* impl) noexcept : impl_(impl) {}

    /// Return the total number of encoded positions.
    [[nodiscard]] std::uint64_t total_count() const noexcept;

    /// Invoke a type-erased callback for each position batch.
    void for_each_batch_erased(position_batch_sink sink) const;

    /// Invoke a callable for each position batch.
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

/// Type-erased callback for one minimizer hash and its position cursor.
struct sorted_pair_kmer_sink {
    void* ctx = nullptr;
    void (*call)(void*, std::uint64_t, const sorted_pair_position_cursor&) = nullptr;
};

/// Resource and merge counters for one sorted-run store.
struct sorted_run_store_stats {
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
    // Dedicated compaction point: unlike `create_before`, this does not count
    // the input spill files created before a merge pass starts.
    compact_create_before,
};

/// Arm one failure at the next matching PostgreSQL leaf call.
void arm_sorted_run_pg_leaf_failure(sorted_run_failure_injection point) noexcept;

/// Arm one failure at the selected matching PostgreSQL leaf call.
void arm_sorted_run_pg_leaf_failure_on_nth(sorted_run_failure_injection point,
                                           std::uint64_t operation) noexcept;
/// Override the merge-reader memory budget for one backend test.
void set_sorted_run_merge_reader_budget_for_test(std::uint64_t bytes) noexcept;
/// Clear every sorted-run failure and budget override.
void disarm_sorted_run_pg_leaf_failure() noexcept;

/// Report whether a sorted-run leaf failure is armed.
[[nodiscard]] bool sorted_run_pg_leaf_failure_is_armed() noexcept;
#endif

} // namespace bioseq::storage::indexing
