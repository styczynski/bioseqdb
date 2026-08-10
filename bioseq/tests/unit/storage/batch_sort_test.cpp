// bioseq/tests/unit/storage/batch_sort_test.cpp
//
// Correctness oracle for the GPU sequence sort kernels.
//
// Drives the REAL seam (`sequence_batch_sorter::sort`) over randomized dna4
// (storage widths
// u8/u16/u32/u64) and dna5 (u64) batches with representative length shapes (empty,
// sub-unit, multi-unit, shared-prefix, prefix-of, equal, all-`A` tail), and
// asserts the returned permutation EQUALS `std::sort` under the exact scalar
// comparator `compare_sequences_scalar` (ascending sequence, index-ascending
// tiebreak), for BOTH directions.
//
//   * vulkan_enabled = true  exercises the GPU façade when a hardware device is
//     present (and transparently the CPU fallback when it is not — same assertion).
//   * vulkan_enabled = false exercises the CPU fallback explicitly.
//
// The two runs must produce the identical permutation, including on
// duplicate sequences). When a hardware Vulkan device is present we additionally
// assert the device key/tiebreak dispatch counter advanced, confirming the GPU
// path actually executed (not a silent decline).

#include <tests/support/common.hpp>

#include <cstdint>
#include <deque>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;   // seq::dna4 / seq::dna5
import bioseq.sequences;
import bioseq.sequences.simd;
import bioseq.storage.lazy_sequence;
import bioseq.storage.scanner;
import bioseq.storage.batch_sort;
import bioseq.gpu.vulkan.runtime;

namespace {

namespace vk = bioseq::gpu::vulkan;
using bioseq::storage::sequence_sort_options;
using bioseq::storage::lazy_sequence;
using bioseq::storage::make_resident_lazy_sequence;
using bioseq::storage::resident_sequence_scanner;
using bioseq::storage::sequence_batch_sorter;

// A batch of sequences over one record type, keeping the owning symbol vectors and ranges
// alive (the lazy wrappers are pointer-thin views).
template <class p_record>
struct owned_batch {
    using alphabet_t = typename p_record::alphabet_type;
    using unit_t = typename compact_sequence_range<p_record>::traits::storage_unit;
    using lazy_t = lazy_sequence<p_record, resident_sequence_scanner<unit_t>>;
    using view_t = bioseq::storage::lazy_sequence_view<alphabet_t>;

    std::deque<std::vector<alphabet_t>> symbols;   // stable addresses
    std::deque<compact_sequence_range<p_record>> ranges;  // stable addresses
    std::vector<lazy_t> lazies;

    void push(const std::vector<std::uint8_t>& ranks) {
        std::vector<alphabet_t> v(ranks.size());
        for (std::size_t i = 0; i < ranks.size(); ++i) {
            alphabet_t a;
            a.assign_rank(static_cast<decltype(a.to_rank())>(ranks[i]));
            v[i] = a;
        }
        symbols.push_back(std::move(v));
        ranges.emplace_back(symbols.back());
        lazies.push_back(make_resident_lazy_sequence(ranges.back()));
    }

    [[nodiscard]] std::vector<view_t> views() const {
        std::vector<view_t> result;
        result.reserve(lazies.size());
        for (const lazy_t& lazy : lazies) {
            result.push_back(bioseq::storage::make_lazy_sequence_view(lazy));
        }
        return result;
    }
    [[nodiscard]] std::size_t size() const { return lazies.size(); }
    [[nodiscard]] const lazy_t& operator[](std::size_t i) const { return lazies[i]; }
};

// Scalar oracle order = std::sort with the exact comparator + direction-applied
// index tiebreak (mirrors both the GPU and CPU paths).
template <class p_batch>
[[nodiscard]] std::vector<std::size_t> oracle_order(p_batch& batch, bool descending) {
    const std::size_t n = batch.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&batch, descending](std::size_t i, std::size_t j) {
        const std::strong_ordering c = bioseq::simd::active_target::compare_sequences_scalar(
            batch[i].underlying(), batch[j].underlying());
        if (c != 0) return descending ? c > 0 : c < 0;
        return descending ? i > j : i < j;
    });
    return order;
}

// Smallest alphabet rank, used for uniform tails.
constexpr std::uint8_t rank_a = 0;

// Build a varied batch spanning the required length shapes for `alphabet_size`-ary
// data. `lpu` = letters_per_unit (drives sub-/multi-unit boundary choices).
std::vector<std::vector<std::uint8_t>> make_shapes(std::mt19937_64& rng,
                                                   std::size_t count,
                                                   std::uint32_t alphabet_size,
                                                   std::size_t lpu) {
    std::vector<std::vector<std::uint8_t>> out;
    auto rank = [&]() { return static_cast<std::uint8_t>(rng() % alphabet_size); };
    auto rand_seq = [&](std::size_t len) {
        std::vector<std::uint8_t> v(len);
        for (auto& x : v) x = rank();
        return v;
    };

    // A shared prefix used by several rows (forces equal-key runs / deep tie-break).
    const std::size_t prefix_len = lpu * 3 + 5;   // spans >1 unit + partial
    std::vector<std::uint8_t> shared_prefix = rand_seq(prefix_len);

    if (count == 0) return out;

    auto add = [&](std::vector<std::uint8_t> v) {
        if (out.size() < count) out.push_back(std::move(v));
    };

    // Deterministic shape seeds first, then random filler to reach `count`.
    add({});                                                       // empty
    add(rand_seq(1));                                              // single base
    add(rand_seq(lpu - 1));                                        // sub-unit
    add(rand_seq(lpu));                                            // exactly one unit
    add(rand_seq(lpu + 1));                                        // just over a unit
    add(rand_seq(lpu * 4 + 3));                                    // multi-unit + partial
    {
        auto p = shared_prefix;                                    // shared prefix, extended
        auto tail = rand_seq(7); p.insert(p.end(), tail.begin(), tail.end());
        add(p);
    }
    {
        auto p = shared_prefix;                                    // SAME shared prefix, diff tail
        auto tail = rand_seq(9); p.insert(p.end(), tail.begin(), tail.end());
        add(p);
    }
    add(shared_prefix);                                            // prefix-of the two above
    {
        auto p = shared_prefix; p.push_back(rank_a);  // prefix + uniform tail
        for (std::size_t i = 0; i < lpu * 2; ++i) p.push_back(rank_a);
        add(p);
    }
    if (out.size() >= 1) add(out[std::min<std::size_t>(6, out.size() - 1)]);  // exact duplicate
    add(std::vector<std::uint8_t>(lpu * 2, rank_a));              // smallest rank
    add(std::vector<std::uint8_t>(lpu * 2,
        static_cast<std::uint8_t>(alphabet_size - 1)));            // all-max (largest)

    while (out.size() < count) {
        const std::size_t len = rng() % (lpu * 5 + 1);
        // Half the random rows reuse the shared prefix to stress equal-key runs.
        if (rng() & 1u) {
            auto p = shared_prefix; auto t = rand_seq(len % (lpu + 1));
            p.insert(p.end(), t.begin(), t.end());
            add(std::move(p));
        } else {
            add(rand_seq(len));
        }
    }
    out.resize(count);
    return out;
}

template <class p_record>
void run_case(const char* label, std::size_t count, std::uint64_t seed,
              std::uint32_t alphabet_size) {
    using traits = typename compact_sequence_range<p_record>::traits;
    using alphabet_t = typename p_record::alphabet_type;
    using view_t = bioseq::storage::lazy_sequence_view<alphabet_t>;
    using sorter = sequence_batch_sorter<alphabet_t>;

    std::mt19937_64 rng(seed);
    auto shapes = make_shapes(rng, count, alphabet_size, traits::letters_per_unit);

    owned_batch<p_record> ob;
    for (auto& s : shapes) ob.push(s);
    const std::vector<view_t> views = ob.views();
    const std::span<const view_t> batch(views.data(), views.size());

    auto& rt = vk::runtime::instance();
    const bool gpu_present = vk::hardware_device_available(rt);

    for (bool descending : {false, true}) {
        DYNAMIC_SECTION(label << " n=" << count << (descending ? " DESC" : " ASC")) {
            const std::vector<std::size_t> expected = oracle_order(ob, descending);

            // ---- GPU-enabled path (device when present, else CPU fallback) ----
            sequence_sort_options gspec{}; gspec.descending = descending; gspec.vulkan_enabled = true;
            const std::uint64_t before = rt.seq_sort_dispatch_count();
            const auto got_gpu = sorter::sort(batch, gspec);
            const std::uint64_t after = rt.seq_sort_dispatch_count();
            REQUIRE(got_gpu == expected);
            if (gpu_present && count > 1) {
                // The façade declines only on oversize/admission; these batches
                // are tiny, so the device path MUST have executed.
                REQUIRE(after > before);
            }

            // ---- CPU-only path (vulkan_enabled = false) ----
            sequence_sort_options cspec{}; cspec.descending = descending; cspec.vulkan_enabled = false;
            const auto got_cpu = sorter::sort(batch, cspec);
            REQUIRE(got_cpu == expected);

            // GPU and CPU permutations are bit-identical.
            REQUIRE(got_gpu == got_cpu);
        }
    }
}

template <class p_record>
void run_oversized_equal_key_case(const char* label) {
    using traits = typename compact_sequence_range<p_record>::traits;
    using alphabet_t = typename p_record::alphabet_type;
    using view_t = bioseq::storage::lazy_sequence_view<alphabet_t>;
    using sorter = sequence_batch_sorter<alphabet_t>;

    constexpr std::size_t count = 300;  // > seq-sort LOCAL_SIZE_X (256)
    constexpr std::size_t suffix_digits = 6;  // 4^6 leaves plenty of unique suffixes
    const std::size_t prefix_len = traits::letters_per_unit * 3u;

    owned_batch<p_record> ob;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t value = (i * 73u) % count;  // deterministic unsorted permutation
        std::vector<std::uint8_t> ranks(prefix_len, rank_a);
        for (std::size_t digit = 0; digit < suffix_digits; ++digit) {
            const std::size_t shift = 2u * (suffix_digits - 1u - digit);
            ranks.push_back(static_cast<std::uint8_t>((value >> shift) & 0x3u));
        }
        ob.push(ranks);
    }
    const std::vector<view_t> views = ob.views();
    const std::span<const view_t> batch(views.data(), views.size());

    auto& rt = vk::runtime::instance();
    const bool gpu_present = vk::hardware_device_available(rt);
    for (bool descending : {false, true}) {
        DYNAMIC_SECTION(label << " oversized equal-key run" << (descending ? " DESC" : " ASC")) {
            const std::vector<std::size_t> expected = oracle_order(ob, descending);

            sequence_sort_options gspec{}; gspec.descending = descending; gspec.vulkan_enabled = true;
            const std::uint64_t sort_before = rt.seq_sort_dispatch_count();
            const std::uint64_t global_before = rt.seq_sort_global_compare_dispatch_count();
            const auto got_gpu = sorter::sort(batch, gspec);
            const std::uint64_t sort_after = rt.seq_sort_dispatch_count();
            const std::uint64_t global_after = rt.seq_sort_global_compare_dispatch_count();

            REQUIRE(got_gpu == expected);
            if (gpu_present) {
                REQUIRE(sort_after > sort_before);
                REQUIRE(global_after > global_before);
            }

            sequence_sort_options cspec{}; cspec.descending = descending; cspec.vulkan_enabled = false;
            const auto got_cpu = sorter::sort(batch, cspec);
            REQUIRE(got_cpu == expected);
            REQUIRE(got_gpu == got_cpu);
        }
    }
}

}  // namespace

TEST_CASE("dna4 GPU sort permutation equals compare_sequences_scalar",
          "[component:bioseq][layer:unit][feature:storage][resource:gpu]") {
    using dna4_u8  = sequence_custom_alphabet<seq::dna4, std::uint8_t>;
    using dna4_u16 = sequence_custom_alphabet<seq::dna4, std::uint16_t>;
    using dna4_u32 = sequence_custom_alphabet<seq::dna4, std::uint32_t>;
    using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;

    auto& rt = vk::runtime::instance();
    bioseq_log<logging_level::debug>("[gpu.sort.test] device usable={} ({})",
                    vk::hardware_device_available(rt),
                    vk::hardware_device_available(rt) ? "GPU path" : "CPU fallback path");

    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                          std::size_t{17}, std::size_t{256}, std::size_t{1000}}) {
        run_case<dna4_u8>("dna4/u8", n, 0xA11CE0000ull + n, 4);
        run_case<dna4_u16>("dna4/u16", n, 0xB0B0B0000ull + n, 4);
        run_case<dna4_u32>("dna4/u32", n, 0xC0FFEE000ull + n, 4);
        run_case<dna4_u64>("dna4/u64", n, 0xD00D00000ull + n, 4);
    }
    run_oversized_equal_key_case<dna4_u64>("dna4/u64");
}

TEST_CASE("dna5 generic GPU sort permutation equals compare_sequences_scalar",
          "[component:bioseq][layer:unit][feature:storage][resource:gpu]") {
    using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;

    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
                          std::size_t{17}, std::size_t{256}, std::size_t{1000}}) {
        run_case<dna5_u64>("dna5/u64", n, 0x5E5E5E000ull + n, 5);
    }
}
