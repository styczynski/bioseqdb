/**
 * @file sequence_generator_vulkan.cppm
 * @brief Thin Vulkan wrapper for the generic sequence generator.
 */
module;

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

export module bioseq.sequences.generation.vulkan;

import bioseq.common.mlogging;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;
import bioseq.sequences;

export namespace bioseq::sequences::generation::vulkan {

template <typename Traits>
inline constexpr bool gpu_generatable =
    Traits::alphabet_size >= 2 &&
    Traits::alphabet_size <= 256 &&
    Traits::bits_per_char >= 1 &&
    Traits::bits_per_char <= 32 &&
    (sizeof(typename Traits::storage_unit) == 4 || sizeof(typename Traits::storage_unit) == 8);

template <typename Traits>
[[nodiscard]] std::optional<bioseq::gpu::vulkan::GenerateUnitsPush> make_generate_units_push(
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_count,
    std::uint64_t seed,
    std::uint32_t chunk_units) {
    constexpr std::uint32_t lpu = Traits::letters_per_unit;
    const std::uint64_t units_per_seq = (symbols_per_seq + lpu - 1) / lpu;
    const std::uint64_t chunks_per_seq = (units_per_seq + chunk_units - 1) / chunk_units;
    const std::uint64_t total_chunks =
        bioseq::parallelism::saturating_mul(chunks_per_seq, seq_count);
    if (symbols_per_seq > 0xFFFFFFFFull ||
        units_per_seq > 0xFFFFFFFFull ||
        chunks_per_seq > 0xFFFFFFFFull ||
        total_chunks > 0xFFFFFFFFull) {
        return std::nullopt;
    }

    bioseq::gpu::vulkan::GenerateUnitsPush push{};
    push.symbols_per_seq = static_cast<std::uint32_t>(symbols_per_seq);
    push.units_per_seq = static_cast<std::uint32_t>(units_per_seq);
    push.chunks_per_seq = static_cast<std::uint32_t>(chunks_per_seq);
    push.chunk_units = chunk_units;
    push.letters_per_unit = lpu;
    push.bits_per_char = Traits::bits_per_char;
    push.alphabet_size = Traits::alphabet_size;
    push.total_chunks = static_cast<std::uint32_t>(total_chunks);
    push.seed_lo = static_cast<std::uint32_t>(seed);
    push.seed_hi = static_cast<std::uint32_t>(seed >> 32);
    return push;
}

template <typename Traits>
[[nodiscard]] bool can_generate_units_vulkan(
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_count,
    std::uint64_t seed,
    std::uint32_t chunk_units) {
    if constexpr (!gpu_generatable<Traits>) {
        return false;
    } else {
        auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
        return runtime.usable() &&
            make_generate_units_push<Traits>(symbols_per_seq, seq_count, seed, chunk_units)
                .has_value();
    }
}

template <typename Traits>
[[nodiscard]] std::uint64_t estimate_generate_units_peak_bytes(
    std::uint64_t out_units,
    std::span<const std::uint32_t> cdf) noexcept {
    using unit_t = typename Traits::storage_unit;
    const std::uint64_t out_bytes = bioseq::parallelism::saturating_mul(
        out_units, static_cast<std::uint64_t>(sizeof(unit_t)));
    return bioseq::parallelism::saturating_add(
        bioseq::parallelism::saturating_mul(out_bytes, 2u),
        bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(cdf.size()),
            static_cast<std::uint64_t>(sizeof(std::uint32_t))));
}

template <typename Traits>
[[nodiscard]] bool try_generate_units_vulkan(
    std::span<typename Traits::storage_unit> out_units,
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_count,
    std::span<const std::uint32_t> cdf,
    std::uint64_t seed,
    std::uint32_t chunk_units,
    bool memory_already_admitted = false) {
    using unit_t = typename Traits::storage_unit;
    if constexpr (!gpu_generatable<Traits>) {
        return false;
    } else {
        auto& runtime = bioseq::gpu::vulkan::Runtime::instance();
        if (!runtime.usable()) {
            return false;
        }

        constexpr std::uint32_t lpu = Traits::letters_per_unit;
        const std::uint64_t units_per_seq = (symbols_per_seq + lpu - 1) / lpu;
        const std::uint64_t total_units =
            bioseq::parallelism::saturating_mul(units_per_seq, seq_count);
        if (total_units != static_cast<std::uint64_t>(out_units.size())) {
            return false;
        }

        auto push = make_generate_units_push<Traits>(
            symbols_per_seq, seq_count, seed, chunk_units);
        if (!push) {
            return false;
        }

        const std::uint64_t out_bytes = bioseq::parallelism::saturating_mul(
            static_cast<std::uint64_t>(out_units.size()),
            static_cast<std::uint64_t>(sizeof(unit_t)));
        std::optional<bioseq::parallelism::MemoryPermit> permit;
        if (!memory_already_admitted) {
            permit = bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(
                estimate_generate_units_peak_bytes<Traits>(
                    static_cast<std::uint64_t>(out_units.size()), cdf));
            if (!permit) {
                return false;
            }
        }

        try {
            auto buffer = runtime.run_generate_units_roundtrip(
                static_cast<std::uint32_t>(sizeof(unit_t) * 8),
                cdf,
                *push,
                out_bytes);
            runtime.download_slice(buffer, 0, std::as_writable_bytes(out_units));
            return true;
        } catch (const bioseq::gpu::vulkan::VulkanRuntimeError& e) {
            bioseq_log<DBG>("[generation.vulkan] GPU generate failed; CPU fallback: {}", e.what());
            return false;
        }
    }
}

}  // namespace bioseq::sequences::generation::vulkan
