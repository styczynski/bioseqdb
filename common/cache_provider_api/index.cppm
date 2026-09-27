/**
 * @file common/cache_provider_api/index.cppm
 * @brief Shared interface between the query cache and its providers.
 *
 * Extensions find this interface through PostgreSQL rendezvous variables.
 * The structs and callbacks use a versioned C-compatible layout.
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
struct PlannedStmt;
struct PlannerInfo;
struct Query;
struct RangeTblEntry;
struct RelOptInfo;
struct RelationData;
struct TupleTableSlot;

enum bioseq_cache_status : uint32_t {
    bioseq_cache_status_ok = 0,
    bioseq_cache_status_unsupported = 1,
    bioseq_cache_status_error = 2,
};

enum bioseq_cache_provider_flags : uint32_t {
    bioseq_cache_provider_exact_source_elision_safe = 1U << 0,
    bioseq_cache_provider_reusable_coverage = 1U << 1,
    bioseq_cache_provider_complete_write_observation = 1U << 2,
};

/**
 * Format version for a provider's security-state digest.
 *
 * A provider with no extra security state uses this version and an all-zero
 * digest. Version zero is rejected because it leaves the security state
 * unspecified.
 */
enum bioseq_cache_provider_security_digest_format : uint32_t {
    bioseq_cache_provider_security_digest_format_canonical = 1,
};

struct bioseq_cache_digest_sink {
    uint32_t struct_size;
    void* opaque;
    bool (*append)(void* opaque, const void* data, size_t size) noexcept;
};

struct bioseq_cache_physical_identity {
    uint32_t struct_size;
    uint32_t kind;
    uint32_t length;
    uint32_t reserved;
    uint8_t bytes[32];
};

struct bioseq_cache_query_intent {
    uint32_t struct_size;
    uint32_t provider_id;
    uint32_t flags;
    uint32_t reserved;
    List* provider_private_nodes;
};

struct bioseq_cache_column_schema {
    uint32_t struct_size;
    uint32_t attno;
    uint64_t semantic_id_high;
    uint64_t semantic_id_low;
    uint32_t type_oid;
    int32_t typmod;
    uint32_t collation_oid;
    uint32_t flags;
};

struct bioseq_cache_column_map_entry {
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

struct bioseq_cache_path_domain {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t compatibility_family_id;
    uint64_t capability_mask;
    Expr* coverage_predicate;
    const bioseq_cache_column_schema* boundary_columns;
    size_t boundary_column_count;
    const bioseq_cache_column_schema* lineage_columns;
    size_t lineage_column_count;
    const uint32_t* required_boundary_attnos;
    size_t required_boundary_attno_count;
    const uint32_t* required_lineage_attnos;
    size_t required_lineage_attno_count;
    List* dependency_nodes;
    List* provider_private_nodes;
    uint32_t provider_security_digest_format_revision;
    uint32_t provider_security_flags;
    uint8_t provider_security_digest[32];
};

struct bioseq_cache_path_description {
    uint32_t struct_size;
    uint32_t flags;
    uint64_t capability_mask;
    List* dependency_nodes;
    List* provider_private_nodes;
};

struct bioseq_cache_reusable_source {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t compatibility_family_id;
    uint64_t capability_mask;
    const bioseq_cache_column_schema* boundary_columns;
    size_t boundary_column_count;
    const bioseq_cache_column_schema* lineage_columns;
    size_t lineage_column_count;
    List* provider_private_nodes;
};

struct bioseq_cache_source_mapping {
    uint32_t struct_size;
    uint32_t flags;
    const bioseq_cache_column_map_entry* boundary_map;
    size_t boundary_map_count;
    const bioseq_cache_column_map_entry* lineage_map;
    size_t lineage_map_count;
    List* provider_private_nodes;
};

struct bioseq_cache_plan_digest_recipe {
    uint32_t struct_size;
    uint32_t flags;
    uint32_t digest_format_revision;
    uint32_t reserved;
    List* plan_domain_nodes;
    uint8_t expected_generation_digest[32];
};

struct bioseq_cache_decorated_path {
    uint32_t struct_size;
    uint32_t flags;
    Path* path;
    const bioseq_cache_column_schema* actual_boundary_columns;
    size_t actual_boundary_column_count;
    const bioseq_cache_column_schema* actual_lineage_columns;
    size_t actual_lineage_column_count;
    List* provider_private_nodes;
};

struct bioseq_cache_occurrence {
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

struct bioseq_cache_late_fetch_sink {
    uint32_t struct_size;
    void* opaque;
    bioseq_cache_status (*emit_value)(
        void* opaque,
        size_t reference_index,
        size_t requested_attribute_index,
        uintptr_t datum,
        bool is_null) noexcept;
    void (*emit_reference_status)(
        void* opaque,
        size_t reference_index,
        bioseq_cache_status status) noexcept;
};

struct bioseq_cache_explain_sink {
    uint32_t struct_size;
    void* opaque;
    void (*property_text)(
        void* opaque, const char* name, const char* value) noexcept;
    void (*property_uint64)(
        void* opaque, const char* name, uint64_t value) noexcept;
    void (*property_bool)(
        void* opaque, const char* name, bool value) noexcept;
};

struct bioseq_cache_provider {
    uint32_t struct_size;
    uint32_t abi_revision;
    uint32_t postgres_major;
    uint32_t provider_id;
    uint32_t provider_flags;
    uint32_t reserved;
    const char* provider_name;
    const CustomScanMethods* provider_scan_methods;

    bioseq_cache_status (*analyze_query)(
        const Query* query,
        bioseq_cache_query_intent* out);
    bioseq_cache_status (*describe_path)(
        PlannerInfo* root,
        RelOptInfo* rel,
        Path* provider_path,
        const bioseq_cache_query_intent* intent,
        bioseq_cache_path_domain* out);
    bioseq_cache_status (*append_domain_semantic_digest)(
        const bioseq_cache_path_domain* domain,
        bioseq_cache_digest_sink* sink);
    bioseq_cache_status (*append_generation_semantic_digest)(
        const bioseq_cache_path_domain* domain,
        bioseq_cache_digest_sink* sink);
    bioseq_cache_status (*append_runtime_generation_semantic_digest)(
        const List* plan_domain_nodes,
        EState* estate,
        bioseq_cache_digest_sink* sink);
    bioseq_cache_status (*map_reusable_source)(
        PlannerInfo* root,
        const bioseq_cache_path_domain* consumer_domain,
        const bioseq_cache_reusable_source* source,
        bioseq_cache_source_mapping* out);
    bioseq_cache_status (*decorate_path)(
        PlannerInfo* root,
        RelOptInfo* rel,
        Path* provider_path,
        const bioseq_cache_path_domain* domain,
        List* gated_qual_exprs,
        bioseq_cache_decorated_path* out);
    bioseq_cache_status (*build_plan_digest_recipe)(
        PlannerInfo* root,
        const Plan* provider_plan,
        bioseq_cache_plan_digest_recipe* out);
    bioseq_cache_status (*extract_occurrence)(
        List* plan_domain_nodes,
        TupleTableSlot* slot,
        bioseq_cache_occurrence* out);
    bioseq_cache_status (*late_fetch_batch)(
        List* plan_domain_nodes,
        EState* estate,
        const bioseq_cache_occurrence* occurrences,
        size_t occurrence_count,
        const int16_t* attribute_numbers,
        size_t attribute_count,
        bioseq_cache_late_fetch_sink* sink);
    void (*explain_domain)(
        List* plan_domain_nodes,
        bioseq_cache_explain_sink* sink);
};

struct bioseq_cache_planner_bridge {
    uint32_t struct_size;
    uint32_t abi_revision;
    uint32_t postgres_major;
    void (*after_provider_rel_paths)(
        PlannerInfo* root,
        RelOptInfo* rel,
        unsigned int rti,
        RangeTblEntry* rte,
        const bioseq_cache_provider* provider);
    /**
     * Refresh exact-cache plan descriptions after the provider finishes changing
     * the plan, including changes made after PostgreSQL resolves plan references.
     *
     * Calling this again for the same PlannedStmt is allowed. It updates planning
     * metadata without starting the executor or the saved child plan.
     */
    void (*after_planned_statement)(PlannedStmt* statement);
};

struct bioseq_cache_provider_registration {
    uint32_t struct_size;
    uint32_t provider_id;
    uint64_t registry_generation;
    uint64_t owner_cookie;
};

struct bioseq_cache_api;

struct bioseq_cache_provider_directory {
    uint32_t struct_size;
    uint32_t abi_revision;
    uint32_t postgres_major;
    uint32_t reserved;
    uint64_t instance_cookie;
    bioseq_cache_status (*bind_cache_api)(
        const bioseq_cache_api* cache_api);
};

struct bioseq_cache_api {
    uint32_t struct_size;
    uint32_t abi_revision;
    uint32_t postgres_major;
    uint32_t reserved;
    uint64_t instance_cookie;
    bioseq_cache_status (*register_provider)(
        const bioseq_cache_provider* provider,
        bioseq_cache_provider_registration* registration);
    bool (*unregister_provider)(
        const bioseq_cache_provider_registration* registration);
    void (*record_relation_write)(
        uint32_t database_oid,
        uint32_t relation_oid,
        const bioseq_cache_physical_identity* physical_identity);
    const bioseq_cache_planner_bridge* planner_bridge;
};

} // extern "C"

export namespace bioseq::cache {

inline constexpr std::uint32_t k_provider_abi_revision = 2;
inline constexpr std::size_t k_provider_digest_bytes = 32;
inline constexpr char k_cache_api_rendezvous_name[] = "bioseq_cache.api";
inline constexpr char k_provider_directory_rendezvous_name[] =
    "bioseq.cache_provider_directory";

using bioseq_cache_status = ::bioseq_cache_status;
using bioseq_cache_provider_flags = ::bioseq_cache_provider_flags;
using bioseq_cache_provider_security_digest_format =
    ::bioseq_cache_provider_security_digest_format;
using bioseq_cache_digest_sink = ::bioseq_cache_digest_sink;
using bioseq_cache_physical_identity = ::bioseq_cache_physical_identity;
using bioseq_cache_query_intent = ::bioseq_cache_query_intent;
using bioseq_cache_column_schema = ::bioseq_cache_column_schema;
using bioseq_cache_column_map_entry = ::bioseq_cache_column_map_entry;
using bioseq_cache_path_domain = ::bioseq_cache_path_domain;
using bioseq_cache_path_description = ::bioseq_cache_path_description;
using bioseq_cache_reusable_source = ::bioseq_cache_reusable_source;
using bioseq_cache_source_mapping = ::bioseq_cache_source_mapping;
using bioseq_cache_plan_digest_recipe = ::bioseq_cache_plan_digest_recipe;
using bioseq_cache_decorated_path = ::bioseq_cache_decorated_path;
using bioseq_cache_occurrence = ::bioseq_cache_occurrence;
using bioseq_cache_late_fetch_sink = ::bioseq_cache_late_fetch_sink;
using bioseq_cache_explain_sink = ::bioseq_cache_explain_sink;
using bioseq_cache_provider = ::bioseq_cache_provider;
using bioseq_cache_planner_bridge = ::bioseq_cache_planner_bridge;
using bioseq_cache_provider_registration =
    ::bioseq_cache_provider_registration;
using bioseq_cache_provider_directory =
    ::bioseq_cache_provider_directory;
using bioseq_cache_api = ::bioseq_cache_api;

/** Check the pointer, struct size, interface version and PostgreSQL major version. */
template <class p_public_struct>
[[nodiscard]] constexpr bool provider_abi_prefix_valid(
    const p_public_struct* value,
    std::uint32_t abi_revision,
    std::uint32_t postgres_major) noexcept {
    return value != nullptr
        && value->struct_size >= sizeof(p_public_struct)
        && value->abi_revision == k_provider_abi_revision
        && value->postgres_major == postgres_major
        && abi_revision == k_provider_abi_revision;
}

/** Check that struct_size includes the whole field before reading it. */
template <class p_public_struct, class p_field>
[[nodiscard]] constexpr bool provider_abi_field_present(
    const p_public_struct* value,
    std::size_t field_offset,
    const p_field&) noexcept {
    return value != nullptr
        && field_offset <= value->struct_size
        && sizeof(p_field) <= value->struct_size - field_offset;
}

static_assert(sizeof(bioseq_cache_status) == sizeof(std::uint32_t));
static_assert(alignof(bioseq_cache_status) == alignof(std::uint32_t));
static_assert(sizeof(bioseq_cache_provider_flags) == sizeof(std::uint32_t));
static_assert(alignof(bioseq_cache_provider_flags) == alignof(std::uint32_t));
static_assert(
    sizeof(bioseq_cache_provider_security_digest_format)
    == sizeof(std::uint32_t));
static_assert(
    alignof(bioseq_cache_provider_security_digest_format)
    == alignof(std::uint32_t));

static_assert(std::is_standard_layout_v<bioseq_cache_digest_sink>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_digest_sink>);
static_assert(offsetof(bioseq_cache_digest_sink, struct_size) == 0);
static_assert(
    offsetof(bioseq_cache_digest_sink, opaque)
    == (sizeof(void*) == 8 ? 8 : 4));
static_assert(
    offsetof(bioseq_cache_digest_sink, append)
    == offsetof(bioseq_cache_digest_sink, opaque) + sizeof(void*));
static_assert(
    sizeof(bioseq_cache_digest_sink)
    == (sizeof(void*) == 8 ? 24 : 12));
static_assert(alignof(bioseq_cache_digest_sink) == alignof(void*));

static_assert(std::is_standard_layout_v<bioseq_cache_physical_identity>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_physical_identity>);
static_assert(offsetof(bioseq_cache_physical_identity, struct_size) == 0);
static_assert(offsetof(bioseq_cache_physical_identity, kind) == 4);
static_assert(offsetof(bioseq_cache_physical_identity, length) == 8);
static_assert(offsetof(bioseq_cache_physical_identity, reserved) == 12);
static_assert(offsetof(bioseq_cache_physical_identity, bytes) == 16);
static_assert(sizeof(bioseq_cache_physical_identity) == 48);
static_assert(alignof(bioseq_cache_physical_identity) == 4);

static_assert(std::is_standard_layout_v<bioseq_cache_query_intent>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_query_intent>);
static_assert(offsetof(bioseq_cache_query_intent, struct_size) == 0);
static_assert(offsetof(bioseq_cache_query_intent, provider_id) == 4);
static_assert(offsetof(bioseq_cache_query_intent, flags) == 8);
static_assert(offsetof(bioseq_cache_query_intent, reserved) == 12);
static_assert(offsetof(bioseq_cache_query_intent, provider_private_nodes) == 16);
static_assert(
    sizeof(bioseq_cache_query_intent)
    == (sizeof(void*) == 8 ? 24 : 20));
static_assert(alignof(bioseq_cache_query_intent) == alignof(void*));

static_assert(std::is_standard_layout_v<bioseq_cache_column_schema>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_column_schema>);
static_assert(offsetof(bioseq_cache_column_schema, struct_size) == 0);
static_assert(offsetof(bioseq_cache_column_schema, attno) == 4);
static_assert(offsetof(bioseq_cache_column_schema, semantic_id_high) == 8);
static_assert(offsetof(bioseq_cache_column_schema, semantic_id_low) == 16);
static_assert(offsetof(bioseq_cache_column_schema, type_oid) == 24);
static_assert(offsetof(bioseq_cache_column_schema, typmod) == 28);
static_assert(offsetof(bioseq_cache_column_schema, collation_oid) == 32);
static_assert(offsetof(bioseq_cache_column_schema, flags) == 36);
static_assert(sizeof(bioseq_cache_column_schema) == 40);
static_assert(alignof(bioseq_cache_column_schema) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_column_map_entry>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_column_map_entry>);
static_assert(offsetof(bioseq_cache_column_map_entry, struct_size) == 0);
static_assert(offsetof(bioseq_cache_column_map_entry, consumer_attno) == 4);
static_assert(offsetof(bioseq_cache_column_map_entry, source_attno) == 8);
static_assert(offsetof(bioseq_cache_column_map_entry, flags) == 12);
static_assert(offsetof(bioseq_cache_column_map_entry, semantic_id_high) == 16);
static_assert(offsetof(bioseq_cache_column_map_entry, semantic_id_low) == 24);
static_assert(offsetof(bioseq_cache_column_map_entry, type_oid) == 32);
static_assert(offsetof(bioseq_cache_column_map_entry, typmod) == 36);
static_assert(offsetof(bioseq_cache_column_map_entry, collation_oid) == 40);
static_assert(offsetof(bioseq_cache_column_map_entry, reserved) == 44);
static_assert(sizeof(bioseq_cache_column_map_entry) == 48);
static_assert(alignof(bioseq_cache_column_map_entry) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_path_domain>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_path_domain>);
static_assert(offsetof(bioseq_cache_path_domain, struct_size) == 0);
static_assert(offsetof(bioseq_cache_path_domain, provider_id) == 4);
static_assert(
    offsetof(bioseq_cache_path_domain, compatibility_family_id) == 8);
static_assert(offsetof(bioseq_cache_path_domain, capability_mask) == 16);
static_assert(offsetof(bioseq_cache_path_domain, coverage_predicate) == 24);
static_assert(offsetof(bioseq_cache_path_domain, boundary_columns) == 32);
static_assert(offsetof(bioseq_cache_path_domain, boundary_column_count) == 40);
static_assert(offsetof(bioseq_cache_path_domain, lineage_columns) == 48);
static_assert(offsetof(bioseq_cache_path_domain, lineage_column_count) == 56);
static_assert(
    offsetof(bioseq_cache_path_domain, required_boundary_attnos) == 64);
static_assert(
    offsetof(bioseq_cache_path_domain, required_boundary_attno_count) == 72);
static_assert(
    offsetof(bioseq_cache_path_domain, required_lineage_attnos) == 80);
static_assert(
    offsetof(bioseq_cache_path_domain, required_lineage_attno_count) == 88);
static_assert(offsetof(bioseq_cache_path_domain, dependency_nodes) == 96);
static_assert(
    offsetof(bioseq_cache_path_domain, provider_private_nodes) == 104);
static_assert(
    offsetof(
        bioseq_cache_path_domain,
        provider_security_digest_format_revision)
    == 112);
static_assert(
    offsetof(bioseq_cache_path_domain, provider_security_flags) == 116);
static_assert(
    offsetof(bioseq_cache_path_domain, provider_security_digest) == 120);
static_assert(sizeof(bioseq_cache_path_domain) == 152);
static_assert(alignof(bioseq_cache_path_domain) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_path_description>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_path_description>);
static_assert(offsetof(bioseq_cache_path_description, struct_size) == 0);
static_assert(offsetof(bioseq_cache_path_description, flags) == 4);
static_assert(offsetof(bioseq_cache_path_description, capability_mask) == 8);
static_assert(offsetof(bioseq_cache_path_description, dependency_nodes) == 16);
static_assert(
    offsetof(bioseq_cache_path_description, provider_private_nodes) == 24);
static_assert(sizeof(bioseq_cache_path_description) == 32);
static_assert(alignof(bioseq_cache_path_description) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_reusable_source>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_reusable_source>);
static_assert(offsetof(bioseq_cache_reusable_source, struct_size) == 0);
static_assert(offsetof(bioseq_cache_reusable_source, provider_id) == 4);
static_assert(
    offsetof(bioseq_cache_reusable_source, compatibility_family_id) == 8);
static_assert(offsetof(bioseq_cache_reusable_source, capability_mask) == 16);
static_assert(offsetof(bioseq_cache_reusable_source, boundary_columns) == 24);
static_assert(
    offsetof(bioseq_cache_reusable_source, boundary_column_count) == 32);
static_assert(offsetof(bioseq_cache_reusable_source, lineage_columns) == 40);
static_assert(
    offsetof(bioseq_cache_reusable_source, lineage_column_count) == 48);
static_assert(
    offsetof(bioseq_cache_reusable_source, provider_private_nodes) == 56);
static_assert(sizeof(bioseq_cache_reusable_source) == 64);
static_assert(alignof(bioseq_cache_reusable_source) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_source_mapping>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_source_mapping>);
static_assert(offsetof(bioseq_cache_source_mapping, struct_size) == 0);
static_assert(offsetof(bioseq_cache_source_mapping, flags) == 4);
static_assert(offsetof(bioseq_cache_source_mapping, boundary_map) == 8);
static_assert(offsetof(bioseq_cache_source_mapping, boundary_map_count) == 16);
static_assert(offsetof(bioseq_cache_source_mapping, lineage_map) == 24);
static_assert(offsetof(bioseq_cache_source_mapping, lineage_map_count) == 32);
static_assert(
    offsetof(bioseq_cache_source_mapping, provider_private_nodes) == 40);
static_assert(sizeof(bioseq_cache_source_mapping) == 48);
static_assert(alignof(bioseq_cache_source_mapping) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_plan_digest_recipe>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_plan_digest_recipe>);
static_assert(offsetof(bioseq_cache_plan_digest_recipe, struct_size) == 0);
static_assert(offsetof(bioseq_cache_plan_digest_recipe, flags) == 4);
static_assert(
    offsetof(bioseq_cache_plan_digest_recipe, digest_format_revision) == 8);
static_assert(offsetof(bioseq_cache_plan_digest_recipe, reserved) == 12);
static_assert(
    offsetof(bioseq_cache_plan_digest_recipe, plan_domain_nodes) == 16);
static_assert(
    offsetof(bioseq_cache_plan_digest_recipe, expected_generation_digest)
    == 24);
static_assert(sizeof(bioseq_cache_plan_digest_recipe) == 56);
static_assert(alignof(bioseq_cache_plan_digest_recipe) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_decorated_path>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_decorated_path>);
static_assert(offsetof(bioseq_cache_decorated_path, struct_size) == 0);
static_assert(offsetof(bioseq_cache_decorated_path, flags) == 4);
static_assert(offsetof(bioseq_cache_decorated_path, path) == 8);
static_assert(
    offsetof(bioseq_cache_decorated_path, actual_boundary_columns) == 16);
static_assert(
    offsetof(bioseq_cache_decorated_path, actual_boundary_column_count) == 24);
static_assert(
    offsetof(bioseq_cache_decorated_path, actual_lineage_columns) == 32);
static_assert(
    offsetof(bioseq_cache_decorated_path, actual_lineage_column_count) == 40);
static_assert(
    offsetof(bioseq_cache_decorated_path, provider_private_nodes) == 48);
static_assert(sizeof(bioseq_cache_decorated_path) == 56);
static_assert(alignof(bioseq_cache_decorated_path) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_occurrence>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_occurrence>);
static_assert(offsetof(bioseq_cache_occurrence, struct_size) == 0);
static_assert(offsetof(bioseq_cache_occurrence, rte_ordinal) == 4);
static_assert(
    offsetof(bioseq_cache_occurrence, logical_relation_oid) == 8);
static_assert(offsetof(bioseq_cache_occurrence, leaf_relation_oid) == 12);
static_assert(offsetof(bioseq_cache_occurrence, identity_kind) == 16);
static_assert(offsetof(bioseq_cache_occurrence, reserved) == 20);
static_assert(offsetof(bioseq_cache_occurrence, row_identity) == 24);
static_assert(offsetof(bioseq_cache_occurrence, occurrence_high) == 32);
static_assert(offsetof(bioseq_cache_occurrence, occurrence_low) == 40);
static_assert(offsetof(bioseq_cache_occurrence, capability_mask) == 48);
static_assert(sizeof(bioseq_cache_occurrence) == 56);
static_assert(alignof(bioseq_cache_occurrence) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_late_fetch_sink>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_late_fetch_sink>);
static_assert(offsetof(bioseq_cache_late_fetch_sink, struct_size) == 0);
static_assert(offsetof(bioseq_cache_late_fetch_sink, opaque) == 8);
static_assert(offsetof(bioseq_cache_late_fetch_sink, emit_value) == 16);
static_assert(
    offsetof(bioseq_cache_late_fetch_sink, emit_reference_status) == 24);
static_assert(sizeof(bioseq_cache_late_fetch_sink) == 32);
static_assert(alignof(bioseq_cache_late_fetch_sink) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_explain_sink>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_explain_sink>);
static_assert(offsetof(bioseq_cache_explain_sink, struct_size) == 0);
static_assert(offsetof(bioseq_cache_explain_sink, opaque) == 8);
static_assert(offsetof(bioseq_cache_explain_sink, property_text) == 16);
static_assert(offsetof(bioseq_cache_explain_sink, property_uint64) == 24);
static_assert(offsetof(bioseq_cache_explain_sink, property_bool) == 32);
static_assert(sizeof(bioseq_cache_explain_sink) == 40);
static_assert(alignof(bioseq_cache_explain_sink) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_provider>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_provider>);
static_assert(offsetof(bioseq_cache_provider, struct_size) == 0);
static_assert(offsetof(bioseq_cache_provider, abi_revision) == 4);
static_assert(offsetof(bioseq_cache_provider, postgres_major) == 8);
static_assert(offsetof(bioseq_cache_provider, provider_id) == 12);
static_assert(offsetof(bioseq_cache_provider, provider_flags) == 16);
static_assert(offsetof(bioseq_cache_provider, reserved) == 20);
static_assert(offsetof(bioseq_cache_provider, provider_name) == 24);
static_assert(
    offsetof(bioseq_cache_provider, provider_scan_methods) == 32);
static_assert(offsetof(bioseq_cache_provider, analyze_query) == 40);
static_assert(offsetof(bioseq_cache_provider, describe_path) == 48);
static_assert(
    offsetof(bioseq_cache_provider, append_domain_semantic_digest) == 56);
static_assert(
    offsetof(bioseq_cache_provider, append_generation_semantic_digest) == 64);
static_assert(
    offsetof(
        bioseq_cache_provider,
        append_runtime_generation_semantic_digest)
    == 72);
static_assert(offsetof(bioseq_cache_provider, map_reusable_source) == 80);
static_assert(offsetof(bioseq_cache_provider, decorate_path) == 88);
static_assert(
    offsetof(bioseq_cache_provider, build_plan_digest_recipe) == 96);
static_assert(offsetof(bioseq_cache_provider, extract_occurrence) == 104);
static_assert(offsetof(bioseq_cache_provider, late_fetch_batch) == 112);
static_assert(offsetof(bioseq_cache_provider, explain_domain) == 120);
static_assert(sizeof(bioseq_cache_provider) == 128);
static_assert(alignof(bioseq_cache_provider) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_planner_bridge>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_planner_bridge>);
static_assert(offsetof(bioseq_cache_planner_bridge, struct_size) == 0);
static_assert(offsetof(bioseq_cache_planner_bridge, abi_revision) == 4);
static_assert(offsetof(bioseq_cache_planner_bridge, postgres_major) == 8);
static_assert(
    offsetof(bioseq_cache_planner_bridge, after_provider_rel_paths) == 16);
static_assert(
    offsetof(bioseq_cache_planner_bridge, after_planned_statement) == 24);
static_assert(sizeof(bioseq_cache_planner_bridge) == 32);
static_assert(alignof(bioseq_cache_planner_bridge) == 8);

static_assert(std::is_standard_layout_v<
    bioseq_cache_provider_registration>);
static_assert(std::is_trivially_copyable_v<
    bioseq_cache_provider_registration>);
static_assert(
    offsetof(bioseq_cache_provider_registration, struct_size) == 0);
static_assert(
    offsetof(bioseq_cache_provider_registration, provider_id) == 4);
static_assert(
    offsetof(bioseq_cache_provider_registration, registry_generation) == 8);
static_assert(
    offsetof(bioseq_cache_provider_registration, owner_cookie) == 16);
static_assert(sizeof(bioseq_cache_provider_registration) == 24);
static_assert(alignof(bioseq_cache_provider_registration) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_provider_directory>);
static_assert(std::is_trivially_copyable_v<
    bioseq_cache_provider_directory>);
static_assert(offsetof(bioseq_cache_provider_directory, struct_size) == 0);
static_assert(offsetof(bioseq_cache_provider_directory, abi_revision) == 4);
static_assert(offsetof(bioseq_cache_provider_directory, postgres_major) == 8);
static_assert(offsetof(bioseq_cache_provider_directory, reserved) == 12);
static_assert(offsetof(bioseq_cache_provider_directory, instance_cookie) == 16);
static_assert(offsetof(bioseq_cache_provider_directory, bind_cache_api) == 24);
static_assert(sizeof(bioseq_cache_provider_directory) == 32);
static_assert(alignof(bioseq_cache_provider_directory) == 8);

static_assert(std::is_standard_layout_v<bioseq_cache_api>);
static_assert(std::is_trivially_copyable_v<bioseq_cache_api>);
static_assert(offsetof(bioseq_cache_api, struct_size) == 0);
static_assert(offsetof(bioseq_cache_api, abi_revision) == 4);
static_assert(offsetof(bioseq_cache_api, postgres_major) == 8);
static_assert(offsetof(bioseq_cache_api, reserved) == 12);
static_assert(offsetof(bioseq_cache_api, instance_cookie) == 16);
static_assert(offsetof(bioseq_cache_api, register_provider) == 24);
static_assert(offsetof(bioseq_cache_api, unregister_provider) == 32);
static_assert(offsetof(bioseq_cache_api, record_relation_write) == 40);
static_assert(offsetof(bioseq_cache_api, planner_bridge) == 48);
static_assert(sizeof(bioseq_cache_api) == 56);
static_assert(alignof(bioseq_cache_api) == 8);

} // namespace bioseq::cache
