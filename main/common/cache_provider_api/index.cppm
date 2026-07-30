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

enum BioseqCacheStatusV2 : uint32_t {
    BIOSEQ_CACHE_STATUS_OK = 0,
    BIOSEQ_CACHE_STATUS_UNSUPPORTED = 1,
    BIOSEQ_CACHE_STATUS_ERROR = 2,
};

enum BioseqCacheProviderFlagsV2 : uint32_t {
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
enum BioseqCacheProviderSecurityDigestFormatV2 : uint32_t {
    BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1 = 1,
};

struct BioseqCacheDigestSinkV2 {
    uint32_t struct_size;
    void* opaque;
    bool (*append)(void* opaque, const void* data, size_t size) noexcept;
};

struct BioseqCachePhysicalIdentityV2 {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t length;
    uint32_t reserved;
    uint8_t bytes[32];
};

struct BioseqCacheQueryIntentV2 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint32_t flags;
    uint32_t reserved;
    List* provider_private_nodes;
};

struct BioseqCacheColumnSchemaV2 {
    uint32_t struct_size;
    uint32_t attno;
    uint64_t semantic_id_high;
    uint64_t semantic_id_low;
    uint32_t type_oid;
    int32_t typmod;
    uint32_t collation_oid;
    uint32_t flags;
};

struct BioseqCacheColumnMapEntryV2 {
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

struct BioseqCachePathDomainV2 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t compatibility_family_id;
    uint64_t capability_mask;
    Expr* coverage_predicate;
    const BioseqCacheColumnSchemaV2* boundary_columns;
    size_t boundary_column_count;
    const BioseqCacheColumnSchemaV2* lineage_columns;
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

struct BioseqCachePathDescriptionV2 {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t capability_mask;
    List* dependency_nodes;
    List* provider_private_nodes;
};

struct BioseqCacheReusableSourceV2 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t compatibility_family_id;
    uint64_t capability_mask;
    const BioseqCacheColumnSchemaV2* boundary_columns;
    size_t boundary_column_count;
    const BioseqCacheColumnSchemaV2* lineage_columns;
    size_t lineage_column_count;
    List* provider_private_nodes;
};

struct BioseqCacheSourceMappingV2 {
    uint32_t struct_size;
    uint32_t flags;
    const BioseqCacheColumnMapEntryV2* boundary_map;
    size_t boundary_map_count;
    const BioseqCacheColumnMapEntryV2* lineage_map;
    size_t lineage_map_count;
    List* provider_private_nodes;
};

struct BioseqCachePlanDigestRecipeV2 {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t digest_format_version;
    uint32_t reserved;
    List* plan_domain_nodes;
    uint8_t expected_generation_digest[32];
};

struct BioseqCacheDecoratedPathV2 {
    uint32_t struct_size;
    uint32_t flags;
    Path* path;
    const BioseqCacheColumnSchemaV2* actual_boundary_columns;
    size_t actual_boundary_column_count;
    const BioseqCacheColumnSchemaV2* actual_lineage_columns;
    size_t actual_lineage_column_count;
    List* provider_private_nodes;
};

struct BioseqCacheOccurrenceV2 {
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

struct BioseqCacheLateFetchSinkV2 {
    uint32_t struct_size;
    void* opaque;
    BioseqCacheStatusV2 (*emit_value)(
        void* opaque,
        size_t reference_index,
        size_t requested_attribute_index,
        uintptr_t datum,
        bool is_null) noexcept;
    void (*emit_reference_status)(
        void* opaque,
        size_t reference_index,
        BioseqCacheStatusV2 status) noexcept;
};

struct BioseqCacheExplainSinkV2 {
    uint32_t struct_size;
    void* opaque;
    void (*property_text)(
        void* opaque, const char* name, const char* value) noexcept;
    void (*property_uint64)(
        void* opaque, const char* name, uint64_t value) noexcept;
    void (*property_bool)(
        void* opaque, const char* name, bool value) noexcept;
};

struct BioseqCacheProviderV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    uint32_t provider_id;
    uint32_t provider_flags;
    uint32_t reserved;
    const char* provider_name;
    const CustomScanMethods* provider_scan_methods;

    BioseqCacheStatusV2 (*analyze_query)(
        const Query* query,
        BioseqCacheQueryIntentV2* out);
    BioseqCacheStatusV2 (*describe_path)(
        PlannerInfo* root,
        RelOptInfo* rel,
        Path* provider_path,
        const BioseqCacheQueryIntentV2* intent,
        BioseqCachePathDomainV2* out);
    BioseqCacheStatusV2 (*append_domain_semantic_digest)(
        const BioseqCachePathDomainV2* domain,
        BioseqCacheDigestSinkV2* sink);
    BioseqCacheStatusV2 (*append_generation_semantic_digest)(
        const BioseqCachePathDomainV2* domain,
        BioseqCacheDigestSinkV2* sink);
    BioseqCacheStatusV2 (*append_runtime_generation_semantic_digest)(
        const List* plan_domain_nodes,
        EState* estate,
        BioseqCacheDigestSinkV2* sink);
    BioseqCacheStatusV2 (*map_reusable_source)(
        PlannerInfo* root,
        const BioseqCachePathDomainV2* consumer_domain,
        const BioseqCacheReusableSourceV2* source,
        BioseqCacheSourceMappingV2* out);
    BioseqCacheStatusV2 (*decorate_path)(
        PlannerInfo* root,
        RelOptInfo* rel,
        Path* provider_path,
        const BioseqCachePathDomainV2* domain,
        List* gated_qual_exprs,
        BioseqCacheDecoratedPathV2* out);
    BioseqCacheStatusV2 (*build_plan_digest_recipe)(
        PlannerInfo* root,
        const Plan* provider_plan,
        BioseqCachePlanDigestRecipeV2* out);
    BioseqCacheStatusV2 (*extract_occurrence)(
        List* plan_domain_nodes,
        TupleTableSlot* slot,
        BioseqCacheOccurrenceV2* out);
    BioseqCacheStatusV2 (*late_fetch_batch)(
        List* plan_domain_nodes,
        EState* estate,
        const BioseqCacheOccurrenceV2* occurrences,
        size_t occurrence_count,
        const int16_t* attribute_numbers,
        size_t attribute_count,
        BioseqCacheLateFetchSinkV2* sink);
    void (*explain_domain)(
        List* plan_domain_nodes,
        BioseqCacheExplainSinkV2* sink);
};

struct BioseqCachePlannerBridgeV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    void (*after_provider_rel_paths)(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const BioseqCacheProviderV2* provider);
};

struct BioseqCacheProviderRegistrationV2 {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t registry_generation;
    uint64_t owner_cookie;
};

struct BioseqCacheApiV2;

struct BioseqCacheProviderDirectoryV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    uint32_t reserved;
    uint64_t instance_cookie;
    BioseqCacheStatusV2 (*bind_cache_api)(
        const BioseqCacheApiV2* cache_api);
};

struct BioseqCacheApiV2 {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t postgres_major;
    uint32_t reserved;
    uint64_t instance_cookie;
    BioseqCacheStatusV2 (*register_provider)(
        const BioseqCacheProviderV2* provider,
        BioseqCacheProviderRegistrationV2* registration);
    bool (*unregister_provider)(
        const BioseqCacheProviderRegistrationV2* registration);
    void (*record_relation_write)(
        uint32_t database_oid,
        uint32_t relation_oid,
        const BioseqCachePhysicalIdentityV2* physical_identity);
    const BioseqCachePlannerBridgeV2* planner_bridge;
};

} // extern "C"

export namespace bioseq::cache {

inline constexpr std::uint32_t kProviderAbiVersion = 2;
inline constexpr std::size_t kProviderDigestBytes = 32;
inline constexpr char kCacheApiRendezvousName[] = "bioseq_cache.api.v2";
inline constexpr char kProviderDirectoryRendezvousName[] =
    "bioseq.cache_provider_directory.v2";

using BioseqCacheStatusV2 = ::BioseqCacheStatusV2;
using BioseqCacheProviderFlagsV2 = ::BioseqCacheProviderFlagsV2;
using BioseqCacheProviderSecurityDigestFormatV2 =
    ::BioseqCacheProviderSecurityDigestFormatV2;
using BioseqCacheDigestSinkV2 = ::BioseqCacheDigestSinkV2;
using BioseqCachePhysicalIdentityV2 = ::BioseqCachePhysicalIdentityV2;
using BioseqCacheQueryIntentV2 = ::BioseqCacheQueryIntentV2;
using BioseqCacheColumnSchemaV2 = ::BioseqCacheColumnSchemaV2;
using BioseqCacheColumnMapEntryV2 = ::BioseqCacheColumnMapEntryV2;
using BioseqCachePathDomainV2 = ::BioseqCachePathDomainV2;
using BioseqCachePathDescriptionV2 = ::BioseqCachePathDescriptionV2;
using BioseqCacheReusableSourceV2 = ::BioseqCacheReusableSourceV2;
using BioseqCacheSourceMappingV2 = ::BioseqCacheSourceMappingV2;
using BioseqCachePlanDigestRecipeV2 = ::BioseqCachePlanDigestRecipeV2;
using BioseqCacheDecoratedPathV2 = ::BioseqCacheDecoratedPathV2;
using BioseqCacheOccurrenceV2 = ::BioseqCacheOccurrenceV2;
using BioseqCacheLateFetchSinkV2 = ::BioseqCacheLateFetchSinkV2;
using BioseqCacheExplainSinkV2 = ::BioseqCacheExplainSinkV2;
using BioseqCacheProviderV2 = ::BioseqCacheProviderV2;
using BioseqCachePlannerBridgeV2 = ::BioseqCachePlannerBridgeV2;
using BioseqCacheProviderRegistrationV2 =
    ::BioseqCacheProviderRegistrationV2;
using BioseqCacheProviderDirectoryV2 =
    ::BioseqCacheProviderDirectoryV2;
using BioseqCacheApiV2 = ::BioseqCacheApiV2;

/** Validate the common fixed-width prefix of a version-two public ABI object. */
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

/** Validate that a declared ABI tail field is present before reading it. */
template <class PublicStruct, class Field>
[[nodiscard]] constexpr bool provider_abi_field_present(
    const PublicStruct* value,
    std::size_t field_offset,
    const Field&) noexcept {
    return value != nullptr
        && field_offset <= value->struct_size
        && sizeof(Field) <= value->struct_size - field_offset;
}

static_assert(sizeof(BioseqCacheStatusV2) == sizeof(std::uint32_t));
static_assert(alignof(BioseqCacheStatusV2) == alignof(std::uint32_t));
static_assert(sizeof(BioseqCacheProviderFlagsV2) == sizeof(std::uint32_t));
static_assert(alignof(BioseqCacheProviderFlagsV2) == alignof(std::uint32_t));
static_assert(
    sizeof(BioseqCacheProviderSecurityDigestFormatV2)
    == sizeof(std::uint32_t));
static_assert(
    alignof(BioseqCacheProviderSecurityDigestFormatV2)
    == alignof(std::uint32_t));

static_assert(std::is_standard_layout_v<BioseqCacheDigestSinkV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheDigestSinkV2>);
static_assert(offsetof(BioseqCacheDigestSinkV2, struct_size) == 0);
static_assert(
    offsetof(BioseqCacheDigestSinkV2, opaque)
    == (sizeof(void*) == 8 ? 8 : 4));
static_assert(
    offsetof(BioseqCacheDigestSinkV2, append)
    == offsetof(BioseqCacheDigestSinkV2, opaque) + sizeof(void*));
static_assert(
    sizeof(BioseqCacheDigestSinkV2)
    == (sizeof(void*) == 8 ? 24 : 12));
static_assert(alignof(BioseqCacheDigestSinkV2) == alignof(void*));

static_assert(std::is_standard_layout_v<BioseqCachePhysicalIdentityV2>);
static_assert(std::is_trivially_copyable_v<BioseqCachePhysicalIdentityV2>);
static_assert(offsetof(BioseqCachePhysicalIdentityV2, struct_size) == 0);
static_assert(offsetof(BioseqCachePhysicalIdentityV2, kind) == 4);
static_assert(offsetof(BioseqCachePhysicalIdentityV2, length) == 8);
static_assert(offsetof(BioseqCachePhysicalIdentityV2, reserved) == 12);
static_assert(offsetof(BioseqCachePhysicalIdentityV2, bytes) == 16);
static_assert(sizeof(BioseqCachePhysicalIdentityV2) == 48);
static_assert(alignof(BioseqCachePhysicalIdentityV2) == 4);

static_assert(std::is_standard_layout_v<BioseqCacheQueryIntentV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheQueryIntentV2>);
static_assert(offsetof(BioseqCacheQueryIntentV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheQueryIntentV2, provider_id) == 4);
static_assert(offsetof(BioseqCacheQueryIntentV2, flags) == 8);
static_assert(offsetof(BioseqCacheQueryIntentV2, reserved) == 12);
static_assert(offsetof(BioseqCacheQueryIntentV2, provider_private_nodes) == 16);
static_assert(
    sizeof(BioseqCacheQueryIntentV2)
    == (sizeof(void*) == 8 ? 24 : 20));
static_assert(alignof(BioseqCacheQueryIntentV2) == alignof(void*));

static_assert(std::is_standard_layout_v<BioseqCacheColumnSchemaV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheColumnSchemaV2>);
static_assert(offsetof(BioseqCacheColumnSchemaV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheColumnSchemaV2, attno) == 4);
static_assert(offsetof(BioseqCacheColumnSchemaV2, semantic_id_high) == 8);
static_assert(offsetof(BioseqCacheColumnSchemaV2, semantic_id_low) == 16);
static_assert(offsetof(BioseqCacheColumnSchemaV2, type_oid) == 24);
static_assert(offsetof(BioseqCacheColumnSchemaV2, typmod) == 28);
static_assert(offsetof(BioseqCacheColumnSchemaV2, collation_oid) == 32);
static_assert(offsetof(BioseqCacheColumnSchemaV2, flags) == 36);
static_assert(sizeof(BioseqCacheColumnSchemaV2) == 40);
static_assert(alignof(BioseqCacheColumnSchemaV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheColumnMapEntryV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheColumnMapEntryV2>);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, consumer_attno) == 4);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, source_attno) == 8);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, flags) == 12);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, semantic_id_high) == 16);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, semantic_id_low) == 24);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, type_oid) == 32);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, typmod) == 36);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, collation_oid) == 40);
static_assert(offsetof(BioseqCacheColumnMapEntryV2, reserved) == 44);
static_assert(sizeof(BioseqCacheColumnMapEntryV2) == 48);
static_assert(alignof(BioseqCacheColumnMapEntryV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCachePathDomainV2>);
static_assert(std::is_trivially_copyable_v<BioseqCachePathDomainV2>);
static_assert(offsetof(BioseqCachePathDomainV2, struct_size) == 0);
static_assert(offsetof(BioseqCachePathDomainV2, provider_id) == 4);
static_assert(
    offsetof(BioseqCachePathDomainV2, compatibility_family_id) == 8);
static_assert(offsetof(BioseqCachePathDomainV2, capability_mask) == 16);
static_assert(offsetof(BioseqCachePathDomainV2, coverage_predicate) == 24);
static_assert(offsetof(BioseqCachePathDomainV2, boundary_columns) == 32);
static_assert(offsetof(BioseqCachePathDomainV2, boundary_column_count) == 40);
static_assert(offsetof(BioseqCachePathDomainV2, lineage_columns) == 48);
static_assert(offsetof(BioseqCachePathDomainV2, lineage_column_count) == 56);
static_assert(
    offsetof(BioseqCachePathDomainV2, required_boundary_attnos) == 64);
static_assert(
    offsetof(BioseqCachePathDomainV2, required_boundary_attno_count) == 72);
static_assert(
    offsetof(BioseqCachePathDomainV2, required_lineage_attnos) == 80);
static_assert(
    offsetof(BioseqCachePathDomainV2, required_lineage_attno_count) == 88);
static_assert(offsetof(BioseqCachePathDomainV2, dependency_nodes) == 96);
static_assert(
    offsetof(BioseqCachePathDomainV2, provider_private_nodes) == 104);
static_assert(
    offsetof(
        BioseqCachePathDomainV2,
        provider_security_digest_format_version)
    == 112);
static_assert(
    offsetof(BioseqCachePathDomainV2, provider_security_flags) == 116);
static_assert(
    offsetof(BioseqCachePathDomainV2, provider_security_digest) == 120);
static_assert(sizeof(BioseqCachePathDomainV2) == 152);
static_assert(alignof(BioseqCachePathDomainV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCachePathDescriptionV2>);
static_assert(std::is_trivially_copyable_v<BioseqCachePathDescriptionV2>);
static_assert(offsetof(BioseqCachePathDescriptionV2, struct_size) == 0);
static_assert(offsetof(BioseqCachePathDescriptionV2, flags) == 4);
static_assert(offsetof(BioseqCachePathDescriptionV2, capability_mask) == 8);
static_assert(offsetof(BioseqCachePathDescriptionV2, dependency_nodes) == 16);
static_assert(
    offsetof(BioseqCachePathDescriptionV2, provider_private_nodes) == 24);
static_assert(sizeof(BioseqCachePathDescriptionV2) == 32);
static_assert(alignof(BioseqCachePathDescriptionV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheReusableSourceV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheReusableSourceV2>);
static_assert(offsetof(BioseqCacheReusableSourceV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheReusableSourceV2, provider_id) == 4);
static_assert(
    offsetof(BioseqCacheReusableSourceV2, compatibility_family_id) == 8);
static_assert(offsetof(BioseqCacheReusableSourceV2, capability_mask) == 16);
static_assert(offsetof(BioseqCacheReusableSourceV2, boundary_columns) == 24);
static_assert(
    offsetof(BioseqCacheReusableSourceV2, boundary_column_count) == 32);
static_assert(offsetof(BioseqCacheReusableSourceV2, lineage_columns) == 40);
static_assert(
    offsetof(BioseqCacheReusableSourceV2, lineage_column_count) == 48);
static_assert(
    offsetof(BioseqCacheReusableSourceV2, provider_private_nodes) == 56);
static_assert(sizeof(BioseqCacheReusableSourceV2) == 64);
static_assert(alignof(BioseqCacheReusableSourceV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheSourceMappingV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheSourceMappingV2>);
static_assert(offsetof(BioseqCacheSourceMappingV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheSourceMappingV2, flags) == 4);
static_assert(offsetof(BioseqCacheSourceMappingV2, boundary_map) == 8);
static_assert(offsetof(BioseqCacheSourceMappingV2, boundary_map_count) == 16);
static_assert(offsetof(BioseqCacheSourceMappingV2, lineage_map) == 24);
static_assert(offsetof(BioseqCacheSourceMappingV2, lineage_map_count) == 32);
static_assert(
    offsetof(BioseqCacheSourceMappingV2, provider_private_nodes) == 40);
static_assert(sizeof(BioseqCacheSourceMappingV2) == 48);
static_assert(alignof(BioseqCacheSourceMappingV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCachePlanDigestRecipeV2>);
static_assert(std::is_trivially_copyable_v<BioseqCachePlanDigestRecipeV2>);
static_assert(offsetof(BioseqCachePlanDigestRecipeV2, struct_size) == 0);
static_assert(offsetof(BioseqCachePlanDigestRecipeV2, flags) == 4);
static_assert(
    offsetof(BioseqCachePlanDigestRecipeV2, digest_format_version) == 8);
static_assert(offsetof(BioseqCachePlanDigestRecipeV2, reserved) == 12);
static_assert(
    offsetof(BioseqCachePlanDigestRecipeV2, plan_domain_nodes) == 16);
static_assert(
    offsetof(BioseqCachePlanDigestRecipeV2, expected_generation_digest)
    == 24);
static_assert(sizeof(BioseqCachePlanDigestRecipeV2) == 56);
static_assert(alignof(BioseqCachePlanDigestRecipeV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheDecoratedPathV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheDecoratedPathV2>);
static_assert(offsetof(BioseqCacheDecoratedPathV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheDecoratedPathV2, flags) == 4);
static_assert(offsetof(BioseqCacheDecoratedPathV2, path) == 8);
static_assert(
    offsetof(BioseqCacheDecoratedPathV2, actual_boundary_columns) == 16);
static_assert(
    offsetof(BioseqCacheDecoratedPathV2, actual_boundary_column_count) == 24);
static_assert(
    offsetof(BioseqCacheDecoratedPathV2, actual_lineage_columns) == 32);
static_assert(
    offsetof(BioseqCacheDecoratedPathV2, actual_lineage_column_count) == 40);
static_assert(
    offsetof(BioseqCacheDecoratedPathV2, provider_private_nodes) == 48);
static_assert(sizeof(BioseqCacheDecoratedPathV2) == 56);
static_assert(alignof(BioseqCacheDecoratedPathV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheOccurrenceV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheOccurrenceV2>);
static_assert(offsetof(BioseqCacheOccurrenceV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheOccurrenceV2, rte_ordinal) == 4);
static_assert(
    offsetof(BioseqCacheOccurrenceV2, logical_relation_oid) == 8);
static_assert(offsetof(BioseqCacheOccurrenceV2, leaf_relation_oid) == 12);
static_assert(offsetof(BioseqCacheOccurrenceV2, identity_kind) == 16);
static_assert(offsetof(BioseqCacheOccurrenceV2, reserved) == 20);
static_assert(offsetof(BioseqCacheOccurrenceV2, row_identity) == 24);
static_assert(offsetof(BioseqCacheOccurrenceV2, occurrence_high) == 32);
static_assert(offsetof(BioseqCacheOccurrenceV2, occurrence_low) == 40);
static_assert(offsetof(BioseqCacheOccurrenceV2, capability_mask) == 48);
static_assert(sizeof(BioseqCacheOccurrenceV2) == 56);
static_assert(alignof(BioseqCacheOccurrenceV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheLateFetchSinkV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheLateFetchSinkV2>);
static_assert(offsetof(BioseqCacheLateFetchSinkV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheLateFetchSinkV2, opaque) == 8);
static_assert(offsetof(BioseqCacheLateFetchSinkV2, emit_value) == 16);
static_assert(
    offsetof(BioseqCacheLateFetchSinkV2, emit_reference_status) == 24);
static_assert(sizeof(BioseqCacheLateFetchSinkV2) == 32);
static_assert(alignof(BioseqCacheLateFetchSinkV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheExplainSinkV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheExplainSinkV2>);
static_assert(offsetof(BioseqCacheExplainSinkV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheExplainSinkV2, opaque) == 8);
static_assert(offsetof(BioseqCacheExplainSinkV2, property_text) == 16);
static_assert(offsetof(BioseqCacheExplainSinkV2, property_uint64) == 24);
static_assert(offsetof(BioseqCacheExplainSinkV2, property_bool) == 32);
static_assert(sizeof(BioseqCacheExplainSinkV2) == 40);
static_assert(alignof(BioseqCacheExplainSinkV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheProviderV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheProviderV2>);
static_assert(offsetof(BioseqCacheProviderV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheProviderV2, abi_version) == 4);
static_assert(offsetof(BioseqCacheProviderV2, postgres_major) == 8);
static_assert(offsetof(BioseqCacheProviderV2, provider_id) == 12);
static_assert(offsetof(BioseqCacheProviderV2, provider_flags) == 16);
static_assert(offsetof(BioseqCacheProviderV2, reserved) == 20);
static_assert(offsetof(BioseqCacheProviderV2, provider_name) == 24);
static_assert(
    offsetof(BioseqCacheProviderV2, provider_scan_methods) == 32);
static_assert(offsetof(BioseqCacheProviderV2, analyze_query) == 40);
static_assert(offsetof(BioseqCacheProviderV2, describe_path) == 48);
static_assert(
    offsetof(BioseqCacheProviderV2, append_domain_semantic_digest) == 56);
static_assert(
    offsetof(BioseqCacheProviderV2, append_generation_semantic_digest) == 64);
static_assert(
    offsetof(
        BioseqCacheProviderV2,
        append_runtime_generation_semantic_digest)
    == 72);
static_assert(offsetof(BioseqCacheProviderV2, map_reusable_source) == 80);
static_assert(offsetof(BioseqCacheProviderV2, decorate_path) == 88);
static_assert(
    offsetof(BioseqCacheProviderV2, build_plan_digest_recipe) == 96);
static_assert(offsetof(BioseqCacheProviderV2, extract_occurrence) == 104);
static_assert(offsetof(BioseqCacheProviderV2, late_fetch_batch) == 112);
static_assert(offsetof(BioseqCacheProviderV2, explain_domain) == 120);
static_assert(sizeof(BioseqCacheProviderV2) == 128);
static_assert(alignof(BioseqCacheProviderV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCachePlannerBridgeV2>);
static_assert(std::is_trivially_copyable_v<BioseqCachePlannerBridgeV2>);
static_assert(offsetof(BioseqCachePlannerBridgeV2, struct_size) == 0);
static_assert(offsetof(BioseqCachePlannerBridgeV2, abi_version) == 4);
static_assert(offsetof(BioseqCachePlannerBridgeV2, postgres_major) == 8);
static_assert(
    offsetof(BioseqCachePlannerBridgeV2, after_provider_rel_paths) == 16);
static_assert(sizeof(BioseqCachePlannerBridgeV2) == 24);
static_assert(alignof(BioseqCachePlannerBridgeV2) == 8);

static_assert(std::is_standard_layout_v<
    BioseqCacheProviderRegistrationV2>);
static_assert(std::is_trivially_copyable_v<
    BioseqCacheProviderRegistrationV2>);
static_assert(
    offsetof(BioseqCacheProviderRegistrationV2, struct_size) == 0);
static_assert(
    offsetof(BioseqCacheProviderRegistrationV2, provider_id) == 4);
static_assert(
    offsetof(BioseqCacheProviderRegistrationV2, registry_generation) == 8);
static_assert(
    offsetof(BioseqCacheProviderRegistrationV2, owner_cookie) == 16);
static_assert(sizeof(BioseqCacheProviderRegistrationV2) == 24);
static_assert(alignof(BioseqCacheProviderRegistrationV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheProviderDirectoryV2>);
static_assert(std::is_trivially_copyable_v<
    BioseqCacheProviderDirectoryV2>);
static_assert(offsetof(BioseqCacheProviderDirectoryV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheProviderDirectoryV2, abi_version) == 4);
static_assert(offsetof(BioseqCacheProviderDirectoryV2, postgres_major) == 8);
static_assert(offsetof(BioseqCacheProviderDirectoryV2, reserved) == 12);
static_assert(offsetof(BioseqCacheProviderDirectoryV2, instance_cookie) == 16);
static_assert(offsetof(BioseqCacheProviderDirectoryV2, bind_cache_api) == 24);
static_assert(sizeof(BioseqCacheProviderDirectoryV2) == 32);
static_assert(alignof(BioseqCacheProviderDirectoryV2) == 8);

static_assert(std::is_standard_layout_v<BioseqCacheApiV2>);
static_assert(std::is_trivially_copyable_v<BioseqCacheApiV2>);
static_assert(offsetof(BioseqCacheApiV2, struct_size) == 0);
static_assert(offsetof(BioseqCacheApiV2, abi_version) == 4);
static_assert(offsetof(BioseqCacheApiV2, postgres_major) == 8);
static_assert(offsetof(BioseqCacheApiV2, reserved) == 12);
static_assert(offsetof(BioseqCacheApiV2, instance_cookie) == 16);
static_assert(offsetof(BioseqCacheApiV2, register_provider) == 24);
static_assert(offsetof(BioseqCacheApiV2, unregister_provider) == 32);
static_assert(offsetof(BioseqCacheApiV2, record_relation_write) == 40);
static_assert(offsetof(BioseqCacheApiV2, planner_bridge) == 48);
static_assert(sizeof(BioseqCacheApiV2) == 56);
static_assert(alignof(BioseqCacheApiV2) == 8);

} // namespace bioseq::cache
