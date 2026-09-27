/**
 * @file bioseq_caching/core/wire.cppm
 * @brief Cache identifiers, shared record fields and dependency serialization.
 */
module;

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

export module bioseq.cache.core:wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t k_shared_abi_revision = 1;
inline constexpr std::uint32_t k_column_format_revision = 1;
inline constexpr std::uint32_t k_coverage_descriptor_format_revision = 1;
inline constexpr std::uint32_t k_source_mapping_format_revision = 1;
inline constexpr std::uint32_t k_provider_recipe_format_revision = 1;
inline constexpr std::array<char, 8> k_column_magic{'B', 'S', 'Q', 'C', 'A', 'C', 'H', 'E'};
inline constexpr std::array<char, 8> k_column_footer_magic{'B', 'S', 'Q', 'C', 'F', 'T', 'R', '1'};

inline constexpr std::uint32_t k_maximum_generations_per_exact_digest = 8;
inline constexpr std::uint32_t k_maximum_coverage_generations_per_domain = 64;
inline constexpr std::uint32_t k_maximum_sources_per_coverage_node = 32;
inline constexpr std::uint32_t k_maximum_coverage_nodes_per_plan = 128;
inline constexpr std::uint32_t k_maximum_provider_boundary_columns = 1024;
inline constexpr std::uint32_t k_maximum_provider_recipes_per_exact_plan = 128;
inline constexpr std::uint32_t k_maximum_precise_dirty_dependencies_per_backend = 256;
inline constexpr std::uint32_t k_maximum_dependencies_per_generation = 1024;
inline constexpr std::uint64_t k_maximum_canonical_key_bytes = 1ULL << 20;
inline constexpr std::uint64_t k_maximum_coverage_domain_key_bytes = 1ULL << 20;
inline constexpr std::uint64_t k_maximum_provider_recipe_bytes = 1ULL << 20;
inline constexpr std::uint64_t k_maximum_display_sql_bytes = 1ULL << 20;
inline constexpr std::uint64_t k_maximum_plan_text_bytes = 4ULL << 20;
inline constexpr std::uint32_t k_default_row_group_rows = 4096;
inline constexpr std::uint64_t k_default_row_group_value_bytes = 8ULL << 20;
inline constexpr std::uint64_t k_cache_memory_fraction_numerator = 1;
inline constexpr std::uint64_t k_cache_memory_fraction_denominator = 4;
inline constexpr std::uint64_t k_maximum_file_cache_bytes = 128ULL << 30;
inline constexpr std::uint64_t k_minimum_file_free_bytes = 10ULL << 30;

/** Digest used to find candidates. Compare their canonical bytes to rule out collisions. */
struct cache_digest_256 {
    std::array<std::uint8_t, 32> bytes{};

    [[nodiscard]] bool operator==(const cache_digest_256&) const noexcept = default;
};

/** Identifies a cache generation across slot reuse and server restarts. */
struct cache_entry_id {
    cache_digest_256 digest{};
    std::uint64_t boot_nonce = 0;
    std::uint64_t generation = 0;
    std::uint64_t claim_sequence = 0;

    [[nodiscard]] bool operator==(const cache_entry_id&) const noexcept = default;
};

enum class cache_entry_kind : std::uint8_t {
    exact_final_result = 1,
    reusable_coverage = 2,
};

enum class cache_entry_state : std::uint32_t {
    absent = 0,
    claimed = 1,
    building = 2,
    complete = 3,
    failed = 4,
    invalidated = 5,
    evicting = 6,
};

enum class cache_failure_code : std::uint32_t {
    none = 0,
    producer_error,
    producer_cancelled,
    producer_exited,
    dependency_changed,
    snapshot_mismatch,
    admission_denied,
    storage_failure,
    corrupt_result,
    unsupported_type,
    runtime_semantic_digest_unavailable,
};

enum class cache_execution_mode : std::uint32_t {
    bypass = 0,
    exact_read = 1,
    exact_produce = 2,
    reuse_merge = 3,
    explain_only = 4,
};

/** Controls whether a diagnostic row includes SQL and plan text. */
enum class cache_diagnostic_disclosure_level : std::uint8_t {
    summary_only = 0,
    full_text = 1,
};

/**
 * Allow SQL and plan text for the producing role, superusers and the policy
 * monitoring role. Other callers receive summary fields only.
 */
[[nodiscard]] constexpr cache_diagnostic_disclosure_level
cache_diagnostic_disclosure(bool caller_is_superuser,
                            bool caller_is_producing_role,
                            bool caller_is_policy_monitor) noexcept {
    return caller_is_superuser || caller_is_producing_role || caller_is_policy_monitor
               ? cache_diagnostic_disclosure_level::full_text
               : cache_diagnostic_disclosure_level::summary_only;
}

enum class dependency_kind : std::uint16_t {
    relation_content = 1,
    relation_topology = 2,
    procedure = 3,
    operator_object = 4,
    type = 5,
    cast = 6,
    collation = 7,
    policy = 8,
    role = 9,
    provider = 10,
    syscache_hash = 11,
    database_write_epoch = 12,
    database_definition_epoch = 13,
};

struct dependency_key {
    std::uint32_t database_oid = 0;
    std::uint32_t class_oid = 0;
    std::uint32_t object_oid = 0;
    std::int32_t object_sub_id = 0;
    dependency_kind kind = dependency_kind::relation_content;
    std::uint16_t reserved = 0;

    [[nodiscard]] bool operator==(const dependency_key&) const noexcept = default;
};

enum class physical_identity_kind : std::uint32_t {
    none = 0,
    pg_rel_file_locator = 1,
    provider_opaque = 2,
};

struct physical_identity {
    physical_identity_kind kind = physical_identity_kind::none;
    std::uint32_t length = 0;
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] bool operator==(const physical_identity&) const noexcept = default;
};

enum class write_observability : std::uint32_t {
    none = 0,
    provider_complete = 1,
    executor_utility_only = 2,
    database_wide_fallback = 3,
};

struct dependency_version {
    dependency_key key{};
    std::uint32_t alignment_reserved = 0;
    std::uint64_t committed_epoch = 0;
    bioseq::cache::physical_identity physical_identity{};
    write_observability observability = write_observability::database_wide_fallback;
    std::uint32_t reserved = 0;

    [[nodiscard]] bool operator==(const dependency_version&) const noexcept = default;
};

inline constexpr std::uint32_t k_dependency_vector_magic = 0x42445631U;
inline constexpr std::uint32_t k_dependency_vector_abi_revision = 1;

/** Dependency version plus a generation counter that detects directory record reuse. */
struct dependency_vector_entry {
    dependency_version version{};
    std::uint64_t record_generation = 0;

    [[nodiscard]] bool operator==(const dependency_vector_entry&) const noexcept = default;
};

inline constexpr std::uint32_t k_dependency_vector_header_bytes = 40;
inline constexpr std::uint32_t k_dependency_vector_entry_bytes = 88;

/** Header fields for the serialized dependency list. It contains no process pointers. */
struct dependency_vector_header {
    std::uint32_t magic = k_dependency_vector_magic;
    std::uint32_t abi_revision = k_dependency_vector_abi_revision;
    std::uint32_t header_bytes = k_dependency_vector_header_bytes;
    std::uint32_t entry_bytes = k_dependency_vector_entry_bytes;
    std::uint32_t entry_count = 0;
    std::uint32_t reserved = 0;
    std::uint64_t total_bytes = k_dependency_vector_header_bytes;
    std::uint64_t payload_checksum = 0;
};

enum class dependency_vector_envelope_error : std::uint32_t {
    none = 0,
    invalid_argument,
    invalid_size,
    invalid_magic,
    abi_mismatch,
    invalid_header,
    count_exceeded,
    checksum_mismatch,
    invalid_entry,
    non_canonical,
};

struct dependency_vector_envelope_result {
    dependency_vector_header header{};
    dependency_vector_envelope_error error = dependency_vector_envelope_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_vector_envelope_error::none;
    }
};

/** Return the serialized size, or zero if the dependency count exceeds the limit. */
[[nodiscard]] constexpr std::size_t
dependency_vector_envelope_bytes(std::size_t entry_count) noexcept {
    return entry_count <= k_maximum_dependencies_per_generation
               ? k_dependency_vector_header_bytes + entry_count * k_dependency_vector_entry_bytes
               : 0;
}

/** Checksum for detecting changes to encoded entries. It is not cryptographic. */
[[nodiscard]] constexpr std::uint64_t
dependency_vector_payload_checksum(std::span<const std::byte> payload) noexcept {
    std::uint64_t checksum = 14695981039346656037ULL;
    for (const std::byte value : payload) {
        checksum ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(value));
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

[[nodiscard]] constexpr bool dependency_key_less(const dependency_key& left,
                                                 const dependency_key& right) noexcept {
    if (left.database_oid != right.database_oid) {
        return left.database_oid < right.database_oid;
    }
    if (left.class_oid != right.class_oid) {
        return left.class_oid < right.class_oid;
    }
    if (left.object_oid != right.object_oid) {
        return left.object_oid < right.object_oid;
    }
    if (left.object_sub_id != right.object_sub_id) {
        return left.object_sub_id < right.object_sub_id;
    }
    return left.kind < right.kind;
}

[[nodiscard]] constexpr bool physical_identity_valid(const physical_identity& identity) noexcept {
    if (identity.kind < physical_identity_kind::none ||
        identity.kind > physical_identity_kind::provider_opaque ||
        identity.length > identity.bytes.size() ||
        (identity.kind == physical_identity_kind::none && identity.length != 0)) {
        return false;
    }
    for (std::size_t index = identity.length; index < identity.bytes.size(); ++index) {
        if (identity.bytes[index] != std::byte{}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool
dependency_vector_entry_valid(const dependency_vector_entry& entry) noexcept {
    const dependency_version& version = entry.version;
    const bool observability_valid =
        version.observability >= write_observability::provider_complete &&
        version.observability <= write_observability::database_wide_fallback;
    return entry.record_generation != 0 && version.key.database_oid != 0 &&
           version.key.kind >= dependency_kind::relation_content &&
           version.key.kind <= dependency_kind::database_definition_epoch &&
           version.key.reserved == 0 && version.alignment_reserved == 0 &&
           version.committed_epoch != 0 && physical_identity_valid(version.physical_identity) &&
           observability_valid && version.reserved == 0;
}

/** Check a database-wide write version used when precise invalidation is unavailable. */
[[nodiscard]] constexpr bool
database_write_guard_entry_valid(const dependency_vector_entry& entry) noexcept {
    const dependency_version& version = entry.version;
    return dependency_vector_entry_valid(entry) && version.key.class_oid == 0 &&
           version.key.object_oid == version.key.database_oid && version.key.object_sub_id == 0 &&
           version.key.kind == dependency_kind::database_write_epoch &&
           version.physical_identity.kind == physical_identity_kind::none &&
           version.physical_identity.length == 0 &&
           version.observability == write_observability::database_wide_fallback;
}

/** Check a database-wide definition version used alongside precise dependencies. */
[[nodiscard]] constexpr bool
database_definition_guard_entry_valid(const dependency_vector_entry& entry) noexcept {
    const dependency_version& version = entry.version;
    return dependency_vector_entry_valid(entry) && version.key.class_oid == 0 &&
           version.key.object_oid == version.key.database_oid && version.key.object_sub_id == 0 &&
           version.key.kind == dependency_kind::database_definition_epoch &&
           version.physical_identity.kind == physical_identity_kind::none &&
           version.physical_identity.length == 0 &&
           version.observability == write_observability::database_wide_fallback;
}

/** Validate the database-wide version record stored separately from the plan dependencies. */
[[nodiscard]] constexpr bool
database_guard_entry_valid(const dependency_vector_entry& entry) noexcept {
    return database_write_guard_entry_valid(entry) || database_definition_guard_entry_valid(entry);
}

/** A database-wide write version also requires checking transaction completions. */
[[nodiscard]] constexpr bool
database_guard_requires_completion(const dependency_key& key) noexcept {
    return key.kind == dependency_kind::database_write_epoch;
}

[[nodiscard]] constexpr bool
database_guard_requires_completion(const dependency_vector_entry& entry) noexcept {
    return database_guard_entry_valid(entry) &&
           database_guard_requires_completion(entry.version.key);
}

/**
 * Check that the dependency sample still belongs to the bound PostgreSQL snapshot.
 * The binding counter must be nonzero and match the current counter. A copied
 * snapshot may have a cleared counter. If that counter or the optional ongoing
 * check is present, it must agree too.
 */
[[nodiscard]] constexpr bool
dependency_completion_handshake_valid(std::uint64_t copied_snapshot_completion_count,
                                      std::uint64_t snapshot_binding_completion_count,
                                      std::uint64_t current_completion_count,
                                      std::uint64_t continuing_completion_count) noexcept {
    return snapshot_binding_completion_count != 0 &&
           current_completion_count == snapshot_binding_completion_count &&
           (copied_snapshot_completion_count == 0 ||
            copied_snapshot_completion_count == snapshot_binding_completion_count) &&
           (continuing_completion_count == 0 ||
            continuing_completion_count == current_completion_count);
}

namespace dependency_vector_wire_detail {

constexpr void
write_u16(std::span<std::byte> output, std::size_t offset, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

constexpr void
write_u32(std::span<std::byte> output, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

constexpr void
write_u64(std::span<std::byte> output, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        output[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

[[nodiscard]] constexpr std::uint16_t read_u16(std::span<const std::byte> input,
                                               std::size_t offset) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr std::uint32_t read_u32(std::span<const std::byte> input,
                                               std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

[[nodiscard]] constexpr std::uint64_t read_u64(std::span<const std::byte> input,
                                               std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

constexpr void encode_header(std::span<std::byte> output,
                             const dependency_vector_header& header) noexcept {
    write_u32(output, 0, header.magic);
    write_u32(output, 4, header.abi_revision);
    write_u32(output, 8, header.header_bytes);
    write_u32(output, 12, header.entry_bytes);
    write_u32(output, 16, header.entry_count);
    write_u32(output, 20, header.reserved);
    write_u64(output, 24, header.total_bytes);
    write_u64(output, 32, header.payload_checksum);
}

[[nodiscard]] constexpr dependency_vector_header
decode_header(std::span<const std::byte> input) noexcept {
    return dependency_vector_header{
        .magic = read_u32(input, 0),
        .abi_revision = read_u32(input, 4),
        .header_bytes = read_u32(input, 8),
        .entry_bytes = read_u32(input, 12),
        .entry_count = read_u32(input, 16),
        .reserved = read_u32(input, 20),
        .total_bytes = read_u64(input, 24),
        .payload_checksum = read_u64(input, 32),
    };
}

constexpr void encode_entry(std::span<std::byte> output,
                            const dependency_vector_entry& entry) noexcept {
    const dependency_version& version = entry.version;
    write_u32(output, 0, version.key.database_oid);
    write_u32(output, 4, version.key.class_oid);
    write_u32(output, 8, version.key.object_oid);
    write_u32(output, 12, std::bit_cast<std::uint32_t>(version.key.object_sub_id));
    write_u16(output, 16, static_cast<std::uint16_t>(version.key.kind));
    write_u16(output, 18, version.key.reserved);
    write_u32(output, 20, version.alignment_reserved);
    write_u64(output, 24, version.committed_epoch);
    write_u32(output, 32, static_cast<std::uint32_t>(version.physical_identity.kind));
    write_u32(output, 36, version.physical_identity.length);
    for (std::size_t index = 0; index < version.physical_identity.bytes.size(); ++index) {
        output[40 + index] = version.physical_identity.bytes[index];
    }
    write_u32(output, 72, static_cast<std::uint32_t>(version.observability));
    write_u32(output, 76, version.reserved);
    write_u64(output, 80, entry.record_generation);
}

[[nodiscard]] constexpr dependency_vector_entry
decode_entry(std::span<const std::byte> input) noexcept {
    dependency_vector_entry entry{};
    entry.version.key.database_oid = read_u32(input, 0);
    entry.version.key.class_oid = read_u32(input, 4);
    entry.version.key.object_oid = read_u32(input, 8);
    entry.version.key.object_sub_id = std::bit_cast<std::int32_t>(read_u32(input, 12));
    entry.version.key.kind = static_cast<dependency_kind>(read_u16(input, 16));
    entry.version.key.reserved = read_u16(input, 18);
    entry.version.alignment_reserved = read_u32(input, 20);
    entry.version.committed_epoch = read_u64(input, 24);
    entry.version.physical_identity.kind = static_cast<physical_identity_kind>(read_u32(input, 32));
    entry.version.physical_identity.length = read_u32(input, 36);
    for (std::size_t index = 0; index < entry.version.physical_identity.bytes.size(); ++index) {
        entry.version.physical_identity.bytes[index] = input[40 + index];
    }
    entry.version.observability = static_cast<write_observability>(read_u32(input, 72));
    entry.version.reserved = read_u32(input, 76);
    entry.record_generation = read_u64(input, 80);
    return entry;
}

} // namespace dependency_vector_wire_detail

/** Write sorted, distinct dependencies into an exactly sized buffer, with a header and checksum. */
[[nodiscard]] inline dependency_vector_envelope_result
initialize_dependency_vector_envelope(std::span<std::byte> envelope,
                                      std::span<const dependency_vector_entry> entries) noexcept {
    dependency_vector_envelope_result result{};
    const std::size_t required = dependency_vector_envelope_bytes(entries.size());
    if (required == 0) {
        result.error = dependency_vector_envelope_error::count_exceeded;
        return result;
    }
    if (envelope.data() == nullptr || envelope.size() != required) {
        result.error = dependency_vector_envelope_error::invalid_size;
        return result;
    }
    std::fill(envelope.begin(), envelope.end(), std::byte{});
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (!dependency_vector_entry_valid(entries[index])) {
            result.error = dependency_vector_envelope_error::invalid_entry;
            return result;
        }
        if (index != 0 &&
            !dependency_key_less(entries[index - 1].version.key, entries[index].version.key)) {
            result.error = dependency_vector_envelope_error::non_canonical;
            return result;
        }
    }
    const std::size_t payload_bytes = entries.size() * k_dependency_vector_entry_bytes;
    std::span<std::byte> payload =
        envelope.subspan(k_dependency_vector_header_bytes, payload_bytes);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        dependency_vector_wire_detail::encode_entry(
            payload.subspan(index * k_dependency_vector_entry_bytes,
                            k_dependency_vector_entry_bytes),
            entries[index]);
    }
    result.header = dependency_vector_header{
        .entry_count = static_cast<std::uint32_t>(entries.size()),
        .total_bytes = static_cast<std::uint64_t>(required),
        .payload_checksum = dependency_vector_payload_checksum(payload),
    };
    dependency_vector_wire_detail::encode_header(envelope.first(k_dependency_vector_header_bytes),
                                                 result.header);
    return result;
}

/** Check the format and checksum, then validate every entry and its order. */
[[nodiscard]] inline dependency_vector_envelope_result
validate_dependency_vector_envelope(std::span<const std::byte> envelope) noexcept {
    dependency_vector_envelope_result result{};
    if (envelope.data() == nullptr || envelope.size() < k_dependency_vector_header_bytes) {
        result.error = dependency_vector_envelope_error::invalid_argument;
        return result;
    }
    result.header = dependency_vector_wire_detail::decode_header(
        envelope.first(k_dependency_vector_header_bytes));
    if (result.header.magic != k_dependency_vector_magic) {
        result.error = dependency_vector_envelope_error::invalid_magic;
        return result;
    }
    if (result.header.abi_revision != k_dependency_vector_abi_revision) {
        result.error = dependency_vector_envelope_error::abi_mismatch;
        return result;
    }
    if (result.header.header_bytes != k_dependency_vector_header_bytes ||
        result.header.entry_bytes != k_dependency_vector_entry_bytes ||
        result.header.reserved != 0) {
        result.error = dependency_vector_envelope_error::invalid_header;
        return result;
    }
    if (result.header.entry_count > k_maximum_dependencies_per_generation) {
        result.error = dependency_vector_envelope_error::count_exceeded;
        return result;
    }
    const std::size_t required = dependency_vector_envelope_bytes(result.header.entry_count);
    if (required == 0 || result.header.total_bytes != required || envelope.size() != required) {
        result.error = dependency_vector_envelope_error::invalid_size;
        return result;
    }
    const std::span<const std::byte> payload = envelope.subspan(k_dependency_vector_header_bytes);
    if (dependency_vector_payload_checksum(payload) != result.header.payload_checksum) {
        result.error = dependency_vector_envelope_error::checksum_mismatch;
        return result;
    }
    dependency_key previous{};
    bool have_previous = false;
    for (std::uint32_t index = 0; index < result.header.entry_count; ++index) {
        const dependency_vector_entry entry = dependency_vector_wire_detail::decode_entry(
            payload.subspan(static_cast<std::size_t>(index) * k_dependency_vector_entry_bytes,
                            k_dependency_vector_entry_bytes));
        if (!dependency_vector_entry_valid(entry)) {
            result.error = dependency_vector_envelope_error::invalid_entry;
            return result;
        }
        if (have_previous && !dependency_key_less(previous, entry.version.key)) {
            result.error = dependency_vector_envelope_error::non_canonical;
            return result;
        }
        previous = entry.version.key;
        have_previous = true;
    }
    return result;
}

/** Validate the encoded list and copy its entries into an exactly sized output buffer. */
[[nodiscard]] inline dependency_vector_envelope_result
copy_dependency_vector_entries(std::span<const std::byte> envelope,
                               std::span<dependency_vector_entry> output) noexcept {
    dependency_vector_envelope_result result = validate_dependency_vector_envelope(envelope);
    if (!result) {
        return result;
    }
    if (output.size() != result.header.entry_count) {
        result.error = dependency_vector_envelope_error::invalid_size;
        return result;
    }
    const std::span<const std::byte> payload = envelope.subspan(k_dependency_vector_header_bytes);
    for (std::uint32_t index = 0; index < result.header.entry_count; ++index) {
        output[index] = dependency_vector_wire_detail::decode_entry(
            payload.subspan(static_cast<std::size_t>(index) * k_dependency_vector_entry_bytes,
                            k_dependency_vector_entry_bytes));
    }
    return result;
}

/**
 * Return the bytes needed to hash the plan dependencies and optional database version.
 * Completion counters check snapshot timing and are excluded from this hash.
 * The fingerprint describes committed dependency versions.
 */
[[nodiscard]] constexpr std::size_t
dependency_fingerprint_material_bytes(std::size_t plan_dependency_count,
                                      bool has_database_guard) noexcept {
    const std::size_t plan_bytes = dependency_vector_envelope_bytes(plan_dependency_count);
    return plan_bytes == 0
               ? 0
               : plan_bytes + (has_database_guard ? k_dependency_vector_entry_bytes : 0U);
}

/**
 * Copy the validated dependency bytes and optional database version for hashing.
 * Explicit field encoding keeps compiler padding out of the fingerprint.
 */
[[nodiscard]] inline dependency_vector_envelope_result initialize_dependency_fingerprint_material(
    std::span<std::byte> material,
    std::span<const std::byte> canonical_plan_envelope,
    std::span<const dependency_vector_entry> database_guard) noexcept {
    dependency_vector_envelope_result result =
        validate_dependency_vector_envelope(canonical_plan_envelope);
    if (!result) {
        return result;
    }
    if (database_guard.size() > 1) {
        result.error = dependency_vector_envelope_error::invalid_argument;
        return result;
    }
    if (!database_guard.empty() && !database_guard_entry_valid(database_guard.front())) {
        result.error = dependency_vector_envelope_error::invalid_entry;
        return result;
    }
    const std::size_t required =
        dependency_fingerprint_material_bytes(result.header.entry_count, !database_guard.empty());
    if (required == 0 || material.data() == nullptr || material.size() != required) {
        result.error = dependency_vector_envelope_error::invalid_size;
        return result;
    }
    std::copy(canonical_plan_envelope.begin(), canonical_plan_envelope.end(), material.begin());
    if (!database_guard.empty()) {
        dependency_vector_wire_detail::encode_entry(
            material.subspan(canonical_plan_envelope.size(), k_dependency_vector_entry_bytes),
            database_guard.front());
    }
    return result;
}

enum class directory_slot_state : std::uint32_t {
    empty = 0,
    occupied = 1,
    tombstone = 2,
    retiring = 3,
};

enum class bootstrap_state : std::uint32_t {
    uninitialized = 0,
    initializing = 1,
    ready = 2,
    failed = 3,
};

enum class transition_owner_state : std::uint32_t {
    free = 0,
    collecting = 1,
    transitioning = 2,
    active = 3,
    repairing = 4,
};

enum class cache_correctness_readiness : std::uint32_t {
    passthrough_only = 0,
    relation_free_only = 1,
    generic_mvcc_ready = 2,
    provider_complete_ready = 3,
    reusable_coverage_ready = 4,
};

enum class data_endian : std::uint8_t {
    little = 1,
    big = 2,
};

enum class column_encoding : std::uint16_t {
    boolean_bits = 1,
    fixed_by_value = 2,
    fixed_by_reference = 3,
    flat_varlena = 4,
    binary_send = 5,
    dictionary = 6,
    run_length = 7,
};

enum class block_backing_tier : std::uint16_t {
    dsa = 1,
    file = 2,
};

struct semantic_id_128 {
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] bool operator==(const semantic_id_128&) const noexcept = default;
};

struct column_store_header {
    std::array<char, 8> magic{};
    std::uint32_t format_revision = 0;
    std::uint32_t postgres_major = 0;
    std::uint32_t shared_abi_revision = 0;
    std::uint32_t flags = 0;
    std::uint64_t boot_nonce = 0;
    std::uint32_t database_oid = 0;
    std::uint32_t owner_role_oid = 0;
    std::uint8_t endian = 0;
    std::uint8_t datum_width = 0;
    std::uint16_t column_count = 0;
    std::uint64_t row_count = 0;
    std::uint64_t row_group_count = 0;
    std::uint64_t column_descriptor_offset = 0;
    std::uint64_t row_group_index_offset = 0;
    cache_digest_256 schema_digest{};
    std::uint32_t header_crc32c = 0;
};

struct column_descriptor {
    std::uint32_t type_oid = 0;
    std::int32_t typmod = -1;
    std::uint32_t collation_oid = 0;
    std::int16_t type_length = 0;
    std::uint8_t by_value = 0;
    std::uint8_t type_alignment = 1;
    column_encoding encoding = column_encoding::binary_send;
    std::uint16_t flags = 0;
    std::uint32_t send_function_oid = 0;
    std::uint32_t receive_function_oid = 0;
    std::uint32_t receive_type_io_param = 0;
    std::uint32_t reserved = 0;
    std::uint64_t name_offset = 0;

    [[nodiscard]] bool operator==(const column_descriptor&) const noexcept = default;
};

struct column_schema_wire {
    std::uint32_t attno = 0;
    semantic_id_128 semantic_id{};
    std::uint32_t type_oid = 0;
    std::int32_t typmod = -1;
    std::uint32_t collation_oid = 0;
    std::uint32_t flags = 0;

    [[nodiscard]] bool operator==(const column_schema_wire&) const noexcept = default;
};

struct row_group_header {
    std::uint64_t first_row_ordinal = 0;
    std::uint32_t row_count = 0;
    std::uint16_t column_count = 0;
    std::uint16_t flags = 0;
    std::uint64_t column_chunk_table_offset = 0;
    std::uint64_t byte_length = 0;
    std::uint32_t group_crc32c = 0;
    std::uint32_t reserved = 0;
};

struct column_chunk {
    std::uint64_t null_bitmap_offset = 0;
    std::uint64_t null_bitmap_bytes = 0;
    std::uint64_t offsets_offset = 0;
    std::uint64_t offsets_bytes = 0;
    std::uint64_t values_offset = 0;
    std::uint64_t values_bytes = 0;
    std::uint64_t logical_values_bytes = 0;
    std::uint32_t crc32c = 0;
    std::uint32_t flags = 0;
};

struct block_reference {
    block_backing_tier tier = block_backing_tier::dsa;
    std::uint16_t flags = 0;
    std::uint32_t reserved = 0;
    std::uint64_t handle_or_file_id = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint32_t crc32c = 0;
    std::uint32_t reserved2 = 0;
};

enum class row_identity_kind : std::uint16_t {
    heap_tid = 1,
    bioseq_row_number = 2,
    provider_opaque = 3,
};

struct base_row_reference {
    std::uint32_t rte_ordinal = 0;
    std::uint32_t logical_rel_oid = 0;
    std::uint32_t leaf_rel_oid = 0;
    bioseq::cache::physical_identity physical_identity{};
    row_identity_kind identity_kind = row_identity_kind::heap_tid;
    std::uint16_t flags = 0;
    std::uint32_t block_number = 0;
    std::uint16_t offset_number = 0;
    std::uint16_t reserved = 0;
    std::uint64_t row_number_or_opaque = 0;
    std::uint64_t xmin_identity = 0;
};

struct result_occurrence {
    std::uint64_t result_ordinal = 0;
    std::uint64_t provider_occurrence_hi = 0;
    std::uint64_t provider_occurrence_lo = 0;
    std::uint32_t base_reference_count = 0;
    std::uint32_t capability_mask = 0;
    std::uint64_t base_reference_offset = 0;
};

enum bioseq_match_capability : std::uint64_t {
    bioseq_cap_score = 1ULL << 0,
    bioseq_cap_dp_score = 1ULL << 1,
    bioseq_cap_query_coords = 1ULL << 2,
    bioseq_cap_target_coords = 1ULL << 3,
    bioseq_cap_counts = 1ULL << 4,
    bioseq_cap_mapq = 1ULL << 5,
    bioseq_cap_divergence = 1ULL << 6,
    bioseq_cap_cigar = 1ULL << 7,
};

static_assert(std::is_trivially_copyable_v<cache_digest_256>);
static_assert(std::is_trivially_copyable_v<cache_entry_id>);
static_assert(std::is_trivially_copyable_v<dependency_key>);
static_assert(std::is_trivially_copyable_v<dependency_version>);
static_assert(std::is_trivially_copyable_v<dependency_vector_entry>);
static_assert(std::is_standard_layout_v<dependency_vector_entry>);
static_assert(sizeof(dependency_vector_entry) == k_dependency_vector_entry_bytes);
static_assert(std::is_trivially_copyable_v<dependency_vector_header>);
static_assert(std::is_standard_layout_v<dependency_vector_header>);
static_assert(sizeof(dependency_vector_header) == k_dependency_vector_header_bytes);
static_assert(std::is_trivially_copyable_v<column_store_header>);
static_assert(std::is_trivially_copyable_v<row_group_header>);
static_assert(std::is_trivially_copyable_v<column_chunk>);
static_assert(std::is_trivially_copyable_v<block_reference>);

} // namespace bioseq::cache
