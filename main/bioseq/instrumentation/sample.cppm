/**
 * @file instrumentation/sample.cppm
 * @brief Per-thread sample ring (Part 19.13 of the design doc).
 *
 * Sampling captures PC + cycle counter + active region from a SIGPROF
 * handler and pushes a Sample into a per-thread ring. v1 default is
 * Args::enable_sampling = false; the type is provided so future args
 * structs can opt in without churning the public surface.
 */
module;

#include <atomic>
#include <cstdint>
#include <new>

export module bioseq_instr.sample;

import bioseq_instr.config;

export namespace bioseq::instr {

struct alignas(64) Sample {
    std::uint64_t t_ns                 = 0;
    std::uint64_t cycles               = 0;
    std::uint32_t cpu                  = 0;
    std::uint32_t thread_id            = 0;
    std::uintptr_t pc                  = 0;
    std::uint64_t pmu_cycles           = 0;
    std::uint64_t pmu_instructions     = 0;
    std::uint64_t pmu_branches         = 0;
    std::uint64_t pmu_branch_misses    = 0;
    std::uint16_t stack_depth          = 0;
    std::uint16_t region_id_at_sample  = 0;
    std::uint32_t _reserved            = 0;
};
// Two cache lines on 64-bit platforms; the second line stays cold during
// region-only profiling and is touched only when the sampler service is
// activated. Keeping it 128 B (instead of squeezing to 64) avoids losing
// fields that the design doc enumerates and prevents alignment surprises.
static_assert(sizeof(Sample) == 128);

template<is_context_args Args>
struct SampleRing {
    static constexpr std::size_t kCapacity = 4096;

    Sample* buffer = nullptr;
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
    std::uint64_t              dropped = 0;

    SampleRing() noexcept {
        if constexpr (Args::enable_sampling) {
            buffer = new (std::align_val_t{64}) Sample[kCapacity]{};
        }
    }
    ~SampleRing() noexcept {
        if (buffer != nullptr) {
            ::operator delete[](buffer, std::align_val_t{64});
        }
    }

    SampleRing(const SampleRing&) = delete;
    SampleRing& operator=(const SampleRing&) = delete;
    SampleRing(SampleRing&&) = delete;
    SampleRing& operator=(SampleRing&&) = delete;

    [[gnu::always_inline]]
    void push(const Sample& s) noexcept {
        if constexpr (!Args::enable_sampling) {
            (void)s;
            return;
        } else {
            const auto h = head.load(std::memory_order_relaxed);
            const auto t = tail.load(std::memory_order_acquire);
            if (h - t < kCapacity) {
                buffer[h % kCapacity] = s;
                head.store(h + 1, std::memory_order_release);
                return;
            }
            ++dropped;
        }
    }
};

}  // namespace bioseq::instr
