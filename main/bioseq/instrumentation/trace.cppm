/**
 * @file instrumentation/trace.cppm
 * @brief Per-thread SPSC trace ring (Part 19.12 / Part 22.4 of the
 *        design doc).
 *
 * Off by default (Args::enable_tracing = false). When enabled, region
 * begin/end push fixed-width 64-byte events into a per-thread ring;
 * a separate consumer thread reads via a TraceCursor. This module
 * defines the data layout and producer-side push; the consumer side
 * lives in bioseq_instr.access.
 */
module;

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>

export module bioseq_instr.trace;

import bioseq_instr.config;

export namespace bioseq::instr {

enum class TraceEventKind : std::uint32_t {
    region_begin = 0,
    region_end   = 1,
    counter      = 2,
    sample       = 3,
    tick         = 4,
};

struct alignas(64) TraceEvent {
    std::uint32_t kind        = 0;
    std::uint32_t region_id   = 0;
    std::uint64_t t_ns        = 0;
    std::uint64_t cycles      = 0;
    std::uint64_t value0      = 0;
    std::uint64_t value1      = 0;
    std::uint64_t value2      = 0;
    std::uint64_t value3      = 0;
};
static_assert(sizeof(TraceEvent) == 64);

template<is_context_args Args>
struct TraceRing {
    static constexpr std::size_t Capacity = Args::trace_capacity;

    TraceEvent* buffer = nullptr;
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
    std::uint64_t              dropped = 0;
    bool                       overwrite_oldest = false;

    TraceRing() noexcept {
        if constexpr (Capacity > 0) {
            buffer = new (std::align_val_t{64}) TraceEvent[Capacity]{};
        }
    }

    ~TraceRing() noexcept {
        if (buffer != nullptr) {
            ::operator delete[](buffer, std::align_val_t{64});
        }
    }

    TraceRing(const TraceRing&) = delete;
    TraceRing& operator=(const TraceRing&) = delete;
    TraceRing(TraceRing&&) = delete;
    TraceRing& operator=(TraceRing&&) = delete;

    [[gnu::always_inline]]
    void push(const TraceEvent& evt) noexcept {
        if constexpr (Capacity == 0) {
            (void)evt;
            return;
        } else {
            const auto h = head.load(std::memory_order_relaxed);
            const auto t = tail.load(std::memory_order_acquire);
            if (h - t < Capacity) {
                buffer[h % Capacity] = evt;
                head.store(h + 1, std::memory_order_release);
                return;
            }
            if (overwrite_oldest) {
                buffer[h % Capacity] = evt;
                head.store(h + 1, std::memory_order_release);
                tail.store(h + 1 - Capacity, std::memory_order_release);
                return;
            }
            ++dropped;
        }
    }
};

}  // namespace bioseq::instr
