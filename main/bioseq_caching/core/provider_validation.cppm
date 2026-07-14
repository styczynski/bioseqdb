/**
 * @file bioseq_caching/core/provider_validation.cppm
 * @brief No-Postgres validation for provider ABI schemas, maps, and keys.
 */
module;

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

export module bioseq.cache.core:provider_validation;
import bioseq.cache.provider_api;
import :checked_math;
import :wire;

export namespace bioseq::cache {

enum class SourceMapError : std::uint16_t {
    NullPointer,
    CountLimit,
    StructSize,
    InvalidAttno,
    DuplicateAttno,
    RequiredAttnoMissing,
    MapCountMismatch,
    TypeMismatch,
    TypmodMismatch,
    CollationMismatch,
    SemanticIdMismatch,
    IllegalRepeatedSource,
    CapabilityMissing,
};

/** One copied consumer/source schema plus its ordered required attributes. */
struct ProviderSchemaView {
    const BioseqCacheColumnSchemaV1* columns = nullptr;
    std::size_t column_count = 0;
    const std::uint32_t* required_attnos = nullptr;
    std::size_t required_attno_count = 0;
};

/** Boundary and lineage halves of one provider contract. */
struct ProviderContractView {
    ProviderSchemaView boundary{};
    ProviderSchemaView lineage{};
    std::uint64_t capability_mask = 0;
};

/** Locate one declared schema field by its one-based provider attno. */
[[nodiscard]] inline const BioseqCacheColumnSchemaV1* find_provider_column(
    const ProviderSchemaView& schema,
    std::uint32_t attno) noexcept {
    if (schema.columns == nullptr) {
        return nullptr;
    }
    for (std::size_t index = 0; index < schema.column_count; ++index) {
        if (schema.columns[index].attno == attno) {
            return &schema.columns[index];
        }
    }
    return nullptr;
}

/** Validate pointer/count/attno invariants for one copied provider schema. */
[[nodiscard]] inline std::expected<void, SourceMapError>
validate_provider_schema(const ProviderSchemaView& schema) noexcept {
    if (schema.column_count > kMaxProviderBoundaryColumns
        || schema.required_attno_count > kMaxProviderBoundaryColumns) {
        return std::unexpected(SourceMapError::CountLimit);
    }
    if ((schema.column_count != 0 && schema.columns == nullptr)
        || (schema.required_attno_count != 0
            && schema.required_attnos == nullptr)) {
        return std::unexpected(SourceMapError::NullPointer);
    }
    for (std::size_t index = 0; index < schema.column_count; ++index) {
        const auto& column = schema.columns[index];
        if (column.struct_size < sizeof(BioseqCacheColumnSchemaV1)
            || column.attno == 0) {
            return std::unexpected(
                column.struct_size < sizeof(BioseqCacheColumnSchemaV1)
                    ? SourceMapError::StructSize
                    : SourceMapError::InvalidAttno);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (schema.columns[prior].attno == column.attno) {
                return std::unexpected(SourceMapError::DuplicateAttno);
            }
        }
    }
    for (std::size_t index = 0;
         index < schema.required_attno_count;
         ++index) {
        const std::uint32_t attno = schema.required_attnos[index];
        if (attno == 0 || find_provider_column(schema, attno) == nullptr) {
            return std::unexpected(SourceMapError::InvalidAttno);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (schema.required_attnos[prior] == attno) {
                return std::unexpected(SourceMapError::DuplicateAttno);
            }
        }
    }
    return {};
}

/** Compare the provider meaning and complete SQL metadata of two columns. */
[[nodiscard]] constexpr std::expected<void, SourceMapError>
validate_column_meaning(
    const BioseqCacheColumnSchemaV1& consumer,
    const BioseqCacheColumnSchemaV1& source,
    const BioseqCacheColumnMapEntryV1& mapping) noexcept {
    if (consumer.type_oid != source.type_oid
        || mapping.type_oid != consumer.type_oid) {
        return std::unexpected(SourceMapError::TypeMismatch);
    }
    if (consumer.typmod != source.typmod
        || mapping.typmod != consumer.typmod) {
        return std::unexpected(SourceMapError::TypmodMismatch);
    }
    if (consumer.collation_oid != source.collation_oid
        || mapping.collation_oid != consumer.collation_oid) {
        return std::unexpected(SourceMapError::CollationMismatch);
    }
    if (consumer.semantic_id_high != source.semantic_id_high
        || consumer.semantic_id_low != source.semantic_id_low
        || mapping.semantic_id_high != consumer.semantic_id_high
        || mapping.semantic_id_low != consumer.semantic_id_low) {
        return std::unexpected(SourceMapError::SemanticIdMismatch);
    }
    return {};
}

/** Validate one boundary/lineage mapping against copied source and consumer POD. */
[[nodiscard]] inline std::expected<void, SourceMapError> validate_map_half(
    const ProviderSchemaView& consumer,
    const ProviderSchemaView& source,
    const BioseqCacheColumnMapEntryV1* mappings,
    std::size_t mapping_count) noexcept {
    if (mapping_count != consumer.required_attno_count) {
        return std::unexpected(SourceMapError::MapCountMismatch);
    }
    if (mapping_count != 0 && mappings == nullptr) {
        return std::unexpected(SourceMapError::NullPointer);
    }
    for (std::size_t index = 0; index < mapping_count; ++index) {
        const auto& mapping = mappings[index];
        if (mapping.struct_size < sizeof(BioseqCacheColumnMapEntryV1)) {
            return std::unexpected(SourceMapError::StructSize);
        }
        if (mapping.consumer_attno == 0 || mapping.source_attno == 0) {
            return std::unexpected(SourceMapError::InvalidAttno);
        }
        bool consumer_required = false;
        for (std::size_t required = 0;
             required < consumer.required_attno_count;
             ++required) {
            consumer_required = consumer_required
                || consumer.required_attnos[required]
                    == mapping.consumer_attno;
        }
        if (!consumer_required) {
            return std::unexpected(SourceMapError::RequiredAttnoMissing);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mappings[prior].consumer_attno == mapping.consumer_attno) {
                return std::unexpected(SourceMapError::DuplicateAttno);
            }
        }
        const auto* consumer_column =
            find_provider_column(consumer, mapping.consumer_attno);
        const auto* source_column =
            find_provider_column(source, mapping.source_attno);
        if (consumer_column == nullptr || source_column == nullptr) {
            return std::unexpected(SourceMapError::InvalidAttno);
        }
        const auto meaning = validate_column_meaning(
            *consumer_column, *source_column, mapping);
        if (!meaning.has_value()) {
            return meaning;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mappings[prior].source_attno != mapping.source_attno) {
                continue;
            }
            const auto* prior_consumer =
                find_provider_column(consumer, mappings[prior].consumer_attno);
            if (prior_consumer == nullptr
                || prior_consumer->semantic_id_high
                    != consumer_column->semantic_id_high
                || prior_consumer->semantic_id_low
                    != consumer_column->semantic_id_low
                || prior_consumer->type_oid != consumer_column->type_oid
                || prior_consumer->typmod != consumer_column->typmod
                || prior_consumer->collation_oid
                    != consumer_column->collation_oid) {
                return std::unexpected(SourceMapError::IllegalRepeatedSource);
            }
        }
    }
    return {};
}

/** Validate both map halves and capability sufficiency. */
[[nodiscard]] inline std::expected<void, SourceMapError> validate_source_map(
    const ProviderContractView& consumer,
    const ProviderContractView& source,
    const BioseqCacheSourceMappingV1& mapping) noexcept {
    const auto consumer_boundary = validate_provider_schema(consumer.boundary);
    const auto consumer_lineage = validate_provider_schema(consumer.lineage);
    const auto source_boundary = validate_provider_schema(source.boundary);
    const auto source_lineage = validate_provider_schema(source.lineage);
    if (!consumer_boundary.has_value()) {
        return consumer_boundary;
    }
    if (!consumer_lineage.has_value()) {
        return consumer_lineage;
    }
    if (!source_boundary.has_value()) {
        return source_boundary;
    }
    if (!source_lineage.has_value()) {
        return source_lineage;
    }
    if ((source.capability_mask & consumer.capability_mask)
        != consumer.capability_mask) {
        return std::unexpected(SourceMapError::CapabilityMissing);
    }
    const auto boundary = validate_map_half(
        consumer.boundary,
        source.boundary,
        mapping.boundary_map,
        mapping.boundary_map_count);
    if (!boundary.has_value()) {
        return boundary;
    }
    return validate_map_half(
        consumer.lineage,
        source.lineage,
        mapping.lineage_map,
        mapping.lineage_map_count);
}

/** Return whether a producer capability mask covers every required bit. */
[[nodiscard]] constexpr bool capabilities_satisfy(
    std::uint64_t producer,
    std::uint64_t required) noexcept {
    return (producer & required) == required;
}

/** Compare only the length-delimited physical identity contract. */
[[nodiscard]] constexpr bool physical_identity_equal(
    const PhysicalIdentityV1& left,
    const PhysicalIdentityV1& right) noexcept {
    if (left.kind != right.kind
        || left.length != right.length
        || left.length > left.bytes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.length; ++index) {
        if (left.bytes[index] != right.bytes[index]) {
            return false;
        }
    }
    return true;
}

/** One digest-bucket candidate retaining the sole equality proof. */
struct CanonicalCacheKey {
    CacheDigest256 digest{};
    std::vector<std::byte> bytes;
};

/** Require digest, length, and every canonical byte to share an entry. */
[[nodiscard]] inline bool collision_checked_key_equal(
    const CanonicalCacheKey& left,
    const CanonicalCacheKey& right) noexcept {
    return left.digest == right.digest && left.bytes == right.bytes;
}

/** Prefix provider payloads with unambiguous provider ID and byte length. */
inline void append_provider_digest_frame(
    std::vector<std::byte>& destination,
    std::uint32_t provider_id,
    std::span<const std::byte> payload) {
    append_little_endian(destination, provider_id);
    append_little_endian(
        destination, static_cast<std::uint64_t>(payload.size()));
    destination.insert(destination.end(), payload.begin(), payload.end());
}

/** Validate bounded runtime recipe shape before any provider callback. */
[[nodiscard]] constexpr bool provider_recipe_shape_valid(
    std::uint32_t recipe_count,
    std::uint64_t total_bytes,
    std::uint32_t node_count) noexcept {
    return recipe_count <= kMaxProviderRecipesPerExactPlan
        && total_bytes <= kMaxProviderRecipeBytes
        && node_count <= kMaxProviderBoundaryColumns;
}

/** Runtime/captured semantic recipes must reproduce the exact expected bytes. */
[[nodiscard]] constexpr bool runtime_digest_matches(
    const CacheDigest256& expected,
    const CacheDigest256& actual) noexcept {
    return expected == actual;
}

/** Validate callback availability implied by provider flags. */
[[nodiscard]] inline bool provider_registration_valid(
    const BioseqCacheProviderV1* provider,
    std::uint32_t postgres_major) noexcept {
    if (provider == nullptr
        || provider->struct_size < sizeof(BioseqCacheProviderV1)
        || provider->abi_version != kProviderAbiVersion
        || provider->postgres_major != postgres_major
        || provider->provider_id == 0
        || provider->provider_name == nullptr
        || provider->analyze_query == nullptr
        || provider->describe_path == nullptr
        || provider->append_generation_semantic_digest == nullptr
        || provider->build_plan_digest_recipe == nullptr
        || provider->append_runtime_generation_semantic_digest == nullptr) {
        return false;
    }
    const bool reusable =
        (provider->provider_flags
         & BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE) != 0;
    if (reusable
        && (provider->append_domain_semantic_digest == nullptr
            || provider->map_reusable_source == nullptr
            || provider->decorate_path == nullptr
            || provider->extract_occurrence == nullptr
            || provider->late_fetch_batch == nullptr)) {
        return false;
    }
    const bool source_elision =
        (provider->provider_flags
         & BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE) != 0;
    return !source_elision || provider->provider_scan_methods != nullptr;
}

/** Preserve self-join binding identity in addition to physical row identity. */
[[nodiscard]] constexpr bool same_base_occurrence(
    const BaseRowReferenceV1& left,
    const BaseRowReferenceV1& right) noexcept {
    return left.rte_ordinal == right.rte_ordinal
        && left.logical_rel_oid == right.logical_rel_oid
        && left.leaf_rel_oid == right.leaf_rel_oid
        && physical_identity_equal(
            left.physical_identity, right.physical_identity)
        && left.identity_kind == right.identity_kind
        && left.block_number == right.block_number
        && left.offset_number == right.offset_number
        && left.row_number_or_opaque == right.row_number_or_opaque;
}

/** Sort and deduplicate dependency lock partitions, never raw keys alone. */
template <class PartitionFunction>
[[nodiscard]] std::vector<std::uint32_t> canonical_dependency_partitions(
    std::span<const DependencyKey> dependencies,
    std::uint32_t partition_count,
    PartitionFunction&& partition_for) {
    std::vector<std::uint32_t> partitions;
    if (partition_count == 0) {
        return partitions;
    }
    partitions.reserve(dependencies.size());
    for (const DependencyKey& key : dependencies) {
        partitions.push_back(
            partition_for(key) % partition_count);
    }
    std::sort(partitions.begin(), partitions.end());
    partitions.erase(
        std::unique(partitions.begin(), partitions.end()), partitions.end());
    return partitions;
}

enum class SemanticSettingClass : std::uint8_t {
    PlanChoice = 1,
    CapturedResultSemantic = 2,
    ProjectionCapability = 4,
    CacheOnly = 8,
};

struct SemanticSettingRecord {
    std::uint32_t setting_id = 0;
    std::uint8_t class_mask = 0;
};

/** Require every result-affecting setting to appear exactly once in one class. */
[[nodiscard]] inline bool semantic_setting_inventory_complete(
    std::span<const std::uint32_t> required_setting_ids,
    std::span<const SemanticSettingRecord> records) noexcept {
    for (const std::uint32_t required : required_setting_ids) {
        std::size_t matches = 0;
        for (const SemanticSettingRecord& record : records) {
            if (record.setting_id == required
                && std::has_single_bit(record.class_mask)) {
                ++matches;
            }
        }
        if (matches != 1) {
            return false;
        }
    }
    return true;
}

} // namespace bioseq::cache
