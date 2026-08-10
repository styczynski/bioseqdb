// bioseq/tests/unit/indexing/minimizer_sequence_index_test.cpp
//
// minimizers_per_sequence: the merged, globally-deduped,
// kmer-range-partitioned per-sequence index. build() spills sorted runs through
// the storage (run_buffer_bytes_ forced TINY so >=2 runs spill), then
// assemble_for_query() loads only the buckets touched by the query kmers.
//
// Reaches collect_minimizers_view / minimap_sketch_data, so the body lives inside
// bioseq::simd::HWY_NAMESPACE (symbols reached UNQUALIFIED). bioseq::indexing::*
// types are named fully-qualified.

#include <hwy/highway.h>
#include <tests/support/common.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.highway.dispatch_contract;
import bioseq.highway.test_support;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.hashers.minimap_int;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.indexing.registry;
import bioseq.gpu.vulkan.runtime;   //   — hardware Vulkan skip guard
import bioseq.gpu.vulkan.abi;

import bioseq.tests.support.indexing_storage_fakes;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {

namespace {

using bioseq::storage::chunked_span_sequence_scanner;
using bioseq::storage::lazy_sequence;
using bioseq::indexing::test::fake_fragment_storage;

using dna4_u64 = sequence_custom_alphabet<seq::dna4, std::uint64_t>;
using range_t  = compact_sequence_range<dna4_u64>;

[[nodiscard]] const ::bioseq::minimizers::minimizer_kernels*
fixed_minimizer_kernels() {
    return &::bioseq::simd_dispatch::highway_test_kernel_table().minimizers;
}

template <sequence_alphabet p_alphabet>
[[nodiscard]] std::uint8_t fixed_alphabet_code() {
    const auto& table =
        ::bioseq::simd_dispatch::highway_test_kernel_table();
    const auto* identity = table.is_complete()
        ? table.template resolve_alphabet_identity<p_alphabet>() : nullptr;
    if (identity == nullptr
        || table.resolve_alphabet_identity(identity->alphabet_code)
            != identity) {
        throw std::logic_error{
            "generated test table has no requested alphabet identity"};
    }
    return identity->alphabet_code;
}

[[nodiscard]] std::vector<seq::dna4> make_dna4(std::size_t n, std::uint64_t seed = 0x9e3779b97f4a7c15ULL) {
    std::vector<seq::dna4> v(n);
    std::uint64_t s = seed;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        seq::dna4 c;
        c.assign_rank(static_cast<decltype(c.to_rank())>((s >> 33) & 0x3));
        v[i] = c;
    }
    return v;
}

template<class p_range>
[[nodiscard]] std::vector<minimap_sketch_data>
whole_sequence_minimizers(p_range& range, std::size_t w, std::size_t k) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, range, range.begin(), range.end(), w, k);
    return collector.extract_sketches_data();
}

// Generic deterministic range builder and whole-target query.
// minimizers (pinned w=10,k=15 to match both persisted index builds).
using test_fragment_storage = fake_fragment_storage<>;

template <class p_config>
[[nodiscard]] compact_sequence_range<p_config>
make_deterministic_test_range(std::size_t n) {
    // Draw ranks from the non-ambiguous letters only (ACGT, ranks
    // 0..3) so persisted-index coverage checks focus on Vulkan routing and
    // storage assembly rather than N-reset density. A letters_per_unit-multiple
    // length also avoids scalar/GPU disagreement over trailing padding slots.
    constexpr std::uint8_t k_non_ambiguous_rank_count =
        compact_sequence_range<p_config>::traits::alphabet_size < 4u
            ? static_cast<std::uint8_t>(compact_sequence_range<p_config>::traits::alphabet_size)
            : 4u;
    // Round down to a whole number of storage units so the trailing unit has
    // ZERO padding — otherwise the scalar unit-iterator sub-range over the final
    // fragment processes the last unit's padding slots (decoded as A) as
    // spurious minimizers that the GPU (which honours tail_padding) omits,
    // breaking exact parity for dna5 (letters_per_unit=21 ∤ 8Mi).
    constexpr std::size_t k_letters_per_unit =
        compact_sequence_range<p_config>::traits::letters_per_unit;
    n -= n % k_letters_per_unit;
    std::vector<typename p_config::alphabet_type> letters(n);
    std::uint64_t s = 0x9e3779b97f4a7c15ULL;
    for (std::size_t i = 0; i < n; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        typename p_config::alphabet_type c{};
        c.assign_rank(static_cast<std::uint8_t>((s >> 33) % k_non_ambiguous_rank_count));
        letters[i] = c;
    }
    return compact_sequence_range<p_config>(std::move(letters));
}

template <class p_config>
[[nodiscard]] std::vector<minimap_sketch_data>
make_deterministic_query_minimizers(compact_sequence_range<p_config>& range) {
    sketch_collector collector;
    collect_minimizers_view<minimap_int_hasher, minimap_sketch_data>(
        collector, range, range.begin(), range.end(), 10, 15);
    return collector.extract_sketches_data();
}

// (kmer -> sorted+unique encoded positions). Dedups to match the global dedup the
// minimizers_per_sequence merge performs.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
lookup_map(const bioseq::indexing::minimizer_hashmap_fragment_index& idx,
           const std::vector<std::uint64_t>& kmers) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : kmers) {
        if (out.count(kmer)) continue;
        auto span = idx.lookup(kmer);
        std::vector<std::uint64_t> v(span.begin(), span.end());
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        out.emplace(kmer, std::move(v));
    }
    return out;
}

// Whole-sequence baseline (kmer -> sorted+unique positions), restricted to a
// query kmer set, with the same max_occ global-count drop assemble_for_query uses.
[[nodiscard]] std::map<std::uint64_t, std::vector<std::uint64_t>>
baseline_for_query(const std::vector<minimap_sketch_data>& whole,
                   const std::vector<std::uint64_t>& query_kmers,
                   std::size_t max_occ) {
    std::map<std::uint64_t, std::vector<std::uint64_t>> per_kmer;
    for (const auto& m : whole) per_kmer[m.kmer()].push_back(m.pos_strand_encoded());
    for (auto& [kmer, v] : per_kmer) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    }
    std::map<std::uint64_t, std::vector<std::uint64_t>> out;
    for (std::uint64_t kmer : query_kmers) {
        if (out.count(kmer)) continue;
        auto it = per_kmer.find(kmer);
        std::vector<std::uint64_t> v = (it == per_kmer.end()) ? std::vector<std::uint64_t>{} : it->second;
        if (v.size() > max_occ) v.clear();   // over-frequent kmer dropped
        out.emplace(kmer, std::move(v));
    }
    return out;
}

void require_lookup_maps_equal(
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& actual,
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& expected) {
    INFO("actual_kmers=" << actual.size() << " expected_kmers=" << expected.size());
    REQUIRE(actual.size() == expected.size());
    auto actual_it = actual.begin();
    auto expected_it = expected.begin();
    for (; actual_it != actual.end() && expected_it != expected.end(); ++actual_it, ++expected_it) {
        INFO("actual_kmer=" << actual_it->first << " expected_kmer=" << expected_it->first
             << " actual_positions=" << actual_it->second.size()
             << " expected_positions=" << expected_it->second.size());
        REQUIRE(actual_it->first == expected_it->first);
        REQUIRE(actual_it->second == expected_it->second);
    }
}

void require_lookup_maps_cover_scalar(
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& actual,
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& expected) {
    INFO("actual_kmers=" << actual.size() << " expected_kmers=" << expected.size());
    REQUIRE(actual.size() == expected.size());
    auto actual_it = actual.begin();
    auto expected_it = expected.begin();
    for (; actual_it != actual.end() && expected_it != expected.end(); ++actual_it, ++expected_it) {
        INFO("actual_kmer=" << actual_it->first << " expected_kmer=" << expected_it->first
             << " actual_positions=" << actual_it->second.size()
             << " expected_positions=" << expected_it->second.size());
        REQUIRE(actual_it->first == expected_it->first);
        REQUIRE(std::includes(
            actual_it->second.begin(),
            actual_it->second.end(),
            expected_it->second.begin(),
            expected_it->second.end()));
    }
}

void require_lookup_maps_cover_scalar_with_tile_extras(
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& actual,
        const std::map<std::uint64_t, std::vector<std::uint64_t>>& expected,
        std::uint64_t total_bp,
        std::uint64_t fragment_bp,
        std::uint32_t k,
        std::uint32_t window,
        std::uint32_t letters_per_unit) {
    (void)fragment_bp;
    INFO("actual_kmers=" << actual.size() << " expected_kmers=" << expected.size());
    REQUIRE(actual.size() == expected.size());
    std::uint64_t total_extras = 0;
    auto actual_it = actual.begin();
    auto expected_it = expected.begin();
    for (; actual_it != actual.end() && expected_it != expected.end(); ++actual_it, ++expected_it) {
        INFO("actual_kmer=" << actual_it->first << " expected_kmer=" << expected_it->first
             << " actual_positions=" << actual_it->second.size()
             << " expected_positions=" << expected_it->second.size());
        REQUIRE(actual_it->first == expected_it->first);
        REQUIRE(std::includes(
            actual_it->second.begin(),
            actual_it->second.end(),
            expected_it->second.begin(),
            expected_it->second.end()));

        std::vector<std::uint64_t> extras;
        std::set_difference(
            actual_it->second.begin(),
            actual_it->second.end(),
            expected_it->second.begin(),
            expected_it->second.end(),
            std::back_inserter(extras));
        total_extras += extras.size();
    }
    const auto whole_plan = bioseq::gpu::vulkan::make_sketch_tile_plan(
        total_bp, k, window, letters_per_unit);
    const std::uint64_t radius =
        static_cast<std::uint64_t>(k) + static_cast<std::uint64_t>(window) + 1u;
    const std::uint64_t max_extras =
        static_cast<std::uint64_t>(whole_plan.tile_count) * 2u * radius;
    INFO("total_extras=" << total_extras << " max_extras=" << max_extras);
    REQUIRE(total_extras <= max_extras);
}

}  // namespace

TEST_CASE("per-sequence admitted run keeps its permit live through unwind, move and release",
          "[component:bioseq][layer:unit][feature:storage]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap_guard() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 20)));

    struct destruction_probe {
        bioseq::parallelism::memory_admission_controller* admission = nullptr;
        std::uint64_t expected_reserved = 0;
        bool* destroyed_with_live_permit = nullptr;
        bool active = true;

        destruction_probe(
                bioseq::parallelism::memory_admission_controller* owner,
                std::uint64_t expected,
                bool* observed) noexcept
            : admission(owner),
              expected_reserved(expected),
              destroyed_with_live_permit(observed)
        {}
        destruction_probe(destruction_probe&& other) noexcept
            : admission(other.admission),
              expected_reserved(other.expected_reserved),
              destroyed_with_live_permit(other.destroyed_with_live_permit),
              active(std::exchange(other.active, false))
        {}
        destruction_probe(const destruction_probe&) = delete;
        destruction_probe& operator=(const destruction_probe&) = delete;
        destruction_probe& operator=(destruction_probe&&) = delete;
        ~destruction_probe() {
            if (active) {
                *destroyed_with_live_permit =
                    admission->reserved_bytes() == expected_reserved;
            }
        }
    };

    using probe_run = bioseq::indexing::detail::admitted_vector_run<destruction_probe>;
    const auto make_run = [&]() {
        constexpr std::uint64_t elements = 4u;
        const std::uint64_t headroom =
            bioseq::indexing::detail::admitted_vector_preallocation_bytes<
                destruction_probe>(elements);
        auto permit = admission.try_reserve(headroom);
        REQUIRE(permit.has_value());
        probe_run run;
        run.permit = std::move(*permit);
        REQUIRE(bioseq::indexing::detail::reserve_admitted_vector_run_capacity(
            run, elements));
        return run;
    };

    bool unwind_observed = false;
    try {
        probe_run run = make_run();
        const std::uint64_t expected = baseline + run.permit.bytes();
        run.pairs.emplace_back(&admission, expected, &unwind_observed);
        throw std::runtime_error("adversarial unwind");
    } catch (const std::runtime_error&) {
    }
    REQUIRE(unwind_observed);
    REQUIRE(admission.reserved_bytes() == baseline);

    bool move_release_observed = false;
    {
        probe_run source = make_run();
        const std::uint64_t expected = baseline + source.permit.bytes();
        source.pairs.emplace_back(
            &admission, expected, &move_release_observed);
        probe_run destination{std::move(source)};
        REQUIRE(source.pairs.capacity() == 0u);
        REQUIRE_FALSE(static_cast<bool>(source.permit));
        REQUIRE(destination.permit.bytes() == destination.admitted_bytes);
        destination.release_capacity_before_permit();
        REQUIRE(destination.pairs.capacity() == 0u);
        REQUIRE(admission.reserved_bytes() == baseline);
    }
    REQUIRE(move_release_observed);
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("per-sequence admitted post-sort compaction preserves pair order and accounting",
          "[component:bioseq][layer:unit][feature:storage]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t previous_cap = admission.index_memory_cap_bytes();
    const std::uint64_t baseline = admission.reserved_bytes();
    struct restore_cap_guard {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous = 0;
        ~restore_cap_guard() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, previous_cap};
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(
        previous_cap, baseline + (std::uint64_t{1} << 20)));

    using pair_type = bioseq::indexing::detail::sorted_pair_t;
    bioseq::indexing::detail::admitted_pair_run run;
    constexpr std::uint64_t reserved_elements = 64u;
    const std::uint64_t headroom =
        bioseq::indexing::detail::admitted_vector_preallocation_bytes<pair_type>(
            reserved_elements);
    auto permit = admission.try_reserve(headroom);
    REQUIRE(permit.has_value());
    run.permit = std::move(*permit);
    REQUIRE(bioseq::indexing::detail::reserve_admitted_vector_run_capacity(
        run, reserved_elements));
    const std::size_t pre_compaction_capacity = run.pairs.capacity();
    run.pairs = {
        {1u, 2u}, {1u, 5u}, {3u, 1u}, {8u, 13u},
        {21u, 34u}, {55u, 89u}, {144u, 233u}, {377u, 610u},
    };
    const std::vector<pair_type> expected = run.pairs;

    bioseq::indexing::detail::compact_admitted_pair_run_after_sort(run);
    REQUIRE(run.pairs == expected);
    REQUIRE(run.pairs.capacity() <= pre_compaction_capacity);
    REQUIRE(run.admitted_bytes
            == run.pairs.capacity() * sizeof(pair_type));
    REQUIRE(run.permit.bytes() == run.admitted_bytes);
    run.release_capacity_before_permit();
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("assemble_for_query equals whole-sequence build restricted to query kmers",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::size_t fragment_units = 1024;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;

    auto data = make_dna4(40000, 0xBEEF01);
    range_t range(data);
    const auto total_bp = range.bp_size();

    fake_fragment_storage<> storage;
    fake_fragment_storage<> exact_storage;
    storage.run_buffer_bytes_ = 4096;   // TINY -> forces multiple spilled runs
    exact_storage.run_buffer_bytes_ = 4096;
    {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto sequence_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        bioseq::indexing::direct_minimizer_bucket_index<
            fake_fragment_storage<>>::build(storage, sequence_view);
    }
    {
        lazy_sequence<dna4_u64, chunked> lazy{
            chunked::from_range(range)};
        const auto exact_sequence_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        bioseq::indexing::direct_minimizer_bucket_index<
            fake_fragment_storage<>>::build(
                exact_storage,
                exact_sequence_view,
                minimizer_sketch_backend::exact_simd,
                fixed_minimizer_kernels(),
                nullptr,
                fixed_alphabet_code<::seq::dna4>());
    }
    REQUIRE(storage.runs_.empty());   // build() calls release_runs() at the end
    REQUIRE(exact_storage.runs_.empty());

    // Query = the whole sequence's own minimizers; max_occ wide-open.
    auto query = whole_sequence_minimizers(range, 10, 15);
    bioseq::indexing::minimizer_assembly_scratch scratch;
    auto idx = bioseq::indexing::direct_minimizer_bucket_index<fake_fragment_storage<>>::assemble_for_query(
        storage, std::span<const minimap_sketch_data>(query), total_bp, /*max_occ*/ INT32_MAX, scratch);
    bioseq::indexing::minimizer_assembly_scratch exact_scratch;
    auto exact_idx = bioseq::indexing::direct_minimizer_bucket_index<fake_fragment_storage<>>::assemble_for_query(
        exact_storage,
        std::span<const minimap_sketch_data>(query),
        total_bp,
        /*max_occ*/ INT32_MAX,
        exact_scratch);

    REQUIRE(idx.target_length() == total_bp);

    std::vector<std::uint64_t> kmers;
    for (const auto& m : query) kmers.push_back(m.kmer());

    require_lookup_maps_equal(
        lookup_map(idx, kmers),
        baseline_for_query(query, kmers, (std::size_t)INT32_MAX));
    require_lookup_maps_equal(
        lookup_map(exact_idx, kmers),
        lookup_map(idx, kmers));
}

TEST_CASE("scalar direct-bucket build stats are operation-originated and fail-closed",
          "[component:bioseq][layer:unit][feature:index]") {
    using stats_type = bioseq::indexing::direct_minimizer_bucket_build_stats;

    const stats_type empty{};
    CHECK_FALSE(empty.is_internally_consistent());

    stats_type first{};
    first.record_scalar_collection(
        /*symbols=*/64,
        /*storage_units=*/2,
        /*storage_unit_bytes=*/8,
        /*letters_per_unit=*/32);
    stats_type second{};
    second.record_scalar_collection(
        /*symbols=*/32,
        /*storage_units=*/1,
        /*storage_unit_bytes=*/8,
        /*letters_per_unit=*/32);

    stats_type aggregate{};
    aggregate.bind_backend(
        minimizer_sketch_backend::scalar,
        /*storage_unit_bytes=*/8,
        /*letters_per_unit=*/32);
    aggregate.merge(first);
    aggregate.merge(second);
    aggregate.record_completed_build();
    REQUIRE(aggregate.is_internally_consistent());
    CHECK(aggregate.scalar_collection_calls == 2);
    CHECK(aggregate.input_symbols == 96);
    CHECK(aggregate.input_storage_units == 3);
    CHECK(aggregate.input_bytes == 24);

    auto wrong_backend = aggregate;
    wrong_backend.selected_backend = minimizer_sketch_backend::exact_simd;
    CHECK_FALSE(wrong_backend.is_internally_consistent());
    auto saturated = aggregate;
    saturated.saturated = true;
    CHECK_FALSE(saturated.is_internally_consistent());
    auto no_calls = aggregate;
    no_calls.scalar_collection_calls = 0;
    CHECK_FALSE(no_calls.is_internally_consistent());
    auto no_symbols = aggregate;
    no_symbols.input_symbols = 0;
    CHECK_FALSE(no_symbols.is_internally_consistent());
    auto forged_bytes = aggregate;
    ++forged_bytes.input_bytes;
    CHECK_FALSE(forged_bytes.is_internally_consistent());
    auto duplicate_completion = aggregate;
    duplicate_completion.record_completed_build();
    CHECK_FALSE(duplicate_completion.is_internally_consistent());

    stats_type mismatched_worker{};
    mismatched_worker.bind_backend(
        minimizer_sketch_backend::exact_simd,
        /*storage_unit_bytes=*/8,
        /*letters_per_unit=*/32);
    auto mismatch = aggregate;
    mismatch.merge(mismatched_worker);
    CHECK_FALSE(mismatch.is_internally_consistent());
    CHECK(mismatch.aggregation_mismatch);
}

TEST_CASE("scalar direct-bucket build stats aggregate deterministically across workers",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::size_t fragment_units =
        bioseq::storage::default_fragment_storage_units;
    constexpr std::size_t fragment_bp =
        fragment_units * range_t::traits::letters_per_unit;
    auto data = make_dna4(5u * fragment_bp, 0x51A1A5u);
    range_t range(data);

    auto& pool = bioseq::parallelism::global_pool::instance();
    struct shutdown_pool_guard {
        bioseq::parallelism::global_pool& pool;
        ~shutdown_pool_guard() { pool.shutdown(); }
    } shutdown{pool};

    const auto run = [range_pointer = &range, pool_pointer = &pool](
            bioseq::parallelism::pipeline_concurrency_mode mode) {
        auto& pool = *pool_pointer;
        pool.restart(bioseq::parallelism::pool_config{
            .maximum_concurrency = mode
                    == bioseq::parallelism::pipeline_concurrency_mode::single_thread
                ? 1u : 2u,
            .mode = mode,
        });
        fake_fragment_storage<> storage;
        storage.run_buffer_bytes_ = 4096;
        auto lazy = bioseq::storage::make_resident_lazy_sequence(*range_pointer);
        const auto sequence_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        bioseq::indexing::direct_minimizer_bucket_build_stats stats{};
        bioseq::indexing::direct_minimizer_bucket_index<
            fake_fragment_storage<>>::build(
                storage,
                sequence_view,
                minimizer_sketch_backend::scalar,
                nullptr,
                &stats);
        return stats;
    };

    const auto serial = run(
        bioseq::parallelism::pipeline_concurrency_mode::single_thread);
    const auto parallel = run(
        bioseq::parallelism::pipeline_concurrency_mode::task_parallel);

    REQUIRE(serial.is_internally_consistent());
    REQUIRE(parallel.is_internally_consistent());
    CHECK(serial == parallel);
    CHECK(serial.scalar_collection_calls > 1);
    CHECK(serial.input_symbols >= range.bp_size());
    CHECK(serial.input_bytes
        == serial.input_storage_units
            * serial.source_storage_unit_bytes);
    CHECK(serial.input_symbols
        <= serial.input_storage_units
            * serial.letters_per_storage_unit);
}

TEST_CASE("exact_simd per-sequence collection accepts fragmented DNA5 with N resets",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    using dna5_u64 = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using dna5_range = compact_sequence_range<dna5_u64>;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, 2>;
    using minimizer_sequence_index_type =
        bioseq::indexing::direct_minimizer_bucket_index<fake_fragment_storage<>>;

    std::vector<seq::dna5> data(257);
    constexpr std::array<std::uint8_t, 4> acgt_ranks{0, 1, 2, 4};
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(acgt_ranks[(index * 3u + index / 7u) % 4u]);
    }
    for (const std::size_t position : {41u, 42u, 43u, 127u, 211u}) {
        data[position].assign_rank(3); // SeqAn dna5 N rank
    }
    dna5_range range(data);
    lazy_sequence<dna5_u64, chunked> lazy{
        chunked::from_range(range)};
    const auto sequence_view = bioseq::storage::make_lazy_sequence_view(lazy);
    fake_fragment_storage<> storage;

    REQUIRE_NOTHROW(minimizer_sequence_index_type::build(
        storage,
        sequence_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        nullptr,
        fixed_alphabet_code<::seq::dna5>()));
    REQUIRE(storage.runs_.empty());
}

TEST_CASE("assemble_for_query drops an over-frequent kmer under a tiny max_occ",
          "[component:bioseq][layer:unit][feature:index]") {
    constexpr std::size_t fragment_units = 1024;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;

    std::vector<seq::dna4> data;
    data.reserve(40000);
    const std::string motif = "ACGTCTAGTCACTGTAGCCTGATCGTAGCTAGT";
    for (std::size_t i = 0; i < 40000; ++i) {
        seq::dna4 c;
        c.assign_char(motif[i % motif.size()]);
        data.push_back(c);
    }
    range_t range(data);
    const auto total_bp = range.bp_size();

    fake_fragment_storage<> storage;
    storage.run_buffer_bytes_ = 4096;
    {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto sequence_view =
            bioseq::storage::make_lazy_sequence_view(lazy);
        bioseq::indexing::direct_minimizer_bucket_index<
            fake_fragment_storage<>>::build(storage, sequence_view);
    }

    auto query = whole_sequence_minimizers(range, 10, 15);

    // Find a kmer whose global DEDUPED count is > 1 (an over-frequent kmer).
    std::map<std::uint64_t, std::vector<std::uint64_t>> per_kmer;
    for (const auto& m : query) per_kmer[m.kmer()].push_back(m.pos_strand_encoded());
    std::uint64_t over_freq_kmer = 0;
    bool found = false;
    for (auto& [kmer, v] : per_kmer) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
        if (v.size() > 1) { over_freq_kmer = kmer; found = true; break; }
    }
    REQUIRE(found);   // a 40 kbp random dna4 sequence has repeated minimizers

    bioseq::indexing::minimizer_assembly_scratch scratch;
    auto idx = bioseq::indexing::direct_minimizer_bucket_index<fake_fragment_storage<>>::assemble_for_query(
        storage, std::span<const minimap_sketch_data>(query), total_bp, /*max_occ*/ 1, scratch);

    // Under max_occ=1 the over-frequent kmer is dropped entirely.
    REQUIRE(idx.lookup(over_freq_kmer).empty());
}

// Both persisted minimizer index layouts built with Vulkan must cover scalar lookups.
TEST_CASE("persisted minimizer indexes built with Vulkan cover scalar lookups",
          "[component:bioseq][layer:unit][feature:index][resource:gpu]") {
    if (!bioseq::gpu::vulkan::hardware_device_available(
            bioseq::gpu::vulkan::runtime::instance())) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }

    using backend_t = minimizer_sketch_backend;
    using config_t = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using cfg_range_t = compact_sequence_range<config_t>;
    using storage_unit = typename cfg_range_t::traits::storage_unit;
    using scanner_t =
        chunked_span_sequence_scanner<
            storage_unit,
            bioseq::storage::default_fragment_storage_units>;
    using lazy_t = lazy_sequence<config_t, scanner_t>;
    // The fragment index owns canonical 32 KiBP logical fragments; scanner chunk
    // width is an input-access detail and must not drive assembly geometry.
    constexpr std::size_t frag_bp = 32'768u;

    cfg_range_t target = make_deterministic_test_range<config_t>(8u << 20);
    lazy_t lazy_scalar{scanner_t::from_range(target)};
    lazy_t lazy_gpu{scanner_t::from_range(target)};
    const auto scalar_sequence_view =
        bioseq::storage::make_lazy_sequence_view(lazy_scalar);
    const auto gpu_sequence_view =
        bioseq::storage::make_lazy_sequence_view(lazy_gpu);

    test_fragment_storage scalar_fragment_storage;
    test_fragment_storage gpu_fragment_storage;
    test_fragment_storage scalar_sequence_storage;
    test_fragment_storage gpu_sequence_storage;

    bioseq::indexing::minimizers_per_fragment_index<test_fragment_storage>::build(
        scalar_fragment_storage, scalar_sequence_view, backend_t::scalar, nullptr);
    bioseq::indexing::minimizers_per_fragment_index<test_fragment_storage>::build(
        gpu_fragment_storage, gpu_sequence_view, backend_t::vulkan_segment, nullptr);

    bioseq::indexing::direct_minimizer_bucket_index<test_fragment_storage>::build(
        scalar_sequence_storage, scalar_sequence_view, backend_t::scalar, nullptr);
    bioseq::indexing::direct_minimizer_bucket_index<test_fragment_storage>::build(
        gpu_sequence_storage, gpu_sequence_view, backend_t::vulkan_segment, nullptr);

    bioseq::indexing::minimizer_assembly_scratch scratch_scalar;
    bioseq::indexing::minimizer_assembly_scratch scratch_gpu;
    bioseq::indexing::minimizer_assembly_scratch fragment_scratch_scalar;
    bioseq::indexing::minimizer_assembly_scratch fragment_scratch_gpu;
    std::vector<minimap_sketch_data> query_minimizers =
        make_deterministic_query_minimizers(target);
    std::vector<std::uint64_t> kmers;
    for (const auto& m : query_minimizers) {
        kmers.push_back(m.kmer());
    }

    const auto total_bp = target.bp_size();
    const std::size_t nfrag =
        static_cast<std::size_t>((total_bp + frag_bp - 1) / frag_bp);

    auto scalar_fragment_index =
        bioseq::indexing::minimizers_per_fragment_index<
            test_fragment_storage>::assemble_segment(
                scalar_fragment_storage,
                0,
                nfrag,
                frag_bp,
                total_bp,
                0,
                total_bp,
                fragment_scratch_scalar);
    auto gpu_fragment_index =
        bioseq::indexing::minimizers_per_fragment_index<
            test_fragment_storage>::assemble_segment(
                gpu_fragment_storage,
                0,
                nfrag,
                frag_bp,
                total_bp,
                0,
                total_bp,
                fragment_scratch_gpu);
    require_lookup_maps_cover_scalar_with_tile_extras(
        lookup_map(gpu_fragment_index, kmers),
        lookup_map(scalar_fragment_index, kmers),
        total_bp,
        frag_bp,
        15,
        10,
        cfg_range_t::traits::letters_per_unit);

    auto scalar_index =
        bioseq::indexing::direct_minimizer_bucket_index<
            test_fragment_storage>::assemble_for_query(
                scalar_sequence_storage,
                query_minimizers,
                total_bp,
                (std::numeric_limits<int>::max)(),
                scratch_scalar);
    auto gpu_index =
        bioseq::indexing::direct_minimizer_bucket_index<
            test_fragment_storage>::assemble_for_query(
                gpu_sequence_storage,
                query_minimizers,
                total_bp,
                (std::numeric_limits<int>::max)(),
                scratch_gpu);

    require_lookup_maps_cover_scalar_with_tile_extras(
        lookup_map(gpu_index, kmers),
        lookup_map(scalar_index, kmers),
        total_bp,
        frag_bp,
        15,
        10,
        cfg_range_t::traits::letters_per_unit);

    bioseq::indexing::minimizer_assembly_scratch scratch_scalar_max_occ;
    bioseq::indexing::minimizer_assembly_scratch scratch_gpu_max_occ;
    auto scalar_max_occ_index =
        bioseq::indexing::direct_minimizer_bucket_index<
            test_fragment_storage>::assemble_for_query(
                scalar_sequence_storage,
                query_minimizers,
                total_bp,
                1,
                scratch_scalar_max_occ);
    auto gpu_max_occ_index =
        bioseq::indexing::direct_minimizer_bucket_index<
            test_fragment_storage>::assemble_for_query(
                gpu_sequence_storage,
                query_minimizers,
                total_bp,
                1,
                scratch_gpu_max_occ);
    const auto gpu_max_occ_lookup = lookup_map(gpu_max_occ_index, kmers);
    const auto scalar_max_occ_lookup = lookup_map(scalar_max_occ_index, kmers);
    require_lookup_maps_cover_scalar(gpu_max_occ_lookup, scalar_max_occ_lookup);
    REQUIRE(bioseq::indexing::detail::effective_max_occ_threshold(1, false) == 1u);
    REQUIRE(bioseq::indexing::detail::effective_max_occ_threshold(1, true) == 2u);
    std::size_t scalar_useful_kmers_at_threshold = 0;
    for (const auto& [kmer, scalar_positions] : scalar_max_occ_lookup) {
        if (scalar_positions.empty()) {
            continue;
        }
        ++scalar_useful_kmers_at_threshold;
        const auto gpu_it = gpu_max_occ_lookup.find(kmer);
        REQUIRE(gpu_it != gpu_max_occ_lookup.end());
        REQUIRE(std::includes(
            gpu_it->second.begin(),
            gpu_it->second.end(),
            scalar_positions.begin(),
            scalar_positions.end()));
    }
    REQUIRE(scalar_useful_kmers_at_threshold > 0);

    bioseq::indexing::minimizer_assembly_scratch scratch_gpu_zero_occ;
    auto gpu_zero_occ_index =
        bioseq::indexing::direct_minimizer_bucket_index<
            test_fragment_storage>::assemble_for_query(
                gpu_sequence_storage,
                query_minimizers,
                total_bp,
                0,
                scratch_gpu_zero_occ);
    const auto gpu_unlimited_lookup = lookup_map(gpu_index, kmers);
    const auto gpu_zero_occ_lookup = lookup_map(gpu_zero_occ_index, kmers);
    std::size_t gpu_nonzero_kmers_filtered = 0;
    for (const auto& [kmer, gpu_positions] : gpu_unlimited_lookup) {
        if (gpu_positions.empty()) {
            continue;
        }
        ++gpu_nonzero_kmers_filtered;
        const auto filtered_it = gpu_zero_occ_lookup.find(kmer);
        REQUIRE(filtered_it != gpu_zero_occ_lookup.end());
        REQUIRE(filtered_it->second.empty());
        break;
    }
    REQUIRE(gpu_nonzero_kmers_filtered > 0);
}

}  // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
