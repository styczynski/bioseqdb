/**
 * @file sequence_generator_test.cpp
 * @brief Check generated sequences, known match coordinates, and CPU/GPU byte equality.
 */
#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>
#include <libassert/assert.hpp>
#include <libassert/assert-catch2.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.gpu.vulkan.runtime;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.indexing.symbol_counts;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.storage.lazy_sequence;

struct rank_300_alphabet {
    using rank_type = std::uint16_t;

    static constexpr std::size_t alphabet_size = 300;

    rank_type rank{};

    constexpr rank_300_alphabet& assign_rank(rank_type value) noexcept {
        rank = static_cast<rank_type>(value % 300u);
        return *this;
    }

    [[nodiscard]] constexpr rank_type to_rank() const noexcept {
        return rank;
    }

    constexpr rank_300_alphabet& assign_char(char value) noexcept {
        rank = static_cast<rank_type>(static_cast<unsigned char>(value) % 300u);
        return *this;
    }

    [[nodiscard]] constexpr char to_char() const noexcept {
        return static_cast<char>('!' + (rank % 90u));
    }
};

namespace {

namespace gen = bioseq::sequences::generation;
namespace gv = bioseq::gpu::vulkan;

[[nodiscard]] bool hardware_vulkan_ready() {
    return gv::hardware_device_available(gv::runtime::instance());
}

template <sequence_alphabet p_alphabet, typename p_unit>
void check_generator(std::string_view label, std::string_view valid_chars) {
    gen::generator_params gp{};
    gp.reference_length = 4000;
    gp.fragment_count = 6;
    gp.fragment_length_mean = 400;
    gp.substitution_rate = 0.02;
    gp.insertion_rate = 0.005;
    gp.deletion_rate = 0.005;
    gp.reverse_complement_rate = 0.5;
    gp.seed = 1234;

    gen::alphabet_sequence_generator<p_alphabet, p_unit> generator(gp);
    auto pairs = generator.generate_sequences_pairs(gp.fragment_count);
    ASSERT(pairs.size() == gp.fragment_count,
           fmt::format("{}: expected {} pairs, got {}", label, gp.fragment_count, pairs.size()),
           label);

    const std::set<char> allowed(valid_chars.begin(), valid_chars.end());
    bool any_reverse = false;
    for (const auto& pair : pairs) {
        const std::string target = pair.target->to_string();
        const std::string query = pair.query.to_string();
        ASSERT(target.size() == gp.reference_length,
               fmt::format("{}: target len {} != {}", label, target.size(), gp.reference_length),
               label);
        ASSERT(pair.target->bp_size() == gp.reference_length,
               fmt::format("{}: target bp_size mismatch", label), label);
        ASSERT(pair.truth_target_start < pair.truth_target_end,
               fmt::format("{}: invalid truth interval", label), label);
        ASSERT(pair.truth_target_end <= gp.reference_length,
               fmt::format("{}: truth interval out of range", label), label);
        for (const char c : target) {
            ASSERT(allowed.contains(c),
                   fmt::format("{}: target base '{}' not in alphabet", label, c), label);
        }
        ASSERT(!query.empty(), fmt::format("{}: empty query", label), label);
        for (const char c : query) {
            ASSERT(allowed.contains(c),
                   fmt::format("{}: query base '{}' not in alphabet", label, c), label);
        }

        std::vector<p_alphabet> reencoded(target.size());
        for (std::size_t i = 0; i < target.size(); ++i) {
            reencoded[i].assign_char(target[i]);
        }
        const auto roundtrip =
            compact_sequence_range<sequence_custom_alphabet<p_alphabet, p_unit>>(
                reencoded);
        ASSERT(roundtrip.to_string() == target,
               fmt::format("{}: to_string round-trip drift", label), label);
        any_reverse = any_reverse || pair.is_reverse;
    }
    ASSERT(any_reverse,
           fmt::format("{}: expected at least one reverse-complement fragment", label),
           label);

    auto repeated_pairs = gen::alphabet_sequence_generator<p_alphabet, p_unit>(gp)
                              .generate_sequences_pairs(gp.fragment_count);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        ASSERT(pairs[i].query.to_string() == repeated_pairs[i].query.to_string(),
               fmt::format("{}: query determinism drift at {}", label, i), label);
        ASSERT(pairs[i].truth_target_start == repeated_pairs[i].truth_target_start,
               fmt::format("{}: truth-start determinism drift at {}", label, i), label);
        ASSERT(pairs[i].is_reverse == repeated_pairs[i].is_reverse,
               fmt::format("{}: strand determinism drift at {}", label, i), label);
    }
    ASSERT(pairs.front().target->to_string()
               == repeated_pairs.front().target->to_string(),
           fmt::format("{}: target determinism drift", label), label);
}

}  // namespace

TEST_CASE("sequence generation produces dna4 uint32 pairs",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_generator<seq::dna4, std::uint32_t>("dna4/u32", "ACGT");
}

TEST_CASE("sequence generation produces dna4 uint64 pairs",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_generator<seq::dna4, std::uint64_t>("dna4/u64", "ACGT");
}

TEST_CASE("sequence generation produces dna5 uint32 pairs",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_generator<seq::dna5, std::uint32_t>("dna5/u32", "ACGTN");
}

TEST_CASE("sequence generation produces dna5 uint64 pairs",
          "[component:bioseq][layer:unit][feature:sequence]") {
    check_generator<seq::dna5, std::uint64_t>("dna5/u64", "ACGTN");
}

TEST_CASE("sequence generation supports aa27 uint32 pairs",
          "[component:bioseq][layer:unit][feature:sequence]") {
    gen::generator_params gp{};
    gp.reference_length = 2000;
    gp.fragment_count = 4;
    gp.seed = 7;
    auto pairs = gen::alphabet_sequence_generator<seq::aa27, std::uint32_t>(gp)
                     .generate_sequences_pairs(gp.fragment_count);
    ASSERT(pairs.size() == gp.fragment_count, "aa27 generator under-produced", pairs.size());
    ASSERT(pairs.front().target->bp_size() == gp.reference_length,
           "aa27 target bp_size mismatch", pairs.front().target->bp_size());
    ASSERT(!pairs.front().query.to_string().empty(), "aa27 query empty");
}

TEST_CASE("sequence generation extracts uint16 ranks for custom alphabets",
          "[component:bioseq][layer:unit][feature:sequence]") {
    using custom_type = sequence_custom_alphabet<rank_300_alphabet, std::uint16_t>;
    using sequence_type = compact_sequence_range<custom_type>;
    static_assert(sizeof(typename sequence_type::traits::letter_unit) > sizeof(std::uint8_t));

    gen::generator_params gp{};
    gp.reference_length = 1000;
    gp.fragment_count = 8;
    gp.fragment_length_mean = 50.0;
    gp.fragment_length_stddev = 0.0;
    gp.substitution_rate = 0.0;
    gp.insertion_rate = 0.0;
    gp.deletion_rate = 0.0;
    gp.reverse_complement_rate = 0.0;
    gp.seed = 5150;

    gen::sequence_generator<custom_type> generator(gp);
    const auto reference = generator.generate_reference();
    const auto pairs = generator.generate_fragment_pairs(reference, 0, gp.fragment_count);
    ASSERT(pairs.size() == gp.fragment_count, "custom alphabet under-produced");

    auto decode = [](const sequence_type& seq) {
        std::vector<typename sequence_type::traits::letter_unit> ranks;
        ranks.reserve(seq.bp_size());
        for (const auto unit : seq.packed_units) {
            ranks.push_back(static_cast<typename sequence_type::traits::letter_unit>(
                unit & sequence_type::traits::char_mask));
            if (ranks.size() == seq.bp_size()) {
                break;
            }
        }
        return ranks;
    };

    const auto reference_ranks = decode(*reference);
    for (const auto& pair : pairs) {
        const auto query_ranks = decode(pair.query);
        ASSERT(query_ranks.size() == gp.fragment_length_mean,
               "custom alphabet query length drift", query_ranks.size());
        ASSERT(pair.truth_target_end - pair.truth_target_start == query_ranks.size(),
               "custom alphabet truth length drift", pair.truth_target_start,
               pair.truth_target_end, query_ranks.size());
        for (std::size_t i = 0; i < query_ranks.size(); ++i) {
            ASSERT(query_ranks[i] == reference_ranks[pair.truth_target_start + i],
                   "custom alphabet rank extraction drift", i);
        }
    }
}

static_assert(gen::is_valid_alphabet_probability<gen::alphabet_probability<seq::dna4>>);
static_assert(gen::is_valid_alphabet_probability<gen::alphabet_probability<seq::dna5>>);
static_assert(gen::is_valid_alphabet_probability<gen::alphabet_probability<seq::aa27>>);
static_assert(gen::packed_sequence_config<sequence_custom_alphabet<seq::dna4, std::uint64_t>>);

TEST_CASE("GPU and CPU sequence generation produce identical bytes",
          "[component:bioseq][layer:unit][feature:sequence][resource:gpu]") {
    gen::generator_params gp{};
    gp.reference_length = 1'000'003;
    gp.fragment_count = 0;
    gp.seed = 99;
    gp.use_vulkan = true;
    auto generator = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>(gp);
    if (!hardware_vulkan_ready()) {
        REQUIRE_THROWS_AS(generator.generate_sequences(1), gv::vulkan_runtime_error);
        return;
    }
    const auto before = gv::runtime::instance().generate_units_dispatch_count();
    auto gpu_or_cpu = generator.generate_sequences(1);
    const auto after = gv::runtime::instance().generate_units_dispatch_count();
    ASSERT(after > before, "hardware Vulkan device but generate_units did not dispatch");
    auto cpu = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>(gp)
                   .debug_generate_sequences_cpu_only(1);
    ASSERT(gpu_or_cpu.front().to_string() == cpu.front().to_string(),
           "GPU/CPU bytes diverge");
}

TEST_CASE("sequence generator shaders compile for 32-bit and 64-bit packed units",
          "[component:bioseq][layer:unit][feature:sequence][resource:gpu]") {
    for (const std::uint32_t word_bits : {32u, 64u}) {
        gv::shader_compile_request request{
            .kind = gv::shader_kind::generate_units,
            .local_size = 256,
            .max_span = word_bits,
        };
        const auto spirv = gv::compile_shader_spirv(request);
        ASSERT(!spirv.empty(), "generate_units shader produced empty SPIR-V");
        const auto scan = gv::scan_spirv_layout(spirv);
        ASSERT(scan.has_binding(0) && scan.has_binding(1),
               "generate_units bindings missing");
        ASSERT(scan.has_array_stride(word_bits / 8u),
               "generate_units output array stride mismatch");
    }
}

TEST_CASE("sequence generation dispatches generate_units through the Vulkan runtime",
          "[component:bioseq][layer:unit][feature:sequence][resource:gpu]") {
    auto& runtime = gv::runtime::instance();
    if (!gv::hardware_device_available(runtime)) {
        SUCCEED("No hardware Vulkan device detected; generate_units smoke skipped");
        return;
    }

    constexpr std::uint32_t symbols = 1024u;
    constexpr std::uint32_t letters_per_unit = 32u;  // DNA4 uses two bits per base.
    constexpr std::uint32_t units =
        (symbols + letters_per_unit - 1u) / letters_per_unit;
    const std::array<std::uint32_t, 4> uniform_cdf{
        0x40000000u, 0x80000000u, 0xC0000000u, 0xFFFFFFFFu};

    gv::generate_units_push push{};
    push.symbols_per_seq = symbols;
    push.units_per_seq = units;
    push.chunks_per_seq = 1u;
    push.chunk_units = units;
    push.letters_per_unit = letters_per_unit;
    push.bits_per_char = 2u;
    push.alphabet_size = 4u;
    push.total_chunks = 1u;
    push.seed_lo = 0x51f15eedu;
    push.seed_hi = 0x9e3779b9u;

    std::vector<std::uint64_t> out(units);
    const auto before = runtime.generate_units_dispatch_count();
    auto buffer = runtime.run_generate_units_roundtrip(
        64u,
        std::span<const std::uint32_t>{uniform_cdf.data(), uniform_cdf.size()},
        push,
        out.size() * sizeof(std::uint64_t));
    runtime.download_slice(
        buffer, 0, std::as_writable_bytes(std::span<std::uint64_t>{out.data(), out.size()}));
    const auto after = runtime.generate_units_dispatch_count();

    ASSERT(after > before, "generate_units smoke kernel did not dispatch");
    bool any_nonzero = false;
    for (const std::uint64_t word : out) {
        any_nonzero = any_nonzero || word != 0u;
    }
    ASSERT(any_nonzero, "generate_units smoke kernel produced an all-zero block");
}

TEST_CASE("sequence generation preserves dna5 symbol frequencies",
          "[component:bioseq][layer:unit][feature:sequence]") {
    gen::generator_params gp{};
    gp.reference_length = 4'000'000;
    gp.fragment_count = 0;
    gp.seed = 3;
    auto seq = gen::alphabet_sequence_generator<seq::dna5, std::uint64_t>(gp)
                   .generate_sequences(1)
                   .front();
    auto lazy = bioseq::storage::make_resident_lazy_sequence(seq);
    const auto sequence_view =
        bioseq::storage::make_lazy_sequence_view(lazy);
    auto counts = bioseq::indexing::tally_symbol_counts(sequence_view);
    const double total = static_cast<double>(gp.reference_length);
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<seq::dna5>; ++rank) {
        const double p = gen::alphabet_probability<seq::dna5>::probability()[rank];
        const double mu = total * p;
        const double sigma = std::sqrt(total * p * (1.0 - p));
        ASSERT(std::abs(static_cast<double>(counts[rank]) - mu) <= 5.0 * sigma,
               fmt::format("rank {} frequency out of 5 sigma band", rank), rank);
    }
}

namespace {

template <typename p_sequence, typename p_function>
void for_each_rank_prefix(
        const p_sequence& seq, std::size_t max_bp, p_function&& fn) {
    using traits = typename p_sequence::traits;
    const std::size_t limit = std::min<std::size_t>(max_bp, seq.bp_size());
    std::size_t pos = 0;
    for (const auto unit : seq.packed_units) {
        auto value = unit;
        for (std::size_t slot = 0; slot < traits::letters_per_unit && pos < limit; ++slot) {
            fn(static_cast<typename traits::letter_unit>(value & traits::char_mask), pos);
            value = static_cast<typename traits::storage_unit>(value >> traits::bits_per_char);
            ++pos;
        }
        if (pos >= limit) {
            break;
        }
    }
}

template <typename p_sequence>
[[nodiscard]] std::vector<typename p_sequence::traits::letter_unit> ranks_for_window(
    const p_sequence& seq,
    std::size_t start,
    std::size_t end,
    bool reverse = false) {
    using rank_type = typename p_sequence::traits::letter_unit;
    std::vector<rank_type> ranks(end - start);
    gen::extract_window_ranks(
        seq, start, end, reverse, std::span<rank_type>{ranks.data(), ranks.size()});
    return ranks;
}

inline void report_validation(std::string_view topic, const std::string& detail) {
    std::cout << topic << ": " << detail << '\n';
}

template <typename p_traits, typename p_sequence>
[[nodiscard]] gen::indel_event_counts replay_indel_counts(
    const p_sequence& reference,
    const gen::generator_params& gp,
    std::size_t count,
    std::uint64_t& total_positions) {
    using rank_type = typename p_traits::letter_unit;
    static constexpr auto insertion_cdf =
        gen::make_integer_cdf(gen::alphabet_probability<seq::dna5>::probability());
    gen::indel_event_counts total{};
    total_positions = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto state = gen::seed_stream(gp.seed, gen::k_reference_stream_space + i);
        const std::size_t length = gen::sample_fragment_length(
            state, gp.fragment_length_mean, gp.fragment_length_stddev, gp.reference_length);
        const std::size_t start = static_cast<std::size_t>(gen::next_below64(
            state, static_cast<std::uint64_t>(gp.reference_length - length + 1)));
        const bool is_reverse = gen::next_unit_double(state) < gp.reverse_complement_rate;

        std::vector<rank_type> fragment(length);
        gen::extract_window_ranks(reference,
                                  start,
                                  start + length,
                                  is_reverse,
                                  std::span<rank_type>{fragment.data(), fragment.size()});
        gen::apply_substitutions<p_traits>(
            std::span<rank_type>{fragment.data(), fragment.size()}, gp.substitution_rate, state);

        std::vector<rank_type> out;
        gen::indel_event_counts local{};
        gen::apply_indels<p_traits>(
            std::span<const rank_type>{fragment.data(), fragment.size()},
            gp,
            std::span<const std::uint32_t>{insertion_cdf.data(), insertion_cdf.size()},
            state,
            out,
            &local);
        total.visited_positions += local.visited_positions;
        total.insertions += local.insertions;
        total.deletions += local.deletions;
        total.inserted_bp += local.inserted_bp;
        total.deleted_bp += local.deleted_bp;
        total_positions += length;
    }
    return total;
}

}  // namespace

TEST_CASE("sequence generation preserves large-scale statistical and coordinate contracts",
          "[component:bioseq][layer:unit][feature:sequence][resource:large_memory]") {
    using generator_type = gen::alphabet_sequence_generator<seq::dna5, std::uint64_t>;
    using sequence_type = typename generator_type::sequence_type;
    using traits = typename sequence_type::traits;

    gen::generator_params gp{};
    gp.reference_length = 2'000'000'000ull;
    gp.fragment_count = 0;
    gp.seed = 0xB10Cull;

    generator_type generator(gp);
    auto reference = generator.generate_reference();

    auto lazy = bioseq::storage::make_resident_lazy_sequence(*reference);
    const auto reference_view =
        bioseq::storage::make_lazy_sequence_view(lazy);
    const auto counts =
        bioseq::indexing::tally_symbol_counts(reference_view);
    const auto probs = gen::alphabet_probability<seq::dna5>::probability();

    for (std::size_t rank = 0; rank < seqan3::alphabet_size<seq::dna5>; ++rank) {
        const double mu = static_cast<double>(gp.reference_length) * probs[rank];
        const double sigma = std::sqrt(
            static_cast<double>(gp.reference_length)
            * probs[rank]
            * (1.0 - probs[rank]));
        ASSERT(std::abs(static_cast<double>(counts[rank]) - mu) <= 6.0 * sigma,
               fmt::format("rank {} count {} outside 6 sigma band around {}",
                           rank, counts[rank], mu));
    }
    report_validation(
        "symbol distribution",
        fmt::format("counts A/C/G/T/N={}/{}/{}/{}/{}",
                    counts[0], counts[1], counts[2], counts[3], counts[4]));

    std::uint64_t longest_run = 0;
    std::uint64_t current_run = 0;
    traits::letter_unit previous_rank = 0;
    bool have_previous = false;
    std::array<std::uint64_t, 16> dinuc{};
    traits::letter_unit previous_window_rank = 0;
    bool have_previous_window_rank = false;
    constexpr std::size_t dinucleotide_window = 200'000'000;
    for_each_rank_prefix(
        *reference,
        gp.reference_length,
        [&current_run,
         &previous_rank,
         &have_previous,
         &longest_run,
         &dinuc,
         &previous_window_rank,
         &have_previous_window_rank](traits::letter_unit rank, std::size_t pos) {
            if (!have_previous || rank != previous_rank) {
                current_run = 1;
                previous_rank = rank;
                have_previous = true;
            } else {
                ++current_run;
            }
            longest_run = std::max(longest_run, current_run);

            if (pos < dinucleotide_window) {
                if (have_previous_window_rank && previous_window_rank < 4 && rank < 4) {
                    ++dinuc[static_cast<std::size_t>(previous_window_rank) * 4u + rank];
                }
                previous_window_rank = rank;
                have_previous_window_rank = true;
            }
        });

    double entropy = 0.0;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<seq::dna5>; ++rank) {
        const double p = static_cast<double>(counts[rank])
            / static_cast<double>(gp.reference_length);
        if (p > 0.0) {
            entropy -= p * std::log2(p);
        }
    }
    ASSERT(longest_run < 40, fmt::format("longest run {} >= 40", longest_run));
    ASSERT(entropy > 1.9, fmt::format("entropy {} <= 1.9", entropy));
    report_validation(
        "run length and entropy",
        fmt::format("longest_run={} entropy={:.6f}", longest_run, entropy));

    double chi2 = 0.0;
    const double pairs = static_cast<double>(dinucleotide_window - 1);
    for (std::size_t a = 0; a < 4; ++a) {
        for (std::size_t b = 0; b < 4; ++b) {
            const double expected = pairs * probs[a] * probs[b];
            const double observed = static_cast<double>(dinuc[a * 4 + b]);
            chi2 += (observed - expected) * (observed - expected) / expected;
        }
    }
    ASSERT(chi2 < 50.0, fmt::format("dinucleotide chi2 {} >= 50", chi2));
    report_validation(
        "dinucleotide distribution", fmt::format("chi2={:.4f}", chi2));

    gen::generator_params interval_params = gp;
    interval_params.fragment_count = 1000;
    auto interval_pairs =
        generator_type(interval_params)
            .generate_fragment_pairs(reference, 0, interval_params.fragment_count);
    ASSERT(interval_pairs.size() == interval_params.fragment_count,
           fmt::format("expected {} pairs, got {}",
                       interval_params.fragment_count, interval_pairs.size()));
    for (const auto& pair : interval_pairs) {
        ASSERT(pair.truth_target_start < pair.truth_target_end &&
               pair.truth_target_end <= interval_params.reference_length,
               "truth interval out of range");
        ASSERT(pair.truth_target_end - pair.truth_target_start == pair.original_query_length,
               "original query length mismatch");
    }
    report_validation("truth intervals", fmt::format("pairs={}", interval_pairs.size()));

    gen::generator_params exact_params = gp;
    exact_params.fragment_count = 200;
    exact_params.fragment_length_mean = 5000;
    exact_params.substitution_rate = 0.0;
    exact_params.insertion_rate = 0.0;
    exact_params.deletion_rate = 0.0;
    exact_params.reverse_complement_rate = 0.0;
    auto exact_pairs = generator_type(exact_params)
                           .generate_fragment_pairs(reference, 0, exact_params.fragment_count);
    for (const auto& pair : exact_pairs) {
        const auto expected = ranks_for_window(
            *reference, pair.truth_target_start, pair.truth_target_end);
        const auto query = ranks_for_window(pair.query, 0, pair.query.bp_size());
        ASSERT(query == expected, "error-free query differs from target window");
    }
    report_validation("exact queries", fmt::format("pairs={}", exact_pairs.size()));

    gen::generator_params substitution_params = exact_params;
    substitution_params.fragment_count = 2000;
    substitution_params.substitution_rate = 0.01;
    auto substitution_pairs =
        generator_type(substitution_params)
            .generate_fragment_pairs(reference, 0, substitution_params.fragment_count);
    std::uint64_t total_bases = 0;
    std::uint64_t mismatches = 0;
    for (const auto& pair : substitution_pairs) {
        const auto expected = ranks_for_window(
            *reference, pair.truth_target_start, pair.truth_target_end);
        const auto query = ranks_for_window(pair.query, 0, pair.query.bp_size());
        ASSERT(query.size() == expected.size(), "query and target window sizes differ");
        for (std::size_t i = 0; i < query.size(); ++i) {
            mismatches += query[i] != expected[i] ? 1u : 0u;
        }
        total_bases += query.size();
    }
    const double mismatch_rate = static_cast<double>(mismatches) / static_cast<double>(total_bases);
    const double mismatch_sigma =
        std::sqrt(substitution_params.substitution_rate
                  * (1.0 - substitution_params.substitution_rate)
                  / static_cast<double>(total_bases));
    ASSERT(std::abs(mismatch_rate - substitution_params.substitution_rate)
               <= 5.0 * mismatch_sigma,
           fmt::format("mismatch rate {} outside 5 sigma band", mismatch_rate));
    report_validation(
        "substitution rate",
        fmt::format("mismatch_rate={:.8f} total_bases={}", mismatch_rate, total_bases));

    gen::generator_params indel_params = exact_params;
    indel_params.fragment_count = 2000;
    indel_params.insertion_rate = 0.01;
    indel_params.deletion_rate = 0.01;
    indel_params.insertion_length_mean = 2.0;
    indel_params.deletion_length_mean = 2.0;
    std::uint64_t indel_positions = 0;
    const auto indel_counts =
        replay_indel_counts<traits>(
            *reference, indel_params, indel_params.fragment_count, indel_positions);
    const double event_opportunities = static_cast<double>(indel_counts.visited_positions);
    const double expected_events = event_opportunities * 0.01;
    const double event_sigma = std::sqrt(event_opportunities * 0.01 * 0.99);
    ASSERT(std::abs(static_cast<double>(indel_counts.insertions) - expected_events)
               <= 5.0 * event_sigma,
           fmt::format("insertion events {} outside 5 sigma band", indel_counts.insertions));
    ASSERT(std::abs(static_cast<double>(indel_counts.deletions) - expected_events)
               <= 5.0 * event_sigma,
           fmt::format("deletion events {} outside 5 sigma band", indel_counts.deletions));
    report_validation(
        "indel rates",
        fmt::format("ins_events={} del_events={} visited_positions={} original_positions={}",
                    indel_counts.insertions, indel_counts.deletions,
                    indel_counts.visited_positions, indel_positions));

    gen::generator_params strand_params = exact_params;
    strand_params.fragment_count = 4000;
    strand_params.reverse_complement_rate = 0.5;
    auto strand_pairs = generator_type(strand_params)
                            .generate_fragment_pairs(reference, 0, strand_params.fragment_count);
    std::size_t reverse_count = 0;
    std::size_t checked_forward = 0;
    std::size_t checked_reverse = 0;
    for (const auto& pair : strand_pairs) {
        reverse_count += pair.is_reverse ? 1u : 0u;
        if ((pair.is_reverse && checked_reverse < 10) ||
            (!pair.is_reverse && checked_forward < 10)) {
            const auto expected = ranks_for_window(
                *reference,
                pair.truth_target_start,
                pair.truth_target_end,
                pair.is_reverse);
            const auto query = ranks_for_window(pair.query, 0, pair.query.bp_size());
            ASSERT(query == expected, "strand-specific query and target window differ");
            if (pair.is_reverse) {
                ++checked_reverse;
            } else {
                ++checked_forward;
            }
        }
    }
    const double reverse_fraction =
        static_cast<double>(reverse_count) / static_cast<double>(strand_pairs.size());
    const double reverse_sigma = std::sqrt(0.25 / static_cast<double>(strand_pairs.size()));
    ASSERT(std::abs(reverse_fraction - 0.5) <= 5.0 * reverse_sigma,
           fmt::format("reverse fraction {} outside 5 sigma band", reverse_fraction));
    ASSERT(checked_forward == 10 && checked_reverse == 10,
           fmt::format("checked forward={} reverse={}", checked_forward, checked_reverse));
    report_validation(
        "strand balance", fmt::format("reverse_fraction={:.6f}", reverse_fraction));

    {
        auto repeated_reference = generator_type(gp).generate_reference();
        ASSERT(reference->packed_units == repeated_reference->packed_units
                   && reference->padding() == repeated_reference->padding(),
               "same-seed references differ");
    }
    {
        gen::generator_params gpu_params = gp;
        gpu_params.use_vulkan = true;
        if (!hardware_vulkan_ready()) {
            REQUIRE_THROWS_AS(generator_type(gpu_params).generate_sequences(1),
                              gv::vulkan_runtime_error);
            report_validation(
                "GPU selection",
                "generation rejected without a hardware Vulkan device");
        } else {
            const auto before = gv::runtime::instance().generate_units_dispatch_count();
            auto gpu_or_cpu = generator_type(gpu_params).generate_sequences(1).front();
            const auto after = gv::runtime::instance().generate_units_dispatch_count();
            ASSERT(after > before,
                   "hardware Vulkan device did not dispatch sequence generation");
            auto cpu = generator_type(gpu_params).debug_generate_sequences_cpu_only(1).front();
            ASSERT(gpu_or_cpu.packed_units == cpu.packed_units && gpu_or_cpu.padding() == cpu.padding(),
                   "GPU and CPU references differ");
            report_validation(
                "GPU parity",
                fmt::format("dispatch_delta={} gpu_hardware={}",
                            after - before, hardware_vulkan_ready()));
        }
    }

    gen::generator_params wide_coordinate_params{};
    wide_coordinate_params.reference_length = 5'000'000'000ull;
    wide_coordinate_params.fragment_count = 200;
    wide_coordinate_params.fragment_length_mean = 5000;
    wide_coordinate_params.fragment_length_stddev = 100.0;
    wide_coordinate_params.substitution_rate = 0.0;
    wide_coordinate_params.insertion_rate = 0.0;
    wide_coordinate_params.deletion_rate = 0.0;
    wide_coordinate_params.reverse_complement_rate = 0.0;
    wide_coordinate_params.seed = 0xB10Cull;
    using wide_coordinate_generator_type =
        gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>;
    auto wide_reference =
        wide_coordinate_generator_type(wide_coordinate_params).generate_reference();
    auto wide_coordinate_pairs =
        wide_coordinate_generator_type(wide_coordinate_params)
            .generate_fragment_pairs(wide_reference, 0, wide_coordinate_params.fragment_count);
    bool saw_high_start = false;
    for (const auto& pair : wide_coordinate_pairs) {
        ASSERT(pair.truth_target_start < pair.truth_target_end &&
               pair.truth_target_end <= wide_coordinate_params.reference_length,
               "wide-coordinate truth interval out of range");
        saw_high_start = saw_high_start || pair.truth_target_start >= 4'294'967'296ull;
    }
    ASSERT(saw_high_start, "no fragment start reached the greater-than-32-bit range");
    report_validation(
        "wide coordinates",
        fmt::format("pairs={} saw_start_ge_2p32={}",
                    wide_coordinate_pairs.size(), saw_high_start));
}

// Check CPU/GPU byte equality before comparing generation speed.
TEST_CASE("GPU and CPU sequence generation reports throughput",
          "[component:bioseq][layer:unit][feature:sequence]"
          "[resource:gpu][resource:large_memory]") {
    using generator_type = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>;
    auto& runtime = gv::runtime::instance();
    const bool usable = hardware_vulkan_ready();
    std::cout << fmt::format(
        "[sequence_generation_throughput] gpu_hardware={} alphabet=dna4 "
        "storage=u64 iterations=min-of-3\n",
        usable);
    if (!usable) {
        SUCCEED("No hardware Vulkan device detected; GPU generation bench skipped");
        return;
    }

    // Generate a sequence once to prepare the Vulkan pipeline before  timing.
    {
        gen::generator_params warm{};
        warm.reference_length = 1'000'000;
        warm.fragment_count = 0;
        warm.seed = 1;
        warm.use_vulkan = true;
        const auto warm_seq = generator_type(warm).generate_sequences(1);
        ASSERT(!warm_seq.empty(), "gpubench warm-up produced no sequence");
    }

    constexpr int iterations = 3;
    for (const std::uint64_t n : {1'000'000ull, 10'000'000ull, 100'000'000ull,
                                  500'000'000ull, 1'000'000'000ull, 2'000'000'000ull}) {
        gen::generator_params gp{};
        gp.reference_length = n;
        gp.fragment_count = 0;
        gp.seed = 0xBEEFull;
        gp.use_vulkan = true;

        // Check that the GPU ran and produced the qame packed bytes as the CPU.
        const auto before = runtime.generate_units_dispatch_count();
        const auto gpu_seq = generator_type(gp).generate_sequences(1);
        const auto after = runtime.generate_units_dispatch_count();
        {
            const auto cpu_seq = generator_type(gp).debug_generate_sequences_cpu_only(1);
            ASSERT(gpu_seq.front().packed_units == cpu_seq.front().packed_units &&
                       gpu_seq.front().padding() == cpu_seq.front().padding(),
                   fmt::format("gpubench GPU != CPU bytes at n={}", n));
        }
        ASSERT(after > before, fmt::format("gpubench no GPU dispatch at n={}", n));

        double gpu_ms = std::numeric_limits<double>::max();
        for (int iteration = 0; iteration < iterations; ++iteration) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto seq = generator_type(gp).generate_sequences(1);
            const auto t1 = std::chrono::steady_clock::now();
            gpu_ms = std::min(gpu_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
            ASSERT(!seq.empty(), "gpubench gpu path empty");
        }
        double cpu_ms = std::numeric_limits<double>::max();
        for (int iteration = 0; iteration < iterations; ++iteration) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto seq = generator_type(gp).debug_generate_sequences_cpu_only(1);
            const auto t1 = std::chrono::steady_clock::now();
            cpu_ms = std::min(cpu_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
            ASSERT(!seq.empty(), "gpubench cpu path empty");
        }
        const double gpu_gbps = (static_cast<double>(n) / 1e9) / (gpu_ms / 1e3);
        const double cpu_gbps = (static_cast<double>(n) / 1e9) / (cpu_ms / 1e3);
        std::cout << fmt::format(
            "[sequence_generation_throughput] n={:>13} bp  "
            "GPU={:>9.3f} ms ({:>5.2f} Gbp/s)  "
            "CPU={:>9.3f} ms ({:>5.2f} Gbp/s)  speedup={:>5.2f}x  dispatch_delta={}\n",
            n, gpu_ms, gpu_gbps, cpu_ms, cpu_gbps, cpu_ms / gpu_ms, after - before);
    }
}
