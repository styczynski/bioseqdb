/**
 * @file bioseq_caching/core/reusable_planning.cppm
 * @brief Chooses and validates cached sources for query, reuse
 *
 * These helpers run without a PostgreSQL backend. They check provider
 * data  and cache keys, assign each row to one source, and validate
 * values fetched from the original rows. Explicit cleanup records keep
 * source references releasable when a PostgreSQL error skips destructors.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:reusable_planning;

import bioseq.cache.provider_api;
import :coverage;
import :provider_validation;
import :reusable_registry;
import :wire;

export namespace bioseq::cache::pg::reuse {

inline constexpr std::uint32_t k_reusable_plan_format_revision = 1;
inline constexpr std::uint8_t k_late_fetch_status_unset = 0xffU;

/** Security identity shared by planning and the cache registry. */
using coverage_security_namespace = reusable_security_namespace;

/** Selects a key for a reusable query domain or one cached generation. */
enum class coverage_canonical_key_kind : std::uint8_t {
    domain = 1,
    generation = 2,
};

/** Identifies the provider fragment and its relation occurrence within the query. */
struct coverage_fragment_binding_signature {
    std::uint32_t format_revision = k_reusable_plan_format_revision;
    std::uint32_t fragment_binding_ordinal = 0;
    std::uint32_t provider_rte_occurrence_ordinal = 0;
    std::uint32_t provider_rte_occurrence_count = 0;
    std::uint32_t logical_relation_oid = 0;
    std::uint32_t selected_relation_oid = 0;
    std::uint32_t provider_id = 0;
    std::uint32_t flags = 0;
    cache_digest_256 binding_role_digest{};

    [[nodiscard]] bool
    operator==(const coverage_fragment_binding_signature&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<coverage_fragment_binding_signature>);
static_assert(std::is_trivially_copyable_v<coverage_fragment_binding_signature>);

/** Checks the provider fragment's IDs, occurrence number and format revision. */
[[nodiscard]] constexpr bool
valid_coverage_fragment_binding(const coverage_fragment_binding_signature& binding) noexcept {
    return binding.format_revision == k_reusable_plan_format_revision &&
           binding.fragment_binding_ordinal == 0 && binding.provider_rte_occurrence_count != 0 &&
           binding.provider_rte_occurrence_ordinal < binding.provider_rte_occurrence_count &&
           binding.logical_relation_oid != 0 && binding.selected_relation_oid != 0 &&
           binding.provider_id != 0 && binding.flags == 0;
}

enum class coverage_binding_semantic_error : std::uint8_t {
    invalid_kind = 1,
    invalid_binding,
    invalid_predicate_bytes,
    output_too_small,
};

inline constexpr std::array<std::byte, 4> k_coverage_binding_semantic_magic{
    std::byte{0x46}, std::byte{0x56}, std::byte{0x54}, std::byte{0x31}};
inline constexpr std::size_t k_coverage_binding_semantic_fixed_bytes = 77;

/** Errors that prevent building a reusable cache key. */
enum class coverage_canonical_key_error : std::uint8_t {
    invalid_kind = 1,
    invalid_security_namespace,
    empty_provider_semantic_bytes,
    key_too_large,
    output_too_small,
};

inline constexpr std::array<std::byte, 4> k_coverage_canonical_key_magic{
    std::byte{0x42}, std::byte{0x43}, std::byte{0x4b}, std::byte{0x31}};
inline constexpr std::size_t k_coverage_canonical_key_fixed_bytes = 78;

namespace canonical_key_detail {

inline constexpr std::uint8_t k_kind_tag = 0x01;
inline constexpr std::uint8_t k_format_revision_tag = 0x02;
inline constexpr std::uint8_t k_database_oid_tag = 0x10;
inline constexpr std::uint8_t k_current_role_oid_tag = 0x11;
inline constexpr std::uint8_t k_session_user_oid_tag = 0x12;
inline constexpr std::uint8_t k_security_context_flags_tag = 0x13;
inline constexpr std::uint8_t k_row_security_mode_tag = 0x14;
inline constexpr std::uint8_t k_provider_security_digest_tag = 0x15;
inline constexpr std::uint8_t k_provider_semantic_bytes_tag = 0x20;

[[nodiscard]] constexpr bool valid_kind(coverage_canonical_key_kind kind) noexcept {
    return kind == coverage_canonical_key_kind::domain ||
           kind == coverage_canonical_key_kind::generation;
}

constexpr void
append_u8(std::span<std::byte> output, std::size_t& offset, std::uint8_t value) noexcept {
    output[offset++] = std::byte{value};
}

constexpr void
append_u32_le(std::span<std::byte> output, std::size_t& offset, std::uint32_t value) noexcept {
    for (std::size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
        append_u8(output, offset, static_cast<std::uint8_t>(value >> (byte_index * 8U)));
    }
}

constexpr void
append_u64_le(std::span<std::byte> output, std::size_t& offset, std::uint64_t value) noexcept {
    for (std::size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
        append_u8(output, offset, static_cast<std::uint8_t>(value >> (byte_index * 8U)));
    }
}

} // namespace canonical_key_detail

/**
 * Writes the  query-frament identity added to provider key bytes.
 * A generation key also includes the normalized predicate. Domain keys
 * leave the predicate out so results from different filters can be
 * considered for reuse.
 */
[[nodiscard]] inline std::expected<std::size_t, coverage_binding_semantic_error>
write_coverage_binding_semantics(coverage_canonical_key_kind kind,
                                 const coverage_fragment_binding_signature& binding,
                                 std::span<const std::byte> normalized_predicate,
                                 std::span<std::byte> output) noexcept {
    using namespace canonical_key_detail;
    if (!valid_kind(kind)) {
        return std::unexpected(coverage_binding_semantic_error::invalid_kind);
    }
    if (!valid_coverage_fragment_binding(binding)) {
        return std::unexpected(coverage_binding_semantic_error::invalid_binding);
    }
    const bool generation = kind == coverage_canonical_key_kind::generation;
    if (generation != !normalized_predicate.empty()) {
        return std::unexpected(coverage_binding_semantic_error::invalid_predicate_bytes);
    }
    const std::size_t predicate_frame_bytes =
        generation ? 1U + sizeof(std::uint64_t) + normalized_predicate.size() : 0U;
    if (normalized_predicate.size() > (std::numeric_limits<std::size_t>::max)() -
                                          k_coverage_binding_semantic_fixed_bytes - 1U -
                                          sizeof(std::uint64_t) ||
        output.size() < k_coverage_binding_semantic_fixed_bytes + predicate_frame_bytes) {
        return std::unexpected(coverage_binding_semantic_error::output_too_small);
    }

    std::size_t offset = 0;
    for (const std::byte value : k_coverage_binding_semantic_magic) {
        output[offset++] = value;
    }
    const auto append_tagged_u32 = [&output, &offset](std::uint8_t tag,
                                                      std::uint32_t value) constexpr {
        append_u8(output, offset, tag);
        append_u32_le(output, offset, value);
    };
    append_tagged_u32(0x01, binding.format_revision);
    append_tagged_u32(0x02, binding.fragment_binding_ordinal);
    append_tagged_u32(0x03, binding.provider_rte_occurrence_ordinal);
    append_tagged_u32(0x04, binding.provider_rte_occurrence_count);
    append_tagged_u32(0x05, binding.logical_relation_oid);
    append_tagged_u32(0x06, binding.selected_relation_oid);
    append_tagged_u32(0x07, binding.provider_id);
    append_tagged_u32(0x08, binding.flags);
    append_u8(output, offset, 0x09);
    for (const std::uint8_t value : binding.binding_role_digest.bytes) {
        append_u8(output, offset, value);
    }
    if (generation) {
        append_u8(output, offset, 0x0a);
        append_u64_le(output, offset, normalized_predicate.size());
        for (const std::byte value : normalized_predicate) {
            output[offset++] = value;
        }
    }
    return offset;
}

/** Copies the provider security digest from a previously validated path description. */
[[nodiscard]] inline cache_digest_256
provider_security_digest(const bioseq_cache_path_domain& domain) noexcept {
    cache_digest_256 result{};
    std::copy_n(domain.provider_security_digest, result.bytes.size(), result.bytes.begin());
    return result;
}

/**
 * Writes security fields and provider key bytes in a fixed tagged
 * order. Integer fields use little-endian encoding, so the key does
 * not depend on host byte order or structure  padding.
 */
[[nodiscard]] inline std::expected<std::size_t, coverage_canonical_key_error>
write_coverage_canonical_key(coverage_canonical_key_kind kind,
                             const coverage_security_namespace& security_namespace,
                             std::span<const std::byte> provider_semantic_bytes,
                             std::span<std::byte> output) noexcept {
    using namespace canonical_key_detail;
    if (!valid_kind(kind)) {
        return std::unexpected(coverage_canonical_key_error::invalid_kind);
    }
    if (!valid_reusable_security_namespace(security_namespace)) {
        return std::unexpected(coverage_canonical_key_error::invalid_security_namespace);
    }
    if (provider_semantic_bytes.empty()) {
        return std::unexpected(coverage_canonical_key_error::empty_provider_semantic_bytes);
    }
    const std::uint64_t limit = kind == coverage_canonical_key_kind::domain
                                    ? k_maximum_coverage_domain_key_bytes
                                    : k_maximum_canonical_key_bytes;
    if (provider_semantic_bytes.size() > limit - k_coverage_canonical_key_fixed_bytes) {
        return std::unexpected(coverage_canonical_key_error::key_too_large);
    }
    const std::size_t required =
        k_coverage_canonical_key_fixed_bytes + provider_semantic_bytes.size();
    if (output.size() < required) {
        return std::unexpected(coverage_canonical_key_error::output_too_small);
    }

    std::size_t offset = 0;
    for (const std::byte value : k_coverage_canonical_key_magic) {
        output[offset++] = value;
    }
    append_u8(output, offset, k_kind_tag);
    append_u8(output, offset, static_cast<std::uint8_t>(kind));
    append_u8(output, offset, k_format_revision_tag);
    append_u32_le(output, offset, k_reusable_plan_format_revision);
    append_u8(output, offset, k_database_oid_tag);
    append_u32_le(output, offset, security_namespace.database_oid);
    append_u8(output, offset, k_current_role_oid_tag);
    append_u32_le(output, offset, security_namespace.current_role_oid);
    append_u8(output, offset, k_session_user_oid_tag);
    append_u32_le(output, offset, security_namespace.session_user_oid);
    append_u8(output, offset, k_security_context_flags_tag);
    append_u32_le(output, offset, security_namespace.security_context_flags);
    append_u8(output, offset, k_row_security_mode_tag);
    append_u32_le(output, offset, static_cast<std::uint32_t>(security_namespace.row_security_mode));
    append_u8(output, offset, k_provider_security_digest_tag);
    for (const std::uint8_t value : security_namespace.provider_security_digest.bytes) {
        append_u8(output, offset, value);
    }
    append_u8(output, offset, k_provider_semantic_bytes_tag);
    append_u64_le(output, offset, provider_semantic_bytes.size());
    for (const std::byte value : provider_semantic_bytes) {
        output[offset++] = value;
    }
    return offset;
}

/** Describes the domain, security identity and provider capabilities needed from cached sources. */
struct coverage_candidate_probe {
    cache_digest_256 domain_digest{};
    std::span<const std::byte> canonical_domain_key{};
    coverage_security_namespace security_namespace{};
    std::uint32_t provider_id = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t required_capability_mask = 0;
};

/** A cached-source snapshot used during planning, with borrowed key bytes. */
struct coverage_candidate_view {
    cache_entry_id entry_id{};
    cache_digest_256 domain_digest{};
    std::span<const std::byte> canonical_domain_key{};
    std::span<const std::byte> canonical_generation_key{};
    coverage_security_namespace security_namespace{};
    std::uint32_t provider_id = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    bool complete = false;
    bool in_flight = false;
    std::uint64_t estimated_covered_rows = 0;
    std::uint64_t stored_bytes = 0;
};

/** Selected indices into the caller's candidate list, with rejection counts and truncation flags. */
struct bounded_candidate_selection {
    std::array<std::size_t, k_maximum_sources_per_coverage_node> indices{};
    std::size_t selected_count = 0;
    std::size_t visited_count = 0;
    std::size_t qualified_count = 0;
    std::size_t digest_collision_count = 0;
    std::size_t invalid_key_count = 0;
    std::size_t invalid_namespace_count = 0;
    bool bucket_truncated = false;
    bool result_truncated = false;
};

[[nodiscard]] constexpr bool bounded_canonical_key(std::span<const std::byte> bytes,
                                                   std::uint64_t limit) noexcept {
    return !bytes.empty() && bytes.size() <= limit;
}

/** Compares both the digest and full key bytes before sharing an exact result */
[[nodiscard]] inline bool
exact_current_key_equal(const cache_digest_256& requested_digest,
                        std::span<const std::byte> requested_bytes,
                        const cache_digest_256& candidate_digest,
                        std::span<const std::byte> candidate_bytes) noexcept {
    return requested_digest == candidate_digest &&
           requested_bytes.size() == candidate_bytes.size() &&
           std::equal(requested_bytes.begin(), requested_bytes.end(), candidate_bytes.begin());
}

[[nodiscard]] inline coverage_source_order_key
candidate_order_key(const coverage_candidate_view& candidate) noexcept {
    return coverage_source_order_key{
        .complete = candidate.complete,
        .estimated_covered_rows = candidate.estimated_covered_rows,
        .stored_bytes = candidate.stored_bytes,
        .claim_sequence = candidate.entry_id.claim_sequence,
        .digest = candidate.entry_id.digest,
    };
}

[[nodiscard]] inline bool candidate_precedes(const coverage_candidate_view& left,
                                             const coverage_candidate_view& right) noexcept {
    return coverage_source_precedes(candidate_order_key(left), candidate_order_key(right));
}

/**
 * Checks up to 64 cached sources and keeps the best 32 that match the
 * requested domain, security identity and, provider capabilities. Full
 * key comparisons reject hash collisions before the caller acquires
 * source references.
 */
[[nodiscard]] inline bounded_candidate_selection
select_candidates(const coverage_candidate_probe& probe,
                  std::span<const coverage_candidate_view> candidates) noexcept {
    bounded_candidate_selection result{};
    const std::size_t visit_count =
        std::min<std::size_t>(candidates.size(), k_maximum_coverage_generations_per_domain);
    result.visited_count = visit_count;
    result.bucket_truncated = candidates.size() > visit_count;

    if (probe.provider_id == 0 || probe.compatibility_family_id == 0 ||
        !valid_reusable_security_namespace(probe.security_namespace) ||
        !bounded_canonical_key(probe.canonical_domain_key, k_maximum_coverage_domain_key_bytes)) {
        return result;
    }

    for (std::size_t candidate_index = 0; candidate_index < visit_count; ++candidate_index) {
        const coverage_candidate_view& candidate = candidates[candidate_index];
        if (!valid_reusable_security_namespace(candidate.security_namespace)) {
            ++result.invalid_namespace_count;
            continue;
        }
        if (!bounded_canonical_key(candidate.canonical_domain_key,
                                   k_maximum_coverage_domain_key_bytes) ||
            !bounded_canonical_key(candidate.canonical_generation_key,
                                   k_maximum_canonical_key_bytes)) {
            ++result.invalid_key_count;
            continue;
        }
        if (candidate.security_namespace != probe.security_namespace ||
            candidate.provider_id != probe.provider_id ||
            candidate.compatibility_family_id != probe.compatibility_family_id ||
            candidate.domain_digest != probe.domain_digest) {
            continue;
        }
        if (candidate.canonical_domain_key.size() != probe.canonical_domain_key.size() ||
            !std::equal(candidate.canonical_domain_key.begin(),
                        candidate.canonical_domain_key.end(),
                        probe.canonical_domain_key.begin())) {
            ++result.digest_collision_count;
            continue;
        }
        if (!capabilities_satisfy(candidate.capability_mask, probe.required_capability_mask) ||
            (!candidate.complete && !candidate.in_flight)) {
            continue;
        }

        ++result.qualified_count;
        std::size_t position = 0;
        while (position < result.selected_count &&
               !candidate_precedes(candidate, candidates[result.indices[position]])) {
            ++position;
        }
        if (position >= result.indices.size()) {
            result.result_truncated = true;
            continue;
        }
        const std::size_t old_count = result.selected_count;
        if (result.selected_count < result.indices.size()) {
            ++result.selected_count;
        } else {
            result.result_truncated = true;
        }
        for (std::size_t move = result.selected_count - 1; move > position; --move) {
            result.indices[move] = result.indices[move - 1];
        }
        result.indices[position] = candidate_index;
        if (old_count == result.indices.size()) {
            result.result_truncated = true;
        }
    }
    return result;
}

enum class provider_contract_error : std::uint16_t {
    none = 0,
    null_pointer,
    struct_size,
    provider_mismatch,
    invalid_flags,
    missing_predicate,
    invalid_compatibility_family,
    boundary_schema,
    lineage_schema,
    decorated_path_missing,
    decorated_boundary_mismatch,
    decorated_lineage_mismatch,
    recipe_shape,
    coverage_digest_mismatch,
    security_namespace,
};

[[nodiscard]] constexpr std::expected<void, provider_contract_error>
validate_query_intent(std::uint32_t provider_id, const bioseq_cache_query_intent* intent) noexcept {
    if (intent == nullptr) {
        return std::unexpected(provider_contract_error::null_pointer);
    }
    if (intent->struct_size < sizeof(bioseq_cache_query_intent)) {
        return std::unexpected(provider_contract_error::struct_size);
    }
    if (provider_id == 0 || intent->provider_id != provider_id) {
        return std::unexpected(provider_contract_error::provider_mismatch);
    }
    if (intent->flags != 0 || intent->reserved != 0) {
        return std::unexpected(provider_contract_error::invalid_flags);
    }
    return {};
}

[[nodiscard]] inline provider_schema_view
boundary_schema_view(const bioseq_cache_path_domain& domain) noexcept {
    return provider_schema_view{
        .columns = domain.boundary_columns,
        .column_count = domain.boundary_column_count,
        .required_attnos = domain.required_boundary_attnos,
        .required_attno_count = domain.required_boundary_attno_count,
    };
}

[[nodiscard]] inline provider_schema_view
lineage_schema_view(const bioseq_cache_path_domain& domain) noexcept {
    return provider_schema_view{
        .columns = domain.lineage_columns,
        .column_count = domain.lineage_column_count,
        .required_attnos = domain.required_lineage_attnos,
        .required_attno_count = domain.required_lineage_attno_count,
    };
}

[[nodiscard]] inline std::expected<void, provider_contract_error>
validate_path_domain(std::uint32_t provider_id, const bioseq_cache_path_domain* domain) noexcept {
    if (domain == nullptr) {
        return std::unexpected(provider_contract_error::null_pointer);
    }
    if (domain->struct_size < sizeof(bioseq_cache_path_domain)) {
        return std::unexpected(provider_contract_error::struct_size);
    }
    if (provider_id == 0 || domain->provider_id != provider_id) {
        return std::unexpected(provider_contract_error::provider_mismatch);
    }
    if (domain->compatibility_family_id == 0) {
        return std::unexpected(provider_contract_error::invalid_compatibility_family);
    }
    if (domain->coverage_predicate == nullptr) {
        return std::unexpected(provider_contract_error::missing_predicate);
    }
    if (domain->provider_security_digest_format_revision !=
            bioseq_cache_provider_security_digest_format_canonical ||
        domain->provider_security_flags != 0) {
        return std::unexpected(provider_contract_error::security_namespace);
    }
    if (!validate_provider_schema(boundary_schema_view(*domain)).has_value()) {
        return std::unexpected(provider_contract_error::boundary_schema);
    }
    if (!validate_provider_schema(lineage_schema_view(*domain)).has_value()) {
        return std::unexpected(provider_contract_error::lineage_schema);
    }
    return {};
}

[[nodiscard]] constexpr bool
provider_column_equal(const bioseq_cache_column_schema& left,
                      const bioseq_cache_column_schema& right) noexcept {
    return left.struct_size >= sizeof(bioseq_cache_column_schema) &&
           right.struct_size >= sizeof(bioseq_cache_column_schema) && left.attno == right.attno &&
           left.semantic_id_high == right.semantic_id_high &&
           left.semantic_id_low == right.semantic_id_low && left.type_oid == right.type_oid &&
           left.typmod == right.typmod && left.collation_oid == right.collation_oid &&
           left.flags == right.flags;
}

[[nodiscard]] inline bool provider_schema_equal(const bioseq_cache_column_schema* left,
                                                std::size_t left_count,
                                                const bioseq_cache_column_schema* right,
                                                std::size_t right_count) noexcept {
    if (left_count != right_count || left_count > k_maximum_provider_boundary_columns ||
        (left_count != 0 && (left == nullptr || right == nullptr))) {
        return false;
    }
    for (std::size_t index = 0; index < left_count; ++index) {
        if (!provider_column_equal(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

/** Checks that a provider's decorated path exposes its declared output and  row-identity columns. */
[[nodiscard]] inline std::expected<void, provider_contract_error>
validate_decorated_path(const bioseq_cache_path_domain* domain,
                        const bioseq_cache_decorated_path* decorated) noexcept {
    if (domain == nullptr || decorated == nullptr) {
        return std::unexpected(provider_contract_error::null_pointer);
    }
    if (decorated->struct_size < sizeof(bioseq_cache_decorated_path)) {
        return std::unexpected(provider_contract_error::struct_size);
    }
    if (decorated->flags != 0) {
        return std::unexpected(provider_contract_error::invalid_flags);
    }
    if (decorated->path == nullptr) {
        return std::unexpected(provider_contract_error::decorated_path_missing);
    }
    if (!provider_schema_equal(domain->boundary_columns,
                               domain->boundary_column_count,
                               decorated->actual_boundary_columns,
                               decorated->actual_boundary_column_count)) {
        return std::unexpected(provider_contract_error::decorated_boundary_mismatch);
    }
    if (!provider_schema_equal(domain->lineage_columns,
                               domain->lineage_column_count,
                               decorated->actual_lineage_columns,
                               decorated->actual_lineage_column_count)) {
        return std::unexpected(provider_contract_error::decorated_lineage_mismatch);
    }
    return {};
}

/** Checks a finalized-plan digest recipe's format, required nodes and sizu limits. */
[[nodiscard]] constexpr std::expected<void, provider_contract_error>
validate_plan_digest_recipe(const bioseq_cache_plan_digest_recipe* recipe,
                            std::uint32_t recipe_count,
                            std::uint64_t serialized_bytes,
                            std::uint32_t node_count) noexcept {
    if (recipe == nullptr) {
        return std::unexpected(provider_contract_error::null_pointer);
    }
    if (recipe->struct_size < sizeof(bioseq_cache_plan_digest_recipe)) {
        return std::unexpected(provider_contract_error::struct_size);
    }
    if (recipe->flags != 0 || recipe->reserved != 0) {
        return std::unexpected(provider_contract_error::invalid_flags);
    }
    if (recipe->digest_format_revision != k_provider_recipe_format_revision ||
        recipe->plan_domain_nodes == nullptr ||
        !provider_recipe_shape_valid(recipe_count, serialized_bytes, node_count)) {
        return std::unexpected(provider_contract_error::recipe_shape);
    }
    return {};
}

/** Validated recipe sizes and the expected generation digest, without PostgreSQL node pointers. */
struct finalized_plan_recipe_contract {
    cache_digest_256 expected_generation_digest{};
    std::uint32_t digest_format_revision = 0;
    std::uint32_t recipe_count = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint32_t node_count = 0;
};

/**
 * Validates a plan recipe and records its expected generation digest.
 * When a path-time digest is supplied, both digests must match before
 * the result can be reused.
 */
[[nodiscard]] constexpr std::expected<finalized_plan_recipe_contract, provider_contract_error>
finalize_plan_digest_recipe(const bioseq_cache_plan_digest_recipe* recipe,
                            std::uint32_t recipe_count,
                            std::uint64_t serialized_bytes,
                            std::uint32_t node_count,
                            const cache_digest_256* coverage_path_digest = nullptr) noexcept {
    const auto validation =
        validate_plan_digest_recipe(recipe, recipe_count, serialized_bytes, node_count);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

    finalized_plan_recipe_contract finalized{
        .digest_format_revision = recipe->digest_format_revision,
        .recipe_count = recipe_count,
        .serialized_bytes = serialized_bytes,
        .node_count = node_count,
    };
    for (std::size_t index = 0; index < finalized.expected_generation_digest.bytes.size();
         ++index) {
        finalized.expected_generation_digest.bytes[index] =
            recipe->expected_generation_digest[index];
    }
    if (coverage_path_digest != nullptr &&
        !runtime_digest_matches(*coverage_path_digest, finalized.expected_generation_digest)) {
        return std::unexpected(provider_contract_error::coverage_digest_mismatch);
    }
    return finalized;
}

/** Caller-owned buffer that receives provider bytes for digest calculation. */
struct bounded_digest_sink_buffer {
    std::span<std::byte> bytes{};
    std::size_t size = 0;
    bool failed = false;
};

[[nodiscard]] inline bool
append_bounded_digest_bytes(void* opaque, const void* bytes, std::size_t size) noexcept {
    auto* buffer = static_cast<bounded_digest_sink_buffer*>(opaque);
    if (buffer == nullptr || buffer->failed || (size != 0 && bytes == nullptr) ||
        size > buffer->bytes.size() || buffer->size > buffer->bytes.size() - size) {
        if (buffer != nullptr) {
            buffer->failed = true;
        }
        return false;
    }
    if (size != 0) {
        std::memcpy(buffer->bytes.data() + buffer->size, bytes, size);
        buffer->size += size;
    }
    return true;
}

[[nodiscard]] inline bioseq_cache_digest_sink
make_digest_sink(bounded_digest_sink_buffer& buffer) noexcept {
    return bioseq_cache_digest_sink{
        .struct_size = sizeof(bioseq_cache_digest_sink),
        .opaque = &buffer,
        .append = &append_bounded_digest_bytes,
    };
}

[[nodiscard]] inline bool digest_bytes_equal(std::span<const std::byte> left,
                                             std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |=
            static_cast<std::uint8_t>(left[index]) ^ static_cast<std::uint8_t>(right[index]);
    }
    return difference == 0;
}

enum class coverage_branch_kind : std::uint8_t {
    drop = 0,
    cached = 1,
    residual = 2,
};

struct coverage_branch_choice {
    coverage_branch_kind kind = coverage_branch_kind::drop;
    std::uint32_t source_index = 0;
};

/**
 * Drops rows that fail the requested predicate. Each remaining row
 * belongs to the first valid cached source whose predicate is true,
 * or to fresh  computation if none matches. SQL unknown values fail
 * the predicate check.
 */
[[nodiscard]] constexpr coverage_branch_choice
choose_coverage_branch(sql_bool requested_predicate,
                       std::span<const sql_bool> source_predicates,
                       std::span<const bool> source_validity) noexcept {
    if (!passes_where(requested_predicate)) {
        return {};
    }
    for (std::size_t source = 0; source < source_validity.size(); ++source) {
        if (!source_validity[source] || source >= source_predicates.size() ||
            !passes_where(source_predicates[source])) {
            continue;
        }
        return coverage_branch_choice{
            .kind = coverage_branch_kind::cached,
            .source_index = static_cast<std::uint32_t>(source),
        };
    }
    return coverage_branch_choice{
        .kind = coverage_branch_kind::residual,
        .source_index = static_cast<std::uint32_t>(source_validity.size()),
    };
}

/** Checks whether this source is the first valid source that supplies the requested row. */
[[nodiscard]] constexpr bool
source_owns_occurrence(std::size_t source_index,
                       sql_bool requested_predicate,
                       std::span<const sql_bool> source_predicates,
                       std::span<const bool> source_validity) noexcept {
    const coverage_branch_choice choice =
        choose_coverage_branch(requested_predicate, source_predicates, source_validity);
    return choice.kind == coverage_branch_kind::cached && choice.source_index == source_index;
}

/** Compares row-occurrence identities, keeping separate query bindings distinct in self-joins. */
[[nodiscard]] constexpr bool occurrence_equal(const bioseq_cache_occurrence& left,
                                              const bioseq_cache_occurrence& right) noexcept {
    return left.struct_size >= sizeof(bioseq_cache_occurrence) &&
           right.struct_size >= sizeof(bioseq_cache_occurrence) &&
           left.rte_ordinal == right.rte_ordinal &&
           left.logical_relation_oid == right.logical_relation_oid &&
           left.leaf_relation_oid == right.leaf_relation_oid &&
           left.identity_kind == right.identity_kind && left.reserved == 0 && right.reserved == 0 &&
           left.row_identity == right.row_identity &&
           left.occurrence_high == right.occurrence_high &&
           left.occurrence_low == right.occurrence_low;
}

/** Executor buffers used to check values and completion statuses returned by a late fetch. */
struct late_fetch_sink_state {
    std::size_t reference_count = 0;
    std::size_t attribute_count = 0;
    std::span<std::uint8_t> terminal_status{};
    std::span<std::uint8_t> value_seen{};
    std::span<std::uintptr_t> values{};
    std::span<std::uint8_t> nulls{};
    bool malformed = false;
    bool any_reference_failure = false;
};

[[nodiscard]] inline bool initialize_late_fetch_sink_state(late_fetch_sink_state& state) noexcept {
    if (state.attribute_count != 0 &&
        state.reference_count > (std::numeric_limits<std::size_t>::max)() / state.attribute_count) {
        state.malformed = true;
        return false;
    }
    const std::size_t value_count = state.reference_count * state.attribute_count;
    if (state.terminal_status.size() < state.reference_count ||
        state.value_seen.size() < value_count || state.values.size() < value_count ||
        state.nulls.size() < value_count) {
        state.malformed = true;
        return false;
    }
    std::fill_n(state.terminal_status.begin(), state.reference_count, k_late_fetch_status_unset);
    std::fill_n(state.value_seen.begin(), value_count, std::uint8_t{0});
    std::fill_n(state.values.begin(), value_count, std::uintptr_t{0});
    std::fill_n(state.nulls.begin(), value_count, std::uint8_t{0});
    state.malformed = false;
    state.any_reference_failure = false;
    return true;
}

[[nodiscard]] inline bioseq_cache_status
late_fetch_emit_value(void* opaque,
                      std::size_t reference_index,
                      std::size_t requested_attribute_index,
                      std::uintptr_t datum,
                      bool is_null) noexcept {
    auto* state = static_cast<late_fetch_sink_state*>(opaque);
    if (state == nullptr || state->malformed || reference_index >= state->reference_count ||
        requested_attribute_index >= state->attribute_count ||
        state->terminal_status[reference_index] != k_late_fetch_status_unset) {
        if (state != nullptr) {
            state->malformed = true;
        }
        return bioseq_cache_status_error;
    }
    const std::size_t index = reference_index * state->attribute_count + requested_attribute_index;
    if (state->value_seen[index] != 0) {
        state->malformed = true;
        return bioseq_cache_status_error;
    }
    state->value_seen[index] = 1;
    state->values[index] = datum;
    state->nulls[index] = is_null ? 1U : 0U;
    return bioseq_cache_status_ok;
}

inline void late_fetch_emit_reference_status(void* opaque,
                                             std::size_t reference_index,
                                             bioseq_cache_status status) noexcept {
    auto* state = static_cast<late_fetch_sink_state*>(opaque);
    if (state == nullptr || state->malformed || reference_index >= state->reference_count ||
        state->terminal_status[reference_index] != k_late_fetch_status_unset ||
        (status != bioseq_cache_status_ok && status != bioseq_cache_status_unsupported &&
         status != bioseq_cache_status_error)) {
        if (state != nullptr) {
            state->malformed = true;
        }
        return;
    }
    state->terminal_status[reference_index] = static_cast<std::uint8_t>(status);
    state->any_reference_failure = state->any_reference_failure || status != bioseq_cache_status_ok;
}

[[nodiscard]] inline bioseq_cache_late_fetch_sink
make_late_fetch_sink(late_fetch_sink_state& state) noexcept {
    return bioseq_cache_late_fetch_sink{
        .struct_size = sizeof(bioseq_cache_late_fetch_sink),
        .opaque = &state,
        .emit_value = &late_fetch_emit_value,
        .emit_reference_status = &late_fetch_emit_reference_status,
    };
}

/** Requires a successful completion status and every requested value, including nulls, for each row reference. */
[[nodiscard]] inline bool late_fetch_complete_and_valid(late_fetch_sink_state& state) noexcept {
    if (state.malformed || state.any_reference_failure) {
        return false;
    }
    for (std::size_t reference = 0; reference < state.reference_count; ++reference) {
        if (state.terminal_status[reference] != static_cast<std::uint8_t>(bioseq_cache_status_ok)) {
            return false;
        }
        for (std::size_t attribute = 0; attribute < state.attribute_count; ++attribute) {
            const std::size_t index = reference * state.attribute_count + attribute;
            if (state.value_seen[index] == 0) {
                state.malformed = true;
                return false;
            }
        }
    }
    return true;
}

enum class candidate_reference_kind : std::uint8_t {
    none = 0,
    provisional_pin = 1,
    in_flight_waiter = 2,
    admission_reservation = 3,
};

using candidate_release_no_error = void (*)(void* opaque,
                                            const cache_entry_id& entry_id,
                                            candidate_reference_kind reference_kind) noexcept;

/** Tracks a source reference for explicit  cleanup when PostgreSQL errors skip C++ destructors. */
struct planner_candidate_cleanup_record {
    planner_candidate_cleanup_record* next = nullptr;
    cache_entry_id entry_id{};
    candidate_reference_kind reference_kind = candidate_reference_kind::none;
    candidate_release_no_error release = nullptr;
    void* release_opaque = nullptr;
    bool active = false;
};

[[nodiscard]] inline bool activate_candidate_reference(planner_candidate_cleanup_record& record,
                                                       const cache_entry_id& entry_id,
                                                       candidate_reference_kind reference_kind,
                                                       candidate_release_no_error release,
                                                       void* release_opaque) noexcept {
    if (record.active || reference_kind == candidate_reference_kind::none || release == nullptr) {
        return false;
    }
    record.entry_id = entry_id;
    record.reference_kind = reference_kind;
    record.release = release;
    record.release_opaque = release_opaque;
    record.active = true;
    return true;
}

/** Clears the active record before releasing its reference, so repeated or nested cleanup calls do nothing */
inline void release_candidate_reference_noerror(planner_candidate_cleanup_record& record) noexcept {
    if (!record.active) {
        return;
    }
    record.active = false;
    const candidate_release_no_error release = record.release;
    void* const release_opaque = record.release_opaque;
    const cache_entry_id entry_id = record.entry_id;
    const candidate_reference_kind reference_kind = record.reference_kind;
    record.reference_kind = candidate_reference_kind::none;
    record.release = nullptr;
    record.release_opaque = nullptr;
    if (release != nullptr) {
        release(release_opaque, entry_id, reference_kind);
    }
}

enum class reuse_preflight_state : std::uint8_t {
    empty = 0,
    collecting = 1,
    frozen = 2,
};

/** Stages source-validity decisions and exposes them together woen preflight finishes. */
struct reuse_gate_freeze {
    std::array<std::uint8_t, k_maximum_sources_per_coverage_node> staged{};
    std::array<std::uint8_t, k_maximum_sources_per_coverage_node> published{};
    std::size_t source_count = 0;
    reuse_preflight_state state = reuse_preflight_state::empty;
};

[[nodiscard]] inline bool begin_gate_preflight(reuse_gate_freeze& gates,
                                               std::size_t source_count) noexcept {
    if (gates.state != reuse_preflight_state::empty || source_count == 0 ||
        source_count > k_maximum_sources_per_coverage_node) {
        return false;
    }
    gates.staged.fill(0);
    gates.published.fill(0);
    gates.source_count = source_count;
    gates.state = reuse_preflight_state::collecting;
    return true;
}

[[nodiscard]] inline bool stage_valid_source(reuse_gate_freeze& gates,
                                             std::size_t source_index) noexcept {
    if (gates.state != reuse_preflight_state::collecting || source_index >= gates.source_count) {
        return false;
    }
    gates.staged[source_index] = 1;
    return true;
}

[[nodiscard]] inline bool freeze_source_decisions(reuse_gate_freeze& gates) noexcept {
    if (gates.state != reuse_preflight_state::collecting) {
        return false;
    }
    gates.published = gates.staged;
    gates.state = reuse_preflight_state::frozen;
    return true;
}

/** Finishes preflight with every cached source disabled, allowing normal execution. */
inline void freeze_all_sources_invalid(reuse_gate_freeze& gates) noexcept {
    if (gates.state != reuse_preflight_state::collecting) {
        return;
    }
    gates.staged.fill(0);
    gates.published.fill(0);
    gates.state = reuse_preflight_state::frozen;
}

[[nodiscard]] constexpr bool source_gate_is_valid(const reuse_gate_freeze& gates,
                                                  std::size_t source_index) noexcept {
    return gates.state == reuse_preflight_state::frozen && source_index < gates.source_count &&
           gates.published[source_index] != 0;
}

/** Wait only for a generation older than every generation owned locally. */
[[nodiscard]] constexpr bool
claim_order_allows_wait(std::uint64_t candidate_claim_sequence,
                        std::span<const std::uint64_t> locally_owned_claim_sequences) noexcept {
    if (candidate_claim_sequence == 0) {
        return false;
    }
    for (const std::uint64_t owned : locally_owned_claim_sequences) {
        if (owned == 0 || candidate_claim_sequence >= owned) {
            return false;
        }
    }
    return true;
}

static_assert(std::is_standard_layout_v<coverage_security_namespace>);
static_assert(std::is_trivially_copyable_v<coverage_security_namespace>);
static_assert(std::is_standard_layout_v<planner_candidate_cleanup_record>);

} // namespace bioseq::cache::pg::reuse
