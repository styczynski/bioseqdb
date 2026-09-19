/**
 * @file fragment_producer_test.cpp
 * @brief Tests producer identity, canonical streams, storage state, and GPU statistics.
 *
 * The tests run inside `bioseq::simd::HWY_NAMESPACE` for fixed-target coverage.
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
import bioseq.common.hashing; //  shared SHA-256

import bioseq.tests.support.indexing_storage_fakes;

import bioseq.tests.unit.indexing.minimizer_hashmap_support;
import bioseq.tests.unit.indexing.minimizer_hashmap.fragment_producer_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;
using namespace fragment_producer_test_support;

TEST_CASE("independent fragment halo and spill-run merge producers are canonical",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024;
    constexpr std::size_t fragment_bp = 32768;
    constexpr std::size_t halo = 10u + 15u - 2u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(70000, 0x371C0DEULL);
    range_t range(data);

    auto oracle_sketches = whole_sequence_minimizers(range, 10, 15);
    std::vector<canonical_minimizer_event> oracle;
    oracle.reserve(oracle_sketches.size());
    for (const minimap_sketch_data& minimizer : oracle_sketches) {
        oracle.push_back({
            .hash = minimizer.kmer(),
            .encoded_position = minimizer.pos_strand_encoded(),
        });
    }
    std::sort(oracle.begin(), oracle.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.hash != rhs.hash)
            return lhs.hash < rhs.hash;
        return lhs.encoded_position < rhs.encoded_position;
    });
    REQUIRE(std::adjacent_find(oracle.begin(), oracle.end()) == oracle.end());
    REQUIRE(std::ranges::any_of(oracle, [](const auto& event) {
        const std::uint64_t position = event.encoded_position >> 1u;
        return position + halo >= fragment_bp && position <= fragment_bp + halo;
    }));

    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto fragment_options = test_generation_options(range.bp_size());
    std::uint64_t cooperative_checkpoint_calls = 0u;
    auto fragment_producer = index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::scalar,
        nullptr,
        fragment_options.algorithm_identity,
        fragment_options.window_size,
        fragment_options.kmer_size,
        fixed_alphabet_code<::seq::dna4>(),
        0u,
        cooperative_cancellation_checkpoint{
            .context = &cooperative_checkpoint_calls,
            .invoke_fn = +[](void* context) {
                ++*static_cast<std::uint64_t*>(context);
            },
        });
    collecting_minimizer_row_sink fragment_sink;
    auto fragment_encoding =
        encode_minimizer_generation(fragment_producer, fragment_sink, fragment_options);
    REQUIRE(fragment_encoding.has_value());
    REQUIRE(fragment_producer.scan_count() == 3);
    REQUIRE(cooperative_checkpoint_calls > fragment_producer.scan_count());
    const minimizer_fragment_generation_execution_receipt scalar_execution =
        fragment_producer.execution_stats();
    REQUIRE(scalar_execution.is_accepted());
    REQUIRE(scalar_execution.selected_backend == minimizer_sketch_backend::scalar);
    REQUIRE(scalar_execution.scalar_core_runs > 0);
    REQUIRE(scalar_execution.exact_simd_core_runs == 0);
    REQUIRE(scalar_execution.bounded_nthash_core_runs == 0);
    REQUIRE(scalar_execution.vulkan_core_runs == 0);
    REQUIRE(fragment_producer.peak_buffered_core_bp() <= k_minimizer_preferred_segment_span_bp);

    fake_fragment_storage<> spill_storage;
    auto spill_options = test_generation_options(
        range.bp_size(), bioseq::simd::minimizer_algorithm::scalar, range.bp_size());
    auto spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::scalar,
        nullptr,
        spill_options.algorithm_identity,
        fragment_bp);
    REQUIRE(spilled.has_value());
    REQUIRE(spilled->run_count() >= 3);
    const std::uint64_t scalar_spill_run_count = spilled->run_count();
    auto spill_producer = spill_run_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spilled));
    collecting_minimizer_row_sink spill_sink;
    ++spill_options.generation_id;
    auto spill_encoding = encode_minimizer_generation(spill_producer, spill_sink, spill_options);
    REQUIRE(spill_encoding.has_value());
    REQUIRE(spill_producer.scan_count() == 3);
    REQUIRE(spill_producer.merge_pass_count() == 3);
    REQUIRE(spill_storage.segment_merge_calls() == 4);
    REQUIRE(spill_storage.segment_merge_run_visits() ==
            4u * spill_storage.segment_spill_run_count());
    REQUIRE(spill_storage.segment_merge_bytes_visited() ==
            4u * spill_storage.segment_spill_bytes());
    STATIC_REQUIRE(decltype(spill_producer)::owned_event_capacity() == 0);
    require_encoded_generations_equal_normalized(
        *fragment_encoding, fragment_sink.rows, *spill_encoding, spill_sink.rows);

    const auto fragment_events = decode_generation_events(*fragment_encoding, fragment_sink.rows);
    const auto spill_events = decode_generation_events(*spill_encoding, spill_sink.rows);
    REQUIRE(fragment_events == oracle);
    REQUIRE(spill_events == oracle);
    const auto oracle_lookup = canonical_event_lookup_map(oracle);
    REQUIRE(canonical_event_lookup_map(fragment_events) == oracle_lookup);
    REQUIRE(canonical_event_lookup_map(spill_events) == oracle_lookup);

    // The internal all-SIMD backend must traverse the same fragmented index
    // cores and external spill runs without scalar sketch fallback. Encoded
    // bytes are compared after normalising generation identity.
    lazy_sequence<dna4_u64, chunked> lazy_exact{chunked::from_range(range)};
    const auto lazy_exact_view = bioseq::storage::make_lazy_sequence_view(lazy_exact);
    auto exact_fragment_options = test_generation_options(
        range.bp_size(), bioseq::simd::minimizer_algorithm::two_stack_nthash);
    auto exact_fragment_producer =
        index_type::make_generation_replay_producer(lazy_exact_view,
                                                    minimizer_sketch_backend::exact_simd,
                                                    fixed_minimizer_kernels(),
                                                    exact_fragment_options.algorithm_identity,
                                                    exact_fragment_options.window_size,
                                                    exact_fragment_options.kmer_size,
                                                    fixed_alphabet_code<::seq::dna4>());
    collecting_minimizer_row_sink exact_fragment_sink;
    exact_fragment_options.generation_id += 2;
    auto exact_fragment_encoding = encode_minimizer_generation(
        exact_fragment_producer, exact_fragment_sink, exact_fragment_options);
    REQUIRE(exact_fragment_encoding.has_value());
    const minimizer_fragment_generation_execution_receipt exact_execution =
        exact_fragment_producer.execution_stats();
    REQUIRE(exact_execution.is_accepted());
    REQUIRE(exact_execution.selected_backend == minimizer_sketch_backend::exact_simd);
    REQUIRE(exact_execution.scalar_core_runs == 0);
    REQUIRE(exact_execution.exact_simd_core_runs > 0);
    REQUIRE(exact_execution.bounded_nthash_core_runs == 0);
    REQUIRE(exact_execution.vulkan_core_runs == 0);
    REQUIRE(exact_execution.exact_simd_stats_present);
    REQUIRE(exact_execution.exact_simd_stats.is_internally_consistent(true));

    fake_fragment_storage<> exact_spill_storage;
    auto exact_spill_options = test_generation_options(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::two_stack_nthash,
        range.bp_size());
    auto exact_spilled = spill_run_index_type::spill_generation_replay_runs_serial(
        exact_spill_storage,
        lazy_exact_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        exact_spill_options.algorithm_identity,
        fragment_bp,
        k_minimizer_preferred_segment_span_bp,
        nullptr,
        fixed_alphabet_code<::seq::dna4>());
    REQUIRE(exact_spilled.has_value());
    REQUIRE(exact_spilled->run_count() == scalar_spill_run_count);
    auto exact_spill_producer = spill_run_index_type::make_generation_replay_producer(
        exact_spill_storage, range.bp_size(), std::move(*exact_spilled));
    collecting_minimizer_row_sink exact_spill_sink;
    exact_spill_options.generation_id += 3;
    auto exact_spill_encoding =
        encode_minimizer_generation(exact_spill_producer, exact_spill_sink, exact_spill_options);
    REQUIRE(exact_spill_encoding.has_value());

    require_encoded_generations_equal_normalized(
        *fragment_encoding, fragment_sink.rows, *exact_fragment_encoding, exact_fragment_sink.rows);
    require_encoded_generations_equal_normalized(
        *spill_encoding, spill_sink.rows, *exact_spill_encoding, exact_spill_sink.rows);
    REQUIRE(decode_generation_events(*exact_fragment_encoding, exact_fragment_sink.rows) == oracle);
    REQUIRE(decode_generation_events(*exact_spill_encoding, exact_spill_sink.rows) == oracle);
    require_complete_generation_rows(*fragment_encoding, fragment_sink.rows);
    require_complete_generation_rows(*spill_encoding, spill_sink.rows);
    require_complete_generation_rows(*exact_fragment_encoding, exact_fragment_sink.rows);
    require_complete_generation_rows(*exact_spill_encoding, exact_spill_sink.rows);
}

TEST_CASE("reference fragment and sequence producers execute the generated K20 owner",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 128u;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using fragment_index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    using sequence_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;

    auto data = make_dna4(4096u, 0x4B32305245414459ULL);
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);

    auto fragment_options = test_generation_options<::seq::dna4, std::uint64_t>(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::simd_minimap2_reference,
        k_minimizer_fragment_algorithm_position_extent_bp,
        20u,
        15u);
    REQUIRE(fragment_options.algorithm_identity.physical_kmer_size == 20u);
    REQUIRE(fragment_options.algorithm_identity.hash_width == minimizer_scalar_width::u64);
    REQUIRE(fragment_options.algorithm_identity.effective_algorithm ==
            bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
    REQUIRE(fragment_options.algorithm_identity.highway_owner_id !=
            bioseq::minimizers::minimizer_highway_owner_id::none);
    auto fragment_producer = fragment_index_type::make_generation_replay_producer(
        lazy_view,
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
    REQUIRE(fragment_generation->manifest.algorithm_identity ==
            fragment_options.algorithm_identity);
    const auto fragment_execution = fragment_producer.execution_stats();
    REQUIRE(fragment_execution.is_accepted());
    REQUIRE(fragment_execution.exact_simd_core_runs > 0u);
    REQUIRE_FALSE(fragment_execution.exact_simd_stats_present);
    REQUIRE(fragment_execution.reference_simd_stats_present);
    REQUIRE(fragment_execution.reference_simd_stats.is_internally_consistent(
        true, sizeof(std::uint64_t)));
    require_complete_generation_rows(*fragment_generation, fragment_sink.rows);

    fake_fragment_storage<> spill_storage;
    auto sequence_options = test_grouped_soa_generation_options<::seq::dna4, std::uint64_t>(
        range.bp_size(),
        65'534u,
        bioseq::simd::minimizer_algorithm::simd_minimap2_reference,
        range.bp_size(),
        20u,
        15u);
    auto spill = sequence_index_type::spill_generation_replay_runs_serial(
        spill_storage,
        lazy_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        sequence_options.algorithm_identity,
        fragment_bp,
        k_minimizer_preferred_segment_span_bp,
        nullptr,
        fixed_alphabet_code<::seq::dna4>());
    REQUIRE(spill.has_value());
    REQUIRE(spill->execution_receipt().algorithm_identity ==
            sequence_options.algorithm_identity);
    REQUIRE(spill->execution_receipt().is_spill_valid());
    REQUIRE_FALSE(spill->exact_simd_stats_available());
    REQUIRE(spill->reference_simd_stats_available());
    REQUIRE(spill->reference_simd_stats().is_internally_consistent(
        true, sizeof(std::uint64_t)));
    auto sequence_producer = sequence_index_type::make_generation_replay_producer(
        spill_storage, range.bp_size(), std::move(*spill));
    collecting_minimizer_row_sink sequence_sink;
    ++sequence_options.generation_id;
    const auto sequence_generation = encode_minimizer_generation(
        sequence_producer, sequence_sink, sequence_options);
    REQUIRE(sequence_generation.has_value());
    REQUIRE(sequence_generation->manifest.algorithm_identity ==
            sequence_options.algorithm_identity);
    require_complete_generation_rows(*sequence_generation, sequence_sink.rows);
}

TEST_CASE("DNA5 reference-labelled generation executes its generated native producer",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 128u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using fragment_index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    std::vector<seq::dna5> data(4096u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
    }
    dna5_range_t range(data);
    lazy_sequence<dna5_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto options = test_generation_options<::seq::dna5, std::uint64_t>(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::simd_minimap2_reference,
        k_minimizer_fragment_algorithm_position_extent_bp,
        15u,
        10u);
    REQUIRE(options.algorithm_identity.requested_algorithm ==
            bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
    REQUIRE(options.algorithm_identity.effective_algorithm ==
            bioseq::simd::minimizer_algorithm::simd_minimap2_reference);
    const auto selected_backend = minimizer_sketch_backend_for_effective_algorithm(
        options.algorithm_identity.effective_algorithm);
    REQUIRE(selected_backend == minimizer_sketch_backend::exact_simd);

    auto producer = fragment_index_type::make_generation_replay_producer(
        lazy_view,
        *selected_backend,
        fixed_minimizer_kernels(),
        options.algorithm_identity,
        options.window_size,
        options.kmer_size,
        fixed_alphabet_code<::seq::dna5>());
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.algorithm_identity == options.algorithm_identity);
    const auto receipt = producer.execution_stats();
    REQUIRE(receipt.is_accepted());
    REQUIRE(receipt.selected_backend == minimizer_sketch_backend::exact_simd);
    REQUIRE(receipt.scalar_core_runs == 0u);
    REQUIRE(receipt.exact_simd_core_runs > 0u);
    REQUIRE_FALSE(receipt.exact_simd_stats_present);
    REQUIRE(receipt.reference_simd_stats_present);
    REQUIRE(receipt.reference_simd_stats.is_internally_consistent(
        true, sizeof(std::uint64_t)));
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("reference fragment halos retain the generated L15 owner",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 1024u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using fragment_index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;

    auto data = make_dna4(33'000u, 0x4C313548414C4FULL);
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto options = test_generation_options<::seq::dna4, std::uint64_t>(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::simd_minimap2_reference,
        k_minimizer_fragment_algorithm_position_extent_bp,
        15u,
        10u);
    REQUIRE(options.algorithm_identity.record_position_log2 == 15u);
    REQUIRE(options.algorithm_identity.highway_owner_id !=
            bioseq::minimizers::minimizer_highway_owner_id::none);

    auto producer = fragment_index_type::make_generation_replay_producer(
        lazy_view,
        minimizer_sketch_backend::exact_simd,
        fixed_minimizer_kernels(),
        options.algorithm_identity,
        options.window_size,
        options.kmer_size,
        fixed_alphabet_code<::seq::dna4>());
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.algorithm_identity == options.algorithm_identity);
    const auto receipt = producer.execution_stats();
    REQUIRE(receipt.is_accepted());
    REQUIRE(receipt.reference_simd_stats_present);
    REQUIRE(receipt.reference_simd_stats.kernel_calls > receipt.exact_simd_core_runs);
    REQUIRE(receipt.reference_simd_stats.is_internally_consistent(
        true, sizeof(std::uint64_t)));
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("grouped-SoA spill-run exact SIMD binds the canonical shape profile",
          "[component:bioseq][layer:unit][feature:storage][resource:highway]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 128;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(18013, 0x4D5053464D543556ULL);
    range_t range(data);

    const auto spill_exact = [&range](fake_fragment_storage<>& storage) {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        return spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::exact_simd,
            fixed_minimizer_kernels(),
            test_generation_algorithm_identity<::seq::dna4, std::uint64_t>(
                bioseq::simd::minimizer_algorithm::two_stack_nthash,
                range.bp_size()),
            fragment_bp,
            k_minimizer_preferred_segment_span_bp,
            nullptr,
            fixed_alphabet_code<::seq::dna4>());
    };

    SECTION("canonical grouped-SoA profile is published with native SIMD stats") {
        fake_fragment_storage<> storage;
        auto spill = spill_exact(storage);
        REQUIRE(spill.has_value());
        REQUIRE(spill->execution_receipt().is_spill_valid());

        const auto options = test_grouped_soa_generation_options(
            range.bp_size(),
            65'534u,
            bioseq::simd::minimizer_algorithm::two_stack_nthash,
            range.bp_size());
        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto encoded = spill_run_index_type::encode_persisted_generation(
            storage, sink, std::move(*spill), options, {}, &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(encoded->manifest.layout == minimizer_generation_layout::grouped_soa_segment_local);
        REQUIRE(encoded->manifest.alphabet_code == k_minimizer_persisted_dna4_alphabet_code);
        REQUIRE(encoded->manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision);
        REQUIRE(encoded->manifest.max_occ_cap == options.max_occ_cap);

        const auto canonical_profile =
            derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity,
                                                 options.alphabet_code,
                                                 options.kmer_size,
                                                 encoded->manifest.position_segment_span_bp,
                                                 options.max_occ_cap);
        REQUIRE(canonical_profile.has_value());
        REQUIRE(encoded->directory.profile() == *canonical_profile);
        const auto manifest_profile = derive_grouped_soa_minimizer_profile(encoded->manifest);
        REQUIRE(manifest_profile.has_value());
        REQUIRE(*manifest_profile == *canonical_profile);

        REQUIRE(execution.is_internally_consistent());
        REQUIRE(execution.generation_layout == options.layout);
        REQUIRE(execution.selected_backend == minimizer_sketch_backend::exact_simd);
        REQUIRE(execution.scalar_core_runs == 0u);
        REQUIRE(execution.exact_simd_core_runs > 0u);
        REQUIRE(execution.exact_simd_stats_available);
        REQUIRE(execution.exact_simd_stats.is_internally_consistent(true));
        REQUIRE(execution.bounded_nthash_core_runs == 0u);
        REQUIRE(execution.vulkan_core_runs == 0u);
        REQUIRE(execution.replay_scan_count == 3u);
        REQUIRE(execution.replay_merge_pass_count == 3u);
        REQUIRE(execution.replay_completed_scan_count == 3u);
        REQUIRE(execution.replay_failed_scan_count == 0u);
        REQUIRE(execution.replay_visited_event_count == 3u * execution.spilled_event_count);
        REQUIRE(sink.grouped_soa_payload_write_calls > 0u);
        REQUIRE(sink.grouped_soa_row_crc_bytes == encoded->manifest.payload_bytes);
        require_complete_generation_rows(*encoded, sink.rows);
    }

    const auto require_profile_rejection =
        [&spill_exact](grouped_soa_generation_options options) {
            fake_fragment_storage<> storage;
            auto spill = spill_exact(storage);
            REQUIRE(spill.has_value());
            collecting_minimizer_row_sink sink;
            minimizer_sequence_generation_execution_receipt execution;
            require_wire_error(spill_run_index_type::encode_persisted_generation(
                                   storage, sink, std::move(*spill), options, {}, &execution),
                               minimizer_generation_encoding_error::storage_contract_mismatch);
            REQUIRE(sink.rows.empty());
            REQUIRE_FALSE(execution.is_internally_consistent());
        };

    SECTION("noncanonical DNA4 cardinality cannot masquerade as pack code") {
        auto options = test_grouped_soa_generation_options(range.bp_size());
        options.alphabet_code = 4u;
        require_profile_rejection(options);
    }

    SECTION("invalid grouped-SoA count profile has no alternate fallback") {
        auto options = test_grouped_soa_generation_options(range.bp_size());
        options.max_occ_cap = (std::numeric_limits<std::uint64_t>::max)();
        require_profile_rejection(options);
    }

    SECTION("unsupported spill-run layouts remain rejected") {
        auto options = test_grouped_soa_generation_options(range.bp_size());
        options.layout = minimizer_generation_layout::invalid;
        require_profile_rejection(options);
    }
}

TEST_CASE("spill-run SHA-256 binding matches standard known vectors and split updates",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto require_hex = [](const direct_minimizer_content_digest& digest,
                                std::string_view expected) {
        REQUIRE(expected.size() == digest.size() * 2u);
        const auto nibble = [](char value) -> std::uint8_t {
            if (value >= '0' && value <= '9') {
                return static_cast<std::uint8_t>(value - '0');
            }
            return static_cast<std::uint8_t>(value - 'a' + 10);
        };
        for (std::size_t index = 0; index < digest.size(); ++index) {
            const std::uint8_t value = static_cast<std::uint8_t>(
                (nibble(expected[index * 2u]) << 4u) | nibble(expected[index * 2u + 1u]));
            REQUIRE(std::to_integer<std::uint8_t>(digest[index]) == value);
        }
    };

    bioseq::common::sha256_hasher empty;
    require_hex(empty.finalize(),
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855");

    const auto bytes = [](std::string_view value) {
        return std::as_bytes(std::span{value.data(), value.size()});
    };
    bioseq::common::sha256_hasher contiguous;
    contiguous.update(bytes("abc"));
    const direct_minimizer_content_digest contiguous_digest = contiguous.finalize();
    require_hex(contiguous_digest,
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad");

    bioseq::common::sha256_hasher split;
    split.update(bytes("a"));
    split.update(bytes("b"));
    split.update(bytes("c"));
    REQUIRE(split.finalize() == contiguous_digest);

    //  promoted helpers: lowercase-hex rendering and the one-shot convenience
    // share the same digest and match the canonical "abc" vector.
    REQUIRE(bioseq::common::to_hex(contiguous_digest) == "ba7816bf8f01cfea414140de5dae2223"
                                                         "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(bioseq::common::sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223"
                                                 "b00361a396177a9cb410ff61f20015ad");
    REQUIRE(bioseq::common::sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb924"
                                              "27ae41e4649b934ca495991b7852b855");

    // update_little_endian defines the byte order used by the spill-run digest.
    bioseq::common::sha256_hasher little_endian;
    little_endian.update_little_endian(0x0807060504030201ull);
    bioseq::common::sha256_hasher raw_bytes;
    const std::array<std::byte, 8> le_expected{std::byte{0x01},
                                               std::byte{0x02},
                                               std::byte{0x03},
                                               std::byte{0x04},
                                               std::byte{0x05},
                                               std::byte{0x06},
                                               std::byte{0x07},
                                               std::byte{0x08}};
    raw_bytes.update(le_expected);
    REQUIRE(little_endian.finalize() == raw_bytes.finalize());
}

TEST_CASE("inactive spill-run storage incarnation rejects duplicate runs ABA and reuse",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::size_t fragment_units = 64;
    constexpr std::uint64_t fragment_bp = fragment_units * 32u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, fragment_units>;
    using spill_run_index_type = direct_minimizer_bucket_index<fake_fragment_storage<>>;
    auto data = make_dna4(4096, 0x494E4341524E4154ULL);
    range_t range(data);
    const auto spill_identity = test_generation_algorithm_identity<::seq::dna4, std::uint64_t>(
        bioseq::simd::minimizer_algorithm::scalar, range.bp_size());

    SECTION("duplicate physical run advances the storage mutation revision") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding before =
            storage.segment_spill_storage_binding();
        REQUIRE_FALSE(storage.segment_runs_.empty());
        const auto segment = storage.segment_runs_.begin()->first;
        REQUIRE_FALSE(storage.segment_runs_.begin()->second.empty());
        const auto duplicate = storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, duplicate);
        const direct_minimizer_spill_storage_binding after =
            storage.segment_spill_storage_binding();
        REQUIRE(after.generation_token == before.generation_token);
        REQUIRE(after.mutation_revision == before.mutation_revision + 1u);
        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        require_wire_error(spill_run_index_type::encode_persisted_generation(
                               storage,
                               sink,
                               std::move(*spill),
                               test_grouped_soa_generation_options(range.bp_size()),
                               {},
                               &execution),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE_FALSE(execution.is_internally_consistent());
        REQUIRE(sink.rows.empty());
    }

    SECTION("wrong storage and moved-from authority cannot be laundered") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> original;
        fake_fragment_storage<> replacement;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        collecting_minimizer_row_sink wrong_sink;
        require_wire_error(spill_run_index_type::encode_persisted_generation(
                               replacement,
                               wrong_sink,
                               std::move(*spill),
                               test_grouped_soa_generation_options(range.bp_size())),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(wrong_sink.rows.empty());

        collecting_minimizer_row_sink accepted_sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto accepted = spill_run_index_type::encode_persisted_generation(
            original,
            accepted_sink,
            std::move(*spill),
            test_grouped_soa_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(accepted.has_value());
        REQUIRE(execution.is_internally_consistent());
        REQUIRE(spill->run_count() == 0);
        REQUIRE(spill->target_length() == 0);
        REQUIRE(spill->position_segment_span_bp() == 0);
        REQUIRE_FALSE(spill->execution_receipt().is_spill_valid());

        collecting_minimizer_row_sink reused_sink;
        require_wire_error(spill_run_index_type::encode_persisted_generation(
                               original,
                               reused_sink,
                               std::move(*spill),
                               test_grouped_soa_generation_options(range.bp_size())),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(reused_sink.rows.empty());
    }

    SECTION("release and new begin invalidate even an empty authority") {
        std::vector<seq::dna4> empty_data;
        range_t empty_range(empty_data);
        lazy_sequence<dna4_u64, chunked> empty_lazy{chunked::from_range(empty_range)};
        const auto empty_lazy_view = bioseq::storage::make_lazy_sequence_view(empty_lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            empty_lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding issued =
            storage.segment_spill_storage_binding();
        REQUIRE(issued.valid());
        storage.release_runs_noexcept();
        const direct_minimizer_spill_storage_binding released =
            storage.segment_spill_storage_binding();
        REQUIRE(released.generation_token == issued.generation_token);
        REQUIRE(released.mutation_revision == issued.mutation_revision + 1u);
        const direct_minimizer_spill_storage_binding replacement =
            storage.begin_segment_spill_generation();
        REQUIRE(replacement.valid());
        REQUIRE(replacement.generation_token != issued.generation_token);
        collecting_minimizer_row_sink sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                storage, sink, std::move(*spill), test_grouped_soa_generation_options(0)),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving storage cannot preserve address-bound authority") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> original;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding issued =
            original.segment_spill_storage_binding();
        fake_fragment_storage<> moved{std::move(original)};
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        collecting_minimizer_row_sink sink;
        require_wire_error(spill_run_index_type::encode_persisted_generation(
                               moved,
                               sink,
                               std::move(*spill),
                               test_grouped_soa_generation_options(range.bp_size())),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("moving an empty spill invalidates the moved-from authority") {
        std::vector<seq::dna4> empty_data;
        range_t empty_range(empty_data);
        lazy_sequence<dna4_u64, chunked> empty_lazy{chunked::from_range(empty_range)};
        const auto empty_lazy_view = bioseq::storage::make_lazy_sequence_view(empty_lazy);
        fake_fragment_storage<> original;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            original,
            empty_lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        REQUIRE(original.segment_spill_storage_binding().valid());

        fake_fragment_storage<> moved{std::move(original)};
        REQUIRE_FALSE(original.segment_spill_storage_binding().valid());
        REQUIRE_FALSE(moved.segment_spill_storage_binding().valid());
        collecting_minimizer_row_sink sink;
        require_wire_error(
            spill_run_index_type::encode_persisted_generation(
                original, sink, std::move(*spill), test_grouped_soa_generation_options(0)),
            minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(sink.rows.empty());
    }

    SECTION("representation-only run compaction preserves logical incarnation") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        const direct_minimizer_spill_storage_binding before =
            storage.segment_spill_storage_binding();
        const std::uint64_t physical_runs_before = storage.segment_spill_run_count();
        REQUIRE(physical_runs_before > storage.segment_runs_.size());

        storage.compact_segment_spill_representation_preserving();
        REQUIRE(storage.segment_spill_storage_binding() == before);
        REQUIRE(storage.segment_spill_run_count() < physical_runs_before);

        collecting_minimizer_row_sink sink;
        minimizer_sequence_generation_execution_receipt execution;
        const auto encoded = spill_run_index_type::encode_persisted_generation(
            storage,
            sink,
            std::move(*spill),
            test_grouped_soa_generation_options(range.bp_size()),
            {},
            &execution);
        REQUIRE(encoded.has_value());
        REQUIRE(execution.is_internally_consistent());
        REQUIRE(execution.allows_ready_release(storage.segment_spill_storage_binding()));
        const auto segment = storage.segment_runs_.begin()->first;
        const auto post_encode_splice = storage.segment_runs_.begin()->second.front();
        storage.spill_sorted_segment_run(segment, post_encode_splice);
        REQUIRE_FALSE(execution.allows_ready_release(storage.segment_spill_storage_binding()));
        require_complete_generation_rows(*encoded, sink.rows);
    }

    SECTION("bound producer and consumed spill are single-use") {
        lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
        const auto lazy_view = bioseq::storage::make_lazy_sequence_view(lazy);
        fake_fragment_storage<> storage;
        auto spill = spill_run_index_type::spill_generation_replay_runs_serial(
            storage,
            lazy_view,
            minimizer_sketch_backend::scalar,
            nullptr,
            spill_identity,
            fragment_bp);
        REQUIRE(spill.has_value());
        auto producer = spill_run_index_type::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink first_sink;
        const auto first = encode_minimizer_generation(
            producer, first_sink, test_generation_options(range.bp_size()));
        REQUIRE(first.has_value());
        collecting_minimizer_row_sink second_sink;
        require_wire_error(encode_minimizer_generation(
                               producer, second_sink, test_generation_options(range.bp_size())),
                           minimizer_generation_encoding_error::segment_span);

        auto moved_from_producer = spill_run_index_type::make_generation_replay_producer(
            storage, range.bp_size(), std::move(*spill));
        collecting_minimizer_row_sink moved_from_sink;
        require_wire_error(encode_minimizer_generation(moved_from_producer,
                                                       moved_from_sink,
                                                       test_generation_options(range.bp_size())),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
        REQUIRE(moved_from_sink.rows.empty());
    }
}

TEST_CASE("inactive spill-run storage incarnation counters fail closed at wrap",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    // The uncommon maximum chunk parameter gives this test an isolated static
    // token namespace, independent of every ordinary fake_fragment_storage<>.
    using wrap_storage_type = fake_fragment_storage<123457u>;

    wrap_storage_type::set_next_segment_spill_generation_token_for_test(
        (std::numeric_limits<std::uint64_t>::max)());
    wrap_storage_type last_valid;
    const direct_minimizer_spill_storage_binding maximum =
        last_valid.begin_segment_spill_generation();
    REQUIRE(maximum.valid());
    REQUIRE(maximum.generation_token == (std::numeric_limits<std::uint64_t>::max)());
    REQUIRE(maximum.mutation_revision == 0);

    wrap_storage_type exhausted;
    REQUIRE_FALSE(exhausted.begin_segment_spill_generation().valid());
    REQUIRE_FALSE(exhausted.begin_segment_spill_generation().valid());

    last_valid.set_segment_spill_mutation_revision_for_test(
        (std::numeric_limits<std::uint64_t>::max)());
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 1> run{{
        {1u, 2u},
    }};
    (void)last_valid.spill_sorted_segment_run(0u, run);
    REQUIRE_FALSE(last_valid.segment_spill_storage_binding().valid());
}

TEST_CASE("minimizer index facades preserve biological rejection precedence",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto data = make_dna4(257u, 0x399500u);
    range_t range(data);
    auto owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto view = bioseq::storage::make_lazy_sequence_view(owner);

    sketch_collector exact_collector;
    try {
        (void)collect_exact_simd_index_range_or_throw(exact_collector,
                                                      view,
                                                      fixed_alphabet_code<::seq::dna4>(),
                                                      2u,
                                                      1u,
                                                      10u,
                                                      15u,
                                                      0u,
                                                      nullptr,
                                                      "precedence probe");
        FAIL("Exact lazy facade accepted a reversed range");
    } catch (const exact_simd_index_runtime_error& error) {
        REQUIRE(error.status() == exact_simd_index_status::invalid_range);
    }

    sketch_collector bounded_collector;
    const auto bounded_lazy = collect_packed_bounded_nthash_index_lazy(
        view, fixed_alphabet_code<::seq::dna4>(), 2u, 1u, 0u, 10u, 15u, nullptr, bounded_collector);
    REQUIRE(bounded_lazy.status == packed_bounded_nthash_index_status::invalid_range);
    REQUIRE(bounded_collector.extract_sketches_data().empty());

    const exact_simd_index_bridge_request unsupported_alphabet{
        .alphabet_code = 77u,
        .bits_per_symbol = 2u,
        .storage_unit_bytes = sizeof(std::uint32_t),
    };
    REQUIRE(collect_exact_simd_index_erased(unsupported_alphabet).status ==
            exact_simd_index_status::unsupported_alphabet);
    auto unsupported_storage = unsupported_alphabet;
    unsupported_storage.alphabet_code = fixed_alphabet_code<::seq::dna4>();
    unsupported_storage.storage_unit_bytes = 3u;
    REQUIRE(collect_exact_simd_index_erased(unsupported_storage).status ==
            exact_simd_index_status::unsupported_storage);
    auto missing_exact_state = unsupported_storage;
    missing_exact_state.storage_unit_bytes = sizeof(std::uint32_t);
    REQUIRE(collect_exact_simd_index_erased(missing_exact_state).status ==
            exact_simd_index_status::invalid_state);

    packed_bounded_nthash_index_bridge_request bounded_request{
        .kernels = nullptr,
        .sequence = fixed_packed_sequence_view(range),
        .sequence_context = &range,
        .collector_context = &bounded_collector,
        .storage_unit_bytes = sizeof(std::uint64_t),
        .letters_per_unit = range_t::traits::letters_per_unit,
        .storage_units = range.size(),
        .local_bp_size = range.bp_size(),
        .window_size = 10u,
        .kmer_size = 15u,
    };
    auto invalid_configuration = bounded_request;
    invalid_configuration.window_size = 0u;
    invalid_configuration.sequence.alphabet_code = 77u;
    invalid_configuration.storage_unit_bytes = sizeof(std::uint16_t);
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_configuration).status ==
            packed_bounded_nthash_index_status::invalid_configuration);

    auto invalid_alphabet = bounded_request;
    invalid_alphabet.sequence.alphabet_code =
        bioseq::minimizers::k_unbound_packed_alphabet_code;
    invalid_alphabet.storage_unit_bytes = sizeof(std::uint16_t);
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_alphabet).status ==
            packed_bounded_nthash_index_status::unsupported_alphabet);

    auto mismatched_dna5_encoding = bounded_request;
    // Registered alphabet geometry is validated by the typed kernel. Supply
    // that kernel so a missing-state rejection cannot mask this malformed view.
    mismatched_dna5_encoding.kernels = fixed_minimizer_kernels();
    mismatched_dna5_encoding.sequence.alphabet_code = fixed_alphabet_code<::seq::dna5>();
    REQUIRE(collect_packed_bounded_nthash_index_erased(mismatched_dna5_encoding).status ==
            packed_bounded_nthash_index_status::invalid_range);

    auto invalid_storage = bounded_request;
    invalid_storage.storage_unit_bytes = sizeof(std::uint16_t);
    invalid_storage.sequence.storage_unit_bytes = sizeof(std::uint16_t);
    invalid_storage.letters_per_unit = 8u;
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_storage).status ==
            packed_bounded_nthash_index_status::unsupported_storage);

    auto invalid_range = bounded_request;
    invalid_range.sequence.end_symbol = range.bp_size() + 1u;
    REQUIRE(collect_packed_bounded_nthash_index_erased(invalid_range).status ==
            packed_bounded_nthash_index_status::invalid_range);
    REQUIRE(collect_packed_bounded_nthash_index_erased(bounded_request).status ==
            packed_bounded_nthash_index_status::invalid_state);
}

TEST_CASE(
    "bounded ntHash index stats aggregation rejects mixed source widths without rewriting identity",
    "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    packed_bounded_nthash_index_execution_stats u32{
        .scheme_id = k_packed_bounded_nthash_kernel_scheme_id,
        .kmer_size = 15u,
        .window_size = 10u,
        .nthash_rotation = 7u,
        .source_storage_unit_bytes = sizeof(std::uint32_t),
        .segments_combined = 1u,
        .logical_streams = 8u,
        .physical_vector_lanes = 4u,
        .vector_registers_per_step = 2u,
        .positions_are_segment_local = true,
    };
    auto second_u32 = u32;
    auto same_width = u32;
    same_width.merge(second_u32);
    REQUIRE_FALSE(same_width.aggregation_mismatch);
    REQUIRE(same_width.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(same_width.segments_combined == 2u);

    auto u64 = u32;
    u64.source_storage_unit_bytes = sizeof(std::uint64_t);
    auto mixed_width = u32;
    mixed_width.merge(u64);
    REQUIRE(mixed_width.aggregation_mismatch);
    REQUIRE(mixed_width.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE_FALSE(mixed_width.is_internally_consistent(false));
}

TEST_CASE("bounded ntHash lazy index admits resident u64 through one canonical u32 window",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    auto data = make_dna4(4096u, 0x5533325041504552ULL);
    range_t range(data);
    auto owner = bioseq::storage::make_resident_lazy_sequence(range);
    const auto view = bioseq::storage::make_lazy_sequence_view(owner);
    REQUIRE(view.try_resident_packed(0u, view.bp_size(), sizeof(std::uint64_t)).has_value());
    REQUIRE_FALSE(view.try_resident_packed(0u, view.bp_size(), sizeof(std::uint32_t)).has_value());

    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const std::uint64_t admission_before = admission.reserved_bytes();
    sketch_collector collector;
    std::uint64_t materialized_window_bp = 0u;
    const auto result = collect_packed_bounded_nthash_index_lazy(view,
                                                                 fixed_alphabet_code<::seq::dna4>(),
                                                                 0u,
                                                                 view.bp_size(),
                                                                 view.bp_size(),
                                                                 10u,
                                                                 15u,
                                                                 fixed_minimizer_kernels(),
                                                                 collector,
                                                                 &materialized_window_bp);
    if (result.status == packed_bounded_nthash_index_status::unsupported_vector_shape) {
        REQUIRE(admission.reserved_bytes() == admission_before);
        SKIP("the compiled Highway target cannot execute packed bounded ntHash ntHash");
    }
    REQUIRE(result.status == packed_bounded_nthash_index_status::ok);
    REQUIRE(result.stats.is_internally_consistent(true));
    REQUIRE(result.stats.source_storage_unit_bytes == sizeof(std::uint32_t));
    REQUIRE(materialized_window_bp == view.bp_size());
    REQUIRE_FALSE(collector.extract_sketches_data().empty());
    REQUIRE(admission.reserved_bytes() == admission_before);
}

TEST_CASE("bounded ntHash lazy index admits native and fragmented DNA5 for both registered profiles",
          "[component:bioseq][layer:unit][feature:index][resource:highway]") {
    using namespace bioseq::indexing;
    const std::array<std::pair<std::uint16_t, std::uint16_t>, 2> profiles{{
        {10u, 15u},
        {15u, 20u},
    }};
    std::vector<seq::dna5> data(4'123u);
    for (std::size_t index = 0u; index < data.size(); ++index) {
        data[index].assign_rank(static_cast<std::uint8_t>(index % 5u));
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();

    const auto check_resident = [&]<class p_storage_unit>() {
        using sequence_type = sequence_custom_alphabet<seq::dna5, p_storage_unit>;
        using sequence_range = compact_sequence_range<sequence_type>;
        sequence_range range(data);
        auto owner = bioseq::storage::make_resident_lazy_sequence(range);
        const auto view = bioseq::storage::make_lazy_sequence_view(owner);
        REQUIRE(view.try_resident_packed(
            0u, view.bp_size(), sizeof(p_storage_unit)).has_value());
        for (const auto [window_size, kmer_size] : profiles) {
            CAPTURE(sizeof(p_storage_unit), window_size, kmer_size);
            const std::uint64_t admission_before = admission.reserved_bytes();
            sketch_collector collector;
            std::uint64_t materialized_window_bp = 1u;
            const auto result = collect_packed_bounded_nthash_index_lazy(
                view,
                fixed_alphabet_code<::seq::dna5>(),
                0u,
                view.bp_size(),
                view.bp_size(),
                window_size,
                kmer_size,
                fixed_minimizer_kernels(),
                collector,
                &materialized_window_bp);
            if (result.status ==
                packed_bounded_nthash_index_status::unsupported_vector_shape) {
                REQUIRE(admission.reserved_bytes() == admission_before);
                SKIP("the compiled Highway target cannot execute packed bounded ntHash");
            }
            REQUIRE(result.status == packed_bounded_nthash_index_status::ok);
            REQUIRE(result.stats.is_internally_consistent(true));
            REQUIRE(result.stats.window_size == window_size);
            REQUIRE(result.stats.kmer_size == kmer_size);
            REQUIRE(result.stats.source_storage_unit_bytes == sizeof(p_storage_unit));
            REQUIRE(result.stats.fallback_calls == 0u);
            REQUIRE(materialized_window_bp == 0u);
            REQUIRE_FALSE(collector.extract_sketches_data().empty());
            for (const auto& minimizer : collector.extract_sketches_data()) {
                REQUIRE(minimizer.last_pos() < data.size());
            }
            REQUIRE(admission.reserved_bytes() == admission_before);
        }
    };
    check_resident.template operator()<std::uint32_t>();
    check_resident.template operator()<std::uint64_t>();

    using sequence_type = sequence_custom_alphabet<seq::dna5, std::uint64_t>;
    using sequence_range = compact_sequence_range<sequence_type>;
    using chunked_scanner =
        bioseq::storage::chunked_span_sequence_scanner<std::uint64_t, 64u>;
    sequence_range range(data);
    for (const auto [window_size, kmer_size] : profiles) {
        CAPTURE(window_size, kmer_size);
        const std::uint64_t admission_before = admission.reserved_bytes();
        sketch_collector collector;
        std::uint64_t materialized_window_bp = 0u;
        {
            bioseq::storage::lazy_sequence<sequence_type, chunked_scanner> fragmented{
                chunked_scanner::from_range(range)};
            const auto view = bioseq::storage::make_lazy_sequence_view(fragmented);
            const auto result = collect_packed_bounded_nthash_index_lazy(
                view,
                fixed_alphabet_code<::seq::dna5>(),
                0u,
                view.bp_size(),
                view.bp_size(),
                window_size,
                kmer_size,
                fixed_minimizer_kernels(),
                collector,
                &materialized_window_bp);
            if (result.status ==
                packed_bounded_nthash_index_status::unsupported_vector_shape) {
                REQUIRE(admission.reserved_bytes() == admission_before);
                SKIP("the compiled Highway target cannot execute packed bounded ntHash");
            }
            REQUIRE(result.status == packed_bounded_nthash_index_status::ok);
            REQUIRE(result.stats.is_internally_consistent(true));
            REQUIRE(result.stats.window_size == window_size);
            REQUIRE(result.stats.kmer_size == kmer_size);
            REQUIRE(result.stats.source_storage_unit_bytes == sizeof(std::uint32_t));
            REQUIRE(result.stats.fallback_calls == 0u);
        }
        REQUIRE(materialized_window_bp == range.bp_size());
        REQUIRE_FALSE(collector.extract_sketches_data().empty());
        for (const auto& minimizer : collector.extract_sketches_data()) {
            REQUIRE(minimizer.last_pos() < data.size());
        }
        REQUIRE(admission.reserved_bytes() == admission_before);
    }
}

TEST_CASE("Vulkan fragment build records accepted operation-originated batched stats",
          "[component:bioseq][layer:unit][feature:storage][resource:gpu]") {
    using namespace bioseq::indexing;
    if (!::bioseq::gpu::vulkan::hardware_device_available(
            ::bioseq::gpu::vulkan::runtime::instance())) {
        SUCCEED("no hardware Vulkan device; skipped");
        return;
    }
    constexpr std::size_t source_fragment_units = 64u;
    constexpr std::size_t persisted_fragment_bp = 32'768u;
    using chunked = chunked_span_sequence_scanner<std::uint64_t, source_fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    auto data = make_dna4(98'417u, 0x4D504656554C4B4EULL);
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    fake_fragment_storage<> storage;
    storage.run_buffer_bytes_ = 64u << 20u;
    minimizer_fragment_generation_execution_receipt evidence;
    index_type::build(storage, view, minimizer_sketch_backend::vulkan_segment, nullptr, &evidence);

    REQUIRE_FALSE(evidence.is_accepted());
    REQUIRE(evidence.direct_build_accepted());
    REQUIRE(evidence.selected_backend == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(evidence.backend_bound);
    REQUIRE(evidence.target_length == range.bp_size());
    REQUIRE(evidence.scalar_core_runs == 0u);
    REQUIRE(evidence.exact_simd_core_runs == 0u);
    REQUIRE(evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(evidence.vulkan_core_runs > 0u);
    REQUIRE(evidence.vulkan_stats.is_internally_consistent(true));
    REQUIRE(evidence.vulkan_stats.completed_roundtrips == evidence.vulkan_core_runs);
    const std::size_t expected_fragments = static_cast<std::size_t>(
        (range.bp_size() + persisted_fragment_bp - 1u) / persisted_fragment_bp);
    REQUIRE(storage.number_of_chunks(k_directory_sub_key) == 1u);
    REQUIRE(*storage.template read_chunk<std::int64_t>(k_directory_sub_key, 0u) ==
            static_cast<std::int64_t>(expected_fragments));
}

TEST_CASE("grouped-SoA Vulkan replay batches adjacent persisted fragments without merging them",
          "[component:bioseq][layer:unit][feature:storage][resource:gpu]") {
    using namespace bioseq::indexing;
    if (!::bioseq::gpu::vulkan::hardware_device_available(
            ::bioseq::gpu::vulkan::runtime::instance())) {
        SKIP("no hardware Vulkan device");
    }
    constexpr std::size_t source_fragment_units = 64u;
    constexpr std::uint64_t persisted_fragment_bp = 32'768u;
    using chunked =
        chunked_span_sequence_scanner<std::uint64_t, source_fragment_units>;
    using index_type = minimizers_per_fragment_index<fake_fragment_storage<>>;
    auto data = make_dna4(98'417u, 0x4D50464241544348ULL);
    range_t range(data);
    lazy_sequence<dna4_u64, chunked> lazy{chunked::from_range(range)};
    const auto view = bioseq::storage::make_lazy_sequence_view(lazy);
    auto options = test_generation_options(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
    options.position_segment_span_bp = persisted_fragment_bp;
    auto producer = index_type::make_generation_replay_producer(
        view,
        minimizer_sketch_backend::vulkan_segment,
        nullptr,
        options.algorithm_identity,
        options.window_size,
        options.kmer_size,
        fixed_alphabet_code<::seq::dna4>(),
        std::size_t{64u} << 20u);
    collecting_minimizer_row_sink sink;
    const auto encoded =
        encode_minimizer_generation(producer, sink, options);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded->manifest.position_segment_span_bp == persisted_fragment_bp);

    const std::uint64_t physical_fragments =
        (range.bp_size() + persisted_fragment_bp - 1u)
        / persisted_fragment_bp;
    const auto evidence = producer.execution_stats();
    REQUIRE(evidence.is_accepted());
    REQUIRE(evidence.selected_backend
            == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(evidence.producer_scan_count == 3u);
    REQUIRE(evidence.scalar_core_runs == 0u);
    REQUIRE(evidence.exact_simd_core_runs == 0u);
    REQUIRE(evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(evidence.vulkan_core_runs
            == evidence.producer_scan_count * physical_fragments);
    REQUIRE(evidence.vulkan_stats.is_internally_consistent(true));
    REQUIRE(evidence.vulkan_stats.completed_roundtrips > 0u);
    REQUIRE(evidence.vulkan_stats.completed_roundtrips
            < evidence.vulkan_core_runs);
    REQUIRE(producer.peak_buffered_core_bp() == range.bp_size());
    REQUIRE(producer.peak_active_fragment_runs() == physical_fragments);
    REQUIRE(producer.peak_buffered_events() > 0u);
    require_complete_generation_rows(*encoded, sink.rows);

    auto single_fragment_options = test_generation_options(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
    single_fragment_options.position_segment_span_bp = persisted_fragment_bp;
    auto single_fragment_producer = index_type::make_generation_replay_producer(
        view,
        minimizer_sketch_backend::vulkan_segment,
        nullptr,
        single_fragment_options.algorithm_identity,
        single_fragment_options.window_size,
        single_fragment_options.kmer_size,
        fixed_alphabet_code<::seq::dna4>(),
        0u);
    collecting_minimizer_row_sink single_fragment_sink;
    const auto single_fragment_encoded = encode_minimizer_generation(
        single_fragment_producer,
        single_fragment_sink,
        single_fragment_options);
    REQUIRE(single_fragment_encoded.has_value());
    REQUIRE(single_fragment_encoded->manifest.position_segment_span_bp
            == persisted_fragment_bp);
    const auto single_fragment_evidence =
        single_fragment_producer.execution_stats();
    REQUIRE(single_fragment_evidence.is_accepted());
    REQUIRE(single_fragment_evidence.selected_backend
            == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(single_fragment_evidence.vulkan_core_runs
            == evidence.vulkan_core_runs);
    REQUIRE(single_fragment_evidence.vulkan_stats.completed_roundtrips
            == single_fragment_evidence.vulkan_core_runs);
    require_complete_generation_rows(*single_fragment_encoded,
                                     single_fragment_sink.rows);
    require_encoded_generations_equal_normalized(
        *encoded,
        sink.rows,
        *single_fragment_encoded,
        single_fragment_sink.rows);
    REQUIRE(decode_generation_events(*encoded, sink.rows)
            == decode_generation_events(*single_fragment_encoded,
                                        single_fragment_sink.rows));

    // A persisted segment narrower than the physical fragment can cross a
    // physical boundary. The buffered producer batches complete adjacent
    // segments while the zero-buffer producer retains the one-segment route;
    // both must preserve the identical encoded generation.
    constexpr std::uint64_t unaligned_persisted_fragment_bp = 21'504u;
    auto unaligned_options = test_generation_options(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
    unaligned_options.position_segment_span_bp =
        unaligned_persisted_fragment_bp;
    auto unaligned_producer = index_type::make_generation_replay_producer(
        view,
        minimizer_sketch_backend::vulkan_segment,
        nullptr,
        unaligned_options.algorithm_identity,
        unaligned_options.window_size,
        unaligned_options.kmer_size,
        fixed_alphabet_code<::seq::dna4>(),
        std::size_t{64u} << 20u);
    collecting_minimizer_row_sink unaligned_sink;
    const auto unaligned_encoded = encode_minimizer_generation(
        unaligned_producer, unaligned_sink, unaligned_options);
    REQUIRE(unaligned_encoded.has_value());
    REQUIRE(unaligned_encoded->manifest.position_segment_span_bp
            == unaligned_persisted_fragment_bp);
    const auto unaligned_evidence = unaligned_producer.execution_stats();
    REQUIRE(unaligned_evidence.is_accepted());
    REQUIRE(unaligned_evidence.selected_backend
            == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(unaligned_evidence.producer_scan_count == 3u);
    REQUIRE(unaligned_evidence.scalar_core_runs == 0u);
    REQUIRE(unaligned_evidence.exact_simd_core_runs == 0u);
    REQUIRE(unaligned_evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(unaligned_evidence.vulkan_core_runs > 0u);
    REQUIRE(unaligned_evidence.vulkan_stats.is_internally_consistent(true));
    REQUIRE(unaligned_evidence.vulkan_stats.completed_roundtrips > 0u);
    REQUIRE(unaligned_producer.peak_buffered_core_bp()
            > unaligned_persisted_fragment_bp);
    REQUIRE(unaligned_producer.peak_active_fragment_runs() > 1u);
    require_complete_generation_rows(*unaligned_encoded, unaligned_sink.rows);

    auto unaligned_single_options = test_generation_options(
        range.bp_size(),
        bioseq::simd::minimizer_algorithm::vulkan_minimap_integer);
    unaligned_single_options.position_segment_span_bp =
        unaligned_persisted_fragment_bp;
    auto unaligned_single_producer =
        index_type::make_generation_replay_producer(
            view,
            minimizer_sketch_backend::vulkan_segment,
            nullptr,
            unaligned_single_options.algorithm_identity,
            unaligned_single_options.window_size,
            unaligned_single_options.kmer_size,
            fixed_alphabet_code<::seq::dna4>(),
            0u);
    collecting_minimizer_row_sink unaligned_single_sink;
    const auto unaligned_single_encoded = encode_minimizer_generation(
        unaligned_single_producer,
        unaligned_single_sink,
        unaligned_single_options);
    REQUIRE(unaligned_single_encoded.has_value());
    const auto unaligned_single_evidence =
        unaligned_single_producer.execution_stats();
    REQUIRE(unaligned_single_evidence.is_accepted());
    REQUIRE(unaligned_single_evidence.selected_backend
            == minimizer_sketch_backend::vulkan_segment);
    REQUIRE(unaligned_single_evidence.producer_scan_count == 3u);
    REQUIRE(unaligned_single_evidence.scalar_core_runs == 0u);
    REQUIRE(unaligned_single_evidence.exact_simd_core_runs == 0u);
    REQUIRE(unaligned_single_evidence.bounded_nthash_core_runs == 0u);
    REQUIRE(unaligned_single_evidence.vulkan_core_runs
            == unaligned_evidence.vulkan_core_runs);
    REQUIRE(unaligned_single_evidence.vulkan_stats.is_internally_consistent(
        true));
    REQUIRE(unaligned_evidence.vulkan_stats.completed_roundtrips
            < unaligned_single_evidence.vulkan_stats.completed_roundtrips);
    REQUIRE(unaligned_single_producer.peak_buffered_core_bp()
            <= unaligned_persisted_fragment_bp);
    require_complete_generation_rows(
        *unaligned_single_encoded, unaligned_single_sink.rows);
    require_encoded_generations_equal_normalized(
        *unaligned_encoded,
        unaligned_sink.rows,
        *unaligned_single_encoded,
        unaligned_single_sink.rows);
    REQUIRE(decode_generation_events(*unaligned_encoded, unaligned_sink.rows)
            == decode_generation_events(*unaligned_single_encoded,
                                        unaligned_single_sink.rows));
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
