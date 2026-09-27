/**
 * @file minimizer_hashmap/grouped_soa/contracts.cppm
 * @brief Declares persisted grouped-SoA profiles and storage records.
 */
export module bioseq.indexing.minimizer_hashmap:grouped_soa_contracts;

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
import bioseq.pipeline.runtime_contract;
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
inline constexpr std::uint16_t k_grouped_soa_extension_layout_revision = 2u;
/// Exact byte length of the grouped-SoA manifest extension.
inline constexpr std::uint16_t k_grouped_soa_extension_bytes = 48u;
/// Persisted supported_alphabets pack index for DNA4. This is an on-wire code,
/// not the alphabet cardinality.
inline constexpr std::uint16_t k_minimizer_persisted_dna4_alphabet_code = 0u;
/// DNA5 wire identity comes from the same generated profile registry as queries.
inline constexpr std::uint16_t k_minimizer_persisted_dna5_alphabet_code =
    ::bioseq::minimizers::minimizer_profile_alphabet_id<seq::dna5>();
static_assert(k_minimizer_persisted_dna4_alphabet_code ==
              ::bioseq::minimizers::minimizer_profile_alphabet_id<seq::dna4>());
/// Maximum producer-local position extent used by minimizer-fragment generation.
inline constexpr std::uint64_t k_minimizer_fragment_algorithm_position_extent_bp = 32'768u;
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

    /// Test whether storage issued a nonzero replay-set generation token.
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
    // Translate the runtime route while keeping an unmatched value fail-closed.
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

/**
 * Return the algorithm genuinely executed by an explicitly named sketch backend.
 *
 * This mapping is for backend-oriented fixtures whose request surface predates
 * the algorithm selector. Production plans carry their requested algorithm
 * directly, including their generated requested/effective algorithm binding.
 */
[[nodiscard]] constexpr std::optional<::bioseq::simd::minimizer_algorithm>
minimizer_algorithm_for_explicit_sketch_backend(minimizer_sketch_backend backend) noexcept {
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    switch (backend) {
    case minimizer_sketch_backend::scalar:
        return algorithm_type::scalar;
    case minimizer_sketch_backend::exact_simd:
        return algorithm_type::two_stack_nthash;
    case minimizer_sketch_backend::bounded_nthash_simd:
        return algorithm_type::bounded_nthash_simd;
    case minimizer_sketch_backend::vulkan_segment:
        return algorithm_type::vulkan_minimap_integer;
    }
    return std::nullopt;
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

/// Shared exact scalar-width tags; persisted ordinals remain 2, 4, and 8 bytes.
using ::bioseq::minimizers::minimizer_scalar_width;

/**
 * Closed READY identity selected from the generated minimizer registry.
 *
 * Existing manifest fields carry alphabet, packed-input width, runtime k/w,
 * and hash width on wire.  Keeping those axes here as well makes producer
 * handoff and receipt comparisons value-complete; the codec reconstructs the
 * duplicated values and standalone validation rejects any disagreement.
 */
struct minimizer_generation_algorithm_identity {
    ::bioseq::simd::minimizer_algorithm requested_algorithm =
        ::bioseq::simd::minimizer_algorithm::scalar;
    ::bioseq::simd::minimizer_algorithm effective_algorithm =
        ::bioseq::simd::minimizer_algorithm::scalar;
    ::bioseq::minimizers::minimizer_physical_record_id physical_record_id =
        ::bioseq::minimizers::minimizer_physical_record_id::invalid;
    ::bioseq::minimizers::minimizer_highway_owner_id highway_owner_id =
        ::bioseq::minimizers::minimizer_highway_owner_id::none;
    std::uint16_t alphabet_id = 0u;
    std::uint8_t record_position_log2 = 0u;
    std::uint8_t physical_kmer_size = 0u;
    minimizer_scalar_width hash_width = minimizer_scalar_width::u32;
    std::uint8_t packed_storage_bits = 0u;
    std::uint16_t runtime_kmer_size = 0u;
    std::uint16_t runtime_window_size = 0u;
    std::uint16_t payload_layout_abi_revision = 0u;

    [[nodiscard]] bool
    operator==(const minimizer_generation_algorithm_identity&) const = default;
};

static_assert(k_grouped_soa_payload_layout_abi_revision == 1u);
static_assert(k_minimizer_producer_identity_revision == 1u);
static_assert(k_grouped_soa_payload_layout_abi_revision ==
              ::bioseq::minimizers::minimizer_profile_payload_layout_abi_revision);
static_assert(k_minimizer_producer_identity_revision ==
              ::bioseq::minimizers::minimizer_profile_producer_identity_revision);

/// Resolve the runtime sketch route from an already selected effective algorithm.
///
/// Persisted callers must select the complete generated identity against the
/// source alphabet and packed width first. The effective algorithm selects the
/// producer while the complete identity preserves the original request.
[[nodiscard]] constexpr std::optional<minimizer_sketch_backend>
minimizer_sketch_backend_for_effective_algorithm(
    ::bioseq::simd::minimizer_algorithm effective_algorithm) noexcept {
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    switch (effective_algorithm) {
    case algorithm_type::scalar:
        return minimizer_sketch_backend::scalar;
    case algorithm_type::simd_minimap2_reference:
    case algorithm_type::two_stack_nthash:
        return minimizer_sketch_backend::exact_simd;
    case algorithm_type::bounded_nthash_simd:
        return minimizer_sketch_backend::bounded_nthash_simd;
    case algorithm_type::vulkan_minimap_integer:
        return minimizer_sketch_backend::vulkan_segment;
    }
    return std::nullopt;
}

/// Map one completed algorithm to the physical generation producer that may attest it.
[[nodiscard]] constexpr minimizer_producer_backend
minimizer_producer_backend_for_effective_algorithm(
    ::bioseq::simd::minimizer_algorithm algorithm) noexcept {
    const auto backend = minimizer_sketch_backend_for_effective_algorithm(algorithm);
    return backend ? minimizer_producer_backend_identity_for_backend(*backend).id
                   : minimizer_producer_backend::invalid;
}

/// Validate the exact kernel slot required by one generated effective identity.
///
/// `exact_simd` is the persisted producer family shared by the generated
/// minimap2-reference owner and the continuation-based two-stack owner.  The
/// effective algorithm, never the requested name alone, selects the concrete
/// table slot. Scalar and Vulkan owners do not consume a Highway table.
[[nodiscard]] constexpr bool minimizer_generation_kernel_binding_is_valid(
    minimizer_sketch_backend backend,
    const minimizer_generation_algorithm_identity& identity,
    const ::bioseq::minimizers::minimizer_kernels* kernels) noexcept {
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    switch (backend) {
    case minimizer_sketch_backend::scalar:
        return identity.effective_algorithm == algorithm_type::scalar;
    case minimizer_sketch_backend::exact_simd:
        if (kernels == nullptr) {
            return false;
        }
        if (identity.effective_algorithm == algorithm_type::simd_minimap2_reference) {
            return kernels->minimap2_reference != nullptr;
        }
        if (identity.effective_algorithm == algorithm_type::two_stack_nthash) {
            return kernels->two_stack_nthash != nullptr;
        }
        return false;
    case minimizer_sketch_backend::bounded_nthash_simd:
        return identity.effective_algorithm == algorithm_type::bounded_nthash_simd &&
               kernels != nullptr && kernels->bounded_nthash != nullptr;
    case minimizer_sketch_backend::vulkan_segment:
        return identity.effective_algorithm == algorithm_type::vulkan_minimap_integer;
    }
    return false;
}

/// Convert one generated selector result into the complete persisted identity.
[[nodiscard]] constexpr std::expected<minimizer_generation_algorithm_identity,
                                      minimizer_generation_encoding_error>
make_minimizer_generation_algorithm_identity(
    const ::bioseq::minimizers::minimizer_physical_layout_selection& selection) noexcept {
    using namespace ::bioseq::minimizers;
    const auto* record = find_minimizer_physical_record(selection.record_id);
    const auto* binding = find_minimizer_runtime_profile(selection.requested_algorithm,
                                                         selection.alphabet_id,
                                                         selection.runtime_kmer_size,
                                                         selection.runtime_window_size,
                                                         selection.record_position_log2,
                                                         selection.packed_storage_bits);
    using family_type = minimizer_physical_layout_family;
    using algorithm_type = ::bioseq::simd::minimizer_algorithm;
    const family_type expected_family =
        selection.requested_algorithm == algorithm_type::bounded_nthash_simd ||
                selection.requested_algorithm == algorithm_type::two_stack_nthash
            ? family_type::fixed_u32_hash_legacy_k15
            : selection.effective_algorithm == algorithm_type::scalar
                ? family_type::minimap2_scalar_reference_exact_k
                : family_type::minimap2_reference_exact_k;
    if (record == nullptr || binding == nullptr || selection.family != expected_family ||
        record->record_id != selection.record_id ||
        record->position_log2 != selection.record_position_log2 ||
        record->physical_kmer_size != selection.physical_kmer_size ||
        binding->requested_algorithm != selection.requested_algorithm ||
        binding->effective_algorithm != selection.effective_algorithm ||
        binding->alphabet_id != selection.alphabet_id ||
        !minimizer_runtime_profile_accepts_shape(
            *binding, selection.runtime_kmer_size,
            selection.runtime_window_size) ||
        binding->packed_storage_bits != selection.packed_storage_bits ||
        binding->record_id != selection.record_id || binding->hash_width != selection.hash_width) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return minimizer_generation_algorithm_identity{
        .requested_algorithm = selection.requested_algorithm,
        .effective_algorithm = selection.effective_algorithm,
        .physical_record_id = selection.record_id,
        .highway_owner_id = binding->highway_owner_id,
        .alphabet_id = selection.alphabet_id,
        .record_position_log2 = selection.record_position_log2,
        .physical_kmer_size = selection.physical_kmer_size,
        .hash_width = selection.hash_width,
        .packed_storage_bits = selection.packed_storage_bits,
        .runtime_kmer_size = selection.runtime_kmer_size,
        .runtime_window_size = selection.runtime_window_size,
        .payload_layout_abi_revision = k_grouped_soa_payload_layout_abi_revision,
    };
}

/// Validate all generated-registry and producer axes without deriving from names.
[[nodiscard]] constexpr bool is_valid_minimizer_generation_algorithm_identity(
    const minimizer_generation_algorithm_identity& identity,
    minimizer_producer_identity producer) noexcept {
    using namespace ::bioseq::minimizers;
    const auto* record = find_minimizer_physical_record(identity.physical_record_id);
    const auto* binding = find_minimizer_runtime_profile(identity.requested_algorithm,
                                                         identity.alphabet_id,
                                                         identity.runtime_kmer_size,
                                                         identity.runtime_window_size,
                                                         identity.record_position_log2,
                                                         identity.packed_storage_bits);
    return is_supported_minimizer_producer_backend_identity(producer) && record != nullptr &&
           binding != nullptr && identity.payload_layout_abi_revision ==
                                     k_grouped_soa_payload_layout_abi_revision &&
           identity.packed_storage_bits != 0u &&
           identity.packed_storage_bits % 8u == 0u &&
           record->record_id == identity.physical_record_id &&
           record->position_log2 == identity.record_position_log2 &&
           record->physical_kmer_size == identity.physical_kmer_size &&
           binding->requested_algorithm == identity.requested_algorithm &&
           binding->effective_algorithm == identity.effective_algorithm &&
           binding->alphabet_id == identity.alphabet_id &&
           minimizer_runtime_profile_accepts_shape(
               *binding, identity.runtime_kmer_size,
               identity.runtime_window_size) &&
           binding->packed_storage_bits == identity.packed_storage_bits &&
           binding->record_id == identity.physical_record_id &&
           binding->hash_width == identity.hash_width &&
           binding->highway_owner_id == identity.highway_owner_id &&
           minimizer_producer_backend_for_effective_algorithm(identity.effective_algorithm) ==
               producer.id;
}

/** Select one generated physical layout against its actual input width and position extent. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::expected<::bioseq::minimizers::minimizer_physical_layout_selection,
                                   minimizer_generation_encoding_error>
select_minimizer_generation_physical_layout(
    ::bioseq::simd::minimizer_algorithm requested_algorithm,
    std::uint64_t maximum_position_extent_bp,
    std::uint8_t packed_storage_bits,
    std::uint16_t runtime_kmer_size,
    std::uint16_t runtime_window_size) noexcept {
    try {
        if (packed_storage_bits == 32u) {
            return ::bioseq::minimizers::select_minimizer_physical_layout<p_alphabet,
                                                                          std::uint32_t>(
                requested_algorithm,
                maximum_position_extent_bp,
                runtime_kmer_size,
                runtime_window_size);
        }
        if (packed_storage_bits == 64u) {
            return ::bioseq::minimizers::select_minimizer_physical_layout<p_alphabet,
                                                                          std::uint64_t>(
                requested_algorithm,
                maximum_position_extent_bp,
                runtime_kmer_size,
                runtime_window_size);
        }
    } catch (const std::exception&) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
}

/** Select and validate one producer-specific identity against its actual position extent. */
template <sequence_alphabet p_alphabet>
[[nodiscard]] inline std::expected<minimizer_generation_algorithm_identity,
                                   minimizer_generation_encoding_error>
select_minimizer_generation_algorithm_identity(
    ::bioseq::simd::minimizer_algorithm requested_algorithm,
    std::uint64_t maximum_position_extent_bp,
    std::uint8_t packed_storage_bits,
    std::uint16_t runtime_kmer_size,
    std::uint16_t runtime_window_size) noexcept {
    const auto selection = select_minimizer_generation_physical_layout<p_alphabet>(
        requested_algorithm,
        maximum_position_extent_bp,
        packed_storage_bits,
        runtime_kmer_size,
        runtime_window_size);
    if (!selection) {
        return std::unexpected(selection.error());
    }
    return make_minimizer_generation_algorithm_identity(*selection);
}

/** Select through the generated registry from a persisted runtime alphabet/width pair. */
[[nodiscard]] inline std::expected<minimizer_generation_algorithm_identity,
                                   minimizer_generation_encoding_error>
select_minimizer_generation_algorithm_identity_runtime(
    ::bioseq::simd::minimizer_algorithm requested_algorithm,
    std::uint64_t maximum_position_extent_bp,
    std::uint16_t alphabet_id,
    std::uint8_t packed_storage_bits,
    std::uint16_t runtime_kmer_size,
    std::uint16_t runtime_window_size) noexcept {
    try {
        const auto selection =
            ::bioseq::minimizers::select_minimizer_physical_layout(
                requested_algorithm, maximum_position_extent_bp,
                runtime_kmer_size, runtime_window_size, alphabet_id,
                packed_storage_bits);
        return make_minimizer_generation_algorithm_identity(selection);
    } catch (const std::exception&) {
        return std::unexpected(
            minimizer_generation_encoding_error::storage_contract_mismatch);
    }
}

/** Select one generated identity for the algorithm actually owned by a sketch backend. */
[[nodiscard]] inline std::expected<minimizer_generation_algorithm_identity,
                                   minimizer_generation_encoding_error>
select_minimizer_generation_algorithm_identity_for_backend(
    minimizer_sketch_backend backend,
    std::uint64_t maximum_position_extent_bp,
    std::uint16_t alphabet_id,
    std::uint8_t packed_storage_bits,
    std::uint16_t runtime_kmer_size,
    std::uint16_t runtime_window_size) noexcept {
    const auto algorithm = minimizer_algorithm_for_explicit_sketch_backend(backend);
    if (!algorithm) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    auto identity = select_minimizer_generation_algorithm_identity_runtime(
        *algorithm,
        maximum_position_extent_bp,
        alphabet_id,
        packed_storage_bits,
        runtime_kmer_size,
        runtime_window_size);
    const minimizer_producer_identity producer =
        minimizer_producer_backend_identity_for_backend(backend);
    if (!identity || !is_valid_minimizer_generation_algorithm_identity(*identity, producer)) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    return identity;
}

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
    if (!::bioseq::minimizers::minimizer_profile_has_alphabet_id(alphabet_code) ||
        kmer_size == 0u) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (scheme == k_bounded_nthash_u32_r7_sum_scheme_identity) {
        if (kmer_size > 31u) {
            return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
        }
        return minimizer_scalar_width::u32;
    }
    if (scheme != k_minimap_scheme_identity || kmer_size > 31u) {
        return std::unexpected(minimizer_generation_encoding_error::storage_contract_mismatch);
    }
    if (kmer_size <= 8u)
        return minimizer_scalar_width::u16;
    if (kmer_size <= 16u)
        return minimizer_scalar_width::u32;
    return minimizer_scalar_width::u64;
}

/// Derive the one admitted grouped-SoA profile from scheme and shape constraints.
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

/// Derive the grouped-SoA profile encoded by a generation manifest.
[[nodiscard]] constexpr std::expected<grouped_soa_minimizer_profile,
                                      minimizer_generation_encoding_error>
derive_grouped_soa_minimizer_profile(const minimizer_generation_manifest& manifest) noexcept;

/// Return the exact manifest byte count for a supported layout.
[[nodiscard]] constexpr std::size_t
minimizer_manifest_encoded_bytes(minimizer_generation_layout layout) noexcept {
    return layout == minimizer_generation_layout::grouped_soa_segment_local ? 176u : 0u;
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
    // Multiply two control-plane dimensions without unsigned wraparound.
    const auto checked_mul = [](std::uint64_t lhs,
                                std::uint64_t rhs) -> std::optional<std::uint64_t> {
        if (rhs != 0 && lhs > (std::numeric_limits<std::uint64_t>::max)() / rhs) {
            return std::nullopt;
        }
        return lhs * rhs;
    };
    // Align one array boundary without unsigned wraparound.
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
    static constexpr std::size_t encoded_bytes = 176u;
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
    minimizer_generation_algorithm_identity algorithm_identity{};
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
    minimizer_generation_algorithm_identity algorithm_identity{};
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
    const minimizer_generation_algorithm_identity& algorithm_identity,
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
    if (!is_valid_minimizer_generation_algorithm_identity(algorithm_identity, producer_backend) ||
        algorithm_identity.alphabet_id != alphabet_code ||
        algorithm_identity.packed_storage_bits != storage_unit_byte_width * 8u ||
        algorithm_identity.runtime_kmer_size != kmer_size ||
        algorithm_identity.runtime_window_size != window_size ||
        algorithm_identity.hash_width != profile->hash_width) {
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
        .algorithm_identity = algorithm_identity,
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
/// @tparam p_maximum_bytes Capacity of the fixed backing array.
template <std::size_t p_maximum_bytes> struct encoded_minimizer_fixed_record {
    std::array<std::byte, p_maximum_bytes> storage{};
    std::size_t byte_count = 0;

    /// Return a read-only pointer to the fixed storage prefix.
    [[nodiscard]] constexpr const std::byte* data() const noexcept {
        return storage.data();
    }
    /// Return a writable pointer to the fixed storage prefix.
    [[nodiscard]] constexpr std::byte* data() noexcept {
        return storage.data();
    }
    /// Return the encoded prefix length in bytes.
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return byte_count;
    }
    /// Return a read-only iterator to the first encoded byte.
    [[nodiscard]] constexpr const std::byte* begin() const noexcept {
        return storage.data();
    }
    /// Return a read-only iterator one past the encoded prefix.
    [[nodiscard]] constexpr const std::byte* end() const noexcept {
        return storage.data() + byte_count;
    }
    /// Return a writable iterator to the first encoded byte.
    [[nodiscard]] constexpr std::byte* begin() noexcept {
        return storage.data();
    }
    /// Return a writable iterator one past the encoded prefix.
    [[nodiscard]] constexpr std::byte* end() noexcept {
        return storage.data() + byte_count;
    }
    /// Access one encoded byte without bounds checking.
    [[nodiscard]] constexpr const std::byte& operator[](std::size_t index) const noexcept {
        return storage[index];
    }
    /// Access one writable encoded byte without bounds checking.
    [[nodiscard]] constexpr std::byte& operator[](std::size_t index) noexcept {
        return storage[index];
    }

    [[nodiscard]] constexpr bool operator==(const encoded_minimizer_fixed_record&) const = default;

    /// Compare the encoded prefix with an exact-width byte array.
    /// @tparam p_size Compared byte-array width.
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

    /// Compare an exact-width byte array with the encoded prefix.
    /// @tparam p_size Compared byte-array width.
    template <std::size_t p_size>
    [[nodiscard]] friend constexpr bool
    operator==(const std::array<std::byte, p_size>& lhs,
               const encoded_minimizer_fixed_record& rhs) noexcept {
        return rhs == lhs;
    }
};

/// Bounded byte container for any supported minimizer manifest record.
using encoded_minimizer_manifest =
    encoded_minimizer_fixed_record<minimizer_generation_manifest::encoded_bytes>;
/// Bounded byte container for one grouped-SoA directory entry.
using encoded_grouped_soa_bucket_directory_entry =
    encoded_minimizer_fixed_record<grouped_soa_bucket_directory_entry::encoded_bytes>;

/// Derive the grouped-SoA profile encoded by a generation manifest.
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
