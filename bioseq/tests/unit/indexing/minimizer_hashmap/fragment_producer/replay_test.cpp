/**
 * @file replay_test.cpp
 * @brief Checks repeated minimizer scans, fragment boundaries, and spill results.
 *
 * Highway compiles these tests for the selected SIMD target.
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
import bioseq.tests.unit.indexing.minimizer_hashmap.fragment_producer_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;
using namespace fragment_producer_test_support;

TEST_CASE("Vulkan fragment replay batch count obeys every existing admission bound",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_count = 1'000u;
    constexpr std::size_t fragment_bp = 32'768u;
    constexpr std::size_t aggregate_halo = 2u * (15u + 20u - 2u);

    REQUIRE(gpu_minimizer_fragments_per_batch(
                fragment_count, fragment_bp, aggregate_halo, 0u)
            == 1u);
    REQUIRE(gpu_minimizer_fragments_per_batch(
                fragment_count,
                fragment_bp,
                aggregate_halo,
                std::size_t{64u} << 20u)
            == 21u);
    REQUIRE(gpu_minimizer_fragments_per_batch(
                fragment_count,
                fragment_bp,
                aggregate_halo,
                std::size_t{2u} << 30u)
            == 255u);
    REQUIRE(gpu_minimizer_fragments_per_batch(
                7u,
                fragment_bp,
                aggregate_halo,
                std::size_t{2u} << 30u)
            == 7u);
    REQUIRE(gpu_minimizer_fragments_per_batch(
                fragment_count,
                fragment_bp,
                static_cast<std::size_t>(
                    minimap_sketch_data::k_position_field_mask),
                std::size_t{2u} << 30u)
            == 1u);
}

TEST_CASE("bounded ntHash fragment replay matches one checked resident execution",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = 32'768u;
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    auto data = make_dna4(70013, 0x50415045524E5448ULL);
    range_t range(data);
    sketch_collector resident_collector;
    const auto resident_result =
        collect_packed_bounded_nthash_index_erased(packed_bounded_nthash_index_bridge_request{
            .kernels = fixed_minimizer_kernels(),
            .sequence = fixed_packed_sequence_view(range),
            .sequence_context = &range,
            .collector_context = &resident_collector,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (resident_result.status == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(resident_result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(resident_result.stats.is_internally_consistent(true));

    std::vector<canonical_minimizer_event> oracle;
    oracle.reserve(resident_collector.extract_sketches_data().size());
    for (const minimap_sketch_data& minimizer : resident_collector.extract_sketches_data()) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::ranges::sort(oracle, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto options = test_generation_options(
        range.bp_size(), bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
    auto producer =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::bounded_nthash_simd,
                                                    fixed_minimizer_kernels(),
                                                    options.algorithm_identity,
                                                    options.window_size,
                                                    options.kmer_size,
                                                    fixed_alphabet_code<::seq::dna4>());
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(producer.scan_count() == 3);
    const minimizer_fragment_generation_execution_receipt execution = producer.execution_stats();
    REQUIRE(execution.is_accepted());
    REQUIRE(execution.selected_backend == minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(execution.scalar_core_runs == 0);
    REQUIRE(execution.exact_simd_core_runs == 0);
    REQUIRE(execution.bounded_nthash_core_runs > 0);
    REQUIRE(execution.vulkan_core_runs == 0);
    REQUIRE(execution.packed_bounded_stats_present);
    REQUIRE(execution.packed_bounded_status == packed_bounded_nthash_index_status::ok);
    REQUIRE(execution.packed_bounded_stats.is_internally_consistent(true));
    REQUIRE(execution.packed_bounded_stats.fallback_calls == 0);
    REQUIRE(execution.packed_bounded_stats.source_storage_unit_bytes == sizeof(std::uint64_t));
    REQUIRE(producer.scheme_identity() == k_bounded_nthash_u32_r7_sum_scheme_identity);
    REQUIRE(producer.packed_bounded_stats_present());
    REQUIRE(producer.packed_bounded_status() == packed_bounded_nthash_index_status::ok);
    REQUIRE(producer.packed_bounded_stats().is_internally_consistent(true));
    REQUIRE(producer.packed_bounded_stats().fallback_calls == 0);
    REQUIRE_FALSE(producer.exact_simd_stats_present());
    REQUIRE(producer.peak_bounded_materialized_window_bp() <=
            fragment_bp + 2u * halo + range_t::traits::letters_per_unit - 1u);
    REQUIRE(producer.peak_bounded_materialized_window_bp() < range.bp_size());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == oracle);
    require_complete_generation_rows(*encoded, sink.rows);
}

TEST_CASE("bounded ntHash fragment canonicalizes tied periodic events identically across replays",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    // Repeating ACGT produces tied windows that can emit the same minimizer
    // several times, with other records between repeats. Replay must remove
    // these duplicates before encoding the index.
    std::vector<seq::dna4> data(65565);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index & 3u));
    }
    range_t range(data);
    sketch_collector raw_collector;
    const auto raw_result =
        collect_packed_bounded_nthash_index_erased(packed_bounded_nthash_index_bridge_request{
            .kernels = fixed_minimizer_kernels(),
            .sequence = fixed_packed_sequence_view(range),
            .sequence_context = &range,
            .collector_context = &raw_collector,
            .storage_unit_bytes = sizeof(std::uint64_t),
            .letters_per_unit = range_t::traits::letters_per_unit,
            .storage_units = range.size(),
            .local_bp_size = range.bp_size(),
            .window_size = 10,
            .kmer_size = 15,
        });
    if (raw_result.status == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(raw_result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(raw_result.stats.is_internally_consistent(true));

    std::vector<canonical_minimizer_event> raw;
    raw.reserve(raw_collector.extract_sketches_data().size());
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> first_seen;
    bool saw_nonadjacent_duplicate = false;
    for (const minimap_sketch_data& minimizer : raw_collector.extract_sketches_data()) {
        const canonical_minimizer_event event{
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        };
        const auto key = std::pair{event.hash, event.encoded_position};
        if (const auto found = first_seen.find(key); found != first_seen.end()) {
            saw_nonadjacent_duplicate |= raw.size() > found->second + 1u;
        } else {
            first_seen.emplace(key, raw.size());
        }
        raw.push_back(event);
    }
    auto canonical = raw;
    std::ranges::sort(canonical, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });
    canonical.erase(std::ranges::unique(canonical).begin(), canonical.end());
    REQUIRE(raw.size() > canonical.size());
    REQUIRE(saw_nonadjacent_duplicate);

    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto bounded_identity =
        test_generation_algorithm_identity<::seq::dna4, std::uint64_t>(
            bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
    auto replay =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::bounded_nthash_simd,
                                                    fixed_minimizer_kernels(),
                                                    bounded_identity,
                                                    10u,
                                                    15u,
                                                    fixed_alphabet_code<::seq::dna4>());
    const auto collect_replay = [](void* context, const canonical_minimizer_event& event)
        -> std::expected<void, minimizer_generation_encoding_error> {
        static_cast<std::vector<canonical_minimizer_event>*>(context)->push_back(event);
        return {};
    };
    std::vector<canonical_minimizer_event> first_replay;
    std::vector<canonical_minimizer_event> second_replay;
    REQUIRE(replay
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &first_replay, .emit_fn = collect_replay})
                .has_value());
    REQUIRE(replay
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &second_replay, .emit_fn = collect_replay})
                .has_value());
    REQUIRE(first_replay == canonical);
    REQUIRE(second_replay == canonical);
    REQUIRE(first_replay == second_replay);
    REQUIRE(replay.scan_count() == 2u);
    REQUIRE(replay.execution_stats().is_accepted());
    REQUIRE(replay.execution_stats().selected_backend ==
            minimizer_sketch_backend::bounded_nthash_simd);
    REQUIRE(replay.execution_stats().scalar_core_runs == 0u);
    REQUIRE(replay.execution_stats().exact_simd_core_runs == 0u);
    REQUIRE(replay.execution_stats().vulkan_core_runs == 0u);
    REQUIRE(replay.execution_stats().bounded_nthash_core_runs > 0u);

    auto encoder_replay =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::bounded_nthash_simd,
                                                    fixed_minimizer_kernels(),
                                                    bounded_identity,
                                                    10u,
                                                    15u,
                                                    fixed_alphabet_code<::seq::dna4>());
    auto options = test_generation_options(
        range.bp_size(), bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(encoder_replay, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(encoder_replay.scan_count() == 3u);
    REQUIRE(encoder_replay.execution_stats().is_accepted());
    REQUIRE(decode_generation_events(*encoded, sink.rows) == canonical);
    require_complete_generation_rows(*encoded, sink.rows);
}

// clang-format off
TEST_CASE("bounded ntHash fragment replay canonicalizes storage and rejects incompatible biological shapes",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    // clang-format on
    using namespace bioseq::indexing;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    SECTION("DNA5 rejects a mismatched manifest alphabet identity") {
        using chunked = chunked_span_sequence_scanner<std::uint64_t, 64>;
        std::vector<seq::dna5> data(4096);
        for (std::size_t index = 0; index < data.size(); ++index) {
            data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
        }
        dna5_range_t range(data);
        lazy_sequence<dna5_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        const auto bounded_identity =
            test_generation_algorithm_identity<::seq::dna5, std::uint64_t>(
                bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
        auto producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::bounded_nthash_simd,
            fixed_minimizer_kernels(),
            bounded_identity,
            10u,
            15u,
            fixed_alphabet_code<::seq::dna5>());
        auto options = test_generation_options(range.bp_size());
        options.alphabet_code = 5;
        options.minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id;
        options.minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision;
        collecting_minimizer_row_sink sink;
        const std::uint64_t admission_before =
            bioseq::parallelism::memory_admission_controller::instance().reserved_bytes();
        require_wire_error(encode_minimizer_generation(producer, sink, options),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE(producer.scan_count() == 0u);
        REQUIRE_FALSE(producer.packed_bounded_stats_present());
        REQUIRE_FALSE(producer.exact_simd_stats_present());
        REQUIRE(bioseq::parallelism::memory_admission_controller::instance().reserved_bytes() ==
                admission_before);
    }

    SECTION("u16 packed storage has no generated READY identity") {
        using chunked = chunked_span_sequence_scanner<std::uint16_t, 64>;
        auto data = make_dna4(4096, 0x16B17u);
        dna4_u16_range_t range(data);
        lazy_sequence<dna4_u16, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        const auto native_identity =
            test_generation_algorithm_identity<::seq::dna4, std::uint64_t>(
                bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
        auto producer = index_type::make_generation_replay_producer(
            lazy_view,
            minimizer_sketch_backend::bounded_nthash_simd,
            fixed_minimizer_kernels(),
            native_identity,
            10u,
            15u,
            fixed_alphabet_code<::seq::dna4>());
        auto options = test_generation_options(
            range.bp_size(), bioseq::simd::minimizer_algorithm::bounded_nthash_simd);
        options.storage_unit_byte_width = sizeof(std::uint16_t);
        collecting_minimizer_row_sink sink;
        const auto encoded = encode_minimizer_generation(producer, sink, options);
        REQUIRE_FALSE(encoded.has_value());
        REQUIRE(encoded.error() == minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
        REQUIRE_FALSE(producer.packed_bounded_stats_present());
        REQUIRE_FALSE(producer.exact_simd_stats_present());
    }

    SECTION("a local position wider than minimap_sketch_data fails before dereference") {
        std::byte sentinel{};
        sketch_collector collector;
        const std::size_t local_bp_size =
            static_cast<std::size_t>(minimap_sketch_data::k_position_field_mask) + 2u;
        const std::size_t storage_units = (local_bp_size + range_t::traits::letters_per_unit - 1u) /
                                          range_t::traits::letters_per_unit;
        const auto rejected =
            collect_packed_bounded_nthash_index_erased(packed_bounded_nthash_index_bridge_request{
                .kernels = fixed_minimizer_kernels(),
                .sequence =
                    {
                        .storage = &sentinel,
                        .storage_units = storage_units,
                        .begin_symbol = 0u,
                        .end_symbol = local_bp_size,
                        .alphabet_code = fixed_alphabet_code<::seq::dna4>(),
                        .bits_per_symbol =
                            static_cast<std::uint8_t>(range_t::traits::bits_per_char),
                        .storage_unit_bytes = sizeof(std::uint64_t),
                        .final_segment = true,
                    },
                .sequence_context = &sentinel,
                .collector_context = &collector,
                .storage_unit_bytes = sizeof(std::uint64_t),
                .letters_per_unit = range_t::traits::letters_per_unit,
                .storage_units = storage_units,
                .local_bp_size = local_bp_size,
                .window_size = 10,
                .kmer_size = 15,
            });
        REQUIRE(rejected.status == packed_bounded_nthash_index_status::unsupported_representation);
        REQUIRE(rejected.stats.fail_closed_rejections == 1);
        REQUIRE(collector.extract_sketches_data().empty());
    }
}

TEST_CASE("actual producers cross the persisted 8MiB core edge bounded",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = k_minimizer_preferred_segment_span_bp;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    auto data = make_dna4(static_cast<std::size_t>(span + 8192u + 7u), 0x8B1B0A4DULL);
    // Mirror the sequence around the segment boundary and fill its centre
    // with A. Equal minima near the boungary must follow the same tie rule
    // as  the whole-sequence sketch.
    for (std::size_t offset = 0; offset < 192; ++offset) {
        seq::dna4 value{};
        value.assign_rank(static_cast<std::uint8_t>((offset * 3u) & 3u));
        data[static_cast<std::size_t>(span) - 1u - offset] = value;
        data[static_cast<std::size_t>(span) + offset] = value;
    }
    for (std::size_t position = static_cast<std::size_t>(span) - 24u;
         position < static_cast<std::size_t>(span) + 24u;
         ++position) {
        data[position].assign_rank(0);
    }
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);

    const auto options = test_generation_options(range.bp_size());
    auto fragment_producer = index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::scalar,
        nullptr,
        options.algorithm_identity,
        options.window_size,
        options.kmer_size);
    const auto fragment_plan = plan_minimizer_generation(fragment_producer, options);
    REQUIRE(fragment_plan.has_value());
    REQUIRE(fragment_plan->position_segment_count == 2);
    REQUIRE(fragment_producer.peak_buffered_core_bp() <= span);
    REQUIRE(fragment_producer.scan_count() == 1);

    fake_fragment_storage<> spill_storage;
    const auto spill_options = test_grouped_soa_generation_options(
        range.bp_size(),
        65'534u,
        bioseq::simd::minimizer_algorithm::scalar,
        range.bp_size());
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::scalar,
        nullptr,
        spill_options.algorithm_identity,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() > 256);
    REQUIRE(spill_storage.runs_.empty());
    REQUIRE(spill_storage.segment_spill_run_count() == spilled->run_count());
    REQUIRE(spill_storage.segment_runs_.size() == 2);
    constexpr std::uint64_t half_span = span / 2u;
    std::vector<std::uint64_t> half_counts(range.bp_size() / half_span +
                                           (range.bp_size() % half_span != 0 ? 1u : 0u));
    for (const auto& [segment, runs] : spill_storage.segment_runs_) {
        (void)segment;
        for (const auto& run : runs) {
            for (const auto& event : run) {
                ++half_counts[(event.second >> 1u) / half_span];
            }
        }
    }
    REQUIRE(half_counts.size() == 3);
    REQUIRE(half_counts[0] > 0);
    REQUIRE(half_counts[1] > 0);
    const std::uint64_t half_max = *std::ranges::max_element(half_counts);
    REQUIRE(half_counts[0] + half_counts[1] > half_max);
    grouped_soa_generation_limits one_half_per_segment;
    one_half_per_segment.max_segment_position_count = half_max;
    collecting_minimizer_row_sink spill_sink;
    minimizer_sequence_generation_execution_receipt execution;
    const auto encoded = spill_run_index_type::encode_persisted_generation(
        spill_storage,
        spill_sink,
        std::move(*spilled),
        spill_options,
        one_half_per_segment,
        &execution);
    REQUIRE(encoded.has_value());
    REQUIRE(execution.is_internally_consistent());
    REQUIRE(execution.replay_scan_count == 4);
    REQUIRE(execution.replay_merge_pass_count == 4);
    REQUIRE(execution.replay_reroute_pass_count == 1);
    REQUIRE(execution.replay_failed_scan_count == 1);
    REQUIRE(execution.replay_binding_checked_failed_scan_count == 1);
    REQUIRE(execution.replay_completed_scan_count == 3);
    REQUIRE(execution.replay_first_completed_content_digest ==
            execution.replay_last_completed_content_digest);
    REQUIRE(execution.replay_final_span_bp == half_span);
    REQUIRE(execution.replay_final_storage_binding.generation_token ==
            execution.spill_storage_binding.generation_token);
    REQUIRE(execution.replay_final_storage_binding.mutation_revision ==
            execution.spill_storage_binding.mutation_revision + 1u);
    REQUIRE(encoded->manifest.position_segment_span_bp == half_span);
    REQUIRE(encoded->manifest.position_segment_count == 3);
    REQUIRE(encoded->manifest.position_count == fragment_plan->canonical_event_count);
    REQUIRE(spill_storage.segment_reroute_calls() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 5);
    require_complete_generation_rows(*encoded, spill_sink.rows);
}

TEST_CASE("actual producers preserve DNA5 N reset and packed tail",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::uint64_t fragment_bp = fragment_units * 21u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    constexpr std::size_t target_size = 70013;
    std::vector<seq::dna5> data(target_size);
    std::uint64_t state = 0xD5A5C0DEULL;
    std::vector<std::size_t> n_positions;
    for (std::size_t index = 0; index < data.size(); ++index) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::uint8_t rank = static_cast<std::uint8_t>((state >> 33u) % 4u);
        if (index % 211u == 97u) {
            rank = 3u; // N resets the rolling hash and minimizer window.
            n_positions.push_back(index);
        } else if (rank == 3u) {
            rank = 4u; // DNA5 stores T at rank 4.
        }
        data[index].assign_rank(rank);
    }
    for (const std::size_t boundary : {static_cast<std::size_t>(fragment_bp - 1u),
                                       static_cast<std::size_t>(fragment_bp),
                                       target_size - 80u}) {
        data[boundary].assign_rank(3u);
        n_positions.push_back(boundary);
    }
    dna5_range_t range(data);
    REQUIRE(range.padding() != 0);

    std::vector<canonical_minimizer_event> oracle = baseline_dna5_events(data, 10, 15);
    std::ranges::sort(oracle, {}, [](const canonical_minimizer_event& event) {
        return std::pair{event.hash, event.encoded_position};
    });

    lazy_sequence<dna5_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto collection_range = spill_run_index_type::make_generation_collection_range(
        lazy_view, fragment_bp, fragment_bp + 100u, 10u, 15u);
    REQUIRE(collection_range.has_value());
    constexpr std::uint64_t halo = 10u + 15u - 2u;
    REQUIRE(collection_range->cs_u == fragment_bp - halo);
    REQUIRE(collection_range->ce_u == fragment_bp + 100u + halo);
    REQUIRE(collection_range->core_lo_bp == fragment_bp);
    REQUIRE(collection_range->core_hi_bp == fragment_bp + 100u);
    auto fragment_producer =
        index_type::make_generation_replay_producer(lazy_view,
                                                    minimizer_sketch_backend::exact_simd,
                                                    fixed_minimizer_kernels(),
                                                    test_generation_algorithm_identity<
                                                        ::seq::dna5,
                                                        std::uint64_t>(
                                                        bioseq::simd::minimizer_algorithm::
                                                            two_stack_nthash),
                                                    10u,
                                                    15u,
                                                    fixed_alphabet_code<::seq::dna5>());
    const auto collect_replay = [](void* context, const canonical_minimizer_event& event)
        -> std::expected<void, minimizer_generation_encoding_error> {
        static_cast<std::vector<canonical_minimizer_event>*>(context)->push_back(event);
        return {};
    };
    std::vector<canonical_minimizer_event> fragment_events;
    REQUIRE(fragment_producer
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &fragment_events, .emit_fn = collect_replay})
                .has_value());

    fake_fragment_storage<> spill_storage;
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        test_generation_algorithm_identity<::seq::dna5, std::uint64_t>(
            bioseq::simd::minimizer_algorithm::two_stack_nthash, range.bp_size()),
        fragment_bp,
        k_minimizer_preferred_segment_span_bp,
        nullptr,
        fixed_alphabet_code<::seq::dna5>());
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->exact_simd_stats_available());
    const exact_simd_index_execution_stats spill_exact_stats = spilled->exact_simd_stats();
    auto spill_producer = spill_run_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    std::vector<canonical_minimizer_event> spill_events;
    REQUIRE(spill_producer
                .scan_sorted_events(k_minimizer_preferred_segment_span_bp,
                                    {.context = &spill_events, .emit_fn = collect_replay})
                .has_value());
    REQUIRE(fragment_producer.exact_simd_stats_present());
    REQUIRE(fragment_producer.exact_simd_stats().is_internally_consistent(true));
    REQUIRE(fragment_producer.exact_simd_stats().ambiguous_symbols > 0);
    REQUIRE(spill_exact_stats.is_internally_consistent(true));
    REQUIRE(spill_exact_stats.ambiguous_symbols > 0);
    REQUIRE(spill_producer.merge_pass_count() == 1);
    REQUIRE(spill_storage.segment_merge_calls() == 2);
    REQUIRE(spill_storage.segment_merge_run_visits() ==
            2u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited() ==
            2u * spill_storage.segment_spill_bytes());
    REQUIRE(fragment_events == oracle);
    REQUIRE(spill_events == oracle);
    REQUIRE(std::ranges::any_of(fragment_events, [](const auto& event) {
        return (event.encoded_position >> 1u) > target_size - 40u;
    }));
    const auto baseline_lookup = canonical_event_lookup_map(oracle);
    REQUIRE(canonical_event_lookup_map(fragment_events) == baseline_lookup);
    REQUIRE(canonical_event_lookup_map(spill_events) == baseline_lookup);
    for (const canonical_minimizer_event& event : fragment_events) {
        const std::uint64_t last = event.encoded_position >> 1u;
        const std::uint64_t first = last + 1u >= 15u ? last + 1u - 15u : 0u;
        REQUIRE(std::ranges::none_of(
            n_positions, [first, last](std::size_t n) { return first <= n && n <= last; }));
    }
}

TEST_CASE("segment overflow retry halves deterministically",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = k_minimizer_preferred_segment_span_bp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> distributed{
        {1, 2u},
        {2, 4u},
        {3, ((preferred / 2u + 1u) << 1u)},
        {4, ((preferred / 2u + 2u) << 1u)},
    };
    const grouped_soa_generation_limits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 2,
    };
    vector_replay_minimizer_producer producer{distributed};
    const auto selected =
        plan_minimizer_generation(producer, test_generation_options(preferred), two_per_core);
    REQUIRE(selected.has_value());
    REQUIRE(selected->position_segment_span_bp == preferred / 2u);
    REQUIRE(producer.scan_count == 2);

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> indivisible{
        {9, 2u},
        {9, 3u},
    };
    const grouped_soa_generation_limits one_per_hash{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    vector_replay_minimizer_producer impossible_producer{indivisible};
    const auto impossible = plan_minimizer_generation(
        impossible_producer, test_generation_options(preferred), one_per_hash);
    require_wire_error(impossible, minimizer_generation_encoding_error::element_count);
}

TEST_CASE("unbound spill-run replay cannot claim a producer backend",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = k_minimizer_preferred_segment_span_bp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> run{
        {1, 2u},
        {2, 4u},
        {3, ((preferred / 2u + 1u) << 1u)},
        {4, ((preferred / 2u + 2u) << 1u)},
    };
    fake_fragment_storage<> storage;
    storage.spill_sorted_segment_run(0,
                                     std::span<const std::pair<std::uint64_t, std::uint64_t>>{run});
    const std::uint64_t spill_bytes = storage.segment_spill_bytes();
    REQUIRE(spill_bytes == run.size() * sizeof(run.front()));

    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage, preferred, preferred, k_minimap_scheme_identity};
    collecting_minimizer_row_sink sink;
    const grouped_soa_generation_limits two_per_core{
        .max_segment_control_count = 2,
        .max_segment_position_count = 2,
        .max_hash_position_count = 1,
    };
    const auto encoded = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(preferred), two_per_core);
    require_wire_error(encoded, minimizer_generation_encoding_error::storage_contract_mismatch);
    const minimizer_sequence_generation_execution_receipt execution = producer.execution_receipt();
    REQUIRE_FALSE(execution.is_internally_consistent());
    REQUIRE(execution.replay_scan_count == 0);
    REQUIRE(execution.replay_merge_pass_count == 0);
    REQUIRE(execution.replay_reroute_pass_count == 0);
    REQUIRE(execution.replay_completed_scan_count == 0);
    REQUIRE(execution.replay_failed_scan_count == 0);
    REQUIRE(execution.replay_illegal_span_transition_count == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_spill_bytes() == spill_bytes);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("raw spill-run replay rejects a non-halving span transition",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t preferred = k_minimizer_preferred_segment_span_bp;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> run{{
        {1u, 2u},
    }};
    fake_fragment_storage<> storage;
    storage.spill_sorted_segment_run(0, run);
    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage, preferred, preferred, k_minimap_scheme_identity};
    int receiver_context = 0;
    const auto rejected = producer.scan_sorted_events(
        preferred / 4u,
        canonical_minimizer_event_receiver{
            .context = &receiver_context,
            .emit_fn = +[](void*, const canonical_minimizer_event&)
                -> std::expected<void, minimizer_generation_encoding_error> { return {}; },
        });
    require_wire_error(rejected, minimizer_generation_encoding_error::segment_span);
    REQUIRE(producer.scan_count() == 1);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(producer.failed_scan_count() == 1);
    REQUIRE(producer.illegal_span_transition_count() == 1);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
}

TEST_CASE("empty unbound spill-run replay also fails producer identity",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    fake_fragment_storage<> storage;
    direct_minimizer_unbound_external_spill_replay_producer<fake_fragment_storage<>> producer{
        storage, 0, k_minimizer_preferred_segment_span_bp, k_minimap_scheme_identity};
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, test_generation_options(0));
    require_wire_error(encoded, minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(producer.scan_count() == 0);
    REQUIRE(producer.merge_pass_count() == 0);
    REQUIRE(producer.reroute_pass_count() == 0);
    REQUIRE(storage.segment_merge_calls() == 0);
    REQUIRE(storage.segment_reroute_calls() == 0);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("exact duplicate canonical events fail before row publication",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> duplicated{
        {7, 20},
        {7, 20},
    };
    vector_replay_minimizer_producer producer{duplicated};
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, test_generation_options(100));
    require_wire_error(encoded, minimizer_generation_encoding_error::duplicate_event);
    REQUIRE(sink.rows.empty());
}

TEST_CASE("encoder rejects a drifting replay before READY publication",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    drifting_replay_minimizer_producer producer;
    collecting_minimizer_row_sink sink;
    const auto encoded = encode_minimizer_generation(producer, sink, test_generation_options(100u));
    require_wire_error(encoded, minimizer_generation_encoding_error::replay_mismatch);
    REQUIRE(producer.scan_count == 2u);
    REQUIRE(std::ranges::none_of(
        sink.rows, [](const auto& row) { return row.address.sub_key == k_directory_sub_key; }));
}

TEST_CASE("grouped-SoA rejects scheme and backend drift at every scan boundary",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    for (const identity_drift_axis axis :
         {identity_drift_axis::scheme, identity_drift_axis::backend}) {
        for (const std::size_t boundary : {1u, 2u, 3u}) {
            INFO("axis=" << static_cast<unsigned>(axis) << " completed scan=" << boundary);
            boundary_identity_drifting_producer producer{
                .drift_after_scan = boundary,
                .axis = axis,
            };
            collecting_minimizer_row_sink sink;
            const auto encoded = encode_minimizer_generation(
                producer, sink, test_grouped_soa_generation_options(100u, 10u));
            require_wire_error(encoded, minimizer_generation_encoding_error::replay_mismatch);
            REQUIRE(producer.scan_count == boundary);
            REQUIRE(std::ranges::none_of(sink.rows, [](const auto& row) {
                return row.address.sub_key == k_directory_sub_key;
            }));
        }
    }
}

TEST_CASE("heavy hash is never split across partitions or page extents",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t target_length = 100000;
    constexpr std::uint64_t heavy_position_count = 70000;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(heavy_position_count + 1u);
    for (std::uint64_t index = 0; index < heavy_position_count; ++index) {
        minimizers.emplace_back(42, (index << 1u) | (index & 1u));
    }
    minimizers.emplace_back(43, 90000u << 1u);
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    auto generation =
        encode_minimizer_generation(producer, sink, test_generation_options(target_length));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2);
    const auto& heavy = generation->directory.entries()[0];
    REQUIRE(heavy.lo_hash == 42);
    REQUIRE(heavy.hi_hash == 42);
    REQUIRE(heavy.control_element_count == 1);
    REQUIRE(heavy.position_element_count == heavy_position_count);
    REQUIRE(heavy.position_encoded_bytes > k_minimizer_partition_target_bytes);
    REQUIRE(heavy.position_chunk_count > 1);
    const auto heavy_positions = collect_encoded_extent(sink.rows, heavy, false);
    REQUIRE(heavy_positions.size() ==
            heavy_position_count *
                static_cast<std::uint8_t>(generation->directory.profile().position_width));
    REQUIRE(generation->directory.entries()[1].lo_hash == 43);
    REQUIRE(sink.max_callback_bytes == k_grouped_soa_extent_payload_bytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("grouped-SoA fragment and external-spill replays are identical",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 256u;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    const auto verify = [](std::vector<seq::dna4> data, bool require_offset_widening) {
        range_t range(data);
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        auto fragment_options = test_grouped_soa_generation_options(
            range.bp_size(),
            65'534u,
            bioseq::simd::minimizer_algorithm::two_stack_nthash);
        auto fragment_producer =
            index_type::make_generation_replay_producer(lazy_view,
                                                        minimizer_sketch_backend::exact_simd,
                                                        fixed_minimizer_kernels(),
                                                        fragment_options.algorithm_identity,
                                                        fragment_options.window_size,
                                                        fragment_options.kmer_size,
                                                        fixed_alphabet_code<::seq::dna4>());
        collecting_minimizer_row_sink fragment_sink;
        const auto fragment_generation =
            encode_minimizer_generation(fragment_producer, fragment_sink, fragment_options);
        REQUIRE(fragment_generation.has_value());
        REQUIRE(fragment_producer.scan_count() == 3u);
        const auto fragment_stats = fragment_producer.execution_stats();
        INFO("grouped-SoA fragment evidence scans="
             << fragment_stats.producer_scan_count
             << " required_successful=" << fragment_stats.required_successful_scan_count()
             << " geometry_retries=" << fragment_stats.geometry_retry_count
             << " saturated=" << fragment_stats.saturated);
        REQUIRE(fragment_stats.generation_layout ==
                minimizer_generation_layout::grouped_soa_segment_local);
        REQUIRE(fragment_stats.required_successful_scan_count() == 3u);
        REQUIRE(fragment_stats.is_accepted());
        REQUIRE(fragment_stats.selected_backend == minimizer_sketch_backend::exact_simd);
        REQUIRE(fragment_stats.scalar_core_runs == 0u);
        REQUIRE(fragment_stats.exact_simd_core_runs > 0u);
        REQUIRE(fragment_stats.bounded_nthash_core_runs == 0u);
        REQUIRE(fragment_stats.vulkan_core_runs == 0u);
        REQUIRE(fragment_stats.exact_simd_stats_present);
        REQUIRE(fragment_stats.exact_simd_stats.is_internally_consistent(true));

        fake_fragment_storage<> spill_storage;
        minimizer_sequence_generation_execution_receipt spill_execution;
        auto spill_options = test_grouped_soa_generation_options(
            range.bp_size(),
            65'534u,
            bioseq::simd::minimizer_algorithm::two_stack_nthash,
            range.bp_size());
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            spill_storage,
            lazy_view,
            minimizer_sketch_backend::exact_simd,
            fixed_minimizer_kernels(),
            spill_options.algorithm_identity,
            fragment_bp,
            k_minimizer_preferred_segment_span_bp,
            &spill_execution,
            fixed_alphabet_code<::seq::dna4>());
        REQUIRE(spill.has_value());
        REQUIRE(spill->run_count() >= 2u);
        REQUIRE(spill_execution.is_spill_valid());
        REQUIRE(spill_execution.selected_backend == minimizer_sketch_backend::exact_simd);
        REQUIRE(spill_execution.scalar_core_runs == 0u);
        REQUIRE(spill_execution.exact_simd_core_runs > 0u);
        REQUIRE(spill_execution.bounded_nthash_core_runs == 0u);
        REQUIRE(spill_execution.vulkan_core_runs == 0u);
        REQUIRE(spill_execution.exact_simd_stats_available);
        REQUIRE(spill_execution.exact_simd_stats.is_internally_consistent(true));
        auto spill_producer = spill_run_index_type::make_generation_replay_producer(
            spill_storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink spill_sink;
        ++spill_options.generation_id;
        const auto spill_generation =
            encode_minimizer_generation(spill_producer, spill_sink, spill_options);
        REQUIRE(spill_generation.has_value());
        REQUIRE(spill_producer.scan_count() == 3u);
        REQUIRE(spill_producer.merge_pass_count() == 3u);
        const auto spill_replay_stats = spill_producer.execution_receipt();
        REQUIRE(spill_replay_stats.generation_layout ==
                minimizer_generation_layout::grouped_soa_segment_local);
        REQUIRE(spill_replay_stats.required_successful_replay_count() == 3u);
        REQUIRE(spill_replay_stats.replay_scan_count == 3u);
        REQUIRE(spill_replay_stats.replay_completed_scan_count == 3u);
        REQUIRE(spill_replay_stats.replay_failed_scan_count == 0u);
        REQUIRE(spill_replay_stats.replay_reroute_pass_count == 0u);
        REQUIRE(spill_replay_stats.replay_visited_event_count ==
                3u * spill_replay_stats.spilled_event_count);
        require_encoded_generations_equal_normalized(
            *fragment_generation, fragment_sink.rows, *spill_generation, spill_sink.rows);
        REQUIRE(decode_generation_events(*fragment_generation, fragment_sink.rows) ==
                decode_generation_events(*spill_generation, spill_sink.rows));
        require_complete_generation_rows(*fragment_generation, fragment_sink.rows);
        require_complete_generation_rows(*spill_generation, spill_sink.rows);

        if (require_offset_widening) {
            REQUIRE(std::ranges::any_of(
                fragment_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width == minimizer_scalar_width::u32;
                }));
            REQUIRE(
                std::ranges::any_of(spill_generation->directory.entries(), [](const auto& entry) {
                    return entry.shape_offset_width == minimizer_scalar_width::u32;
                }));
        }

        const auto fourth_replay = spill_producer.scan_sorted_events(
            spill_generation->manifest.position_segment_span_bp,
            canonical_minimizer_event_receiver{
                .context = nullptr,
                .emit_fn = [](void*, const canonical_minimizer_event&)
                    -> std::expected<void, minimizer_generation_encoding_error> { return {}; },
            });
        require_wire_error(fourth_replay, minimizer_generation_encoding_error::segment_span);
        const auto terminal_evidence = spill_producer.execution_receipt();
        REQUIRE(terminal_evidence.replay_scan_count == 4u);
        REQUIRE(terminal_evidence.replay_completed_scan_count == 3u);
        REQUIRE(terminal_evidence.replay_failed_scan_count == 1u);
    };

    SECTION("random fragmented sequence") {
        verify(make_dna4(18'013u, 0x373352414E444F4DULL), false);
    }
    SECTION("long tandem sequence widens offsets") {
        std::vector<seq::dna4> tandem(70'100u);
        for (auto& base : tandem)
            base.assign_rank(0u);
        verify(std::move(tandem), true);
    }
}

TEST_CASE("registered transient fragment build binds K20 DNA5 reference request to native owner",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    const auto unambiguous = make_dna4(1024u, 0x484153484F574E52ULL);
    std::vector<seq::dna5> data(unambiguous.size());
    for (std::size_t index = 0u; index < data.size(); ++index) {
        data[index].assign_char(unambiguous[index].to_char());
    }
    // Leave long runs of bases between the two Ns so this test builds
    // a nonempty index.
    data[127u].assign_char('N');
    data[769u].assign_char('N');
    dna5_range_t range(data);
    lazy_sequence<dna5_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    const auto identity = test_generation_algorithm_identity<::seq::dna5, std::uint64_t>(
        bioseq::simd::minimizer_algorithm::simd_minimap2_reference,
        k_minimizer_fragment_algorithm_position_extent_bp,
        20u,
        15u);
    REQUIRE(identity.requested_algorithm ==
            bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
    REQUIRE(identity.effective_algorithm ==
            bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
    REQUIRE(identity.hash_width == minimizer_scalar_width::u64);
    REQUIRE(identity.packed_storage_bits == 64u);

    fake_fragment_storage<> storage;
    const auto built = index_type::build_registered(storage,
                                                    lazy_view,
                                                    minimizer_sketch_backend::exact_simd,
                                                    fixed_minimizer_kernels(),
                                                    identity,
                                                    15u,
                                                    20u,
                                                    fixed_alphabet_code<::seq::dna5>());
    REQUIRE(built.has_value());
    REQUIRE(built->direct_build_accepted());
    REQUIRE(built->algorithm_identity == identity);
    REQUIRE(built->selected_backend == minimizer_sketch_backend::exact_simd);
    REQUIRE(built->producer_scan_count == 1u);
    REQUIRE(built->scalar_core_runs == 0u);
    REQUIRE(built->exact_simd_core_runs > 0u);
    REQUIRE(built->bounded_nthash_core_runs == 0u);
    REQUIRE_FALSE(built->exact_simd_stats_present);
    REQUIRE(built->reference_simd_stats_present);
    REQUIRE(built->reference_simd_stats.is_internally_consistent(
        true, sizeof(std::uint64_t)));
    REQUIRE(storage.number_of_chunks(k_directory_sub_key) == 1u);
    REQUIRE(storage.number_of_sub_keys() == 1u);
    REQUIRE(*storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0u) == 1);

    std::size_t emitted_positions = 0u;
    bool has_wide_hash = false;
    const auto* blob = storage.template read_chunk<char>(0, 0u);
    wire_for_each_entry(blob, storage.chunk_byte_size(0, 0u),
        [&emitted_positions, &has_wide_hash, target_length = data.size()](
            std::uint64_t hash, std::uint64_t position) {
            REQUIRE((position >> 1u) < target_length);
            has_wide_hash = has_wide_hash ||
                hash > (std::numeric_limits<std::uint32_t>::max)();
            ++emitted_positions;
        });
    REQUIRE(emitted_positions > 0u);
    REQUIRE(has_wide_hash);

    auto invalid_identity = identity;
    invalid_identity.runtime_window_size = 10u;
    fake_fragment_storage<> rejected_storage;
    const auto rejected = index_type::build_registered(
        rejected_storage,
        lazy_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        invalid_identity,
        15u,
        20u,
        fixed_alphabet_code<::seq::dna5>());
    require_wire_error(rejected,
                       minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(rejected_storage.number_of_sub_keys() == 0u);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
