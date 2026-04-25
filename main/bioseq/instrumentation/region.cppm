/**
 * @file instrumentation/region.cppm
 * @brief RegionScope<Args> RAII type and the user-facing region() helper.
 *
 * Hot-path entry point. The scope reads the typed profiler block from
 * `tls_profiler<Args>`. When Args::enable_profiling is false, every
 * body is empty and the type collapses.
 */
module;

#include <cstdint>

export module bioseq_instr.region;

import bioseq_instr.config;
import bioseq_instr.descriptor;
import bioseq_instr.aggregate;
import bioseq_instr.thread_state;
import bioseq_instr.clock;
import bioseq_instr.global_state;

export namespace bioseq::instr {

/// Stack-local state captured at region begin. ~16 bytes; never allocated.
struct Region {
    const RegionDescriptor* desc = nullptr;
    std::uint64_t           t_begin_ns = 0;
    bool                    skipped = true;
};

template<is_context_args Args>
struct [[nodiscard]] RegionScope {
    Region r{};

    [[gnu::always_inline]]
    explicit RegionScope(const RegionDescriptor* d) noexcept {
        if constexpr (Args::enable_profiling) {
            if (!runtime_enabled() || d == nullptr || d->id == 0) return;
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            r.desc = d;
            r.t_begin_ns = clock::now_ns();
            prof->stack_depth += 1;
            r.skipped = false;
        } else {
            (void)d;
        }
    }

    [[gnu::always_inline]] ~RegionScope() noexcept {
        if constexpr (Args::enable_profiling) {
            if (r.skipped) return;
            const auto end_ns = clock::now_ns();
            const auto delta_ns = end_ns - r.t_begin_ns;
            auto* prof = tls_profiler<Args>;
            if (prof == nullptr) return;
            if (prof->stack_depth > 0) prof->stack_depth -= 1;
            auto* agg = prof->aggregates.get_or_create(r.desc->id);
            if (agg != nullptr) {
                aggregate_record(*agg, delta_ns);
            }
        }
    }

    RegionScope(RegionScope&&) = delete;
    RegionScope(const RegionScope&) = delete;
    RegionScope& operator=(RegionScope&&) = delete;
    RegionScope& operator=(const RegionScope&) = delete;
};

template<is_context_args Args>
[[nodiscard, gnu::always_inline]]
inline RegionScope<Args> region(const RegionDescriptor* d) noexcept {
    return RegionScope<Args>(d);
}

}  // namespace bioseq::instr
