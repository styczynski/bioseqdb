/**
 * @file common/cache_provider_api/index.cppm
 * @brief Versioned, module-native rendezvous ABI shared by cache providers.
 */
module;

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

export module bioseq.cache.provider_api;

export extern "C" {

struct CustomPath;
struct CustomScanMethods;
struct EState;
struct ExplainState;
struct Expr;
struct List;
struct Path;
struct Plan;
struct PlannerInfo;
struct Query;
struct RangeTblEntry;
struct RelOptInfo;
struct RelationData;
struct TupleTableSlot;

enum BioseqCacheStatusV1 {
    BIOSEQ_CACHE_STATUS_OK = 0,
    BIOSEQ_CACHE_STATUS_UNSUPPORTED = 1,
    BIOSEQ_CACHE_STATUS_ERROR = 2,
};

enum BioseqCacheProviderFlagsV1 {
    BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE = 1U << 0,
    BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE = 1U << 1,
    BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION = 1U << 2,
};

/**
 * Version of the fixed-width provider security namespace digest.
 *
 * A provider that has no provider-specific security state still writes this
 * version and leaves all digest bytes zero.  Version zero means the provider
 * omitted the contract and is rejected; it is not the provider-independent
 * sentinel.
 */
enum BioseqCacheProviderSecurityDigestFormatV1 {
    BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1 = 1,
};

struct BioseqCacheDigestSinkV1 {
    uint32_t struct_size;
    void* opaque;
    bool (*append)(void* opaque, const void* data, size_t size);
};

struct BioseqCachePhysicalIdentityV1 {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t length;
    uint32_t reserved;
    uint8_t bytes[32];
};

struct BioseqCacheQueryIntentV1 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint32_t flags;
    uint32_t reserved;
    List* provider_private_nodes;
};

struct BioseqCacheColumnSchemaV1 {
    uint32_t struct_size;
    uint32_t attno;
    uint64_t semantic_id_high;
    uint64_t semantic_id_low;
    uint32_t type_oid;
    int32_t typmod;
    uint32_t collation_oid;
    uint32_t flags;
};

struct BioseqCacheColumnMapEntryV1 {
    uint32_t struct_size;
    uint32_t consumer_attno;
    uint32_t source_attno;
    uint32_t flags;
    uint64_t semantic_id_high;
    uint64_t semantic_id_low;
    uint32_t type_oid;
    int32_t typmod;
    uint32_t collation_oid;
    uint32_t reserved;
};

struct BioseqCachePathDomainV1 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t compatibility_family_id;
    uint64_t capability_mask;
    Expr* coverage_predicate;
    const BioseqCacheColumnSchemaV1* boundary_columns;
    size_t boundary_column_count;
    const BioseqCacheColumnSchemaV1* lineage_columns;
    size_t lineage_column_count;
    const uint32_t* required_boundary_attnos;
    size_t required_boundary_attno_count;
    const uint32_t* required_lineage_attnos;
    size_t required_lineage_attno_count;
    List* dependency_nodes;
    List* provider_private_nodes;
    uint32_t provider_security_digest_format_version;
    uint32_t provider_security_flags;
    uint8_t provider_security_digest[32];
};

struct BioseqCachePathDescriptionV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t capability_mask;
    List* dependency_nodes;
    List* provider_private_nodes;
};

struct BioseqCacheReusableSourceV1 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t compatibility_family_id;
    uint64_t capability_mask;
    const BioseqCacheColumnSchemaV1* boundary_columns;
    size_t boundary_column_count;
    const BioseqCacheColumnSchemaV1* lineage_columns;
    size_t lineage_column_count;
    List* provider_private_nodes;
};

struct BioseqCacheSourceMappingV1 {
    uint32_t struct_size;
    uint32_t flags;
    const BioseqCacheColumnMapEntryV1* boundary_map;
    size_t boundary_map_count;
    const BioseqCacheColumnMapEntryV1* lineage_map;
    size_t lineage_map_count;
    List* provider_private_nodes;
};

struct BioseqCachePlanDigestRecipeV1 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t digest_format_version;
    uint32_t reserved;
    List* plan_domain_nodes;
    uint8_t expected_generation_digest[32];
};

struct BioseqCacheDecoratedPathV1 {
    uint32_t struct_size;
    uint32_t flags;
    Path* path;
    const BioseqCacheColumnSchemaV1* actual_boundary_columns;
    size_t actual_boundary_column_count;
    const BioseqCacheColumnSchemaV1* actual_lineage_columns;
    size_t actual_lineage_column_count;
    List* provider_private_nodes;
};

struct BioseqCacheOccurrenceV1 {
    uint32_t struct_size;
    uint32_t rte_ordinal;
    uint32_t logical_relation_oid;
    uint32_t leaf_relation_oid;
    uint32_t identity_kind;
    uint32_t reserved;
    uint64_t row_identity;
    uint64_t occurrence_high;
    uint64_t occurrence_low;
    uint64_t capability_mask;
};

struct BioseqCacheLateFetchSinkV1 {
    uint32_t struct_size;
    void* opaque;
    BioseqCacheStatusV1 (*emit_value)(
        void* opaque,
        size_t reference_index,
        size_t requested_attribute_index,
        uintptr_t datum,
        bool is_null);
    void (*emit_reference_status)(
        void* opaque,
        size_t reference_index,
        BioseqCacheStatusV1 status);
};

struct BioseqCacheExplainSinkV1 {
    uint32_t struct_size;
    void* opaque;
    void (*property_text)(void* opaque, const char* name, const char* value);
    void (*property_uint64)(void* opaque, const char* name, uint64_t value);
    void (*property_bool)(void* opaque, const char* name, bool value);
};

struct BioseqCacheProviderV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    uint32_t provider_id;
    uint32_t provider_flags;
    uint32_t reserved;
    const char* provider_name;
    const CustomScanMethods* provider_scan_methods;

    BioseqCacheStatusV1 (*analyze_query)(
        const Query* query,
        BioseqCacheQueryIntentV1* out);
    BioseqCacheStatusV1 (*describe_path)(
        PlannerInfo* root,
        RelOptInfo* rel,
        Path* provider_path,
        const BioseqCacheQueryIntentV1* intent,
        BioseqCachePathDomainV1* out);
    BioseqCacheStatusV1 (*append_domain_semantic_digest)(
        const BioseqCachePathDomainV1* domain,
        BioseqCacheDigestSinkV1* sink);
    BioseqCacheStatusV1 (*append_generation_semantic_digest)(
        const BioseqCachePathDomainV1* domain,
        BioseqCacheDigestSinkV1* sink);
    BioseqCacheStatusV1 (*append_runtime_generation_semantic_digest)(
        const List* plan_domain_nodes,
        EState* estate,
        BioseqCacheDigestSinkV1* sink);
    BioseqCacheStatusV1 (*map_reusable_source)(
        PlannerInfo* root,
        const BioseqCachePathDomainV1* consumer_domain,
        const BioseqCacheReusableSourceV1* source,
        BioseqCacheSourceMappingV1* out);
    BioseqCacheStatusV1 (*decorate_path)(
        PlannerInfo* root,
        RelOptInfo* rel,
        Path* provider_path,
        const BioseqCachePathDomainV1* domain,
        List* gated_qual_exprs,
        BioseqCacheDecoratedPathV1* out);
    BioseqCacheStatusV1 (*build_plan_digest_recipe)(
        PlannerInfo* root,
        const Plan* provider_plan,
        BioseqCachePlanDigestRecipeV1* out);
    BioseqCacheStatusV1 (*extract_occurrence)(
        List* plan_domain_nodes,
        TupleTableSlot* slot,
        BioseqCacheOccurrenceV1* out);
    BioseqCacheStatusV1 (*late_fetch_batch)(
        List* plan_domain_nodes,
        EState* estate,
        const BioseqCacheOccurrenceV1* occurrences,
        size_t occurrence_count,
        const int16_t* attribute_numbers,
        size_t attribute_count,
        BioseqCacheLateFetchSinkV1* sink);
    void (*explain_domain)(
        List* plan_domain_nodes,
        BioseqCacheExplainSinkV1* sink);
};

struct BioseqCachePlannerBridgeV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    void (*after_provider_rel_paths)(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const BioseqCacheProviderV1* provider);
};

struct BioseqCacheApiV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    bool (*register_provider)(const BioseqCacheProviderV1* provider);
    void (*unregister_provider)(uint32_t provider_id);
    void (*record_relation_write)(
        uint32_t database_oid,
        uint32_t relation_oid,
        const BioseqCachePhysicalIdentityV1* physical_identity);
    const BioseqCachePlannerBridgeV1* planner_bridge;
};

} // extern "C"

export namespace bioseq::cache {

inline constexpr std::uint32_t kProviderAbiVersion = 1;
inline constexpr std::size_t kProviderDigestBytes = 32;

using BioseqCacheStatusV1 = ::BioseqCacheStatusV1;
using BioseqCacheProviderFlagsV1 = ::BioseqCacheProviderFlagsV1;
using BioseqCacheProviderSecurityDigestFormatV1 =
    ::BioseqCacheProviderSecurityDigestFormatV1;
using BioseqCacheDigestSinkV1 = ::BioseqCacheDigestSinkV1;
using BioseqCachePhysicalIdentityV1 = ::BioseqCachePhysicalIdentityV1;
using BioseqCacheQueryIntentV1 = ::BioseqCacheQueryIntentV1;
using BioseqCacheColumnSchemaV1 = ::BioseqCacheColumnSchemaV1;
using BioseqCacheColumnMapEntryV1 = ::BioseqCacheColumnMapEntryV1;
using BioseqCachePathDomainV1 = ::BioseqCachePathDomainV1;
using BioseqCachePathDescriptionV1 = ::BioseqCachePathDescriptionV1;
using BioseqCacheReusableSourceV1 = ::BioseqCacheReusableSourceV1;
using BioseqCacheSourceMappingV1 = ::BioseqCacheSourceMappingV1;
using BioseqCachePlanDigestRecipeV1 = ::BioseqCachePlanDigestRecipeV1;
using BioseqCacheDecoratedPathV1 = ::BioseqCacheDecoratedPathV1;
using BioseqCacheOccurrenceV1 = ::BioseqCacheOccurrenceV1;
using BioseqCacheLateFetchSinkV1 = ::BioseqCacheLateFetchSinkV1;
using BioseqCacheExplainSinkV1 = ::BioseqCacheExplainSinkV1;
using BioseqCacheProviderV1 = ::BioseqCacheProviderV1;
using BioseqCachePlannerBridgeV1 = ::BioseqCachePlannerBridgeV1;
using BioseqCacheApiV1 = ::BioseqCacheApiV1;

/** Validate the common prefix of a version-one public ABI object. */
template <class PublicStruct>
[[nodiscard]] constexpr bool provider_abi_prefix_valid(
    const PublicStruct* value,
    std::uint32_t abi_version,
    std::uint32_t postgres_major) noexcept {
    return value != nullptr
        && value->struct_size >= sizeof(PublicStruct)
        && value->abi_version == kProviderAbiVersion
        && value->postgres_major == postgres_major
        && abi_version == kProviderAbiVersion;
}

static_assert(std::is_standard_layout_v<BioseqCacheProviderV1>);
static_assert(std::is_trivially_copyable_v<BioseqCacheProviderV1>);
static_assert(std::is_standard_layout_v<BioseqCacheApiV1>);
static_assert(std::is_trivially_copyable_v<BioseqCacheApiV1>);

} // namespace bioseq::cache
