/**
 * @file minimizer_hashmap/grouped_soa/codecs.cppm
 * @brief Encodes and decodes grouped-SoA metadata and directories.
 */
module;

#include <new>

export module bioseq.indexing.minimizer_hashmap:grouped_soa_codecs;

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

namespace bioseq::indexing::minimizer_generation_wire_detail {

/// Write an unsigned scalar to a fixed little-endian byte range.
template <class p_uint>
    requires(std::is_unsigned_v<p_uint>)
constexpr void write_le(std::span<std::byte> output, std::size_t offset, p_uint value) noexcept {
    for (std::size_t byte = 0; byte < sizeof(p_uint); ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & p_uint{0xff});
        value >>= 8u;
    }
}

/// Read an unsigned scalar from a fixed little-endian byte range.
template <class p_uint>
    requires(std::is_unsigned_v<p_uint>)
[[nodiscard]] constexpr p_uint read_le(std::span<const std::byte> input,
                                       std::size_t offset) noexcept {
    p_uint value = 0;
    for (std::size_t byte = 0; byte < sizeof(p_uint); ++byte) {
        value |= static_cast<p_uint>(std::to_integer<std::uint8_t>(input[offset + byte]))
                 << (byte * 8u);
    }
    return value;
}

/// Write a width-tagged grouped-SoA scalar when the range and value fit.
[[nodiscard]] constexpr bool write_shape_scalar(std::span<std::byte> output,
                                                std::size_t offset,
                                                minimizer_scalar_width width,
                                                std::uint64_t value) noexcept {
    const std::size_t bytes = static_cast<std::uint8_t>(width);
    if (!is_minimizer_scalar_width(width) || offset > output.size() ||
        bytes > output.size() - offset || value > minimizer_scalar_width_max(width)) {
        return false;
    }
    for (std::size_t byte = 0; byte < bytes; ++byte) {
        output[offset + byte] = static_cast<std::byte>(value & 0xffu);
        value >>= 8u;
    }
    return true;
}

/// Read a width-tagged grouped-SoA scalar when the range is complete.
[[nodiscard]] constexpr std::optional<std::uint64_t> read_shape_scalar(
    std::span<const std::byte> input, std::size_t offset, minimizer_scalar_width width) noexcept {
    const std::size_t bytes = static_cast<std::uint8_t>(width);
    if (!is_minimizer_scalar_width(width) || offset > input.size() ||
        bytes > input.size() - offset) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t byte = 0; byte < bytes; ++byte) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + byte]))
                 << (byte * 8u);
    }
    return value;
}

/// Return whether every byte in the half-open interval is zero.
[[nodiscard]] constexpr bool
all_zero(std::span<const std::byte> input, std::size_t begin, std::size_t end) noexcept {
    for (std::size_t offset = begin; offset < end; ++offset) {
        if (input[offset] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

/// Validate invariants available from a fixed manifest without referenced rows.
/// Cross-object counts, checksums, and identities remain publication checks.
[[nodiscard]] constexpr std::optional<minimizer_generation_encoding_error>
validate_manifest_standalone(const minimizer_generation_manifest& manifest) noexcept {
    const minimizer_producer_identity producer_backend{
        .id = manifest.producer_backend_id,
        .revision = manifest.producer_identity_revision,
    };
    if (!is_supported_minimizer_producer_backend_identity(producer_backend)) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    const auto profile = derive_grouped_soa_minimizer_profile(manifest);
    if (!profile) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    if (minimizer_scheme_identity_for_producer_backend(producer_backend) !=
        profile->minimizer_scheme) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    if (manifest.layout == minimizer_generation_layout::grouped_soa_segment_local &&
        manifest.gpu_abi_revision != profile->gpu_abi_revision) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    if (manifest.segment_geometry_revision !=
        minimizer_generation_manifest::uniform_segment_geometry_revision) {
        return minimizer_generation_encoding_error::segment_geometry_revision;
    }
    if (!sql_integer_can_represent(manifest.position_segment_count)) {
        return minimizer_generation_encoding_error::segment_count;
    }
    if (!sql_integer_can_represent(manifest.directory_chunk_count)) {
        return minimizer_generation_encoding_error::directory_chunk_count;
    }
    if (manifest.target_length == 0) {
        if (manifest.position_segment_count != 0) {
            return minimizer_generation_encoding_error::segment_count;
        }
        return std::nullopt;
    }
    if (manifest.position_segment_span_bp == 0) {
        return minimizer_generation_encoding_error::segment_span;
    }
    const std::uint64_t expected_count =
        manifest.target_length / manifest.position_segment_span_bp +
        (manifest.target_length % manifest.position_segment_span_bp != 0 ? 1u : 0u);
    if (!sql_integer_can_represent(expected_count) ||
        expected_count != manifest.position_segment_count) {
        return minimizer_generation_encoding_error::segment_count;
    }
    return std::nullopt;
}

} // namespace bioseq::indexing::minimizer_generation_wire_detail

export namespace bioseq::indexing {

/// Encode a validated generation head to its exact 64-byte little-endian image.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, minimizer_generation_head::encoded_bytes>,
    minimizer_generation_encoding_error>
encode_minimizer_generation_head(const minimizer_generation_head& head) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (!is_supported_minimizer_generation_layout(head.layout)) {
        return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    if (head.generation_id == 0 || !sql_bigint_can_represent(head.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    std::array<std::byte, minimizer_generation_head::encoded_bytes> output{};
    std::span<std::byte> bytes{output};
    write_le<std::uint32_t>(bytes, 0, minimizer_generation_head::magic);
    write_le<std::uint16_t>(bytes, 4, static_cast<std::uint16_t>(head.layout));
    write_le<std::uint16_t>(
        bytes, 6, static_cast<std::uint16_t>(minimizer_generation_head::encoded_bytes));
    write_le<std::uint64_t>(bytes, 8, head.generation_id);
    write_le<std::uint64_t>(bytes, 16, head.source_identity);
    write_le<std::uint64_t>(bytes, 24, head.storage_contract_checksum);
    write_le<std::uint64_t>(bytes, 32, head.manifest_checksum);
    write_le<std::uint64_t>(bytes, 40, head.source_content_checksum);
    return output;
}

/// Decode one exact 64-byte generation head, rejecting malformed or reserved data.
[[nodiscard]] constexpr std::expected<minimizer_generation_head,
                                      minimizer_generation_encoding_error>
decode_minimizer_generation_head(std::span<const std::byte> input) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != minimizer_generation_head::encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    if (read_le<std::uint32_t>(input, 0) != minimizer_generation_head::magic) {
        return std::unexpected(minimizer_generation_encoding_error::magic);
    }
    const auto layout = static_cast<minimizer_generation_layout>(read_le<std::uint16_t>(input, 4));
    if (!is_supported_minimizer_generation_layout(layout)) {
        return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    if (read_le<std::uint16_t>(input, 6) != minimizer_generation_head::encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::declared_size);
    }
    if (!all_zero(input, 48, 64)) {
        return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
    }
    minimizer_generation_head head{
        .layout = layout,
        .generation_id = read_le<std::uint64_t>(input, 8),
        .source_identity = read_le<std::uint64_t>(input, 16),
        .storage_contract_checksum = read_le<std::uint64_t>(input, 24),
        .manifest_checksum = read_le<std::uint64_t>(input, 32),
        .source_content_checksum = read_le<std::uint64_t>(input, 40),
    };
    if (head.generation_id == 0 || !sql_bigint_can_represent(head.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    return head;
}

/// Encode a standalone-valid grouped-SoA manifest as exactly 160 bytes,
/// including the frozen 32-byte profile extension.
[[nodiscard]] constexpr std::expected<encoded_minimizer_manifest,
                                      minimizer_generation_encoding_error>
encode_minimizer_generation_manifest_record(
    const minimizer_generation_manifest& manifest) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (manifest.generation_id == 0 || !sql_bigint_can_represent(manifest.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    encoded_minimizer_manifest output{
        .byte_count = minimizer_manifest_encoded_bytes(manifest.layout),
    };
    std::span<std::byte> bytes{output.data(), output.size()};
    write_le<std::uint32_t>(bytes, 0, minimizer_generation_manifest::magic);
    write_le<std::uint16_t>(bytes, 4, static_cast<std::uint16_t>(manifest.layout));
    write_le<std::uint16_t>(bytes, 6, static_cast<std::uint16_t>(output.size()));
    write_le<std::uint64_t>(bytes, 8, manifest.generation_id);
    write_le<std::uint64_t>(bytes, 16, manifest.source_identity);
    write_le<std::uint64_t>(bytes, 24, manifest.source_content_checksum);
    write_le<std::uint64_t>(bytes, 32, manifest.target_length);
    write_le<std::uint64_t>(bytes, 40, manifest.control_count);
    write_le<std::uint64_t>(bytes, 48, manifest.position_count);
    write_le<std::uint64_t>(bytes, 56, manifest.payload_bytes);
    write_le<std::uint64_t>(bytes, 64, manifest.payload_checksum);
    write_le<std::uint64_t>(bytes, 72, manifest.directory_bytes);
    write_le<std::uint64_t>(bytes, 80, manifest.directory_checksum);
    write_le<std::uint32_t>(bytes, 88, manifest.position_segment_count);
    write_le<std::uint32_t>(bytes, 92, manifest.extent_count);
    write_le<std::uint32_t>(bytes, 96, manifest.directory_chunk_count);
    write_le<std::uint16_t>(bytes, 100, manifest.alphabet_code);
    write_le<std::uint16_t>(bytes, 102, manifest.storage_unit_byte_width);
    write_le<std::uint16_t>(bytes, 104, manifest.kmer_size);
    write_le<std::uint16_t>(bytes, 106, manifest.window_size);
    write_le<std::uint32_t>(bytes, 108, manifest.gpu_abi_revision);
    write_le<std::uint64_t>(bytes, 112, manifest.position_segment_span_bp);
    write_le<std::uint16_t>(bytes, 120, manifest.segment_geometry_revision);
    write_le<std::uint16_t>(bytes, 122, manifest.minimizer_scheme_id);
    write_le<std::uint16_t>(bytes, 124, manifest.minimizer_scheme_revision);
    write_le<std::uint8_t>(bytes, 126, static_cast<std::uint8_t>(manifest.producer_backend_id));
    write_le<std::uint8_t>(bytes, 127, manifest.producer_identity_revision);
    write_le<std::uint32_t>(bytes, 128, k_grouped_soa_extension_magic);
    write_le<std::uint16_t>(bytes, 132, k_grouped_soa_extension_layout_revision);
    write_le<std::uint16_t>(bytes, 134, k_grouped_soa_extension_bytes);
    write_le<std::uint8_t>(bytes, 136, static_cast<std::uint8_t>(manifest.hash_width));
    write_le<std::uint8_t>(bytes, 137, static_cast<std::uint8_t>(manifest.position_width));
    write_le<std::uint8_t>(bytes, 138, static_cast<std::uint8_t>(manifest.count_width));
    write_le<std::uint8_t>(bytes, 139, static_cast<std::uint8_t>(manifest.offset_policy));
    write_le<std::uint8_t>(bytes, 140, static_cast<std::uint8_t>(manifest.control_layout));
    write_le<std::uint8_t>(bytes, 141, static_cast<std::uint8_t>(manifest.count_semantics));
    write_le<std::uint8_t>(bytes, 142, static_cast<std::uint8_t>(manifest.position_semantics));
    write_le<std::uint64_t>(bytes, 144, manifest.max_hash);
    write_le<std::uint64_t>(bytes, 152, manifest.max_occ_cap);
    return output;
}

/// Encode the grouped-SoA manifest to its exact fixed-size byte array.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, minimizer_generation_manifest::encoded_bytes>,
    minimizer_generation_encoding_error>
encode_minimizer_generation_manifest(const minimizer_generation_manifest& manifest) noexcept {
    if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto encoded = encode_minimizer_generation_manifest_record(manifest);
    if (!encoded || encoded->size() != minimizer_generation_manifest::encoded_bytes) {
        return std::unexpected(encoded ? minimizer_generation_encoding_error::encoded_size
                                       : encoded.error());
    }
    return encoded->storage;
}

/// Decode the exact 160-byte grouped-SoA manifest.
[[nodiscard]] constexpr std::expected<minimizer_generation_manifest,
                                      minimizer_generation_encoding_error>
decode_minimizer_generation_manifest(std::span<const std::byte> input) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (input.size() != minimizer_generation_manifest::encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    if (read_le<std::uint32_t>(input, 0) != minimizer_generation_manifest::magic) {
        return std::unexpected(minimizer_generation_encoding_error::magic);
    }
    const auto layout = static_cast<minimizer_generation_layout>(read_le<std::uint16_t>(input, 4));
    if (!is_supported_minimizer_generation_layout(layout)) {
        return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    const std::size_t expected_size = minimizer_manifest_encoded_bytes(layout);
    if (input.size() != expected_size || read_le<std::uint16_t>(input, 6) != expected_size) {
        return std::unexpected(minimizer_generation_encoding_error::declared_size);
    }
    minimizer_generation_manifest manifest{
        .layout = layout,
        .generation_id = read_le<std::uint64_t>(input, 8),
        .source_identity = read_le<std::uint64_t>(input, 16),
        .source_content_checksum = read_le<std::uint64_t>(input, 24),
        .target_length = read_le<std::uint64_t>(input, 32),
        .control_count = read_le<std::uint64_t>(input, 40),
        .position_count = read_le<std::uint64_t>(input, 48),
        .payload_bytes = read_le<std::uint64_t>(input, 56),
        .payload_checksum = read_le<std::uint64_t>(input, 64),
        .directory_bytes = read_le<std::uint64_t>(input, 72),
        .directory_checksum = read_le<std::uint64_t>(input, 80),
        .position_segment_count = read_le<std::uint32_t>(input, 88),
        .extent_count = read_le<std::uint32_t>(input, 92),
        .directory_chunk_count = read_le<std::uint32_t>(input, 96),
        .alphabet_code = read_le<std::uint16_t>(input, 100),
        .storage_unit_byte_width = read_le<std::uint16_t>(input, 102),
        .kmer_size = read_le<std::uint16_t>(input, 104),
        .window_size = read_le<std::uint16_t>(input, 106),
        .gpu_abi_revision = read_le<std::uint32_t>(input, 108),
        .position_segment_span_bp = read_le<std::uint64_t>(input, 112),
        .segment_geometry_revision = read_le<std::uint16_t>(input, 120),
        .minimizer_scheme_id = read_le<std::uint16_t>(input, 122),
        .minimizer_scheme_revision = read_le<std::uint16_t>(input, 124),
        .producer_backend_id =
            static_cast<minimizer_producer_backend>(read_le<std::uint8_t>(input, 126)),
        .producer_identity_revision = read_le<std::uint8_t>(input, 127),
    };
    if (read_le<std::uint32_t>(input, 128) != k_grouped_soa_extension_magic ||
        read_le<std::uint16_t>(input, 132) != k_grouped_soa_extension_layout_revision ||
        read_le<std::uint16_t>(input, 134) != k_grouped_soa_extension_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::unsupported_storage_layout);
    }
    if (input[143] != std::byte{0}) {
        return std::unexpected(minimizer_generation_encoding_error::reserved_nonzero);
    }
    manifest.hash_width = static_cast<minimizer_scalar_width>(read_le<std::uint8_t>(input, 136));
    manifest.position_width =
        static_cast<minimizer_scalar_width>(read_le<std::uint8_t>(input, 137));
    manifest.count_width = static_cast<minimizer_scalar_width>(read_le<std::uint8_t>(input, 138));
    manifest.offset_policy =
        static_cast<grouped_soa_offset_policy>(read_le<std::uint8_t>(input, 139));
    manifest.control_layout =
        static_cast<grouped_soa_control_layout>(read_le<std::uint8_t>(input, 140));
    manifest.count_semantics =
        static_cast<grouped_soa_count_semantics>(read_le<std::uint8_t>(input, 141));
    manifest.position_semantics =
        static_cast<grouped_soa_position_semantics>(read_le<std::uint8_t>(input, 142));
    manifest.max_hash = read_le<std::uint64_t>(input, 144);
    manifest.max_occ_cap = read_le<std::uint64_t>(input, 152);
    if (manifest.generation_id == 0 || !sql_bigint_can_represent(manifest.generation_id)) {
        return std::unexpected(minimizer_generation_encoding_error::generation_id);
    }
    if (auto error = validate_manifest_standalone(manifest)) {
        return std::unexpected(*error);
    }
    return manifest;
}

/// Largest supported tagged generation-head encoding.
inline constexpr std::size_t k_max_generation_head_bytes = k_hash_major_minimizer_head_bytes;
/// Largest supported tagged generation-manifest encoding.
inline constexpr std::size_t k_max_manifest_bytes = k_hash_major_minimizer_manifest_bytes;
static_assert(minimizer_generation_head::encoded_bytes == 64u);
static_assert(minimizer_generation_manifest::encoded_bytes == 160u);
static_assert(k_max_generation_head_bytes == 224u);
static_assert(k_max_manifest_bytes == 336u);

/// Identifies the rejected branch of exact-length tagged metadata decoding.
enum class tagged_minimizer_generation_encoding_error : std::uint8_t {
    encoded_size,
    grouped_soa,
    hash_major,
};

/// Any supported persisted minimizer generation head.
using tagged_minimizer_generation_head =
    std::variant<minimizer_generation_head, hash_major_minimizer_head>;
/// Any supported persisted minimizer generation manifest.
using tagged_minimizer_generation_manifest =
    std::variant<minimizer_generation_manifest, hash_major_minimizer_manifest>;

/** Exact-length grouped-SoA/hash-major dispatch. Only the declared storage
 * layouts participate in this tagged publication branch. */
[[nodiscard]] inline std::expected<tagged_minimizer_generation_head,
                                   tagged_minimizer_generation_encoding_error>
decode_tagged_minimizer_generation_head(std::span<const std::byte> input) noexcept {
    if (input.size() == minimizer_generation_head::encoded_bytes) {
        auto decoded = decode_minimizer_generation_head(input);
        if (!decoded || decoded->layout != minimizer_generation_layout::grouped_soa_segment_local) {
            return std::unexpected(tagged_minimizer_generation_encoding_error::grouped_soa);
        }
        return tagged_minimizer_generation_head{std::in_place_index<0>, *decoded};
    }
    if (input.size() == k_hash_major_minimizer_head_bytes) {
        auto decoded = decode_hash_major_minimizer_head(input);
        if (!decoded) {
            return std::unexpected(tagged_minimizer_generation_encoding_error::hash_major);
        }
        return tagged_minimizer_generation_head{std::in_place_index<1>, *decoded};
    }
    return std::unexpected(tagged_minimizer_generation_encoding_error::encoded_size);
}

/// Decode a generation manifest by exact supported byte length.
[[nodiscard]] inline std::expected<tagged_minimizer_generation_manifest,
                                   tagged_minimizer_generation_encoding_error>
decode_tagged_minimizer_generation_manifest(std::span<const std::byte> input) noexcept {
    if (input.size() == minimizer_generation_manifest::encoded_bytes) {
        auto decoded = decode_minimizer_generation_manifest(input);
        if (!decoded || decoded->layout != minimizer_generation_layout::grouped_soa_segment_local) {
            return std::unexpected(tagged_minimizer_generation_encoding_error::grouped_soa);
        }
        return tagged_minimizer_generation_manifest{std::in_place_index<0>, *decoded};
    }
    if (input.size() == k_hash_major_minimizer_manifest_bytes) {
        auto decoded = decode_hash_major_minimizer_manifest(input);
        if (!decoded) {
            return std::unexpected(tagged_minimizer_generation_encoding_error::hash_major);
        }
        return tagged_minimizer_generation_manifest{std::in_place_index<1>, *decoded};
    }
    return std::unexpected(tagged_minimizer_generation_encoding_error::encoded_size);
}

/// Translate the fixed runtime producer identity into the registered
/// hash-major storage profile. Keeping this bridge beside the
/// grouped-SoA producer/scheme registry prevents PostgreSQL publication and
/// reopen code from independently reconstructing (or silently defaulting)
/// persisted biological semantics.
[[nodiscard]] constexpr std::expected<hash_major_minimizer_storage_profile,
                                      hash_major_minimizer_error>
hash_major_minimizer_profile_for_backend(minimizer_sketch_backend backend,
                                         std::uint16_t alphabet_code,
                                         std::uint16_t kmer_size,
                                         std::uint16_t window_size,
                                         std::uint64_t target_length) noexcept {
    const minimizer_scheme_identity scheme = minimizer_scheme_identity_for_backend(backend);
    const minimizer_producer_identity producer =
        minimizer_producer_backend_identity_for_backend(backend);
    if (!is_supported_minimizer_scheme_identity(scheme.id, scheme.revision) ||
        !is_supported_minimizer_producer_backend_identity(producer) || alphabet_code != 0u ||
        kmer_size != 15u || window_size != 10u) {
        return std::unexpected(hash_major_minimizer_error::storage_contract_mismatch);
    }
    const hash_major_minimizer_storage_profile profile{
        .hash_scheme_identity = scheme.id,
        .hash_scheme_revision = scheme.revision,
        .alphabet_code = alphabet_code,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .hash_width_bytes = 4u,
        .stored_posting_width_bytes =
            hash_major_minimizer_uses_u32_postings(target_length) ? 4u : 8u,
        .position_encoding_revision = 1u,
        .hash_semantics_revision = 1u,
        .count_semantics_revision = 1u,
        .producer_backend_identity = static_cast<std::uint16_t>(producer.id),
        .producer_backend_revision = producer.revision,
        .controls_per_block = k_hash_major_minimizer_controls_per_block,
        .hash_seed = 0u,
    };
    if (!is_valid_hash_major_minimizer_storage_profile(profile, target_length)) {
        return std::unexpected(hash_major_minimizer_error::storage_contract_mismatch);
    }
    return profile;
}

/// Validate invariants intrinsic to one decoded directory entry.
[[nodiscard]] constexpr std::optional<minimizer_generation_encoding_error>
validate_minimizer_bucket_dir_entry(const grouped_soa_bucket_directory_entry& entry,
                                    const grouped_soa_minimizer_profile& profile) noexcept {
    if (!validate_grouped_soa_minimizer_profile(profile)) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    if (entry.position_segment < 0) {
        return minimizer_generation_encoding_error::position_segment;
    }
    if (entry.lo_hash > entry.hi_hash || entry.hi_hash > profile.max_hash) {
        return minimizer_generation_encoding_error::hash_range;
    }
    if (!derive_minimizer_position_extent_sub_key(entry.sub_key)) {
        return minimizer_generation_encoding_error::sub_key;
    }
    if (profile.layout == minimizer_generation_layout::grouped_soa_segment_local &&
        entry.sub_key != static_cast<index_sub_key_t>(entry.partition_id)) {
        return minimizer_generation_encoding_error::sub_key;
    }
    if (entry.control_element_count == 0 || entry.position_element_count == 0 ||
        entry.control_element_count > (std::numeric_limits<std::uint32_t>::max)()) {
        return minimizer_generation_encoding_error::element_count;
    }
    const std::uint64_t position_width = static_cast<std::uint8_t>(profile.position_width);
    if (entry.shape_offset_width != select_grouped_soa_offset_width(entry.position_element_count)) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    const auto control_layout = plan_grouped_soa_control_plane(entry.control_element_count,
                                                               profile.hash_width,
                                                               entry.shape_offset_width,
                                                               profile.count_width);
    if (!control_layout || entry.control_encoded_bytes != control_layout->encoded_bytes ||
        entry.position_element_count >
            (std::numeric_limits<std::uint64_t>::max)() / position_width ||
        entry.position_encoded_bytes != entry.position_element_count * position_width) {
        return minimizer_generation_encoding_error::byte_count;
    }
    if (entry.control_first_chunk != 0 || entry.position_first_chunk != 0 ||
        entry.control_chunk_count == 0 || entry.position_chunk_count == 0) {
        return minimizer_generation_encoding_error::chunk_range;
    }
    const std::uint64_t control_end =
        static_cast<std::uint64_t>(entry.control_first_chunk) + entry.control_chunk_count;
    const std::uint64_t position_end =
        static_cast<std::uint64_t>(entry.position_first_chunk) + entry.position_chunk_count;
    if (control_end > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u ||
        position_end >
            static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)()) + 1u) {
        return minimizer_generation_encoding_error::chunk_range;
    }
    return std::nullopt;
}

/// Encode one logical directory entry to the exact 128-byte little-endian ABI.
[[nodiscard]] constexpr std::expected<encoded_grouped_soa_bucket_directory_entry,
                                      minimizer_generation_encoding_error>
encode_minimizer_bucket_dir_entry_record(const grouped_soa_bucket_directory_entry& entry,
                                         const grouped_soa_minimizer_profile& profile) noexcept {
    using namespace minimizer_generation_wire_detail;
    if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
        return std::unexpected(*error);
    }
    encoded_grouped_soa_bucket_directory_entry output{
        .byte_count = minimizer_directory_entry_encoded_bytes(profile),
    };
    std::span<std::byte> bytes{output.data(), output.size()};
    write_le<std::uint32_t>(bytes, 0, entry.partition_id);
    write_le<std::uint32_t>(bytes, 4, std::bit_cast<std::uint32_t>(entry.position_segment));
    write_le<std::uint64_t>(bytes, 8, entry.lo_hash);
    write_le<std::uint64_t>(bytes, 16, entry.hi_hash);
    write_le<std::uint64_t>(bytes, 24, std::bit_cast<std::uint64_t>(entry.sub_key));
    write_le<std::uint32_t>(bytes, 32, entry.control_first_chunk);
    write_le<std::uint32_t>(bytes, 36, entry.control_chunk_count);
    write_le<std::uint32_t>(bytes, 40, entry.position_first_chunk);
    write_le<std::uint32_t>(bytes, 44, entry.position_chunk_count);
    write_le<std::uint64_t>(bytes, 48, entry.control_element_count);
    write_le<std::uint64_t>(bytes, 56, entry.position_element_count);
    write_le<std::uint64_t>(bytes, 64, entry.control_encoded_bytes);
    write_le<std::uint64_t>(bytes, 72, entry.position_encoded_bytes);
    write_le<std::uint64_t>(bytes, 80, entry.control_checksum);
    write_le<std::uint64_t>(bytes, 88, entry.position_checksum);
    write_le<std::uint8_t>(bytes, 96, static_cast<std::uint8_t>(profile.hash_width));
    write_le<std::uint8_t>(bytes, 97, static_cast<std::uint8_t>(entry.shape_offset_width));
    write_le<std::uint8_t>(bytes, 98, static_cast<std::uint8_t>(profile.count_width));
    write_le<std::uint8_t>(bytes, 99, static_cast<std::uint8_t>(profile.position_width));
    write_le<std::uint8_t>(bytes, 100, static_cast<std::uint8_t>(profile.control_layout));
    write_le<std::uint8_t>(bytes, 101, static_cast<std::uint8_t>(profile.count_semantics));
    write_le<std::uint8_t>(bytes, 102, static_cast<std::uint8_t>(profile.position_semantics));
    write_le<std::uint8_t>(bytes, 103, 1u); // terminal-prefix present
    return output;
}

/// Encode one grouped-SoA directory entry to its exact fixed-size byte array.
[[nodiscard]] constexpr std::expected<
    std::array<std::byte, grouped_soa_bucket_directory_entry::encoded_bytes>,
    minimizer_generation_encoding_error>
encode_grouped_soa_bucket_directory_entry(const grouped_soa_bucket_directory_entry& entry,
                                          const grouped_soa_minimizer_profile& profile) noexcept {
    if (profile.layout != minimizer_generation_layout::grouped_soa_segment_local) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto encoded = encode_minimizer_bucket_dir_entry_record(entry, profile);
    if (!encoded || encoded->size() != grouped_soa_bucket_directory_entry::encoded_bytes) {
        return std::unexpected(encoded ? minimizer_generation_encoding_error::encoded_size
                                       : encoded.error());
    }
    return encoded->storage;
}

/// Decode and intrinsically validate one exact 128-byte directory entry.
[[nodiscard]] constexpr std::expected<grouped_soa_bucket_directory_entry,
                                      minimizer_generation_encoding_error>
decode_minimizer_bucket_dir_entry(std::span<const std::byte> input,
                                  const grouped_soa_minimizer_profile& profile) noexcept {
    using namespace minimizer_generation_wire_detail;
    const std::size_t expected_size = minimizer_directory_entry_encoded_bytes(profile);
    if (input.size() != expected_size) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    grouped_soa_bucket_directory_entry entry{
        .partition_id = read_le<std::uint32_t>(input, 0),
        .position_segment = std::bit_cast<std::int32_t>(read_le<std::uint32_t>(input, 4)),
        .lo_hash = read_le<std::uint64_t>(input, 8),
        .hi_hash = read_le<std::uint64_t>(input, 16),
        .sub_key = std::bit_cast<index_sub_key_t>(read_le<std::uint64_t>(input, 24)),
        .control_first_chunk = read_le<std::uint32_t>(input, 32),
        .control_chunk_count = read_le<std::uint32_t>(input, 36),
        .position_first_chunk = read_le<std::uint32_t>(input, 40),
        .position_chunk_count = read_le<std::uint32_t>(input, 44),
        .control_element_count = read_le<std::uint64_t>(input, 48),
        .position_element_count = read_le<std::uint64_t>(input, 56),
        .control_encoded_bytes = read_le<std::uint64_t>(input, 64),
        .position_encoded_bytes = read_le<std::uint64_t>(input, 72),
        .control_checksum = read_le<std::uint64_t>(input, 80),
        .position_checksum = read_le<std::uint64_t>(input, 88),
    };
    if (read_le<std::uint8_t>(input, 96) != static_cast<std::uint8_t>(profile.hash_width) ||
        read_le<std::uint8_t>(input, 98) != static_cast<std::uint8_t>(profile.count_width) ||
        read_le<std::uint8_t>(input, 99) != static_cast<std::uint8_t>(profile.position_width) ||
        read_le<std::uint8_t>(input, 100) != static_cast<std::uint8_t>(profile.control_layout) ||
        read_le<std::uint8_t>(input, 101) != static_cast<std::uint8_t>(profile.count_semantics) ||
        read_le<std::uint8_t>(input, 102) !=
            static_cast<std::uint8_t>(profile.position_semantics) ||
        read_le<std::uint8_t>(input, 103) != 1u || !all_zero(input, 104, 128)) {
        return std::unexpected(
            !all_zero(input, 104, 128)
                ? minimizer_generation_encoding_error::reserved_nonzero
                : minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    entry.shape_offset_width =
        static_cast<minimizer_scalar_width>(read_le<std::uint8_t>(input, 97));
    if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
        return std::unexpected(*error);
    }
    return entry;
}

/// Validate stable ordinal assignment and sorted, non-overlapping hash ranges.
[[nodiscard]] inline std::optional<minimizer_generation_encoding_error>
validate_minimizer_bucket_directory_entries(
    std::span<const grouped_soa_bucket_directory_entry> entries,
    const grouped_soa_minimizer_profile& profile) noexcept {
    if (!validate_grouped_soa_minimizer_profile(profile)) {
        return minimizer_generation_encoding_error::storage_contract_mismatch;
    }
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const grouped_soa_bucket_directory_entry& entry = entries[index];
        if (entry.partition_id != index) {
            return minimizer_generation_encoding_error::partition_id;
        }
        if (auto error = validate_minimizer_bucket_dir_entry(entry, profile)) {
            return error;
        }
        if (index == 0) {
            continue;
        }
        const grouped_soa_bucket_directory_entry& previous = entries[index - 1u];
        if (entry.position_segment < previous.position_segment ||
            (entry.position_segment == previous.position_segment &&
             entry.lo_hash <= previous.hi_hash)) {
            return minimizer_generation_encoding_error::directory_order;
        }
    }
    return std::nullopt;
}

/// Encode the authoritative header-free sequence of fixed directory entries.
[[nodiscard]] inline std::expected<std::vector<std::byte>, minimizer_generation_encoding_error>
encode_minimizer_bucket_directory(std::span<const grouped_soa_bucket_directory_entry> entries,
                                  const grouped_soa_minimizer_profile& profile) {
    if (auto error = validate_minimizer_bucket_directory_entries(entries, profile)) {
        return std::unexpected(*error);
    }
    const std::size_t entry_bytes = minimizer_directory_entry_encoded_bytes(profile);
    if (entries.size() > (std::numeric_limits<std::size_t>::max)() / entry_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    std::vector<std::byte> output(entries.size() * entry_bytes);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto encoded = encode_minimizer_bucket_dir_entry_record(entries[index], profile);
        if (!encoded) {
            return std::unexpected(encoded.error());
        }
        std::copy(encoded->begin(),
                  encoded->end(),
                  output.begin() + static_cast<std::ptrdiff_t>(index * entry_bytes));
    }
    return output;
}

/// Owning decoded view of the authoritative fixed directory bytes.
class grouped_soa_directory_view {
  public:
    grouped_soa_directory_view() = default;

    /// Copy and validate a complete header-free encoded directory.
    [[nodiscard]] static std::expected<grouped_soa_directory_view,
                                       minimizer_generation_encoding_error>
    decode(std::span<const std::byte> bytes, const grouped_soa_minimizer_profile& profile) {
        if (!validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const std::size_t entry_bytes = minimizer_directory_entry_encoded_bytes(profile);
        if (bytes.size() % entry_bytes != 0) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        grouped_soa_directory_view view;
        view.profile_ = profile;
        view.encoded_.assign(bytes.begin(), bytes.end());
        const std::size_t count = bytes.size() / entry_bytes;
        view.entries_.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            const std::span<const std::byte> encoded_entry{
                view.encoded_.data() + index * entry_bytes, entry_bytes};
            auto entry = decode_minimizer_bucket_dir_entry(encoded_entry, profile);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            view.entries_.push_back(*entry);
        }
        if (auto error = validate_minimizer_bucket_directory_entries(view.entries_, profile)) {
            return std::unexpected(*error);
        }
        return view;
    }

    /// Decode under a caller-held logical reservation. Both vectors reserve
    /// first, then their actual capacities are reconciled before any persisted
    /// byte or decoded entry is written. The caller's permit may include other
    /// simultaneously-live allocations. Only the directory capacity delta is
    /// added here.
    [[nodiscard]] static std::expected<grouped_soa_directory_view,
                                       minimizer_generation_encoding_error>
    decode_admitted(std::span<const std::byte> bytes,
                    const grouped_soa_minimizer_profile& profile,
                    bioseq::parallelism::memory_permit& permit,
                    bioseq::parallelism::memory_admission_failure* admission_failure = nullptr) {
        if (!validate_grouped_soa_minimizer_profile(profile)) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        const std::size_t entry_bytes = minimizer_directory_entry_encoded_bytes(profile);
        if (bytes.size() % entry_bytes != 0) {
            return std::unexpected(minimizer_generation_encoding_error::directory_size);
        }
        const std::size_t count = bytes.size() / entry_bytes;
        const std::uint64_t logical_bytes = bioseq::common::saturating_add(
            bytes.size(),
            bioseq::common::saturating_mul(count, sizeof(grouped_soa_bucket_directory_entry)));
        if (logical_bytes == (std::numeric_limits<std::uint64_t>::max)() ||
            permit.bytes() < logical_bytes) {
            return std::unexpected(logical_bytes == (std::numeric_limits<std::uint64_t>::max)()
                                       ? minimizer_generation_encoding_error::arithmetic_overflow
                                       : minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t non_directory_bytes = permit.bytes() - logical_bytes;
        const std::uint64_t allocation_headroom = bioseq::common::saturating_add(
            non_directory_bytes,
            bioseq::common::saturating_mul(
                logical_bytes, bioseq::parallelism::k_vector_preallocation_headroom_factor));
        if (allocation_headroom == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        if (!permit.grow_to(allocation_headroom, admission_failure)) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        grouped_soa_directory_view view;
        view.profile_ = profile;
        try {
            view.encoded_.reserve(bytes.size());
            view.entries_.reserve(count);
        } catch (const std::bad_alloc&) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        const std::uint64_t actual_bytes = view.owned_capacity_bytes();
        if (actual_bytes == (std::numeric_limits<std::uint64_t>::max)()) {
            return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
        }
        const std::uint64_t reconciled =
            bioseq::common::saturating_add(non_directory_bytes, actual_bytes);
        if (reconciled == (std::numeric_limits<std::uint64_t>::max)() ||
            reconciled > allocation_headroom) {
            return std::unexpected(minimizer_generation_encoding_error::admission);
        }
        permit.shrink_to(reconciled);
        view.encoded_.assign(bytes.begin(), bytes.end());
        for (std::size_t index = 0; index < count; ++index) {
            const auto entry = decode_minimizer_bucket_dir_entry(
                std::span<const std::byte>{view.encoded_}.subspan(index * entry_bytes, entry_bytes),
                profile);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            view.entries_.push_back(*entry);
        }
        if (auto error = validate_minimizer_bucket_directory_entries(view.entries_, profile)) {
            return std::unexpected(*error);
        }
        return view;
    }

    /// Return the immutable encoded bytes suitable for direct GPU upload.
    [[nodiscard]] std::span<const std::byte> encoded_bytes() const noexcept {
        return encoded_;
    }

    /// Return decoded entries in their stable directory-ordinal order.
    [[nodiscard]] std::span<const grouped_soa_bucket_directory_entry> entries() const noexcept {
        return entries_;
    }

    /// Return the canonical profile used to qualify every decoded entry.
    [[nodiscard]] const grouped_soa_minimizer_profile& profile() const noexcept {
        return profile_;
    }

    /// Heap bytes owned by this view. Readers use capacities, rather than
    /// logical sizes, when retaining a memory-admission permit.
    [[nodiscard]] std::uint64_t owned_capacity_bytes() const noexcept {
        return bioseq::common::saturating_add(
            static_cast<std::uint64_t>(encoded_.capacity()),
            bioseq::common::saturating_mul(static_cast<std::uint64_t>(entries_.capacity()),
                                           sizeof(grouped_soa_bucket_directory_entry)));
    }

    /// Resolve one stable directory ordinal.
    [[nodiscard]] const grouped_soa_bucket_directory_entry*
    entry_by_ordinal(std::uint32_t ordinal) const noexcept {
        return ordinal < entries_.size() ? &entries_[ordinal] : nullptr;
    }

    /// Find the unique hash partition in one fixed persisted position core.
    [[nodiscard]] const grouped_soa_bucket_directory_entry*
    find(std::int32_t position_segment, std::uint64_t hash) const noexcept {
        std::size_t lo = 0;
        std::size_t hi = entries_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2u;
            const grouped_soa_bucket_directory_entry& entry = entries_[mid];
            if (entry.position_segment < position_segment ||
                (entry.position_segment == position_segment && entry.hi_hash < hash)) {
                lo = mid + 1u;
            } else {
                hi = mid;
            }
        }
        if (lo >= entries_.size()) {
            return nullptr;
        }
        const grouped_soa_bucket_directory_entry& entry = entries_[lo];
        return entry.position_segment == position_segment && entry.lo_hash <= hash &&
                       hash <= entry.hi_hash
                   ? &entry
                   : nullptr;
    }

  private:
    grouped_soa_minimizer_profile profile_{};
    std::vector<std::byte> encoded_;
    std::vector<grouped_soa_bucket_directory_entry> entries_;
};

} // namespace bioseq::indexing
