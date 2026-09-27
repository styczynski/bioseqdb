/**
 * @file minimizer_hashmap_test.cpp
 * @brief Checks SIMD minimizer collection and in-memory hash indexes.
 *
 * Tests call minimizer collectors in the selected Highway SIMD namespace.
 */

#include <hwy/highway.h>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.common.checked_arithmetic;
import bioseq.common.context;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.chaining.core;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.sequences.kmers.minimap2_sketch_baseline;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.pipeline.minimap_port;
import bioseq.gpu.vulkan.runtime;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.common.hashing;

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;

namespace {

/// Look up each distinct query hash and sort its positions for comparison.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
lookup_map(const bioseq::indexing::minimizer_hashmap_fragment_index& idx,
           const std::vector<std::uint64_t>& kmers) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : kmers) {
        if (out.count(kmer))
            continue;
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> v(span.begin(), span.end());
        std::sort(v.begin(), v.end());
        out.emplace(kmer, std::move(v));
    }
    return out;
}
} // namespace

TEST_CASE("exact_simd indexing bridge is linked, exact, and instrumented",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    auto data = make_dna4(4097, 0xB81D6EULL);
    range_t range(data);
    auto range_owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto range_view = bioseq::storage::make_lazy_sequence_view(range_owner);
    sketch_collector exact_collector;
#if HWY_TARGET == HWY_SCALAR
    REQUIRE_THROWS_AS((void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
                          exact_collector,
                          range_view,
                          fixed_alphabet_code<::seq::dna4>(),
                          0,
                          range_view.bp_size(),
                          10,
                          15,
                          0,
                          fixed_minimizer_kernels(),
                          "bridge contract"),
                      bioseq::indexing::exact_simd_index_runtime_error);
#else
    const auto stats = bioseq::indexing::collect_exact_simd_index_range_or_throw(
        exact_collector,
        range_view,
        fixed_alphabet_code<::seq::dna4>(),
        0,
        range_view.bp_size(),
        10,
        15,
        0,
        fixed_minimizer_kernels(),
        "bridge contract");
    const auto scalar = whole_sequence_minimizers(range, 10, 15);
    const auto& exact = exact_collector.extract_sketches_data();
    REQUIRE(exact.size() == scalar.size());
    for (std::size_t index = 0; index < exact.size(); ++index) {
        REQUIRE(exact[index].raw_value() == scalar[index].raw_value());
    }
    REQUIRE(stats.vector_batches > 0);
    REQUIRE(stats.packed_decode_batches > 0);
    REQUIRE(stats.packed_decode_lanes == stats.bp_consumed);
    REQUIRE(stats.scalar_backend_branches == 0);
    REQUIRE(stats.scalar_tail_symbols == 0);
    REQUIRE(stats.scalar_hash_symbols == 0);
    REQUIRE(stats.scalar_window_transitions == 0);
    REQUIRE(stats.scalar_event_records == 0);
    REQUIRE(stats.scalar_sort_records == 0);
    REQUIRE(stats.scalar_dedup_records == 0);
#endif
}

TEST_CASE("exact_simd indexing bridge preserves DNA5 nt4 reset behavior and stats",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    constexpr std::size_t w = 10;
    constexpr std::size_t k = 15;
    std::vector<seq::dna5> data(4097);
    constexpr std::array<std::uint8_t, 4> acgt_ranks{0, 1, 2, 4};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(acgt_ranks[(index * 7u + index / 11u) % 4u]);
    }
    dna5_range_t no_n_range(data);
    auto no_n_owner = bioseq::storage::make_resident_lazy_sequence(no_n_range);
    const auto no_n_view = bioseq::storage::make_lazy_sequence_view(no_n_owner);
    sketch_collector no_n_exact_collector;
    sketch_collector mismatched_alphabet_collector;
    REQUIRE_THROWS_AS((void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
                          mismatched_alphabet_collector,
                          no_n_view,
                          fixed_alphabet_code<::seq::dna4>(),
                          0,
                          no_n_view.bp_size(),
                          w,
                          k,
                          0,
                          fixed_minimizer_kernels(),
                          "DNA5 bridge mismatched alphabet evidence"),
                      bioseq::indexing::exact_simd_index_runtime_error);
#if HWY_TARGET == HWY_SCALAR
    REQUIRE_THROWS_AS((void)bioseq::indexing::collect_exact_simd_index_range_or_throw(
                          no_n_exact_collector,
                          no_n_view,
                          fixed_alphabet_code<::seq::dna5>(),
                          0,
                          no_n_view.bp_size(),
                          w,
                          k,
                          0,
                          fixed_minimizer_kernels(),
                          "DNA5 bridge contract"),
                      bioseq::indexing::exact_simd_index_runtime_error);
#else
    const auto no_n_stats = bioseq::indexing::collect_exact_simd_index_range_or_throw(
        no_n_exact_collector,
        no_n_view,
        fixed_alphabet_code<::seq::dna5>(),
        0,
        no_n_view.bp_size(),
        w,
        k,
        0,
        fixed_minimizer_kernels(),
        "DNA5 bridge contract");
    const auto no_n_scalar = whole_sequence_minimizers(no_n_range, w, k);
    const auto& no_n_exact = no_n_exact_collector.extract_sketches_data();
    REQUIRE(no_n_exact.size() == no_n_scalar.size());
    for (std::size_t index = 0; index < no_n_exact.size(); ++index) {
        REQUIRE(no_n_exact[index].raw_value() == no_n_scalar[index].raw_value());
    }
    REQUIRE(no_n_stats.ambiguous_symbols == 0);
    REQUIRE(no_n_stats.ambiguous_reset_batches == 0);
    REQUIRE(no_n_stats.vector_batches > 0);
    REQUIRE(no_n_stats.packed_decode_lanes == no_n_stats.bp_consumed);

    constexpr std::array<std::size_t, 5> n_positions{97, 1023, 2048, 3071, 4095};
    for (const std::size_t position : n_positions) {
        data[position].assign_rank(3); // N in the SeqAn DNA5 alphabet.
    }
    dna5_range_t with_n_range(data);
    auto with_n_owner = bioseq::storage::make_resident_lazy_sequence(with_n_range);
    const auto with_n_view = bioseq::storage::make_lazy_sequence_view(with_n_owner);
    sketch_collector with_n_collector;
    const auto with_n_stats = bioseq::indexing::collect_exact_simd_index_range_or_throw(
        with_n_collector,
        with_n_view,
        fixed_alphabet_code<::seq::dna5>(),
        0,
        with_n_view.bp_size(),
        w,
        k,
        0,
        fixed_minimizer_kernels(),
        "DNA5 bridge N-reset contract");
    REQUIRE(with_n_stats.ambiguous_symbols == n_positions.size());
    REQUIRE(with_n_stats.ambiguous_reset_batches > 0);
    REQUIRE(with_n_stats.scalar_backend_branches == 0);
    REQUIRE(with_n_stats.scalar_hash_symbols == 0);
    REQUIRE(with_n_stats.scalar_window_transitions == 0);
    REQUIRE(with_n_stats.scalar_event_records == 0);
    REQUIRE(with_n_stats.scalar_sort_records == 0);
    REQUIRE(with_n_stats.scalar_dedup_records == 0);
    const auto expected_events = baseline_dna5_events(data, w, k);
    const auto& exact_events = with_n_collector.extract_sketches_data();
    REQUIRE(exact_events.size() == expected_events.size());
    for (std::size_t index = 0; index < exact_events.size(); ++index) {
        REQUIRE(exact_events[index].kmer() == expected_events[index].hash);
        REQUIRE(exact_events[index].pos_strand_encoded() ==
                expected_events[index].encoded_position);
    }
#endif
}

TEST_CASE("minimizer_hashmap_fragment_index build(vector) + lookup",
          "[component:bioseq][layer:unit][feature:index]") {
    // Keep these test positions within the sketch record
    // position limit.
    std::vector<minimap_sketch_data> sketches{
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 5, /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 9, /*strand*/ 1},
        minimap_sketch_data{/*kmer*/ 7, /*pos*/ 2, /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 1, /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 42, /*pos*/ 13, /*strand*/ 1},
    };
    bioseq::indexing::minimizer_hashmap_fragment_index idx;
    idx.build(sketches, /*target_length*/ 1000);
    REQUIRE(idx.target_length() == 1000);

    auto enc = [](std::uint64_t pos, std::uint64_t strand) { return (pos << 1) | strand; };

    { // Hash 100 has three positions.
        auto span = idx.lookup(100);
        std::vector<std::uint64_t> got(span.begin(), span.end());
        std::sort(got.begin(), got.end());
        std::vector<std::uint64_t> want{enc(1, 0), enc(5, 0), enc(9, 1)};
        std::sort(want.begin(), want.end());
        REQUIRE(got == want);
    }
    { // Hash 7 has one position.
        auto span = idx.lookup(7);
        REQUIRE(span.size() == 1u);
        REQUIRE(span[0] == enc(2, 0));
    }
    { // Hash 42 has one position.
        auto span = idx.lookup(42);
        REQUIRE(span.size() == 1u);
        REQUIRE(span[0] == enc(13, 1));
    }
    REQUIRE(idx.lookup(/*absent*/ 999).empty());
}

TEST_CASE("transient minimizer index preserves the XXL 30-bit radix-key domain",
          "[component:bioseq][layer:unit][feature:index]") {
    using xxl_minimizer_type = minimizer_representation<33u, 15u>;
    static_assert(xxl_minimizer_type::layout_type::k_hash_bits == 30u);
    static_assert(sizeof(xxl_minimizer_type) == sizeof(std::uint64_t));

    constexpr std::uint64_t low_key = 0x0000'0021u;
    constexpr std::uint64_t high_key = (std::uint64_t{1} << 30u) - 2u;
    std::vector<xxl_minimizer_type> sketches{
        xxl_minimizer_type{/*kmer*/ high_key, /*pos*/ 499'999'991u, /*strand*/ 1u},
        xxl_minimizer_type{/*kmer*/ low_key, /*pos*/ 17u, /*strand*/ 0u},
        xxl_minimizer_type{/*kmer*/ high_key, /*pos*/ 123'456'789u, /*strand*/ 0u},
        xxl_minimizer_type{/*kmer*/ low_key, /*pos*/ 29u, /*strand*/ 1u},
    };

    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build(sketches, /*target_length*/ 500'000'000u);

    auto sorted_positions = [&index](std::uint64_t key) {
        const auto hits = index.lookup(key);
        std::vector<std::uint64_t> result{hits.begin(), hits.end()};
        std::ranges::sort(result);
        return result;
    };
    REQUIRE((sorted_positions(low_key) == std::vector<std::uint64_t>{17u << 1u, (29u << 1u) | 1u}));
    REQUIRE((sorted_positions(high_key) ==
             std::vector<std::uint64_t>{123'456'789u << 1u, (499'999'991u << 1u) | 1u}));
    REQUIRE(index.lookup((std::uint64_t{1} << 30u) - 3u).empty());
}

TEST_CASE("transient minimizer index preserves the 128-bit representation kmer domain",
          "[component:bioseq][layer:unit][feature:index]") {
    using wide_minimizer_type = minimizer_representation<28u, 30u>;
    static_assert(wide_minimizer_type::layout_type::k_hash_bits > 64u);
    static_assert(sizeof(wide_minimizer_type) == 2u * sizeof(std::uint64_t));
    static_assert(
        std::is_same_v<decltype(std::declval<const wide_minimizer_type&>().kmer()), std::uint64_t>);

    // Use hashes above 32 bits to catch narrowing during radix sorting.
    // This record exposes 56 hash bits beside its eight-bit span.
    constexpr std::uint64_t low_key = 0x0000'0001'2345'6789ULL;
    constexpr std::uint64_t high_key = 0x00FE'DCBA'9876'5432ULL;
    std::vector<wide_minimizer_type> sketches{
        wide_minimizer_type{/*kmer*/ high_key,
                            /*pos*/ 200'000'003u,
                            /*strand*/ 1u,
                            /*span*/ 30u},
        wide_minimizer_type{/*kmer*/ low_key,
                            /*pos*/ 19u,
                            /*strand*/ 0u,
                            /*span*/ 30u},
        wide_minimizer_type{/*kmer*/ high_key,
                            /*pos*/ 123'456'789u,
                            /*strand*/ 0u,
                            /*span*/ 30u},
    };

    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build(sketches, /*target_length*/ 250'000'000u);

    REQUIRE(index.lookup(low_key).size() == 1u);
    REQUIRE(index.lookup(low_key).front() == (19u << 1u));
    const auto high_hits = index.lookup(high_key);
    REQUIRE(high_hits.size() == 2u);
    std::vector<std::uint64_t> sorted_high{high_hits.begin(), high_hits.end()};
    std::ranges::sort(sorted_high);
    REQUIRE(
        (sorted_high == std::vector<std::uint64_t>{123'456'789u << 1u, (200'000'003u << 1u) | 1u}));
    REQUIRE(index.lookup(0x0000'0002'2345'6789ULL).empty());
}

TEST_CASE("transient minimizer index consumes source and releases storage",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr auto large_table_plan =
        bioseq::indexing::plan_minimizer_hashmap_table_allocation(93'308'896u);
    STATIC_REQUIRE(large_table_plan.valid);
    STATIC_REQUIRE(large_table_plan.group_count == 8'388'608u);
    STATIC_REQUIRE(large_table_plan.capacity == 125'829'119u);
    STATIC_REQUIRE(large_table_plan.allocation_bytes == 2'147'483'648u);

    std::vector<minimap_sketch_data> sketches{
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 5, /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 7, /*pos*/ 2, /*strand*/ 0},
        minimap_sketch_data{/*kmer*/ 100, /*pos*/ 9, /*strand*/ 1},
    };
    bioseq::indexing::minimizer_hashmap_fragment_index idx;
    idx.build_consuming(std::move(sketches), /*target_length*/ 1000);

    REQUIRE(sketches.empty());
    REQUIRE(sketches.capacity() == 0u);
    REQUIRE(idx.lookup(100).size() == 2u);
    REQUIRE(idx.lookup(7).size() == 1u);
    REQUIRE(idx.target_length() == 1000);
    // Check allocation counters while the index owns its table.
    REQUIRE(idx.table_allocation_bytes() != 0u);
    REQUIRE(idx.table_peak_allocation_bytes() == idx.table_allocation_bytes());

    idx.release_storage();
    REQUIRE(idx.table_empty());
    REQUIRE(idx.table_allocation_bytes() == 0u);
    REQUIRE(idx.table_peak_allocation_bytes() == 0u);
    REQUIRE(idx.positions.empty());
    REQUIRE(idx.positions.capacity() == 0u);
    REQUIRE(idx.target_length() == 1000);
}

TEST_CASE("one-shot transient minimizer index stays inside its retained permit",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::size_t count = 4'096u;
    std::vector<minimap_sketch_data> sketches;
    sketches.reserve(count);
    for (std::size_t i = 0u; i < count; ++i) {
        sketches.emplace_back(static_cast<std::uint64_t>(i + 1u),
                              static_cast<std::uint64_t>(i),
                              static_cast<std::uint64_t>(i & 1u));
    }

    const auto table_plan = bioseq::indexing::plan_minimizer_hashmap_table_allocation(count);
    REQUIRE(table_plan.valid);
    const std::uint64_t position_payload = count * sizeof(std::uint64_t);
    const std::uint64_t position_headroom =
        bioseq::gpu::vulkan::vulkan_exact_host_allocation_admission_bytes(position_payload);
    const std::uint64_t table_headroom =
        bioseq::gpu::vulkan::vulkan_exact_host_allocation_admission_bytes(
            table_plan.allocation_bytes);
    const std::uint64_t build_permit_bytes =
        bioseq::common::saturating_add(position_headroom, table_headroom);

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& controller;
        std::uint64_t value = 0u;
        ~restore_cap() {
            controller.set_index_memory_cap_bytes(value);
        }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(
        bioseq::common::saturating_add(baseline, build_permit_bytes + (std::uint64_t{1} << 20u)));
    auto reserved = admission.try_reserve(build_permit_bytes);
    REQUIRE(reserved.has_value());

    // Declare the index after the memory permit so its storage is freed
    // before the reservation on stack unwinding.
    bioseq::parallelism::memory_permit permit = std::move(*reserved);
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build_consuming_full_table(std::move(sketches), /*target_length=*/count);
    REQUIRE(sketches.capacity() == 0u);
    REQUIRE(index.table_allocation_bytes() == table_plan.allocation_bytes);
    REQUIRE(index.table_peak_allocation_bytes() == table_plan.allocation_bytes);
    const std::uint64_t retained_position_bytes =
        index.positions.capacity() * sizeof(std::uint64_t);
    REQUIRE(retained_position_bytes <= position_headroom);
    const std::uint64_t retained_bytes = retained_position_bytes + index.table_allocation_bytes();
    permit.shrink_to(retained_bytes);
    REQUIRE(admission.reserved_bytes() == baseline + retained_bytes);

    index.release_storage();
    REQUIRE(admission.reserved_bytes() == baseline + retained_bytes);
    permit.release();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("empty one-shot minimizer index has an exact zero-byte plan",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr auto empty_plan = bioseq::indexing::plan_minimizer_hashmap_table_allocation(0u);
    STATIC_REQUIRE(empty_plan.valid);
    STATIC_REQUIRE(empty_plan.distinct_upper_bound == 0u);
    STATIC_REQUIRE(empty_plan.requested_slots == 0u);
    STATIC_REQUIRE(empty_plan.group_count == 0u);
    STATIC_REQUIRE(empty_plan.capacity == 0u);
    STATIC_REQUIRE(empty_plan.allocation_bytes == 0u);

    std::vector<minimap_sketch_data> empty;
    empty.reserve(64u);
    bioseq::indexing::minimizer_hashmap_fragment_index index;
    index.build_consuming_full_table(std::move(empty), /*target_length=*/1'000u);

    REQUIRE(empty.capacity() == 0u);
    REQUIRE(index.positions.empty());
    REQUIRE(index.positions.capacity() == 0u);
    REQUIRE(index.table_empty());
    REQUIRE(index.table_allocation_bytes() == 0u);
    REQUIRE(index.table_peak_allocation_bytes() == 0u);
    REQUIRE(index.target_length() == 1'000u);
}

TEST_CASE("minimizer_hashmap_fragment_index serialize/deserialize round-trip",
          "[component:bioseq][layer:unit][feature:index]") {
    auto data = make_dna4(2000, 0xC0FFEE);
    range_t range(data);
    auto sketches = whole_sequence_minimizers(range, 10, 15);

    bioseq::indexing::minimizer_hashmap_fragment_index idx;
    idx.build(sketches, range.bp_size());

    std::vector<char> buf(idx.serialize_binary_size());
    idx.serialize_to_binary(buf.data());
    auto round = bioseq::indexing::minimizer_hashmap_fragment_index::deserialize_from_binary(
        buf.data(), buf.size());

    REQUIRE(round.target_length() == idx.target_length());
    REQUIRE(round.positions.size() == idx.positions.size());
    REQUIRE(round.table_size() == idx.table_size());

    // Serialization must preserve the positions stored for every query hash.
    std::vector<std::uint64_t> kmers;
    for (const auto& m : sketches)
        kmers.push_back(m.kmer());
    REQUIRE(lookup_map(round, kmers) == lookup_map(idx, kmers));
}

TEST_CASE("build_from_pairs equals build(vector) over equivalent pairs",
          "[component:bioseq][layer:unit][feature:index]") {
    auto data = make_dna4(3000, 0x5EED);
    range_t range(data);
    auto sketches = whole_sequence_minimizers(range, 10, 15);

    bioseq::indexing::minimizer_hashmap_fragment_index via_vector;
    via_vector.build(sketches, range.bp_size());

    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    pairs.reserve(sketches.size());
    for (const auto& m : sketches)
        pairs.emplace_back(m.kmer(), m.pos_strand_encoded());

    bioseq::indexing::minimizer_hashmap_fragment_index via_pairs;
    via_pairs.build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>>(pairs),
                               range.bp_size());

    REQUIRE(via_pairs.target_length() == via_vector.target_length());
    REQUIRE(via_pairs.positions.size() == via_vector.positions.size());

    std::vector<std::uint64_t> kmers;
    for (const auto& m : sketches)
        kmers.push_back(m.kmer());
    REQUIRE(lookup_map(via_pairs, kmers) == lookup_map(via_vector, kmers));
}

TEST_CASE("assemble_segment over fragmented storage equals whole-sequence build",
          "[component:bioseq][layer:unit][feature:index]") {
    // The 40,000-base sequence crosses two 32,768-base fragments.
    constexpr std::size_t fragment_units = 1024;
    constexpr std::size_t frag_bp = fragment_units * 32;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;

    auto data = make_dna4(40000, 0xABC123);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);
    REQUIRE(nfrag >= 2u);

    fake_fragment_storage<> storage;
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>::build(storage,
                                                                                    lazy_view);

    bioseq::indexing::minimizer_assembly_scratch scratch;
    auto assembled =
        bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>::assemble_segment(
            storage,
            0,
            nfrag,
            static_cast<bioseq::storage::sequence_size_t>(frag_bp),
            total_bp,
            0,
            total_bp,
            scratch);

    // Build a reference index from a sketch of the whole sequence.
    auto whole = whole_sequence_minimizers(range, 10, 15);
    bioseq::indexing::minimizer_hashmap_fragment_index baseline;
    baseline.build(whole, total_bp);

    REQUIRE(assembled.target_length() == total_bp);

    // Compare positions for every hash in the whole-sequence sketch.
    std::vector<std::uint64_t> kmers;
    for (const auto& minimizer : whole)
        kmers.push_back(minimizer.kmer());
    REQUIRE(lookup_map(assembled, kmers) == lookup_map(baseline, kmers));
}

TEST_CASE("exact_simd fragmented-index build preserves scalar segment lookups",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    constexpr std::size_t fragment_units = 2;
    // Stored index fragments cover 32,768 bases. The scanner reads much
    // smaller pieces here to exercise loading across read boundaries.
    constexpr std::size_t frag_bp = 32'768u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = bioseq::indexing::minimizers_per_fragment_index<fake_fragment_storage<>>;

    auto data = make_dna4(4099, 0xE2AC751DULL);
    range_t range(data);
    const auto total_bp = range.bp_size();
    const std::size_t nfrag = static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);

    fake_fragment_storage<> scalar_storage;
    fake_fragment_storage<> exact_storage;
    lazy_sequence<dna4_u64, chunked> scalar_lazy{chunked::from_range(range)};
    lazy_sequence<dna4_u64, chunked> exact_lazy{chunked::from_range(range)};
    const auto scalar_view = bioseq::storage::make_lazy_sequence_view(scalar_lazy);
    const auto exact_view = bioseq::storage::make_lazy_sequence_view(exact_lazy);
    bioseq::indexing::minimizer_fragment_generation_execution_receipt scalar_evidence;
    bioseq::indexing::minimizer_fragment_generation_execution_receipt exact_evidence;
    index_type::build(
        scalar_storage, scalar_view, minimizer_sketch_backend::scalar, nullptr, &scalar_evidence);
    index_type::build(exact_storage,
                      exact_view,
                      minimizer_sketch_backend::exact_simd,
                      fixed_minimizer_kernels(),
                      &exact_evidence,
                      fixed_alphabet_code<::seq::dna4>());
    REQUIRE(scalar_evidence.scalar_core_runs > 0u);
    REQUIRE(scalar_evidence.exact_simd_core_runs == 0u);
    REQUIRE(scalar_evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(scalar_evidence.vulkan_core_runs == 0u);
    REQUIRE(exact_evidence.scalar_core_runs == 0u);
    REQUIRE(exact_evidence.exact_simd_core_runs > 0u);
    REQUIRE(exact_evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(exact_evidence.vulkan_core_runs == 0u);
    REQUIRE(exact_evidence.exact_simd_stats_present);
    REQUIRE(exact_evidence.exact_simd_stats.is_internally_consistent(true));

    bioseq::indexing::minimizer_assembly_scratch scalar_scratch;
    bioseq::indexing::minimizer_assembly_scratch exact_scratch;
    auto scalar =
        index_type::assemble_segment(scalar_storage,
                                     0,
                                     nfrag,
                                     static_cast<bioseq::storage::sequence_size_t>(frag_bp),
                                     total_bp,
                                     0,
                                     total_bp,
                                     scalar_scratch);
    auto exact =
        index_type::assemble_segment(exact_storage,
                                     0,
                                     nfrag,
                                     static_cast<bioseq::storage::sequence_size_t>(frag_bp),
                                     total_bp,
                                     0,
                                     total_bp,
                                     exact_scratch);

    auto whole = whole_sequence_minimizers(range, 10, 15);
    std::vector<std::uint64_t> kmers;
    kmers.reserve(whole.size());
    for (const auto& minimizer : whole)
        kmers.push_back(minimizer.kmer());
    REQUIRE(lookup_map(exact, kmers) == lookup_map(scalar, kmers));
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
