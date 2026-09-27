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

enum class source_map_error : std::uint16_t {
    null_pointer,
    count_limit,
    struct_size,
    invalid_attno,
    duplicate_attno,
    required_attno_missing,
    map_count_mismatch,
    type_mismatch,
    typmod_mismatch,
    collation_mismatch,
    semantic_id_mismatch,
    illegal_repeated_source,
    capability_missing,
    invalid_flags,
};

/** One copied consumer/source schema plus its ordered required attributes. */
struct provider_schema_view {
    const bioseq_cache_column_schema* columns = nullptr;
    std::size_t column_count = 0;
    const std::uint32_t* required_attnos = nullptr;
    std::size_t required_attno_count = 0;
};

/** Boundary and lineage halves of one provider contract. */
struct provider_contract_view {
    provider_schema_view boundary{};
    provider_schema_view lineage{};
    std::uint64_t capability_mask = 0;
};

/** Locate one declared schema field by its one-based provider attno. */
[[nodiscard]] inline const bioseq_cache_column_schema*
find_provider_column(const provider_schema_view& schema, std::uint32_t attno) noexcept {
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
[[nodiscard]] inline std::expected<void, source_map_error>
validate_provider_schema(const provider_schema_view& schema) noexcept {
    if (schema.column_count > k_maximum_provider_boundary_columns ||
        schema.required_attno_count > k_maximum_provider_boundary_columns) {
        return std::unexpected(source_map_error::count_limit);
    }
    if ((schema.column_count != 0 && schema.columns == nullptr) ||
        (schema.required_attno_count != 0 && schema.required_attnos == nullptr)) {
        return std::unexpected(source_map_error::null_pointer);
    }
    for (std::size_t index = 0; index < schema.column_count; ++index) {
        const auto& column = schema.columns[index];
        if (column.struct_size < sizeof(bioseq_cache_column_schema) || column.attno == 0 ||
            column.attno > schema.column_count) {
            return std::unexpected(column.struct_size < sizeof(bioseq_cache_column_schema)
                                       ? source_map_error::struct_size
                                       : source_map_error::invalid_attno);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (schema.columns[prior].attno == column.attno) {
                return std::unexpected(source_map_error::duplicate_attno);
            }
            const auto& previous = schema.columns[prior];
            const bool same_semantic_id = previous.semantic_id_high == column.semantic_id_high &&
                                          previous.semantic_id_low == column.semantic_id_low;
            if (same_semantic_id &&
                (previous.type_oid != column.type_oid || previous.typmod != column.typmod ||
                 previous.collation_oid != column.collation_oid ||
                 previous.flags != column.flags)) {
                return std::unexpected(source_map_error::semantic_id_mismatch);
            }
        }
    }
    for (std::size_t index = 0; index < schema.required_attno_count; ++index) {
        const std::uint32_t attno = schema.required_attnos[index];
        if (attno == 0 || find_provider_column(schema, attno) == nullptr) {
            return std::unexpected(source_map_error::invalid_attno);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (schema.required_attnos[prior] == attno) {
                return std::unexpected(source_map_error::duplicate_attno);
            }
        }
    }
    return {};
}

/** Compare the provider meaning and complete SQL metadata of two columns. */
[[nodiscard]] constexpr std::expected<void, source_map_error>
validate_column_meaning(const bioseq_cache_column_schema& consumer,
                        const bioseq_cache_column_schema& source,
                        const bioseq_cache_column_map_entry& mapping) noexcept {
    if (consumer.type_oid != source.type_oid || mapping.type_oid != consumer.type_oid) {
        return std::unexpected(source_map_error::type_mismatch);
    }
    if (consumer.typmod != source.typmod || mapping.typmod != consumer.typmod) {
        return std::unexpected(source_map_error::typmod_mismatch);
    }
    if (consumer.collation_oid != source.collation_oid ||
        mapping.collation_oid != consumer.collation_oid) {
        return std::unexpected(source_map_error::collation_mismatch);
    }
    if (consumer.semantic_id_high != source.semantic_id_high ||
        consumer.semantic_id_low != source.semantic_id_low ||
        mapping.semantic_id_high != consumer.semantic_id_high ||
        mapping.semantic_id_low != consumer.semantic_id_low) {
        return std::unexpected(source_map_error::semantic_id_mismatch);
    }
    return {};
}

/** Validate one boundary/lineage mapping against copied source and consumer POD. */
[[nodiscard]] inline std::expected<void, source_map_error>
validate_map_half(const provider_schema_view& consumer,
                  const provider_schema_view& source,
                  const bioseq_cache_column_map_entry* mappings,
                  std::size_t mapping_count) noexcept {
    if (mapping_count != consumer.required_attno_count) {
        return std::unexpected(source_map_error::map_count_mismatch);
    }
    if (mapping_count != 0 && mappings == nullptr) {
        return std::unexpected(source_map_error::null_pointer);
    }
    for (std::size_t index = 0; index < mapping_count; ++index) {
        const auto& mapping = mappings[index];
        if (mapping.struct_size < sizeof(bioseq_cache_column_map_entry)) {
            return std::unexpected(source_map_error::struct_size);
        }
        if (mapping.flags != 0 || mapping.reserved != 0) {
            return std::unexpected(source_map_error::invalid_flags);
        }
        if (mapping.consumer_attno == 0 || mapping.source_attno == 0) {
            return std::unexpected(source_map_error::invalid_attno);
        }
        bool consumer_required = false;
        for (std::size_t required = 0; required < consumer.required_attno_count; ++required) {
            consumer_required =
                consumer_required || consumer.required_attnos[required] == mapping.consumer_attno;
        }
        if (!consumer_required) {
            return std::unexpected(source_map_error::required_attno_missing);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mappings[prior].consumer_attno == mapping.consumer_attno) {
                return std::unexpected(source_map_error::duplicate_attno);
            }
        }
        const auto* consumer_column = find_provider_column(consumer, mapping.consumer_attno);
        const auto* source_column = find_provider_column(source, mapping.source_attno);
        if (consumer_column == nullptr || source_column == nullptr) {
            return std::unexpected(source_map_error::invalid_attno);
        }
        const auto meaning = validate_column_meaning(*consumer_column, *source_column, mapping);
        if (!meaning.has_value()) {
            return meaning;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (mappings[prior].source_attno != mapping.source_attno) {
                continue;
            }
            const auto* prior_consumer =
                find_provider_column(consumer, mappings[prior].consumer_attno);
            if (prior_consumer == nullptr ||
                prior_consumer->semantic_id_high != consumer_column->semantic_id_high ||
                prior_consumer->semantic_id_low != consumer_column->semantic_id_low ||
                prior_consumer->type_oid != consumer_column->type_oid ||
                prior_consumer->typmod != consumer_column->typmod ||
                prior_consumer->collation_oid != consumer_column->collation_oid) {
                return std::unexpected(source_map_error::illegal_repeated_source);
            }
        }
    }
    return {};
}

/** Validate both map halves and capability sufficiency. */
[[nodiscard]] inline std::expected<void, source_map_error>
validate_source_map(const provider_contract_view& consumer,
                    const provider_contract_view& source,
                    const bioseq_cache_source_mapping& mapping) noexcept {
    if (mapping.struct_size < sizeof(bioseq_cache_source_mapping)) {
        return std::unexpected(source_map_error::struct_size);
    }
    if (mapping.flags != 0) {
        return std::unexpected(source_map_error::invalid_flags);
    }
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
    if ((source.capability_mask & consumer.capability_mask) != consumer.capability_mask) {
        return std::unexpected(source_map_error::capability_missing);
    }
    const auto boundary = validate_map_half(
        consumer.boundary, source.boundary, mapping.boundary_map, mapping.boundary_map_count);
    if (!boundary.has_value()) {
        return boundary;
    }
    return validate_map_half(
        consumer.lineage, source.lineage, mapping.lineage_map, mapping.lineage_map_count);
}

/** Return whether a producer capability mask covers every required bit. */
[[nodiscard]] constexpr bool capabilities_satisfy(std::uint64_t producer,
                                                  std::uint64_t required) noexcept {
    return (producer & required) == required;
}

/** Compare only the length-delimited physical identity contract. */
[[nodiscard]] constexpr bool physical_identity_equal(const physical_identity& left,
                                                     const physical_identity& right) noexcept {
    if (left.kind != right.kind || left.length != right.length || left.length > left.bytes.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.length; ++index) {
        if (left.bytes[index] != right.bytes[index]) {
            return false;
        }
    }
    return true;
}

/** One digest-bucket candidate retaining the canonical equality evidence. */
struct canonical_cache_key {
    cache_digest_256 digest{};
    std::vector<std::byte> bytes;
};

/** Require digest, length, and every canonical byte to share an entry. */
[[nodiscard]] inline bool collision_checked_key_equal(const canonical_cache_key& left,
                                                      const canonical_cache_key& right) noexcept {
    return left.digest == right.digest && left.bytes == right.bytes;
}

/** Prefix provider payloads with unambiguous provider ID and byte length. */
inline void append_provider_digest_frame(std::vector<std::byte>& destination,
                                         std::uint32_t provider_id,
                                         std::span<const std::byte> payload) {
    append_little_endian(destination, provider_id);
    append_little_endian(destination, static_cast<std::uint64_t>(payload.size()));
    destination.insert(destination.end(), payload.begin(), payload.end());
}

/** Validate bounded runtime recipe shape before any provider callback. */
[[nodiscard]] constexpr bool provider_recipe_shape_valid(std::uint32_t recipe_count,
                                                         std::uint64_t total_bytes,
                                                         std::uint32_t node_count) noexcept {
    return recipe_count <= k_maximum_provider_recipes_per_exact_plan &&
           total_bytes <= k_maximum_provider_recipe_bytes &&
           node_count <= k_maximum_provider_boundary_columns;
}

/** Runtime/captured semantic recipes must reproduce the exact expected bytes. */
[[nodiscard]] constexpr bool runtime_digest_matches(const cache_digest_256& expected,
                                                    const cache_digest_256& actual) noexcept {
    return expected == actual;
}

/** Validate callback availability implied by provider flags. */
[[nodiscard]] inline bool provider_registration_valid(const bioseq_cache_provider* provider,
                                                      std::uint32_t postgres_major) noexcept {
    if (provider == nullptr || provider->struct_size < sizeof(bioseq_cache_provider) ||
        provider->abi_revision != k_provider_abi_revision ||
        provider->postgres_major != postgres_major || provider->provider_id == 0 ||
        provider->reserved != 0 || provider->provider_name == nullptr ||
        provider->provider_scan_methods == nullptr || provider->analyze_query == nullptr ||
        provider->describe_path == nullptr ||
        provider->append_generation_semantic_digest == nullptr ||
        provider->build_plan_digest_recipe == nullptr ||
        provider->append_runtime_generation_semantic_digest == nullptr) {
        return false;
    }
    constexpr std::uint32_t known_flags = bioseq_cache_provider_exact_source_elision_safe |
                                          bioseq_cache_provider_reusable_coverage |
                                          bioseq_cache_provider_complete_write_observation;
    if ((provider->provider_flags & ~known_flags) != 0) {
        return false;
    }
    const bool reusable = (provider->provider_flags & bioseq_cache_provider_reusable_coverage) != 0;
    if (reusable &&
        (provider->append_domain_semantic_digest == nullptr ||
         provider->map_reusable_source == nullptr || provider->decorate_path == nullptr ||
         provider->extract_occurrence == nullptr || provider->late_fetch_batch == nullptr)) {
        return false;
    }
    return true;
}

/** Preserve self-join binding identity in addition to physical row identity. */
[[nodiscard]] constexpr bool same_base_occurrence(const base_row_reference& left,
                                                  const base_row_reference& right) noexcept {
    return left.rte_ordinal == right.rte_ordinal && left.logical_rel_oid == right.logical_rel_oid &&
           left.leaf_rel_oid == right.leaf_rel_oid &&
           physical_identity_equal(left.physical_identity, right.physical_identity) &&
           left.identity_kind == right.identity_kind && left.block_number == right.block_number &&
           left.offset_number == right.offset_number &&
           left.row_number_or_opaque == right.row_number_or_opaque &&
           left.xmin_identity == right.xmin_identity;
}

/** Sort and deduplicate dependency lock partitions, never raw keys alone. */
template <class p_partition_function>
[[nodiscard]] std::vector<std::uint32_t>
canonical_dependency_partitions(std::span<const dependency_key> dependencies,
                                std::uint32_t partition_count,
                                p_partition_function&& partition_for) {
    std::vector<std::uint32_t> partitions;
    if (partition_count == 0) {
        return partitions;
    }
    partitions.reserve(dependencies.size());
    for (const dependency_key& key : dependencies) {
        partitions.push_back(partition_for(key) % partition_count);
    }
    std::sort(partitions.begin(), partitions.end());
    partitions.erase(std::unique(partitions.begin(), partitions.end()), partitions.end());
    return partitions;
}

enum class semantic_setting_class : std::uint8_t {
    plan_choice = 1,
    captured_result_semantic = 2,
    projection_capability = 4,
    cache_only = 8,
};

struct semantic_setting_record {
    std::uint32_t setting_id = 0;
    std::uint8_t class_mask = 0;
};

/** Accept only one of the four semantic setting classes. */
[[nodiscard]] constexpr bool known_semantic_setting_class(std::uint8_t class_mask) noexcept {
    return class_mask == static_cast<std::uint8_t>(semantic_setting_class::plan_choice) ||
           class_mask ==
               static_cast<std::uint8_t>(semantic_setting_class::captured_result_semantic) ||
           class_mask == static_cast<std::uint8_t>(semantic_setting_class::projection_capability) ||
           class_mask == static_cast<std::uint8_t>(semantic_setting_class::cache_only);
}

/** Require every result-affecting setting to appear exactly once in one class. */
[[nodiscard]] inline bool
semantic_setting_inventory_complete(std::span<const std::uint32_t> required_setting_ids,
                                    std::span<const semantic_setting_record> records) noexcept {
    for (std::size_t required_index = 0; required_index < required_setting_ids.size();
         ++required_index) {
        const std::uint32_t required = required_setting_ids[required_index];
        for (std::size_t prior = 0; prior < required_index; ++prior) {
            if (required_setting_ids[prior] == required) {
                return false;
            }
        }
        std::size_t matches = 0;
        for (const semantic_setting_record& record : records) {
            if (record.setting_id != required) {
                continue;
            }
            ++matches;
            if (!known_semantic_setting_class(record.class_mask)) {
                return false;
            }
        }
        if (matches != 1) {
            return false;
        }
    }
    return true;
}

} // namespace bioseq::cache
