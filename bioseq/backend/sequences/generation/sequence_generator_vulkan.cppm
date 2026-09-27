/**
 * @file sequence_generator_vulkan.cppm
 * @brief Thin Vulkan wrapper for the generic sequence generator.
 *
 * This module validates packed-trait and dispatch geometry, estimates the
 * roundtrip's admitted memory, and forwards accepted work to the shared Vulkan
 * runtime. Rejected shapes return without selecting a CPU implementation.
 */
module;

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

export module bioseq.sequences.generation.vulkan;

import bioseq.common.checked_arithmetic;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.memory_admission;

export namespace bioseq::sequences::generation::vulkan {

/**
 * @brief Report whether packed traits fit the generation shader contract.
 * @tparam p_traits Compact-sequence traits describing alphabet and packed storage.
 * @return `true` for shader-representable alphabet, rank-width, and storage-unit sizes.
 */
template <typename p_traits>
inline constexpr bool gpu_generatable =
    p_traits::alphabet_size >= 2 && p_traits::alphabet_size <= 256 &&
    p_traits::bits_per_char >= 1 && p_traits::bits_per_char <= 32 &&
    (sizeof(typename p_traits::storage_unit) == 4 || sizeof(typename p_traits::storage_unit) == 8);

/**
 * @brief Build the validated push constants for one generation dispatch.
 * @tparam p_traits Compact-sequence traits supplying packing geometry.
 * @param symbols_per_seq Logical symbols generated for each sequence.
 * @param seq_count Number of independent sequences in the dispatch.
 * @param seed Deterministic 64-bit generator seed.
 * @param chunk_units Packed storage units produced by one logical chunk.
 * @return Complete push constants, or `std::nullopt` when a derived dispatch
 * dimension cannot be represented by the shader's 32-bit ABI.
 * @pre `chunk_units` is nonzero.
 */
template <typename p_traits>
[[nodiscard]] std::optional<bioseq::gpu::vulkan::generate_units_push> make_generate_units_push(
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_count,
    std::uint64_t seed,
    std::uint32_t chunk_units) {
    constexpr std::uint32_t letters_per_unit = p_traits::letters_per_unit;
    const std::uint64_t units_per_seq = (symbols_per_seq + letters_per_unit - 1) / letters_per_unit;
    const std::uint64_t chunks_per_seq = (units_per_seq + chunk_units - 1) / chunk_units;
    const std::uint64_t total_chunks =
        bioseq::common::saturating_mul(chunks_per_seq, seq_count);
    if (symbols_per_seq > 0xFFFFFFFFull ||
        units_per_seq > 0xFFFFFFFFull ||
        chunks_per_seq > 0xFFFFFFFFull ||
        total_chunks > 0xFFFFFFFFull) {
        return std::nullopt;
    }

    bioseq::gpu::vulkan::generate_units_push push{};
    push.symbols_per_seq = static_cast<std::uint32_t>(symbols_per_seq);
    push.units_per_seq = static_cast<std::uint32_t>(units_per_seq);
    push.chunks_per_seq = static_cast<std::uint32_t>(chunks_per_seq);
    push.chunk_units = chunk_units;
    push.letters_per_unit = letters_per_unit;
    push.bits_per_char = p_traits::bits_per_char;
    push.alphabet_size = p_traits::alphabet_size;
    push.total_chunks = static_cast<std::uint32_t>(total_chunks);
    push.seed_lo = static_cast<std::uint32_t>(seed);
    push.seed_hi = static_cast<std::uint32_t>(seed >> 32);
    return push;
}

/**
 * @brief Report whether a hardware Vulkan device can run a requested shape.
 * @tparam p_traits Compact-sequence traits supplying shader packing geometry.
 * @param symbols_per_seq Logical symbols generated for each sequence.
 * @param seq_count Number of independent sequences in the dispatch.
 * @param seed Deterministic 64-bit generator seed.
 * @param chunk_units Packed storage units produced by one logical chunk.
 * @return `true` when the traits and derived push constants are representable
 * and the shared runtime owns a hardware Vulkan device.
 * @pre `chunk_units` is nonzero.
 */
template <typename p_traits>
[[nodiscard]] bool can_generate_units_vulkan(
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_count,
    std::uint64_t seed,
    std::uint32_t chunk_units) {
    if constexpr (!gpu_generatable<p_traits>) {
        return false;
    } else {
        auto& runtime = bioseq::gpu::vulkan::runtime::instance();
        return bioseq::gpu::vulkan::hardware_device_available(runtime) &&
            make_generate_units_push<p_traits>(symbols_per_seq, seq_count, seed, chunk_units)
                .has_value();
    }
}

/**
 * @brief Estimate admitted host and device bytes for one generation roundtrip.
 * @tparam p_traits Compact-sequence traits defining the output storage unit.
 * @param out_units Number of packed output units.
 * @param cdf Alphabet cumulative-distribution table uploaded with the dispatch.
 * @return Twice the packed-output byte count plus the CDF byte count, saturated
 * at `std::uint64_t` maximum.
 */
template <typename p_traits>
[[nodiscard]] std::uint64_t estimate_generate_units_peak_bytes(
    std::uint64_t out_units,
    std::span<const std::uint32_t> cdf) noexcept {
    using storage_unit_type = typename p_traits::storage_unit;
    const std::uint64_t out_bytes = bioseq::common::saturating_mul(
        out_units, static_cast<std::uint64_t>(sizeof(storage_unit_type)));
    return bioseq::common::saturating_add(
        bioseq::common::saturating_mul(out_bytes, 2u),
        bioseq::common::saturating_mul(
            static_cast<std::uint64_t>(cdf.size()),
            static_cast<std::uint64_t>(sizeof(std::uint32_t))));
}

/**
 * @brief Generate packed units when the complete Vulkan contract is valid.
 * @tparam p_traits Compact-sequence traits defining alphabet and packed storage.
 * @param out_units Exact destination span for every generated packed unit.
 * @param symbols_per_seq Logical symbols generated for each sequence.
 * @param seq_count Number of independent sequences in the dispatch.
 * @param cdf Alphabet cumulative-distribution table consumed by the shader.
 * @param seed Deterministic 64-bit generator seed.
 * @param chunk_units Packed storage units produced by one logical chunk.
 * @param memory_already_admitted Whether the caller already owns memory
 * admission covering the complete roundtrip estimate.
 * @return `false` before dispatch when traits, hardware, output extent, push
 * constants, or memory admission reject the request; otherwise `true` after
 * the downloaded packed bytes fill @p out_units.
 * @pre `chunk_units` is nonzero.
 *
 * Runtime dispatch and transfer failures retain the shared Vulkan runtime's
 * error semantics. This boundary never substitutes CPU generation.
 */
template <typename p_traits>
[[nodiscard]] bool try_generate_units_vulkan(
    std::span<typename p_traits::storage_unit> out_units,
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_count,
    std::span<const std::uint32_t> cdf,
    std::uint64_t seed,
    std::uint32_t chunk_units,
    bool memory_already_admitted = false) {
    using storage_unit_type = typename p_traits::storage_unit;
    if constexpr (!gpu_generatable<p_traits>) {
        return false;
    } else {
        auto& runtime = bioseq::gpu::vulkan::runtime::instance();
        if (!bioseq::gpu::vulkan::hardware_device_available(runtime)) {
            return false;
        }

        constexpr std::uint32_t letters_per_unit = p_traits::letters_per_unit;
        const std::uint64_t units_per_seq =
            (symbols_per_seq + letters_per_unit - 1) / letters_per_unit;
        const std::uint64_t total_units =
            bioseq::common::saturating_mul(units_per_seq, seq_count);
        if (total_units != static_cast<std::uint64_t>(out_units.size())) {
            return false;
        }

        auto push = make_generate_units_push<p_traits>(
            symbols_per_seq, seq_count, seed, chunk_units);
        if (!push) {
            return false;
        }

        const std::uint64_t out_bytes =
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(out_units.size()),
                                           static_cast<std::uint64_t>(sizeof(storage_unit_type)));
        std::optional<bioseq::parallelism::memory_permit> permit;
        if (!memory_already_admitted) {
            permit = bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                estimate_generate_units_peak_bytes<p_traits>(
                    static_cast<std::uint64_t>(out_units.size()), cdf));
            if (!permit) {
                return false;
            }
        }

        auto buffer = runtime.run_generate_units_roundtrip(
            static_cast<std::uint32_t>(sizeof(storage_unit_type) * 8), cdf, *push, out_bytes);
        runtime.download_slice(buffer, 0, std::as_writable_bytes(out_units));
        return true;
    }
}

}  // namespace bioseq::sequences::generation::vulkan
