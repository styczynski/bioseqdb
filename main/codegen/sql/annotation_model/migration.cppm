module;

#include <cstdint>
#include <string_view>

export module bioseq.codegen.sql.annotation_model.migration;

export import bioseq.codegen.sql.annotation_model;

export namespace bioseq::sql {

struct migration_install_action final {
  checked_sql sql;
  documentation_text reason;
};

enum class migration_statement_kind : std::uint8_t {
  alter,
  comment,
  access_method,
  aggregate,
  cast,
  function,
  index,
  operator_,
  opclass,
  procedure,
  schema,
  sequence,
  session_setting,
  sql_function,
  table,
  type,
  view,
  grant,
  revoke,
  config_dump,
  catalog_dependencies,
};

enum class migration_routine_language : std::uint8_t {
  none,
  c,
  sql,
};

enum class migration_volatility : std::uint8_t {
  volatile_,
  stable,
  immutable,
};

enum class migration_null_input_policy : std::uint8_t {
  called,
  strict,
};

enum class migration_security_mode : std::uint8_t {
  invoker,
  definer,
};

enum class migration_parallel_safety : std::uint8_t {
  unsafe,
  restricted,
  safe,
};

struct migration_routine_metadata final {
  std::string_view identity{"not_a_routine"};
  std::string_view argument_declarations{"none"};
  std::string_view returns{"none"};
  migration_routine_language language{migration_routine_language::none};
  identifier c_symbol{"none"};
  migration_volatility volatility_value{migration_volatility::volatile_};
  migration_null_input_policy null_input{migration_null_input_policy::called};
  migration_security_mode security{migration_security_mode::invoker};
  migration_parallel_safety parallel{migration_parallel_safety::unsafe};
  std::string_view configuration{"none"};
  std::string_view cost{"none"};
  std::string_view rows{"none"};
  std::string_view support{"none"};
};

struct typed_install_statement final {
  checked_sql sql;
  migration_statement_kind kind{};
  std::string_view sql_identity;
  bool create_or_replace{};
  migration_routine_metadata routine{};
};

struct install_action_options {
  identifier baseline_physical_id{"P000"};
  std::string_view sql_identity{"unknown"};
  documentation_text migration_ledger_reason{"migration"};
};

struct typed_install_options {
  identifier baseline_physical_id{"P000"};
  std::string_view sql_identity{"unknown"};
  bool create_or_replace{};
  migration_routine_language language{migration_routine_language::none};
  identifier c_symbol{"none"};
  std::string_view argument_declarations{"none"};
  std::string_view returns{"none"};
  migration_volatility volatility_value{migration_volatility::volatile_};
  migration_null_input_policy null_input{migration_null_input_policy::called};
  migration_security_mode security{migration_security_mode::invoker};
  migration_parallel_safety parallel{migration_parallel_safety::unsafe};
  std::string_view configuration{"none"};
  std::string_view cost{"none"};
  std::string_view rows{"none"};
  std::string_view support{"none"};
};

[[nodiscard]] consteval annotation_spec<install_action_options>
install_action(install_action_options options) {
  return {.kind = annotation_kind::install_action, .options = options};
}

[[nodiscard]] consteval annotation_spec<install_action_options>
install_action() {
  return {.kind = annotation_kind::install_action, .options = {}};
}

#define BIOSEQ_SQL_MIGRATION_BUILDER(Name, Kind)                               \
  [[nodiscard]] consteval annotation_spec<typed_install_options> Name(         \
      typed_install_options options) {                                         \
    return {.kind = annotation_kind::Kind, .options = options};                \
  }

BIOSEQ_SQL_MIGRATION_BUILDER(alter, alter)
BIOSEQ_SQL_MIGRATION_BUILDER(comment, comment)
BIOSEQ_SQL_MIGRATION_BUILDER(function, function)
BIOSEQ_SQL_MIGRATION_BUILDER(procedure, procedure)
BIOSEQ_SQL_MIGRATION_BUILDER(session_setting, session_setting)
BIOSEQ_SQL_MIGRATION_BUILDER(schema, schema)
BIOSEQ_SQL_MIGRATION_BUILDER(sequence, sequence)
BIOSEQ_SQL_MIGRATION_BUILDER(table, table)
BIOSEQ_SQL_MIGRATION_BUILDER(view, view)
BIOSEQ_SQL_MIGRATION_BUILDER(sql_function, sql_function)
BIOSEQ_SQL_MIGRATION_BUILDER(operator_, operator_)
BIOSEQ_SQL_MIGRATION_BUILDER(type, type)
BIOSEQ_SQL_MIGRATION_BUILDER(cast, cast)
BIOSEQ_SQL_MIGRATION_BUILDER(aggregate, aggregate)
BIOSEQ_SQL_MIGRATION_BUILDER(opclass, opclass)
BIOSEQ_SQL_MIGRATION_BUILDER(access_method, access_method)
BIOSEQ_SQL_MIGRATION_BUILDER(index, index)
BIOSEQ_SQL_MIGRATION_BUILDER(grant, grant)
BIOSEQ_SQL_MIGRATION_BUILDER(revoke, revoke)
BIOSEQ_SQL_MIGRATION_BUILDER(config_dump, config_dump)
BIOSEQ_SQL_MIGRATION_BUILDER(am_catalog_dependencies, catalog_dependencies)

#undef BIOSEQ_SQL_MIGRATION_BUILDER

} // namespace bioseq::sql
