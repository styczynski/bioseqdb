/**
 * @file bioseq_caching/pg/object_dependency_hooks.cppm
 * @brief PostgreSQL object-access adapter for definition dependencies.
 *
 * PostgreSQL object callbacks identify local catalog mutations.  This module
 * translates the OID-addressable classes into precise object and syscache
 * dependency keys while retaining the per-database definition epoch as the
 * mandatory correctness backstop.  Unsupported or ambiguous addresses use
 * only that backstop; shared role mutations additionally select the global
 * emergency epoch so cached plans in every database are invalidated.
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "catalog/objectaccess.h"
#include "catalog/pg_aggregate_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_collation_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_policy_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/syscache.h"
}

#include <cstdint>

export module bioseq.cache.pg.object_dependency_hooks;

import bioseq.cache.core;
import bioseq.cache.pg.dependency_runtime;

export namespace bioseq::cache::pg::object_dependency_hooks {

namespace detail {

inline object_access_hook_type g_previous_object_access_hook = nullptr;
inline bool g_object_access_hook_installed = false;

[[nodiscard]] bool mutation_event(ObjectAccessType access) noexcept {
  switch (access) {
    case OAT_POST_CREATE:
    case OAT_DROP:
    case OAT_POST_ALTER:
      return true;
    case OAT_NAMESPACE_SEARCH:
    case OAT_FUNCTION_EXECUTE:
    case OAT_TRUNCATE:
      return false;
  }
  return false;
}

void force_database_definition_guard() noexcept {
  const dependency_runtime::dependency_runtime_result observed =
      dependency_runtime::observe_database_definition_dependency(
          static_cast<std::uint32_t>(MyDatabaseId));
  if (!observed) {
    dependency_runtime::force_emergency_fallback();
  }
}

[[nodiscard]] bool stage_object_dependency(
    Oid class_id, Oid object_id, dependency_kind kind) noexcept {
  const object_definition_mutation mutation =
      make_object_definition_mutation(dependency_key{
          .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
          .class_oid = static_cast<std::uint32_t>(class_id),
          .object_oid = static_cast<std::uint32_t>(object_id),
          .object_sub_id = 0,
          .kind = kind,
          .reserved = 0,
      });
  const dependency_runtime::dependency_runtime_result observed =
      dependency_runtime::observe_object_definition_dependency(mutation);
  if (!observed) {
    dependency_runtime::force_emergency_fallback();
    return false;
  }
  return true;
}

[[nodiscard]] bool stage_syscache_dependency(
    int cache_id, Oid object_id) noexcept {
  const std::uint32_t hash_value = GetSysCacheHashValue1(
      cache_id, ObjectIdGetDatum(object_id));
  const object_definition_mutation mutation =
      make_syscache_definition_mutation(
          static_cast<std::uint32_t>(MyDatabaseId),
          static_cast<std::uint32_t>(cache_id), hash_value);
  const dependency_runtime::dependency_runtime_result observed =
      dependency_runtime::observe_object_definition_dependency(mutation);
  if (!observed) {
    dependency_runtime::force_emergency_fallback();
    return false;
  }
  return true;
}

void stage_procedure_definition(Oid class_id, Oid object_id) noexcept {
  static_cast<void>(stage_object_dependency(
      class_id, object_id, dependency_kind::procedure));
  static_cast<void>(stage_syscache_dependency(PROCOID, object_id));

  // pg_aggregate.aggfnoid is the aggregate's pg_proc OID.  Staging this for
  // every procedure is conservative and makes aggregate creation/replacement
  // exact without catalog access from the hook.
  static_cast<void>(stage_syscache_dependency(AGGFNOID, object_id));
}

void stage_supported_definition(Oid class_id, Oid object_id) noexcept {
  switch (class_id) {
    case ProcedureRelationId:
    case AggregateRelationId:
      stage_procedure_definition(class_id, object_id);
      return;
    case OperatorRelationId:
      static_cast<void>(stage_object_dependency(
          class_id, object_id, dependency_kind::operator_object));
      static_cast<void>(stage_syscache_dependency(OPEROID, object_id));
      return;
    case TypeRelationId:
      static_cast<void>(stage_object_dependency(
          class_id, object_id, dependency_kind::type));
      static_cast<void>(stage_syscache_dependency(TYPEOID, object_id));
      return;
    case CollationRelationId:
      static_cast<void>(stage_object_dependency(
          class_id, object_id, dependency_kind::collation));
      static_cast<void>(stage_syscache_dependency(COLLOID, object_id));
      return;
    case AuthIdRelationId:
      static_cast<void>(stage_object_dependency(
          class_id, object_id, dependency_kind::role));
      static_cast<void>(stage_syscache_dependency(AUTHOID, object_id));
      // pg_authid is shared across databases.  A current-database definition
      // key cannot cover cached role-sensitive plans owned by other databases.
      dependency_runtime::force_emergency_fallback();
      return;
    case CastRelationId:
    case PolicyRelationId:
      // CASTSOURCETARGET needs two catalog columns and policies have no exact
      // syscache key.  A drop callback cannot safely reconstruct either key.
      force_database_definition_guard();
      return;
    default:
      force_database_definition_guard();
      return;
  }
}

void object_access(
    ObjectAccessType access, Oid class_id, Oid object_id, int sub_id,
    void *argument) {
  if (g_previous_object_access_hook != nullptr) {
    g_previous_object_access_hook(
        access, class_id, object_id, sub_id, argument);
  }
  if (!mutation_event(access)) {
    return;
  }
  if (!OidIsValid(MyDatabaseId) || !OidIsValid(class_id) ||
      !OidIsValid(object_id) || sub_id != 0) {
    force_database_definition_guard();
    return;
  }
  stage_supported_definition(class_id, object_id);
}

}  // namespace detail

/** Install the permanently chained object-definition mutation hook. */
void install_object_dependency_hooks() noexcept {
  if (detail::g_object_access_hook_installed) {
    return;
  }
  detail::g_previous_object_access_hook = object_access_hook;
  object_access_hook = detail::object_access;
  detail::g_object_access_hook_installed = true;
}

}  // namespace bioseq::cache::pg::object_dependency_hooks
