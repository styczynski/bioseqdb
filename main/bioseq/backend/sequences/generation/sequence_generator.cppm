/**
 * @file sequence_generator.cppm
 * @brief Generic synthetic sequence generator with Vulkan HybridTaus offload
 *        and bit-identical TBB CPU fallback.
 */
module;

#include "common.h"

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
#include <type_traits>
#include <utility>
#include <vector>

export module bioseq.sequences.generation;

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.parallelism.concepts;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pool;
import bioseq.sequences;
import bioseq.sequences.generation.vulkan;

export namespace bioseq::sequences::generation {

/// Simulation knobs. Substitution + indel models mirror SEAL's generators.
struct generator_params {
    std::size_t reference_length       = 10'000;
    std::size_t fragment_count         = 100;
    std::size_t fragment_length_mean   = 150;
    double      fragment_length_stddev = 10.0;

    double substitution_rate = 0.01;
    double insertion_rate    = 0.0005;
    double deletion_rate     = 0.0005;

    double insertion_length_mean   = 2.0;
    double insertion_length_stddev = 1.0;
    double deletion_length_mean    = 2.0;
    double deletion_length_stddev  = 1.0;

    double        reverse_complement_rate = 0.5;
    std::uint64_t seed                    = 42;
};

template <sequence_alphabet p_alphabet_t>
struct base_alphabet_probability {
    using alphabet_t = p_alphabet_t;
    static constexpr std::size_t alphabet_size = seqan3::alphabet_size<alphabet_t>;
    static constexpr std::array<double, alphabet_size> probability() noexcept {
        std::array<double, alphabet_size> p{};
        for (std::size_t i = 0; i < alphabet_size; ++i) {
            p[i] = 1.0 / static_cast<double>(alphabet_size);
        }
        return p;
    }
};

template <sequence_alphabet p_alphabet_t>
struct alphabet_probability : base_alphabet_probability<p_alphabet_t> {};

template <>
struct alphabet_probability<seq::dna5> : base_alphabet_probability<seq::dna5> {
    static constexpr std::array<double, alphabet_size> probability() noexcept {
        constexpr double n_rate = 0.01;
        const std::size_t n_rank =
            static_cast<std::size_t>(seq::dna5{}.assign_char('N').to_rank());
        std::array<double, alphabet_size> p{};
        for (std::size_t i = 0; i < alphabet_size; ++i) {
            p[i] = (1.0 - n_rate) / 4.0;
        }
        p[n_rank] = n_rate;
        return p;
    }
};

template <std::size_t N>
consteval bool probabilities_sum_to_one(const std::array<double, N>& p) noexcept {
    double sum = 0.0;
    for (double value : p) {
        sum += value;
    }
    return sum > 1.0 - 1e-9 && sum < 1.0 + 1e-9;
}

template <std::size_t N>
consteval bool probabilities_nonnegative(const std::array<double, N>& p) noexcept {
    for (double value : p) {
        if (value < 0.0) {
            return false;
        }
    }
    return true;
}

template <typename T>
concept is_valid_alphabet_probability =
    sequence_alphabet<typename T::alphabet_t> &&
    requires {
        { T::alphabet_size } -> std::convertible_to<std::size_t>;
        { T::probability() } -> std::same_as<std::array<double, T::alphabet_size>>;
    } &&
    (T::alphabet_size == seqan3::alphabet_size<typename T::alphabet_t>) &&
    probabilities_nonnegative(T::probability()) &&
    probabilities_sum_to_one(T::probability());

static_assert(is_valid_alphabet_probability<alphabet_probability<seq::dna4>>);
static_assert(is_valid_alphabet_probability<alphabet_probability<seq::dna5>>);
static_assert(is_valid_alphabet_probability<alphabet_probability<seq::aa27>>);

template <typename R>
concept packed_sequence_config =
    is_sequence_custom_alphabet<R> &&
    requires {
        typename compact_sequence_range<R>::base_container;
        typename compact_sequence_range<R>::traits;
        { compact_sequence_range<R>::from_packed(
              typename compact_sequence_range<R>::base_container{},
              std::size_t{0}) } -> std::same_as<compact_sequence_range<R>>;
    };

inline constexpr std::uint32_t kChunkUnits = 8;
inline constexpr std::uint64_t kReferenceStreamSpace = 1ull << 40;

struct taus_lcg_state {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;
    std::uint32_t w = 0;
};

[[gnu::always_inline]] inline std::uint32_t taus_step(
    std::uint32_t value, int s1, int s2, int s3, std::uint32_t mask) noexcept {
    const std::uint32_t b = (((value << s1) ^ value) >> s2);
    return (((value & mask) << s3) ^ b);
}

[[gnu::always_inline]] inline std::uint32_t lcg_step(
    std::uint32_t value, std::uint32_t a, std::uint32_t c) noexcept {
    return a * value + c;
}

[[gnu::always_inline]] inline std::uint32_t next_u32(taus_lcg_state& state) noexcept {
    state.x = taus_step(state.x, 13, 19, 12, 4294967294u);
    state.y = taus_step(state.y,  2, 25,  4, 4294967288u);
    state.z = taus_step(state.z,  3, 11, 17, 4294967280u);
    state.w = lcg_step(state.w, 1664525u, 1013904223u);
    return state.x ^ state.y ^ state.z ^ state.w;
}

[[gnu::always_inline]] inline std::uint64_t splitmix64(std::uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

[[gnu::always_inline]] inline taus_lcg_state seed_stream(
    std::uint64_t global_seed, std::uint64_t stream_id) noexcept {
    const std::uint64_t s = global_seed ^ (stream_id * 0x9e3779b97f4a7c15ULL);
    const std::uint64_t a = splitmix64(s);
    const std::uint64_t b = splitmix64(a);
    taus_lcg_state state{
        static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(a >> 32),
        static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(b >> 32)};
    if (state.x < 129u) state.x += 129u;
    if (state.y < 129u) state.y += 129u;
    if (state.z < 129u) state.z += 129u;
    return state;
}

template <std::size_t N>
constexpr std::array<std::uint32_t, N> make_integer_cdf(
    const std::array<double, N>& probabilities) noexcept {
    std::array<std::uint32_t, N> cdf{};
    double acc = 0.0;
    for (std::size_t k = 0; k < N; ++k) {
        acc += probabilities[k];
        const double scaled = acc * 4294967296.0;
        cdf[k] = scaled >= 4294967295.0
            ? 0xFFFFFFFFu
            : static_cast<std::uint32_t>(scaled);
    }
    return cdf;
}

[[gnu::always_inline]] inline std::uint32_t sample_rank(
    const std::uint32_t* cdf, std::uint32_t alphabet_size, std::uint32_t draw) noexcept {
    std::uint32_t rank = 0;
    while (rank + 1u < alphabet_size && draw >= cdf[rank]) {
        ++rank;
    }
    return rank;
}

[[gnu::always_inline]] inline double next_unit_double(taus_lcg_state& state) noexcept {
    return static_cast<double>(next_u32(state)) * 2.3283064365386963e-10;
}

[[gnu::always_inline]] inline std::uint32_t next_below(
    taus_lcg_state& state, std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(next_u32(state)) * n) >> 32);
}

[[gnu::always_inline]] inline std::uint64_t next_u64(taus_lcg_state& state) noexcept {
    const std::uint64_t hi = next_u32(state);
    const std::uint64_t lo = next_u32(state);
    return (hi << 32) | lo;
}

[[gnu::always_inline]] inline std::uint64_t next_below64(
    taus_lcg_state& state, std::uint64_t n) noexcept {
    if (n == 0) {
        return 0;
    }
    const std::uint64_t threshold = (std::uint64_t{0} - n) % n;
    for (;;) {
        const std::uint64_t x = next_u64(state);
        if (x >= threshold) {
            return x % n;
        }
    }
}

[[gnu::always_inline]] inline std::uint32_t next_other_rank(
    taus_lcg_state& state, std::uint32_t alphabet_size, std::uint32_t original) noexcept {
    const std::uint32_t draw = next_below(state, alphabet_size - 1u);
    return draw >= original ? draw + 1u : draw;
}

inline double next_gaussian(
    taus_lcg_state& state, double mean, double stddev) noexcept {
    double u1 = next_unit_double(state);
    const double u2 = next_unit_double(state);
    if (u1 < 1e-300) {
        u1 = 1e-300;
    }
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double theta = 6.283185307179586 * u2;
    return mean + stddev * (r * std::cos(theta));
}

template <typename Traits>
[[gnu::always_inline]] constexpr typename Traits::storage_unit pack_unit(
    const typename Traits::letter_unit* ranks, std::size_t valid) noexcept {
    using unit_t = typename Traits::storage_unit;
    unit_t acc = 0;
    for (std::size_t slot = 0; slot < valid; ++slot) {
        acc = static_cast<unit_t>(
            acc | (static_cast<unit_t>(ranks[slot]) << (slot * Traits::bits_per_char)));
    }
    return acc;
}

template <typename Traits>
[[nodiscard]] std::pair<std::vector<typename Traits::storage_unit>, std::size_t>
pack_ranks_no_sentinel(std::span<const typename Traits::letter_unit> ranks) {
    using unit_t = typename Traits::storage_unit;
    std::vector<unit_t> out;
    const std::size_t units =
        (ranks.size() + Traits::letters_per_unit - 1) / Traits::letters_per_unit;
    out.resize(units);
    for (std::size_t unit = 0; unit < units; ++unit) {
        const std::size_t first = unit * Traits::letters_per_unit;
        const std::size_t valid =
            std::min<std::size_t>(Traits::letters_per_unit, ranks.size() - first);
        out[unit] = pack_unit<Traits>(ranks.data() + first, valid);
    }
    const std::size_t padding = units == 0 ? 0 : units * Traits::letters_per_unit - ranks.size();
    return {std::move(out), padding};
}

template <typename Seq>
void extract_window_ranks(
    const Seq& seq,
    std::size_t start,
    std::size_t end,
    bool is_reverse,
    std::span<typename Seq::traits::letter_unit> out_ranks) {
    using traits = typename Seq::traits;
    DEBUG_ASSERT(out_ranks.size() >= end - start, "extract_window_ranks: output too small");
    if (end <= start) {
        return;
    }
    if constexpr (sizeof(typename traits::letter_unit) == sizeof(std::uint8_t)) {
        auto out8 = std::span<std::uint8_t>{
            reinterpret_cast<std::uint8_t*>(out_ranks.data()), end - start};
        if (is_reverse) {
            seq.template to_encoded_into<false, true>(out8, start, end);
            if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                for (auto& rank : out_ranks) {
                    rank = static_cast<typename traits::letter_unit>(
                        traits::letter_complement(static_cast<typename traits::storage_unit>(rank)));
                }
            }
        } else {
            seq.template to_encoded_into<false, false>(out8, start, end);
        }
    } else {
        const std::size_t length = end - start;
        auto iter = seq.template iterable<BP>().begin();
        for (std::size_t i = 0; i < start; ++i) {
            ++iter;
        }
        if (is_reverse) {
            for (std::size_t i = 0; i < length; ++i) {
                auto letter = *iter;
                if constexpr (seqan3::nucleotide_alphabet<typename traits::alphabet_type>) {
                    out_ranks[length - 1 - i] = static_cast<typename traits::letter_unit>(
                        traits::letter_complement(static_cast<typename traits::storage_unit>(letter)));
                } else {
                    out_ranks[length - 1 - i] = static_cast<typename traits::letter_unit>(letter);
                }
                ++iter;
            }
        } else {
            for (std::size_t i = 0; i < length; ++i) {
                out_ranks[i] = static_cast<typename traits::letter_unit>(*iter);
                ++iter;
            }
        }
    }
}

template <typename Traits>
void generate_units_cpu_for_sequence(
    std::span<typename Traits::storage_unit> out_units,
    std::uint64_t symbols_per_seq,
    std::uint64_t seq_index,
    std::uint64_t /*seq_count*/,
    std::span<const std::uint32_t> cdf,
    std::uint64_t seed) {
    constexpr std::uint32_t lpu = Traits::letters_per_unit;
    const std::uint64_t units_per_seq = (symbols_per_seq + lpu - 1) / lpu;
    const std::uint64_t chunks_per_seq = (units_per_seq + kChunkUnits - 1) / kChunkUnits;
    const std::uint32_t alphabet_size = Traits::alphabet_size;
    const std::uint64_t stream_base = seq_index * chunks_per_seq;

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.parallel_for(0, static_cast<std::size_t>(chunks_per_seq), 1,
        bioseq::parallelism::range_body{
            [out_units, cdf_ptr = cdf.data(), symbols_per_seq, units_per_seq,
             alphabet_size, stream_base, seed](std::size_t lo, std::size_t hi) {
                typename Traits::letter_unit ranks[kChunkUnits * Traits::letters_per_unit];
                for (std::size_t local_chunk = lo; local_chunk < hi; ++local_chunk) {
                    taus_lcg_state state = seed_stream(seed, stream_base + local_chunk);
                    const std::uint64_t local_unit0 = local_chunk * kChunkUnits;
                    for (std::uint32_t u = 0; u < kChunkUnits; ++u) {
                        const std::uint64_t local_unit = local_unit0 + u;
                        if (local_unit >= units_per_seq) {
                            break;
                        }
                        const std::uint64_t sym0 = local_unit * lpu;
                        const std::uint32_t valid = static_cast<std::uint32_t>(
                            std::min<std::uint64_t>(lpu, symbols_per_seq - sym0));
                        for (std::uint32_t slot = 0; slot < valid; ++slot) {
                            ranks[slot] = static_cast<typename Traits::letter_unit>(
                                sample_rank(cdf_ptr, alphabet_size, next_u32(state)));
                        }
                        out_units[static_cast<std::size_t>(local_unit)] =
                            pack_unit<Traits>(ranks, valid);
                    }
                }
            }});
}

[[gnu::always_inline]] inline std::size_t clamp_round_nonneg(double value) noexcept {
    const double rounded = std::round(value);
    return rounded > 0.0 ? static_cast<std::size_t>(rounded) : std::size_t{0};
}

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

template <typename Traits>
void apply_substitutions(
    std::span<typename Traits::letter_unit> ranks,
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
            rank = static_cast<typename Traits::letter_unit>(
                next_other_rank(state, Traits::alphabet_size, rank));
        }
    }
}

struct indel_event_counts {
    std::uint64_t visited_positions = 0;
    std::uint64_t insertions = 0;
    std::uint64_t deletions = 0;
    std::uint64_t inserted_bp = 0;
    std::uint64_t deleted_bp = 0;
};

template <typename Traits, typename OutVec>
void apply_indels(
    std::span<const typename Traits::letter_unit> in_ranks,
    const generator_params& params,
    std::span<const std::uint32_t> cdf,
    taus_lcg_state& state,
    OutVec& out_ranks,
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

    std::size_t pos = 0;
    while (pos < in_ranks.size()) {
        if (counts != nullptr) {
            counts->visited_positions += 1;
        }
        out_ranks.push_back(in_ranks[pos]);
        if (insertion_threshold != 0u &&
            (params.insertion_rate >= 1.0 || next_u32(state) < insertion_threshold)) {
            const std::size_t length = clamp_round_nonneg(
                next_gaussian(state, params.insertion_length_mean, params.insertion_length_stddev));
            for (std::size_t i = 0; i < length; ++i) {
                out_ranks.push_back(static_cast<typename Traits::letter_unit>(
                    sample_rank(cdf.data(), Traits::alphabet_size, next_u32(state))));
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
        pos += advance;
    }
}

template <
    packed_sequence_config R,
    typename Prob = alphabet_probability<typename compact_sequence_range<R>::traits::alphabet_type>>
requires is_valid_alphabet_probability<Prob>
class sequence_generator {
public:
    using seq_t = compact_sequence_range<R>;
    using seq_traits = typename seq_t::traits;
    using alphabet_t = typename seq_traits::alphabet_type;
    using unit_t = typename seq_traits::storage_unit;
    using rank_t = typename seq_traits::letter_unit;

    static_assert(std::same_as<typename Prob::alphabet_t, alphabet_t>,
                  "Prob must describe the same alphabet as R");

    explicit sequence_generator(const generator_params& params) noexcept
        : params_(params) {}

    struct query_target_pair {
        seq_t query;
        std::shared_ptr<const seq_t> target;
        std::size_t truth_target_start = 0;
        std::size_t truth_target_end = 0;
        bool is_reverse = false;
        double effective_error_rate = 0.0;
        std::size_t original_query_length = 0;
    };

    [[nodiscard]] std::vector<seq_t> generate_sequences(std::size_t count) {
        constexpr std::uint32_t lpu = seq_traits::letters_per_unit;
        const std::uint64_t symbols = params_.reference_length;
        const std::uint64_t units = (symbols + lpu - 1) / lpu;
        const std::size_t padding =
            units == 0 ? 0 : static_cast<std::size_t>(units * lpu - symbols);
        static constexpr auto kCdf = make_integer_cdf(Prob::probability());

        std::vector<seq_t> out;
        out.reserve(count);

        if constexpr (vulkan::gpu_generatable<seq_traits>) {
            const std::uint64_t total_units = bioseq::parallelism::saturating_mul(
                static_cast<std::uint64_t>(count), units);
            if (total_units != (std::numeric_limits<std::uint64_t>::max)() &&
                total_units <= static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) &&
                vulkan::can_generate_units_vulkan<seq_traits>(
                    symbols,
                    static_cast<std::uint64_t>(count),
                    params_.seed,
                    kChunkUnits)) {
                const auto cdf_span = std::span<const std::uint32_t>{kCdf.data(), kCdf.size()};
                const std::uint64_t reserve =
                    vulkan::estimate_generate_units_peak_bytes<seq_traits>(total_units, cdf_span);
                auto permit =
                    bioseq::parallelism::MemoryAdmissionController::instance().try_reserve(reserve);
                if (permit) {
                    std::vector<unit_t> block(static_cast<std::size_t>(total_units));
                    const bool on_gpu = vulkan::try_generate_units_vulkan<seq_traits>(
                        block,
                        symbols,
                        static_cast<std::uint64_t>(count),
                        cdf_span,
                        params_.seed,
                        kChunkUnits,
                        true);
                    if (on_gpu) {
                        const std::size_t units_size = static_cast<std::size_t>(units);
                        for (std::size_t seq_index = 0; seq_index < count; ++seq_index) {
                            std::vector<unit_t> data(units_size);
                            const std::size_t first = seq_index * units_size;
                            std::copy(
                                block.begin() + static_cast<std::ptrdiff_t>(first),
                                block.begin() + static_cast<std::ptrdiff_t>(first + units_size),
                                data.begin());
                            out.push_back(seq_t::from_packed(std::move(data), padding));
                        }
                        return out;
                    }
                }
            }
        }

        for (std::size_t seq_index = 0; seq_index < count; ++seq_index) {
            std::vector<unit_t> data(static_cast<std::size_t>(units));
            generate_units_cpu_for_sequence<seq_traits>(
                data, symbols, seq_index, count,
                std::span<const std::uint32_t>{kCdf.data(), kCdf.size()},
                params_.seed);
            out.push_back(seq_t::from_packed(std::move(data), padding));
        }
        return out;
    }

    [[nodiscard]] std::vector<query_target_pair> generate_sequences_pairs(std::size_t count) {
        return generate_fragment_pairs(generate_reference(), 0, count);
    }

    [[nodiscard]] std::shared_ptr<const seq_t> generate_reference() {
        constexpr std::uint32_t lpu = seq_traits::letters_per_unit;
        const std::uint64_t symbols = params_.reference_length;
        const std::uint64_t units = (symbols + lpu - 1) / lpu;
        static constexpr auto kCdf = make_integer_cdf(Prob::probability());

        std::vector<unit_t> data(static_cast<std::size_t>(units));
        const bool on_gpu = vulkan::try_generate_units_vulkan<seq_traits>(
            data,
            symbols,
            1,
            std::span<const std::uint32_t>{kCdf.data(), kCdf.size()},
            params_.seed,
            kChunkUnits);
        if (!on_gpu) {
            generate_units_cpu_for_sequence<seq_traits>(
                data,
                symbols,
                0,
                1,
                std::span<const std::uint32_t>{kCdf.data(), kCdf.size()},
                params_.seed);
        }

        const std::size_t padding =
            units == 0 ? 0 : static_cast<std::size_t>(units * lpu - symbols);
        return std::make_shared<const seq_t>(seq_t::from_packed(std::move(data), padding));
    }

    [[nodiscard]] std::vector<query_target_pair> generate_fragment_pairs(
        const std::shared_ptr<const seq_t>& reference,
        std::size_t first,
        std::size_t count) {
        DEBUG_ASSERT(reference != nullptr, "generate_fragment_pairs: reference must not be null");
        if (reference == nullptr) {
            bioseq_log<ERR>(
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
            bioseq_log<ERR>(
                "[generation] rejected fragment generation request "
                "(reference={}, reference_bp={}, expected_bp={}, count={})",
                true,
                reference->bp_size(),
                params_.reference_length,
                count);
            return {};
        }
        static constexpr auto kCdf = make_integer_cdf(Prob::probability());
        std::vector<query_target_pair> pairs(count);
        auto& pool = bioseq::parallelism::GlobalPool::instance();
        pool.parallel_for_with_context(0, count, 1, bioseq::parallelism::range_body{
            [this, &pairs, &reference, first](std::size_t lo, std::size_t hi) {
                for (std::size_t i = lo; i < hi; ++i) {
                    const std::size_t fragment_index = first + i;
                    taus_lcg_state state =
                        seed_stream(params_.seed, kReferenceStreamSpace + fragment_index);
                    const std::size_t length = sample_fragment_length(
                        state,
                        params_.fragment_length_mean,
                        params_.fragment_length_stddev,
                        params_.reference_length);
                    const std::size_t start = static_cast<std::size_t>(next_below64(
                        state,
                        static_cast<std::uint64_t>(params_.reference_length - length + 1)));
                    const std::size_t end = start + length;
                    const bool is_reverse =
                        next_unit_double(state) < params_.reverse_complement_rate;

                    std::vector<rank_t> fragment(length);
                    extract_window_ranks(
                        *reference,
                        start,
                        end,
                        is_reverse,
                        std::span<rank_t>{fragment.data(), fragment.size()});
                    apply_substitutions<seq_traits>(
                        std::span<rank_t>{fragment.data(), fragment.size()},
                        params_.substitution_rate,
                        state);

                    std::vector<rank_t> indel_ranks;
                    apply_indels<seq_traits>(
                        std::span<const rank_t>{fragment.data(), fragment.size()},
                        params_,
                        std::span<const std::uint32_t>{kCdf.data(), kCdf.size()},
                        state,
                        indel_ranks);
                    auto [query_units, padding] = pack_ranks_no_sentinel<seq_traits>(
                        std::span<const rank_t>{indel_ranks.data(), indel_ranks.size()});

                    query_target_pair pair;
                    pair.query = seq_t::from_packed(std::move(query_units), padding);
                    pair.target = reference;
                    pair.truth_target_start = start;
                    pair.truth_target_end = end;
                    pair.is_reverse = is_reverse;
                    pair.effective_error_rate =
                        params_.substitution_rate + params_.insertion_rate + params_.deletion_rate;
                    pair.original_query_length = length;
                    pairs[i] = std::move(pair);
                }
            }});
        return pairs;
    }

    [[nodiscard]] std::vector<seq_t> debug_generate_sequences_cpu_only(std::size_t count) {
        constexpr std::uint32_t lpu = seq_traits::letters_per_unit;
        const std::uint64_t symbols = params_.reference_length;
        const std::uint64_t units = (symbols + lpu - 1) / lpu;
        const std::size_t padding =
            units == 0 ? 0 : static_cast<std::size_t>(units * lpu - symbols);
        static constexpr auto kCdf = make_integer_cdf(Prob::probability());

        std::vector<seq_t> out;
        out.reserve(count);
        for (std::size_t seq_index = 0; seq_index < count; ++seq_index) {
            std::vector<unit_t> data(static_cast<std::size_t>(units));
            generate_units_cpu_for_sequence<seq_traits>(
                data,
                symbols,
                seq_index,
                count,
                std::span<const std::uint32_t>{kCdf.data(), kCdf.size()},
                params_.seed);
            out.push_back(seq_t::from_packed(std::move(data), padding));
        }
        return out;
    }

private:
    generator_params params_;
};

template <sequence_alphabet A, typename U = std::uint32_t, typename Prob = alphabet_probability<A>>
using alphabet_sequence_generator = sequence_generator<sequence_custom_alphabet<A, U>, Prob>;

}  // namespace bioseq::sequences::generation
