/**
 * External-only adaptation of bench/e2e/index.cppm (copied before pruning).
 * Preserves its shared target/query truth, q-prefix and fresh-process contracts.
 * SQL, minimap2 invocation and Bioseq algorithm imports are intentionally absent.
 * The deterministic generator helpers below are copied verbatim from
 * sequences/generation/sequence_generator.cppm; only storage is streamed to FASTA.
 */
module;
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <unistd.h>
export module bioseq.bench.e2e_external;
namespace {
struct generator_params {
    /// Number of symbols generated for each reference.
    std::size_t reference_length       = 10'000;
    /// Default number of query fragments.
    std::size_t fragment_count         = 100;
    /// Mean query fragment length before edits.
    std::size_t fragment_length_mean   = 150;
    /// Standard deviation of query fragment length.
    double      fragment_length_stddev = 10.0;

    /// Independent substitution probability per source symbol.
    double substitution_rate = 0.01;
    /// Independent insertion-event probability per source symbol.
    double insertion_rate    = 0.0005;
    /// Independent deletion-event probability per source symbol.
    double deletion_rate     = 0.0005;

    /// Mean number of symbols in an insertion event.
    double insertion_length_mean   = 2.0;
    /// Standard deviation of insertion-event length.
    double insertion_length_stddev = 1.0;
    /// Mean number of symbols in a deletion event.
    double deletion_length_mean    = 2.0;
    /// Standard deviation of deletion-event length.
    double deletion_length_stddev  = 1.0;

    /// Probability that a query uses the reverse-complement orientation.
    double        reverse_complement_rate = 0.5;
    /// Seed shared by deterministic CPU and Vulkan streams.
    std::uint64_t seed                    = 42;

    /// Explicit selector for the Vulkan HybridTaus generator. When enabled,
    /// dispatch failure is fatal. Ordinary callers stay on the CPU path.
    bool use_vulkan = false;
};

inline constexpr std::uint32_t k_chunk_units = 8;
/// Stream-id region reserved for reference-derived query generation.
inline constexpr std::uint64_t k_reference_stream_space = 1ull << 40;

/// @brief Stores the four words of the combined Tausworthe and LCG generator.
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

/// @brief Mixes a 64-bit seed for independent stream initialization.
[[gnu::always_inline]] inline std::uint64_t splitmix64(std::uint64_t state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    state = (state ^ (state >> 30)) * 0xbf58476d1ce4e5b9ULL;
    state = (state ^ (state >> 27)) * 0x94d049bb133111ebULL;
    return state ^ (state >> 31);
}

/// @brief Derives a valid random state for one deterministic stream id.
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

/// @brief Converts rank probabilities to inclusive 32-bit CDF thresholds.
/// @tparam p_probability_count Number of rank probabilities.
/// @param probabilities Probability assigned to each rank.
/// @return Inclusive thresholds in rank order.
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

/// @brief Maps a random draw to its rank through a compact CDF table.
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

/// @brief Returns a uniformly distributed integer below the supplied bound.
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

/// @brief Returns an unbiased 64-bit remainder below the supplied bound.
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

/// @brief Samples a rank that differs from the original rank.
[[gnu::always_inline]] inline std::uint32_t next_other_rank(
    taus_lcg_state& state, std::uint32_t alphabet_size, std::uint32_t original) noexcept {
    const std::uint32_t draw = next_below(state, alphabet_size - 1u);
    return draw >= original ? draw + 1u : draw;
}

/// @brief Samples a Gaussian value with the requested mean and deviation.
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

[[gnu::always_inline]] inline std::size_t clamp_round_nonneg(double value) noexcept {
    const double rounded = std::round(value);
    return rounded > 0.0 ? static_cast<std::size_t>(rounded) : std::size_t{0};
}

/// @brief Samples a nonempty fragment length below the reference bound.
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

/// @brief Applies independent substitutions while preserving alphabet ranks.
/// @tparam p_sequence_traits Alphabet and rank traits.
/// @param ranks Logical ranks modified in place.
/// @param rate Independent substitution probability per rank.
/// @param state Deterministic random state advanced by this operation.
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

/// @brief Records indel events for deterministic generator diagnostics.
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


/** Traits preserve uint64 packed-stream boundaries used by the baseline. */
template<std::size_t Size> struct alphabet_traits {
    using letter_unit = std::uint8_t;
    static constexpr std::size_t alphabet_size = Size;
    static constexpr std::size_t letters_per_unit = 64 / (Size == 4 ? 2 : 3);
};

/** Generate a reference interval from its original independent stream ids. */
template<std::size_t Size>
std::vector<std::uint8_t> reference_slice(std::size_t begin, std::size_t end,
        std::uint64_t seed, const std::array<std::uint32_t,Size>& cdf) {
    constexpr std::size_t width = k_chunk_units * alphabet_traits<Size>::letters_per_unit;
    std::vector<std::uint8_t> ranks(end-begin);
    for (std::size_t chunk=begin/width; chunk<(end+width-1)/width; ++chunk) {
        auto state=seed_stream(seed,chunk);
        for (std::size_t offset=0; offset<width && chunk*width+offset<end; ++offset) {
            const auto rank=sample_rank(cdf.data(),Size,next_u32(state));
            const auto position=chunk*width+offset;
            if (position>=begin) ranks[position-begin]=static_cast<std::uint8_t>(rank);
        }
    }
    return ranks;
}

/** Write the original campaign's deterministic target and all query ordinals. */
template<std::size_t Size>
void generate_fixture(std::size_t length, std::uint64_t seed, const std::filesystem::path& out) {
    using traits=alphabet_traits<Size>;
    const std::string alphabet=Size==4 ? "ACGT" : "ACGNT";
    std::array<double,Size> probabilities;
    probabilities.fill(Size==4 ? 0.25 : 0.2475);
    if constexpr (Size==5) probabilities[3]=0.01;
    const auto cdf=make_integer_cdf(probabilities);
    std::filesystem::create_directories(out);
    std::ofstream target(out/"target.fasta",std::ios::binary);
    constexpr std::size_t split_bp=2000000352;
    // Same logical reference sharding as the baseline; line wrapping is cosmetic.
    for(std::size_t shard=0; shard*split_bp<length; ++shard) {
        target << ">ref_part" << shard << "\n";
        const auto end=std::min(length,(shard+1)*split_bp);
        for(std::size_t begin=shard*split_bp; begin<end;) {
            const auto stop=std::min(end,begin+std::size_t{1<<20});
            const auto ranks=reference_slice<Size>(begin,stop,seed,cdf);
            std::string bases; bases.reserve(ranks.size()+1);
            for(auto rank:ranks) bases.push_back(alphabet[rank]);
            target << bases << '\n';
            begin=stop;
        }
    }
    target.close();
    if(!target) throw std::runtime_error("target FASTA write failed");
    std::ofstream truth(out/"truth.tsv");
    std::ofstream queries(out/"queries.fasta");
    generator_params params;
    params.reference_length=length;
    params.fragment_length_mean=20000; params.fragment_length_stddev=500;
    params.substitution_rate=0.001; params.insertion_rate=0.0005; params.deletion_rate=0.0005;
    params.insertion_length_mean=1.; params.deletion_length_mean=1.; params.seed=seed;
    for(std::size_t ordinal=0; ordinal<1024; ++ordinal) {
        auto state=seed_stream(seed,k_reference_stream_space+ordinal);
        const auto count=sample_fragment_length(state,params.fragment_length_mean,params.fragment_length_stddev,length);
        const auto start=next_below64(state,length-count+1);
        const auto end=start+count;
        const bool reverse=next_unit_double(state)<params.reverse_complement_rate;
        auto ranks=reference_slice<Size>(start,end,seed,cdf);
        if(reverse) {
            std::reverse(ranks.begin(),ranks.end());
            for(auto& rank:ranks) {
                const char base=alphabet[rank];
                const char complement=base=='A'?'T':base=='C'?'G':base=='G'?'C':base=='T'?'A':'N';
                rank=static_cast<std::uint8_t>(alphabet.find(complement));
            }
        }
        apply_substitutions<traits>(ranks,params.substitution_rate,state);
        std::vector<std::uint8_t> edited;
        apply_indels<traits>(ranks,params,cdf,state,edited);
        queries << ">q" << ordinal << '\n';
        for(auto rank:edited) queries << alphabet[rank];
        queries << '\n';
        const auto window_start=start>15 ? start-15 : 0;
        const auto window_end=std::min<std::size_t>(length,end+15);
        const auto window=reference_slice<Size>(window_start,window_end,seed,cdf);
        truth << ordinal << '\t' << start << '\t' << end << '\t' << reverse << '\t' << edited.size() << '\t' << window_start << '\t';
        for(auto rank:window) truth << alphabet[rank];
        truth << '\n';
    }
    queries.close(); truth.close();
    if(!queries || !truth) throw std::runtime_error("query fixture write failed");
}
}

/** External entry point: fixture preparation or the isolated campaign controller. */
export int external_main(int argc,char** argv) {
    try {
        if(argc==6 && std::string_view(argv[1])=="fixture") {
            const auto length=std::stoull(argv[3]); const auto seed=std::stoull(argv[4]);
            if(std::string_view(argv[2])=="dna4") generate_fixture<4>(length,seed,argv[5]);
            else if(std::string_view(argv[2])=="dna5") generate_fixture<5>(length,seed,argv[5]);
            else throw std::runtime_error("unsupported campaign alphabet");
            return 0;
        }
        if(argc==3 && std::string_view(argv[1])=="run") {
            execlp("python3","python3","bench/e2e_external/campaign.py",argv[2],nullptr);
            throw std::runtime_error("cannot execute external campaign controller");
        }
        throw std::runtime_error("usage: e2e_external fixture dna4|dna5 length seed output; or run CONFIG");
    } catch(const std::exception& error) {
        std::fprintf(stderr,"e2e_external: %s\n",error.what());
        return 1;
    }
}
