/**
 * @file sequence_generator.cppm
 * @brief Generate random references and query fragments with substitutions and gaps.
 *
 * CPU and Vulkan reference generation use the same random streams and packing,
 * so the seed and sequence settings produce the same bytes on either backend.
 * Query fragments are extracted and edited on the CPU.
 */
module;

#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <tuple>
#include <utility>
#include <vector>

export module bioseq.sequences.generation;

import bioseq.common.checked_arithmetic;
import bioseq.common.hashing;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.cigar;
import bioseq.sequences.rank_pack;
import bioseq.sequences.simd;
import bioseq.simd.utils;
import bioseq.storage.lazy_sequence;
import bioseq.sequences.generation.vulkan;
import bioseq.gpu.vulkan.runtime;

export namespace bioseq::sequences::generation {

/// Reference sizes, fragment sampling, and mutation settings.
struct generator_params {
    /// Number of symbols generated for each reference.
    std::size_t reference_length       = 10'000;
    /// Number of query fragments requested by sample-table callers.
    std::size_t fragment_count         = 100;
    /// Mean of the normal distribution used to sample fragment lengths before edits.
    std::size_t fragment_length_mean   = 150;
    /// Standard deviation used when sampling  fragment lengths before edits.
    double      fragment_length_stddev = 10.0;

    /// Independent substitution probability per source symbol.
    double substitution_rate = 0.01;
    /// Chance of inserting symbols after each source position reached by the indel pass.
    double insertion_rate    = 0.0005;
    /// Chance of deleting following symbols at each position reached by the indel pass.
    double deletion_rate     = 0.0005;

    /// Mean of the insertion-length distribution before rounding and clamping to zero.
    double insertion_length_mean   = 2.0;
    /// Standard deviation of insertion-event length.
    double insertion_length_stddev = 1.0;
    /// Mean of the dletion-length distribution before rounding and clamping to zero.
    double deletion_length_mean    = 2.0;
    /// Standard deviation of deletion-event length.
    double deletion_length_stddev  = 1.0;

    /// Chance of reversing a query window, also complementing nucleotide symbols.
    double        reverse_complement_rate = 0.5;
    /// Seed shared by deterministic CPU and Vulkan streams.
    std::uint64_t seed                    = 42;

    /// Generate references with Vulkan. Unsupported requests or failed dispatches
    /// throw an error Query  extraction and mutation still run on the CPU.
    bool use_vulkan = false;
};

/** Checked failures shared by native generation and its host adapters. */
enum class generation_error {
    invalid_parameter, size_overflow, allocation_rejected, invalid_source,
    unsupported_layout, output_contract
};

/** Inclusive bounds and an optional Gaussian or fixed mean for generated lengths. */
struct generation_length_parameters {
    std::size_t minimum{1000};
    std::size_t maximum{1000};
    std::optional<double> mean{};
    double stddev{};
};

/** Validate length geometry and floating inputs before any sampling or allocation. */
[[nodiscard]] inline std::expected<void, generation_error> validate_generation_lengths(
    const generation_length_parameters& parameters, std::size_t minimum_allowed = 0u,
    std::size_t maximum_allowed = (std::numeric_limits<std::uint32_t>::max)()) {
    if (parameters.minimum < minimum_allowed || parameters.minimum > parameters.maximum ||
        !std::isfinite(parameters.stddev) || parameters.stddev < 0.0 ||
        (parameters.mean && !std::isfinite(*parameters.mean)))
        return std::unexpected(generation_error::invalid_parameter);
    if (parameters.maximum > maximum_allowed)
        return std::unexpected(generation_error::size_overflow);
    return {};
}

/** Validate all shared mutation probabilities and event-length distributions. */
[[nodiscard]] inline std::expected<void, generation_error> validate_mutation_parameters(
    const generator_params& parameters) {
    const std::array rates{parameters.substitution_rate, parameters.insertion_rate,
        parameters.deletion_rate, parameters.reverse_complement_rate};
    for (const auto rate : rates)
        if (!std::isfinite(rate) || rate < 0.0 || rate > 1.0)
            return std::unexpected(generation_error::invalid_parameter);
    const std::array nonnegative{parameters.fragment_length_stddev,
        parameters.insertion_length_mean, parameters.insertion_length_stddev,
        parameters.deletion_length_mean, parameters.deletion_length_stddev};
    for (const auto value : nonnegative)
        if (!std::isfinite(value) || value < 0.0)
            return std::unexpected(generation_error::invalid_parameter);
    return {};
}

/// @brief Provides a uniform probability model for an alphabet.
/// @tparam p_alphabet Alphabet whose ranks receive probabilities.
template <sequence_alphabet p_alphabet>
struct base_alphabet_probability {
    /// Alphabet described by this probability model.
    using alphabet_type = p_alphabet;
    /// Number of ranks in the alphabet.
    static constexpr std::size_t alphabet_size = seqan3::alphabet_size<alphabet_type>;
    /// @brief Returns the rank probabilities in alphabet order.
    static constexpr std::array<double, alphabet_size> probability() noexcept {
        std::array<double, alphabet_size> probabilities{};
        for (std::size_t alphabet_index = 0; alphabet_index < alphabet_size; ++alphabet_index) {
            probabilities[alphabet_index] = 1.0 / static_cast<double>(alphabet_size);
        }
        return probabilities;
    }
};

/// @brief Selects the default probability model for an alphabet.
/// @tparam p_alphabet Alphabet whose default model is selected.
template <sequence_alphabet p_alphabet>
struct alphabet_probability : base_alphabet_probability<p_alphabet> {};

/// @brief Gives dna5 ambiguity rank N a one-percent probability.
template <>
struct alphabet_probability<seq::dna5> : base_alphabet_probability<seq::dna5> {
    /// @brief Give N probability 0.01 and divide the rest equally among, A, C, G, and T.
    static constexpr std::array<double, alphabet_size> probability() noexcept {
        constexpr double unknown_base_rate = 0.01;
        const std::size_t unknown_base_rank =
            static_cast<std::size_t>(seq::dna5{}.assign_char('N').to_rank());
        std::array<double, alphabet_size> probabilities{};
        for (std::size_t alphabet_index = 0; alphabet_index < alphabet_size; ++alphabet_index) {
            probabilities[alphabet_index] = (1.0 - unknown_base_rate) / static_cast<double>(alphabet_size - 1u);
        }
        probabilities[unknown_base_rank] = unknown_base_rate;
        return probabilities;
    }
};

/// @brief Check that rank probabilities sum to one within the allowed tolerance.
/// @tparam p_probability_count Number of alphabet ranks.
/// @param probabilities Probability assigned to each rank.
/// @return True when the sum differs from one by less than 1e-9.
template <std::size_t p_probability_count>
consteval bool probabilities_sum_to_one(
    const std::array<double, p_probability_count>& probabilities) noexcept {
    double sum = 0.0;
    for (double value : probabilities) {
        sum += value;
    }
    return sum > 1.0 - 1e-9 && sum < 1.0 + 1e-9;
}

/// @brief Checks that a compile-time probability table has no negative entry.
/// @tparam p_probability_count Number of rank probabilities.
/// @param probabilities Probability assigned to each rank.
/// @return `true` when every entry is nonnegative.
template <std::size_t p_probability_count>
consteval bool probabilities_nonnegative(
    const std::array<double, p_probability_count>& probabilities) noexcept {
    for (double value : probabilities) {
        if (value < 0.0) {
            return false;
        }
    }
    return true;
}

/// @brief Require one nonnegative probability per alphabet rank, summing to one.
/// @tparam p_probability_model Probability model checked at compile time.
template <typename p_probability_model>
concept is_valid_alphabet_probability =
    sequence_alphabet<typename p_probability_model::alphabet_type> &&
    requires {
        { p_probability_model::alphabet_size } -> std::convertible_to<std::size_t>;
        {
            p_probability_model::probability()
        } -> std::same_as<std::array<double, p_probability_model::alphabet_size>>;
    } &&
    (p_probability_model::alphabet_size ==
     seqan3::alphabet_size<typename p_probability_model::alphabet_type>) &&
    probabilities_nonnegative(p_probability_model::probability()) &&
    probabilities_sum_to_one(p_probability_model::probability());

static_assert([]<class... Alphabets>(std::tuple<Alphabets...>) consteval {
    return (is_valid_alphabet_probability<alphabet_probability<Alphabets>> && ...);
}(supported_biological_alphabets::alphabets_tuple{}));

/// @brief Accepts sequence configurations that support packed construction.
/// @tparam p_sequence_config Candidate compact-sequence configuration.
template <typename p_sequence_config>
concept packed_sequence_config =
    is_sequence_custom_alphabet<p_sequence_config> &&
    requires {
        typename compact_sequence_range<p_sequence_config>::base_container;
        typename compact_sequence_range<p_sequence_config>::traits;
        { compact_sequence_range<p_sequence_config>::from_packed(
              typename compact_sequence_range<p_sequence_config>::base_container{},
              std::size_t{0}) } ->
            std::same_as<compact_sequence_range<p_sequence_config>>;
    };

/// Packed units generated from one independently seeded random stream.
inline constexpr std::uint32_t k_chunk_units = 8;
/// First stream ID used when sampling  and editing query fragments.
inline constexpr std::uint64_t k_reference_stream_space = 1ull << 40;

/// @brief State of the HybridTaus generator: three Tausworthe words and one LCG word.
struct taus_lcg_state {
    /// First Tausworthe state word.
    std::uint32_t first_taus_state = 0;
    /// Second Tausworthe state word.
    std::uint32_t second_taus_state = 0;
    /// Third Tausworthe state word.
    std::uint32_t third_taus_state = 0;
    /// Linear-congruential state word.
    std::uint32_t lcg_state = 0;
};

/// @brief Advances one masked Tausworthe state word.
[[gnu::always_inline]] inline std::uint32_t taus_step(
    std::uint32_t value,
    int xor_left_shift,
    int xor_right_shift,
    int state_left_shift,
    std::uint32_t mask) noexcept {
    const std::uint32_t mixed_bits = (((value << xor_left_shift) ^ value) >> xor_right_shift);
    return (((value & mask) << state_left_shift) ^ mixed_bits);
}

/// @brief Advances one linear-congruential state word.
[[gnu::always_inline]] inline std::uint32_t lcg_step(
    std::uint32_t value, std::uint32_t multiplier, std::uint32_t increment) noexcept {
    return multiplier * value + increment;
}

/// @brief Produces the next 32 random bits and advances all state words.
[[gnu::always_inline]] inline std::uint32_t next_u32(taus_lcg_state& state) noexcept {
    state.first_taus_state = taus_step(state.first_taus_state, 13, 19, 12, 4294967294u);
    state.second_taus_state = taus_step(state.second_taus_state, 2, 25, 4, 4294967288u);
    state.third_taus_state = taus_step(state.third_taus_state, 3, 11, 17, 4294967280u);
    state.lcg_state = lcg_step(state.lcg_state, 1664525u, 1013904223u);
    return state.first_taus_state ^ state.second_taus_state ^
           state.third_taus_state ^ state.lcg_state;
}

/// @brief Derive the same valid generator state for a gven seed and stream ID.
[[gnu::always_inline]] inline taus_lcg_state seed_stream(
    std::uint64_t global_seed, std::uint64_t stream_id) noexcept {
    const std::uint64_t stream_seed = global_seed ^ (stream_id * 0x9e3779b97f4a7c15ULL);
    const std::uint64_t first_mix = bioseq::common::splitmix64(stream_seed);
    const std::uint64_t second_mix = bioseq::common::splitmix64(first_mix);
    taus_lcg_state state{
        static_cast<std::uint32_t>(first_mix), static_cast<std::uint32_t>(first_mix >> 32),
        static_cast<std::uint32_t>(second_mix), static_cast<std::uint32_t>(second_mix >> 32)};
    if (state.first_taus_state < 129u) state.first_taus_state += 129u;
    if (state.second_taus_state < 129u) state.second_taus_state += 129u;
    if (state.third_taus_state < 129u) state.third_taus_state += 129u;
    return state;
}

/// @brief Convert probabilities to integer boundaries for sample_rank().
/// @tparam p_probability_count Number of alphabet ranks.
/// @param probabilities Probability assigned to each rank.
/// @return Cumulative boundaries in rank order. A draw at a boundary moves to
/// the next rank. The final rank accepts all remaining draws.
template <std::size_t p_probability_count>
constexpr std::array<std::uint32_t, p_probability_count> make_integer_cdf(
    const std::array<double, p_probability_count>& probabilities) noexcept {
    std::array<std::uint32_t, p_probability_count> cumulative_distribution{};
    double accumulated_probability = 0.0;
    for (std::size_t probability_index = 0;
         probability_index < p_probability_count;
         ++probability_index) {
        accumulated_probability += probabilities[probability_index];
        const double scaled = accumulated_probability * 4294967296.0;
        cumulative_distribution[probability_index] = scaled >= 4294967295.0
            ? 0xFFFFFFFFu
            : static_cast<std::uint32_t>(scaled);
    }
    return cumulative_distribution;
}

/// @brief Choose the rank whose  cumulative probability interval contains the draw.
[[gnu::always_inline]] inline std::uint32_t sample_rank(
    const std::uint32_t* cumulative_distribution,
    std::uint32_t alphabet_size,
    std::uint32_t draw) noexcept {
    std::uint32_t rank = 0;
    while (rank + 1u < alphabet_size && draw >= cumulative_distribution[rank]) {
        ++rank;
    }
    return rank;
}

/// @brief Returns a random value in the half-open interval [0, 1).
[[gnu::always_inline]] inline double next_unit_double(taus_lcg_state& state) noexcept {
    return static_cast<double>(next_u32(state)) * 2.3283064365386963e-10;
}

/// @brief Scale a random draw to an integer below a positive bound
/// Multiplication can introduce a small bias. Use next_below64() when unbiased
/// selection is required. A zero bound returns zero.
[[gnu::always_inline]] inline std::uint32_t next_below(
    taus_lcg_state& state, std::uint32_t exclusive_upper_bound) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(next_u32(state)) * exclusive_upper_bound) >> 32);
}

/// @brief Combines two generator draws into a 64-bit value.
[[gnu::always_inline]] inline std::uint64_t next_u64(taus_lcg_state& state) noexcept {
    const std::uint64_t high_bits = next_u32(state);
    const std::uint64_t low_bits = next_u32(state);
    return (high_bits << 32) | low_bits;
}

/// @brief Draw an integer below a positive bound without modulo bias.
/// Reject the uneven part of the random range before taking the remainder.
/// A zero bound returns zero.
[[gnu::always_inline]] inline std::uint64_t next_below64(
    taus_lcg_state& state, std::uint64_t exclusive_upper_bound) noexcept {
    if (exclusive_upper_bound == 0) {
        return 0;
    }
    const std::uint64_t threshold =
        (std::uint64_t{0} - exclusive_upper_bound) % exclusive_upper_bound;
    for (;;) {
        const std::uint64_t random_value = next_u64(state);
        if (random_value >= threshold) {
            return random_value % exclusive_upper_bound;
        }
    }
}

/// @brief Choose  a different rank from an alphabet with at least two ranks.
/// The original rank, must belong to that alphabet.
[[gnu::always_inline]] inline std::uint32_t next_other_rank(
    taus_lcg_state& state, std::uint32_t alphabet_size, std::uint32_t original) noexcept {
    const std::uint32_t draw = next_below(state, alphabet_size - 1u);
    return draw >= original ? draw + 1u : draw;
}

/// @brief Sample a normal distribution with the requested mean and standard deviation.
inline double next_gaussian(
    taus_lcg_state& state, double mean, double stddev) noexcept {
    if (stddev == 0.0) return mean;
    double radial_uniform = next_unit_double(state);
    const double angular_uniform = next_unit_double(state);
    if (radial_uniform < 1e-300) {
        radial_uniform = 1e-300;
    }
    const double radius = std::sqrt(-2.0 * std::log(radial_uniform));
    const double theta = 6.283185307179586 * angular_uniform;
    return mean + stddev * (radius * std::cos(theta));
}

/** Sample a bounded length, clamping floating values before integer conversion. */
[[nodiscard]] inline std::expected<std::size_t, generation_error> sample_generation_length(
    const generation_length_parameters& parameters, taus_lcg_state& random) {
    const auto valid = validate_generation_lengths(parameters);
    if (!valid) return std::unexpected(valid.error());
    if (parameters.minimum == parameters.maximum) return parameters.minimum;
    if (!parameters.mean && parameters.stddev == 0.0)
        return parameters.minimum + static_cast<std::size_t>(next_below64(random,
            static_cast<std::uint64_t>(parameters.maximum - parameters.minimum) + 1u));
    const auto mean = parameters.mean.value_or(static_cast<double>(parameters.minimum)
        + static_cast<double>(parameters.maximum - parameters.minimum) * 0.5);
    const auto sampled = next_gaussian(random, mean, parameters.stddev);
    if (std::isnan(sampled)) return std::unexpected(generation_error::invalid_parameter);
    const auto bounded = std::clamp(sampled, static_cast<double>(parameters.minimum),
        static_cast<double>(parameters.maximum));
    return static_cast<std::size_t>(std::round(bounded));
}

/// @brief Pack ranks starting at the low bits of one storage unit.
/// @tparam p_sequence_traits Alphabet and storage layout.
/// @param ranks Valid alphnbet ranks to pack.
/// @param valid Number of ranks, which must fit in one storage unit.
/// @return Packed ranks with unused high bits set to zero.
template <typename p_sequence_traits>
[[gnu::always_inline]] constexpr typename p_sequence_traits::storage_unit pack_unit(
    const typename p_sequence_traits::letter_unit* ranks, std::size_t valid) noexcept {
    using storage_unit_type = typename p_sequence_traits::storage_unit;
    storage_unit_type packed_unit = 0;
    for (std::size_t slot = 0; slot < valid; ++slot) {
        packed_unit = static_cast<storage_unit_type>(
            packed_unit | (static_cast<storage_unit_type>(ranks[slot])
                           << (slot * p_sequence_traits::bits_per_char)));
    }
    return packed_unit;
}

/// @brief Decode a reference window to ranks in the requested orientation.
/// Nucleotide windows are also complemented when reversed.
/// @tparam p_sequence_type Packed sequence type.
/// @param sequence Source sequence.
/// @param start First source position in the window
/// @param end Position just past the window.
/// @param is_reverse Read the window backwards when true.
/// @param out_ranks Destination  with room for end - start ranks.
template <typename p_sequence_type>
void extract_window_ranks(
    const p_sequence_type& sequence,
    std::size_t start,
    std::size_t end,
    bool is_reverse,
    std::span<typename p_sequence_type::traits::letter_unit> out_ranks) {
    using sequence_traits = typename p_sequence_type::traits;
    DEBUG_ASSERT(out_ranks.size() >= end - start, "extract_window_ranks: output too small");
    if (end <= start) {
        return;
    }
    if constexpr (sizeof(typename sequence_traits::letter_unit) == sizeof(std::uint8_t)) {
        auto byte_output = std::span<std::uint8_t>{
            reinterpret_cast<std::uint8_t*>(out_ranks.data()), end - start};
        if (is_reverse) {
            sequence.template to_encoded_into<false, true>(byte_output, start, end);
            if constexpr (seqan3::nucleotide_alphabet<typename sequence_traits::alphabet_type>) {
                for (auto& rank : out_ranks) {
                    rank = static_cast<typename sequence_traits::letter_unit>(
                        sequence_traits::letter_complement(
                            static_cast<typename sequence_traits::storage_unit>(rank)));
                }
            }
        } else {
            sequence.template to_encoded_into<false, false>(byte_output, start, end);
        }
    } else {
        const std::size_t length = end - start;
        auto iter = sequence.template iterable<logical_rank>().begin();
        for (std::size_t offset = 0; offset < start; ++offset) {
            ++iter;
        }
        if (is_reverse) {
            for (std::size_t offset = 0; offset < length; ++offset) {
                auto letter = *iter;
                if constexpr (seqan3::nucleotide_alphabet<typename sequence_traits::alphabet_type>) {
                    out_ranks[length - 1 - offset] =
                        static_cast<typename sequence_traits::letter_unit>(
                            sequence_traits::letter_complement(
                                static_cast<typename sequence_traits::storage_unit>(letter)));
                } else {
                    out_ranks[length - 1 - offset] =
                        static_cast<typename sequence_traits::letter_unit>(letter);
                }
                ++iter;
            }
        } else {
            for (std::size_t offset = 0; offset < length; ++offset) {
                out_ranks[offset] = static_cast<typename sequence_traits::letter_unit>(*iter);
                ++iter;
            }
        }
    }
}

/** One native output interval borrowed for the duration of a generation batch. */
template<class Traits>
struct random_packed_destination {
    using storage_unit = typename Traits::storage_unit;
    std::span<storage_unit> units;
    std::uint64_t symbol_count{};
    std::uint64_t sequence_index{};
    std::uint64_t first_output_unit{};
    /// Explicit row seed isolates variable-length SQL streams; absent preserves CPU/Vulkan mapping.
    std::optional<std::uint64_t> row_seed{};
};

/** A batch supplies stable, nonoverlapping native output spans, without host-specific state. */
template<class Batch, class Traits>
concept random_packed_batch = requires(Batch& batch, std::size_t index) {
    { batch.size() } -> std::convertible_to<std::size_t>;
    { batch[index] } -> std::convertible_to<random_packed_destination<Traits>>;
};

/** Fill one existing HybridTaus chunk; its streams remain byte-identical to Vulkan. */
template<class Traits>
void generate_sequence_chunk(random_packed_destination<Traits> destination,
    std::span<const std::uint32_t> cdf, std::uint64_t seed, std::uint64_t chunk_index) {
    constexpr auto letters_per_unit = Traits::letters_per_unit;
    const auto units_per_sequence = destination.symbol_count / letters_per_unit
        + static_cast<std::uint64_t>(destination.symbol_count % letters_per_unit != 0u);
    const auto chunks_per_sequence = units_per_sequence / k_chunk_units
        + static_cast<std::uint64_t>(units_per_sequence % k_chunk_units != 0u);
    const auto stream_base = destination.row_seed ? 0u : destination.sequence_index * chunks_per_sequence;
    auto random = seed_stream(destination.row_seed.value_or(seed), stream_base + chunk_index);
    const auto first_unit = chunk_index * k_chunk_units;
    const auto output_end = destination.first_output_unit + destination.units.size();
    std::array<typename Traits::letter_unit, letters_per_unit> ranks{};
    for (std::size_t offset = 0; offset < k_chunk_units && first_unit + offset < units_per_sequence; ++offset) {
        const auto unit_index = first_unit + offset;
        const auto first_symbol = unit_index * letters_per_unit;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            letters_per_unit, destination.symbol_count - first_symbol));
        for (std::size_t symbol = 0; symbol < count; ++symbol)
            ranks[symbol] = static_cast<typename Traits::letter_unit>(sample_rank(
                cdf.data(), static_cast<std::uint32_t>(Traits::alphabet_size), next_u32(random)));
        if (unit_index >= destination.first_output_unit && unit_index < output_end)
            destination.units[static_cast<std::size_t>(unit_index - destination.first_output_unit)] =
                pack_unit<Traits>(ranks.data(), count);
    }
}

/** Fill all admitted destinations in one joined pool operation, inlining small batches. */
template<class Traits, class Batch>
    requires random_packed_batch<Batch, Traits>
[[nodiscard]] std::expected<void, generation_error> generate_packed_batch(
    Batch& destinations, std::span<const std::uint32_t> cdf, std::uint64_t seed) {
    if (cdf.size() != Traits::alphabet_size)
        return std::unexpected(generation_error::invalid_parameter);
    std::size_t total_chunks{};
    std::size_t total_units{};
    for (std::size_t index = 0; index < destinations.size(); ++index) {
        const random_packed_destination<Traits> destination = destinations[index];
        constexpr auto letters_per_unit = Traits::letters_per_unit;
        const auto units = destination.symbol_count / letters_per_unit
            + static_cast<std::uint64_t>(destination.symbol_count % letters_per_unit != 0u);
        if ((!destination.units.empty() && destination.units.data() == nullptr) ||
            destination.first_output_unit > units || destination.units.size() > units - destination.first_output_unit)
            return std::unexpected(generation_error::output_contract);
        const auto sequence_chunks = units / k_chunk_units + static_cast<std::uint64_t>(units % k_chunk_units != 0u);
        if (!destination.row_seed && sequence_chunks != 0u && destination.sequence_index >
            ((std::numeric_limits<std::uint64_t>::max)() - sequence_chunks + 1u) / sequence_chunks)
            return std::unexpected(generation_error::size_overflow);
        const auto output_end = destination.first_output_unit + destination.units.size();
        const auto chunk_count = destination.units.empty() ? 0u : output_end / k_chunk_units
            + static_cast<std::uint64_t>(output_end % k_chunk_units != 0u)
            - destination.first_output_unit / k_chunk_units;
        if (chunk_count > (std::numeric_limits<std::size_t>::max)() - total_chunks ||
            destination.units.size() > (std::numeric_limits<std::size_t>::max)() - total_units)
            return std::unexpected(generation_error::size_overflow);
        total_chunks += static_cast<std::size_t>(chunk_count);
        total_units += destination.units.size();
    }
    if (total_chunks == 0u) return {};
    const auto run_rows = [&destinations, cdf, seed](std::size_t first, std::size_t last) {
        for (auto index = first; index < last; ++index) {
            const random_packed_destination<Traits> destination = destinations[index];
            if (destination.units.empty()) continue;
            const auto output_end = destination.first_output_unit + destination.units.size();
            const auto final_chunk = output_end / k_chunk_units
                + static_cast<std::uint64_t>(output_end % k_chunk_units != 0u);
            for (auto chunk = destination.first_output_unit / k_chunk_units; chunk < final_chunk; ++chunk)
                generate_sequence_chunk<Traits>(destination, cdf, seed, chunk);
        }
    };
    const auto run = [&destinations, cdf, seed](std::size_t first, std::size_t last) {
        std::size_t prefix{};
        for (std::size_t index = 0; index < destinations.size() && prefix < last; ++index) {
            const random_packed_destination<Traits> destination = destinations[index];
            const auto first_chunk = destination.first_output_unit / k_chunk_units;
            const auto output_end = destination.first_output_unit + destination.units.size();
            const auto count = destination.units.empty() ? 0u : output_end / k_chunk_units
                + static_cast<std::uint64_t>(output_end % k_chunk_units != 0u) - first_chunk;
            const auto next_prefix = prefix + static_cast<std::size_t>(count);
            if (next_prefix > first) {
                const auto begin = first > prefix ? first - prefix : 0u;
                const auto end = std::min(last, next_prefix) - prefix;
                for (auto chunk = begin; chunk < end; ++chunk)
                    generate_sequence_chunk<Traits>(destination, cdf, seed, first_chunk + chunk);
            }
            prefix = next_prefix;
        }
    };
    constexpr std::size_t parallel_units = 64u * 1024u / sizeof(typename Traits::storage_unit);
    if (total_units < parallel_units || total_chunks < 2u) run_rows(0u, destinations.size());
    else if (destinations.size() > 1u && total_chunks / destinations.size() <= 64u)
        bioseq::parallelism::global_pool::instance().parallel_for(
            0u, destinations.size(), 16u, bioseq::parallelism::range_body{run_rows});
    else bioseq::parallelism::global_pool::instance().parallel_for(
        0u, total_chunks, 64u, bioseq::parallelism::range_body{run});
    return {};
}

/** Generate a bounded packed interval through the same single-item batch kernel. */
template<typename Traits>
void generate_units_cpu_for_sequence(
    std::span<typename Traits::storage_unit> output, std::uint64_t symbol_count,
    std::uint64_t sequence_index, std::uint64_t /*sequence_count*/,
    std::span<const std::uint32_t> cdf, std::uint64_t seed,
    std::uint64_t first_output_unit = 0u) {
    std::array destinations{random_packed_destination<Traits>{
        output, symbol_count, sequence_index, first_output_unit}};
    const auto generated = generate_packed_batch<Traits>(destinations, cdf, seed);
    DEBUG_ASSERT(generated.has_value(), "native random generation requires checked output geometry");
}

/// @brief Rounds a floating-point sample and clamps negative values to zero.
[[gnu::always_inline]] inline std::size_t clamp_round_nonneg(double value) noexcept {
    const double rounded = std::round(value);
    if (rounded >= static_cast<double>((std::numeric_limits<std::size_t>::max)()))
        return (std::numeric_limits<std::size_t>::max)();
    return rounded > 0.0 ? static_cast<std::size_t>(rounded) : std::size_t{0};
}

/** Draw a Bernoulli event with the shared integer threshold and exact endpoint behavior. */
[[nodiscard, gnu::always_inline]] inline bool sample_mutation_event(
    double probability, taus_lcg_state& random) noexcept {
    return probability >= 1.0 || (probability > 0.0 && next_u32(random) <
        static_cast<std::uint32_t>(probability * 4294967296.0));
}

/** Draw an event extent, rejecting nonrepresentable values before converting to an integer. */
[[nodiscard]] inline std::expected<std::size_t, generation_error> sample_mutation_length(
    double mean, double stddev, taus_lcg_state& random) {
    const auto value = std::round(next_gaussian(random, mean, stddev));
    if (!std::isfinite(value) || value > static_cast<double>((std::numeric_limits<std::uint32_t>::max)()))
        return std::unexpected(generation_error::size_overflow);
    return value > 0.0 ? static_cast<std::size_t>(value) : 0u;
}

/// @brief Sample and round a fragment length, keeping it between one and
/// upper_bound - 1. Return  one when the reference bound is at most one.
[[nodiscard]] inline std::size_t sample_fragment_length(
    taus_lcg_state& state,
    std::size_t mean,
    double stddev,
    std::size_t upper_bound) {
    if (upper_bound <= 1) {
        return 1;
    }
    std::size_t length =
        clamp_round_nonneg(next_gaussian(state, static_cast<double>(mean), stddev));
    if (length == 0) {
        length = 1;
    }
    if (length >= upper_bound) {
        length = upper_bound - 1;
    }
    return length;
}

/// @brief Replace randomly selected symbols with different alphabet ranks.
/// @tparam p_sequence_traits Alphabet and rank types.
/// @param ranks Sequence ranks to edit in, place.
/// @param rate Substitution probability for each rank.
/// @param state Random state advanced by selection and replacement draws.
template <typename p_sequence_traits>
void apply_substitutions(
    std::span<typename p_sequence_traits::letter_unit> ranks,
    double rate,
    taus_lcg_state& state) {
    if (rate <= 0.0) {
        return;
    }
    for (auto& rank : ranks) {
        if (sample_mutation_event(rate, state)) {
            rank = static_cast<typename p_sequence_traits::letter_unit>(
                next_other_rank(state, p_sequence_traits::alphabet_size, rank));
        }
    }
}

/// @brief Counts observed insertion and deletion events for generator checks.
struct indel_event_counts {
    /// Source positions examined by the indel pass.
    std::uint64_t visited_positions = 0;
    /// Number of insertion events.
    std::uint64_t insertions = 0;
    /// Number of deletion events.
    std::uint64_t deletions = 0;
    /// Total number of inserted symbols.
    std::uint64_t inserted_bp = 0;
    /// Total number of deleted symbols requested by events.
    std::uint64_t deleted_bp = 0;
    /// Logical symbols actually emitted by the streaming mutation kernel.
    std::uint64_t output_symbols = 0;
};

/// @brief Throw an error when requested Vulkan generation cannot run.
/// Include the runtime initialization error or software-device details when available.
[[noreturn]] inline void throw_vulkan_generation_unavailable(const char* operation) {
    auto& runtime = bioseq::gpu::vulkan::runtime::instance();
    std::string message = "Vulkan sequence generation selected but ";
    message += operation;
    message += " cannot run";
    if (!runtime.is_usable()) {
        message += ": runtime unavailable";
        if (!runtime.initialization_error().empty()) {
            message += ": ";
            message += runtime.initialization_error();
        }
    } else if (runtime.features().software_device) {
        message += fmt::format(
            ": active Vulkan device '{}' is a CPU/software device "
            "(VkPhysicalDeviceType={}); explicit GPU generation requires a hardware Vulkan device",
            runtime.features().device_name.data(),
            runtime.features().device_type);
    }
    throw bioseq::gpu::vulkan::vulkan_runtime_error(message);
}

/// @brief Copy the source  while inserting symbols and skipping deleted spans.
/// The current symbol is kept before either event is tried Deletion skips
/// following source positions, which receive no further event checks.
/// @tparam p_sequence_traits Alphabet and rank types.
/// @tparam p_output_vector Mutable vector type for the output ranks.
/// @param in_ranks Source ranks before insertions and deletions.
/// @param params Event probabilities and length distributions.
/// @param cdf Cumulative rank boundaries for inserted symbols.
/// @param state Random state advanced by this operation.
/// @param out_ranks Replaced witm the resulting sequence ranks.
/// @param counts Optional counters to add the observed events to.
template <typename p_sequence_traits, typename p_output_vector>
void apply_indels(
    std::span<const typename p_sequence_traits::letter_unit> in_ranks,
    const generator_params& params,
    std::span<const std::uint32_t> cdf,
    taus_lcg_state& state,
    p_output_vector& out_ranks,
    indel_event_counts* counts = nullptr) {
    out_ranks.clear();
    out_ranks.reserve(in_ranks.size() + in_ranks.size() / 8);
    std::size_t position = 0;
    while (position < in_ranks.size()) {
        if (counts != nullptr) {
            counts->visited_positions += 1;
        }
        out_ranks.push_back(in_ranks[position]);
        if (sample_mutation_event(params.insertion_rate, state)) {
            const std::size_t length = clamp_round_nonneg(
                next_gaussian(state, params.insertion_length_mean, params.insertion_length_stddev));
            for (std::size_t inserted_position = 0;
                 inserted_position < length;
                 ++inserted_position) {
                out_ranks.push_back(static_cast<typename p_sequence_traits::letter_unit>(
                    sample_rank(
                        cdf.data(), p_sequence_traits::alphabet_size, next_u32(state))));
            }
            if (counts != nullptr) {
                counts->insertions += 1;
                counts->inserted_bp += length;
            }
        }

        std::size_t advance = 1;
        if (sample_mutation_event(params.deletion_rate, state)) {
            const std::size_t deleted = clamp_round_nonneg(
                next_gaussian(state, params.deletion_length_mean, params.deletion_length_stddev));
            advance += deleted;
            if (counts != nullptr) {
                counts->deletions += 1;
                counts->deleted_bp += deleted;
            }
        }
        position += advance;
    }
}

/** Dry-count destination implementing the same checked packing contract as fixed output. */
template<supports_compact_sequence_range Instance>
class counting_rank_sink {
public:
    using traits = typename compact_sequence_range<Instance>::traits;
    using storage_unit = typename traits::storage_unit;
private:
    std::uint64_t symbols_{};
    std::uint64_t limit_;
    rank_pack_status status_{rank_pack_status::ok};
public:
    /** Bound the eventual materialized extent without allocating output storage. */
    explicit counting_rank_sink(std::uint64_t limit = (std::numeric_limits<std::uint32_t>::max)())
        : limit_(limit) {}
    /** Count an already validated native rank group. */
    [[nodiscard]] rank_pack_status append_count(std::uint64_t count) {
        if (status_ != rank_pack_status::ok) return status_;
        if (count > limit_ - symbols_) return status_ = rank_pack_status::length_mismatch;
        symbols_ += count;
        return status_;
    }
    /** Validate and count one native symbol. */
    [[nodiscard]] rank_pack_status append_rank(typename traits::letter_unit rank) {
        if (rank >= traits::alphabet_size) return status_ = rank_pack_status::invalid_rank;
        return append_count(1u);
    }
    /** Count a checked packed group without retaining its storage. */
    [[nodiscard]] rank_pack_status append_prepacked_ranks(storage_unit, std::size_t count) {
        if (count > traits::letters_per_unit) return status_ = rank_pack_status::unit_count_mismatch;
        return append_count(count);
    }
    /** Return the exact number of symbols needed by replay. */
    [[nodiscard]] std::uint64_t symbol_count() const noexcept { return symbols_; }
    /** Report the same unit and padding geometry as the fixed writer. */
    [[nodiscard]] rank_pack_result finish(std::optional<std::uint64_t> expected_symbols = {},
                                         std::optional<std::uint64_t> expected_units = {}) {
        const auto units = symbols_ / traits::letters_per_unit + (symbols_ % traits::letters_per_unit != 0u);
        if (expected_symbols && *expected_symbols != symbols_) status_ = rank_pack_status::length_mismatch;
        if (expected_units && *expected_units != units) status_ = rank_pack_status::unit_count_mismatch;
        return {.status = status_, .emitted_symbols = symbols_, .written_units = units,
            .padding_letters = static_cast<std::uint16_t>(units * traits::letters_per_unit - symbols_)};
    }
};

/** A bounded native-rank reader exposes no host internals; its owner must outlive all reads. */
template<class Source, class Rank>
concept generation_rank_source = std::unsigned_integral<Rank> && requires(
        const Source& source, std::size_t from, std::size_t count, std::span<Rank> scratch) {
    { source.symbol_count } -> std::convertible_to<std::size_t>;
    { source.read_checked(from, count, scratch).has_value() } -> std::same_as<bool>;
    { *source.read_checked(from, count, scratch) } -> std::convertible_to<std::span<const Rank>>;
};

/** Adapt custom alphabets without narrowing ranks or allocating a decoded sequence. */
template<bool ReverseComplement, supports_compact_sequence_range Range>
struct compact_generation_rank_source {
    using traits = typename compact_sequence_range<Range>::traits;
    using rank_type = typename traits::letter_unit;
    const compact_sequence_range<Range>& sequence;
    const std::size_t symbol_count;

    /** Borrow an immutable range for bounded reads during mutation count and replay. */
    explicit compact_generation_rank_source(const compact_sequence_range<Range>& input)
        : sequence(input), symbol_count(input.bp_size()) {}

    /** Decode only the requested oriented window, preserving ranks wider than one byte. */
    [[nodiscard]] std::expected<std::span<const rank_type>, generation_error> read_checked(
            std::size_t from, std::size_t count, std::span<rank_type> scratch) const {
        if (from > symbol_count || count > symbol_count - from || count > scratch.size())
            return std::unexpected(generation_error::invalid_source);
        if (count == 0u) return std::span<const rank_type>{};
        if constexpr (std::same_as<rank_type, std::uint8_t>) {
            sequence.template to_encoded_into<ReverseComplement>(scratch, from, from + count);
        } else {
            const auto forward_from = [this, from, count] {
                if constexpr (ReverseComplement) return symbol_count - from - count;
                else return from;
            }();
            auto unit = sequence.cbegin();
            std::advance(unit, forward_from / traits::letters_per_unit);
            auto symbol = sequence.template iter<logical_rank>(unit);
            for (std::size_t skipped = 0u; skipped < forward_from % traits::letters_per_unit; ++skipped)
                ++symbol;
            for (std::size_t index = 0u; index < count; ++index, ++symbol) {
                const auto rank = *symbol;
                if (rank >= traits::alphabet_size)
                    return std::unexpected(generation_error::invalid_source);
                if constexpr (ReverseComplement)
                    scratch[count - index - 1u] = static_cast<rank_type>(traits::letter_complement(rank));
                else scratch[index] = static_cast<rank_type>(rank);
            }
        }
        return std::span<const rank_type>{scratch.data(), count};
    }
};

/** Append bounded native ranks using SIMD byte packing or the existing wider-rank packer. */
template<sequence_conversion_sink Writer>
[[nodiscard]] rank_pack_status append_generation_ranks(
        std::span<const typename Writer::traits::letter_unit> ranks, Writer& output) {
    using traits = typename Writer::traits;
    if constexpr (std::same_as<typename traits::letter_unit, std::uint8_t>) {
        return bioseq::simd::active_target::append_native_ranks_chunk(ranks, output);
    } else {
        for (std::size_t first = 0u; first < ranks.size();) {
            const auto count = std::min<std::size_t>(traits::letters_per_unit, ranks.size() - first);
            for (std::size_t index = 0u; index < count; ++index)
                if (ranks[first + index] >= traits::alphabet_size) return rank_pack_status::invalid_rank;
            const auto status = output.append_prepacked_ranks(pack_unit<traits>(ranks.data() + first, count), count);
            if (status != rank_pack_status::ok) return status;
            first += count;
        }
        return rank_pack_status::ok;
    }
}

/** Mutate bounded native windows; count and replay execute precisely the same event draws. */
template<sequence_alphabet Alphabet, class Source, sequence_conversion_sink Writer>
    requires generation_rank_source<Source, typename Writer::traits::letter_unit>
[[nodiscard]] std::expected<indel_event_counts, generation_error> mutate_rank_windows(
    const Source& source,
    std::size_t from, std::size_t to, const generator_params& parameters,
    taus_lcg_state& random, Writer& output,
    std::span<const std::uint32_t> insertion_cdf = {}) {
    static_assert(std::same_as<Alphabet, typename Writer::traits::alphabet_type>);
    static_assert(!bioseq::cigar_operation_alphabet<Alphabet>);
    if constexpr (requires { typename Source::traits::alphabet_type; })
        static_assert(std::same_as<Alphabet, typename Source::traits::alphabet_type>);
    if (from > to || to > source.symbol_count)
        return std::unexpected(generation_error::invalid_source);
    if constexpr (requires { source.alphabet_code; }) {
        if (source.alphabet_code != supported_storage_alphabets::template index_of<Alphabet>())
            return std::unexpected(generation_error::invalid_source);
    }
    const auto valid = validate_mutation_parameters(parameters);
    if (!valid) return std::unexpected(valid.error());
    constexpr auto cardinality = seqan3::alphabet_size<Alphabet>;
    static constexpr auto default_cdf = make_integer_cdf(alphabet_probability<Alphabet>::probability());
    const auto cdf = insertion_cdf.empty() ? std::span<const std::uint32_t>{default_cdf} : insertion_cdf;
    if (cdf.size() != cardinality) return std::unexpected(generation_error::invalid_parameter);
    constexpr auto capacity = bioseq::simd::sequence_rank_io_chunk_capacity;
    using rank_type = typename Writer::traits::letter_unit;
    std::array<rank_type, capacity> input_scratch{};
    std::array<rank_type, capacity> output_scratch{};
    std::size_t output_used{};
    indel_event_counts counts;
    const auto flush = [&output, &output_scratch, &output_used]() {
        if (output_used == 0u) return rank_pack_status::ok;
        const auto status = append_generation_ranks(
            std::span<const rank_type>{output_scratch.data(), output_used}, output);
        output_used = 0u;
        return status;
    };
    const auto append = [&output, &output_scratch, &output_used, &counts, &flush](rank_type rank) {
        if (counts.output_symbols == (std::numeric_limits<std::uint32_t>::max)()) return false;
        ++counts.output_symbols;
        if constexpr (requires { output.append_count(std::uint64_t{}); }) {
            return output.append_rank(rank) == rank_pack_status::ok;
        } else {
            output_scratch[output_used++] = rank;
            return output_used < output_scratch.size() || flush() == rank_pack_status::ok;
        }
    };
    const bool unchanged = parameters.substitution_rate == 0.0 && parameters.insertion_rate == 0.0
        && parameters.deletion_rate == 0.0;
    for (std::size_t position = from; position < to;) {
        const auto requested = std::min(capacity, to - position);
        const auto input = source.read_checked(position, requested, input_scratch);
        if (!input) return std::unexpected(generation_error::invalid_source);
        if (unchanged) {
            if (requested > (std::numeric_limits<std::uint32_t>::max)() - counts.output_symbols)
                return std::unexpected(generation_error::size_overflow);
            for (const auto rank : *input)
                if (rank >= cardinality) return std::unexpected(generation_error::invalid_source);
            const auto appended = [&output, &input, requested] {
                if constexpr (requires { output.append_count(std::uint64_t{}); })
                    return output.append_count(requested);
                else return append_generation_ranks(*input, output);
            }();
            if (appended != rank_pack_status::ok) return std::unexpected(generation_error::output_contract);
            counts.output_symbols += requested;
            counts.visited_positions += requested;
            position += requested;
            continue;
        }
        const auto window_begin = position;
        while (position - window_begin < input->size() && position < to) {
            auto rank = (*input)[position - window_begin];
            if (rank >= cardinality) return std::unexpected(generation_error::invalid_source);
            ++counts.visited_positions;
            if (sample_mutation_event(parameters.substitution_rate, random))
                rank = static_cast<rank_type>(next_other_rank(random, cardinality, rank));
            if (!append(rank)) return std::unexpected(generation_error::size_overflow);
            ++position;
            if (sample_mutation_event(parameters.insertion_rate, random)) {
                const auto extent = sample_mutation_length(parameters.insertion_length_mean,
                    parameters.insertion_length_stddev, random);
                if (!extent) return std::unexpected(extent.error());
                if (*extent > (std::numeric_limits<std::uint32_t>::max)() - counts.output_symbols)
                    return std::unexpected(generation_error::size_overflow);
                ++counts.insertions;
                counts.inserted_bp += *extent;
                for (std::size_t inserted = 0; inserted < *extent; ++inserted)
                    if (!append(static_cast<rank_type>(sample_rank(cdf.data(), cardinality, next_u32(random)))))
                        return std::unexpected(generation_error::output_contract);
            }
            if (sample_mutation_event(parameters.deletion_rate, random)) {
                const auto extent = sample_mutation_length(parameters.deletion_length_mean,
                    parameters.deletion_length_stddev, random);
                if (!extent) return std::unexpected(extent.error());
                ++counts.deletions;
                counts.deleted_bp += *extent;
                position += std::min(*extent, to - position);
            }
        }
    }
    if (flush() != rank_pack_status::ok) return std::unexpected(generation_error::output_contract);
    return counts;
}

/** Adapt an immutable compact source to the shared bounded-window mutation kernel. */
template<bool ReverseComplement = false, supports_compact_sequence_range Range, sequence_conversion_sink Writer>
    requires (!bioseq::cigar_operation_alphabet<typename compact_sequence_range<Range>::traits::alphabet_type>)
[[nodiscard]] std::expected<indel_event_counts, generation_error> mutate_sequence(
    const compact_sequence_range<Range>& source, std::size_t from, std::size_t to,
    const generator_params& parameters, taus_lcg_state& random, Writer& output,
    std::span<const std::uint32_t> insertion_cdf = {}) {
    using alphabet = typename compact_sequence_range<Range>::traits::alphabet_type;
    if (from > to || to > source.bp_size()) return std::unexpected(generation_error::invalid_source);
    const auto borrowed = [&source] {
        if constexpr (requires { typename supported_storage_alphabets::template entry_for<alphabet>; })
            return bioseq::storage::make_borrowed_rank_source<ReverseComplement>(source);
        else return compact_generation_rank_source<ReverseComplement, Range>{source};
    }();
    if constexpr (ReverseComplement)
        return mutate_rank_windows<alphabet>(borrowed, source.bp_size() - to, source.bp_size() - from,
            parameters, random, output, insertion_cdf);
    else return mutate_rank_windows<alphabet>(borrowed, from, to, parameters, random, output, insertion_cdf);
}

/** Measure canonical run output with only its trailing run retained for coalescing. */
template<bioseq::cigar_operation_alphabet Alphabet>
class counting_cigar_sink {
public:
    using value_type = bioseq::cigar_run_value<Alphabet>;
private:
    value_type trailing_{};
    std::size_t run_count_{};
    std::uint64_t preceding_symbols_{};
public:
    /** Whether no positive canonical run has been emitted. */
    [[nodiscard]] bool empty() const noexcept { return run_count_ == 0u; }
    /** Number of canonical runs required by replay. */
    [[nodiscard]] std::size_t size() const noexcept { return run_count_; }
    /** Logical extent, including in-place trailing-run coalescing. */
    [[nodiscard]] std::uint64_t symbol_count() const noexcept {
        return preceding_symbols_ + (empty() ? 0u : trailing_.length);
    }
    /** Borrow the trailing run for the existing canonical append helper. */
    [[nodiscard]] value_type& back() noexcept { return trailing_; }
    /** Count a new canonical run without allocating a run array. */
    [[nodiscard]] std::expected<void, bioseq::cigar_error> try_push_back(value_type run) {
        if (run_count_ == (std::numeric_limits<std::uint32_t>::max)())
            return std::unexpected(bioseq::cigar_error{bioseq::cigar_error_code::output_too_large});
        if (!empty()) preceding_symbols_ += trailing_.length;
        trailing_ = run;
        ++run_count_;
        return {};
    }
};

/** One of seven nonempty independent substitution/insertion/deletion event combinations. */
struct mutation_event_distribution {
    std::array<double, 7> cumulative{};
    double probability{};
    /** Resolve the union distribution once, without a per-position rejection loop. */
    explicit mutation_event_distribution(const generator_params& parameters) {
        const std::array rates{parameters.substitution_rate, parameters.insertion_rate, parameters.deletion_rate};
        for (std::size_t mask = 1u; mask < 8u; ++mask) {
            double weight = 1.0;
            for (std::size_t event = 0u; event < rates.size(); ++event)
                weight *= (mask & (std::size_t{1u} << event)) != 0u ? rates[event] : 1.0 - rates[event];
            probability += weight;
            cumulative[mask - 1u] = probability;
        }
        probability = std::min(probability, 1.0);
    }
    /** Skip an unchanged span; endpoint cases avoid invalid logarithms. */
    [[nodiscard]] std::uint64_t unchanged_prefix(taus_lcg_state& random) const {
        if (probability == 0.0) return (std::numeric_limits<std::uint64_t>::max)();
        if (probability >= 1.0) return 0u;
        const auto uniform = 1.0 - next_unit_double(random);
        const auto length = std::floor(std::log(uniform) / std::log1p(-probability));
        if (!std::isfinite(length) || length >= static_cast<double>((std::numeric_limits<std::uint64_t>::max)()))
            return (std::numeric_limits<std::uint64_t>::max)();
        return static_cast<std::uint64_t>(std::max(length, 0.0));
    }
    /** Select a nonempty event set conditional on reaching an event position. */
    [[nodiscard]] std::uint8_t events(taus_lcg_state& random) const {
        const auto draw = next_unit_double(random) * probability;
        for (std::size_t index = 0; index < cumulative.size(); ++index)
            if (draw < cumulative[index]) return static_cast<std::uint8_t>(index + 1u);
        return 7u;
    }
};

/** Adapt ordinary native-run iteration to the existing checked CIGAR cursor contract. */
template<class Iterator, class Sentinel>
struct generation_cigar_cursor {
    using run_type = std::iter_value_t<Iterator>;
    Iterator current;
    Sentinel end;
    /** Return the next borrowed run value without expanding logical positions. */
    [[nodiscard]] std::expected<std::optional<run_type>, bioseq::cigar_error> next() {
        if (current == end) return std::optional<run_type>{};
        const auto run = *current;
        ++current;
        return std::optional<run_type>{run};
    }
};

/** Mutate logical CIGAR coordinates in O(runs + events), preserving the declared space. */
template<bioseq::cigar_operation_alphabet Alphabet, bioseq::cigar_run_cursor Cursor, class Sink>
[[nodiscard]] std::expected<indel_event_counts, generation_error> mutate_cigar_runs(
    Cursor cursor, bioseq::cigar_space space, const generator_params& parameters,
    taus_lcg_state& random, Sink& output) {
    static_assert(std::same_as<typename Cursor::run_type, bioseq::cigar_run_value<Alphabet>>);
    static_assert(std::same_as<typename Sink::value_type, bioseq::cigar_run_value<Alphabet>>);
    const auto valid = validate_mutation_parameters(parameters);
    if (!valid) return std::unexpected(valid.error());
    if (!bioseq::valid_cigar_space(space)) return std::unexpected(generation_error::invalid_parameter);
    const auto mask = bioseq::cigar_space_mask<Alphabet>(space);
    std::array<Alphabet, seqan3::alphabet_size<Alphabet>> operations{};
    std::size_t operation_count{};
    for (std::size_t rank = 0; rank < operations.size(); ++rank)
        if (mask.test(rank)) operations[operation_count++] = Alphabet{}.assign_rank(
            static_cast<seqan3::alphabet_rank_t<Alphabet>>(rank));
    if (operation_count == 0u || (parameters.substitution_rate > 0.0 && operation_count < 2u))
        return std::unexpected(generation_error::invalid_parameter);
    const mutation_event_distribution events{parameters};
    auto unchanged = events.unchanged_prefix(random);
    std::uint64_t deleted_remaining{};
    indel_event_counts counts;
    const auto append = [&output, &counts](Alphabet operation, std::uint64_t length)
        -> std::expected<void, generation_error> {
        if (length == 0u) return {};
        if (length > (std::numeric_limits<std::uint32_t>::max)() - counts.output_symbols)
            return std::unexpected(generation_error::size_overflow);
        const auto written = bioseq::append_canonical_run(output, bioseq::cigar_run_value<Alphabet>{
            static_cast<std::uint32_t>(length), operation});
        if (!written) return std::unexpected(generation_error::output_contract);
        counts.output_symbols += length;
        return {};
    };
    for (;;) {
        const auto next = cursor.next();
        if (!next) return std::unexpected(generation_error::invalid_source);
        if (!*next) break;
        const auto run = **next;
        if (run.length == 0u || seqan3::to_rank(run.operation) >= seqan3::alphabet_size<Alphabet> ||
            !bioseq::visible_in_space(run.operation, space))
            return std::unexpected(generation_error::invalid_source);
        std::uint64_t remaining = run.length;
        while (remaining != 0u) {
            if (deleted_remaining != 0u) {
                const auto skip = std::min(deleted_remaining, remaining);
                deleted_remaining -= skip;
                remaining -= skip;
                continue;
            }
            if (unchanged != 0u) {
                const auto length = std::min(unchanged, remaining);
                const auto written = append(run.operation, length);
                if (!written) return std::unexpected(written.error());
                counts.visited_positions += length;
                remaining -= length;
                unchanged -= length;
                continue;
            }
            const auto selected = events.events(random);
            auto operation = run.operation;
            if ((selected & 1u) != 0u) {
                std::size_t old_index{};
                while (old_index < operation_count && operations[old_index] != operation) ++old_index;
                if (old_index == operation_count) return std::unexpected(generation_error::invalid_source);
                operation = operations[next_other_rank(random, static_cast<std::uint32_t>(operation_count),
                    static_cast<std::uint32_t>(old_index))];
            }
            const auto written = append(operation, 1u);
            if (!written) return std::unexpected(written.error());
            ++counts.visited_positions;
            --remaining;
            if ((selected & 2u) != 0u) {
                const auto length = sample_mutation_length(parameters.insertion_length_mean,
                    parameters.insertion_length_stddev, random);
                if (!length) return std::unexpected(length.error());
                if (*length > (std::numeric_limits<std::uint32_t>::max)() - counts.output_symbols)
                    return std::unexpected(generation_error::size_overflow);
                ++counts.insertions;
                counts.inserted_bp += *length;
                for (std::size_t index = 0; index < *length; ++index) {
                    const auto inserted = append(operations[next_below(random,
                        static_cast<std::uint32_t>(operation_count))], 1u);
                    if (!inserted) return std::unexpected(inserted.error());
                }
            }
            if ((selected & 4u) != 0u) {
                const auto length = sample_mutation_length(parameters.deletion_length_mean,
                    parameters.deletion_length_stddev, random);
                if (!length) return std::unexpected(length.error());
                ++counts.deletions;
                counts.deleted_bp += *length;
                deleted_remaining = *length;
            }
            unchanged = events.unchanged_prefix(random);
        }
    }
    return counts;
}

/** Borrow compact/native runs through the same checked cursor used by lazy host sources. */
template<bioseq::cigar_operation_alphabet Alphabet, std::ranges::input_range Runs, class Sink>
[[nodiscard]] std::expected<indel_event_counts, generation_error> mutate_cigar_runs(
    Runs&& runs, bioseq::cigar_space space, const generator_params& parameters,
    taus_lcg_state& random, Sink& output) {
    return mutate_cigar_runs<Alphabet>(generation_cigar_cursor{
        std::ranges::begin(runs), std::ranges::end(runs)}, space, parameters, random, output);
}

/** Generate an exact logical CIGAR extent from a paired run and sparse substitutions. */
template<bioseq::cigar_operation_alphabet Alphabet, class Sink>
[[nodiscard]] std::expected<indel_event_counts, generation_error> generate_cigar(
    std::size_t symbol_count, bioseq::cigar_space space, taus_lcg_state& random, Sink& output) {
    if (symbol_count > (std::numeric_limits<std::uint32_t>::max)())
        return std::unexpected(generation_error::size_overflow);
    if (!bioseq::valid_cigar_space(space)) return std::unexpected(generation_error::invalid_parameter);
    std::optional<Alphabet> baseline;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<Alphabet>; ++rank) {
        const auto operation = Alphabet{}.assign_rank(static_cast<seqan3::alphabet_rank_t<Alphabet>>(rank));
        const auto role = bioseq::cigar_alphabet_traits<Alphabet>::role(operation);
        if (bioseq::visible_in_space(operation, space) &&
            (role == bioseq::cigar_role::paired_unspecified || role == bioseq::cigar_role::paired_equal)) {
            baseline = operation;
            if (role == bioseq::cigar_role::paired_unspecified) break;
        }
    }
    if (!baseline) return std::unexpected(generation_error::unsupported_layout);
    if (symbol_count == 0u) return indel_event_counts{};
    generator_params parameters;
    parameters.insertion_rate = 0.0;
    parameters.deletion_rate = 0.0;
    std::array runs{bioseq::cigar_run_value<Alphabet>{static_cast<std::uint32_t>(symbol_count), *baseline}};
    return mutate_cigar_runs<Alphabet>(runs, space, parameters, random, output);
}

/// @brief Generate repeatable references and mutated fragments with  known origins.
/// @tparam p_sequence_config Packed sequence type settings.
/// @tparam p_probability_model Rank probabilities chosen at compile time.
template <
    packed_sequence_config p_sequence_config,
    typename p_probability_model = alphabet_probability<
        typename compact_sequence_range<p_sequence_config>::traits::alphabet_type>>
requires is_valid_alphabet_probability<p_probability_model>
class sequence_generator {
public:
  /// Packed sequence type produced by this generator.
  using sequence_type = compact_sequence_range<p_sequence_config>;
  /// Traits for the generated packed sequence.
  using sequence_traits = typename sequence_type::traits;
  /// Alphabet used by generated sequences.
  using alphabet_type = typename sequence_traits::alphabet_type;
  /// Packed storage word type.
  using storage_unit_type = typename sequence_traits::storage_unit;
  /// Integer type used for decoded alphabet ranks.
  using rank_type = typename sequence_traits::letter_unit;

  static_assert(std::same_as<typename p_probability_model::alphabet_type, alphabet_type>,
                "probability model must describe the configured sequence alphabet");

  /// @brief Copy the settings used for subsequent generation calls.
  /// @param params Sequence sizes, mutation rates, seed, and backend choice.
  explicit sequence_generator(const generator_params& params) noexcept : params_(params) {
      if constexpr (!seqan3::nucleotide_alphabet<alphabet_type>) params_.reverse_complement_rate = 0.0;
  }

  /// @brief An edited query and the reference window from which it was sampled.
  struct query_target_pair {
      /// Admission remains live until the packed query allocation is destroyed.
      bioseq::parallelism::memory_permit query_memory;
      /// Edited query sequence.
      sequence_type query;
      /// Shared immutable reference that owns the source window.
      std::shared_ptr<const sequence_type> target;
      /// Inclusive start of the source reference window.
      std::size_t truth_target_start = 0;
      /// Exclusive end of the source reference window.
      std::size_t truth_target_end = 0;
      /// Whether the window was reversed, with complementation for nucleotide alphabets.
      bool is_reverse = false;
      /// Sum of configured substitution, insertion, and deletion event rates.
      /// This is a simulation  setting, not a measured error rate for the, query.
      double effective_error_rate = 0.0;
      /// Query length before indels were applied.
      std::size_t original_query_length = 0;
  };

  /// @brief Generate packed references using the configured seed and alphabet probabilities.
  /// @param count Number of sequences, each of reference_length symbols.
  /// @return Sequences in stream-index order. Vulkan is required when use_vulkan is set.
  [[nodiscard]] std::vector<sequence_type> generate_sequences(std::size_t count) {
      constexpr std::uint32_t letters_per_unit = sequence_traits::letters_per_unit;
      const std::uint64_t symbols = params_.reference_length;
      const std::uint64_t units = (symbols + letters_per_unit - 1) / letters_per_unit;
      const std::size_t padding =
          units == 0 ? 0 : static_cast<std::size_t>(units * letters_per_unit - symbols);
      static constexpr auto k_cdf = make_integer_cdf(p_probability_model::probability());

      std::vector<sequence_type> out;
      out.reserve(count);

      if (params_.use_vulkan) {
          if constexpr (!vulkan::gpu_generatable<sequence_traits>) {
              throw_vulkan_generation_unavailable("generate_sequences");
          } else {
              const std::uint64_t total_units =
                  bioseq::common::saturating_mul(static_cast<std::uint64_t>(count), units);
              if (total_units != (std::numeric_limits<std::uint64_t>::max)() &&
                  total_units <=
                      static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) &&
                  vulkan::can_generate_units_vulkan<sequence_traits>(
                      symbols, static_cast<std::uint64_t>(count), params_.seed, k_chunk_units)) {
                  const auto cdf_span = std::span<const std::uint32_t>{k_cdf.data(), k_cdf.size()};
                  const std::uint64_t reserve =
                      vulkan::estimate_generate_units_peak_bytes<sequence_traits>(total_units,
                                                                                  cdf_span);
                  auto permit =
                      bioseq::parallelism::memory_admission_controller::instance().try_reserve(
                          reserve);
                  if (permit) {
                      std::vector<storage_unit_type> block(static_cast<std::size_t>(total_units));
                      const bool on_gpu = vulkan::try_generate_units_vulkan<sequence_traits>(
                          block,
                          symbols,
                          static_cast<std::uint64_t>(count),
                          cdf_span,
                          params_.seed,
                          k_chunk_units,
                          true);
                      if (on_gpu) {
                          const std::size_t units_size = static_cast<std::size_t>(units);
                          for (std::size_t seq_index = 0; seq_index < count; ++seq_index) {
                              std::vector<storage_unit_type> data(units_size);
                              const std::size_t first = seq_index * units_size;
                              std::copy(block.begin() + static_cast<std::ptrdiff_t>(first),
                                        block.begin() +
                                            static_cast<std::ptrdiff_t>(first + units_size),
                                        data.begin());
                              out.push_back(sequence_type::from_packed(std::move(data), padding));
                          }
                          return out;
                      }
                  }
              }
              throw_vulkan_generation_unavailable("generate_sequences");
          }
      }

      for (std::size_t seq_index = 0; seq_index < count; ++seq_index) {
          std::vector<storage_unit_type> data(static_cast<std::size_t>(units));
          generate_units_cpu_for_sequence<sequence_traits>(
              data,
              symbols,
              seq_index,
              count,
              std::span<const std::uint32_t>{k_cdf.data(), k_cdf.size()},
              params_.seed);
          out.push_back(sequence_type::from_packed(std::move(data), padding));
      }
      return out;
  }

  /// @brief Generates a reference and the requested number of query pairs.
  /// @param count Number of query fragments.
  /// @return Edited queries paired with the shared generated reference.
  [[nodiscard]] std::vector<query_target_pair> generate_sequences_pairs(std::size_t count) {
      return generate_fragment_pairs(generate_reference(), 0, count);
  }

  /// @brief Generates one shared immutable reference sequence.
  /// @return Shared owner of the packed reference.
  [[nodiscard]] std::shared_ptr<const sequence_type> generate_reference() {
      constexpr std::uint32_t letters_per_unit = sequence_traits::letters_per_unit;
      const std::uint64_t symbols = params_.reference_length;
      const std::uint64_t units = (symbols + letters_per_unit - 1) / letters_per_unit;
      static constexpr auto k_cdf = make_integer_cdf(p_probability_model::probability());

      std::vector<storage_unit_type> data(static_cast<std::size_t>(units));
      if (params_.use_vulkan) {
          if constexpr (!vulkan::gpu_generatable<sequence_traits>) {
              throw_vulkan_generation_unavailable("generate_reference");
          } else if (!vulkan::can_generate_units_vulkan<sequence_traits>(
                         symbols, 1, params_.seed, k_chunk_units)) {
              throw_vulkan_generation_unavailable("generate_reference");
          } else if (!vulkan::try_generate_units_vulkan<sequence_traits>(
                         data,
                         symbols,
                         1,
                         std::span<const std::uint32_t>{k_cdf.data(), k_cdf.size()},
                         params_.seed,
                         k_chunk_units)) {
              throw_vulkan_generation_unavailable("generate_reference");
          }
      } else {
          generate_units_cpu_for_sequence<sequence_traits>(
              data,
              symbols,
              0,
              1,
              std::span<const std::uint32_t>{k_cdf.data(), k_cdf.size()},
              params_.seed);
      }

      const std::size_t padding =
          units == 0 ? 0 : static_cast<std::size_t>(units * letters_per_unit - symbols);
      return std::make_shared<const sequence_type>(
          sequence_type::from_packed(std::move(data), padding));
  }

  /// @brief Sample, orient, and mutate query windows from a shared reference.
  /// Fragment IDs determine their random streams, so splitting a request into
  /// batches preserves the generated queries.
  /// @param reference Source of reference_length symbols, kept alive by each result
  /// @param first First fragmenj ID to generate.
  /// @param count Number of consecutive fragment IDs.
  /// @return Queries and their original reference windows in fragment-ID order.
  [[nodiscard]] std::vector<query_target_pair> generate_fragment_pairs(
      const std::shared_ptr<const sequence_type>& reference, std::size_t first, std::size_t count) {
      DEBUG_ASSERT(reference != nullptr, "generate_fragment_pairs: reference must not be null");
      if (reference == nullptr) {
          bioseq_log<logging_level::error>(
              "[generation] rejected fragment generation request "
              "(reference=false, reference_bp=0, expected_bp={}, count={})",
              params_.reference_length,
              count);
          return {};
      }
      DEBUG_ASSERT(reference->bp_size() == params_.reference_length,
                   "generate_fragment_pairs: reference length must match params");
      DEBUG_ASSERT(params_.reference_length > 0 || count == 0,
                   "generate_fragment_pairs: nonzero count requires nonempty reference");
      if (reference->bp_size() != params_.reference_length ||
          (params_.reference_length == 0 && count != 0)) {
          bioseq_log<logging_level::error>(
              "[generation] rejected fragment generation request "
              "(reference={}, reference_bp={}, expected_bp={}, count={})",
              true,
              reference->bp_size(),
              params_.reference_length,
              count);
          return {};
      }
      if (!validate_mutation_parameters(params_) || first > (std::numeric_limits<std::size_t>::max)() - count ||
          static_cast<std::uint64_t>(first) > (std::numeric_limits<std::uint64_t>::max)() - k_reference_stream_space ||
          count > (std::numeric_limits<std::uint64_t>::max)() - k_reference_stream_space - first) {
          bioseq_log<logging_level::error>("[generation] invalid fragment mutation parameters or stream interval");
          return {};
      }
      static constexpr auto k_cdf = make_integer_cdf(p_probability_model::probability());
      std::vector<query_target_pair> pairs(count);
      std::atomic<bool> failed{false};
      auto& pool = bioseq::parallelism::global_pool::instance();
      // Workers fill separate  result slots and read the same unchanged reference.
      pool.parallel_for_with_context(
          0,
          count,
          1,
          bioseq::parallelism::range_body{[this, &pairs, &reference, &failed, first](std::size_t lo,
                                                                            std::size_t hi) {
              for (std::size_t pair_index = lo; pair_index < hi; ++pair_index) {
                  const std::size_t fragment_index = first + pair_index;
                  taus_lcg_state state =
                      seed_stream(params_.seed, k_reference_stream_space + fragment_index);
                  const std::size_t length = sample_fragment_length(state,
                                                                    params_.fragment_length_mean,
                                                                    params_.fragment_length_stddev,
                                                                    params_.reference_length);
                  const std::size_t start = static_cast<std::size_t>(next_below64(
                      state, static_cast<std::uint64_t>(params_.reference_length - length + 1)));
                  const std::size_t end = start + length;
                  const bool is_reverse = next_unit_double(state) < params_.reverse_complement_rate;

                  query_target_pair pair;
                  {
                      const auto initial_random = state;
                      const auto mutate = [this, &reference, start, end, is_reverse](auto& random, auto& writer) {
                          if constexpr (seqan3::nucleotide_alphabet<alphabet_type>) {
                              if (is_reverse) return mutate_sequence<true>(*reference, start, end,
                                  params_, random, writer, k_cdf);
                          }
                          return mutate_sequence<false>(*reference, start, end, params_, random, writer, k_cdf);
                      };
                      counting_rank_sink<p_sequence_config> counter;
                      const auto counted = mutate(state, counter);
                      if (!counted) { failed.store(true, std::memory_order_relaxed); continue; }
                      const auto geometry = counter.finish();
                      bioseq::parallelism::memory_permit query_memory;
                      std::vector<storage_unit_type> query_units;
                      try {
                          bioseq::parallelism::prepare_admitted_vector_capacity<
                              std::runtime_error,
                              bioseq::parallelism::vector_admission_policy::retain_permit>(
                                  query_units, query_memory,
                                  static_cast<std::size_t>(geometry.written_units));
                          query_units.resize(static_cast<std::size_t>(geometry.written_units));
                      } catch (const std::bad_alloc&) {
                          failed.store(true, std::memory_order_relaxed); continue;
                      } catch (const std::length_error&) {
                          failed.store(true, std::memory_order_relaxed); continue;
                      } catch (const std::runtime_error&) {
                          failed.store(true, std::memory_order_relaxed); continue;
                      }
                      using sink = fixed_rank_pack_unit_sink<storage_unit_type>;
                      using writer_type = checked_rank_pack_sink<p_sequence_config, invalid_rank_policy::reject,
                          pack_capacity_policy::fixed_span, sink>;
                      writer_type writer{sink{query_units}};
                      state = initial_random;
                      const auto replayed = mutate(state, writer);
                      const auto finished = writer.finish(geometry.emitted_symbols, geometry.written_units);
                      if (!replayed || !finished || replayed->output_symbols != counted->output_symbols) {
                          failed.store(true, std::memory_order_relaxed); continue;
                      }
                      pair.query = sequence_type::from_packed(std::move(query_units), finished.padding_letters);
                      pair.query_memory = std::move(query_memory);
                  }
                  pair.target = reference;
                  pair.truth_target_start = start;
                  pair.truth_target_end = end;
                  pair.is_reverse = is_reverse;
                  pair.effective_error_rate =
                      params_.substitution_rate + params_.insertion_rate + params_.deletion_rate;
                  pair.original_query_length = length;
                  pairs[pair_index] = std::move(pair);
              }
          }});
      if (failed.load(std::memory_order_relaxed)) {
          bioseq_log<logging_level::error>("[generation] fragment mutation rejected its output extent or memory admission");
          return {};
      }
      return pairs;
  }

  /// @brief Generate CPU references for comparison with Vulkan output.
  /// @param count Number of sequences to generate.
  /// @return Sequences in stream-index order, ignoring use_vulkan.
  [[nodiscard]] std::vector<sequence_type> debug_generate_sequences_cpu_only(std::size_t count) {
      constexpr std::uint32_t letters_per_unit = sequence_traits::letters_per_unit;
      const std::uint64_t symbols = params_.reference_length;
      const std::uint64_t units = (symbols + letters_per_unit - 1) / letters_per_unit;
      const std::size_t padding =
          units == 0 ? 0 : static_cast<std::size_t>(units * letters_per_unit - symbols);
      static constexpr auto k_cdf = make_integer_cdf(p_probability_model::probability());

      std::vector<sequence_type> out;
      out.reserve(count);
      for (std::size_t seq_index = 0; seq_index < count; ++seq_index) {
          std::vector<storage_unit_type> data(static_cast<std::size_t>(units));
          generate_units_cpu_for_sequence<sequence_traits>(
              data,
              symbols,
              seq_index,
              count,
              std::span<const std::uint32_t>{k_cdf.data(), k_cdf.size()},
              params_.seed);
          out.push_back(sequence_type::from_packed(std::move(data), padding));
      }
      return out;
  }

private:
    generator_params params_;
};

/// @brief Choose a generator from an alphabet and packed-word type.
/// @tparam p_alphabet Sequence alphabet.
/// @tparam p_storage_unit Type of each packed storage word.
/// @tparam p_probability_model Rank probabilities chosen at compile time.
template <
    sequence_alphabet p_alphabet,
    typename p_storage_unit = std::uint32_t,
    typename p_probability_model = alphabet_probability<p_alphabet>>
using alphabet_sequence_generator = sequence_generator<
    sequence_custom_alphabet<p_alphabet, p_storage_unit>,
    p_probability_model>;

}  // namespace bioseq::sequences::generation
