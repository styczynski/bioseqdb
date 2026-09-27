/**
 * @file instrumentation/samples.cppm
 * @brief Sample records and a ring buffer with storage allocated in advance.
 *
 * Pushing a sample copies it into existing storage or counts it as dropped.
 */
module;

#include <atomic>
#include <cstdint>
#include <new>

export module bioseq.instrumentation.samples;

import bioseq.instrumentation.configuration;

export namespace bioseq::instrumentation {

/** @brief Timing, program location, and counters captured in one  sample. */
struct alignas(64) sample_record {
    std::uint64_t timestamp_ns = 0;
    std::uint64_t cycles = 0;
    std::uint32_t cpu_id = 0;
    std::uint32_t thread_id = 0;
    std::uintptr_t program_counter = 0;
    std::uint64_t hardware_cycles = 0;
    std::uint64_t hardware_instructions = 0;
    std::uint64_t hardware_branches = 0;
    std::uint64_t hardware_branch_misses = 0;
    std::uint16_t stack_depth = 0;
    std::uint16_t active_region_id = 0;
    std::uint32_t reserved = 0;
};
// Keep the sample layout at 128 bytes with 64-byte alignment.
static_assert(sizeof(sample_record) == 128);
static_assert(alignof(sample_record) == 64);

/** @brief Bounded single-producer sample ring for one profiler configuration. */
template<profiling_configuration p_configuration_type>
struct sample_ring {
    static constexpr std::size_t ring_capacity = 4096;

    sample_record* buffer = nullptr;
    std::atomic<std::uint64_t> write_sequence{0};
    std::atomic<std::uint64_t> read_sequence{0};
    std::uint64_t dropped_sample_count = 0;

    /** @brief Allocate ring storage only when sampling is compiled in. */
    sample_ring() noexcept {
        if constexpr (p_configuration_type::enable_sampling) {
            buffer = new (std::align_val_t{64}) sample_record[ring_capacity]{};
        }
    }
    /** @brief Release sampling storage when one was allocated. */
    ~sample_ring() noexcept {
        if (buffer != nullptr) {
            ::operator delete[](buffer, std::align_val_t{64});
        }
    }

    sample_ring(const sample_ring&) = delete;
    sample_ring& operator=(const sample_ring&) = delete;
    sample_ring(sample_ring&&) = delete;
    sample_ring& operator=(sample_ring&&) = delete;

    /** @brief Append a sample or count it as dropped when the ring is full. */
    [[gnu::always_inline]]
    void push(const sample_record& sample) noexcept {
        if constexpr (!p_configuration_type::enable_sampling) {
            (void)sample;
            return;
        } else {
            const auto write_position = write_sequence.load(std::memory_order_relaxed);
            const auto read_position = read_sequence.load(std::memory_order_acquire);
            if (write_position - read_position < ring_capacity) {
                buffer[write_position % ring_capacity] = sample;
                write_sequence.store(write_position + 1, std::memory_order_release);
                return;
            }
            ++dropped_sample_count;
        }
    }
};

}  // namespace bioseq::instrumentation
