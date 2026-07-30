/**
 * @file bioseq_caching/pg/reusable_planning.cppm
 * @brief Bounded, provider-neutral reusable-coverage planning primitives.
 *
 * This module intentionally has no PostgreSQL header dependency.  It owns the
 * contracts that must remain testable without a backend: collision-safe
 * candidate discovery, provider POD validation, runtime digest sinks,
 * occurrence-disjoint SQL-3VL branch ownership, late-fetch sink validation,
 * and no-throw candidate-reference cleanup.
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

export module bioseq.cache.pg.reusable_planning;

import bioseq.cache.core;

export namespace bioseq::cache::pg::reuse {

inline constexpr std::uint32_t kReusablePlanFormatVersionV1 = 1;
inline constexpr std::uint8_t kLateFetchStatusUnsetV1 = 0xffU;

/** Planning and shared-registry code use one padding-independent contract. */
using CoverageSecurityNamespaceV1 = ReusableSecurityNamespaceV1;

/** Provider callback payload being framed into one canonical reusable key. */
enum class CoverageCanonicalKeyKindV1 : std::uint8_t {
    Domain = 1,
    Generation = 2,
};

/** Binding-neutral identity of the one provider fragment supported by V1. */
struct CoverageFragmentBindingSignatureV1 {
    std::uint32_t format_version = 1;
    std::uint32_t fragment_binding_ordinal = 0;
    std::uint32_t provider_rte_occurrence_ordinal = 0;
    std::uint32_t provider_rte_occurrence_count = 0;
    std::uint32_t logical_relation_oid = 0;
    std::uint32_t selected_relation_oid = 0;
    std::uint32_t provider_id = 0;
    std::uint32_t flags = 0;
    CacheDigest256 binding_role_digest{};

    [[nodiscard]] bool operator==(
        const CoverageFragmentBindingSignatureV1&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<CoverageFragmentBindingSignatureV1>);
static_assert(std::is_trivially_copyable_v<
    CoverageFragmentBindingSignatureV1>);

/** Structural checks that do not depend on PostgreSQL planner memory. */
[[nodiscard]] constexpr bool valid_coverage_fragment_binding(
    const CoverageFragmentBindingSignatureV1& binding) noexcept {
    return binding.format_version == 1
        && binding.fragment_binding_ordinal == 0
        && binding.provider_rte_occurrence_count != 0
        && binding.provider_rte_occurrence_ordinal
            < binding.provider_rte_occurrence_count
        && binding.logical_relation_oid != 0
        && binding.selected_relation_oid != 0
        && binding.provider_id != 0
        && binding.flags == 0;
}

enum class CoverageBindingSemanticErrorV1 : std::uint8_t {
    InvalidKind = 1,
    InvalidBinding,
    InvalidPredicateBytes,
    OutputTooSmall,
};

inline constexpr std::array<std::byte, 4> kCoverageBindingSemanticMagicV1{
    std::byte{0x46}, std::byte{0x56}, std::byte{0x54}, std::byte{0x31}};
inline constexpr std::size_t kCoverageBindingSemanticFixedBytesV1 = 77;

/** Fail-closed canonical framing errors; callers retain the provider path. */
enum class CoverageCanonicalKeyErrorV1 : std::uint8_t {
    InvalidKind = 1,
    InvalidSecurityNamespace,
    EmptyProviderSemanticBytes,
    KeyTooLarge,
    OutputTooSmall,
};

inline constexpr std::array<std::byte, 4> kCoverageCanonicalKeyMagicV1{
    std::byte{0x42}, std::byte{0x43}, std::byte{0x4b}, std::byte{0x31}};
inline constexpr std::size_t kCoverageCanonicalKeyFixedBytesV1 = 78;

namespace canonical_key_detail {

inline constexpr std::uint8_t kKindTagV1 = 0x01;
inline constexpr std::uint8_t kFormatVersionTagV1 = 0x02;
inline constexpr std::uint8_t kDatabaseOidTagV1 = 0x10;
inline constexpr std::uint8_t kCurrentRoleOidTagV1 = 0x11;
inline constexpr std::uint8_t kSessionUserOidTagV1 = 0x12;
inline constexpr std::uint8_t kSecurityContextFlagsTagV1 = 0x13;
inline constexpr std::uint8_t kRowSecurityModeTagV1 = 0x14;
inline constexpr std::uint8_t kProviderSecurityDigestTagV1 = 0x15;
inline constexpr std::uint8_t kProviderSemanticBytesTagV1 = 0x20;

[[nodiscard]] constexpr bool valid_kind(
    CoverageCanonicalKeyKindV1 kind) noexcept {
    return kind == CoverageCanonicalKeyKindV1::Domain
        || kind == CoverageCanonicalKeyKindV1::Generation;
}

constexpr void append_u8(
    std::span<std::byte> output,
    std::size_t& offset,
    std::uint8_t value) noexcept {
    output[offset++] = std::byte{value};
}

constexpr void append_u32_le(
    std::span<std::byte> output,
    std::size_t& offset,
    std::uint32_t value) noexcept {
    for (std::size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
        append_u8(
            output,
            offset,
            static_cast<std::uint8_t>(value >> (byte_index * 8U)));
    }
}

constexpr void append_u64_le(
    std::span<std::byte> output,
    std::size_t& offset,
    std::uint64_t value) noexcept {
    for (std::size_t byte_index = 0; byte_index < sizeof(value); ++byte_index) {
        append_u8(
            output,
            offset,
            static_cast<std::uint8_t>(value >> (byte_index * 8U)));
    }
}

} // namespace canonical_key_detail

/**
 * Write the cache-owned fragment-binding trailer appended after provider
 * semantic bytes. Generation trailers additionally carry the exact already-
 * normalized predicate bytes; domain trailers deliberately exclude Pi.
 */
[[nodiscard]] inline std::expected<
    std::size_t, CoverageBindingSemanticErrorV1>
write_coverage_binding_semantics(
    CoverageCanonicalKeyKindV1 kind,
    const CoverageFragmentBindingSignatureV1& binding,
    std::span<const std::byte> normalized_predicate,
    std::span<std::byte> output) noexcept {
    using namespace canonical_key_detail;
    if (!valid_kind(kind)) {
        return std::unexpected(
            CoverageBindingSemanticErrorV1::InvalidKind);
    }
    if (!valid_coverage_fragment_binding(binding)) {
        return std::unexpected(
            CoverageBindingSemanticErrorV1::InvalidBinding);
    }
    const bool generation =
        kind == CoverageCanonicalKeyKindV1::Generation;
    if (generation != !normalized_predicate.empty()) {
        return std::unexpected(
            CoverageBindingSemanticErrorV1::InvalidPredicateBytes);
    }
    const std::size_t predicate_frame_bytes = generation
        ? 1U + sizeof(std::uint64_t) + normalized_predicate.size()
        : 0U;
    if (normalized_predicate.size()
            > (std::numeric_limits<std::size_t>::max)()
                - kCoverageBindingSemanticFixedBytesV1
                - 1U - sizeof(std::uint64_t)
        || output.size()
            < kCoverageBindingSemanticFixedBytesV1
                + predicate_frame_bytes) {
        return std::unexpected(
            CoverageBindingSemanticErrorV1::OutputTooSmall);
    }

    std::size_t offset = 0;
    for (const std::byte value : kCoverageBindingSemanticMagicV1) {
        output[offset++] = value;
    }
    const auto append_tagged_u32 = [&output, &offset](
            std::uint8_t tag, std::uint32_t value) constexpr {
        append_u8(output, offset, tag);
        append_u32_le(output, offset, value);
    };
    append_tagged_u32(0x01, binding.format_version);
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

/** Copy the provider-owned fixed digest after validate_path_domain succeeds. */
[[nodiscard]] inline CacheDigest256 provider_security_digest(
    const BioseqCachePathDomainV2& domain) noexcept {
    CacheDigest256 result{};
    std::copy_n(
        domain.provider_security_digest,
        result.bytes.size(),
        result.bytes.begin());
    return result;
}

/**
 * Frame security fields and provider semantic bytes in a fixed tagged order.
 * Integers are normalized to little endian and struct padding is never read.
 */
[[nodiscard]] inline std::expected<std::size_t, CoverageCanonicalKeyErrorV1>
write_coverage_canonical_key(
    CoverageCanonicalKeyKindV1 kind,
    const CoverageSecurityNamespaceV1& security_namespace,
    std::span<const std::byte> provider_semantic_bytes,
    std::span<std::byte> output) noexcept {
    using namespace canonical_key_detail;
    if (!valid_kind(kind)) {
        return std::unexpected(CoverageCanonicalKeyErrorV1::InvalidKind);
    }
    if (!valid_reusable_security_namespace(security_namespace)) {
        return std::unexpected(
            CoverageCanonicalKeyErrorV1::InvalidSecurityNamespace);
    }
    if (provider_semantic_bytes.empty()) {
        return std::unexpected(
            CoverageCanonicalKeyErrorV1::EmptyProviderSemanticBytes);
    }
    const std::uint64_t limit =
        kind == CoverageCanonicalKeyKindV1::Domain
        ? kMaxCoverageDomainKeyBytes
        : kMaxCanonicalKeyBytes;
    if (provider_semantic_bytes.size()
        > limit - kCoverageCanonicalKeyFixedBytesV1) {
        return std::unexpected(CoverageCanonicalKeyErrorV1::KeyTooLarge);
    }
    const std::size_t required =
        kCoverageCanonicalKeyFixedBytesV1 + provider_semantic_bytes.size();
    if (output.size() < required) {
        return std::unexpected(CoverageCanonicalKeyErrorV1::OutputTooSmall);
    }

    std::size_t offset = 0;
    for (const std::byte value : kCoverageCanonicalKeyMagicV1) {
        output[offset++] = value;
    }
    append_u8(output, offset, kKindTagV1);
    append_u8(output, offset, static_cast<std::uint8_t>(kind));
    append_u8(output, offset, kFormatVersionTagV1);
    append_u32_le(output, offset, kReusablePlanFormatVersionV1);
    append_u8(output, offset, kDatabaseOidTagV1);
    append_u32_le(output, offset, security_namespace.database_oid);
    append_u8(output, offset, kCurrentRoleOidTagV1);
    append_u32_le(output, offset, security_namespace.current_role_oid);
    append_u8(output, offset, kSessionUserOidTagV1);
    append_u32_le(output, offset, security_namespace.session_user_oid);
    append_u8(output, offset, kSecurityContextFlagsTagV1);
    append_u32_le(output, offset, security_namespace.security_context_flags);
    append_u8(output, offset, kRowSecurityModeTagV1);
    append_u32_le(
        output,
        offset,
        static_cast<std::uint32_t>(security_namespace.row_security_mode));
    append_u8(output, offset, kProviderSecurityDigestTagV1);
    for (const std::uint8_t value :
         security_namespace.provider_security_digest.bytes) {
        append_u8(output, offset, value);
    }
    append_u8(output, offset, kProviderSemanticBytesTagV1);
    append_u64_le(output, offset, provider_semantic_bytes.size());
    for (const std::byte value : provider_semantic_bytes) {
        output[offset++] = value;
    }
    return offset;
}

/** One collision bucket request. Canonical bytes remain the equality proof. */
struct CoverageCandidateProbeV1 {
    CacheDigest256 domain_digest{};
    std::span<const std::byte> canonical_domain_key{};
    CoverageSecurityNamespaceV1 security_namespace{};
    std::uint32_t provider_id = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t required_capability_mask = 0;
};

/** Immutable planner snapshot of one member in a bounded coverage bucket. */
struct CoverageCandidateViewV1 {
    CacheEntryId entry_id{};
    CacheDigest256 domain_digest{};
    std::span<const std::byte> canonical_domain_key{};
    std::span<const std::byte> canonical_generation_key{};
    CoverageSecurityNamespaceV1 security_namespace{};
    std::uint32_t provider_id = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    bool complete = false;
    bool in_flight = false;
    std::uint64_t estimated_covered_rows = 0;
    std::uint64_t stored_bytes = 0;
};

/** Diagnostics and stable indices into the caller's candidate snapshot. */
struct BoundedCandidateSelectionV1 {
    std::array<std::size_t, kMaxSourcesPerCoverageNode> indices{};
    std::size_t selected_count = 0;
    std::size_t visited_count = 0;
    std::size_t qualified_count = 0;
    std::size_t digest_collision_count = 0;
    std::size_t invalid_key_count = 0;
    std::size_t invalid_namespace_count = 0;
    bool bucket_truncated = false;
    bool result_truncated = false;
};

[[nodiscard]] constexpr bool bounded_canonical_key(
    std::span<const std::byte> bytes,
    std::uint64_t limit) noexcept {
    return !bytes.empty() && bytes.size() <= limit;
}

/** Exact-current coalescing always proves digest, length, then every byte. */
[[nodiscard]] inline bool exact_current_key_equal(
    const CacheDigest256& requested_digest,
    std::span<const std::byte> requested_bytes,
    const CacheDigest256& candidate_digest,
    std::span<const std::byte> candidate_bytes) noexcept {
    return requested_digest == candidate_digest
        && requested_bytes.size() == candidate_bytes.size()
        && std::equal(
            requested_bytes.begin(), requested_bytes.end(),
            candidate_bytes.begin());
}

[[nodiscard]] inline CoverageSourceOrderKey candidate_order_key(
    const CoverageCandidateViewV1& candidate) noexcept {
    return CoverageSourceOrderKey{
        .complete = candidate.complete,
        .estimated_covered_rows = candidate.estimated_covered_rows,
        .stored_bytes = candidate.stored_bytes,
        .claim_sequence = candidate.entry_id.claim_sequence,
        .digest = candidate.entry_id.digest,
    };
}

[[nodiscard]] inline bool candidate_precedes(
    const CoverageCandidateViewV1& left,
    const CoverageCandidateViewV1& right) noexcept {
    return coverage_source_precedes(
        candidate_order_key(left), candidate_order_key(right));
}

/**
 * Visit at most 64 bucket members and retain the best 32.  Security and full
 * domain-key equality are established before a caller may pin or map a source.
 */
[[nodiscard]] inline BoundedCandidateSelectionV1 select_candidates(
    const CoverageCandidateProbeV1& probe,
    std::span<const CoverageCandidateViewV1> candidates) noexcept {
    BoundedCandidateSelectionV1 result{};
    const std::size_t visit_count = std::min<std::size_t>(
        candidates.size(), kMaxCoverageGenerationsPerDomain);
    result.visited_count = visit_count;
    result.bucket_truncated = candidates.size() > visit_count;

    if (probe.provider_id == 0
        || probe.compatibility_family_id == 0
        || !valid_reusable_security_namespace(probe.security_namespace)
        || !bounded_canonical_key(
            probe.canonical_domain_key, kMaxCoverageDomainKeyBytes)) {
        return result;
    }

    for (std::size_t candidate_index = 0;
         candidate_index < visit_count;
         ++candidate_index) {
        const CoverageCandidateViewV1& candidate =
            candidates[candidate_index];
        if (!valid_reusable_security_namespace(
                candidate.security_namespace)) {
            ++result.invalid_namespace_count;
            continue;
        }
        if (!bounded_canonical_key(
                candidate.canonical_domain_key,
                kMaxCoverageDomainKeyBytes)
            || !bounded_canonical_key(
                candidate.canonical_generation_key,
                kMaxCanonicalKeyBytes)) {
            ++result.invalid_key_count;
            continue;
        }
        if (candidate.security_namespace != probe.security_namespace
            || candidate.provider_id != probe.provider_id
            || candidate.compatibility_family_id
                != probe.compatibility_family_id
            || candidate.domain_digest != probe.domain_digest) {
            continue;
        }
        if (candidate.canonical_domain_key.size()
                != probe.canonical_domain_key.size()
            || !std::equal(
                candidate.canonical_domain_key.begin(),
                candidate.canonical_domain_key.end(),
                probe.canonical_domain_key.begin())) {
            ++result.digest_collision_count;
            continue;
        }
        if (!capabilities_satisfy(
                candidate.capability_mask,
                probe.required_capability_mask)
            || (!candidate.complete && !candidate.in_flight)) {
            continue;
        }

        ++result.qualified_count;
        std::size_t position = 0;
        while (position < result.selected_count
               && !candidate_precedes(
                   candidate, candidates[result.indices[position]])) {
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
        for (std::size_t move = result.selected_count - 1;
             move > position;
             --move) {
            result.indices[move] = result.indices[move - 1];
        }
        result.indices[position] = candidate_index;
        if (old_count == result.indices.size()) {
            result.result_truncated = true;
        }
    }
    return result;
}

enum class ProviderContractErrorV1 : std::uint16_t {
    None = 0,
    NullPointer,
    StructSize,
    ProviderMismatch,
    InvalidFlags,
    MissingPredicate,
    InvalidCompatibilityFamily,
    BoundarySchema,
    LineageSchema,
    DecoratedPathMissing,
    DecoratedBoundaryMismatch,
    DecoratedLineageMismatch,
    RecipeShape,
    CoverageDigestMismatch,
    SecurityNamespace,
};

[[nodiscard]] constexpr std::expected<void, ProviderContractErrorV1>
validate_query_intent(
    std::uint32_t provider_id,
    const BioseqCacheQueryIntentV2* intent) noexcept {
    if (intent == nullptr) {
        return std::unexpected(ProviderContractErrorV1::NullPointer);
    }
    if (intent->struct_size < sizeof(BioseqCacheQueryIntentV2)) {
        return std::unexpected(ProviderContractErrorV1::StructSize);
    }
    if (provider_id == 0 || intent->provider_id != provider_id) {
        return std::unexpected(ProviderContractErrorV1::ProviderMismatch);
    }
    if (intent->flags != 0 || intent->reserved != 0) {
        return std::unexpected(ProviderContractErrorV1::InvalidFlags);
    }
    return {};
}

[[nodiscard]] inline ProviderSchemaView boundary_schema_view(
    const BioseqCachePathDomainV2& domain) noexcept {
    return ProviderSchemaView{
        .columns = domain.boundary_columns,
        .column_count = domain.boundary_column_count,
        .required_attnos = domain.required_boundary_attnos,
        .required_attno_count = domain.required_boundary_attno_count,
    };
}

[[nodiscard]] inline ProviderSchemaView lineage_schema_view(
    const BioseqCachePathDomainV2& domain) noexcept {
    return ProviderSchemaView{
        .columns = domain.lineage_columns,
        .column_count = domain.lineage_column_count,
        .required_attnos = domain.required_lineage_attnos,
        .required_attno_count = domain.required_lineage_attno_count,
    };
}

[[nodiscard]] inline std::expected<void, ProviderContractErrorV1>
validate_path_domain(
    std::uint32_t provider_id,
    const BioseqCachePathDomainV2* domain) noexcept {
    if (domain == nullptr) {
        return std::unexpected(ProviderContractErrorV1::NullPointer);
    }
    if (domain->struct_size < sizeof(BioseqCachePathDomainV2)) {
        return std::unexpected(ProviderContractErrorV1::StructSize);
    }
    if (provider_id == 0 || domain->provider_id != provider_id) {
        return std::unexpected(ProviderContractErrorV1::ProviderMismatch);
    }
    if (domain->compatibility_family_id == 0) {
        return std::unexpected(
            ProviderContractErrorV1::InvalidCompatibilityFamily);
    }
    if (domain->coverage_predicate == nullptr) {
        return std::unexpected(ProviderContractErrorV1::MissingPredicate);
    }
    if (domain->provider_security_digest_format_version
            != BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1
        || domain->provider_security_flags != 0) {
        return std::unexpected(ProviderContractErrorV1::SecurityNamespace);
    }
    if (!validate_provider_schema(boundary_schema_view(*domain)).has_value()) {
        return std::unexpected(ProviderContractErrorV1::BoundarySchema);
    }
    if (!validate_provider_schema(lineage_schema_view(*domain)).has_value()) {
        return std::unexpected(ProviderContractErrorV1::LineageSchema);
    }
    return {};
}

[[nodiscard]] constexpr bool provider_column_equal(
    const BioseqCacheColumnSchemaV2& left,
    const BioseqCacheColumnSchemaV2& right) noexcept {
    return left.struct_size >= sizeof(BioseqCacheColumnSchemaV2)
        && right.struct_size >= sizeof(BioseqCacheColumnSchemaV2)
        && left.attno == right.attno
        && left.semantic_id_high == right.semantic_id_high
        && left.semantic_id_low == right.semantic_id_low
        && left.type_oid == right.type_oid
        && left.typmod == right.typmod
        && left.collation_oid == right.collation_oid
        && left.flags == right.flags;
}

[[nodiscard]] inline bool provider_schema_equal(
    const BioseqCacheColumnSchemaV2* left,
    std::size_t left_count,
    const BioseqCacheColumnSchemaV2* right,
    std::size_t right_count) noexcept {
    if (left_count != right_count
        || left_count > kMaxProviderBoundaryColumns
        || (left_count != 0 && (left == nullptr || right == nullptr))) {
        return false;
    }
    for (std::size_t index = 0; index < left_count; ++index) {
        if (!provider_column_equal(left[index], right[index])) {
            return false;
        }
    }
    return true;
}

/** Decorated output must reproduce the complete advertised schema contract. */
[[nodiscard]] inline std::expected<void, ProviderContractErrorV1>
validate_decorated_path(
    const BioseqCachePathDomainV2* domain,
    const BioseqCacheDecoratedPathV2* decorated) noexcept {
    if (domain == nullptr || decorated == nullptr) {
        return std::unexpected(ProviderContractErrorV1::NullPointer);
    }
    if (decorated->struct_size < sizeof(BioseqCacheDecoratedPathV2)) {
        return std::unexpected(ProviderContractErrorV1::StructSize);
    }
    if (decorated->flags != 0) {
        return std::unexpected(ProviderContractErrorV1::InvalidFlags);
    }
    if (decorated->path == nullptr) {
        return std::unexpected(
            ProviderContractErrorV1::DecoratedPathMissing);
    }
    if (!provider_schema_equal(
            domain->boundary_columns,
            domain->boundary_column_count,
            decorated->actual_boundary_columns,
            decorated->actual_boundary_column_count)) {
        return std::unexpected(
            ProviderContractErrorV1::DecoratedBoundaryMismatch);
    }
    if (!provider_schema_equal(
            domain->lineage_columns,
            domain->lineage_column_count,
            decorated->actual_lineage_columns,
            decorated->actual_lineage_column_count)) {
        return std::unexpected(
            ProviderContractErrorV1::DecoratedLineageMismatch);
    }
    return {};
}

/** Validate the copyable, bounded finalized-plan/runtime recipe envelope. */
[[nodiscard]] constexpr std::expected<void, ProviderContractErrorV1>
validate_plan_digest_recipe(
    const BioseqCachePlanDigestRecipeV2* recipe,
    std::uint32_t recipe_count,
    std::uint64_t serialized_bytes,
    std::uint32_t node_count) noexcept {
    if (recipe == nullptr) {
        return std::unexpected(ProviderContractErrorV1::NullPointer);
    }
    if (recipe->struct_size < sizeof(BioseqCachePlanDigestRecipeV2)) {
        return std::unexpected(ProviderContractErrorV1::StructSize);
    }
    if (recipe->flags != 0 || recipe->reserved != 0) {
        return std::unexpected(ProviderContractErrorV1::InvalidFlags);
    }
    if (recipe->digest_format_version != kProviderRecipeFormatVersion
        || recipe->plan_domain_nodes == nullptr
        || !provider_recipe_shape_valid(
            recipe_count, serialized_bytes, node_count)) {
        return std::unexpected(ProviderContractErrorV1::RecipeShape);
    }
    return {};
}

/**
 * Pure, all-or-nothing snapshot of one validated finalized-plan recipe.
 * PostgreSQL-owned recipe Nodes remain outside this provider-neutral value.
 */
struct FinalizedPlanRecipeContractV1 {
    CacheDigest256 expected_generation_digest{};
    std::uint32_t digest_format_version = 0;
    std::uint32_t recipe_count = 0;
    std::uint64_t serialized_bytes = 0;
    std::uint32_t node_count = 0;
};

/**
 * Accept recipe shape and expected output digest as one value.  Coverage may
 * additionally require the finalized digest to equal its path-time digest.
 */
[[nodiscard]] constexpr std::expected<
    FinalizedPlanRecipeContractV1,
    ProviderContractErrorV1>
finalize_plan_digest_recipe(
    const BioseqCachePlanDigestRecipeV2* recipe,
    std::uint32_t recipe_count,
    std::uint64_t serialized_bytes,
    std::uint32_t node_count,
    const CacheDigest256* coverage_path_digest = nullptr) noexcept {
    const auto validation = validate_plan_digest_recipe(
        recipe, recipe_count, serialized_bytes, node_count);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

    FinalizedPlanRecipeContractV1 finalized{
        .digest_format_version = recipe->digest_format_version,
        .recipe_count = recipe_count,
        .serialized_bytes = serialized_bytes,
        .node_count = node_count,
    };
    for (std::size_t index = 0;
         index < finalized.expected_generation_digest.bytes.size();
         ++index) {
        finalized.expected_generation_digest.bytes[index] =
            recipe->expected_generation_digest[index];
    }
    if (coverage_path_digest != nullptr
        && !runtime_digest_matches(
            *coverage_path_digest,
            finalized.expected_generation_digest)) {
        return std::unexpected(
            ProviderContractErrorV1::CoverageDigestMismatch);
    }
    return finalized;
}

/** Caller-owned fixed buffer used directly as a provider digest sink. */
struct BoundedDigestSinkBufferV1 {
    std::span<std::byte> bytes{};
    std::size_t size = 0;
    bool failed = false;
};

[[nodiscard]] inline bool append_bounded_digest_bytes(
    void* opaque,
    const void* bytes,
    std::size_t size) noexcept {
    auto* buffer = static_cast<BoundedDigestSinkBufferV1*>(opaque);
    if (buffer == nullptr || buffer->failed
        || (size != 0 && bytes == nullptr)
        || size > buffer->bytes.size()
        || buffer->size > buffer->bytes.size() - size) {
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

[[nodiscard]] inline BioseqCacheDigestSinkV2 make_digest_sink(
    BoundedDigestSinkBufferV1& buffer) noexcept {
    return BioseqCacheDigestSinkV2{
        .struct_size = sizeof(BioseqCacheDigestSinkV2),
        .opaque = &buffer,
        .append = &append_bounded_digest_bytes,
    };
}

[[nodiscard]] inline bool digest_bytes_equal(
    std::span<const std::byte> left,
    std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index) {
        difference |= static_cast<std::uint8_t>(left[index])
            ^ static_cast<std::uint8_t>(right[index]);
    }
    return difference == 0;
}

enum class CoverageBranchKindV1 : std::uint8_t {
    Drop = 0,
    Cached = 1,
    Residual = 2,
};

struct CoverageBranchChoiceV1 {
    CoverageBranchKindV1 kind = CoverageBranchKindV1::Drop;
    std::uint32_t source_index = 0;
};

/** Apply `P AND Pi AND earlier(Pj IS NOT TRUE)` using PostgreSQL 3VL. */
[[nodiscard]] constexpr CoverageBranchChoiceV1 choose_coverage_branch(
    SqlBool requested_predicate,
    std::span<const SqlBool> source_predicates,
    std::span<const bool> source_validity) noexcept {
    if (!passes_where(requested_predicate)) {
        return {};
    }
    for (std::size_t source = 0;
         source < source_validity.size();
         ++source) {
        if (!source_validity[source]
            || source >= source_predicates.size()
            || !passes_where(source_predicates[source])) {
            continue;
        }
        return CoverageBranchChoiceV1{
            .kind = CoverageBranchKindV1::Cached,
            .source_index = static_cast<std::uint32_t>(source),
        };
    }
    return CoverageBranchChoiceV1{
        .kind = CoverageBranchKindV1::Residual,
        .source_index = static_cast<std::uint32_t>(source_validity.size()),
    };
}

/** Whether a row read from source i belongs to i's disjoint branch. */
[[nodiscard]] constexpr bool source_owns_occurrence(
    std::size_t source_index,
    SqlBool requested_predicate,
    std::span<const SqlBool> source_predicates,
    std::span<const bool> source_validity) noexcept {
    const CoverageBranchChoiceV1 choice = choose_coverage_branch(
        requested_predicate, source_predicates, source_validity);
    return choice.kind == CoverageBranchKindV1::Cached
        && choice.source_index == source_index;
}

/** Keep self-join/binding identity in the occurrence equality contract. */
[[nodiscard]] constexpr bool occurrence_equal(
    const BioseqCacheOccurrenceV2& left,
    const BioseqCacheOccurrenceV2& right) noexcept {
    return left.struct_size >= sizeof(BioseqCacheOccurrenceV2)
        && right.struct_size >= sizeof(BioseqCacheOccurrenceV2)
        && left.rte_ordinal == right.rte_ordinal
        && left.logical_relation_oid == right.logical_relation_oid
        && left.leaf_relation_oid == right.leaf_relation_oid
        && left.identity_kind == right.identity_kind
        && left.reserved == 0
        && right.reserved == 0
        && left.row_identity == right.row_identity
        && left.occurrence_high == right.occurrence_high
        && left.occurrence_low == right.occurrence_low;
}

/** Storage supplied by the executor for validating one late-fetch callback. */
struct LateFetchSinkStateV1 {
    std::size_t reference_count = 0;
    std::size_t attribute_count = 0;
    std::span<std::uint8_t> terminal_status{};
    std::span<std::uint8_t> value_seen{};
    std::span<std::uintptr_t> values{};
    std::span<std::uint8_t> nulls{};
    bool malformed = false;
    bool any_reference_failure = false;
};

[[nodiscard]] inline bool initialize_late_fetch_sink_state(
    LateFetchSinkStateV1& state) noexcept {
    if (state.attribute_count != 0
        && state.reference_count
            > (std::numeric_limits<std::size_t>::max)()
                / state.attribute_count) {
        state.malformed = true;
        return false;
    }
    const std::size_t value_count =
        state.reference_count * state.attribute_count;
    if (state.terminal_status.size() < state.reference_count
        || state.value_seen.size() < value_count
        || state.values.size() < value_count
        || state.nulls.size() < value_count) {
        state.malformed = true;
        return false;
    }
    std::fill_n(
        state.terminal_status.begin(), state.reference_count,
        kLateFetchStatusUnsetV1);
    std::fill_n(state.value_seen.begin(), value_count, std::uint8_t{0});
    std::fill_n(state.values.begin(), value_count, std::uintptr_t{0});
    std::fill_n(state.nulls.begin(), value_count, std::uint8_t{0});
    state.malformed = false;
    state.any_reference_failure = false;
    return true;
}

[[nodiscard]] inline BioseqCacheStatusV2 late_fetch_emit_value(
    void* opaque,
    std::size_t reference_index,
    std::size_t requested_attribute_index,
    std::uintptr_t datum,
    bool is_null) noexcept {
    auto* state = static_cast<LateFetchSinkStateV1*>(opaque);
    if (state == nullptr || state->malformed
        || reference_index >= state->reference_count
        || requested_attribute_index >= state->attribute_count
        || state->terminal_status[reference_index]
            != kLateFetchStatusUnsetV1) {
        if (state != nullptr) {
            state->malformed = true;
        }
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    const std::size_t index =
        reference_index * state->attribute_count
        + requested_attribute_index;
    if (state->value_seen[index] != 0) {
        state->malformed = true;
        return BIOSEQ_CACHE_STATUS_ERROR;
    }
    state->value_seen[index] = 1;
    state->values[index] = datum;
    state->nulls[index] = is_null ? 1U : 0U;
    return BIOSEQ_CACHE_STATUS_OK;
}

inline void late_fetch_emit_reference_status(
    void* opaque,
    std::size_t reference_index,
    BioseqCacheStatusV2 status) noexcept {
    auto* state = static_cast<LateFetchSinkStateV1*>(opaque);
    if (state == nullptr || state->malformed
        || reference_index >= state->reference_count
        || state->terminal_status[reference_index]
            != kLateFetchStatusUnsetV1
        || (status != BIOSEQ_CACHE_STATUS_OK
            && status != BIOSEQ_CACHE_STATUS_UNSUPPORTED
            && status != BIOSEQ_CACHE_STATUS_ERROR)) {
        if (state != nullptr) {
            state->malformed = true;
        }
        return;
    }
    state->terminal_status[reference_index] =
        static_cast<std::uint8_t>(status);
    state->any_reference_failure =
        state->any_reference_failure || status != BIOSEQ_CACHE_STATUS_OK;
}

[[nodiscard]] inline BioseqCacheLateFetchSinkV2 make_late_fetch_sink(
    LateFetchSinkStateV1& state) noexcept {
    return BioseqCacheLateFetchSinkV2{
        .struct_size = sizeof(BioseqCacheLateFetchSinkV2),
        .opaque = &state,
        .emit_value = &late_fetch_emit_value,
        .emit_reference_status = &late_fetch_emit_reference_status,
    };
}

/** Require one terminal per reference and one value/null event per OK field. */
[[nodiscard]] inline bool late_fetch_complete_and_valid(
    LateFetchSinkStateV1& state) noexcept {
    if (state.malformed || state.any_reference_failure) {
        return false;
    }
    for (std::size_t reference = 0;
         reference < state.reference_count;
         ++reference) {
        if (state.terminal_status[reference]
            != static_cast<std::uint8_t>(BIOSEQ_CACHE_STATUS_OK)) {
            return false;
        }
        for (std::size_t attribute = 0;
             attribute < state.attribute_count;
             ++attribute) {
            const std::size_t index =
                reference * state.attribute_count + attribute;
            if (state.value_seen[index] == 0) {
                state.malformed = true;
                return false;
            }
        }
    }
    return true;
}

enum class CandidateReferenceKindV1 : std::uint8_t {
    None = 0,
    ProvisionalPin = 1,
    InFlightWaiter = 2,
    AdmissionReservation = 3,
};

using CandidateReleaseNoErrorV1 = void (*)(
    void* opaque,
    const CacheEntryId& entry_id,
    CandidateReferenceKindV1 reference_kind) noexcept;

/** Longjmp-safe record: no destructor participates in reference release. */
struct PlannerCandidateCleanupRecordV1 {
    PlannerCandidateCleanupRecordV1* next = nullptr;
    CacheEntryId entry_id{};
    CandidateReferenceKindV1 reference_kind =
        CandidateReferenceKindV1::None;
    CandidateReleaseNoErrorV1 release = nullptr;
    void* release_opaque = nullptr;
    bool active = false;
};

[[nodiscard]] inline bool activate_candidate_reference(
    PlannerCandidateCleanupRecordV1& record,
    const CacheEntryId& entry_id,
    CandidateReferenceKindV1 reference_kind,
    CandidateReleaseNoErrorV1 release,
    void* release_opaque) noexcept {
    if (record.active || reference_kind == CandidateReferenceKindV1::None
        || release == nullptr) {
        return false;
    }
    record.entry_id = entry_id;
    record.reference_kind = reference_kind;
    record.release = release;
    record.release_opaque = release_opaque;
    record.active = true;
    return true;
}

/** Idempotent and reentrancy-safe: clear the record before registry release. */
inline void release_candidate_reference_noerror(
    PlannerCandidateCleanupRecordV1& record) noexcept {
    if (!record.active) {
        return;
    }
    record.active = false;
    const CandidateReleaseNoErrorV1 release = record.release;
    void* const release_opaque = record.release_opaque;
    const CacheEntryId entry_id = record.entry_id;
    const CandidateReferenceKindV1 reference_kind =
        record.reference_kind;
    record.reference_kind = CandidateReferenceKindV1::None;
    record.release = nullptr;
    record.release_opaque = nullptr;
    if (release != nullptr) {
        release(
            release_opaque,
            entry_id,
            reference_kind);
    }
}

enum class ReusePreflightStateV1 : std::uint8_t {
    Empty = 0,
    Collecting = 1,
    Frozen = 2,
};

/** Gates remain observably false until every independent source is decided. */
struct ReuseGateFreezeV1 {
    std::array<std::uint8_t, kMaxSourcesPerCoverageNode> staged{};
    std::array<std::uint8_t, kMaxSourcesPerCoverageNode> published{};
    std::size_t source_count = 0;
    ReusePreflightStateV1 state = ReusePreflightStateV1::Empty;
};

[[nodiscard]] inline bool begin_gate_preflight(
    ReuseGateFreezeV1& gates,
    std::size_t source_count) noexcept {
    if (gates.state != ReusePreflightStateV1::Empty
        || source_count == 0
        || source_count > kMaxSourcesPerCoverageNode) {
        return false;
    }
    gates.staged.fill(0);
    gates.published.fill(0);
    gates.source_count = source_count;
    gates.state = ReusePreflightStateV1::Collecting;
    return true;
}

[[nodiscard]] inline bool stage_valid_source(
    ReuseGateFreezeV1& gates,
    std::size_t source_index) noexcept {
    if (gates.state != ReusePreflightStateV1::Collecting
        || source_index >= gates.source_count) {
        return false;
    }
    gates.staged[source_index] = 1;
    return true;
}

[[nodiscard]] inline bool freeze_source_decisions(
    ReuseGateFreezeV1& gates) noexcept {
    if (gates.state != ReusePreflightStateV1::Collecting) {
        return false;
    }
    gates.published = gates.staged;
    gates.state = ReusePreflightStateV1::Frozen;
    return true;
}

/** Fail open as one atomic all-false decision. */
inline void freeze_all_sources_invalid(ReuseGateFreezeV1& gates) noexcept {
    if (gates.state != ReusePreflightStateV1::Collecting) {
        return;
    }
    gates.staged.fill(0);
    gates.published.fill(0);
    gates.state = ReusePreflightStateV1::Frozen;
}

[[nodiscard]] constexpr bool source_gate_is_valid(
    const ReuseGateFreezeV1& gates,
    std::size_t source_index) noexcept {
    return gates.state == ReusePreflightStateV1::Frozen
        && source_index < gates.source_count
        && gates.published[source_index] != 0;
}

/** Wait only for a generation older than every generation owned locally. */
[[nodiscard]] constexpr bool claim_order_allows_wait(
    std::uint64_t candidate_claim_sequence,
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

static_assert(std::is_standard_layout_v<CoverageSecurityNamespaceV1>);
static_assert(std::is_trivially_copyable_v<CoverageSecurityNamespaceV1>);
static_assert(std::is_standard_layout_v<PlannerCandidateCleanupRecordV1>);

} // namespace bioseq::cache::pg::reuse
