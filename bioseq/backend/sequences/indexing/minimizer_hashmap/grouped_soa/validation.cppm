/**
 * @file minimizer_hashmap/grouped_soa/validation.cppm
 * @brief Validates grouped-SoA geometry, identity, and metadata equations.
 */
export module bioseq.indexing.minimizer_hashmap:grouped_soa_validation;

import std;
import :exact_simd;
import bioseq.common.logging;
import bioseq.common.seqan_adapter;
import bioseq.indexing.type;
import bioseq.indexing.storage;
import bioseq.indexing.traits;
import bioseq.indexing.minimizer_index;
import bioseq.indexing.compact_hash32_direct_index;
import bioseq.indexing.hash_major_minimizer_index;
import bioseq.common.checked_arithmetic;
import bioseq.sequences;
import bioseq.sequences.kmers;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.sequences.kmers.vulkan_minimizers;
import bioseq.storage.scanner;
import bioseq.storage.lazy_sequence;
import bioseq.storage.size;
import bioseq.chaining.core;
import bioseq.common.radix_sort;
import bioseq.instrumentation;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;
import bioseq.parallelism.pipeline_ops;
import :grouped_soa_contracts;
import :grouped_soa_codecs;

export namespace bioseq::indexing {

/// Half-open logical-rank bounds for one persisted position segment.
struct minimizer_position_segment_bounds {
    std::uint64_t begin_bp = 0u;
    std::uint64_t end_bp = 0u;

    [[nodiscard]] bool operator==(const minimizer_position_segment_bounds&) const = default;
};

/// Derive the overflow-safe half-open core for one persisted segment ordinal.
[[nodiscard]] constexpr std::expected<minimizer_position_segment_bounds,
                                      minimizer_generation_encoding_error>
derive_minimizer_position_segment_bounds(const minimizer_generation_manifest& manifest,
                                         std::uint32_t ordinal) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    if (ordinal >= manifest.position_segment_count) {
        return std::unexpected(minimizer_generation_encoding_error::segment_ordinal);
    }
    if (manifest.position_segment_span_bp != 0 &&
        ordinal > (std::numeric_limits<std::uint64_t>::max)() / manifest.position_segment_span_bp) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const std::uint64_t begin =
        static_cast<std::uint64_t>(ordinal) * manifest.position_segment_span_bp;
    if (begin >= manifest.target_length) {
        return std::unexpected(minimizer_generation_encoding_error::segment_ordinal);
    }
    const std::uint64_t remaining = manifest.target_length - begin;
    const std::uint64_t width = std::min(manifest.position_segment_span_bp, remaining);
    return minimizer_position_segment_bounds{begin, begin + width};
}

/// Map a half-open base-pair range to the covering persisted segment ordinals.
[[nodiscard]] constexpr std::expected<minimizer_position_segment_range,
                                      minimizer_generation_encoding_error>
minimizer_position_segments_for_bp_range(const minimizer_generation_manifest& manifest,
                                         std::uint64_t begin_bp,
                                         std::uint64_t end_bp) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    if (begin_bp > end_bp || end_bp > manifest.target_length) {
        return std::unexpected(minimizer_generation_encoding_error::bp_range);
    }
    if (begin_bp == end_bp) {
        const std::uint32_t ordinal =
            begin_bp == manifest.target_length
                ? manifest.position_segment_count
                : static_cast<std::uint32_t>(begin_bp / manifest.position_segment_span_bp);
        return minimizer_position_segment_range{ordinal, ordinal};
    }
    const std::uint64_t first = begin_bp / manifest.position_segment_span_bp;
    const std::uint64_t last = (end_bp - 1u) / manifest.position_segment_span_bp;
    if (last >= manifest.position_segment_count || !sql_integer_can_represent(first) ||
        !sql_integer_can_represent(last + 1u)) {
        return std::unexpected(minimizer_generation_encoding_error::segment_ordinal);
    }
    return minimizer_position_segment_range{static_cast<std::uint32_t>(first),
                                            static_cast<std::uint32_t>(last + 1u)};
}

/// Compute the storage-contract identity from every named semantic/ABI axis.
[[nodiscard]] constexpr std::uint64_t compute_minimizer_generation_storage_contract_checksum(
    const minimizer_generation_manifest& manifest) noexcept {
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    crc64_ecma182_accumulator crc;
    crc.update_little_endian(manifest.alphabet_code);
    crc.update_little_endian(manifest.storage_unit_byte_width);
    crc.update_little_endian(manifest.kmer_size);
    crc.update_little_endian(manifest.window_size);
    crc.update_little_endian(k_minimizer_hash_semantics_revision);
    crc.update_little_endian(k_minimizer_segment_local_position_encoding_revision);
    crc.update_little_endian(static_cast<std::uint32_t>(k_grouped_soa_payload_layout_abi_revision));
    crc.update_little_endian(manifest.segment_geometry_revision);
    crc.update_little_endian(manifest.position_segment_span_bp);
    crc.update_little_endian(manifest.position_segment_count);
    crc.update_little_endian(manifest.gpu_abi_revision);
    crc.update_little_endian(static_cast<std::uint8_t>(manifest.producer_backend_id));
    crc.update_little_endian(manifest.producer_identity_revision);
    // Named schemes extend the checksum with their frozen wire
    // identity so otherwise identical indexes cannot cross scheme boundaries.
    if (manifest.minimizer_scheme_id != k_minimap_scheme_id ||
        manifest.minimizer_scheme_revision != k_minimap_scheme_revision) {
        crc.update_little_endian(manifest.minimizer_scheme_id);
        crc.update_little_endian(manifest.minimizer_scheme_revision);
    }
    crc.update_little_endian(static_cast<std::uint16_t>(manifest.layout));
    if (profile) {
        crc.update_little_endian(std::uint16_t{0u});
        crc.update_little_endian(static_cast<std::uint16_t>(profile->position_width));
        crc.update_little_endian(profile->max_hash);
        crc.update_little_endian(static_cast<std::uint8_t>(profile->hash_width));
        crc.update_little_endian(static_cast<std::uint8_t>(profile->position_width));
        crc.update_little_endian(static_cast<std::uint8_t>(profile->count_width));
        crc.update_little_endian(profile->max_occ_cap);
    }
    return crc.checksum();
}

/// Reject a persisted generation that does not match the caller's biological
/// minimizer scheme or the fixed representation axes used to consume it.
[[nodiscard]] constexpr std::expected<void, minimizer_generation_encoding_error>
validate_minimizer_generation_storage_contract(
    const minimizer_generation_manifest& manifest,
    const expected_minimizer_generation_identity& expected) noexcept {
    if (auto error = minimizer_generation_wire_detail::validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    const auto manifest_profile = derive_grouped_soa_minimizer_profile(manifest);
    const auto expected_profile = manifest_profile;
    const minimizer_producer_identity expected_producer_backend{
        .id = expected.producer_backend_id,
        .revision = expected.producer_identity_revision,
    };
    if (!manifest_profile || !expected_profile ||
        !is_supported_minimizer_producer_backend_identity(expected_producer_backend) ||
        minimizer_scheme_identity_for_producer_backend(expected_producer_backend) !=
            expected_profile->minimizer_scheme ||
        *manifest_profile != *expected_profile ||
        manifest.gpu_abi_revision != manifest_profile->gpu_abi_revision ||
        expected.gpu_abi_revision != expected_profile->gpu_abi_revision ||
        manifest.layout != expected.layout || manifest.alphabet_code != expected.alphabet_code ||
        manifest.storage_unit_byte_width != expected.storage_unit_byte_width ||
        manifest.kmer_size != expected.kmer_size || manifest.window_size != expected.window_size ||
        manifest.gpu_abi_revision != expected.gpu_abi_revision ||
        manifest.minimizer_scheme_id != expected.minimizer_scheme_id ||
        manifest.minimizer_scheme_revision != expected.minimizer_scheme_revision ||
        manifest.producer_backend_id != expected.producer_backend_id ||
        manifest.producer_identity_revision != expected.producer_identity_revision ||
        !expected.max_occ_cap || *expected.max_occ_cap != manifest_profile->max_occ_cap) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return {};
}

/// Validate head/manifest/directory equations once all metadata objects exist.
[[nodiscard]] inline std::expected<void, minimizer_generation_encoding_error>
validate_minimizer_generation_metadata(const minimizer_generation_head& head,
                                       const minimizer_generation_manifest& manifest,
                                       const grouped_soa_directory_view& directory) noexcept {
    if (auto error = minimizer_generation_wire_detail::validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    if (!profile || directory.profile() != *profile) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (head.layout != manifest.layout) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (head.generation_id != manifest.generation_id ||
        head.source_identity != manifest.source_identity ||
        head.source_content_checksum != manifest.source_content_checksum) {
        return std::unexpected(minimizer_generation_encoding_error::identity_mismatch);
    }
    const auto manifest_bytes = encode_minimizer_generation_manifest_record(manifest);
    if (!manifest_bytes ||
        head.manifest_checksum != crc64_ecma182(std::span<const std::byte>{*manifest_bytes})) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    if (manifest.gpu_abi_revision != profile->gpu_abi_revision ||
        head.storage_contract_checksum !=
            compute_minimizer_generation_storage_contract_checksum(manifest)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (manifest.directory_bytes != directory.encoded_bytes().size() ||
        manifest.directory_checksum != crc64_ecma182(directory.encoded_bytes())) {
        return std::unexpected(minimizer_generation_encoding_error::checksum);
    }
    const std::uint32_t extent_payload_bytes = minimizer_extent_payload_bytes(manifest.layout);
    if (extent_payload_bytes == 0) {
        return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    const std::uint64_t expected_directory_chunks =
        manifest.directory_bytes / extent_payload_bytes +
        (manifest.directory_bytes % extent_payload_bytes != 0 ? 1u : 0u);
    if (expected_directory_chunks != manifest.directory_chunk_count) {
        return std::unexpected(minimizer_generation_encoding_error::chunk_range);
    }

    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    for (const grouped_soa_bucket_directory_entry& entry : directory.entries()) {
        if (static_cast<std::uint64_t>(entry.position_segment) >= manifest.position_segment_count) {
            return std::unexpected(minimizer_generation_encoding_error::position_segment);
        }
        const std::uint64_t expected_control_chunks =
            entry.control_encoded_bytes / extent_payload_bytes +
            (entry.control_encoded_bytes % extent_payload_bytes != 0 ? 1u : 0u);
        const std::uint64_t expected_position_chunks =
            entry.position_encoded_bytes / extent_payload_bytes +
            (entry.position_encoded_bytes % extent_payload_bytes != 0 ? 1u : 0u);
        if (entry.control_first_chunk != 0 ||
            entry.control_chunk_count != expected_control_chunks ||
            entry.position_first_chunk != 0 ||
            entry.position_chunk_count != expected_position_chunks) {
            return std::unexpected(minimizer_generation_encoding_error::chunk_range);
        }
        if (control_count >
                (std::numeric_limits<std::uint64_t>::max)() - entry.control_element_count ||
            position_count >
                (std::numeric_limits<std::uint64_t>::max)() - entry.position_element_count ||
            payload_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - entry.control_encoded_bytes ||
            payload_bytes + entry.control_encoded_bytes >
                (std::numeric_limits<std::uint64_t>::max)() - entry.position_encoded_bytes) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        control_count += entry.control_element_count;
        position_count += entry.position_element_count;
        payload_bytes += entry.control_encoded_bytes + entry.position_encoded_bytes;
    }
    if (directory.entries().size() > (std::numeric_limits<std::uint32_t>::max)() / 2u ||
        manifest.extent_count != directory.entries().size() * 2u ||
        manifest.control_count != control_count || manifest.position_count != position_count ||
        manifest.payload_bytes != payload_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::element_count);
    }
    return {};
}

} // namespace bioseq::indexing
