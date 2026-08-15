/**
 * @file minimizer_hashmap/grouped_soa.cppm
 * @brief Persisted grouped-SoA codecs and transient minimizer lookup storage.
 */
export module bioseq.indexing.minimizer_hashmap:grouped_soa;

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

namespace bioseq::indexing::detail {
struct minimizer_hashmap_fragment_index_table;
}

export namespace bioseq::indexing {

// Persisted minimizer codecs stay in this storage owner. Their shared CRC
// implementation is imported from the lower chunk-storage module so row,
// manifest, fake, and PostgreSQL paths cannot drift.

/// Canonical minimap-style hash/tie/strand semantics used by persisted indexes.
inline constexpr std::uint32_t k_minimizer_hash_semantics_revision = 1u;

/// Segment-local `(local_bp << 1) | strand` persisted encoding semantics.
inline constexpr std::uint32_t k_minimizer_segment_local_position_encoding_revision = 2u;
/// Revision of the grouped-SoA payload byte layout.
inline constexpr std::uint16_t k_grouped_soa_payload_layout_abi_revision = 1u;
/// Revision consumed by grouped-SoA GPU readers.
inline constexpr std::uint32_t k_grouped_soa_gpu_abi_revision = 3u;
/// Magic word stored at the start of the grouped-SoA manifest extension.
inline constexpr std::uint32_t k_grouped_soa_extension_magic = 0x42494f53u;
/// Revision encoded in the grouped-SoA manifest extension.
inline constexpr std::uint16_t k_grouped_soa_extension_layout_revision = 1u;
/// Exact byte length of the grouped-SoA manifest extension.
inline constexpr std::uint16_t k_grouped_soa_extension_bytes = 32u;
/// Persisted supported_alphabets pack index for DNA4. This is an on-wire code,
/// not the alphabet cardinality.
inline constexpr std::uint16_t k_minimizer_persisted_dna4_alphabet_code = 0u;
/// Largest hash produced by the persisted minimap k15 profile.
inline constexpr std::uint32_t k_minimap_k15_max_hash = 0x3fffffffu;

/// Persisted minimap scheme identifier.
inline constexpr std::uint16_t k_minimap_scheme_id = 0u;
/// Persisted minimap scheme revision.
inline constexpr std::uint16_t k_minimap_scheme_revision = 0u;
/// Persisted identifier for the bounded u32 ntHash r7 sum scheme.
inline constexpr std::uint16_t k_bounded_nthash_u32_r7_sum_scheme_id = 1u;
/// Persisted revision for the bounded u32 ntHash r7 sum scheme.
inline constexpr std::uint16_t k_bounded_nthash_u32_r7_sum_scheme_revision = 1u;
/// Sentinel used when no persisted minimizer scheme is selected.
inline constexpr std::uint16_t k_invalid_minimizer_scheme_id =
    (std::numeric_limits<std::uint16_t>::max)();
/// Sentinel used when no persisted minimizer scheme revision is selected.
inline constexpr std::uint16_t k_invalid_minimizer_scheme_revision =
    (std::numeric_limits<std::uint16_t>::max)();

/// Frozen biological scheme identity shared by producers, writers and readers.
struct minimizer_scheme_identity {
    std::uint16_t id = k_invalid_minimizer_scheme_id;
    std::uint16_t revision = k_invalid_minimizer_scheme_revision;

    [[nodiscard]] bool operator==(const minimizer_scheme_identity&) const = default;
};

/// Storage-issued incarnation of one minimizer-sequence canonical replay set.  The token
/// changes on every begin operation. The revision changes whenever a caller can
/// alter the qualified logical replay (including rerouting or physical-run
/// injection). Internal compaction that preserves the exact canonical replay
/// is representation-only and leaves this binding stable. A zero token is
/// never authoritative.
struct direct_minimizer_spill_storage_binding {
    std::uint64_t generation_token = 0;
    std::uint64_t mutation_revision = 0;

    [[nodiscard]] constexpr bool valid() const noexcept {
        return generation_token != 0;
    }

    [[nodiscard]] constexpr bool
    operator==(const direct_minimizer_spill_storage_binding&) const noexcept = default;
};

/// Persisted identity of the minimap scheme.
inline constexpr minimizer_scheme_identity k_minimap_scheme_identity{
    .id = k_minimap_scheme_id,
    .revision = k_minimap_scheme_revision,
};
/// Persisted identity of the bounded u32 ntHash r7 sum scheme.
inline constexpr minimizer_scheme_identity k_bounded_nthash_u32_r7_sum_scheme_identity{
    .id = k_bounded_nthash_u32_r7_sum_scheme_id,
    .revision = k_bounded_nthash_u32_r7_sum_scheme_revision,
};

/// Frozen physical producer identifiers persisted in manifest byte 126.
/// These values identify the implementation that actually produced the
/// canonical event stream. They are intentionally independent of biological
/// scheme identity because `scalar`, `exact_simd`, and `vulkan_segment` share
/// the minimap scheme. The full-resident `vector_stream` exact-SIMD
/// route has distinct selection semantics and must never be persisted or
/// consumed under this identity. It remains confined to paired transient
/// no-index work. Persisted consumers require continuation/index route stats.
enum class minimizer_producer_backend : std::uint8_t {
    invalid = 0u,
    scalar = 1u,
    exact_simd = 2u,
    bounded_nthash_simd = 3u,
    vulkan_segment = 4u,
};

/// Revision of the producer identity namespace persisted in manifest byte 127.
inline constexpr std::uint8_t k_minimizer_producer_identity_revision = 1u;

/// Frozen producer identifier and its namespace revision.
struct minimizer_producer_identity {
    minimizer_producer_backend id = minimizer_producer_backend::invalid;
    std::uint8_t revision = 0u;

    [[nodiscard]] bool operator==(const minimizer_producer_identity&) const = default;
};

/// Return whether a producer identity is valid for persisted generation data.
[[nodiscard]] constexpr bool
is_supported_minimizer_producer_backend_identity(minimizer_producer_identity identity) noexcept {
    if (identity.revision != k_minimizer_producer_identity_revision) {
        return false;
    }
    switch (identity.id) {
    case minimizer_producer_backend::scalar:
    case minimizer_producer_backend::exact_simd:
    case minimizer_producer_backend::bounded_nthash_simd:
    case minimizer_producer_backend::vulkan_segment:
        return true;
    case minimizer_producer_backend::invalid:
        return false;
    }
    return false;
}

/// Bind a runtime route to its frozen physical producer identity. Adding a
/// route without extending this switch is fail-closed.
[[nodiscard]] constexpr minimizer_producer_identity
minimizer_producer_backend_identity_for_backend(minimizer_sketch_backend backend) noexcept {
    const auto identity = [backend] {
        switch (backend) {
        case minimizer_sketch_backend::scalar:
            return minimizer_producer_backend::scalar;
        case minimizer_sketch_backend::exact_simd:
            return minimizer_producer_backend::exact_simd;
        case minimizer_sketch_backend::bounded_nthash_simd:
            return minimizer_producer_backend::bounded_nthash_simd;
        case minimizer_sketch_backend::vulkan_segment:
            return minimizer_producer_backend::vulkan_segment;
        }
        return minimizer_producer_backend::invalid;
    }();
    return identity == minimizer_producer_backend::invalid
               ? minimizer_producer_identity{}
               : minimizer_producer_identity{
                     .id = identity,
                     .revision = k_minimizer_producer_identity_revision,
                 };
}

/// Return whether an id/revision pair names a supported persisted scheme.
[[nodiscard]] constexpr bool
is_supported_minimizer_scheme_identity(std::uint16_t scheme_id,
                                       std::uint16_t scheme_revision) noexcept {
    return (scheme_id == k_minimap_scheme_id && scheme_revision == k_minimap_scheme_revision) ||
           (scheme_id == k_bounded_nthash_u32_r7_sum_scheme_id &&
            scheme_revision == k_bounded_nthash_u32_r7_sum_scheme_revision);
}

/// Bind every persistable sketch producer to its persisted semantics. `exact_simd`
/// names its continuation/index producer, not the optional paired-resident
/// `vector_stream` sub-route. Adding a backend without extending
/// this switch is a fail-closed operation.
[[nodiscard]] constexpr minimizer_scheme_identity
minimizer_scheme_identity_for_backend(minimizer_sketch_backend backend) noexcept {
    switch (backend) {
    case minimizer_sketch_backend::scalar:
    case minimizer_sketch_backend::vulkan_segment:
    case minimizer_sketch_backend::exact_simd:
        return k_minimap_scheme_identity;
    case minimizer_sketch_backend::bounded_nthash_simd:
        return k_bounded_nthash_u32_r7_sum_scheme_identity;
    }
    return {};
}

/// Biological scheme implied by one frozen physical producer identity. This
/// retains the scheme mapping while preventing shared-scheme
/// backends from relabelling one another.
[[nodiscard]] constexpr minimizer_scheme_identity
minimizer_scheme_identity_for_producer_backend(minimizer_producer_identity identity) noexcept {
    if (!is_supported_minimizer_producer_backend_identity(identity)) {
        return {};
    }
    switch (identity.id) {
    case minimizer_producer_backend::scalar:
    case minimizer_producer_backend::exact_simd:
    case minimizer_producer_backend::vulkan_segment:
        return k_minimap_scheme_identity;
    case minimizer_producer_backend::bounded_nthash_simd:
        return k_bounded_nthash_u32_r7_sum_scheme_identity;
    case minimizer_producer_backend::invalid:
        break;
    }
    return {};
}

/// Uniform fixed-core segment geometry plus disjoint control/position extent
/// namespaces encoded in persisted manifests.
inline constexpr std::uint16_t k_minimizer_segment_geometry_revision = 2u;

/// Preferred immutable core span. Overflow retries halve this power of two.
inline constexpr std::uint64_t k_minimizer_preferred_segment_span_bp = 8u * 1024u * 1024u;

/// Deterministic hash-partition payload target used by the grouped-SoA encoder.
inline constexpr std::uint64_t k_minimizer_partition_target_bytes = 256u * 1024u;

/// PostgreSQL S1 page-dense quantum selected by grouped-SoA storage.
inline constexpr std::uint32_t k_grouped_soa_extent_payload_bytes = 8064u;

/// Production grouped-SoA count saturation threshold.  The value preserves a
/// 16-bit count lane while reserving 65,535 as the persisted "cap exceeded"
/// sentinel.
inline constexpr std::uint64_t k_grouped_soa_default_max_occurrence_cap = 65'534u;

/// Return whether a non-negative unsigned key fits a PostgreSQL BIGINT key.
[[nodiscard]] constexpr bool sql_bigint_can_represent(std::uint64_t value) noexcept {
    // Key axes are non-negative numeric values. CRC words persisted through a
    // signed SQL column use an explicit bit-preserving conversion instead.
    return value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
}

/// Return whether a non-negative unsigned key fits a PostgreSQL INTEGER key.
[[nodiscard]] constexpr bool sql_integer_can_represent(std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
}

/// Error classes reported by fixed-width storage codecs and geometry checks.
enum class minimizer_generation_encoding_error : std::uint8_t {
    encoded_size,
    magic,
    unsupported_storage_layout,
    declared_size,
    reserved_nonzero,
    generation_id,
    segment_geometry_revision,
    segment_span,
    segment_count,
    directory_chunk_count,
    arithmetic_overflow,
    segment_ordinal,
    bp_range,
    directory_size,
    directory_order,
    partition_id,
    position_segment,
    hash_range,
    sub_key,
    chunk_range,
    element_count,
    byte_count,
    checksum,
    identity_mismatch,
    storage_contract_mismatch,
    control_order,
    control_range,
    position_range,
    admission,
    duplicate_event,
    replay_mismatch,
    row_order,
    row_count,
};

/// Frozen persisted wire/profile selector encoded in every fixed metadata row.
enum class minimizer_generation_layout : std::uint16_t {
    invalid = 0u,
    grouped_soa_segment_local = 5u,
};

/// Return the immutable physical row quantum selected by the storage layout.
/// The value selects the fixed 8,064-byte physical row quantum.
[[nodiscard]] constexpr std::uint32_t
minimizer_extent_payload_bytes(minimizer_generation_layout layout) noexcept {
    return layout == minimizer_generation_layout::grouped_soa_segment_local
               ? k_grouped_soa_extent_payload_bytes
               : 0u;
}

static_assert(minimizer_extent_payload_bytes(
                  minimizer_generation_layout::grouped_soa_segment_local) == 8064u);

/// Return whether a persisted layout selector has a canonical local profile.
[[nodiscard]] constexpr bool
is_supported_minimizer_generation_layout(minimizer_generation_layout layout) noexcept {
    return layout == minimizer_generation_layout::grouped_soa_segment_local;
}

/// Exact scalar widths admitted by grouped-SoA storage.
enum class minimizer_scalar_width : std::uint8_t {
    u16 = 2u,
    u32 = 4u,
    u64 = 8u,
};

/// Selects how each grouped-SoA extent chooses its prefix width.
enum class grouped_soa_offset_policy : std::uint8_t {
    per_extent_narrowest = 1u,
};

/// Selects the byte arrangement of grouped-SoA control arrays.
enum class grouped_soa_control_layout : std::uint8_t {
    soa_hash_prefix_offset_count = 1u,
};

/// Selects how counts above the configured cap are represented.
enum class grouped_soa_count_semantics : std::uint8_t {
    saturating_cap_plus_one = 1u,
};

/// Selects the persisted position coordinate system.
enum class grouped_soa_position_semantics : std::uint8_t {
    all_segment_local_bp_strand = 2u,
};

/**
 * Complete grouped-SoA payload contract derived from persisted identity.
 * Required scalar widths and semantics live directly in this profile. There
 * is no optional shape wrapper or alternate layout profile.
 */
struct grouped_soa_minimizer_profile {
    minimizer_generation_layout layout = minimizer_generation_layout::grouped_soa_segment_local;
    minimizer_scheme_identity minimizer_scheme{};
    std::uint16_t alphabet_code = 0u;
    std::uint16_t kmer_size = 0u;
    minimizer_scalar_width hash_width = minimizer_scalar_width::u32;
    minimizer_scalar_width position_width = minimizer_scalar_width::u32;
    minimizer_scalar_width count_width = minimizer_scalar_width::u32;
    grouped_soa_offset_policy offset_policy = grouped_soa_offset_policy::per_extent_narrowest;
    grouped_soa_control_layout control_layout =
        grouped_soa_control_layout::soa_hash_prefix_offset_count;
    grouped_soa_count_semantics count_semantics =
        grouped_soa_count_semantics::saturating_cap_plus_one;
    grouped_soa_position_semantics position_semantics =
        grouped_soa_position_semantics::all_segment_local_bp_strand;
    std::uint16_t payload_layout_abi_revision = k_grouped_soa_payload_layout_abi_revision;
    std::uint32_t gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    std::uint64_t max_hash = 0u;
    std::uint64_t max_occ_cap = 0u;
    bool segment_local_positions = true;

    [[nodiscard]] bool operator==(const grouped_soa_minimizer_profile&) const = default;
};

struct minimizer_generation_manifest;
struct grouped_soa_bucket_directory_entry;

/// Return whether a scalar-width tag is supported by grouped-SoA codecs.
[[nodiscard]] constexpr bool is_minimizer_scalar_width(minimizer_scalar_width width) noexcept {
    return width == minimizer_scalar_width::u16 || width == minimizer_scalar_width::u32 ||
           width == minimizer_scalar_width::u64;
}

/// Return the largest integer representable by a grouped-SoA scalar width.
[[nodiscard]] constexpr std::uint64_t
minimizer_scalar_width_max(minimizer_scalar_width width) noexcept {
    switch (width) {
    case minimizer_scalar_width::u16:
        return (std::numeric_limits<std::uint16_t>::max)();
    case minimizer_scalar_width::u32:
        return (std::numeric_limits<std::uint32_t>::max)();
    case minimizer_scalar_width::u64:
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return 0u;
}

/// Select the canonical persisted hash width for one scheme and biological shape.
[[nodiscard]] constexpr std::expected<minimizer_scalar_width, minimizer_generation_encoding_error>
select_grouped_soa_hash_width(minimizer_scheme_identity scheme,
                              std::uint16_t alphabet_code,
                              std::uint16_t kmer_size) noexcept {
    if (alphabet_code != k_minimizer_persisted_dna4_alphabet_code || kmer_size == 0u) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (scheme == k_bounded_nthash_u32_r7_sum_scheme_identity) {
        if (kmer_size != 15u) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return minimizer_scalar_width::u32;
    }
    if (scheme != k_minimap_scheme_identity || kmer_size > 28u) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (kmer_size <= 8u)
        return minimizer_scalar_width::u16;
    if (kmer_size <= 16u)
        return minimizer_scalar_width::u32;
    return minimizer_scalar_width::u64;
}

/** Derive the one admitted grouped-SoA profile. */
/// Derive a grouped-SoA profile from a decoded manifest.
[[nodiscard]] constexpr std::expected<grouped_soa_minimizer_profile,
                                      minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(minimizer_scheme_identity scheme,
                                     std::uint16_t alphabet_code,
                                     std::uint16_t kmer_size,
                                     std::uint64_t position_segment_span_bp,
                                     std::uint64_t max_occ_cap) noexcept {
    const auto hash_width = select_grouped_soa_hash_width(scheme, alphabet_code, kmer_size);
    if (!hash_width || position_segment_span_bp == 0u ||
        max_occ_cap == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const minimizer_scalar_width position_width =
        position_segment_span_bp <= (std::uint64_t{1u} << 15u)   ? minimizer_scalar_width::u16
        : position_segment_span_bp <= (std::uint64_t{1u} << 31u) ? minimizer_scalar_width::u32
                                                                 : minimizer_scalar_width::u64;
    const minimizer_scalar_width count_width =
        max_occ_cap <= 65'534u ? minimizer_scalar_width::u16
        : max_occ_cap <=
                static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) - 1u
            ? minimizer_scalar_width::u32
            : minimizer_scalar_width::u64;
    const std::uint64_t max_hash =
        scheme == k_bounded_nthash_u32_r7_sum_scheme_identity
            ? static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)())
            : (std::uint64_t{1u} << (2u * kmer_size)) - 1u;
    return grouped_soa_minimizer_profile{
        .layout = minimizer_generation_layout::grouped_soa_segment_local,
        .minimizer_scheme = scheme,
        .alphabet_code = alphabet_code,
        .kmer_size = kmer_size,
        .hash_width = *hash_width,
        .position_width = position_width,
        .count_width = count_width,
        .max_hash = max_hash,
        .max_occ_cap = max_occ_cap,
    };
}

/** Reject profiles that disagree with canonical grouped-SoA derivation. */
[[nodiscard]] constexpr std::expected<void, minimizer_generation_encoding_error>
validate_grouped_soa_minimizer_profile(const grouped_soa_minimizer_profile& profile) noexcept {
    const std::uint64_t representative_span =
        profile.position_width == minimizer_scalar_width::u16   ? (std::uint64_t{1u} << 15u)
        : profile.position_width == minimizer_scalar_width::u32 ? (std::uint64_t{1u} << 31u)
                                                                : (std::uint64_t{1u} << 31u) + 1u;
    const auto expected = derive_grouped_soa_minimizer_profile(profile.minimizer_scheme,
                                                               profile.alphabet_code,
                                                               profile.kmer_size,
                                                               representative_span,
                                                               profile.max_occ_cap);
    if (!expected || profile.layout != minimizer_generation_layout::grouped_soa_segment_local ||
        expected->hash_width != profile.hash_width ||
        expected->count_width != profile.count_width ||
        expected->position_width != profile.position_width ||
        profile.offset_policy != grouped_soa_offset_policy::per_extent_narrowest ||
        profile.control_layout != grouped_soa_control_layout::soa_hash_prefix_offset_count ||
        profile.count_semantics != grouped_soa_count_semantics::saturating_cap_plus_one ||
        profile.position_semantics != grouped_soa_position_semantics::all_segment_local_bp_strand ||
        profile.max_hash != expected->max_hash ||
        profile.payload_layout_abi_revision != k_grouped_soa_payload_layout_abi_revision ||
        profile.gpu_abi_revision != k_grouped_soa_gpu_abi_revision ||
        !profile.segment_local_positions) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return {};
}

[[nodiscard]] constexpr std::expected<grouped_soa_minimizer_profile,
                                      minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(const minimizer_generation_manifest& manifest) noexcept;

/// Return the exact manifest byte count for a supported layout.
[[nodiscard]] constexpr std::size_t
minimizer_manifest_encoded_bytes(minimizer_generation_layout layout) noexcept {
    return layout == minimizer_generation_layout::grouped_soa_segment_local ? 160u : 0u;
}

/// Return the exact directory-entry byte count for a validated profile.
[[nodiscard]] constexpr std::size_t
minimizer_directory_entry_encoded_bytes(const grouped_soa_minimizer_profile& profile) noexcept {
    return profile.layout == minimizer_generation_layout::grouped_soa_segment_local ? 128u : 0u;
}

/// Select the narrowest offset width that represents a terminal prefix.
[[nodiscard]] constexpr minimizer_scalar_width
select_grouped_soa_offset_width(std::uint64_t terminal_prefix) noexcept {
    return terminal_prefix <= (std::numeric_limits<std::uint16_t>::max)()
               ? minimizer_scalar_width::u16
           : terminal_prefix <= (std::numeric_limits<std::uint32_t>::max)()
               ? minimizer_scalar_width::u32
               : minimizer_scalar_width::u64;
}

/// Byte offsets and total size of one grouped-SoA control plane.
struct grouped_soa_control_plane_offsets {
    std::uint64_t hash_offset = 0;
    std::uint64_t prefix_offset = 0;
    std::uint64_t count_offset = 0;
    std::uint64_t encoded_bytes = 0;

    [[nodiscard]] bool operator==(const grouped_soa_control_plane_offsets&) const = default;
};

/// Plan aligned hash, prefix, and count arrays for one control plane.
[[nodiscard]] constexpr std::expected<grouped_soa_control_plane_offsets,
                                      minimizer_generation_encoding_error>
plan_grouped_soa_control_plane(std::uint64_t control_count,
                               minimizer_scalar_width hash_width,
                               minimizer_scalar_width offset_width,
                               minimizer_scalar_width count_width) noexcept {
    if (!is_minimizer_scalar_width(hash_width) || !is_minimizer_scalar_width(offset_width) ||
        !is_minimizer_scalar_width(count_width)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto checked_mul = [](std::uint64_t lhs,
                                std::uint64_t rhs) -> std::optional<std::uint64_t> {
        if (rhs != 0 && lhs > (std::numeric_limits<std::uint64_t>::max)() / rhs) {
            return std::nullopt;
        }
        return lhs * rhs;
    };
    const auto align_up = [](std::uint64_t value,
                             std::uint64_t alignment) -> std::optional<std::uint64_t> {
        const std::uint64_t remainder = value % alignment;
        if (remainder == 0)
            return value;
        const std::uint64_t add = alignment - remainder;
        if (value > (std::numeric_limits<std::uint64_t>::max)() - add) {
            return std::nullopt;
        }
        return value + add;
    };
    const std::uint64_t h = static_cast<std::uint8_t>(hash_width);
    const std::uint64_t o = static_cast<std::uint8_t>(offset_width);
    const std::uint64_t c = static_cast<std::uint8_t>(count_width);
    const auto hash_end = checked_mul(control_count, h);
    if (!hash_end || control_count == (std::numeric_limits<std::uint64_t>::max)()) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const auto prefix_offset = align_up(*hash_end, std::max<std::uint64_t>(4u, o));
    const auto prefix_bytes = checked_mul(control_count + 1u, o);
    if (!prefix_offset || !prefix_bytes ||
        *prefix_offset > (std::numeric_limits<std::uint64_t>::max)() - *prefix_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const auto count_offset =
        align_up(*prefix_offset + *prefix_bytes, std::max<std::uint64_t>(4u, c));
    const auto count_bytes = checked_mul(control_count, c);
    if (!count_offset || !count_bytes ||
        *count_offset > (std::numeric_limits<std::uint64_t>::max)() - *count_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    const auto encoded_bytes = align_up(*count_offset + *count_bytes, 8u);
    if (!encoded_bytes) {
        return std::unexpected(minimizer_generation_encoding_error::arithmetic_overflow);
    }
    return grouped_soa_control_plane_offsets{
        .hash_offset = 0,
        .prefix_offset = *prefix_offset,
        .count_offset = *count_offset,
        .encoded_bytes = *encoded_bytes,
    };
}

/// Derive the physical position-extent key from one authoritative logical /
/// control key.  The checked boundary keeps both payload namespaces below the
/// temporary-run reservation without changing the directory wire.
[[nodiscard]] constexpr std::expected<index_sub_key_t, minimizer_generation_encoding_error>
derive_minimizer_position_extent_sub_key(index_sub_key_t control_sub_key) noexcept {
    if (control_sub_key < 0 || control_sub_key >= k_position_extent_sub_key_base) {
        return std::unexpected(minimizer_generation_encoding_error::sub_key);
    }
    return k_position_extent_sub_key_base + control_sub_key;
}

/// Project one physical extent key back into the authoritative logical /
/// control namespace without performing unchecked signed arithmetic.
[[nodiscard]] constexpr std::expected<index_sub_key_t, minimizer_generation_encoding_error>
project_minimizer_extent_sub_key(index_sub_key_t physical_sub_key, bool position_extent) noexcept {
    if (position_extent) {
        if (physical_sub_key < k_position_extent_sub_key_base ||
            physical_sub_key >= k_temporary_run_sub_key_base) {
            return std::unexpected(minimizer_generation_encoding_error::sub_key);
        }
        return physical_sub_key - k_position_extent_sub_key_base;
    }
    if (physical_sub_key < 0 || physical_sub_key >= k_position_extent_sub_key_base) {
        return std::unexpected(minimizer_generation_encoding_error::sub_key);
    }
    return physical_sub_key;
}

/// Fixed 64-byte generation publication head stored at k_generation_head_sub_key.
struct minimizer_generation_head {
    static constexpr std::uint32_t magic = 0x42494F48u; // BIOH numeric tag
    static constexpr std::size_t encoded_bytes = 64u;

    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t storage_contract_checksum = 0;
    std::uint64_t manifest_checksum = 0;
    std::uint64_t source_content_checksum = 0;

    [[nodiscard]] bool operator==(const minimizer_generation_head&) const = default;
};

/// Immutable grouped-SoA generation manifest with its persisted shape fields.
struct minimizer_generation_manifest {
    static constexpr std::uint32_t magic = 0x42494F47u; // BIOG numeric tag
    static constexpr std::size_t header_bytes = 128u;
    static constexpr std::size_t encoded_bytes = 160u;
    static constexpr std::uint16_t uniform_segment_geometry_revision =
        k_minimizer_segment_geometry_revision;

    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint64_t generation_id = 0;
    std::uint64_t source_identity = 0;
    std::uint64_t source_content_checksum = 0;
    std::uint64_t target_length = 0;
    std::uint64_t control_count = 0;
    std::uint64_t position_count = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t payload_checksum = 0;
    std::uint64_t directory_bytes = 0;
    std::uint64_t directory_checksum = 0;
    std::uint32_t position_segment_count = 0;
    std::uint32_t extent_count = 0;
    std::uint32_t directory_chunk_count = 0;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    std::uint32_t gpu_abi_revision = 0;
    std::uint64_t position_segment_span_bp = 0;
    std::uint16_t segment_geometry_revision = 0;
    std::uint16_t minimizer_scheme_id = k_minimap_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_minimap_scheme_revision;
    minimizer_producer_backend producer_backend_id = minimizer_producer_backend::invalid;
    std::uint8_t producer_identity_revision = 0u;
    minimizer_scalar_width hash_width = minimizer_scalar_width::u32;
    minimizer_scalar_width position_width = minimizer_scalar_width::u32;
    minimizer_scalar_width count_width = minimizer_scalar_width::u32;
    grouped_soa_offset_policy offset_policy = grouped_soa_offset_policy::per_extent_narrowest;
    grouped_soa_control_layout control_layout =
        grouped_soa_control_layout::soa_hash_prefix_offset_count;
    grouped_soa_count_semantics count_semantics =
        grouped_soa_count_semantics::saturating_cap_plus_one;
    grouped_soa_position_semantics position_semantics =
        grouped_soa_position_semantics::all_segment_local_bp_strand;
    std::uint64_t max_hash = 0u;
    std::uint64_t max_occ_cap = 0u;

    [[nodiscard]] bool operator==(const minimizer_generation_manifest&) const = default;
};

/// Caller-owned storage contract checked before consuming one generation.
struct expected_minimizer_generation_identity {
    minimizer_generation_layout layout = minimizer_generation_layout::invalid;
    std::uint16_t alphabet_code = 0;
    std::uint16_t storage_unit_byte_width = 0;
    std::uint16_t kmer_size = 0;
    std::uint16_t window_size = 0;
    std::uint32_t gpu_abi_revision = k_grouped_soa_gpu_abi_revision;
    std::uint16_t minimizer_scheme_id = k_invalid_minimizer_scheme_id;
    std::uint16_t minimizer_scheme_revision = k_invalid_minimizer_scheme_revision;
    minimizer_producer_backend producer_backend_id = minimizer_producer_backend::invalid;
    std::uint8_t producer_identity_revision = 0u;
    std::optional<std::uint64_t> max_occ_cap{};

    [[nodiscard]] bool operator==(const expected_minimizer_generation_identity&) const = default;
};

/// Construct an independent persisted-read contract from the selected sketch
/// backend. No manifest field participates in this selection.
[[nodiscard]] constexpr std::expected<expected_minimizer_generation_identity,
                                      minimizer_generation_encoding_error>
expected_grouped_soa_generation_identity_for_backend(
    minimizer_generation_layout layout,
    minimizer_sketch_backend backend,
    std::uint16_t alphabet_code,
    std::uint16_t storage_unit_byte_width,
    std::uint16_t kmer_size,
    std::uint16_t window_size,
    std::optional<std::uint64_t> max_occ_cap = std::nullopt) noexcept {
    const minimizer_scheme_identity scheme = minimizer_scheme_identity_for_backend(backend);
    const minimizer_producer_identity producer_backend =
        minimizer_producer_backend_identity_for_backend(backend);
    const auto profile = derive_grouped_soa_minimizer_profile(
        scheme,
        alphabet_code,
        kmer_size,
        1u,
        max_occ_cap.value_or((std::numeric_limits<std::uint64_t>::max)()));
    if (!profile || storage_unit_byte_width == 0 || window_size == 0 ||
        layout != minimizer_generation_layout::grouped_soa_segment_local || !max_occ_cap) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return expected_minimizer_generation_identity{
        .layout = layout,
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width = storage_unit_byte_width,
        .kmer_size = kmer_size,
        .window_size = window_size,
        .gpu_abi_revision = profile->gpu_abi_revision,
        .minimizer_scheme_id = scheme.id,
        .minimizer_scheme_revision = scheme.revision,
        .producer_backend_id = producer_backend.id,
        .producer_identity_revision = producer_backend.revision,
        .max_occ_cap = max_occ_cap,
    };
}

/// Logical directory entry. Persistence always uses the fixed codec.
/// `sub_key` is the logical/control key. Position rows use the checked derived
/// key k_position_extent_sub_key_base + sub_key. Both extent-local chunk sequences start
/// at zero and use the fixed 128-byte directory ABI.
struct grouped_soa_bucket_directory_entry {
    static constexpr std::size_t header_bytes = 96u;
    static constexpr std::size_t encoded_bytes = 128u;

    std::uint32_t partition_id = 0;
    std::int32_t position_segment = 0;
    std::uint64_t lo_hash = 0;
    std::uint64_t hi_hash = 0;
    index_sub_key_t sub_key = 0;
    std::uint32_t control_first_chunk = 0;
    std::uint32_t control_chunk_count = 0;
    std::uint32_t position_first_chunk = 0;
    std::uint32_t position_chunk_count = 0;
    std::uint64_t control_element_count = 0;
    std::uint64_t position_element_count = 0;
    std::uint64_t control_encoded_bytes = 0;
    std::uint64_t position_encoded_bytes = 0;
    std::uint64_t control_checksum = 0;
    std::uint64_t position_checksum = 0;
    minimizer_scalar_width shape_offset_width = minimizer_scalar_width::u32;

    [[nodiscard]] bool operator==(const grouped_soa_bucket_directory_entry&) const = default;
};

/// Variable-size fixed-record wrapper. Its unused suffix is always zero. The
/// byte_count is the only exposed range so stored checksums and golden bytes remain
/// exactly 128/96 bytes.
template <std::size_t p_maximum_bytes> struct encoded_minimizer_fixed_record {
    std::array<std::byte, p_maximum_bytes> storage{};
    std::size_t byte_count = 0;

    [[nodiscard]] constexpr const std::byte* data() const noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr std::byte* data() noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return byte_count;
    }
    [[nodiscard]] constexpr const std::byte* begin() const noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr const std::byte* end() const noexcept {
        return storage.data() + byte_count;
    }
    [[nodiscard]] constexpr std::byte* begin() noexcept {
        return storage.data();
    }
    [[nodiscard]] constexpr std::byte* end() noexcept {
        return storage.data() + byte_count;
    }
    [[nodiscard]] constexpr const std::byte& operator[](std::size_t index) const noexcept {
        return storage[index];
    }
    [[nodiscard]] constexpr std::byte& operator[](std::size_t index) noexcept {
        return storage[index];
    }

    [[nodiscard]] constexpr bool operator==(const encoded_minimizer_fixed_record&) const = default;

    template <std::size_t p_size>
    [[nodiscard]] friend constexpr bool
    operator==(const encoded_minimizer_fixed_record& lhs,
               const std::array<std::byte, p_size>& rhs) noexcept {
        if (lhs.byte_count != p_size)
            return false;
        for (std::size_t i = 0; i < p_size; ++i) {
            if (lhs.storage[i] != rhs[i])
                return false;
        }
        return true;
    }

    template <std::size_t p_size>
    [[nodiscard]] friend constexpr bool
    operator==(const std::array<std::byte, p_size>& lhs,
               const encoded_minimizer_fixed_record& rhs) noexcept {
        return rhs == lhs;
    }
};

using encoded_minimizer_manifest =
    encoded_minimizer_fixed_record<minimizer_generation_manifest::encoded_bytes>;
using encoded_grouped_soa_bucket_directory_entry =
    encoded_minimizer_fixed_record<grouped_soa_bucket_directory_entry::encoded_bytes>;

[[nodiscard]] constexpr std::expected<grouped_soa_minimizer_profile,
                                      minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(const minimizer_generation_manifest& manifest) noexcept {
    const minimizer_scheme_identity scheme{
        .id = manifest.minimizer_scheme_id,
        .revision = manifest.minimizer_scheme_revision,
    };
    if (manifest.layout != minimizer_generation_layout::grouped_soa_segment_local) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    const auto profile = derive_grouped_soa_minimizer_profile(
        scheme,
        manifest.alphabet_code,
        manifest.kmer_size,
        manifest.position_segment_span_bp == 0 ? 1u : manifest.position_segment_span_bp,
        manifest.max_occ_cap);
    if (!profile || profile->hash_width != manifest.hash_width ||
        profile->position_width != manifest.position_width ||
        profile->count_width != manifest.count_width ||
        profile->offset_policy != manifest.offset_policy ||
        profile->control_layout != manifest.control_layout ||
        profile->count_semantics != manifest.count_semantics ||
        profile->position_semantics != manifest.position_semantics ||
        profile->max_hash != manifest.max_hash) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return profile;
}

/// Half-open ordinal interval covering a requested base-pair range.
struct minimizer_position_segment_range {
    std::uint32_t begin = 0;
    std::uint32_t end = 0; // half-open

    [[nodiscard]] bool operator==(const minimizer_position_segment_range&) const = default;
};

} // namespace bioseq::indexing

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

/// Exact allocation bound for the transient flat hash table.
struct minimizer_hashmap_table_allocation_plan {
    std::uint64_t distinct_upper_bound = 0u;
    std::uint64_t requested_slots = 0u;
    std::uint64_t group_count = 0u;
    std::uint64_t capacity = 0u;
    std::uint64_t allocation_bytes = 0u;
    bool valid = false;
};

/// Plan the table capacity and retained bytes for a distinct-hash upper bound.
[[nodiscard]] constexpr minimizer_hashmap_table_allocation_plan
plan_minimizer_hashmap_table_allocation(std::uint64_t possible_distinct) noexcept {
    using bioseq::common::saturating_add;
    using bioseq::common::saturating_mul;
    constexpr std::uint64_t sentinel_margin = 64u;
    constexpr std::uint64_t load_numerator = 7u;
    constexpr std::uint64_t load_denominator = 8u;
    constexpr std::uint64_t slots_per_group = 15u;
    constexpr std::uint64_t bytes_per_group = 256u;
    constexpr std::uint64_t max = (std::numeric_limits<std::uint64_t>::max)();

    // An empty sketch builds no table at all.  Keep that real lifetime in the
    // exact plan instead of charging the non-empty sentinel margin and then
    // rejecting the builder's zero retained bytes at handoff validation.
    if (possible_distinct == 0u) {
        return minimizer_hashmap_table_allocation_plan{.valid = true};
    }

    const std::uint64_t bounded_distinct = saturating_add(possible_distinct, sentinel_margin);
    if (bounded_distinct == max)
        return {};
    const std::uint64_t quotient = bounded_distinct / load_numerator;
    const std::uint64_t remainder = bounded_distinct % load_numerator;
    const std::uint64_t requested_slots =
        saturating_add(saturating_mul(quotient, load_denominator),
                       (remainder * load_denominator + load_numerator - 1u) / load_numerator);
    if (requested_slots == max)
        return {};
    const std::uint64_t groups_needed = requested_slots / slots_per_group + 1u;
    std::uint64_t groups = 2u;
    while (groups < groups_needed) {
        if (groups > max / 2u)
            return {};
        groups *= 2u;
    }
    const std::uint64_t capacity = saturating_mul(groups, slots_per_group);
    const std::uint64_t allocation_bytes = saturating_mul(groups, bytes_per_group);
    if (capacity == max || allocation_bytes == max || capacity == 0u) {
        return {};
    }
    return minimizer_hashmap_table_allocation_plan{
        .distinct_upper_bound = bounded_distinct,
        .requested_slots = requested_slots,
        .group_count = groups,
        .capacity = capacity - 1u,
        .allocation_bytes = allocation_bytes,
        .valid = true,
    };
}

/// Transient storage-free minimizer lookup used by in-memory seed routes.
struct minimizer_hashmap_fragment_index {
  private:
    std::unique_ptr<detail::minimizer_hashmap_fragment_index_table> table_;

    void clear_table();
    void reserve_table(std::size_t bucket_count);
    void reserve_table_for_distinct_upper_bound(std::size_t distinct_count);
    void emplace_table(std::uint64_t hash, std::uint64_t packed);

    template <class p_minimizer_type>
    static void sort_minimizers_in_place_(std::vector<p_minimizer_type>& sorted_minimizers) {
        if (sorted_minimizers.empty())
            return;
        // `MinimizerRepr::kmer()` deliberately exposes a uint64_t ABI even
        // when the selected representation carries a much narrower hash.  Do
        // not let that ABI choice force radix_sort through known-zero high
        // bytes: XXL_HIFI uses MinimizerRepr<33, 15> with exactly 30 hash bits,
        // so a uint64_t key performs four all-zero radix levels over the whole
        // target before reaching a significant byte.  Returning the smallest
        // unsigned key type that can represent the public kmer() domain
        // preserves the ordering exactly while removing those scans.  The
        // exported 128-bit representation can report a layout hash field
        // wider than 64 bits while kmer() intentionally remains uint64_t; it
        // therefore stays on the uint64_t radix key.
        using minimizer_layout = typename p_minimizer_type::layout_type;
        using public_kmer = decltype(std::declval<const p_minimizer_type&>().kmer());
        static_assert(minimizer_layout::k_hash_bits > 0u);
        static_assert(std::is_same_v<public_kmer, std::uint64_t>);
        using kmer_radix_key = std::conditional_t<
            (minimizer_layout::k_hash_bits <= 8u),
            std::uint8_t,
            std::conditional_t<(minimizer_layout::k_hash_bits <= 16u),
                               std::uint16_t,
                               std::conditional_t<(minimizer_layout::k_hash_bits <= 32u),
                                                  std::uint32_t,
                                                  std::uint64_t>>>;
        struct kmer_key {
            [[nodiscard, gnu::always_inline]]
            inline kmer_radix_key operator()(const p_minimizer_type& m) const noexcept {
                return static_cast<kmer_radix_key>(m.kmer());
            }
        };
        bioseq::sorting::radix_sort(sorted_minimizers.data(),
                                    sorted_minimizers.data() + sorted_minimizers.size(),
                                    kmer_key{});
    }

    template <class p_minimizer_type>
    void build_presorted_in_place_(std::vector<p_minimizer_type>& sorted_minimizers,
                                   sequence_size_t target_length,
                                   bool reserve_full_table_once) {
        target_len = target_length;
        clear_table();
        positions.clear();

        const std::size_t total = sorted_minimizers.size();
        if (total == 0u) {
            return;
        }

        positions.resize(total);
        if (reserve_full_table_once) {
            // The admitted Vulkan route reserves for the worst-case D=N once.
            // A single allocation avoids half-size allocation/rehash overlap.
            reserve_table_for_distinct_upper_bound(total);
        } else {
            // Apply the CPU allocation policy for every non-Vulkan caller.
            reserve_table(total / 2u + 16u);
        }
        std::size_t i = 0;
        while (i < total) {
            const std::uint64_t kmer = sorted_minimizers[i].kmer();
            std::size_t j = i;
            while (j < total && sorted_minimizers[j].kmer() == kmer) {
                positions[j] = sorted_minimizers[j].pos_strand_encoded();
                ++j;
            }
            emplace_table(kmer,
                          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(i)) << 32) |
                              static_cast<std::uint64_t>(static_cast<std::uint32_t>(j - i)));
            i = j;
        }
    }

    template <class p_minimizer_type>
    void build_sorted_in_place(std::vector<p_minimizer_type>& sorted_minimizers,
                               sequence_size_t target_length,
                               bool reserve_full_table_once = false) {
        static const auto* _desc =
            bioseq::instrumentation::register_static_region("target_index_build");
        auto _scope = bioseq::instrumentation::region(_desc);
        sort_minimizers_in_place_(sorted_minimizers);
        build_presorted_in_place_(sorted_minimizers, target_length, reserve_full_table_once);
    }

  public:
    /// Global `(position << 1) | strand` values in hash-bucket order.
    std::vector<std::uint64_t> positions; // (last_pos<<1)|strand, full uint64
    /// Full target length represented by this lookup.
    sequence_size_t target_len = 0;

    /// Construct an empty lookup.
    minimizer_hashmap_fragment_index();
    /// Release the transient table and position storage.
    ~minimizer_hashmap_fragment_index();
    /// Copy lookup contents into independent storage.
    minimizer_hashmap_fragment_index(const minimizer_hashmap_fragment_index& other);
    /// Replace lookup contents with a copy.
    minimizer_hashmap_fragment_index& operator=(const minimizer_hashmap_fragment_index& other);
    /// Transfer lookup contents without rebuilding the table.
    minimizer_hashmap_fragment_index(minimizer_hashmap_fragment_index&& other) noexcept;
    /// Replace lookup contents by transfer.
    minimizer_hashmap_fragment_index& operator=(minimizer_hashmap_fragment_index&& other) noexcept;

    /// Magic word stored in the transient index binary header.
    static constexpr std::uint32_t k_encoded_magic = 0xB1000001u;
    /// Revision of the transient index binary layout.
    static constexpr std::uint32_t k_encoded_layout_revision = 2u;
    /// Registry name for per-fragment minimizer indexes.
    static constexpr std::string_view index_name = k_minimizers_per_fragment_index_name;

    /// Fixed little-endian transient index header fields.
    struct encoded_header {
        std::uint32_t magic;
        std::uint32_t layout_revision;
        std::uint64_t target_len; ///< Full 64-bit target length.
        std::uint32_t bucket_count;
        std::uint32_t reserved; ///< Padding written as zero
        std::uint64_t total_positions;
    };
    /// Fixed little-endian hash bucket fields.
    struct encoded_bucket {
        std::uint64_t hash;
        std::uint32_t pos_offset;
        std::uint32_t pos_count;
    };
    static constexpr std::size_t k_encoded_header_bytes = sizeof(encoded_header);
    static constexpr std::size_t k_encoded_bucket_bytes = sizeof(encoded_bucket);
    static_assert(k_encoded_header_bytes == 32, "wire header layout pinned");
    static_assert(k_encoded_bucket_bytes == 16, "wire bucket layout pinned");
    static_assert(std::endian::native == std::endian::little,
                  "serialized index requires a little-endian host");
    static_assert(sizeof(std::size_t) >= 8, "serialized index requires 64-bit size_t");

    /// Return the number of distinct hash buckets.
    [[nodiscard]] std::size_t table_size() const noexcept;
    /// Return whether the lookup has no hash buckets.
    [[nodiscard]] bool table_empty() const noexcept;
    /// Return bytes currently retained by the hash table.
    [[nodiscard]] std::uint64_t table_allocation_bytes() const noexcept;
    /// Return the peak hash-table bytes retained during this lifetime.
    [[nodiscard]] std::uint64_t table_peak_allocation_bytes() const noexcept;
    /// Visit every hash bucket through an erased callback.
    void for_each_table_entry_erased(
        void* ctx, void (*emit)(void* ctx, std::uint64_t hash, std::uint64_t packed)) const;

    /// Visit every hash bucket with a typed callable.
    template <class p_function> void for_each_table_entry(p_function&& emit) const {
        using fn = std::remove_reference_t<p_function>;
        for_each_table_entry_erased(&emit, [](void* ctx, std::uint64_t hash, std::uint64_t packed) {
            (*static_cast<fn*>(ctx))(hash, packed);
        });
    }

    /// Build from a caller-owned immutable minimizer sequence.
    template <class p_minimizer_type>
    void build(const std::vector<p_minimizer_type>& target_minimizers,
               sequence_size_t target_length) {
        // A call-local copy bounds scratch lifetime to this operation so later
        // GPU stages do not retain the target-sized allocation in worker RSS.
        std::vector<p_minimizer_type> sorted_minimizers{target_minimizers.begin(),
                                                        target_minimizers.end()};
        build_sorted_in_place(sorted_minimizers, target_length);
    }

    /// Sort one caller-owned minimizer source by its canonical hash without
    /// constructing the FOA lookup table or a positions vector.  The direct
    /// mapped Vulkan route uses this same production radix implementation,
    /// then streams the sorted source into its final resident allocation.
    template <class p_minimizer_type>
    static void sort_source_by_hash_in_place(std::vector<p_minimizer_type>& source) {
        sort_minimizers_in_place_(source);
    }

    /// Build a transient lookup index by consuming caller-owned minimizers.
    /// Sorting happens in the caller's allocation and the vector is
    /// deterministically deallocated before this function returns.
    template <class p_minimizer_type>
    void build_consuming(std::vector<p_minimizer_type>&& target_minimizers,
                         sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers, target_length);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Admitted Vulkan build: reserve one table for worst-case D=N so no FOA
    /// growth allocation can overlap it. The caller must acquire positions +
    /// planned-table admission before entering this method.
    template <class p_minimizer_type>
    void build_consuming_full_table(std::vector<p_minimizer_type>&& target_minimizers,
                                    sequence_size_t target_length) {
        build_sorted_in_place(target_minimizers,
                              target_length,
                              /*reserve_full_table_once=*/true);
        std::vector<p_minimizer_type>{}.swap(target_minimizers);
    }

    /// Deallocate lookup storage after the final seed consumer.  This is for
    /// an operation-owned transient index only. Borrowed persisted indexes
    /// must never be passed to the post-seed lifetime coordinator.
    void release_storage() noexcept;

    /// Radix-bin pre-extracted global (kmer, encoded_position) pairs.
    /// `pairs` is mutable and radix-sorted in place in caller-owned scratch without copying.
    void build_from_pairs(std::span<std::pair<std::uint64_t, std::uint64_t>> pairs,
                          sequence_size_t target_length);

    /// Return all encoded positions for one minimizer hash.
    [[nodiscard]] std::span<const std::uint64_t> lookup(std::uint64_t kmer_hash) const;

    /// Add a global base-pair offset to every stored position.
    void add_position_offset(sequence_size_t offset);

    /// Return the full target length represented by the lookup.
    [[nodiscard]] sequence_size_t target_length() const {
        return target_len;
    }

    /// Return the exact serialized byte count.
    [[nodiscard]] std::size_t serialize_binary_size() const;

    /// Serialize into an exact-size caller-owned buffer.
    void serialize_to_binary(char* output) const;

    /// Decode and validate one fixed little-endian transient index image.
    [[nodiscard]] static minimizer_hashmap_fragment_index
    deserialize_from_binary(const char* input, std::size_t input_size);
};
static_assert(minimizer_lookup_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model minimizer_lookup_index");
static_assert(persistable_minimizer_index<minimizer_hashmap_fragment_index>,
              "minimizer_hashmap_fragment_index must model persistable_minimizer_index");

/// Encode one sorted-dense control without persisting native struct layout.
[[nodiscard]] constexpr std::array<std::byte, 16> encode_minimizer_serialized_bucket(
    const minimizer_hashmap_fragment_index::encoded_bucket& bucket) noexcept {
    std::array<std::byte, 16> output{};
    std::span<std::byte> bytes{output};
    minimizer_generation_wire_detail::write_le<std::uint64_t>(bytes, 0, bucket.hash);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(bytes, 8, bucket.pos_offset);
    minimizer_generation_wire_detail::write_le<std::uint32_t>(bytes, 12, bucket.pos_count);
    return output;
}

/// Decode one exact sorted-dense control from fixed little-endian bytes.
[[nodiscard]] constexpr std::expected<minimizer_hashmap_fragment_index::encoded_bucket,
                                      minimizer_generation_encoding_error>
decode_minimizer_serialized_bucket(std::span<const std::byte> input) noexcept {
    if (input.size() != 16u) {
        return std::unexpected(minimizer_generation_encoding_error::encoded_size);
    }
    return minimizer_hashmap_fragment_index::encoded_bucket{
        .hash = minimizer_generation_wire_detail::read_le<std::uint64_t>(input, 0),
        .pos_offset = minimizer_generation_wire_detail::read_le<std::uint32_t>(input, 8),
        .pos_count = minimizer_generation_wire_detail::read_le<std::uint32_t>(input, 12),
    };
}

} // namespace bioseq::indexing
