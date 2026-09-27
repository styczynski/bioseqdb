/**
 * @file instrumentation/tracing.cppm
 * @brief Fixed-layout single-producer/single-consumer trace ring.
 *
 * Instantiated producers push fixed-width 64-byte events into a per-thread
 * ring. A separate consumer can advance the read sequence. This module owns
 * the fixed data layout and producer-side push operation.
 */
module;

#include <atomic>
#include <cstdint>
#include <new>

export module bioseq.instrumentation.tracing;

import bioseq.instrumentation.configuration;

export namespace bioseq::instrumentation {

/** @brief Operation encoded by a trace event. */
enum class trace_event_kind : std::uint32_t {
    region_begin = 0,
    region_end   = 1,
    counter      = 2,
    sample       = 3,
    loop_iteration = 4,
};

/** @brief Fixed one-cache-line event written to a trace ring. */
struct alignas(64) trace_event {
    trace_event_kind kind = trace_event_kind::region_begin;
    std::uint32_t region_id = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t cycles = 0;
    std::uint64_t payload_0 = 0;
    std::uint64_t payload_1 = 0;
    std::uint64_t payload_2 = 0;
    std::uint64_t payload_3 = 0;
};
static_assert(sizeof(trace_event) == 64);
static_assert(alignof(trace_event) == 64);

/** @brief Bounded single-producer trace ring for one profiler configuration. */
template<profiling_configuration p_configuration_type>
struct trace_ring {
    static constexpr std::size_t capacity = p_configuration_type::trace_capacity;

    trace_event* buffer = nullptr;
    std::atomic<std::uint64_t> write_sequence{0};
    std::atomic<std::uint64_t> read_sequence{0};
    std::uint64_t dropped_event_count = 0;
    bool should_overwrite_oldest = false;

    /** @brief Allocate storage when the configured capacity is nonzero. */
    trace_ring() noexcept {
        if constexpr (capacity > 0) {
            buffer = new (std::align_val_t{64}) trace_event[capacity]{};
        }
    }

    /** @brief Release trace storage when one was allocated. */
    ~trace_ring() noexcept {
        if (buffer != nullptr) {
            ::operator delete[](buffer, std::align_val_t{64});
        }
    }

    trace_ring(const trace_ring&) = delete;
    trace_ring& operator=(const trace_ring&) = delete;
    trace_ring(trace_ring&&) = delete;
    trace_ring& operator=(trace_ring&&) = delete;

    /** @brief Append, overwrite, or drop an event according to ring policy. */
    [[gnu::always_inline]]
    void push(const trace_event& event) noexcept {
        if constexpr (capacity == 0) {
            (void)event;
            return;
        } else {
            const auto write_position = write_sequence.load(std::memory_order_relaxed);
            const auto read_position = read_sequence.load(std::memory_order_acquire);
            if (write_position - read_position < capacity) {
                buffer[write_position % capacity] = event;
                write_sequence.store(write_position + 1, std::memory_order_release);
                return;
            }
            if (should_overwrite_oldest) {
                buffer[write_position % capacity] = event;
                write_sequence.store(write_position + 1, std::memory_order_release);
                read_sequence.store(
                    write_position + 1 - capacity, std::memory_order_release);
                return;
            }
            ++dropped_event_count;
        }
    }
};

}  // namespace bioseq::instrumentation
