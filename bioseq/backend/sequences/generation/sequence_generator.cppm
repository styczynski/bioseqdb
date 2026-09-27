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
#include <concepts>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.generation;

import bioseq.common.checked_arithmetic;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pool;
import bioseq.sequences;
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
    /// Standard deviation used when sampling fragment lengths before edits.
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
    /// Mean of the deletion-length distribution before rounding and clamping to zero.
    double deletion_length_mean    = 2.0;
    /// Standard deviation of deletion-event length.
    double deletion_length_stddev  = 1.0;

    /// Chance of reversing a query window, also complementing nucleotide symbols.
    double        reverse_complement_rate = 0.5;
    /// Seed shared by deterministic CPU and Vulkan streams.
    std::uint64_t seed                    = 42;

    /// Generate references with Vulkan. Unsupported requests or failed dispatches
    /// throw an error. Query extraction and mutation still run on the CPU.
    bool use_vulkan = false;
};

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
    /// @brief Give N probability 0.01 and divide the rest equally among A, C, G, and T.
    static constexpr std::array<double, alphabet_size> probability() noexcept {
        constexpr double unknown_base_rate = 0.01;
        const std::size_t unknown_base_rank =
            static_cast<std::size_t>(seq::dna5{}.assign_char('N').to_rank());
        std::array<double, alphabet_size> probabilities{};
        for (std::size_t alphabet_index = 0; alphabet_index < alphabet_size; ++alphabet_index) {
            probabilities[alphabet_index] = (1.0 - unknown_base_rate) / 4.0;
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

static_assert(is_valid_alphabet_probability<alphabet_probability<seq::dna4>>);
static_assert(is_valid_alphabet_probability<alphabet_probability<seq::dna5>>);
static_assert(is_valid_alphabet_probability<alphabet_probability<seq::aa27>>);

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
/// First stream ID used when sampling and editing query fragments.
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

/// @brief Mix seed bits with SplitMix64 before initializing a random stream.
[[gnu::always_inline]] inline std::uint64_t splitmix64(std::uint64_t state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
    state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;
    return state ^ (state >> 31);
}

/// @brief Derive the same valid generator state for a given seed and stream ID.
[[gnu::always_inline]] inline taus_lcg_state seed_stream(
    std::uint64_t global_seed, std::uint64_t stream_id) noexcept {
    const std::uint64_t stream_seed = global_seed ^ (stream_id * 0x9e3779b97f4a7c15ULL);
    const std::uint64_t first_mix = splitmix64(stream_seed);
    const std::uint64_t second_mix = splitmix64(first_mix);
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

/// @brief Choose the rank whose cumulative probability interval contains the draw.
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

/// @brief Scale a random draw to an integer below a positive bound.
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

/// @brief Choose a different rank from an alphabet with at least two ranks.
/// The original rank must belong to that alphabet.
[[gnu::always_inline]] inline std::uint32_t next_other_rank(
    taus_lcg_state& state, std::uint32_t alphabet_size, std::uint32_t original) noexcept {
    const std::uint32_t draw = next_below(state, alphabet_size - 1u);
    return draw >= original ? draw + 1u : draw;
}

/// @brief Sample a normal distribution with the requested mean and standard deviation.
inline double next_gaussian(
    taus_lcg_state& state, double mean, double stddev) noexcept {
    double radial_uniform = next_unit_double(state);
    const double angular_uniform = next_unit_double(state);
    if (radial_uniform < 1e-300) {
        radial_uniform = 1e-300;
    }
    const double radius = std::sqrt(-2.0 * std::log(radial_uniform));
    const double theta = 6.283185307179586 * angular_uniform;
    return mean + stddev * (radius * std::cos(theta));
}

/// @brief Pack ranks starting at the low bits of one storage unit.
/// @tparam p_sequence_traits Alphabet and storage layout.
/// @param ranks Valid alphabet ranks to pack.
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

/// @brief Pack sequence ranks into storage words without an extra sentinel word.
/// @tparam p_sequence_traits Alphabet and storage layout.
/// @param ranks Valid alphabet ranks in sequence order.
/// @return Packed words and the number of unused symbol slots in the last word.
template <typename p_sequence_traits>
[[nodiscard]] std::pair<std::vector<typename p_sequence_traits::storage_unit>, std::size_t>
pack_ranks_no_sentinel(std::span<const typename p_sequence_traits::letter_unit> ranks) {
    using storage_unit_type = typename p_sequence_traits::storage_unit;
    std::vector<storage_unit_type> packed_units;
    const std::size_t units =
        (ranks.size() + p_sequence_traits::letters_per_unit - 1) /
        p_sequence_traits::letters_per_unit;
    packed_units.resize(units);
    for (std::size_t unit = 0; unit < units; ++unit) {
        const std::size_t first = unit * p_sequence_traits::letters_per_unit;
        const std::size_t valid =
            std::min<std::size_t>(p_sequence_traits::letters_per_unit, ranks.size() - first);
        packed_units[unit] = pack_unit<p_sequence_traits>(ranks.data() + first, valid);
    }
    const std::size_t padding =
        units == 0 ? 0 : units * p_sequence_traits::letters_per_unit - ranks.size();
    return {std::move(packed_units), padding};
}

/// @brief Decode a reference window to ranks in the requested orientation.
/// Nucleotide windows are also complemented when reversed.
/// @tparam p_sequence_type Packed sequence type.
/// @param sequence Source sequence.
/// @param start First source position in the window.
/// @param end Position just past the window.
/// @param is_reverse Read the window backwards when true.
/// @param out_ranks Destination with room for end - start ranks.
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

/// @brief Generate one packed sequence using reproducible CPU random streams.
/// Each chunk has its own seed, so thread scheduling does not change the output.
/// @tparam p_sequence_traits Alphabet and storage layout.
/// @param out_units Destination words for one sequence.
/// @param symbols_per_seq Number of sequence symbols to generate.
/// @param seq_index Sequence number used to choose its random streams.
/// @param cdf Cumulative rank boundaries from make_integer_cdf().
/// @param seed Global random seed.
template <typename p_sequence_traits>
void generate_units_cpu_for_sequence(
    std::span<typename p_sequence_traits::storage_unit> out_units,
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_index,
    std::uint64_t /*seq_count*/,
    std::span<const std::uint32_t> cdf,
    std::uint64_t seed) {
    constexpr std::uint32_t letters_per_unit = p_sequence_traits::letters_per_unit;
    const std::uint64_t units_per_seq = (symbols_per_seq + letters_per_unit - 1) / letters_per_unit;
    const std::uint64_t chunks_per_seq = (units_per_seq + k_chunk_units - 1) / k_chunk_units;
    constexpr std::uint32_t alphabet_size = p_sequence_traits::alphabet_size;
    const std::uint64_t stream_base = seq_index * chunks_per_seq;

    auto& pool = bioseq::parallelism::global_pool::instance();
    // Give each chunk its own stream so workers can fill it in any order.
    pool.parallel_for(
        0,
        static_cast<std::size_t>(chunks_per_seq),
        1,
        bioseq::parallelism::range_body{
            [out_units, cdf_ptr = cdf.data(), symbols_per_seq, units_per_seq, stream_base, seed](
                std::size_t lo, std::size_t hi) {
                std::array<typename p_sequence_traits::letter_unit,
                           k_chunk_units * p_sequence_traits::letters_per_unit>
                    ranks;
                for (std::size_t local_chunk = lo; local_chunk < hi; ++local_chunk) {
                    taus_lcg_state state = seed_stream(seed, stream_base + local_chunk);
                    const std::uint64_t first_local_unit = local_chunk * k_chunk_units;
                    for (std::uint32_t unit_offset = 0; unit_offset < k_chunk_units;
                         ++unit_offset) {
                        const std::uint64_t local_unit = first_local_unit + unit_offset;
                        if (local_unit >= units_per_seq) {
                            break;
                        }
                        const std::uint64_t first_symbol = local_unit * letters_per_unit;
                        const std::uint32_t valid =
                            static_cast<std::uint32_t>(std::min<std::uint64_t>(
                                letters_per_unit, symbols_per_seq - first_symbol));
                        for (std::uint32_t slot = 0; slot < valid; ++slot) {
                            ranks[slot] = static_cast<typename p_sequence_traits::letter_unit>(
                                sample_rank(cdf_ptr, alphabet_size, next_u32(state)));
                        }
                        out_units[static_cast<std::size_t>(local_unit)] =
                            pack_unit<p_sequence_traits>(ranks.data(), valid);
                    }
                }
            }});
}

/// @brief Rounds a floating-point sample and clamps negative values to zero.
[[gnu::always_inline]] inline std::size_t clamp_round_nonneg(double value) noexcept {
    const double rounded = std::round(value);
    return rounded > 0.0 ? static_cast<std::size_t>(rounded) : std::size_t{0};
}

/// @brief Sample and round a fragment length, keeping it between one and
/// upper_bound - 1. Return one when the reference bound is at most one.
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
/// @param ranks Sequence ranks to edit in place.
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
    const std::uint32_t threshold = rate >= 1.0
        ? 0xFFFFFFFFu
        : static_cast<std::uint32_t>(rate * 4294967296.0);
    for (auto& rank : ranks) {
        if (rate >= 1.0 || next_u32(state) < threshold) {
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

/// @brief Copy the source while inserting symbols and skipping deleted spans.
/// The current symbol is kept before either event is tried. Deletion skips
/// following source positions, which receive no further event checks.
/// @tparam p_sequence_traits Alphabet and rank types.
/// @tparam p_output_vector Mutable vector type for the output ranks.
/// @param in_ranks Source ranks before insertions and deletions.
/// @param params Event probabilities and length distributions.
/// @param cdf Cumulative rank boundaries for inserted symbols.
/// @param state Random state advanced by this operation.
/// @param out_ranks Replaced with the resulting sequence ranks.
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
    const std::uint32_t insertion_threshold =
        params.insertion_rate <= 0.0 ? 0u :
        params.insertion_rate >= 1.0 ? 0xFFFFFFFFu :
        static_cast<std::uint32_t>(params.insertion_rate * 4294967296.0);
    const std::uint32_t deletion_threshold =
        params.deletion_rate <= 0.0 ? 0u :
        params.deletion_rate >= 1.0 ? 0xFFFFFFFFu :
        static_cast<std::uint32_t>(params.deletion_rate * 4294967296.0);

    std::size_t position = 0;
    while (position < in_ranks.size()) {
        if (counts != nullptr) {
            counts->visited_positions += 1;
        }
        out_ranks.push_back(in_ranks[position]);
        if (insertion_threshold != 0u &&
            (params.insertion_rate >= 1.0 || next_u32(state) < insertion_threshold)) {
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
        if (deletion_threshold != 0u &&
            (params.deletion_rate >= 1.0 || next_u32(state) < deletion_threshold)) {
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

/// @brief Generate repeatable references and mutated fragments with known origins.
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
  explicit sequence_generator(const generator_params& params) noexcept : params_(params) {}

  /// @brief An edited query and the reference window from which it was sampled.
  struct query_target_pair {
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
      /// This is a simulation setting, not a measured error rate for the query.
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
  /// @param reference Source of reference_length symbols, kept alive by each result.
  /// @param first First fragment ID to generate.
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
      static constexpr auto k_cdf = make_integer_cdf(p_probability_model::probability());
      std::vector<query_target_pair> pairs(count);
      auto& pool = bioseq::parallelism::global_pool::instance();
      // Workers fill separate result slots and read the same unchanged reference.
      pool.parallel_for_with_context(
          0,
          count,
          1,
          bioseq::parallelism::range_body{[this, &pairs, &reference, first](std::size_t lo,
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

                  std::vector<rank_type> fragment(length);
                  extract_window_ranks(*reference,
                                       start,
                                       end,
                                       is_reverse,
                                       std::span<rank_type>{fragment.data(), fragment.size()});
                  apply_substitutions<sequence_traits>(
                      std::span<rank_type>{fragment.data(), fragment.size()},
                      params_.substitution_rate,
                      state);

                  std::vector<rank_type> indel_ranks;
                  apply_indels<sequence_traits>(
                      std::span<const rank_type>{fragment.data(), fragment.size()},
                      params_,
                      std::span<const std::uint32_t>{k_cdf.data(), k_cdf.size()},
                      state,
                      indel_ranks);
                  auto [query_units, padding] = pack_ranks_no_sentinel<sequence_traits>(
                      std::span<const rank_type>{indel_ranks.data(), indel_ranks.size()});

                  query_target_pair pair;
                  pair.query = sequence_type::from_packed(std::move(query_units), padding);
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
