/**
 * @file reader_test.cpp
 * @brief Checks grouped index reads, row validation, and memory reservations.
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
import bioseq.pipeline.runtime_contract;
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
import bioseq.tests.unit.indexing.minimizer_hashmap.grouped_soa_support;

using namespace seq::literals;

HWY_BEFORE_NAMESPACE();
namespace bioseq::simd::HWY_NAMESPACE {
using namespace minimizer_hashmap_test_support;
using namespace grouped_soa_test_support;

TEST_CASE("grouped-SoA cold reader admits retained positions once",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    // Give each of 2048 query hashes one target position to check that
    // the reader reserves the final position buffer once.
    constexpr std::uint64_t query_count = 2048u;
    constexpr std::uint64_t segment_span = 8u * 1024u * 1024u;
    constexpr std::uint64_t segment_count = 3u;
    constexpr auto retained_bp = [](std::uint64_t index) {
        return (index % segment_count) * segment_span + index / segment_count;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    std::vector<std::uint64_t> queries;
    minimizers.reserve(query_count);
    queries.reserve(query_count);
    for (std::uint64_t index = 0; index < query_count; ++index) {
        // Multiplication by an odd number gives distinct hashes across the
        // 30-bit domain, spreading this fixture over many radix buckets.
        const std::uint64_t hash = ((index + 1u) * 2'654'435'761ULL) & ((1ULL << 30u) - 1u);
        minimizers.emplace_back(hash, retained_bp(index) << 1u);
        queries.push_back(hash);
    }
    std::ranges::sort(queries);

    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(segment_count * segment_span, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == segment_count);

    bioseq::parallelism::memory_admission_controller admission;
    auto reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 queries,
                                                 10u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());

    const auto evidence = reader->memory_stats();
    REQUIRE(evidence.global_position_storage_preparations == 1u);
    REQUIRE(evidence.global_position_storage_allocations == 1u);
    REQUIRE(evidence.position_append_growth_reservations == 0u);
    REQUIRE(evidence.retained_position_count == query_count);
    REQUIRE(evidence.retained_position_capacity == query_count);
    REQUIRE(evidence.query_record_size_bytes == sizeof(std::uint16_t));
    REQUIRE(evidence.query_record_count == query_count);
    REQUIRE(evidence.query_record_capacity >= query_count);
    REQUIRE(evidence.query_record_capacity_bytes ==
            query_count * (sizeof(std::uint16_t) + sizeof(std::uint64_t)));
    REQUIRE(evidence.query_route_radix_bits == 9u);
    REQUIRE(evidence.query_route_radix_bucket_count == 512u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count > 0u);
    REQUIRE(evidence.query_route_radix_nonempty_bucket_count <=
            evidence.query_route_radix_bucket_count);
    REQUIRE(evidence.query_route_radix_max_bucket_size <= 8u);
    REQUIRE(evidence.query_hash_capacity_bytes ==
            query_count * sizeof(std::uint32_t) +
                (evidence.query_route_radix_bucket_count + 1u) * sizeof(std::uint32_t));
    REQUIRE(evidence.released_query_hash_capacity_bytes >= query_count * sizeof(std::uint64_t));
    REQUIRE(evidence.decoded_control_records == query_count);
    REQUIRE(evidence.query_route_control_lookups == evidence.decoded_control_records);
    REQUIRE(evidence.query_route_matches == query_count);
    const std::uint64_t global_lower_bound_probe_ceiling =
        evidence.query_route_control_lookups * (std::bit_width(query_count - 1u) + 1u);
    REQUIRE(evidence.query_route_hash_probes * 2u < global_lower_bound_probe_ceiling);
    REQUIRE(evidence.query_route_hash_probes <= evidence.query_route_control_lookups * 5u);
    REQUIRE(evidence.retained_selected_control_records == query_count);
    REQUIRE(evidence.selected_control_size_bytes == 8u);
    // Stored controls fit in a 16-bit position end and a 32-bit route
    // per query. The reader can release the decoded control bytes.
    REQUIRE(evidence.retained_selected_control_capacity_bytes ==
            query_count * (sizeof(std::uint16_t) + sizeof(std::uint32_t)));
    REQUIRE(evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(evidence.global_position_capacity_bytes ==
            evidence.retained_position_capacity * sizeof(std::uint64_t));
    REQUIRE(evidence.control_payload_capacity_bytes == 0u);
    REQUIRE(evidence.retained_layout_equation_holds());
    REQUIRE(evidence.owned_capacity_bytes == reader->admitted_bytes());
    // Check memory totals at the four reader stage boundaries.
    constexpr std::uint64_t expected_barrier_audits = 4u;
    REQUIRE(evidence.capacity_accounting_passes == expected_barrier_audits);
    REQUIRE_FALSE(evidence.saturated);
    // Hash order differs from source-position order in this permuted fixture.
    const auto first_query = std::ranges::find_if(
        minimizers, [&queries](const auto& entry) { return entry.first == queries.front(); });
    const auto last_query = std::ranges::find_if(
        minimizers, [&queries](const auto& entry) { return entry.first == queries.back(); });
    REQUIRE(first_query != minimizers.end());
    REQUIRE(last_query != minimizers.end());
    REQUIRE(reader->lookup(queries.front()).positions.front() == first_query->second);
    REQUIRE(reader->lookup(queries.back()).positions.front() == last_query->second);

    // Add a reader with one query to check that combined memory counters
    // still agree when readers select different sets of extents.
    const std::array<std::uint64_t, 1> sparse_queries{queries.front()};
    const std::uint64_t sparse_selected_extent_count = static_cast<std::uint64_t>(
        std::ranges::count_if(generation->directory.entries(),
                              [&sparse_queries](const grouped_soa_bucket_directory_entry& entry) {
                                  return entry.lo_hash <= sparse_queries.front() &&
                                         sparse_queries.front() <= entry.hi_hash;
                              }));
    REQUIRE(sparse_selected_extent_count > 0u);
    auto sparse_reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 sparse_queries,
                                                 10u);
    REQUIRE(sparse_reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(sparse_reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(sparse_reader->finish().has_value());
    const auto sparse_evidence = sparse_reader->memory_stats();
    REQUIRE(sparse_evidence.global_position_storage_preparations == 1u);
    REQUIRE(sparse_evidence.global_position_storage_allocations == 1u);
    REQUIRE(sparse_evidence.position_append_growth_reservations == 0u);
    REQUIRE(sparse_evidence.retained_position_count == 1u);
    REQUIRE(sparse_evidence.retained_position_capacity == 1u);
    REQUIRE(sparse_evidence.query_record_size_bytes == sizeof(std::uint16_t));
    REQUIRE(sparse_evidence.query_record_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_bits == 0u);
    REQUIRE(sparse_evidence.query_route_radix_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_nonempty_bucket_count == 1u);
    REQUIRE(sparse_evidence.query_route_radix_max_bucket_size == 1u);
    REQUIRE(sparse_evidence.query_hash_capacity_bytes ==
            sizeof(std::uint32_t) + 2u * sizeof(std::uint32_t));
    REQUIRE(sparse_evidence.released_query_hash_capacity_bytes >= sizeof(std::uint64_t));
    REQUIRE(sparse_evidence.decoded_control_records > 1u);
    REQUIRE(sparse_evidence.query_route_control_lookups > 0u);
    REQUIRE(sparse_evidence.query_route_control_lookups <= sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.query_route_matches == 1u);
    REQUIRE(sparse_evidence.query_route_hash_probes <=
            sparse_evidence.query_route_control_lookups * 2u);
    REQUIRE(sparse_evidence.retained_selected_control_records ==
            sparse_evidence.decoded_control_records);
    REQUIRE(sparse_evidence.retained_selected_control_capacity_bytes ==
            sparse_evidence.decoded_control_records * sizeof(std::uint16_t) +
                sizeof(std::uint32_t) * 2u);
    REQUIRE(sparse_evidence.current_selected_control_capacity_bytes == 0u);
    REQUIRE(sparse_evidence.retained_layout_equation_holds());
    REQUIRE(sparse_evidence.owned_capacity_bytes == sparse_reader->admitted_bytes());
    REQUIRE(sparse_evidence.capacity_accounting_passes == expected_barrier_audits);
    REQUIRE_FALSE(sparse_evidence.saturated);

    // An absent hash still passes through control and position processing.
    // It should allocate no position buffer.
    const std::array<std::uint64_t, 1> absent_queries{queries.front() ^ 1u};
    REQUIRE(std::ranges::find(queries, absent_queries.front()) == queries.end());
    auto empty_reader =
        prepared_grouped_soa_query_reader::begin(admission,
                                                 generation->head,
                                                 generation->manifest,
                                                 test_grouped_soa_expected_storage_contract(10u),
                                                 generation->directory.encoded_bytes(),
                                                 absent_queries,
                                                 10u);
    REQUIRE(empty_reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(empty_reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(empty_reader->finish().has_value());
    const auto empty_evidence = empty_reader->memory_stats();
    REQUIRE(empty_evidence.global_position_storage_preparations == 1u);
    REQUIRE(empty_evidence.global_position_storage_allocations == 0u);
    REQUIRE(empty_evidence.retained_position_count == 0u);
    REQUIRE(empty_evidence.retained_position_capacity == 0u);
    REQUIRE(empty_evidence.global_position_capacity_bytes == 0u);
    REQUIRE(empty_evidence.retained_layout_equation_holds());
    REQUIRE(empty_evidence.owned_capacity_bytes == empty_reader->admitted_bytes());
    REQUIRE(empty_reader->lookup(absent_queries.front()).positions.empty());

    constexpr std::uint64_t aggregate_sessions = 2u;
    const std::uint64_t aggregate_passes =
        evidence.capacity_accounting_passes + sparse_evidence.capacity_accounting_passes;
    REQUIRE(aggregate_passes == aggregate_sessions * expected_barrier_audits);
}

TEST_CASE("grouped-SoA persisted reader accepts a generated K20 u64 query image",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t selected_hash = (std::uint64_t{1u} << 39u) | 0x35u;
    constexpr std::uint64_t other_hash = selected_hash + 9u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {selected_hash, (25u << 1u) | 1u},
        {other_hash, 41u << 1u},
    };
    vector_replay_minimizer_producer producer{
        minimizers,
        k_minimap_scheme_identity,
        minimizer_producer_backend_identity_for_backend(minimizer_sketch_backend::exact_simd)};
    collecting_minimizer_row_sink sink;
    constexpr auto options = test_grouped_soa_generation_options<seq::dna4, std::uint64_t>(
        100u,
        10u,
        minimizer_algorithm::simd_minimap2_reference,
        100u,
        20u,
        15u);
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.algorithm_identity == options.algorithm_identity);
    REQUIRE(generation->manifest.kmer_size == 20u);
    REQUIRE(generation->manifest.window_size == 15u);
    const auto expected = expected_grouped_soa_generation_identity_for_backend(
        minimizer_generation_layout::grouped_soa_segment_local,
        minimizer_sketch_backend::exact_simd,
        options.algorithm_identity,
        k_minimizer_persisted_dna4_alphabet_code,
        sizeof(std::uint64_t),
        20u,
        15u,
        10u);
    REQUIRE(expected.has_value());

    const std::array<std::uint64_t, 1> queries{selected_hash};
    bioseq::parallelism::memory_admission_controller admission;
    auto reader = prepared_grouped_soa_query_reader::begin(admission,
                                                           generation->head,
                                                           generation->manifest,
                                                           *expected,
                                                           generation->directory.encoded_bytes(),
                                                           queries,
                                                           10u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());
    const auto selected = reader->lookup(selected_hash);
    REQUIRE(selected.positions.size() == 1u);
    REQUIRE(selected.positions.front() == minimizers.front().second);
    REQUIRE(reader->lookup(other_hash).positions.empty());

    const std::array<std::uint32_t, 1> narrow_queries{0x35u};
    auto rejected = prepared_grouped_soa_query_reader::begin(
        admission,
        generation->head,
        generation->manifest,
        *expected,
        generation->directory.encoded_bytes(),
        narrow_queries,
        10u);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == minimizer_generation_encoding_error::storage_contract_mismatch);
}

TEST_CASE("grouped-SoA admission failures name the exact owner substage",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(static_cast<std::uint8_t>(minimizer_generation_encoding_error::admission) ==
                   28u);
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {2u, 4u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    const std::array<std::uint64_t, 2> queries{1u, 2u};
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0)
            payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);

    using substage_type = grouped_soa_query_admission_stage;
    constexpr std::array begin_substages{
        substage_type::identity_and_directory_reservation,
        substage_type::query_hash_reservation,
        substage_type::query_hash_allocation,
        substage_type::query_hash_reconcile,
        substage_type::query_route_reservation,
        substage_type::query_route_allocation,
        substage_type::query_route_reconcile,
    };
    constexpr std::array payload_substages{
        substage_type::control_payload_reservation,
        substage_type::control_payload_allocation,
        substage_type::control_payload_reconcile,
        substage_type::control_decode_reservation,
        substage_type::control_decode_allocation,
        substage_type::control_decode_reconcile,
        substage_type::position_image_reservation,
        substage_type::position_image_allocation,
        substage_type::position_image_reconcile,
    };

    for (const substage_type wanted : begin_substages) {
        INFO("begin admission substage " << grouped_soa_query_admission_substage_name(wanted));
        bioseq::parallelism::memory_admission_controller admission;
        grouped_soa_query_admission_stats evidence;
        const auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &evidence,
            grouped_soa_query_reader_options{
                .fail_admission_substage = wanted,
            });
        require_wire_error(made, minimizer_generation_encoding_error::admission);
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(grouped_soa_query_admission_substage_name(wanted) != std::string_view{"unknown"});
        if (wanted != substage_type::identity_and_directory_reservation) {
            REQUIRE(evidence.projected_bytes > 0u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    for (const substage_type wanted : payload_substages) {
        INFO("payload admission substage " << grouped_soa_query_admission_substage_name(wanted));
        bioseq::parallelism::memory_admission_controller admission;
        grouped_soa_query_admission_stats begin_evidence;
        auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &begin_evidence,
            grouped_soa_query_reader_options{
                .fail_admission_substage = wanted,
            });
        REQUIRE(made.has_value());
        REQUIRE(begin_evidence.substage == substage_type::none);
        bool rejected = false;
        for (const collected_minimizer_row* row : payload) {
            const auto accepted = made->accept(row_view(*row));
            if (!accepted) {
                REQUIRE(accepted.error() == minimizer_generation_encoding_error::admission);
                rejected = true;
                break;
            }
        }
        REQUIRE(rejected);
        const auto evidence = made->admission_failure_stats();
        REQUIRE(evidence.substage == wanted);
        REQUIRE(evidence.injected_failure);
        REQUIRE(evidence.projected_bytes > 0u);
        REQUIRE(evidence.query_record_count == queries.size());
        REQUIRE(evidence.query_record_capacity >= queries.size());
        REQUIRE(evidence.releasable_protocol_query_hash_bytes == 0u);
        if (wanted == substage_type::position_image_reservation ||
            wanted == substage_type::position_image_allocation ||
            wanted == substage_type::position_image_reconcile) {
            REQUIRE(evidence.retained_position_count == queries.size());
            REQUIRE(evidence.retained_position_bytes == queries.size() * sizeof(std::uint64_t));
            REQUIRE(evidence.allocation_headroom_bytes == 0u);
            if (wanted == substage_type::position_image_reconcile) {
                REQUIRE(evidence.projected_bytes == evidence.owned_capacity_bytes);
            } else {
                const std::uint64_t compact_final_bytes =
                    queries.size() * sizeof(std::uint64_t) + queries.size() * sizeof(std::uint8_t);
                REQUIRE(evidence.projected_bytes == evidence.owned_capacity_bytes +
                                                        evidence.retained_position_bytes +
                                                        compact_final_bytes);
            }
        }
        REQUIRE(made->failed());
        REQUIRE(made->admitted_bytes() == 0u);
        const auto released = made->memory_stats();
        REQUIRE(released.retained_position_count == 0u);
        REQUIRE(released.retained_position_capacity == 0u);
        REQUIRE(released.global_position_capacity_bytes == 0u);
        REQUIRE(released.retained_layout_equation_holds());
        REQUIRE(released.global_position_storage_allocations ==
                (wanted == substage_type::position_image_reconcile ? 1u : 0u));
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("real low budget retains the controller predicate snapshot") {
        bioseq::parallelism::memory_admission_controller admission;
        admission.set_index_memory_cap_bytes(1u);
        grouped_soa_query_admission_stats evidence;
        const auto made = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            queries,
            10u,
            &evidence);
        require_wire_error(made, minimizer_generation_encoding_error::admission);
        REQUIRE(evidence.substage == substage_type::identity_and_directory_reservation);
        REQUIRE_FALSE(evidence.injected_failure);
        REQUIRE(evidence.controller_failure_snapshot_present);
        REQUIRE(evidence.controller_reason !=
                bioseq::parallelism::memory_admission_failure_reason::none);
        REQUIRE(evidence.requested_growth_bytes > 1u);
        REQUIRE(evidence.controller_budget_ceiling_bytes <= 1u);
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("arithmetic overflow is not relabelled as admission") {
        const auto ordinary = plan_grouped_soa_position_image(3u, 5u);
        REQUIRE(ordinary.has_value());
        REQUIRE(ordinary->retained_position_bytes == 24u);
        REQUIRE(ordinary->allocation_headroom_bytes == 0u);
        REQUIRE(ordinary->projected_bytes == 29u);
        constexpr std::uint64_t adversarial_count = 100'000'000u;
        const auto adversarial = plan_grouped_soa_position_image(adversarial_count, 17u);
        REQUIRE(adversarial.has_value());
        REQUIRE(adversarial->retained_position_bytes == 800'000'000u);
        REQUIRE(adversarial->allocation_headroom_bytes == 0u);
        REQUIRE(adversarial->projected_bytes == 800'000'017u);
        REQUIRE(adversarial->projected_bytes - 17u <
                adversarial->retained_position_bytes *
                    bioseq::parallelism::k_vector_preallocation_headroom_factor);
        constexpr std::uint64_t adversarial_query_count = 100'000'000u;
        constexpr std::uint64_t prior_query_owned_bytes =
            adversarial_query_count * (64u + 2u * sizeof(std::uint64_t));
        constexpr std::uint64_t exact_query_state_and_hash_bytes =
            adversarial_query_count * (sizeof(std::uint32_t) + sizeof(std::uint8_t));
        // The radix table uses 25 of the 30 hash bits. The remaining five
        // fit in one byte per query.
        constexpr std::uint64_t exact_query_radix_bytes =
            ((1ULL << 25u) + 1u) * sizeof(std::uint32_t);
        constexpr std::uint64_t exact_query_owned_bytes =
            exact_query_state_and_hash_bytes + exact_query_radix_bytes;
        STATIC_REQUIRE(prior_query_owned_bytes == 8'000'000'000u);
        STATIC_REQUIRE(exact_query_state_and_hash_bytes == 500'000'000u);
        STATIC_REQUIRE(exact_query_radix_bytes == 134'217'732u);
        STATIC_REQUIRE(exact_query_owned_bytes == 634'217'732u);
        STATIC_REQUIRE(prior_query_owned_bytes - exact_query_owned_bytes == 7'365'782'268u);
        require_wire_error(
            plan_grouped_soa_position_image((std::numeric_limits<std::uint64_t>::max)(), 0u),
            minimizer_generation_encoding_error::arithmetic_overflow);
        require_wire_error(
            plan_grouped_soa_position_image(1u, (std::numeric_limits<std::uint64_t>::max)() - 1u),
            minimizer_generation_encoding_error::arithmetic_overflow);
    }
}

TEST_CASE("grouped-SoA selects its exact immutable row quantum",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_extent_payload_bytes(
                       minimizer_generation_layout::grouped_soa_segment_local) == 8064u);

    constexpr std::uint64_t position_count = 5000u;
    constexpr std::uint64_t first_control_count = 8064u / 4u;
    constexpr std::uint64_t queried_control_count = position_count - first_control_count - 1u;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers;
    minimizers.reserve(position_count);
    for (std::uint64_t position = 0; position < position_count; ++position) {
        const std::uint64_t hash = position < first_control_count                           ? 6u
                                   : position < first_control_count + queried_control_count ? 7u
                                                                                            : 8u;
        minimizers.emplace_back(hash, position << 1u);
    }
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(position_count + 1u, 65'534u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest_row.byte_count == 176u);
    REQUIRE(generation->manifest.position_segment_span_bp == k_minimizer_preferred_segment_span_bp);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u32);
    REQUIRE(generation->directory.entries().size() == 1u);
    const grouped_soa_bucket_directory_entry& entry = generation->directory.entries().front();
    REQUIRE(entry.position_encoded_bytes == position_count * 4u);
    REQUIRE(entry.position_chunk_count == 3u);
    REQUIRE(sink.grouped_soa_payload_write_calls ==
            static_cast<std::uint64_t>(entry.control_chunk_count) + entry.position_chunk_count);
    REQUIRE(sink.grouped_soa_row_crc_bytes == generation->manifest.payload_bytes);
    const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
    REQUIRE(position_sub_key.has_value());
    const auto first_position =
        std::ranges::find_if(sink.rows, [position_sub_key](const collected_minimizer_row& row) {
            return row.address.sub_key == *position_sub_key && row.address.chunk_no == 0u;
        });
    REQUIRE(first_position != sink.rows.end());
    REQUIRE(first_position->bytes.size() == 8064u);
    REQUIRE(sink.max_callback_bytes == 8064u);
    require_complete_generation_rows(*generation, sink.rows);

    // The first hash ends at a row boundary. Query only the middle hash
    // to check that sparse lookup continues in the next position row.
    bioseq::parallelism::memory_admission_controller admission;
    const std::array<std::uint64_t, 1> query{7u};
    auto reader = prepared_grouped_soa_query_reader::begin(
        admission,
        generation->head,
        generation->manifest,
        test_grouped_soa_expected_storage_contract(65'534u),
        generation->directory.encoded_bytes(),
        query,
        65'534u);
    REQUIRE(reader.has_value());
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0) {
            REQUIRE(reader->accept(row_view(row)).has_value());
        }
    }
    REQUIRE(reader->finish().has_value());
    const auto lookup = reader->lookup(7u);
    REQUIRE(lookup.positions.size() == queried_control_count);
    REQUIRE(lookup.positions.front() == first_control_count << 1u);
    REQUIRE(lookup.positions.back() == (first_control_count + queried_control_count - 1u) << 1u);
    const auto evidence = reader->memory_stats();
    REQUIRE(evidence.decoded_control_records == 3u);
    REQUIRE(evidence.retained_selected_control_capacity_bytes ==
            3u * sizeof(std::uint16_t) + 2u * sizeof(std::uint32_t));
}

TEST_CASE("grouped-SoA narrows positions only with a narrow immutable span",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t narrow_span = std::uint64_t{1} << 15u;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {7u, 0u},
        {8u, narrow_span << 1u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    grouped_soa_generation_limits one_control_per_segment;
    one_control_per_segment.max_segment_control_count = 1u;

    const auto generation =
        encode_minimizer_generation(producer,
                                    sink,
                                    test_grouped_soa_generation_options(narrow_span + 1u, 65'534u),
                                    one_control_per_segment);
    REQUIRE(generation.has_value());
    REQUIRE(generation->manifest.position_segment_span_bp == narrow_span);
    REQUIRE(generation->manifest.position_segment_count == 2u);
    REQUIRE(generation->manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision);
    REQUIRE(generation->manifest.position_width == minimizer_scalar_width::u16);

    const auto canonical_profile =
        derive_grouped_soa_minimizer_profile(k_minimap_scheme_identity,
                                             generation->manifest.alphabet_code,
                                             generation->manifest.kmer_size,
                                             generation->manifest.position_segment_span_bp,
                                             generation->manifest.max_occ_cap);
    REQUIRE(canonical_profile.has_value());
    REQUIRE(generation->directory.profile() == *canonical_profile);
    const auto manifest_profile = derive_grouped_soa_minimizer_profile(generation->manifest);
    REQUIRE(manifest_profile.has_value());
    REQUIRE(*manifest_profile == *canonical_profile);

    REQUIRE(generation->directory.entries().size() == 2u);
    std::uint64_t position_count = 0u;
    std::uint64_t position_bytes = 0u;
    for (const grouped_soa_bucket_directory_entry& entry : generation->directory.entries()) {
        REQUIRE(entry.position_element_count == 1u);
        REQUIRE(entry.position_encoded_bytes == 2u);
        REQUIRE(entry.position_chunk_count == 1u);
        position_count += entry.position_element_count;
        position_bytes += entry.position_encoded_bytes;
    }
    REQUIRE(position_count == generation->manifest.position_count);
    REQUIRE(position_bytes == generation->manifest.position_count * 2u);
    REQUIRE(sink.grouped_soa_row_crc_bytes == generation->manifest.payload_bytes);
    require_complete_generation_rows(*generation, sink.rows);
}

TEST_CASE("grouped-SoA authenticates global controls then global positions",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = k_minimizer_preferred_segment_span_bp;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {42u, 2u},
        {42u, 4u},
        {42u, ((span + 1u) << 1u)},
        {42u, ((span + 2u) << 1u)},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation = encode_minimizer_generation(
        producer, sink, test_grouped_soa_generation_options(span + 100u, 10u));
    REQUIRE(generation.has_value());
    REQUIRE(generation->directory.entries().size() == 2u);
    for (std::size_t ordinal = 0; ordinal < generation->directory.entries().size(); ++ordinal) {
        const auto& entry = generation->directory.entries()[ordinal];
        REQUIRE(entry.partition_id == ordinal);
        REQUIRE(entry.sub_key == static_cast<index_sub_key_t>(ordinal));
        REQUIRE(entry.control_chunk_count == 1u);
        REQUIRE(entry.position_chunk_count == 1u);
    }
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0)
            payload.push_back(&row);
    }
    REQUIRE(payload.size() == 4u);
    REQUIRE(payload[0]->address.position_segment == 0);
    REQUIRE(payload[0]->address.sub_key == 0);
    REQUIRE(payload[1]->address.position_segment == 1);
    REQUIRE(payload[1]->address.sub_key == 1);
    REQUIRE(payload[2]->address.position_segment == 0);
    REQUIRE(payload[2]->address.sub_key == *derive_minimizer_position_extent_sub_key(0));
    REQUIRE(payload[3]->address.position_segment == 1);
    REQUIRE(payload[3]->address.sub_key == *derive_minimizer_position_extent_sub_key(1));
    require_complete_generation_rows(*generation, sink.rows);

    auto interleaved = make_generation_row_validator(*generation);
    REQUIRE(interleaved.accept(row_view(*payload[0])).has_value());
    require_wire_error(interleaved.accept(row_view(*payload[2])),
                       minimizer_generation_encoding_error::row_order);

    const std::array<std::uint64_t, 1> query{42u};
    SECTION("max-occ is target-global across both segments") {
        bioseq::parallelism::memory_admission_controller admission;
        auto pruned = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            query,
            3u);
        REQUIRE(pruned.has_value());
        for (const collected_minimizer_row* row : payload) {
            REQUIRE(pruned->accept(row_view(*row)).has_value());
        }
        REQUIRE(pruned->finish().has_value());
        const auto found = pruned->lookup(42u);
        REQUIRE(found.global_occurrence_count == 4u);
        REQUIRE_FALSE(found.positions_complete);
        REQUIRE(found.positions.empty());
    }
    SECTION("boundary max-occ retains all cross-segment positions") {
        bioseq::parallelism::memory_admission_controller admission;
        auto retained = prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            query,
            4u);
        REQUIRE(retained.has_value());
        for (const collected_minimizer_row* row : payload) {
            REQUIRE(retained->accept(row_view(*row)).has_value());
        }
        REQUIRE(retained->finish().has_value());
        const auto found = retained->lookup(42u);
        REQUIRE(found.global_occurrence_count == 4u);
        REQUIRE(found.positions_complete);
        REQUIRE((std::vector<std::uint64_t>{found.positions.begin(), found.positions.end()} ==
                 std::vector<std::uint64_t>{2u, 4u, (span + 1u) << 1u, (span + 2u) << 1u}));
    }

    auto invalid_entry = generation->directory.entries().front();
    invalid_entry.sub_key = 7;
    require_wire_error(
        encode_grouped_soa_bucket_directory_entry(invalid_entry, generation->directory.profile()),
        minimizer_generation_encoding_error::sub_key);
}

TEST_CASE("grouped-SoA reader releases admission on every terminal path",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1u, 2u},
        {1u, 4u},
    };
    vector_replay_minimizer_producer producer{minimizers};
    collecting_minimizer_row_sink sink;
    const auto generation =
        encode_minimizer_generation(producer, sink, test_grouped_soa_generation_options(100u, 10u));
    REQUIRE(generation.has_value());
    std::vector<const collected_minimizer_row*> payload;
    for (const auto& row : sink.rows) {
        if (row.address.position_segment >= 0)
            payload.push_back(&row);
    }
    REQUIRE(payload.size() == 2u);
    const std::array<std::uint64_t, 1> query{1u};
    const auto begin = [&generation, &query](auto& admission) {
        return prepared_grouped_soa_query_reader::begin(
            admission,
            generation->head,
            generation->manifest,
            test_grouped_soa_expected_storage_contract(10u),
            generation->directory.encoded_bytes(),
            query,
            10u);
    };

    SECTION("trusted receipt is move-only and grouped-SoA requires authenticated row CRC") {
        bioseq::parallelism::memory_admission_controller admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto first_view = row_view(*payload.front());
            auto bad_first_view = first_view;
            bad_first_view.row_checksum ^= 1u;
            require_wire_error(validate_minimizer_generation_row_receipt(bad_first_view),
                               minimizer_generation_encoding_error::checksum);
            auto validated_first = validate_minimizer_generation_row_receipt(first_view);
            REQUIRE(validated_first.has_value());
            auto first = std::move(*validated_first);
            REQUIRE(first.has_valid_authority());
            first_view.row_checksum ^= 1u;
            auto moved = std::move(first);
            REQUIRE_FALSE(first.has_valid_authority());
            REQUIRE(moved.has_valid_authority());
            REQUIRE(made->accept(moved).has_value());
            auto second = trusted_row_receipt(*payload.back());
            REQUIRE(made->accept(second).has_value());
            REQUIRE(made->finish().has_value());
            REQUIRE(made->lookup(1u).positions.size() == 2u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted wrong address still poisons and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto wrong = *payload.front();
        ++wrong.address.sub_key;
        recompute_row_checksum(wrong);
        auto receipt = trusted_row_receipt(wrong);
        require_wire_error(made->accept(receipt), minimizer_generation_encoding_error::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("ordinary grouped-SoA row with wrong row CRC remains rejected") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto untrusted = row_view(*payload.front());
        untrusted.row_checksum ^= 1u;
        require_wire_error(made->accept(untrusted), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("moved-from receipt fails runtime authority validation") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto source = trusted_row_receipt(*payload.front());
        auto owner = std::move(source);
        REQUIRE(owner.has_valid_authority());
        REQUIRE_FALSE(source.has_valid_authority());
        require_wire_error(made->accept(source), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted wrong order still poisons and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto receipt = trusted_row_receipt(*payload.back());
        require_wire_error(made->accept(receipt), minimizer_generation_encoding_error::row_order);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("grouped-SoA trusted row cannot bypass extent CRC") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        recompute_row_checksum(corrupt);
        auto receipt = trusted_row_receipt(corrupt);
        require_wire_error(made->accept(receipt), minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move transfers the sole permit owner") {
        bioseq::parallelism::memory_admission_controller admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            auto source = std::move(*made);
            const std::uint64_t reserved = admission.reserved_bytes();
            REQUIRE(reserved > 0u);
            prepared_grouped_soa_query_reader destination{std::move(source)};
            REQUIRE(admission.reserved_bytes() == reserved);
            for (const collected_minimizer_row* row : payload) {
                REQUIRE(destination.accept(row_view(*row)).has_value());
            }
            REQUIRE(destination.finish().has_value());
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("move after exact position allocation transfers array ownership") {
        bioseq::parallelism::memory_admission_controller admission;
        {
            auto made = begin(admission);
            REQUIRE(made.has_value());
            REQUIRE(made->accept(row_view(*payload.front())).has_value());
            const auto before_move = made->memory_stats();
            REQUIRE(before_move.global_position_storage_preparations == 1u);
            REQUIRE(before_move.global_position_storage_allocations == 1u);
            REQUIRE(before_move.retained_position_count == 2u);
            REQUIRE(before_move.retained_position_capacity == 2u);
            REQUIRE(before_move.global_position_capacity_bytes == 2u * sizeof(std::uint64_t));
            REQUIRE(before_move.owned_capacity_bytes == made->admitted_bytes());
            const std::uint64_t reserved = admission.reserved_bytes();

            prepared_grouped_soa_query_reader destination{std::move(*made)};
            REQUIRE(made->failed());
            REQUIRE(made->admitted_bytes() == 0u);
            REQUIRE(made->memory_stats().retained_position_count == 0u);
            REQUIRE(made->memory_stats().retained_position_capacity == 0u);
            REQUIRE(admission.reserved_bytes() == reserved);
            REQUIRE(destination.memory_stats().retained_position_count == 2u);
            REQUIRE(destination.accept(row_view(*payload.back())).has_value());
            REQUIRE(destination.finish().has_value());
            const auto found = destination.lookup(1u);
            REQUIRE(found.positions.size() == 2u);
            REQUIRE(found.positions[0] == 2u);
            REQUIRE(found.positions[1] == 4u);
        }
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("incomplete finish poisons and releases immediately") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        require_wire_error(made->finish(), minimizer_generation_encoding_error::row_count);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("row corruption poisons and releases immediately") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        auto corrupt = *payload.front();
        corrupt.bytes.front() ^= std::byte{1};
        require_wire_error(made->accept(row_view(corrupt)),
                           minimizer_generation_encoding_error::checksum);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }

    SECTION("post-begin allocation denial rolls back and releases") {
        bioseq::parallelism::memory_admission_controller admission;
        auto made = begin(admission);
        REQUIRE(made.has_value());
        admission.set_index_memory_cap_bytes(admission.reserved_bytes());
        require_wire_error(made->accept(row_view(*payload.front())),
                           minimizer_generation_encoding_error::admission);
        REQUIRE(made->failed());
        REQUIRE(admission.reserved_bytes() == 0u);
    }
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
