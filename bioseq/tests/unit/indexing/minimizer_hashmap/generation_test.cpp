/**
 * @file generation_test.cpp
 * @brief Checks minimizer generation bytes, metadata, and storage callbacks.
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

/// Check that a codec operation succeeds with the expected value.
template <class p_t>
void require_wire_value(
    const std::expected<p_t, bioseq::indexing::minimizer_generation_encoding_error>& value,
    const p_t& expected) {
    REQUIRE(value.has_value());
    REQUIRE(*value == expected);
}
/// Collect rows through the basic storage callback.
struct row_only_collecting_minimizer_sink {
    std::vector<collected_minimizer_row> rows;

    /// Copy a row, its address, and its checksum into test storage.
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(row.bytes.begin(), row.bytes.end()),
            .row_checksum = row.row_checksum,
        });
        return {};
    }
};

/// Callback kinds used to check publication order.
enum class planned_direct_sink_event : std::uint8_t {
    begin,
    payload,
    row,
};

/// Record the storage plan and keep direct payload writes separate from
/// ordinary row writes.
struct planned_direct_minimizer_sink {
    std::optional<bioseq::indexing::grouped_soa_storage_plan_view> storage_plan;
    std::vector<collected_minimizer_row> payload_rows;
    std::vector<collected_minimizer_row> ordinary_rows;
    std::vector<planned_direct_sink_event> events;
    std::uint64_t ordinary_payload_row_callbacks = 0u;

    /// Save the storage plan, rejecting a second plan or an out-of-order call.
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    begin_planned_generation(const bioseq::indexing::grouped_soa_storage_plan_view& plan) {
        if (storage_plan.has_value() || !events.empty()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_order);
        }
        storage_plan = plan;
        events.push_back(planned_direct_sink_event::begin);
        return {};
    }

    /// Copy an ordinary row after the storage plan has been received.
    [[nodiscard]] std::expected<void, bioseq::indexing::minimizer_generation_encoding_error>
    write(const bioseq::indexing::minimizer_generation_row_view& row) {
        if (!storage_plan.has_value()) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_order);
        }
        if (row.address.position_segment >= 0) {
            ++ordinary_payload_row_callbacks;
        }
        ordinary_rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(row.bytes.begin(), row.bytes.end()),
            .row_checksum = row.row_checksum,
        });
        events.push_back(planned_direct_sink_event::row);
        return {};
    }

    /// Copy a direct payload row and return its computed checksum.
    [[nodiscard]] std::expected<bioseq::indexing::minimizer_generation_row_write_receipt,
                                bioseq::indexing::minimizer_generation_encoding_error>
    write_grouped_soa_payload(
        const bioseq::indexing::unchecked_minimizer_generation_row_view& row) {
        if (!storage_plan.has_value() || row.address.generation_id != storage_plan->generation_id ||
            row.address.position_segment < 0) {
            return std::unexpected(
                bioseq::indexing::minimizer_generation_encoding_error::row_order);
        }
        const std::uint64_t row_checksum =
            bioseq::indexing::crc64_index_chunk_row(row.address.generation_id,
                                                    row.address.position_segment,
                                                    row.address.sub_key,
                                                    row.address.chunk_no,
                                                    static_cast<std::uint32_t>(row.bytes.size()),
                                                    row.bytes);
        payload_rows.push_back(collected_minimizer_row{
            .address = row.address,
            .bytes = std::vector<std::byte>(row.bytes.begin(), row.bytes.end()),
            .row_checksum = row_checksum,
        });
        events.push_back(planned_direct_sink_event::payload);
        return bioseq::indexing::minimizer_generation_row_write_receipt{
            .row_checksum = row_checksum,
        };
    }
};
} // namespace

TEST_CASE("canonical reserved generation-head sub-key is range-pinned",
          "[component:bioseq][layer:unit][feature:storage]") {
    STATIC_REQUIRE(bioseq::indexing::k_generation_head_sub_key ==
                   (std::numeric_limits<bioseq::indexing::index_sub_key_t>::max)() - 1);
    STATIC_REQUIRE(bioseq::indexing::k_generation_head_sub_key >
                   bioseq::indexing::k_temporary_run_sub_key_base);
    STATIC_REQUIRE(bioseq::indexing::k_generation_head_sub_key <
                   bioseq::indexing::k_directory_sub_key);
}

TEST_CASE("CRC-64/ECMA-182 is exact and incrementally composable",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::indexing::crc64_ecma182;
    using bioseq::indexing::crc64_ecma182_accumulator;

    STATIC_REQUIRE(crc64_ecma182(std::string_view{}) == 0);
    STATIC_REQUIRE(crc64_ecma182("123456789") == 0x6C40DF5F0B497347ULL);

    crc64_ecma182_accumulator incremental;
    incremental.update("1234");
    incremental.update("56789");
    REQUIRE(incremental.checksum() == 0x6C40DF5F0B497347ULL);

    constexpr std::array<std::byte, 9> bytes{
        std::byte{'1'},
        std::byte{'2'},
        std::byte{'3'},
        std::byte{'4'},
        std::byte{'5'},
        std::byte{'6'},
        std::byte{'7'},
        std::byte{'8'},
        std::byte{'9'},
    };
    STATIC_REQUIRE(crc64_ecma182(std::span<const std::byte>{bytes}) == 0x6C40DF5F0B497347ULL);
}

TEST_CASE("validated row CRC algebra reconstructs raw extent CRC",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;

    REQUIRE(crc64_ecma182_advance_zero_bytes(0, 0) == 0);
    REQUIRE(crc64_ecma182_concatenate(crc64_ecma182("control"), crc64_ecma182("position"), 8u) ==
            crc64_ecma182("controlposition"));

    const index_chunk_address signed_address{
        .generation_id = 0xFEDCBA9876543210ULL,
        .position_segment = (std::numeric_limits<std::int32_t>::min)() + 17,
        .sub_key = (std::numeric_limits<index_sub_key_t>::min)() + 29,
        .chunk_no = 0x89ABCDEFu,
    };
    constexpr std::uint32_t signed_length = 0x10203040u;
    crc64_ecma182_accumulator independently_encoded_prefix;
    independently_encoded_prefix.update_little_endian(signed_address.generation_id);
    independently_encoded_prefix.update_little_endian(
        std::bit_cast<std::uint32_t>(signed_address.position_segment));
    independently_encoded_prefix.update_little_endian(
        std::bit_cast<std::uint64_t>(signed_address.sub_key));
    independently_encoded_prefix.update_little_endian(signed_address.chunk_no);
    independently_encoded_prefix.update_little_endian(signed_length);
    REQUIRE(crc64_index_chunk_row_prefix(signed_address.generation_id,
                                         signed_address.position_segment,
                                         signed_address.sub_key,
                                         signed_address.chunk_no,
                                         signed_length) == independently_encoded_prefix.checksum());

    std::uint64_t algebraic_extent = 0;
    crc64_ecma182_accumulator direct_extent;
    std::uint64_t random_state = 0xD1B54A32D192ED03ULL;
    const auto next_random = [&random_state]() noexcept {
        random_state ^= random_state >> 12u;
        random_state ^= random_state << 25u;
        random_state ^= random_state >> 27u;
        return random_state * 0x2545F4914F6CDD1DULL;
    };
    const auto append_case = [&next_random, &direct_extent, &algebraic_extent](
                                 std::size_t length, std::uint32_t ordinal) {
        INFO("payload length=" << length << " ordinal=" << ordinal);
        std::vector<std::byte> payload(length);
        for (std::byte& byte : payload) {
            byte = static_cast<std::byte>(next_random() & 0xffu);
        }
        const index_chunk_address address{
            .generation_id = 0x8000000000000000ULL + ordinal,
            .position_segment = ordinal % 2u == 0 ? (std::numeric_limits<std::int32_t>::min)() +
                                                        static_cast<std::int32_t>(ordinal)
                                                  : (std::numeric_limits<std::int32_t>::max)() -
                                                        static_cast<std::int32_t>(ordinal),
            .sub_key = ordinal % 2u == 0 ? (std::numeric_limits<index_sub_key_t>::min)() +
                                               static_cast<index_sub_key_t>(ordinal)
                                         : (std::numeric_limits<index_sub_key_t>::max)() -
                                               static_cast<index_sub_key_t>(ordinal),
            .chunk_no = 0xF0000000u + ordinal,
        };
        const auto bytes = std::span<const std::byte>{payload};
        const auto chunk_bytes = static_cast<std::uint32_t>(bytes.size());
        const std::uint64_t row_checksum = crc64_index_chunk_row(address.generation_id,
                                                                 address.position_segment,
                                                                 address.sub_key,
                                                                 address.chunk_no,
                                                                 chunk_bytes,
                                                                 bytes);
        direct_extent.update(bytes);
        algebraic_extent = crc64_append_validated_index_chunk_payload(
            algebraic_extent, address, chunk_bytes, row_checksum);
        REQUIRE(algebraic_extent == direct_extent.checksum());
    };

    const std::array<std::size_t, 6> boundary_lengths{0u, 1u, 17u, 8063u, 8064u, 31u};
    std::uint32_t ordinal = 0;
    for (const std::size_t length : boundary_lengths) {
        append_case(length, ordinal++);
    }
    for (std::size_t iteration = 0; iteration < 128u; ++iteration) {
        append_case(static_cast<std::size_t>(next_random() % 8065u), ordinal++);
    }

    REQUIRE(algebraic_extent == direct_extent.checksum());
}

TEST_CASE("grouped-SoA cached ownership accounting replaces and moves exactly",
          "[component:bioseq][layer:unit][feature:storage]") {
    using bioseq::indexing::protocol_owned_bytes_ledger;
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<protocol_owned_bytes_ledger>);
    STATIC_REQUIRE(std::is_nothrow_move_assignable_v<protocol_owned_bytes_ledger>);

    protocol_owned_bytes_ledger accounting;
    const auto replace = [&accounting](std::uint64_t& field, std::uint64_t value) {
        REQUIRE(accounting.replace_checked(field, value));
        field = value;
        REQUIRE(accounting.field_sum_holds());
    };
    replace(accounting.directory_bytes, 101u);
    replace(accounting.query_hash_bytes, 211u);
    replace(accounting.query_record_bytes, 307u);
    replace(accounting.selected_extent_owner_bytes, 401u);
    replace(accounting.query_record_position_bytes, 503u);
    replace(accounting.control_scratch_bytes, 601u);
    replace(accounting.selected_extent_owner_bytes, 17u);
    REQUIRE(accounting.add_temporary_peak_checked(8192u));

    const std::uint64_t total_before_failure = accounting.total_bytes;
    REQUIRE_FALSE(accounting.replace_checked(total_before_failure + 1u, 0u));
    REQUIRE(accounting.total_bytes == total_before_failure);
    REQUIRE(accounting.field_sum_holds());

    protocol_owned_bytes_ledger moved{std::move(accounting)};
    REQUIRE(moved.total_bytes == total_before_failure);
    REQUIRE(moved.field_sum_holds());
    REQUIRE(accounting.total_bytes == 0u);
    REQUIRE(accounting.field_sum_holds());
    moved.reset();
    REQUIRE(moved.total_bytes == 0u);
    REQUIRE(moved.field_sum_holds());

    protocol_owned_bytes_ledger overflow;
    overflow.directory_bytes = (std::numeric_limits<std::uint64_t>::max)() - 3u;
    overflow.total_bytes = overflow.directory_bytes;
    REQUIRE_FALSE(overflow.replace_checked(0u, 4u));
    REQUIRE_FALSE(overflow.add_temporary_peak_checked(4u));
    REQUIRE(overflow.field_sum_holds());
}

TEST_CASE("canonical manifests distinguish bounded ntHash while preserving minimap identity",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr grouped_soa_generation_options unbound_writer{};
    STATIC_REQUIRE_FALSE(is_supported_minimizer_scheme_identity(
        unbound_writer.minimizer_scheme_id, unbound_writer.minimizer_scheme_revision));
    constexpr expected_minimizer_generation_identity unbound_reader{};
    STATIC_REQUIRE_FALSE(is_supported_minimizer_scheme_identity(
        unbound_reader.minimizer_scheme_id, unbound_reader.minimizer_scheme_revision));
    STATIC_REQUIRE_FALSE(
        is_supported_minimizer_producer_backend_identity(minimizer_producer_identity{
            .id = unbound_reader.producer_backend_id,
            .revision = unbound_reader.producer_identity_revision,
        }));
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(minimizer_sketch_backend::scalar) ==
                   k_minimap_scheme_identity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(minimizer_sketch_backend::exact_simd) ==
                   k_minimap_scheme_identity);
    STATIC_REQUIRE(minimizer_scheme_identity_for_backend(
                       minimizer_sketch_backend::vulkan_segment) == k_minimap_scheme_identity);
    STATIC_REQUIRE(
        minimizer_scheme_identity_for_backend(minimizer_sketch_backend::bounded_nthash_simd) ==
        k_bounded_nthash_u32_r7_sum_scheme_identity);
    STATIC_REQUIRE(
        minimizer_producer_backend_identity_for_backend(minimizer_sketch_backend::scalar).id ==
        minimizer_producer_backend::scalar);
    STATIC_REQUIRE(
        minimizer_producer_backend_identity_for_backend(minimizer_sketch_backend::exact_simd).id ==
        minimizer_producer_backend::exact_simd);
    STATIC_REQUIRE(minimizer_producer_backend_identity_for_backend(
                       minimizer_sketch_backend::bounded_nthash_simd)
                       .id == minimizer_producer_backend::bounded_nthash_simd);
    STATIC_REQUIRE(
        minimizer_producer_backend_identity_for_backend(minimizer_sketch_backend::vulkan_segment)
            .id == minimizer_producer_backend::vulkan_segment);
    constexpr auto minimap_manifest = golden_generation_manifest();
    constexpr auto minimap_bytes = encode_minimizer_generation_manifest(minimap_manifest);
    STATIC_REQUIRE(minimap_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*minimap_bytes, 122) == k_minimap_scheme_id);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*minimap_bytes, 124) == k_minimap_scheme_revision);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*minimap_bytes, 126) ==
                   static_cast<std::uint8_t>(minimizer_producer_backend::scalar));
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*minimap_bytes, 127) ==
                   k_minimizer_producer_identity_revision);

    constexpr auto bounded = [] {
        auto value = golden_generation_manifest();
        assign_grouped_soa_profile(value, k_test_bounded_grouped_soa_profile);
        value.producer_backend_id = minimizer_producer_backend::bounded_nthash_simd;
        value.algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::bounded_nthash_simd);
        return value;
    }();
    constexpr auto bounded_bytes = encode_minimizer_generation_manifest(bounded);
    STATIC_REQUIRE(bounded_bytes.has_value());
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*bounded_bytes, 122) ==
                   k_bounded_nthash_u32_r7_sum_scheme_id);
    STATIC_REQUIRE(test_read_le<std::uint16_t>(*bounded_bytes, 124) ==
                   k_bounded_nthash_u32_r7_sum_scheme_revision);
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*bounded_bytes, 126) ==
                   static_cast<std::uint8_t>(minimizer_producer_backend::bounded_nthash_simd));
    STATIC_REQUIRE(test_read_le<std::uint8_t>(*bounded_bytes, 127) ==
                   k_minimizer_producer_identity_revision);
    STATIC_REQUIRE(*bounded_bytes != *minimap_bytes);
    constexpr auto decoded_bounded =
        decode_minimizer_generation_manifest(std::span<const std::byte>{*bounded_bytes});
    STATIC_REQUIRE(decoded_bounded.has_value());
    STATIC_REQUIRE(*decoded_bounded == bounded);
    STATIC_REQUIRE(compute_minimizer_generation_storage_contract_checksum(bounded) !=
                   compute_minimizer_generation_storage_contract_checksum(minimap_manifest));

    constexpr auto canonical_minimap = [] {
        auto value = golden_generation_manifest();
        value.gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
        return value;
    }();
    constexpr auto canonical_bounded = [] {
        auto value = golden_generation_manifest();
        assign_grouped_soa_profile(value, k_test_bounded_grouped_soa_profile);
        value.producer_backend_id = minimizer_producer_backend::bounded_nthash_simd;
        value.algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::bounded_nthash_simd);
        return value;
    }();

    constexpr expected_minimizer_generation_identity minimap_reader{
        .layout = minimizer_generation_layout::grouped_soa_segment_local,
        .alphabet_code = canonical_minimap.alphabet_code,
        .storage_unit_byte_width = canonical_minimap.storage_unit_byte_width,
        .kmer_size = canonical_minimap.kmer_size,
        .window_size = canonical_minimap.window_size,
        .gpu_abi_revision = canonical_minimap.gpu_abi_revision,
        .minimizer_scheme_id = k_minimap_scheme_id,
        .minimizer_scheme_revision = k_minimap_scheme_revision,
        .producer_backend_id = minimizer_producer_backend::scalar,
        .producer_identity_revision = k_minimizer_producer_identity_revision,
        .algorithm_identity = test_generation_algorithm_identity(),
        .max_occ_cap = k_test_grouped_soa_profile.max_occ_cap,
    };
    constexpr expected_minimizer_generation_identity bounded_reader{
        .layout = minimizer_generation_layout::grouped_soa_segment_local,
        .alphabet_code = canonical_bounded.alphabet_code,
        .storage_unit_byte_width = canonical_bounded.storage_unit_byte_width,
        .kmer_size = canonical_bounded.kmer_size,
        .window_size = canonical_bounded.window_size,
        .gpu_abi_revision = canonical_bounded.gpu_abi_revision,
        .minimizer_scheme_id = k_bounded_nthash_u32_r7_sum_scheme_id,
        .minimizer_scheme_revision = k_bounded_nthash_u32_r7_sum_scheme_revision,
        .producer_backend_id = minimizer_producer_backend::bounded_nthash_simd,
        .producer_identity_revision = k_minimizer_producer_identity_revision,
        .algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::bounded_nthash_simd),
        .max_occ_cap = k_test_bounded_grouped_soa_profile.max_occ_cap,
    };
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(canonical_minimap, minimap_reader)
                       .has_value());
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(canonical_bounded, bounded_reader)
                       .has_value());
    STATIC_REQUIRE_FALSE(
        validate_minimizer_generation_storage_contract(canonical_minimap, bounded_reader)
            .has_value());
    STATIC_REQUIRE(
        validate_minimizer_generation_storage_contract(canonical_minimap, bounded_reader).error() ==
        minimizer_generation_encoding_error::storage_contract_mismatch);
    STATIC_REQUIRE_FALSE(
        validate_minimizer_generation_storage_contract(canonical_bounded, minimap_reader)
            .has_value());
    STATIC_REQUIRE(
        validate_minimizer_generation_storage_contract(canonical_bounded, minimap_reader).error() ==
        minimizer_generation_encoding_error::storage_contract_mismatch);

    constexpr auto canonical_exact = [canonical_minimap] {
        auto value = canonical_minimap;
        value.producer_backend_id = minimizer_producer_backend::exact_simd;
        value.algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::two_stack_nthash);
        return value;
    }();
    constexpr auto canonical_vulkan = [canonical_minimap] {
        auto value = canonical_minimap;
        value.producer_backend_id = minimizer_producer_backend::vulkan_segment;
        value.algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::vulkan_minimap_integer);
        return value;
    }();
    constexpr auto exact_reader = [minimap_reader] {
        auto value = minimap_reader;
        value.producer_backend_id = minimizer_producer_backend::exact_simd;
        value.algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::two_stack_nthash);
        return value;
    }();
    constexpr auto vulkan_reader = [minimap_reader] {
        auto value = minimap_reader;
        value.producer_backend_id = minimizer_producer_backend::vulkan_segment;
        value.algorithm_identity = test_generation_algorithm_identity(
            minimizer_algorithm::vulkan_minimap_integer);
        return value;
    }();
    STATIC_REQUIRE(
        validate_minimizer_generation_storage_contract(canonical_exact, exact_reader).has_value());
    STATIC_REQUIRE(validate_minimizer_generation_storage_contract(canonical_vulkan, vulkan_reader)
                       .has_value());
    STATIC_REQUIRE_FALSE(
        validate_minimizer_generation_storage_contract(canonical_minimap, exact_reader)
            .has_value());
    STATIC_REQUIRE_FALSE(
        validate_minimizer_generation_storage_contract(canonical_exact, minimap_reader)
            .has_value());
    STATIC_REQUIRE_FALSE(
        validate_minimizer_generation_storage_contract(canonical_minimap, vulkan_reader)
            .has_value());
    STATIC_REQUIRE_FALSE(
        validate_minimizer_generation_storage_contract(canonical_vulkan, minimap_reader)
            .has_value());
    STATIC_REQUIRE(compute_minimizer_generation_storage_contract_checksum(canonical_minimap) !=
                   compute_minimizer_generation_storage_contract_checksum(canonical_exact));
    STATIC_REQUIRE(compute_minimizer_generation_storage_contract_checksum(canonical_minimap) !=
                   compute_minimizer_generation_storage_contract_checksum(canonical_vulkan));

    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {1, 2},
        {2, 4},
        {2, 6},
    };
    vector_replay_minimizer_producer producer{minimizers,
                                              k_bounded_nthash_u32_r7_sum_scheme_identity};
    collecting_minimizer_row_sink sink;
    auto options =
        test_generation_options(100, minimizer_algorithm::bounded_nthash_simd);
    const auto generated = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generated.has_value());
    REQUIRE(generated->manifest.minimizer_scheme_id == k_bounded_nthash_u32_r7_sum_scheme_id);
    REQUIRE(generated->manifest.minimizer_scheme_revision ==
            k_bounded_nthash_u32_r7_sum_scheme_revision);
    REQUIRE(generated->manifest.producer_backend_id ==
            minimizer_producer_backend::bounded_nthash_simd);
    REQUIRE(generated->manifest.producer_identity_revision ==
            k_minimizer_producer_identity_revision);

    for (const minimizer_sketch_backend backend :
         {minimizer_sketch_backend::exact_simd, minimizer_sketch_backend::vulkan_segment}) {
        const auto backend_identity = minimizer_producer_backend_identity_for_backend(backend);
        vector_replay_minimizer_producer physical_producer{
            minimizers, k_minimap_scheme_identity, backend_identity};
        collecting_minimizer_row_sink physical_sink;
        const auto requested_algorithm = minimizer_algorithm_for_explicit_sketch_backend(backend);
        REQUIRE(requested_algorithm.has_value());
        const auto physical_options = test_generation_options(100, *requested_algorithm);
        const auto physical = encode_minimizer_generation(
            physical_producer, physical_sink, physical_options);
        REQUIRE(physical.has_value());
        REQUIRE(physical->manifest.producer_backend_id == backend_identity.id);
        REQUIRE(physical->manifest.producer_identity_revision == backend_identity.revision);
        const auto matching = expected_grouped_soa_generation_identity_for_backend(
            minimizer_generation_layout::grouped_soa_segment_local,
            backend,
            physical_options.algorithm_identity,
            k_minimizer_persisted_dna4_alphabet_code,
            8,
            15,
            10,
            65'534u);
        REQUIRE(matching.has_value());
        REQUIRE(validate_minimizer_generation_storage_contract(physical->manifest, *matching)
                    .has_value());
        REQUIRE_FALSE(validate_minimizer_generation_storage_contract(
                          physical->manifest, test_expected_storage_contract())
                          .has_value());
    }

    vector_replay_minimizer_producer invalid_producer{minimizers,
                                                      k_bounded_nthash_u32_r7_sum_scheme_identity};
    collecting_minimizer_row_sink invalid_sink;
    options.minimizer_scheme_revision = k_minimap_scheme_revision;
    require_wire_error(encode_minimizer_generation(invalid_producer, invalid_sink, options),
                       minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(invalid_producer.scan_count == 0);
    REQUIRE(invalid_sink.rows.empty());

    vector_replay_minimizer_producer mismatched_producer{
        minimizers, k_bounded_nthash_u32_r7_sum_scheme_identity};
    collecting_minimizer_row_sink mismatched_sink;
    auto baseline_options = test_generation_options(100);
    require_wire_error(
        encode_minimizer_generation(mismatched_producer, mismatched_sink, baseline_options),
        minimizer_generation_encoding_error::storage_contract_mismatch);
    REQUIRE(mismatched_producer.scan_count == 0);
    REQUIRE(mismatched_sink.rows.empty());
}

TEST_CASE("generation manifest rejects corrupt headers and invalid geometry",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto manifest = golden_generation_manifest();
    const auto good = *encode_minimizer_generation_manifest(manifest);

    require_wire_error(decode_minimizer_generation_manifest(
                           std::span<const std::byte>{good.data(), good.size() - 1}),
                       minimizer_generation_encoding_error::encoded_size);
    std::array<std::byte, 161> oversized{};
    std::copy(good.begin(), good.end(), oversized.begin());
    require_wire_error(decode_minimizer_generation_manifest(oversized),
                       minimizer_generation_encoding_error::encoded_size);

    auto corrupted = good;
    corrupted[0] ^= std::byte{1};
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::magic);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 4, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::unsupported_storage_layout);
    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 6, 159);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::declared_size);

    // Reject unknown producer backends and revisions.
    corrupted = good;
    corrupted[126] = std::byte{0};
    corrupted[127] = std::byte{0};
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::storage_contract_mismatch);
    for (const std::uint8_t unsupported_backend : {0u, 5u, 255u}) {
        corrupted = good;
        test_write_le<std::uint8_t>(corrupted, 126, unsupported_backend);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    for (const std::uint8_t unsupported_revision : {0u, 2u, 255u}) {
        corrupted = good;
        test_write_le<std::uint8_t>(corrupted, 127, unsupported_revision);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    corrupted = good;
    test_write_le<std::uint8_t>(
        corrupted, 126, static_cast<std::uint8_t>(minimizer_producer_backend::bounded_nthash_simd));
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::storage_contract_mismatch);

    constexpr std::array<std::pair<std::uint16_t, std::uint16_t>, 4> unsupported_schemes{{
        {k_minimap_scheme_id, k_bounded_nthash_u32_r7_sum_scheme_revision},
        {k_bounded_nthash_u32_r7_sum_scheme_id, k_minimap_scheme_revision},
        {2u, k_bounded_nthash_u32_r7_sum_scheme_revision},
        {k_bounded_nthash_u32_r7_sum_scheme_id, 2u},
    }};
    for (const auto [scheme_id, scheme_revision] : unsupported_schemes) {
        INFO("unsupported minimizer scheme " << scheme_id << "/" << scheme_revision);
        auto invalid_scheme = manifest;
        invalid_scheme.minimizer_scheme_id = scheme_id;
        invalid_scheme.minimizer_scheme_revision = scheme_revision;
        require_wire_error(encode_minimizer_generation_manifest(invalid_scheme),
                           minimizer_generation_encoding_error::storage_contract_mismatch);

        corrupted = good;
        test_write_le<std::uint16_t>(corrupted, 122, scheme_id);
        test_write_le<std::uint16_t>(corrupted, 124, scheme_revision);
        require_wire_error(decode_minimizer_generation_manifest(corrupted),
                           minimizer_generation_encoding_error::storage_contract_mismatch);
    }

    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 8, 0);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::generation_id);
    corrupted = good;
    test_write_le<std::uint64_t>(
        corrupted, 8, static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::generation_id);

    corrupted = good;
    test_write_le<std::uint16_t>(corrupted, 120, 1);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_geometry_revision);
    corrupted = good;
    test_write_le<std::uint64_t>(corrupted, 112, 0);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_span);
    corrupted = good;
    test_write_le<std::uint32_t>(corrupted, 88, 2);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_count);
    corrupted = good;
    test_write_le<std::uint32_t>(
        corrupted, 96, static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::directory_chunk_count);

    corrupted = good;
    test_write_le<std::uint64_t>(
        corrupted, 32, static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    test_write_le<std::uint64_t>(corrupted, 112, 1);
    test_write_le<std::uint32_t>(
        corrupted, 88, static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u);
    require_wire_error(decode_minimizer_generation_manifest(corrupted),
                       minimizer_generation_encoding_error::segment_count);

    auto invalid = manifest;
    invalid.layout = minimizer_generation_layout::invalid;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::storage_contract_mismatch);
    invalid = manifest;
    invalid.generation_id = 0;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::generation_id);
    invalid = manifest;
    invalid.position_segment_count = 4;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::segment_count);
    invalid = manifest;
    invalid.position_segment_span_bp = 0;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::segment_span);
    invalid = manifest;
    invalid.segment_geometry_revision = 1;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::segment_geometry_revision);
    invalid = manifest;
    invalid.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::directory_chunk_count);
    invalid = manifest;
    invalid.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u;
    require_wire_error(encode_minimizer_generation_manifest(invalid),
                       minimizer_generation_encoding_error::generation_id);

    auto signed_boundary = manifest;
    signed_boundary.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    signed_boundary.target_length =
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
    signed_boundary.position_segment_span_bp = 1;
    signed_boundary.position_segment_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    signed_boundary.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    const auto signed_boundary_bytes = encode_minimizer_generation_manifest(signed_boundary);
    REQUIRE(signed_boundary_bytes.has_value());
    require_wire_value(decode_minimizer_generation_manifest(*signed_boundary_bytes),
                       signed_boundary);

    auto unrestricted_words = manifest;
    unrestricted_words.generation_id =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    unrestricted_words.source_identity = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.source_content_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.target_length = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.control_count = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.position_count = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.payload_bytes = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.payload_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.directory_bytes = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.directory_checksum = (std::numeric_limits<std::uint64_t>::max)();
    unrestricted_words.position_segment_count = 2;
    unrestricted_words.extent_count = (std::numeric_limits<std::uint32_t>::max)();
    unrestricted_words.directory_chunk_count =
        static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)());
    unrestricted_words.position_segment_span_bp = std::uint64_t{1} << 63u;
    unrestricted_words.position_width = minimizer_scalar_width::u64;
    const auto unrestricted_bytes = encode_minimizer_generation_manifest(unrestricted_words);
    REQUIRE(unrestricted_bytes.has_value());
    require_wire_value(decode_minimizer_generation_manifest(*unrestricted_bytes),
                       unrestricted_words);

    auto empty = manifest;
    empty.target_length = 0;
    empty.position_segment_count = 0;
    REQUIRE(encode_minimizer_generation_manifest(empty).has_value());
    empty.position_segment_span_bp = 0;
    REQUIRE(encode_minimizer_generation_manifest(empty).has_value());
    empty.position_segment_count = 1;
    require_wire_error(encode_minimizer_generation_manifest(empty),
                       minimizer_generation_encoding_error::segment_count);
}

TEST_CASE("standalone manifest codec defers referenced-object consistency",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;

    auto manifest = golden_generation_manifest();
    // These fields can round-trip through the manifest codec without
    // matching any real directory or payload. Generation validation must
    // check those referenced bytes before the index can be used.
    manifest.payload_bytes = 0;
    manifest.payload_checksum = (std::numeric_limits<std::uint64_t>::max)();
    manifest.directory_bytes = 1;
    manifest.directory_checksum = (std::numeric_limits<std::uint64_t>::max)();
    manifest.extent_count = (std::numeric_limits<std::uint32_t>::max)();
    manifest.directory_chunk_count = 0;

    const auto encoded = encode_minimizer_generation_manifest(manifest);
    REQUIRE(encoded.has_value());
    require_wire_value(decode_minimizer_generation_manifest(*encoded), manifest);
}

TEST_CASE("uniform persisted segment cores and bp ranges are exact at boundaries",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const auto manifest = golden_generation_manifest();

    require_wire_value(derive_minimizer_position_segment_bounds(manifest, 0),
                       minimizer_position_segment_bounds{0, 8});
    require_wire_value(derive_minimizer_position_segment_bounds(manifest, 1),
                       minimizer_position_segment_bounds{8, 16});
    require_wire_value(derive_minimizer_position_segment_bounds(manifest, 2),
                       minimizer_position_segment_bounds{16, 20});
    require_wire_error(derive_minimizer_position_segment_bounds(manifest, 3),
                       minimizer_generation_encoding_error::segment_ordinal);

    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 0, 0),
                       minimizer_position_segment_range{0, 0});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 0, 1),
                       minimizer_position_segment_range{0, 1});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 7, 9),
                       minimizer_position_segment_range{0, 2});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 16, 20),
                       minimizer_position_segment_range{2, 3});
    require_wire_value(minimizer_position_segments_for_bp_range(manifest, 20, 20),
                       minimizer_position_segment_range{3, 3});
    require_wire_error(minimizer_position_segments_for_bp_range(manifest, 9, 8),
                       minimizer_generation_encoding_error::bp_range);
    require_wire_error(minimizer_position_segments_for_bp_range(manifest, 0, 21),
                       minimizer_generation_encoding_error::bp_range);

    auto huge = manifest;
    huge.target_length = (std::numeric_limits<std::uint64_t>::max)();
    huge.position_segment_span_bp = std::uint64_t{1} << 63u;
    huge.position_segment_count = 2;
    huge.position_width = minimizer_scalar_width::u64;
    const auto last = derive_minimizer_position_segment_bounds(huge, 1);
    REQUIRE(last.has_value());
    REQUIRE(last->begin_bp == (std::uint64_t{1} << 63u));
    REQUIRE(last->end_bp == (std::numeric_limits<std::uint64_t>::max)());

    auto empty = manifest;
    empty.target_length = 0;
    empty.position_segment_span_bp = 0;
    empty.position_segment_count = 0;
    require_wire_value(minimizer_position_segments_for_bp_range(empty, 0, 0),
                       minimizer_position_segment_range{0, 0});
    require_wire_error(derive_minimizer_position_segment_bounds(empty, 0),
                       minimizer_generation_encoding_error::segment_ordinal);

    STATIC_REQUIRE(sql_bigint_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())));
    STATIC_REQUIRE_FALSE(sql_bigint_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) + 1u));
    STATIC_REQUIRE(sql_integer_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())));
    STATIC_REQUIRE_FALSE(sql_integer_can_represent(
        static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u));

    STATIC_REQUIRE(k_minimizer_segment_geometry_revision == 2u);
    STATIC_REQUIRE(k_position_extent_sub_key_base == (index_sub_key_t{1} << 61));
    STATIC_REQUIRE(k_position_extent_sub_key_base * 2 == k_temporary_run_sub_key_base);
    constexpr auto first_position_key = derive_minimizer_position_extent_sub_key(0);
    constexpr auto last_position_key =
        derive_minimizer_position_extent_sub_key(k_position_extent_sub_key_base - 1);
    STATIC_REQUIRE(first_position_key.has_value());
    STATIC_REQUIRE(*first_position_key == k_position_extent_sub_key_base);
    STATIC_REQUIRE(last_position_key.has_value());
    STATIC_REQUIRE(*last_position_key == k_temporary_run_sub_key_base - 1);
    STATIC_REQUIRE_FALSE(derive_minimizer_position_extent_sub_key(-1).has_value());
    STATIC_REQUIRE_FALSE(
        derive_minimizer_position_extent_sub_key(k_position_extent_sub_key_base).has_value());
    STATIC_REQUIRE(project_minimizer_extent_sub_key(0, false).has_value());
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(0, false) == 0);
    STATIC_REQUIRE(
        project_minimizer_extent_sub_key(k_position_extent_sub_key_base, true).has_value());
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(k_position_extent_sub_key_base, true) == 0);
    STATIC_REQUIRE(*project_minimizer_extent_sub_key(k_temporary_run_sub_key_base - 1, true) ==
                   k_position_extent_sub_key_base - 1);
    STATIC_REQUIRE_FALSE(
        project_minimizer_extent_sub_key((std::numeric_limits<index_sub_key_t>::min)(), true)
            .has_value());
    STATIC_REQUIRE_FALSE(
        project_minimizer_extent_sub_key(k_position_extent_sub_key_base - 1, true).has_value());
    STATIC_REQUIRE_FALSE(
        project_minimizer_extent_sub_key(k_position_extent_sub_key_base, false).has_value());
    STATIC_REQUIRE_FALSE(
        project_minimizer_extent_sub_key(k_temporary_run_sub_key_base, true).has_value());
}

TEST_CASE("lookup and persistence minimizer roles are atomically split",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    STATIC_REQUIRE(minimizer_lookup_index<minimizer_hashmap_fragment_index>);
    STATIC_REQUIRE(persistable_minimizer_index<minimizer_hashmap_fragment_index>);
    STATIC_REQUIRE(minimizer_lookup_index<prepared_grouped_soa_query_reader>);
    STATIC_REQUIRE_FALSE(persistable_minimizer_index<prepared_grouped_soa_query_reader>);

    constexpr segment_unit_range range{1, 9, 32, 256};
    STATIC_REQUIRE(range.cs_u == 1);
    STATIC_REQUIRE(range.ce_u == 9);
    STATIC_REQUIRE(range.core_lo_bp == 32);
    STATIC_REQUIRE(range.core_hi_bp == 256);
}

TEST_CASE("multi-span lookup carries one global occurrence decision",
          "[component:bioseq][layer:unit][feature:index]") {
    using namespace bioseq::indexing;
    struct segmented_fixture_index {
        minimizer_lookup_spans result{};
        [[nodiscard]] minimizer_lookup_spans lookup_spans(std::uint64_t) const noexcept {
            return result;
        }
        [[nodiscard]] bioseq::storage::sequence_size_t target_length() const noexcept {
            return 4096;
        }
    };
    struct unsafe_owning_lookup_index {
        [[nodiscard]] std::vector<std::uint64_t> lookup(std::uint64_t) const {
            return {};
        }
        [[nodiscard]] bioseq::storage::sequence_size_t target_length() const noexcept {
            return 0;
        }
    };
    STATIC_REQUIRE(minimizer_lookup_index<segmented_fixture_index>);
    STATIC_REQUIRE(segmented_minimizer_lookup_index<segmented_fixture_index>);
    STATIC_REQUIRE_FALSE(contiguous_minimizer_lookup_index<segmented_fixture_index>);
    STATIC_REQUIRE_FALSE(contiguous_minimizer_lookup_index<unsafe_owning_lookup_index>);
    STATIC_REQUIRE_FALSE(minimizer_lookup_index<unsafe_owning_lookup_index>);

    const std::array<std::uint64_t, 2> first{4, 8};
    const std::array<std::uint64_t, 3> second{12, 16, 20};
    const std::array<minimizer_position_span, 2> spans{
        minimizer_position_span{first},
        minimizer_position_span{second},
    };
    const auto complete = minimizer_lookup_spans::segmented(spans, /*global_occurrence_count=*/5);
    REQUIRE(complete.positions_complete());
    REQUIRE(complete.global_occurrence_count() == 5);
    REQUIRE(complete.contiguous_positions().empty());
    REQUIRE(complete.segmented_positions().size() == 2);
    REQUIRE(complete.well_formed());

    const auto malformed_count =
        minimizer_lookup_spans::segmented(spans, /*global_occurrence_count=*/4);
    REQUIRE_FALSE(malformed_count.well_formed());
    const auto unavailable = minimizer_lookup_spans::positions_unavailable(1'000);
    REQUIRE_FALSE(unavailable.positions_complete());
    REQUIRE(unavailable.global_occurrence_count() == 1'000);
    REQUIRE(unavailable.well_formed());

    segmented_fixture_index index{complete};
    const auto adapted = lookup_minimizer_spans(index, 7);
    REQUIRE(adapted.well_formed());
    REQUIRE(adapted.global_occurrence_count() == 5);
}

TEST_CASE("directory entry has an exact 128-byte little-endian layout",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr grouped_soa_bucket_directory_entry entry{
        .partition_id = 0x11223344U,
        .position_segment = 0x10203040,
        .lo_hash = 0x01020304ULL,
        .hi_hash = 0x11121314ULL,
        .sub_key = 0x11223344LL,
        .control_first_chunk = 0,
        .control_chunk_count = 2,
        .position_first_chunk = 0,
        .position_chunk_count = 3,
        .control_element_count = 768,
        .position_element_count = 2304,
        .control_encoded_bytes = 6152,
        .position_encoded_bytes = 4608,
        .control_checksum = 0x3132333435363738ULL,
        .position_checksum = 0x4142434445464748ULL,
        .shape_offset_width = minimizer_scalar_width::u16,
    };
    constexpr auto encoded =
        encode_minimizer_bucket_dir_entry_record(entry, k_test_grouped_soa_profile);
    STATIC_REQUIRE(encoded.has_value());
    STATIC_REQUIRE(encoded->size() == 128);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 0) == entry.partition_id);
    STATIC_REQUIRE(std::bit_cast<std::int32_t>(test_read_le<std::uint32_t>(*encoded, 4)) ==
                   entry.position_segment);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 8) == entry.lo_hash);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 16) == entry.hi_hash);
    STATIC_REQUIRE(std::bit_cast<std::int64_t>(test_read_le<std::uint64_t>(*encoded, 24)) ==
                   entry.sub_key);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 32) == 0);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 36) == 2);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 40) == 0);
    STATIC_REQUIRE(test_read_le<std::uint32_t>(*encoded, 44) == 3);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 48) == 768);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 56) == 2304);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 64) == 6152);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 72) == 4608);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 80) == entry.control_checksum);
    STATIC_REQUIRE(test_read_le<std::uint64_t>(*encoded, 88) == entry.position_checksum);
    constexpr auto decoded =
        decode_minimizer_bucket_dir_entry(*encoded, k_test_grouped_soa_profile);
    STATIC_REQUIRE(decoded.has_value());
    STATIC_REQUIRE(*decoded == entry);

    require_wire_error(decode_minimizer_bucket_dir_entry(
                           std::span<const std::byte>{encoded->data(), encoded->size() - 1u},
                           k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::encoded_size);
    auto invalid = entry;
    invalid.position_segment = -1;
    require_wire_error(
        encode_minimizer_bucket_dir_entry_record(invalid, k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::position_segment);
    invalid = entry;
    invalid.lo_hash = invalid.hi_hash + 1u;
    require_wire_error(
        encode_minimizer_bucket_dir_entry_record(invalid, k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::hash_range);
    invalid = entry;
    invalid.sub_key = k_position_extent_sub_key_base;
    require_wire_error(
        encode_minimizer_bucket_dir_entry_record(invalid, k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::sub_key);
    invalid = entry;
    invalid.control_encoded_bytes -= 16u;
    require_wire_error(
        encode_minimizer_bucket_dir_entry_record(invalid, k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::byte_count);
    invalid = entry;
    invalid.position_first_chunk = 1;
    require_wire_error(
        encode_minimizer_bucket_dir_entry_record(invalid, k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::chunk_range);
}

TEST_CASE("grouped_soa_directory_view owns bytes and rejects unstable directory order",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const grouped_soa_bucket_directory_entry first{
        .partition_id = 0,
        .position_segment = 0,
        .lo_hash = 10,
        .hi_hash = 20,
        .sub_key = 0,
        .control_first_chunk = 0,
        .control_chunk_count = 1,
        .position_first_chunk = 0,
        .position_chunk_count = 1,
        .control_element_count = 1,
        .position_element_count = 1,
        .control_encoded_bytes = 16,
        .position_encoded_bytes = 2,
        .control_checksum = 1,
        .position_checksum = 2,
        .shape_offset_width = minimizer_scalar_width::u16,
    };
    auto second = first;
    second.partition_id = 1;
    second.lo_hash = 21;
    second.hi_hash = 30;
    second.sub_key = 1;
    const std::array entries{first, second};
    auto bytes = encode_minimizer_bucket_directory(entries, k_test_grouped_soa_profile);
    REQUIRE(bytes.has_value());
    auto view = grouped_soa_directory_view::decode(*bytes, k_test_grouped_soa_profile);
    REQUIRE(view.has_value());
    REQUIRE(view->entries().size() == 2);
    REQUIRE(view->find(0, 10)->partition_id == 0);
    REQUIRE(view->find(0, 30)->partition_id == 1);
    REQUIRE(view->find(0, 31) == nullptr);
    REQUIRE(view->find(1, 10) == nullptr);

    // The view owns a copy, so changing the source bytes must leave it intact.
    (*bytes)[8] ^= std::byte{0xff};
    REQUIRE(view->entries()[0].lo_hash == 10);
    REQUIRE(view->encoded_bytes()[8] != (*bytes)[8]);

    auto overlap = second;
    overlap.lo_hash = first.hi_hash;
    const std::array overlapping{first, overlap};
    require_wire_error(encode_minimizer_bucket_directory(overlapping, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::directory_order);
    auto wrong_ordinal = second;
    wrong_ordinal.partition_id = 7;
    const std::array wrong_ordinals{first, wrong_ordinal};
    require_wire_error(
        encode_minimizer_bucket_directory(wrong_ordinals, k_test_grouped_soa_profile),
        minimizer_generation_encoding_error::partition_id);
    std::array<std::byte, 127> truncated{};
    require_wire_error(grouped_soa_directory_view::decode(truncated, k_test_grouped_soa_profile),
                       minimizer_generation_encoding_error::directory_size);
}

TEST_CASE("deterministic encoder fixes core ownership and policy bytes",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    constexpr std::uint64_t span = k_minimizer_preferred_segment_span_bp;
    constexpr auto encoded_position = [](std::uint64_t position, std::uint64_t strand) {
        return (position << 1u) | strand;
    };
    std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {100, encoded_position(span, 1)},
        {7, encoded_position(span - 1u, 0)},
        {100, encoded_position(9, 1)},
        {9, encoded_position(span + 3u, 0)},
        {100, encoded_position(3, 0)},
    };
    const auto options = test_generation_options(span + 100u);

    vector_replay_minimizer_producer direct_producer{minimizers};
    collecting_minimizer_row_sink direct_sink;
    auto direct = encode_minimizer_generation(direct_producer, direct_sink, options);
    REQUIRE(direct.has_value());
    REQUIRE(direct_producer.scan_count == 3);

    std::reverse(minimizers.begin(), minimizers.end());
    vector_replay_minimizer_producer reversed_producer{minimizers};
    collecting_minimizer_row_sink reversed_sink;
    auto reversed_input = encode_minimizer_generation(reversed_producer, reversed_sink, options);
    REQUIRE(reversed_input.has_value());
    require_encoded_generations_equal_normalized(
        *direct, direct_sink.rows, *reversed_input, reversed_sink.rows);

    REQUIRE(direct->manifest.position_segment_span_bp == span);
    REQUIRE(direct->manifest.position_segment_count == 2);
    REQUIRE(direct->manifest.segment_geometry_revision == k_minimizer_segment_geometry_revision);
    REQUIRE(direct->manifest.gpu_abi_revision == k_grouped_soa_gpu_abi_revision);
    REQUIRE(direct->head.storage_contract_checksum ==
            compute_minimizer_generation_storage_contract_checksum(direct->manifest));
    REQUIRE(
        validate_minimizer_generation_metadata(direct->head, direct->manifest, direct->directory)
            .has_value());

    bool saw_first_core = false;
    bool saw_second_core = false;
    for (const grouped_soa_bucket_directory_entry& entry : direct->directory.entries()) {
        saw_first_core = saw_first_core || entry.position_segment == 0;
        saw_second_core = saw_second_core || entry.position_segment == 1;
        REQUIRE(entry.sub_key >= 0);
        REQUIRE(entry.sub_key < k_position_extent_sub_key_base);
        REQUIRE(entry.control_first_chunk == 0);
        REQUIRE(entry.position_first_chunk == 0);
        const auto position_sub_key = derive_minimizer_position_extent_sub_key(entry.sub_key);
        REQUIRE(position_sub_key.has_value());
        const auto control_first_row =
            std::ranges::find_if(direct_sink.rows, [entry](const collected_minimizer_row& row) {
                return row.address.position_segment == entry.position_segment &&
                       row.address.sub_key == entry.sub_key && row.address.chunk_no == 0;
            });
        const auto position_first_row = std::ranges::find_if(
            direct_sink.rows, [entry, position_sub_key](const collected_minimizer_row& row) {
                return row.address.position_segment == entry.position_segment &&
                       row.address.sub_key == *position_sub_key && row.address.chunk_no == 0;
            });
        REQUIRE(control_first_row != direct_sink.rows.end());
        REQUIRE(position_first_row != direct_sink.rows.end());
        const auto core = derive_minimizer_position_segment_bounds(
            direct->manifest, static_cast<std::uint32_t>(entry.position_segment));
        REQUIRE(core.has_value());
        const auto position_bytes = collect_encoded_extent(direct_sink.rows, entry, false);
        for (std::size_t offset = 0; offset < position_bytes.size(); offset += 4u) {
            const std::uint32_t local = test_read_le<std::uint32_t>(position_bytes, offset);
            const std::uint64_t local_bp = local >> 1u;
            REQUIRE(local_bp < core->end_bp - core->begin_bp);
            const std::uint64_t global_bp = core->begin_bp + local_bp;
            REQUIRE(global_bp >= core->begin_bp);
            REQUIRE(global_bp < core->end_bp);
        }
        REQUIRE(entry.control_encoded_bytes % 8u == 0);
        REQUIRE(entry.position_encoded_bytes % 4u == 0);
    }
    REQUIRE(saw_first_core);
    REQUIRE(saw_second_core);
    REQUIRE(direct_sink.max_callback_bytes <= k_grouped_soa_extent_payload_bytes);
    for (const collected_minimizer_row& row : direct_sink.rows) {
        REQUIRE(row.bytes.size() <= k_grouped_soa_extent_payload_bytes);
        REQUIRE(row.row_checksum ==
                crc64_index_chunk_row(row.address.generation_id,
                                      row.address.position_segment,
                                      row.address.sub_key,
                                      row.address.chunk_no,
                                      static_cast<std::uint32_t>(row.bytes.size()),
                                      row.bytes));
    }
    REQUIRE(direct_sink.rows.back().address == direct->manifest_row.address);
    REQUIRE(std::ranges::none_of(direct_sink.rows, [direct](const collected_minimizer_row& row) {
        return row.address == direct->unpublished_head_row.address;
    }));
    require_complete_generation_rows(*direct, direct_sink.rows);
}

TEST_CASE("grouped-SoA planned metadata precedes the direct grouped-SoA payload",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {11u, 4u << 1u},
        {7u, 12u << 1u},
        {11u, 70u << 1u},
        {19u, 76u << 1u},
    };
    auto options = test_grouped_soa_generation_options(128u, 10u);
    options.position_segment_span_bp = 64u;
    vector_replay_minimizer_producer producer{minimizers};
    planned_direct_minimizer_sink sink;

    STATIC_REQUIRE(planned_grouped_soa_generation_sink<planned_direct_minimizer_sink>);
    STATIC_REQUIRE(grouped_soa_minimizer_payload_sink<planned_direct_minimizer_sink>);
    const auto generation = encode_minimizer_generation(producer, sink, options);
    REQUIRE(generation.has_value());
    REQUIRE(sink.storage_plan.has_value());
    REQUIRE_FALSE(sink.payload_rows.empty());
    REQUIRE(sink.events.size() == 1u + sink.payload_rows.size() + sink.ordinary_rows.size());
    REQUIRE(sink.events.front() == planned_direct_sink_event::begin);
    REQUIRE(std::ranges::count(sink.events, planned_direct_sink_event::begin) == 1u);
    for (std::size_t index = 0u; index < sink.payload_rows.size(); ++index) {
        REQUIRE(sink.events[index + 1u] == planned_direct_sink_event::payload);
    }
    for (std::size_t index = 1u + sink.payload_rows.size(); index < sink.events.size(); ++index) {
        REQUIRE(sink.events[index] == planned_direct_sink_event::row);
    }

    const auto align_up_8 = [](std::uint64_t value) noexcept {
        return (value + 7u) & ~std::uint64_t{7u};
    };
    const std::uint64_t expected_directory_offset =
        align_up_8(4096u + generation->manifest.payload_bytes);
    const std::uint64_t expected_manifest_offset =
        align_up_8(expected_directory_offset + generation->directory.encoded_bytes().size());
    const grouped_soa_storage_plan_view expected_plan{
        .layout = options.layout,
        .generation_id = options.generation_id,
        .payload_bytes = generation->manifest.payload_bytes,
        .directory_bytes = generation->directory.encoded_bytes().size(),
        .manifest_bytes = minimizer_generation_manifest::encoded_bytes,
        .directory_offset = expected_directory_offset,
        .manifest_offset = expected_manifest_offset,
        .file_bytes = expected_manifest_offset + minimizer_generation_manifest::encoded_bytes,
    };
    REQUIRE(*sink.storage_plan == expected_plan);
}

TEST_CASE("grouped-SoA direct payload is row-exact without tuple callbacks",
          "[component:bioseq][layer:unit][feature:storage]") {
    using namespace bioseq::indexing;
    const std::vector<std::pair<std::uint64_t, std::uint64_t>> minimizers{
        {11u, 4u << 1u},
        {7u, 12u << 1u},
        {11u, 70u << 1u},
        {19u, 76u << 1u},
    };
    auto options = test_grouped_soa_generation_options(128u, 10u);
    options.position_segment_span_bp = 64u;

    STATIC_REQUIRE_FALSE(planned_grouped_soa_generation_sink<row_only_collecting_minimizer_sink>);
    STATIC_REQUIRE_FALSE(grouped_soa_minimizer_payload_sink<row_only_collecting_minimizer_sink>);
    vector_replay_minimizer_producer row_producer{minimizers};
    row_only_collecting_minimizer_sink row_sink;
    const auto row_generation = encode_minimizer_generation(row_producer, row_sink, options);
    REQUIRE(row_generation.has_value());

    vector_replay_minimizer_producer direct_producer{minimizers};
    planned_direct_minimizer_sink direct_sink;
    const auto direct_generation =
        encode_minimizer_generation(direct_producer, direct_sink, options);
    REQUIRE(direct_generation.has_value());
    REQUIRE(row_generation->head == direct_generation->head);
    REQUIRE(row_generation->manifest == direct_generation->manifest);
    REQUIRE(std::ranges::equal(row_generation->directory.encoded_bytes(),
                               direct_generation->directory.encoded_bytes()));

    std::vector<collected_minimizer_row> direct_rows{direct_sink.payload_rows.begin(),
                                                     direct_sink.payload_rows.end()};
    direct_rows.insert(
        direct_rows.end(), direct_sink.ordinary_rows.begin(), direct_sink.ordinary_rows.end());
    REQUIRE(row_sink.rows.size() == direct_rows.size());
    REQUIRE(direct_sink.ordinary_payload_row_callbacks == 0u);
    REQUIRE(std::ranges::all_of(direct_sink.ordinary_rows, [](const collected_minimizer_row& row) {
        return row.address.position_segment == -1;
    }));

    crc64_ecma182_accumulator direct_payload_row_checksums;
    for (std::size_t index = 0u; index < direct_rows.size(); ++index) {
        const collected_minimizer_row& expected = row_sink.rows[index];
        const collected_minimizer_row& actual = direct_rows[index];
        REQUIRE(actual.address == expected.address);
        REQUIRE(actual.bytes == expected.bytes);
        REQUIRE(actual.row_checksum == expected.row_checksum);
        REQUIRE(actual.row_checksum ==
                crc64_index_chunk_row(actual.address.generation_id,
                                      actual.address.position_segment,
                                      actual.address.sub_key,
                                      actual.address.chunk_no,
                                      static_cast<std::uint32_t>(actual.bytes.size()),
                                      actual.bytes));
        if (index < direct_sink.payload_rows.size()) {
            direct_payload_row_checksums.update_little_endian(actual.row_checksum);
        }
    }
    REQUIRE(direct_payload_row_checksums.checksum() ==
            direct_generation->manifest.payload_checksum);
    require_complete_generation_rows(*row_generation, row_sink.rows);
    require_complete_generation_rows(*direct_generation, direct_rows);
}

} // namespace bioseq::simd::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();
