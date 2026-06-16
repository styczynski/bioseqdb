/**
 * @file instrumentation/aggregate.cppm
 * @brief Per-region aggregate counters and per-thread aggregate table.
 *
 * RegionAggregate accumulates one row per (region_id, thread). The
 * thread is the sole writer; the consumer (access::write_summary_to_justlog)
 * sums across threads under the threads-list mutex. Producer writes are
 * plain (eventual consistency for cross-thread reads — see Part 22.1 of
 * the design doc).
 */
module;

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <new>

export module bioseq.instr.aggregate;

import bioseq.instr.config;
import bioseq.instr.descriptor;

export namespace bioseq::instr {

/// 32-bucket log2 histogram of nanosecond durations. The previous
/// 16-bucket version saturated at 2^15 ns = 32.77 us, which collapsed
/// every multi-millisecond region into the last bucket and made
/// `p50` / `p99` indistinguishable from the bucket-end constant. With
/// 32 buckets the range now extends to ~2^32 ns ≈ 4.3 s; in practice
/// every region we instrument fits with several buckets to spare.
/// The structure is still cache-line friendly: 32 × 8 B = 256 B = 4
/// cache lines, and the type is a member of the per-region aggregate
/// rather than something we walk over millions of in a tight loop.
struct Histogram16 {
    std::array<std::uint64_t, 32> buckets{};

    [[gnu::always_inline]]
    void add(std::uint64_t value) noexcept {
        if (value == 0) {
            ++buckets[0];
            return;
        }
        const unsigned msb = 63u - static_cast<unsigned>(std::countl_zero(value));
        ++buckets[msb < 32u ? msb : 31u];
    }
};
static_assert(sizeof(Histogram16) == 256);

/// Region aggregate. One row per region per thread. 192 bytes.
struct alignas(64) RegionAggregate {
    std::uint64_t total_ns;
    std::uint64_t total_cycles;
    std::uint64_t total_instructions;
    std::uint64_t total_branches;
    std::uint64_t total_branch_misses;
    std::uint64_t visits;
    std::uint64_t recursion_collisions;
    std::uint64_t min_ns;            // initialised to UINT64_MAX
    std::uint64_t max_ns;            // initialised to 0
    Histogram16   ns_hist;
};

[[gnu::always_inline]]
inline void aggregate_init(RegionAggregate& a) noexcept {
    a.total_ns           = 0;
    a.total_cycles       = 0;
    a.total_instructions = 0;
    a.total_branches     = 0;
    a.total_branch_misses = 0;
    a.visits             = 0;
    a.recursion_collisions = 0;
    a.min_ns             = std::numeric_limits<std::uint64_t>::max();
    a.max_ns             = 0;
    a.ns_hist            = Histogram16{};
}

[[gnu::always_inline]]
inline void aggregate_record(RegionAggregate& a, std::uint64_t ns) noexcept {
    a.total_ns += ns;
    a.visits   += 1;
    if (ns < a.min_ns) a.min_ns = ns;
    if (ns > a.max_ns) a.max_ns = ns;
    a.ns_hist.add(ns);
}

/// Loop aggregate extends region aggregate with iteration / work fields.
struct alignas(64) LoopAggregate : RegionAggregate {
    std::uint64_t total_iterations;
    std::uint64_t total_work;
    std::uint64_t empty_visits;
    std::uint64_t min_per_iter_ns;
    std::uint64_t max_per_iter_ns;
    Histogram16   per_iter_ns_hist;
};

[[gnu::always_inline]]
inline void loop_aggregate_init(LoopAggregate& a) noexcept {
    aggregate_init(static_cast<RegionAggregate&>(a));
    a.total_iterations = 0;
    a.total_work       = 0;
    a.empty_visits     = 0;
    a.min_per_iter_ns  = std::numeric_limits<std::uint64_t>::max();
    a.max_per_iter_ns  = 0;
    a.per_iter_ns_hist = Histogram16{};
}

[[gnu::always_inline]]
inline void loop_aggregate_record(
    LoopAggregate& a,
    std::uint64_t ns,
    std::uint64_t iterations,
    std::uint64_t work) noexcept
{
    aggregate_record(static_cast<RegionAggregate&>(a), ns);
    a.total_iterations += iterations;
    a.total_work       += work;
    if (iterations == 0) {
        a.empty_visits += 1;
        return;
    }
    const auto per_iter = ns / iterations;
    if (per_iter < a.min_per_iter_ns) a.min_per_iter_ns = per_iter;
    if (per_iter > a.max_per_iter_ns) a.max_per_iter_ns = per_iter;
    a.per_iter_ns_hist.add(per_iter);
}

/// Per-thread aggregate table. Direct id-indexed; 4096 blocks of 256
/// aggregates. Each block is allocated lazily on first touch.
template<is_context_args Args>
struct RegionAggregateTable {
    static constexpr std::uint32_t BlockSize = Args::aggr_block_size;
    static constexpr std::uint32_t MaxBlocks = 4096;

    std::array<RegionAggregate*, MaxBlocks> blocks{};

    ~RegionAggregateTable() noexcept {
        for (auto* b : blocks) {
            if (b != nullptr) {
                ::operator delete(b, std::align_val_t{alignof(RegionAggregate)});
            }
        }
    }

    [[nodiscard, gnu::always_inline]]
    RegionAggregate* try_get(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / BlockSize;
        const auto slot_idx  = raw_id % BlockSize;
        if (block_idx >= MaxBlocks) return nullptr;
        auto* b = blocks[block_idx];
        if (b == nullptr) return nullptr;
        return &b[slot_idx];
    }

    [[nodiscard, gnu::cold]]
    RegionAggregate* get_or_create(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / BlockSize;
        const auto slot_idx  = raw_id % BlockSize;
        if (block_idx >= MaxBlocks) return nullptr;
        if (blocks[block_idx] == nullptr) {
            auto* b = static_cast<RegionAggregate*>(
                ::operator new(sizeof(RegionAggregate) * BlockSize,
                               std::align_val_t{alignof(RegionAggregate)}));
            for (std::uint32_t i = 0; i < BlockSize; ++i) {
                new (b + i) RegionAggregate{};
                aggregate_init(b[i]);
            }
            blocks[block_idx] = b;
        }
        return &blocks[block_idx][slot_idx];
    }
};

template<is_context_args Args>
struct LoopAggregateTable {
    static constexpr std::uint32_t BlockSize = Args::aggr_block_size;
    static constexpr std::uint32_t MaxBlocks = 4096;

    std::array<LoopAggregate*, MaxBlocks> blocks{};

    ~LoopAggregateTable() noexcept {
        for (auto* b : blocks) {
            if (b != nullptr) {
                ::operator delete(b, std::align_val_t{alignof(LoopAggregate)});
            }
        }
    }

    [[nodiscard, gnu::always_inline]]
    LoopAggregate* try_get(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / BlockSize;
        const auto slot_idx  = raw_id % BlockSize;
        if (block_idx >= MaxBlocks) return nullptr;
        auto* b = blocks[block_idx];
        if (b == nullptr) return nullptr;
        return &b[slot_idx];
    }

    [[nodiscard, gnu::cold]]
    LoopAggregate* get_or_create(std::uint32_t id) noexcept {
        if (id == 0) return nullptr;
        const auto raw_id = id - 1;
        const auto block_idx = raw_id / BlockSize;
        const auto slot_idx  = raw_id % BlockSize;
        if (block_idx >= MaxBlocks) return nullptr;
        if (blocks[block_idx] == nullptr) {
            auto* b = static_cast<LoopAggregate*>(
                ::operator new(sizeof(LoopAggregate) * BlockSize,
                               std::align_val_t{alignof(LoopAggregate)}));
            for (std::uint32_t i = 0; i < BlockSize; ++i) {
                new (b + i) LoopAggregate{};
                loop_aggregate_init(b[i]);
            }
            blocks[block_idx] = b;
        }
        return &blocks[block_idx][slot_idx];
    }
};

}  // namespace bioseq::instr
