/**
 * @file sequence_generator_test.cpp
 * @brief DOC0335 generic generator tests.
 */
#include "common_simd.h"
#include "common_tests.h"
#include "common.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import bioseq.common.mlogging;
import bioseq.common.sequan_compat;
import bioseq.common.testing;
import bioseq.gpu.vulkan.runtime;
import bioseq.gpu.vulkan.shader_compiler;
import bioseq.indexing.symbol_counts;
import bioseq.sequences;
import bioseq.sequences.generation;
import bioseq.storage.lazy;

struct doc0335_rank300 {
    using rank_type = std::uint16_t;

    static constexpr std::size_t alphabet_size = 300;

    rank_type rank{};

    constexpr doc0335_rank300& assign_rank(rank_type value) noexcept {
        rank = static_cast<rank_type>(value % 300u);
        return *this;
    }

    [[nodiscard]] constexpr rank_type to_rank() const noexcept {
        return rank;
    }

    constexpr doc0335_rank300& assign_char(char value) noexcept {
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
    return gv::hardware_device_available(gv::Runtime::instance());
}

template <sequence_alphabet A, typename U>
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

    gen::alphabet_sequence_generator<A, U> generator(gp);
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

        std::vector<A> reencoded(target.size());
        for (std::size_t i = 0; i < target.size(); ++i) {
            reencoded[i].assign_char(target[i]);
        }
        const auto roundtrip =
            compact_sequence_range<sequence_custom_alphabet<A, U>>(reencoded);
        ASSERT(roundtrip.to_string() == target,
               fmt::format("{}: to_string round-trip drift", label), label);
        any_reverse = any_reverse || pair.is_reverse;
    }
    ASSERT(any_reverse,
           fmt::format("{}: expected at least one reverse-complement fragment", label),
           label);

    auto pairs2 = gen::alphabet_sequence_generator<A, U>(gp)
                      .generate_sequences_pairs(gp.fragment_count);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        ASSERT(pairs[i].query.to_string() == pairs2[i].query.to_string(),
               fmt::format("{}: query determinism drift at {}", label, i), label);
        ASSERT(pairs[i].truth_target_start == pairs2[i].truth_target_start,
               fmt::format("{}: truth-start determinism drift at {}", label, i), label);
        ASSERT(pairs[i].is_reverse == pairs2[i].is_reverse,
               fmt::format("{}: strand determinism drift at {}", label, i), label);
    }
    ASSERT(pairs.front().target->to_string() == pairs2.front().target->to_string(),
           fmt::format("{}: target determinism drift", label), label);
}

}  // namespace

TEST_CASE("generator: dna4/uint32", "[generator][doc0335]") {
    check_generator<seq::dna4, std::uint32_t>("dna4/u32", "ACGT");
}

TEST_CASE("generator: dna4/uint64", "[generator][doc0335]") {
    check_generator<seq::dna4, std::uint64_t>("dna4/u64", "ACGT");
}

TEST_CASE("generator: dna5/uint32", "[generator][doc0335]") {
    check_generator<seq::dna5, std::uint32_t>("dna5/u32", "ACGTN");
}

TEST_CASE("generator: dna5/uint64", "[generator][doc0335]") {
    check_generator<seq::dna5, std::uint64_t>("dna5/u64", "ACGTN");
}

TEST_CASE("generator: aa27/uint32 genericity", "[generator][doc0335]") {
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

TEST_CASE("generator: custom alphabet with uint16 rank extraction", "[generator][doc0335]") {
    using custom_t = sequence_custom_alphabet<doc0335_rank300, std::uint16_t>;
    using seq_t = compact_sequence_range<custom_t>;
    static_assert(sizeof(typename seq_t::traits::letter_unit) > sizeof(std::uint8_t));

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

    gen::sequence_generator<custom_t> generator(gp);
    const auto reference = generator.generate_reference();
    const auto pairs = generator.generate_fragment_pairs(reference, 0, gp.fragment_count);
    ASSERT(pairs.size() == gp.fragment_count, "custom alphabet under-produced");

    auto decode = [](const seq_t& seq) {
        std::vector<typename seq_t::traits::letter_unit> ranks;
        ranks.reserve(seq.bp_size());
        for (const auto unit : seq._data) {
            ranks.push_back(static_cast<typename seq_t::traits::letter_unit>(
                unit & seq_t::traits::char_mask));
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

TEST_CASE("generator: GPU == CPU bytes", "[generator][doc0335][gpu]") {
    gen::generator_params gp{};
    gp.reference_length = 1'000'003;
    gp.fragment_count = 0;
    gp.seed = 99;
    gp.use_vulkan = true;
    auto generator = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>(gp);
    if (!hardware_vulkan_ready()) {
        REQUIRE_THROWS_AS(generator.generate_sequences(1), gv::VulkanRuntimeError);
        return;
    }
    const auto before = gv::Runtime::instance().generate_units_dispatch_count();
    auto gpu_or_cpu = generator.generate_sequences(1);
    const auto after = gv::Runtime::instance().generate_units_dispatch_count();
    ASSERT(after > before, "hardware Vulkan device but generate_units did not dispatch");
    auto cpu = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>(gp)
                   .debug_generate_sequences_cpu_only(1);
    ASSERT(gpu_or_cpu.front().to_string() == cpu.front().to_string(),
           "GPU/CPU bytes diverge");
}

TEST_CASE("generator shader compiles for 32/64-bit packed units", "[generator][doc0335][gpu]") {
    for (const std::uint32_t word_bits : {32u, 64u}) {
        gv::ShaderCompileRequest request{
            .kind = gv::ShaderKind::generate_units,
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

TEST_CASE("generator GPU smoke: generate_units dispatch requires Vulkan runtime",
          "[generator][doc0335][gpu][gpu-smoke]") {
    auto& runtime = gv::Runtime::instance();
    if (!gv::hardware_device_available(runtime)) {
        SUCCEED("No hardware Vulkan device detected; generate_units smoke skipped");
        return;
    }

    constexpr std::uint32_t symbols = 1024u;
    constexpr std::uint32_t letters_per_unit = 32u;  // dna4 packed into uint64_t
    constexpr std::uint32_t units =
        (symbols + letters_per_unit - 1u) / letters_per_unit;
    const std::array<std::uint32_t, 4> uniform_cdf{
        0x40000000u, 0x80000000u, 0xC0000000u, 0xFFFFFFFFu};

    gv::GenerateUnitsPush push{};
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

TEST_CASE("generator: dna5 symbol frequencies", "[generator][doc0335]") {
    gen::generator_params gp{};
    gp.reference_length = 4'000'000;
    gp.fragment_count = 0;
    gp.seed = 3;
    auto seq = gen::alphabet_sequence_generator<seq::dna5, std::uint64_t>(gp)
                   .generate_sequences(1)
                   .front();
    auto lazy = bioseq::storage::make_lazy_in_memory(seq);
    auto counts = bioseq::indexing::tally_symbol_counts(lazy);
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

template <typename Seq, typename Fn>
void for_each_rank_prefix(const Seq& seq, std::size_t max_bp, Fn&& fn) {
    using traits = typename Seq::traits;
    const std::size_t limit = std::min<std::size_t>(max_bp, seq.bp_size());
    std::size_t pos = 0;
    for (const auto unit : seq._data) {
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

template <typename Seq>
[[nodiscard]] std::vector<typename Seq::traits::letter_unit> ranks_for_window(
    const Seq& seq,
    std::size_t start,
    std::size_t end,
    bool reverse = false) {
    using rank_t = typename Seq::traits::letter_unit;
    std::vector<rank_t> ranks(end - start);
    gen::extract_window_ranks(seq, start, end, reverse, std::span<rank_t>{ranks.data(), ranks.size()});
    return ranks;
}

inline void manual_pass(std::string_view marker, const std::string& detail) {
    std::cout << marker << " PASS " << detail << '\n';
}

template <typename Traits, typename Seq>
[[nodiscard]] gen::indel_event_counts replay_indel_counts(
    const Seq& reference,
    const gen::generator_params& gp,
    std::size_t count,
    std::uint64_t& total_positions) {
    using rank_t = typename Traits::letter_unit;
    static constexpr auto kCdf = gen::make_integer_cdf(gen::alphabet_probability<seq::dna5>::probability());
    gen::indel_event_counts total{};
    total_positions = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto state = gen::seed_stream(gp.seed, gen::kReferenceStreamSpace + i);
        const std::size_t length = gen::sample_fragment_length(
            state, gp.fragment_length_mean, gp.fragment_length_stddev, gp.reference_length);
        const std::size_t start = static_cast<std::size_t>(gen::next_below64(
            state, static_cast<std::uint64_t>(gp.reference_length - length + 1)));
        const bool is_reverse = gen::next_unit_double(state) < gp.reverse_complement_rate;

        std::vector<rank_t> fragment(length);
        gen::extract_window_ranks(
            reference, start, start + length, is_reverse,
            std::span<rank_t>{fragment.data(), fragment.size()});
        gen::apply_substitutions<Traits>(
            std::span<rank_t>{fragment.data(), fragment.size()}, gp.substitution_rate, state);

        std::vector<rank_t> out;
        gen::indel_event_counts local{};
        gen::apply_indels<Traits>(
            std::span<const rank_t>{fragment.data(), fragment.size()},
            gp,
            std::span<const std::uint32_t>{kCdf.data(), kCdf.size()},
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

TEST_CASE("generator: DOC0335 manual 2Gbp validation harness", "[.][generator][doc0335][manual]") {
    using generator_t = gen::alphabet_sequence_generator<seq::dna5, std::uint64_t>;
    using seq_t = typename generator_t::seq_t;
    using traits = typename seq_t::traits;

    gen::generator_params gp{};
    gp.reference_length = 2'000'000'000ull;
    gp.fragment_count = 0;
    gp.seed = 0xB10Cull;

    generator_t generator(gp);
    auto reference = generator.generate_reference();

    auto lazy = bioseq::storage::make_lazy_in_memory(*reference);
    const auto counts = bioseq::indexing::tally_symbol_counts(lazy);
    const auto probs = gen::alphabet_probability<seq::dna5>::probability();

    for (std::size_t rank = 0; rank < seqan3::alphabet_size<seq::dna5>; ++rank) {
        const double mu = static_cast<double>(gp.reference_length) * probs[rank];
        const double sigma = std::sqrt(static_cast<double>(gp.reference_length) * probs[rank] * (1.0 - probs[rank]));
        ASSERT(std::abs(static_cast<double>(counts[rank]) - mu) <= 6.0 * sigma,
               fmt::format("M1 rank {} count {} outside 6 sigma band around {}",
                           rank, counts[rank], mu));
    }
    manual_pass("M1", fmt::format("counts A/C/G/T/N={}/{}/{}/{}/{}",
                                  counts[0], counts[1], counts[2], counts[3], counts[4]));

    std::uint64_t longest_run = 0;
    std::uint64_t current_run = 0;
    traits::letter_unit previous_rank = 0;
    bool have_previous = false;
    std::array<std::uint64_t, 16> dinuc{};
    traits::letter_unit previous_window_rank = 0;
    bool have_previous_window_rank = false;
    constexpr std::size_t kDinucWindow = 200'000'000;
    for_each_rank_prefix(*reference, gp.reference_length, [&](traits::letter_unit rank, std::size_t pos) {
        if (!have_previous || rank != previous_rank) {
            current_run = 1;
            previous_rank = rank;
            have_previous = true;
        } else {
            ++current_run;
        }
        longest_run = std::max(longest_run, current_run);

        if (pos < kDinucWindow) {
            if (have_previous_window_rank && previous_window_rank < 4 && rank < 4) {
                ++dinuc[static_cast<std::size_t>(previous_window_rank) * 4u + rank];
            }
            previous_window_rank = rank;
            have_previous_window_rank = true;
        }
    });

    double entropy = 0.0;
    for (std::size_t rank = 0; rank < seqan3::alphabet_size<seq::dna5>; ++rank) {
        const double p = static_cast<double>(counts[rank]) / static_cast<double>(gp.reference_length);
        if (p > 0.0) {
            entropy -= p * std::log2(p);
        }
    }
    ASSERT(longest_run < 40, fmt::format("M2 longest run {} >= 40", longest_run));
    ASSERT(entropy > 1.9, fmt::format("M2 entropy {} <= 1.9", entropy));
    manual_pass("M2", fmt::format("longest_run={} entropy={:.6f}", longest_run, entropy));

    double chi2 = 0.0;
    const double pairs = static_cast<double>(kDinucWindow - 1);
    for (std::size_t a = 0; a < 4; ++a) {
        for (std::size_t b = 0; b < 4; ++b) {
            const double expected = pairs * probs[a] * probs[b];
            const double observed = static_cast<double>(dinuc[a * 4 + b]);
            chi2 += (observed - expected) * (observed - expected) / expected;
        }
    }
    ASSERT(chi2 < 50.0, fmt::format("M3 dinucleotide chi2 {} >= 50", chi2));
    manual_pass("M3", fmt::format("dinucleotide_chi2={:.4f}", chi2));

    gen::generator_params gp_m4 = gp;
    gp_m4.fragment_count = 1000;
    auto m4_pairs = generator_t(gp_m4).generate_fragment_pairs(reference, 0, gp_m4.fragment_count);
    ASSERT(m4_pairs.size() == gp_m4.fragment_count,
           fmt::format("M4 expected {} pairs, got {}", gp_m4.fragment_count, m4_pairs.size()));
    for (const auto& pair : m4_pairs) {
        ASSERT(pair.truth_target_start < pair.truth_target_end &&
               pair.truth_target_end <= gp_m4.reference_length,
               "M4 truth interval out of range");
        ASSERT(pair.truth_target_end - pair.truth_target_start == pair.original_query_length,
               "M4 original_query_length mismatch");
    }
    manual_pass("M4", fmt::format("pairs={}", m4_pairs.size()));

    gen::generator_params gp_m5 = gp;
    gp_m5.fragment_count = 200;
    gp_m5.fragment_length_mean = 5000;
    gp_m5.substitution_rate = 0.0;
    gp_m5.insertion_rate = 0.0;
    gp_m5.deletion_rate = 0.0;
    gp_m5.reverse_complement_rate = 0.0;
    auto m5_pairs = generator_t(gp_m5).generate_fragment_pairs(reference, 0, gp_m5.fragment_count);
    for (const auto& pair : m5_pairs) {
        const auto expected = ranks_for_window(*reference, pair.truth_target_start, pair.truth_target_end);
        const auto query = ranks_for_window(pair.query, 0, pair.query.bp_size());
        ASSERT(query == expected, "M5 error-free query differs from target window");
    }
    manual_pass("M5", fmt::format("exact_pairs={}", m5_pairs.size()));

    gen::generator_params gp_m6 = gp_m5;
    gp_m6.fragment_count = 2000;
    gp_m6.substitution_rate = 0.01;
    auto m6_pairs = generator_t(gp_m6).generate_fragment_pairs(reference, 0, gp_m6.fragment_count);
    std::uint64_t total_bases = 0;
    std::uint64_t mismatches = 0;
    for (const auto& pair : m6_pairs) {
        const auto expected = ranks_for_window(*reference, pair.truth_target_start, pair.truth_target_end);
        const auto query = ranks_for_window(pair.query, 0, pair.query.bp_size());
        ASSERT(query.size() == expected.size(), "M6 query/window size mismatch");
        for (std::size_t i = 0; i < query.size(); ++i) {
            mismatches += query[i] != expected[i] ? 1u : 0u;
        }
        total_bases += query.size();
    }
    const double mismatch_rate = static_cast<double>(mismatches) / static_cast<double>(total_bases);
    const double mismatch_sigma =
        std::sqrt(gp_m6.substitution_rate * (1.0 - gp_m6.substitution_rate) / static_cast<double>(total_bases));
    ASSERT(std::abs(mismatch_rate - gp_m6.substitution_rate) <= 5.0 * mismatch_sigma,
           fmt::format("M6 mismatch rate {} outside 5 sigma band", mismatch_rate));
    manual_pass("M6", fmt::format("mismatch_rate={:.8f} total_bases={}", mismatch_rate, total_bases));

    gen::generator_params gp_m7 = gp_m5;
    gp_m7.fragment_count = 2000;
    gp_m7.insertion_rate = 0.01;
    gp_m7.deletion_rate = 0.01;
    gp_m7.insertion_length_mean = 2.0;
    gp_m7.deletion_length_mean = 2.0;
    std::uint64_t indel_positions = 0;
    const auto indel_counts =
        replay_indel_counts<traits>(*reference, gp_m7, gp_m7.fragment_count, indel_positions);
    const double event_opportunities = static_cast<double>(indel_counts.visited_positions);
    const double expected_events = event_opportunities * 0.01;
    const double event_sigma = std::sqrt(event_opportunities * 0.01 * 0.99);
    ASSERT(std::abs(static_cast<double>(indel_counts.insertions) - expected_events) <= 5.0 * event_sigma,
           fmt::format("M7 insertion events {} outside 5 sigma band", indel_counts.insertions));
    ASSERT(std::abs(static_cast<double>(indel_counts.deletions) - expected_events) <= 5.0 * event_sigma,
           fmt::format("M7 deletion events {} outside 5 sigma band", indel_counts.deletions));
    manual_pass("M7", fmt::format("ins_events={} del_events={} visited_positions={} original_positions={}",
                                  indel_counts.insertions, indel_counts.deletions,
                                  indel_counts.visited_positions, indel_positions));

    gen::generator_params gp_m8 = gp_m5;
    gp_m8.fragment_count = 4000;
    gp_m8.reverse_complement_rate = 0.5;
    auto m8_pairs = generator_t(gp_m8).generate_fragment_pairs(reference, 0, gp_m8.fragment_count);
    std::size_t reverse_count = 0;
    std::size_t checked_forward = 0;
    std::size_t checked_reverse = 0;
    for (const auto& pair : m8_pairs) {
        reverse_count += pair.is_reverse ? 1u : 0u;
        if ((pair.is_reverse && checked_reverse < 10) ||
            (!pair.is_reverse && checked_forward < 10)) {
            const auto expected =
                ranks_for_window(*reference, pair.truth_target_start, pair.truth_target_end, pair.is_reverse);
            const auto query = ranks_for_window(pair.query, 0, pair.query.bp_size());
            ASSERT(query == expected, "M8 strand-specific query/window ranks differ");
            if (pair.is_reverse) {
                ++checked_reverse;
            } else {
                ++checked_forward;
            }
        }
    }
    const double reverse_fraction = static_cast<double>(reverse_count) / static_cast<double>(m8_pairs.size());
    const double reverse_sigma = std::sqrt(0.25 / static_cast<double>(m8_pairs.size()));
    ASSERT(std::abs(reverse_fraction - 0.5) <= 5.0 * reverse_sigma,
           fmt::format("M8 reverse fraction {} outside 5 sigma band", reverse_fraction));
    ASSERT(checked_forward == 10 && checked_reverse == 10,
           fmt::format("M8 checked forward={} reverse={}", checked_forward, checked_reverse));
    manual_pass("M8", fmt::format("reverse_fraction={:.6f}", reverse_fraction));

    {
        auto reference2 = generator_t(gp).generate_reference();
        ASSERT(reference->_data == reference2->_data && reference->padding() == reference2->padding(),
               "M9 same-seed references differ");
    }
    {
        gen::generator_params gp_gpu = gp;
        gp_gpu.use_vulkan = true;
        if (!hardware_vulkan_ready()) {
            REQUIRE_THROWS_AS(generator_t(gp_gpu).generate_sequences(1), gv::VulkanRuntimeError);
            manual_pass("M9", "selected GPU generation rejected without a hardware Vulkan device");
        } else {
            const auto before = gv::Runtime::instance().generate_units_dispatch_count();
            auto gpu_or_cpu = generator_t(gp_gpu).generate_sequences(1).front();
            const auto after = gv::Runtime::instance().generate_units_dispatch_count();
            ASSERT(after > before, "M9 hardware Vulkan device but dispatch counter did not increase");
            auto cpu = generator_t(gp_gpu).debug_generate_sequences_cpu_only(1).front();
            ASSERT(gpu_or_cpu._data == cpu._data && gpu_or_cpu.padding() == cpu.padding(),
                   "M9 GPU/CPU references differ");
            manual_pass("M9", fmt::format("dispatch_delta={} gpu_hardware={}", after - before,
                                          hardware_vulkan_ready()));
        }
    }

    gen::generator_params gp_m10{};
    gp_m10.reference_length = 5'000'000'000ull;
    gp_m10.fragment_count = 200;
    gp_m10.fragment_length_mean = 5000;
    gp_m10.fragment_length_stddev = 100.0;
    gp_m10.substitution_rate = 0.0;
    gp_m10.insertion_rate = 0.0;
    gp_m10.deletion_rate = 0.0;
    gp_m10.reverse_complement_rate = 0.0;
    gp_m10.seed = 0xB10Cull;
    using generator10_t = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>;
    auto reference10 = generator10_t(gp_m10).generate_reference();
    auto m10_pairs = generator10_t(gp_m10).generate_fragment_pairs(reference10, 0, gp_m10.fragment_count);
    bool saw_high_start = false;
    for (const auto& pair : m10_pairs) {
        ASSERT(pair.truth_target_start < pair.truth_target_end &&
               pair.truth_target_end <= gp_m10.reference_length,
               "M10 truth interval out of range");
        saw_high_start = saw_high_start || pair.truth_target_start >= 4'294'967'296ull;
    }
    ASSERT(saw_high_start, "M10 no fragment start reached the >32-bit coordinate range");
    manual_pass("M10", fmt::format("pairs={} saw_start_ge_2p32={}", m10_pairs.size(), saw_high_start));
}

// GPU-vs-CPU reference-generation throughput (DOC0335 §15 sign-off measurement). Hidden by the
// leading [.] so it is opt-in; run it explicitly. dna4/u64 is the GPU-eligible benchmark case.
// For every size it asserts GPU bytes == CPU bytes and (on a usable device) that a real dispatch
// occurred, then reports min-of-3 wall-clock for each path.
TEST_CASE("generator: GPU vs CPU generation throughput", "[.][generator][doc0335][gpubench]") {
    using gen_t = gen::alphabet_sequence_generator<seq::dna4, std::uint64_t>;
    auto& runtime = gv::Runtime::instance();
    const bool usable = hardware_vulkan_ready();
    std::cout << fmt::format("[gpubench] gpu_hardware={} alphabet=dna4 storage=u64 iters=min-of-3\n",
                             usable);
    if (!usable) {
        SUCCEED("No hardware Vulkan device detected; GPU generation bench skipped");
        return;
    }

    // Warm up: builds the generate_units pipeline + pages memory, so timings exclude one-time setup.
    {
        gen::generator_params warm{};
        warm.reference_length = 1'000'000;
        warm.fragment_count = 0;
        warm.seed = 1;
        warm.use_vulkan = true;  // explicit GPU selection (DOC0338 use_vulkan default is false)
        const auto warm_seq = gen_t(warm).generate_sequences(1);
        ASSERT(!warm_seq.empty(), "gpubench warm-up produced no sequence");
    }

    constexpr int kIters = 3;
    for (const std::uint64_t n : {1'000'000ull, 10'000'000ull, 100'000'000ull,
                                  500'000'000ull, 1'000'000'000ull, 2'000'000'000ull}) {
        gen::generator_params gp{};
        gp.reference_length = n;
        gp.fragment_count = 0;
        gp.seed = 0xBEEFull;
        gp.use_vulkan = true;  // explicit GPU selection (DOC0338 use_vulkan default is false)

        // Correctness gate: a real dispatch must occur and GPU bytes must equal CPU bytes.
        const auto before = runtime.generate_units_dispatch_count();
        const auto gpu_seq = gen_t(gp).generate_sequences(1);
        const auto after = runtime.generate_units_dispatch_count();
        {
            const auto cpu_seq = gen_t(gp).debug_generate_sequences_cpu_only(1);
            ASSERT(gpu_seq.front()._data == cpu_seq.front()._data &&
                       gpu_seq.front().padding() == cpu_seq.front().padding(),
                   fmt::format("gpubench GPU != CPU bytes at n={}", n));
        }
        ASSERT(after > before, fmt::format("gpubench no GPU dispatch at n={}", n));

        double gpu_ms = std::numeric_limits<double>::max();
        for (int it = 0; it < kIters; ++it) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto seq = gen_t(gp).generate_sequences(1);
            const auto t1 = std::chrono::steady_clock::now();
            gpu_ms = std::min(gpu_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
            ASSERT(!seq.empty(), "gpubench gpu path empty");
        }
        double cpu_ms = std::numeric_limits<double>::max();
        for (int it = 0; it < kIters; ++it) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto seq = gen_t(gp).debug_generate_sequences_cpu_only(1);
            const auto t1 = std::chrono::steady_clock::now();
            cpu_ms = std::min(cpu_ms, std::chrono::duration<double, std::milli>(t1 - t0).count());
            ASSERT(!seq.empty(), "gpubench cpu path empty");
        }
        const double gpu_gbps = (static_cast<double>(n) / 1e9) / (gpu_ms / 1e3);
        const double cpu_gbps = (static_cast<double>(n) / 1e9) / (cpu_ms / 1e3);
        std::cout << fmt::format(
            "[gpubench] n={:>13} bp  GPU={:>9.3f} ms ({:>5.2f} Gbp/s)  "
            "CPU={:>9.3f} ms ({:>5.2f} Gbp/s)  speedup={:>5.2f}x  dispatch_delta={}\n",
            n, gpu_ms, gpu_gbps, cpu_ms, cpu_gbps, cpu_ms / gpu_ms, after - before);
    }
}
