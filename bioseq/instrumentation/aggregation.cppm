/**
 * @file instrumentation/aggregate.cppm
 * @brief Per-thread storage for region and loop measurements.
 *
 * region_aggregate accumulates one row per (region_id, thread). The
 * Each table has one owning producer thread. Summary reads occur after the
 * measured work is quiescent, so aggregate fields remain non-atomic.
 */
module;

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <new>

export module bioseq.instrumentation.aggregation;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;

export namespace bioseq::instrumentation {

/**
 * @brief Fixed 32-bucket base-two histogram of nanosecond durations.
 *
 * Values above the represented range saturate in the final bucket. The
 * fixed layout is part of the in-process metrics boundary.
 */
struct duration_histogram {
    std::array<std::uint64_t, 32> buckets{};

    [[gnu::always_inline]]
    inline void add(std::uint64_t value) noexcept {
        if (value == 0) {
            ++buckets[0];
            return;
        }
        const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(value));
        ++buckets[msb < 32u ? msb : 31u];
    }
};
static_assert(sizeof(duration_histogram) == 256);

/** @brief Metrics accumulated for one region on one producer thread. */
struct alignas(64) region_aggregate {
    std::uint64_t total_ns;
    std::uint64_t total_cycles;
    std::uint64_t total_instructions;
    std::uint64_t total_branches;
    std::uint64_t total_branch_misses;
    std::uint64_t visits;
    std::uint64_t recursion_collisions;
    std::uint64_t min_ns;            // initialised to UINT64_MAX
    std::uint64_t max_ns;            // initialised to 0
    duration_histogram duration_distribution;
};
static_assert(sizeof(region_aggregate) == 384);
static_assert(alignof(region_aggregate) == 64);

[[gnu::always_inline]]
inline void initialize_aggregate(region_aggregate& aggregate) noexcept {
    aggregate.total_ns = 0;
    aggregate.total_cycles = 0;
    aggregate.total_instructions = 0;
    aggregate.total_branches = 0;
    aggregate.total_branch_misses = 0;
    aggregate.visits = 0;
    aggregate.recursion_collisions = 0;
    aggregate.min_ns = std::numeric_limits<std::uint64_t>::max();
    aggregate.max_ns = 0;
    aggregate.duration_distribution = duration_histogram{};
}

[[gnu::always_inline]]
inline void record_region_duration(
    region_aggregate& aggregate,
    std::uint64_t duration_ns) noexcept
{
    aggregate.total_ns += duration_ns;
    aggregate.visits += 1;
    if (duration_ns < aggregate.min_ns) aggregate.min_ns = duration_ns;
    if (duration_ns > aggregate.max_ns) aggregate.max_ns = duration_ns;
    aggregate.duration_distribution.add(duration_ns);
}

/// Loop aggregate extends region aggregate with iteration / work fields.
struct alignas(64) loop_aggregate : region_aggregate {
    std::uint64_t total_iterations;
    std::uint64_t total_work;
    std::uint64_t empty_visits;
    std::uint64_t min_per_iter_ns;
    std::uint64_t max_per_iter_ns;
    duration_histogram per_iteration_duration_distribution;
};
static_assert(sizeof(loop_aggregate) == 704);
static_assert(alignof(loop_aggregate) == 64);

[[gnu::always_inline]]
inline void initialize_loop_aggregate(loop_aggregate& aggregate) noexcept {
    initialize_aggregate(static_cast<region_aggregate&>(aggregate));
    aggregate.total_iterations = 0;
    aggregate.total_work = 0;
    aggregate.empty_visits = 0;
    aggregate.min_per_iter_ns = std::numeric_limits<std::uint64_t>::max();
    aggregate.max_per_iter_ns = 0;
    aggregate.per_iteration_duration_distribution = duration_histogram{};
}

[[gnu::always_inline]]
inline void record_loop_duration(
    loop_aggregate& aggregate,
    std::uint64_t duration_ns,
    std::uint64_t iterations,
    std::uint64_t work) noexcept
{
    record_region_duration(static_cast<region_aggregate&>(aggregate), duration_ns);
    aggregate.total_iterations += iterations;
    aggregate.total_work += work;
    if (iterations == 0) {
        aggregate.empty_visits += 1;
        return;
    }
    const auto per_iteration_ns = duration_ns / iterations;
    if (per_iteration_ns < aggregate.min_per_iter_ns) {
        aggregate.min_per_iter_ns = per_iteration_ns;
    }
    if (per_iteration_ns > aggregate.max_per_iter_ns) {
        aggregate.max_per_iter_ns = per_iteration_ns;
    }
    aggregate.per_iteration_duration_distribution.add(per_iteration_ns);
}

/// Per-thread aggregate table. Direct id-indexed; 4096 blocks of 256
/// aggregates. Each block is allocated lazily on first touch.
template<profiling_configuration configuration_type>
struct region_aggregate_table {
    static constexpr std::uint32_t block_size = configuration_type::aggregate_block_size;
    static constexpr std::uint32_t maximum_block_count = 4096;

    std::array<region_aggregate*, maximum_block_count> blocks{};

    ~region_aggregate_table() noexcept {
        for (auto* block : blocks) {
            if (block != nullptr) {
                ::operator delete(block, std::align_val_t{alignof(region_aggregate)});
            }
        }
    }

    [[nodiscard, gnu::always_inline]]
    inline region_aggregate* find(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / block_size;
        const auto slot_idx  = raw_id % block_size;
        if (block_idx >= maximum_block_count) return nullptr;
        auto* block = blocks[block_idx];
        if (block == nullptr) return nullptr;
        return &block[slot_idx];
    }

    [[nodiscard, gnu::cold]]
    region_aggregate* find_or_create(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / block_size;
        const auto slot_idx  = raw_id % block_size;
        if (block_idx >= maximum_block_count) return nullptr;
        if (blocks[block_idx] == nullptr) {
            auto* block = static_cast<region_aggregate*>(
                ::operator new(sizeof(region_aggregate) * block_size,
                               std::align_val_t{alignof(region_aggregate)}));
            for (std::uint32_t i = 0; i < block_size; ++i) {
                new (block + i) region_aggregate{};
                initialize_aggregate(block[i]);
            }
            blocks[block_idx] = block;
        }
        return &blocks[block_idx][slot_idx];
    }
};

template<profiling_configuration configuration_type>
struct loop_aggregate_table {
    static constexpr std::uint32_t block_size = configuration_type::aggregate_block_size;
    static constexpr std::uint32_t maximum_block_count = 4096;

    std::array<loop_aggregate*, maximum_block_count> blocks{};

    ~loop_aggregate_table() noexcept {
        for (auto* block : blocks) {
            if (block != nullptr) {
                ::operator delete(block, std::align_val_t{alignof(loop_aggregate)});
            }
        }
    }

    [[nodiscard, gnu::always_inline]]
    inline loop_aggregate* find(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / block_size;
        const auto slot_idx  = raw_id % block_size;
        if (block_idx >= maximum_block_count) return nullptr;
        auto* block = blocks[block_idx];
        if (block == nullptr) return nullptr;
        return &block[slot_idx];
    }

    [[nodiscard, gnu::cold]]
    loop_aggregate* find_or_create(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / block_size;
        const auto slot_idx  = raw_id % block_size;
        if (block_idx >= maximum_block_count) return nullptr;
        if (blocks[block_idx] == nullptr) {
            auto* block = static_cast<loop_aggregate*>(
                ::operator new(sizeof(loop_aggregate) * block_size,
                               std::align_val_t{alignof(loop_aggregate)}));
            for (std::uint32_t i = 0; i < block_size; ++i) {
                new (block + i) loop_aggregate{};
                initialize_loop_aggregate(block[i]);
            }
            blocks[block_idx] = block;
        }
        return &blocks[block_idx][slot_idx];
    }
};

}  // namespace bioseq::instrumentation
