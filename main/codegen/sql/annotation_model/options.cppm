module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

export module bioseq.codegen.sql.annotation_model:options;

import :entities;

export namespace bioseq::sql {

namespace pg_catalog_reference {

struct routine_reference final {
};

struct access_method_reference final {
};

inline constexpr routine_reference scalarltsel{};
inline constexpr routine_reference scalarltjoinsel{};
inline constexpr routine_reference scalarlesel{};
inline constexpr routine_reference scalarlejoinsel{};
inline constexpr routine_reference eqsel{};
inline constexpr routine_reference eqjoinsel{};
inline constexpr routine_reference neqsel{};
inline constexpr routine_reference neqjoinsel{};
inline constexpr routine_reference scalargesel{};
inline constexpr routine_reference scalargejoinsel{};
inline constexpr routine_reference scalargtsel{};
inline constexpr routine_reference scalargtjoinsel{};
inline constexpr routine_reference int8_avg_serialize{};
inline constexpr routine_reference int8_avg_deserialize{};

inline constexpr access_method_reference btree{};
inline constexpr access_method_reference hash{};

}  // namespace pg_catalog_reference

enum class create_mode : std::uint8_t {
    create,
    or_replace,
    if_not_exists,
};

enum class volatility : std::uint8_t {
    volatile_,
    stable,
    immutable,
};

enum class parallel_safety : std::uint8_t {
    unsafe,
    restricted,
    safe,
};

enum class security_mode : std::uint8_t {
    invoker,
    definer,
};

enum class null_input_policy : std::uint8_t {
    derived,
    called,
    strict,
};

enum class persistence : std::uint8_t {
    ordinary,
    temporary,
    unlogged,
};

enum class type_alignment : std::uint8_t {
    character,
    int2,
    int4,
    double_,
};

enum class type_storage : std::uint8_t {
    plain,
    external,
    extended,
    main,
};

enum class aggregate_modify : std::uint8_t {
    read_only,
    shareable,
    read_write,
};

enum class constraint_placement : std::uint8_t {
    column_inline,
    table_inline,
    alter_table,
};

enum class access_method_kind : std::uint8_t {
    table,
    index,
};

enum class guc_context : std::uint8_t {
    internal,
    postmaster,
    sighup,
    superuser_backend,
    backend,
    superuser,
    user,
};

enum class guc_flag : std::uint16_t {
    none = 0,
    no_show_all = 1U << 0U,
    not_in_sample = 1U << 1U,
    unit_kb = 1U << 2U,
    unit_mb = 1U << 3U,
    unit_blocks = 1U << 4U,
    unit_ms = 1U << 5U,
    unit_s = 1U << 6U,
    unit_min = 1U << 7U,
};

[[nodiscard]] consteval guc_flag operator|(
    guc_flag left,
    guc_flag right) noexcept {
    return static_cast<guc_flag>(
        static_cast<std::uint16_t>(left)
        | static_cast<std::uint16_t>(right));
}

enum class privileges : std::uint16_t {
    select = 1U << 0U,
    insert = 1U << 1U,
    update = 1U << 2U,
    delete_ = 1U << 3U,
    truncate = 1U << 4U,
    references = 1U << 5U,
    trigger = 1U << 6U,
    usage = 1U << 7U,
    execute = 1U << 8U,
    all = 1U << 9U,
};

enum class acl_scope : std::uint8_t {
    object,
    all_tables_in_schema,
    all_sequences_in_schema,
    all_functions_in_schema,
};

[[nodiscard]] consteval privileges operator|(
    privileges left,
    privileges right) noexcept {
    return static_cast<privileges>(
        static_cast<std::uint16_t>(left)
        | static_cast<std::uint16_t>(right));
}

struct function_options {
    schema_ref schema{};
    create_mode create{create_mode::create};
    bioseq::sql::volatility volatility{
        bioseq::sql::volatility::volatile_};
    parallel_safety parallel{parallel_safety::unsafe};
    security_mode security{security_mode::invoker};
    bool leakproof{};
    double cost{};
    double rows{};
    bool has_cost{};
    bool has_rows{};
    identifier support{"none"};
    checked_sql configuration{};
};

struct procedure_options {
    schema_ref schema{};
    create_mode create{create_mode::create};
    security_mode security{security_mode::invoker};
};

struct param_options {
    pg_type sql_type{};
    checked_sql default_value{};
};

struct result_options {
    pg_type sql_type{};
};

struct table_options {
    schema_ref schema{};
    bioseq::sql::persistence persistence{
        bioseq::sql::persistence::ordinary};
    create_mode create{create_mode::create};
    identifier access_method{"default"};
    bool user_catalog_table{};
};

struct column_options {
    pg_type sql_type{};
    checked_sql default_value{};
    identifier collation{"default"};
};

struct schema_options {
    create_mode create{create_mode::create};
};

struct sequence_options {
    schema_ref schema{};
    create_mode create{create_mode::create};
    std::int64_t increment{1};
    std::int64_t minimum{1};
    std::int64_t maximum{std::numeric_limits<std::int64_t>::max()};
    std::int64_t cache{1};
    bool cycle{};
};

struct view_options {
    schema_ref schema{};
    create_mode create{create_mode::create};
    bool recursive{};
    bool temporary{};
    bool security_barrier{};
    bool security_invoker{};
};

struct sql_function_options {
    schema_ref schema{};
    create_mode create{create_mode::create};
    bioseq::sql::volatility volatility{
        bioseq::sql::volatility::volatile_};
    parallel_safety parallel{parallel_safety::unsafe};
    security_mode security{security_mode::invoker};
};

struct operator_options {
    fixed_text<8> symbol{"="};
    declaration_reference commutator{};
    declaration_reference negator{};
    declaration_reference restrict{};
    declaration_reference join{};
    bool hashes{};
    bool merges{};
};

struct type_options {
    schema_ref schema{};
    std::int32_t internal_length{};
    bool variable_length{};
    bool passed_by_value{};
    type_alignment alignment{type_alignment::int4};
    type_storage storage{type_storage::plain};
    fixed_text<2> category{"U"};
    bool preferred{};
    char delimiter{','};
    bool collatable{};
    pg_type element{};
    pg_type array{};
    checked_sql default_value{};
};

struct cast_options {
    bool assignment{};
    bool implicit{};
    bool inout{};
    bool binary{};
};

struct aggregate_options {
    declaration_reference transition{};
    declaration_reference final{};
    aggregate_initial_condition initial_condition{};
    declaration_reference serial{};
    declaration_reference deserial{};
    std::int64_t state_space{};
    declaration_reference combine{};
    bool final_function_extra{};
    aggregate_modify final_function_modify{aggregate_modify::read_only};
    declaration_reference moving_transition{};
    declaration_reference moving_inverse{};
    declaration_reference moving_final{};
    std::int64_t moving_state_space{};
    aggregate_initial_condition moving_initial_condition{};
    bool moving_final_function_extra{};
    aggregate_modify moving_final_function_modify{
        aggregate_modify::read_only};
    declaration_reference sort_operator{};
    parallel_safety parallel{parallel_safety::unsafe};
};

struct opclass_options {
    declaration_reference access_method{};
    bool is_default{};
    pg_type storage{};
    declaration_reference family{};
    opclass_operator_list operators{};
    opclass_function_list functions{};
};

struct access_method_options {
    access_method_kind kind{access_method_kind::table};
};

struct type_role_options {
    pg_type for_type{};
};

struct catalog_dependencies_options {
    entity_reference_list roots{};
};

struct guc_options {
    guc_context context{guc_context::user};
    guc_flag flags{guc_flag::none};
    std::int64_t minimum{std::numeric_limits<std::int64_t>::min()};
    std::int64_t maximum{std::numeric_limits<std::int64_t>::max()};
};

struct primary_key_options {
    identifier name{"primary_key"};
    constraint_placement placement{constraint_placement::table_inline};
    std::array<identifier, 16> columns{};
};

struct unique_options {
    identifier name{"unique"};
    constraint_placement placement{constraint_placement::table_inline};
    std::array<identifier, 16> columns{};
};

struct index_options {
    identifier name{"index"};
    identifier access_method{"btree"};
    create_mode create{create_mode::create};
    bool unique{};
    checked_sql expression{};
    checked_sql predicate{};
};

struct check_options {
    identifier name{"check"};
    checked_sql expression{};
    constraint_placement placement{constraint_placement::table_inline};
};

struct foreign_key_options {
    identifier name{"foreign_key"};
    constraint_placement placement{constraint_placement::table_inline};
};

struct grant_options {
    bioseq::sql::privileges privileges{
        bioseq::sql::privileges::execute};
    identifier to{"public"};
    acl_scope scope{acl_scope::object};
    bool grant_option{};
};

struct revoke_options {
    bioseq::sql::privileges privileges{
        bioseq::sql::privileges::execute};
    identifier from{"public"};
    acl_scope scope{acl_scope::object};
    bool grant_option{};
};

struct guc_alias_options {
    identifier spelling;
    bool hidden{};
};

struct guc_alias_list {
    std::array<guc_alias_options, 1> entries{};
    std::size_t size{};
};

struct guc_value_options {
    bool omit{};
    guc_alias_list aliases{};
};

[[nodiscard]] consteval guc_alias_list guc_aliases(
    guc_alias_options alias) {
    return {.entries = {alias}, .size = 1};
}

}  // namespace bioseq::sql
